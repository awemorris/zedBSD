/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The greeter: sessiond starts it as _greeter with one end of a socket
 * pair as descriptor 3, and answers its requests, one line each:
 *
 *   READY                  the greeter is about to take the display; GO
 *                          (ws035-p101: nothing else holds it then)
 *   STYLES name            the ways the account can log in now
 *   AUTH name style        then the secret on the next line: /sbin/passkey
 *                          checks it (auth.c, ws172-p002); TOUCH while a
 *                          security key waits, then OK, or FAIL reason
 *                          after a delay that grows with the failures
 *   CANCEL                 stops a security key attempt
 *   POWER poweroff|reboot  ends the machine; OK
 *   POWER suspend          sleeps it (sleep.c, ws052-p011): SLEPT woke=…,
 *                          NOSLEEP …, ERROR busy or ERROR when it is done
 *   POWER cancel           stops a sleep before the kernel is asked; no answer
 *
 * Anything else is answered ERROR.  The secret is erased as soon as it is
 * written to passkey, and never written anywhere else.  sessiond's loop
 * keeps serving the greeter while passkey works.
 *
 * After OK the greeter stays on the screen ("Starting session") while the
 * session starts; once the session is ready to take the display sessiond
 * shuts its side of the socket down, the greeter gives the display back,
 * says RELEASED and ends (ws035-p101), so the text console never shows
 * between the two.  At a Log Out a greeter is started while the session
 * still shows; its READY is answered only once the session has given the
 * display back (session.c).
 */

#include "auth.h"
#include "power-rules.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* The greeter's output. */
#define GREETER_LOG_PATH	"/var/log/greeter.log"

/* How much of the end of the greeter's log is searched for its last word (KWL EXIT) after a failure. */
#define GREETER_REASON_BYTES	8192

/* A greeter that ends with an error within this many seconds of its start has failed. */
#define GREETER_QUICK_SECONDS	20

/* How long a greeter may take to end after a login before it is killed. */
#define GREETER_END_SECONDS	10

/* How long a greeter may take to say READY, and to give the display back (milliseconds). */
#define GREETER_READY_MS	30000
#define GREETER_RELEASE_MS	5000

/* How often a new input device is given to the greeter (milliseconds). */
#define GREETER_SEAT_MS		1000LL

/*
 * One running greeter: its process, sessiond's end of the socket, the
 * request that has come in so far, and the login attempt under way.
 */
struct greeter {
	pid_t pid;
	int socket;
	time_t started;
	char line[SESSIOND_LINE_MAX];
	size_t used;
	int logged_in;
	struct sessiond_exchange exchange;
};

static int greeter_account_find(struct sessiond_account *greeter_account);
static int greeter_start(struct sessiond *daemon, struct greeter *greeter, struct sessiond_account *greeter_account);
static void greeter_child(struct sessiond *daemon, int socket, struct sessiond_account *greeter_account);
static int greeter_read(struct greeter *greeter);
static void greeter_request(struct greeter *greeter, char *line);
static void greeter_power(struct greeter *greeter, const char *what);
static void greeter_reply(struct greeter *greeter, const char *reply);
static enum sessiond_greeter_end greeter_wait(struct greeter *greeter);
static void greeter_kill(struct greeter *greeter);
static void greeter_reason(void);

/*
 * Runs one greeter until a user logs in, it ends, or sessiond stops.
 *
 * On SESSIOND_GREETER_LOGIN account holds the user who logged in.
 */
enum sessiond_greeter_end
sessiond_greeter_run(
	struct sessiond *daemon,
	struct sessiond_account *account)
{
	struct sessiond_account greeter_account;
	struct greeter greeter;
	struct pollfd poll_entry[4];
	enum sessiond_greeter_end end;
	long long seat_ms;
	long long now_ms;
	nfds_t entries;
	nfds_t sleep_slot;
	nfds_t seat_slot;
	int timeout;
	int busy;
	int ready;
	int error;
	int closed;
	int plugged;

