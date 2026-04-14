# ============================================================================
# sock — Unix-domain-socket control channel (dispatch-tracer)
#
# Components:
#   * librocp_stub_sock.so     — preloaded stub, NO rocprofiler_configure
#   * libmock_sdk_tool_sock.so — dlopen'd tool, EXPORTS rocprofiler_configure
#   * rocp_ctrl_sock           — external controller CLI
# ============================================================================

message("${Green}[dispatch_tracer] sock channel (Unix socket)${ColorReset}")

# ----- librocp_stub_sock.so ------------------------------------------------
add_library(rocp_stub_sock SHARED
    stub_sock.c
)
target_include_directories(rocp_stub_sock
    PRIVATE
        ${DISPATCH_TRACER_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(rocp_stub_sock PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(rocp_stub_sock PRIVATE pthread dl)
set_target_properties(rocp_stub_sock PROPERTIES
    OUTPUT_NAME "rocp_stub_sock"
    VERSION 1.0
    SOVERSION 1
)

# ----- libmock_sdk_tool_sock.so -------------------------------------------
add_library(mock_sdk_tool_sock SHARED
    tool_sock.c
)
target_include_directories(mock_sdk_tool_sock
    PRIVATE
        ${DISPATCH_TRACER_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(mock_sdk_tool_sock PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(mock_sdk_tool_sock PRIVATE mock_sdk mock_register pthread dl)
set_target_properties(mock_sdk_tool_sock PROPERTIES
    OUTPUT_NAME "mock_sdk_tool_sock"
    VERSION 1.0
    SOVERSION 1
    BUILD_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

set_target_properties(rocp_stub_sock PROPERTIES
    BUILD_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)

# ----- rocp_ctrl_sock (CLI controller) ------------------------------------
add_executable(rocp_ctrl_sock
    ctrl_sock.c
)
target_include_directories(rocp_ctrl_sock
    PRIVATE
        ${DISPATCH_TRACER_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(rocp_ctrl_sock PRIVATE -O2 -Wall -Wextra)

message("${Green}  ✓ librocp_stub_sock.so${ColorReset}")
message("${Green}  ✓ libmock_sdk_tool_sock.so${ColorReset}")
message("${Green}  ✓ rocp_ctrl_sock${ColorReset}")
