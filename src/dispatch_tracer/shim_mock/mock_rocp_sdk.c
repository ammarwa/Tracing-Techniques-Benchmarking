/*
 * mock_rocp_sdk.c — Mock of rocprofiler-sdk with buffer tracing.
 *
 * This mock implements the SDK's role in the new architecture:
 *  - force_configure (multi-client, not one-shot)
 *  - Dispatch table wrapping (the SDK does this, not the shim)
 *  - Buffer tracing record generation into a sink (shim's ring)
 *  - Context/buffer lifecycle management
 *
 * The shim calls mock_sdk_set_record_sink() to provide the ring.
 * On each traced API call, the SDK emplace writes a buffer record
 * via the sink callback.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "mock_rocp_sdk.h"
#include "mock_register.h"
#include "mylib_dispatch.h"
#include "mock_libA.h"
#include "mock_libB.h"

/* ------------------------------------------------------------------ */
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

#define MAX_CLIENTS    4
#define MAX_CONTEXTS   16
#define MAX_BUFFERS    16
#define MAX_TABLES     16
#define MAX_OPS        32

typedef struct {
    int              in_use;
    uint64_t         handle;
    _Atomic int      active;
    uint64_t         buffer_handle;
    uint32_t         traced_kinds;
    uint32_t         op_mask[SHIM_BUF_TRACING_NUM];
    int              all_ops[SHIM_BUF_TRACING_NUM];
} sdk_context_t;

typedef struct {
    int                        in_use;
    uint64_t                   handle;
    uint64_t                   size;
    uint64_t                   watermark;
    rocp_buffer_callback_t     user_callback;
    void*                      user_data;
    mock_sdk_record_sink_fn    sink;
    mock_sdk_watermark_fn      watermark_cb;
    void*                      sink_data;
    _Atomic uint64_t           records_written;
    _Atomic uint64_t           records_dropped;
    _Atomic uint64_t           bytes_since_wm;
} sdk_buffer_t;

typedef struct {
    int       in_use;
    char      name[64];
    void**    fn_slots;
    uint64_t  n_entries;
    void*     originals[MAX_OPS];
    shim_buffer_tracing_kind_t kind;
} sdk_table_t;

static sdk_context_t    g_contexts[MAX_CONTEXTS];
static sdk_buffer_t     g_buffers[MAX_BUFFERS];
static sdk_table_t      g_tables[MAX_TABLES];
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_next_handle = 1;
static _Atomic uint64_t g_correlation_counter = 1;
static int              g_n_clients = 0;

/* ------------------------------------------------------------------ */
/* Table indices (set during set_api_table)                            */
/* ------------------------------------------------------------------ */

static int g_mylib_idx = -1;
static int g_liba_idx  = -1;
static int g_libb_idx  = -1;

/* ------------------------------------------------------------------ */
/* Record emission                                                     */
/* ------------------------------------------------------------------ */

static void emit_record(shim_buffer_tracing_kind_t kind, uint32_t op,
                        uint64_t start_tsc, uint64_t end_tsc,
                        uint64_t corr_id)
{
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        sdk_context_t* c = &g_contexts[i];
        if (!c->in_use) continue;
        if (atomic_load_explicit(&c->active, memory_order_acquire) == 0) continue;
        if (!(c->traced_kinds & (1u << kind))) continue;
        if (!c->all_ops[kind] && !(c->op_mask[kind] & (1u << op))) continue;

        for (int j = 0; j < MAX_BUFFERS; j++) {
            sdk_buffer_t* b = &g_buffers[j];
            if (!b->in_use || b->handle != c->buffer_handle) continue;

            shim_buffer_record_t rec = {
                .size             = sizeof(shim_buffer_record_t),
                .kind             = kind,
                .operation        = op,
                .correlation_id   = corr_id,
                .start_timestamp  = start_tsc,
                .end_timestamp    = end_tsc,
                .thread_id        = (uint64_t)syscall(SYS_gettid),
            };

            if (b->sink) {
                int rc = b->sink(b->handle, &rec, b->sink_data);
                if (rc < 0) {
                    atomic_fetch_add(&b->records_dropped, 1);
                } else {
                    atomic_fetch_add(&b->records_written, 1);
                    uint64_t acc = atomic_fetch_add(&b->bytes_since_wm,
                        sizeof(shim_buffer_record_t)) + sizeof(shim_buffer_record_t);
                    if (b->watermark > 0 && b->watermark_cb && acc >= b->watermark) {
                        atomic_store(&b->bytes_since_wm, 0);
                        b->watermark_cb(b->handle, b->sink_data);
                    }
                }
            } else {
                atomic_fetch_add(&b->records_dropped, 1);
            }
            break;
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Dispatch table wrappers — SDK wraps these (not the shim)            */
/* ------------------------------------------------------------------ */

/* mylib wrappers */
typedef void (*mylib_op0_fn_t)(int, uint64_t, double, void*);
typedef void (*mylib_op1_fn_t)(unsigned int);

static void sdk_wrap_mylib_op0(int a1, uint64_t a2, double a3, void* a4)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    mylib_op0_fn_t orig = (mylib_op0_fn_t)g_tables[g_mylib_idx].originals[0];
    orig(a1, a2, a3, a4);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HIP_RUNTIME_API, 0, t0, t1, corr);
}

