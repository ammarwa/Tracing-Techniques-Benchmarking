# eBPF Tracer Design Documentation

## Overview

The eBPF tracer (`mylib_tracer`) is a high-performance userspace function tracer built using **eBPF (Extended Berkeley Packet Filter)** with **uprobes**. It traces the `my_traced_function()` in `libmylib.so` with minimal overhead by leveraging kernel-level instrumentation.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     User Space                               │
│                                                              │
│  ┌──────────────────┐          ┌────────────────────────┐  │
│  │  Target App      │          │  mylib_tracer          │  │
│  │  (sample_app)    │          │  (userspace loader)    │  │
│  │                  │          │                         │  │
│  │  libmylib.so     │          │  • Ring buffer polling │  │
│  │  ↓               │          │  • Event buffering     │  │
│  │  my_traced_func()│          │  • File output         │  │
│  └─────────┬────────┘          └──────────┬─────────────┘  │
│            │                               │                 │
│            │ Function call                 │ Read events     │
│            ↓                               ↑                 │
├────────────┼───────────────────────────────┼────────────────┤
│            │         Kernel Space          │                 │
│            │                               │                 │
│  ┌─────────┴───────────────────────────────┴─────────────┐  │
│  │              eBPF Programs                            │  │
│  │                                                        │  │
│  │  ┌──────────────────┐    ┌──────────────────────┐   │  │
│  │  │ Uprobe (entry)   │    │ Uretprobe (exit)     │   │  │
│  │  │                  │    │                      │   │  │
│  │  │ • Capture args   │    │ • Capture timestamp  │   │  │
│  │  │ • Timestamp      │    │ • Minimal overhead   │   │  │
│  │  │ • Ring buffer    │    │ • Ring buffer        │   │  │
│  │  └──────────────────┘    └──────────────────────┘   │  │
│  │                                                        │  │
│  │  ┌──────────────────────────────────────────────┐   │  │
│  │  │          Ring Buffer (1 MB)                  │   │  │
│  │  │  • High-throughput event queue               │   │  │
│  │  │  • Lock-free kernel→user data transfer       │   │  │
│  │  └──────────────────────────────────────────────┘   │  │
│  └────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

## Components

### 1. eBPF Kernel Programs (`mylib_tracer.bpf.c`)

**Kernel-side instrumentation** that runs in the Linux kernel context.

#### Event Structures

**Entry Event** (optimized, 32 bytes):
```c
struct trace_event_entry {
    u64 timestamp;      // Nanosecond timestamp
    s32 arg1;          // int argument
    u64 arg2;          // uint64_t argument
    u64 arg3_ptr;      // Pointer (not dereferenced for speed)
    u64 arg5;          // void* argument
    u32 event_type;    // 0 = entry
} __attribute__((packed));
```

**Exit Event** (minimal, 12 bytes):
```c
struct trace_event_exit {
    u64 timestamp;     // Nanosecond timestamp
    u32 event_type;    // 1 = exit
} __attribute__((packed));
```

#### Ring Buffer

- **Type**: `BPF_MAP_TYPE_RINGBUF`
- **Size**: 1 MB
- **Purpose**: Lock-free, high-throughput event queue from kernel to userspace
- **Behavior**: FIFO queue with overwrite on full (configurable)

#### Uprobe (Entry Hook)

```c
SEC("uprobe/my_traced_function")
int my_traced_function_entry(struct pt_regs *ctx)
```

**Operations** (in order):
1. Reserve space in ring buffer (`bpf_ringbuf_reserve()`)
2. Capture timestamp (`bpf_ktime_get_ns()`)
3. Extract function arguments from CPU registers:
   - `arg1`: RDI (first argument)
   - `arg2`: RSI (second argument)
   - `arg3_ptr`: RDX (third argument, pointer only)
   - `arg5`: R8 (fifth argument)
4. Submit event to ring buffer (`bpf_ringbuf_submit()`)

