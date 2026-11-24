/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client side of one SSH connection as a state machine without I/O:
 * the service request, the user authentication, the session channel with
 * its terminal, the X11 channels the server forwards, the forwarded TCP
 * channels both ways, and the keepalives.  The order of the login and of the
 * session is the one the ssh program follows; the difference is that
 * every wait is a state the engine returns from.
 */

#include <stdlib.h>
#include <string.h>

#include "ssh-client-engine.h"

/* The most bytes given to the transport at once (its input keeps room for that much beside one partial packet). */
#define ENGINE_FEED_SIZE	4096U

/*
 * The transport output above which the user's input is not sent, so that
 * the engine's own replies (window adjustments, keepalives) always fit
 * below the transport's limit.
 */
#define ENGINE_OUTPUT_HIGH	(1024U * 1024U)

/* The most bytes of messages kept while the transport refuses more. */
#define ENGINE_BACKLOG_MAX	(1024U * 1024U)

/* The most events waiting to be taken: one message gives at most two, a call one more. */
#define ENGINE_EVENTS_MAX	8U

/* The longest encoded terminal modes taken from the caller (the ssh program's bound for its own). */
#define ENGINE_MODES_MAX	1024U

/* The most channel requests waiting for their reply (pty-req and shell or exec). */
#define ENGINE_REPLIES_MAX	4U

/* The longest time between two ticks, so that re-exchanges start on time. */
#define ENGINE_TICK_MS		1000U

/* The terminal type sent when the caller gives none (the ssh program's for an unknown TERM). */
#define ENGINE_DEFAULT_TERM	"dumb"

/* The global request of ServerAliveInterval (OpenSSH PROTOCOL; any name the server refuses works). */
#define ENGINE_KEEPALIVE	"keepalive@openssh.com"

/* The description of the DISCONNECT this side sends at the end of a session. */
#define ENGINE_BYE		"disconnected by user"

/*
 * The X11 forwarding request and channel (RFC 4254 6.3.1, 6.3.2) and the
 * authentication protocol whose cookie it carries (the X Window System
 * protocol's MIT-MAGIC-COOKIE-1).
 */
#define ENGINE_X11_REQUEST	"x11-req"
#define ENGINE_X11_CHANNEL	"x11"
#define ENGINE_X11_PROTOCOL	"MIT-MAGIC-COOKIE-1"

/* The X screen forwarded (the display's first). */
#define ENGINE_X11_SCREEN	0U

/* The words of a refused x11-req, followed by the channel's number (OpenSSH's client's). */
#define ENGINE_X11_FAILED	"X11 forwarding request failed on channel "

/* The most X11 channels open at once. */
#define ENGINE_X11_CHANNELS_MAX		32U

/* The most forwarded TCP channels open at once on a connection that asks for X11 (the X11 channels keep their room). */
#define ENGINE_FORWARD_CHANNELS_X11	32U

/*
 * The global request and the channel types of TCP forwarding (RFC 4254
 * 7.1 "tcpip-forward", 7.2 "forwarded-tcpip" and "direct-tcpip").
 */
#define ENGINE_TCPIP_FORWARD	"tcpip-forward"
#define ENGINE_FORWARDED_TCPIP	"forwarded-tcpip"
#define ENGINE_DIRECT_TCPIP	"direct-tcpip"

/* The longest data of a direct-tcpip open: two strings of a host's length and two ports. */
#define ENGINE_DIRECT_DATA_MAX	(2U * (4U + SSH_CONFIG_HOST_MAX + 4U))

/*
 * The most global requests waiting for their answer (RFC 4254 4: answered
 * in order).  Keepalives leave room for every remote forwarding.
 */
#define ENGINE_GLOBALS_MAX	64U

/* What a global request waiting in the list is when it is no remote forwarding (whose number it holds otherwise). */
#define ENGINE_GLOBAL_KEEPALIVE	0xffffffffU

/* The words of the session's open failure, before the reason (OpenSSH's client's). */
#define ENGINE_OPEN_FAILED	"channel open failed: "

/*
 * The opcodes of the terminal modes a terminal without settings sends,
 * and their values.
 *
 * Values referenced:
 *   RFC 4254 section 8: 1 VINTR, 2 VQUIT, 3 VERASE, 4 VKILL, 5 VEOF,
 *   8 VSTART, 9 VSTOP, 10 VSUSP, 13 VWERASE, 14 VLNEXT, 36 ICRNL, 38 IXON,
 *   50 ISIG, 51 ICANON, 53 ECHO, 54 ECHOE, 55 ECHOK, 59 IEXTEN,
 *   60 ECHOCTL, 61 ECHOKE, 70 OPOST, 72 ONLCR, 91 CS8, 128 TTY_OP_ISPEED,
 *   129 TTY_OP_OSPEED, 0 TTY_OP_END; each opcode but the last is followed
 *   by a uint32.
 *   RFC 8160 section 3: 42 IUTF8.
 * The characters are the usual ones of a POSIX terminal: ^C, ^\, DEL,
 * ^U, ^D, ^Q, ^S, ^Z, ^W, ^V.
 */
#define ENGINE_TTY_VINTR	1U
#define ENGINE_TTY_VQUIT	2U
#define ENGINE_TTY_VERASE	3U
#define ENGINE_TTY_VKILL	4U
#define ENGINE_TTY_VEOF		5U
#define ENGINE_TTY_VSTART	8U
#define ENGINE_TTY_VSTOP	9U
#define ENGINE_TTY_VSUSP	10U
#define ENGINE_TTY_VWERASE	13U
#define ENGINE_TTY_VLNEXT	14U
#define ENGINE_TTY_ICRNL	36U
#define ENGINE_TTY_IXON		38U
#define ENGINE_TTY_IUTF8	42U
#define ENGINE_TTY_ISIG		50U
#define ENGINE_TTY_ICANON	51U
#define ENGINE_TTY_ECHO		53U
#define ENGINE_TTY_ECHOE	54U
#define ENGINE_TTY_ECHOK	55U
#define ENGINE_TTY_IEXTEN	59U
#define ENGINE_TTY_ECHOCTL	60U
#define ENGINE_TTY_ECHOKE	61U
#define ENGINE_TTY_OPOST	70U
#define ENGINE_TTY_ONLCR	72U
#define ENGINE_TTY_CS8		91U
#define ENGINE_TTY_ISPEED	128U
#define ENGINE_TTY_OSPEED	129U
#define ENGINE_TTY_END		0U
#define ENGINE_TTY_SPEED	38400U

/* Where the connection is. */
enum engine_phase {
	/* The first key exchange is in progress. */
	ENGINE_PHASE_KEX = 0,

	/* SSH_ENGINE_HOST_KEY is out; the caller's answer is awaited. */
	ENGINE_PHASE_HOST = 1,

	/* The service request is sent; its acceptance is awaited. */
	ENGINE_PHASE_SERVICE = 2,

	/* An authentication request is sent (request says which); its answer is awaited. */
	ENGINE_PHASE_AUTH = 3,

	/* SSH_ENGINE_PASSWORD is out; the caller's password is awaited. */
	ENGINE_PHASE_PASSWORD = 4,

	/* The user is authenticated; the session, if any, runs. */
	ENGINE_PHASE_CONNECTED = 5,

	/* The connection ended; only the pending output remains. */
	ENGINE_PHASE_CLOSED = 6,

	/* SSH_ENGINE_PASSPHRASE is out; the passphrase of the key the server would take is awaited. */
	ENGINE_PHASE_PASSPHRASE = 7
};

/* Which authentication request waits for its answer. */
enum engine_request {
	/* "none", to learn the methods (RFC 4252 5.2). */
	ENGINE_REQUEST_NONE = 0,

	/* "publickey" without a signature: would the server take this key (RFC 4252 7)? */
	ENGINE_REQUEST_QUERY = 1,

	/* "publickey" with the signature. */
	ENGINE_REQUEST_SIGNED = 2,

	/* "password" (RFC 4252 8). */
	ENGINE_REQUEST_PASSWORD = 3
};

/* What a channel request waiting for its reply was. */
enum engine_reply_kind {
	ENGINE_REPLY_PTY = 0,
	ENGINE_REPLY_START = 1,
	ENGINE_REPLY_X11 = 2
};

/* What a channel other than the session carries. */
enum engine_kind {
	/* The slot is free, or the session's. */
	ENGINE_KIND_NONE = 0,

	/* An X11 connection the server forwards. */
	ENGINE_KIND_X11 = 1,

	/* A direct-tcpip channel this side opened. */
	ENGINE_KIND_DIRECT = 2,

	/* A forwarded-tcpip channel the server opened. */
	ENGINE_KIND_FORWARDED = 3,

	/* A session channel opened for a subsystem. */
	ENGINE_KIND_SUBSYSTEM = 4
};

/* Where a remote forwarding (tcpip-forward) is. */
enum engine_forward_state {
	/* Asked for; the answer is awaited. */
	ENGINE_FORWARD_ASKED = 0,

	/* The server listens. */
	ENGINE_FORWARD_ACCEPTED = 1,

	/* The server refused it. */
	ENGINE_FORWARD_REFUSED = 2
};

/*
 * One remote forwarding asked for: address and port as sent, and bound,
 * the port the server listens on once it accepted (port, or its choice
 * for 0).
 */
struct engine_forward {
	enum engine_forward_state state;
	char address[SSH_CONFIG_HOST_MAX + 1];
	unsigned port;
	unsigned bound;
};

/* Where X11 forwarding is. */
enum engine_x11 {
	/* Not asked for (or not yet: the session has not opened). */
	ENGINE_X11_NONE = 0,

	/* x11-req is sent; its answer is awaited. */
	ENGINE_X11_ASKED = 1,

	/* The server accepted it: X11 channels are taken. */
	ENGINE_X11_ACCEPTED = 2,

	/* The server refused it. */
	ENGINE_X11_REFUSED = 3
};

/*
 * One identity the caller gave, kept until the login ends.
 *
 * name is the caller's name for it (empty when none); file holds the
 * bytes of the private key file (secret).
 */
struct engine_identity {
	char name[SSH_CONFIG_PATH_MAX];
	struct ssh_buf file;
};

/*
 * One connection.
 *
 * transport is the transport layer; payload the message it last handed
 * up (also the bytes of an SSH_ENGINE_DATA the caller holds); backlog the
 * messages the transport refused while its output was full (each as
 * uint32 length and the bytes), sent in order; out the message being
 * built.  phase is where the connection is and request which
 * authentication request waits.  host_key is the server's key blob of the
 * first exchange (have_host_key 1 once it is kept) and host_changed 1 when
 * a later exchange showed another.  methods are the methods the server
 * last named; identity_next the next identity to try and key the one
 * whose query or signature is out (secret; only its public half while
 * key_encrypted is 1, until its passphrase opens the file), key_identity
 * that key's identity and passphrase_tries how many passphrases did not
 * open its file; password_tries how many
 * passwords were refused, passwords_over 1 once no more are asked for.
 * channels and channel are the session channel (NULL before it is open
 * and after its close went both ways); replies the kinds of the channel
 * requests waiting for a reply; unconsumed the bytes of SSH_ENGINE_DATA
 * handed out that the caller has not reported used.  x11 is 1 when
 * X11 forwarding is wanted, x11_cookie its fake cookie, x11_single 1 for
 * one X client only, x11_state where the request is, x11_taken how many
 * X11 channels the server opened, and held the bytes of each X11 or
 * forwarded channel (by its number) handed out and not reported used;
 * kinds says what each channel but the session's carries, and
 * close_wanted marks a direct-tcpip channel the caller closed before its
 * answer.  forwards (forward_count of them) are the remote forwardings
 * asked for, and globals (global_count from global_first) the global
 * requests whose answers are awaited, in order: a remote forwarding's
 * number, or ENGINE_GLOBAL_KEEPALIVE.  session_over is 1
 * once the session channel closed both ways.  pty_refused,
 * started, eof_wanted, have_exit, exit_status, exit_signal and
 * session_reason are what the session came to.  events (event_count of them from event_first) wait to
 * be taken; closed_queued is 1 once SSH_ENGINE_CLOSED is among them or
 * taken.  peer_ended is 1 once the caller said the connection reached its
 * end, identified 1 once the server's identification line was read, and
 * rekeys counts the later exchanges that showed the first exchange's host
 * key.  now_ms is the time of the last tick, alive_due when the next
 * keepalive is due and alive_sent how many went unanswered.
 */
struct ssh_engine {
	struct ssh_transport transport;
	struct ssh_buf payload;
	struct ssh_buf backlog;
	struct ssh_buf out;
	enum engine_phase phase;
	enum engine_request request;
	char user[SSH_USER_MAX + 1];
	struct engine_identity identities[SSH_ENGINE_IDENTITIES_MAX];
	unsigned identity_count;
	unsigned identity_next;
	struct ssh_key key;
	int key_encrypted;
	unsigned key_identity;
	unsigned passphrase_tries;
	int pubkey;
	int password;
	unsigned password_tries;
	int passwords_over;
	char methods[SSH_ENGINE_METHODS_MAX];
	uint8_t host_key[SSH_KEY_BLOB_SIZE];
	int have_host_key;
	int host_changed;
	char banner[SSH_BANNER_MAX + 1];
	int session;
	int pty;
	char term[SSH_CHANNEL_TERM_MAX + 1];
	unsigned columns;
	unsigned rows;
	unsigned width;
	unsigned height;
	struct ssh_buf modes;
	struct ssh_buf command;
	struct ssh_channels channels;
	struct ssh_channel *channel;
	enum engine_reply_kind replies[ENGINE_REPLIES_MAX];
	unsigned reply_count;
	size_t unconsumed;
	int x11;
	char x11_cookie[SSH_ENGINE_X11_COOKIE_DIGITS + 1];
	int x11_single;
	enum engine_x11 x11_state;
	unsigned x11_taken;
	size_t held[SSH_CHANNEL_MAX];
	enum engine_kind kinds[SSH_CHANNEL_MAX];
	int close_wanted[SSH_CHANNEL_MAX];
	int subsystem_reply[SSH_CHANNEL_MAX];
	char subsystem_names[SSH_CHANNEL_MAX][SSH_CHANNEL_ENV_NAME_MAX + 1];
	int more_sessions;
	struct engine_forward forwards[SSH_ENGINE_REMOTE_FORWARDS_MAX];
	unsigned forward_count;
	uint32_t globals[ENGINE_GLOBALS_MAX];
	unsigned global_first;
	unsigned global_count;
	int session_over;
	int pty_refused;
	int started;
	int eof_wanted;
	int have_exit;
	int exit_status;
	char exit_signal[SSH_CHANNEL_SIGNAL_MAX + 1];
	char session_reason[SSH_ENGINE_REASON_MAX];
	struct ssh_engine_event events[ENGINE_EVENTS_MAX];
	unsigned event_first;
	unsigned event_count;
	int closed_queued;
	int peer_ended;
	int identified;
	unsigned long rekeys;
	uint64_t now_ms;
	uint64_t alive_due;
	unsigned alive_interval;
	unsigned alive_count;
	unsigned alive_sent;
};

