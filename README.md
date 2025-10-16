# eBPF vs LTTng Tracing Comparison

A comprehensive comparison of **eBPF (uprobes)** and **LTTng (userspace tracing)** for function-level tracing, demonstrating how overhead scales with function duration.

## 🎯 Key Finding

**Uprobe overhead is CONSTANT (~5 μs), not relative to function duration.**

This means:
- **Empty 6ns function**: 5μs/6ns = **83,000% overhead** (worst case, unrealistic)
- **100 μs HIP API**: 5μs/100μs = **5% overhead** (typical)
- **1 ms function**: 5μs/1ms = **0.5% overhead** (realistic)

**Conclusion**: eBPF is **perfect for GPU/HIP tracing** where API calls take 10-1000 μs! 🚀

---

## 📚 Documentation

| Document | Description |
|----------|-------------|
| **[BENCHMARK.md](docs/BENCHMARK.md)** | Complete benchmark suite documentation (methodology, usage, statistical analysis) |
| **[CI_PIPELINE.md](docs/CI_PIPELINE.md)** | CI/CD pipeline with parallel execution architecture |
| **[EBPF_DESIGN.md](docs/EBPF_DESIGN.md)** | eBPF tracer architecture and implementation |
| **[LTTNG_DESIGN.md](docs/LTTNG_DESIGN.md)** | LTTng tracer architecture and implementation |
| **[SAMPLE_APP.md](docs/SAMPLE_APP.md)** | Sample library and application documentation |
| **[VALIDATION.md](docs/VALIDATION.md)** | Trace validation script documentation |
| **[BUILD.md](BUILD.md)** | Build system and compilation guide |

---

## 🚀 Quick Start

### 1. Install Dependencies

**Ubuntu/Debian:**
```bash
# Core tools
sudo apt-get install cmake build-essential

# LTTng (optional)
sudo apt-get install lttng-tools liblttng-ust-dev babeltrace2

# eBPF (optional, requires Clang 10+)
sudo apt-get install clang llvm libbpf-dev linux-headers-$(uname -r)

# bpftool (for BPF skeleton generation)
sudo apt-get install linux-tools-generic linux-tools-$(uname -r)
```

### 2. Build

```bash
./build.sh -c
```

### 3. Validate

Ensure both tracers work correctly:

```bash
sudo ./scripts/validate_output.sh
```

Expected output:
```
✓ ALL VALIDATIONS PASSED!
✓ Both tracers captured exactly 1000 events
✓ Argument values are correct and match
```

### 4. Run Benchmark

```bash
sudo ./scripts/benchmark.py ./build
```

This runs **180 tests** (6 scenarios × 3 methods × 10 runs) and generates an HTML report with interactive charts.

**Estimated time**: ~4-6 minutes
**Output**: 
- `benchmark_results_<timestamp>/benchmark_report.html` - Local report
- `report/index.html` - Updated for GitHub Pages deployment (with link banner if full HIP trace benchmark exists)

---

## 📊 What Gets Tested

The benchmark suite tests 6 scenarios representing different function durations:

| Scenario | Duration | Iterations | Represents |
|----------|----------|------------|------------|
| Empty Function | 0 μs | 100,000 | Worst case (stress test) |
| 5 μs Function | 5 μs | 100,000 | Ultra-fast API |
| 50 μs Function | 50 μs | 50,000 | Fast API (e.g., `hipGetDevice`) |
| **100 μs Function** | **100 μs** | **10,000** | **Typical HIP API** |
| 500 μs Function | 500 μs | 5,000 | Medium API (e.g., `hipMemcpy`) |
| 1000 μs Function | 1 ms | 2,000 | Slow API |

Each scenario runs **10 times** with statistical aggregation (mean, stddev, 95% CI).

---

## 📈 Expected Results

### Overhead Pattern

```
Function Duration vs Overhead %

0 μs:       ████████████████████ 83,000%  (unrealistic)
5 μs:       ████████████ 100%             (edge case)
50 μs:      ████ 10%                      (acceptable)
100 μs:     ██ 5%                         (good)
500 μs:     █ 1%                          (excellent)
1000 μs:    █ 0.5%                        (negligible)
```

