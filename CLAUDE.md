# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Benchmarking framework comparing eBPF (kernel uprobe), bpftime (userspace uprobe), and LTTng (LD_PRELOAD) tracing overhead for HIP/GPU API instrumentation. Core finding: kernel uprobe overhead is ~5-26 μs/call; bpftime with LLVM 16 JIT achieves ~6-15 μs/call (comparable or faster, purely userspace, no root needed).

## Build Commands

```bash
# Full build (requires clang-14 and all deps)
export CC=clang-14
./build.sh -c              # Clean build
./build.sh -j$(nproc)      # Parallel build
./build.sh --no-lttng      # Skip LTTng components
./build.sh --no-ebpf       # Skip eBPF components
./build.sh --no-bpftime    # Skip bpftime components
./build.sh -d              # Debug build
./build.sh -t              # Build + run baseline test

# Manual CMake
mkdir -p build && cd build
cmake .. -DCMAKE_C_COMPILER=clang-14
make -j$(nproc)
```

Build outputs: `build/bin/` (executables), `build/lib/` (shared libraries), `build/mylib_tracer.bpf.o` (eBPF bytecode).

## Running

```bash
# Baseline (no tracing)
SIMULATED_WORK_US=100 build/bin/sample_app 10000

# eBPF tracer (requires root)
sudo build/bin/mylib_tracer -l build/lib/libmylib.so -a build/bin/sample_app -- 10000

# bpftime tracer (userspace uprobes, no root needed)
# Terminal 1: start tracer with syscall-server
LD_PRELOAD=~/.bpftime/libbpftime-syscall-server.so build/bin/bpftime_tracer -l build/lib/libmylib.so
# Terminal 2: run app with bpftime agent
LD_PRELOAD=~/.bpftime/libbpftime-agent.so SIMULATED_WORK_US=100 build/bin/sample_app 10000

# LTTng tracer
lttng create && lttng enable-event -u 'mylib:*' && lttng start
LD_PRELOAD=build/lib/libmylib_lttng.so SIMULATED_WORK_US=100 build/bin/sample_app 10000
lttng stop && lttng destroy
```

## Testing & Benchmarks

```bash
# Validation (correctness, requires root)
sudo ./scripts/validate_output.sh

# Full benchmark suite (~4-6 min, requires root)
sudo python3 scripts/benchmark.py

# Generate report from existing results
python3 scripts/regenerate_report.py
python3 scripts/combine_results.py
```

Benchmarks must run on **bare metal** — VMs cause ~20× eBPF overhead distortion. CI only runs validation, not performance benchmarks.

## Architecture

**Dual-tracer design** with shared target library for fair comparison:

- `src/sample/sample_library/` — Target library (`libmylib.so`) with `my_traced_function()` that has configurable work duration via `SIMULATED_WORK_US`
- `src/sample/sample_app/` — Benchmark harness calling the function N times, measuring wall/CPU time and memory
- `src/tools/ebpf_tracer/` — Kernel-side eBPF program (`*.bpf.c`) using uprobes/uretprobes + userspace loader. BPF skeleton header is auto-generated at build time
- `src/tools/bpftime_tracer/` — Userspace eBPF loader using bpftime runtime. Same BPF program as kernel tracer but runs entirely in userspace via `LD_PRELOAD` interception of BPF syscalls
- `src/tools/lttng_tracer/` — LD_PRELOAD wrapper intercepting function calls + LTTng tracepoint definitions (`mylib_tp.h`/`mylib_tp.c`)

**Benchmark pipeline** (`scripts/benchmark.py`): Runs 6 scenarios (0-1000 μs work durations) × up to 4 methods (baseline, eBPF, bpftime, LTTng) × 10 runs each, computing statistics (mean, stddev, 95% CI) and generating interactive Plotly HTML reports.

## Key Dependencies

- CMake 3.16+, Clang 14+ (required for eBPF compilation)
- libbpf 1.7+, libelf, zlib, bpftool (eBPF stack)
- lttng-tools, liblttng-ust-dev, babeltrace2 (LTTng stack)
- bpftime runtime (`~/.bpftime/`) — optional, for userspace eBPF tracing. Install from https://github.com/eunomia-bpf/bpftime
- Python3 with plotly, pandas, numpy (benchmarking/reports)

## CI

GitHub Actions (`.github/workflows/benchmark-ci.yml`): builds with clang-14, runs `validate_output.sh`, deploys pre-generated reports to GitHub Pages on main branch pushes.
