/*
 * tool_signal.c - libmock_sdk_tool_signal.so
 *
 * Identical in behavior to the mmap channel's tool_mmap.c. The signal in Option
 * Signal is just a wake mechanism used by the stub — the tool library
 * itself is oblivious to how the stub got woken up. It still:
 *   - exports rocprofiler_configure
 *   - reads the stub's pending_config via rocp_stub_get_state()
 *   - creates a context + registers domain callbacks
 *   - starts the context
 *   - emits events via the tool callback, honoring the runtime filter
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rocp_protocol.h"
#include "mock_sdk.h"

typedef rocp_stub_state_t* (*get_state_fn_t)(void);

static rocprofiler_context_id_t s_saved_ctx   = {0};
static _Atomic int              s_finalized   = 0;
static pthread_mutex_t          s_output_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE*                    s_output_file = NULL;
static char                     s_output_path[256] = {0};

/* Filter symbols exported by the signal stub. */
extern _Atomic uint32_t g_runtime_domain_mask;
extern _Atomic uint32_t g_runtime_output_fmt;

static rocp_stub_state_t* get_stub_state(void)
{
    get_state_fn_t fn =
        (get_state_fn_t)dlsym(RTLD_DEFAULT, "rocp_stub_get_state");
    if (!fn) return NULL;
    return fn();
}

static void open_output_locked(const char* path)
{
    if (s_output_file) { fclose(s_output_file); s_output_file = NULL; }
    if (!path || !path[0]) return;
    s_output_file = fopen(path, "w");
    if (s_output_file) {
        snprintf(s_output_path, sizeof(s_output_path), "%s", path);
    } else {
        fprintf(stderr,
                "[rocp_tool_signal] could not open output file '%s'\n", path);
    }
}

static void my_callback(rocprofiler_callback_tracing_record_t record,
                        void* user_data)
{
    rocp_stub_state_t* state = (rocp_stub_state_t*)user_data;

    uint32_t mask = atomic_load_explicit(&g_runtime_domain_mask, memory_order_acquire);
    if (record.kind >= 0 && record.kind < 32 &&
        !(mask & (1u << (uint32_t)record.kind))) {
        if (state && state->ctrl) {
            atomic_fetch_add_explicit(&state->ctrl->events_dropped, 1ull,
                               memory_order_relaxed);
        }
        return;
    }

    if (record.phase != ROCPROFILER_CALLBACK_PHASE_ENTER) return;

    pthread_mutex_lock(&s_output_lock);
    if (s_output_file) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        fprintf(s_output_file,
                "%lld.%09ld kind=%u op=%u fn=%s\n",
                (long long)ts.tv_sec, (long)ts.tv_nsec,
                (unsigned)record.kind, (unsigned)record.operation,
                record.function_name ? record.function_name : "?");
        fflush(s_output_file);
    }
    pthread_mutex_unlock(&s_output_lock);

    if (state && state->ctrl) {
        atomic_fetch_add_explicit(&state->ctrl->events_traced, 1ull,
                           memory_order_relaxed);
    }
}

static int tool_initialize(void* fini, void* tool_data)
{
    (void)fini; (void)tool_data;

    rocp_stub_state_t* state = get_stub_state();
    if (!state || !state->pending_config) {
        fprintf(stderr, "[rocp_tool_signal] no stub state available\n");
        return -1;
    }
    const rocp_config_t* cfg = state->pending_config;

    pthread_mutex_lock(&s_output_lock);
    open_output_locked(cfg->output_path);
    pthread_mutex_unlock(&s_output_lock);

    rocprofiler_status_t st = rocprofiler_create_context(&s_saved_ctx);
    if (st != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr, "[rocp_tool_signal] create_context failed: %d\n",
                (int)st);
        return -1;
    }

    int any = cfg->enable_hip || cfg->enable_hsa ||
              cfg->enable_rccl || cfg->enable_ompt;
    if (!any || cfg->enable_hip) {
        rocprofiler_configure_callback_tracing_service(
            s_saved_ctx, ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
            NULL, 0, my_callback, (void*)state);
    }
    if (cfg->enable_hsa) {
        rocprofiler_configure_callback_tracing_service(
            s_saved_ctx, ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API,
            NULL, 0, my_callback, (void*)state);
    }
    if (cfg->enable_rccl) {
        rocprofiler_configure_callback_tracing_service(
            s_saved_ctx, ROCPROFILER_CALLBACK_TRACING_RCCL_API,
            NULL, 0, my_callback, (void*)state);
    }
    if (cfg->enable_ompt) {
        rocprofiler_configure_callback_tracing_service(
            s_saved_ctx, ROCPROFILER_CALLBACK_TRACING_OMPT,
            NULL, 0, my_callback, (void*)state);
    }

    rocprofiler_start_context(s_saved_ctx);

    *state->saved_ctx = s_saved_ctx;
    if (state->ctrl) {
        atomic_store_explicit(&state->ctrl->context_id,
                         (uint64_t)s_saved_ctx.handle, memory_order_release);
        atomic_store_explicit(&state->ctrl->context_active, 1u, memory_order_release);
    }
    return 0;
}

static void tool_finalize(void* tool_data)
{
    (void)tool_data;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_finalized, &expected, 1,
                                     memory_order_seq_cst,
                                     memory_order_seq_cst))
        return;

    rocp_stub_state_t* state = get_stub_state();
    if (state && state->ctrl &&
        atomic_load_explicit(&state->ctrl->context_active, memory_order_acquire)) {
        rocprofiler_stop_context(s_saved_ctx);
        atomic_store_explicit(&state->ctrl->context_active, 0u, memory_order_release);
    }

    pthread_mutex_lock(&s_output_lock);
    if (s_output_file) {
        fclose(s_output_file);
        s_output_file = NULL;
    }
    pthread_mutex_unlock(&s_output_lock);
}

__attribute__((visibility("default")))
rocprofiler_tool_configure_result_t*
rocprofiler_configure(uint32_t version, const char* runtime_version,
                      uint32_t priority, void* client_id)
{
    (void)version; (void)runtime_version; (void)priority; (void)client_id;
    static rocprofiler_tool_configure_result_t result = {
        .size       = sizeof(rocprofiler_tool_configure_result_t),
        .initialize = tool_initialize,
        .finalize   = tool_finalize,
    };
    return &result;
}

__attribute__((destructor))
static void tool_dtor(void)
{
    tool_finalize(NULL);
}
