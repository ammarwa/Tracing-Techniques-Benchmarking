# ============================================================================
# memfd channel — abstract Unix socket + memfd_create + SCM_RIGHTS hybrid
# ============================================================================

message("${Green}[dispatch_tracer/memfd] Building stub + tool + controller${ColorReset}")

# ----- librocp_stub_memfd.so ------------------------------------------------
# Preloaded via LD_PRELOAD. Does NOT export rocprofiler_configure.
add_library(rocp_stub_memfd SHARED
    stub_memfd.c
)
target_include_directories(rocp_stub_memfd
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(rocp_stub_memfd PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(rocp_stub_memfd PRIVATE pthread dl)
set_target_properties(rocp_stub_memfd PROPERTIES
    OUTPUT_NAME "rocp_stub_memfd"
)

# ----- libmock_sdk_tool_memfd.so --------------------------------------------
# dlopen'd by the stub on CMD_CONFIGURE. Exports rocprofiler_configure.
# Links the mock SDK (and transitively mock_register).
add_library(mock_sdk_tool_memfd SHARED
    tool_memfd.c
)
target_include_directories(mock_sdk_tool_memfd
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(mock_sdk_tool_memfd PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(mock_sdk_tool_memfd PRIVATE mock_sdk dl)
set_target_properties(mock_sdk_tool_memfd PROPERTIES
    OUTPUT_NAME "mock_sdk_tool_memfd"
)

# ----- rocp_ctrl_memfd controller binary ------------------------------------
add_executable(rocp_ctrl_memfd
    ctrl_memfd.c
)
target_include_directories(rocp_ctrl_memfd
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(rocp_ctrl_memfd PRIVATE -O2 -Wall -Wextra)

message("${Green}  ✓ librocp_stub_memfd.so${ColorReset}")
message("${Green}  ✓ libmock_sdk_tool_memfd.so${ColorReset}")
message("${Green}  ✓ rocp_ctrl_memfd${ColorReset}")
