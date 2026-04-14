# Option B: Dispatch Tracer with mmap Regular File Control Channel

## Overview

This design uses a **memory-mapped regular file** as the control channel between the tool library inside the target process and an external controller process. The file lives under the user's private runtime directory (`/run/user/<uid>/`), providing directory-level permission isolation.

The key insight from analyzing rocprofiler-sdk's existing code: **the existing functor wrappers already check active contexts on every call** via `populate_contexts()`. When no context is active, the wrapper calls the original directly (~10-20 ns overhead). So the control channel's job is simply to **toggle context activation** — everything else (wrapper installation, callback dispatch, buffer management) is already implemented.

## What Reuses Existing rocprofiler-sdk Code (No Changes)

- `rocprofiler_set_api_table()` — runtime registration entry point
- `copy_table()` / `update_table()` — saves originals, installs wrappers
- `hip_api_impl<TableIdx, OpIdx>::functor()` — the wrapper template
- `populate_contexts()` / `context_filter()` — the call-time noop check
- `callback_tracing_service` / `buffer_tracing_service` — context services
- `execute_phase_enter_callbacks()` / `execute_phase_exit_callbacks()` — callback dispatch
- `execute_buffer_record_emplace()` — buffer record writing
- Correlation ID system, domain/operation bitsets, all of it

## What Changes (Minimal) — Late-Load Architecture

1. **Stub library** (preloaded via `LD_PRELOAD`): does NOT export `rocprofiler_configure`. Sets up the mmap control file and spawns a background thread. Because no `rocprofiler_configure` symbol exists, rocprofiler-register's symbol scan finds no tool and does NOT `dlopen` rocprofiler-sdk. The dispatch tables keep their original function pointers — **0 ns hot-path overhead**.

2. **Background thread**: polls the mmap. On `CMD_CONFIGURE`:
   - Stashes the controller's config (which domains, output format, etc.) in a static struct
   - `dlopen("librocprofiler-sdk-tool.so", RTLD_NOW)` — this brings rocprofiler-sdk into the address space along with the tool's `rocprofiler_configure` symbol
   - Calls `rocprofiler_force_configure(real_tool_configure)` — succeeds because rocprofiler-sdk's `init_status` is still 0 at this point
   - SDK initializes, real_tool_configure runs, context is created with the controller's domains, propagation re-replays runtime API tables, wrappers install
   - Calls `rocprofiler_start_context(ctx)` to activate

