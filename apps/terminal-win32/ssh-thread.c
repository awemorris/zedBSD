/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A tab's SSH connection on a thread of its own (session.h): it chooses
 * the host (the Connect host, or the one the user types at "Host:" from
 * the list of ssh\config's hosts), reads ssh\config for it, asks for the
 * user when none is set ("login as:"), connects, and runs zedBSD's SSH
 * client engine on the socket until the connection ends.
 *
 * The socket is always read, so the transport's own messages (a
 * re-exchange, a keepalive, a DISCONNECT) are never held up; the server's
 * window is given back as the tab reads, not as the bytes come.  The
 * questions the connection needs (an unknown host's key, a password) are
 * asked in the tab while the socket goes on.
 *
 * A connection that ends normally ends the tab.  One that fails leaves
 * why in the tab and waits for Enter to connect again (Ctrl+C or Ctrl+D
 * closes the tab).  When the tab closes, the thread sends DISCONNECT,
 * waits a little for it to leave, and lets the session go.
 */

#include "session.h"

#include "userland/base/ssh/ssh-client-engine.h"
#include "userland/base/ssh/ssh-key.h"

#include <compat/crypto/crypto.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most addresses of a host tried, and the most hosts of ssh\config listed. */
#define THREAD_ADDRESSES	8U
#define THREAD_HOSTS		32U

/* The longest name of a host in the list, and the longest reason a connection ended. */
#define THREAD_NAME_MAX		256U
#define THREAD_REASON_MAX	(SSH_ENGINE_REASON_MAX + 128U)

/* The bytes read from the socket at once. */
#define THREAD_RECEIVE		16384U

/* The longest wait of one round when nothing is due, and how long DISCONNECT may take to leave, in milliseconds. */
#define THREAD_ROUND_MS		1000U
#define THREAD_FAREWELL_MS	1000U

/* Where the settings of the hosts are, inside the folder. */
#define THREAD_SSH_CONFIG	"ssh/config"

/* What an attempt to connect came to. */
#define THREAD_DONE		0
#define THREAD_FAILED		1
#define THREAD_CLOSING		2
#define THREAD_GIVEN_UP		3

/* What the engine asked and the user has not answered yet. */
#define THREAD_ASKED_NONE	0U
#define THREAD_ASKED_HOST_KEY	1U
#define THREAD_ASKED_PASSWORD	2U

/*
 * What the user chose to connect to: the name given (a Host of ssh\config,
 * or a host's name or address), and the user and port given with it
 * (user@host:port; empty and 0 when not given).
 */
struct thread_target {
	char host[MIKUTERM_HOST_MAX];
	char user[SSH_USER_MAX + 1];
	uint16_t port;
};

/*
 * The hosts ssh\config has settings for, as the list "Host:" shows: their
 * names in the order written, each once.
 */
struct thread_hosts {
	char names[THREAD_HOSTS][THREAD_NAME_MAX];
	unsigned count;
};

/*
 * One attempt to connect: the settings for the host, the socket and the
 * engine, the bytes read and not yet taken by the engine, the question
 * waiting for the user, and how the attempt ended.
 */
struct thread_link {
	struct mikuterm_session *session;
	struct thread_target *target;
	struct ssh_config config;
	const char *host_name;
	struct kl_socket *socket;
	struct ssh_engine *engine;
	uint8_t received[THREAD_RECEIVE];
	size_t received_start;
	size_t received_end;
	unsigned asked;
	uint8_t host_key[SSH_ED25519_PUBLIC];
	int host_refused;
	int disconnecting;
	uint64_t farewell_ms;
	int closed;
	int error;
	char reason[THREAD_REASON_MAX];
};

static int thread_choose(struct mikuterm_session *session, struct thread_target *target);
static int thread_host_named(void *data, const char *name, size_t size);
static int thread_parse_target(const char *text, struct thread_target *target);
static int thread_attempt(struct thread_link *link);
static int thread_settings(struct thread_link *link);
static int thread_dial(struct thread_link *link);
static int thread_identities(struct thread_link *link, struct ssh_engine_identity *identities, unsigned *count);
static void thread_identities_free(struct ssh_engine_identity *identities, unsigned count);
static int thread_converse(struct thread_link *link);
static void thread_events(struct thread_link *link);
static void thread_host_key(struct thread_link *link, const struct ssh_engine_event *event);
static void thread_host_answer(struct thread_link *link, const char *answer, int cancelled);
static int thread_known_hosts_add(struct thread_link *link);
static void thread_closed(struct thread_link *link, const struct ssh_engine_event *event);
static void thread_from_tab(struct thread_link *link);
static void thread_answers(struct thread_link *link);
static int thread_send(struct thread_link *link);
static int thread_receive(struct thread_link *link);
static void thread_feed(struct thread_link *link);
static int thread_ask(struct mikuterm_session *session, const char *prompt, unsigned kind, char *answer, size_t size);
static void thread_ask_start(struct mikuterm_session *session, const char *prompt, unsigned kind);
static int thread_ask_take(struct mikuterm_session *session, char *answer, size_t size, int *cancelled);
static int thread_closing(struct mikuterm_session *session);
static void thread_say_bytes(struct mikuterm_session *session, const void *bytes, size_t size);
static const char *thread_error_text(int error);
static uint64_t thread_now(void);

/*
 * Runs one tab's connection, from choosing the host to its end, and lets
 * the session go (the thread's whole life).
 */
void
session_run(
	void *argument)
{
	struct mikuterm_session *session;
	struct thread_target target;
	struct thread_link *link;
	char answer[8];
	int status;

	/* The session; the settings' problems come first. */
	session = argument;
	if (session->notice[0] != '\0')
		session_say(session, session->notice);

	/* The attempts' state is large: it is on the heap. */
	link = malloc(sizeof(*link));
	if (link == NULL) {
		session_say(session, "mikuterm: out of memory\r\n");
		session_ended(session);
		session_release(session);
		session_thread_ended();
		return;
	}

	/* The host; giving the question up ends the tab. */
	status = thread_choose(session, &target);
	if (status == ECANCELED)
		session_ended(session);

	/* Connects until the connection ends well, the tab closes, or the user gives up after a failure. */
	while (status == 0) {
		/* One attempt. */
		memset(link, 0, sizeof(*link));
		link->session = session;
		link->target = &target;
		status = thread_attempt(link);

		/* A normal end, or the user giving up, ends the tab. */
		if (status == THREAD_DONE || status == THREAD_GIVEN_UP) {
			session_ended(session);
			break;
		}

		/* A tab that closed wants nothing more. */
		if (status == THREAD_CLOSING)
			break;

		/* Why it failed, and Enter to try again. */
		session_say(session, "\r\n");
		session_say(session, link->reason);
		session_say(session, "\r\n");
		status = thread_ask(session, "Press Enter to connect again (Ctrl+C closes the tab). ", SESSION_ASK_LINE, answer, sizeof(answer));
		if (status == ECANCELED)
			session_ended(session);
	}

	/* The thread lets go of the session, and says it ended. */
	OPENSSL_cleanse(link, sizeof(*link));
	free(link);
	session_release(session);
	session_thread_ended();
}

/*
 * Chooses what to connect to: the session's host when it has one, else
 * the one the user types after the list of ssh\config's hosts (a name, a
 * number of the list, or user@host:port).  Returns 0, ECANCELED when the
 * user gave up, or EPIPE when the tab closed.
 */
static int
thread_choose(
	struct mikuterm_session *session,
	struct thread_target *target)
{
	struct thread_hosts *hosts;
	char path[MIKUTERM_PATH_MAX];
	char answer[MIKUTERM_HOST_MAX];
	char line[THREAD_NAME_MAX + 16U];
	char *text;
	char *end;
	size_t size;
	unsigned long number;
	unsigned index;
	int error;

	/* The host given. */
	memset(target, 0, sizeof(*target));
	if (session->host[0] != '\0') {
		error = thread_parse_target(session->host, target);
		if (error == 0)
			return 0;
	}

	/* The list of the hosts: ssh\config's names (none when it is not there). */
	hosts = calloc(1U, sizeof(*hosts));
	if (hosts == NULL)
		return ECANCELED;
	text = NULL;
	size = 0U;
	error = mikuterm_path(path, sizeof(path), THREAD_SSH_CONFIG);
	if (error == 0)
		error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error == 0) {
		(void)ssh_config_hosts(text, size, thread_host_named, hosts, NULL);
		free(text);
	}

	/* Shown with their numbers. */
	if (hosts->count == 0U)
		session_say(session, "ssh\\config names no host: type one (user@host:port).\r\n");
	for (index = 0U; index < hosts->count; index++) {
		(void)snprintf(line, sizeof(line), "  %2u  %s\r\n", index + 1U, hosts->names[index]);
		session_say(session, line);
	}

	/* The answer, until it is not empty. */
	for (;;) {
		error = thread_ask(session, "Host: ", SESSION_ASK_LINE, answer, sizeof(answer));
		if (error != 0) {
			free(hosts);
			return error;
		}
		if (answer[0] == '\0')
			continue;

		/* A number of the list is its host. */
		number = strtoul(answer, &end, 10);
		if (*end == '\0' && number >= 1UL && number <= hosts->count)
			(void)snprintf(answer, sizeof(answer), "%s", hosts->names[number - 1UL]);

		/* The host, the user and the port typed. */
		error = thread_parse_target(answer, target);
		if (error == 0)
			break;
		session_say(session, "That is not a host: type a name, a number, or user@host:port.\r\n");
	}

	/* Succeeded: the target is chosen. */
	free(hosts);
	return 0;
}

