/*
 * shim_mock.c — libshim_mock.so
 *
 * Mock of Jonathan Madsen's proposed rocprofiler-sdk-shim design:
 * rocprofiler-register unconditionally dlopens this library when a
 * runtime registers a dispatch table. The shim wraps every table entry
 * with a thin "maybe-call-profiler" functor of the shape:
 *
 *     functor(args...) {
 *         auto prof = atomic_load(&profiler_functor[Op]);
 *         if (prof) prof(orig, args...);
 *         else      orig(args...);
 *     }
 *
 * When no OOP tool has attached, profiler_functor[Op] is NULL and the
 * functor degenerates to one atomic-acquire load + one well-predicted
 * branch + a tail call to the original function pointer. The purpose of
 * this mock is to measure that path against baseline (no wrappers at
 * all) and confirm Jonathan's "noise-equivalent" claim — or refute it.
 *
 * This library is a drop-in replacement for libmock_sdk.so at the
 * mock_register dlopen seam:
 *   - Exports rocprofiler_configure so any test that needs
 *     register-lib's symbol-scan path to succeed still works.
 *   - Exports mock_sdk_set_api_table, which is what mock_register calls
 *     for every registered table. This is where the shim installs its
 *     wrappers.
 *
 * Switching the shim in requires only:
 *   MOCK_REGISTER_LIB=$BUILD_DIR/lib/libshim_mock.so
 * (see src/dispatch_tracer/mock_register/mock_register.c).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "mock_register.h"
#include "mylib_dispatch.h"

/* ------------------------------------------------------------------ */
/* Per-op state: one cell per dispatch-table slot we care about.       */
/* ------------------------------------------------------------------ */

#define SHIM_NUM_OPS 2   /* op0 = my_traced_function, op1 = set_simulated_work_duration */

/* Type-erased function-pointer holders. The shim wrappers cast back to
 * the known signatures at call time. Keep these as plain void* with
 * atomic stores/loads so the "is a profiler attached?" check costs a
 * single atomic-acquire load per call. */
static _Atomic(void*) g_shim_orig[SHIM_NUM_OPS]     = { NULL, NULL };
static _Atomic(void*) g_shim_profiler[SHIM_NUM_OPS] = { NULL, NULL };

/* Book-keeping. Not touched on the hot path. */
static pthread_mutex_t g_shim_install_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_shim_installed[SHIM_NUM_OPS] = { 0, 0 };

/* Profiler function-pointer signatures. Each profiler functor gets the
 * original function pointer as its first argument plus the original
 * call arguments, and is responsible for (optionally) invoking the
 * original. This matches Jonathan's sketch. */
typedef void (*shim_prof_op0_t)(void* orig, int a1, uint64_t a2, double a3, void* a4);
typedef void (*shim_prof_op1_t)(void* orig, unsigned int us);

/* Signatures of the original library entry points. */
typedef void (*orig_op0_t)(int, uint64_t, double, void*);
typedef void (*orig_op1_t)(unsigned int);

/* ------------------------------------------------------------------ */
/* The shim wrappers — this is the *only* code path on the hot path.   */
/*                                                                     */
/*   mov    g_shim_profiler[Op](%rip), %rax   # 1 L1 load              */
/*   test   %rax, %rax                        # 1 cmp                  */
/*   jz     .Lfast                            # 1 predicted branch     */
/*   mov    g_shim_orig[Op](%rip), %rdi       # (profiler path)        */
/*   jmp    *%rax                                                      */
/* .Lfast:                                                             */
/*   mov    g_shim_orig[Op](%rip), %rax                                */
/*   jmp    *%rax                             # tail-call original     */
/* ------------------------------------------------------------------ */

static void shim_wrap_op0(int a1, uint64_t a2, double a3, void* a4)
{
    void* prof = atomic_load_explicit(&g_shim_profiler[0], memory_order_acquire);
    void* orig = atomic_load_explicit(&g_shim_orig[0],     memory_order_acquire);
    if (__builtin_expect(prof == NULL, 1)) {
        ((orig_op0_t)orig)(a1, a2, a3, a4);
        return;
    }
    ((shim_prof_op0_t)prof)(orig, a1, a2, a3, a4);
}

