# Option F: Dispatch Tracer with Unix Domain Socket Control Channel

## Overview

This design uses a **Unix domain socket** (abstract namespace) as the control channel. The LD_PRELOAD library creates a listener socket and a background thread. The controller connects, is authenticated via `SO_PEERCRED` (kernel-verified, unforgeable UID/GID/PID), and sends structured commands (enable, disable, configure, query status). The hot path uses a **process-local atomic variable** — no socket I/O per intercepted call.

This option provides the **strongest authentication model** of all options: the kernel verifies the connecting process's identity, and this verification cannot be forged.

## Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                  │
│                                                                 │
│  LD_PRELOAD=libmylib_dispatch_sock.so                          │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Main Thread (application code)                           │  │
│  │                                                           │  │
│  │  hot path:                                                │  │
│  │    if (__atomic_load_n(&local_enabled, __ATOMIC_ACQUIRE)) │  │
│  │        trace(func_id, args);                              │  │
│  │    real_fn(args);                                         │  │
│  │                                                           │  │
│  │  // No socket I/O here — existing populate_contexts()    │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Background Thread (control plane)                        │  │
│  │                                                           │  │
│  │  sock = socket(AF_UNIX, SOCK_STREAM, 0)                  │  │
│  │  bind(sock, "\0dispatch_<pid>")  // abstract namespace    │  │
│  │  listen(sock, 1)                                          │  │
│  │                                                           │  │
│  │  loop:                                                    │  │
│  │    client = accept(sock)                                  │  │
│  │    getsockopt(client, SO_PEERCRED, &cred)                │  │
│  │    if (cred.uid != getuid()) { close(client); continue; }│  │
│  │                                                           │  │
│  │    recv(client, &cmd)                                     │  │
│  │    switch (cmd.type):                                     │  │
│  │      CMD_ENABLE:                                          │  │
│  │        apply_config(cmd.config);                          │  │
│  │        atomic_store(&local_enabled, 1);                   │  │
│  │        send(client, {status: OK, apis_traced: N});        │  │
│  │      CMD_DISABLE:                                         │  │
│  │        atomic_store(&local_enabled, 0);                   │  │
│  │        flush_buffers();                                   │  │
│  │        send(client, {status: OK, events: count});         │  │
│  │      CMD_STATUS:                                          │  │
│  │        send(client, {enabled, event_count, ...});         │  │
│  │      CMD_CONFIGURE:                                       │  │
│  │        update_filters(cmd.config);                        │  │
│  │        send(client, {status: OK});                        │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  Abstract socket: \0dispatch_12345 (no filesystem entry)       │
└──────────────────────┬─────────────────────────────────────────┘
                       │ Unix domain socket (abstract namespace)
┌──────────────────────▼─────────────────────────────────────────┐
│                    Controller (dispatch_ctrl_sock)               │
│                                                                 │
│  sock = socket(AF_UNIX, SOCK_STREAM, 0)                        │
│  connect(sock, "\0dispatch_<pid>")                              │
│                                                                 │
│  // Send enable command with configuration:                     │
│  cmd = {                                                        │
│    .type = CMD_ENABLE,                                          │
│    .config = {                                                  │
│      .filter = "my_traced_*",                                   │
│      .output_format = OUTPUT_JSON,                              │
│      .ring_buffer_size = 4 * 1024 * 1024,                      │
│    }                                                            │
│  };                                                             │
│  send(sock, &cmd, sizeof(cmd), 0);                             │
│  recv(sock, &response, sizeof(response), 0);                   │
│  printf("Attached: %d APIs traced\n", response.apis_traced);   │
│                                                                 │
│  // Wait, then detach:                                          │
│  cmd.type = CMD_DISABLE;                                        │
│  send(sock, &cmd, sizeof(cmd), 0);                             │
│  recv(sock, &response, sizeof(response), 0);                   │
│  printf("Events: %lu\n", response.events_traced);              │
└────────────────────────────────────────────────────────────────┘
```

## Protocol Design

### Command Types

```c
enum dispatch_cmd_type {
    CMD_ENABLE     = 1,   // Enable tracing with config
    CMD_DISABLE    = 2,   // Disable tracing, flush buffers
    CMD_CONFIGURE  = 3,   // Update config without restart
    CMD_STATUS     = 4,   // Query current state
    CMD_LIST_FUNCS = 5,   // List discovered functions
};
```

### Message Format

```c
typedef struct {
    uint32_t type;          // dispatch_cmd_type
    uint32_t payload_len;   // Length of payload in bytes
    /* Variable-length payload follows */
} dispatch_cmd_header_t;