/* Adds one Host name of ssh\config to the list, once; answers 0 to go on. */
static int
thread_host_named(
	void *data,
	const char *name,
	size_t size)
{
	struct thread_hosts *hosts;
	unsigned index;
	int differs;

	/* A full list, or a name too long to show, is left out. */
	hosts = data;
	if (hosts->count == THREAD_HOSTS || size >= THREAD_NAME_MAX)
		return 0;

	/* A name already listed. */
	for (index = 0U; index < hosts->count; index++) {
		/* The same name. */
		differs = strncmp(hosts->names[index], name, size);
		if (differs == 0 && hosts->names[index][size] == '\0')
			return 0;
	}

	/* The name, at the end. */
	memcpy(hosts->names[hosts->count], name, size);
	hosts->names[hosts->count][size] = '\0';
	hosts->count++;
	return 0;
}

/*
 * Reads what to connect to: [user@]host[:port], the host a name, an IPv4
 * address, or an IPv6 one in brackets.  Returns 0 or EINVAL.
 */
static int
thread_parse_target(
	const char *text,
	struct thread_target *target)
{
	const char *host;
	const char *at;
	const char *colon;
	const char *close;
	const char *second;
	const char *space;
	unsigned long port;
	size_t length;
	char *end;

	/* The user before the '@'. */
	memset(target, 0, sizeof(*target));
	host = text;
	at = strrchr(text, '@');
	if (at != NULL) {
		length = (size_t)(at - text);
		if (length == 0U || length > SSH_USER_MAX)
			return EINVAL;
		memcpy(target->user, text, length);
		target->user[length] = '\0';
		host = at + 1;
	}

