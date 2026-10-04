/**
 * @file sertos_stream_buffer.c
 * @brief Single-Producer Single-Consumer (SPSC) byte stream buffer primitive implementation.
 *
 * Implements lightweight, byte-oriented stream buffers backed by ring_buffer with
 * priority-ordered sender and receiver blocking wait lists.
 */

#include "sertos_stream_buffer.h"
#include "sertos_scheduler.h"
#include "sertos_port.h"
#include "memory_pool.h"

/**
 * @brief Validates stream buffer handle and integrity canary.
 *
 * @param handle Pointer to SertosStreamBuffer.
 * @return true if handle is non-null and valid.
 */
static inline __attribute__((no_instrument_function)) bool is_valid_handle(const SertosStreamBuffer* handle)
{
    if (handle == NULL) {
        return false;
    }
    return (handle->magic == SERTOS_STREAM_BUFFER_MAGIC_WORD);
}

/**
 * @brief Validates send parameters.
 *
 * @param handle Stream buffer handle.
 * @param data Data pointer.
 * @param length Byte length.
 * @return true if all parameters are valid.
 */
static __attribute__((no_instrument_function)) bool is_send_params_valid(const SertosStreamBuffer* handle,
                                                                          const void* data,
                                                                          size_t length)
{
    if (!is_valid_handle(handle)) {
        return false;
    }
    return ((data != NULL) && (length > 0U));
}

/**
 * @brief Validates receive parameters.
 *
 * @param handle Stream buffer handle.
 * @param buffer Buffer pointer.
 * @param max_length Maximum byte length.
 * @return true if all parameters are valid.
 */
static __attribute__((no_instrument_function)) bool is_recv_params_valid(const SertosStreamBuffer* handle,
                                                                          const void* buffer,
                                                                          size_t max_length)
{
    if (!is_valid_handle(handle)) {
        return false;
    }
    return ((buffer != NULL) && (max_length > 0U));
}

/**
 * @brief Triggers scheduler context switch if an unblocked task requires rescheduling.
 *
 * @param unblocked Unblocked task pointer.
 */
static __attribute__((no_instrument_function)) void notify_unblocked_task(const SertosTaskControlBlock* unblocked)
{
    if ((unblocked != NULL) && sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }
}

/**
 * @brief Evaluates whether unblocking a task in ISR context requires a context switch.
 *
 * @param unblocked Task unblocked by ISR operation.
 * @param out_higher_prio_woken Pointer to store flag indicating higher priority woken.
 */
static __attribute__((no_instrument_function)) void update_higher_prio_woken(const SertosTaskControlBlock* unblocked,
                                                                              bool* out_higher_prio_woken)
{
    if ((unblocked != NULL) && (out_higher_prio_woken != NULL)) {
        SertosTaskControlBlock* current = sertos_scheduler_get_current_tcb();
        if ((current != NULL) && (unblocked->priority > current->priority)) {
            *out_higher_prio_woken = true;
        }
    }
}

/**
 * @brief Returns available free bytes in the underlying ring buffer.
 *
 * @param rb Pointer to the RingBuffer instance.
 * @return Number of free bytes available for writing.
 */
