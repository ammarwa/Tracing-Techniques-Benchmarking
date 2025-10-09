#include "mylib.h"
#include <string.h>
#include <time.h>

// Volatile to prevent compiler optimization
static volatile int dummy = 0;
static unsigned int simulated_work_us = 0;

// Busy-wait nanosleep for accurate microsecond delays
static void busy_sleep_us(unsigned int microseconds) {
    if (microseconds == 0) return;

    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);

    unsigned long target_ns = microseconds * 1000UL;
    unsigned long elapsed_ns;

    do {
        clock_gettime(CLOCK_MONOTONIC, &current);
        elapsed_ns = (current.tv_sec - start.tv_sec) * 1000000000UL +
                     (current.tv_nsec - start.tv_nsec);
    } while (elapsed_ns < target_ns);
}

void set_simulated_work_duration(unsigned int sleep_us) {
    simulated_work_us = sleep_us;
}

void my_traced_function(
    int arg1,
    uint64_t arg2,
    double arg3,
    void* arg4)
{
    // Do some minimal work to prevent complete optimization
    dummy = arg1 + (int)arg2;
    dummy += (int)arg3;

    if (arg4) {
        dummy += 1;
    }

    // Simulate realistic API work duration
    if (simulated_work_us > 0) {
        busy_sleep_us(simulated_work_us);
    }
}