	/* The port after the host: after "]" for an address in brackets, after the one ':' otherwise. */
	colon = NULL;
	if (host[0] == '[') {
		close = strchr(host, ']');
		if (close == NULL)
			return EINVAL;
		if (close[1] == ':')
			colon = close + 1;
		else if (close[1] != '\0')
			return EINVAL;
		length = (size_t)(close - host - 1);
		host++;
	} else {
		/* More than one ':' is an IPv6 address without brackets, which has no port. */
		colon = strchr(host, ':');
		second = NULL;
		if (colon != NULL)
			second = strchr(colon + 1, ':');
		if (second != NULL)
			colon = NULL;
		length = strlen(host);
		if (colon != NULL)
			length = (size_t)(colon - host);
	}

	/* The port, a number from 1 to 65535. */
	if (colon != NULL) {
		port = strtoul(colon + 1, &end, 10);
		if (colon[1] == '\0' || *end != '\0' || port == 0UL || port > 65535UL)
			return EINVAL;
		target->port = (uint16_t)port;
	}

	/* The host, not empty and without spaces. */
	if (length == 0U || length >= sizeof(target->host))
		return EINVAL;
	memcpy(target->host, host, length);
	target->host[length] = '\0';
	space = strpbrk(target->host, " \t");
	if (space != NULL)
		return EINVAL;

	/* Succeeded: the target. */
	return 0;
}

/*
 * Makes one attempt: the settings, the user, the socket and the engine,
 * then the conversation until the connection ends.  Returns THREAD_DONE,
 * THREAD_FAILED (link->reason says why), THREAD_CLOSING or
 * THREAD_GIVEN_UP.
 */
static int
thread_attempt(
	struct thread_link *link)
{
	struct ssh_engine_options options;
	struct ssh_engine_identity identities[SSH_ENGINE_IDENTITIES_MAX];
	struct mikuterm_session *session;
	char line[MIKUTERM_HOST_MAX + 64U];
	unsigned count;
	int status;
	int error;

	/* The settings for the host. */
	session = link->session;
	status = thread_settings(link);
	if (status != THREAD_DONE)
		return status;

	/* The user, asked once when none is set. */
	if (link->config.user[0] == '\0') {
		error = thread_ask(session, "login as: ", SESSION_ASK_LINE, link->target->user, sizeof(link->target->user));
		if (error == ECANCELED)
			return THREAD_GIVEN_UP;
		if (error != 0)
			return THREAD_CLOSING;
		if (link->target->user[0] == '\0') {
			(void)snprintf(link->reason, sizeof(link->reason), "No user was given.");
			return THREAD_FAILED;
		}
		(void)snprintf(link->config.user, sizeof(link->config.user), "%s", link->target->user);
	}

	/* The tab's title is the host, and where it goes is said. */
	(void)snprintf(line, sizeof(line), "\033]2;%s\007Connecting to %s port %u ...\r\n", link->target->host, link->host_name, (unsigned)link->config.port);
	session_say(session, line);

	/* The connection. */
	status = thread_dial(link);
	if (status != THREAD_DONE)
		return status;

	/* The user's keys. */
	count = 0U;
	error = thread_identities(link, identities, &count);
	if (error != 0) {
		kl_socket_close(link->socket);
		(void)snprintf(link->reason, sizeof(link->reason), "The keys could not be read (%s).", thread_error_text(error));
		return THREAD_FAILED;
	}

	/* The engine: a shell on a terminal of the tab's size. */
	memset(&options, 0, sizeof(options));
	options.user = link->config.user;
	options.identities = identities;
	options.identity_count = count;
	options.pubkey = link->config.pubkey;
	options.password = link->config.password;
	options.session = 1;
	options.pty = 1;
	options.term = session->term;
	options.modes = NULL;
	options.command = NULL;
	options.alive_interval = link->config.server_alive_interval;
	options.alive_count = link->config.server_alive_count;
	options.now_ms = thread_now();
	kl_mutex_lock(&session->lock);

	options.columns = session->columns;
	options.rows = session->rows;
	options.width = session->width;
	options.height = session->height;
	session->resized = 0;

	kl_mutex_unlock(&session->lock);

	/* The engine copies the keys; the copies here are wiped. */
	error = ssh_engine_open(&options, &link->engine);
	thread_identities_free(identities, count);
	if (error != 0) {
		kl_socket_close(link->socket);
		(void)snprintf(link->reason, sizeof(link->reason), "The connection could not start (%s).", thread_error_text(error));
		return THREAD_FAILED;
	}

	/* The conversation until the end. */
	status = thread_converse(link);
	ssh_engine_close(link->engine);
	link->engine = NULL;
	kl_socket_close(link->socket);
	link->socket = NULL;

	/* Reports how it ended. */
	return status;
}

/*
 * Reads ssh\config's settings for the target, with the user and port
 * typed winning.  Returns THREAD_DONE, or THREAD_FAILED with the reason.
 */
static int
thread_settings(
	struct thread_link *link)
{
	char path[MIKUTERM_PATH_MAX];
	char error_text[SSH_CONFIG_ERROR_MAX];
	char *text;
	size_t size;
	int error;

