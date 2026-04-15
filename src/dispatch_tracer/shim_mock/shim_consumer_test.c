/*
 * shim_consumer_test.c — minimal OOP consumer for the shim mock.
 *
 * Usage: shim_consumer_test <target_pid> [duration_sec]
 *
 * Validates the end-to-end path: socket connect → SO_PEERCRED →
 * SCM_RIGHTS memfd+eventfd → mmap → enable ops → poll ring → print
 * records → detach. Matches SHIM_MEMFD_SOCK_DESIGN §4, §10, §13.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

#include "shim_protocol.h"
#include "shim_ipc.h"

static volatile int g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* Global ctrl pointer for op_info name lookup inside the callback. */
static shim_ctrl_t* g_con_ctrl = NULL;

/* Include the typed arg structs from the mock libraries so the consumer
 * can decode args for every op. In the real shim, these come from
 * rocprofiler-sdk's generated headers. */
#include "mock_libA.h"
#include "mock_libB.h"

/* Packed arg structs — matching shim_mock.c */
typedef struct { int a1; uint64_t a2; double a3; void* a4; } packed_mylib_op0_t;
typedef struct { unsigned int us; } packed_mylib_op1_t;
typedef struct { liba_agent_t agent; uint32_t size; liba_queue_t* out; } packed_liba_op0_t;
typedef struct { liba_memory_region_t region; uint64_t size; void** out_ptr; } packed_liba_op1_t;
typedef struct { liba_queue_t queue; const liba_dispatch_packet_t* pkt; liba_signal_t completion; } packed_liba_op2_t;
typedef struct { liba_signal_t signal; uint64_t timeout_ns; } packed_liba_op3_t;
typedef struct { const libb_launch_config_t* config; } packed_libb_op0_t;
typedef struct { void* dst; const void* src; uint64_t size; libb_stream_t stream; } packed_libb_op1_t;
typedef struct { libb_stream_t stream; } packed_libb_op2_t;
typedef struct { libb_device_prop_t* prop; int device_id; } packed_libb_op3_t;

/* Find which table a global slot_idx belongs to, return the table name
 * and the local op index within that table. */
static const char* find_table_for_op(uint32_t slot_idx, uint32_t* local_op)
{
    if (!g_con_ctrl) return NULL;
    for (uint32_t i = 0; i < g_con_ctrl->n_registrations; i++) {
        const shim_table_registration_t* t = &g_con_ctrl->registrations[i];
        if (slot_idx >= t->slot_base && slot_idx < t->slot_base + t->n_ops) {
            *local_op = slot_idx - t->slot_base;
            return t->name;
        }
    }
    return NULL;
}

static void print_args(const shim_record_t* rec)
{
    /* Args are captured on EXIT (after orig() returns) so output params
     * like out_queue, prop, etc. have their final values. */
    if (rec->phase != SHIM_PHASE_EXIT || rec->arg_bytes == 0) return;

    uint32_t local_op = 0;
    const char* tbl = find_table_for_op(rec->op, &local_op);
    if (!tbl) return;

    if (strcmp(tbl, "mylib") == 0) {
        if (local_op == 0 && rec->arg_bytes >= sizeof(packed_mylib_op0_t)) {
            const packed_mylib_op0_t* a = (const packed_mylib_op0_t*)rec->args;
            printf("  args(a1=%d, a2=%" PRIu64 ", a3=%.1f, a4=%p)",
                   a->a1, a->a2, a->a3, a->a4);
        } else if (local_op == 1 && rec->arg_bytes >= sizeof(packed_mylib_op1_t)) {
            const packed_mylib_op1_t* a = (const packed_mylib_op1_t*)rec->args;
            printf("  args(us=%u)", a->us);
        }
    } else if (strcmp(tbl, "libA_hsa") == 0) {
        if (local_op == 0 && rec->arg_bytes >= sizeof(packed_liba_op0_t)) {
            const packed_liba_op0_t* a = (const packed_liba_op0_t*)rec->args;
            printf("  args(agent=0x%" PRIx64 ", size=%u, out=%p)",
                   a->agent.handle, a->size, (void*)a->out);
        } else if (local_op == 1 && rec->arg_bytes >= sizeof(packed_liba_op1_t)) {
            const packed_liba_op1_t* a = (const packed_liba_op1_t*)rec->args;
            printf("  args(region={base=0x%" PRIx64 ", size=%" PRIu64 ", seg=%u}, sz=%" PRIu64 ")",
                   a->region.base_address, a->region.size_bytes, a->region.segment, a->size);
        } else if (local_op == 2 && rec->arg_bytes >= sizeof(packed_liba_op2_t)) {
            const packed_liba_op2_t* a = (const packed_liba_op2_t*)rec->args;
            printf("  args(queue=0x%" PRIx64 ", pkt=%p, completion=0x%" PRIx64 ")",
                   a->queue.handle, (void*)a->pkt, a->completion.handle);
        } else if (local_op == 3 && rec->arg_bytes >= sizeof(packed_liba_op3_t)) {
            const packed_liba_op3_t* a = (const packed_liba_op3_t*)rec->args;
            printf("  args(signal=0x%" PRIx64 ", timeout=%" PRIu64 ")",
                   a->signal.handle, a->timeout_ns);
        }
    } else if (strcmp(tbl, "libB_hip") == 0) {
        if (local_op == 0 && rec->arg_bytes >= sizeof(packed_libb_op0_t)) {
            const packed_libb_op0_t* a = (const packed_libb_op0_t*)rec->args;
            /* config is a pointer into the target's stack — cannot deref
             * from the consumer process (or even later in the same process
             * after the stack frame is gone). Print pointer value only.
             * This matches §7B.3: HANDLE args are opaque pointer values. */
            printf("  args(config=%p)", (void*)a->config);
        } else if (local_op == 1 && rec->arg_bytes >= sizeof(packed_libb_op1_t)) {
            const packed_libb_op1_t* a = (const packed_libb_op1_t*)rec->args;
            printf("  args(dst=%p, src=%p, size=%" PRIu64 ", stream=0x%" PRIx64 ")",
                   a->dst, a->src, a->size, a->stream.handle);
        } else if (local_op == 2 && rec->arg_bytes >= sizeof(packed_libb_op2_t)) {
            const packed_libb_op2_t* a = (const packed_libb_op2_t*)rec->args;
            printf("  args(stream=0x%" PRIx64 ")", a->stream.handle);
        } else if (local_op == 3 && rec->arg_bytes >= sizeof(packed_libb_op3_t)) {
            const packed_libb_op3_t* a = (const packed_libb_op3_t*)rec->args;
            printf("  args(prop=%p, device_id=%d)", (void*)a->prop, a->device_id);
        }
    }
}

