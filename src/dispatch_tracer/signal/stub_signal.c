/*
 * stub_signal.c - librocp_stub_signal.so
 *
 * LD_PRELOAD stub identical in spirit to the mmap channel's stub (mmap'd control
 * file + background thread), but woken instantly by SIGRTMIN+7 via
 * sigqueue() from the controller instead of polling a version counter
 * on a 1 ms timer.
 *
 * The signal handler itself is strictly async-signal-safe: it verifies
 * info->si_uid against a cached uid and writes exactly one byte to a
 * non-blocking self-pipe. All SDK work (dlopen, force_configure,
 * start/stop_context, runtime filter updates) happens in the background
 * thread, which wakes from poll() on the pipe read-end.
 *
 * This stub does NOT export rocprofiler_configure — mock_register's
 * symbol scan will not discover a tool, so the SDK is not loaded until
 * the controller issues CMD_CONFIGURE.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "rocp_protocol.h"
#include "mock_sdk.h"

#ifndef ROCP_SIGNAL_NUM
#define ROCP_SIGNAL_NUM (SIGRTMIN + 7)
#endif

/* ----------------------------------------------------------------- */
/* Shared state exposed to the tool via rocp_stub_get_state().        */
/* ----------------------------------------------------------------- */
static rocp_ctrl_t*              g_ctrl            = NULL;
static rocp_config_t             g_pending_config;
static rocprofiler_context_id_t  g_saved_ctx       = {0};
static rocp_stub_state_t         g_state;

/* Filter (updated on CMD_RECONFIGURE, read by tool callbacks) */
#define EXPORT __attribute__((visibility("default")))
EXPORT _Atomic uint32_t  g_runtime_domain_mask = 0xFFFFFFFFu;
EXPORT _Atomic uint32_t  g_runtime_output_fmt  = 0;
EXPORT char              g_runtime_output_path[256];
pthread_mutex_t   g_runtime_filter_lock = PTHREAD_MUTEX_INITIALIZER;

/* ----------------------------------------------------------------- */
/* Background-thread / lifecycle bookkeeping                          */
/* ----------------------------------------------------------------- */
static pthread_t        g_bg_thread;
static int              g_bg_thread_started = 0;
static _Atomic int      g_shutdown_flag     = 0;
static _Atomic bool     g_sdk_loaded        = false;
static _Atomic int      g_finalize_status   = 0;
static void*            g_sdk_handle        = NULL;
static uid_t            g_cached_uid;
static pid_t            g_pid;
static char             g_ctrl_path[PATH_MAX];
static char             g_ctrl_dir[PATH_MAX];

/* Self-pipe: signal handler writes, bg thread reads. */
static int              g_wakeup_pipe[2] = { -1, -1 };

/* Function pointers resolved after dlopen of the tool library */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t) = NULL;
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t)       = NULL;
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t)        = NULL;

/* ----------------------------------------------------------------- */
/* Helpers                                                            */
/* ----------------------------------------------------------------- */

