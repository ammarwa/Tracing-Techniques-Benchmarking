# Option B: Dispatch Tracer with mmap Regular File Control Channel

## Overview

This design uses a **memory-mapped regular file** as the control channel between the LD_PRELOAD dispatch tracer library and an external controller process. The file lives under the user's private runtime directory (`/run/user/<uid>/`), providing directory-level permission isolation. Both the controller and the library mmap the same file, giving ~1-5 ns atomic load overhead on the hot path with no syscall.

## Initialization: rocprofiler-register Methodology

Instead of using `__attribute__((constructor))` or `dlsym(RTLD_NEXT)`, the dispatch tracer uses the **rocprofiler-register** pattern: each runtime library (HIP, HSA, etc.) registers its API table during its own initialization, and the tracer intercepts the tables during registration.

```
Registration flow:

  1. Runtime library (e.g., libamdhip64.so) initializes
  2. Runtime calls: rocprofiler_register_library_api_table(
         "hip", import_func, version, &api_tables, table_count, &id)
  3. rocprofiler-register scans for rocprofiler_configure symbols
  4. If found, invokes tool callbacks with the API table pointers
  5. Tool (dispatch tracer) receives table pointers, saves originals,
     installs noop shim wrappers that check ctrl->tracing_enabled
  6. Runtime continues — all API calls now go through shims
```

This means:
- **No LD_PRELOAD needed** for the interception itself — the runtime voluntarily passes its API table
- **No `dlsym(RTLD_NEXT)`** — original function pointers come directly from the registration
- **Works with any library** that calls `rocprofiler_register_library_api_table()`
- **Tool discovery** via `rocprofiler_configure` symbol (weak symbol or `ROCP_TOOL_LIBRARIES` env var)

For the benchmark repo's sample library, we simulate this by adding a `register_api_table()` call in `libmylib.so`'s init.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Target Process (sample_app)              │
│                                                              │
│  libmylib.so (runtime library):                             │
│    init():                                                   │
│      api_table.my_traced_function = &real_impl;             │
│      dispatch_register_library_api_table(                   │
│          "mylib", &api_table, 1);                           │
│                                                              │
│  libdispatch_tool.so (tracer — discovered via               │
│                       DISPATCH_TOOL_LIBRARIES env var):      │
│    dispatch_configure():                                     │
│      // Called by register library during registration       │
│      save original: orig_table = copy(api_table);           │
│      install shim:  api_table->my_traced_function =         │
│                       &shim_my_traced_function;             │
│      setup control: create mmap file, init ctrl struct      │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  Shim setup (during registration callback):             │ │
│  │    dir = /run/user/<uid>/dispatch/<pid>/                │ │
│  │    mkdir(dir, 0700)                                     │ │
│  │    fd = open(dir/ctrl, O_CREAT|O_RDWR, 0600)           │ │
│  │    ctrl = mmap(fd, ...)                                 │ │
│  │    ctrl->tracing_enabled = 0  // noop by default        │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐ │
│  │  hot path (every intercepted call):                     │ │
│  │                                                         │ │
│  │    if (__atomic_load_n(&ctrl->tracing_enabled,          │ │
│  │                        __ATOMIC_ACQUIRE)) {             │ │
│  │        uint32_t fmask = __atomic_load_n(                │ │
│  │            &ctrl->func_enable_mask[func_id / 32],       │ │
│  │            __ATOMIC_RELAXED);                           │ │
│  │        if (fmask & (1u << (func_id % 32))) {           │ │
│  │            trace_entry(func_id, args);                  │ │
│  │            real_fn(args);                               │ │
│  │            trace_exit(func_id);                         │ │
│  │            return;                                      │ │
│  │        }                                                │ │
│  │    }                                                    │ │
│  │    real_fn(args);                                       │ │
│  └────────────────────────────────────────────────────────┘ │
│                                                              │
│  File: /run/user/1000/dispatch/12345/ctrl  [mode 0600]      │
│        /run/user/1000/dispatch/            [mode 0700]      │
└──────────────────┬──────────────────────────────────────────┘
                   │ mmap (same physical pages via page cache)
