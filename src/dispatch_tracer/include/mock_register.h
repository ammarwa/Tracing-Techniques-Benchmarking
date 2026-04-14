/*
 * mock_register.h - Public API mimicking rocprofiler-register.
 *
 * This mock reproduces the two relevant behaviors of rocprofiler-register:
 *
 *  1. Runtimes (HIP/HSA/... — here, our mylib_dispatch) call
 *     mock_register_library_api_table() during their constructor to publish
 *     a mutable table of function pointers.
 *
 *  2. On each registration, mock_register scans for the symbol
 *     "rocprofiler_configure" via dlsym(RTLD_DEFAULT, ...). If found it
 *     dlopens libmock_sdk.so (if not already loaded) and hands off all
 *     stored tables to the SDK. If not found, it does nothing — the
 *     runtime's table stays untouched and the hot path is zero overhead.
 */
#ifndef MOCK_REGISTER_H
#define MOCK_REGISTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return values for mock_register_library_api_table. */
#define MOCK_REGISTER_OK           0  /* Stored and (if SDK present) handed off */
#define MOCK_REGISTER_NO_TOOL      1  /* Stored only; no rocprofiler_configure visible */
#define MOCK_REGISTER_ERROR       -1  /* Capacity exceeded or bad arguments */

/* Signature the SDK exports and register-lib invokes once the SDK has been
 * dlopened. The register library holds this as a published mutable function
 * pointer: the SDK fills it in from its constructor/initialize path. */
typedef int (*mock_register_set_api_table_fn_t)(const char* name,
                                                uint32_t version,
                                                void** api_tables,
                                                uint64_t num_tables);

/* Runtime entry point — called by each API-table owner (our mylib_dispatch). */
int mock_register_library_api_table(const char* name,
                                    uint32_t version,
                                    void** api_tables,
                                    uint64_t num_tables);

/* Replays all registered API tables through the SDK's set_api_table callback.
 * Invoked by the SDK during force_configure (invoke_register_propagation). */
int mock_register_invoke_all_registrations(void);

/* Published mutable function pointer. NULL until the SDK sets it.
 * Exposed as a C global so the SDK can write to it from its init path. */
extern mock_register_set_api_table_fn_t mock_register_set_api_table_fn;

#ifdef __cplusplus
}
#endif

#endif /* MOCK_REGISTER_H */
