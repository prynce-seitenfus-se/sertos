/**
 * @file sertos_types.h
 * @brief Fundamental kernel types, error codes, and handles for SertOS.
 *
 * Conforms to ISO C99 and MISRA C:2012. Defines architecture-agnostic primitives
 * and return status enumerations.
 */

#ifndef SERTOS_TYPES_H
#define SERTOS_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "sertos_config.h"

/**
 * @brief Kernel operation return status codes.
 */
typedef enum SertosStatus {
    SERTOS_STATUS_OK                     = 0,  /**< Operation completed successfully. */
    SERTOS_STATUS_ERROR_NULL_PTR         = 1,  /**< A null pointer argument was provided. */
    SERTOS_STATUS_ERROR_INVALID_PARAM    = 2,  /**< An invalid parameter value was provided. */
    SERTOS_STATUS_ERROR_NO_MEMORY        = 3,  /**< Memory pool exhausted or stack buffer insufficient. */
    SERTOS_STATUS_ERROR_TIMEOUT          = 4,  /**< Operation timed out before completion. */
    SERTOS_STATUS_ERROR_RESOURCE_BUSY    = 5,  /**< Resource unavailable or locked. */
    SERTOS_STATUS_ERROR_NOT_INITIALIZED  = 6,  /**< Kernel subsystem not initialized. */
    SERTOS_STATUS_ERROR_ISR_CONTEXT      = 7,  /**< Operation illegal from interrupt service routine. */
    SERTOS_STATUS_ERROR_STACK_OVERFLOW   = 8   /**< Stack canary corrupted or boundary violated. */
} SertosStatus;

/**
 * @brief Task priority level.
 *
 * Lower numerical values denote lower priority; 0 is reserved for the Idle Task.
 */
typedef uint8_t SertosPriority;

/**
 * @brief System tick count representation.
 */
typedef uint32_t SertosTick;

/**
 * @brief Cumulative run-time counter representation for runtime statistics.
 *
 * Widened to 64 bits so that accumulation of a fast free-running 32-bit CPU
 * cycle counter does not overflow during long uptimes.
 */
typedef uint64_t SertosRunCount;

/**
 * @brief Special timeout constants.
 */
#define SERTOS_NO_WAIT       ((SertosTick)0U)
#define SERTOS_WAIT_FOREVER  ((SertosTick)0xFFFFFFFFU)

/**
 * @brief Stack memory alignment boundary in bytes.
 */
#define SERTOS_STACK_ALIGNMENT_BYTES (SERTOS_CONFIG_STACK_ALIGNMENT_BYTES)

/**
 * @brief Task entry function pointer type.
 *
 * @param param User-supplied context pointer passed during task creation.
 */
typedef void (*SertosTaskFunction)(void* param);

/* Forward declaration of the internal Task Control Block structure. */
struct SertosTaskControlBlock;

/**
 * @brief Opaque task handle exposed to application and API layers.
 */
typedef struct SertosTaskControlBlock* SertosTaskHandle;

/**
 * @brief Runtime kernel scheduler configuration parameters.
 *
 * Enables dynamic customization of the scheduler without requiring
 * recompilation of precompiled static kernel libraries.
 */
typedef struct SertosConfig {
    uint32_t tick_rate_hz;              /**< Tick rate in Hz (e.g. 1000U for 1ms). If 0, uses default SERTOS_CONFIG_TICK_RATE_HZ. */
    bool enable_time_slicing;           /**< True to enable round-robin time slicing, false for cooperative scheduling. */
    void* idle_task_stack;              /**< Optional caller-supplied static buffer for Idle task stack (NULL for internal buffer). */
    size_t idle_task_stack_size;        /**< Size in bytes of idle_task_stack (must be >= SERTOS_CONFIG_MINIMAL_STACK_SIZE if supplied). */
    void (*tick_hook)(void);            /**< Optional user callback invoked monotonically on each scheduler tick (or NULL). */
    void (*idle_hook)(void);            /**< Optional user callback invoked in the Idle task loop (or NULL). */
    bool enable_runtime_stats;          /**< True to enable per-task runtime statistics accounting on each context switch (default false). */
    /**
     * Optional context-switch callback (or NULL), invoked with interrupts masked
     * after the scheduler commits a switch from prev to next (prev != next; prev
     * is NULL on the first switch). On Cortex-M it runs inside PendSV: it must
     * be short, must not block or call kernel APIs, and should be declared
     * __attribute__((no_instrument_function)).
     */
    void (*switch_hook)(SertosTaskHandle prev, SertosTaskHandle next);
} SertosConfig;

#endif /* SERTOS_TYPES_H */
