/**
 * @file sertos_task.c
 * @brief Task lifecycle management, creation, stack painting, and inspection.
 *
 * Implements deterministic static and dynamic task instantiation conforming
 * to MISRA C:2012 and ISO C99.
 */

#include "sertos_task.h"
#include "sertos_scheduler.h"
#include "sertos_port.h"
#include "memory_pool.h"
#include "crc.h"
#include "fsm.h"
#include <string.h>

/**
 * @brief Validates common task configuration parameters.
 *
 * @param config Pointer to task configuration structure.
 * @return SERTOS_STATUS_OK if valid, or error status code.
 */
static __attribute__((no_instrument_function)) SertosStatus validate_task_config(const SertosTaskConfig* config)
{
    uintptr_t buffer_addr;

    if (config == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if (config->entry_func == NULL) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }
    if (config->priority >= SERTOS_CONFIG_MAX_PRIORITIES) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }
    if (config->stack_size < SERTOS_CONFIG_MINIMAL_STACK_SIZE) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }
    if (config->stack_buffer != NULL) {
        buffer_addr = (uintptr_t)config->stack_buffer;
        if ((buffer_addr % (uintptr_t)SERTOS_STACK_ALIGNMENT_BYTES) != 0U) {
            return SERTOS_STATUS_ERROR_INVALID_PARAM;
        }
    }

    return SERTOS_STATUS_OK;
}

/**
 * @brief Initializes TCB fields and stack memory.
 *
 * @param tcb Pointer to TCB to initialize.
 * @param config Pointer to validated configuration.
 * @param is_static True if statically allocated.
 */
static __attribute__((no_instrument_function)) void init_tcb_and_stack(SertosTaskControlBlock* tcb,
                                                                        const SertosTaskConfig* config,
                                                                        bool is_static)
{
    uintptr_t stack_base_addr;
    uintptr_t stack_top_addr;
    void* stack_top;

    stack_base_addr = (uintptr_t)config->stack_buffer;
    stack_top_addr = stack_base_addr + config->stack_size;
    stack_top_addr &= ~((uintptr_t)SERTOS_STACK_ALIGNMENT_BYTES - 1U);
    stack_top = (void*)stack_top_addr;

    /* Paint stack with canary byte pattern for high-water mark tracking */
    (void)memset(config->stack_buffer, (int)SERTOS_TASK_STACK_FILL_BYTE, config->stack_size);

    tcb->stack_base = config->stack_buffer;
    tcb->stack_size = config->stack_size;
    tcb->stack_limit = config->stack_buffer;
    tcb->stack_ptr = sertos_port_stack_init(stack_top, tcb->stack_limit, config->entry_func, config->param);

    tcb->state = SERTOS_TASK_STATE_READY;
    tcb->priority = config->priority;
    tcb->base_priority = config->priority;
    tcb->delay_ticks = 0U;
    tcb->entry_func = config->entry_func;
    tcb->param = config->param;
    tcb->name = (config->name != NULL) ? config->name : "Task";
    tcb->state_node.next = NULL;
    tcb->state_node.prev = NULL;
    tcb->event_node.next = NULL;
    tcb->event_node.prev = NULL;
    tcb->wait_list = NULL;
    tcb->is_statically_allocated = is_static;
    tcb->magic = SERTOS_TASK_MAGIC_WORD;
    tcb->port_context = NULL;
    tcb->run_time_total = 0U;
    tcb->run_time_last_entry = 0U;
    tcb->switch_in_count = 0U;
    tcb->registry_node.next = NULL;
    tcb->registry_node.prev = NULL;

    sertos_port_task_create_hook(tcb);
}

