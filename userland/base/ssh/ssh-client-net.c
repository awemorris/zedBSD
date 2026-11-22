/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The connection of the ssh program: the TCP connection to the server,
 * the bytes between the socket and the engine, and the words for how a
 * connection ended.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ssh-client-common.h"

/* The longest numeric port text with its NUL. */
#define CLIENT_PORT_TEXT	8U

/* How the transport's reason for a failed negotiation starts. */
#define CLIENT_NO_MATCH		"no matching "

static int client_connect_one(const struct addrinfo *address, unsigned timeout, int *socket_out);
static int client_wait_connected(int descriptor, unsigned timeout);
static const char *client_kind_words(const char *kind);

/*
 * Starts a connection's state.
 */
void
ssh_client_init(
	struct ssh_client *client)
{
	/* Clears every field. */
	memset(client, 0, sizeof(*client));

	/* No socket and no engine yet. */
	client->socket = -1;
	client->engine = NULL;

	/* Messages go out at the INFO level until the settings say otherwise. */
	client->log_level = SSH_CLIENT_LOG_INFO;
}

/*
 * Wipes and frees everything of the connection.
 */
void
ssh_client_free(
	struct ssh_client *client)
{
	/* Closes the socket. */
	if (client->socket >= 0) {
		close(client->socket);
		client->socket = -1;
	}

	/* Wipes the engine's keys and frees it. */
	ssh_engine_close(client->engine);
	client->engine = NULL;

	/* Wipes the bytes not yet fed. */
	ssh_wipe(client->receive, sizeof(client->receive));
	client->receive_size = 0;
	client->receive_offset = 0;
}

/*
 * Connects to the server.
 */
int
ssh_client_connect(
	struct ssh_client *client)
{
	char port_text[CLIENT_PORT_TEXT];
	struct addrinfo hints;
	struct addrinfo *addresses;
	struct addrinfo *address;
	int descriptor;
	int status;
	int error;
	int on;

	/* Asks for stream addresses of any family for the host and the port. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	sprintf(port_text, "%u", (unsigned)client->port);

	/* Looks the host up. */
	addresses = NULL;
	status = getaddrinfo(client->host_name, port_text, &hints, &addresses);
	if (status != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: Could not resolve hostname %s: %s", client->host_name, gai_strerror(status));
		return ENOENT;
	}

	/* Tries each address in turn until one connects, saying why each one that did not failed. */
	error = ECONNREFUSED;
	descriptor = -1;
	for (address = addresses;
	     address != NULL;
	     address = address->ai_next) {
		ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: connecting to %s port %s", client->host_name, port_text);
		error = client_connect_one(address, client->config.connect_timeout, &descriptor);
		if (error == 0) {
			/* Keeps the address reached in numbers, which the question about a new host shows. */
			status = getnameinfo(address->ai_addr, address->ai_addrlen, client->address, sizeof(client->address), NULL, 0, NI_NUMERICHOST);
			if (status != 0)
				client->address[0] = '\0';
			break;
		}
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: connect to host %s port %s: %s", client->host_name, port_text, strerror(error));
	}

	/* Frees the address list. */
	freeaddrinfo(addresses);

	/* Reports that no address took the connection. */
	if (error != 0)
		return error;

	/* Sends small messages at once: a key press should not wait for more. */
	on = 1;
	(void)setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

	/* Succeeded: the connection is open. */
	client->socket = descriptor;
	return 0;
}

/*
 * Reads from the socket into the receive buffer.
 */
int
ssh_client_read_socket(
	struct ssh_client *client)
{
	ssize_t count;

	/* Reads only when the last bytes are all fed. */
	if (client->receive_size != 0)
		return 0;

	/* Reads what the socket has. */
	count = read(client->socket, client->receive, sizeof(client->receive));

	/* Takes a read a signal cut short, or one that found nothing yet, as nothing read. */
	if (count < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		client->peer_closed = 1;
		return 0;
	}

	/* Notes the end of the connection. */
	if (count == 0) {
		client->peer_closed = 1;
		return 0;
	}

	/* Succeeded: the bytes wait to be fed. */
	client->receive_size = (size_t)count;
	client->receive_offset = 0;
	return 0;
}

