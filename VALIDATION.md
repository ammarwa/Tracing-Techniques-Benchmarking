# Trace Validation Script Documentation

## Overview

The `validate_output.sh` script is an **automated correctness test** that verifies both LTTng and eBPF tracers capture the same function calls with identical argument values. This ensures functional equivalence before comparing performance.

## Purpose

**Why validation is critical:**

1. **Correctness First**: Before comparing overhead, we must verify both tracers work correctly
2. **Event Count Verification**: Ensures no events are dropped or duplicated
3. **Argument Capture**: Validates that function arguments are correctly captured
4. **Functional Equivalence**: Proves both methods trace the same data (just with different overhead)

## How It Works

### Execution Flow

```
┌─────────────────────────────────────────────────────────┐
│  1. Build Detection                                     │
│     • Find sample_app, libmylib_lttng.so, mylib_tracer │
│     • Auto-detect build/ or current directory          │
└─────────────────┬───────────────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────────────┐
│  2. Run LTTng Trace                                     │
│     • Create session: /tmp/lttng_validation_$$          │
│     • Enable events: mylib:*                            │
│     • Run: LD_PRELOAD=libmylib_lttng.so sample_app 1000 │
│     • Convert to text: babeltrace2 → lttng_text.txt     │
│     • Count events: grep entry/exit                     │
└─────────────────┬───────────────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────────────┐
│  3. Run eBPF Trace                                      │
│     • Start tracer: mylib_tracer /tmp/ebpf_trace.txt    │
│     • Run: sample_app 1000                              │
│     • Stop tracer: kill -INT                            │
│     • Count events: grep entry/exit                     │
└─────────────────┬───────────────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────────────┐
│  4. Compare Results                                     │
│     • Event counts: LTTng vs eBPF vs expected (1000)    │
│     • Argument values: arg1=42, arg2=0xDEADBEEF, etc.   │
│     • String capture: arg3="test_string"                │
└─────────────────┬───────────────────────────────────────┘
                  │
                  ↓
┌─────────────────────────────────────────────────────────┐
│  5. Report Results                                      │
│     • ✓ PASS: All validations passed                    │
│     • ✗ FAIL: Show mismatches and errors               │
│     • Exit code: 0=success, 1=failure                   │
└─────────────────────────────────────────────────────────┘
```

## Usage

### Basic Usage

```bash
sudo ./validate_output.sh
```

**Why sudo?**
- eBPF uprobe attachment requires root privileges
- LTTng can run as regular user, but script runs both

### Example Output (Success)

```
========================================
eBPF vs LTTng Output Validation
========================================
Testing with 1000 iterations

========================================
Running LTTng Trace
========================================
Running application with LTTng...
✓ LTTng trace captured
  Entry events: 1000
  Exit events:  1000

========================================
Running eBPF Trace
========================================
Starting eBPF tracer...
Running application with eBPF...
✓ eBPF trace captured
  Entry events: 1000
  Exit events:  1000

========================================
Validating Event Counts
========================================
✓ LTTng captured all 1000 entry events
✓ eBPF captured all 1000 entry events
✓ LTTng captured all 1000 exit events
✓ eBPF captured all 1000 exit events
✓ Entry event counts match between LTTng and eBPF
✓ Exit event counts match between LTTng and eBPF

========================================
Validating Argument Values
========================================
Sample LTTng entry event:
[12:34:56.123456789] (+0.000000000) hostname mylib:my_traced_function_entry: { arg1 = 42, arg2 = 3735928559, arg3 = "test_string", arg4 = 3.14159, arg5 = 0x12345678 }

Sample eBPF entry event:
[1728567890.123456789] mylib:my_traced_function_entry: { arg1 = 42, arg2 = 3735928559, arg3_ptr = 0x7ffd12345678, arg5 = 0x12345678 }

✓ arg1 correct in both traces (42)
✓ arg2 correct in both traces (3735928559 = 0xDEADBEEF)
✓ arg3 correct in both traces ("test_string")

========================================
Validation Summary
========================================
✓ ALL VALIDATIONS PASSED!

Summary:
  ✓ Both tracers captured exactly 1000 events
  ✓ Event counts match between LTTng and eBPF
  ✓ Argument values are correct and match
  ✓ Both tracing methods are functionally equivalent

========================================
Cleaning Up
========================================
✓ Temporary files cleaned up

========================================
Validation Complete
========================================
```

### Example Output (Failure)

```
========================================
Validating Event Counts
========================================
✓ LTTng captured all 1000 entry events
✗ eBPF entry count mismatch: expected 1000, got 950
...
========================================
Validation Summary
========================================
✗ VALIDATION FAILED

Please review the errors above and check:
  1. Build was successful
  2. Both tracers are working correctly
  3. Application is calling the library correctly
```