/* CMD_ENABLE / CMD_CONFIGURE payload */
typedef struct {
    uint32_t output_format;              // TEXT=0, JSON=1, PERFETTO=2
    uint32_t ring_buffer_size;           // Bytes
    uint32_t func_enable_mask[16];       // 512-bit mask
    char filter_pattern[256];            // Glob include
    char exclude_pattern[256];           // Glob exclude
    char output_path[256];
} dispatch_config_payload_t;

/* Response from library to controller */
typedef struct {
    uint32_t status;         // OK=0, ERROR=1, PARTIAL=2
    uint32_t payload_len;
    /* Variable-length payload follows */
} dispatch_response_header_t;

/* CMD_ENABLE response payload */
typedef struct {
    uint32_t functions_discovered;
    uint32_t functions_enabled;
} dispatch_enable_response_t;

/* CMD_STATUS response payload */
typedef struct {
    uint32_t tracing_enabled;
    uint32_t functions_enabled;
    uint64_t events_traced;
    uint64_t events_dropped;
    uint64_t uptime_ns;
} dispatch_status_response_t;
```

### Protocol Flow

```
Controller                      Library (bg thread)
    │                               │
    │  connect("\0dispatch_<pid>")  │
    ├──────────────────────────────>│
    │                               │ accept() + SO_PEERCRED check
    │                               │
    │  CMD_ENABLE + config          │
    ├──────────────────────────────>│
    │                               │ apply config
    │                               │ atomic_store(&local_enabled, 1)
    │     {OK, funcs_enabled: 1}   │
    │<──────────────────────────────┤
    │                               │
    │  CMD_STATUS                   │
    ├──────────────────────────────>│
    │  {enabled:1, events:42000}   │
    │<──────────────────────────────┤
    │                               │
    │  CMD_DISABLE                  │
    ├──────────────────────────────>│
    │                               │ atomic_store(&local_enabled, 0)
    │                               │ flush ring buffers
    │  {OK, events_flushed: 42500} │
    │<──────────────────────────────┤
    │                               │
    │  close()                      │
    ├──────────────────────────────>│
