# Dispatch Table Tracer Design Documents

This directory contains the design documentation for a new tracing technique: the **Dispatch Table Tracer**. This technique uses LD_PRELOAD to intercept API calls via a function dispatch table that starts as a noop passthrough and only activates when an external controller process attaches at runtime — inspired by [rocprofiler-sdk](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-sdk)'s intercept table architecture.

## Documents

| Document | Description |
|----------|-------------|
| [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) | Survey of all 13 control channel mechanisms evaluated, elimination criteria, and final selection of 4 surviving options |
| [OPTION_B_MMAP_FILE.md](OPTION_B_MMAP_FILE.md) | Design using memory-mapped regular file under `/run/user/<uid>/` — simplest, no threads |
| [OPTION_F_UNIX_SOCKET.md](OPTION_F_UNIX_SOCKET.md) | Design using Unix domain socket with `SO_PEERCRED` — strongest authentication |
| [OPTION_F_MEMFD.md](OPTION_F_MEMFD.md) | Design combining Unix socket auth with `memfd_create` anonymous shared memory — best overall for production |
| [OPTION_SIGNAL.md](OPTION_SIGNAL.md) | Design using real-time signals as instant notification layered on top of B or F+memfd |

## Quick Comparison

| Option | Hot-Path | Auth Model | Filesystem Footprint | Best For |
|--------|----------|------------|---------------------|----------|
| **B** (mmap file) | ~1-5 ns | Dir `0700` + file `0600` | `/run/user/<uid>/dispatch/` | Simplicity |
| **F** (Unix socket) | ~1-5 ns | `SO_PEERCRED` (kernel-verified) | None (abstract namespace) | Authentication |
| **F+memfd** | ~1-5 ns | `SO_PEERCRED` + anonymous memory | None whatsoever | Production |
| **Signal+B/F** | ~1-5 ns | `kill()` UID + paired channel | Depends on pair | Instant notification |

All options require **no root, no capabilities, and no specific kernel version**.

## Context

These designs emerged from evaluating 13 different IPC mechanisms against two hard constraints:

1. **No sudo / capabilities** — Users should not need elevated privileges to trace their own processes
2. **Cross-user security** — Other users must not be able to interfere with profiling sessions

See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) for the full evaluation including the 9 eliminated options and why they were rejected.

## Additional Topics

The survey also covers:

- **OpenMP OMPT late attachment** — why OMPT cannot be enabled after process start, and how the dispatch tracer handles this
- **Third-party API plugin interface** — how external teams (RCCL, rocdecode, rocjpeg) can register their own tracing
- **OpenTelemetry as output format** — using OTel for export/transport, not instrumentation
- **Cross-platform considerations** — Linux-specific mechanisms and portability fallbacks
