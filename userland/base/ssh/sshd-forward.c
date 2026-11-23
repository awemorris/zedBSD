/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sshd's port forwardings (RFC 4254 7), run by the session process as
 * the user: the remote forwardings it listens for, the channels of
 * forwarded connections both ways, and the policy of sshd_config(5).
 *
 * The rules follow OpenSSH's sshd: the address a
 * remote forwarding listens on comes from GatewayPorts and the address
 * the client asked for; PermitListen matches the asked address as a
 * pattern without regard to case and the port exactly or "*"; PermitOpen
 * matches the destination host as written and the port exactly or "*".
 * Where this sshd differs: it refuses a privileged port to a user other
 * than root itself (RFC 4254 7.1 says a server should), it gives a
 * connection to a destination 30 seconds, it sends no SSH_MSG_DEBUG with
 * a refusal, and it listens and connects only with the address family of
 * its AddressFamily for listening (destinations of direct-tcpip may be of
 * either family).
 */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ssh-hostfile.h"
#include "sshd-forward.h"

/* The global request a client asks a remote forwarding with, and the one it cancels one with (RFC 4254 7.1). */
#define FORWARD_REQUEST		"tcpip-forward"
#define FORWARD_CANCEL		"cancel-tcpip-forward"

/* The channel types of forwarded connections (RFC 4254 7.2). */
#define FORWARD_DIRECT		"direct-tcpip"
#define FORWARD_FORWARDED	"forwarded-tcpip"

/* The loopback and every-address addresses of the two families. */
#define FORWARD_LOOPBACK4	"127.0.0.1"
#define FORWARD_LOOPBACK6	"::1"
#define FORWARD_ANY4		"0.0.0.0"
#define FORWARD_ANY6		"::"

/* The highest port that only root may listen on is one below this. */
#define FORWARD_PRIVILEGED_END	1024U

/* The longest numeric port text with its NUL. */
#define FORWARD_PORT_TEXT	8U

/* The longest numeric address text with its NUL. */
#define FORWARD_ADDRESS_TEXT	NI_MAXHOST

/*
 * One address a remote forwarding listens on, as getaddrinfo gave it.
 */
struct forward_address {
	struct sockaddr_storage address;
	socklen_t length;
	int family;
};

