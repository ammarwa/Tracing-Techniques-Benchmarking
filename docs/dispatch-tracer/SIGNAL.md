# signal: Dispatch Tracer with Signal-Triggered Data Channel

## Overview

This design uses a **real-time signal** (`SIGRTMIN+n`) as an instant notification mechanism combined with a **data channel** (mmap or memfd) for configuration. The signal solves the one limitation shared by all other channels: **notification latency**. Without signals, the library must poll a flag on every intercepted call (which it already does). With signals, a background thread can block-wait with zero CPU cost and be woken instantly when the controller wants to change configuration.

This is not a standalone channel — it's an **enhancement** layered on top of mmap (file) or memfd (socket + anonymous shm) to add instant, zero-CPU-cost notification for configuration changes.

> **Precise reading of "preloaded":** `LD_PRELOAD=librocp_stub_signal.so` — only the stub. rocprofiler-register is already a `DT_NEEDED` dependency of HIP/HSA/OpenMP/RCCL (auto-loaded); rocprofiler-sdk is neither preloaded nor linked and is only `dlopen`'d at attach. OMPT is handled via a silent `ompt_start_tool` in the stub. See [CONTROL_CHANNEL_SURVEY.md § What Exactly Gets LD_PRELOAD'd](CONTROL_CHANNEL_SURVEY.md#what-exactly-gets-ld_preloadd--and-what-does-not) and [§ OpenMP / OMPT](CONTROL_CHANNEL_SURVEY.md#openmp--ompt--a-different-registration-path).

## Why Signals?

Under the late-load architecture (see [mmap](MMAP.md#what-changes-minimal--late-load-architecture)), **before any controller attaches, no wrappers exist** — rocprofiler-sdk is not loaded, the dispatch tables still point at original function pointers, and the hot path cost is **0 ns**. There is no `populate_contexts()` check happening per call yet, because the SDK has not been configured.

Signals become valuable precisely because of this architecture: the first attach (`CMD_CONFIGURE`) triggers heavy one-shot work — `dlopen("librocprofiler-sdk-tool.so")`, symbol resolution, `rocprofiler_force_configure()`, runtime API-table re-propagation, and `update_table()` wrapper installation. This work typically takes **~5-50 ms** and must happen off the hot path on a background thread. A sleeping `poll()`-based background thread costs 0 CPU; a real-time signal wakes it within ~1-5 μs of the controller's `sigqueue()`, so attach latency is dominated by the unavoidable dlopen work, not by polling interval.

Signals are also valuable for:

1. **Heavy post-attach reconfigure** — allocating new ring buffers, opening output files, applying new filter patterns on `CMD_RECONFIGURE`. Work happens on the background thread, not in the signal handler.
2. **Graceful shutdown** — the controller signals "flush and stop" and the background thread handles it immediately without waiting for a polling tick.
3. **Instant activate/deactivate** — once the SDK is loaded, `CMD_ACTIVATE` / `CMD_DEACTIVATE` map to `rocprofiler_start_context()` / `rocprofiler_stop_context()` with no polling latency.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                   │
│                                                                  │
│  Process start (no controller attached):                         │
│                                                                  │
│  HIP/HSA/RCCL runtimes load → link rocprofiler-register          │
│  Stub library preloaded via LD_PRELOAD                           │
│    (NO rocprofiler_configure symbol exported)                    │
│  Stub constructor:                                               │
│    create pipe2(wakeup_pipe, O_NONBLOCK|O_CLOEXEC)               │
│    register sigaction(SIGRTMIN+7, sig_handler)                   │
│    init paired data channel (mmap file or memfd)                 │
│    spawn background thread (poll-based, 0 CPU idle)              │
│                                                                  │
│  Runtime calls rocprofiler_register_library_api_table(...)       │
│    rocprofiler-register scans for rocprofiler_configure          │
│      → not found (only stub is loaded) → does NOT load SDK       │
│  Original function pointers stay in dispatch tables              │
│  Hot path: 0 ns (no wrappers, no SDK code)                       │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Signal Handler (SIGRTMIN+7) — registered in stub ctor     │  │
│  │                                                            │  │
│  │  // Async-signal-safe: ONLY writes to the wakeup pipe.     │  │
│  │  // No dlopen, no SDK calls, no malloc.                    │  │
│  │  static uid_t cached_uid;  // set in stub constructor      │  │
│  │  static void sig_handler(int sig, siginfo_t *info, ...) {  │  │
│  │      if (info->si_uid != cached_uid) return;               │  │
│  │      char c = 'W';                                         │  │
│  │      write(wakeup_pipe[1], &c, 1);  // 1 byte, non-block   │  │
│  │  }                                                         │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Background Thread (command dispatcher)                    │  │
│  │                                                            │  │
│  │  loop:                                                     │  │
│  │    poll(wakeup_pipe[0], POLLIN, 30000)  // 0 CPU idle      │  │
│  │    drain wakeup pipe                                       │  │
│  │                                                            │  │
│  │    // Read command from paired data channel                │  │
│  │    ver = atomic_load(&ctrl->version)                       │  │
│  │    cmd = atomic_load(&ctrl->command)                       │  │
│  │    switch (cmd) {                                          │  │
│  │      case CMD_CONFIGURE:                                   │  │
│  │        // First attach: heavy work ~5-50 ms                │  │
│  │        if (!sdk_loaded) load_sdk_and_configure();          │  │
│  │        else apply_runtime_filter(&ctrl->config);           │  │
│  │      case CMD_ACTIVATE:                                    │  │
│  │        p_start_context(saved_ctx);                         │  │
│  │      case CMD_DEACTIVATE:                                  │  │
│  │        p_stop_context(saved_ctx);                          │  │
│  │      case CMD_RECONFIGURE:                                 │  │
│  │        apply_runtime_filter(&ctrl->config); // atomic upd  │  │
│  │      case CMD_STATUS:                                      │  │
│  │        write stats back into ctrl                          │  │
│  │    }                                                       │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────┬──────────────────────────────────────────┘
                       │
                       │ SIGRTMIN+7 via sigqueue()
                       │ + paired data channel (mmap file, or memfd)
                       │
┌──────────────────────▼──────────────────────────────────────────┐
│                    Controller                                    │
│                                                                  │
│  // Step 1: Write config + command to data channel              │
│  ctrl->config = { .enable_hip = 1, .enable_hsa = 1, ... };      │
│  atomic_store(&ctrl->command, CMD_CONFIGURE);                   │
│  atomic_store(&ctrl->version, v+1);                             │
│                                                                  │
│  // Step 2: Signal the target to apply immediately              │
│  union sigval val = { .sival_int = ctrl->version };             │
│  sigqueue(target_pid, SIGRTMIN + 7, val);                       │
│                                                                  │
│  // The stub's bg thread wakes within ~1-5 μs;                 │
│  // on CMD_CONFIGURE it dlopens the tool library and calls force_configure.          │
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

### Signal + mmap (file)

```
Controller writes config to /run/user/<uid>/dispatch/<pid>/ctrl
Controller calls sigqueue(pid, SIGRTMIN+7, version)
Library bg thread wakes, reads ctrl file, applies config
```

- Simple, no socket needed
- Config update latency: ~1-5 μs (signal delivery)
- No bidirectional communication

### Signal + memfd (Unix socket + anonymous shm)

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

Same as all options — uses the **late-load design** in [mmap](MMAP.md#what-changes-minimal--late-load-architecture): stub preloaded (no `rocprofiler_configure` symbol, 0 ns hot path), SDK `dlopen`'d at attach via `rocprofiler_force_configure()`.

The signal serves as an instant wake mechanism for the stub's background thread. When woken, the thread reads the paired data channel (mmap or memfd) and dispatches: `CMD_CONFIGURE` → dlopen tool library (brings rocprofiler-sdk via link dep) + force_configure (first attach only), `CMD_ACTIVATE` → `rocprofiler_start_context()`, `CMD_DEACTIVATE` → `rocprofiler_stop_context()`, `CMD_RECONFIGURE` → atomic update of the runtime filter. The signal handler itself only writes a wake byte to a pipe — all SDK/dlopen work happens in the background thread.

## Control Structure

The control struct, command enum, and `rocp_config_t` layout are identical to mmap. The canonical command enum lives in [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) and defines:

```c
enum rocp_ctrl_command {
    CMD_NONE        = 0,   // No pending command
    CMD_CONFIGURE   = 1,   // First attach: dlopen tool library (brings rocprofiler-sdk via link dep) + force_configure,
                           //   OR later: update runtime filter
    CMD_ACTIVATE    = 2,   // rocprofiler_start_context()
    CMD_DEACTIVATE  = 3,   // rocprofiler_stop_context()
    CMD_RECONFIGURE = 4,   // Atomic update of runtime filter only
    CMD_STATUS      = 5,   // Tool writes stats into ctrl
};
```

See mmap's [Control Structure section](MMAP.md#control-structure) for the full `rocp_ctrl_t` layout and field semantics.

## Components

### 1. Stub Library (`librocp_stub_signal.so` — preloaded, NO rocprofiler_configure symbol)

The stub is loaded via `LD_PRELOAD` at process start. It does NOT export `rocprofiler_configure`, so rocprofiler-register's symbol scan finds no tool and does NOT load rocprofiler-sdk. The stub registers the signal handler, sets up the paired data channel, and spawns the background thread — then goes idle until signaled.

```c
static int wakeup_pipe[2];
static uid_t cached_uid;
static rocp_ctrl_t* ctrl = NULL;                /* paired data channel */
static pthread_t bg_thread;
static rocp_config_t g_pending_config;          /* read by tool_initialize */
static rocprofiler_context_id_t saved_ctx;
static void* sdk_handle = NULL;

/* Exported accessor — tool calls this after being dlopen'd at attach time.
 * Same pattern as mmap to avoid extern cross-DSO globals. */
typedef struct {
    rocp_ctrl_t* ctrl;
    rocp_config_t* pending_config;
    rocprofiler_context_id_t* saved_ctx;
} rocp_stub_state_t;

__attribute__((visibility("default")))
const rocp_stub_state_t* rocp_stub_get_state(void) {
    static rocp_stub_state_t state;
    state.ctrl = ctrl;
    state.pending_config = &g_pending_config;
    state.saved_ctx = &saved_ctx;
    return &state;
}

/* Async-signal-safe: only writes 1 byte to the wakeup pipe. No dlopen,
 * no SDK calls, no malloc, no stdio. All heavy work happens in the bg
 * thread which wakes from poll(). */
static void sig_handler(int sig, siginfo_t* info, void* ucontext) {
    if (info->si_uid != cached_uid) return;
    char c = 'W';
    (void)write(wakeup_pipe[1], &c, 1);  /* EAGAIN is fine — poll timeout recovers */
}

/* Loaded at process start via LD_PRELOAD. Registers signal handler,
 * opens the paired data channel, spawns the bg thread. Does NOT load
 * rocprofiler-sdk. */
__attribute__((constructor))
static void stub_init(void) {
    cached_uid = getuid();

    /* Self-pipe for signal → bg-thread wakeup */
    pipe2(wakeup_pipe, O_NONBLOCK | O_CLOEXEC);

    /* Register RT signal handler BEFORE spawning the bg thread so
     * early signals are not lost. */
    struct sigaction sa = {
        .sa_sigaction = sig_handler,
        .sa_flags     = SA_SIGINFO | SA_RESTART,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 7, &sa, NULL);

    /* Set up paired data channel (mmap file or memfd — same as mmap/memfd) */
    init_data_channel(&ctrl);

    /* Spawn bg thread (joinable for clean shutdown) */
    pthread_create(&bg_thread, NULL, control_poll_loop, NULL);
}
```

### 2. Background Thread (signal-woken dispatcher, dlopens the tool library on first CMD_CONFIGURE)

The bg thread mirrors mmap's `control_poll_loop` — same command dispatch, same `load_sdk_and_configure()` sequence — but blocks on `poll(wakeup_pipe)` instead of busy-polling a version counter. The signal wakes it within ~1-5 μs.

```c
/* On first CMD_CONFIGURE: dlopen rocprofiler-sdk-tool, resolve symbols
 * via RTLD_DEFAULT, call force_configure. Identical to mmap. */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t);
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t);
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t);

static void load_sdk_and_configure(void) {
    /* RTLD_GLOBAL is required so the tool's rocprofiler_configure is
     * visible to the SDK's dlsym(RTLD_DEFAULT, ...) scan. */
    sdk_handle = dlopen("librocprofiler-sdk-tool.so", RTLD_NOW | RTLD_GLOBAL);
    if (!sdk_handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return; }

    typedef rocprofiler_tool_configure_result_t* (*configure_fn_t)(
        uint32_t, const char*, uint32_t, rocprofiler_client_id_t*);
    configure_fn_t tool_configure = dlsym(RTLD_DEFAULT, "rocprofiler_configure");
    p_force_configure = dlsym(RTLD_DEFAULT, "rocprofiler_force_configure");
    p_start_context   = dlsym(RTLD_DEFAULT, "rocprofiler_start_context");
    p_stop_context    = dlsym(RTLD_DEFAULT, "rocprofiler_stop_context");

    if (!p_force_configure || !tool_configure) return;
    p_force_configure(tool_configure);
    /* tool_initialize runs here via SDK — reads g_pending_config via
     * rocp_stub_get_state(), registers domains, starts context. */
}

static _Atomic bool sdk_loaded = false;
static _Atomic int  shutdown_flag = 0;

static void* control_poll_loop(void* arg) {
    /* Block all signals except our RT signal on this thread — the
     * main thread handles SIGRTMIN+7 delivery; the bg thread just
     * wakes from poll(). */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    struct pollfd pfd = { .fd = wakeup_pipe[0], .events = POLLIN };
    uint32_t last_version = 0;

    while (!__atomic_load_n(&shutdown_flag, __ATOMIC_ACQUIRE)) {
        /* 30s timeout is a fallback for dropped wakeup bytes
         * (pipe full at signal time). */
        poll(&pfd, 1, 30000);

        /* Drain pipe */
        char buf[64];
        while (read(wakeup_pipe[0], buf, sizeof(buf)) > 0) {}

        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver == last_version) continue;

        uint32_t cmd = __atomic_load_n(&ctrl->command, __ATOMIC_ACQUIRE);
        switch (cmd) {
        case CMD_CONFIGURE: {
            memcpy(&g_pending_config, &ctrl->config, sizeof(g_pending_config));
            bool expected = false;
            if (__atomic_compare_exchange_n(&sdk_loaded, &expected, true,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                load_sdk_and_configure();  /* heavy: ~5-50 ms */
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
            } else {
                apply_runtime_filter(&ctrl->config);
            }
            break;
        }
        case CMD_ACTIVATE:
            if (__atomic_load_n(&sdk_loaded, __ATOMIC_ACQUIRE) && p_start_context) {
                p_start_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
            }
            break;
        case CMD_DEACTIVATE:
            if (__atomic_load_n(&sdk_loaded, __ATOMIC_ACQUIRE) && p_stop_context) {
                p_stop_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 0, __ATOMIC_RELEASE);
            }
            break;
        case CMD_RECONFIGURE:
            apply_runtime_filter(&ctrl->config);
            break;
        case CMD_STATUS:
            /* Statistics are already maintained by the callbacks via
             * atomic counters in ctrl — no action needed here. */
            break;
        }
        last_version = ver;
    }
    return NULL;
}
```

### 3. Tool Library (`librocprofiler-sdk-tool.so` — dlopen'd at attach)

Identical to [mmap's tool library](MMAP.md#3-tool-library-librocprofiler-sdk-toolso--dlopend-at-attach): exports `rocprofiler_configure`, its `tool_initialize` calls `rocp_stub_get_state()` to retrieve the pending config, registers the controller-selected domains, and calls `rocprofiler_start_context()`.

### 4. Controller

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
| **Data channel auth** | Depends on paired option (B: file mode, memfd: SO_PEERCRED) |
| **Signal number conflict** | Low risk — SIGRTMIN+7 is in the application-reserved range |
| **Signal flood** | RT signals are queued (up to `RLIMIT_SIGPENDING`); excess are dropped with `EAGAIN` |

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Stub init (constructor) | ~10-50 μs | pipe2 + sigaction + mmap/memfd + pthread_create. No SDK loaded. |
| **Hot-path before any attach** | **0 ns** | rocprofiler-sdk not loaded, no wrappers installed, original function pointers in dispatch tables |
| Signal handler execution | ~0.5-2 μs | Handler writes 1 byte to wakeup pipe (async-signal-safe only) |
| Background thread wakeup | ~1-3 μs | `poll()` returns + drain pipe |
| Controller attach + first CMD_CONFIGURE | ~5-50 ms | dlopen rocprofiler-sdk-tool (brings rocprofiler-sdk as link dep) + force_configure + propagation + update_table. Runs on bg thread, NOT in signal handler. |
| **Hot-path (active, callback emits)** | **~50-200 ns** | `populate_contexts()` + enter callbacks + original call + exit callbacks + buffer emplace |
| **Hot-path (active, runtime filter rejects)** | **~30-50 ns** | `populate_contexts()` + callback fires + atomic load of filter mask + return |
| Reconfigure (CMD_RECONFIGURE) | ~1 μs + signal wake | Atomic stores to runtime filter — effect immediate on next event |
| Activate / Deactivate (after first attach) | ~1-5 μs | Signal wake + bg thread calls start/stop_context |
| Stub cleanup | ~5 μs | munmap + close pipe + unlink |

**Key property**: when no controller ever attaches, the application has **0 ns hot-path overhead** because rocprofiler-sdk is never loaded — only the tiny stub library is in the address space, its signal handler is dormant, and the bg thread sleeps in `poll()` with 0 CPU cost.

## Multi-Runtime Application (rocprofiler-sdk)

The signal approach is particularly valuable for multi-runtime scenarios:

```
Controller writes CMD_CONFIGURE with config for HIP + HSA + RCCL
Controller sends ONE sigqueue() to the target
Stub bg thread wakes and, on first attach:
  1. dlopens librocprofiler-sdk-tool.so
  2. Calls rocprofiler_force_configure(tool_configure)
  3. tool_initialize reads pending_config via rocp_stub_get_state()
  4. Registers HIP + HSA + RCCL callback tracing services on one context
  5. Calls rocprofiler_start_context(ctx)
All runtimes activated in a single wake cycle, one context for all
```

This avoids per-runtime socket round-trips and ensures all runtimes activate simultaneously.

## File Layout

```
src/tools/rocprofiler_tool_signal/
├── rocp_signal.h              # Signal number, rocp_ctrl_t (same as mmap)
├── rocp_stub_signal.c         # Stub library: sig handler + bg thread (preloaded, no rocprofiler_configure)
├── rocp_signal_tool.c         # SDK tool library (dlopen'd at attach, exports rocprofiler_configure)
├── rocp_signal_data.c         # Paired data-channel integration (B or memfd)
└── rocp_signal_controller.c   # CLI controller
```

## Build Integration (CMakeLists.txt additions)

```cmake
option(BUILD_ROCP_TOOL_SIGNAL "Build rocprofiler tool with signal control channel" ON)

if(BUILD_ROCP_TOOL_SIGNAL)
    # Stub library — preloaded via LD_PRELOAD, no rocprofiler-sdk dependency.
    # Registers RT signal handler, opens paired data channel, spawns bg thread.
    add_library(rocp_stub_signal SHARED
        src/tools/rocprofiler_tool_signal/rocp_stub_signal.c
        src/tools/rocprofiler_tool_signal/rocp_signal_data.c
    )
    target_link_libraries(rocp_stub_signal PRIVATE pthread dl)
    target_compile_options(rocp_stub_signal PRIVATE -O2 -fPIC)

    # SDK tool library — dlopen'd at attach time by the stub's bg thread.
    # Exports rocprofiler_configure so rocprofiler-sdk discovers it.
    add_library(rocprofiler_tool_signal SHARED
        src/tools/rocprofiler_tool_signal/rocp_signal_tool.c
    )
    target_link_libraries(rocprofiler_tool_signal PRIVATE rocprofiler-sdk::rocprofiler-sdk)
    target_compile_options(rocprofiler_tool_signal PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_signal
        src/tools/rocprofiler_tool_signal/rocp_signal_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Stub preloaded, no SDK loaded — 0 ns hot-path overhead:
LD_PRELOAD=build/lib/librocp_stub_signal.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# Late attach with full configuration:
# Terminal 1:
LD_PRELOAD=build/lib/librocp_stub_signal.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/rocp_ctrl_signal --pid $! configure --hip --hsa --output json
build/bin/rocp_ctrl_signal --pid $! deactivate
```

## Limitations

1. **Signal handler context** — Only async-signal-safe functions allowed in the handler. Our handler only sets an atomic flag and writes 1 byte to a pipe (both safe).
2. **Signal conflict** — If the application also uses `SIGRTMIN+7`, our handler clobbers theirs. Mitigated by using a configurable signal number via environment variable (`DISPATCH_SIGNAL=SIGRTMIN+9`).
3. **Signal queuing limit** — `RLIMIT_SIGPENDING` (default ~128K) limits queued signals. Not a concern for config changes (rare events).
4. **Background thread** — Same overhead as socket/memfd (~2 MB default stack). Thread must be joinable so `tool_finalize()` (called via `atexit()`) can shut it down cleanly.
5. **Lost wakeup recovery** — If signal handler fires when the self-pipe is full (`O_NONBLOCK`), the wakeup byte is silently dropped. The background thread uses `poll()` with a 30-second timeout as a fallback, so config changes are eventually applied even under signal flood.
6. **Signal number coordination** — The controller and library must use the same signal number. If both sides are configured independently via environment variables (`DISPATCH_SIGNAL`), a mismatch causes silent failure. The controller should verify via `/proc/<pid>/status` SigCgt mask that the expected signal is registered.
7. **fork() behavior** — After `fork()`, the signal handler is inherited but the background thread is not. A `pthread_atfork()` child handler should reset the signal handler to SIG_DFL, close the pipe, and set `finalize_status = 1` so the child's atexit handler skips cleanup.
8. **Complexity** — This is a composite option (signal + data channel), so it has more moving parts than B or F alone. The benefit is instant notification for heavy config changes.
9. **Overhead estimates are pre-implementation** — All timing figures should be validated with benchmarks after implementation.

## When to Use This Channel

- **Use signal** when you want the simplest possible approach with instant notification and no socket overhead.
- **Use Signal+memfd** when you need the strongest security (SO_PEERCRED) plus instant notification plus zero filesystem footprint.
- **Skip signals** (use B or memfd alone) when the hot-path polling is sufficient and you don't need background config setup.