3. **Tool library** (`librocprofiler-sdk-tool.so`, dlopen'd at attach): exports `rocprofiler_configure` returning real callbacks. The `tool_initialize` callback reads the stub's stashed config and registers the controller-selected domains.

4. **Reconfigure (CMD_RECONFIGURE)**: once SDK is loaded, `force_configure` is locked. The tool callbacks read a runtime filter (atomic) to decide whether to emit each event — this gives late filtering of which APIs actually emit, even though the wrapper set is fixed at first attach.

That's it. The wrapper code, dispatch table machinery, and callback infrastructure are untouched.

See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#late-load-design-defer-rocprofiler-sdk-loading-until-attach) for the full mechanism explanation.

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
│    create mmap file at                                       │
│      /run/user/<uid>/rocprofiler/<pid>/ctrl                  │
│    spawn background thread polling the control file          │
│                                                              │
│  Runtime calls rocprofiler_register_library_api_table(...)   │
│    rocprofiler-register stores table pointer                 │
│    rocprofiler-register scans for rocprofiler_configure      │
│      → not found (only stub is loaded) → does NOT load SDK   │
│  Original function pointers stay in dispatch tables          │
│  Hot path: 0 ns (no wrappers, no SDK code)                  │
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
│  │  Background thread (NEW — reads mmap'd control file):   │ │
│  │                                                         │ │
│  │  loop:                                                  │ │
│  │    if (ctrl->command == CMD_ACTIVATE &&                 │ │
│  │        !context_active) {                               │ │
│  │        rocprofiler_start_context(ctx_id);  // existing  │ │
│  │        context_active = true;                           │ │
│  │    }                                                    │ │
│  │    if (ctrl->command == CMD_DEACTIVATE &&               │ │
│  │        context_active) {                                │ │
│  │        rocprofiler_stop_context(ctx_id);   // existing  │ │
│  │        context_active = false;                          │ │
│  │    }                                                    │ │
│  │    usleep or futex_wait on ctrl->version change         │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                              │
│  /run/user/1000/rocprofiler/12345/ctrl  [mode 0600]         │
│  /run/user/1000/rocprofiler/            [mode 0700]         │
└──────────────────┬──────────────────────────────────────────┘
                   │ mmap (same physical pages)
┌──────────────────▼──────────────────────────────────────────┐
│  Controller (e.g., rocprofv3 --attach --channel mmap)       │
│                                                              │
│  fd = open(/run/user/<uid>/rocprofiler/<pid>/ctrl, O_RDWR)  │
│  ctrl = mmap(fd, PROT_READ|PROT_WRITE, MAP_SHARED)         │
│  verify ctrl->magic, ctrl->struct_version                   │
│                                                              │
│  // ATTACH:                                                 │
│  ctrl->command = CMD_ACTIVATE;                              │
│  __atomic_store_n(&ctrl->version, v+1, __ATOMIC_RELEASE);  │
│                                                              │
│  // DETACH:                                                 │
│  ctrl->command = CMD_DEACTIVATE;                            │
│  __atomic_store_n(&ctrl->version, v+1, __ATOMIC_RELEASE);  │
└─────────────────────────────────────────────────────────────┘
```

## Control Structure

Since the existing rocprofiler-sdk context system handles per-operation enable/disable, buffer management, output format, and callback configuration, the mmap control struct is dramatically simplified. It only needs to carry **commands** (activate/deactivate) and **status** (statistics from the tool):

```c
#define ROCP_CTRL_MAGIC   0xD15EA7C0
#define ROCP_CTRL_VERSION 1

enum rocp_ctrl_command {
    CMD_NONE        = 0,   // No pending command
    CMD_ACTIVATE    = 1,   // Apply config + rocprofiler_start_context()
    CMD_DEACTIVATE  = 2,   // rocprofiler_stop_context() (wrappers stay, Level 2 noop)
    CMD_RECONFIGURE = 3,   // Update runtime filter (output, domains-to-emit, patterns)
                           //   without changing context activation state
};

/* Configuration the controller sends with CMD_ACTIVATE / CMD_RECONFIGURE.
 * The tool's real_tool_initialize reads this to decide which
 * domains/services to register. */
typedef struct {
    uint32_t enable_hip       : 1;
    uint32_t enable_hsa       : 1;
    uint32_t enable_rccl      : 1;
    uint32_t enable_ompt      : 1;
    uint32_t enable_rocdecode : 1;
    uint32_t enable_rocjpeg   : 1;
    uint32_t enable_kernel_dispatch : 1;
    uint32_t reserved         : 25;

    uint32_t output_format;   // TEXT=0, JSON=1, OTLP=2, PERFETTO=3
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

    /* Configuration (controller writes, tool reads on CMD_ACTIVATE / CMD_RECONFIGURE) */
    rocp_config_t config;

    /* Status (tool → controller, read-only from controller side) */
    _Atomic uint32_t context_active;  // 0 = inactive, 1 = active
    _Atomic uint32_t context_id;      // rocprofiler_context_id_t.handle (0 if not yet configured)
    _Atomic uint64_t events_traced;
    _Atomic uint64_t events_dropped;

    /* Tool identification */
    uint32_t pid;                // Target process PID (for stale detection)
    uint64_t start_time;         // /proc/<pid>/stat start time (for PID reuse)
} __attribute__((aligned(64))) rocp_ctrl_t;
```

**Why this is so much simpler**: All the per-function bitmasks, output format, ring buffer sizes, filter patterns, etc. are configured through the existing `rocprofiler_configure_callback_tracing_service()` / `rocprofiler_configure_buffer_tracing_service()` APIs during the tool's `initialize` callback. The mmap control channel only toggles context activation — a single atomic flag that the existing `populate_contexts()` already checks.

## Components

### 1. Stub Library (`librocp_stub_mmap.so` — preloaded, NO rocprofiler_configure symbol)

The stub is loaded via `LD_PRELOAD` at process start. It does NOT export `rocprofiler_configure`, so rocprofiler-register's symbol scan finds no tool and does NOT load rocprofiler-sdk. The stub only sets up the control channel and waits.

```c
/* Loaded at process start via LD_PRELOAD.
 * Sets up the mmap control file and spawns the bg thread.
 * Does NOT load rocprofiler-sdk. */
__attribute__((constructor))
static void stub_init(void) {
    setup_mmap_control();
}

static rocp_ctrl_t* ctrl = NULL;
static pthread_t bg_thread;
static rocp_config_t g_pending_config;  /* read by tool_initialize via accessor */
static rocprofiler_context_id_t saved_ctx;
static void* sdk_handle = NULL;

/* Exported accessor — tool calls this after being dlopen'd */
__attribute__((visibility("default")))
const rocp_stub_state_t* rocp_stub_get_state(void) {
    static rocp_stub_state_t state;
    state.ctrl = ctrl;
    state.pending_config = &g_pending_config;
    state.saved_ctx = &saved_ctx;
    return &state;
}
/* Function pointers resolved after dlopen of rocprofiler-sdk */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t);
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t);
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t);

