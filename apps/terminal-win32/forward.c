/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The port forwardings of one SSH connection of mikuterm (forward.h):
 * the sockets that listen for the local forwardings, the remote
 * forwardings asked of the server, and the bytes of each forwarded
 * connection between its socket and its channel.
 *
 * The rules are those of zedBSD's ssh (ssh-client-forward.c), which
 * follow OpenSSH's ssh: a local forwarding listens on the loopback
 * addresses unless GatewayPorts or a bind address says otherwise, a
 * remote forwarding without a bind address asks the server for
 * "localhost", ExitOnForwardFailure ends the connection when a
 * forwarding cannot be set up, a connection to a remote forwarding's
 * destination may take 30 seconds, a socket is read only while its last
 * bytes were taken by the channel, the window goes back only for what was
 * written to the socket, and each way of a connection ends on its own
 * (the EOF and the shut of the socket's sending).
 *
 * Procedure referenced: OpenSSH's ssh, openssh-portable
 * fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0: ssh.c (ssh_init_forwarding,
 * ssh_confirm_remote_forward), channels.c
 * (channel_setup_fwd_listener_tcpip, channel_fwd_bind_addr,
 * channel_rfwd_bind_host, connect_to).  The messages are theirs.
 *
 * On Windows each listening socket is made exclusive (SO_EXCLUSIVEADDRUSE)
 * before it is bound, so that no other program, even the same user's,
 * binds the same port to take its connections.
 * Procedure referenced: Microsoft, "Using SO_REUSEADDR and
 * SO_EXCLUSIVEADDRUSE" (Windows Sockets 2).
 *
 * What differs from OpenSSH's ssh: a connection that cannot be made to a
 * remote forwarding's destination, and a direct-tcpip channel the server
 * refused, close their connection without a line in the tab.
 */

#include "forward.h"

#include <keiland/keiland_thread.h>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The address a remote forwarding without a bind address asks the server to listen on (OpenSSH's channel_rfwd_bind_host). */
#define FORWARD_REMOTE_DEFAULT	"localhost"

/* The most addresses a host name of a bind address gives. */
#define FORWARD_RESOLVE_MAX	FORWARD_LISTEN_ADDRESSES

static int forward_listen_local(struct forward_state *forwards, unsigned index);
static int forward_bind_addresses(struct forward_state *forwards, const struct ssh_forward *forward, struct kl_address *addresses, size_t *count);
static int forward_listen_one(struct forward_state *forwards, unsigned index, const struct kl_address *address, int last);
static int forward_bind(const struct kl_address *address, struct kl_socket **bound);
static int forward_make_exclusive(struct kl_socket *socket);
static int forward_ask_remote(struct forward_state *forwards, unsigned index);
static void forward_accept(struct forward_state *forwards, struct forward_listener *listener, uint64_t now_ms);
static void forward_take_data(struct forward_state *forwards, const struct ssh_engine_event *event, int *taken);
static void forward_take_eof(struct forward_state *forwards, const struct ssh_engine_event *event, int *taken);
static void forward_take_closed(struct forward_state *forwards, const struct ssh_engine_event *event, int *taken);
static int forward_take_reply(struct forward_state *forwards, const struct ssh_engine_event *event);
static void forward_take_open(struct forward_state *forwards, const struct ssh_engine_event *event, uint64_t now_ms);
static void forward_take_open_reply(struct forward_state *forwards, const struct ssh_engine_event *event);
static void forward_connect_next(struct forward_state *forwards, struct forward_link *link);
static void forward_connected(struct forward_state *forwards, struct forward_link *link);
static void forward_answer(struct forward_state *forwards, struct forward_link *link, int accept);
static void forward_read(struct forward_state *forwards, struct forward_link *link);
static void forward_write(struct forward_state *forwards, struct forward_link *link);
static void forward_settle(struct forward_state *forwards, struct forward_link *link);
static void forward_abort(struct forward_state *forwards, struct forward_link *link);
static void forward_release(struct forward_link *link);
static struct forward_link *forward_find_link(struct forward_state *forwards, uint32_t channel);
static struct forward_link *forward_take_link(struct forward_state *forwards, unsigned forward);
static const struct forward_remote *forward_find_remote(const struct forward_state *forwards, unsigned number);
static void forward_address_text(const struct kl_address *address, char *text, size_t size);
static const char *forward_error_text(int error);
static void forward_say(struct forward_state *forwards, const char *line);

/*
 * Starts the forwardings of a connection: nothing listens and nothing is
 * asked yet.
 */
void
forward_init(
	struct forward_state *forwards,
	struct ssh_engine *engine,
	const struct ssh_config *config,
	void (*say)(void *data, const char *line),
	void *data)
{
	struct forward_link *link;
	unsigned index;

	/* Keeps the connection's engine and settings, and where lines are said. */
	forwards->engine = engine;
	forwards->config = config;
	forwards->say = say;
	forwards->data = data;

	/* Nothing listens, nothing is asked, and no line ends the connection. */
	forwards->listening = 0;
	forwards->rebind_wait = 0;
	forwards->listener_count = 0;
	forwards->remote_count = 0;
	forwards->fatal[0] = '\0';

	/* Every forwarded connection's entry is free, without a socket and with an empty queue. */
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		link = &forwards->links[index];
		link->state = FORWARD_LINK_FREE;
		link->socket = NULL;
		link->chunk_size = 0;
		link->chunk_offset = 0;
		link->queue_offset = 0;
		link->address_count = 0;
		link->next_address = 0;
		link->wait = -1;
		ssh_buf_init(&link->queue, FORWARD_QUEUE_MAX, 1);
	}
}

/*
 * Sets up the forwardings once the user is authenticated: listens for
 * each local forwarding and asks the server for each remote one.
 * Returns 0, or FORWARD_FATAL with the line in fatal when
 * ExitOnForwardFailure ends the connection.
 */
