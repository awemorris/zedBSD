/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The connection of the ssh program around its engine: the login (the
 * host key against known_hosts, the identity files, passwords asked on
 * the terminal), the session (RFC 4254 6) with a pseudo-terminal when one
 * is wanted, and the standard streams carried over it until the
 * connection ends; the escape sequences of the terminal (ssh(1) ESCAPE
 * CHARACTERS), the window size, ConnectTimeout (ssh_config(5)), the
 * messages for how the connection ended, and the exit status.
 *
 * Everything runs in one poll loop.  The socket does not block; the
 * standard streams keep their mode (they may be shared with other
 * processes) and are written in pieces poll says fit.  The engine does the
 * protocol: the transport, the order of the login, the channel, its window
 * and ServerAliveInterval.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include "ssh-client-common.h"
#include "ssh-ttymodes.h"

/* The longest wait of one poll, so that the engine gets the time at least this often. */
#define CLIENT_SESSION_POLL_MS	1000U

/* The pollfd slots of the loop. */
#define CLIENT_SLOT_SOCKET	0
#define CLIENT_SLOT_INPUT	1
#define CLIENT_SLOT_OUTPUT	2
#define CLIENT_SLOT_ERROR	3
#define CLIENT_SLOT_SIGNAL	4
#define CLIENT_SLOTS		5

/* The terminal type sent when TERM does not give one. */
#define CLIENT_DEFAULT_TERM	"dumb"

/* How the engine words a refused session channel, before the reason. */
#define CLIENT_OPEN_FAILED	"channel open failed: "

/* The number of the one channel the program opens (the engine's first). */
#define CLIENT_CHANNEL		0UL

/*
 * One standard stream the server's data goes to.
 *
 * queue holds the bytes not written yet from offset on (the written
 * bytes before offset are removed in large steps, not after every small
 * write, so that writing costs no more than the bytes written); broken is
 * 1 after a write failed (later data is dropped); chunk is the most
 * written at once.
 */
struct client_stream {
	struct ssh_buf queue;
	size_t offset;
	int descriptor;
	int broken;
	size_t chunk;
};

/*
 * The state of the loop.
 *
 * want_session is 0 for -N.  input holds the user's bytes read and passed
 * through the escapes but not yet taken by the engine (input_size of
 * them, from input_offset).  input_end is 1 once the standard input
 * reached its end (or -n, -N, "~."), eof_given 1 once the engine was told.
 * pty is 1 when a terminal is asked for (cleared once the program said
 * itself that the connection closed), tty_note 1 when the program is to
 * say at the login that it asks for none.  raw is 1 while the local
 * terminal is in raw mode (saved holds its settings), escape_on while the
 * escapes are read.  host_refused is 1 when the program refused the host
 * key (and said why), authenticated 1 after the login, started 1 once the
 * shell or the command runs.  password_out is 1 while a password waits
 * for its answer and passwords_sent counts them.  have_exit and
 * exit_status are the command's status, killed 1 when a signal ended it.
 * failed is 1 when the session or the connection failed (the exit value
 * is 255), quit 1 when it ended at the user's or a signal's word.  closed
 * is 1 once the engine said the connection ended, reported 1 when that
 * end was reported as an error.  done is 1 when the loop is to end.
 * deadline is when a server that has not identified itself is given up
 * (ConnectTimeout; 0 for never).
 */
struct client_session {
	struct ssh_client *client;
	int want_session;
	struct client_stream output;
	struct client_stream error;
	uint8_t input[SSH_CLIENT_INPUT_CHUNK + 1];
	size_t input_size;
	size_t input_offset;
	int input_end;
	int eof_given;
	int pty;
	int tty_note;
	int raw;
	struct termios saved;
	int escape_on;
	struct ssh_client_escape escape;
	int host_refused;
	int authenticated;
	int started;
	int password_out;
	unsigned passwords_sent;
	int have_exit;
	int exit_status;
	int killed;
	int failed;
	int quit;
	int closed;
	int reported;
	int done;
	uint64_t deadline;
};

/*
 * The pipe a signal handler writes the signal's number into, so that the
 * poll loop wakes up and handles the signal outside the handler.
 *
 * [0] is read by the loop, [1] written by the handler; both are -1 until
 * client_signals_start makes the pipe, and stay open until the program
 * ends.  Only the handler and the loop touch them.
 */
static int client_signal_pipe[2] = { -1, -1 };

