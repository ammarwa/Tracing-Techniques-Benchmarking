#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

// Simple stub for testing ptrace - we'll trace this function
void my_traced_function(int arg1, unsigned long arg2, double arg3, void* arg4) {
    printf("Function called: %d, %lx, %f, %p\n", arg1, arg2, arg3, arg4);
    fflush(stdout);
}

int main(int argc, char* argv[]) {
    int iterations = argc > 1 ? atoi(argv[1]) : 5;
    
    printf("Ptrace demo program - PID: %d\n", getpid());
    printf("Will call my_traced_function %d times\n", iterations);
    printf("Attach tracer with: sudo ./build/bin/lttng_ptrace_tracer %d\n", getpid());
    
    // Give tracer time to attach
    printf("Waiting 3 seconds for tracer to attach...\n");
    fflush(stdout);
    sleep(3);
    
    printf("Starting function calls...\n");
    fflush(stdout);
    
    for (int i = 0; i < iterations; i++) {
        printf("Calling function %d/%d\n", i+1, iterations);
        fflush(stdout);
        my_traced_function(i, 0x1234567890ABCDEF, 3.14159 * i, (void*)(0x12345678 + i));
        sleep(1); // Sleep 1 second between calls to make it easy to trace
    }
    
    printf("Demo completed!\n");
    return 0;
}