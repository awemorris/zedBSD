/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The port forwardings of the ssh program (ssh(1) -L and -R,
 * ssh_config(5) LocalForward and RemoteForward): the sockets that listen
 * for local forwardings, the remote forwardings asked of the server, and
 * the bytes of each forwarded connection between its socket and its
 * channel.
 *
 * The engine owns the channels and their windows (ssh-client-engine.h);
 * the sockets are here, in the program's one poll loop.  A local
 * forwarding listens where OpenSSH's ssh listens (the loopback addresses
 * unless GatewayPorts or a bind address says otherwise) and opens a
 * direct-tcpip channel for each connection it accepts.  A remote
 * forwarding's forwarded-tcpip channel is answered once the connection to
 * its destination is made.  The messages are OpenSSH's ssh's
 * (ssh_init_forwarding, channel_setup_fwd_listener_tcpip, connect_to and
 * ssh_confirm_remote_forward in openssh-portable).
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ssh-client-common.h"

/* The longest numeric port text with its NUL. */
#define FORWARD_PORT_TEXT	8U

/* The longest numeric address text with its NUL (an IPv6 address with a scope). */
#define FORWARD_ADDRESS_TEXT	NI_MAXHOST

/* The address a remote forwarding without a bind address asks the server to listen on (OpenSSH's channel_rfwd_bind_host). */
#define FORWARD_REMOTE_DEFAULT	"localhost"

/*
 * One address a local forwarding listens on, as getaddrinfo gave it.
 */
struct forward_address {
	struct sockaddr_storage address;
	socklen_t length;
	int family;
};

static int forward_listen_local(struct ssh_client_forwards *forwards, unsigned index);
static int forward_bind_addresses(struct ssh_client_forwards *forwards, const struct ssh_forward *forward, struct forward_address *addresses, unsigned *count);
static int forward_add_addresses(struct ssh_client_forwards *forwards, const char *host, const char *port, int flags, int required, struct forward_address *addresses, unsigned *count);
static int forward_listen_one(struct ssh_client_forwards *forwards, unsigned index, const struct forward_address *address, int last);
static int forward_ask_remote(struct ssh_client_forwards *forwards, unsigned index);
static void forward_accept(struct ssh_client_forwards *forwards, struct ssh_client_listener *listener, uint64_t now_ms);
static void forward_take_data(struct ssh_client_forwards *forwards, const struct ssh_engine_event *event);
static void forward_take_eof(struct ssh_client_forwards *forwards, const struct ssh_engine_event *event);
static void forward_take_closed(struct ssh_client_forwards *forwards, const struct ssh_engine_event *event);
static int forward_take_reply(struct ssh_client_forwards *forwards, const struct ssh_engine_event *event);
static void forward_take_open(struct ssh_client_forwards *forwards, const struct ssh_engine_event *event);
static void forward_take_open_reply(struct ssh_client_forwards *forwards, const struct ssh_engine_event *event);
static void forward_connect_next(struct ssh_client_forwards *forwards, struct ssh_client_link *link);
static void forward_connected(struct ssh_client_forwards *forwards, struct ssh_client_link *link);
static void forward_answer(struct ssh_client_forwards *forwards, struct ssh_client_link *link, int accept);
static void forward_read(struct ssh_client_forwards *forwards, struct ssh_client_link *link);
static void forward_write(struct ssh_client_forwards *forwards, struct ssh_client_link *link);
static void forward_settle(struct ssh_client_forwards *forwards, struct ssh_client_link *link);
static void forward_abort(struct ssh_client_forwards *forwards, struct ssh_client_link *link);
static void forward_release(struct ssh_client_link *link);
static struct ssh_client_link *forward_find_link(struct ssh_client_forwards *forwards, uint32_t channel);
static struct ssh_client_link *forward_take_link(struct ssh_client_forwards *forwards, unsigned forward);
static const struct ssh_client_remote *forward_find_remote(const struct ssh_client_forwards *forwards, unsigned number);
static void forward_name_address(const struct sockaddr *address, socklen_t length, char *host, size_t host_capacity, char *port, size_t port_capacity);
static int forward_prepare_socket(int descriptor);

/*
 * Starts the forwardings of a connection.
 */
void
ssh_client_forwards_init(
	struct ssh_client_forwards *forwards,
	struct ssh_client *client)
{
	unsigned index;

	/* Nothing listens and nothing is asked yet. */
	forwards->client = client;
	forwards->listening = 0;
	forwards->listener_count = 0;
	forwards->remote_count = 0;

	/* Every forwarded connection's entry is free, its socket closed and its queue empty. */
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
		forwards->links[index].state = SSH_CLIENT_LINK_FREE;
		forwards->links[index].socket = -1;
		forwards->links[index].addresses = NULL;
		forwards->links[index].next_address = NULL;
		forwards->links[index].slot = -1;
		ssh_buf_init(&forwards->links[index].queue, SSH_CLIENT_LINK_QUEUE_MAX, 1);
	}
}

/*
 * Sets up the forwardings once the user is authenticated.
 */
int
ssh_client_forwards_start(
	struct ssh_client_forwards *forwards)
{
	struct ssh_client *client;
	const struct ssh_config *config;
	unsigned index;
	unsigned asked;
	unsigned listened;
	int status;

