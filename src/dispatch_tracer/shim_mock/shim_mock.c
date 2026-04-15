/*
 * shim_mock.c — libshim_mock.so
 *
 * Mock of the proposed rocprofiler-sdk-shim design (post-P0 revision):
 * rocprofiler-register unconditionally dlopens this library when a
 * runtime registers a dispatch table. The shim wraps every table entry
 * with a thin wrapper of the shape:
 *
 *     functor(args...) {
 *         uint32_t mode = atomic_load(&op_mode[Op]);
 *         if (mode == MODE_OFF)  tail-call orig(args);
 *         else                   shim_handle_event(Op, mode, orig, args);
 *     }
 *
 * op_mode values are INTEGER MODE SELECTORS, not function pointers.
 * All code the target executes is part of this library, already in the
 * target's address space. No consumer code ever runs in the target.
 * (See SHIM_MEMFD_SOCK_DESIGN §5.1 P0 erratum.)
 *
 * Mode values:
 *   MODE_OFF          = 0  — fast path, tail-call original
 *   MODE_RECORD       = 1  — emit tsc + kind + op + phase to ring
 *   MODE_RECORD_ARGS  = 2  — above + inline typed arg payload
 *   MODE_RECORD_FULL  = 3  — above + variable-ring strings
 *
 * The consumer (a separate process) writes mode values into shared
 * memory via the memfd+sock channel. It reads records from the ring.
 * It never injects code into the target.
 *
 * Switching the shim in requires only:
 *   MOCK_REGISTER_LIB=$BUILD_DIR/lib/libshim_mock.so
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "mock_register.h"
#include "mylib_dispatch.h"
#include "shim_protocol.h"
#include "shim_ipc.h"

/* ------------------------------------------------------------------ */
/* Per-op state                                                        */
/* ------------------------------------------------------------------ */

#define SHIM_NUM_OPS 2   /* op0 = my_traced_function, op1 = set_simulated_work_duration */

/* Saved original function pointers (const after install). */
static _Atomic(void*) g_runtime_original[SHIM_NUM_OPS] = { NULL, NULL };

/* Mutable "next in chain" for SDK layering (§8). */
static _Atomic(void*) g_next_in_chain[SHIM_NUM_OPS]    = { NULL, NULL };

/* IPC layer — memfd + socket + ring buffer + eventfd.
 * Initialized in shim_register_ctor. When IPC is active, op_mode lives
 * in the mmap'd ctrl region; when IPC init fails (e.g. memfd not
 * supported), we fall back to local atomics for benchmark-only mode. */
static shim_ipc_target_t g_ipc;
static int               g_ipc_ok = 0;

/* Fallback local mode (used when IPC not available, e.g. benchmark). */
static _Atomic uint32_t g_local_op_mode[SHIM_NUM_OPS] = { ROCP_SHIM_MODE_OFF,
                                                            ROCP_SHIM_MODE_OFF };

/* Returns the op_mode slot — from shared memory if IPC is up, else local. */
static inline _Atomic uint32_t* shim_op_mode_ptr(int op)
{
    if (g_ipc_ok && g_ipc.ctrl)
        return &g_ipc.ctrl->op_mode[op];
    return &g_local_op_mode[op];
}

/* Book-keeping. Not touched on the hot path. */
static pthread_mutex_t g_shim_install_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_shim_installed[SHIM_NUM_OPS] = { 0, 0 };

/* Signatures of the original library entry points. */
typedef void (*orig_op0_t)(int, uint64_t, double, void*);
typedef void (*orig_op1_t)(unsigned int);

/* Packed arg structs for the two mock ops — matches §7B.1.
 * In the real shim these are generated from the SDK's arg metadata. */
typedef struct {
    int      a1;
    uint64_t a2;
    double   a3;
    void*    a4;   /* pointer value only — cannot be dereferenced OOP */
} packed_op0_args_t;

typedef struct {
    unsigned int us;
} packed_op1_args_t;

/* ------------------------------------------------------------------ */
/* Forward declarations for correlation helpers (defined below).       */
/* ------------------------------------------------------------------ */

static inline shim_correlation_id_t shim_push_correlation(void);
static inline void                  shim_pop_correlation (void);

/* ------------------------------------------------------------------ */
/* Mock "record write" — in the real shim this writes to a ring buffer */
/* in the shared memfd. For the mock we just bump a counter so the     */
/* benchmark can verify records were emitted.                          */
/* ------------------------------------------------------------------ */

static _Atomic uint64_t g_records_emitted = 0;