	/* The file; none is no settings. */
	text = NULL;
	size = 0U;
	error = mikuterm_path(path, sizeof(path), THREAD_SSH_CONFIG);
	if (error == 0)
		error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error != 0 && error != ENOENT) {
		(void)snprintf(link->reason, sizeof(link->reason), "ssh\\config could not be read (%s).", thread_error_text(error));
		return THREAD_FAILED;
	}

	/* The settings for the host, then the defaults. */
	ssh_config_init(&link->config);
	error_text[0] = '\0';
	error = 0;
	if (text != NULL) {
		error = ssh_config_parse(&link->config, text, size, link->target->host, error_text);
		free(text);
	}
	if (error != 0) {
		(void)snprintf(link->reason, sizeof(link->reason), "ssh\\config: %s", error_text);
		return THREAD_FAILED;
	}
	ssh_config_defaults(&link->config);

	/* What was typed wins over the file. */
	if (link->target->user[0] != '\0')
		(void)snprintf(link->config.user, sizeof(link->config.user), "%s", link->target->user);
	if (link->target->port != 0U)
		link->config.port = link->target->port;

	/* The name connected to: HostName, or the name given. */
	link->host_name = link->target->host;
	if (link->config.host_name[0] != '\0')
		link->host_name = link->config.host_name;

	/* Succeeded: the settings. */
	return THREAD_DONE;
}

/*
 * Finds the host's addresses and connects to the first that answers, in
 * the time the settings give (ConnectTimeout of ssh\config, else
 * mikuterm.conf's).  Returns THREAD_DONE with link->socket connected,
 * THREAD_FAILED with the reason, or THREAD_CLOSING.
 */
static int
thread_dial(
	struct thread_link *link)
{
	struct kl_address addresses[THREAD_ADDRESSES];
	struct kl_socket_wait waits[2];
	struct kl_socket *socket;
	size_t count;
	size_t index;
	size_t ready;
	unsigned timeout;
	uint64_t deadline;
	uint64_t now;
	int closing;
	int error;

	/* The host's addresses (the thread waits for the system's answer). */
	error = kl_socket_resolve(link->host_name, link->config.port, KL_ADDRESS_ANY, KL_SOCKET_STREAM, addresses, THREAD_ADDRESSES, &count);
	if (error != 0) {
		(void)snprintf(link->reason, sizeof(link->reason), "ssh: Could not resolve hostname %s: %s", link->host_name, thread_error_text(error));
		return THREAD_FAILED;
	}

	/* The time for each address. */
	timeout = link->config.connect_timeout;
	if (timeout == 0U)
		timeout = link->session->connect_timeout;

	/* Each address in turn, until one connects. */
	error = ENOENT;
	for (index = 0U; index < count; index++) {
		/* A socket that does not block, so the wait can end when the tab closes. */
		error = kl_socket_open(addresses[index].family, KL_SOCKET_STREAM, KL_SOCKET_NONBLOCKING, &socket);
		if (error != 0)
			continue;
		error = kl_socket_connect(socket, &addresses[index]);
		if (error == EINPROGRESS) {
			/* The connection's end, the tab's close, or the time. */
			deadline = thread_now() + (uint64_t)timeout * 1000U;
			for (;;) {
				waits[0].socket = socket;
				waits[0].waker = NULL;
				waits[0].events = KL_SOCKET_WRITABLE;
				waits[0].ready = 0U;
				waits[1].socket = NULL;
				waits[1].waker = link->session->waker;
				waits[1].events = KL_SOCKET_READABLE;
				waits[1].ready = 0U;
				now = thread_now();
				if (timeout != 0U && now >= deadline) {
					error = ETIMEDOUT;
					break;
				}
				error = kl_socket_poll(waits, 2U, (int)THREAD_ROUND_MS, &ready);
				if (error != 0)
					break;

				/* A tab that closed wants no connection. */
				closing = thread_closing(link->session);
				if (closing) {
					kl_socket_close(socket);
					return THREAD_CLOSING;
				}
				if ((waits[0].ready & (KL_SOCKET_WRITABLE | KL_SOCKET_FAILED)) != 0U) {
					error = kl_socket_connect_result(socket);
					break;
				}
			}
		}

		/* Connected. */
		if (error == 0) {
			link->socket = socket;
			return THREAD_DONE;
		}
		kl_socket_close(socket);
	}

	/* No address answered. */
	(void)snprintf(link->reason, sizeof(link->reason), "ssh: connect to host %s port %u: %s", link->host_name, (unsigned)link->config.port, thread_error_text(error));
	return THREAD_FAILED;
}

/*
 * Reads the identity files the settings name (a file that is not there
 * is passed over).  Returns 0, or the error of a file that is there but
 * cannot be read.
 */
static int
thread_identities(
	struct thread_link *link,
	struct ssh_engine_identity *identities,
	unsigned *count)
{
	char path[MIKUTERM_PATH_MAX];
	char *text;
	size_t size;
	unsigned index;
	int error;

	/* Each file in the order the settings give. */
	*count = 0U;
	for (index = 0U; index < link->config.identity_count && index < SSH_ENGINE_IDENTITIES_MAX; index++) {
		/* Its path in the folder; one that names nothing is passed over. */
		error = mikuterm_ssh_path(path, sizeof(path), link->config.identity[index]);
		if (error != 0)
			continue;

		/* Its bytes. */
		error = mikuterm_file_read(path, SSH_KEY_FILE_MAX, &text, &size);
		if (error == ENOENT)
			continue;
		if (error != 0) {
			thread_identities_free(identities, *count);
			*count = 0U;
			return error;
		}

		/* One more identity. */
		identities[*count].name = link->config.identity[index];
		identities[*count].file = (const uint8_t *)text;
		identities[*count].size = size;
		(*count)++;
	}

	/* Succeeded: the identities that are there. */
	return 0;
}