static void shim_wrap_op1(unsigned int us)
{
    void* prof = atomic_load_explicit(&g_shim_profiler[1], memory_order_acquire);
    void* orig = atomic_load_explicit(&g_shim_orig[1],     memory_order_acquire);
    if (__builtin_expect(prof == NULL, 1)) {
        ((orig_op1_t)orig)(us);
        return;
    }
    ((shim_prof_op1_t)prof)(orig, us);
}

/* ------------------------------------------------------------------ */
/* Installation: rewrite the runtime's api_table to point at our       */
/* wrappers and save the originals for the fast path.                  */
/* ------------------------------------------------------------------ */

static void shim_install_mylib_table(void** slots, uint64_t num_entries)
{
    pthread_mutex_lock(&g_shim_install_lock);

    /* Slot 0: my_traced_function. */
    if (num_entries > 0 && !g_shim_installed[0]) {
        void* cur = slots[0];
        /* Guard against re-install if mock_register replays the table. */
        if (cur != (void*)&shim_wrap_op0) {
            atomic_store_explicit(&g_shim_orig[0], cur, memory_order_release);
            slots[0] = (void*)&shim_wrap_op0;
        }
        g_shim_installed[0] = 1;
    }

    /* Slot 1: set_simulated_work_duration. */
    if (num_entries > 1 && !g_shim_installed[1]) {
        void* cur = slots[1];
        if (cur != (void*)&shim_wrap_op1) {
            atomic_store_explicit(&g_shim_orig[1], cur, memory_order_release);
            slots[1] = (void*)&shim_wrap_op1;
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

    /* For this mock we know only one runtime: "mylib". Extend to HIP /
     * HSA / RCCL / OMPT the same way mock_sdk_set_api_table does in
     * the full SDK mock (one set of wrappers per runtime × per op). */
    if (strcmp(name, "mylib") == 0 && api_tables[0]) {
        uint64_t num_entries =
            sizeof(mylib_api_table_t) / sizeof(void*);
        shim_install_mylib_table((void**)api_tables[0], num_entries);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Trivial rocprofiler_configure — exported so mock_register's symbol  */
/* scan succeeds for tests that still go through that path. The shim   */
/* itself does not need tool configuration; this exists only so the    */
/* dlopen+symbol-present handshake that register-lib performs does not */
/* short-circuit before mock_sdk_set_api_table gets a chance to run.   */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int rocprofiler_configure(uint32_t version, const char* runtime_version,
                          uint32_t priority, void* client_id)
{
    (void)version; (void)runtime_version; (void)priority; (void)client_id;
    return 0;  /* no-op — the shim is self-configuring */
}

/* ------------------------------------------------------------------ */
/* OOP-side entry points — invoked by an external tool (or a test) to  */
/* flip profiler_functor slots on and off. Kept deliberately simple:   */
/* no IPC here. The real shim would gate this behind an authenticated  */
/* abstract-socket + SCM_RIGHTS memfd handshake (see MEMFD.md); for    */
/* the hot-path-cost experiment all we need is a way to toggle the     */
/* pointer from inside the same process (e.g. from the benchmark       */
/* harness itself via dlsym).                                          */
/* ------------------------------------------------------------------ */

__attribute__((visibility("default")))
int shim_set_profiler_functor(int op, void* fn)
{
    if (op < 0 || op >= SHIM_NUM_OPS) return -1;
    atomic_store_explicit(&g_shim_profiler[op], fn, memory_order_release);
    return 0;
}

__attribute__((visibility("default")))
void* shim_get_original(int op)
{
    if (op < 0 || op >= SHIM_NUM_OPS) return NULL;
    return atomic_load_explicit(&g_shim_orig[op], memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* mock_register wiring. When mock_register dlopens this library, its  */
/* constructor sets the register-side function pointer so that the     */
/* re-replay path hands tables to us directly. Mirror what             */
/* libmock_sdk.so does.                                                */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void shim_register_ctor(void)
{
    mock_register_set_api_table_fn = &mock_sdk_set_api_table;
    mock_register_invoke_all_registrations();
}
