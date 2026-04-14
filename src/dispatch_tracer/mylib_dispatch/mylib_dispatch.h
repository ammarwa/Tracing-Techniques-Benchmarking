/*
 * mylib_dispatch.h - Public API for the dispatch-table-based mylib.
 *
 * Mirrors src/sample/sample_library/mylib.h but internally routes every
 * call through a mutable api_table registered with the mock
 * rocprofiler-register. Downstream tools can swap entries in the table
 * to intercept calls.
 */
#ifndef MYLIB_DISPATCH_H
#define MYLIB_DISPATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYLIB_API_TABLE_VERSION 1u

typedef struct {
    void (*my_traced_function)(int arg1, uint64_t arg2, double arg3, void* arg4);
    void (*set_simulated_work_duration)(unsigned int sleep_us);
} mylib_api_table_t;

/* Public API — same signatures as the original mylib. */
void my_traced_function(int arg1, uint64_t arg2, double arg3, void* arg4);
void set_simulated_work_duration(unsigned int sleep_us);

#ifdef __cplusplus
}
#endif

#endif /* MYLIB_DISPATCH_H */
