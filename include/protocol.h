/*
 * protocol.h - wire framing + AF_UNIX helpers. transport is a unix domain
 * socket = local IPC, NOT tcp/ip (the brief forbids tcp). every message:
 *   [ u32 big-endian length ][ payload ]
 * length prefix kills the partial-read problem -- reader knows exactly how
 * many bytes to wait for. payload is a plain text command line.
 */
#ifndef DURAKV_PROTOCOL_H
#define DURAKV_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define PROTO_MAX_PAYLOAD (1u << 20)   /* 1 MiB cap on any single frame */

/* Loop until exactly n bytes are transferred. Return 0 on success, -1 on
 * error or premature EOF. */
int io_read_all(int fd, void *buf, size_t n);
int io_write_all(int fd, const void *buf, size_t n);

/* Write one length-prefixed frame. Return 0 on success, -1 on error. */
int frame_write(int fd, const void *payload, uint32_t len);

/* Read one frame into buf (capacity cap), setting *len. Returns:
 *    0  success
 *   -1  peer closed / I/O error
 *   -2  framed length exceeds cap or PROTO_MAX_PAYLOAD (protocol violation) */
int frame_read(int fd, void *buf, uint32_t cap, uint32_t *len);

/* AF_UNIX endpoint helpers. Return a connected/listening fd, or -1 on error. */
int unix_listen(const char *path, int backlog);
int unix_connect(const char *path);

#endif /* DURAKV_PROTOCOL_H */