static void client_session_setup(struct client_session *session, struct ssh_client *client, const struct ssh_client_args *args);
static void client_session_finish(struct client_session *session);
static int client_open_engine(struct client_session *session, const uint8_t *command, size_t size);
static int client_put_terminal(struct ssh_engine_options *options, struct ssh_buf *modes);
static int client_wants_pty(struct client_session *session, size_t command_size);
static int client_session_run(struct client_session *session);
static int client_take_events(struct client_session *session);
static int client_take_event(struct client_session *session, const struct ssh_engine_event *event);
static void client_take_host_key(struct client_session *session, const struct ssh_engine_event *event);
static int client_take_password(struct client_session *session, const struct ssh_engine_event *event);
static int client_take_login(struct client_session *session);
static void client_take_start(struct client_session *session, const struct ssh_engine_event *event);
static int client_take_data(struct client_session *session, const struct ssh_engine_event *event);
static void client_take_exit(struct client_session *session, const struct ssh_engine_event *event);
static void client_take_closed(struct client_session *session, const struct ssh_engine_event *event);
static void client_note_rekeys(struct client_session *session);
static int client_signals_start(void);
static void client_signal_handler(int number);
static void client_handle_signals(struct client_session *session);
static void client_send_window(struct client_session *session);
static void client_read_input(struct client_session *session);
static void client_send_input(struct client_session *session);
static void client_write_stream(struct client_session *session, struct client_stream *stream);
static void client_enter_raw(struct client_session *session);
static void client_leave_raw(struct client_session *session);
static void client_stream_init(struct client_stream *stream, int descriptor);
static int client_finished(const struct client_session *session);
static void client_quit(struct client_session *session);

/*
 * Runs the connection after the socket is connected.
 */
int
ssh_client_run(
	struct ssh_client *client,
	const struct ssh_client_args *args,
	const uint8_t *command,
	size_t size,
	int *exit_status)
{
	struct client_session *session;
	int error;

	/* Fails until the session says otherwise. */
	*exit_status = SSH_CLIENT_EXIT_FAILURE;

	/* Allocates the loop's state, which is large for the stack. */
	session = malloc(sizeof(*session));
	if (session == NULL) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: %s", strerror(ENOMEM));
		return ENOMEM;
	}

	/* The session's starting state from the arguments. */
	client_session_setup(session, client, args);

	/* Opens the engine for the login and the session. */
	error = client_open_engine(session, command, size);
	if (error != 0) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: cannot start the connection: %s", strerror(error));
		client_session_finish(session);
		free(session);
		return error;
	}

	/* Runs the loop. */
	error = client_session_run(session);

	/* Puts the terminal back before any message, so that the message reads well. */
	client_leave_raw(session);

	/* Reports an error of the loop itself, and tells the server the connection ends. */
	if (error != 0) {
		ssh_client_report(client, error, 0, "");
		ssh_engine_disconnect(client->engine, NULL);
		client_session_finish(session);
		free(session);
		return error;
	}

	/* Tells the user the connection is over when the session ran on a terminal. */
	if (session->authenticated &&
	    session->pty &&
	    !session->reported)
		ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Connection to %s closed.", client->host_name);

	/* Takes the exit value: the command's status, or 255 for a signal, a failure or no status. */
	if (session->have_exit &&
	    !session->killed &&
	    !session->failed &&
	    !session->quit)
		*exit_status = session->exit_status & 0xff;
	client_session_finish(session);
	free(session);

	/* Succeeded: the connection ran to its end and *exit_status is set. */
	return 0;
}

/*
 * Starts the escape state at the start of a line.
 */
void
ssh_client_escape_init(
	struct ssh_client_escape *escape)
{
	/* The first byte typed is at the start of a line, and no '~' waits. */
	escape->line_start = 1;
	escape->pending = 0;
}

/*
 * Passes the user's input through the escape state.
 */
void
ssh_client_escape_filter(
	struct ssh_client_escape *escape,
	const uint8_t *in,
	size_t size,
	uint8_t *out,
	size_t *out_size,
	int *disconnect)
{
	size_t index;
	size_t count;
	uint8_t byte;

	/* Passes each byte, holding back a '~' at the start of a line. */
	count = 0;
	*disconnect = 0;
	for (index = 0; index < size; index++) {
		byte = in[index];

		/* Reads the byte after a held '~'. */
		if (escape->pending) {
			escape->pending = 0;

			/* "~." ends the connection; nothing after it is sent. */
			if (byte == '.') {
				*disconnect = 1;
				break;
			}

			/* "~~" sends one '~'; any other byte sends the '~' and itself. */
			out[count] = '~';
			count++;
			if (byte != '~') {
				out[count] = byte;
				count++;
			}

			/* Notes whether the next byte starts a line. */
			escape->line_start = 0;
			if (byte == '\r' || byte == '\n')
				escape->line_start = 1;
			continue;
		}

		/* Holds a '~' at the start of a line until the next byte shows what it means. */
		if (escape->line_start && byte == '~') {
			escape->pending = 1;
			continue;
		}

		/* Sends any other byte; a line ends at CR (a raw terminal's Enter) or LF. */
		out[count] = byte;
		count++;
		escape->line_start = 0;
		if (byte == '\r' || byte == '\n')
			escape->line_start = 1;
	}

	/* Reports the bytes to send. */
	*out_size = count;
}

/*
 * Starts the loop's state.
 */
static void
client_session_setup(
	struct client_session *session,
	struct ssh_client *client,
	const struct ssh_client_args *args)
{
	/* Clears every field. */
	memset(session, 0, sizeof(*session));
	session->client = client;

	/* -N opens no session and reads no input; -n reads no input. */
	session->want_session = 1;
	if (args->no_command) {
		session->want_session = 0;
		session->input_end = 1;
	}

	/* -n alone reads no input either. */
	if (args->null_stdin)
		session->input_end = 1;

	/* Starts the two output streams. */
	client_stream_init(&session->output, STDOUT_FILENO);
	client_stream_init(&session->error, STDERR_FILENO);

	/* Starts the escapes at the start of a line. */
	ssh_client_escape_init(&session->escape);
}