static int forward_take_request(struct sshd_forwards *forwards, struct ssh_reader *reader, int want_reply);
static int forward_take_cancel(struct sshd_forwards *forwards, struct ssh_reader *reader, int want_reply);
static int forward_remote_refusal(const struct sshd_forwards *forwards, const char *address, uint32_t port, int want_reply, const char **why);
static int forward_listen_permitted(const struct sshd_forwards *forwards, const char *address, uint32_t port);
static int forward_open_permitted(const struct sshd_forwards *forwards, const char *host, uint32_t port);
static int forward_bind_addresses(const struct sshd_forwards *forwards, const char *asked, uint16_t port, struct forward_address *addresses, unsigned *count);
static int forward_listen(struct sshd_forwards *forwards, struct sshd_forward_listen *remote, const char *asked, uint16_t port);
static int forward_reply(struct sshd_forwards *forwards, int want_reply, int success, int with_port, uint16_t port);
static int forward_accept(struct sshd_forwards *forwards, struct sshd_forward_listen *remote, unsigned index, uint64_t now_ms);
static unsigned forward_channel_count(const struct sshd_forwards *forwards);
static int forward_connect_next(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static int forward_connected(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static int forward_answer(struct sshd_forwards *forwards, struct sshd_forward_channel *entry, int accept);
static int forward_read(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static int forward_write(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static int forward_settle(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static int forward_abort(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static int forward_close(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static void forward_try_free(struct sshd_forwards *forwards, struct sshd_forward_channel *entry);
static void forward_release(struct sshd_forward_channel *entry);
static void forward_close_listen(struct sshd_forward_listen *remote);
static int forward_send(struct sshd_forwards *forwards);
static int forward_text_ok(const uint8_t *text, size_t size, size_t limit);
static int forward_prepare_socket(int descriptor);
static void forward_name_address(const struct sockaddr *address, socklen_t length, char *host, size_t host_capacity, unsigned *port);

/*
 * Starts the forwardings of a connection.
 */
void
sshd_forward_init(
	struct sshd_forwards *forwards,
	const struct sshd_config *config,
	uid_t uid,
	struct ssh_channels *channels,
	struct ssh_transport *transport)
{
	unsigned index;
	unsigned position;

	/* Takes the connection's settings, user, channels and transport. */
	memset(forwards, 0, sizeof(*forwards));
	forwards->config = config;
	forwards->uid = uid;
	forwards->channels = channels;
	forwards->transport = transport;
	ssh_buf_init(&forwards->message, SSH_PACKET_LIMIT, 0);

	/* Leaves a slot for every session MaxSessions allows, and one more (a slot is never shared). */
	forwards->channel_limit = 0;
	if (config->max_sessions + 1U < SSH_CHANNEL_MAX)
		forwards->channel_limit = SSH_CHANNEL_MAX - 1U - config->max_sessions;

	/* No remote forwarding listens. */
	for (index = 0; index < SSHD_FORWARD_LISTEN_MAX; index++) {
		for (position = 0; position < SSHD_FORWARD_ADDRESSES; position++) {
			forwards->listens[index].sockets[position] = -1;
			forwards->listens[index].slots[position] = -1;
		}
	}

	/* No channel is forwarded; each queue may hold a window and a packet. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		forwards->entries[index].state = SSHD_FORWARD_FREE;
		forwards->entries[index].socket = -1;
		forwards->entries[index].slot = -1;
		ssh_buf_init(&forwards->entries[index].queue, SSHD_FORWARD_QUEUE_MAX, 1);
	}
}

/*
 * Closes every socket of the forwardings and frees what they hold.
 */
void
sshd_forward_free(
	struct sshd_forwards *forwards)
{
	unsigned index;

	/* Stops listening. */
	for (index = 0; index < SSHD_FORWARD_LISTEN_MAX; index++)
		forward_close_listen(&forwards->listens[index]);

	/* Closes every forwarded connection and frees its queue. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		forward_release(&forwards->entries[index]);
		ssh_buf_free(&forwards->entries[index].queue);
	}

	/* Frees the message buffer. */
	ssh_buf_free(&forwards->message);
}

/*
 * Takes a tcpip-forward or cancel-tcpip-forward request.
 */
int
sshd_forward_global(
	struct sshd_forwards *forwards,
	const uint8_t *payload,
	size_t size,
	int *handled)
{
	struct ssh_reader reader;
	const uint8_t *name;
	size_t name_size;
	int want_reply;
	int request;
	int cancel;
	int different;
	int status;

	/* Reads the name and want_reply; a payload too short is left to the caller, which refuses it as malformed. */
	*handled = 0;
	ssh_reader_init(&reader, payload, size);
	(void)ssh_reader_get_u8(&reader);
	ssh_reader_get_string(&reader, &name, &name_size);
	want_reply = ssh_reader_get_bool(&reader);
	status = ssh_reader_error(&reader);
	if (status != 0)
		return 0;

	/* Tells the two names of the forwardings from any other request. */
	request = 0;
	cancel = 0;
	different = 1;
	if (name_size == sizeof(FORWARD_REQUEST) - 1U)
		different = memcmp(name, FORWARD_REQUEST, name_size);
	if (different == 0)
		request = 1;
	different = 1;
	if (name_size == sizeof(FORWARD_CANCEL) - 1U)
		different = memcmp(name, FORWARD_CANCEL, name_size);
	if (different == 0)
		cancel = 1;

	/* Leaves any other request to the caller. */
	if (!request && !cancel)
		return 0;
	*handled = 1;

	/* Answers the request or the cancel. */
	if (request) {
		status = forward_take_request(forwards, &reader, want_reply);
	} else {
		status = forward_take_cancel(forwards, &reader, want_reply);
	}

	/* Reports a malformed request or a message that could not be queued. */
	if (status != 0)
		return status;

	/* Succeeded: the request is answered. */
	return 0;
}

/*
 * Reports whether a CHANNEL_OPEN is of type "direct-tcpip".
 */
int
sshd_forward_is_direct(
	const uint8_t *payload,
	size_t size)
{
	struct ssh_reader reader;
	const uint8_t *type;
	size_t type_size;
	int status;
	int different;

	/* Reads the type after the message number. */
	ssh_reader_init(&reader, payload, size);
	(void)ssh_reader_get_u8(&reader);
	ssh_reader_get_string(&reader, &type, &type_size);
	status = ssh_reader_error(&reader);
	if (status != 0)
		return 0;

	/* Compares it with the forwarded type. */
	if (type_size != sizeof(FORWARD_DIRECT) - 1U)
		return 0;
	different = memcmp(type, FORWARD_DIRECT, type_size);
	if (different != 0)
		return 0;

	/* It is a direct-tcpip open. */
	return 1;
}

/*
 * Takes a direct-tcpip open.
 *
 * Procedure referenced: openssh-portable fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0 serverloop.c
 * (server_request_direct_tcpip).
 */
int
sshd_forward_open(
	struct sshd_forwards *forwards,
	const uint8_t *payload,
	size_t size)
{
	struct sshd_forward_channel *entry;
	struct ssh_channel *channel;
	struct ssh_reader reader;
	struct addrinfo hints;
	struct addrinfo *found;
	const uint8_t *data;
	const uint8_t *host_bytes;
	const uint8_t *origin_bytes;
	size_t data_size;
	size_t host_size;
	size_t origin_size;
	uint32_t port;
	uint32_t origin_port;
	uint32_t reason;
	char host[SSH_CONFIG_HOST_MAX + 1];
	char origin[SSH_CONFIG_HOST_MAX + 1];
	char port_text[FORWARD_PORT_TEXT];
	const char *why;
	size_t index;
	unsigned in_use;
	int permitted;
	int printable;
	int status;

	/* Takes the open into a slot, or refuses it when no slot is free. */
	ssh_buf_reset(&forwards->message);
	status = ssh_channel_take_open(forwards->channels, payload, size, FORWARD_DIRECT, SSHD_FORWARD_WINDOW, &forwards->message, &channel, &data, &data_size);
	if (status != 0)
		return status;
	if (channel == NULL) {
		status = forward_send(forwards);
		return status;
	}

	/* Reads the destination and the originator (RFC 4254 7.2). */
	ssh_reader_init(&reader, data, data_size);
	ssh_reader_get_string(&reader, &host_bytes, &host_size);
	port = ssh_reader_get_u32(&reader);
	ssh_reader_get_string(&reader, &origin_bytes, &origin_size);
	origin_port = ssh_reader_get_u32(&reader);
	status = ssh_reader_finish(&reader);
	if (status != 0)
		return EPROTO;

	/* Keeps the originator for the log, any byte that cannot be shown as '?'. */
	if (origin_size > SSH_CONFIG_HOST_MAX)
		origin_size = SSH_CONFIG_HOST_MAX;
	memcpy(origin, origin_bytes, origin_size);
	origin[origin_size] = '\0';
	for (index = 0; index < origin_size; index++) {
		if ((unsigned char)origin[index] < 0x21U || (unsigned char)origin[index] > 0x7eU)
			origin[index] = '?';
	}

	/* Decides by the policy: a refusal of the administrator's, or a destination that cannot be one. */
	why = NULL;
	reason = SSH_OPEN_ADMINISTRATIVELY_PROHIBITED;
	printable = forward_text_ok(host_bytes, host_size, SSH_CONFIG_HOST_MAX);
	in_use = forward_channel_count(forwards);
	if (forwards->config->disable_forwarding) {
		why = "DisableForwarding";
	} else if ((forwards->config->tcp_forwarding & SSHD_TCP_FORWARD_LOCAL) == 0) {
		why = "AllowTcpForwarding";
	} else if (!printable) {
		why = "a bad host";
	} else if (port == 0 || port > 65535U) {
		why = "a bad port";
	} else if (in_use >= forwards->channel_limit) {
		why = "too many forwarded channels";
		reason = SSH_OPEN_RESOURCE_SHORTAGE;
	}

	/* Copies the host, which is printable and short enough when nothing refused it so far. */
	host[0] = '\0';
	if (why == NULL) {
		memcpy(host, host_bytes, host_size);
		host[host_size] = '\0';
		permitted = forward_open_permitted(forwards, host, port);
		if (!permitted)
			why = "PermitOpen";
	}

	/* Refuses the open with the reason. */
	if (why != NULL) {
		syslog(LOG_INFO, "session: refused direct-tcpip from %.64s port %u: %s", origin, (unsigned)origin_port, why);
		ssh_buf_reset(&forwards->message);
		status = ssh_channel_refuse_open(forwards->channels, channel, reason, &forwards->message);
		if (status != 0)
			return status;
		status = forward_send(forwards);
		return status;
	}

	/* Starts the entry of the channel's slot. */
	entry = &forwards->entries[channel->local_id];
	forward_release(entry);
	entry->state = SSHD_FORWARD_CONNECTING;
	entry->channel = channel;
	entry->deadline = sshd_forward_now_ms() + SSHD_FORWARD_CONNECT_MS;
	syslog(LOG_DEBUG, "session: direct-tcpip to %.255s port %u from %.64s port %u", host, (unsigned)port, origin, (unsigned)origin_port);

	/* Looks the destination up (this waits, as OpenSSH's sshd does). */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	sprintf(port_text, "%u", (unsigned)port);
	found = NULL;
	status = getaddrinfo(host, port_text, &hints, &found);
	if (status != 0) {
		status = forward_answer(forwards, entry, 0);
		return status;
	}

	/* Connects to the first address that takes the connection. */
	entry->addresses = found;
	entry->next_address = found;
	status = forward_connect_next(forwards, entry);
	if (status != 0)
		return status;

	/* Succeeded: the open is answered or waits for its connection. */
	return 0;
}

/*
 * Takes the client's answer to a forwarded-tcpip open.
 */
int
sshd_forward_open_reply(
	struct sshd_forwards *forwards,
	const uint8_t *payload,
	size_t size)
{
	struct sshd_forward_channel *entry;
	struct ssh_channel *channel;
	uint32_t reason;
	int status;

	/* Reads the answer; only this side's forwarded channels are ever opening. */
	channel = NULL;
	reason = 0;
	status = ssh_channel_open_reply(forwards->channels, payload, size, &channel, &reason);
	if (status != 0 && status != EACCES)
		return status;
	if (channel == NULL || channel->local_id >= SSH_CHANNEL_MAX)
		return EPROTO;
	entry = &forwards->entries[channel->local_id];
	if (entry->state != SSHD_FORWARD_OPENING)
		return EPROTO;

	/* A refusal closes the connection; the table freed the slot. */
	if (status == EACCES) {
		forward_release(entry);
		return 0;
	}

	/* An open channel carries the connection. */
	entry->state = SSHD_FORWARD_OPEN;
	return 0;
}

/*
 * Reports whether a channel is forwarded.
 */
int
sshd_forward_owns(
	const struct sshd_forwards *forwards,
	const struct ssh_channel *channel)
{
	const struct sshd_forward_channel *entry;

	/* The entry of the channel's slot holds it. */
	if (channel == NULL || channel->local_id >= SSH_CHANNEL_MAX)
		return 0;
	entry = &forwards->entries[channel->local_id];
	if (entry->state == SSHD_FORWARD_FREE)
		return 0;
	if (entry->channel != channel)
		return 0;

	/* The channel is forwarded. */
	return 1;
}

/*
 * Takes the client's data for a forwarded channel.
 */
int
sshd_forward_data(
	struct sshd_forwards *forwards,
	struct ssh_channel *channel,
	const uint8_t *data,
	size_t size,
	uint32_t extended)
{
	struct sshd_forward_channel *entry;
	int status;

	/* Takes the entry of the channel. */
	entry = &forwards->entries[channel->local_id];

	/* Data for an open connection waits for its socket. */
	if (entry->state == SSHD_FORWARD_OPEN &&
	    entry->socket >= 0 &&
	    extended == 0 &&
	    !entry->client_ended) {
		ssh_buf_put_bytes(&entry->queue, data, size);
		status = ssh_buf_error(&entry->queue);
		if (status != 0)
			return EPROTO;
		return 0;
	}

	/* Anything else (extended data, data for a closing connection) is dropped and its window given back. */
	ssh_buf_reset(&forwards->message);
	ssh_channel_consumed(channel, size, &forwards->message);
	status = forward_send(forwards);
	if (status != 0)
		return status;

	/* Succeeded: the data is taken. */
	return 0;
}

/*
 * Takes the client's EOF or CLOSE of a forwarded channel.
 */
int
sshd_forward_end(
	struct sshd_forwards *forwards,
	struct ssh_channel *channel,
	int close)
{
	struct sshd_forward_channel *entry;
	int status;

	/* Takes the entry of the channel. */
	entry = &forwards->entries[channel->local_id];

	/* The client sends no more: its last byte is written before the socket's sending side is shut. */
	entry->client_ended = 1;
	if (close)
		entry->client_closed = 1;

	/* Shuts, closes or frees what is due now. */
	status = forward_settle(forwards, entry);
	if (status != 0)
		return status;

	/* Succeeded: the end is taken. */
	return 0;
}

/*
 * Sends what is due on the forwarded channels.
 */
int
sshd_forward_pump(
	struct sshd_forwards *forwards,
	uint64_t now_ms)
{
	struct sshd_forward_channel *entry;
	unsigned index;
	int status;

	/* Moves each forwarded channel on. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		entry = &forwards->entries[index];

		/* Gives up a connection to a destination that did not answer in time. */
		if (entry->state == SSHD_FORWARD_CONNECTING) {
			if (now_ms < entry->deadline)
				continue;
			status = forward_answer(forwards, entry, 0);
			if (status != 0)
				return status;
			continue;
		}

		/* Sends the EOF, shuts the socket, and closes the channel when they are due. */
		if (entry->state == SSHD_FORWARD_OPEN || entry->state == SSHD_FORWARD_CLOSING) {
			status = forward_settle(forwards, entry);
			if (status != 0)
				return status;
		}
	}

	/* Succeeded: what was due is queued. */
	return 0;
}

/*
 * Puts the sockets the forwardings wait on into the poll slots.
 */
unsigned
sshd_forward_poll(
	struct sshd_forwards *forwards,
	struct pollfd *slots,
	unsigned capacity,
	size_t pending,
	uint64_t now_ms,
	int *timeout_ms)
{
	struct sshd_forward_listen *remote;
	struct sshd_forward_channel *entry;
	uint64_t wait;
	unsigned count;
	unsigned index;
	unsigned position;
	size_t room;
	short events;

	/* Watches every listening socket for a connection, unless its forwarding rests. */
	count = 0;
	for (index = 0; index < SSHD_FORWARD_LISTEN_MAX; index++) {
		remote = &forwards->listens[index];
		for (position = 0; position < SSHD_FORWARD_ADDRESSES; position++)
			remote->slots[position] = -1;
		if (!remote->in_use)
			continue;

		/* Wakes the loop when a resting forwarding may accept again. */
		if (remote->paused_until > now_ms) {
			wait = remote->paused_until - now_ms;
			if (wait < (uint64_t)*timeout_ms)
				*timeout_ms = (int)wait;
			continue;
		}

		/* The resting time is over, so the forwarding accepts again. */
		remote->paused_until = 0;

		/* Takes a slot for each of its sockets. */
		for (position = 0; position < remote->socket_count; position++) {
			if (count >= capacity)
				break;
			slots[count].fd = remote->sockets[position];
			slots[count].events = POLLIN;
			slots[count].revents = 0;
			remote->slots[position] = (int)count;
			count++;
		}
	}

	/* Watches each forwarded channel's socket for what it waits for. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		entry = &forwards->entries[index];
		entry->slot = -1;
		if (entry->socket < 0)
			continue;

		/* Chooses the events: a connection being made, bytes to read while they may be sent, room to write. */
		events = 0;
		if (entry->state == SSHD_FORWARD_CONNECTING) {
			events |= POLLOUT;
			wait = 0;
			if (entry->deadline > now_ms)
				wait = entry->deadline - now_ms;
			if (wait < (uint64_t)*timeout_ms)
				*timeout_ms = (int)wait;
		} else if (entry->state == SSHD_FORWARD_OPEN) {
			room = ssh_channel_send_room(entry->channel);
			if (!entry->socket_ended &&
			    room > 0 &&
			    pending < SSHD_FORWARD_OUTPUT_SOFT)
				events |= POLLIN;
			if (entry->queue.size > entry->queue_offset)
				events |= POLLOUT;
		}

		/* A socket that waits for nothing here (the client's answer, or room in the window) takes no slot. */
		if (events == 0 || count >= capacity)
			continue;

		/* Takes a slot for the socket. */
		slots[count].fd = entry->socket;
		slots[count].events = events;
		slots[count].revents = 0;
		entry->slot = (int)count;
		count++;
	}

	/* Reports how many slots were filled. */
	return count;
}

/*
 * Acts on what poll reported for the forwardings' slots.
 */
int
sshd_forward_serve(
	struct sshd_forwards *forwards,
	const struct pollfd *slots,
	uint64_t now_ms)
{
	struct sshd_forward_listen *remote;
	struct sshd_forward_channel *entry;
	unsigned index;
	unsigned position;
	short revents;
	int status;

	/* Accepts a connection on each listening socket that has one. */
	for (index = 0; index < SSHD_FORWARD_LISTEN_MAX; index++) {
		remote = &forwards->listens[index];
		for (position = 0; position < SSHD_FORWARD_ADDRESSES; position++) {
			if (!remote->in_use || remote->slots[position] < 0)
				continue;
			revents = slots[remote->slots[position]].revents;
			if ((revents & (POLLIN | POLLERR | POLLHUP)) == 0)
				continue;

			/* Opens a forwarded-tcpip channel for the connection. */
			status = forward_accept(forwards, remote, position, now_ms);
			if (status != 0)
				return status;
		}
	}

	/* Moves each watched forwarded channel on. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		entry = &forwards->entries[index];
		if (entry->slot < 0)
			continue;
		revents = slots[entry->slot].revents;
		if (revents == 0)
			continue;

		/* A connection being made has its outcome when the socket is writable. */
		if (entry->state == SSHD_FORWARD_CONNECTING) {
			status = forward_connected(forwards, entry);
			if (status != 0)
				return status;
			continue;
		}

		/* Writes the client's bytes when the socket takes them, or failed. */
		if ((slots[entry->slot].events & POLLOUT) != 0 &&
		    (revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
			status = forward_write(forwards, entry);
			if (status != 0)
				return status;
		}

		/* Reads the socket when it has bytes, ended or failed, unless the write ended the connection. */
		if (entry->state == SSHD_FORWARD_OPEN &&
		    entry->socket >= 0 &&
		    (slots[entry->slot].events & POLLIN) != 0 &&
		    (revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
			status = forward_read(forwards, entry);
			if (status != 0)
				return status;
		}
	}

	/* Succeeded: what was ready is handled. */
	return 0;
}

/*
 * Returns the monotonic clock in milliseconds.
 */
uint64_t
sshd_forward_now_ms(void)
{
	struct timespec now;

	/* Reads the clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/*
 * Takes a tcpip-forward request: string address, uint32 port.  Listens
 * when the policy allows and answers.  Returns 0, EINVAL (malformed) or
 * the error of queueing the answer.
 *
 * Procedure referenced: openssh-portable fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0 serverloop.c
 * (server_input_global_request).
 */
static int
forward_take_request(
	struct sshd_forwards *forwards,
	struct ssh_reader *reader,
	int want_reply)
{
	struct sshd_forward_listen *remote;
	const uint8_t *address_bytes;
	size_t address_size;
	uint32_t port;
	char address[SSH_CONFIG_HOST_MAX + 1];
	const char *why;
	unsigned index;
	int printable;
	int status;

	/* Reads the address and the port. */
	ssh_reader_get_string(reader, &address_bytes, &address_size);
	port = ssh_reader_get_u32(reader);
	status = ssh_reader_finish(reader);
	if (status != 0)
		return EINVAL;

	/* An address that cannot be one is refused like any other refusal. */
	printable = forward_text_ok(address_bytes, address_size, SSH_CONFIG_HOST_MAX);
	if (address_size == 0)
		printable = 1;
	if (!printable) {
		syslog(LOG_INFO, "session: refused tcpip-forward: a bad address");
		status = forward_reply(forwards, want_reply, 0, 0, 0);
		return status;
	}

	/* Copies the address as a string. */
	memcpy(address, address_bytes, address_size);
	address[address_size] = '\0';

	/* Applies the policy. */
	why = NULL;
	status = forward_remote_refusal(forwards, address, port, want_reply, &why);
	if (status) {
		syslog(LOG_INFO, "session: refused tcpip-forward for %.255s port %u: %s", address, (unsigned)port, why);
		status = forward_reply(forwards, want_reply, 0, 0, 0);
		return status;
	}

	/* Takes a free entry for the forwarding. */
	remote = NULL;
	for (index = 0; index < SSHD_FORWARD_LISTEN_MAX; index++) {
		if (!forwards->listens[index].in_use) {
			remote = &forwards->listens[index];
			break;
		}
	}

	/* Refuses one more than the table keeps. */
	if (remote == NULL) {
		syslog(LOG_INFO, "session: refused tcpip-forward for %.255s port %u: too many forwardings", address, (unsigned)port);
		status = forward_reply(forwards, want_reply, 0, 0, 0);
		return status;
	}

	/* Listens; a failure (the port in use, an address of another family) is a refusal. */
	status = forward_listen(forwards, remote, address, (uint16_t)port);
	if (status != 0) {
		syslog(LOG_INFO, "session: refused tcpip-forward for %.255s port %u: %s", address, (unsigned)port, strerror(status));
		status = forward_reply(forwards, want_reply, 0, 0, 0);
		return status;
	}

	/* Answers with the port chosen when port 0 was asked (RFC 4254 7.1). */
	status = forward_reply(forwards, want_reply, 1, port == 0, remote->port);
	if (status != 0)
		return status;

	/* Succeeded: the forwarding listens. */
	return 0;
}

/*
 * Takes a cancel-tcpip-forward request: closes the forwarding asked for
 * with the same address and listening on the same port; the channels it
 * opened stay.  Returns 0, EINVAL (malformed) or the error of queueing the
 * answer.
 */
static int
forward_take_cancel(
	struct sshd_forwards *forwards,
	struct ssh_reader *reader,
	int want_reply)
{
	struct sshd_forward_listen *remote;
	const uint8_t *address_bytes;
	size_t address_size;
	uint32_t port;
	size_t length;
	unsigned index;
	int found;
	int different;
	int status;

	/* Reads the address and the port. */
	ssh_reader_get_string(reader, &address_bytes, &address_size);
	port = ssh_reader_get_u32(reader);
	status = ssh_reader_finish(reader);
	if (status != 0)
		return EINVAL;

	/* Finds the forwarding and stops it. */
	found = 0;
	for (index = 0; index < SSHD_FORWARD_LISTEN_MAX; index++) {
		remote = &forwards->listens[index];
		if (!remote->in_use || remote->port != port)
			continue;
		length = strlen(remote->address);
		if (length != address_size)
			continue;
		different = memcmp(remote->address, address_bytes, address_size);
		if (different != 0)
			continue;
		forward_close_listen(remote);
		found = 1;
		break;
	}

	/* Answers whether there was one. */
	status = forward_reply(forwards, want_reply, found, 0, 0);
	if (status != 0)
		return status;

	/* Succeeded: the cancel is answered. */
	return 0;
}

/*
 * Decides whether the policy refuses a remote forwarding (1, *why set)
 * or allows it (0).
 */
static int
forward_remote_refusal(
	const struct sshd_forwards *forwards,
	const char *address,
	uint32_t port,
	int want_reply,
	const char **why)
{
	int permitted;

	/* The administrator turned every forwarding off. */
	if (forwards->config->disable_forwarding) {
		*why = "DisableForwarding";
		return 1;
	}

	/* AllowTcpForwarding does not allow remote forwardings. */
	if ((forwards->config->tcp_forwarding & SSHD_TCP_FORWARD_REMOTE) == 0) {
		*why = "AllowTcpForwarding";
		return 1;
	}

	/* A chosen port could not be told without a reply. */
	if (port == 0 && !want_reply) {
		*why = "port 0 without a reply";
		return 1;
	}

	/* A port is 16 bits. */
	if (port > 65535U) {
		*why = "a bad port";
		return 1;
	}

	/* A privileged port is root's (RFC 4254 7.1), whatever the kernel would allow. */
	if (port != 0 && port < FORWARD_PRIVILEGED_END && forwards->uid != 0) {
		*why = "a privileged port";
		return 1;
	}

	/* PermitListen must allow the address and the port. */
	permitted = forward_listen_permitted(forwards, address, port);
	if (!permitted) {
		*why = "PermitListen";
		return 1;
	}

	/* The policy allows it. */
	return 0;
}

/*
 * Reports whether PermitListen allows a remote forwarding: any, none, or
 * an entry whose host pattern matches the address (without regard to
 * case) and whose port is the port or "*".
 *
 * Procedure referenced: openssh-portable fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0 channels.c
 * (remote_open_match).
 */
static int
forward_listen_permitted(
	const struct sshd_forwards *forwards,
	const char *address,
	uint32_t port)
{
	const struct sshd_permit *entry;
	unsigned index;
	int matched;

	/* any and none decide alone. */
	if (forwards->config->permit_listen_mode == SSHD_PERMIT_ANY)
		return 1;
	if (forwards->config->permit_listen_mode == SSHD_PERMIT_NONE)
		return 0;

	/* Looks for an entry that matches. */
	for (index = 0; index < forwards->config->permit_listen_count; index++) {
		entry = &forwards->config->permit_listen[index];
		if (entry->port != 0 && entry->port != port)
			continue;
		matched = ssh_pattern_match(entry->host, strlen(entry->host), address, 1);
		if (matched)
			return 1;
	}

	/* No entry allows it. */
	return 0;
}

/*
 * Reports whether PermitOpen allows a destination: any, none, or an
 * entry whose host is "*" or the host as written, and whose port is the
 * port or "*".
 *
 * Procedure referenced: openssh-portable fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0 channels.c
 * (open_match).
 */
static int
forward_open_permitted(
	const struct sshd_forwards *forwards,
	const char *host,
	uint32_t port)
{
	const struct sshd_permit *entry;
	unsigned index;
	int different;

	/* any and none decide alone. */
	if (forwards->config->permit_open_mode == SSHD_PERMIT_ANY)
		return 1;
	if (forwards->config->permit_open_mode == SSHD_PERMIT_NONE)
		return 0;

	/* Looks for an entry that matches. */
	for (index = 0; index < forwards->config->permit_open_count; index++) {
		entry = &forwards->config->permit_open[index];
		if (entry->port != 0 && entry->port != port)
			continue;
		different = strcmp(entry->host, "*");
		if (different == 0)
			return 1;
		different = strcmp(entry->host, host);
		if (different == 0)
			return 1;
	}

	/* No entry allows it. */
	return 0;
}

/*
 * Finds the addresses a remote forwarding listens on (OpenSSH's
 * channel_fwd_bind_addr for the server), in the family of AddressFamily:
 * with GatewayPorts no, the loopback address unless "127.0.0.1" or "::1"
 * was asked; with yes, every address; with clientspecified, every address
 * for "" and "*", the loopback for "localhost", and the address asked
 * otherwise (a name is looked up).  Returns 0 with *count of them, or the
 * error of the lookup.
 *
 * Procedure referenced: openssh-portable fa4b2a82be7df9768272cfabc2d78e4a12c2b3c0 channels.c
 * (channel_fwd_bind_addr).
 */
static int
forward_bind_addresses(
	const struct sshd_forwards *forwards,
	const char *asked,
	uint16_t port,
	struct forward_address *addresses,
	unsigned *count)
{
	struct addrinfo hints;
	struct addrinfo *found;
	struct addrinfo *entry;
	const char *host;
	char port_text[FORWARD_PORT_TEXT];
	int family;
	int flags;
	int loopback_asked;
	int every_asked;
	int localhost_asked;
	int different;
	int status;

	/* The family of AddressFamily, and the port in digits. */
	family = AF_INET;
	if (forwards->config->inet6)
		family = AF_INET6;
	sprintf(port_text, "%u", (unsigned)port);
	*count = 0;

	/* Notes what was asked: a loopback address, which every setting but yes keeps, every address, or localhost. */
	loopback_asked = 0;
	different = strcmp(asked, FORWARD_LOOPBACK4);
	if (different == 0)
		loopback_asked = 1;
	different = strcmp(asked, FORWARD_LOOPBACK6);
	if (different == 0)
		loopback_asked = 1;
	every_asked = 0;
	different = strcmp(asked, "*");
	if (asked[0] == '\0' || different == 0)
		every_asked = 1;
	localhost_asked = 0;
	different = strcmp(asked, "localhost");
	if (different == 0)
		localhost_asked = 1;

	/* Decides between every address, the loopback, and the address asked. */
	host = NULL;
	flags = AI_NUMERICHOST;
	if (forwards->config->gateway_ports == SSHD_GATEWAY_YES) {
		/* yes: every address, whatever was asked. */
		flags |= AI_PASSIVE;
	} else if (forwards->config->gateway_ports == SSHD_GATEWAY_NO) {
		/* no: the loopback, or the loopback address asked. */
		if (loopback_asked)
			host = asked;
	} else if (every_asked) {
		/* clientspecified, "" or "*": every address. */
		flags |= AI_PASSIVE;
	} else if (!localhost_asked) {
		/* clientspecified, an address or a name: that one, looked up. */
		host = asked;
		flags = 0;
	}

	/* Names the loopback or every address of the family when no address is taken from the request. */
	if (host == NULL && (flags & AI_PASSIVE) != 0) {
		host = FORWARD_ANY4;
		if (family == AF_INET6)
			host = FORWARD_ANY6;
	} else if (host == NULL) {
		host = FORWARD_LOOPBACK4;
		if (family == AF_INET6)
			host = FORWARD_LOOPBACK6;
	}

	/* Looks the address up in the family. */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = flags;
	found = NULL;
	status = getaddrinfo(host, port_text, &hints, &found);
	if (status != 0)
		return EADDRNOTAVAIL;

	/* Keeps each address of the family while there is room. */
	for (entry = found;
	     entry != NULL;
	     entry = entry->ai_next) {
		if (*count >= SSHD_FORWARD_ADDRESSES)
			break;
		if (entry->ai_family != family)
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

	/* Refuses a forwarding with nowhere to listen. */
	if (*count == 0)
		return EADDRNOTAVAIL;

	/* Succeeded: *count addresses to listen on. */
	return 0;
}

/*
 * Listens for a remote forwarding on every address it names, all on one
 * port (the one asked, or the one the first socket got for 0).  Returns 0
 * when at least one socket listens, or the error of the last attempt.
 */
static int
forward_listen(
	struct sshd_forwards *forwards,
	struct sshd_forward_listen *remote,
	const char *asked,
	uint16_t port)
{
	struct forward_address addresses[SSHD_FORWARD_ADDRESSES];
	struct sockaddr_storage bound;
	struct sockaddr_in *inet4;
	struct sockaddr_in6 *inet6;
	socklen_t length;
	uint16_t chosen;
	unsigned count;
	unsigned position;
	int descriptor;
	int status;
	int error;
	int on;

	/* Finds the addresses. */
	status = forward_bind_addresses(forwards, asked, port, addresses, &count);
	if (status != 0)
		return status;

	/* Listens on each, on the port asked or on the one the first socket was given. */
	chosen = port;
	error = EADDRNOTAVAIL;
	remote->socket_count = 0;
	for (position = 0; position < count; position++) {
		/* Puts the port in the address. */
		if (addresses[position].family == AF_INET) {
			inet4 = (struct sockaddr_in *)&addresses[position].address;
			inet4->sin_port = htons(chosen);
		} else {
			inet6 = (struct sockaddr_in6 *)&addresses[position].address;
			inet6->sin6_port = htons(chosen);
		}

		/* Makes the socket: it may take a port a closed listener left, and IPv6 stays IPv6. */
		descriptor = socket(addresses[position].family, SOCK_STREAM, 0);
		if (descriptor < 0) {
			error = errno;
			continue;
		}

		/* Sends each small packet at once, as an interactive stream needs. */
		on = 1;
		(void)setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		if (addresses[position].family == AF_INET6)
			(void)setsockopt(descriptor, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));

		/* Binds the address. */
		status = bind(descriptor, (const struct sockaddr *)&addresses[position].address, addresses[position].length);
		if (status != 0) {
			error = errno;
			close(descriptor);
			continue;
		}

		/* Listens. */
		status = listen(descriptor, SSHD_FORWARD_BACKLOG);
		if (status != 0) {
			error = errno;
			close(descriptor);
			continue;
		}

		/* Keeps the socket from the children and from blocking the loop. */
		error = forward_prepare_socket(descriptor);
		if (error != 0) {
			close(descriptor);
			continue;
		}

		/* Takes the port the first socket got for port 0, so every socket listens on it. */
		if (chosen == 0) {
			length = sizeof(bound);
			status = getsockname(descriptor, (struct sockaddr *)&bound, &length);
			if (status != 0) {
				error = errno;
				close(descriptor);
				continue;
			}

			/* Reads the port from the address family the socket has. */
			if (bound.ss_family == AF_INET) {
				chosen = ntohs(((struct sockaddr_in *)&bound)->sin_port);
			} else {
				chosen = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
			}
		}

		/* Keeps the socket. */
		remote->sockets[remote->socket_count] = descriptor;
		remote->slots[remote->socket_count] = -1;
		remote->socket_count++;
	}

	/* Reports a forwarding that listens nowhere. */
	if (remote->socket_count == 0)
		return error;

	/* Keeps the forwarding by the address asked and the port it listens on. */
	remote->in_use = 1;
	strcpy(remote->address, asked);
	remote->port = chosen;
	remote->paused_until = 0;

	/* Succeeded: the forwarding listens. */
	return 0;
}

/*
 * Answers a global request when a reply is wanted: REQUEST_SUCCESS (with
 * the port when with_port) or REQUEST_FAILURE.  Returns 0 or the error of
 * queueing it.
 */
static int
forward_reply(
	struct sshd_forwards *forwards,
	int want_reply,
	int success,
	int with_port,
	uint16_t port)
{
	int status;

	/* No reply is sent unless one is wanted. */
	if (!want_reply)
		return 0;

	/* Builds the answer. */
	ssh_buf_reset(&forwards->message);
	if (success) {
		ssh_buf_put_u8(&forwards->message, SSH_MSG_REQUEST_SUCCESS);
		if (with_port)
			ssh_buf_put_u32(&forwards->message, port);
	} else {
		ssh_buf_put_u8(&forwards->message, SSH_MSG_REQUEST_FAILURE);
	}

	/* Sends it. */
	status = forward_send(forwards);
	if (status != 0)
		return status;

	/* Succeeded: the answer is queued. */
	return 0;
}

/*
 * Accepts a connection on one socket of a remote forwarding and opens a
 * forwarded-tcpip channel for it (RFC 4254 7.2: the address asked and
 * the port listened on, the originator's address and port).  A connection
 * beyond the forwarded channels' limit or the table is closed.  Returns 0
 * or the error of queueing the open.
 */
static int
forward_accept(
	struct sshd_forwards *forwards,
	struct sshd_forward_listen *remote,
	unsigned index,
	uint64_t now_ms)
{
	struct sockaddr_storage peer;
	struct sshd_forward_channel *entry;
	struct ssh_channel *channel;
	struct ssh_buf data;
	char origin[FORWARD_ADDRESS_TEXT];
	unsigned origin_port;
	unsigned in_use;
	socklen_t length;
	int descriptor;
	int status;
	int error;
	int on;

	/* Accepts the connection. */
	length = sizeof(peer);
	descriptor = accept(remote->sockets[index], (struct sockaddr *)&peer, &length);
	if (descriptor < 0) {
		error = errno;

		/* Rests the forwarding for a while when descriptors ran out, leaving the connection in the backlog. */
		if (error == EMFILE || error == ENFILE)
			remote->paused_until = now_ms + SSHD_FORWARD_PAUSE_MS;

		/* Nothing more to do: a connection that went away, an interrupt, or a rest. */
		return 0;
	}

	/* Keeps the socket from the children and from blocking the loop, and sends small messages at once. */
	error = forward_prepare_socket(descriptor);
	if (error != 0) {
		close(descriptor);
		return 0;
	}

	/* Sends each small packet at once, as an interactive stream needs. */
	on = 1;
	(void)setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

	/* Closes a connection beyond the forwarded channels' limit. */
	in_use = forward_channel_count(forwards);
	if (in_use >= forwards->channel_limit) {
		syslog(LOG_INFO, "session: closed a connection to forwarded port %u: too many forwarded channels", (unsigned)remote->port);
		close(descriptor);
		return 0;
	}

	/* Builds the open's own data. */
	forward_name_address((const struct sockaddr *)&peer, length, origin, sizeof(origin), &origin_port);
	ssh_buf_init(&data, SSH_CONFIG_HOST_MAX * 2U + 64U, 0);
	ssh_buf_put_cstring(&data, remote->address);
	ssh_buf_put_u32(&data, remote->port);
	ssh_buf_put_cstring(&data, origin);
	ssh_buf_put_u32(&data, origin_port);
	status = ssh_buf_error(&data);
	if (status != 0) {
		ssh_buf_free(&data);
		close(descriptor);
		return 0;
	}

	/* Opens the channel; a full table closes the connection. */
	ssh_buf_reset(&forwards->message);
	status = ssh_channel_open(forwards->channels, FORWARD_FORWARDED, SSHD_FORWARD_WINDOW, data.data, data.size, &forwards->message, &channel);
	ssh_buf_free(&data);
	if (status != 0) {
		close(descriptor);
		return 0;
	}

	/* Keeps the connection until the client answers. */
	entry = &forwards->entries[channel->local_id];
	forward_release(entry);
	entry->state = SSHD_FORWARD_OPENING;
	entry->channel = channel;
	entry->socket = descriptor;

	/* Sends the open. */
	status = forward_send(forwards);
	if (status != 0)
		return status;

	/* Succeeded: the open is queued. */
	return 0;
}

/*
 * Counts the forwarded channels in use.
 */
static unsigned
forward_channel_count(
	const struct sshd_forwards *forwards)
{
	unsigned index;
	unsigned count;

	/* Counts every entry that is not free. */
	count = 0;
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		if (forwards->entries[index].state != SSHD_FORWARD_FREE)
			count++;
	}

	/* Reports the count. */
	return count;
}

/*
 * Starts the connection to the next address of a direct-tcpip
 * destination, answering the client when one connects at once or none
 * is left.  Returns 0 or the error of queueing the answer.
 */
static int
forward_connect_next(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	struct addrinfo *address;
	int descriptor;
	int status;
	int error;

	/* Tries each address left until one connects or is on its way. */
	while (entry->next_address != NULL) {
		address = entry->next_address;
		entry->next_address = address->ai_next;

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
			entry->socket = descriptor;
			status = forward_answer(forwards, entry, 1);
			return status;
		}

		/* Waits for a connection on its way. */
		if (errno == EINPROGRESS) {
			entry->socket = descriptor;
			return 0;
		}

		/* Goes on with the next address. */
		close(descriptor);
	}

	/* No address took the connection: the client hears it failed. */
	status = forward_answer(forwards, entry, 0);
	if (status != 0)
		return status;

	/* Succeeded: the failure is answered. */
	return 0;
}

/*
 * Takes the outcome of a connection on its way to a direct-tcpip
 * destination.  Returns 0 or the error of queueing the answer.
 */
static int
forward_connected(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	socklen_t length;
	int failure;
	int status;

	/* Reads how the connection ended. */
	failure = 0;
	length = sizeof(failure);
	status = getsockopt(entry->socket, SOL_SOCKET, SO_ERROR, &failure, &length);
	if (status != 0)
		failure = errno;

	/* A made connection opens the channel. */
	if (failure == 0) {
		status = forward_answer(forwards, entry, 1);
		return status;
	}

	/* A failed one tries the next address. */
	close(entry->socket);
	entry->socket = -1;
	status = forward_connect_next(forwards, entry);
	if (status != 0)
		return status;

	/* Succeeded: the next address is tried. */
	return 0;
}

/*
 * Answers a direct-tcpip open: accept 1 opens the channel (OPEN_
 * CONFIRMATION), 0 refuses it with CONNECT_FAILED and frees the entry.
 * Returns 0 or the error of queueing the answer.
 */
static int
forward_answer(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry,
	int accept)
{
	struct ssh_channel *channel;
	int status;

	/* The destination's addresses are no longer needed. */
	if (entry->addresses != NULL)
		freeaddrinfo(entry->addresses);
	entry->addresses = NULL;
	entry->next_address = NULL;

	/* Builds the answer; a refusal frees the slot in the table. */
	channel = entry->channel;
	ssh_buf_reset(&forwards->message);
	status = ssh_channel_answer_open(forwards->channels, channel, accept, &forwards->message);
	if (status != 0)
		return status;

	/* A refusal ends the entry; an acceptance carries the connection. */
	if (!accept) {
		forward_release(entry);
	} else {
		entry->state = SSHD_FORWARD_OPEN;
	}

	/* Sends the answer. */
	status = forward_send(forwards);
	if (status != 0)
		return status;

	/* Succeeded: the answer is queued. */
	return 0;
}

/*
 * Reads a forwarded socket and sends its bytes on the channel, no more
 * than the channel's room; the socket's end is sent as EOF.  Returns 0 or
 * the error of queueing a message.
 */
static int
forward_read(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	uint8_t chunk[SSHD_FORWARD_CHUNK];
	size_t room;
	ssize_t count;
	int status;

	/* Reads at most what the channel may send now. */
	room = ssh_channel_send_room(entry->channel);
	if (room > sizeof(chunk))
		room = sizeof(chunk);
	if (room == 0)
		return 0;
	count = read(entry->socket, chunk, room);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;

	/* A failed socket ends the connection. */
	if (count < 0) {
		status = forward_abort(forwards, entry);
		return status;
	}

	/* The socket's end goes to the client as EOF. */
	if (count == 0) {
		entry->socket_ended = 1;
		status = forward_settle(forwards, entry);
		return status;
	}

	/* Sends the bytes; a message that cannot be queued ends the connection. */
	ssh_buf_reset(&forwards->message);
	status = ssh_channel_put_data(entry->channel, 0, chunk, (size_t)count, &forwards->message);
	ssh_wipe(chunk, (size_t)count);
	if (status != 0)
		return status;
	status = forward_send(forwards);
	if (status != 0)
		return status;

	/* Succeeded: the bytes are queued. */
	return 0;
}

/*
 * Writes the client's queued bytes to a forwarded socket and gives the
 * window back for what was written.  Returns 0 or the error of queueing a
 * message.
 */
static int
forward_write(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	ssize_t count;
	size_t size;
	int status;

	/* Writes as much as the socket takes. */
	size = entry->queue.size - entry->queue_offset;
	count = write(entry->socket, entry->queue.data + entry->queue_offset, size);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;

	/* A socket that cannot be written ends the connection. */
	if (count <= 0) {
		status = forward_abort(forwards, entry);
		return status;
	}

	/* Steps over what was written, emptying the queue when all of it is, or every half window. */
	entry->queue_offset += (size_t)count;
	if (entry->queue_offset == entry->queue.size) {
		ssh_buf_reset(&entry->queue);
		entry->queue_offset = 0;
	} else if (entry->queue_offset >= SSHD_FORWARD_WINDOW / 2U) {
		ssh_buf_consume(&entry->queue, entry->queue_offset);
		entry->queue_offset = 0;
	}

	/* Gives the window back for what was written. */
	ssh_buf_reset(&forwards->message);
	ssh_channel_consumed(entry->channel, (size_t)count, &forwards->message);
	status = forward_send(forwards);
	if (status != 0)
		return status;

	/* Shuts or closes what the last bytes made due. */
	status = forward_settle(forwards, entry);
	if (status != 0)
		return status;

	/* Succeeded: the bytes are written. */
	return 0;
}

/*
 * Sends what is due on a forwarded channel: the EOF after the socket's
 * end, the shut of the socket after the client's last byte, and the
 * close once both ways ended or the client closed and its bytes are
 * written; frees the slot once the close went both ways.  Returns 0 or
 * the error of queueing a message.
 */
static int
forward_settle(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	int written;
	int status;

	/* A channel this side closed waits only for the client's CLOSE. */
	if (entry->state == SSHD_FORWARD_CLOSING) {
		forward_try_free(forwards, entry);
		return 0;
	}

	/* Nothing is due before the channel is open. */
	if (entry->state != SSHD_FORWARD_OPEN)
		return 0;

	/* Tells the client the socket ended. */
	if (entry->socket_ended && !entry->channel->eof_sent) {
		ssh_buf_reset(&forwards->message);
		ssh_channel_put_eof(entry->channel, &forwards->message);
		status = forward_send(forwards);
		if (status != 0)
			return status;
	}

	/* Shuts the socket's sending side once the client's last byte is written. */
	written = 0;
	if (entry->queue.size == entry->queue_offset)
		written = 1;
	if (entry->client_ended &&
	    written &&
	    !entry->shut &&
	    entry->socket >= 0) {
		(void)shutdown(entry->socket, SHUT_WR);
		entry->shut = 1;
	}

	/* Closes once both ways ended, or once the client closed and its bytes are written. */
	if (entry->socket_ended && entry->shut) {
		status = forward_close(forwards, entry);
		return status;
	}

	/* Closes a connection the client closed once its bytes are written. */
	if (entry->client_closed && written) {
		status = forward_close(forwards, entry);
		return status;
	}

	/* Succeeded: nothing more is due now. */
	return 0;
}

/*
 * Ends a forwarded connection at once (its socket failed): closes the
 * socket and the channel, dropping what waits.  Returns 0 or the error of
 * queueing the close.
 */
static int
forward_abort(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	int status;

	/* Drops the client's bytes that wait for the socket. */
	ssh_buf_reset(&entry->queue);
	entry->queue_offset = 0;

	/* Closes the socket and the channel. */
	status = forward_close(forwards, entry);
	if (status != 0)
		return status;

	/* Succeeded: the close is queued. */
	return 0;
}

/*
 * Closes a forwarded connection's socket and sends the channel's CLOSE
 * (once); the slot is freed when the client's CLOSE has come too.
 * Returns 0 or the error of queueing the close.
 */
static int
forward_close(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	int status;

	/* Closes the socket. */
	if (entry->socket >= 0)
		close(entry->socket);
	entry->socket = -1;
	entry->state = SSHD_FORWARD_CLOSING;

	/* Sends CLOSE, unless it went already. */
	if (!entry->channel->close_sent) {
		ssh_buf_reset(&forwards->message);
		ssh_channel_put_close(entry->channel, &forwards->message);
		status = forward_send(forwards);
		if (status != 0)
			return status;
	}

	/* Frees the slot when the client closed too. */
	forward_try_free(forwards, entry);

	/* Succeeded: the close is queued. */
	return 0;
}

/*
 * Frees a forwarded channel's slot and entry once its close went both
 * ways.
 */
static void
forward_try_free(
	struct sshd_forwards *forwards,
	struct sshd_forward_channel *entry)
{
	int freed;

	/* Waits for the close of both sides. */
	if (!entry->channel->close_sent || !entry->channel->close_received)
		return;

	/* Frees the slot in the table, then the entry. */
	freed = ssh_channel_closed(forwards->channels, entry->channel);
	if (!freed)
		return;
	forward_release(entry);
}

/*
 * Empties a forwarded channel's entry: closes its socket, frees the
 * destination's addresses, empties its queue (keeping the memory).
 */
static void
forward_release(
	struct sshd_forward_channel *entry)
{
	/* Closes the socket. */
	if (entry->socket >= 0)
		close(entry->socket);
	entry->socket = -1;

	/* Frees the addresses of a connection being made. */
	if (entry->addresses != NULL)
		freeaddrinfo(entry->addresses);
	entry->addresses = NULL;
	entry->next_address = NULL;

	/* Forgets the bytes and every mark. */
	ssh_buf_reset(&entry->queue);
	entry->queue_offset = 0;
	entry->socket_ended = 0;
	entry->client_ended = 0;
	entry->shut = 0;
	entry->client_closed = 0;
	entry->deadline = 0;
	entry->slot = -1;

	/* The entry is free. */
	entry->channel = NULL;
	entry->state = SSHD_FORWARD_FREE;
}

/*
 * Stops a remote forwarding: closes its sockets and frees its entry.
 */
static void
forward_close_listen(
	struct sshd_forward_listen *remote)
{
	unsigned position;

	/* Closes every socket. */
	for (position = 0; position < SSHD_FORWARD_ADDRESSES; position++) {
		if (remote->sockets[position] >= 0)
			close(remote->sockets[position]);
		remote->sockets[position] = -1;
		remote->slots[position] = -1;
	}

	/* The entry is free. */
	remote->socket_count = 0;
	remote->in_use = 0;
	remote->address[0] = '\0';
	remote->port = 0;
	remote->paused_until = 0;
}

/*
 * Queues the message built in forwards->message for the client.  Returns
 * 0, or the error of building or queueing it (the connection then ends:
 * a forwarded byte is never dropped silently).
 */
static int
forward_send(
	struct sshd_forwards *forwards)
{
	int status;

	/* Refuses a message that was not built. */
	status = ssh_buf_error(&forwards->message);
	if (status != 0) {
		ssh_buf_reset(&forwards->message);
		return status;
	}

	/* Nothing was built: nothing goes. */
	if (forwards->message.size == 0)
		return 0;

	/* Queues the message for the client. */
	status = ssh_transport_send(forwards->transport, forwards->message.data, forwards->message.size);
	ssh_buf_reset(&forwards->message);
	if (status != 0)
		return status;

	/* Succeeded: the message is queued. */
	return 0;
}

/*
 * Reports whether size bytes are 1 to limit printable ASCII characters
 * without blanks (a host name or an address).
 */
static int
forward_text_ok(
	const uint8_t *text,
	size_t size,
	size_t limit)
{
	size_t index;

	/* An empty or overlong text is no host. */
	if (size == 0 || size > limit)
		return 0;

	/* Every byte must be a visible ASCII character. */
	for (index = 0; index < size; index++) {
		if (text[index] < 0x21U || text[index] > 0x7eU)
			return 0;
	}

	/* The text can be a host. */
	return 1;
}

/*
 * Keeps a socket from the children and from blocking the loop.  Returns
 * 0 or the error of the system.
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

/*
 * Writes an address in numbers and its port ("?" and 0 when it cannot
 * be written).
 */
static void
forward_name_address(
	const struct sockaddr *address,
	socklen_t length,
	char *host,
	size_t host_capacity,
	unsigned *port)
{
	char port_text[FORWARD_PORT_TEXT];
	int status;

	/* Asks for the numeric forms. */
	*port = 0;
	status = getnameinfo(address, length, host, host_capacity, port_text, sizeof(port_text), NI_NUMERICHOST | NI_NUMERICSERV);
	if (status != 0) {
		snprintf(host, host_capacity, "?");
		return;
	}

	/* Reads the port's digits. */
	*port = (unsigned)strtoul(port_text, NULL, 10);
}
