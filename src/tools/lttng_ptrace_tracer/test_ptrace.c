// Simple test program for ptrace tracer
#include <stdio.h>
#include <unistd.h>
#include "mylib_tp.h"

int main() {
    printf("Test program starting - PID: %d\n", getpid());
    printf("Waiting for ptrace tracer to attach...\n");
    sleep(2);  // Give time to attach
    
    printf("Firing test tracepoint manually...\n");
    tracepoint(mylib, my_traced_function_entry, 123, 456ULL, 3.14159, (void*)0xABCD);
    tracepoint(mylib, my_traced_function_exit);
    
    printf("Test completed\n");
    return 0;
}