	/* The greeter a Log Out started, or a new one. */
	memset(&greeter, 0, sizeof(greeter));
	greeter.socket = -1;
	if (daemon->greeter_pid > 0) {
		greeter.pid = daemon->greeter_pid;
		greeter.socket = daemon->greeter_socket;
		greeter.started = daemon->greeter_started;
		daemon->greeter_pid = 0;
		daemon->greeter_socket = -1;
		error = greeter_account_find(&greeter_account);
		sessiond_log("SESSIOND GREETER adopt pid=%ld", (long)greeter.pid);
	} else {
		error = greeter_start(daemon, &greeter, &greeter_account);
	}

	/* Without its account the greeter cannot be looked after. */
	if (error != 0)
		return SESSIOND_GREETER_FAILED;
	sessiond_exchange_init(&greeter.exchange, greeter.socket, NULL, account);

	/* Its requests, until it logs a user in or goes. */
	closed = 0;
	seat_ms = sessiond_milliseconds();
	while (!closed) {
		/* sessiond is being stopped: the greeter (and an attempt under way) goes first. */
		if (sessiond_stopping) {
			sessiond_exchange_stop(&greeter.exchange);
			greeter_kill(&greeter);
			return SESSIOND_GREETER_STOP;
		}

		/* A request, passkey's answer, or the time to look at the attempt or the seat again. */
		poll_entry[0].fd = greeter.socket;
		poll_entry[0].events = POLLIN;
		poll_entry[0].revents = 0;
		entries = 1;
		poll_entry[1].fd = sessiond_exchange_fd(&greeter.exchange);
		poll_entry[1].events = POLLIN;
		poll_entry[1].revents = 0;
		if (poll_entry[1].fd >= 0)
			entries = 2;

		/* A sleep's answer, whoever asked it (sleep.c, ws052-p011). */
		sleep_slot = entries;
		poll_entry[sleep_slot].fd = sessiond_sleep_fd();
		poll_entry[sleep_slot].events = POLLIN;
		poll_entry[sleep_slot].revents = 0;
		if (poll_entry[sleep_slot].fd >= 0)
			entries++;

		/* A device plugged in (seat.c, BUG-264). */
		seat_slot = entries;
		poll_entry[seat_slot].fd = sessiond_seat_events_fd();
		poll_entry[seat_slot].events = POLLIN;
		poll_entry[seat_slot].revents = 0;
		if (poll_entry[seat_slot].fd >= 0)
			entries++;
		busy = sessiond_exchange_busy(&greeter.exchange);
		timeout = 1000;
		if (busy)
			timeout = SESSIOND_EXCHANGE_TICK_MS;
		ready = poll(poll_entry, entries, timeout);

		/* Reads what came; the greeter closing its end is its end. */
		if (ready > 0 && poll_entry[0].revents != 0)
			closed = greeter_read(&greeter);

		/* The sleep's answer goes to the compositor that asked. */
		if (ready > 0 && sleep_slot < entries && poll_entry[sleep_slot].revents != 0)
			sessiond_sleep_collect();

		/* The attempt moves on; a login passkey granted takes the account. */
		sessiond_exchange_tick(&greeter.exchange);
		if (greeter.exchange.logged_in) {
			greeter.logged_in = 1;
			break;
		}

		/* A device that came is heard at once; the others are looked for every GREETER_SEAT_MS. */
		plugged = 0;
		if (ready > 0 && seat_slot < entries && poll_entry[seat_slot].revents != 0)
			plugged = sessiond_seat_events_collect();

		/* A new input device is the greeter's (never a security key, seat.c). */
		now_ms = sessiond_milliseconds();
		if (plugged || now_ms - seat_ms >= GREETER_SEAT_MS) {
			sessiond_seat_give(greeter_account.passwd.pw_uid, greeter_account.passwd.pw_gid, 0);
			seat_ms = now_ms;
		}
	}

	/* An attempt still under way ends with the greeter. */
	sessiond_exchange_stop(&greeter.exchange);

	/* After a login the greeter stays on the screen until the session is ready for the display. */
	if (greeter.logged_in && !sessiond_stopping) {
		daemon->greeter_pid = greeter.pid;
		daemon->greeter_socket = greeter.socket;
		daemon->greeter_started = greeter.started;
		memset(&greeter_account, 0, sizeof(greeter_account));
		sessiond_log("SESSIOND GREETER stays pid=%ld", (long)greeter.pid);
		return SESSIOND_GREETER_LOGIN;
	}

