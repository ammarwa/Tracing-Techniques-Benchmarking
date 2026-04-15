/*
 * mock_libB.h — Mock runtime library "B" (mimics HIP-level APIs).
 *
 * Higher-level runtime that internally calls libA (HSA-level) functions.
 * This creates cross-library correlation chains: when the shim wraps
 * both tables, libB's wrapper pushes a correlation frame, then calls
 * the real libB function which calls libA → libA's wrapper pushes its
 * own frame with ancestor = libB's internal id.
 *
 * Arg structs model HIP-like complexity: dim3, streams, kernel
 * function pointers, hipDeviceProp, launch configs with nested structs.
 */
#ifndef MOCK_LIBB_H
#define MOCK_LIBB_H

#include <stddef.h>
#include <stdint.h>
#include "mock_libA.h"  /* libB depends on libA */

/* --- HIP-like struct types --- */
typedef struct { uint32_t x, y, z; } libb_dim3_t;

typedef struct { uint64_t handle; } libb_stream_t;

typedef struct {
    char     name[64];
    uint64_t total_global_mem;
    uint32_t max_threads_per_block;
    uint32_t multiprocessor_count;
    uint32_t warp_size;
    uint32_t clock_rate_khz;
    uint32_t memory_bus_width;
    uint32_t major;
    uint32_t minor;
} libb_device_prop_t;

typedef struct {
    const void*    func;          /* kernel function pointer (opaque handle) */
    libb_dim3_t    grid;
    libb_dim3_t    block;
    void**         kernarg_ptr;   /* pointer to kernel arguments */
    uint64_t       shared_mem;
    libb_stream_t  stream;
} libb_launch_config_t;

typedef enum {
    LIBB_SUCCESS        = 0,
    LIBB_ERROR_INVALID  = 1,
    LIBB_ERROR_NOMEM    = 2,
} libb_error_t;

/* --- API function table --- */
typedef struct {
    size_t size;
    libb_error_t (*launch_kernel)(const libb_launch_config_t* config);
    libb_error_t (*memcpy_async)(void* dst, const void* src,
                                  uint64_t size_bytes,
                                  libb_stream_t stream);
    libb_error_t (*stream_synchronize)(libb_stream_t stream);
    libb_error_t (*get_device_properties)(libb_device_prop_t* prop,
                                           int device_id);
} libb_api_table_t;

/* Public entry points. */
libb_error_t libb_launch_kernel(const libb_launch_config_t* config);
libb_error_t libb_memcpy_async(void* dst, const void* src,
                                uint64_t size_bytes, libb_stream_t stream);
libb_error_t libb_stream_synchronize(libb_stream_t stream);
libb_error_t libb_get_device_properties(libb_device_prop_t* prop, int device_id);

void libb_register(void);

#endif /* MOCK_LIBB_H */