static void sdk_wrap_mylib_op1(unsigned int us)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    mylib_op1_fn_t orig = (mylib_op1_fn_t)g_tables[g_mylib_idx].originals[1];
    orig(us);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HIP_RUNTIME_API, 1, t0, t1, corr);
}

/* libA wrappers */
typedef liba_status_t (*liba_op0_fn_t)(liba_agent_t, uint32_t, liba_queue_t*);
typedef liba_status_t (*liba_op1_fn_t)(liba_memory_region_t, uint64_t, void**);
typedef liba_status_t (*liba_op2_fn_t)(liba_queue_t, const liba_dispatch_packet_t*, liba_signal_t);
typedef liba_status_t (*liba_op3_fn_t)(liba_signal_t, uint64_t);

static liba_status_t sdk_wrap_liba_op0(liba_agent_t a, uint32_t sz, liba_queue_t* q)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    liba_status_t r = ((liba_op0_fn_t)g_tables[g_liba_idx].originals[0])(a, sz, q);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HSA_CORE_API, 0, t0, t1, corr);
    return r;
}

static liba_status_t sdk_wrap_liba_op1(liba_memory_region_t r, uint64_t sz, void** p)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    liba_status_t s = ((liba_op1_fn_t)g_tables[g_liba_idx].originals[1])(r, sz, p);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HSA_CORE_API, 1, t0, t1, corr);
    return s;
}

static liba_status_t sdk_wrap_liba_op2(liba_queue_t q, const liba_dispatch_packet_t* p, liba_signal_t c)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    liba_status_t s = ((liba_op2_fn_t)g_tables[g_liba_idx].originals[2])(q, p, c);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HSA_CORE_API, 2, t0, t1, corr);
    return s;
}

static liba_status_t sdk_wrap_liba_op3(liba_signal_t s, uint64_t t)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    liba_status_t st = ((liba_op3_fn_t)g_tables[g_liba_idx].originals[3])(s, t);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HSA_CORE_API, 3, t0, t1, corr);
    return st;
}

/* libB wrappers */
typedef libb_error_t (*libb_op0_fn_t)(const libb_launch_config_t*);
typedef libb_error_t (*libb_op1_fn_t)(void*, const void*, uint64_t, libb_stream_t);
typedef libb_error_t (*libb_op2_fn_t)(libb_stream_t);
typedef libb_error_t (*libb_op3_fn_t)(libb_device_prop_t*, int);

static libb_error_t sdk_wrap_libb_op0(const libb_launch_config_t* cfg)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    libb_error_t r = ((libb_op0_fn_t)g_tables[g_libb_idx].originals[0])(cfg);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HIP_RUNTIME_API, 2, t0, t1, corr);
    return r;
}

static libb_error_t sdk_wrap_libb_op1(void* d, const void* s, uint64_t sz, libb_stream_t st)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    libb_error_t r = ((libb_op1_fn_t)g_tables[g_libb_idx].originals[1])(d, s, sz, st);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HIP_RUNTIME_API, 3, t0, t1, corr);
    return r;
}

