/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A session's admin actions: a program that a definition in
 * /etc/keiland/admin.d names (admin-rules.c) run as root once the person
 * at the screen has allowed it in the compositor's own dialog.  The
 * compositor asks on the session's socket, one line each, every one
 * starting with ADMIN:
 *
 *   ASK id client locale streams action [word...]
 *       asks for the action; answered with REFUSED, FAILED, STARTED (a
 *       permission kept), CONNECT (a permission kept, for a program of
 *       the desktop), or TITLE, MESSAGE and SHOW (the dialog)
 *   ALLOW id
 *       the person allowed it (or, after CONNECT, the connection comes);
 *       for a program of the desktop it carries one descriptor (the
 *       compositor's connection lent to the program); answered with
 *       STARTED or FAILED
 *   PASSWORD id name, then a secret line
 *       not made yet: FAILED, and the secret line is erased unread
 *   CANCEL id      the dialog went: the request is forgotten; no answer
 *   FORGET client  the client went: what it was allowed goes; no answer
 *
 * and sessiond answers:
 *
 *   ADMIN TITLE id text, ADMIN MESSAGE id text, ADMIN SHOW id consent [display]
 *   ADMIN CONNECT id
 *   ADMIN STARTED id bits     with the descriptors (SCM_RIGHTS): the
 *                             program's input (written), output and error
 *                             (read, the ones asked for) and its status
 *                             (read: one line, "EXIT n" or "SIGNAL n")
 *   ADMIN REFUSED id unknown|invalid|not-allowed|method|busy|locked
 *   ADMIN FAILED id errno
 *
 * Every request that waits for an answer (ASK, ALLOW, PASSWORD) gets
 * exactly one, so the compositor never waits for nothing.  One request
 * waits at a time, for two minutes at most.
 *
 * The program is started by a runner: sessiond forks a child that puts
 * the program's descriptors in place and closes every other, sets the
 * signals, the session and the limits back to their defaults, forks the
 * runner and ends; the runner (init's child, not sessiond's) forks the
 * program with a fixed environment, waits for it, and writes how it
 * ended to the status pipe.  The program is the file opened when the
 * request was asked (fexecve), so what runs is what the person was
 * shown.  The program stops when its input ends: the input is always a
 * pipe, closed when the application closes it or ends.
 *
 * The requests are read a byte at a time with recvmsg
 * (sessiond_read_request), so the descriptor an ALLOW lends comes with
 * its line and none is lost; any other line's descriptors are closed.
 *
 * A program of the desktop (display wayland: the installer) is given a
 * connection the compositor made for it as WAYLAND_SOCKET: a program
 * run as root never reaches the user's own socket, and the compositor
 * knows which of its clients runs as root.  Its language and theme come
 * through that connection as for every other client; the desktop's
 * locale is its LANG.
 */

#include "sessiond.h"
#include "admin-rules.h"
#include "../../base/common/account.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

/*
 * The root the definitions and programs are opened below, and the owner
 * every directory and file on the way must have.  The host tests name a
 * directory of their own and their own user.
 */
#ifndef SESSIOND_ADMIN_ROOT
#define SESSIOND_ADMIN_ROOT	""
#endif
#ifndef SESSIOND_ADMIN_OWNER
#define SESSIOND_ADMIN_OWNER	0
#endif

/* How long a request waits for the person's answer before it is forgotten, in milliseconds (a host test shortens it). */
#ifndef SESSIOND_ADMIN_WAIT_MS
#define SESSIOND_ADMIN_WAIT_MS	120000LL
#endif

/* The fixed environment's search path, and the program's home. */
#define ADMIN_PATH		"PATH=/bin:/sbin:/usr/bin:/usr/sbin"
#define ADMIN_HOME		"HOME=/root"

/*
 * Where the program's descriptors are in the runner: its input, output
 * and error at 0 to 2, the status pipe the runner writes at 3, the
 * connection lent to a program of the desktop at 4, and the program's
 * file at 5.  The child puts them there from copies above ADMIN_HIGH_FD.
 */
#define ADMIN_FD_STATUS		3
#define ADMIN_FD_DISPLAY	4
#define ADMIN_FD_PROGRAM	5
#define ADMIN_FD_COUNT		6
#define ADMIN_HIGH_FD		16

/* The longest line sessiond writes, and room for the dialog's three lines at once. */
#define ADMIN_REPLY_MAX		SESSIOND_LINE_MAX
#define ADMIN_DIALOG_MAX	1024U

/* The room of the program's environment: its strings, and its entries. */
#define ADMIN_ENVIRONMENT_MAX	1024U
#define ADMIN_ENVIRONMENT_COUNT	12U

/* The program and its words: the program's path first, the end last. */
#define ADMIN_ARGUMENTS_COUNT	(SESSIOND_ADMIN_WORDS_MAX + 2U)

/* What a waiting request waits for: the person's answer in the dialog, or only the compositor's connection. */
enum admin_wait {
	ADMIN_WAIT_CONSENT,
	ADMIN_WAIT_CONNECTION
};

/*
 * The request that waits for its answer: the compositor's number and
 * client, what it waits for, whether a kept permission let it through
 * (then nothing new is kept), until when, the streams and the words'
 * set, the locale, the program opened when it was asked, and the
 * definition and file as the person was shown them.
 */
struct admin_pending {
	int active;
	unsigned long id;
	unsigned long client;
	enum admin_wait wait;
	int kept;
	long long deadline_ms;
	unsigned streams;
	struct sessiond_admin_words words;
	char locale[SESSIOND_ADMIN_LOCALE_MAX + 1U];
	int program;
	struct sessiond_admin_identity identity;
	struct sessiond_admin_definition definition;
};

/*
 * The running session as the admin actions see it: its compositor's
 * socket (-1 when there is none), its user, its generation, whether its
 * lock screen shows, whether the next line is a secret to erase unread,
 * the permissions kept for its clients, and the request that waits.
 */
struct admin_session {
	int control;
	const struct sessiond_account *account;
	unsigned long long generation;
	int locked;
	int skip_secret;
	struct sessiond_admin_keep keep;
	struct admin_pending pending;
};

/*
 * The program's argument and environment strings as the child gets
 * them, built before the fork so that the child only copies descriptors
 * and runs the program.
 */
struct admin_launch {
	char *arguments[ADMIN_ARGUMENTS_COUNT];
	char words[SESSIOND_ADMIN_WORDS_MAX][SESSIOND_ADMIN_WORD_MAX + 1U];
	char program[SESSIOND_ADMIN_PATH_MAX];
	char *environment[ADMIN_ENVIRONMENT_COUNT];
	char strings[ADMIN_ENVIRONMENT_MAX];
	size_t used;
	unsigned count;
	char action[SESSIOND_ADMIN_NAME_MAX + 1U];
};

/*
 * The running session's admin state.  sessiond_admin_begin fills it when
 * a session starts and sessiond_admin_end empties it; one session runs
 * at a time, on sessiond's one thread.  Its account is NULL while no
 * session runs, and then nothing is answered.
 */
static struct admin_session admin_session;

/*
 * The generation of the last session begun.  It only grows, so that a
 * kept permission of a session that ended is never taken for one of the
 * session after it, whose compositor numbers its clients from 1 again.
 */
static unsigned long long admin_generation;

/* The place definitions and programs are opened from. */
static const struct sessiond_admin_place admin_place = { SESSIOND_ADMIN_ROOT, (uid_t)SESSIOND_ADMIN_OWNER };

static void admin_ask(const char *arguments);
static void admin_allow(const char *arguments, struct sessiond_fds *fds);
static void admin_password(const char *arguments);
static void admin_cancel(const char *arguments);
static void admin_forget(const char *arguments);
static void admin_cut(const char *arguments);
static void admin_show(void);
static void admin_start(int display);
static int admin_prepare(struct admin_launch *launch);
static int admin_environment_add(struct admin_launch *launch, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void admin_middle(const int *table, struct admin_launch *launch) __attribute__((noreturn));
static void admin_runner(struct admin_launch *launch, int display) __attribute__((noreturn));
static void admin_helper(struct admin_launch *launch) __attribute__((noreturn));
static int admin_send_started(unsigned long id, unsigned bits, const int *descriptors, unsigned count);
static void admin_pending_clear(void);
static void admin_reply(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void admin_close_pair(int *pair);
static const char *admin_verb(const char *arguments, const char *verb);
static void admin_fds_take(struct sessiond_fds *fds, struct msghdr *header);

/*
 * Begins the admin actions of a session: its compositor's socket, its
 * user, and whether it starts with the lock screen.  Nothing is kept
 * from a session before.
 */
void
sessiond_admin_begin(
	int control,
	const struct sessiond_account *account,
	int locked)
{
	/* What an earlier session left goes. */
	sessiond_admin_end();

	/* A new generation: no permission kept before reaches this session. */
	admin_generation++;
	admin_session.control = control;
	admin_session.account = account;
	admin_session.generation = admin_generation;
	admin_session.locked = locked;
	admin_session.skip_secret = 0;
	sessiond_admin_keep_clear(&admin_session.keep);
}

/*
 * Takes the line after ADMIN PASSWORD, which is a secret: it is erased
 * unread.  Returns 1 when the line was that secret, 0 for any other line.
 */
int
sessiond_admin_secret(
	char *line)
{
	size_t length;

	/* Only the line right after a PASSWORD. */
	if (!admin_session.skip_secret)
		return 0;
	admin_session.skip_secret = 0;

	/* Erased where it lies. */
	length = strlen(line);
	memset(line, 0, length);
	return 1;
}

/*
 * Answers one ADMIN request of the session (arguments: the line after
 * "ADMIN "; cut when the line was longer than any request and its end
 * was thrown away).  The descriptors that came with the line are taken
 * by an ALLOW that lends a connection, and closed otherwise.
 */
void
sessiond_admin_request(
	const char *arguments,
	int cut,
	struct sessiond_fds *fds)
{
	const char *rest;

	/* A line cut short is never carried out, but a request waiting for an answer gets one. */
	if (cut) {
		sessiond_fds_close(fds);
		admin_cut(arguments);
		return;
	}

	/* An action asked for. */
	rest = admin_verb(arguments, "ASK");
	if (rest != NULL) {
		sessiond_fds_close(fds);
		admin_ask(rest);
		return;
	}

	/* The person allowed it, or the connection came. */
	rest = admin_verb(arguments, "ALLOW");
	if (rest != NULL) {
		admin_allow(rest, fds);
		sessiond_fds_close(fds);
		return;
	}

	/* A password, not made yet. */
	rest = admin_verb(arguments, "PASSWORD");
	if (rest != NULL) {
		sessiond_fds_close(fds);
		admin_password(rest);
		return;
	}

	/* The dialog went. */
	rest = admin_verb(arguments, "CANCEL");
	if (rest != NULL) {
		sessiond_fds_close(fds);
		admin_cancel(rest);
		return;
	}

	/* A client went. */
	rest = admin_verb(arguments, "FORGET");
	if (rest != NULL) {
		sessiond_fds_close(fds);
		admin_forget(rest);
		return;
	}

	/* Anything else is not a request. */
	sessiond_fds_close(fds);
	sessiond_log("SESSIOND ADMIN unknown request");
	admin_reply("ERROR");
}

/*
 * Follows the session's lock screen: when it shows, every kept
 * permission and the request that waits go, and no request is taken
 * until it has gone.
 */
void
sessiond_admin_lock(
	int locked)
{
	/* What was allowed before the lock is not allowed after it. */
	admin_session.locked = locked;
	if (!locked)
		return;
	sessiond_admin_keep_clear(&admin_session.keep);
	if (admin_session.pending.active)
		sessiond_log("SESSIOND ADMIN locked id=%lu", admin_session.pending.id);
	admin_pending_clear();
}

/*
 * Runs out what waits: the request nobody answered within two minutes
 * (forgotten without a word: the compositor's dialog gave up first), and
 * the permissions kept for seconds that are over.
 */
void
sessiond_admin_tick(
	void)
{
	long long now_ms;

	/* The time now. */
	now_ms = sessiond_milliseconds();

	/* The request that waited too long. */
	if (admin_session.pending.active && now_ms >= admin_session.pending.deadline_ms) {
		sessiond_log("SESSIOND ADMIN expired id=%lu", admin_session.pending.id);
		admin_pending_clear();
	}

	/* The permissions whose time is over. */
	sessiond_admin_keep_expire(&admin_session.keep, now_ms);
}

/*
 * Ends the admin actions of a session: its socket closed or the session
 * ended.  Every kept permission and the request that waits go.
 */
void
sessiond_admin_end(
	void)
{
	/* Nothing of the session is left. */
	admin_pending_clear();
	sessiond_admin_keep_clear(&admin_session.keep);
	admin_session.control = -1;
	admin_session.account = NULL;
	admin_session.locked = 0;
	admin_session.skip_secret = 0;
}

/*
 * Writes the greeter's answer to an ADMIN request: the login screen runs
 * no action, so an ASK is refused and an ALLOW or PASSWORD fails, each
 * by its number; CANCEL and FORGET get no answer.  Returns 1 with the
 * answer (without its line end), or 0 when there is none to write.
 */
int
sessiond_admin_greeter_reply(
	const char *arguments,
	char *reply,
	size_t size)
{
	const char *rest;
	unsigned long id;
	int error;

	/* No answer for what never waits for one. */
	rest = admin_verb(arguments, "CANCEL");
	if (rest != NULL)
		return 0;
	rest = admin_verb(arguments, "FORGET");
	if (rest != NULL)
		return 0;

	/* An ASK is refused by its number. */
	rest = admin_verb(arguments, "ASK");
	if (rest != NULL) {
		error = sessiond_admin_parse_id(rest, &id, &rest);
		if (error == 0) {
			(void)snprintf(reply, size, "ADMIN REFUSED %lu not-allowed", id);
			return 1;
		}
	}

	/* An ALLOW or a PASSWORD fails by its number. */
	rest = admin_verb(arguments, "ALLOW");
	if (rest == NULL)
		rest = admin_verb(arguments, "PASSWORD");
	if (rest != NULL) {
		error = sessiond_admin_parse_id(rest, &id, &rest);
		if (error == 0) {
			(void)snprintf(reply, size, "ADMIN FAILED %lu %d", id, EPERM);
			return 1;
		}
	}

	/* Anything else, as any request the greeter does not know. */
	(void)snprintf(reply, size, "ERROR");
	return 1;
}

/*
 * Reads one line of a session's requests from its socket, a byte at a
 * time (nothing after it is taken), with the descriptors that came with
 * it (SCM_RIGHTS, closed on exec), waiting at most timeout_ms in all.
 * Returns 1 with the line (its line end removed; *cut set when it was
 * longer than the room and its end was thrown away), 0 when the time
 * ran out, or -1 at the end of the stream or on an error.  The
 * descriptors are the caller's in every case: it closes those it does
 * not take (sessiond_fds_close).
 */
int
sessiond_read_request(
	int descriptor,
	char *line,
	size_t size,
	int timeout_ms,
	struct sessiond_fds *fds,
	int *cut)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int) * SESSIOND_FDS_MAX)];
	} control;
	struct msghdr header;
	struct iovec vector;
	struct pollfd entry;
	long long deadline;
	long long left;
	size_t used;
	ssize_t count;
	char byte;
	int ready;

	/* Nothing yet. */
	memset(fds, 0, sizeof(*fds));
	*cut = 0;

	/* Byte after byte until the line ends. */
	deadline = sessiond_milliseconds() + timeout_ms;
	used = 0;
	for (;;) {
		/* The time left. */
		left = deadline - sessiond_milliseconds();
		if (left <= 0)
			return 0;

		/* A byte, or the end of the wait. */
		entry.fd = descriptor;
		entry.events = POLLIN;
		entry.revents = 0;
		ready = poll(&entry, 1, (int)left);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return -1;
		if (ready == 0)
			return 0;

		/* One byte, and the descriptors sent with it. */
		memset(&header, 0, sizeof(header));
		memset(&control, 0, sizeof(control));
		vector.iov_base = &byte;
		vector.iov_len = 1U;
		header.msg_iov = &vector;
		header.msg_iovlen = 1;
		header.msg_control = control.space;
		header.msg_controllen = sizeof(control.space);
		count = recvmsg(descriptor, &header, MSG_CMSG_CLOEXEC);
		if (count < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (count <= 0)
			return -1;
		admin_fds_take(fds, &header);

		/* The line ends, or grows while there is room. */
		if (byte == '\n')
			break;
		if (used + 1U < size) {
			line[used++] = byte;
		} else {
			*cut = 1;
		}
	}

	/* Succeeded: a whole line. */
	line[used] = '\0';
	return 1;
}

