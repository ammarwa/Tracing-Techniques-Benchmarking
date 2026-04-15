/*
 * stub_sock.c — librocp_stub_sock.so
 *
 * Preloaded via LD_PRELOAD. Does NOT export rocprofiler_configure, so
 * mock_register's symbol scan does not find a tool at startup and the
 * mock SDK is not loaded. Sets up an abstract-namespace Unix domain
 * socket and a background thread that blocks on accept(). On
 * CMD_CONFIGURE, the bg thread dlopens libmock_sdk_tool_sock.so
 * (which exports rocprofiler_configure) and drives
 * rocprofiler_force_configure().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "mock_sdk.h"
#include "rocp_protocol.h"
#include "rocp_sock_protocol.h"

/* ---------------- Internal state (in-memory only) ---------------- */

static rocp_ctrl_t               g_ctrl;                 /* NOT mmap'd */
static rocp_config_t             g_pending_config;
static rocprofiler_context_id_t  g_saved_ctx;
static _Atomic uint32_t          g_sdk_loaded     = 0;
static void*                     g_sdk_handle     = NULL;

static int                       g_listen_fd      = -1;
static pthread_t                 g_control_thread;
static _Atomic int               g_thread_started = 0;
static _Atomic int               g_shutdown       = 0;
static _Atomic int               g_atexit_armed   = 0;

/* Resolved from libmock_sdk_tool_sock.so / libmock_sdk.so after dlopen */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t) = NULL;
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t)       = NULL;
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t)        = NULL;

/* ---------------- Exported accessor (used by tool_sock) ---------------- */

static rocp_stub_state_t g_state;
static void stub_state_init_once(void)
{
    g_state.ctrl            = &g_ctrl;
    g_state.pending_config  = &g_pending_config;
    g_state.saved_ctx       = &g_saved_ctx;
}

__attribute__((visibility("default")))
rocp_stub_state_t* rocp_stub_get_state(void)
{
    /* Populate once — see mmap stub for rationale. */
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, stub_state_init_once);
    return &g_state;
}

/* ---------------- Helpers ---------------- */

static void apply_runtime_filter(const rocp_config_t* cfg)
{
    /* For the mock, "runtime filter" simply means mirroring the controller's
     * requested bits into the ctrl struct so CMD_STATUS and tool_initialize
     * can see the current filter. */
    if (!cfg) return;
    memcpy(&g_ctrl.config, cfg, sizeof(*cfg));
    memcpy(&g_pending_config, cfg, sizeof(*cfg));
}

