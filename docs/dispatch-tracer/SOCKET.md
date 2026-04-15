# socket: Dispatch Tracer with Unix Domain Socket Control Channel

## Overview

This design uses a **Unix domain socket** (abstract namespace) as the control channel between a stub library inside the target process and an external controller process. The stub creates a listener socket and a background thread during its constructor. The controller connects, is authenticated via `SO_PEERCRED` (kernel-verified, unforgeable UID/GID), and sends structured commands (configure, activate, deactivate, reconfigure, query status). The hot path is entirely within rocprofiler-sdk's existing `populate_contexts()` — no socket I/O per intercepted call.

This option provides the **strongest authentication model** of all options: the kernel verifies the connecting process's identity, and this verification cannot be forged.

## Integration with rocprofiler-sdk

> **Precise reading of "preloaded":** `LD_PRELOAD=librocp_stub_sock.so` — only the stub. rocprofiler-register is already a `DT_NEEDED` dependency of HIP/HSA/OpenMP/RCCL (auto-loaded); rocprofiler-sdk is neither preloaded nor linked and is only `dlopen`'d at attach. OMPT will be handled via a silent `ompt_start_tool` in the stub (planned — see survey § OMPT for status). See [CONTROL_CHANNEL_SURVEY.md § What Exactly Gets LD_PRELOAD'd](CONTROL_CHANNEL_SURVEY.md#what-exactly-gets-ld_preloadd--and-what-does-not) and [§ OpenMP / OMPT](CONTROL_CHANNEL_SURVEY.md#openmp--ompt--a-different-registration-path).

Same as all options — uses the **late-load design** described in [mmap](MMAP.md#what-changes-minimal--late-load-architecture):

- A small **stub library** is preloaded (via `LD_PRELOAD`) that does NOT export `rocprofiler_configure`, so rocprofiler-register doesn't load rocprofiler-sdk → 0 ns hot path before any attach
- On `CMD_CONFIGURE`, the stub `dlopen`s the tool library and calls `rocprofiler_force_configure()` (succeeds because SDK `init_status` is still 0)
- SDK initializes with the controller-specified domains, propagation runs, wrappers install only for the requested operations

The **only difference from mmap** is the IPC mechanism: a Unix domain socket where the stub's background thread blocks on `accept()`/`recv()` and responds to commands directly. The protocol carries `CMD_CONFIGURE` (full config payload), `CMD_ACTIVATE`, `CMD_DEACTIVATE`, `CMD_RECONFIGURE`, and `CMD_STATUS`.

**Advantage over mmap**: the socket is inherently bidirectional, so the controller can synchronously wait for `CMD_CONFIGURE` to complete (including dlopen + force_configure + propagation, ~1-2 ms (mock; real SDK dlopen would make this ~5-50 ms)) and receive a confirmation ACK with event counters.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Target Process                           │
│                                                              │
│  Process start (no controller attached):                     │
│                                                              │
│  HIP/HSA/RCCL runtimes load → link rocprofiler-register      │
│  Stub library loaded via LD_PRELOAD                          │
│    (NO rocprofiler_configure symbol exported)                │
│  Stub setup:                                                 │
│    bind abstract socket \0rocprofiler_<pid>                  │
│    listen(sock, 5)                                           │
│    spawn background thread blocked on accept()               │
│                                                              │
│  Runtime calls rocprofiler_register_library_api_table(...)   │
│    rocprofiler-register stores table pointer                 │
│    rocprofiler-register scans for rocprofiler_configure      │
│      → not found (only stub is loaded) → does NOT load SDK   │
│  Original function pointers stay in dispatch tables          │
│  Hot path: 0 ns (no wrappers, no SDK code)                   │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Existing functor hot path (NO CHANGES):                │ │
│  │                                                         │ │
│  │  hip_api_impl<T,Op>::functor(args...):                  │ │
│  │    populate_contexts(domain, op,                        │ │
│  │        callback_ctxs, buffered_ctxs);                   │ │
│  │    if (callback_ctxs.empty() && buffered_ctxs.empty())  │ │
│  │        return exec(get_table_func(), args);  // noop    │ │
│  │    // ... full tracing path (callbacks, buffers, etc.)  │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Background thread (NEW — accepts socket connections):  │ │
│  │                                                         │ │
│  │  loop:                                                  │ │
│  │    client = accept(listen_fd)                           │ │
│  │    SO_PEERCRED → verify cred.uid == getuid()            │ │
│  │    while recv(client, &cmd) > 0:                        │ │
│  │      switch (cmd.type):                                 │ │
│  │        CMD_CONFIGURE:                                   │ │
│  │          if (!sdk_loaded):                              │ │
│  │            stash config, dlopen tool library,               │ │
│  │            rocprofiler_force_configure(tool_configure)  │ │
│  │          else: apply_runtime_filter(cfg)                │ │
│  │          send {OK, ctx_id, ...}                         │ │
│  │        CMD_ACTIVATE:                                    │ │
│  │          rocprofiler_start_context(saved_ctx)           │ │
│  │          send {OK, active:1}                            │ │
│  │        CMD_DEACTIVATE:                                  │ │
│  │          rocprofiler_stop_context(saved_ctx)            │ │
│  │          send {OK, events_traced}                       │ │
│  │        CMD_RECONFIGURE:                                 │ │
│  │          apply_runtime_filter(cfg); send {OK}           │ │
│  │        CMD_STATUS:                                      │ │
│  │          send {active, events_traced, events_dropped}   │ │
│  │    close(client)                                        │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                              │
│  Abstract socket: \0rocprofiler_12345 (no filesystem entry) │
└──────────────────┬──────────────────────────────────────────┘
                   │ Unix domain socket (abstract namespace)
┌──────────────────▼──────────────────────────────────────────┐
│  Controller (e.g., rocprofv3 --attach --channel sock)       │
│                                                              │
│  sock = socket(AF_UNIX, SOCK_STREAM)                         │
│  connect("\0rocprofiler_<pid>")                              │
│                                                              │
│  // ATTACH (first time — triggers dlopen + force_configure): │
│  send {CMD_CONFIGURE, config={hip, hsa, output=json, ...}}  │
│  recv {OK, ctx_id}        // blocks until SDK initialized   │
│                                                              │
│  // TOGGLE:                                                  │
│  send {CMD_ACTIVATE}    ; recv {OK, active:1}                │
│  send {CMD_STATUS}      ; recv {active:1, events:42000}      │
│  send {CMD_DEACTIVATE}  ; recv {OK, events:42500}            │
└─────────────────────────────────────────────────────────────┘
```

## Protocol Design

### Command Types

The command enum is the canonical set shared by all late-load options —
see [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#late-load-design-defer-rocprofiler-sdk-loading-until-attach) for the full definition:

```
CMD_NONE        = 0   // No pending command / reserved
CMD_CONFIGURE   = 1   // First attach: dlopen tool library (brings rocprofiler-sdk via link dep) + force_configure with config
CMD_ACTIVATE    = 2   // rocprofiler_start_context() (after configure)
CMD_DEACTIVATE  = 3   // rocprofiler_stop_context() (wrappers stay, Level 2 noop)
CMD_RECONFIGURE = 4   // Update runtime filter (domains-to-emit, output) without re-arming
CMD_STATUS      = 5   // Query current state / counters
```

### Message Format

Only the protocol-specific wire framing is defined here. The config payload
(`rocp_config_t`) is identical to mmap.

```c
/* Request: controller → tool */
typedef struct {
    uint32_t type;          // enum rocp_ctrl_command (see survey)
    uint32_t flags;         // Reserved
    rocp_config_t config;   // Only meaningful for CMD_CONFIGURE / CMD_RECONFIGURE
} rocp_cmd_t;

/* Response: tool → controller */
typedef struct {
    uint32_t status;         // OK=0, ERROR=1
    uint32_t context_active; // 0 = inactive, 1 = active
    uint32_t context_id;     // rocprofiler_context_id_t.handle (0 if not configured)
    uint32_t reserved;
    uint64_t events_traced;
    uint64_t events_dropped;
} rocp_response_t;
```

### Protocol Flow

```
Controller                      Stub bg thread
    │                               │
    │  connect("\0rocprofiler_<pid>")│
    ├──────────────────────────────>│
    │                               │ accept() + SO_PEERCRED check
    │                               │
    │  CMD_CONFIGURE {config}       │
    ├──────────────────────────────>│ dlopen tool lib →
    │                               │ rocprofiler_force_configure() →
    │                               │ SDK init, wrappers install,
    │                               │ tool_initialize creates ctx
    │     {OK, ctx_id}              │ (blocks ~1-2 ms (mock; real SDK dlopen would make this ~5-50 ms))
    │<──────────────────────────────┤
    │                               │
    │  CMD_ACTIVATE                 │
    ├──────────────────────────────>│ rocprofiler_start_context(saved_ctx)
    │     {OK, active:1}            │
    │<──────────────────────────────┤
    │                               │
    │  CMD_STATUS                   │
    ├──────────────────────────────>│
    │  {OK, active:1, events:42000} │
    │<──────────────────────────────┤
    │                               │
    │  CMD_DEACTIVATE               │
    ├──────────────────────────────>│ rocprofiler_stop_context(saved_ctx)
    │  {OK, events:42500}           │
    │<──────────────────────────────┤
    │                               │
    │  close()                      │
    ├──────────────────────────────>│
```

## Components

### 1. Stub Library (`librocp_stub_sock.so` — preloaded, NO rocprofiler_configure symbol)

The stub is loaded via `LD_PRELOAD` at process start. It does NOT export `rocprofiler_configure`, so rocprofiler-register's symbol scan finds no tool and does NOT load rocprofiler-sdk. The stub only sets up the socket and waits.

```c
/* Loaded at process start via LD_PRELOAD.
 * Binds the abstract-namespace socket and spawns the bg thread.
 * Does NOT load rocprofiler-sdk. */
__attribute__((constructor))
static void stub_init(void) {
    setup_socket_control();
}

static int listen_fd_global = -1;
static pthread_t control_thread;
static rocp_config_t g_pending_config;   /* read by tool_initialize via accessor */
static rocprofiler_context_id_t saved_ctx;
static _Atomic bool sdk_loaded = false;
static _Atomic uint32_t context_active = 0;
static _Atomic uint64_t events_traced = 0;
static _Atomic uint64_t events_dropped = 0;
static void* sdk_handle = NULL;

/* Exported accessor — tool calls this after being dlopen'd.
 * Avoids extern cross-DSO globals (which require RTLD_GLOBAL
 * and can fail silently if the load order is wrong). */
typedef struct {
    rocp_config_t* pending_config;
    rocprofiler_context_id_t* saved_ctx;
    _Atomic uint32_t* context_active;
    _Atomic uint64_t* events_traced;
    _Atomic uint64_t* events_dropped;
} rocp_stub_state_t;

__attribute__((visibility("default")))
const rocp_stub_state_t* rocp_stub_get_state(void) {
    static rocp_stub_state_t state = {
        .pending_config = &g_pending_config,
        .saved_ctx      = &saved_ctx,
        .context_active = &context_active,
        .events_traced  = &events_traced,
        .events_dropped = &events_dropped,
    };
    return &state;
}

/* Function pointers resolved after dlopen of rocprofiler-sdk */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t);
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t);
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t);

