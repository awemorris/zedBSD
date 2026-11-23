/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A tab's SSH connection on a thread of its own (session.h): it chooses
 * the host (the Connect host, or the one the user takes on the launch
 * screen of launch.c: a host of ssh\config, one typed, or a distribution
 * of WSL, whose sshd it reaches like any host), reads ssh\config for it,
 * asks for the user when none is set ("login as:"), connects, and runs
 * zedBSD's SSH client engine on the socket until the connection ends.
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
#include "forward.h"
#include "launch.h"
#include "remote-command.h"
#include "x11-host.h"
#include "mikuterm-version.h"

#include "userland/base/ssh/ssh-client-engine.h"
#include "userland/base/ssh/ssh-crypto.h"
#include "userland/base/ssh/ssh-key.h"

#include <compat/crypto/crypto.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

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

/* The waits of one round: the socket, the waker, the X server's link, and the forwardings' sockets. */
#define THREAD_WAITS		(3U + FORWARD_WAITS_MAX)

/* The longest local user's name, home folder and computer's name for RemoteCommand's tokens, in UTF-8 with the NUL. */
#define THREAD_LOCAL_NAME_MAX	(MIKUTERM_PATH_MAX)

/* The longest wait of one round when nothing is due, and how long DISCONNECT may take to leave, in milliseconds. */
#define THREAD_ROUND_MS		1000U
#define THREAD_FAREWELL_MS	1000U

/* Where the settings of the hosts are, inside the folder. */
#define THREAD_SSH_CONFIG	"ssh/config"

/* The launch screen's title: the name (with the Greek mu, in UTF-8) and the version. */
#define THREAD_TITLE		"Mik\xce\xbcTerm " MIKUTERM_VERSION_TEXT

/*
 * WSL: how long wsl.exe may take to list the distributions and to start
 * one (milliseconds), the start of an ssh\config Host for a distribution,
 * and the host a distribution without one is reached on.
 */
#define THREAD_WSL_LIST_MS	15000U
#define THREAD_WSL_START_MS	60000U
#define THREAD_WSL_PREFIX	"wsl-"
#define THREAD_WSL_HOST		"localhost"

/* The largest text wsl.exe writes that is read, and the screen's text at its largest. */
#define THREAD_WSL_OUTPUT	16384U
#define THREAD_SCREEN_MAX	65536U

/* The known_hosts file that stands for none: nothing is read from it and nothing kept in it. */
#define THREAD_NO_FILE		"/dev/null"

/* The longest line about the hosts' keys shown at once (a path, a name and the words round them). */
#define THREAD_LINE_MAX		(MIKUTERM_PATH_MAX + MIKUTERM_HOST_MAX + 128U)

/* The frame of the warnings about a host's key. */
#define THREAD_WARNING_RULE	"@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"

/* What an answer about an unknown host's key says. */
#define THREAD_CONFIRM_NO	0
#define THREAD_CONFIRM_YES	1
#define THREAD_CONFIRM_AGAIN	2

/* What an attempt to connect came to. */
#define THREAD_DONE		0
#define THREAD_FAILED		1
#define THREAD_CLOSING		2
#define THREAD_GIVEN_UP		3

/* What the engine asked and the user has not answered yet. */
#define THREAD_ASKED_NONE	0U
#define THREAD_ASKED_HOST_KEY	1U
#define THREAD_ASKED_PASSWORD	2U
#define THREAD_ASKED_PASSPHRASE	3U

/*
 * What the user chose to connect to: the name given (a Host of ssh\config,
 * or a host's name or address), and the user and port given with it
 * (user@host:port; empty and 0 when not given), the tab's title, and the
 * WSL distribution it is (empty: none).
 */
struct thread_target {
	char host[MIKUTERM_HOST_MAX];
	char user[SSH_USER_MAX + 1];
	uint16_t port;
	char title[MIKUTERM_HOST_MAX];
	char distribution[LAUNCH_NAME_MAX];
};

/*
 * The hosts ssh\config has settings for, as the launch screen lists them:
 * their names in the order written, each once, and the User each one's
 * settings give (empty: none).
 */
struct thread_hosts {
	char names[THREAD_HOSTS][THREAD_NAME_MAX];
	char users[THREAD_HOSTS][SSH_USER_MAX + 1];
	unsigned count;
};

/*
 * The launch screen's state while the user chooses: the session, the
 * hosts, the screens of the hosts and of WSL's distributions, the
 * distributions' names, what wsl.exe wrote and its text, and the
 * screen's text as it is drawn.
 */
struct thread_choice {
	struct mikuterm_session *session;
	struct thread_hosts hosts;
	struct launch_screen screen;
	struct launch_screen wsl;
	char names[LAUNCH_NAMES_MAX][LAUNCH_NAME_MAX];
	unsigned char output[THREAD_WSL_OUTPUT];
	char decoded[THREAD_WSL_OUTPUT * 2U];
	char text[THREAD_SCREEN_MAX];
};

/*
 * One attempt to connect: the settings for the host, the socket and the
 * address it reached, the engine, the bytes read and not yet taken by the
 * engine, the server's key with what known_hosts says of it, the question
 * waiting for the user (for a passphrase, whose key), the paths of the key
 * files in the engine's order and whether a passphrase kept for the
 * Windows session was tried for each, and how the attempt ended.
 * again is 1 for an attempt after a failed one.  command is
 * RemoteCommand with its tokens put in (empty: the shell).  forwards
 * holds the port forwardings (NULL when the host has none), and
 * forward_failed is 1 when ExitOnForwardFailure ended the connection
 * (reason says why).
 */
struct thread_link {
	struct mikuterm_session *session;
	struct thread_target *target;
	struct ssh_config config;
	const char *host_name;
	struct kl_socket *socket;
	struct kl_address address;
	struct ssh_engine *engine;
	uint8_t received[THREAD_RECEIVE];
	size_t received_start;
	size_t received_end;
	unsigned asked;
	unsigned asked_identity;
	char identity_paths[SSH_ENGINE_IDENTITIES_MAX][MIKUTERM_PATH_MAX];
	int recalled[SSH_ENGINE_IDENTITIES_MAX];
	uint8_t host_key[SSH_ED25519_PUBLIC];
	char fingerprint[SSH_KEY_FINGERPRINT_SIZE];
	char known_name[MIKUTERM_HOST_MAX + 8U];
	struct mikuterm_known_scan known;
	int host_refused;
	int disconnecting;
	uint64_t farewell_ms;
	int closed;
	int error;
	char reason[THREAD_REASON_MAX];
	int x11;
	int x11_refused;
	uint64_t x11_since_ms;
	char x11_cookie[SSH_ENGINE_X11_COOKIE_DIGITS + 1];
	struct x11_host x11_host;
	int again;
	struct ssh_buf command;
	struct forward_state *forwards;
	int forward_failed;
};