static void setup_mmap_control(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/run/user/%u/rocprofiler",
             (unsigned)getuid());
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "/run/user/%u/rocprofiler/%d",
             (unsigned)getuid(), getpid());
    mkdir(path, 0700);
    strncat(path, "/ctrl", sizeof(path) - strlen(path) - 1);

    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    ftruncate(fd, sizeof(rocp_ctrl_t));
    ctrl = mmap(NULL, sizeof(rocp_ctrl_t),
                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    ctrl->magic = ROCP_CTRL_MAGIC;
    ctrl->struct_version = ROCP_CTRL_VERSION;
    ctrl->command = CMD_NONE;
    ctrl->context_active = 0;
    ctrl->context_id = saved_ctx.handle;
    ctrl->pid = getpid();

    pthread_create(&bg_thread, NULL, control_poll_loop, NULL);
}
```

### 2. Background Thread (polls mmap, dlopens SDK on first attach)

```c
/* On first CMD_CONFIGURE: dlopen rocprofiler-sdk-tool, call force_configure.
 * The tool library exports rocprofiler_configure which the SDK will invoke. */
static void load_sdk_and_configure(void) {
    /* RTLD_GLOBAL is REQUIRED so the tool's rocprofiler_configure becomes
     * visible to the SDK's dlsym(RTLD_DEFAULT, "rocprofiler_configure")
     * scan during initialize(). Without RTLD_GLOBAL the tool would be
     * silently ignored. */
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
}

static _Atomic bool sdk_loaded = false;  /* atomic guards double-dlopen */

static void* control_poll_loop(void* arg) {
    uint32_t last_version = 0;

    while (!__atomic_load_n(&shutdown_flag, __ATOMIC_ACQUIRE)) {
        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver == last_version) {
            usleep(1000);  /* 1ms poll (or futex_wait on ctrl->version) */
            continue;
        }

        uint32_t cmd = __atomic_load_n(&ctrl->command, __ATOMIC_ACQUIRE);
        switch (cmd) {
        case CMD_CONFIGURE: {
            /* First attach: dlopen SDK, force_configure with controller's
             * domain selection. After this, force_configure is locked. */
            memcpy(&g_pending_config, &ctrl->config, sizeof(g_pending_config));
            bool expected = false;
            if (__atomic_compare_exchange_n(&sdk_loaded, &expected, true,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE)) {
                load_sdk_and_configure();
                /* tool_initialize calls start_context, so context is active */
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
            } else {
                /* SDK already loaded — update runtime filter only.
                 * Cannot add new domains; force_configure is locked. */
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
            /* Tool-side filter only — no SDK calls needed */
            apply_runtime_filter(&ctrl->config);
            break;
        }
        last_version = ver;
    }
    return NULL;
}
```

### 3. Tool Library (`librocprofiler-sdk-tool.so` — dlopen'd at attach)

Loaded by the stub via `dlopen` only when the controller attaches. Exports `rocprofiler_configure` so the SDK can find it.

**Stub↔Tool state-sharing contract**: instead of `extern` cross-DSO globals (which require `RTLD_GLOBAL` and can fail silently if the load order is wrong), the stub exports a single accessor function that the tool calls to get pointers to the shared state:

```c
/* Exported by the stub library. Tool calls this once during tool_initialize. */
typedef struct {
    rocp_ctrl_t* ctrl;             // mmap'd control struct
    rocp_config_t* pending_config;  // controller's config to apply
    rocprofiler_context_id_t* saved_ctx;  // tool writes context ID here
} rocp_stub_state_t;

const rocp_stub_state_t* rocp_stub_get_state(void);
```

```c
/* Discovered by the SDK during force_configure (called by stub). */
static rocprofiler_context_id_t saved_ctx_local;

rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t version, const char* runtime_version,
                      uint32_t priority, rocprofiler_client_id_t* id)
{
    *id = (rocprofiler_client_id_t){.name = "rocp-mmap-tool"};
    static rocprofiler_tool_configure_result_t result = {
        .size = sizeof(result),
        .initialize = tool_initialize,
        .finalize = tool_finalize,
    };
    return &result;
}

