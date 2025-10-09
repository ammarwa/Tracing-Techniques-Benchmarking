# Benchmark Suite Documentation

## Overview

This project includes a comprehensive Python-based benchmark suite that measures eBPF and LTTng tracing overhead across multiple realistic scenarios. The benchmark demonstrates how uprobe overhead scales with function duration and proves that eBPF is suitable for GPU/HIP API tracing.

## Quick Start

### 1. Build the Project

```bash
./build.sh -c
```

### 2. Run Comprehensive Benchmark

```bash
# Default: 10 repetitions per scenario (~4-6 minutes)
sudo ../scripts/benchmark.py ./build

# Quick test: 5 repetitions (~2-4 minutes)
sudo ../scripts/benchmark.py ./build --runs 5

# CI analysis: 20 repetitions (~8-12 minutes)
sudo ../scripts/benchmark.py ./build --runs 20

# Full statistical analysis: 100 repetitions (~40-60 minutes)
sudo ../scripts/benchmark.py ./build --runs 100
```

**Help and Options:**
```bash
../scripts/benchmark.py --help
```

### 3. View Results

The benchmark creates a timestamped results directory:

```
benchmark_results_YYYYMMDD_HHMMSS/
├── benchmark_report.html       # Interactive HTML report (open this!)
├── results.json                # Raw benchmark data
├── lttng_*us_r*/              # LTTng trace files (N runs per scenario)
└── ebpf_*us_r*.txt           # eBPF trace files (N runs per scenario)
```

where N is the number of repetitions (default: 10)

Open the HTML report in your browser:
```bash
firefox benchmark_results_*/benchmark_report.html
```

---

## Test Scenarios

The benchmark tests 6 scenarios representing different function durations:

| Scenario | Work Duration | Iterations | Represents |
|----------|---------------|------------|------------|
| **Empty Function** | 0 μs | 1,000,000 | Worst case: minimal work function |
| **5 μs Function** | 5 μs | 100,000 | Ultra-fast API (comparable to uprobe overhead) |
| **50 μs Function** | 50 μs | 50,000 | Fast API (e.g., `hipGetDevice`) |
| **100 μs Function** | 100 μs | 10,000 | Typical API (e.g., `hipMalloc` small) |
| **500 μs Function** | 500 μs | 5,000 | Medium API (e.g., `hipMemcpy` medium) |
| **1000 μs Function** | 1 ms | 2,000 | Slow API (e.g., large allocations) |

---

## Statistical Reliability

### Multiple Runs for Trustworthy Results

Each scenario runs **multiple times** to ensure statistical reliability. The number of repetitions is configurable via the `--runs` option:

- **Mean**: Average value across all runs
- **Standard Deviation**: Measure of variability/spread
- **Min/Max**: Range of observed values
- **95% Confidence Interval**: ±margin of error (1.96 × std_err)

### Recommended Run Counts

| Runs | Total Tests | Duration | Use Case |
|------|-------------|----------|----------|
| **5** | 90 (6×3×5) | ~2-4 min | Quick development test, smoke tests |
| **10** *(default)* | 180 (6×3×10) | ~4-6 min | Daily development, fast iteration |
| **20** | 360 (6×3×20) | ~8-12 min | **CI/CD, automated testing, GitHub Actions** |
| **50** | 900 (6×3×50) | ~20-30 min | Production analysis, detailed reports |
| **100** | 1,800 (6×3×100) | ~40-60 min | Research, publication-quality statistics |
| **200** | 3,600 (6×3×200) | ~80-120 min | Maximum precision, long-term benchmarks |

### Command-Line Usage

```bash
# Quick test (5 runs)
sudo ../scripts/benchmark.py ./build --runs 5

# Default (10 runs)
sudo ../scripts/benchmark.py ./build

# CI/CD (20 runs) - recommended for automated testing
sudo ../scripts/benchmark.py ./build --runs 20

# Production analysis (50 runs)
sudo ../scripts/benchmark.py ./build --runs 50

# Full statistical analysis (100 runs)
sudo ../scripts/benchmark.py ./build --runs 100

# Maximum precision (200 runs)
sudo ../scripts/benchmark.py ./build --runs 200
```

The script will show a warning and ask for confirmation if you specify more than 200 runs.

### Statistical Output

