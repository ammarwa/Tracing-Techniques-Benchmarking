/*
 * sample_app_dispatch - benchmark harness for the dispatch-tracer variants.
 *
 * Linked against libmylib_dispatch (which routes calls through the
 * mock_register-published api_table). Stays alive long enough for a
 * controller to connect and attach. When the caller passes a large
 * iteration count, the app loops calling my_traced_function; between
 * iterations it checks for a stop file so controllers can cleanly end
 * the run without sending SIGTERM.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pwd.h>

#include "mylib_dispatch.h"
#include "rocp_protocol.h"

/* If WAIT_FOR_ATTACH=1, block until ctrl->context_active==1 before starting
 * the main iteration loop. Allows benchmark scripts to attach before the
 * workload runs, so active-tracing measurements reflect the full run.
 * Only works with the mmap-backed options (mmap, signal) which share the
 * control file at /run/user/<uid>/rocprofiler/<pid>/ctrl. */
static void wait_for_attach(void) {
    char path[256];
    snprintf(path, sizeof(path), "/run/user/%u/rocprofiler/%d/ctrl",
             (unsigned)getuid(), (int)getpid());

    /* Wait up to 30 seconds for the stub to create the ctrl file.
     * The stub creates it from its constructor, which runs before main(). */
    int fd = -1;
    for (int tries = 0; tries < 3000; tries++) {
        fd = open(path, O_RDWR);
        if (fd >= 0) break;
        usleep(10000);  /* 10 ms */
    }
    if (fd < 0) {
        fprintf(stderr, "[wait_for_attach] ctrl file never appeared: %s\n", path);
        return;
    }

    rocp_ctrl_t* ctrl = mmap(NULL, sizeof(rocp_ctrl_t),
                              PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (ctrl == MAP_FAILED) {
        fprintf(stderr, "[wait_for_attach] mmap failed\n");
        return;
    }

    /* Poll context_active until the controller attaches.
     * Use atomic_load_explicit for proper acquire semantics across the
     * shared mapping (portable to AArch64, not just x86-64).
     * Time out after 30 seconds to avoid hanging benchmark runs. */
    int max_tries = 30000;  /* 30 s at 1 ms intervals */
    for (int i = 0; i < max_tries; i++) {
        uint32_t active = atomic_load_explicit(
            (_Atomic uint32_t*)&ctrl->context_active, memory_order_acquire);
        if (active) break;
        usleep(1000);
    }
    munmap((void*)ctrl, sizeof(rocp_ctrl_t));
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <num_iterations>\n", prog);
    fprintf(stderr, "  num_iterations: Number of times to call the traced function\n");
    fprintf(stderr, "Example: %s 100000\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    long num_iterations = atol(argv[1]);
    if (num_iterations <= 0) {
        fprintf(stderr, "Error: num_iterations must be positive\n");
        return 1;
    }

    const char* work_env = getenv("SIMULATED_WORK_US");
    if (work_env) {
        unsigned int work_us = (unsigned int)atoi(work_env);
        set_simulated_work_duration(work_us);
        printf("[sample_app_dispatch pid=%d] %ld iters, work=%u us\n",
               getpid(), num_iterations, work_us);
    } else {
        printf("[sample_app_dispatch pid=%d] %ld iters\n",
               getpid(), num_iterations);
    }
    fflush(stdout);

    const char* wait_env = getenv("WAIT_FOR_ATTACH");
    if (wait_env && atoi(wait_env)) {
        printf("[sample_app_dispatch] waiting for controller attach...\n");
        fflush(stdout);
        wait_for_attach();
        printf("[sample_app_dispatch] attach detected, starting workload\n");
        fflush(stdout);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (long i = 0; i < num_iterations; i++) {
        my_traced_function(42, 0xDEADBEEFULL, 3.14159, (void*)0x12345678);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("Completed %ld iterations in %.6f seconds\n", num_iterations, elapsed);
    if (num_iterations > 0) {
        printf("Average time per call: %.2f nanoseconds\n",
               (elapsed / (double)num_iterations) * 1e9);
    }
    return 0;
}
