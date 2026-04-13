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

1. **Tool `initialize` callback**: creates the mmap control file and a background thread that polls it
2. **Background thread**: reads the mmap'd control struct; when it sees `enabled=1`, calls `rocprofiler_start_context(ctx_id)`; when `enabled=0`, calls `rocprofiler_stop_context(ctx_id)`
3. **`should_wrap_functor` override**: the tool registers interest in all operations during `rocprofiler_configure`, so wrappers are installed for everything (Level 1 noop is bypassed, but Level 2 noop still works at ~10-20 ns)

That's it. The wrapper code, dispatch table machinery, and callback infrastructure are untouched.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Target Process                           │
│                                                              │
│  Existing rocprofiler-sdk flow (unchanged):                 │
│    Runtime → rocprofiler_set_api_table() → copy_table()     │
│    → update_table() installs functor wrappers                │
│                                                              │
│  Tool library (loaded via ROCP_TOOL_LIBRARIES):             │
│    rocprofiler_configure():                                  │
│      create context with callback/buffer tracing             │
│      register interest in ALL HIP/HSA/RCCL/OMPT operations  │
│      DO NOT activate context yet (stays inactive)            │
│    initialize():                                             │
│      create mmap control file at                             │
│        /run/user/<uid>/rocprofiler/<pid>/ctrl                │
│      spawn background thread polling the control file        │
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
    CMD_ACTIVATE   = 1,   // Activate the context (start tracing)
    CMD_DEACTIVATE = 2,   // Deactivate the context (stop tracing)
};

typedef struct {
    /* Identification */
    uint32_t magic;              // Must equal ROCP_CTRL_MAGIC
    uint32_t struct_version;     // ROCP_CTRL_VERSION

    /* Command channel (controller → tool) */
    _Atomic uint32_t command;    // rocp_ctrl_command
    _Atomic uint32_t version;    // Bumped by controller on every command

    /* Status (tool → controller, read-only from controller side) */
    _Atomic uint32_t context_active;  // 0 = inactive, 1 = active
    _Atomic uint32_t context_id;      // rocprofiler_context_id_t.handle
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

/* tool_initialize — called after all runtimes registered */
static void tool_initialize(rocprofiler_client_finalize_t fini_func,
                             void* tool_data)
{
    /* 1. Create context with callback+buffer tracing (existing API) */
    rocprofiler_context_id_t ctx;
    rocprofiler_create_context(&ctx);

    /* 2. Register interest in ALL HIP/HSA/RCCL operations.
     *    This causes update_table() to install wrappers for everything.
     *    (NULL operations = all operations in the domain) */
    rocprofiler_configure_callback_tracing_service(
        ctx, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
        NULL, 0, my_callback, NULL);
    rocprofiler_configure_callback_tracing_service(
        ctx, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API,
        NULL, 0, my_callback, NULL);
    /* ... repeat for RCCL, OMPT, rocdecode, rocjpeg ... */

    /* 3. DO NOT activate context yet — stays inactive.
     *    populate_contexts() will find nothing → wrappers noop. */

    /* 4. Setup mmap control channel (the only new code) */
    setup_mmap_control(ctx);
}

static void setup_mmap_control(rocprofiler_context_id_t ctx) {
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
    ctrl->context_id = ctx.handle;
    ctrl->pid = getpid();

    /* Spawn background thread to poll control file */
    saved_ctx = ctx;
    pthread_create(&bg_thread, NULL, control_poll_loop, NULL);
}
```

### 2. Background Thread (polls mmap for commands)

```c
static void* control_poll_loop(void* arg) {
    uint32_t last_version = 0;
    while (!__atomic_load_n(&shutdown_flag, __ATOMIC_ACQUIRE)) {
        uint32_t ver = __atomic_load_n(&ctrl->version, __ATOMIC_ACQUIRE);
        if (ver != last_version) {
            uint32_t cmd = __atomic_load_n(&ctrl->command, __ATOMIC_ACQUIRE);
            if (cmd == CMD_ACTIVATE && !ctrl->context_active) {
                rocprofiler_start_context(saved_ctx);   // EXISTING API
                __atomic_store_n(&ctrl->context_active, 1, __ATOMIC_RELEASE);
            } else if (cmd == CMD_DEACTIVATE && ctrl->context_active) {
                rocprofiler_stop_context(saved_ctx);    // EXISTING API
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

    if (strcmp(action, "activate") == 0) {
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
| Tool init | ~10-50 μs | `mkdir` + `open` + `ftruncate` + `mmap` + `pthread_create` |
| Controller attach | ~5-20 μs | `open` + `mmap` + write CMD_ACTIVATE |
| **Hot-path (noop)** | **~10-20 ns** | Existing `populate_contexts()` — iterates active contexts, finds none |
| **Hot-path (tracing)** | **~50-200 ns** | `populate_contexts()` + enter callbacks + original call + exit callbacks + buffer emplace |
| Context toggle latency | ~1 ms | Background thread poll interval (or futex wake) |
| Controller detach | ~1 μs | Write CMD_DEACTIVATE + munmap |
| Tool cleanup | ~5 μs | `rocprofiler_stop_context` + munmap + unlink + rmdir |

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
# Noop overhead (tool loaded, context not activated):
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing:
# Terminal 1:
ROCP_TOOL_LIBRARIES=build/lib/librocprofiler_tool_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/rocp_ctrl_mmap --pid $! activate
# ... tracing active ...
build/bin/rocp_ctrl_mmap --pid $! deactivate
```

## Limitations

1. **No notification** — The background thread polls the mmap'd control file on a timer (~1 ms). There is no instant wake mechanism (see Option Signal for that enhancement). The hot path itself (`populate_contexts()`) runs on every intercepted call and does not involve the mmap.
2. **Depends on `/run/user/<uid>/`** — Requires systemd's `pam_systemd` or equivalent to create the per-user tmpfs directory. If unavailable, a fallback to `/tmp/` is possible but requires extra care: `/tmp/` is world-writable, so the implementation must use `mkdtemp`-style randomization and `O_NOFOLLOW` to prevent symlink attacks.
3. **No bidirectional communication** — The controller cannot query the library's state (e.g., "how many functions were discovered?"). It can only read the statistics counters.
4. **fork() behavior** — After `fork()`, the child inherits the mmap'd control region but has a different PID. A `pthread_atfork()` child handler should call `rocprofiler_stop_context()` and set `finalize_status = 1` so the child's atexit handler skips cleanup for the parent's control file.
6. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall/memory-access costs and should be validated with benchmarks after implementation.
