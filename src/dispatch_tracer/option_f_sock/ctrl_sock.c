/*
 * ctrl_sock.c — rocp_ctrl_sock CLI controller.
 *
 * Usage:
 *   rocp_ctrl_sock --pid <pid> configure   [--hip] [--hsa] [--rccl] [--ompt]
 *                                          [--output <text|json|otlp|perfetto>]
 *                                          [--out <path>]
 *                                          [--filter <glob>] [--exclude <glob>]
 *                                          [--buffer-kb <N>]
 *   rocp_ctrl_sock --pid <pid> activate
 *   rocp_ctrl_sock --pid <pid> deactivate
 *   rocp_ctrl_sock --pid <pid> reconfigure ...same flags as configure...
 *   rocp_ctrl_sock --pid <pid> status
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "rocp_protocol.h"
#include "rocp_sock_protocol.h"

static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s --pid <pid> <action> [flags]\n"
        "Actions: configure | activate | deactivate | reconfigure | status\n"
        "Flags:\n"
        "  --hip        Enable HIP runtime domain\n"
        "  --hsa        Enable HSA core domain\n"
        "  --rccl       Enable RCCL domain\n"
        "  --ompt       Enable OMPT domain\n"
        "  --output <fmt>    text|json|otlp|perfetto\n"
        "  --out    <path>   Output file path\n"
        "  --filter  <glob>  Filter pattern\n"
        "  --exclude <glob>  Exclude pattern\n"
        "  --buffer-kb <N>   Buffer size in KB\n",
        prog);
}

static int has_flag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

static const char* get_str(int argc, char** argv, const char* name)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
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

static void fill_config_from_argv(rocp_config_t* cfg, int argc, char** argv)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable_hip   = has_flag(argc, argv, "--hip");
    cfg->enable_hsa   = has_flag(argc, argv, "--hsa");
    cfg->enable_rccl  = has_flag(argc, argv, "--rccl");
    cfg->enable_ompt  = has_flag(argc, argv, "--ompt");
    cfg->output_format = parse_output_fmt(get_str(argc, argv, "--output"));

    const char* out = get_str(argc, argv, "--out");
    if (out) snprintf(cfg->output_path, sizeof(cfg->output_path), "%s", out);
    const char* flt = get_str(argc, argv, "--filter");
    if (flt) snprintf(cfg->filter_pattern, sizeof(cfg->filter_pattern), "%s", flt);
    const char* exc = get_str(argc, argv, "--exclude");
    if (exc) snprintf(cfg->exclude_pattern, sizeof(cfg->exclude_pattern), "%s", exc);
    const char* bk = get_str(argc, argv, "--buffer-kb");
    if (bk) cfg->buffer_size_kb = (uint32_t)atoi(bk);
}

int main(int argc, char** argv)
{
    const char* pid_str = get_str(argc, argv, "--pid");
    if (!pid_str) { usage(argv[0]); return 2; }
    pid_t target_pid = (pid_t)atoi(pid_str);
    if (target_pid <= 0) { usage(argv[0]); return 2; }

    /* First non-flag positional arg after --pid is the action. */
    const char* action = NULL;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            /* skip flag and possibly its value */
            if (strcmp(argv[i], "--pid") == 0 ||
                strcmp(argv[i], "--output") == 0 ||
                strcmp(argv[i], "--out") == 0 ||
                strcmp(argv[i], "--filter") == 0 ||
                strcmp(argv[i], "--exclude") == 0 ||
                strcmp(argv[i], "--buffer-kb") == 0) {
                ++i;
            }
            continue;
        }
        action = argv[i];
        break;
    }
    if (!action) { usage(argv[0]); return 2; }

    rocp_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (strcmp(action, "configure") == 0) {
        cmd.type = CMD_CONFIGURE;
        fill_config_from_argv(&cmd.config, argc, argv);
    } else if (strcmp(action, "activate") == 0) {
        cmd.type = CMD_ACTIVATE;
    } else if (strcmp(action, "deactivate") == 0) {
        cmd.type = CMD_DEACTIVATE;
    } else if (strcmp(action, "reconfigure") == 0) {
        cmd.type = CMD_RECONFIGURE;
        fill_config_from_argv(&cmd.config, argc, argv);
    } else if (strcmp(action, "status") == 0) {
        cmd.type = CMD_STATUS;
    } else {
        usage(argv[0]);
        return 2;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    int n = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                     "rocprofiler_%d", (int)target_pid);
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);

    if (connect(sock, (struct sockaddr*)&addr, addrlen) < 0) {
        fprintf(stderr, "connect(pid=%d): %s\n", (int)target_pid, strerror(errno));
        close(sock);
        return 1;
    }

    size_t sent = 0;
    while (sent < sizeof(cmd)) {
        ssize_t w = send(sock, ((char*)&cmd) + sent, sizeof(cmd) - sent, 0);
        if (w <= 0) {
            fprintf(stderr, "send: %s\n", strerror(errno));
            close(sock);
            return 1;
        }
        sent += (size_t)w;
    }

    rocp_response_t resp;
    memset(&resp, 0, sizeof(resp));
    size_t got = 0;
    while (got < sizeof(resp)) {
        ssize_t r = recv(sock, ((char*)&resp) + got, sizeof(resp) - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(sock);

    if (got != sizeof(resp)) {
        fprintf(stderr, "short response (%zu of %zu bytes)\n", got, sizeof(resp));
        return 1;
    }

    if (strcmp(action, "status") == 0) {
        printf("active=%u events_traced=%lu events_dropped=%lu ctx_id=%u status=%u\n",
               resp.context_active,
               (unsigned long)resp.events_traced,
               (unsigned long)resp.events_dropped,
               resp.context_id,
               resp.status);
    } else {
        printf("%s: status=%u active=%u ctx_id=%u events=%lu dropped=%lu\n",
               action, resp.status, resp.context_active, resp.context_id,
               (unsigned long)resp.events_traced,
               (unsigned long)resp.events_dropped);
    }
    return resp.status == ROCP_RESP_OK ? 0 : 1;
}
