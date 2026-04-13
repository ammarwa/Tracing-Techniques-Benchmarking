# Option F: Dispatch Tracer with Unix Domain Socket Control Channel

## Overview

This design uses a **Unix domain socket** (abstract namespace) as the control channel. The tool library creates a listener socket and a background thread during its `initialize` callback. The controller connects, is authenticated via `SO_PEERCRED` (kernel-verified, unforgeable UID/GID), and sends structured commands (activate, deactivate, query status). The hot path is entirely within rocprofiler-sdk's existing `populate_contexts()` — no socket I/O per intercepted call.

This option provides the **strongest authentication model** of all options: the kernel verifies the connecting process's identity, and this verification cannot be forged.

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                  │
│                                                                 │
│  Existing rocprofiler-sdk flow (unchanged):                    │
│    Runtime → rocprofiler_set_api_table() → copy_table()        │
│    → update_table() installs functor wrappers                   │
│                                                                 │
│  Tool library (loaded via ROCP_TOOL_LIBRARIES):                │
│    rocprofiler_configure():                                     │
│      create context, register all domains                      │
│      DO NOT activate context yet                               │
│    tool_initialize():                                           │
│      setup_socket_control(ctx)                                 │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Existing functor hot path (NO CHANGES):                  │  │
│  │                                                           │  │
│  │  hip_api_impl<T,Op>::functor(args...):                    │  │
│  │    populate_contexts(domain, op,                          │  │
│  │        callback_ctxs, buffered_ctxs);                     │  │
│  │    if (callback_ctxs.empty() && buffered_ctxs.empty())    │  │
│  │        return exec(get_table_func(), args);  // noop      │  │
│  │    // ... full tracing path                               │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Background Thread (NEW — listens on abstract socket):    │  │
│  │                                                           │  │
│  │  loop:                                                    │  │
│  │    client = accept(sock)                                  │  │
│  │    SO_PEERCRED → verify UID                               │  │
│  │    recv(client, &cmd)                                     │  │
│  │    switch (cmd.type):                                     │  │
│  │      CMD_ACTIVATE:                                        │  │
│  │        rocprofiler_start_context(ctx);  // existing API   │  │
│  │        send(client, {OK});                                │  │
│  │      CMD_DEACTIVATE:                                      │  │
│  │        rocprofiler_stop_context(ctx);   // existing API   │  │
│  │        send(client, {OK, events_count});                  │  │
│  │      CMD_STATUS:                                          │  │
│  │        send(client, {active, events, ...});               │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  Abstract socket: \0rocprofiler_12345 (no filesystem entry)    │
└──────────────────────┬─────────────────────────────────────────┘
                       │ Unix domain socket (abstract namespace)
┌──────────────────────▼─────────────────────────────────────────┐
│                    Controller                                   │
│                                                                 │
│  sock = connect("\0rocprofiler_<pid>")                          │
│  send(sock, {CMD_ACTIVATE})                                    │
│  recv(sock, {OK})                                              │
│  // ... tracing active ...                                     │
│  send(sock, {CMD_DEACTIVATE})                                  │
│  recv(sock, {OK, events: 42500})                               │
└────────────────────────────────────────────────────────────────┘
```

## Protocol Design

Since per-function configuration and output format are handled by existing `rocprofiler_configure_*` APIs at init time, the socket protocol only needs to carry **context activation commands** and **status queries**:

### Command Types

```c
enum rocp_cmd_type {
    CMD_ACTIVATE   = 1,   // Activate context (start tracing)
    CMD_DEACTIVATE = 2,   // Deactivate context (stop tracing)
    CMD_STATUS     = 3,   // Query current state
};
```

### Message Format

```c
typedef struct {
    uint32_t type;          // rocp_cmd_type
    uint32_t flags;         // Reserved for future use
} rocp_cmd_t;

typedef struct {
    uint32_t status;         // OK=0, ERROR=1
    uint32_t context_active; // 0 = inactive, 1 = active
    uint64_t events_traced;
    uint64_t events_dropped;
} rocp_response_t;
```

### Protocol Flow

```
Controller                      Tool (bg thread)
    │                               │
    │  connect("\0rocprofiler_<pid>")│
    ├──────────────────────────────>│
    │                               │ accept() + SO_PEERCRED check
    │                               │
    │  CMD_ACTIVATE                 │
    ├──────────────────────────────>│
    │                               │ rocprofiler_start_context(ctx)
    │     {OK, active: 1}          │
    │<──────────────────────────────┤
    │                               │
    │  CMD_STATUS                   │
    ├──────────────────────────────>│
    │  {OK, active:1, events:42000}│
    │<──────────────────────────────┤
    │                               │
    │  CMD_DEACTIVATE               │
    ├──────────────────────────────>│
    │                               │ rocprofiler_stop_context(ctx)
    │  {OK, events: 42500}         │
    │<──────────────────────────────┤
    │                               │
    │  close()                      │
    ├──────────────────────────────>│