static libb_error_t sdk_wrap_libb_op2(libb_stream_t st)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    libb_error_t r = ((libb_op2_fn_t)g_tables[g_libb_idx].originals[2])(st);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HIP_RUNTIME_API, 4, t0, t1, corr);
    return r;
}

static libb_error_t sdk_wrap_libb_op3(libb_device_prop_t* p, int dev)
{
    uint64_t corr = atomic_fetch_add(&g_correlation_counter, 1);
    uint64_t t0 = shim_rdtsc();
    libb_error_t r = ((libb_op3_fn_t)g_tables[g_libb_idx].originals[3])(p, dev);
    uint64_t t1 = shim_rdtsc();
    emit_record(SHIM_BUF_TRACING_HIP_RUNTIME_API, 5, t0, t1, corr);
    return r;
}

/* Wrapper tables */
static void* g_mylib_wrappers[] = {
    (void*)sdk_wrap_mylib_op0, (void*)sdk_wrap_mylib_op1
};
static void* g_liba_wrappers[] = {
    (void*)sdk_wrap_liba_op0, (void*)sdk_wrap_liba_op1,
    (void*)sdk_wrap_liba_op2, (void*)sdk_wrap_liba_op3
};
static void* g_libb_wrappers[] = {
    (void*)sdk_wrap_libb_op0, (void*)sdk_wrap_libb_op1,
    (void*)sdk_wrap_libb_op2, (void*)sdk_wrap_libb_op3
};

/* ------------------------------------------------------------------ */
/* Table installation                                                  */
/* ------------------------------------------------------------------ */

static int should_wrap(shim_buffer_tracing_kind_t kind, uint32_t op)
{
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        sdk_context_t* c = &g_contexts[i];
        if (!c->in_use) continue;
        if (!(c->traced_kinds & (1u << kind))) continue;
        if (c->all_ops[kind] || (c->op_mask[kind] & (1u << op)))
            return 1;
    }
    return 0;
}

static void install_wrappers(sdk_table_t* t, void** wrappers, uint32_t n_wrappers)
{
    for (uint32_t i = 0; i < t->n_entries && i < n_wrappers; i++) {
        void* cur = t->fn_slots[i];
        if (cur == wrappers[i]) continue;
        t->originals[i] = cur;
        t->fn_slots[i]  = wrappers[i];
    }
}

static void uninstall_wrappers(sdk_table_t* t, void** wrappers, uint32_t n_wrappers)
{
    for (uint32_t i = 0; i < t->n_entries && i < n_wrappers; i++) {
        if (t->fn_slots[i] == wrappers[i] && t->originals[i])
            t->fn_slots[i] = t->originals[i];
    }
}

static void update_all_tables(void)
{
    for (int i = 0; i < MAX_TABLES; i++) {
        sdk_table_t* t = &g_tables[i];
        if (!t->in_use) continue;
        if (strcmp(t->name, "mylib") == 0 && g_mylib_idx == i) {
            for (uint32_t op = 0; op < t->n_entries && op < 2; op++) {
                if (should_wrap(SHIM_BUF_TRACING_HIP_RUNTIME_API, op))
                    install_wrappers(t, g_mylib_wrappers, 2);
            }
        } else if (strcmp(t->name, "libA_hsa") == 0 && g_liba_idx == i) {
            for (uint32_t op = 0; op < t->n_entries && op < 4; op++) {
                if (should_wrap(SHIM_BUF_TRACING_HSA_CORE_API, op))
                    install_wrappers(t, g_liba_wrappers, 4);
            }
        } else if (strcmp(t->name, "libB_hip") == 0 && g_libb_idx == i) {
            for (uint32_t op = 0; op < t->n_entries && op < 4; op++) {
                if (should_wrap(SHIM_BUF_TRACING_HIP_RUNTIME_API, op + 2))
                    install_wrappers(t, g_libb_wrappers, 4);
            }
        }
    }
}

