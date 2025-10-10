#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "mylib_tp.h"
#include "../sample_library/mylib.h"

// Function pointers to the real implementations - optimized for speed
static void (*real_my_traced_function)(int, uint64_t, double, void*) = NULL;
static void (*real_set_simulated_work_duration)(unsigned int) = NULL;

// Use GCC constructor to initialize once at library load time
__attribute__((constructor))
static void init_real_functions(void) {
    // First try RTLD_NEXT (for LD_PRELOAD case)
    real_my_traced_function = dlsym(RTLD_NEXT, "my_traced_function");

    // If that fails, explicitly load the original library
    if (!real_my_traced_function) {
        // Try multiple possible paths for the original library
        const char* lib_paths[] = {
            "./build/lib/libmylib.so.1",
            "./build/lib/libmylib.so",
            "./lib/libmylib.so.1",
            "./lib/libmylib.so",
            "../lib/libmylib.so.1",
            "../lib/libmylib.so",
            NULL
        };

        void* handle = NULL;
        for (int i = 0; lib_paths[i] != NULL && !handle; i++) {
            // Try RTLD_NOLOAD first (faster - only if already loaded)
            handle = dlopen(lib_paths[i], RTLD_LAZY | RTLD_NOLOAD);
            if (!handle) {
                // Fallback to normal loading
                handle = dlopen(lib_paths[i], RTLD_LAZY);
            }
        }

        if (handle) {
            real_my_traced_function = dlsym(handle, "my_traced_function");
            real_set_simulated_work_duration = dlsym(handle, "set_simulated_work_duration");
        }
    } else {
        real_set_simulated_work_duration = dlsym(RTLD_NEXT, "set_simulated_work_duration");
    }

    if (!real_my_traced_function) {
        fprintf(stderr, "Error: Could not find my_traced_function in any location\n");
        exit(1);
    }

    if (!real_set_simulated_work_duration) {
        fprintf(stderr, "Error: Could not find set_simulated_work_duration in any location\n");
        exit(1);
    }
}

// Wrapper function that adds tracing - optimized for minimal overhead
__attribute__((hot))
inline void my_traced_function(
    int arg1,
    uint64_t arg2,
    double arg3,
    void* arg4)
{
    // Entry tracepoint - optimized: reduced arguments, fast path
    tracepoint(mylib, my_traced_function_entry, arg1, arg2, arg4);

    // Call the real function - guaranteed to be initialized
    real_my_traced_function(arg1, arg2, arg3, arg4);

    // Exit tracepoint - minimal overhead version
    tracepoint(mylib, my_traced_function_exit);
}

// Wrapper for set_simulated_work_duration to pass through to real implementation
void set_simulated_work_duration(unsigned int sleep_us) {
    if (real_set_simulated_work_duration) {
        real_set_simulated_work_duration(sleep_us);
    }
}