int
forward_start(
	struct forward_state *forwards)
{
	const struct ssh_config *config;
	unsigned index;
	unsigned asked;
	unsigned listened;
	int status;

	/* Takes the settings. */
	config = forwards->config;

	/* Listens for each local forwarding, counting those that listen on at least one address. */
	asked = 0;
	listened = 0;
	for (index = 0; index < config->forward_count; index++) {
		if (config->forward[index].kind != SSH_FORWARD_LOCAL)
			continue;
		asked++;

		/* Counts a forwarding that listens. */
		status = forward_listen_local(forwards, index);
		if (status)
			listened++;
	}

	/* With ExitOnForwardFailure, one local forwarding that could not listen ends the connection. */
	if (asked > 0 &&
	    listened != asked &&
	    config->exit_on_forward_failure) {
		(void)snprintf(forwards->fatal, sizeof(forwards->fatal), "Could not request local forwarding.");
		return FORWARD_FATAL;
	}

	/* Without it, the connection goes on unless none could listen, which is said. */
	if (asked > 0 && listened == 0)
		forward_say(forwards, "Could not request local forwarding.");

	/* New connections are taken from now until the session ends. */
	forwards->listening = 1;

	/* Asks the server for each remote forwarding. */
	for (index = 0; index < config->forward_count; index++) {
		if (config->forward[index].kind != SSH_FORWARD_REMOTE)
			continue;

		/* Stops when ExitOnForwardFailure ends the connection. */
		status = forward_ask_remote(forwards, index);
		if (status != 0)
			return status;
	}

	/* Succeeded: the forwardings are set up as far as they could be. */
	return 0;
}

/*
 * Closes the listening sockets (the session ended); the connections they
 * took go on.
 */
void
forward_stop_listening(
	struct forward_state *forwards)
{
	unsigned index;

	/* Closes every listening socket. */
	for (index = 0; index < forwards->listener_count; index++) {
		kl_socket_close(forwards->listeners[index].socket);
		forwards->listeners[index].socket = NULL;
		forwards->listeners[index].wait = -1;
	}

	/* No more connections are taken. */
	forwards->listener_count = 0;
	forwards->listening = 0;
}

/*
 * Takes an event of the engine if it belongs to the forwardings (now_ms
 * is the monotonic clock in milliseconds): *taken is 1 when it did, 0
 * when it is the session's or another channel's (an X client's).  Returns 0, or FORWARD_FATAL with the line in fatal when a
 * refused remote forwarding ends the connection (ExitOnForwardFailure).
 */
int
forward_event(
	struct forward_state *forwards,
	const struct ssh_engine_event *event,
	uint64_t now_ms,
	int *taken)
{
	int status;

	/* Takes the event unless it turns out to be another's. */
	*taken = 1;
	status = 0;

	/* Acts by the kind of the event. */
	switch (event->kind) {
	case SSH_ENGINE_DATA:
		forward_take_data(forwards, event, taken);
		break;
	case SSH_ENGINE_CHANNEL_EOF:
		forward_take_eof(forwards, event, taken);
		break;
	case SSH_ENGINE_CHANNEL_CLOSED:
		forward_take_closed(forwards, event, taken);
		break;
	case SSH_ENGINE_FORWARD_REPLY:
		status = forward_take_reply(forwards, event);
		break;
	case SSH_ENGINE_FORWARD_OPEN:
		forward_take_open(forwards, event, now_ms);
		break;
	case SSH_ENGINE_OPEN_REPLY:
		forward_take_open_reply(forwards, event);
		break;
	default:
		/* Any other event is the connection's. */
		*taken = 0;
		break;
	}

	/* Reports ExitOnForwardFailure ending the connection. */
	if (status != 0)
		return status;

	/* Succeeded: the event is handled, or left to the caller. */
	return 0;
}

/*
 * Moves the forwarded connections' bytes read into their channels, and
 * sends the EOFs and closes that are due.
 */
void
forward_pump(
	struct forward_state *forwards)
{
	struct forward_link *link;
	size_t written;
	unsigned index;
	int error;

	/* Gives each open connection's waiting bytes to its channel. */
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		link = &forwards->links[index];
		if (link->state != FORWARD_LINK_OPEN)
			continue;

		/* A channel the engine ended takes nothing more. */
		if (link->channel_gone) {
			forward_settle(forwards, link);
			continue;
		}

		/* Gives the bytes while the channel's window and the engine's output take them. */
		error = 0;
		while (link->chunk_offset < link->chunk_size) {
			error = ssh_engine_channel_write(forwards->engine, link->channel, link->chunk + link->chunk_offset, link->chunk_size - link->chunk_offset, &written);
			if (error != 0)
				break;

			/* Waits for room when the engine took nothing. */
			if (written == 0)
				break;
			link->chunk_offset += written;
		}

		/* A channel that cannot be written any more ends the connection. */
		if (error != 0) {
			forward_abort(forwards, link);
			continue;
		}

		/* Starts the chunk again once all of it went, so that the socket is read again. */
		if (link->chunk_offset == link->chunk_size) {
			link->chunk_offset = 0;
			link->chunk_size = 0;
		}

		/* Sends the EOF, shuts the socket, and closes the channel when they are due. */
		forward_settle(forwards, link);
	}
}

/*
 * Puts the sockets the forwardings wait on into waits (at most
 * capacity), and brings *wake_ms forward to the next time one of them
 * needs acting on without an event.  Returns how many waits were filled.
 */
