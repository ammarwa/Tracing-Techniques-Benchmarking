/*
 * ctrl_signal.c - rocp_ctrl_signal
 *
 * Same as the mmap channel's controller, but after writing the command + bumping
 * the version counter it sends SIGRTMIN+7 to the target via sigqueue()
 * so the stub's background thread is woken instantly instead of waiting
 * on a poll() timeout.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "rocp_protocol.h"

#ifndef ROCP_SIGNAL_NUM
#define ROCP_SIGNAL_NUM (SIGRTMIN + 7)
#endif

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s --pid <pid> <command> [flags]\n"
        "  Commands: configure | activate | deactivate | reconfigure | status\n"
        "  Flags:\n"
        "    --hip --hsa --rccl --ompt\n"
        "    --output {text|json}\n"
        "    --out <path>\n"
        "    --buf-kb <N>\n"
        "    --filter <pattern>\n"
        "    --exclude <pattern>\n",
        prog);
}

static int has_flag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

static const char* get_str_flag(int argc, char** argv,
                                const char* name, const char* def)
{
    for (int i = 1; i < argc - 1; ++i)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return def;
}

static int get_int_flag(int argc, char** argv,
                        const char* name, int def)
{
    const char* s = get_str_flag(argc, argv, name, NULL);
    return s ? atoi(s) : def;
}

static uint64_t proc_start_time(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
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

static uint32_t parse_output_fmt(const char* s)
{
    if (!s) return ROCP_OUTPUT_TEXT;
    if (strcmp(s, "text") == 0)     return ROCP_OUTPUT_TEXT;
    if (strcmp(s, "json") == 0)     return ROCP_OUTPUT_JSON;
    if (strcmp(s, "otlp") == 0)     return ROCP_OUTPUT_OTLP;
    if (strcmp(s, "perfetto") == 0) return ROCP_OUTPUT_PERFETTO;
    return ROCP_OUTPUT_TEXT;
}

static void write_config(rocp_ctrl_t* ctrl, int argc, char** argv)
{
    ctrl->config.enable_hip  = has_flag(argc, argv, "--hip")  ? 1u : 0u;
    ctrl->config.enable_hsa  = has_flag(argc, argv, "--hsa")  ? 1u : 0u;
    ctrl->config.enable_rccl = has_flag(argc, argv, "--rccl") ? 1u : 0u;
    ctrl->config.enable_ompt = has_flag(argc, argv, "--ompt") ? 1u : 0u;

    ctrl->config.output_format =
        parse_output_fmt(get_str_flag(argc, argv, "--output", "text"));
    ctrl->config.buffer_size_kb =
        (uint32_t)get_int_flag(argc, argv, "--buf-kb", 4096);

    const char* out = get_str_flag(argc, argv, "--out", "");
    snprintf(ctrl->config.output_path,
             sizeof(ctrl->config.output_path), "%s", out);

    const char* flt = get_str_flag(argc, argv, "--filter", "");
    snprintf(ctrl->config.filter_pattern,
             sizeof(ctrl->config.filter_pattern), "%s", flt);

    const char* exc = get_str_flag(argc, argv, "--exclude", "");
    snprintf(ctrl->config.exclude_pattern,
             sizeof(ctrl->config.exclude_pattern), "%s", exc);
}

/* Returns the new version (post-bump) that was published. */
static uint32_t send_cmd(rocp_ctrl_t* ctrl, uint32_t cmd)
{
    uint32_t v = atomic_load_explicit(&ctrl->version, memory_order_relaxed);
    atomic_store_explicit(&ctrl->command, cmd, memory_order_relaxed);
    uint32_t nv = v + 1u;
    atomic_store_explicit(&ctrl->version, nv, memory_order_release);
    return nv;
}

