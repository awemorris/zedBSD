/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The port forwardings sshd's session process serves (RFC 4254 7): the
 * client's "direct-tcpip" channels, which sshd connects to their
 * destination (ssh -L), and the client's "tcpip-forward" requests, for
 * which sshd listens and opens a "forwarded-tcpip" channel to the client
 * for each connection (ssh -R), with "cancel-tcpip-forward".
 *
 * Everything runs in the session process after it became the user, so a
 * forwarding binds and connects with the user's rights; the monitor never
 * sees a forwarded byte or socket.  What sshd decides on top of that is
 * the administrator's policy (sshd_config(5) AllowTcpForwarding,
 * DisableForwarding, GatewayPorts, PermitOpen, PermitListen, with
 * OpenSSH's defaults) and the privileged ports: a port below 1024 is
 * listened on only for root, whatever the kernel would allow.
 *
 * A forwarded channel's window is SSHD_FORWARD_WINDOW.  The client's
 * bytes wait in a queue until its socket takes them, and the window is
 * given back only for what was written; a socket is read only while its
 * channel has room and the connection's output is below
 * SSHD_FORWARD_OUTPUT_SOFT, so a slow side stops the other.  A message
 * that cannot be queued for the client ends the connection, never drops
 * the data silently.
 */

#ifndef SSH_SSHD_FORWARD_H
#define SSH_SSHD_FORWARD_H

#include <netdb.h>
#include <poll.h>
#include <sys/types.h>

#include "ssh-channel.h"
#include "ssh-config.h"
#include "ssh-transport.h"

/* The most remote forwardings (tcpip-forward) one connection keeps. */
#define SSHD_FORWARD_LISTEN_MAX		16U

/* The most sockets one remote forwarding listens on (the addresses its bind address names). */
#define SSHD_FORWARD_ADDRESSES		4U

/* The window of a forwarded channel (512 KiB, as the client's). */
#define SSHD_FORWARD_WINDOW		(512U * 1024U)

/* The most bytes of the client's data kept for one socket: a window and a packet. */
#define SSHD_FORWARD_QUEUE_MAX		(SSHD_FORWARD_WINDOW + SSH_CHANNEL_PACKET)

/* The most bytes read from a forwarded socket at once. */
#define SSHD_FORWARD_CHUNK		16384U

/* The connection's pending output above which no forwarded socket is read (the session's own gate). */
#define SSHD_FORWARD_OUTPUT_SOFT	(1024U * 1024U)

/* The longest a connection to a direct-tcpip destination takes, in milliseconds. */
#define SSHD_FORWARD_CONNECT_MS		30000U

/* How long a listening socket rests after an accept failed for the lack of descriptors, in milliseconds. */
#define SSHD_FORWARD_PAUSE_MS		1000U

/* The backlog of a forwarding's listening socket (OpenSSH's SSH_LISTEN_BACKLOG, for local and remote forwardings alike). */
#define SSHD_FORWARD_BACKLOG		128

/* The poll slots the forwardings take at most: every listening socket and one socket per channel. */
#define SSHD_FORWARD_SLOTS		(SSHD_FORWARD_LISTEN_MAX * SSHD_FORWARD_ADDRESSES + SSH_CHANNEL_MAX)

/*
 * What one forwarded channel is doing.
 *
 * A direct-tcpip channel connects to its destination in CONNECTING while
 * the client waits for the answer; a forwarded-tcpip channel waits in
 * OPENING for the client's answer.  Both carry bytes in OPEN, and stay in
 * CLOSING once this side sent CLOSE, until the client's CLOSE frees the
 * slot.
 */
enum sshd_forward_state {
	SSHD_FORWARD_FREE = 0,
	SSHD_FORWARD_CONNECTING = 1,
	SSHD_FORWARD_OPENING = 2,
	SSHD_FORWARD_OPEN = 3,
	SSHD_FORWARD_CLOSING = 4
};

/*
 * One forwarded channel and its socket.
 *
 * channel is the channel of the table (its slot is this entry's index).
 * socket is the local end (-1 before and after).  queue holds the
 * client's bytes not yet written to the socket (from queue_offset on).
 * socket_ended is 1 once the socket reached its end (and EOF went to the
 * client), client_ended once the client sent EOF, shut once the socket's
 * sending side was shut after the client's last byte.  client_closed is 1
 * once the client sent CLOSE: this side's CLOSE follows once the queue is
 * written.  addresses and next_address are the destination's addresses
 * and the next one to try while CONNECTING, until deadline (milliseconds
 * of the monotonic clock).  slot is the socket's place in the poll array
 * of the last turn (-1 when it was not watched).
 */
struct sshd_forward_channel {
	enum sshd_forward_state state;
	struct ssh_channel *channel;
	int socket;
	struct ssh_buf queue;
	size_t queue_offset;
	int socket_ended;
	int client_ended;
	int shut;
	int client_closed;
	struct addrinfo *addresses;
	struct addrinfo *next_address;
	uint64_t deadline;
	int slot;
};

