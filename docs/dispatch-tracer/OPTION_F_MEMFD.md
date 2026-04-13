# Option F+memfd: Dispatch Tracer with Socket + Anonymous Shared Memory

## Overview

This design combines the **Unix domain socket** (Option F) for authentication and command/response with **`memfd_create`** for anonymous shared memory passed via `SCM_RIGHTS`. This gives us the best of both worlds:

- `SO_PEERCRED` for kernel-verified authentication (from Option F)
- mmap-speed (~1-5 ns) config access (from Options A/B)
- **Zero filesystem footprint** — no files in `/dev/shm/`, no files in `/run/`, no socket files. The abstract socket and memfd are both anonymous and vanish on process exit.

This is the **recommended option for production** (e.g., rocprofiler-sdk) as it has the strongest security guarantees and lowest possible hot-path overhead with no cleanup burden.

## Initialization: rocprofiler-register Methodology

Same as all options — see [Option B](OPTION_B_MMAP_FILE.md#initialization-rocprofiler-register-methodology) for the full registration flow. The runtime library calls `dispatch_register_library_api_table()` during its own init. The tool library receives the API table via a callback, saves originals, installs shim wrappers, creates the memfd + socket control channel, and spawns the background thread.

No `__attribute__((constructor))` or `dlsym(RTLD_NEXT)` — original function pointers come from the registration.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                   │
│                                                                  │
│  libmylib.so (runtime):                                         │
│    init(): dispatch_register_library_api_table("mylib", ...)    │
│                                                                  │
│  libdispatch_tool.so (tracer — via DISPATCH_TOOL_LIBRARIES):    │
│    on_intercept_table_registration():                            │
│      save orig_table, install shim wrappers                     │
│      create memfd + socket, spawn bg thread                     │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Main Thread (API calls go through shims in api_table)     │  │
│  │                                                            │  │
│  │  hot path:                                                 │  │
│  │    enabled = __atomic_load_n(&ctrl->tracing_enabled,       │  │
│  │                              __ATOMIC_ACQUIRE);            │  │
│  │    if (enabled) { trace(); orig_fn(); trace_exit(); }      │  │
│  │    else { orig_fn(); }                                     │  │
│  │                                                            │  │
│  │  ctrl points to mmap'd memfd (anonymous shared memory)     │  │
│  │  ~1-5 ns per check — no syscall, no socket, no file I/O   │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Background Thread (created during registration callback)  │  │
│  │                                                            │  │
│  │  Phase 1: Create memfd + control struct                   │  │
│  │    memfd = memfd_create("ctrl", MFD_CLOEXEC |             │  │
│  │                              MFD_ALLOW_SEALING)           │  │
│  │    ftruncate(memfd, sizeof(dispatch_ctrl_t))               │  │
│  │    ctrl = mmap(memfd, PROT_READ|PROT_WRITE, MAP_SHARED)   │  │
│  │    ctrl->tracing_enabled = 0                               │  │
│  │                                                            │  │
│  │  Phase 2: Listen for controller connection                 │  │
│  │    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)  │  │
│  │    bind(sock, "\0dispatch_<pid>")                          │  │
│  │    listen(sock, 1)                                         │  │
│  │                                                            │  │
│  │  Phase 3: On accept, authenticate + share memfd            │  │
│  │    client = accept(sock)                                   │  │
│  │    getsockopt(client, SO_PEERCRED, &cred)                 │  │
│  │    if (cred.uid != getuid()) reject                       │  │
│  │    sendmsg(client, memfd via SCM_RIGHTS)  ← fd passing    │  │
│  │                                                            │  │
│  │  Phase 4: Process commands via socket                      │  │
│  │    recv(client, &cmd) → apply config → send response      │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────┬──────────────────────────────────────────┘
                       │ Abstract Unix socket + SCM_RIGHTS fd pass