/*
 * Closes the descriptors that came with a line and were not taken.
 */
void
sessiond_fds_close(
	struct sessiond_fds *fds)
{
	unsigned index;

	/* Each one still held. */
	for (index = 0; index < fds->count; index++) {
		if (fds->descriptors[index] >= 0)
			(void)close(fds->descriptors[index]);
	}

	/* None left. */
	fds->count = 0;
}

/* Answers an ADMIN ASK: refused, started on a kept permission, or the dialog. */
static void
admin_ask(
	const char *arguments)
{
	struct sessiond_admin_ask ask;
	struct sessiond_admin_identity identity;
	struct admin_pending *pending;
	enum sessiond_admin_verdict verdict;
	const struct sessiond_account *account;
	unsigned extra;
	int in_wheel;
	int program;
	int kept;
	int error;

	/* The request, refused by its number when it can be read but not understood. */
	pending = &admin_session.pending;
	account = admin_session.account;
	error = sessiond_admin_parse_ask(arguments, &ask);
	if (error != 0) {
		sessiond_log("SESSIOND ADMIN refused id=%lu reason=invalid", ask.id);
		if (ask.id != 0UL)
			admin_reply("ADMIN REFUSED %lu invalid", ask.id);
		return;
	}
	syslog(LOG_NOTICE, "admin action %s asked by %s (uid %u) for client %lu", ask.action, account->passwd.pw_name, (unsigned)account->passwd.pw_uid, ask.client);

	/* One request waits at a time. */
	if (pending->active) {
		sessiond_log("SESSIOND ADMIN refused id=%lu reason=busy waiting=%lu", ask.id, pending->id);
		admin_reply("ADMIN REFUSED %lu busy", ask.id);
		return;
	}

	/* Nothing is asked while the lock screen shows. */
	if (admin_session.locked) {
		sessiond_log("SESSIOND ADMIN refused id=%lu reason=locked", ask.id);
		admin_reply("ADMIN REFUSED %lu locked", ask.id);
		return;
	}

	/* The action's definition, trusted from the root down. */
	error = sessiond_admin_load(&admin_place, ask.action, &pending->definition, &identity);
	if (error != 0) {
		sessiond_log("SESSIOND ADMIN refused id=%lu action=%s reason=unknown errno=%d", ask.id, ask.action, error);
		syslog(LOG_WARNING, "admin action %s refused: no usable definition (errno %d)", ask.action, error);
		admin_reply("ADMIN REFUSED %lu unknown", ask.id);
		return;
	}

	/* Words the definition lists exactly. */
	error = sessiond_admin_match(&pending->definition, &ask.words);
	if (error != 0) {
		sessiond_log("SESSIOND ADMIN refused id=%lu action=%s reason=unknown words=%u", ask.id, ask.action, ask.words.count);
		syslog(LOG_WARNING, "admin action %s refused: words it does not list", ask.action);
		admin_reply("ADMIN REFUSED %lu unknown", ask.id);
		return;
	}

	/* Streams it allows; the input is always there, as the program's life line. */
	extra = ask.streams & ~(pending->definition.streams | SESSIOND_ADMIN_INPUT);
	if (extra != 0U) {
		sessiond_log("SESSIOND ADMIN refused id=%lu action=%s reason=invalid streams=%x", ask.id, ask.action, ask.streams);
		admin_reply("ADMIN REFUSED %lu invalid", ask.id);
		return;
	}

	/* Who may run it, and how the person is asked. */
	in_wheel = account_in_wheel(account->passwd.pw_name, account->passwd.pw_gid);
	verdict = sessiond_admin_decide(&pending->definition, account->passwd.pw_uid, in_wheel);
	if (verdict == SESSIOND_ADMIN_VERDICT_NOT_ALLOWED) {
		sessiond_log("SESSIOND ADMIN refused id=%lu action=%s reason=not-allowed wheel=%d", ask.id, ask.action, in_wheel);
		syslog(LOG_WARNING, "admin action %s refused for %s: not allowed", ask.action, account->passwd.pw_name);
		admin_reply("ADMIN REFUSED %lu not-allowed", ask.id);
		return;
	}

	/* The passwords are not made yet: never a looser way in their place. */
	if (verdict != SESSIOND_ADMIN_VERDICT_CONSENT) {
		sessiond_log("SESSIOND ADMIN refused id=%lu action=%s reason=method", ask.id, ask.action);
		admin_reply("ADMIN REFUSED %lu method", ask.id);
		return;
	}

	/* The program, opened now: what runs is what the person is shown. */
	error = sessiond_admin_open_program(&admin_place, &pending->definition, &program);
	if (error != 0) {
		sessiond_log("SESSIOND ADMIN failed id=%lu action=%s program=%s errno=%d", ask.id, ask.action, pending->definition.program, error);
		syslog(LOG_WARNING, "admin action %s: its program %s cannot be used (errno %d)", ask.action, pending->definition.program, error);
		admin_reply("ADMIN FAILED %lu %d", ask.id, error);
		return;
	}

	/* The request waits from now. */
	pending->active = 1;
	pending->id = ask.id;
	pending->client = ask.client;
	pending->deadline_ms = sessiond_milliseconds() + SESSIOND_ADMIN_WAIT_MS;
	pending->streams = ask.streams | SESSIOND_ADMIN_INPUT;
	pending->words = ask.words;
	(void)snprintf(pending->locale, sizeof(pending->locale), "%s", ask.locale);
	pending->program = program;
	pending->identity = identity;

	/* A permission kept for the client: no dialog. */
	kept = sessiond_admin_keep_find(&admin_session.keep, admin_session.generation, ask.client, ask.action, &identity, sessiond_milliseconds());
	pending->kept = kept;
	if (kept) {
		syslog(LOG_NOTICE, "admin action %s allowed for %s: kept", ask.action, account->passwd.pw_name);

		/* A program of the desktop waits for the compositor's connection first. */
		if (pending->definition.display) {
			pending->wait = ADMIN_WAIT_CONNECTION;
			sessiond_log("SESSIOND ADMIN connect id=%lu action=%s via=keep", ask.id, ask.action);
			admin_reply("ADMIN CONNECT %lu", ask.id);
			return;
		}

		/* Any other starts now. */
		sessiond_log("SESSIOND ADMIN allow id=%lu action=%s via=keep", ask.id, ask.action);
		admin_start(-1);
		return;
	}

	/* The dialog asks the person. */
	pending->wait = ADMIN_WAIT_CONSENT;
	admin_show();
}

