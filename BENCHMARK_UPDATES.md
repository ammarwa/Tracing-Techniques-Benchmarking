# Benchmark Script Updates

## Summary

Updated the `benchmark.py` script with two major improvements:

1. **Fixed Empty Chart Sections** - Charts in the HTML report were not rendering due to f-string issues
2. **Added Scenario Selection** - Users can now run specific benchmark scenarios instead of all scenarios

---

## 1. Fixed Chart Rendering Issues

### Problem
The HTML benchmark reports had empty sections for:
- Resource Usage Comparison
- Absolute Timing Comparison
- Overhead by Function Duration

### Root Cause
The JavaScript data arrays were not being properly embedded in the HTML. The issue was that the final section of HTML (containing the `<script>` tag) was being concatenated using a regular string (`html += """`) instead of an f-string (`html += f"""`), so Python variables like `{js_work_us}` were treated as literal text instead of being evaluated.

### Fix
Changed line 736 in `benchmark.py` from:
```python
html += """
```

To:
```python
html += f"""
```

This ensures all Python variables in the JavaScript section are properly interpolated.

---

## 2. Added Scenario Selection Feature

### New Functionality
Users can now select specific scenarios to run instead of always running all 6 scenarios.

### Usage Examples

**List available scenarios:**
```bash
python3 scripts/benchmark.py --list-scenarios
```

**Run specific scenarios (e.g., only longer-duration functions):**
```bash
python3 scripts/benchmark.py ./build --scenarios 3 4 5
```

**Run single scenario with more repetitions:**
```bash
python3 scripts/benchmark.py ./build -s 0 -r 50
```

**Run multiple scenarios with fewer repetitions for quick testing:**
```bash
python3 scripts/benchmark.py ./build --scenarios 2 3 --runs 5
```

### Available Scenarios
- **[0]** Empty Function (0 μs, 1M iterations)
- **[1]** 5 μs Function (100K iterations)
- **[2]** 50 μs Function (50K iterations)
- **[3]** 100 μs Function (10K iterations)
- **[4]** 500 μs Function (5K iterations)
- **[5]** 1000 μs (1ms) Function (2K iterations)

### Implementation Details

**Modified `BenchmarkSuite.__init__()`:**
- Added `scenario_indices` parameter
- Filters `ALL_SCENARIOS` based on user selection
- Validates scenario indices

**Updated `main()` function:**
- Added `--scenarios` (`-s`) argument to accept list of scenario indices
- Added `--list-scenarios` flag to display all available scenarios
- Added validation for scenario indices
- Updated time estimates based on selected scenarios

---

## 3. New Regenerate Report Script

### Purpose
Allow regenerating HTML reports from existing `results.json` files without re-running benchmarks. Useful for:
- Applying chart fixes to old results
- Updating report styling
- Re-generating reports after visualization improvements

### Usage

**Regenerate from results directory:**
```bash
python3 scripts/regenerate_report.py benchmark_results_20251009_174347
```

**Regenerate from specific results.json file:**
```bash
python3 scripts/regenerate_report.py path/to/results.json
```

**Regenerate to different output directory:**
```bash
python3 scripts/regenerate_report.py results.json -o new_report_dir
```

### Script Location
`scripts/regenerate_report.py`

---

## Benefits

### Time Savings
- **Targeted testing**: Run only relevant scenarios (e.g., skip nanosecond functions if you're only interested in microsecond-scale overhead)
- **Quick iterations**: Test specific scenarios with fewer runs during development
- **Faster debugging**: Focus on problematic scenarios

### Example Time Savings
- Full benchmark (6 scenarios × 10 runs): ~4-6 minutes
- Single scenario (1 scenario × 10 runs): ~40-60 seconds
- Quick test (3 scenarios × 5 runs): ~1-2 minutes

### Flexibility
- Run comprehensive tests on all scenarios for final reports
- Run quick sanity checks during development
- Focus on specific performance ranges (e.g., only GPU-relevant timings)

---

## Testing

To verify the fixes work:

1. **Test scenario selection:**
   ```bash
   python3 scripts/benchmark.py --list-scenarios
   python3 scripts/benchmark.py ./build -s 5 -r 2
   ```

2. **Verify charts render** (once benchmark completes):
   - Open the generated `benchmark_report.html`
   - Verify all three charts appear:
     - "Overhead by Function Duration"
     - "Absolute Timing Comparison"
     - "Resource Usage Comparison"

3. **Test report regeneration:**
   ```bash
   python3 scripts/regenerate_report.py <results_directory>
   ```

---

## Backward Compatibility

All changes are backward compatible:
- Running `python3 scripts/benchmark.py ./build` without `--scenarios` runs all scenarios (original behavior)
- Existing `results.json` files can be used with the new regeneration script
- HTML report format remains the same, just with working charts

---

## Files Modified

1. **scripts/benchmark.py**
   - Fixed f-string issue on line 736
   - Added `scenario_indices` parameter to `BenchmarkSuite.__init__()`
   - Moved scenarios to class-level `ALL_SCENARIOS` attribute
   - Updated `main()` with new argument parsing
   - Added scenario selection and validation logic

2. **scripts/regenerate_report.py** (NEW)
   - Standalone script to regenerate HTML reports from existing results

---

## Future Enhancements

Potential improvements for future versions:
- Add scenario selection by duration range (e.g., `--duration-range 50-500`)
- Add method selection (e.g., run only eBPF, skip LTTng)
- Support for custom scenarios via config file
- Parallel execution of independent scenarios
- Progress bars for long-running benchmarks