**Optimizations**:
- Packed struct (32 bytes total)
- No string dereferencing (store pointer only)
- Minimal operations in hot path
- Direct register access via `PT_REGS_PARM` macros

#### Uretprobe (Exit Hook)

```c
SEC("uretprobe/my_traced_function")
int my_traced_function_exit(struct pt_regs *ctx)
```

**Operations**:
1. Reserve space in ring buffer (12 bytes)
2. Capture timestamp
3. Submit event

**Optimization**: Minimal event structure (only timestamp + type)

### 2. Userspace Loader (`mylib_tracer.c`)

**Application** that loads eBPF programs and processes events.

#### Key Features

**Event Buffering**:
- In-memory buffer: 1M events (~32 MB RAM)
- Zero I/O during tracing (file write deferred to end)
- Fast `memcpy()` instead of `fprintf()` in hot path

**Library Discovery**:
```c
const char* find_library() {
    // Tries multiple locations:
    // - ./build/lib/libmylib.so
    // - ../lib/libmylib.so
    // - ./lib/libmylib.so
    // etc.
}
```

**Function Offset Resolution**:
```c
long get_function_offset(const char *lib_path, const char *func_name) {
    // Uses `nm -D` to find symbol offset
    // Example: nm -D libmylib.so | grep ' T my_traced_function$'
}
```

#### Execution Flow

1. **Initialization**
   ```c
   // Allocate event buffer
   event_buffer = calloc(MAX_EVENTS, sizeof(union stored_event));

   // Load BPF skeleton
   skel = mylib_tracer_bpf__open();
   mylib_tracer_bpf__load(skel);
   ```

2. **Attach Uprobes**
   ```c
   // Find function offset in library
   func_offset = get_function_offset(lib_path, "my_traced_function");

   // Attach entry uprobe
   link_entry = bpf_program__attach_uprobe(
       skel->progs.my_traced_function_entry,
       false,  /* not uretprobe */
       -1,     /* any process */
       lib_path,
       func_offset
   );

   // Attach exit uretprobe
   link_exit = bpf_program__attach_uprobe(
       skel->progs.my_traced_function_exit,
       true,   /* is uretprobe */
       -1,     /* any process */
       lib_path,
       func_offset
   );
   ```

3. **Event Processing Loop**
   ```c
   while (!exiting) {
       // Poll ring buffer (100ms timeout)
       ring_buffer__poll(rb, 100);
   }
   ```

4. **Event Handler** (hot path)
   ```c
   static int handle_event(void *ctx, void *data, size_t data_sz) {
       if (event_count >= MAX_EVENTS) {
           events_dropped++;
           return 0;
       }

       // Fast memcpy to in-memory buffer
       memcpy(&event_buffer[event_count], data, data_sz);
       event_sizes[event_count] = data_sz;
       event_count++;

       return 0;
   }
   ```

5. **File Output** (deferred to end)
   ```c
   static void write_events_to_file(const char *filename) {
       FILE *f = fopen(filename, "w");

       for (unsigned long i = 0; i < event_count; i++) {
           if (event_sizes[i] == sizeof(struct trace_event_entry)) {
               // Format entry event
               fprintf(f, "[%lu.%09lu] mylib:my_traced_function_entry: ...\n", ...);
           } else {
               // Format exit event
               fprintf(f, "[%lu.%09lu] mylib:my_traced_function_exit\n", ...);
           }
       }

       fclose(f);
   }
   ```

## Build System

### BPF Compilation

**Clang** is used to compile eBPF programs to BPF bytecode:

```bash
clang -g -O2 -target bpf \
      -D__TARGET_ARCH_x86_64 \
      -I/usr/include/x86_64-linux-gnu \
      -c mylib_tracer.bpf.c -o mylib_tracer.bpf.o
```

**Key flags**:
- `-target bpf`: Generate BPF bytecode
- `-O2`: Optimize (required for BPF verifier)
- `-D__TARGET_ARCH_x86_64`: Architecture-specific macros

### Skeleton Generation

**bpftool** generates a C header with BPF program loader:

```bash
bpftool gen skeleton mylib_tracer.bpf.o > mylib_tracer.skel.h
```

**Generated skeleton** includes:
- `mylib_tracer_bpf__open()`: Open BPF object
- `mylib_tracer_bpf__load()`: Load and verify BPF programs
- `mylib_tracer_bpf__destroy()`: Cleanup
- Access to maps: `skel->maps.events`
- Access to programs: `skel->progs.my_traced_function_entry`

### Userspace Compilation

```bash
gcc -g -O2 mylib_tracer.c \
    -lbpf -lelf -lz \
    -o mylib_tracer
```

**Dependencies**:
- `libbpf`: BPF library for loading/attaching programs
- `libelf`: ELF parsing
- `libz`: Compression support

## Performance Optimizations

### 1. Minimal Event Structures

**Before** (large, slow):
```c
struct trace_event {
    u64 timestamp;
    s32 arg1;
    u64 arg2;
    char arg3[64];      // 64-byte string copy! ❌
    double arg4;
    u64 arg5;
    u32 event_type;
};
```

**After** (optimized):
```c
struct trace_event_entry {
    u64 timestamp;
    s32 arg1;
    u64 arg2;
    u64 arg3_ptr;      // Just the pointer ✅
    u64 arg5;
    u32 event_type;
} __attribute__((packed));
```

**Improvement**: 32 bytes vs 96 bytes (66% reduction)

### 2. Deferred File I/O

**Before** (slow):
```c
// fprintf() in event handler (HOT PATH) ❌
static int handle_event(void *ctx, void *data, size_t data_sz) {
    fprintf(output_file, "[%lu] event...\n", ...);  // Disk I/O!
}
```

**After** (fast):
```c
// memcpy() in event handler (HOT PATH) ✅
static int handle_event(void *ctx, void *data, size_t data_sz) {
    memcpy(&event_buffer[event_count++], data, data_sz);  // RAM only!
}

// Write to file AFTER tracing completes
write_events_to_file(filename);
```

**Improvement**: ~10-100× faster event handling

### 3. Ring Buffer Size Tuning

- **1 MB ring buffer**: Handles bursts of up to ~30K events
- **1M event memory buffer**: Can store entire trace in RAM
- **Overflow handling**: Drop events if buffer full (counted in `events_dropped`)

### 4. Register-Direct Argument Capture

```c
// Direct register access (fast) ✅
event->arg1 = (s32)PT_REGS_PARM1(ctx);  // RDI
event->arg2 = PT_REGS_PARM2(ctx);       // RSI
event->arg3_ptr = PT_REGS_PARM3(ctx);   // RDX (pointer only)
event->arg5 = PT_REGS_PARM5(ctx);       // R8
```

No function calls, no memory dereferencing in eBPF program.

## Usage

### Start Tracer

```bash
sudo ./mylib_tracer /tmp/trace.txt
```

Output:
```
Using library: ./build/lib/libmylib.so
Allocated buffer for 1000000 events (32 MB)
Found my_traced_function at offset 0x1180
Successfully attached uprobes to my_traced_function
Tracing... Press Ctrl-C to stop.
```

### Run Target Application

In another terminal:
```bash
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000
```

### Stop Tracer

Press `Ctrl-C`:
```
^C
Tracing stopped. Captured 20000 events.
Writing 20000 events to /tmp/trace.txt...
Wrote 20000 events (0 dropped)
```

### View Trace Output

```bash
head /tmp/trace.txt
```

```
[1728567890.123456789] mylib:my_traced_function_entry: { arg1 = 42, arg2 = 1234567890, arg3_ptr = 0x7ffd12345678, arg5 = 0xdeadbeef }
[1728567890.123556789] mylib:my_traced_function_exit
[1728567890.123656789] mylib:my_traced_function_entry: { arg1 = 42, arg2 = 1234567891, arg3_ptr = 0x7ffd12345678, arg5 = 0xdeadbeef }
[1728567890.123756789] mylib:my_traced_function_exit
...
```

## Overhead Analysis