size_t
forward_waits(
	struct forward_state *forwards,
	struct kl_socket_wait *waits,
	size_t capacity,
	uint64_t now_ms,
	uint64_t *wake_ms)
{
	struct forward_listener *listener;
	struct forward_link *link;
	size_t count;
	unsigned index;
	unsigned events;

	/* Watches each listening socket for a connection, unless it rests after a lack of descriptors. */
	count = 0;
	for (index = 0; index < forwards->listener_count; index++) {
		listener = &forwards->listeners[index];
		listener->wait = -1;

		/* Wakes the loop when a resting listener may accept again. */
		if (listener->paused_until > now_ms) {
			if (listener->paused_until < *wake_ms)
				*wake_ms = listener->paused_until;
			continue;
		}

		/* Ends the rest once its time is over. */
		listener->paused_until = 0;
		if (count >= capacity)
			continue;

		/* Takes a wait for the listener. */
		waits[count].socket = listener->socket;
		waits[count].waker = NULL;
		waits[count].events = KL_SOCKET_READABLE;
		waits[count].ready = 0;
		listener->wait = (int)count;
		count++;
	}

	/* Watches each forwarded connection for what it waits for. */
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		link = &forwards->links[index];
		link->wait = -1;
		if (link->state == FORWARD_LINK_FREE)
			continue;
		if (link->socket == NULL)
			continue;

		/* Chooses the events: a connection being made, bytes to read, room to write. */
		events = 0;
		if (link->state == FORWARD_LINK_CONNECTING) {
			/* The connection's end shows as room to write; its time limit wakes the loop. */
			events |= KL_SOCKET_WRITABLE;
			if (link->deadline < *wake_ms)
				*wake_ms = link->deadline;
		} else if (link->state == FORWARD_LINK_OPEN) {
			/* Reads only when the chunk read last went to the channel, and the socket has not ended. */
			if (!link->channel_gone &&
			    link->chunk_size == 0 &&
			    !link->socket_ended)
				events |= KL_SOCKET_READABLE;

			/* Waits for room while the server's bytes wait for the socket. */
			if (link->queue.size > link->queue_offset)
				events |= KL_SOCKET_WRITABLE;
		}

		/* A connection that waits for nothing here (the server's answer, room in the window) takes no wait. */
		if (events == 0)
			continue;
		if (count >= capacity)
			continue;

		/* Takes a wait for the connection. */
		waits[count].socket = link->socket;
		waits[count].waker = NULL;
		waits[count].events = events;
		waits[count].ready = 0;
		link->wait = (int)count;
		count++;
	}

	/* Reports how many waits were filled. */
	return count;
}

/*
 * Acts on what the wait found for the forwardings' sockets (waits as
 * forward_waits filled them).
 */
void
forward_serve(
	struct forward_state *forwards,
	const struct kl_socket_wait *waits,
	uint64_t now_ms)
{
	struct forward_listener *listener;
	struct forward_link *link;
	unsigned index;
	unsigned ready;
	unsigned events;

	/* Accepts a connection on each listening socket that has one. */
	for (index = 0; index < forwards->listener_count; index++) {
		listener = &forwards->listeners[index];
		if (listener->wait < 0)
			continue;

		/* A connection waits, or the socket failed (the accept tells). */
		ready = waits[listener->wait].ready;
		if ((ready & (KL_SOCKET_READABLE | KL_SOCKET_FAILED)) != 0)
			forward_accept(forwards, listener, now_ms);
	}

	/* Moves each forwarded connection on. */
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		link = &forwards->links[index];

		/* Gives up a connection to a destination that did not answer in time. */
		if (link->state == FORWARD_LINK_CONNECTING && now_ms >= link->deadline) {
			kl_socket_close(link->socket);
			link->socket = NULL;
			link->next_address = link->address_count;
			forward_connect_next(forwards, link);
			continue;
		}

		/* Passes over a connection the wait did not watch. */
		if (link->wait < 0)
			continue;
		ready = waits[link->wait].ready;
		events = waits[link->wait].events;

		/* A connection being made has its outcome when the socket is writable, or failed. */
		if (link->state == FORWARD_LINK_CONNECTING) {
			if ((ready & (KL_SOCKET_WRITABLE | KL_SOCKET_FAILED)) != 0)
				forward_connected(forwards, link);
			continue;
		}

		/* Writes the server's bytes when the socket takes them, or failed. */
		if ((events & KL_SOCKET_WRITABLE) != 0 &&
		    (ready & (KL_SOCKET_WRITABLE | KL_SOCKET_FAILED)) != 0)
			forward_write(forwards, link);

		/* Reads the socket when it has bytes, ended or failed, unless the write ended the connection. */
		if (link->state == FORWARD_LINK_OPEN &&
		    (events & KL_SOCKET_READABLE) != 0 &&
		    (ready & (KL_SOCKET_READABLE | KL_SOCKET_FAILED)) != 0)
			forward_read(forwards, link);
	}
}

/*
 * Reports how many forwarded connections are open or on their way.
 */
unsigned
forward_open_count(
	const struct forward_state *forwards)
{
	unsigned count;
	unsigned index;

	/* Counts every entry in use. */
	count = 0;
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		if (forwards->links[index].state != FORWARD_LINK_FREE)
			count++;
	}

	/* Reports the count. */
	return count;
}

/*
 * Closes every socket of the forwardings and frees what they hold.
 */
void
forward_free(
	struct forward_state *forwards)
{
	unsigned index;

	/* Closes the listening sockets. */
	forward_stop_listening(forwards);

	/* Closes every forwarded connection and frees its queue. */
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		forward_release(&forwards->links[index]);
		ssh_buf_free(&forwards->links[index].queue);
	}
}

/*
 * Listens for one local forwarding on every address it names.  Returns 1
 * when at least one socket listens, 0 when none does (the reason said).
 */
static int
forward_listen_local(
	struct forward_state *forwards,
	unsigned index)
{
	struct kl_address addresses[FORWARD_LISTEN_ADDRESSES];
	size_t count;
	size_t position;
	int listened;
	int status;

	/* Finds the addresses to listen on. */
	status = forward_bind_addresses(forwards, &forwards->config->forward[index], addresses, &count);
	if (status != 0)
		return 0;

	/* Listens on each; only a failure on the last address is said (the other family may have the port). */
	listened = 0;
	for (position = 0; position < count; position++) {
		status = forward_listen_one(forwards, index, &addresses[position], position + 1U == count);
		if (status)
			listened = 1;
	}

	/* Reports whether the forwarding listens. */
	return listened;
}