/*
 * One remote forwarding: the address the client asked for (as it sent
 * it: forwarded-tcpip channels name it so, and cancel-tcpip-forward finds
 * it by it), the port listened on, and the listening sockets
 * (socket_count of them).  paused_until is when sockets that failed to
 * accept for the lack of descriptors are watched again (0 when not
 * paused).  slots are the sockets' places in the poll array of the last
 * turn (-1 when not watched).
 */
struct sshd_forward_listen {
	int in_use;
	char address[SSH_CONFIG_HOST_MAX + 1];
	uint16_t port;
	int sockets[SSHD_FORWARD_ADDRESSES];
	int slots[SSHD_FORWARD_ADDRESSES];
	unsigned socket_count;
	uint64_t paused_until;
};

/*
 * The forwardings of one connection.
 *
 * config is the settings, uid the user's (0 may listen on privileged
 * ports).  channels and transport are the connection's, shared with the
 * session's own channels (a slot is a session's or a forwarding's).
 * message is the scratch buffer each outgoing message is built in.
 * channel_limit is the most forwarded channels open at once, so that
 * MaxSessions sessions always find a slot.
 */
struct sshd_forwards {
	const struct sshd_config *config;
	uid_t uid;
	struct ssh_channels *channels;
	struct ssh_transport *transport;
	struct ssh_buf message;
	unsigned channel_limit;
	struct sshd_forward_listen listens[SSHD_FORWARD_LISTEN_MAX];
	struct sshd_forward_channel entries[SSH_CHANNEL_MAX];
};

/* Starts the forwardings of a connection: nothing listens and no channel is forwarded. */
void sshd_forward_init(struct sshd_forwards *forwards, const struct sshd_config *config, uid_t uid, struct ssh_channels *channels, struct ssh_transport *transport);

/* Closes every socket of the forwardings and frees what they hold. */
void sshd_forward_free(struct sshd_forwards *forwards);

/*
 * Takes a GLOBAL_REQUEST when it is "tcpip-forward" or
 * "cancel-tcpip-forward" (*handled 1) and answers it; any other is left
 * to the caller (*handled 0).  Returns 0, EINVAL (malformed) or the error
 * of queueing the answer.
 */
int sshd_forward_global(struct sshd_forwards *forwards, const uint8_t *payload, size_t size, int *handled);

/* Reports whether a CHANNEL_OPEN is of type "direct-tcpip" (1) or not (0). */
int sshd_forward_is_direct(const uint8_t *payload, size_t size);

/*
 * Takes a CHANNEL_OPEN "direct-tcpip": refuses it by the policy, or
 * starts the connection to its destination and answers once it is made
 * or failed.  Returns 0, EINVAL or EPROTO (malformed), or the error of
 * queueing the answer.
 */
int sshd_forward_open(struct sshd_forwards *forwards, const uint8_t *payload, size_t size);

/*
 * Takes OPEN_CONFIRMATION or OPEN_FAILURE for a forwarded-tcpip channel
 * this side opened.  Returns 0, EINVAL or EPROTO.
 */
int sshd_forward_open_reply(struct sshd_forwards *forwards, const uint8_t *payload, size_t size);

/* Reports whether channel is a forwarded one (1) or not (0). */
int sshd_forward_owns(const struct sshd_forwards *forwards, const struct ssh_channel *channel);

/*
 * Takes size bytes of the client's data (extended the type, 0 for data)
 * for a forwarded channel, already taken from its window.  Returns 0 or
 * the error of queueing a message.
 */
int sshd_forward_data(struct sshd_forwards *forwards, struct ssh_channel *channel, const uint8_t *data, size_t size, uint32_t extended);

/*
 * Takes the client's EOF (close 0) or CLOSE (close 1) of a forwarded
 * channel, already read.  Returns 0 or the error of queueing a message.
 */
int sshd_forward_end(struct sshd_forwards *forwards, struct ssh_channel *channel, int close);

/*
 * Sends what is due: the EOFs, shuts and closes of the forwarded
 * channels, and the answer to a connection that ran out of time.
 * now_ms is the monotonic clock in milliseconds.  Returns 0 or the error
 * of queueing a message.
 */
int sshd_forward_pump(struct sshd_forwards *forwards, uint64_t now_ms);

/*
 * Puts the sockets the forwardings wait on into slots (at most capacity
 * of them) and returns how many.  pending is the connection's output not
 * yet written with the messages held during a key re-exchange (a socket
 * is read only while it is below SSHD_FORWARD_OUTPUT_SOFT, far from the
 * transport's limits, so the data a read sends is always taken);
 * *timeout_ms is lowered to the time a paused listener or a connection's
 * deadline wants the loop back.
 */
unsigned sshd_forward_poll(struct sshd_forwards *forwards, struct pollfd *slots, unsigned capacity, size_t pending, uint64_t now_ms, int *timeout_ms);

/*
 * Acts on what poll reported for the slots sshd_forward_poll filled.
 * Returns 0 or the error of queueing a message.
 */
int sshd_forward_serve(struct sshd_forwards *forwards, const struct pollfd *slots, uint64_t now_ms);

/* Returns the monotonic clock in milliseconds. */
uint64_t sshd_forward_now_ms(void);

#endif
