/*
 * tool_sock.c — libmock_sdk_tool_sock.so
 *
 * Linked against libmock_sdk and libmock_register. Exports
 * rocprofiler_configure so that when the stub dlopens this library and
 * calls rocprofiler_force_configure, the mock SDK's force_configure
 * lifecycle runs tool_initialize here, which creates a context, enables
 * the requested domains, and starts the context.
 *
 * Reads the controller's pending config via rocp_stub_get_state() from
 * the preloaded stub (resolved via dlsym(RTLD_DEFAULT,...)).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_sdk.h"
#include "rocp_protocol.h"

/* Retrieved lazily from the preloaded stub. */
static rocp_stub_get_state_fn_t p_get_state = NULL;

static FILE* g_output_file = NULL;

static void output_open_if_needed(const rocp_config_t* cfg)
{
    if (g_output_file) return;
    if (!cfg || cfg->output_path[0] == '\0') {
        g_output_file = stderr;
        return;
    }
    g_output_file = fopen(cfg->output_path, "w");
    if (!g_output_file) g_output_file = stderr;
}

static void my_callback(rocprofiler_callback_tracing_record_t record,
                        void* user_data)
{
    rocp_stub_state_t* state = (rocp_stub_state_t*)user_data;
    if (!state || !state->ctrl) return;

    if (record.phase == ROCPROFILER_CALLBACK_PHASE_EXIT) {
        atomic_fetch_add(&state->ctrl->events_traced, 1ull);
    }

    if (g_output_file && record.phase == ROCPROFILER_CALLBACK_PHASE_ENTER) {
        fprintf(g_output_file, "[tool_sock] %s (kind=%d op=%u)\n",
                record.function_name ? record.function_name : "?",
                (int)record.kind, record.operation);
        fflush(g_output_file);
    }
}

static rocprofiler_context_id_t g_local_ctx;
static _Atomic int g_initialized = 0;

static int tool_initialize(void* fini, void* tool_data)
{
    (void)fini;
    (void)tool_data;

    if (!p_get_state) {
        p_get_state = (rocp_stub_get_state_fn_t)
            dlsym(RTLD_DEFAULT, "rocp_stub_get_state");
    }
    if (!p_get_state) {
        fprintf(stderr, "[tool_sock] cannot resolve rocp_stub_get_state\n");
        return -1;
    }

    rocp_stub_state_t* state = p_get_state();
    if (!state || !state->pending_config) {
        fprintf(stderr, "[tool_sock] stub state unavailable\n");
        return -1;
    }

    output_open_if_needed(state->pending_config);

    rocprofiler_status_t st = rocprofiler_create_context(&g_local_ctx);
    if (st != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr, "[tool_sock] rocprofiler_create_context failed\n");
        return -1;
    }

    const rocp_config_t* cfg = state->pending_config;

    if (cfg->enable_hip) {
        rocprofiler_configure_callback_tracing_service(
            g_local_ctx, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
            NULL, 0, my_callback, (void*)state);
    }
    if (cfg->enable_hsa) {
        rocprofiler_configure_callback_tracing_service(
            g_local_ctx, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API,
            NULL, 0, my_callback, (void*)state);
    }
    if (cfg->enable_rccl) {
        rocprofiler_configure_callback_tracing_service(
            g_local_ctx, ROCPROFILER_CALLBACK_TRACING_RCCL_API,
            NULL, 0, my_callback, (void*)state);
    }
    if (cfg->enable_ompt) {
        rocprofiler_configure_callback_tracing_service(
            g_local_ctx, ROCPROFILER_CALLBACK_TRACING_OMPT,
            NULL, 0, my_callback, (void*)state);
    }

    if (state->saved_ctx) *state->saved_ctx = g_local_ctx;

    rocprofiler_start_context(g_local_ctx);
    if (state->ctrl) atomic_store(&state->ctrl->context_active, 1u);

    atomic_store(&g_initialized, 1);
    return 0;
}

static void tool_finalize(void* tool_data)
{
    (void)tool_data;
    if (!atomic_exchange(&g_initialized, 0)) return;

    rocprofiler_stop_context(g_local_ctx);
    if (p_get_state) {
        rocp_stub_state_t* state = p_get_state();
        if (state && state->ctrl) {
            atomic_store(&state->ctrl->context_active, 0u);
        }
    }
    if (g_output_file && g_output_file != stderr) {
        fclose(g_output_file);
        g_output_file = NULL;
    }
}

/* Exported — discovered by mock_sdk's force_configure via the pointer
 * we pass to it from the stub. */
__attribute__((visibility("default")))
rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t version, const char* runtime_version,
                      uint32_t priority, void* client_id)
{
    (void)version;
    (void)runtime_version;
    (void)priority;
    (void)client_id;
    static rocprofiler_tool_configure_result_t result = {
        .size       = sizeof(result),
        .initialize = tool_initialize,
        .finalize   = tool_finalize,
    };
    return &result;
}