/* Answers an ADMIN ALLOW: the waiting request starts, with the connection it is lent. */
static void
admin_allow(
	const char *arguments,
	struct sessiond_fds *fds)
{
	struct admin_pending *pending;
	const char *rest;
	unsigned long id;
	int display;
	int error;

	/* The request's number, alone on the line. */
	pending = &admin_session.pending;
	error = sessiond_admin_parse_id(arguments, &id, &rest);
	if (error != 0 || *rest != '\0') {
		sessiond_log("SESSIOND ADMIN allow unreadable");
		return;
	}

	/* Only the request that waits: never a program started twice, or after it was forgotten. */
	if (!pending->active || pending->id != id) {
		sessiond_log("SESSIOND ADMIN failed id=%lu reason=stale", id);
		admin_reply("ADMIN FAILED %lu %d", id, ESTALE);
		return;
	}

	/* A program of the desktop comes with exactly one connection, any other with none. */
	display = -1;
	if (pending->definition.display) {
		if (fds->count != 1U || fds->lost) {
			sessiond_log("SESSIOND ADMIN failed id=%lu reason=connection count=%u lost=%d", id, fds->count, fds->lost);
			admin_reply("ADMIN FAILED %lu %d", id, EINVAL);
			admin_pending_clear();
			return;
		}
		display = fds->descriptors[0];
		fds->descriptors[0] = -1;
	} else if (fds->count != 0U || fds->lost) {
		sessiond_log("SESSIOND ADMIN failed id=%lu reason=descriptors count=%u", id, fds->count);
		admin_reply("ADMIN FAILED %lu %d", id, EINVAL);
		admin_pending_clear();
		return;
	}

