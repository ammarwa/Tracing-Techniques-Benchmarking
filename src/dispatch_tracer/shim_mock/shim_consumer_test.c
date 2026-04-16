/*
 * shim_consumer_test.c — OOP consumer for the new shim architecture.
 *
 * Uses the standard rocp_* API (linked via libroc-shim-consumer.so).
 * Identical API to an in-process tool — just linked differently.
 *
 * Usage: shim_consumer_test <target_pid> [duration_sec]
 *
 * Validates: connect → handshake → force_configure relay →
 * create_buffer → create_context → configure_buffer_tracing →
 * start_context → records flow → stop → detach.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mock_rocp_sdk.h"

/* Consumer-side connect/disconnect (from shim_consumer_lib.c) */
extern int  shim_consumer_connect(pid_t target_pid);
extern void shim_consumer_disconnect(void);

static volatile int g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ */
/* Buffer callback — fires when consumer buffer hits watermark         */
/* ------------------------------------------------------------------ */

static _Atomic uint64_t g_total_records = 0;

static void on_buffer_records(rocp_context_id_t ctx,
                              rocp_buffer_id_t buffer_id,
                              const shim_buffer_record_t* records,
                              uint64_t n_records,
                              void* user_data,
                              uint64_t drop_count)
{
    (void)ctx; (void)buffer_id; (void)user_data; (void)drop_count;

    uint64_t prev = atomic_fetch_add(&g_total_records, n_records);
    for (uint64_t i = 0; i < n_records; i++) {
        if (prev + i < 20 || ((prev + i) % 5000) == 0) {
            const shim_buffer_record_t* r = &records[i];
            printf("[rec %" PRIu64 "] kind=%u op=%u corr=%" PRIu64
                   " start=%" PRIu64 " end=%" PRIu64 " tid=%" PRIu64
                   " delta=%" PRIu64 "\n",
                   prev + i, r->kind, r->operation,
                   r->correlation_id,
                   r->start_timestamp, r->end_timestamp,
                   r->thread_id,
                   r->end_timestamp - r->start_timestamp);
        }
    }
}

/* ------------------------------------------------------------------ */
/* tool_initialize — called during force_configure relay               */
/* ------------------------------------------------------------------ */

static rocp_context_id_t g_ctx;
static rocp_buffer_id_t  g_buf;

static int my_tool_init(void* fini, void* tool_data)
{
    (void)fini; (void)tool_data;

    rocp_status_t rc;

    rc = rocp_create_context(&g_ctx);
    if (rc != ROCP_STATUS_SUCCESS) {
        fprintf(stderr, "[tool] create_context failed\n");
        return -1;
    }
    printf("[tool] context created: id=%" PRIu64 "\n", g_ctx.handle);

    rc = rocp_create_buffer(g_ctx, 65536, 4096,
                            on_buffer_records, NULL, &g_buf);
    if (rc != ROCP_STATUS_SUCCESS) {
        fprintf(stderr, "[tool] create_buffer failed\n");
        return -1;
    }
    printf("[tool] buffer created: id=%" PRIu64 "\n", g_buf.handle);

    rc = rocp_configure_buffer_tracing_service(
        g_ctx, SHIM_BUF_TRACING_HIP_RUNTIME_API,
        NULL, 0, g_buf);
    if (rc != ROCP_STATUS_SUCCESS) {
        fprintf(stderr, "[tool] configure_buffer_tracing failed\n");
        return -1;
    }
    printf("[tool] HIP runtime API tracing enabled (all ops)\n");

    rc = rocp_configure_buffer_tracing_service(
        g_ctx, SHIM_BUF_TRACING_HSA_CORE_API,
        NULL, 0, g_buf);
    if (rc != ROCP_STATUS_SUCCESS) {
        fprintf(stderr, "[tool] configure HSA tracing failed\n");
        return -1;
    }
    printf("[tool] HSA core API tracing enabled (all ops)\n");

    rc = rocp_start_context(g_ctx);
    if (rc != ROCP_STATUS_SUCCESS) {
        fprintf(stderr, "[tool] start_context failed\n");
        return -1;
    }
    printf("[tool] context started — records should flow now\n");

    return 0;
}

static rocp_tool_configure_result_t g_tool_result = {
    .size       = sizeof(rocp_tool_configure_result_t),
    .initialize = my_tool_init,
    .finalize   = NULL,
};

static rocp_tool_configure_result_t* my_tool_configure(
    uint32_t version, const char* runtime_version,
    uint32_t priority, void* client_id)
{
    (void)version; (void)runtime_version; (void)priority; (void)client_id;
    return &g_tool_result;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <target_pid> [duration_sec]\n", argv[0]);
        return 2;
    }
    pid_t target = atoi(argv[1]);
    int duration = argc > 2 ? atoi(argv[2]) : 5;
    signal(SIGINT, on_sigint);

    /* 1. Connect to target's shim */
    printf("=== Connecting to pid=%d ===\n", target);
    if (shim_consumer_connect(target) < 0) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }

    /* 2. force_configure → tool_init relay */
    printf("=== Calling force_configure ===\n");
    rocp_status_t rc = rocp_force_configure(my_tool_configure);
    if (rc != ROCP_STATUS_SUCCESS) {
        fprintf(stderr, "force_configure failed\n");
        shim_consumer_disconnect();
        return 1;
    }
    printf("=== force_configure succeeded ===\n");

    /* 3. Wait for records */
    printf("=== Collecting for %d seconds ===\n", duration);
    time_t start = time(NULL);
    while (!g_stop && (time(NULL) - start) < duration) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&ts, NULL);
    }

    /* 4. Stop + flush + cleanup */
    printf("=== Stopping ===\n");
    rocp_stop_context(g_ctx);
    rocp_flush_buffer(g_buf);

    uint64_t total = atomic_load(&g_total_records);
    printf("=== Total records: %" PRIu64 " ===\n", total);

    rocp_destroy_buffer(g_buf);
    rocp_destroy_context(g_ctx);
    shim_consumer_disconnect();
    printf("=== Detached ===\n");
    return 0;
}
