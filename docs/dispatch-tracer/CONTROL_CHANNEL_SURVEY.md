# Dispatch Tracer Control Channel Survey

## Overview

This document surveys all control channel mechanisms considered for a new tracing technique: the **Dispatch Table Tracer**. This tracer uses LD_PRELOAD to intercept API calls via a function dispatch table that starts as a **noop passthrough** and only activates tracing when an external controller process attaches at runtime with configuration.

The core architecture is inspired by [rocprofiler-sdk](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-sdk), which uses intercept tables that start as passthrough and are swapped to tracing wrappers when a profiling tool attaches. The goal is to bring this pattern to the Tracing-Techniques-Benchmarking framework and evaluate it against the existing LTTng, eBPF, and bpftime techniques.

The key design question is: **how does the external controller tell the LD_PRELOAD library to start/stop tracing and with what configuration?** This is the "control channel."

## Requirements

The control channel must satisfy:

1. **No root/sudo** — The user should not need elevated privileges to trace their own process
2. **No capabilities** — No `CAP_BPF`, `CAP_SYS_PTRACE`, `CAP_PERFMON`, etc.
3. **No specific kernel version** — Must work on any modern Linux kernel
4. **Cross-user security** — Other users on the system must not be able to interfere with, read, or modify another user's profiling session
5. **Minimal hot-path overhead** — The "is tracing enabled?" check runs on every intercepted API call and must be as close to zero-cost as possible
6. **Configuration richness** — Must support per-function enable/disable, filter patterns, output format, buffer sizes
7. **Multi-runtime applicability** — Must scale to tracing HIP, HSA/ROCR, RCCL, OpenMP, rocdecode, rocjpeg, and other GPU API libraries simultaneously

## Approaches Evaluated

We evaluated 13 control channel mechanisms across 6 categories.

### Category 1: Shared Memory

| ID | Mechanism | Hot-Path Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| A | POSIX shared memory (`shm_open` + `mmap`) | ~1-5 ns | Nothing | File mode `0600` on `/dev/shm/` |
| B | mmap on regular file | ~1-5 ns | Nothing | Dir `0700` + file `0600` |

**How they work:** The LD_PRELOAD library creates a shared memory region at initialization. The controller opens the same region and writes configuration (enable/disable flags, filter masks, output settings). The library reads the control flags via atomic loads on every intercepted call. No syscall on the hot path — pure memory access.

### Category 2: BPF-Based

| ID | Mechanism | Hot-Path Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| C | BPF array map + `BPF_F_MMAPABLE` | ~1-5 ns | `CAP_BPF`, kernel 5.5+ | Capability-gated |
| O | BPF map (syscall access, no mmap) | ~150-300 ns | `CAP_BPF` | Capability-gated |
| P | BPF ring buffer | ~10-50 ns | `CAP_BPF`, kernel 6.1+ for USER_RINGBUF | Capability-gated |
| Q | BPF token + mmapable map | ~1-5 ns | Kernel 6.9+ | Token-delegated |

**How they work:** BPF maps are kernel-managed shared data structures accessible from both userspace and BPF programs. A mmapable BPF array map (`BPF_F_MMAPABLE`) can be mmap'd by both the controller and the LD_PRELOAD library, giving the same ~1-5 ns atomic load performance as POSIX shm. The unique value is that the same map is simultaneously readable by kernel-side BPF programs, enabling hybrid kernel+userspace tracing controlled by a single flag. BPF ring buffers provide high-throughput streaming (good for trace data) but are unidirectional and message-based (poor for shared state).

### Category 3: Socket-Based

