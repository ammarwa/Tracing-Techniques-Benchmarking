#!/bin/bash

# LTTng Ptrace Tracing Script
# Usage: ./run_lttng_ptrace_trace.sh <num_iterations>

if [ $# -ne 1 ]; then
    echo "Usage: $0 <num_iterations>"
    exit 1
fi

NUM_ITERATIONS=$1

# Create a unique session name with timestamp
SESSION_NAME="mylib_ptrace_trace_$(date +%s)"

echo "Starting LTTng Ptrace trace session: $SESSION_NAME"
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

# Find the executables
PTRACE_TRACER=""
SAMPLE_APP=""

# Try different relative paths based on where script is run from
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ -f "$PROJECT_ROOT/build/bin/lttng_ptrace_tracer" ] && [ -f "$PROJECT_ROOT/build/bin/sample_app" ]; then
    PTRACE_TRACER="$PROJECT_ROOT/build/bin/lttng_ptrace_tracer"
    SAMPLE_APP="$PROJECT_ROOT/build/bin/sample_app"
elif [ -f "../build/bin/lttng_ptrace_tracer" ] && [ -f "../build/bin/sample_app" ]; then
    PTRACE_TRACER="../build/bin/lttng_ptrace_tracer"
    SAMPLE_APP="../build/bin/sample_app"
elif [ -f "./build/bin/lttng_ptrace_tracer" ] && [ -f "./build/bin/sample_app" ]; then
    PTRACE_TRACER="./build/bin/lttng_ptrace_tracer"
    SAMPLE_APP="./build/bin/sample_app"
else
    echo "ERROR: Could not find lttng_ptrace_tracer or sample_app"
    echo "Please build the project first with: ./build.sh"
    echo "Searched in:"
    echo "  $PROJECT_ROOT/build/bin/"
    echo "  ../build/bin/"
    echo "  ./build/bin/"
    lttng destroy
    exit 1
fi

echo "Running traced application with ptrace..."
echo "Command: $PTRACE_TRACER $SAMPLE_APP $NUM_ITERATIONS"

# Run the ptrace tracer with the sample application
$PTRACE_TRACER $SAMPLE_APP $NUM_ITERATIONS

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