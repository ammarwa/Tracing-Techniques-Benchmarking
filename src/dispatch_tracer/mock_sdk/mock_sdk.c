/*
 * mock_sdk.c - libmock_sdk.so
 *
 * Faithful mock of the subset of rocprofiler-sdk needed for the
 * dispatch-tracer benchmarks. Implements:
 *
 *  - The force_configure -> init_status lifecycle (one-shot gate)
 *  - Contexts, callback tracing service registration
 *  - start/stop_context (atomic activation flag)
 *  - The dispatch-table mechanism: mock_sdk_set_api_table() copies the
 *    original function pointers into a per-table slot and rewrites the
 *    runtime's table to point at wrapper functors. Each functor checks
 *    the registered contexts at call time (populate_contexts equivalent);
 *    if none are active for its (domain, op), it dispatches straight to
 *    the saved original (Level 2 noop).
 *
 * For this foundation we wire up the mylib_dispatch table which is
 * modeled as HIP_RUNTIME_API with two operations:
 *    op 0 : my_traced_function(int, uint64_t, double, void*) -> void
 *    op 1 : set_simulated_work_duration(unsigned int)         -> void
 *
 * Adding new runtimes is a matter of defining additional wrapper slots
 * with the corresponding signatures.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_register.h"
#include "mock_sdk.h"

/* ------------------------------------------------------------------ */
/* Context registry                                                    */
/* ------------------------------------------------------------------ */

#define MAX_CONTEXTS          16
#define MAX_TABLES            16
#define MAX_OPS_PER_DOMAIN    32

typedef struct {
    rocprofiler_callback_tracing_cb_t cb;
    void*                             user_data;
    int                               registered;   /* any op cared-about */
    /* Per-op bitset: 1 = this op is traced by this service.
     * We treat op_count == 0 / operations == NULL as "all ops". */
    uint32_t                          op_mask;      /* up to 32 ops */
    int                               all_ops;
} callback_service_t;

typedef struct {
    uint64_t            handle;
    int                 in_use;
    _Atomic int         active;  /* 0 = stopped, 1 = started */
    /* One service entry per domain. */
    callback_service_t  services[ROCPROFILER_CALLBACK_TRACING_NUM];
} context_t;

static context_t      g_contexts[MAX_CONTEXTS];
static pthread_mutex_t g_ctx_lock     = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_next_handle = 1;

/* ------------------------------------------------------------------ */
/* Dispatch table registry                                             */
/* ------------------------------------------------------------------ */

/* A single registered table. We keep a snapshot of originals and
 * a pointer back to the runtime's mutable table so we can rewrite
 * its slots with wrapper functors. */
typedef struct {
    int       in_use;
    char      name[64];
    uint32_t  version;
    void**    runtime_table;    /* points at runtime's api_table struct */
    uint64_t  num_entries;      /* number of void* slots */
    void*     originals[MAX_OPS_PER_DOMAIN];
    rocprofiler_callback_tracing_kind_t kind;
} dispatch_table_t;

static dispatch_table_t g_tables[MAX_TABLES];
static pthread_mutex_t  g_tables_lock = PTHREAD_MUTEX_INITIALIZER;

/* Resolve the (table, op) pair a wrapper belongs to. The wrappers for
 * mylib are slot 0 (HIP_RUNTIME_API). Additional tables would install
 * their own wrapper sets. */
