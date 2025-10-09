// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>

// Basic type definitions
typedef __u8 u8;
typedef __u16 u16;
typedef __u32 u32;
typedef __u64 u64;
typedef __s8 s8;
typedef __s16 s16;
typedef __s32 s32;
typedef __s64 s64;

// pt_regs structure for x86_64
struct pt_regs {
    unsigned long r15;
    unsigned long r14;
    unsigned long r13;
    unsigned long r12;
    unsigned long rbp;
    unsigned long rbx;
    unsigned long r11;
    unsigned long r10;
    unsigned long r9;
    unsigned long r8;
    unsigned long rax;
    unsigned long rcx;
    unsigned long rdx;
    unsigned long rsi;
    unsigned long rdi;
    unsigned long orig_rax;
    unsigned long rip;
    unsigned long cs;
    unsigned long eflags;
    unsigned long rsp;
    unsigned long ss;
};

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

// BPF map types
#ifndef BPF_MAP_TYPE_RINGBUF
#define BPF_MAP_TYPE_RINGBUF 27
#endif

#define MAX_STRING_LEN 64

// Optimized: Smaller event structures to reduce memory allocation overhead
// Entry event with all arguments
struct trace_event_entry {
    u64 timestamp;
    s32 arg1;
    u64 arg2;
    u64 arg3_ptr;  // Store pointer instead of copying string
    u64 arg5;
    u32 event_type;  // 0=entry
} __attribute__((packed));

// Exit event - minimal size
struct trace_event_exit {
    u64 timestamp;
    u32 event_type;  // 1=exit
} __attribute__((packed));

// Ring buffer for events - increased size for better performance
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 * 1024 * 1024);  // 1MB for high-throughput scenarios
} events SEC(".maps");

// Entry probe - optimized for speed
SEC("uprobe/my_traced_function")
int my_traced_function_entry(struct pt_regs *ctx) {
    struct trace_event_entry *event;

    // Reserve smaller event structure
    event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    // Minimal work - just capture the essentials
    event->timestamp = bpf_ktime_get_ns();
    event->event_type = 0;

    // Capture arguments directly without extra operations
    event->arg1 = (s32)PT_REGS_PARM1(ctx);
    event->arg2 = PT_REGS_PARM2(ctx);
    event->arg3_ptr = PT_REGS_PARM3(ctx);  // Just store pointer, don't copy string
    event->arg5 = PT_REGS_PARM5(ctx);

    // Submit with BPF_RB_FORCE_WAKEUP for lower latency (optional)
    bpf_ringbuf_submit(event, 0);
    return 0;
}

// Exit probe - minimal overhead
SEC("uretprobe/my_traced_function")
int my_traced_function_exit(struct pt_regs *ctx) {
    struct trace_event_exit *event;

    // Reserve minimal event structure
    event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
    if (!event)
        return 0;

    event->timestamp = bpf_ktime_get_ns();
    event->event_type = 1;

    bpf_ringbuf_submit(event, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
