# Option Signal+B/F: Dispatch Tracer with Signal-Triggered Data Channel

## Overview

This design uses a **real-time signal** (`SIGRTMIN+n`) as an instant notification mechanism combined with a **data channel** (Option B or F) for configuration. The signal solves the one limitation shared by all other options: **notification latency**. Without signals, the library must poll a flag on every intercepted call (which it already does). With signals, a background thread can block-wait with zero CPU cost and be woken instantly when the controller wants to change configuration.

This is not a standalone option — it's an **enhancement** layered on top of B (mmap file) or F/F+memfd (Unix socket) to add instant, zero-CPU-cost notification for configuration changes.

## Why Signals?

In Options B, F, and F+memfd, the hot-path already checks `tracing_enabled` on every call. So for the enable/disable toggle, signals add no value — the flag is checked every ~100 ns anyway.

Signals become valuable when:

1. **The library needs to do background work on config change** — e.g., allocate new ring buffers, open output files, re-discover functions, apply new filter patterns. This work should not happen on the hot path.
2. **Lazy initialization** — The library defers all tracing setup until the first attach signal, reducing startup overhead to near zero.
3. **Graceful shutdown** — The controller signals "flush and stop" and the library's background thread handles it immediately without waiting for the next intercepted call.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                   │
│                                                                  │
│  LD_PRELOAD=libmylib_dispatch_signal.so                         │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Main Thread (application code)                            │  │
│  │                                                            │  │
│  │  hot path:                                                 │  │
│  │    if (__atomic_load_n(&ctrl->tracing_enabled,             │  │
│  │                        __ATOMIC_ACQUIRE))                  │  │
│  │        trace(func_id, args);                               │  │
│  │    real_fn(args);                                          │  │
│  │                                                            │  │
│  │  // Same as B or F+memfd — signal does not affect this     │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Signal Handler (SIGRTMIN+7)                               │  │
│  │                                                            │  │
│  │  // Async-signal-safe: only sets a flag                    │  │
│  │  static uid_t cached_uid;  // set once in constructor     │  │
│  │  static void sig_handler(int sig, siginfo_t *info, ...) { │  │
│  │      // Defense-in-depth: kernel already checked UID on    │  │
│  │      // sigqueue(), but verify anyway using cached UID     │  │
│  │      // (getuid() is async-signal-safe but redundant)      │  │
│  │      if (info->si_uid != cached_uid) return;               │  │
│  │      __atomic_store_n(&config_changed, 1,                  │  │
│  │                       __ATOMIC_RELEASE);                   │  │
│  │      // Wake the background thread via write to pipe       │  │
│  │      write(wakeup_pipe[1], "W", 1);                        │  │
│  │  }                                                         │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Background Thread (config handler)                        │  │
│  │                                                            │  │
│  │  loop:                                                     │  │
│  │    poll(wakeup_pipe[0], POLLIN, -1)  // blocks, 0 CPU     │  │
│  │    read(wakeup_pipe[0], ...)         // drain the byte     │  │
│  │                                                            │  │
│  │    // Read new config from data channel (B or F+memfd)     │  │
│  │    new_version = ctrl->version;                            │  │
│  │    if (new_version > last_version) {                       │  │
│  │        apply_config(ctrl);                                 │  │
│  │        // Allocate ring buffers, open output files, etc.   │  │
│  │        __atomic_store_n(&ctrl->tracing_enabled, 1,         │  │
│  │                         __ATOMIC_RELEASE);                 │  │
│  │        last_version = new_version;                         │  │
│  │    }                                                       │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       │ SIGRTMIN+7 via sigqueue()
                       │ + data channel (B: mmap file, or F+memfd)
                       │
