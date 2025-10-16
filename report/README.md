# Pre-Generated Benchmark Report

This directory contains the official benchmark report generated on **bare metal hardware** for accurate performance results.

## Why Pre-Generated?

eBPF uprobe performance is highly sensitive to the execution environment:
- **Bare metal**: ~5-10μs overhead per uprobe (accurate)
- **VMs/Cloud**: ~100-200μs overhead per uprobe (20x worse due to virtualization)

Since GitHub Actions runs on VMs, running benchmarks in CI would produce misleading results showing eBPF as 20x slower than it actually is.

## Contents

- `index.html` - Interactive HTML report with charts (main benchmark)
- `full_app_benchmark_results.html` - Full HIP trace benchmark results including eBPF, LTTng/Exatracer, and ROCProfV3
- `results.json` - Raw benchmark data
- `GENERATION_INFO.md` - Information about when/where this was generated

## How to Update

To regenerate this report (on bare metal):

```bash
# Run full benchmark suite (10 runs, ~5-8 minutes)
# This automatically updates the report/ directory
sudo python3 scripts/benchmark.py ./build -r 10

# The script automatically copies:
# - benchmark_results_*/benchmark_report.html → report/index.html (with link banner if full_app_benchmark_results.html exists)
# - benchmark_results_*/results.json → report/results.json (if needed)
#
# Note: report/full_app_benchmark_results.html should be manually provided
# (contains full HIP trace benchmark with eBPF, LTTng/Exatracer, ROCProfV3)

# Manual update of generation info (if needed)
echo "Generated: $(date)" > report/GENERATION_INFO.md
echo "Hostname: $(hostname)" >> report/GENERATION_INFO.md
echo "Kernel: $(uname -r)" >> report/GENERATION_INFO.md
echo "CPU: $(lscpu | grep 'Model name' | cut -d: -f2 | xargs)" >> report/GENERATION_INFO.md

# Commit and push
git add report/
git commit -m "Update benchmark report"
git push
```

The CI will automatically deploy this to GitHub Pages.

## Full HIP Trace Benchmark

The `full_app_benchmark_results.html` file should be manually provided and contains:
- Complete tracing tools comparison including eBPF, LTTng/Exatracer, and ROCProfV3
- Wall-time overhead measurements for real-world HIP tracing scenarios
- Full implementation benchmark results

When this file exists, the main report automatically includes a link banner at the top.

**Deployed URLs:**
- Main report: https://ammarwa.github.io/Tracing-Techniques-Benchmarking/
- Full HIP trace benchmark: https://ammarwa.github.io/Tracing-Techniques-Benchmarking/full-hip-trace-benchmark/
