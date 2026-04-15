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
#include "mock_libA.h"
#include "mock_libB.h"
#include "shim_protocol.h"
#include "shim_ipc.h"
#include "shim_arg_info.h"

/* Arg descriptors defined in shim_arg_descriptors.c */
extern const shim_op_arg_descriptor_t g_mylib_arg_descs[];
extern const shim_op_arg_descriptor_t g_liba_arg_descs[];
extern const shim_op_arg_descriptor_t g_libb_arg_descs[];

/* Forward declaration — get_arg_desc is defined after g_ipc. */
static const shim_op_arg_descriptor_t* get_arg_desc(uint32_t slot_idx);

/* ------------------------------------------------------------------ */
/* Per-op state                                                        */
/* ------------------------------------------------------------------ */

/* Total ops across all registered tables:
 *   mylib:    op 0 = my_traced_function, op 1 = set_simulated_work_duration
 *   libA_hsa: op 2 = queue_create, op 3 = memory_allocate,
 *             op 4 = kernel_dispatch, op 5 = signal_wait
 *   libB_hip: op 6 = launch_kernel, op 7 = memcpy_async,
 *             op 8 = stream_synchronize, op 9 = get_device_properties
 */
#define SHIM_NUM_OPS 10

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

/* Runtime-assigned slot bases — set dynamically when mock_sdk_set_api_table
 * is called for each library. Wrappers read these to compute their global
 * slot index (slot_idx = base + local_op_index). */
static uint32_t g_mylib_base = 0;
static uint32_t g_liba_base  = 0;
static uint32_t g_libb_base  = 0;

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

        /* ENTER record — no args yet (output params not filled). */
        shim_emit_record(op, SHIM_PHASE_ENTER, &corr, NULL, 0);

        /* Call the original — output args are filled by this call. */
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

        /* EXIT record — carries the RAW packed-args struct (compact binary).
         * String conversion happens consumer-side via iterate_args(),
         * matching rocprofiler-sdk's pattern: the record is compact, the
         * tool decodes on demand using the shared .def descriptors. */
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
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(g_mylib_base + 0), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[g_mylib_base + 0],
                                          memory_order_acquire);
        ((orig_op0_t)orig)(a1, a2, a3, a4);
        return;
    }
    void* orig = atomic_load_explicit(&g_runtime_original[g_mylib_base + 0],
                                      memory_order_acquire);
    packed_op0_args_t args = { a1, a2, a3, a4 };
    shim_handle_event(g_mylib_base + 0, mode, orig, &args, sizeof(args));
}

static void shim_wrap_op1(unsigned int us)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(g_mylib_base + 1), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[g_mylib_base + 1],
                                          memory_order_acquire);
        ((orig_op1_t)orig)(us);
        return;
    }
    void* orig = atomic_load_explicit(&g_runtime_original[g_mylib_base + 1],
                                      memory_order_acquire);
    packed_op1_args_t args = { us };
    shim_handle_event(g_mylib_base + 1, mode, orig, &args, sizeof(args));
}

/* ================================================================== */
/* libA_hsa wrappers (ops 2-5) — typed per-op, matching mock_libA.h    */
/* ================================================================== */

typedef liba_status_t (*liba_queue_create_fn)(liba_agent_t, uint32_t, liba_queue_t*);
typedef liba_status_t (*liba_memory_allocate_fn)(liba_memory_region_t, uint64_t, void**);
typedef liba_status_t (*liba_kernel_dispatch_fn)(liba_queue_t, const liba_dispatch_packet_t*, liba_signal_t);
typedef liba_status_t (*liba_signal_wait_fn)(liba_signal_t, uint64_t);

typedef struct { liba_agent_t agent; uint32_t size; liba_queue_t* out; } packed_liba_op0_t;
typedef struct { liba_memory_region_t region; uint64_t size; void** out_ptr; } packed_liba_op1_t;
typedef struct { liba_queue_t queue; const liba_dispatch_packet_t* pkt; liba_signal_t completion; } packed_liba_op2_t;
typedef struct { liba_signal_t signal; uint64_t timeout_ns; } packed_liba_op3_t;

#define SHIM_LIBA_BASE g_liba_base

