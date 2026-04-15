/*
 * mock_libB.c — High-level runtime (HIP analogue) that calls into libA.
 *
 * launch_kernel internally:
 *   1. Calls liba_queue_create (HSA queue setup)
 *   2. Builds an HSA dispatch packet from the HIP launch config
 *   3. Calls liba_kernel_dispatch (HSA dispatch)
 *   4. Calls liba_signal_wait (wait for completion)
 *
 * When both tables are shim-wrapped, this produces:
 *   ENTER libB.launch_kernel  corr={i=N, a=0}       ← top-level
 *     ENTER libA.queue_create   corr={i=N+1, a=N}   ← ancestor = libB's id
 *     EXIT  libA.queue_create
 *     ENTER libA.kernel_dispatch corr={i=N+2, a=N}
 *     EXIT  libA.kernel_dispatch
 *     ENTER libA.signal_wait    corr={i=N+3, a=N}
 *     EXIT  libA.signal_wait
 *   EXIT  libB.launch_kernel
 *
 * Consumer reconstructs the tree by joining ancestor → internal.
 */
#include "mock_libB.h"
#include "mock_register.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static volatile int g_dummy = 0;

static libb_error_t real_launch_kernel(const libb_launch_config_t* cfg)
{
    if (!cfg) return LIBB_ERROR_INVALID;

    /* Internally calls libA (HSA-level) — creates cross-library chain. */
    liba_agent_t agent = { .handle = 0x1 };
    liba_queue_t queue;
    liba_queue_create(agent, 1024, &queue);

    liba_dispatch_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.workgroup_size_x = (uint16_t)cfg->block.x;
    pkt.workgroup_size_y = (uint16_t)cfg->block.y;
    pkt.workgroup_size_z = (uint16_t)cfg->block.z;
    pkt.grid_size_x      = cfg->grid.x * cfg->block.x;
    pkt.grid_size_y      = cfg->grid.y * cfg->block.y;
    pkt.grid_size_z      = cfg->grid.z * cfg->block.z;
    pkt.kernel_object    = (uint64_t)(uintptr_t)cfg->func;
    pkt.kernarg_address  = (void*)cfg->kernarg_ptr;
    pkt.private_segment_size = 0;
    pkt.group_segment_size   = (uint32_t)cfg->shared_mem;

    liba_signal_t completion = { .handle = 0xC0DE };
    pkt.completion_signal_handle = completion.handle;

    liba_kernel_dispatch(queue, &pkt, completion);
    liba_signal_wait(completion, 1000000);

    return LIBB_SUCCESS;
}

static libb_error_t real_memcpy_async(void* dst, const void* src,
                                       uint64_t size_bytes,
                                       libb_stream_t stream)
{
    /* Internally calls libA memory_allocate to simulate HSA involvement. */
    liba_memory_region_t region = {
        .base_address = (uint64_t)(uintptr_t)src,
        .size_bytes   = size_bytes,
        .segment      = 0, /* GLOBAL */
        .flags        = 0,
    };
    void* tmp = NULL;
    liba_memory_allocate(region, size_bytes, &tmp);

    g_dummy += (int)(uintptr_t)dst + (int)size_bytes + (int)stream.handle;
    return LIBB_SUCCESS;
}

static libb_error_t real_stream_synchronize(libb_stream_t stream)
{
    liba_signal_t sig = { .handle = stream.handle };
    liba_signal_wait(sig, 0);
    return LIBB_SUCCESS;
}

static libb_error_t real_get_device_properties(libb_device_prop_t* prop,
                                                int device_id)
{
    if (!prop) return LIBB_ERROR_INVALID;
    memset(prop, 0, sizeof(*prop));
    snprintf(prop->name, sizeof(prop->name), "Mock GPU %d", device_id);
    prop->total_global_mem       = 16ULL * 1024 * 1024 * 1024;
    prop->max_threads_per_block  = 1024;
    prop->multiprocessor_count   = 120;
    prop->warp_size              = 64;
    prop->clock_rate_khz         = 1700000;
    prop->memory_bus_width       = 4096;
    prop->major                  = 9;
    prop->minor                  = 4;
    return LIBB_SUCCESS;
}

/* Mutable dispatch table. */
static libb_api_table_t g_api_table = {
    .launch_kernel       = &real_launch_kernel,
    .memcpy_async        = &real_memcpy_async,
    .stream_synchronize  = &real_stream_synchronize,
    .get_device_properties = &real_get_device_properties,
};

/* Public entry points go through the table. */
libb_error_t libb_launch_kernel(const libb_launch_config_t* cfg)
{ return g_api_table.launch_kernel(cfg); }

libb_error_t libb_memcpy_async(void* dst, const void* src,
                                uint64_t size, libb_stream_t stream)
{ return g_api_table.memcpy_async(dst, src, size, stream); }

libb_error_t libb_stream_synchronize(libb_stream_t stream)
{ return g_api_table.stream_synchronize(stream); }

libb_error_t libb_get_device_properties(libb_device_prop_t* prop, int device_id)
{ return g_api_table.get_device_properties(prop, device_id); }

void libb_register(void)
{
    void** slots = (void**)&g_api_table;
    uint64_t n = sizeof(libb_api_table_t) / sizeof(void*);
    mock_register_library_api_table("libB_hip", 1, slots, n);
}

__attribute__((constructor))
static void libb_ctor(void) { libb_register(); }