/*
 * Finds the addresses a local forwarding listens on (OpenSSH's
 * channel_fwd_bind_addr for the client): no bind address is the loopback
 * addresses, or every address with GatewayPorts; "" and "*" are every
 * address; "localhost" is the loopback addresses; any other is that
 * address (a name is looked up).  Returns 0 with *count of them, or
 * ENOENT when there is none (the reason said).
 */
static int
forward_bind_addresses(
	struct forward_state *forwards,
	const struct ssh_forward *forward,
	struct kl_address *addresses,
	size_t *count)
{
	char line[FORWARD_LINE_MAX];
	int wildcard;
	int loopback;
	int compare;
	int error;

	/* Decides between every address, the loopback addresses, and the one named. */
	*count = 0;
	wildcard = 0;
	loopback = 0;
	if (!forward->have_listen_host) {
		/* No bind address: GatewayPorts decides. */
		if (forwards->config->gateway_ports) {
			wildcard = 1;
		} else {
			loopback = 1;
		}
	} else if (forward->listen_host[0] == '\0') {
		/* "" or "*": every address, whatever GatewayPorts says. */
		wildcard = 1;
	} else {
		/* "localhost" is both loopback addresses, never looked up. */
		compare = strcmp(forward->listen_host, "localhost");
		if (compare == 0)
			loopback = 1;
	}

	/* Every address of both families. */
	if (wildcard) {
		kl_address_any(KL_ADDRESS_IPV4, forward->listen_port, &addresses[0]);
		kl_address_any(KL_ADDRESS_IPV6, forward->listen_port, &addresses[1]);
		*count = 2;
		return 0;
	}

	/* The loopback address of both families. */
	if (loopback) {
		kl_address_loopback(KL_ADDRESS_IPV4, forward->listen_port, &addresses[0]);
		kl_address_loopback(KL_ADDRESS_IPV6, forward->listen_port, &addresses[1]);
		*count = 2;
		return 0;
	}

	/* The address named, looked up when it is a name (the thread waits for the system's answer). */
	error = kl_socket_resolve(forward->listen_host, forward->listen_port, KL_ADDRESS_ANY, KL_SOCKET_STREAM, addresses, FORWARD_RESOLVE_MAX, count);
	if (error != 0) {
		(void)snprintf(line, sizeof(line), "getaddrinfo(%s): %s", forward->listen_host, forward_error_text(error));
		forward_say(forwards, line);
		*count = 0;
		return ENOENT;
	}

	/* Refuses a forwarding with nowhere to listen. */
	if (*count == 0)
		return ENOENT;

	/* Succeeded: *count addresses to listen on. */
	return 0;
}

/*
 * Makes one socket that listens on address for the local forwarding at
 * index.  A failed bind is said when last.  Returns 1 when it listens, 0
 * when not.
 */
static int
forward_listen_one(
	struct forward_state *forwards,
	unsigned index,
	const struct kl_address *address,
	int last)
{
	struct forward_listener *listener;
	struct kl_socket *socket;
	char host[KL_ADDRESS_TEXT];
	char line[FORWARD_LINE_MAX];
	int error;

	/* Writes the address in numbers for the lines. */
	forward_address_text(address, host, sizeof(host));

	/* Refuses a socket the table has no room for (it holds the most addresses of every forwarding). */
	if (forwards->listener_count >= FORWARD_LISTENERS_MAX)
		return 0;

	/* Binds a socket to the address. */
	error = forward_bind(address, &socket);

	/*
	 * A port the last attempt's connections still hold (Windows keeps an
	 * exclusive port until the connections a closed listener accepted are
	 * no longer active) is tried once more after a wait, once for the
	 * whole attempt.
	 */
	if (error == EADDRINUSE && forwards->rebind_wait) {
		forwards->rebind_wait = 0;
		kl_thread_sleep(FORWARD_REBIND_WAIT_MS);
		error = forward_bind(address, &socket);
	}

	/* A failed bind is said for the last address (the family the system lacks is no error). */
	if (error != 0) {
		if (last && error != EAFNOSUPPORT) {
			(void)snprintf(line, sizeof(line), "bind [%s]:%u: %s", host, (unsigned)address->port, forward_error_text(error));
			forward_say(forwards, line);
		}
		return 0;
	}

	/* Starts listening for connections. */
	error = kl_socket_listen(socket, FORWARD_LISTEN_BACKLOG);
	if (error != 0) {
		(void)snprintf(line, sizeof(line), "listen [%s]:%u: %s", host, (unsigned)address->port, forward_error_text(error));
		forward_say(forwards, line);
		kl_socket_close(socket);
		return 0;
	}

	/* Keeps the listener. */
	listener = &forwards->listeners[forwards->listener_count];
	listener->socket = socket;
	listener->forward = index;
	listener->paused_until = 0;
	listener->wait = -1;
	forwards->listener_count++;

	/* Succeeded: the socket listens. */
	return 1;
}

/*
 * Opens a socket that does not block and binds it to address: on
 * Windows made exclusive first, elsewhere free to take a port a closed
 * socket left (as OpenSSH's SO_REUSEADDR).  Returns 0 with *bound, or the
 * error.
 */
static int
forward_bind(
	const struct kl_address *address,
	struct kl_socket **bound)
{
	struct kl_socket *socket;
	int error;

	/* Makes a socket of the address's family (an IPv6 one talks IPv6 alone). */
	*bound = NULL;
	error = kl_socket_open(address->family, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, &socket);
	if (error != 0)
		return error;

	/* Takes the port again at once after an earlier listener on it closed (nothing on Windows, whose meaning differs). */
	(void)kl_socket_set_option(socket, KL_SOCKET_REUSE_ADDRESS, 1);

