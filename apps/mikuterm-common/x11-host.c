/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * mikuterm's side of the X server of one SSH connection: the link to the
 * child process, its start and end, and the X clients' channels between
 * the engine and the link.  The same on every system: the system's own
 * part, which starts and ends the process, is x11-spawn.h's.
 */

#include "x11-tab.h"
#include "x11-host.h"
#include "x11-spawn.h"

#include "userland/base/ssh/ssh-client-engine.h"
#include <keiland/keiland_socket.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bytes one channel may hold each way (the engine's 1.5 windows in, the child's credit out, with room). */
#define HOST_TO_CHILD_MAX	(256U * 1024U)
#define HOST_TO_REMOTE_MAX	(X11_LINK_CREDIT_TO_PARENT)

/* The frames waiting for the socket, at most (the link's credit keeps it far below). */
#define HOST_OUT_MAX		(8U * 1024U * 1024U)

/* What is read of the socket at once, and the reading buffer (a whole frame with room). */
#define HOST_READ		65536U
#define HOST_IN_MAX		(X11_LINK_HEADER + X11_LINK_BODY_MAX + HOST_READ)

/* How long the child has to say HELLO, how often it is pinged, how long its PONG may take (ms). */
#define HOST_HELLO_MS		10000U
#define HOST_PING_MS		5000U
#define HOST_PONG_MS		10000U

/* How many times one connection starts an X server. */
#define HOST_STARTS_MAX		3U

/* The HELLO's settings: the version, the scale, the flags, the cookie and the root window's size, then the host's name. */
#define HOST_HELLO_ROOT		(12U + X11_HOST_COOKIE)
#define HOST_HELLO_FIXED	(HOST_HELLO_ROOT + 8U)

/* The scales the child's tabs may have, in percent (the X server's). */
#define HOST_SCALE_MIN		100U
#define HOST_SCALE_MAX		400U

static int host_start(struct x11_host *host, uint64_t now);
static int host_pair(struct x11_host *host, struct kl_socket **child_end);
static void host_fail(struct x11_host *host, struct ssh_engine *engine, const char *why);
static int host_frame(struct x11_host *host, unsigned type, uint32_t id, const void *body, uint32_t size);
static int host_receive(struct x11_host *host, struct ssh_engine *engine, uint64_t now, const char **why);
static int host_take(struct x11_host *host, struct ssh_engine *engine, const struct x11_link_frame *frame, const char **why);
static void host_move(struct x11_host *host, struct ssh_engine *engine);
static int host_send(struct x11_host *host);
static struct x11_host_channel *host_by_channel(struct x11_host *host, uint32_t channel);
static struct x11_host_channel *host_by_id(struct x11_host *host, uint32_t id);
static void host_free(struct x11_host *host, struct x11_host_channel *slot);
static void host_stop_child(struct x11_host *host);
static int host_tab_take(struct x11_host *host, const struct x11_link_frame *frame, const char **why);
static int host_tab_open(struct x11_host *host, const struct x11_link_frame *frame, const char **why);
static int host_tab_pixels(struct x11_tab *tab, const struct x11_link_frame *frame, const char **why);
static unsigned host_tab_find(const struct x11_host *host, uint32_t window);
static void host_tab_drop(struct x11_host *host, unsigned place);
static void host_tabs_move(struct x11_host *host);
static void host_tab_input(struct x11_host *host, const struct x11_tab *tab, const struct x11_tab_input *input);
static void host_tabs_end(struct x11_host *host);

/*
 * Starts a connection's X server state: nothing runs until the first X
 * client comes.
 */
void
x11_host_init(
	struct x11_host *host,
	const struct x11_host_settings *settings)
{
	/* No child yet. */
	memset(host, 0, sizeof(*host));
	host->settings = *settings;
	x11_link_table_init(&host->table, X11_LINK_PARENT);
}

/*
 * Ends a connection's X server: the child is asked to end (SHUTDOWN) and
 * ended if it has not after a moment; the channels are closed on the
 * connection if it is still there (engine not NULL).
 */
void
x11_host_end(
	struct x11_host *host,
	struct ssh_engine *engine)
{
	unsigned i;

	/* Closes the channels left. */
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (!host->channels[i].used)
			continue;
		if (engine != NULL)
			(void)ssh_engine_channel_close(engine, host->channels[i].channel);
		host_free(host, &host->channels[i]);
	}

	/* Asks the child to end, and ends it. */
	if (host->socket != NULL) {
		(void)host_frame(host, X11_LINK_SHUTDOWN, 0, NULL, 0);
		(void)host_send(host);
	}
	host_stop_child(host);
	free(host->out);
	free(host->in);
	host->out = NULL;
	host->in = NULL;
}