The benchmark calculates comprehensive statistics:

```json
{
  "scenario": "100 μs Function",
  "method": "lttng",
  "avg_time_per_call_ns": 143567.89,
  "num_runs": 100,
  "avg_time_stddev": 1234.56,
  "avg_time_min": 140123.45,
  "avg_time_max": 147890.12,
  "confidence_95_margin": 23.45
}
```

The HTML report displays confidence intervals in the results table:

| Scenario | Method | Avg Time (ns) | ±95% CI | Overhead % |
|----------|--------|--------------|---------|------------|
| 100 μs   | Baseline | 108,234.56 | ±12.34 | 0% |
| 100 μs   | LTTng | 143,567.89 | ±23.45 | 32.6% |
| 100 μs   | eBPF | 122,345.67 | ±18.92 | 13.0% |

---

## How It Works

### Simulated Work Duration

The sample library supports configurable work duration via environment variable:

```c
// In src/sample/sample_library/mylib.c
void set_simulated_work_duration(unsigned int sleep_us);

// Called automatically from environment variable
SIMULATED_WORK_US=100 ./build/bin/sample_app 1000
```

### Busy-Wait Implementation

We use a busy-wait loop (not `usleep`) for accurate microsecond timing:

```c
static void busy_sleep_us(unsigned int microseconds) {
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);

    unsigned long target_ns = microseconds * 1000UL;
    unsigned long elapsed_ns;

    do {
        clock_gettime(CLOCK_MONOTONIC, &current);
        elapsed_ns = (current.tv_sec - start.tv_sec) * 1000000000UL +
                     (current.tv_nsec - start.tv_nsec);
    } while (elapsed_ns < target_ns);
}
```

This ensures:
- ✅ Accurate microsecond delays
- ✅ Consistent CPU usage (simulates actual work)
- ✅ No scheduler interference

### Metrics Captured

For each scenario and method, we capture:

**Application Metrics:**
- Wall time (total elapsed time)
- User CPU time
- System CPU time
- Maximum RSS (memory usage)
- Average time per function call

**Tracer Metrics (eBPF only):**
- Tracer CPU usage percentage
- Tracer memory consumption
- Trace file size
- Events captured/dropped

---

## Understanding the Results

### Key Finding: Absolute vs Relative Overhead

**Uprobe overhead is CONSTANT (~5 μs per call), not relative to function duration.**

This means:
- **Empty 6ns function**: 5,000/6 = **83,000% overhead** ❌ (worst case, unrealistic)
- **100 μs HIP API**: 5,000/100,000 = **5% overhead** ✅ (typical case)
- **1 ms GPU kernel**: 5,000/1,000,000 = **0.5% overhead** ✅ (realistic workload)

### Expected Overhead Pattern

```
Function Duration (log scale) vs Overhead %

Empty (0μs):      █████████████████████ 83,000% overhead ❌
5 μs:             ████████████ 100% overhead ⚠️
50 μs:            ████ 10% overhead ⚠️
100 μs:           ██ 5% overhead ✅
500 μs:           █ 1% overhead ✅
1000 μs:          █ 0.5% overhead ✅
```

### Key Insights

1. **Crossover point: ~100 μs**
   - Below 100 μs: Overhead is noticeable (>5%)
   - Above 100 μs: Overhead is acceptable (<5%)
   - Above 1 ms: Overhead is negligible (<1%)

2. **Production GPU workloads**
   - HIP API calls: 10-1000 μs → 0.5-5% overhead ✅
   - GPU kernels: 1-1000 ms → <0.1% overhead ✅
   - **Total application overhead: <1%** ✅

3. **Why 83% and 0.3% are both correct**
   - 83% is for empty 6ns functions (stress test, not realistic)
   - 0.3% is for real GPU applications with ms-scale kernels (realistic)
   - Both measure the same ~5 μs constant overhead!

---

## HTML Report Features

The generated HTML report includes:

### 📈 Interactive Charts (Plotly.js)

1. **Relative Overhead vs Function Duration (Log-Log Plot)**
   - Shows how overhead percentage decreases as function duration increases
   - Demonstrates the constant absolute overhead principle

2. **Absolute Time Per Call (Stacked Bar Chart)**
   - Shows baseline time + tracer overhead
   - Visualizes how overhead becomes insignificant for longer functions

