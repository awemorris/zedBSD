/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of libkeiland-backend's session on zedBSD (ws131-p006):
 * session-zedbsd.c against a socket pair standing for sessiond, as the
 * login screen (the greeter's descriptor) and as a session (the control
 * descriptor).  Prints "host-session: N/M passed".
 *
 *   sh plan/ws131/tests/host-session.sh
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void check(int passed, const char *what);
static struct kl_backend *open_backend(int greeter, int session);
static void heard_stop(void *data, unsigned reason);
static void heard_answer(void *data, unsigned request, int error);
static int read_line(int descriptor, const char *expected);
static int administrator(void);
static void test_login_screen(void);
static void test_session(void);
static void test_unanswered(void);

static unsigned checks_run;
static unsigned checks_passed;

/* What the callbacks heard last, and how often. */
static unsigned stop_reason;
static unsigned stop_count;
static unsigned answer_request;
static int answer_error;
static unsigned answer_count;

/* The peer's descriptor the stop callback reads from, to see that RELEASED comes after it. */
static int stop_peer = -1;
static int released_before_stop;

/*
 * Runs the login screen's, the session's and the deadline's checks.
 */
int
main(
	void)
{
	/* A write to a closed peer reports EPIPE rather than ending the test. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* The three cases. */
	test_login_screen();
	test_session();
	test_unanswered();

	/* The summary. */
	printf("host-session: %u/%u passed\n", checks_passed, checks_run);
	if (checks_passed != checks_run)
		return 1;
	return 0;
}

/* Counts one check and prints it. */
static void
check(
	int passed,
	const char *what)
{
	checks_run++;
	if (passed)
		checks_passed++;
	printf("%s: %s\n", passed ? "ok" : "FAIL", what);
}

/* Opens a backend with the login screen's and the session's descriptors (-1 for none). */
static struct kl_backend *
open_backend(
	int greeter,
	int session)
{
	struct kl_backend_options options;
	struct kl_backend_host host;
	struct kl_backend *backend;
	int error;

	/* The compositor gives the backend descriptors that do not block. */
	if (greeter >= 0)
		(void)fcntl(greeter, F_SETFL, fcntl(greeter, F_GETFL) | O_NONBLOCK);
	if (session >= 0)
		(void)fcntl(session, F_SETFL, fcntl(session, F_GETFL) | O_NONBLOCK);

	/* The callbacks record what they heard. */
	memset(&options, 0, sizeof(options));
	memset(&host, 0, sizeof(host));
	options.greeter_descriptor = greeter;
	options.session_descriptor = session;
	host.session_stop = heard_stop;
	host.session_answer = heard_answer;
	error = kl_backend_open(&options, &host, &backend);
	if (error != 0)
		return NULL;

	/* Nothing heard yet. */
	stop_count = 0U;
	answer_count = 0U;
	return backend;
}

/* Records a stop, and whether RELEASED had already been written when it came. */
static void
heard_stop(
	void *data,
	unsigned reason)
{
	char byte;
	ssize_t count;

	/* The peer must not have RELEASED yet: the compositor gives the display back first. */
	(void)data;
	stop_reason = reason;
	stop_count++;
	if (stop_peer >= 0) {
		count = recv(stop_peer, &byte, 1U, MSG_DONTWAIT | MSG_PEEK);
		released_before_stop = count > 0;
	}
}

/* Records an answer. */
static void
heard_answer(
	void *data,
	unsigned request,
	int error)
{
	(void)data;
	answer_request = request;
	answer_error = error;
	answer_count++;
}

/* Reads one line from the peer and compares it. */
static int
read_line(
	int descriptor,
	const char *expected)
{
	char line[256];
	ssize_t count;
	int same;

	/* What the backend wrote (the test writes nothing else to this side). */
	memset(line, 0, sizeof(line));
	count = recv(descriptor, line, strlen(expected), MSG_DONTWAIT);
	if (count != (ssize_t)strlen(expected))
		return 0;
	same = strcmp(line, expected);
	return same == 0;
}

