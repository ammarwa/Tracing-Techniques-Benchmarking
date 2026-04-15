/*
 * shim_multilib_test.c — Test app for cross-library correlation.
 *
 * Calls libB (HIP-level) APIs which internally call libA (HSA-level).
 * When the shim wraps both tables, the consumer sees ENTER/EXIT pairs
 * for both libraries with ancestor fields linking child (libA) to
 * parent (libB) calls.
 *
 * Usage: shim_multilib_test [iterations]
 *
 * Respects SIMULATED_WORK_US env var — adds a busy-wait between
 * iterations to slow down the loop, same as sample_app_dispatch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "mock_libB.h"
#include "mylib_dispatch.h"

static void busy_sleep_us(unsigned int microseconds)
{
    if (microseconds == 0) return;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long target_ns = (unsigned long)microseconds * 1000UL;
    unsigned long elapsed;
    do {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (unsigned long)(now.tv_sec - t0.tv_sec) * 1000000000UL
                + (unsigned long)(now.tv_nsec - t0.tv_nsec);
    } while (elapsed < target_ns);
}

int main(int argc, char** argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 10;

    const char* work_env = getenv("SIMULATED_WORK_US");
    unsigned int work_us = work_env ? (unsigned int)atoi(work_env) : 0;

    printf("[multilib_test pid=%d] %d iterations, work=%u us\n",
           getpid(), iters, work_us);

    /* Give the shim bg thread time to finish binding. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
    nanosleep(&ts, NULL);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < iters; i++) {
        /* 1. Launch a kernel (libB → libA chain) */
        libb_launch_config_t cfg = {
            .func       = (const void*)0xDEADC0DE,
            .grid       = { 256, 1, 1 },
            .block      = { 64, 1, 1 },
            .kernarg_ptr = NULL,
            .shared_mem = 4096,
            .stream     = { .handle = 0x42 },
        };
        libb_launch_kernel(&cfg);

        /* 2. Memcpy (libB → libA chain) */
        libb_memcpy_async((void*)0x1000, (void*)0x2000, 1024 * 1024,
                          (libb_stream_t){ .handle = 0x42 });

        /* 3. Sync (libB → libA chain) */
        libb_stream_synchronize((libb_stream_t){ .handle = 0x42 });

        /* 4. Query device properties (libB only, no libA call) */
        libb_device_prop_t prop;
        libb_get_device_properties(&prop, 0);

        /* 5. Call mylib's traced function (has shim wrappers installed,
         *    so the consumer will see ENTER/EXIT records with args). */
        my_traced_function(i, (uint64_t)cfg.grid.x, 3.14, (void*)(uintptr_t)i);

        if (work_us > 0) busy_sleep_us(work_us);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (double)(end.tv_sec - start.tv_sec)
                   + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    printf("[multilib_test] %d iterations in %.6f seconds\n", iters, elapsed);
    printf("Average time per iteration: %.2f nanoseconds\n",
           elapsed * 1e9 / iters);
    return 0;
}
