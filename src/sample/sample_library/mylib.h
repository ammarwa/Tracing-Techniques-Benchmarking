#ifndef MYLIB_H
#define MYLIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sample API function with multiple arguments
// Does nothing but provides a realistic function signature for tracing
void my_traced_function(
    int arg1,
    uint64_t arg2,
    double arg3,
    void* arg4
);

// Set simulated work duration (in microseconds)
// sleep_us = 0: Empty function (minimal work)
// sleep_us > 0: Sleep for specified duration to simulate real API calls
void set_simulated_work_duration(unsigned int sleep_us);

#ifdef __cplusplus
}
#endif

#endif // MYLIB_H
