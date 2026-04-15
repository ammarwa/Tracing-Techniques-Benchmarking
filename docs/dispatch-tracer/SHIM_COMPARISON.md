# Dispatch Tracer vs. rocprofiler-sdk-shim — Design Comparison

This document compares the two late-attach OOP-profiling designs that have been proposed for rocprofiler-sdk-adjacent tracing:

- **Late-load stub** (the design surveyed in the rest of this directory): a tiny library is `LD_PRELOAD`'d into the target process; it defers loading rocprofiler-sdk until a controller attaches.
- **rocprofiler-sdk-shim** (Jonathan Madsen's proposal): a shim library is `dlopen`'d unconditionally by rocprofiler-register when it receives a dispatch table, installs a lightweight "maybe-call-profiler" functor over every entry from the start, and flips a pointer at attach time instead of loading the SDK.

Both designs solve the same real problem — late-attach, no-sudo, no-ptrace OOP profiling for HIP/HSA/RCCL/OMPT — but from opposite architectural ends. This doc lays out what is different, what is the same, whether either violates the requirements we set in [CONTROL_CHANNEL_SURVEY.md § Requirements](CONTROL_CHANNEL_SURVEY.md#requirements), and how our preferred IPC hybrid (**memfd + socket**, see [MEMFD.md](MEMFD.md)) plugs into each.

## TL;DR

| Dimension | Late-load stub (ours) | rocprofiler-sdk-shim (Jonathan's) |
|---|---|---|
| How the tracer enters the process | `LD_PRELOAD=librocp_stub_<channel>.so` | None — rocprofiler-register always `dlopen`s the shim when a runtime registers a table |
| State of dispatch tables before any OOP attach | Original pointers, never wrapped | Wrapped from process start by a shim `functor(args...)` |
| Hot-path cost when no OOP attached | **Genuine 0 ns** (verified 20×1M-iter, stub-vs-baseline indistinguishable at 95% CI) | **One atomic load + one well-predicted branch** per call (Jonathan argues noise-equivalent, citing prior `disabled-sdk-contexts` measurements) |
| Attach cost | ~1.8 ms (`dlopen` tool library + `rocprofiler_force_configure` + `update_table()` propagation) | ≈ single atomic store into `profiler_functor` slot |
| Does rocprofiler-sdk get loaded for OOP profiling? | **Yes** — at attach | **No** — shim has its own OOP API; SDK stays out of the address space |
| Coexistence of in-process SDK + OOP shim tool | Awkward — SDK is the only wrapper machinery | Natural — SDK layers its wrappers on top of the shim's `functor` |
| OMPT handling | Stub exports a silent `ompt_start_tool` (design contract; not in the mock yet) | Shim must do the same — `ompt_start_tool` is a one-shot OpenMP init scan, not a dispatch table |
| IPC control channel surface (auth, rendezvous, config transport) | memfd+sock hybrid (see [MEMFD.md](MEMFD.md)) | Same IPC survey applies — same memfd+sock hybrid recommended |
| New code we own and ship | ~20 KiB stub per channel | A new library (`librocprofiler-sdk-shim.so`) with its own OOP API + external shim tool |
| What gets relaxed relative to requirements | Nothing | "0 ns hot path" → "noise-equivalent hot path"; "no preload" becomes **stronger** (no preload needed at all) |

## 1. How each design installs its wrappers

### Late-load stub (ours)

```
Process start:
  libamdhip64 / libhsa-runtime64 / libomptarget / librccl load
    → each of them links librocprofiler-register (DT_NEEDED)
    → register's constructor runs
    → register calls dlsym(RTLD_DEFAULT, "rocprofiler_configure")
       → returns NULL (stub does NOT export it)
    → register stores the table but does NOT dlopen librocprofiler-sdk
    → api_table keeps original function pointers

Hot path pre-attach:
  runtime calls my_api(args)
    → single indirect call through api_table[Op] → original function
    → 0 wrapper frames on the stack, 0 atomic loads, 0 branches added
```

**Claim:** hot-path cost is the same as baseline. Measured (20 × 1M iters): baseline 3.604 ± 0.23 ns, stub-loaded stub variants 3.42–3.54 ± 0.04–0.16 ns, all within the 95% two-sample margin of the baseline.

### rocprofiler-sdk-shim (Jonathan's)

```
Process start:
  libamdhip64 / libhsa-runtime64 / libomptarget / librccl load
    → each of them links librocprofiler-register
    → register's constructor runs
    → runtime calls rocprofiler_register_library_api_table(name, table, count)
    → register ALWAYS dlopens librocprofiler-sdk-shim.so
    → shim receives the table and rewrites every entry to point at a wrapper:

        functor(args...) {
            auto orig = original_functor;             // const, set at install
            auto prof = atomic_load(&profiler_functor); // mutable, flipped at attach
            if (prof) return prof(orig, args...);
            else      return orig(args...);
        }

Hot path pre-attach:
  runtime calls my_api(args)
    → indirect call into shim's functor
    → one atomic acquire-load of profiler_functor
    → well-predicted branch (predicted not-taken every call)
    → tail call to original
```

**Claim:** the atomic-load + branch is "indistinguishable from noise". Plausible on modern OoO cores — the load hits L1, the branch is monotonically not-taken so the predictor handles it for free, and the tail call fuses with the surrounding frame. Empirically it is at most a couple of cycles; Jonathan backs it up with prior `disabled-sdk-contexts` measurements from rocprofiler-sdk where the equivalent "check if a context is active, none are" path has never surfaced above noise.

The two designs trade a hard 0 for an argued-noise-equivalent cost in exchange for getting rid of `LD_PRELOAD`.

## 2. Attach and detach

| Step | Late-load stub | Shim |
|---|---|---|
| Rendezvous | Stub-bound IPC (see §4); controller sends `CMD_CONFIGURE` | Shim-bound IPC (same survey applies); controller sends a "set profiler_functor" message |
| Turning tracing on | `dlopen` tool library → `rocprofiler_force_configure` → SDK initializes → `update_table()` rewrites api_table with real wrappers for selected operations | Atomic store of a function pointer into `profiler_functor[Op]` |
| Measured cost | **~1.8 ms** (dominated by `dlopen` + SDK init) | **≈ single atomic store** — submicrosecond |
| Turning tracing off | `rocprofiler_stop_context` — wrappers stay installed, Level-2 noop path (~50-200 ns with real SDK) | Atomic store of `NULL` back into `profiler_functor[Op]` — back to noise-floor cost |
| Unloading on detach | Not attempted (SDK init is one-shot; SDK stays loaded till process exit) | Not needed — shim is always loaded; swap back to null |

Jonathan's design completely dominates on attach latency — it's three orders of magnitude faster because there is no `dlopen` on the critical path. Our 1.8 ms is fine for a human-initiated attach, but for anything programmatic (e.g. "trace this one kernel launch") his is the right shape.

## 3. Coexistence of in-process rocprofiler-sdk and OOP tools

Our late-load design has a structural problem here: if the user is already running a rocprofv3 session (in-process SDK tool loaded), the SDK's `update_table()` has already rewritten every entry to point at SDK wrappers, and `init_status != 0`, so `rocprofiler_force_configure` will return `ROCPROFILER_STATUS_ERROR_CONFIGURATION_LOCKED`. Our late-load mechanism cannot attach in that case. The workaround is "don't combine" or "use ptrace attach" — which is the path we were trying to eliminate.

Jonathan's shim fixes this by making the layering explicit:

```
runtime  →  shim.functor  →  (if in-process SDK)  SDK.functor  →  original
                         ↓
                  if profiler_functor non-null,
                  profiler_functor(orig, args...)  →  OOP ring buffer
```

`update_table()` is still rocprofiler-sdk's way of wrapping every entry, but the "original" it saves is now the shim's `functor` instead of the runtime's raw function pointer. The shim's `profiler_functor` pointer is independent of anything the SDK does. This gives natural coexistence: in-process and OOP tools observe the same call at different layers and don't interfere.

This is a real architectural advantage. We'd have to do meaningful extra work in our design to approximate it.

## 4. IPC control channel — memfd+sock applies to both

The four-channel survey in this directory (mmap / socket / memfd / signal) is a transport-layer analysis; it is orthogonal to the functor-installation model. Both designs need the same rendezvous, authenticated peer verification, and command/data paths. Our analysis recommended the **socket + memfd hybrid** (see [MEMFD.md](MEMFD.md)) because it combines:

- Abstract Unix socket for the one-time authenticated rendezvous (`SO_PEERCRED`, `SCM_RIGHTS`)
- Anonymous `memfd_create` for fast shared-memory command/response + ring buffer
- `F_SEAL_SHRINK | F_SEAL_GROW` for integrity of the shared region after handoff
- Zero persistent filesystem entries (see [MEMFD.md § What "no filesystem footprint" precisely means](MEMFD.md#what-no-filesystem-footprint-precisely-means))

Here is how the hybrid plugs into **both** designs:

### Our late-load design (today's mock)

```
In-process:                                     Controller:
  stub constructor:                             rocp_ctrl connect to \0rocprofiler_<pid>
    bind \0rocprofiler_<pid>                        → SO_PEERCRED verified
    memfd_create(rocp-ctrl)                         → recvmsg(SCM_RIGHTS) memfd
    ftruncate + mmap + seal                         → mmap memfd
  on CMD_CONFIGURE (via memfd version bump):    write config + CMD_CONFIGURE, bump version
    dlopen libmock_sdk_tool_*.so                poll until ctrl->context_active == 1
    rocprofiler_force_configure(tool)
    SDK inits, update_table installs wrappers
```

### Jonathan's shim design (hypothetical)

```
In-process:                                     External shim tool:
  shim constructor (called by register):        connect to \0rocprofiler-shim_<pid>
    bind \0rocprofiler-shim_<pid>                   → SO_PEERCRED verified
    memfd_create(shim-ring)                         → recvmsg(SCM_RIGHTS) memfd
      layout:                                       → mmap memfd, read ring layout
        - header (magic, version, watermark)    negotiate enabled operations
        - profiler_functor[N_OPS] slots         write into profiler_functor[Op] slots
        - begin/end record ring buffer          poll ring buffer (or eventfd
      F_ADD_SEALS F_SEAL_SHRINK | F_SEAL_GROW     → watermark kick)
    register wraps every table entry with       process records, stream to OOP
      shim.functor
  hot path:
    runtime → shim.functor →
      if profiler_functor[Op]:
        write begin record to ring
        orig(args...)
        write end record to ring
        if ring crosses watermark,
          eventfd_write to wake external
      else: tail-call orig
```

The memfd is even better fit for the shim than for our stub because the shim is emitting *records*, not just receiving *commands*. A sealed, known-size ring buffer with a shared watermark and an eventfd-on-kick is a textbook in-process-to-out-of-process producer/consumer pipeline. The abstract socket's job is still one-shot authentication + fd handoff; after that, all data flow is shared-memory + a single eventfd wake.

**Practical consequence:** most of what we wrote in [CONTROL_CHANNEL_SURVEY.md](CONTROL_CHANNEL_SURVEY.md) and [MEMFD.md](MEMFD.md) — the elimination rationale for the 9 rejected mechanisms, the canonical protocol shape, the `SO_PEERCRED` + `SCM_RIGHTS` + sealing argument, the security/complexity tables — ports over to his design almost word-for-word. The layer that changes is not the transport.

## 5. OpenMP / OMPT — same asymmetry, same fix

OMPT is the odd one out in both designs because **it is not a dispatch table** — it's a one-shot callback registration driven by `dlsym(RTLD_DEFAULT, "ompt_start_tool")` at OpenMP init (see [CONTROL_CHANNEL_SURVEY.md § OpenMP / OMPT](CONTROL_CHANNEL_SURVEY.md#openmp--ompt--a-different-registration-path) for the full mechanism).

Both designs need to solve the same problem in the same way:

- The shim (Jonathan's) or the stub (ours) must export `ompt_start_tool`.
- `initialize(lookup, ...)` must save the `ompt_set_callback` function pointer for later use.
- No callbacks are installed at OpenMP init (OMPT hot path stays at runtime-native cost).
- At controller attach, whichever library owns the OOP side uses the saved `ompt_set_callback` pointer to install real OMPT callbacks on the live runtime.

Neither design has a natural advantage here — this is a property of the OMPT spec, not of either wrapping model. Our doc already captures the mechanism; a shim implementation would reuse the same pattern.

## 6. ABI contract — a cost Jonathan's design adds

With our design, the only ABI the stub depends on is the rocprofiler-sdk public C API (`rocprofiler_force_configure`, `rocprofiler_configure_*`, `rocprofiler_start_context`, etc.). That's already a published, versioned contract.

With Jonathan's design, there is a **new** ABI between rocprofiler-sdk-shim and rocprofiler-sdk: when an in-process SDK tool is also loaded, the SDK's `update_table()` has to know how to wrap the shim's `functor` (not the raw runtime function), and when it calls through, the shim's `functor` expects to be called with `orig` bound to the SDK's wrapper. That contract — what the shim stores as `original_functor`, how the SDK signals "wrap me, don't wrap my original" — needs to be defined, documented, and versioned alongside the rocprofiler-sdk releases.

This is a perfectly reasonable cost to pay for the architectural cleanness, but it is a real cost that our design avoids.

## 7. What rules get broken, kept, or strengthened

Rules from [CONTROL_CHANNEL_SURVEY.md § Requirements](CONTROL_CHANNEL_SURVEY.md#requirements) and our design goals:

| Rule | Ours | Jonathan's |
|---|---|---|
| No sudo, no capabilities, no kernel version requirement | ✓ | ✓ |
| Cross-user security enforceable by the kernel | ✓ (`SO_PEERCRED` via same IPC survey) | ✓ (same IPC survey applies) |
| Late attach without ptrace | ✓ (~1.8 ms) | ✓ (submicrosecond — **stronger**) |
| Multi-runtime (HIP/HSA/RCCL/OMPT) | ✓ | ✓ |
| No binary rewriting / no uprobe placement | ✓ | ✓ in default path; GOTCHA is his optional Linux-only optimization that would be a form of binary rewriting if enabled |
| Cross-architecture | ✓ | ✓ in default path |
| **0 ns hot path when no controller attached** | **Genuine 0** (statistically verified) | **Relaxed** to "one atomic load + branch, noise-equivalent" |
| **No `LD_PRELOAD` required** | **Broken** — requires `LD_PRELOAD=librocp_stub_*.so` | **Stronger** — never required |
| In-process SDK + OOP tool coexistence | Broken — `init_status` locks out `force_configure` | **Stronger** — natural layering |
| rocprofiler-sdk loaded into every OOP-profiled process | Yes, at attach | **Stronger** — never loaded for OOP-only use |
| New code to design and maintain | ~20 KiB stub per channel; reuses SDK as-is | `librocprofiler-sdk-shim.so` + external shim tool + new shim ABI |

Net: Jonathan's design relaxes "genuine 0 ns" to "noise-equivalent" and takes on the cost of designing and shipping a new library with its own ABI. In return it strengthens four other properties (no preload, no SDK for OOP, coexistence, attach cost). The IPC-channel survey we've already done applies to both with the same recommended hybrid.

## 7a. Measured: the shim fast path *is* noise-equivalent

Rather than leave "indistinguishable from noise" as a claim, this repo now ships a validation mock — `src/dispatch_tracer/shim_mock/` — that implements exactly the `atomic_load + branch + tail_call` shape against our existing `mylib_dispatch` / `sample_app_dispatch` harness. It is loaded by `mock_register` via a new `MOCK_REGISTER_LIB` env var, with **no `LD_PRELOAD`** on the sample app, mirroring the production "register always dlopens the shim" assumption. The shim rewrites `mylib_dispatch`'s api_table with wrappers of the form:

```c
void shim_wrap_op0(int a1, uint64_t a2, double a3, void* a4)
{
    void* prof = atomic_load_explicit(&g_shim_profiler[0], memory_order_acquire);
    void* orig = atomic_load_explicit(&g_shim_orig[0],     memory_order_acquire);
    if (__builtin_expect(prof == NULL, 1)) {
        ((orig_op0_t)orig)(a1, a2, a3, a4);
        return;
    }
    ((shim_prof_op0_t)prof)(orig, a1, a2, a3, a4);
}
```

`profiler_functor` stays `NULL` for the whole measurement, so each call pays exactly the fast-path cost the design claims should be noise-equivalent. Harness: `scripts/benchmark_noop_noise.py`, 20 × 1,000,000 iterations per configuration with 2 warmup runs discarded.

| Config | Mean (ns) | Stdev | 95% CI | Δ vs baseline | Distinguishable at 95%? |
|---|---:|---:|---:|---:|:---|
| Baseline (no stub, no shim)  | 3.578 | 0.339 | ±0.149 | — | — |
| stub mmap                    | 3.502 | 0.339 | ±0.149 | −0.076 ± 0.210 | no |
| stub sock                    | 3.504 | 0.365 | ±0.160 | −0.074 ± 0.218 | no |
| stub memfd                   | 3.443 | 0.018 | ±0.008 | −0.135 ± 0.149 | no |
| stub signal                  | 3.438 | 0.004 | ±0.002 | −0.139 ± 0.149 | no |
| **shim (profiler=NULL)**     | **3.483** | 0.164 | ±0.072 | **−0.095 ± 0.165** | **no** |

The shim's delta is well inside the 95% two-sample margin; its absolute mean sits between the most-active stub (mmap, 3.502) and the quietest stub (signal, 3.438). Empirically there is no cost you can measure for the `atomic-load + branch + tail-call` path on this hardware. Jonathan's claim holds on AMD EPYC 9354 at 1M-iteration sample sizes; the 0 ns story is preserved.

Raw data: `report/dispatch_noise.json` (includes per-run samples for all six configurations).

## 8. Open questions to take back to Jonathan

1. ~~**Hot-path cost validation.**~~ **Done** (see §7a): measured 3.483 ns vs baseline 3.578 ns, not distinguishable at 95%. Worth re-running on a production x86-64 and on aarch64 if that hardware is in scope, but the pattern is validated.
2. **OMPT handling.** The shim needs to export `ompt_start_tool` for the same reasons our stub does. Is that in scope for rocprofiler-sdk-shim or does it live in a separate library?
3. **ABI versioning between shim and sdk.** When both are loaded and the SDK's `update_table()` wraps shim entries, what's the versioning discipline? A mismatched pair would silently wrap at the wrong level.
4. **What does the shim expose for control?** Our control channel ships `rocp_config_t` (domain bitfields, output format, filter patterns). The shim needs an equivalent — is it per-op pointer slots plus a ring-buffer config, or something richer? This is where the IPC survey (and specifically the memfd+sock hybrid) carries over directly.
5. **GOTCHA optimization.** Listed as optional. Worth a one-line answer: is this a maintenance path we'd actually turn on, or is it only there in case the noise-equivalent claim doesn't hold on some future microarchitecture?
6. **Multi-tool coexistence on the OOP side.** The shim's `profiler_functor` is a single pointer per operation. Two concurrent OOP tools would need either daisy-chaining (tool A wraps tool B wraps original) or a multiplexing fanout layer — which one is the intended design?

## 9. Recommendation

Jonathan's design is architecturally cleaner for the OOP-profiling problem as a long-lived product. It gives up a statistically-verified 0 ns for a mathematically-tiny atomic-load-plus-branch (which he is almost certainly right is noise-equivalent), and in exchange it removes `LD_PRELOAD`, keeps rocprofiler-sdk out of every OOP-profiled process, and makes in-process + OOP coexistence natural. The IPC transport question we've already settled (memfd+sock hybrid) applies to it unchanged.

Our late-load stub is the right pattern for a **mock that validates the 0-ns claim and the control-channel security properties with no changes to rocprofiler-sdk or rocprofiler-register**. It is not the right pattern for the long-lived design, because it requires every OOP-profiled process to preload an extra `.so` and because it loads the full SDK into the process for OOP-only use.

The two designs are complementary, not competing. If the project adopts Jonathan's shim as the long-term architecture, the work in this repo — the four-channel survey, the memfd+sock hybrid, the security comparison, the OMPT late-bind mechanism — becomes the transport and OMPT layer under it. None of it is wasted; most of it is necessary regardless.
