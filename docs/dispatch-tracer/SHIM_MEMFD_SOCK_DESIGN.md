# Shim Architecture with memfd+sock Channel — End-to-End Design

## 0. Scope

This document is the detailed end-to-end design for the **shim architecture** (see [SHIM_COMPARISON.md](SHIM_COMPARISON.md) for the rationale for choosing this over the late-load stub approach) wired to the **memfd + sock hybrid** control channel (see [MEMFD_SOCK.md](MEMFD_SOCK.md) for the rationale for choosing this hybrid over mmap / signal / pure-sock).

It covers:

1. What libraries are in the process, when, and by whom they are loaded
2. The shim ↔ SDK ABI contract (functor layering)
3. The shared-state layout in the memfd (profiler-functor slot table, ring buffer, header)
4. The socket bootstrap protocol (auth, fd handoff, watermark wake)
5. Lifecycle: process start → first attach → tracing active → reconfigure → detach → shutdown → crash recovery
6. How the shim coexists with an in-process rocprofiler-sdk tool
7. How OMPT integrates (its one-shot `ompt_start_tool` scan is not a dispatch table)
8. The external-process OOP profiler API the shim publishes
9. Security model and threat analysis
10. Open questions left to implement

Everything below assumes no sudo, no capabilities (CAP_BPF / CAP_SYS_PTRACE / etc.), no specific kernel version beyond Linux 3.17 (for `memfd_create` + `F_SEAL_*`), no binary rewriting in the default path, and works across architectures.

## 1. Libraries in the process

| Library | How it enters the process | When | Purpose |
|---|---|---|---|
| `libamdhip64.so` / `libhsa-runtime64.so` / `libomptarget.so` / `librccl.so` | Linked by application or `dlopen`'d by user code | At normal runtime load | Publishes mutable dispatch tables via `rocprofiler_register_library_api_table` |
| `librocprofiler-register.so.0` | `DT_NEEDED` of every runtime above | Implicitly, when the runtime loads | Stores the tables, scans for tools, decides who to hand them to |
| **`librocprofiler-sdk-shim.so`** | **`dlopen`'d unconditionally by rocprofiler-register** when any runtime calls `register_library_api_table` | Implicitly, at first runtime registration | Wraps every table entry with `shim_wrap_<Op>`; owns the memfd+sock control channel; publishes the OOP profiler API |
| `librocprofiler-sdk.so` | `dlopen`'d by rocprofiler-register only if an in-process SDK tool is present (user has `ROCP_TOOL_LIBRARIES=...` or a linked tool that exports `rocprofiler_configure`) | When register's scan succeeds | Does in-process profiling as today — `update_table()` wraps the shim's `functor` (not the raw runtime function) |
| `libX-oop-tool-consumer.so` (external process, not in-target) | User's OOP profiler binary linked against `librocprofiler-shim-consumer.so` | Separate process | Connects to the target's abstract socket, receives the memfd, consumes records |

The user's process has **only the shim plus register** in its address space by default. The SDK enters only when an in-process SDK tool is requested. The OOP consumer lives in its own process entirely.

## 2. High-level component diagram

```
┌────────────────────────────────────────────────────────────────────────────┐
│                         Target process                                     │
│                                                                            │
│  libamdhip64 / libhsa-runtime64 / libomptarget / librccl                  │
│           │         (DT_NEEDED)                                            │
│           ▼                                                                │
│  ┌───────────────────────────┐                                             │
│  │ librocprofiler-register   │                                             │
│  │ (scans + stores tables,   │                                             │
│  │  always dlopen's shim)    │                                             │
│  └──────────┬────────────────┘                                             │
│             │ (dlopen + handoff)                                           │
│             ▼                                                              │
│  ┌──────────────────────────────────────────────────────────────────┐     │
│  │ librocprofiler-sdk-shim.so                                       │     │
│  │                                                                  │     │
│  │  hot path:    functor(args)                                      │     │
│  │                 ├─ atomic_load(profiler_functor[Op])             │     │
│  │                 ├─ branch                                        │     │
│  │                 └─ tail-call orig(args)     (fast path)          │     │
│  │                   or profiler_functor(orig, args)  (attached)    │     │
│  │                                                                  │     │
│  │  bg thread:   accept abstract sock → SO_PEERCRED                 │     │
│  │               → SCM_RIGHTS memfd to consumer                     │     │
│  │               → poll ring watermark → eventfd_wake               │     │
│  │                                                                  │     │
│  │  shared with consumer (mmap'd memfd):                            │     │
│  │     [header | profiler_functor[N_OPS] | eventfd | ring_buffer ]  │     │
│  └──────────┬────────────────────┬───────────────────────────────────┘     │
│             │                    │                                         │
│             │                    │ (optional, only if in-process tool      │
│             │                    │  exports rocprofiler_configure)         │
│             ▼                    ▼                                         │
│  ┌────────────────────┐  ┌──────────────────────────────┐                 │
│  │ OOP callbacks in   │  │ librocprofiler-sdk.so        │                 │
│  │ the shim: write    │  │ (in-process profiling today) │                 │
│  │ begin/end records  │  │ update_table() wraps the     │                 │
│  │ to ring buffer     │  │ shim's functor (layered)     │                 │
│  └────────────────────┘  └──────────────────────────────┘                 │
│                                                                            │
└────────────────────────────────────────────────────────────────────────────┘
                  ▲                          ▲
                  │ abstract socket          │ mmap'd memfd
                  │ "\0rocprof-shim_<pid>"  │ (shared memory)
                  │                          │
┌─────────────────┴──────────────────────────┴──────────────────────────────┐
│                       OOP consumer process                                │
│                                                                           │
│   ┌─────────────────────────────────────────────────────────────────┐    │
│   │ librocprofiler-shim-consumer.so  (linked by OOP tool binary)    │    │
│   │                                                                 │    │
│   │  - connect → SO_PEERCRED → recv SCM_RIGHTS memfd                │    │
│   │  - mmap memfd, verify header magic + version                    │    │
│   │  - read ring buffer (poll eventfd for watermark wake)           │    │
│   │  - write profiler_functor[Op] slots to enable/disable tracing   │    │
│   │  - hand records to user's consumer callback                     │    │
│   └─────────────────────────────────────────────────────────────────┘    │
└───────────────────────────────────────────────────────────────────────────┘
```

## 3. Process startup sequence

```
T+0  exec → ld.so starts linking
T+1  libamdhip64 (DT_NEEDED: librocprofiler-register.so) loads
     → register's constructor runs
T+2  register constructor:
       - clear internal state
       - dlopen("librocprofiler-sdk-shim.so", RTLD_NOW | RTLD_GLOBAL)
         ↑ UNCONDITIONAL — no symbol scan needed for the shim itself
       - shim's constructor runs (see T+3)
       - register-side hook fn pointer is now non-null; register is ready
T+3  shim constructor:
       - memfd_create("rocp-shim-ctrl", MFD_CLOEXEC | MFD_ALLOW_SEALING)
       - ftruncate(memfd, SHIM_CTRL_SIZE)
       - mmap(memfd, PROT_READ|PROT_WRITE, MAP_SHARED) → g_ctrl_region
       - initialize header (magic, version, pid, start_time, N_OPS)
       - zero-init profiler_functor[N_OPS] slot table
       - eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) → stored in header
       - F_ADD_SEALS: F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL
         (size frozen, seal-set itself frozen; writes still allowed)
       - socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC) → abstract namespace
         bind "\0rocprof-shim_<pid>"
         listen(backlog=1)   ← single-controller model (see §14)
       - publish set_api_table callback (exported as `mock_sdk_set_api_table` in the mock; production shim would use `rocprofiler_shim_set_api_table`) to register
       - pthread_create(g_bg_thread, bg_main)
       - bg thread blocks on accept()
T+4  libamdhip64 calls rocprofiler_register_library_api_table("hip", tbl)
     → register calls shim_set_api_table_fn("hip", tbl, N_hip)
     → shim's shim_set_api_table:
         for each slot i in tbl:
           g_orig[i] = tbl[i]                          (store release)
           tbl[i] = &shim_wrap_hip_op_i                (atomic store release)
         g_installed = 1
     → register also records the table for possible SDK replay
T+5  libhsa-runtime64 same dance for "hsa" domain
T+6  libomptarget's OMPT init (separate from dispatch tables, see §9)
T+7  main() starts; app begins calling HIP / HSA etc.
     Hot path: every call enters shim_wrap_<domain>_op_<i>
       → atomic_load(profiler_functor[i])     (acquire)
       → null? → tail-call g_orig[i]
       → non-null? → prof(g_orig[i], args…)

                             <<<  +0.8 ns / call  >>>
                             (measured; see §14 and SHIM_COMPARISON.md §7a)
```

No external profiler has touched the process yet. The memfd exists but is not shared with anyone. The socket is listening but nothing has connected. The profiler_functor slots are all NULL; every call takes the fast path.

## 4. First attach — consumer process → target