static liba_status_t shim_wrap_liba_queue_create(liba_agent_t agent, uint32_t size, liba_queue_t* out)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBA_BASE + 0), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 0], memory_order_acquire);
        return ((liba_queue_create_fn)orig)(agent, size, out);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 0], memory_order_acquire);
    packed_liba_op0_t args = { agent, size, out };
    shim_handle_event(SHIM_LIBA_BASE + 0, mode, orig, &args, sizeof(args));
    return LIBA_STATUS_SUCCESS;
}

static liba_status_t shim_wrap_liba_memory_allocate(liba_memory_region_t region, uint64_t size, void** out_ptr)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBA_BASE + 1), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 1], memory_order_acquire);
        return ((liba_memory_allocate_fn)orig)(region, size, out_ptr);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 1], memory_order_acquire);
    packed_liba_op1_t args = { region, size, out_ptr };
    shim_handle_event(SHIM_LIBA_BASE + 1, mode, orig, &args, sizeof(args));
    return LIBA_STATUS_SUCCESS;
}

static liba_status_t shim_wrap_liba_kernel_dispatch(liba_queue_t queue, const liba_dispatch_packet_t* pkt, liba_signal_t completion)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBA_BASE + 2), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 2], memory_order_acquire);
        return ((liba_kernel_dispatch_fn)orig)(queue, pkt, completion);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 2], memory_order_acquire);
    packed_liba_op2_t args = { queue, pkt, completion };
    shim_handle_event(SHIM_LIBA_BASE + 2, mode, orig, &args, sizeof(args));
    return LIBA_STATUS_SUCCESS;
}

static liba_status_t shim_wrap_liba_signal_wait(liba_signal_t signal, uint64_t timeout_ns)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBA_BASE + 3), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 3], memory_order_acquire);
        return ((liba_signal_wait_fn)orig)(signal, timeout_ns);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBA_BASE + 3], memory_order_acquire);
    packed_liba_op3_t args = { signal, timeout_ns };
    shim_handle_event(SHIM_LIBA_BASE + 3, mode, orig, &args, sizeof(args));
    return LIBA_STATUS_SUCCESS;
}

/* ================================================================== */
/* libB_hip wrappers (ops 6-9) — typed per-op, matching mock_libB.h    */
/* ================================================================== */

typedef libb_error_t (*libb_launch_kernel_fn)(const libb_launch_config_t*);
typedef libb_error_t (*libb_memcpy_async_fn)(void*, const void*, uint64_t, libb_stream_t);
typedef libb_error_t (*libb_stream_sync_fn)(libb_stream_t);
typedef libb_error_t (*libb_get_dev_prop_fn)(libb_device_prop_t*, int);

typedef struct { const libb_launch_config_t* config; } packed_libb_op0_t;
typedef struct { void* dst; const void* src; uint64_t size; libb_stream_t stream; } packed_libb_op1_t;
typedef struct { libb_stream_t stream; } packed_libb_op2_t;
typedef struct { libb_device_prop_t* prop; int device_id; } packed_libb_op3_t;

#define SHIM_LIBB_BASE g_libb_base

static libb_error_t shim_wrap_libb_launch_kernel(const libb_launch_config_t* cfg)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBB_BASE + 0), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 0], memory_order_acquire);
        return ((libb_launch_kernel_fn)orig)(cfg);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 0], memory_order_acquire);
    packed_libb_op0_t args = { cfg };
    shim_handle_event(SHIM_LIBB_BASE + 0, mode, orig, &args, sizeof(args));
    return LIBB_SUCCESS;
}

static libb_error_t shim_wrap_libb_memcpy_async(void* dst, const void* src, uint64_t size, libb_stream_t stream)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBB_BASE + 1), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 1], memory_order_acquire);
        return ((libb_memcpy_async_fn)orig)(dst, src, size, stream);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 1], memory_order_acquire);
    packed_libb_op1_t args = { dst, src, size, stream };
    shim_handle_event(SHIM_LIBB_BASE + 1, mode, orig, &args, sizeof(args));
    return LIBB_SUCCESS;
}