static uint64_t read_proc_start_time(void)
{
    int fd = open("/proc/self/stat", O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char* rparen = strrchr(buf, ')');
    if (!rparen) return 0;
    char* p = rparen + 2;
    for (int i = 0; i < 19; ++i) {
        char* sp = strchr(p, ' ');
        if (!sp) return 0;
        p = sp + 1;
    }
    return (uint64_t)strtoull(p, NULL, 10);
}

static int mkdir_p(const char* path, mode_t mode)
{
    if (mkdir(path, mode) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void apply_runtime_filter(const rocp_config_t* cfg)
{
    uint32_t mask = 0;
    if (cfg->enable_hip)  mask |= (1u << ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API);
    if (cfg->enable_hsa)  mask |= (1u << ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API);
    if (cfg->enable_rccl) mask |= (1u << ROCPROFILER_CALLBACK_TRACING_RCCL_API);
    if (cfg->enable_ompt) mask |= (1u << ROCPROFILER_CALLBACK_TRACING_OMPT);
    if (mask == 0) mask = 0xFFFFFFFFu;
    atomic_store_explicit(&g_runtime_domain_mask, mask, memory_order_release);
    atomic_store_explicit(&g_runtime_output_fmt, cfg->output_format, memory_order_release);

    pthread_mutex_lock(&g_runtime_filter_lock);
    snprintf(g_runtime_output_path, sizeof(g_runtime_output_path),
             "%s", cfg->output_path);
    pthread_mutex_unlock(&g_runtime_filter_lock);
}

/* ----------------------------------------------------------------- */
/* Exported accessor for the tool library                             */
/* ----------------------------------------------------------------- */
__attribute__((visibility("default")))
rocp_stub_state_t* rocp_stub_get_state(void)
{
    g_state.ctrl           = g_ctrl;
    g_state.pending_config = &g_pending_config;
    g_state.saved_ctx      = &g_saved_ctx;
    return &g_state;
}

/* ----------------------------------------------------------------- */
/* Signal handler (async-signal-safe)                                 */
/*                                                                    */
/* Only allowed work: cache-uid check and a single write() to the     */
/* non-blocking wakeup pipe. write() on a pipe is async-signal-safe    */
/* per POSIX. If the pipe is full (unlikely — 64 KiB default) the      */
/* byte is silently dropped; the bg thread's poll() timeout recovers.  */
/* ----------------------------------------------------------------- */
static void sig_handler(int sig, siginfo_t* info, void* ucontext)
{
    (void)sig;
    (void)ucontext;
    if (!info) return;
    if (info->si_uid != g_cached_uid) return;
    if (g_wakeup_pipe[1] < 0) return;
    char c = 'W';
    ssize_t r = write(g_wakeup_pipe[1], &c, 1);
    (void)r;  /* EAGAIN is fine — poll() timeout recovers */
}

/* ----------------------------------------------------------------- */
/* Load SDK tool + force_configure (first attach)                     */
/* ----------------------------------------------------------------- */
static void load_sdk_and_configure(void)
{
    const char* env_override = getenv("ROCP_TOOL_SIGNAL_PATH");
    char envpath[PATH_MAX] = {0};
    const char* libdir = getenv("ROCP_DISPATCH_LIB_DIR");
    if (libdir) snprintf(envpath, sizeof(envpath), "%s/libmock_sdk_tool_signal.so", libdir);
    char sibling[PATH_MAX] = {0};
    Dl_info dli;
    if (dladdr((void*)&load_sdk_and_configure, &dli) && dli.dli_fname) {
        const char* slash = strrchr(dli.dli_fname, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - dli.dli_fname);
            if (dlen + sizeof("/libmock_sdk_tool_signal.so") < sizeof(sibling)) {
                memcpy(sibling, dli.dli_fname, dlen);
                snprintf(sibling + dlen, sizeof(sibling) - dlen,
                         "/libmock_sdk_tool_signal.so");
            }
        }
    }
    const char* candidates[] = {
        env_override,
        envpath[0] ? envpath : NULL,
        sibling[0] ? sibling : NULL,
        "libmock_sdk_tool_signal.so",
        "./libmock_sdk_tool_signal.so",
    };
    /* Iterate by index — any NULL/empty entry is skipped, not terminating. */
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]) && !g_sdk_handle; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        g_sdk_handle = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (!g_sdk_handle) {
            fprintf(stderr, "[rocp_stub_signal] dlopen('%s'): %s\n",
                    candidates[i], dlerror());
        }
    }
    if (!g_sdk_handle) {
        fprintf(stderr, "[rocp_stub_signal] failed to dlopen tool: %s\n",
                dlerror());
        return;
    }

    rocprofiler_configure_func_t tool_configure =
        (rocprofiler_configure_func_t)dlsym(g_sdk_handle, "rocprofiler_configure");
    p_force_configure =
        (rocprofiler_status_t(*)(rocprofiler_configure_func_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_force_configure");
    p_start_context =
        (rocprofiler_status_t(*)(rocprofiler_context_id_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_start_context");
    p_stop_context =
        (rocprofiler_status_t(*)(rocprofiler_context_id_t))
        dlsym(RTLD_DEFAULT, "rocprofiler_stop_context");

    if (!tool_configure || !p_force_configure) {
        fprintf(stderr, "[rocp_stub_signal] could not resolve tool/SDK symbols\n");
        return;
    }

    rocprofiler_status_t st = p_force_configure(tool_configure);
    if (st != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr, "[rocp_stub_signal] rocprofiler_force_configure failed: %d\n",
                (int)st);
        return;
    }

    atomic_store_explicit(&g_ctrl->context_active, 1u, memory_order_release);
}

/* ----------------------------------------------------------------- */
/* Background signal-woken loop                                       */
/* ----------------------------------------------------------------- */
static void* control_poll_loop(void* arg)
{
    (void)arg;

    /* Block the RT signal on this thread — we want the main thread (or
     * any other arbitrary thread) to receive the signal; this thread
     * only wakes from poll() on the pipe read end. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, ROCP_SIGNAL_NUM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);

    struct pollfd pfd = { .fd = g_wakeup_pipe[0], .events = POLLIN };
    uint32_t last_version = 0;

    while (!atomic_load_explicit(&g_shutdown_flag, memory_order_acquire)) {
        /* 30s timeout as fallback for any lost wakeup bytes. */
        int pr = poll(&pfd, 1, 30000);
        if (pr < 0 && errno == EINTR) continue;

        /* Drain the pipe regardless of outcome. */
        char buf[64];
        while (read(g_wakeup_pipe[0], buf, sizeof(buf)) > 0) {}

        if (atomic_load_explicit(&g_shutdown_flag, memory_order_acquire)) break;
        if (!g_ctrl) continue;

        uint32_t ver = atomic_load_explicit(&g_ctrl->version, memory_order_acquire);
        if (ver == last_version) continue;

        uint32_t cmd = atomic_load_explicit(&g_ctrl->command, memory_order_acquire);
        switch (cmd) {
        case CMD_CONFIGURE: {
            memcpy(&g_pending_config, &g_ctrl->config, sizeof(g_pending_config));
            apply_runtime_filter(&g_pending_config);
            bool expected = false;
            if (atomic_compare_exchange_strong_explicit(&g_sdk_loaded, &expected, true,
                                            memory_order_acq_rel,
                                            memory_order_acquire)) {
                load_sdk_and_configure();
            } else {
                apply_runtime_filter(&g_ctrl->config);
            }
            break;
        }
        case CMD_ACTIVATE:
            if (atomic_load_explicit(&g_sdk_loaded, memory_order_acquire) && p_start_context) {
                p_start_context(g_saved_ctx);
                atomic_store_explicit(&g_ctrl->context_active, 1u, memory_order_release);
            }
            break;
        case CMD_DEACTIVATE:
            if (atomic_load_explicit(&g_sdk_loaded, memory_order_acquire) && p_stop_context) {
                p_stop_context(g_saved_ctx);
                atomic_store_explicit(&g_ctrl->context_active, 0u, memory_order_release);
            }
            break;
        case CMD_RECONFIGURE:
            apply_runtime_filter(&g_ctrl->config);
            break;
        case CMD_STATUS:
        case CMD_NONE:
        default:
            break;
        }
        last_version = ver;
    }
    return NULL;
}

/* ----------------------------------------------------------------- */
/* atexit cleanup                                                     */
/* ----------------------------------------------------------------- */
static void stub_atexit(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_finalize_status, &expected, -1,
                                     memory_order_seq_cst,
                                     memory_order_seq_cst)) {
        return;
    }

    if (g_ctrl &&
        atomic_load_explicit(&g_ctrl->context_active, memory_order_acquire) &&
        p_stop_context) {
        p_stop_context(g_saved_ctx);
        atomic_store_explicit(&g_ctrl->context_active, 0u, memory_order_release);
    }

    /* Ask the bg thread to exit; wake it via the pipe so poll() returns
     * immediately instead of waiting up to 30s. */
    atomic_store_explicit(&g_shutdown_flag, 1, memory_order_release);
    if (g_wakeup_pipe[1] >= 0) {
        char c = 'X';
        ssize_t r = write(g_wakeup_pipe[1], &c, 1);
        (void)r;
    }
    if (g_bg_thread_started) {
        pthread_join(g_bg_thread, NULL);
        g_bg_thread_started = 0;
    }

    if (g_wakeup_pipe[0] >= 0) { close(g_wakeup_pipe[0]); g_wakeup_pipe[0] = -1; }
    if (g_wakeup_pipe[1] >= 0) { close(g_wakeup_pipe[1]); g_wakeup_pipe[1] = -1; }

    if (g_ctrl) {
        munmap(g_ctrl, sizeof(rocp_ctrl_t));
        g_ctrl = NULL;
    }
    if (g_ctrl_path[0]) unlink(g_ctrl_path);
    if (g_ctrl_dir[0])  rmdir(g_ctrl_dir);

    atomic_store_explicit(&g_finalize_status, 1, memory_order_seq_cst);
}

