/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of sessiond's authentication (ws172-p002): the counts'
 * rules (auth-policy.c) and the exchange (auth.c) with a fake passkey (a
 * script the .sh writes), over a socket pair as the greeter and the
 * session use.
 */

#include "userland/desktop/sessiond/auth.h"

#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The delays after each failure in a row. */
static const unsigned test_delays[] = { 2U, 2U, 2U, 4U, 4U, 4U, 8U, 8U, 8U, 16U, 16U, 16U, 16U, 16U };

/* The checks that failed. */
static int failures;

static void check(int condition, const char *what);
static void test_policy(void);
static void test_exchange(const char *name);
static void test_change_offers_pin(const char *name);
static int answer(struct sessiond_exchange *exchange, int peer, char *line, size_t size, int timeout_ms);
static int file_put(const char *path, const char *text);
static int file_says(const char *path, const char *text);
static void send_line(struct sessiond_exchange *exchange, const char *text);

/* sessiond's own (main.c), for the test. */
long long
sessiond_milliseconds(void)
{
	struct timespec now;

	/* The monotonic clock, in milliseconds. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000L;
}

void
sessiond_log(
	const char *format,
	...)
{
	va_list arguments;

	/* One line on standard error (the .sh keeps it). */
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
}

int
main(void)
{
	struct passwd *self;
	pid_t child;
	uid_t uid;
	int status;

	/* The counts' rules, and no PIN's try kept from an earlier run. */
	test_policy();
	check(file_put(SESSIOND_PIN_TRIES_PATH, ""), "the tries file emptied");

	/* The exchange, about the test's own account (the fake passkey answers its user ID). */
	uid = getuid();
	self = getpwuid(uid);
	check(self != NULL, "the test's own account");

	/* A change by the password offers the PIN (BUG-285), from the counts of a fresh sessiond: in a child, before any login here. */
	if (self != NULL) {
		child = fork();
		if (child == 0) {
			test_change_offers_pin(self->pw_name);
			if (failures != 0)
				_exit(1);
			_exit(0);
		}

		/* The child's result. */
		status = 1;
		if (child > 0)
			(void)waitpid(child, &status, 0);
		check(child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "a change offers the PIN (the child)");
	}

	/* The exchange from here on. */
	if (self != NULL)
		test_exchange(self->pw_name);

	/* The result. */
	if (failures != 0) {
		printf("sessiond-auth-host-test: %d failures\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("sessiond-auth-host-test: ok\n");
	return 0;
}

static void
check(
	int condition,
	const char *what)
{
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* The counts' rules. */
static void
test_policy(void)
{
	static struct sessiond_policy policy;
	static struct sessiond_policy read_back;
	char file[SESSIOND_PIN_TRIES_TEXT];
	struct sessiond_count *count;
	struct sessiond_count *other;
	char styles[64];
	char text[32];
	char *cursor;
	unsigned index;

	/* The words. */
	snprintf(text, sizeof(text), " a  b,c ");
	cursor = text;
	check(strcmp(sessiond_policy_word(&cursor, ' '), "a") == 0, "word a");
	check(strcmp(sessiond_policy_word(&cursor, ' '), "b,c") == 0, "word b,c");
	check(sessiond_policy_word(&cursor, ' ') == NULL, "word end");
	check(sessiond_policy_word(&cursor, ' ') == NULL, "word end again");

	/* Counts by user ID; unknown names share one. */
	count = sessiond_policy_count(&policy, 1, 1000);
	check(sessiond_policy_count(&policy, 1, 1000) == count, "same uid, same count");
	other = sessiond_policy_count(&policy, 1, 1001);
	check(other != count, "another uid, another count");
	check(sessiond_policy_count(&policy, 0, 0) == &policy.unknown, "unknown names share");
	check(sessiond_policy_count(&policy, 0, 1000) == &policy.unknown, "unknown with a uid shares");

	/* The PIN: offered from the start (a machine without a keyboard logs in with it). */
	check(sessiond_policy_pin_allowed(count), "the PIN from the start");
	sessiond_policy_styles("password,pin,fido2", count, styles, sizeof(styles));
	check(strcmp(styles, "password pin fido2") == 0, "styles with the PIN from the start");
	sessiond_policy_attempt(count, SESSIOND_STYLE_PASSWORD);
	sessiond_policy_success(count, SESSIOND_STYLE_PASSWORD);
	check(sessiond_policy_pin_allowed(count), "PIN after a password");
	sessiond_policy_styles("password,pin", count, styles, sizeof(styles));
	check(strcmp(styles, "password pin") == 0, "styles with the PIN");

	/* A right PIN keeps it offered and clears its wrong tries. */
	sessiond_policy_attempt(other, SESSIOND_STYLE_PIN);
	sessiond_policy_success(other, SESSIOND_STYLE_PIN);
	check(sessiond_policy_pin_allowed(other) && other->pin_wrong == 0U, "a PIN success keeps the PIN");

	/* Five wrong PINs turn it off; a password turns it on again. */
	for (index = 0U; index < 4U; index++)
		sessiond_policy_attempt(count, SESSIOND_STYLE_PIN);
	check(sessiond_policy_pin_allowed(count), "PIN after four wrong");
	sessiond_policy_attempt(count, SESSIOND_STYLE_PIN);
	check(!sessiond_policy_pin_allowed(count), "PIN off after five wrong");
	sessiond_policy_styles("password,pin", count, styles, sizeof(styles));
	check(strcmp(styles, "password") == 0, "styles without the PIN after five");
	sessiond_policy_attempt(count, SESSIOND_STYLE_PASSWORD);
	sessiond_policy_success(count, SESSIOND_STYLE_PASSWORD);
	check(sessiond_policy_pin_allowed(count), "PIN back after a password");

	/* A wrong password between wrong PINs keeps the PIN's count. */
	for (index = 0U; index < 4U; index++)
		sessiond_policy_attempt(count, SESSIOND_STYLE_PIN);
	sessiond_policy_attempt(count, SESSIOND_STYLE_PASSWORD);
	sessiond_policy_attempt(count, SESSIOND_STYLE_PIN);
	check(!sessiond_policy_pin_allowed(count), "PIN off across a wrong password");
	sessiond_policy_success(count, SESSIOND_STYLE_FIDO2);
	check(sessiond_policy_pin_allowed(count), "PIN back after a key");

	/* The PIN's tries in their file's text: written for the accounts with some, read back the larger. */
	for (index = 0U; index < 2U; index++)
		sessiond_policy_attempt(count, SESSIOND_STYLE_PIN);
	check(sessiond_policy_tries_write(&policy, file, sizeof(file)) != 0U, "tries written");
	check(strcmp(file, "# sessiond pin-tries 1\n1000 2\n") == 0, "tries text");
	check(sessiond_policy_tries_write(&policy, file, 20U) == 0U, "tries that do not fit");
	memset(&read_back, 0, sizeof(read_back));
	sessiond_policy_tries_read(&read_back, "# sessiond pin-tries 1\n1000 2\n1001 9\nx 3\n1002 1x\n1003 4");
	check(sessiond_policy_count(&read_back, 1, 1000)->pin_wrong == 2U, "tries read");
	check(sessiond_policy_count(&read_back, 1, 1001)->pin_wrong == SESSIOND_PIN_TRIES, "tries bounded");
	check(!sessiond_policy_pin_allowed(sessiond_policy_count(&read_back, 1, 1001)), "PIN off from the file");
	check(sessiond_policy_count(&read_back, 1, 1002)->pin_wrong == 0U, "a bad line skipped");
	check(sessiond_policy_count(&read_back, 1, 1003)->pin_wrong == 4U, "the last line without its end");
	memset(&read_back, 0, sizeof(read_back));
	sessiond_policy_tries_read(&read_back, "# other 1\n1000 3\n");
	check(sessiond_policy_count(&read_back, 1, 1000)->pin_wrong == 0U, "another file's text ignored");
	sessiond_policy_success(count, SESSIOND_STYLE_PASSWORD);

	/* The delays: 2, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16... */
	count->wrong = 0U;
	check(sessiond_policy_delay(count) == 2U, "delay before any");
	for (index = 0U; index < sizeof(test_delays) / sizeof(test_delays[0]); index++) {
		sessiond_policy_attempt(count, SESSIOND_STYLE_PASSWORD);
		check(sessiond_policy_delay(count) == test_delays[index], "delay");
	}

	/* The reasons. */
	check(strcmp(sessiond_policy_reason("no-such-user"), "bad-secret") == 0, "no-such-user is bad-secret");
	check(strcmp(sessiond_policy_reason("not-enrolled"), "bad-secret") == 0, "not-enrolled is bad-secret");
	check(strcmp(sessiond_policy_reason("locked-account"), "locked") == 0, "locked-account is locked");
	check(strcmp(sessiond_policy_reason("key-locked"), "locked") == 0, "key-locked is locked");
	check(strcmp(sessiond_policy_reason("cloned"), "cloned") == 0, "cloned is told as it is (ws199-p001 R6)");
	check(strcmp(sessiond_policy_reason("timeout"), "timeout") == 0, "timeout");
	check(strcmp(sessiond_policy_reason("anything"), "bad-secret") == 0, "unknown reason");

	/* A full table gives the shared entry. */
	for (index = 0U; index < SESSIOND_COUNTS_MAX + 2U; index++)
		(void)sessiond_policy_count(&policy, 1, 5000U + index);
	check(sessiond_policy_count(&policy, 1, 9999U) == &policy.unknown, "full table shares");
	check(sessiond_policy_count(&policy, 1, 1000) == count, "old entry kept");
}

/* Sends one line to the exchange, as the greeter or the session would. */
static void
send_line(
	struct sessiond_exchange *exchange,
	const char *text)
{
	char line[SESSIOND_LINE_MAX];

	/* The line (which the exchange may erase) and the exchange's taking it. */
	snprintf(line, sizeof(line), "%s", text);
	check(sessiond_exchange_line(exchange, line) == 1, "the exchange takes the line");
}

/* Waits for the exchange's next line on the peer's end, ticking it as sessiond's loop does. */
static int
answer(
	struct sessiond_exchange *exchange,
	int peer,
	char *line,
	size_t size,
	int timeout_ms)
{
	struct pollfd entry;
	long long deadline;
	size_t used;
	ssize_t count;
	long long now;
	char byte;
	int ready;

	/* Byte after byte until the line ends or the time is up. */
	deadline = sessiond_milliseconds() + timeout_ms;
	now = sessiond_milliseconds();
	used = 0U;
	while (now < deadline) {
		sessiond_exchange_tick(exchange);
		entry.fd = peer;
		entry.events = POLLIN;
		entry.revents = 0;
		ready = poll(&entry, 1, 20);
		now = sessiond_milliseconds();
		if (ready <= 0)
			continue;

		/* A byte; the socket's end is no line. */
		count = read(peer, &byte, 1U);
		if (count <= 0)
			return 0;

		/* The line's end, or one more byte of it. */
		if (byte == '\n') {
			line[used] = '\0';
			return 1;
		}

		/* A byte past the buffer is dropped. */
		if (used + 1U < size) {
			line[used] = byte;
			used++;
		}
	}

	/* No line in time. */
	return 0;
}

/* The exchange with the fake passkey (its answers depend on the secret, see the .sh). */
static void
test_exchange(
	const char *name)
{
	struct sessiond_exchange exchange;
	struct sessiond_account owner;
	struct sessiond_account login;
	struct passwd *found;
	char line[SESSIOND_LINE_MAX];
	char text[SESSIOND_LINE_MAX];
	char expected[64];
	long long started;
	long long took;
	int pair[2];
	int got;
	int error;

	/* The socket pair, sessiond's end and the greeter's or the session's. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (error != 0) {
		check(0, "socketpair");
		return;
	}

	/* The greeter's: STYLES before any login has the PIN. */
	memset(&login, 0, sizeof(login));
	sessiond_exchange_init(&exchange, pair[0], NULL, &login);
	snprintf(text, sizeof(text), "STYLES %s", name);
	send_line(&exchange, text);
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password pin") == 0, "greeter STYLES with the PIN from the start");

	/* A wrong PIN before any password: tried, refused after the delay, and its try is in the file. */
	snprintf(text, sizeof(text), "AUTH %s pin", name);
	send_line(&exchange, text);
	send_line(&exchange, "123456");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "FAIL bad-secret") == 0, "a wrong PIN before a password");
	snprintf(expected, sizeof(expected), "# sessiond pin-tries 1\n%u 1\n", (unsigned)getuid());
	check(file_says(SESSIOND_PIN_TRIES_PATH, expected), "the PIN's try in the file");

	/* A wrong password: FAIL after the 2-second delay. */
	snprintf(text, sizeof(text), "AUTH %s password", name);
	send_line(&exchange, text);
	check(sessiond_exchange_busy(&exchange) == 0, "not busy before the secret");
	started = sessiond_milliseconds();
	send_line(&exchange, "wrong");
	check(sessiond_exchange_busy(&exchange), "busy after the secret");
	send_line(&exchange, "STYLES x");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "ERROR busy") == 0, "busy answer");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	took = sessiond_milliseconds() - started;
	check(got && strcmp(line, "FAIL bad-secret") == 0, "wrong password");
	check(took >= 1900 && took < 4000, "the delay of a wrong password");

	/* A right password with a touch: TOUCH, then OK, and the account is taken. */
	snprintf(text, sizeof(text), "AUTH %s password", name);
	send_line(&exchange, text);
	send_line(&exchange, "touch");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "TOUCH") == 0, "touch relayed");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "OK") == 0, "right password");
	check(exchange.logged_in && login.passwd.pw_name != NULL && strcmp(login.passwd.pw_name, name) == 0, "login account");
	check(file_says(SESSIOND_PIN_TRIES_PATH, "# sessiond pin-tries 1\n"), "the PIN's tries cleared in the file");

	/* Now the PIN is offered. */
	exchange.logged_in = 0;
	snprintf(text, sizeof(text), "STYLES %s", name);
	send_line(&exchange, text);
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password pin") == 0, "greeter STYLES with the PIN");

	/* A uid passkey gets wrong is a failure. */
	snprintf(text, sizeof(text), "AUTH %s password", name);
	send_line(&exchange, text);
	send_line(&exchange, "otheruid");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strncmp(line, "FAIL", 4U) == 0 && !exchange.logged_in, "a wrong uid fails");

	/* A name of no account is told as a wrong secret. */
	send_line(&exchange, "AUTH no-such-account-zz password");
	send_line(&exchange, "right");
	got = answer(&exchange, pair[1], line, sizeof(line), 6000);
	check(got && strcmp(line, "FAIL bad-secret") == 0 && !exchange.logged_in, "unknown name");

	/* The greeter may not ask the session's requests. */
	send_line(&exchange, "ENROLLED");
	got = answer(&exchange, pair[1], line, sizeof(line), 2000);
	check(got && strcmp(line, "ERROR") == 0, "greeter ENROLLED refused");

	/* Other requests are not the exchange's (and the line is left as it was). */
	snprintf(line, sizeof(line), "POWER reboot");
	check(sessiond_exchange_line(&exchange, line) == 0 && strcmp(line, "POWER reboot") == 0, "POWER is not the exchange's");

	/* CANCEL of a hanging passkey: killed, FAIL timeout after the delay. */
	snprintf(text, sizeof(text), "AUTH %s password", name);
	send_line(&exchange, text);
	send_line(&exchange, "hang");
	got = answer(&exchange, pair[1], line, sizeof(line), 300);
	check(!got, "nothing while it hangs");
	send_line(&exchange, "CANCEL");
	got = answer(&exchange, pair[1], line, sizeof(line), 8000);
	check(got && strcmp(line, "FAIL timeout") == 0, "cancel");
	check(!sessiond_exchange_busy(&exchange), "not busy after cancel");

	/* The deadline: a passkey that ignores TERM is killed after the grace. */
	snprintf(text, sizeof(text), "AUTH %s password", name);
	send_line(&exchange, text);
	send_line(&exchange, "stubborn");
	got = answer(&exchange, pair[1], line, sizeof(line), 30000);
	check(got && strcmp(line, "FAIL timeout") == 0, "deadline and kill");

	/* A passkey that ends without an answer is an internal failure, not a timeout. */
	snprintf(text, sizeof(text), "AUTH %s password", name);
	send_line(&exchange, text);
	send_line(&exchange, "silent");
	got = answer(&exchange, pair[1], line, sizeof(line), 30000);
	check(got && strcmp(line, "FAIL internal") == 0, "no answer is internal");

	/* Without passkey: AUTH refused at once (not counted), STYLES only the password. */
	error = chmod(SESSIOND_PASSKEY, 0600);
	check(error == 0, "passkey made not executable");
	snprintf(text, sizeof(text), "AUTH %s password", name);
	send_line(&exchange, text);
	started = sessiond_milliseconds();
	send_line(&exchange, "right");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	took = sessiond_milliseconds() - started;
	check(got && strcmp(line, "FAIL internal") == 0 && took < 1000, "missing passkey fails at once");
	snprintf(text, sizeof(text), "STYLES %s", name);
	send_line(&exchange, text);
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password") == 0, "missing passkey STYLES");
	error = chmod(SESSIOND_PASSKEY, 0700);
	check(error == 0, "passkey made executable again");
	sessiond_exchange_stop(&exchange);

	/* The session's: its own user, UNLOCK, ENROLLED, ENROLL pin, REMOVE pin. */
	memset(&owner, 0, sizeof(owner));
	found = NULL;
	(void)getpwnam_r(name, &owner.passwd, owner.buffer, sizeof(owner.buffer), &found);
	sessiond_exchange_init(&exchange, pair[0], &owner, NULL);
	send_line(&exchange, "UNLOCK password");
	send_line(&exchange, "right");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "OK") == 0 && !exchange.logged_in, "unlock");
	send_line(&exchange, "STYLES");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password pin") == 0, "session STYLES");
	send_line(&exchange, "ENROLLED");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "ENROLLED pin=1 fido2=0") == 0, "enrolled");
	send_line(&exchange, "ENROLL pin");
	send_line(&exchange, "right");
	send_line(&exchange, "654321");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "OK") == 0, "enroll pin");
	send_line(&exchange, "ENROLL fido2 My Key");
	send_line(&exchange, "right");
	send_line(&exchange, "1234");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "OK id=Q1") == 0, "enroll fido2 with a spaced label");
	send_line(&exchange, "REMOVE pin");
	send_line(&exchange, "wrong");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "FAIL bad-secret") == 0, "remove pin with a wrong password");
	send_line(&exchange, "AUTH x password");
	got = answer(&exchange, pair[1], line, sizeof(line), 2000);
	check(got && strcmp(line, "ERROR") == 0, "session AUTH refused");
	sessiond_exchange_stop(&exchange);
	close(pair[0]);
	close(pair[1]);
}

