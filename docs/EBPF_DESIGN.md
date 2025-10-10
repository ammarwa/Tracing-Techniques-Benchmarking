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

## Detailed Implementation Flow

This section explains the complete flow from source code to runtime tracing, including compilation, kernel interactions, and event capture.

### Phase 1: BPF Program Compilation

#### Step 1.1: Writing the BPF Kernel Program (`mylib_tracer.bpf.c`)

The BPF program contains kernel-side instrumentation code that will run when function calls are intercepted. Key contents:

**Map Definitions:**
```c
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1024 * 1024);  // 1 MB ring buffer
} events SEC(".maps");
```

This defines a ring buffer that acts as a lock-free, high-throughput queue for transferring events from kernel space to user space.

**Event Structures:**
```c
struct trace_event_entry {
    u64 timestamp;
    s32 arg1;
    u64 arg2;
    u64 arg3_ptr;
    u64 arg5;
    u32 event_type;
} __attribute__((packed));
```

Defines the data format for events. Packed to minimize memory footprint.

**Uprobe Handler (Entry Point):**
```c
SEC("uprobe/my_traced_function")
int my_traced_function_entry(struct pt_regs *ctx) {
    // Allocate event in ring buffer
    struct trace_event_entry *event =
        bpf_ringbuf_reserve(&events, sizeof(*event), 0);

    if (!event) return 0;

    // Capture data from CPU registers
    event->arg1 = (s32)PT_REGS_PARM1(ctx);
    event->arg2 = PT_REGS_PARM2(ctx);
    event->timestamp = bpf_ktime_get_ns();
    event->event_type = 0;

    // Submit to ring buffer
    bpf_ringbuf_submit(event, 0);
    return 0;
}
```

**Uretprobe Handler (Exit Point):**
```c
SEC("uretprobe/my_traced_function")
int my_traced_function_exit(struct pt_regs *ctx) {
    // Similar structure but captures exit event
}
```

#### Step 1.2: Compiling BPF Program to Bytecode

The BPF program is compiled to BPF bytecode (not native x86):

```bash
clang -g -O2 -target bpf \
      -D__TARGET_ARCH_x86_64 \
      -I/usr/include/x86_64-linux-gnu \
      -c mylib_tracer.bpf.c -o mylib_tracer.bpf.o
```

**What happens:**
- Clang compiles the C code to BPF instructions (BPF is a RISC-like instruction set)
- `-target bpf` tells clang to emit BPF bytecode instead of x86 machine code
- `-O2` optimization is **required** - the BPF verifier expects optimized code
- Output `mylib_tracer.bpf.o` is an ELF object file containing:
  - BPF bytecode in `.text` sections
  - BPF map definitions in `.maps` section
  - BTF (BPF Type Format) debug information
  - Relocation information for the loader

**BPF Bytecode Example:**
The C code `event->arg1 = (s32)PT_REGS_PARM1(ctx);` compiles to BPF instructions like:
```
r1 = *(u64 *)(r1 + 0x70)   # Load RDI from pt_regs
r2 = r2                     # Move to destination
*(u32 *)(r2 + 8) = r1      # Store to event struct
```

### Phase 2: Skeleton Generation

#### Step 2.1: Generating C Loader Code

The `bpftool` utility analyzes the compiled BPF object and generates a C header file:

```bash
bpftool gen skeleton mylib_tracer.bpf.o > mylib_tracer.skel.h
```

**What `mylib_tracer.skel.h` contains:**

1. **Struct Definitions** (mirroring BPF structures):
```c
struct mylib_tracer_bpf {
    struct bpf_object_skeleton *skeleton;
    struct bpf_object *obj;
    struct {
        struct bpf_map *events;  // Pointer to ring buffer map
    } maps;
    struct {
        struct bpf_program *my_traced_function_entry;
        struct bpf_program *my_traced_function_exit;
    } progs;
    struct {
        struct bpf_link *my_traced_function_entry;
        struct bpf_link *my_traced_function_exit;
    } links;
};
```

2. **Embedded BPF Bytecode**:
```c
static inline const void *mylib_tracer_bpf__elf_bytes(size_t *sz) {
    static const char data[] __attribute__((__aligned__(8))) = {
        0x7f, 0x45, 0x4c, 0x46, ...  // Entire .bpf.o file embedded!
    };
    *sz = sizeof(data);
    return data;
}
```
The entire compiled BPF object is embedded as a byte array in the header!