	/* Started; a permission the person gave is kept as the definition says. */
	if (pending->kept) {
		sessiond_log("SESSIOND ADMIN allow id=%lu action=%s via=keep", id, pending->definition.name);
	} else {
		sessiond_log("SESSIOND ADMIN allow id=%lu action=%s via=dialog", id, pending->definition.name);
		syslog(LOG_NOTICE, "admin action %s allowed by %s", pending->definition.name, admin_session.account->passwd.pw_name);
	}
	admin_start(display);
	if (display >= 0)
		(void)close(display);
}

/* Answers an ADMIN PASSWORD: no action asks for a password yet; its secret line is erased unread. */
static void
admin_password(
	const char *arguments)
{
	const char *rest;
	unsigned long id;
	int error;

	/* The secret's line comes next, whatever this one says. */
	admin_session.skip_secret = 1;

	/* Failed by its number. */
	error = sessiond_admin_parse_id(arguments, &id, &rest);
	if (error != 0) {
		sessiond_log("SESSIOND ADMIN password unreadable");
		return;
	}
	sessiond_log("SESSIOND ADMIN failed id=%lu reason=password", id);
	admin_reply("ADMIN FAILED %lu %d", id, EPERM);
}

/* Answers an ADMIN CANCEL: the waiting request with the number is forgotten. */
static void
admin_cancel(
	const char *arguments)
{
	const char *rest;
	unsigned long id;
	int error;

	/* The number. */
	error = sessiond_admin_parse_id(arguments, &id, &rest);
	if (error != 0 || *rest != '\0') {
		sessiond_log("SESSIOND ADMIN cancel unreadable");
		return;
	}

	/* Only the one that waits. */
	if (!admin_session.pending.active || admin_session.pending.id != id) {
		sessiond_log("SESSIOND ADMIN cancel id=%lu not-waiting", id);
		return;
	}
	sessiond_log("SESSIOND ADMIN cancel id=%lu action=%s", id, admin_session.pending.definition.name);
	admin_pending_clear();
}

