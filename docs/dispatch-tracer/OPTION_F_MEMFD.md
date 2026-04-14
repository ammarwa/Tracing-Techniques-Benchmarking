# Option F+memfd: Dispatch Tracer with Socket + Anonymous Shared Memory

## Overview

This design combines the **Unix domain socket** (Option F) for authentication and command/response with **`memfd_create`** for anonymous shared memory passed via `SCM_RIGHTS`. This gives us the best of both worlds:

- `SO_PEERCRED` for kernel-verified authentication (from Option F)
- mmap-speed command writes from controller (~50-100 ns cache-line transfer)
- **Zero filesystem footprint** — no files in `/dev/shm/`, no files in `/run/`, no socket files. The abstract socket and memfd are both anonymous and vanish on process exit.

This is the **recommended option for production** (e.g., rocprofiler-sdk) as it has the strongest security guarantees with no cleanup burden.

## Integration with rocprofiler-sdk

Same as all options — the tool uses standard rocprofiler-sdk APIs and follows the **single-phase design** described in [Option B](OPTION_B_MMAP_FILE.md#what-changes-minimal): tool registers all domains at init, context starts inactive, control channel handles activate/deactivate/reconfigure. `rocprofiler_force_configure()` is locked after init so adding new domains post-init requires ptrace.

This option combines Option F's socket (for authentication + bootstrap) with a memfd containing the same `rocp_ctrl_t` struct as Option B (including the `rocp_config_t` payload). The differences from Option B: the control struct lives in anonymous memory (no filesystem) and authentication is via `SO_PEERCRED`. The background thread polls the memfd for `CMD_ACTIVATE` / `CMD_DEACTIVATE` / `CMD_RECONFIGURE` and calls `rocprofiler_start_context()` / `rocprofiler_stop_context()` or updates the runtime filter — same as Option B.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                   │
│                                                                  │
│  Existing rocprofiler-sdk flow (unchanged):                     │
│    Runtime → rocprofiler_set_api_table() → copy_table()         │
│    → update_table() installs functor wrappers                    │
│                                                                  │
│  Tool library (loaded via ROCP_TOOL_LIBRARIES):                 │
│    rocprofiler_configure():                                      │
│      create context, register all domains                       │
│      DO NOT activate context yet                                │
│    tool_initialize():                                            │
│      create memfd (rocp_ctrl_t) + socket, spawn bg thread       │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Existing functor hot path (NO CHANGES):                   │  │
│  │                                                            │  │
│  │  hip_api_impl<T,Op>::functor(args...):                     │  │
│  │    populate_contexts(domain, op,                           │  │
│  │        callback_ctxs, buffered_ctxs);                      │  │
│  │    if (callback_ctxs.empty() && buffered_ctxs.empty())     │  │
│  │        return exec(get_table_func(), args);  // noop       │  │
│  │    // ... full tracing path                                │  │
│  │  ~10-20 ns per check (existing populate_contexts)          │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Background Thread (NEW — polls memfd + listens on socket) │  │
│  │                                                            │  │
│  │  Phase 1: Create memfd + control struct (rocp_ctrl_t)     │  │
│  │    memfd = memfd_create("ctrl", MFD_CLOEXEC |             │  │
│  │                              MFD_ALLOW_SEALING)           │  │
│  │    ftruncate(memfd, sizeof(rocp_ctrl_t))                   │  │
│  │    ctrl = mmap(memfd, PROT_READ|PROT_WRITE, MAP_SHARED)   │  │
│  │    ctrl->command = CMD_NONE                                │  │
│  │                                                            │  │
│  │  Phase 2: Listen for controller connection                 │  │
│  │    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)  │  │
│  │    bind(sock, "\0rocprofiler_<pid>")                       │  │
│  │    listen(sock, 1)                                         │  │
│  │                                                            │  │
│  │  Phase 3: On accept, authenticate + share memfd            │  │
│  │    client = accept(sock)                                   │  │
│  │    getsockopt(client, SO_PEERCRED, &cred)                 │  │
│  │    if (cred.uid != getuid()) reject                       │  │
│  │    sendmsg(client, memfd via SCM_RIGHTS)                  │  │
│  │                                                            │  │
│  │  Phase 4: Poll memfd for commands + listen for socket cmds │  │
│  │    if (ctrl->command == CMD_ACTIVATE)                      │  │
│  │      rocprofiler_start_context(ctx);                      │  │
│  │    if (ctrl->command == CMD_DEACTIVATE)                    │  │
│  │      rocprofiler_stop_context(ctx);                       │  │
│  │    // Also handles socket-based CMD_STATUS queries         │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────┬──────────────────────────────────────────┘
                       │ Abstract Unix socket + SCM_RIGHTS fd pass
