#!/bin/bash

# Output Validation Script
# Compares LTTng and eBPF trace outputs to ensure they capture the same data

set -e

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    print_error "This script needs to be run as root for eBPF tracing"
    print_warning "Run with: sudo ./validate_output.sh"
    exit 1
fi

# Detect build paths
if [ -d "bin" ] && [ -d "lib" ]; then
    SAMPLE_APP="./bin/sample_app"
    MYLIB_LTTNG="./lib/libmylib_lttng.so"
    MYLIB_TRACER="./bin/mylib_tracer"
elif [ -d "build/bin" ] && [ -d "build/lib" ]; then
    SAMPLE_APP="./build/bin/sample_app"
    MYLIB_LTTNG="./build/lib/libmylib_lttng.so"
    MYLIB_TRACER="./build/bin/mylib_tracer"
else
    print_error "Could not find build artifacts"
    print_error "Please build first with: ./build.sh"
    exit 1
fi

# Test parameters
NUM_ITERATIONS=1000
LTTNG_SESSION="validation_test_$$"
LTTNG_TRACE_DIR="/tmp/lttng_validation_$$"
EBPF_TRACE_FILE="/tmp/ebpf_validation_$$.txt"

print_header "eBPF vs LTTng Output Validation"
echo "Testing with $NUM_ITERATIONS iterations"
echo ""

#############################################
# Run LTTng Trace
#############################################
print_header "Running LTTng Trace"

# Clean up any existing session
lttng destroy $LTTNG_SESSION 2>/dev/null || true

# Create session
mkdir -p $LTTNG_TRACE_DIR
lttng create $LTTNG_SESSION --output=$LTTNG_TRACE_DIR >/dev/null 2>&1

# Enable events
lttng enable-event -u mylib:* >/dev/null 2>&1

# Start tracing
lttng start >/dev/null 2>&1

# Run application
echo "Running application with LTTng..."
LD_PRELOAD=$MYLIB_LTTNG $SAMPLE_APP $NUM_ITERATIONS >/dev/null 2>&1

# Stop and destroy
lttng stop >/dev/null 2>&1
lttng destroy >/dev/null 2>&1

# Convert to text format
LTTNG_TEXT="/tmp/lttng_text_$$.txt"
babeltrace2 $LTTNG_TRACE_DIR 2>/dev/null > $LTTNG_TEXT

print_success "LTTng trace captured"

# Count events
LTTNG_ENTRY_COUNT=$(grep "my_traced_function_entry" $LTTNG_TEXT | wc -l)
LTTNG_EXIT_COUNT=$(grep "my_traced_function_exit" $LTTNG_TEXT | wc -l)

echo "  Entry events: $LTTNG_ENTRY_COUNT"
echo "  Exit events:  $LTTNG_EXIT_COUNT"
echo ""

#############################################
# Run eBPF Trace
#############################################
print_header "Running eBPF Trace"

# Start eBPF tracer in background
echo "Starting eBPF tracer..."
$MYLIB_TRACER $EBPF_TRACE_FILE >/dev/null 2>&1 &
TRACER_PID=$!

# Wait for tracer to attach
sleep 2

# Check if tracer is running
if ! kill -0 $TRACER_PID 2>/dev/null; then
    print_error "eBPF tracer failed to start"
    exit 1
fi

# Run application
echo "Running application with eBPF..."
$SAMPLE_APP $NUM_ITERATIONS >/dev/null 2>&1

# Wait for events to be processed
sleep 1

# Stop tracer
kill -INT $TRACER_PID 2>/dev/null
wait $TRACER_PID 2>/dev/null || true

print_success "eBPF trace captured"

# Count events
EBPF_ENTRY_COUNT=$(grep "my_traced_function_entry" $EBPF_TRACE_FILE | wc -l)
EBPF_EXIT_COUNT=$(grep "my_traced_function_exit" $EBPF_TRACE_FILE | wc -l)

echo "  Entry events: $EBPF_ENTRY_COUNT"
echo "  Exit events:  $EBPF_EXIT_COUNT"
echo ""

#############################################
# Compare Event Counts
#############################################
print_header "Validating Event Counts"

VALIDATION_PASSED=true

# Check entry events
if [ "$LTTNG_ENTRY_COUNT" -eq "$NUM_ITERATIONS" ]; then
    print_success "LTTng captured all $NUM_ITERATIONS entry events"
else
    print_error "LTTng entry count mismatch: expected $NUM_ITERATIONS, got $LTTNG_ENTRY_COUNT"
    VALIDATION_PASSED=false
fi

if [ "$EBPF_ENTRY_COUNT" -eq "$NUM_ITERATIONS" ]; then
    print_success "eBPF captured all $NUM_ITERATIONS entry events"
else
    print_error "eBPF entry count mismatch: expected $NUM_ITERATIONS, got $EBPF_ENTRY_COUNT"
    VALIDATION_PASSED=false
fi

# Check exit events
if [ "$LTTNG_EXIT_COUNT" -eq "$NUM_ITERATIONS" ]; then
    print_success "LTTng captured all $NUM_ITERATIONS exit events"
