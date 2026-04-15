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

/* The shim serializes args as an array of shim_arg_entry_t at EXIT time,
 * matching rocprofiler-sdk's iterate_callback_tracing_kind_operation_args
 * pattern. Each entry has: name (param name), type (C type string),
 * value (human-readable string). The consumer does NOT need to know the
 * struct layout — it just reads the string triples. */
#include "shim_arg_info.h"

static void print_args(const shim_record_t* rec)
{
    if (rec->phase != SHIM_PHASE_EXIT || rec->arg_bytes == 0) return;
    if (rec->arg_bytes < sizeof(shim_arg_entry_t)) return;

    uint32_t n_entries = rec->arg_bytes / sizeof(shim_arg_entry_t);
    const shim_arg_entry_t* entries = (const shim_arg_entry_t*)rec->args;

    printf("  (");
    for (uint32_t i = 0; i < n_entries; i++) {
        if (i > 0) printf(", ");
        printf("%s=%s", entries[i].name, entries[i].value);
    }
    printf(")");
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