	/* Takes the connection and its settings. */
	client = forwards->client;
	config = &client->config;

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
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Could not request local forwarding.");
		return SSH_CLIENT_FORWARD_FATAL;
	}

	/* Without it, the connection goes on unless none could listen, which is said. */
	if (asked > 0 && listened == 0)
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Could not request local forwarding.");

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
 * Closes the listening sockets.
 */
void
ssh_client_forwards_stop_listening(
	struct ssh_client_forwards *forwards)
{
	unsigned index;

	/* Closes every listening socket; the connections they took go on. */
	for (index = 0; index < forwards->listener_count; index++) {
		close(forwards->listeners[index].socket);
		forwards->listeners[index].socket = -1;
		forwards->listeners[index].slot = -1;
	}

	/* No more connections are taken. */
	forwards->listener_count = 0;
	forwards->listening = 0;
}

/*
 * Takes an event of the engine that belongs to the forwardings.
 */
int
ssh_client_forwards_event(
	struct ssh_client_forwards *forwards,
	const struct ssh_engine_event *event,
	int *taken)
{
	int status;

	/* Takes the event unless it turns out to be the session's or of no forwarding. */
	*taken = 1;
	status = 0;

	/* Acts by the kind of the event. */
	switch (event->kind) {
	case SSH_ENGINE_DATA:
		/* The session's data is the session loop's. */
		if (event->channel == SSH_ENGINE_SESSION_CHANNEL) {
			*taken = 0;
			break;
		}

		/* Queues the data for its connection's socket. */
		forward_take_data(forwards, event);
		break;
	case SSH_ENGINE_CHANNEL_EOF:
		forward_take_eof(forwards, event);
		break;
	case SSH_ENGINE_CHANNEL_CLOSED:
		forward_take_closed(forwards, event);
		break;
	case SSH_ENGINE_FORWARD_REPLY:
		status = forward_take_reply(forwards, event);
		break;
	case SSH_ENGINE_FORWARD_OPEN:
		forward_take_open(forwards, event);
		break;
	case SSH_ENGINE_OPEN_REPLY:
		forward_take_open_reply(forwards, event);
		break;
	default:
		/* Any other event is the session loop's. */
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
 * Moves the forwarded connections' bytes into the engine and sends the
 * EOFs and closes that are due.
 */
void
ssh_client_forwards_pump(
	struct ssh_client_forwards *forwards)
{
	struct ssh_client_link *link;
	size_t written;
	unsigned index;
	int error;

	/* Gives each open connection's waiting bytes to its channel. */
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
		link = &forwards->links[index];
		if (link->state != SSH_CLIENT_LINK_OPEN)
			continue;

		/* A channel the engine ended takes nothing more. */
		if (link->channel_gone) {
			forward_settle(forwards, link);
			continue;
		}

		/* Gives the bytes while the channel's window and the engine's output take them. */
		error = 0;
		while (link->chunk_offset < link->chunk_size) {
			error = ssh_engine_channel_write(forwards->client->engine, link->channel, link->chunk + link->chunk_offset, link->chunk_size - link->chunk_offset, &written);
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
 * Puts the sockets the forwardings wait on into the poll slots.
 */
unsigned
ssh_client_forwards_poll(
	struct ssh_client_forwards *forwards,
	struct pollfd *slots,
	unsigned capacity,
	uint64_t now_ms,
	uint64_t *wake_ms)
{
	struct ssh_client_listener *listener;
	struct ssh_client_link *link;
	unsigned count;
	unsigned index;
	short events;

	/* Watches each listening socket for a connection, unless it rests after a lack of descriptors. */
	count = 0;
	for (index = 0; index < forwards->listener_count; index++) {
		listener = &forwards->listeners[index];
		listener->slot = -1;

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

		/* Takes a slot for the listener. */
		slots[count].fd = listener->socket;
		slots[count].events = POLLIN;
		slots[count].revents = 0;
		listener->slot = (int)count;
		count++;
	}

	/* Watches each forwarded connection for what it waits for. */
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
		link = &forwards->links[index];
		link->slot = -1;
		if (link->state == SSH_CLIENT_LINK_FREE || link->socket < 0)
			continue;

		/* Chooses the events: a connection being made, bytes to read, room to write. */
		events = 0;
		if (link->state == SSH_CLIENT_LINK_CONNECTING) {
			events |= POLLOUT;
			if (link->deadline < *wake_ms)
				*wake_ms = link->deadline;
		} else if (link->state == SSH_CLIENT_LINK_OPEN) {
			if (!link->channel_gone &&
			    link->chunk_size == 0 &&
			    !link->socket_ended)
				events |= POLLIN;
			if (link->queue.size > link->queue_offset)
				events |= POLLOUT;
		}

		/* A connection that waits for nothing here (the server's answer) takes no slot. */
		if (events == 0 || count >= capacity)
			continue;

		/* Takes a slot for the connection. */
		slots[count].fd = link->socket;
		slots[count].events = events;
		slots[count].revents = 0;
		link->slot = (int)count;
		count++;
	}

	/* Reports how many slots were filled. */
	return count;
}

/*
 * Acts on what poll reported for the forwardings' slots.
 */
void
ssh_client_forwards_serve(
	struct ssh_client_forwards *forwards,
	const struct pollfd *slots,
	uint64_t now_ms)
{
	struct ssh_client_link *link;
	unsigned index;
	short revents;

	/* Accepts a connection on each listening socket that has one. */
	for (index = 0; index < forwards->listener_count; index++) {
		if (forwards->listeners[index].slot < 0)
			continue;
		revents = slots[forwards->listeners[index].slot].revents;
		if ((revents & (POLLIN | POLLERR | POLLHUP)) != 0)
			forward_accept(forwards, &forwards->listeners[index], now_ms);
	}

	/* Moves each forwarded connection on. */
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
		link = &forwards->links[index];

		/* Gives up a connection to a destination that did not answer in time. */
		if (link->state == SSH_CLIENT_LINK_CONNECTING && now_ms >= link->deadline) {
			close(link->socket);
			link->socket = -1;
			link->next_address = NULL;
			forward_connect_next(forwards, link);
			continue;
		}

		/* Passes over a connection poll did not watch. */
		if (link->slot < 0)
			continue;
		revents = slots[link->slot].revents;

		/* A connection being made has its outcome when the socket is writable. */
		if (link->state == SSH_CLIENT_LINK_CONNECTING) {
			if ((revents & (POLLOUT | POLLERR | POLLHUP)) != 0)
				forward_connected(forwards, link);
			continue;
		}

		/* Writes the server's bytes when the socket takes them, or failed. */
		if ((slots[link->slot].events & POLLOUT) != 0 &&
		    (revents & (POLLOUT | POLLERR | POLLHUP)) != 0)
			forward_write(forwards, link);

		/* Reads the socket when it has bytes, ended or failed, unless the write ended the connection. */
		if (link->state == SSH_CLIENT_LINK_OPEN &&
		    (slots[link->slot].events & POLLIN) != 0 &&
		    (revents & (POLLIN | POLLERR | POLLHUP)) != 0)
			forward_read(forwards, link);
	}
}

/*
 * Reports whether a forwarded connection still has bytes to write.
 */
int
ssh_client_forwards_draining(
	const struct ssh_client_forwards *forwards)
{
	const struct ssh_client_link *link;
	unsigned index;

	/* Looks for a socket with the server's bytes not yet written. */
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
		link = &forwards->links[index];
		if (link->state == SSH_CLIENT_LINK_FREE || link->socket < 0)
			continue;
		if (link->queue.size > link->queue_offset)
			return 1;
	}

	/* Every socket has had all its bytes. */
	return 0;
}

/*
 * Closes every socket of the forwardings and frees what they hold.
 */
void
ssh_client_forwards_free(
	struct ssh_client_forwards *forwards)
{
	unsigned index;

	/* Closes the listening sockets. */
	ssh_client_forwards_stop_listening(forwards);

	/* Closes every forwarded connection and frees its queue. */
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
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
	struct ssh_client_forwards *forwards,
	unsigned index)
{
	struct forward_address addresses[SSH_CLIENT_LISTEN_ADDRESSES];
	const struct ssh_forward *forward;
	const char *listen_name;
	unsigned count;
	unsigned position;
	int listened;
	int status;

	/* Takes the forwarding. */
	forward = &forwards->client->config.forward[index];

	/* Names where it listens as OpenSSH's ssh does at the debug level. */
	listen_name = forward->listen_host;
	if (!forward->have_listen_host && forwards->client->config.gateway_ports) {
		listen_name = "*";
	} else if (!forward->have_listen_host) {
		listen_name = "LOCALHOST";
	} else if (forward->listen_host[0] == '\0') {
		listen_name = "*";
	}
	ssh_client_log(forwards->client, SSH_CLIENT_LOG_VERBOSE, "Local connections to %s:%u forwarded to remote address %s:%u", listen_name, (unsigned)forward->listen_port, forward->connect_host, (unsigned)forward->connect_port);

	/* Finds the addresses to listen on. */
	status = forward_bind_addresses(forwards, forward, addresses, &count);
	if (status != 0)
		return 0;

	/* Listens on each; only a failure on the last address is said as an error. */
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
	struct ssh_client_forwards *forwards,
	const struct ssh_forward *forward,
	struct forward_address *addresses,
	unsigned *count)
{
	char port[FORWARD_PORT_TEXT];
	int wildcard;
	int loopback;
	int compare;
	int status;

	/* Writes the port in digits. */
	sprintf(port, "%u", (unsigned)forward->listen_port);
	*count = 0;

	/* Decides between every address, the loopback addresses, and the one named. */
	wildcard = 0;
	loopback = 0;
	if (!forward->have_listen_host) {
		/* No bind address: GatewayPorts decides. */
		if (forwards->client->config.gateway_ports) {
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
		(void)forward_add_addresses(forwards, "0.0.0.0", port, AI_NUMERICHOST | AI_PASSIVE, 0, addresses, count);
		(void)forward_add_addresses(forwards, "::", port, AI_NUMERICHOST | AI_PASSIVE, 0, addresses, count);
	} else if (loopback) {
		/* The loopback address of both families. */
		(void)forward_add_addresses(forwards, "127.0.0.1", port, AI_NUMERICHOST, 0, addresses, count);
		(void)forward_add_addresses(forwards, "::1", port, AI_NUMERICHOST, 0, addresses, count);
	} else {
		/* The address named, looked up when it is a name. */
		status = forward_add_addresses(forwards, forward->listen_host, port, 0, 1, addresses, count);
		if (status != 0)
			return status;
	}

	/* Refuses a forwarding with nowhere to listen. */
	if (*count == 0)
		return ENOENT;

	/* Succeeded: *count addresses to listen on. */
	return 0;
}

/*
 * Appends the stream addresses getaddrinfo gives for host and port (with
 * flags) to addresses, up to SSH_CLIENT_LISTEN_ADDRESSES in all.  A
 * failed lookup is said as an error when required, at the verbose level
 * otherwise (a family the system does not have).  Returns 0 or ENOENT.
 */
static int
forward_add_addresses(
	struct ssh_client_forwards *forwards,
	const char *host,
	const char *port,
	int flags,
	int required,
	struct forward_address *addresses,
	unsigned *count)
{
	struct addrinfo hints;
	struct addrinfo *found;
	struct addrinfo *entry;
	int level;
	int status;

	/* Asks for stream addresses of any family. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = flags;

	/* Looks the host up. */
	found = NULL;
	status = getaddrinfo(host, port, &hints, &found);
	if (status != 0) {
		level = SSH_CLIENT_LOG_VERBOSE;
		if (required)
			level = SSH_CLIENT_LOG_QUIET;
		ssh_client_log(forwards->client, level, "getaddrinfo(%s): %s", host, gai_strerror(status));
		return ENOENT;
	}

	/* Keeps each IPv4 and IPv6 address while there is room. */
	for (entry = found;
	     entry != NULL;
	     entry = entry->ai_next) {
		if (*count >= SSH_CLIENT_LISTEN_ADDRESSES)
			break;
		if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6)
			continue;
		if (entry->ai_addrlen > sizeof(addresses[*count].address))
			continue;

		/* Copies the address. */
		memset(&addresses[*count], 0, sizeof(addresses[*count]));
		memcpy(&addresses[*count].address, entry->ai_addr, entry->ai_addrlen);
		addresses[*count].length = entry->ai_addrlen;
		addresses[*count].family = entry->ai_family;
		(*count)++;
	}

	/* Frees the lookup's list. */
	freeaddrinfo(found);

	/* Succeeded: the addresses found are appended. */
	return 0;
}

/*
 * Makes one socket that listens on address for the local forwarding at
 * index.  A failed bind is an error when last (OpenSSH says the earlier
 * ones only verbosely: the other family may have the port).  Returns 1
 * when it listens, 0 when not.
 */
static int
forward_listen_one(
	struct ssh_client_forwards *forwards,
	unsigned index,
	const struct forward_address *address,
	int last)
{
	struct ssh_client_listener *listener;
	char host[FORWARD_ADDRESS_TEXT];
	char port[FORWARD_PORT_TEXT];
	int descriptor;
	int level;
	int status;
	int error;
	int on;

	/* Writes the address in numbers for the messages. */
	forward_name_address((const struct sockaddr *)&address->address, address->length, host, sizeof(host), port, sizeof(port));

	/* Refuses a socket the table has no room for (the table holds four for each forwarding). */
	if (forwards->listener_count >= SSH_CLIENT_LISTENERS_MAX)
		return 0;

	/* Makes a socket of the address's family; a family the system lacks is no error. */
	descriptor = socket(address->family, SOCK_STREAM, 0);
	if (descriptor < 0) {
		ssh_client_log(forwards->client, SSH_CLIENT_LOG_VERBOSE, "socket [%s]:%s: %s", host, port, strerror(errno));
		return 0;
	}

	/* Takes the port again at once after an earlier listener on it closed, and keeps IPv6 to IPv6. */
	on = 1;
	(void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if (address->family == AF_INET6)
		(void)setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));

	/* Binds the address; the last address's failure is an error, the others' verbose. */
	status = bind(descriptor, (const struct sockaddr *)&address->address, address->length);
	if (status != 0) {
		error = errno;
		level = SSH_CLIENT_LOG_VERBOSE;
		if (last)
			level = SSH_CLIENT_LOG_QUIET;
		ssh_client_log(forwards->client, level, "bind [%s]:%s: %s", host, port, strerror(error));
		close(descriptor);
		return 0;
	}

	/* Starts listening for connections. */
	status = listen(descriptor, SSH_CLIENT_LISTEN_BACKLOG);
	if (status != 0) {
		error = errno;
		ssh_client_log(forwards->client, SSH_CLIENT_LOG_QUIET, "listen [%s]:%s: %s", host, port, strerror(error));
		close(descriptor);
		return 0;
	}

	/* Keeps the socket from programs and from blocking the loop. */
	error = forward_prepare_socket(descriptor);
	if (error != 0) {
		close(descriptor);
		return 0;
	}

	/* Keeps the listener. */
	listener = &forwards->listeners[forwards->listener_count];
	listener->socket = descriptor;
	listener->forward = index;
	listener->paused_until = 0;
	listener->slot = -1;
	forwards->listener_count++;
	ssh_client_log(forwards->client, SSH_CLIENT_LOG_DEBUG, "Local forwarding listening on %s port %s.", host, port);

	/* Succeeded: the socket listens. */
	return 1;
}