	/* Keeps every other program from binding the same port while this one listens. */
	error = forward_make_exclusive(socket);
	if (error != 0) {
		kl_socket_close(socket);
		return error;
	}

	/* Binds the address. */
	error = kl_socket_bind(socket, address);
	if (error != 0) {
		kl_socket_close(socket);
		return error;
	}

	/* Succeeded: the socket is bound. */
	*bound = socket;
	return 0;
}

/*
 * Makes a socket's port its own on Windows (SO_EXCLUSIVEADDRUSE, before
 * the bind): by default Windows lets a program of the same user bind the
 * port again with SO_REUSEADDR and take the connections.  Elsewhere a
 * bound port is not shared without both sockets asking.  Returns 0 or
 * EIO.
 */
static int
forward_make_exclusive(
	struct kl_socket *socket)
{
#ifdef _WIN32
	SOCKET handle;
	BOOL on;
	int status;

	/* Sets the option on the system's socket. */
	handle = (SOCKET)kl_socket_native(socket);
	on = TRUE;
	status = setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, (int)sizeof(on));
	if (status != 0)
		return EIO;
#else
	(void)socket;
#endif

	/* Succeeded: the port is the socket's alone. */
	return 0;
}

/*
 * Asks the server for the remote forwarding at index (tcpip-forward):
 * "localhost" when no bind address was given, as OpenSSH's ssh sends.
 * Returns 0, or FORWARD_FATAL when the request could not go and
 * ExitOnForwardFailure ends the connection.
 */
static int
forward_ask_remote(
	struct forward_state *forwards,
	unsigned index)
{
	const struct ssh_forward *forward;
	const char *address;
	unsigned number;
	int error;

	/* Chooses the address the server is asked to listen on. */
	forward = &forwards->config->forward[index];
	address = FORWARD_REMOTE_DEFAULT;
	if (forward->have_listen_host)
		address = forward->listen_host;

	/* Sends the request; its answer comes as SSH_ENGINE_FORWARD_REPLY. */
	error = ssh_engine_forward_listen(forwards->engine, address, forward->listen_port, &number);

	/* Keeps the forwarding under the engine's number for the answer and the channels. */
	if (error == 0 && forwards->remote_count < SSH_CONFIG_FORWARDS_MAX) {
		forwards->remotes[forwards->remote_count].forward = index;
		forwards->remotes[forwards->remote_count].number = number;
		forwards->remote_count++;
		return 0;
	}

	/* A request that could not go ends the connection with ExitOnForwardFailure. */
	if (forwards->config->exit_on_forward_failure) {
		(void)snprintf(forwards->fatal, sizeof(forwards->fatal), "Could not request remote forwarding.");
		return FORWARD_FATAL;
	}

	/* Without it, the connection goes on. */
	forward_say(forwards, "Warning: Could not request remote forwarding.");
	return 0;
}

/*
 * Accepts one connection on a listening socket and opens a direct-tcpip
 * channel for it to the forwarding's destination.
 */
static void
forward_accept(
	struct forward_state *forwards,
	struct forward_listener *listener,
	uint64_t now_ms)
{
	struct kl_address peer;
	struct kl_socket *accepted;
	struct forward_link *link;
	const struct ssh_forward *forward;
	char origin[KL_ADDRESS_TEXT];
	uint32_t channel;
	int error;

	/* Takes the forwarding the socket listens for. */
	forward = &forwards->config->forward[listener->forward];

	/* Accepts the connection, as a socket that does not block. */
	error = kl_socket_accept(listener->socket, KL_SOCKET_NONBLOCKING, &accepted, &peer);
	if (error != 0) {
		/* Rests the listener for a while when descriptors ran out, leaving the connection in the backlog. */
		if (error == EMFILE || error == ENFILE)
			listener->paused_until = now_ms + FORWARD_ACCEPT_PAUSE_MS;

		/* Nothing more to do: a connection that went away, or a rest. */
		return;
	}

	/* Sends small writes at once, as a forwarded terminal session wants. */
	(void)kl_socket_set_option(accepted, KL_SOCKET_NO_DELAY, 1);

	/* Takes an entry for the connection; one too many is closed. */
	link = forward_take_link(forwards, listener->forward);
	if (link == NULL) {
		kl_socket_close(accepted);
		return;
	}

	/* Opens the channel, telling the server where the connection came from; its answer comes as SSH_ENGINE_OPEN_REPLY. */
	forward_address_text(&peer, origin, sizeof(origin));
	error = ssh_engine_open_direct(forwards->engine, forward->connect_host, forward->connect_port, origin, peer.port, &channel);
	if (error != 0) {
		kl_socket_close(accepted);
		link->state = FORWARD_LINK_FREE;
		return;
	}

	/* The connection waits for the server's answer before any byte goes. */
	link->socket = accepted;
	link->channel = channel;
	link->state = FORWARD_LINK_OPENING;
}

/*
 * Queues a forwarded channel's data for its socket; *taken is 0 for
 * data of a channel no forwarded connection holds.
 */
static void
forward_take_data(
	struct forward_state *forwards,
	const struct ssh_engine_event *event,
	int *taken)
{
	struct forward_link *link;
	int error;

	/* The session's data, and an X client's, are not the forwardings'. */
	link = NULL;
	if (event->channel != SSH_ENGINE_SESSION_CHANNEL)
		link = forward_find_link(forwards, event->channel);
	if (link == NULL) {
		*taken = 0;
		return;
	}

	/* Data arriving after the socket was closed is used at once, so its window goes back. */
	if (link->socket == NULL) {
		ssh_engine_channel_consumed(forwards->engine, event->channel, event->size);
		return;
	}

	/* Queues the bytes; a server that sent more than its window ends the connection. */
	ssh_buf_put_bytes(&link->queue, event->bytes, event->size);
	error = ssh_buf_error(&link->queue);
	if (error != 0)
		forward_abort(forwards, link);
}

