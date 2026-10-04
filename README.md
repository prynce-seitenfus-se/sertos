# SertOS: Safety-Critical Preemptive Real-Time Operating System

**SertOS** is a strict ISO C99, freestanding, MISRA C:2012-compliant, hardware-agnostic preemptive real-time operating system kernel designed for deterministic mission-critical applications.

---

## Key Architectural Principles

1. **Freestanding & Zero Syscalls**:
   - Zero OS dynamic allocation (`malloc`/`free`/`sbrk`) per MISRA C:2012 Rule 21.3.
   - Dual task provisioning: caller-supplied static buffers (`sertos_task_create_static`) or deterministic block pool allocation (`sertos_task_create`).

2. **Deterministic $O(1)$ Preemptive Scheduling**:
   - Up to 32 scheduling priority levels using single-cycle Count-Leading-Zeros (CLZ) bitmap search.
   - Intrusive circular doubly linked lists for round-robin time-slicing among equal-priority tasks.

3. **Multi-Architecture Hardware Abstraction**:
   - **ARM Cortex-M55** (ARMv8.1-M Mainline with Helium MVE, double-precision FPU, `PSPLIM` stack limits).
   - **ARM Cortex-M33** (ARMv8-M Mainline with `PSPLIM` hardware stack limit traps and TrustZone).
   - **ARM Cortex-M23** (ARMv8-M Baseline with `PSPLIM` stack limit traps).
   - **ARM Cortex-M7** (ARMv7E-M with double-precision FPU and cache hooks).
   - **ARM Cortex-M4** (ARMv7E-M with single-precision floating-point stacking).
   - **ARM Cortex-M3** (ARMv7-M Thumb-2).
   - **ARM Cortex-M0+** (ARMv6-M with VTOR vector table relocation).
   - **ARM Cortex-M0** (ARMv6-M Thumb-1).
   - **RISC-V RV32I** (32-bit machine-mode trap handler).
   - **Native Windows Simulator** (`port/windows/` for Win32 MinGW-w64).
   - **Native POSIX Simulator** (`port/posix/` for Linux/macOS).

4. **Stack Safety & Canaries**:
   - Configurable stack alignment (8-byte AAPCS compliant).
   - Pattern painting (`0xA5`) and stack high-water mark runtime analysis (`sertos_task_get_stack_high_water_mark`).
   - Magic word TCB canary protection (`0x54434221U`).

## Foundational Submodules

SertOS leverages 7 modular, freestanding submodules tracked via Git submodules under `modules/`:

| Module | Purpose in SertOS | Key Integration Points |
| :--- | :--- | :--- |
| **`atomic`** | Lock-free synchronization | Atomic tick counter (`s_system_ticks`) with acquire/release memory semantics. |
| **`bitmap`** | $O(1)$ priority search | 32-level scheduler ready table using hardware CLZ instruction. |
| **`linked_list`** | Intrusive task queues | Circular doubly linked lists for ready tasks, blocked delay lists, and mutex owner tracking. |
| **`memory_pool`** | Deterministic allocation | Fixed-size block allocation for dynamic tasks, semaphores, mutexes, and queues without OS syscalls. |
| **`ring_buffer`** | FIFO storage | Lock-free single-producer single-consumer circular queue backing `sertos_queue`. |
| **`crc`** | Integrity validation | 32-bit CRC checksum calculation over TCB metadata and stack integrity guards. |
| **`fsm`** | Task lifecycle governance | Formal state transition verification (`READY`, `RUNNING`, `BLOCKED`, `SUSPENDED`, `TERMINATED`). |

---

## Directory Structure