static int engine_check_options(const struct ssh_engine_options *options);
static int engine_copy_options(struct ssh_engine *engine, const struct ssh_engine_options *options);
static struct ssh_engine_event *engine_event(struct ssh_engine *engine, enum ssh_engine_event_kind kind);
static void engine_copy_text(char *destination, size_t capacity, const char *source);
static int engine_send(struct ssh_engine *engine, const uint8_t *payload, size_t size);
static int engine_send_out(struct ssh_engine *engine);
static int engine_backlog_put(struct ssh_engine *engine, const uint8_t *payload, size_t size);
static void engine_flush(struct ssh_engine *engine);
static void engine_end(struct ssh_engine *engine, int error, uint32_t code, const char *reason);
static void engine_end_by_transport(struct ssh_engine *engine, int error);
static void engine_forget(struct ssh_engine *engine);
static int engine_check_host_key(void *context, const uint8_t *blob, size_t size);
static void engine_dispatch(struct ssh_engine *engine);
static int engine_take_service(struct ssh_engine *engine);
static int engine_take_auth(struct ssh_engine *engine);
static int engine_auth_next(struct ssh_engine *engine);
static int engine_send_query(struct ssh_engine *engine);
static int engine_send_signed(struct ssh_engine *engine);
static int engine_give_up_key(struct ssh_engine *engine);
static int engine_authenticated(struct ssh_engine *engine);
static int engine_take_session(struct ssh_engine *engine);
static int engine_opened(struct ssh_engine *engine);
static int engine_take_data(struct ssh_engine *engine);
static int engine_take_end(struct ssh_engine *engine);
static int engine_take_request(struct ssh_engine *engine);
static int engine_take_reply(struct ssh_engine *engine);
static int engine_refuse_open(struct ssh_engine *engine, uint32_t reason, const char *text);
static int engine_take_open(struct ssh_engine *engine);
static int engine_take_x11(struct ssh_engine *engine);
static int engine_take_forwarded(struct ssh_engine *engine);
static int engine_take_open_reply(struct ssh_engine *engine);
static int engine_take_direct_reply(struct ssh_engine *engine, struct ssh_channel *channel, int refused, uint32_t reason);
static int engine_take_subsystem_open(struct ssh_engine *engine, struct ssh_channel *channel, int refused, uint32_t reason);
static int engine_take_global_reply(struct ssh_engine *engine);
static int engine_find_forward(const struct ssh_engine *engine, const uint8_t *address, size_t size, uint32_t port, unsigned *forward);
static unsigned engine_count_kind(const struct ssh_engine *engine, enum engine_kind kind);
static unsigned engine_forward_channels_max(const struct ssh_engine *engine);
static void engine_global_push(struct ssh_engine *engine, uint32_t entry);
static int engine_text_ok(const char *text, size_t most, int empty);
static int engine_type_is(const uint8_t *type, size_t size, const char *name);
static int engine_send_x11_request(struct ssh_engine *engine);
static struct ssh_channel *engine_other_channel(struct ssh_engine *engine, uint32_t number);
static int engine_send_data(struct ssh_engine *engine, struct ssh_channel *channel, const uint8_t *bytes, size_t size, size_t *written);
static void engine_give_window(struct ssh_engine *engine, struct ssh_channel *channel, size_t *held, size_t size);
static int engine_channels_left(const struct ssh_engine *engine);
static void engine_finish(struct ssh_engine *engine);
static void engine_copy_address(char *destination, const uint8_t *source, size_t size);
static void engine_append_number(char *text, size_t capacity, unsigned number);
static void engine_append_safe(char *text, size_t capacity, const uint8_t *source, size_t size);
static int engine_send_eof(struct ssh_engine *engine);
static int engine_keep_alive(struct ssh_engine *engine);
static int engine_put_mode(struct ssh_buf *out, unsigned opcode, uint32_t value);
static int engine_method_offered(const struct ssh_engine *engine, const char *method);
static const char *engine_open_words(uint32_t reason);

/*
 * Makes an engine for one connection and queues this side's
 * identification line.
 */
int
ssh_engine_open(
	const struct ssh_engine_options *options,
	struct ssh_engine **engine)
{
	struct ssh_transport_ops ops;
	struct ssh_engine *made;
	unsigned index;
	int error;

	/* Starts with no engine handed out. */
	*engine = NULL;

	/* Refuses options the engine cannot act on. */
	error = engine_check_options(options);
	if (error != 0)
		return error;

	/* Allocates the state, which is large for the stack. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memset(made, 0, sizeof(*made));

	/* Starts the buffers: received messages and the user's input are secret. */
	ssh_buf_init(&made->payload, SSH_PACKET_LIMIT, 1);
	ssh_buf_init(&made->backlog, ENGINE_BACKLOG_MAX, 1);
	ssh_buf_init(&made->out, SSH_PACKET_LIMIT, 1);
	ssh_buf_init(&made->modes, ENGINE_MODES_MAX, 0);
	ssh_buf_init(&made->command, SSH_CHANNEL_COMMAND_MAX, 0);
	for (index = 0; index < SSH_ENGINE_IDENTITIES_MAX; index++)
		ssh_buf_init(&made->identities[index].file, SSH_KEY_FILE_MAX, 1);

	/* The session keeps channel number 0, also on a connection that never opens it. */
	ssh_channels_init(&made->channels);
	made->channels.keep_first = 1;

	/* Copies what the connection is to do. */
	error = engine_copy_options(made, options);
	if (error != 0) {
		ssh_engine_close(made);
		return error;
	}

	/* Starts the transport with the engine's own host key check, which queues the identification line. */
	memset(&ops, 0, sizeof(ops));
	ops.check_host_key = engine_check_host_key;
	ops.context = made;
	error = ssh_transport_init(&made->transport, SSH_ROLE_CLIENT, &ops, options->now_ms / 1000U);
	if (error != 0) {
		ssh_engine_close(made);
		return error;
	}

	/* The first exchange comes first; the clock starts now. */
	made->phase = ENGINE_PHASE_KEX;
	made->now_ms = options->now_ms;

	/* Succeeded: the caller owns the engine. */
	*engine = made;
	return 0;
}

/*
 * Wipes every secret of the engine and frees it.
 */
void
ssh_engine_close(
	struct ssh_engine *engine)
{
	/* Nothing to free. */
	if (engine == NULL)
		return;

	/* Wipes the identities, the key and the transport's keys. */
	engine_forget(engine);
	ssh_transport_free(&engine->transport);

	/* Wipes and frees the buffers. */
	ssh_buf_free(&engine->payload);
	ssh_buf_free(&engine->backlog);
	ssh_buf_free(&engine->out);
	ssh_buf_free(&engine->modes);
	ssh_buf_free(&engine->command);

	/* Frees the state, wiped first: it held the user's name and the session's events. */
	ssh_wipe(engine, sizeof(*engine));
	free(engine);
}

/*
 * Takes received bytes, or the end of the connection.
 */
int
ssh_engine_input(
	struct ssh_engine *engine,
	const uint8_t *bytes,
	size_t size,
	size_t *taken)
{
	size_t chunk;
	size_t fed;
	int error;

	/* Starts with nothing taken. */
	*taken = 0;

	/* Notes the end of the connection; ssh_engine_next reports it once the input is used. */
	if (size == 0) {
		engine->peer_ended = 1;
		return 0;
	}

	/* Takes and drops everything after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED) {
		*taken = size;
		return 0;
	}

	/* Gives the bytes to the transport in pieces while it takes them. */
	fed = 0;
	while (fed < size) {
		chunk = size - fed;
		if (chunk > ENGINE_FEED_SIZE)
			chunk = ENGINE_FEED_SIZE;
		error = ssh_transport_input(&engine->transport, bytes + fed, chunk);
		if (error == EMSGSIZE)
			break;

		/* Ends the connection when the input could not be kept. */
		if (error != 0) {
			engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "out of memory");
			return error;
		}

		/* Steps over what was taken. */
		fed += chunk;
	}

	/* Succeeded: *taken bytes are the transport's. */
	*taken = fed;
	return 0;
}

/*
 * Returns the bytes waiting to be written.
 */
const uint8_t *
ssh_engine_output(
	const struct ssh_engine *engine,
	size_t *size)
{
	const uint8_t *pending;

	/* Asks the transport for its output. */
	pending = ssh_transport_pending(&engine->transport, size);

	/* Reports where the bytes start. */
	return pending;
}

/*
 * Removes written bytes from the output.
 */
void
ssh_engine_written(
	struct ssh_engine *engine,
	size_t size)
{
	/* Drops the written bytes. */
	ssh_transport_written(&engine->transport, size);

	/* Moves waiting messages into the room just made. */
	engine_flush(engine);
}

/*
 * Gives the time.
 */
int
ssh_engine_tick(
	struct ssh_engine *engine,
	uint64_t now_ms,
	uint64_t *wake_ms)
{
	int error;

	/* Keeps the time, which the keepalive's next due time counts from. */
	engine->now_ms = now_ms;
	*wake_ms = now_ms + ENGINE_TICK_MS;

	/* An ended connection has nothing to time. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return 0;

	/* Starts a re-exchange when one is due. */
	error = ssh_transport_tick(&engine->transport, now_ms / 1000U);
	if (error != 0) {
		engine_end_by_transport(engine, error);
		return 0;
	}

	/* Sends a keepalive when one is due, or gives up on a silent server. */
	error = engine_keep_alive(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return 0;
	}

	/* Calls again no later than the next keepalive. */
	if (engine->alive_interval != 0 &&
	    engine->phase == ENGINE_PHASE_CONNECTED &&
	    engine->alive_due < *wake_ms)
		*wake_ms = engine->alive_due;

	/* Moves waiting messages into the transport. */
	engine_flush(engine);

	/* Succeeded: the time is given. */
	return 0;
}

/*
 * Takes the next event.
 */
int
ssh_engine_next(
	struct ssh_engine *engine,
	struct ssh_engine_event *event)
{
	int error;

	/* Turns until an event is ready or nothing more can be done. */
	for (;;) {
		/* Hands out the oldest waiting event. */
		if (engine->event_count > 0) {
			*event = engine->events[engine->event_first];
			memset(&engine->events[engine->event_first], 0, sizeof(engine->events[0]));
			engine->event_first = (engine->event_first + 1U) % ENGINE_EVENTS_MAX;
			engine->event_count--;
			return 0;
		}

		/* An ended connection has no more events. */
		if (engine->phase == ENGINE_PHASE_CLOSED)
			return EAGAIN;

		/* Shows the host key once the first exchange ended, and waits for the answer. */
		if (engine->phase == ENGINE_PHASE_KEX && engine->transport.state == SSH_TRANSPORT_RUNNING) {
			engine->phase = ENGINE_PHASE_HOST;
			(void)engine_event(engine, SSH_ENGINE_HOST_KEY);
			continue;
		}

		/* Moves waiting messages into the transport before taking more. */
		engine_flush(engine);

		/* Takes the next message from what the transport holds. */
		error = ssh_transport_next(&engine->transport, &engine->payload);

		/* Notes that the server identified itself once the transport went past its identification line. */
		if (engine->transport.state == SSH_TRANSPORT_INITIAL_KEX || engine->transport.state == SSH_TRANSPORT_RUNNING)
			engine->identified = 1;

		/* Reports the end of the connection once every byte before it is used. */
		if (error == EAGAIN && engine->peer_ended) {
			if (engine->phase == ENGINE_PHASE_KEX && engine->transport.state == SSH_TRANSPORT_RUNNING)
				continue;
			engine_end(engine, ECONNRESET, 0, "connection closed by the server");
			continue;
		}

		/* Nothing more until more input, an answer or a tick. */
		if (error == EAGAIN) {
			if (engine->phase == ENGINE_PHASE_KEX && engine->transport.state == SSH_TRANSPORT_RUNNING)
				continue;
			return EAGAIN;
		}

		/* Ends on the server's DISCONNECT or a failure of the transport. */
		if (error != 0) {
			engine_end_by_transport(engine, error);
			continue;
		}

		/* Shows the host key first when the message came right after the first exchange. */
		if (engine->phase == ENGINE_PHASE_KEX) {
			engine->phase = ENGINE_PHASE_HOST;
			(void)engine_event(engine, SSH_ENGINE_HOST_KEY);
		}

		/* Any message shows the server is alive. */
		engine->alive_sent = 0;
		engine->alive_due = engine->now_ms + (uint64_t)engine->alive_interval * 1000U;

		/* Acts on the message. */
		engine_dispatch(engine);
	}
}

/*
 * Answers SSH_ENGINE_HOST_KEY.
 */
int
ssh_engine_host_answer(
	struct ssh_engine *engine,
	int accept)
{
	int error;

	/* Refuses an answer nothing asked for. */
	if (engine->phase != ENGINE_PHASE_HOST)
		return EINVAL;

	/* Ends the connection on a refused key. */
	if (!accept) {
		engine_end(engine, EACCES, SSH_DISCONNECT_HOST_KEY_NOT_VERIFIABLE, "host key not accepted");
		return 0;
	}

	/* Asks for the authentication service (RFC 4253 10), the first message of the user's. */
	ssh_buf_reset(&engine->out);
	ssh_auth_put_service_request(SSH_SERVICE_USERAUTH, &engine->out);
	error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return 0;
	}

	/* Waits for the service to be accepted. */
	engine->phase = ENGINE_PHASE_SERVICE;

	/* Succeeded: the login starts. */
	return 0;
}

/*
 * Answers SSH_ENGINE_PASSWORD.
 */
int
ssh_engine_password(
	struct ssh_engine *engine,
	const char *password)
{
	size_t length;
	int error;

	/* Refuses an answer nothing asked for. */
	if (engine->phase != ENGINE_PHASE_PASSWORD)
		return EINVAL;

	/* A user who gives up ends the passwords; another method or the end follows. */
	if (password == NULL) {
		engine->passwords_over = 1;
		engine->phase = ENGINE_PHASE_AUTH;
		error = engine_auth_next(engine);
		if (error != 0)
			engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return 0;
	}

	/* Refuses a password longer than a request carries. */
	length = strlen(password);
	if (length > SSH_PASSWORD_MAX)
		return EINVAL;

	/* Sends the password in the secret buffer, which is wiped after. */
	ssh_buf_reset(&engine->out);
	ssh_auth_put_password(engine->user, password, &engine->out);
	engine->request = ENGINE_REQUEST_PASSWORD;
	engine->phase = ENGINE_PHASE_AUTH;
	error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return 0;
	}

	/* Succeeded: the password is on its way. */
	return 0;
}

/*
 * Answers SSH_ENGINE_PASSPHRASE.
 */
int
ssh_engine_passphrase(
	struct ssh_engine *engine,
	unsigned identity,
	const char *passphrase)
{
	struct ssh_engine_event *event;
	struct engine_identity *file;
	struct ssh_key opened;
	size_t length;
	int wrong;
	int same;
	int error;

	/* Refuses an answer nothing asked for, or for another identity. */
	if (engine->phase != ENGINE_PHASE_PASSPHRASE || identity != engine->key_identity)
		return EINVAL;

	/* A user who gives up, or answers nothing, passes over the key. */
	if (passphrase == NULL || passphrase[0] == '\0') {
		error = engine_give_up_key(engine);
		if (error != 0)
			engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return 0;
	}

	/* Refuses a passphrase longer than a file takes. */
	length = strlen(passphrase);
	if (length > SSH_KEY_PASSPHRASE_MAX)
		return EINVAL;

	/* Opens the file with the passphrase. */
	file = &engine->identities[engine->key_identity];
	error = ssh_key_parse_private_passphrase((const char *)file->file.data, file->file.size, (const uint8_t *)passphrase, length, &opened);

	/* A wrong passphrase counts against the tries. */
	if (error == EACCES)
		engine->passphrase_tries++;

	/* Asks again while tries are left. */
	if (error == EACCES && engine->passphrase_tries < SSH_ENGINE_PASSPHRASE_TRIES) {
		event = engine_event(engine, SSH_ENGINE_PASSPHRASE);
		if (event != NULL) {
			event->identity = engine->key_identity;
			event->tries = engine->passphrase_tries;
		}

		/* The caller hears the refusal; the question is out again. */
		return EACCES;
	}

	/* The key must be the one offered: a file whose sealed key is not its blob's is no use. */
	same = 0;
	if (error == 0)
		same = ssh_ct_equal(opened.public_key, engine->key.public_key, SSH_ED25519_PUBLIC);

	/* Passes over the key after the last wrong passphrase, for a file broken under its encryption, and for another key. */
	if (!same) {
		ssh_key_clear(&opened);
		wrong = 0;
		if (error == EACCES)
			wrong = 1;
		error = engine_give_up_key(engine);
		if (error != 0)
			engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");

		/* Tells a caller that kept the passphrase that it was wrong. */
		if (wrong)
			return EACCES;
		return 0;
	}

	/* The opened key takes the place of the public half that was offered. */
	engine->key = opened;
	ssh_key_clear(&opened);
	engine->key_encrypted = 0;

	/* Signs the request the server said it would take, and sends it. */
	engine->phase = ENGINE_PHASE_AUTH;
	error = engine_send_signed(engine);
	if (error != 0)
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");

	/* Succeeded: the answer is taken. */
	return 0;
}