/*
 * Asks the server for the remote forwarding at index (tcpip-forward):
 * "localhost" when no bind address was given, as OpenSSH's ssh sends.
 * Returns 0, or SSH_CLIENT_FORWARD_FATAL when the request could not go
 * and ExitOnForwardFailure ends the connection.
 */
static int
forward_ask_remote(
	struct ssh_client_forwards *forwards,
	unsigned index)
{
	struct ssh_client *client;
	const struct ssh_forward *forward;
	const char *address;
	const char *listen_name;
	unsigned number;
	int error;

	/* Takes the connection and the forwarding. */
	client = forwards->client;
	forward = &client->config.forward[index];

	/* Chooses the address the server is asked to listen on. */
	address = FORWARD_REMOTE_DEFAULT;
	listen_name = "LOCALHOST";
	if (forward->have_listen_host) {
		address = forward->listen_host;
		listen_name = forward->listen_host;
	}
	ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "Remote connections from %s:%u forwarded to local address %s:%u", listen_name, (unsigned)forward->listen_port, forward->connect_host, (unsigned)forward->connect_port);

	/* Sends the request; its answer comes as SSH_ENGINE_FORWARD_REPLY. */
	error = ssh_engine_forward_listen(client->engine, address, forward->listen_port, &number);
	if (error == 0 && forwards->remote_count < SSH_CONFIG_FORWARDS_MAX) {
		forwards->remotes[forwards->remote_count].forward = index;
		forwards->remotes[forwards->remote_count].number = number;
		forwards->remote_count++;
		return 0;
	}

	/* A request that could not go ends the connection with ExitOnForwardFailure. */
	if (client->config.exit_on_forward_failure) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Could not request remote forwarding.");
		return SSH_CLIENT_FORWARD_FATAL;
	}

	/* Without it, the connection goes on. */
	ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Warning: Could not request remote forwarding.");
	return 0;
}