| ID | Mechanism | Hot-Path Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| F | Unix domain socket | ~1-5 ns (local atomic) | Nothing | `SO_PEERCRED` (kernel-verified effective UID/PID) |
| F+memfd | Unix socket + `memfd_create` | ~1-5 ns (mmap'd memfd) | Nothing | `SO_PEERCRED` + no filesystem entry |

**How they work:** The LD_PRELOAD library creates a listener socket (abstract namespace) and spawns a background thread. The controller connects, is authenticated via `SO_PEERCRED` (kernel-verified effective UID/GID at connect time, unforgeable), and sends commands. The library updates a process-local atomic variable that the hot path checks. The socket is only for the control plane — no socket I/O per intercepted call. The F+memfd variant adds `memfd_create` to create anonymous shared memory passed via `SCM_RIGHTS`, combining socket authentication with mmap-speed shared config access.

### Category 4: Direct Memory Access

| ID | Mechanism | Hot-Path Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| L | `/proc/<pid>/mem` direct write | ~1-5 ns (target side) | ptrace permissions | `ptrace_scope` + UID |
| M | `process_vm_writev` | ~1-5 ns (target side) | ptrace permissions | `ptrace_scope` + UID |

**How they work:** The LD_PRELOAD library exports known symbols (`__dispatch_ctrl`, `__dispatch_config`) in its `.data` section. The controller resolves their addresses by parsing `/proc/<pid>/maps` + ELF symbol offsets, then writes directly to the target's memory via `/proc/<pid>/mem` (pwrite) or `process_vm_writev` (scatter-gather syscall). The target process is completely passive — no setup, no threads, no IPC. This is conceptually how GDB modifies variables in a running process.

### Category 5: Signal-Based

| ID | Mechanism | Hot-Path Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| J | SIGUSR1/SIGUSR2 | ~1-5 ns (`sig_atomic_t`) | Nothing | `kill()` UID check |
| K | Real-time signal (SIGRTMIN+n) | ~1-5 ns | Nothing | `kill()` UID check |

**How they work:** The LD_PRELOAD library registers a signal handler that toggles a `volatile sig_atomic_t` flag. The controller sends `kill(target_pid, SIGUSR1)`. The kernel enforces that only processes with the same UID can send signals. Carries zero configuration data (SIGUSR1/2) or one `int` via `sigqueue()` (real-time signals). Useful only as a trigger, not a config channel.

### Category 6: Code Injection

| ID | Mechanism | Hot-Path Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| N | ptrace inject (rocprofiler-sdk approach) | ~50-200 ns | ptrace permissions, x86-64 | `ptrace_scope` + UID |

**How it works:** This is what rocprofiler-sdk uses today for `rocprofv3 --attach`. The controller uses `PTRACE_SEIZE` to attach to a pre-created safe thread in the target, then injects x86-64 assembly (`0f 05` syscall + `cc` INT3) to allocate memory, write environment data, and call `rocprofiler_register_attach()` — which triggers full profiler initialization including dlopen of tool libraries and intercept table updates. The target must have opted in by running with `ROCP_TOOL_ATTACH=1` (which creates the safe background thread). This is the most powerful approach (can execute arbitrary code in the target) but also the most complex (~1500 lines of ptrace/assembly code).

## Elimination Criteria

We applied two hard filters:

### Filter 1: No sudo / No capabilities / No specific kernel version

| Eliminated | Reason |
|------------|--------|
| **C** (BPF map + mmap) | Requires `CAP_BPF` on both controller and LD_PRELOAD library |
| **O** (BPF map syscall) | Requires `CAP_BPF`; also ~150-300 ns per-call syscall overhead is unacceptable |
| **P** (BPF ring buffer) | Requires `CAP_BPF`; kernel 6.1+ for `USER_RINGBUF`; wrong primitive for control state |
| **Q** (BPF token) | Requires kernel 6.9+ (not available on RHEL 8/9, Ubuntu 22.04) |
| **L** (`/proc/pid/mem`) | Requires ptrace permissions; on Ubuntu default (`ptrace_scope=1`) only parent processes can access |
| **M** (`process_vm_writev`) | Same ptrace permission requirements as L |
| **N** (ptrace inject) | Requires ptrace permissions; x86-64 only; target must opt in with background thread |

### Filter 2: Cross-user security (kernel-enforced)

| Eliminated | Reason |
|------------|--------|
| **A** (POSIX shm) | Predictable name in flat `/dev/shm/` namespace; vulnerable to race condition where a malicious user pre-creates the segment with their ownership before the target starts |
| **J/K** (Signals) | Secure (kernel UID check) but carry zero configuration data — cannot function as a standalone control channel |

### What Survived

| Option | Hot-Path | Auth Model | Config | Filesystem Footprint |
|--------|----------|------------|--------|---------------------|
| **B** (mmap regular file) | ~1-5 ns | Directory `0700` + file `0600` | Full struct via mmap | `/run/user/<uid>/...` |
| **F** (Unix domain socket) | ~1-5 ns (local atomic) | `SO_PEERCRED` (kernel-verified) | Full protocol | None (abstract namespace) |
| **F+memfd** (Socket + anonymous shm) | ~1-5 ns (mmap'd memfd) | `SO_PEERCRED` | Full struct + protocol | None whatsoever |
| **Signal + B or F** (Trigger + data channel) | ~1-5 ns | `kill()` UID check + paired channel | Via paired channel | Depends on pair |

## Detailed Comparison of Surviving Options

### Security Comparison

| Threat | B (mmap file) | F (Unix socket) | F+memfd | Signal+paired |
|--------|---------------|-----------------|---------|---------------|
| Other user reads config | Blocked (0700 dir) | Blocked (SO_PEERCRED) | Blocked (SO_PEERCRED) | Blocked (kill UID) |
| Other user modifies config | Blocked (0700 dir) | Blocked (SO_PEERCRED) | Blocked (SO_PEERCRED) | Blocked (kill UID) |
| Race / pre-creation attack | Protected (user-owned dir) | Not applicable (abstract ns) | Not applicable (no FS) | Depends on pair |
| Identity forging | No identity check | UID/GID unforgeable (kernel-verified at connect time) | UID/GID unforgeable | Unforgeable (kill UID check) |
| Stale artifacts on crash | Stale file in tmpfs | None (abstract socket) | None (anonymous) | Depends on pair |

### Overhead Comparison

| Phase | B | F | F+memfd | Signal+B |
|-------|---|---|---------|----------|
| Init (target side) | ~10 μs (open+mmap) | ~10 μs (socket+bind+listen+thread) | ~15 μs (socket+thread) | ~10 μs (sigaction+paired init) |
| Attach (controller) | ~5 μs (open+mmap) | ~5 μs (connect+SO_PEERCRED) | ~10 μs (connect+memfd+SCM_RIGHTS) | ~5 μs (paired attach) |
| Hot-path check | ~1-5 ns (atomic load) | ~1-5 ns (atomic load) | ~1-5 ns (atomic load) | ~1-5 ns (atomic load) |
| Config update | ~50-100 ns (cache line) | ~1-5 μs (socket send) | ~50-100 ns (cache line) | ~1-5 μs (signal + read) |

### Architecture Fit for rocprofiler-sdk

| Aspect | B | F | F+memfd | Signal+B |
|--------|---|---|---------|----------|
| Per-function enable (500+ HIP APIs) | Bitmask in mmap'd struct | Bitmask sent via protocol | Bitmask in mmap'd memfd | Bitmask in paired channel |
| Multi-runtime config | Directory hierarchy | Single protocol session | Single session + shared regions | Complex pairing |
| Bidirectional (status queries) | No (shared memory is state) | Yes (native) | Yes (socket) + fast state (memfd) | Limited |
| Graceful detach with flush | Write disable flag | Send DETACH command, get ACK | Send DETACH, get ACK | Signal + paired |
| Dynamic reconfiguration | Write new config struct | Send CONFIG command | Write to memfd | Signal + write |

## Special Considerations

### OpenMP OMPT: Late Attachment Is Not Supported

The OpenMP 5.0/5.1/5.2 spec defines OMPT tool initialization as a **one-shot event** during runtime startup. The runtime calls `ompt_start_tool()` exactly once during the first OpenMP construct or API call. If no tool is found at that point, OMPT is disabled for the lifetime of the process. There is no re-initialization path.

- `OMP_TOOL_LIBRARIES` must be set **before process launch**. The runtime reads it during initialization only.
- `OMP_TOOL=enabled|disabled` is similarly read once at init time.
- OpenMP 6.0 does not change this — OMPT remains init-time-only.

**Implication for the dispatch tracer**: For OpenMP tracing, the dispatch shim must be loaded before the OpenMP runtime initializes. This means either:
1. The shim is compiled into the runtime (build-time integration), or
2. `LD_PRELOAD` injects the shim before the runtime's constructor runs (viable since LD_PRELOAD constructors run before the application's `main()` and typically before OpenMP's lazy init).

The late-attach control channel (Options B/F/F+memfd) can still dynamically **enable/disable** OMPT-style tracing, but the shim code must already be resident in the process. The control channel toggles an atomic flag that the pre-loaded shim checks — this is consistent with our noop-by-default architecture.

For APIs entirely outside the team's control (e.g., a third-party library using OpenMP internally), the shim approach cannot intercept OMPT callbacks without the library's cooperation. In such cases, a **standardized annotation interface** (see below) is the fallback.

### Third-Party API Tracing: Plugin Interface

Not all APIs will be under the team's control. OpenMP, RCCL, rocdecode, rocjpeg, and future libraries may be maintained by other teams or external contributors. The dispatch tracer needs a mechanism for external parties to register their own tracing without modifying the core tracer.

Existing profiling tools handle this differently:

| Tool | Approach | How third parties participate |
|------|----------|------------------------------|
| **rocprofiler-sdk** | Compile-time code generation from API table YAML/CSV files. Adding a new API family means adding table definitions and regenerating. | Fork/PR — not a runtime plugin system |
| **Intel VTune (ITT)** | Annotation API: libraries call `__itt_task_begin()`/`__itt_task_end()` to mark regions. VTune collects events. | Library must instrument itself with ITT calls |
| **NVIDIA Nsight (NVTX)** | Annotation API: `nvtxRangePush()`/`nvtxRangePop()` with custom domains. Nsight collects alongside CUPTI. | Library must instrument itself with NVTX calls |

For the dispatch tracer, a **plugin-based approach** would combine both models:

```c
/* Plugin interface: third-party libraries register their own intercepts */
struct dispatch_plugin {
    const char *name;           // e.g., "rccl", "rocdecode"
    const char *version;

    /* Discovery: plugin resolves original function pointers */
    int (*register_intercepts)(struct intercept_table *table);

    /* Lifecycle */
    int (*init)(struct tracer_context *ctx);
    void (*fini)(void);

    /* Event output: plugin flushes events to shared ring buffer */
    void (*flush_events)(struct event_buffer *buf);
};

/* Registration: plugins call this at load time */
int dispatch_register_plugin(const struct dispatch_plugin *plugin);
```

This allows external teams to:
1. Provide wrapper functions for their API (dispatch-table interception)
2. Use the shared control channel and ring buffer infrastructure
3. Participate in the same enable/disable lifecycle as built-in APIs

### OpenTelemetry as Output Format

[OpenTelemetry](https://opentelemetry.io/) (OTel) does not have first-party instrumentation for HIP, CUDA, HSA, or ROCm. No OTel auto-instrumentation libraries exist for GPU API calls. AMD uses rocprofiler-sdk/roctracer; NVIDIA uses CUPTI/NVTX.

However, OTel is valuable as the **output and transport layer**, not the instrumentation layer:

- **Instrumentation**: The dispatch table shim intercepts API calls (faster and more precise than OTel auto-instrumentation).
- **Export**: Convert trace events to OTel spans using the [OTel C/C++ SDK](https://github.com/open-telemetry/opentelemetry-cpp). Each API call becomes a span with GPU-specific attributes (kernel name, stream, device, memory size).
- **Transport**: OTLP (gRPC or HTTP) to any OTel collector, then to Jaeger, Grafana Tempo, Zipkin, or any OTel-compatible backend.

This pattern — custom dispatch-table instrumentation feeding OTel export — provides the best of both worlds: low-overhead interception with industry-standard trace output. The output format choice (`OUTPUT_TEXT`, `OUTPUT_JSON`, `OUTPUT_PERFETTO`) in the control structure could be extended with `OUTPUT_OTLP` for OTel export.

### Cross-Platform Considerations

All four surviving control channel options (B, F, F+memfd, Signal) are **Linux-specific**:

- Abstract Unix sockets (Options F, F+memfd) are Linux-only (not macOS/BSD)
- `memfd_create` (Option F+memfd) is Linux-only
- `/run/user/<uid>/` (Option B) depends on systemd
- Real-time signals with `sigqueue` (Option Signal) are POSIX but behavior varies

For future cross-platform support, Option B could fall back to a platform-appropriate temp directory, and Option F could fall back to filesystem-namespace Unix sockets. The dispatch table mechanism itself (atomic load + function pointer check) is portable to any platform with C11 atomics.

## Recommendation

The four surviving options each have distinct strengths:

- **B** is the simplest — minimal code, no threads, no sockets. Best for straightforward enable/disable.
- **F** has the strongest authentication — kernel-verified identity on every connection. Best for security-sensitive deployments.
- **F+memfd** combines F's authentication with mmap-speed config access and zero filesystem footprint. Best overall for production.
- **Signal+B/F** adds instant notification without polling. Useful as an enhancement to B or F.

Each option is designed in detail in its own document:

- [Option B Design: mmap Regular File](OPTION_B_MMAP_FILE.md)
- [Option F Design: Unix Domain Socket](OPTION_F_UNIX_SOCKET.md)
- [Option F+memfd Design: Socket + Anonymous Shared Memory](OPTION_F_MEMFD.md)
- [Option Signal+B/F Design: Signal-Triggered Data Channel](OPTION_SIGNAL.md)