3. **Loader Functions**:
```c
static inline struct mylib_tracer_bpf *
mylib_tracer_bpf__open(void) {
    // Allocates struct, loads embedded bytecode
    // Returns handle for further operations
}

static inline int
mylib_tracer_bpf__load(struct mylib_tracer_bpf *obj) {
    // Calls bpf() syscall to load programs into kernel
    // Creates BPF maps (ring buffer)
    // Performs BPF verifier checks
}

static inline void
mylib_tracer_bpf__destroy(struct mylib_tracer_bpf *obj) {
    // Cleanup and resource release
}
```

**Why skeleton?**
- Self-contained: No need to ship separate `.bpf.o` file
- Type-safe: C structs match BPF definitions exactly
- Convenient: Simple API instead of manual libbpf calls

### Phase 3: Userspace Tracer Compilation

#### Step 3.1: Writing the Userspace Loader (`mylib_tracer.c`)

This program loads BPF code into the kernel and processes events. Key components:

**Includes:**
```c
#include "mylib_tracer.skel.h"  // Generated skeleton
#include <bpf/libbpf.h>         // BPF library functions
#include <bpf/bpf.h>            // Low-level BPF syscall wrappers
```

**Library Discovery Function:**
```c
const char* find_library(void) {
    const char* search_paths[] = {
        "./build/lib/libmylib.so",
        "../lib/libmylib.so",
        "./lib/libmylib.so",
        // ... more paths
    };

    for (int i = 0; i < sizeof(search_paths)/sizeof(search_paths[0]); i++) {
        if (access(search_paths[i], F_OK) == 0) {
            return search_paths[i];
        }
    }
    return NULL;
}
```

This searches common locations for the target library using filesystem checks.

**Function Offset Resolution:**
```c
long get_function_offset(const char *lib_path, const char *func_name) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "nm -D %s | grep ' T %s$'",
             lib_path, func_name);

    FILE *fp = popen(cmd, "r");
    // Parse output: "00000000000011a0 T my_traced_function"
    // Returns: 0x11a0 (offset from library base)
}
```

**How it works:**
- `nm -D` lists dynamic symbols from the shared library
- Finds the symbol address (offset from library base)
- Example output: `00000000000011a0 T my_traced_function`
- The offset `0x11a0` is where the function code starts in the library

**Why offset instead of absolute address?**
- Libraries are loaded at different addresses each run (ASLR)
- Offset from library base is constant
- Kernel resolves: `absolute_address = library_base + offset`

**Event Buffer Allocation:**
```c
union stored_event {
    struct trace_event_entry entry;
    struct trace_event_exit exit;
};

unsigned long event_count = 0;
unsigned long MAX_EVENTS = 1000000;
union stored_event *event_buffer;
size_t *event_sizes;

// Allocate ~32 MB for 1M events
event_buffer = calloc(MAX_EVENTS, sizeof(union stored_event));
event_sizes = calloc(MAX_EVENTS, sizeof(size_t));
```

**Event Handler (Hot Path):**
```c
static int handle_event(void *ctx, void *data, size_t data_sz) {
    if (event_count >= MAX_EVENTS) {
        events_dropped++;
        return 0;
    }

    // Fast memcpy to RAM buffer (no I/O!)
    memcpy(&event_buffer[event_count], data, data_sz);
    event_sizes[event_count] = data_sz;
    event_count++;

    return 0;
}
```

This is called by libbpf when events arrive from the ring buffer.

#### Step 3.2: Compiling the Userspace Tracer

```bash
gcc -g -O2 mylib_tracer.c \
    -lbpf -lelf -lz \
    -o mylib_tracer
```

**Linked libraries:**
- `libbpf`: Provides BPF loading, map management, ring buffer polling
- `libelf`: ELF file parsing (needed to read BPF objects)
- `libz`: Compression (BTF data may be compressed)

**Output:** `mylib_tracer` executable (userspace program)

### Phase 4: Runtime Execution

#### Step 4.1: Starting the Tracer

```bash
sudo ./mylib_tracer /tmp/trace.txt
```

**Execution sequence in `main()`:**

1. **Unbuffer stdout/stderr** (for immediate output):
```c
setbuf(stdout, NULL);
setbuf(stderr, NULL);
```

2. **Find the target library:**
```c
lib_path = find_library();
// Result: "./build/lib/libmylib.so"
```