```
Consumer side                                    Target side (shim bg thread)
─────────────                                    ─────────────────────────────
sock = socket(AF_UNIX, SOCK_STREAM)
                                                 (blocked in accept(listen_fd))
connect("\0rocprof-shim_<target_pid>")
                                                 accept returns client fd
                                                 getsockopt(client, SO_PEERCRED)
                                                   → ucred { pid, uid, gid }
                                                 if (uid != geteuid())
                                                   close(client); continue;
                                                 validate pid start_time
                                                   via /proc/<pid>/stat
                                                   (PID-reuse defense)

                                                 build msghdr:
                                                   iov[0] = "SHIM" + version
                                                   cmsg   = SCM_RIGHTS memfd
                                                 sendmsg(client, msg)
recv msghdr:
  verify header magic "SHIM"
  extract memfd from SCM_RIGHTS
close(sock)  ← optional; socket is only needed
              for the handoff and rare queries

mmap(memfd, PROT_READ|PROT_WRITE, MAP_SHARED)
  → consumer_ctrl
verify consumer_ctrl->magic == ROCP_SHIM_MAGIC
verify consumer_ctrl->struct_version compatible
eventfd_fd = consumer_ctrl->eventfd_num
  (note: eventfd is also dup'd on the socket via
   SCM_RIGHTS in a second cmsg — see §5.2)

// Enable tracing for chosen ops:
consumer_ctrl->profiler_functor[OP_hip_launch] =
    (uint64_t)&my_oop_tool_begin_end;
atomic_store_release(consumer_ctrl->gen_counter,
                     gen_counter + 1);
                                                 (target is now emitting
                                                  records into ring buffer)

poll(eventfd_fd, POLLIN, ...)
                                                 hot path writes record,
                                                 if ring crosses watermark:
                                                   eventfd_write(ef, 1)
wake → drain ring into local consumer callback
```

Key properties:

- **No `dlopen` on the attach path.** The shim is already in the process; all attach does is an atomic store into a mmap'd slot. Attach cost is **~submicrosecond** — three to four orders of magnitude faster than the late-load stub's ~1.8 ms.
- **One-shot authenticated handoff.** Only the single `sendmsg(SCM_RIGHTS)` crosses a socket. All subsequent traffic is shared memory + `eventfd` wakes.
- **Pull model for records.** The target never blocks on the consumer; it writes to the ring and increments the head pointer. The consumer reads and increments the tail. If the consumer is slow, the ring fills and `events_dropped` increments — the target never stalls.

## 5. Shared memory layout (the memfd)

The memfd is `SHIM_CTRL_SIZE` bytes. Concretely `SHIM_CTRL_SIZE = 1 << 20` (1 MiB) — most of which is the ring buffer. Layout:

```
offset          size          contents
─────────────────────────────────────────────────────────────────
0x00000000    64 bytes     Header (cache-line aligned)
                            - uint32_t magic        = 'S''H''I''M' = 0x4D494853
                            - uint32_t struct_version
                            - uint32_t pid
                            - uint64_t start_time   (from /proc/self/stat field 22)
                            - uint32_t n_ops
                            - uint32_t eventfd_num  (informational;
                                                     real fd passed via SCM_RIGHTS)
                            - uint32_t watermark_bytes
                            - uint64_t events_traced   (atomic, write by target)
                            - uint64_t events_dropped  (atomic, write by target)
                            - uint32_t gen_counter     (atomic, write by consumer)
                            - uint32_t reserved[5]

0x00000040   N_OPS * 8    profiler_functor[N_OPS]  (uint64_t each)
                            - Slot per (domain, op) pair.
                            - Consumer writes the function pointer (as uint64)
                              that target should call on every intercepted event.
                            - NULL = do not trace this op.
                            - Layout: domain-major, op-minor
                                slot_index = domain_kind_base[D] + op_within_D

0x00000400    32 bytes     Ring buffer header
                            - uint64_t head       (atomic, write by target)
                            - uint64_t tail       (atomic, write by consumer)
                            - uint64_t mask       (capacity-1, read-only both sides)
                            - uint32_t record_size
                            - uint32_t reserved

0x00000420    (rest)       Ring buffer data  (records — see §7A.5 + §7B.1)
                            rocp_shim_record_t {
                                uint64_t tsc;
                                uint32_t kind;          /* rocprofiler_callback_tracing_kind_t */
                                uint32_t op;            /* rocprofiler_*_id_t within kind */
                                uint32_t phase;         /* ENTER / EXIT */
                                uint32_t cpu;
                                uint64_t thread_id;
                                rocprofiler_correlation_id_t correlation_id;
                                        /* { internal, external, ancestor } */
                                uint32_t arg_overflow;  /* 0 = args inline only,
                                                           else bytes in variable ring */
                                uint32_t arg_bytes;     /* valid bytes in args[] */
                                uint8_t  args[RECORD_ARG_BYTES];
                                        /* Typed payload — cast to the SDK's
                                         * rocprofiler_<domain>_api_args_<op>_t. */
                            }

(second memfd, separate mapping)
0x00000000    (full)       Variable-size auxiliary ring — strings, deep structs,
                           kernarg blobs. Each entry is prefixed with
                           { correlation_internal_id, arg_index, kind, length }
                           so consumer joins on correlation_id. See §7B.2.
```

Sealing: after `F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL`:
- Size cannot shrink (SHRINK) — protects the consumer from the target truncating the fd
- Size cannot grow (GROW) — protects the target from the consumer inflating the fd (we want a fixed, known-size region)
- Seal set itself is frozen (SEAL) — a malicious same-UID peer cannot subsequently add `F_SEAL_WRITE` to lock the target out of its own state

Reads and writes to the body proceed with normal acquire/release atomics on `head`, `tail`, `gen_counter`, `events_traced`, `events_dropped`, and the `profiler_functor[Op]` slots. All other fields are const-after-init.

### 5.1 profiler_functor slot write

When the consumer wants to enable tracing for op `Op` in domain `D`:

```c
uint64_t slot_idx = domain_base[D] + Op;
uint64_t* slot = &ctrl->profiler_functor[slot_idx];

/* Typical pattern: consumer installs a callback that writes to the
 * ring. The callback lives in the consumer process's address space;
 * the target calls it via the function-pointer value written here. */
atomic_store_release(slot, (uint64_t)&my_ring_begin_end);

/* Bump gen_counter so the target can detect that the slot table has
 * changed and potentially flush cached pointers (for future revisions
 * of the shim that read-copy-update the slots). */
atomic_fetch_add(&ctrl->gen_counter, 1);
```

To disable tracing:

```c
atomic_store_release(slot, 0);
atomic_fetch_add(&ctrl->gen_counter, 1);
```

Each hot-path call in the target reads `profiler_functor[Op]` with acquire ordering. If null, tail-call original. If non-null, call it with (original, args…) — the consumer's function pointer is responsible for invoking the original if it wants pre/post semantics.

### 5.2 Eventfd for consumer wake

At setup time the shim creates an `eventfd` and passes it alongside the memfd in the same `sendmsg` as a second `SCM_RIGHTS` cmsg. The consumer polls this eventfd. When the target writes a record and crosses the watermark threshold (e.g. ring half-full), it does `eventfd_write(ef, 1)`. Consumer's `poll()` returns, consumer drains records from the ring, consumer acknowledges via `atomic_store(&ctrl->tail, new_tail)`.

No polling loop on the target side, no polling loop on the consumer side. Both block in the kernel when idle.

## 6. Socket bootstrap protocol

The socket is used only for connection setup, authenticated fd handoff, and the rare query that needs a response (e.g. `SHIM_QUERY_STATS`). All per-event traffic goes through the memfd ring.

### 6.1 Connection

- **Address**: abstract namespace, `sun_path[0] = '\0'`, `sun_path[1..]` = `"rocprof-shim_<pid>"`.
- **Type**: `AF_UNIX, SOCK_STREAM`.
- **Options set by target**: `SOCK_CLOEXEC` on the listening fd. `listen(1)` backlog = 1 — only one controller at a time (see §14 for rationale).

### 6.2 Handshake message (server → client, sent on accept)

```
iovec[0]:        struct shim_hello {
                    char     magic[4];       // "SHIM"
                    uint32_t struct_version;
                    uint32_t n_ops;
                    uint32_t watermark_bytes;
                    uint64_t start_time;     // /proc/self/stat field 22
                 };
cmsg[0]:         SCM_RIGHTS, fd = memfd
cmsg[1]:         SCM_RIGHTS, fd = eventfd
```

Consumer verifies `magic == "SHIM"` and `struct_version` is compatible, then mmaps the memfd. The `start_time` in the handshake must match `start_time` in the mmap'd header (PID-reuse defense: if a malicious peer bound the same abstract name on a reused PID, the start_time in procfs will not match).

### 6.3 Query commands (post-bootstrap, both directions)

One query per `send`/`recv` round-trip. Keeps the protocol stateless.

```
enum shim_query_type {
    SHIM_Q_STATS     = 1,   // consumer → server
    SHIM_Q_DETACH    = 2,   // consumer → server
    SHIM_Q_FLUSH     = 3,   // consumer → server
    SHIM_Q_CAPABILITIES = 4 // consumer → server
};

struct shim_query {
    uint32_t type;
    uint32_t reserved;
    uint64_t arg0;
};

struct shim_response {
    uint32_t type;
    uint32_t status;
    uint64_t arg0;
    uint64_t events_traced;
    uint64_t events_dropped;
};
```

Ordinary tracing does not use the socket at all after the handshake. Queries cross the socket only when the consumer genuinely wants a server-side answer — e.g. a final stats snapshot before disconnect.

## 7. Hot-path functor anatomy

For each (domain, op) pair we generate a wrapper. In C with `musttail` it looks like:

```c
/* Generated once per (domain, op). Example: HIP op "hipLaunchKernel" */
static void shim_wrap_hip_launch_kernel(hipStream_t s, /* … */)
{
    uint64_t prof_raw = atomic_load_explicit(
        &g_ctrl->profiler_functor[HIP_BASE + OP_LAUNCH_KERNEL],
        memory_order_acquire);

    void (*orig)(hipStream_t, /* … */) =
        (void(*)(hipStream_t, /* … */))g_orig_hip_launch_kernel;

    if (__builtin_expect(prof_raw == 0, 1)) {
        /* Fast path — one atomic load, one branch, one tail-call.
         * Measured +0.8 ± 0.25 ns on EPYC 9354. */
        __attribute__((musttail)) return orig(s, /* … */);
    }

    /* Profiler attached — hand off to consumer-supplied callback,
     * passing the original so the callback can invoke it. */
    void (*prof)(void*, hipStream_t, /* … */) =
        (void(*)(void*, hipStream_t, /* … */))(uintptr_t)prof_raw;
    prof((void*)orig, s, /* … */);
}
```

Generated via a code-generator that reads the intercept-table schemas from rocprofiler-sdk's build system (same source of truth as today's `update_table` generator).

### 7.1 Alternative: GOTCHA rewrite (optional, Linux-only)

When the consumer has never attached and the process is long-running, the shim can optionally do a one-time GOT rewrite to replace `shim_wrap_*` with `g_orig_*` pointers directly, collapsing the fast-path cost to zero. On consumer attach, rewrite back. This is an optimization; the default path is the always-present wrapper as measured.

## 7A. Correlation IDs — mirror rocprofiler-sdk's model end-to-end

Correlation IDs are how a tool answers "which kernel did this `hipLaunchKernel` call cause?" and "which HSA enqueue came from which HIP entry?" The shim reproduces **exactly** the model rocprofiler-sdk exposes today, so a consumer can join shim records against SDK records on the same ID without any translation layer.

### 7A.1 Three ID spaces, same as SDK

Every record the shim emits carries the same `rocprofiler_correlation_id_t` shape the SDK ships:

```c
typedef struct {
    uint64_t                  internal;    /* shim-assigned, thread-local monotonic */
    rocprofiler_user_data_t   external;    /* user-pushed (Kineto, PyTorch, ...) */
    uint64_t                  ancestor;    /* internal-id of the enclosing wrapper */
} rocprofiler_correlation_id_t;
```

- **internal** — assigned by whichever shim wrapper is the outermost on this thread's stack when the call enters. Monotonic across the whole process. Sole source of truth for "this is a distinct call."
- **external** — the current top of the thread-local external-correlation stack. The shim exposes `rocprofiler_shim_push_external_correlation_id()` / `rocprofiler_shim_pop_external_correlation_id()` with identical semantics to the SDK's public API.
- **ancestor** — the `internal` ID of the wrapper one level up the call stack, or `0` at the top. This is what makes **cross-library correlation** work: if `hipMemcpy` internally calls `hsa_memory_copy`, the HSA record's `ancestor` is the HIP record's `internal`. Your consumer reconstructs the call tree by joining `ancestor → internal`.

### 7A.2 Thread-local correlation stack