static void setup_socket_control(void) {
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    addr.sun_path[0] = '\0';
    int n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                     "rocprofiler_%d", getpid());
    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path) + 1 + n;
    bind(sock, (struct sockaddr*)&addr, addrlen);

    /* backlog=5: allow attach-detach-reattach cycles where a new controller
     * may connect before the previous one has been fully drained. */
    listen(sock, 5);

    listen_fd_global = sock;
    pthread_create(&control_thread, NULL, control_loop,
                   (void*)(intptr_t)sock);
}
```

### 2. Background Thread (accepts connections, dlopens the tool library on CMD_CONFIGURE)

```c
/* On first CMD_CONFIGURE: dlopen rocprofiler-sdk-tool, call force_configure.
 * The tool library exports rocprofiler_configure which the SDK will invoke. */
static void load_sdk_and_configure(void) {
    /* RTLD_GLOBAL is REQUIRED so the tool's rocprofiler_configure becomes
     * visible to the SDK's dlsym(RTLD_DEFAULT, "rocprofiler_configure")
     * scan during initialize(). */
    sdk_handle = dlopen("librocprofiler-sdk-tool.so", RTLD_NOW | RTLD_GLOBAL);
    if (!sdk_handle) {
        fprintf(stderr, "Failed to dlopen rocprofiler-sdk-tool: %s\n", dlerror());
        return;
    }

    /* Resolve symbols via RTLD_DEFAULT (POSIX-portable, searches all
     * loaded libs) instead of dlsym(sdk_handle, ...) which has
     * implementation-specific scope rules. */
    typedef rocprofiler_tool_configure_result_t* (*configure_fn_t)(
        uint32_t, const char*, uint32_t, rocprofiler_client_id_t*);
    configure_fn_t tool_configure = dlsym(RTLD_DEFAULT, "rocprofiler_configure");
    p_force_configure = dlsym(RTLD_DEFAULT, "rocprofiler_force_configure");
    p_start_context   = dlsym(RTLD_DEFAULT, "rocprofiler_start_context");
    p_stop_context    = dlsym(RTLD_DEFAULT, "rocprofiler_stop_context");

    if (!p_force_configure || !tool_configure) {
        fprintf(stderr, "Failed to resolve SDK/tool symbols\n");
        return;
    }

    /* Pass an explicit configure_func (NOT NULL). NULL would rely on
     * an internal symbol scan that may silently miss the tool. */
    rocprofiler_status_t st = p_force_configure(tool_configure);
    if (st != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr, "rocprofiler_force_configure failed: %d\n", st);
    }
    /* tool_initialize has now run and called rocprofiler_start_context,
     * so context_active is already 1. */
}