/*
 * Frees the loop's state and wipes the user's bytes it still holds.
 */
static void
client_session_finish(
	struct client_session *session)
{
	/* Puts the terminal back, if the loop did not. */
	client_leave_raw(session);

	/* Wipes and frees the buffers. */
	ssh_buf_free(&session->output.queue);
	ssh_buf_free(&session->error.queue);
	ssh_wipe(session->input, sizeof(session->input));
}

/*
 * Opens the engine with what the connection is to do: the user, the
 * identity files, the methods, the session and its terminal, the command
 * and the keepalives.
 */
static int
client_open_engine(
	struct client_session *session,
	const uint8_t *command,
	size_t size)
{
	struct ssh_client *client;
	struct ssh_client_identities identities;
	struct ssh_engine_options options;
	struct ssh_buf modes;
	int terminal;
	int error;

	/* Takes the client of the session. */
	client = session->client;

	/* Reads the identity files the engine offers. */
	error = ssh_client_load_identities(client, &identities);
	if (error != 0) {
		ssh_client_free_identities(&identities);
		return error;
	}

	/* The user and the methods: passwords only when they may be asked. */
	memset(&options, 0, sizeof(options));
	options.user = client->user;
	options.identities = identities.identities;
	options.identity_count = identities.count;
	options.pubkey = client->config.pubkey;
	options.password = 0;
	if (client->config.password && !client->config.batch_mode)
		options.password = 1;

	/* The session, its terminal when one is wanted, and the command (none for the shell). */
	ssh_buf_init(&modes, SSH_TTYMODES_MAX, 0);
	options.session = session->want_session;
	if (session->want_session)
		session->pty = client_wants_pty(session, size);
	options.pty = session->pty;
	if (session->pty) {
		error = client_put_terminal(&options, &modes);
		if (error != 0) {
			ssh_buf_free(&modes);
			ssh_client_free_identities(&identities);
			return error;
		}
	}

	/* The command to run instead of a shell. */
	options.command = command;
	options.command_size = size;

	/* The escapes work only on the user's own terminal. */
	terminal = isatty(STDIN_FILENO);
	if (session->pty && terminal)
		session->escape_on = 1;

	/* ServerAliveInterval and ServerAliveCountMax, and the time now. */
	options.alive_interval = client->config.server_alive_interval;
	options.alive_count = client->config.server_alive_count;
	options.now_ms = ssh_client_now_ms();

	/* Opens the engine, which copies everything; the copies here are wiped. */
	error = ssh_engine_open(&options, &client->engine);
	ssh_buf_free(&modes);
	ssh_client_free_identities(&identities);
	if (error != 0)
		return error;

	/* Succeeded: the engine queued the identification line. */
	return 0;
}

/*
 * Puts the terminal like the user's into options: its type (TERM), its
 * size and its modes (into modes, which options then points at).
 */
static int
client_put_terminal(
	struct ssh_engine_options *options,
	struct ssh_buf *modes)
{
	struct termios settings;
	struct winsize size;
	const char *term;
	size_t term_length;
	int terminal;
	int status;
	int error;

	/* The type of the user's terminal, or "dumb" for one that cannot be sent. */
	term = getenv("TERM");
	term_length = 0;
	if (term != NULL)
		term_length = strlen(term);
	if (term_length == 0 || term_length > SSH_CHANNEL_TERM_MAX)
		term = CLIENT_DEFAULT_TERM;
	options->term = term;

	/* The size and the modes of the user's terminal, when the input is one. */
	memset(&size, 0, sizeof(size));
	terminal = isatty(STDIN_FILENO);
	if (terminal) {
		(void)ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
		status = tcgetattr(STDIN_FILENO, &settings);
		if (status == 0)
			ssh_ttymodes_put(&settings, modes);
	}

	/* Ends the mode list when no mode was put. */
	if (modes->size == 0)
		ssh_buf_put_u8(modes, SSH_TTY_OP_END);
	error = ssh_buf_error(modes);
	if (error != 0)
		return error;

	/* Hands the size and the modes over. */
	options->columns = size.ws_col;
	options->rows = size.ws_row;
	options->width = size.ws_xpixel;
	options->height = size.ws_ypixel;
	options->modes = modes->data;
	options->modes_size = modes->size;

	/* Succeeded: options describe the user's terminal. */
	return 0;
}

/*
 * Decides whether to ask for a terminal (ssh_config(5) RequestTTY): with
 * auto, when no command is given and the standard input is a terminal;
 * with yes, when the standard input is a terminal; with force, always.
 */
static int
client_wants_pty(
	struct client_session *session,
	size_t command_size)
{
	int terminal;

	/* Notes whether the standard input is a terminal. */
	terminal = isatty(STDIN_FILENO);

	/* Applies the setting. */
	switch (session->client->config.request_tty) {
	case SSH_TTY_NO:
		return 0;
	case SSH_TTY_FORCE:
		return 1;
	case SSH_TTY_YES:
		if (!terminal) {
			/* Says so once the login is over, where the session starts. */
			session->tty_note = 1;
			return 0;
		}

		/* Allocates a terminal because stdin is one. */
		return 1;
	default:
		if (command_size == 0 && terminal)
			return 1;
		return 0;
	}
}