static void resolve_sdk_symbols(void)
{
    /* Use RTLD_DEFAULT — the tool .so was dlopen'd with RTLD_GLOBAL so its
     * symbols (and transitively libmock_sdk's) are visible everywhere. */
    (void)dlerror();
    p_force_configure = (rocprofiler_status_t(*)(rocprofiler_configure_func_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_force_configure");
    p_start_context = (rocprofiler_status_t(*)(rocprofiler_context_id_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_start_context");
    p_stop_context = (rocprofiler_status_t(*)(rocprofiler_context_id_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_stop_context");
}

/* On first CMD_CONFIGURE: dlopen the tool library (which links mock_sdk)
 * and invoke force_configure. The tool's rocprofiler_configure is the
 * argument to force_configure. */
static int load_sdk_and_configure(void)
{
    char envpath[PATH_MAX] = {0};
    const char* libdir = getenv("ROCP_DISPATCH_LIB_DIR");
    if (libdir) snprintf(envpath, sizeof(envpath), "%s/libmock_sdk_tool_sock.so", libdir);
    char sibling[PATH_MAX] = {0};
    Dl_info dli;
    if (dladdr((void*)&load_sdk_and_configure, &dli) && dli.dli_fname) {
        const char* slash = strrchr(dli.dli_fname, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - dli.dli_fname);
            if (dlen + sizeof("/libmock_sdk_tool_sock.so") < sizeof(sibling)) {
                memcpy(sibling, dli.dli_fname, dlen);
                snprintf(sibling + dlen, sizeof(sibling) - dlen,
                         "/libmock_sdk_tool_sock.so");
            }
        }
    }
    const char* candidates[] = {
        envpath[0] ? envpath : NULL,
        sibling[0] ? sibling : NULL,
        "libmock_sdk_tool_sock.so",
        "./libmock_sdk_tool_sock.so",
        NULL
    };
    for (size_t i = 0; candidates[i]; ++i) {
        g_sdk_handle = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_sdk_handle) break;
    }
    if (!g_sdk_handle) {
        fprintf(stderr, "[stub_sock] dlopen(libmock_sdk_tool_sock.so) failed: %s\n",
                dlerror());
        return -1;
    }

    /* Tool's rocprofiler_configure is now visible via the tool-scope handle. */
    rocprofiler_configure_func_t tool_configure =
        (rocprofiler_configure_func_t)dlsym(g_sdk_handle, "rocprofiler_configure");
    resolve_sdk_symbols();
    if (!p_force_configure || !tool_configure) {
        fprintf(stderr, "[stub_sock] could not resolve SDK/tool symbols\n");
        goto fail;
    }

    rocprofiler_status_t st = p_force_configure(tool_configure);
    if (st != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr, "[stub_sock] rocprofiler_force_configure returned %d\n",
                (int)st);
        goto fail;
    }
    /* tool_initialize has run by now — it created a context and started it,
     * storing the handle in g_saved_ctx via the accessor. */
    return 0;

fail:
    /* Release partially-initialized state so a subsequent attempt can retry
     * cleanly (handle_configure rolls g_sdk_loaded back to 0 on failure). */
    if (g_sdk_handle) { dlclose(g_sdk_handle); g_sdk_handle = NULL; }
    p_force_configure = NULL;
    p_start_context   = NULL;
    p_stop_context    = NULL;
    return -1;
}

/* ---------------- Background thread ---------------- */

static void handle_configure(const rocp_cmd_t* cmd, rocp_response_t* resp)
{
    uint32_t expected = 0;
    if (atomic_compare_exchange_strong(&g_sdk_loaded, &expected, 1)) {
        /* First attach. Stash config for tool_initialize to read, then load. */
        memcpy(&g_pending_config, &cmd->config, sizeof(g_pending_config));
        memcpy(&g_ctrl.config,    &cmd->config, sizeof(g_ctrl.config));
        if (load_sdk_and_configure() != 0) {
            /* Roll back the flag so a later attempt can retry. */
            atomic_store(&g_sdk_loaded, 0);
            resp->status = ROCP_RESP_ERROR;
            return;
        }
    } else {
        /* SDK already loaded — runtime filter update only. */
        apply_runtime_filter(&cmd->config);
    }
    resp->status         = ROCP_RESP_OK;
    resp->context_id     = (uint32_t)g_saved_ctx.handle;
    resp->context_active = atomic_load(&g_ctrl.context_active);
    resp->events_traced  = atomic_load(&g_ctrl.events_traced);
    resp->events_dropped = atomic_load(&g_ctrl.events_dropped);
}

static void handle_activate(rocp_response_t* resp)
{
    if (atomic_load(&g_sdk_loaded) && p_start_context && g_saved_ctx.handle) {
        rocprofiler_status_t st = p_start_context(g_saved_ctx);
        if (st == ROCPROFILER_STATUS_SUCCESS) {
            atomic_store(&g_ctrl.context_active, 1);
        } else {
            resp->status = ROCP_RESP_ERROR;
        }
    } else {
        resp->status = ROCP_RESP_ERROR;
    }
    resp->context_id     = (uint32_t)g_saved_ctx.handle;
    resp->context_active = atomic_load(&g_ctrl.context_active);
    resp->events_traced  = atomic_load(&g_ctrl.events_traced);
    resp->events_dropped = atomic_load(&g_ctrl.events_dropped);
}

static void handle_deactivate(rocp_response_t* resp)
{
    if (atomic_load(&g_sdk_loaded) && p_stop_context && g_saved_ctx.handle) {
        rocprofiler_status_t st = p_stop_context(g_saved_ctx);
        if (st == ROCPROFILER_STATUS_SUCCESS) {
            atomic_store(&g_ctrl.context_active, 0);
        } else {
            resp->status = ROCP_RESP_ERROR;
        }
    } else {
        resp->status = ROCP_RESP_ERROR;
    }
    resp->context_id     = (uint32_t)g_saved_ctx.handle;
    resp->context_active = atomic_load(&g_ctrl.context_active);
    resp->events_traced  = atomic_load(&g_ctrl.events_traced);
    resp->events_dropped = atomic_load(&g_ctrl.events_dropped);
}

static void handle_status(rocp_response_t* resp)
{
    resp->status         = ROCP_RESP_OK;
    resp->context_id     = (uint32_t)g_saved_ctx.handle;
    resp->context_active = atomic_load(&g_ctrl.context_active);
    resp->events_traced  = atomic_load(&g_ctrl.events_traced);
    resp->events_dropped = atomic_load(&g_ctrl.events_dropped);
}

static void handle_reconfigure(const rocp_cmd_t* cmd, rocp_response_t* resp)
{
    apply_runtime_filter(&cmd->config);
    resp->status         = ROCP_RESP_OK;
    resp->context_id     = (uint32_t)g_saved_ctx.handle;
    resp->context_active = atomic_load(&g_ctrl.context_active);
    resp->events_traced  = atomic_load(&g_ctrl.events_traced);
    resp->events_dropped = atomic_load(&g_ctrl.events_dropped);
}

static void* control_loop(void* arg)
{
    int listen_fd = (int)(intptr_t)arg;

    /* Block signals on this thread so the main app handles them. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    while (!atomic_load(&g_shutdown)) {
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            /* EBADF / EINVAL on shutdown — exit loop. */
            break;
        }

        /* Authenticate via SO_PEERCRED. */
        struct ucred cred;
        socklen_t    clen = sizeof(cred);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 ||
            cred.uid != getuid()) {
            close(client);
            continue;
        }

        /* One command per connection (keeps life simple and matches ctrl CLI). */
        rocp_cmd_t cmd;
        ssize_t got = 0;
        size_t  total = 0;
        while (total < sizeof(cmd)) {
            got = recv(client, ((char*)&cmd) + total,
                       sizeof(cmd) - total, 0);
            if (got <= 0) break;
            total += (size_t)got;
        }
        if (total != sizeof(cmd)) { close(client); continue; }

        /* Defense-in-depth: if the controller sent an un-terminated string
         * field in cmd.config (char[256] arrays), force NUL-termination
         * before anything downstream passes them to fprintf / fopen. */
        cmd.config.output_path[sizeof(cmd.config.output_path)     - 1] = '\0';
        cmd.config.filter_pattern[sizeof(cmd.config.filter_pattern) - 1] = '\0';
        cmd.config.exclude_pattern[sizeof(cmd.config.exclude_pattern)- 1] = '\0';

        rocp_response_t resp;
        memset(&resp, 0, sizeof(resp));

        switch (cmd.type) {
        case CMD_CONFIGURE:   handle_configure(&cmd, &resp);   break;
        case CMD_ACTIVATE:    handle_activate(&resp);          break;
        case CMD_DEACTIVATE:  handle_deactivate(&resp);        break;
        case CMD_RECONFIGURE: handle_reconfigure(&cmd, &resp); break;
        case CMD_STATUS:      handle_status(&resp);            break;
        default:
            resp.status = ROCP_RESP_ERROR;
            break;
        }

        /* Best-effort send; ignore partial writes for a single struct. */
        (void)send(client, &resp, sizeof(resp), MSG_NOSIGNAL);
        close(client);
    }
    return NULL;
}