static void* control_loop(void* arg) {
    int listen_fd = (intptr_t)arg;

    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);

    while (1) {
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) break;

        /* Authenticate via SO_PEERCRED (kernel-verified, unforgeable) */
        struct ucred cred;
        socklen_t len = sizeof(cred);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
            cred.uid != getuid()) {
            close(client); continue;
        }

        rocp_cmd_t cmd;
        while (recv(client, &cmd, sizeof(cmd), 0) == sizeof(cmd)) {
            rocp_response_t resp = {0};
            switch (cmd.type) {
            case CMD_CONFIGURE: {
                bool expected = false;
                if (__atomic_compare_exchange_n(&sdk_loaded, &expected, true,
                                                false, __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE)) {
                    /* First attach: stash config and dlopen the tool library.
                     * tool_initialize will read g_pending_config. */
                    memcpy(&g_pending_config, &cmd.config,
                           sizeof(g_pending_config));
                    load_sdk_and_configure();
                } else {
                    /* SDK already loaded — update runtime filter only.
                     * force_configure is locked; cannot add new domains. */
                    apply_runtime_filter(&cmd.config);
                }
                resp.status = 0;
                resp.context_id = saved_ctx.handle;
                resp.context_active =
                    __atomic_load_n(&context_active, __ATOMIC_ACQUIRE);
                break;
            }

            case CMD_ACTIVATE:
                if (__atomic_load_n(&sdk_loaded, __ATOMIC_ACQUIRE) && p_start_context) {
                    p_start_context(saved_ctx);
                    __atomic_store_n(&context_active, 1, __ATOMIC_RELEASE);
                }
                resp.context_active = 1;
                break;

            case CMD_DEACTIVATE:
                if (__atomic_load_n(&sdk_loaded, __ATOMIC_ACQUIRE) && p_stop_context) {
                    p_stop_context(saved_ctx);
                    __atomic_store_n(&context_active, 0, __ATOMIC_RELEASE);
                }
                resp.context_active = 0;
                resp.events_traced =
                    __atomic_load_n(&events_traced, __ATOMIC_RELAXED);
                break;

            case CMD_RECONFIGURE:
                apply_runtime_filter(&cmd.config);
                resp.context_active =
                    __atomic_load_n(&context_active, __ATOMIC_ACQUIRE);
                break;

            case CMD_STATUS:
                resp.context_active =
                    __atomic_load_n(&context_active, __ATOMIC_ACQUIRE);
                resp.events_traced =
                    __atomic_load_n(&events_traced, __ATOMIC_RELAXED);
                resp.events_dropped =
                    __atomic_load_n(&events_dropped, __ATOMIC_RELAXED);
                break;
            }
            send(client, &resp, sizeof(resp), 0);
        }
        close(client);
    }
    return NULL;
}
```

### 3. Tool Library (`librocprofiler-sdk-tool.so` — dlopen'd at attach)

Loaded by the stub via `dlopen` only when `CMD_CONFIGURE` arrives. Exports `rocprofiler_configure` so the SDK can find it. The tool library is the ONLY component that links against rocprofiler-sdk.

```c
static rocprofiler_context_id_t saved_ctx_local;