3. **Allocate event buffer:**
```c
event_buffer = calloc(MAX_EVENTS, sizeof(union stored_event));
printf("Allocated buffer for %lu events (%lu MB)\n",
       MAX_EVENTS, (MAX_EVENTS * sizeof(union stored_event)) / (1024*1024));
```

4. **Open BPF skeleton:**
```c
skel = mylib_tracer_bpf__open();
```
This calls the skeleton function which:
- Allocates `struct mylib_tracer_bpf`
- Extracts embedded BPF bytecode from the skeleton header
- Parses the ELF object using libbpf
- **Does NOT load into kernel yet**

5. **Load BPF programs into kernel:**
```c
err = mylib_tracer_bpf__load(skel);
```
This performs the actual kernel loading:
- Calls `bpf(BPF_MAP_CREATE, ...)` syscall to create ring buffer map
- Calls `bpf(BPF_PROG_LOAD, ...)` syscall to load each BPF program
- **BPF verifier runs here** - checks for safety:
  - No infinite loops
  - All memory accesses are valid
  - Stack usage < 512 bytes
  - No out-of-bounds accesses
- If verification fails, loading is rejected
- On success, kernel returns file descriptors for maps and programs

6. **Find function offset:**
```c
func_offset = get_function_offset(lib_path, "my_traced_function");
printf("Found my_traced_function at offset 0x%lx\n", func_offset);
// Example output: 0x11a0
```

7. **Attach uprobes:**
```c
// Attach entry uprobe
skel->links.my_traced_function_entry = bpf_program__attach_uprobe(
    skel->progs.my_traced_function_entry,  // BPF program
    false,      // retprobe = false (entry hook)
    -1,         // pid = -1 (all processes)
    lib_path,   // "./build/lib/libmylib.so"
    func_offset // 0x11a0
);

// Attach exit uretprobe
skel->links.my_traced_function_exit = bpf_program__attach_uprobe(
    skel->progs.my_traced_function_exit,   // BPF program
    true,       // retprobe = true (exit hook)
    -1,         // pid = -1 (all processes)
    lib_path,   // "./build/lib/libmylib.so"
    func_offset // 0x11a0
);
```

**What happens inside `bpf_program__attach_uprobe()`:**

a. **Calculate inode and offset:**
   - Stats the library file: `stat("./build/lib/libmylib.so")`
   - Gets inode number (unique file identifier)
   - Calculates file offset: `file_offset = func_offset`

b. **Register uprobe with kernel:**
   - Opens perf event: `perf_event_open()` syscall
   - Configures uprobe/uretprobe:
     ```c
     struct perf_event_attr attr = {
         .type = PERF_TYPE_TRACEPOINT,
         .size = sizeof(attr),
         .config = trace_id,  // Kernel trace point ID
         .sample_type = PERF_SAMPLE_RAW,
     };
     ```
   - Attaches BPF program to perf event: `ioctl(perf_fd, PERF_EVENT_IOC_SET_BPF, prog_fd)`

c. **Kernel creates breakpoint:**
   - Kernel's uprobe subsystem inserts a **software breakpoint** (INT3 on x86) at:
     - Address: `library_base_address + func_offset` for each process that has loaded the library
   - Registers the BPF program to run when breakpoint is hit
   - For uretprobe: also hijacks the return address (more details below)

8. **Setup ring buffer polling:**
```c
rb = ring_buffer__new(
    bpf_map__fd(skel->maps.events),  // File descriptor of ring buffer map
    handle_event,                     // Callback function
    NULL,                             // Context (unused)
    NULL                              // Options (default)
);
```

9. **Setup signal handler:**
```c
signal(SIGINT, sig_handler);  // Catch Ctrl-C
```

10. **Start polling loop:**
```c
printf("Tracing... Press Ctrl-C to stop.\n");
while (!exiting) {
    err = ring_buffer__poll(rb, 100);  // Poll with 100ms timeout
}
```

**What `ring_buffer__poll()` does:**
- Calls `epoll_wait()` on the ring buffer file descriptor
- When kernel writes events to the ring buffer, epoll wakes up
- Reads events from ring buffer memory (shared kernel-user memory)
- Calls `handle_event()` for each event
- Returns when timeout expires or events are processed

#### Step 4.2: Application Runs and Triggers Uprobe

In another terminal, the target application runs:

```bash
SIMULATED_WORK_US=100 ./build/bin/sample_app 10000
```

