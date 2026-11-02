/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A client for sshd-pair-host.sh built from the library's own client
 * parts, for what the host's OpenSSH client never sends: requests on a
 * channel the server has closed, a change of user or service during
 * authentication, refused environment variables and repeated requests,
 * and a signal to a running command.  The expected answers come from RFC
 * 4252 5 (a server may disconnect a client that changes the user or
 * service), RFC 4254 5.3 (a request on a closing channel is failed),
 * 6.4, 6.9 and 6.10, and from the server's contract (sshd-session.c,
 * sshd-preauth.c).
 *
 *   sshd-pair-client <port> <user> <user key file> <case>
 *
 * Prints "ok <case>" or "FAIL <case>: <what>" and exits 0 or 1.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ssh-auth.h"
#include "ssh-channel.h"
#include "ssh-transport.h"

/* One connection of the client. */
struct client {
	int socket;
	struct ssh_transport transport;
	struct ssh_channels channels;
	struct ssh_buf payload;
	struct ssh_buf out;
	struct ssh_key key;
	const char *user;
};

static int client_accept_host(void *context, const uint8_t *blob, size_t size);
static int client_next(struct client *client);
static int client_send(struct client *client);
static int client_connect(struct client *client, unsigned port);
static int client_login(struct client *client);
static int client_open(struct client *client, struct ssh_channel **channel);
static int client_wait(struct client *client, unsigned type, struct ssh_channel *channel);
static int case_closing(struct client *client);
static int case_user_change(struct client *client);
static int case_service(struct client *client);
static int case_requests(struct client *client);
static int case_signal(struct client *client);
static int fail(const char *name, const char *what);

/* Takes any host key: the test's known_hosts is not this client's concern. */
static int
client_accept_host(
	void *context,
	const uint8_t *blob,
	size_t size)
{
	(void)context;
	(void)blob;
	(void)size;
	return 0;
}

/*
 * Takes the next upper-layer message into client->payload, reading and
 * writing the connection as needed (10 seconds at most for each wait).
 * Returns 0, ECONNABORTED (the server disconnected: the transport holds
 * the code), EPIPE or another error.
 */
static int
client_next(
	struct client *client)
{
	struct pollfd waiting;
	uint8_t chunk[16384];
	const uint8_t *pending;
	size_t size;
	ssize_t got;
	int status;

	for (;;) {
		ssh_buf_reset(&client->payload);
		status = ssh_transport_next(&client->transport, &client->payload);
		if (status != EAGAIN)
			return status;
		pending = ssh_transport_pending(&client->transport, &size);
		while (size > 0) {
			got = write(client->socket, pending, size);
			if (got <= 0)
				return EPIPE;
			ssh_transport_written(&client->transport, (size_t)got);
			pending = ssh_transport_pending(&client->transport, &size);
		}
		waiting.fd = client->socket;
		waiting.events = POLLIN;
		waiting.revents = 0;
		if (poll(&waiting, 1, 10000) <= 0)
			return ETIMEDOUT;
		got = read(client->socket, chunk, sizeof(chunk));
		if (got <= 0)
			return EPIPE;
		status = ssh_transport_input(&client->transport, chunk, (size_t)got);
		if (status != 0)
			return status;
	}
}

/* Sends the message in client->out and writes the output; 0 or an error. */
static int
client_send(
	struct client *client)
{
	const uint8_t *pending;
	size_t size;
	ssize_t got;
	int status;

	status = ssh_buf_error(&client->out);
	if (status == 0)
		status = ssh_transport_send(&client->transport, client->out.data, client->out.size);
	ssh_buf_reset(&client->out);
	if (status != 0)
		return status;
	pending = ssh_transport_pending(&client->transport, &size);
	while (size > 0) {
		got = write(client->socket, pending, size);
		if (got <= 0)
			return EPIPE;
		ssh_transport_written(&client->transport, (size_t)got);
		pending = ssh_transport_pending(&client->transport, &size);
	}
	return 0;
}

