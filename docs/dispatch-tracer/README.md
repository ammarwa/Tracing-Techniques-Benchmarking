# Dispatch Table Tracer Design Documents

This directory contains the design documentation for adding an **external control channel** to rocprofiler-sdk via a **late-load architecture**. A small stub library is preloaded at process start (no `rocprofiler_configure` symbol → rocprofiler-register sees no tool → does NOT load rocprofiler-sdk → original function pointers stay in dispatch tables → **0 ns hot-path overhead**). When the controller attaches via the control channel, the stub `dlopen`s the SDK tool library and calls `rocprofiler_force_configure()` (which succeeds because SDK init_status is still 0). The SDK initializes with the controller-specified domains, propagation runs, and wrappers install for the requested operations only.

## Documents

| Document | Description |
|----------|-------------|
| [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) | Survey of all 13 control channel mechanisms evaluated, elimination criteria, and final selection of 4 surviving options |
| [OPTION_B_MMAP_FILE.md](OPTION_B_MMAP_FILE.md) | Design using memory-mapped regular file under `/run/user/<uid>/` — simplest control channel |
| [OPTION_F_UNIX_SOCKET.md](OPTION_F_UNIX_SOCKET.md) | Design using Unix domain socket with `SO_PEERCRED` — strongest authentication |
| [OPTION_F_MEMFD.md](OPTION_F_MEMFD.md) | Design combining Unix socket auth with `memfd_create` anonymous shared memory — best overall for production |
| [OPTION_SIGNAL.md](OPTION_SIGNAL.md) | Design using real-time signals as instant notification layered on top of mmap or memfd |
| [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) | Measured benchmark results for all 4 options (noop, attach latency, active tracing) on AMD EPYC 9354 |

## Quick Comparison

| Channel | Hot-Path (no attach) | Hot-Path (active) | Auth Model | Best For |
|--------|----------------------|-------------------|------------|----------|
| **mmap** (file) | **0 ns** | ~50-200 ns | Dir `0700` + file `0600` | Simplicity |
| **socket** (Unix domain) | **0 ns** | ~50-200 ns | `SO_PEERCRED` (kernel-verified) | Authentication |
| **memfd** (socket + anonymous shm) | **0 ns** | ~50-200 ns | `SO_PEERCRED` + anonymous memory | Production |
| **signal** (+ mmap or memfd) | **0 ns** | ~50-200 ns | `kill()` UID + paired channel | Instant attach |

**Late-load architecture**: A small **stub library** is preloaded that does NOT export `rocprofiler_configure`, so rocprofiler-register sees no tool and does NOT load rocprofiler-sdk. Original function pointers remain in the dispatch tables. The application runs at native speed.

When the controller attaches via the control channel, the stub `dlopen`s the actual rocprofiler-sdk tool library and calls `rocprofiler_force_configure()` — which works because rocprofiler-sdk's `init_status` is still 0. The SDK initializes with the controller's chosen domains, propagation runs, and wrappers install for the requested operations.

This achieves **true zero-overhead late attach** without ptrace. See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#late-load-design-defer-rocprofiler-sdk-loading-until-attach) for the full mechanism.

All options require **no root, no capabilities, and no specific kernel version**.

## Context

These designs emerged from evaluating 13 different IPC mechanisms against two hard constraints:

1. **No sudo / capabilities** — Users should not need elevated privileges to trace their own processes
2. **Cross-user security** — Other users must not be able to interfere with profiling sessions

See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) for the full evaluation including the 9 eliminated options and why they were rejected.

## Additional Topics

The survey also covers:

- **OpenMP OMPT integration** — OMPT starts enabled at init, shim controls noop behavior inside callbacks, same control channel as other runtimes
- **OpenMP tool plugin interface** — how the OMPT tool library fits the dispatch tracer architecture, compared with rocprofiler-sdk, VTune, and Nsight approaches
- **OpenTelemetry as output format** — using OTel for export/transport, not instrumentation
- **Late configuration vs late activation** — what the control channel can and cannot reconfigure after process start, and how it coexists with rocprofiler-sdk's existing ptrace attach
- **Cross-platform considerations** — Linux-specific mechanisms and portability fallbacks
