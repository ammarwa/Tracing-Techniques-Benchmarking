/*
 * mock_sdk.h - Public API mimicking the subset of rocprofiler-sdk we need.
 *
 * Provides:
 *  - rocprofiler_force_configure()   (one-shot, returns LOCKED on re-entry)
 *  - rocprofiler_create_context()
 *  - rocprofiler_configure_callback_tracing_service()
 *  - rocprofiler_start_context() / rocprofiler_stop_context()
 *
 * Plus the dispatch-table mechanism: mock_sdk_set_api_table() is registered
 * into libmock_register and invoked from each runtime registration. It
 * copies originals (copy_table) and installs wrapper functors that check
 * active contexts on every call and fall through to the original when no
 * context cares (populate_contexts / should_wrap_functor equivalent).
 */
#ifndef MOCK_SDK_H
#define MOCK_SDK_H

#include <stddef.h>
#include <stdint.h>

#include "rocp_protocol.h"  /* pulls in rocprofiler_context_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. */
typedef enum {
    ROCPROFILER_STATUS_SUCCESS                      = 0,
    ROCPROFILER_STATUS_ERROR_CONFIGURATION_LOCKED   = 1,
    ROCPROFILER_STATUS_ERROR                        = 2,
} rocprofiler_status_t;

/* Tracing domain kinds. */
typedef enum {
    ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API = 0,
    ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API    = 1,
    ROCPROFILER_CALLBACK_TRACING_RCCL_API        = 2,
    ROCPROFILER_CALLBACK_TRACING_OMPT            = 3,
    ROCPROFILER_CALLBACK_TRACING_NUM             = 4,
} rocprofiler_callback_tracing_kind_t;

/* Callback phase. */
typedef enum {
    ROCPROFILER_CALLBACK_PHASE_ENTER = 0,
    ROCPROFILER_CALLBACK_PHASE_EXIT  = 1,
} rocprofiler_callback_phase_t;

/* Record delivered to each tool callback. */
typedef struct {
    rocprofiler_callback_tracing_kind_t kind;
    uint32_t                            operation;
    rocprofiler_callback_phase_t        phase;
    const char*                         function_name;
    void*                               args;
} rocprofiler_callback_tracing_record_t;

/* Tool callback signature. */
typedef void (*rocprofiler_callback_tracing_cb_t)(
    rocprofiler_callback_tracing_record_t record,
    void* user_data);

/* Tool configure result (what rocprofiler_configure returns). */
typedef struct rocprofiler_tool_configure_result_t {
    size_t size;
    int  (*initialize)(void* fini, void* tool_data);
    void (*finalize)(void* tool_data);
} rocprofiler_tool_configure_result_t;

/* Tool's exported entry point. */
typedef rocprofiler_tool_configure_result_t* (*rocprofiler_configure_func_t)(
    uint32_t version,
    const char* runtime_version,
    uint32_t priority,
    void* client_id);

/* ---------- Public SDK API ---------- */

/* One-shot late-load entry point. Returns LOCKED if init_status != 0 or a
 * forced configure has already been accepted. */
rocprofiler_status_t rocprofiler_force_configure(
    rocprofiler_configure_func_t configure_func);

/* Allocate a new context. */
rocprofiler_status_t rocprofiler_create_context(
    rocprofiler_context_id_t* ctx);

/* Register a callback service for a given domain on a context.
 * operations / op_count identify which ops within the domain to trace
 * (NULL/0 means all ops). */
rocprofiler_status_t rocprofiler_configure_callback_tracing_service(
    rocprofiler_context_id_t            ctx,
    rocprofiler_callback_tracing_kind_t kind,
    void*                               operations,
    size_t                              op_count,
    rocprofiler_callback_tracing_cb_t   cb,
    void*                               user_data);

rocprofiler_status_t rocprofiler_start_context(rocprofiler_context_id_t ctx);
rocprofiler_status_t rocprofiler_stop_context(rocprofiler_context_id_t ctx);

/* ---------- Dispatch-table mechanism ----------
 *
 * Entry point invoked by mock_register once the SDK is loaded. Matches the
 * mock_register_set_api_table_fn_t callback signature:
 *   (name, lib_version, lib_instance, tables, num_tables)
 *
 * tables[i] is a POINTER TO a table struct whose first field is
 * `size_t size` (total byte size of the struct), followed by function
 * pointers. The implementation dereferences tables[0], reads the size
 * field, and computes the number of function-pointer slots as:
 *   num_fn_entries = (size - sizeof(size_t)) / sizeof(void*)
 *
 * Does what rocprofiler-sdk's rocprofiler_set_api_table does:
 *   - copy_table(): stash originals
 *   - update_table(): replace pointers with wrapper functors for ops that
 *     any registered context cares about (should_wrap_functor).
 */
int mock_sdk_set_api_table(const char*  name,
                           uint64_t     lib_version,
                           uint64_t     lib_instance,
                           void**       tables,
                           uint64_t     num_tables);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_SDK_H */
