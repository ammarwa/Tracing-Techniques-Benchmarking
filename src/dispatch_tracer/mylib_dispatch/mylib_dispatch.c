/*
 * mylib_dispatch.c - libmylib_dispatch.so
 *
 * Drop-in replacement of mylib that uses the rocprofiler-register
 * dispatch pattern. The library ships with a mutable api_table of
 * function pointers pointing at its internal implementations, and on
 * load registers this table with mock_register_library_api_table(). If
 * a tool is present and the mock SDK loads, the SDK will rewrite the
 * table entries in place to install wrapper functors.
 *
 * The public C entry points go through the table so the wrappers
 * actually run.
 */
#include "mylib_dispatch.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

#include "mock_register.h"

/* -------- Real implementations (private) -------- */

static volatile int       g_dummy              = 0;
static unsigned int       g_simulated_work_us  = 0;

static void busy_sleep_us(unsigned int microseconds)
{
    if (microseconds == 0) return;
    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);
    unsigned long target_ns = (unsigned long)microseconds * 1000UL;
    unsigned long elapsed_ns;
    do {
        clock_gettime(CLOCK_MONOTONIC, &current);
        elapsed_ns = (unsigned long)(current.tv_sec - start.tv_sec) * 1000000000UL
                   + (unsigned long)(current.tv_nsec - start.tv_nsec);
    } while (elapsed_ns < target_ns);
}

static void real_my_traced_function(int arg1, uint64_t arg2,
                                    double arg3, void* arg4)
{
    g_dummy = arg1 + (int)arg2;
    g_dummy += (int)arg3;
    if (arg4) g_dummy += 1;
    if (g_simulated_work_us > 0) busy_sleep_us(g_simulated_work_us);
}

static void real_set_simulated_work_duration(unsigned int sleep_us)
{
    g_simulated_work_us = sleep_us;
}

/* -------- Mutable dispatch table -------- */

static mylib_api_table_t g_api_table = {
    .my_traced_function          = &real_my_traced_function,
    .set_simulated_work_duration = &real_set_simulated_work_duration,
};

/* -------- Public API routes through the table -------- */

void my_traced_function(int arg1, uint64_t arg2, double arg3, void* arg4)
{
    g_api_table.my_traced_function(arg1, arg2, arg3, arg4);
}

void set_simulated_work_duration(unsigned int sleep_us)
{
    g_api_table.set_simulated_work_duration(sleep_us);
}

/* -------- Registration -------- */

__attribute__((constructor))
static void mylib_dispatch_register(void)
{
    /* Publish the api_table to mock_register. The "tables" array layout
     * follows the rocprofiler-register convention: we publish the
     * address of our api_table's first slot plus the number of entries
     * so the SDK's update_table can index into it. */
    void** table_slots = (void**)&g_api_table;
    uint64_t num_slots =
        sizeof(mylib_api_table_t) / sizeof(void*);

    (void)mock_register_library_api_table("mylib",
                                          MYLIB_API_TABLE_VERSION,
                                          table_slots,
                                          num_slots);
}