static int thread_choose(struct mikuterm_session *session, struct thread_target *target);
static void thread_hosts_read(struct thread_hosts *hosts);
static int thread_launch(struct thread_choice *choice, struct launch_screen *screen, unsigned *event);
static void thread_draw(struct thread_choice *choice, const struct launch_screen *screen);
static int thread_wsl(struct thread_choice *choice, struct thread_target *target);
static int thread_wsl_list(struct thread_choice *choice, unsigned *count);
static int thread_wsl_host(const struct thread_choice *choice, const char *name, char *host, size_t size);
static int thread_wsl_target(struct thread_choice *choice, const char *name, struct thread_target *target);
static int thread_same_name(const char *first, const char *second);
static int thread_user_name(const char *name);
static int thread_stop(void *data);
static int thread_host_named(void *data, const char *name, size_t size);
static int thread_parse_target(const char *text, struct thread_target *target);
static int thread_attempt(struct thread_link *link);
static int thread_settings(struct thread_link *link);
static int thread_dial(struct thread_link *link);
static int thread_wait_connected(struct thread_link *link, struct kl_socket *socket, unsigned timeout);
static int thread_identities(struct thread_link *link, struct ssh_engine_identity *identities, unsigned *count);
static void thread_identities_free(struct ssh_engine_identity *identities, unsigned count);
static int thread_converse(struct thread_link *link);
static void thread_events(struct thread_link *link);
static void thread_host_key(struct thread_link *link, const struct ssh_engine_event *event);
static enum ssh_known_result thread_known_read(struct thread_link *link, const char *written);
static void thread_host_ask(struct thread_link *link, enum ssh_known_result user_answer, enum ssh_known_result global_answer);
static void thread_host_changed(struct thread_link *link);
static void thread_host_revoked(struct thread_link *link);
static void thread_host_answer(struct thread_link *link, const char *answer, int cancelled);
static int thread_confirm(const char *answer, const char *fingerprint);
static int thread_known_hosts_add(struct thread_link *link);
static int thread_known_hosts_write(struct thread_link *link, const char *path);
static void thread_closed(struct thread_link *link, const struct ssh_engine_event *event);
static void thread_from_tab(struct thread_link *link);
static void thread_answers(struct thread_link *link);
static int thread_send(struct thread_link *link);
static int thread_receive(struct thread_link *link);
static void thread_feed(struct thread_link *link);
static int thread_ask(struct mikuterm_session *session, const char *prompt, unsigned kind, char *answer, size_t size);
static void thread_ask_start(struct mikuterm_session *session, const char *prompt, unsigned kind);
static int thread_ask_take(struct mikuterm_session *session, char *answer, size_t size, int *cancelled, int *keep);
static void thread_passphrase(struct thread_link *link, const struct ssh_engine_event *event);
static void thread_passphrase_answer(struct thread_link *link, const char *answer, int cancelled, int keep);
static int thread_closing(struct mikuterm_session *session);
static void thread_say_bytes(struct mikuterm_session *session, const void *bytes, size_t size);
static const char *thread_error_text(int error);
static uint64_t thread_now(void);
static int thread_x11_start(struct thread_link *link);
static void thread_x11_open(struct thread_link *link, const struct ssh_engine_event *event);
static int thread_remote_command(struct thread_link *link);
static void thread_local_names(struct remote_command_tokens *tokens, char *local_user, char *home, char *local_host, size_t size);
#ifdef _WIN32
static int thread_local_name(const wchar_t *wide, char *name, size_t size);
#endif
static int thread_forwards_open(struct thread_link *link);
static void thread_forwards_close(struct thread_link *link);
static void thread_forward_event(struct thread_link *link, const struct ssh_engine_event *event, int *taken);
static void thread_forward_start(struct thread_link *link);
static void thread_forward_fail(struct thread_link *link);
static void thread_forward_say(void *data, const char *line);

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
	char hint[LAUNCH_NAME_MAX + 96U];
	int again;
	int status;

	/* The session (the settings' problems are said when the host is chosen). */
	session = argument;

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
	again = 0;
	while (status == 0) {
		/* One attempt; one after a failed one may find a forwarded port still held by the last. */
		memset(link, 0, sizeof(*link));
		link->session = session;
		link->target = &target;
		link->again = again;
		status = thread_attempt(link);
		again = 1;

		/* A normal end, or the user giving up, ends the tab. */
		if (status == THREAD_DONE || status == THREAD_GIVEN_UP) {
			session_ended(session);
			break;
		}

		/* A tab that closed wants nothing more. */
		if (status == THREAD_CLOSING)
			break;

		/* Why it failed, on a line of its own (for WSL, what it needs), and Enter to try again. */
		session_say_line(session, link->reason);
		session_say(session, "\r\n");
		if (target.distribution[0] != '\0') {
			(void)snprintf(hint, sizeof(hint), "%s needs an sshd listening on port %u (README.txt, WSL).\r\n", target.distribution, (unsigned)link->config.port);
			session_say(session, hint);
		}

		/* Enter tries again; Ctrl+C closes the tab. */
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
 * what the user takes on the launch screen: a host of ssh\config, a host
 * typed on the ">>> " line (a name, or user@host:port), or a
 * distribution of WSL.  Settings opens the settings' tab beside this one
 * and the screen stays.  Returns 0, ECANCELED when the user gave up, or
 * EPIPE when the tab closed.
 */
static int
thread_choose(
	struct mikuterm_session *session,
	struct thread_target *target)
{
	struct thread_choice *choice;
	char key[LAUNCH_KEY_MAX];
	char label[LAUNCH_LABEL_MAX];
	char config[sizeof(THREAD_SSH_CONFIG)];
	char status[LAUNCH_STATUS_MAX];
	unsigned event;
	unsigned index;
	size_t length;
	int taken;
	int error;

	/* The host given; the settings' problems are said first. */
	memset(target, 0, sizeof(*target));
	if (session->host[0] != '\0') {
		error = thread_parse_target(session->host, target);
		if (error == 0) {
			if (session->notice[0] != '\0')
				session_say(session, session->notice);
			(void)snprintf(target->title, sizeof(target->title), "%s", target->host);
			return 0;
		}
	}

	/* The screen's state is large: it is on the heap. */
	choice = calloc(1U, sizeof(*choice));
	if (choice == NULL)
		return ECANCELED;
	choice->session = session;

	/* The hosts of ssh\config (none when it is not there), each with its User. */
	thread_hosts_read(&choice->hosts);

	/* The screen: the hosts by number, WSL and the settings. */
	launch_init(&choice->screen, "Hosts:");
	for (index = 0U; index < choice->hosts.count; index++) {
		/* user@name when ssh\config gives the user, the name alone when not. */
		(void)snprintf(key, sizeof(key), "%u", index + 1U);
		if (choice->hosts.users[index][0] != '\0')
			(void)snprintf(label, sizeof(label), "%s@%s", choice->hosts.users[index], choice->hosts.names[index]);
		else
			(void)snprintf(label, sizeof(label), "%s", choice->hosts.names[index]);
		(void)launch_add(&choice->screen, key, LAUNCH_HOST, label, NULL);
	}

	/* WSL and the settings after the hosts; with no host, how to type one. */
	(void)launch_add(&choice->screen, "w", LAUNCH_WSL, "WSL", NULL);
	(void)launch_add(&choice->screen, "x", LAUNCH_SETTINGS, "Settings", NULL);
	if (choice->hosts.count == 0U) {
		mikuterm_path_shown(THREAD_SSH_CONFIG, config, sizeof(config));
		(void)snprintf(status, sizeof(status), "%s names no host: type one as user@host:port.", config);
		launch_say(&choice->screen, status);
	}

	/* The user's choice, until one is a host to connect to. */
	error = 0;
	for (;;) {
		/* The keys until Enter, Escape, or the user giving up. */
		error = thread_launch(choice, &choice->screen, &event);
		if (error != 0)
			break;
		if (event == LAUNCH_EVENT_CANCEL) {
			error = ECANCELED;
			break;
		}

		/* Escape puts the chosen row back on the line. */
		if (event == LAUNCH_EVENT_ESCAPE) {
			launch_choose(&choice->screen, choice->screen.chosen);
			continue;
		}

		/* What Enter took: nothing, a host typed, or a row. */
		launch_say(&choice->screen, NULL);
		taken = launch_row_taken(&choice->screen);
		if (taken == -2)
			continue;
		if (taken == -1) {
			/* A host typed: user@host:port, or a number of the list. */
			error = thread_parse_target(choice->screen.line, target);
			if (error == 0) {
				(void)snprintf(target->title, sizeof(target->title), "%s", target->host);
				break;
			}

			/* Not a host: the screen says so and asks again. */
			launch_say(&choice->screen, "That is not a host: type a name, or user@host:port.");
			error = 0;
			continue;
		}

		/* A host of ssh\config: its name, the user and port its settings give. */
		index = (unsigned)taken;
		if (choice->screen.rows[index].kind == LAUNCH_HOST) {
			length = strlen(choice->hosts.names[index]);
			memcpy(target->host, choice->hosts.names[index], length + 1U);
			(void)snprintf(target->title, sizeof(target->title), "%s", target->host);
			break;
		}

		/* Settings: their tab opens beside this one, which stays on the screen. */
		if (choice->screen.rows[index].kind == LAUNCH_SETTINGS) {
			session_settings(session);
			launch_choose(&choice->screen, index);
			continue;
		}

		/* WSL: a distribution, or back to the hosts with why not. */
		error = thread_wsl(choice, target);
		if (error == EAGAIN) {
			error = 0;
			continue;
		}

		/* A distribution, or the user giving up, or the tab closing. */
		break;
	}

	/* The screen goes before the connection's own lines, unless the tab closed. */
	if (error != EPIPE) {
		length = launch_leave(choice->text, sizeof(choice->text));
		thread_say_bytes(session, choice->text, length);
	}

	/* The screen's state goes. */
	free(choice);

	/* Reports the user giving up, or the tab closing. */
	if (error != 0)
		return error;

	/* Succeeded: the target is chosen. */
	return 0;
}

/*
 * Reads the hosts ssh\config has settings for: their names as the list
 * shows them, each with the User its settings give (empty when none).
 * An ssh\config that is not there, or cannot be read, has none.
 */
static void
thread_hosts_read(
	struct thread_hosts *hosts)
{
	struct ssh_config *config;
	char path[MIKUTERM_PATH_MAX];
	char error_text[SSH_CONFIG_ERROR_MAX];
	char *text;
	size_t size;
	unsigned index;
	int error;

	/* The file. */
	hosts->count = 0U;
	text = NULL;
	size = 0U;
	error = mikuterm_path(path, sizeof(path), THREAD_SSH_CONFIG);
	if (error == 0)
		error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error != 0)
		return;

	/* The names of its Host lines. */
	(void)ssh_config_hosts(text, size, thread_host_named, hosts, NULL);

	/* Each name's User, from its settings as a connection reads them. */
	config = malloc(sizeof(*config));
	if (config != NULL) {
		for (index = 0U; index < hosts->count; index++) {
			/* The settings for the name; a file the reader refuses gives no User. */
			ssh_config_init(config);
			error = ssh_config_parse(config, text, size, hosts->names[index], error_text);
			if (error == 0)
				(void)snprintf(hosts->users[index], sizeof(hosts->users[index]), "%s", config->user);
		}

		/* The settings read go. */
		free(config);
	}

	/* The file's text goes. */
	free(text);
}

/*
 * Shows a screen and takes the tab's keys for it until one ends the line
 * (*event: Enter, Escape, or the user giving up); the screen is drawn
 * again after keys and when the grid's size changes.  Returns 0, or
 * EPIPE when the tab closed.
 */
