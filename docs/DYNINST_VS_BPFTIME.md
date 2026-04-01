# Dyninst vs bpftime: Binary Instrumentation Comparison

## Overview

Both [Dyninst](https://github.com/dyninst/dyninst) and [bpftime](https://github.com/eunomia-bpf/bpftime) are userspace binary instrumentation tools that can trace function calls without kernel involvement. They use fundamentally the same approach — **code patching with instruction relocation** — but differ in architecture, maturity, and requirements.

## Instrumentation Technique

### Dyninst

Dyninst uses a **two-level trampoline** architecture:

1. The instruction at the function entry is replaced with a jump to a **base trampoline**
2. The base trampoline jumps to a **mini-trampoline** that:
   - Saves registers
   - Executes the instrumentation code (arbitrary C/C++ snippets)
   - Restores registers
   - Jumps back to the base trampoline
3. The base trampoline executes the **relocated original instruction** and returns to the code after the patch point

Dyninst has its own instruction relocator built over 30+ years of development. It supports both **static binary rewriting** (patching the binary on disk) and **dynamic instrumentation** (patching a running process via ptrace).

### bpftime

bpftime delegates to **Frida-gum's `gum_interceptor`** for inline hooking:

1. The function's prologue bytes are overwritten with a jump (5-byte `jmp rel32` or 16-byte far jump)
2. Frida's **X86Relocator** (backed by libcapstone) reads and relocates the overwritten instructions into the trampoline's "on-invoke" section
3. The trampoline has three sections: on-enter (runs BPF program), on-leave (for uretprobes), and on-invoke (relocated original instructions + jump back)

bpftime then executes **eBPF bytecode** (via LLVM JIT or ubpf interpreter) as the instrumentation payload, rather than arbitrary C/C++ code.

## Why Dyninst Loads the Entire Library (and bpftime Doesn't)

Dyninst requires **parsing the entire binary/library** before instrumenting any function. This is fundamental to how it achieves safe instrumentation without requiring frame pointers:

1. **Control Flow Graph (CFG) construction**: Before patching any function, Dyninst parses all instructions to build a complete CFG. It must know where every basic block starts/ends and where branches go, so it can safely insert trampolines without breaking existing jump targets.

2. **Function boundary detection**: Dyninst doesn't trust symbol tables alone. It performs recursive disassembly from known entry points to discover all functions, including those stripped from the symbol table. This requires reading the entire `.text` section.

3. **Instruction relocation safety**: When relocating instructions at a patch point, Dyninst must verify that no other code jumps INTO the middle of the bytes being overwritten. Only a full CFG analysis can guarantee this. If some branch elsewhere targets byte 3 of a 5-byte patch zone, the patch would corrupt that branch target.

4. **Address space management**: Dyninst allocates trampoline memory and must track all code and data sections to avoid conflicts.

bpftime (via Frida) skips all this analysis — it patches at the given offset without understanding the surrounding code. This is the fundamental trade-off:

| | Dyninst | bpftime (Frida) |
|---|---|---|
| **Startup cost** | High (parse entire binary) | Low (just patch at offset) |
| **Safety guarantee** | Proven safe (full CFG analysis) | Best-effort (may crash on complex prologues) |
| **Frame pointers needed** | No (knows full code structure) | Yes (can't analyze surrounding code) |
| **Large binary penalty** | Yes (parsing time scales with binary size) | No (constant-time attach) |

This trade-off explains why Dyninst is the standard in HPC (where binaries are large and optimized) while bpftime/Frida is preferred for lightweight, quick-attach scenarios where the target can be recompiled.

## Why bpftime Needs `-fno-omit-frame-pointer` (and Dyninst Doesn't)

This is the key technical difference between the two tools.

### The Problem

On x86-64, patching a function entry requires overwriting **at least 5 bytes** (for a near `jmp rel32`). The overwritten bytes must be:
- **Complete instructions** — you cannot cut an instruction in half
- **Safely relocatable** — they must produce the same effect when executed at a different address

### With Frame Pointers (`-fno-omit-frame-pointer`)

The function prologue is:
```asm
push %rbp          ; 1 byte  (0x55)
mov %rsp, %rbp     ; 3 bytes (0x48 0x89 0xe5)
sub $N, %rsp       ; 4 bytes (0x48 0x83 0xec 0xNN)
```
Total: **8 bytes** of clean, position-independent stack operations. Frida can trivially relocate these — they work identically at any address.

### Without Frame Pointers (default at `-O2`)

The compiler may produce:
```asm
push %r14          ; 2 bytes (0x41 0x56)
push %rbx          ; 1 byte  (0x53)
sub $0x28, %rsp    ; 4 bytes (0x48 0x83 0xec 0x28)
add %edi, %esi     ; 2 bytes (0x01 0xfe)  <-- actual code, may use RIP-relative addressing
```

Or even worse for leaf functions:
```asm
; no prologue at all (uses red zone)
mov %rdi, %rax     ; first "real" instruction
```

The problems:
1. **Insufficient safe bytes**: Leaf functions using the red zone may have zero prologue bytes before actual code that uses RIP-relative addressing
2. **Instruction boundary misalignment**: The 5-byte jump may land in the middle of a multi-byte instruction, forcing the relocator to also relocate that instruction
3. **Complex instructions at patch boundary**: If the next instruction after the patch uses RIP-relative addressing (e.g., `lea`, `call`, memory operands with `%rip`), relocation becomes complex and can fail silently — producing a broken trampoline that causes SIGTRAP

### Why Dyninst Handles This

Dyninst's instruction relocator is **more mature and handles a wider range of instruction patterns**. It was developed over 30+ years specifically for instrumenting optimized production binaries in HPC environments where `-fomit-frame-pointer` (the default) is the norm.

Dyninst's relocator:
- Handles RIP-relative addressing by adjusting offsets in relocated instructions
- Can relocate instructions across wider address ranges
- Has been extensively tested against compiler-optimized code from GCC, Clang, ICC, etc.

Frida-gum's X86Relocator also attempts to handle arbitrary instruction sequences, but in practice, certain optimized prologue patterns produce instruction sequences it cannot safely handle. The frame pointer prologue acts as a reliable "safe landing zone" of easily-relocatable instructions.

## Overhead Comparison

### Measured Results (AMD EPYC 9354)

| Scenario | Kernel eBPF (uprobe) | bpftime (LLVM 16 JIT) | Dyninst (estimated) |
|---|---|---|---|
| Empty function | ~26 μs/call | ~6 μs/call | ~1-3 μs/call |
| 100 μs function | ~20 μs/call | ~11 μs/call | ~1-3 μs/call |
| 500 μs function | ~17 μs/call | ~13 μs/call | ~1-3 μs/call |

Dyninst overhead estimates are from literature (not measured in this project). Dyninst's two indirect jumps + register save/restore typically cost low single-digit microseconds per instrumented call.

bpftime's higher overhead compared to Dyninst comes from:
- Running eBPF bytecode through a JIT rather than native C/C++ instrumentation code
- Frida-gum's interceptor overhead (more complex trampoline than Dyninst's)
- Shared memory ring buffer operations for event delivery

## Feature Comparison

| Aspect | Dyninst | bpftime |
|---|---|---|
| **Instrumentation engine** | Own relocator + two-level trampolines | Frida-gum interceptor + libcapstone |
| **Static binary rewriting** | Yes (patch binary on disk) | No |
| **Attach to running process** | Yes (via ptrace) | No (requires LD_PRELOAD at startup) |
| **Frame pointer required** | **No** | **Yes** (in practice) |
| **BPF program support** | No (runs C/C++ snippets) | Yes (same .bpf.c programs work in kernel and userspace) |
| **BPF ecosystem compatibility** | None | Full (libbpf, bpftool, CO-RE) |
| **Kernel visibility** | None (userspace only) | None (userspace only) |
| **Root required** | No | No |
| **Target modification** | None (patches in memory) | LD_PRELOAD + `-fno-omit-frame-pointer` |
| **Language** | C++ API | C (libbpf API) |
| **Maturity** | 30+ years, production HPC use | Recent (2023+), active development |
| **Primary use case** | HPC performance analysis, debugging | Drop-in replacement for kernel uprobes |

## When to Use Each

### Use Dyninst When
- You need to instrument **optimized binaries** without recompilation
- You need to **attach to already-running processes**
- You want the **lowest possible overhead** for sparse instrumentation
- You can write instrumentation in **C/C++**
- Frame pointers are not available in the target binary

### Use bpftime When
- You have existing **eBPF/BPF programs** you want to run in userspace
- You want **kernel uprobe compatibility** (same .bpf.c works in both)
- You can control the target's compilation (add `-fno-omit-frame-pointer`)
- You can control the target's launch (use LD_PRELOAD)
- You want to avoid kernel overhead without changing your BPF tooling

## References

### Dyninst
- [Dyninst GitHub Repository](https://github.com/dyninst/dyninst)
- [Anywhere, Any-Time Binary Instrumentation (Bernat & Miller, 2011)](https://pages.cs.wisc.edu/~bernat/publications/Bernat11AWAT.pdf) — Describes Dyninst's two-level trampoline architecture and instruction relocation approach
- [An API for Runtime Code Patching (Buck & Hollingsworth, 2000)](https://doi.org/10.1177/109434200001400404) — Original Dyninst paper describing the code patching and CFG analysis techniques
- [Dyninst Programmer's Guide](https://github.com/dyninst/dyninst/wiki) — Official documentation covering BPatch API, process attachment, and binary rewriting
- [Dyninst on RISC-V (SC'25)](https://dl.acm.org/doi/10.1145/3731599.3767527) — Recent work extending Dyninst's instrumentation to new architectures
- [Using the SystemTap Dyninst Runtime (Red Hat)](https://developers.redhat.com/blog/2021/04/16/using-the-systemtap-dyninst-runtime-environment) — Practical use of Dyninst as SystemTap's userspace backend

### bpftime and Frida
- [bpftime GitHub Repository](https://github.com/eunomia-bpf/bpftime)
- [bpftime: Userspace eBPF Runtime (Zheng et al., 2023)](https://arxiv.org/abs/2311.07923) — Original bpftime paper with performance claims
- [bpftime Issue #424: LLVM 17 Symbol Bug](https://github.com/eunomia-bpf/bpftime/issues/424) — Documents the LLVM ORC JIT incompatibility
- [Frida-gum Interceptor Source (guminterceptor.c)](https://github.com/frida/frida-gum/blob/main/gum/guminterceptor.c) — Core hooking logic
- [Frida-gum x86 Relocator (gumx86relocator.c)](https://github.com/frida/frida-gum/blob/main/gum/arch-x86/gumx86relocator.c) — Instruction relocation implementation that requires safe prologues
- [Frida: Comparison of Function Hooking Libraries](https://github.com/frida/frida/wiki/Comparison-of-function-hooking-libraries) — Frida's own comparison with other hooking frameworks

### Binary Instrumentation Background
- [Unveiling Dynamic Binary Instrumentation Techniques (2025)](https://arxiv.org/html/2508.00682v1) — Survey comparing DBI tools including Dyninst, Pin, DynamoRIO, and Frida
- [iProbe: A Lightweight User-Level Dynamic Instrumentation Tool (Arora et al.)](https://www.nipunarora.net/pdf/iprobe.pdf) — Compares overhead of Dyninst, SystemTap, and iProbe for userspace tracing
- [Unravelling Code Injection in Binaries (Tuxology)](https://suchakra.wordpress.com/2016/07/03/unravelling-code-injection-in-binaries/) — Visual explanation of code patching and trampoline techniques
- [Learning How Frida Works (IdentX Labs)](https://medium.com/@identx_labs/learning-how-frida-works-a-deep-dive-into-the-hooking-framework-d522ae13cc67) — Deep dive into Frida's interceptor internals

### Frame Pointers and x86-64 ABI
- [The Return of the Frame Pointers (Brendan Gregg, 2024)](https://www.brendangregg.com/blog/2024-03-17/the-return-of-the-frame-pointers.html) — Why frame pointers are making a comeback in production systems
- [Stack Frame Layout on x86-64 (Eli Bendersky)](https://eli.thegreenplace.net/2011/09/06/stack-frame-layout-on-x86-64/) — Technical reference for x86-64 function prologues and the red zone
- [System V AMD64 ABI](https://gitlab.com/x86-psABIs/x86-64-ABI) — Official x86-64 calling convention and stack frame specification

### This Project
- [bpftime Design Documentation](BPFTIME_DESIGN.md) — Full bpftime integration findings, installation, and benchmark results
- [eBPF Tracer Design](EBPF_DESIGN.md) — Kernel eBPF uprobe implementation details
- [Benchmark Methodology](BENCHMARK.md) — How the benchmarks are structured and run