/* Wipes and frees the identity files read. */
static void
thread_identities_free(
	struct ssh_engine_identity *identities,
	unsigned count)
{
	unsigned index;

	/* Each file's bytes are a private key. */
	for (index = 0U; index < count; index++) {
		OPENSSL_cleanse((void *)identities[index].file, identities[index].size);
		free((void *)identities[index].file);
	}
}

/*
 * Runs the connection: the engine's events, the user's answers, the
 * tab's keys and size, the output, the timers and the socket, round
 * after round until the engine says the connection ended.  Returns
 * THREAD_DONE (ended normally), THREAD_FAILED, THREAD_CLOSING or
 * THREAD_GIVEN_UP.
 */
static int
thread_converse(
	struct thread_link *link)
{
	struct kl_socket_wait waits[2];
	uint64_t now;
	uint64_t wake;
	uint64_t wait;
	size_t ready;
	size_t pending;
	int error;

	/* Round after round. */
	for (;;) {
		/* What the engine has to say, and what the bytes read so far still hold. */
		thread_feed(link);
		thread_events(link);

		/* The end: what is left to send goes, briefly. */
		if (link->closed) {
			(void)thread_send(link);
			break;
		}

		/* The user's answers, and what the tab has: the keys, the size, how much it read, its close. */
		thread_answers(link);
		thread_from_tab(link);

		/* A tab gone too long ago to say goodbye. */
		now = thread_now();
		if (link->disconnecting && now >= link->farewell_ms) {
			link->closed = 1;
			break;
		}

		/* The output; a socket that failed is the connection's end. */
		error = thread_send(link);
		if (error != 0) {
			(void)ssh_engine_input(link->engine, NULL, 0U, &pending);
			continue;
		}

		/* The timers: a re-exchange, a keepalive. */
		wake = now + THREAD_ROUND_MS;
		(void)ssh_engine_tick(link->engine, now, &wake);

		/* The wait: the socket (and its room while output waits), the tab, the next timer. */
		wait = 0U;
		if (wake > now)
			wait = wake - now;
		if (wait > THREAD_ROUND_MS)
			wait = THREAD_ROUND_MS;
		if (link->received_start != link->received_end)
			wait = 0U;
		(void)ssh_engine_output(link->engine, &pending);
		waits[0].socket = link->socket;
		waits[0].waker = NULL;
		waits[0].events = KL_SOCKET_READABLE;
		if (pending != 0U)
			waits[0].events |= KL_SOCKET_WRITABLE;
		waits[0].ready = 0U;
		waits[1].socket = NULL;
		waits[1].waker = link->session->waker;
		waits[1].events = KL_SOCKET_READABLE;
		waits[1].ready = 0U;
		error = kl_socket_poll(waits, 2U, (int)wait, &ready);
		if (error != 0) {
			(void)snprintf(link->reason, sizeof(link->reason), "The wait for the network failed (%s).", thread_error_text(error));
			return THREAD_FAILED;
		}

		/* What came; a socket that ended or failed is the connection's end. */
		if ((waits[0].ready & (KL_SOCKET_READABLE | KL_SOCKET_FAILED)) != 0U)
			(void)thread_receive(link);
	}

	/* The tab closed. */
	if (link->disconnecting)
		return THREAD_CLOSING;

	/* A normal end. */
	if (link->error == 0)
		return THREAD_DONE;

	/* The user did not want the host or gave the password up: the tab shows why and waits. */
	return THREAD_FAILED;
}

/* Takes the engine's events until it has none: the questions, the data for the tab, the end. */
static void
thread_events(
	struct thread_link *link)
{
	struct ssh_engine_event event;
	struct mikuterm_session *session;
	char prompt[SSH_USER_MAX + MIKUTERM_HOST_MAX + 32U];
	int error;

	/* Each event in turn. */
	session = link->session;
	for (;;) {
		error = ssh_engine_next(link->engine, &event);
		if (error != 0)
			break;

		/* What it says. */
		switch (event.kind) {
		case SSH_ENGINE_HOST_KEY:
			thread_host_key(link, &event);
			break;
		case SSH_ENGINE_BANNER:
			thread_say_bytes(session, event.bytes, event.size);
			break;
		case SSH_ENGINE_PASSWORD:
			/* A password, asked in the tab without being shown. */
			if (event.tries != 0U)
				session_say(session, "Permission denied, please try again.\r\n");
			(void)snprintf(prompt, sizeof(prompt), "%s@%s's password: ", link->config.user, link->host_name);
			thread_ask_start(session, prompt, SESSION_ASK_SECRET);
			link->asked = THREAD_ASKED_PASSWORD;
			break;
		case SSH_ENGINE_DATA:
			/* The session's bytes for the tab, given back to the window as the tab reads them. */
			thread_say_bytes(session, event.bytes, event.size);
			break;
		case SSH_ENGINE_CLOSED:
			thread_closed(link, &event);
			break;
		default:
			/* The login, the start of the shell and its exit say nothing in the tab. */
			break;
		}
	}
}

/*
 * Decides about the server's host key: the user's and the folder's
 * known_hosts and StrictHostKeyChecking say whether it is accepted, added,
 * refused, or asked about.
 */