static int
thread_launch(
	struct thread_choice *choice,
	struct launch_screen *screen,
	unsigned *event)
{
	struct mikuterm_session *session;
	struct kl_socket_wait wait;
	size_t ready;
	size_t used;
	unsigned columns;
	unsigned rows;
	unsigned drawn_columns;
	unsigned drawn_rows;
	int changed;
	int room;
	int closing;
	int error;

	/* The screen drawn and the keys taken round by round, nothing drawn yet. */
	session = choice->session;
	drawn_columns = 0U;
	drawn_rows = 0U;
	changed = 1;
	*event = LAUNCH_EVENT_NONE;
	for (;;) {
		/* The keys typed so far, as far as one ends the line (the rest wait for the next screen), and the grid's size. */
		kl_mutex_lock(&session->lock);

		room = 0;
		if (session->input_length != 0U) {
			*event = launch_keys(screen, session->input, session->input_length, &used);
			memmove(session->input, session->input + used, session->input_length - used);
			session->input_length -= used;
			changed = 1;
			if (session->input_full) {
				session->input_full = 0;
				room = 1;
			}
		}

		/* The grid's size and the tab's close. */
		columns = session->columns;
		rows = session->rows;
		closing = session->closing;

		kl_mutex_unlock(&session->lock);

		/* A tab that closed wants nothing more. */
		if (closing)
			return EPIPE;

		/* The tab may send the keys it kept. */
		if (room)
			session_post(session);

		/* A key that ended the line. */
		if (*event != LAUNCH_EVENT_NONE)
			return 0;

		/* The screen as it is now, when it changed or the grid did. */
		if (columns != drawn_columns || rows != drawn_rows)
			changed = 1;
		if (changed) {
			thread_draw(choice, screen);
			drawn_columns = columns;
			drawn_rows = rows;
			changed = 0;
		}

		/* The waker, rung by a key, the grid's size or the tab's close. */
		wait.socket = NULL;
		wait.waker = session->waker;
		wait.events = KL_SOCKET_READABLE;
		wait.ready = 0U;
		error = kl_socket_poll(&wait, 1U, (int)THREAD_ROUND_MS, &ready);
		if (error != 0)
			return EPIPE;
	}
}

/* Draws a screen in the tab at the grid's size, the settings' problems (the first tab's) over its rows. */
static void
thread_draw(
	struct thread_choice *choice,
	const struct launch_screen *screen)
{
	unsigned columns;
	unsigned rows;
	size_t length;

	/* The grid's size now. */
	kl_mutex_lock(&choice->session->lock);

	columns = choice->session->columns;
	rows = choice->session->rows;

	kl_mutex_unlock(&choice->session->lock);

	/* The whole screen at once. */
	length = launch_draw(screen, THREAD_TITLE, choice->session->notice, columns, rows, choice->text, sizeof(choice->text));
	thread_say_bytes(choice->session, choice->text, length);
}

/*
 * Asks wsl.exe for WSL's distributions and lets the user take one: the
 * target is then its ssh\config Host (wsl-<name>) or localhost on
 * WslPort.  Returns 0 with the target, EAGAIN to go back to the hosts
 * (the screen's message says why when WSL could not list them),
 * ECANCELED when the user gave up, or EPIPE when the tab closed.
 */
static int
thread_wsl(
	struct thread_choice *choice,
	struct thread_target *target)
{
	struct launch_screen *screen;
	char key[LAUNCH_KEY_MAX];
	char note[LAUNCH_NOTE_MAX];
	char found[THREAD_NAME_MAX];
	char config[sizeof(THREAD_SSH_CONFIG)];
	unsigned event;
	unsigned count;
	unsigned index;
	int taken;
	int same;
	int error;

	/* The list, while the screen says it is being asked for. */
	launch_say(&choice->screen, "Asking WSL for its distributions ...");
	thread_draw(choice, &choice->screen);
	count = 0U;
	error = thread_wsl_list(choice, &count);
	if (error != 0)
		return error;

	/* The list is read: the message goes. */
	launch_say(&choice->screen, NULL);

	/* The screen: the distributions by number, each with where it is reached, and the way back. */
	screen = &choice->wsl;
	launch_init(screen, "WSL distributions:");
	mikuterm_path_shown(THREAD_SSH_CONFIG, config, sizeof(config));
	for (index = 0U; index < count; index++) {
		/* The Host of ssh\config for it, or localhost on WslPort. */
		(void)snprintf(key, sizeof(key), "%u", index + 1U);
		error = thread_wsl_host(choice, choice->names[index], found, sizeof(found));
		if (error == 0)
			(void)snprintf(note, sizeof(note), "(%s: %s)", config, found);
		else
			(void)snprintf(note, sizeof(note), "(localhost:%u)", choice->session->wsl_port);
		(void)launch_add(screen, key, LAUNCH_DISTRIBUTION, choice->names[index], note);
	}

	/* The way back. */
	(void)launch_add(screen, "b", LAUNCH_BACK, "Back", NULL);

	/* The user's choice, until it is a distribution or back. */
	for (;;) {
		/* The keys until Enter, Escape, or the user giving up. */
		error = thread_launch(choice, screen, &event);
		if (error != 0)
			return error;
		if (event == LAUNCH_EVENT_CANCEL)
			return ECANCELED;
		if (event == LAUNCH_EVENT_ESCAPE)
			return EAGAIN;

		/* What Enter took: nothing, a name typed, or a row. */
		launch_say(screen, NULL);
		taken = launch_row_taken(screen);
		if (taken == -2)
			continue;
		if (taken == -1) {
			/* A distribution's name typed, in any case. */
			for (index = 0U; index < count; index++) {
				/* The same name. */
				same = thread_same_name(choice->names[index], screen->line);
				if (same)
					break;
			}

			/* A name WSL did not list is said; one it did is its row. */
			if (index == count) {
				launch_say(screen, "WSL has no such distribution.");
				continue;
			}

			/* The row of the name. */
			taken = (int)index;
		}

		/* Back to the hosts. */
		if (screen->rows[taken].kind == LAUNCH_BACK)
			return EAGAIN;

		/* The distribution: started, its user, and where it is reached. */
		error = thread_wsl_target(choice, choice->names[taken], target);
		if (error != 0)
			return error;

		/* Succeeded: the distribution is the target. */
		return 0;
	}
}

/*
 * Runs "wsl.exe -l -q" and reads the distributions into choice->names
 * (*count of them).  Returns 0, EAGAIN with the screen's message saying
 * why there are none (no WSL, no distribution, wsl.exe's own error), or
 * EPIPE when the tab closed meanwhile.
 */
static int
thread_wsl_list(
	struct thread_choice *choice,
	unsigned *count)
{
	char line[LAUNCH_STATUS_MAX - 32U];
	size_t size;
	unsigned long code;
	int error;

	/* What wsl.exe says. */
	*count = 0U;
	error = mikuterm_wsl_run("-l -q", THREAD_WSL_LIST_MS, thread_stop, choice->session, choice->output, sizeof(choice->output), &size, &code);
	if (error == ECANCELED)
		return EPIPE;

	/* Why it did not answer. */
	if (error == ENOENT) {
		launch_say(&choice->screen, "WSL is not installed: Windows has no wsl.exe.");
		return EAGAIN;
	}

	/* A wsl.exe that did not end in time (it was ended). */
	if (error == ETIMEDOUT) {
		launch_say(&choice->screen, "wsl.exe did not answer in time.");
		return EAGAIN;
	}

	/* A wsl.exe that could not be started. */
	if (error != 0) {
		launch_say(&choice->screen, "wsl.exe could not be run.");
		return EAGAIN;
	}

	/* Its text; a failure says why in its first line. */
	launch_output_text(choice->output, size, choice->decoded, sizeof(choice->decoded));
	if (code != 0UL) {
		error = launch_first_line(choice->decoded, line, sizeof(line));
		if (error == 0)
			(void)snprintf(choice->screen.status, sizeof(choice->screen.status), "WSL: %s", line);
		else
			(void)snprintf(choice->screen.status, sizeof(choice->screen.status), "wsl.exe failed (exit code 0x%lx).", code);
		return EAGAIN;
	}

	/* The names, one a line; none is said. */
	*count = launch_names(choice->decoded, choice->names, LAUNCH_NAMES_MAX);
	if (*count == 0U) {
		launch_say(&choice->screen, "WSL has no distribution: install one (wsl --install).");
		return EAGAIN;
	}

	/* Succeeded: the distributions. */
	return 0;
}

/*
 * Finds the Host of ssh\config for a distribution: wsl-<name>, in any
 * case, as ssh\config writes it.  Returns 0 with it in host, or ENOENT.
 */
static int
thread_wsl_host(
	const struct thread_choice *choice,
	const char *name,
	char *host,
	size_t size)
{
	char wanted[THREAD_NAME_MAX];
	unsigned index;
	int same;

	/* The name the Host would have. */
	(void)snprintf(wanted, sizeof(wanted), "%s%s", THREAD_WSL_PREFIX, name);

	/* A Host with that name. */
	for (index = 0U; index < choice->hosts.count; index++) {
		/* The same name, in any case: the Host as written. */
		same = thread_same_name(choice->hosts.names[index], wanted);
		if (same) {
			(void)snprintf(host, size, "%s", choice->hosts.names[index]);
			return 0;
		}
	}

	/* No Host for it. */
	return ENOENT;
}

/*
 * Makes the target of a distribution: its Host of ssh\config (with that
 * Host's settings), or localhost on WslPort.  The distribution is started
 * first ("wsl.exe -d name -e whoami", so an sshd systemd starts is up),
 * and the user it answers with is the one logged in as when ssh\config
 * gives none.  Returns 0, or EPIPE when the tab closed.
 */