┌──────────────────────▼──────────────────────────────────────────┐
│                    Controller                                    │
│                                                                  │
│  // Step 1: Write config to data channel                        │
│  ctrl->output_format = OUTPUT_JSON;                             │
│  ctrl->ring_buffer_size = 4 * 1024 * 1024;                     │
│  memset(ctrl->func_enable_mask, 0xFF, ...);                     │
│  __atomic_store_n(&ctrl->version, v+1, __ATOMIC_RELEASE);      │
│                                                                  │
│  // Step 2: Signal the target to apply config                   │
│  union sigval val = { .sival_int = ctrl->version };             │
│  sigqueue(target_pid, SIGRTMIN + 7, val);                       │
│                                                                  │
│  // The library's bg thread wakes up within ~1-5 μs,           │
│  // reads the new config, and enables tracing.                  │
└─────────────────────────────────────────────────────────────────┘
```

## Signal Choice: Why Real-Time Signals?

| Signal Type | Queued? | Carries Data? | Conflict Risk |
|-------------|---------|---------------|---------------|
| SIGUSR1/SIGUSR2 | No (can be lost) | No | High (many tools use these) |
| SIGRTMIN+n | **Yes (queued)** | **Yes (`sigval`)** | Low (32 RT signals available) |

We use `SIGRTMIN + 7` (arbitrary choice in the RT range) to avoid conflicts with standard signal handlers. Real-time signals are queued, so multiple `sigqueue()` calls are not lost.

## Signal Security

The `kill()` / `sigqueue()` syscall enforces:

```
Sender's real or effective UID must match target's real or saved-set UID
  — OR sender has CAP_KILL
```

This is a **kernel-enforced check** — other users cannot send signals to your process. Additionally, the signal handler verifies `info->si_uid`:

```c
static uid_t cached_uid;  // initialized once in constructor

static void sig_handler(int sig, siginfo_t *info, void *ucontext) {
    // Defense-in-depth: kernel already enforced UID on sigqueue(), but
    // verify in handler using cached UID. (getuid() is async-signal-safe
    // per POSIX but the cache avoids any doubt and is marginally faster.)
    if (info->si_uid != cached_uid) return;

    __atomic_store_n(&config_changed, 1, __ATOMIC_RELEASE);
    // Wake background thread via self-pipe trick.
    // write() is async-signal-safe. If the pipe is full (O_NONBLOCK),
    // the byte is silently dropped — acceptable because the bg thread
    // uses poll() with a timeout as a fallback for lost wakeups.
    char c = 'W';
    write(wakeup_pipe[1], &c, 1);
}
```

## Combination Variants

### Signal + B (mmap file)

```
Controller writes config to /run/user/<uid>/dispatch/<pid>/ctrl
Controller calls sigqueue(pid, SIGRTMIN+7, version)
Library bg thread wakes, reads ctrl file, applies config
```

- Simple, no socket needed
- Config update latency: ~1-5 μs (signal delivery)
- No bidirectional communication

### Signal + F+memfd (Unix socket + anonymous shm)

```
Phase 1 (bootstrap): Socket connect + SO_PEERCRED + SCM_RIGHTS memfd
Phase 2 (runtime): Controller writes to memfd + sigqueue()
Phase 3 (queries): Controller uses socket for CMD_STATUS, CMD_FLUSH
```

- Strongest security (SO_PEERCRED + signal UID check)
- Fastest config propagation (memfd mmap + signal wake)
- Bidirectional via socket for queries
- Zero filesystem footprint

## Initialization: rocprofiler-register Methodology

Same as all options — see [Option B](OPTION_B_MMAP_FILE.md#initialization-rocprofiler-register-methodology) for the full registration flow. The signal setup happens during the tool's registration callback, not in a constructor.

## Components

### 1. Signal Registration (in the registration callback)

```c
static int wakeup_pipe[2];

