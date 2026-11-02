/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The connection of the ssh program: the TCP connection to the server,
 * the bytes between the socket and the transport, the upper-layer
 * messages the transport could not take yet, and the words for how a
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

/* The longest wait of one poll while waiting for a message, so that re-exchanges start on time. */
#define CLIENT_POLL_MS		1000

/* The longest numeric port text with its NUL. */
#define CLIENT_PORT_TEXT	8U

/* How the transport's reason for a failed negotiation starts. */
#define CLIENT_NO_MATCH		"no matching "

/* How this side's identification line starts in the output (RFC 4253 4.2). */
#define CLIENT_VERSION_PREFIX	"SSH-"

static int client_connect_one(const struct addrinfo *address, unsigned timeout, int *socket_out);
static int client_wait_connected(int descriptor, unsigned timeout);
static int client_backlog_put(struct ssh_client *client, const uint8_t *payload, size_t size);
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

	/* No socket yet. */
	client->socket = -1;

	/* The received messages may be plaintext of the session: secret. */
	ssh_buf_init(&client->payload, SSH_PACKET_LIMIT, 1);

	/* The backlog holds messages that may carry the user's input or a password: secret. */
	ssh_buf_init(&client->backlog, SSH_CLIENT_BACKLOG_MAX, 1);

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

	/* Wipes the transport's keys. */
	if (client->transport_started) {
		ssh_transport_free(&client->transport);
		client->transport_started = 0;
	}

	/* Wipes the buffers and the bytes not yet fed. */
	ssh_buf_free(&client->payload);
	ssh_buf_free(&client->backlog);
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
		if (error == 0)
			break;
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
 * Starts the transport as a client.
 */
int
ssh_client_start(
	struct ssh_client *client)
{
	struct ssh_transport_ops ops;
	uint64_t now;
	int error;

	/* Hands the host key check to the known_hosts logic, with the connection as its context. */
	memset(&ops, 0, sizeof(ops));
	ops.check_host_key = ssh_client_check_host_key;
	ops.context = client;

	/* Starts the transport, which queues the identification line. */
	now = ssh_client_now_ms() / 1000U;
	error = ssh_transport_init(&client->transport, SSH_ROLE_CLIENT, &ops, now);
	if (error != 0)
		return error;

	/* Succeeded: the transport runs and must be freed. */
	client->transport_started = 1;
	return 0;
}

/*
 * Sends one upper-layer message.
 */
