# Shim Mock — Validation of the rocprofiler-sdk-shim Design

This directory implements a working mock of the shim architecture described in [`docs/dispatch-tracer/SHIM_MEMFD_SOCK_DESIGN.md`](../../../docs/dispatch-tracer/SHIM_MEMFD_SOCK_DESIGN.md). It validates every design mechanism except OMPT across two separate processes.

## Build Artifacts

| Artifact | Type | Purpose |
|---|---|---|
| `build/lib/libshim_mock.so` | Shared library | The shim itself — loaded into the target process via `MOCK_REGISTER_LIB`. Wraps dispatch tables, creates memfd+socket IPC, writes records to a ring buffer. |
| `build/bin/shim_consumer_test` | Binary | External OOP consumer — attaches to a running target by PID, enables tracing, drains the ring, prints records with function names + typed arguments. |
| `build/bin/shim_multilib_test` | Binary | Test app that links `mock_libA` (HSA-level) and `mock_libB` (HIP-level). libB calls into libA, creating cross-library call chains for correlation-ID testing. |
| `build/lib/libmock_libA.so` | Shared library | Mock low-level runtime (HSA/ROCR analogue) with complex struct args: queue handles, dispatch packets, memory regions, signals. Registers as `libA_hsa`. |
| `build/lib/libmock_libB.so` | Shared library | Mock high-level runtime (HIP analogue) with dim3, launch configs, device properties, streams. Registers as `libB_hip`. Calls into libA internally. |

## How to Build

```bash
cd ~/work/eBPF/Tracing-Techniques-Benchmarking
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc) --target shim_mock shim_consumer_test shim_multilib_test mock_libA mock_libB
```

## How to Run

### 1. Basic single-library test (mylib)

```bash
# Terminal 1 — launch target with shim (1ms work per call, 10000 iterations)
MOCK_REGISTER_LIB=$PWD/build/lib/libshim_mock.so \
  LD_LIBRARY_PATH=$PWD/build/lib \
  SIMULATED_WORK_US=1000 \
  build/bin/sample_app_dispatch 10000 &
echo "target pid=$!"

# Terminal 2 — attach OOP consumer for 5 seconds
build/bin/shim_consumer_test $! 5
```

Expected output:
```
=== Attached to pid=12345, 1 registrations, 2 total_ops ===
  table[0]: name="mylib" instance=0 v1.0 slots=[0..2)
=== Enabled 2 ops, mode=RECORD ===
[tsc=...] ENTER  my_traced_function  tid=12345  corr={i=1 e=0 a=0}  args(a1=42, a2=3735928559, a3=3.1, a4=0x12345678)
[tsc=...] EXIT   my_traced_function  tid=12345  corr={i=1 e=0 a=0}
...
=== Stats: traced=4993 dropped=0 consumer_read=4993 ===
=== Detached ===
```

### 2. Multi-library test (libA + libB + mylib)

```bash
# Terminal 1 — launch multilib test with shim (10M iterations to stay alive)
MOCK_REGISTER_LIB=$PWD/build/lib/libshim_mock.so \
  LD_LIBRARY_PATH=$PWD/build/lib \
  build/bin/shim_multilib_test 10000000 &
echo "target pid=$!"

# Terminal 2 — attach consumer
build/bin/shim_consumer_test $! 3
```

Expected output shows 3 registered tables:
```
=== Attached to pid=12345, 3 registrations, 10 total_ops ===
  table[0]: name="mylib"    instance=0 v1.0 slots=[0..2)
  table[1]: name="libA_hsa" instance=0 v1.0 slots=[2..6)
  table[2]: name="libB_hip" instance=0 v1.0 slots=[6..10)
=== Enabled 10 ops, mode=RECORD ===
```

> **Note:** The mock only installs typed wrappers for `mylib` (2 ops). `libA_hsa` and `libB_hip` tables are registered for metadata visibility but do not emit records — the real shim's code generator produces per-op typed wrappers for every table. The multi-library registration, per-table slot ranges, and cross-library dependency (libB → libA) are all validated.

### 3. Fast-path benchmark (no consumer, no tracing)

