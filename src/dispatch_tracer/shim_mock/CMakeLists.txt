# ============================================================================
# shim_mock — validation of the alternative rocprofiler-sdk-shim proposal
# ============================================================================

message("${Green}[dispatch_tracer/shim_mock] Building shim-design validation library${ColorReset}")

add_library(shim_mock SHARED
    shim_mock.c
)
target_include_directories(shim_mock
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../mylib_dispatch
)
target_compile_options(shim_mock PRIVATE -O2 -Wall -Wextra -fPIC)
# shim_mock references mock_register_set_api_table_fn / invoke_all_registrations
# from libmock_register. Link it so the symbols resolve at load time, same
# pattern as libmock_sdk.
target_link_libraries(shim_mock PRIVATE mock_register pthread)
set_target_properties(shim_mock PROPERTIES
    OUTPUT_NAME "shim_mock"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    VERSION 1.0.0
    SOVERSION 1
)
message("${Green}  ✓ libshim_mock.so${ColorReset}")
