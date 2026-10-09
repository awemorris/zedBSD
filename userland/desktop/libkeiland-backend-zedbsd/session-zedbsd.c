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
 *   RELEASED                (none): the display has been given back
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
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <poll.h>
#include "power-outcome.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How long the compositor waits for GO, and for QUIT after LOGOUT (milliseconds). */
#define SESSION_WAIT_MS 20000U
#define SESSION_LOGOUT_MS 30000U

/* The longest request the compositor sends (ENROLL with its password's and PIN's lines), and the longest secret. */
#define SESSION_REQUEST_MAX 1024U
#define SESSION_SECRET_MAX 256U

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

	/* What has come (nothing new still leaves the lines kept during the wait for GO). */
	count = read(descriptor, backend->session_line + backend->session_used, sizeof(backend->session_line) - 1U - backend->session_used);
	if (count < 0 && backend->session_used == 0U)
		return;

	/* sessiond done with the login screen ends it (RELEASED is still said on the open side). */
	if (count == 0 && backend->options.greeter_descriptor >= 0) {
		backend->session_gone = 1U;
		session_end(backend, KL_BACKEND_SESSION_ENDED);
		return;
	}

	/* A session carries on without sessiond, whose descriptor is closed; a sleep asked is over (ws052-p011). */
	if (count == 0) {
		backend->session_gone = 1U;
		(void)close(descriptor);
		backend->options.session_descriptor = -1;
		if (backend->power_asked == KL_BACKEND_POWER_SUSPEND)
			session_slept(backend, "ERROR");
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

/* Takes STYLES' list ("password pin fido2"): the KL_BACKEND_STYLE_* bits. */
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
	styles = KL_BACKEND_STYLE_PASSWORD;
	word = copy;
	while (word != NULL && *word != '\0') {
		rest = strchr(word, ' ');
		if (rest != NULL) {
			*rest = '\0';
			rest++;
		}

		/* The PIN, or a security key. */
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
