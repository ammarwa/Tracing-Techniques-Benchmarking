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

## What Changes (Minimal)

1. **Placeholder `rocprofiler_configure`** at process start: returns a tool that does nothing — no context created, no domains registered. This means `update_table()` installs ZERO wrappers and the application runs at 100% native speed when no controller is attached.
2. **Tool `initialize` callback**: only sets up the mmap control file and a background thread.
3. **Background thread**: reads commands from the mmap. On `CMD_CONFIGURE`, stashes the config and calls `rocprofiler_force_configure(real_configure)`. On `CMD_ACTIVATE` / `CMD_DEACTIVATE`, calls `rocprofiler_start_context()` / `rocprofiler_stop_context()`.
4. **Real configure callback** (called by SDK during `force_configure`): creates the context with the controller-specified domains, services, and output settings. The SDK automatically re-propagates runtime API tables, installing wrappers for the newly-interested operations.

That's it. The wrapper code, dispatch table machinery, and callback infrastructure are untouched. **Late configuration is fully supported** — no ptrace required.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Target Process                           │
│                                                              │
│  Existing rocprofiler-sdk flow (unchanged):                 │
│    Runtime → rocprofiler_set_api_table() → copy_table()     │
│    → update_table() — installs ZERO wrappers initially       │
│      (placeholder tool registered no contexts)               │
│                                                              │
│  Tool library (loaded via ROCP_TOOL_LIBRARIES):             │
│    placeholder_configure():                                  │
│      // Phase 1: do NOTHING                                  │
│      // No context, no domains, no services                  │
│      return result with initialize=phase1_init only          │
│    phase1_init():                                            │
│      // Only setup the control channel                       │
│      create mmap file at                                     │
│        /run/user/<uid>/rocprofiler/<pid>/ctrl                │
│      spawn background thread polling the control file        │
│      // No rocprofiler context exists yet                    │
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
    CMD_NONE       = 0,   // No pending command
    CMD_CONFIGURE  = 1,   // Apply config, create/recreate context (force_configure)
    CMD_ACTIVATE   = 2,   // Start an existing configured context
    CMD_DEACTIVATE = 3,   // Stop the context (wrappers stay, Level 2 noop)
};

/* Configuration the controller sends with CMD_CONFIGURE.
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

    /* Configuration (controller writes, tool reads on CMD_CONFIGURE) */
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

### 1. Tool Library (`librocprofiler-sdk-tool.so` — uses existing rocprofiler-sdk APIs)

The tool uses standard rocprofiler-sdk APIs. No custom dispatch table, no custom wrappers, no custom bitmasks. The only new code is the mmap control channel setup and background thread.

```c
/* rocprofiler_configure — discovered via ROCP_TOOL_LIBRARIES */
rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t version, const char* runtime_version,
                      uint32_t priority, rocprofiler_client_id_t* id)
{
    *id = (rocprofiler_client_id_t){.name = "mmap-channel-tool"};
    static rocprofiler_tool_configure_result_t result = {
        .size = sizeof(result),
        .initialize = tool_initialize,
        .finalize = tool_finalize,
    };
    return &result;
}

/* phase1_init — placeholder, only sets up the control channel.
 * No context, no domains, no services.
 * update_table() installs ZERO wrappers — application runs at native speed. */
static void phase1_init(rocprofiler_client_finalize_t fini_func, void* tool_data)
{
    setup_mmap_control();  /* Only new code at process start */
}

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
    ctrl->pid = getpid();

    /* Spawn background thread — context will be created later on attach */
    pthread_create(&bg_thread, NULL, control_poll_loop, NULL);
}
```

### 1b. Real Configure (Phase 2 — invoked at attach via `force_configure`)