static int find_table_by_name_locked(const char* name)
{
    for (int i = 0; i < MAX_TABLES; ++i) {
        if (g_tables[i].in_use && strcmp(g_tables[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Force-configure lifecycle                                           */
/* ------------------------------------------------------------------ */

/* 0 = not started, -1 = in progress, 1 = done */
static atomic_int                           g_init_status   = 0;
static rocprofiler_configure_func_t         g_forced_config = NULL;
static pthread_mutex_t                      g_init_lock     = PTHREAD_MUTEX_INITIALIZER;

static int get_init_status(void) { return atomic_load(&g_init_status); }

/* Forward: invoked from inside force_configure after tool init runs. */
static void invoke_register_propagation(void);

rocprofiler_status_t rocprofiler_force_configure(
    rocprofiler_configure_func_t configure_func)
{
    if (!configure_func) return ROCPROFILER_STATUS_ERROR;

    pthread_mutex_lock(&g_init_lock);
    if (get_init_status() != 0 || g_forced_config != NULL) {
        pthread_mutex_unlock(&g_init_lock);
        return ROCPROFILER_STATUS_ERROR_CONFIGURATION_LOCKED;
    }
    atomic_store(&g_init_status, -1);
    g_forced_config = configure_func;
    pthread_mutex_unlock(&g_init_lock);

    /* Ask the tool for its configure-result. */
    rocprofiler_tool_configure_result_t* result =
        configure_func(/*version*/0, /*runtime_version*/"mock-1",
                       /*priority*/128, /*client_id*/NULL);

    int init_rc = 0;
    if (result && result->initialize) {
        init_rc = result->initialize(/*fini*/(void*)result->finalize,
                                     /*tool_data*/NULL);
    }

    /* Re-replay registered runtime tables through the dispatch mechanism. */
    invoke_register_propagation();

    atomic_store(&g_init_status, 1);

    if (init_rc != 0) return ROCPROFILER_STATUS_ERROR;
    return ROCPROFILER_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Context API                                                         */
/* ------------------------------------------------------------------ */

rocprofiler_status_t rocprofiler_create_context(rocprofiler_context_id_t* ctx)
{
    if (!ctx) return ROCPROFILER_STATUS_ERROR;

    pthread_mutex_lock(&g_ctx_lock);
    int slot = -1;
    for (int i = 0; i < MAX_CONTEXTS; ++i) {
        if (!g_contexts[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_ctx_lock);
        return ROCPROFILER_STATUS_ERROR;
    }
    memset(&g_contexts[slot], 0, sizeof(g_contexts[slot]));
    g_contexts[slot].in_use = 1;
    g_contexts[slot].handle = atomic_fetch_add(&g_next_handle, 1);
    atomic_store(&g_contexts[slot].active, 0);

    ctx->handle = g_contexts[slot].handle;
    pthread_mutex_unlock(&g_ctx_lock);
    return ROCPROFILER_STATUS_SUCCESS;
}

static context_t* find_ctx_locked(uint64_t handle)
{
    for (int i = 0; i < MAX_CONTEXTS; ++i) {
        if (g_contexts[i].in_use && g_contexts[i].handle == handle)
            return &g_contexts[i];
    }
    return NULL;
}

rocprofiler_status_t rocprofiler_configure_callback_tracing_service(
    rocprofiler_context_id_t            ctx,
    rocprofiler_callback_tracing_kind_t kind,
    void*                               operations,
    size_t                              op_count,
    rocprofiler_callback_tracing_cb_t   cb,
    void*                               user_data)
{
    if (kind < 0 || kind >= ROCPROFILER_CALLBACK_TRACING_NUM)
        return ROCPROFILER_STATUS_ERROR;

    pthread_mutex_lock(&g_ctx_lock);
    context_t* c = find_ctx_locked(ctx.handle);
    if (!c) { pthread_mutex_unlock(&g_ctx_lock); return ROCPROFILER_STATUS_ERROR; }

    callback_service_t* s = &c->services[kind];
    s->cb         = cb;
    s->user_data  = user_data;
    s->registered = 1;
    if (operations == NULL || op_count == 0) {
        s->all_ops = 1;
        s->op_mask = 0xFFFFFFFFu;
    } else {
        s->all_ops = 0;
        s->op_mask = 0;
        uint32_t* ops = (uint32_t*)operations;
        for (size_t i = 0; i < op_count && i < 32; ++i) {
            if (ops[i] < 32) s->op_mask |= (1u << ops[i]);
        }
    }
    pthread_mutex_unlock(&g_ctx_lock);
    return ROCPROFILER_STATUS_SUCCESS;
}

rocprofiler_status_t rocprofiler_start_context(rocprofiler_context_id_t ctx)
{
    pthread_mutex_lock(&g_ctx_lock);
    context_t* c = find_ctx_locked(ctx.handle);
    if (!c) { pthread_mutex_unlock(&g_ctx_lock); return ROCPROFILER_STATUS_ERROR; }
    atomic_store(&c->active, 1);
    pthread_mutex_unlock(&g_ctx_lock);
    return ROCPROFILER_STATUS_SUCCESS;
}

rocprofiler_status_t rocprofiler_stop_context(rocprofiler_context_id_t ctx)
{
    pthread_mutex_lock(&g_ctx_lock);
    context_t* c = find_ctx_locked(ctx.handle);
    if (!c) { pthread_mutex_unlock(&g_ctx_lock); return ROCPROFILER_STATUS_ERROR; }
    atomic_store(&c->active, 0);
    pthread_mutex_unlock(&g_ctx_lock);
    return ROCPROFILER_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* populate_contexts — the Level 2 check.                              */
/* Returns the first active context whose service for (kind, op)       */
/* is registered, or NULL if none.                                     */
/* ------------------------------------------------------------------ */

static context_t* populate_contexts(rocprofiler_callback_tracing_kind_t kind,
                                    uint32_t op)
{
    if (kind < 0 || kind >= ROCPROFILER_CALLBACK_TRACING_NUM) return NULL;
    /* No lock on the hot path — contexts are append-only during attach
     * and atomic flags handle activation. The worst case for a racy
     * read is a missed/stale event at attach boundaries which is
     * acceptable for a benchmark mock. */
    for (int i = 0; i < MAX_CONTEXTS; ++i) {
        context_t* c = &g_contexts[i];
        if (!c->in_use) continue;
        if (atomic_load(&c->active) == 0) continue;
        callback_service_t* s = &c->services[kind];
        if (!s->registered) continue;
        if (!s->all_ops && (op >= 32 || !(s->op_mask & (1u << op)))) continue;
        return c;
    }
    return NULL;
}

/* Should an op slot get a wrapper installed? (update_table / should_wrap) */
static int should_wrap_functor(rocprofiler_callback_tracing_kind_t kind,
                               uint32_t op)
{
    pthread_mutex_lock(&g_ctx_lock);
    for (int i = 0; i < MAX_CONTEXTS; ++i) {
        context_t* c = &g_contexts[i];
        if (!c->in_use) continue;
        callback_service_t* s = &c->services[kind];
        if (!s->registered) continue;
        if (!s->all_ops && (op >= 32 || !(s->op_mask & (1u << op)))) continue;
        pthread_mutex_unlock(&g_ctx_lock);
        return 1;
    }
    pthread_mutex_unlock(&g_ctx_lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wrapper functors for the mylib_dispatch table                       */
/*                                                                     */
/* We bind each wrapper to a fixed (table_slot, op_index) by having    */
/* the wrapper look up the table entry through a per-table anchor.    */
/* The set_api_table step stores the table index into g_mylib_idx so   */
/* the wrappers can find their originals.                              */
/* ------------------------------------------------------------------ */

static int g_mylib_idx = -1;  /* index into g_tables for mylib */

static void fire_callbacks(rocprofiler_callback_tracing_kind_t kind,
                           uint32_t op,
                           const char* fname,
                           void* args,
                           rocprofiler_callback_phase_t phase)
{
    for (int i = 0; i < MAX_CONTEXTS; ++i) {
        context_t* c = &g_contexts[i];
        if (!c->in_use) continue;
        if (atomic_load(&c->active) == 0) continue;
        callback_service_t* s = &c->services[kind];
        if (!s->registered || !s->cb) continue;
        if (!s->all_ops && (op >= 32 || !(s->op_mask & (1u << op)))) continue;
        rocprofiler_callback_tracing_record_t rec = {
            .kind          = kind,
            .operation     = op,
            .phase         = phase,
            .function_name = fname,
            .args          = args,
        };
        s->cb(rec, s->user_data);
    }
}

/* Wrapper for mylib op 0: my_traced_function */
typedef void (*mylib_op0_fn_t)(int, uint64_t, double, void*);
typedef void (*mylib_op1_fn_t)(unsigned int);

struct mylib_op0_args { int a1; uint64_t a2; double a3; void* a4; };
struct mylib_op1_args { unsigned int us; };

static void mylib_wrap_op0(int a1, uint64_t a2, double a3, void* a4)
{
    mylib_op0_fn_t orig = NULL;
    if (g_mylib_idx >= 0) orig = (mylib_op0_fn_t)g_tables[g_mylib_idx].originals[0];

    context_t* active = populate_contexts(
        ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API, 0);
    if (!active) {
        /* Level 2 noop — call original directly. */
        if (orig) orig(a1, a2, a3, a4);
        return;
    }
    struct mylib_op0_args args = { a1, a2, a3, a4 };
    fire_callbacks(ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
                   0, "my_traced_function", &args,
                   ROCPROFILER_CALLBACK_PHASE_ENTER);
    if (orig) orig(a1, a2, a3, a4);
    fire_callbacks(ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
                   0, "my_traced_function", &args,
                   ROCPROFILER_CALLBACK_PHASE_EXIT);
}

static void mylib_wrap_op1(unsigned int us)
{
    mylib_op1_fn_t orig = NULL;
    if (g_mylib_idx >= 0) orig = (mylib_op1_fn_t)g_tables[g_mylib_idx].originals[1];

    context_t* active = populate_contexts(
        ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API, 1);
    if (!active) {
        if (orig) orig(us);
        return;
    }
    struct mylib_op1_args args = { us };
    fire_callbacks(ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
                   1, "set_simulated_work_duration", &args,
                   ROCPROFILER_CALLBACK_PHASE_ENTER);
    if (orig) orig(us);
    fire_callbacks(ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API,
                   1, "set_simulated_work_duration", &args,
                   ROCPROFILER_CALLBACK_PHASE_EXIT);
}

/* ------------------------------------------------------------------ */
/* copy_table / update_table                                           */
/* ------------------------------------------------------------------ */

static void copy_table(dispatch_table_t* t)
{
    /* Copy original pointers out of the runtime-visible table. */
    void** slots = t->runtime_table;
    for (uint64_t i = 0; i < t->num_entries && i < MAX_OPS_PER_DOMAIN; ++i) {
        t->originals[i] = slots[i];
    }
}

static void update_table_mylib(dispatch_table_t* t)
{
    void** slots = t->runtime_table;
    /* Op 0: my_traced_function */
    if (t->num_entries > 0 &&
        should_wrap_functor(ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API, 0)) {
        slots[0] = (void*)&mylib_wrap_op0;
    } else if (t->num_entries > 0) {
        slots[0] = t->originals[0];
    }
    /* Op 1: set_simulated_work_duration */
    if (t->num_entries > 1 &&
        should_wrap_functor(ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API, 1)) {
        slots[1] = (void*)&mylib_wrap_op1;
    } else if (t->num_entries > 1) {
        slots[1] = t->originals[1];
    }
}

/* ------------------------------------------------------------------ */
/* mock_sdk_set_api_table — the entry point register-lib calls.        */
/* ------------------------------------------------------------------ */

int mock_sdk_set_api_table(const char* name,
                           uint32_t version,
                           void** api_tables,
                           uint64_t num_tables)
{
    if (!name || !api_tables || num_tables == 0) return -1;
    /* For our model each registration provides one table-struct whose
     * address is at api_tables[0..num_tables-1] — we take index 0 for
     * mylib (matching mylib_dispatch's registration). */
    pthread_mutex_lock(&g_tables_lock);

    int slot = find_table_by_name_locked(name);
    if (slot < 0) {
        for (int i = 0; i < MAX_TABLES; ++i) {
            if (!g_tables[i].in_use) { slot = i; break; }
        }
        if (slot < 0) { pthread_mutex_unlock(&g_tables_lock); return -1; }
        memset(&g_tables[slot], 0, sizeof(g_tables[slot]));
        g_tables[slot].in_use = 1;
        snprintf(g_tables[slot].name, sizeof(g_tables[slot].name), "%s", name);
    }

    dispatch_table_t* t = &g_tables[slot];
    t->version       = version;
    /* api_tables is the address of the caller's "void** slot array"
     * (i.e. the runtime's api_table cast to void**). */
    t->runtime_table = api_tables;
    t->num_entries   = num_tables;

    if (strcmp(name, "mylib") == 0) {
        t->kind      = ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API;
        g_mylib_idx  = slot;
        copy_table(t);
        update_table_mylib(t);
    } else {
        /* Unknown runtime: save originals but install no wrappers. */
        t->kind = ROCPROFILER_CALLBACK_TRACING_NUM;  /* unknown */
        copy_table(t);
    }

    pthread_mutex_unlock(&g_tables_lock);
    return 0;
}

/* Called inside force_configure after tool_initialize returns.
 * Asks register-lib to replay everything through our set_api_table,
 * which will cause update_table to install wrappers for the ops the
 * tool's freshly-registered services care about. */
static void invoke_register_propagation(void)
{
    /* Publish our set_api_table into register-lib's mutable function
     * pointer. Prefer a direct assignment (same-process symbol), fall
     * back to dlsym lookup if the register library resolved its copy
     * independently. */
    mock_register_set_api_table_fn = &mock_sdk_set_api_table;
    (void)mock_register_invoke_all_registrations();
}

/* Publish the function pointer as early as possible. A constructor is
 * enough because libmock_sdk.so is dlopen'd on demand. */
__attribute__((constructor))
static void mock_sdk_ctor(void)
{
    mock_register_set_api_table_fn = &mock_sdk_set_api_table;
}