SertosStatus sertos_task_create_static(const SertosTaskConfig* config,
                                       SertosTaskControlBlock* tcb,
                                       SertosTaskHandle* out_handle)
{
    SertosStatus status;
    uint32_t crit_status;

    if ((tcb == NULL) || (out_handle == NULL)) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if ((config == NULL) || (config->stack_buffer == NULL)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    status = validate_task_config(config);
    if (status != SERTOS_STATUS_OK) {
        return status;
    }

    init_tcb_and_stack(tcb, config, true);

    crit_status = sertos_port_enter_critical();
    status = sertos_scheduler_add_ready(tcb);
    if (status == SERTOS_STATUS_OK) {
        sertos_scheduler_register_task(tcb);
    }
    sertos_port_exit_critical(crit_status);

    if (status == SERTOS_STATUS_OK) {
        *out_handle = tcb;
        if (sertos_scheduler_is_running()) {
            sertos_scheduler_reschedule();
        }
    }

    return status;
}

SertosStatus sertos_task_create(const SertosTaskConfig* config, SertosTaskHandle* out_handle)
{
    SertosStatus status;
    SertosTaskControlBlock* tcb;
    void* stack_buf;
    SertosTaskConfig dynamic_cfg;
    uint32_t crit_status;

    if (out_handle == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }

    status = validate_task_config(config);
    if (status != SERTOS_STATUS_OK) {
        return status;
    }

    dynamic_cfg = *config;
    tcb = (SertosTaskControlBlock*)memory_pool_malloc(sizeof(SertosTaskControlBlock));
    if (tcb == NULL) {
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    stack_buf = memory_pool_malloc(dynamic_cfg.stack_size);
    if (stack_buf == NULL) {
        memory_pool_free(tcb);
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    dynamic_cfg.stack_buffer = stack_buf;
    init_tcb_and_stack(tcb, &dynamic_cfg, false);

    status = sertos_scheduler_add_ready(tcb);
    if (status != SERTOS_STATUS_OK) {
        memory_pool_free(stack_buf);
        memory_pool_free(tcb);
        return status;
    }

    crit_status = sertos_port_enter_critical();
    sertos_scheduler_register_task(tcb);
    sertos_port_exit_critical(crit_status);

    *out_handle = tcb;
    if (sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

static const FsmTransition s_task_transitions[] = {
    { (FsmStateId)SERTOS_TASK_STATE_READY,     1U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_RUNNING },
    { (FsmStateId)SERTOS_TASK_STATE_RUNNING,   2U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_READY },
    { (FsmStateId)SERTOS_TASK_STATE_RUNNING,   3U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_BLOCKED },
    { (FsmStateId)SERTOS_TASK_STATE_BLOCKED,   4U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_READY },
    { (FsmStateId)SERTOS_TASK_STATE_READY,     5U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_SUSPENDED },
    { (FsmStateId)SERTOS_TASK_STATE_RUNNING,   5U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_SUSPENDED },
    { (FsmStateId)SERTOS_TASK_STATE_BLOCKED,   5U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_SUSPENDED },
    { (FsmStateId)SERTOS_TASK_STATE_SUSPENDED, 6U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_READY },
    { (FsmStateId)SERTOS_TASK_STATE_READY,     7U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_TERMINATED },
    { (FsmStateId)SERTOS_TASK_STATE_RUNNING,   7U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_TERMINATED },
    { (FsmStateId)SERTOS_TASK_STATE_BLOCKED,   7U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_TERMINATED },
    { (FsmStateId)SERTOS_TASK_STATE_SUSPENDED, 7U, NULL, NULL, (FsmStateId)SERTOS_TASK_STATE_TERMINATED }
};

bool sertos_task_is_valid_transition(SertosTaskState from, SertosTaskState to)
{
    size_t i;
    size_t count;

    if (from == to) {
        return false;
    }

    count = sizeof(s_task_transitions) / sizeof(s_task_transitions[0]);
    for (i = 0U; i < count; i++) {
        if ((s_task_transitions[i].current_state == (FsmStateId)from) &&
            (s_task_transitions[i].next_state == (FsmStateId)to)) {
            return true;
        }
    }

    return false;
}

SertosStatus sertos_task_delete(SertosTaskHandle handle)
{
    SertosTaskControlBlock* tcb;
    uint32_t crit_status;
    bool is_current;

    tcb = (handle == NULL) ? sertos_scheduler_get_current_tcb() : handle;
    if ((tcb == NULL) || (tcb->magic != SERTOS_TASK_MAGIC_WORD) ||
        !sertos_task_is_valid_transition(tcb->state, SERTOS_TASK_STATE_TERMINATED)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    is_current = (tcb == sertos_scheduler_get_current_tcb());

    if (tcb->state == SERTOS_TASK_STATE_READY) {
        (void)sertos_scheduler_remove_ready(tcb);
    }

    sertos_scheduler_unregister_task(tcb);
    tcb->state = SERTOS_TASK_STATE_TERMINATED;
    sertos_port_task_delete_hook(tcb);

    if (!tcb->is_statically_allocated) {
        memory_pool_free(tcb->stack_base);
        memory_pool_free(tcb);
    }
    sertos_port_exit_critical(crit_status);

    if (is_current && sertos_scheduler_is_running()) {
        sertos_scheduler_yield();
    }

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_task_suspend(SertosTaskHandle handle)
{
    SertosTaskControlBlock* tcb;
    uint32_t crit_status;
    bool is_current;

    tcb = (handle == NULL) ? sertos_scheduler_get_current_tcb() : handle;
    if ((tcb == NULL) || (tcb->magic != SERTOS_TASK_MAGIC_WORD) ||
        !sertos_task_is_valid_transition(tcb->state, SERTOS_TASK_STATE_SUSPENDED)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    is_current = (tcb == sertos_scheduler_get_current_tcb());

    if (tcb->state == SERTOS_TASK_STATE_READY) {
        (void)sertos_scheduler_remove_ready(tcb);
    }
    tcb->state = SERTOS_TASK_STATE_SUSPENDED;
    sertos_port_exit_critical(crit_status);

    if (is_current && sertos_scheduler_is_running()) {
        sertos_scheduler_yield();
    }

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_task_resume(SertosTaskHandle handle)
{
    SertosTaskControlBlock* tcb;
    uint32_t crit_status;
    SertosStatus status;

    tcb = handle;
    if ((tcb == NULL) || (tcb->magic != SERTOS_TASK_MAGIC_WORD) ||
        !sertos_task_is_valid_transition(tcb->state, SERTOS_TASK_STATE_READY)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    status = sertos_scheduler_add_ready(tcb);
    sertos_port_exit_critical(crit_status);

    if ((status == SERTOS_STATUS_OK) && sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return status;
}

SertosTaskState sertos_task_get_state(SertosTaskHandle handle)
{
    if ((handle == NULL) || (handle->magic != SERTOS_TASK_MAGIC_WORD)) {
        return SERTOS_TASK_STATE_TERMINATED;
    }
    return handle->state;
}

SertosTaskHandle sertos_task_get_current(void)
{
    return sertos_scheduler_get_current_tcb();
}

const char* sertos_task_get_name(SertosTaskHandle handle)
{
    if ((handle == NULL) || (handle->magic != SERTOS_TASK_MAGIC_WORD)) {
        return "Unknown";
    }
    return (handle->name != NULL) ? handle->name : "Unknown";
}

SertosPriority sertos_task_get_priority(SertosTaskHandle handle)
{
    if ((handle == NULL) || (handle->magic != SERTOS_TASK_MAGIC_WORD)) {
        return 0U;
    }
    return handle->priority;
}

SertosStatus sertos_task_set_priority(SertosTaskHandle handle, SertosPriority new_priority)
{
    SertosTaskControlBlock* tcb;
    uint32_t crit_status;
    bool was_ready;

    if (new_priority >= SERTOS_CONFIG_MAX_PRIORITIES) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    tcb = (handle == NULL) ? sertos_scheduler_get_current_tcb() : handle;
    if ((tcb == NULL) || (tcb->magic != SERTOS_TASK_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    was_ready = (tcb->state == SERTOS_TASK_STATE_READY);

    if (was_ready) {
        (void)sertos_scheduler_remove_ready(tcb);
    }

    tcb->priority = new_priority;
    tcb->base_priority = new_priority;

    if (was_ready) {
        (void)sertos_scheduler_add_ready(tcb);
    }
    sertos_port_exit_critical(crit_status);

    if (sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

size_t sertos_task_get_stack_high_water_mark(SertosTaskHandle handle)
{
    const uint8_t* byte_ptr;
    size_t free_bytes = 0U;

    if ((handle == NULL) || (handle->magic != SERTOS_TASK_MAGIC_WORD)) {
        return 0U;
    }

    byte_ptr = (const uint8_t*)handle->stack_base;
    while ((free_bytes < handle->stack_size) && (*byte_ptr == SERTOS_TASK_STACK_FILL_BYTE)) {
        free_bytes++;
        byte_ptr++;
    }

    return free_bytes;
}

uint32_t sertos_task_compute_crc(const SertosTaskControlBlock* tcb)
{
    if (tcb == NULL) {
        return 0U;
    }

    return crc_32_calculate((const uint8_t*)tcb, offsetof(SertosTaskControlBlock, magic));
}