	/* The greeter ends: after a login it ends itself, otherwise it has already. */
	end = greeter_wait(&greeter);
	sessiond_sleep_forget(greeter.socket);
	(void)close(greeter.socket);
	memset(&greeter_account, 0, sizeof(greeter_account));

	/* Succeeded: says how the greeter ended. */
	return end;
}

/*
 * Ends the greeter left on the screen after a login: its socket is closed,
 * which it takes as the sign to end; one that does not end in time is
 * ended.  Nothing happens when no greeter was left.
 */
void
sessiond_greeter_finish(
	struct sessiond *daemon)
{
	struct greeter greeter;

	/* Only a greeter left on the screen. */
	if (daemon->greeter_pid <= 0)
		return;

	/* The socket closes, then the greeter goes. */
	memset(&greeter, 0, sizeof(greeter));
	greeter.pid = daemon->greeter_pid;
	greeter.socket = daemon->greeter_socket;
	greeter.logged_in = 1;
	sessiond_sleep_forget(greeter.socket);
	(void)close(greeter.socket);
	(void)greeter_wait(&greeter);

	/* Succeeded: no greeter is left. */
	daemon->greeter_pid = 0;
	daemon->greeter_socket = -1;
}

/*
 * Has the greeter left on the screen give the display back ahead of its
 * end: sessiond's side of the socket is shut down, which the greeter takes
 * as the sign to go, and its RELEASED (or its end) is waited for, at most
 * GREETER_RELEASE_MS.  The process is reaped by sessiond_greeter_finish.
 */
void
sessiond_greeter_release(
	struct sessiond *daemon)
{
	char line[SESSIOND_LINE_MAX];
	int released;
	int got;
	int match;

	/* Only a greeter left on the screen. */
	if (daemon->greeter_pid <= 0)
		return;

	/* The sign to go, then RELEASED. */
	(void)shutdown(daemon->greeter_socket, SHUT_WR);
	released = 0;
	while (!released) {
		got = sessiond_read_line(daemon->greeter_socket, line, sizeof(line), GREETER_RELEASE_MS);
		if (got <= 0)
			break;
		match = strcmp(line, "RELEASED");
		if (match == 0)
			released = 1;
	}

	/* Succeeded: the display is free (or the greeter has gone, or was given its time). */
	sessiond_log("SESSIOND HANDOFF greeter released=%d at_ms=%lld", released, sessiond_milliseconds());
}

/*
 * Starts a greeter while the session still shows (a Log Out, ws035-p101),
 * the seat given to it, and waits for its READY, which is not answered
 * yet (sessiond_greeter_go does).  The greeter is left for
 * sessiond_greeter_run to adopt.  Returns 0 once it said READY, or an
 * errno value.
 */
int
sessiond_greeter_prepare(
	struct sessiond *daemon)
{
	struct sessiond_account greeter_account;
	struct greeter greeter;
	char line[SESSIOND_LINE_MAX];
	int error;
	int got;
	int match;

	/* The greeter, as at any start. */
	memset(&greeter, 0, sizeof(greeter));
	greeter.socket = -1;
	error = greeter_start(daemon, &greeter, &greeter_account);
	memset(&greeter_account, 0, sizeof(greeter_account));
	if (error != 0)
		return error;

	/* Left for sessiond_greeter_run. */
	daemon->greeter_pid = greeter.pid;
	daemon->greeter_socket = greeter.socket;
	daemon->greeter_started = greeter.started;

	/* Its READY (it says nothing before it). */
	for (;;) {
		got = sessiond_read_line(greeter.socket, line, sizeof(line), GREETER_READY_MS);
		if (got <= 0) {
			sessiond_log("SESSIOND HANDOFF greeter not ready");
			return ETIMEDOUT;
		}

		/* Only READY ends the wait. */
		match = strcmp(line, "READY");
		if (match == 0)
			break;
	}

	/* Succeeded: the greeter waits for GO. */
	sessiond_log("SESSIOND HANDOFF greeter ready: waits at_ms=%lld", sessiond_milliseconds());
	return 0;
}

/* Tells the greeter a Log Out started that the display is free. */
void
sessiond_greeter_go(
	struct sessiond *daemon)
{
	ssize_t written;

	/* Only a greeter left for sessiond_greeter_run. */
	if (daemon->greeter_pid <= 0)
		return;