3. **Memory Usage Comparison (Grouped Bar Chart)**
   - Compares application and tracer memory consumption
   - Highlights LTTng's in-process buffering (~330 MB) vs eBPF's separate process (~2 MB)

### 📊 Detailed Tables

- Complete results for all scenarios and methods
- Color-coded overhead percentages:
  - 🟢 Green: <10% overhead (good)
  - 🟡 Yellow: 10-50% overhead (warning)
  - 🔴 Red: >50% overhead (bad)
- CPU and memory breakdowns
- Statistical confidence intervals (±95% CI)

### 📝 Analysis and Recommendations

The report includes:
- When to use LTTng vs eBPF
- GPU/HIP tracing recommendations
- Interpretation guidelines

---

## Methodology

### Benchmark Design

Each benchmark run follows this sequence:

1. **Setup Phase**
   - Create trace session (LTTng) or start tracer (eBPF)
   - Configure simulated work duration via `SIMULATED_WORK_US`

2. **Measurement Phase**
   - Run application with `/usr/bin/time` for resource metrics
   - Capture stdout for per-call timing
   - Monitor tracer resource usage (eBPF only)

3. **Collection Phase**
   - Stop trace session
   - Calculate trace file size
   - Parse timing and resource data

4. **Aggregation Phase** (100 runs)
   - Collect individual measurements
   - Calculate mean, stddev, min, max
   - Compute 95% confidence interval

### Statistical Validity

- **Sample size**: Configurable (default n=10, CI uses n=20, research n=100)
- **Confidence level**: 95% (1.96 × std_err)
- **Why multiple runs?**
  - Good statistical power (can detect 10-20% differences with n≥20)
  - Narrow confidence intervals (±1-5% of mean with n≥20)
  - Configurable tradeoff between speed and precision
  - Industry standard for performance benchmarks

### Interpreting Confidence Intervals

**Overlapping intervals** → No significant difference:
```
Method A: 100 ± 10 ns  (90-110 ns range)
Method B: 105 ± 8 ns   (97-113 ns range)
```

**Non-overlapping intervals** → Significantly different:
```
Method A: 100 ± 5 ns   (95-105 ns range)
Method B: 120 ± 5 ns   (115-125 ns range)
```

---

## Manual Testing

### Test Specific Durations

Without running the full suite:

```bash
# Baseline (no tracing)
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000

# With LTTng
lttng create test_session
lttng enable-event -u mylib:*
lttng start
SIMULATED_WORK_US=100 LD_PRELOAD=./build/lib/libmylib_lttng.so ./build/bin/sample_app 10000
lttng stop
lttng destroy test_session

# With eBPF (in separate terminal)
sudo ./build/bin/mylib_tracer /tmp/trace.txt
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000
sudo pkill -INT mylib_tracer
```

### Custom Scenarios

Modify `benchmark.py`:

```python
self.scenarios = [
    BenchmarkScenario(
        name="Custom Test",
        simulated_work_us=250,  # 250 μs
        iterations=20000,
        description="My custom scenario"
    ),
    # Add more...
]
```

---

## Troubleshooting

### Python Dependencies

The benchmark uses only standard library modules (no external dependencies):
- `os`, `sys`, `subprocess`, `time`, `json`, `re`, `statistics`, `pathlib`, `datetime`, `dataclasses`, `typing`
- Plotly.js is loaded via CDN (no local install needed)

### Permission Errors

eBPF tracing requires root:

```bash
sudo ../scripts/benchmark.py ./build
```

### LTTng Session Errors

If LTTng sessions persist:

```bash
lttng destroy -a  # Destroy all sessions
lttng-sessiond --daemonize  # Restart daemon
```

### Tracer Timeout

If eBPF tracer hangs:

```bash
sudo pkill -9 mylib_tracer
```

### High Standard Deviation

If confidence intervals are wide (>10% of mean):
- Close unnecessary applications
- Disable CPU frequency scaling: `sudo cpupower frequency-set -g performance`
- Run during low system load
- Increase number of runs: `sudo ../scripts/benchmark.py ./build --runs 100` or `--runs 200`

---

## Example Output

### Console Output