Each thread owns a small stack (say, 16 entries deep — same as SDK's default) storing `(internal, external)` pairs. The shim's wrapper does:

```c
static void shim_wrap_<domain>_<op>(args...)
{
    uint64_t prof_raw = atomic_load_acquire(&profiler_functor[Op]);
    void*    orig     = g_orig_<domain>_<op>;

    if (__builtin_expect(prof_raw == 0, 1)) {
        /* Fast path. NO correlation push — it's bookkeeping we don't need
         * when nobody is listening. Same saving the SDK takes when no
         * context is active. Measured +0.8 ns/call; §7 of SHIM_COMPARISON. */
        __attribute__((musttail)) return ((orig_t)orig)(args);
    }

    /* Slow path — a consumer is attached. Full correlation bookkeeping
     * runs here, mirroring the SDK's per-call behavior. */
    rocprofiler_correlation_id_t corr = shim_push_correlation();
    /*
     *  shim_push_correlation() {
     *      uint64_t id  = ++tls.next_internal;
     *      uint64_t par = tls.stack_depth ? tls.stack[tls.depth-1].internal : 0;
     *      rocprofiler_user_data_t ext = tls.stack_depth
     *              ? tls.stack[tls.depth-1].external
     *              : g_thread_external_base;
     *      tls.stack[tls.depth++] = (corr_entry){ id, ext };
     *      return (rocprofiler_correlation_id_t){ id, ext, par };
     *  }
     *
     * If the SDK is also loaded, this additionally calls
     * rocprofiler_push_external_correlation_id(ext) so SDK-managed
     * downstream events (HSA queue dispatch, kernel completion,
     * counter samples) carry the same external ID. See §7A.4.
     */

    shim_cb_t prof = (shim_cb_t)(uintptr_t)prof_raw;
    prof(orig, &corr, args);   /* callback gets the full {internal,ext,anc} */

    shim_pop_correlation();
}
```

Cross-library correlation falls out naturally: when `hipMemcpy`'s wrapper is on the stack and HIP internally calls `hsa_memory_copy`, the HSA wrapper pushes its own internal ID, sees the HIP wrapper's entry on the stack beneath it, and stamps `ancestor = hip_internal_id`. The consumer reconstructs the parent-child chain with a hash join.

### 7A.3 External correlation IDs — user-facing API

```c
/* Public, for PyTorch / Kineto / any higher-level tool. Called BY USER
 * CODE running inside the target process (same as the SDK's equivalent). */
int rocprofiler_shim_push_external_correlation_id(rocprofiler_user_data_t id);
int rocprofiler_shim_pop_external_correlation_id (rocprofiler_user_data_t* out);
```

Implementation: thread-local stack of `rocprofiler_user_data_t` values. The shim's wrapper reads the current top on each entry and copies it into the correlation-id struct.

If the SDK is loaded in the same process, the shim's push/pop **also** calls the SDK's `rocprofiler_push/pop_external_correlation_id` under the same stack semantics. The two stacks stay synchronized; downstream SDK-emitted events (buffered tracing, kernel dispatch records, HW counter samples) carry the same external ID your shim record carries. **One push from user code, both stacks updated, one ID visible everywhere. **Caveat**: user code that calls the SDK's `rocprofiler_push_external_correlation_id` directly (bypassing the shim) will not be seen by the shim's stack; mixed use within a single process should pick one entry point. When in doubt, use the shim's push — it delegates to the SDK.** This is the design committed in §7A — one push, synchronized stacks, one ID everywhere.

### 7A.4 When rocprofiler-sdk is loaded — GPU-side events

Kernel dispatches, HSA queue events, HW counter samples, PC samples — none of these are host API calls and the shim does not see them directly. But when the SDK is loaded:

- Shim's `shim_push_correlation()` sets its `internal` ID into thread-local storage that the SDK's intercept-table wrappers read.
- Shim pushes its external ID via the SDK's `rocprofiler_push_external_correlation_id` (or skips if the user did it themselves — the stacks compose).
- SDK's HSA queue interceptor, at enqueue time, reads thread-local correlation and stamps it on the kernel-dispatch record it emits into its own buffer.

Result: the kernel-dispatch record the SDK writes to its buffer carries `external_id == shim's external_id` and `internal_id == SDK's own internal` (different space, intentional — `internal` is scoped to the emitter). Your consumer joins shim records and SDK kernel-dispatch records on `external_id`.

If the shim is running without the SDK, GPU-side events are simply not captured — same as running rocprofv3 with callback tracing but no buffered tracing today. Documented limitation; same architectural trade-off.

### 7A.5 Summary — what a consumer sees

Every shim record carries:

```
+------------------------------------------------------------+
| rocp_shim_record_t                                         |
|   tsc, kind, op, phase, thread_id                          |
|   correlation_id = { internal, external, ancestor }        |
|   args = typed payload (see §7B)                           |
+------------------------------------------------------------+
```

Joins:

- `ancestor → internal` reconstructs cross-library parent chains within the shim's host events.
- `external_id` reconstructs user-driven scopes (PyTorch module, training iteration, etc.).
- `external_id` also joins shim records to SDK-emitted GPU events when both coexist.

No new concepts, no translation table. Identical to how rocprofv3's JSON output joins today.

## 7B. Arguments — mirror rocprofiler-sdk's buffer tracing model

For every API the shim wraps, we need to deliver its arguments to a consumer in a different address space. The shim does exactly what the SDK's buffer tracer does: **inline-typed payload for scalars/handles, variable-size auxiliary ring for strings and deep-pointed data.** The schema that classifies each arg is the same one the SDK uses today.

### 7B.1 Typed payload — inline in the record

For every (domain, op), there is a generated typed struct — the **same struct** rocprofiler-sdk emits into its callback-tracing records today. Reuse, don't parallel-define. Source of truth lives in rocprofiler-sdk's headers.

```c
/* Generated from rocprofiler-sdk/hip/api_args.h — identical layout. */
typedef struct {
    const void*  func;
    dim3         grid;
    dim3         block;
    void**       kernarg_ptr;   /* pointer VALUE only, see §7B.3 */
    size_t       shmem;
    hipStream_t  stream;        /* opaque handle — pointer value */
} rocprofiler_hip_api_args_hipLaunchKernel_t;
```

Every shim ring record reserves a fixed payload area (default 256 bytes) big enough for every wrapped API's packed args. For APIs whose args exceed the inline budget, the record flags the overflow and the extra bytes go in the variable-size ring (§7B.2).

### 7B.2 Variable-size auxiliary ring — same role as SDK's buffered string pool

A second memfd-backed producer-consumer ring, mapped alongside the primary record ring, carrying:

- String-valued args (kernel names, file paths, shader source)
- Deep-copied structs the API schema marks "copy contents"
- Kernel argument buffers (for APIs like `hipLaunchKernel` where the `void**` points at a runtime-owned blob of scalar kernel args)

Each variable-size entry is prefixed with `{correlation_internal_id, arg_index, kind, length}` (4 fields, matching the struct at §5) so the consumer joins on correlation_id to the primary record. The SDK's buffer tracer does precisely this today (see `rocprofiler-sdk-tool/buffered_output.cpp`).

Layout:

```
variable ring entry:
    uint64_t  correlation_internal_id
    uint16_t  arg_index          /* which parameter of the API this blob is for */
    uint16_t  kind               /* STRING / DEEP_STRUCT / KERNARG_BLOB / ... */
    uint32_t  length
    uint8_t   payload[length]
```

### 7B.3 Pointer semantics — same as SDK callback tracing

When an arg is `hipStream_t stream`, the SDK's in-process callback gets the pointer value and can call `hipStreamGetName(stream)` (cheap, in-process). The consumer in the shim's OOP world has the same pointer value but cannot deref — it lives in a different address space.

The policy the shim applies per-arg, from the SDK schema:

| Schema classification | What the shim does | Consumer sees |
|---|---|---|
| **SCALAR** (`int`, `size_t`, `dim3`, enum) | Copy by value into the inline payload | Direct field access |
| **HANDLE** (`hipStream_t`, `hsa_queue_t`, `ihipModule_t*`) | Copy pointer value into the inline payload | Pointer as opaque ID |
| **FIXED_STRUCT** (`hipMemcpy3DParms`) | Copy struct by value into the inline payload | Direct struct access |
| **STRING** (`const char*`, null-terminated) | Write blob into variable-size ring | Join on correlation_id, read blob |
| **DEEP_STRUCT** (schema says "follow pointer, copy contents") | Serialize into variable-size ring | Join + deserialize |
| **KERNARG_BLOB** (`void** kargs` in `hipLaunchKernel`) | Opt-in per op — when enabled, copy kernel-arg bytes into variable-size ring | Join + deserialize per kernel's signature |
| **OUTPUT_ONLY** (`hipError_t*` return slot) | Skip at ENTER, capture at EXIT | Populated only on EXIT record |

The same schema classification drives the SDK's own `rocprofiler_iterate_callback_tracing_kind_operation_args` today. The shim's code generator reads that schema and emits per-op serializers; the SDK's generator emits the iterator. Same input, different outputs for different destinations.

**Kernarg blob caveat**: the shim captures `KERNARG_BLOB` at API entry (ENTER) because that is when the wrapper fires. The SDK's in-process buffer tracer captures kernargs inside the HSA queue-enqueue path where ordering is defined (the runtime has committed the args and holds the dispatch lock). The shim does **not** hold the runtime's dispatch lock, so the bytes it captures may be stale, partially filled, or reused by the runtime between API entry and actual GPU dispatch. `KERNARG_BLOB` capture via the shim is therefore **best-effort and not safe for correctness-critical tools**. Tools that need exact kernarg values should co-load the SDK and use its buffer tracing. This limitation is inherent to wrapping at the API level rather than at the enqueue level.

### 7B.4 Opt-in serialization — per-op, controlled by consumer

The variable-size ring is cost. Always emitting kernel-name strings for every `hipLaunchKernel` at MHz rates can swamp the consumer. The shim exposes per-op policy:

```c
/* Consumer side: */
int rocp_shim_set_arg_policy(rocp_shim_handle_t h,
                             uint32_t domain, uint32_t op,
                             rocp_shim_arg_policy_t policy);

enum rocp_shim_arg_policy_t {
    ROCP_SHIM_ARGS_INLINE_ONLY  = 0,  /* default: scalars + handles only */
    ROCP_SHIM_ARGS_WITH_STRINGS = 1,  /* plus string args in variable ring */
    ROCP_SHIM_ARGS_FULL         = 2,  /* plus DEEP_STRUCT / KERNARG_BLOB */
};
```

Written as another atomic slot in the memfd header; target reads per-op policy on every call (zero overhead if policy == INLINE_ONLY, which is the default).

### 7B.5 Consumer code — identical to SDK buffer tracing

```c
static void on_record(const rocp_shim_record_t* rec, ...)
{
    if (rec->kind == ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API &&
        rec->op   == HIPAPI_ID_hipLaunchKernel)
    {
        /* Same typed struct the SDK hands you. */
        const rocprofiler_hip_api_args_hipLaunchKernel_t* a =
            (const void*)rec->args;

        /* Kernel name: resolved via variable-size ring, joined on
         * correlation_id. Helper walks the ring for us. */
        const char* name = rocp_shim_get_string_arg(rec->correlation_id.internal,
                                                    /* arg_index */ 0);

        printf("[tsc=%" PRIu64 ", corr=%" PRIu64 " (parent %" PRIu64 ")] "
               "%s grid=%ux%ux%u\n",
               rec->tsc, rec->correlation_id.internal,
               rec->correlation_id.ancestor,
               name ? name : "?",
               a->grid.x, a->grid.y, a->grid.z);
    }
}
```

Line-for-line the same as a rocprofv3 callback-tracing tool — swap `rocprofiler_callback_tracing_record_t` for `rocp_shim_record_t` and the typed args cast is identical because the struct layout is sourced from the same header.

### 7B.6 Variable-ring backpressure policy

The variable-size auxiliary ring has its own capacity independent of the main record ring. The policy when it overflows:

1. **Check var-ring capacity before committing the main record.** If the variable-size payload (string, deep struct, kernarg blob) does not fit in the var ring, do **not** write to the var ring.
2. **Emit the main record anyway** with `arg_overflow = ARG_TRUNCATED` flag set. The inline-only payload (scalars, handles) is still complete.
3. **Increment `var_bytes_dropped`** counter in the header (new field; consumer reads it alongside `events_dropped` for capacity-planning feedback).
4. **Consumer must treat `ARG_TRUNCATED` records** as having no variable-ring data, even if a stale entry with a matching correlation_id happens to exist from a previous cycle.
5. **ENTER/EXIT atomicity**: if ENTER writes a variable-ring payload but EXIT cannot (var ring full between the two), set `arg_overflow` on the EXIT record only. The consumer has the ENTER's strings and the EXIT's inline data — acceptable degradation. If ENTER itself cannot write, both records carry `ARG_TRUNCATED`.

This is the same philosophy as the main record ring's drop-on-full: the target never blocks, the consumer adapts by enlarging the ring or reducing the traced set.

### 7B.7 OUTPUT_ONLY args and thread-death race

`OUTPUT_ONLY` args (e.g., the `hipError_t` return value) are captured at EXIT, not ENTER. If the calling thread is killed mid-call (signal, `pthread_cancel`, `abort()` inside the API), the ENTER record is emitted but EXIT is never written — the output slot stays empty and the consumer's ENTER/EXIT pairing logic orphans the record.

Mitigation: the shim registers a `pthread_cleanup_push` handler per thread on first instrumented entry. On thread death with an unbalanced correlation stack (depth > 0), the cleanup handler emits synthetic EXIT records with `phase = SHIM_EXIT_UNREACHED` for each outstanding ENTER. Consumer must handle `SHIM_EXIT_UNREACHED` as "call did not return; output args are invalid." The `atexit` handler does the same for the main thread.

## 8. In-process rocprofiler-sdk coexistence

The shim's wrappers sit between the runtime and the original function. If an in-process SDK tool is also loaded:

```
runtime call
    ↓
shim_wrap_hip_op          ← shim's functor
    │
    │ prof_raw = atomic_load(profiler_functor[Op])
    │ (if consumer attached, writes to ring)
    ↓
orig (≡ the shim's saved g_orig[Op])
    ↓
sdk_wrap_hip_op           ← installed by SDK's update_table()
    │ SDK's populate_contexts + enter/exit callbacks +
    │ buffer emplace — all its existing machinery
    ↓
real_hip_op               ← the runtime's actual function
```

- The shim saves the runtime's original function pointer in `g_orig[Op]` **once**, before any SDK wrapping happens.
- When the SDK later runs `update_table()`, it rewrites the api_table — but the api_table now already points at `shim_wrap_hip_op`. So SDK saves the shim's wrapper as its original, and installs `sdk_wrap_hip_op` over the top. The shim's wrapper remains in the call chain because nobody has rewritten the api_table again.
- When the shim's `g_orig[Op]` is called, it goes to the SDK's installed wrapper, which eventually calls what it saved — the shim's wrapper... wait. This would recurse.

**Correct ABI contract** (the subtlety flagged in SHIM_COMPARISON.md §6): the shim must expose a way for the SDK's `update_table()` to distinguish "raw runtime function" from "shim wrapper". Two options:

**Option A — shim exposes the runtime-original via a side-table**:

```c
// In the shim's public header:
void* rocprofiler_shim_get_runtime_original(uint32_t domain, uint32_t op);
```

SDK's `update_table()`, when it sees a shim-wrapped slot, calls this to get the real runtime function and installs its wrapper pointing at that.

**Semantics**: `get_runtime_original(D, Op)` always returns the pointer the shim captured at `shim_set_api_table` time — the raw runtime function, never updated, never the SDK's wrapper. The shim internally maintains a two-slot structure per op: `runtime_original` (const after first set, returned by this function) and `next_in_chain` (mutable, updated to point at the SDK's new wrapper when the SDK calls `update_table`). The shim's hot-path wrapper calls `next_in_chain`, not `runtime_original`, so the layered chain is `shim_wrap → sdk_wrap → real_runtime`. Consumer callbacks still fire on the outer shim_wrap.