/*
 * Writes the engine's pending output to the socket.
 */
int
ssh_client_write_socket(
	struct ssh_client *client)
{
	const uint8_t *pending;
	size_t size;
	ssize_t count;

	/* Finds what waits to be written. */
	pending = ssh_engine_output(client->engine, &size);
	if (size == 0)
		return 0;

	/* Writes as much as the socket takes now. */
	count = send(client->socket, pending, size, MSG_NOSIGNAL);

	/* Takes a write a signal cut short, or one the socket could not take yet, as nothing written. */
	if (count < 0) {
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		client->peer_closed = 1;
		return 0;
	}

	/* Succeeded: removes what was written from the output. */
	ssh_engine_written(client->engine, (size_t)count);
	return 0;
}

/*
 * Gives the engine the bytes read so far, and the end of the connection.
 */
int
ssh_client_feed(
	struct ssh_client *client,
	int *fed)
{
	size_t taken;
	int error;

	/* Starts with nothing given. */
	*fed = 0;

	/* Gives what is left of the bytes read, as far as the engine takes them. */
	if (client->receive_offset < client->receive_size) {
		error = ssh_engine_input(client->engine, client->receive + client->receive_offset, client->receive_size - client->receive_offset, &taken);
		if (error != 0)
			return error;
		client->receive_offset += taken;
		if (taken > 0)
			*fed = 1;
	}

	/* Empties the receive buffer once all of it is given, so that the socket is read again. */
	if (client->receive_offset == client->receive_size) {
		ssh_wipe(client->receive, client->receive_size);
		client->receive_offset = 0;
		client->receive_size = 0;
	}

	/* Tells the engine of the end once every byte before it is given. */
	if (client->peer_closed && client->receive_size == 0 && !client->end_told) {
		error = ssh_engine_input(client->engine, client->receive, 0, &taken);
		if (error != 0)
			return error;
		client->end_told = 1;
		*fed = 1;
	}

	/* Succeeded: the engine has what it could take. */
	return 0;
}

/*
 * Prints why the connection ended.
 */
void
ssh_client_report(
	const struct ssh_client *client,
	int error,
	uint32_t code,
	const char *reason)
{
	const char *kind;
	int compare;

	/* Shows the server's own reason for ending the connection. */
	if (error == ECONNABORTED) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Received disconnect from %s port %u:%lu: %s", client->host_name, (unsigned)client->port, (unsigned long)code, reason);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Disconnected from %s port %u", client->host_name, (unsigned)client->port);
		return;
	}

	/* Names a server that did not identify itself in time. */
	if (error == ETIMEDOUT) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Connection timed out during banner exchange");
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Connection to %s port %u timed out", client->host_name, (unsigned)client->port);
		return;
	}

	/* Names a connection the server closed without a word. */
	if (error == ECONNRESET || error == EPIPE) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Connection closed by %s port %u", client->host_name, (unsigned)client->port);
		return;
	}

	/* Names the error alone when no reason was given. */
	if (reason[0] == '\0') {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: connection to %s port %u failed: %s", client->host_name, (unsigned)client->port, strerror(error));
		return;
	}

	/* A host key that changed in a re-exchange ends the connection. */
	if (error == EACCES) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s; disconnecting", reason);
		return;
	}

	/* Words a failed negotiation as the kind of algorithm that had no match. */
	compare = strncmp(reason, CLIENT_NO_MATCH, sizeof(CLIENT_NO_MATCH) - 1U);
	if (compare == 0) {
		kind = client_kind_words(reason + sizeof(CLIENT_NO_MATCH) - 1U);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Unable to negotiate with %s port %u: no matching %s found", client->host_name, (unsigned)client->port, kind);
		return;
	}

	/* Reports any other reason this side gave, with the host and port. */
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s port %u: %s", client->host_name, (unsigned)client->port, reason);
}

/*
 * Ends the connection.
 */
void
ssh_client_close(
	struct ssh_client *client)
{
	struct pollfd descriptor;
	size_t pending;
	uint64_t deadline;
	uint64_t now;
	int status;

	/* Nothing to end without a socket. */
	if (client->socket < 0)
		return;