else
    print_error "LTTng exit count mismatch: expected $NUM_ITERATIONS, got $LTTNG_EXIT_COUNT"
    VALIDATION_PASSED=false
fi

if [ "$EBPF_EXIT_COUNT" -eq "$NUM_ITERATIONS" ]; then
    print_success "eBPF captured all $NUM_ITERATIONS exit events"
else
    print_error "eBPF exit count mismatch: expected $NUM_ITERATIONS, got $EBPF_EXIT_COUNT"
    VALIDATION_PASSED=false
fi

# Check if both match
if [ "$LTTNG_ENTRY_COUNT" -eq "$EBPF_ENTRY_COUNT" ]; then
    print_success "Entry event counts match between LTTng and eBPF"
else
    print_error "Entry event count mismatch: LTTng=$LTTNG_ENTRY_COUNT, eBPF=$EBPF_ENTRY_COUNT"
    VALIDATION_PASSED=false
fi

if [ "$LTTNG_EXIT_COUNT" -eq "$EBPF_EXIT_COUNT" ]; then
    print_success "Exit event counts match between LTTng and eBPF"
else
    print_error "Exit event count mismatch: LTTng=$LTTNG_EXIT_COUNT, eBPF=$EBPF_EXIT_COUNT"
    VALIDATION_PASSED=false
fi

echo ""

#############################################
# Compare Argument Values
#############################################
print_header "Validating Argument Values"

# Extract first entry event from each
echo "Sample LTTng entry event:"
grep "my_traced_function_entry" $LTTNG_TEXT | head -1
echo ""

echo "Sample eBPF entry event:"
grep "my_traced_function_entry" $EBPF_TRACE_FILE | head -1
echo ""

# Parse arguments from first event of each
LTTNG_FIRST=$(grep "my_traced_function_entry" $LTTNG_TEXT | head -1)
EBPF_FIRST=$(grep "my_traced_function_entry" $EBPF_TRACE_FILE | head -1)

# Extract arg1 (should be 42)
LTTNG_ARG1=$(echo "$LTTNG_FIRST" | grep -oP 'arg1 = \K[0-9]+' || echo "")
EBPF_ARG1=$(echo "$EBPF_FIRST" | grep -oP 'arg1 = \K-?[0-9]+' || echo "")

# Extract arg2 (should be 0xDEADBEEF = 3735928559)
LTTNG_ARG2=$(echo "$LTTNG_FIRST" | grep -oP 'arg2 = \K[0-9]+' || echo "")
EBPF_ARG2=$(echo "$EBPF_FIRST" | grep -oP 'arg2 = \K[0-9]+' || echo "")

# Extract arg3 (should be "test_string")
LTTNG_ARG3=$(echo "$LTTNG_FIRST" | grep -oP 'arg3 = "\K[^"]+' || echo "")
EBPF_ARG3=$(echo "$EBPF_FIRST" | grep -oP 'arg3 = "\K[^"]+' || echo "")

# Validate arg1
if [ "$LTTNG_ARG1" = "42" ] && [ "$EBPF_ARG1" = "42" ]; then
    print_success "arg1 correct in both traces (42)"
else
    print_error "arg1 mismatch: LTTng=$LTTNG_ARG1, eBPF=$EBPF_ARG1, expected=42"
    VALIDATION_PASSED=false
fi

# Validate arg2
if [ "$LTTNG_ARG2" = "3735928559" ] && [ "$EBPF_ARG2" = "3735928559" ]; then
    print_success "arg2 correct in both traces (3735928559 = 0xDEADBEEF)"
else
    print_error "arg2 mismatch: LTTng=$LTTNG_ARG2, eBPF=$EBPF_ARG2, expected=3735928559"
    VALIDATION_PASSED=false
fi

# Validate arg3
if [ "$LTTNG_ARG3" = "test_string" ] && [ "$EBPF_ARG3" = "test_string" ]; then
    print_success "arg3 correct in both traces (\"test_string\")"
else
    print_error "arg3 mismatch: LTTng='$LTTNG_ARG3', eBPF='$EBPF_ARG3', expected='test_string'"
    VALIDATION_PASSED=false
fi

echo ""

#############################################
# Final Result
#############################################
print_header "Validation Summary"

if [ "$VALIDATION_PASSED" = true ]; then
    print_success "ALL VALIDATIONS PASSED!"
    echo ""
    echo "Summary:"
    echo "  ✓ Both tracers captured exactly $NUM_ITERATIONS events"
    echo "  ✓ Event counts match between LTTng and eBPF"
    echo "  ✓ Argument values are correct and match"
    echo "  ✓ Both tracing methods are functionally equivalent"
    echo ""
    RESULT=0
else
    print_error "VALIDATION FAILED"
    echo ""
    echo "Please review the errors above and check:"
    echo "  1. Build was successful"
    echo "  2. Both tracers are working correctly"
    echo "  3. Application is calling the library correctly"
    echo ""
    RESULT=1
fi

#############################################
# Cleanup
#############################################
print_header "Cleaning Up"

rm -rf $LTTNG_TRACE_DIR
rm -f $LTTNG_TEXT
rm -f $EBPF_TRACE_FILE

print_success "Temporary files cleaned up"
echo ""

print_header "Validation Complete"

exit $RESULT