/* Reads stub's pending_config, registers the controller-selected domains.
 * SDK then propagates runtime API tables and installs wrappers. */
static int tool_initialize(rocprofiler_client_finalize_t fini, void* tool_data)
{
    /* Get shared state from stub via the exported accessor */
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
    __atomic_store_n(&state->ctrl->context_id, saved_ctx_local.handle,
                     __ATOMIC_RELEASE);
    return 0;
}

static void my_callback(rocprofiler_callback_tracing_record_t record,
                        rocprofiler_user_data_t* user_data,
                        void* callback_data)
{
    const rocp_stub_state_t* state = callback_data;
    /* Optional runtime filter (set by CMD_RECONFIGURE) */
    uint32_t mask = __atomic_load_n(&g_runtime_filter.enabled_domain_mask,
                                    __ATOMIC_ACQUIRE);
    if (!(mask & (1u << record.kind))) return;

    emit_event_to_output(&record);
    __atomic_fetch_add(&state->ctrl->events_traced, 1, __ATOMIC_RELAXED);
}
```

### 4. Controller (external process)

```c
int main(int argc, char** argv) {
    pid_t target_pid = parse_args(argc, argv);
    const char* action = parse_action(argc, argv);  // "activate" or "deactivate"

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/run/user/%u/rocprofiler/%d/ctrl",
             (unsigned)getuid(), target_pid);

    int fd = open(path, O_RDWR | O_NOFOLLOW);
    rocp_ctrl_t* ctrl = mmap(NULL, sizeof(rocp_ctrl_t),
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (ctrl->magic != ROCP_CTRL_MAGIC) { /* error */ }
    if (ctrl->struct_version != ROCP_CTRL_VERSION) { /* error */ }

    if (strcmp(action, "activate") == 0) {
        /* Write filter config + send CMD_ACTIVATE.
         * Tool's bg thread updates runtime filter and calls
         * rocprofiler_start_context(). Tracing begins immediately. */
        ctrl->config.enable_hip      = parse_flag(argc, argv, "--hip");
        ctrl->config.enable_hsa      = parse_flag(argc, argv, "--hsa");
        ctrl->config.enable_rccl     = parse_flag(argc, argv, "--rccl");
        ctrl->config.enable_ompt     = parse_flag(argc, argv, "--ompt");
        ctrl->config.output_format   = parse_format(argc, argv);
        ctrl->config.buffer_size_kb  = parse_int(argc, argv, "--buf-kb", 4096);
        strncpy(ctrl->config.output_path, parse_str(argc, argv, "--out"), 255);

        uint32_t v = __atomic_load_n(&ctrl->version, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->command, CMD_ACTIVATE, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->version, v + 1, __ATOMIC_RELEASE);
        printf("Activated tracing for PID %d\n", target_pid);

    } else if (strcmp(action, "reconfigure") == 0) {
        /* Update which domains emit events / output format, without
         * changing activation state. Effect is immediate on next event. */
        ctrl->config.enable_hip = parse_flag(argc, argv, "--hip");
        /* ... other flags ... */

        uint32_t v = __atomic_load_n(&ctrl->version, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->command, CMD_RECONFIGURE, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->version, v + 1, __ATOMIC_RELEASE);
        printf("Reconfigured filter for PID %d\n", target_pid);

    } else {  /* deactivate */
        uint32_t v = __atomic_load_n(&ctrl->version, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->command, CMD_DEACTIVATE, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->version, v + 1, __ATOMIC_RELEASE);
        printf("Deactivated. Events: %lu\n",
               __atomic_load_n(&ctrl->events_traced, __ATOMIC_RELAXED));
    }
    return 0;
}
```

### 4. Finalization (atexit, rocprofiler-sdk pattern)

```c
static _Atomic int finalize_status = 0;