static void
thread_host_key(
	struct thread_link *link,
	const struct ssh_engine_event *event)
{
	enum ssh_known_result user_answer;
	enum ssh_known_result global_answer;
	enum ssh_engine_host_action action;
	char path[MIKUTERM_PATH_MAX];
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];
	char prompt[MIKUTERM_HOST_MAX * 2U + SSH_KEY_FINGERPRINT_SIZE + 256U];
	char *text;
	size_t size;
	int error;

	/* The key; one that is not ssh-ed25519 is refused. */
	error = ssh_key_parse_public_blob(event->bytes, event->size, link->host_key);
	if (error != 0) {
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		return;
	}

	/* The user's known_hosts (none: nothing known). */
	user_answer = SSH_KNOWN_ABSENT;
	error = mikuterm_ssh_path(path, sizeof(path), link->config.user_known_hosts);
	if (error == 0)
		error = mikuterm_file_read(path, MIKUTERM_HOSTS_MAX, &text, &size);
	if (error == 0) {
		(void)ssh_known_hosts_check(text, size, link->host_name, link->config.port, link->host_key, &user_answer, NULL);
		free(text);
	}

	/* The global one, when the settings name a file of the folder or of Windows. */
	global_answer = SSH_KNOWN_ABSENT;
	error = mikuterm_ssh_path(path, sizeof(path), link->config.global_known_hosts);
	if (error == 0)
		error = mikuterm_file_read(path, MIKUTERM_HOSTS_MAX, &text, &size);
	if (error == 0) {
		(void)ssh_known_hosts_check(text, size, link->host_name, link->config.port, link->host_key, &global_answer, NULL);
		free(text);
	}

	/* What to do. */
	error = ssh_engine_host_decide(user_answer, global_answer, link->config.strict, link->config.batch_mode, &action);
	if (error != 0)
		action = SSH_ENGINE_HOST_REFUSE_UNKNOWN;
	(void)ssh_key_fingerprint(link->host_key, fingerprint, sizeof(fingerprint));

	/* Accepted, added, asked about or refused. */
	switch (action) {
	case SSH_ENGINE_HOST_ACCEPT:
		(void)ssh_engine_host_answer(link->engine, 1);
		break;
	case SSH_ENGINE_HOST_ADD:
		(void)thread_known_hosts_add(link);
		(void)ssh_engine_host_answer(link->engine, 1);
		break;
	case SSH_ENGINE_HOST_ASK:
		(void)snprintf(prompt,
			       sizeof(prompt),
			       "The authenticity of host '%s' can't be established.\r\n"
			       "ED25519 key fingerprint is %s.\r\n"
			       "Are you sure you want to continue connecting (yes/no)? ",
			       link->host_name,
			       fingerprint);
		thread_ask_start(link->session, prompt, SESSION_ASK_LINE);
		link->asked = THREAD_ASKED_HOST_KEY;
		break;
	case SSH_ENGINE_HOST_REFUSE_CHANGED:
		(void)snprintf(prompt,
			       sizeof(prompt),
			       "WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\r\n"
			       "The ED25519 key of %s is now %s, not the one in ssh\\known_hosts.\r\n",
			       link->host_name,
			       fingerprint);
		session_say(link->session, prompt);
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		break;
	default:
		/* Revoked, or unknown where the settings do not let it be added. */
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		break;
	}
}

/* Takes the user's answer about an unknown host: yes accepts and keeps the key, no refuses, anything else asks again. */
static void
thread_host_answer(
	struct thread_link *link,
	const char *answer,
	int cancelled)
{
	int yes_differs;
	int no_differs;
	int yes;
	int no;

	/* Yes or no, in any case; giving up is no. */
	yes_differs = _stricmp(answer, "yes");
	no_differs = _stricmp(answer, "no");
	yes = 0;
	no = 0;
	if (cancelled) {
		no = 1;
	} else if (yes_differs == 0) {
		yes = 1;
	} else if (no_differs == 0) {
		no = 1;
	}

	/* Anything else asks again. */
	if (!yes && !no) {
		thread_ask_start(link->session, "Please type 'yes' or 'no': ", SESSION_ASK_LINE);
		return;
	}

	/* No ends the connection. */
	link->asked = THREAD_ASKED_NONE;
	if (no) {
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		return;
	}

	/* Yes: the key is kept for the next time (a folder that cannot keep it accepts it for now). */
	(void)thread_known_hosts_add(link);
	(void)ssh_engine_host_answer(link->engine, 1);
}

/*
 * Appends the host's line to the user's known_hosts, in one write, and
 * says so.  Returns 0 or the error (the key is then accepted for this
 * connection alone).
 */
static int
thread_known_hosts_add(
	struct thread_link *link)
{
	static struct kl_mutex known_hosts_lock = KL_MUTEX_INIT;
	struct ssh_buf line;
	char path[MIKUTERM_PATH_MAX];
	char name[MIKUTERM_HOST_MAX + 8U];
	char text[MIKUTERM_HOST_MAX + 128U];
	int error;

	/* The line. */
	ssh_buf_init(&line, 1024U, 0);
	error = ssh_known_hosts_put_line(link->host_name, link->config.port, link->host_key, &line);
	if (error != 0) {
		ssh_buf_free(&line);
		return error;
	}

	/* The file, written by one thread of the program at a time. */
	error = mikuterm_ssh_path(path, sizeof(path), link->config.user_known_hosts);
	if (error == 0) {
		kl_mutex_lock(&known_hosts_lock);

		error = mikuterm_file_append(path, line.data, line.size);

		kl_mutex_unlock(&known_hosts_lock);
	}
	ssh_buf_free(&line);

	/* The host as known_hosts names it: [host]:port for a port other than 22. */
	if (link->config.port == 22U)
		(void)snprintf(name, sizeof(name), "%s", link->host_name);
	else
		(void)snprintf(name, sizeof(name), "[%s]:%u", link->host_name, (unsigned)link->config.port);