/* The login screen: READY and GO, AUTH and its answers, POWER's answer, the end with RELEASED after the stop. */
static void
test_login_screen(
	void)
{
	struct kl_backend *backend;
	int ends[2];
	int error;

	/* sessiond's end (0) and the login screen's (1). */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, ends);
	check(error == 0, "a socket pair stands for sessiond");
	if (error != 0)
		return;
	backend = open_backend(ends[1], -1);
	check(backend != NULL, "the login screen's backend opens");
	if (backend == NULL)
		return;
	check(kl_backend_session_managed(backend) == 0, "the login screen is not a managed session");

	/* GO already waiting: READY is written and the wait ends at once. */
	check(kl_backend_session_styles_get(backend) == KL_BACKEND_STYLE_PASSWORD, "the password alone before STYLES");
	(void)write(ends[0], "GO\n", 3U);
	error = kl_backend_session_ready(backend);
	check(error == 0 && read_line(ends[0], "READY\n"), "READY written, GO taken");

	/* An answer that came before GO (STYLES asked before READY, T1-203) is kept for the tick. */
	error = kl_backend_session_styles(backend, "kei");
	check(error == 0 && read_line(ends[0], "STYLES kei\n"), "STYLES kei written before READY");
	(void)write(ends[0], "STYLES password\nGO\n", 19U);
	error = kl_backend_session_ready(backend);
	check(error == 0 && read_line(ends[0], "READY\n") && answer_count == 0U, "GO taken after the answer, which waits");
	kl_backend_tick(backend, 998U);
	check(answer_count == 1U && answer_request == KL_BACKEND_SESSION_STYLES && answer_error == 0, "the answer before GO is given by the tick");
	answer_count = 0U;

	/* STYLES (ws172-p002): what sessiond said. */
	error = kl_backend_session_styles(backend, "kei");
	check(error == 0 && read_line(ends[0], "STYLES kei\n"), "STYLES kei written");
	(void)write(ends[0], "STYLES password pin\n", 20U);
	kl_backend_tick(backend, 999U);
	check(answer_count == 1U && answer_request == KL_BACKEND_SESSION_STYLES && answer_error == 0, "STYLES answered");
	check(kl_backend_session_styles_get(backend) == (KL_BACKEND_STYLE_PASSWORD | KL_BACKEND_STYLE_PIN), "the PIN among the styles");
	answer_count = 0U;

	/* STYLES without the password (WS200: the password turned off): the PIN and the key alone. */
	error = kl_backend_session_styles(backend, "kei");
	check(error == 0 && read_line(ends[0], "STYLES kei\n"), "STYLES kei written again");
	(void)write(ends[0], "STYLES pin fido2\n", 17U);
	kl_backend_tick(backend, 999U);
	check(kl_backend_session_styles_get(backend) == (KL_BACKEND_STYLE_PIN | KL_BACKEND_STYLE_KEY), "the password turned off is not among the styles");
	answer_count = 0U;

	/* An empty STYLES: the password. */
	error = kl_backend_session_styles(backend, "kei");
	check(error == 0 && read_line(ends[0], "STYLES kei\n"), "STYLES kei written a third time");
	(void)write(ends[0], "STYLES \n", 8U);
	kl_backend_tick(backend, 999U);
	check(kl_backend_session_styles_get(backend) == KL_BACKEND_STYLE_PASSWORD, "no styles listed is the password");
	answer_count = 0U;

	/* AUTH with its secret on a line of its own, and a second request while it waits: EBUSY. */
	error = kl_backend_session_authenticate(backend, "kei", KL_BACKEND_STYLE_PASSWORD, "secret");
	check(error == 0 && read_line(ends[0], "AUTH kei password\nsecret\n"), "AUTH kei password and the secret written");
	error = kl_backend_session_authenticate(backend, "kei", KL_BACKEND_STYLE_PIN, "123456");
	check(error == EBUSY, "a second request while one waits is EBUSY");
	error = kl_backend_session_authenticate(backend, "two words", KL_BACKEND_STYLE_PASSWORD, "x");
	check(error == EINVAL, "a name with a space is EINVAL");
	error = kl_backend_session_authenticate(backend, "kei", KL_BACKEND_STYLE_PASSWORD, "two\nlines");
	check(error == EINVAL, "a secret with a line end is EINVAL");
	error = kl_backend_session_authenticate(backend, "kei", 0x80U, "x");
	check(error == EINVAL, "an unknown style is EINVAL");
	error = kl_backend_session_unlock(backend, KL_BACKEND_STYLE_PASSWORD, "x");
	check(error == ENOTSUP, "the login screen has no unlock");
	error = kl_backend_session_set_pin(backend, "x", "123456");
	check(error == ENOTSUP, "the login screen sets no PIN");

	/* TOUCH is told but answers nothing (ws172-p003): the request still waits. */
	(void)write(ends[0], "TOUCH\n", 6U);
	kl_backend_tick(backend, 1000U);
	check(answer_count == 1U && answer_request == KL_BACKEND_SESSION_TOUCH && answer_error == 0, "TOUCH is told");
	error = kl_backend_session_authenticate(backend, "kei", KL_BACKEND_STYLE_PASSWORD, "x");
	check(error == EBUSY, "after TOUCH the request still waits");

	/* FAIL with its word answers AUTH with EACCES. */
	(void)write(ends[0], "FAIL bad-secret\n", 16U);
	kl_backend_tick(backend, 1000U);
	answer_count--;
	check(answer_count == 1U && answer_request == KL_BACKEND_SESSION_AUTH && answer_error == EACCES, "FAIL answers AUTH with EACCES");
	check(strcmp(kl_backend_session_reason(backend), "bad-secret") == 0, "the refusal's word is kept");

	/* ERROR busy is EBUSY. */
	(void)kl_backend_session_authenticate(backend, "kei", KL_BACKEND_STYLE_PIN, "123456");
	(void)read_line(ends[0], "AUTH kei pin\n123456\n");
	(void)write(ends[0], "ERROR busy\n", 11U);
	kl_backend_tick(backend, 1000U);
	check(answer_count == 2U && answer_error == EBUSY && kl_backend_session_reason(backend)[0] == '\0', "ERROR busy is EBUSY");
	answer_count = 1U;

	/* ERROR and OK, split over two reads. */
	(void)kl_backend_session_authenticate(backend, "kei", KL_BACKEND_STYLE_PASSWORD, "secret");
	(void)read_line(ends[0], "AUTH kei password\nsecret\n");
	(void)write(ends[0], "ERR", 3U);
	kl_backend_tick(backend, 1001U);
	check(answer_count == 1U, "half a line answers nothing");
	(void)write(ends[0], "OR\n", 3U);
	kl_backend_tick(backend, 1002U);
	check(answer_count == 2U && answer_error == EIO, "ERROR answers with EIO");

	/* Power's OK is power's answer. */
	error = kl_backend_power_action(backend, KL_BACKEND_POWER_REBOOT);
	check(error == 0 && read_line(ends[0], "POWER reboot\n"), "POWER reboot written");
	(void)write(ends[0], "OK\n", 3U);
	kl_backend_tick(backend, 1003U);
	check(answer_count == 3U && answer_request == KL_BACKEND_SESSION_POWER && answer_error == 0, "OK answers POWER");

	/* A line nobody asked for: request NONE, EPROTO. */
	(void)write(ends[0], "HELLO\n", 6U);
	kl_backend_tick(backend, 1004U);
	check(answer_count == 4U && answer_request == KL_BACKEND_SESSION_NONE && answer_error == EPROTO, "an unasked line is NONE, EPROTO");

	/* sessiond shuts its side down: ENDED, then RELEASED (written after the stop). */
	stop_peer = ends[0];
	released_before_stop = 0;
	(void)shutdown(ends[0], SHUT_WR);
	kl_backend_tick(backend, 1005U);
	check(stop_count == 1U && stop_reason == KL_BACKEND_SESSION_ENDED, "the shut-down side ends the login screen");
	check(!released_before_stop && read_line(ends[0], "RELEASED\n"), "RELEASED comes after the stop");
	stop_peer = -1;

	/* Nothing more is read after the end. */
	kl_backend_tick(backend, 1006U);
	check(stop_count == 1U, "the end is told once");
	kl_backend_close(backend);
	(void)close(ends[0]);
	(void)close(ends[1]);
}