static int
thread_wsl_target(
	struct thread_choice *choice,
	const char *name,
	struct thread_target *target)
{
	char arguments[LAUNCH_NAME_MAX + 32U];
	char user[SSH_USER_MAX + 1];
	char status[LAUNCH_STATUS_MAX];
	size_t size;
	unsigned long code;
	unsigned index;
	int known;
	int same;
	int error;

	/* The distribution started, while the screen says so. */
	(void)snprintf(status, sizeof(status), "Starting %s ...", name);
	launch_say(&choice->wsl, status);
	thread_draw(choice, &choice->wsl);
	(void)snprintf(arguments, sizeof(arguments), "-d \"%s\" -e whoami", name);
	error = mikuterm_wsl_run(arguments, THREAD_WSL_START_MS, thread_stop, choice->session, choice->output, sizeof(choice->output), &size, &code);
	if (error == ECANCELED)
		return EPIPE;

	/* The user it answered with, when it did (else "login as:" asks). */
	user[0] = '\0';
	if (error == 0 && code == 0UL) {
		launch_output_text(choice->output, size, choice->decoded, sizeof(choice->decoded));
		error = launch_first_line(choice->decoded, user, sizeof(user));
		known = 0;
		if (error == 0)
			known = thread_user_name(user);
		if (!known)
			user[0] = '\0';
	}

	/* The Host of ssh\config for it, whose User wins over the one it answered with. */
	memset(target, 0, sizeof(*target));
	error = thread_wsl_host(choice, name, target->host, sizeof(target->host));
	if (error == 0) {
		for (index = 0U; index < choice->hosts.count; index++) {
			/* The Host's own User. */
			same = strcmp(choice->hosts.names[index], target->host);
			if (same == 0 && choice->hosts.users[index][0] != '\0')
				user[0] = '\0';
		}
	} else {
		/* Else localhost, on WSL's sshd port. */
		(void)snprintf(target->host, sizeof(target->host), "%s", THREAD_WSL_HOST);
		target->port = (uint16_t)choice->session->wsl_port;
	}

	/* The user, the title and the distribution. */
	(void)snprintf(target->user, sizeof(target->user), "%s", user);
	(void)snprintf(target->title, sizeof(target->title), "%s", name);
	(void)snprintf(target->distribution, sizeof(target->distribution), "%s", name);

	/* Succeeded: the distribution is the target. */
	return 0;
}

/* Tells whether two names are the same in any case of the ASCII letters (1) or not (0). */
static int
thread_same_name(
	const char *first,
	const char *second)
{
	size_t index;
	int a;
	int b;

	/* Each character, the letters folded. */
	for (index = 0U; first[index] != '\0' || second[index] != '\0'; index++) {
		/* The two characters, small. */
		a = (unsigned char)first[index];
		b = (unsigned char)second[index];
		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return 0;
	}

	/* The same. */
	return 1;
}

/*
 * Tells whether a text is a user's name a login takes (1) or not (0):
 * letters, digits, '.', '_', '-' and '$', not starting with '-'.
 */
static int
thread_user_name(
	const char *name)
{
	size_t index;
	char character;

	/* Not empty, not an option. */
	if (name[0] == '\0' || name[0] == '-')
		return 0;

	/* Each character one a name has. */
	for (index = 0U; name[index] != '\0'; index++) {
		/* A letter, a digit or one of the marks. */
		character = name[index];
		if (character >= 'a' && character <= 'z')
			continue;
		if (character >= 'A' && character <= 'Z')
			continue;
		if (character >= '0' && character <= '9')
			continue;
		if (character == '.' ||
		    character == '_' ||
		    character == '-' ||
		    character == '$')
			continue;

		/* Any other character is not a name's. */
		return 0;
	}

	/* A name. */
	return 1;
}

/* Tells wsl.c whether to give up waiting: the tab closed (1) or not (0). */
static int
thread_stop(
	void *data)
{
	int closing;

	/* The tab's close. */
	closing = thread_closing(data);

	/* Reports it. */
	return closing;
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
		if (colon[1] == '\0' ||
		    *end != '\0' ||
		    port == 0UL ||
		    port > 65535UL)
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

	/* The user, asked until one is given when none is set (kept for the next attempt). */
	while (link->config.user[0] == '\0') {
		error = thread_ask(session, "login as: ", SESSION_ASK_LINE, link->target->user, sizeof(link->target->user));
		if (error == ECANCELED)
			return THREAD_GIVEN_UP;
		if (error != 0)
			return THREAD_CLOSING;
		(void)snprintf(link->config.user, sizeof(link->config.user), "%s", link->target->user);
	}

	/* RemoteCommand with its tokens put in, before the connection as OpenSSH's ssh does it. */
	ssh_buf_init(&link->command, SSH_CHANNEL_COMMAND_MAX, 0);
	error = thread_remote_command(link);
	if (error != 0) {
		ssh_buf_free(&link->command);
		return THREAD_FAILED;
	}

	/* The tab's title is the host, and where it goes is said. */
	(void)snprintf(line, sizeof(line), "\033]2;%s\007Connecting to %s port %u ...\r\n", link->target->title, link->host_name, (unsigned)link->config.port);
	session_say(session, line);

	/* The connection. */
	status = thread_dial(link);
	if (status != THREAD_DONE) {
		ssh_buf_free(&link->command);
		return status;
	}

	/* The user's keys. */
	count = 0U;
	error = thread_identities(link, identities, &count);
	if (error != 0) {
		kl_socket_close(link->socket);
		ssh_buf_free(&link->command);
		(void)snprintf(link->reason, sizeof(link->reason), "The keys could not be read (%s).", thread_error_text(error));
		return THREAD_FAILED;
	}

	/* The engine: a shell, or RemoteCommand, on a terminal of the tab's size (the tab is a terminal either way). */
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
	options.command_size = 0U;
	if (link->command.size != 0U) {
		options.command = link->command.data;
		options.command_size = link->command.size;
	}
	options.alive_interval = link->config.server_alive_interval;

	/* X11 forwarding, when the host's settings ask for it: a fake cookie of this connection's. */
	error = thread_x11_start(link);
	if (error == 0 && link->x11) {
		options.x11 = 1;
		options.x11_cookie = link->x11_cookie;
	}
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
	ssh_buf_free(&link->command);
	if (error != 0) {
		kl_socket_close(link->socket);
		(void)snprintf(link->reason, sizeof(link->reason), "The connection could not start (%s).", thread_error_text(error));
		return THREAD_FAILED;
	}

	/* The port forwardings' state, when the host has any (they start once the user is authenticated). */
	error = thread_forwards_open(link);
	if (error != 0) {
		ssh_engine_close(link->engine);
		link->engine = NULL;
		kl_socket_close(link->socket);
		link->socket = NULL;
		(void)snprintf(link->reason, sizeof(link->reason), "The connection could not start (%s).", thread_error_text(error));
		return THREAD_FAILED;
	}

	/* The conversation until the end; the X server and the forwardings end with it. */
	status = thread_converse(link);
	if (link->x11)
		x11_host_end(&link->x11_host, link->engine);
	thread_forwards_close(link);
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
	char config[sizeof(THREAD_SSH_CONFIG)];
	char error_text[SSH_CONFIG_ERROR_MAX];
	char *text;
	size_t size;
	int error;

	/* The file; none is no settings. */
	text = NULL;
	size = 0U;
	mikuterm_path_shown(THREAD_SSH_CONFIG, config, sizeof(config));
	error = mikuterm_path(path, sizeof(path), THREAD_SSH_CONFIG);
	if (error == 0)
		error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error != 0 && error != ENOENT) {
		(void)snprintf(link->reason, sizeof(link->reason), "%s could not be read (%s).", config, thread_error_text(error));
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

	/* A file that cannot be read gives the reason. */
	if (error != 0) {
		(void)snprintf(link->reason, sizeof(link->reason), "%s: %s", config, error_text);
		return THREAD_FAILED;
	}

	/* What the file did not say takes the defaults. */
	ssh_config_defaults(&link->config);

	/* A connection without a session (SessionType none) is refused rather than opened as a shell. */
	if (link->config.session_type_none) {
		(void)snprintf(link->reason, sizeof(link->reason), "%s: SessionType none is not supported", config);
		return THREAD_FAILED;
	}

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
	struct kl_socket *socket;
	size_t count;
	size_t index;
	unsigned timeout;
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

		/* The connection, waited for while it is on its way. */
		error = kl_socket_connect(socket, &addresses[index]);
		if (error == EINPROGRESS)
			error = thread_wait_connected(link, socket, timeout);

		/* A tab that closed wants no connection. */
		if (error == EPIPE) {
			kl_socket_close(socket);
			return THREAD_CLOSING;
		}

		/* Connected. */
		if (error == 0) {
			link->socket = socket;
			link->address = addresses[index];
			return THREAD_DONE;
		}

		/* The next address, on a new socket. */
		kl_socket_close(socket);
	}

	/* No address answered. */
	(void)snprintf(link->reason, sizeof(link->reason), "ssh: connect to host %s port %u: %s", link->host_name, (unsigned)link->config.port, thread_error_text(error));
	return THREAD_FAILED;
}

/*
 * Waits for a connection on its way to end, for the tab's close, or for
 * timeout seconds (0: no limit).  Returns 0 (connected), why it failed,
 * ETIMEDOUT, or EPIPE when the tab closed.
 */
