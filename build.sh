#!/bin/bash

# Build script for eBPF vs LTTng comparison project
# This script wraps CMake for easier building

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

print_header() {
    echo -e "${CYAN}========================================${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}========================================${NC}"
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

# Parse arguments
BUILD_DIR="build"
BUILD_TYPE="Release"
JOBS=$(nproc)
VERBOSE=0
CLEAN=0
INSTALL=0
TEST=0

usage() {
    cat << EOF
Usage: $0 [OPTIONS]

Build the eBPF vs LTTng comparison project using CMake

Options:
    -h, --help          Show this help message
    -d, --debug         Build in Debug mode (default: Release)
    -j, --jobs N        Number of parallel jobs (default: $(nproc))
    -c, --clean         Clean build directory before building
    -i, --install       Install after building
    -t, --test          Run baseline test after building
    -v, --verbose       Verbose build output
    --build-dir DIR     Specify build directory (default: build)
    --no-lttng          Skip LTTng tracer build
    --no-ebpf           Skip eBPF tracer build

Examples:
    $0                  # Build in Release mode
    $0 -d               # Build in Debug mode
    $0 -c -j8           # Clean build with 8 jobs
    $0 -c -i -t         # Clean, build, install, and test

EOF
}

CMAKE_OPTS=""

while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -d|--debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -i|--install)
            INSTALL=1
            shift
            ;;
        -t|--test)
            TEST=1
            shift
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --no-lttng)
            CMAKE_OPTS="$CMAKE_OPTS -DBUILD_LTTNG=OFF"
            shift
            ;;
        --no-ebpf)
            CMAKE_OPTS="$CMAKE_OPTS -DBUILD_EBPF=OFF"
            shift
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

print_header "eBPF vs LTTng - CMake Build"

# Clean if requested
if [ $CLEAN -eq 1 ]; then
    echo -e "${YELLOW}Cleaning build directory: $BUILD_DIR${NC}"
    rm -rf "$BUILD_DIR"
    print_success "Build directory cleaned"
fi

# Create build directory
mkdir -p "$BUILD_DIR"

# Configure
print_header "Configuring with CMake"
echo "Build directory: $BUILD_DIR"
echo "Build type:      $BUILD_TYPE"
echo "Parallel jobs:   $JOBS"
echo ""

CMAKE_CMD="cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=$BUILD_TYPE $CMAKE_OPTS"

if [ $VERBOSE -eq 1 ]; then
    CMAKE_CMD="$CMAKE_CMD -DCMAKE_VERBOSE_MAKEFILE=ON"
fi

echo "Running: $CMAKE_CMD"
eval $CMAKE_CMD

if [ $? -ne 0 ]; then
    print_error "CMake configuration failed"
    exit 1
fi

print_success "Configuration complete"

# Build
print_header "Building"

BUILD_CMD="cmake --build $BUILD_DIR -j$JOBS"

if [ $VERBOSE -eq 1 ]; then
    BUILD_CMD="$BUILD_CMD --verbose"
fi

echo "Running: $BUILD_CMD"
eval $BUILD_CMD

if [ $? -ne 0 ]; then
    print_error "Build failed"
    exit 1
fi

print_success "Build complete"

# Install if requested
if [ $INSTALL -eq 1 ]; then
    print_header "Installing"

    if [ "$EUID" -ne 0 ]; then
        print_warning "Installation requires root privileges"
        echo "Running: sudo cmake --install $BUILD_DIR"
        sudo cmake --install "$BUILD_DIR"
    else
        cmake --install "$BUILD_DIR"
    fi

    if [ $? -eq 0 ]; then
        print_success "Installation complete"
    else
        print_error "Installation failed"
        exit 1
    fi
fi

# Test if requested
if [ $TEST -eq 1 ]; then
    print_header "Running Baseline Test"

    if [ -f "$BUILD_DIR/bin/sample_app" ]; then
        "$BUILD_DIR/bin/sample_app" 1000000
        print_success "Baseline test complete"
    else
        print_error "sample_app not found in $BUILD_DIR/bin/"
        exit 1
    fi
fi

# Build summary
print_header "Build Summary"

echo "Build artifacts:"
echo ""

if [ -f "$BUILD_DIR/lib/libmylib.so" ]; then
    print_success "libmylib.so"
else
    print_error "libmylib.so not found"
fi

if [ -f "$BUILD_DIR/bin/sample_app" ]; then
    print_success "sample_app"
else
    print_error "sample_app not found"
fi

if [ -f "$BUILD_DIR/lib/libmylib_lttng.so" ]; then
    print_success "libmylib_lttng.so"
else
    print_warning "libmylib_lttng.so not built (LTTng not available or disabled)"
fi

if [ -f "$BUILD_DIR/bin/mylib_tracer" ]; then
    print_success "mylib_tracer"
else
    print_warning "mylib_tracer not built (eBPF tools not available or disabled)"
fi

echo ""
print_header "Next Steps"

echo "Run baseline test:"
echo "  $BUILD_DIR/bin/sample_app 1000000"
echo ""

if [ -f "$BUILD_DIR/lib/libmylib_lttng.so" ]; then
    echo "Run LTTng trace:"
    echo "  cd lttng_tracer && LD_LIBRARY_PATH=../$BUILD_DIR/lib ./run_lttng_trace.sh 100000"
    echo ""
fi

if [ -f "$BUILD_DIR/bin/mylib_tracer" ]; then
    echo "Run eBPF trace:"
    echo "  cd ebpf_tracer && sudo ../$BUILD_DIR/bin/mylib_tracer output.txt &"
    echo "  LD_LIBRARY_PATH=../$BUILD_DIR/lib ../$BUILD_DIR/bin/sample_app 100000"
    echo ""
fi

echo "Run complete benchmark:"
echo "  python3 ./scripts/benchmark.py ./build"
echo ""

print_header "Build Complete!"