/*
 * Sends the user's input to the session.
 */
int
ssh_engine_write(
	struct ssh_engine *engine,
	const uint8_t *bytes,
	size_t size,
	size_t *written)
{
	int error;

	/* Starts with nothing sent. */
	*written = 0;

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* Nothing goes before the session started, or after the input ended. */
	if (engine->channel == NULL ||
	    !engine->started ||
	    engine->eof_wanted)
		return 0;

	/* Sends what the session's window and the output take. */
	error = engine_send_data(engine, engine->channel, bytes, size, written);
	if (error != 0)
		return error;

	/* Succeeded: *written bytes are on their way. */
	return 0;
}

/*
 * Reports data used, so that the window may be given back.
 */
void
ssh_engine_consumed(
	struct ssh_engine *engine,
	size_t size)
{
	/* Gives back the session's window for what the caller used. */
	engine_give_window(engine, engine->channel, &engine->unconsumed, size);
}

/*
 * Answers SSH_ENGINE_X11_OPEN.
 */
int
ssh_engine_x11_answer(
	struct ssh_engine *engine,
	uint32_t channel,
	int accept)
{
	int error;

	/* Answers as for any channel the server opens. */
	error = ssh_engine_open_answer(engine, channel, accept);
	if (error != 0)
		return error;

	/* Succeeded: the answer is on its way. */
	return 0;
}

/*
 * Answers SSH_ENGINE_X11_OPEN or SSH_ENGINE_FORWARD_OPEN.
 */
int
ssh_engine_open_answer(
	struct ssh_engine *engine,
	uint32_t channel,
	int accept)
{
	struct ssh_channel *slot;
	int error;

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* Refuses an answer for a channel that waits for none. */
	if (channel >= SSH_CHANNEL_MAX)
		return EINVAL;
	slot = &engine->channels.channel[channel];
	if (slot->state != SSH_CHANNEL_ANSWERING)
		return EINVAL;

	/* Confirms the channel, or refuses it and frees its number. */
	ssh_buf_reset(&engine->out);
	error = ssh_channel_answer_open(&engine->channels, slot, accept, &engine->out);
	if (error == 0)
		error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return 0;
	}

	/* The caller holds nothing of the channel yet. */
	engine->held[channel] = 0;

	/* A refused channel carries nothing, and may have been the last one after the session. */
	if (!accept) {
		engine->kinds[channel] = ENGINE_KIND_NONE;
		engine_finish(engine);
	}

	/* Succeeded: the answer is on its way. */
	return 0;
}

/*
 * Asks the server to listen for connections to forward here.
 */
int
ssh_engine_forward_listen(
	struct ssh_engine *engine,
	const char *address,
	unsigned port,
	unsigned *forward)
{
	struct engine_forward *asked;
	int text_ok;
	int error;

	/* Starts with no number given. */
	*forward = 0;

	/* Nothing goes after the end, and nothing before the login. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;
	if (engine->phase != ENGINE_PHASE_CONNECTED)
		return EINVAL;

	/* Refuses an address the server could not be told, or a port out of range. */
	if (address == NULL || port > 65535U)
		return EINVAL;
	text_ok = engine_text_ok(address, SSH_CONFIG_HOST_MAX, 1);
	if (!text_ok)
		return EINVAL;

	/* Refuses a forwarding beyond the limit, or one whose answer could not be waited for. */
	if (engine->forward_count >= SSH_ENGINE_REMOTE_FORWARDS_MAX)
		return EAGAIN;
	if (engine->global_count >= ENGINE_GLOBALS_MAX)
		return EAGAIN;

	/*
	 * Asks for it: the address to bind and the port, with a reply wanted
	 * (which carries the port the server chose for 0).
	 *
	 * Values referenced: RFC 4254 7.1, the "tcpip-forward" global request
	 * (string address to bind, uint32 port number to bind).
	 */
	ssh_buf_reset(&engine->out);
	ssh_buf_put_u8(&engine->out, SSH_MSG_GLOBAL_REQUEST);
	ssh_buf_put_cstring(&engine->out, ENGINE_TCPIP_FORWARD);
	ssh_buf_put_bool(&engine->out, 1);
	ssh_buf_put_cstring(&engine->out, address);
	ssh_buf_put_u32(&engine->out, (uint32_t)port);
	error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return EPIPE;
	}

	/* Keeps it, and waits for its answer behind the global requests sent before. */
	asked = &engine->forwards[engine->forward_count];
	memset(asked, 0, sizeof(*asked));
	asked->state = ENGINE_FORWARD_ASKED;
	engine_copy_text(asked->address, sizeof(asked->address), address);
	asked->port = port;
	engine_global_push(engine, engine->forward_count);

	/* Succeeded: the caller names the forwarding by its number. */
	*forward = engine->forward_count;
	engine->forward_count++;
	return 0;
}

/*
 * Opens a direct-tcpip channel to host and port.
 */
int
ssh_engine_open_direct(
	struct ssh_engine *engine,
	const char *host,
	unsigned port,
	const char *origin,
	unsigned origin_port,
	uint32_t *channel)
{
	struct ssh_channel *slot;
	struct ssh_buf data;
	unsigned open_count;
	unsigned most;
	int text_ok;
	int error;

	/* Starts with no number given. */
	*channel = 0;

	/* Nothing goes after the end, nor before the login or after the session. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;
	if (engine->phase != ENGINE_PHASE_CONNECTED || engine->session_over)
		return EINVAL;

	/* Refuses a destination or an originator the server could not be told. */
	if (host == NULL || origin == NULL)
		return EINVAL;
	if (port < 1U || port > 65535U || origin_port > 65535U)
		return EINVAL;
	text_ok = engine_text_ok(host, SSH_CONFIG_HOST_MAX, 0);
	if (!text_ok)
		return EINVAL;
	text_ok = engine_text_ok(origin, SSH_CONFIG_HOST_MAX, 1);
	if (!text_ok)
		return EINVAL;

	/* Refuses a channel beyond the forwarded channels' share of the table. */
	open_count = engine_count_kind(engine, ENGINE_KIND_DIRECT) + engine_count_kind(engine, ENGINE_KIND_FORWARDED);
	most = engine_forward_channels_max(engine);
	if (open_count >= most)
		return EAGAIN;

	/*
	 * Builds the type's data: where to connect and where the connection
	 * came from.
	 *
	 * Values referenced: RFC 4254 7.2, the "direct-tcpip" channel open
	 * (string host to connect, uint32 port to connect, string originator
	 * IP address, uint32 originator port).
	 */
	ssh_buf_init(&data, ENGINE_DIRECT_DATA_MAX, 0);
	ssh_buf_put_cstring(&data, host);
	ssh_buf_put_u32(&data, (uint32_t)port);
	ssh_buf_put_cstring(&data, origin);
	ssh_buf_put_u32(&data, (uint32_t)origin_port);
	error = ssh_buf_error(&data);

	/* Opens the channel with the forwarded channels' window. */
	ssh_buf_reset(&engine->out);
	slot = NULL;
	if (error == 0)
		error = ssh_channel_open(&engine->channels, ENGINE_DIRECT_TCPIP, SSH_ENGINE_FORWARD_WINDOW, data.data, data.size, &engine->out, &slot);
	ssh_buf_free(&data);
	if (error == EAGAIN)
		return EAGAIN;
	if (error == 0)
		error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return EPIPE;
	}

	/* The channel carries a forwarded connection; the caller holds nothing of it. */
	engine->kinds[slot->local_id] = ENGINE_KIND_DIRECT;
	engine->close_wanted[slot->local_id] = 0;
	engine->held[slot->local_id] = 0;

	/* Succeeded: the open is on its way. */
	*channel = slot->local_id;
	return 0;
}

/*
 * Opens a "session" channel and asks it for the subsystem name.
 */
int
ssh_engine_open_subsystem(
	struct ssh_engine *engine,
	const char *name,
	uint32_t *channel)
{
	struct ssh_channel *slot;
	unsigned open_count;
	int text_ok;
	int error;

	/* Starts with no number given. */
	*channel = 0;

	/* Nothing goes after the end, nor when more_sessions is disabled. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;
	if (!engine->more_sessions)
		return EPERM;

	/* Allowed only while the main session channel is open (keeps slot 0). */
	if (engine->phase != ENGINE_PHASE_CONNECTED ||
	    !engine->session ||
	    engine->session_over ||
	    engine->channel == NULL ||
	    engine->channel->close_sent)
		return EINVAL;

	/* Refuses a subsystem name that cannot be sent in a channel request. */
	if (name == NULL)
		return EINVAL;
	text_ok = engine_text_ok(name, SSH_CHANNEL_ENV_NAME_MAX, 0);
	if (!text_ok)
		return EINVAL;

	/* Refuses a channel beyond the subsystems' limit. */
	open_count = engine_count_kind(engine, ENGINE_KIND_SUBSYSTEM);
	if (open_count >= SSH_ENGINE_SUBSYSTEMS_MAX)
		return EAGAIN;

	/* Opens a "session" channel with the subsystem window. */
	ssh_buf_reset(&engine->out);
	slot = NULL;
	error = ssh_channel_open(&engine->channels, "session", SSH_ENGINE_SUBSYSTEM_WINDOW, NULL, 0, &engine->out, &slot);
	if (error == EAGAIN)
		return EAGAIN;
	if (error == 0)
		error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return EPIPE;
	}

	/* Records the subsystem channel state. */
	engine->kinds[slot->local_id] = ENGINE_KIND_SUBSYSTEM;
	engine->close_wanted[slot->local_id] = 0;
	engine->held[slot->local_id] = 0;
	engine->subsystem_reply[slot->local_id] = 0;
	engine_copy_text(engine->subsystem_names[slot->local_id], sizeof(engine->subsystem_names[slot->local_id]), name);

	/* Succeeded: the open is on its way. */
	*channel = slot->local_id;
	return 0;
}

/*
 * Sends bytes on a channel.
 */
int
ssh_engine_channel_write(
	struct ssh_engine *engine,
	uint32_t channel,
	const uint8_t *bytes,
	size_t size,
	size_t *written)
{
	struct ssh_channel *slot;
	int error;

	/* Starts with nothing sent. */
	*written = 0;

	/* The session's channel is written as the user's input. */
	if (channel == SSH_ENGINE_SESSION_CHANNEL) {
		error = ssh_engine_write(engine, bytes, size, written);
		if (error != 0)
			return error;
		return 0;
	}

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* Refuses a number that is no open channel but the session's. */
	slot = engine_other_channel(engine, channel);
	if (slot == NULL)
		return EINVAL;

	/* Sends what the channel's window and the output take. */
	error = engine_send_data(engine, slot, bytes, size, written);
	if (error != 0)
		return error;

	/* Succeeded: *written bytes are on their way. */
	return 0;
}

/*
 * Reports data of a channel used.
 */
void
ssh_engine_channel_consumed(
	struct ssh_engine *engine,
	uint32_t channel,
	size_t size)
{
	struct ssh_channel *slot;

	/* The session's data is counted as before. */
	if (channel == SSH_ENGINE_SESSION_CHANNEL) {
		ssh_engine_consumed(engine, size);
		return;
	}

	/* A channel that is no longer there takes nothing back. */
	slot = engine_other_channel(engine, channel);
	if (slot == NULL)
		return;

	/* Gives back the channel's window for what the caller used. */
	engine_give_window(engine, slot, &engine->held[channel], size);
}

/*
 * Says that this side sends no more data on a channel.
 */
int
ssh_engine_channel_eof(
	struct ssh_engine *engine,
	uint32_t channel)
{
	struct ssh_channel *slot;
	int error;

	/* The session's end of input is the user's. */
	if (channel == SSH_ENGINE_SESSION_CHANNEL) {
		error = ssh_engine_eof(engine);
		if (error != 0)
			return error;
		return 0;
	}

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* Refuses a number that is no open channel but the session's. */
	slot = engine_other_channel(engine, channel);
	if (slot == NULL)
		return EINVAL;

	/* An EOF goes once, and not after this side's close. */
	if (slot->state != SSH_CHANNEL_OPEN || slot->eof_sent)
		return 0;

	/* Sends it. */
	ssh_buf_reset(&engine->out);
	ssh_channel_put_eof(slot, &engine->out);
	error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return EPIPE;
	}

	/* Succeeded: the server knows this side sends no more. */
	return 0;
}

/*
 * Closes an X11 or forwarded channel.
 */
int
ssh_engine_channel_close(
	struct ssh_engine *engine,
	uint32_t channel)
{
	struct ssh_channel *slot;
	int error;

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* A direct-tcpip or subsystem channel waiting for its answer is closed once it opens (CLOSE needs the server's number, RFC 4254 5.3). */
	if (channel < SSH_CHANNEL_MAX &&
	    engine->channels.channel[channel].state == SSH_CHANNEL_OPENING &&
	    (engine->kinds[channel] == ENGINE_KIND_DIRECT ||
	     engine->kinds[channel] == ENGINE_KIND_SUBSYSTEM)) {
		engine->close_wanted[channel] = 1;
		return 0;
	}

	/* Refuses a number that is no open channel (the session's included). */
	slot = engine_other_channel(engine, channel);
	if (slot == NULL)
		return EINVAL;

	/* A close goes once; the server's coming back ends the channel. */
	if (slot->close_sent)
		return 0;

	/* Sends it. */
	ssh_buf_reset(&engine->out);
	ssh_channel_put_close(slot, &engine->out);
	error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return EPIPE;
	}

	/* Succeeded: the channel is closing. */
	return 0;
}

/*
 * Gives the terminal's new size.
 */
int
ssh_engine_resize(
	struct ssh_engine *engine,
	unsigned columns,
	unsigned rows,
	unsigned width,
	unsigned height)
{
	int error;

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* Keeps the size for a terminal not yet asked for. */
	engine->columns = columns;
	engine->rows = rows;
	engine->width = width;
	engine->height = height;

	/* Only a session with a terminal on an open channel has a window to change (RFC 4254 6.7). */
	if (!engine->pty ||
	    engine->pty_refused ||
	    engine->channel == NULL ||
	    !engine->started)
		return 0;
	if (engine->channel->state != SSH_CHANNEL_OPEN)
		return 0;

	/* Sends the new size. */
	ssh_buf_reset(&engine->out);
	ssh_channel_put_window_change(engine->channel, columns, rows, width, height, &engine->out);
	error = engine_send_out(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return EPIPE;
	}

	/* Succeeded: the server knows the new size. */
	return 0;
}

