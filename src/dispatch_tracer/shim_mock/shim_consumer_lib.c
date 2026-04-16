/*
 * shim_consumer_lib.c — libroc-shim-consumer.so
 *
 * Exports the same rocp_* API as the mock SDK, but marshalls every
 * call over a socket to libroc-shim.so in the target process. The
 * consumer links this instead of the SDK.
 *
 * Receives records from the socket and fires the user's buffer
 * callback when the user's watermark is crossed.
 *
 * Matches SHIM_MEMFD_SOCK_DESIGN.md §5, §6, §8.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "shim_protocol.h"
#include "shim_ipc.h"
#include "mock_rocp_sdk.h"

/* ------------------------------------------------------------------ */
/* Connection state                                                    */
/* ------------------------------------------------------------------ */

static int             g_sock = -1;
static pid_t           g_target_pid = 0;
static _Atomic int     g_connected = 0;
static _Atomic uint32_t g_msg_id = 1;
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Per-buffer state (consumer side)                                    */
/* ------------------------------------------------------------------ */

#define MAX_CON_BUFFERS 8

typedef struct {
    int                    in_use;
    uint64_t               buffer_id;
    rocp_buffer_callback_t callback;
    void*                  user_data;
    uint64_t               watermark;
    shim_buffer_record_t*  records;
    uint32_t               n_records;
    uint32_t               capacity;
} con_buffer_t;

static con_buffer_t g_con_buffers[MAX_CON_BUFFERS];
static pthread_mutex_t g_con_buf_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Record receiver thread                                              */
/* ------------------------------------------------------------------ */

static pthread_t   g_recv_thread;
static int         g_recv_thread_ok = 0;
static _Atomic int g_recv_stop = 0;

static void deliver_records(uint64_t buffer_id,
                            const shim_buffer_record_t* records,
                            uint32_t n_records)
{
    pthread_mutex_lock(&g_con_buf_lock);
    con_buffer_t* cb = NULL;
    for (int i = 0; i < MAX_CON_BUFFERS; i++) {
        if (g_con_buffers[i].in_use &&
            g_con_buffers[i].buffer_id == buffer_id) {
            cb = &g_con_buffers[i];
            break;
        }
    }
    if (!cb || !cb->callback) {
        pthread_mutex_unlock(&g_con_buf_lock);
        return;
    }

    for (uint32_t i = 0; i < n_records; i++) {
        if (cb->n_records >= cb->capacity) {
            uint32_t new_cap = cb->capacity ? cb->capacity * 2 : 256;
            shim_buffer_record_t* new_buf = realloc(cb->records,
                new_cap * sizeof(shim_buffer_record_t));
            if (!new_buf) break;
            cb->records  = new_buf;
            cb->capacity = new_cap;
        }
        cb->records[cb->n_records++] = records[i];

        uint64_t bytes = cb->n_records * sizeof(shim_buffer_record_t);
        if (cb->watermark > 0 && bytes >= cb->watermark) {
            rocp_context_id_t ctx = { .handle = 0 };
            rocp_buffer_id_t bid = { .handle = buffer_id };
            cb->callback(ctx, bid, cb->records, cb->n_records,
                         cb->user_data, 0);
            cb->n_records = 0;
        }
    }
    pthread_mutex_unlock(&g_con_buf_lock);
}

/* The recv thread and proxy_call both read from the same socket.
 * To prevent interleaving, the recv thread uses trylock on g_send_lock:
 * if proxy_call holds it (doing a synchronous request/response), the
 * recv thread backs off. proxy_call holds g_send_lock for the full
 * send+recv cycle, so it has exclusive socket access. */