static __attribute__((no_instrument_function)) size_t stream_buffer_free_bytes(const RingBuffer* rb)
{
    size_t cap = ring_buffer_capacity(rb);
    size_t cnt = ring_buffer_count(rb);
    return (cap > cnt) ? (cap - cnt) : 0U;
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

SertosStatus sertos_stream_buffer_create_static(SertosStreamBuffer* stream_buf,
                                                void* storage_buffer,
                                                size_t storage_size,
                                                size_t trigger_level_bytes,
                                                SertosStreamBufferHandle* out_handle)
{
    if ((stream_buf == NULL) || (storage_buffer == NULL) || (out_handle == NULL)) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if (storage_size < 2U) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    if (!ring_buffer_init(&stream_buf->ring_buf, (uint8_t*)storage_buffer, storage_size)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    if (trigger_level_bytes == 0U) {
        trigger_level_bytes = 1U;
    }
    if (trigger_level_bytes > ring_buffer_capacity(&stream_buf->ring_buf)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    stream_buf->trigger_level_bytes = trigger_level_bytes;
    stream_buf->storage_buffer = storage_buffer;
    (void)linked_list_init(&stream_buf->wait_send);
    (void)linked_list_init(&stream_buf->wait_recv);
    stream_buf->is_statically_allocated = true;
    stream_buf->magic = SERTOS_STREAM_BUFFER_MAGIC_WORD;

    *out_handle = stream_buf;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_stream_buffer_create(size_t buffer_size,
                                         size_t trigger_level_bytes,
                                         SertosStreamBufferHandle* out_handle)
{
    size_t needed_bytes;
    size_t power_capacity;
    SertosStreamBuffer* stream_buf;
    void* storage;
    SertosStatus status;

    if (out_handle == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if (buffer_size == 0U) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    needed_bytes = buffer_size + 1U;
    power_capacity = next_power_of_two(needed_bytes);

    stream_buf = (SertosStreamBuffer*)memory_pool_malloc(sizeof(SertosStreamBuffer));
    if (stream_buf == NULL) {
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    storage = memory_pool_malloc(power_capacity);
    if (storage == NULL) {
        memory_pool_free(stream_buf);
        return SERTOS_STATUS_ERROR_NO_MEMORY;
    }

    status = sertos_stream_buffer_create_static(stream_buf,
                                                storage,
                                                power_capacity,
                                                trigger_level_bytes,
                                                out_handle);
    if (status != SERTOS_STATUS_OK) {
        memory_pool_free(storage);
        memory_pool_free(stream_buf);
        return status;
    }

    stream_buf->is_statically_allocated = false;
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_stream_buffer_delete(SertosStreamBufferHandle handle)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;

    if (!is_valid_handle(handle)) {
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

size_t sertos_stream_buffer_send(SertosStreamBufferHandle handle,
                                 const void* data,
                                 size_t length,
                                 SertosTick timeout)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked = NULL;
    SertosStatus status;
    size_t bytes_sent = 0U;

    if (!is_send_params_valid(handle, data, length)) {
        return 0U;
    }

    crit_status = sertos_port_enter_critical();
    while (stream_buffer_free_bytes(&handle->ring_buf) == 0U) {
        if (timeout == SERTOS_NO_WAIT) {
            sertos_port_exit_critical(crit_status);
            return 0U;
        }
        sertos_port_exit_critical(crit_status);

        status = sertos_scheduler_wait_list_block(&handle->wait_send, timeout);
        crit_status = sertos_port_enter_critical();
        if ((status != SERTOS_STATUS_OK) || !is_valid_handle(handle)) {
            sertos_port_exit_critical(crit_status);
            return 0U;
        }
    }

    bytes_sent = ring_buffer_write(&handle->ring_buf, (const uint8_t*)data, length);
    if (ring_buffer_count(&handle->ring_buf) >= handle->trigger_level_bytes) {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_recv);
    }
    sertos_port_exit_critical(crit_status);

    notify_unblocked_task(unblocked);
    return bytes_sent;
}

size_t sertos_stream_buffer_receive(SertosStreamBufferHandle handle,
                                    void* buffer,
                                    size_t max_length,
                                    SertosTick timeout)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked = NULL;
    SertosStatus status;
    size_t bytes_received = 0U;

    if (!is_recv_params_valid(handle, buffer, max_length)) {
        return 0U;
    }

    crit_status = sertos_port_enter_critical();
    while (ring_buffer_count(&handle->ring_buf) < handle->trigger_level_bytes) {
        if (timeout == SERTOS_NO_WAIT) {
            break;
        }
        sertos_port_exit_critical(crit_status);

        status = sertos_scheduler_wait_list_block(&handle->wait_recv, timeout);
        crit_status = sertos_port_enter_critical();
        if (!is_valid_handle(handle)) {
            sertos_port_exit_critical(crit_status);
            return 0U;
        }
        if (status != SERTOS_STATUS_OK) {
            break;
        }
    }

    bytes_received = ring_buffer_read(&handle->ring_buf, (uint8_t*)buffer, max_length);
    if (bytes_received > 0U) {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_send);
    }
    sertos_port_exit_critical(crit_status);

    notify_unblocked_task(unblocked);
    return bytes_received;
}

size_t sertos_stream_buffer_send_from_isr(SertosStreamBufferHandle handle,
                                          const void* data,
                                          size_t length,
                                          bool* out_higher_prio_woken)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked = NULL;
    size_t bytes_sent;

    if (out_higher_prio_woken != NULL) {
        *out_higher_prio_woken = false;
    }
    if (!is_send_params_valid(handle, data, length)) {
        return 0U;
    }

    crit_status = sertos_port_enter_critical();
    bytes_sent = ring_buffer_write(&handle->ring_buf, (const uint8_t*)data, length);
    if ((bytes_sent > 0U) &&
        (ring_buffer_count(&handle->ring_buf) >= handle->trigger_level_bytes)) {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_recv);
        update_higher_prio_woken(unblocked, out_higher_prio_woken);
    }
    sertos_port_exit_critical(crit_status);

    return bytes_sent;
}

size_t sertos_stream_buffer_receive_from_isr(SertosStreamBufferHandle handle,
                                             void* buffer,
                                             size_t max_length,
                                             bool* out_higher_prio_woken)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked = NULL;
    size_t bytes_recv;

    if (out_higher_prio_woken != NULL) {
        *out_higher_prio_woken = false;
    }
    if (!is_recv_params_valid(handle, buffer, max_length)) {
        return 0U;
    }

    crit_status = sertos_port_enter_critical();
    bytes_recv = ring_buffer_read(&handle->ring_buf, (uint8_t*)buffer, max_length);
    if (bytes_recv > 0U) {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_send);
        update_higher_prio_woken(unblocked, out_higher_prio_woken);
    }
    sertos_port_exit_critical(crit_status);

    return bytes_recv;
}