## Test Parameters

### Configurable Constants

```bash
NUM_ITERATIONS=1000                          # Number of function calls
LTTNG_SESSION="validation_test_$$"          # LTTng session name (PID-unique)
LTTNG_TRACE_DIR="/tmp/lttng_validation_$$"  # LTTng output directory
EBPF_TRACE_FILE="/tmp/ebpf_validation_$$.txt"  # eBPF output file
```

**Why 1000 iterations?**
- Fast enough for quick validation (~1-2 seconds)
- Large enough to detect event loss patterns
- Moderate trace file sizes (~50 KB each)

**Why PID-based names (`$$`)?**
- Allows multiple concurrent validation runs
- Prevents conflicts between users/sessions
- Auto-cleanup doesn't affect other tests

## Validation Checks

### 1. Event Count Validation

**What it checks:**
- LTTng captured exactly `NUM_ITERATIONS` entry events
- LTTng captured exactly `NUM_ITERATIONS` exit events
- eBPF captured exactly `NUM_ITERATIONS` entry events
- eBPF captured exactly `NUM_ITERATIONS` exit events
- LTTng and eBPF entry counts match
- LTTng and eBPF exit counts match

**Implementation:**
```bash
LTTNG_ENTRY_COUNT=$(grep "my_traced_function_entry" $LTTNG_TEXT | wc -l)
EBPF_ENTRY_COUNT=$(grep "my_traced_function_entry" $EBPF_TRACE_FILE | wc -l)

if [ "$LTTNG_ENTRY_COUNT" -eq "$NUM_ITERATIONS" ]; then
    print_success "LTTng captured all $NUM_ITERATIONS entry events"
else
    print_error "LTTng entry count mismatch"
    VALIDATION_PASSED=false
fi
```

**Detects:**
- Event drops (buffer overflow, ring buffer full)
- Event duplication (double tracing)
- Tracer attachment failures (no events)

### 2. Argument Value Validation

**What it checks:**
- `arg1 = 42` (int)
- `arg2 = 3735928559` (uint64_t, hex: 0xDEADBEEF)
- `arg3 = "test_string"` (const char*)

**Implementation:**
```bash
# Extract first entry event
LTTNG_FIRST=$(grep "my_traced_function_entry" $LTTNG_TEXT | head -1)
EBPF_FIRST=$(grep "my_traced_function_entry" $EBPF_TRACE_FILE | head -1)

# Parse arg1
LTTNG_ARG1=$(echo "$LTTNG_FIRST" | grep -oP 'arg1 = \K[0-9]+')
EBPF_ARG1=$(echo "$EBPF_FIRST" | grep -oP 'arg1 = \K-?[0-9]+')

# Validate
if [ "$LTTNG_ARG1" = "42" ] && [ "$EBPF_ARG1" = "42" ]; then
    print_success "arg1 correct in both traces (42)"
fi
```

**Detects:**
- Incorrect register mapping (wrong argument captured)
- Endianness issues (byte order)
- String capture failures (pointer vs value)
- Type conversion errors (int vs uint64_t)

## Build Detection

The script auto-detects the build directory:

```bash
if [ -d "bin" ] && [ -d "lib" ]; then
    # Running from build directory
    SAMPLE_APP="./bin/sample_app"
    MYLIB_LTTNG="./lib/libmylib_lttng.so"
    MYLIB_TRACER="./bin/mylib_tracer"
elif [ -d "build/bin" ] && [ -d "build/lib" ]; then
    # Running from project root
    SAMPLE_APP="./build/bin/sample_app"
    MYLIB_LTTNG="./build/lib/libmylib_lttng.so"
    MYLIB_TRACER="./build/bin/mylib_tracer"
else
    print_error "Could not find build artifacts"
    exit 1
fi
```

**Supports two layouts:**
1. **From project root**: `./build/bin/sample_app`
2. **From build directory**: `./bin/sample_app`

## Cleanup

**Automatic cleanup** removes temporary files:

```bash
rm -rf $LTTNG_TRACE_DIR          # /tmp/lttng_validation_12345/
rm -f $LTTNG_TEXT                # /tmp/lttng_text_12345.txt
rm -f $EBPF_TRACE_FILE           # /tmp/ebpf_validation_12345.txt
```

**Runs even on failure** (always cleans up)

## Exit Codes

| Exit Code | Meaning |
|-----------|---------|
| `0` | All validations passed |
| `1` | One or more validations failed |

**Usage in CI/CD:**
```bash
#!/bin/bash
sudo ./validate_output.sh
if [ $? -eq 0 ]; then
    echo "✓ Validation passed, proceeding to benchmarks..."
    ./benchmark.py ./build
else
    echo "✗ Validation failed, aborting!"
    exit 1
fi
```