static void on_record(const shim_record_t* rec, void* user_data)
{
    uint64_t* count = (uint64_t*)user_data;
    (*count)++;
    if (*count > 40 && (*count % 10000) != 0) return;

    const char* name = "?";
    if (g_con_ctrl && rec->op < g_con_ctrl->total_ops)
        name = g_con_ctrl->op_info[rec->op].name;

    const char* phase = rec->phase == SHIM_PHASE_ENTER ? "ENTER" :
                        rec->phase == SHIM_PHASE_EXIT  ? "EXIT " : "UNRCH";

    printf("[tsc=%" PRIu64 "] %s  %s  tid=%" PRIu64
           "  corr={i=%" PRIu64 " e=%" PRIu64 " a=%" PRIu64 "}",
           rec->tsc, phase, name, rec->thread_id,
           rec->correlation_id.internal,
           rec->correlation_id.external,
           rec->correlation_id.ancestor);

    print_args(rec);
    printf("\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pid> [duration_sec]\n", argv[0]);
        return 2;
    }
    pid_t target = atoi(argv[1]);
    int duration = argc > 2 ? atoi(argv[2]) : 5;
    signal(SIGINT, on_sigint);

    /* 1. Attach (§4) */
    shim_ipc_consumer_t con;
    int rc = shim_consumer_attach(target, &con);
    if (rc) {
        fprintf(stderr, "attach(%d) failed\n", target);
        return 1;
    }

    g_con_ctrl = con.ctrl;

    /* Print registrations (§13.6) */
    printf("=== Attached to pid=%u, %u registrations, %u total_ops ===\n",
           con.ctrl->pid, con.ctrl->n_registrations, con.ctrl->total_ops);
    for (uint32_t i = 0; i < con.ctrl->n_registrations; i++) {
        const shim_table_registration_t* t = &con.ctrl->registrations[i];
        printf("  table[%u]: name=\"%s\" instance=%u v%u.%u slots=[%u..%u)\n",
               i, t->name, t->lib_instance,
               t->major_version, t->minor_version,
               t->slot_base, t->slot_base + t->n_ops);
    }

    /* 2. Enable all ops with RECORD mode (§5.1 install-while-off) */
    for (uint32_t i = 0; i < con.ctrl->total_ops; i++) {
        atomic_store_explicit(&con.ctrl->op_mode[i],
                              ROCP_SHIM_MODE_RECORD,
                              memory_order_release);
    }
    atomic_fetch_add(&con.ctrl->gen_counter, 1);
    printf("=== Enabled %u ops, mode=RECORD ===\n", con.ctrl->total_ops);

    /* 3. Poll loop */
    uint64_t total_records = 0;
    time_t start = time(NULL);
    while (!g_stop && (time(NULL) - start) < duration) {
        int n = shim_consumer_poll(&con, on_record, &total_records, 500);
        (void)n;
    }

    /* 4. Stats (§13.2) */
    uint64_t traced  = atomic_load(&con.ctrl->events_traced);
    uint64_t dropped = atomic_load(&con.ctrl->events_dropped);
    printf("=== Stats: traced=%" PRIu64 " dropped=%" PRIu64
           " consumer_read=%" PRIu64 " ===\n",
           traced, dropped, total_records);

    /* 5. Detach (§10.3) — zeros all op_mode slots */
    shim_consumer_detach(&con);
    printf("=== Detached ===\n");
    return 0;
}