/* ---------------- Constructor / atexit ---------------- */

static void stub_atexit(void)
{
    if (atomic_exchange(&g_atexit_armed, 1)) return;
    atomic_store(&g_shutdown, 1);
    if (g_listen_fd >= 0) {
        int fd = g_listen_fd;
        g_listen_fd = -1;
        /* shutdown() first to wake accept(), then close(). */
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (atomic_load(&g_thread_started)) {
        pthread_join(g_control_thread, NULL);
    }
}

static void setup_socket_control(void)
{
    /* Initialize ctrl struct header (informational for status). */
    g_ctrl.magic           = ROCP_CTRL_MAGIC;
    g_ctrl.struct_version  = ROCP_CTRL_VERSION;
    g_ctrl.pid             = (uint32_t)getpid();
    atomic_store(&g_ctrl.command, (uint32_t)CMD_NONE);
    atomic_store(&g_ctrl.version, 0u);
    atomic_store(&g_ctrl.context_active, 0u);
    atomic_store(&g_ctrl.events_traced, 0ull);
    atomic_store(&g_ctrl.events_dropped, 0ull);

    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        fprintf(stderr, "[stub_sock] socket() failed: %s\n", strerror(errno));
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Abstract namespace: leading NUL in sun_path. */
    addr.sun_path[0] = '\0';
    int n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                     "rocprofiler_%d", getpid());
    if (n <= 0) {
        close(sock);
        return;
    }
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);

    if (bind(sock, (struct sockaddr*)&addr, addrlen) < 0) {
        fprintf(stderr, "[stub_sock] bind(\\0rocprofiler_%d) failed: %s\n",
                getpid(), strerror(errno));
        close(sock);
        return;
    }
    if (listen(sock, 5) < 0) {
        fprintf(stderr, "[stub_sock] listen() failed: %s\n", strerror(errno));
        close(sock);
        return;
    }

    g_listen_fd = sock;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* Joinable so atexit can pthread_join. */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    int rc = pthread_create(&g_control_thread, &attr,
                            control_loop, (void*)(intptr_t)sock);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "[stub_sock] pthread_create failed: %d\n", rc);
        close(sock);
        g_listen_fd = -1;
        return;
    }
    atomic_store(&g_thread_started, 1);

    atexit(stub_atexit);

    fprintf(stderr, "[stub_sock] listening on \\0rocprofiler_%d (fd=%d)\n",
            getpid(), sock);
}

__attribute__((constructor))
static void stub_ctor(void)
{
    setup_socket_control();
}