static int
thread_wait_connected(
	struct thread_link *link,
	struct kl_socket *socket,
	unsigned timeout)
{
	struct kl_socket_wait waits[2];
	uint64_t deadline;
	uint64_t now;
	size_t ready;
	int closing;
	int error;

	/* The time it gives up. */
	deadline = thread_now() + (uint64_t)timeout * 1000U;

	/* Round after round until something ends the wait. */
	for (;;) {
		/* The time is up. */
		now = thread_now();
		if (timeout != 0U && now >= deadline)
			return ETIMEDOUT;

		/* The socket's end of the connection, or the waker. */
		waits[0].socket = socket;
		waits[0].waker = NULL;
		waits[0].events = KL_SOCKET_WRITABLE;
		waits[0].ready = 0U;
		waits[1].socket = NULL;
		waits[1].waker = link->session->waker;
		waits[1].events = KL_SOCKET_READABLE;
		waits[1].ready = 0U;
		error = kl_socket_poll(waits, 2U, (int)THREAD_ROUND_MS, &ready);
		if (error != 0)
			return error;

		/* A tab that closed wants no connection. */
		closing = thread_closing(link->session);
		if (closing)
			return EPIPE;

		/* The connection ended one way or the other. */
		if ((waits[0].ready & (KL_SOCKET_WRITABLE | KL_SOCKET_FAILED)) != 0U)
			break;
	}

	/* How it ended. */
	error = kl_socket_connect_result(socket);
	if (error != 0)
		return error;

	/* Succeeded: connected. */
	return 0;
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

		/* One more identity, whose path names it for its passphrase. */
		(void)snprintf(link->identity_paths[*count], sizeof(link->identity_paths[*count]), "%s", path);
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
	struct kl_socket_wait waits[THREAD_WAITS];
	struct kl_socket *x11_socket;
	int x11_writable;
	size_t forward_base;
	size_t count;
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

		/* The X server's round: the X clients' bytes both ways; a line for the tab when it stopped. */
		if (link->x11) {
			if (x11_host_pump(&link->x11_host, link->engine, thread_now()) != 0)
				session_say(link->session, link->x11_host.message);
		}

		/* The forwarded connections' bytes read go into their channels. */
		if (link->forwards != NULL)
			forward_pump(link->forwards);

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
		count = 2U;
		x11_writable = 0;
		x11_socket = NULL;
		if (link->x11)
			x11_socket = x11_host_socket(&link->x11_host, &x11_writable);
		if (x11_socket != NULL) {
			waits[2].socket = x11_socket;
			waits[2].waker = NULL;
			waits[2].events = KL_SOCKET_READABLE;
			if (x11_writable)
				waits[2].events |= KL_SOCKET_WRITABLE;
			waits[2].ready = 0U;
			count = 3U;
		}

		/* The forwardings' sockets after the others; a time limit of theirs may shorten the wait. */
		forward_base = count;
		if (link->forwards != NULL) {
			wake = now + wait;
			count += forward_waits(link->forwards, waits + forward_base, THREAD_WAITS - forward_base, now, &wake);
			if (wake <= now) {
				wait = 0U;
			} else if (wake < now + wait) {
				wait = wake - now;
			}
		}

		/* The wait. */
		error = kl_socket_poll(waits, count, (int)wait, &ready);
		if (error != 0) {
			(void)snprintf(link->reason, sizeof(link->reason), "The wait for the network failed (%s).", thread_error_text(error));
			return THREAD_FAILED;
		}

		/* What came; a socket that ended or failed is the connection's end. */
		if ((waits[0].ready & (KL_SOCKET_READABLE | KL_SOCKET_FAILED)) != 0U)
			(void)thread_receive(link);

		/* The forwardings' connections, accepted, connected, read and written. */
		if (link->forwards != NULL)
			forward_serve(link->forwards, waits + forward_base, thread_now());
	}

	/* The tab closed. */
	if (link->disconnecting)
		return THREAD_CLOSING;

	/* ExitOnForwardFailure ended it: the tab shows why and waits. */
	if (link->forward_failed)
		return THREAD_FAILED;

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
	int taken;
	int error;

	/* Each event in turn. */
	session = link->session;
	for (;;) {
		error = ssh_engine_next(link->engine, &event);
		if (error != 0)
			break;

		/* An event of the forwardings is theirs alone. */
		thread_forward_event(link, &event, &taken);
		if (taken)
			continue;

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
		case SSH_ENGINE_PASSPHRASE:
			/* The passphrase of a key the server would take: the one kept, or asked in the tab. */
			thread_passphrase(link, &event);
			break;
		case SSH_ENGINE_AUTHENTICATED:
			/* The port forwardings start once the user is in. */
			thread_forward_start(link);
			break;
		case SSH_ENGINE_DATA:
			/* An X client's bytes go to the X server. */
			if (event.channel != SSH_ENGINE_SESSION_CHANNEL) {
				if (link->x11)
					x11_host_data(&link->x11_host, link->engine, event.channel, event.bytes, event.size);
				break;
			}

			/* The session's bytes for the tab, given back to the window as the tab reads them. */
			thread_say_bytes(session, event.bytes, event.size);
			break;
		case SSH_ENGINE_X11_REPLY:
			/* A refusal is said as OpenSSH says it; the session goes on. */
			if (!event.accepted) {
				session_say(session, event.reason);
				session_say(session, "\r\n");
			}
			break;
		case SSH_ENGINE_X11_OPEN:
			thread_x11_open(link, &event);
			break;
		case SSH_ENGINE_CHANNEL_EOF:
			if (link->x11)
				x11_host_eof(&link->x11_host, event.channel);
			break;
		case SSH_ENGINE_CHANNEL_CLOSED:
			if (link->x11)
				x11_host_closed(&link->x11_host, event.channel);
			break;
		case SSH_ENGINE_EXIT:
			/* The shell ended while X clients stay: the connection waits for them. */
			if (link->x11 && x11_host_open_count(&link->x11_host) != 0)
				session_say(session, "\r\nThe shell ended; the connection stays open while X clients run (close the tab to end them).\r\n");

			/* No more forwarded connections are taken; those open keep the connection until they end. */
			if (link->forwards != NULL) {
				forward_stop_listening(link->forwards);
				if (forward_open_count(link->forwards) != 0U)
					session_say(session, "\r\nThe shell ended; the connection stays open while forwarded connections remain (close the tab to end them).\r\n");
			}
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
 * Decides about the server's host key: the user's and the global
 * known_hosts and StrictHostKeyChecking say whether it is accepted,
 * added, refused, or asked about, and the user is told why as OpenSSH
 * tells it.
 */
static void
thread_host_key(
	struct thread_link *link,
	const struct ssh_engine_event *event)
{
	enum ssh_known_result user_answer;
	enum ssh_known_result global_answer;
	enum ssh_engine_host_action action;
	char line[THREAD_LINE_MAX];
	int error;

	/* The key; one that is not ssh-ed25519 is refused. */
	error = ssh_key_parse_public_blob(event->bytes, event->size, link->host_key);
	if (error != 0) {
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		return;
	}

	/* Its fingerprint, and the host's name in known_hosts. */
	(void)ssh_key_fingerprint(link->host_key, link->fingerprint, sizeof(link->fingerprint));
	(void)mikuterm_host_port_name(link->host_name, link->config.port, link->known_name, sizeof(link->known_name));

	/* What the user's known_hosts and the global one say of it. */
	memset(&link->known, 0, sizeof(link->known));
	user_answer = thread_known_read(link, link->config.user_known_hosts);
	global_answer = thread_known_read(link, link->config.global_known_hosts);

	/* What to do. */
	error = ssh_engine_host_decide(user_answer, global_answer, link->config.strict, link->config.batch_mode, &action);
	if (error != 0)
		action = SSH_ENGINE_HOST_REFUSE_UNKNOWN;

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
		thread_host_ask(link, user_answer, global_answer);
		break;
	case SSH_ENGINE_HOST_REFUSE_CHANGED:
		thread_host_changed(link);
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		break;
	case SSH_ENGINE_HOST_REFUSE_REVOKED:
		thread_host_revoked(link);
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		break;
	default:
		/* Unknown where the settings do not let it be added. */
		(void)snprintf(line, sizeof(line), "No ED25519 host key is known for %s and you have requested strict checking.\r\n", link->known_name);
		session_say(link->session, line);
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		break;
	}
}

/*
 * Reads a known_hosts file the settings name (none, one that is not
 * there, or "/dev/null": nothing known) and looks the server's key up in
 * it, keeping what the user is to be told in link->known.  Returns the
 * library's answer.
 */
static enum ssh_known_result
thread_known_read(
	struct thread_link *link,
	const char *written)
{
	enum ssh_known_result answer;
	char path[MIKUTERM_PATH_MAX];
	char shown[MIKUTERM_PATH_MAX];
	char *text;
	size_t size;
	int differs;
	int error;

	/* The file that stands for none. */
	answer = SSH_KNOWN_ABSENT;
	differs = strcmp(written, THREAD_NO_FILE);
	if (differs == 0)
		return answer;

	/* The file in the folder, or on Windows; one that cannot be read knows nothing. */
	error = mikuterm_ssh_path(path, sizeof(path), written);
	if (error != 0)
		return answer;
	error = mikuterm_file_read(path, MIKUTERM_HOSTS_MAX, &text, &size);
	if (error != 0)
		return answer;

	/* The library's answer, and the lines the user is told of. */
	(void)ssh_known_hosts_check(text, size, link->host_name, link->config.port, link->host_key, &answer, NULL);
	mikuterm_path_shown(path, shown, sizeof(shown));
	mikuterm_known_hosts_scan(text, size, shown, link->known_name, link->host_key, &link->known);
	free(text);

	/* Succeeded: what the file says. */
	return answer;
}

