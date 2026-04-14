# ============================================================================
# mmap regular-file control channel
# ============================================================================

message("${Green}[dispatch_tracer/mmap] Building mmap channel components${ColorReset}")

# ----- librocp_stub_mmap.so -----------------------------------------------
# LD_PRELOAD'd stub. Does NOT export rocprofiler_configure. Uses dlopen
# at runtime to resolve the SDK/tool symbols — compile with -Wl,-z,defs
# to ensure no unresolved link-time references.
add_library(rocp_stub_mmap SHARED
    stub_mmap.c
)
target_include_directories(rocp_stub_mmap
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(rocp_stub_mmap PRIVATE
    -O2 -Wall -Wextra -fPIC -fvisibility=hidden
)
target_link_options(rocp_stub_mmap PRIVATE "-Wl,-z,defs")
target_link_libraries(rocp_stub_mmap PRIVATE pthread dl)
set_target_properties(rocp_stub_mmap PROPERTIES
    OUTPUT_NAME "rocp_stub_mmap"
    VERSION 1.0
    SOVERSION 1
    BUILD_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}:\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN"
)

# ----- libmock_sdk_tool_mmap.so -------------------------------------------
# dlopen'd by the stub at first attach. Exports rocprofiler_configure so
# mock_sdk's force_configure discovers it via dlsym(RTLD_DEFAULT, ...).
add_library(mock_sdk_tool_mmap SHARED
    tool_mmap.c
)
target_include_directories(mock_sdk_tool_mmap
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(mock_sdk_tool_mmap PRIVATE
    -O2 -Wall -Wextra -fPIC
)
target_link_libraries(mock_sdk_tool_mmap PRIVATE
    mock_sdk mock_register pthread dl
)
set_target_properties(mock_sdk_tool_mmap PROPERTIES
    OUTPUT_NAME "mock_sdk_tool_mmap"
    VERSION 1.0
    SOVERSION 1
)

# ----- rocp_ctrl_mmap -----------------------------------------------------
add_executable(rocp_ctrl_mmap
    ctrl_mmap.c
)
target_include_directories(rocp_ctrl_mmap
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(rocp_ctrl_mmap PRIVATE
    -O2 -Wall -Wextra
)
target_link_libraries(rocp_ctrl_mmap PRIVATE pthread dl)

message("${Green}  ✓ librocp_stub_mmap.so${ColorReset}")
message("${Green}  ✓ libmock_sdk_tool_mmap.so${ColorReset}")
message("${Green}  ✓ rocp_ctrl_mmap${ColorReset}")