┌──────────────────────▼──────────────────────────────────────────┐
│                    Controller                                    │
│                                                                  │
│  Phase 1: Connect and authenticate                              │
│    sock = connect("\0rocprofiler_<pid>")                         │
│    // Tool checks our SO_PEERCRED automatically                 │
│                                                                  │
│  Phase 2: Receive memfd via SCM_RIGHTS                          │
│    recvmsg(sock, &msg) → extract memfd_fd from ancillary       │
│    ctrl = mmap(memfd_fd, PROT_READ|PROT_WRITE, MAP_SHARED)    │
│                                                                  │
│  Phase 3: Write commands directly to shared memory              │
│    ctrl->command = CMD_ACTIVATE;                                │
│    atomic_store(&ctrl->version, v+1, RELEASE);                 │
│    // Context activates within ~1 ms (bg thread poll)           │
│                                                                  │
│  Phase 4: Use socket for queries needing responses              │
│    send(sock, CMD_STATUS) → recv response                      │
│                                                                  │
│  Phase 5: Deactivate via mmap                                   │
│    ctrl->command = CMD_DEACTIVATE;                              │
│    atomic_store(&ctrl->version, v+1, RELEASE);                 │
└─────────────────────────────────────────────────────────────────┘

Key: After the initial socket handshake + SCM_RIGHTS fd passing,
     activate/deactivate commands go through mmap (~50-100 ns write).
     The socket is only used for queries needing a response.
```

## Control Structure

Same as Option B — the `rocp_ctrl_t` struct is minimal because per-function configuration is handled by existing `rocprofiler_configure_*` APIs:

```c
#define ROCP_CTRL_MAGIC   0xD15EA7C0  // Same as Option B for interoperability
#define ROCP_CTRL_VERSION 1

enum rocp_ctrl_command {
    CMD_NONE       = 0,
    CMD_ACTIVATE   = 1,
    CMD_DEACTIVATE = 2,
};

typedef struct {
    uint32_t magic;
    uint32_t struct_version;

    _Atomic uint32_t command;
    _Atomic uint32_t version;

    _Atomic uint32_t context_active;
    _Atomic uint32_t context_id;
    _Atomic uint64_t events_traced;
    _Atomic uint64_t events_dropped;

    uint32_t pid;
    uint64_t start_time;
} __attribute__((aligned(64))) rocp_ctrl_t;
```

## SCM_RIGHTS File Descriptor Passing

This is the core mechanism that makes F+memfd work. `SCM_RIGHTS` allows one process to send a file descriptor to another over a Unix domain socket. The kernel duplicates the fd into the receiver's fd table.

### Tool side (sender):

```c
static int send_fd(int sock, int fd_to_send) {
    char buf[1] = {0};
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    return sendmsg(sock, &msg, 0);
}
```

### Controller side (receiver):

```c
static int recv_fd(int sock) {
    char buf[1];
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    if (recvmsg(sock, &msg, 0) <= 0) return -1;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}
```

## memfd Sealing (Optional Hardening)

```c
// The memfd MUST be created with MFD_ALLOW_SEALING for sealing to work.
// Without it, fcntl(F_ADD_SEALS) returns EPERM.
int memfd = memfd_create("ctrl", MFD_CLOEXEC | MFD_ALLOW_SEALING);

