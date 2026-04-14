/*
 * rocp_protocol.h - Canonical control protocol shared between dispatch-tracer
 * stub libraries, tool libraries, and external controllers.
 *
 * See docs/dispatch-tracer/CONTROL_CHANNEL_SURVEY.md for the definitive
 * specification. All four IPC options (mmap, socket, memfd, signal) share
 * these definitions.
 */
#ifndef ROCP_PROTOCOL_H
#define ROCP_PROTOCOL_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROCP_CTRL_MAGIC   0xD15EA7C0u
#define ROCP_CTRL_VERSION 1u

/* Commands (controller -> tool) */
enum rocp_ctrl_command {
    CMD_NONE        = 0,  /* No pending command */
    CMD_CONFIGURE   = 1,  /* First attach: dlopen SDK + force_configure */
    CMD_ACTIVATE    = 2,  /* rocprofiler_start_context() */
    CMD_DEACTIVATE  = 3,  /* rocprofiler_stop_context() */
    CMD_RECONFIGURE = 4,  /* Update tool-side runtime filter (no SDK calls) */
    CMD_STATUS      = 5,  /* Query current state */
};

/* Output formats */
enum rocp_output_format {
    ROCP_OUTPUT_TEXT     = 0,
    ROCP_OUTPUT_JSON     = 1,
    ROCP_OUTPUT_OTLP     = 2,
    ROCP_OUTPUT_PERFETTO = 3,
};

/* Configuration payload carried on CMD_CONFIGURE / CMD_RECONFIGURE */
typedef struct {
    uint32_t enable_hip             : 1;
    uint32_t enable_hsa             : 1;
    uint32_t enable_rccl            : 1;
    uint32_t enable_ompt            : 1;
    uint32_t enable_rocdecode       : 1;
    uint32_t enable_rocjpeg         : 1;
    uint32_t enable_kernel_dispatch : 1;
    uint32_t reserved               : 25;

    uint32_t output_format;   /* rocp_output_format */
    uint32_t buffer_size_kb;
    char     output_path[256];
    char     filter_pattern[256];
    char     exclude_pattern[256];
} rocp_config_t;

/* Shared control structure. Lives in the backing IPC medium
 * (mmap'd file, memfd, shared memory region). */
typedef struct {
    /* Identification */
    uint32_t magic;           /* Must equal ROCP_CTRL_MAGIC */
    uint32_t struct_version;  /* ROCP_CTRL_VERSION */

    /* Command channel (controller -> tool) */
    _Atomic uint32_t command; /* rocp_ctrl_command */
    _Atomic uint32_t version; /* Bumped by controller on every command */

    /* Configuration (controller writes, tool reads on CMD_CONFIGURE/RECONFIGURE) */
    rocp_config_t config;

    /* Status (tool writes, controller reads) */
    _Atomic uint32_t context_active;
    _Atomic uint64_t context_id;    /* rocprofiler_context_id_t.handle */
    _Atomic uint64_t events_traced;
    _Atomic uint64_t events_dropped;

    /* Tool identification */
    uint32_t pid;
    uint64_t start_time;     /* /proc/<pid>/stat field 22 */
} __attribute__((aligned(64))) rocp_ctrl_t;

/* Forward declaration — the SDK headers define this as a concrete type.
 * Here we only need enough to hold the handle. Kept in sync with
 * mock_sdk.h's rocprofiler_context_id_t. */
#ifndef ROCPROFILER_CONTEXT_ID_T_DEFINED
#define ROCPROFILER_CONTEXT_ID_T_DEFINED
typedef struct { uint64_t handle; } rocprofiler_context_id_t;
#endif

/* Stub<->Tool state-sharing contract.
 *
 * The stub exports an accessor function returning a pointer to a state
 * struct so the tool library (dlopen'd at attach time) can discover the
 * shared control block, pending configuration, and saved context handle
 * without cross-DSO extern globals.
 */
typedef struct {
    rocp_ctrl_t*              ctrl;
    rocp_config_t*            pending_config;
    rocprofiler_context_id_t* saved_ctx;
} rocp_stub_state_t;

typedef rocp_stub_state_t* (*rocp_stub_get_state_fn_t)(void);

#ifdef __cplusplus
}
#endif

#endif /* ROCP_PROTOCOL_H */