/* Exported — discovered by the SDK during force_configure (called by stub). */
rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t version, const char* runtime_version,
                      uint32_t priority, rocprofiler_client_id_t* id)
{
    *id = (rocprofiler_client_id_t){.name = "rocp-sock-tool"};
    static rocprofiler_tool_configure_result_t result = {
        .size = sizeof(result),
        .initialize = tool_initialize,
        .finalize = tool_finalize,
    };
    return &result;
}

/* Reads stub's pending_config via the accessor, registers the
 * controller-selected domains. SDK then propagates runtime API tables
 * and installs wrappers. */
static int tool_initialize(rocprofiler_client_finalize_t fini, void* tool_data)
{
    const rocp_stub_state_t* state = rocp_stub_get_state();
    if (!state || !state->pending_config) return -1;

    rocprofiler_create_context(&saved_ctx_local);

    if (state->pending_config->enable_hip)
        rocprofiler_configure_callback_tracing_service(
            saved_ctx_local, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
            NULL, 0, my_callback, (void*)state);
    if (state->pending_config->enable_hsa)
        rocprofiler_configure_callback_tracing_service(
            saved_ctx_local, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API,
            NULL, 0, my_callback, (void*)state);
    if (state->pending_config->enable_rccl)
        rocprofiler_configure_callback_tracing_service(
            saved_ctx_local, ROCPROFILER_CALLBACK_TRACING_RCCL_API,
            NULL, 0, my_callback, (void*)state);
    /* ... OMPT, rocdecode, rocjpeg per controller config ... */

    apply_runtime_filter(state->pending_config);
    rocprofiler_start_context(saved_ctx_local);
    *state->saved_ctx = saved_ctx_local;
    __atomic_store_n(state->context_active, 1, __ATOMIC_RELEASE);
    return 0;
}

