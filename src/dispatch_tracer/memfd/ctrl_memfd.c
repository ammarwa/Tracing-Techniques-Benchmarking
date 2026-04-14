/*
 * ctrl_memfd.c - rocp_ctrl_memfd CLI controller.
 *
 * Usage:
 *   rocp_ctrl_memfd --pid <pid> configure [--hip] [--hsa] [--rccl] [--ompt]
 *                                         [--output text|json|otlp|perfetto]
 *                                         [--out <path>]
 *                                         [--filter <pat>] [--exclude <pat>]
 *                                         [--buffer-kb <n>]
 *   rocp_ctrl_memfd --pid <pid> activate
 *   rocp_ctrl_memfd --pid <pid> deactivate
 *   rocp_ctrl_memfd --pid <pid> reconfigure [same options as configure]
 *   rocp_ctrl_memfd --pid <pid> status
 *
 * Flow:
 *   - connect("\0rocprofiler_<pid>")
 *   - recvmsg with SCM_RIGHTS -> memfd
 *   - mmap(memfd, sizeof(rocp_ctrl_t))
 *   - verify magic/struct_version
 *   - write command/config, bump version atomically
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rocp_protocol.h"

static int recv_fd(int sock)
{
    char buf[1];
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

    ssize_t n;
    do { n = recvmsg(sock, &msg, 0); }
    while (n < 0 && errno == EINTR);
    if (n <= 0) return -1;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
        return -1;
    }
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

static int connect_abstract(pid_t target_pid)
{
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    int name_len = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                            "rocprofiler_%d", (int)target_pid);
    socklen_t slen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
    /* Retry briefly on ECONNREFUSED/ENOENT to tolerate the race where the
     * target process hasn't yet finished bind()+listen(). Budget ~2s. */
    int rc = -1;
    for (int attempt = 0; attempt < 40; ++attempt) {
        rc = connect(s, (struct sockaddr*)&addr, slen);
        if (rc == 0) break;
        if (errno != ECONNREFUSED && errno != ENOENT) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (rc != 0) {
        fprintf(stderr, "connect(\"\\0rocprofiler_%d\"): %s\n",
                (int)target_pid, strerror(errno));
        close(s);
        return -1;
    }
    return s;
}

static int parse_output_format(const char* s)
{
    if (!s) return ROCP_OUTPUT_TEXT;
    if (strcmp(s, "text")     == 0) return ROCP_OUTPUT_TEXT;
    if (strcmp(s, "json")     == 0) return ROCP_OUTPUT_JSON;
    if (strcmp(s, "otlp")     == 0) return ROCP_OUTPUT_OTLP;
    if (strcmp(s, "perfetto") == 0) return ROCP_OUTPUT_PERFETTO;
    fprintf(stderr, "unknown --output value: %s\n", s);
    return -1;
}

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s --pid <pid> configure|reconfigure [--hip] [--hsa] [--rccl] [--ompt]\n"
        "                                        [--output text|json|otlp|perfetto]\n"
        "                                        [--out <path>] [--buffer-kb <n>]\n"
        "                                        [--filter <pat>] [--exclude <pat>]\n"
        "  %s --pid <pid> activate | deactivate | status\n",
        prog, prog);
}

