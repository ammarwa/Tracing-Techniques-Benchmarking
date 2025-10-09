cmake_minimum_required(VERSION 3.16)
project(eBPF_vs_LTTng_Comparison C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Options
option(BUILD_LTTNG "Build LTTng tracer" ON)
option(BUILD_EBPF "Build eBPF tracer" ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Color output
if(NOT WIN32)
    string(ASCII 27 Esc)
    set(ColorReset "${Esc}[m")
    set(ColorBold  "${Esc}[1m")
    set(Red         "${Esc}[31m")
    set(Green       "${Esc}[32m")
    set(Yellow      "${Esc}[33m")
    set(Blue        "${Esc}[34m")
    set(Magenta     "${Esc}[35m")
    set(Cyan        "${Esc}[36m")
    set(White       "${Esc}[37m")
endif()

message("${Cyan}========================================${ColorReset}")
message("${Cyan}  eBPF vs LTTng Comparison Build${ColorReset}")
message("${Cyan}========================================${ColorReset}")

# ============================================================================
# 1. Sample Library
# ============================================================================
message("${Green}[1/4] Building Sample Library...${ColorReset}")

add_library(mylib SHARED
    src/sample/sample_library/mylib.c
    src/sample/sample_library/mylib.h
)

set_target_properties(mylib PROPERTIES
    PUBLIC_HEADER src/sample/sample_library/mylib.h
    VERSION 1.0
    SOVERSION 1
)

target_compile_options(mylib PRIVATE -O2 -fPIC)

message("${Green}  ✓ libmylib.so${ColorReset}")

# ============================================================================
# 2. Sample Application
# ============================================================================
message("${Green}[2/4] Building Sample Application...${ColorReset}")

add_executable(sample_app
    src/sample/sample_app/main.c
)

target_link_libraries(sample_app PRIVATE mylib)
target_include_directories(sample_app PRIVATE src/sample/sample_library)

# Set RPATH to find library
set_target_properties(sample_app PROPERTIES
    BUILD_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

message("${Green}  ✓ sample_app${ColorReset}")

# ============================================================================
# 3. LTTng Tracer (Optional)
# ============================================================================
if(BUILD_LTTNG)
    message("${Green}[3/4] Building LTTng Tracer...${ColorReset}")

    # Check for LTTng
    find_library(LTTNG_UST_LIBRARY lttng-ust)
    find_path(LTTNG_UST_INCLUDE_DIR lttng/tracepoint.h)

    if(LTTNG_UST_LIBRARY AND LTTNG_UST_INCLUDE_DIR)
        add_library(mylib_lttng SHARED
            src/tools/lttng_tracer/mylib_tp.c
            src/tools/lttng_tracer/mylib_wrapper.c
        )

        target_include_directories(mylib_lttng PRIVATE
            ${LTTNG_UST_INCLUDE_DIR}
            src/sample/sample_library
            src/tools/lttng_tracer
        )

        target_link_libraries(mylib_lttng PRIVATE
            ${LTTNG_UST_LIBRARY}
            dl
        )

        target_compile_options(mylib_lttng PRIVATE -O2 -fPIC)

        message("${Green}  ✓ libmylib_lttng.so${ColorReset}")
    else()
        message("${Yellow}  ⚠ LTTng not found. Install with: sudo apt install liblttng-ust-dev${ColorReset}")
        message("${Yellow}  ⚠ Skipping LTTng tracer build${ColorReset}")
    endif()
else()
    message("${Yellow}  ⊘ LTTng tracer build disabled${ColorReset}")
endif()

# ============================================================================
# 4. eBPF Tracer (Optional)
# ============================================================================
if(BUILD_EBPF)
    message("${Green}[4/4] Building eBPF Tracer...${ColorReset}")

    # Check for required tools
    find_program(CLANG clang)
    find_program(BPFTOOL bpftool)
    find_library(LIBBPF_LIBRARY bpf)
    find_library(LIBELF_LIBRARY elf)
    find_library(ZLIB_LIBRARY z)

    if(CLANG AND BPFTOOL AND LIBBPF_LIBRARY AND LIBELF_LIBRARY AND ZLIB_LIBRARY)
        # Detect architecture
        execute_process(
            COMMAND uname -m
            OUTPUT_VARIABLE ARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(ARCH STREQUAL "x86_64")
            set(BPF_ARCH "x86")
        elseif(ARCH STREQUAL "aarch64")
            set(BPF_ARCH "arm64")
        else()
            set(BPF_ARCH ${ARCH})
        endif()

        # BPF object file
        set(BPF_SRC ${CMAKE_SOURCE_DIR}/src/tools/ebpf_tracer/mylib_tracer.bpf.c)
        set(BPF_OBJ ${CMAKE_BINARY_DIR}/mylib_tracer.bpf.o)
        set(BPF_SKEL ${CMAKE_BINARY_DIR}/mylib_tracer.skel.h)

        # Compile BPF program
        add_custom_command(
            OUTPUT ${BPF_OBJ}
            COMMAND ${CLANG}
                -g -O2 -target bpf
                -D__TARGET_ARCH_${BPF_ARCH}
                -I/usr/include
                -I/usr/include/bpf
                -c ${BPF_SRC}
                -o ${BPF_OBJ}
            DEPENDS ${BPF_SRC}
            COMMENT "Compiling eBPF program..."
        )

        # Generate skeleton
        add_custom_command(
            OUTPUT ${BPF_SKEL}
            COMMAND ${BPFTOOL} gen skeleton ${BPF_OBJ} > ${BPF_SKEL}
            DEPENDS ${BPF_OBJ}
            COMMENT "Generating BPF skeleton..."
        )

        # Userspace program
        add_executable(mylib_tracer
            src/tools/ebpf_tracer/mylib_tracer.c
            ${BPF_SKEL}
        )

        target_include_directories(mylib_tracer PRIVATE
            ${CMAKE_BINARY_DIR}
            /usr/include/bpf
        )

        target_link_libraries(mylib_tracer PRIVATE
            ${LIBBPF_LIBRARY}
            ${LIBELF_LIBRARY}
            ${ZLIB_LIBRARY}
        )

        target_compile_options(mylib_tracer PRIVATE -O2)

        message("${Green}  ✓ mylib_tracer.bpf.o${ColorReset}")
        message("${Green}  ✓ mylib_tracer.skel.h${ColorReset}")
        message("${Green}  ✓ mylib_tracer${ColorReset}")
    else()
        set(MISSING_TOOLS "")
        if(NOT CLANG)
            list(APPEND MISSING_TOOLS "clang")
        endif()
        if(NOT BPFTOOL)
            list(APPEND MISSING_TOOLS "bpftool")
        endif()
        if(NOT LIBBPF_LIBRARY)
            list(APPEND MISSING_TOOLS "libbpf")
        endif()
        if(NOT LIBELF_LIBRARY)
            list(APPEND MISSING_TOOLS "libelf")
        endif()
        if(NOT ZLIB_LIBRARY)
            list(APPEND MISSING_TOOLS "zlib")
        endif()

        string(REPLACE ";" ", " MISSING_TOOLS_STR "${MISSING_TOOLS}")
        message("${Yellow}  ⚠ Missing dependencies: ${MISSING_TOOLS_STR}${ColorReset}")
        message("${Yellow}  ⚠ Install with: sudo apt install clang llvm libbpf-dev libelf-dev zlib1g-dev bpftool${ColorReset}")
        message("${Yellow}  ⚠ Skipping eBPF tracer build${ColorReset}")
    endif()
else()
    message("${Yellow}  ⊘ eBPF tracer build disabled${ColorReset}")
endif()

# ============================================================================
# Installation
# ============================================================================
install(TARGETS mylib
    LIBRARY DESTINATION lib
    PUBLIC_HEADER DESTINATION include
)

install(TARGETS sample_app
    RUNTIME DESTINATION bin
)

if(TARGET mylib_lttng)
    install(TARGETS mylib_lttng
        LIBRARY DESTINATION lib
    )
endif()

if(TARGET mylib_tracer)
    install(TARGETS mylib_tracer
        RUNTIME DESTINATION bin
    )
endif()

# Install scripts
install(PROGRAMS benchmark.sh
    DESTINATION bin
    RENAME ebpf_vs_lttng_benchmark
)

if(TARGET mylib_lttng)
    install(PROGRAMS src/tools/lttng_tracer/run_lttng_trace.sh
        DESTINATION bin
        RENAME run_lttng_trace
    )
endif()

if(TARGET mylib_tracer)
    install(PROGRAMS src/tools/ebpf_tracer/run_ebpf_trace.sh
        DESTINATION bin
        RENAME run_ebpf_trace
    )
endif()

# Install documentation
install(FILES
    README.md
    QUICKSTART.md
    PROJECT_SUMMARY.md
    DESTINATION share/doc/ebpf_vs_lttng
)

# ============================================================================
# Build Summary
# ============================================================================
message("${Cyan}========================================${ColorReset}")
message("${Cyan}  Build Configuration Summary${ColorReset}")
message("${Cyan}========================================${ColorReset}")
message("  Architecture:     ${ARCH}")
message("  C Compiler:       ${CMAKE_C_COMPILER}")
message("  Build Type:       ${CMAKE_BUILD_TYPE}")
message("  Install Prefix:   ${CMAKE_INSTALL_PREFIX}")
message("")
message("${Green}Components:${ColorReset}")
message("  ✓ Sample Library")
message("  ✓ Sample Application")

if(TARGET mylib_lttng)
    message("  ✓ LTTng Tracer")
else()
    message("  ${Yellow}⊘ LTTng Tracer (not built)${ColorReset}")
endif()

if(TARGET mylib_tracer)
    message("  ✓ eBPF Tracer")
else()
    message("  ${Yellow}⊘ eBPF Tracer (not built)${ColorReset}")
endif()

message("${Cyan}========================================${ColorReset}")
message("")
message("${Green}Build commands:${ColorReset}")
message("  cmake -B build")
message("  cmake --build build -j\$(nproc)")
message("")
message("${Green}Run benchmark:${ColorReset}")
message("  cd build && sudo ../benchmark.sh")
message("")
message("${Green}Install:${ColorReset}")
message("  sudo cmake --install build")
message("${Cyan}========================================${ColorReset}")

# ============================================================================
# Custom targets
# ============================================================================

# Clean target
add_custom_target(clean-all
    COMMAND ${CMAKE_BUILD_TOOL} clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/bin
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/lib
    COMMENT "Cleaning all build artifacts"
)

# Test target (runs sample app without tracing)
add_custom_target(test-baseline
    COMMAND ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/sample_app 1000000
    DEPENDS sample_app
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running baseline test (1M iterations)"
)

# Help target
add_custom_target(help-build
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Available targets:"
    COMMAND ${CMAKE_COMMAND} -E echo "  all           - Build all components"
    COMMAND ${CMAKE_COMMAND} -E echo "  mylib         - Build sample library only"
    COMMAND ${CMAKE_COMMAND} -E echo "  sample_app    - Build sample application only"
    COMMAND ${CMAKE_COMMAND} -E echo "  mylib_lttng   - Build LTTng tracer only"
    COMMAND ${CMAKE_COMMAND} -E echo "  mylib_tracer  - Build eBPF tracer only"
    COMMAND ${CMAKE_COMMAND} -E echo "  clean-all     - Clean all build artifacts"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-baseline - Run baseline test"
    COMMAND ${CMAKE_COMMAND} -E echo "  install       - Install all components"
    COMMAND ${CMAKE_COMMAND} -E echo ""
)