static int signal_target(pid_t target, uint32_t version)
{
    union sigval val;
    memset(&val, 0, sizeof(val));
    val.sival_int = (int)version;
    if (sigqueue(target, ROCP_SIGNAL_NUM, val) != 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "sigqueue: PID %d not found\n", (int)target);
        } else if (errno == EPERM) {
            fprintf(stderr, "sigqueue: permission denied for PID %d\n",
                    (int)target);
        } else {
            fprintf(stderr, "sigqueue: %s\n", strerror(errno));
        }
        return -1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    pid_t target = (pid_t)get_int_flag(argc, argv, "--pid", 0);
    if (target <= 0) {
        usage(argv[0]);
        return 2;
    }
    const char* action = NULL;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' &&
            !(i > 1 && strcmp(argv[i - 1], "--pid")    == 0) &&
            !(i > 1 && strcmp(argv[i - 1], "--output") == 0) &&
            !(i > 1 && strcmp(argv[i - 1], "--out")    == 0) &&
            !(i > 1 && strcmp(argv[i - 1], "--buf-kb") == 0) &&
            !(i > 1 && strcmp(argv[i - 1], "--filter") == 0) &&
            !(i > 1 && strcmp(argv[i - 1], "--exclude")== 0)) {
            action = argv[i];
            break;
        }
    }
    if (!action) {
        usage(argv[0]);
        return 2;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/run/user/%u/rocprofiler/%d/ctrl",
             (unsigned)getuid(), (int)target);

    int fd = open(path, O_RDWR | O_NOFOLLOW);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 1;
    }
    rocp_ctrl_t* ctrl = (rocp_ctrl_t*)mmap(NULL, sizeof(rocp_ctrl_t),
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, 0);
    close(fd);
    if (ctrl == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        return 1;
    }
    if (ctrl->magic != ROCP_CTRL_MAGIC) {
        fprintf(stderr, "bad magic 0x%x (want 0x%x)\n",
                ctrl->magic, ROCP_CTRL_MAGIC);
        return 1;
    }
    if (ctrl->struct_version != ROCP_CTRL_VERSION) {
        fprintf(stderr, "version mismatch %u vs %u\n",
                ctrl->struct_version, ROCP_CTRL_VERSION);
        return 1;
    }

    uint64_t cur_start = proc_start_time(target);
    if (cur_start != 0 && ctrl->start_time != 0 &&
        cur_start != ctrl->start_time) {
        fprintf(stderr,
                "stale control file (start_time %llu != %llu)\n",
                (unsigned long long)ctrl->start_time,
                (unsigned long long)cur_start);
        return 1;
    }

    int need_signal = 1;
    uint32_t nv = 0;
    int rc = 0;

    if (strcmp(action, "configure") == 0) {
        write_config(ctrl, argc, argv);
        nv = send_cmd(ctrl, CMD_CONFIGURE);
        printf("Configured PID %d (version=%u)\n", (int)target, nv);
    } else if (strcmp(action, "activate") == 0) {
        nv = send_cmd(ctrl, CMD_ACTIVATE);
        printf("Activated PID %d (version=%u)\n", (int)target, nv);
    } else if (strcmp(action, "deactivate") == 0) {
        nv = send_cmd(ctrl, CMD_DEACTIVATE);
        printf("Deactivated PID %d (version=%u)\n", (int)target, nv);
    } else if (strcmp(action, "reconfigure") == 0) {
        write_config(ctrl, argc, argv);
        nv = send_cmd(ctrl, CMD_RECONFIGURE);
        printf("Reconfigured PID %d (version=%u)\n", (int)target, nv);
    } else if (strcmp(action, "status") == 0) {
        need_signal = 0;
        uint32_t active =
            atomic_load_explicit(&ctrl->context_active, memory_order_acquire);
        uint64_t cid =
            atomic_load_explicit(&ctrl->context_id, memory_order_acquire);
        uint64_t traced =
            atomic_load_explicit(&ctrl->events_traced, memory_order_relaxed);
        uint64_t dropped =
            atomic_load_explicit(&ctrl->events_dropped, memory_order_relaxed);
        printf("pid=%d active=%u context_id=%llu traced=%llu dropped=%llu\n",
               (int)target, active,
               (unsigned long long)cid,
               (unsigned long long)traced,
               (unsigned long long)dropped);
    } else {
        fprintf(stderr, "unknown action: %s\n", action);
        usage(argv[0]);
        munmap(ctrl, sizeof(rocp_ctrl_t));
        return 2;
    }

    if (need_signal) {
        if (signal_target(target, nv) != 0) rc = 1;
    }

    munmap(ctrl, sizeof(rocp_ctrl_t));
    return rc;
}
