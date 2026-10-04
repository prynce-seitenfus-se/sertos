/**
 * @file sertos_mutex.c
 * @brief Mutual exclusion synchronization primitive with Priority Inheritance Protocol (PIP).
 *
 * Implements deterministic PIP to eliminate unbounded priority inversion.
 */

#include "sertos_mutex.h"
#include "sertos_scheduler.h"
#include "sertos_port.h"
#include "memory_pool.h"

/**
 * @brief Boosts the active priority of the current mutex owner to avoid priority inversion.
 *
 * @param mutex Pointer to the mutex instance.
 * @param current Pointer to the higher-priority task requesting the mutex.
 */
static __attribute__((no_instrument_function)) void apply_priority_inheritance(SertosMutex* mutex,
                                                                               SertosTaskControlBlock* current)
{
    bool was_ready;

    if (current->priority > mutex->owner->priority) {
        was_ready = (mutex->owner->state == SERTOS_TASK_STATE_READY);
        if (was_ready) {
            (void)sertos_scheduler_remove_ready(mutex->owner);
        }
        mutex->owner->priority = current->priority;
        if (was_ready) {
            (void)sertos_scheduler_add_ready(mutex->owner);
        }
    }
}

/**
 * @brief Restores the owner task's priority back to its original base priority upon unlocking.
 *
 * @param mutex Pointer to the mutex instance.
 * @param current Pointer to the owner task releasing the lock.
 */
static __attribute__((no_instrument_function)) void restore_owner_priority(SertosMutex* mutex,
                                                                            SertosTaskControlBlock* current)
{
    bool was_ready;

    if (current->priority != mutex->original_owner_priority) {
        was_ready = (current->state == SERTOS_TASK_STATE_READY);
        if (was_ready) {
            (void)sertos_scheduler_remove_ready(current);
        }
        current->priority = mutex->original_owner_priority;
        if (was_ready) {
            (void)sertos_scheduler_add_ready(current);
        }
    }
}

SertosStatus sertos_mutex_create_static(SertosMutex* mutex, SertosMutexHandle* out_handle)
{
    if ((mutex == NULL) || (out_handle == NULL)) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }

    mutex->owner = NULL;
    mutex->lock_count = 0U;
    mutex->original_owner_priority = 0U;
    (void)linked_list_init(&mutex->wait_list);
    mutex->is_statically_allocated = true;
    mutex->magic = SERTOS_MUTEX_MAGIC_WORD;

    *out_handle = mutex;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_mutex_create(SertosMutexHandle* out_handle)
{
    SertosMutex* mutex;
    SertosStatus status;

    if (out_handle == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }

    mutex = (SertosMutex*)memory_pool_malloc(sizeof(SertosMutex));
    if (mutex == NULL) {
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    status = sertos_mutex_create_static(mutex, out_handle);
    if (status != SERTOS_STATUS_OK) {
        memory_pool_free(mutex);
        return status;
    }

    mutex->is_statically_allocated = false;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_mutex_delete(SertosMutexHandle handle)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;

    if ((handle == NULL) || (handle->magic != SERTOS_MUTEX_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    if (handle->owner != NULL) {
        restore_owner_priority(handle, handle->owner);
    }
    handle->magic = 0U;

    do {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_list);
    } while (unblocked != NULL);

    if (!handle->is_statically_allocated) {
        memory_pool_free(handle);
    }
    sertos_port_exit_critical(crit_status);

    if (sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_mutex_lock(SertosMutexHandle handle, SertosTick timeout)
{
    uint32_t crit_status;
    SertosTaskControlBlock* current;
    SertosStatus status;

    if ((handle == NULL) || (handle->magic != SERTOS_MUTEX_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    current = sertos_scheduler_get_current_tcb();
    if (current == NULL) {
        return SERTOS_STATUS_ERROR_NOT_INITIALIZED;
    }

    crit_status = sertos_port_enter_critical();

    /* Unlocked: acquire immediately */
    if (handle->owner == NULL) {
        handle->owner = current;
        handle->lock_count = 1U;
        handle->original_owner_priority = current->priority;
        sertos_port_exit_critical(crit_status);
        return SERTOS_STATUS_OK;
    }

    /* Recursive acquisition by current owner */
    if (handle->owner == current) {
        handle->lock_count++;
        sertos_port_exit_critical(crit_status);
        return SERTOS_STATUS_OK;
    }

    /* Lock held by another task: apply Priority Inheritance */
    apply_priority_inheritance(handle, current);

    if (timeout == SERTOS_NO_WAIT) {
        sertos_port_exit_critical(crit_status);
        return SERTOS_STATUS_ERROR_TIMEOUT;
    }

    sertos_port_exit_critical(crit_status);

    status = sertos_scheduler_wait_list_block(&handle->wait_list, timeout);
    if (status == SERTOS_STATUS_OK) {
        /* Task unblocked by previous owner yielding lock directly to this task */
        handle->owner = current;
        handle->lock_count = 1U;
        handle->original_owner_priority = current->base_priority;
    }

    return status;
}

SertosStatus sertos_mutex_unlock(SertosMutexHandle handle)
{
    uint32_t crit_status;
    SertosTaskControlBlock* current;
    SertosTaskControlBlock* next_owner;

    if ((handle == NULL) || (handle->magic != SERTOS_MUTEX_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    current = sertos_scheduler_get_current_tcb();
    if ((current == NULL) || (handle->owner != current)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    handle->lock_count--;
    if (handle->lock_count > 0U) {
        /* Still recursively held */
        sertos_port_exit_critical(crit_status);
        return SERTOS_STATUS_OK;
    }

    /* Full release: restore owner priority */
    restore_owner_priority(handle, current);

    /* Check if other tasks are waiting */
    next_owner = sertos_scheduler_wait_list_unblock_highest(&handle->wait_list);
    if (next_owner != NULL) {
        handle->owner = next_owner;
        handle->lock_count = 1U;
        handle->original_owner_priority = next_owner->base_priority;
    } else {
        handle->owner = NULL;
    }

    sertos_port_exit_critical(crit_status);

    if (sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

SertosTaskHandle sertos_mutex_get_owner(SertosMutexHandle handle)
{
    if ((handle == NULL) || (handle->magic != SERTOS_MUTEX_MAGIC_WORD)) {
        return NULL;
    }
    return handle->owner;
}
