# shim_mock — libroc-shim IPC Transport Mock

Validates the redesigned architecture where `libroc-shim.so` is a **pure IPC transport layer** — it does zero profiling logic. rocprofiler-sdk (mocked as `libmock_rocp_sdk.so`) handles all dispatch table wrapping, correlation ID generation, and buffer record creation. The shim manages ring buffers and delivers records over a socket to the consumer.

See [`docs/dispatch-tracer/SHIM_MEMFD_SOCK_DESIGN.md`](../../../docs/dispatch-tracer/SHIM_MEMFD_SOCK_DESIGN.md) for the full design.

## Architecture

```
Target Process                          Consumer Process
──────────────                          ────────────────

 mock_libA/mock_libB/mylib_dispatch
       │ (DT_NEEDED)
       ▼
 mock_register
       │ (unconditional dlopen)
       ▼
 libroc-shim.so (shim_mock.c)           libroc-shim-consumer.so
   │ dormant until attach                  │ exports rocp_* API
   │                                       │ proxies over socket
   ▼                                       ▼
 On consumer attach:                     shim_consumer_test uses
   shim proxies API calls to SDK          identical API to in-process
   SDK wraps dispatch tables              tool — just linked to
   SDK writes buffer records              consumer lib instead of SDK
   → shim ring → socket → consumer
```

## Build

```bash
cd build && cmake .. && make -j$(nproc)
```

## Artifacts

| File | Library/Binary | Role |
|------|---------------|------|
| `lib/libmock_rocp_sdk.so` | Mock SDK | Wraps tables, generates buffer records |
| `lib/libroc-shim.so` | Target-side shim | IPC transport: socket, ring buffer, API proxy |
| `lib/libroc-shim-consumer.so` | Consumer-side proxy | Exports rocp_* API, marshalls over socket |
| `bin/shim_multilib_test` | Target app | Exercises libA/libB/mylib APIs |
| `bin/shim_consumer_test` | OOP consumer | Uses standard rocp_* API to trace target |

## How to Run

```bash
# Terminal 1: start the target app (shim is dormant)
export LD_LIBRARY_PATH=$PWD/build/lib
MOCK_REGISTER_LIB=$PWD/build/lib/libroc-shim.so \
  SIMULATED_WORK_US=1000 \
  ./build/bin/shim_multilib_test 5000 --wait

# Terminal 2: attach the OOP consumer
export LD_LIBRARY_PATH=$PWD/build/lib
./build/bin/shim_consumer_test <target_pid> 5
# Then press Enter in terminal 1 to start the workload
```

Without `--wait`, the target sleeps 1 second then starts immediately:

```bash
# Automated (no interactive wait):
export LD_LIBRARY_PATH=$PWD/build/lib
MOCK_REGISTER_LIB=$PWD/build/lib/libroc-shim.so \
  SIMULATED_WORK_US=1000 \
  ./build/bin/shim_multilib_test 5000 &
TARGET_PID=$!
sleep 2
./build/bin/shim_consumer_test $TARGET_PID 3
```

## Source Map

| File | Purpose |
|------|---------|
| `shim_protocol.h` | IPC protocol: message framing, API IDs, record types, ring header |
| `mock_rocp_sdk.h/c` | Mock rocprofiler-sdk: force_configure, buffer tracing, table wrapping |
| `shim_mock.c` | `libroc-shim.so`: socket listener, API proxy, ring management |
| `shim_consumer_lib.c` | `libroc-shim-consumer.so`: API proxy over socket |
| `shim_consumer_test.c` | Consumer test app using standard rocp_* API |
| `shim_ipc.h/c` | Low-level IPC: message framing, ring buffer, SCM_RIGHTS |
| `mock_libA.h/c` | HSA/ROCR analogue (4 ops) |
| `mock_libB.h/c` | HIP analogue (4 ops, calls libA) |
| `shim_multilib_test.c` | Target app exercising cross-library calls |
