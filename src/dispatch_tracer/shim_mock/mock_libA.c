/*
 * mock_libA.c — Low-level runtime (HSA/ROCR analogue) with dispatch table.
 * Registers with mock_register on load; shim wraps the table entries.
 */
#include "mock_libA.h"
#include "mock_register.h"
#include <stdint.h>

static volatile int g_dummy = 0;

static liba_status_t real_queue_create(liba_agent_t agent, uint32_t size,
                                        liba_queue_t* out_queue)
{
    g_dummy += (int)agent.handle + (int)size;
    if (out_queue) out_queue->handle = 0xA000 + size;
    return LIBA_STATUS_SUCCESS;
}

static liba_status_t real_memory_allocate(liba_memory_region_t region,
                                           uint64_t size, void** out_ptr)
{
    g_dummy += (int)region.base_address + (int)size;
    if (out_ptr) *out_ptr = (void*)(uintptr_t)(region.base_address + size);
    return LIBA_STATUS_SUCCESS;
}

static liba_status_t real_kernel_dispatch(liba_queue_t queue,
                                           const liba_dispatch_packet_t* pkt,
                                           liba_signal_t completion)
{
    g_dummy += (int)queue.handle;
    if (pkt) g_dummy += pkt->grid_size_x * pkt->grid_size_y * pkt->grid_size_z;
    g_dummy += (int)completion.handle;
    return LIBA_STATUS_SUCCESS;
}

static liba_status_t real_signal_wait(liba_signal_t signal, uint64_t timeout_ns)
{
    g_dummy += (int)signal.handle + (int)timeout_ns;
    return LIBA_STATUS_SUCCESS;
}

/* Mutable dispatch table — shim will rewrite these entries. */
static liba_api_table_t g_api_table = {
    .queue_create     = &real_queue_create,
    .memory_allocate  = &real_memory_allocate,
    .kernel_dispatch  = &real_kernel_dispatch,
    .signal_wait      = &real_signal_wait,
};

/* Public entry points go through the table. */
liba_status_t liba_queue_create(liba_agent_t agent, uint32_t size,
                                liba_queue_t* out_queue)
{ return g_api_table.queue_create(agent, size, out_queue); }

liba_status_t liba_memory_allocate(liba_memory_region_t region,
                                    uint64_t size, void** out_ptr)
{ return g_api_table.memory_allocate(region, size, out_ptr); }

liba_status_t liba_kernel_dispatch(liba_queue_t queue,
                                    const liba_dispatch_packet_t* pkt,
                                    liba_signal_t completion)
{ return g_api_table.kernel_dispatch(queue, pkt, completion); }

liba_status_t liba_signal_wait(liba_signal_t signal, uint64_t timeout_ns)
{ return g_api_table.signal_wait(signal, timeout_ns); }

void liba_register(void)
{
    void** slots = (void**)&g_api_table;
    uint64_t n = sizeof(liba_api_table_t) / sizeof(void*);
    mock_register_library_api_table("libA_hsa", 1, slots, n);
}

__attribute__((constructor))
static void liba_ctor(void) { liba_register(); }