static libb_error_t shim_wrap_libb_stream_sync(libb_stream_t stream)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBB_BASE + 2), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 2], memory_order_acquire);
        return ((libb_stream_sync_fn)orig)(stream);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 2], memory_order_acquire);
    packed_libb_op2_t args = { stream };
    shim_handle_event(SHIM_LIBB_BASE + 2, mode, orig, &args, sizeof(args));
    return LIBB_SUCCESS;
}

static libb_error_t shim_wrap_libb_get_dev_prop(libb_device_prop_t* prop, int device_id)
{
    uint32_t mode = atomic_load_explicit(shim_op_mode_ptr(SHIM_LIBB_BASE + 3), memory_order_acquire);
    if (__builtin_expect(mode == ROCP_SHIM_MODE_OFF, 1)) {
        void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 3], memory_order_acquire);
        return ((libb_get_dev_prop_fn)orig)(prop, device_id);
    }
    void* orig = atomic_load_explicit(&g_runtime_original[SHIM_LIBB_BASE + 3], memory_order_acquire);
    packed_libb_op3_t args = { prop, device_id };
    shim_handle_event(SHIM_LIBB_BASE + 3, mode, orig, &args, sizeof(args));
    return LIBB_SUCCESS;
}

/* ================================================================== */
/* Generic table installation helper                                   */
/* ================================================================== */

static void* g_wrapper_table_liba[] = {
    (void*)&shim_wrap_liba_queue_create,
    (void*)&shim_wrap_liba_memory_allocate,
    (void*)&shim_wrap_liba_kernel_dispatch,
    (void*)&shim_wrap_liba_signal_wait,
};

static void* g_wrapper_table_libb[] = {
    (void*)&shim_wrap_libb_launch_kernel,
    (void*)&shim_wrap_libb_memcpy_async,
    (void*)&shim_wrap_libb_stream_sync,
    (void*)&shim_wrap_libb_get_dev_prop,
};

