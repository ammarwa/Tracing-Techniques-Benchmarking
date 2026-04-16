/*
 * shim_mock.c — libroc-shim.so (pure IPC transport, no profiling logic).
 *
 * Loaded unconditionally by rocprofiler-register. Dormant until a
 * consumer attaches via the abstract socket. On attach, proxies the
 * standard rocprofiler_* API calls to rocprofiler-sdk (mock), manages
 * ring buffers for record transport, and delivers records to the
 * consumer over the socket.
 *
 * The shim does NOT wrap dispatch tables, generate correlation IDs,
 * or serialize arguments. rocprofiler-sdk handles all of that.
 *
 * Matches SHIM_MEMFD_SOCK_DESIGN.md §0-§12.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "shim_protocol.h"
#include "shim_ipc.h"
#include "mock_rocp_sdk.h"

/* ------------------------------------------------------------------ */
/* Per-buffer state                                                    */
/* ------------------------------------------------------------------ */

#define MAX_SHIM_BUFFERS 8

typedef struct {
    int            in_use;
    uint64_t       sdk_buffer_id;
    shim_ring_t    ring;
    int            eventfd;
    uint64_t       watermark;
    _Atomic uint64_t records_written;
    _Atomic uint64_t records_dropped;
} shim_buffer_t;

static shim_buffer_t g_shim_buffers[MAX_SHIM_BUFFERS];
static pthread_mutex_t g_buf_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Shim global state                                                   */
/* ------------------------------------------------------------------ */

static int              g_listen_sock = -1;
static _Atomic int      g_client_sock = -1;
static pthread_t        g_bg_thread;
static int              g_bg_thread_ok = 0;
static _Atomic int      g_shutdown = 0;
static uint64_t         g_start_time = 0;
static _Atomic uint32_t g_msg_id_counter = 0;
static pthread_mutex_t  g_send_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int      g_relay_sock = -1;

/* ------------------------------------------------------------------ */
/* Record sink — SDK calls this to write records into the shim ring    */
/* ------------------------------------------------------------------ */

static int shim_record_sink(uint64_t buffer_id,
                            const shim_buffer_record_t* record,
                            void* user_data)
{
    (void)user_data;
    pthread_mutex_lock(&g_buf_lock);
    shim_buffer_t* sb = NULL;
    for (int i = 0; i < MAX_SHIM_BUFFERS; i++) {
        if (g_shim_buffers[i].in_use &&
            g_shim_buffers[i].sdk_buffer_id == buffer_id) {
            sb = &g_shim_buffers[i];
            break;
        }
    }
    if (!sb) {
        pthread_mutex_unlock(&g_buf_lock);
        return -1;
    }

    int rc = shim_ring_write(&sb->ring, record, sizeof(*record));
    if (rc < 0) {
        atomic_fetch_add(&sb->records_dropped, 1);
        pthread_mutex_unlock(&g_buf_lock);
        return -1;
    }
    atomic_fetch_add(&sb->records_written, 1);
    pthread_mutex_unlock(&g_buf_lock);
    return 0;
}

/* Watermark callback — drain ring and send records over socket.
 * Called from traced application threads. Uses sendmsg with iovec
 * for atomic framing + g_send_lock to prevent interleaving with
 * the bg thread's command responses. Non-blocking: drops entire
 * batch if socket buffer is full. */
static void shim_watermark_cb(uint64_t buffer_id, void* user_data)
{
    (void)user_data;
    int sock = atomic_load_explicit(&g_client_sock, memory_order_acquire);
    if (sock < 0) return;

    pthread_mutex_lock(&g_buf_lock);
    shim_buffer_t* sb = NULL;
    for (int i = 0; i < MAX_SHIM_BUFFERS; i++) {
        if (g_shim_buffers[i].in_use &&
            g_shim_buffers[i].sdk_buffer_id == buffer_id) {
            sb = &g_shim_buffers[i];
            break;
        }
    }
    if (!sb) {
        pthread_mutex_unlock(&g_buf_lock);
        return;
    }

    shim_buffer_record_t batch[64];
    uint32_t n = 0;
    while (n < 64 && shim_ring_read(&sb->ring, &batch[n],
                                     sizeof(shim_buffer_record_t)) == 0) {
        n++;
    }
    pthread_mutex_unlock(&g_buf_lock);
    if (n == 0) return;

    shim_records_header_t rh = {
        .n_records   = n,
        .total_bytes = n * (uint32_t)sizeof(shim_buffer_record_t),
        .buffer_id   = buffer_id,
    };
    shim_msg_header_t hdr = {
        .api_id       = SHIM_API_RECORDS,
        .msg_id       = atomic_fetch_add(&g_msg_id_counter, 1),
        .payload_size = (uint32_t)(sizeof(rh) + rh.total_bytes),
        .status       = SHIM_STATUS_OK,
    };

    struct iovec iov[3] = {
        { .iov_base = &hdr, .iov_len = sizeof(hdr) },
        { .iov_base = &rh,  .iov_len = sizeof(rh) },
        { .iov_base = batch, .iov_len = rh.total_bytes },
    };
    struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 3 };

    pthread_mutex_lock(&g_send_lock);
    sendmsg(sock, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    pthread_mutex_unlock(&g_send_lock);

    if (sb->eventfd >= 0) {
        uint64_t one = 1;
        (void)write(sb->eventfd, &one, sizeof(one));
    }
}

