# Build System Documentation

## Overview

The project uses **CMake** as the build system, wrapped by a convenient `build.sh` script for common workflows. The build system compiles the sample library, sample application, and both LTTng and eBPF tracers.

## Quick Start

### Basic Build

```bash
./build.sh -c
```

This will:
1. Clean the build directory
2. Configure with CMake (Release mode)
3. Build all components
4. Show build summary

### Build Output

```
build/
├── bin/
│   ├── sample_app           # Sample application
│   └── mylib_tracer        # eBPF tracer
└── lib/
    ├── libmylib.so*        # Sample library (symlinks)
    ├── libmylib.so.1*
    ├── libmylib.so.1.0
    ├── libmylib_lttng.so*  # LTTng wrapper (symlinks)
    ├── libmylib_lttng.so.1*
    └── libmylib_lttng.so.1.0
```

## Build Script Options

### Command-Line Flags

```bash
./build.sh [OPTIONS]

Options:
  -h, --help          Show help message
  -d, --debug         Build in Debug mode (default: Release)
  -j, --jobs N        Number of parallel jobs (default: nproc)
  -c, --clean         Clean build directory before building
  -i, --install       Install after building (requires sudo)
  -t, --test          Run baseline test after building
  -v, --verbose       Verbose build output
  --build-dir DIR     Specify build directory (default: build)
  --no-lttng          Skip LTTng tracer build
  --no-ebpf           Skip eBPF tracer build
```

### Common Use Cases

**Clean build** (recommended):
```bash
./build.sh -c
```

**Debug build** (for development):
```bash
./build.sh -d -c
```

**Fast parallel build** (8 jobs):
```bash
./build.sh -c -j8
```

**Build and test**:
```bash
./build.sh -c -t
```

**Build only LTTng** (skip eBPF):
```bash
./build.sh -c --no-ebpf
```

**Build only eBPF** (skip LTTng):
```bash
./build.sh -c --no-lttng
```

**Verbose output** (see all commands):
```bash
./build.sh -c -v
```

## Dependencies

### Required

- **CMake** >= 3.10
- **GCC** or **Clang** (C compiler)
- **GNU Make** or **Ninja**

### Optional (for LTTng)

- **LTTng UST** development files
  ```bash
  sudo apt-get install lttng-tools liblttng-ust-dev babeltrace2
  ```

### Optional (for eBPF)

- **Clang** (for BPF compilation)
- **LLVM** (for BPF tools)
- **libbpf** development files
- **bpftool** (BPF skeleton generation)
- **Kernel headers**

  ```bash
  sudo apt-get install clang llvm libbpf-dev linux-headers-$(uname -r)

  # bpftool (if not in package repos)
  sudo apt-get install bpftool || {
      # Build from kernel source
      git clone --depth=1 https://github.com/torvalds/linux.git
      cd linux/tools/bpf/bpftool
      make && sudo make install
  }
  ```

## CMake Build Process

### Manual CMake Build

If you prefer using CMake directly:

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)

# Install (optional)
sudo cmake --install build
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Release` | Build type (`Debug`, `Release`, `RelWithDebInfo`) |
| `BUILD_LTTNG` | `ON` | Build LTTng tracer (auto-disabled if deps missing) |
| `BUILD_EBPF` | `ON` | Build eBPF tracer (auto-disabled if deps missing) |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | Install prefix |

**Examples**:

```bash
# Debug build with verbose output
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_VERBOSE_MAKEFILE=ON
cmake --build build --verbose

# Disable LTTng tracer
cmake -B build -DBUILD_LTTNG=OFF
cmake --build build

# Custom install prefix
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/ebpf_vs_lttng
cmake --build build
sudo cmake --install build
```

## Build Targets

The CMake project defines the following targets:

### Libraries

1. **`mylib`**: Sample shared library
   - Source: `sample_library/mylib.c`
   - Output: `libmylib.so.1.0` (versioned)
   - Features: Configurable work duration, minimal overhead

2. **`mylib_lttng`**: LTTng wrapper library (if `BUILD_LTTNG=ON`)
   - Source: `lttng_tracer/mylib_wrapper.c`, `lttng_tracer/mylib_tp.c`
   - Output: `libmylib_lttng.so.1.0` (versioned)
   - Depends on: `liblttng-ust`

### Executables

3. **`sample_app`**: Sample application
   - Source: `sample_app/main.c`
   - Links to: `libmylib.so`
   - RPATH: Set to `../lib` for easy execution

4. **`mylib_tracer`**: eBPF tracer (if `BUILD_EBPF=ON`)
   - Source: `ebpf_tracer/mylib_tracer.c`, `ebpf_tracer/mylib_tracer.bpf.c`
   - Depends on: `libbpf`, BPF skeleton generation
   - Requires: Clang, bpftool

### Custom Targets

5. **`clean_all`**: Remove build directory
   ```bash
   cmake --build build --target clean_all
   # Equivalent to: rm -rf build
   ```

## Build Process Details

### 1. Sample Library Build

```bash
# Compile
gcc -fPIC -c -o mylib.o sample_library/mylib.c

# Link with versioning
gcc -shared -Wl,-soname,libmylib.so.1 -o libmylib.so.1.0 mylib.o

# Create symlinks
ln -sf libmylib.so.1.0 libmylib.so.1
ln -sf libmylib.so.1 libmylib.so
```

### 2. LTTng Tracer Build

```bash
# Compile tracepoint definitions
gcc -I. -fPIC -c -o mylib_tp.o lttng_tracer/mylib_tp.c

# Compile wrapper
gcc -I. -fPIC -c -o mylib_wrapper.o lttng_tracer/mylib_wrapper.c

