# Pre-Generated Benchmark Report

This directory contains the official benchmark report generated on **bare metal hardware** for accurate performance results.

## Why Pre-Generated?

eBPF uprobe performance is highly sensitive to the execution environment:
- **Bare metal**: ~5-10μs overhead per uprobe (accurate)
- **VMs/Cloud**: ~100-200μs overhead per uprobe (20x worse due to virtualization)

Since GitHub Actions runs on VMs, running benchmarks in CI would produce misleading results showing eBPF as 20x slower than it actually is.

## Contents

- `index.html` - Interactive HTML report with charts
- `results.json` - Raw benchmark data
- `GENERATION_INFO.md` - Information about when/where this was generated

## How to Update

To regenerate this report (on bare metal):

```bash
# Run full benchmark suite (10 runs, ~5-8 minutes)
sudo python3 scripts/benchmark.py ./build -r 10

# Copy results to report folder
cp benchmark_results_*/benchmark_report.html report/index.html
cp benchmark_results_*/results.json report/results.json

# Update generation info
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