/*
 * Accepts one connection on a listening socket and opens a direct-tcpip
 * channel for it to the forwarding's destination.
 */
static void
forward_accept(
	struct ssh_client_forwards *forwards,
	struct ssh_client_listener *listener,
	uint64_t now_ms)
{
	struct sockaddr_storage peer;
	struct ssh_client_link *link;
	const struct ssh_forward *forward;
	char origin[FORWARD_ADDRESS_TEXT];
	char origin_port[FORWARD_PORT_TEXT];
	socklen_t length;
	uint32_t channel;
	unsigned long port;
	int descriptor;
	int error;
	int on;

	/* Takes the forwarding the socket listens for. */
	forward = &forwards->client->config.forward[listener->forward];

	/* Accepts the connection. */
	length = sizeof(peer);
	descriptor = accept(listener->socket, (struct sockaddr *)&peer, &length);
	if (descriptor < 0) {
		error = errno;

		/* Rests the listener for a while when descriptors ran out, leaving the connection in the backlog. */
		if (error == EMFILE || error == ENFILE) {
			listener->paused_until = now_ms + SSH_CLIENT_ACCEPT_PAUSE_MS;
			ssh_client_log(forwards->client, SSH_CLIENT_LOG_VERBOSE, "accept: %s", strerror(error));
		}

		/* Nothing more to do: a connection that went away, an interrupt, or a rest. */
		return;
	}

	/* Keeps the socket from programs and from blocking the loop, and sends small messages at once. */
	error = forward_prepare_socket(descriptor);
	if (error != 0) {
		close(descriptor);
		return;
	}
	on = 1;
	(void)setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

	/* Names where the connection came from, which the channel tells the server. */
	forward_name_address((const struct sockaddr *)&peer, length, origin, sizeof(origin), origin_port, sizeof(origin_port));
	port = strtoul(origin_port, NULL, 10);
	ssh_client_log(forwards->client, SSH_CLIENT_LOG_DEBUG, "Connection to port %u forwarding to %s port %u requested.", (unsigned)forward->listen_port, forward->connect_host, (unsigned)forward->connect_port);

	/* Takes an entry for the connection; one too many is closed. */
	link = forward_take_link(forwards, listener->forward);
	if (link == NULL) {
		ssh_client_log(forwards->client, SSH_CLIENT_LOG_VERBOSE, "ssh: too many forwarded connections; closing one from %s", origin);
		close(descriptor);
		return;
	}

	/* Opens the channel; its answer comes as SSH_ENGINE_OPEN_REPLY. */
	error = ssh_engine_open_direct(forwards->client->engine, forward->connect_host, forward->connect_port, origin, (unsigned)port, &channel);
	if (error != 0) {
		ssh_client_log(forwards->client, SSH_CLIENT_LOG_VERBOSE, "ssh: cannot open a channel for a connection from %s: %s", origin, strerror(error));
		close(descriptor);
		link->state = SSH_CLIENT_LINK_FREE;
		return;
	}

	/* The connection waits for the server's answer before any byte goes. */
	link->socket = descriptor;
	link->channel = channel;
	link->state = SSH_CLIENT_LINK_OPENING;
}