┌──────────────────▼──────────────────────────────────────────┐
│                  Controller (dispatch_ctrl_mmap)              │
│                                                              │
│  fd = open(/run/user/<uid>/dispatch/<pid>/ctrl, O_RDWR)     │
│  ctrl = mmap(fd, PROT_READ|PROT_WRITE, MAP_SHARED)         │
│  verify ctrl->magic                                         │
│                                                              │
│  // ATTACH: write config then enable                        │
│  ctrl->output_format = OUTPUT_JSON;                         │
│  ctrl->ring_buffer_size = 4 * 1024 * 1024;                 │
│  memset(ctrl->func_enable_mask, 0xFF, ...);  // all funcs  │
│  __atomic_store_n(&ctrl->version, v+1, __ATOMIC_RELEASE);  │
│  __atomic_store_n(&ctrl->tracing_enabled, 1,               │
│                   __ATOMIC_RELEASE);                        │
│                                                              │
│  // DETACH:                                                 │
│  __atomic_store_n(&ctrl->tracing_enabled, 0,               │
│                   __ATOMIC_RELEASE);                        │
└─────────────────────────────────────────────────────────────┘
```

## Control Structure

```c
#define DISPATCH_MAGIC 0xD15EA7C0
#define MAX_TRACED_FUNCTIONS 2048  // Enough for HIP (1300+), HSA (400+), etc.
#define DISPATCH_STRUCT_VERSION 1  // Bump on layout changes for compat detection

typedef struct {
    /* Identification */
    uint32_t magic;                      // Must equal DISPATCH_MAGIC
    uint32_t struct_version;             // DISPATCH_STRUCT_VERSION — detects layout mismatches
    uint32_t config_version;             // Bumped on every config change

    /* Master control */
    _Atomic uint32_t tracing_enabled;    // 0 = noop, 1 = trace

    /* Per-function enable bitmask (double-buffered for atomic config swap).
     * The controller writes to the inactive slot [!active_config_slot],
     * then does an atomic CAS on active_config_slot to publish. Readers
     * load active_config_slot with ACQUIRE, then read that slot's bitmask.
     * This eliminates data races during reconfiguration. */
    _Atomic uint32_t active_config_slot; // 0 or 1
    _Atomic uint32_t func_enable_mask[2][MAX_TRACED_FUNCTIONS / 32];

    /* Output configuration (written to inactive slot, swapped atomically) */
    struct {
        uint32_t output_format;          // TEXT=0, JSON=1, PERFETTO=2
        uint32_t ring_buffer_size;       // Bytes, must be power of 2
        char output_path[256];           // Where to write trace data
        char filter_pattern[256];        // Glob include pattern
        char exclude_pattern[256];       // Glob exclude pattern
    } config_slots[2];

    /* Statistics (written by library, read by controller) */
    _Atomic uint64_t events_traced;
    _Atomic uint64_t events_dropped;
} __attribute__((aligned(64))) dispatch_ctrl_t;
// aligned(64) on the struct ensures sizeof is a multiple of a cache line,
// preventing false sharing with adjacent allocations.
```

## Components

### 1. Runtime Library Registration (`libmylib.so` — the traced library)

The runtime library registers its API table during its own initialization. This replaces LD_PRELOAD + `dlsym(RTLD_NEXT)`:

```c
/* In the runtime library (e.g., libmylib.so or libamdhip64.so) */

/* The API table: function pointers for all public APIs */
typedef struct {
    void (*my_traced_function)(int, uint64_t, double, void*);
    void (*set_simulated_work_duration)(unsigned int);
} mylib_api_table_t;

static mylib_api_table_t api_table = {
    .my_traced_function = real_my_traced_function_impl,
    .set_simulated_work_duration = real_set_simulated_work_duration_impl,
};

/* Called during library init — NOT a constructor, but part of the
 * library's normal initialization path. In rocprofiler-sdk, HIP/HSA
 * runtimes call this during their first API call or explicit init. */
void mylib_init(void) {
    /* Register our API table with the registration library.
     * If a tool (dispatch tracer) has registered interest via
     * dispatch_configure, it will be called back with a pointer
     * to our api_table, allowing it to save originals and
     * install shim wrappers. */
    dispatch_register_library_api_table(
        "mylib",                    // library name
        MYLIB_API_VERSION,          // version
        (void**)&api_table,         // pointer to API table
        sizeof(api_table) / sizeof(void(*)(void)),  // function count
        NULL);                      // output identifier
}

/* All public API functions go through the table */
void my_traced_function(int a1, uint64_t a2, double a3, void* a4) {
    api_table.my_traced_function(a1, a2, a3, a4);
}
```

### 2. Dispatch Tool Library (`libdispatch_tool.so` — the tracer)

The tool library provides a `dispatch_configure` function (analogous to `rocprofiler_configure`). The registration library discovers it via `DISPATCH_TOOL_LIBRARIES` environment variable or weak symbol scan.

```c
/* Discovered by the registration library during library registration */
dispatch_tool_configure_result_t* dispatch_configure(
    uint32_t version,
    const char* version_string,
    uint32_t client_id)
{
    static dispatch_tool_configure_result_t result = {
        .initialize = tool_initialize,
        .finalize = tool_finalize,
    };
    return &result;
}