┌──────────────────────▼──────────────────────────────────────────┐
│                    Controller (dispatch_ctrl_memfd)               │
│                                                                  │
│  Phase 1: Connect and authenticate                              │
│    sock = connect("\0dispatch_<pid>")                            │
│    // Library checks our SO_PEERCRED automatically              │
│                                                                  │
│  Phase 2: Receive memfd via SCM_RIGHTS                          │
│    recvmsg(sock, &msg)  → extract memfd_fd from ancillary      │
│    ctrl = mmap(memfd_fd, PROT_READ|PROT_WRITE, MAP_SHARED)     │
│                                                                  │
│  Phase 3: Write config directly to shared memory                │
│    ctrl->output_format = OUTPUT_JSON;                           │
│    ctrl->ring_buffer_size = 4 * 1024 * 1024;                   │
│    memset(ctrl->func_enable_mask, 0xFF, ...);                   │
│    __atomic_store_n(&ctrl->tracing_enabled, 1,                  │
│                     __ATOMIC_RELEASE);                           │
│                                                                  │
│  // Config changes now take effect in ~50-100 ns               │
│  // (cache-line transfer, no socket round-trip needed!)         │
│                                                                  │
│  Phase 4: Use socket for commands (flush, status, detach)       │
│    send(sock, CMD_STATUS) → recv response                       │
│    send(sock, CMD_DISABLE) → recv response                      │
│                                                                  │
│  // Detach: write enabled=0 to memfd, then close socket         │
│  __atomic_store_n(&ctrl->tracing_enabled, 0, __ATOMIC_RELEASE);│
│  close(sock);                                                    │
└─────────────────────────────────────────────────────────────────┘

Key: After the initial socket handshake + SCM_RIGHTS fd passing,
     config reads/writes go through mmap'd shared memory (~1-5 ns).
     The socket is only used for commands that need a response
     (status queries, flush requests, graceful detach).
```

## Bootstrap Sequence (Detailed)

```
Controller                               Library (bg thread)
    │                                        │
    │  1. connect("\0dispatch_<pid>")         │
    ├───────────────────────────────────────>│
    │                                        │ accept()
    │                                        │ SO_PEERCRED → verify UID
    │                                        │
    │  2. Library sends memfd via SCM_RIGHTS │
    │<───────────────────────────────────────┤ sendmsg(cmsg=SCM_RIGHTS,
    │                                        │         fd=memfd)
    │  recvmsg() → extract memfd fd          │
    │  mmap(memfd_fd) → ctrl pointer         │
    │                                        │
    │  3. Write config to shared memory      │
    │  (direct mmap write, no socket needed) │
    │     ctrl->output_format = JSON         │
    │     ctrl->func_enable_mask = 0xFF...   │
    │     atomic_store(enabled, 1)           │
    │ - - - - - - - - - - - - - - - - - - ->│ atomic_load sees enabled=1
    │                                        │ tracing begins
    │                                        │
    │  4. (later) Status query via socket    │
    │  CMD_STATUS                             │
    ├───────────────────────────────────────>│
    │  {enabled:1, events:42000}             │
    │<───────────────────────────────────────┤
    │                                        │
    │  5. Disable via shared memory          │
    │     atomic_store(enabled, 0)           │
    │ - - - - - - - - - - - - - - - - - - ->│ tracing stops
    │                                        │
    │  6. Request flush via socket           │
    │  CMD_FLUSH                              │
    ├───────────────────────────────────────>│
    │  {OK, events_flushed: 42500}           │
    │<───────────────────────────────────────┤
    │                                        │
    │  7. close()                             │
    ├───────────────────────────────────────>│
```

## SCM_RIGHTS File Descriptor Passing

This is the core mechanism that makes F+memfd work. `SCM_RIGHTS` allows one process to send a file descriptor to another over a Unix domain socket. The kernel duplicates the fd into the receiver's fd table.

### Library side (sender):

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

## Control Structure

Same as Option B, but lives in anonymous memory (memfd) rather than a file:

```c
#define DISPATCH_MAGIC 0xD15EFD00
#define MAX_TRACED_FUNCTIONS 2048  // HIP ~1300, HSA ~400, RCCL ~300
#define DISPATCH_STRUCT_VERSION 1  // Bump on layout changes

typedef struct {
    uint32_t magic;
    uint32_t struct_version;         // Detects library/controller version mismatch
    uint32_t config_version;         // Bumped on every config change

    _Atomic uint32_t tracing_enabled;

    /* Double-buffered config for race-free reconfiguration.
     * Controller writes to inactive slot, then atomically swaps active_config_slot. */
    _Atomic uint32_t active_config_slot; // 0 or 1
    _Atomic uint32_t func_enable_mask[2][MAX_TRACED_FUNCTIONS / 32];

    struct {
        uint32_t output_format;      // TEXT=0, JSON=1, PERFETTO=2
        uint32_t ring_buffer_size;   // Bytes, must be power of 2
        char output_path[256];
        char filter_pattern[256];
        char exclude_pattern[256];
    } config_slots[2];

    _Atomic uint64_t events_traced;
    _Atomic uint64_t events_dropped;
} __attribute__((aligned(64))) dispatch_ctrl_t;
```

## memfd Sealing (Optional Hardening)

After the controller receives the memfd and before tracing begins, the library can apply **seals** to prevent unexpected modifications:

```c
// The memfd MUST be created with MFD_ALLOW_SEALING for sealing to work.
// Without it, fcntl(F_ADD_SEALS) returns EPERM.
int memfd = memfd_create("dispatch_ctrl", MFD_CLOEXEC | MFD_ALLOW_SEALING);

