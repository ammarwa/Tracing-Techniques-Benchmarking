# CI/CD Pipeline Documentation

This document provides a comprehensive overview of the GitHub Actions CI/CD pipeline for the eBPF vs LTTng benchmark project.

## Table of Contents

- [Overview](#overview)
- [Pipeline Architecture](#pipeline-architecture)
- [Job Definitions](#job-definitions)
- [Parallel Execution Strategy](#parallel-execution-strategy)
- [Artifacts and Retention](#artifacts-and-retention)
- [GitHub Pages Deployment](#github-pages-deployment)
- [Performance Characteristics](#performance-characteristics)
- [Local Development](#local-development)
- [Troubleshooting](#troubleshooting)

## Overview

The CI pipeline automatically runs comprehensive benchmarks comparing eBPF and LTTng tracing overhead across multiple scenarios. The pipeline is optimized for speed using parallel execution while maintaining statistical reliability with 20 runs per scenario.

**Pipeline Triggers:**
- Push to `main` or `master` branch
- Pull requests targeting `main` or `master`
- Manual workflow dispatch

**Total Execution Time:** ~12-15 minutes (down from ~40 minutes sequential)

## Pipeline Architecture

The pipeline consists of 5 sequential stages with parallel execution in the benchmark stage:

```
┌─────────────────────────────────────────────────────────────┐
│  Stage 1: Build and Validate (Sequential)                   │
│  - Checkout code                                             │
│  - Install dependencies                                      │
│  - Build project                                             │
│  - Run validation tests                                      │
│  Duration: ~5 minutes                                        │
└────────────────────────┬────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────────────┐
│  Stage 2: Benchmark Scenarios (Parallel - Matrix)           │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ Scenario 0   │  │ Scenario 1   │  │ Scenario 2   │      │
│  │ (0μs empty)  │  │ (5μs)        │  │ (50μs)       │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │ Scenario 3   │  │ Scenario 4   │  │ Scenario 5   │      │
│  │ (100μs)      │  │ (500μs)      │  │ (1000μs)     │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│  Duration: ~6-8 minutes (longest scenario)                  │
└────────────────────────┬────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────────────┐
│  Stage 3: Combine and Report (Sequential)                   │
│  - Download all scenario artifacts                          │
│  - Combine results.json files                               │
│  - Generate unified HTML report                             │
│  - Prepare GitHub Pages content                             │
│  Duration: ~1 minute                                         │
└────────────────────────┬────────────────────────────────────┘
                         ↓
                    ┌────┴─────┐
                    ↓          ↓
┌─────────────────────────┐  ┌────────────────────────────────┐
│ Stage 4: Deploy         │  │ Stage 5: Summary               │
│ - Deploy to Pages       │  │ - Download results             │
│ Duration: ~30 seconds   │  │ - Create job summary           │
│ (main/master only)      │  │ Duration: ~10 seconds          │
└─────────────────────────┘  └────────────────────────────────┘
```

## Job Definitions

### Job 1: `build-and-validate`

**Purpose:** Build the project and validate functionality before running benchmarks.

**Steps:**
1. **Checkout code** - Uses `actions/checkout@v4`
2. **Install dependencies** - Installs system packages and Python dependencies
   - CMake, build tools
   - Clang-14, LLVM-14
   - libbpf-dev, linux-headers
   - LTTng tools and libraries
   - Python 3 with plotly, pandas, numpy
3. **Build project** - Runs `./build.sh -c` with clang-14
4. **Run validation tests** - Executes `./scripts/validate_output.sh`

**Environment:**
- Runner: `ubuntu-latest`
- Compiler: `clang-14`
- Requires: `sudo` for validation tests

**Success Criteria:** All tests must pass before benchmarks start.

---

### Job 2: `benchmark` (Matrix Strategy)

**Purpose:** Run benchmark scenarios in parallel to reduce total execution time.

**Matrix Configuration:**
```yaml
strategy:
  fail-fast: false  # Continue other scenarios if one fails
  matrix:
    scenario: [0, 1, 2, 3, 4, 5]
```

**Scenario Mapping:**

| Matrix Value | Name | Description | Work Duration | Iterations |
|-------------|------|-------------|---------------|------------|
| 0 | `0us-empty` | Empty Function | 0 μs | 1,000,000 |
| 1 | `5us` | Ultra-fast API | 5 μs | 100,000 |
| 2 | `50us` | Fast API | 50 μs | 50,000 |
| 3 | `100us` | Typical API | 100 μs | 10,000 |
| 4 | `500us` | Medium API | 500 μs | 5,000 |
| 5 | `1000us` | Slow API | 1000 μs (1ms) | 2,000 |

**Steps per Matrix Job:**
1. **Checkout code**
2. **Install dependencies** (same as build job)
3. **Build project** (cached by GitHub Actions)
4. **Set CPU governor** - Attempts to set performance mode
5. **Run benchmark** - Executes single scenario with 20 runs
   ```bash
   sudo python3 scripts/benchmark.py ./build --runs 20 --scenarios ${{ matrix.scenario }}
   ```
6. **Upload scenario results** - Uploads `results.json` as artifact

**Artifact Naming:**
- `benchmark-results-scenario-0us-empty`
- `benchmark-results-scenario-5us`
- `benchmark-results-scenario-50us`
- `benchmark-results-scenario-100us`
- `benchmark-results-scenario-500us`
- `benchmark-results-scenario-1000us`

**Timeout:** 60 minutes per scenario

**Key Features:**
- `fail-fast: false` ensures one failed scenario doesn't stop others
- Each scenario runs 3 methods (baseline, LTTng, eBPF) × 20 repetitions
- Produces individual `results.json` file per scenario

---

### Job 3: `combine-and-report`

**Purpose:** Combine parallel results and generate unified HTML report.

**Dependencies:** Requires all `benchmark` matrix jobs to complete.

**Steps:**
1. **Checkout code** - Get scripts for combining and reporting
2. **Install Python dependencies** - plotly, pandas, numpy
3. **Download all scenario results** - Uses pattern matching
   ```yaml
   pattern: benchmark-results-scenario-*
   path: scenario_results/
   ```
4. **List downloaded artifacts** - Debug output
5. **Combine results** - Merge all `results.json` files
   ```bash
   python3 scripts/combine_results.py \
     --input-dir scenario_results \
     --output combined_results/results.json \
     --validate
   ```
6. **Generate combined HTML report** - Create unified visualization
   ```bash
   python3 scripts/regenerate_report.py \
     combined_results/results.json \
     --output-dir combined_results
   ```
7. **Upload combined results** - Single artifact with all data
8. **Prepare GitHub Pages content** - For deployment (main/master only)

**Validation Checks:**
- ✅ All 6 scenarios present
- ✅ All 3 methods per scenario (baseline, LTTng, eBPF)
- ✅ Expected total: 18 results (6 scenarios × 3 methods)
- ✅ JSON structure matches `BenchmarkResult` schema

**Artifacts Produced:**
- `benchmark-results-combined` (90 days retention)
  - `results.json` - Combined raw data
  - `benchmark_report.html` - Interactive HTML report

---

### Job 4: `deploy`

**Purpose:** Deploy combined benchmark report to GitHub Pages.

**Dependencies:** Requires `combine-and-report` to complete.

**Conditions:**
- Only runs on `main` or `master` branch
- Skipped for pull requests and feature branches

**Steps:**
1. **Deploy to GitHub Pages** - Uses `actions/deploy-pages@v4`

**Pages Content:**
- `index.html` - Combined benchmark report
- `results.json` - Raw combined data
- `README.md` - Landing page with metadata

**URL:** `https://{owner}.github.io/{repository}/`

**Permissions Required:**
```yaml
permissions:
  contents: read
  pages: write
  id-token: write
```

---

### Job 5: `summary`

**Purpose:** Create GitHub Actions job summary with links and statistics.

**Dependencies:** Requires `build-and-validate` and `combine-and-report`.

**Conditions:** Always runs (`if: always()`)

**Steps:**
1. **Download combined results** - Get final artifact
2. **Create job summary** - Generate markdown summary

**Summary Content:**
- ✅ Build, validation, and benchmark status
- 📊 Link to artifacts
- 🌐 Link to GitHub Pages (if deployed)
- ⚙️ Configuration details (parallel execution, runs, scenarios)

**Example Output:**
```markdown
## eBPF vs LTTng Benchmark CI Results

### Build Status: ✅ Complete
### Validation Status: ✅ Passed
### Benchmark Status: ✅ Complete (Parallel Execution)

### Quick Results
- 📊 View Full Interactive Report
- 🌐 View GitHub Pages Report

### Configuration
- Execution Mode: Parallel (6 jobs)
- Runs per scenario: 20
- Scenarios tested: 6 (0μs, 5μs, 50μs, 100μs, 500μs, 1000μs)
- Methods compared: Baseline, LTTng, eBPF
```

## Parallel Execution Strategy

### Why Parallel Execution?

**Problem:** Sequential execution of all scenarios takes ~40 minutes.

**Solution:** Split scenarios into parallel matrix jobs.

**Benefits:**
- ⚡ **60-70% time reduction** (~12-15 min vs ~40 min)
- 🔄 **Better resource utilization** - Multiple GitHub runners
- 🛡️ **Fault isolation** - Failed scenario doesn't block others
- 📈 **Scalability** - Easy to add more scenarios

### Matrix Strategy Explained

GitHub Actions matrix strategy creates multiple jobs from a single definition:

```yaml
strategy:
  fail-fast: false
  matrix:
    scenario: [0, 1, 2, 3, 4, 5]
```

This creates 6 parallel jobs, each with `matrix.scenario` set to 0-5.

**Key Configuration:**
- `fail-fast: false` - If scenario 2 fails, scenarios 0, 1, 3, 4, 5 continue
- Each job is independent with its own VM
- GitHub Actions runs up to 20 jobs in parallel (free tier)

### Result Combination

After parallel execution, results are combined using `scripts/combine_results.py`:

**Input:**
```
scenario_results/
├── benchmark-results-scenario-0us-empty/
│   └── results.json (3 results: baseline, lttng, ebpf)
├── benchmark-results-scenario-5us/
│   └── results.json (3 results)
├── benchmark-results-scenario-50us/
│   └── results.json (3 results)
├── benchmark-results-scenario-100us/
│   └── results.json (3 results)
├── benchmark-results-scenario-500us/
│   └── results.json (3 results)
└── benchmark-results-scenario-1000us/
    └── results.json (3 results)
```

**Output:**
```
combined_results/
├── results.json (18 results, sorted)
└── benchmark_report.html (unified report)
```

**Sorting:** Results are sorted by work duration and method for consistency.

## Artifacts and Retention

### Individual Scenario Artifacts

**Name Pattern:** `benchmark-results-scenario-{name}`

**Retention:** 30 days

**Contents:** Single `results.json` file with 3 results (baseline, LTTng, eBPF)

**Use Cases:**
- Debug individual scenario issues
- Compare scenario-level results
- Re-run specific scenarios locally

### Combined Artifacts

**Name:** `benchmark-results-combined`

**Retention:** 90 days (longer for analysis)

**Contents:**
- `results.json` - All 18 combined results
- `benchmark_report.html` - Unified interactive report

**Use Cases:**
- Download complete benchmark results
- Offline report viewing
- Historical comparison

### Artifact Download

**Via GitHub UI:**
1. Navigate to Actions tab
2. Click on workflow run
3. Scroll to "Artifacts" section
4. Download desired artifact

**Via GitHub CLI:**
```bash
# List artifacts
gh run list --workflow=benchmark-ci.yml

# Download specific artifact
gh run download <run-id> -n benchmark-results-combined
```

## GitHub Pages Deployment

### Configuration

The repository must have GitHub Pages enabled:

1. **Settings** → **Pages**
2. **Source:** GitHub Actions
3. **Branch:** Automatic (managed by workflow)

### Deployment Process

**Trigger:** Only on `main` or `master` branch push

**Steps:**
1. Combine job prepares `gh-pages/` directory
2. Copies `benchmark_report.html` → `index.html`
3. Copies `results.json`
4. Creates `README.md` with metadata
5. Upload using `actions/upload-pages-artifact@v3`
6. Deploy job publishes to Pages

### Published Content

**URL:** `https://{owner}.github.io/{repository}/`

**Files:**
- `index.html` - Interactive benchmark report with charts
- `results.json` - Raw combined data for analysis
- `README.md` - Landing page description

**Update Frequency:** On every push to main/master (after successful benchmark)

### Viewing the Report

**Direct Link:**
```
https://{owner}.github.io/{repository}/
```

**From CI Summary:**
- Click on workflow run
- Look for "🌐 View GitHub Pages Report" link

## Performance Characteristics

### Execution Time Breakdown

| Stage | Sequential | Parallel | Savings |
|-------|-----------|----------|---------|
| Build & Validate | 5 min | 5 min | 0% |
| Scenario 0 (0μs) | 6 min | - | - |
| Scenario 1 (5μs) | 5 min | - | - |
| Scenario 2 (50μs) | 5 min | - | - |
| Scenario 3 (100μs) | 4 min | - | - |
| Scenario 4 (500μs) | 3 min | - | - |
| Scenario 5 (1000μs) | 2 min | - | - |
| **All Scenarios** | **30 min** | **~8 min** | **73%** |
| Combine & Report | N/A | 1 min | - |
| Deploy | 1 min | 1 min | 0% |
| **TOTAL** | **~36 min** | **~15 min** | **58%** |

### Resource Usage

**GitHub Actions Free Tier:**
- 2,000 minutes/month for private repos
- Unlimited for public repos
- Up to 20 concurrent jobs

**This Pipeline:**
- Uses ~15 minutes per run
- Runs 6 parallel jobs (well under limit)
- Efficient use of free tier

### Statistical Reliability

**Runs per Scenario:** 20 repetitions

**Statistical Measures:**
- Mean (average time per call)
- Standard deviation
- Min/Max values
- 95% confidence interval

**Quality:** Parallel execution maintains same statistical quality as sequential.

## Local Development

### Running Full Benchmark Locally

```bash
# Sequential (original)
sudo python3 scripts/benchmark.py ./build --runs 20

# Specific scenarios
sudo python3 scripts/benchmark.py ./build --runs 20 --scenarios 0 1 2

# Single scenario (faster testing)
sudo python3 scripts/benchmark.py ./build --runs 5 --scenarios 3
```

### Simulating Parallel Execution Locally

**Terminal 1: Scenario 0**
```bash
sudo python3 scripts/benchmark.py ./build --runs 20 --scenarios 0
```

**Terminal 2: Scenario 1**
```bash
sudo python3 scripts/benchmark.py ./build --runs 20 --scenarios 1
```

**... (repeat for other scenarios)**

**Combine Results:**
```bash
# Create output directory
mkdir -p combined_results

# Combine all results.json files
python3 scripts/combine_results.py \
  --input-dir . \
  --output combined_results/results.json \
  --validate

# Generate combined report
python3 scripts/regenerate_report.py \
  combined_results/results.json \
  --output-dir combined_results

# Open report
firefox combined_results/benchmark_report.html
```

### Testing CI Changes

**Test workflow syntax:**
```bash
# Install yamllint
pip install yamllint

# Validate YAML
yamllint .github/workflows/benchmark-ci.yml
```

**Test Python scripts:**
```bash
# Test combine script
python3 scripts/combine_results.py --help

# Test regenerate script
python3 scripts/regenerate_report.py --help
```

**Dry-run workflow locally:**
```bash
# Install act (https://github.com/nektos/act)
brew install act  # macOS
# OR
curl https://raw.githubusercontent.com/nektos/act/master/install.sh | sudo bash

# Run workflow locally
act push -W .github/workflows/benchmark-ci.yml
```

## Troubleshooting

### Common Issues

#### Issue: Scenario job fails with timeout

**Symptoms:**
```
Error: The operation was canceled.
```

**Causes:**
- Scenario taking longer than 60 minutes
- System resources exhausted

**Solutions:**
1. Increase timeout: `timeout-minutes: 120`
2. Reduce runs: `--runs 10` instead of 20
3. Check for hanging eBPF tracer processes

#### Issue: Combine job fails - missing artifacts

**Symptoms:**
```
Warning: File not found: scenario_results/benchmark-results-scenario-X/results.json
```

**Causes:**
- One or more scenario jobs failed
- Artifact upload failed

**Solutions:**
1. Check individual scenario job logs
2. Re-run failed scenarios
3. Use `if: always()` on upload steps

#### Issue: HTML report not generated

**Symptoms:**
```
Error: Could not find results.json at combined_results/results.json
```

**Causes:**
- Combine script failed
- Invalid JSON in results files

**Solutions:**
1. Check combine job logs
2. Validate individual `results.json` files:
   ```bash
   python3 -c "import json; json.load(open('results.json'))"
   ```
3. Run combine script with `--validate` flag

#### Issue: Pages deployment fails

**Symptoms:**
```
Error: Pages deployment failed
```

**Causes:**
- GitHub Pages not enabled
- Wrong branch configuration
- Permission issues

**Solutions:**
1. Enable Pages in repository settings
2. Verify permissions in workflow:
   ```yaml
   permissions:
     pages: write
     id-token: write
   ```
3. Check Pages source is set to "GitHub Actions"

### Debug Mode

Enable debug logging in GitHub Actions:

1. **Repository Settings** → **Secrets and variables** → **Actions**
2. Add repository variable:
   - Name: `ACTIONS_RUNNER_DEBUG`
   - Value: `true`
3. Add repository variable:
   - Name: `ACTIONS_STEP_DEBUG`
   - Value: `true`

This enables detailed logs for debugging workflow issues.

### Getting Help

1. **Check workflow logs** - Review each step's output
2. **Download artifacts** - Inspect results files locally
3. **Review validation output** - Check `validate_output.sh` logs
4. **Test locally** - Reproduce issue on your machine
5. **Open an issue** - Provide workflow run ID and error logs

## Best Practices

### When to Use Parallel Execution

✅ **Use parallel execution when:**
- Running full benchmark suite (all scenarios)
- CI/CD automation
- Time is critical (PR checks, releases)
- Multiple independent scenarios

❌ **Use sequential execution when:**
- Debugging specific scenario
- Resource-constrained environment
- Testing changes to benchmark framework
- Need simplified logs

### Optimizing CI Performance

**Caching:**
- GitHub Actions automatically caches dependencies
- Build artifacts are cached between matrix jobs

**Resource Management:**
- Clean up LTTng trace directories after data extraction
- Don't write eBPF traces to disk during benchmarking
- Use `continue-on-error: true` for optional steps

**Matrix Optimization:**
- Keep scenarios independent (no shared state)
- Balance scenario duration (longest determines total time)
- Use `fail-fast: false` for fault tolerance

## Related Documentation

- [Benchmark Design](BENCHMARK.md) - Benchmark methodology and scenarios
- [eBPF Implementation](EBPF_DESIGN.md) - eBPF tracer design
- [LTTng Implementation](LTTNG_DESIGN.md) - LTTng tracer design
- [Validation Tests](VALIDATION.md) - Test suite documentation

## Changelog

### Version 2.0 - Parallel Execution (Current)
- Added matrix strategy for parallel scenario execution
- Created `scripts/combine_results.py` for result aggregation
- Updated GitHub Pages deployment to use combined report
- Reduced total execution time by ~60%
- Added validation checks for combined results

### Version 1.0 - Sequential Execution (Legacy)
- Single job running all scenarios sequentially
- Direct HTML report generation
- ~40 minute execution time
