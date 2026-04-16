/*
 * mock_rocp_sdk.h — Mock of rocprofiler-sdk's buffer tracing API.
 *
 * Provides the subset of the SDK needed for the shim mock:
 * force_configure (multi-client), create_buffer, create_context,
 * configure_buffer_tracing_service, start/stop_context, flush/destroy.
 *
 * The SDK wraps dispatch tables and generates buffer records. The shim
 * provides the ring buffer backing store via mock_sdk_set_record_sink().
 */
#ifndef MOCK_ROCP_SDK_H
#define MOCK_ROCP_SDK_H

#include <stddef.h>
#include <stdint.h>
#include "shim_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    ROCP_STATUS_SUCCESS = 0,
    ROCP_STATUS_ERROR   = 1,
} rocp_status_t;

typedef struct { uint64_t handle; } rocp_context_id_t;
typedef struct { uint64_t handle; } rocp_buffer_id_t;

typedef void (*rocp_buffer_callback_t)(rocp_context_id_t ctx,
                                       rocp_buffer_id_t  buffer_id,
                                       const shim_buffer_record_t* records,
                                       uint64_t          n_records,
                                       void*             user_data,
                                       uint64_t          drop_count);

typedef struct {
    size_t size;
    int  (*initialize)(void* fini, void* tool_data);
    void (*finalize)(void* tool_data);
} rocp_tool_configure_result_t;

typedef rocp_tool_configure_result_t* (*rocp_configure_func_t)(
    uint32_t version, const char* runtime_version,
    uint32_t priority, void* client_id);

/* ------------------------------------------------------------------ */
/* Public API — these are the functions the consumer calls              */
/* (in the real SDK, or via libroc-shim-consumer.so proxy)             */
/* ------------------------------------------------------------------ */

rocp_status_t rocp_force_configure(rocp_configure_func_t configure_func);

rocp_status_t rocp_create_context(rocp_context_id_t* ctx);

rocp_status_t rocp_create_buffer(rocp_context_id_t ctx,
                                 uint64_t          size,
                                 uint64_t          watermark,
                                 rocp_buffer_callback_t callback,
                                 void*             user_data,
                                 rocp_buffer_id_t* buffer);

rocp_status_t rocp_configure_buffer_tracing_service(
    rocp_context_id_t            ctx,
    shim_buffer_tracing_kind_t   kind,
    uint32_t*                    operations,
    size_t                       op_count,
    rocp_buffer_id_t             buffer);

rocp_status_t rocp_start_context(rocp_context_id_t ctx);
rocp_status_t rocp_stop_context(rocp_context_id_t ctx);
rocp_status_t rocp_flush_buffer(rocp_buffer_id_t buffer);
rocp_status_t rocp_destroy_buffer(rocp_buffer_id_t buffer);
rocp_status_t rocp_destroy_context(rocp_context_id_t ctx);

/* ------------------------------------------------------------------ */
/* Internal API — called by the shim to wire up the record sink        */
/* ------------------------------------------------------------------ */

/* Record sink callback: SDK calls this to emit each buffer record.
 * The shim provides an implementation that writes into the ring. */
typedef int (*mock_sdk_record_sink_fn)(uint64_t           buffer_id,
                                       const shim_buffer_record_t* record,
                                       void*              user_data);

/* Watermark callback: SDK calls this when the buffer hits watermark. */
typedef void (*mock_sdk_watermark_fn)(uint64_t buffer_id, void* user_data);

void mock_sdk_set_record_sink(uint64_t buffer_id,
                              mock_sdk_record_sink_fn sink,
                              mock_sdk_watermark_fn   watermark_cb,
                              void*                   user_data);

/* Register set_api_table — called by mock_register for table walking. */
int mock_sdk_set_api_table(const char*  name,
                           uint64_t     lib_version,
                           uint64_t     lib_instance,
                           void**       tables,
                           uint64_t     num_tables);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_ROCP_SDK_H */