/*
 * A session that started without the password (an automatic login): the
 * PIN from the start, but not while the file keeps five wrong tries of the
 * account from before a restart; a change by the password clears them.
 */
static void
test_change_offers_pin(
	const char *name)
{
	struct sessiond_exchange exchange;
	struct sessiond_account owner;
	struct passwd *found;
	char line[SESSIOND_LINE_MAX];
	char text[64];
	int pair[2];
	int got;
	int error;

	/* The session's end and sessiond's. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (error != 0) {
		check(0, "socketpair (change)");
		return;
	}

	/* The session's own user. */
	memset(&owner, 0, sizeof(owner));
	found = NULL;
	(void)getpwnam_r(name, &owner.passwd, owner.buffer, sizeof(owner.buffer), &found);
	sessiond_exchange_init(&exchange, pair[0], &owner, NULL);

	/* Five wrong tries kept from before the restart: no PIN. */
	snprintf(text, sizeof(text), "# sessiond pin-tries 1\n%u 5\n", (unsigned)getuid());
	check(file_put(SESSIOND_PIN_TRIES_PATH, text), "the tries file of before the restart");
	send_line(&exchange, "STYLES");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password") == 0, "no PIN after five wrong before a restart (change)");

	/* The PIN set with the password. */
	send_line(&exchange, "ENROLL pin");
	send_line(&exchange, "right");
	send_line(&exchange, "654321");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "OK") == 0, "enroll pin (change)");

	/* Offered from now on. */
	send_line(&exchange, "STYLES");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password pin") == 0, "the PIN offered after a change by the password");
	check(file_says(SESSIOND_PIN_TRIES_PATH, "# sessiond pin-tries 1\n"), "the tries cleared by the change");
	sessiond_exchange_stop(&exchange);
	(void)close(pair[0]);
	(void)close(pair[1]);
}

/* Writes a file whole (0600), in place of what it held.  Returns 1 when it did. */
static int
file_put(
	const char *path,
	const char *text)
{
	FILE *file;
	int closed;

	/* The file, emptied first. */
	file = fopen(path, "w");
	if (file == NULL)
		return 0;
	(void)chmod(path, 0600);
	(void)fputs(text, file);
	closed = fclose(file);
	return closed == 0;
}

/* Tells whether a file holds exactly a text. */
static int
file_says(
	const char *path,
	const char *text)
{
	char held[256];
	size_t length;
	FILE *file;

	/* The file whole. */
	file = fopen(path, "r");
	if (file == NULL)
		return 0;
	length = fread(held, 1U, sizeof(held) - 1U, file);
	(void)fclose(file);
	held[length] = '\0';
	return strcmp(held, text) == 0;
}