/*
 * Runs the poll loop until the connection ends.  Returns 0, or an error
 * of the loop itself (ETIMEDOUT for a server that did not identify itself
 * within ConnectTimeout).
 */
static int
client_session_run(
	struct client_session *session)
{
	struct ssh_client *client;
	struct pollfd slots[CLIENT_SLOTS];
	size_t pending;
	uint64_t now;
	uint64_t wake;
	uint64_t wait_ms;
	int identified;
	int finished;
	int status;
	int error;

	/* Takes the client of the session. */
	client = session->client;

	/* Bounds the wait for the server's identification by ConnectTimeout. */
	session->deadline = 0;
	if (client->config.connect_timeout != 0)
		session->deadline = ssh_client_now_ms() + (uint64_t)client->config.connect_timeout * 1000U;

	/* Turns until the connection ends. */
	for (;;) {
		/* Gives the engine the time: re-exchanges and keepalives start on time. */
		now = ssh_client_now_ms();
		(void)ssh_engine_tick(client->engine, now, &wake);

		/* Takes the bytes read and acts on everything the engine says. */
		error = client_take_events(session);
		if (error != 0)
			return error;
		client_note_rekeys(session);

		/* Gives the engine the user's input it takes. */
		client_send_input(session);

		/* Ends when the connection is over and the session's output written. */
		finished = client_finished(session);
		if (session->done || finished)
			return 0;

		/* Gives up on a server that has not identified itself within ConnectTimeout. */
		wait_ms = CLIENT_SESSION_POLL_MS;
		identified = ssh_engine_identified(client->engine);
		if (session->deadline != 0 &&
		    !identified &&
		    !session->closed) {
			if (now >= session->deadline)
				return ETIMEDOUT;
			if (session->deadline - now < wait_ms)
				wait_ms = session->deadline - now;
		}

		/* Wakes no later than the engine asks. */
		if (wake <= now) {
			wait_ms = 0;
		} else if (wake - now < wait_ms) {
			wait_ms = wake - now;
		}

		/* Watches the socket: for bytes when the last are fed, for room when output waits. */
		(void)ssh_engine_output(client->engine, &pending);
		memset(slots, 0, sizeof(slots));
		slots[CLIENT_SLOT_SOCKET].fd = client->socket;
		if (client->peer_closed)
			slots[CLIENT_SLOT_SOCKET].fd = -1;
		if (client->receive_size == 0 && !session->closed)
			slots[CLIENT_SLOT_SOCKET].events |= POLLIN;
		if (pending > 0)
			slots[CLIENT_SLOT_SOCKET].events |= POLLOUT;

		/* Watches the standard input while the session takes input and the output is not piled up. */
		slots[CLIENT_SLOT_INPUT].fd = -1;
		if (session->started &&
		    !session->closed &&
		    !session->input_end &&
		    session->input_size == 0 &&
		    pending < SSH_CLIENT_OUTPUT_HIGH) {
			slots[CLIENT_SLOT_INPUT].fd = STDIN_FILENO;
			slots[CLIENT_SLOT_INPUT].events = POLLIN;
		}

		/* Watches the output stream when data is queued. */
		slots[CLIENT_SLOT_OUTPUT].fd = -1;
		if (session->output.queue.size > session->output.offset) {
			slots[CLIENT_SLOT_OUTPUT].fd = session->output.descriptor;
			slots[CLIENT_SLOT_OUTPUT].events = POLLOUT;
		}

		/* Watches the error stream when data is queued. */
		slots[CLIENT_SLOT_ERROR].fd = -1;
		if (session->error.queue.size > session->error.offset) {
			slots[CLIENT_SLOT_ERROR].fd = session->error.descriptor;
			slots[CLIENT_SLOT_ERROR].events = POLLOUT;
		}

		/* Watches the signal pipe once the signals are caught. */
		slots[CLIENT_SLOT_SIGNAL].fd = client_signal_pipe[0];
		slots[CLIENT_SLOT_SIGNAL].events = POLLIN;

		/* Waits for any of them, at most until the engine or the deadline wants the time. */
		status = poll(slots, CLIENT_SLOTS, (int)wait_ms);
		if (status < 0 && errno != EINTR)
			return errno;

		/* Starts again after an interrupt. */
		if (status < 0)
			continue;

		/* Handles the signals that came. */
		if ((slots[CLIENT_SLOT_SIGNAL].revents & POLLIN) != 0) {
			client_handle_signals(session);
			if (session->done)
				return 0;
		}

		/* Writes and reads the socket. */
		if ((slots[CLIENT_SLOT_SOCKET].revents & POLLOUT) != 0)
			(void)ssh_client_write_socket(client);
		if ((slots[CLIENT_SLOT_SOCKET].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			(void)ssh_client_read_socket(client);

		/* Reads the user's input. */
		if ((slots[CLIENT_SLOT_INPUT].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			client_read_input(session);
			if (session->done)
				return 0;
		}

		/* Writes the output stream when it can take data or failed. */
		if ((slots[CLIENT_SLOT_OUTPUT].revents & (POLLOUT | POLLHUP | POLLERR)) != 0)
			client_write_stream(session, &session->output);

		/* Writes the error stream when it can take data or failed. */
		if ((slots[CLIENT_SLOT_ERROR].revents & (POLLOUT | POLLHUP | POLLERR)) != 0)
			client_write_stream(session, &session->error);
	}
}

/*
 * Feeds the engine the bytes read and acts on its events until it has
 * nothing more to say.  Returns 0 or an error of the program.
 */
static int
client_take_events(
	struct client_session *session)
{
	struct ssh_engine_event event;
	int fed;
	int took;
	int status;
	int error;

	/* Alternates feeding and taking events while either moves. */
	for (;;) {
		error = ssh_client_feed(session->client, &fed);
		if (error != 0)
			return error;

		/* Takes every event the engine has now. */
		took = 0;
		for (;;) {
			status = ssh_engine_next(session->client->engine, &event);
			if (status != 0)
				break;
			took = 1;
			error = client_take_event(session, &event);
			if (error != 0)
				return error;
			if (session->done)
				return 0;
		}

		/* Stops when nothing was fed and nothing came. */
		if (!fed && !took)
			return 0;
	}
}

/*
 * Acts on one event of the engine.  Returns 0 or an error of the
 * program.
 */
static int
client_take_event(
	struct client_session *session,
	const struct ssh_engine_event *event)
{
	struct ssh_client *client;
	int error;

	/* Takes the client of the session. */
	client = session->client;

	/* Acts by the kind of the event. */
	error = 0;
	switch (event->kind) {
	case SSH_ENGINE_HOST_KEY:
		client_take_host_key(session, event);
		break;
	case SSH_ENGINE_BANNER:
		/* Shows the banner (made safe for the terminal) unless the user asked for quiet. */
		if (client->log_level >= SSH_CLIENT_LOG_INFO) {
			fwrite(event->bytes, 1, event->size, stderr);
			fflush(stderr);
		}

		break;
	case SSH_ENGINE_PASSWORD:
		error = client_take_password(session, event);
		break;
	case SSH_ENGINE_PASSPHRASE:
		/* Asks the passphrase of the encrypted key the server would take. */
		error = ssh_client_answer_passphrase(client, event->identity);
		break;
	case SSH_ENGINE_AUTHENTICATED:
		error = client_take_login(session);
		break;
	case SSH_ENGINE_STARTED:
		client_take_start(session, event);
		break;
	case SSH_ENGINE_DATA:
		error = client_take_data(session, event);
		break;
	case SSH_ENGINE_EXIT:
		client_take_exit(session, event);
		break;
	case SSH_ENGINE_CLOSED:
		client_take_closed(session, event);
		break;
	default:
		/* Passes over an event of no use to the program. */
		break;
	}

	/* Reports an error that ends the program's loop. */
	if (error != 0)
		return error;

	/* Succeeded: the event is handled. */
	return 0;
}

/*
 * Checks the host key the engine shows and answers it.
 */
static void
client_take_host_key(
	struct client_session *session,
	const struct ssh_engine_event *event)
{
	struct ssh_client *client;
	int accept;
	int error;

	/* Takes the client of the session. */
	client = session->client;

	/* Checks the key against known_hosts and the settings, asking the user when they say so. */
	error = ssh_client_check_host_key(client, event->bytes, event->size);
	if (error != 0 && error != EACCES) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: the host key could not be checked: %s", strerror(error));
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Host key verification failed.");
	}

	/* Accepts the key that passed; a refused one ends the connection, its reason already said. */
	accept = 0;
	if (error == 0) {
		accept = 1;
		ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: authenticating as %s", client->user);
	} else {
		session->host_refused = 1;
	}

	/* Answers the engine. */
	(void)ssh_engine_host_answer(client->engine, accept);
}

/*
 * Answers the engine's question for a password.
 */
static int
client_take_password(
	struct client_session *session,
	const struct ssh_engine_event *event)
{
	int sent;
	int error;

	/* The question answers the last password, if one was out. */
	session->password_out = 0;

	/* Asks the user and gives the engine the answer. */
	error = ssh_client_answer_password(session->client, event->tries, &sent);
	if (error != 0)
		return error;

	/* Notes a password that went, so that its answer can be told. */
	if (sent) {
		session->password_out = 1;
		session->passwords_sent++;
	}

	/* Succeeded: the question is answered. */
	return 0;
}

/*
 * Takes the end of the login: the signals the session handles are caught,
 * and the terminal goes to raw mode for a session on a terminal.
 */
static int
client_take_login(
	struct client_session *session)
{
	struct ssh_client *client;
	int error;

	/* Takes the client of the session. */
	client = session->client;

	/* Notes the login. */
	session->authenticated = 1;
	session->password_out = 0;
	ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: authenticated to %s port %u", client->host_name, (unsigned)client->port);

	/* Makes the signal pipe and catches the signals the loop handles. */
	error = client_signals_start();
	if (error != 0)
		return error;

	/* Says that no terminal is asked for although -t asked for one. */
	if (session->tty_note)
		ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Pseudo-terminal will not be allocated because stdin is not a terminal.");

	/* Every key goes to the remote terminal as it is typed from now on. */
	if (session->pty)
		client_enter_raw(session);

	/* Succeeded: the session may start. */
	return 0;
}

/*
 * Takes the start of the shell or the command: input may go now.
 */
static void
client_take_start(
	struct client_session *session,
	const struct ssh_engine_event *event)
{
	/* The user's input goes from now on. */
	session->started = 1;

	/* A refused terminal is reported, and the session goes on without one. */
	if (session->pty && !event->pty)
		ssh_client_log(session->client, SSH_CLIENT_LOG_QUIET, "PTY allocation request failed on channel %lu", CLIENT_CHANNEL);
}

/*
 * Queues the session's data for the stream it belongs to.  Returns 0 or
 * the error of a queue that could not hold it.
 */
static int
client_take_data(
	struct client_session *session,
	const struct ssh_engine_event *event)
{
	struct client_stream *stream;
	int error;

	/* Chooses the stream: the output, or the error output. */
	stream = &session->output;
	if (event->stream == 1)
		stream = &session->error;

	/* Drops data for a stream that cannot be written, giving its window back. */
	if (stream->broken) {
		ssh_engine_consumed(session->client->engine, event->size);
		return 0;
	}

	/* Queues the bytes; the window is given back as they are written. */
	ssh_buf_put_bytes(&stream->queue, event->bytes, event->size);
	error = ssh_buf_error(&stream->queue);
	if (error != 0)
		return error;

	/* Succeeded: the data waits to be written. */
	return 0;
}

/*
 * Takes how the session ended: the command's status or signal, or why
 * the shell or the command did not start.
 */
static void
client_take_exit(
	struct client_session *session,
	const struct ssh_engine_event *event)
{
	/* Keeps the signal that ended the command, or its status. */
	if (event->exit_signal[0] != '\0') {
		session->have_exit = 1;
		session->killed = 1;
		ssh_client_log(session->client, SSH_CLIENT_LOG_VERBOSE, "ssh: the remote command ended by signal %s", event->exit_signal);
	} else if (event->exit_status >= 0) {
		session->have_exit = 1;
		session->exit_status = event->exit_status;
	}

	/* A refused shell or command ends the session with 255. */
	if (event->reason[0] != '\0') {
		ssh_client_log(session->client, SSH_CLIENT_LOG_QUIET, "%s on channel %lu", event->reason, CLIENT_CHANNEL);
		session->failed = 1;
	}
}

/*
 * Takes the end of the connection and says why it ended, as the program
 * words each way it can end.
 */
static void
client_take_closed(
	struct client_session *session,
	const struct ssh_engine_event *event)
{
	struct ssh_client *client;
	const char *reason;
	int compare;

	/* Takes the client of the session. */
	client = session->client;
	session->closed = 1;

	/* An end this side asked for (after the session, or at the user's word) says nothing. */
	if (event->error == 0)
		return;

	/* Any other end stops the loop at once, with 255. */
	session->done = 1;
	session->failed = 1;

	/* A refused host key was reported when it was refused. */
	if (session->host_refused)
		return;

	/* No method was left: tells how the last password was answered, then the methods. */
	if (!session->authenticated &&
	    event->error == EACCES &&
	    event->code == SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE) {
		if (session->password_out && event->tries >= session->passwords_sent) {
			ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Permission denied, please try again.");
		} else if (session->password_out) {
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "ssh: the server asks for a new password, which is not supported");
		}

		/* Names the methods the server offered. */
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "%s@%s: %s", client->user, client->host_name, event->reason);
		return;
	}