int main(int argc, char** argv)
{
    pid_t target_pid = 0;
    const char* cmd  = NULL;
    rocp_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.output_format = ROCP_OUTPUT_TEXT;
    cfg.buffer_size_kb = 64;

    int i = 1;
    while (i < argc) {
        const char* a = argv[i];
        if (strcmp(a, "--pid") == 0 && i+1 < argc) {
            target_pid = (pid_t)atoi(argv[++i]);
        } else if (!cmd && a[0] != '-') {
            cmd = a;
        } else if (strcmp(a, "--hip") == 0) {
            cfg.enable_hip = 1;
        } else if (strcmp(a, "--hsa") == 0) {
            cfg.enable_hsa = 1;
        } else if (strcmp(a, "--rccl") == 0) {
            cfg.enable_rccl = 1;
        } else if (strcmp(a, "--ompt") == 0) {
            cfg.enable_ompt = 1;
        } else if (strcmp(a, "--output") == 0 && i+1 < argc) {
            int f = parse_output_format(argv[++i]);
            if (f < 0) return 2;
            cfg.output_format = (uint32_t)f;
        } else if (strcmp(a, "--out") == 0 && i+1 < argc) {
            snprintf(cfg.output_path, sizeof(cfg.output_path), "%s", argv[++i]);
        } else if (strcmp(a, "--filter") == 0 && i+1 < argc) {
            snprintf(cfg.filter_pattern, sizeof(cfg.filter_pattern), "%s", argv[++i]);
        } else if (strcmp(a, "--exclude") == 0 && i+1 < argc) {
            snprintf(cfg.exclude_pattern, sizeof(cfg.exclude_pattern), "%s", argv[++i]);
        } else if (strcmp(a, "--buffer-kb") == 0 && i+1 < argc) {
            cfg.buffer_size_kb = (uint32_t)atoi(argv[++i]);
        } else {
            fprintf(stderr, "unknown arg: %s\n", a);
            usage(argv[0]);
            return 2;
        }
        ++i;
    }

    if (target_pid <= 0 || !cmd) { usage(argv[0]); return 2; }

    int sock = connect_abstract(target_pid);
    if (sock < 0) return 1;

    int memfd = recv_fd(sock);
    if (memfd < 0) {
        fprintf(stderr, "failed to receive memfd via SCM_RIGHTS\n");
        close(sock);
        return 1;
    }

    rocp_ctrl_t* ctrl = mmap(NULL, sizeof(rocp_ctrl_t),
                             PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (ctrl == MAP_FAILED) {
        perror("mmap(memfd)");
        close(memfd); close(sock);
        return 1;
    }

    if (ctrl->magic != ROCP_CTRL_MAGIC ||
        ctrl->struct_version != ROCP_CTRL_VERSION) {
        fprintf(stderr, "bad magic/version: magic=0x%x ver=%u\n",
                ctrl->magic, ctrl->struct_version);
        munmap(ctrl, sizeof(*ctrl));
        close(memfd); close(sock);
        return 1;
    }

    uint32_t cmdnum = CMD_NONE;
    int write_config = 0;

    if (strcmp(cmd, "configure") == 0)         { cmdnum = CMD_CONFIGURE;   write_config = 1; }
    else if (strcmp(cmd, "reconfigure") == 0)  { cmdnum = CMD_RECONFIGURE; write_config = 1; }
    else if (strcmp(cmd, "activate") == 0)     { cmdnum = CMD_ACTIVATE; }
    else if (strcmp(cmd, "deactivate") == 0)   { cmdnum = CMD_DEACTIVATE; }
    else if (strcmp(cmd, "status") == 0)       { cmdnum = CMD_STATUS; }
    else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        munmap(ctrl, sizeof(*ctrl));
        close(memfd); close(sock);
        return 2;
    }

    if (cmdnum == CMD_STATUS) {
        printf("pid=%u start_time=%lu\n",
               (unsigned)ctrl->pid, (unsigned long)ctrl->start_time);
        printf("magic=0x%08x struct_version=%u\n",
               ctrl->magic, ctrl->struct_version);
        printf("command=%u version=%u\n",
               (unsigned)atomic_load(&ctrl->command),
               (unsigned)atomic_load(&ctrl->version));
        printf("context_active=%u context_id=0x%lx\n",
               (unsigned)atomic_load(&ctrl->context_active),
               (unsigned long)atomic_load(&ctrl->context_id));
        printf("events_traced=%lu events_dropped=%lu\n",
               (unsigned long)atomic_load(&ctrl->events_traced),
               (unsigned long)atomic_load(&ctrl->events_dropped));
        printf("config: hip=%u hsa=%u rccl=%u ompt=%u output_format=%u buf_kb=%u\n",
               ctrl->config.enable_hip, ctrl->config.enable_hsa,
               ctrl->config.enable_rccl, ctrl->config.enable_ompt,
               ctrl->config.output_format, ctrl->config.buffer_size_kb);
        printf("output_path=\"%s\"\n", ctrl->config.output_path);
    } else {
        if (write_config) {
            memcpy(&ctrl->config, &cfg, sizeof(cfg));
        }
        atomic_store_explicit(&ctrl->command, cmdnum, memory_order_release);
        uint32_t v = atomic_load(&ctrl->version);
        atomic_store_explicit(&ctrl->version, v + 1, memory_order_release);
        printf("sent %s (version -> %u)\n", cmd, (unsigned)(v + 1));
    }

    munmap(ctrl, sizeof(*ctrl));
    close(memfd);
    close(sock);
    return 0;
}
