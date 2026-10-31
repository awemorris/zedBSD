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
 * SSH_MON_STATE_MAX for the two that carry the exported transport
 * (AUTH_DONE, SESSION_START) and for CONFIG (which carries a whole
 * configuration file), is
 * refused.
 *
 * Messages from the monitor that start a child (sent first, without a
 * request):
 *
 *   CONFIG          string sshd_config text (the file as the listener read
 *                   it; the child parses it with sshd_config_parse),
 *                   string host key blob, string peer, uint32 privileged
 *   SESSION_START   string user, string exported transport (to the
 *                   session process only, after CONFIG)
 *
 * Requests (pre-authentication process to monitor) and their replies:
 *
 *   SIGN            string h (32 bytes)            -> SIGNATURE string blob
 *                   (the first h signed becomes the session identifier the
 *                   monitor keeps; at most SSHD_PREAUTH_SIGN_MAX in all)
 *   PUBKEY_QUERY    string user, string key blob   -> RESULT uint32 0 or
 *                   (is this key in the user's authorized_keys?) EACCES
 *   PUBKEY_LOGIN    string user, string key blob, string signed data,
 *                   string signature blob          -> RESULT
 *                   (the monitor reads the signed data itself and checks
 *                   that it is its session identifier as a string, then
 *                   SSH_MSG_USERAUTH_REQUEST, this user, "ssh-connection",
 *                   "publickey", TRUE, "ssh-ed25519" and this key blob,
 *                   and nothing after; that the key is authorized; and the
 *                   signature)
 *   PASSWORD_LOGIN  string user, string password   -> RESULT
 *   AUTH_DONE       string exported transport      -> (no reply; the
 *                   pre-authentication process exits, the monitor starts
 *                   the session process; allowed only after a RESULT 0 of
 *                   a login, and only once)
 *
 * Requests (session process to monitor):
 *
 *   SIGN            as above (a re-exchange): only an h of 32 bytes, at
 *                   most one per SSHD_SIGN_INTERVAL_MS (a sooner request
 *                   waits)
 *   LOGIN_RECORD    string tty path, uint32 pid    -> RESULT (the monitor
 *                   checks that the tty is a /dev/pts path owned by the
 *                   user and that pid is a child of the session process it
 *                   started, then writes the USER_PROCESS record)
 *   LOGOUT_RECORD   string tty path, uint32 pid    -> RESULT (DEAD_PROCESS;
 *                   only for a record this session wrote)
 *
 * A request of a type not listed for the process's stage, or out of order,
 * ends the connection (the monitor kills the child and exits).
 *
 * Accepted risk: the session identifier is whatever h the pre-
 * authentication process asks to sign first.  A compromised pre-
 * authentication process working with a man in the middle could have the
 * monitor sign an exchange hash of another connection and relay a
 * victim's login; the monitor cannot see the exchange it did not take
 * part in.  OpenSSH's monitor has the same limit.  The bound on the number
 * of signatures keeps the process from being a signing service.
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
	SSH_MON_PUBKEY_QUERY = 3,
	SSH_MON_PUBKEY_LOGIN = 4,
	SSH_MON_PASSWORD_LOGIN = 5,
	SSH_MON_AUTH_DONE = 6,
	SSH_MON_LOGIN_RECORD = 7,
	SSH_MON_LOGOUT_RECORD = 8,
	SSH_MON_RESULT = 9,
	SSH_MON_CONFIG = 10,
	SSH_MON_SESSION_START = 11
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
 * reset).  The caller always passes a secret body buffer: PASSWORD_LOGIN,
 * AUTH_DONE and SESSION_START carry secrets, and the type is not known
 * until the frame is read.  A length over the limit of the type, a type
 * outside the enumeration, or a length of 0 is
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