/*
 * Says that the user's input ended.
 */
int
ssh_engine_eof(
	struct ssh_engine *engine)
{
	int error;

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* EOF_WANTED makes the session send EOF as soon as it runs, and no more data after. */
	engine->eof_wanted = 1;

	/* Sends it now when the session runs. */
	error = engine_send_eof(engine);
	if (error != 0) {
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
		return EPIPE;
	}

	/* Succeeded: the end of the input is noted. */
	return 0;
}

/*
 * Ends the connection at the user's word.
 */
void
ssh_engine_disconnect(
	struct ssh_engine *engine,
	const char *description)
{
	/* Says goodbye with the user's words, or the usual ones. */
	if (description == NULL)
		description = ENGINE_BYE;
	engine_end(engine, 0, SSH_DISCONNECT_BY_APPLICATION, description);
}

/*
 * Appends the terminal modes of a terminal without its own settings.
 */
int
ssh_engine_put_default_modes(
	struct ssh_buf *out)
{
	int error;

	/* The control characters: ^C, ^\, DEL, ^U, ^D, ^Q, ^S, ^Z, ^W, ^V. */
	(void)engine_put_mode(out, ENGINE_TTY_VINTR, 0x03U);
	(void)engine_put_mode(out, ENGINE_TTY_VQUIT, 0x1cU);
	(void)engine_put_mode(out, ENGINE_TTY_VERASE, 0x7fU);
	(void)engine_put_mode(out, ENGINE_TTY_VKILL, 0x15U);
	(void)engine_put_mode(out, ENGINE_TTY_VEOF, 0x04U);
	(void)engine_put_mode(out, ENGINE_TTY_VSTART, 0x11U);
	(void)engine_put_mode(out, ENGINE_TTY_VSTOP, 0x13U);
	(void)engine_put_mode(out, ENGINE_TTY_VSUSP, 0x1aU);
	(void)engine_put_mode(out, ENGINE_TTY_VWERASE, 0x17U);
	(void)engine_put_mode(out, ENGINE_TTY_VLNEXT, 0x16U);

	/* The input: CR read as NL, no flow control by keys (it would stop a terminal on ^S), UTF-8. */
	(void)engine_put_mode(out, ENGINE_TTY_ICRNL, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_IXON, 0U);
	(void)engine_put_mode(out, ENGINE_TTY_IUTF8, 1U);

	/* The line discipline: signals, line editing and echo. */
	(void)engine_put_mode(out, ENGINE_TTY_ISIG, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ICANON, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ECHO, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ECHOE, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ECHOK, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_IEXTEN, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ECHOCTL, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ECHOKE, 1U);

	/* The output: NL written as CR NL; eight bits; the speed. */
	(void)engine_put_mode(out, ENGINE_TTY_OPOST, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ONLCR, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_CS8, 1U);
	(void)engine_put_mode(out, ENGINE_TTY_ISPEED, ENGINE_TTY_SPEED);
	(void)engine_put_mode(out, ENGINE_TTY_OSPEED, ENGINE_TTY_SPEED);

	/* Ends the list. */
	ssh_buf_put_u8(out, ENGINE_TTY_END);

	/* Reports the first failure the buffer recorded. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: the modes are in out. */
	return 0;
}

/*
 * Decides what to do with a host key.
 */
int
ssh_engine_host_decide(
	enum ssh_known_result user_answer,
	enum ssh_known_result global_answer,
	enum ssh_strict_host strict,
	int batch_mode,
	enum ssh_engine_host_action *action)
{
	/* Refuses an answer that no lookup gives. */
	if ((unsigned)user_answer > SSH_KNOWN_OTHER_TYPES || (unsigned)global_answer > SSH_KNOWN_OTHER_TYPES)
		return EINVAL;
	if ((unsigned)strict > SSH_STRICT_ACCEPT_NEW)
		return EINVAL;

	/* A revoked key is refused whatever else is known. */
	if (user_answer == SSH_KNOWN_REVOKED || global_answer == SSH_KNOWN_REVOKED) {
		*action = SSH_ENGINE_HOST_REFUSE_REVOKED;
		return 0;
	}

	/* A key known for the host in either file is the host's. */
	if (user_answer == SSH_KNOWN_MATCH || global_answer == SSH_KNOWN_MATCH) {
		*action = SSH_ENGINE_HOST_ACCEPT;
		return 0;
	}

	/* Another key known for the host means the key changed: refused whatever the settings say. */
	if (user_answer == SSH_KNOWN_CHANGED || global_answer == SSH_KNOWN_CHANGED) {
		*action = SSH_ENGINE_HOST_REFUSE_CHANGED;
		return 0;
	}

	/* The host is not known with an ssh-ed25519 key: the settings decide. */
	switch (strict) {
	case SSH_STRICT_YES:
		*action = SSH_ENGINE_HOST_REFUSE_UNKNOWN;
		break;
	case SSH_STRICT_NO:
	case SSH_STRICT_ACCEPT_NEW:
		*action = SSH_ENGINE_HOST_ADD;
		break;
	default:
		/* Asks, unless nothing may be asked. */
		if (batch_mode) {
			*action = SSH_ENGINE_HOST_REFUSE_UNKNOWN;
		} else {
			*action = SSH_ENGINE_HOST_ASK;
		}

		break;
	}

	/* Succeeded: *action says what to do. */
	return 0;
}

/*
 * Reports whether the server's identification line was read.
 */
int
ssh_engine_identified(
	const struct ssh_engine *engine)
{
	/* The line was read when the transport went past it at any time before. */
	if (engine->identified)
		return 1;

	/* Reports that the server has not identified itself yet. */
	return 0;
}

/*
 * Reports how many later exchanges showed the first exchange's host key.
 */
unsigned long
ssh_engine_rekeys(
	const struct ssh_engine *engine)
{
	/* Reports the count kept by the host key check. */
	return engine->rekeys;
}

/* Refuses options the engine cannot act on: EINVAL, or 0. */
static int
engine_check_options(
	const struct ssh_engine_options *options)
{
	size_t length;

	/* The user must be named, within the protocol's limit. */
	if (options == NULL || options->user == NULL)
		return EINVAL;
	length = strlen(options->user);
	if (length == 0 || length > SSH_USER_MAX)
		return EINVAL;

	/* The identities must fit, and each must have its bytes. */
	if (options->identity_count > SSH_ENGINE_IDENTITIES_MAX)
		return EINVAL;
	if (options->identity_count > 0 && options->identities == NULL)
		return EINVAL;

	/* The terminal type must fit a pty-req. */
	if (options->term != NULL) {
		length = strlen(options->term);
		if (length == 0 || length > SSH_CHANNEL_TERM_MAX)
			return EINVAL;
	}

	/* The command and the modes must fit their requests. */
	if (options->command_size > SSH_CHANNEL_COMMAND_MAX)
		return EINVAL;
	if (options->command_size > 0 && options->command == NULL)
		return EINVAL;
	if (options->modes_size > ENGINE_MODES_MAX)
		return EINVAL;
	if (options->modes_size > 0 && options->modes == NULL)
		return EINVAL;

	/* X11 forwarding needs the session it is asked on, and its cookie in hexadecimal. */
	if (options->x11) {
		if (!options->session || options->x11_cookie == NULL)
			return EINVAL;
		length = strspn(options->x11_cookie, "0123456789abcdefABCDEF");
		if (length != SSH_ENGINE_X11_COOKIE_DIGITS || options->x11_cookie[length] != '\0')
			return EINVAL;
	}

	/* The options can be acted on. */
	return 0;
}

/* Copies the options into the engine: 0, EINVAL (an identity's bytes too long) or ENOMEM. */
static int
engine_copy_options(
	struct ssh_engine *engine,
	const struct ssh_engine_options *options)
{
	const struct ssh_engine_identity *identity;
	unsigned index;
	int error;

	/* The user and the methods to try. */
	engine_copy_text(engine->user, sizeof(engine->user), options->user);
	engine->pubkey = options->pubkey;
	engine->password = options->password;

	/* Copies each identity's name and bytes (secret). */
	for (index = 0; index < options->identity_count; index++) {
		identity = &options->identities[index];
		if (identity->name != NULL)
			engine_copy_text(engine->identities[index].name, sizeof(engine->identities[index].name), identity->name);
		if (identity->size > SSH_KEY_FILE_MAX)
			return EINVAL;
		ssh_buf_put_bytes(&engine->identities[index].file, identity->file, identity->size);
		error = ssh_buf_error(&engine->identities[index].file);
		if (error != 0)
			return error;
	}

	/* Every identity is kept. */
	engine->identity_count = options->identity_count;

	/* The session and its terminal. */
	engine->session = options->session;
	engine->more_sessions = options->more_sessions;
	engine->pty = options->pty;
	engine_copy_text(engine->term, sizeof(engine->term), ENGINE_DEFAULT_TERM);
	if (options->term != NULL)
		engine_copy_text(engine->term, sizeof(engine->term), options->term);
	engine->columns = options->columns;
	engine->rows = options->rows;
	engine->width = options->width;
	engine->height = options->height;

	/* The terminal modes: the caller's, or those of a terminal without settings. */
	if (options->modes_size > 0) {
		ssh_buf_put_bytes(&engine->modes, options->modes, options->modes_size);
		error = ssh_buf_error(&engine->modes);
	} else {
		error = ssh_engine_put_default_modes(&engine->modes);
	}

	/* A buffer that could not grow is told. */
	if (error != 0)
		return error;

	/* The command, or none for the shell. */
	if (options->command_size > 0) {
		ssh_buf_put_bytes(&engine->command, options->command, options->command_size);
		error = ssh_buf_error(&engine->command);
		if (error != 0)
			return error;
	}

	/* The keepalives. */
	engine->alive_interval = options->alive_interval;
	engine->alive_count = options->alive_count;

	/* X11 forwarding and its cookie. */
	if (options->x11) {
		engine->x11 = 1;
		engine_copy_text(engine->x11_cookie, sizeof(engine->x11_cookie), options->x11_cookie);
		engine->x11_single = 0;
		if (options->x11_single)
			engine->x11_single = 1;
	}

	/* Succeeded: the options are the engine's. */
	return 0;
}

/*
 * Appends an event of kind to the queue and returns it, cleared, for the
 * caller to fill; NULL when the queue is full, which the bound of
 * ENGINE_EVENTS_MAX keeps from happening.
 */
static struct ssh_engine_event *
engine_event(
	struct ssh_engine *engine,
	enum ssh_engine_event_kind kind)
{
	struct ssh_engine_event *event;
	unsigned slot;

	/* Refuses an event the queue has no room for. */
	if (engine->event_count >= ENGINE_EVENTS_MAX)
		return NULL;

	/* Takes the slot after the last waiting event. */
	slot = (engine->event_first + engine->event_count) % ENGINE_EVENTS_MAX;
	event = &engine->events[slot];
	memset(event, 0, sizeof(*event));
	event->kind = kind;
	engine->event_count++;

	/* The host key event shows the key kept from the first exchange. */
	if (kind == SSH_ENGINE_HOST_KEY) {
		event->bytes = engine->host_key;
		event->size = sizeof(engine->host_key);
	}

	/* Succeeded: the event waits to be taken. */
	return event;
}

/* Copies a NUL-terminated text, cut to capacity - 1 bytes. */
static void
engine_copy_text(
	char *destination,
	size_t capacity,
	const char *source)
{
	size_t length;

	/* Measures the text, cut to the room. */
	length = strlen(source);
	if (length > capacity - 1U)
		length = capacity - 1U;

	/* Copies and ends it. */
	memcpy(destination, source, length);
	destination[length] = '\0';
}

/*
 * Sends one message: to the transport, or to the backlog while the
 * transport's output is full or the backlog is not empty.  Returns 0,
 * ENOBUFS (the backlog is full too), or the transport's error.
 */
