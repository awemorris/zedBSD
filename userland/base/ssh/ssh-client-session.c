/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session of the ssh program (RFC 4254 6): one "session" channel
 * with a pseudo-terminal when one is wanted, the shell or the command,
 * and the standard streams carried over it until the channel closes;
 * the escape sequences of the terminal (ssh(1) ESCAPE CHARACTERS), the
 * window size, ServerAliveInterval (ssh_config(5)), and the exit status.
 *
 * Everything runs in one poll loop.  The socket does not block; the
 * standard streams keep their mode (they may be shared with other
 * processes) and are written in pieces poll says fit.
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

/* The longest wait of one poll, so that re-exchanges and keepalives start on time. */
#define CLIENT_SESSION_POLL_MS	1000

/* The pollfd slots of the loop. */
#define CLIENT_SLOT_SOCKET	0
#define CLIENT_SLOT_INPUT	1
#define CLIENT_SLOT_OUTPUT	2
#define CLIENT_SLOT_ERROR	3
#define CLIENT_SLOT_SIGNAL	4
#define CLIENT_SLOTS		5

/* The most channel requests waiting for their reply (pty-req and shell or exec). */
#define CLIENT_REPLIES_MAX	4U

/* The terminal type sent when TERM does not give one. */
#define CLIENT_DEFAULT_TERM	"dumb"

/* The global request of ServerAliveInterval (OpenSSH PROTOCOL; any name the server refuses works). */
#define CLIENT_KEEPALIVE	"keepalive@openssh.com"

/* What a channel request waiting for its reply was. */
enum client_reply_kind {
	CLIENT_REPLY_PTY = 0,
	CLIENT_REPLY_START = 1
};

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
 * The state of the session loop.
 *
 * channel is the session channel (NULL before it is opened when
 * no_session is 0, and after its close went both ways).  out builds one
 * message at a time.  input holds the user's bytes read and passed
 * through the escapes but not yet sent (input_size of them, from
 * input_offset).  input_end is 1 once the standard input reached its end
 * (or -n, or "~.").  pty is 1 when a terminal is asked for, raw when the
 * local terminal is in raw mode (saved holds its settings), escape_on when
 * the escapes are read.  replies are the kinds of the channel requests
 * waiting for a reply, in order.  exit_status is the status the server
 * sent (have_exit 1), killed 1 when the command ended by a signal.
 * failed is 1 when the session failed (the exit value is 255).  done is 1
 * when the loop is to end, quit 1 when it ends at the user's or a
 * signal's word.  alive_due is when the next keepalive is due and
 * alive_sent how many went unanswered.  unacknowledged counts the
 * received bytes written (or dropped) that the window has not been
 * given back for yet: the window is given back only while less than
 * half a window waits in the queues, so the queues stay bounded however
 * slowly the streams are written.
 */
