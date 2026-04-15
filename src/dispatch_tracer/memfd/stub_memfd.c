/*
 * stub_memfd.c - librocp_stub_memfd.so
 *
 * memfd channel stub library. Preloaded via LD_PRELOAD. Does NOT export
 * rocprofiler_configure — so mock_register's symbol scan finds no tool
 * and leaves libmylib_dispatch's api_table untouched (0 ns hot-path).
 *
 * At construction:
 *   - memfd_create("rocp-ctrl", MFD_CLOEXEC | MFD_ALLOW_SEALING)
 *   - ftruncate + mmap sizeof(rocp_ctrl_t)
 *   - initialize ctrl (magic, struct_version, pid, ...)
 *   - F_ADD_SEALS: F_SEAL_SHRINK | F_SEAL_GROW
 *   - create abstract socket "\0rocprofiler_<pid>", bind, listen
 *   - spawn background thread
 *
 * Background thread:
 *   - accept(); verify SO_PEERCRED.uid == geteuid()
 *   - send_fd(client, memfd) via SCM_RIGHTS
 *   - poll ctrl->version; on change, dispatch the command.
 *     CMD_CONFIGURE on first attach dlopens libmock_sdk_tool_memfd.so and
 *     calls rocprofiler_force_configure() so the tool's initialize runs.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rocp_protocol.h"
#include "mock_sdk.h"

/* Older glibc headers may be missing memfd_create / MFD_* flags. Provide
 * fallbacks that go directly through the syscall. */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC       0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW   0x0004
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL   0x0001
#endif

static int compat_memfd_create(const char* name, unsigned int flags)
{
#ifdef __NR_memfd_create
    return (int)syscall(__NR_memfd_create, name, flags);
#else
    (void)name; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static rocp_ctrl_t*             g_ctrl            = NULL;
static int                      g_memfd           = -1;
static int                      g_listen_sock     = -1;
static pthread_t                g_bg_thread;
static int                      g_bg_thread_ok    = 0;
static _Atomic int              g_shutdown_flag   = 0;

static rocp_config_t            g_pending_config;
static rocprofiler_context_id_t g_saved_ctx;

static void*                    g_tool_handle     = NULL;
static _Atomic int              g_sdk_loaded      = 0;

/* Resolved after dlopen of the tool library */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t) = NULL;
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t)       = NULL;
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t)        = NULL;

/* ------------------------------------------------------------------ */
/* Exported accessor — tool calls this after being dlopen'd            */
/* ------------------------------------------------------------------ */

static rocp_stub_state_t g_state;
static void stub_state_init_once(void)
{
    g_state.ctrl           = g_ctrl;
    g_state.pending_config = &g_pending_config;
    g_state.saved_ctx      = &g_saved_ctx;
}

__attribute__((visibility("default")))
rocp_stub_state_t* rocp_stub_get_state(void)
{
    /* Populate once under pthread_once — the underlying pointers are set
     * in the stub constructor and do not change. Prevents the read-write
     * race where two tool threads entering the accessor concurrently
     * could observe a half-written state struct. */
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, stub_state_init_once);
    return &g_state;
}

/* ------------------------------------------------------------------ */
/* SCM_RIGHTS helpers                                                  */
/* ------------------------------------------------------------------ */