The application calls `my_traced_function()`:

```c
// In sample_app
for (int i = 0; i < 10000; i++) {
    my_traced_function(42, i, "test", 3.14, (void*)0xdeadbeef);
}
```

**What happens when `my_traced_function()` is called:**

### Phase 5: Kernel Trap and Event Capture (Entry)

#### Step 5.1: CPU Executes Breakpoint

1. **Instruction fetch:**
   - CPU fetches instruction at `my_traced_function` address
   - Finds **INT3 (0xCC)** instruction (inserted by uprobe mechanism)

2. **Hardware trap:**
   - CPU raises **#BP (Breakpoint) exception**
   - Saves current CPU state (registers, instruction pointer)
   - Switches to **kernel mode** (ring 0)
   - Jumps to kernel's breakpoint exception handler

#### Step 5.2: Kernel Uprobe Handler

1. **Kernel identifies uprobe:**
   - Exception handler looks up: "What uprobe is registered at this address?"
   - Finds: `my_traced_function_entry` BPF program

2. **Save register state:**
   - Kernel captures `struct pt_regs`:
     ```c
     struct pt_regs {
         unsigned long r15, r14, r13, r12, rbp, rbx;
         unsigned long r11, r10, r9, r8;
         unsigned long rax, rcx, rdx, rsi, rdi;  // Function arguments!
         unsigned long orig_rax;
         unsigned long rip;  // Instruction pointer
         // ... more fields
     };
     ```
   - This preserves all CPU registers including function arguments

3. **Execute BPF program:**
   - Kernel's BPF runtime executes the loaded bytecode
   - BPF program runs in **kernel context** (ring 0)
   - Has access to `pt_regs` passed as context pointer

**Inside BPF program execution:**

```c
// my_traced_function_entry() BPF program runs in kernel

// 1. Reserve space in ring buffer
struct trace_event_entry *event =
    bpf_ringbuf_reserve(&events, sizeof(*event), 0);
```
- Calls BPF helper function `bpf_ringbuf_reserve()`
- Allocates 32 bytes in the 1 MB ring buffer (shared memory)
- Returns pointer to reserved space

```c
// 2. Check allocation success
if (!event) return 0;  // Ring buffer full, skip event
```

```c
// 3. Extract function arguments from saved registers
event->arg1 = (s32)PT_REGS_PARM1(ctx);  // ctx->rdi
event->arg2 = PT_REGS_PARM2(ctx);       // ctx->rsi
event->arg3_ptr = PT_REGS_PARM3(ctx);   // ctx->rdx (pointer)
event->arg5 = PT_REGS_PARM5(ctx);       // ctx->r8
```

**How PT_REGS_PARM macros work (x86-64 calling convention):**
```c
#define PT_REGS_PARM1(x) ((x)->rdi)  // 1st arg in RDI
#define PT_REGS_PARM2(x) ((x)->rsi)  // 2nd arg in RSI
#define PT_REGS_PARM3(x) ((x)->rdx)  // 3rd arg in RDX
#define PT_REGS_PARM4(x) ((x)->rcx)  // 4th arg in RCX
#define PT_REGS_PARM5(x) ((x)->r8)   // 5th arg in R8
```

```c
// 4. Capture timestamp
event->timestamp = bpf_ktime_get_ns();
```
- Calls BPF helper to read kernel monotonic clock
- Nanosecond precision (typically from TSC - Time Stamp Counter)

```c
// 5. Set event type
event->event_type = 0;  // Entry event
```

```c
// 6. Submit event to ring buffer
bpf_ringbuf_submit(event, 0);
```
- Marks the event as complete in ring buffer
- Makes it visible to userspace
- May trigger epoll notification to wake up `ring_buffer__poll()`

```c
// 7. Return from BPF program
return 0;
```

4. **Restore execution:**
   - Kernel removes INT3 (temporarily)
   - Single-steps the original instruction
   - Re-inserts INT3
   - **Returns to user mode** (ring 3)
   - Application continues executing `my_traced_function` body

**Total time:** ~2-3 microseconds for trap, BPF execution, and return

#### Step 5.3: Userspace Receives Event

1. **Epoll wakes up:**
   - Kernel wrote event to ring buffer
   - Triggers epoll notification

2. **`ring_buffer__poll()` returns:**
   - libbpf reads event from shared memory
   - Calls `handle_event()` callback