/*
 * Queues a forwarded channel's data for its socket.
 */
static void
forward_take_data(
	struct ssh_client_forwards *forwards,
	const struct ssh_engine_event *event)
{
	struct ssh_client_link *link;
	int error;

	/* Data of a channel no connection holds is used at once, so its window goes back. */
	link = forward_find_link(forwards, event->channel);
	if (link == NULL || link->socket < 0) {
		ssh_engine_channel_consumed(forwards->client->engine, event->channel, event->size);
		return;
	}

	/* Queues the bytes; a server that sent more than its window ends the connection. */
	ssh_buf_put_bytes(&link->queue, event->bytes, event->size);
	error = ssh_buf_error(&link->queue);
	if (error != 0) {
		ssh_client_log(forwards->client, SSH_CLIENT_LOG_VERBOSE, "ssh: channel %lu: too much data waits for its socket", (unsigned long)link->channel);
		forward_abort(forwards, link);
	}
}

/*
 * Notes that the server sends no more on a forwarded channel; the socket's
 * sending side is shut once the queued bytes are written.
 */
static void
forward_take_eof(
	struct ssh_client_forwards *forwards,
	const struct ssh_engine_event *event)
{
	struct ssh_client_link *link;

	/* Notes the end for the connection that holds the channel. */
	link = forward_find_link(forwards, event->channel);
	if (link == NULL)
		return;
	link->channel_ended = 1;
}