/*
 * Asks whether to trust an unknown host's key, as OpenSSH asks: the host
 * and the address reached, the key's fingerprint, the other names the key
 * is known by, and yes, no or the fingerprint.
 */
static void
thread_host_ask(
	struct thread_link *link,
	enum ssh_known_result user_answer,
	enum ssh_known_result global_answer)
{
	struct mikuterm_known_other *other;
	char address[MIKUTERM_HOST_MAX + 8U];
	char line[THREAD_LINE_MAX];
	unsigned index;
	int other_types;
	int error;

	/* The host as known_hosts names it, and the address reached as OpenSSH shows it. */
	error = mikuterm_address_name(&link->address, link->config.port, address, sizeof(address));
	if (error != 0)
		(void)snprintf(address, sizeof(address), "%s", link->known_name);

	/* Whether the host is known only by keys of other types. */
	other_types = 0;
	if (user_answer == SSH_KNOWN_OTHER_TYPES || global_answer == SSH_KNOWN_OTHER_TYPES)
		other_types = 1;

	/* Whose key it is, and its fingerprint. */
	if (other_types) {
		(void)snprintf(line,
			       sizeof(line),
			       "The authenticity of host '%s (%s)' can't be established\r\n"
			       "but keys of different type are already known for this host.\r\n",
			       link->known_name,
			       address);
	} else {
		(void)snprintf(line, sizeof(line), "The authenticity of host '%s (%s)' can't be established.\r\n", link->known_name, address);
	}

	/* Tells whose key it is, then its fingerprint. */
	session_say(link->session, line);
	(void)snprintf(line, sizeof(line), "ED25519 key fingerprint is %s.\r\n", link->fingerprint);
	session_say(link->session, line);

	/* The other names it is known by, the first few of them. */
	if (link->known.other_count == 0U) {
		session_say(link->session, "This key is not known by any other names.\r\n");
	} else {
		session_say(link->session, "This host key is known by the following other names/addresses:\r\n");
		for (index = 0U; index < link->known.other_count && index < MIKUTERM_KNOWN_OTHERS; index++) {
			other = &link->known.others[index];
			(void)snprintf(line, sizeof(line), "    %s:%u: %s\r\n", other->path, other->line, other->names);
			session_say(link->session, line);
		}
	}

	/* The names left out. */
	if (link->known.other_count > MIKUTERM_KNOWN_OTHERS) {
		(void)snprintf(line, sizeof(line), "    (%u additional names omitted)\r\n", link->known.other_count - MIKUTERM_KNOWN_OTHERS);
		session_say(link->session, line);
	}

	/* The question. */
	thread_ask_start(link->session, "Are you sure you want to continue connecting (yes/no/[fingerprint])? ", SESSION_ASK_LINE);
	link->asked = THREAD_ASKED_HOST_KEY;
}

/*
 * Warns that the host showed another key than the one known for it, and
 * says which line holds the old one and how to take it away.  The
 * connection is refused: the screen never replaces a known key.
 */
static void
thread_host_changed(
	struct thread_link *link)
{
	char line[THREAD_LINE_MAX];

	/* The warning's frame, as OpenSSH draws it. */
	session_say(link->session, THREAD_WARNING_RULE);
	session_say(link->session, "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\r\n");
	session_say(link->session, THREAD_WARNING_RULE);

	/* What it may mean. */
	session_say(link->session, "The host's key is not the one known_hosts holds for it.  Someone may be\r\n");
	session_say(link->session, "listening in on the connection (a man-in-the-middle attack), or the\r\n");
	session_say(link->session, "host's key may have been replaced.\r\n");

	/* The key the server showed. */
	session_say(link->session, "The fingerprint for the ED25519 key sent by the remote host is\r\n");
	(void)snprintf(line, sizeof(line), "%s.\r\n", link->fingerprint);
	session_say(link->session, line);

	/* The line that holds the old key, and what to do when the key was replaced on purpose. */
	if (link->known.changed_line != 0U) {
		(void)snprintf(line,
			       sizeof(line),
			       "If the key was replaced on purpose, delete line %u of %s\r\n"
			       "and connect again.\r\n",
			       link->known.changed_line,
			       link->known.changed_path);
		session_say(link->session, line);
		(void)snprintf(line, sizeof(line), "Offending ED25519 key in %s:%u\r\n", link->known.changed_path, link->known.changed_line);
		session_say(link->session, line);
	}

	/* The refusal. */
	(void)snprintf(line, sizeof(line), "Host key for %s has changed and you have requested strict checking.\r\n", link->known_name);
	session_say(link->session, line);
}

/* Warns that the host showed a key marked as revoked; the connection is refused. */
static void
thread_host_revoked(
	struct thread_link *link)
{
	char line[THREAD_LINE_MAX];

	/* The warning's frame, as OpenSSH draws it. */
	session_say(link->session, THREAD_WARNING_RULE);
	session_say(link->session, "@       WARNING: REVOKED HOST KEY DETECTED!               @\r\n");
	session_say(link->session, THREAD_WARNING_RULE);

	/* Which key, where it is marked, and what it may mean. */
	if (link->known.revoked_line != 0U) {
		(void)snprintf(line,
			       sizeof(line),
			       "The ED25519 host key for %s is marked as revoked (%s:%u).\r\n",
			       link->known_name,
			       link->known.revoked_path,
			       link->known.revoked_line);
	} else {
		(void)snprintf(line, sizeof(line), "The ED25519 host key for %s is marked as revoked.\r\n", link->known_name);
	}

	/* What that may mean. */
	session_say(link->session, line);
	session_say(link->session, "Someone may be using a stolen key to pretend to be this host.\r\n");
}

/*
 * Takes the user's answer about an unknown host: yes or the fingerprint
 * accepts and keeps the key, no or nothing refuses, anything else asks
 * again.
 */
static void
thread_host_answer(
	struct thread_link *link,
	const char *answer,
	int cancelled)
{
	int confirmed;

	/* What the answer says; giving the question up is no. */
	confirmed = THREAD_CONFIRM_NO;
	if (!cancelled)
		confirmed = thread_confirm(answer, link->fingerprint);

	/* Anything else asks again. */
	if (confirmed == THREAD_CONFIRM_AGAIN) {
		thread_ask_start(link->session, "Please type 'yes', 'no' or the fingerprint: ", SESSION_ASK_LINE);
		return;
	}

	/* No ends the connection. */
	link->asked = THREAD_ASKED_NONE;
	if (confirmed == THREAD_CONFIRM_NO) {
		link->host_refused = 1;
		(void)ssh_engine_host_answer(link->engine, 0);
		return;
	}

	/* Yes: the key is kept for the next time (a folder that cannot keep it accepts it for now). */
	(void)thread_known_hosts_add(link);
	(void)ssh_engine_host_answer(link->engine, 1);
}

/*
 * Reads an answer to the question about an unknown host's key, as OpenSSH
 * reads it: its first word (white space before and after it ignored);
 * nothing or "no" is THREAD_CONFIRM_NO, "yes" (both in any case) or the
 * fingerprint exactly is THREAD_CONFIRM_YES, anything else
 * THREAD_CONFIRM_AGAIN.
 */
static int
thread_confirm(
	const char *answer,
	const char *fingerprint)
{
	char word[SESSION_LINE_MAX];
	size_t length;
	int no_differs;
	int yes_differs;
	int fingerprint_differs;

	/* The first word. */
	while (*answer == ' ' || *answer == '\t')
		answer++;
	length = strcspn(answer, " \t");
	if (length >= sizeof(word))
		length = sizeof(word) - 1U;
	memcpy(word, answer, length);
	word[length] = '\0';

	/* Nothing, or no. */
	no_differs = _stricmp(word, "no");
	if (length == 0U || no_differs == 0)
		return THREAD_CONFIRM_NO;

	/* Yes, or the fingerprint itself. */
	yes_differs = _stricmp(word, "yes");
	fingerprint_differs = strcmp(word, fingerprint);
	if (yes_differs == 0 || fingerprint_differs == 0)
		return THREAD_CONFIRM_YES;

	/* Succeeded: the answer is neither. */
	return THREAD_CONFIRM_AGAIN;
}

/*
 * Keeps the host's key in the user's known_hosts and says so as OpenSSH
 * does ("/dev/null" keeps nothing and is told the same).  Returns 0, or
 * the error of a file that could not keep it (the key is then accepted
 * for this connection alone, and the user is told).
 */
static int
thread_known_hosts_add(
	struct thread_link *link)
{
	char path[MIKUTERM_PATH_MAX];
	char shown[MIKUTERM_PATH_MAX];
	char text[THREAD_LINE_MAX];
	int differs;
	int error;

	/* The file, unless it is the one that keeps nothing. */
	differs = strcmp(link->config.user_known_hosts, THREAD_NO_FILE);
	error = 0;
	if (differs != 0) {
		/* Where it is, and how the user is told of it. */
		mikuterm_path_shown(link->config.user_known_hosts, shown, sizeof(shown));
		error = mikuterm_ssh_path(path, sizeof(path), link->config.user_known_hosts);
		if (error == 0) {
			mikuterm_path_shown(path, shown, sizeof(shown));
			error = thread_known_hosts_write(link, path);
		}
	}