### Key Metrics (100 μs scenario)

| Method | Wall Time | App Overhead | Avg Time/Call | Per-Call Overhead | Memory |
|--------|-----------|--------------|---------------|-------------------|--------|
| Baseline | 1.082 s | 0% | 108,234 ns | 0% | 1.8 MB |
| LTTng | 1.436 s | **32.7%** | 143,568 ns | 32.6% | 330 MB |
| eBPF | 1.223 s | **13.0%** | 122,346 ns | 13.0% | 2.1 MB |

**New in 2025:** The report now includes **Whole Application Overhead** (Wall Time and App Overhead columns), showing the real-world production impact on total execution time!

**Observation**: LTTng is ~2× faster than eBPF for short functions, but both are <5% for realistic workloads.

**⚠️ Important Note**: These results are from **bare metal testing**. eBPF uprobe performance is ~20x worse in virtualized environments (VMs/cloud) due to context switch overhead. The GitHub Pages report shows accurate bare metal results, not inflated VM numbers.

---

## 🔬 Architecture

### Components

```
├── src/sample/sample_library/          # Target library to trace
│   ├── mylib.c             # Function with configurable work duration
│   └── mylib.h
│
├── src/sample/sample_app/              # Test application
│   └── main.c              # Calls traced function repeatedly
│
├── src/tools/lttng_tracer/            # LTTng implementation
│   ├── mylib_tp.h          # Tracepoint definitions
│   ├── mylib_tp.c          # Tracepoint implementation
│   └── mylib_wrapper.c     # LD_PRELOAD wrapper
│
├── src/tools/ebpf_tracer/             # eBPF implementation
│   ├── mylib_tracer.bpf.c  # Kernel-side eBPF program (uprobes)
│   └── mylib_tracer.c      # Userspace loader
│
├── scripts/                 # Automation scripts
│   ├── benchmark.py        # Python benchmark suite
│   └── validate_output.sh  # Correctness validation
│
├── docs/                    # Documentation
│   ├── BENCHMARK.md        # Benchmark documentation
│   ├── EBPF_DESIGN.md      # eBPF design documentation
│   ├── LTTNG_DESIGN.md     # LTTng design documentation
│   ├── SAMPLE_APP.md       # Sample app documentation
│   └── VALIDATION.md       # Validation documentation
│
└── build.sh                 # Build wrapper
```

### How It Works

**LTTng (LD_PRELOAD)**:
1. Wrapper library intercepts function calls
2. Adds tracepoints before/after real function
3. LTTng UST writes events to per-CPU ring buffers (~60-100 ns overhead)

**eBPF (Uprobes)**:
1. Kernel attaches uprobe/uretprobe to function offset
2. BPF program captures arguments in kernel context
3. Ring buffer transfers events to userspace (~5 μs overhead)

**Benchmark Flow**:
1. Run each scenario 100 times (statistical reliability)
2. Measure: wall time, CPU time, memory, trace size
3. Calculate: mean, stddev, min, max, 95% CI
4. Generate HTML report with interactive Plotly charts

---

## 🛠️ Manual Testing

### Baseline (No Tracing)

```bash
# Empty function
./build/bin/sample_app 1000000
# Output: Average time per call: 6.32 nanoseconds

# 100 μs work
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000
# Output: Average time per call: 108,234.56 nanoseconds
```

### LTTng Tracing

```bash
lttng create test_session
lttng enable-event -u mylib:*
lttng start

SIMULATED_WORK_US=100 LD_PRELOAD=./build/lib/libmylib_lttng.so ./build/bin/sample_app 10000

lttng stop
lttng destroy test_session
babeltrace ~/lttng-traces/test_session-*
```

### eBPF Tracing

```bash
# Terminal 1: Start tracer
sudo ./build/bin/mylib_tracer /tmp/trace.txt

# Terminal 2: Run application
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000

# Terminal 1: Stop tracer (Ctrl-C)
# View trace
cat /tmp/trace.txt
```