/*
 * Notes that the server sends no more on a forwarded channel; the
 * socket's sending side is shut once the queued bytes are written.
 */
static void
forward_take_eof(
	struct forward_state *forwards,
	const struct ssh_engine_event *event,
	int *taken)
{
	struct forward_link *link;

	/* An end of a channel no forwarded connection holds is another's. */
	link = forward_find_link(forwards, event->channel);
	if (link == NULL) {
		*taken = 0;
		return;
	}

	/* Notes the end for the connection. */
	link->channel_ended = 1;
}

/*
 * Takes the end of a forwarded channel: the connection is closed once its
 * queued bytes are written.
 */
static void
forward_take_closed(
	struct forward_state *forwards,
	const struct ssh_engine_event *event,
	int *taken)
{
	struct forward_link *link;

	/* A close of a channel no forwarded connection holds is another's. */
	link = forward_find_link(forwards, event->channel);
	if (link == NULL) {
		*taken = 0;
		return;
	}

	/* The channel is gone: nothing more is sent, and the bytes read but not sent are dropped. */
	link->channel_gone = 1;
	link->chunk_size = 0;
	link->chunk_offset = 0;

	/* Closes the connection now unless the server's last bytes still wait for its socket. */
	if (link->socket == NULL || link->queue.size == link->queue_offset) {
		forward_release(link);
		return;
	}

	/* Writes the rest first. */
	link->state = FORWARD_LINK_OPEN;
}

/*
 * Takes the server's answer to a remote forwarding.  Returns 0, or
 * FORWARD_FATAL when a refusal ends the connection
 * (ExitOnForwardFailure).
 */
static int
forward_take_reply(
	struct forward_state *forwards,
	const struct ssh_engine_event *event)
{
	const struct forward_remote *remote;
	const struct ssh_forward *forward;
	char line[FORWARD_LINE_MAX];

	/* Finds the forwarding the answer is for. */
	remote = forward_find_remote(forwards, event->forward);
	if (remote == NULL)
		return 0;
	forward = &forwards->config->forward[remote->forward];

	/* Tells the port the server chose for port 0. */
	if (event->accepted) {
		if (forward->listen_port == 0) {
			(void)snprintf(line, sizeof(line), "Allocated port %u for remote forward to %s:%u", event->port, forward->connect_host, (unsigned)forward->connect_port);
			forward_say(forwards, line);
		}
		return 0;
	}

	/* A refusal ends the connection with ExitOnForwardFailure. */
	if (forwards->config->exit_on_forward_failure) {
		(void)snprintf(forwards->fatal, sizeof(forwards->fatal), "Error: remote port forwarding failed for listen port %u", (unsigned)forward->listen_port);
		return FORWARD_FATAL;
	}

	/* Without it, the refusal is told and the connection goes on. */
	(void)snprintf(line, sizeof(line), "Warning: remote port forwarding failed for listen port %u", (unsigned)forward->listen_port);
	forward_say(forwards, line);
	return 0;
}

/*
 * Takes a forwarded-tcpip channel the server opened for a remote
 * forwarding: connects to the forwarding's destination, and answers the
 * server once the connection is made or failed.
 */
static void
forward_take_open(
	struct forward_state *forwards,
	const struct ssh_engine_event *event,
	uint64_t now_ms)
{
	const struct forward_remote *remote;
	const struct ssh_forward *forward;
	struct forward_link *link;
	int error;

	/* Finds the forwarding; the engine opens only for the ones asked. */
	remote = forward_find_remote(forwards, event->forward);
	if (remote == NULL) {
		(void)ssh_engine_open_answer(forwards->engine, event->channel, 0);
		return;
	}
	forward = &forwards->config->forward[remote->forward];

	/* Takes an entry for the connection; one too many is refused. */
	link = forward_take_link(forwards, remote->forward);
	if (link == NULL) {
		(void)ssh_engine_open_answer(forwards->engine, event->channel, 0);
		return;
	}
	link->channel = event->channel;

	/* Looks the destination up (the thread waits for the system's answer, as OpenSSH's ssh does). */
	error = kl_socket_resolve(forward->connect_host, forward->connect_port, KL_ADDRESS_ANY, KL_SOCKET_STREAM, link->addresses, FORWARD_CONNECT_ADDRESSES, &link->address_count);
	if (error != 0) {
		link->address_count = 0;
		forward_answer(forwards, link, 0);
		return;
	}

	/* Connects to the first address that takes it, within the time allowed for all of them. */
	link->state = FORWARD_LINK_CONNECTING;
	link->next_address = 0;
	link->deadline = now_ms + FORWARD_CONNECT_WAIT_MS;
	forward_connect_next(forwards, link);
}

/*
 * Takes the server's answer to a direct-tcpip channel: the connection's
 * bytes go from now on, or it is closed.
 */
static void
forward_take_open_reply(
	struct forward_state *forwards,
	const struct ssh_engine_event *event)
{
	struct forward_link *link;

	/* Finds the connection that waits for the answer. */
	link = forward_find_link(forwards, event->channel);
	if (link == NULL)
		return;
	if (link->state != FORWARD_LINK_OPENING)
		return;

	/* An open channel carries the connection. */
	if (event->accepted) {
		link->state = FORWARD_LINK_OPEN;
		return;
	}

	/* A refused one closes its connection; the engine freed the number. */
	forward_release(link);
}

/*
 * Starts the connection to the next address of a remote forwarding's
 * destination, answering the server when one connects at once or none is
 * left.
 */
static void
forward_connect_next(
	struct forward_state *forwards,
	struct forward_link *link)
{
	const struct kl_address *address;
	struct kl_socket *socket;
	int error;