static void shim_emit_record(uint32_t slot_idx, uint32_t phase,
                             const shim_correlation_id_t* corr,
                             const void* packed_args, uint32_t arg_bytes)
{
    if (!g_ipc_ok) {
        atomic_fetch_add_explicit(&g_records_emitted, 1, memory_order_relaxed);
        return;
    }
    shim_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.tsc            = shim_rdtsc();
    rec.kind           = 0; /* mock: single domain */
    rec.op             = slot_idx;
    rec.phase          = phase;
    rec.thread_id      = (uint64_t)gettid();
    rec.correlation_id = *corr;
    rec.slot_idx       = slot_idx;
    if (packed_args && arg_bytes > 0) {
        uint32_t copy = arg_bytes < SHIM_RECORD_ARG_BYTES
                      ? arg_bytes : SHIM_RECORD_ARG_BYTES;
        memcpy(rec.args, packed_args, copy);
        rec.arg_bytes = copy;
    }
    shim_ring_write(&g_ipc, &rec);
}

static int shim_check_value_filter(uint32_t slot_idx, void* packed_args)
{
    if (!g_ipc_ok || !g_ipc.ctrl) return 1; /* pass */
    uint32_t count = g_ipc.ctrl->value_filter_count[slot_idx];
    if (count == 0) return 1;
    /* Evaluate declarative rules (§13.5 Phase 3) */
    for (uint32_t i = 0; i < count && i < SHIM_MAX_VALUE_RULES_PER_OP; i++) {
        const shim_value_rule_t* r = &g_ipc.ctrl->value_rules[slot_idx][i];
        /* For the mock we only support arg_index 0 (first scalar arg).
         * The real shim's code generator would know the struct layout. */
        uint64_t val = 0;
        if (packed_args && r->arg_index == 0)
            val = *(uint64_t*)packed_args; /* type-punned for mock */
        int pass = 0;
        switch (r->comparison) {
        case SHIM_CMP_EQ:      pass = (val == r->operand); break;
        case SHIM_CMP_NEQ:     pass = (val != r->operand); break;
        case SHIM_CMP_GT:      pass = (val >  r->operand); break;
        case SHIM_CMP_LT:      pass = (val <  r->operand); break;
        case SHIM_CMP_BITMASK: pass = ((val & r->operand) != 0); break;
        default: pass = 1; break;
        }
        if (!pass) return 0; /* AND semantics: any rule fails → reject */
    }
    return 1;
}

static void shim_handle_event(uint32_t op, uint32_t mode,
                              void* orig_fn,
                              const void* packed_args, uint32_t arg_bytes)
{
    (void)mode;
    /* Phase 1: name filter bitmap (§13.5) — racy but benign */
    if (g_ipc_ok && g_ipc.ctrl && !shim_filter_test(g_ipc.ctrl->name_filter, op))
        goto call_original;

    /* Phase 3: value filter (§13.5) */
    if (!shim_check_value_filter(op, (void*)packed_args))
        goto call_original;

    {
        shim_correlation_id_t corr = shim_push_correlation();

        /* ENTER record — includes typed arg payload */
        shim_emit_record(op, SHIM_PHASE_ENTER, &corr, packed_args, arg_bytes);

        /* Call through the chain */
        void* next = atomic_load_explicit(&g_next_in_chain[op],
                                          memory_order_acquire);
        if (!next) next = orig_fn;

        if (op == 0) {
            const packed_op0_args_t* a = (const packed_op0_args_t*)packed_args;
            ((orig_op0_t)next)(a->a1, a->a2, a->a3, a->a4);
        } else if (op == 1) {
            const packed_op1_args_t* a = (const packed_op1_args_t*)packed_args;
            ((orig_op1_t)next)(a->us);
        }

        /* EXIT record */
        shim_emit_record(op, SHIM_PHASE_EXIT, &corr, packed_args, arg_bytes);

        shim_pop_correlation();
    }
    return;

call_original:
    if (op == 0) {
        const packed_op0_args_t* a = (const packed_op0_args_t*)packed_args;
        ((orig_op0_t)orig_fn)(a->a1, a->a2, a->a3, a->a4);
    } else if (op == 1) {
        const packed_op1_args_t* a = (const packed_op1_args_t*)packed_args;
        ((orig_op1_t)orig_fn)(a->us);
    }
}

/* ------------------------------------------------------------------ */
/* The shim wrappers — hot path.                                       */
/*                                                                     */
/* Fast path: one atomic load of op_mode, one branch, one atomic load  */
/* of g_runtime_original, one tail-call. ~0.8 ns on EPYC 9354.        */
/*                                                                     */
/* Slow path (mode != OFF): shim_handle_event runs shim-local code     */
/* that writes records to the ring. No consumer code runs in target.   */
/* ------------------------------------------------------------------ */

static void shim_wrap_op0(int a1, uint64_t a2, double a3, void* a4)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(0), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[0],
                                          memory_order_acquire);
        ((orig_op0_t)orig)(a1, a2, a3, a4);
        return;
    }
    void* orig = atomic_load_explicit(&g_runtime_original[0],
                                      memory_order_acquire);
    packed_op0_args_t args = { a1, a2, a3, a4 };
    shim_handle_event(0, mode, orig, &args, sizeof(args));
}