```
======================================================================
COMPREHENSIVE eBPF vs LTTng BENCHMARK SUITE
======================================================================

======================================================================
Scenario: 100 μs Function
  Work Duration: 100 μs
  Iterations: 10,000
  Description: Typical API: ~100μs (e.g., hipMalloc small, hipMemcpy small)
======================================================================

  [BASELINE] 100 μs Function - Running 10 times for statistical reliability
    Run 1/10...
    Completed 10 runs

  [LTTNG] 100 μs Function - Running 10 times for statistical reliability
    Run 1/10...
    Completed 10 runs

  [EBPF] 100 μs Function - Running 10 times for statistical reliability
    Run 1/10...
    Completed 10 runs

======================================================================
✅ BENCHMARK COMPLETE!
======================================================================

HTML Report: benchmark_results_20251009_143022/benchmark_report.html
View in browser: file:///home/.../benchmark_report.html
```

### JSON Results

```json
{
  "scenario": "100 μs Function",
  "method": "lttng",
  "iterations": 10000,
  "simulated_work_us": 100,
  "wall_time_s": 1.436,
  "user_cpu_s": 1.234,
  "system_cpu_s": 0.189,
  "max_rss_kb": 332416,
  "avg_time_per_call_ns": 143567.89,
  "trace_size_mb": 145.67,
  "num_runs": 10,
  "avg_time_stddev": 1234.56,
  "avg_time_min": 140123.45,
  "avg_time_max": 147890.12,
  "wall_time_stddev": 0.012,
  "confidence_95_margin": 23.45
}
```

---

## Recommendations

### When to Use Each Tracing Method

| Method | Best For | Avoid When |
|--------|----------|------------|
| **LTTng** | • Functions > 100ns<br>• Can modify app or use LD_PRELOAD<br>• Need rich userspace context<br>• High call frequency (>10K/sec) | • Cannot modify application<br>• Need kernel-level tracing<br>• Want dynamic attach/detach |
| **eBPF** | • Functions > 10μs<br>• Cannot modify application<br>• Need kernel visibility<br>• Want dynamic attach/detach<br>• **GPU/HIP API tracing** | • Ultra-fast functions (<1μs)<br>• Very high frequency (>1M calls/sec)<br>• Need real-time streaming |

### GPU/HIP Tracing Recommendation

For GPU and HIP API tracing, **eBPF is highly recommended** because:

1. **HIP API calls typically take 10-1000 μs** (much slower than uprobe overhead)
2. **GPU kernel execution takes milliseconds** (making tracer overhead negligible)
3. **No application modification required** (zero code changes)
4. **Can trace at kernel/driver boundary** for complete visibility
5. **Expected total application overhead: <1%** ✅

### For Different Use Cases

**Daily Development:**
- Use quick manual tests with specific `SIMULATED_WORK_US` values
- Fast iteration without full benchmark suite

**Performance Analysis:**
- Run comprehensive benchmark with default 100 runs
- Generate HTML report for detailed insights

**Presentations & Reports:**
- Use the interactive HTML report
- Demonstrates overhead scaling visually
- Includes statistical confidence intervals

**Production Decisions:**
- Run benchmark on target hardware
- Share HTML report with stakeholders
- Use statistical data to justify eBPF for GPU tracing

---

## Integration with CI/CD

### GitHub Actions Configuration

This project includes a GitHub Actions workflow (`.github/workflows/benchmark-ci.yml`) that:

1. **Builds the project** with both LTTng and eBPF support
2. **Runs validation tests** to ensure tracers work correctly
3. **Executes benchmark with 20 repetitions** for CI statistical analysis
4. **Publishes HTML report** to GitHub Pages
5. **Archives results** as workflow artifacts

**Key features:**
- Runs on: push to main/master, pull requests, manual trigger
- Duration: ~8-12 minutes for 20 repetitions
- Publishes interactive benchmark report at `https://<username>.github.io/<repo>/`
- Stores raw results as artifacts for 30 days

**Workflow snippet:**
```yaml
- name: Run benchmark with 20 repetitions
  run: |
    sudo python3 scripts/benchmark.py ./build --runs 20
  timeout-minutes: 30
```

### Local CI Testing

Test the CI configuration locally:

```bash
# Build like CI does
./build.sh -c

# Run validation like CI does
sudo ./scripts/validate_output.sh

# Run benchmark with CI settings (20 runs)
sudo ./scripts/benchmark.py ./build --runs 20
```