int
ssh_client_send(
	struct ssh_client *client,
	const uint8_t *payload,
	size_t size)
{
	int error;

	/* Keeps the order: while older messages wait, this one waits behind them. */
	if (client->backlog.size > 0) {
		error = client_backlog_put(client, payload, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* Gives the message to the transport. */
	error = ssh_transport_send(&client->transport, payload, size);

	/* Keeps a message the full output refused, to send it later. */
	if (error == ENOBUFS) {
		error = client_backlog_put(client, payload, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* Reports an error of the transport. */
	if (error != 0)
		return error;

	/* Succeeded: the transport has the message. */
	return 0;
}

/*
 * Sends the message built in out.
 */
int
ssh_client_send_buf(
	struct ssh_client *client,
	const struct ssh_buf *out)
{
	int error;

	/* Reports a message that could not be built. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Sends what was built. */
	error = ssh_client_send(client, out->data, out->size);
	if (error != 0)
		return error;

	/* Succeeded: the message is sent or kept in order. */
	return 0;
}

/*
 * Moves backlog messages into the transport while it takes them.
 */
int
ssh_client_flush(
	struct ssh_client *client)
{
	uint32_t length;
	size_t taken;
	int error;

	/* Hands the messages over in order until the transport refuses one. */
	taken = 0;
	error = 0;
	while (client->backlog.size - taken >= 4U) {
		length = ssh_load_be32(client->backlog.data + taken);
		error = ssh_transport_send(&client->transport, client->backlog.data + taken + 4U, length);
		if (error != 0)
			break;
		taken += 4U + (size_t)length;
	}

	/* Removes the messages the transport took (wiping them). */
	ssh_buf_consume(&client->backlog, taken);

	/* A refusal for a full output only means waiting longer. */
	if (error == ENOBUFS)
		return 0;

	/* Reports an error of the transport. */
	if (error != 0)
		return error;

	/* Succeeded: the backlog is empty. */
	return 0;
}

/*
 * Feeds received bytes to the transport and takes the next message.
 */
int
ssh_client_receive(
	struct ssh_client *client)
{
	size_t chunk;
	int error;

	/* Alternates feeding and taking until a message is ready or nothing is left. */
	for (;;) {
		/* Feeds the next part of what was read, as far as the transport takes it. */
		if (client->receive_offset < client->receive_size) {
			chunk = client->receive_size - client->receive_offset;
			if (chunk > SSH_CLIENT_FEED_SIZE)
				chunk = SSH_CLIENT_FEED_SIZE;
			error = ssh_transport_input(&client->transport, client->receive + client->receive_offset, chunk);
			if (error == 0) {
				client->receive_offset += chunk;
			} else if (error != EMSGSIZE) {
				return error;
			}
		}

		/* Empties the receive buffer once all of it is fed, so that the socket is read again. */
		if (client->receive_offset == client->receive_size) {
			client->receive_offset = 0;
			client->receive_size = 0;
		}

		/* Takes the next message from what the transport holds. */
		error = ssh_transport_next(&client->transport, &client->payload);

		/* Goes on feeding while bytes are left; reports that nothing is ready otherwise. */
		if (error == EAGAIN) {
			if (client->receive_offset < client->receive_size)
				continue;
			return EAGAIN;
		}

		/* Reports an error of the connection. */
		if (error != 0)
			return error;

		/* Succeeded: client->payload holds one message. */
		return 0;
	}
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
 * Writes the transport's pending output to the socket.
 */
int
ssh_client_write_socket(
	struct ssh_client *client)
{
	const uint8_t *pending;
	size_t size;
	ssize_t count;

	/* Finds what waits to be written. */
	pending = ssh_transport_pending(&client->transport, &size);
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
	ssh_transport_written(&client->transport, (size_t)count);
	return 0;
}

/*
 * Waits until the transport hands up one message.
 */
int
ssh_client_wait(
	struct ssh_client *client,
	uint64_t deadline_ms)
{
	struct pollfd descriptor;
	size_t pending;
	uint64_t now;
	uint64_t left;
	int timeout;
	int status;
	int error;

	/* Turns until a message comes or the connection ends. */
	for (;;) {
		/* Moves waiting messages into the transport. */
		error = ssh_client_flush(client);
		if (error != 0)
			return error;

		/* Takes a message that is already there. */
		error = ssh_client_receive(client);
		if (error == 0)
			return 0;
		if (error != EAGAIN)
			return error;

		/* Reports a connection the server closed. */
		if (client->peer_closed)
			return ECONNRESET;

		/* Gives the transport the time, so that it starts a re-exchange when one is due. */
		now = ssh_client_now_ms();
		error = ssh_transport_tick(&client->transport, now / 1000U);
		if (error != 0)
			return error;

		/* Stops waiting for a server that has not identified itself within ConnectTimeout. */
		timeout = CLIENT_POLL_MS;
		if (deadline_ms != 0 && client->transport.state == SSH_TRANSPORT_VERSION) {
			if (now >= deadline_ms)
				return ETIMEDOUT;
			left = deadline_ms - now;
			if (left < (uint64_t)timeout)
				timeout = (int)left;
		}

		/* Waits for bytes to read, or for room to write the pending output. */
		(void)ssh_transport_pending(&client->transport, &pending);
		descriptor.fd = client->socket;
		descriptor.events = POLLIN;
		if (pending > 0)
			descriptor.events |= POLLOUT;
		descriptor.revents = 0;
		status = poll(&descriptor, 1, timeout);
		if (status < 0 && errno != EINTR)
			return errno;

		/* Writes when the socket has room. */
		if ((descriptor.revents & POLLOUT) != 0)
			(void)ssh_client_write_socket(client);

		/* Reads when the socket has bytes, an end or an error. */
		if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			(void)ssh_client_read_socket(client);
	}
}

/*
 * Prints why the connection ended.
 */
void
ssh_client_report(
	struct ssh_client *client,
	int error)
{
	char reason[SSH_TRANSPORT_REASON_MAX];
	const char *kind;
	int compare;
	int found;

	/* Shows the server's own reason for ending the connection. */
	if (error == ECONNABORTED) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Received disconnect from %s port %u:%lu: %s", client->host_name, (unsigned)client->port, (unsigned long)client->transport.reason_code, client->transport.reason);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Disconnected from %s port %u", client->host_name, (unsigned)client->port);
		return;
	}

	/* The host key check printed its own words. */
	if (error == EACCES)
		return;

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

	/* Shows the reason this side gave the server, when it can still be read. */
	found = ssh_client_local_reason(client, reason, sizeof(reason));
	if (found) {
		/* Words a failed negotiation as the kind of algorithm that had no match. */
		compare = strncmp(reason, CLIENT_NO_MATCH, sizeof(CLIENT_NO_MATCH) - 1U);
		if (compare == 0) {
			kind = client_kind_words(reason + sizeof(CLIENT_NO_MATCH) - 1U);
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Unable to negotiate with %s port %u: no matching %s found", client->host_name, (unsigned)client->port, kind);
			return;
		}
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s port %u: %s", client->host_name, (unsigned)client->port, reason);
		return;
	}

	/* Otherwise names the error. */
	ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: connection to %s port %u failed: %s", client->host_name, (unsigned)client->port, strerror(error));
}

/*
 * Ends the connection.
 */
void
ssh_client_close(
	struct ssh_client *client,
	uint32_t code,
	const char *description)
{
	struct pollfd descriptor;
	size_t pending;
	uint64_t deadline;
	uint64_t now;
	int status;

	/* Nothing to end without a socket. */
	if (client->socket < 0)
		return;

	/* Tells the server why, unless the transport has already ended. */
	if (client->transport_started && client->transport.state != SSH_TRANSPORT_CLOSED)
		ssh_transport_disconnect(&client->transport, code, description);

	/* Writes what is pending, for a bounded time. */
	deadline = ssh_client_now_ms() + SSH_CLIENT_CLOSE_WAIT_MS;
	while (client->transport_started && !client->peer_closed) {
		(void)ssh_transport_pending(&client->transport, &pending);
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
 * Finds the description of the DISCONNECT this side queued.
 *
 * Before the first NEWKEYS the output is the identification line and
 * plain binary packets (RFC 4253 4.2, 6: uint32 packet_length, byte
 * padding_length, the payload, the padding), so the packets can be
 * walked and the last DISCONNECT (RFC 4253 11.1: byte 1, uint32 code,
 * string description, string language) read.
 */
int
ssh_client_local_reason(
	const struct ssh_client *client,
	char *out,
	size_t capacity)
{
	struct ssh_reader reader;
	const uint8_t *pending;
	const uint8_t *description;
	const uint8_t *line_end;
	size_t description_size;
	size_t size;
	size_t offset;
	uint32_t packet_length;
	uint8_t padding_length;
	uint8_t number;
	int compare;
	int found;
	int error;

	/* Refuses an encrypted output, or no room for an answer. */
	if (!client->transport_started)
		return 0;
	if (client->transport.have_session_id)
		return 0;
	if (capacity == 0)
		return 0;

	/* Steps over the identification line when it is still in the output. */
	pending = ssh_transport_pending(&client->transport, &size);
	offset = 0;
	compare = 1;
	if (size >= sizeof(CLIENT_VERSION_PREFIX) - 1U)
		compare = memcmp(pending, CLIENT_VERSION_PREFIX, sizeof(CLIENT_VERSION_PREFIX) - 1U);
	if (compare == 0) {
		line_end = memchr(pending, '\n', size);
		if (line_end == NULL)
			return 0;
		offset = (size_t)(line_end - pending) + 1U;
	}

	/* Walks the packets, keeping the description of the last DISCONNECT. */
	found = 0;
	while (size - offset >= 5U) {
		packet_length = ssh_load_be32(pending + offset);
		if (packet_length > size - offset - 4U)
			break;
		padding_length = pending[offset + 4U];
		if ((size_t)padding_length + 1U > packet_length)
			break;

		/* Reads a DISCONNECT payload: number, code, description. */
		ssh_reader_init(&reader, pending + offset + 5U, packet_length - 1U - padding_length);
		number = ssh_reader_get_u8(&reader);
		(void)ssh_reader_get_u32(&reader);
		ssh_reader_get_string(&reader, &description, &description_size);
		error = ssh_reader_error(&reader);

		/* Keeps the description of a DISCONNECT, cut to the room. */
		if (number == SSH_MSG_DISCONNECT && error == 0) {
			if (description_size >= capacity)
				description_size = capacity - 1U;
			memcpy(out, description, description_size);
			out[description_size] = '\0';
			found = 1;
		}

		/* Goes to the next packet. */
		offset += 4U + (size_t)packet_length;
	}

	/* Succeeded: reports whether a description was found. */
	return found;
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
 * Appends one message to the backlog as uint32 length and bytes.
 */
static int
client_backlog_put(
	struct ssh_client *client,
	const uint8_t *payload,
	size_t size)
{
	int error;

	/* Refuses a message the backlog has no room for (nothing appended). */
	if (size > SSH_CLIENT_BACKLOG_MAX - 4U || client->backlog.size > SSH_CLIENT_BACKLOG_MAX - 4U - size)
		return ENOBUFS;

	/* Appends the length and the bytes. */
	ssh_buf_put_u32(&client->backlog, (uint32_t)size);
	ssh_buf_put_bytes(&client->backlog, payload, size);
	error = ssh_buf_error(&client->backlog);
	if (error != 0)
		return error;

	/* Succeeded: the message waits in order. */
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