	/* What became of it. */
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "Warning: the key of '%s' could not be kept (%s).\r\n", name, thread_error_text(error));
		session_say(link->session, text);
		return error;
	}

	/* Succeeded: kept. */
	(void)snprintf(text, sizeof(text), "Warning: Permanently added '%s' (ED25519) to the list of known hosts.\r\n", name);
	session_say(link->session, text);
	return 0;
}

/* Notes how the connection ended, and why in the user's words. */
static void
thread_closed(
	struct thread_link *link,
	const struct ssh_engine_event *event)
{
	/* The end. */
	link->closed = 1;
	link->error = event->error;

	/* Why, for a failure. */
	switch (event->error) {
	case 0:
		break;
	case ECONNABORTED:
		(void)snprintf(link->reason, sizeof(link->reason), "Disconnected by the server: %s", event->reason);
		break;
	case ECONNRESET:
		(void)snprintf(link->reason, sizeof(link->reason), "Connection to %s closed by the remote host.", link->host_name);
		break;
	case EACCES:
		if (link->host_refused)
			(void)snprintf(link->reason, sizeof(link->reason), "Host key verification failed.");
		else
			(void)snprintf(link->reason, sizeof(link->reason), "%s@%s: Permission denied (%s).", link->config.user, link->host_name, event->reason);
		break;
	case ETIMEDOUT:
		(void)snprintf(link->reason, sizeof(link->reason), "Timeout, server %s not responding.", link->host_name);
		break;
	default:
		(void)snprintf(link->reason, sizeof(link->reason), "The connection failed: %s (%s).", event->reason, thread_error_text(event->error));
		break;
	}
}

/*
 * Takes what the tab has for the connection: how much it read (the
 * window goes back), the grid's size, the keys typed, and its close
 * (DISCONNECT, then a short wait for it to leave).
 */
static void
thread_from_tab(
	struct thread_link *link)
{
	struct mikuterm_session *session;
	size_t taken;
	size_t written;
	unsigned columns;
	unsigned rows;
	unsigned width;
	unsigned height;
	int resized;
	int room;
	int closing;

	/* What the tab left. */
	session = link->session;
	kl_mutex_lock(&session->lock);

	taken = session->taken;
	session->taken = 0U;
	resized = session->resized;
	session->resized = 0;
	columns = session->columns;
	rows = session->rows;
	width = session->width;
	height = session->height;
	closing = session->closing;

	/* The keys, as many as the session takes now (none before it starts); the rest stay. */
	room = 0;
	written = 0U;
	if (session->input_length != 0U && !closing)
		(void)ssh_engine_write(link->engine, session->input, session->input_length, &written);
	if (written != 0U) {
		memmove(session->input, session->input + written, session->input_length - written);
		session->input_length -= written;
		if (session->input_full) {
			session->input_full = 0;
			room = 1;
		}
	}

	kl_mutex_unlock(&session->lock);

	/* The window, as the tab read. */
	if (taken != 0U)
		ssh_engine_consumed(link->engine, taken);

	/* The size. */
	if (resized)
		(void)ssh_engine_resize(link->engine, columns, rows, width, height);

	/* The tab may send the keys it kept. */
	if (room)
		session_post(session);

	/* The tab closed: goodbye, once. */
	if (closing && !link->disconnecting) {
		ssh_engine_disconnect(link->engine, "closed by the user");
		link->disconnecting = 1;
		link->farewell_ms = thread_now() + THREAD_FAREWELL_MS;
	}
}

/* Takes the user's answer to the question asked for the engine, if it came. */
static void
thread_answers(
	struct thread_link *link)
{
	char answer[SESSION_LINE_MAX];
	int cancelled;
	int answered;

	/* Nothing asked. */
	if (link->asked == THREAD_ASKED_NONE)
		return;

	/* The answer, when it came. */
	answered = thread_ask_take(link->session, answer, sizeof(answer), &cancelled);
	if (!answered)
		return;

	/* What it answers. */
	if (link->asked == THREAD_ASKED_HOST_KEY) {
		thread_host_answer(link, answer, cancelled);
	} else {
		/* A password, or none more when the user gave up. */
		link->asked = THREAD_ASKED_NONE;
		if (cancelled)
			(void)ssh_engine_password(link->engine, NULL);
		else
			(void)ssh_engine_password(link->engine, answer);
	}

	/* The answer may be a password. */
	OPENSSL_cleanse(answer, sizeof(answer));
}

/* Sends what the engine has to send, as much as the socket takes; returns 0, or the socket's error. */
static int
thread_send(
	struct thread_link *link)
{
	const uint8_t *bytes;
	size_t size;
	size_t sent;
	int error;

	/* Until nothing is left or the socket is full. */
	for (;;) {
		bytes = ssh_engine_output(link->engine, &size);
		if (bytes == NULL || size == 0U)
			return 0;
		error = kl_socket_send(link->socket, bytes, size, &sent);
		if (error == EAGAIN)
			return 0;
		if (error != 0)
			return error;
		ssh_engine_written(link->engine, sent);
	}
}

/* Reads what the socket has (when the bytes before are all taken); the end or a failure is the engine's input end. */
static int
thread_receive(
	struct thread_link *link)
{
	size_t received;
	size_t taken;
	int error;

	/* The bytes read before come first. */
	if (link->received_start != link->received_end)
		return 0;

	/* What came. */
	error = kl_socket_receive(link->socket, link->received, sizeof(link->received), &received);
	if (error == EAGAIN)
		return 0;

	/* The end, or a failure: the engine learns the connection is over. */
	if (error != 0 || received == 0U) {
		(void)ssh_engine_input(link->engine, NULL, 0U, &taken);
		return error;
	}

