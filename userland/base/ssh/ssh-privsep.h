/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The messages between sshd's privileged monitor and the unprivileged
 * processes of one connection: the pre-authentication process (which
 * talks to the network before anyone has logged in) and the session
 * process (which runs as the user after the login).
 *
 * The monitor holds what the others must not: the host's private key, the
 * shadow file, the right to read any user's authorized_keys, the login
 * records.  The others ask through these messages over a socketpair, one
 * request and one reply at a time.  Every request is untrusted (the asking
 * process may be compromised): the monitor checks each against the state
 * it keeps itself (which requests are allowed now, which user is being
 * authenticated, how many attempts are left, the session identifier it
 * signed first).
 *
 * The frame: uint32 length (of what follows), byte type, then the body
 * in RFC 4251 types.  A frame longer than SSH_MON_FRAME_MAX, or
 * SSH_MON_STATE_MAX for the one that carries the exported transport, is
 * refused.
 *
 * Requests (pre-authentication process to monitor) and their replies:
 *
 *   SIGN            string h (32 bytes)            -> SIGNATURE string blob
 *                   (the first h signed becomes the session identifier
 *                   the monitor keeps; at most one per key exchange)
 *   HOST_KEY        (empty)                        -> HOST_KEY string blob
 *   PUBKEY_QUERY    string user, string key blob   -> RESULT uint32 0 or
 *                   (is this key in the user's authorized_keys?) EACCES
 *   PUBKEY_LOGIN    string user, string key blob, string signed data,
 *                   string signature blob          -> RESULT
 *                   (the monitor checks that the signed data begins with
 *                   its session identifier and names this user, the
 *                   connection service, "publickey" and this key, that the
 *                   key is authorized, and the signature)
 *   PASSWORD_LOGIN  string user, string password   -> RESULT
 *   AUTH_DONE       string exported transport      -> (no reply; the
 *                   pre-authentication process exits, the monitor starts
 *                   the session process)
 *
 * Requests (session process to monitor):
 *
 *   SIGN            as above (a re-exchange), only an h of 32 bytes,
 *                   counted against a limit per hour
 *   LOGIN_RECORD    string tty path, uint32 pid    -> RESULT (the monitor
 *                   checks that the tty is owned by the user and writes the
 *                   USER_PROCESS record)
 *   LOGOUT_RECORD   string tty path, uint32 pid    -> RESULT (DEAD_PROCESS)
 *
 * RESULT 0 means granted; EACCES refused; another value a failure.  A
 * refused login does not say why (the log does).
 */

#ifndef SSH_SSH_PRIVSEP_H
#define SSH_SSH_PRIVSEP_H

#include "ssh-buf.h"
#include "ssh-packet.h"

/* The largest frame other than the exported transport. */
#define SSH_MON_FRAME_MAX	(64U * 1024U)

/* The largest frame carrying the exported transport (its keys, lines and the unprocessed input). */
#define SSH_MON_STATE_MAX	(SSH_PACKET_INPUT_MAX + 64U * 1024U)

/* The frame types. */
enum ssh_mon_type {
	SSH_MON_SIGN = 1,
	SSH_MON_SIGNATURE = 2,
	SSH_MON_HOST_KEY = 3,
	SSH_MON_PUBKEY_QUERY = 4,
	SSH_MON_PUBKEY_LOGIN = 5,
	SSH_MON_PASSWORD_LOGIN = 6,
	SSH_MON_AUTH_DONE = 7,
	SSH_MON_LOGIN_RECORD = 8,
	SSH_MON_LOGOUT_RECORD = 9,
	SSH_MON_RESULT = 10
};

/*
 * Appends one frame (length, type, size bytes of body) to out.  The body
 * is built by the caller with ssh-buf.h.  Returns 0, EMSGSIZE (a body
 * over the limit of its type) or the error recorded in out.
 */
int ssh_mon_put_frame(enum ssh_mon_type type, const uint8_t *body, size_t size, struct ssh_buf *out);

/*
 * Takes one whole frame from the front of input (bytes read from the
 * socketpair): *type and the body (copied into body, which the caller
 * reset; secret when the type carries a secret).  A length over the limit
 * of the type, a type outside the enumeration, or a length of 0 is
 * EINVAL.  Returns 0, EAGAIN (incomplete), EINVAL or ENOMEM.
 */
int ssh_mon_next_frame(struct ssh_buf *input, enum ssh_mon_type *type, struct ssh_buf *body);

/*
 * Writes a whole frame to descriptor and reads the whole reply frame,
 * blocking (the asking side of one request).  Returns 0 with *type and
 * body filled, EIO (the other side closed or the descriptor failed), or
 * an error of ssh_mon_next_frame.  EINTR is retried.
 */
int ssh_mon_call(int descriptor, enum ssh_mon_type type, const uint8_t *request, size_t request_size, enum ssh_mon_type *reply_type, struct ssh_buf *reply);

#endif