	/* Before the session, any other end is reported in the words of the connection. */
	if (!session->authenticated) {
		ssh_client_report(client, event->error, event->code, event->reason);
		session->reported = 1;
		return;
	}

	/* Puts the terminal back before the message. */
	client_leave_raw(session);

	/* A server that answered none of the keepalives allowed. */
	if (event->error == ETIMEDOUT) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Timeout, server %s not responding.", client->host_name);
		return;
	}

	/* A refused session channel, named by its reason. */
	if (event->error == ECONNREFUSED) {
		reason = event->reason;
		compare = strncmp(reason, CLIENT_OPEN_FAILED, sizeof(CLIENT_OPEN_FAILED) - 1U);
		if (compare == 0)
			reason += sizeof(CLIENT_OPEN_FAILED) - 1U;
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "channel %lu: open failed: %s", CLIENT_CHANNEL, reason);
		return;
	}

	/* A connection the server closed under the session. */
	if (event->error == ECONNRESET) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Connection to %s closed by remote host.", client->host_name);
		session->pty = 0;
		return;
	}

	/* Any other end is reported in the words of the connection. */
	ssh_client_report(client, event->error, event->code, event->reason);
	session->reported = 1;
}

/*
 * Tells, at the debug level, of each re-exchange that showed the first
 * exchange's host key again.
 */