/* A session: POWER, LOGOUT and QUIT with RELEASED after the stop, UNLOCK, STYLES, ENROLLED, the PIN, and sessiond leaving. */
static void
test_session(
	void)
{
	struct kl_backend *backend;
	struct kl_backend_key keys_listed[5];
	size_t listed;
	unsigned pin;
	unsigned keys;
	int ends[2];
	int error;
	int admin;

	/* sessiond's end (0) and the session's (1). */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, ends);
	if (error != 0)
		return;
	backend = open_backend(-1, ends[1]);
	check(backend != NULL, "the session's backend opens");
	if (backend == NULL)
		return;
	check(kl_backend_session_managed(backend) == 1, "a session sessiond started is managed");
	error = kl_backend_session_authenticate(backend, "kei", KL_BACKEND_STYLE_PASSWORD, "x");
	check(error == ENOTSUP, "a session has no log in");

	/*
	 * Power Off from a session (ws131-p027, root or wheel only): for such a
	 * user POWER is written, sessiond's refusal is EACCES and lets the
	 * action be asked again, its OK answers it; for another user nothing is
	 * offered (this host's user decides which is checked).
	 */
	admin = administrator();
	if (admin) {
		error = kl_backend_power_action(backend, KL_BACKEND_POWER_POWEROFF);
		check(error == 0 && read_line(ends[0], "POWER poweroff\n"), "wheel: a session's POWER poweroff written");
		(void)write(ends[0], "FAIL wheel\n", 11U);
		kl_backend_tick(backend, 2000U);
		check(answer_count == 1U && answer_request == KL_BACKEND_SESSION_POWER && answer_error == EACCES, "FAIL wheel answers POWER as EACCES");
		error = kl_backend_power_action(backend, KL_BACKEND_POWER_REBOOT);
		check(error == 0 && read_line(ends[0], "POWER reboot\n"), "refused: the action may be asked again");
		(void)write(ends[0], "OK\n", 3U);
		kl_backend_tick(backend, 2000U);
		check(answer_count == 2U && answer_request == KL_BACKEND_SESSION_POWER && answer_error == 0, "OK answers POWER");
	} else {
		error = kl_backend_power_action(backend, KL_BACKEND_POWER_POWEROFF);
		check(error == ENOTSUP, "not in wheel: a session's poweroff is ENOTSUP");
		check(answer_count == 0U, "not in wheel: nothing asked");
	}
	answer_count = 0U;

	/* UNLOCK and OK. */
	error = kl_backend_session_unlock(backend, KL_BACKEND_STYLE_PIN, "123456");
	check(error == 0 && read_line(ends[0], "UNLOCK pin\n123456\n"), "UNLOCK pin and the PIN written");
	(void)write(ends[0], "OK\n", 3U);
	kl_backend_tick(backend, 2000U);
	check(answer_count == 1U && answer_request == KL_BACKEND_SESSION_UNLOCK && answer_error == 0, "OK answers UNLOCK");

	/* A session's STYLES names nobody. */
	error = kl_backend_session_styles(backend, NULL);
	check(error == 0 && read_line(ends[0], "STYLES\n"), "a session's STYLES written");
	(void)write(ends[0], "STYLES password\n", 16U);
	kl_backend_tick(backend, 2000U);
	check(answer_count == 2U && kl_backend_session_styles_get(backend) == KL_BACKEND_STYLE_PASSWORD, "STYLES password answered");

	/* ENROLLED: what is enrolled. */
	error = kl_backend_session_enrolled(backend);
	check(error == 0 && read_line(ends[0], "ENROLLED\n"), "ENROLLED written");
	(void)write(ends[0], "ENROLLED pin=1 fido2=2\n", 23U);
	kl_backend_tick(backend, 2000U);
	kl_backend_session_enrolled_get(backend, &pin, &keys);
	check(answer_count == 3U && answer_request == KL_BACKEND_SESSION_ENROLLED && pin == 1U && keys == 2U, "ENROLLED answered");

	/* ENROLL pin with the password and the PIN; REMOVE pin with the password. */
	error = kl_backend_session_set_pin(backend, "secret", "123456");
	check(error == 0 && read_line(ends[0], "ENROLL pin\nsecret\n123456\n"), "ENROLL pin and its lines written");
	(void)write(ends[0], "OK\n", 3U);
	kl_backend_tick(backend, 2000U);
	check(answer_count == 4U && answer_request == KL_BACKEND_SESSION_ENROLL && answer_error == 0, "OK answers ENROLL");
	error = kl_backend_session_set_pin(backend, "secret", "");
	check(error == 0 && read_line(ends[0], "REMOVE pin\nsecret\n"), "REMOVE pin and the password written");
	(void)write(ends[0], "FAIL locked\n", 12U);
	kl_backend_tick(backend, 2000U);
	check(answer_count == 5U && answer_error == EACCES && strcmp(kl_backend_session_reason(backend), "locked") == 0, "FAIL locked answers REMOVE");

	/* The keys (ws172-p003): listed by ENROLLED, added with a label, the password and the key's PIN, removed by reference. */
	error = kl_backend_session_enrolled(backend);
	check(error == 0 && read_line(ends[0], "ENROLLED\n"), "ENROLLED written again");
	(void)write(ends[0], "ENROLLED pin=0 fido2=2 key=0123456789abcdef/59756269 key=fedcba9876543210/4b657920320a\n", 87U);
	kl_backend_tick(backend, 2000U);
	listed = kl_backend_session_keys_get(backend, keys_listed, 5U);
	check(listed == 1U && strcmp(keys_listed[0].ref, "0123456789abcdef") == 0 && strcmp(keys_listed[0].label, "Yubi") == 0,
	    "ENROLLED's keys (one with a line end in its label is left out)");
	error = kl_backend_session_add_key(backend, "secret", "Yubi Key", "1234");
	check(error == 0 && read_line(ends[0], "ENROLL fido2 Yubi Key\nsecret\n1234\n"), "ENROLL fido2 with its label and lines written");
	(void)write(ends[0], "TOUCH\nOK id=AQID\n", 17U);
	kl_backend_tick(backend, 2000U);
	check(answer_count == 8U && answer_request == KL_BACKEND_SESSION_ENROLL && answer_error == 0, "TOUCH, then OK answers the addition");
	error = kl_backend_session_add_key(backend, "secret", "a:b", "1234");
	check(error == EINVAL, "a label with a colon is EINVAL");
	error = kl_backend_session_remove_key(backend, "secret", "0123456789abcdef");
	check(error == 0 && read_line(ends[0], "REMOVE fido2 0123456789abcdef\nsecret\n"), "REMOVE fido2 with its reference written");
	(void)write(ends[0], "OK\n", 3U);
	kl_backend_tick(backend, 2000U);
	check(answer_count == 9U && answer_request == KL_BACKEND_SESSION_ENROLL && answer_error == 0, "OK answers the removal");
	error = kl_backend_session_remove_key(backend, "secret", "xyz");
	check(error == EINVAL, "a reference that is not one is EINVAL");
	error = kl_backend_session_cancel(backend);
	check(error == 0 && read_line(ends[0], "CANCEL\n"), "CANCEL written");
	answer_count = 1U;

	/* LOGOUT once, QUIT, then RELEASED after the stop. */
	error = kl_backend_session_logout(backend);
	check(error == 0 && read_line(ends[0], "LOGOUT\n"), "LOGOUT written");
	error = kl_backend_session_logout(backend);
	check(error == 0 && !read_line(ends[0], "LOGOUT\n"), "a second Log Out writes nothing");
	stop_peer = ends[0];
	released_before_stop = 0;
	(void)write(ends[0], "QUIT\n", 5U);
	kl_backend_tick(backend, 2001U);
	check(stop_count == 1U && stop_reason == KL_BACKEND_SESSION_QUIT, "QUIT ends the session");
	check(!released_before_stop && read_line(ends[0], "RELEASED\n"), "RELEASED comes after the stop");
	stop_peer = -1;

	/* sessiond closes: the session carries on, no longer managed. */
	(void)close(ends[0]);
	kl_backend_tick(backend, 2002U);
	check(stop_count == 1U && kl_backend_session_managed(backend) == 0, "sessiond gone: the session carries on unmanaged");
	error = kl_backend_session_logout(backend);
	check(error == ENOTSUP, "Log Out without sessiond ends at once (ENOTSUP)");
	kl_backend_close(backend);
}

