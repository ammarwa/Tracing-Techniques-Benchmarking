# ============================================================================
# shim_mock — validation of the libroc-shim IPC transport design
#
# New architecture: libroc-shim.so is a pure IPC transport layer.
# rocprofiler-sdk (mock) does all profiling (wraps dispatch tables,
# generates buffer records). The shim manages ring buffers and delivers
# records over a socket to libroc-shim-consumer.so.
# ============================================================================

message("${Green}[dispatch_tracer/shim_mock] Building shim IPC transport mock${ColorReset}")

# ----- mock_rocp_sdk.so (mock of rocprofiler-sdk with buffer tracing) -----
add_library(mock_rocp_sdk SHARED
    mock_rocp_sdk.c
)
target_include_directories(mock_rocp_sdk
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../mylib_dispatch
)
target_compile_options(mock_rocp_sdk PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(mock_rocp_sdk PRIVATE mock_register pthread)
set_target_properties(mock_rocp_sdk PROPERTIES
    OUTPUT_NAME "mock_rocp_sdk"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
message("${Green}  ✓ libmock_rocp_sdk.so (mock SDK with buffer tracing)${ColorReset}")

# ----- libroc-shim.so (target-side IPC transport) -------------------------
add_library(shim_mock SHARED
    shim_mock.c
    shim_ipc.c
)
target_include_directories(shim_mock
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(shim_mock PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(shim_mock PRIVATE mock_rocp_sdk mock_register pthread)
set_target_properties(shim_mock PROPERTIES
    OUTPUT_NAME "roc-shim"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
message("${Green}  ✓ libroc-shim.so (IPC transport)${ColorReset}")

# ----- libroc-shim-consumer.so (consumer-side API proxy) ------------------
add_library(shim_consumer_lib SHARED
    shim_consumer_lib.c
    shim_ipc.c
)
target_include_directories(shim_consumer_lib
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(shim_consumer_lib PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(shim_consumer_lib PRIVATE pthread)
set_target_properties(shim_consumer_lib PROPERTIES
    OUTPUT_NAME "roc-shim-consumer"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
message("${Green}  ✓ libroc-shim-consumer.so (API proxy)${ColorReset}")

# ----- shim_consumer_test (OOP consumer binary) --------------------------
add_executable(shim_consumer_test
    shim_consumer_test.c
)
target_include_directories(shim_consumer_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(shim_consumer_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(shim_consumer_test PRIVATE shim_consumer_lib pthread)
set_target_properties(shim_consumer_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
message("${Green}  ✓ shim_consumer_test (OOP consumer)${ColorReset}")

# ----- mock_libA.so (HSA/ROCR analogue) ----------------------------------
add_library(mock_libA SHARED mock_libA.c)
target_include_directories(mock_libA
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(mock_libA PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(mock_libA PRIVATE mock_register)
set_target_properties(mock_libA PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# ----- mock_libB.so (HIP analogue, calls libA) ---------------------------
add_library(mock_libB SHARED mock_libB.c)
target_include_directories(mock_libB
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(mock_libB PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(mock_libB PRIVATE mock_register mock_libA)
set_target_properties(mock_libB PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# ----- shim_multilib_test (target app for OOP profiling) ------------------
add_executable(shim_multilib_test shim_multilib_test.c)
target_include_directories(shim_multilib_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
            ${CMAKE_CURRENT_SOURCE_DIR}/../mylib_dispatch
)
target_compile_options(shim_multilib_test PRIVATE -O2 -Wall -Wextra)
target_link_libraries(shim_multilib_test PRIVATE mock_libB mock_libA mock_register mylib_dispatch shim_mock)
set_target_properties(shim_multilib_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
message("${Green}  ✓ mock_libA + mock_libB + shim_multilib_test${ColorReset}")
