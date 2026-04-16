/*
 * shim_ipc.c — Low-level IPC implementation.
 *
 * Message framing, ring buffer, SCM_RIGHTS, start_time lookup.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "shim_ipc.h"

/* ---- memfd compat ---- */
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
    (void)name; (void)flags; errno = ENOSYS; return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Message framing                                                     */
/* ------------------------------------------------------------------ */

static int send_all(int sock, const void* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int recv_all(int sock, void* buf, size_t len)
{
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(sock, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

int shim_msg_send(int sock, const shim_msg_header_t* hdr,
                  const void* payload, size_t payload_size)
{
    if (send_all(sock, hdr, sizeof(*hdr)) < 0) return -1;
    if (payload_size > 0 && payload)
        if (send_all(sock, payload, payload_size) < 0) return -1;
    return 0;
}

int shim_msg_recv(int sock, shim_msg_header_t* hdr,
                  void* payload, size_t max_payload)
{
    if (recv_all(sock, hdr, sizeof(*hdr)) < 0) return -1;
    if (hdr->payload_size > 0) {
        if (hdr->payload_size > max_payload) return -1;
        if (payload)
            if (recv_all(sock, payload, hdr->payload_size) < 0) return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

static size_t round_up_pow2(size_t v)
{
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16; v |= v >> 32;
    return v + 1;
}

int shim_ring_create(shim_ring_t* ring, size_t capacity)
{
    memset(ring, 0, sizeof(*ring));
    ring->memfd = -1;

    capacity = round_up_pow2(capacity);
    if (capacity < 4096) capacity = 4096;

    size_t total = sizeof(shim_ring_header_t) + capacity;
    ring->memfd = compat_memfd_create("roc-shim-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (ring->memfd < 0) return -1;

    if (ftruncate(ring->memfd, (off_t)total) < 0) {
        close(ring->memfd); ring->memfd = -1; return -1;
    }

    void* map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, ring->memfd, 0);
    if (map == MAP_FAILED) {
        close(ring->memfd); ring->memfd = -1; return -1;
    }

    ring->hdr = (shim_ring_header_t*)map;
    ring->data = (uint8_t*)map + sizeof(shim_ring_header_t);
    ring->data_size = capacity;

    ring->hdr->capacity = capacity;
    ring->hdr->mask     = capacity - 1;
    atomic_store(&ring->hdr->head, 0);
    atomic_store(&ring->hdr->tail, 0);

    pthread_spin_init(&ring->write_lock, PTHREAD_PROCESS_PRIVATE);
    ring->lock_initialized = 1;

    fcntl(ring->memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    return 0;
}

void shim_ring_destroy(shim_ring_t* ring)
{
    if (ring->lock_initialized) {
        pthread_spin_destroy(&ring->write_lock);
        ring->lock_initialized = 0;
    }
    if (ring->hdr) {
        munmap(ring->hdr, sizeof(shim_ring_header_t) + ring->data_size);
        ring->hdr = NULL;
    }
    if (ring->memfd >= 0) {
        close(ring->memfd);
        ring->memfd = -1;
    }
}

int shim_ring_write(shim_ring_t* ring, const void* record, size_t size)
{
    pthread_spin_lock(&ring->write_lock);

    shim_ring_header_t* h = ring->hdr;
    uint64_t head = atomic_load_explicit(&h->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&h->tail, memory_order_acquire);

    uint64_t used = head - tail;
    if (used + size > h->capacity) {
        pthread_spin_unlock(&ring->write_lock);
        return -1;
    }

    uint64_t pos = head & h->mask;
    if (pos + size <= h->capacity) {
        memcpy(ring->data + pos, record, size);
    } else {
        size_t first = h->capacity - pos;
        memcpy(ring->data + pos, record, first);
        memcpy(ring->data, (const uint8_t*)record + first, size - first);
    }

    atomic_store_explicit(&h->head, head + size, memory_order_release);
    pthread_spin_unlock(&ring->write_lock);
    return 0;
}

int shim_ring_read(shim_ring_t* ring, void* out, size_t record_size)
{
    shim_ring_header_t* h = ring->hdr;
    uint64_t head = atomic_load_explicit(&h->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&h->tail, memory_order_relaxed);

    if (head - tail < record_size) return -1;

    uint64_t pos = tail & h->mask;
    if (pos + record_size <= h->capacity) {
        memcpy(out, ring->data + pos, record_size);
    } else {
        size_t first = h->capacity - pos;
        memcpy(out, ring->data + pos, first);
        memcpy((uint8_t*)out + first, ring->data, record_size - first);
    }

    atomic_store_explicit(&h->tail, tail + record_size, memory_order_release);
    return 0;
}

size_t shim_ring_available(shim_ring_t* ring)
{
    shim_ring_header_t* h = ring->hdr;
    uint64_t head = atomic_load_explicit(&h->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&h->tail, memory_order_relaxed);
    return (size_t)(head - tail);
}

/* ------------------------------------------------------------------ */
/* SCM_RIGHTS                                                          */
/* ------------------------------------------------------------------ */

int shim_send_fd(int sock, int fd, const void* data, size_t len)
{
    struct iovec iov = { .iov_base = (void*)data, .iov_len = len };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg = {0};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    *(int*)CMSG_DATA(cmsg) = fd;

    return sendmsg(sock, &msg, MSG_NOSIGNAL) >= 0 ? 0 : -1;
}

int shim_recv_fd(int sock, int* fd, void* data, size_t len)
{
    struct iovec iov = { .iov_base = data, .iov_len = len };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg = {0};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return -1;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    *fd = *(int*)CMSG_DATA(cmsg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* start_time from /proc/self/stat                                     */
/* ------------------------------------------------------------------ */

uint64_t shim_get_start_time(void)
{
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char* p = strrchr(buf, ')');
    if (!p) return 0;
    p += 2;
    uint64_t val = 0;
    for (int field = 3; field <= 22 && p; field++) {
        val = strtoull(p, &p, 10);
        while (*p == ' ') p++;
    }
    return val;
}