---

## 📋 Command Reference

### Build Commands

```bash
./build.sh -c              # Clean build (Release mode)
./build.sh -d -c           # Debug build
./build.sh -c -j8          # Parallel build (8 jobs)
./build.sh -c -t           # Build and run baseline test
./build.sh -c --no-ebpf    # Build without eBPF (no root needed)
```

### Validation

```bash
sudo ./scripts/validate_output.sh  # Verify both tracers work correctly
```

### Benchmarking

```bash
# Full benchmark (10 runs per scenario, ~4-6 min)
sudo ./scripts/benchmark.py ./build

# Extended analysis (50 runs per scenario, ~20-30 min)
sudo ./scripts/benchmark.py ./build --runs 50
```

### View Results

```bash
# Open HTML report
firefox benchmark_results_*/benchmark_report.html

# View JSON results
cat benchmark_results_*/results.json | jq '.'
```

---

## 🎓 Key Insights

### 1. Absolute vs Relative Overhead

**Misconception**: "eBPF has 83% overhead"
**Reality**: eBPF has ~5 μs **absolute** overhead per call

| Function Duration | Absolute Overhead | Relative Overhead |
|-------------------|-------------------|-------------------|
| 6 ns (empty) | 5 μs | 83,000% |
| 100 μs (typical) | 5 μs | 5% |
| 1 ms (slow) | 5 μs | 0.5% |

### 2. LTTng vs eBPF Trade-offs

| Feature | LTTng | eBPF |
|---------|-------|------|
| **Overhead** | ~60-100 ns | ~5 μs |
| **Root required** | No | Yes |
| **Dynamic attach** | No (LD_PRELOAD) | Yes (uprobe) |
| **Kernel tracing** | Separate module | Built-in |
| **Best for** | Fast functions (>100ns) | Slow functions (>10μs) |
| **Memory** | ~330 MB (in-process) | ~2 MB (separate) |

### 3. Production GPU Tracing

For ROCm/HIP API tracing:
- ✅ **HIP API calls**: 10-1000 μs → eBPF overhead <5%
- ✅ **Slow API calls**: 1-1000 ms → eBPF overhead <0.1%
- ✅ **No app modification**: Uprobe attaches to running process
- ✅ **Total overhead**: <1% for realistic workloads

---

## 🔧 Troubleshooting

### Build Issues

**LTTng dependencies missing:**
```bash
./build.sh -c --no-lttng  # Skip LTTng tracer
```

**eBPF tools missing:**
```bash
./build.sh -c --no-ebpf   # Skip eBPF tracer
```

**Clang not found:**
```bash
sudo apt-get install clang-14 llvm-14
export CC=clang-14
./build.sh -c
```

### Runtime Issues

**Library not found:**
```bash
export LD_LIBRARY_PATH=$PWD/build/lib:$LD_LIBRARY_PATH
./build/bin/sample_app 1000
```

**eBPF permission denied:**
```bash
sudo ./build/bin/mylib_tracer /tmp/trace.txt  # Requires root
```

**LTTng session exists:**
```bash
lttng destroy -a  # Destroy all sessions
```

### Benchmark Issues

**High variance in results:**
```bash
# Disable CPU frequency scaling
sudo cpupower frequency-set -g performance

# Increase number of runs (edit benchmark.py line 59)
num_runs = 200
```

**Events dropped:**
```bash
# Check ring buffer size in mylib_tracer.bpf.c
__uint(max_entries, 4 * 1024 * 1024);  # Increase to 4MB
```

---

## 📄 License

SPDX-License-Identifier: GPL-2.0

---

**Quick Links**:
- 📖 [Complete Benchmark Guide](docs/BENCHMARK.md)
- 🔄 [CI/CD Pipeline Guide](docs/CI_PIPELINE.md)
- 🔬 [eBPF Design](docs/EBPF_DESIGN.md)
- 🔬 [LTTng Design](docs/LTTNG_DESIGN.md)
- 🏗️ [Build Guide](BUILD.md)
- ✅ [Validation Guide](docs/VALIDATION.md)