/*
 * Takes an X client the server opens (SSH_ENGINE_X11_OPEN's channel):
 * the child is started when none runs, and a link id is opened for the
 * channel.  Returns 0 (answer the open with accept), or an errno value
 * (refuse it): EMFILE when the connection has X11_LINK_CHANNELS clients,
 * EAGAIN when the X server could not start or has failed too often.
 */
int
x11_host_accept(
	struct x11_host *host,
	uint32_t channel,
	uint64_t now)
{
	struct x11_link_channel *link;
	struct x11_host_channel *slot;
	unsigned i;
	int error;

	/* Starts the X server if it is not running. */
	if (host->socket == NULL) {
		if (host->starts >= HOST_STARTS_MAX)
			return EAGAIN;
		error = host_start(host, now);
		if (error != 0)
			return EAGAIN;
	}

	/* A free slot, and a link id for it. */
	slot = NULL;
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (!host->channels[i].used) {
			slot = &host->channels[i];
			break;
		}
	}
	if (slot == NULL)
		return EMFILE;
	error = x11_link_open(&host->table, 0, &link);
	if (error != 0)
		return EMFILE;

	/* The channel's buffers. */
	memset(slot, 0, sizeof(*slot));
	slot->to_child = malloc(HOST_TO_CHILD_MAX);
	slot->to_remote = malloc(HOST_TO_REMOTE_MAX);
	if (slot->to_child == NULL || slot->to_remote == NULL) {
		free(slot->to_child);
		free(slot->to_remote);
		x11_link_close(&host->table, link->id);
		return ENOMEM;
	}
	slot->used = 1;
	slot->channel = channel;
	slot->id = link->id;

	/* Tells the child. */
	error = host_frame(host, X11_LINK_OPEN, slot->id, NULL, 0);
	if (error != 0) {
		host_free(host, slot);
		return ENOMEM;
	}

	/* Succeeded: the engine may open the channel. */
	return 0;
}

/*
 * Takes bytes the remote X client sent (SSH_ENGINE_DATA of an X11
 * channel): they wait for the child's credit.  Bytes past what a channel
 * holds (the engine's window keeps them within it) close the channel.
 */
void
x11_host_data(
	struct x11_host *host,
	struct ssh_engine *engine,
	uint32_t channel,
	const uint8_t *bytes,
	size_t size)
{
	struct x11_host_channel *slot;

	/* Only a channel the X server has. */
	slot = host_by_channel(host, channel);
	if (slot == NULL)
		return;

	/* More than the window allows: the channel is closed. */
	if (size > HOST_TO_CHILD_MAX - slot->to_child_size) {
		(void)ssh_engine_channel_close(engine, channel);
		(void)host_frame(host, X11_LINK_CLOSE, slot->id, NULL, 0);
		host_free(host, slot);
		return;
	}

	/* Keeps them for the child. */
	memcpy(slot->to_child + slot->to_child_size, bytes, size);
	slot->to_child_size += size;
}

/*
 * Takes the remote X client's EOF: it goes to the child after the bytes
 * before it.
 */
void
x11_host_eof(
	struct x11_host *host,
	uint32_t channel)
{
	struct x11_host_channel *slot;

	/* Marks the end of the remote side. */
	slot = host_by_channel(host, channel);
	if (slot != NULL)
		slot->remote_eof = 1;
}

/*
 * Takes the close of an X11 channel (SSH_ENGINE_CHANNEL_CLOSED): the
 * child closes its client.
 */
void
x11_host_closed(
	struct x11_host *host,
	uint32_t channel)
{
	struct x11_host_channel *slot;

	/* Tells the child, and frees the slot. */
	slot = host_by_channel(host, channel);
	if (slot == NULL)
		return;
	if (!slot->child_closed)
		(void)host_frame(host, X11_LINK_CLOSE, slot->id, NULL, 0);
	host_free(host, slot);
}

/*
 * Does a round of the X server's work: reads what the child sent, moves
 * the bytes both ways as the credit and the window allow, pings the child,
 * and sends the frames.  A child that failed is ended and its channels
 * closed.  Returns 0, or 1 when host->message has a line for the tab.
 */
