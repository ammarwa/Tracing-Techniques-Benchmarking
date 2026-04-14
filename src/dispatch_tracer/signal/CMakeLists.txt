# ============================================================================
# signal channel — real-time signal (SIGRTMIN+7) wake + mmap data channel
# ============================================================================

message("${Green}[dispatch_tracer/signal] Building signal channel components${ColorReset}")

# ----- librocp_stub_signal.so ---------------------------------------------
# LD_PRELOAD'd stub. Does NOT export rocprofiler_configure. Registers an
# async-signal-safe handler for SIGRTMIN+7 whose only job is to write one
# byte to a self-pipe; a background thread wakes from poll() and dispatches
# commands read from the mmap'd control file.
add_library(rocp_stub_signal SHARED
    stub_signal.c
)
target_include_directories(rocp_stub_signal
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(rocp_stub_signal PRIVATE
    -O2 -Wall -Wextra -fPIC -fvisibility=hidden
)
target_link_options(rocp_stub_signal PRIVATE "-Wl,-z,defs")
target_link_libraries(rocp_stub_signal PRIVATE pthread dl)
set_target_properties(rocp_stub_signal PROPERTIES
    OUTPUT_NAME "rocp_stub_signal"
    VERSION 1.0
    SOVERSION 1
)

# ----- libmock_sdk_tool_signal.so -----------------------------------------
# dlopen'd by the stub at first attach. Exports rocprofiler_configure so
# mock_sdk's force_configure discovers it via dlsym(RTLD_DEFAULT, ...).
add_library(mock_sdk_tool_signal SHARED
    tool_signal.c
)
target_include_directories(mock_sdk_tool_signal
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(mock_sdk_tool_signal PRIVATE
    -O2 -Wall -Wextra -fPIC
)
target_link_libraries(mock_sdk_tool_signal PRIVATE
    mock_sdk mock_register pthread dl
)
set_target_properties(mock_sdk_tool_signal PROPERTIES
    OUTPUT_NAME "mock_sdk_tool_signal"
    VERSION 1.0
    SOVERSION 1
)

# ----- rocp_ctrl_signal ---------------------------------------------------
add_executable(rocp_ctrl_signal
    ctrl_signal.c
)
target_include_directories(rocp_ctrl_signal
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(rocp_ctrl_signal PRIVATE
    -O2 -Wall -Wextra
)
target_link_libraries(rocp_ctrl_signal PRIVATE pthread dl rt)

message("${Green}  ✓ librocp_stub_signal.so${ColorReset}")
message("${Green}  ✓ libmock_sdk_tool_signal.so${ColorReset}")
message("${Green}  ✓ rocp_ctrl_signal${ColorReset}")