This contract prevents infinite recursion if `update_table` is called multiple times (SDK calls it in phases during init) — each call gets the same constant `runtime_original`, not its own wrapper from the previous phase.

**Option B — SDK installs at a different layer**: SDK detects shim presence via `dlsym(RTLD_DEFAULT, "rocprofiler_shim_present")` and, if present, installs its wrappers into a **separate dispatch table** owned by the shim, not the runtime's table. Shim's wrapper then calls the SDK's table (not `g_orig[Op]` directly). This is cleaner but requires shim↔SDK API coordination.

Option A is the lower-coupling choice and is what this design assumes. Versioned via the shim's `struct_version`.

## 9. OMPT integration

OMPT does not use a dispatch table — it has a one-shot scan at OpenMP init for `ompt_start_tool`. The shim must therefore export its own `ompt_start_tool` and play the OMPT role itself:

```c
// In the shim — exported
ompt_start_tool_result_t*
ompt_start_tool(unsigned int omp_version, const char* rt_version) {
    /* Claim the OMPT session. Register no callbacks yet — the OMPT
     * hot path stays at native runtime cost. Save ompt_set_callback
     * so we can install callbacks later, when a consumer attaches. */
    static ompt_start_tool_result_t r = {
        .initialize = shim_ompt_initialize,
        .finalize   = shim_ompt_finalize,
        .tool_data  = { .value = 0 },
    };
    return &r;
}

static int
shim_ompt_initialize(ompt_function_lookup_t lookup,
                     int initial_device_num, ompt_data_t* tool_data)
{
    g_ompt_set_callback =
        (ompt_set_callback_t)lookup("ompt_set_callback");
    g_ompt_lookup = lookup;
    /* No callbacks set. OpenMP runs as if no tool were present. */
    return 1;
}
```

On consumer attach, if `profiler_functor[OMPT_BASE + Kind]` goes non-null, the shim's bg thread invokes `g_ompt_set_callback(Kind, &shim_ompt_cb)` to install a callback that writes OMPT events into the ring. On detach (slot set back to null), the shim calls `g_ompt_set_callback(Kind, NULL)` to unregister.

**OMPT + correlation IDs**: unlike dispatch-table wrappers (§7A.2), OMPT callbacks are invoked by the OpenMP runtime on worker threads, not through the shim's wrapper chain. The shim's `shim_ompt_cb` must therefore call `shim_push_correlation()` / `shim_pop_correlation()` explicitly around its ring write, so OMPT records carry proper `{internal, external, ancestor}` values. The `ancestor` field links OMPT events to a parent HIP/HSA dispatch entry if the same thread happens to be inside a wrapped API call (e.g. `hipLaunchKernel` → OpenMP target offload → OMPT callback) — the push sees the HIP entry on the thread-local stack and stamps it. If the OMPT callback fires on a pure OpenMP thread with no dispatch-table parent, `ancestor = 0`.

**Preemption caveat**: `ompt_start_tool` is first-match-wins per the OpenMP spec. If the user has `OMP_TOOL_LIBRARIES=libX_ompt.so` preloaded, OpenMP calls X's `ompt_start_tool` first and the shim's is never reached. The shim does not silently take over from another tool. Documented limitation; users running a specific OMPT tool must also run that tool's dispatch-table analogue if they want full visibility, or we provide a `ROCP_SHIM_OMPT_CHAIN` env var that lets the shim call a next-in-chain `ompt_start_tool` after its own.

## 10. Lifecycle diagrams

### 10.1 Attach

```
consumer                                target-shim bg thread
────────                                ──────────────────────
connect → socket in kernel queue
                                        accept() returns client fd
                                        getsockopt(SO_PEERCRED)
                                          uid check
                                          start_time check
                                        build shim_hello msg
                                        sendmsg(client, hello + SCM_RIGHTS)
recvmsg: hello + memfd + eventfd
verify magic / version / start_time
mmap memfd (RW shared)
(optional) close(socket)
                                        close(client)   ← peer has what it needs
                                        back to accept()

consumer writes profiler_functor[Op] = &cb
atomic_fetch_add(gen_counter, 1)
                                        (no action needed — hot path
                                         will see the new slot on next
                                         call via acquire-ordered load)

consumer poll(eventfd_fd)
                                        hot path: record written
                                        head % watermark == 0 → eventfd_write(1)
consumer drains ring, calls user_cb
consumer atomic_store(tail, new_tail)
```

### 10.2 Reconfigure (change traced set mid-session)

```
consumer                                target-shim
────────                                ──────────
atomic_store_release(
    profiler_functor[NewOp], &cb2);
atomic_fetch_add(gen_counter, 1);
                                        next call to NewOp:
                                          shim_wrap reads non-null slot
                                          → invokes cb2
                                        next call to OldOp:
                                          shim_wrap reads null slot
                                          → tail-calls original

(optional) atomic_store(
    profiler_functor[OldOp], 0);
```

No socket traffic. Zero latency between consumer's store and target's observation — each target thread sees the new slot on its next call via the acquire load.

### 10.3 Detach

```
consumer                                target-shim
────────                                ──────────
for each Op that was enabled:
  atomic_store_release(
    profiler_functor[Op], 0);
atomic_fetch_add(gen_counter, 1);

(optional) send SHIM_Q_DETACH
           over socket
                                        receive DETACH:
                                          record events_dropped into
                                          response, send it back

munmap(consumer_ctrl_region)
close(eventfd)
close(memfd)
(socket is already closed)
                                        (target continues running;
                                         all hot-path calls take fast
                                         path again; listen socket
                                         still open for next attach)
```

### 10.4 Crash recovery

| Who crashes | What happens in the target | What happens in the consumer |
|---|---|---|
| Consumer SIGKILL mid-session | All `profiler_functor[Op]` slots still point at the dead consumer's callback addresses. **Target will segfault on next call.** Primary defense: target's bg thread monitors the consumer's control socket via `poll(POLLHUP)` — kernel delivers HUP within **microseconds** of the consumer dying, at which point the bg thread atomically zeros all profiler_functor slots. This replaces the earlier 1 Hz heartbeat design and closes the ~1 s dangerous window. | N/A |
| Consumer SIGSEGV | Same as above — `POLLHUP` fires on process death regardless of signal. | N/A |
| Consumer clean exit without DETACH | Same POLLHUP mechanism — TCP-like close propagates to the server side. Slots zeroed within µs. | N/A |
| Target SIGKILL | memfd refcount drops, anonymous memory freed. eventfd refcount drops. abstract socket dies with bound-socket close. | Consumer's mmap becomes zero-filled (kernel unmaps the backing pages). Consumer's `poll(eventfd)` returns EPOLLHUP. Consumer detects and exits. |

**Consumer-liveness detection is load-bearing for correctness.** The target holds function pointers that are meaningful only inside the consumer's address space; if the consumer dies, those pointers dangle. The primary mechanism is `POLLHUP` on the control socket (µs-scale detection); the bg thread keeps the socket fd open for exactly this purpose even after the handshake completes. Fallback for the case where the consumer intentionally closed the socket post-handoff: a `liveness` counter in the memfd header, polled at 1 Hz by the bg thread, with all slots zeroed on staleness. Both mechanisms are part of the shim's design, not post-hoc.

## 11. Ring buffer protocol

Single-producer (target hot path) / single-consumer (external process) queue. Non-blocking, drop-on-full.

```
Target write:
  idx      = atomic_load_acquire(&hdr->head);
  next     = (idx + record_size) & hdr->mask;
  tail     = atomic_load_acquire(&hdr->tail);
  if ((next - tail) & hdr->mask_inverse == 0) {
      atomic_fetch_add(&hdr->events_dropped, 1);
      return;
  }
  memcpy(&ring[idx], &record, record_size);
  atomic_store_release(&hdr->head, next);
  atomic_fetch_add(&hdr->events_traced, 1);
  if ((next & watermark_mask) == 0) {
      eventfd_write(ef, 1);   ← wake consumer
  }

Consumer read:
  poll(ef, POLLIN);
  eventfd_read(ef, &val);     ← drains counter
  head = atomic_load_acquire(&hdr->head);
  tail = atomic_load_acquire(&hdr->tail);
  while (tail != head) {
      memcpy(&local, &ring[tail], record_size);
      process(local);
      tail = (tail + record_size) & hdr->mask;
  }
  atomic_store_release(&hdr->tail, tail);
```

Ordering argument: target's `store_release(head)` after the `memcpy` pairs with consumer's `load_acquire(head)` before reading the record. Consumer's `store_release(tail)` pairs with target's `load_acquire(tail)` on the next record write.

