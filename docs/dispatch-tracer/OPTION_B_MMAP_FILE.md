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

1. **`rocprofiler_configure`** at process start: creates ONE context and registers callback services for ALL domains the tool might trace (HIP, HSA, RCCL, OMPT, rocdecode, rocjpeg). Context starts inactive.
2. **`tool_initialize` callback**: sets up the mmap control file and spawns a background thread.
3. **Background thread**: reads commands from the mmap. On `CMD_ACTIVATE`/`CMD_DEACTIVATE`, calls `rocprofiler_start_context()` / `rocprofiler_stop_context()`. On `CMD_RECONFIGURE`, atomically updates a runtime filter struct that callbacks read.
4. **Tool callback** (fires for every traced operation when context is active): reads the runtime filter to decide whether to actually emit the event for the controller's chosen output destination/format.

That's it. The wrapper code, dispatch table machinery, and callback infrastructure are untouched.

**Note on late configuration**: rocprofiler-sdk's `rocprofiler_force_configure()` only works before SDK init starts (returns `CONFIGURATION_LOCKED` afterward), so we cannot expand the set of wrapped domains post-init via the control channel. The tool registers all possible domains at init time. The control channel provides late **activation** and runtime **filtering** — sufficient for most use cases. To genuinely add a new domain post-init, the existing `rocprofv3 --attach --pid` ptrace mechanism is the only option. See [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md#late-activation--runtime-filtering-what-the-control-channel-provides) for the full trade-off discussion.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Target Process                           │
│                                                              │
│  Existing rocprofiler-sdk flow (unchanged):                 │
│    Runtime → rocprofiler_set_api_table() → copy_table()     │
│    → update_table() installs wrappers for ALL operations     │
│      across all domains the tool registered interest in      │
│                                                              │
│  Tool library (loaded via ROCP_TOOL_LIBRARIES):             │
│    rocprofiler_configure():                                  │
│      create ONE context                                      │
│      register callback services for ALL domains              │
│        (HIP, HSA, RCCL, OMPT, rocdecode, rocjpeg)            │
│      DO NOT call rocprofiler_start_context yet               │
│    tool_initialize():                                        │
│      create mmap file at                                     │
│        /run/user/<uid>/rocprofiler/<pid>/ctrl                │
│      spawn background thread polling the control file        │
│      // Context exists but is inactive                       │
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

/* tool_initialize — called by SDK after rocprofiler_configure returns.
 * Creates ONE context, registers all domains, leaves it inactive.
 * Then sets up the control channel. */
static rocprofiler_context_id_t saved_ctx;

static int tool_initialize(rocprofiler_client_finalize_t fini, void* tool_data)
{
    /* Create the single context and register interest in all domains.
     * update_table() (run later by SDK) will install wrappers for all
     * operations across these domains. The context starts inactive,
     * so populate_contexts() finds nothing → wrappers noop (~10-20 ns). */
    rocprofiler_create_context(&saved_ctx);

    rocprofiler_configure_callback_tracing_service(
        saved_ctx, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
        NULL, 0, my_callback, NULL);
    rocprofiler_configure_callback_tracing_service(
        saved_ctx, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API,
        NULL, 0, my_callback, NULL);
    rocprofiler_configure_callback_tracing_service(
        saved_ctx, ROCPROFILER_CALLBACK_TRACING_RCCL_API,
        NULL, 0, my_callback, NULL);
    /* ... OMPT, rocdecode, rocjpeg, kernel_dispatch ... */

    /* Set up control channel + spawn background thread */
    setup_mmap_control();
    return 0;
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
    ctrl->context_id = saved_ctx.handle;
    ctrl->pid = getpid();

    pthread_create(&bg_thread, NULL, control_poll_loop, NULL);
}
```

### 1b. Tool Callback (reads runtime filter, decides whether to emit)

```c
/* Updated atomically by the background thread on CMD_RECONFIGURE */
static struct {
    _Atomic uint32_t enabled_domain_mask;  /* which domains to actually emit */
    _Atomic uint32_t output_format;        /* TEXT=0, JSON=1, OTLP=2 */
    char output_path[256];
    char filter_pattern[256];
    char exclude_pattern[256];
} g_runtime_filter;

/* Tool callback fires when the context is active. Cheap atomic read
 * decides whether this event should actually be emitted. */
static void my_callback(rocprofiler_callback_tracing_record_t record,
                        rocprofiler_user_data_t* user_data,
                        void* callback_data)
{
    uint32_t mask = __atomic_load_n(&g_runtime_filter.enabled_domain_mask,
                                    __ATOMIC_ACQUIRE);
    if (!(mask & (1u << record.kind))) return;  /* domain disabled */

    /* Optional name-pattern filter (skip glob_match if patterns empty) */
    /* ... */

    emit_event_to_output(&record);
    __atomic_fetch_add(&ctrl->events_traced, 1, __ATOMIC_RELAXED);
}
```

### 2. Background Thread (polls mmap for commands)

```c
static void apply_runtime_filter(const rocp_config_t* cfg) {
    /* Build domain bitmask from controller's flags */
    uint32_t mask = 0;
    if (cfg->enable_hip)  mask |= (1u << ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API);
    if (cfg->enable_hsa)  mask |= (1u << ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API);
    if (cfg->enable_rccl) mask |= (1u << ROCPROFILER_CALLBACK_TRACING_RCCL_API);
    if (cfg->enable_ompt) mask |= (1u << ROCPROFILER_CALLBACK_TRACING_OMPT);
    /* ... */

    __atomic_store_n(&g_runtime_filter.enabled_domain_mask, mask, __ATOMIC_RELEASE);
    __atomic_store_n(&g_runtime_filter.output_format, cfg->output_format,
                     __ATOMIC_RELEASE);
    /* Strings: copy under a guard if needed; for simplicity assume single controller */
    strncpy(g_runtime_filter.output_path,    cfg->output_path,    255);
    strncpy(g_runtime_filter.filter_pattern, cfg->filter_pattern, 255);
    strncpy(g_runtime_filter.exclude_pattern,cfg->exclude_pattern,255);
}

