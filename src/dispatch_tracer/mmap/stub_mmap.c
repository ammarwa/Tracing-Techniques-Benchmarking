/*
 * stub_mmap.c - librocp_stub_mmap.so
 *
 * LD_PRELOAD stub that sets up the mmap'd control file at
 *   /run/user/<uid>/rocprofiler/<pid>/ctrl
 * and spawns a polling background thread. Does NOT export
 * rocprofiler_configure — so mock_register's symbol scan does not load
 * the SDK until the controller sends CMD_CONFIGURE, at which point the
 * stub dlopens the tool library (libmock_sdk_tool_mmap.so) and forces
 * configuration.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#define LOAD_ACQ(p)      atomic_load_explicit((p), memory_order_acquire)
#define STORE_REL(p, v)  atomic_store_explicit((p), (v), memory_order_release)
#define STORE_SC(p, v)   atomic_store_explicit((p), (v), memory_order_seq_cst)
#define FADD_RLX(p, v)   atomic_fetch_add_explicit((p), (v), memory_order_relaxed)
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

/* ----------------------------------------------------------------- */
/* Shared state exposed to the tool via rocp_stub_get_state().        */
/* ----------------------------------------------------------------- */
static rocp_ctrl_t*              g_ctrl            = NULL;
static rocp_config_t             g_pending_config;
static rocprofiler_context_id_t  g_saved_ctx       = {0};
static rocp_stub_state_t         g_state;

/* Filter (updated on CMD_RECONFIGURE, read by tool callbacks).
 * Must have default visibility so the tool lib can see them. */
#define EXPORT __attribute__((visibility("default")))
EXPORT _Atomic uint32_t  g_runtime_domain_mask = 0xFFFFFFFFu;
EXPORT _Atomic uint32_t  g_runtime_output_fmt  = 0;
EXPORT char              g_runtime_output_path[256];
EXPORT pthread_mutex_t   g_runtime_filter_lock = PTHREAD_MUTEX_INITIALIZER;

/* ----------------------------------------------------------------- */
/* Background-thread / lifecycle bookkeeping                          */
/* ----------------------------------------------------------------- */
static pthread_t        g_bg_thread;
static int              g_bg_thread_started = 0;
static _Atomic int      g_shutdown_flag     = 0;
static _Atomic bool     g_sdk_loaded        = false;
static _Atomic int      g_finalize_status   = 0;
static void*            g_sdk_handle        = NULL;
static uid_t            g_uid;
static pid_t            g_pid;
static char             g_ctrl_path[PATH_MAX];
static char             g_ctrl_dir[PATH_MAX];

/* Function pointers resolved after dlopen of the tool library */
static rocprofiler_status_t (*p_force_configure)(rocprofiler_configure_func_t) = NULL;
static rocprofiler_status_t (*p_start_context)(rocprofiler_context_id_t)       = NULL;
static rocprofiler_status_t (*p_stop_context)(rocprofiler_context_id_t)        = NULL;

/* ----------------------------------------------------------------- */
/* Helpers                                                            */
/* ----------------------------------------------------------------- */

/* Read /proc/self/stat field 22 (starttime). Field 2 is the executable
 * name in parentheses which may contain spaces; handle by finding ')'. */
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
    /* After ')' the next tokens are fields 3,4,... separated by spaces.
     * starttime is field 22, so we need to skip 22-3 = 19 spaces from
     * the start of field 3 (i.e. after "). " skip 19 fields). */
    char* p = rparen + 2; /* skip ") " */
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

static void apply_runtime_filter_locked(const rocp_config_t* cfg)
{
    uint32_t mask = 0;
    if (cfg->enable_hip)  mask |= (1u << ROCPROFILER_CALLBACK_TRACING_HIP_RUNTIME_API);
    if (cfg->enable_hsa)  mask |= (1u << ROCPROFILER_CALLBACK_TRACING_HSA_CORE_API);
    if (cfg->enable_rccl) mask |= (1u << ROCPROFILER_CALLBACK_TRACING_RCCL_API);
    if (cfg->enable_ompt) mask |= (1u << ROCPROFILER_CALLBACK_TRACING_OMPT);
    if (mask == 0) mask = 0xFFFFFFFFu; /* nothing specified => don't drop */
    atomic_store_explicit(&g_runtime_domain_mask, mask, memory_order_release);
    atomic_store_explicit(&g_runtime_output_fmt, cfg->output_format, memory_order_release);

    pthread_mutex_lock(&g_runtime_filter_lock);
    snprintf(g_runtime_output_path, sizeof(g_runtime_output_path),
             "%s", cfg->output_path);
    pthread_mutex_unlock(&g_runtime_filter_lock);
}