int
x11_host_pump(
	struct x11_host *host,
	struct ssh_engine *engine,
	uint64_t now)
{
	const char *why;
	int error;

	/* Nothing runs. */
	host->message[0] = '\0';
	if (host->socket == NULL)
		return 0;

	/* What the child sent. */
	why = NULL;
	error = host_receive(host, engine, now, &why);
	if (error != 0) {
		host_fail(host, engine, why);
		return 1;
	}

	/* The bytes both ways, and what the tabs have for the child. */
	host_move(host, engine);
	host_tabs_move(host);

	/* A child that never said HELLO, or stopped answering. */
	if (!host->hello && now - host->started_ms > HOST_HELLO_MS) {
		host_fail(host, engine, "it did not start");
		return 1;
	}
	if (host->ping_waiting && now - host->ping_ms > HOST_PONG_MS) {
		host_fail(host, engine, "it stopped answering");
		return 1;
	}
	if (host->hello && !host->ping_waiting && now - host->heard_ms > HOST_PING_MS) {
		(void)host_frame(host, X11_LINK_PING, 0, NULL, 0);
		host->ping_ms = now;
		host->ping_waiting = 1;
	}

	/* The frames. */
	error = host_send(host);
	if (error != 0) {
		host_fail(host, engine, "its link failed");
		return 1;
	}

	/* Succeeded: a round with nothing to say. */
	return 0;
}

/*
 * Gives the link's socket to wait on (NULL: no child), and whether frames
 * wait for room to be sent.
 */
struct kl_socket *
x11_host_socket(
	const struct x11_host *host,
	int *writable)
{
	/* Frames wait while the socket had no room. */
	*writable = 0;
	if (host->out_size != 0)
		*writable = 1;

	/* The socket, or none. */
	return host->socket;
}

/*
 * Gives how many X clients are open (the connection stays while any is).
 */
int
x11_host_open_count(
	const struct x11_host *host)
{
	unsigned i;
	int count;

	/* Counts the used slots. */
	count = 0;
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (host->channels[i].used)
			count++;
	}

	/* The count. */
	return count;
}

/*
 * Gives the network thread the oldest X window's tab the child opened that
 * it has not taken yet, with the run's hold on it (NULL when none waits).
 */
struct x11_tab *
x11_host_new_tab(
	struct x11_host *host)
{
	struct x11_tab *tab;

	/* None waits. */
	if (host->fresh_count == 0U)
		return NULL;

	/* The oldest, out of the list. */
	tab = host->fresh[0];
	host->fresh_count--;
	memmove(&host->fresh[0], &host->fresh[1], host->fresh_count * sizeof(host->fresh[0]));

	/* Succeeded: the tab, for the session. */
	return tab;
}

/*
 * Starts the child: the socket pair, the process, and the HELLO with the
 * settings.  Returns 0, or an errno value.
 */
static int
host_start(
	struct x11_host *host,
	uint64_t now)
{
	struct kl_socket *child_end;
	uint8_t hello[HOST_HELLO_FIXED + X11_HOST_NAME_MAX];
	size_t name;
	int error;

	/* The buffers of the link. */
	host->starts++;
	if (host->in == NULL)
		host->in = malloc(HOST_IN_MAX);
	if (host->in == NULL)
		return ENOMEM;
	host->in_size = 0;
	host->out_size = 0;
	x11_link_table_init(&host->table, X11_LINK_PARENT);

	/* The loopback pair. */
	error = host_pair(host, &child_end);
	if (error != 0)
		return error;

	/*
	 * The process, given its end; mikuterm keeps its own descriptor of
	 * that end until the child says HELLO (where the system makes the
	 * child's copy only when the child opens it, it must still exist),
	 * unless the system's part closed it once the child held its copy.
	 */
	host->child_end = child_end;
	error = x11_spawn_start(host, child_end);
	if (error != 0) {
		host_stop_child(host);
		return error;
	}

	/* The settings. */
	name = strlen(host->settings.host);
	if (name >= X11_HOST_NAME_MAX)
		name = X11_HOST_NAME_MAX - 1U;
	x11_link_put32(hello, X11_LINK_VERSION);
	x11_link_put32(hello + 4, host->settings.scale);
	x11_link_put32(hello + 8, host->settings.flags);
	memcpy(hello + 12, host->settings.cookie, X11_HOST_COOKIE);
	x11_link_put32(hello + HOST_HELLO_ROOT, host->settings.root_width);
	x11_link_put32(hello + HOST_HELLO_ROOT + 4U, host->settings.root_height);
	memcpy(hello + HOST_HELLO_FIXED, host->settings.host, name);
	(void)host_frame(host, X11_LINK_HELLO, 0, hello, (uint32_t)(HOST_HELLO_FIXED + name));

	/* Succeeded: the child has HOST_HELLO_MS to answer. */
	host->hello = 0;
	host->started_ms = now;
	host->heard_ms = now;
	host->ping_waiting = 0;
	return 0;
}

/*
 * Makes the connected loopback pair: a listening socket of 127.0.0.1 that
 * no one else can share, a connection to it, the accepted end checked to
 * be that connection, and the listener closed.  The parent keeps the
 * accepted end (non-blocking); the child gets the other.  Returns 0, or an
 * errno value.
 */
