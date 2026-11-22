/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client side of one SSH connection as a state machine without I/O.
 *
 * Written from RFC 4253 section 10 (the service request), RFC 4252
 * sections 5 to 8 (the authentication requests and their answers), RFC
 * 4254 sections 5 and 6 (the session channel, its requests, its data and
 * its end), RFC 4254 section 8 and RFC 8160 (the terminal modes), and
 * ssh_config(5) of OpenSSH 10.5p1 (ServerAliveInterval,
 * ServerAliveCountMax, StrictHostKeyChecking, BatchMode).  The order of the
 * login and of the session is the one the ssh program follows; the
 * difference is that every wait is a state the engine returns from.
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
	ENGINE_REPLY_START = 1
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
 * handed out that the caller has not reported used.  pty_refused,
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
static int engine_refuse_open(struct ssh_engine *engine);
static int engine_send_eof(struct ssh_engine *engine);
static int engine_keep_alive(struct ssh_engine *engine);
static int engine_put_mode(struct ssh_buf *out, unsigned opcode, uint32_t value);
static int engine_method_offered(const struct ssh_engine *engine, const char *method);
static const char *engine_open_reason(uint32_t reason);

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
	ssh_channels_init(&made->channels);

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
	if (engine->alive_interval != 0 && engine->phase == ENGINE_PHASE_CONNECTED && engine->alive_due < *wake_ms)
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
	size_t pending;
	size_t room;
	size_t chunk;
	int error;

	/* Starts with nothing sent. */
	*written = 0;

	/* Nothing goes after the end. */
	if (engine->phase == ENGINE_PHASE_CLOSED)
		return EPIPE;

	/* Nothing goes before the session started, or after the input ended. */
	if (engine->channel == NULL || !engine->started || engine->eof_wanted)
		return 0;
	if (engine->channel->state != SSH_CHANNEL_OPEN || engine->channel->eof_sent)
		return 0;

	/* Sends while there is input, room in the window and in the output. */
	while (*written < size) {
		room = ssh_channel_send_room(engine->channel);
		(void)ssh_transport_pending(&engine->transport, &pending);
		if (room == 0 || engine->backlog.size > 0 || pending >= ENGINE_OUTPUT_HIGH)
			break;

		/* Sends one piece the window takes. */
		chunk = size - *written;
		if (chunk > room)
			chunk = room;
		ssh_buf_reset(&engine->out);
		error = ssh_channel_put_data(engine->channel, 0, bytes + *written, chunk, &engine->out);
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
 * Reports data used, so that the window may be given back.
 */
void
ssh_engine_consumed(
	struct ssh_engine *engine,
	size_t size)
{
	int error;

	/* Counts what the caller used of what it holds (never more than it holds). */
	if (size > engine->unconsumed)
		size = engine->unconsumed;
	engine->unconsumed -= size;

	/* A closed connection or channel takes no window back. */
	if (engine->phase == ENGINE_PHASE_CLOSED || engine->channel == NULL || size == 0)
		return;

	/* Waits while the caller still holds half a window or more: the window refills whole. */
	if (engine->unconsumed >= SSH_CHANNEL_WINDOW / 2U)
		return;

	/* Gives the window back when half of it is used (ssh_channel_consumed decides). */
	ssh_buf_reset(&engine->out);
	ssh_channel_consumed(engine->channel, size, &engine->out);
	if (engine->out.size == 0)
		return;
	error = engine_send_out(engine);
	if (error != 0)
		engine_end(engine, error, SSH_DISCONNECT_BY_APPLICATION, "cannot send");
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
	if (!engine->pty || engine->pty_refused || engine->channel == NULL || !engine->started)
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
	engine->identity_count = options->identity_count;

	/* The session and its terminal. */
	engine->session = options->session;
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
	while (engine->pubkey && offered && engine->identity_next < engine->identity_count) {
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
	uint32_t reason;
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
		/* The answer to a keepalive: the server is alive, which receiving it already noted. */
		break;
	case SSH_MSG_CHANNEL_OPEN:
		error = engine_refuse_open(engine);
		break;
	case SSH_MSG_CHANNEL_OPEN_CONFIRMATION:
	case SSH_MSG_CHANNEL_OPEN_FAILURE:
		/* A refused session ends the connection; an accepted one starts. */
		error = ssh_channel_open_reply(&engine->channels, engine->payload.data, engine->payload.size, &channel, &reason);
		if (error == EACCES) {
			engine->channel = NULL;
			engine_copy_text(engine->session_reason, sizeof(engine->session_reason), engine_open_reason(reason));
			engine_end(engine, ECONNREFUSED, SSH_DISCONNECT_BY_APPLICATION, engine->session_reason);
			error = 0;
			break;
		}

		/* Fails when the message could not be read. */
		if (error != 0) {
			error = EPROTO;
			break;
		}

		/* Starts the session on the open channel. */
		error = engine_opened(engine);
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

	/* Tells the server no other session will be opened on this connection (OpenSSH PROTOCOL 2.2). */
	ssh_buf_reset(&engine->out);
	ssh_channel_put_no_more_sessions(&engine->out);
	error = engine_send_out(engine);
	if (error != 0)
		return error;

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
	size_t size;
	uint32_t extended;
	int error;

	/* Reads the data and takes it from the window. */
	error = ssh_channel_parse_data(&engine->channels, engine->payload.data, engine->payload.size, &channel, &data, &size, &extended);
	if (error != 0)
		return EPROTO;

	/* Drops data of a closing channel and of a type other than the error output, giving its window back unless the caller holds half a window. */
	if (channel->close_sent || (extended != 0 && extended != SSH_EXTENDED_DATA_STDERR)) {
		if (engine->unconsumed >= SSH_CHANNEL_WINDOW / 2U)
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
	engine->unconsumed += size;
	event = engine_event(engine, SSH_ENGINE_DATA);
	if (event != NULL) {
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
 * the session (SSH_ENGINE_EXIT) and the connection.
 */
static int
engine_take_end(
	struct ssh_engine *engine)
{
	struct ssh_engine_event *event;
	struct ssh_channel *channel;
	int freed;
	int error;

	/* Reads which end it is. */
	error = ssh_channel_parse_end(&engine->channels, engine->payload.data, engine->payload.size, &channel);
	if (error != 0)
		return EPROTO;

	/* An EOF only says the server sends no more data. */
	if (!channel->close_received)
		return 0;

	/* Answers the server's CLOSE with this side's (RFC 4254 5.3). */
	if (!channel->close_sent) {
		ssh_buf_reset(&engine->out);
		ssh_channel_put_close(channel, &engine->out);
		error = engine_send_out(engine);
		if (error != 0)
			return error;
	}

	/* Frees the channel when both sides have closed it. */
	freed = ssh_channel_closed(&engine->channels, channel);
	if (!freed || channel != engine->channel)
		return 0;
	engine->channel = NULL;

	/* Tells how the session ended. */
	event = engine_event(engine, SSH_ENGINE_EXIT);
	if (event != NULL) {
		event->exit_status = -1;
		if (engine->have_exit)
			event->exit_status = engine->exit_status;
		engine_copy_text(event->exit_signal, sizeof(event->exit_signal), engine->exit_signal);
		engine_copy_text(event->reason, sizeof(event->reason), engine->session_reason);
	}

	/* Ends the connection with the session, as the ssh program does. */
	engine_end(engine, 0, SSH_DISCONNECT_BY_APPLICATION, ENGINE_BYE);

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

	/* Keeps how the command ended. */
	if (request.type == SSH_CREQ_EXIT_STATUS) {
		engine->have_exit = 1;
		engine->exit_status = (int)(request.status & 0x7fffffffU);
	}

	/* Keeps the signal that ended the command. */
	if (request.type == SSH_CREQ_EXIT_SIGNAL)
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

	/* Refuses a reply nothing waits for. */
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
 * server's channel number): a client offers no channel.
 */
static int
engine_refuse_open(
	struct ssh_engine *engine)
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

	/* Answers OPEN_FAILURE, administratively prohibited. */
	ssh_buf_reset(&engine->out);
	ssh_buf_put_u8(&engine->out, SSH_MSG_CHANNEL_OPEN_FAILURE);
	ssh_buf_put_u32(&engine->out, sender);
	ssh_buf_put_u32(&engine->out, SSH_OPEN_ADMINISTRATIVELY_PROHIBITED);
	ssh_buf_put_cstring(&engine->out, "open refused");
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
	if (!engine->eof_wanted || !engine->started || engine->channel == NULL)
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
 * unanswered (ssh_config(5)).
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

	/* Sends one that wants a reply. */
	ssh_buf_reset(&engine->out);
	ssh_channel_put_global_request(ENGINE_KEEPALIVE, 1, &engine->out);
	error = engine_send_out(engine);
	if (error != 0)
		return error;

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
engine_open_reason(
	uint32_t reason)
{
	/*
	 * Values referenced: RFC 4254 5.1 (the reason codes of
	 * SSH_MSG_CHANNEL_OPEN_FAILURE) and RFC 4250 4.3 (their names).
	 */
	switch (reason) {
	case SSH_OPEN_ADMINISTRATIVELY_PROHIBITED:
		return "channel open failed: administratively prohibited";
	case SSH_OPEN_CONNECT_FAILED:
		return "channel open failed: connect failed";
	case SSH_OPEN_UNKNOWN_CHANNEL_TYPE:
		return "channel open failed: unknown channel type";
	case SSH_OPEN_RESOURCE_SHORTAGE:
		return "channel open failed: resource shortage";
	default:
		return "channel open failed: unknown reason";
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