```text
sertos/
├── .gitmodules                 # Submodule configuration
├── CMakeLists.txt              # Static library build configuration
├── inc/                        # Public architecture-agnostic headers
│   ├── sertos_config.h         # User-configurable parameters
│   ├── sertos_types.h          # Core types and status codes
│   ├── sertos_task.h           # TCB and task lifecycle API
│   ├── sertos_scheduler.h      # Preemptive scheduler API
│   ├── sertos_sem.h            # Binary and counting semaphores
│   ├── sertos_mutex.h          # Priority Inheritance Protocol mutex
│   ├── sertos_queue.h          # FIFO message queue (ring_buffer)
│   ├── sertos_stream_buffer.h  # SPSC byte stream buffer
│   └── sertos_timer.h          # Monotonic software timers
├── src/                        # Core portable kernel source
│   ├── sertos_task.c           # Task creation, deletion, stack checks
│   ├── sertos_scheduler.c      # O(1) bitmap scheduler engine
│   ├── sertos_sem.c            # Semaphore take/give/ISR signaling
│   ├── sertos_mutex.c          # Recursive mutex with PIP elevation
│   ├── sertos_queue.c          # Thread-safe multi-task queue
│   ├── sertos_stream_buffer.c  # SPSC byte stream buffer implementation
│   └── sertos_timer.c          # Software timer dispatcher
├── modules/                    # Foundational submodules
│   ├── atomic/                 # Lock-free primitives & memory barriers
│   ├── bitmap/                 # Bit array and priority indexing
│   ├── crc/                    # CRC-8/16/32 checksum engines
│   ├── fsm/                    # Table-driven state machine
│   ├── linked_list/            # Intrusive circular doubly linked list
│   ├── memory_pool/            # Deterministic block allocator
│   └── ring_buffer/            # Circular byte buffer
└── port/                       # Hardware and simulator ports
    ├── sertos_port.h           # Hardware port contract
    ├── windows/                # Win32 host simulator
    ├── posix/                  # POSIX host simulator
    ├── arm/
    │   ├── cortex-m55/         # ARMv8.1-M (Helium MVE + PSPLIM)
    │   ├── cortex-m33/         # ARMv8-M Mainline (PSPLIM stack limits)
    │   ├── cortex-m23/         # ARMv8-M Baseline (PSPLIM stack limits)
    │   ├── cortex-m7/          # ARMv7E-M (Double-Precision FPU)
    │   ├── cortex-m4/          # ARMv7E-M (Lazy Single-Precision FPU)
    │   ├── cortex-m3/          # ARMv7-M (Thumb-2)
    │   ├── cortex-m0plus/      # ARMv6-M (VTOR relocation)
    │   └── cortex-m0/          # ARMv6-M (Thumb-1)
    └── riscv/
        └── rv32i/              # RISC-V 32-bit trap handler
```

---

## Kernel Configuration & Customization

SertOS provides a dual configuration model so applications can customize kernel behavior either at runtime (without rebuilding static libraries) or at compile time (via application header overrides):

### 1. Dynamic Runtime Configuration (`SertosConfig`)
The scheduler can be initialized with custom parameters directly from user code via [`sertos_scheduler_init_with_config`](inc/sertos_scheduler.h). Applications can simply include the single primary umbrella header [`sertos.h`](inc/sertos.h):

```c
#include "sertos.h"

static uint8_t s_custom_idle_stack[1024] __attribute__((aligned(8)));

void custom_tick_hook(void)
{
    /* User logic executed monotonically on each timer tick */
}

void app_init(void)
{
    const SertosConfig cfg = {
        .tick_rate_hz        = 500U,                  /* 500 Hz = 2 ms tick resolution */
        .enable_time_slicing = true,                  /* Enable round-robin time slicing */
        .idle_task_stack     = s_custom_idle_stack,   /* Custom static Idle stack buffer */
        .idle_task_stack_size= sizeof(s_custom_idle_stack),
        .tick_hook           = custom_tick_hook,      /* Optional tick callback */
        .idle_hook           = NULL,                  /* Optional idle loop callback */
        .enable_runtime_stats= false                  /* Enable per-task CPU telemetry (see below) */
    };

    (void)sertos_scheduler_init_with_config(&cfg);
}
```