	/* GO. */
	written = write(daemon->greeter_socket, "GO\n", 3U);
	sessiond_log("SESSIOND HANDOFF greeter go written=%ld at_ms=%lld", (long)written, sessiond_milliseconds());
}

/* Looks up the greeter's account.  Returns 0, or ENOENT. */
static int
greeter_account_find(
	struct sessiond_account *greeter_account)
{
	struct passwd *found;
	int error;

	/* The account by its name. */
	found = NULL;
	error = getpwnam_r(SESSIOND_GREETER_USER, &greeter_account->passwd, greeter_account->buffer, sizeof(greeter_account->buffer), &found);
	if (error != 0 || found == NULL) {
		sessiond_log("SESSIOND GREETER no account=%s", SESSIOND_GREETER_USER);
		return ENOENT;
	}

	/* Succeeded. */
	return 0;
}

/* Starts the greeter as _greeter with the display and the input devices given to it. */
static int
greeter_start(
	struct sessiond *daemon,
	struct greeter *greeter,
	struct sessiond_account *greeter_account)
{
	int pair[2];
	int error;

	/* The greeter's account. */
	error = greeter_account_find(greeter_account);
	if (error != 0)
		return error;

	/* The seat is the greeter's while it runs. */
	sessiond_seat_give(greeter_account->passwd.pw_uid, greeter_account->passwd.pw_gid, 0);

	/* The socket the greeter asks on. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (error != 0) {
		sessiond_log("SESSIOND GREETER socketpair errno=%d", errno);
		return errno;
	}

	/* The greeter's process. */
	greeter->pid = fork();
	if (greeter->pid < 0) {
		sessiond_log("SESSIOND GREETER fork errno=%d", errno);
		(void)close(pair[0]);
		(void)close(pair[1]);
		return EAGAIN;
	}

	/* The child becomes the greeter and does not come back. */
	if (greeter->pid == 0) {
		(void)close(pair[0]);
		greeter_child(daemon, pair[1], greeter_account);
	}

	/* sessiond keeps its own end. */
	(void)close(pair[1]);
	greeter->socket = pair[0];
	(void)fcntl(greeter->socket, F_SETFD, FD_CLOEXEC);
	greeter->started = time(NULL);

	/* Succeeded: the greeter is starting. */
	sessiond_log("SESSIOND GREETER start pid=%ld uid=%u", (long)greeter->pid, (unsigned)greeter_account->passwd.pw_uid);
	return 0;
}

/* Becomes the greeter: its descriptors, its account, its environment, then the program. */
static void
greeter_child(
	struct sessiond *daemon,
	int socket,
	struct sessiond_account *greeter_account)
{
	char *arguments[5];
	char *environment[5];
	char user[96];
	char wallpaper[160];
	struct stat status;
	int descriptor;
	int error;

	/* The socket as descriptor 3, the log as its output, nothing to read on standard input. */
	(void)dup2(socket, SESSIOND_AUTH_FD);
	descriptor = open("/dev/null", O_RDONLY);
	if (descriptor >= 0)
		(void)dup2(descriptor, STDIN_FILENO);
	descriptor = open(GREETER_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0640);
	if (descriptor >= 0) {
		(void)dup2(descriptor, STDOUT_FILENO);
		(void)dup2(descriptor, STDERR_FILENO);
	}

	/* No other descriptor of sessiond's goes with it. */
	for (descriptor = SESSIOND_AUTH_FD + 1; descriptor < 256; descriptor++)
		(void)close(descriptor);

	/* Its own session, and its account's groups and ids (the last step as root). */
	(void)setsid();
	error = initgroups(greeter_account->passwd.pw_name, greeter_account->passwd.pw_gid);
	if (error != 0)
		_exit(126);
	error = setgid(greeter_account->passwd.pw_gid);
	if (error != 0)
		_exit(126);
	error = setuid(greeter_account->passwd.pw_uid);
	if (error != 0)
		_exit(126);
	(void)chdir("/");

	/* A small environment of its own. */
	snprintf(user, sizeof(user), "USER=%s", greeter_account->passwd.pw_name);
	environment[0] = user;
	environment[1] = "HOME=/var/empty";
	environment[2] = "PATH=/bin:/sbin:/usr/bin";
	environment[3] = NULL;

