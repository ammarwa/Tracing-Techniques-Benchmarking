/*
 * shim_protocol.h — IPC protocol between libroc-shim.so and
 * libroc-shim-consumer.so.
 *
 * Defines the API proxy message format, handshake, ring buffer header,
 * and buffer tracing record types. Both target-side shim and consumer
 * link against this header.
 *
 * Matches SHIM_MEMFD_SOCK_DESIGN.md §5, §6, §7, §8, §11.
 */
#ifndef SHIM_PROTOCOL_H
#define SHIM_PROTOCOL_H

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* API proxy message IDs (§5)                                          */
/* ------------------------------------------------------------------ */

enum shim_api_id {
    SHIM_API_HANDSHAKE           = 0,
    SHIM_API_FORCE_CONFIGURE     = 1,
    SHIM_API_TOOL_INIT_BEGIN     = 2,
    SHIM_API_TOOL_INIT_DONE      = 3,
    SHIM_API_CREATE_CONTEXT      = 4,
    SHIM_API_CREATE_BUFFER       = 5,
    SHIM_API_CONFIG_BUF_TRACE    = 6,
    SHIM_API_START_CONTEXT       = 7,
    SHIM_API_STOP_CONTEXT        = 8,
    SHIM_API_FLUSH_BUFFER        = 9,
    SHIM_API_DESTROY_BUFFER      = 10,
    SHIM_API_DESTROY_CONTEXT     = 11,
    SHIM_API_RECORDS             = 12,
    SHIM_API_TOOL_FINI           = 13,
};

/* ------------------------------------------------------------------ */
/* Message framing (§5)                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t api_id;
    uint32_t msg_id;
    uint32_t payload_size;
    uint32_t status;
} shim_msg_header_t;

#define SHIM_STATUS_OK      0
#define SHIM_STATUS_ERROR   1

/* ------------------------------------------------------------------ */
/* Handshake payload (§6)                                              */
/* ------------------------------------------------------------------ */

#define SHIM_PROTOCOL_VERSION  2
#define SHIM_HELLO_MAGIC       "SHIM"

typedef struct {
    char     magic[4];
    uint32_t protocol_version;
    uint32_t pid;
    uint32_t pad;
    uint64_t start_time;
} shim_handshake_t;

/* ------------------------------------------------------------------ */
/* API payloads — request/response structs for each proxied call       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t buffer_size;
    uint64_t watermark;
} shim_create_buffer_req_t;

typedef struct {
    uint64_t buffer_id;
} shim_create_buffer_resp_t;

typedef struct {
    uint64_t context_id;
} shim_create_context_resp_t;

typedef struct {
    uint64_t context_id;
    uint32_t kind;
    uint64_t buffer_id;
    uint32_t n_operations;
    uint32_t operations[32];
} shim_config_buf_trace_req_t;

typedef struct {
    uint64_t context_id;
} shim_context_req_t;

typedef struct {
    uint64_t buffer_id;
} shim_buffer_req_t;

typedef struct {
    int32_t  rc;
} shim_tool_init_done_t;

typedef struct {
    uint32_t n_records;
    uint32_t total_bytes;
    uint64_t buffer_id;
} shim_records_header_t;

/* ------------------------------------------------------------------ */
/* Buffer tracing kinds (mock subset of rocprofiler_buffer_tracing_kind)*/
/* ------------------------------------------------------------------ */

typedef enum {
    SHIM_BUF_TRACING_HIP_RUNTIME_API   = 0,
    SHIM_BUF_TRACING_HSA_CORE_API      = 1,
    SHIM_BUF_TRACING_KERNEL_DISPATCH   = 2,
    SHIM_BUF_TRACING_NUM               = 3,
} shim_buffer_tracing_kind_t;

/* ------------------------------------------------------------------ */
/* Buffer tracing record (matches SDK's buffer_tracing_*_record_t)     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t size;
    uint32_t kind;
    uint32_t operation;
    uint32_t padding;
    uint64_t correlation_id;
    uint64_t start_timestamp;
    uint64_t end_timestamp;
    uint64_t thread_id;
} shim_buffer_record_t;

/* ------------------------------------------------------------------ */
/* Ring buffer header (§7)                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    uint64_t         capacity;
    uint64_t         mask;
} shim_ring_header_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline uint64_t shim_rdtsc(void)
{
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

#endif /* SHIM_PROTOCOL_H */
