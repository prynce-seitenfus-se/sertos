/**
 * @file sertos_scheduler.c
 * @brief Preemptive priority-based real-time scheduler engine for SertOS.
 *
 * Employs hardware-accelerated CLZ bitmap searching for O(1) scheduling
 * and intrusive linked lists for round-robin time slicing.
 */

#include "sertos_scheduler.h"
#include "sertos_timer.h"
#include "sertos_port.h"
#include "sertos_stats.h"
#include "bitmap.h"
#include "linked_list.h"
#include "atomic.h"
#include <string.h>

/**
 * @brief Storage for the ready bitmap words.
 */
static BitmapWord s_ready_bitmap_storage[BITMAP_BITS_TO_WORDS(SERTOS_CONFIG_MAX_PRIORITIES)];

/**
 * @brief O(1) priority lookup bitmap instance.
 */
static Bitmap s_ready_bitmap;

/**
 * @brief Array of ready queues for each priority level.
 */
static LinkedList s_ready_queues[SERTOS_CONFIG_MAX_PRIORITIES];

/**
 * @brief Queue of tasks blocked on timer delays.
 */
static LinkedList s_delay_list;

/**
 * @brief Queue of tasks placed into explicit suspension.
 */
static LinkedList s_suspended_list;

/**
 * @brief Global registry linking every live task for whole-system enumeration.
 */
static LinkedList s_task_registry;

/**
 * @brief Currently executing task control block pointer.
 */
SertosTaskControlBlock* volatile sertos_current_tcb = NULL;

/**
 * @brief Scheduler execution state flag.
 */
static bool s_is_running = false;

/**
 * @brief Scheduler preemption lock nesting counter.
 */
static uint32_t s_lock_nesting = 0U;

/**
 * @brief Flag indicating a context switch is pending due to scheduler lock.
 */
static bool s_reschedule_pending = false;

/**
 * @brief Monotonic system tick counter.
 */
static atomic_size_t s_system_ticks;

/**
 * @brief Active tick rate in Hertz (zero-initialized in .bss, configured during scheduler init).
 */
static uint32_t s_tick_rate_hz = 0U;

/**
 * @brief Dynamic round-robin time-slicing enablement flag (zero-initialized in .bss).
 */
static bool s_time_slicing_enabled = false;

/**
 * @brief Optional user tick hook callback.
 */
static void (*s_tick_hook)(void) = NULL;

/**
 * @brief Optional user idle hook callback.
 */
static void (*s_idle_hook)(void) = NULL;

/**
 * @brief Static allocation storage for default system Idle Task.
 */
static SertosTaskControlBlock s_idle_tcb;
static uint8_t s_idle_stack[SERTOS_CONFIG_IDLE_TASK_STACK_SIZE] __attribute__((aligned(SERTOS_CONFIG_STACK_ALIGNMENT_BYTES)));

/**
 * @brief System Idle Task entry loop.
 *
 * @param param Unused context pointer.
 */
static __attribute__((no_instrument_function)) void idle_task_entry(void* param)
{
    (void)param;
    while (true) {
        if (s_idle_hook != NULL) {
            s_idle_hook();
        }
        /* Idle loop: enter low-power sleep on MCU or yield on host simulator */
#if defined(__arm__) || defined(__thumb__) || defined(__riscv)
        __asm__ volatile ("wfi");
#else
        sertos_port_yield();
#endif
    }
}

/**
 * @brief Updates tick counts of all tasks on the delay list and unblocks expired tasks.
 *
 * @return true if at least one task unblocked, false otherwise.
 */
static __attribute__((no_instrument_function)) bool process_delayed_tasks(void)
{
    LinkedListNode* curr_node;
    LinkedListNode* next_node;
    SertosTaskControlBlock* task;
    bool task_unblocked = false;

    curr_node = linked_list_peek_head(&s_delay_list);
    while (curr_node != NULL) {
        next_node = curr_node->next;
        if (next_node == &s_delay_list.root) {
            next_node = NULL;
        }

        task = LINKED_LIST_CONTAINER_OF(curr_node, SertosTaskControlBlock, state_node);
        if (task->delay_ticks > 0U) {
            task->delay_ticks--;
        }

        if (task->delay_ticks == 0U) {
            linked_list_remove_direct(&s_delay_list, curr_node);
            if (task->wait_list != NULL) {
                sertos_scheduler_wait_list_remove(task->wait_list, task);
                task->wait_list = NULL;
            }
            (void)sertos_scheduler_add_ready(task);
            task_unblocked = true;
        }

        curr_node = next_node;
    }

    return task_unblocked;
}

