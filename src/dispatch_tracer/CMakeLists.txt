# ============================================================================
# Dispatch Tracer — foundational components
#
# Builds the mock rocprofiler-register + mock rocprofiler-sdk pair plus the
# dispatch-table-enabled mylib. These provide the late-load, dlopen-based
# architecture on which the four control-channel options (mmap, socket,
# memfd, signal) will be implemented.
# ============================================================================

message("${Green}[dispatch_tracer] Building mock rocprofiler-register/SDK + mylib_dispatch${ColorReset}")

set(DISPATCH_TRACER_INCLUDE_DIR
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ----- libmock_register.so ------------------------------------------------
add_library(mock_register SHARED
    mock_register/mock_register.c
)
target_include_directories(mock_register
    PUBLIC  ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(mock_register PRIVATE -O2 -Wall -Wextra -fPIC)
target_link_libraries(mock_register PRIVATE pthread dl)
set_target_properties(mock_register PROPERTIES
    OUTPUT_NAME "mock_register"
    VERSION 1.0
    SOVERSION 1
)

# ----- libmock_sdk.so -----------------------------------------------------
add_library(mock_sdk SHARED
    mock_sdk/mock_sdk.c
)
target_include_directories(mock_sdk
    PUBLIC  ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(mock_sdk PRIVATE -O2 -Wall -Wextra -fPIC)
# mock_sdk references mock_register_set_api_table_fn / invoke_all_registrations
# from libmock_register. Link it so the symbols resolve at load time.
target_link_libraries(mock_sdk PRIVATE mock_register pthread dl)
set_target_properties(mock_sdk PROPERTIES
    OUTPUT_NAME "mock_sdk"
    VERSION 1.0
    SOVERSION 1
)

# ----- libmylib_dispatch.so -----------------------------------------------
add_library(mylib_dispatch SHARED
    mylib_dispatch/mylib_dispatch.c
    mylib_dispatch/mylib_dispatch.h
)
target_include_directories(mylib_dispatch
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/mylib_dispatch
    PRIVATE ${DISPATCH_TRACER_INCLUDE_DIR}
)
target_compile_options(mylib_dispatch PRIVATE -O2 -Wall -Wextra -fPIC -fno-omit-frame-pointer)
target_link_libraries(mylib_dispatch PRIVATE mock_register dl)
set_target_properties(mylib_dispatch PROPERTIES
    OUTPUT_NAME "mylib_dispatch"
    VERSION 1.0
    SOVERSION 1
)

message("${Green}  ✓ libmock_register.so${ColorReset}")
message("${Green}  ✓ libmock_sdk.so${ColorReset}")
message("${Green}  ✓ libmylib_dispatch.so${ColorReset}")

# ----- sample_app_dispatch ------------------------------------------------
add_executable(sample_app_dispatch
    sample_app_dispatch/main.c
)
target_include_directories(sample_app_dispatch
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mylib_dispatch
            ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(sample_app_dispatch PRIVATE mylib_dispatch)
target_compile_options(sample_app_dispatch PRIVATE -O2 -Wall -Wextra)
set_target_properties(sample_app_dispatch PROPERTIES
    BUILD_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
)
message("${Green}  ✓ sample_app_dispatch${ColorReset}")

# ----- per-channel control-channel subdirectories -------------------------
option(BUILD_DISPATCH_MMAP "Build dispatch-tracer mmap channel" ON)
if(BUILD_DISPATCH_MMAP AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mmap/CMakeLists.txt)
    add_subdirectory(mmap)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sock/CMakeLists.txt)
    add_subdirectory(sock)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/memfd/CMakeLists.txt)
    add_subdirectory(memfd)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/signal/CMakeLists.txt)
    add_subdirectory(signal)
endif()

# shim_mock — validation of the rocprofiler-sdk-shim design alternative
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/shim_mock/CMakeLists.txt)
    add_subdirectory(shim_mock)
endif()