static void remove_all_wrappers(void)
{
    for (int i = 0; i < MAX_TABLES; i++) {
        sdk_table_t* t = &g_tables[i];
        if (!t->in_use) continue;
        if (strcmp(t->name, "mylib") == 0)
            uninstall_wrappers(t, g_mylib_wrappers, 2);
        else if (strcmp(t->name, "libA_hsa") == 0)
            uninstall_wrappers(t, g_liba_wrappers, 4);
        else if (strcmp(t->name, "libB_hip") == 0)
            uninstall_wrappers(t, g_libb_wrappers, 4);
    }
}

/* ------------------------------------------------------------------ */
/* Public API implementation                                           */
/* ------------------------------------------------------------------ */

rocp_status_t rocp_force_configure(rocp_configure_func_t configure_func)
{
    if (!configure_func) return ROCP_STATUS_ERROR;

    rocp_tool_configure_result_t* result =
        configure_func(0, "mock-sdk-2", 128, NULL);

    int init_rc = 0;
    if (result && result->initialize)
        init_rc = result->initialize((void*)result->finalize, NULL);

    if (init_rc != 0) return ROCP_STATUS_ERROR;

    pthread_mutex_lock(&g_lock);
    g_n_clients++;
    pthread_mutex_unlock(&g_lock);

    mock_register_set_api_table_fn = &mock_sdk_set_api_table;
    mock_register_invoke_all_registrations();

    update_all_tables();

    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_create_context(rocp_context_id_t* ctx)
{
    if (!ctx) return ROCP_STATUS_ERROR;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (!g_contexts[i].in_use) {
            memset(&g_contexts[i], 0, sizeof(sdk_context_t));
            g_contexts[i].in_use = 1;
            g_contexts[i].handle = atomic_fetch_add(&g_next_handle, 1);
            ctx->handle = g_contexts[i].handle;
            pthread_mutex_unlock(&g_lock);
            return ROCP_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ROCP_STATUS_ERROR;
}

rocp_status_t rocp_create_buffer(rocp_context_id_t ctx,
                                 uint64_t size, uint64_t watermark,
                                 rocp_buffer_callback_t callback,
                                 void* user_data,
                                 rocp_buffer_id_t* buffer)
{
    (void)ctx;
    if (!buffer) return ROCP_STATUS_ERROR;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!g_buffers[i].in_use) {
            memset(&g_buffers[i], 0, sizeof(sdk_buffer_t));
            g_buffers[i].in_use        = 1;
            g_buffers[i].handle        = atomic_fetch_add(&g_next_handle, 1);
            g_buffers[i].size          = size;
            g_buffers[i].watermark     = watermark;
            g_buffers[i].user_callback = callback;
            g_buffers[i].user_data     = user_data;
            buffer->handle = g_buffers[i].handle;
            pthread_mutex_unlock(&g_lock);
            return ROCP_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ROCP_STATUS_ERROR;
}

rocp_status_t rocp_configure_buffer_tracing_service(
    rocp_context_id_t ctx, shim_buffer_tracing_kind_t kind,
    uint32_t* operations, size_t op_count, rocp_buffer_id_t buffer)
{
    if (kind >= SHIM_BUF_TRACING_NUM) return ROCP_STATUS_ERROR;
    pthread_mutex_lock(&g_lock);
    sdk_context_t* c = NULL;
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].in_use && g_contexts[i].handle == ctx.handle) {
            c = &g_contexts[i];
            break;
        }
    }
    if (!c) { pthread_mutex_unlock(&g_lock); return ROCP_STATUS_ERROR; }

    c->buffer_handle = buffer.handle;
    c->traced_kinds |= (1u << kind);
    if (!operations || op_count == 0) {
        c->all_ops[kind] = 1;
        c->op_mask[kind] = 0xFFFFFFFFu;
    } else {
        c->all_ops[kind] = 0;
        c->op_mask[kind] = 0;
        for (size_t i = 0; i < op_count && i < 32; i++)
            c->op_mask[kind] |= (1u << operations[i]);
    }
    pthread_mutex_unlock(&g_lock);
    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_start_context(rocp_context_id_t ctx)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].in_use && g_contexts[i].handle == ctx.handle) {
            atomic_store(&g_contexts[i].active, 1);
            pthread_mutex_unlock(&g_lock);
            update_all_tables();
            return ROCP_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ROCP_STATUS_ERROR;
}