static void
client_note_rekeys(
	struct client_session *session)
{
	struct ssh_client *client;
	unsigned long rekeys;

	/* Takes the count the engine keeps. */
	client = session->client;
	rekeys = ssh_engine_rekeys(client->engine);

	/* Tells of each re-exchange not yet told. */
	while (client->rekeys < rekeys) {
		ssh_client_log(client, SSH_CLIENT_LOG_DEBUG, "ssh: key re-exchange: the host key checked again is the same");
		client->rekeys++;
	}
}

/*
 * Makes the signal pipe and installs the handlers: SIGWINCH (the window
 * size changed), SIGINT, SIGTERM and SIGHUP (end the program).
 */
static int
client_signals_start(void)
{
	struct sigaction action;
	int status;
	int index;
	int flags;

	/* Makes the pipe once; neither end blocks, and a program the session starts does not get them. */
	if (client_signal_pipe[0] < 0) {
		status = pipe(client_signal_pipe);
		if (status != 0)
			return errno;
		for (index = 0; index < 2; index++) {
			(void)fcntl(client_signal_pipe[index], F_SETFD, FD_CLOEXEC);
			flags = fcntl(client_signal_pipe[index], F_GETFL);
			if (flags >= 0)
				(void)fcntl(client_signal_pipe[index], F_SETFL, flags | O_NONBLOCK);
		}
	}

	/* Installs the handler for each signal the loop handles. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = client_signal_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	(void)sigaction(SIGWINCH, &action, NULL);
	(void)sigaction(SIGINT, &action, NULL);
	(void)sigaction(SIGTERM, &action, NULL);
	(void)sigaction(SIGHUP, &action, NULL);

	/* Succeeded: signals now wake the loop. */
	return 0;
}