	/* Tries each address left until one connects or is on its way. */
	while (link->next_address < link->address_count) {
		address = &link->addresses[link->next_address];
		link->next_address++;

		/* Makes a socket of the address's family that does not block. */
		error = kl_socket_open(address->family, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, &socket);
		if (error != 0)
			continue;

		/* Starts the connection. */
		error = kl_socket_connect(socket, address);
		if (error == 0) {
			link->socket = socket;
			forward_answer(forwards, link, 1);
			return;
		}

		/* Waits for a connection on its way. */
		if (error == EINPROGRESS) {
			link->socket = socket;
			return;
		}

		/* Goes on with the next address. */
		kl_socket_close(socket);
	}

	/* No address took the connection: the server hears it failed. */
	forward_answer(forwards, link, 0);
}

/*
 * Takes the outcome of a connection on its way to a remote forwarding's
 * destination.
 */
static void
forward_connected(
	struct forward_state *forwards,
	struct forward_link *link)
{
	int error;

	/* Reads how the connection ended. */
	error = kl_socket_connect_result(link->socket);

	/* A made connection opens the channel. */
	if (error == 0) {
		forward_answer(forwards, link, 1);
		return;
	}

	/* A failed one tries the next address. */
	kl_socket_close(link->socket);
	link->socket = NULL;
	forward_connect_next(forwards, link);
}

/*
 * Answers the server's forwarded-tcpip channel: accept 1 opens it and
 * the connection carries bytes, 0 refuses it and frees the entry.
 */
static void
forward_answer(
	struct forward_state *forwards,
	struct forward_link *link,
	int accept)
{
	int error;

	/* The destination's addresses are no longer needed. */
	link->address_count = 0;
	link->next_address = 0;

	/* Sends small writes at once, as on a local forwarding's connection. */
	if (accept)
		(void)kl_socket_set_option(link->socket, KL_SOCKET_NO_DELAY, 1);

	/* Answers the server. */
	error = ssh_engine_open_answer(forwards->engine, link->channel, accept);

	/* A refusal, or an answer that could not go, ends the entry (the engine freed the number). */
	if (!accept || error != 0) {
		forward_release(link);
		return;
	}

	/* The connection carries bytes from now on. */
	link->state = FORWARD_LINK_OPEN;
}

/*
 * Reads a forwarded connection's socket into its chunk, noting its end.
 */
static void
forward_read(
	struct forward_state *forwards,
	struct forward_link *link)
{
	size_t received;
	int error;

	/* Reads what is there. */
	error = kl_socket_receive(link->socket, link->chunk, sizeof(link->chunk), &received);
	if (error == EAGAIN)
		return;

	/* A failed socket ends the connection. */
	if (error != 0) {
		forward_abort(forwards, link);
		return;
	}

	/* The socket's end goes to the server once the bytes before it went. */
	if (received == 0) {
		link->socket_ended = 1;
		forward_settle(forwards, link);
		return;
	}

	/* Keeps the bytes until the channel takes them. */
	link->chunk_size = received;
	link->chunk_offset = 0;
}

/*
 * Writes the server's queued bytes to a forwarded connection's socket and
 * gives the channel's window back for what was written.
 */
static void
forward_write(
	struct forward_state *forwards,
	struct forward_link *link)
{
	size_t size;
	size_t sent;
	int error;

	/* Writes as much as the socket takes. */
	size = link->queue.size - link->queue_offset;
	error = kl_socket_send(link->socket, link->queue.data + link->queue_offset, size, &sent);
	if (error == EAGAIN)
		return;

	/* A socket that cannot be written ends the connection. */
	if (error != 0) {
		forward_abort(forwards, link);
		return;
	}

	/* Steps over what was written, emptying the queue when all of it is, or every half window. */
	link->queue_offset += sent;
	if (link->queue_offset == link->queue.size) {
		ssh_buf_reset(&link->queue);
		link->queue_offset = 0;
	} else if (link->queue_offset >= SSH_ENGINE_FORWARD_WINDOW / 2U) {
		ssh_buf_consume(&link->queue, link->queue_offset);
		link->queue_offset = 0;
	}

	/* Gives the window back for what was written, while the channel is there. */
	if (!link->channel_gone)
		ssh_engine_channel_consumed(forwards->engine, link->channel, sent);

	/* A channel that ended is closed once its last bytes are written. */
	forward_settle(forwards, link);
}

/*
 * Sends what is due on an open forwarded connection: the EOF after the
 * socket's last byte, the shut of the socket after the server's last
 * byte, and the close once both ways ended.
 */
static void
forward_settle(
	struct forward_state *forwards,
	struct forward_link *link)
{
	int error;

	/* A channel the engine ended leaves only its bytes to write; the connection closes after them. */
	if (link->channel_gone) {
		if (link->queue.size == link->queue_offset)
			forward_release(link);
		return;
	}

	/* Tells the server the socket ended, once every byte before the end went. */
	if (link->socket_ended &&
	    link->chunk_size == 0 &&
	    !link->eof_sent) {
		error = ssh_engine_channel_eof(forwards->engine, link->channel);
		if (error != 0) {
			forward_abort(forwards, link);
			return;
		}
		link->eof_sent = 1;
	}

	/* Shuts the socket's sending side once the server's last byte is written. */
	if (link->channel_ended &&
	    link->queue.size == link->queue_offset &&
	    !link->shut) {
		(void)kl_socket_shutdown(link->socket, KL_SOCKET_SHUT_SEND);
		link->shut = 1;
	}

	/* Both ways ended: closes the socket and the channel, whose end the engine tells later. */
	if (link->eof_sent && link->shut) {
		kl_socket_close(link->socket);
		link->socket = NULL;
		ssh_buf_reset(&link->queue);
		link->queue_offset = 0;
		link->state = FORWARD_LINK_CLOSING;

		/* Closes the channel; one that cannot be closed frees the entry now. */
		error = ssh_engine_channel_close(forwards->engine, link->channel);
		if (error != 0)
			forward_release(link);
	}
}

/*
 * Ends a forwarded connection at once (its socket failed, or its channel
 * cannot be written): closes the socket and the channel, dropping what
 * waits either way.
 */
