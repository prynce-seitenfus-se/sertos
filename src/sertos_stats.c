/**
 * @file sertos_stats.c
 * @brief Runtime kernel statistics (telemetry) implementation for SertOS.
 *
 * Implements deterministic O(1) per-switch accounting and O(n) snapshot
 * walkers conforming to ISO C99 and MISRA C:2012.
 */

#include "sertos_stats.h"
#include "sertos_scheduler.h"
#include "sertos_port.h"
#include "sertos_task.h"
#include <string.h>

/**
 * @brief Runtime accounting enablement flag (zero-initialized in .bss).
 */
static bool s_stats_enabled = false;

/**
 * @brief Global count of context switches recorded since the last reset.
 */
static uint32_t s_total_switches = 0U;

/**
 * @brief Context passed to the task-summation visitor.
 */
typedef struct StatsSumContext {
    SertosRunCount total;   /**< Accumulated run-time across visited tasks. */
    uint32_t count;         /**< Number of tasks visited. */
} StatsSumContext;

/**
 * @brief Context passed to the per-task snapshot fill visitor.
 */
typedef struct StatsFillContext {
    SertosTaskStats* arr;   /**< Destination array. */
    size_t capacity;        /**< Capacity of the destination array. */
    size_t written;         /**< Number of records written so far. */
    SertosRunCount total;   /**< Accumulated run-time across all visited tasks. */
} StatsFillContext;

/**
 * @brief Computes a fixed-point hundredths-of-a-percent share.
 *
 * @param part  Numerator run-time component.
 * @param total Denominator total run-time.
 * @return Share in hundredths of a percent (0..10000), or 0 if total is 0.
 */
static __attribute__((no_instrument_function)) uint32_t compute_percent_x100(SertosRunCount part, SertosRunCount total)
{
    if (total == 0U) {
        return 0U;
    }

    return (uint32_t)((part * 10000ULL) / total);
}

/**
 * @brief Visitor that zeroes a task's counters and reseeds its baseline.
 *
 * @param tcb Task control block to reset.
 * @param ctx Pointer to the current run-time counter sample.
 */
static __attribute__((no_instrument_function)) void reset_visitor(SertosTaskControlBlock* tcb, void* ctx)
{
    const uint32_t* now = (const uint32_t*)ctx;

    tcb->run_time_total = 0U;
    tcb->run_time_last_entry = *now;
    tcb->switch_in_count = 0U;
}

/**
 * @brief Visitor that accumulates total run-time and task count.
 *
 * @param tcb Task control block to sum.
 * @param ctx Pointer to a StatsSumContext.
 */
static __attribute__((no_instrument_function)) void sum_visitor(SertosTaskControlBlock* tcb, void* ctx)
{
    StatsSumContext* sum = (StatsSumContext*)ctx;

    sum->total += tcb->run_time_total;
    sum->count++;
}

/**
 * @brief Visitor that fills a per-task snapshot record and sums total run-time.
 *
 * @param tcb Task control block to capture.
 * @param ctx Pointer to a StatsFillContext.
 */
static __attribute__((no_instrument_function)) void fill_visitor(SertosTaskControlBlock* tcb, void* ctx)
{
    StatsFillContext* fill = (StatsFillContext*)ctx;
    SertosTaskStats* rec;

    fill->total += tcb->run_time_total;
    if (fill->written < fill->capacity) {
        rec = &fill->arr[fill->written];
        rec->name = tcb->name;
        rec->priority = tcb->priority;
        rec->state = tcb->state;
        rec->run_time = tcb->run_time_total;
        rec->cpu_percent_x100 = 0U;
        rec->switch_in_count = tcb->switch_in_count;
        rec->stack_high_water = sertos_task_get_stack_high_water_mark(tcb);
        fill->written++;
    }
}

__attribute__((no_instrument_function)) void sertos_stats_set_enabled(bool enabled)
{
    s_stats_enabled = enabled;
}

bool sertos_stats_is_enabled(void)
{
    return s_stats_enabled;
}

__attribute__((no_instrument_function)) void sertos_stats_on_switch(SertosTaskControlBlock* prev,
                                                                     SertosTaskControlBlock* next)
{
    uint32_t now;
    uint32_t delta;

    if (!s_stats_enabled) {
        return;
    }

    now = sertos_port_runtime_counter();
    if (prev != NULL) {
        delta = now - prev->run_time_last_entry;
        prev->run_time_total += (SertosRunCount)delta;
    }

    if (next != NULL) {
        next->run_time_last_entry = now;
        if (prev != next) {
            next->switch_in_count++;
            s_total_switches++;
        }
    }
}

__attribute__((no_instrument_function)) void sertos_stats_reset(void)
{
    uint32_t now;

    now = sertos_port_runtime_counter();
    sertos_scheduler_visit_all_tasks(reset_visitor, &now);
    s_total_switches = 0U;
}

SertosStatus sertos_stats_get_system(SertosSystemStats* out)
{
    StatsSumContext sum;
    SertosTaskControlBlock* idle;

    if (out == NULL) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if (!s_stats_enabled) {
        (void)memset(out, 0, sizeof(*out));
        return SERTOS_STATUS_ERROR_NOT_INITIALIZED;
    }

    sum.total = 0U;
    sum.count = 0U;
    sertos_scheduler_visit_all_tasks(sum_visitor, &sum);
    idle = sertos_scheduler_get_idle_tcb();

    out->total_run_time = sum.total;
    out->idle_run_time = idle->run_time_total;
    out->idle_percent_x100 = compute_percent_x100(idle->run_time_total, sum.total);
    out->total_switches = s_total_switches;
    out->total_ticks = sertos_scheduler_get_tick_count();
    out->task_count = sum.count;

    return SERTOS_STATUS_OK;
}

size_t sertos_stats_get_tasks(SertosTaskStats* out_array, size_t capacity)
{
    StatsFillContext fill;
    size_t i;

    if ((out_array == NULL) || (capacity == 0U) || !s_stats_enabled) {
        return 0U;
    }

    fill.arr = out_array;
    fill.capacity = capacity;
    fill.written = 0U;
    fill.total = 0U;
    sertos_scheduler_visit_all_tasks(fill_visitor, &fill);

    for (i = 0U; i < fill.written; i++) {
        out_array[i].cpu_percent_x100 = compute_percent_x100(out_array[i].run_time, fill.total);
    }

    return fill.written;
}

SertosStatus sertos_stats_get_task(SertosTaskHandle handle, SertosTaskStats* out)
{
    SertosSystemStats sys;
    SertosStatus status;

    if ((handle == NULL) || (out == NULL)) {
        return SERTOS_STATUS_ERROR_NULL_PTR;
    }
    if (!s_stats_enabled) {
        (void)memset(out, 0, sizeof(*out));
        return SERTOS_STATUS_ERROR_NOT_INITIALIZED;
    }

    status = sertos_stats_get_system(&sys);
    if (status != SERTOS_STATUS_OK) {
        return status;
    }

    out->name = handle->name;
    out->priority = handle->priority;
    out->state = handle->state;
    out->run_time = handle->run_time_total;
    out->cpu_percent_x100 = compute_percent_x100(handle->run_time_total, sys.total_run_time);
    out->switch_in_count = handle->switch_in_count;
    out->stack_high_water = sertos_task_get_stack_high_water_mark(handle);

    return SERTOS_STATUS_OK;
}

uint32_t sertos_stats_get_cpu_load_x100(void)
{
    SertosSystemStats sys;

    if (sertos_stats_get_system(&sys) != SERTOS_STATUS_OK) {
        return 0U;
    }

    return 10000U - sys.idle_percent_x100;
}
