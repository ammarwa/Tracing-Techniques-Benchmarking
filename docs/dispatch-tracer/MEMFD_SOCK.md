# memfd: Dispatch Tracer with Socket-Bootstrapped Anonymous Shared Memory

> **This is a hybrid channel — not "just memfd."** A `memfd` is an *anonymous* file descriptor with no name; no other process can open it directly. The only no-sudo, no-filesystem way to hand it to the controller is via a Unix domain socket with `SCM_RIGHTS`. So this channel combines **two** mechanisms: a Unix abstract-namespace socket used *once* for authenticated fd handoff, and a sealed `memfd_create` region used for all subsequent control traffic.
>
> It is listed separately from the pure [SOCKET](SOCKET.md) channel because the post-bootstrap behavior is fundamentally different: after the one-time handshake, the controller writes commands directly into mmap'd memory (~50 ns cache-line transfer) instead of sending socket messages (~5 µs round-trip). The socket in this design is vestigial after connection setup — it exists solely to satisfy the "how does the controller learn about the memfd" rendezvous problem.

## Why combine them?

| Constraint | Pure socket | Pure memfd | Socket + memfd (this doc) |
|---|---|---|---|
| No sudo / no ptrace | ✓ | ✗ — needs `pidfd_getfd` (`CAP_SYS_PTRACE`) | ✓ |
| No persistent filesystem entries | ✓ (abstract) | ✓ (anonymous) | ✓ (both are anonymous; combining doesn't create a file) |
| Kernel-verified peer auth | ✓ (`SO_PEERCRED`) | N/A (no rendezvous primitive) | ✓ (inherited from socket) |
| Cache-line-speed command delivery | ✗ (per-command `sendmsg`/`recvmsg`) | ✓ (mmap store) | ✓ (mmap store after bootstrap) |
| Shared-state integrity seal | N/A | ✓ (`F_SEAL_SHRINK \| F_SEAL_GROW`) | ✓ (inherited from memfd) |

The socket is doing exactly one job: *hand an anonymous fd across a process boundary with authentication*. The memfd is doing exactly one job: *hold shared control state at mmap speed*. Neither can do the other's job without losing a key property, so both are present.

### What "no filesystem footprint" precisely means

The phrase is a common source of confusion — combining *two* IPC mechanisms sounds like it should create *more* on-disk state, not zero. The claim holds because of what each mechanism is:

- **Abstract Unix socket** (`\0rocprofiler_<pid>`): the leading NUL byte in `sun_path` tells the kernel to use its *abstract namespace*, which is a flat table inside the kernel's network namespace — **not** a mount, **not** a file, **not** under any directory. `stat("/tmp")`, `ls /run/user`, `find / -name 'rocprofiler*'` — none of these will ever see it. It is kernel state that disappears when the bound socket is closed (and closes automatically on process exit, including SIGKILL).
- **`memfd_create`**: returns a file descriptor to an anonymous kernel memory region with **no path**. There is no directory entry anywhere — no `/tmp`, no `/dev/shm`, no `/run`. The memory is freed when the last fd is closed (same process-exit guarantee).

So "combining both" does not add filesystem state because **neither of the two mechanisms has any to add**. Compared to the `mmap` channel, which puts a file at `/run/user/<uid>/rocprofiler/<pid>/ctrl` (mode `0600`, directory `0700`) that survives a crash and needs cleanup, this channel writes to zero paths.

**Honest caveats.** Two places on Linux still show traces of both IPCs, but neither is the filesystem in the sense the claim cares about:

1. `/proc/net/unix` lists the abstract-namespace socket (as does `ss -xap state listening | grep rocprofiler`). This is a kernel runtime table exposed via procfs — an in-memory view of live sockets, not a persistent file. Nothing to clean up.
2. `/proc/<pid>/fd/<N>` for the memfd will resolve (via `readlink`) to `/memfd:rocp-ctrl (deleted)`. This is the standard procfs representation of any open fd, including regular files; the `(deleted)` suffix reflects that there was never a path backing it. It vanishes with the process.

Neither of these is a file in any filesystem you can mount, back up, or `rm`. The design property the original "no filesystem footprint" line was trying to capture is: **zero paths to race on, zero stale entries on crash, zero cleanup burden**. That property does hold for the hybrid — and that is what the phrase now says explicitly.

## At a glance

- Controller ↔ stub rendezvous: **abstract Unix socket** `\0rocprofiler_<pid>` (one `accept` per controller invocation).
- Authentication: **`SO_PEERCRED`** — kernel-verified `uid == geteuid()` before handing over anything.
- Shared-state fd: **`memfd_create(..., MFD_CLOEXEC | MFD_ALLOW_SEALING)`** sized to `sizeof(rocp_ctrl_t)`, passed to controller via `sendmsg(..., SCM_RIGHTS)`.
- Post-handoff integrity: **`fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW)`** — size is frozen after the controller has its fd, so neither side can resize under the other.
- Command delivery: controller writes `rocp_cmd_t` fields into its mmap of the memfd, bumps `ctrl->version`, and (optionally) `close()`s the socket. Stub polls `ctrl->version` at 1 ms intervals and dispatches.
- Filesystem footprint: **zero persistent paths.** The abstract socket lives in the kernel's network namespace (visible via `/proc/net/unix` but not any mounted filesystem); the memfd lives in anonymous memory (visible via `/proc/<pid>/fd/<N>` as a `(deleted)` symlink, never a path). Both die automatically on process exit — no stale files on crash, nothing to `rm`. See [§ What "no filesystem footprint" precisely means](#what-no-filesystem-footprint-precisely-means) below for the full accounting.

## Integration with rocprofiler-sdk

> **Precise reading of "preloaded":** `LD_PRELOAD=librocp_stub_memfd.so` — only the stub. rocprofiler-register is already a `DT_NEEDED` dependency of HIP/HSA/OpenMP/RCCL (auto-loaded); rocprofiler-sdk is neither preloaded nor linked and is only `dlopen`'d at attach. OMPT will be handled via a silent `ompt_start_tool` in the stub (planned — see survey § OMPT for status). See [CONTROL_CHANNEL_SURVEY.md § What Exactly Gets LD_PRELOAD'd](CONTROL_CHANNEL_SURVEY.md#what-exactly-gets-ld_preloadd--and-what-does-not) and [§ OpenMP / OMPT](CONTROL_CHANNEL_SURVEY.md#openmp--ompt--a-different-registration-path).

Same late-load architecture as every channel in this design (see [MMAP.md](MMAP.md#what-changes-minimal--late-load-architecture) for the canonical description): the stub library is preloaded with no `rocprofiler_configure` symbol, so rocprofiler-register's startup scan finds no tool and never `dlopen`s the SDK — hot path stays at 0 ns. The controller later sends `CMD_CONFIGURE`; the stub `dlopen`s `libmock_sdk_tool_memfd.so` (which *does* export `rocprofiler_configure`), which calls `rocprofiler_force_configure()` to install wrappers for the controller-specified domains.

The background thread polls the memfd for `CMD_CONFIGURE` / `CMD_ACTIVATE` / `CMD_DEACTIVATE` / `CMD_RECONFIGURE` / `CMD_STATUS`.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Target Process (sample_app)                   │
│                                                                  │
│  Process start (no controller attached):                         │
│                                                                  │
│  HIP/HSA/RCCL runtimes load → link rocprofiler-register          │
│  Stub library loaded via LD_PRELOAD                              │
│    (NO rocprofiler_configure symbol exported)                    │
│  Stub setup:                                                     │
│    memfd_create("rocp-ctrl",                                     │
│        MFD_CLOEXEC | MFD_ALLOW_SEALING)                          │
│    ftruncate(memfd, sizeof(rocp_ctrl_t))                         │
│    ctrl = mmap(memfd, PROT_READ|PROT_WRITE, MAP_SHARED)          │
│    bind abstract socket "\0rocprofiler_<pid>"                    │
│    spawn background thread (accept + poll memfd)                 │
│                                                                  │
│  Runtime calls rocprofiler_register_library_api_table(...)       │
│    rocprofiler-register scans for rocprofiler_configure          │
│      → not found (only stub is loaded) → does NOT dlopen SDK     │
│  Original function pointers stay in dispatch tables              │
│  Hot path: 0 ns (rocprofiler-sdk not loaded, no wrappers)        │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐   │
│  │  Existing functor hot path (NO CHANGES, only present      │   │
│  │  after SDK is dlopen'd at first attach):                  │   │
│  │                                                           │   │
│  │  hip_api_impl<T,Op>::functor(args...):                    │   │
│  │    populate_contexts(domain, op,                          │   │
│  │        callback_ctxs, buffered_ctxs);                     │   │
│  │    if (callback_ctxs.empty() && buffered_ctxs.empty())    │   │
│  │        return exec(get_table_func(), args);  // noop      │   │
│  │    // ... full tracing path                               │   │
│  └───────────────────────────────────────────────────────────┘   │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐   │
│  │  Background Thread (NEW — socket bootstrap + memfd poll)  │   │
│  │                                                           │   │
│  │  Phase 1: Listen for controller connection                │   │
│  │    accept(listen_sock) → client                           │   │
│  │    getsockopt(client, SO_PEERCRED, &cred)                 │   │
│  │    if (cred.uid != geteuid()) close & reject              │   │
│  │                                                           │   │
│  │  Phase 2: Hand off memfd via SCM_RIGHTS                   │   │
│  │    sendmsg(client, memfd via SCM_RIGHTS)                  │   │
│  │    fcntl(memfd, F_ADD_SEALS,                              │   │
│  │          F_SEAL_SHRINK | F_SEAL_GROW)                     │   │
│  │                                                           │   │
│  │  Phase 3: Poll memfd for commands                         │   │
│  │    if (ctrl->command == CMD_CONFIGURE && !sdk_loaded) {   │   │
│  │        memcpy(&g_pending_config, &ctrl->config, ...);     │   │
│  │        dlopen("librocprofiler-sdk-tool.so",               │   │
│  │               RTLD_NOW | RTLD_GLOBAL);                    │   │
│  │        p_force_configure(tool_configure);                 │   │
│  │        /* tool_initialize registers domains +             │   │
│  │           calls rocprofiler_start_context() */            │   │
│  │        ctrl->context_active = 1;                          │   │
│  │    }                                                      │   │
│  │    if (ctrl->command == CMD_ACTIVATE)                     │   │
│  │        p_start_context(saved_ctx);                        │   │
│  │    if (ctrl->command == CMD_DEACTIVATE)                   │   │
│  │        p_stop_context(saved_ctx);                         │   │
│  │    if (ctrl->command == CMD_RECONFIGURE)                  │   │
│  │        apply_runtime_filter(&ctrl->config);               │   │
│  └───────────────────────────────────────────────────────────┘   │
│                                                                  │
│  Filesystem footprint: NONE (abstract socket + memfd only)       │
└──────────────────────┬───────────────────────────────────────────┘
                       │ Abstract Unix socket + SCM_RIGHTS fd pass
┌──────────────────────▼───────────────────────────────────────────┐
│                    Controller                                    │
│                                                                  │
│  Phase 1: Connect and authenticate                               │
│    sock = connect("\0rocprofiler_<pid>")                         │
│    // Tool validates our SO_PEERCRED on accept                   │
│                                                                  │
│  Phase 2: Receive memfd via SCM_RIGHTS                           │
│    recvmsg(sock, &msg) → extract memfd_fd from ancillary         │
│    ctrl = mmap(memfd_fd, PROT_READ|PROT_WRITE, MAP_SHARED)       │
│    verify ctrl->magic == ROCP_CTRL_MAGIC                         │
│                                                                  │
│  Phase 3: First attach — write config + CMD_CONFIGURE            │
│    ctrl->config.enable_hip = 1; /* ...domains... */              │
│    ctrl->command = CMD_CONFIGURE;                                │
│    atomic_store(&ctrl->version, v+1, RELEASE);                   │
│    /* bg thread dlopens the tool library, force_configure, start_context */   │
│                                                                  │
│  Phase 4: Subsequent commands via mmap (~50-100 ns)              │
│    ctrl->command = CMD_DEACTIVATE;                               │
│    atomic_store(&ctrl->version, v+1, RELEASE);                   │
│                                                                  │
│  Phase 5: Use socket for queries needing responses               │
│    send(sock, CMD_STATUS) → recv response                        │
└──────────────────────────────────────────────────────────────────┘

Key: After the initial socket handshake + SCM_RIGHTS fd passing,
     configure/activate/deactivate commands go through mmap
     (~50-100 ns write). The socket is only used for queries
     needing a response.
```

Uses the same **late-load design** as [mmap](MMAP.md#what-changes-minimal--late-load-architecture): a stub library is preloaded with no `rocprofiler_configure` symbol (0 ns hot path); `rocprofiler-sdk` is `dlopen`'d on the first `CMD_CONFIGURE` and `rocprofiler_force_configure()` installs wrappers for the controller-specified domains. See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#late-load-design-defer-rocprofiler-sdk-loading-until-attach) for the full mechanism explanation.

## Control Structure

Same as mmap — the `rocp_ctrl_t` struct carries commands, the controller's per-attach `rocp_config_t`, and status counters. The canonical definitions live in [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#canonical-control-struct). Summary:

```c
#define ROCP_CTRL_MAGIC   0xD15EA7C0  // Same as mmap for interoperability
#define ROCP_CTRL_VERSION 1

/* See CONTROL_CHANNEL_SURVEY.md for the canonical enum. */
enum rocp_ctrl_command {
    CMD_NONE        = 0,
    CMD_CONFIGURE   = 1,  // First attach: dlopen tool library (brings rocprofiler-sdk via link dep) + force_configure
    CMD_ACTIVATE    = 2,  // rocprofiler_start_context()
    CMD_DEACTIVATE  = 3,  // rocprofiler_stop_context()
    CMD_RECONFIGURE = 4,  // Update runtime filter without toggling context
    CMD_STATUS      = 5,  // Socket-side query (response via socket)
};

/* Same layout as mmap's rocp_config_t — see CONTROL_CHANNEL_SURVEY.md.
 * Carries the domain enable bits, output format, buffer sizing, filter
 * patterns; the tool's real_tool_initialize reads this to decide which
 * services to register. */
typedef struct {
    uint32_t enable_hip       : 1;
    uint32_t enable_hsa       : 1;
    uint32_t enable_rccl      : 1;
    uint32_t enable_ompt      : 1;
    uint32_t enable_rocdecode : 1;
    uint32_t enable_rocjpeg   : 1;
    uint32_t enable_kernel_dispatch : 1;
    uint32_t reserved         : 25;

    uint32_t output_format;   // TEXT=0, JSON=1, PERFETTO=2
    uint32_t buffer_size_kb;
    char output_path[256];
    char filter_pattern[256];
    char exclude_pattern[256];
} rocp_config_t;

typedef struct {
    /* Identification */
    uint32_t magic;              // Must equal ROCP_CTRL_MAGIC
    uint32_t struct_version;     // ROCP_CTRL_VERSION

    /* Command channel (controller → tool) */
    _Atomic uint32_t command;    // rocp_ctrl_command
    _Atomic uint32_t version;    // Bumped by controller on every command

    /* Configuration (controller writes, tool reads on CMD_CONFIGURE / CMD_RECONFIGURE) */
    rocp_config_t config;

    /* Status (tool → controller, read-only from controller side) */
    _Atomic uint32_t context_active;
    _Atomic uint32_t context_id;
    _Atomic uint64_t events_traced;
    _Atomic uint64_t events_dropped;

    /* Tool identification */
    uint32_t pid;
    uint64_t start_time;
} __attribute__((aligned(64))) rocp_ctrl_t;
```

## Components

### 1. Stub Library (`librocp_stub_memfd.so` — preloaded, NO `rocprofiler_configure` symbol)

Loaded via `LD_PRELOAD` at process start. Creates the memfd, binds the abstract socket, spawns the background thread. Does NOT link rocprofiler-sdk — only `pthread` and `dl`. Because it never exports `rocprofiler_configure`, rocprofiler-register's symbol scan finds no tool and does NOT `dlopen` rocprofiler-sdk. The dispatch tables keep their original function pointers — **0 ns hot-path overhead**.

```c
/* Loaded at process start via LD_PRELOAD. */
__attribute__((constructor))
static void stub_init(void) {
    setup_memfd_and_socket();
}

static rocp_ctrl_t* ctrl       = NULL;
static int         memfd       = -1;
static int         listen_sock = -1;
static pthread_t   bg_thread;
static rocp_config_t g_pending_config;   /* read by tool_initialize via accessor */
static rocprofiler_context_id_t saved_ctx;
static void* sdk_handle = NULL;

/* Exported accessor — tool calls this after being dlopen'd.
 * Avoids extern cross-DSO globals (see mmap's stub↔tool contract). */
typedef struct {
    rocp_ctrl_t* ctrl;
    rocp_config_t* pending_config;
    rocprofiler_context_id_t* saved_ctx;
} rocp_stub_state_t;

__attribute__((visibility("default")))
const rocp_stub_state_t* rocp_stub_get_state(void) {
    static rocp_stub_state_t state;
    state.ctrl           = ctrl;
    state.pending_config = &g_pending_config;
    state.saved_ctx      = &saved_ctx;
    return &state;
}

/* Resolved after dlopen of rocprofiler-sdk */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t);
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t);
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t);

static void setup_memfd_and_socket(void) {
    memfd = memfd_create("rocp-ctrl", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ftruncate(memfd, sizeof(rocp_ctrl_t));
    ctrl = mmap(NULL, sizeof(rocp_ctrl_t),
                PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    ctrl->magic          = ROCP_CTRL_MAGIC;
    ctrl->struct_version = ROCP_CTRL_VERSION;
    ctrl->command        = CMD_NONE;
    ctrl->pid            = getpid();

    listen_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    /* Abstract namespace: leading NUL, no filesystem entry */
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
             "rocprofiler_%d", getpid());
    bind(listen_sock, (struct sockaddr*)&addr,
         offsetof(struct sockaddr_un, sun_path) + 1 +
         strlen(addr.sun_path + 1));
    listen(listen_sock, 1);

    pthread_create(&bg_thread, NULL, control_loop, NULL);
}
```

### 2. Background Thread (socket bootstrap + memfd poll + dlopen the tool library on first attach)

```c
/* On first CMD_CONFIGURE: dlopen rocprofiler-sdk-tool, force_configure.
 * See mmap for the shared stub↔tool state contract and rationale
 * for RTLD_GLOBAL + RTLD_DEFAULT symbol resolution. */
static void load_sdk_and_configure(void) {
    sdk_handle = dlopen("librocprofiler-sdk-tool.so",
                        RTLD_NOW | RTLD_GLOBAL);
    if (!sdk_handle) return;

    typedef rocprofiler_tool_configure_result_t* (*configure_fn_t)(
        uint32_t, const char*, uint32_t, rocprofiler_client_id_t*);
    configure_fn_t tool_configure = dlsym(RTLD_DEFAULT, "rocprofiler_configure");
    p_force_configure = dlsym(RTLD_DEFAULT, "rocprofiler_force_configure");
    p_start_context   = dlsym(RTLD_DEFAULT, "rocprofiler_start_context");
    p_stop_context    = dlsym(RTLD_DEFAULT, "rocprofiler_stop_context");

    if (p_force_configure && tool_configure)
        p_force_configure(tool_configure);
}

static _Atomic bool sdk_loaded = false;
static _Atomic int  shutdown_flag = 0;

static void* control_loop(void* arg) {
    /* Phase 1: accept + SO_PEERCRED auth */
    int client = accept(listen_sock, NULL, NULL);
    if (client < 0) return NULL;

    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
        cred.uid != geteuid()) {
        close(client); return NULL;
    }

    /* Phase 2: hand off memfd via SCM_RIGHTS, then seal */
    send_fd(client, memfd);
    fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW);

    /* Phase 3: poll memfd for commands */
    uint32_t last_version = 0;
    while (!__atomic_load_n(&shutdown_flag, __ATOMIC_ACQUIRE)) {
        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver == last_version) { usleep(1000); continue; }

        uint32_t cmd = __atomic_load_n(&ctrl->command, __ATOMIC_ACQUIRE);
        switch (cmd) {
        case CMD_CONFIGURE: {
            memcpy(&g_pending_config, &ctrl->config, sizeof(g_pending_config));
            bool expected = false;
            if (__atomic_compare_exchange_n(&sdk_loaded, &expected, true,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                load_sdk_and_configure();  /* tool_initialize calls start_context */
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
            } else {
                apply_runtime_filter(&ctrl->config);
            }
            break;
        }
        case CMD_ACTIVATE:
            if (__atomic_load_n(&sdk_loaded, __ATOMIC_ACQUIRE) && p_start_context) {
                p_start_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
            }
            break;
        case CMD_DEACTIVATE:
            if (__atomic_load_n(&sdk_loaded, __ATOMIC_ACQUIRE) && p_stop_context) {
                p_stop_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 0, __ATOMIC_RELEASE);
            }
            break;
        case CMD_RECONFIGURE:
            apply_runtime_filter(&ctrl->config);
            break;
        }
        last_version = ver;
    }
    return NULL;
}
```

### 3. Tool Library (`librocprofiler-sdk-tool.so` — dlopen'd at first attach)

Same as mmap. The tool exports `rocprofiler_configure`, and its `tool_initialize` calls the stub's `rocp_stub_get_state()` accessor to read the pending config and write the created `rocprofiler_context_id_t` back for the bg thread to toggle. See [mmap § Tool Library](MMAP.md#3-tool-library-librocprofiler-sdk-toolso--dlopend-at-attach) for the full `tool_initialize` body and the stub↔tool accessor contract — the only difference under memfd is that the control struct is backed by the memfd instead of a `/run/user/<uid>/` file.

### 4. Controller

The controller connects to the abstract socket, receives the memfd via `SCM_RIGHTS`, mmaps it, and then drives `CMD_CONFIGURE` / `CMD_ACTIVATE` / `CMD_DEACTIVATE` / `CMD_RECONFIGURE` identically to mmap's controller. `CMD_STATUS` (response-bearing query) goes over the socket instead of mmap.

## SCM_RIGHTS File Descriptor Passing

This is the core mechanism that makes memfd work. `SCM_RIGHTS` allows one process to send a file descriptor to another over a Unix domain socket. The kernel duplicates the fd into the receiver's fd table.

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

### Why memfd is more secure than mmap (file):

1. **No predictable path** — the memfd has no filesystem entry at all
2. **No race window** — the memfd is created by the tool, not discovered by name
3. **Authenticated handoff** — the controller only gets the memfd fd after passing SO_PEERCRED
4. **Sealable** — the tool can prevent size manipulation

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Tool init | ~15-20 μs | `memfd_create` + `mmap` + `socket` + `bind` + `listen` + `pthread_create` |
| Controller connect + memfd recv | ~10-15 μs | `connect` + `recvmsg(SCM_RIGHTS)` + `mmap` |
| **Hot-path (no attach)** | **0 ns** | Stub loaded but rocprofiler-sdk not loaded — original function pointers |
| **Hot-path (active)** | **~50-200 ns** | `populate_contexts()` + callbacks + buffer emplace |
| Command write (mmap) | ~50-100 ns | Direct write to shared memory (cache-line transfer) |
| Context toggle latency | ~1 ms | Background thread poll interval (or futex wake) |
| Status query (socket) | ~2-5 μs | Socket round-trip for response |

## Multi-Runtime Application (rocprofiler-sdk)

Since the tool uses rocprofiler-sdk's context system, multi-runtime support is the same as mmap: a single context covers all registered domains. A single `rocprofiler_start_context(ctx)` activates tracing for HIP, HSA, RCCL, OMPT, etc. simultaneously.

The memfd carries only the `rocp_ctrl_t` command/status struct — no per-runtime control needed. The controller writes `CMD_ACTIVATE` to the memfd; the tool's background thread calls `rocprofiler_start_context()` and all registered domains begin tracing.

**Advantage over mmap**: The memfd has no filesystem path, so there's nothing to clean up on crash and no PID-reuse stale file problem.

**Advantage over socket**: Commands go through mmap (~50-100 ns) instead of socket send (~1-5 μs). The socket is reserved for status queries that need responses.

## File Layout

```
src/tools/rocprofiler_tool_memfd/
├── rocp_memfd.h             # Shared structs (rocp_ctrl_t, rocp_config_t, rocp_stub_state_t), constants
├── rocp_stub_memfd.c        # Stub library (preloaded via LD_PRELOAD, no rocprofiler_configure,
│                            #   creates memfd + abstract socket, spawns bg thread, dlopens the tool library on CMD_CONFIGURE)
├── rocp_memfd_tool.c        # SDK tool library (dlopen'd at attach, exports rocprofiler_configure)
├── rocp_memfd_fdpass.c      # SCM_RIGHTS helper functions (shared by stub + controller)
└── rocp_memfd_controller.c  # CLI controller tool
```

## Build Integration

```cmake
option(BUILD_ROCP_TOOL_MEMFD "Build rocprofiler tool with memfd control channel" ON)