static void apply_runtime_filter(const rocp_config_t* cfg)
{
    apply_runtime_filter_locked(cfg);
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
/* Load SDK tool + force_configure (first attach)                     */
/* ----------------------------------------------------------------- */
static void load_sdk_and_configure(void)
{
    /* Discover the directory of this stub library via dladdr so we can
     * load the sibling tool library without relying on LD_LIBRARY_PATH.
     * Also check $ROCP_DISPATCH_LIB_DIR if set (used by benchmark scripts). */
    char sibling[PATH_MAX] = {0};
    Dl_info info;
    if (dladdr((void*)&load_sdk_and_configure, &info) && info.dli_fname) {
        const char* slash = strrchr(info.dli_fname, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - info.dli_fname);
            if (dir_len + sizeof("/libmock_sdk_tool_mmap.so") < sizeof(sibling)) {
                memcpy(sibling, info.dli_fname, dir_len);
                snprintf(sibling + dir_len,
                         sizeof(sibling) - dir_len,
                         "/libmock_sdk_tool_mmap.so");
            }
        }
    }

    char envpath[PATH_MAX] = {0};
    const char* libdir = getenv("ROCP_DISPATCH_LIB_DIR");
    if (libdir) {
        snprintf(envpath, sizeof(envpath), "%s/libmock_sdk_tool_mmap.so", libdir);
    }

    const char* candidates[] = {
        envpath[0] ? envpath : NULL,
        sibling[0] ? sibling : NULL,
        "libmock_sdk_tool_mmap.so",
        "./libmock_sdk_tool_mmap.so",
        NULL
    };
    for (size_t i = 0; candidates[i] && !g_sdk_handle; ++i) {
        g_sdk_handle = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (!g_sdk_handle) {
            fprintf(stderr, "[rocp_stub_mmap] dlopen('%s') failed: %s\n",
                    candidates[i], dlerror());
        }
    }
    if (!g_sdk_handle) {
        fprintf(stderr, "[rocp_stub_mmap] all dlopen candidates failed\n");
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
        fprintf(stderr,
                "[rocp_stub_mmap] could not resolve tool/SDK symbols\n");
        return;
    }

    rocprofiler_status_t st = p_force_configure(tool_configure);
    if (st != ROCPROFILER_STATUS_SUCCESS) {
        fprintf(stderr,
                "[rocp_stub_mmap] rocprofiler_force_configure failed: %d\n",
                (int)st);
        return;
    }

    atomic_store_explicit(&g_ctrl->context_active, 1u, memory_order_release);
}