/* Called during registration — NOT a constructor */
static void on_intercept_table_registration(
    const char* lib_name,
    void** api_table,
    size_t func_count)
{
    // Save originals and install shim wrappers (same as other options)
    memcpy(&orig_table, api_table, func_count * sizeof(void*));
    ((mylib_api_table_t*)api_table)->my_traced_function =
        shim_my_traced_function;

    // Cache UID for signal handler (avoids calling getuid() in handler)
    cached_uid = getuid();

    // Create self-pipe for signal→thread notification
    pipe2(wakeup_pipe, O_NONBLOCK | O_CLOEXEC);

    // Register real-time signal handler
    struct sigaction sa = {
        .sa_sigaction = sig_handler,
        .sa_flags = SA_SIGINFO | SA_RESTART,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 7, &sa, NULL);

    // Initialize data channel (B or F+memfd)
    init_data_channel();

    // Spawn background config thread (joinable for clean shutdown)
    pthread_create(&config_thread, NULL, config_loop, NULL);
}
```

### 2. Background Config Thread

```c
static void* config_loop(void* arg) {
    // Block all signals except our RT signal
    sigset_t mask;
    sigfillset(&mask);
    sigdelset(&mask, SIGRTMIN + 7);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    struct pollfd pfd = { .fd = wakeup_pipe[0], .events = POLLIN };
    uint32_t last_version = 0;

    while (1) {
        // Block with zero CPU cost until signaled.
        // Use 30s timeout as fallback in case a signal wakeup byte was
        // lost (pipe full when handler fired). This ensures eventual
        // convergence even under signal flood conditions.
        poll(&pfd, 1, 30000);

        // Drain pipe
        char buf[64];
        while (read(wakeup_pipe[0], buf, sizeof(buf)) > 0) {}

        // Check for new config
        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver > last_version) {
            // Heavy setup work (safe to do here, not on hot path):
            allocate_ring_buffers(ctrl->ring_buffer_size);
            open_output_file(ctrl->output_path);
            apply_filters(ctrl->filter_pattern, ctrl->exclude_pattern);

            // Enable tracing (hot path will see this)
            __atomic_store_n(&ctrl->tracing_enabled, 1, __ATOMIC_RELEASE);
            last_version = ver;
        }
    }
    return NULL;
}
```

### 3. Controller

```c
// After writing config to data channel:
union sigval val = { .sival_int = new_version };
int ret = sigqueue(target_pid, SIGRTMIN + 7, val);
if (ret < 0) {
    if (errno == ESRCH)
        fprintf(stderr, "Process %d not found\n", target_pid);
    else if (errno == EPERM)
        fprintf(stderr, "Permission denied "
                "(cannot signal PID %d)\n", target_pid);
}
```

## Security Analysis

| Property | Assessment |
|----------|------------|
| **Signal delivery auth** | Kernel-enforced UID check on `sigqueue()` — unforgeable |
| **Handler UID verification** | `info->si_uid` double-check in handler |
| **Data channel auth** | Depends on paired option (B: file mode, F+memfd: SO_PEERCRED) |
| **Signal number conflict** | Low risk — SIGRTMIN+7 is in the application-reserved range |
| **Signal flood** | RT signals are queued (up to `RLIMIT_SIGPENDING`); excess are dropped with `EAGAIN` |

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Signal handler execution | ~0.5-2 μs | Handler sets flag + writes 1 byte to pipe |
| Background thread wakeup | ~1-3 μs | `poll()` returns + pipe read |
| Config application | ~10-500 μs | Depends on work (buffer alloc, file open, etc.) |
| **Hot-path (noop)** | **~1-5 ns** | Same as B/F/F+memfd — signal doesn't affect hot path |
| **Hot-path (tracing)** | **~50-150 ns** | Same as B/F/F+memfd |
| **Config change notification** | **~1-5 μs** | Signal delivery + thread wakeup (vs polling interval) |

## Multi-Runtime Application (rocprofiler-sdk)

The signal approach is particularly valuable for multi-runtime scenarios:

```
Controller writes new config for HIP + HSA + RCCL to shared memory
Controller sends ONE sigqueue() to the target
Library bg thread wakes and:
  1. Reads HIP config → enables HIP tracing
  2. Reads HSA config → enables HSA tracing
  3. Reads RCCL config → enables RCCL tracing
  4. Sets master tracing_enabled = 1