static void shim_wrap_op1(unsigned int us)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(1), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[1],
                                          memory_order_acquire);
        ((orig_op1_t)orig)(us);
        return;
    }
    void* orig = atomic_load_explicit(&g_runtime_original[1],
                                      memory_order_acquire);
    packed_op1_args_t args = { us };
    shim_handle_event(1, mode, orig, &args, sizeof(args));
}

/* ------------------------------------------------------------------ */
/* Installation: rewrite the runtime's api_table to point at our       */
/* wrappers and save the originals.                                    */
/* ------------------------------------------------------------------ */

static void shim_install_mylib_table(void** slots, uint64_t num_entries)
{
    pthread_mutex_lock(&g_shim_install_lock);

    if (num_entries > 0 && !g_shim_installed[0]) {
        void* cur = slots[0];
        if (cur != (void*)&shim_wrap_op0) {
            atomic_store_explicit(&g_runtime_original[0], cur,
                                  memory_order_release);
            atomic_store_explicit(&g_next_in_chain[0], cur,
                                  memory_order_release);
            atomic_store_explicit((_Atomic(void*)*)&slots[0],
                                  (void*)&shim_wrap_op0,
                                  memory_order_release);
        }
        g_shim_installed[0] = 1;
    }

    if (num_entries > 1 && !g_shim_installed[1]) {
        void* cur = slots[1];
        if (cur != (void*)&shim_wrap_op1) {
            atomic_store_explicit(&g_runtime_original[1], cur,
                                  memory_order_release);
            atomic_store_explicit(&g_next_in_chain[1], cur,
                                  memory_order_release);
            atomic_store_explicit((_Atomic(void*)*)&slots[1],
                                  (void*)&shim_wrap_op1,
                                  memory_order_release);
        }
        g_shim_installed[1] = 1;
    }

    pthread_mutex_unlock(&g_shim_install_lock);
}