/*
 * Takes the end of a forwarded channel: the connection is closed once its
 * queued bytes are written.
 */
static void
forward_take_closed(
	struct ssh_client_forwards *forwards,
	const struct ssh_engine_event *event)
{
	struct ssh_client_link *link;

	/* Finds the connection that held the channel. */
	link = forward_find_link(forwards, event->channel);
	if (link == NULL)
		return;

	/* The channel is gone: nothing more is sent, and the bytes read but not sent are dropped. */
	link->channel_gone = 1;
	link->chunk_size = 0;
	link->chunk_offset = 0;

	/* Closes the connection now unless the server's last bytes still wait for its socket. */
	if (link->socket < 0 || link->queue.size == link->queue_offset) {
		forward_release(link);
		return;
	}

	/* Writes the rest first (a connection that was never opened has none). */
	link->state = SSH_CLIENT_LINK_OPEN;
}

/*
 * Takes the server's answer to a remote forwarding.  Returns 0, or
 * SSH_CLIENT_FORWARD_FATAL when a refusal ends the connection
 * (ExitOnForwardFailure).
 */
static int
forward_take_reply(
	struct ssh_client_forwards *forwards,
	const struct ssh_engine_event *event)
{
	struct ssh_client *client;
	const struct ssh_client_remote *remote;
	const struct ssh_forward *forward;

	/* Finds the forwarding the answer is for. */
	client = forwards->client;
	remote = forward_find_remote(forwards, event->forward);
	if (remote == NULL)
		return 0;
	forward = &client->config.forward[remote->forward];

	/* Tells the port the server chose for port 0. */
	if (event->accepted) {
		if (forward->listen_port == 0)
			ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Allocated port %u for remote forward to %s:%u", event->port, forward->connect_host, (unsigned)forward->connect_port);
		return 0;
	}

	/* A refusal ends the connection with ExitOnForwardFailure. */
	if (client->config.exit_on_forward_failure) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Error: remote port forwarding failed for listen port %u", (unsigned)forward->listen_port);
		return SSH_CLIENT_FORWARD_FATAL;
	}

	/* Without it, the refusal is told and the connection goes on. */
	ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Warning: remote port forwarding failed for listen port %u", (unsigned)forward->listen_port);
	return 0;
}

/*
 * Takes a forwarded-tcpip channel the server opened for a remote
 * forwarding: connects to the forwarding's destination, and answers the
 * server once the connection is made or failed.
 */
static void
forward_take_open(
	struct ssh_client_forwards *forwards,
	const struct ssh_engine_event *event)
{
	struct ssh_client *client;
	const struct ssh_client_remote *remote;
	const struct ssh_forward *forward;
	struct ssh_client_link *link;
	struct addrinfo hints;
	struct addrinfo *found;
	char port[FORWARD_PORT_TEXT];
	int status;

	/* Finds the forwarding; the engine opens only for the ones asked. */
	client = forwards->client;
	remote = forward_find_remote(forwards, event->forward);
	if (remote == NULL) {
		(void)ssh_engine_open_answer(client->engine, event->channel, 0);
		return;
	}
	forward = &client->config.forward[remote->forward];
	ssh_client_log(client, SSH_CLIENT_LOG_DEBUG, "ssh: forwarded connection to %s port %u from %s port %u", event->listen_address, event->listen_port, event->address, event->port);

	/* Takes an entry for the connection; one too many is refused. */
	link = forward_take_link(forwards, remote->forward);
	if (link == NULL) {
		ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: too many forwarded connections; refusing one from %s", event->address);
		(void)ssh_engine_open_answer(client->engine, event->channel, 0);
		return;
	}
	link->channel = event->channel;

	/* Looks the destination up (this waits, as OpenSSH's ssh does). */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	sprintf(port, "%u", (unsigned)forward->connect_port);
	found = NULL;
	status = getaddrinfo(forward->connect_host, port, &hints, &found);
	if (status != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "connect_to %s: unknown host (%s)", forward->connect_host, gai_strerror(status));
		forward_answer(forwards, link, 0);
		return;
	}

	/* Connects to the first address that takes it, within the time allowed. */
	link->state = SSH_CLIENT_LINK_CONNECTING;
	link->addresses = found;
	link->next_address = found;
	link->deadline = ssh_client_now_ms() + SSH_CLIENT_CONNECT_WAIT_MS;
	forward_connect_next(forwards, link);
}

/*
 * Takes the server's answer to a direct-tcpip channel: the connection's
 * bytes go from now on, or it is closed with the reason told.
 */