/* ------------------------------------------------------------------ */
/* Locked send — serializes with watermark callback sends              */
/* ------------------------------------------------------------------ */

static int shim_locked_send(int sock, const shim_msg_header_t* hdr,
                            const void* payload, size_t payload_size)
{
    pthread_mutex_lock(&g_send_lock);
    int rc = shim_msg_send(sock, hdr, payload, payload_size);
    pthread_mutex_unlock(&g_send_lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Command handlers — proxy consumer API calls to SDK                  */
/* ------------------------------------------------------------------ */

static void handle_create_context(int sock, uint32_t msg_id)
{
    rocp_context_id_t ctx;
    rocp_status_t rc = rocp_create_context(&ctx);

    shim_create_context_resp_t resp = { .context_id = ctx.handle };
    shim_msg_header_t rh = {
        .api_id       = SHIM_API_CREATE_CONTEXT,
        .msg_id       = msg_id,
        .payload_size = sizeof(resp),
        .status       = (rc == ROCP_STATUS_SUCCESS) ? SHIM_STATUS_OK : SHIM_STATUS_ERROR,
    };
    shim_locked_send(sock, &rh, &resp, sizeof(resp));
}

static void handle_create_buffer(int sock, uint32_t msg_id,
                                 const shim_create_buffer_req_t* req)
{
    rocp_context_id_t dummy_ctx = { .handle = 0 };
    rocp_buffer_id_t buf;
    rocp_status_t rc = rocp_create_buffer(dummy_ctx, req->buffer_size,
                                          req->watermark, NULL, NULL, &buf);
    if (rc != ROCP_STATUS_SUCCESS) goto fail;

    pthread_mutex_lock(&g_buf_lock);
    int slot = -1;
    for (int i = 0; i < MAX_SHIM_BUFFERS; i++) {
        if (!g_shim_buffers[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_buf_lock);
        rocp_destroy_buffer(buf);
        goto fail;
    }

    shim_buffer_t* sb = &g_shim_buffers[slot];
    memset(sb, 0, sizeof(*sb));
    sb->in_use = 1;
    sb->sdk_buffer_id = buf.handle;
    sb->watermark = req->watermark;
    sb->eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    size_t ring_cap = req->buffer_size;
    if (ring_cap < 65536) ring_cap = 65536;
    if (shim_ring_create(&sb->ring, ring_cap) < 0) {
        sb->in_use = 0;
        pthread_mutex_unlock(&g_buf_lock);
        rocp_destroy_buffer(buf);
        goto fail;
    }
    pthread_mutex_unlock(&g_buf_lock);

    mock_sdk_set_record_sink(buf.handle, shim_record_sink,
                             shim_watermark_cb, NULL);

    shim_create_buffer_resp_t resp = { .buffer_id = buf.handle };
    shim_msg_header_t rh = {
        .api_id       = SHIM_API_CREATE_BUFFER,
        .msg_id       = msg_id,
        .payload_size = sizeof(resp),
        .status       = SHIM_STATUS_OK,
    };
    shim_locked_send(sock, &rh, &resp, sizeof(resp));
    return;

fail: {
        shim_create_buffer_resp_t resp = { .buffer_id = 0 };
        shim_msg_header_t rh = {
            .api_id = SHIM_API_CREATE_BUFFER, .msg_id = msg_id,
            .payload_size = sizeof(resp), .status = SHIM_STATUS_ERROR,
        };
        shim_locked_send(sock, &rh, &resp, sizeof(resp));
    }
}

static void handle_config_buf_trace(int sock, uint32_t msg_id,
                                    const shim_config_buf_trace_req_t* req)
{
    rocp_context_id_t ctx = { .handle = req->context_id };
    rocp_buffer_id_t buf  = { .handle = req->buffer_id };
    uint32_t ops[32];
    size_t n_ops = req->n_operations;
    if (n_ops > 32) n_ops = 32;
    for (size_t i = 0; i < n_ops; i++) ops[i] = req->operations[i];

    rocp_status_t rc = rocp_configure_buffer_tracing_service(
        ctx, (shim_buffer_tracing_kind_t)req->kind,
        n_ops > 0 ? ops : NULL, n_ops, buf);

    shim_msg_header_t rh = {
        .api_id = SHIM_API_CONFIG_BUF_TRACE, .msg_id = msg_id,
        .payload_size = 0,
        .status = (rc == ROCP_STATUS_SUCCESS) ? SHIM_STATUS_OK : SHIM_STATUS_ERROR,
    };
    shim_locked_send(sock, &rh, NULL, 0);
}

static void handle_start_context(int sock, uint32_t msg_id,
                                 const shim_context_req_t* req)
{
    rocp_context_id_t ctx = { .handle = req->context_id };
    rocp_status_t rc = rocp_start_context(ctx);
    shim_msg_header_t rh = {
        .api_id = SHIM_API_START_CONTEXT, .msg_id = msg_id,
        .payload_size = 0,
        .status = (rc == ROCP_STATUS_SUCCESS) ? SHIM_STATUS_OK : SHIM_STATUS_ERROR,
    };
    shim_locked_send(sock, &rh, NULL, 0);
}

static void handle_stop_context(int sock, uint32_t msg_id,
                                const shim_context_req_t* req)
{
    rocp_context_id_t ctx = { .handle = req->context_id };
    rocp_status_t rc = rocp_stop_context(ctx);
    shim_msg_header_t rh = {
        .api_id = SHIM_API_STOP_CONTEXT, .msg_id = msg_id,
        .payload_size = 0,
        .status = (rc == ROCP_STATUS_SUCCESS) ? SHIM_STATUS_OK : SHIM_STATUS_ERROR,
    };
    shim_locked_send(sock, &rh, NULL, 0);
}

static void handle_destroy_buffer(int sock, uint32_t msg_id,
                                  const shim_buffer_req_t* req)
{
    rocp_buffer_id_t buf = { .handle = req->buffer_id };
    rocp_destroy_buffer(buf);

    pthread_mutex_lock(&g_buf_lock);
    for (int i = 0; i < MAX_SHIM_BUFFERS; i++) {
        if (g_shim_buffers[i].in_use &&
            g_shim_buffers[i].sdk_buffer_id == req->buffer_id) {
            shim_ring_destroy(&g_shim_buffers[i].ring);
            if (g_shim_buffers[i].eventfd >= 0)
                close(g_shim_buffers[i].eventfd);
            g_shim_buffers[i].in_use = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_buf_lock);

    shim_msg_header_t rh = {
        .api_id = SHIM_API_DESTROY_BUFFER, .msg_id = msg_id,
        .payload_size = 0, .status = SHIM_STATUS_OK,
    };
    shim_locked_send(sock, &rh, NULL, 0);
}

static void handle_destroy_context(int sock, uint32_t msg_id,
                                   const shim_context_req_t* req)
{
    rocp_context_id_t ctx = { .handle = req->context_id };
    rocp_destroy_context(ctx);
    shim_msg_header_t rh = {
        .api_id = SHIM_API_DESTROY_CONTEXT, .msg_id = msg_id,
        .payload_size = 0, .status = SHIM_STATUS_OK,
    };
    shim_locked_send(sock, &rh, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* force_configure relay (§6)                                          */
/* ------------------------------------------------------------------ */

static int shim_proxy_tool_init(void* fini, void* tool_data)
{
    (void)fini; (void)tool_data;
    int sock = atomic_load(&g_relay_sock);
    if (sock < 0) return -1;

    shim_msg_header_t init_begin = {
        .api_id = SHIM_API_TOOL_INIT_BEGIN, .msg_id = 0,
        .payload_size = 0, .status = SHIM_STATUS_OK,
    };
    shim_locked_send(sock, &init_begin, NULL, 0);

    /* §6: mandatory timeout on tool_initialize relay. */
    const char* timeout_env = getenv("ROC_SHIM_INIT_TIMEOUT_SEC");
    int timeout_sec = timeout_env ? atoi(timeout_env) : 30;
    if (timeout_sec <= 0) timeout_sec = 30;
    time_t deadline = time(NULL) + timeout_sec;

    uint8_t payload_buf[512];
    while (time(NULL) < deadline) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) continue;

        shim_msg_header_t hdr;
        if (shim_msg_recv(sock, &hdr, payload_buf, sizeof(payload_buf)) < 0)
            return -1;

        switch (hdr.api_id) {
        case SHIM_API_TOOL_INIT_DONE: {
            shim_tool_init_done_t* done = (shim_tool_init_done_t*)payload_buf;
            return done->rc;
        }
        case SHIM_API_CREATE_CONTEXT:
            handle_create_context(sock, hdr.msg_id);
            break;
        case SHIM_API_CREATE_BUFFER:
            handle_create_buffer(sock, hdr.msg_id,
                                 (shim_create_buffer_req_t*)payload_buf);
            break;
        case SHIM_API_CONFIG_BUF_TRACE:
            handle_config_buf_trace(sock, hdr.msg_id,
                                    (shim_config_buf_trace_req_t*)payload_buf);
            break;
        case SHIM_API_START_CONTEXT:
            handle_start_context(sock, hdr.msg_id,
                                 (shim_context_req_t*)payload_buf);
            break;
        default:
            fprintf(stderr, "[shim] unknown API %u during tool_init\n", hdr.api_id);
            break;
        }
    }
    fprintf(stderr, "[shim] tool_initialize timed out after %d seconds\n", timeout_sec);
    return -1;
}

static rocp_tool_configure_result_t g_proxy_result = {
    .size       = sizeof(rocp_tool_configure_result_t),
    .initialize = shim_proxy_tool_init,
    .finalize   = NULL,
};

static rocp_tool_configure_result_t* shim_proxy_configure(
    uint32_t version, const char* runtime_version,
    uint32_t priority, void* client_id)
{
    (void)version; (void)runtime_version; (void)priority; (void)client_id;
    return &g_proxy_result;
}

static void handle_force_configure(int sock, uint32_t msg_id)
{
    atomic_store(&g_relay_sock, sock);
    rocp_status_t rc = rocp_force_configure(shim_proxy_configure);
    atomic_store(&g_relay_sock, -1);

    shim_msg_header_t rh = {
        .api_id = SHIM_API_FORCE_CONFIGURE, .msg_id = msg_id,
        .payload_size = 0,
        .status = (rc == ROCP_STATUS_SUCCESS) ? SHIM_STATUS_OK : SHIM_STATUS_ERROR,
    };
    shim_locked_send(sock, &rh, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Background thread — socket listener and command loop                */
/* ------------------------------------------------------------------ */

static void cleanup_all_buffers(void)
{
    pthread_mutex_lock(&g_buf_lock);
    for (int i = 0; i < MAX_SHIM_BUFFERS; i++) {
        if (g_shim_buffers[i].in_use) {
            shim_ring_destroy(&g_shim_buffers[i].ring);
            if (g_shim_buffers[i].eventfd >= 0)
                close(g_shim_buffers[i].eventfd);
            g_shim_buffers[i].in_use = 0;
        }
    }
    pthread_mutex_unlock(&g_buf_lock);
}

static void serve_client(int sock)
{
    /* Handshake */
    shim_handshake_t hs = {0};
    memcpy(hs.magic, SHIM_HELLO_MAGIC, 4);
    hs.protocol_version = SHIM_PROTOCOL_VERSION;
    hs.pid = (uint32_t)getpid();
    hs.start_time = g_start_time;

    shim_msg_header_t hs_hdr = {
        .api_id = SHIM_API_HANDSHAKE, .msg_id = 0,
        .payload_size = sizeof(hs), .status = SHIM_STATUS_OK,
    };
    if (shim_locked_send(sock, &hs_hdr, &hs, sizeof(hs)) < 0) return;

    /* Receive consumer handshake */
    shim_msg_header_t chdr;
    shim_handshake_t chs;
    if (shim_msg_recv(sock, &chdr, &chs, sizeof(chs)) < 0) return;
    if (chdr.api_id != SHIM_API_HANDSHAKE) return;
    if (chs.protocol_version != SHIM_PROTOCOL_VERSION) {
        fprintf(stderr, "[shim] protocol version mismatch: %u vs %u\n",
                chs.protocol_version, SHIM_PROTOCOL_VERSION);
        return;
    }

    fprintf(stderr, "[shim] consumer attached (pid=%u)\n", chs.pid);
    atomic_store(&g_client_sock, sock);

    /* Command loop */
    uint8_t payload_buf[512];
    while (!atomic_load(&g_shutdown)) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr < 0 && errno == EINTR) continue;
        if (pr > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
            fprintf(stderr, "[shim] consumer disconnected\n");
            break;
        }
        if (pr <= 0) continue;

        shim_msg_header_t hdr;
        if (shim_msg_recv(sock, &hdr, payload_buf, sizeof(payload_buf)) < 0)
            break;

        switch (hdr.api_id) {
        case SHIM_API_FORCE_CONFIGURE:
            handle_force_configure(sock, hdr.msg_id);
            break;
        case SHIM_API_CREATE_CONTEXT:
            handle_create_context(sock, hdr.msg_id);
            break;
        case SHIM_API_CREATE_BUFFER:
            handle_create_buffer(sock, hdr.msg_id,
                                 (shim_create_buffer_req_t*)payload_buf);
            break;
        case SHIM_API_CONFIG_BUF_TRACE:
            handle_config_buf_trace(sock, hdr.msg_id,
                                    (shim_config_buf_trace_req_t*)payload_buf);
            break;
        case SHIM_API_START_CONTEXT:
            handle_start_context(sock, hdr.msg_id,
                                 (shim_context_req_t*)payload_buf);
            break;
        case SHIM_API_STOP_CONTEXT:
            handle_stop_context(sock, hdr.msg_id,
                                (shim_context_req_t*)payload_buf);
            break;
        case SHIM_API_FLUSH_BUFFER: {
            shim_msg_header_t rh = {
                .api_id = SHIM_API_FLUSH_BUFFER, .msg_id = hdr.msg_id,
                .payload_size = 0, .status = SHIM_STATUS_OK,
            };
            shim_locked_send(sock, &rh, NULL, 0);
            break;
        }
        case SHIM_API_DESTROY_BUFFER:
            handle_destroy_buffer(sock, hdr.msg_id,
                                  (shim_buffer_req_t*)payload_buf);
            break;
        case SHIM_API_DESTROY_CONTEXT:
            handle_destroy_context(sock, hdr.msg_id,
                                   (shim_context_req_t*)payload_buf);
            break;
        default:
            fprintf(stderr, "[shim] unknown API %u\n", hdr.api_id);
            break;
        }
    }

    /* Cleanup */
    atomic_store(&g_client_sock, -1);
    cleanup_all_buffers();
    fprintf(stderr, "[shim] session ended, returning to dormant\n");
}

static void* shim_bg_main(void* arg)
{
    (void)arg;
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    while (!atomic_load(&g_shutdown)) {
        int client = accept(g_listen_sock, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* SO_PEERCRED auth */
        struct ucred cred;
        socklen_t clen = sizeof(cred);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0
            || cred.uid != geteuid()) {
            fprintf(stderr, "[shim] rejected connection: wrong uid\n");
            close(client);
            continue;
        }

        serve_client(client);
        close(client);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Constructor — called when mock_register dlopens us                   */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void shim_ctor(void)
{
    if (getenv("ROC_SHIM_DISABLE")) return;

    g_start_time = shim_get_start_time();

    /* Abstract socket */
    g_listen_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listen_sock < 0) { perror("[shim] socket"); return; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    int n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                     "roc-shim_%d", getpid());
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);

    if (bind(g_listen_sock, (struct sockaddr*)&addr, addrlen) < 0) {
        perror("[shim] bind"); close(g_listen_sock); g_listen_sock = -1; return;
    }
    if (listen(g_listen_sock, 1) < 0) {
        perror("[shim] listen"); close(g_listen_sock); g_listen_sock = -1; return;
    }

    /* Background thread */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_create(&g_bg_thread, &attr, shim_bg_main, NULL) == 0)
        g_bg_thread_ok = 1;
    pthread_attr_destroy(&attr);

    fprintf(stderr, "[shim] IPC ready: sock=\\0roc-shim_%d\n", getpid());
}

__attribute__((destructor))
static void shim_dtor(void)
{
    atomic_store(&g_shutdown, 1);
    if (g_listen_sock >= 0) {
        shutdown(g_listen_sock, SHUT_RDWR);
        close(g_listen_sock);
        g_listen_sock = -1;
    }
    {
        int cs = atomic_load(&g_client_sock);
        if (cs >= 0) {
            close(cs);
            atomic_store(&g_client_sock, -1);
        }
    }
    if (g_bg_thread_ok)
        pthread_join(g_bg_thread, NULL);
    cleanup_all_buffers();
}