/* Answers an ADMIN FORGET: a client went, with what it was allowed and what it asked. */
static void
admin_forget(
	const char *arguments)
{
	unsigned long client;
	char *end;

	/* The client's number, all of the rest. */
	errno = 0;
	client = strtoul(arguments, &end, 10);
	if (errno != 0 ||
	    end == arguments ||
	    *end != '\0') {
		sessiond_log("SESSIOND ADMIN forget unreadable");
		return;
	}

	/* Its permissions, and its request if it waits. */
	sessiond_admin_keep_forget(&admin_session.keep, client);
	if (admin_session.pending.active && admin_session.pending.client == client)
		admin_pending_clear();
	sessiond_log("SESSIOND ADMIN forget client=%lu", client);
}

/* Answers a request whose line was cut: an ASK is refused by its number, if it can be read. */
static void
admin_cut(
	const char *arguments)
{
	const char *rest;
	unsigned long id;
	int error;

	/* A PASSWORD's secret still follows. */
	rest = admin_verb(arguments, "PASSWORD");
	if (rest != NULL)
		admin_session.skip_secret = 1;

	/* An ASK: refused as invalid. */
	rest = admin_verb(arguments, "ASK");
	if (rest != NULL) {
		error = sessiond_admin_parse_id(rest, &id, &rest);
		if (error == 0 && *rest == ' ') {
			sessiond_log("SESSIOND ADMIN refused id=%lu reason=invalid cut", id);
			admin_reply("ADMIN REFUSED %lu invalid", id);
			return;
		}
	}

	/* Anything else is only noted. */
	sessiond_log("SESSIOND ADMIN cut line");
}

/* Sends the dialog: the title, the message, and SHOW last, in one write. */
static void
admin_show(
	void)
{
	char lines[ADMIN_DIALOG_MAX];
	struct admin_pending *pending;
	const char *title;
	const char *message;
	const char *display;
	ssize_t written;
	int length;

	/* The texts in the desktop's language. */
	pending = &admin_session.pending;
	title = sessiond_admin_text(pending->definition.titles, pending->definition.title_count, pending->locale);
	message = sessiond_admin_text(pending->definition.messages, pending->definition.message_count, pending->locale);
	display = "";
	if (pending->definition.display)
		display = " display";

	/* The three lines; the definition's limits keep each under a line's room. */
	length = snprintf(lines, sizeof(lines), "ADMIN TITLE %lu %s\nADMIN MESSAGE %lu %s\nADMIN SHOW %lu consent%s\n",
	    pending->id, title, pending->id, message, pending->id, display);
	if (length < 0 || (size_t)length >= sizeof(lines)) {
		sessiond_log("SESSIOND ADMIN failed id=%lu reason=dialog", pending->id);
		admin_reply("ADMIN FAILED %lu %d", pending->id, EOVERFLOW);
		admin_pending_clear();
		return;
	}

	/* Written whole. */
	written = write(admin_session.control, lines, (size_t)length);
	if (written != (ssize_t)length)
		sessiond_log("SESSIOND ADMIN show lost id=%lu errno=%d", pending->id, errno);
	sessiond_log("SESSIOND ADMIN show id=%lu action=%s how=consent display=%d", pending->id, pending->definition.name, pending->definition.display);
}

/*
 * Starts the waiting request's program through the runner and sends
 * STARTED with its streams, or FAILED.  The request no longer waits
 * after it; a permission the dialog gave is kept as the definition
 * says.  display is the connection lent to a program of the desktop
 * (the caller closes it), or -1.
 */