/* ----------------------------------------------------------------- */
/* Constructor — runs at LD_PRELOAD time                              */
/* ----------------------------------------------------------------- */
__attribute__((constructor))
static void stub_init(void)
{
    g_cached_uid = getuid();
    g_pid        = getpid();

    /* --- 1) wakeup pipe ------------------------------------------- */
    if (pipe2(g_wakeup_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        fprintf(stderr, "[rocp_stub_signal] pipe2 failed: %s\n", strerror(errno));
        return;
    }

    /* --- 2) signal handler registered BEFORE the bg thread --------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sig_handler;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(ROCP_SIGNAL_NUM, &sa, NULL) != 0) {
        fprintf(stderr, "[rocp_stub_signal] sigaction(SIGRTMIN+7): %s\n",
                strerror(errno));
        return;
    }

    /* --- 3) mmap control file ------------------------------------- */
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "/run/user/%u", (unsigned)g_cached_uid);
    struct stat st;
    if (stat(base, &st) != 0) {
        fprintf(stderr, "[rocp_stub_signal] %s missing; needs pam_systemd\n", base);
        return;
    }

    char dir1[PATH_MAX];
    snprintf(dir1, sizeof(dir1), "/run/user/%u/rocprofiler", (unsigned)g_cached_uid);
    if (mkdir_p(dir1, 0700) != 0) {
        fprintf(stderr, "[rocp_stub_signal] mkdir %s: %s\n",
                dir1, strerror(errno));
        return;
    }

    snprintf(g_ctrl_dir, sizeof(g_ctrl_dir),
             "/run/user/%u/rocprofiler/%d", (unsigned)g_cached_uid, (int)g_pid);
    if (mkdir_p(g_ctrl_dir, 0700) != 0) {
        fprintf(stderr, "[rocp_stub_signal] mkdir %s: %s\n",
                g_ctrl_dir, strerror(errno));
        return;
    }

    snprintf(g_ctrl_path, sizeof(g_ctrl_path), "%s/ctrl", g_ctrl_dir);

    int fd = open(g_ctrl_path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fprintf(stderr, "[rocp_stub_signal] open %s: %s\n",
                g_ctrl_path, strerror(errno));
        return;
    }
    if (ftruncate(fd, sizeof(rocp_ctrl_t)) != 0) {
        fprintf(stderr, "[rocp_stub_signal] ftruncate: %s\n", strerror(errno));
        close(fd);
        return;
    }
    g_ctrl = (rocp_ctrl_t*)mmap(NULL, sizeof(rocp_ctrl_t),
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_ctrl == MAP_FAILED) {
        fprintf(stderr, "[rocp_stub_signal] mmap: %s\n", strerror(errno));
        g_ctrl = NULL;
        return;
    }

    memset(g_ctrl, 0, sizeof(*g_ctrl));
    g_ctrl->magic          = ROCP_CTRL_MAGIC;
    g_ctrl->struct_version = ROCP_CTRL_VERSION;
    atomic_store_explicit(&g_ctrl->command, (uint32_t)CMD_NONE, memory_order_release);
    atomic_store_explicit(&g_ctrl->version, 0u, memory_order_release);
    atomic_store_explicit(&g_ctrl->context_active, 0u, memory_order_release);
    atomic_store_explicit(&g_ctrl->context_id, 0ull, memory_order_release);
    g_ctrl->pid        = (uint32_t)g_pid;
    g_ctrl->start_time = read_proc_start_time();

    /* --- 4) atexit + bg thread ------------------------------------ */
    atexit(stub_atexit);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&g_bg_thread, &attr, control_poll_loop, NULL) == 0) {
        g_bg_thread_started = 1;
    } else {
        fprintf(stderr, "[rocp_stub_signal] pthread_create failed\n");
    }
    pthread_attr_destroy(&attr);
}