/* Called when a runtime library registers its API table */
static void on_intercept_table_registration(
    const char* lib_name,
    void** api_table,
    size_t func_count)
{
    // Save original function pointers
    memcpy(&orig_table, api_table, func_count * sizeof(void*));

    // Install noop shim wrappers
    ((mylib_api_table_t*)api_table)->my_traced_function =
        shim_my_traced_function;

    // Setup control channel (mmap file)
    setup_mmap_control();
}

static void setup_mmap_control(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/run/user/%u/dispatch",
             (unsigned)getuid());
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "/run/user/%u/dispatch/%d",
             (unsigned)getuid(), getpid());
    if (mkdir(path, 0700) < 0 && errno != EEXIST) { /* handle */ }
    strncat(path, "/ctrl", sizeof(path) - strlen(path) - 1);

    int fd = open(path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    ftruncate(fd, sizeof(dispatch_ctrl_t));
    ctrl = mmap(NULL, sizeof(dispatch_ctrl_t),
                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);  // mapping persists after close

    ctrl->magic = DISPATCH_MAGIC;
    ctrl->struct_version = DISPATCH_STRUCT_VERSION;
    ctrl->active_config_slot = 0;
    ctrl->tracing_enabled = 0;
}

/* Shim: noop by default, traces when enabled */
__attribute__((hot))
static void shim_my_traced_function(
    int arg1, uint64_t arg2, double arg3, void* arg4)
{
    if (__builtin_expect(
            __atomic_load_n(&ctrl->tracing_enabled, __ATOMIC_ACQUIRE), 0)) {
        trace_entry(FUNC_MY_TRACED_FUNCTION, arg1, arg2, arg3, arg4);
        orig_table.my_traced_function(arg1, arg2, arg3, arg4);
        trace_exit(FUNC_MY_TRACED_FUNCTION);
        return;
    }
    orig_table.my_traced_function(arg1, arg2, arg3, arg4);
}
```

### 2. Controller (`dispatch_ctrl_mmap.c`)

```c
int main(int argc, char** argv) {
    pid_t target_pid = parse_args(argc, argv);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/run/user/%u/dispatch/%d/ctrl",
             (unsigned)getuid(), target_pid);

    int fd = open(path, O_RDWR | O_NOFOLLOW);
    if (fd < 0) {
        fprintf(stderr, "Cannot open control file for PID %d "
                "(is the dispatch tracer loaded?)\n", target_pid);
        return 1;
    }

    dispatch_ctrl_t* ctrl = mmap(NULL, sizeof(dispatch_ctrl_t),
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ctrl->magic != DISPATCH_MAGIC) {
        fprintf(stderr, "Invalid magic — wrong version or corrupted\n");
        return 1;
    }

    if (ctrl->struct_version != DISPATCH_STRUCT_VERSION) {
        fprintf(stderr, "Struct version mismatch (expected %d, got %d)\n",
                DISPATCH_STRUCT_VERSION, ctrl->struct_version);
        return 1;
    }

    // Write config to inactive slot, then swap atomically
    uint32_t inactive = !__atomic_load_n(&ctrl->active_config_slot, __ATOMIC_ACQUIRE);
    memset(ctrl->func_enable_mask[inactive], 0xFF,
           sizeof(ctrl->func_enable_mask[0]));
    ctrl->config_slots[inactive].output_format = OUTPUT_TEXT;
    ctrl->config_slots[inactive].ring_buffer_size = 4 * 1024 * 1024;
    // Publish: swap active slot (atomic CAS)
    uint32_t expected = !inactive;
    __atomic_compare_exchange_n(&ctrl->active_config_slot, &expected, inactive,
                                0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    __atomic_store_n(&ctrl->tracing_enabled, 1, __ATOMIC_RELEASE);
    printf("Attached to PID %d, tracing enabled\n", target_pid);

    // Wait for user to press Enter, then disable
    getchar();
    __atomic_store_n(&ctrl->tracing_enabled, 0, __ATOMIC_RELEASE);
    printf("Detached. Events traced: %lu\n",
           __atomic_load_n(&ctrl->events_traced, __ATOMIC_RELAXED));
    return 0;
}
```

### 3. Destructor / Cleanup

```c
__attribute__((destructor))
static void cleanup(void) {
    if (ctrl) {
        munmap(ctrl, sizeof(dispatch_ctrl_t));
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/run/user/%u/dispatch/%d/ctrl",
             (unsigned)getuid(), getpid());
    unlink(path);
    // Remove directory (only succeeds if empty)
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "/run/user/%u/dispatch/%d",
             (unsigned)getuid(), getpid());
    rmdir(dir);
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
| **PID reuse** | After crash, a stale control file with valid magic may match a new unrelated process with the same PID. Controller should verify `/proc/<pid>/stat` start time or check that the LD_PRELOAD library is loaded via `/proc/<pid>/maps` |
| **Stale artifacts** | Control file persists in tmpfs on crash. Cleaned up on user logout (tmpfs). The controller can detect stale entries by checking if the PID is still alive |

