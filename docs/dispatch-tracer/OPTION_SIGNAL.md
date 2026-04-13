# Option Signal+B/F: Dispatch Tracer with Signal-Triggered Data Channel

## Overview

This design uses a **real-time signal** (`SIGRTMIN+n`) as an instant notification mechanism combined with a **data channel** (Option B or F) for configuration. The signal solves the one limitation shared by all other options: **notification latency**. Without signals, the library must poll a flag on every intercepted call (which it already does). With signals, a background thread can block-wait with zero CPU cost and be woken instantly when the controller wants to change configuration.

This is not a standalone option — it's an **enhancement** layered on top of B (mmap file) or F/F+memfd (Unix socket) to add instant, zero-CPU-cost notification for configuration changes.

## Why Signals?

In Options B, F, and F+memfd, the existing `populate_contexts()` already checks active contexts on every call (~10-20 ns). So for the enable/disable toggle, signals add no value — the context check happens every call anyway.

Signals become valuable when:

1. **The library needs to do background work on config change** — e.g., allocate new ring buffers, open output files, re-discover functions, apply new filter patterns. This work should not happen on the hot path.
2. **Lazy initialization** — The library defers all tracing setup until the first attach signal, reducing startup overhead to near zero.
3. **Graceful shutdown** — The controller signals "flush and stop" and the library's background thread handles it immediately without waiting for the next intercepted call.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                   │
│                                                                  │
│  Tool loaded via ROCP_TOOL_LIBRARIES                            │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Main Thread (application code)                            │  │
│  │                                                            │  │
│  │  hot path (existing, NO CHANGES):                          │  │
│  │    populate_contexts(domain, op, cb_ctxs, buf_ctxs);      │  │
│  │    if (empty) → call original;  // noop ~10-20 ns         │  │
│  │    else → callbacks + original + buffers                   │  │
│  │                                                            │  │
│  │  // Same as B or F+memfd — signal does not affect this     │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Signal Handler (SIGRTMIN+7)                               │  │
│  │                                                            │  │
│  │  // Async-signal-safe: only sets a flag                    │  │
│  │  static uid_t cached_uid;  // set in tool_initialize       │  │
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
│  │        // Heavy setup if needed, then activate context     │  │
│  │        rocprofiler_start_context(saved_ctx);               │  │
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
│  // Step 1: Write command to data channel (mmap or memfd)       │
│  ctrl->command = CMD_ACTIVATE;                                  │
│  __atomic_store_n(&ctrl->version, v+1, __ATOMIC_RELEASE);      │
│                                                                  │
│  // Step 2: Signal the target to apply command immediately      │
│  union sigval val = { .sival_int = ctrl->version };             │
│  sigqueue(target_pid, SIGRTMIN + 7, val);                       │
│                                                                  │
│  // The tool's bg thread wakes within ~1-5 μs,                 │
│  // calls rocprofiler_start_context(ctx).                       │
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

## Integration with rocprofiler-sdk

Same as all options — the tool uses standard rocprofiler-sdk APIs including `rocprofiler_force_configure()` for **late configuration**. See [Option B](OPTION_B_MMAP_FILE.md#what-changes-minimal) for the full late-configuration design.

The signal serves as an instant wake mechanism for the background thread. When woken, the thread reads the paired data channel (mmap or memfd) and dispatches the command: `CMD_CONFIGURE` → `rocprofiler_force_configure()`, `CMD_ACTIVATE` → `rocprofiler_start_context()`, `CMD_DEACTIVATE` → `rocprofiler_stop_context()`. The signal handler itself only writes a wake byte to a pipe — the heavy work (force_configure, propagation, etc.) happens in the background thread, not the signal context.

## Components

### 1. Signal Registration (in the registration callback)

```c
static int wakeup_pipe[2];

/* Called during tool_initialize — context and domains already registered
 * by rocprofiler_configure. This only sets up the signal control channel. */
static void setup_signal_control(rocprofiler_context_id_t ctx) {
    saved_ctx = ctx;

    // Cache UID for signal handler
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

        // Check for new command
        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver > last_version) {
            uint32_t cmd = __atomic_load_n(&ctrl->command, __ATOMIC_ACQUIRE);
            if (cmd == CMD_ACTIVATE && !context_active) {
                rocprofiler_start_context(saved_ctx);  // EXISTING API
                context_active = true;
            } else if (cmd == CMD_DEACTIVATE && context_active) {
                rocprofiler_stop_context(saved_ctx);   // EXISTING API
                context_active = false;
            }
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
| **Hot-path (noop)** | **~10-20 ns** | Existing `populate_contexts()` — context inactive |
| **Hot-path (tracing)** | **~50-200 ns** | `populate_contexts()` + callbacks + buffer emplace |
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
  4. Calls rocprofiler_start_context(ctx)
All runtimes activated in a single wake cycle
```

This avoids per-runtime socket round-trips and ensures all runtimes activate simultaneously.

## File Layout

```
src/tools/rocprofiler_tool_signal/
├── rocp_signal.h              # Signal number, rocp_ctrl_t (same as Option B)
├── rocp_signal_tool.c         # rocprofiler tool library (configure + signal setup)
├── rocp_signal_data.c         # Data channel integration (B or F+memfd)
└── rocp_signal_controller.c   # CLI controller
```

## Build Integration

```cmake
option(BUILD_ROCP_TOOL_SIGNAL "Build rocprofiler tool with signal control channel" ON)

if(BUILD_ROCP_TOOL_SIGNAL)
    add_library(rocprofiler_tool_signal SHARED
        src/tools/rocprofiler_tool_signal/rocp_signal_tool.c
        src/tools/rocprofiler_tool_signal/rocp_signal_data.c
    )
    target_link_libraries(rocprofiler_tool_signal PRIVATE pthread)
    target_compile_options(rocprofiler_tool_signal PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_signal
        src/tools/rocprofiler_tool_signal/rocp_signal_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Noop overhead (tool loaded, context not activated):
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_signal.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing:
# Terminal 1:
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_signal.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/rocp_ctrl_signal --pid $! activate
build/bin/rocp_ctrl_signal --pid $! deactivate
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
