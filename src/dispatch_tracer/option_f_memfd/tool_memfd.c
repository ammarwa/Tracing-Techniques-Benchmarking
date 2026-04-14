/*
 * tool_memfd.c - libmock_sdk_tool_memfd.so
 *
 * Option F+memfd tool library. dlopen'd by the stub on the first
 * CMD_CONFIGURE. Exports rocprofiler_configure so the mock SDK's
 * rocprofiler_force_configure() can drive tool_initialize().
 *
 * tool_initialize:
 *   - Fetch stub state via the stub's rocp_stub_get_state() accessor
 *     (looked up with dlsym(RTLD_DEFAULT, ...)).
 *   - Read pending_config and create/configure/start a rocprofiler context
 *     for the domains the controller asked for.
 *   - Store the created context id in state->saved_ctx so the stub's bg
 *     thread can toggle activate/deactivate later.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rocp_protocol.h"
#include "mock_sdk.h"

/* ---------------- Trace output handling ---------------- */

static FILE*       g_out_fp       = NULL;
static _Atomic unsigned long g_events_traced = 0;

static void tool_callback(rocprofiler_callback_tracing_record_t rec, void* user_data)
{
    (void)user_data;
    if (rec.phase != ROCPROFILER_CALLBACK_PHASE_ENTER) return;
    atomic_fetch_add(&g_events_traced, 1);
    if (g_out_fp) {
        fprintf(g_out_fp,
                "[memfd-tool] kind=%d op=%u fn=%s\n",
                (int)rec.kind, rec.operation,
                rec.function_name ? rec.function_name : "?");
    }
}

/* ---------------- tool_initialize / finalize ---------------- */

static rocp_stub_state_t* get_stub_state(void)
{
    typedef rocp_stub_state_t* (*fn_t)(void);
    fn_t f = (fn_t)dlsym(RTLD_DEFAULT, "rocp_stub_get_state");
    if (!f) {
        fprintf(stderr, "[tool_memfd] rocp_stub_get_state not found: %s\n", dlerror());
        return NULL;
    }
    return f();
}

static int tool_initialize(void* fini, void* tool_data)
{
    (void)fini; (void)tool_data;

    rocp_stub_state_t* st = get_stub_state();
    if (!st || !st->ctrl || !st->pending_config || !st->saved_ctx) {
        fprintf(stderr, "[tool_memfd] stub state unavailable\n");
        return -1;
    }

    const rocp_config_t* cfg = st->pending_config;

    /* Open the output file (text format for now — JSON/OTLP/Perfetto
     * would add more backends; this mock writes a simple text line per
     * event). */
    const char* path = cfg->output_path[0] ? cfg->output_path : "/tmp/rocp_memfd_trace.txt";
    g_out_fp = fopen(path, "w");
    if (!g_out_fp) {
        fprintf(stderr, "[tool_memfd] fopen(%s) failed\n", path);
    }

    /* Create + configure + start a context. */
    rocprofiler_context_id_t ctx = { .handle = 0 };
    if (rocprofiler_create_context(&ctx) != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr, "[tool_memfd] rocprofiler_create_context failed\n");
        return -1;
    }

    if (cfg->enable_hip) {
        rocprofiler_configure_callback_tracing_service(
            ctx, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
            NULL, 0, tool_callback, NULL);
    }
    if (cfg->enable_hsa) {
        rocprofiler_configure_callback_tracing_service(
            ctx, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API,
            NULL, 0, tool_callback, NULL);
    }
    if (cfg->enable_rccl) {
        rocprofiler_configure_callback_tracing_service(
            ctx, ROCPROFILER_CALLBACK_TRACING_RCCL_API,
            NULL, 0, tool_callback, NULL);
    }
    if (cfg->enable_ompt) {
        rocprofiler_configure_callback_tracing_service(
            ctx, ROCPROFILER_CALLBACK_TRACING_OMPT,
            NULL, 0, tool_callback, NULL);
    }

    rocprofiler_start_context(ctx);

    *st->saved_ctx = ctx;
    atomic_store(&st->ctrl->context_id, ctx.handle);
    atomic_store(&st->ctrl->context_active, 1);

    return 0;
}

static void tool_finalize(void* tool_data)
{
    (void)tool_data;
    rocp_stub_state_t* st = get_stub_state();
    if (st && st->saved_ctx && st->saved_ctx->handle != 0) {
        rocprofiler_stop_context(*st->saved_ctx);
    }
    if (st && st->ctrl) {
        atomic_store(&st->ctrl->events_traced,
                     atomic_load(&g_events_traced));
    }
    if (g_out_fp) { fclose(g_out_fp); g_out_fp = NULL; }
}

/* ---------------- rocprofiler_configure (exported) ---------------- */

__attribute__((visibility("default")))
rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t version,
                      const char* runtime_version,
                      uint32_t priority,
                      void* client_id)
{
    (void)version; (void)runtime_version; (void)priority; (void)client_id;
    static rocprofiler_tool_configure_result_t result = {
        .size       = sizeof(rocprofiler_tool_configure_result_t),
        .initialize = &tool_initialize,
        .finalize   = &tool_finalize,
    };
    return &result;
}