	/* A file that could not keep it. */
	if (error != 0) {
		(void)snprintf(text, sizeof(text), "Failed to add the host to the list of known hosts (%s).\r\n", shown);
		session_say(link->session, text);
		return error;
	}

	/* Succeeded: kept. */
	(void)snprintf(text, sizeof(text), "Warning: Permanently added '%s' (ED25519) to the list of known hosts.\r\n", link->known_name);
	session_say(link->session, text);
	return 0;
}

/*
 * Appends the host's line to a known_hosts file in one write, by one
 * thread of the program at a time: the host in lower case ([host]:port
 * for a port other than 22), not hashed, and a LF first when the file's
 * last line has none.  Returns 0 or the error.
 */
static int
thread_known_hosts_write(
	struct thread_link *link,
	const char *path)
{
	static struct kl_mutex known_hosts_lock = KL_MUTEX_INIT;
	struct ssh_buf line;
	char host[MIKUTERM_HOST_MAX];
	char *old;
	size_t size;
	int unended;
	int error;

	/* The host as OpenSSH writes it: in lower case (the line adds the port). */
	error = mikuterm_host_port_name(link->host_name, 22U, host, sizeof(host));
	if (error != 0)
		return error;

	/* The line, with room for a LF before it. */
	ssh_buf_init(&line, 1024U, 0);
	ssh_buf_put_u8(&line, '\n');
	error = ssh_known_hosts_put_line(host, link->config.port, link->host_key, &line);
	if (error != 0) {
		ssh_buf_free(&line);
		return error;
	}

	/* The file as it ends now, and the line after it. */
	kl_mutex_lock(&known_hosts_lock);

	old = NULL;
	size = 0U;
	error = mikuterm_file_read(path, MIKUTERM_HOSTS_MAX, &old, &size);
	unended = 0;
	if (error == 0 &&
	    size != 0U &&
	    old[size - 1U] != '\n')
		unended = 1;
	free(old);
	if (unended)
		error = mikuterm_file_append(path, line.data, line.size);
	else
		error = mikuterm_file_append(path, line.data + 1U, line.size - 1U);

	kl_mutex_unlock(&known_hosts_lock);


	/* The line is written or not; its buffer goes. */
	ssh_buf_free(&line);

	/* A file that did not take the line. */
	if (error != 0)
		return error;

	/* Succeeded: the line is in the file. */
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
			(void)snprintf(link->reason, sizeof(link->reason), "%s@%s: %s", link->config.user, link->host_name, event->reason);
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

	/* The keys taken leave the buffer, and a full buffer has room again. */
	if (written != 0U) {
		memmove(session->input, session->input + written, session->input_length - written);
		session->input_length -= written;
		if (session->input_full) {
			session->input_full = 0;
			room = 1;
		}
	}

	kl_mutex_unlock(&session->lock);

	/* The user typed in the tab: its X server's windows may come to the front (not while a question is asked). */
	if (written != 0U && link->x11 && link->asked == THREAD_ASKED_NONE)
		x11_host_foreground(&link->x11_host);

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
	int keep;

	/* Nothing asked. */
	if (link->asked == THREAD_ASKED_NONE)
		return;

	/* The answer, when it came. */
	answered = thread_ask_take(link->session, answer, sizeof(answer), &cancelled, &keep);
	if (!answered)
		return;

	/* What it answers. */
	if (link->asked == THREAD_ASKED_HOST_KEY) {
		thread_host_answer(link, answer, cancelled);
	} else if (link->asked == THREAD_ASKED_PASSPHRASE) {
		thread_passphrase_answer(link, answer, cancelled, keep);
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

		/* What the engine took is gone from the buffer; none taken waits for more. */
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
		answered = thread_ask_take(session, answer, size, &cancelled, NULL);
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
 * size), whether the user gave up and, when keep is not NULL, whether the
 * user wants it kept for the Windows session.  Returns 1 when it came, 0
 * when not yet.
 */
static int
thread_ask_take(
	struct mikuterm_session *session,
	char *answer,
	size_t size,
	int *cancelled,
	int *keep)
{
	size_t length;
	int answered;

	/* The line, once answered; the question is over and the line wiped. */
	kl_mutex_lock(&session->lock);

	answered = session->answered;
	*cancelled = session->cancelled;
	if (keep != NULL)
		*keep = session->keep;
	if (answered) {
		length = session->line_length;
		if (length >= size)
			length = size - 1U;
		memcpy(answer, session->line, length);
		answer[length] = '\0';
		OPENSSL_cleanse(session->line, sizeof(session->line));
		session->line_length = 0U;
		session->question = SESSION_ASK_NONE;
		session->keep_offered = 0;
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

/*
 * Answers the engine's question for the passphrase of a key the server
 * would take: the passphrase kept for the Windows session, tried once in
 * an attempt (a wrong one is forgotten, and the engine asks again), or
 * else the user, asked in the tab without the passphrase being shown,
 * with the choice of keeping it.
 */
static void
thread_passphrase(
	struct thread_link *link,
	const struct ssh_engine_event *event)
{
	char kept[SESSION_LINE_MAX];
	char shown[MIKUTERM_PATH_MAX];
	const char *path;
	unsigned identity;
	int error;

	/* A key the attempt did not read cannot be asked for: it is passed over. */
	identity = event->identity;
	if (identity >= SSH_ENGINE_IDENTITIES_MAX) {
		(void)ssh_engine_passphrase(link->engine, identity, NULL);
		return;
	}

	/* The key's file. */
	path = link->identity_paths[identity];

	/* The passphrase kept for the Windows session, the first time the key is asked for in this attempt. */
	if (!link->recalled[identity]) {
		link->recalled[identity] = 1;
		error = mikuterm_passphrase_recall(path, kept, sizeof(kept));
		if (error == 0) {
			/* Opens the key with it; a wrong one is forgotten, and the engine asks again. */
			error = ssh_engine_passphrase(link->engine, identity, kept);
			OPENSSL_cleanse(kept, sizeof(kept));
			printf("MIKUTERM PASSPHRASE kept used=%d\n", error == 0);
			fflush(stdout);
			if (error == EACCES)
				mikuterm_passphrase_forget(path);
			return;
		}
	}

	/* Asks the user, naming the key as its place in the folder, with the choice of keeping it. */
	mikuterm_path_shown(path, shown, sizeof(shown));
	kl_mutex_lock(&link->session->lock);

	(void)snprintf(link->session->prompt, sizeof(link->session->prompt), "Enter passphrase for key '%s': ", shown);
	if (!link->session->line_started)
		(void)session_put_locked(link->session, "\r\n", 2U);
	session_keep_line_locked(link->session);
	link->session->keep_offered = 1;

	kl_mutex_unlock(&link->session->lock);

	/* The prompt; the keys typed are the passphrase from now on. */
	thread_ask_start(link->session, link->session->prompt, SESSION_ASK_SECRET);
	link->asked = THREAD_ASKED_PASSPHRASE;
	link->asked_identity = identity;
}

/*
 * Gives the engine the user's passphrase (or none, when the user gave up)
 * and keeps it for the Windows session when it opened the key and the
 * user chose to.
 */
static void
thread_passphrase_answer(
	struct thread_link *link,
	const char *answer,
	int cancelled,
	int keep)
{
	const char *path;
	int error;

	/* The question is answered. */
	link->asked = THREAD_ASKED_NONE;
	path = link->identity_paths[link->asked_identity];

	/* The user gave up: the key is passed over. */
	if (cancelled) {
		(void)ssh_engine_passphrase(link->engine, link->asked_identity, NULL);
		return;
	}

	/* The engine opens the key with it at once; an empty one passes the key over. */
	error = ssh_engine_passphrase(link->engine, link->asked_identity, answer);
	if (error != 0 ||
	    answer[0] == '\0' ||
	    !keep)
		return;

	/* Keeps the passphrase that opened the key until the user signs out of Windows. */
	error = mikuterm_passphrase_keep(path, answer);
	printf("MIKUTERM PASSPHRASE keep error=%d\n", error);
	fflush(stdout);
}

/*
 * Prepares X11 forwarding when the host's settings ask for it
 * (ForwardX11 yes): a fake MIT-MAGIC-COOKIE-1 of 16 random bytes for this
 * connection (its hexadecimal text for x11-req), and the X server's
 * settings.  Returns 0, or an errno value (forwarding stays off).
 */
static int
thread_x11_start(
	struct thread_link *link)
{
	struct x11_host_settings settings;
	unsigned i;
	int error;
	static const char digits[] = "0123456789abcdef";

	/* Nothing unless asked. */
	link->x11 = 0;
	link->x11_refused = 0;
	if (!link->config.forward_x11)
		return 0;

	/* The cookie, from the system's random numbers. */
	memset(&settings, 0, sizeof(settings));
	error = ssh_random(settings.cookie, X11_HOST_COOKIE);
	if (error != 0)
		return error;
	for (i = 0; i < X11_HOST_COOKIE; i++) {
		link->x11_cookie[i * 2U] = digits[settings.cookie[i] >> 4];
		link->x11_cookie[i * 2U + 1U] = digits[settings.cookie[i] & 0x0FU];
	}
	link->x11_cookie[X11_HOST_COOKIE * 2U] = '\0';

	/* The X server's settings: the host's name in the titles, the scale, the clipboard's policy. */
	(void)snprintf(settings.host, sizeof(settings.host), "%s", link->target->title);
	settings.scale = link->session->x11_scale;
	settings.flags = 0;
	if (link->config.forward_x11_trusted)
		settings.flags |= X11_HOST_TRUSTED;
	if (link->session->x11_primary)
		settings.flags |= X11_HOST_PRIMARY;
	if (link->session->x11_render_cpu)
		settings.flags |= X11_HOST_RENDER_CPU;
	x11_host_init(&link->x11_host, &settings);

	/* Succeeded: x11-req goes with the session. */
	link->x11 = 1;
	link->x11_since_ms = thread_now();
	return 0;
}

/*
 * Answers an X client the server forwards: refused when forwarding was not
 * asked, when ForwardX11Timeout passed on an untrusted connection, or when
 * the X server cannot take it (the tab is told once).
 */
static void
thread_x11_open(
	struct thread_link *link,
	const struct ssh_engine_event *event)
{
	uint64_t now;
	int error;

	/* Not asked: refused. */
	if (!link->x11) {
		(void)ssh_engine_x11_answer(link->engine, event->channel, 0);
		return;
	}

	/* An untrusted connection's time for new X clients may have passed. */
	now = thread_now();
	if (!link->config.forward_x11_trusted && link->config.forward_x11_timeout != 0 &&
	    now - link->x11_since_ms > (uint64_t)link->config.forward_x11_timeout * 1000U) {
		if (!link->x11_refused)
			session_say(link->session, "\r\nRejected X11 connection after ForwardX11Timeout expired\r\n");
		link->x11_refused = 1;
		(void)ssh_engine_x11_answer(link->engine, event->channel, 0);
		return;
	}

	/* The X server takes it, or cannot. */
	error = x11_host_accept(&link->x11_host, event->channel, now);
	if (error != 0) {
		if (!link->x11_refused)
			session_say(link->session, "\r\nX11: an X client was refused (the X server could not take it).\r\n");
		link->x11_refused = 1;
		(void)ssh_engine_x11_answer(link->engine, event->channel, 0);
		return;
	}
	(void)ssh_engine_x11_answer(link->engine, event->channel, 1);
}

/*
 * Puts RemoteCommand's tokens in their places (link->command; empty when
 * the host has none).  Returns 0, or EINVAL with the reason.
 */
static int
thread_remote_command(
	struct thread_link *link)
{
	struct remote_command_tokens tokens;
	char local_user[THREAD_LOCAL_NAME_MAX];
	char home[THREAD_LOCAL_NAME_MAX];
	char local_host[THREAD_LOCAL_NAME_MAX];
	char config[sizeof(THREAD_SSH_CONFIG)];
	char error_text[REMOTE_COMMAND_ERROR_MAX];
	int error;

	/* No RemoteCommand: the shell. */
	if (link->config.remote_command[0] == '\0')
		return 0;

	/* The connection's values, and the local user's and computer's. */
	memset(&tokens, 0, sizeof(tokens));
	tokens.host_name = link->host_name;
	tokens.host = link->target->host;
	tokens.port = link->config.port;
	tokens.user = link->config.user;
	thread_local_names(&tokens, local_user, home, local_host, THREAD_LOCAL_NAME_MAX);

	/* The command, its tokens put in. */
	error = remote_command_expand(link->config.remote_command, &tokens, &link->command, error_text, sizeof(error_text));
	if (error != 0) {
		mikuterm_path_shown(THREAD_SSH_CONFIG, config, sizeof(config));
		(void)snprintf(link->reason, sizeof(link->reason), "%s: %s", config, error_text);
		return error;
	}

	/* Succeeded: link->command holds the command. */
	return 0;
}

/*
 * Finds the local user's name (%u), home folder (%d) and the computer's
 * name (%l) for RemoteCommand, each in size bytes: on Windows from the
 * system (the home folder is the profile's, as Windows' OpenSSH gives
 * it), elsewhere (the host tests) from the environment.  A name not found
 * is left NULL in tokens (the computer's name empty).
 */
static void
thread_local_names(
	struct remote_command_tokens *tokens,
	char *local_user,
	char *home,
	char *local_host,
	size_t size)
{
#ifdef _WIN32
	wchar_t wide[THREAD_LOCAL_NAME_MAX];
	DWORD length;
	BOOL done;
	int error;

	/* The local user's name, when Windows tells it. */
	length = THREAD_LOCAL_NAME_MAX;
	done = GetUserNameW(wide, &length);
	if (done) {
		error = thread_local_name(wide, local_user, size);
		if (error == 0)
			tokens->local_user = local_user;
	}

	/* The profile's folder. */
	length = GetEnvironmentVariableW(L"USERPROFILE", wide, THREAD_LOCAL_NAME_MAX);
	if (length != 0U && length < THREAD_LOCAL_NAME_MAX) {
		error = thread_local_name(wide, home, size);
		if (error == 0)
			tokens->home = home;
	}

	/* The computer's name in the DNS, or none. */
	local_host[0] = '\0';
	length = THREAD_LOCAL_NAME_MAX;
	done = GetComputerNameExW(ComputerNameDnsHostname, wide, &length);
	if (done)
		(void)thread_local_name(wide, local_host, size);
	tokens->local_host = local_host;
#else
	const char *value;
	int status;

	/* The local user's name. */
	value = getenv("USER");
	if (value != NULL) {
		(void)snprintf(local_user, size, "%s", value);
		tokens->local_user = local_user;
	}

	/* The home folder. */
	value = getenv("HOME");
	if (value != NULL) {
		(void)snprintf(home, size, "%s", value);
		tokens->home = home;
	}

	/* The computer's name, or none. */
	status = gethostname(local_host, size);
	if (status != 0)
		local_host[0] = '\0';
	local_host[size - 1U] = '\0';
	tokens->local_host = local_host;
#endif
}

#ifdef _WIN32
/*
 * Writes a name Windows gave in UTF-16 as UTF-8.  Returns 0, or EINVAL
 * when it does not fit (name is then empty).
 */
static int
thread_local_name(
	const wchar_t *wide,
	char *name,
	size_t size)
{
	int written;

	/* Converts the name with its NUL. */
	written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, (int)size, NULL, NULL);
	if (written <= 0) {
		name[0] = '\0';
		return EINVAL;
	}

	/* Succeeded: name holds the name. */
	return 0;
}
#endif

/*
 * Makes the port forwardings' state of a connection whose host has
 * LocalForward or RemoteForward (none for one without).  Returns 0 or
 * ENOMEM.
 */
static int
thread_forwards_open(
	struct thread_link *link)
{
	struct forward_state *forwards;

	/* A host without forwardings needs none. */
	link->forwards = NULL;
	link->forward_failed = 0;
	if (link->config.forward_count == 0U)
		return 0;

	/* The state is large (a chunk for each connection): it is on the heap. */
	forwards = malloc(sizeof(*forwards));
	if (forwards == NULL)
		return ENOMEM;

	/* Starts it on the connection's engine and settings; an attempt after a failed one waits once for a port still held. */
	forward_init(forwards, link->engine, &link->config, thread_forward_say, link);
	forwards->rebind_wait = link->again;
	link->forwards = forwards;

	/* Succeeded: the forwardings start once the user is authenticated. */
	return 0;
}

/* Closes the port forwardings' sockets and frees their state. */
static void
thread_forwards_close(
	struct thread_link *link)
{
	/* A host without forwardings has nothing to close. */
	if (link->forwards == NULL)
		return;

	/* Closes every socket and wipes the bytes held, then frees the state. */
	forward_free(link->forwards);
	OPENSSL_cleanse(link->forwards, sizeof(*link->forwards));
	free(link->forwards);
	link->forwards = NULL;
}

/*
 * Hands an event of the engine to the port forwardings: *taken is 1 when
 * it was theirs.
 */
static void
thread_forward_event(
	struct thread_link *link,
	const struct ssh_engine_event *event,
	int *taken)
{
	int status;

	/* A host without forwardings takes none. */
	*taken = 0;
	if (link->forwards == NULL)
		return;

	/* The forwardings take theirs; a refused remote forwarding may end the connection. */
	status = forward_event(link->forwards, event, thread_now(), taken);
	if (status == FORWARD_FATAL)
		thread_forward_fail(link);
}

/* Starts the port forwardings once the user is authenticated. */
static void
thread_forward_start(
	struct thread_link *link)
{
	int status;

	/* A host without forwardings has none to start. */
	if (link->forwards == NULL)
		return;

	/* Listens and asks; a forwarding that could not be set up may end the connection. */
	status = forward_start(link->forwards);
	if (status == FORWARD_FATAL)
		thread_forward_fail(link);
}

/*
 * Ends the connection because a forwarding could not be set up and
 * ExitOnForwardFailure is yes: the tab shows the forwardings' line as the
 * reason and waits for Enter, as for a failed connection.
 */
static void
thread_forward_fail(
	struct thread_link *link)
{
	/* Only the first failure ends it. */
	if (link->forward_failed)
		return;

	/* The reason is the forwardings' line, as OpenSSH's ssh writes it before it exits. */
	(void)snprintf(link->reason, sizeof(link->reason), "%s", link->forwards->fatal);
	link->forward_failed = 1;

	/* No more connections are taken, and the connection ends. */
	forward_stop_listening(link->forwards);
	ssh_engine_disconnect(link->engine, "port forwarding failed");
}

/* Shows a line of the port forwardings in the tab, on a line of its own. */
static void
thread_forward_say(
	void *data,
	const char *line)
{
	struct thread_link *link;

	/* The connection the forwardings belong to. */
	link = data;

	/* The line, and its end. */
	session_say_line(link->session, line);
	session_say(link->session, "\r\n");
}