```

## Integration with rocprofiler-sdk

Same as all options — the tool uses standard rocprofiler-sdk APIs (`rocprofiler_configure`, `rocprofiler_create_context`, `rocprofiler_configure_callback_tracing_service`, `rocprofiler_start_context` / `rocprofiler_stop_context`). The existing dispatch table machinery (`copy_table`/`update_table`/`functor`) is reused as-is. See [Option B](OPTION_B_MMAP_FILE.md#what-reuses-existing-rocprofiler-sdk-code-no-changes) for the full list of reused vs new code.

The **only difference from Option B** is the IPC mechanism: instead of an mmap'd file, this option uses a Unix domain socket. The tool's `initialize` callback sets up the socket instead of the mmap file.

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
        dispatch_cmd_header_t cmd;
        while (recv(client, &cmd, sizeof(cmd), 0) > 0) {
            switch (cmd.type) {
            case CMD_ENABLE:
                rocprofiler_start_context(saved_ctx);  // EXISTING API
                send_response(client, STATUS_OK);
                break;
            case CMD_DISABLE:
                rocprofiler_stop_context(saved_ctx);   // EXISTING API
                send_response(client, STATUS_OK);
                break;
            case CMD_STATUS:
                send_status(client, saved_ctx);
                break;
            }
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

### 4. Controller (`dispatch_ctrl_sock.c`)

```c
int main(int argc, char** argv) {
    pid_t target_pid = parse_args(argc, argv);
    const char* action = parse_action(argc, argv);

    // Connect to target's abstract socket
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    addr.sun_path[0] = '\0';
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
             "dispatch_%d", target_pid);

    if (connect(sock, (struct sockaddr*)&addr, ...) < 0) {
        fprintf(stderr, "Cannot connect to PID %d "
                "(is the dispatch tracer loaded?)\n", target_pid);
        return 1;
    }

    if (strcmp(action, "enable") == 0) {
        dispatch_cmd_header_t cmd = { .type = CMD_ENABLE };
        dispatch_config_payload_t config = {
            .output_format = OUTPUT_TEXT,
            .ring_buffer_size = 4 * 1024 * 1024,
        };
        memset(config.func_enable_mask, 0xFF, sizeof(config.func_enable_mask));
        cmd.payload_len = sizeof(config);
        send(sock, &cmd, sizeof(cmd), 0);
        send(sock, &config, sizeof(config), 0);

        dispatch_response_header_t resp;
        recv(sock, &resp, sizeof(resp), 0);
        // ...
    }
    // ... handle other actions
}
```

## Security Analysis

| Property | Assessment |
|----------|------------|
| **Authentication** | Strongest of all options — `SO_PEERCRED` provides kernel-verified effective UID, GID, PID at connect time |
| **Identity forging** | UID/GID are unforgeable. PID is accurate at connect time but may become stale on peer exit |
| **Other-user access** | Blocked — library rejects connections where `cred.uid != getuid()` |
| **Abstract namespace** | No filesystem entry — nothing to race, nothing to pre-create, nothing to symlink. However, any process in the same network namespace can *attempt* to connect (probing for socket existence). The SO_PEERCRED check in accept rejects unauthorized connections, but socket existence is discoverable |
| **Auto-cleanup** | Abstract sockets vanish when the last fd closes (process exit) |
| **Container isolation** | Abstract sockets are scoped to the network namespace |
| **PID allowlisting** | Optional: library can also check `cred.pid` against an expected controller PID |

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Library init | ~10-15 μs | `socket` + `bind` + `listen` + `pthread_create` |
| Controller connect | ~3-5 μs | `socket` + `connect` |
| SO_PEERCRED check | ~1 μs | `getsockopt` |
| Command send/recv | ~1-5 μs | Per command (kernel copies data between socket buffers) |
| **Hot-path (noop)** | **~10-20 ns** | Existing `populate_contexts()` — context inactive |
| **Hot-path (tracing)** | **~50-200 ns** | `populate_contexts()` + callbacks + buffer emplace |
| Config change | ~2-5 μs | Socket recv + apply + atomic store |
| Thread idle overhead | ~0 | Thread blocked on `accept()`, no CPU |

## Multi-Runtime Application (rocprofiler-sdk)

The socket protocol naturally supports multi-runtime configuration in a single session:

```c
/* CMD_ENABLE payload for multi-runtime */
typedef struct {
    uint32_t runtime_count;
    struct {
        uint32_t runtime_id;    // HIP=0, HSA=1, RCCL=2, OMPT=3, ...
        uint32_t enabled;
        uint32_t func_mask[16]; // 512-bit per-function mask
        char filter[256];
        char exclude[256];
    } runtimes[];
} dispatch_multi_config_t;
```

The controller sends one `CMD_ENABLE` with all runtime configurations. The library applies them atomically (all configs written before the master enable flag is set).

### Bidirectional Queries

Unlike Options A/B (shared memory), the socket naturally supports rich queries:

```
Controller → Library:  CMD_LIST_FUNCS {runtime: HIP}
Library → Controller:  {count: 512, funcs: ["hipMalloc", "hipMemcpy", ...]}

