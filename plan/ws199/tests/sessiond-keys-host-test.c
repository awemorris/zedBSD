/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws199-p001 i03: the host test of sessiond's requests of a security key's
 * own (KEYINFO, KEYPIN, KEYRESET) with a fake passkey (a script the .sh
 * writes), over a socket pair as the session uses: KEYINFO and KEYPIN
 * touch no count and are told at once, a reset's wrong password is a
 * delayed password failure, a reset whose password was right clears the
 * counts (the PIN is offered) and its later failure is told at once, the
 * status lines become REPLUG and TOUCH, a change is told passkey's own
 * word, and a CANCEL'ed attempt is told at once.
 */

#include "userland/desktop/sessiond/auth.h"

#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* The longest an answer told at once may take (passkey's run), and the delay of a first failure. */
#define TEST_AT_ONCE_MS		1000LL
#define TEST_DELAYED_MS		1500LL

/* The checks that failed. */
static int failures;

static void check(int condition, const char *what);
static void send_line(struct sessiond_exchange *exchange, const char *text);
static int answer(struct sessiond_exchange *exchange, int peer, char *line, size_t size, int timeout_ms);
static void test_keys(const char *name);

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

	/* About the test's own account (the fake passkey answers its user ID). */
	self = getpwuid(getuid());
	check(self != NULL, "the test's own account");
	if (self != NULL)
		test_keys(self->pw_name);

	/* The result. */
	if (failures != 0) {
		printf("sessiond-keys-host-test: %d failures\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("sessiond-keys-host-test: ok\n");
	return 0;
}

/* Notes a check that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Only a failure is said. */
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* Gives the exchange one line, as the session sends it. */
static void
send_line(
	struct sessiond_exchange *exchange,
	const char *text)
{
	char line[SESSIOND_LINE_MAX];
	int taken;

	/* The line (which the exchange may erase) and the exchange's taking it. */
	snprintf(line, sizeof(line), "%s", text);
	taken = sessiond_exchange_line(exchange, line);
	check(taken == 1, "the exchange takes the line");
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
	long long now;
	size_t used;
	ssize_t count;
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

		/* The line's end. */
		if (byte == '\n') {
			line[used] = '\0';
			return 1;
		}

		/* One more byte of it (one past the buffer is dropped). */
		if (used + 1U < size) {
			line[used] = byte;
			used++;
		}
	}

	/* No line in time. */
	return 0;
}

/* The keys' requests, in an order whose counts each check knows. */
static void
test_keys(
	const char *name)
{
	struct sessiond_exchange exchange;
	struct sessiond_account owner;
	struct passwd *found;
	char line[SESSIOND_LINE_MAX];
	long long started;
	long long took;
	int pair[2];
	int got;
	int same;
	int error;

	/* The session's end and sessiond's, the session's own user. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (error != 0) {
		check(0, "socketpair");
		return;
	}

	/* The session's own user. */
	memset(&owner, 0, sizeof(owner));
	found = NULL;
	(void)getpwnam_r(name, &owner.passwd, owner.buffer, sizeof(owner.buffer), &found);
	sessiond_exchange_init(&exchange, pair[0], &owner, NULL);

	/* KEYINFO: the key's facts as passkey gave them. */
	send_line(&exchange, "KEYINFO");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	same = got && strcmp(line, "KEYINFO count=1 name=59 pin=1 retries=8 min=4") == 0;
	check(same, "KEYINFO tells the key's facts");

	/* KEYPIN set refused: passkey's word, at once. */
	started = sessiond_milliseconds();
	send_line(&exchange, "KEYPIN set");
	send_line(&exchange, "short");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	took = sessiond_milliseconds() - started;
	check(got && strcmp(line, "FAIL pin-policy") == 0, "KEYPIN set tells pin-policy");
	check(took < TEST_AT_ONCE_MS, "KEYPIN's failure is told at once");

	/* KEYPIN change with the wrong key PIN: the key's word, at once. */
	send_line(&exchange, "KEYPIN change");
	send_line(&exchange, "wrong");
	send_line(&exchange, "fresh");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "FAIL bad-key-pin") == 0, "KEYPIN change tells bad-key-pin");

	/* KEYPIN set: OK. */
	send_line(&exchange, "KEYPIN set");
	send_line(&exchange, "right");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "OK") == 0, "KEYPIN set OK");

	/* None of them proved the password: no PIN offered yet (review-2 N1). */
	send_line(&exchange, "STYLES");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password") == 0, "KEYINFO and KEYPIN do not offer the PIN");

	/* KEYRESET with a wrong password: a password failure, delayed. */
	started = sessiond_milliseconds();
	send_line(&exchange, "KEYRESET");
	send_line(&exchange, "wrong");
	got = answer(&exchange, pair[1], line, sizeof(line), 8000);
	took = sessiond_milliseconds() - started;
	check(got && strcmp(line, "FAIL bad-secret") == 0, "KEYRESET's wrong password is bad-secret");
	check(took >= TEST_DELAYED_MS, "KEYRESET's wrong password waits out the delay");

	/* KEYRESET whose password was right, then a failure of the key: REPLUG, and the failure at once (review-2 N6). */
	started = sessiond_milliseconds();
	send_line(&exchange, "KEYRESET");
	send_line(&exchange, "late");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "REPLUG") == 0, "KEYRESET tells REPLUG");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	took = sessiond_milliseconds() - started;
	check(got && strcmp(line, "FAIL not-allowed") == 0, "KEYRESET's key failure tells its word");
	check(took < TEST_AT_ONCE_MS, "a failure after the right password is told at once");

	/* The right password cleared the counts: the PIN is offered. */
	send_line(&exchange, "STYLES");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "STYLES password pin") == 0, "KEYRESET's right password offers the PIN");

	/* KEYRESET done: REPLUG, TOUCH, OK removed=1. */
	send_line(&exchange, "KEYRESET");
	send_line(&exchange, "right");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "REPLUG") == 0, "KEYRESET done: REPLUG");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "TOUCH") == 0, "KEYRESET done: TOUCH");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	check(got && strcmp(line, "OK removed=1") == 0, "KEYRESET done: OK removed=1");

	/* ENROLL fido2 is told passkey's own word (review-2 N8). */
	send_line(&exchange, "ENROLL fido2 Desk key");
	send_line(&exchange, "right");
	send_line(&exchange, "1234");
	got = answer(&exchange, pair[1], line, sizeof(line), 8000);
	check(got && strcmp(line, "FAIL bad-key-pin") == 0, "ENROLL tells bad-key-pin");

	/* UNLOCK fido2 CANCEL'ed: timeout at once (review-3 R5). */
	started = sessiond_milliseconds();
	send_line(&exchange, "UNLOCK fido2");
	send_line(&exchange, "1234");
	(void)answer(&exchange, pair[1], line, sizeof(line), 200);
	send_line(&exchange, "CANCEL");
	got = answer(&exchange, pair[1], line, sizeof(line), 5000);
	took = sessiond_milliseconds() - started;
	check(got && strcmp(line, "FAIL timeout") == 0, "a CANCEL'ed unlock tells timeout");
	check(took < 200LL + TEST_AT_ONCE_MS + 500LL, "a CANCEL'ed unlock is told without the delay");

	/* The exchange ends. */
	sessiond_exchange_stop(&exchange);
	(void)close(pair[0]);
	(void)close(pair[1]);
}