if(BUILD_ROCP_TOOL_MEMFD)
    # Stub library — preloaded via LD_PRELOAD. No rocprofiler-sdk dependency.
    # Does NOT export rocprofiler_configure, so rocprofiler-register skips SDK load.
    add_library(rocp_stub_memfd SHARED
        src/tools/rocprofiler_tool_memfd/rocp_stub_memfd.c
        src/tools/rocprofiler_tool_memfd/rocp_memfd_fdpass.c
    )
    target_link_libraries(rocp_stub_memfd PRIVATE pthread dl)
    target_compile_options(rocp_stub_memfd PRIVATE -O2 -fPIC)

    # SDK tool library — dlopen'd at attach time. Links rocprofiler-sdk.
    add_library(rocprofiler_tool_memfd SHARED
        src/tools/rocprofiler_tool_memfd/rocp_memfd_tool.c
    )
    target_link_libraries(rocprofiler_tool_memfd
        PRIVATE rocprofiler-sdk::rocprofiler-sdk)
    target_compile_options(rocprofiler_tool_memfd PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_memfd
        src/tools/rocprofiler_tool_memfd/rocp_memfd_controller.c
        src/tools/rocprofiler_tool_memfd/rocp_memfd_fdpass.c
    )
endif()
```

## Benchmark Usage

```bash
# Stub preloaded, no SDK loaded — 0 ns hot-path overhead:
LD_PRELOAD=build/lib/librocp_stub_memfd.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# Late attach with full configuration:
# Terminal 1:
LD_PRELOAD=build/lib/librocp_stub_memfd.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/rocp_ctrl_memfd --pid $! configure --hip --hsa --output json
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
