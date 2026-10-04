/**
 * @file sertos_queue.c
 * @brief Thread-safe message queue IPC primitive implementation for SertOS.
 *
 * Implements bounded FIFO queues backed by ring_buffer with priority-ordered
 * sender and receiver wait lists.
 */

#include "sertos_queue.h"
#include "sertos_scheduler.h"
#include "sertos_port.h"
#include "memory_pool.h"

/**
 * @brief Returns available free bytes in the underlying ring buffer.
 *
 * @param rb Pointer to the RingBuffer instance.
 * @return Number of free bytes available for writing.
 */
static __attribute__((no_instrument_function)) size_t queue_free_bytes(const RingBuffer* rb)
{
    size_t cap = ring_buffer_capacity(rb);
    size_t cnt = ring_buffer_count(rb);
    return (cap > cnt) ? (cap - cnt) : 0U;
}

/**
 * @brief Peeks bytes from the ring buffer without moving the read index.
 *
 * @param rb Pointer to the RingBuffer instance.
 * @param dest Destination buffer.
 * @param count Number of bytes to peek.
 */
static __attribute__((no_instrument_function)) void queue_peek_bytes(const RingBuffer* rb,
                                                                      uint8_t* dest,
                                                                      size_t count)
{
    size_t tail = atomic_load_relaxed(&rb->tail);
    size_t i;

    for (i = 0U; i < count; i++) {
        dest[i] = rb->buffer[(tail + i) & rb->mask];
    }
}

/**
 * @brief Computes the next power of 2 greater than or equal to a value.
 *
 * @param value Input value.
 * @return Power of 2 value >= value.
 */
static __attribute__((no_instrument_function)) size_t next_power_of_two(size_t value)
{
    size_t p = 2U;
    while (p < value) {
        p <<= 1U;
    }
    return p;
}