static int
host_pair(
	struct x11_host *host,
	struct kl_socket **child_end)
{
	struct kl_socket *listener;
	struct kl_socket *connecting;
	struct kl_socket *accepted;
	struct kl_address address;
	struct kl_address peer;
	struct kl_address local;
	int nodelay;
	int error;

	/* The listener, on a free port of 127.0.0.1, its address not to be shared. */
	*child_end = NULL;
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0, &listener);
	if (error != 0)
		return error;
	x11_spawn_guard(listener);
	kl_address_loopback(KL_ADDRESS_IPV4, 0, &address);
	error = kl_socket_bind(listener, &address);
	if (error == 0)
		error = kl_socket_listen(listener, 1);
	if (error == 0)
		error = kl_socket_local_address(listener, &address);
	if (error != 0) {
		kl_socket_close(listener);
		return error;
	}

	/* The connection to it. */
	error = kl_socket_open(KL_ADDRESS_IPV4, KL_SOCKET_STREAM, 0, &connecting);
	if (error != 0) {
		kl_socket_close(listener);
		return error;
	}
	error = kl_socket_connect(connecting, &address);
	if (error == 0)
		error = kl_socket_local_address(connecting, &local);
	if (error != 0) {
		kl_socket_close(connecting);
		kl_socket_close(listener);
		return error;
	}

	/* The accepted end must be that connection, not another program's. */
	error = kl_socket_accept(listener, KL_SOCKET_NONBLOCKING, &accepted, &peer);
	kl_socket_close(listener);
	if (error != 0) {
		kl_socket_close(connecting);
		return error;
	}
	if (peer.port != local.port || memcmp(peer.bytes, local.bytes, 4) != 0) {
		kl_socket_close(accepted);
		kl_socket_close(connecting);
		return EACCES;
	}

	/*
	 * Small frames go at once both ways (each X request and reply would
	 * otherwise wait on Nagle and delayed ACKs); without it the link
	 * still works, slower, and that is logged.
	 */
	nodelay = kl_socket_set_option(accepted, KL_SOCKET_NO_DELAY, 1);
	if (nodelay != 0)
		printf("MIKUTERM X11 link no-delay failed end=parent errno=%d\n", nodelay);
	nodelay = kl_socket_set_option(connecting, KL_SOCKET_NO_DELAY, 1);
	if (nodelay != 0)
		printf("MIKUTERM X11 link no-delay failed end=child errno=%d\n", nodelay);

	/* Succeeded: the two ends. */
	host->socket = accepted;
	*child_end = connecting;
	return 0;
}

/*
 * Ends a child that failed: it is ended, every channel is closed on the
 * connection, and the tab is told why in host->message.
 */
static void
host_fail(
	struct x11_host *host,
	struct ssh_engine *engine,
	const char *why)
{
	unsigned i;
	unsigned closed;

	/* The channels go. */
	closed = 0;
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (!host->channels[i].used)
			continue;
		(void)ssh_engine_channel_close(engine, host->channels[i].channel);
		host_free(host, &host->channels[i]);
		closed++;
	}

	/* The child goes. */
	host_stop_child(host);
	if (why == NULL)
		why = "it failed";
	(void)snprintf(host->message, sizeof(host->message), "X11: the X server stopped (%s); %u X client%s closed.\r\n",
	    why, closed, closed == 1 ? " was" : "s were");
}

/*
 * Queues one frame for the child.  Returns 0, or ENOMEM when the frames
 * waiting would pass HOST_OUT_MAX.
 */
static int
host_frame(
	struct x11_host *host,
	unsigned type,
	uint32_t id,
	const void *body,
	uint32_t size)
{
	size_t needed;
	size_t capacity;
	uint8_t *grown;

	/* Room for it. */
	needed = host->out_size + X11_LINK_HEADER + size;
	if (needed > HOST_OUT_MAX)
		return ENOMEM;
	if (needed > host->out_capacity) {
		capacity = host->out_capacity * 2U;
		if (capacity < needed)
			capacity = needed + HOST_READ;
		grown = realloc(host->out, capacity);
		if (grown == NULL)
			return ENOMEM;
		host->out = grown;
		host->out_capacity = capacity;
	}

	/* The header and the body. */
	x11_link_header(host->out + host->out_size, type, id, size);
	if (size != 0)
		memcpy(host->out + host->out_size + X11_LINK_HEADER, body, size);
	host->out_size = needed;
	return 0;
}

/*
 * Reads what the child sent and takes each whole frame.  Returns 0, or
 * EPIPE with *why when the child's link ended or broke the rules.
 */
