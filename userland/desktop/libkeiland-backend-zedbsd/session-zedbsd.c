/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session on zedBSD: sessiond's lines (ws035-p095, ws035-p101,
 * ws035-p102; libkeiland-backend since ws131-p006).
 *
 * sessiond starts the compositor with one descriptor: the login screen's
 * (--auth-fd, options.greeter_descriptor) or a session's (--control-fd,
 * options.session_descriptor).  Every message is a line:
 *
 *   READY                   GO: the display may be taken
 *   STYLES name             STYLES password[ pin][ fido2] (login screen; a
 *                           session's STYLES has no name)
 *   AUTH name style         then the secret's line: OK, the user is in;
 *                           FAIL reason; ERROR (login screen)
 *   UNLOCK style            then the secret's line: OK; FAIL reason (a
 *                           session's lock screen)
 *   ENROLLED                ENROLLED pin=0|1 fido2=N[ key=REF/LABEL...]
 *                           (a session; the label's bytes in hexadecimal)
 *   ENROLL pin / REMOVE pin then the password's line (and the PIN's for
 *                           ENROLL): OK; FAIL reason (a session)
 *   ENROLL fido2 LABEL      then the password's and the key PIN's lines:
 *                           TOUCH..., OK id=ID; FAIL reason (ws172-p003)
 *   KEYINFO                 KEYINFO count=N [name=HEX pin= retries= min=]
 *   KEYOWNER                KEYOWNER user=NAME key-pin= key-touch= card=, or
 *                           KEYOWNER REASON (login screen and session, ws199-p001)
 *   KEYPIN set|change       then the PINs' lines: OK; FAIL reason
 *   KEYRESET                then the password's line: REPLUG, TOUCH...,
 *                           OK removed=N; FAIL reason (ws199-p001)
 *   REMOVE fido2 REF        then the password's line: OK; FAIL reason
 *   CANCEL                  (none): a security key's attempt stops
 *   POWER poweroff|reboot   OK; FAIL others; ERROR (login screen and session, power-zedbsd.c)
 *   SERVICE sshd on|off|status  SERVICE available= ...; DENIED; ERROR (a session, sharing-zedbsd.c)
 *   LOGOUT                  QUIT: the greeter is up, the session ends
 *   LOCKED, UNLOCKED        (none): the session's lock screen shows, or has
 *                           gone (a session that crashes starts again with
 *                           it then)
 *   RELEASED                (none): the display has been given back
 *   ADMIN ASK id client locale streams action [word...]
 *                           ADMIN TITLE, ADMIN MESSAGE and ADMIN SHOW (the
 *                           dialog); ADMIN CONNECT; ADMIN STARTED id bits
 *                           with the program's streams (SCM_RIGHTS);
 *                           ADMIN REFUSED id reason; ADMIN FAILED id errno
 *   ADMIN ALLOW id          (with the connection lent to a program of the
 *                           desktop): ADMIN STARTED; ADMIN FAILED
 *   ADMIN CANCEL id, ADMIN FORGET client   (none)
 *
 * READY is said just before the compositor first takes the display, when
 * everything slow is done, and GO is awaited: sessiond sends it once the
 * greeter has ended and let the display go, so the screen goes from the
 * greeter's last frame to the desktop's first without the text console
 * between them.  Without GO in time (another sessiond, or none) the
 * display is taken anyway.
 *
 * The login screen ends when sessiond shuts its side of the socket down
 * (the session is ready to take the display); a session ends on QUIT.  In
 * both the compositor gives the display back first and then RELEASED is
 * said, before the slower rest of its end.  When sessiond closes a
 * session's descriptor, the session carries on without it.
 *
 * The descriptors do not block: the tick reads what has come, cuts it into
 * lines and answers the request awaited (one at a time).  A secret goes on
 * a line of its own after its request (ws172-p002), and TOUCH lines (a
 * security key waits to be touched, ws172-p003) answer nothing yet: the
 * host hears session_answer(KL_BACKEND_SESSION_TOUCH, 0) for each.
 *
 * The admin requests' answers all start with ADMIN and name their
 * request, so they answer no other request, and the other requests do not
 * wait for them: one admin answer is awaited at a time, beside the one of
 * the other requests, for KL_BACKEND_ADMIN_WAIT_MS at most.  The
 * descriptors the manager sends with a line (a program's streams) are read
 * with recvmsg and kept in their order until the STARTED line that names
 * them; any other is closed.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <poll.h>
#include "power-outcome.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

/* How long the compositor waits for GO, and for QUIT after LOGOUT (milliseconds). */
#define SESSION_WAIT_MS 20000U
#define SESSION_LOGOUT_MS 30000U

/* The longest request the compositor sends (ENROLL with its password's and PIN's lines), and the longest secret. */
#define SESSION_REQUEST_MAX 1024U
#define SESSION_SECRET_MAX 256U

/* An admin request's bounds: an action's name, one word, the words joined, how many words, and a locale (without their NULs). */
#define SESSION_ADMIN_ACTION_MAX 32U
#define SESSION_ADMIN_WORD_MAX 64U
#define SESSION_ADMIN_WORDS_MAX 256U
#define SESSION_ADMIN_WORD_COUNT 8U
#define SESSION_ADMIN_LOCALE_MAX 16U

/* What a line of sessiond's answers (but QUIT and TOUCH). */
enum session_answer {
	SESSION_ANSWER_UNKNOWN,
	SESSION_ANSWER_SERVICE,
	SESSION_ANSWER_STYLES,
	SESSION_ANSWER_ENROLLED,
	SESSION_ANSWER_KEYINFO,
	SESSION_ANSWER_KEYOWNER,
	SESSION_ANSWER_OK,
	SESSION_ANSWER_FAIL,
	SESSION_ANSWER_BUSY,
	SESSION_ANSWER_ERROR
};

/* The answers' words: a line is the answer when it is the word, or starts with the word and a space. */
struct session_word {
	const char *word;
	enum session_answer kind;
};

/* The words, the longer before the shorter they start with. */
static const struct session_word session_words[] = {
	{ "STYLES", SESSION_ANSWER_STYLES },
	{ "ENROLLED", SESSION_ANSWER_ENROLLED },
	{ "KEYINFO", SESSION_ANSWER_KEYINFO },
	{ "KEYOWNER", SESSION_ANSWER_KEYOWNER },
	{ "OK", SESSION_ANSWER_OK },
	{ "FAIL", SESSION_ANSWER_FAIL },
	{ "ERROR busy", SESSION_ANSWER_BUSY },
	{ "ERROR", SESSION_ANSWER_ERROR },
};

static int session_descriptor(const struct kl_backend *backend);
static enum session_answer session_kind(const char *line);
static const char *session_style_word(unsigned style);
static int session_secret_valid(const char *secret);
static int session_name_valid(const char *name);
static int session_ask(struct kl_backend *backend, unsigned request, char *line, int length);
static void session_take_styles(struct kl_backend *backend, const char *list);
static void session_take_enrolled(struct kl_backend *backend, const char *list);
static void session_take_key_info(struct kl_backend *backend, const char *list);
static void session_take_key_owner(struct kl_backend *backend, const char *list);
static int session_take_key(const char *word, struct kl_backend_key *key);
static int session_hex_value(char digit);
static int session_send(struct kl_backend *backend, unsigned request, const char *line);
static void session_answered(struct kl_backend *backend, const char *line);
static void session_slept(struct kl_backend *backend, const char *line);
static void session_end(struct kl_backend *backend, unsigned reason);
static uint64_t session_milliseconds(void);
static ssize_t session_receive(struct kl_backend *backend, int descriptor);
static void session_hold(struct kl_backend *backend, struct msghdr *header);
static int session_admin_usable(const struct kl_backend *backend);
static int session_admin_action_valid(const char *action);
static int session_admin_words_valid(const char *words);
static int session_admin_word_byte(char byte);
static int session_admin_locale_valid(const char *locale);
static int session_admin_send(struct kl_backend *backend, const char *line, int connection);
static void session_admin_line(struct kl_backend *backend, const char *line);
static const char *session_admin_verb(const char *text, const char *verb);
static int session_admin_number(const char *text, unsigned base, unsigned long limit, unsigned long *value, const char **rest);
static int session_admin_id(const char *text, uint32_t *id, const char **rest);
static int session_admin_show(const char *rest, struct kl_backend_admin_answer *answer);
static int session_admin_refused(const char *rest, struct kl_backend_admin_answer *answer);
static void session_admin_take(struct kl_backend *backend, struct kl_backend_admin_answer *answer);
static void session_admin_give(struct kl_backend *backend, struct kl_backend_admin_answer *answer);
static void session_admin_late(struct kl_backend *backend, struct kl_backend_admin_answer *answer);
static void session_admin_fail(struct kl_backend *backend, int error);
static void session_admin_drop(struct kl_backend *backend);

/*
 * Says READY and waits for GO, before the display is first taken.
 */
int
kl_backend_session_ready(
	struct kl_backend *backend)
{
	struct pollfd entry;
	uint64_t started;
	uint64_t waited;
	char line[KL_BACKEND_SESSION_LINE];
	size_t used;
	ssize_t count;
	char byte;
	int descriptor;
	int status;
	int same;
	int whole;

	/* The login screen's descriptor, or the session's; none when sessiond did not start the compositor. */
	if (backend == NULL)
		return EINVAL;
	descriptor = session_descriptor(backend);
	if (descriptor < 0)
		return ENOTSUP;

	/* READY. */
	count = write(descriptor, "READY\n", 6U);
	if (count < 0)
		return errno;
	if (count != 6)
		return EIO;

	/*
	 * GO, a line of its own, read a byte at a time (nothing after it is
	 * taken).  A line before it answers a request asked before READY (the
	 * login screen's STYLES, ws172-p002): it is kept for the tick.
	 */
	started = session_milliseconds();
	used = 0;
	whole = 1;
	for (;;) {
		/* The time left. */
		waited = session_milliseconds() - started;
		if (waited >= SESSION_WAIT_MS)
			break;

		/* A byte, or the end of the wait. */
		entry.fd = descriptor;
		entry.events = POLLIN;
		entry.revents = 0;
		status = poll(&entry, 1, (int)(SESSION_WAIT_MS - waited));
		if (status < 0 && errno == EINTR)
			continue;

		/* Ends the wait when polling failed or its time expired. */
		if (status <= 0)
			break;

		/* Reads only the byte that polling made available. */
		count = read(descriptor, &byte, 1U);
		if (count < 0 &&
		    (errno == EINTR ||
		    errno == EAGAIN))
			continue;

		/* Ends the wait when the peer closed or a read failed. */
		if (count <= 0)
			break;

		/* A whole line is looked at; a long one is thrown away. */
		if (byte != '\n') {
			/* Keeps a bounded line while passing over excess bytes. */
			if (used + 1U < sizeof(line))
				line[used++] = byte;
			else
				whole = 0;
			continue;
		}

		/* The line ends here. */
		line[used] = '\0';
		same = strcmp(line, "GO");
		if (same == 0)
			return 0;

		/* Another line is the tick's, as if it came after GO (one too long, or that does not fit, is thrown away). */
		if (whole && backend->session_used + used + 1U < sizeof(backend->session_line)) {
			memcpy(backend->session_line + backend->session_used, line, used);
			backend->session_used += used;
			backend->session_line[backend->session_used++] = '\n';
			backend->session_line[backend->session_used] = '\0';
		}

		/* The next line starts empty. */
		used = 0;
		whole = 1;
	}

	/* The display is taken without GO. */
	return ETIMEDOUT;
}

/*
 * Asks sessiond for a greeter (LOGOUT); the session goes on until QUIT.
 */
int
kl_backend_session_logout(
	struct kl_backend *backend)
{
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* Once: the answer of the first is awaited. */
	if (backend->logout_asked)
		return 0;

	/* LOGOUT; its answer is QUIT, not a request's answer. */
	error = session_send(backend, KL_BACKEND_SESSION_NONE, "LOGOUT\n");
	if (error != 0)
		return error;

	/* Succeeded: the next tick starts the deadline. */
	backend->logout_asked = 1U;
	backend->logout_ms = 0U;
	return 0;
}

/*
 * Tells sessiond that the session's lock screen shows (LOCKED) or has gone
 * (UNLOCKED); it has no answer.
 */
int
kl_backend_session_lock_state(
	struct kl_backend *backend,
	unsigned locked)
{
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The word, which asks for nothing. */
	error = session_send(backend, KL_BACKEND_SESSION_NONE, locked ? "LOCKED\n" : "UNLOCKED\n");
	return error;
}

/*
 * Asks sessiond to log a user in (AUTH name style, then the secret).
 */
int
kl_backend_session_authenticate(
	struct kl_backend *backend,
	const char *user,
	unsigned style,
	const char *secret)
{
	char line[SESSION_REQUEST_MAX];
	const char *word;
	int valid;
	int length;
	int error;

	/* Only the login screen asks, with a name of one word and a secret of one line. */
	if (backend == NULL || user == NULL || secret == NULL)
		return EINVAL;
	if (backend->options.greeter_descriptor < 0)
		return ENOTSUP;
	valid = session_name_valid(user);
	if (!valid)
		return EINVAL;
	word = session_style_word(style);
	if (word == NULL)
		return EINVAL;
	valid = session_secret_valid(secret);
	if (!valid)
		return EINVAL;

	/* The request and its secret; nothing of the secret is kept once it is sent. */
	length = snprintf(line, sizeof(line), "AUTH %s %s\n%s\n", user, word, secret);
	error = session_ask(backend, KL_BACKEND_SESSION_AUTH, line, length);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Asks sessiond to unlock the session's lock screen (UNLOCK style, then the secret).
 */
int
kl_backend_session_unlock(
	struct kl_backend *backend,
	unsigned style,
	const char *secret)
{
	char line[SESSION_REQUEST_MAX];
	const char *word;
	int valid;
	int length;
	int error;

	/* Only a session sessiond started and still listens to, with a secret of one line. */
	if (backend == NULL || secret == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;
	word = session_style_word(style);
	if (word == NULL)
		return EINVAL;
	valid = session_secret_valid(secret);
	if (!valid)
		return EINVAL;

	/* The request and its secret; nothing of the secret is kept once it is sent. */
	length = snprintf(line, sizeof(line), "UNLOCK %s\n%s\n", word, secret);
	error = session_ask(backend, KL_BACKEND_SESSION_UNLOCK, line, length);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Asks sessiond which styles a user may use now (STYLES).
 */
int
kl_backend_session_styles(
	struct kl_backend *backend,
	const char *user)
{
	char line[SESSION_REQUEST_MAX];
	int valid;
	int length;
	int error;

	/* The login screen names the user; a session asks for its own. */
	if (backend == NULL)
		return EINVAL;
	if (backend->options.greeter_descriptor >= 0) {
		valid = 0;
		if (user != NULL)
			valid = session_name_valid(user);
		if (!valid)
			return EINVAL;
		length = snprintf(line, sizeof(line), "STYLES %s\n", user);
	} else if (backend->options.session_descriptor >= 0 && !backend->session_gone) {
		length = snprintf(line, sizeof(line), "STYLES\n");
	} else {
		return ENOTSUP;
	}

	/* The request; the answer comes through session_answer. */
	error = session_ask(backend, KL_BACKEND_SESSION_STYLES, line, length);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Gives the styles sessiond last answered (the password alone before any answer).
 */
unsigned
kl_backend_session_styles_get(
	const struct kl_backend *backend)
{
	/* No answer yet: the password, which every account has. */
	if (backend == NULL || backend->session_styles == 0U)
		return KL_BACKEND_STYLE_PASSWORD;

	/* Succeeded: the styles of the last answer. */
	return backend->session_styles;
}

/*
 * Asks sessiond to set the session user's PIN (ENROLL pin), or to remove it (REMOVE pin).
 */
int
kl_backend_session_set_pin(
	struct kl_backend *backend,
	const char *password,
	const char *pin)
{
	char line[SESSION_REQUEST_MAX];
	int valid;
	int length;
	int error;

	/* Only a session sessiond started and still listens to, with secrets of one line each. */
	if (backend == NULL || password == NULL || pin == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;
	valid = session_secret_valid(password);
	if (!valid || password[0] == '\0')
		return EINVAL;
	valid = session_secret_valid(pin);
	if (!valid)
		return EINVAL;

	/* A new PIN with the password, or the removal with the password alone. */
	if (pin[0] != '\0') {
		length = snprintf(line, sizeof(line), "ENROLL pin\n%s\n%s\n", password, pin);
	} else {
		length = snprintf(line, sizeof(line), "REMOVE pin\n%s\n", password);
	}

	/* The request; nothing of the secrets is kept once it is sent. */
	error = session_ask(backend, KL_BACKEND_SESSION_ENROLL, line, length);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Asks sessiond what the session user has enrolled (ENROLLED).
 */
int
kl_backend_session_enrolled(
	struct kl_backend *backend)
{
	char line[SESSION_REQUEST_MAX];
	int length;
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The request; the answer comes through session_answer. */
	length = snprintf(line, sizeof(line), "ENROLLED\n");
	error = session_ask(backend, KL_BACKEND_SESSION_ENROLLED, line, length);
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Gives what sessiond last answered to ENROLLED.
 */
void
kl_backend_session_enrolled_get(
	const struct kl_backend *backend,
	unsigned *pin,
	unsigned *keys)
{
	/* Nothing before an answer. */
	*pin = 0U;
	*keys = 0U;
	if (backend == NULL)
		return;

	/* The last answer. */
	*pin = backend->session_pin;
	*keys = backend->session_keys;
}

/*
 * Gives the keys of the last ENROLLED answer.
 */
size_t
kl_backend_session_keys_get(
	const struct kl_backend *backend,
	struct kl_backend_key *keys,
	size_t capacity)
{
	size_t count;

	/* Nothing before an answer. */
	if (backend == NULL)
		return 0U;

	/* As many as fit. */
	count = backend->session_key_count;
	if (count > capacity)
		count = capacity;
	memcpy(keys, backend->session_key_list, count * sizeof(keys[0]));
	return backend->session_key_count;
}

/*
 * Asks sessiond to register the key plugged in for the session user (ENROLL fido2).
 */
int
kl_backend_session_add_key(
	struct kl_backend *backend,
	const char *password,
	const char *label,
	const char *pin)
{
	char line[SESSION_REQUEST_MAX];
	const char *colon;
	size_t length;
	int valid;
	int written;
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL || password == NULL || label == NULL || pin == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The password and the key's PIN, one line each; a label of 1 to 32 bytes without a colon. */
	valid = session_secret_valid(password);
	if (!valid || password[0] == '\0')
		return EINVAL;
	valid = session_secret_valid(pin);
	if (!valid || pin[0] == '\0')
		return EINVAL;
	valid = session_secret_valid(label);
	length = strlen(label);
	colon = strchr(label, ':');
	if (!valid || length == 0U || length >= KL_BACKEND_KEY_LABEL || colon != NULL)
		return EINVAL;

	/* The request; nothing of the secrets is kept once it is sent. */
	written = snprintf(line, sizeof(line), "ENROLL fido2 %s\n%s\n%s\n", label, password, pin);
	error = session_ask(backend, KL_BACKEND_SESSION_ENROLL, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Asks sessiond to remove one of the session user's keys (REMOVE fido2).
 */
int
kl_backend_session_remove_key(
	struct kl_backend *backend,
	const char *password,
	const char *ref)
{
	char line[SESSION_REQUEST_MAX];
	size_t length;
	size_t index;
	int valid;
	int written;
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL || password == NULL || ref == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The password, and a reference of 16 small hexadecimal digits. */
	valid = session_secret_valid(password);
	if (!valid || password[0] == '\0')
		return EINVAL;
	length = strlen(ref);
	if (length != KL_BACKEND_KEY_REF - 1U)
		return EINVAL;
	for (index = 0U; index < length; index++) {
		valid = session_hex_value(ref[index]);
		if (valid < 0)
			return EINVAL;
	}

	/* The request; nothing of the password is kept once it is sent. */
	written = snprintf(line, sizeof(line), "REMOVE fido2 %s\n%s\n", ref, password);
	error = session_ask(backend, KL_BACKEND_SESSION_ENROLL, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Gives the session user's key options as the last ENROLLED told them (ws199-p001).
 */
void
kl_backend_session_options_get(
	const struct kl_backend *backend,
	unsigned *key_pin,
	unsigned *key_touch)
{
	/* Asked, without a backend or an answer. */
	*key_pin = 1U;
	*key_touch = 1U;
	if (backend == NULL)
		return;

	/* Succeeded: the last answer's. */
	*key_pin = backend->session_key_pin;
	*key_touch = backend->session_key_touch;
}

/*
 * Asks sessiond to set the session user's key options (SETOPTIONS, ws199-p001).
 */
int
kl_backend_session_set_options(
	struct kl_backend *backend,
	const char *password,
	unsigned key_pin,
	unsigned key_touch)
{
	char line[SESSION_REQUEST_MAX];
	int valid;
	int written;
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL || password == NULL || key_pin > 1U || key_touch > 1U)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The password, one line; no touch only without the PIN. */
	valid = session_secret_valid(password);
	if (!valid || password[0] == '\0' || (key_pin == 1U && key_touch == 0U))
		return EINVAL;

	/* The request; nothing of the password is kept once it is sent. */
	written = snprintf(line, sizeof(line), "SETOPTIONS %u %u\n%s\n", key_pin, key_touch, password);
	error = session_ask(backend, KL_BACKEND_SESSION_ENROLL, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Gives the methods the login and locked screens take for the session user,
 * as the last ENROLLED told them (WS200).
 */
unsigned
kl_backend_session_methods_get(
	const struct kl_backend *backend)
{
	/* Every method, without a backend or before ENROLLED told them. */
	if (backend == NULL || backend->session_methods == 0U)
		return KL_BACKEND_METHODS_ALL;

	/* Succeeded: the last answer's. */
	return backend->session_methods;
}

/*
 * Asks sessiond to set the methods the login and locked screens take
 * (SETMETHODS, WS200): the password or a key among them.
 */
int
kl_backend_session_set_methods(
	struct kl_backend *backend,
	const char *password,
	unsigned methods)
{
	char line[SESSION_REQUEST_MAX];
	char words[32];
	const char *separator;
	size_t used;
	int valid;
	int written;
	int error;

	/* Only a session sessiond started and still listens to; known methods with a first sign-in. */
	if (backend == NULL || password == NULL)
		return EINVAL;
	if ((methods & ~KL_BACKEND_METHODS_ALL) != 0U)
		return EINVAL;
	if ((methods & (KL_BACKEND_METHOD_PASSWORD | KL_BACKEND_METHOD_KEY)) == 0U)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The password, one line. */
	valid = session_secret_valid(password);
	if (!valid || password[0] == '\0')
		return EINVAL;

	/* The methods' words, in their order. */
	words[0] = '\0';
	used = 0U;
	separator = "";
	if ((methods & KL_BACKEND_METHOD_PASSWORD) != 0U) {
		used += (size_t)snprintf(words + used, sizeof(words) - used, "%spassword", separator);
		separator = ",";
	}

	/* The PIN. */
	if ((methods & KL_BACKEND_METHOD_PIN) != 0U) {
		used += (size_t)snprintf(words + used, sizeof(words) - used, "%spin", separator);
		separator = ",";
	}

	/* A security key. */
	if ((methods & KL_BACKEND_METHOD_KEY) != 0U)
		(void)snprintf(words + used, sizeof(words) - used, "%sfido2", separator);

	/* The request; nothing of the password is kept once it is sent. */
	written = snprintf(line, sizeof(line), "SETMETHODS %s\n%s\n", words, password);
	error = session_ask(backend, KL_BACKEND_SESSION_ENROLL, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Asks sessiond what the security keys there are (KEYINFO, ws199-p001).
 */
int
kl_backend_session_key_info(
	struct kl_backend *backend)
{
	char line[SESSION_REQUEST_MAX];
	int written;
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The request. */
	written = snprintf(line, sizeof(line), "KEYINFO\n");
	error = session_ask(backend, KL_BACKEND_SESSION_KEYINFO, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Asks sessiond whose the security key there is (KEYOWNER, ws199-p001).
 */
int
kl_backend_session_key_owner(
	struct kl_backend *backend)
{
	char line[SESSION_REQUEST_MAX];
	int written;
	int error;

	/* The login screen, or a session sessiond started and still listens to. */
	if (backend == NULL)
		return EINVAL;
	if (backend->options.greeter_descriptor < 0 && (backend->options.session_descriptor < 0 || backend->session_gone))
		return ENOTSUP;

	/* The request. */
	written = snprintf(line, sizeof(line), "KEYOWNER\n");
	error = session_ask(backend, KL_BACKEND_SESSION_KEYOWNER, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Gives what sessiond last answered to KEYOWNER.
 */
void
kl_backend_session_key_owner_get(
	const struct kl_backend *backend,
	struct kl_backend_key_owner *owner)
{
	/* None without a backend. */
	memset(owner, 0, sizeof(*owner));
	if (backend == NULL)
		return;

	/* Succeeded: the last answer. */
	*owner = backend->session_key_owner;
}

/*
 * Gives what sessiond last answered to KEYINFO.
 */
void
kl_backend_session_key_info_get(
	const struct kl_backend *backend,
	struct kl_backend_key_info *info)
{
	/* None without a backend. */
	memset(info, 0, sizeof(*info));
	if (backend == NULL)
		return;

	/* Succeeded: the last answer. */
	*info = backend->session_key_info;
}

/*
 * Asks sessiond to set the key's first PIN (current NULL) or to change it (KEYPIN, ws199-p001).
 */
int
kl_backend_session_key_pin(
	struct kl_backend *backend,
	const char *current,
	const char *pin)
{
	char line[SESSION_REQUEST_MAX];
	int valid;
	int written;
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL || pin == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The PINs, one line each, not empty. */
	valid = session_secret_valid(pin);
	if (!valid || pin[0] == '\0')
		return EINVAL;
	if (current != NULL) {
		valid = session_secret_valid(current);
		if (!valid || current[0] == '\0')
			return EINVAL;
	}

	/* The request; nothing of the PINs is kept once it is sent. */
	if (current == NULL) {
		written = snprintf(line, sizeof(line), "KEYPIN set\n%s\n", pin);
	} else {
		written = snprintf(line, sizeof(line), "KEYPIN change\n%s\n%s\n", current, pin);
	}

	/* Sent. */
	error = session_ask(backend, KL_BACKEND_SESSION_KEYOP, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Asks sessiond to reset the key the user plugs in again (KEYRESET, ws199-p001).
 */
int
kl_backend_session_key_reset(
	struct kl_backend *backend,
	const char *password)
{
	char line[SESSION_REQUEST_MAX];
	int valid;
	int written;
	int error;

	/* Only a session sessiond started and still listens to. */
	if (backend == NULL || password == NULL)
		return EINVAL;
	if (backend->options.session_descriptor < 0 || backend->session_gone)
		return ENOTSUP;

	/* The password, one line, not empty. */
	valid = session_secret_valid(password);
	if (!valid || password[0] == '\0')
		return EINVAL;

	/* The request; nothing of the password is kept once it is sent. */
	backend->session_key_removed = 0U;
	written = snprintf(line, sizeof(line), "KEYRESET\n%s\n", password);
	error = session_ask(backend, KL_BACKEND_SESSION_KEYOP, line, written);
	if (error != 0)
		return error;

	/* Succeeded: the answer comes through session_answer. */
	return 0;
}

/*
 * Gives how many registrations the last reset removed.
 */
unsigned
kl_backend_session_key_removed(
	const struct kl_backend *backend)
{
	/* None without a backend. */
	if (backend == NULL)
		return 0U;

	/* Succeeded: the count. */
	return backend->session_key_removed;
}

/*
 * Stops a security key's attempt under way (CANCEL); its answer comes as the attempt's.
 */
int
kl_backend_session_cancel(
	struct kl_backend *backend)
{
	int descriptor;
	int error;

	/* The login screen's or a session's descriptor, while sessiond listens. */
	if (backend == NULL)
		return EINVAL;
	descriptor = session_descriptor(backend);
	if (descriptor < 0)
		return ENOTSUP;

	/* The line, awaiting nothing of its own. */
	error = session_send(backend, KL_BACKEND_SESSION_NONE, "CANCEL\n");
	if (error != 0)
		return error;

	/* Succeeded: said. */
	return 0;
}

/*
 * Gives the word of the last refusal.
 */
const char *
kl_backend_session_reason(
	const struct kl_backend *backend)
{
	/* None without a backend. */
	if (backend == NULL)
		return "";

	/* Succeeded: the word, empty when the refusal had none. */
	return backend->session_reason;
}

/*
 * Tells whether sessiond started this session and still listens.
 */
int
kl_backend_session_managed(
	const struct kl_backend *backend)
{
	/* No backend, the login screen, or a session sessiond left. */
	if (backend == NULL || backend->options.session_descriptor < 0 || backend->session_gone)
		return 0;

	/* Succeeded: it can be locked and logged out through sessiond. */
	return 1;
}

/*
 * Asks sessiond for an admin action (ADMIN ASK); its answer comes as session_admin.
 */
int
kl_backend_session_admin_ask(
	struct kl_backend *backend,
	uint32_t id,
	uint64_t client,
	const char *locale,
	unsigned streams,
	const char *action,
	const char *words)
{
	char line[KL_BACKEND_SESSION_LINE];
	const char *shown_locale;
	const char *space;
	int usable;
	int valid;
	int length;
	int error;

	/* Only a session sessiond started and still listens to asks, with a request's number. */
	if (backend == NULL || action == NULL || id == 0U)
		return EINVAL;
	usable = session_admin_usable(backend);
	if (!usable)
		return ENOTSUP;

	/* The streams of the three; the action's name, its words and the locale as sessiond reads them. */
	if ((streams & ~(KL_BACKEND_ADMIN_INPUT | KL_BACKEND_ADMIN_OUTPUT | KL_BACKEND_ADMIN_ERROR)) != 0U)
		return EINVAL;
	valid = session_admin_action_valid(action);
	if (!valid)
		return EINVAL;
	if (words == NULL)
		words = "";
	valid = session_admin_words_valid(words);
	if (!valid)
		return EINVAL;
	if (locale == NULL)
		locale = "";
	valid = session_admin_locale_valid(locale);
	if (!valid)
		return EINVAL;

	/* One admin answer is awaited at a time. */
	if (backend->admin_awaited != 0U)
		return EBUSY;

	/* No locale is written "-", and no words leave no space before them. */
	shown_locale = locale;
	if (locale[0] == '\0')
		shown_locale = "-";
	space = " ";
	if (words[0] == '\0')
		space = "";

	/* The request, whose bounds keep it within a line of sessiond's. */
	length = snprintf(line, sizeof(line), "ADMIN ASK %lu %llu %s %x %s%s%s\n",
	    (unsigned long)id, (unsigned long long)client, shown_locale, streams, action, space, words);
	if (length < 0 || (size_t)length >= sizeof(line))
		return EINVAL;
	error = session_admin_send(backend, line, -1);
	if (error != 0)
		return error;

	/* Succeeded: the answer is awaited, and its time starts at the next tick. */
	backend->admin_awaited = id;
	backend->admin_due_ms = 0U;
	backend->admin_title[0] = '\0';
	backend->admin_message[0] = '\0';
	return 0;
}

/*
 * Tells sessiond the person allowed the dialog of id, or answers its
 * CONNECT (ADMIN ALLOW, with the connection lent to a program of the
 * desktop); its answer comes as session_admin.
 */
int
kl_backend_session_admin_allow(
	struct kl_backend *backend,
	uint32_t id,
	int connection)
{
	char line[KL_BACKEND_SESSION_LINE];
	int usable;
	int length;
	int error;

	/* Only a session sessiond still listens to, with a request's number. */
	if (backend == NULL || id == 0U || connection < -1)
		return EINVAL;
	usable = session_admin_usable(backend);
	if (!usable)
		return ENOTSUP;

	/* One admin answer is awaited at a time. */
	if (backend->admin_awaited != 0U)
		return EBUSY;

	/* The line, with the connection when there is one. */
	length = snprintf(line, sizeof(line), "ADMIN ALLOW %lu\n", (unsigned long)id);
	if (length < 0 || (size_t)length >= sizeof(line))
		return EINVAL;
	error = session_admin_send(backend, line, connection);
	if (error != 0)
		return error;

	/* Succeeded: STARTED or FAILED is awaited. */
	backend->admin_awaited = id;
	backend->admin_due_ms = 0U;
	return 0;
}

/*
 * Tells sessiond the dialog of id went without an allow (ADMIN CANCEL); it has no answer.
 */
int
kl_backend_session_admin_cancel(
	struct kl_backend *backend,
	uint32_t id)
{
	char line[KL_BACKEND_SESSION_LINE];
	int usable;
	int length;
	int error;

	/* Only a session sessiond still listens to, with a request's number. */
	if (backend == NULL || id == 0U)
		return EINVAL;
	usable = session_admin_usable(backend);
	if (!usable)
		return ENOTSUP;

	/* The line, which awaits nothing. */
	length = snprintf(line, sizeof(line), "ADMIN CANCEL %lu\n", (unsigned long)id);
	if (length < 0 || (size_t)length >= sizeof(line))
		return EINVAL;
	error = session_admin_send(backend, line, -1);
	if (error != 0)
		return error;

	/* Succeeded: sessiond forgets the request. */
	return 0;
}

/*
 * Tells sessiond a client went (ADMIN FORGET): what it was allowed goes; it has no answer.
 */
int
kl_backend_session_admin_forget(
	struct kl_backend *backend,
	uint64_t client)
{
	char line[KL_BACKEND_SESSION_LINE];
	int usable;
	int length;
	int error;

	/* Only a session sessiond still listens to. */
	if (backend == NULL)
		return EINVAL;
	usable = session_admin_usable(backend);
	if (!usable)
		return ENOTSUP;

	/* The line, which awaits nothing. */
	length = snprintf(line, sizeof(line), "ADMIN FORGET %llu\n", (unsigned long long)client);
	if (length < 0 || (size_t)length >= sizeof(line))
		return EINVAL;
	error = session_admin_send(backend, line, -1);
	if (error != 0)
		return error;

	/* Succeeded: the client's permissions are gone. */
	return 0;
}

/*
 * Reads what sessiond sent, and ends a Log Out that was not answered in time.
 */
void
kl_backend_session_tick(
	struct kl_backend *backend,
	uint64_t now_ms)
{
	ssize_t count;
	char *end;
	int descriptor;

	/* Nothing to read without sessiond, or after it left. */
	descriptor = session_descriptor(backend);
	if (descriptor < 0 || backend->session_gone)
		return;

	/* A Log Out sessiond did not answer in time ends the compositor anyway. */
	if (backend->logout_asked) {
		if (backend->logout_ms == 0U)
			backend->logout_ms = now_ms;
		if (now_ms - backend->logout_ms >= SESSION_LOGOUT_MS) {
			backend->logout_asked = 0U;
			if (backend->host.session_stop != NULL)
				backend->host.session_stop(backend->host.data, KL_BACKEND_SESSION_UNANSWERED);
			return;
		}
	}

	/* An admin answer awaited starts its time at the first tick, and is given up once it has run out. */
	if (backend->admin_awaited != 0U) {
		if (backend->admin_due_ms == 0U)
			backend->admin_due_ms = now_ms + KL_BACKEND_ADMIN_WAIT_MS;
		if (now_ms >= backend->admin_due_ms)
			session_admin_fail(backend, ETIMEDOUT);
	}

	/* What has come, with the descriptors sent along (nothing new still leaves the lines kept during the wait for GO). */
	count = session_receive(backend, descriptor);
	if (count < 0 && backend->session_used == 0U)
		return;

	/* sessiond done with the login screen ends it (RELEASED is still said on the open side). */
	if (count == 0 && backend->options.greeter_descriptor >= 0) {
		backend->session_gone = 1U;
		session_end(backend, KL_BACKEND_SESSION_ENDED);
		return;
	}

	/*
	 * A session carries on without sessiond, whose descriptor is closed; a
	 * sleep asked is over (ws052-p011), and so is an admin answer awaited
	 * (the descriptors kept go with it).
	 */
	if (count == 0) {
		backend->session_gone = 1U;
		(void)close(descriptor);
		backend->options.session_descriptor = -1;
		if (backend->power_asked == KL_BACKEND_POWER_SUSPEND)
			session_slept(backend, "ERROR");
		if (backend->admin_awaited != 0U)
			session_admin_fail(backend, EPIPE);
		session_admin_drop(backend);
		return;
	}

	/* Each whole line. */
	if (count > 0)
		backend->session_used += (size_t)count;
	backend->session_line[backend->session_used] = '\0';
	for (;;) {
		end = strchr(backend->session_line, '\n');
		if (end == NULL)
			break;

		/* Acts on the line before taking it out of what is kept. */
		*end = '\0';
		session_answered(backend, backend->session_line);
		backend->session_used -= (size_t)(end - backend->session_line) + 1U;
		memmove(backend->session_line, end + 1, backend->session_used + 1U);
	}

	/* A line that never ends is thrown away. */
	if (backend->session_used + 1U >= sizeof(backend->session_line))
		backend->session_used = 0U;

	/* Descriptors no line took are closed once no line is left half read (a line's come with its first byte). */
	if (backend->session_used == 0U)
		session_admin_drop(backend);
}

/*
 * Tells what kl_backend_session_tick waits for: sessiond's lines, and the
 * end of the time a Log Out asked waits for its answer.
 */
int
kl_backend_session_wait(
	const struct kl_backend *backend,
	uint64_t *due_ms)
{
	int descriptor;

	/* Nothing to read without sessiond, or after it left. */
	*due_ms = UINT64_MAX;
	descriptor = session_descriptor(backend);
	if (descriptor < 0 || backend->session_gone)
		return -1;

	/* A Log Out asked: its time starts at the next tick, and runs out after SESSION_LOGOUT_MS. */
	if (backend->logout_asked) {
		*due_ms = 0U;
		if (backend->logout_ms != 0U)
			*due_ms = backend->logout_ms + SESSION_LOGOUT_MS;
	}

	/* An admin answer awaited: the same, after KL_BACKEND_ADMIN_WAIT_MS. */
	if (backend->admin_awaited != 0U && backend->admin_due_ms < *due_ms)
		*due_ms = backend->admin_due_ms;

	/* Succeeded: sessiond's lines wake the wait. */
	return descriptor;
}

/*
 * Sends one request line to sessiond (sharing-zedbsd.c's SERVICE).
 */
int
kl_backend_session_send(
	struct kl_backend *backend,
	unsigned request,
	const char *line)
{
	int error;

	/* The line, as the others are sent. */
	error = session_send(backend, request, line);
	return error;
}

/* Returns the descriptor to sessiond: the login screen's, a session's, or -1. */
static int
session_descriptor(
	const struct kl_backend *backend)
{
	/* The login screen asks on --auth-fd. */
	if (backend->options.greeter_descriptor >= 0)
		return backend->options.greeter_descriptor;

	/* A session's --control-fd (-1 without sessiond). */
	return backend->options.session_descriptor;
}

/* Writes one request line whole and remembers which request awaits its answer. */
static int
session_send(
	struct kl_backend *backend,
	unsigned request,
	const char *line)
{
	size_t length;
	ssize_t written;

	/* One request at a time. */
	if (request != KL_BACKEND_SESSION_NONE && backend->session_request != KL_BACKEND_SESSION_NONE)
		return EBUSY;

	/* The whole line in one write (it is short). */
	length = strlen(line);
	written = write(session_descriptor(backend), line, length);
	if (written < 0)
		return errno;
	if ((size_t)written != length)
		return EIO;

	/* Succeeded: the answer is awaited. */
	if (request != KL_BACKEND_SESSION_NONE)
		backend->session_request = request;
	return 0;
}

/* Acts on one line sessiond sent. */
static void
session_answered(
	struct kl_backend *backend,
	const char *line)
{
	enum session_answer kind;
	unsigned request;
	int same;
	int error;

	/* An admin request's answer names its request: it answers no other (not a SERVICE's nor a sleep's). */
	same = strncmp(line, "ADMIN ", 6U);
	if (same == 0) {
		session_admin_line(backend, line + 6);
		return;
	}

	/* QUIT: the greeter is ready; the display goes back, then the compositor ends. */
	same = strcmp(line, "QUIT");
	if (same == 0 && backend->options.greeter_descriptor < 0) {
		backend->logout_asked = 0U;
		session_end(backend, KL_BACKEND_SESSION_QUIT);
		return;
	}

	/* TOUCH: a security key waits to be touched; the request is still under way and the host shows it (ws172-p003). */
	same = strcmp(line, "TOUCH");
	if (same == 0) {
		if (backend->host.session_answer != NULL)
			backend->host.session_answer(backend->host.data, KL_BACKEND_SESSION_TOUCH, 0);
		return;
	}

	/* REPLUG: the key is to be plugged in again for its reset; the request is still under way (ws199-p001). */
	same = strcmp(line, "REPLUG");
	if (same == 0) {
		if (backend->host.session_answer != NULL)
			backend->host.session_answer(backend->host.data, KL_BACKEND_SESSION_REPLUG, 0);
		return;
	}

	/* A sleep's answer is its outcome, never a login's or an action's answer (ws052-p011). */
	if (backend->session_request == KL_BACKEND_SESSION_POWER && backend->power_asked == KL_BACKEND_POWER_SUSPEND) {
		session_slept(backend, line);
		return;
	}

	/* What the answer says: a SERVICE request's is the state, or why not (ws089-p025). */
	backend->session_reason[0] = '\0';
	kind = SESSION_ANSWER_SERVICE;
	if (backend->session_request != KL_BACKEND_SESSION_SERVICE)
		kind = session_kind(line);
	error = EPROTO;
	switch (kind) {
	case SESSION_ANSWER_SERVICE:
		error = kl_backend_sharing_take(backend, line);
		break;
	case SESSION_ANSWER_STYLES:
		/* The styles a user may use now. */
		session_take_styles(backend, line + strlen("STYLES "));
		error = 0;
		break;
	case SESSION_ANSWER_ENROLLED:
		/* What the session user has enrolled. */
		session_take_enrolled(backend, line + strlen("ENROLLED "));
		error = 0;
		break;
	case SESSION_ANSWER_KEYINFO:
		/* What the keys there are. */
		session_take_key_info(backend, line + strlen("KEYINFO"));
		error = 0;
		break;
	case SESSION_ANSWER_KEYOWNER:
		/* Whose the key is, or why nobody's. */
		session_take_key_owner(backend, line + strlen("KEYOWNER"));
		error = 0;
		break;
	case SESSION_ANSWER_OK:
		/* Granted (a reset says how many registrations went). */
		if (backend->session_request == KL_BACKEND_SESSION_KEYOP)
			(void)sscanf(line, "OK removed=%u", &backend->session_key_removed);
		error = 0;
		break;
	case SESSION_ANSWER_FAIL:
		/* Refused, with its word when it has one. */
		if (line[4] == ' ')
			snprintf(backend->session_reason, sizeof(backend->session_reason), "%.*s", (int)sizeof(backend->session_reason) - 1, line + 5);
		error = EACCES;
		break;
	case SESSION_ANSWER_BUSY:
		/* Another request was under way. */
		error = EBUSY;
		break;
	case SESSION_ANSWER_ERROR:
		/* Not done. */
		error = EIO;
		break;
	case SESSION_ANSWER_UNKNOWN:
		break;
	}

	/* The request it answers is no longer awaited; a refused power action may be asked again. */
	request = backend->session_request;
	backend->session_request = KL_BACKEND_SESSION_NONE;
	if (request == KL_BACKEND_SESSION_POWER && error != 0)
		backend->power_asked = 0U;
	if (backend->host.session_answer != NULL)
		backend->host.session_answer(backend->host.data, request, error);
}

/*
 * Takes sessiond's answer to "POWER suspend" (ws052-p011): its outcome is
 * kept for kl_backend_power_outcome, the sleep may be asked again, and the
 * host hears session_answer(KL_BACKEND_SESSION_POWER) with its error.
 */
static void
session_slept(
	struct kl_backend *backend,
	const char *line)
{
	int error;

	/* The outcome, and no sleep asked any more. */
	error = kl_backend_power_parse_outcome(line, &backend->power_outcome);
	backend->power_asked = 0U;
	backend->session_request = KL_BACKEND_SESSION_NONE;

	/* The host hears it. */
	if (backend->host.session_answer != NULL)
		backend->host.session_answer(backend->host.data, KL_BACKEND_SESSION_POWER, error);
}

/* Says what a line answers, by its first words. */
static enum session_answer
session_kind(
	const char *line)
{
	size_t index;
	size_t length;
	int same;

	/* The first word that the line is, or starts with before a space. */
	for (index = 0U; index < sizeof(session_words) / sizeof(session_words[0]); index++) {
		length = strlen(session_words[index].word);
		same = strncmp(line, session_words[index].word, length);
		if (same != 0)
			continue;
		if (line[length] == '\0' || line[length] == ' ')
			return session_words[index].kind;
	}

	/* A line not understood. */
	return SESSION_ANSWER_UNKNOWN;
}

/*
 * Takes STYLES' list ("password pin fido2", or part of it: WS200 may turn
 * the password off): the KL_BACKEND_STYLE_* bits; an empty list leaves
 * none (kl_backend_session_styles_get then gives the password).
 */
static void
session_take_styles(
	struct kl_backend *backend,
	const char *list)
{
	char copy[KL_BACKEND_SESSION_LINE];
	char *word;
	char *rest;
	unsigned styles;
	int same;

	/* Each word of the list. */
	snprintf(copy, sizeof(copy), "%s", list);
	styles = 0U;
	word = copy;
	while (word != NULL && *word != '\0') {
		rest = strchr(word, ' ');
		if (rest != NULL) {
			*rest = '\0';
			rest++;
		}

		/* The password, the PIN, or a security key. */
		same = strcmp(word, "password");
		if (same == 0)
			styles |= KL_BACKEND_STYLE_PASSWORD;
		same = strcmp(word, "pin");
		if (same == 0)
			styles |= KL_BACKEND_STYLE_PIN;
		same = strcmp(word, "fido2");
		if (same == 0)
			styles |= KL_BACKEND_STYLE_KEY;

		/* The next word. */
		word = rest;
	}

	/* Succeeded: the styles kept. */
	backend->session_styles = styles;
}

/* Takes ENROLLED's list ("pin=1 fido2=2 key=REF/LABEL ..."): whether a PIN is set, the number of keys and the keys. */
static void
session_take_enrolled(
	struct kl_backend *backend,
	const char *list)
{
	const char *word;
	const char *option;
	unsigned pin;
	unsigned keys;
	unsigned methods;
	int scanned;
	int error;

	/* Both counts, or nothing. */
	pin = 0U;
	keys = 0U;
	scanned = sscanf(list, "pin=%u fido2=%u", &pin, &keys);
	if (scanned != 2) {
		pin = 0U;
		keys = 0U;
	}

	/* What is enrolled, kept. */
	backend->session_pin = 0U;
	if (pin != 0U)
		backend->session_pin = 1U;
	backend->session_keys = keys;

	/* The key's options (ws199-p001): asked unless said otherwise. */
	backend->session_key_pin = 1U;
	backend->session_key_touch = 1U;
	option = strstr(list, " key-pin=0");
	if (option != NULL)
		backend->session_key_pin = 0U;
	option = strstr(list, " key-touch=0");
	if (option != NULL)
		backend->session_key_touch = 0U;

	/* The methods the screens take (WS200): every one unless said otherwise. */
	backend->session_methods = KL_BACKEND_METHODS_ALL;
	option = strstr(list, " methods=");
	if (option != NULL) {
		scanned = sscanf(option, " methods=%u", &methods);
		if (scanned == 1 && methods != 0U && (methods & ~KL_BACKEND_METHODS_ALL) == 0U)
			backend->session_methods = methods;
	}

	/* Each key listed, while there is room (one that does not read is left out). */
	backend->session_key_count = 0U;
	word = strstr(list, " key=");
	while (word != NULL && backend->session_key_count < KL_BACKEND_KEYS_MAX) {
		error = session_take_key(word + 5, &backend->session_key_list[backend->session_key_count]);
		if (error == 0)
			backend->session_key_count++;
		word = strstr(word + 5, " key=");
	}
}

/* Takes KEYINFO's list (" count=N name=HEX pin=0|1 retries=N min=N"); a list that does not read is no key. */
static void
session_take_key_info(
	struct kl_backend *backend,
	const char *list)
{
	struct kl_backend_key_info *info;
	const char *name;
	const char *rest;
	size_t length;
	size_t index;
	int scanned;
	int high;
	int low;
	int character;

	/* How many. */
	info = &backend->session_key_info;
	memset(info, 0, sizeof(*info));
	scanned = sscanf(list, " count=%u", &info->count);
	if (scanned != 1) {
		info->count = 0U;
		return;
	}

	/* The one key's facts. */
	name = strstr(list, " name=");
	if (info->count != 1U || name == NULL)
		return;
	rest = strchr(name + 1, ' ');
	scanned = 0;
	if (rest != NULL)
		scanned = sscanf(rest, " pin=%u retries=%u min=%u", &info->pin, &info->retries, &info->min);
	if (scanned != 3)
		info->pin = 0U;

	/* Its name: two digits a byte (a control character ends it). */
	name += strlen(" name=");
	length = strcspn(name, " ");
	for (index = 0U; index < length / 2U && index + 1U < sizeof(info->name); index++) {
		high = session_hex_value(name[2U * index]);
		low = session_hex_value(name[2U * index + 1U]);
		if (high < 0 || low < 0)
			break;
		character = high << 4 | low;
		if (character < 0x20 || character == 0x7f)
			break;
		info->name[index] = (char)character;
	}
}

/*
 * Takes KEYOWNER's answer (" user=NAME key-pin=0|1 key-touch=0|1
 * card=0|1", or " REASON"); an answer that does not read is nobody's with
 * the reason "internal".
 */
static void
session_take_key_owner(
	struct kl_backend *backend,
	const char *list)
{
	struct kl_backend_key_owner *owner;
	char user[KL_BACKEND_KEY_LABEL];
	int scanned;
	int same;
	int valid;

	/* Nobody's until it reads. */
	owner = &backend->session_key_owner;
	memset(owner, 0, sizeof(*owner));
	user[0] = '\0';

	/* An owner: the name (one word that may go in a request) and the key's options. */
	same = strncmp(list, " user=", 6U);
	if (same == 0) {
		scanned = sscanf(list, " user=%32s key-pin=%u key-touch=%u card=%u", user, &owner->key_pin, &owner->key_touch, &owner->card);
		valid = session_name_valid(user);
		if (scanned == 4 && valid && owner->key_pin <= 1U && owner->key_touch <= 1U && owner->card <= 1U) {
			snprintf(owner->user, sizeof(owner->user), "%s", user);
			owner->found = 1U;
			return;
		}

		/* A line that does not read. */
		memset(owner, 0, sizeof(*owner));
		snprintf(owner->reason, sizeof(owner->reason), "internal");
		return;
	}

	/* The reason, one word. */
	if (list[0] == ' ')
		list++;
	snprintf(owner->reason, sizeof(owner->reason), "%.*s", (int)strcspn(list, " "), list);
}

/* Takes one "REF/LABEL" word (up to a space or the end), the label's bytes in hexadecimal (no control character).  Returns 0 or EINVAL. */
static int
session_take_key(
	const char *word,
	struct kl_backend_key *key)
{
	const char *slash;
	size_t length;
	size_t index;
	int high;
	int low;
	int character;

	/* The reference: 16 digits before the slash. */
	memset(key, 0, sizeof(*key));
	slash = strchr(word, '/');
	if (slash == NULL || (size_t)(slash - word) != KL_BACKEND_KEY_REF - 1U)
		return EINVAL;
	memcpy(key->ref, word, KL_BACKEND_KEY_REF - 1U);

	/* The label: two digits a byte, up to a space or the end. */
	length = strcspn(slash + 1, " ");
	if (length % 2U != 0U || length / 2U >= sizeof(key->label))
		return EINVAL;
	for (index = 0U; index < length / 2U; index++) {
		high = session_hex_value(slash[1U + 2U * index]);
		low = session_hex_value(slash[2U + 2U * index]);
		if (high < 0 || low < 0)
			return EINVAL;
		character = high << 4 | low;
		if (character < 0x20 || character == 0x7f)
			return EINVAL;
		key->label[index] = (char)character;
	}

	/* Succeeded: the key. */
	return 0;
}

/* Gives a small hexadecimal digit's value, or -1. */
static int
session_hex_value(
	char digit)
{
	/* The digits, then the letters. */
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;

	/* Not one. */
	return -1;
}

/* Gives a style's word in sessiond's requests, or NULL for none. */
static const char *
session_style_word(
	unsigned style)
{
	/* The password. */
	if (style == KL_BACKEND_STYLE_PASSWORD)
		return "password";

	/* The PIN. */
	if (style == KL_BACKEND_STYLE_PIN)
		return "pin";

	/* A security key. */
	if (style == KL_BACKEND_STYLE_KEY)
		return "fido2";

	/* Not a style. */
	return NULL;
}

/* Tells whether a secret may go on a line of its own: not too long, no control character. */
static int
session_secret_valid(
	const char *secret)
{
	size_t index;

	/* Each byte. */
	for (index = 0U; secret[index] != '\0'; index++) {
		if (index >= SESSION_SECRET_MAX)
			return 0;
		if ((unsigned char)secret[index] < 0x20U || secret[index] == 0x7f)
			return 0;
	}

	/* Succeeded: the secret fits a line. */
	return 1;
}

/* Tells whether an account's name may go in a request: one word, not empty, no control character. */
static int
session_name_valid(
	const char *name)
{
	size_t index;

	/* An empty name is none. */
	if (name[0] == '\0')
		return 0;

	/* Each byte. */
	for (index = 0U; name[index] != '\0'; index++) {
		if (index >= SESSION_SECRET_MAX)
			return 0;
		if ((unsigned char)name[index] <= 0x20U || name[index] == 0x7f)
			return 0;
	}

	/* Succeeded: the name is one word. */
	return 1;
}

/* Sends a request whose lines were written to line (length as snprintf said), and erases them. */
static int
session_ask(
	struct kl_backend *backend,
	unsigned request,
	char *line,
	int length)
{
	int error;

	/* A request that did not fit is not sent. */
	if (length < 0 || (size_t)length >= SESSION_REQUEST_MAX) {
		memset(line, 0, SESSION_REQUEST_MAX);
		return EINVAL;
	}

	/* The lines, then nothing of them is kept. */
	error = session_send(backend, request, line);
	memset(line, 0, SESSION_REQUEST_MAX);
	if (error != 0)
		return error;

	/* Succeeded: the answer is awaited. */
	return 0;
}

/* Ends the compositor: it gives the display back in the callback, then sessiond hears RELEASED. */
static void
session_end(
	struct kl_backend *backend,
	unsigned reason)
{
	int descriptor;

	/* The compositor closes its output (and asks to stop). */
	if (backend->host.session_stop != NULL)
		backend->host.session_stop(backend->host.data, reason);

	/* sessiond hears the display is free (it may be gone already). */
	descriptor = session_descriptor(backend);
	if (descriptor >= 0)
		(void)write(descriptor, "RELEASED\n", 9U);
}

/* The monotonic clock in milliseconds, for the wait for GO. */
static uint64_t
session_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* The clock cannot fail with this identifier; a failure reads as 0. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Succeeded: the milliseconds since an arbitrary start. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Reads what sessiond sent after the lines kept, and keeps the descriptors sent along; returns as read. */
static ssize_t
session_receive(
	struct kl_backend *backend,
	int descriptor)
{
	union {
		struct cmsghdr align;
		char space[CMSG_SPACE(sizeof(int) * KL_BACKEND_ADMIN_HELD)];
	} control;
	struct msghdr header;
	struct iovec vector;
	ssize_t count;

	/* The room after the lines kept, and room for two programs' descriptors. */
	vector.iov_base = backend->session_line + backend->session_used;
	vector.iov_len = sizeof(backend->session_line) - 1U - backend->session_used;
	memset(&header, 0, sizeof(header));
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.space;
	header.msg_controllen = sizeof(control.space);

	/* What has come; a descriptor received is never inherited by the compositor's children. */
	count = recvmsg(descriptor, &header, MSG_CMSG_CLOEXEC);
	if (count < 0 && errno == ENOTSOCK) {
		/* A descriptor that is no socket carries no descriptors: it is read. */
		count = read(descriptor, vector.iov_base, vector.iov_len);
		return count;
	}
	if (count < 0)
		return count;

	/* The descriptors, kept for the line that names them. */
	session_hold(backend, &header);

	/* Succeeded: the bytes that came (0 at the end). */
	return count;
}

/* Keeps the descriptors a message carried, in their order; those beyond the room are closed and the loss noted. */
static void
session_hold(
	struct kl_backend *backend,
	struct msghdr *header)
{
	struct cmsghdr *message;
	unsigned char *data;
	size_t count;
	size_t index;
	int received;

	/* Descriptors the kernel had no room for are lost: the STARTED they belonged to fails. */
	if ((header->msg_flags & MSG_CTRUNC) != 0)
		backend->admin_lost = 1U;

	/* Each SCM_RIGHTS message's descriptors. */
	for (message = CMSG_FIRSTHDR(header);
	     message != NULL;
	     message = CMSG_NXTHDR(header, message)) {
		/* Only descriptors are kept. */
		if (message->cmsg_level != SOL_SOCKET || message->cmsg_type != SCM_RIGHTS)
			continue;

		/* Each in its order, or closed when the room is full. */
		data = CMSG_DATA(message);
		count = (message->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (index = 0; index < count; index++) {
			memcpy(&received, data + index * sizeof(int), sizeof(int));
			if (backend->admin_held_count < KL_BACKEND_ADMIN_HELD) {
				backend->admin_held[backend->admin_held_count] = received;
				backend->admin_held_count++;
			} else {
				(void)close(received);
				backend->admin_lost = 1U;
			}
		}
	}
}

/* Tells whether admin requests may be sent: a session (not the login screen) sessiond still listens to. */
static int
session_admin_usable(
	const struct kl_backend *backend)
{
	/* The login screen asks for no action (sessiond refuses them there). */
	if (backend->options.greeter_descriptor >= 0)
		return 0;

	/* No session manager, or one that left. */
	if (backend->options.session_descriptor < 0)
		return 0;
	if (backend->session_gone)
		return 0;

	/* Succeeded: a session sessiond listens to. */
	return 1;
}

/* Tells whether an action's name is one sessiond reads: a lowercase letter or digit, then those and '-', at most 32. */
static int
session_admin_action_valid(
	const char *action)
{
	size_t index;
	char byte;

	/* Each byte, the first not a '-'. */
	for (index = 0U; action[index] != '\0'; index++) {
		byte = action[index];
		if (index >= SESSION_ADMIN_ACTION_MAX)
			return 0;
		if (byte >= 'a' && byte <= 'z')
			continue;
		if (byte >= '0' && byte <= '9')
			continue;
		if (byte == '-' && index != 0U)
			continue;

		/* A byte no name has. */
		return 0;
	}

	/* An empty name is none. */
	if (index == 0U)
		return 0;

	/* Succeeded: a name. */
	return 1;
}

/*
 * Tells whether the words are ones sessiond reads: at most 8 words of 1 to
 * 64 bytes each, joined by one space, at most 256 bytes in all (empty for
 * none).
 */
static int
session_admin_words_valid(
	const char *words)
{
	size_t index;
	size_t word_length;
	unsigned count;
	int allowed;

	/* No words. */
	if (words[0] == '\0')
		return 1;

	/* Each byte: a word's, or the one space between two words. */
	word_length = 0U;
	count = 1U;
	for (index = 0U; words[index] != '\0'; index++) {
		if (index >= SESSION_ADMIN_WORDS_MAX)
			return 0;

		/* A space ends a word that is not empty, and starts the next. */
		if (words[index] == ' ') {
			if (word_length == 0U)
				return 0;
			word_length = 0U;
			count++;
			if (count > SESSION_ADMIN_WORD_COUNT)
				return 0;
			continue;
		}

		/* A word's byte, within a word's length. */
		allowed = session_admin_word_byte(words[index]);
		if (!allowed)
			return 0;
		word_length++;
		if (word_length > SESSION_ADMIN_WORD_MAX)
			return 0;
	}

	/* The last word is not empty (no space at the end). */
	if (word_length == 0U)
		return 0;

	/* Succeeded: words sessiond reads. */
	return 1;
}

/* Tells whether a byte may be in a word: a letter, a digit, or one of "._/:=@+,-". */
static int
session_admin_word_byte(
	char byte)
{
	const char *punctuation;

	/* Letters and digits. */
	if (byte >= 'a' && byte <= 'z')
		return 1;
	if (byte >= 'A' && byte <= 'Z')
		return 1;
	if (byte >= '0' && byte <= '9')
		return 1;

	/* The punctuation a path or an option has; the end of the string is not a byte of it. */
	punctuation = strchr("._/:=@+,-", byte);
	if (byte != '\0' && punctuation != NULL)
		return 1;

	/* Another byte. */
	return 0;
}

/* Tells whether a locale is one sessiond reads: empty, or "ll" or "lll", then "_CC", then ".UTF-8", the last two each if any. */
static int
session_admin_locale_valid(
	const char *locale)
{
	size_t letters;
	int same;

	/* No locale. */
	if (locale[0] == '\0')
		return 1;

	/* Two or three lowercase letters. */
	letters = 0U;
	while (locale[letters] >= 'a' && locale[letters] <= 'z')
		letters++;
	if (letters < 2U || letters > 3U)
		return 0;
	locale += letters;

	/* The region: '_' and two capitals. */
	if (locale[0] == '_') {
		if (locale[1] < 'A' || locale[1] > 'Z')
			return 0;
		if (locale[2] < 'A' || locale[2] > 'Z')
			return 0;
		locale += 3;
	}

	/* The end, or the encoding and the end. */
	if (locale[0] == '\0')
		return 1;
	same = strcmp(locale, ".UTF-8");
	if (same != 0)
		return 0;

	/* Succeeded: a locale with its encoding. */
	return 1;
}

/* Writes one admin line whole, with the connection lent to a program as SCM_RIGHTS when it is not -1. */
static int
session_admin_send(
	struct kl_backend *backend,
	const char *line,
	int connection)
{
	union {
		struct cmsghdr align;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *message;
	struct msghdr header;
	struct iovec vector;
	size_t length;
	ssize_t sent;
	int error;

	/* A line without a descriptor goes as the other requests'. */
	if (connection < 0) {
		error = session_send(backend, KL_BACKEND_SESSION_NONE, line);
		if (error != 0)
			return error;
		return 0;
	}

	/* The line, and the descriptor with its first byte. */
	length = strlen(line);
	vector.iov_base = (void *)line;
	vector.iov_len = length;
	memset(&control, 0, sizeof(control));
	memset(&header, 0, sizeof(header));
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.space;
	header.msg_controllen = sizeof(control.space);
	message = CMSG_FIRSTHDR(&header);
	message->cmsg_level = SOL_SOCKET;
	message->cmsg_type = SCM_RIGHTS;
	message->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(message), &connection, sizeof(int));

	/* Sent whole, or the error; a gone sessiond is an error, not a signal. */
	sent = sendmsg(session_descriptor(backend), &header, MSG_NOSIGNAL);
	if (sent < 0)
		return errno;
	if ((size_t)sent != length)
		return EIO;

	/* Succeeded: sessiond has the line and its own copy of the connection. */
	return 0;
}

/* Acts on one answer of sessiond's to an admin request (the line after "ADMIN "). */
static void
session_admin_line(
	struct kl_backend *backend,
	const char *line)
{
	struct kl_backend_admin_answer answer;
	unsigned long value;
	const char *rest;
	unsigned index;
	int error;

	/* No descriptor yet, and nothing known of the answer. */
	memset(&answer, 0, sizeof(answer));
	for (index = 0U; index < KL_BACKEND_ADMIN_DESCRIPTORS; index++)
		answer.descriptors[index] = -1;

	/* TITLE and MESSAGE: the dialog's texts, kept until its SHOW (only the awaited request's). */
	rest = session_admin_verb(line, "TITLE");
	if (rest != NULL) {
		error = session_admin_id(rest, &answer.id, &rest);
		if (error == 0 && *rest == ' ' && answer.id == backend->admin_awaited)
			(void)snprintf(backend->admin_title, sizeof(backend->admin_title), "%s", rest + 1);
		return;
	}
	rest = session_admin_verb(line, "MESSAGE");
	if (rest != NULL) {
		error = session_admin_id(rest, &answer.id, &rest);
		if (error == 0 && *rest == ' ' && answer.id == backend->admin_awaited)
			(void)snprintf(backend->admin_message, sizeof(backend->admin_message), "%s", rest + 1);
		return;
	}

	/* SHOW: the dialog; one not understood is cancelled, and its request fails. */
	rest = session_admin_verb(line, "SHOW");
	if (rest != NULL) {
		error = session_admin_id(rest, &answer.id, &rest);
		if (error != 0)
			return;
		answer.kind = KL_BACKEND_ADMIN_SHOW;
		answer.title = backend->admin_title;
		answer.message = backend->admin_message;
		error = session_admin_show(rest, &answer);
		if (error != 0) {
			(void)kl_backend_session_admin_cancel(backend, answer.id);
			if (answer.id == backend->admin_awaited)
				session_admin_fail(backend, EPROTO);
			return;
		}
		session_admin_give(backend, &answer);
		return;
	}

	/* CONNECT: a permission kept for a program of the desktop, whose connection is asked for. */
	rest = session_admin_verb(line, "CONNECT");
	if (rest != NULL) {
		error = session_admin_id(rest, &answer.id, &rest);
		if (error != 0 || *rest != '\0')
			return;
		answer.kind = KL_BACKEND_ADMIN_CONNECT;
		session_admin_give(backend, &answer);
		return;
	}

	/* STARTED: the program runs; its descriptors are the next kept ones. */
	rest = session_admin_verb(line, "STARTED");
	if (rest != NULL) {
		error = session_admin_id(rest, &answer.id, &rest);
		if (error != 0 || *rest != ' ')
			return;
		error = session_admin_number(rest + 1, 16U, 0xfUL, &value, &rest);
		if (error != 0 || *rest != '\0')
			return;
		answer.kind = KL_BACKEND_ADMIN_STARTED;
		answer.bits = (unsigned)value;
		session_admin_take(backend, &answer);
		session_admin_give(backend, &answer);
		return;
	}

	/* REFUSED: the action does not run, and why. */
	rest = session_admin_verb(line, "REFUSED");
	if (rest != NULL) {
		error = session_admin_id(rest, &answer.id, &rest);
		if (error != 0 || *rest != ' ')
			return;
		answer.kind = KL_BACKEND_ADMIN_REFUSED;
		(void)session_admin_refused(rest + 1, &answer);
		session_admin_give(backend, &answer);
		return;
	}

	/* FAILED: the program could not be started, or the request was not one sessiond waits for. */
	rest = session_admin_verb(line, "FAILED");
	if (rest != NULL) {
		error = session_admin_id(rest, &answer.id, &rest);
		if (error != 0 || *rest != ' ')
			return;
		error = session_admin_number(rest + 1, 10U, 65535UL, &value, &rest);
		if (error != 0 || *rest != '\0')
			value = EPROTO;
		answer.kind = KL_BACKEND_ADMIN_FAILED;
		answer.error = (int)value;
		session_admin_give(backend, &answer);
		return;
	}

	/* A line not understood answers nothing (an awaited answer runs out of time). */
	return;
}

/* Gives what follows "VERB " in an admin line, or NULL when the line is not that verb's. */
static const char *
session_admin_verb(
	const char *text,
	const char *verb)
{
	size_t length;
	int same;

	/* The verb, then one space. */
	length = strlen(verb);
	same = strncmp(text, verb, length);
	if (same != 0)
		return NULL;
	if (text[length] != ' ')
		return NULL;

	/* Succeeded: what follows the verb. */
	return text + length + 1;
}

/*
 * Reads a number of one or more digits in base (10 or 16, lowercase) up
 * to limit; *rest is where it ended.  Returns 0, or EINVAL.
 */
static int
session_admin_number(
	const char *text,
	unsigned base,
	unsigned long limit,
	unsigned long *value,
	const char **rest)
{
	unsigned long number;
	unsigned digit;
	size_t index;
	char byte;

	/* Each digit, while the number stays within its limit. */
	number = 0UL;
	for (index = 0U; text[index] != '\0'; index++) {
		byte = text[index];
		if (byte >= '0' && byte <= '9') {
			digit = (unsigned)(byte - '0');
		} else if (base == 16U && byte >= 'a' && byte <= 'f') {
			digit = (unsigned)(byte - 'a') + 10U;
		} else {
			break;
		}
		if (number > (limit - digit) / base)
			return EINVAL;
		number = number * base + digit;
	}

	/* At least one digit. */
	if (index == 0U)
		return EINVAL;

	/* Succeeded: the number and where it ended. */
	*value = number;
	*rest = text + index;
	return 0;
}

/* Reads a request's number (1 to 4294967295); *rest is where it ended.  Returns 0, or EINVAL. */
static int
session_admin_id(
	const char *text,
	uint32_t *id,
	const char **rest)
{
	unsigned long value;
	int error;

	/* The digits. */
	error = session_admin_number(text, 10U, 0xffffffffUL, &value, rest);
	if (error != 0)
		return error;

	/* No request is numbered 0. */
	if (value == 0UL)
		return EINVAL;

	/* Succeeded: the request's number. */
	*id = (uint32_t)value;
	return 0;
}

/* Reads SHOW's rest (" how", then " display" or nothing) into the answer.  Returns 0, or EPROTO. */
static int
session_admin_show(
	const char *rest,
	struct kl_backend_admin_answer *answer)
{
	static const struct {
		const char *word;
		unsigned how;
	} hows[] = {
		{ "consent", KL_BACKEND_ADMIN_CONSENT },
		{ "password-self", KL_BACKEND_ADMIN_PASSWORD_SELF },
		{ "password-admin", KL_BACKEND_ADMIN_PASSWORD_ADMIN },
	};
	const char *after;
	size_t length;
	size_t index;
	int same;

	/* How the person confirms: one of the words, after one space, ending the line or before another space. */
	if (rest[0] != ' ')
		return EPROTO;
	rest++;
	after = NULL;
	for (index = 0U; index < sizeof(hows) / sizeof(hows[0]); index++) {
		length = strlen(hows[index].word);
		same = strncmp(rest, hows[index].word, length);
		if (same == 0 && (rest[length] == '\0' || rest[length] == ' ')) {
			answer->how = hows[index].how;
			after = rest + length;
			break;
		}
	}
	if (after == NULL)
		return EPROTO;

	/* The end, or a connection to lend. */
	if (after[0] == '\0')
		return 0;
	same = strcmp(after, " display");
	if (same != 0)
		return EPROTO;

	/* Succeeded: the ALLOW lends a connection. */
	answer->display = 1U;
	return 0;
}

/* Reads REFUSED's reason word into the answer (a word not known here is KL_BACKEND_ADMIN_OTHER).  Returns 0. */
static int
session_admin_refused(
	const char *rest,
	struct kl_backend_admin_answer *answer)
{
	static const struct {
		const char *word;
		unsigned reason;
	} reasons[] = {
		{ "unknown", KL_BACKEND_ADMIN_UNKNOWN },
		{ "invalid", KL_BACKEND_ADMIN_INVALID },
		{ "not-allowed", KL_BACKEND_ADMIN_NOT_ALLOWED },
		{ "method", KL_BACKEND_ADMIN_METHOD },
		{ "busy", KL_BACKEND_ADMIN_BUSY },
		{ "locked", KL_BACKEND_ADMIN_LOCKED },
	};
	size_t index;
	int same;

	/* The word's reason. */
	answer->reason = KL_BACKEND_ADMIN_OTHER;
	for (index = 0U; index < sizeof(reasons) / sizeof(reasons[0]); index++) {
		same = strcmp(rest, reasons[index].word);
		if (same == 0) {
			answer->reason = reasons[index].reason;
			break;
		}
	}

	/* Succeeded: the reason, known or not. */
	return 0;
}

/*
 * Takes a STARTED's descriptors from the ones kept: one for each bit, the
 * input and the status always among them.  When they are not all there
 * (lost, too few, or bits without the two), the ones taken are closed and
 * the answer becomes a FAILED with EPROTO: the program sees its input end
 * and stops.
 */
static void
session_admin_take(
	struct kl_backend *backend,
	struct kl_backend_admin_answer *answer)
{
	unsigned wanted;
	unsigned index;
	unsigned bit;
	int whole;

	/* As many as the bits say. */
	wanted = 0U;
	for (bit = 0x1U; bit <= KL_BACKEND_ADMIN_STATUS; bit <<= 1) {
		if ((answer->bits & bit) != 0U)
			wanted++;
	}

	/* Whole: nothing was lost, the input and the status are named, and enough came. */
	whole = 1;
	if (backend->admin_lost)
		whole = 0;
	if ((answer->bits & KL_BACKEND_ADMIN_INPUT) == 0U)
		whole = 0;
	if ((answer->bits & KL_BACKEND_ADMIN_STATUS) == 0U)
		whole = 0;
	if (backend->admin_held_count < wanted)
		whole = 0;

	/* A whole set goes to the answer, in its order; the rest stay kept. */
	if (whole) {
		for (index = 0U; index < wanted; index++)
			answer->descriptors[index] = backend->admin_held[index];
		answer->count = wanted;
		backend->admin_held_count -= wanted;
		memmove(backend->admin_held, backend->admin_held + wanted, backend->admin_held_count * sizeof(backend->admin_held[0]));
		return;
	}

	/* Otherwise every kept one is closed (which belong to this line is not known), and the start fails. */
	session_admin_drop(backend);
	answer->kind = KL_BACKEND_ADMIN_FAILED;
	answer->error = EPROTO;
	answer->bits = 0U;
}

/*
 * Gives an answer to the host when it is the awaited request's (the
 * request then no longer awaits), or cleans up after a late one; the
 * descriptors the host did not keep are closed.
 */
static void
session_admin_give(
	struct kl_backend *backend,
	struct kl_backend_admin_answer *answer)
{
	unsigned index;

	/* An answer nobody awaits (its time ran out, or it was never asked) is cleaned up. */
	if (backend->admin_awaited == 0U || answer->id != backend->admin_awaited) {
		session_admin_late(backend, answer);
	} else {
		/* The request no longer awaits; the host hears it, or the dialog it cannot show is cancelled. */
		backend->admin_awaited = 0U;
		backend->admin_due_ms = 0U;
		if (backend->host.session_admin != NULL)
			backend->host.session_admin(backend->host.data, answer);
		else
			session_admin_late(backend, answer);
	}

	/* The descriptors no one kept, and the dialog's texts, go. */
	for (index = 0U; index < KL_BACKEND_ADMIN_DESCRIPTORS; index++) {
		if (answer->descriptors[index] >= 0)
			(void)close(answer->descriptors[index]);
		answer->descriptors[index] = -1;
	}
	backend->admin_title[0] = '\0';
	backend->admin_message[0] = '\0';
}

/* Cleans up after an answer nobody awaits: a dialog or a connection asked for is cancelled (the streams close in the caller). */
static void
session_admin_late(
	struct kl_backend *backend,
	struct kl_backend_admin_answer *answer)
{
	/* sessiond would wait two minutes for the person: it forgets the request now. */
	if (answer->kind == KL_BACKEND_ADMIN_SHOW || answer->kind == KL_BACKEND_ADMIN_CONNECT)
		(void)kl_backend_session_admin_cancel(backend, answer->id);
}

/* Ends the awaited admin request without its answer: the host hears FAILED with error. */
static void
session_admin_fail(
	struct kl_backend *backend,
	int error)
{
	struct kl_backend_admin_answer answer;
	unsigned index;

	/* The answer: the awaited request failed. */
	memset(&answer, 0, sizeof(answer));
	for (index = 0U; index < KL_BACKEND_ADMIN_DESCRIPTORS; index++)
		answer.descriptors[index] = -1;
	answer.kind = KL_BACKEND_ADMIN_FAILED;
	answer.id = backend->admin_awaited;
	answer.error = error;

	/* It no longer awaits; the host hears it. */
	backend->admin_awaited = 0U;
	backend->admin_due_ms = 0U;
	backend->admin_title[0] = '\0';
	backend->admin_message[0] = '\0';
	if (backend->host.session_admin != NULL)
		backend->host.session_admin(backend->host.data, &answer);
}

/* Closes the descriptors kept for no line, and forgets that any were lost. */
static void
session_admin_drop(
	struct kl_backend *backend)
{
	unsigned index;

	/* Each kept one. */
	for (index = 0U; index < backend->admin_held_count; index++)
		(void)close(backend->admin_held[index]);
	backend->admin_held_count = 0U;
	backend->admin_lost = 0U;
}