/* Connects to 127.0.0.1:port and starts the transport; the exchange runs as messages are taken. */
static int
client_connect(
	struct client *client,
	unsigned port)
{
	struct sockaddr_in address;
	struct ssh_transport_ops ops;
	char service[SSH_NAME_MAX + 1];
	int status;

	client->socket = socket(AF_INET, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((unsigned short)port);
	if (connect(client->socket, (struct sockaddr *)&address, sizeof(address)) != 0)
		return EIO;
	memset(&ops, 0, sizeof(ops));
	ops.check_host_key = client_accept_host;
	status = ssh_transport_init(&client->transport, SSH_ROLE_CLIENT, &ops, 0);
	if (status != 0)
		return status;
	ssh_channels_init(&client->channels);
	ssh_buf_init(&client->payload, SSH_PACKET_LIMIT, 1);
	ssh_buf_init(&client->out, SSH_PACKET_LIMIT, 1);

	/* The service request, answered once the exchange is done. */
	ssh_auth_put_service_request(SSH_SERVICE_USERAUTH, &client->out);
	status = client_send(client);
	if (status != 0)
		return status;
	status = client_next(client);
	if (status != 0)
		return status;
	if (client->payload.data[0] != SSH_MSG_SERVICE_ACCEPT)
		return EPROTO;
	status = ssh_auth_parse_service(client->payload.data, client->payload.size, service);
	if (status != 0 || strcmp(service, SSH_SERVICE_USERAUTH) != 0)
		return EPROTO;
	return 0;
}

/* Logs in with the user key (signed publickey); 0 on USERAUTH_SUCCESS. */
static int
client_login(
	struct client *client)
{
	int status;

	status = ssh_auth_put_publickey_signed(client->user, &client->key, client->transport.session_id, &client->out);
	if (status == 0)
		status = client_send(client);
	if (status == 0)
		status = client_next(client);
	if (status != 0)
		return status;
	if (client->payload.data[0] != SSH_MSG_USERAUTH_SUCCESS)
		return EACCES;
	return 0;
}

/* Opens a session channel; 0 with *channel open. */
static int
client_open(
	struct client *client,
	struct ssh_channel **channel)
{
	uint32_t reason;
	int status;

	status = ssh_channel_open_session(&client->channels, &client->out, channel);
	if (status == 0)
		status = client_send(client);
	if (status == 0)
		status = client_next(client);
	if (status != 0)
		return status;
	status = ssh_channel_open_reply(&client->channels, client->payload.data, client->payload.size, channel, &reason);
	return status;
}

/*
 * Takes messages until one of type comes for channel (data, window
 * adjustments and EOF in between are read and dropped, the window given
 * back); 0 with it in client->payload.
 */
static int
client_wait(
	struct client *client,
	unsigned type,
	struct ssh_channel *channel)
{
	struct ssh_channel *other;
	const uint8_t *data;
	size_t size;
	uint32_t extended;
	int status;

	for (;;) {
		status = client_next(client);
		if (status != 0)
			return status;
		if (client->payload.data[0] == type)
			return 0;
		if (client->payload.data[0] == SSH_MSG_CHANNEL_DATA || client->payload.data[0] == SSH_MSG_CHANNEL_EXTENDED_DATA) {
			status = ssh_channel_parse_data(&client->channels, client->payload.data, client->payload.size, &other, &data, &size, &extended);
			if (status != 0)
				return status;
			ssh_channel_consumed(other, size, &client->out);
			if (client->out.size > 0) {
				status = client_send(client);
				if (status != 0)
					return status;
			}
		} else if (client->payload.data[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
			status = ssh_channel_parse_window_adjust(&client->channels, client->payload.data, client->payload.size, &other);
			if (status != 0)
				return status;
		} else if (client->payload.data[0] == SSH_MSG_CHANNEL_EOF) {
			status = ssh_channel_parse_end(&client->channels, client->payload.data, client->payload.size, &other);
			if (status != 0)
				return status;
		} else if (client->payload.data[0] == SSH_MSG_CHANNEL_REQUEST ||
		    client->payload.data[0] == SSH_MSG_CHANNEL_SUCCESS ||
		    client->payload.data[0] == SSH_MSG_CHANNEL_FAILURE) {
			/* exit-status, or a reply, before the message waited for. */
			continue;
		} else {
			return EPROTO;
		}
		(void)channel;
	}
}

/*
 * A command that ends: the server sends exit-status, EOF, CLOSE in this
 * order after the output; a request on the channel the server has closed
 * (before this side's CLOSE) is answered with FAILURE; then the closes
 * cross, and the connection takes another channel.
 */
static int
case_closing(
	struct client *client)
{
	struct ssh_channel *channel;
	struct ssh_channel *other;
	struct ssh_channel_request request;
	const uint8_t *data;
	size_t size;
	uint32_t extended;
	int status;
	int success;
	int seen_data;
	int seen_status;
	int seen_eof;

	status = client_login(client);
	if (status != 0)
		return fail("closing", "login");
	status = client_open(client, &channel);
	if (status != 0)
		return fail("closing", "open");
	ssh_channel_put_exec(channel, (const uint8_t *)"echo closing-output", 19, &client->out);
	status = client_send(client);
	if (status != 0)
		return fail("closing", "exec");

	/* The exec's reply, the output, exit-status, EOF and CLOSE, in that order. */
	seen_data = seen_status = seen_eof = 0;
	for (;;) {
		status = client_next(client);
		if (status != 0)
			return fail("closing", "the end did not come");
		if (client->payload.data[0] == SSH_MSG_CHANNEL_SUCCESS)
			continue;
		if (client->payload.data[0] == SSH_MSG_CHANNEL_DATA) {
			status = ssh_channel_parse_data(&client->channels, client->payload.data, client->payload.size, &other, &data, &size, &extended);
			if (status != 0 || seen_status || seen_eof)
				return fail("closing", "data after the exit status or EOF");
			if (size != 15 || memcmp(data, "closing-output\n", 15) != 0)
				return fail("closing", "the output differs");
			seen_data = 1;
			continue;
		}
		if (client->payload.data[0] == SSH_MSG_CHANNEL_REQUEST) {
			status = ssh_channel_parse_request(&client->channels, client->payload.data, client->payload.size, &request);
			if (status != 0 || request.type != SSH_CREQ_EXIT_STATUS || request.status != 0 || request.want_reply)
				return fail("closing", "not exit-status 0");
			if (!seen_data || seen_eof)
				return fail("closing", "exit-status out of order");
			seen_status = 1;
			continue;
		}
		if (client->payload.data[0] == SSH_MSG_CHANNEL_EOF) {
			status = ssh_channel_parse_end(&client->channels, client->payload.data, client->payload.size, &other);
			if (status != 0 || !seen_status)
				return fail("closing", "EOF before the exit status");
			seen_eof = 1;
			continue;
		}
		if (client->payload.data[0] == SSH_MSG_CHANNEL_CLOSE) {
			status = ssh_channel_parse_end(&client->channels, client->payload.data, client->payload.size, &other);
			if (status != 0 || !seen_eof)
				return fail("closing", "CLOSE before EOF");
			break;
		}
		return fail("closing", "an unexpected message");
	}

	/* The server's side is closing: a request is failed, data is taken. */
	ssh_channel_put_env(channel, "LANG", "C", &client->out);
	client->out.data[1U + 4U + 4U + 3U] = 1;
	status = client_send(client);
	if (status != 0)
		return fail("closing", "send");
	status = client_next(client);
	if (status != 0 || client->payload.data[0] != SSH_MSG_CHANNEL_FAILURE)
		return fail("closing", "a request on the closing channel was not failed");
	status = ssh_channel_parse_reply(&client->channels, client->payload.data, client->payload.size, &other, &success);
	if (status != 0 || success)
		return fail("closing", "the failure");
	ssh_channel_put_close(channel, &client->out);
	status = client_send(client);
	if (status != 0)
		return fail("closing", "close");
	(void)ssh_channel_closed(&client->channels, channel);

	/* The connection still works: a second channel runs a command. */
	status = client_open(client, &channel);
	if (status != 0)
		return fail("closing", "second open");
	ssh_channel_put_exec(channel, (const uint8_t *)"true", 4, &client->out);
	status = client_send(client);
	if (status == 0)
		status = client_wait(client, SSH_MSG_CHANNEL_CLOSE, channel);
	if (status != 0)
		return fail("closing", "second command");
	return 0;
}

/* The user changes between two requests: the server disconnects (protocol error). */
static int
case_user_change(
	struct client *client)
{
	int status;

	ssh_auth_put_none(client->user, &client->out);
	status = client_send(client);
	if (status == 0)
		status = client_next(client);
	if (status != 0 || client->payload.data[0] != SSH_MSG_USERAUTH_FAILURE)
		return fail("user-change", "none was not failed");
	ssh_auth_put_none("root", &client->out);
	status = client_send(client);
	if (status == 0)
		status = client_next(client);
	if (status != ECONNABORTED || client->transport.reason_code != SSH_DISCONNECT_PROTOCOL_ERROR)
		return fail("user-change", "no disconnect");
	return 0;
}

/* A first request for another service than ssh-connection: disconnected with SERVICE_NOT_AVAILABLE. */
static int
case_service(
	struct client *client)
{
	int status;

	ssh_buf_put_u8(&client->out, SSH_MSG_USERAUTH_REQUEST);
	ssh_buf_put_cstring(&client->out, client->user);
	ssh_buf_put_cstring(&client->out, "ssh-other");
	ssh_buf_put_cstring(&client->out, "none");
	status = client_send(client);
	if (status == 0)
		status = client_next(client);
	if (status != ECONNABORTED || client->transport.reason_code != SSH_DISCONNECT_SERVICE_NOT_AVAILABLE)
		return fail("service", "no disconnect");
	return 0;
}

/*
 * Requests before and after the command starts: env of a name that is
 * not taken fails, LANG succeeds, a second pty-req fails, a second exec
 * fails, a subsystem other than sftp fails.
 */
static int
case_requests(
	struct client *client)
{
	struct ssh_channel *channel;
	struct ssh_channel *other;
	int status;
	int success;
	int index;
	static const char *names[] = { "LD_PRELOAD", "LANG", "LC_", "lc_all", "LC_ALL" };
	static const int expected[] = { 0, 1, 0, 0, 1 };

	status = client_login(client);
	if (status == 0)
		status = client_open(client, &channel);
	if (status != 0)
		return fail("requests", "login or open");

	/* env with a reply wanted (the field after the name in RFC 4254 6.4 is set by hand). */
	for (index = 0; index < 5; index++) {
		ssh_channel_put_env(channel, names[index], "x", &client->out);
		client->out.data[1U + 4U + 4U + 3U] = 1;
		status = client_send(client);
		if (status == 0)
			status = client_next(client);
		if (status != 0)
			return fail("requests", "env");
		status = ssh_channel_parse_reply(&client->channels, client->payload.data, client->payload.size, &other, &success);
		if (status != 0 || success != expected[index]) {
			fprintf(stderr, "env %s: %d\n", names[index], success);
			return fail("requests", "env answer");
		}
	}

	/* Two pty-req: the second fails. */
	for (index = 0; index < 2; index++) {
		ssh_channel_put_pty(channel, "xterm", 80, 24, 0, 0, (const uint8_t *)"", 0, &client->out);
		status = client_send(client);
		if (status == 0)
			status = client_next(client);
		if (status != 0)
			return fail("requests", "pty");
		status = ssh_channel_parse_reply(&client->channels, client->payload.data, client->payload.size, &other, &success);
		if (status != 0 || success != (index == 0))
			return fail("requests", "pty answer");
	}

	/* A subsystem that is not offered fails; then exec, then a second exec fails. */
	ssh_channel_put_subsystem(channel, "other", &client->out);
	status = client_send(client);
	if (status == 0)
		status = client_next(client);
	if (status != 0 || client->payload.data[0] != SSH_MSG_CHANNEL_FAILURE)
		return fail("requests", "subsystem other");
	ssh_channel_put_exec(channel, (const uint8_t *)"sleep 1", 7, &client->out);
	status = client_send(client);
	if (status == 0)
		status = client_next(client);
	if (status != 0 || client->payload.data[0] != SSH_MSG_CHANNEL_SUCCESS)
		return fail("requests", "exec");
	ssh_channel_put_exec(channel, (const uint8_t *)"true", 4, &client->out);
	status = client_send(client);
	if (status == 0)
		status = client_wait(client, SSH_MSG_CHANNEL_FAILURE, channel);
	if (status != 0)
		return fail("requests", "second exec was not failed");
	ssh_channel_put_env(channel, "LANG", "x", &client->out);
	client->out.data[1U + 4U + 4U + 3U] = 1;
	status = client_send(client);
	if (status == 0)
		status = client_wait(client, SSH_MSG_CHANNEL_FAILURE, channel);
	if (status != 0)
		return fail("requests", "env after the start was not failed");
	status = client_wait(client, SSH_MSG_CHANNEL_CLOSE, channel);
	if (status != 0)
		return fail("requests", "the end");
	return 0;
}

/* A signal to a running command: it ends with exit-signal TERM. */
static int
case_signal(
	struct client *client)
{
	struct ssh_channel *channel;
	struct ssh_channel_request request;
	int status;

	status = client_login(client);
	if (status == 0)
		status = client_open(client, &channel);
	if (status != 0)
		return fail("signal", "login or open");
	ssh_channel_put_exec(channel, (const uint8_t *)"sleep 30", 8, &client->out);
	status = client_send(client);
	if (status == 0)
		status = client_wait(client, SSH_MSG_CHANNEL_SUCCESS, channel);
	if (status != 0)
		return fail("signal", "exec");

	/* "signal" (RFC 4254 6.9): boolean FALSE, string "TERM". */
	sleep(1);
	ssh_buf_put_u8(&client->out, SSH_MSG_CHANNEL_REQUEST);
	ssh_buf_put_u32(&client->out, channel->remote_id);
	ssh_buf_put_cstring(&client->out, "signal");
	ssh_buf_put_bool(&client->out, 0);
	ssh_buf_put_cstring(&client->out, "TERM");
	status = client_send(client);
	if (status == 0)
		status = client_wait(client, SSH_MSG_CHANNEL_REQUEST, channel);
	if (status != 0)
		return fail("signal", "no exit request");
	status = ssh_channel_parse_request(&client->channels, client->payload.data, client->payload.size, &request);
	if (status != 0 || request.type != SSH_CREQ_EXIT_SIGNAL || strcmp(request.name, "TERM") != 0)
		return fail("signal", "not exit-signal TERM");
	return 0;
}

/* Prints a failure and returns 1. */
static int
fail(
	const char *name,
	const char *what)
{
	printf("FAIL %s: %s\n", name, what);
	return 1;
}

/*
 * Runs one case against the server on port.
 */
int
main(
	int argc,
	char **argv)
{
	static char text[SSH_KEY_FILE_MAX + 1U];
	struct client client;
	ssize_t got;
	int descriptor;
	int status;

	if (argc != 5)
		return 2;
	memset(&client, 0, sizeof(client));
	client.user = argv[2];
	descriptor = open(argv[3], O_RDONLY);
	if (descriptor < 0)
		return 2;
	got = read(descriptor, text, sizeof(text) - 1U);
	(void)close(descriptor);
	if (got <= 0 || ssh_key_parse_private(text, (size_t)got, &client.key) != 0)
		return 2;
	status = client_connect(&client, (unsigned)atoi(argv[1]));
	if (status != 0) {
		printf("FAIL %s: connect %d\n", argv[4], status);
		return 1;
	}
	if (strcmp(argv[4], "closing") == 0)
		status = case_closing(&client);
	else if (strcmp(argv[4], "user-change") == 0)
		status = case_user_change(&client);
	else if (strcmp(argv[4], "service") == 0)
		status = case_service(&client);
	else if (strcmp(argv[4], "requests") == 0)
		status = case_requests(&client);
	else if (strcmp(argv[4], "signal") == 0)
		status = case_signal(&client);
	else
		status = fail(argv[4], "no such case");
	if (status == 0)
		printf("ok %s\n", argv[4]);
	ssh_transport_free(&client.transport);
	ssh_buf_free(&client.payload);
	ssh_buf_free(&client.out);
	ssh_key_clear(&client.key);
	(void)close(client.socket);
	return status;
}