	/* Succeeded: the bytes wait for the engine. */
	link->received_start = 0U;
	link->received_end = received;
	return 0;
}

/* Gives the engine the bytes read, as many as it takes, taking its events between. */
static void
thread_feed(
	struct thread_link *link)
{
	size_t taken;
	int error;

	/* Until they are all taken, or the engine takes no more before its events are taken. */
	while (link->received_start != link->received_end) {
		error = ssh_engine_input(link->engine, link->received + link->received_start, link->received_end - link->received_start, &taken);
		if (error != 0) {
			link->received_start = link->received_end;
			break;
		}
		link->received_start += taken;
		if (taken == 0U)
			break;
		thread_events(link);
	}
}

/*
 * Asks the user a question in the tab and waits for the answer (the
 * questions before a connection, when no socket needs the thread).
 * Returns 0 with the answer, ECANCELED when the user gave up, or
 * EPIPE when the tab closed.
 */
static int
thread_ask(
	struct mikuterm_session *session,
	const char *prompt,
	unsigned kind,
	char *answer,
	size_t size)
{
	struct kl_socket_wait wait;
	size_t ready;
	int cancelled;
	int answered;
	int closing;
	int error;

	/* The question. */
	thread_ask_start(session, prompt, kind);

	/* The wait for the answer or the tab's close. */
	for (;;) {
		answered = thread_ask_take(session, answer, size, &cancelled);
		if (answered)
			break;

		/* A tab that closed wants no answer. */
		closing = thread_closing(session);
		if (closing)
			return EPIPE;

		/* The waker, rung by a key or the tab's close. */
		wait.socket = NULL;
		wait.waker = session->waker;
		wait.events = KL_SOCKET_READABLE;
		wait.ready = 0U;
		error = kl_socket_poll(&wait, 1U, (int)THREAD_ROUND_MS, &ready);
		if (error != 0)
			return EPIPE;
	}

	/* A tab that closed meanwhile wants no answer. */
	closing = thread_closing(session);
	if (closing)
		return EPIPE;

	/* Reports a question given up. */
	if (cancelled)
		return ECANCELED;

	/* Succeeded: the answer. */
	return 0;
}

/* Asks a question in the tab: the prompt is shown and the keys typed are the answer's line from now on. */
static void
thread_ask_start(
	struct mikuterm_session *session,
	const char *prompt,
	unsigned kind)
{
	/* The prompt and an empty line. */
	kl_mutex_lock(&session->lock);

	(void)session_put_locked(session, prompt, strlen(prompt));
	session->question = kind;
	session->line_length = 0U;
	session->escape = SESSION_ESCAPE_NONE;
	session->answered = 0;
	session->cancelled = 0;

	kl_mutex_unlock(&session->lock);

	/* The tab shows it. */
	session_post(session);
}

/*
 * Takes the answer to the question asked, when it came: its text (cut to
 * size) and whether the user gave up.  Returns 1 when it came, 0 when not
 * yet.
 */
static int
thread_ask_take(
	struct mikuterm_session *session,
	char *answer,
	size_t size,
	int *cancelled)
{
	size_t length;
	int answered;

	/* The line, once answered; the question is over and the line wiped. */
	kl_mutex_lock(&session->lock);

	answered = session->answered;
	*cancelled = session->cancelled;
	if (answered) {
		length = session->line_length;
		if (length >= size)
			length = size - 1U;
		memcpy(answer, session->line, length);
		answer[length] = '\0';
		OPENSSL_cleanse(session->line, sizeof(session->line));
		session->line_length = 0U;
		session->question = SESSION_ASK_NONE;
		session->answered = 0;
		session->cancelled = 0;
	}

	kl_mutex_unlock(&session->lock);

	/* Reports whether it came. */
	return answered;
}

/* Tells whether the tab closed (1) or not (0). */
static int
thread_closing(
	struct mikuterm_session *session)
{
	int closing;

	/* The flag the tab set. */
	kl_mutex_lock(&session->lock);

	closing = session->closing;

	kl_mutex_unlock(&session->lock);

	/* Reports it. */
	return closing;
}

/* Puts bytes in the tab's ring (the session's data, a banner) and tells the tab. */
static void
thread_say_bytes(
	struct mikuterm_session *session,
	const void *bytes,
	size_t size)
{
	/* The bytes. */
	kl_mutex_lock(&session->lock);

	(void)session_put_locked(session, bytes, size);

	kl_mutex_unlock(&session->lock);

	/* The tab reads them. */
	session_post(session);
}

/* Returns a short English text for an error of the network or the files. */
static const char *
thread_error_text(
	int error)
{
	/* The errors a connection meets. */
	switch (error) {
	case ECONNREFUSED:
		return "Connection refused";
	case ETIMEDOUT:
		return "Connection timed out";
	case ENETUNREACH:
		return "Network is unreachable";
	case EHOSTUNREACH:
		return "No route to host";
	case ECONNRESET:
		return "Connection reset by peer";
	case ENOENT:
		return "Name or service not known";
	case EAGAIN:
		return "Temporary failure in name resolution";
	case EACCES:
		return "Permission denied";
	case ENOMEM:
		return "Out of memory";
	case EFBIG:
		return "File too large";
	case EINVAL:
		return "Invalid argument";
	default:
		return "I/O error";
	}
}

/* Returns the time of the monotonic clock in milliseconds. */
static uint64_t
thread_now(void)
{
	/* The clock's microseconds, in milliseconds. */
	return kl_clock_us() / 1000U;
}