static void my_callback(rocprofiler_callback_tracing_record_t record,
                        rocprofiler_user_data_t* user_data,
                        void* callback_data)
{
    const rocp_stub_state_t* state = callback_data;
    uint32_t mask = __atomic_load_n(&g_runtime_filter.enabled_domain_mask,
                                    __ATOMIC_ACQUIRE);
    if (!(mask & (1u << record.kind))) return;

    emit_event_to_output(&record);
    __atomic_fetch_add(state->events_traced, 1, __ATOMIC_RELAXED);
}
```

### 4. Authentication Detail

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

### 5. Controller (external process)

```c
int main(int argc, char** argv) {
    pid_t target_pid = parse_args(argc, argv);
    const char* action = parse_action(argc, argv);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    addr.sun_path[0] = '\0';
    int n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                     "rocprofiler_%d", target_pid);
    socklen_t addrlen = offsetof(struct sockaddr_un, sun_path) + 1 + n;

    if (connect(sock, (struct sockaddr*)&addr, addrlen) < 0) {
        fprintf(stderr, "Cannot connect to PID %d\n", target_pid);
        return 1;
    }

    rocp_cmd_t cmd = {0};
    if (strcmp(action, "configure") == 0) {
        cmd.type = CMD_CONFIGURE;
        cmd.config.enable_hip     = parse_flag(argc, argv, "--hip");
        cmd.config.enable_hsa     = parse_flag(argc, argv, "--hsa");
        cmd.config.enable_rccl    = parse_flag(argc, argv, "--rccl");
        cmd.config.output_format  = parse_format(argc, argv);
        strncpy(cmd.config.output_path,
                parse_str(argc, argv, "--out"), 255);
    } else if (strcmp(action, "activate") == 0) {
        cmd.type = CMD_ACTIVATE;
    } else if (strcmp(action, "deactivate") == 0) {
        cmd.type = CMD_DEACTIVATE;
    } else if (strcmp(action, "reconfigure") == 0) {
        cmd.type = CMD_RECONFIGURE;
        cmd.config.enable_hip = parse_flag(argc, argv, "--hip");
        /* ... other filter flags ... */
    } else {
        cmd.type = CMD_STATUS;
    }

    send(sock, &cmd, sizeof(cmd), 0);
    rocp_response_t resp;
    recv(sock, &resp, sizeof(resp), 0);
    printf("ctx_id: %u, active: %u, events: %lu (dropped: %lu)\n",
           resp.context_id, resp.context_active,
           resp.events_traced, resp.events_dropped);
    return 0;
}
```

### 6. Finalization (atexit, rocprofiler-sdk pattern)

```c
static _Atomic int finalize_status = 0;

