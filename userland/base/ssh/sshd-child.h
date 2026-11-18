/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What sshd's two unprivileged processes (the pre-authentication process
 * and the session process) share: taking the settings the monitor sends
 * first (CONFIG), reading a frame from the monitor, the host key
 * operations that go through the monitor, and the blocking write of the
 * transport's pending output.
 *
 * Both processes start with the client's connection at SSHD_FD_CLIENT
 * and the monitor's socket at SSHD_FD_MONITOR (sshd.h).  The monitor's
 * socket is used blocking, one request and its reply at a time; the
 * client's connection is made non-blocking and driven with poll.
 */

#ifndef SSH_SSHD_CHILD_H
#define SSH_SSHD_CHILD_H

#include "ssh-privsep.h"
#include "ssh-transport.h"
#include "sshd.h"

/*
 * What a child knows of its connection from CONFIG: the settings (read
 * with sshd_config_parse from the defaults), the host's public key blob,
 * the peer for the log, and whether sshd runs as root.
 */
struct sshd_child {
	struct sshd_config config;
	uint8_t host_key_blob[SSH_KEY_BLOB_SIZE];
	char peer[64];
	int privileged;
};

/*
 * Starts a child of the monitor: closes every descriptor above
 * SSHD_FD_MONITOR, ignores SIGPIPE (a write to a closed socket is an
 * error, not the end of the process), makes the client's connection
 * non-blocking and opens the log before anything can take the file
 * system away.
 */
void sshd_child_start(void);

/*
 * Reads one whole frame from the monitor's socket (blocking) into *type
 * and body (secret).  input keeps bytes read past the frame for the next
 * call (secret, limit 4 + SSH_MON_STATE_MAX).  Returns 0, EIO (the
 * monitor closed or the socket failed) or an error of ssh_mon_next_frame.
 */
int sshd_child_read_frame(struct ssh_buf *input, enum ssh_mon_type *type, struct ssh_buf *body);

/*
 * Reads CONFIG into child: the text with sshd_config_parse, the blob
 * (which must be SSH_KEY_BLOB_SIZE bytes of an ssh-ed25519 key), the peer
 * and the mode, and nothing after them.  input as for
 * sshd_child_read_frame.  Returns 0, EIO or EPROTO (another frame, a
 * malformed body, settings that do not parse).
 */
int sshd_child_read_config(struct ssh_buf *input, struct sshd_child *child);

/* The host_key operation of struct ssh_transport_ops: appends the blob of the struct sshd_child that context points at. */
int sshd_child_host_key(void *context, struct ssh_buf *out);

/*
 * The sign operation of struct ssh_transport_ops: asks the monitor for
 * the signature of h (SIGN) and appends the blob of its SIGNATURE reply.
 * Returns 0 or EIO.
 */
int sshd_child_sign(void *context, const uint8_t *h, struct ssh_buf *out);

/*
 * Asks the monitor with one request of type and size bytes of body and
 * takes its RESULT into *result.  Returns 0, or EIO when the call fails or
 * the reply is not a RESULT of four bytes.
 */
int sshd_child_ask(enum ssh_mon_type type, const uint8_t *body, size_t size, uint32_t *result);

/* Sends one frame of type with size bytes of body to the monitor, for a message that has no reply; 0 or EIO. */
int sshd_child_send(enum ssh_mon_type type, const uint8_t *body, size_t size);

/*
 * Writes the transport's pending output to the client's connection until
 * none is left (polling while the connection is full).  Returns 0 or EIO.
 */
int sshd_child_flush(struct ssh_transport *transport);

/* Returns the seconds of the monotonic clock, for the transport's tick. */
uint64_t sshd_child_now(void);

#endif
