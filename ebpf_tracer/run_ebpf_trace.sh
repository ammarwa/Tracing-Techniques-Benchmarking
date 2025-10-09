#!/bin/bash

# eBPF Tracing Script
# Usage: ./run_ebpf_trace.sh <num_iterations>

if [ $# -ne 1 ]; then
    echo "Usage: $0 <num_iterations>"
    exit 1
fi

NUM_ITERATIONS=$1
TIMESTAMP=$(date +%s)
OUTPUT_DIR="./ebpf_traces"
OUTPUT_FILE="$OUTPUT_DIR/trace_${TIMESTAMP}.txt"

# Create output directory
mkdir -p $OUTPUT_DIR

echo "Starting eBPF trace"
echo "Iterations: $NUM_ITERATIONS"
echo "Output: $OUTPUT_FILE"
echo "=========================================="

# Find the tracer and app binaries
if [ -f "../build/bin/mylib_tracer" ] && [ -f "../build/bin/sample_app" ]; then
    TRACER="../build/bin/mylib_tracer"
    SAMPLE_APP="../build/bin/sample_app"
elif [ -f "./mylib_tracer" ] && [ -f "../sample_app/sample_app" ]; then
    TRACER="./mylib_tracer"
    SAMPLE_APP="../sample_app/sample_app"
else
    echo "ERROR: Could not find mylib_tracer or sample_app"
    echo "Please build the project first with: ./build.sh"
    exit 1
fi

# Start the eBPF tracer in the background
sudo $TRACER $OUTPUT_FILE &
TRACER_PID=$!

# Give tracer time to attach
sleep 1

# Check if tracer is still running
if ! kill -0 $TRACER_PID 2>/dev/null; then
    echo "Error: Tracer failed to start"
    exit 1
fi

echo "eBPF tracer started (PID: $TRACER_PID)"
echo "Running traced application..."

# Run the application
$SAMPLE_APP $NUM_ITERATIONS

# Give time for events to be processed
sleep 1

# Stop the tracer
echo "Stopping tracer..."
sudo kill -INT $TRACER_PID
wait $TRACER_PID 2>/dev/null

echo "=========================================="
echo "Trace saved to: $OUTPUT_FILE"
echo ""
echo "To view the trace:"
echo "  cat $OUTPUT_FILE"
echo ""
echo "To get statistics:"
echo "  grep my_traced_function_entry $OUTPUT_FILE | wc -l"