static void
forward_take_open_reply(
	struct ssh_client_forwards *forwards,
	const struct ssh_engine_event *event)
{
	struct ssh_client_link *link;

	/* Finds the connection that waits for the answer. */
	link = forward_find_link(forwards, event->channel);
	if (link == NULL || link->state != SSH_CLIENT_LINK_OPENING)
		return;

	/* An open channel carries the connection. */
	if (event->accepted) {
		link->state = SSH_CLIENT_LINK_OPEN;
		return;
	}

	/* A refused one is told and its connection closed; the engine freed the number. */
	ssh_client_log(forwards->client, SSH_CLIENT_LOG_INFO, "channel %lu: open failed: %s", (unsigned long)event->channel, event->reason);
	forward_release(link);
}

/*
 * Starts the connection to the next address of a remote forwarding's
 * destination, answering the server when one connects at once or none is
 * left.
 */
static void
forward_connect_next(
	struct ssh_client_forwards *forwards,
	struct ssh_client_link *link)
{
	const struct ssh_forward *forward;
	struct addrinfo *address;
	int descriptor;
	int status;
	int error;

	/* Tries each address left until one connects or is on its way. */
	while (link->next_address != NULL) {
		address = link->next_address;
		link->next_address = address->ai_next;

		/* Makes a socket of the address's family that does not block. */
		descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
		if (descriptor < 0)
			continue;
		error = forward_prepare_socket(descriptor);
		if (error != 0) {
			close(descriptor);
			continue;
		}

		/* Starts the connection. */
		status = connect(descriptor, address->ai_addr, address->ai_addrlen);
		if (status == 0) {
			link->socket = descriptor;
			forward_answer(forwards, link, 1);
			return;
		}

		/* Waits for a connection on its way. */
		if (errno == EINPROGRESS) {
			link->socket = descriptor;
			return;
		}

		/* Goes on with the next address. */
		ssh_client_log(forwards->client, SSH_CLIENT_LOG_DEBUG, "ssh: channel %lu: connection failed: %s", (unsigned long)link->channel, strerror(errno));
		close(descriptor);
	}

	/* No address took the connection: the server hears it failed. */
	forward = &forwards->client->config.forward[link->forward];
	ssh_client_log(forwards->client, SSH_CLIENT_LOG_QUIET, "connect_to %s port %u: failed.", forward->connect_host, (unsigned)forward->connect_port);
	forward_answer(forwards, link, 0);
}

/*
 * Takes the outcome of a connection on its way to a remote forwarding's
 * destination.
 */
static void
forward_connected(
	struct ssh_client_forwards *forwards,
	struct ssh_client_link *link)
{
	socklen_t length;
	int failure;
	int status;

	/* Reads how the connection ended. */
	failure = 0;
	length = sizeof(failure);
	status = getsockopt(link->socket, SOL_SOCKET, SO_ERROR, &failure, &length);
	if (status != 0)
		failure = errno;

	/* A made connection opens the channel. */
	if (failure == 0) {
		forward_answer(forwards, link, 1);
		return;
	}

	/* A failed one tries the next address. */
	ssh_client_log(forwards->client, SSH_CLIENT_LOG_DEBUG, "ssh: channel %lu: connection failed: %s", (unsigned long)link->channel, strerror(failure));
	close(link->socket);
	link->socket = -1;
	forward_connect_next(forwards, link);
}

/*
 * Answers the server's forwarded-tcpip channel: accept 1 opens it and
 * the connection carries bytes, 0 refuses it and frees the entry.
 */
static void
forward_answer(
	struct ssh_client_forwards *forwards,
	struct ssh_client_link *link,
	int accept)
{
	int error;

	/* The destination's addresses are no longer needed. */
	if (link->addresses != NULL)
		freeaddrinfo(link->addresses);
	link->addresses = NULL;
	link->next_address = NULL;

	/* Answers the server. */
	error = ssh_engine_open_answer(forwards->client->engine, link->channel, accept);

	/* A refusal, or an answer that could not go, ends the entry (the engine freed the number). */
	if (!accept || error != 0) {
		forward_release(link);
		return;
	}

	/* The connection carries bytes from now on. */
	link->state = SSH_CLIENT_LINK_OPEN;
}

/*
 * Reads a forwarded connection's socket into its chunk, noting its end.
 */
static void
forward_read(
	struct ssh_client_forwards *forwards,
	struct ssh_client_link *link)
{
	ssize_t count;

	/* Reads what is there. */
	count = read(link->socket, link->chunk, sizeof(link->chunk));
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return;

	/* A failed socket ends the connection. */
	if (count < 0) {
		forward_abort(forwards, link);
		return;
	}

	/* The socket's end goes to the server once the bytes before it went. */
	if (count == 0) {
		link->socket_ended = 1;
		return;
	}

	/* Keeps the bytes until the channel takes them. */
	link->chunk_size = (size_t)count;
	link->chunk_offset = 0;
}

/*
 * Writes the server's queued bytes to a forwarded connection's socket and
 * gives the channel's window back for what was written.
 */
static void
forward_write(
	struct ssh_client_forwards *forwards,
	struct ssh_client_link *link)
{
	ssize_t count;
	size_t size;

	/* Writes as much as the socket takes. */
	size = link->queue.size - link->queue_offset;
	count = write(link->socket, link->queue.data + link->queue_offset, size);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return;

	/* A socket that cannot be written ends the connection. */
	if (count <= 0) {
		forward_abort(forwards, link);
		return;
	}