Drop vs block: the target never blocks. Dropping is the correct behavior for a tracing tool — a slow consumer should not corrupt application performance. The `events_dropped` counter in the header is the user's signal to run the consumer on a less-loaded core or enlarge the ring.

## 12. Security

Summary per threat:

| Threat | Defense |
|---|---|
| Another user attaches to trace me | `SO_PEERCRED` + `uid != geteuid()` rejection at `accept()`. Abstract socket lives in the kernel's net namespace — discoverable by peer via `/proc/net/unix`, but connecting gets blocked at auth. |
| Malicious same-UID peer reads my memfd contents | Same-UID access is intentional (SO_PEERCRED lets them in). The threat is within the UID boundary — same as any other same-UID process. |
| Malicious same-UID peer seals my memfd writable-out-of-existence | `F_SEAL_SEAL` prevents further seal additions. Writes remain allowed to both sides. |
| PID reuse | `start_time` from `/proc/self/stat` field 22 embedded in both the shim's header and the handshake message. Consumer must verify both match. Kernel-monotonic; re-use is detectable. |
| Stale session after target crashes | memfd/eventfd/socket are anonymous → die with the process. Nothing on disk to clean up. |
| Ring-buffer content poisoning | Consumer only reads — trust boundary is one-way. Target never executes data from the ring. |
| Consumer-side code injection via `profiler_functor` pointer | Consumer writes function pointers into the memfd that the target calls. This is intentional (that is the attach mechanism) and requires same-UID peering. The attack surface is the OOP-tool developer's code quality, not the channel. |

