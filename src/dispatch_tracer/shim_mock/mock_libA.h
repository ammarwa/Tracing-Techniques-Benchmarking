/*
 * mock_libA.h — Mock runtime library "A" (mimics HSA/ROCR-level APIs).
 *
 * Models a low-level runtime with complex struct args:
 *   - Opaque handles (queues, signals, agents)
 *   - Nested structs (memory region descriptors)
 *   - Pointer args (kernel dispatch packets)
 *   - Enum args
 *
 * libB (HIP-level) calls into libA (HSA-level) internally, creating
 * cross-library correlation chains via the shim's ancestor field.
 */
#ifndef MOCK_LIBA_H
#define MOCK_LIBA_H

#include <stdint.h>

/* --- Opaque handles (like hsa_queue_t*, hsa_signal_t, hsa_agent_t) --- */
typedef struct { uint64_t handle; } liba_queue_t;
typedef struct { uint64_t handle; } liba_signal_t;
typedef struct { uint64_t handle; } liba_agent_t;

/* --- Nested struct (like hsa_region_t + hsa_amd_memory_pool_t) --- */
typedef struct {
    uint64_t base_address;
    uint64_t size_bytes;
    uint32_t segment;       /* 0=GLOBAL, 1=READONLY, 2=PRIVATE, 3=GROUP */
    uint32_t flags;
} liba_memory_region_t;

/* --- Kernel dispatch packet (like hsa_kernel_dispatch_packet_t) --- */
typedef struct {
    uint16_t header;
    uint16_t setup;
    uint16_t workgroup_size_x;
    uint16_t workgroup_size_y;
    uint16_t workgroup_size_z;
    uint16_t reserved0;
    uint32_t grid_size_x;
    uint32_t grid_size_y;
    uint32_t grid_size_z;
    uint32_t private_segment_size;
    uint32_t group_segment_size;
    uint64_t kernel_object;
    void*    kernarg_address;
    uint64_t completion_signal_handle;
} liba_dispatch_packet_t;

/* --- Enum (like hsa_status_t) --- */
typedef enum {
    LIBA_STATUS_SUCCESS = 0,
    LIBA_STATUS_ERROR   = 1,
    LIBA_STATUS_BUSY    = 2,
} liba_status_t;

/* --- API function table (dispatch-table pattern) --- */
typedef struct {
    liba_status_t (*queue_create)(liba_agent_t agent,
                                  uint32_t size,
                                  liba_queue_t* out_queue);
    liba_status_t (*memory_allocate)(liba_memory_region_t region,
                                     uint64_t size,
                                     void** out_ptr);
    liba_status_t (*kernel_dispatch)(liba_queue_t queue,
                                     const liba_dispatch_packet_t* packet,
                                     liba_signal_t completion);
    liba_status_t (*signal_wait)(liba_signal_t signal,
                                 uint64_t timeout_ns);
} liba_api_table_t;

/* Public entry points (go through the dispatch table). */
liba_status_t liba_queue_create(liba_agent_t agent, uint32_t size,
                                liba_queue_t* out_queue);
liba_status_t liba_memory_allocate(liba_memory_region_t region,
                                    uint64_t size, void** out_ptr);
liba_status_t liba_kernel_dispatch(liba_queue_t queue,
                                    const liba_dispatch_packet_t* packet,
                                    liba_signal_t completion);
liba_status_t liba_signal_wait(liba_signal_t signal, uint64_t timeout_ns);

/* Register with mock_register (called from constructor). */
void liba_register(void);

#endif /* MOCK_LIBA_H */
