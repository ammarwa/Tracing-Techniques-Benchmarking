# bpftime: Userspace eBPF Runtime Design Documentation

## Overview

[bpftime](https://github.com/eunomia-bpf/bpftime) is a userspace eBPF runtime that replaces kernel uprobes with Frida-gum based binary rewriting. All tracing happens in userspace -- no kernel context switches, no root required.

With kernel uprobes, each traced function call triggers:
1. INT3 breakpoint exception
2. Trap to kernel mode (context switch)
3. BPF program execution in kernel
4. Return to user mode (context switch)

This context switch pair accounts for most of the ~5 us overhead per call. bpftime eliminates this entirely by performing all instrumentation and BPF execution in userspace.

## Architecture

bpftime uses an **LD_PRELOAD mechanism** with two components:

- **syscall-server** (`libbpftime-syscall-server.so`): Loaded into the tracer process via `LD_PRELOAD`. Intercepts BPF syscalls (`bpf()`, `perf_event_open()`) from the tracer and redirects them to the userspace runtime instead of the kernel.
- **agent** (`libbpftime-agent.so`): Loaded into the target application via `LD_PRELOAD`. Injects Frida-gum based hooks into the target app's function prologues via code patching.

Key architectural details:
- Uses the **same BPF program/skeleton** as the kernel eBPF tracer (`mylib_tracer.bpf.c`) -- no code changes needed
- **Frida-gum** hooks function prologues via binary code patching (rewriting the first few instructions to redirect to a trampoline)
- **Shared memory (POSIX shm)** is used for inter-process communication between the syscall-server (tracer) and agent (target app)

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
|  |  libmylib.so     |          |  - Userspace BPF VM      |   |
|  |  | (rewritten)   | ------> |  - Ring buffer polling   |   |
|  |  my_traced_func()|          |  - Event buffering       |   |
|  +------------------+          +-------------------------+   |
|                                                               |
|  No kernel interaction for tracing!                           |
+--------------------------------------------------------------+
```

## Critical: Compilation Requirements

**This is the most important section.** The target library (`libmylib.so`) **MUST** be compiled with `-fno-omit-frame-pointer`.

### Why This Is Required

Without frame pointers, Frida's instruction relocator generates **invalid trampolines** when hooking at function prologues, causing **SIGTRAP crashes**.

Here is what happens under the hood:

1. Frida's `gum_interceptor_attach` needs to save and relocate the first few instructions of the hooked function. These instructions are overwritten with a jump to the trampoline, and the original instructions are placed in a "relocated prologue" that executes before jumping back.

2. With `-fno-omit-frame-pointer`, the function prologue follows the standard pattern:
   ```asm
   push %rbp
   mov %rsp, %rbp
   ```
   These instructions are position-independent and can be safely relocated.

3. **Without** frame pointers (the default for optimized builds), the compiler may emit a different prologue that is not safely relocatable. The relocated instruction sequence can be invalid, leading to crashes.

### How This Was Discovered

This was found through extensive debugging:
- Hooking at **offset 0x0** worked (it is effectively a noop/library base)
- Hooking at **offset 0x1120** (the actual function) crashed with SIGTRAP
- The crash was traced to the function prologue format -- Frida could not safely relocate the non-standard prologue instructions
- Adding `-fno-omit-frame-pointer` to the library's compilation flags resolved the issue completely

### Build Flag

In the project's CMake, ensure the target library is compiled with:
```cmake
target_compile_options(mylib PRIVATE -fno-omit-frame-pointer)
```

Or if compiling manually:
```bash
gcc -shared -fPIC -fno-omit-frame-pointer -o libmylib.so mylib.c
```

## LLVM JIT vs Interpreter

bpftime supports two BPF execution modes:

### LLVM JIT (`-DBPFTIME_LLVM_JIT=ON`)

Compiles BPF bytecode to native x86 machine code via LLVM at runtime. Approximately **3x faster** than the interpreter. This is the recommended mode for benchmarking.

### Interpreter (`-DBPFTIME_LLVM_JIT=OFF`)

Interprets BPF bytecode instruction by instruction. Slower but more portable and easier to debug.

### Build Considerations

- The **LLVM JIT full build** has a libbpf linker error when building the entire project. The workaround is to build only the `bpftime-agent` and `bpftime-syscall-server` targets individually, not the full project.
- Set `BPFTIME_DISABLE_JIT=1` environment variable to force interpreter mode even when using a JIT-enabled build.
- Set `BPFTIME_VM_NAME=ubpf` to use the alternative uBPF JIT backend instead of LLVM.

## Building bpftime

### Clone and Initialize

```bash
git clone --depth 1 https://github.com/eunomia-bpf/bpftime.git /tmp/bpftime
cd /tmp/bpftime && git submodule update --init --recursive
```

### With LLVM JIT (Recommended)

```bash
cmake -Bbuild -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBPFTIME_LLVM_JIT=ON -DBUILD_BPFTIME_DAEMON=0
cmake --build build --target bpftime-agent --target bpftime-syscall-server -j$(nproc)
mkdir -p ~/.bpftime
cp build/runtime/agent/libbpftime-agent.so ~/.bpftime/
cp build/runtime/syscall-server/libbpftime-syscall-server.so ~/.bpftime/
```

Note: Building individual targets (`--target bpftime-agent --target bpftime-syscall-server`) avoids the libbpf linker error that occurs with a full project build.

### Without JIT (Fallback)

```bash
cmake -Bbuild -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBPFTIME_LLVM_JIT=OFF -DBUILD_BPFTIME_DAEMON=0
cmake --build build --target bpftime-agent --target bpftime-syscall-server -j$(nproc)
mkdir -p ~/.bpftime
cp build/runtime/agent/libbpftime-agent.so ~/.bpftime/
cp build/runtime/syscall-server/libbpftime-syscall-server.so ~/.bpftime/
```

## Running

### Terminal 1: Start the Tracer with syscall-server

```bash
LD_PRELOAD=~/.bpftime/libbpftime-syscall-server.so ./build/bin/bpftime_tracer -l ./build/lib/libmylib.so
```

The syscall-server intercepts the BPF syscalls from the tracer binary and sets up the userspace BPF runtime. The tracer then waits for events, just as it would with kernel eBPF.

### Terminal 2: Run the Target App with agent

```bash
LD_PRELOAD=~/.bpftime/libbpftime-agent.so SIMULATED_WORK_US=100 ./build/bin/sample_app 10000
```

The agent connects to the shared memory region created by the syscall-server, reads the hook configuration, and uses Frida-gum to patch the target function's prologue. When `my_traced_function()` is called, execution is redirected to the userspace BPF program.

No root/sudo is required -- all tracing happens in userspace.

## Shared Memory Management

bpftime uses **POSIX shared memory** for inter-process communication between the tracer (syscall-server) and target app (agent).

- Shared memory location: `/dev/shm/bpftime_maps_shm`
- **Stale shm from previous runs MUST be cleaned** before starting a new session:
  ```bash
  sudo rm -f /dev/shm/bpftime*
  ```
- When running under `sudo`, the shm files are owned by root. Subsequent non-root runs will fail with "Permission denied" because they cannot open the root-owned shm.
- The benchmark script (`scripts/run_benchmarks.sh` or equivalent) cleans shm automatically between runs.

## Verified Userspace Operation

The following checks confirm that bpftime runs purely in userspace with zero kernel involvement:

| Check | Result |
|---|---|
| `/sys/kernel/debug/tracing/uprobe_events` | No entries |
| `bpftool prog list` | No BPF programs loaded |
| `strace` for `bpf()` syscalls | Zero calls |
| `strace` for `perf_event_open()` syscalls | Zero calls |
| Root/sudo required | No |

This confirms that bpftime's tracing is entirely userspace-based, with no kernel uprobes, no BPF program loading into the kernel, and no perf event registration.

## Environment Variables

| Variable | Effect |
|---|---|
| `BPFTIME_DISABLE_JIT` | Set to `1` to force interpreter mode even with JIT build |
| `BPFTIME_VM_NAME` | Select VM backend (`llvm`, `ubpf`) |
| `BPFTIME_SHM_MEMORY_MB` | Configure shared memory size in megabytes |
| `SPDLOG_LEVEL` | Log level for bpftime internals (`debug`, `info`, `warn`, `error`) |

## Trade-offs vs Kernel Uprobes

| Aspect | Kernel eBPF (uprobes) | bpftime (userspace) |
|--------|----------------------|---------------------|
| **Overhead** | ~5 us/call | ~0.5 us/call (~10x reduction) |
| **Root required** | Yes (CAP_SYS_ADMIN) | No |
| **Kernel visibility** | Full kernel context | No kernel visibility |
| **Attach to running process** | Yes | No (requires LD_PRELOAD at launch) |
| **BPF program** | Runs in kernel VM | Runs in userspace VM |
| **Target modification** | None | LD_PRELOAD on target app |
| **Frame pointer requirement** | No | Yes (`-fno-omit-frame-pointer`) |

## Known Limitations

- **Ring buffer event delivery**: Event delivery from the agent process to the tracer process via shared memory may not work in all configurations. In some setups, events are captured in-process only.
- **Requires `-fno-omit-frame-pointer`**: Target libraries must be compiled with frame pointers for Frida-gum's instruction relocator to work correctly. Without this, SIGTRAP crashes occur.
- **LLVM JIT full build has a libbpf linker error**: The workaround is to build individual targets (`bpftime-agent` and `bpftime-syscall-server`) rather than the full project.
- **No kernel-level visibility**: bpftime cannot trace kernel functions, only userspace functions.
- **LD_PRELOAD required**: Cannot attach to already-running processes; the target must be launched with the agent preloaded.

## References

- [bpftime GitHub Repository](https://github.com/eunomia-bpf/bpftime)
- [Frida-gum (binary instrumentation)](https://frida.re/)
- [BPF Documentation](https://www.kernel.org/doc/html/latest/bpf/index.html)
- [libbpf Documentation](https://libbpf.readthedocs.io/)
