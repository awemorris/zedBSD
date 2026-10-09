/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sessiond's authentication requests (auth.h; ws172-p002).
 *
 * The greeter's (docs/architecture/keiland.md):
 *   STYLES name                     STYLES password[ pin][ fido2]
 *   AUTH name style, then the secret's line
 *                                   TOUCH..., then OK, or FAIL reason after the delay
 *   CANCEL                          stops a security key attempt (FAIL timeout)
 * The session's:
 *   STYLES                          the session user's
 *   UNLOCK style, then the secret's line
 *   ENROLLED                        ENROLLED pin=0|1 fido2=N
 *   ENROLL pin, then the password's and the PIN's lines
 *   ENROLL fido2 label, then the password's and the key PIN's lines
 *   REMOVE pin, then the password's line
 *   REMOVE fido2 id, then the password's line
 *   KEYINFO                         KEYINFO count=N [name=HEX pin=0|1 retries=N min=N]
 *   KEYPIN set, then the new key PIN's line
 *   KEYPIN change, then the key PIN's and the new one's lines
 *   SETOPTIONS PIN TOUCH, then the password's line: the key's PIN and touch
 *                                   for signing in (each 0 or 1; ws199-p001)
 *   KEYRESET, then the password's line
 *                                   REPLUG while the key is to be plugged in again,
 *                                   TOUCH..., then OK removed=N or FAIL reason
 *   CANCEL
 * (the keys' own requests, ws199-p001 section 4.4: KEYINFO and KEYPIN are
 * not attempts of the account and touch no count; KEYRESET counts as a
 * password attempt until passkey-fido2 says the password was right, which
 * clears the counts as a password does, and its later failures are told
 * at once.  ENROLL, REMOVE and the keys' requests are told passkey's own
 * reason word; AUTH and UNLOCK the greeter's words.)
 * A request that comes while another is answered is ERROR busy.  The lines
 * after a request are always its secrets, never requests.  Every secret is
 * written to passkey's standard input and erased.
 */

#include "auth.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/wait.h>
#include <unistd.h>

/* The most of passkey's answer after its user ID that is passed on (styles=, pin= fido2= key=..., id=); ENROLLED's line must hold it. */
#define AUTH_EXTRA_MAX		480U

/* The longest reason word passkey answers. */
#define AUTH_REASON_MAX		32U

/* The descriptors a passkey child closes (all but its pipes). */
#define AUTH_DESCRIPTORS_MAX	256

/* The exit of a child that could not start passkey (passkey itself exits 0, 1 or 2). */
#define AUTH_EXEC_FAILED	127

/* The words of the requests, by enum sessiond_command. */
static const char *const auth_commands[SESSIOND_COMMAND_COUNT] = {
	"STYLES",
	"AUTH",
	"UNLOCK",
	"ENROLLED",
	"ENROLL",
	"REMOVE",
	"KEYINFO",
	"KEYPIN",
	"KEYRESET",
	"SETOPTIONS",
};

/* The words of the styles, by SESSIOND_STYLE_*. */
static const char *const auth_styles[] = {
	"password",
	"pin",
	"fido2",
};

/* The counts, for sessiond's life (in memory only). */
static struct sessiond_policy auth_policy;

static int auth_command_find(const char *line);
static int auth_parse(struct sessiond_exchange *exchange, const char *line);
static void auth_begin(struct sessiond_exchange *exchange);
static int auth_request(struct sessiond_exchange *exchange, char *request, size_t size);
static int auth_start(struct sessiond_exchange *exchange, const char *request, size_t length, long long timeout_ms);
static void auth_read(struct sessiond_exchange *exchange);
static void auth_kill(struct sessiond_exchange *exchange, int signal_number);
static void auth_finish(struct sessiond_exchange *exchange, int status);
static void auth_listing(struct sessiond_exchange *exchange, int ok, const char *extra);
static void auth_granted(struct sessiond_exchange *exchange, unsigned answer_uid, const char *extra);
static void auth_refused(struct sessiond_exchange *exchange, const char *reason);
static void auth_key_answer(struct sessiond_exchange *exchange, int ok, const char *reason, const char *extra);
static int auth_status(struct sessiond_exchange *exchange, const char *line);
static void auth_reply(struct sessiond_exchange *exchange, const char *line);
static int auth_lookup(const char *name, uid_t *uid);
static void auth_wipe(void *memory, size_t size);

/*
 * Starts the exchange of one socket: a session's, whose user owner is, or
 * (owner NULL) the greeter's, whose granted login is written to login.
 */
void
sessiond_exchange_init(
	struct sessiond_exchange *exchange,
	int socket,
	const struct sessiond_account *owner,
	struct sessiond_account *login)
{
	memset(exchange, 0, sizeof(*exchange));
	exchange->socket = socket;
	exchange->owner = owner;
	exchange->session = 0;
	if (owner != NULL)
		exchange->session = 1;
	exchange->login = login;
	exchange->output = -1;
}

/* Tells whether a request is being answered (passkey runs, or an answer is held back). */
int
sessiond_exchange_busy(
	const struct sessiond_exchange *exchange)
{
	/* passkey runs. */
	if (exchange->pid > 0)
		return 1;

	/* A failure's answer waits out its delay. */
	if (exchange->held[0] != '\0')
		return 1;

	/* Nothing is under way. */
	return 0;
}

/* Gives passkey's output to poll, or -1 (none, or it has ended). */
int
sessiond_exchange_fd(
	const struct sessiond_exchange *exchange)
{
	/* Only a running passkey's output that is still open. */
	if (exchange->pid <= 0)
		return -1;
	if (exchange->eof)
		return -1;

	/* The descriptor to poll. */
	return exchange->output;
}

/*
 * Takes one line of the socket.  Returns 1 when it was the exchange's (a
 * request of its own, or a secret's line, which is erased), 0 when it is
 * another request (the line is left as it was).
 */
int
sessiond_exchange_line(
	struct sessiond_exchange *exchange,
	char *line)
{
	size_t length;
	int command;
	int match;
	int error;
	int busy;

	/* A secret's line of the request waiting for it: kept for passkey, erased from the line. */
	if (exchange->lines_wanted > exchange->lines_have) {
		length = strlen(line);
		snprintf(exchange->lines[exchange->lines_have], sizeof(exchange->lines[0]), "%s", line);
		auth_wipe(line, length);
		exchange->lines_have++;

		/* The last of its lines starts the request. */
		if (exchange->lines_have == exchange->lines_wanted)
			auth_begin(exchange);
		return 1;
	}

	/* CANCEL stops passkey (its answer is FAIL timeout); without one it does nothing. */
	match = strcmp(line, "CANCEL");
	if (match == 0) {
		if (exchange->pid > 0 && exchange->term_ms == 0)
			auth_kill(exchange, SIGTERM);
		return 1;
	}

	/* A request that is not about credentials is the caller's. */
	command = auth_command_find(line);
	if (command < 0)
		return 0;

	/* One at a time. */
	busy = sessiond_exchange_busy(exchange);
	if (busy) {
		auth_reply(exchange, "ERROR busy");
		return 1;
	}

	/* The request's words; a malformed one is answered ERROR. */
	error = auth_parse(exchange, line);
	if (error != 0) {
		auth_reply(exchange, "ERROR");
		return 1;
	}

	/* A request without secret lines starts at once. */
	if (exchange->lines_wanted == 0U)
		auth_begin(exchange);
	return 1;
}

/*
 * Moves the exchange on: passkey's output read, its deadline kept (TERM,
 * then KILL), its answer handled, a held answer sent when its delay is
 * over.  Called every pass of the loop that serves the socket.
 */
void
sessiond_exchange_tick(
	struct sessiond_exchange *exchange)
{
	long long now;
	pid_t waited;
	int status;

	/* A held answer whose delay is over. */
	now = sessiond_milliseconds();
	if (exchange->pid <= 0 && exchange->held[0] != '\0' && now >= exchange->reply_ms) {
		auth_reply(exchange, exchange->held);
		exchange->held[0] = '\0';
	}

	/* Nothing more without a passkey running. */
	if (exchange->pid <= 0)
		return;

	/* What passkey wrote. */
	auth_read(exchange);

	/* The deadline: TERM, then KILL after the grace. */
	if (exchange->term_ms == 0 && now >= exchange->deadline_ms)
		auth_kill(exchange, SIGTERM);
	if (exchange->term_ms != 0 && now >= exchange->term_ms + SESSIOND_PASSKEY_GRACE_MS)
		(void)kill(-exchange->pid, SIGKILL);

	/* passkey's end, with what it wrote last. */
	waited = waitpid(exchange->pid, &status, WNOHANG);
	if (waited != exchange->pid)
		return;
	auth_read(exchange);

	/* Its answer, or none (it was killed, could not start, or died before it answered). */
	auth_finish(exchange, status);
}

/* Ends what runs (sessiond stops, or the socket went): passkey is killed and reaped. */
void
sessiond_exchange_stop(
	struct sessiond_exchange *exchange)
{
	int status;

	/* passkey's group goes. */
	if (exchange->pid > 0) {
		(void)kill(-exchange->pid, SIGKILL);
		(void)waitpid(exchange->pid, &status, 0);
		exchange->pid = 0;
	}

	/* Its output is closed. */
	if (exchange->output >= 0)
		(void)close(exchange->output);
	exchange->output = -1;

	/* No secret, request or answer is left. */
	auth_wipe(exchange->lines, sizeof(exchange->lines));
	auth_wipe(exchange->answer, sizeof(exchange->answer));
	exchange->held[0] = '\0';
	exchange->lines_wanted = 0U;
	exchange->lines_have = 0U;
}

/* Gives the enum sessiond_command of a line's first word, or -1. */
static int
auth_command_find(
	const char *line)
{
	size_t length;
	int command;
	int match;

	/* The first word's length. */
	length = strcspn(line, " ");

	/* The request whose word it is. */
	for (command = 0; command < SESSIOND_COMMAND_COUNT; command++) {
		match = strncmp(line, auth_commands[command], length);
		if (match == 0 && auth_commands[command][length] == '\0')
			return command;
	}

	/* Not a request about credentials. */
	return -1;
}

/*
 * Reads a request's words into the exchange: its command, the account it
 * is about (the greeter names it; a session's is always its own user), its
 * style and argument, and how many secret lines follow.  Returns 0, or -1
 * for a malformed request or one the socket may not ask.
 */
static int
auth_parse(
	struct sessiond_exchange *exchange,
	const char *line)
{
	char split[SESSIOND_LINE_MAX];
	char *word[4];
	char *cursor;
	const char *argument;
	size_t length;
	unsigned count;
	int command;
	int match;
	int set;
	int greeter_only;
	int session_only;
	int style;

	/* The words, at most four (a label's spaces are taken again below). */
	snprintf(split, sizeof(split), "%s", line);
	memset(word, 0, sizeof(word));
	cursor = split;
	count = 0U;
	word[0] = sessiond_policy_word(&cursor, ' ');
	while (word[count] != NULL && count < 3U) {
		count++;
		word[count] = sessiond_policy_word(&cursor, ' ');
	}

	/* The command. */
	command = auth_command_find(line);
	if (command < 0)
		return -1;
	memset(exchange->name, 0, sizeof(exchange->name));
	memset(exchange->argument, 0, sizeof(exchange->argument));
	exchange->command = (enum sessiond_command)command;
	exchange->style = SESSIOND_STYLE_PASSWORD;
	exchange->lines_wanted = 0U;
	exchange->lines_have = 0U;

	/* The greeter may only ask STYLES and AUTH; a session anything but AUTH. */
	greeter_only = 0;
	if (command == SESSIOND_COMMAND_AUTH)
		greeter_only = 1;
	session_only = 1;
	if (command == SESSIOND_COMMAND_STYLES || command == SESSIOND_COMMAND_AUTH)
		session_only = 0;
	if (exchange->session && greeter_only)
		return -1;
	if (!exchange->session && session_only)
		return -1;

	/* The greeter names the account in its second word; the session's is its user. */
	if (!exchange->session) {
		if (count < 2U)
			return -1;
		length = strlen(word[1]);
		if (length >= sizeof(exchange->name))
			return -1;
		memcpy(exchange->name, word[1], length + 1U);
		word[1] = word[2];
		word[2] = word[3];
		count--;
	} else if (exchange->owner != NULL) {
		snprintf(exchange->name, sizeof(exchange->name), "%s", exchange->owner->passwd.pw_name);
	}

	/* STYLES, ENROLLED and KEYINFO take nothing more. */
	exchange->verified = 0;
	if (command == SESSIOND_COMMAND_STYLES || command == SESSIOND_COMMAND_ENROLLED)
		return 0;
	if (command == SESSIOND_COMMAND_KEYINFO) {
		exchange->style = SESSIOND_STYLE_FIDO2;
		return 0;
	}

	/* KEYRESET takes the password's line. */
	if (command == SESSIOND_COMMAND_KEYRESET) {
		exchange->style = SESSIOND_STYLE_FIDO2;
		exchange->lines_wanted = 1U;
		return 0;
	}

	/* SETOPTIONS takes the key's PIN and touch (0 or 1 each), then the password's line. */
	if (command == SESSIOND_COMMAND_SETOPTIONS) {
		if (count < 3U)
			return -1;
		match = strcmp(word[1], "0") == 0 || strcmp(word[1], "1") == 0;
		set = strcmp(word[2], "0") == 0 || strcmp(word[2], "1") == 0;
		if (!match || !set)
			return -1;
		snprintf(exchange->argument, sizeof(exchange->argument), "%s\n%s", word[1], word[2]);
		exchange->lines_wanted = 1U;
		return 0;
	}

	/* KEYPIN set takes the new key PIN's line, KEYPIN change the key PIN's and the new one's. */
	if (command == SESSIOND_COMMAND_KEYPIN) {
		if (count < 2U)
			return -1;
		exchange->style = SESSIOND_STYLE_FIDO2;
		exchange->lines_wanted = 1U;
		match = strcmp(word[1], "change");
		if (match == 0)
			exchange->lines_wanted = 2U;
		set = strcmp(word[1], "set");
		if (match != 0 && set != 0)
			return -1;
		snprintf(exchange->argument, sizeof(exchange->argument), "%s", word[1]);
		return 0;
	}

	/* The others name a style, and AUTH and UNLOCK take its secret. */
	if (count < 2U)
		return -1;
	style = sessiond_policy_style(word[1]);
	if (style < 0)
		return -1;
	exchange->style = style;
	if (command == SESSIOND_COMMAND_AUTH || command == SESSIOND_COMMAND_UNLOCK) {
		exchange->lines_wanted = 1U;
		return 0;
	}

	/* ENROLL and REMOVE are about the PIN or a key, not the password. */
	if (style == SESSIOND_STYLE_PASSWORD)
		return -1;

	/*
	 * A key's: a label (which may have spaces: the rest of the line) to
	 * enroll, an ID (one word) to remove.  A longer one is refused.
	 */
	argument = NULL;
	if (style == SESSIOND_STYLE_FIDO2 && count >= 3U) {
		argument = word[2];
		if (command == SESSIOND_COMMAND_ENROLL)
			argument = line + (word[2] - split);
	}

	/* A key's request needs its label or ID, which is kept. */
	if (style == SESSIOND_STYLE_FIDO2 && argument == NULL)
		return -1;
	if (argument != NULL) {
		length = strlen(argument);
		if (length >= sizeof(exchange->argument))
			return -1;
		memcpy(exchange->argument, argument, length + 1U);
	}

	/* ENROLL takes the password and the new secret, REMOVE the password. */
	exchange->lines_wanted = 1U;
	if (command == SESSIOND_COMMAND_ENROLL)
		exchange->lines_wanted = 2U;

	/* Succeeded: the request is read. */
	return 0;
}

/* The request has all its lines: the counts' rules, then passkey. */
static void
auth_begin(
	struct sessiond_exchange *exchange)
{
	char request[SESSIOND_REQUEST_SIZE];
	long long timeout;
	uid_t uid;
	int known;
	int allowed;
	int missing;
	int length;
	int error;

	/*
	 * Without passkey nothing can be checked: refused at once and logged,
	 * not counted (the system's fault, not a guess).  STYLES and ENROLLED
	 * answer as when passkey gave no answer.
	 */
	missing = access(SESSIOND_PASSKEY, X_OK);
	if (missing != 0) {
		sessiond_log("SESSIOND passkey missing path=%s errno=%d", SESSIOND_PASSKEY, errno);
		auth_wipe(exchange->lines, sizeof(exchange->lines));
		exchange->lines_wanted = 0U;
		exchange->lines_have = 0U;
		if (exchange->command == SESSIOND_COMMAND_STYLES || exchange->command == SESSIOND_COMMAND_ENROLLED) {
			auth_listing(exchange, 0, "");
			return;
		}

		/* The others fail. */
		auth_reply(exchange, "FAIL internal");
		return;
	}

	/* The account's counts (the names of no account share one). */
	uid = (uid_t)-1;
	known = auth_lookup(exchange->name, &uid);
	exchange->uid = uid;
	exchange->count = sessiond_policy_count(&auth_policy, known, uid);

	/* The PIN is tried only while it is offered; that is no attempt. */
	allowed = 1;
	if (exchange->command == SESSIOND_COMMAND_AUTH || exchange->command == SESSIOND_COMMAND_UNLOCK) {
		if (exchange->style == SESSIOND_STYLE_PIN)
			allowed = sessiond_policy_pin_allowed(exchange->count);
	}

	/* A PIN that is not offered is refused at once. */
	if (!allowed) {
		auth_wipe(exchange->lines, sizeof(exchange->lines));
		exchange->lines_wanted = 0U;
		exchange->lines_have = 0U;
		auth_reply(exchange, "FAIL pin-off");
		return;
	}

	/*
	 * An attempt counts before passkey runs: a login or an unlock in its
	 * style, a change by its password.
	 */
	if ((exchange->command == SESSIOND_COMMAND_AUTH || exchange->command == SESSIOND_COMMAND_UNLOCK) &&
	    exchange->style != SESSIOND_STYLE_FIDO2) {
		sessiond_policy_attempt(exchange->count, exchange->style);
	} else if (exchange->command == SESSIOND_COMMAND_ENROLL || exchange->command == SESSIOND_COMMAND_REMOVE ||
		   exchange->command == SESSIOND_COMMAND_KEYRESET || exchange->command == SESSIOND_COMMAND_SETOPTIONS) {
		sessiond_policy_attempt(exchange->count, SESSIOND_STYLE_PASSWORD);
	}

	/* passkey's request; the secrets are in it only. */
	length = auth_request(exchange, request, sizeof(request));
	auth_wipe(exchange->lines, sizeof(exchange->lines));
	exchange->lines_wanted = 0U;
	exchange->lines_have = 0U;
	if (length < 0) {
		auth_wipe(request, sizeof(request));
		auth_reply(exchange, "FAIL internal");
		return;
	}

	/* passkey runs (a key's touch is waited for longer, a reset's plugging in again and touch longer still; what a key is, not). */
	timeout = SESSIOND_PASSKEY_MS;
	if (exchange->style == SESSIOND_STYLE_FIDO2)
		timeout = SESSIOND_PASSKEY_KEY_MS;
	if (exchange->command == SESSIOND_COMMAND_KEYINFO)
		timeout = SESSIOND_PASSKEY_MS;
	if (exchange->command == SESSIOND_COMMAND_KEYRESET)
		timeout = SESSIOND_PASSKEY_RESET_MS;
	error = auth_start(exchange, request, (size_t)length, timeout);
	auth_wipe(request, sizeof(request));
	if (error != 0) {
		sessiond_log("SESSIOND passkey start errno=%d", error);
		auth_reply(exchange, "FAIL internal");
	}
}

/*
 * Writes passkey's request for the exchange (docs/architecture/security.md,
 * "The request"): one field per line.  Returns its length, or -1 when it
 * does not fit.
 */
static int
auth_request(
	struct sessiond_exchange *exchange,
	char *request,
	size_t size)
{
	const char *context;
	const char *style;
	int length;

	/* Each request's fields. */
	style = auth_styles[exchange->style];
	length = -1;
	switch (exchange->command) {
	case SESSIOND_COMMAND_STYLES:
		length = snprintf(request, size, "styles\n%s\n", exchange->name);
		break;
	case SESSIOND_COMMAND_ENROLLED:
		length = snprintf(request, size, "enrolled\n%s\n", exchange->name);
		break;
	case SESSIOND_COMMAND_AUTH:
	case SESSIOND_COMMAND_UNLOCK:
		/* A key's: the login or the unlock, as the account's options ask (ws199-p001). */
		if (exchange->style == SESSIOND_STYLE_FIDO2) {
			context = "login";
			if (exchange->command == SESSIOND_COMMAND_UNLOCK)
				context = "unlock";
			length = snprintf(request, size, "auth-fido2\n%s\n%s\n%s\n", exchange->name, context, exchange->lines[0]);
			break;
		}
		length = snprintf(request, size, "auth\n%s\n%s\n%s\n", exchange->name, style, exchange->lines[0]);
		break;
	case SESSIOND_COMMAND_SETOPTIONS:
		length = snprintf(request, size, "set-options\n%s\n%s\n%s\n", exchange->name, exchange->lines[0], exchange->argument);
		break;
	case SESSIOND_COMMAND_ENROLL:
		if (exchange->style == SESSIOND_STYLE_PIN) {
			length = snprintf(request, size, "enroll-pin\n%s\n%s\n%s\n", exchange->name, exchange->lines[0], exchange->lines[1]);
		} else {
			length = snprintf(
				request,
				size,
				"enroll-fido2\n%s\n%s\n%s\n%s\n",
				exchange->name,
				exchange->lines[0],
				exchange->argument,
				exchange->lines[1]);
		}

		/* The enrollment's request is written. */
		break;
	case SESSIOND_COMMAND_REMOVE:
		if (exchange->style == SESSIOND_STYLE_PIN) {
			length = snprintf(request, size, "remove-pin\n%s\n%s\n", exchange->name, exchange->lines[0]);
		} else {
			length = snprintf(request, size, "remove-fido2\n%s\n%s\n%s\n", exchange->name, exchange->lines[0], exchange->argument);
		}

		/* The removal's request is written. */
		break;
	case SESSIOND_COMMAND_KEYINFO:
		length = snprintf(request, size, "key-info\n%s\n", exchange->name);
		break;
	case SESSIOND_COMMAND_KEYPIN:
		if (exchange->lines_wanted == 2U) {
			length = snprintf(request, size, "key-change-pin\n%s\n%s\n%s\n", exchange->name, exchange->lines[0], exchange->lines[1]);
		} else {
			length = snprintf(request, size, "key-set-pin\n%s\n%s\n", exchange->name, exchange->lines[0]);
		}

		/* The key PIN's request is written. */
		break;
	case SESSIOND_COMMAND_KEYRESET:
		length = snprintf(request, size, "key-reset\n%s\n%s\n", exchange->name, exchange->lines[0]);
		break;
	case SESSIOND_COMMAND_COUNT:
		break;
	}

	/* A request that did not fit is none. */
	if (length < 0 || (size_t)length >= size)
		return -1;

	/* Succeeded: the request's length. */
	return length;
}

/* Starts passkey in a process group of its own, the request on its input, its output to poll. */
static int
auth_start(
	struct sessiond_exchange *exchange,
	const char *request,
	size_t length,
	long long timeout_ms)
{
	char *argv[2];
	char *environment[1];
	int input[2];
	int output[2];
	int descriptor;
	int error;
	pid_t child;

	/* The request's pipe. */
	error = pipe(input);
	if (error != 0)
		return errno;

	/* The answer's pipe. */
	error = pipe(output);
	if (error != 0) {
		error = errno;
		(void)close(input[0]);
		(void)close(input[1]);
		return error;
	}

	/* The child. */
	child = fork();
	if (child < 0) {
		error = errno;
		(void)close(input[0]);
		(void)close(input[1]);
		(void)close(output[0]);
		(void)close(output[1]);
		return error;
	}

	/* passkey: its own group, the pipes as 0 and 1, /dev/null as 2, nothing else open, no environment. */
	if (child == 0) {
		(void)setpgid(0, 0);
		(void)dup2(input[0], 0);
		(void)dup2(output[1], 1);
		for (descriptor = 2; descriptor < AUTH_DESCRIPTORS_MAX; descriptor++)
			(void)close(descriptor);
		descriptor = open("/dev/null", O_WRONLY);
		if (descriptor >= 0 && descriptor != 2) {
			(void)dup2(descriptor, 2);
			(void)close(descriptor);
		}

		/* passkey itself, or an exit that sessiond reads as no answer (and logs). */
		argv[0] = "passkey";
		argv[1] = NULL;
		environment[0] = NULL;
		(void)execve(SESSIOND_PASSKEY, argv, environment);
		_exit(AUTH_EXEC_FAILED);
	}

	/* The group is passkey's whichever of the two runs first. */
	(void)setpgid(child, child);

	/* The request, then the end of its input. */
	(void)close(input[0]);
	(void)close(output[1]);
	(void)write(input[1], request, length);
	(void)close(input[1]);

	/* Its answer is read without waiting, and no other child inherits it. */
	(void)fcntl(output[0], F_SETFL, O_NONBLOCK);
	(void)fcntl(output[0], F_SETFD, FD_CLOEXEC);

	/* Succeeded: passkey runs. */
	exchange->pid = child;
	exchange->output = output[0];
	exchange->deadline_ms = sessiond_milliseconds() + timeout_ms;
	exchange->term_ms = 0;
	exchange->used = 0U;
	exchange->done = 0;
	exchange->eof = 0;
	exchange->answer[0] = '\0';
	return 0;
}

/*
 * Reads what passkey wrote, without waiting: a TOUCH for each "status
 * touch", and its last line (its answer) kept.  The end of its output is
 * noted, so that the loop does not poll it again.
 */
static void
auth_read(
	struct sessiond_exchange *exchange)
{
	ssize_t count;
	size_t consumed;
	char *end;
	int match;

	/* Until nothing more is there, the answer has come, or the buffer is full. */
	while (!exchange->done && !exchange->eof && exchange->used + 1U < sizeof(exchange->answer)) {
		count = read(exchange->output, exchange->answer + exchange->used, sizeof(exchange->answer) - 1U - exchange->used);
		if (count == 0)
			exchange->eof = 1;
		if (count <= 0)
			break;
		exchange->used += (size_t)count;
		exchange->answer[exchange->used] = '\0';

		/* Each whole line: a status is taken at once (auth_status), anything else is the answer. */
		end = strchr(exchange->answer, '\n');
		while (end != NULL) {
			*end = '\0';
			match = auth_status(exchange, exchange->answer);
			if (!match) {
				exchange->done = 1;
				break;
			}

			/* The status's line goes. */
			consumed = (size_t)(end + 1 - exchange->answer);
			memmove(exchange->answer, end + 1, exchange->used - consumed + 1U);
			exchange->used -= consumed;
			end = strchr(exchange->answer, '\n');
		}
	}
}

/* Sends a signal to passkey's group (TERM: the time of it is kept). */
static void
auth_kill(
	struct sessiond_exchange *exchange,
	int signal_number)
{
	(void)kill(-exchange->pid, signal_number);
	if (signal_number == SIGTERM)
		exchange->term_ms = sessiond_milliseconds();
}

/*
 * passkey ended (status is waitpid's): its answer handled and told.  No
 * answer is a timeout when sessiond stopped it (its deadline or CANCEL),
 * and otherwise an internal failure, which is logged with how it ended (an
 * exit of AUTH_EXEC_FAILED: passkey is missing or could not run).
 */
static void
auth_finish(
	struct sessiond_exchange *exchange,
	int status)
{
	char extra[AUTH_EXTRA_MAX];
	char said[AUTH_REASON_MAX];
	const char *rest;
	const char *reason;
	unsigned answer_uid;
	int scanned;
	int signaled;
	int exited;
	int match;
	int ok;

	/* The process and its output are gone. */
	exchange->pid = 0;
	(void)close(exchange->output);
	exchange->output = -1;

	/* Its answer: ok uid=N [extra], or fail REASON; anything else is an internal failure. */
	ok = 0;
	reason = "internal";
	answer_uid = 0U;
	extra[0] = '\0';
	scanned = 0;
	match = -1;
	if (exchange->done) {
		scanned = sscanf(exchange->answer, "ok uid=%u", &answer_uid);
		match = strncmp(exchange->answer, "fail ", 5U);
	}

	/* No answer: stopped by sessiond (a timeout), or ended by itself, which is logged. */
	exited = WIFEXITED(status);
	signaled = WIFSIGNALED(status);
	if (!exchange->done && exchange->term_ms != 0) {
		reason = "timeout";
	} else if (!exchange->done && exited) {
		sessiond_log("SESSIOND passkey gave no answer exit=%d", WEXITSTATUS(status));
	} else if (!exchange->done && signaled) {
		sessiond_log("SESSIOND passkey gave no answer signal=%d", WTERMSIG(status));
	}

	/* What the answer says: the rest after the user ID, or the reason. */
	if (scanned == 1) {
		ok = 1;
		rest = strchr(exchange->answer + 3, ' ');
		if (rest != NULL)
			snprintf(extra, sizeof(extra), "%.*s", (int)sizeof(extra) - 1, rest + 1);
	} else if (match == 0) {
		snprintf(said, sizeof(said), "%.*s", (int)sizeof(said) - 1, exchange->answer + 5);
		reason = said;
	}

	/* The answer is not kept. */
	auth_wipe(exchange->answer, sizeof(exchange->answer));

	/* An answer about another user than the one asked about is a failure. */
	if (ok && exchange->uid == (uid_t)-1) {
		ok = 0;
		reason = "bad-secret";
	} else if (ok && answer_uid != (unsigned)exchange->uid) {
		ok = 0;
		reason = "bad-secret";
		sessiond_log("SESSIOND passkey answered uid=%u for user=%s", answer_uid, exchange->name);
	}

	/* STYLES and ENROLLED are told as they are. */
	if (exchange->command == SESSIOND_COMMAND_STYLES || exchange->command == SESSIOND_COMMAND_ENROLLED) {
		auth_listing(exchange, ok, extra);
		return;
	}

	/* The keys' own requests touch no count, but a reset's wrong password (ws199-p001). */
	if (exchange->command == SESSIOND_COMMAND_KEYINFO || exchange->command == SESSIOND_COMMAND_KEYPIN ||
	    exchange->command == SESSIOND_COMMAND_KEYRESET) {
		if (!ok && exchange->command == SESSIOND_COMMAND_KEYRESET && !exchange->verified) {
			auth_refused(exchange, reason);
			return;
		}

		/* Told as it is. */
		auth_key_answer(exchange, ok, reason, extra);
		return;
	}

	/* A success, or a failure. */
	if (ok) {
		auth_granted(exchange, answer_uid, extra);
	} else {
		auth_refused(exchange, reason);
	}
}

/* Answers STYLES (filtered by the counts: the PIN only while it is offered) or ENROLLED. */
static void
auth_listing(
	struct sessiond_exchange *exchange,
	int ok,
	const char *extra)
{
	char reply[SESSIOND_LINE_MAX];
	char styles[AUTH_EXTRA_MAX];
	const char *listed;

	/* ENROLLED: the account's PIN and keys, nothing when passkey did not answer. */
	if (exchange->command == SESSIOND_COMMAND_ENROLLED) {
		if (!ok)
			extra = "pin=0 fido2=0";
		snprintf(reply, sizeof(reply), "ENROLLED %s", extra);
		auth_reply(exchange, reply);
		return;
	}

	/* STYLES: the password at least, whatever the account (an unknown name looks like any other). */
	listed = NULL;
	if (ok)
		listed = strstr(extra, "styles=");
	snprintf(styles, sizeof(styles), "password");
	if (listed != NULL)
		sessiond_policy_styles(listed + strlen("styles="), exchange->count, styles, sizeof(styles));
	snprintf(reply, sizeof(reply), "STYLES %s", styles);
	auth_reply(exchange, reply);
}

/* passkey granted the request: the counts cleared, a login's account taken, OK told. */
static void
auth_granted(
	struct sessiond_exchange *exchange,
	unsigned answer_uid,
	const char *extra)
{
	char reply[SESSIOND_LINE_MAX];
	struct passwd *found;
	const char *style;
	int error;
	int match;

	/*
	 * The counts start again.  A change (a PIN or a key set or removed) is
	 * checked by the account's password, so it proves the password as a
	 * login does and offers the PIN from now on (BUG-285: a PIN set in a
	 * session that started without the password, an automatic login, was
	 * never offered on the lock screen).
	 */
	style = auth_styles[exchange->style];
	if (exchange->command == SESSIOND_COMMAND_AUTH || exchange->command == SESSIOND_COMMAND_UNLOCK) {
		sessiond_policy_success(exchange->count, exchange->style);
	} else {
		sessiond_policy_success(exchange->count, SESSIOND_STYLE_PASSWORD);
	}

	/* A greeter's login: the account sessiond starts the session for. */
	if (exchange->command == SESSIOND_COMMAND_AUTH) {
		found = NULL;
		error = -1;
		if (exchange->login != NULL)
			error = getpwnam_r(exchange->name, &exchange->login->passwd, exchange->login->buffer, sizeof(exchange->login->buffer), &found);
		if (error != 0 || found == NULL || found->pw_uid != (uid_t)answer_uid) {
			if (exchange->login != NULL)
				memset(exchange->login, 0, sizeof(*exchange->login));
			auth_reply(exchange, "FAIL internal");
			return;
		}

		/* The login is granted and on record. */
		exchange->logged_in = 1;
		syslog(LOG_NOTICE, "login %s on the graphical seat (%s)", exchange->name, style);
		sessiond_log("SESSIOND AUTH ok user=%s uid=%u style=%s", exchange->name, answer_uid, style);
	}

	/* A lock screen's unlock. */
	if (exchange->command == SESSIOND_COMMAND_UNLOCK) {
		syslog(LOG_NOTICE, "unlock %s on the graphical seat (%s)", exchange->name, style);
		sessiond_log("SESSIOND UNLOCK ok user=%s style=%s", exchange->name, style);
	}

	/* A change of the PIN, a key, or the key's options. */
	if (exchange->command == SESSIOND_COMMAND_ENROLL || exchange->command == SESSIOND_COMMAND_REMOVE ||
	    exchange->command == SESSIOND_COMMAND_SETOPTIONS) {
		syslog(LOG_NOTICE, "%s %s of %s", auth_commands[exchange->command], style, exchange->name);
		sessiond_log("SESSIOND %s %s ok user=%s", auth_commands[exchange->command], style, exchange->name);
	}

	/* OK, with a new key's ID. */
	snprintf(reply, sizeof(reply), "OK");
	match = strncmp(extra, "id=", 3U);
	if (match == 0)
		snprintf(reply, sizeof(reply), "OK %s", extra);
	auth_reply(exchange, reply);
}

/* passkey refused the request: on record, the answer held for the failure's delay. */
static void
auth_refused(
	struct sessiond_exchange *exchange,
	const char *reason)
{
	const char *what;
	const char *told;
	unsigned delay;
	int timeout;
	int counts;

	/* What failed, for syslog; the word told (the greeter's words for a login or an unlock, passkey's own for a change). */
	told = reason;
	if (exchange->command == SESSIOND_COMMAND_AUTH || exchange->command == SESSIOND_COMMAND_UNLOCK)
		told = sessiond_policy_reason(reason);
	what = "change";
	if (exchange->command == SESSIOND_COMMAND_AUTH)
		what = "login";
	if (exchange->command == SESSIOND_COMMAND_UNLOCK)
		what = "unlock";

	/*
	 * A key's login or unlock counts by its answer (R4): a wrong key PIN,
	 * a clone or an answer that does not verify; the others are told at
	 * once.
	 */
	counts = 1;
	if ((exchange->command == SESSIOND_COMMAND_AUTH || exchange->command == SESSIOND_COMMAND_UNLOCK) &&
	    exchange->style == SESSIOND_STYLE_FIDO2) {
		counts = sessiond_policy_key_counts(reason);
		if (counts)
			sessiond_policy_attempt(exchange->count, exchange->style);
	}

	/*
	 * The failure goes on record.  One sessiond ended (CANCEL, the
	 * deadline) without passkey's judgment is told at once: it says nothing
	 * of the secret (R5).
	 */
	delay = sessiond_policy_delay(exchange->count);
	if (!counts)
		delay = 0U;
	timeout = strcmp(reason, "timeout") == 0 || strcmp(reason, "canceled") == 0;
	if (exchange->term_ms != 0 && timeout)
		delay = 0U;
	syslog(LOG_WARNING, "failed %s %s on the graphical seat (%u in a row)", what, exchange->name, exchange->count->wrong);
	sessiond_log(
		"SESSIOND %s fail user=%s wrong=%u delay=%u style=%s reason=%s",
		auth_commands[exchange->command],
		exchange->name,
		exchange->count->wrong,
		delay,
		auth_styles[exchange->style],
		reason);

	/* The answer waits out the delay (sessiond_exchange_tick sends it). */
	snprintf(exchange->held, sizeof(exchange->held), "FAIL %s", told);
	exchange->reply_ms = sessiond_milliseconds() + (long long)delay * 1000LL;
}

/*
 * Answers a key's own request (ws199-p001 section 4.4): KEYINFO's facts,
 * OK for a PIN, OK removed=N for a reset; a failure at once with passkey's
 * word, without a delay or a failed change on record (the key counts its
 * own wrong PINs).
 */
static void
auth_key_answer(
	struct sessiond_exchange *exchange,
	int ok,
	const char *reason,
	const char *extra)
{
	char reply[SESSIOND_LINE_MAX];
	const char *word;

	/* What it was. */
	word = auth_commands[exchange->command];

	/* A failure, told at once. */
	if (!ok) {
		sessiond_log("SESSIOND %s fail user=%s reason=%s", word, exchange->name, reason);
		snprintf(reply, sizeof(reply), "FAIL %s", reason);
		auth_reply(exchange, reply);
		return;
	}

	/* KEYINFO: the key's facts. */
	if (exchange->command == SESSIOND_COMMAND_KEYINFO) {
		snprintf(reply, sizeof(reply), "KEYINFO %s", extra);
		auth_reply(exchange, reply);
		return;
	}

	/* A PIN set or changed, or a key reset, on record. */
	syslog(LOG_NOTICE, "%s of %s", word, exchange->name);
	sessiond_log("SESSIOND %s ok user=%s %s", word, exchange->name, extra);
	snprintf(reply, sizeof(reply), "OK");
	if (extra[0] != '\0')
		snprintf(reply, sizeof(reply), "OK %s", extra);
	auth_reply(exchange, reply);
}

/*
 * Takes one of passkey's status lines: "status touch" is told as TOUCH,
 * "status replug" as REPLUG, and "status verified" (a reset's password was
 * right) clears the counts as a password does.  Returns 1 when the line
 * was a status, 0 when it is the answer.
 */
static int
auth_status(
	struct sessiond_exchange *exchange,
	const char *line)
{
	int match;

	/* The touch. */
	match = strcmp(line, "status touch");
	if (match == 0) {
		auth_reply(exchange, "TOUCH");
		return 1;
	}

	/* The key to plug in again. */
	match = strcmp(line, "status replug");
	if (match == 0) {
		auth_reply(exchange, "REPLUG");
		return 1;
	}

	/* The reset's password was right: from here its failures are told at once. */
	match = strcmp(line, "status verified");
	if (match == 0) {
		if (exchange->command == SESSIOND_COMMAND_KEYRESET && !exchange->verified) {
			exchange->verified = 1;
			sessiond_policy_success(exchange->count, SESSIOND_STYLE_PASSWORD);
		}

		/* Taken. */
		return 1;
	}

	/* The answer. */
	return 0;
}

/* Writes one line on the socket. */
static void
auth_reply(
	struct sessiond_exchange *exchange,
	const char *line)
{
	char text[SESSIOND_LINE_MAX + 2U];
	int length;

	/* The line and its end, in one write. */
	length = snprintf(text, sizeof(text), "%s\n", line);
	if (length > 0 && (size_t)length < sizeof(text))
		(void)write(exchange->socket, text, (size_t)length);
}

/* Looks a name's user ID up; 1 when it is an account's. */
static int
auth_lookup(
	const char *name,
	uid_t *uid)
{
	struct passwd account;
	struct passwd *found;
	char buffer[SESSIOND_ACCOUNT_BUFFER];
	int error;

	/* The account in /etc/passwd. */
	found = NULL;
	error = getpwnam_r(name, &account, buffer, sizeof(buffer), &found);
	if (error != 0 || found == NULL)
		return 0;

	/* Succeeded: the account's user ID. */
	*uid = account.pw_uid;
	return 1;
}

/* Wipes a secret through a volatile pointer, which the compiler may not drop. */
static void
auth_wipe(
	void *memory,
	size_t size)
{
	volatile unsigned char *byte;
	size_t index;

	/* Every byte. */
	byte = memory;
	for (index = 0U; index < size; index++)
		byte[index] = 0U;
}