static int
host_receive(
	struct x11_host *host,
	struct ssh_engine *engine,
	uint64_t now,
	const char **why)
{
	struct x11_link_frame frame;
	size_t received;
	size_t used;
	size_t offset;
	int error;

	/* Reads until nothing waits. */
	for (;;) {
		if (HOST_IN_MAX - host->in_size < HOST_READ)
			break;
		error = kl_socket_receive(host->socket, host->in + host->in_size, HOST_READ, &received);
		if (error == EAGAIN)
			break;
		if (error != 0 || received == 0) {
			*why = "it ended";
			return EPIPE;
		}
		host->in_size += received;
		host->heard_ms = now;
	}

	/* Takes each whole frame. */
	offset = 0;
	for (;;) {
		error = x11_link_parse(host->in + offset, host->in_size - offset, &frame, &used);
		if (error == EAGAIN)
			break;
		if (error != 0) {
			*why = "it broke the link's rules";
			return EPIPE;
		}
		error = host_take(host, engine, &frame, why);
		if (error != 0)
			return EPIPE;
		offset += used;
	}

	/* Keeps the part of a frame not yet whole. */
	if (offset != 0) {
		memmove(host->in, host->in + offset, host->in_size - offset);
		host->in_size -= offset;
	}
	return 0;
}

/*
 * Takes one frame of the child's: HELLO and PONG, an X client's data, EOF
 * and close, and credit.  Returns 0, or EPIPE with *why for a frame
 * against the rules.
 */
static int
host_take(
	struct x11_host *host,
	struct ssh_engine *engine,
	const struct x11_link_frame *frame,
	const char **why)
{
	struct x11_host_channel *slot;
	int verdict;
	int error;

	/* The rules first. */
	verdict = x11_link_check(&host->table, frame);
	if (verdict == X11_LINK_VIOLATION) {
		*why = "it broke the link's rules";
		return EPIPE;
	}
	if (verdict == X11_LINK_DROP)
		return 0;

	/* What it says. */
	switch (frame->type) {
	case X11_LINK_HELLO:
		/* The child's version must be this mikuterm's. */
		if (frame->size < 4 || x11_link_get32(frame->body) != X11_LINK_VERSION) {
			*why = "it is of another version";
			return EPIPE;
		}
		host->hello = 1;
		if (host->child_end != NULL) {
			kl_socket_close(host->child_end);
			host->child_end = NULL;
		}
		return 0;
	case X11_LINK_PONG:
		host->ping_waiting = 0;
		return 0;
	case X11_LINK_TAB_OPEN:
	case X11_LINK_TAB_TITLE:
	case X11_LINK_TAB_SIZE:
	case X11_LINK_TAB_PIXELS:
	case X11_LINK_TAB_GONE:
		error = host_tab_take(host, frame, why);
		return error;
	default:
		break;
	}

	/* A frame of a channel the parent still has (CREDIT needs nothing more than the check). */
	slot = host_by_id(host, frame->id);
	if (slot == NULL || frame->type == X11_LINK_CREDIT)
		return 0;

	/* The X server's bytes for the remote client (the credit keeps them within the buffer). */
	if (frame->type == X11_LINK_DATA) {
		if (frame->size > HOST_TO_REMOTE_MAX - slot->to_remote_size) {
			*why = "it sent past its credit";
			return EPIPE;
		}
		memcpy(slot->to_remote + slot->to_remote_size, frame->body, frame->size);
		slot->to_remote_size += frame->size;
		return 0;
	}

	/* The X server closed the client (EOF or CLOSE): the channel closes once its bytes are written. */
	slot->child_closed = 1;
	x11_link_close(&host->table, slot->id);
	(void)engine;
	return 0;
}

/*
 * Moves the bytes of each channel: the child's to the connection (and the
 * credit given back for them), the remote client's to the child within
 * its credit (and the SSH window given back for them), the remote EOF
 * after its bytes, and a channel the child closed closed on the
 * connection once its bytes are written.
 */
static void
host_move(
	struct x11_host *host,
	struct ssh_engine *engine)
{
	struct x11_host_channel *slot;
	uint8_t credit[4];
	unsigned i;
	size_t written;
	size_t size;
	uint32_t room;
	int error;

	/* Each channel. */
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		slot = &host->channels[i];
		if (!slot->used)
			continue;