## Overhead Profile

| Phase | Cost | Detail |
|-------|------|--------|
| Library init | ~10-50 μs | `mkdir` + `open` + `ftruncate` + `mmap` |
| Controller attach | ~5-20 μs | `open` + `mmap` + config write |
| **Hot-path (noop)** | **~1-5 ns** | Single `__atomic_load_n`, branch-not-taken |
| **Hot-path (tracing)** | **~50-150 ns** | Atomic load + timestamp + ring buffer |
| Config change | ~50-100 ns | Cache-line transfer between cores |
| Controller detach | ~1 μs | Atomic store + munmap |
| Library cleanup | ~5 μs | munmap + unlink + rmdir |

## Multi-Runtime Application (rocprofiler-sdk)

For tracing HIP, HSA, RCCL, OpenMP, rocdecode, rocjpeg simultaneously:

```
/run/user/1000/dispatch/12345/
├── ctrl             # Global enable + per-runtime enable bits
├── hip_funcs        # mmap'd bitmask for 512+ HIP API functions
├── hsa_funcs        # mmap'd bitmask for 256+ HSA API functions
├── rccl_funcs       # mmap'd bitmask for RCCL functions
├── ompt_funcs       # mmap'd bitmask for OpenMP runtime functions
├── rocdecode_funcs  # mmap'd bitmask for rocdecode functions
├── rocjpeg_funcs    # mmap'd bitmask for rocjpeg functions
└── events/          # Directory for trace output
```

Each runtime's LD_PRELOAD wrapper opens and mmaps only its own control file. The controller can configure each runtime independently or atomically (by writing all configs before setting the global enable flag).

## File Layout

```
src/tools/dispatch_tracer_mmap/
├── dispatch_mmap.h          # dispatch_ctrl_t, constants, shared definitions
├── dispatch_mmap_wrapper.c  # LD_PRELOAD library
├── dispatch_mmap_trace.c    # Tracing logic (timestamps, ring buffer output)
└── dispatch_mmap_controller.c  # CLI controller tool
```

## Build Integration (CMakeLists.txt additions)

```cmake
option(BUILD_DISPATCH_MMAP "Build dispatch table tracer (mmap)" ON)

if(BUILD_DISPATCH_MMAP)
    add_library(mylib_dispatch_mmap SHARED
        src/tools/dispatch_tracer_mmap/dispatch_mmap_wrapper.c
        src/tools/dispatch_tracer_mmap/dispatch_mmap_trace.c
    )
    target_link_libraries(mylib_dispatch_mmap PRIVATE dl)
    target_compile_options(mylib_dispatch_mmap PRIVATE -O2 -fPIC)

    add_executable(dispatch_ctrl_mmap
        src/tools/dispatch_tracer_mmap/dispatch_mmap_controller.c
    )
endif()
```

## Benchmark Usage

```bash
# Noop overhead (loaded but not attached):
LD_PRELOAD=build/lib/libmylib_dispatch_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 1000000

# With tracing enabled:
# Terminal 1:
LD_PRELOAD=build/lib/libmylib_dispatch_mmap.so \
  SIMULATED_WORK_US=100 build/bin/sample_app 10000000 &

# Terminal 2:
build/bin/dispatch_ctrl_mmap --pid $! --enable
# Press Enter to detach

# Noop vs attached comparison measures the dispatch table overhead alone
```

## Limitations

1. **No notification** — The library must poll the `tracing_enabled` flag on every call. There is no way for the controller to "wake up" the library. For the dispatch tracer this is fine since the check is on every call anyway.
2. **Depends on `/run/user/<uid>/`** — Requires systemd's `pam_systemd` or equivalent to create the per-user tmpfs directory. If unavailable, a fallback to `/tmp/` is possible but requires extra care: `/tmp/` is world-writable, so the implementation must use `mkdtemp`-style randomization and `O_NOFOLLOW` to prevent symlink attacks.
3. **No bidirectional communication** — The controller cannot query the library's state (e.g., "how many functions were discovered?"). It can only read the statistics counters.
4. **Config struct size fixed at compile time** — `MAX_TRACED_FUNCTIONS=2048` is sufficient for all current ROCm APIs (HIP around 1300, HSA around 400, RCCL around 300). If new runtimes exceed this, bump the constant and `DISPATCH_STRUCT_VERSION`.
5. **fork() behavior** — After `fork()`, the child inherits the mmap'd control region but has a different PID. The child's destructor would unlink a non-existent path. A `pthread_atfork()` child handler should reset `ctrl->tracing_enabled = 0` and set a flag to skip cleanup in the destructor.
6. **Overhead estimates are pre-implementation** — All timing figures are projected from known syscall/memory-access costs and should be validated with benchmarks after implementation.
