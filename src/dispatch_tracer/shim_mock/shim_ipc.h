/*
 * shim_ipc.h — Low-level IPC helpers for the shim mock.
 *
 * Provides abstract socket, SCM_RIGHTS fd passing, SO_PEERCRED auth,
 * message framing, and ring buffer operations.
 *
 * Matches SHIM_MEMFD_SOCK_DESIGN.md §11, §12.
 */
#ifndef SHIM_IPC_H
#define SHIM_IPC_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "shim_protocol.h"

/* ---- Message framing ---- */

int shim_msg_send(int sock, const shim_msg_header_t* hdr,
                  const void* payload, size_t payload_size);
int shim_msg_recv(int sock, shim_msg_header_t* hdr,
                  void* payload, size_t max_payload);

/* ---- Ring buffer ---- */

typedef struct {
    shim_ring_header_t* hdr;
    uint8_t*            data;
    size_t              data_size;
    int                 memfd;
    pthread_spinlock_t  write_lock;
    int                 lock_initialized;
} shim_ring_t;

int  shim_ring_create(shim_ring_t* ring, size_t capacity);
void shim_ring_destroy(shim_ring_t* ring);
int  shim_ring_write(shim_ring_t* ring, const void* record, size_t size);
int  shim_ring_read(shim_ring_t* ring, void* out, size_t record_size);
size_t shim_ring_available(shim_ring_t* ring);

/* ---- SCM_RIGHTS ---- */

int shim_send_fd(int sock, int fd, const void* data, size_t len);
int shim_recv_fd(int sock, int* fd, void* data, size_t len);

/* ---- Misc ---- */

uint64_t shim_get_start_time(void);

#endif /* SHIM_IPC_H */