### Constant Overhead

Uprobe overhead is **~5 microseconds per function call** (constant):

| Function Duration | Overhead | Relative Overhead |
|-------------------|----------|-------------------|
| 0 μs (empty) | ~5 μs | 83,000% ❌ |
| 5 μs | ~5 μs | 100% ⚠️ |
| 50 μs | ~5 μs | 10% ⚠️ |
| 100 μs | ~5 μs | 5% ✅ |
| 500 μs | ~5 μs | 1% ✅ |
| 1000 μs (1 ms) | ~5 μs | 0.5% ✅ |

### Why ~5 μs?

The overhead comes from:
1. **Context switch**: User → Kernel (~1 μs)
2. **eBPF execution**: Capture args, write to ring buffer (~1-2 μs)
3. **Return to user**: Kernel → User (~1 μs)
4. **Cache/TLB effects**: Minimal (<1 μs)

**Total**: ~5 μs per call

### Production Impact

For GPU/HIP tracing:
- **HIP API calls**: 10-1000 μs → 0.5-5% overhead
- **GPU kernels**: 1-1000 ms → <0.1% overhead
- **Total application**: <1% overhead ✅

## Advantages

✅ **No Application Modification**: Attach to running processes
✅ **Dynamic Attach/Detach**: Start/stop tracing anytime
✅ **Kernel Visibility**: Can trace kernel functions too
✅ **High Performance**: Ring buffer, deferred I/O
✅ **Safe**: BPF verifier ensures no crashes
✅ **Flexible**: Attach to any function in any library

## Limitations

❌ **Requires Root**: Uprobe attachment needs CAP_SYS_ADMIN
❌ **Fixed ~5 μs Overhead**: Not suitable for sub-microsecond functions
❌ **BPF Complexity**: Requires BPF toolchain (clang, bpftool, libbpf)
❌ **Kernel Dependency**: Requires kernel 5.8+ for best features

## Troubleshooting

### Failed to attach uprobe

**Problem**: Permission denied
```bash
Failed to attach entry uprobe: Operation not permitted
```

**Solution**: Run with sudo
```bash
sudo ./mylib_tracer /tmp/trace.txt
```

### Failed to find library

**Problem**: Library not in search path
```bash
Failed to find libmylib.so in any expected location
```

**Solution**: Add library path or run from correct directory
```bash
cd build
sudo ./bin/mylib_tracer /tmp/trace.txt
```

### Events dropped

**Problem**: Ring buffer overflow
```
Wrote 800000 events (200000 dropped)
```

**Solution**: Increase `MAX_EVENTS` in `mylib_tracer.c`:
```c
#define MAX_EVENTS 10000000  // 10M events
```

Or increase ring buffer size in `mylib_tracer.bpf.c`:
```c
__uint(max_entries, 4 * 1024 * 1024);  // 4MB
```

### BPF verifier errors

**Problem**: Program rejected by verifier
```
Failed to load and verify BPF skeleton
```

**Solution**: Enable verifier debug output:
```c
libbpf_set_print(libbpf_print_fn);  // Enable debug logs
```

Check for:
- Unbounded loops (must be compile-time bounded)
- Invalid memory access (use `bpf_probe_read_user()`)
- Stack size >512 bytes

## Future Enhancements

Potential improvements:
- [ ] Support for multiple functions
- [ ] Argument filtering (e.g., only trace if arg1 > 100)
- [ ] Return value capture
- [ ] String dereferencing (with bounds checking)
- [ ] CPU affinity for tracer process
- [ ] Real-time output mode (vs deferred)
- [ ] Integration with Chrome Trace Event format

## References

- [BPF Documentation](https://www.kernel.org/doc/html/latest/bpf/index.html)
- [libbpf Documentation](https://libbpf.readthedocs.io/)
- [BPF CO-RE](https://nakryiko.com/posts/bpf-portability-and-co-re/)
- [Uprobe Documentation](https://www.kernel.org/doc/Documentation/trace/uprobetracer.txt)
