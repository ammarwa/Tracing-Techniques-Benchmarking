# Sample Library and Application Documentation

## Overview

The sample library (`libmylib.so`) and application (`sample_app`) form the **target workload** for the eBPF vs LTTng tracing comparison. They simulate realistic API call patterns with configurable work durations to demonstrate how tracer overhead scales.

## Architecture

```
┌────────────────────────────────────────────────────────┐
│                   sample_app                           │
│                                                        │
│  ┌──────────────────────────────────────────────┐    │
│  │  main()                                      │    │
│  │                                               │    │
│  │  • Parse command-line args                  │    │
│  │  • Read SIMULATED_WORK_US env var           │    │
│  │  • Call my_traced_function() in loop        │    │
│  │  • Measure and report timing                │    │
│  └───────────────┬──────────────────────────────┘    │
│                  │                                     │
│                  │ Links to                            │
│                  ↓                                     │
│  ┌──────────────────────────────────────────────┐    │
│  │           libmylib.so                        │    │
│  │                                               │    │
│  │  void set_simulated_work_duration(us)       │    │
│  │    → Sets global work duration              │    │
│  │                                               │    │
│  │  void my_traced_function(...)                │    │
│  │    → Minimal work (prevent optimization)     │    │
│  │    → Busy-wait sleep (simulate real work)    │    │
│  └──────────────────────────────────────────────┘    │
└────────────────────────────────────────────────────────┘
```

## Components

### 1. Sample Library (`libmylib.so`)

A minimal shared library that provides:
1. A function to trace: `my_traced_function()`
2. Configurable work simulation: `set_simulated_work_duration()`

#### Header File (`mylib.h`)

```c
#ifndef MYLIB_H
#define MYLIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sample API function with multiple arguments
// Does nothing but provides a realistic function signature for tracing
void my_traced_function(
    int arg1,
    uint64_t arg2,
    double arg3,
    void* arg4
);

// Set simulated work duration (in microseconds)
// sleep_us = 0: Empty function (minimal work)
// sleep_us > 0: Sleep for specified duration to simulate real API calls
void set_simulated_work_duration(unsigned int sleep_us);

#ifdef __cplusplus
}
#endif

#endif // MYLIB_H
```

**Design choices**:
- **Multiple argument types**: Tests tracer's ability to capture different data types
- **C linkage**: Ensures compatibility with C and C++ applications
- **Include guards**: Standard header protection

#### Implementation (`mylib.c`)

**Key Features**:

##### 1. Minimal Work (Prevent Optimization)

```c
static volatile int dummy = 0;

void my_traced_function(...) {
    // Do some minimal work to prevent complete optimization
    dummy = arg1 + (int)arg2;
    dummy += (int)arg3;

    if (arg4) {
        dummy += 1;
    }
}
```

**Why `volatile`?**
- Prevents compiler from optimizing away the function entirely
- Ensures function has measurable execution time (even if tiny)
- Simulates "empty function" worst-case scenario

##### 2. Busy-Wait Sleep (Accurate Timing)

```c
static void busy_sleep_us(unsigned int microseconds) {
    if (microseconds == 0) return;

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

**Why busy-wait instead of `usleep()` or `nanosleep()`?**

| Method | Accuracy | CPU Usage | Scheduler Impact |
|--------|----------|-----------|------------------|
| `usleep()` / `nanosleep()` | ±1-10 ms | Low | High (context switches) |
| **Busy-wait** | ±1-10 ns | High | None (no context switch) |

**Advantages of busy-wait**:
- ✅ **Microsecond precision**: Actual delay matches target within nanoseconds
- ✅ **Consistent CPU usage**: Simulates real computational work
- ✅ **No scheduler interference**: No context switches that would skew timing measurements
- ✅ **Deterministic behavior**: Same CPU usage pattern every run

##### 3. Configurable Work Duration

```c
static unsigned int simulated_work_us = 0;

void set_simulated_work_duration(unsigned int sleep_us) {
    simulated_work_us = sleep_us;
}

void my_traced_function(...) {
    // ... minimal work ...

    // Simulate realistic API work duration
    if (simulated_work_us > 0) {
        busy_sleep_us(simulated_work_us);
    }
}
```

**Usage scenarios**:
- `0 μs`: Empty function (worst-case overhead test)
- `5 μs`: Ultra-fast API (comparable to uprobe overhead)
- `100 μs`: Typical HIP API (e.g., `hipMalloc` small buffer)
- `1000 μs`: Slow API (e.g., `hipMalloc` large buffer, kernel launch)

### 2. Sample Application (`sample_app`)

A simple benchmark harness that calls the traced function repeatedly.

#### Command-Line Interface

```bash
./sample_app <num_iterations>
```

**Arguments**:
- `num_iterations`: Number of times to call `my_traced_function()`

**Examples**:
```bash
# Empty function test (1M iterations)
./sample_app 1000000

