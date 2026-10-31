/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The transport layer of one connection (RFC 4253): the exchange of
 * identification lines (4.2), the key exchange and its re-exchanges (7,
 * 9) with strict key exchange (draft-ietf-sshm-strict-kex-02 3), the
 * switch of keys at SSH_MSG_NEWKEYS (7.3), and the generic messages
 * (11: DISCONNECT, IGNORE, UNIMPLEMENTED, DEBUG).  Messages of the
 * layers above (5 and 6 service, 50 to 79 authentication, 80 to 127
 * connection, 192 to 255 local) are handed up and taken down.
 *
 * The layer does no I/O and does not block: the caller feeds received
 * bytes with ssh_transport_input, takes upper-layer messages with
 * ssh_transport_next, sends with ssh_transport_send, and writes what
 * ssh_transport_pending returns.  The host key operations are callbacks,
 * so that a privilege-separated server can ask its monitor for a
 * signature (struct ssh_transport_ops).
 *
 * The order of one exchange (both sides send KEXINIT after receiving the
 * peer's identification line, or when re-keying):
 *
 *   client: KEXINIT out, KEXINIT in, KEX_HYBRID_INIT out,
 *           KEX_HYBRID_REPLY in (check the host key with ops->check_host_key,
 *           verify the signature over H), NEWKEYS out, NEWKEYS in
 *   server: KEXINIT out, KEXINIT in, KEX_HYBRID_INIT in, KEX_HYBRID_REPLY
 *           out (signature from ops->sign), NEWKEYS out, NEWKEYS in
 *
 * After sending NEWKEYS the sending direction uses the new key; after
 * receiving it, the receiving direction does; with strict key exchange
 * each direction's sequence number is set to 0 at that point (3.3).  The
 * H of the first exchange is the session identifier (7.2) and never
 * changes.  From the moment this side sends KEXINIT until it sends
 * NEWKEYS, in every exchange whoever started it, upper-layer messages
 * given to ssh_transport_send are held and sent after this side's NEWKEYS
 * in order (RFC 4253 7.1 forbids sending them in between).  Upper-layer
 * messages the peer sends before its own KEXINIT are delivered; after its
 * KEXINIT and before its NEWKEYS they are a protocol error (answered with
 * UNIMPLEMENTED and dropped, as below).
 *
 * Strict key exchange (required, ssh-kex.h): until the initial exchange
 * has ended, the first received packet must be KEXINIT, and only KEXINIT,
 * NEWKEYS and the two hybrid messages are accepted, each the expected
 * number of times; anything else, or a sequence number that wraps, ends
 * the connection (EPROTO).
 *
 * Re-keying (RFC 4253 9): this side starts an exchange when either
 * direction has carried SSH_TRANSPORT_REKEY_BYTES bytes or
 * SSH_TRANSPORT_REKEY_PACKETS packets since its last key, or when
 * SSH_TRANSPORT_REKEY_SECONDS have passed (checked by
 * ssh_transport_tick); it answers an exchange the peer starts at any time
 * after the initial one.
 */

#ifndef SSH_SSH_TRANSPORT_H
#define SSH_SSH_TRANSPORT_H

#include "ssh-kex.h"
#include "ssh-packet.h"

/* The generic message numbers (RFC 4250 4.1.2). */
#define SSH_MSG_DISCONNECT		1
#define SSH_MSG_IGNORE			2
#define SSH_MSG_UNIMPLEMENTED		3
#define SSH_MSG_DEBUG			4
#define SSH_MSG_SERVICE_REQUEST		5
#define SSH_MSG_SERVICE_ACCEPT		6
#define SSH_MSG_EXT_INFO		7

/* The disconnect reason codes used (RFC 4250 4.2.2). */
#define SSH_DISCONNECT_PROTOCOL_ERROR			2
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED		3
#define SSH_DISCONNECT_MAC_ERROR			5
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE		7
#define SSH_DISCONNECT_PROTOCOL_VERSION_NOT_SUPPORTED	8
#define SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE		9
#define SSH_DISCONNECT_BY_APPLICATION			11
#define SSH_DISCONNECT_TOO_MANY_CONNECTIONS		12
#define SSH_DISCONNECT_AUTH_CANCELLED_BY_USER		13
#define SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE	14

/* The re-keying limits of RFC 4253 9 ("after each gigabyte ... or after each hour"). */
#define SSH_TRANSPORT_REKEY_BYTES	(1024UL * 1024UL * 1024UL)
#define SSH_TRANSPORT_REKEY_PACKETS	0x10000000UL
#define SSH_TRANSPORT_REKEY_SECONDS	3600U

/* The most bytes of upper-layer payloads held during an exchange (with their length fields). */
#define SSH_TRANSPORT_HELD_MAX	(4U * 1024U * 1024U)

/* The longest description kept from a peer's SSH_MSG_DISCONNECT. */
#define SSH_TRANSPORT_REASON_MAX	256U

/* The roles. */
enum ssh_role {
	SSH_ROLE_CLIENT = 0,
	SSH_ROLE_SERVER = 1
};

/* Where the connection is. */
enum ssh_transport_state {
	/* Waiting for the peer's identification line. */
	SSH_TRANSPORT_VERSION = 0,

	/* The initial key exchange is in progress. */
	SSH_TRANSPORT_INITIAL_KEX = 1,

	/* Keys are in use; a re-exchange may be in progress (kex_step not 0). */
	SSH_TRANSPORT_RUNNING = 2,

	/* A DISCONNECT was sent or received, or a fatal error happened; only pending output remains. */
	SSH_TRANSPORT_CLOSED = 3
};

/*
 * The host key operations.
 *
 * A server sets host_key and sign: host_key appends the server's public
 * key blob (ssh-key.h) to out; sign appends the signature blob of the
 * 32-byte exchange hash h made with the host key.  A client sets
 * check_host_key: it gets the server's key blob and answers 0 to accept
 * it, EACCES to refuse (the transport then disconnects with
 * SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE), or another error.  The
 * transport verifies the signature itself.  context is passed back.
 * Each returns 0 or an error the transport passes on.
 */
struct ssh_transport_ops {
	int (*host_key)(void *context, struct ssh_buf *out);
	int (*sign)(void *context, const uint8_t *h, struct ssh_buf *out);
	int (*check_host_key)(void *context, const uint8_t *blob, size_t size);
	void *context;
};

/*
 * The transport of one connection.
 *
 * The owner allocates it, calls ssh_transport_init and, at the end,
 * ssh_transport_free.  Fields are the implementation's; the callers read
 * only state, session_id (once have_session_id is 1), reason and
 * reason_code.  kex_step is the position in the exchange in progress (0
 * for none; the values are the .c file's).  version_local and
 * version_peer are the identification lines without CR LF (kept for the
 * exchange hash of every re-exchange).  kexinit_local and kexinit_peer are
 * the KEXINIT payloads of the exchange in progress.  hybrid is the
 * client's half while it waits for the reply.  k and h are kept between
 * computing them and the NEWKEYS that uses them (secret).  held keeps
 * upper-layer payloads sent during an exchange (each as uint32 length and
 * the bytes).  kex_time is the tick time of the last key.
 */
struct ssh_transport {
	enum ssh_role role;
	enum ssh_transport_state state;
	struct ssh_transport_ops ops;
	struct ssh_packet packet;
	char version_local[SSH_VERSION_LINE_MAX];
	char version_peer[SSH_VERSION_LINE_MAX];
	unsigned prelude_lines;
	size_t prelude_bytes;
	int kex_step;
	int initial_done;
	struct ssh_kex_choice choice;
	struct ssh_buf kexinit_local;
	struct ssh_buf kexinit_peer;
	struct ssh_hybrid_client hybrid;
	uint8_t k[SSH_KEX_HASH_SIZE];
	uint8_t h[SSH_KEX_HASH_SIZE];
	uint8_t session_id[SSH_KEX_HASH_SIZE];
	int have_session_id;
	struct ssh_buf held;
	uint64_t kex_time;
	uint64_t now;
	uint32_t reason_code;
	char reason[SSH_TRANSPORT_REASON_MAX];
};

/*
 * Starts a transport for role with ops (copied) and queues this side's
 * identification line (SSH_VERSION_STRING and CR LF) in the output.
 *
 * now is the current time in seconds of a monotonic clock.  Returns 0 or
 * ENOMEM (the transport is then freed).
 */
int ssh_transport_init(struct ssh_transport *transport, enum ssh_role role, const struct ssh_transport_ops *ops, uint64_t now);

/* Wipes every secret (keys, k, h, the client's half, held payloads) and frees the memory. */
void ssh_transport_free(struct ssh_transport *transport);

/*
 * Appends size received bytes.  Returns 0, EMSGSIZE (stop reading until
 * ssh_transport_next has consumed input), or ENOMEM.
 */
int ssh_transport_input(struct ssh_transport *transport, const uint8_t *data, size_t size);

/*
 * Processes received input until one upper-layer message is ready or the
 * input is used up.
 *
 * Handles the identification line (the client skips up to
 * SSH_VERSION_PRELUDE_LINES other lines, SSH_VERSION_PRELUDE_BYTES in
 * all; the line must start with "SSH-2.0-" or "SSH-1.99-", else the
 * connection ends with EPROTO; a server takes no other line), the key
 * exchange messages, IGNORE, DEBUG, UNIMPLEMENTED and EXT_INFO
 * (dropped; these programs do not ask for EXT_INFO), and DISCONNECT (state
 * CLOSED, reason_code and reason kept, returns ECONNABORTED).  A key
 * exchange message (21, 30 to 49) that does not fit the step of the
 * exchange in progress, or arrives with no exchange in progress, is
 * EPROTO.  A message number outside every layer (8 to 19, 22 to
 * 29, 50 or more while a key exchange of the peer is in progress, 128 to
 * 191) is answered with SSH_MSG_UNIMPLEMENTED carrying its sequence
 * number (RFC 4253 11.4) and dropped; inside an initial strict exchange it
 * is EPROTO instead.
 *
 * On 0, payload (reset first) holds one upper-layer message, message
 * number first.  Returns 0, EAGAIN (nothing more until more input),
 * ECONNABORTED, or a fatal error (EPROTO, EINVAL, EBADMSG, EMSGSIZE,
 * ENOTSUP, EACCES, ENOMEM, EIO); after a fatal error a DISCONNECT with the
 * matching reason is queued, the state is CLOSED, and the caller writes
 * the pending output and closes.
 */
int ssh_transport_next(struct ssh_transport *transport, struct ssh_buf *payload);

/*
 * Sends one upper-layer message (message number first; 5, 6, or 50 and
 * above).  While this side's exchange is in progress it is held and sent
 * after this side's NEWKEYS, in order.
 *
 * Returns 0; EINVAL (empty, a number the transport owns, or a payload
 * longer than ssh_packet_send takes: a caller's mistake); ENOBUFS when the
 * output (SSH_PACKET_OUTPUT_MAX) or the held messages
 * (SSH_TRANSPORT_HELD_MAX) are full: nothing is queued, and the caller
 * sends again after writing pending output (back-pressure, not an error of
 * the connection); EPIPE (state CLOSED); ENOMEM or EIO.
 */
int ssh_transport_send(struct ssh_transport *transport, const uint8_t *payload, size_t size);

/*
 * Gives the current time (seconds, monotonic) and starts a re-exchange
 * when a limit is reached and none is in progress (state RUNNING).
 * Returns 0 or an error of sending KEXINIT.
 */
int ssh_transport_tick(struct ssh_transport *transport, uint64_t now);

/*
 * Queues SSH_MSG_DISCONNECT with code and description (NUL-terminated,
 * cut to SSH_TRANSPORT_REASON_MAX) and sets the state CLOSED.  Further
 * sends fail with EPIPE.
 */
void ssh_transport_disconnect(struct ssh_transport *transport, uint32_t code, const char *description);

/* Returns the bytes waiting to be written (as ssh_packet_pending). */
const uint8_t *ssh_transport_pending(const struct ssh_transport *transport, size_t *size);

/* Removes size written bytes from the output. */
void ssh_transport_written(struct ssh_transport *transport, size_t size);

/*
 * Appends everything another process needs to go on with this connection
 * after the initial exchange and authentication (a privilege-separated
 * server hands its connection from the pre-authentication process to the
 * session process): role, both identification lines, the session
 * identifier, the strict flag, both directions' keys, sequence numbers and
 * counters, the time of the last key, and the received bytes not yet
 * processed.  Only allowed in state RUNNING with no exchange in progress
 * and no held or pending output (EBUSY otherwise).
 *
 * out must be secret.  Returns 0, EBUSY or the error recorded in out.
 */
int ssh_transport_export(const struct ssh_transport *transport, struct ssh_buf *out);

/*
 * Rebuilds a transport from size bytes made by ssh_transport_export, with
 * new ops.  The bytes come from the other process of the pair and are
 * checked as untrusted (EINVAL on any inconsistency: a length, a role, a
 * state).  now as for ssh_transport_init.  Returns 0, EINVAL or ENOMEM;
 * the caller wipes data afterwards.
 */
int ssh_transport_import(struct ssh_transport *transport, const uint8_t *data, size_t size, const struct ssh_transport_ops *ops, uint64_t now);

#endif
