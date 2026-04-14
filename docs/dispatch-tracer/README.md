# Dispatch Table Tracer Design Documents

This directory contains the design documentation for adding an **external control channel** to rocprofiler-sdk's existing dispatch table tracer. The existing functor wrappers already have a noop fast-path (`populate_contexts()` → empty → call original at ~10-20 ns). The control channel lets an external process toggle context activation at runtime, enabling/disabling tracing without restarting the application.

## Documents

| Document | Description |
|----------|-------------|
| [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) | Survey of all 13 control channel mechanisms evaluated, elimination criteria, and final selection of 4 surviving options |
| [OPTION_B_MMAP_FILE.md](OPTION_B_MMAP_FILE.md) | Design using memory-mapped regular file under `/run/user/<uid>/` — simplest control channel |
| [OPTION_F_UNIX_SOCKET.md](OPTION_F_UNIX_SOCKET.md) | Design using Unix domain socket with `SO_PEERCRED` — strongest authentication |
| [OPTION_F_MEMFD.md](OPTION_F_MEMFD.md) | Design combining Unix socket auth with `memfd_create` anonymous shared memory — best overall for production |
| [OPTION_SIGNAL.md](OPTION_SIGNAL.md) | Design using real-time signals as instant notification layered on top of B or F+memfd |

## Quick Comparison

| Option | Hot-Path (loaded, inactive) | Hot-Path (active) | Auth Model | Best For |
|--------|-----------------------------|-------------------|------------|----------|
| **B** (mmap file) | ~10-20 ns | ~50-200 ns | Dir `0700` + file `0600` | Simplicity |
| **F** (Unix socket) | ~10-20 ns | ~50-200 ns | `SO_PEERCRED` (kernel-verified) | Authentication |
| **F+memfd** | ~10-20 ns | ~50-200 ns | `SO_PEERCRED` + anonymous memory | Production |
| **Signal+B/F** | ~10-20 ns | ~50-200 ns | `kill()` UID + paired channel | Instant attach |

The tool registers all domains it might ever trace at init time, so `update_table()` installs wrappers for all those operations. The wrappers' `populate_contexts()` check noops at ~10-20 ns when the context is inactive. When the controller activates the context via the control channel, callbacks fire and emit events according to a runtime filter. The control channel adds zero overhead to the hot path itself.

**Note**: rocprofiler-sdk's `rocprofiler_force_configure()` only works before SDK init starts, so the control channel cannot add new domains post-init. To genuinely add domains after process start (e.g., enable OMPT in a tool that didn't request it at init), use the existing `rocprofv3 --attach --pid` ptrace mechanism.

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