	/* The greeter's options: the wallpaper when the image has one. */
	arguments[0] = (char *)daemon->greeter;
	arguments[1] = "--greeter";
	arguments[2] = "--auth-fd=3";
	arguments[3] = NULL;
	error = stat(SESSIOND_WALLPAPER, &status);
	if (error == 0) {
		snprintf(wallpaper, sizeof(wallpaper), "--wallpaper=%s", SESSIOND_WALLPAPER);
		arguments[3] = wallpaper;
		arguments[4] = NULL;
	}

	/* Only a failed exec comes back. */
	(void)execve(daemon->greeter, arguments, environment);
	_exit(127);
}

/* Reads what the greeter sent and answers each whole line; reports 1 when the greeter has closed its end. */
static int
greeter_read(
	struct greeter *greeter)
{
	char *end;
	ssize_t count;
	size_t length;

	/* What came. */
	count = read(greeter->socket, greeter->line + greeter->used, sizeof(greeter->line) - 1U - greeter->used);
	if (count < 0 && errno == EINTR)
		return 0;
	if (count <= 0)
		return 1;
	greeter->used += (size_t)count;
	greeter->line[greeter->used] = '\0';

	/* Each whole line. */
	for (;;) {
		end = strchr(greeter->line, '\n');
		if (end == NULL)
			break;
		*end = '\0';
		length = (size_t)(end - greeter->line) + 1U;
		greeter_request(greeter, greeter->line);

		/* The rest moves to the front, and what was read is erased (a password may be in it). */
		memmove(greeter->line, greeter->line + length, greeter->used - length);
		greeter->used -= length;
		memset(greeter->line + greeter->used, 0, sizeof(greeter->line) - greeter->used);
		if (greeter->logged_in)
			return 0;
	}

	/* A line longer than any request is thrown away. */
	if (greeter->used + 1U >= sizeof(greeter->line)) {
		memset(greeter->line, 0, sizeof(greeter->line));
		greeter->used = 0;
		greeter_reply(greeter, "ERROR");
	}

	/* Succeeded: the greeter is still there. */
	return 0;
}

/* Answers one request of the greeter. */
static void
greeter_request(
	struct greeter *greeter,
	char *line)
{
	int match;

	/* The greeter is about to take the display: nothing holds it while no session runs. */
	match = strcmp(line, "READY");
	if (match == 0) {
		sessiond_log("SESSIOND HANDOFF greeter ready: go");
		greeter_reply(greeter, "GO");
		return;
	}

	/* A login's request (STYLES, AUTH, CANCEL), or the secret's line after AUTH. */
	match = sessiond_exchange_line(&greeter->exchange, line);
	if (match)
		return;

	/* The power button. */
	match = strncmp(line, "POWER ", 6);
	if (match == 0) {
		greeter_power(greeter, line + 6);
		return;
	}

	/* Anything else. */
	greeter_reply(greeter, "ERROR");
}

/* Ends the machine the way the greeter's power button asks (no one is logged in on the console), or sleeps it. */
static void
greeter_power(
	struct greeter *greeter,
	const char *what)
{
	const char *program;
	int differs;
	int error;

	/* The sleep, and its cancel (never answered), for the login screen too (ws052-p011). */
	differs = strcmp(what, "suspend");
	if (differs == 0) {
		sessiond_sleep_request(greeter->socket, "the graphical login");
		return;
	}
	differs = strcmp(what, "cancel");
	if (differs == 0) {
		sessiond_sleep_cancel();
		return;
	}

	/* Which program ends the machine that way (power-rules.c). */
	program = sessiond_power_program(what);
	if (program == NULL) {
		greeter_reply(greeter, "ERROR");
		return;
	}

	/* Runs it; it asks init, which stops sessiond among the rest. */
	error = sessiond_power_run(program, what, "the graphical login");
	if (error != 0) {
		greeter_reply(greeter, "ERROR");
		return;
	}

	/* Succeeded: the greeter hears that the machine is ending. */
	greeter_reply(greeter, "OK");
}

/* Sends one reply line to the greeter. */
static void
greeter_reply(
	struct greeter *greeter,
	const char *reply)
{
	char line[SESSIOND_LINE_MAX];
	size_t length;
	ssize_t written;

	/* The reply and its line end, in one write (room for any reply, ws172 m1). */
	snprintf(line, sizeof(line), "%s\n", reply);
	length = strlen(line);
	written = write(greeter->socket, line, length);
	if (written != (ssize_t)length)
		sessiond_log("SESSIOND GREETER reply lost");
}