```

## Integration with rocprofiler-sdk

Same as all options — the tool uses standard rocprofiler-sdk APIs including `rocprofiler_force_configure()` for **late configuration**. See [Option B](OPTION_B_MMAP_FILE.md#what-changes-minimal) for the full late-configuration design (placeholder configure at process start, real configure invoked at attach via `rocprofiler_force_configure`, propagation re-runs `update_table` to install wrappers for the requested domains).

The **only difference from Option B** is the IPC mechanism: instead of an mmap'd file polled by a background thread, this option uses a Unix domain socket where the background thread blocks on `accept()`/`recv()` and responds to commands directly. The protocol carries `CMD_CONFIGURE` (with full `rocp_config_t` payload), `CMD_ACTIVATE`, `CMD_DEACTIVATE`, and `CMD_STATUS`.

**Advantage over Option B**: the socket is inherently bidirectional, so the controller can synchronously wait for `CMD_CONFIGURE` to complete (including `force_configure` and propagation, ~5-50 ms) and receive a confirmation that wrappers are installed.

## Components

### 1. Tool Library (socket control channel setup)

The `rocprofiler_configure` and context setup are identical to Option B. Only the control channel differs:

```c
/* In tool_initialize — after creating context and registering domains */
static void setup_socket_control(rocprofiler_context_id_t ctx) {
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    addr.sun_path[0] = '\0';
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
             "rocprofiler_%d", getpid());
    bind(sock, (struct sockaddr*)&addr,
         offsetof(struct sockaddr_un, sun_path) + 1 +
         strlen(addr.sun_path + 1));
    listen(sock, 1);
    listen_fd_global = sock;
    saved_ctx = ctx;

    pthread_create(&control_thread, NULL, control_loop,
                   (void*)(intptr_t)sock);
}
```

### 2. Control Loop (background thread — receives commands, toggles context)

```c
static void* control_loop(void* arg) {
    int listen_fd = (intptr_t)arg;

    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);

    while (1) {
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) break;

        // Authenticate via SO_PEERCRED
        struct ucred cred;
        socklen_t len = sizeof(cred);
        getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len);
        if (cred.uid != getuid()) { close(client); continue; }

        // Process commands
        rocp_cmd_t cmd;
        while (recv(client, &cmd, sizeof(cmd), 0) > 0) {
            rocp_response_t resp = {0};
            switch (cmd.type) {
            case CMD_ACTIVATE:
                rocprofiler_start_context(saved_ctx);  // EXISTING API
                resp.status = 0;
                resp.context_active = 1;
                break;
            case CMD_DEACTIVATE:
                rocprofiler_stop_context(saved_ctx);   // EXISTING API
                resp.status = 0;
                resp.context_active = 0;
                break;
            case CMD_STATUS:
                resp.status = 0;
                /* read stats from context */
                break;
            }
            send(client, &resp, sizeof(resp), 0);
        }
        close(client);
    }
    return NULL;
}
```

### 3. Authentication Detail

```c
// SO_PEERCRED returns:
struct ucred {
    pid_t pid;    // PID of connecting process
    uid_t uid;    // UID of connecting process
    gid_t gid;    // GID of connecting process
};

// The kernel fills these from the connecting process's *effective* credentials
// (effective UID/GID, not real UID/GID) at connect() time. Even if the
// connecting process tries to forge them, the kernel overwrites with the
// actual values. The UID/GID are unforgeable. The PID is accurate at
// connection time but may become stale if the peer exits and the PID is reused.
```

### 4. Controller

```c
int main(int argc, char** argv) {
    pid_t target_pid = parse_args(argc, argv);
    const char* action = parse_action(argc, argv);  // "activate" or "deactivate"

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    addr.sun_path[0] = '\0';
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
             "rocprofiler_%d", target_pid);

    if (connect(sock, (struct sockaddr*)&addr, ...) < 0) {
        fprintf(stderr, "Cannot connect to PID %d\n", target_pid);
        return 1;
    }

    rocp_cmd_t cmd = {0};
    if (strcmp(action, "activate") == 0)
        cmd.type = CMD_ACTIVATE;
    else if (strcmp(action, "deactivate") == 0)
        cmd.type = CMD_DEACTIVATE;
    else
        cmd.type = CMD_STATUS;

    send(sock, &cmd, sizeof(cmd), 0);
    rocp_response_t resp;
    recv(sock, &resp, sizeof(resp), 0);
    printf("Context active: %u, events: %lu\n",
           resp.context_active, resp.events_traced);
    return 0;
}
```

### 5. Finalization (atexit, rocprofiler-sdk pattern)

```c
static _Atomic int finalize_status = 0;

