#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../sample_library/mylib.h"

void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <num_iterations>\n", prog);
    fprintf(stderr, "  num_iterations: Number of times to call the traced function\n");
    fprintf(stderr, "Example: %s 1000000\n", prog);
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

    // Check for SIMULATED_WORK_US environment variable
    const char* work_env = getenv("SIMULATED_WORK_US");
    if (work_env) {
        unsigned int work_us = atoi(work_env);
        set_simulated_work_duration(work_us);
        printf("Starting benchmark with %ld iterations (simulated work: %u μs)...\n",
               num_iterations, work_us);
    } else {
        printf("Starting benchmark with %ld iterations...\n", num_iterations);
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    // Call the traced function many times
    for (long i = 0; i < num_iterations; i++) {
        my_traced_function(
            42,                    // int arg1
            0xDEADBEEF,           // uint64_t arg2
            "test_string",        // const char* arg3
            3.14159,              // double arg4
            (void*)0x12345678     // void* arg5
        );
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    // Calculate elapsed time
    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("Completed %ld iterations in %.6f seconds\n", num_iterations, elapsed);
    printf("Average time per call: %.2f nanoseconds\n",
           (elapsed / num_iterations) * 1e9);

    return 0;
}