/* Waits for the greeter to end and says how it ended. */
static enum sessiond_greeter_end
greeter_wait(
	struct greeter *greeter)
{
	time_t waited_from;
	time_t now;
	pid_t waited;
	int status;
	int exited;
	int code;
	int failed;

	/* A greeter that logged a user in ends by itself; one that does not in time is ended. */
	waited_from = time(NULL);
	for (;;) {
		waited = waitpid(greeter->pid, &status, WNOHANG);
		if (waited == greeter->pid)
			break;

		/* A greeter that is gone some other way has no status. */
		if (waited < 0 && errno != EINTR) {
			status = 0;
			break;
		}

		/* One that takes too long is ended. */
		now = time(NULL);
		if (now - waited_from >= GREETER_END_SECONDS) {
			greeter_kill(greeter);
			status = 0;
			break;
		}

		/* Looks again shortly. */
		usleep(50000);
	}

	/* After a login the session starts, however the greeter went. */
	sessiond_log("SESSIOND GREETER end pid=%ld status=%d", (long)greeter->pid, status);
	if (greeter->logged_in)
		return SESSIOND_GREETER_LOGIN;

	/* A greeter that ended with an error soon after it started has failed. */
	now = time(NULL);
	exited = WIFEXITED(status);
	code = WEXITSTATUS(status);
	failed = 0;
	if (!exited)
		failed = 1;
	else if (code != 0)
		failed = 1;
	if (failed && now - greeter->started < GREETER_QUICK_SECONDS) {
		greeter_reason();
		return SESSIOND_GREETER_FAILED;
	}

	/* Succeeded: the greeter ended by itself. */
	return SESSIOND_GREETER_ENDED;
}

/* Ends the greeter: TERM, then KILL when it does not go. */
static void
greeter_kill(
	struct greeter *greeter)
{
	pid_t waited;
	int status;
	int tries;

	/* Asks it to go. */
	(void)kill(greeter->pid, SIGTERM);

	/* Gives it three seconds. */
	for (tries = 0; tries < 60; tries++) {
		waited = waitpid(greeter->pid, &status, WNOHANG);
		if (waited == greeter->pid)
			return;
		usleep(50000);
	}

	/* Then makes it go. */
	(void)kill(greeter->pid, SIGKILL);
	(void)waitpid(greeter->pid, &status, 0);
}

/*
 * Puts the reason a greeter failed into sessiond's log (BUG-122): the last
 * line of its own log that says how it exited (KWL EXIT … error=), or its
 * last line when it wrote none.
 */
static void
greeter_reason(void)
{
	char tail[GREETER_REASON_BYTES + 1];
	char *line;
	char *found;
	char *end;
	off_t size;
	ssize_t count;
	int descriptor;

	/* The end of the log. */
	descriptor = open(GREETER_LOG_PATH, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return;
	size = lseek(descriptor, 0, SEEK_END);
	if (size > (off_t)GREETER_REASON_BYTES)
		(void)lseek(descriptor, size - (off_t)GREETER_REASON_BYTES, SEEK_SET);
	else
		(void)lseek(descriptor, 0, SEEK_SET);
	count = read(descriptor, tail, GREETER_REASON_BYTES);
	(void)close(descriptor);
	if (count <= 0)
		return;
	tail[count] = '\0';

	/* The last KWL EXIT line, else the last line. */
	found = NULL;
	line = strstr(tail, "KWL EXIT");
	while (line != NULL) {
		found = line;
		line = strstr(line + 1, "KWL EXIT");
	}

	/* No such line: the last line, without the line ends after it. */
	if (found == NULL) {
		while (count > 0 && tail[count - 1] == '\n') {
			count--;
			tail[count] = '\0';
		}

		/* After the line end before it. */
		found = strrchr(tail, '\n');
		if (found == NULL)
			found = tail;
		else
			found++;
	}

	/* One line of it. */
	end = strchr(found, '\n');
	if (end != NULL)
		*end = '\0';
	sessiond_log("SESSIOND GREETER failed reason=\"%.200s\"", found);
}