/*
 * Writes the signal's number into the signal pipe (a full pipe drops it:
 * the loop has a wake-up waiting already).
 */
static void
client_signal_handler(
	int number)
{
	unsigned char byte;
	ssize_t written;
	int saved;

	/* Keeps errno for the code the signal interrupted. */
	saved = errno;
	byte = (unsigned char)number;
	written = write(client_signal_pipe[1], &byte, 1);
	(void)written;
	errno = saved;
}

/*
 * Handles the signals in the pipe: a window change is sent to the
 * server, an ending signal ends the program.
 */
static void
client_handle_signals(
	struct client_session *session)
{
	unsigned char numbers[64];
	ssize_t count;
	ssize_t index;

	/* Reads what the handler wrote. */
	count = read(client_signal_pipe[0], numbers, sizeof(numbers));
	if (count <= 0)
		return;

	/* Acts on each signal. */
	for (index = 0; index < count; index++) {
		/* Tells the server the new window size. */
		if (numbers[index] == SIGWINCH) {
			client_send_window(session);
			continue;
		}

		/* Ends the program on any other caught signal. */
		client_leave_raw(session);
		ssh_client_log(session->client, SSH_CLIENT_LOG_QUIET, "Killed by signal %d.", (int)numbers[index]);
		client_quit(session);
		return;
	}
}

/*
 * Gives the engine the terminal's new size (RFC 4254 6.7), which it sends
 * once the session has its terminal.
 */
static void
client_send_window(
	struct client_session *session)
{
	struct winsize size;
	int status;

	/* Only a session with a terminal has a window to change. */
	if (!session->pty)
		return;

	/* Reads the size of the user's terminal. */
	memset(&size, 0, sizeof(size));
	status = ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
	if (status != 0)
		return;

	/* Gives it to the engine (after the end it goes nowhere). */
	(void)ssh_engine_resize(session->client->engine, size.ws_col, size.ws_row, size.ws_xpixel, size.ws_ypixel);
}

/*
 * Reads the user's input into session->input, through the escapes.
 */
static void
client_read_input(
	struct client_session *session)
{
	uint8_t chunk[SSH_CLIENT_INPUT_CHUNK];
	ssize_t count;
	size_t size;
	int disconnect;

	/* Reads what is there. */
	count = read(STDIN_FILENO, chunk, sizeof(chunk));
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return;

	/* Notes the end of the input (an error ends it too). */
	if (count <= 0) {
		session->input_end = 1;
		return;
	}

	/* Passes the bytes through the escapes, or takes them as they are. */
	disconnect = 0;
	if (session->escape_on) {
		ssh_client_escape_filter(&session->escape, chunk, (size_t)count, session->input, &size, &disconnect);
	} else {
		memcpy(session->input, chunk, (size_t)count);
		size = (size_t)count;
	}

	/* Wipes the chunk that was read. */
	ssh_wipe(chunk, sizeof(chunk));

	/* "~." ends the connection at once; the bytes before it are not sent. */
	if (disconnect) {
		client_leave_raw(session);
		ssh_client_log(session->client, SSH_CLIENT_LOG_INFO, "Connection to %s closed.", session->client->host_name);
		session->pty = 0;
		ssh_wipe(session->input, size);
		client_quit(session);
		return;
	}

	/* Keeps the bytes until the engine takes them. */
	session->input_size = size;
	session->input_offset = 0;
}

/*
 * Gives the engine the user's input it takes now, and the end of the
 * input once every byte before it went.
 */
static void
client_send_input(
	struct client_session *session)
{
	struct ssh_engine *engine;
	size_t written;
	int error;