/* A Log Out sessiond never answers ends the compositor after 30 s. */
static void
test_unanswered(
	void)
{
	struct kl_backend *backend;
	int ends[2];
	int error;

	/* sessiond's end (0) and the session's (1). */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, ends);
	if (error != 0)
		return;
	backend = open_backend(-1, ends[1]);
	if (backend == NULL)
		return;

	/* LOGOUT; the deadline starts at the first tick. */
	(void)kl_backend_session_logout(backend);
	kl_backend_tick(backend, 5000U);
	kl_backend_tick(backend, 34999U);
	check(stop_count == 0U, "no stop before 30 s");
	kl_backend_tick(backend, 35000U);
	check(stop_count == 1U && stop_reason == KL_BACKEND_SESSION_UNANSWERED, "unanswered after 30 s");
	kl_backend_close(backend);
	(void)close(ends[0]);
	(void)close(ends[1]);
}

/* Tells whether this host's user is root or a member of wheel (as the backend decides, power-zedbsd.c). */
static int
administrator(void)
{
	struct passwd *user;
	struct group *wheel;
	unsigned index;
	uid_t uid;
	int same;

	/* Root. */
	uid = getuid();
	if (uid == 0)
		return 1;

	/* The user and wheel, known. */
	user = getpwuid(uid);
	if (user == NULL)
		return 0;
	wheel = getgrnam("wheel");
	if (wheel == NULL)
		return 0;

	/* Wheel as its primary group. */
	if (wheel->gr_gid == user->pw_gid)
		return 1;

	/* Or a member by name. */
	for (index = 0U; wheel->gr_mem != NULL && wheel->gr_mem[index] != NULL; index++) {
		same = strcmp(wheel->gr_mem[index], user->pw_name);
		if (same == 0)
			return 1;
	}

	/* Not in it. */
	return 0;
}
