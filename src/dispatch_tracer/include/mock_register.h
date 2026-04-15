/*
 * mock_register.h — Public API mimicking rocprofiler-register.
 *
 * Faithfully reproduces the real rocprofiler_register_library_api_table()
 * signature and semantics from rocprofiler-register.h:
 *
 *  - api_tables is void** — an array of POINTERS TO table structs
 *    (not a flat function-pointer array). api_tables[i] points to a
 *    struct whose first field is size_t size, followed by function
 *    pointers.
 *
 *  - api_table_length is the number of table structs being passed
 *    (typically 1 per call; HIP calls 3 times: runtime, compiler, tools).
 *
 *  - lib_version is encoded as 10000*major + 100*minor + patch, matching
 *    the real HIP_ROCP_REG_VERSION / HSA version encoding.
 *
 *  - register_id is an output giving a unique library-instance identifier,
 *    matching rocprofiler_register_library_indentifier_t.
 *
 *  On each registration, mock_register conditionally loads either:
 *  - libshim_mock.so (via MOCK_REGISTER_LIB env var — unconditional shim)
 *  - libmock_sdk.so (if rocprofiler_configure is found via dlsym)
 *
 *  The loaded library's set_api_table callback receives tables in the
 *  rocprofiler_set_api_table format:
 *    (name, lib_version, lib_instance, tables, num_tables)
 */
#ifndef MOCK_REGISTER_H
#define MOCK_REGISTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return values. */
#define MOCK_REGISTER_OK           0
#define MOCK_REGISTER_NO_TOOL      1
#define MOCK_REGISTER_ERROR       -1

/* Unique library-instance identifier (output of registration).
 * Matches rocprofiler_register_library_indentifier_t conceptually. */
typedef struct {
    uint32_t category;    /* assigned by register based on lib_name */
    uint32_t instance;    /* monotonic per name */
} mock_register_id_t;

/* Callback signature the SDK/shim exports. Matches the real
 * rocprofiler_set_api_table(name, lib_version, lib_instance, tables, num_tables).
 * tables[i] is a pointer to a table struct (first field = size_t size). */
typedef int (*mock_register_set_api_table_fn_t)(const char*  name,
                                                uint64_t     lib_version,
                                                uint64_t     lib_instance,
                                                void**       tables,
                                                uint64_t     num_tables);

/* Runtime entry point — called by each API-table owner (HIP, HSA, RCCL, ...).
 *
 * @param lib_name          String identifier ("hip", "hsa", "rccl", ...)
 * @param lib_version       Encoded as 10000*major + 100*minor + patch
 * @param api_tables        Array of pointers to table structs. Each struct
 *                          has size_t size as first field, then fn pointers.
 * @param api_table_length  Number of table structs in api_tables (typically 1)
 * @param register_id       Output: unique library instance identifier
 */
int mock_register_library_api_table(const char*        lib_name,
                                    uint32_t           lib_version,
                                    void**             api_tables,
                                    uint64_t           api_table_length,
                                    mock_register_id_t* register_id);

/* Replays all registered API tables through the SDK/shim callback. */
int mock_register_invoke_all_registrations(void);

/* Published mutable function pointer. NULL until SDK/shim sets it. */
extern mock_register_set_api_table_fn_t mock_register_set_api_table_fn;

#ifdef __cplusplus
}
#endif

#endif /* MOCK_REGISTER_H */