All runtimes activated in a single wake cycle
```

This avoids per-runtime socket round-trips and ensures all runtimes activate simultaneously.

## File Layout

```
src/tools/dispatch_tracer_signal/
├── dispatch_signal.h            # Signal number, shared structs
├── dispatch_signal_wrapper.c    # LD_PRELOAD lib + signal handler + bg thread
├── dispatch_signal_trace.c      # Tracing logic
├── dispatch_signal_data.c       # Data channel integration (B or F+memfd)
└── dispatch_signal_controller.c # CLI controller
```

## Build Integration

```cmake
option(BUILD_DISPATCH_SIGNAL "Build dispatch table tracer (signal)" ON)

if(BUILD_DISPATCH_SIGNAL)
    add_library(mylib_dispatch_signal SHARED
        src/tools/dispatch_tracer_signal/dispatch_signal_wrapper.c
        src/tools/dispatch_tracer_signal/dispatch_signal_trace.c
        src/tools/dispatch_tracer_signal/dispatch_signal_data.c
    )
    target_link_libraries(mylib_dispatch_signal PRIVATE dl pthread)
    target_compile_options(mylib_dispatch_signal PRIVATE -O2 -fPIC)

    add_executable(dispatch_ctrl_signal
        src/tools/dispatch_tracer_signal/dispatch_signal_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Noop overhead:
LD_PRELOAD=build/lib/libmylib_dispatch_signal.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing:
# Terminal 1:
LD_PRELOAD=build/lib/libmylib_dispatch_signal.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/dispatch_ctrl_signal --pid $! enable
build/bin/dispatch_ctrl_signal --pid $! disable
```

## Limitations

1. **Signal handler context** — Only async-signal-safe functions allowed in the handler. Our handler only sets an atomic flag and writes 1 byte to a pipe (both safe).
2. **Signal conflict** — If the application also uses `SIGRTMIN+7`, our handler clobbers theirs. Mitigated by using a configurable signal number via environment variable (`DISPATCH_SIGNAL=SIGRTMIN+9`).
3. **Signal queuing limit** — `RLIMIT_SIGPENDING` (default ~128K) limits queued signals. Not a concern for config changes (rare events).
4. **Background thread** — Same overhead as Options F/F+memfd (~2 MB default stack). Thread must be joinable so `tool_finalize()` (called via `atexit()`) can shut it down cleanly.
5. **Lost wakeup recovery** — If signal handler fires when the self-pipe is full (`O_NONBLOCK`), the wakeup byte is silently dropped. The background thread uses `poll()` with a 30-second timeout as a fallback, so config changes are eventually applied even under signal flood.
6. **Signal number coordination** — The controller and library must use the same signal number. If both sides are configured independently via environment variables (`DISPATCH_SIGNAL`), a mismatch causes silent failure. The controller should verify via `/proc/<pid>/status` SigCgt mask that the expected signal is registered.
7. **fork() behavior** — After `fork()`, the signal handler is inherited but the background thread is not. A `pthread_atfork()` child handler should reset the signal handler to SIG_DFL, close the pipe, and set `finalize_status = 1` so the child's atexit handler skips cleanup.
8. **Complexity** — This is a composite option (signal + data channel), so it has more moving parts than B or F alone. The benefit is instant notification for heavy config changes.
9. **Overhead estimates are pre-implementation** — All timing figures should be validated with benchmarks after implementation.

## When to Use This Option

- **Use Signal+B** when you want the simplest possible approach with instant notification and no socket overhead.
- **Use Signal+F+memfd** when you need the strongest security (SO_PEERCRED) plus instant notification plus zero filesystem footprint.
- **Skip signals** (use B or F+memfd alone) when the hot-path polling is sufficient and you don't need background config setup.