3. **Event buffered in RAM:**
```c
static int handle_event(void *ctx, void *data, size_t data_sz) {
    // data points to struct trace_event_entry
    memcpy(&event_buffer[event_count], data, data_sz);
    event_sizes[event_count] = data_sz;
    event_count++;
    return 0;
}
```

**No file I/O happens here** - pure memory operation (~50 nanoseconds)

### Phase 6: Function Exit and Uretprobe

#### Step 6.1: Function Returns

When `my_traced_function()` executes `return`:

```c
void my_traced_function(...) {
    // ... function body ...
    return;  // <-- Return instruction
}
```

#### Step 6.2: Uretprobe Mechanism

**How uretprobe works (different from uprobe):**

When the entry uprobe was attached, the kernel **also** modified the return mechanism:

1. **On function entry** (before our BPF program ran):
   - Kernel saved the original return address from the stack
   - Replaced it with a **trampoline address** (kernel's uretprobe handler)

2. **On function return:**
   - CPU executes `ret` instruction
   - Pops return address from stack
   - **Jumps to kernel trampoline** (not original caller!)
   - This triggers the uretprobe

#### Step 6.3: Kernel Uretprobe Handler

1. **Trap to kernel:**
   - Return address was hijacked to kernel trampoline
   - CPU switches to kernel mode

2. **Execute exit BPF program:**
```c
// my_traced_function_exit() BPF program

struct trace_event_exit *event =
    bpf_ringbuf_reserve(&events, sizeof(*event), 0);

if (!event) return 0;

event->timestamp = bpf_ktime_get_ns();
event->event_type = 1;  // Exit event

bpf_ringbuf_submit(event, 0);
return 0;
```

**Much simpler than entry:**
- Only captures timestamp (12 bytes)
- No argument extraction needed
- Faster execution (~1-2 μs)

3. **Restore original return:**
   - Kernel restores the **original return address**
   - Returns to the actual caller
   - Application continues normally

#### Step 6.4: Userspace Receives Exit Event

Same as entry event:
- Ring buffer notification
- `handle_event()` called
- Event buffered in RAM

### Phase 7: Stopping and Writing Results

#### Step 7.1: User Presses Ctrl-C

```c
static void sig_handler(int sig) {
    exiting = 1;
}
```

- Sets flag to exit polling loop

#### Step 7.2: Cleanup and File Write

```c
// Main loop exits
printf("Tracing stopped. Captured %lu events.\n", event_count);

// Write all buffered events to file
printf("Writing %lu events to %s...\n", event_count, output_file);
write_events_to_file(output_file);
printf("Wrote %lu events (%lu dropped)\n", event_count, events_dropped);

// Cleanup
ring_buffer__free(rb);
mylib_tracer_bpf__destroy(skel);
free(event_buffer);
free(event_sizes);
```

**File writing (deferred, not in hot path):**
```c
static void write_events_to_file(const char *filename) {
    FILE *f = fopen(filename, "w");

    for (unsigned long i = 0; i < event_count; i++) {
        if (event_sizes[i] == sizeof(struct trace_event_entry)) {
            // Entry event
            struct trace_event_entry *e =
                (struct trace_event_entry*)&event_buffer[i];

            fprintf(f, "[%lu.%09lu] mylib:my_traced_function_entry: "
                       "{ arg1 = %d, arg2 = %lu, arg3_ptr = 0x%lx, "
                       "arg5 = 0x%lx }\n",
                    e->timestamp / 1000000000,
                    e->timestamp % 1000000000,
                    e->arg1, e->arg2, e->arg3_ptr, e->arg5);
        } else {
            // Exit event
            struct trace_event_exit *e =
                (struct trace_event_exit*)&event_buffer[i];

            fprintf(f, "[%lu.%09lu] mylib:my_traced_function_exit\n",
                    e->timestamp / 1000000000,
                    e->timestamp % 1000000000);
        }
    }

    fclose(f);
}
```

**Output example:**
```
[1728567890.123456789] mylib:my_traced_function_entry: { arg1 = 42, arg2 = 0, arg3_ptr = 0x7ffd12345678, arg5 = 0xdeadbeef }
[1728567890.123556789] mylib:my_traced_function_exit
[1728567890.123656789] mylib:my_traced_function_entry: { arg1 = 42, arg2 = 1, arg3_ptr = 0x7ffd12345678, arg5 = 0xdeadbeef }
[1728567890.123756789] mylib:my_traced_function_exit
```

### Summary: Complete Data Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ COMPILE TIME                                                     │
├─────────────────────────────────────────────────────────────────┤
│ 1. Write mylib_tracer.bpf.c (BPF program with uprobe handlers) │
│ 2. clang -target bpf → mylib_tracer.bpf.o (BPF bytecode)       │
│ 3. bpftool gen skeleton → mylib_tracer.skel.h (embedded loader) │
│ 4. gcc mylib_tracer.c → mylib_tracer (userspace executable)    │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│ TRACER STARTUP                                                   │
├─────────────────────────────────────────────────────────────────┤
│ 1. find_library() → locate libmylib.so                          │
│ 2. get_function_offset() → nm -D finds function offset (0x11a0) │
│ 3. mylib_tracer_bpf__open() → load embedded BPF bytecode        │
│ 4. mylib_tracer_bpf__load() → kernel verifies & loads BPF progs │
│ 5. bpf_program__attach_uprobe() → kernel inserts INT3 at addr   │
│ 6. ring_buffer__poll() → wait for events                        │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│ FUNCTION CALL (Entry)                                            │
├─────────────────────────────────────────────────────────────────┤
│ App: my_traced_function(42, 123, "test", ...)                   │
│  ↓                                                               │
│ CPU: Fetch instruction → INT3 (breakpoint)                       │
│  ↓                                                               │
│ HW:  Breakpoint exception → kernel mode                          │
│  ↓                                                               │
│ Kernel: Save pt_regs (all registers incl. args)                 │
│  ↓                                                               │
│ Kernel: Execute BPF program (my_traced_function_entry)           │
│  ↓                                                               │
│ BPF: bpf_ringbuf_reserve() → allocate 32 bytes                  │
│ BPF: Extract args from pt_regs (RDI, RSI, RDX, R8)              │
│ BPF: bpf_ktime_get_ns() → capture timestamp                     │
│ BPF: bpf_ringbuf_submit() → publish event                       │
│  ↓                                                               │
│ Kernel: Restore execution → user mode                            │
│  ↓                                                               │
│ App: Continue executing function body                            │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│ USERSPACE EVENT PROCESSING                                       │
├─────────────────────────────────────────────────────────────────┤
│ Ring buffer notification → epoll wakes up                        │
│  ↓                                                               │
│ ring_buffer__poll() reads from shared memory                     │
│  ↓                                                               │
│ handle_event() → memcpy to RAM buffer (NO file I/O)             │
│  ↓                                                               │
│ Continue polling...                                              │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│ FUNCTION RETURN (Exit)                                           │
├─────────────────────────────────────────────────────────────────┤
│ App: return from my_traced_function()                            │
│  ↓                                                               │
│ CPU: Pop return addr → kernel trampoline (hijacked!)             │
│  ↓                                                               │
│ Kernel: Execute BPF program (my_traced_function_exit)            │
│  ↓                                                               │
│ BPF: bpf_ringbuf_reserve() → allocate 12 bytes                  │
│ BPF: bpf_ktime_get_ns() → capture timestamp                     │
│ BPF: bpf_ringbuf_submit() → publish event                       │
│  ↓                                                               │
│ Kernel: Restore original return address                          │
│  ↓                                                               │
│ App: Return to actual caller                                     │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│ SHUTDOWN (Ctrl-C)                                                │
├─────────────────────────────────────────────────────────────────┤
│ 1. Signal handler sets exiting = 1                               │
│ 2. Polling loop exits                                            │
│ 3. write_events_to_file() → fprintf() all buffered events        │
│ 4. mylib_tracer_bpf__destroy() → detach uprobes, cleanup         │
│ 5. free() buffers                                                │
└─────────────────────────────────────────────────────────────────┘
```

### Key Insights

1. **Compilation produces BPF bytecode**, not native code - runs in kernel VM
2. **Skeleton embeds entire BPF object** - single binary deployment
3. **nm tool finds function offset** - consistent across ASLR
4. **INT3 breakpoint** triggers kernel trap - hardware-level interception
5. **pt_regs structure** preserves all registers - captures function arguments
6. **Ring buffer** is shared memory - zero-copy kernel→user transfer
7. **Uretprobe hijacks return address** - trampoline mechanism
8. **Events buffered in RAM** - file I/O deferred to end for performance
9. **BPF verifier ensures safety** - no kernel crashes possible
10. **~5 μs overhead** is constant - from context switches, not BPF execution

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
