# CI/CD Pipeline Documentation

This document provides a comprehensive overview of the GitHub Actions CI/CD pipeline for the eBPF vs LTTng validation and deployment project.

## Table of Contents

- [Overview](#overview)
- [Pipeline Architecture](#pipeline-architecture)
- [Job Definitions](#job-definitions)
- [Artifacts and Deployment](#artifacts-and-deployment)
- [GitHub Pages Deployment](#github-pages-deployment)
- [Local Development](#local-development)
- [Troubleshooting](#troubleshooting)

## Overview

The CI pipeline automatically validates the build system and tracer functionality, then deploys pre-generated benchmark reports to GitHub Pages. This is a **validation CI**, not a benchmark execution CI, because eBPF performance varies dramatically between bare metal and virtualized environments.

**Pipeline Triggers:**
- Push to `main` or `master` branch
- Pull requests targeting `main` or `master`
- Manual workflow dispatch

**Total Execution Time:** ~5-8 minutes

**Why Pre-Generated Reports?**
eBPF uprobe performance is highly sensitive to the execution environment:
- **Bare Metal**: ~5-10μs overhead per uprobe (accurate)
- **VMs/Cloud**: ~100-200μs overhead per uprobe (20x worse due to virtualization)

Since GitHub Actions runs on VMs, executing benchmarks in CI would produce misleading results showing eBPF as 20x slower than it actually is in production.

## Pipeline Architecture

The pipeline consists of 3 sequential jobs:

```
┌─────────────────────────────────────────────────────────────┐
│  Job 1: build-and-validate                                  │
│  - Checkout code                                             │
│  - Install LTTng and eBPF dependencies                      │
│  - Build project (all components)                           │
│  - Run validation tests                                      │
│  Duration: ~4-6 minutes                                      │
└────────────────────────┬────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────────────┐
│  Job 2: deploy (main/master only)                           │
│  - Upload pre-generated report from report/ directory       │
│  - Deploy to GitHub Pages                                   │
│  Duration: ~1-2 minutes                                      │
└────────────────────────┬────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────────────┐
│  Job 3: summary                                             │
│  - Display workflow summary                                 │
│  - Show build and deployment status                         │
│  Duration: ~30 seconds                                       │
└─────────────────────────────────────────────────────────────┘
```

## Job Definitions

### Job 1: Build and Validate

**Purpose:** Ensure code builds correctly and tracers function properly

**Runner:** `ubuntu-latest`

**Steps:**

1. **Checkout Code**
   ```yaml
   - uses: actions/checkout@v4
   ```

2. **Install Dependencies**
   ```bash
   # LTTng dependencies
   sudo apt-get update
   sudo apt-get install -y lttng-tools liblttng-ust-dev babeltrace2
   
   # eBPF dependencies (Clang 14 for better BPF support)
   sudo apt-get install -y clang-14 llvm-14 libbpf-dev linux-headers-$(uname -r)
   sudo apt-get install -y linux-tools-generic linux-tools-$(uname -r)
   
   # Set Clang 14 as default
   sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-14 100
   ```

3. **Build Project**
   ```bash
   ./build.sh -c
   ```
   
   **What this builds:**
   - `sample_app` - Target application
   - `libmylib.so` - Sample library
   - `libmylib_lttng.so` - LTTng wrapper library
   - `mylib_tracer` - eBPF tracer (requires root)

4. **Run Validation Tests**
   ```bash
   sudo ./scripts/validate_output.sh
   ```
   
   **What this validates:**
   - LTTng tracer captures events correctly
   - eBPF tracer captures events correctly
   - Both tracers capture identical argument values
   - Event counts match expected (1000 events per tracer)

**Success Criteria:**
- Build completes without errors
- All validation tests pass
- Both LTTng and eBPF tracers functional

### Job 2: Deploy (Conditional)

**Purpose:** Deploy pre-generated benchmark report to GitHub Pages

**Condition:** Only runs on `main` or `master` branch pushes

**Runner:** `ubuntu-latest`

**Steps:**

1. **Checkout Code**
   ```yaml
   - uses: actions/checkout@v4
   ```

2. **Setup Pages**
   ```yaml
   - uses: actions/configure-pages@v4
   ```

3. **Upload Artifact**
   ```yaml
   - uses: actions/upload-pages-artifact@v3
     with:
       path: ./report
   ```
   
   **Artifact Contents:**
   - `index.html` - Interactive HTML report with charts
   - `results.json` - Raw benchmark data
   - `GENERATION_INFO.md` - Report metadata

4. **Deploy to Pages**
   ```yaml
   - uses: actions/deploy-pages@v4
   ```

**Permissions Required:**
```yaml
permissions:
  contents: read
  pages: write
  id-token: write
```

### Job 3: Summary

**Purpose:** Provide workflow status summary

**Runner:** `ubuntu-latest`

**Steps:**
- Display build status
- Show deployment status (if applicable)
- Provide links to deployed report

## Artifacts and Deployment

### Pre-Generated Report

The `report/` directory contains:

```
report/
├── index.html              # Interactive HTML report
├── results.json            # Raw benchmark data
├── README.md              # Report documentation
└── GENERATION_INFO.md     # Generation metadata
```

**Report Features:**
- Interactive Plotly charts
- 5 visualization types (overhead, timing, memory)
- Statistical confidence intervals
- Responsive design

**Generation Info Example:**
```
Generated: Fri Oct 10 04:34:28 PM CDT 2025
Hostname: smc300x-clt-r4c7-26
Kernel: 5.15.0-143-generic
CPU: AMD EPYC 9354 32-Core Processor
Platform: Bare Metal (not virtualized)
```

### GitHub Pages Configuration

**Settings:**
- Source: GitHub Actions
- Custom domain: Not configured
- HTTPS: Enforced

**URL Pattern:**
```
https://<username>.github.io/<repository>/
```

**Deployment Frequency:**
- On every push to `main`/`master`
- Manual workflow dispatch

## Local Development

### Testing the CI Pipeline Locally

**1. Validate Build Process:**
```bash
# Test the build
./build.sh -c

# Test validation
sudo ./scripts/validate_output.sh
```

**2. Preview Report Locally:**
```bash
# Serve the report locally
cd report
python3 -m http.server 8000

# Open in browser
firefox http://localhost:8000
```

**3. Simulate CI Dependencies:**
```bash
# Install same dependencies as CI
sudo apt-get install -y lttng-tools liblttng-ust-dev babeltrace2
sudo apt-get install -y clang-14 llvm-14 libbpf-dev
sudo apt-get install -y linux-tools-generic

# Use Clang 14
export CC=clang-14
./build.sh -c
```

### Manual Report Generation

**For updating the pre-generated report:**

```bash
# Generate new benchmark data on bare metal
sudo ./scripts/benchmark.py ./build --runs 20

# Copy to report directory
cp benchmark_results_*/benchmark_report.html report/index.html
cp benchmark_results_*/results.json report/results.json

# Update generation info
echo "Generated: $(date)" > report/GENERATION_INFO.md
echo "Hostname: $(hostname)" >> report/GENERATION_INFO.md
echo "Platform: Bare Metal" >> report/GENERATION_INFO.md

# Commit and push
git add report/
git commit -m "Update benchmark report with new bare metal results"
git push
```

## Troubleshooting

### Build Failures

**Problem:** Dependencies missing
```
error: lttng-ust not found
```

**Solution:** Install dependencies
```bash
sudo apt-get update
sudo apt-get install -y lttng-tools liblttng-ust-dev babeltrace2
```

**Problem:** Clang version issues
```
error: unknown target 'bpf'
```

**Solution:** Use Clang 14+
```bash
sudo apt-get install -y clang-14 llvm-14
export CC=clang-14
```

### Validation Failures

**Problem:** eBPF tracer permission denied
```
Failed to attach uprobe: Operation not permitted
```

**Solution:** Run with sudo (expected in CI)
```bash
sudo ./scripts/validate_output.sh
```

**Problem:** LTTng session errors
```
Error: Session already exists
```

**Solution:** Clean up sessions
```bash
lttng destroy -a
```

### Deployment Issues

**Problem:** Pages deployment failed
```
Error: Artifact not found
```

**Solution:** Check artifact path
```yaml
- uses: actions/upload-pages-artifact@v3
  with:
    path: ./report  # Must exist and contain index.html
```

**Problem:** Permission errors
```
Error: insufficient permissions
```

**Solution:** Check repository settings
1. Go to Settings → Pages
2. Ensure source is "GitHub Actions"
3. Check workflow permissions

### Performance Notes

**Expected CI Performance:**
- **Build time**: 3-5 minutes
- **Validation time**: 1-2 minutes
- **Deployment time**: 30-60 seconds

**Build Performance Tips:**
- Use `-j$(nproc)` for parallel building
- Install dependencies in parallel where possible
- Cache dependencies if running frequently

**Validation Performance:**
- Validation uses 1000 function calls (quick)
- Each tracer test takes ~30-60 seconds
- Total validation: ~2-3 minutes

## Integration with Development Workflow

### Pull Request Workflow

1. **Developer pushes changes**
2. **CI triggers build-and-validate job**
3. **Reports build status on PR**
4. **Deployment skipped (not main branch)**

### Main Branch Workflow

1. **Changes merged to main**
2. **CI runs full pipeline**
3. **Pre-generated report deployed to Pages**
4. **New report available at GitHub Pages URL**

### Manual Dispatch

```bash
# Trigger manually via GitHub UI
# Repository → Actions → CI → Run workflow
```

Or via GitHub CLI:
```bash
gh workflow run ci.yml
```

## Summary

This CI pipeline provides:

✅ **Build Validation**: Ensures code compiles on Ubuntu with standard dependencies
✅ **Functional Testing**: Validates both LTTng and eBPF tracers work correctly
✅ **Automated Deployment**: Publishes pre-generated reports to GitHub Pages
✅ **Development Support**: Fast feedback loop for development changes
✅ **Production Accuracy**: Uses bare metal benchmark results, not misleading VM results

**Key Difference from Benchmark CI**: This pipeline validates functionality rather than measuring performance, ensuring accurate results by using pre-generated bare metal benchmarks instead of running performance tests in virtualized CI environments.