```bash
MOCK_REGISTER_LIB=$PWD/build/lib/libshim_mock.so \
  LD_LIBRARY_PATH=$PWD/build/lib \
  build/bin/sample_app_dispatch 1000000
# → "Average time per call: 3.70 nanoseconds"
```

### 4. Noise-floor measurement (statistical, 20 runs × 1M iterations)

```bash
python3 -u scripts/benchmark_noop_noise.py --iters 1000000 --runs 20
```

### 5. Crash recovery test

```bash
MOCK_REGISTER_LIB=$PWD/build/lib/libshim_mock.so \
  LD_LIBRARY_PATH=$PWD/build/lib \
  SIMULATED_WORK_US=10000 \
  build/bin/sample_app_dispatch 1000 &
APP=$!
sleep 0.5

# Attach consumer briefly
build/bin/shim_consumer_test $APP 1 &
CON=$!
sleep 0.5

# Kill consumer (simulate crash)
kill -9 $CON

# Target should continue running; stderr shows:
# "[shim] consumer disconnected, slots zeroed"
wait $APP
```

### 6. Ring size override

```bash
ROCP_SHIM_RING_SIZE_MB=16 \
  MOCK_REGISTER_LIB=$PWD/build/lib/libshim_mock.so \
  LD_LIBRARY_PATH=$PWD/build/lib \
  SIMULATED_WORK_US=100 \
  build/bin/sample_app_dispatch 100000 &
build/bin/shim_consumer_test $! 3
```

## Source Files

| File | Purpose |
|---|---|
| `shim_protocol.h` | Shared types: memfd header (`shim_ctrl_t`), record format (`shim_record_t`), mode selectors, per-table registration, filter bitmap helpers, value-filter rules, ring-buffer header |
| `shim_ipc.h` / `shim_ipc.c` | IPC layer: target-side memfd+socket+ring+eventfd setup, bg thread with SO_PEERCRED + SCM_RIGHTS + POLLHUP liveness; consumer-side attach/detach/poll |
| `shim_mock.c` | The shim library: wrappers for mylib, correlation stack, mode-selector hot path, filter chain, IPC initialization, per-table registration |
| `shim_consumer_test.c` | Consumer binary: attach → enable ops → poll ring → print records with function names + typed args → detach |
| `mock_libA.h` / `mock_libA.c` | HSA/ROCR analogue: 4 ops with dispatch packets, memory regions, queue/signal/agent handles |
| `mock_libB.h` / `mock_libB.c` | HIP analogue: 4 ops with dim3, launch configs, device properties, streams. Calls into libA. |
| `shim_multilib_test.c` | Test app exercising libB → libA cross-library chains |
| `CMakeLists.txt` | Build rules for all targets |

## What This Validates

- [x] memfd_create + mmap + F_SEAL_SHRINK|GROW|SEAL (§5)
- [x] Abstract socket with SO_PEERCRED authentication (§6, §12)
- [x] SCM_RIGHTS fd handoff for memfd + eventfd (§6.2)
- [x] SPSC ring buffer with eventfd watermark wake (§11)
- [x] Integer mode selectors, NOT cross-process function pointers (§5.1, P0 fix)
- [x] Record emission with function names + typed arg payloads (§7, §7B.1)
- [x] Thread-local correlation ID stack: internal/external/ancestor (§7A)
- [x] External correlation push/pop API (§7A.3)
- [x] POLLHUP-based consumer crash recovery (§10.4)
- [x] Per-table registration with lib_instance dimension (§13.6)
- [x] Atomic name-filter bitmap (§13.5 Phase 1)
- [x] Declarative value-filter rules (§13.5 Phase 3)
- [x] ROCP_SHIM_RING_SIZE_MB env var (§16)
- [x] Multi-library registration (libA + libB + mylib)
- [x] Complex struct args: dim3, dispatch packets, memory regions, device properties
- [x] Cross-library dependency (libB calls libA through dispatch tables)
- [ ] Per-op typed wrappers for libA/libB (requires code generator — real shim only)
- [ ] Cross-library ancestor chain in ring records (depends on above)
- [ ] OMPT integration (excluded per request)
- [ ] Variable-size auxiliary ring for strings (§7B.2)
