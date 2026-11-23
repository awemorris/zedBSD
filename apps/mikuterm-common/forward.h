/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The port forwardings of one SSH connection of mikuterm (forward.c):
 * ssh_config's LocalForward and RemoteForward, as OpenSSH's ssh does them
 * with -L and -R.
 *
 * The connection's network thread owns it, as it owns the engine.  Once
 * the user is authenticated, forward_start listens for each local
 * forwarding and asks the server for each remote one; the thread then
 * puts the sockets into its wait (forward_waits), lets them act on what
 * the wait found (forward_serve), hands over the engine's events of the
 * forwarded channels (forward_event), and moves the bytes read into the
 * engine each round (forward_pump).  The engine owns the channels and
 * their windows; the sockets and the bytes between them and the channels
 * are here.
 *
 * What the user needs to know (a port that could not be listened on, a
 * remote forwarding the server refused, the port the server chose for
 * port 0) is said through the say function, as the short lines OpenSSH's
 * ssh writes on its standard error.
 *
 * It does not include a header of Windows: the listening sockets are made
 * exclusive on Windows (SO_EXCLUSIVEADDRUSE) inside forward.c, and the
 * rest is libkeiland's sockets, so the tests run it on the host.
 */

#ifndef MIKUTERM_FORWARD_H
#define MIKUTERM_FORWARD_H

#include <keiland/keiland_socket.h>

#include "userland/base/ssh/ssh-buf.h"
#include "userland/base/ssh/ssh-client-engine.h"
#include "userland/base/ssh/ssh-config.h"

#include <stddef.h>
#include <stdint.h>

/* The most addresses one local forwarding listens on (a name may give several). */
#define FORWARD_LISTEN_ADDRESSES	4U

/* The most addresses of a remote forwarding's destination tried. */
#define FORWARD_CONNECT_ADDRESSES	8U

/* The most listening sockets of a connection. */
#define FORWARD_LISTENERS_MAX		(SSH_CONFIG_FORWARDS_MAX * FORWARD_LISTEN_ADDRESSES)

/* The most forwarded connections at once (as many as the engine opens channels for). */
#define FORWARD_LINKS_MAX		SSH_ENGINE_FORWARD_CHANNELS_MAX

/* The most waits forward_waits fills. */
#define FORWARD_WAITS_MAX		(FORWARD_LISTENERS_MAX + FORWARD_LINKS_MAX)

/* The bytes read from a forwarded connection's socket at once. */
#define FORWARD_CHUNK			16384U

/*
 * The most of the server's bytes a forwarded connection holds for its
 * socket: the engine gives the window back only for what was written, so
 * a server that keeps to its window never sends more than one and a half
 * of it.
 */
#define FORWARD_QUEUE_MAX		(SSH_ENGINE_FORWARD_WINDOW + SSH_ENGINE_FORWARD_WINDOW / 2U)

/* How long a connection to a remote forwarding's destination may take, in milliseconds (all its addresses). */
#define FORWARD_CONNECT_WAIT_MS		30000U

/* How long a listening socket rests after the system ran out of descriptors, in milliseconds. */
#define FORWARD_ACCEPT_PAUSE_MS		1000U

/* The connections a listening socket holds before they are taken. */
#define FORWARD_LISTEN_BACKLOG		128U

/* How long a bind of a port still held by the connections of the last attempt waits before it tries once more, in milliseconds. */
#define FORWARD_REBIND_WAIT_MS		2000U

/* The longest line forward_start leaves for the connection's end, and the longest line said. */
#define FORWARD_LINE_MAX		(2U * SSH_CONFIG_HOST_MAX + 128U)

/* forward_start's and forward_event's answer when ExitOnForwardFailure ends the connection. */
#define FORWARD_FATAL			(-1)

/* What a forwarded connection is doing. */
enum forward_link_state {
	/* The entry holds no connection. */
	FORWARD_LINK_FREE = 0,

	/* A local connection waits for the server's answer to its direct-tcpip channel. */
	FORWARD_LINK_OPENING = 1,

	/* A connection to a remote forwarding's destination is on its way. */
	FORWARD_LINK_CONNECTING = 2,

	/* The connection carries bytes both ways. */
	FORWARD_LINK_OPEN = 3,

	/* The socket is closed and the channel's close waits for the server's. */
	FORWARD_LINK_CLOSING = 4
};

/*
 * One socket that listens for a local forwarding: the forwarding (its
 * place in the settings' list), the time it rests until after the system
 * ran out of descriptors (0: it does not rest), and its place in the
 * waits of this round (-1: none).
 */
struct forward_listener {
	struct kl_socket *socket;
	unsigned forward;
	uint64_t paused_until;
	int wait;
};

/*
 * One remote forwarding asked of the server: its place in the settings'
 * list, and the number the engine names it by.
 */
struct forward_remote {
	unsigned forward;
	unsigned number;
};

/*
 * One forwarded connection: its socket and its channel, the forwarding
 * it belongs to, the bytes read from the socket not yet taken by the
 * channel (chunk), the server's bytes not yet written to the socket
 * (queue from queue_offset), how each end has ended, and, while the
 * connection to a remote forwarding's destination is made, the addresses
 * left to try and the time it gives up.
 */
struct forward_link {
	enum forward_link_state state;
	struct kl_socket *socket;
	uint32_t channel;
	unsigned forward;
	uint8_t chunk[FORWARD_CHUNK];
	size_t chunk_size;
	size_t chunk_offset;
	int socket_ended;
	int eof_sent;
	struct ssh_buf queue;
	size_t queue_offset;
	int channel_ended;
	int shut;
	int channel_gone;
	struct kl_address addresses[FORWARD_CONNECT_ADDRESSES];
	size_t address_count;
	size_t next_address;
	uint64_t deadline;
	int wait;
};

/*
 * The forwardings of one connection.
 *
 * engine and config are the connection's (not owned).  say shows a line
 * (no line end) to the user, with data.  listening is 1 from
 * forward_start until the session ended (forward_stop_listening).
 * rebind_wait is 1 when a bind that finds its port in use waits once
 * before it tries again (an attempt after a failed one, whose closed
 * connections may still hold the port on Windows).  fatal holds the line
 * that ends the connection when FORWARD_FATAL is answered.
 */
struct forward_state {
	struct ssh_engine *engine;
	const struct ssh_config *config;
	void (*say)(void *data, const char *line);
	void *data;
	int listening;
	int rebind_wait;
	struct forward_listener listeners[FORWARD_LISTENERS_MAX];
	unsigned listener_count;
	struct forward_remote remotes[SSH_CONFIG_FORWARDS_MAX];
	unsigned remote_count;
	struct forward_link links[FORWARD_LINKS_MAX];
	char fatal[FORWARD_LINE_MAX];
};

void forward_init(struct forward_state *forwards, struct ssh_engine *engine, const struct ssh_config *config, void (*say)(void *data, const char *line), void *data);
int forward_start(struct forward_state *forwards);
void forward_stop_listening(struct forward_state *forwards);
int forward_event(struct forward_state *forwards, const struct ssh_engine_event *event, uint64_t now_ms, int *taken);
void forward_pump(struct forward_state *forwards);
size_t forward_waits(struct forward_state *forwards, struct kl_socket_wait *waits, size_t capacity, uint64_t now_ms, uint64_t *wake_ms);
void forward_serve(struct forward_state *forwards, const struct kl_socket_wait *waits, uint64_t now_ms);
unsigned forward_open_count(const struct forward_state *forwards);
void forward_free(struct forward_state *forwards);

#endif