	/* Takes the engine of the connection. */
	engine = session->client->engine;

	/* Gives the bytes while the engine takes them (it takes none before the session runs or while the window is full). */
	while (session->input_offset < session->input_size) {
		error = ssh_engine_write(engine, session->input + session->input_offset, session->input_size - session->input_offset, &written);
		if (error != 0) {
			/* The connection ended: the input goes nowhere. */
			session->input_offset = session->input_size;
			break;
		}

		/* Waits for room when the engine took nothing. */
		if (written == 0)
			return;
		session->input_offset += written;
	}

	/* Forgets the input that went. */
	ssh_wipe(session->input, session->input_size);
	session->input_size = 0;
	session->input_offset = 0;

	/* Tells the engine once that the input ended, after its last byte. */
	if (session->input_end && !session->eof_given) {
		(void)ssh_engine_eof(engine);
		session->eof_given = 1;
	}
}

/*
 * Writes the next piece of a stream's queue and gives the window back for
 * what was written.
 */
static void
client_write_stream(
	struct client_session *session,
	struct client_stream *stream)
{
	ssize_t count;
	size_t size;

	/* Writes at most one piece that fits without blocking. */
	size = stream->queue.size - stream->offset;
	if (size > stream->chunk)
		size = stream->chunk;
	count = write(stream->descriptor, stream->queue.data + stream->offset, size);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return;

	/* A stream that cannot be written drops everything from now on. */
	if (count <= 0) {
		stream->broken = 1;
		count = (ssize_t)(stream->queue.size - stream->offset);
	}

	/* Steps over what was written (or dropped), emptying the queue when all of it is, or every half window. */
	stream->offset += (size_t)count;
	if (stream->offset == stream->queue.size) {
		ssh_buf_reset(&stream->queue);
		stream->offset = 0;
	} else if (stream->offset >= SSH_CHANNEL_WINDOW / 2U) {
		ssh_buf_consume(&stream->queue, stream->offset);
		stream->offset = 0;
	}

	/* Gives the window back for what was written (or dropped). */
	ssh_engine_consumed(session->client->engine, (size_t)count);
}

/*
 * Puts the user's terminal in raw mode, so that every key goes to the
 * remote terminal as it is typed (the remote side echoes and edits).
 */
static void
client_enter_raw(
	struct client_session *session)
{
	struct termios raw;
	int terminal;
	int status;

	/* Only the user's own terminal is changed, and only once. */
	terminal = isatty(STDIN_FILENO);
	if (session->raw || !terminal)
		return;
	status = tcgetattr(STDIN_FILENO, &session->saved);
	if (status != 0)
		return;

	/* Turns off line editing, echo, signals from keys, flow control and output processing. */
	raw = session->saved;
	raw.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF);
	raw.c_iflag |= IGNPAR;
	raw.c_lflag &= ~(tcflag_t)(ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL | IEXTEN);
	raw.c_oflag &= ~(tcflag_t)OPOST;
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	status = tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
	if (status != 0)
		return;

	/* Messages now end with CR LF until the terminal is put back. */
	session->raw = 1;
	session->client->raw = 1;
}

/*
 * Puts the user's terminal back as it was.
 */
static void
client_leave_raw(
	struct client_session *session)
{
	/* Does nothing when the terminal was not changed. */
	if (!session->raw)
		return;

	/* Restores the saved settings. */
	(void)tcsetattr(STDIN_FILENO, TCSADRAIN, &session->saved);
	session->raw = 0;
	session->client->raw = 0;
}

/*
 * Starts an output stream on a descriptor: a regular file takes large
 * writes, anything else pieces poll guarantees to fit.
 */
static void
client_stream_init(
	struct client_stream *stream,
	int descriptor)
{
	struct stat information;
	int status;

	/* Starts an empty queue (the server's data may be secret to the user). */
	ssh_buf_init(&stream->queue, SSH_CLIENT_STREAM_MAX, 1);
	stream->offset = 0;
	stream->descriptor = descriptor;
	stream->broken = 0;

	/* Chooses the size of one write. */
	stream->chunk = SSH_CLIENT_STREAM_CHUNK;
	status = fstat(descriptor, &information);
	if (status == 0 && S_ISREG(information.st_mode))
		stream->chunk = SSH_CLIENT_FILE_CHUNK;
}

/*
 * Reports whether the session is over: the connection ended and every
 * byte of the output written (or dropped).
 */
static int
client_finished(
	const struct client_session *session)
{
	/* The connection must have ended. */
	if (!session->closed)
		return 0;

	/* The output must be written (or dropped). */
	if (session->output.queue.size > session->output.offset && !session->output.broken)
		return 0;
	if (session->error.queue.size > session->error.offset && !session->error.broken)
		return 0;

	/* Reports that the session is over. */
	return 1;
}

/*
 * Ends the connection at the user's or a signal's word: the exit value is
 * 255, and the engine queues the DISCONNECT.
 */
static void
client_quit(
	struct client_session *session)
{
	/* Stops the loop with 255. */
	session->quit = 1;
	session->done = 1;

	/* Tells the server. */
	ssh_engine_disconnect(session->client->engine, NULL);
}