## Troubleshooting

### Permission Denied

**Problem:**
```
✗ This script needs to be run as root for eBPF tracing
```

**Solution:**
```bash
sudo ./validate_output.sh
```

### Build Artifacts Not Found

**Problem:**
```
✗ Could not find build artifacts
Please build first with: ./build.sh
```

**Solution:**
```bash
./build.sh -c
sudo ./validate_output.sh
```

### eBPF Tracer Failed to Start

**Problem:**
```
✗ eBPF tracer failed to start
```

**Possible causes:**
1. **Library not found**: Check `ldd ./build/bin/mylib_tracer`
2. **BPF not enabled**: Check `CONFIG_BPF=y` in kernel config
3. **Permissions**: Ensure running as root

**Debug:**
```bash
sudo ./build/bin/mylib_tracer /tmp/test.txt
# Should print: "Using library: ..." and "Successfully attached uprobes..."
```

### Event Count Mismatch

**Problem:**
```
✗ eBPF entry count mismatch: expected 1000, got 950
```

**Possible causes:**
1. **Ring buffer overflow**: Events dropped due to buffer full
   - **Solution**: Increase ring buffer size in `mylib_tracer.bpf.c`

2. **Tracer startup delay**: App started before tracer attached
   - **Solution**: Increase `sleep 2` in script after starting tracer

3. **Session buffer overflow** (LTTng):
   - **Solution**: Increase subbuf size: `lttng enable-channel -u --subbuf-size=8M`

### Argument Value Mismatch

**Problem:**
```
✗ arg1 mismatch: LTTng=42, eBPF=0, expected=42
```

**Possible causes:**
1. **Incorrect register mapping**: eBPF reading wrong register
   - **Fix**: Check `PT_REGS_PARM1(ctx)` in `mylib_tracer.bpf.c`

2. **Calling convention mismatch**: x86-64 vs other architecture
   - **Fix**: Ensure architecture matches (x86-64 expected)

3. **Compiler optimization**: Arguments optimized away
   - **Fix**: Add `__attribute__((noinline))` to `my_traced_function()`

## Integration with Testing

### Continuous Integration

```yaml
# .github/workflows/test.yml
name: Validation Tests

on: [push, pull_request]

jobs:
  validate:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2

      - name: Install Dependencies
        run: |
          sudo apt-get install -y lttng-tools liblttng-ust-dev \
            babeltrace2 clang llvm libbpf-dev bpftool

      - name: Build
        run: ./build.sh -c

      - name: Run Validation
        run: sudo ./validate_output.sh

      - name: Run Benchmarks (if validation passes)
        run: sudo ./benchmark.py ./build
```

### Pre-Benchmark Check

```bash
#!/bin/bash
# run_benchmarks.sh

# Validate first
sudo ./validate_output.sh
if [ $? -ne 0 ]; then
    echo "❌ Validation failed, aborting benchmarks"
    exit 1
fi

# Validation passed, run benchmarks
echo "✅ Validation passed, starting benchmarks..."
sudo ./benchmark.py ./build
```

## Advanced Usage

### Custom Iteration Count

Edit the script to change iterations:

```bash
# Line 56
NUM_ITERATIONS=10000  # Test with 10K iterations
```

Or make it a command-line argument:

```bash
#!/bin/bash
NUM_ITERATIONS=${1:-1000}  # Default 1000, or first arg

sudo ./validate_output.sh 5000  # Run with 5000 iterations
```

### Save Traces for Inspection

Disable cleanup to inspect traces:

```bash
# Comment out line 286-288
# rm -rf $LTTNG_TRACE_DIR
# rm -f $LTTNG_TEXT
# rm -f $EBPF_TRACE_FILE

sudo ./validate_output.sh

# Traces remain in /tmp/lttng_validation_* and /tmp/ebpf_validation_*
cat /tmp/ebpf_validation_*.txt | head -20
babeltrace2 /tmp/lttng_validation_*/ | head -20
```

### Verbose Mode

Add `-x` for debug output:

```bash
#!/bin/bash
set -ex  # Add -x for debug output
```

Shows every command executed:

```
+ lttng create validation_test_12345
+ lttng enable-event -u mylib:*
+ LD_PRELOAD=./build/lib/libmylib_lttng.so ./build/bin/sample_app 1000
...
```

## Summary

The validation script ensures:

✅ **Correctness**: Both tracers capture all function calls
✅ **Accuracy**: Argument values are correctly captured
✅ **Equivalence**: LTTng and eBPF trace the same data
✅ **Reliability**: No events dropped or duplicated
✅ **Automation**: One command validates everything

**Best practice**: Run validation before every benchmark to ensure tracers work correctly!

```bash
sudo ./validate_output.sh && sudo ./benchmark.py ./build
```