/**
 * @brief Performs round-robin rotation for tasks of the current priority.
 */
static __attribute__((no_instrument_function)) void process_time_slicing(void)
{
    LinkedList* current_queue;

    if (!s_time_slicing_enabled || (sertos_current_tcb == NULL)) {
        return;
    }

    current_queue = &s_ready_queues[sertos_current_tcb->priority];
    if (current_queue->count > 1U) {
        linked_list_remove_direct(current_queue, &sertos_current_tcb->state_node);
        linked_list_insert_tail_direct(current_queue, &sertos_current_tcb->state_node);
    }
}

SertosStatus sertos_scheduler_init_with_config(const SertosConfig* config)
{
    size_t i;
    SertosTaskConfig idle_cfg;
    SertosTaskHandle idle_handle;
    SertosStatus status;

    (void)memset(s_ready_bitmap_storage, 0, sizeof(s_ready_bitmap_storage));
    if (!bitmap_init(&s_ready_bitmap, s_ready_bitmap_storage, SERTOS_CONFIG_MAX_PRIORITIES)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    for (i = 0U; i < SERTOS_CONFIG_MAX_PRIORITIES; i++) {
        linked_list_init(&s_ready_queues[i]);
    }

    linked_list_init(&s_delay_list);
    linked_list_init(&s_suspended_list);
    linked_list_init(&s_task_registry);

    sertos_current_tcb = NULL;
    s_is_running = false;
    s_lock_nesting = 0U;
    s_reschedule_pending = false;
    atomic_init_size_t(&s_system_ticks, 0U);

    if (config != NULL) {
        s_tick_rate_hz = (config->tick_rate_hz > 0U) ? config->tick_rate_hz : SERTOS_CONFIG_TICK_RATE_HZ;
        s_time_slicing_enabled = config->enable_time_slicing;
        s_tick_hook = config->tick_hook;
        s_idle_hook = config->idle_hook;
        sertos_stats_set_enabled(config->enable_runtime_stats);

        if ((config->idle_task_stack != NULL) &&
            (config->idle_task_stack_size >= SERTOS_CONFIG_MINIMAL_STACK_SIZE)) {
            idle_cfg.stack_buffer = config->idle_task_stack;
            idle_cfg.stack_size = config->idle_task_stack_size;
        } else {
            idle_cfg.stack_buffer = s_idle_stack;
            idle_cfg.stack_size = sizeof(s_idle_stack);
        }
    } else {
        s_tick_rate_hz = SERTOS_CONFIG_TICK_RATE_HZ;
        s_time_slicing_enabled = (SERTOS_CONFIG_TIME_SLICING != 0U);
        s_tick_hook = NULL;
        s_idle_hook = NULL;
        sertos_stats_set_enabled(false);
        idle_cfg.stack_buffer = s_idle_stack;
        idle_cfg.stack_size = sizeof(s_idle_stack);
    }

    idle_cfg.name = "Idle";
    idle_cfg.entry_func = idle_task_entry;
    idle_cfg.param = NULL;
    idle_cfg.priority = 0U;

    status = sertos_task_create_static(&idle_cfg, &s_idle_tcb, &idle_handle);
    sertos_stats_reset();
    return status;
}

SertosStatus sertos_scheduler_init(void)
{
    return sertos_scheduler_init_with_config(NULL);
}

__attribute__((no_instrument_function)) SertosStatus sertos_scheduler_add_ready(SertosTaskControlBlock* tcb)
{
    if ((tcb == NULL) || (tcb->priority >= SERTOS_CONFIG_MAX_PRIORITIES)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    linked_list_insert_tail_direct(&s_ready_queues[tcb->priority], &tcb->state_node);
    (void)bitmap_set_bit(&s_ready_bitmap, tcb->priority);
    tcb->state = SERTOS_TASK_STATE_READY;

    return SERTOS_STATUS_OK;
}

__attribute__((no_instrument_function)) SertosStatus sertos_scheduler_remove_ready(SertosTaskControlBlock* tcb)
{
    if ((tcb == NULL) || (tcb->priority >= SERTOS_CONFIG_MAX_PRIORITIES)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    if ((tcb->state_node.prev != NULL) && (tcb->state_node.next != NULL)) {
        linked_list_remove_direct(&s_ready_queues[tcb->priority], &tcb->state_node);
        if (linked_list_is_empty(&s_ready_queues[tcb->priority])) {
            (void)bitmap_clear_bit(&s_ready_bitmap, tcb->priority);
        }
    }

    return SERTOS_STATUS_OK;
}

__attribute__((no_instrument_function)) SertosTaskControlBlock* sertos_scheduler_select_next_task(void)
{
    size_t highest_prio = 0U;
    LinkedListNode* head_node;

    if (!bitmap_find_last_set(&s_ready_bitmap, &highest_prio)) {
        return NULL;
    }

    head_node = linked_list_peek_head(&s_ready_queues[highest_prio]);
    if (head_node == NULL) {
        return NULL;
    }

    return LINKED_LIST_CONTAINER_OF(head_node, SertosTaskControlBlock, state_node);
}

/**
 * @brief Invokes a visitor for each task linked into a list via its registry_node.
 *
 * @param list  Pointer to the intrusive list to walk.
 * @param visit Visitor callback.
 * @param ctx   Opaque context forwarded to the visitor.
 */
static __attribute__((no_instrument_function)) void visit_list_state_nodes(LinkedList* list,
                                                                           SertosTaskVisitor visit,
                                                                           void* ctx)
{
    LinkedListNode* node;
    SertosTaskControlBlock* task;

    node = list->root.next;
    while (node != &list->root) {
        task = LINKED_LIST_CONTAINER_OF(node, SertosTaskControlBlock, registry_node);
        visit(task, ctx);
        node = node->next;
    }
}

__attribute__((no_instrument_function)) void sertos_scheduler_visit_all_tasks(SertosTaskVisitor visit, void* ctx)
{
    uint32_t crit_status;

    if (visit == NULL) {
        return;
    }

    crit_status = sertos_port_enter_critical();
    visit_list_state_nodes(&s_task_registry, visit, ctx);
    sertos_port_exit_critical(crit_status);
}

__attribute__((no_instrument_function)) void sertos_scheduler_register_task(SertosTaskControlBlock* tcb)
{
    if (tcb == NULL) {
        return;
    }
    linked_list_insert_tail_direct(&s_task_registry, &tcb->registry_node);
}

void sertos_scheduler_unregister_task(SertosTaskControlBlock* tcb)
{
    if (tcb == NULL) {
        return;
    }
    if (tcb->registry_node.next != NULL) {
        linked_list_remove_direct(&s_task_registry, &tcb->registry_node);
        tcb->registry_node.next = NULL;
        tcb->registry_node.prev = NULL;
    }
}

SertosTaskControlBlock* sertos_scheduler_get_idle_tcb(void)
{
    return &s_idle_tcb;
}

__attribute__((no_instrument_function)) SertosTaskControlBlock* sertos_scheduler_perform_switch(void)
{
    SertosTaskControlBlock* next_task;
    SertosTaskControlBlock* prev_task;

    prev_task = sertos_current_tcb;
    next_task = sertos_scheduler_select_next_task();
    if (next_task != NULL) {
        if ((prev_task != NULL) && (prev_task->state == SERTOS_TASK_STATE_RUNNING)) {
            prev_task->state = SERTOS_TASK_STATE_READY;
        }
        sertos_current_tcb = next_task;
        sertos_current_tcb->state = SERTOS_TASK_STATE_RUNNING;
        sertos_stats_on_switch(prev_task, next_task);
    }

    return sertos_current_tcb;
}

__attribute__((no_instrument_function)) void sertos_scheduler_switch_context(void)
{
    SertosTaskControlBlock* next_task;
    uint32_t crit;

    if (s_lock_nesting > 0U) {
        s_reschedule_pending = true;
        return;
    }

    crit = sertos_port_enter_critical();
    next_task = sertos_scheduler_select_next_task();
    if (next_task == NULL) {
        sertos_port_exit_critical(crit);
        return;
    }

    if (next_task != sertos_current_tcb) {
#if defined(SERTOS_PORT_HOST) || defined(_WIN32) || defined(__linux__)
        (void)sertos_scheduler_perform_switch();
#endif
        sertos_port_exit_critical(crit);
        sertos_port_yield();
    } else {
        sertos_port_exit_critical(crit);
    }
}

__attribute__((no_instrument_function)) void sertos_scheduler_reschedule(void)
{
    SertosTaskControlBlock* next_task;
    uint32_t crit;

    if (!s_is_running) {
        return;
    }

    crit = sertos_port_enter_critical();
    next_task = sertos_scheduler_select_next_task();
    if ((next_task != NULL) && ((sertos_current_tcb == NULL) || (next_task->priority > sertos_current_tcb->priority))) {
        sertos_port_exit_critical(crit);
        sertos_scheduler_switch_context();
    } else {
        sertos_port_exit_critical(crit);
    }
}

void sertos_scheduler_start(void)
{
    s_is_running = true;
    sertos_port_runtime_counter_init();
    if (sertos_port_tick_init(s_tick_rate_hz) != SERTOS_STATUS_OK) {
        s_is_running = false;
        return;
    }

    (void)sertos_scheduler_perform_switch();

    sertos_port_start_first_task();
}

void sertos_scheduler_stop(void)
{
    s_is_running = false;
    sertos_port_stop_scheduler();
}

__attribute__((no_instrument_function)) void sertos_scheduler_tick(void)
{
    uint32_t crit_status;
    bool need_reschedule;

    crit_status = sertos_port_enter_critical();
    atomic_store_relaxed(&s_system_ticks, atomic_load_relaxed(&s_system_ticks) + 1U);

    need_reschedule = process_delayed_tasks();
    process_time_slicing();
    sertos_timer_tick();

    if (s_tick_hook != NULL) {
        s_tick_hook();
    }

    sertos_port_exit_critical(crit_status);

    if (need_reschedule || s_time_slicing_enabled) {
        sertos_scheduler_switch_context();
    }
}

void sertos_scheduler_yield(void)
{
    uint32_t crit_status;

    crit_status = sertos_port_enter_critical();
    process_time_slicing();
    sertos_port_exit_critical(crit_status);

    sertos_scheduler_switch_context();
}

void sertos_scheduler_lock(void)
{
    uint32_t crit_status;

    crit_status = sertos_port_enter_critical();
    s_lock_nesting++;
    sertos_port_exit_critical(crit_status);
}

void sertos_scheduler_unlock(void)
{
    uint32_t crit_status;
    bool switch_needed = false;

    crit_status = sertos_port_enter_critical();
    if (s_lock_nesting > 0U) {
        s_lock_nesting--;
        if ((s_lock_nesting == 0U) && s_reschedule_pending) {
            s_reschedule_pending = false;
            switch_needed = true;
        }
    }
    sertos_port_exit_critical(crit_status);

    if (switch_needed) {
        sertos_scheduler_switch_context();
    }
}

bool sertos_scheduler_is_locked(void)
{
    return (s_lock_nesting > 0U);
}

__attribute__((no_instrument_function)) bool sertos_scheduler_is_running(void)
{
    return s_is_running;
}

SertosTick sertos_scheduler_get_tick_count(void)
{
    return (SertosTick)atomic_load_acquire(&s_system_ticks);
}

__attribute__((no_instrument_function)) uint32_t sertos_scheduler_get_tick_rate_hz(void)
{
    return (s_tick_rate_hz > 0U) ? s_tick_rate_hz : SERTOS_CONFIG_TICK_RATE_HZ;
}

bool sertos_scheduler_is_time_slicing_enabled(void)
{
    return s_time_slicing_enabled;
}

SertosStatus sertos_scheduler_delay(SertosTick ticks)
{
    uint32_t crit_status;

    if (ticks == 0U) {
        sertos_scheduler_yield();
        return SERTOS_STATUS_OK;
    }

    if (!s_is_running || (sertos_current_tcb == NULL)) {
        return SERTOS_STATUS_ERROR_NOT_INITIALIZED;
    }

    crit_status = sertos_port_enter_critical();
    (void)sertos_scheduler_remove_ready(sertos_current_tcb);
    sertos_current_tcb->state = SERTOS_TASK_STATE_BLOCKED;
    sertos_current_tcb->delay_ticks = ticks;
    linked_list_insert_tail_direct(&s_delay_list, &sertos_current_tcb->state_node);
    sertos_port_exit_critical(crit_status);

    sertos_scheduler_switch_context();
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_scheduler_delay_ms(uint32_t ms)
{
    if (ms == 0U) {
        sertos_scheduler_yield();
        return SERTOS_STATUS_OK;
    }

    return sertos_scheduler_delay(SERTOS_MS_TO_TICKS(ms));
}

SertosStatus sertos_scheduler_delay_until(SertosTick* last_wake_time, SertosTick period)
{
    SertosTick elapsed;
    SertosStatus status;

    if (last_wake_time == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }

    if (period == 0U) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    elapsed = sertos_scheduler_get_tick_count() - *last_wake_time;
    *last_wake_time += period;

    if (elapsed < period) {
        status = sertos_scheduler_delay(period - elapsed);
    } else {
        sertos_scheduler_yield();
        status = SERTOS_STATUS_OK;
    }

    return status;
}

SertosTaskControlBlock* sertos_scheduler_get_current_tcb(void)
{
    return sertos_current_tcb;
}

void sertos_scheduler_set_current_tcb(SertosTaskControlBlock* tcb)
{
    sertos_current_tcb = tcb;
}

static __attribute__((no_instrument_function)) void insert_wait_list_priority(LinkedList* wait_list,
                                                                              SertosTaskControlBlock* tcb)
{
    LinkedListNode* curr;
    SertosTaskControlBlock* iter_task;

    curr = linked_list_peek_head(wait_list);
    while (curr != NULL) {
        iter_task = LINKED_LIST_CONTAINER_OF(curr, SertosTaskControlBlock, event_node);
        if (tcb->priority > iter_task->priority) {
            tcb->event_node.next = curr;
            tcb->event_node.prev = curr->prev;
            curr->prev->next = &tcb->event_node;
            curr->prev = &tcb->event_node;
            wait_list->count++;
            return;
        }
        curr = curr->next;
        if (curr == &wait_list->root) {
            curr = NULL;
        }
    }

    linked_list_insert_tail_direct(wait_list, &tcb->event_node);
}

__attribute__((no_instrument_function)) void sertos_scheduler_wait_list_remove(LinkedList* wait_list,
                                                                               SertosTaskControlBlock* tcb)
{
    if ((wait_list == NULL) || (tcb == NULL) || (tcb->event_node.next == NULL)) {
        return;
    }
    linked_list_remove_direct(wait_list, &tcb->event_node);
    tcb->event_node.next = NULL;
    tcb->event_node.prev = NULL;
}

__attribute__((no_instrument_function)) SertosTaskControlBlock*
sertos_scheduler_wait_list_unblock_highest(LinkedList* wait_list)
{
    LinkedListNode* head;
    SertosTaskControlBlock* task;

    if (wait_list == NULL) {
        return NULL;
    }

    head = linked_list_peek_head(wait_list);
    if (head == NULL) {
        return NULL;
    }

    task = LINKED_LIST_CONTAINER_OF(head, SertosTaskControlBlock, event_node);
    sertos_scheduler_wait_list_remove(wait_list, task);

    if (task->delay_ticks > 0U) {
        linked_list_remove_direct(&s_delay_list, &task->state_node);
        task->delay_ticks = 0U;
    }

    task->wait_list = NULL;
    (void)sertos_scheduler_add_ready(task);
    return task;
}

__attribute__((no_instrument_function)) SertosStatus
sertos_scheduler_wait_list_block(LinkedList* wait_list, SertosTick timeout)
{
    uint32_t crit_status;
    SertosTaskControlBlock* current;

    if (wait_list == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if (!s_is_running || (sertos_current_tcb == NULL)) {
        return SERTOS_STATUS_ERROR_NOT_INITIALIZED;
    }

    current = sertos_current_tcb;
    crit_status = sertos_port_enter_critical();
    (void)sertos_scheduler_remove_ready(current);
    current->state = SERTOS_TASK_STATE_BLOCKED;
    current->wait_list = wait_list;
    insert_wait_list_priority(wait_list, current);

    if (timeout != SERTOS_WAIT_FOREVER) {
        current->delay_ticks = timeout;
        linked_list_insert_tail_direct(&s_delay_list, &current->state_node);
    } else {
        current->delay_ticks = 0U;
        current->state_node.next = NULL;
        current->state_node.prev = NULL;
    }
    sertos_port_exit_critical(crit_status);

    sertos_scheduler_switch_context();

    if ((timeout != SERTOS_WAIT_FOREVER) && (current->delay_ticks == 0U) && (current->wait_list == NULL)) {
        return SERTOS_STATUS_ERROR_TIMEOUT;
    }

    return SERTOS_STATUS_OK;
}