# Link wrapper library
gcc -shared -Wl,-soname,libmylib_lttng.so.1 -o libmylib_lttng.so.1.0 \
    mylib_wrapper.o mylib_tp.o -llttng-ust -ldl

# Create symlinks
ln -sf libmylib_lttng.so.1.0 libmylib_lttng.so.1
ln -sf libmylib_lttng.so.1 libmylib_lttng.so
```

### 3. eBPF Tracer Build

```bash
# Compile BPF program to object file
clang -g -O2 -target bpf -D__TARGET_ARCH_x86_64 \
      -c ebpf_tracer/mylib_tracer.bpf.c -o mylib_tracer.bpf.o

# Generate BPF skeleton
bpftool gen skeleton mylib_tracer.bpf.o > mylib_tracer.skel.h

# Compile userspace loader
gcc -g -O2 ebpf_tracer/mylib_tracer.c -o mylib_tracer \
    -lbpf -lelf -lz

# Set capabilities (optional, instead of always requiring sudo)
sudo setcap cap_sys_admin,cap_bpf=ep mylib_tracer
```

### 4. Sample Application Build

```bash
# Compile
gcc -c -o main.o sample_app/main.c

# Link with rpath
gcc -o sample_app main.o -L./lib -lmylib -Wl,-rpath,\$ORIGIN/../lib
```

## Troubleshooting

### CMake Configuration Failed

**Problem**: CMake cannot find dependencies

```
CMake Error: Could not find lttng-ust
```

**Solution**: Install missing dependencies

```bash
# Ubuntu/Debian
sudo apt-get install liblttng-ust-dev

# Fedora/RHEL
sudo dnf install lttng-ust-devel

# Or skip that component
./build.sh -c --no-lttng
```

### BPF Compilation Failed

**Problem**: Clang cannot compile BPF

```
error: unknown target 'bpf'
```

**Solution**: Install/upgrade Clang

```bash
# Need Clang 10+
sudo apt-get install clang-14 llvm-14
export CC=clang-14
./build.sh -c
```

### bpftool Not Found

**Problem**: BPF skeleton generation fails

```
bpftool: command not found
```

**Solution**: Install bpftool

```bash
# Option 1: From package
sudo apt-get install linux-tools-generic linux-tools-$(uname -r)

# Option 2: Build from source
git clone --depth=1 https://github.com/libbpf/bpftool.git
cd bpftool/src
make
sudo make install
```

### Library Not Found at Runtime

**Problem**: `sample_app` cannot find `libmylib.so`

```
./build/bin/sample_app: error while loading shared libraries: libmylib.so.1: cannot open shared object file
```

**Solution**: RPATH is embedded, but if it still fails:

```bash
# Option 1: Set LD_LIBRARY_PATH
export LD_LIBRARY_PATH=$PWD/build/lib:$LD_LIBRARY_PATH
./build/bin/sample_app 1000

# Option 2: Install to system
sudo cp build/lib/libmylib.so* /usr/local/lib/
sudo ldconfig

# Option 3: Check RPATH
readelf -d build/bin/sample_app | grep RPATH
# Should show: (RPATH) Library rpath: [$ORIGIN/../lib]
```

### Parallel Build Errors

**Problem**: Random build failures with `-j` flag

```
make[2]: *** No rule to make target 'mylib_tracer.skel.h'
```

**Solution**: Reduce parallelism or use serial build

```bash
# Serial build (safer, slower)
./build.sh -c -j1

# Moderate parallelism
./build.sh -c -j4
```

## Development Workflow

### Iterative Development

```bash
# 1. Edit code
vim sample_library/mylib.c

# 2. Rebuild (incremental)
cmake --build build

# 3. Test
./build/bin/sample_app 1000

# 4. Clean rebuild if needed
./build.sh -c
```

### Debug Build

```bash
# Build with debug symbols and no optimization
./build.sh -d -c

# Run with GDB
gdb --args ./build/bin/sample_app 1000000

# Run with Valgrind
valgrind ./build/bin/sample_app 1000000
```

### Cross-Compilation

For cross-compiling (e.g., ARM):

```bash
# Create toolchain file
cat > toolchain-arm.cmake << 'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
EOF

# Configure with toolchain
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=toolchain-arm.cmake

# Build
cmake --build build-arm
```

## Installation

### System-Wide Installation

```bash
./build.sh -c -i

# Installs to /usr/local by default:
# /usr/local/lib/libmylib.so*
# /usr/local/lib/libmylib_lttng.so*
# /usr/local/bin/sample_app
# /usr/local/bin/mylib_tracer
```

### Custom Install Prefix

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/myapp
cmake --build build
sudo cmake --install build

# Installed to /opt/myapp/{bin,lib}
```

### Uninstall

CMake doesn't have built-in uninstall, but you can:

```bash
# Option 1: Remove manually
sudo rm /usr/local/lib/libmylib*
sudo rm /usr/local/lib/libmylib_lttng*
sudo rm /usr/local/bin/sample_app
sudo rm /usr/local/bin/mylib_tracer
sudo ldconfig

# Option 2: Use install manifest
cat build/install_manifest.txt | sudo xargs rm
```

## Summary

The build system provides:

✅ **Easy builds**: `./build.sh -c` for most use cases
✅ **Flexible options**: Debug/Release, parallel jobs, selective builds
✅ **Auto-detection**: Skips components if dependencies missing
✅ **Proper versioning**: Shared libraries with SO versioning
✅ **RPATH support**: Applications find libraries without LD_LIBRARY_PATH
✅ **Development friendly**: Incremental builds, debug mode, verbose output

**Quick reference**:
```bash
# Standard build
./build.sh -c

# Debug build
./build.sh -d -c

# Build and test
./build.sh -c -t

# Skip eBPF (no root needed)
./build.sh -c --no-ebpf
```