// After setup, seal against size changes:
fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
// The memfd cannot be resized after this point.
// Both sides can still read/write the content (F_SEAL_WRITE is NOT set).
```

Sealing only prevents size manipulation, not content corruption. The tool should validate command values read from shared memory.

## Security Analysis

| Property | Assessment |
|----------|------------|
| **Authentication** | `SO_PEERCRED` — kernel-verified effective UID/GID at connect time. PID accurate at connect time, may become stale on peer exit |
| **Filesystem footprint** | **None** — abstract socket + memfd are both anonymous |
| **Race/pre-creation attack** | **Impossible** — no filesystem paths to race on |
| **Other-user access** | Blocked by SO_PEERCRED check in tool |
| **Stale artifacts** | **None** — both abstract socket and memfd vanish on process exit |
| **memfd integrity** | Sealing prevents size tampering (`F_SEAL_SHRINK | F_SEAL_GROW`) |
| **Container isolation** | Abstract sockets scoped to network namespace |
| **Socket probing** | Any process in the same network namespace can attempt to connect, revealing socket existence. SO_PEERCRED check rejects unauthorized connections, but tracer-loaded PIDs are discoverable |

### Why F+memfd is more secure than Option B (mmap file):

1. **No predictable path** — the memfd has no filesystem entry at all
2. **No race window** — the memfd is created by the tool, not discovered by name
3. **Authenticated handoff** — the controller only gets the memfd fd after passing SO_PEERCRED
4. **Sealable** — the tool can prevent size manipulation

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Tool init | ~15-20 μs | `memfd_create` + `mmap` + `socket` + `bind` + `listen` + `pthread_create` |
| Controller connect + memfd recv | ~10-15 μs | `connect` + `recvmsg(SCM_RIGHTS)` + `mmap` |
| **Hot-path (noop)** | **~10-20 ns** | Existing `populate_contexts()` — context inactive |
| **Hot-path (tracing)** | **~50-200 ns** | `populate_contexts()` + callbacks + buffer emplace |
| Command write (mmap) | ~50-100 ns | Direct write to shared memory (cache-line transfer) |
| Context toggle latency | ~1 ms | Background thread poll interval (or futex wake) |
| Status query (socket) | ~2-5 μs | Socket round-trip for response |

## Multi-Runtime Application (rocprofiler-sdk)

Since the tool uses rocprofiler-sdk's context system, multi-runtime support is the same as Option B: a single context covers all registered domains. A single `rocprofiler_start_context(ctx)` activates tracing for HIP, HSA, RCCL, OMPT, etc. simultaneously.

The memfd carries only the `rocp_ctrl_t` command/status struct — no per-runtime control needed. The controller writes `CMD_ACTIVATE` to the memfd; the tool's background thread calls `rocprofiler_start_context()` and all registered domains begin tracing.

**Advantage over Option B**: The memfd has no filesystem path, so there's nothing to clean up on crash and no PID-reuse stale file problem.

**Advantage over Option F (pure socket)**: Commands go through mmap (~50-100 ns) instead of socket send (~1-5 μs). The socket is reserved for status queries that need responses.

### OpenMP Integration

OMPT is registered at init time via `OMP_TOOL_LIBRARIES`. OMPT callbacks register a rocprofiler context with `ROCPROFILER_CALLBACK_TRACING_OMPT` domain. When the controller activates the context, `populate_contexts()` starts finding it — OMPT callbacks begin recording alongside HIP/HSA events. See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#openmp-ompt-always-enabled-shim-with-noop-control) for the full design.

### OpenTelemetry Export

The output format can be extended with `OUTPUT_OTLP` to export trace events as OpenTelemetry spans via OTLP. This is configured at init time via the tool library, not through the control channel.

## File Layout

```
src/tools/rocprofiler_tool_memfd/
├── rocp_memfd.h            # Shared structs (rocp_ctrl_t), constants
├── rocp_memfd_tool.c       # rocprofiler tool library (configure + initialize + memfd + socket)
├── rocp_memfd_fdpass.c     # SCM_RIGHTS helper functions
└── rocp_memfd_controller.c # CLI controller tool
```

## Build Integration

```cmake
option(BUILD_ROCP_TOOL_MEMFD "Build rocprofiler tool with memfd control channel" ON)

if(BUILD_ROCP_TOOL_MEMFD)
    add_library(rocprofiler_tool_memfd SHARED
        src/tools/rocprofiler_tool_memfd/rocp_memfd_tool.c
        src/tools/rocprofiler_tool_memfd/rocp_memfd_fdpass.c
    )
    target_link_libraries(rocprofiler_tool_memfd PRIVATE pthread)
    target_compile_options(rocprofiler_tool_memfd PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_memfd
        src/tools/rocprofiler_tool_memfd/rocp_memfd_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Noop overhead (tool loaded, context not activated):
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_memfd.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing:
# Terminal 1:
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_memfd.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/rocp_ctrl_memfd --pid $! activate
build/bin/rocp_ctrl_memfd --pid $! status
build/bin/rocp_ctrl_memfd --pid $! deactivate
```

## Limitations

1. **Background thread** — Default pthread stack is ~2 MB. Thread must be joinable so `tool_finalize()` (called via `atexit()`) can shut it down cleanly. Finalization is driven by the registration library's atexit handler, matching rocprofiler-sdk.
2. **fork() handling** — After `fork()`, the child inherits the socket fd but the background thread is gone. A `pthread_atfork()` child handler must close the listen fd and set `finalize_status = 1`.
3. **Socket/memfd on exec()** — Listen socket must use `SOCK_CLOEXEC`, memfd must use `MFD_CLOEXEC`, to prevent fd leak after `execve()`.
4. **`memfd_create` availability** — Requires kernel 3.17+ (2014). Sealing requires `MFD_ALLOW_SEALING` flag at creation.
5. **SCM_RIGHTS complexity** — The fd-passing code is ~50 lines of boilerplate.
6. **Single controller** — Only one controller at a time. Limits multi-tool scenarios.
7. **memfd not inspectable** — Contents not viewable with standard tools. Debugging requires the controller.
8. **Overhead estimates are pre-implementation** — Timing figures should be validated after implementation.