static void tool_finalize(void* tool_data) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&finalize_status, &expected, -1,
                                     0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return;

    /* Stop context if still active */
    if (ctrl && ctrl->context_active)
        rocprofiler_stop_context(saved_ctx);

    /* Shutdown background thread */
    __atomic_store_n(&shutdown_flag, 1, __ATOMIC_RELEASE);
    pthread_join(bg_thread, NULL);

    /* Cleanup mmap */
    if (ctrl) { munmap(ctrl, sizeof(rocp_ctrl_t)); ctrl = NULL; }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/run/user/%u/rocprofiler/%d/ctrl",
             (unsigned)getuid(), getpid());
    unlink(path);
    snprintf(path, sizeof(path), "/run/user/%u/rocprofiler/%d",
             (unsigned)getuid(), getpid());
    rmdir(path);

    __atomic_store_n(&finalize_status, 1, __ATOMIC_SEQ_CST);
}
```

## Security Analysis

| Property | Assessment |
|----------|------------|
| **Other-user read** | Blocked — `/run/user/<uid>/` is mode `0700`, only accessible by owning UID |
| **Other-user write** | Blocked — same directory-level protection |
| **Root access** | Root can always access (unavoidable on any Unix system) |
| **Same-user interference** | Possible — any process running as the same user can write to the file. Mitigated by the magic cookie and version counter |
| **Race condition** | Protected — `/run/user/<uid>/` is created by `pam_systemd` with correct ownership. PID-specific subdirectory created with `mkdir` + `O_NOFOLLOW` on file open to prevent symlink attacks by same-user processes |
| **PID reuse** | After crash, a stale control file with valid magic may match a new unrelated process with the same PID. Controller should verify `/proc/<pid>/stat` start time matches `ctrl->start_time`, or check that the tool library is loaded via `/proc/<pid>/maps` |
| **Stale artifacts** | Control file persists in tmpfs on crash. Cleaned up on user logout (tmpfs). The controller can detect stale entries by checking if the PID is still alive |

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Stub init | ~10-50 μs | mkdir + open + ftruncate + mmap + pthread_create. No SDK loaded yet. |
| **Hot-path before any attach** | **0 ns** | rocprofiler-sdk not loaded, no wrappers installed, original function pointers in dispatch tables |
| Controller attach + first configure | ~5-50 ms | dlopen rocprofiler-sdk + force_configure + propagation + update_table for all registered runtime API tables |
| **Hot-path (active, callback emits)** | **~50-200 ns** | `populate_contexts()` + enter callbacks + original call + exit callbacks + buffer emplace |
| **Hot-path (active, runtime filter rejects)** | **~30-50 ns** | `populate_contexts()` + callback fires + atomic load of filter mask + return |
| Reconfigure (change runtime filter) | ~1 ms | Atomic stores to `g_runtime_filter` — effect immediate on next event |
| Activate / Deactivate (after first attach) | ~1 ms | bg thread sees CMD_*, calls start/stop_context |
| Stub cleanup | ~5 μs | munmap + unlink + rmdir |

**Key property**: when no controller ever attaches, the application has **0 ns hot-path overhead** because rocprofiler-sdk is never loaded — only the tiny stub library is in the address space, and it's not in the API call path.

## Multi-Runtime Application (rocprofiler-sdk)

Since the tool uses rocprofiler-sdk's existing context system, multi-runtime support is straightforward:

```c
/* In tool_initialize: create one context covering all runtimes */
rocprofiler_context_id_t ctx;
rocprofiler_create_context(&ctx);