// After setup, seal against size changes:
fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);
// The memfd cannot be resized after this point.
// Both sides can still read/write the content (F_SEAL_WRITE is NOT set).
```

This prevents a compromised controller from extending the memfd to cause the library to read out-of-bounds. Note that sealing only prevents size manipulation, not content corruption — the library should validate all fields read from shared memory (bounds-check strings, validate enum ranges, etc.).

## Security Analysis

| Property | Assessment |
|----------|------------|
| **Authentication** | `SO_PEERCRED` — kernel-verified effective UID/GID at connect time. PID accurate at connect time, may become stale on peer exit |
| **Filesystem footprint** | **None** — abstract socket + memfd are both anonymous |
| **Race/pre-creation attack** | **Impossible** — no filesystem paths to race on |
| **Other-user access** | Blocked by SO_PEERCRED check in library |
| **Stale artifacts** | **None** — both abstract socket and memfd vanish on process exit |
| **memfd integrity** | Sealing prevents size tampering (`F_SEAL_SHRINK | F_SEAL_GROW`) |
| **Container isolation** | Abstract sockets scoped to network namespace |
| **Socket probing** | Any process in the same network namespace can attempt to connect, revealing socket existence. SO_PEERCRED check rejects unauthorized connections, but tracer-loaded PIDs are discoverable |

### Why F+memfd is more secure than A (POSIX shm):

1. **No predictable name** in `/dev/shm/` — the memfd has no filesystem entry at all
2. **No race window** — the memfd is created by the library, not discovered by name
3. **Authenticated handoff** — the controller only gets the memfd fd after passing SO_PEERCRED
4. **Sealable** — the library can prevent size manipulation

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Library init | ~15-20 μs | `memfd_create` + `mmap` + `socket` + `bind` + `listen` + `pthread_create` |
| Controller connect + memfd recv | ~10-15 μs | `connect` + `recvmsg(SCM_RIGHTS)` + `mmap` |
| **Hot-path (noop)** | **~1-5 ns** | Atomic load of mmap'd memfd — identical to Options A/B |
| **Hot-path (tracing)** | **~50-150 ns** | Atomic load + timestamp + ring buffer |
| Config change (mmap) | ~50-100 ns | Direct write to shared memory (cache-line transfer) |
| Status query (socket) | ~2-5 μs | Socket round-trip for response |
| Detach (mmap + socket) | ~5 μs | Atomic store + CMD_FLUSH + close |

## Multi-Runtime Application (rocprofiler-sdk)

For tracing HIP, HSA, RCCL, OpenMP, rocdecode, rocjpeg:

The library creates **one memfd per runtime** and shares all of them during the socket handshake:

```
Bootstrap:
  Controller connects → SO_PEERCRED verified
  Library sends 6 memfds via SCM_RIGHTS:
    fd[0] = hip_ctrl     (mmap'd by HIP dispatch wrapper)
    fd[1] = hsa_ctrl     (mmap'd by HSA dispatch wrapper)
    fd[2] = rccl_ctrl    (mmap'd by RCCL dispatch wrapper)
    fd[3] = ompt_ctrl    (mmap'd by OpenMP dispatch wrapper)
    fd[4] = rocdecode_ctrl
    fd[5] = rocjpeg_ctrl

Runtime:
  Controller writes to hip_ctrl->tracing_enabled = 1   (~50 ns)
  Controller writes to hsa_ctrl->tracing_enabled = 1   (~50 ns)
  // Each runtime's wrapper reads its own ctrl independently
  // No socket I/O for enable/disable — just mmap writes

  Controller sends CMD_STATUS via socket → gets aggregated stats
  Controller sends CMD_FLUSH via socket → all runtimes flush
```

This scales cleanly to any number of runtimes without protocol changes.

### OpenMP Integration

OMPT is started **enabled at init time** — the OMPT tool library (`libdispatch_ompt_tool.so`) is loaded via `OMP_TOOL_LIBRARIES` before the OpenMP runtime initializes. All OMPT callbacks are registered immediately but behave as **noops by default** — each checks the same `ctrl->tracing_enabled` atomic flag that the dispatch table hot path uses. When the controller attaches and sets `tracing_enabled = 1`, OMPT callbacks begin recording events through the same memfd-backed ring buffer as HIP/HSA events. This makes OpenMP tracing a peer of other runtimes, controlled by the same memfd control region.

The OMPT tool's memfd is included in the bootstrap handshake alongside the other runtimes:

```
Bootstrap (updated):
  Library sends 7 memfds via SCM_RIGHTS:
    fd[0] = hip_ctrl
    fd[1] = hsa_ctrl
    fd[2] = rccl_ctrl
    fd[3] = ompt_ctrl    ← OpenMP OMPT control
    fd[4] = rocdecode_ctrl
    fd[5] = rocjpeg_ctrl
    fd[6] = global_ctrl  ← master enable + aggregated stats
```

See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#openmp-ompt-always-enabled-shim-with-noop-control) for the full OMPT noop-shim design and callback examples.

### OpenTelemetry Export

The output format can be extended with `OUTPUT_OTLP` to export trace events as OpenTelemetry spans via OTLP. Each intercepted API call maps to an OTel span with attributes like `hip.function`, `hip.stream`, `gpu.device_id`, `memory.size_bytes`. This enables integration with Jaeger, Grafana Tempo, and other OTel-compatible backends.

## File Layout

```
src/tools/dispatch_tracer_memfd/
├── dispatch_memfd.h            # Shared structs, constants
├── dispatch_memfd_wrapper.c    # LD_PRELOAD library + bg thread + memfd
├── dispatch_memfd_trace.c      # Tracing logic
├── dispatch_memfd_fdpass.c     # SCM_RIGHTS helper functions
└── dispatch_memfd_controller.c # CLI controller tool
```

## Build Integration

```cmake
option(BUILD_DISPATCH_MEMFD "Build dispatch table tracer (memfd)" ON)

if(BUILD_DISPATCH_MEMFD)
    add_library(mylib_dispatch_memfd SHARED
        src/tools/dispatch_tracer_memfd/dispatch_memfd_wrapper.c
        src/tools/dispatch_tracer_memfd/dispatch_memfd_trace.c
        src/tools/dispatch_tracer_memfd/dispatch_memfd_fdpass.c
    )
    target_link_libraries(mylib_dispatch_memfd PRIVATE dl pthread)
    target_compile_options(mylib_dispatch_memfd PRIVATE -O2 -fPIC)

    add_executable(dispatch_ctrl_memfd
        src/tools/dispatch_tracer_memfd/dispatch_memfd_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Noop overhead:
LD_PRELOAD=build/lib/libmylib_dispatch_memfd.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing:
# Terminal 1:
LD_PRELOAD=build/lib/libmylib_dispatch_memfd.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/dispatch_ctrl_memfd --pid $! enable
build/bin/dispatch_ctrl_memfd --pid $! status
build/bin/dispatch_ctrl_memfd --pid $! disable
```

## Limitations

1. **Background thread** — Default pthread stack is ~2 MB. Thread must be joinable (not detached) so the destructor can `pthread_join()` before library unmap on `dlclose()`.
2. **fork() handling** — After `fork()`, the child inherits the socket fd but the background thread is gone. A `pthread_atfork()` child handler must: close the listen fd, set `tracing_enabled = 0`, and optionally re-create the control socket.
3. **Socket/memfd on exec()** — Listen socket must use `SOCK_CLOEXEC`, memfd must use `MFD_CLOEXEC`, to prevent fd leak after `execve()`.
4. **`memfd_create` availability** — Requires kernel 3.17+ (2014). Sealing requires `MFD_ALLOW_SEALING` flag at creation.
5. **SCM_RIGHTS complexity** — The fd-passing code is ~50 lines of boilerplate.
6. **Single controller** — Only one controller at a time. Limits multi-tool scenarios in rocprofiler-sdk.
7. **memfd not inspectable** — Contents not viewable with standard tools. Debugging requires the controller.
8. **Overhead estimates are pre-implementation** — Timing figures should be validated after implementation.
