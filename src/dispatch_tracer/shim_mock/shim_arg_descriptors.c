/*
 * shim_arg_descriptors.c — Per-op arg descriptors with string formatters.
 *
 * Mimics rocprofiler-sdk's iterate_args pattern: each op has a descriptor
 * listing its arguments' names, types, and a format function that converts
 * the packed arg struct into a human-readable string at capture time.
 *
 * In the real shim, this file is code-generated from the SDK's arg metadata.
 */
#include "shim_arg_info.h"
#include "mock_libA.h"
#include "mock_libB.h"
#include "mylib_dispatch.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

/* ---- mylib op0: my_traced_function(int, uint64_t, double, void*) ---- */
typedef struct { int a1; uint64_t a2; double a3; void* a4; } packed_mylib_op0_t;
static void fmt_mylib_op0_a1(const void* p, char* d, uint32_t s) { snprintf(d,s,"%d", ((packed_mylib_op0_t*)p)->a1); }
static void fmt_mylib_op0_a2(const void* p, char* d, uint32_t s) { snprintf(d,s,"%" PRIu64, ((packed_mylib_op0_t*)p)->a2); }
static void fmt_mylib_op0_a3(const void* p, char* d, uint32_t s) { snprintf(d,s,"%.2f", ((packed_mylib_op0_t*)p)->a3); }
static void fmt_mylib_op0_a4(const void* p, char* d, uint32_t s) { snprintf(d,s,"%p", ((packed_mylib_op0_t*)p)->a4); }

/* ---- mylib op1: set_simulated_work_duration(unsigned int) ---- */
typedef struct { unsigned int us; } packed_mylib_op1_t;
static void fmt_mylib_op1_us(const void* p, char* d, uint32_t s) { snprintf(d,s,"%u", ((packed_mylib_op1_t*)p)->us); }

/* ---- libA op0: queue_create(agent, size, *out_queue) ---- */
typedef struct { liba_agent_t agent; uint32_t size; liba_queue_t* out; } packed_liba_op0_t;
static void fmt_liba_op0_agent(const void* p, char* d, uint32_t s) { snprintf(d,s,"0x%" PRIx64, ((packed_liba_op0_t*)p)->agent.handle); }
static void fmt_liba_op0_size(const void* p, char* d, uint32_t s) { snprintf(d,s,"%u", ((packed_liba_op0_t*)p)->size); }
static void fmt_liba_op0_out(const void* p, char* d, uint32_t s) {
    snprintf(d,s,"%p", (void*)((packed_liba_op0_t*)p)->out);
}

/* ---- libA op1: memory_allocate(region, size, **out_ptr) ---- */
typedef struct { liba_memory_region_t region; uint64_t size; void** out_ptr; } packed_liba_op1_t;
static void fmt_liba_op1_region(const void* p, char* d, uint32_t s) {
    const liba_memory_region_t* r = &((packed_liba_op1_t*)p)->region;
    snprintf(d,s,"{base=0x%" PRIx64 ",size=%" PRIu64 ",seg=%u,flags=0x%x}",
             r->base_address, r->size_bytes, r->segment, r->flags);
}
static void fmt_liba_op1_size(const void* p, char* d, uint32_t s) { snprintf(d,s,"%" PRIu64, ((packed_liba_op1_t*)p)->size); }
static void fmt_liba_op1_out(const void* p, char* d, uint32_t s) {
    snprintf(d,s,"%p", (void*)((packed_liba_op1_t*)p)->out_ptr);
}

/* ---- libA op2: kernel_dispatch(queue, *packet, completion) ---- */
typedef struct { liba_queue_t queue; const liba_dispatch_packet_t* pkt; liba_signal_t completion; } packed_liba_op2_t;
static void fmt_liba_op2_queue(const void* p, char* d, uint32_t s) { snprintf(d,s,"0x%" PRIx64, ((packed_liba_op2_t*)p)->queue.handle); }
static void fmt_liba_op2_pkt(const void* p, char* d, uint32_t s) {
    snprintf(d,s,"%p", (void*)((packed_liba_op2_t*)p)->pkt);
}
static void fmt_liba_op2_comp(const void* p, char* d, uint32_t s) { snprintf(d,s,"0x%" PRIx64, ((packed_liba_op2_t*)p)->completion.handle); }

/* ---- libA op3: signal_wait(signal, timeout_ns) ---- */
typedef struct { liba_signal_t signal; uint64_t timeout_ns; } packed_liba_op3_t;
static void fmt_liba_op3_sig(const void* p, char* d, uint32_t s) { snprintf(d,s,"0x%" PRIx64, ((packed_liba_op3_t*)p)->signal.handle); }
static void fmt_liba_op3_timeout(const void* p, char* d, uint32_t s) { snprintf(d,s,"%" PRIu64, ((packed_liba_op3_t*)p)->timeout_ns); }