static int send_fd(int sock, int fd_to_send)
{
    char buf[1] = {0};
    struct iovec iov = { .iov_base = buf, .iov_len = 1 };

    union {
        char         buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    ssize_t n;
    do { n = sendmsg(sock, &msg, MSG_NOSIGNAL); }
    while (n < 0 && errno == EINTR);
    return (int)n;
}

/* ------------------------------------------------------------------ */
/* /proc/self/stat field 22 (start_time) for tool identification       */
/* ------------------------------------------------------------------ */

static uint64_t read_start_time(void)
{
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    /* Skip pid and comm (comm can have spaces/parens); easiest is to
     * read the whole line and scan from after the last ')'. */
    char line[4096];
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    line[n] = '\0';
    char* p = strrchr(line, ')');
    if (!p) return 0;
    ++p; /* skip ')' */
    /* field after ')': state, then 19 more fields until starttime (22). */
    int field = 2;
    uint64_t value = 0;
    while (*p && field <= 22) {
        while (*p == ' ') ++p;
        if (field == 22) {
            value = strtoull(p, NULL, 10);
            break;
        }
        while (*p && *p != ' ') ++p;
        ++field;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* SDK load on first CMD_CONFIGURE                                     */
/* ------------------------------------------------------------------ */

static void load_tool_and_configure(void)
{
    /* Find and load the memfd tool library. RTLD_GLOBAL so that
     * mock_register's tool_present() dlsym(RTLD_DEFAULT, ...) lookup
     * sees rocprofiler_configure. */
    char envpath[PATH_MAX] = {0};
    const char* libdir = getenv("ROCP_DISPATCH_LIB_DIR");
    if (libdir) snprintf(envpath, sizeof(envpath), "%s/libmock_sdk_tool_memfd.so", libdir);
    char sibling[PATH_MAX] = {0};
    Dl_info dli;
    if (dladdr((void*)&load_tool_and_configure, &dli) && dli.dli_fname) {
        const char* slash = strrchr(dli.dli_fname, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - dli.dli_fname);
            if (dlen + sizeof("/libmock_sdk_tool_memfd.so") < sizeof(sibling)) {
                memcpy(sibling, dli.dli_fname, dlen);
                snprintf(sibling + dlen, sizeof(sibling) - dlen,
                         "/libmock_sdk_tool_memfd.so");
            }
        }
    }
    const char* candidates[] = {
        envpath[0] ? envpath : NULL,
        sibling[0] ? sibling : NULL,
        "libmock_sdk_tool_memfd.so",
        "./libmock_sdk_tool_memfd.so",
        NULL
    };
    for (size_t i = 0; candidates[i] && !g_tool_handle; ++i) {
        g_tool_handle = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
    }
    if (!g_tool_handle) {
        fprintf(stderr, "[stub_memfd] dlopen(libmock_sdk_tool_memfd.so) failed: %s\n",
                dlerror());
        return;
    }

    /* Resolve SDK entry points. force_configure/start/stop live in
     * libmock_sdk.so (pulled in as a dependency of the tool lib). */
    rocprofiler_configure_func_t tool_configure =
        (rocprofiler_configure_func_t)dlsym(g_tool_handle, "rocprofiler_configure");
    p_force_configure =
        (rocprofiler_status_t (*)(rocprofiler_configure_func_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_force_configure");
    p_start_context =
        (rocprofiler_status_t (*)(rocprofiler_context_id_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_start_context");
    p_stop_context =
        (rocprofiler_status_t (*)(rocprofiler_context_id_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_stop_context");

    if (!tool_configure || !p_force_configure) {
        fprintf(stderr, "[stub_memfd] missing symbols: tool_configure=%p force_configure=%p\n",
                (void*)tool_configure, (void*)p_force_configure);
        goto fail;
    }

    rocprofiler_status_t st = p_force_configure(tool_configure);
    if (st != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr, "[stub_memfd] rocprofiler_force_configure returned %d\n", (int)st);
        goto fail;
    }
    return;

fail:
    if (g_tool_handle) { dlclose(g_tool_handle); g_tool_handle = NULL; }
    p_force_configure = NULL;
    p_start_context   = NULL;
    p_stop_context    = NULL;
}

/* ------------------------------------------------------------------ */
/* Background thread: accept + SCM_RIGHTS + memfd poll                 */
/* ------------------------------------------------------------------ */

static void* control_loop(void* arg)
{
    (void)arg;

    uint32_t last_version = 0;

    while (!atomic_load(&g_shutdown_flag)) {
        /* Accept next controller (serial — one at a time). */
        int client = accept(g_listen_sock, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            /* Listen socket closed during shutdown */
            if (atomic_load(&g_shutdown_flag)) break;
            usleep(10000);
            continue;
        }

        struct ucred cred;
        socklen_t    clen = sizeof(cred);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &clen) < 0 ||
            cred.uid != geteuid()) {
            close(client);
            continue;
        }

        if (send_fd(client, g_memfd) < 0) {
            close(client);
            continue;
        }

        /* Poll memfd for commands. Check version BEFORE disconnect peek so
         * one-shot controllers (connect → write command → close) always get
         * their command processed. */
        int client_alive = 1;
        for (;;) {
            if (atomic_load(&g_shutdown_flag)) break;

            uint32_t ver = atomic_load_explicit(&g_ctrl->version, memory_order_acquire);
            if (ver == last_version) {
                if (!client_alive) break;  /* done, peer closed and no new work */
                /* Detect disconnect, but keep looping for at least one more
                 * tick so late commands from the controller are processed. */
                char peek;
                ssize_t pr = recv(client, &peek, 1, MSG_DONTWAIT | MSG_PEEK);
                if (pr == 0 || (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    client_alive = 0;
                    usleep(1000);  /* grace period for any pending write */
                    continue;
                }
                usleep(1000);
                continue;
            }

            uint32_t cmd = atomic_load_explicit(&g_ctrl->command, memory_order_acquire);
            switch (cmd) {
            case CMD_CONFIGURE: {
                memcpy(&g_pending_config, &g_ctrl->config, sizeof(g_pending_config));
                int expected = 0;
                if (atomic_compare_exchange_strong(&g_sdk_loaded, &expected, 1)) {
                    load_tool_and_configure();
                    atomic_store_explicit(&g_ctrl->context_active, 1, memory_order_release);
                }
                /* Subsequent CMD_CONFIGUREs are treated as RECONFIGURE here
                 * (tool's init is one-shot) — config copy above suffices. */
                break;
            }
            case CMD_ACTIVATE:
                if (atomic_load(&g_sdk_loaded) && p_start_context) {
                    p_start_context(g_saved_ctx);
                    atomic_store_explicit(&g_ctrl->context_active, 1, memory_order_release);
                }
                break;
            case CMD_DEACTIVATE:
                if (atomic_load(&g_sdk_loaded) && p_stop_context) {
                    p_stop_context(g_saved_ctx);
                    atomic_store_explicit(&g_ctrl->context_active, 0, memory_order_release);
                }
                break;
            case CMD_RECONFIGURE:
                memcpy(&g_pending_config, &g_ctrl->config, sizeof(g_pending_config));
                break;
            case CMD_STATUS:
            case CMD_NONE:
            default:
                break;
            }
            last_version = ver;
        }
        close(client);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Constructor / destructor                                            */
/* ------------------------------------------------------------------ */

static int setup_memfd(void)
{
    g_memfd = compat_memfd_create("rocp-ctrl", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (g_memfd < 0) {
        perror("[stub_memfd] memfd_create");
        return -1;
    }
    if (ftruncate(g_memfd, sizeof(rocp_ctrl_t)) != 0) {
        perror("[stub_memfd] ftruncate");
        close(g_memfd); g_memfd = -1;
        return -1;
    }
    g_ctrl = mmap(NULL, sizeof(rocp_ctrl_t),
                  PROT_READ | PROT_WRITE, MAP_SHARED, g_memfd, 0);
    if (g_ctrl == MAP_FAILED) {
        perror("[stub_memfd] mmap");
        close(g_memfd); g_memfd = -1;
        g_ctrl = NULL;
        return -1;
    }

    memset(g_ctrl, 0, sizeof(*g_ctrl));
    g_ctrl->magic          = ROCP_CTRL_MAGIC;
    g_ctrl->struct_version = ROCP_CTRL_VERSION;
    atomic_store(&g_ctrl->command, CMD_NONE);
    atomic_store(&g_ctrl->version, 0);
    atomic_store(&g_ctrl->context_active, 0);
    atomic_store(&g_ctrl->context_id, 0);
    atomic_store(&g_ctrl->events_traced, 0);
    atomic_store(&g_ctrl->events_dropped, 0);
    g_ctrl->pid        = (uint32_t)getpid();
    g_ctrl->start_time = read_start_time();

    /* Seal against size changes. Content remains writable. */
    /* F_SEAL_SEAL included so a malicious same-UID peer with the memfd fd
     * cannot later add F_SEAL_WRITE and lock the stub out of its own ctrl
     * region. SHRINK|GROW freeze the size; SEAL freezes the seal set itself. */
    if (fcntl(g_memfd, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        /* Non-fatal: sealing is hardening only. */
        fprintf(stderr, "[stub_memfd] F_ADD_SEALS failed (non-fatal): %s\n",
                strerror(errno));
    }
    return 0;
}

static int setup_socket(void)
{
    g_listen_sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (g_listen_sock < 0) {
        perror("[stub_memfd] socket");
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Abstract namespace: leading NUL. */
    int name_len = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                            "rocprofiler_%d", getpid());
    socklen_t slen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    if (bind(g_listen_sock, (struct sockaddr*)&addr, slen) != 0) {
        perror("[stub_memfd] bind(abstract)");
        close(g_listen_sock); g_listen_sock = -1;
        return -1;
    }
    if (listen(g_listen_sock, 5) != 0) {
        perror("[stub_memfd] listen");
        close(g_listen_sock); g_listen_sock = -1;
        return -1;
    }
    return 0;
}

__attribute__((constructor))
static void stub_memfd_init(void)
{
    if (setup_memfd() != 0) return;
    if (setup_socket() != 0) return;

    if (pthread_create(&g_bg_thread, NULL, control_loop, NULL) != 0) {
        fprintf(stderr, "[stub_memfd] pthread_create failed: %s\n", strerror(errno));
        return;
    }
    g_bg_thread_ok = 1;

    fprintf(stderr, "[stub_memfd] ready: pid=%u abstract=\"\\0rocprofiler_%d\"\n",
            (unsigned)getpid(), (int)getpid());
}

__attribute__((destructor))
static void stub_memfd_fini(void)
{
    atomic_store(&g_shutdown_flag, 1);
    if (g_listen_sock >= 0) {
        shutdown(g_listen_sock, SHUT_RDWR);
        close(g_listen_sock);
        g_listen_sock = -1;
    }
    if (g_bg_thread_ok) {
        pthread_join(g_bg_thread, NULL);
        g_bg_thread_ok = 0;
    }
    if (g_ctrl) {
        munmap(g_ctrl, sizeof(rocp_ctrl_t));
        g_ctrl = NULL;
    }
    if (g_memfd >= 0) { close(g_memfd); g_memfd = -1; }
    if (g_tool_handle) { /* Intentionally leak handle; process exiting. */ }
}