### Automated Regression Testing

Create a custom CI script with regression checks:

```bash
#!/bin/bash
# ci_benchmark_with_checks.sh

# Build
./build.sh -c

# Run benchmark (adjust repetitions based on CI time budget)
sudo python3 scripts/benchmark.py ./build --runs 20

# Check for regressions
python3 << 'PYTHON'
import json
from pathlib import Path
import sys

# Load results
results_dir = max(Path('.').glob('benchmark_results_*'), key=lambda p: p.stat().st_mtime)
with open(results_dir / 'results.json') as f:
    results = json.load(f)

# Find baseline for each scenario
baselines = {r['scenario']: r['avg_time_per_call_ns']
             for r in results if r['method'] == 'baseline'}

# Check eBPF overhead for realistic workloads
failed = False
for r in results:
    if r['method'] == 'ebpf' and r['simulated_work_us'] >= 100:
        baseline_ns = baselines[r['scenario']]
        overhead_pct = ((r['avg_time_per_call_ns'] / baseline_ns) - 1) * 100

        # Threshold: 10% for realistic workloads (≥100μs)
        if overhead_pct > 10:
            print(f"❌ REGRESSION: {r['scenario']} has {overhead_pct:.1f}% overhead (threshold: 10%)")
            failed = True
        else:
            print(f"✅ PASS: {r['scenario']} has {overhead_pct:.1f}% overhead")

if failed:
    print("\n❌ Benchmark regression detected!")
    sys.exit(1)
else:
    print("\n✅ All benchmarks passed!")
PYTHON
```

### Export Results to CSV

```python
import csv
from pathlib import Path
import json

# Load results
with open('benchmark_results_*/results.json') as f:
    results = json.load(f)

# Export to CSV
with open('benchmark_results.csv', 'w', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=results[0].keys())
    writer.writeheader()
    writer.writerows(results)
```

---

## Performance Impact

### Benchmark Duration

Time depends on number of repetitions:

| Runs | Total Tests | Estimated Duration |
|------|-------------|-------------------|
| 5 | 90 (6×3×5) | ~2-4 minutes |
| **10** *(default)* | 180 (6×3×10) | ~4-6 minutes |
| **20** *(CI)* | 360 (6×3×20) | ~8-12 minutes |
| 50 | 900 (6×3×50) | ~20-30 minutes |
| 100 | 1,800 (6×3×100) | ~40-60 minutes |
| 200 | 3,600 (6×3×200) | ~80-120 minutes |

**Formula**: `duration_minutes ≈ num_runs × 0.4 to 0.6`

### Disk Usage

Each run creates trace files (scales with number of repetitions):

**For default 10 runs:**
- **LTTng**: ~150 MB per run × 10 runs × 6 scenarios = ~9 GB (temporary)
- **eBPF**: ~1-5 MB per run × 10 runs × 6 scenarios = ~300 MB

**For CI 20 runs:**
- **LTTng**: ~150 MB per run × 20 runs × 6 scenarios = ~18 GB (temporary)
- **eBPF**: ~1-5 MB per run × 20 runs × 6 scenarios = ~600 MB

**For production 50 runs:**
- **LTTng**: ~150 MB per run × 50 runs × 6 scenarios = ~45 GB (temporary)
- **eBPF**: ~1-5 MB per run × 50 runs × 6 scenarios = ~1.5 GB

**For research 100 runs:**
- **LTTng**: ~150 MB per run × 100 runs × 6 scenarios = ~90 GB (temporary)
- **eBPF**: ~1-5 MB per run × 100 runs × 6 scenarios = ~3 GB

Trace files are kept for debugging but can be cleaned up:

```bash
# Clean up old benchmark results
rm -rf benchmark_results_*/
```

---

## Credits

This comprehensive benchmark suite was created to address the discrepancy between:
- **Empty function stress test**: 83% overhead (worst case, unrealistic)
- **Real-world GPU tracing**: 0.3% overhead (realistic workload)

It demonstrates that **uprobe overhead is absolute (~5 μs), not relative**, making eBPF perfect for GPU tracing! 🚀

The statistical enhancements ensure results are trustworthy and can be used confidently for production decisions.