static void tool_finalize(void* tool_data) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&finalize_status, &expected, -1,
                                     0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;

    rocprofiler_stop_context(saved_ctx);

    if (listen_fd_global >= 0) {
        close(listen_fd_global);
        listen_fd_global = -1;
    }
    pthread_join(control_thread, NULL);

    __atomic_store_n(&finalize_status, 1, __ATOMIC_SEQ_CST);
}
```

## Security Analysis

| Property | Assessment |
|----------|------------|
| **Authentication** | Strongest of all options — `SO_PEERCRED` provides kernel-verified effective UID, GID, PID at connect time |
| **Identity forging** | UID/GID are unforgeable. PID is accurate at connect time but may become stale on peer exit |
| **Other-user access** | Blocked — tool rejects connections where `cred.uid != getuid()` |
| **Abstract namespace** | No filesystem entry — nothing to race, nothing to pre-create, nothing to symlink. However, any process in the same network namespace can *attempt* to connect (probing for socket existence). The SO_PEERCRED check in accept rejects unauthorized connections, but socket existence is discoverable |
| **Auto-cleanup** | Abstract sockets vanish when the last fd closes (process exit) |
| **Container isolation** | Abstract sockets are scoped to the network namespace |
| **PID allowlisting** | Optional: tool can also check `cred.pid` against an expected controller PID |

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Tool init | ~10-15 μs | `socket` + `bind` + `listen` + `pthread_create` |
| Controller connect | ~3-5 μs | `socket` + `connect` |
| SO_PEERCRED check | ~1 μs | `getsockopt` |
| Command send/recv | ~1-5 μs | Per command (kernel copies data between socket buffers) |
| **Hot-path (noop)** | **~10-20 ns** | Existing `populate_contexts()` — context inactive |
| **Hot-path (tracing)** | **~50-200 ns** | `populate_contexts()` + callbacks + buffer emplace |
| Context toggle | ~5 μs | Socket recv + `rocprofiler_start/stop_context()` |
| Thread idle overhead | ~0 | Thread blocked on `accept()`, no CPU |

## Multi-Runtime Application (rocprofiler-sdk)

Since the tool uses rocprofiler-sdk's context system, multi-runtime support is the same as Option B: a single context covers all registered domains. A single `rocprofiler_start_context(ctx)` activates tracing for HIP, HSA, RCCL, OMPT, etc. simultaneously.

The socket's bidirectional nature adds value for status queries:

```
Controller → Tool:  CMD_STATUS
Tool → Controller:  {active: true, events_traced: 42000, ...}
```

### Bidirectional Queries

Unlike Option B (shared memory status fields), the socket naturally supports rich queries with structured responses. This is useful for `rocprofv3`-style tools that want to display what's being traced and how many events have been collected.

### OpenMP Integration

OMPT is registered at init time via `OMP_TOOL_LIBRARIES`. OMPT callbacks register a rocprofiler context with `ROCPROFILER_CALLBACK_TRACING_OMPT` domain. When the controller sends `CMD_ACTIVATE`, `rocprofiler_start_context()` activates the context and `populate_contexts()` starts finding it — OMPT callbacks begin recording alongside HIP/HSA events. See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#openmp-ompt-always-enabled-shim-with-noop-control) for the full design.

## File Layout

```
src/tools/rocprofiler_tool_sock/
├── rocp_sock_protocol.h    # Command types, message structs (rocp_cmd_t, rocp_response_t)
├── rocp_sock_tool.c        # rocprofiler tool library (configure + initialize + socket setup)
└── rocp_sock_controller.c  # CLI controller tool
```

## Build Integration

```cmake
option(BUILD_ROCP_TOOL_SOCK "Build rocprofiler tool with socket control channel" ON)

if(BUILD_ROCP_TOOL_SOCK)
    add_library(rocprofiler_tool_sock SHARED
        src/tools/rocprofiler_tool_sock/rocp_sock_tool.c
    )
    target_link_libraries(rocprofiler_tool_sock PRIVATE pthread)
    target_compile_options(rocprofiler_tool_sock PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_sock
        src/tools/rocprofiler_tool_sock/rocp_sock_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Noop overhead (tool loaded, context not activated):
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_sock.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing:
# Terminal 1:
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_sock.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/rocp_ctrl_sock --pid $! activate
# ... tracing active ...
build/bin/rocp_ctrl_sock --pid $! status
build/bin/rocp_ctrl_sock --pid $! deactivate
```

## Limitations

1. **Background thread** — Default pthread stack is ~2 MB (configurable). Thread must be joinable so `tool_finalize` can `pthread_join()` it.
2. **fork() handling** — After `fork()`, the background thread is gone. `pthread_atfork()` child handler must close the listen fd and set `finalize_status = 1` so the child's atexit handler skips cleanup.
3. **Socket buffer limits** — Large response payloads may require multiple `send()`/`recv()` calls with framing.
4. **Abstract namespace portability** — Abstract Unix sockets are Linux-specific (not available on macOS/BSD). For cross-platform, fall back to filesystem sockets.
5. **Context toggle latency** — ~5 μs per activate/deactivate (socket round-trip) vs ~1 ms (poll interval) for Option B. Socket is faster for toggle but has higher per-command overhead.
6. **Single controller** — `listen(sock, 1)` with serial `handle_client()` means only one controller at a time. Limits multi-tool scenarios.
7. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall costs and should be validated with benchmarks.
