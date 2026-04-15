# libroc-shim: IPC Transport Layer for Out-of-Process Profiling — End-to-End Design

> **This document supersedes the earlier "shim wraps dispatch tables" design.** The architecture was fundamentally redesigned: `libroc-shim.so` is now a **transparent IPC transport** for rocprofiler-sdk's own buffered services, not a standalone profiler. The SDK does all profiling logic; the shim handles channeling and IPC.

## Table of Contents

- [§0. Scope](#0-scope)
- [§1. Architecture overview](#1-architecture-overview)
- [§2. Libraries](#2-libraries)
- [§3. Startup and dormancy](#3-startup-and-dormancy)
- [§4. Attach flow — replacing ptrace](#4-attach-flow--replacing-ptrace)
- [§5. API proxy protocol](#5-api-proxy-protocol)
- [§6. The tool_initialize relay](#6-the-tool_initialize-relay)
- [§7. Ring buffer — internal shim transport](#7-ring-buffer--internal-shim-transport)
- [§8. Record delivery — shim ring to consumer buffer](#8-record-delivery--shim-ring-to-consumer-buffer)
- [§9. Detach and cleanup](#9-detach-and-cleanup)
- [§10. Crash recovery](#10-crash-recovery)
- [§11. IPC channel — memfd + abstract socket + eventfd](#11-ipc-channel--memfd--abstract-socket--eventfd)
- [§12. Security](#12-security)
- [§13. What changes in rocprofiler-sdk](#13-what-changes-in-rocprofiler-sdk)
- [§14. What changes in rocprofiler-register](#14-what-changes-in-rocprofiler-register)
- [§15. Naming and configuration](#15-naming-and-configuration)
- [§16. Sysadmin system-wide access (future)](#16-sysadmin-system-wide-access-future)
- [§17. Open questions](#17-open-questions)
- [§18. Integration risks](#18-integration-risks)
- [Appendix A: What the shim owns vs. what it does NOT own](#appendix-a-what-the-shim-owns-vs-what-it-does-not-own)
- [Appendix B: Comparison with previous design](#appendix-b-comparison-with-previous-design)

## 0. Scope

This document is the end-to-end design for **`libroc-shim.so`** — an IPC transport layer that enables out-of-process (OOP) profiling of ROCm applications without ptrace, sudo, or code injection.

The shim is loaded unconditionally by `rocprofiler-register` into every ROCm process. It is dormant until an external consumer attaches. When a consumer attaches, the shim proxies the standard `rocprofiler_*` tool API over a memfd+socket IPC channel, allowing the consumer to configure rocprofiler-sdk's buffered tracing services remotely. The SDK writes records into the shim's internal ring buffer; the shim delivers them to the consumer.

**The shim does zero profiling logic.** It does not wrap dispatch tables, generate correlation IDs, or serialize arguments. rocprofiler-sdk handles all of that — the same code it runs for in-process tools. The shim is purely IPC + transport.

**Only buffered services** are supported for out-of-process use. Callback tracing (synchronous, same-thread) is inherently in-process and is not proxied.

Everything below assumes no sudo, no capabilities, no specific kernel version beyond Linux 3.17 (for `memfd_create`), and works on x86-64. Windows is out of scope for v1.

## 1. Architecture overview

```
Target Process                                    Consumer Process
──────────────                                    ────────────────

 HIP/HSA/RCCL/OpenMP runtimes
       │ (DT_NEEDED)
       ▼
 librocprofiler-register.so
       │ (unconditional dlopen)
       ▼
 libroc-shim.so                                    libroc-shim-consumer.so
   │                                                 │
   ├─ abstract socket  ◄────── IPC commands ──────►  ├─ socket (API proxy)
   ├─ internal ring    (SDK writes, shim reads)      │
   │  buffer(s)        ─── records over socket ───►  ├─ consumer's user buffer
   ├─ eventfd          ◄────── watermark wake ────►  │    (consumer-created memory)
   │                                                 │
   │  On consumer attach:                            │  Consumer's tool code:
   │    force_configure → SDK initializes            │    rocprofiler_force_configure()
   │    SDK creates buffer backed by shim ring       │    rocprofiler_create_buffer()
   │    SDK configures services per consumer          │    rocprofiler_create_context()
   │    SDK writes records into shim ring            │    rocprofiler_configure_buffer_tracing_service()
   │                                                 │    rocprofiler_start_context()
   │  At shim watermark:                             │    ... (identical to in-process tool)
   │    shim drains ring → sends over socket         │
   │                                                 │  Consumer's buffer callback fires
   │                                                 │    → processes SDK-native records
   │                                                 │
   ▼                                                 ▼
 rocprofiler-sdk                                   (no SDK needed in consumer)
   wraps dispatch tables
   generates correlation IDs
   serializes args
   emplaces buffer records → shim ring
```

**Key principle**: the consumer programs against the **exact same `rocprofiler_*` API** as an in-process tool. `libroc-shim-consumer.so` exports the same symbols as `librocprofiler-sdk.so`, but marshalls every call over the IPC channel. The consumer's source code is link-compatible:

```bash
# In-process tool:
gcc my_tool.c -lrocprofiler-sdk -o my_tool

# OOP tool (identical source code):
gcc my_tool.c -lroc-shim-consumer -o my_oop_tool
```

## 2. Libraries

| Library | Process | Loaded by | Purpose |
|---|---|---|---|
| `libroc-shim.so` | Target | rocprofiler-register (unconditional `dlopen`) | IPC channel + ring buffers + API proxy to SDK |
| `libroc-shim-consumer.so` | Consumer | Consumer tool (linked at build time) | Exports `rocprofiler_*` stubs that marshall over socket |
| `librocprofiler-register.so` | Target | DT_NEEDED by HIP/HSA/RCCL/OpenMP runtimes | Stores dispatch tables, loads shim + SDK |
| `librocprofiler-sdk.so` | Target | Loaded by shim via `force_configure` at consumer attach | Does all profiling — wraps tables, generates records |

The consumer process has **only `libroc-shim-consumer.so`** — no SDK, no register, no shim. It speaks the SDK's API but links a thin socket-proxy library.

The target process has the shim + register always loaded. The SDK enters only when a consumer attaches (via `force_configure`).

## 3. Startup and dormancy

```
T+0   exec → ld.so links libamdhip64 → DT_NEEDED librocprofiler-register
T+1   register constructor:
        if (getenv("ROC_SHIM_DISABLE")) → skip entirely
        dlopen("libroc-shim.so", RTLD_NOW | RTLD_GLOBAL)
T+2   libroc-shim.so constructor:
        memfd_create("roc-shim-ctrl", MFD_CLOEXEC | MFD_ALLOW_SEALING)
          → control header only (no rings yet — created at attach time)
        F_ADD_SEALS: SHRINK | GROW | SEAL
        socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)
        bind("\0roc-shim_<pid>")
        listen(backlog=1)
        pthread_create(bg_thread) → blocks on accept()
T+3   Runtimes register dispatch tables with rocprofiler-register
        register scans for rocprofiler_configure → not found (no tool)
        dispatch tables stay untouched
T+4   Application runs. libroc-shim.so is dormant.
        Cost: one bg thread sleeping in accept(). Zero hot-path overhead.
```

**Dormant state properties:**
- No SDK loaded in the process
- No dispatch table wrappers installed
- No ring buffers allocated (only the small control header memfd)
- No profiling overhead whatsoever
- The only resource cost: one sleeping pthread + one abstract socket fd + one small memfd

## 4. Attach flow — replacing ptrace

**Today (ptrace):**
```
rocprofv3 --attach --pid <N>
  → ptrace(PTRACE_ATTACH, pid) — needs CAP_SYS_PTRACE
  → inject dlopen into target
  → configure SDK in-process
  → ptrace(PTRACE_DETACH, pid)
```

**New (shim):**
```
rocprofv3 --attach --pid <N>     (or any OOP tool)
  → links libroc-shim-consumer.so
  → connect("\0roc-shim_<pid>") — same UID only (SO_PEERCRED)
  → rocprofiler_force_configure(tool_init)
    → marshalled to libroc-shim.so in target
    → SDK initializes, tool_init relayed, services configured
  → records flow: SDK → shim ring → socket → consumer buffer
  → consumer's buffer callback fires with SDK-native records
```

| Aspect | ptrace (current) | shim (new) |
|---|---|---|
| Privilege | `CAP_SYS_PTRACE` or `ptrace_scope=0` | Same UID only (`SO_PEERCRED`) |
| Mechanism | Inject code into target | Send commands over socket |
| Target disruption | Target paused during inject | Target never paused |
| Works in containers | Requires `SYS_PTRACE` capability | Works (abstract socket in net namespace) |
| Detach | ptrace detach + in-process cleanup | `stop_context` + `flush` over socket |
| Code loaded at attach | Tool `.so` injected into target | SDK loaded via `force_configure` (standard path) |

## 5. API proxy protocol

Every `rocprofiler_*` call the consumer makes is serialized as a request/response message pair over the socket.

```c
struct shim_request {
    uint32_t msg_id;        // monotonic, for matching responses
    uint32_t api_id;        // which rocprofiler_* function
    uint32_t payload_size;  // bytes of serialized args following
    // ... serialized args follow ...
};

struct shim_response {
    uint32_t msg_id;        // matches the request
    uint32_t status;        // rocprofiler_status_t return value
    uint32_t payload_size;  // bytes of output args following
    // ... serialized output args follow ...
};
```

**Proxied APIs (v1 — buffered services only):**

| API | Direction | Notes |
|---|---|---|
| `rocprofiler_force_configure` | consumer → shim → SDK | Triggers the tool_initialize relay |
| `rocprofiler_create_context` | consumer → shim → SDK | Returns context_id |
| `rocprofiler_create_buffer` | consumer → shim → SDK | Shim creates internal ring, SDK writes there. Returns buffer_id. |
| `rocprofiler_configure_buffer_tracing_service` | consumer → shim → SDK | Enables domain+ops for context+buffer |
| `rocprofiler_start_context` | consumer → shim → SDK | SDK starts wrapping, records start flowing |
| `rocprofiler_stop_context` | consumer → shim → SDK | SDK stops, wrappers go noop |
| `rocprofiler_flush_buffer` | consumer → shim → SDK | Shim drains ring → socket → consumer buffer |
| `rocprofiler_destroy_buffer` | consumer → shim → SDK | Shim tears down internal ring |
| `rocprofiler_destroy_context` | consumer → shim → SDK | SDK cleanup |
| `tool_initialize` callback | shim → consumer | Relayed during force_configure |
| `tool_finalize` callback | shim → consumer | Relayed at shutdown |
| buffer watermark callback | shim → consumer (via records over socket) | Shim drains ring, sends records |

**Not proxied (v1):**

- Callback tracing services (inherently in-process, not supported OOP)
- `rocprofiler_iterate_*` introspection APIs (consumer can use these locally against SDK headers)
- PC sampling, counter collection (future scope)

## 6. The tool_initialize relay

The consumer's `tool_initialize` is a **synchronous interactive session** over the socket. Each `rocprofiler_*` call inside it is a blocking request/response.

```
Consumer                    Socket                    libroc-shim.so              SDK
────────                    ──────                    ──────────────              ───

force_configure(my_init)
  → FORCE_CONFIGURE {}
                                                      force_configure(
                                                        shim_proxy_init)
                                                                                  calls shim_proxy_init()
                                                      ← TOOL_INIT_BEGIN {}

  consumer's tool_initialize:
    create_buffer(size, watermark, cb)
      → CREATE_BUFFER {size, wm}
                                                      create internal ring (memfd)
                                                      SDK create_buffer → shim ring
                                                      ← CREATE_BUFFER_RESP {buf_id}

    create_context()
      → CREATE_CONTEXT {}
                                                      SDK create_context
                                                      ← CREATE_CONTEXT_RESP {ctx_id}

    configure_buffer_tracing(ctx, HIP, buf, ops)
      → CONFIG_BUF_TRACE {ctx, kind, buf, ops}
                                                      SDK configure service
                                                      ← CONFIG_RESP {status}

    start_context(ctx)
      → START_CONTEXT {ctx}
                                                      SDK start_context
                                                      → dispatch tables wrapped
                                                      → records start flowing
                                                      ← START_RESP {status}

    return 0
  → TOOL_INIT_DONE {rc=0}
                                                      shim_proxy_init returns
                                                                                  SDK initialized

  ← FORCE_CONFIGURE_RESP {SUCCESS}
```

This is not on any hot path — `tool_initialize` runs exactly once per consumer lifetime.

## 7. Ring buffer — internal shim transport

When the consumer calls `rocprofiler_create_buffer`, the shim creates an **internal ring buffer** backed by a memfd. The SDK writes records into this ring. The consumer **never sees this ring** — it's an internal transport between the SDK and the shim.

```
libroc-shim.so creates:
  memfd_create("roc-shim-ring-N")
  ftruncate(shim_ring_size)    ← shim decides, not consumer
  mmap(PROT_READ|PROT_WRITE, MAP_SHARED)
  F_ADD_SEALS: SHRINK | GROW | SEAL

shim provides this mmap region to SDK as the buffer backing store.

SDK emplace path writes here:
  memcpy(ring + head, record, record_size)
  head += record_size
  if (head >= shim_watermark)
    shim_watermark_callback()
```

**Ring parameters:**

| Parameter | Controlled by | Value |
|---|---|---|
| Ring size | Shim (`ROC_SHIM_RING_SIZE_MB` env, default 1 MiB) | Power of two, clamped [1, 256] MiB |
| Ring watermark | Shim (internal, not exposed to consumer) | Tuned for batch efficiency (e.g., 64 KB) |
| Ring format | SDK | SDK-native `buffer_tracing_*_record_t` structs |
| Ring lifetime | Shim | Created at `create_buffer`, destroyed at `destroy_buffer` or consumer disconnect |

**One ring per consumer-created buffer.** If the consumer creates 2 buffers (e.g., one for HIP API, one for kernel dispatches), the shim creates 2 internal rings.

## 8. Record delivery — shim ring to consumer buffer

```
SDK hot path (target thread)         libroc-shim.so                   libroc-shim-consumer.so
────────────────────────────         ──────────────                    ───────────────────────

hipLaunchKernel(...)
  → SDK wrapper
  → buffer_record_emplace
  → record in shim internal ring

                                     ring crosses SHIM_WATERMARK:
                                       SDK calls shim_watermark_callback

                                     shim drains ring:
                                       read [tail → head)
                                       batch into socket message:
                                         SHIM_MSG_RECORDS {
                                           buffer_id,
                                           n_records,
                                           total_bytes,
                                           records[]  ← SDK-native structs
                                         }
                                       send(socket, msg)
                                       advance tail

                                                                      recv(socket, msg)

                                                                      write records into
                                                                      consumer's user buffer
                                                                      (consumer-allocated memory)

                                                                      user buffer crosses
                                                                      consumer's watermark?
                                                                        → call consumer's
                                                                          buffer_callback(
                                                                            ctx, buffer_id,
                                                                            records, n_records)

                                                                      consumer processes records
```

**Record format: SDK-native, no translation.**

Records are the exact same structs the SDK produces for in-process buffer tracing:
- `rocprofiler_buffer_tracing_hip_api_record_t`
- `rocprofiler_buffer_tracing_hip_api_ext_record_t`
- `rocprofiler_buffer_tracing_hsa_api_record_t`
- `rocprofiler_buffer_tracing_kernel_dispatch_record_t`
- etc.

No shim-specific record format. The consumer uses standard `rocprofiler-sdk` headers.

**Two-tier watermark:**

- **Shim watermark** (internal): controls when the shim drains the ring and sends a batch over the socket. Tuned for batch efficiency — shim's decision, not exposed to consumer.
- **Consumer watermark** (user-facing): the watermark the consumer specified in `create_buffer`. `libroc-shim-consumer.so` honors this — it accumulates records in the user buffer and fires the consumer's callback when the user's watermark is crossed.

**Drop policy:**

- The SDK writes freely into the ring. If the ring fills (consumer too slow to drain), the SDK's next emplace sees a full buffer and increments `events_dropped`.
- The shim does NOT block the SDK. The watermark callback returns immediately.
- `events_dropped` is visible to the consumer via `rocprofiler_get_buffer_tracing_info` (proxied).

## 9. Detach and cleanup

```
Consumer                         libroc-shim.so                SDK
────────                         ──────────────                ───

stop_context(ctx)
  → STOP_CONTEXT {ctx}
                                 stop_context(sdk_ctx)
                                                               SDK stops, wrappers noop
                                 ← STOP_RESP {OK}

flush_buffer(buf)
  → FLUSH_BUFFER {buf}
                                 drain remaining records
                                 from shim ring → socket
                                 ← FLUSH_RESP {n_records}

  recv final records
  consumer callback fires

destroy_buffer(buf)
  → DESTROY_BUFFER {buf}
                                 destroy_buffer(sdk_buf)
                                 munmap shim ring
                                 close ring memfd
                                 ← DESTROY_RESP {OK}

destroy_context(ctx)
  → DESTROY_CONTEXT {ctx}
                                 destroy_context(sdk_ctx)
                                 ← DESTROY_RESP {OK}

close(socket)
                                 bg thread: POLLHUP
                                 cleanup any remaining state
                                 back to accept() — dormant again
```

**The target survives consumer detach.** After cleanup, `libroc-shim.so` returns to dormant — bg thread sleeping in `accept()`, no SDK wrappers, zero overhead. A new consumer can attach later (subject to `force_configure` one-shot — if the SDK was already initialized, the new consumer gets `CONFIGURATION_LOCKED`).

## 10. Crash recovery

| Who crashes | Target behavior | Consumer behavior |
|---|---|---|
| **Consumer SIGKILL** | Shim detects `POLLHUP` on socket (µs). Stops all contexts, flushes rings, tears down state, returns to dormant. Records in flight are lost — acceptable for profiling. | N/A |
| **Consumer clean exit without detach** | Same as SIGKILL — `POLLHUP` triggers cleanup. | N/A |
| **Target SIGKILL** | N/A | Consumer's socket returns `EPOLLHUP`. Consumer detects and exits. |

The consumer **must** keep the socket open for the session lifetime. `POLLHUP` is the sole liveness signal. Closing the socket = detach.

## 11. IPC channel — memfd + abstract socket + eventfd

Same IPC primitives as the earlier design — these are transport-layer choices independent of the profiling architecture.

**Abstract socket:** `\0roc-shim_<pid>` in the kernel's network namespace. No filesystem entry. Dies with the process.

**Authentication:** `SO_PEERCRED` — kernel-verified `uid == geteuid()`. Checked at `accept()`.

**Memfd:** Used for the control header and for each internal ring buffer. Sealed with `F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL`.

**Eventfd:** One per ring buffer. Shim fires it at watermark to wake the consumer's recv loop in `libroc-shim-consumer.so`.

**PID-reuse defense:** `start_time` from `/proc/self/stat` field 22 embedded in the control header. Consumer verifies at connect time.

**Zero persistent filesystem entries.** Abstract socket + anonymous memfd = nothing on disk, nothing to clean up on crash.

## 12. Security

| Threat | Defense |
|---|---|
| Cross-UID attach | `SO_PEERCRED` `uid != geteuid()` → rejected at `accept()` |
| PID reuse | `start_time` verification in control header |
| Stale session after crash | All IPC artifacts (socket, memfd, eventfd) are anonymous → die with process |
| Consumer code injection | **Not possible.** The shim proxies SDK API calls — it does not execute consumer code in the target. The consumer sends structured commands; the shim validates and translates them to SDK calls. |
| Malicious SDK commands | The shim validates every proxied API call (bounds checks, valid context/buffer IDs) before forwarding to the SDK. Invalid commands return error status. |
| Ring buffer poisoning | Only the SDK writes to the ring (target-side). Consumer has no write access to the ring — it receives records via the socket. |

## 13. What changes in rocprofiler-sdk

**Minimal changes — the shim works with the SDK's existing public API.**

| Change | Where | Scope |
|---|---|---|
| Accept external buffer backing store | `force_configure` path / buffer creation | When the configuring tool is the shim, the SDK uses the shim's memfd-backed ring as the buffer's storage instead of allocating its own memory. The emplace path is unchanged — it just writes to different memory. |
| Detect shim presence | `force_configure` path | SDK detects that the tool came through the shim (e.g., a flag in the tool_configure result, or the shim sets an env var). |
| Call shim watermark callback | Buffer watermark path | The shim's callback is wired as the buffer's watermark callback via standard `create_buffer` API. No special path needed if the SDK already supports caller-provided watermark callbacks. |

**What does NOT change in the SDK:**

- Dispatch table wrapping (SDK already does this)
- Correlation IDs (SDK already generates them)
- Arg serialization (SDK already does this for buffer tracing)
- `force_configure` semantics (already public API, one-shot)
- Context/service configuration logic
- Buffer record emplace path (writes to buffer's backing store — if that's a memfd mmap, records go there automatically)
- Record format (`buffer_tracing_*_record_t` structs are unchanged)

**Estimated SDK changes:** ~100-200 LOC.

## 14. What changes in rocprofiler-register

One change: add unconditional `dlopen("libroc-shim.so")` in the register constructor, before the tool scan.

```c
// In rocprofiler_register.cpp constructor, before tool_present() scan:
if (!getenv("ROC_SHIM_DISABLE")) {
    dlopen("libroc-shim.so", RTLD_NOW | RTLD_GLOBAL);
    // shim ctor runs: sets up socket, bg thread
    // shim does NOT export rocprofiler_configure
    // → tool_present() scan still works as before
}
```

The shim does NOT export `rocprofiler_configure`, so register's tool scan is unaffected. If an in-process tool is also present, it configures the SDK normally. The shim is dormant until a consumer attaches.

**Estimated register changes:** ~20 LOC.

## 15. Naming and configuration

**Libraries:**
- `libroc-shim.so` — in-process IPC transport
- `libroc-shim-consumer.so` — consumer-side API proxy

No mention of `rocprofiler`, `profiler`, or `tracer` in any user-facing name.

**Headers:**
The consumer includes standard `rocprofiler-sdk` headers. No shim-specific headers needed.

**Environment variables:**

| Variable | Default | Effect |
|---|---|---|
| `ROC_SHIM_DISABLE=1` | not set | Register skips `dlopen("libroc-shim.so")` |
| `ROC_SHIM_RING_SIZE_MB=N` | 1 | Size of each internal shim ring buffer |

**Socket:** `\0roc-shim_<pid>` (abstract namespace)

## 16. Sysadmin system-wide access (future)

**Intent:** A system administrator can run a tool that peeks at the shim's internal ring buffers across all ROCm processes on a node to collect aggregate metrics — GPU utilization, API call rates, kernel dispatch counts — without configuring or controlling any individual process's profiling. Read-only, non-disruptive.

**How it would work:**
- Sysadmin tools discover shim-instrumented processes by scanning `/proc/net/unix` for `@roc-shim_*` sockets
- Connect and send a `PEEK_STATUS` command (read-only, does not require `force_configure`)
- Receive counters: events_traced, events_dropped, active services, ring utilization
- Future: `PEEK_RING` for read-only access to ring contents (record headers for aggregation)

**v1 scope:** Not implemented. The shim's socket and ring are designed to be extensible for this. The `listen(backlog=1)` may need to change to allow peek connections alongside the primary consumer.

## 17. Open questions

1. **Direct ring sharing optimization** — current design sends records over the socket from the shim ring to the consumer. A future optimization could share the ring directly (consumer mmaps the shim's memfd read-only) to eliminate the socket copy for the record data path. This changes the consumer's buffer model and needs careful design for watermark semantics and multi-reader support.

2. **In-process + OOP coexistence** — if an in-process tool already called `force_configure`, the OOP consumer gets `CONFIGURATION_LOCKED`. A future enhancement could allow the SDK to accept multiple clients (each with its own buffer). This requires SDK-side changes beyond v1.

3. **SDK buffer backing store mechanism** — the exact API for the shim to provide its memfd ring as the SDK's buffer storage needs detailed SDK-side design. Options: a new `rocprofiler_create_buffer_with_external_memory()` variant, or the shim's `tool_initialize` pre-allocates the memory and passes it through the existing `create_buffer` with a convention.

4. **Record batching and flow control** — the shim's internal watermark vs the consumer's watermark creates a two-tier buffering system. Tuning guidance (watermark values, batch sizes, socket buffer sizes) is implementation-time work.

5. **Graceful flush semantics** — when the consumer calls `flush_buffer`, the shim must drain the ring synchronously and deliver all pending records before returning the response. The SDK's `flush_buffer` semantics need to align with this (ensure all in-flight emplaces complete before the flush returns).

6. **API versioning** — the socket protocol needs a version handshake so `libroc-shim.so` and `libroc-shim-consumer.so` can detect mismatches (e.g., consumer built against newer SDK headers than the target's shim).

## 18. Integration risks

### 18.1 rocprofiler-register semantic change

The design requires register to unconditionally `dlopen` the shim. Today register only loads libraries when a tool is detected or `ROCPROFILER_REGISTER_FORCE_LOAD` is set. This is a deliberate behavioral change that needs register maintainer buy-in. `ROC_SHIM_DISABLE=1` provides a kill switch.

### 18.2 force_configure one-shot semantics

`force_configure` is locked after the first call. If an in-process tool configures the SDK before the OOP consumer attaches, the OOP attach fails with `CONFIGURATION_LOCKED`. This is the same constraint the current ptrace attach has — it's not a regression, but it limits coexistence.

### 18.3 SDK buffer storage externalization

The SDK must accept the shim's memfd ring as the buffer backing store. If the SDK's buffer implementation tightly couples allocation with its internal memory manager, this may require refactoring the buffer creation path. Estimated scope: small if the SDK already supports caller-provided memory; moderate if not.

### 18.4 Socket protocol stability

The request/response protocol between `libroc-shim.so` and `libroc-shim-consumer.so` is a new ABI. Both libraries must be version-matched (or at least forward-compatible). A version handshake at connect time is essential.

---

## Appendix A: What the shim owns vs. what it does NOT own

| Responsibility | Owner |
|---|---|
| IPC channel (socket, memfd, eventfd) | **libroc-shim.so** |
| Ring buffer lifecycle (create, seal, share, watermark) | **libroc-shim.so** |
| API proxy (marshall `rocprofiler_*` calls) | **libroc-shim.so** + **libroc-shim-consumer.so** |
| Record delivery (ring → socket → consumer buffer) | **libroc-shim.so** + **libroc-shim-consumer.so** |
| Attach/detach (replacing ptrace) | **libroc-shim.so** |
| Crash recovery (POLLHUP → cleanup) | **libroc-shim.so** |
| Dispatch table wrapping | **rocprofiler-sdk** |
| Correlation IDs | **rocprofiler-sdk** |
| Arg serialization / record format | **rocprofiler-sdk** |
| Context management logic | **rocprofiler-sdk** |
| Buffer tracing service configuration | **rocprofiler-sdk** |
| Which services to enable | **Consumer** (via standard `rocprofiler_*` API) |

## Appendix B: Comparison with previous design

The earlier design (documented in the `shim_mock/` code) had the shim wrapping dispatch tables, generating its own records, managing its own correlation IDs, and serializing args. That design was a functional prototype that validated the IPC transport (memfd+sock, ring buffer, eventfd wake, POLLHUP recovery, per-table registration, SO_PEERCRED auth).

| Aspect | Previous design | Current design |
|---|---|---|
| Who wraps dispatch tables | Shim | **SDK** |
| Who generates records | Shim (custom format) | **SDK** (native `buffer_tracing_*` format) |
| Who manages correlation IDs | Shim (thread-local stack) | **SDK** (existing correlation infrastructure) |
| Who serializes args | Shim (per-op formatters) | **SDK** (existing arg iteration) |
| Consumer API | Shim-specific (mode selectors, filter bitmaps) | **Standard `rocprofiler_*` API** (source-compatible) |
| What the shim does | Profiling + IPC | **IPC only** |
| Consumer needs to know about shim? | Yes (shim-specific types, protocols) | **No** (programs against SDK headers) |
| Ring buffer contents | Shim-format records | **SDK-native records** |
| Supported services | All (shim intercepted everything) | **Buffered services only** (callback tracing is in-process only) |

The IPC transport layer (memfd+sock, ring buffer, eventfd, POLLHUP, SO_PEERCRED) carries over unchanged. The profiling-logic layer is removed from the shim entirely — the SDK handles it.

---

**Cross-references:**
- [MEMFD_SOCK.md](MEMFD_SOCK.md) — detailed IPC channel analysis (memfd+sock hybrid rationale, security comparison, "no filesystem footprint" accounting)
- [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) — survey of 13 IPC mechanisms, elimination criteria
- [BENCHMARK_RESULTS.md](BENCHMARK_RESULTS.md) — measured hot-path overhead for the IPC mock (transport-layer numbers still apply)