# Typical workload (10K iterations)
./sample_app 10000
```

#### Environment Variable: SIMULATED_WORK_US

The application reads the `SIMULATED_WORK_US` environment variable to configure simulated work duration:

```bash
# Empty function (0 μs work)
./sample_app 1000000

# 100 μs work per call
SIMULATED_WORK_US=100 ./sample_app 10000

# 1 ms work per call
SIMULATED_WORK_US=1000 ./sample_app 2000
```

#### Timing Measurement

```c
struct timespec start, end;
clock_gettime(CLOCK_MONOTONIC, &start);

// Call the traced function many times
for (long i = 0; i < num_iterations; i++) {
    my_traced_function(...);
}

clock_gettime(CLOCK_MONOTONIC, &end);

// Calculate elapsed time
double elapsed = (end.tv_sec - start.tv_sec) +
                 (end.tv_nsec - start.tv_nsec) / 1e9;

printf("Average time per call: %.2f nanoseconds\n",
       (elapsed / num_iterations) * 1e9);
```

**Why `CLOCK_MONOTONIC`?**
- Not affected by system time adjustments (NTP, manual changes)
- Consistent across measurements
- High-resolution timing (nanosecond precision)

#### Output Format

```
Starting benchmark with 10000 iterations (simulated work: 100 μs)...
Completed 10000 iterations in 1.083456 seconds
Average time per call: 108345.60 nanoseconds
```

**Parsed by benchmark scripts** to extract:
- Total elapsed time
- Average time per call (in nanoseconds)

## Build System

### Shared Library

```bash
# Compile object file
gcc -fPIC -c -o mylib.o mylib.c

# Link shared library with versioning
gcc -shared -Wl,-soname,libmylib.so.1 -o libmylib.so.1.0 mylib.o

# Create symlinks
ln -sf libmylib.so.1.0 libmylib.so.1
ln -sf libmylib.so.1 libmylib.so
```

**Key flags**:
- `-fPIC`: Position-independent code (required for shared libraries)
- `-shared`: Build as shared library
- `-Wl,-soname,libmylib.so.1`: Set SONAME for version management

### Application

```bash
gcc -o sample_app main.c -L./build/lib -lmylib -Wl,-rpath,./build/lib
```

**Key flags**:
- `-L./build/lib`: Library search path at link time
- `-lmylib`: Link against `libmylib.so`
- `-Wl,-rpath,./build/lib`: Embed runtime library search path (no need for `LD_LIBRARY_PATH`)

## Usage Examples

### 1. Baseline (No Tracing)

Measure function overhead without any tracing:

```bash
# Empty function
./build/bin/sample_app 1000000
# Output: Average time per call: 6.32 nanoseconds

# 100 μs work
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000
# Output: Average time per call: 108234.56 nanoseconds
```

### 2. With LTTng Tracing

```bash
# Start LTTng session
lttng create test_session
lttng enable-event -u mylib:*
lttng start

# Run with LTTng wrapper
SIMULATED_WORK_US=100 LD_PRELOAD=./build/lib/libmylib_lttng.so ./build/bin/sample_app 10000

# Stop and view trace
lttng stop
lttng destroy test_session
babeltrace ~/lttng-traces/test_session-*
```

### 3. With eBPF Tracing

```bash
# Start eBPF tracer (in one terminal)
sudo ./build/bin/mylib_tracer /tmp/trace.txt

# Run application (in another terminal)
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000

# Stop tracer
# (Press Ctrl-C in tracer terminal)

# View trace
cat /tmp/trace.txt
```

### 4. In Benchmark Suite

The comprehensive benchmark uses the sample library/app:

```bash
sudo ../scripts/benchmark.py ./build
```

Internally runs:
```python
# Baseline
env = {'SIMULATED_WORK_US': '100'}
run('/usr/bin/time ./build/bin/sample_app 10000', env=env)

# LTTng
env = {'SIMULATED_WORK_US': '100', 'LD_PRELOAD': './build/lib/libmylib_lttng.so'}
run('/usr/bin/time ./build/bin/sample_app 10000', env=env)