Calling `sertos_scheduler_init()` remains supported as a zero-overhead convenience wrapper that applies default configuration parameters.

### 2. Header-Based Customization (`sertos_app_config.h`)
When building SertOS directly as a CMake submodule or from source, you can define `sertos_app_config.h` in your project's include path to override default `#ifndef` parameters without modifying repository files:

```c
/* sertos_app_config.h */
#define SERTOS_CONFIG_TICK_RATE_HZ          (500U)
#define SERTOS_CONFIG_TIME_SLICING          (0U)
#define SERTOS_CONFIG_IDLE_TASK_STACK_SIZE  (1024U)
```

### 3. Runtime Statistics / Telemetry (`sertos_stats.h`)
SertOS ships an always-compiled, zero-allocation runtime-statistics subsystem that tracks per-task CPU time, context-switch counts, idle/CPU load, and stack high-water usage. Collection is **runtime-configured**: it is enabled at initialization through `SertosConfig.enable_runtime_stats` (default `false`). When disabled, the per-switch hot path costs a single predictable branch; the per-task TCB overhead (a few machine words) is always present.

Accounting uses a free-running run-time counter that ticks faster than the scheduler tick. Each port provides a default `sertos_port_runtime_counter()` (DWT `CYCCNT` on Cortex-M3/M4/M7/M33/M55, `mcycle` on RISC-V, `QueryPerformanceCounter` on Windows, `CLOCK_MONOTONIC` on POSIX; a coarse tick-based fallback on Cortex-M0/M0+/M23). The DWT-based ports probe `CYCCNT` at init and automatically fall back to the tick-based counter when the cycle counter is unavailable (e.g. some QEMU machine models), so statistics remain meaningful under emulation. For cross-compiled targets the hook is weak — override it with a dedicated hardware timer for higher resolution:

```c
/* Optional application override for a high-resolution timer */
uint32_t sertos_port_runtime_counter(void)
{
    return MY_TIMER->CNT;
}
```

Enable the feature and dump a CPU-load table from a periodic task:

```c
#include "sertos.h"

void stats_task(void* param)
{
    SertosTaskStats tasks[SERTOS_CONFIG_STATS_MAX_TASKS];
    SertosSystemStats sys;
    SertosTick wake;

    (void)param;
    wake = sertos_scheduler_get_tick_count();
    for (;;) {
        size_t count = sertos_stats_get_tasks(tasks, SERTOS_CONFIG_STATS_MAX_TASKS);
        (void)sertos_stats_get_system(&sys);

        /* sys.idle_percent_x100 and each tasks[i].cpu_percent_x100 are in
           hundredths of a percent (e.g. 4250 == 42.50%). */
        for (size_t i = 0U; i < count; i++) {
            printf("%-12s prio=%u cpu=%u.%02u%% switches=%u stack_free=%zu\n",
                   tasks[i].name, tasks[i].priority,
                   tasks[i].cpu_percent_x100 / 100U, tasks[i].cpu_percent_x100 % 100U,
                   tasks[i].switch_in_count, tasks[i].stack_high_water);
        }
        (void)sertos_scheduler_delay_until(&wake, SERTOS_MS_TO_TICKS(1000U));
    }
}

void app_init(void)
{
    SertosConfig cfg = { 0 };
    cfg.tick_rate_hz = 1000U;
    cfg.enable_runtime_stats = true;   /* Turn on telemetry collection */
    (void)sertos_scheduler_init_with_config(&cfg);
    /* ... create tasks, then sertos_scheduler_start(); */
}
```

The snapshot API is deterministic: accounting is O(1) per context switch, while `sertos_stats_get_tasks()` / `sertos_stats_get_system()` walk the task lists once under a critical section and copy into caller-provided buffers (no dynamic allocation, no callbacks). `sertos_stats_reset()` zeroes all counters, and `sertos_stats_get_cpu_load_x100()` returns `10000 − idle%`. Because the run-time counter is 32-bit and may wrap, deltas are accumulated into a 64-bit `SertosRunCount` on every switch.