SertosStatus sertos_queue_create_static(SertosQueue* queue,
                                        void* storage_buffer,
                                        size_t storage_size,
                                        size_t item_size,
                                        SertosQueueHandle* out_handle)
{
    if ((queue == NULL) || (storage_buffer == NULL) || (out_handle == NULL)) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if ((item_size == 0U) || (storage_size < 2U)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    if (!ring_buffer_init(&queue->ring_buf, (uint8_t*)storage_buffer, storage_size)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    queue->item_size = item_size;
    queue->max_items = ring_buffer_capacity(&queue->ring_buf) / item_size;
    queue->storage_buffer = storage_buffer;
    (void)linked_list_init(&queue->wait_send);
    (void)linked_list_init(&queue->wait_recv);
    queue->is_statically_allocated = true;
    queue->magic = SERTOS_QUEUE_MAGIC_WORD;

    *out_handle = queue;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_queue_create(size_t item_size,
                                 size_t max_items,
                                 SertosQueueHandle* out_handle)
{
    SertosQueue* queue;
    void* storage;
    SertosStatus status;
    size_t needed_bytes;
    size_t power_capacity;

    if (out_handle == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if ((item_size == 0U) || (max_items == 0U)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    needed_bytes = (item_size * max_items) + 1U;
    power_capacity = next_power_of_two(needed_bytes);

    queue = (SertosQueue*)memory_pool_malloc(sizeof(SertosQueue));
    if (queue == NULL) {
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    storage = memory_pool_malloc(power_capacity);
    if (storage == NULL) {
        memory_pool_free(queue);
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    status = sertos_queue_create_static(queue, storage, power_capacity, item_size, out_handle);
    if (status != SERTOS_STATUS_OK) {
        memory_pool_free(storage);
        memory_pool_free(queue);
        return status;
    }

    queue->is_statically_allocated = false;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_queue_delete(SertosQueueHandle handle)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;

    if ((handle == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    handle->magic = 0U;

    do {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_send);
    } while (unblocked != NULL);

    do {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_recv);
    } while (unblocked != NULL);

    if (!handle->is_statically_allocated) {
        memory_pool_free(handle->storage_buffer);
        memory_pool_free(handle);
    }
    sertos_port_exit_critical(crit_status);

    if (sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_queue_send(SertosQueueHandle handle, const void* item, SertosTick timeout)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;
    SertosStatus status;

    if ((handle == NULL) || (item == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    while (queue_free_bytes(&handle->ring_buf) < handle->item_size) {
        if (timeout == SERTOS_NO_WAIT) {
            sertos_port_exit_critical(crit_status);
            return SERTOS_STATUS_ERROR_TIMEOUT;
        }
        sertos_port_exit_critical(crit_status);

        status = sertos_scheduler_wait_list_block(&handle->wait_send, timeout);
        if (status != SERTOS_STATUS_OK) {
            return status;
        }
        crit_status = sertos_port_enter_critical();
    }

    (void)ring_buffer_write(&handle->ring_buf, (const uint8_t*)item, handle->item_size);
    unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_recv);
    sertos_port_exit_critical(crit_status);

    if ((unblocked != NULL) && sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_queue_receive(SertosQueueHandle handle, void* item, SertosTick timeout)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;
    SertosStatus status;

    if ((handle == NULL) || (item == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    while (ring_buffer_count(&handle->ring_buf) < handle->item_size) {
        if (timeout == SERTOS_NO_WAIT) {
            sertos_port_exit_critical(crit_status);
            return SERTOS_STATUS_ERROR_TIMEOUT;
        }
        sertos_port_exit_critical(crit_status);

        status = sertos_scheduler_wait_list_block(&handle->wait_recv, timeout);
        if (status != SERTOS_STATUS_OK) {
            return status;
        }
        crit_status = sertos_port_enter_critical();
    }

    (void)ring_buffer_read(&handle->ring_buf, (uint8_t*)item, handle->item_size);
    unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_send);
    sertos_port_exit_critical(crit_status);

    if ((unblocked != NULL) && sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_queue_peek(SertosQueueHandle handle, void* item, SertosTick timeout)
{
    uint32_t crit_status;
    SertosStatus status;

    if ((handle == NULL) || (item == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    while (ring_buffer_count(&handle->ring_buf) < handle->item_size) {
        if (timeout == SERTOS_NO_WAIT) {
            sertos_port_exit_critical(crit_status);
            return SERTOS_STATUS_ERROR_TIMEOUT;
        }
        sertos_port_exit_critical(crit_status);

        status = sertos_scheduler_wait_list_block(&handle->wait_recv, timeout);
        if (status != SERTOS_STATUS_OK) {
            return status;
        }
        crit_status = sertos_port_enter_critical();
    }

    queue_peek_bytes(&handle->ring_buf, (uint8_t*)item, handle->item_size);
    sertos_port_exit_critical(crit_status);
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_queue_send_from_isr(SertosQueueHandle handle,
                                        const void* item,
                                        bool* out_higher_prio_woken)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;
    SertosTaskControlBlock* current;

    if ((handle == NULL) || (item == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    if (out_higher_prio_woken != NULL) {
        *out_higher_prio_woken = false;
    }

    crit_status = sertos_port_enter_critical();
    if (queue_free_bytes(&handle->ring_buf) < handle->item_size) {
        sertos_port_exit_critical(crit_status);
        return SERTOS_STATUS_ERROR_RESOURCE_BUSY;
    }

    (void)ring_buffer_write(&handle->ring_buf, (const uint8_t*)item, handle->item_size);
    unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_recv);

    if (unblocked != NULL) {
        current = sertos_scheduler_get_current_tcb();
        if ((out_higher_prio_woken != NULL) && (current != NULL)) {
            if (unblocked->priority > current->priority) {
                *out_higher_prio_woken = true;
            }
        }
    }
    sertos_port_exit_critical(crit_status);

    return SERTOS_STATUS_OK;
}

SertosStatus sertos_queue_receive_from_isr(SertosQueueHandle handle,
                                           void* item,
                                           bool* out_higher_prio_woken)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;
    SertosTaskControlBlock* current;

    if ((handle == NULL) || (item == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    if (out_higher_prio_woken != NULL) {
        *out_higher_prio_woken = false;
    }

    crit_status = sertos_port_enter_critical();
    if (ring_buffer_count(&handle->ring_buf) < handle->item_size) {
        sertos_port_exit_critical(crit_status);
        return SERTOS_STATUS_ERROR_RESOURCE_BUSY;
    }

    (void)ring_buffer_read(&handle->ring_buf, (uint8_t*)item, handle->item_size);
    unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_send);

    if (unblocked != NULL) {
        current = sertos_scheduler_get_current_tcb();
        if ((out_higher_prio_woken != NULL) && (current != NULL)) {
            if (unblocked->priority > current->priority) {
                *out_higher_prio_woken = true;
            }
        }
    }
    sertos_port_exit_critical(crit_status);

    return SERTOS_STATUS_OK;
}

size_t sertos_queue_get_count(SertosQueueHandle handle)
{
    if ((handle == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return 0U;
    }
    return ring_buffer_count(&handle->ring_buf) / handle->item_size;
}

size_t sertos_queue_get_spaces_available(SertosQueueHandle handle)
{
    if ((handle == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return 0U;
    }
    return queue_free_bytes(&handle->ring_buf) / handle->item_size;
}

SertosStatus sertos_queue_reset(SertosQueueHandle handle)
{
    uint32_t crit_status;

    if ((handle == NULL) || (handle->magic != SERTOS_QUEUE_MAGIC_WORD)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    ring_buffer_clear(&handle->ring_buf);
    sertos_port_exit_critical(crit_status);

    return SERTOS_STATUS_OK;
}