		/* The child's bytes to the connection, as far as the window lets them. */
		if (slot->to_remote_size != 0) {
			written = 0;
			error = ssh_engine_channel_write(engine, slot->channel, slot->to_remote, slot->to_remote_size, &written);
			if (error == 0 && written != 0) {
				memmove(slot->to_remote, slot->to_remote + written, slot->to_remote_size - written);
				slot->to_remote_size -= written;
				if (!slot->child_closed) {
					x11_link_put32(credit, (uint32_t)written);
					x11_link_gave(&host->table, slot->id, (uint32_t)written);
					(void)host_frame(host, X11_LINK_CREDIT, slot->id, credit, 4);
				}
			}
		}

		/* A channel the child closed closes once its bytes are written. */
		if (slot->child_closed) {
			if (slot->to_remote_size == 0) {
				(void)ssh_engine_channel_close(engine, slot->channel);
				host_free(host, slot);
			}
			continue;
		}

		/* The remote client's bytes to the child, within the credit and a frame's size. */
		while (slot->to_child_size != 0) {
			room = x11_link_room(&host->table, slot->id);
			if (room == 0)
				break;
			size = slot->to_child_size;
			if (size > room)
				size = room;
			if (size > X11_LINK_BODY_MAX)
				size = X11_LINK_BODY_MAX;
			error = host_frame(host, X11_LINK_DATA, slot->id, slot->to_child, (uint32_t)size);
			if (error != 0)
				break;
			x11_link_sent(&host->table, slot->id, (uint32_t)size);
			memmove(slot->to_child, slot->to_child + size, slot->to_child_size - size);
			slot->to_child_size -= size;
			ssh_engine_channel_consumed(engine, slot->channel, size);
		}

		/* The remote EOF after its bytes. */
		if (slot->remote_eof && !slot->eof_sent && slot->to_child_size == 0) {
			(void)host_frame(host, X11_LINK_EOF, slot->id, NULL, 0);
			slot->eof_sent = 1;
		}
	}
}

/*
 * Sends the frames waiting, as far as the socket takes them.  Returns 0,
 * or EPIPE when the socket failed.
 */
static int
host_send(
	struct x11_host *host)
{
	size_t sent;
	int error;

	/* Sends until all went or the socket is full. */
	while (host->out_size != 0) {
		error = kl_socket_send(host->socket, host->out, host->out_size, &sent);
		if (error == EAGAIN)
			break;
		if (error != 0)
			return EPIPE;
		memmove(host->out, host->out + sent, host->out_size - sent);
		host->out_size -= sent;
	}

	/* Succeeded: what could go went. */
	return 0;
}

/* Finds a used slot by the engine's channel number. */
static struct x11_host_channel *
host_by_channel(
	struct x11_host *host,
	uint32_t channel)
{
	unsigned i;

	/* Looks through the slots. */
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (host->channels[i].used && host->channels[i].channel == channel)
			return &host->channels[i];
	}

	/* None. */
	return NULL;
}

/* Finds a used slot by its link id. */
static struct x11_host_channel *
host_by_id(
	struct x11_host *host,
	uint32_t id)
{
	unsigned i;

	/* Looks through the slots. */
	for (i = 0; i < X11_LINK_CHANNELS; i++) {
		if (host->channels[i].used && host->channels[i].id == id)
			return &host->channels[i];
	}

	/* None. */
	return NULL;
}

/* Frees a channel's slot and its link id. */
static void
host_free(
	struct x11_host *host,
	struct x11_host_channel *slot)
{
	/* The link id is never used again; the buffers go. */
	x11_link_close(&host->table, slot->id);
	free(slot->to_child);
	free(slot->to_remote);
	memset(slot, 0, sizeof(*slot));
}

/*
 * Ends the child: the link closes, the tabs close, and the process ends
 * (x11_spawn_stop gives it a moment to end by itself first).
 */
static void
host_stop_child(
	struct x11_host *host)
{
	/* The link closes, with the child's end if mikuterm still holds it. */
	if (host->child_end != NULL) {
		kl_socket_close(host->child_end);
		host->child_end = NULL;
	}
	if (host->socket != NULL) {
		kl_socket_close(host->socket);
		host->socket = NULL;
	}

	/* The X windows' tabs close. */
	host_tabs_end(host);

	/* The process ends. */
	x11_spawn_stop(host);
	host->hello = 0;
	host->in_size = 0;
	host->out_size = 0;
}

/*
 * Takes a frame of an X window's tab from the child: its opening (a new
 * tab), a new title, a new size, a part of its pixels, its end.  A frame
 * of a window the host no longer holds (it crossed a closing or a
 * refusal) is dropped.  Returns 0, or EPIPE with *why for a frame
 * against the rules.
 */