rocp_status_t rocp_stop_context(rocp_context_id_t ctx)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].in_use && g_contexts[i].handle == ctx.handle) {
            atomic_store(&g_contexts[i].active, 0);
            pthread_mutex_unlock(&g_lock);
            remove_all_wrappers();
            return ROCP_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ROCP_STATUS_ERROR;
}

rocp_status_t rocp_flush_buffer(rocp_buffer_id_t buffer)
{
    (void)buffer;
    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_destroy_buffer(rocp_buffer_id_t buffer)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (g_buffers[i].in_use && g_buffers[i].handle == buffer.handle) {
            g_buffers[i].in_use = 0;
            pthread_mutex_unlock(&g_lock);
            return ROCP_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ROCP_STATUS_ERROR;
}

rocp_status_t rocp_destroy_context(rocp_context_id_t ctx)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (g_contexts[i].in_use && g_contexts[i].handle == ctx.handle) {
            g_contexts[i].in_use = 0;
            pthread_mutex_unlock(&g_lock);
            return ROCP_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ROCP_STATUS_ERROR;
}

/* ------------------------------------------------------------------ */
/* Internal API — shim provides the record sink                        */
/* ------------------------------------------------------------------ */

void mock_sdk_set_record_sink(uint64_t buffer_id,
                              mock_sdk_record_sink_fn sink,
                              mock_sdk_watermark_fn watermark_cb,
                              void* user_data)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (g_buffers[i].in_use && g_buffers[i].handle == buffer_id) {
            g_buffers[i].sink         = sink;
            g_buffers[i].watermark_cb = watermark_cb;
            g_buffers[i].sink_data    = user_data;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------------------------ */
/* set_api_table — called by mock_register for table walking           */
/* ------------------------------------------------------------------ */

int mock_sdk_set_api_table(const char* name, uint64_t lib_version,
                           uint64_t lib_instance, void** tables,
                           uint64_t num_tables)
{
    if (!name || !tables || num_tables == 0) return -1;

    void* table_struct = tables[0];
    if (!table_struct) return -1;

    size_t tbl_size = *(size_t*)table_struct;
    if (tbl_size <= sizeof(size_t)) return -1;

    void** fn_slots = (void**)((char*)table_struct + sizeof(size_t));
    uint64_t n_entries = (tbl_size - sizeof(size_t)) / sizeof(void*);

    pthread_mutex_lock(&g_lock);

    int slot = -1;
    for (int i = 0; i < MAX_TABLES; i++) {
        if (g_tables[i].in_use && strcmp(g_tables[i].name, name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_TABLES; i++) {
            if (!g_tables[i].in_use) { slot = i; break; }
        }
    }
    if (slot < 0) { pthread_mutex_unlock(&g_lock); return -1; }

    sdk_table_t* t = &g_tables[slot];
    t->in_use    = 1;
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->fn_slots  = fn_slots;
    t->n_entries = n_entries;

    for (uint64_t i = 0; i < n_entries && i < MAX_OPS; i++) {
        void* cur = fn_slots[i];
        int is_wrapper = 0;
        for (uint32_t w = 0; w < 4; w++) {
            if (cur == g_mylib_wrappers[w % 2] ||
                cur == g_liba_wrappers[w] ||
                cur == g_libb_wrappers[w])
                is_wrapper = 1;
        }
        if (!is_wrapper) t->originals[i] = cur;
    }

    if (strcmp(name, "mylib") == 0) {
        t->kind = SHIM_BUF_TRACING_HIP_RUNTIME_API;
        g_mylib_idx = slot;
    } else if (strcmp(name, "libA_hsa") == 0) {
        t->kind = SHIM_BUF_TRACING_HSA_CORE_API;
        g_liba_idx = slot;
    } else if (strcmp(name, "libB_hip") == 0) {
        t->kind = SHIM_BUF_TRACING_HIP_RUNTIME_API;
        g_libb_idx = slot;
    }

    pthread_mutex_unlock(&g_lock);
    (void)lib_version;
    (void)lib_instance;
    return 0;
}

__attribute__((constructor))
static void mock_rocp_sdk_ctor(void)
{
    mock_register_set_api_table_fn = &mock_sdk_set_api_table;
}
