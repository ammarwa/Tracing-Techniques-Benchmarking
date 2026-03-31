# bpftime: Userspace eBPF Runtime Design Documentation

## Overview

[bpftime](https://github.com/eunomia-bpf/bpftime) is a userspace eBPF runtime that replaces kernel uprobes with Frida-gum based binary rewriting. All tracing happens in userspace -- no kernel context switches, no root required.

With kernel uprobes, each traced function call triggers:
1. INT3 breakpoint exception
2. Trap to kernel mode (context switch)
3. BPF program execution in kernel
4. Return to user mode (context switch)

bpftime eliminates this by performing all instrumentation and BPF execution in userspace.

## Benchmark Results

Measured on AMD EPYC 9354 bare metal. bpftime measured in Docker with LLVM 16 JIT (10 runs per scenario):

| Scenario | Kernel eBPF Overhead | bpftime JIT Overhead | Winner |
|---|---|---|---|
| Empty (0 μs) | ~26 μs | ~6 μs | **bpftime 4.5x faster** |
| 5 μs Function | ~4 μs | ~6 μs | Kernel eBPF 1.6x faster |
| 50 μs Function | ~17 μs | ~10 μs | **bpftime 1.7x faster** |
| 100 μs Function | ~20 μs | ~11 μs | **bpftime 1.8x faster** |
| 500 μs Function | ~17 μs | ~13 μs | **bpftime 1.4x faster** |
| 1000 μs Function | ~25 μs | ~15 μs | **bpftime 1.7x faster** |

**Key finding**: With LLVM 16 JIT, bpftime achieves **~6-15 μs overhead per call**, which is **comparable to or faster than** kernel eBPF (~4-26 μs) for most scenarios. bpftime runs purely in userspace with no root required.

**Note**: bpftime was measured in Docker (Ubuntu 24.04 with LLVM 16) while kernel eBPF was measured directly on the host (Ubuntu 22.04). The overhead values are computed relative to each environment's own baseline.

## Architecture

bpftime uses an **LD_PRELOAD mechanism** with two components:

- **syscall-server** (`libbpftime-syscall-server.so`): Loaded into the tracer process via `LD_PRELOAD`. Intercepts BPF syscalls (`bpf()`, `perf_event_open()`) and redirects them to the userspace runtime.
- **agent** (`libbpftime-agent.so`): Loaded into the target application via `LD_PRELOAD`. Injects Frida-gum based hooks into the target function's prologue via code patching.

```
+--------------------------------------------------------------+
|                      User Space (only)                        |
|                                                               |
|  +------------------+          +-------------------------+    |
|  |  Target App      |          |  bpftime_tracer          |   |
|  |  (sample_app)    |          |  (with syscall server)   |   |
|  |                  |          |                          |   |
|  |  LD_PRELOAD:     |          |  LD_PRELOAD:             |   |
|  |  libbpftime-     |          |  libbpftime-syscall-     |   |
|  |  agent.so        |          |  server.so               |   |
|  |                  |          |                          |   |
|  |  libmylib.so     |  shm    |  - Userspace BPF VM      |   |
|  |  | (rewritten)   | ------> |  - Ring buffer polling   |   |
|  |  my_traced_func()|          |  - Event buffering       |   |
|  +------------------+          +-------------------------+    |
|                                                               |
|  No kernel interaction for tracing!                           |
+--------------------------------------------------------------+
```

The same BPF program/skeleton (`mylib_tracer.bpf.c`) is used for both kernel eBPF and bpftime -- no code changes needed.

## Critical: LLVM Version Requirement

**bpftime requires LLVM 16.** LLVM 17 has a known symbol resolution bug (`llvm_orc_registerEHFrameSectionWrapper` not found) that prevents the JIT from working. See [bpftime issue #424](https://github.com/eunomia-bpf/bpftime/issues/424).

This affects:
- The official Docker image (`ghcr.io/eunomia-bpf/bpftime:latest`) which ships with LLVM 17 -- **their own examples fail**
- Building from source on systems with LLVM 17 as default

The fix: install LLVM 16 and point cmake to it with `-DLLVM_DIR=/usr/lib/llvm-16/cmake`.

## Critical: Compilation Requirements

The target library (`libmylib.so`) **MUST** be compiled with `-fno-omit-frame-pointer`.

Without frame pointers, Frida's instruction relocator generates invalid trampolines when hooking at function prologues, causing SIGTRAP crashes. This was discovered through debugging:

- Hooking at offset 0x0 worked (noop/library base)
- Hooking at the actual function offset (0x1120) crashed with SIGTRAP
- Adding `-fno-omit-frame-pointer` resolved the issue by providing a standard `push %rbp; mov %rsp, %rbp` prologue that Frida can safely relocate

In CMake:
```cmake
target_compile_options(mylib PRIVATE -fno-omit-frame-pointer)
```

## Installation

### Docker (Recommended)

A `Dockerfile.bpftime` is provided in the repository:

```bash
docker build -t bpftime-bench -f Dockerfile.bpftime .
docker run --rm --privileged -v $(pwd):/workspace bpftime-bench
```

This builds bpftime with LLVM 16 JIT from the official bpftime image.

### From Source

```bash
# Prerequisites
sudo apt-get install -y cmake build-essential clang llvm \
    libbpf-dev libelf-dev zlib1g-dev libboost-dev libyaml-cpp-dev \
    llvm-16-dev  # MUST be LLVM 16, not 17

# Clone
git clone --depth 1 https://github.com/eunomia-bpf/bpftime.git /tmp/bpftime
cd /tmp/bpftime && git submodule update --init --recursive

# Build with LLVM 16 JIT
cmake -Bbuild \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBPFTIME_LLVM_JIT=ON \
    -DBUILD_BPFTIME_DAEMON=0 \
    -DLLVM_DIR=/usr/lib/llvm-16/cmake
cmake --build build --parallel $(nproc) --target all install

# Libraries install to ~/.bpftime/
```

**Note on submodules**: If `git submodule update` fails with "directory already exists", remove the stale directory first:
```bash
rm -rf third_party/bpftool/libbpf
git submodule update --init --recursive
```

### Without JIT (Fallback)

If LLVM 16 is unavailable, build without JIT. This uses the BPF interpreter (~2x slower):

```bash
cmake -Bbuild -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBPFTIME_LLVM_JIT=OFF -DBUILD_BPFTIME_DAEMON=0
cmake --build build --target bpftime-agent --target bpftime-syscall-server -j$(nproc)
mkdir -p ~/.bpftime
cp build/runtime/agent/libbpftime-agent.so ~/.bpftime/
cp build/runtime/syscall-server/libbpftime-syscall-server.so ~/.bpftime/
```

## Running

### Terminal 1: Start the Tracer

```bash
LD_PRELOAD=~/.bpftime/libbpftime-syscall-server.so \
    ./build/bin/bpftime_tracer -l ./build/lib/libmylib.so
```

### Terminal 2: Run the Target App

```bash
LD_PRELOAD=~/.bpftime/libbpftime-agent.so \
    SIMULATED_WORK_US=100 ./build/bin/sample_app 10000
```

No root/sudo is required.

## Shared Memory Management

bpftime uses POSIX shared memory (`/dev/shm/bpftime_maps_shm`) for IPC.

**Important**: Stale shm from previous runs MUST be cleaned before starting a new session:
```bash
sudo rm -f /dev/shm/bpftime*
```

When running under `sudo`, shm files are owned by root. Subsequent non-root runs fail with "Permission denied". The benchmark script cleans shm automatically between runs.

## Verified Userspace Operation

Verified via strace and kernel debugging interfaces:

| Check | Result |
|---|---|
| `/sys/kernel/debug/tracing/uprobe_events` | No entries |
| `bpftool prog list` | No BPF programs loaded |
| `strace` for `bpf()` syscalls | Zero calls |
| `strace` for `perf_event_open()` syscalls | Zero calls |
| Root/sudo required | No |

## Observations and Findings

### 1. bpftime With LLVM JIT Is Comparable To Kernel eBPF

With LLVM 16 JIT in Docker, bpftime achieves **~6-15 μs overhead per call**, which is comparable to kernel eBPF's ~4-26 μs. For most function durations (50 μs+), bpftime is actually **1.4-1.8x faster** than kernel eBPF. The constant overhead for empty functions is ~6 μs (vs kernel eBPF's ~26 μs on the host).

The improvement comes from eliminating the kernel context switch (INT3 trap + return). However, the exact comparison depends on the environment — kernel eBPF overhead varies between host and Docker.

### 2. Interpreter Mode Is Much Slower

Without LLVM JIT (`-DBPFTIME_LLVM_JIT=OFF`), bpftime uses the ubpf interpreter:
- Interpreter mode: ~35-47 μs overhead per call (~4-8x slower than kernel eBPF)
- System CPU time dominates, suggesting the interpreter makes kernel calls despite being "userspace"

### 3. LLVM 17 Incompatibility

bpftime's LLVM JIT is broken with LLVM 17. The `llvm_orc_registerEHFrameSectionWrapper` symbol is missing, causing the agent to crash on startup. This affects:
- The official Docker image
- Any build using LLVM 17
- The fix is to use LLVM 16

### 4. Ring Buffer Event Delivery

Ring buffer events are captured successfully in Docker with LLVM 16 JIT (20,000 events for 10,000 iterations = entry + exit). On the host with interpreter mode, events were not delivered (0 captured), though tracing overhead was still measurable.

### 5. Frame Pointer Dependency

Without `-fno-omit-frame-pointer` on the target library, Frida's instruction relocator generates invalid trampolines. This is a fundamental requirement for bpftime's Frida-based hooking approach.

## Environment Variables

| Variable | Effect |
|---|---|
| `BPFTIME_DISABLE_JIT` | Set to `1` to force interpreter mode |
| `BPFTIME_VM_NAME` | Select VM backend (`llvm`, `ubpf`) |
| `BPFTIME_SHM_MEMORY_MB` | Shared memory size in megabytes |
| `SPDLOG_LEVEL` | Log level (`debug`, `info`, `warn`, `error`) |

## Trade-offs vs Kernel Uprobes

| Aspect | Kernel eBPF (uprobes) | bpftime (userspace) |
|--------|----------------------|---------------------|
| **Overhead per call** | ~4-26 μs | ~6-15 μs (JIT), ~35-47 μs (interpreter) |
| **Root required** | Yes (CAP_SYS_ADMIN) | No |
| **Kernel visibility** | Full kernel context | No kernel visibility |
| **Attach to running process** | Yes | No (requires LD_PRELOAD) |
| **BPF program** | Runs in kernel JIT | Runs in userspace JIT/interpreter |
| **Target modification** | None | LD_PRELOAD + frame pointers required |
| **LLVM version** | N/A | Must be LLVM 16 (not 17) |

## Known Limitations

1. **LLVM 17 incompatible**: JIT crashes with missing ORC symbols. Use LLVM 16.
2. **Requires `-fno-omit-frame-pointer`**: Target libraries need standard prologues for Frida hooks.
3. **Comparable to kernel eBPF with JIT**: ~6-15 μs vs ~4-26 μs. Faster for most scenarios, but not the 10x improvement claimed.
4. **No kernel visibility**: Cannot trace kernel functions.
5. **LD_PRELOAD required**: Cannot attach to already-running processes.
6. **Shared memory cleanup**: Stale shm causes failures; must clean between runs.

## References

- [bpftime GitHub Repository](https://github.com/eunomia-bpf/bpftime)
- [bpftime Issue #424: LLVM 17 Symbol Bug](https://github.com/eunomia-bpf/bpftime/issues/424)
- [Frida-gum (binary instrumentation)](https://frida.re/)
- [BPF Documentation](https://www.kernel.org/doc/html/latest/bpf/index.html)