	/* Steps over what was written, emptying the queue when all of it is, or every half window. */
	link->queue_offset += (size_t)count;
	if (link->queue_offset == link->queue.size) {
		ssh_buf_reset(&link->queue);
		link->queue_offset = 0;
	} else if (link->queue_offset >= SSH_ENGINE_FORWARD_WINDOW / 2U) {
		ssh_buf_consume(&link->queue, link->queue_offset);
		link->queue_offset = 0;
	}

	/* Gives the window back for what was written, while the channel is there. */
	if (!link->channel_gone)
		ssh_engine_channel_consumed(forwards->client->engine, link->channel, (size_t)count);

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
	struct ssh_client_forwards *forwards,
	struct ssh_client_link *link)
{
	struct ssh_engine *engine;
	int error;

	/* Takes the engine of the connection. */
	engine = forwards->client->engine;

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
		error = ssh_engine_channel_eof(engine, link->channel);
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
		(void)shutdown(link->socket, SHUT_WR);
		link->shut = 1;
	}

	/* Both ways ended: closes the socket and the channel, whose end the engine tells later. */
	if (link->eof_sent && link->shut) {
		close(link->socket);
		link->socket = -1;
		ssh_buf_reset(&link->queue);
		link->queue_offset = 0;
		link->state = SSH_CLIENT_LINK_CLOSING;
		error = ssh_engine_channel_close(engine, link->channel);
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
	struct ssh_client_forwards *forwards,
	struct ssh_client_link *link)
{
	int error;

	/* Closes the socket and drops the bytes both ways. */
	if (link->socket >= 0)
		close(link->socket);
	link->socket = -1;
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
	link->state = SSH_CLIENT_LINK_CLOSING;
	error = ssh_engine_channel_close(forwards->client->engine, link->channel);
	if (error != 0)
		forward_release(link);
}

/*
 * Frees a forwarded connection's entry: closes its socket and empties its
 * queue (keeping the queue's memory for the next connection).
 */
static void
forward_release(
	struct ssh_client_link *link)
{
	/* Closes the socket. */
	if (link->socket >= 0)
		close(link->socket);
	link->socket = -1;

	/* Frees the destination's addresses of a connection being made. */
	if (link->addresses != NULL)
		freeaddrinfo(link->addresses);
	link->addresses = NULL;
	link->next_address = NULL;

	/* Forgets the bytes either way. */
	ssh_wipe(link->chunk, link->chunk_size);
	link->chunk_size = 0;
	link->chunk_offset = 0;
	ssh_buf_reset(&link->queue);
	link->queue_offset = 0;

	/* The entry is free. */
	link->state = SSH_CLIENT_LINK_FREE;
	link->slot = -1;
}

/*
 * Finds the forwarded connection that holds channel (NULL when none).
 */
static struct ssh_client_link *
forward_find_link(
	struct ssh_client_forwards *forwards,
	uint32_t channel)
{
	unsigned index;

	/* Looks at every entry in use. */
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
		if (forwards->links[index].state == SSH_CLIENT_LINK_FREE)
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
static struct ssh_client_link *
forward_take_link(
	struct ssh_client_forwards *forwards,
	unsigned forward)
{
	struct ssh_client_link *link;
	unsigned index;

	/* Finds a free entry. */
	link = NULL;
	for (index = 0; index < SSH_CLIENT_LINKS_MAX; index++) {
		if (forwards->links[index].state == SSH_CLIENT_LINK_FREE) {
			link = &forwards->links[index];
			break;
		}
	}

	/* Reports a full table. */
	if (link == NULL)
		return NULL;

	/* Starts the entry; its state is set by the caller once the connection has a channel. */
	link->state = SSH_CLIENT_LINK_CONNECTING;
	link->socket = -1;
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
	link->addresses = NULL;
	link->next_address = NULL;
	link->deadline = 0;
	link->slot = -1;

	/* Succeeded: the entry is the caller's. */
	return link;
}

/*
 * Finds the remote forwarding the engine numbers number (NULL when none).
 */
static const struct ssh_client_remote *
forward_find_remote(
	const struct ssh_client_forwards *forwards,
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
 * Writes an address and its port in numbers ("?" and "0" when they
 * cannot be written).
 */
static void
forward_name_address(
	const struct sockaddr *address,
	socklen_t length,
	char *host,
	size_t host_capacity,
	char *port,
	size_t port_capacity)
{
	int status;

	/* Asks for the numeric forms. */
	status = getnameinfo(address, length, host, host_capacity, port, port_capacity, NI_NUMERICHOST | NI_NUMERICSERV);
	if (status == 0)
		return;

	/* Puts placeholders in when the address cannot be written. */
	snprintf(host, host_capacity, "?");
	snprintf(port, port_capacity, "0");
}

/*
 * Keeps a socket from the programs a session might start and from
 * blocking the loop.  Returns 0 or the error of the system.
 */
static int
forward_prepare_socket(
	int descriptor)
{
	int flags;
	int status;

	/* Closes the socket on exec. */
	status = fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	if (status < 0)
		return errno;

	/* Makes the socket not block. */
	flags = fcntl(descriptor, F_GETFL);
	if (flags < 0)
		return errno;
	status = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (status < 0)
		return errno;

	/* Succeeded: the socket is ready for the loop. */
	return 0;
}