static void* control_poll_loop(void* arg) {
    uint32_t last_version = 0;
    while (!__atomic_load_n(&shutdown_flag, __ATOMIC_ACQUIRE)) {
        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver != last_version) {
            uint32_t cmd = __atomic_load_n(&ctrl->command, __ATOMIC_ACQUIRE);

            switch (cmd) {
            case CMD_RECONFIGURE:
                /* Update tool-side runtime filter (no SDK call needed) */
                apply_runtime_filter(&ctrl->config);
                break;

            case CMD_ACTIVATE:
                /* Apply filter then start the (already-registered) context */
                apply_runtime_filter(&ctrl->config);
                rocprofiler_start_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
                break;

            case CMD_DEACTIVATE:
                rocprofiler_stop_context(saved_ctx);
                __atomic_store_n(&ctrl->context_active, 0, __ATOMIC_RELEASE);
                break;
            }
            last_version = ver;
        }
        usleep(1000);  /* 1ms poll (or futex_wait on ctrl->version) */
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
| Tool init | ~50-200 μs | Create context + register all domain services + setup mmap + spawn thread |
| **Hot-path (tool loaded, not yet activated)** | **~10-20 ns** | Existing `populate_contexts()` finds inactive context, returns empty |
| **Hot-path (active, callback emits)** | **~50-200 ns** | `populate_contexts()` + enter callbacks + original call + exit callbacks + buffer emplace |
| **Hot-path (active, runtime filter rejects)** | **~30-50 ns** | `populate_contexts()` + callback fires + atomic load of filter mask + return |
| Activate / Deactivate | ~1 ms | Write `CMD_*` + bg thread sees within poll interval, calls start/stop_context |
| Reconfigure (change runtime filter) | ~1 ms | Atomic stores to `g_runtime_filter` — effect immediate on next event |
| Tool cleanup | ~5 μs | `rocprofiler_stop_context` + munmap + unlink + rmdir |

**Key property**: the tool has **~10-20 ns Level 2 noop** per intercepted API call when loaded but inactive. True zero overhead requires not loading the tool at all (don't set `ROCP_TOOL_LIBRARIES`).

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
# Tool loaded, context inactive — ~10-20 ns Level 2 noop per intercepted call:
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# Late activation with filter (no ptrace):
# Terminal 1:
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2 — activate with filter (HIP+HSA emit events, others suppressed):
build/bin/rocp_ctrl_mmap --pid $! activate --hip --hsa --output json --out trace.json
# ... tracing now active, callbacks emit only HIP+HSA events ...

# Reconfigure runtime filter to also emit RCCL (callbacks were already firing,
# they just weren't emitting RCCL events; now they will):
build/bin/rocp_ctrl_mmap --pid $! reconfigure --hip --hsa --rccl --output json --out trace.json

# Toggle off (context stops, callbacks stop firing):
build/bin/rocp_ctrl_mmap --pid $! deactivate

# Toggle on again with same config:
build/bin/rocp_ctrl_mmap --pid $! activate --hip --hsa --rccl --output json --out trace.json
```

**Note**: the tool always registers all domains at init time, so wrappers are installed for every API. The runtime filter just decides which events to emit. To genuinely add a domain not registered at init (for example, OMPT in a tool that didn't enable it at compile time), you must use `rocprofv3 --attach --pid` (ptrace) since rocprofiler-sdk's `rocprofiler_force_configure()` is locked after init.

## Limitations

1. **No notification** — The background thread polls the mmap'd control file on a timer (~1 ms). There is no instant wake mechanism (see Option Signal for that enhancement). The hot path itself (`populate_contexts()`) runs on every intercepted call and does not involve the mmap.
2. **Depends on `/run/user/<uid>/`** — Requires systemd's `pam_systemd` or equivalent to create the per-user tmpfs directory. If unavailable, a fallback to `/tmp/` is possible but requires extra care: `/tmp/` is world-writable, so the implementation must use `mkdtemp`-style randomization and `O_NOFOLLOW` to prevent symlink attacks.
3. **No bidirectional communication** — The controller cannot query the library's state (e.g., "how many functions were discovered?"). It can only read the statistics counters.
4. **fork() behavior** — After `fork()`, the child inherits the mmap'd control region but has a different PID. A `pthread_atfork()` child handler should call `rocprofiler_stop_context()` and set `finalize_status = 1` so the child's atexit handler skips cleanup for the parent's control file.
6. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall/memory-access costs and should be validated with benchmarks after implementation.