/* ----------------------------------------------------------------- */
/* Background polling loop                                            */
/* ----------------------------------------------------------------- */
static void* control_poll_loop(void* arg)
{
    (void)arg;
    uint32_t last_version = 0;

    while (!atomic_load_explicit(&g_shutdown_flag, memory_order_acquire)) {
        uint32_t ver = atomic_load_explicit(&g_ctrl->version, memory_order_acquire);
        if (ver == last_version) {
            usleep(1000);
            continue;
        }

        uint32_t cmd = atomic_load_explicit(&g_ctrl->command, memory_order_acquire);
        switch (cmd) {
        case CMD_CONFIGURE: {
            memcpy(&g_pending_config, &g_ctrl->config, sizeof(g_pending_config));
            apply_runtime_filter(&g_pending_config);
            bool expected = false;
            if (atomic_compare_exchange_strong_explicit(&g_sdk_loaded,
                                            &expected, true,
                                            memory_order_acq_rel,
                                            memory_order_acquire)) {
                load_sdk_and_configure();
            } else {
                /* SDK already loaded — runtime filter only. */
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
/* atexit cleanup — per design, NOT __attribute__((destructor)).      */
/* ----------------------------------------------------------------- */
static void stub_atexit(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_finalize_status,
                                     &expected, -1,
                                     memory_order_seq_cst,
                                     memory_order_seq_cst)) {
        return;
    }

    /* Stop context if still active. */
    if (g_ctrl &&
        atomic_load_explicit(&g_ctrl->context_active, memory_order_acquire) &&
        p_stop_context) {
        p_stop_context(g_saved_ctx);
        atomic_store_explicit(&g_ctrl->context_active, 0u, memory_order_release);
    }

    /* Signal and join background thread. */
    atomic_store_explicit(&g_shutdown_flag, 1, memory_order_release);
    if (g_bg_thread_started) {
        pthread_join(g_bg_thread, NULL);
        g_bg_thread_started = 0;
    }

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
    g_uid = getuid();
    g_pid = getpid();

    char base[PATH_MAX];
    snprintf(base, sizeof(base), "/run/user/%u", (unsigned)g_uid);
    /* /run/user/<uid> is normally created by pam_systemd; don't fail if
     * it already exists. We do not attempt to create it when missing
     * (no privilege) — instead surface an error. */
    struct stat st;
    if (stat(base, &st) != 0) {
        fprintf(stderr,
                "[rocp_stub_mmap] %s missing; needs pam_systemd\n", base);
        return;
    }

    char dir1[PATH_MAX];
    snprintf(dir1, sizeof(dir1), "/run/user/%u/rocprofiler", (unsigned)g_uid);
    if (mkdir_p(dir1, 0700) != 0) {
        fprintf(stderr, "[rocp_stub_mmap] mkdir %s: %s\n",
                dir1, strerror(errno));
        return;
    }

    snprintf(g_ctrl_dir, sizeof(g_ctrl_dir),
             "/run/user/%u/rocprofiler/%d", (unsigned)g_uid, (int)g_pid);
    if (mkdir_p(g_ctrl_dir, 0700) != 0) {
        fprintf(stderr, "[rocp_stub_mmap] mkdir %s: %s\n",
                g_ctrl_dir, strerror(errno));
        return;
    }

    snprintf(g_ctrl_path, sizeof(g_ctrl_path),
             "%s/ctrl", g_ctrl_dir);

    int fd = open(g_ctrl_path, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
    if (fd < 0) {
        fprintf(stderr, "[rocp_stub_mmap] open %s: %s\n",
                g_ctrl_path, strerror(errno));
        return;
    }
    if (ftruncate(fd, sizeof(rocp_ctrl_t)) != 0) {
        fprintf(stderr, "[rocp_stub_mmap] ftruncate: %s\n", strerror(errno));
        close(fd);
        return;
    }
    g_ctrl = (rocp_ctrl_t*)mmap(NULL, sizeof(rocp_ctrl_t),
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_ctrl == MAP_FAILED) {
        fprintf(stderr, "[rocp_stub_mmap] mmap: %s\n", strerror(errno));
        g_ctrl = NULL;
        return;
    }

    /* Initialize ctrl struct. */
    memset(g_ctrl, 0, sizeof(*g_ctrl));
    g_ctrl->magic          = ROCP_CTRL_MAGIC;
    g_ctrl->struct_version = ROCP_CTRL_VERSION;
    atomic_store_explicit(&g_ctrl->command, (uint32_t)CMD_NONE, memory_order_release);
    atomic_store_explicit(&g_ctrl->version, 0u, memory_order_release);
    atomic_store_explicit(&g_ctrl->context_active, 0u, memory_order_release);
    atomic_store_explicit(&g_ctrl->context_id, 0ull, memory_order_release);
    g_ctrl->pid        = (uint32_t)g_pid;
    g_ctrl->start_time = read_proc_start_time();

    /* Register atexit handler BEFORE starting the thread. */
    atexit(stub_atexit);

    /* Spawn joinable background thread. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (pthread_create(&g_bg_thread, &attr, control_poll_loop, NULL) == 0) {
        g_bg_thread_started = 1;
    } else {
        fprintf(stderr, "[rocp_stub_mmap] pthread_create failed\n");
    }
    pthread_attr_destroy(&attr);
}