static void
admin_start(
	int display)
{
	struct admin_launch launch;
	struct admin_pending *pending;
	int input[2] = { -1, -1 };
	int output[2] = { -1, -1 };
	int error_pipe[2] = { -1, -1 };
	int status_pipe[2] = { -1, -1 };
	int table[ADMIN_FD_COUNT];
	int sent[SESSIOND_FDS_MAX];
	unsigned bits;
	unsigned count;
	pid_t middle;
	pid_t waited;
	int status;
	int failed;
	int error;

	/* The program's arguments and environment, before any process is made. */
	pending = &admin_session.pending;
	error = admin_prepare(&launch);
	if (error != 0) {
		admin_reply("ADMIN FAILED %lu %d", pending->id, error);
		admin_pending_clear();
		return;
	}

	/* The input always (the program's life line), the status always, output and error when asked for. */
	failed = pipe2(input, O_CLOEXEC);
	if (failed == 0)
		failed = pipe2(status_pipe, O_CLOEXEC);
	if (failed == 0 && (pending->streams & SESSIOND_ADMIN_OUTPUT) != 0U)
		failed = pipe2(output, O_CLOEXEC);
	if (failed == 0 && (pending->streams & SESSIOND_ADMIN_ERROR) != 0U)
		failed = pipe2(error_pipe, O_CLOEXEC);
	if (failed != 0) {
		error = errno;
		sessiond_log("SESSIOND ADMIN failed id=%lu reason=pipe errno=%d", pending->id, error);
		admin_close_pair(input);
		admin_close_pair(output);
		admin_close_pair(error_pipe);
		admin_close_pair(status_pipe);
		admin_reply("ADMIN FAILED %lu %d", pending->id, error);
		admin_pending_clear();
		return;
	}

	/* The program's side: its streams (-1: /dev/null), the status, the connection, the program. */
	table[0] = input[0];
	table[1] = output[1];
	table[2] = error_pipe[1];
	table[ADMIN_FD_STATUS] = status_pipe[1];
	table[ADMIN_FD_DISPLAY] = display;
	table[ADMIN_FD_PROGRAM] = pending->program;

	/* The child that puts them in place and leaves the runner to init. */
	middle = fork();
	if (middle == 0)
		admin_middle(table, &launch);

	/* sessiond keeps only the application's side. */
	(void)close(input[0]);
	input[0] = -1;
	(void)close(status_pipe[1]);
	status_pipe[1] = -1;
	if (output[1] >= 0) {
		(void)close(output[1]);
		output[1] = -1;
	}
	if (error_pipe[1] >= 0) {
		(void)close(error_pipe[1]);
		error_pipe[1] = -1;
	}

	/* The child ends at once once the runner is on its own; a child that could not be made is a failure. */
	status = 0;
	failed = 0;
	if (middle < 0)
		failed = 1;
	if (!failed) {
		do {
			waited = waitpid(middle, &status, 0);
		} while (waited < 0 && errno == EINTR);

		/* Only a child that ended by itself with 0 left a runner behind. */
		if (waited != middle) {
			failed = 1;
		} else if (!WIFEXITED(status)) {
			failed = 1;
		} else if (WEXITSTATUS(status) != 0) {
			failed = 1;
		}
	}
	if (failed) {
		sessiond_log("SESSIOND ADMIN failed id=%lu action=%s reason=runner status=%d", pending->id, launch.action, status);
		admin_close_pair(input);
		admin_close_pair(output);
		admin_close_pair(error_pipe);
		admin_close_pair(status_pipe);
		admin_reply("ADMIN FAILED %lu %d", pending->id, EAGAIN);
		admin_pending_clear();
		return;
	}

	/* The application's side, in the order of the bits: input, output, error, status. */
	count = 0;
	bits = 0;
	sent[count++] = input[1];
	bits |= SESSIOND_ADMIN_INPUT;
	if (output[0] >= 0) {
		sent[count++] = output[0];
		bits |= SESSIOND_ADMIN_OUTPUT;
	}
	if (error_pipe[0] >= 0) {
		sent[count++] = error_pipe[0];
		bits |= SESSIOND_ADMIN_ERROR;
	}
	sent[count++] = status_pipe[0];
	bits |= SESSIOND_ADMIN_STATUS;

	/* Sent; a compositor that does not take them leaves the program to see its input end. */
	error = admin_send_started(pending->id, bits, sent, count);
	sessiond_log("SESSIOND ADMIN started id=%lu action=%s bits=%x sent=%d", pending->id, launch.action, bits, error == 0);
	syslog(LOG_NOTICE, "admin action %s started for %s", launch.action, admin_session.account->passwd.pw_name);
	admin_close_pair(input);
	admin_close_pair(output);
	admin_close_pair(error_pipe);
	admin_close_pair(status_pipe);

	/* A permission the dialog gave is kept for the client as the definition says. */
	if (!pending->kept) {
		sessiond_admin_keep_add(&admin_session.keep, admin_session.generation, pending->client, &pending->definition,
		    &pending->identity, sessiond_milliseconds());
	}
	admin_pending_clear();
}

/*
 * Builds the program's arguments (its path, then the words) and its
 * fixed environment.  Returns 0, or EOVERFLOW when they do not fit.
 */
static int
admin_prepare(
	struct admin_launch *launch)
{
	const struct admin_pending *pending;
	const struct sessiond_account *account;
	unsigned index;
	int error;

	/* The program's path first, then the words, then the end. */
	pending = &admin_session.pending;
	account = admin_session.account;
	memset(launch, 0, sizeof(*launch));
	(void)snprintf(launch->program, sizeof(launch->program), "%s", pending->definition.program);
	(void)snprintf(launch->action, sizeof(launch->action), "%s", pending->definition.name);
	launch->arguments[0] = launch->program;
	for (index = 0; index < pending->words.count; index++) {
		(void)snprintf(launch->words[index], sizeof(launch->words[index]), "%s", pending->words.words[index]);
		launch->arguments[index + 1U] = launch->words[index];
	}
	launch->arguments[pending->words.count + 1U] = NULL;

	/* The fixed environment: nothing of the session's own. */
	error = admin_environment_add(launch, "%s", ADMIN_PATH);
	if (error == 0)
		error = admin_environment_add(launch, "%s", ADMIN_HOME);
	if (error == 0)
		error = admin_environment_add(launch, "USER=root");
	if (error == 0)
		error = admin_environment_add(launch, "LOGNAME=root");
	if (error == 0)
		error = admin_environment_add(launch, "SHELL=/bin/sh");