static void tool_finalize(void* tool_data) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&finalize_status, &expected, -1,
                                     0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;

    /* Only stop if the SDK was actually loaded and the context is active.
     * tool_finalize may run from the stub side (via atexit) even in
     * cases where CMD_CONFIGURE never arrived. */
    if (__atomic_load_n(&sdk_loaded, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&context_active, __ATOMIC_ACQUIRE) &&
        p_stop_context) {
        p_stop_context(saved_ctx);
        __atomic_store_n(&context_active, 0, __ATOMIC_RELEASE);
    }

    /* Shut down background thread by closing the listen fd — accept() wakes
     * with EBADF and the loop exits. */
    if (listen_fd_global >= 0) {
        int fd = listen_fd_global;
        listen_fd_global = -1;
        close(fd);
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
| **Other-user access** | Blocked — stub rejects connections where `cred.uid != getuid()` |
| **Abstract namespace** | No filesystem entry — nothing to race, nothing to pre-create, nothing to symlink. However, any process in the same network namespace can *attempt* to connect (probing for socket existence). The SO_PEERCRED check in accept rejects unauthorized connections, but socket existence is discoverable |
| **Auto-cleanup** | Abstract sockets vanish when the last fd closes (process exit) |
| **Container isolation** | Abstract sockets are scoped to the network namespace |
| **PID allowlisting** | Optional: stub can also check `cred.pid` against an expected controller PID |

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Stub init | ~10-15 μs | `socket` + `bind` + `listen` + `pthread_create`. No SDK loaded yet. |
| **Hot-path before any attach** | **0 ns** | rocprofiler-sdk not loaded, no wrappers installed, original function pointers in dispatch tables |
| Controller connect | ~3-5 μs | `socket` + `connect` |
| SO_PEERCRED check | ~1 μs | `getsockopt` |
| Command send/recv | ~1-5 μs | Per command (kernel copies data between socket buffers) |
| Controller attach + first CMD_CONFIGURE | ~1-2 ms (mock; real SDK dlopen would make this ~5-50 ms) | dlopen rocprofiler-sdk-tool (brings rocprofiler-sdk as link dep) + force_configure + propagation + update_table for all registered runtime API tables |
| **Hot-path (active, callback emits)** | **~50-200 ns** | `populate_contexts()` + enter callbacks + original call + exit callbacks + buffer emplace |
| **Hot-path (active, runtime filter rejects)** | **~30-50 ns** | `populate_contexts()` + callback fires + atomic load of filter mask + return |
| Reconfigure (change runtime filter) | ~5 μs | Socket round-trip + atomic stores to `g_runtime_filter` |
| Activate / Deactivate (after first attach) | ~5 μs | Socket round-trip + start/stop_context |
| Thread idle overhead | ~0 | Thread blocked on `accept()`, no CPU |

**Key property**: when no controller ever attaches, the application has **0 ns hot-path overhead** because rocprofiler-sdk is never loaded — only the tiny stub library is in the address space, and it's not in the API call path.

## Multi-Runtime Application (rocprofiler-sdk)

Since the tool uses rocprofiler-sdk's context system, multi-runtime support is the same as mmap: a single context covers all registered domains. A single `rocprofiler_start_context(ctx)` activates tracing for HIP, HSA, RCCL, OMPT, etc. simultaneously.

The socket's bidirectional nature adds value for status queries:

```
Controller → Tool:  CMD_STATUS
Tool → Controller:  {active: true, events_traced: 42000, ...}
```

### Bidirectional Queries

Unlike mmap (shared memory status fields), the socket naturally supports rich queries with structured responses. This is useful for `rocprofv3`-style tools that want to display what's being traced and how many events have been collected.

## File Layout

```
src/tools/rocprofiler_tool_sock/
├── rocp_sock_protocol.h    # Command types, message structs (rocp_cmd_t, rocp_response_t)
├── rocp_stub_sock.c        # Stub library (preloaded via LD_PRELOAD, no rocprofiler_configure)
├── rocp_sock_tool.c        # SDK tool library (dlopen'd at attach, exports rocprofiler_configure)
└── rocp_sock_controller.c  # CLI controller tool
```

## Build Integration

```cmake
option(BUILD_ROCP_TOOL_SOCK "Build rocprofiler tool with socket control channel" ON)

if(BUILD_ROCP_TOOL_SOCK)
    # Stub library — preloaded via LD_PRELOAD, no rocprofiler-sdk dependency
    add_library(rocp_stub_sock SHARED
        src/tools/rocprofiler_tool_sock/rocp_stub_sock.c
    )
    target_link_libraries(rocp_stub_sock PRIVATE pthread dl)
    target_compile_options(rocp_stub_sock PRIVATE -O2 -fPIC)

    # SDK tool library — dlopen'd at attach time
    add_library(rocprofiler_tool_sock SHARED
        src/tools/rocprofiler_tool_sock/rocp_sock_tool.c
    )
    target_link_libraries(rocprofiler_tool_sock PRIVATE rocprofiler-sdk::rocprofiler-sdk)
    target_compile_options(rocprofiler_tool_sock PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_sock
        src/tools/rocprofiler_tool_sock/rocp_sock_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Stub preloaded, no SDK loaded — 0 ns hot-path overhead:
LD_PRELOAD=build/lib/librocp_stub_sock.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# Late attach with full configuration (no ptrace, no special privileges):
# Terminal 1:
LD_PRELOAD=build/lib/librocp_stub_sock.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2 — first attach: stub dlopens the tool library (SDK arrives as its link dependency), force_configure runs,
# wrappers installed only for the requested domains:
build/bin/rocp_ctrl_sock --pid $! configure --hip --hsa --output json --out trace.json
# ... tracing now active ...
build/bin/rocp_ctrl_sock --pid $! status
build/bin/rocp_ctrl_sock --pid $! deactivate
build/bin/rocp_ctrl_sock --pid $! activate
```

**Note**: the stub library is preloaded but does NOT export `rocprofiler_configure`, so rocprofiler-register's symbol scan does NOT load rocprofiler-sdk. The full SDK is `dlopen`'d only at first attach via `CMD_CONFIGURE`. This achieves true zero hot-path overhead before any controller attaches.

## Limitations

1. **Background thread** — Default pthread stack is ~2 MB (configurable). Thread must be joinable so `tool_finalize` can `pthread_join()` it.
2. **fork() handling** — After `fork()`, the background thread is gone. `pthread_atfork()` child handler must close the listen fd and set `finalize_status = 1` so the child's atexit handler skips cleanup.
3. **Socket buffer limits** — Large response payloads may require multiple `send()`/`recv()` calls with framing.
4. **Abstract namespace portability** — Abstract Unix sockets are Linux-specific (not available on macOS/BSD). For cross-platform, fall back to filesystem sockets.
5. **Context toggle latency** — ~5 μs per activate/deactivate (socket round-trip) vs ~1 ms (poll interval) for mmap. Socket is faster for toggle but has higher per-command overhead.
6. **Backlog sizing** — Use `listen(sock, 5)` rather than `listen(sock, 1)`. A backlog of 1 only supports a single in-flight connection, which breaks attach-detach-reattach cycles if a new controller connects before the previous one is fully drained. Backlog=5 gives headroom for transient overlap without meaningfully increasing resource use.
7. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall costs and should be validated with benchmarks.