/* ---- libB op0: launch_kernel(*config) ---- */
typedef struct { const libb_launch_config_t* config; } packed_libb_op0_t;
static void fmt_libb_op0_config(const void* p, char* d, uint32_t s) {
    /* config is a pointer into the target's stack. OOP consumer cannot
     * dereference it. Print pointer value only. Deep expansion requires
     * the shim to inline the struct into the record at EXIT time (§7B.2
     * variable-size ring). For the mock, pointer value is sufficient. */
    snprintf(d,s,"%p", (void*)((packed_libb_op0_t*)p)->config);
}

/* ---- libB op1: memcpy_async(dst, src, size, stream) ---- */
typedef struct { void* dst; const void* src; uint64_t size; libb_stream_t stream; } packed_libb_op1_t;
static void fmt_libb_op1_dst(const void* p, char* d, uint32_t s) { snprintf(d,s,"%p", ((packed_libb_op1_t*)p)->dst); }
static void fmt_libb_op1_src(const void* p, char* d, uint32_t s) { snprintf(d,s,"%p", ((packed_libb_op1_t*)p)->src); }
static void fmt_libb_op1_sz(const void* p, char* d, uint32_t s) { snprintf(d,s,"%" PRIu64, ((packed_libb_op1_t*)p)->size); }
static void fmt_libb_op1_stream(const void* p, char* d, uint32_t s) { snprintf(d,s,"0x%" PRIx64, ((packed_libb_op1_t*)p)->stream.handle); }

/* ---- libB op2: stream_synchronize(stream) ---- */
typedef struct { libb_stream_t stream; } packed_libb_op2_t;
static void fmt_libb_op2_stream(const void* p, char* d, uint32_t s) { snprintf(d,s,"0x%" PRIx64, ((packed_libb_op2_t*)p)->stream.handle); }

/* ---- libB op3: get_device_properties(*prop, device_id) ---- */
typedef struct { libb_device_prop_t* prop; int device_id; } packed_libb_op3_t;
static void fmt_libb_op3_prop(const void* p, char* d, uint32_t s) {
    /* prop is a pointer into the target's stack — OOP consumer cannot deref.
     * Deep expansion (name, mem, cu, warp, version) requires the shim to
     * inline the struct into the record at EXIT time (§7B.2 var ring). */
    snprintf(d,s,"%p", (void*)((packed_libb_op3_t*)p)->prop);
}
static void fmt_libb_op3_devid(const void* p, char* d, uint32_t s) { snprintf(d,s,"%d", ((packed_libb_op3_t*)p)->device_id); }

/* ================================================================== */
/* Deep-copy functions — called at EXIT time IN THE TARGET to inline    */
/* pointed-at structs into the record's extension area.                 */
/* Only defined for ops with pointer-to-struct args.                    */
/* ================================================================== */

static uint32_t deep_libb_op0(const void* p, uint8_t* dst, uint32_t sz) {
    const libb_launch_config_t* c = ((packed_libb_op0_t*)p)->config;
    if (!c || sz < sizeof(libb_launch_config_t)) return 0;
    memcpy(dst, c, sizeof(libb_launch_config_t));
    return sizeof(libb_launch_config_t);
}

static uint32_t deep_libb_op3(const void* p, uint8_t* dst, uint32_t sz) {
    const libb_device_prop_t* pr = ((packed_libb_op3_t*)p)->prop;
    if (!pr || sz < sizeof(libb_device_prop_t)) return 0;
    memcpy(dst, pr, sizeof(libb_device_prop_t));
    return sizeof(libb_device_prop_t);
}

static uint32_t deep_liba_op0(const void* p, uint8_t* dst, uint32_t sz) {
    const liba_queue_t* q = ((packed_liba_op0_t*)p)->out;
    if (!q || sz < sizeof(liba_queue_t)) return 0;
    memcpy(dst, q, sizeof(liba_queue_t));
    return sizeof(liba_queue_t);
}

static uint32_t deep_liba_op2(const void* p, uint8_t* dst, uint32_t sz) {
    const liba_dispatch_packet_t* pk = ((packed_liba_op2_t*)p)->pkt;
    if (!pk || sz < sizeof(liba_dispatch_packet_t)) return 0;
    memcpy(dst, pk, sizeof(liba_dispatch_packet_t));
    return sizeof(liba_dispatch_packet_t);
}

/* Deep-format functions — read from the inlined copy, not target ptrs */

static void dfmt_libb_op0_config(const void* deep, char* d, uint32_t s) {
    const libb_launch_config_t* c = (const libb_launch_config_t*)deep;
    snprintf(d,s,"{func=%p,grid={%u,%u,%u},block={%u,%u,%u},shmem=%" PRIu64 ",stream=0x%" PRIx64 "}",
             c->func, c->grid.x, c->grid.y, c->grid.z,
             c->block.x, c->block.y, c->block.z, c->shared_mem, c->stream.handle);
}

static void dfmt_libb_op3_prop(const void* deep, char* d, uint32_t s) {
    const libb_device_prop_t* pr = (const libb_device_prop_t*)deep;
    snprintf(d,s,"{name=\"%s\",mem=%" PRIu64 ",cu=%u,warp=%u,v%u.%u}",
             pr->name, pr->total_global_mem, pr->multiprocessor_count,
             pr->warp_size, pr->major, pr->minor);
}

