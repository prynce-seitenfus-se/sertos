/**
 * @file sertos_timer.c
 * @brief Monotonic software timers primitive implementation for SertOS.
 *
 * Implements one-shot and periodic software timers driven by scheduler ticks.
 */

#include "sertos_timer.h"
#include "sertos_port.h"
#include "memory_pool.h"

/**
 * @brief Circular intrusive queue of currently active timers.
 */
static LinkedList s_active_timers;

/**
 * @brief Tracks whether the timer subsystem list container has been initialized.
 */
static bool s_timer_subsystem_initialized = false;

void sertos_timer_init(void)
{
    (void)linked_list_init(&s_active_timers);
    s_timer_subsystem_initialized = true;
}

SertosStatus sertos_timer_create_static(const SertosTimerConfig* config,
                                        SertosTimer* timer,
                                        SertosTimerHandle* out_handle)
{
    if ((config == NULL) || (timer == NULL) || (out_handle == NULL)) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if ((config->period == 0U) || (config->callback == NULL)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    if (!s_timer_subsystem_initialized) {
        sertos_timer_init();
    }

    timer->name = (config->name != NULL) ? config->name : "Timer";
    timer->period_ticks = config->period;
    timer->remaining_ticks = config->period;
    timer->is_periodic = config->is_periodic;
    timer->is_active = false;
    timer->callback = config->callback;
    timer->param = config->param;
    timer->node.next = NULL;
    timer->node.prev = NULL;
    timer->is_statically_allocated = true;
    timer->magic = SERTOS_TIMER_MAGIC_WORD;

    *out_handle = timer;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_timer_create(const SertosTimerConfig* config,
                                 SertosTimerHandle* out_handle)
{
    SertosTimer* timer;
    SertosStatus status;

    if (out_handle == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }

    timer = (SertosTimer*)memory_pool_malloc(sizeof(SertosTimer));
    if (timer == NULL) {
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    status = sertos_timer_create_static(config, timer, out_handle);
    if (status != SERTOS_STATUS_OK) {
        memory_pool_free(timer);
        return status;
    }

    timer->is_statically_allocated = false;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_timer_delete(SertosTimerHandle handle)
{
    uint32_t crit_status;

    if ((handle == NULL) || (handle->magic != SERTOS_TIMER_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    if (handle->is_active) {
        linked_list_remove_direct(&s_active_timers, &handle->node);
        handle->is_active = false;
    }
    handle->magic = 0U;

    if (!handle->is_statically_allocated) {
        memory_pool_free(handle);
    }
    sertos_port_exit_critical(crit_status);

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_timer_start(SertosTimerHandle handle)
{
    uint32_t crit_status;

    if ((handle == NULL) || (handle->magic != SERTOS_TIMER_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    if (handle->is_active) {
        linked_list_remove_direct(&s_active_timers, &handle->node);
    }

    handle->remaining_ticks = handle->period_ticks;
    handle->is_active = true;
    linked_list_insert_tail_direct(&s_active_timers, &handle->node);
    sertos_port_exit_critical(crit_status);

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_timer_stop(SertosTimerHandle handle)
{
    uint32_t crit_status;

    if ((handle == NULL) || (handle->magic != SERTOS_TIMER_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    if (!handle->is_active) {
        sertos_port_exit_critical(crit_status);
        return SERTOS_STATUS_OK;
    }

    linked_list_remove_direct(&s_active_timers, &handle->node);
    handle->node.next = NULL;
    handle->node.prev = NULL;
    handle->is_active = false;
    sertos_port_exit_critical(crit_status);

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_timer_reset(SertosTimerHandle handle)
{
    return sertos_timer_start(handle);
}

SertosStatus sertos_timer_change_period(SertosTimerHandle handle, SertosTick new_period)
{
    if ((handle == NULL) || (handle->magic != SERTOS_TIMER_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }
    if (new_period == 0U) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    handle->period_ticks = new_period;
    return sertos_timer_reset(handle);
}

bool sertos_timer_is_active(SertosTimerHandle handle)
{
    if ((handle == NULL) || (handle->magic != SERTOS_TIMER_MAGIC_WORD)) {
        return false;
    }
    return handle->is_active;
}

__attribute__((no_instrument_function)) void sertos_timer_tick(void)
{
    LinkedListNode* curr;
    LinkedListNode* next;
    SertosTimer* timer;

    if (!s_timer_subsystem_initialized) {
        return;
    }

    curr = linked_list_peek_head(&s_active_timers);
    while (curr != NULL) {
        next = curr->next;
        if (next == &s_active_timers.root) {
            next = NULL;
        }

        timer = LINKED_LIST_CONTAINER_OF(curr, SertosTimer, node);
        if (timer->remaining_ticks > 0U) {
            timer->remaining_ticks--;
        }

        if (timer->remaining_ticks == 0U) {
            if (timer->is_periodic) {
                timer->remaining_ticks = timer->period_ticks;
            } else {
                linked_list_remove_direct(&s_active_timers, curr);
                timer->node.next = NULL;
                timer->node.prev = NULL;
                timer->is_active = false;
            }

            if (timer->callback != NULL) {
                timer->callback(timer, timer->param);
            }
        }

        curr = next;
    }
}