Abstract socket + sealed anonymous memfd together give **zero persistent filesystem entries** (see [MEMFD_SOCK.md § What "no filesystem footprint" precisely means](MEMFD_SOCK.md#what-no-filesystem-footprint-precisely-means) for the precise accounting).

## 13. Exported ABI

### 13.1 What the shim exports (visible to rocprofiler-register and to in-process SDK)

```c
/* Called by rocprofiler-register unconditionally when any runtime
 * registers a dispatch table. Shim rewrites the slots to its wrappers. */
int mock_sdk_set_api_table(const char* name, uint32_t version,
                           void** api_tables, uint64_t num_tables);

/* SDK can query this to learn the original runtime function behind
 * a shim-wrapped slot (for layered wrapping — see §8). */
void* rocprofiler_shim_get_runtime_original(uint32_t domain, uint32_t op);

/* Presence flag — SDK dlsym(RTLD_DEFAULT, "rocprofiler_shim_present")
 * returns non-null if the shim is in the process. SDK uses this to
 * decide between direct api_table wrapping (legacy) and shim-layered
 * wrapping (this design).
 * (Planned — not yet exported by the mock.) */
int rocprofiler_shim_present;

/* OMPT entry point — called by the OpenMP runtime at init.
 * (Planned — not yet exported by the mock.) */
ompt_start_tool_result_t* ompt_start_tool(unsigned int, const char*);

/* External-correlation push/pop — called by user code running inside the
 * target (PyTorch profiler, Kineto, HPCToolkit, ...). Identical semantics
 * to rocprofiler-sdk's public push/pop. When the SDK is also loaded in
 * the process, the shim forwards every push/pop to the SDK so the two
 * external-correlation stacks stay synchronized. See §7A.3.
 *
 * NOTE: the production shim takes rocprofiler_user_data_t (a union);
 * the current mock simplifies to uint64_t. See shim_mock.c for the
 * deliberate deviation and its rationale. */
int rocprofiler_shim_push_external_correlation_id(rocprofiler_user_data_t id);
int rocprofiler_shim_pop_external_correlation_id (rocprofiler_user_data_t* out);

/* --- Mock-only entry points (not in the production shim ABI) --- */
/* shim_set_profiler_functor — in the production shim, slot writes go
 * through the memfd+sock handshake; the mock exposes this for direct
 * in-process test control. */
int shim_set_profiler_functor(int op, void* fn);
void* shim_get_original(int op);      /* mock alias for rocprofiler_shim_get_runtime_original */
uint64_t shim_current_internal_id(void);  /* test introspection */
```

### 13.2 What the consumer-side library exports (visible to OOP tool binaries)

```c
/* Connect to a running shim-instrumented target. */
rocp_shim_handle_t rocp_shim_attach(pid_t target_pid);

/* Query capabilities (domain/op coverage). */
int rocp_shim_get_capabilities(rocp_shim_handle_t h,
                               rocp_shim_caps_t* out);

/* Enable / disable tracing for a specific (domain, op). */
int rocp_shim_enable_op(rocp_shim_handle_t h,
                        uint32_t domain, uint32_t op,
                        rocp_shim_cb_t cb, void* user_data);

int rocp_shim_disable_op(rocp_shim_handle_t h,
                         uint32_t domain, uint32_t op);

/* Poll the ring for records. Blocks on eventfd until watermark.
 * Returns the number of records handed to cb. */
int rocp_shim_poll(rocp_shim_handle_t h,
                   rocp_shim_record_cb_t cb, void* user_data,
                   int timeout_ms);

/* Query target stats. */
int rocp_shim_get_stats(rocp_shim_handle_t h, rocp_shim_stats_t* out);

/* Clean detach. */
int rocp_shim_detach(rocp_shim_handle_t h);

/* Per-op argument serialization policy. Default is INLINE_ONLY (scalars
 * + handles, zero variable-size ring traffic). See §7B.4. */
int rocp_shim_set_arg_policy(rocp_shim_handle_t h,
                             uint32_t domain, uint32_t op,
                             rocp_shim_arg_policy_t policy);

/* Resolve a variable-size arg (string, deep struct, kernarg blob) from
 * the auxiliary ring by joining on correlation_internal_id + arg_index.
 * Called from inside the consumer's record callback. */
const void* rocp_shim_get_var_arg(rocp_shim_handle_t h,
                                  uint64_t correlation_internal_id,
                                  uint16_t arg_index,
                                  uint32_t* out_length,
                                  uint16_t* out_kind);
```

### 13.3 User's consumer callback

```c
typedef void (*rocp_shim_cb_t)(
    void* orig,                /* original function pointer — can be called */
    const rocp_shim_context_t* ctx,
    void* args);

/* rocp_shim_context_t — passed to the callback on every event.
 * Subsumes the per-event metadata; args is the typed payload (§7B). */
typedef struct {
    uint32_t                         kind;           /* domain */
    uint32_t                         op;             /* op within domain */
    uint64_t                         tsc;            /* timestamp counter */
    uint64_t                         thread_id;
    rocprofiler_correlation_id_t     correlation_id; /* {internal, external, ancestor} — see §7A */
} rocp_shim_context_t;
/* args casts to the SDK's rocprofiler_<domain>_api_args_<op>_t — see §7B */
```

The callback is invoked **by the target** (not the consumer) with the original function pointer as its first argument. The callback is responsible for:
- Optionally invoking the original (pre/post semantics)
- Writing a record into the ring if it wants the consumer to see this event
- Returning; long-running callbacks stall the target

For records-only consumers, the shim provides a helper:

```c
/* Prebuilt consumer callback that just writes a record and calls orig. */
void rocp_shim_record_and_call(void* orig,
                               const rocp_shim_context_t* ctx,
                               void* args);
```

The common case (user wants OOP trace records, not side effects): `rocp_shim_enable_op(h, DOMAIN_HIP, OP_LAUNCH_KERNEL, rocp_shim_record_and_call, NULL)`.

### 13.4 Consumer-provided thread pool for record dispatch

The shim library **never creates its own threads** on the consumer side. The consumer owns every thread involved in record processing — their creation, affinity, priority, naming, and lifetime. This is a deliberate design choice: profiling tools running on HPC clusters or inside containers need to control thread counts, CPU-set masks, and NUMA placement; a library that spawns its own threads behind the consumer's back breaks those contracts.

The consumer can provide a thread pool at attach time:

```c
/* Thread-pool interface the consumer implements. The shim calls
 * submit() to hand a batch of records to a worker; the consumer's
 * pool picks which thread runs the work. */
typedef struct {
    void (*submit)(void* pool_ctx,
                   const rocp_shim_record_t* records,
                   size_t count,
                   rocp_shim_record_cb_t cb,
                   void* cb_user_data);
    void* pool_ctx;   /* opaque — passed back to every submit() call */
} rocp_shim_thread_pool_t;

/* Attach with a thread pool. If pool is NULL, rocp_shim_poll runs
 * the callback synchronously on the calling thread (single-threaded
 * default). If pool is non-NULL, rocp_shim_poll drains the ring,
 * partitions records into batches, and hands each batch to
 * pool->submit(). The pool decides which thread runs the callback. */
rocp_shim_handle_t rocp_shim_attach_ex(pid_t target_pid,
                                        const rocp_shim_thread_pool_t* pool);
```

How it works:

1. `rocp_shim_poll()` wakes on eventfd, drains the ring into a local batch array (same as today).
2. If no pool was provided: calls `cb(record, user_data)` synchronously for each record. Single-threaded. This is the simple default.
3. If a pool was provided: splits the batch by thread_id (records from the same target thread go to the same worker — preserves per-thread ordering) and calls `pool->submit()` for each partition. The consumer's worker threads call `cb(record, user_data)` on their own stacks.

The consumer controls:
- **How many threads** — create a pool with 1 thread and it's serial; create one with 64 and it fans out.
- **Thread attributes** — `pthread_attr_setaffinity_np`, priority, stack size, naming — all set by the consumer before passing the pool.
- **Ordering guarantees** — per-thread ordering is preserved (same target thread → same worker). Cross-thread ordering is not guaranteed (records from different target threads may be processed out of timestamp order). Consumer-side merge-sort on TSC if global ordering is needed.

This mirrors how rocprofiler-sdk's buffer-tracing callbacks work today: the SDK's `rocprofiler_flush_buffer` delivers records on a **user-controlled callback thread** (the one that calls flush), not on an SDK-internal thread. The shim extends this pattern to the OOP consumer side.

### 13.5 Per-op filter chain

Today `profiler_functor[Op]` is all-or-nothing: either an op is traced or it isn't. Real profiling sessions need finer control — "trace only `hipLaunchKernel`", "trace only HIP calls that take a `hipStream_t`", "trace only `hipMemcpyAsync` where `sizeBytes > 1 MB`". The shim provides a three-phase filter chain that runs **in the target's hot path** before any record is emitted, so filtered calls never touch the ring and pay only the filter-evaluation cost.

#### Phase 1 — Function name filter (cheapest, always evaluated first)

The consumer specifies a set of function names (or glob/regex patterns) to include or exclude. The shim maps each name to its `(domain, op)` index at filter-install time using the same name→op lookup table rocprofiler-sdk ships (the `rocprofiler_iterate_callback_tracing_kind_operation_names` family). At runtime the filter is a **per-op bit** in a bitmap stored in the memfd header — one bit per op, one bitmap per domain. The hot path checks this bit before the `profiler_functor` load:

```c
/* Inside shim_wrap_<domain>_<op>: */
if (!filter_bitmap_test(g_ctrl->name_filter[DOMAIN], OP))
    goto fast_path;   /* filtered out — tail-call original, no record */
```

Cost: one cache-line-resident bit test per call. ~0.3 ns on x86-64 (the bitmap lives in the same memfd header cache line the wrapper already touches for `profiler_functor`).

Consumer API:

```c
/* Include only the named functions. Names use the SDK's canonical
 * operation-name strings (e.g. "hipLaunchKernel", "hsa_queue_create").
 * Glob patterns accepted: "hip*", "hsa_memory_*". */
int rocp_shim_set_name_filter(rocp_shim_handle_t h,
                              uint32_t domain,
                              const char* const* include_names,
                              size_t include_count,
                              const char* const* exclude_names,
                              size_t exclude_count);
```

This is the same pattern rocprofiler-sdk uses today when the user passes `--hip-api hipLaunchKernel,hipMemcpyAsync` to rocprofv3 — a name→op resolution at tool init, then a per-op bitmask at runtime. Zero overhead for non-matching ops because the bit test short-circuits before any callback or ring write.

#### Phase 2 — Argument-type filter (medium cost, evaluated only for ops that passed Phase 1)

The consumer can restrict tracing to ops whose signature contains a specific argument type. Example: "trace only HIP functions that take a `hipStream_t` argument." The shim's code generator knows every op's argument types from the SDK schema (same source of truth as §7B). At filter-install time, the consumer specifies type names; the shim resolves them to the set of ops whose signatures include that type and sets the Phase 1 bitmap accordingly.

```c
/* Restrict the traced set to ops whose signature includes arg_type_name.
 * Intersects with the current name filter (if set). */
int rocp_shim_set_type_filter(rocp_shim_handle_t h,
                              uint32_t domain,
                              const char* arg_type_name);
/* Example: rocp_shim_set_type_filter(h, HIP_API, "hipStream_t")
 *   → enables only ops that have a hipStream_t parameter */
```

This filter is **resolved entirely at install time** — it turns into the same per-op bitmap as Phase 1. Zero additional hot-path cost beyond the bitmap test.

#### Phase 3 — Argument-value filter (highest cost, evaluated only for ops that passed Phases 1+2)

The consumer can filter on **runtime argument values**. Example: "trace only `hipMemcpyAsync` where `sizeBytes > 1048576`" or "trace only `hipLaunchKernel` where `stream == 0x7f...`". This requires inspecting the actual arguments at call time — the only phase that adds per-call cost beyond the bitmap test.

The consumer installs a **predicate function** that runs in the target's address space, receives the typed args struct (same layout as §7B.1), and returns accept/reject:

```c
/* Predicate signature — called in the target's hot path.
 * Must be fast, must not allocate, must not call wrapped APIs
 * (re-entrancy). Return 1 to accept (emit record), 0 to reject. */
typedef int (*rocp_shim_value_predicate_t)(
    uint32_t domain,
    uint32_t op,
    const void* args,    /* typed args struct per §7B.1 */
    void* predicate_ctx);

int rocp_shim_set_value_filter(rocp_shim_handle_t h,
                               uint32_t domain, uint32_t op,
                               rocp_shim_value_predicate_t pred,
                               void* predicate_ctx);
```

Example consumer-side predicate:

```c
static int only_large_memcpy(uint32_t domain, uint32_t op,
                             const void* args, void* ctx)
{
    (void)domain; (void)op; (void)ctx;
    const rocprofiler_hip_api_args_hipMemcpyAsync_t* a = args;
    return a->sizeBytes > 1048576;   /* 1 MB threshold */
}

rocp_shim_set_value_filter(h, HIP_API, OP_hipMemcpyAsync,
                           only_large_memcpy, NULL);
```

Hot-path cost of Phase 3: one indirect call to the predicate function per traced call that passed Phases 1+2. The predicate runs in the target's context (same thread, same address space) and must be async-signal-safe-ish (no allocation, no re-entrancy into wrapped APIs). Expected cost: 2–10 ns depending on predicate complexity — well below the ~50 ns cost of a full record write.

If no value predicate is set for an op, Phase 3 is skipped (the slot is NULL and the branch is not taken).

#### Filter evaluation order in the hot path

```c
static void shim_wrap_<domain>_<op>(args...)
{
    /* Phase 1: name/type bitmap (resolved at install time). */
    if (!filter_bitmap_test(g_ctrl->name_filter[DOMAIN], OP))
        goto fast_path;

    /* profiler_functor load — is anyone listening at all? */
    void* prof = atomic_load_acquire(&profiler_functor[OP]);
    if (prof == NULL)
        goto fast_path;

    /* Phase 3: value predicate (if installed for this op). */
    void* pred = atomic_load_acquire(&value_predicate[OP]);
    if (pred && !((predicate_t)pred)(DOMAIN, OP, &packed_args, pred_ctx))
        goto fast_path;

    /* All three phases passed — push correlation, call callback, emit record. */
    ...

fast_path:
    orig(args);
}
```

The bitmap test (Phase 1+2) is **before** the `profiler_functor` load, so ops filtered out by name or type never even check whether a consumer is attached. This is the cheapest possible rejection path — one bit test + branch, same cache line as the functor slot.

#### Summary

| Phase | What it filters | When resolved | Hot-path cost | Consumer API |
|---|---|---|---|---|
| 1 — Name | Function name (glob/regex) | Install time → bitmap | ~0.3 ns (bit test) | `rocp_shim_set_name_filter` |
| 2 — Arg type | Ops whose signature includes a given type | Install time → same bitmap | 0 ns (folded into Phase 1 bitmap) | `rocp_shim_set_type_filter` |
| 3 — Arg value | Runtime argument values (predicate function) | Each call | ~2–10 ns (indirect call) | `rocp_shim_set_value_filter` |

All three phases are optional and composable. A consumer that only wants name filtering pays ~0.3 ns per filtered-out call. A consumer that also wants value filtering pays an additional ~2–10 ns per call that passed the name filter. Calls that match all three phases proceed to the full callback + record-write path.

## 14. Concurrency model

### 14.1 Single-controller limit

`listen(1)` means one controller at a time. Rationale:
- Two consumers stomping on `profiler_functor` slots would race — each would reset slots the other was using.
- Two consumers reading the ring would race on `tail`.
- Multi-consumer fan-out should live in a separate process (one "aggregator" consumer that multiplexes to N subscribers) rather than in the shim's protocol.

### 14.2 Multi-tool within one consumer

A single consumer can register multiple callbacks (one per op). If two tools in the same consumer want to observe the same op, they coordinate via a consumer-side fan-out. The shim's protocol sees only one pointer per slot.

### 14.3 Target thread safety

Every thread of the target executes the hot path concurrently. `atomic_load_acquire` on `profiler_functor[Op]` per call is correct — each thread sees its own snapshot. If a consumer flips a slot mid-call, some threads may see the old pointer and some the new; both are safe function pointers (`NULL` is the safe fallback, else a valid consumer callback).

### 14.4 Generation counter

`gen_counter` in the header bumps on every slot change. Target can optionally batch-read all slots once per generation (future optimization — not required for correctness). Today it is purely informational.

## 15. Test and verification plan

Minimum validation suite:

1. **Hot-path noise floor** — the +0.8 ns/call figure in SHIM_COMPARISON.md § 7a. Reproduce via `scripts/benchmark_noop_noise.py`. Must remain in the ~0.5–1.5 ns range on EPYC-class x86-64.
2. **Attach time** — consumer connect → first record visible in ring should complete in < 10 µs (no `dlopen`, no SDK init).
3. **Enable/disable latency** — consumer slot-store → target sees the new pointer on next call. Measure via a "tight loop + flip slot from another process" test; expect < 1 µs per flip.
4. **Ring integrity under load** — 10M records written at saturating rate, consumer reads all; `events_traced - events_dropped` matches the target's internal count.
5. **Crash recovery** — target runs, consumer `SIGKILL`'s itself mid-session. Target's heartbeat watchdog must zero all slots within its 1 s polling budget. Subsequent calls must not segfault.
6. **PID reuse** — target exits, new process reuses the PID, consumer tries to connect to the old name. Must fail on `start_time` mismatch.
7. **Same-UID other peer** — attacker process with same UID attempts `connect`. Succeeds (same-UID is inside the trust boundary). Memfd contents visible. This is expected; cross-UID is what we defend against.
8. **Cross-UID other peer** — attacker process with a different UID attempts `connect`. `SO_PEERCRED` rejects before handshake.
9. **Seal tampering** — connected same-UID attacker attempts `fcntl(F_ADD_SEALS, F_SEAL_WRITE)` on its memfd. Must fail with `EPERM` because of `F_SEAL_SEAL`.
10. **In-process SDK coexistence** — running rocprofv3 against the same process does not break shim tracing; neither do shim callbacks interfere with rocprofv3's records.
11. **OMPT** — an OpenMP application with `profiler_functor[OMPT_BASE + ompt_callback_target]` set receives OMPT callbacks via the ring.

## 16. Open questions / follow-up

- ~~**Multi-threaded consumer-side callback invocation**~~ — **resolved**: see §13.4 below. The consumer provides a thread pool at attach time; `rocp_shim_poll` dispatches records to that pool. The shim never creates its own profiling threads — the consumer owns all threads and controls their affinity, priority, and lifetime.
- ~~**Per-op filters**~~ — **resolved**: see §13.5 below. Three-phase filter chain: (1) function-name matching, (2) argument-type matching, (3) argument-value matching. All phases run in the target's hot path before emitting a record; filtered calls never touch the ring.
- ~~**Correlation IDs across shim and in-process SDK**~~ — **resolved**: see §7A. Shim maintains a thread-local internal/external/ancestor stack with identical semantics to the SDK's; when the SDK is loaded, the shim forwards external pushes into the SDK's stack so downstream GPU events carry the same external ID. Cross-library correlation (HIP calling HSA etc.) works via the `ancestor` field.
- ~~**Argument serialization model**~~ — **resolved**: see §7B. Inline fixed-size typed payload in each record (scalars + handles) + variable-size auxiliary ring keyed by `correlation_internal_id` for strings and deep-copied data. Same two-tier structure the SDK's buffer tracer uses today; schema classification reused from rocprofiler-sdk's existing arg metadata.
- **Record schema evolution**: the inline typed payload is sourced from SDK headers. Target and consumer link independently; SDK minor versions may add fields (struct sizes are not ABI-stable across ROCm minor releases). **V1 policy**: embed `args_schema_version` (an integer derived from the SDK build-time struct hash) in every record. Consumer checks `args_schema_version` against its own compiled-in value; on mismatch, falls back to inline-only decoding (scalars at fixed offsets are stable; any field beyond the consumer's known size is ignored). This is a best-effort heuristic, not a hard ABI freeze — a full freeze (shim-owned wire structs with translation at the consumer helper) is tracked as a v2 improvement.
- **Ring size as a consumer option**: consumer may want larger rings for heavy workloads. Today the size is fixed at target startup. A `rocp_shim_resize_ring` query could reallocate if both sides agree, but `F_SEAL_GROW` prevents grow-in-place; would need a secondary memfd + handoff. Not a v1 feature.
- ~~**Aarch64 validation**~~ — **deferred**: this design targets x86-64 only for now. The +0.8 ns measurement is on AMD EPYC. Aarch64 is out of scope for the initial implementation; if it becomes in-scope, re-measure and adjust the atomic-ordering choices (`LDAR` on ARMv8 is heavier than x86's implicit acquire).
- **Windows alternative** (not a v1 goal — documented here so the next evaluation doesn't start from scratch):

  The entire IPC layer in this design is Linux-specific: `memfd_create` (anonymous shared memory), abstract Unix domain sockets (rendezvous), `SCM_RIGHTS` (fd handoff), `eventfd` (watermark wake), `SO_PEERCRED` (authentication), `F_SEAL_*` (integrity). None of these exist on Windows.

  A Windows port would need to replace each primitive:

  | Linux primitive | Windows equivalent | Notes |
  |---|---|---|
  | `memfd_create` + `mmap` | `CreateFileMapping(INVALID_HANDLE_VALUE, ...)` + `MapViewOfFile` | Named shared memory in the `Global\` or `Local\` namespace. Named → discoverable, unlike memfd's anonymous fd. |
  | Abstract Unix socket | Named pipe (`\\.\pipe\rocprof-shim_<pid>`) | Pipe provides rendezvous. `ConnectNamedPipe` blocks until a client arrives, analogous to `accept`. |
  | `SCM_RIGHTS` (fd handoff) | `DuplicateHandle` with target process handle | Requires `PROCESS_DUP_HANDLE` access right on the target, which is same-user by default under Windows ACLs. |
  | `SO_PEERCRED` (peer auth) | `GetNamedPipeClientProcessId` + SID matching via `OpenProcessToken` / `GetTokenInformation` | More verbose but equivalent: kernel tells you who connected. |
  | `eventfd` (watermark wake) | `CreateEvent` (named, auto-reset) or `CreateSemaphore` | Consumer `WaitForSingleObject`; target `SetEvent` on watermark. |
  | `F_SEAL_*` (integrity) | Security descriptor on the file mapping (deny `FILE_MAP_WRITE` to non-owner) | Not equivalent — Windows ACLs are per-object, not per-fd. Sealing is coarser-grained. |

  The shim's hot-path functor, the correlation stack, and the ring-buffer protocol are all platform-independent C — only the IPC bootstrap and the consumer-side `poll` loop would need a Windows backend. Estimated scope: ~500 LOC of platform-specific IPC code behind `#ifdef _WIN32`, plus a named-pipe-based `shim_bg_thread_win.c` replacing the socket+eventfd bg thread.

## 17. Files that would implement this

If this design is adopted, the concrete files (sketched — not yet committed):

```
src/
├── rocprofiler-sdk-shim/
│   ├── CMakeLists.txt
│   ├── include/rocprofiler-sdk-shim/
│   │   ├── abi.h                 # Shared types (record, context, caps)
│   │   ├── wrappers_hip.h        # Generated HIP wrapper declarations
│   │   ├── wrappers_hsa.h        # Generated HSA wrapper declarations
│   │   ├── wrappers_rccl.h       # Generated RCCL wrapper declarations
│   │   └── wrappers_ompt.h       # OMPT callback glue
│   ├── src/
│   │   ├── shim_ctor.c           # Library constructor, memfd+socket setup
│   │   ├── shim_set_api_table.c  # register-side hook, slot rewrite
│   │   ├── shim_bg_thread.c      # accept, SCM_RIGHTS, eventfd wake
│   │   ├── shim_wrappers_gen.c   # Generated wrappers (from rocprofiler schemas)
│   │   ├── shim_ring.c           # Ring-buffer producer
│   │   ├── shim_ompt.c           # ompt_start_tool + ompt_set_callback bridge
│   │   ├── shim_abi_query.c      # rocprofiler_shim_get_runtime_original etc.
│   │   ├── shim_heartbeat.c      # Consumer-liveness watchdog
│   │   └── shim_cleanup.c        # atexit + signal cleanup
│   └── tests/
│       ├── noise_floor_test.c    # §15.1
│       ├── attach_latency_test.c # §15.2
│       ├── ring_integrity_test.c # §15.4
│       ├── crash_recovery_test.c # §15.5
│       ├── pid_reuse_test.c      # §15.6
│       └── cross_uid_test.c      # §15.8
│
├── rocprofiler-shim-consumer/
│   ├── CMakeLists.txt
│   ├── include/rocprofiler-shim-consumer/
│   │   └── api.h                 # §13.2 public API
│   └── src/
│       ├── attach.c              # rocp_shim_attach
│       ├── poll.c                # rocp_shim_poll
│       ├── enable.c              # rocp_shim_enable_op / disable_op
│       ├── stats.c               # rocp_shim_get_stats
│       └── helpers.c             # rocp_shim_record_and_call
│
└── rocprofiler-sdk/  (existing, modified)
    └── source/lib/rocprofiler-sdk/
        └── intercept_table.cpp   # +50 lines: detect shim, use get_runtime_original
```

Approximate effort: ~3 KLOC of shim library, ~1 KLOC of consumer library, ~100 LOC of SDK integration. The generated wrappers (`shim_wrappers_gen.c`) come from the same schemas that drive today's SDK dispatch-table generator and scale linearly with the API surface.

---

**Cross-references**:
- [SHIM_COMPARISON.md](SHIM_COMPARISON.md) — why the shim architecture vs. the late-load stub
- [MEMFD_SOCK.md](MEMFD_SOCK.md) — why memfd+sock vs. the other three channels
- [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) — broader channel analysis and security comparison
- [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) — measured hot-path, attach, active-tracing numbers for the late-load stubs and the shim mock