```c
/* Stash for the new configure callback to read */
static rocp_config_t g_pending_config;
static rocprofiler_context_id_t saved_ctx = {0};

/* Called by the SDK when the bg thread invokes rocprofiler_force_configure() */
rocprofiler_tool_configure_result_t*
real_tool_configure(uint32_t version, const char* runtime_version,
                    uint32_t priority, rocprofiler_client_id_t* id)
{
    *id = (rocprofiler_client_id_t){.name = "rocp-mmap-real"};
    static rocprofiler_tool_configure_result_t result = {
        .size = sizeof(result),
        .initialize = real_tool_initialize,
        .finalize   = real_tool_finalize,
    };
    return &result;
}

/* Creates context with controller-specified domains.
 * SDK then re-propagates runtime API tables, installing wrappers
 * only for operations this context cares about. */
static void real_tool_initialize(rocprofiler_client_finalize_t fini, void* d)
{
    rocprofiler_context_id_t ctx;
    rocprofiler_create_context(&ctx);

    if (g_pending_config.enable_hip)
        rocprofiler_configure_callback_tracing_service(
            ctx, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
            NULL, 0, my_callback, NULL);
    if (g_pending_config.enable_hsa)
        rocprofiler_configure_callback_tracing_service(
            ctx, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API,
            NULL, 0, my_callback, NULL);
    if (g_pending_config.enable_rccl)
        rocprofiler_configure_callback_tracing_service(
            ctx, ROCPROFILER_CALLBACK_TRACING_RCCL_API,
            NULL, 0, my_callback, NULL);
    /* ... OMPT, rocdecode, rocjpeg per controller config ... */

    saved_ctx = ctx;
    __atomic_store_n(&ctrl->context_id, ctx.handle, __ATOMIC_RELEASE);
    rocprofiler_start_context(ctx);
    __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
}
```

### 2. Background Thread (polls mmap for commands)

```c
static void* control_poll_loop(void* arg) {
    uint32_t last_version = 0;
    bool first_configure = true;

    while (!__atomic_load_n(&shutdown_flag, __ATOMIC_ACQUIRE)) {
        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver != last_version) {
            uint32_t cmd = __atomic_load_n(&ctrl->command, __ATOMIC_ACQUIRE);

            if (cmd == CMD_CONFIGURE) {
                /* Read controller's config from the mmap */
                memcpy(&g_pending_config, &ctrl->config, sizeof(g_pending_config));

                if (first_configure) {
                    /* First attach: trigger late configuration via SDK API.
                     * SDK will call real_tool_configure → real_tool_initialize
                     * → re-propagate runtime API tables → install wrappers */
                    rocprofiler_force_configure(real_tool_configure);
                    first_configure = false;
                } else {
                    /* Reconfigure: stop current context, force re-configure
                     * with new domains, start new context */
                    rocprofiler_stop_context(saved_ctx);
                    rocprofiler_force_configure(real_tool_configure);
                    /* real_tool_initialize creates new context and starts it */
                }

            } else if (cmd == CMD_ACTIVATE && saved_ctx.handle != 0) {
                rocprofiler_start_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);

            } else if (cmd == CMD_DEACTIVATE && saved_ctx.handle != 0) {
                rocprofiler_stop_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 0, __ATOMIC_RELEASE);
            }
            last_version = ver;
        }
        usleep(1000);  // 1ms poll (or futex_wait on ctrl->version)
    }
    return NULL;
}
```

