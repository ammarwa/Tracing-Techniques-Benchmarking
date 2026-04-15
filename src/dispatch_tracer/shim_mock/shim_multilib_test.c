/*
 * shim_multilib_test.c — Test app for cross-library correlation.
 *
 * Calls libB (HIP-level) APIs which internally call libA (HSA-level).
 * When the shim wraps both tables, the consumer sees ENTER/EXIT pairs
 * for both libraries with ancestor fields linking child (libA) to
 * parent (libB) calls.
 *
 * Usage: shim_multilib_test [iterations]
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "mock_libB.h"

int main(int argc, char** argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 10;

    printf("[multilib_test pid=%d] %d iterations\n", getpid(), iters);

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
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (double)(end.tv_sec - start.tv_sec)
                   + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    printf("[multilib_test] %d iterations in %.3f ms\n", iters, elapsed * 1000);
    return 0;
}