	/* Who asked, and for what: the program may check its caller. */
	if (error == 0)
		error = admin_environment_add(launch, "KEILAND_ADMIN_ACTION=%s", pending->definition.name);
	if (error == 0)
		error = admin_environment_add(launch, "KEILAND_ADMIN_UID=%u", (unsigned)account->passwd.pw_uid);
	if (error == 0)
		error = admin_environment_add(launch, "KEILAND_ADMIN_USER=%s", account->passwd.pw_name);

	/* A program of the desktop: the connection lent to it, and the desktop's locale. */
	if (error == 0 && pending->definition.display)
		error = admin_environment_add(launch, "WAYLAND_SOCKET=%d", ADMIN_FD_DISPLAY);
	if (error == 0 && pending->definition.display && pending->locale[0] != '\0')
		error = admin_environment_add(launch, "LANG=%s", pending->locale);
	if (error != 0)
		return error;

	/* Succeeded: the arguments and the environment are ready. */
	launch->environment[launch->count] = NULL;
	return 0;
}

/* Adds one entry to the program's environment; returns 0 or EOVERFLOW. */
static int
admin_environment_add(
	struct admin_launch *launch,
	const char *format,
	...)
{
	va_list arguments;
	size_t room;
	int length;

	/* Room for one more entry and the end. */
	if (launch->count + 1U >= ADMIN_ENVIRONMENT_COUNT)
		return EOVERFLOW;

	/* The entry, in the strings' room. */
	room = sizeof(launch->strings) - launch->used;
	va_start(arguments, format);
	length = vsnprintf(launch->strings + launch->used, room, format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= room)
		return EOVERFLOW;

	/* Succeeded: the entry is in place. */
	launch->environment[launch->count] = launch->strings + launch->used;
	launch->count++;
	launch->used += (size_t)length + 1U;
	return 0;
}

/*
 * The child between sessiond and the runner: puts the program's
 * descriptors at 0 to 5 and closes every other (the session's sockets,
 * the seat's, passkey's, and the application's side of the pipes),
 * leaves sessiond's session, signals and mask behind, forks the runner,
 * and ends.  Never returns.
 */
static void
admin_middle(
	const int *table,
	struct admin_launch *launch)
{
	int high[ADMIN_FD_COUNT];
	sigset_t empty;
	pid_t runner;
	int signal_number;
	int display;
	int null;
	int index;
	int moved;

	/* The authentication log's socket goes with the others; the runner opens its own. */
	closelog();

	/* What a stream not asked for reads and writes. */
	null = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (null < 0)
		_exit(1);

	/* Copies above the places, so that putting one in place never closes another. */
	for (index = 0; index < ADMIN_FD_COUNT; index++) {
		high[index] = -1;
		if (table[index] < 0 && index > 2)
			continue;
		if (table[index] < 0) {
			high[index] = fcntl(null, F_DUPFD_CLOEXEC, ADMIN_HIGH_FD);
		} else {
			high[index] = fcntl(table[index], F_DUPFD_CLOEXEC, ADMIN_HIGH_FD);
		}
		if (high[index] < 0)
			_exit(1);
	}

	/* Each in its place; dup2 leaves the place open across exec. */
	for (index = 0; index < ADMIN_FD_COUNT; index++) {
		if (high[index] < 0) {
			(void)close(index);
			continue;
		}
		moved = dup2(high[index], index);
		if (moved != index)
			_exit(1);
	}

	/* The status and the program's file are the runner's: the program does not inherit them. */
	(void)fcntl(ADMIN_FD_STATUS, F_SETFD, FD_CLOEXEC);
	(void)fcntl(ADMIN_FD_PROGRAM, F_SETFD, FD_CLOEXEC);

	/* Every other descriptor goes. */
	(void)closefrom(ADMIN_FD_COUNT);

	/* Its own session, no signal blocked, every signal as by default (SIGPIPE ignored by sessiond too). */
	(void)setsid();
	sigemptyset(&empty);
	(void)sigprocmask(SIG_SETMASK, &empty, NULL);
	for (signal_number = 1; signal_number < NSIG; signal_number++) {
		if (signal_number == SIGKILL || signal_number == SIGSTOP)
			continue;
		(void)signal(signal_number, SIG_DFL);
	}

	/* The runner, left to init once this child ends. */
	display = table[ADMIN_FD_DISPLAY] >= 0;
	runner = fork();
	if (runner < 0)
		_exit(1);
	if (runner == 0)
		admin_runner(launch, display);
	_exit(0);
}

/*
 * The runner: starts the program, waits for it, and writes how it ended
 * to the status pipe ("EXIT n" or "SIGNAL n"), noting both in the
 * authentication log.  Never returns.
 */