SertosStatus sertos_stream_buffer_set_trigger_level(SertosStreamBufferHandle handle,
                                                    size_t trigger_level_bytes)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked = NULL;

    if (!is_valid_handle(handle)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }
    if (trigger_level_bytes == 0U) {
        trigger_level_bytes = 1U;
    }
    if (trigger_level_bytes > ring_buffer_capacity(&handle->ring_buf)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    handle->trigger_level_bytes = trigger_level_bytes;
    if (ring_buffer_count(&handle->ring_buf) >= trigger_level_bytes) {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_recv);
    }
    sertos_port_exit_critical(crit_status);

    notify_unblocked_task(unblocked);
    return SERTOS_STATUS_OK;
}

SertosStatus sertos_stream_buffer_reset(SertosStreamBufferHandle handle)
{
    uint32_t crit_status;
    SertosTaskControlBlock* unblocked;

    if (!is_valid_handle(handle)) {
        return SERTOS_STATUS_ERROR_INVALID_PARAM;
    }

    crit_status = sertos_port_enter_critical();
    ring_buffer_clear(&handle->ring_buf);

    do {
        unblocked = sertos_scheduler_wait_list_unblock_highest(&handle->wait_send);
    } while (unblocked != NULL);
    sertos_port_exit_critical(crit_status);

    if (sertos_scheduler_is_running()) {
        sertos_scheduler_reschedule();
    }

    return SERTOS_STATUS_OK;
}

size_t sertos_stream_buffer_bytes_available(SertosStreamBufferHandle handle)
{
    if (!is_valid_handle(handle)) {
        return 0U;
    }
    return ring_buffer_count(&handle->ring_buf);
}

size_t sertos_stream_buffer_spaces_available(SertosStreamBufferHandle handle)
{
    if (!is_valid_handle(handle)) {
        return 0U;
    }
    return stream_buffer_free_bytes(&handle->ring_buf);
}

bool sertos_stream_buffer_is_empty(SertosStreamBufferHandle handle)
{
    if (!is_valid_handle(handle)) {
        return true;
    }
    return ring_buffer_is_empty(&handle->ring_buf);
}

bool sertos_stream_buffer_is_full(SertosStreamBufferHandle handle)
{
    if (!is_valid_handle(handle)) {
        return false;
    }
    return ring_buffer_is_full(&handle->ring_buf);
}
