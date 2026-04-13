# Dispatch Tracer Control Channel Survey

## Overview

This document surveys all control channel mechanisms considered for a new tracing technique: the **Dispatch Table Tracer**. This tracer intercepts API calls via a function dispatch table that starts as a **noop passthrough** and only activates tracing when an external controller process attaches at runtime with configuration.

The core architecture is inspired by [rocprofiler-sdk](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-sdk), which uses intercept tables that start as passthrough and are swapped to tracing wrappers when a profiling tool attaches. The goal is to bring this pattern to the Tracing-Techniques-Benchmarking framework and evaluate it against the existing LTTng, eBPF, and bpftime techniques.

### How rocprofiler-sdk's Dispatch Table Already Works

The existing rocprofiler-sdk already implements a dispatch table with a two-level noop fast-path. Understanding it precisely reveals what we can reuse and what the minimal change is.

**Existing flow (no changes needed):**

1. Runtime (HIP/HSA) calls `rocprofiler_set_api_table(name, version, instance, &tables, count)` during its init, passing its mutable function pointer table.
2. `copy_table()` saves the original function pointers into a static singleton.
3. `update_table()` iterates every `OpIdx` and checks `should_wrap_functor()` which queries all registered contexts. Only operations with interested contexts get wrappers; the rest keep original pointers (**Level 1 noop: zero overhead**).
4. Installed wrappers use `hip_api_impl<TableIdx, OpIdx>::functor()` which at call time calls `populate_contexts()` to check active contexts. If `callback_contexts` and `buffered_contexts` are both empty, it calls the original directly (**Level 2 noop: ~10-20 ns**).
5. When contexts ARE active, the wrapper fires enter callbacks, calls the original, fires exit callbacks, and emplaces buffer records.

**The noop overhead today** (wrapper installed but no active context): ~10-20 ns per call. `populate_contexts()` iterates active contexts (bitset check), finds none, returns.

### Minimal Change Strategy

The designs in this document minimize changes to rocprofiler-sdk:

1. **Reuse `copy_table`/`update_table`/`functor` machinery as-is.** No changes to wrapper code.
2. **Reuse `context`, `callback_tracing_service`, `buffer_tracing_service` as-is.** No new service types needed.
3. **Install wrappers for all operations at init time** (`should_wrap_functor` returns true for the tool's registered domains). Level 1 noop is bypassed but Level 2 noop still works.
4. **Add an external control channel** that activates/deactivates the tool's context at runtime. When active, `populate_contexts()` finds it; when inactive, wrappers fall through to originals.

The control channel's job is narrow: **toggle a `rocprofiler_context_id_t` between active and inactive.** Everything else (wrappers, callbacks, buffers, correlation IDs) is already implemented.

**Changes required to rocprofiler-sdk:**
- Control channel setup in the tool's `initialize` callback (shm/socket/memfd)
- Background thread or signal handler to receive commands and call `rocprofiler_start_context()` / `rocprofiler_stop_context()` (existing public APIs)
- Optionally: extend `rocprofiler_configure_attach()` to support new channels alongside ptrace

Note: No new SDK APIs are needed. The existing `rocprofiler_start_context()` / `rocprofiler_stop_context()` are sufficient — they are public APIs designed to be called from any thread.

**What the control channel does NOT need to do:**
- No custom dispatch table or function pointer management (rocprofiler-sdk already handles this)
- No double-buffered config struct (context activation/deactivation is atomic)
- No per-function bitmask in shared memory (context domain/operation masks handle this)

The key design question remains: **what IPC mechanism does the external controller use to toggle contexts?**

## Requirements

The control channel must satisfy:

1. **No root/sudo** — The user should not need elevated privileges to trace their own process
2. **No capabilities** — No `CAP_BPF`, `CAP_SYS_PTRACE`, `CAP_PERFMON`, etc.
3. **No specific kernel version** — Must work on any modern Linux kernel
4. **Cross-user security** — Other users on the system must not be able to interfere with, read, or modify another user's profiling session
5. **Minimal hot-path overhead** — The "is tracing enabled?" check runs on every intercepted API call and must be as close to zero-cost as possible
6. **Configuration richness** — Per-function enable/disable, filter patterns, output format, and buffer sizes are handled by existing `rocprofiler_configure_*` APIs at init time. The control channel must at minimum support activate/deactivate commands, and optionally carry additional runtime configuration
7. **Multi-runtime applicability** — Must scale to tracing HIP, HSA/ROCR, RCCL, OpenMP, rocdecode, rocjpeg, and other GPU API libraries simultaneously

## Approaches Evaluated

We evaluated 13 control channel mechanisms across 6 categories.

### Category 1: Shared Memory

The "Hot-Path Cost" column below shows the **raw IPC mechanism cost** (atomic load from mmap'd memory). In the rocprofiler-sdk integration, the actual hot-path overhead is ~10-20 ns from `populate_contexts()` — the IPC mechanism adds zero to the hot path since it only toggles context activation from a background thread.

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|----------------|----------|----------|
| A | POSIX shared memory (`shm_open` + `mmap`) | ~1-5 ns | Nothing | File mode `0600` on `/dev/shm/` |
| B | mmap on regular file | ~1-5 ns | Nothing | Dir `0700` + file `0600` |

**How they work:** The tool library creates a shared memory region during its `initialize` callback. The controller opens the same region and writes commands (activate/deactivate context). The tool's background thread reads the command and calls `rocprofiler_start_context()` / `rocprofiler_stop_context()`. No IPC on the hot path — the hot path is entirely within rocprofiler-sdk's existing `populate_contexts()` check.

### Category 2: BPF-Based

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| C | BPF array map + `BPF_F_MMAPABLE` | ~1-5 ns | `CAP_BPF`, kernel 5.5+ | Capability-gated |
| O | BPF map (syscall access, no mmap) | ~150-300 ns | `CAP_BPF` | Capability-gated |
| P | BPF ring buffer | ~10-50 ns | `CAP_BPF`, kernel 6.1+ for USER_RINGBUF | Capability-gated |
| Q | BPF token + mmapable map | ~1-5 ns | Kernel 6.9+ | Token-delegated |

**How they work:** BPF maps are kernel-managed shared data structures accessible from both userspace and BPF programs. A mmapable BPF array map (`BPF_F_MMAPABLE`) can be mmap'd by both the controller and the tool library, giving ~1-5 ns atomic load performance. The unique value is that the same map is simultaneously readable by kernel-side BPF programs, enabling hybrid kernel+userspace tracing controlled by a single flag. BPF ring buffers provide high-throughput streaming (good for trace data) but are unidirectional and message-based (poor for shared state).

### Category 3: Socket-Based

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| F | Unix domain socket | ~1-5 ns (local atomic) | Nothing | `SO_PEERCRED` (kernel-verified effective UID/PID) |
| F+memfd | Unix socket + `memfd_create` | ~1-5 ns (mmap'd memfd) | Nothing | `SO_PEERCRED` + no filesystem entry |

**How they work:** The tool library creates a listener socket (abstract namespace) and spawns a background thread during its `initialize` callback. The controller connects, is authenticated via `SO_PEERCRED` (kernel-verified effective UID/GID at connect time, unforgeable), and sends commands. The background thread calls `rocprofiler_start_context()` / `rocprofiler_stop_context()`. No socket I/O on the hot path — the hot path is entirely within rocprofiler-sdk's existing `populate_contexts()`. The F+memfd variant adds `memfd_create` for anonymous shared memory passed via `SCM_RIGHTS`, enabling the controller to write commands directly to mmap'd memory.

### Category 4: Direct Memory Access

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| L | `/proc/<pid>/mem` direct write | ~1-5 ns (target side) | ptrace permissions | `ptrace_scope` + UID |
| M | `process_vm_writev` | ~1-5 ns (target side) | ptrace permissions | `ptrace_scope` + UID |

**How they work:** The tool library exports known symbols in its `.data` section. The controller resolves their addresses by parsing `/proc/<pid>/maps` + ELF symbol offsets, then writes directly to the target's memory via `/proc/<pid>/mem` (pwrite) or `process_vm_writev` (scatter-gather syscall). The target process is completely passive — no background thread needed. This is conceptually how GDB modifies variables in a running process.

### Category 5: Signal-Based

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| J | SIGUSR1/SIGUSR2 | ~1-5 ns (`sig_atomic_t`) | Nothing | `kill()` UID check |
| K | Real-time signal (SIGRTMIN+n) | ~1-5 ns | Nothing | `kill()` UID check |

**How they work:** The tool library registers a signal handler during its `initialize` callback. The controller sends `kill(target_pid, SIGUSR1)`. The kernel enforces that only processes with the same UID can send signals. Carries zero configuration data (SIGUSR1/2) or one `int` via `sigqueue()` (real-time signals). Useful as an instant wakeup trigger paired with a data channel (B or F+memfd) for the actual command.

### Category 6: Code Injection

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| N | ptrace inject (rocprofiler-sdk approach) | ~50-200 ns | ptrace permissions, x86-64 | `ptrace_scope` + UID |

**How it works:** This is what rocprofiler-sdk uses today for `rocprofv3 --attach`. The controller uses `PTRACE_SEIZE` to attach to a pre-created safe thread in the target, then injects x86-64 assembly (`0f 05` syscall + `cc` INT3) to allocate memory, write environment data, and call `rocprofiler_register_attach()` — which triggers full profiler initialization including dlopen of tool libraries and intercept table updates. The target must have opted in by running with `ROCP_TOOL_ATTACH=1` (which creates the safe background thread). This is the most powerful approach (can execute arbitrary code in the target) but also the most complex (~1500 lines of ptrace/assembly code).

## Elimination Criteria

We applied two hard filters:

### Filter 1: No sudo / No capabilities / No specific kernel version

| Eliminated | Reason |
|------------|--------|
| **C** (BPF map + mmap) | Requires `CAP_BPF` on both controller and tool library |
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
| **B** (mmap regular file) | ~10-20 ns | Directory `0700` + file `0600` | Command + status | `/run/user/<uid>/...` |
| **F** (Unix domain socket) | ~10-20 ns | `SO_PEERCRED` (kernel-verified) | Full protocol | None (abstract namespace) |
| **F+memfd** (Socket + anonymous shm) | ~10-20 ns | `SO_PEERCRED` | Command struct + protocol | None whatsoever |
| **Signal + B or F** (Trigger + data channel) | ~10-20 ns | `kill()` UID check + paired channel | Via paired channel | Depends on pair |

Hot-path overhead is from rocprofiler-sdk's existing `populate_contexts()` (~10-20 ns). The control channel itself adds zero to the hot path — it only toggles `rocprofiler_start_context()` / `rocprofiler_stop_context()` from a background thread or signal handler.

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
| Hot-path (noop) | ~10-20 ns (existing populate_contexts) | ~10-20 ns | ~10-20 ns | ~10-20 ns |
| Context toggle | ~1 ms (poll interval) | ~5 μs (socket recv) | ~50-100 ns (mmap write) + ~1 ms (poll) | ~1-5 μs (signal delivery) |

### Architecture Fit for rocprofiler-sdk

| Aspect | B | F | F+memfd | Signal+B |
|--------|---|---|---------|----------|
| Per-function enable | Existing `rocprofiler_configure_*` at init | Same | Same | Same |
| Multi-runtime config | Single ctrl file per process | Single socket per process | Single socket + memfd | Single signal + paired channel |
| Bidirectional (status queries) | Status fields in mmap | Yes (native) | Yes (socket) + fast status (memfd) | Limited |
| Graceful detach | CMD_DEACTIVATE in mmap | Send CMD_DISABLE, get ACK | Send CMD_DISABLE, get ACK | Signal + paired |
| Changes to rocprofiler-sdk | Minimal (tool lib + bg thread) | Same + socket protocol | Same + memfd + SCM_RIGHTS | Same + signal handler |

## Special Considerations

### OpenMP OMPT: Always-Enabled Shim with Noop Control

The OpenMP 5.0/5.1/5.2 spec defines OMPT tool initialization as a **one-shot event** during runtime startup. The runtime calls `ompt_start_tool()` exactly once during the first OpenMP construct or API call. If no tool is found at that point, OMPT is disabled for the lifetime of the process. There is no re-initialization or late-attachment path.

- `OMP_TOOL_LIBRARIES` must be set **before process launch**. The runtime reads it during initialization only.
- `OMP_TOOL=enabled|disabled` is similarly read once at init time.
- OpenMP 6.0 does not change this — OMPT remains init-time-only.

**The solution: start OMPT enabled, let rocprofiler-sdk's context activation control noop behavior.** The OMPT tool is always registered from process start via `OMP_TOOL_LIBRARIES`. The OMPT callbacks feed into rocprofiler-sdk's existing tracing infrastructure — they register a context with `ROCPROFILER_CALLBACK_TRACING_OMPT` domain. When the context is inactive (controller hasn't attached), `populate_contexts()` finds nothing and the callbacks are effectively noop. When the controller activates the context, callbacks start recording.

This approach means:
1. **OMPT is registered at process start** via `ompt_start_tool()` / `OMP_TOOL_LIBRARIES`
2. **OMPT callbacks register a rocprofiler context** with OMPT tracing domain during `tool_initialize`
3. **Context starts inactive** — `populate_contexts()` returns empty, wrappers noop (~10-20 ns)
4. **When the controller attaches**, the control channel calls `rocprofiler_start_context()` — OMPT callbacks start recording
5. **When the controller detaches**, `rocprofiler_stop_context()` — callbacks return to noop

This is fully consistent with the noop-by-default dispatch tracer architecture. The OMPT shim is just another "runtime" alongside HIP, HSA, RCCL, etc., controlled by the same atomic flag and same control channel.

### OpenMP Tool Plugin Interface

Anthony raised the concern about tracing OpenMP when the runtime is outside the team's direct control. The OMPT spec itself defines a **plugin-based tool interface** that fits naturally into the dispatch tracer:

| Mechanism | How it works | Who controls it |
|-----------|-------------|-----------------|
| `ompt_start_tool()` | The OpenMP runtime calls this to discover tools. The tool returns callback registrations. | OpenMP spec — standard interface |
| `OMP_TOOL_LIBRARIES` | Environment variable listing shared libraries that provide `ompt_start_tool()`. | User sets before launch |
| `ompt_set_callback()` | Tool registers callbacks for specific OMPT events (parallel begin/end, task create, sync, etc.). | Tool library at init time |

The dispatch tracer provides its OMPT tool as a shared library (e.g., `librocprofiler-sdk-ompt-tool.so`) that:
1. Exports `ompt_start_tool()` — discovered via `OMP_TOOL_LIBRARIES`
2. Registers all OMPT callbacks via `ompt_set_callback()`
3. Each callback feeds into rocprofiler-sdk's context system — `populate_contexts()` determines whether the context is active
4. Events flow through the existing `callback_tracing_service` / `buffer_tracing_service` infrastructure

For comparison, here is how other profiling tools handle OpenMP:

| Tool | OpenMP approach |
|------|----------------|
| **rocprofiler-sdk** | Provides an OMPT tool library; registers at init via `OMP_TOOL_LIBRARIES` |
| **Intel VTune** | Uses ITT + OMPT integration; OMPT tool built into VTune collector |
| **NVIDIA Nsight** | OMPT support via `libnvtx_ompt.so` loaded via `OMP_TOOL_LIBRARIES` |

All of them follow the same pattern: ship an OMPT tool library, register at init, control behavior at runtime. The dispatch tracer's noop-shim approach is identical.

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

- **B** is the simplest — minimal code, one background thread, no sockets. Best for straightforward enable/disable.
- **F** has the strongest authentication — kernel-verified identity on every connection. Best for security-sensitive deployments.
- **F+memfd** combines F's authentication with mmap-speed config access and zero filesystem footprint. Best overall for production.
- **Signal+B/F** adds instant notification without polling. Useful as an enhancement to B or F.

Each option is designed in detail in its own document:

- [Option B Design: mmap Regular File](OPTION_B_MMAP_FILE.md)
- [Option F Design: Unix Domain Socket](OPTION_F_UNIX_SOCKET.md)
- [Option F+memfd Design: Socket + Anonymous Shared Memory](OPTION_F_MEMFD.md)
- [Option Signal+B/F Design: Signal-Triggered Data Channel](OPTION_SIGNAL.md)

## References

### rocprofiler-sdk Source Code

- [rocprofiler-sdk](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-sdk) — AMD's profiling SDK with intercept table architecture
- `source/lib/rocprofiler-sdk/intercept_table.cpp` — Intercept table registration and notification
- `source/lib/rocprofiler-sdk/hip/hip.cpp` — HIP API dispatch table interception (`copy_table`, `update_table`, `functor`)
- `source/lib/rocprofiler-sdk/hsa/hsa.cpp` — HSA API dispatch table interception
- `source/lib/rocprofiler-sdk/tracing/tracing.hpp` — `populate_contexts`, `context_filter`, callback/buffer dispatch
- `source/lib/rocprofiler-sdk/registration.cpp` — Tool registration, `rocprofiler_configure`, `atexit` finalization
- `source/lib/rocprofiler-sdk-rocattach/rocattach.cpp` — Existing ptrace-based attach mechanism

### rocprofiler-register

- [rocprofiler-register](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-register) — Library registration coordinator
- `source/include/rocprofiler-register/rocprofiler-register.h` — Public API: `rocprofiler_register_library_api_table()`

### Linux IPC Mechanisms

- [shm_open(3)](https://man7.org/linux/man-pages/man3/shm_open.3.html) — POSIX shared memory
- [unix(7)](https://man7.org/linux/man-pages/man7/unix.7.html) — Unix domain sockets, `SO_PEERCRED`, abstract namespace
- [memfd_create(2)](https://man7.org/linux/man-pages/man2/memfd_create.2.html) — Anonymous memory file descriptors
- [cmsg(3)](https://man7.org/linux/man-pages/man3/cmsg.3.html) — `SCM_RIGHTS` file descriptor passing
- [sigqueue(3)](https://man7.org/linux/man-pages/man3/sigqueue.3.html) — Real-time signals with data
- [bpf(2)](https://man7.org/linux/man-pages/man2/bpf.2.html) — BPF maps (evaluated and eliminated)
- [ptrace(2)](https://man7.org/linux/man-pages/man2/ptrace.2.html) — Process trace (evaluated and eliminated)
- [process_vm_writev(2)](https://man7.org/linux/man-pages/man2/process_vm_writev.2.html) — Cross-process memory write (evaluated and eliminated)

### OpenMP OMPT

- [OpenMP 5.2 Specification](https://www.openmp.org/spec-html/5.2/openmpch19.html) — OMPT tool interface
- `ompt_start_tool()` — One-shot init-time tool discovery (no late attachment)

### OpenTelemetry

- [OpenTelemetry C++ SDK](https://github.com/open-telemetry/opentelemetry-cpp) — Potential output format via OTLP export

### Security

- [Yama LSM](https://www.kernel.org/doc/html/latest/admin-guide/LSM/Yama.html) — `ptrace_scope` (reason for eliminating Options L, M, N)
- [BPF Token](https://docs.ebpf.io/linux/concepts/token/) — Capability delegation (evaluated, eliminated due to kernel 6.9+ requirement)
- [Pinning](https://docs.ebpf.io/linux/concepts/pinning/) — BPF filesystem pinning