/* ------------------------------------------------------------------ */
/* Entry point register-lib calls.                                     */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int mock_sdk_set_api_table(const char* name,
                           uint32_t    version,
                           void**      api_tables,
                           uint64_t    num_tables)
{
    (void)version;
    if (!name || !api_tables || num_tables == 0) return -1;
    if (strcmp(name, "mylib") == 0) {
        shim_install_mylib_table(api_tables, num_tables);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* rocprofiler_configure — exported for register-lib's symbol scan.    */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int rocprofiler_configure(uint32_t version, const char* runtime_version,
                          uint32_t priority, void* client_id)
{
    (void)version; (void)runtime_version; (void)priority; (void)client_id;
    return 0;
}

/* ------------------------------------------------------------------ */
/* OOP-side control — mode selectors, NOT function pointers.           */
/*                                                                     */
/* In the real shim these live in the mmap'd memfd and are written by  */
/* the consumer process. For the mock we expose them as exported       */
/* functions so the benchmark harness and tests can toggle modes from  */
/* within the same process.                                            */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int shim_set_op_mode(int op, uint32_t mode)
{
    if (op < 0 || op >= SHIM_NUM_OPS) return -1;
    if (mode > ROCP_SHIM_MODE_RECORD_FULL) return -1;
    atomic_store_explicit(shim_op_mode_ptr(op), mode, memory_order_release);
    return 0;
}

__attribute__((visibility("default")))
uint32_t shim_get_op_mode(int op)
{
    if (op < 0 || op >= SHIM_NUM_OPS) return ROCP_SHIM_MODE_OFF;
    return atomic_load_explicit(shim_op_mode_ptr(op), memory_order_acquire);
}

/* Returns the runtime-original function pointer (const after install).
 * SDK's update_table calls this to get the raw runtime function, NOT
 * the shim wrapper. See SHIM_MEMFD_SOCK_DESIGN §8. */
__attribute__((visibility("default")))
void* shim_get_runtime_original(int op)
{
    if (op < 0 || op >= SHIM_NUM_OPS) return NULL;
    return atomic_load_explicit(&g_runtime_original[op], memory_order_acquire);
}

/* Update the "next in chain" pointer — called by the SDK when it
 * installs its own wrapper on top of the shim's. */
__attribute__((visibility("default")))
int shim_set_next_in_chain(int op, void* sdk_wrapper)
{
    if (op < 0 || op >= SHIM_NUM_OPS) return -1;
    atomic_store_explicit(&g_next_in_chain[op], sdk_wrapper,
                          memory_order_release);
    return 0;
}

__attribute__((visibility("default")))
uint64_t shim_get_records_emitted(void)
{
    return atomic_load_explicit(&g_records_emitted, memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* Correlation IDs — mirrors rocprofiler-sdk's semantics.              */
/* See SHIM_MEMFD_SOCK_DESIGN §7A.                                    */
/* ------------------------------------------------------------------ */

#define SHIM_CORR_STACK_MAX 32

typedef struct { uint64_t internal; uint64_t external; } shim_corr_frame_t;

static _Atomic uint64_t                g_next_internal     = 0;
static _Thread_local int               tls_corr_depth      = 0;
static _Thread_local int               tls_corr_dropped    = 0;
static _Thread_local shim_corr_frame_t tls_corr_stack[SHIM_CORR_STACK_MAX];

static _Thread_local int      tls_ext_depth = 0;
static _Thread_local uint64_t tls_ext_stack[SHIM_CORR_STACK_MAX];

static inline uint64_t shim_current_external(void)
{
    return tls_ext_depth ? tls_ext_stack[tls_ext_depth - 1] : 0;
}

static inline shim_correlation_id_t shim_push_correlation(void)
{
    uint64_t id  = atomic_fetch_add_explicit(&g_next_internal, 1,
                                             memory_order_relaxed) + 1;
    uint64_t par = tls_corr_depth
                       ? tls_corr_stack[tls_corr_depth - 1].internal : 0;
    uint64_t ext = shim_current_external();

    if (tls_corr_depth < SHIM_CORR_STACK_MAX) {
        tls_corr_stack[tls_corr_depth].internal = id;
        tls_corr_stack[tls_corr_depth].external = ext;
        tls_corr_depth++;
    } else {
        tls_corr_dropped++;
    }
    return (shim_correlation_id_t){ id, ext, par };
}

static inline void shim_pop_correlation(void)
{
    if (tls_corr_dropped > 0) { tls_corr_dropped--; return; }
    if (tls_corr_depth > 0)    tls_corr_depth--;
}

/* External correlation push/pop — mirrors SDK's API. */
__attribute__((visibility("default")))
int rocprofiler_shim_push_external_correlation_id(uint64_t id)
{
    if (tls_ext_depth < SHIM_CORR_STACK_MAX) {
        tls_ext_stack[tls_ext_depth++] = id;
        return 0;
    }
    return -1;
}

__attribute__((visibility("default")))
int rocprofiler_shim_pop_external_correlation_id(uint64_t* out)
{
    if (tls_ext_depth == 0) { if (out) *out = 0; return -1; }
    tls_ext_depth--;
    if (out) *out = tls_ext_stack[tls_ext_depth];
    return 0;
}

__attribute__((visibility("default")))
uint64_t shim_current_internal_id(void)
{
    return tls_corr_depth ? tls_corr_stack[tls_corr_depth - 1].internal : 0;
}

/* ------------------------------------------------------------------ */
/* mock_register wiring.                                               */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void shim_register_ctor(void)
{
    /* Initialize IPC (memfd + socket + ring + eventfd).
     * If this fails (e.g. old kernel without memfd_create), the shim
     * falls back to local-mode with g_local_op_mode — still works for
     * the benchmark's fast-path measurement, just no IPC. */
    if (shim_ipc_init(&g_ipc) == 0) {
        g_ipc_ok = 1;
        /* Register mylib as a table in the memfd header (§13.6). */
        if (g_ipc.ctrl->n_registrations < SHIM_MAX_REGISTRATIONS) {
            uint32_t idx = g_ipc.ctrl->n_registrations;
            shim_table_registration_t* reg = &g_ipc.ctrl->registrations[idx];
            snprintf(reg->name, SHIM_TABLE_NAME_MAX, "mylib");
            reg->lib_instance  = 0;
            reg->major_version = 1;
            reg->minor_version = 0;
            reg->slot_base     = 0;
            reg->n_ops         = SHIM_NUM_OPS;
            g_ipc.ctrl->n_registrations = idx + 1;
            g_ipc.ctrl->total_ops       = SHIM_NUM_OPS;
            /* Register op metadata — names + arg sizes */
            snprintf(g_ipc.ctrl->op_info[0].name, SHIM_OP_NAME_MAX,
                     "my_traced_function");
            g_ipc.ctrl->op_info[0].n_args = 4;
            g_ipc.ctrl->op_info[0].arg_total_bytes = sizeof(packed_op0_args_t);

            snprintf(g_ipc.ctrl->op_info[1].name, SHIM_OP_NAME_MAX,
                     "set_simulated_work_duration");
            g_ipc.ctrl->op_info[1].n_args = 1;
            g_ipc.ctrl->op_info[1].arg_total_bytes = sizeof(packed_op1_args_t);

            /* Enable all ops in the name-filter bitmap by default */
            for (uint32_t i = 0; i < SHIM_NUM_OPS; i++)
                shim_filter_set(g_ipc.ctrl->name_filter, i);
        }
    }

    mock_register_set_api_table_fn = &mock_sdk_set_api_table;
}
