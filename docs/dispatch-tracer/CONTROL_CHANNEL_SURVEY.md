# Dispatch Tracer Control Channel Survey

## Overview

This document surveys all control channel mechanisms considered for a new tracing technique: the **Dispatch Table Tracer**. This tracer intercepts API calls via a function dispatch table that starts as a **noop passthrough** and only activates tracing when an external controller process attaches at runtime with configuration.

The core architecture is inspired by [rocprofiler-sdk](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-sdk), which uses intercept tables that start as passthrough and are swapped to tracing wrappers when a profiling tool attaches. The goal is to bring this pattern to the Tracing-Techniques-Benchmarking framework and evaluate it against the existing LTTng, eBPF, and bpftime techniques.

### How rocprofiler-sdk's Dispatch Table Already Works

The existing rocprofiler-sdk already implements a dispatch table with a two-level noop fast-path. Understanding it precisely reveals what we can reuse and what the minimal change is.

**Existing flow (no changes needed):**

1. Runtime (HIP/HSA) calls `rocprofiler_set_api_table(name, version, instance, &tables, count)` during its init, passing its mutable function pointer table.
2. `copy_table()` saves the original function pointers into a static singleton.
3. `update_table()` iterates every `OpIdx` and checks `should_wrap_functor()` which queries all registered contexts. Only operations with interested contexts get wrappers; the rest keep original pointers (**Level 1 noop: zero overhead**).
4. Installed wrappers use `hip_api_impl<TableIdx, OpIdx>::functor()` which at call time calls `populate_contexts()` to check active contexts. If `callback_contexts` and `buffered_contexts` are both empty, it calls the original directly (**Level 2 noop: ~10-20 ns**).
5. When contexts ARE active, the wrapper fires enter callbacks, calls the original, fires exit callbacks, and emplaces buffer records.

**The noop overhead today** (wrapper installed but no active context): ~10-20 ns per call. `populate_contexts()` iterates active contexts (bitset check), finds none, returns.

### Applicability preconditions — read this before the mechanism

The late-load stub design has three hard preconditions. If any are violated the whole mechanism silently degrades or fails, so they are called out here rather than buried in the failure-mode analysis:

1. **No in-process rocprofiler-sdk tool may already be loaded.** `rocprofiler_force_configure()` checks `get_init_status() != 0` and returns `ROCPROFILER_STATUS_ERROR_CONFIGURATION_LOCKED` if the SDK has already initialized. A user running rocprofv3 *and then* trying to attach our OOP controller cannot succeed — the attach silently fails with a locked-configuration error. Workaround today: do not combine the two; use ptrace attach instead.
2. **`OMP_TOOL_LIBRARIES` must be unset or set to a compatible tool.** OpenMP's `ompt_start_tool` scan is one-shot and first-match-wins; our stub's future OMPT export would be preempted by any other library preloaded via `OMP_TOOL_LIBRARIES` that also exports the symbol.
3. **The stub must be `LD_PRELOAD`'d before any runtime init.** If libamdhip64/libhsa-runtime64 has already completed its `rocprofiler_register_library_api_table` call before the stub's constructor runs (e.g., the stub is `dlopen`'d mid-execution), rocprofiler-register will have already scanned for `rocprofiler_configure` and moved on — attach will not work.

the rocprofiler-sdk-shim alternative (see [SHIM_COMPARISON.md](SHIM_COMPARISON.md)) relaxes precondition 1 (natural in-process+OOP coexistence via the shim's `functor` layering) and precondition 3 (the shim is always dlopen'd by register, no user opt-in needed) at the cost of a measured +0.8 ns/call always-on wrapper. For the long-lived product, the shim design is the right shape; our stub design is appropriate as a mock that validates the control-channel and security properties with zero changes to rocprofiler-sdk/register.

### Late-Load Design: Defer rocprofiler-sdk Loading Until Attach

The key insight from rocprofiler-sdk's architecture is that there are **two libraries**:

- **rocprofiler-register**: Linked into HIP/HSA/RCCL runtimes. Loaded at process start. Stores API table pointers when runtimes call `rocprofiler_register_library_api_table()`. Scans for the `rocprofiler_configure` symbol — if found, `dlopen`s rocprofiler-sdk and hands over the tables. If not found, **rocprofiler-sdk is never loaded** and the dispatch tables keep their original function pointers (zero overhead).

- **rocprofiler-sdk**: The full profiler. Provides `rocprofiler_set_api_table()`, `copy_table`, `update_table`, the functor wrappers, `populate_contexts`, etc. Loaded only when a tool with `rocprofiler_configure` is present.

**The leverage point:** `rocprofiler_force_configure()` checks `get_init_status() != 0` — but this is the **rocprofiler-sdk's** init status, not the runtimes'. If rocprofiler-sdk has not yet been loaded into the address space, `get_init_status() == 0` and `force_configure()` works. The CHANGELOG entry "Late-start profiling support: Automatic profiling activation when rocprofiler-sdk loads after runtime initialization" describes exactly this scenario.

**Late-load tool design:**

**Process start:**
- HIP/HSA/RCCL runtimes load and link in rocprofiler-register (always present)
- A small **control channel stub library** is loaded (via `LD_PRELOAD`, or via a preload mechanism)
- The stub does NOT export `rocprofiler_configure` — so rocprofiler-register's scan finds no tool and does NOT `dlopen` rocprofiler-sdk
- Stub sets up the IPC control channel and spawns a background thread
- Runtimes call `rocprofiler_register_library_api_table()` → register-lib stores the tables but installs no wrappers
- **Hot path: 0 ns. Original function pointers in dispatch tables.**

**Controller attaches:**
- Controller writes full config (which domains, output format, buffers, filters) and sends `CMD_CONFIGURE`
- Stub's background thread:
  1. Stashes the config in a static struct
  2. `dlopen("librocprofiler-sdk-tool.so")` — this brings rocprofiler-sdk into the address space along with a `rocprofiler_configure` symbol
  3. Calls `rocprofiler_force_configure(real_tool_configure)` — works because rocprofiler-sdk's `init_status` is still 0
  4. SDK initializes:
     - Calls `real_tool_configure()` → returns `tool_initialize` callback
     - Calls `tool_initialize()` → creates context, registers callback services for the controller's selected domains
     - Calls `invoke_register_propagation()` → re-replays `rocprofiler_set_api_table()` for every runtime that registered before SDK loaded
     - For each table, `copy_table()` saves originals and `update_table()` installs wrappers for the operations the new context cares about
  5. `rocprofiler_start_context(ctx)` activates tracing

**Reconfigure (add new domains):**
- Once SDK is loaded, `force_configure()` is locked. To add new domains, the tool can:
  - Use `CMD_RECONFIGURE` to update tool-side filtering (which already-wrapped operations to actually emit) — works without SDK calls
  - For genuinely new domains not registered at first attach, fall back to `rocprofv3 --attach --pid` (ptrace)

**Detach:**
- `CMD_DEACTIVATE` → `rocprofiler_stop_context(ctx)` — wrappers stay but Level 2 noop kicks in (~10-20 ns)

### What This Gives Us

| Property | Achieved? |
|---|---|
| Zero overhead before any controller attaches | ✅ Yes — rocprofiler-sdk not loaded, no wrappers |
| Late attach without ptrace | ✅ Yes — `rocprofiler_force_configure()` works while SDK init_status is 0 |
| Late configuration of which APIs to trace | ✅ Yes — controller specifies at attach, before SDK loads |
| Late configuration of output format / buffers | ✅ Yes — passed via control channel to the configure callback |
| Multi-runtime (HIP, HSA, RCCL, OMPT) | ✅ Yes — propagation re-replays for all registered runtimes |
| Works on any architecture (no ptrace) | ✅ Yes — pure dlopen + C API calls |
| Add new domains after first attach | ⚠️ Only via tool-side filter (wrappers already chosen at first attach) |

### Why This Works (Mechanism Summary)

```
Process start:
  HIP runtime loads → links rocprofiler-register
  Stub library loaded (no rocprofiler_configure symbol)
  HIP calls rocprofiler_register_library_api_table("hip", ...)
    → register-lib stores HIP table pointer
    → register-lib scans for rocprofiler_configure → not found
    → register-lib does NOT dlopen rocprofiler-sdk
  HIP table is untouched. Application runs at native speed.

Controller attaches:
  Stub bg thread receives CMD_CONFIGURE with config
  Stub dlopen("librocprofiler-sdk-tool.so")
    → SDK code now in address space
    → rocprofiler_configure symbol now visible
  Stub calls rocprofiler_force_configure(real_tool_configure)
    → SDK get_init_status() == 0 ✓
    → forced_config == nullptr ✓
    → SDK initialize() runs (status 0 → 1):
       - calls real_tool_configure() → returns tool_initialize
       - calls tool_initialize() → creates context, registers domains per config
    → invoke_register_propagation() runs:
       - rocprofiler-register replays rocprofiler_set_api_table("hip", ...)
       - SDK's copy_table saves originals, update_table installs HIP wrappers
       - Same for HSA, RCCL, OMPT, etc.
  Stub calls rocprofiler_start_context(ctx)
  Tracing now active for the controller's chosen domains.
```

### What Exactly Gets `LD_PRELOAD`'d — and What Does NOT

This is the single most common source of confusion in this design. To be precise:

**`LD_PRELOAD=librocp_stub_<channel>.so` — that is it.** The stub is a small library (≈20 KiB) we ship with the dispatch tracer. Nothing else in the dispatch-tracer workflow uses `LD_PRELOAD`.

In particular:

| Library | How it enters the process | Who `LD_PRELOAD`s it? |
|---|---|---|
| **librocp_stub_&lt;channel&gt;.so** (ours) | `LD_PRELOAD=...` on the user's command | **The user** (this is the new thing) |
| **librocprofiler-register.so.0** | Automatic — it is a `DT_NEEDED` dependency of libamdhip64 / libhsa-runtime64 / libomptarget / librccl. The dynamic linker loads it the moment HIP/HSA/OpenMP/RCCL load. | Nobody — the runtimes already link to it |
| **librocprofiler-sdk.so** | `dlopen`'d by the stub on first `CMD_CONFIGURE` — **not** present at startup, **not** preloaded | The stub, at attach time |
| **libmock_sdk_tool_&lt;channel&gt;.so** (or a real tool) | `dlopen`'d by the stub on first `CMD_CONFIGURE` (it exports `rocprofiler_configure`, which causes rocprofiler-register's scan to succeed and load librocprofiler-sdk as a side effect of SDK-tool's link graph) | The stub, at attach time |

Three consequences follow from this:

1. **Users who never run a profiler do not touch `LD_PRELOAD` at all.** The stub is opt-in; it is there only for sessions where the user wants the possibility of attaching later.
2. **rocprofiler-register is already in every HIP/HSA/OpenMP/RCCL process today, with or without our stub.** That is how its symbol-scan for `rocprofiler_configure` currently works — it runs from the runtime's own constructor path. We do not add it; it was already there.
3. **rocprofiler-sdk (the big profiler) is never in memory until the user explicitly attaches.** This is what buys the "0 ns when no controller attached" property. If the user never attaches, the SDK is never `mmap`'d into the process.

So "`LD_PRELOAD` the dispatch tracer" does **not** mean preloading the SDK, and does **not** mean preloading rocprofiler-register. It means preloading ~20 KiB of glue whose only job at process start is to bind a socket (or mmap a file, or install a signal handler) and block on a background thread.

**Symbol-export contract** (verified via `nm -D` on the built artifacts):

| Symbol | Exported by stub? | Exported by tool library? |
|---|:---:|:---:|
| `rocprofiler_configure` (weak ref in register) | **no** — this is what keeps the SDK from loading at startup | **yes** — when the tool is `dlopen`'d, the symbol appears via `RTLD_DEFAULT` and register's rescan succeeds |
| `ompt_start_tool` (weak ref in OpenMP runtime) | **yes, but silent** — see OpenMP section below | no (OpenMP only scans at OMPT init, which has already happened) |
| `rocp_stub_get_state` (stub↔tool accessor) | yes | no |

### OpenMP / OMPT — A Different Registration Path

OMPT is **not** a dispatch table. Unlike HIP/HSA/RCCL — which publish mutable function-pointer tables and let rocprofiler-sdk's `update_table()` swap pointers — OpenMP uses a one-shot callback-registration model defined by the OMPT spec. This affects our stub in a subtle but important way.

**How OMPT tool discovery works in OpenMP runtimes today** (confirmed in `rocprofiler-sdk/source/lib/rocprofiler-sdk/ompt.cpp:243-251` and in the OMPT 5.0+ specification):

1. At OpenMP init, the runtime calls `dlsym(RTLD_DEFAULT, "ompt_start_tool")`.
2. If found, the runtime calls `ompt_start_tool(omp_version, runtime_version)`, which returns a `ompt_start_tool_result_t*` containing pointers to `initialize`, `finalize`, and tool data.
3. OpenMP calls `result->initialize(lookup, initial_device_num, tool_data)`, passing an `ompt_function_lookup_t`.
4. Inside `initialize`, the tool resolves `ompt_set_callback` via `lookup("ompt_set_callback")` and calls it once per event it wants to observe. **After `initialize` returns, the runtime never scans again.** (See rocprofiler-sdk's `initialize()` function at `ompt.cpp:148-180` for the canonical implementation.)
5. Importantly, `ompt_set_callback` itself can be invoked at any time after `initialize` returns — the tool is free to save the function pointer and use it to install, swap, or unregister callbacks later.

**What this means for late-load:**

The naive late-load pattern (stub exports no symbols, SDK is `dlopen`'d at attach) **does not work for OMPT**, because the `ompt_start_tool` rendezvous has already passed by the time the controller attaches. OpenMP's runtime has already done its `dlsym`, found nothing, and moved on.

**Our handling:** the stub does export `ompt_start_tool`, but the implementation is trivial:

```c
/* In stub — same shape for all four channels. */
static ompt_set_callback_t     g_ompt_set_callback = NULL;
static ompt_function_lookup_t  g_ompt_lookup       = NULL;
static _Atomic int             g_ompt_ready        = 0;

static int stub_ompt_initialize(ompt_function_lookup_t lookup,
                                int device_num, ompt_data_t* tool_data)
{
    /* Save the function-lookup primitive for later use at attach. */
    g_ompt_lookup = lookup;
    g_ompt_set_callback =
        (ompt_set_callback_t) lookup("ompt_set_callback");
    atomic_store(&g_ompt_ready, 1);
    /* Install NO callbacks — zero OMPT overhead at runtime. */
    return 1;  /* 1 = success in the OMPT spec */
}

static void stub_ompt_finalize(ompt_data_t* tool_data) { }

__attribute__((visibility("default")))
ompt_start_tool_result_t*
ompt_start_tool(unsigned int omp_version, const char* runtime_version)
{
    static ompt_start_tool_result_t r = {
        .initialize = stub_ompt_initialize,
        .finalize   = stub_ompt_finalize,
        .tool_data  = { .value = 0 },
    };
    return &r;
}
```

The important properties:

- **Zero OMPT overhead before attach.** `stub_ompt_initialize` registers no callbacks, so OpenMP's callback sites stay at their default noop paths. No dispatch-table equivalent exists for OMPT, so there is no wrapper installed anywhere.
- **Attach still works.** When the controller sends `CMD_CONFIGURE` with `--ompt` enabled, the stub's background thread `dlopen`s the tool library (as usual for HIP/HSA — the SDK arrives via that library's link graph). The tool's `tool_initialize` calls back into the stub via the `rocp_stub_get_state()` accessor to retrieve `g_ompt_set_callback` and then installs the real OMPT callbacks on the live OpenMP runtime. This is exactly what rocprofiler-sdk's `set_ompt_callbacks()` already does (`ompt.cpp:64-78`), except it is driven from the controller instead of unconditionally at SDK init.
- **Reconfigure and deactivate work.** Because `g_ompt_set_callback` is persistent, the controller can later set callbacks to null pointers (OMPT supports deregistration via `ompt_set_callback(kind, NULL)`) to silence OMPT without unloading the SDK.

**What OMPT cannot do that the HIP/HSA dispatch path can:** there is no analog of rocprofiler-sdk's per-operation `update_table()` filtering for OMPT — callbacks are set per event kind, not per operation within an API. Fine-grained "trace only some OMPT events" selection is implemented as a tool-side filter in the callback body, not by skipping `ompt_set_callback`.

**Symbol-export implication:** the stub `.so`'s symbol-export contract is slightly richer than the HIP-only case. `rocprofiler_configure` is still deliberately absent, but `ompt_start_tool` is present (silent at init, activated at attach). The built artifact can be verified with `nm -D librocp_stub_<channel>.so | grep -E 'rocprofiler_configure|ompt_start_tool'`.

> **Implementation status.** As of this writing, the mock stubs do **not** yet export `ompt_start_tool` — the OMPT handling described above is the *design contract* that the next mock iteration will add. The current mock sample application exercises only the HIP-style dispatch-table path, so the benchmark numbers in [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) reflect the HIP path and not OMPT. `nm -D build/lib/librocp_stub_<channel>.so` on the current build shows neither `rocprofiler_configure` nor `ompt_start_tool`, which is expected for the HIP-only mock. The OMPT export is localized to the stub's `ompt_start_tool` function plus an extension to the stub↔tool accessor, and is tracked as follow-up work.

### What This Reuses From rocprofiler-sdk (No Changes)

- The entire `copy_table`/`update_table`/`functor` machinery
- `context`, `callback_tracing_service`, `buffer_tracing_service`
- `populate_contexts`, `context_filter`
- `rocprofiler_force_configure()` — public API, works because SDK init_status is 0 when called
- `rocprofiler_register_invoke_all_registrations()` — internally triggered by force_configure
- `rocprofiler_create_context`, `rocprofiler_start_context`, `rocprofiler_stop_context`
- `rocprofiler_configure_callback_tracing_service`, `rocprofiler_configure_buffer_tracing_service`

### What Needs To Be Added

- A small **stub library** (preloaded via `LD_PRELOAD`) that:
  - Does NOT export `rocprofiler_configure` (so rocprofiler-register doesn't auto-load SDK)
  - Sets up the control channel IPC (mmap/socket/memfd)
  - Background thread that on `CMD_CONFIGURE` does `dlopen` + `rocprofiler_force_configure`
- A **tool library** (`librocprofiler-sdk-tool.so`) that:
  - Exports `rocprofiler_configure` returning the real tool callbacks
  - Reads the controller's config from the stub's static stash
  - Creates the context and registers services accordingly

**No changes to rocprofiler-sdk source code.** All public APIs already exist; we just need to time the dlopen correctly.

## Canonical Control Protocol

All four IPC options share the same protocol semantics. Define once, reference from each option doc.

### Commands (controller → tool)

```c
enum rocp_ctrl_command {
    CMD_NONE        = 0,   // No pending command
    CMD_CONFIGURE   = 1,   // First attach: dlopen tool library (brings rocprofiler-sdk via link dep) + force_configure with config
    CMD_ACTIVATE    = 2,   // rocprofiler_start_context() (after configure)
    CMD_DEACTIVATE  = 3,   // rocprofiler_stop_context()
    CMD_RECONFIGURE = 4,   // Update tool-side runtime filter (no SDK calls)
    CMD_STATUS      = 5,   // Query current state (where bidirectional channel exists)
};
```

### Configuration payload (`rocp_config_t`)

```c
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
```

### Control struct (`rocp_ctrl_t`)

```c
#define ROCP_CTRL_MAGIC   0xD15EA7C0
#define ROCP_CTRL_VERSION 1

typedef struct {
    uint32_t magic;
    uint32_t struct_version;

    _Atomic uint32_t command;        // rocp_ctrl_command
    _Atomic uint32_t version;        // bumped on every command

    rocp_config_t config;            // controller writes, tool reads on CMD_*

    _Atomic uint32_t context_active; // tool writes status
    _Atomic uint64_t context_id;     // rocprofiler_context_id_t.handle (uint64_t)
    _Atomic uint64_t events_traced;
    _Atomic uint64_t events_dropped;

    uint32_t pid;
    uint64_t start_time;             // /proc/<pid>/stat field 22 — for stale-pid detection
} __attribute__((aligned(64))) rocp_ctrl_t;
```

### Stub↔Tool state-sharing contract

Instead of `extern` cross-DSO globals (fragile), the stub exports a single accessor function:

```c
typedef struct {
    rocp_ctrl_t* ctrl;
    rocp_config_t* pending_config;
    rocprofiler_context_id_t* saved_ctx;
} rocp_stub_state_t;

const rocp_stub_state_t* rocp_stub_get_state(void);  // exported by stub
```

The tool's `rocprofiler_configure` calls `rocp_stub_get_state()` (resolved via `dlsym(RTLD_DEFAULT, ...)` after the stub was preloaded with effective `RTLD_GLOBAL` visibility) to obtain pointers to the shared state.

### What this design reuses from rocprofiler-sdk (unchanged)

- `copy_table` / `update_table` / `functor` template machinery
- `context`, `callback_tracing_service`, `buffer_tracing_service`
- `populate_contexts`, `context_filter`
- `rocprofiler_force_configure()` — public API, succeeds when SDK init_status == 0 (i.e., before SDK has loaded)
- `rocprofiler_register_invoke_all_registrations()` — internally triggered by force_configure to replay runtime API tables
- `rocprofiler_create_context`, `rocprofiler_start_context`, `rocprofiler_stop_context`
- `rocprofiler_configure_callback_tracing_service`, `rocprofiler_configure_buffer_tracing_service`

### What this design adds

Per option (all four implement the same pattern, differing only in IPC):
- A **stub library** preloaded via `LD_PRELOAD` (no `rocprofiler_configure` symbol → register-lib doesn't load SDK)
- A **tool library** dlopen'd by the stub at first attach (exports `rocprofiler_configure` returning real callbacks)
- A **controller binary** that writes config + commands to the IPC channel

**No new SDK APIs are needed.** All public APIs already exist.

The key design question remains: **what IPC mechanism does the external controller use to communicate with the tool's background thread?**

## Requirements

The control channel must satisfy:

1. **No root/sudo** — The user should not need elevated privileges to trace their own process
2. **No capabilities** — No `CAP_BPF`, `CAP_SYS_PTRACE`, `CAP_PERFMON`, etc.
3. **No specific kernel version** — Must work on any modern Linux kernel
4. **Cross-user security** — Other users on the system must not be able to interfere with, read, or modify another user's profiling session
5. **Minimal hot-path overhead** — The "is tracing enabled?" check runs on every intercepted API call and must be as close to zero-cost as possible
6. **Configuration richness** — Per-function enable/disable, filter patterns, output format, and buffer sizes are handled by existing `rocprofiler_configure_*` APIs at init time. The control channel must at minimum support activate/deactivate commands, and optionally carry additional runtime configuration
7. **Multi-runtime applicability** — Must scale to tracing HIP, HSA/ROCR, RCCL, OpenMP, rocdecode, rocjpeg, and other GPU API libraries simultaneously

## Approaches Evaluated

We evaluated 13 control channel mechanisms across 6 categories.

### Category 1: Shared Memory

The "Hot-Path Cost" column below shows the **raw IPC mechanism cost** (atomic load from mmap'd memory). In the rocprofiler-sdk integration, the actual hot-path overhead is ~10-20 ns from `populate_contexts()` — the IPC mechanism adds zero to the hot path since it only toggles context activation from a background thread.

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|----------------|----------|----------|
| A | POSIX shared memory (`shm_open` + `mmap`) | ~1-5 ns | Nothing | File mode `0600` on `/dev/shm/` |
| B | mmap on regular file | ~1-5 ns | Nothing | Dir `0700` + file `0600` |

**How they work:** The tool library creates a shared memory region during its `initialize` callback. The controller opens the same region and writes commands (activate/deactivate context). The tool's background thread reads the command and calls `rocprofiler_start_context()` / `rocprofiler_stop_context()`. No IPC on the hot path — the hot path is entirely within rocprofiler-sdk's existing `populate_contexts()` check.

### Category 2: BPF-Based

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| C | BPF array map + `BPF_F_MMAPABLE` | ~1-5 ns | `CAP_BPF`, kernel 5.5+ | Capability-gated |
| O | BPF map (syscall access, no mmap) | ~150-300 ns | `CAP_BPF` | Capability-gated |
| P | BPF ring buffer | ~10-50 ns | `CAP_BPF`, kernel 6.1+ for USER_RINGBUF | Capability-gated |
| Q | BPF token + mmapable map | ~1-5 ns | Kernel 6.9+ | Token-delegated |

**How they work:** BPF maps are kernel-managed shared data structures accessible from both userspace and BPF programs. A mmapable BPF array map (`BPF_F_MMAPABLE`) can be mmap'd by both the controller and the tool library, giving ~1-5 ns atomic load performance. The unique value is that the same map is simultaneously readable by kernel-side BPF programs, enabling hybrid kernel+userspace tracing controlled by a single flag. BPF ring buffers provide high-throughput streaming (good for trace data) but are unidirectional and message-based (poor for shared state).

### Category 3: Socket-Based

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| F | Unix domain socket | ~1-5 ns (local atomic) | Nothing | `SO_PEERCRED` (kernel-verified effective UID/PID) |
| memfd | Unix socket + `memfd_create` | ~1-5 ns (mmap'd memfd) | Nothing | `SO_PEERCRED` + no filesystem entry |

**How they work:** The tool library creates a listener socket (abstract namespace) and spawns a background thread during its `initialize` callback. The controller connects, is authenticated via `SO_PEERCRED` (kernel-verified effective UID/GID at connect time, unforgeable), and sends commands. The background thread calls `rocprofiler_start_context()` / `rocprofiler_stop_context()`. No socket I/O on the hot path — the hot path is entirely within rocprofiler-sdk's existing `populate_contexts()`. The memfd variant adds `memfd_create` for anonymous shared memory passed via `SCM_RIGHTS`, enabling the controller to write commands directly to mmap'd memory.

### Category 4: Direct Memory Access

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| L | `/proc/<pid>/mem` direct write | ~1-5 ns (target side) | ptrace permissions | `ptrace_scope` + UID |
| M | `process_vm_writev` | ~1-5 ns (target side) | ptrace permissions | `ptrace_scope` + UID |

**How they work:** The tool library exports known symbols in its `.data` section. The controller resolves their addresses by parsing `/proc/<pid>/maps` + ELF symbol offsets, then writes directly to the target's memory via `/proc/<pid>/mem` (pwrite) or `process_vm_writev` (scatter-gather syscall). The target process is completely passive — no background thread needed. This is conceptually how GDB modifies variables in a running process.

### Category 5: Signal-Based

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| J | SIGUSR1/SIGUSR2 | ~1-5 ns (`sig_atomic_t`) | Nothing | `kill()` UID check |
| K | Real-time signal (SIGRTMIN+n) | ~1-5 ns | Nothing | `kill()` UID check |

**How they work:** The tool library registers a signal handler during its `initialize` callback. The controller sends `kill(target_pid, SIGUSR1)`. The kernel enforces that only processes with the same UID can send signals. Carries zero configuration data (SIGUSR1/2) or one `int` via `sigqueue()` (real-time signals). Useful as an instant wakeup trigger paired with a data channel (B or memfd) for the actual command.

### Category 6: Code Injection

| ID | Mechanism | IPC Check Cost | Requires | Security |
|----|-----------|---------------|----------|----------|
| N | ptrace inject (rocprofiler-sdk approach) | ~50-200 ns | ptrace permissions, x86-64 | `ptrace_scope` + UID |

**How it works:** This is what rocprofiler-sdk uses today for `rocprofv3 --attach`. The controller uses `PTRACE_SEIZE` to attach to a pre-created safe thread in the target, then injects x86-64 assembly (`0f 05` syscall + `cc` INT3) to allocate memory, write environment data, and call `rocprofiler_register_attach()` — which triggers full profiler initialization including dlopen of tool libraries and intercept table updates. The target must have opted in by running with `ROCP_TOOL_ATTACH=1` (which creates the safe background thread). This is the most powerful approach (can execute arbitrary code in the target) but also the most complex (~1500 lines of ptrace/assembly code).

## Elimination Criteria

We applied two hard filters:

### Filter 1: No sudo / No capabilities / No specific kernel version

| Eliminated | Reason |
|------------|--------|
| **C** (BPF map + mmap) | Requires `CAP_BPF` on both controller and tool library |
| **O** (BPF map syscall) | Requires `CAP_BPF`; also ~150-300 ns per-call syscall overhead is unacceptable |
| **P** (BPF ring buffer) | Requires `CAP_BPF`; kernel 6.1+ for `USER_RINGBUF`; wrong primitive for control state |
| **Q** (BPF token) | Requires kernel 6.9+ (not available on RHEL 8/9, Ubuntu 22.04) |
| **L** (`/proc/pid/mem`) | Requires ptrace permissions; on Ubuntu default (`ptrace_scope=1`) only parent processes can access |
| **M** (`process_vm_writev`) | Same ptrace permission requirements as L |
| **N** (ptrace inject) | Requires ptrace permissions; x86-64 only; target must opt in with background thread |

### Filter 2: Cross-user security (kernel-enforced)

| Eliminated | Reason |
|------------|--------|
| **A** (POSIX shm) | Predictable name in flat `/dev/shm/` namespace; vulnerable to race condition where a malicious user pre-creates the segment with their ownership before the target starts |
| **J/K** (Signals) | Secure (kernel UID check) but carry zero configuration data — cannot function as a standalone control channel |

### What Survived

| Channel | Hot-Path | Auth Model | Config | Filesystem Footprint |
|---------|----------|------------|--------|---------------------|
| **mmap** (regular file) | **0 ns** | Directory `0700` + file `0600` | Command + status | `/run/user/<uid>/...` |
| **socket** (Unix domain) | **0 ns** | `SO_PEERCRED` (kernel-verified) | Full protocol | None (abstract namespace) |
| **memfd** (socket + anonymous shm) | **0 ns** | `SO_PEERCRED` | Command struct + protocol | None whatsoever |
| **signal** (trigger + mmap/memfd data channel) | **0 ns** | `kill()` UID check + paired channel | Via paired channel | Depends on pair |

**Before any controller attaches**, hot-path overhead is 0 ns because the stub library doesn't export `rocprofiler_configure`, so rocprofiler-register never loads rocprofiler-sdk — no wrappers are installed. **After detach** (controller previously attached and then deactivated), wrappers remain installed and `populate_contexts()` returns empty at ~10-20 ns per call (Level 2 noop).

## Detailed Comparison of Surviving Options

### Security Comparison

| Threat | mmap (file) | socket (Unix socket) | memfd | signal |
|--------|---------------|-----------------|---------|---------------|
| Other user reads config | Blocked (0700 dir) | Blocked (SO_PEERCRED) | Blocked (SO_PEERCRED) | Blocked (kill UID) |
| Other user modifies config | Blocked (0700 dir) | Blocked (SO_PEERCRED) | Blocked (SO_PEERCRED) | Blocked (kill UID) |
| Race / pre-creation attack | Protected (user-owned dir) | Not applicable (abstract ns) | Not applicable (no FS) | Depends on pair |
| Identity forging | No identity check | UID/GID unforgeable (kernel-verified at connect time) | UID/GID unforgeable | Unforgeable (kill UID check) |
| Stale artifacts on crash | Stale file in tmpfs | None (abstract socket) | None (anonymous) | Depends on pair |

### Overhead Comparison

| Phase | mmap | sock | memfd | signal |
|-------|------|------|-------|--------|
| Init (target side) | ~10 μs (open+mmap) | ~10 μs (socket+bind+listen+thread) | ~15 μs (socket+thread) | ~10 μs (sigaction+paired init) |
| Attach (controller) | ~5 μs (open+mmap) | ~5 μs (connect+SO_PEERCRED) | ~10 μs (connect+memfd+SCM_RIGHTS) | ~5 μs (paired attach) |
| Hot-path (no attach) | 0 ns | 0 ns | 0 ns | 0 ns |
| Hot-path (after detach, wrappers stay) | ~10-20 ns | ~10-20 ns | ~10-20 ns | ~10-20 ns |
| Context toggle | ~1 ms (poll interval) | ~5 μs (socket recv) | ~50-100 ns (mmap write) + ~1 ms (poll) | ~1-5 μs (signal delivery) |

### Architecture Fit for rocprofiler-sdk

| Aspect | mmap | sock | memfd | signal |
|--------|------|------|-------|--------|
| Per-function enable | Existing `rocprofiler_configure_*` at init | Same | Same | Same |
| Multi-runtime config | Single ctrl file per process | Single socket per process | Single socket + memfd | Single signal + paired channel |
| Bidirectional (status queries) | Status fields in mmap | Yes (native) | Yes (socket) + fast status (memfd) | Limited |
| Graceful detach | CMD_DEACTIVATE in mmap | Send CMD_DEACTIVATE, get ACK | Send CMD_DEACTIVATE, get ACK | Signal + paired |
| Changes to rocprofiler-sdk | Minimal (tool lib + bg thread) | Same + socket protocol | Same + memfd + SCM_RIGHTS | Same + signal handler |

## Special Considerations

### OpenMP OMPT: Always-Enabled Shim with Noop Control

The OpenMP 5.0/5.1/5.2 spec defines OMPT tool initialization as a **one-shot event** during runtime startup. The runtime calls `ompt_start_tool()` exactly once during the first OpenMP construct or API call. If no tool is found at that point, OMPT is disabled for the lifetime of the process. There is no re-initialization or late-attachment path.

- `OMP_TOOL_LIBRARIES` must be set **before process launch**. The runtime reads it during initialization only.
- `OMP_TOOL=enabled|disabled` is similarly read once at init time.
- OpenMP 6.0 does not change this — OMPT remains init-time-only.

**The solution: start OMPT enabled, let rocprofiler-sdk's context activation control noop behavior.** The OMPT tool is always registered from process start via `OMP_TOOL_LIBRARIES`. The OMPT callbacks feed into rocprofiler-sdk's existing tracing infrastructure — they register a context with `ROCPROFILER_CALLBACK_TRACING_OMPT` domain. When the context is inactive (controller hasn't attached), `populate_contexts()` finds nothing and the callbacks are effectively noop. When the controller activates the context, callbacks start recording.

This approach means:
1. **OMPT is registered at process start** via `ompt_start_tool()` / `OMP_TOOL_LIBRARIES`
2. **OMPT callbacks register a rocprofiler context** with OMPT tracing domain during `tool_initialize`
3. **Context starts inactive** — `populate_contexts()` returns empty, wrappers noop (~10-20 ns)
4. **When the controller attaches**, the control channel calls `rocprofiler_start_context()` — OMPT callbacks start recording
5. **When the controller detaches**, `rocprofiler_stop_context()` — callbacks return to noop

This is fully consistent with the noop-by-default dispatch tracer architecture. The OMPT shim is just another "runtime" alongside HIP, HSA, RCCL, etc., controlled by the same atomic flag and same control channel.

### OpenMP Tool Plugin Interface

Anthony raised the concern about tracing OpenMP when the runtime is outside the team's direct control. The OMPT spec itself defines a **plugin-based tool interface** that fits naturally into the dispatch tracer:

| Mechanism | How it works | Who controls it |
|-----------|-------------|-----------------|
| `ompt_start_tool()` | The OpenMP runtime calls this to discover tools. The tool returns callback registrations. | OpenMP spec — standard interface |
| `OMP_TOOL_LIBRARIES` | Environment variable listing shared libraries that provide `ompt_start_tool()`. | User sets before launch |
| `ompt_set_callback()` | Tool registers callbacks for specific OMPT events (parallel begin/end, task create, sync, etc.). | Tool library at init time |

The dispatch tracer provides its OMPT tool as a shared library (e.g., `librocprofiler-sdk-ompt-tool.so`) that:
1. Exports `ompt_start_tool()` — discovered via `OMP_TOOL_LIBRARIES`
2. Registers all OMPT callbacks via `ompt_set_callback()`
3. Each callback feeds into rocprofiler-sdk's context system — `populate_contexts()` determines whether the context is active
4. Events flow through the existing `callback_tracing_service` / `buffer_tracing_service` infrastructure

For comparison, here is how other profiling tools handle OpenMP:

| Tool | OpenMP approach |
|------|----------------|
| **rocprofiler-sdk** | Provides an OMPT tool library; registers at init via `OMP_TOOL_LIBRARIES` |
| **Intel VTune** | Uses ITT + OMPT integration; OMPT tool built into VTune collector |
| **NVIDIA Nsight** | OMPT support via `libnvtx_ompt.so` loaded via `OMP_TOOL_LIBRARIES` |

All of them follow the same pattern: ship an OMPT tool library, register at init, control behavior at runtime. The dispatch tracer's noop-shim approach is identical.

### Why OpenTelemetry is not a viable control channel

OpenTelemetry was raised in review as a candidate control channel — i.e. the mechanism through which the external controller sends `CMD_CONFIGURE` / `CMD_ACTIVATE` / etc. to the target process — alongside mmap, Unix socket, memfd, and signal. It does not fit that role, for the following reasons:

- **Wrong direction.** OTel is a one-way telemetry *export* protocol: data flows from the instrumented process outward to a collector (OTLP/gRPC or OTLP/HTTP). There is no OTel primitive for the *collector* to push commands back into a running process, which is exactly what a control channel needs. The SDK has no inbound-command surface.
- **No in-process listener.** Our requirement is that the target process bind a local rendezvous (abstract socket, mmap file, memfd, signal handler) that an external controller connects to after the process has already started. OTel SDKs do the opposite — they open outbound connections to a configured collector endpoint. We would have to invent a collector-to-agent control path that the spec does not define.
- **No peer authentication primitive.** Every surviving channel in this survey uses a kernel-verified same-UID check (`SO_PEERCRED`, directory mode `0700`, `siginfo->si_uid`). OTel runs over HTTP/gRPC with TLS or no transport security — there is no equivalent of "this peer is the same local user" built into the protocol. Meeting the cross-UID security requirement would mean bolting an orthogonal auth layer on top.
- **Heavy dependencies for a same-host primitive.** The other four channels are single syscall families (`socket`/`mmap`/`memfd_create`/`sigaction`) with zero runtime dependencies. OTel pulls in a large C++ SDK, protobuf, gRPC or curl, and a collector configuration — orders of magnitude more surface for a local IPC mechanism.
- **Export-format question sits elsewhere.** OTel *can* be useful as an export format for trace records *after* the tool is running — i.e. converting rocprofiler-sdk callback events into OTLP spans. That is a tool-library output-format decision that lives downstream of the control channel and is orthogonal to this survey; conflating it with the control-channel question is what caused the confusion in the first place.

OTel is therefore eliminated from control-channel consideration on the same grounds as the nine options in § Elimination Criteria: it does not meet the requirements. The four surviving channels (mmap, socket, memfd, signal) are the ones that do.

### Comparison with ptrace attach

The late-load design provides full late configuration without ptrace. Here is the comparison:

| Capability | Late-load control channel | ptrace attach |
|---|---|---|
| What's loaded at process start | Stub library only (no SDK) | Nothing (tool injected at attach) |
| Hot-path overhead before any attach | **0 ns** (no wrappers — SDK not loaded) | 0 ns (tool not loaded) |
| Configure which APIs to trace at attach | Yes (passed via control channel) | Yes |
| Configure output format / buffers at attach | Yes | Yes |
| Add domain not in first-attach config | No (subsequent force_configure locked) | Yes (re-injects fresh SDK) |
| Privileges required | None | `CAP_SYS_PTRACE` (or appropriate ptrace_scope) |
| Architecture | Any | x86-64 only |
| Attach latency | ~1-2 ms (mock; real SDK dlopen would make this ~5-50 ms) (dlopen + force_configure + propagation) | ~10-50 ms (ptrace + injection) |

### Use Case Matrix

| Use case | Recommended mechanism |
|---|---|
| App launched with stub preload, attach later with full config | **Late-load control channel** |
| Need full late configuration without ptrace | **Late-load control channel** |
| App not launched with stub preload (no preparation done) | ptrace attach (only option) |
| Need to add domains AFTER first attach | ptrace attach (control channel cannot — force_configure locked after first call) |
| Container without `CAP_SYS_PTRACE` | **Late-load control channel** |
| ARM64 / non-x86_64 platforms | **Late-load control channel** |

### Cross-Platform Considerations

All four surviving control channel options (mmap, sock, memfd, signal) are **Linux-specific**:

- Abstract Unix sockets (Options F, memfd) are Linux-only (not macOS/BSD)
- `memfd_create` (memfd) is Linux-only
- `/run/user/<uid>/` (mmap) depends on systemd
- Real-time signals with `sigqueue` (signal) are POSIX but behavior varies

For future cross-platform support, mmap could fall back to a platform-appropriate temp directory, and socket could fall back to filesystem-namespace Unix sockets. The dispatch table mechanism itself (atomic load + function pointer check) is portable to any platform with C11 atomics.

## Recommendation

The four surviving options each have distinct strengths:

- **B** is the simplest — minimal code, one background thread, no sockets. Best for straightforward enable/disable.
- **F** has the strongest authentication — kernel-verified identity on every connection. Best for security-sensitive deployments.
- **memfd** combines F's authentication with mmap-speed config access and zero filesystem footprint. Best overall for production.
- **signal** adds instant notification without polling. Useful as an enhancement to mmap or memfd.

Each option is designed in detail in its own document:

- [mmap Design: mmap Regular File](MMAP.md)
- [socket Design: Unix Domain Socket](SOCKET.md)
- [memfd Design: Socket + Anonymous Shared Memory](MEMFD_SOCK.md)
- [signal Design: Signal-Triggered Data Channel](SIGNAL.md)

## References

### rocprofiler-sdk Source Code

- [rocprofiler-sdk](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-sdk) — AMD's profiling SDK with intercept table architecture
- `source/lib/rocprofiler-sdk/intercept_table.cpp` — Intercept table registration and notification
- `source/lib/rocprofiler-sdk/hip/hip.cpp` — HIP API dispatch table interception (`copy_table`, `update_table`, `functor`)
- `source/lib/rocprofiler-sdk/hsa/hsa.cpp` — HSA API dispatch table interception
- `source/lib/rocprofiler-sdk/tracing/tracing.hpp` — `populate_contexts`, `context_filter`, callback/buffer dispatch
- `source/lib/rocprofiler-sdk/registration.cpp` — Tool registration, `rocprofiler_configure`, `atexit` finalization
- `source/lib/rocprofiler-sdk-rocattach/rocattach.cpp` — Existing ptrace-based attach mechanism

### rocprofiler-register

- [rocprofiler-register](https://github.com/ROCm/rocm-systems/tree/develop/projects/rocprofiler-register) — Library registration coordinator
- `source/include/rocprofiler-register/rocprofiler-register.h` — Public API: `rocprofiler_register_library_api_table()`

### Linux IPC Mechanisms

- [shm_open(3)](https://man7.org/linux/man-pages/man3/shm_open.3.html) — POSIX shared memory
- [unix(7)](https://man7.org/linux/man-pages/man7/unix.7.html) — Unix domain sockets, `SO_PEERCRED`, abstract namespace
- [memfd_create(2)](https://man7.org/linux/man-pages/man2/memfd_create.2.html) — Anonymous memory file descriptors
- [cmsg(3)](https://man7.org/linux/man-pages/man3/cmsg.3.html) — `SCM_RIGHTS` file descriptor passing
- [sigqueue(3)](https://man7.org/linux/man-pages/man3/sigqueue.3.html) — Real-time signals with data
- [bpf(2)](https://man7.org/linux/man-pages/man2/bpf.2.html) — BPF maps (evaluated and eliminated)
- [ptrace(2)](https://man7.org/linux/man-pages/man2/ptrace.2.html) — Process trace (evaluated and eliminated)
- [process_vm_writev(2)](https://man7.org/linux/man-pages/man2/process_vm_writev.2.html) — Cross-process memory write (evaluated and eliminated)

### OpenMP OMPT

- [OpenMP 5.2 Specification](https://www.openmp.org/spec-html/5.2/openmpch19.html) — OMPT tool interface
- `ompt_start_tool()` — One-shot init-time tool discovery (no late attachment)

### Security

- [Yama LSM](https://www.kernel.org/doc/html/latest/admin-guide/LSM/Yama.html) — `ptrace_scope` (reason for eliminating Options L, M, N)
- [BPF Token](https://docs.ebpf.io/linux/concepts/token/) — Capability delegation (evaluated, eliminated due to kernel 6.9+ requirement)
- [Pinning](https://docs.ebpf.io/linux/concepts/pinning/) — BPF filesystem pinning