/* Register interest in all domains (existing APIs) */
rocprofiler_configure_callback_tracing_service(ctx, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API, ...);
rocprofiler_configure_callback_tracing_service(ctx, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API, ...);
rocprofiler_configure_callback_tracing_service(ctx, ROCPROFILER_CALLBACK_TRACING_RCCL_API, ...);
rocprofiler_configure_callback_tracing_service(ctx, ROCPROFILER_CALLBACK_TRACING_OMPT, ...);
/* rocdecode, rocjpeg — when rocprofiler-sdk adds their domains */
```

A single `rocprofiler_start_context(ctx)` activates tracing for ALL registered domains simultaneously. The control file is just:

```
/run/user/1000/rocprofiler/12345/ctrl   # 64-byte rocp_ctrl_t
```

No per-runtime files needed. Per-function and per-domain configuration is handled by the existing `rocprofiler_configure_*` APIs at init time.

## File Layout

```
src/tools/rocprofiler_tool_mmap/
├── rocp_mmap.h              # rocp_ctrl_t, rocp_config_t, constants
├── rocp_stub_mmap.c         # Stub library (preloaded via LD_PRELOAD, no rocprofiler_configure)
├── rocp_mmap_tool.c         # SDK tool library (dlopen'd at attach, exports rocprofiler_configure)
└── rocp_mmap_controller.c   # CLI controller tool
```

## Build Integration (CMakeLists.txt additions)

```cmake
option(BUILD_ROCP_TOOL_MMAP "Build rocprofiler tool with mmap control channel" ON)

if(BUILD_ROCP_TOOL_MMAP)
    # Stub library — preloaded via LD_PRELOAD, no rocprofiler-sdk dependency
    add_library(rocp_stub_mmap SHARED
        src/tools/rocprofiler_tool_mmap/rocp_stub_mmap.c
    )
    target_link_libraries(rocp_stub_mmap PRIVATE pthread dl)
    target_compile_options(rocp_stub_mmap PRIVATE -O2 -fPIC)

    # SDK tool library — dlopen'd at attach time
    add_library(rocprofiler_tool_mmap SHARED
        src/tools/rocprofiler_tool_mmap/rocp_mmap_tool.c
    )
    target_link_libraries(rocprofiler_tool_mmap PRIVATE rocprofiler-sdk::rocprofiler-sdk)
    target_compile_options(rocprofiler_tool_mmap PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_mmap
        src/tools/rocprofiler_tool_mmap/rocp_mmap_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Stub preloaded, no SDK loaded — 0 ns hot-path overhead:
LD_PRELOAD=build/lib/librocp_stub_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# Late attach with full configuration (no ptrace, no special privileges):
# Terminal 1:
LD_PRELOAD=build/lib/librocp_stub_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2 — first attach: stub dlopens SDK, force_configure runs,
# wrappers installed only for the requested domains:
build/bin/rocp_ctrl_mmap --pid $! configure --hip --hsa --output json --out trace.json
# ... tracing now active, only HIP+HSA wrappers exist ...

# Reconfigure runtime filter (callbacks check the filter; works without SDK changes):
build/bin/rocp_ctrl_mmap --pid $! reconfigure --hip --hsa --rccl --output json --out trace.json
# Note: RCCL wrappers were NOT installed at first attach. The filter mask only
# affects already-wrapped operations. To add new domains after first attach,
# use rocprofv3 --attach --pid (ptrace).

# Toggle off (context stops, callbacks stop firing):
build/bin/rocp_ctrl_mmap --pid $! deactivate

# Toggle on again with same domain set:
build/bin/rocp_ctrl_mmap --pid $! activate
```

**Note**: the stub library is preloaded but does NOT export `rocprofiler_configure`, so rocprofiler-register's symbol scan does NOT load rocprofiler-sdk. The full SDK is `dlopen`'d only at first attach via `CMD_CONFIGURE`. This achieves true zero hot-path overhead before any controller attaches.

## Limitations

1. **No notification** — The background thread polls the mmap'd control file on a timer (~1 ms). There is no instant wake mechanism (see Option Signal for that enhancement). The hot path itself (`populate_contexts()`) runs on every intercepted call and does not involve the mmap.
2. **Depends on `/run/user/<uid>/`** — Requires systemd's `pam_systemd` or equivalent to create the per-user tmpfs directory. If unavailable, a fallback to `/tmp/` is possible but requires extra care: `/tmp/` is world-writable, so the implementation must use `mkdtemp`-style randomization and `O_NOFOLLOW` to prevent symlink attacks.
3. **No bidirectional communication** — The controller cannot query the library's state (e.g., "how many functions were discovered?"). It can only read the statistics counters.
4. **fork() behavior** — After `fork()`, the child inherits the mmap'd control region but has a different PID. A `pthread_atfork()` child handler should call `rocprofiler_stop_context()` and set `finalize_status = 1` so the child's atexit handler skips cleanup for the parent's control file.
6. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall/memory-access costs and should be validated with benchmarks after implementation.