### 3. Controller (external process)

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

    if (strcmp(action, "configure") == 0) {
        /* First-time attach OR reconfigure: write config, send CMD_CONFIGURE.
         * The tool's bg thread will call rocprofiler_force_configure(),
         * which causes wrappers to be installed for the requested domains. */
        ctrl->config.enable_hip       = parse_flag(argc, argv, "--hip");
        ctrl->config.enable_hsa       = parse_flag(argc, argv, "--hsa");
        ctrl->config.enable_rccl      = parse_flag(argc, argv, "--rccl");
        ctrl->config.enable_ompt      = parse_flag(argc, argv, "--ompt");
        ctrl->config.output_format    = parse_format(argc, argv);
        ctrl->config.buffer_size_kb   = parse_int(argc, argv, "--buf-kb", 4096);
        strncpy(ctrl->config.output_path, parse_str(argc, argv, "--out"), 255);

        __atomic_store_n(&ctrl->command, CMD_CONFIGURE, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->version, ctrl->version + 1, __ATOMIC_RELEASE);
        printf("Configured & activated tracing for PID %d\n", target_pid);
    } else if (strcmp(action, "activate") == 0) {
        __atomic_store_n(&ctrl->command, CMD_ACTIVATE, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->version, ctrl->version + 1, __ATOMIC_RELEASE);
        printf("Activated context %u for PID %d\n", ctrl->context_id, target_pid);
    } else {
        __atomic_store_n(&ctrl->command, CMD_DEACTIVATE, __ATOMIC_RELAXED);
        __atomic_store_n(&ctrl->version, ctrl->version + 1, __ATOMIC_RELEASE);
        printf("Deactivated. Events: %lu\n", ctrl->events_traced);
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
| Tool init (Phase 1, no context) | ~10-50 μs | `mkdir` + `open` + `ftruncate` + `mmap` + `pthread_create` |
| **Hot-path before any attach** | **0 ns** | No wrappers installed — original function pointers in dispatch table |
| Controller attach + first configure | ~5-50 ms | `mmap` + `force_configure` + re-propagate runtime API tables + `update_table` |
| **Hot-path (tool configured but inactive)** | **~10-20 ns** | Existing `populate_contexts()` finds no active context |
| **Hot-path (tool configured and active)** | **~50-200 ns** | `populate_contexts()` + enter callbacks + original call + exit callbacks + buffer emplace |
| Activate / Deactivate (after configure) | ~1 μs | Write `CMD_*` + bg thread sees within 1 ms |
| Reconfigure (add new domains) | ~5-50 ms | Same as first configure (re-run propagation) |
| Tool cleanup | ~5 μs | `rocprofiler_stop_context` + munmap + unlink + rmdir |

**Key property**: when no controller ever attaches, the tool has **zero hot-path overhead** because no wrappers are installed. The application runs at 100% native speed.

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
├── rocp_mmap.h              # rocp_ctrl_t, constants, shared definitions
├── rocp_mmap_tool.c         # rocprofiler tool library (configure + initialize + mmap)
└── rocp_mmap_controller.c   # CLI controller tool
```

## Build Integration (CMakeLists.txt additions)

```cmake
option(BUILD_ROCP_TOOL_MMAP "Build rocprofiler tool with mmap control channel" ON)

if(BUILD_ROCP_TOOL_MMAP)
    add_library(rocprofiler_tool_mmap SHARED
        src/tools/rocprofiler_tool_mmap/rocp_mmap_tool.c
    )
    target_link_libraries(rocprofiler_tool_mmap PRIVATE pthread)
    target_compile_options(rocprofiler_tool_mmap PRIVATE -O2 -fPIC)

    add_executable(rocp_ctrl_mmap
        src/tools/rocprofiler_tool_mmap/rocp_mmap_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Zero overhead (tool loaded, no controller attached, no wrappers installed):
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# Late configuration + activation (true late attach, no ptrace):
# Terminal 1:
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2 — first attach: configure which APIs to trace:
build/bin/rocp_ctrl_mmap --pid $! configure --hip --hsa --output json --out trace.json
# ... tracing now active for HIP+HSA only ...

# Toggle off without losing config:
build/bin/rocp_ctrl_mmap --pid $! deactivate

# Toggle on again:
build/bin/rocp_ctrl_mmap --pid $! activate

# Reconfigure mid-run to add RCCL:
build/bin/rocp_ctrl_mmap --pid $! configure --hip --hsa --rccl --output json --out trace.json
```

## Limitations

1. **No notification** — The background thread polls the mmap'd control file on a timer (~1 ms). There is no instant wake mechanism (see Option Signal for that enhancement). The hot path itself (`populate_contexts()`) runs on every intercepted call and does not involve the mmap.
2. **Depends on `/run/user/<uid>/`** — Requires systemd's `pam_systemd` or equivalent to create the per-user tmpfs directory. If unavailable, a fallback to `/tmp/` is possible but requires extra care: `/tmp/` is world-writable, so the implementation must use `mkdtemp`-style randomization and `O_NOFOLLOW` to prevent symlink attacks.
3. **No bidirectional communication** — The controller cannot query the library's state (e.g., "how many functions were discovered?"). It can only read the statistics counters.
4. **fork() behavior** — After `fork()`, the child inherits the mmap'd control region but has a different PID. A `pthread_atfork()` child handler should call `rocprofiler_stop_context()` and set `finalize_status = 1` so the child's atexit handler skips cleanup for the parent's control file.
6. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall/memory-access costs and should be validated with benchmarks after implementation.