static int
engine_send(
	struct ssh_engine *engine,
	const uint8_t *payload,
	size_t size)
{
	int error;

	/* Keeps the order: while older messages wait, this one waits behind them. */
	if (engine->backlog.size > 0) {
		error = engine_backlog_put(engine, payload, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* Gives the message to the transport. */
	error = ssh_transport_send(&engine->transport, payload, size);

	/* Keeps a message the full output refused, to send it later. */
	if (error == ENOBUFS) {
		error = engine_backlog_put(engine, payload, size);
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

/* Sends the message built in out and empties out (wiping it). */
static int
engine_send_out(
	struct ssh_engine *engine)
{
	int error;

	/* Refuses a message that could not be built. */
	error = ssh_buf_error(&engine->out);

	/* Sends what was built. */
	if (error == 0)
		error = engine_send(engine, engine->out.data, engine->out.size);
	ssh_buf_free(&engine->out);
	ssh_buf_init(&engine->out, SSH_PACKET_LIMIT, 1);

	/* Reports a message that could not be sent. */
	if (error != 0)
		return error;

	/* Succeeded: the message is sent or kept in order. */
	return 0;
}

/* Appends one message to the backlog as uint32 length and bytes; ENOBUFS when it has no room. */
static int
engine_backlog_put(
	struct ssh_engine *engine,
	const uint8_t *payload,
	size_t size)
{
	int error;

	/* Refuses a message the backlog has no room for (nothing appended). */
	if (size > ENGINE_BACKLOG_MAX - 4U || engine->backlog.size > ENGINE_BACKLOG_MAX - 4U - size)
		return ENOBUFS;

	/* Appends the length and the bytes. */
	ssh_buf_put_u32(&engine->backlog, (uint32_t)size);
	ssh_buf_put_bytes(&engine->backlog, payload, size);
	error = ssh_buf_error(&engine->backlog);
	if (error != 0)
		return error;

	/* Succeeded: the message waits in order. */
	return 0;
}

/* Moves backlog messages into the transport while it takes them; a failure of the transport ends the connection. */
static void
engine_flush(
	struct ssh_engine *engine)
{
	uint32_t length;
	size_t taken;
	int error;

	/* Nothing to move after the end, or with an empty backlog. */
	if (engine->phase == ENGINE_PHASE_CLOSED || engine->backlog.size == 0)
		return;

	/* Hands the messages over in order until the transport refuses one. */
	taken = 0;
	error = 0;
	while (engine->backlog.size - taken >= 4U) {
		length = ssh_load_be32(engine->backlog.data + taken);
		error = ssh_transport_send(&engine->transport, engine->backlog.data + taken + 4U, length);
		if (error != 0)
			break;
		taken += 4U + (size_t)length;
	}

	/* Removes the messages the transport took (wiping them). */
	ssh_buf_consume(&engine->backlog, taken);

	/* Ends the connection on an error other than a full output. */
	if (error != 0 && error != ENOBUFS)
		engine_end_by_transport(engine, error);
}

/*
 * Ends the connection: queues DISCONNECT with code and reason unless the
 * transport has already ended, wipes the secrets, and queues
 * SSH_ENGINE_CLOSED with error, code and reason.  A second end is
 * ignored.
 */
static void
engine_end(
	struct ssh_engine *engine,
	int error,
	uint32_t code,
	const char *reason)
{
	struct ssh_engine_event *event;

	/* Ends only once. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return;

	/* Tells the server why, unless the transport has already ended (code 0: nothing to tell). */
	if (code != 0 && engine->transport.state != SSH_TRANSPORT_CLOSED)
		ssh_transport_disconnect(&engine->transport, code, reason);

	/* CLOSED tells every later call that only the pending output remains. */
	engine->phase = ENGINE_PHASE_CLOSED;
	engine_forget(engine);

	/* Queues the last event. */
	event = engine_event(engine, SSH_ENGINE_CLOSED);
	if (event != NULL) {
		event->error = error;
		event->code = code;
		event->tries = engine->password_tries;
		engine_copy_text(event->reason, sizeof(event->reason), reason);
	}

	/* The closing event has been queued, or tried when the queue was full. */
	engine->closed_queued = 1;
}

/*
 * Ends the connection after the transport ended it: the server's
 * DISCONNECT (ECONNABORTED), or a failure the transport answered with its
 * own DISCONNECT.
 */
static void
engine_end_by_transport(
	struct ssh_engine *engine,
	int error)
{
	struct ssh_engine_event *event;

	/* Ends only once. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return;

	/* Ends with the words of whoever ended the connection. */
	if (error == ECONNABORTED) {
		engine_end(engine, error, 0, engine->transport.reason);
	} else if (engine->host_changed) {
		engine_end(engine, error, 0, "the server's host key changed during a key re-exchange");
	} else {
		engine_end(engine, error, 0, engine->transport.local_reason);
	}

	/* Gives the code of the DISCONNECT that ended it. */
	event = &engine->events[(engine->event_first + engine->event_count - 1U) % ENGINE_EVENTS_MAX];
	if (event->kind == SSH_ENGINE_CLOSED) {
		event->code = engine->transport.local_code;
		if (error == ECONNABORTED)
			event->code = engine->transport.reason_code;
	}
}

/* Wipes the identities and the key, which the connection no longer needs. */
static void
engine_forget(
	struct ssh_engine *engine)
{
	unsigned index;

	/* Wipes and frees each identity's bytes. */
	for (index = 0; index < SSH_ENGINE_IDENTITIES_MAX; index++)
		ssh_buf_free(&engine->identities[index].file);
	engine->identity_count = 0;

	/* Wipes the key whose query or signature was out. */
	ssh_key_clear(&engine->key);
}

/*
 * Checks the server's host key blob for the transport: the first
 * exchange's key is kept (the caller is asked once the exchange ended and
 * its signature verified); every later exchange must show the same key.
 */
static int
engine_check_host_key(
	void *context,
	const uint8_t *blob,
	size_t size)
{
	struct ssh_engine *engine;
	int compare;

	/* Takes the engine from the callback context. */
	engine = context;

	/* The transport passes only an ssh-ed25519 blob, which has one size. */
	if (size != sizeof(engine->host_key))
		return EINVAL;

	/* Keeps the first exchange's key, to show the caller. */
	if (!engine->have_host_key) {
		memcpy(engine->host_key, blob, size);
		engine->have_host_key = 1;
		return 0;
	}

	/* A re-exchange must show the key of the first. */
	compare = memcmp(blob, engine->host_key, size);
	if (compare != 0) {
		engine->host_changed = 1;
		return EACCES;
	}

	/* Counts one more exchange that showed the key shown before. */
	engine->rekeys++;

	/* Succeeded: the key is the one shown before. */
	return 0;
}

/* Acts on the message in payload by the phase; a protocol error ends the connection. */
static void
engine_dispatch(
	struct ssh_engine *engine)
{
	int error;

	/* Hands the message to the part of the protocol the phase is in. */
	switch (engine->phase) {
	case ENGINE_PHASE_SERVICE:
		error = engine_take_service(engine);
		break;
	case ENGINE_PHASE_AUTH:
	case ENGINE_PHASE_PASSWORD:
	case ENGINE_PHASE_PASSPHRASE:
		error = engine_take_auth(engine);
		break;
	case ENGINE_PHASE_CONNECTED:
		error = engine_take_session(engine);
		break;
	default:
		/* Nothing of the user's was sent before the host key was accepted, so nothing may answer it (RFC 4253 10). */
		error = EPROTO;
		break;
	}

	/* Ends the connection on a broken protocol. */
	if (error == EPROTO) {
		engine_end(engine, EPROTO, SSH_DISCONNECT_PROTOCOL_ERROR, "protocol error");
		return;
	}

	/* Ends the connection on a message that could not be answered. */
	if (error != 0)
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
}

/* Takes the acceptance of the authentication service and sends "none" (RFC 4253 10, RFC 4252 5.2). */
static int
engine_take_service(
	struct ssh_engine *engine)
{
	char service[SSH_NAME_MAX + 1];
	int compare;
	int error;

	/* Requires the acceptance of the service asked for. */
	if (engine->payload.data[0] != SSH_MSG_SERVICE_ACCEPT)
		return EPROTO;
	error = ssh_auth_parse_service(engine->payload.data, engine->payload.size, service);
	if (error != 0)
		return EPROTO;
	compare = strcmp(service, SSH_SERVICE_USERAUTH);
	if (compare != 0)
		return EPROTO;

	/* Asks with "none", which shows the methods the server offers. */
	ssh_buf_reset(&engine->out);
	ssh_auth_put_none(engine->user, &engine->out);
	engine->request = ENGINE_REQUEST_NONE;
	engine->phase = ENGINE_PHASE_AUTH;
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Succeeded: "none" is on its way. */
	return 0;
}

/*
 * Takes a message of the authentication: a banner, the success, a failure
 * (the next method follows), or 60 (PK_OK after a query, the request to
 * change the password after a password).
 */
static int
engine_take_auth(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	size_t length;
	int partial;
	int error;

	/* Acts by the message number. */
	switch (engine->payload.data[0]) {
	case SSH_MSG_USERAUTH_BANNER:
		/* Shows the banner, made safe for a terminal. */
		error = ssh_auth_parse_banner(engine->payload.data, engine->payload.size, engine->banner, sizeof(engine->banner));
		if (error != 0)
			return EPROTO;
		length = strlen(engine->banner);
		if (length == 0)
			return 0;
		event = engine_event(engine, SSH_ENGINE_BANNER);
		if (event != NULL) {
			event->bytes = (const uint8_t *)engine->banner;
			event->size = length;
		}

		/* Waits on for the answer the banner came before. */
		return 0;
	case SSH_MSG_USERAUTH_SUCCESS:
		/* A success may only answer a request that is out. */
		if (engine->phase != ENGINE_PHASE_AUTH)
			return EPROTO;
		error = engine_authenticated(engine);
		if (error != 0)
			return error;
		return 0;
	case SSH_MSG_USERAUTH_FAILURE:
		/* A failure may only answer a request that is out. */
		if (engine->phase != ENGINE_PHASE_AUTH)
			return EPROTO;
		error = ssh_auth_parse_failure(engine->payload.data, engine->payload.size, engine->methods, sizeof(engine->methods), &partial);
		if (error != 0)
			return EPROTO;

		/* A refused password counts against the tries. */
		if (engine->request == ENGINE_REQUEST_PASSWORD)
			engine->password_tries++;

		/* Tries the next method. */
		error = engine_auth_next(engine);
		if (error != 0)
			return error;
		return 0;
	case SSH_MSG_USERAUTH_PK_OK:
		/* 60 answers only a query or a password (RFC 4252 7, 8). */
		if (engine->phase != ENGINE_PHASE_AUTH)
			return EPROTO;

		/* After a password it asks for a new one, which is not served: that ends the passwords. */
		if (engine->request == ENGINE_REQUEST_PASSWORD) {
			engine->passwords_over = 1;
			error = engine_auth_next(engine);
			if (error != 0)
				return error;
			return 0;
		}

		/* Anything but a query does not take it. */
		if (engine->request != ENGINE_REQUEST_QUERY)
			return EPROTO;

		/* The server would take the key it names, which must be the one offered. */
		error = ssh_auth_parse_pk_ok(engine->payload.data, engine->payload.size, engine->key.public_key);
		if (error != 0)
			return EPROTO;

		/* The key of an encrypted file needs its passphrase before it can sign. */
		if (engine->key_encrypted) {
			engine->phase = ENGINE_PHASE_PASSPHRASE;
			event = engine_event(engine, SSH_ENGINE_PASSPHRASE);
			if (event != NULL) {
				event->identity = engine->key_identity;
				event->tries = engine->passphrase_tries;
			}

			/* Succeeded: the question is out, and the signature waits for its answer. */
			return 0;
		}

		/* Signs the request over the session identifier and sends it. */
		error = engine_send_signed(engine);
		if (error != 0)
			return error;
		return 0;
	default:
		/* Nothing else may come before the authentication ends (RFC 4252 6). */
		return EPROTO;
	}
}

/*
 * Tries the next method the server offers: the next identity with
 * publickey, then a password; ends the connection when none is left
 * (RFC 4252 5.1, after "no more authentication methods").
 */
static int
engine_auth_next(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	char reason[SSH_ENGINE_REASON_MAX];
	size_t used;
	int offered;
	int error;

	/* Forgets the key of the request that was answered. */
	ssh_key_clear(&engine->key);

	/* Offers the next identity that can be used while the server offers publickey. */
	offered = engine_method_offered(engine, "publickey");
	while (engine->pubkey &&
	       offered &&
	       engine->identity_next < engine->identity_count) {
		error = engine_send_query(engine);
		if (error == 0)
			return 0;
		if (error != EAGAIN)
			return error;
	}

	/* Asks for a password while the server offers it and tries are left. */
	offered = engine_method_offered(engine, "password");
	if (engine->password &&
	    offered &&
	    !engine->passwords_over &&
	    engine->password_tries < SSH_ENGINE_PASSWORD_TRIES) {
		engine->phase = ENGINE_PHASE_PASSWORD;
		event = engine_event(engine, SSH_ENGINE_PASSWORD);
		if (event != NULL)
			event->tries = engine->password_tries;
		return 0;
	}

	/* Words the end as the ssh program does: the methods the server offered (they fit: see SSH_ENGINE_REASON_MAX). */
	engine_copy_text(reason, sizeof(reason), "Permission denied (");
	used = strlen(reason);
	engine_copy_text(reason + used, sizeof(reason) - used, engine->methods);
	used = strlen(reason);
	engine_copy_text(reason + used, sizeof(reason) - used, ").");

	/* Gives up: no method is left. */
	engine_end(engine, EACCES, SSH_DISCONNECT_NO_MORE_AUTH_METHODS_AVAILABLE, reason);

	/* Succeeded: the end is queued. */
	return 0;
}

/*
 * Loads the next identity and asks whether the server would take its key
 * (RFC 4252 7).  Returns 0 (the query is out), EAGAIN (the identity could
 * not be used: an encrypted one, or not a private key file), or an error
 * of sending.
 */
static int
engine_send_query(
	struct ssh_engine *engine)
{
	struct engine_identity *identity;
	int encrypted;
	int error;

	/* Takes the next identity. */
	identity = &engine->identities[engine->identity_next];
	engine->identity_next++;

	/* Reads the public half of its key, which an encrypted file keeps in the clear; a file that cannot be used is passed over. */
	error = ssh_key_parse_private_public((const char *)identity->file.data, identity->file.size, &engine->key, &encrypted);
	if (error != 0) {
		ssh_key_clear(&engine->key);
		return EAGAIN;
	}

	/* A file in the clear gives its whole key now. */
	if (!encrypted) {
		error = ssh_key_parse_private((const char *)identity->file.data, identity->file.size, &engine->key);
		if (error != 0) {
			ssh_key_clear(&engine->key);
			return EAGAIN;
		}
	}

	/* Notes whose key is out and whether it still needs its passphrase. */
	engine->key_encrypted = encrypted;
	engine->key_identity = engine->identity_next - 1U;
	engine->passphrase_tries = 0U;

	/* Asks about the key without a signature. */
	ssh_buf_reset(&engine->out);
	ssh_auth_put_publickey_query(engine->user, engine->key.public_key, &engine->out);
	engine->request = ENGINE_REQUEST_QUERY;
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Succeeded: the query is on its way. */
	return 0;
}

/* Notes the login and opens the session channel when one is wanted (RFC 4254 6.1). */
static int
engine_authenticated(
	struct ssh_engine *engine)
{
	int error;

	/* The login is over: the keys and passwords are no longer needed. */
	engine->phase = ENGINE_PHASE_CONNECTED;
	engine_forget(engine);
	engine->alive_due = engine->now_ms + (uint64_t)engine->alive_interval * 1000U;
	(void)engine_event(engine, SSH_ENGINE_AUTHENTICATED);

	/* A connection without a session waits for the server or the user. */
	if (!engine->session)
		return 0;

	/* Opens the session channel. */
	ssh_buf_reset(&engine->out);
	error = ssh_channel_open_session(&engine->channels, &engine->out, &engine->channel);
	if (error != 0)
		return error;
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Succeeded: the open is on its way. */
	return 0;
}

/* Takes a message of the connection protocol (RFC 4254). */
static int
engine_take_session(
	struct ssh_engine *engine)
{
	char name[SSH_NAME_MAX + 1];
	struct ssh_channel *channel;
	int want_reply;
	int error;

	/* Acts by the message number. */
	error = 0;
	switch (engine->payload.data[0]) {
	case SSH_MSG_GLOBAL_REQUEST:
		/* Refuses every global request the server makes (keepalives get their FAILURE). */
		ssh_buf_reset(&engine->out);
		error = ssh_channel_global_request(&engine->channels, engine->payload.data, engine->payload.size, name, &want_reply, &engine->out);
		if (error != 0) {
			error = EPROTO;
			break;
		}

		/* Sends the reply when one was built. */
		if (engine->out.size > 0)
			error = engine_send_out(engine);
		break;
	case SSH_MSG_REQUEST_SUCCESS:
	case SSH_MSG_REQUEST_FAILURE:
		/* The answer to the oldest global request: a keepalive (the server is alive, which receiving it noted) or a remote forwarding. */
		error = engine_take_global_reply(engine);
		break;
	case SSH_MSG_CHANNEL_OPEN:
		error = engine_take_open(engine);
		break;
	case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
	case SSH_MSG_CHANNEL_OPEN_FAILURE:
		/* The answer to the session's open, or to a direct-tcpip one. */
		error = engine_take_open_reply(engine);
		break;
	case SSH_MSG_CHANNEL_WINDOW_ADJUST:
		error = ssh_channel_parse_window_adjust(&engine->channels, engine->payload.data, engine->payload.size, &channel);
		if (error != 0)
			error = EPROTO;
		break;
	case SSH_MSG_CHANNEL_DATA:
	case SSH_MSG_CHANNEL_EXTENDED_DATA:
		error = engine_take_data(engine);
		break;
	case SSH_MSG_CHANNEL_EOF:
	case SSH_MSG_CHANNEL_CLOSE:
		error = engine_take_end(engine);
		break;
	case SSH_MSG_CHANNEL_REQUEST:
		error = engine_take_request(engine);
		break;
	case SSH_MSG_CHANNEL_SUCCESS:
	case SSH_MSG_CHANNEL_FAILURE:
		error = engine_take_reply(engine);
		break;
	default:
		/* Passes over a late message of the authentication or of no known use. */
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
 * no-more-sessions request, the shell or the command, and the end of the
 * input when it already ended.
 */
static int
engine_opened(
	struct ssh_engine *engine)
{
	int error;

	/* Asks for a terminal of the type, size and modes given (RFC 4254 6.2). */
	if (engine->pty) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_pty(engine->channel, engine->term, engine->columns, engine->rows, engine->width, engine->height, engine->modes.data, engine->modes.size, &engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;
		engine->replies[engine->reply_count] = ENGINE_REPLY_PTY;
		engine->reply_count++;
	}

	/* Asks for X11 forwarding before the shell or the command starts (RFC 4254 6.3.1). */
	if (engine->x11) {
		error = engine_send_x11_request(engine);
		if (error != 0)
			return error;
		engine->replies[engine->reply_count] = ENGINE_REPLY_X11;
		engine->reply_count++;
		engine->x11_state = ENGINE_X11_ASKED;
	}

	/* Tells the server no other session will be opened on this connection unless more_sessions is set (OpenSSH PROTOCOL 2.2). */
	if (!engine->more_sessions) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_no_more_sessions(&engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;
	}

	/* Starts the command, or the shell when there is none (RFC 4254 6.5). */
	ssh_buf_reset(&engine->out);
	if (engine->command.size > 0) {
		ssh_channel_put_exec(engine->channel, engine->command.data, engine->command.size, &engine->out);
	} else {
		ssh_channel_put_shell(engine->channel, &engine->out);
	}

	/* Sends the request. */
	error = engine_send_out(engine);
	if (error != 0)
		return error;
	engine->replies[engine->reply_count] = ENGINE_REPLY_START;
	engine->reply_count++;

	/* Succeeded: the requests are on their way. */
	return 0;
}

/* Takes channel data: an event for the caller, or dropped (and its window given back) when nobody reads it. */
static int
engine_take_data(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	struct ssh_channel *channel;
	const uint8_t *data;
	size_t *held;
	size_t size;
	uint32_t extended;
	int dropped;
	int error;

	/* Reads the data and takes it from the window. */
	error = ssh_channel_parse_data(&engine->channels, engine->payload.data, engine->payload.size, &channel, &data, &size, &extended);
	if (error != 0)
		return EPROTO;

	/* What the caller holds of the channel: the session's count, or the X11 channel's. */
	held = &engine->unconsumed;
	if (channel != engine->channel)
		held = &engine->held[channel->local_id];

	/* Data of a closing channel is dropped, and extended data but the session's error output. */
	dropped = 0;
	if (channel->close_sent) {
		dropped = 1;
	} else if (extended != 0 && channel != engine->channel) {
		dropped = 1;
	} else if (extended != 0 && extended != SSH_EXTENDED_DATA_STDERR) {
		dropped = 1;
	}

	/* Gives the window of dropped data back unless the caller holds half a window. */
	if (dropped) {
		if (*held >= channel->window_size / 2U)
			return 0;
		ssh_buf_reset(&engine->out);
		ssh_channel_consumed(channel, size, &engine->out);
		if (engine->out.size == 0)
			return 0;
		error = engine_send_out(engine);
		if (error != 0)
			return error;
		return 0;
	}

	/* An empty message has nothing to hand out. */
	if (size == 0)
		return 0;

	/* Hands the bytes out, counted until the caller reports them used; they point into payload until the next call. */
	*held += size;
	event = engine_event(engine, SSH_ENGINE_DATA);
	if (event != NULL) {
		event->channel = channel->local_id;
		event->bytes = data;
		event->size = size;
		event->stream = 0;
		if (extended == SSH_EXTENDED_DATA_STDERR)
			event->stream = 1;
	}

	/* Succeeded: the data waits to be taken. */
	return 0;
}

/*
 * Takes CHANNEL_EOF or CHANNEL_CLOSE; a close that went both ways ends
 * an X11 channel (SSH_ENGINE_CHANNEL_CLOSED) or the session
 * (SSH_ENGINE_EXIT), and the connection once no channel is left after
 * the session.
 */
static int
engine_take_end(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	struct ssh_channel *channel;
	uint32_t number;
	int freed;
	int error;

	/* Reads which end it is. */
	error = ssh_channel_parse_end(&engine->channels, engine->payload.data, engine->payload.size, &channel);
	if (error != 0)
		return EPROTO;

	/* An EOF only says the server sends no more data: the caller hears it of a channel but the session's. */
	if (!channel->close_received) {
		if (channel == engine->channel)
			return 0;
		event = engine_event(engine, SSH_ENGINE_CHANNEL_EOF);
		if (event != NULL)
			event->channel = channel->local_id;
		return 0;
	}

	/* Answers the server's CLOSE with this side's (RFC 4254 5.3). */
	if (!channel->close_sent) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_close(channel, &engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;
	}

	/* Frees the channel when both sides have closed it. */
	number = channel->local_id;
	freed = ssh_channel_closed(&engine->channels, channel);
	if (!freed)
		return 0;

	/* An X11, forwarded or subsystem channel ends alone; the connection ends with it when it was the last after the session. */
	if (channel != engine->channel) {
		engine->held[number] = 0;
		engine->kinds[number] = ENGINE_KIND_NONE;
		engine->close_wanted[number] = 0;
		engine->subsystem_reply[number] = 0;
		engine->subsystem_names[number][0] = '\0';
		event = engine_event(engine, SSH_ENGINE_CHANNEL_CLOSED);
		if (event != NULL)
			event->channel = number;
		engine_finish(engine);
		return 0;
	}

	/* The session is over. */
	engine->channel = NULL;
	engine->session_over = 1;

	/* Tells how the session ended. */
	event = engine_event(engine, SSH_ENGINE_EXIT);
	if (event != NULL) {
		event->exit_status = -1;
		if (engine->have_exit)
			event->exit_status = engine->exit_status;
		engine_copy_text(event->exit_signal, sizeof(event->exit_signal), engine->exit_signal);
		engine_copy_text(event->reason, sizeof(event->reason), engine->session_reason);
	}

	/* Ends the connection with the session, as the ssh program does, unless X11 channels remain. */
	engine_finish(engine);

	/* Succeeded: the session is over. */
	return 0;
}

/*
 * Takes a channel request from the server: the exit status or signal
 * (RFC 4254 6.10), and a refusal of anything that wants a reply.
 */
static int
engine_take_request(
	struct ssh_engine *engine)
{
	struct ssh_channel_request request;
	struct ssh_channel *channel;
	int error;

	/* Reads the request. */
	error = ssh_channel_parse_request(&engine->channels, engine->payload.data, engine->payload.size, &request);
	if (error != 0)
		return EPROTO;
	channel = ssh_channel_find(&engine->channels, request.recipient);
	if (channel == NULL)
		return EPROTO;

	/* Keeps how the command ended (only the session's channel says it). */
	if (request.type == SSH_CREQ_EXIT_STATUS && channel == engine->channel) {
		engine->have_exit = 1;
		engine->exit_status = (int)(request.status & 0x7fffffffU);
	}

	/* Keeps the signal that ended the command. */
	if (request.type == SSH_CREQ_EXIT_SIGNAL && channel == engine->channel)
		engine_copy_text(engine->exit_signal, sizeof(engine->exit_signal), request.name);

	/* Refuses whatever wants a reply: a client offers nothing on request (OpenSSH's keepalives among them). */
	if (request.want_reply) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_reply(channel, 0, &engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;
	}

	/* Succeeded: the request is handled. */
	return 0;
}

/*
 * Takes the reply to the oldest channel request waiting: a refused
 * terminal goes on without one, a refused shell or command closes the
 * channel, and the start of the shell or command is told.
 */
static int
engine_take_reply(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	struct ssh_channel *channel;
	enum engine_reply_kind kind;
	unsigned index;
	int success;
	int error;

	/* Reads the reply. */
	error = ssh_channel_parse_reply(&engine->channels, engine->payload.data, engine->payload.size, &channel, &success);
	if (error != 0)
		return EPROTO;

	/* Handles a reply for a subsystem channel before checking the session's FIFO. */
	if (channel != engine->channel) {
		if (engine->kinds[channel->local_id] != ENGINE_KIND_SUBSYSTEM ||
		    !engine->subsystem_reply[channel->local_id])
			return EPROTO;

		/* Clears the pending reply flag and ignores replies after close. */
		engine->subsystem_reply[channel->local_id] = 0;
		if (channel->close_sent)
			return 0;

		/* Publishes the subsystem request outcome to the caller. */
		event = engine_event(engine, SSH_ENGINE_SUBSYSTEM_REPLY);
		if (event != NULL) {
			event->channel = channel->local_id;
			event->accepted = success;
			if (!success) {
				event->code = 0;
				engine_copy_text(event->reason, sizeof(event->reason), "subsystem request failed on channel ");
				engine_append_number(event->reason, sizeof(event->reason), channel->local_id);
			}
		}

		/* Closes the channel if the subsystem request was rejected. */
		if (!success && !channel->close_sent) {
			ssh_buf_reset(&engine->out);
			ssh_channel_put_close(channel, &engine->out);
			error = engine_send_out(engine);
			if (error != 0)
				return error;
		}

		/* Succeeded: the subsystem reply was processed. */
		return 0;
	}

	/* Refuses a reply nothing waits for on the session channel. */
	if (engine->reply_count == 0)
		return EPROTO;

	/* Takes the oldest request off the list. */
	kind = engine->replies[0];
	for (index = 1; index < engine->reply_count; index++)
		engine->replies[index - 1U] = engine->replies[index];
	engine->reply_count--;

	/* A refused terminal: the session goes on without one. */
	if (kind == ENGINE_REPLY_PTY) {
		if (!success)
			engine->pty_refused = 1;
		return 0;
	}

	/* X11 forwarding: the caller hears the answer, and the session goes on either way. */
	if (kind == ENGINE_REPLY_X11) {
		engine->x11_state = ENGINE_X11_REFUSED;
		if (success)
			engine->x11_state = ENGINE_X11_ACCEPTED;
		event = engine_event(engine, SSH_ENGINE_X11_REPLY);
		if (event != NULL) {
			event->accepted = success;
			if (!success) {
				engine_copy_text(event->reason, sizeof(event->reason), ENGINE_X11_FAILED);
				engine_append_number(event->reason, sizeof(event->reason), channel->local_id);
			}
		}

		/* Reports that the reply was handled. */
		return 0;
	}

	/* A refused shell or command ends the session: the channel is closed. */
	if (!success) {
		if (engine->command.size > 0) {
			engine_copy_text(engine->session_reason, sizeof(engine->session_reason), "exec request failed");
		} else {
			engine_copy_text(engine->session_reason, sizeof(engine->session_reason), "shell request failed");
		}

		/* Closes the channel; its close coming back ends the session. */
		if (!channel->close_sent) {
			ssh_buf_reset(&engine->out);
			ssh_channel_put_close(channel, &engine->out);
			error = engine_send_out(engine);
			if (error != 0)
				return error;
		}

		/* Reports that the reply was handled. */
		return 0;
	}

	/* The shell or the command runs: the caller may send now. */
	engine->started = 1;
	event = engine_event(engine, SSH_ENGINE_STARTED);
	if (event != NULL) {
		event->pty = 0;
		if (engine->pty && !engine->pty_refused)
			event->pty = 1;
	}

	/* Sends the end of the input when it already ended. */
	error = engine_send_eof(engine);
	if (error != 0)
		return error;

	/* Succeeded: the session runs. */
	return 0;
}

/*
 * Refuses a channel the server opens (RFC 4254 5.1: OPEN_FAILURE with the
 * server's channel number, reason and text): a client offers no channel
 * but the X11 and forwarded ones it asked for, and those only within
 * their limits.
 */
static int
engine_refuse_open(
	struct ssh_engine *engine,
	uint32_t reason,
	const char *text)
{
	struct ssh_reader reader;
	const uint8_t *type;
	size_t type_size;
	uint32_t sender;
	int error;

	/* Reads the number, the type and the server's channel number. */
	ssh_reader_init(&reader, engine->payload.data, engine->payload.size);
	(void)ssh_reader_get_u8(&reader);
	ssh_reader_get_string(&reader, &type, &type_size);
	sender = ssh_reader_get_u32(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EPROTO;

	/* Answers OPEN_FAILURE with the reason. */
	ssh_buf_reset(&engine->out);
	ssh_buf_put_u8(&engine->out, SSH_MSG_CHANNEL_OPEN_FAILURE);
	ssh_buf_put_u32(&engine->out, sender);
	ssh_buf_put_u32(&engine->out, reason);
	ssh_buf_put_cstring(&engine->out, text);
	ssh_buf_put_cstring(&engine->out, "");
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Succeeded: the open is refused. */
	return 0;
}

/* Sends CHANNEL_EOF once, when the user's input ended and the session runs (RFC 4254 5.3). */
static int
engine_send_eof(
	struct ssh_engine *engine)
{
	int error;

	/* Waits for the end of the input and a running session. */
	if (!engine->eof_wanted ||
	    !engine->started ||
	    engine->channel == NULL)
		return 0;
	if (engine->channel->eof_sent || engine->channel->close_sent)
		return 0;

	/* Sends it. */
	ssh_buf_reset(&engine->out);
	ssh_channel_put_eof(engine->channel, &engine->out);
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Succeeded: the server knows the input ended. */
	return 0;
}

/*
 * Sends a keepalive when the server has been silent for
 * ServerAliveInterval, and gives up after ServerAliveCountMax of them go
 * unanswered (ssh_config(5) of OpenSSH 10.5p1).
 */
static int
engine_keep_alive(
	struct ssh_engine *engine)
{
	int error;

	/* Does nothing when keepalives are off, before the login, or when none is due. */
	if (engine->alive_interval == 0 || engine->phase != ENGINE_PHASE_CONNECTED)
		return 0;
	if (engine->now_ms < engine->alive_due)
		return 0;

	/* Gives up on a server that answered none of the keepalives allowed. */
	if (engine->alive_sent >= engine->alive_count) {
		engine_end(engine, ETIMEDOUT, SSH_DISCONNECT_BY_APPLICATION, "Timeout, server not responding");
		return 0;
	}

	/*
	 * Sends one that wants a reply, and waits for its answer behind the
	 * global requests sent before.  While the list of answers awaited has
	 * no room beside every remote forwarding's, none is sent but it counts
	 * as sent, so a server that answers nothing still runs out of them.
	 */
	if (engine->global_count < ENGINE_GLOBALS_MAX - SSH_ENGINE_REMOTE_FORWARDS_MAX) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_global_request(ENGINE_KEEPALIVE, 1, &engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;
		engine_global_push(engine, ENGINE_GLOBAL_KEEPALIVE);
	}

	/* Counts it until any message answers, and times the next. */
	engine->alive_sent++;
	engine->alive_due = engine->now_ms + (uint64_t)engine->alive_interval * 1000U;

	/* Succeeded: the keepalive is on its way. */
	return 0;
}

/* Appends one terminal mode: the opcode and its uint32 value (RFC 4254 8). */
static int
engine_put_mode(
	struct ssh_buf *out,
	unsigned opcode,
	uint32_t value)
{
	int error;

	/* Writes the opcode and the value. */
	ssh_buf_put_u8(out, (uint8_t)opcode);
	ssh_buf_put_u32(out, value);

	/* Reports a buffer that failed. */
	error = ssh_buf_error(out);
	if (error != 0)
		return error;

	/* Succeeded: the mode is in out. */
	return 0;
}

/* Reports whether the server's last list of methods has method. */
static int
engine_method_offered(
	const struct ssh_engine *engine,
	const char *method)
{
	size_t size;
	int offered;

	/* Looks the name up in the list. */
	size = strlen(engine->methods);
	offered = ssh_namelist_contains(engine->methods, size, method);

	/* Reports whether the server offers it. */
	if (offered != 0)
		return 1;
	return 0;
}

/*
 * Words a channel open failure reason.
 */
static const char *
engine_open_words(
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

/* Signs the publickey request for engine->key over the session identifier and sends it (RFC 4252 7).  Returns 0 or an error of sending. */
static int
engine_send_signed(
	struct ssh_engine *engine)
{
	int error;

	/* Builds the signed request. */
	ssh_buf_reset(&engine->out);
	error = ssh_auth_put_publickey_signed(engine->user, &engine->key, engine->transport.session_id, &engine->out);
	if (error != 0)
		return error;

	/* Sends it and waits for its answer. */
	engine->request = ENGINE_REQUEST_SIGNED;
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Succeeded: the signed request is on its way. */
	return 0;
}

/*
 * Passes over the key whose passphrase was asked for: the next identity,
 * a password or the end follows, the methods being those the server last
 * named.  Returns 0 or an error of sending.
 */
static int
engine_give_up_key(
	struct ssh_engine *engine)
{
	int error;

	/* Back to the requests, without the key. */
	engine->phase = ENGINE_PHASE_AUTH;
	engine->key_encrypted = 0;
	engine->passphrase_tries = 0U;

	/* Tries the next method. */
	error = engine_auth_next(engine);
	if (error != 0)
		return error;

	/* Succeeded: the next method is on its way, or the end is queued. */
	return 0;
}

/*
 * Takes a CHANNEL_OPEN from the server by its type: "x11" and
 * "forwarded-tcpip" as asked for; any other open is refused
 * (administratively prohibited; a client refuses "direct-tcpip", RFC
 * 4254 7.2).
 */
static int
engine_take_open(
	struct ssh_engine *engine)
{
	struct ssh_reader reader;
	const uint8_t *type;
	size_t type_size;
	int error;
	int x11;
	int forwarded;

	/* Reads the type. */
	ssh_reader_init(&reader, engine->payload.data, engine->payload.size);
	(void)ssh_reader_get_u8(&reader);
	ssh_reader_get_string(&reader, &type, &type_size);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EPROTO;

	/* An X11 connection. */
	x11 = engine_type_is(type, type_size, ENGINE_X11_CHANNEL);
	if (x11) {
		error = engine_take_x11(engine);
		if (error != 0)
			return error;
		return 0;
	}

	/* A connection to a port the server listens on for this side. */
	forwarded = engine_type_is(type, type_size, ENGINE_FORWARDED_TCPIP);
	if (forwarded) {
		error = engine_take_forwarded(engine);
		if (error != 0)
			return error;
		return 0;
	}

	/* Any other type. */
	error = engine_refuse_open(engine, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED, "open refused");
	if (error != 0)
		return error;

	/* Succeeded: the open is refused. */
	return 0;
}

/*
 * Takes an "x11" open: once X11 forwarding was accepted and while the
 * session is open (one only when asked for one), within
 * ENGINE_X11_CHANNELS_MAX, shown to the caller as SSH_ENGINE_X11_OPEN.
 */
static int
engine_take_x11(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	struct ssh_channel *channel;
	struct ssh_reader reader;
	const uint8_t *data;
	const uint8_t *address;
	size_t data_size;
	size_t address_size;
	uint32_t port;
	unsigned open_count;
	int error;

	/* Refuses every open while no X11 channel may come: before the answer, without the request, after the session. */
	if (engine->x11_state != ENGINE_X11_ACCEPTED || engine->channel == NULL) {
		error = engine_refuse_open(engine, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED, "open refused");
		if (error != 0)
			return error;
		return 0;
	}

	/* Refuses a second X11 channel when one X client only was asked for. */
	if (engine->x11_single && engine->x11_taken > 0) {
		error = engine_refuse_open(engine, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED, "open refused");
		if (error != 0)
			return error;
		return 0;
	}

	/* Refuses an X11 channel beyond their share of the table. */
	open_count = engine_count_kind(engine, ENGINE_KIND_X11);
	if (open_count >= ENGINE_X11_CHANNELS_MAX) {
		error = engine_refuse_open(engine, SSH_OPEN_RESOURCE_SHORTAGE, "resource shortage");
		if (error != 0)
			return error;
		return 0;
	}

	/* Takes the open into a slot that waits for the caller's answer; a full table is refused. */
	ssh_buf_reset(&engine->out);
	error = ssh_channel_take_open(&engine->channels, engine->payload.data, engine->payload.size, ENGINE_X11_CHANNEL, SSH_ENGINE_X11_WINDOW, &engine->out, &channel, &data, &data_size);
	if (error == EINVAL || error == EPROTO)
		return EPROTO;
	if (error != 0)
		return error;

	/* Sends the refusal of an open that was not taken. */
	if (channel == NULL) {
		if (engine->out.size == 0)
			return 0;
		error = engine_send_out(engine);
		if (error != 0)
			return error;
		return 0;
	}

	/* Reads the originator of the X client (RFC 4254 6.3.2). */
	ssh_reader_init(&reader, data, data_size);
	ssh_reader_get_string(&reader, &address, &address_size);
	port = ssh_reader_get_u32(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EPROTO;

	/* Counts the X client; the caller holds nothing of it yet. */
	engine->x11_taken++;
	engine->held[channel->local_id] = 0;
	engine->kinds[channel->local_id] = ENGINE_KIND_X11;

	/* Asks the caller. */
	event = engine_event(engine, SSH_ENGINE_X11_OPEN);
	if (event != NULL) {
		event->channel = channel->local_id;
		engine_copy_address(event->address, address, address_size);
		event->port = port;
	}

	/* Succeeded: the open waits for the caller's answer. */
	return 0;
}

/*
 * Takes a "forwarded-tcpip" open: before the session closed (on a
 * connection without one, at any time), for an address and port a remote
 * forwarding asked for, within the forwarded channels' share of the
 * table; shown to the caller as SSH_ENGINE_FORWARD_OPEN.
 *
 * Procedure referenced: RFC 4254 7.2 (the client must refuse a
 * forwarded-tcpip open for a port it did not ask for) and
 * openssh-portable fa4b2a82be7d channels.c (open_listen_match_tcpip:
 * the address and port connected must be the ones asked for, the port
 * the server chose for 0; a forwarding is taken from the moment it is
 * asked for).
 */
static int
engine_take_forwarded(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	struct ssh_channel *channel;
	struct ssh_reader reader;
	const uint8_t *data;
	const uint8_t *text;
	const uint8_t *address;
	const uint8_t *origin;
	size_t data_size;
	size_t text_size;
	size_t address_size;
	size_t origin_size;
	uint32_t port;
	uint32_t origin_port;
	unsigned forward;
	unsigned open_count;
	unsigned most;
	int found;
	int error;

	/*
	 * Reads the open whole: the type, the server's number, window and
	 * packet size, then the address and port connected and the
	 * originator's.
	 *
	 * Values referenced: RFC 4254 7.2, the "forwarded-tcpip" channel open
	 * (string address that was connected, uint32 port that was connected,
	 * string originator IP address, uint32 originator port).
	 */
	ssh_reader_init(&reader, engine->payload.data, engine->payload.size);
	(void)ssh_reader_get_u8(&reader);
	ssh_reader_get_string(&reader, &text, &text_size);
	(void)ssh_reader_get_u32(&reader);
	(void)ssh_reader_get_u32(&reader);
	(void)ssh_reader_get_u32(&reader);
	ssh_reader_get_string(&reader, &address, &address_size);
	port = ssh_reader_get_u32(&reader);
	ssh_reader_get_string(&reader, &origin, &origin_size);
	origin_port = ssh_reader_get_u32(&reader);
	error = ssh_reader_error(&reader);
	if (error != 0)
		return EPROTO;

	/* Refuses every open after the session, which ends the connection once its channels are gone. */
	if (engine->session_over) {
		error = engine_refuse_open(engine, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED, "open refused");
		if (error != 0)
			return error;
		return 0;
	}

	/* Refuses a connection to an address and port not asked for. */
	found = engine_find_forward(engine, address, address_size, port, &forward);
	if (!found) {
		error = engine_refuse_open(engine, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED, "open refused");
		if (error != 0)
			return error;
		return 0;
	}

	/* Refuses a channel beyond the forwarded channels' share of the table. */
	open_count = engine_count_kind(engine, ENGINE_KIND_DIRECT) + engine_count_kind(engine, ENGINE_KIND_FORWARDED);
	most = engine_forward_channels_max(engine);
	if (open_count >= most) {
		error = engine_refuse_open(engine, SSH_OPEN_RESOURCE_SHORTAGE, "resource shortage");
		if (error != 0)
			return error;
		return 0;
	}

	/* Takes the open into a slot that waits for the caller's answer; a full table is refused. */
	ssh_buf_reset(&engine->out);
	error = ssh_channel_take_open(&engine->channels, engine->payload.data, engine->payload.size, ENGINE_FORWARDED_TCPIP, SSH_ENGINE_FORWARD_WINDOW, &engine->out, &channel, &data, &data_size);
	if (error == EINVAL || error == EPROTO)
		return EPROTO;
	if (error != 0)
		return error;

	/* Sends the refusal of an open that was not taken. */
	if (channel == NULL) {
		if (engine->out.size == 0)
			return 0;
		error = engine_send_out(engine);
		if (error != 0)
			return error;
		return 0;
	}

	/* The channel carries a forwarded connection; the caller holds nothing of it yet. */
	engine->held[channel->local_id] = 0;
	engine->kinds[channel->local_id] = ENGINE_KIND_FORWARDED;
	engine->close_wanted[channel->local_id] = 0;

	/* Asks the caller. */
	event = engine_event(engine, SSH_ENGINE_FORWARD_OPEN);
	if (event != NULL) {
		event->channel = channel->local_id;
		event->forward = forward;
		engine_copy_text(event->listen_address, sizeof(event->listen_address), engine->forwards[forward].address);
		event->listen_port = port;
		engine_copy_address(event->address, origin, origin_size);
		event->port = origin_port;
	}

	/* Succeeded: the open waits for the caller's answer. */
	return 0;
}

/*
 * Takes OPEN_CONFIRMATION or OPEN_FAILURE: for the session, a refusal
 * ends the connection and a confirmation starts the session; for a
 * direct-tcpip channel, the caller hears it.
 */
static int
engine_take_open_reply(
	struct ssh_engine *engine)
{
	struct ssh_channel *channel;
	const char *words;
	uint32_t reason;
	int refused;
	int error;

	/* Reads the answer. */
	error = ssh_channel_open_reply(&engine->channels, engine->payload.data, engine->payload.size, &channel, &reason);
	if (error != 0 && error != EACCES)
		return EPROTO;
	refused = 0;
	if (error == EACCES)
		refused = 1;

	/* The answer for a direct-tcpip or subsystem channel. */
	if (channel != engine->channel) {
		if (engine->kinds[channel->local_id] == ENGINE_KIND_SUBSYSTEM)
			return engine_take_subsystem_open(engine, channel, refused, reason);
		error = engine_take_direct_reply(engine, channel, refused, reason);
		if (error != 0)
			return error;
		return 0;
	}

	/* A refused session ends the connection. */
	if (refused) {
		engine->channel = NULL;
		words = engine_open_words(reason);
		engine_copy_text(engine->session_reason, sizeof(engine->session_reason), ENGINE_OPEN_FAILED);
		engine_append_safe(engine->session_reason, sizeof(engine->session_reason), (const uint8_t *)words, strlen(words));
		engine_end(engine, ECONNREFUSED, SSH_DISCONNECT_BY_APPLICATION, engine->session_reason);
		return 0;
	}

	/* Starts the session on the open channel. */
	error = engine_opened(engine);
	if (error != 0)
		return error;

	/* Succeeded: the session starts. */
	return 0;
}

/*
 * Takes the answer for a direct-tcpip channel (refused 1 with reason, or
 * confirmed): the caller hears SSH_ENGINE_OPEN_REPLY, or, when it closed
 * the channel before the answer, the confirmed channel is closed at once
 * and a refused one ends with SSH_ENGINE_CHANNEL_CLOSED.
 */
static int
engine_take_direct_reply(
	struct ssh_engine *engine,
	struct ssh_channel *channel,
	int refused,
	uint32_t reason)
{
	struct ssh_engine_event *event;
	struct ssh_reader reader;
	const uint8_t *description;
	size_t description_size;
	uint32_t number;
	int error;

	/* Only a channel this side opened for a forwarded connection has an answer here. */
	number = channel->local_id;
	if (engine->kinds[number] != ENGINE_KIND_DIRECT)
		return EPROTO;

	/* A refusal frees the number. */
	if (refused) {
		engine->kinds[number] = ENGINE_KIND_NONE;
		engine->held[number] = 0;

		/* A channel the caller closed ends without an answer of its own. */
		if (engine->close_wanted[number]) {
			engine->close_wanted[number] = 0;
			event = engine_event(engine, SSH_ENGINE_CHANNEL_CLOSED);
			if (event != NULL)
				event->channel = number;
			engine_finish(engine);
			return 0;
		}

		/* Reads the server's description (RFC 4254 5.1), read whole before. */
		ssh_reader_init(&reader, engine->payload.data, engine->payload.size);
		(void)ssh_reader_get_u8(&reader);
		(void)ssh_reader_get_u32(&reader);
		(void)ssh_reader_get_u32(&reader);
		ssh_reader_get_string(&reader, &description, &description_size);

		/* Tells the caller why, as OpenSSH's client words it: the reason, then the description. */
		event = engine_event(engine, SSH_ENGINE_OPEN_REPLY);
		if (event != NULL) {
			event->channel = number;
			event->accepted = 0;
			event->code = reason;
			engine_copy_text(event->reason, sizeof(event->reason), engine_open_words(reason));
			if (description_size > 0) {
				engine_append_safe(event->reason, sizeof(event->reason), (const uint8_t *)": ", 2);
				engine_append_safe(event->reason, sizeof(event->reason), description, description_size);
			}
		}

		/* A refused channel may have been the last one after the session. */
		engine_finish(engine);
		return 0;
	}

	/* A channel the caller closed before the answer is closed now; its close coming back ends it. */
	engine->held[number] = 0;
	if (engine->close_wanted[number]) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_close(channel, &engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;
		return 0;
	}

	/* Tells the caller the channel is open. */
	event = engine_event(engine, SSH_ENGINE_OPEN_REPLY);
	if (event != NULL) {
		event->channel = number;
		event->accepted = 1;
	}

	/* Succeeded: the answer is handled. */
	return 0;
}

/*
 * Takes the answer for a subsystem "session" channel: on confirmation,
 * sends the subsystem request; on refusal, reports SSH_ENGINE_SUBSYSTEM_REPLY.
 */
static int
engine_take_subsystem_open(
	struct ssh_engine *engine,
	struct ssh_channel *channel,
	int refused,
	uint32_t reason)
{
	struct ssh_engine_event *event;
	struct ssh_reader reader;
	const uint8_t *description;
	size_t description_size;
	const char *words;
	uint32_t number;
	int error;

	number = channel->local_id;

	/* A refusal frees the number at once. */
	if (refused) {
		engine->kinds[number] = ENGINE_KIND_NONE;
		engine->held[number] = 0;
		engine->subsystem_reply[number] = 0;
		engine->subsystem_names[number][0] = '\0';

		/* A channel the caller closed ends without an answer of its own. */
		if (engine->close_wanted[number]) {
			engine->close_wanted[number] = 0;
			event = engine_event(engine, SSH_ENGINE_CHANNEL_CLOSED);
			if (event != NULL)
				event->channel = number;

			/* A closed channel may have been the last one after the session. */
			engine_finish(engine);
			return 0;
		}

		/* Reads the server's description (RFC 4254 5.1), read whole before. */
		ssh_reader_init(&reader, engine->payload.data, engine->payload.size);
		(void)ssh_reader_get_u8(&reader);
		(void)ssh_reader_get_u32(&reader);
		(void)ssh_reader_get_u32(&reader);
		ssh_reader_get_string(&reader, &description, &description_size);

		/* Tells the caller why, as OpenSSH's client words it. */
		words = engine_open_words(reason);
		event = engine_event(engine, SSH_ENGINE_SUBSYSTEM_REPLY);
		if (event != NULL) {
			event->channel = number;
			event->accepted = 0;
			event->code = reason;
			engine_copy_text(event->reason, sizeof(event->reason), "channel ");
			engine_append_number(event->reason, sizeof(event->reason), number);
			engine_append_safe(event->reason, sizeof(event->reason), (const uint8_t *)": open failed: ", 15);
			engine_append_safe(event->reason, sizeof(event->reason), (const uint8_t *)words, strlen(words));
			if (description_size > 0) {
				engine_append_safe(event->reason, sizeof(event->reason), (const uint8_t *)": ", 2);
				engine_append_safe(event->reason, sizeof(event->reason), description, description_size);
			}
		}

		/* A refused channel may have been the last one after the session. */
		engine_finish(engine);
		return 0;
	}

	/* A channel the caller closed before the answer is closed now; its close coming back ends it. */
	engine->held[number] = 0;
	if (engine->close_wanted[number]) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_close(channel, &engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;

		/* Succeeded: the close is on its way. */
		return 0;
	}

	/* Asks the open session channel for the named subsystem (RFC 4254 6.5). */
	ssh_buf_reset(&engine->out);
	ssh_channel_put_subsystem(channel, engine->subsystem_names[number], &engine->out);
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Marks the channel as awaiting the subsystem request reply. */
	engine->subsystem_reply[number] = 1;

	/* Succeeded: the subsystem request is on its way. */
	return 0;
}

/*
 * Takes REQUEST_SUCCESS or REQUEST_FAILURE, the answer to the oldest
 * global request waiting (RFC 4254 4): nothing more for a keepalive; for
 * a remote forwarding, the caller hears it as SSH_ENGINE_FORWARD_REPLY.
 * An answer nothing waits for is dropped.
 *
 * Procedure referenced: RFC 4254 7.1 (REQUEST_SUCCESS carries the port
 * the server chose when 0 was asked for) and openssh-portable
 * fa4b2a82be7d ssh.c (ssh_confirm_remote_forward: a chosen port above
 * 65535 is a refusal).
 */
static int
engine_take_global_reply(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	struct engine_forward *forward;
	struct ssh_reader reader;
	uint32_t entry;
	uint32_t chosen;
	int accepted;
	int error;

	/* Drops an answer nothing waits for. */
	if (engine->global_count == 0)
		return 0;

	/* Takes the oldest request off the list. */
	entry = engine->globals[engine->global_first];
	engine->global_first = (engine->global_first + 1U) % ENGINE_GLOBALS_MAX;
	engine->global_count--;

	/* A keepalive's answer only shows the server is alive. */
	if (entry == ENGINE_GLOBAL_KEEPALIVE)
		return 0;

	/* A remote forwarding: accepted or refused. */
	forward = &engine->forwards[entry];
	accepted = 0;
	if (engine->payload.data[0] == SSH_MSG_REQUEST_SUCCESS)
		accepted = 1;
	forward->bound = forward->port;

	/* The port the server chose for 0; one out of range is a refusal. */
	if (accepted && forward->port == 0) {
		ssh_reader_init(&reader, engine->payload.data, engine->payload.size);
		(void)ssh_reader_get_u8(&reader);
		chosen = ssh_reader_get_u32(&reader);
		error = ssh_reader_error(&reader);
		if (error != 0)
			return EPROTO;
		forward->bound = chosen;
		if (chosen == 0 || chosen > 65535U)
			accepted = 0;
	}

	/* Keeps where the forwarding is. */
	forward->state = ENGINE_FORWARD_REFUSED;
	if (accepted)
		forward->state = ENGINE_FORWARD_ACCEPTED;

	/* Tells the caller. */
	event = engine_event(engine, SSH_ENGINE_FORWARD_REPLY);
	if (event != NULL) {
		event->forward = entry;
		event->accepted = accepted;
		event->port = forward->port;
		if (accepted)
			event->port = forward->bound;
	}

	/* Succeeded: the answer is handled. */
	return 0;
}

/*
 * Finds the remote forwarding a forwarded-tcpip channel is for: the
 * address (size bytes) as asked for and the port listened on (the one
 * asked for, from the moment of asking, or once accepted the one the
 * server chose for 0).  Returns 1 with *forward its number, or 0.
 */
static int
engine_find_forward(
	const struct ssh_engine *engine,
	const uint8_t *address,
	size_t size,
	uint32_t port,
	unsigned *forward)
{
	const struct engine_forward *asked;
	size_t length;
	unsigned listened;
	unsigned index;
	int compare;

	/* Looks at each forwarding asked for. */
	for (index = 0; index < engine->forward_count; index++) {
		asked = &engine->forwards[index];

		/* A refused forwarding listens nowhere; one for port 0 not yet answered has no port yet. */
		if (asked->state == ENGINE_FORWARD_REFUSED)
			continue;
		if (asked->state == ENGINE_FORWARD_ASKED && asked->port == 0)
			continue;

		/* The port listened on must be the one connected. */
		listened = asked->port;
		if (asked->state == ENGINE_FORWARD_ACCEPTED)
			listened = asked->bound;
		if (listened != port)
			continue;

		/* The address must be the one asked for, byte for byte. */
		length = strlen(asked->address);
		if (length != size)
			continue;
		compare = memcmp(asked->address, address, size);
		if (compare != 0)
			continue;

		/* Found. */
		*forward = index;
		return 1;
	}

	/* No forwarding was asked for it. */
	return 0;
}

/* Counts the channels in use that carry kind. */
static unsigned
engine_count_kind(
	const struct ssh_engine *engine,
	enum engine_kind kind)
{
	unsigned index;
	unsigned count;

	/* Looks at each slot in use. */
	count = 0;
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		if (engine->channels.channel[index].state == SSH_CHANNEL_FREE)
			continue;
		if (engine->kinds[index] == kind)
			count++;
	}

	/* Reports the count. */
	return count;
}

/* Returns the most forwarded channels open at once: fewer on a connection that asks for X11 or subsystems, whose channels keep their room. */
static unsigned
engine_forward_channels_max(
	const struct ssh_engine *engine)
{
	unsigned limit;

	/* X11 keeps its share of the table. */
	limit = SSH_ENGINE_FORWARD_CHANNELS_MAX;
	if (engine->x11)
		limit = ENGINE_FORWARD_CHANNELS_X11;

	/* Subsystems keep their share when more_sessions is enabled. */
	if (engine->more_sessions)
		limit -= SSH_ENGINE_SUBSYSTEMS_MAX;

	/* Returns the limit. */
	return limit;
}

/* Appends a global request whose answer is awaited to the list (which the callers keep from overflowing). */
static void
engine_global_push(
	struct ssh_engine *engine,
	uint32_t entry)
{
	unsigned last;

	/* Refuses to overflow (the callers check the room first). */
	if (engine->global_count >= ENGINE_GLOBALS_MAX)
		return;

	/* Appends it after the last. */
	last = (engine->global_first + engine->global_count) % ENGINE_GLOBALS_MAX;
	engine->globals[last] = entry;
	engine->global_count++;
}

/* Reports whether the size bytes of a channel type are the type name (1 or 0). */
static int
engine_type_is(
	const uint8_t *type,
	size_t size,
	const char *name)
{
	size_t length;
	int compare;

	/* The lengths must be the same. */
	length = strlen(name);
	if (size != length)
		return 0;

	/* And the bytes. */
	compare = memcmp(type, name, size);
	if (compare != 0)
		return 0;

	/* The type is the name. */
	return 1;
}

/* Reports whether text is 0 (only when empty is 1) to most printable characters without blanks (1 or 0). */
static int
engine_text_ok(
	const char *text,
	size_t most,
	int empty)
{
	size_t length;
	size_t index;
	unsigned char byte;

	/* Checks the length. */
	length = strlen(text);
	if (length == 0 && !empty)
		return 0;
	if (length > most)
		return 0;

	/* Checks each byte. */
	for (index = 0; index < length; index++) {
		byte = (unsigned char)text[index];
		if (byte < 0x21U || byte > 0x7eU)
			return 0;
	}

	/* The text may be sent. */
	return 1;
}

/*
 * Sends x11-req on the session channel (RFC 4254 6.3.1): a reply wanted,
 * single connection as asked, the protocol MIT-MAGIC-COOKIE-1 with the
 * caller's fake cookie, and the first screen.  Returns 0 or an error of
 * sending.
 */
static int
engine_send_x11_request(
	struct ssh_engine *engine)
{
	int error;

	/* Builds the request in the secret buffer (the cookie stands for the X server's key). */
	ssh_buf_reset(&engine->out);
	ssh_buf_put_u8(&engine->out, SSH_MSG_CHANNEL_REQUEST);
	ssh_buf_put_u32(&engine->out, engine->channel->remote_id);
	ssh_buf_put_cstring(&engine->out, ENGINE_X11_REQUEST);
	ssh_buf_put_bool(&engine->out, 1);
	ssh_buf_put_bool(&engine->out, engine->x11_single);
	ssh_buf_put_cstring(&engine->out, ENGINE_X11_PROTOCOL);
	ssh_buf_put_cstring(&engine->out, engine->x11_cookie);
	ssh_buf_put_u32(&engine->out, ENGINE_X11_SCREEN);

	/* Sends it. */
	error = engine_send_out(engine);
	if (error != 0)
		return error;

	/* Succeeded: the request is on its way. */
	return 0;
}

/*
 * Returns the X11 or forwarded channel numbered number while it is open
 * or closing, or NULL (the session's, one waiting for its answer, a free
 * number).
 */
static struct ssh_channel *
engine_other_channel(
	struct ssh_engine *engine,
	uint32_t number)
{
	struct ssh_channel *channel;

	/* Finds the channel that carries data. */
	channel = ssh_channel_find(&engine->channels, number);
	if (channel == NULL)
		return NULL;

	/* The session's channel is not one of them. */
	if (channel == engine->channel)
		return NULL;

	/* Reports the channel. */
	return channel;
}

/*
 * Sends bytes on an open channel while there is room in its window and
 * in the output; *written receives how many went (0 after this side's EOF
 * or close).  Returns 0, or EPIPE when the connection ended on a failure
 * of sending.
 */
static int
engine_send_data(
	struct ssh_engine *engine,
	struct ssh_channel *channel,
	const uint8_t *bytes,
	size_t size,
	size_t *written)
{
	size_t pending;
	size_t room;
	size_t chunk;
	int error;

	/* Nothing goes on a channel that is not open, or after this side's EOF. */
	*written = 0;
	if (channel->state != SSH_CHANNEL_OPEN || channel->eof_sent)
		return 0;

	/* Sends while there is input, room in the window and in the output. */
	while (*written < size) {
		room = ssh_channel_send_room(channel);
		(void)ssh_transport_pending(&engine->transport, &pending);
		if (room == 0 ||
		    engine->backlog.size > 0 ||
		    pending >= ENGINE_OUTPUT_HIGH)
			break;

		/* Sends one piece the window takes. */
		chunk = size - *written;
		if (chunk > room)
			chunk = room;
		ssh_buf_reset(&engine->out);
		error = ssh_channel_put_data(channel, 0, bytes + *written, chunk, &engine->out);
		if (error == 0)
			error = engine_send_out(engine);
		if (error != 0) {
			engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
			return EPIPE;
		}

		/* Steps over what went. */
		*written += chunk;
	}

	/* Succeeded: *written bytes are on their way. */
	return 0;
}

/*
 * Counts size bytes of what the caller holds of a channel (*held) as
 * used, and gives the channel's window back once the caller holds less
 * than half of it and half of the window is used (ssh_channel_consumed
 * decides).  channel may be NULL after it closed.
 */
static void
engine_give_window(
	struct ssh_engine *engine,
	struct ssh_channel *channel,
	size_t *held,
	size_t size)
{
	int error;

	/* Counts what the caller used of what it holds (never more than it holds). */
	if (size > *held)
		size = *held;
	*held -= size;

	/* A closed connection or channel takes no window back. */
	if (engine->phase == ENGINE_PHASE_CLOSED ||
	    channel == NULL ||
	    size == 0)
		return;

	/* Waits while the caller still holds half a window or more: the window refills whole. */
	if (*held >= channel->window_size / 2U)
		return;

	/* Gives the window back when half of it is used. */
	ssh_buf_reset(&engine->out);
	ssh_channel_consumed(channel, size, &engine->out);
	if (engine->out.size == 0)
		return;
	error = engine_send_out(engine);
	if (error != 0)
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
}

/* Reports whether any channel is in use: 1, or 0 when every slot is free. */
static int
engine_channels_left(
	const struct ssh_engine *engine)
{
	unsigned index;

	/* Looks for a slot in any state but free. */
	for (index = 0; index < SSH_CHANNEL_MAX; index++) {
		if (engine->channels.channel[index].state != SSH_CHANNEL_FREE)
			return 1;
	}

	/* Reports that no channel is left. */
	return 0;
}

/*
 * Ends the connection, as the ssh program does, once the session is over
 * and no X11 channel is left.
 */
static void
engine_finish(
	struct ssh_engine *engine)
{
	int left;

	/* Waits for the session. */
	if (!engine->session_over)
		return;

	/* Waits for the X11 channels the session leaves. */
	left = engine_channels_left(engine);
	if (left)
		return;

	/* Says goodbye. */
	engine_end(engine, 0, SSH_DISCONNECT_BY_APPLICATION, ENGINE_BYE);
}

/*
 * Copies size bytes of an originator address into destination (capacity
 * SSH_ENGINE_ADDRESS_MAX + 1) as text: cut, and every byte that is not a
 * printable ASCII character written as '?'.
 */
static void
engine_copy_address(
	char *destination,
	const uint8_t *source,
	size_t size)
{
	size_t index;

	/* Cuts what does not fit. */
	if (size > SSH_ENGINE_ADDRESS_MAX)
		size = SSH_ENGINE_ADDRESS_MAX;

	/* Copies each byte, made safe. */
	for (index = 0; index < size; index++) {
		destination[index] = '?';
		if (source[index] >= 0x20U && source[index] < 0x7fU)
			destination[index] = (char)source[index];
	}

	/* Ends the text. */
	destination[size] = '\0';
}

/* Appends number in decimal to the NUL-terminated text (capacity bytes), cut to the room. */
static void
engine_append_number(
	char *text,
	size_t capacity,
	unsigned number)
{
	char digits[16];
	size_t count;
	size_t used;

	/* Writes the digits from the last. */
	count = 0;
	do {
		digits[count] = (char)('0' + number % 10U);
		count++;
		number /= 10U;
	} while (number != 0);

	/* Appends them in order while there is room. */
	used = strlen(text);
	while (count > 0 && used + 1U < capacity) {
		count--;
		text[used] = digits[count];
		used++;
	}

	/* Ends the text. */
	text[used] = '\0';
}

/* Appends size bytes to the NUL-terminated text (capacity bytes) made safe: every byte that is not a printable ASCII character as '?', cut to the room. */
static void
engine_append_safe(
	char *text,
	size_t capacity,
	const uint8_t *source,
	size_t size)
{
	size_t used;
	size_t index;

	/* Appends each byte while there is room. */
	used = strlen(text);
	for (index = 0; index < size && used + 1U < capacity; index++) {
		text[used] = '?';
		if (source[index] >= 0x20U && source[index] < 0x7fU)
			text[used] = (char)source[index];
		used++;
	}

	/* Ends the text. */
	text[used] = '\0';
}