static void* recv_thread_main(void* arg)
{
    (void)arg;
    while (!atomic_load(&g_recv_stop)) {
        struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;
        if (pfd.revents & (POLLHUP | POLLERR)) break;
        if (!(pfd.revents & POLLIN)) continue;

        if (pthread_mutex_trylock(&g_send_lock) != 0) continue;

        uint8_t buf[16384];
        shim_msg_header_t hdr;
        if (shim_msg_recv(g_sock, &hdr, buf, sizeof(buf)) < 0) {
            pthread_mutex_unlock(&g_send_lock);
            break;
        }
        pthread_mutex_unlock(&g_send_lock);

        if (hdr.api_id == SHIM_API_RECORDS) {
            shim_records_header_t* rh = (shim_records_header_t*)buf;
            uint32_t expected = rh->n_records * (uint32_t)sizeof(shim_buffer_record_t);
            if (rh->total_bytes != expected) continue;
            if (sizeof(*rh) + rh->total_bytes > sizeof(buf)) continue;

            shim_buffer_record_t* records =
                (shim_buffer_record_t*)(buf + sizeof(*rh));
            deliver_records(rh->buffer_id, records, rh->n_records);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Connection management                                               */
/* ------------------------------------------------------------------ */

int shim_consumer_connect(pid_t target_pid)
{
    g_target_pid = target_pid;
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    int n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                     "roc-shim_%d", (int)target_pid);
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);

    for (int attempt = 0; attempt < 40; attempt++) {
        if (connect(g_sock, (struct sockaddr*)&addr, addrlen) == 0)
            goto connected;
        if (errno != ECONNREFUSED && errno != ENOENT) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "[consumer] connect failed: %s\n", strerror(errno));
    close(g_sock); g_sock = -1;
    return -1;

connected:;
    /* Receive server handshake */
    shim_msg_header_t shdr;
    shim_handshake_t shs;
    if (shim_msg_recv(g_sock, &shdr, &shs, sizeof(shs)) < 0) {
        fprintf(stderr, "[consumer] handshake recv failed\n");
        close(g_sock); g_sock = -1; return -1;
    }
    if (memcmp(shs.magic, SHIM_HELLO_MAGIC, 4) != 0) {
        fprintf(stderr, "[consumer] bad magic\n");
        close(g_sock); g_sock = -1; return -1;
    }
    if (shs.protocol_version != SHIM_PROTOCOL_VERSION) {
        fprintf(stderr, "[consumer] protocol version mismatch\n");
        close(g_sock); g_sock = -1; return -1;
    }

    /* Send our handshake */
    shim_handshake_t chs = {0};
    memcpy(chs.magic, SHIM_HELLO_MAGIC, 4);
    chs.protocol_version = SHIM_PROTOCOL_VERSION;
    chs.pid = (uint32_t)getpid();
    chs.start_time = shim_get_start_time();

    shim_msg_header_t chdr = {
        .api_id = SHIM_API_HANDSHAKE, .msg_id = 0,
        .payload_size = sizeof(chs), .status = SHIM_STATUS_OK,
    };
    if (shim_msg_send(g_sock, &chdr, &chs, sizeof(chs)) < 0) {
        close(g_sock); g_sock = -1; return -1;
    }

    atomic_store(&g_connected, 1);
    fprintf(stderr, "[consumer] connected to target pid=%u\n", shs.pid);
    return 0;
}

void shim_consumer_disconnect(void)
{
    atomic_store(&g_recv_stop, 1);
    if (g_recv_thread_ok) {
        pthread_join(g_recv_thread, NULL);
        g_recv_thread_ok = 0;
    }
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    atomic_store(&g_connected, 0);

    pthread_mutex_lock(&g_con_buf_lock);
    for (int i = 0; i < MAX_CON_BUFFERS; i++) {
        if (g_con_buffers[i].in_use) {
            free(g_con_buffers[i].records);
            g_con_buffers[i].in_use = 0;
        }
    }
    pthread_mutex_unlock(&g_con_buf_lock);
}

/* ------------------------------------------------------------------ */
/* API proxy — each rocp_* call goes over the socket                   */
/* ------------------------------------------------------------------ */

static int proxy_call(uint32_t api_id, const void* req_payload,
                      uint32_t req_size, void* resp_payload,
                      uint32_t max_resp)
{
    pthread_mutex_lock(&g_send_lock);
    uint32_t mid = atomic_fetch_add(&g_msg_id, 1);

    shim_msg_header_t req = {
        .api_id = api_id, .msg_id = mid,
        .payload_size = req_size, .status = SHIM_STATUS_OK,
    };
    if (shim_msg_send(g_sock, &req, req_payload, req_size) < 0) {
        pthread_mutex_unlock(&g_send_lock);
        return -1;
    }

    shim_msg_header_t resp;
    if (shim_msg_recv(g_sock, &resp, resp_payload, max_resp) < 0) {
        pthread_mutex_unlock(&g_send_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_send_lock);

    return (resp.status == SHIM_STATUS_OK) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* tool_initialize relay context                                       */
/* ------------------------------------------------------------------ */

static rocp_configure_func_t g_user_configure = NULL;

rocp_status_t rocp_force_configure(rocp_configure_func_t configure_func)
{
    if (!atomic_load(&g_connected) || !configure_func)
        return ROCP_STATUS_ERROR;

    g_user_configure = configure_func;

    /* Send FORCE_CONFIGURE to shim */
    shim_msg_header_t req = {
        .api_id = SHIM_API_FORCE_CONFIGURE,
        .msg_id = atomic_fetch_add(&g_msg_id, 1),
        .payload_size = 0, .status = SHIM_STATUS_OK,
    };
    pthread_mutex_lock(&g_send_lock);
    shim_msg_send(g_sock, &req, NULL, 0);

    /* Receive TOOL_INIT_BEGIN */
    shim_msg_header_t init_hdr;
    shim_msg_recv(g_sock, &init_hdr, NULL, 0);
    pthread_mutex_unlock(&g_send_lock);

    if (init_hdr.api_id != SHIM_API_TOOL_INIT_BEGIN)
        return ROCP_STATUS_ERROR;

    /* Call user's tool_configure to get result */
    rocp_tool_configure_result_t* result =
        configure_func(0, "mock-sdk-2", 128, NULL);

    int init_rc = -1;
    if (result && result->initialize)
        init_rc = result->initialize((void*)result->finalize, NULL);

    /* Send TOOL_INIT_DONE */
    shim_tool_init_done_t done = { .rc = init_rc };
    shim_msg_header_t done_hdr = {
        .api_id = SHIM_API_TOOL_INIT_DONE,
        .msg_id = atomic_fetch_add(&g_msg_id, 1),
        .payload_size = sizeof(done), .status = SHIM_STATUS_OK,
    };
    pthread_mutex_lock(&g_send_lock);
    shim_msg_send(g_sock, &done_hdr, &done, sizeof(done));

    /* Receive FORCE_CONFIGURE response */
    shim_msg_header_t fc_resp;
    shim_msg_recv(g_sock, &fc_resp, NULL, 0);
    pthread_mutex_unlock(&g_send_lock);

    if (fc_resp.status != SHIM_STATUS_OK) return ROCP_STATUS_ERROR;

    /* Start record receiver thread */
    atomic_store(&g_recv_stop, 0);
    if (pthread_create(&g_recv_thread, NULL, recv_thread_main, NULL) == 0)
        g_recv_thread_ok = 1;

    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_create_context(rocp_context_id_t* ctx)
{
    if (!ctx) return ROCP_STATUS_ERROR;
    shim_create_context_resp_t resp;
    if (proxy_call(SHIM_API_CREATE_CONTEXT, NULL, 0, &resp, sizeof(resp)) < 0)
        return ROCP_STATUS_ERROR;
    ctx->handle = resp.context_id;
    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_create_buffer(rocp_context_id_t ctx,
                                 uint64_t size, uint64_t watermark,
                                 rocp_buffer_callback_t callback,
                                 void* user_data,
                                 rocp_buffer_id_t* buffer)
{
    if (!buffer) return ROCP_STATUS_ERROR;
    (void)ctx;

    shim_create_buffer_req_t req = {
        .buffer_size = size, .watermark = watermark,
    };
    shim_create_buffer_resp_t resp;
    if (proxy_call(SHIM_API_CREATE_BUFFER, &req, sizeof(req),
                   &resp, sizeof(resp)) < 0)
        return ROCP_STATUS_ERROR;

    buffer->handle = resp.buffer_id;

    pthread_mutex_lock(&g_con_buf_lock);
    for (int i = 0; i < MAX_CON_BUFFERS; i++) {
        if (!g_con_buffers[i].in_use) {
            memset(&g_con_buffers[i], 0, sizeof(con_buffer_t));
            g_con_buffers[i].in_use    = 1;
            g_con_buffers[i].buffer_id = resp.buffer_id;
            g_con_buffers[i].callback  = callback;
            g_con_buffers[i].user_data = user_data;
            g_con_buffers[i].watermark = watermark;
            break;
        }
    }
    pthread_mutex_unlock(&g_con_buf_lock);

    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_configure_buffer_tracing_service(
    rocp_context_id_t ctx, shim_buffer_tracing_kind_t kind,
    uint32_t* operations, size_t op_count, rocp_buffer_id_t buffer)
{
    shim_config_buf_trace_req_t req = {
        .context_id = ctx.handle,
        .kind = kind,
        .buffer_id = buffer.handle,
        .n_operations = (uint32_t)op_count,
    };
    for (size_t i = 0; i < op_count && i < 32; i++)
        req.operations[i] = operations[i];

    return proxy_call(SHIM_API_CONFIG_BUF_TRACE, &req, sizeof(req),
                      NULL, 0) < 0 ? ROCP_STATUS_ERROR : ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_start_context(rocp_context_id_t ctx)
{
    shim_context_req_t req = { .context_id = ctx.handle };
    return proxy_call(SHIM_API_START_CONTEXT, &req, sizeof(req),
                      NULL, 0) < 0 ? ROCP_STATUS_ERROR : ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_stop_context(rocp_context_id_t ctx)
{
    shim_context_req_t req = { .context_id = ctx.handle };
    return proxy_call(SHIM_API_STOP_CONTEXT, &req, sizeof(req),
                      NULL, 0) < 0 ? ROCP_STATUS_ERROR : ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_flush_buffer(rocp_buffer_id_t buffer)
{
    pthread_mutex_lock(&g_con_buf_lock);
    for (int i = 0; i < MAX_CON_BUFFERS; i++) {
        con_buffer_t* cb = &g_con_buffers[i];
        if (cb->in_use && cb->buffer_id == buffer.handle &&
            cb->callback && cb->n_records > 0) {
            rocp_context_id_t ctx = { .handle = 0 };
            cb->callback(ctx, buffer, cb->records, cb->n_records,
                         cb->user_data, 0);
            cb->n_records = 0;
        }
    }
    pthread_mutex_unlock(&g_con_buf_lock);
    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_destroy_buffer(rocp_buffer_id_t buffer)
{
    shim_buffer_req_t req = { .buffer_id = buffer.handle };
    proxy_call(SHIM_API_DESTROY_BUFFER, &req, sizeof(req), NULL, 0);

    pthread_mutex_lock(&g_con_buf_lock);
    for (int i = 0; i < MAX_CON_BUFFERS; i++) {
        if (g_con_buffers[i].in_use &&
            g_con_buffers[i].buffer_id == buffer.handle) {
            free(g_con_buffers[i].records);
            g_con_buffers[i].in_use = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_con_buf_lock);
    return ROCP_STATUS_SUCCESS;
}

rocp_status_t rocp_destroy_context(rocp_context_id_t ctx)
{
    shim_context_req_t req = { .context_id = ctx.handle };
    return proxy_call(SHIM_API_DESTROY_CONTEXT, &req, sizeof(req),
                      NULL, 0) < 0 ? ROCP_STATUS_ERROR : ROCP_STATUS_SUCCESS;
}