static void shim_install_generic_table(void** slots, uint64_t num_entries,
                                       uint32_t base_slot,
                                       void** wrapper_table, uint32_t n_wrappers)
{
    pthread_mutex_lock(&g_shim_install_lock);
    for (uint32_t i = 0; i < num_entries && i < n_wrappers; i++) {
        uint32_t slot = base_slot + i;
        if (slot >= SHIM_NUM_OPS || g_shim_installed[slot]) continue;
        void* cur = slots[i];
        if (cur != wrapper_table[i]) {
            atomic_store_explicit(&g_runtime_original[slot], cur, memory_order_release);
            atomic_store_explicit(&g_next_in_chain[slot], cur, memory_order_release);
            atomic_store_explicit((_Atomic(void*)*)&slots[i], wrapper_table[i],
                                  memory_order_release);
        }
        g_shim_installed[slot] = 1;
    }
    pthread_mutex_unlock(&g_shim_install_lock);
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
            atomic_store_explicit(&g_runtime_original[g_mylib_base + 0], cur,
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
            atomic_store_explicit(&g_runtime_original[g_mylib_base + 1], cur,
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
                           uint64_t    lib_version,
                           uint64_t    lib_instance,
                           void**      tables,
                           uint64_t    num_tables)
{
    (void)lib_version;
    (void)lib_instance;
    if (!name || !tables || num_tables == 0) return -1;

    /* Dereference tables[0] to get the table struct.
     * Layout: { size_t size; void* fn_ptr[]; }
     * Extract the function-pointer slots that follow the size field. */
    void*    table_struct  = tables[0];
    size_t   table_size    = *(size_t*)table_struct;
    void**   fn_slots      = (void**)((char*)table_struct + sizeof(size_t));
    uint64_t num_fn_entries = (table_size - sizeof(size_t)) / sizeof(void*);

    /* Allocate a slot range in the memfd for this table FIRST, so the
     * runtime base is known before wrapper installation. */
    uint32_t base = 0;
    if (g_ipc_ok && g_ipc.ctrl) {
        base = g_ipc.ctrl->total_ops;
    }

    /* Set the runtime slot base for this library and install wrappers. */
    static void* mylib_wrappers[] = { (void*)&shim_wrap_op0, (void*)&shim_wrap_op1 };
    if (strcmp(name, "mylib") == 0) {
        g_mylib_base = base;
        shim_install_generic_table(fn_slots, num_fn_entries, base,
                                   mylib_wrappers, 2);
    } else if (strcmp(name, "libA_hsa") == 0) {
        g_liba_base = base;
        shim_install_generic_table(fn_slots, num_fn_entries, base,
                                   g_wrapper_table_liba, 4);
    } else if (strcmp(name, "libB_hip") == 0) {
        g_libb_base = base;
        shim_install_generic_table(fn_slots, num_fn_entries, base,
                                   g_wrapper_table_libb, 4);
    }

    /* Register table metadata + op names in the memfd header. */
    if (g_ipc_ok && g_ipc.ctrl) {
        uint32_t idx = g_ipc.ctrl->n_registrations;
        if (idx < SHIM_MAX_REGISTRATIONS) {
            shim_table_registration_t* reg = &g_ipc.ctrl->registrations[idx];
            snprintf(reg->name, SHIM_TABLE_NAME_MAX, "%s", name);
            reg->lib_instance  = (uint32_t)lib_instance;
            reg->major_version = (uint32_t)(lib_version / 10000);
            reg->minor_version = (uint32_t)((lib_version / 100) % 100);
            reg->slot_base     = base;
            reg->n_ops         = (uint32_t)num_fn_entries;
            g_ipc.ctrl->n_registrations = idx + 1;
            g_ipc.ctrl->total_ops += (uint32_t)num_fn_entries;

            /* Register op names for this table. */
            static const char* mylib_names[] = {
                "my_traced_function", "set_simulated_work_duration"
            };
            static const char* liba_names[] = {
                "liba_queue_create", "liba_memory_allocate",
                "liba_kernel_dispatch", "liba_signal_wait"
            };
            static const char* libb_names[] = {
                "libb_launch_kernel", "libb_memcpy_async",
                "libb_stream_synchronize", "libb_get_device_properties"
            };
            const char** op_names = NULL;
            uint32_t max_names = 0;
            if (strcmp(name, "mylib") == 0)      { op_names = mylib_names; max_names = 2; }
            else if (strcmp(name, "libA_hsa") == 0) { op_names = liba_names; max_names = 4; }
            else if (strcmp(name, "libB_hip") == 0) { op_names = libb_names; max_names = 4; }
            if (op_names) {
                for (uint32_t i = 0; i < num_fn_entries && i < max_names; i++) {
                    snprintf(g_ipc.ctrl->op_info[base + i].name,
                             SHIM_OP_NAME_MAX, "%s", op_names[i]);
                }
            }

            /* Enable all new ops in the name-filter bitmap. */
            for (uint32_t i = 0; i < num_fn_entries; i++)
                shim_filter_set(g_ipc.ctrl->name_filter, base + i);
        }
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
        /* Do NOT pre-register tables here. Table registration happens
         * naturally when mock_register calls back into mock_sdk_set_api_table
         * for each runtime that registers. Pre-registering caused duplicate
         * mylib entries and slot-base mismatches with the compile-time
         * SHIM_LIBA_BASE / SHIM_LIBB_BASE constants. */
    }

    mock_register_set_api_table_fn = &mock_sdk_set_api_table;
}

/* get_arg_desc — defined here because it needs g_ipc (declared above). */
static const shim_op_arg_descriptor_t* get_arg_desc(uint32_t slot_idx)
{
    if (!g_ipc_ok || !g_ipc.ctrl) return NULL;
    for (uint32_t i = 0; i < g_ipc.ctrl->n_registrations; i++) {
        const shim_table_registration_t* t = &g_ipc.ctrl->registrations[i];
        if (slot_idx >= t->slot_base && slot_idx < t->slot_base + t->n_ops) {
            uint32_t local_op = slot_idx - t->slot_base;
            if (strcmp(t->name, "mylib") == 0 && local_op < 2)
                return &g_mylib_arg_descs[local_op];
            if (strcmp(t->name, "libA_hsa") == 0 && local_op < 4)
                return &g_liba_arg_descs[local_op];
            if (strcmp(t->name, "libB_hip") == 0 && local_op < 4)
                return &g_libb_arg_descs[local_op];
        }
    }
    return NULL;
}