# eBPF
start_tracer()
env = {'SIMULATED_WORK_US': '100'}
run('/usr/bin/time ./build/bin/sample_app 10000', env=env)
stop_tracer()
```

## Design Rationale

### Why This Design?

#### 1. Realistic Function Signature

```c
void my_traced_function(
    int arg1,           // Common: integer
    uint64_t arg2,      // Common: large integer (e.g., size_t)
    double arg3,        // Floating-point value
    void* arg4          // Common: opaque pointer
);
```

**Maps to real HIP/ROCm APIs**:
- `hipMalloc(void** ptr, size_t size)` → 2 args (pointer, uint64_t)
- `hipMemcpy(void* dst, void* src, size_t size, hipMemcpyKind kind)` → 4 args
- `hipLaunchKernel(void* func, dim3 grid, dim3 block, ...)` → many args

**Tests tracer capabilities**:
- Argument capture from registers (x86-64: RDI, RSI, XMM0, RCX)
- Different data types (int, uint64_t, double, pointer)
- Floating-point register handling (XMM registers)

#### 2. Configurable Work Duration

**Problem**: Single "empty function" test doesn't represent real workloads

**Solution**: Environment variable controls simulated work
- Benchmark can test multiple scenarios (0 μs, 5 μs, 100 μs, 1000 μs)
- Demonstrates how overhead scales with function duration
- Proves constant overhead principle

#### 3. Busy-Wait Sleep

**Problem**: `usleep()` has high variance (±10 ms), context switches

**Solution**: Busy-wait with `clock_gettime()`
- Accurate to nanoseconds
- Simulates CPU-bound work (like real API calls)
- No scheduler interference

#### 4. Minimal Actual Work

**Problem**: Compiler might optimize away empty function

**Solution**: `volatile` variable prevents optimization
- Function has measurable execution time
- Simulates "worst case" empty function scenario
- Still only ~6 ns overhead

## Troubleshooting

### Library Not Found

**Problem**: `./sample_app: error while loading shared libraries: libmylib.so: cannot open shared object file`

**Solution 1**: Use rpath (embedded at build time)
```bash
gcc -o sample_app main.c -L./build/lib -lmylib -Wl,-rpath,./build/lib
```

**Solution 2**: Set `LD_LIBRARY_PATH`
```bash
export LD_LIBRARY_PATH=$PWD/build/lib:$LD_LIBRARY_PATH
./build/bin/sample_app 10000
```

**Solution 3**: Install to system path
```bash
sudo cp build/lib/libmylib.so* /usr/local/lib/
sudo ldconfig
```

### Simulated Work Not Applied

**Problem**: Even with `SIMULATED_WORK_US=100`, function still takes ~6 ns

**Possible causes**:
1. **Wrong library loaded**: Check with `ldd`
   ```bash
   ldd ./build/bin/sample_app
   # Should show: libmylib.so.1 => /path/to/build/lib/libmylib.so.1
   ```

2. **LTTng wrapper doesn't export `set_simulated_work_duration()`**
   - Fixed in `mylib_wrapper.c` (see LTTNG_DESIGN.md)

3. **Environment variable not passed**
   ```bash
   # Wrong (env var not passed to LD_PRELOAD command)
   export SIMULATED_WORK_US=100
   LD_PRELOAD=./build/lib/libmylib_lttng.so ./build/bin/sample_app 10000

   # Correct (inline env var)
   SIMULATED_WORK_US=100 LD_PRELOAD=./build/lib/libmylib_lttng.so ./build/bin/sample_app 10000
   ```

### High Variance in Timing

**Problem**: "Average time per call" varies widely between runs

**Solutions**:
1. **Disable CPU frequency scaling**
   ```bash
   sudo cpupower frequency-set -g performance
   ```

2. **Pin to specific CPU**
   ```bash
   taskset -c 0 ./build/bin/sample_app 10000
   ```

3. **Close background applications**
   - Browser, IDE, etc. consume CPU

4. **Use benchmark's multiple-run averaging**
   ```python
   suite = BenchmarkSuite(build_dir, num_runs=100)  # 100 runs for statistics
   ```

## Integration with Tracers

### How eBPF Finds the Function

1. **Library path**: `/path/to/libmylib.so`
2. **Function name**: `my_traced_function`
3. **Symbol resolution**: `nm -D libmylib.so | grep my_traced_function`
   ```
   0000000000001180 T my_traced_function
   ```
4. **Uprobe attachment**: Kernel attaches probe at offset `0x1180`

### How LTTng Wraps the Function

1. **LD_PRELOAD**: `libmylib_lttng.so` loaded before `libmylib.so`
2. **Symbol interposition**: `my_traced_function` in wrapper shadows original
3. **dlsym(RTLD_NEXT)**: Wrapper finds real function
4. **Tracepoint injection**: Wrapper adds `tracepoint()` calls before/after

## Future Enhancements

Potential improvements:
- [ ] Add more argument types (structs, arrays)
- [ ] Return value testing (int, pointer, struct)
- [ ] Multi-threaded version (concurrent calls)
- [ ] GPU kernel simulation (longer work durations)
- [ ] Configurable argument values (not hardcoded)
- [ ] Error injection (test tracer robustness)

## Summary

The sample library and application provide a **flexible, realistic test workload** for comparing eBPF and LTTng tracing overhead. Key features:

✅ **Realistic function signature**: Multiple argument types (int, uint64_t, double, pointer)
✅ **Configurable work duration**: Simulates API calls from 0 μs (empty) to 1000 μs (slow)
✅ **Accurate timing**: Busy-wait sleep ensures microsecond precision
✅ **Minimal overhead**: ~6 ns baseline (worst-case empty function)
✅ **Easy integration**: Used by both manual tests and automated benchmarks