---

## Building the Kernel Library

### Tick Timer Clock

The Cortex-M ports use SysTick with the processor clock as its input. Their
default tick clock remains 25 MHz for compatibility. If the target runs at a
different frequency, provide a strong definition of the port clock hook in the
application. The hook is called when the scheduler starts, so initialize the
target clock first:

```c
#include "sertos_port.h"
#include "stm32h5xx_hal.h"

uint32_t sertos_port_tick_clock_hz(void)
{
    return SystemCoreClock;
}
```

The returned frequency must describe the tick timer's input clock, not
necessarily the CPU clock on every architecture. The RISC-V QEMU virt port
defaults to its 10 MHz CLINT timer input. Host simulator ports do not use the
ARM-only `cortex_m_systick()` helper; no empty non-ARM implementation is
provided because those ports do not have SysTick hardware. Build the port
library once with this hook-based implementation; each
application can then provide its own strong override at final link time without
rebuilding the library for every clock frequency. `sertos_scheduler_start()`
returns only if the port rejects its tick timer configuration or the active
port supports stopping the scheduler; applications may treat any return as a
startup failure on bare-metal targets.

### Automated Batch Script (`build.bat`)
The repository provides a unified `build.bat` script supporting both Host simulators (Windows/POSIX) and all 8 ARM Cortex targets:

```powershell
# Build all target libraries (mingw64 + linux + 8 ARM Cortex + RISC-V targets)
.\build.bat all

# Build MinGW-w64 host library (lib/mingw64/libsertos_mingw64.a)
.\build.bat mingw64
.\build.bat windows

# Build POSIX host library (lib/posix/libsertos_posix.a)
.\build.bat linux
.\build.bat posix

# Build RISC-V RV32I library (lib/riscv/libsertos_rv32i.a)
.\build.bat riscv
.\build.bat rv32i

# Build all 8 ARM Cortex libraries (lib/arm/libsertos_cortex_m*.a)
.\build.bat arm

# Build a specific ARM target (m0, m0plus, m3, m4, m7, m23, m33, or m55)
.\build.bat m7
.\build.bat m55

# Remove generated build and library outputs before rebuilding a target
.\build.bat --clean m33
```

### CMake Alternative
The kernel can also be built using CMake:

```powershell
# Configure and build library for host simulation
cmake -B build -G "MinGW Makefiles" -DSERTOS_PORT=mingw64
cmake --build build

# Build the optional instrumented variant
cmake --build build --target sertos_kernel_instrumented
```

The instrumented target creates
`lib/mingw64/libsertos_mingw64_instrumented.a` with
`-finstrument-functions` applied to the SerTOS API translation units under
`src/`. Port and module dependency translation units remain uninstrumented.
Header-defined inline helpers used by an instrumented API translation unit are
instrumented unless marked `__attribute__((no_instrument_function))`; the
internal scheduler, statistics, timer, and dependency helpers used by this
target carry that attribute so the profile focuses on API calls.
Instrumented functions call `__cyg_profile_func_enter()` and
`__cyg_profile_func_exit()`, which the application must provide. Mark those
callbacks with the compiler's `no_instrument_function` attribute to prevent
recursive instrumentation. The regular `sertos_kernel` target remains
uninstrumented and is built by default. Application translation units need
their own instrumentation compile option if they should also be profiled.

---

## Quality Metrics & Testing

All unit tests reside exclusively in the centralized [`test_bench`](../test_bench/) repository conforming to workspace guidelines. Automated gates enforce:
- Cyclomatic Complexity: $\le 10$ (CCN)
- Function Lines of Code: $\le 75$ (NLOC)
- Parameter Count: $\le 5$ (Params)
- 100% Pass Rate across 14 test suites via Unity + gcov.

---

## License

MIT License. Copyright (c) 2026 Prynce Seitenfus.