struct client_session {
	struct ssh_client *client;
	int no_session;
	struct ssh_channels channels;
	struct ssh_channel *channel;
	struct ssh_buf out;
	struct client_stream output;
	struct client_stream error;
	uint8_t input[SSH_CLIENT_INPUT_CHUNK + 1];
	size_t input_size;
	size_t input_offset;
	int input_end;
	int input_eof_sent;
	int pty;
	int raw;
	struct termios saved;
	int escape_on;
	struct ssh_client_escape escape;
	enum client_reply_kind replies[CLIENT_REPLIES_MAX];
	unsigned reply_count;
	const uint8_t *command;
	size_t command_size;
	int have_exit;
	uint32_t exit_status;
	int killed;
	int failed;
	int done;
	int quit;
	uint64_t alive_due;
	unsigned alive_sent;
	size_t unacknowledged;
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

static int client_session_run(struct client_session *session);
static void client_session_setup(struct client_session *session, struct ssh_client *client, int no_session);
static void client_session_finish(struct client_session *session);
static int client_wants_pty(const struct ssh_client *client, size_t command_size);
static int client_signals_start(void);
static void client_signal_handler(int number);
static int client_handle_signals(struct client_session *session);
static int client_dispatch(struct client_session *session);
static int client_opened(struct client_session *session);
static int client_take_data(struct client_session *session);
static int client_take_end(struct client_session *session);
static int client_take_request(struct client_session *session);
static int client_take_reply(struct client_session *session);
static int client_refuse_open(struct client_session *session);
static int client_send_out(struct client_session *session);
static int client_read_input(struct client_session *session);
static int client_send_input(struct client_session *session);
static int client_write_stream(struct client_session *session, struct client_stream *stream);
static int client_keep_alive(struct client_session *session, uint64_t now);
static int client_send_window(struct client_session *session);
static void client_enter_raw(struct client_session *session);
static void client_leave_raw(struct client_session *session);
static void client_stream_init(struct client_stream *stream, int descriptor);
static int client_finished(const struct client_session *session);
static const char *client_open_reason(uint32_t reason);
static int client_give_window(struct client_session *session);

/*
 * Runs the session channel.
 */
int
ssh_client_run_session(
	struct ssh_client *client,
	const struct ssh_client_args *args,
	const uint8_t *command,
	size_t size,
	int *exit_status)
{
	struct client_session *session;
	int terminal;
	int error;

	/* Allocates the loop's state, which is large for the stack. */
	session = malloc(sizeof(*session));
	if (session == NULL)
		return ENOMEM;
	client_session_setup(session, client, 0);
	session->command = command;
	session->command_size = size;
	if (args->null_stdin)
		session->input_end = 1;

	/* Decides whether to ask for a terminal; the escapes work only on the user's own terminal. */
	session->pty = client_wants_pty(client, size);
	terminal = isatty(STDIN_FILENO);
	if (session->pty && terminal)
		session->escape_on = 1;

	/* Runs the loop. */
	error = client_session_run(session);

	/* Takes the exit value: the command's status, or 255 for a signal, a failure or no status. */
	*exit_status = SSH_CLIENT_EXIT_FAILURE;
	if (error == 0 &&
	    session->have_exit &&
	    !session->killed &&
	    !session->failed &&
	    !session->quit)
		*exit_status = (int)(session->exit_status & 0xffU);
	client_session_finish(session);
	free(session);

	/* Reports an error that ended the connection. */
	if (error != 0)
		return error;

	/* Succeeded: the session ended and *exit_status is set. */
	return 0;
}

/*
 * Keeps the connection without a session.
 */
int
ssh_client_run_without_session(
	struct ssh_client *client,
	int *exit_status)
{
	struct client_session *session;
	int error;

	/* Allocates the loop's state. */
	session = malloc(sizeof(*session));
	if (session == NULL)
		return ENOMEM;
	client_session_setup(session, client, 1);
	session->input_end = 1;

	/* Runs the loop until the connection or a signal ends it. */
	error = client_session_run(session);
	client_session_finish(session);
	free(session);

	/* Ends with 255: the connection was cut, not finished by a command. */
	*exit_status = SSH_CLIENT_EXIT_FAILURE;
	if (error != 0)
		return error;

	/* Succeeded: the loop ended. */
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
 * Runs the poll loop until the session or the connection ends.
 */
static int
client_session_run(
	struct client_session *session)
{
	struct ssh_client *client;
	struct pollfd slots[CLIENT_SLOTS];
	size_t pending;
	size_t room;
	uint64_t now;
	int finished;
	int status;
	int error;

	client = session->client;

	/* Makes the signal pipe and catches the signals the loop handles. */
	error = client_signals_start();
	if (error != 0)
		return error;

	/* Opens the session channel. */
	if (!session->no_session) {
		ssh_buf_reset(&session->out);
		error = ssh_channel_open_session(&session->channels, &session->out, &session->channel);
		if (error == 0)
			error = client_send_out(session);
		if (error != 0)
			return error;
	}

	/* The first keepalive is due one interval after the session starts. */
	now = ssh_client_now_ms();
	session->alive_due = now + (uint64_t)client->config.server_alive_interval * 1000U;

	/* Turns until the session ends. */
	for (;;) {
		/* Moves waiting messages into the transport. */
		error = ssh_client_flush(client);
		if (error != 0)
			break;

		/* Takes every message that has come. */
		for (;;) {
			error = ssh_client_receive(client);
			if (error != 0)
				break;

			/* Any message shows the server is alive. */
			session->alive_sent = 0;
			session->alive_due = ssh_client_now_ms() + (uint64_t)client->config.server_alive_interval * 1000U;
			error = client_dispatch(session);
			if (error != 0)
				break;
		}
		if (error == EAGAIN)
			error = 0;
		if (error != 0)
			break;

		/* Sends the user's input the window has room for. */
		error = client_send_input(session);
		if (error != 0)
			break;

		/* Ends when the session is over and its output written. */
		finished = client_finished(session);
		if (session->done || finished)
			break;

		/* Notes a connection the server closed under a session that was not over. */
		if (client->peer_closed && (session->channel != NULL || session->no_session)) {
			client_leave_raw(session);
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Connection to %s closed by remote host.", client->host_name);
			session->failed = 1;
			session->pty = 0;
			break;
		}

		/* Gives the transport the time and sends a keepalive when one is due. */
		now = ssh_client_now_ms();
		error = ssh_transport_tick(&client->transport, now / 1000U);
		if (error != 0)
			break;
		error = client_keep_alive(session, now);
		if (error != 0)
			break;
		if (session->done)
			break;

		/* Watches the socket: for bytes when the last are fed, for room when output waits. */
		(void)ssh_transport_pending(&client->transport, &pending);
		memset(slots, 0, sizeof(slots));
		slots[CLIENT_SLOT_SOCKET].fd = client->socket;
		if (client->peer_closed)
			slots[CLIENT_SLOT_SOCKET].fd = -1;
		if (client->receive_size == 0)
			slots[CLIENT_SLOT_SOCKET].events |= POLLIN;
		if (pending > 0)
			slots[CLIENT_SLOT_SOCKET].events |= POLLOUT;

		/* Watches the standard input when its bytes can go out soon. */
		slots[CLIENT_SLOT_INPUT].fd = -1;
		room = 0;
		if (session->channel != NULL)
			room = ssh_channel_send_room(session->channel);
		if (!session->input_end &&
		    session->input_size == 0 &&
		    room > 0 &&
		    client->backlog.size == 0 &&
		    pending < SSH_CLIENT_OUTPUT_HIGH) {
			slots[CLIENT_SLOT_INPUT].fd = STDIN_FILENO;
			slots[CLIENT_SLOT_INPUT].events = POLLIN;
		}

		/* Watches the output streams that have bytes waiting. */
		slots[CLIENT_SLOT_OUTPUT].fd = -1;
		if (session->output.queue.size > session->output.offset) {
			slots[CLIENT_SLOT_OUTPUT].fd = session->output.descriptor;
			slots[CLIENT_SLOT_OUTPUT].events = POLLOUT;
		}
		slots[CLIENT_SLOT_ERROR].fd = -1;
		if (session->error.queue.size > session->error.offset) {
			slots[CLIENT_SLOT_ERROR].fd = session->error.descriptor;
			slots[CLIENT_SLOT_ERROR].events = POLLOUT;
		}

		/* Watches the signal pipe. */
		slots[CLIENT_SLOT_SIGNAL].fd = client_signal_pipe[0];
		slots[CLIENT_SLOT_SIGNAL].events = POLLIN;

		/* Waits for any of them, at most until the next time check. */
		status = poll(slots, CLIENT_SLOTS, CLIENT_SESSION_POLL_MS);
		if (status < 0 && errno != EINTR) {
			error = errno;
			break;
		}
		if (status < 0)
			continue;

		/* Handles the signals that came. */
		if ((slots[CLIENT_SLOT_SIGNAL].revents & POLLIN) != 0) {
			error = client_handle_signals(session);
			if (error != 0)
				break;
			if (session->done)
				break;
		}

		/* Writes and reads the socket. */
		if ((slots[CLIENT_SLOT_SOCKET].revents & POLLOUT) != 0)
			(void)ssh_client_write_socket(client);
		if ((slots[CLIENT_SLOT_SOCKET].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			(void)ssh_client_read_socket(client);

		/* Reads the user's input. */
		if ((slots[CLIENT_SLOT_INPUT].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			error = client_read_input(session);
			if (error != 0)
				break;
			if (session->done)
				break;
		}

		/* Writes the streams. */
		if ((slots[CLIENT_SLOT_OUTPUT].revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
			error = client_write_stream(session, &session->output);
			if (error != 0)
				break;
		}
		if ((slots[CLIENT_SLOT_ERROR].revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
			error = client_write_stream(session, &session->error);
			if (error != 0)
				break;
		}
	}

	/* Puts the terminal back before any message, so that the message reads well. */
	client_leave_raw(session);

	/* Reports the error that ended the connection. */
	if (error != 0) {
		ssh_client_report(client, error);
		return error;
	}

	/* Tells the user the connection is over when it ran on a terminal. */
	if (session->pty)
		ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Connection to %s closed.", client->host_name);

	/* Succeeded: the session is over. */
	return 0;
}

/*
 * Starts the loop's state.
 */
static void
client_session_setup(
	struct client_session *session,
	struct ssh_client *client,
	int no_session)
{
	/* Clears every field. */
	memset(session, 0, sizeof(*session));
	session->client = client;
	session->no_session = no_session;

	/* Starts an empty channel table. */
	ssh_channels_init(&session->channels);

	/* The message being built may hold the user's input: secret. */
	ssh_buf_init(&session->out, SSH_PACKET_LIMIT, 1);

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
	ssh_buf_free(&session->out);
	ssh_buf_free(&session->output.queue);
	ssh_buf_free(&session->error.queue);
	ssh_wipe(session->input, sizeof(session->input));
}

/*
 * Decides whether to ask for a terminal (ssh_config(5) RequestTTY): with
 * auto, when no command is given and the standard input is a terminal;
 * with yes, when the standard input is a terminal; with force, always.
 */
static int
client_wants_pty(
	const struct ssh_client *client,
	size_t command_size)
{
	int terminal;

	/* Notes whether the standard input is a terminal. */
	terminal = isatty(STDIN_FILENO);

	/* Applies the setting. */
	switch (client->config.request_tty) {
	case SSH_TTY_NO:
		return 0;
	case SSH_TTY_FORCE:
		return 1;
	case SSH_TTY_YES:
		if (!terminal) {
			ssh_client_log(client, SSH_CLIENT_LOG_INFO, "Pseudo-terminal will not be allocated because stdin is not a terminal.");
			return 0;
		}
		return 1;
	default:
		if (command_size == 0 && terminal)
			return 1;
		return 0;
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
static int
client_handle_signals(
	struct client_session *session)
{
	unsigned char numbers[64];
	ssize_t count;
	ssize_t index;
	int error;

	/* Reads what the handler wrote. */
	count = read(client_signal_pipe[0], numbers, sizeof(numbers));
	if (count <= 0)
		return 0;

	/* Acts on each signal. */
	for (index = 0; index < count; index++) {
		/* Tells the server the new window size. */
		if (numbers[index] == SIGWINCH) {
			error = client_send_window(session);
			if (error != 0)
				return error;
			continue;
		}

		/* Ends the program on any other caught signal. */
		client_leave_raw(session);
		ssh_client_log(session->client, SSH_CLIENT_LOG_QUIET, "Killed by signal %d.", (int)numbers[index]);
		session->quit = 1;
		session->done = 1;
		return 0;
	}

	/* Succeeded: the signals are handled. */
	return 0;
}

/*
 * Handles one message from the server (in client->payload).
 */
static int
client_dispatch(
	struct client_session *session)
{
	struct ssh_client *client;
	char name[SSH_NAME_MAX + 1];
	struct ssh_channel *channel;
	uint32_t reason;
	int want_reply;
	int error;

	client = session->client;

	/* Acts by the message number. */
	error = 0;
	switch (client->payload.data[0]) {
	case SSH_MSG_GLOBAL_REQUEST:
		/* Refuses every global request the server makes (keepalives get their FAILURE). */
		ssh_buf_reset(&session->out);
		error = ssh_channel_global_request(&session->channels, client->payload.data, client->payload.size, name, &want_reply, &session->out);
		if (error != 0) {
			error = EPROTO;
			break;
		}
		if (session->out.size > 0)
			error = client_send_out(session);
		break;
	case SSH_MSG_REQUEST_SUCCESS:
	case SSH_MSG_REQUEST_FAILURE:
		/* The answer to a keepalive: the server is alive, which receiving it already noted. */
		break;
	case SSH_MSG_CHANNEL_OPEN:
		error = client_refuse_open(session);
		break;
	case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
	case SSH_MSG_CHANNEL_OPEN_FAILURE:
		/* A refused session ends the program with 255; an accepted one starts. */
		error = ssh_channel_open_reply(&session->channels, client->payload.data, client->payload.size, &channel, &reason);
		if (error == EACCES) {
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "channel 0: open failed: %s", client_open_reason(reason));
			session->channel = NULL;
			session->failed = 1;
			session->done = 1;
			error = 0;
			break;
		}
		if (error != 0) {
			error = EPROTO;
			break;
		}
		error = client_opened(session);
		break;
	case SSH_MSG_CHANNEL_WINDOW_ADJUST:
		error = ssh_channel_parse_window_adjust(&session->channels, client->payload.data, client->payload.size, &channel);
		if (error != 0)
			error = EPROTO;
		break;
	case SSH_MSG_CHANNEL_DATA:
	case SSH_MSG_CHANNEL_EXTENDED_DATA:
		error = client_take_data(session);
		break;
	case SSH_MSG_CHANNEL_EOF:
	case SSH_MSG_CHANNEL_CLOSE:
		error = client_take_end(session);
		break;
	case SSH_MSG_CHANNEL_REQUEST:
		error = client_take_request(session);
		break;
	case SSH_MSG_CHANNEL_SUCCESS:
	case SSH_MSG_CHANNEL_FAILURE:
		error = client_take_reply(session);
		break;
	default:
		/* Passes over a late message of the authentication or of no known use. */
		ssh_client_log(client, SSH_CLIENT_LOG_DEBUG, "ssh: message %u ignored", (unsigned)client->payload.data[0]);
		break;
	}

	/* Reports a message that broke the protocol or could not be answered. */
	if (error != 0)
		return error;

	/* Succeeded: the message is handled. */
	return 0;
}

/*
 * Starts the session on its open channel: the terminal, the
 * no-more-sessions request, and the shell or the command.
 */
static int
client_opened(
	struct client_session *session)
{
	struct ssh_buf modes;
	struct termios settings;
	struct winsize size;
	const char *term;
	size_t term_length;
	int terminal;
	int status;
	int error;

	/* Asks for a terminal like the user's: its type, size and modes. */
	if (session->pty) {
		term = getenv("TERM");
		term_length = 0;
		if (term != NULL)
			term_length = strlen(term);
		if (term_length == 0 || term_length > SSH_CHANNEL_TERM_MAX)
			term = CLIENT_DEFAULT_TERM;
		memset(&size, 0, sizeof(size));
		ssh_buf_init(&modes, SSH_TTYMODES_MAX, 0);
		terminal = isatty(STDIN_FILENO);
		if (terminal) {
			(void)ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
			status = tcgetattr(STDIN_FILENO, &settings);
			if (status == 0)
				ssh_ttymodes_put(&settings, &modes);
		}
		if (modes.size == 0)
			ssh_buf_put_u8(&modes, SSH_TTY_OP_END);
		error = ssh_buf_error(&modes);
		if (error != 0) {
			ssh_buf_free(&modes);
			return error;
		}
		ssh_buf_reset(&session->out);
		ssh_channel_put_pty(session->channel, term, size.ws_col, size.ws_row, size.ws_xpixel, size.ws_ypixel, modes.data, modes.size, &session->out);
		ssh_buf_free(&modes);
		error = client_send_out(session);
		if (error != 0)
			return error;
		session->replies[session->reply_count] = CLIENT_REPLY_PTY;
		session->reply_count++;
		client_enter_raw(session);
	}

	/* Tells the server no other session will be opened on this connection (OpenSSH PROTOCOL 2.2). */
	ssh_buf_reset(&session->out);
	ssh_channel_put_no_more_sessions(&session->out);
	error = client_send_out(session);
	if (error != 0)
		return error;

	/* Starts the command, or the shell when there is none. */
	ssh_buf_reset(&session->out);
	if (session->command_size > 0) {
		ssh_channel_put_exec(session->channel, session->command, session->command_size, &session->out);
	} else {
		ssh_channel_put_shell(session->channel, &session->out);
	}
	error = client_send_out(session);
	if (error != 0)
		return error;
	session->replies[session->reply_count] = CLIENT_REPLY_START;
	session->reply_count++;

	/* Succeeded: the requests are on their way. */
	return 0;
}

/*
 * Takes channel data into the stream it belongs to.
 */
static int
client_take_data(
	struct client_session *session)
{
	struct ssh_client *client;
	struct ssh_channel *channel;
	struct client_stream *stream;
	const uint8_t *data;
	size_t size;
	uint32_t extended;
	int error;

	client = session->client;

	/* Reads the data and takes it from the window. */
	error = ssh_channel_parse_data(&session->channels, client->payload.data, client->payload.size, &channel, &data, &size, &extended);
	if (error != 0)
		return EPROTO;

	/* Chooses the stream: the output, the error output, or none for any other type. */
	stream = NULL;
	if (extended == 0)
		stream = &session->output;
	if (extended == SSH_EXTENDED_DATA_STDERR)
		stream = &session->error;

	/* Drops data of a closing channel, of another type, or for a stream that cannot be written. */
	if (stream == NULL ||
	    stream->broken ||
	    channel->close_sent) {
		session->unacknowledged += size;
		error = client_give_window(session);
		if (error != 0)
			return error;
		return 0;
	}

	/* Queues the bytes; the window is given back as they are written. */
	ssh_buf_put_bytes(&stream->queue, data, size);
	error = ssh_buf_error(&stream->queue);
	if (error != 0)
		return error;

	/* Succeeded: the data waits to be written. */
	return 0;
}

/*
 * Takes CHANNEL_EOF or CHANNEL_CLOSE.
 */
static int
client_take_end(
	struct client_session *session)
{
	struct ssh_client *client;
	struct ssh_channel *channel;
	int freed;
	int error;

	client = session->client;

	/* Reads which end it is. */
	error = ssh_channel_parse_end(&session->channels, client->payload.data, client->payload.size, &channel);
	if (error != 0)
		return EPROTO;

	/* An EOF only says the server sends no more data. */
	if (!channel->close_received)
		return 0;

	/* Answers the server's CLOSE with this side's, then forgets the channel. */
	if (!channel->close_sent) {
		ssh_buf_reset(&session->out);
		ssh_channel_put_close(channel, &session->out);
		error = client_send_out(session);
		if (error != 0)
			return error;
	}
	freed = ssh_channel_closed(&session->channels, channel);
	if (freed && channel == session->channel)
		session->channel = NULL;

	/* Succeeded: the end is noted. */
	return 0;
}

/*
 * Takes a channel request from the server: the exit status or signal,
 * and a refusal of anything that wants a reply.
 */
static int
client_take_request(
	struct client_session *session)
{
	struct ssh_client *client;
	struct ssh_channel_request request;
	struct ssh_channel *channel;
	int error;

	client = session->client;

	/* Reads the request. */
	error = ssh_channel_parse_request(&session->channels, client->payload.data, client->payload.size, &request);
	if (error != 0)
		return EPROTO;
	channel = ssh_channel_find(&session->channels, request.recipient);
	if (channel == NULL)
		return EPROTO;

	/* Keeps how the command ended. */
	if (request.type == SSH_CREQ_EXIT_STATUS) {
		session->have_exit = 1;
		session->exit_status = request.status;
	}
	if (request.type == SSH_CREQ_EXIT_SIGNAL) {
		session->have_exit = 1;
		session->killed = 1;
		ssh_client_log(client, SSH_CLIENT_LOG_VERBOSE, "ssh: the remote command ended by signal %s", request.name);
	}

	/* Refuses whatever wants a reply: a client offers nothing on request (OpenSSH's keepalives among them). */
	if (request.want_reply) {
		ssh_buf_reset(&session->out);
		ssh_channel_put_reply(channel, 0, &session->out);
		error = client_send_out(session);
		if (error != 0)
			return error;
	}

	/* Succeeded: the request is handled. */
	return 0;
}

/*
 * Takes the reply to the oldest channel request waiting.
 */
static int
client_take_reply(
	struct client_session *session)
{
	struct ssh_client *client;
	struct ssh_channel *channel;
	enum client_reply_kind kind;
	unsigned index;
	int success;
	int error;

	client = session->client;

	/* Reads the reply. */
	error = ssh_channel_parse_reply(&session->channels, client->payload.data, client->payload.size, &channel, &success);
	if (error != 0)
		return EPROTO;

	/* Refuses a reply nothing waits for. */
	if (session->reply_count == 0)
		return EPROTO;

	/* Takes the oldest request off the list. */
	kind = session->replies[0];
	for (index = 1; index < session->reply_count; index++)
		session->replies[index - 1U] = session->replies[index];
	session->reply_count--;

	/* A refused terminal is reported, and the session goes on without one. */
	if (!success && kind == CLIENT_REPLY_PTY) {
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "PTY allocation request failed on channel %lu", (unsigned long)channel->local_id);
		return 0;
	}

	/* A refused shell or command ends the session with 255. */
	if (!success) {
		if (session->command_size > 0) {
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "exec request failed on channel %lu", (unsigned long)channel->local_id);
		} else {
			ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "shell request failed on channel %lu", (unsigned long)channel->local_id);
		}
		session->failed = 1;
		if (!channel->close_sent) {
			ssh_buf_reset(&session->out);
			ssh_channel_put_close(channel, &session->out);
			error = client_send_out(session);
			if (error != 0)
				return error;
		}
		return 0;
	}

	/* Succeeded: the request was granted. */
	return 0;
}

/*
 * Refuses a channel the server opens (RFC 4254 5.1: OPEN_FAILURE with the
 * server's channel number): a client offers no channel.
 */
static int
client_refuse_open(
	struct client_session *session)
{
	struct ssh_reader reader;
	const uint8_t *type;
	size_t type_size;
	uint32_t sender;
	int error;

	/* Reads the number, the type and the server's channel number. */
	ssh_reader_init(&reader, session->client->payload.data, session->client->payload.size);
	(void)ssh_reader_get_u8(&reader);
	ssh_reader_get_string(&reader, &type, &type_size);
	sender = ssh_reader_get_u32(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EPROTO;

	/* Answers OPEN_FAILURE, administratively prohibited. */
	ssh_buf_reset(&session->out);
	ssh_buf_put_u8(&session->out, SSH_MSG_CHANNEL_OPEN_FAILURE);
	ssh_buf_put_u32(&session->out, sender);
	ssh_buf_put_u32(&session->out, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED);
	ssh_buf_put_cstring(&session->out, "open refused");
	ssh_buf_put_cstring(&session->out, "");
	error = client_send_out(session);
	if (error != 0)
		return error;

	/* Succeeded: the open is refused. */
	return 0;
}

/*
 * Sends the message built in session->out.
 */
static int
client_send_out(
	struct client_session *session)
{
	int error;

	/* Hands the message to the connection. */
	error = ssh_client_send_buf(session->client, &session->out);
	ssh_buf_reset(&session->out);

	/* Reports a message that could not be sent. */
	if (error != 0)
		return error;

	/* Succeeded: the message is on its way. */
	return 0;
}

/*
 * Reads the user's input into session->input, through the escapes.
 */
static int
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
		return 0;

	/* Notes the end of the input (an error ends it too). */
	if (count <= 0) {
		session->input_end = 1;
		return 0;
	}

	/* Passes the bytes through the escapes, or takes them as they are. */
	if (session->escape_on) {
		ssh_client_escape_filter(&session->escape, chunk, (size_t)count, session->input, &size, &disconnect);
		if (disconnect) {
			client_leave_raw(session);
			ssh_client_log(session->client, SSH_CLIENT_LOG_INFO, "Connection to %s closed.", session->client->host_name);
			session->pty = 0;
			session->quit = 1;
			session->done = 1;
		}
	} else {
		memcpy(session->input, chunk, (size_t)count);
		size = (size_t)count;
	}
	ssh_wipe(chunk, sizeof(chunk));

	/* Succeeded: the bytes wait to be sent. */
	session->input_size = size;
	session->input_offset = 0;
	return 0;
}

/*
 * Sends the user's input the window has room for, and the end of the
 * input once it is all sent.
 */
static int
client_send_input(
	struct client_session *session)
{
	struct ssh_client *client;
	size_t pending;
	size_t room;
	size_t size;
	int error;

	client = session->client;

	/* Sends nothing before the channel is open. */
	if (session->channel == NULL || session->channel->state != SSH_CHANNEL_OPEN)
		return 0;

	/* Sends while there is input, room in the window and in the output. */
	while (session->input_offset < session->input_size) {
		room = ssh_channel_send_room(session->channel);
		(void)ssh_transport_pending(&client->transport, &pending);
		if (room == 0 ||
		    client->backlog.size > 0 ||
		    pending >= SSH_CLIENT_OUTPUT_HIGH)
			return 0;
		size = session->input_size - session->input_offset;
		if (size > room)
			size = room;
		ssh_buf_reset(&session->out);
		error = ssh_channel_put_data(session->channel, 0, session->input + session->input_offset, size, &session->out);
		if (error == 0)
			error = client_send_out(session);
		if (error != 0)
			return error;
		session->input_offset += size;
	}

	/* Forgets the sent input. */
	ssh_wipe(session->input, session->input_size);
	session->input_size = 0;
	session->input_offset = 0;

	/* Sends the end of the input once, after the last byte. */
	if (session->input_end &&
	    !session->input_eof_sent &&
	    !session->channel->close_sent) {
		ssh_buf_reset(&session->out);
		ssh_channel_put_eof(session->channel, &session->out);
		error = client_send_out(session);
		if (error != 0)
			return error;
		session->input_eof_sent = 1;
	}

	/* Succeeded: what could be sent is sent. */
	return 0;
}

/*
 * Writes the next piece of a stream's queue and gives the window back for
 * what was written.
 */
static int
client_write_stream(
	struct client_session *session,
	struct client_stream *stream)
{
	ssize_t count;
	size_t size;
	int error;

	/* Writes at most one piece that fits without blocking. */
	size = stream->queue.size - stream->offset;
	if (size > stream->chunk)
		size = stream->chunk;
	count = write(stream->descriptor, stream->queue.data + stream->offset, size);
	if (count < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;

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

	/* Gives the window back when the queues allow. */
	session->unacknowledged += (size_t)count;
	error = client_give_window(session);
	if (error != 0)
		return error;

	/* Succeeded: the stream moved on. */
	return 0;
}

/*
 * Sends a keepalive when the server has been silent for
 * ServerAliveInterval, and gives up after ServerAliveCountMax of them go
 * unanswered (ssh_config(5)).
 */
static int
client_keep_alive(
	struct client_session *session,
	uint64_t now)
{
	struct ssh_client *client;
	int error;

	client = session->client;

	/* Does nothing when keepalives are off or none is due. */
	if (client->config.server_alive_interval == 0)
		return 0;
	if (now < session->alive_due)
		return 0;

	/* Gives up on a server that answered none of the keepalives allowed. */
	if (session->alive_sent >= client->config.server_alive_count) {
		client_leave_raw(session);
		ssh_client_log(client, SSH_CLIENT_LOG_QUIET, "Timeout, server %s not responding.", client->host_name);
		session->failed = 1;
		session->done = 1;
		return 0;
	}

	/* Sends one that wants a reply. */
	ssh_buf_reset(&session->out);
	ssh_channel_put_global_request(CLIENT_KEEPALIVE, 1, &session->out);
	error = client_send_out(session);
	if (error != 0)
		return error;
	session->alive_sent++;
	session->alive_due = now + (uint64_t)client->config.server_alive_interval * 1000U;

	/* Succeeded: the keepalive is on its way. */
	return 0;
}

/*
 * Sends the terminal's new size (RFC 4254 6.7).
 */
static int
client_send_window(
	struct client_session *session)
{
	struct winsize size;
	int status;
	int error;

	/* Only a session with a terminal on an open channel has a window to change. */
	if (!session->pty ||
	    session->channel == NULL ||
	    session->channel->state != SSH_CHANNEL_OPEN)
		return 0;

	/* Reads the size of the user's terminal. */
	memset(&size, 0, sizeof(size));
	status = ioctl(STDIN_FILENO, TIOCGWINSZ, &size);
	if (status != 0)
		return 0;

	/* Sends it. */
	ssh_buf_reset(&session->out);
	ssh_channel_put_window_change(session->channel, size.ws_col, size.ws_row, size.ws_xpixel, size.ws_ypixel, &session->out);
	error = client_send_out(session);
	if (error != 0)
		return error;

	/* Succeeded: the server knows the new size. */
	return 0;
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
 * Reports whether the session is over: its channel closed both ways and
 * every byte written.
 */
static int
client_finished(
	const struct client_session *session)
{
	/* A connection without a session ends only by the server or a signal. */
	if (session->no_session)
		return 0;

	/* The channel must have closed both ways. */
	if (session->channel != NULL)
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
 * Words a channel open failure reason.
 */
static const char *
client_open_reason(
	uint32_t reason)
{
	/*
	 * Values referenced: RFC 4254 5.1 (the reason codes of
	 * SSH_MSG_CHANNEL_OPEN_FAILURE) and RFC 4250 4.3 (their names).
	 */
	switch (reason) {
	case SSH_OPEN_ADMINISTRATIVELY_PROHIBITED:
		return "administratively prohibited";
	case SSH_OPEN_CONNECT_FAILED:
		return "connect failed";
	case SSH_OPEN_UNKNOWN_CHANNEL_TYPE:
		return "unknown channel type";
	case SSH_OPEN_RESOURCE_SHORTAGE:
		return "resource shortage";
	default:
		return "unknown reason";
	}
}

/*
 * Gives the server back the window for the bytes written or dropped,
 * while the queues hold less than half a window (ssh_channel_consumed
 * refills the whole window, so it is called only when the queues have
 * room for it).
 */
static int
client_give_window(
	struct client_session *session)
{
	size_t queued;
	int error;

	/* Nothing to give back, or no channel to give it on. */
	if (session->unacknowledged == 0 || session->channel == NULL)
		return 0;

	/* Waits while the queues still hold half a window or more. */
	queued = session->output.queue.size - session->output.offset;
	queued += session->error.queue.size - session->error.offset;
	if (queued >= SSH_CHANNEL_WINDOW / 2U)
		return 0;

	/* Gives the window back. */
	ssh_buf_reset(&session->out);
	ssh_channel_consumed(session->channel, session->unacknowledged, &session->out);
	session->unacknowledged = 0;
	if (session->out.size == 0)
		return 0;
	error = client_send_out(session);
	if (error != 0)
		return error;

	/* Succeeded: the server may send again. */
	return 0;
}