static void
forward_abort(
	struct forward_state *forwards,
	struct forward_link *link)
{
	int error;

	/* Closes the socket and drops the bytes both ways. */
	kl_socket_close(link->socket);
	link->socket = NULL;
	link->chunk_size = 0;
	link->chunk_offset = 0;
	ssh_buf_reset(&link->queue);
	link->queue_offset = 0;

	/* A channel the engine ended needs no close. */
	if (link->channel_gone) {
		forward_release(link);
		return;
	}

	/* Closes the channel; the entry is freed when the engine says it ended, or now when the close cannot go. */
	link->state = FORWARD_LINK_CLOSING;
	error = ssh_engine_channel_close(forwards->engine, link->channel);
	if (error != 0)
		forward_release(link);
}

/*
 * Frees a forwarded connection's entry: closes its socket and empties its
 * queue (keeping the queue's memory for the next connection).
 */
static void
forward_release(
	struct forward_link *link)
{
	/* Closes the socket. */
	kl_socket_close(link->socket);
	link->socket = NULL;

	/* Forgets the destination's addresses. */
	link->address_count = 0;
	link->next_address = 0;

	/* Forgets the bytes either way. */
	memset(link->chunk, 0, link->chunk_size);
	link->chunk_size = 0;
	link->chunk_offset = 0;
	ssh_buf_reset(&link->queue);
	link->queue_offset = 0;

	/* The entry is free. */
	link->state = FORWARD_LINK_FREE;
	link->wait = -1;
}

/*
 * Finds the forwarded connection that holds channel (NULL when none).
 */
static struct forward_link *
forward_find_link(
	struct forward_state *forwards,
	uint32_t channel)
{
	unsigned index;

	/* Looks at every entry in use. */
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		if (forwards->links[index].state == FORWARD_LINK_FREE)
			continue;
		if (forwards->links[index].channel == channel)
			return &forwards->links[index];
	}

	/* No connection holds the channel. */
	return NULL;
}

/*
 * Takes a free entry for a new connection of the forwarding at forward,
 * starting it with no socket and nothing either way (NULL when the table
 * is full).
 */
static struct forward_link *
forward_take_link(
	struct forward_state *forwards,
	unsigned forward)
{
	struct forward_link *link;
	unsigned index;

	/* Finds a free entry. */
	link = NULL;
	for (index = 0; index < FORWARD_LINKS_MAX; index++) {
		if (forwards->links[index].state == FORWARD_LINK_FREE) {
			link = &forwards->links[index];
			break;
		}
	}

	/* Reports a full table. */
	if (link == NULL)
		return NULL;

	/* Starts the entry; its state is set by the caller once the connection has a channel. */
	link->state = FORWARD_LINK_CONNECTING;
	link->socket = NULL;
	link->channel = 0;
	link->forward = forward;
	link->chunk_size = 0;
	link->chunk_offset = 0;
	link->socket_ended = 0;
	link->eof_sent = 0;
	ssh_buf_reset(&link->queue);
	link->queue_offset = 0;
	link->channel_ended = 0;
	link->shut = 0;
	link->channel_gone = 0;
	link->address_count = 0;
	link->next_address = 0;
	link->deadline = 0;
	link->wait = -1;

	/* Succeeded: the entry is the caller's. */
	return link;
}

/*
 * Finds the remote forwarding the engine numbers number (NULL when none).
 */
static const struct forward_remote *
forward_find_remote(
	const struct forward_state *forwards,
	unsigned number)
{
	unsigned index;

	/* Looks at every remote forwarding asked. */
	for (index = 0; index < forwards->remote_count; index++) {
		if (forwards->remotes[index].number == number)
			return &forwards->remotes[index];
	}

	/* The engine names no forwarding asked here. */
	return NULL;
}

/*
 * Writes an address in numbers without its port and without brackets
 * ("127.0.0.1", "::1"); "?" when it cannot be written.
 */
static void
forward_address_text(
	const struct kl_address *address,
	char *text,
	size_t size)
{
	char written[KL_ADDRESS_TEXT];
	char *end;
	const char *start;
	size_t length;
	int error;

	/* Writes the address with its port ("192.0.2.1:22", "[2001:db8::1]:22"). */
	error = kl_address_format(address, written, sizeof(written));
	if (error != 0) {
		(void)snprintf(text, size, "?");
		return;
	}

	/* The address ends before the last ':', and an IPv6 one is inside brackets. */
	start = written;
	end = strrchr(written, ':');
	if (end == NULL) {
		(void)snprintf(text, size, "?");
		return;
	}
	if (written[0] == '[') {
		start = written + 1;
		end--;
	}

	/* Copies the address alone. */
	length = (size_t)(end - start);
	if (length >= size)
		length = size - 1U;
	memcpy(text, start, length);
	text[length] = '\0';
}

/*
 * Names an error of the network as OpenSSH's lines name it (the words of
 * the C library of the systems it runs on).
 */
static const char *
forward_error_text(
	int error)
{
	/* The errors a forwarding meets. */
	switch (error) {
	case EADDRINUSE:
		return "Address already in use";
	case EADDRNOTAVAIL:
		return "Cannot assign requested address";
	case EACCES:
		return "Permission denied";
	case EAFNOSUPPORT:
		return "Address family not supported by protocol";
	case EMFILE:
		return "Too many open files";
	case ENFILE:
		return "Too many open files in system";
	case ENOMEM:
		return "Cannot allocate memory";
	case ENOENT:
		return "Name or service not known";
	case EAGAIN:
		return "Temporary failure in name resolution";
	case EINVAL:
		return "Invalid argument";
	default:
		return "Input/output error";
	}
}

/*
 * Shows a line to the user.
 */
static void
forward_say(
	struct forward_state *forwards,
	const char *line)
{
	/* Hands the line to the connection's thread, which shows it in the tab. */
	forwards->say(forwards->data, line);
}