static void dfmt_liba_op0_out(const void* deep, char* d, uint32_t s) {
    const liba_queue_t* q = (const liba_queue_t*)deep;
    snprintf(d,s,"{handle=0x%" PRIx64 "}", q->handle);
}

static void dfmt_liba_op2_pkt(const void* deep, char* d, uint32_t s) {
    const liba_dispatch_packet_t* pk = (const liba_dispatch_packet_t*)deep;
    snprintf(d,s,"{grid=%ux%ux%u,wg=%ux%ux%u,kernel=0x%" PRIx64 "}",
             pk->grid_size_x, pk->grid_size_y, pk->grid_size_z,
             pk->workgroup_size_x, pk->workgroup_size_y, pk->workgroup_size_z,
             pk->kernel_object);
}

/* ================================================================== */
/* Descriptor table — indexed by (table_name, local_op).               */
/* In the real shim this is code-generated from the SDK's arg metadata. */
/* ================================================================== */

const shim_op_arg_descriptor_t g_mylib_arg_descs[] = {
    /* op0: my_traced_function — all scalar args, no deep copy */
    { .n_args = 4, .args = {
        { "arg1",  "int",      fmt_mylib_op0_a1, NULL },
        { "arg2",  "uint64_t", fmt_mylib_op0_a2, NULL },
        { "arg3",  "double",   fmt_mylib_op0_a3, NULL },
        { "arg4",  "void*",    fmt_mylib_op0_a4, NULL },
    }, .deep_copy = NULL },
    /* op1: set_simulated_work_duration — scalar, no deep copy */
    { .n_args = 1, .args = {
        { "sleep_us", "unsigned int", fmt_mylib_op1_us, NULL },
    }, .deep_copy = NULL },
};

const shim_op_arg_descriptor_t g_liba_arg_descs[] = {
    /* op0: queue_create — out_queue is a pointer arg */
    { .n_args = 3, .args = {
        { "agent",     "liba_agent_t",  fmt_liba_op0_agent, NULL },
        { "size",      "uint32_t",      fmt_liba_op0_size,  NULL },
        { "out_queue", "liba_queue_t*", fmt_liba_op0_out,   dfmt_liba_op0_out },
    }, .deep_copy = deep_liba_op0 },
    /* op1: memory_allocate — out_ptr is pointer but can't deep-copy void** */
    { .n_args = 3, .args = {
        { "region",  "liba_memory_region_t", fmt_liba_op1_region, NULL },
        { "size",    "uint64_t",             fmt_liba_op1_size,   NULL },
        { "out_ptr", "void**",               fmt_liba_op1_out,    NULL },
    }, .deep_copy = NULL },
    /* op2: kernel_dispatch — packet is a pointer-to-struct arg */
    { .n_args = 3, .args = {
        { "queue",      "liba_queue_t",                fmt_liba_op2_queue, NULL },
        { "packet",     "const liba_dispatch_packet_t*", fmt_liba_op2_pkt, dfmt_liba_op2_pkt },
        { "completion", "liba_signal_t",               fmt_liba_op2_comp,  NULL },
    }, .deep_copy = deep_liba_op2 },
    /* op3: signal_wait — all scalar */
    { .n_args = 2, .args = {
        { "signal",     "liba_signal_t", fmt_liba_op3_sig,     NULL },
        { "timeout_ns", "uint64_t",      fmt_liba_op3_timeout, NULL },
    }, .deep_copy = NULL },
};

const shim_op_arg_descriptor_t g_libb_arg_descs[] = {
    /* op0: launch_kernel — config is a pointer-to-struct */
    { .n_args = 1, .args = {
        { "config", "const libb_launch_config_t*", fmt_libb_op0_config, dfmt_libb_op0_config },
    }, .deep_copy = deep_libb_op0 },
    /* op1: memcpy_async — all scalar/handle args */
    { .n_args = 4, .args = {
        { "dst",    "void*",          fmt_libb_op1_dst,    NULL },
        { "src",    "const void*",    fmt_libb_op1_src,    NULL },
        { "size",   "uint64_t",       fmt_libb_op1_sz,     NULL },
        { "stream", "libb_stream_t",  fmt_libb_op1_stream, NULL },
    }, .deep_copy = NULL },
    /* op2: stream_synchronize — scalar */
    { .n_args = 1, .args = {
        { "stream", "libb_stream_t", fmt_libb_op2_stream, NULL },
    }, .deep_copy = NULL },
    /* op3: get_device_properties — prop is a pointer-to-struct */
    { .n_args = 2, .args = {
        { "prop",      "libb_device_prop_t*", fmt_libb_op3_prop, dfmt_libb_op3_prop },
        { "device_id", "int",                 fmt_libb_op3_devid, NULL },
    }, .deep_copy = deep_libb_op3 },
};
