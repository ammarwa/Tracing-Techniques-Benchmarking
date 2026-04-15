# ============================================================================
# shim_mock — validation of the rocprofiler-sdk-shim design
# ============================================================================

message("${Green}[dispatch_tracer/shim_mock] Building shim-design validation${ColorReset}")

# ----- libshim_mock.so (target-side shim library) -------------------------
add_library(shim_mock SHARED
    shim_mock.c
    shim_ipc.c
)
target_include_directories(shim_mock
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../mylib_dispatch
)
target_compile_options(shim_mock PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(shim_mock PRIVATE mock_register pthread)
set_target_properties(shim_mock PROPERTIES
    OUTPUT_NAME "shim_mock"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    VERSION 1.0.0
    SOVERSION 1
)
message("${Green}  ✓ libshim_mock.so${ColorReset}")

# ----- shim_consumer_test (external OOP consumer binary) ------------------
add_executable(shim_consumer_test
    shim_consumer_test.c
    shim_ipc.c
)
target_include_directories(shim_consumer_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_options(shim_consumer_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(shim_consumer_test PRIVATE pthread)
set_target_properties(shim_consumer_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
message("${Green}  ✓ shim_consumer_test${ColorReset}")