static void
admin_runner(
	struct admin_launch *launch,
	int display)
{
	char line[32];
	pid_t helper;
	pid_t waited;
	int status;
	int length;

	/* The program. */
	helper = fork();
	if (helper == 0)
		admin_helper(launch);

	/* The program's streams and connection are the program's alone: their ends are seen when it ends. */
	(void)close(0);
	(void)close(1);
	(void)close(2);
	if (display)
		(void)close(ADMIN_FD_DISPLAY);

	/* The log, opened after the fork so that the program never holds it. */
	openlog("sessiond", LOG_PID, LOG_AUTH);
	if (helper < 0) {
		syslog(LOG_ERR, "admin action %s: the program could not be started", launch->action);
		_exit(1);
	}
	syslog(LOG_NOTICE, "admin action %s: %s runs as pid %ld", launch->action, launch->program, (long)helper);

	/* Its end. */
	status = 0;
	do {
		waited = waitpid(helper, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != helper)
		_exit(1);

	/* How it ended, for the application. */
	length = 0;
	if (WIFEXITED(status))
		length = snprintf(line, sizeof(line), "EXIT %d\n", WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		length = snprintf(line, sizeof(line), "SIGNAL %d\n", WTERMSIG(status));
	if (length > 0 && (size_t)length < sizeof(line))
		(void)write(ADMIN_FD_STATUS, line, (size_t)length);
	syslog(LOG_NOTICE, "admin action %s: pid %ld ended (status %d)", launch->action, (long)helper, status);
	_exit(0);
}

/*
 * The program's process: the file mask, the directory, the limits and
 * the priority as for a program init starts, then the program opened
 * when the request was asked.  Never returns.
 */
static void
admin_helper(
	struct admin_launch *launch)
{
	struct rlimit limit;

	/* The defaults of a program the system starts. */
	(void)umask(022);
	(void)chdir("/");
	limit.rlim_cur = 0;
	limit.rlim_max = 0;
	(void)setrlimit(RLIMIT_CORE, &limit);
	limit.rlim_cur = RLIM_INFINITY;
	limit.rlim_max = RLIM_INFINITY;
	(void)setrlimit(RLIMIT_FSIZE, &limit);
	(void)setrlimit(RLIMIT_CPU, &limit);
	(void)setpriority(PRIO_PROCESS, 0, 0);

	/* The program the person was shown. */
	(void)fexecve(ADMIN_FD_PROGRAM, launch->arguments, launch->environment);
	_exit(127);
}

/* Sends ADMIN STARTED with the descriptors in one message, never waiting; returns 0 or an errno value. */
static int
admin_send_started(
	unsigned long id,
	unsigned bits,
	const int *descriptors,
	unsigned count)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int) * SESSIOND_FDS_MAX)];
	} control;
	char line[ADMIN_REPLY_MAX];
	struct cmsghdr *message;
	struct msghdr header;
	struct iovec vector;
	ssize_t sent;
	int length;

	/* The line. */
	length = snprintf(line, sizeof(line), "ADMIN STARTED %lu %x\n", id, bits);
	if (length < 0 || (size_t)length >= sizeof(line))
		return EOVERFLOW;

	/* The descriptors ride with its first byte. */
	memset(&header, 0, sizeof(header));
	memset(&control, 0, sizeof(control));
	vector.iov_base = line;
	vector.iov_len = (size_t)length;
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.space;
	header.msg_controllen = CMSG_SPACE(sizeof(int) * count);
	message = CMSG_FIRSTHDR(&header);
	message->cmsg_level = SOL_SOCKET;
	message->cmsg_type = SCM_RIGHTS;
	message->cmsg_len = CMSG_LEN(sizeof(int) * count);
	memcpy(CMSG_DATA(message), descriptors, sizeof(int) * count);

	/* Sent whole, or not at all: a compositor that does not read is not waited for. */
	sent = sendmsg(admin_session.control, &header, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (sent != (ssize_t)length) {
		sessiond_log("SESSIOND ADMIN started lost id=%lu errno=%d", id, errno);
		return EIO;
	}

	/* Succeeded: the compositor has them. */
	return 0;
}

/* Forgets the waiting request, closing the program it opened. */
static void
admin_pending_clear(
	void)
{
	/* The program's file is not needed any more. */
	if (admin_session.pending.active && admin_session.pending.program >= 0)
		(void)close(admin_session.pending.program);

	/* Nothing waits; -1 is no program. */
	memset(&admin_session.pending, 0, sizeof(admin_session.pending));
	admin_session.pending.program = -1;
}

/* Writes one answer line to the session's compositor. */
static void
admin_reply(
	const char *format,
	...)
{
	char line[ADMIN_REPLY_MAX];
	va_list arguments;
	ssize_t written;
	int length;

	/* No session's compositor to answer. */
	if (admin_session.account == NULL)
		return;

	/* The answer and its line end. */
	va_start(arguments, format);
	length = vsnprintf(line, sizeof(line) - 1U, format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= sizeof(line) - 1U)
		return;
	line[length] = '\n';
	length++;

	/* In one write. */
	written = write(admin_session.control, line, (size_t)length);
	if (written != (ssize_t)length)
		sessiond_log("SESSIOND ADMIN reply lost errno=%d", errno);
}

/* Closes both ends of a pipe that are still open. */
static void
admin_close_pair(
	int *pair)
{
	/* The read end. */
	if (pair[0] >= 0)
		(void)close(pair[0]);
	pair[0] = -1;

	/* The write end. */
	if (pair[1] >= 0)
		(void)close(pair[1]);
	pair[1] = -1;
}

/* Reports what follows a request's verb and its space (the verb alone gives ""), or NULL for another verb. */
static const char *
admin_verb(
	const char *arguments,
	const char *verb)
{
	size_t length;
	int differs;

	/* The verb itself. */
	length = strlen(verb);
	differs = strncmp(arguments, verb, length);
	if (differs != 0)
		return NULL;

	/* Then the end, or a space and the rest. */
	if (arguments[length] == '\0')
		return arguments + length;
	if (arguments[length] != ' ')
		return NULL;

	/* Succeeded: the rest after the space. */
	return arguments + length + 1U;
}

/* Keeps the descriptors a message carried, closing those there is no room for. */
static void
admin_fds_take(
	struct sessiond_fds *fds,
	struct msghdr *header)
{
	struct cmsghdr *message;
	unsigned char *data;
	size_t length;
	size_t index;
	int received;

	/* The kernel cut what was sent: some descriptors are gone. */
	if ((header->msg_flags & MSG_CTRUNC) != 0)
		fds->lost = 1;

	/* Each SCM_RIGHTS block, each descriptor in it. */
	for (message = CMSG_FIRSTHDR(header);
	     message != NULL;
	     message = CMSG_NXTHDR(header, message)) {
		if (message->cmsg_level != SOL_SOCKET || message->cmsg_type != SCM_RIGHTS)
			continue;
		data = CMSG_DATA(message);
		length = message->cmsg_len - (size_t)(data - (unsigned char *)message);
		for (index = 0; index + sizeof(int) <= length; index += sizeof(int)) {
			memcpy(&received, data + index, sizeof(int));

			/* Kept while there is room, else closed and noted. */
			if (fds->count < SESSIOND_FDS_MAX) {
				fds->descriptors[fds->count] = received;
				fds->count++;
			} else {
				(void)close(received);
				fds->lost = 1;
			}
		}
	}
}