static int
host_tab_take(
	struct x11_host *host,
	const struct x11_link_frame *frame,
	const char **why)
{
	struct x11_tab *tab;
	unsigned place;
	uint32_t width;
	uint32_t height;
	int error;

	/* An opening makes a tab. */
	if (frame->type == X11_LINK_TAB_OPEN) {
		error = host_tab_open(host, frame, why);
		return error;
	}

	/* The tab of the window; one the host let go of drops it. */
	place = host_tab_find(host, frame->id);
	if (place >= X11_LINK_TABS)
		return 0;
	tab = host->tabs[place];

	/* What it says. */
	switch (frame->type) {
	case X11_LINK_TAB_TITLE:
		x11_tab_set_title(tab, frame->body, frame->size);
		break;
	case X11_LINK_TAB_SIZE:
		/* A size a tab may have; without memory for it the tab ends. */
		width = x11_link_get32(frame->body);
		height = x11_link_get32(frame->body + 4);
		if (width == 0U ||
		    height == 0U ||
		    width > X11_LINK_TAB_SIDE_MAX ||
		    height > X11_LINK_TAB_SIDE_MAX ||
		    (uint64_t)width * height > X11_LINK_TAB_AREA_MAX) {
			*why = "it sent a tab's size past the limits";
			return EPIPE;
		}
		error = x11_tab_set_size(tab, width, height);
		if (error != 0) {
			x11_tab_end(tab);
			host_tab_drop(host, place);
			return 0;
		}
		break;
	case X11_LINK_TAB_PIXELS:
		error = host_tab_pixels(tab, frame, why);
		if (error != 0)
			return error;
		break;
	default:
		/* The window ended: its tab closes. */
		x11_tab_end(tab);
		host_tab_drop(host, place);
		return 0;
	}

	/* Succeeded: the application's thread hears of the news. */
	x11_tab_wake(tab);
	return 0;
}

/*
 * Takes the opening of an X window's tab: a window the host has no tab
 * for, a free place, a size and a scale a tab may have, and its title.
 * The tab waits for the network thread to take it.  Returns 0, or EPIPE
 * with *why for an opening against the rules or without memory.
 */
static int
host_tab_open(
	struct x11_host *host,
	const struct x11_link_frame *frame,
	const char **why)
{
	struct x11_tab *tab;
	uint32_t width;
	uint32_t height;
	uint32_t scale;
	unsigned place;
	unsigned found;
	int error;

	/* A window the host has no tab for, and a free place. */
	found = host_tab_find(host, frame->id);
	place = X11_LINK_TABS;
	if (found >= X11_LINK_TABS)
		place = host_tab_find(host, 0U);
	if (place >= X11_LINK_TABS) {
		*why = "it opened a tab twice or too many";
		return EPIPE;
	}

	/* The size and the scale, within the limits. */
	width = x11_link_get32(frame->body);
	height = x11_link_get32(frame->body + 4);
	scale = x11_link_get32(frame->body + 8);
	if (width == 0U ||
	    height == 0U ||
	    width > X11_LINK_TAB_SIDE_MAX ||
	    height > X11_LINK_TAB_SIDE_MAX ||
	    (uint64_t)width * height > X11_LINK_TAB_AREA_MAX ||
	    scale < HOST_SCALE_MIN ||
	    scale > HOST_SCALE_MAX) {
		*why = "it opened a tab past the limits";
		return EPIPE;
	}

	/* The tab, with the title after the fixed part. */
	error = x11_tab_create(frame->id,
			       width,
			       height,
			       scale,
			       host->settings.host,
			       frame->body + X11_LINK_TAB_OPEN_FIXED,
			       frame->size - X11_LINK_TAB_OPEN_FIXED,
			       &tab);
	if (error != 0) {
		*why = "a tab could not be made";
		return EPIPE;
	}

	/* Succeeded: the host holds it, and it waits for the network thread. */
	if (host->settings.root_width != 0U)
		tab->touch.mode = X11_TOUCH_MODE_ROOT;
	host->tabs[place] = tab;
	host->fresh[host->fresh_count] = tab;
	host->fresh_count++;
	return 0;
}

/*
 * Takes a part of a tab's pixels: its place and size, then its pixels,
 * exactly as many as the part has, inside the window.  Returns 0, or
 * EPIPE with *why.
 */
static int
host_tab_pixels(
	struct x11_tab *tab,
	const struct x11_link_frame *frame,
	const char **why)
{
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint64_t bytes;
	int error;

	/* The part's place and size, and the bytes its pixels take. */
	x = x11_link_get32(frame->body);
	y = x11_link_get32(frame->body + 4);
	width = x11_link_get32(frame->body + 8);
	height = x11_link_get32(frame->body + 12);
	bytes = (uint64_t)width * height * 4U;
	if (bytes != (uint64_t)frame->size - X11_LINK_TAB_PIXELS_FIXED) {
		*why = "it sent a tab's pixels of another size";
		return EPIPE;
	}

	/* The pixels, inside the window. */
	error = x11_tab_put(tab, x, y, width, height, frame->body + X11_LINK_TAB_PIXELS_FIXED);
	if (error != 0) {
		*why = "it sent a tab's pixels outside the window";
		return EPIPE;
	}

	/* Succeeded: the part is the tab's. */
	return 0;
}

