#!/bin/bash

# LTTng Tracing Script
# Usage: ./run_lttng_trace.sh <num_iterations>

if [ $# -ne 1 ]; then
    echo "Usage: $0 <num_iterations>"
    exit 1
fi

NUM_ITERATIONS=$1

# Create a unique session name with timestamp
SESSION_NAME="mylib_trace_$(date +%s)"

echo "Starting LTTng trace session: $SESSION_NAME"
echo "Iterations: $NUM_ITERATIONS"
echo "=========================================="

# Destroy any existing session with the same name
lttng destroy $SESSION_NAME 2>/dev/null || true

# Create LTTng session
lttng create $SESSION_NAME --output=./lttng_traces/$SESSION_NAME

# Enable all mylib events
lttng enable-event -u mylib:*

# Start tracing
lttng start

# Run the application with LD_PRELOAD
echo "Running traced application..."
# Try CMake build first, fall back to Makefile build
if [ -f "../build/lib/libmylib_lttng.so" ] && [ -f "../build/bin/sample_app" ]; then
    LD_PRELOAD=../build/lib/libmylib_lttng.so ../build/bin/sample_app $NUM_ITERATIONS
elif [ -f "./libmylib_lttng.so" ] && [ -f "../sample_app/sample_app" ]; then
    LD_PRELOAD=./libmylib_lttng.so ../sample_app/sample_app $NUM_ITERATIONS
else
    echo "ERROR: Could not find libmylib_lttng.so or sample_app"
    echo "Please build the project first with: ./build.sh"
    lttng destroy
    exit 1
fi

# Stop tracing
lttng stop

# Destroy session
lttng destroy

echo "=========================================="
echo "Trace saved to: ./lttng_traces/$SESSION_NAME"
echo ""
echo "To view the trace, run:"
echo "  babeltrace2 ./lttng_traces/$SESSION_NAME"
echo ""
echo "To get statistics:"
echo "  babeltrace2 ./lttng_traces/$SESSION_NAME | grep my_traced_function_entry | wc -l"
