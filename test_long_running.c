#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "src/sample/sample_library/mylib.h"

int main(int argc, char* argv[]) {
    int iterations = argc > 1 ? atoi(argv[1]) : 1000;
    
    printf("Long-running test program - PID: %d\n", getpid());
    printf("Will call my_traced_function %d times with delays\n", iterations);
    printf("Run: sudo ./build/bin/lttng_ptrace_tracer %d\n", getpid());
    
    // Give tracer time to attach
    sleep(2);
    
    for (int i = 0; i < iterations; i++) {
        my_traced_function(i, 0x1234567890ABCDEF, 3.14159 * i, (void*)(0x12345678 + i));
        usleep(100000); // Sleep 100ms between calls
        
        if (i % 10 == 0) {
            printf("Completed %d/%d calls\n", i, iterations);
        }
    }
    
    printf("Test completed!\n");
    return 0;
}