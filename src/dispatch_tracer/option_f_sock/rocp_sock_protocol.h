/*
 * rocp_sock_protocol.h — wire format for the Unix-socket control channel.
 *
 * The command enum is canonical (see rocp_protocol.h). This header
 * defines only the request/response envelopes carried over the socket.
 */
#ifndef ROCP_SOCK_PROTOCOL_H
#define ROCP_SOCK_PROTOCOL_H

#include <stdint.h>
#include "rocp_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Request: controller -> tool */
typedef struct {
    uint32_t      type;    /* enum rocp_ctrl_command */
    uint32_t      flags;   /* reserved */
    rocp_config_t config;  /* meaningful for CMD_CONFIGURE / CMD_RECONFIGURE */
} rocp_cmd_t;

/* Response: tool -> controller */
typedef struct {
    uint32_t status;         /* 0 = OK, 1 = ERROR */
    uint32_t context_active; /* 0 = inactive, 1 = active */
    uint32_t context_id;     /* truncated handle; 0 if not configured */
    uint32_t reserved;
    uint64_t events_traced;
    uint64_t events_dropped;
} rocp_response_t;

#define ROCP_RESP_OK    0u
#define ROCP_RESP_ERROR 1u

#ifdef __cplusplus
}
#endif

#endif /* ROCP_SOCK_PROTOCOL_H */