Controller → Library:  CMD_STATUS
Library → Controller:  {enabled: true, hip_events: 42000, hsa_events: 1200, ...}
```

This is particularly useful for `rocprofv3`-style tools that want to display what's being traced and how many events have been collected.

### OpenMP Integration

OMPT is started **enabled at init time** via `OMP_TOOL_LIBRARIES`, but all callbacks are noop by default — each checks the same `local_enabled` atomic flag. When the controller sends `CMD_ENABLE`, OMPT callbacks begin recording alongside HIP/HSA events. The socket protocol's `CMD_ENABLE` payload includes a runtime bitmask that can independently enable/disable OpenMP tracing:

```c
/* In dispatch_multi_config_t: */
struct {
    uint32_t runtime_id;    // HIP=0, HSA=1, RCCL=2, OMPT=3, ...
    uint32_t enabled;       // Per-runtime enable
    ...
} runtimes[];
```

The OMPT tool library ships as `libdispatch_ompt_tool.so` and plugs into the same socket-based control channel. See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#openmp-ompt-always-enabled-shim-with-noop-control) for the full noop-shim design.

## File Layout

```
src/tools/dispatch_tracer_sock/
├── dispatch_sock_protocol.h    # Command types, message structs
├── dispatch_sock_wrapper.c     # LD_PRELOAD library + control thread
├── dispatch_sock_trace.c       # Tracing logic (timestamps, ring buffer)
└── dispatch_sock_controller.c  # CLI controller tool
```

## Build Integration

```cmake
option(BUILD_DISPATCH_SOCK "Build dispatch table tracer (unix socket)" ON)

if(BUILD_DISPATCH_SOCK)
    add_library(mylib_dispatch_sock SHARED
        src/tools/dispatch_tracer_sock/dispatch_sock_wrapper.c
        src/tools/dispatch_tracer_sock/dispatch_sock_trace.c
    )
    target_link_libraries(mylib_dispatch_sock PRIVATE dl pthread)
    target_compile_options(mylib_dispatch_sock PRIVATE -O2 -fPIC)

    add_executable(dispatch_ctrl_sock
        src/tools/dispatch_tracer_sock/dispatch_sock_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Noop overhead:
LD_PRELOAD=build/lib/libmylib_dispatch_sock.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing:
# Terminal 1:
LD_PRELOAD=build/lib/libmylib_dispatch_sock.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/dispatch_ctrl_sock --pid $! enable
# ... tracing active ...
build/bin/dispatch_ctrl_sock --pid $! disable
build/bin/dispatch_ctrl_sock --pid $! status
```

## Limitations

1. **Background thread** — Default pthread stack is ~2 MB (configurable). Thread must be joinable so `tool_finalize` can `pthread_join()` it.
2. **fork() handling** — After `fork()`, the background thread is gone. `pthread_atfork()` child handler must close the listen fd, reset `local_enabled = 0`, and set `finalize_status = 1` so the child's atexit handler skips cleanup.
5. **Socket buffer limits** — Large config payloads may require multiple `send()`/`recv()` calls with framing.
6. **Abstract namespace portability** — Abstract Unix sockets are Linux-specific (not available on macOS/BSD). For cross-platform, fall back to filesystem sockets.
7. **Config update latency** — ~2-5 μs per config change (socket round-trip) vs ~50-100 ns for mmap-based options. Irrelevant for attach/detach but noticeable for high-frequency config updates.
8. **Single controller** — `listen(sock, 1)` with serial `handle_client()` means only one controller can be connected at a time. A second connection attempt will queue (backlog=1) or be refused. This is intentional for simplicity but limits multi-tool scenarios.
9. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall costs and should be validated with benchmarks after implementation.