	/* Writes what is pending (the DISCONNECT among it), for a bounded time. */
	deadline = ssh_client_now_ms() + SSH_CLIENT_CLOSE_WAIT_MS;
	while (client->engine != NULL && !client->peer_closed) {
		(void)ssh_engine_output(client->engine, &pending);
		if (pending == 0)
			break;

		/* Stops when the time is up. */
		now = ssh_client_now_ms();
		if (now >= deadline)
			break;

		/* Waits for room in the socket, then writes. */
		descriptor.fd = client->socket;
		descriptor.events = POLLOUT;
		descriptor.revents = 0;
		status = poll(&descriptor, 1, (int)(deadline - now));
		if (status < 0 && errno != EINTR)
			break;
		if ((descriptor.revents & (POLLERR | POLLHUP)) != 0)
			break;
		if ((descriptor.revents & POLLOUT) != 0)
			(void)ssh_client_write_socket(client);
	}

	/* Closes the socket. */
	close(client->socket);
	client->socket = -1;
}

/*
 * Connects one address, within timeout seconds when it is not 0.
 */
static int
client_connect_one(
	const struct addrinfo *address,
	unsigned timeout,
	int *socket_out)
{
	int descriptor;
	int flags;
	int status;
	int error;

	/* Makes a socket of the address's family. */
	descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
	if (descriptor < 0)
		return errno;

	/* Keeps the socket from a program the session might start. */
	(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);

	/* Makes the socket not block. */
	flags = fcntl(descriptor, F_GETFL);
	status = -1;
	if (flags >= 0)
		status = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
	if (status < 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* Starts the connection. */
	status = connect(descriptor, address->ai_addr, address->ai_addrlen);
	if (status != 0 && errno != EINPROGRESS) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* Waits for a connection that is still being made. */
	if (status != 0) {
		error = client_wait_connected(descriptor, timeout);
		if (error != 0) {
			close(descriptor);
			return error;
		}
	}

	/* Succeeded: the socket is connected. */
	*socket_out = descriptor;
	return 0;
}

/*
 * Waits for a non-blocking connect to finish and reports its outcome.
 */
static int
client_wait_connected(
	int descriptor,
	unsigned timeout)
{
	struct pollfd poller;
	socklen_t length;
	uint64_t deadline;
	uint64_t now;
	int wait_ms;
	int status;
	int failure;

	/* Waits until the socket is writable, which is when the connection is made or failed. */
	deadline = 0;
	if (timeout != 0)
		deadline = ssh_client_now_ms() + (uint64_t)timeout * 1000U;
	for (;;) {
		wait_ms = -1;
		if (deadline != 0) {
			now = ssh_client_now_ms();
			if (now >= deadline)
				return ETIMEDOUT;
			wait_ms = (int)(deadline - now);
		}

		/* Waits until the descriptor can be written. */
		poller.fd = descriptor;
		poller.events = POLLOUT;
		poller.revents = 0;
		status = poll(&poller, 1, wait_ms);
		if (status < 0 && errno == EINTR)
			continue;
		if (status < 0)
			return errno;
		if (status > 0)
			break;
	}

	/* Reads how the connection ended. */
	failure = 0;
	length = sizeof(failure);
	status = getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &failure, &length);
	if (status != 0)
		return errno;

	/* Reports a connection the server refused or the network could not make. */
	if (failure != 0)
		return failure;

	/* Succeeded: the connection is made. */
	return 0;
}

/*
 * Words the kind a negotiation failed on as the user knows it.
 */
static const char *
client_kind_words(
	const char *kind)
{
	int compare;

	/* The key exchange, also when strict key exchange is what was missing. */
	compare = strncmp(kind, "kex_algorithms", 14);
	if (compare == 0)
		return "key exchange method";

	/* The host key type. */
	compare = strncmp(kind, "server_host_key_algorithms", 26);
	if (compare == 0)
		return "host key type";

	/* A cipher of either direction. */
	compare = strncmp(kind, "encryption_algorithms", 21);
	if (compare == 0)
		return "cipher";

	/* A compression of either direction. */
	compare = strncmp(kind, "compression_algorithms", 22);
	if (compare == 0)
		return "compression method";

	/* Otherwise the transport's own name for it. */
	return kind;
}