/* Gives the place of the tab of an X window (window 0: a free place), or X11_LINK_TABS for none. */
static unsigned
host_tab_find(
	const struct x11_host *host,
	uint32_t window)
{
	unsigned place;

	/* Looks through the places. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (window == 0U && host->tabs[place] == NULL)
			return place;
		if (window != 0U && host->tabs[place] != NULL && host->tabs[place]->window == window)
			return place;
	}

	/* None. */
	return X11_LINK_TABS;
}

/* Lets go of the host's hold on the tab in a place, which is free from now on. */
static void
host_tab_drop(
	struct x11_host *host,
	unsigned place)
{
	struct x11_tab *tab;

	/* The place is free; the tab goes when the run lets go too. */
	tab = host->tabs[place];
	host->tabs[place] = NULL;
	x11_tab_release(tab);
}

/*
 * Sends the child what each tab has: its closing or refusal (the host
 * then lets go of it), the size the tab gives the window, and the user's
 * input in it.
 */
static void
host_tabs_move(
	struct x11_host *host)
{
	struct x11_tab_input input;
	struct x11_tab *tab;
	uint8_t size[X11_LINK_TAB_SIZE_BODY];
	unsigned place;
	unsigned width;
	unsigned height;
	int leaving;
	int refused;
	int taken;

	/* Each tab the host holds. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		tab = host->tabs[place];
		if (tab == NULL)
			continue;

		/* A tab closed or refused: the child is told, and the host lets go. */
		leaving = x11_tab_leaving(tab, &refused);
		if (leaving) {
			if (refused)
				(void)host_frame(host, X11_LINK_TAB_REFUSE, tab->window, NULL, 0);
			else
				(void)host_frame(host, X11_LINK_TAB_CLOSE, tab->window, NULL, 0);
			host_tab_drop(host, place);
			continue;
		}

		/* The size the tab gives the window. */
		taken = x11_tab_take_size(tab, &width, &height);
		if (taken) {
			x11_link_put32(size, width);
			x11_link_put32(size + 4, height);
			(void)host_frame(host, X11_LINK_TAB_CONFIGURE, tab->window, size, sizeof(size));
		}

		/* The user's input, oldest first. */
		for (;;) {
			taken = x11_tab_take_input(tab, &input);
			if (!taken)
				break;
			host_tab_input(host, tab, &input);
		}
	}
}

/* Sends one of the user's inputs in a tab: its kind and three numbers, or the kind and a commit's text. */
static void
host_tab_input(
	struct x11_host *host,
	const struct x11_tab *tab,
	const struct x11_tab_input *input)
{
	uint8_t body[4U + X11_TAB_TEXT_MAX];
	size_t length;

	/* The kind first. */
	x11_link_put32(body, input->kind);

	/* Text after the kind, when there is some. */
	if (input->kind == X11_LINK_INPUT_TEXT) {
		length = strlen(input->text);
		if (length == 0U)
			return;
		memcpy(body + 4, input->text, length);
		(void)host_frame(host, X11_LINK_TAB_INPUT, tab->window, body, (uint32_t)(4U + length));
		return;
	}

	/* The three numbers otherwise. */
	x11_link_put32(body + 4, input->numbers[0]);
	x11_link_put32(body + 8, input->numbers[1]);
	x11_link_put32(body + 12, input->numbers[2]);
	(void)host_frame(host, X11_LINK_TAB_INPUT, tab->window, body, X11_LINK_TAB_INPUT_BODY);
}

/*
 * Ends every tab, as the X server ends: each closes, the host lets go of
 * it, and a new one the network thread has not taken goes too.
 */
static void
host_tabs_end(
	struct x11_host *host)
{
	unsigned place;
	unsigned index;

	/* The new tabs no one took: the run's hold goes. */
	for (index = 0U; index < host->fresh_count; index++) {
		x11_tab_end(host->fresh[index]);
		x11_tab_release(host->fresh[index]);
	}
	host->fresh_count = 0U;

	/* Each tab held. */
	for (place = 0U; place < X11_LINK_TABS; place++) {
		if (host->tabs[place] == NULL)
			continue;
		x11_tab_end(host->tabs[place]);
		host_tab_drop(host, place);
	}
}
