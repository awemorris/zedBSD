/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The user's session: after a login sessiond gives the seat to the user,
 * makes the user's runtime directory (/run/user/UID, 0700, the user's: the
 * Wayland socket goes there, where no other user can reach it), and runs
 * the session script (/etc/keiland/session) with /bin/sh as the user, the
 * way login starts a shell: initgroups, setgid, setuid, HOME, USER,
 * LOGNAME, PATH, SHELL and XDG_RUNTIME_DIR.  The login is recorded in
 * utmpx.  An account whose home directory is not there yet gets it at its
 * first login (ws035-p120).
 *
 * The session's user is also given the group "network" (2026-09-28 user
 * decision, ws035-p104): networkd's socket admits that group, and the
 * system bar's network menu speaks to networkd as the session's user.
 *
 * networkd is told when the session opens and when it has ended
 * (2026-10-02 user decision, ws005-p024): while the session is open, the
 * Wi-Fi networks saved in the user's own store are ones networkd joins on its
 * own; after the logout they are not, and what was joined with them ends.
 * The telling runs "net wifi session open|close UID" in the background, so a
 * slow or absent networkd never holds up a login or a logout.
 *
 * The session ends when the script ends (the compositor's Log Out).  Whatever of
 * it is left is ended too: its process group, and every process of the
 * user (not for root, whose processes are the system's).
 *
 * The hand-over of the display (ws035-p101): the script is given one end of
 * a socket pair as descriptor 3 and the option --control-fd=3 for the compositor.
 * The greeter stays on the screen while the session starts; the compositor says
 * READY when it is about to take the display, sessiond then has the
 * greeter give it back and answers GO.  A session that never says READY
 * (another script) has the greeter ended after SESSION_READY_SECONDS.  The
 * session's requests on the socket, one line each:
 *
 *   LOGOUT           Log Out with the display handed to a new greeter:
 *                    QUIT comes once the greeter is ready; the session
 *                    answers RELEASED when it has given the display back,
 *                    and ends
 *   UNLOCK style     then the secret on the next line: the lock screen
 *                    (ws035-p102); /sbin/passkey checks the session user's
 *                    password, PIN or key as a login does (auth.c,
 *                    ws172-p002); OK, or FAIL reason after a delay
 *   STYLES, ENROLLED, ENROLL pin|fido2 label, REMOVE pin|fido2 id, CANCEL
 *                    the session user's ways to unlock, and their changes
 *                    (Settings > Users); the current password and the new
 *                    secret follow on their own lines
 *   SERVICE name on|off|status
 *                    a system service the Sharing page turns on or off
 *                    (ws089-p025, service.c: sshd only, root or wheel);
 *                    SERVICE available= enabled= running= port=, DENIED,
 *                    or ERROR
 *   POWER poweroff|reboot
 *                    ends the machine (ws131-p027, power.c: the Power Off
 *                    dialog; root or wheel only); OK, FAIL wheel, or ERROR
 *   POWER suspend    sleeps the machine (ws052-p011, sleep.c; any session
 *                    user); SLEPT woke=…, NOSLEEP …, ERROR busy or ERROR
 *                    once the sleep is done
 *   POWER cancel     stops a sleep before the kernel is asked; no answer
 */

#include "auth.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

/* Where the users' runtime directories are. */
#define SESSION_RUNTIME_ROOT	"/run/user"

/* The utmpx line of the graphical seat. */
#define SESSION_LINE		"seat0"

/* The group networkd's socket admits, which every session's user joins. */
#define SESSION_NETWORK_GROUP	"network"

/* The most groups a session's user is read with. */
#define SESSION_GROUPS_MAX	64

/* The command that tells networkd about the session (ws005-p024). */
#define SESSION_NET_COMMAND	"/sbin/net"

/* How long the greeter stays on the screen for a session that does not say READY (seconds). */
#define SESSION_READY_SECONDS	30


static int session_runtime(struct sessiond_account *account, char *directory, size_t size);
static void session_home(struct sessiond_account *account);
static void session_runtime_clean(const char *directory);
static void session_child(struct sessiond *daemon, struct sessiond_account *account, const char *directory, int control);
static void session_network_group(void);
static void session_network_notify(const struct sessiond_account *account, const char *change);
static void session_handoff(struct sessiond *daemon, int control);
static int session_request(struct sessiond *daemon, struct sessiond_account *account, int control, struct sessiond_exchange *exchange);
static void session_logout(struct sessiond *daemon, int control);
static void session_record(int type, pid_t pid, const char *user);
static void session_sweep(struct sessiond_account *account, pid_t leader);
static void session_signal_user(uid_t uid, int signal_number);

/*
 * Runs a user's session until it ends or sessiond stops.
 */
int
sessiond_session_run(
	struct sessiond *daemon,
	struct sessiond_account *account)
{
	struct sessiond_exchange exchange;
	struct pollfd entry[4];
	char directory[64];
	pid_t child;
	pid_t waited;
	int pair[2];
	int control;
	int leaving;
	nfds_t entries;
	nfds_t sleep_slot;
	nfds_t seat_slot;
	int timeout;
	int busy;
	int ready;
	int status;
	int error;

	/* The seat is the user's now (the greeter keeps what it has open until it ends). */
	sessiond_seat_give(account->passwd.pw_uid, account->passwd.pw_gid, 1);

	/* The user's runtime directory (without it, the greeter left on the screen goes). */
	error = session_runtime(account, directory, sizeof(directory));
	if (error != 0) {
		sessiond_greeter_finish(daemon);
		return error;
	}

	/* The user's home directory, made on the first login of an account that has none yet. */
	session_home(account);

	/* The socket the session talks to sessiond on. */
	error = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (error != 0) {
		error = errno;
		sessiond_log("SESSIOND SESSION socketpair errno=%d", error);
		sessiond_greeter_finish(daemon);
		return error;
	}

	/* The session's process. */
	child = fork();
	if (child < 0) {
		sessiond_log("SESSIOND SESSION fork errno=%d", errno);
		(void)close(pair[0]);
		(void)close(pair[1]);
		sessiond_greeter_finish(daemon);
		return EAGAIN;
	}

	/* The child becomes the session and does not come back. */
	if (child == 0) {
		(void)close(pair[0]);
		session_child(daemon, account, directory, pair[1]);
	}

	/* sessiond keeps its own end. */
	(void)close(pair[1]);
	control = pair[0];
	(void)fcntl(control, F_SETFD, FD_CLOEXEC);

	/* The login is on record while the session runs. */
	session_record(USER_PROCESS, child, account->passwd.pw_name);
	sessiond_log("SESSIOND SESSION start user=%s uid=%u pid=%ld runtime=%s", account->passwd.pw_name, (unsigned)account->passwd.pw_uid, (long)child, directory);

	/* networkd may join the user's saved Wi-Fi networks while the session is open. */
	session_network_notify(account, "open");

	/* The display goes from the greeter to the session. */
	session_handoff(daemon, control);
	sessiond_exchange_init(&exchange, control, account, NULL);

	/* Waits for it to end, answering it, and giving a new input device to the user every second. */
	status = 0;
	leaving = 0;
	for (;;) {
		waited = waitpid(child, &status, WNOHANG);
		if (waited == child)
			break;
		if (waited < 0 && errno != EINTR)
			break;

		/* sessiond is being stopped: the session goes first. */
		if (sessiond_stopping) {
			(void)kill(-child, SIGTERM);
			(void)kill(child, SIGTERM);
		}

		/* A request of the session, passkey's answer, or a second (less while an attempt is under way). */
		entry[0].fd = control;
		entry[0].events = POLLIN;
		entry[0].revents = 0;
		entries = 1;
		entry[1].fd = sessiond_exchange_fd(&exchange);
		entry[1].events = POLLIN;
		entry[1].revents = 0;
		if (entry[1].fd >= 0)
			entries = 2;

		/* A sleep's answer, whoever asked it (sleep.c, ws052-p011). */
		sleep_slot = entries;
		entry[sleep_slot].fd = sessiond_sleep_fd();
		entry[sleep_slot].events = POLLIN;
		entry[sleep_slot].revents = 0;
		if (entry[sleep_slot].fd >= 0)
			entries++;

		/* A device plugged in, given below at once (seat.c, BUG-264). */
		seat_slot = entries;
		entry[seat_slot].fd = sessiond_seat_events_fd();
		entry[seat_slot].events = POLLIN;
		entry[seat_slot].revents = 0;
		if (entry[seat_slot].fd >= 0)
			entries++;
		busy = sessiond_exchange_busy(&exchange);
		timeout = 1000;
		if (busy)
			timeout = SESSIOND_EXCHANGE_TICK_MS;
		ready = 0;
		if (control >= 0)
			ready = poll(entry, entries, timeout);
		else
			sleep(1);
		if (ready > 0 && entry[0].revents != 0) {
			leaving |= session_request(daemon, account, control, &exchange);

			/* A session that closed its end says nothing more, and its attempt ends. */
			if ((entry[0].revents & (POLLHUP | POLLERR)) != 0 && (entry[0].revents & POLLIN) == 0) {
				sessiond_exchange_stop(&exchange);
				sessiond_sleep_forget(control);
				(void)close(control);
				control = -1;
			}
		}

		/* The sleep's answer goes to the compositor that asked. */
		if (ready > 0 && sleep_slot < entries && entry[sleep_slot].revents != 0)
			sessiond_sleep_collect();

		/* The events of the devices are read; the giving below follows every pass anyway. */
		if (ready > 0 && seat_slot < entries && entry[seat_slot].revents != 0)
			(void)sessiond_seat_events_collect();

		/* An attempt under way moves on. */
		if (control >= 0)
			sessiond_exchange_tick(&exchange);

		/* Any new input device is the user's too, until the session hands the seat to the greeter. */
		if (!leaving)
			sessiond_seat_give(account->passwd.pw_uid, account->passwd.pw_gid, 1);
	}

	/* The session has ended: what is left of it goes, and the record says so. */
	sessiond_exchange_stop(&exchange);
	sessiond_sleep_forget(control);
	if (control >= 0)
		(void)close(control);
	sessiond_log("SESSIOND SESSION end user=%s pid=%ld status=%d", account->passwd.pw_name, (long)child, status);
	session_sweep(account, child);
	session_record(DEAD_PROCESS, child, "");
	session_runtime_clean(directory);

	/* The user's saved Wi-Fi networks are no longer joined, and a connection made with them ends. */
	session_network_notify(account, "close");

	/* Succeeded: the session ran and ended. */
	return 0;
}

/*
 * Hands the display from the greeter to the session: waits for the
 * session's READY (at most SESSION_READY_SECONDS, or until it closes its
 * end), has the greeter give the display back, and answers GO; the
 * greeter's process is then reaped.
 */
static void
session_handoff(
	struct sessiond *daemon,
	int control)
{
	long long started;
	long long waited;
	char line[SESSIOND_LINE_MAX];
	ssize_t count;
	int ready;
	int got;
	int match;

	/* READY, a line of its own (anything before it is not the session's to say yet). */
	started = sessiond_milliseconds();
	ready = 0;
	while (!ready && !sessiond_stopping) {
		/* The time left. */
		waited = sessiond_milliseconds() - started;
		if (waited >= SESSION_READY_SECONDS * 1000LL)
			break;

		/* A line, or the end of the wait. */
		got = sessiond_read_line(control, line, sizeof(line), (int)(SESSION_READY_SECONDS * 1000LL - waited));
		if (got <= 0)
			break;
		match = strcmp(line, "READY");
		if (match == 0)
			ready = 1;
	}

	/* The greeter gives the display back. */
	waited = sessiond_milliseconds() - started;
	sessiond_log("SESSIOND HANDOFF session ready=%d waited_ms=%lld", ready, waited);
	sessiond_greeter_release(daemon);

	/* The session may take the display. */
	if (ready) {
		count = write(control, "GO\n", 3U);
		sessiond_log("SESSIOND HANDOFF go written=%ld at_ms=%lld", (long)count, sessiond_milliseconds());
	}

	/* The greeter's process goes after. */
	sessiond_greeter_finish(daemon);
}

/*
 * Answers one request of the session (a line on its socket).  Returns 1
 * when the session has handed the seat to the greeter (LOGOUT).
 */
static int
session_request(
	struct sessiond *daemon,
	struct sessiond_account *account,
	int control,
	struct sessiond_exchange *exchange)
{
	char line[SESSIOND_LINE_MAX];
	int got;
	int match;

	/* The line (a short wait: it came whole or nearly). */
	got = sessiond_read_line(control, line, sizeof(line), 500);
	if (got <= 0)
		return 0;

	/* The lock screen's and Settings' requests, or a secret's line after one (erased with the line). */
	match = sessiond_exchange_line(exchange, line);
	if (match) {
		memset(line, 0, sizeof(line));
		return 0;
	}

	/* A system service of the Sharing page (ws089-p025). */
	match = strncmp(line, "SERVICE ", 8);
	if (match == 0) {
		sessiond_service(account, control, line + 8);
		return 0;
	}

	/* Power Off or Restart (ws131-p027). */
	match = strncmp(line, "POWER ", 6);
	if (match == 0) {
		sessiond_power_session(account, control, line + 6);
		return 0;
	}

	/* Log Out, handing the display to a new greeter. */
	match = strcmp(line, "LOGOUT");
	if (match == 0) {
		session_logout(daemon, control);
		return 1;
	}

	/* Anything else. */
	(void)write(control, "ERROR\n", 6U);
	return 0;
}

/*
 * Log Out with the display handed over (ws035-p101): the greeter starts
 * while the session still shows, and says READY when it is about to take
 * the display; the session is then told QUIT, gives the display back
 * (RELEASED) and ends, and the greeter is told GO.  main() then carries on
 * with that greeter (sessiond_greeter_run adopts it).
 */
static void
session_logout(
	struct sessiond *daemon,
	int control)
{
	char line[SESSIOND_LINE_MAX];
	ssize_t count;
	int released;
	int got;
	int match;
	int error;

	/* The greeter, given the seat, until it is ready for the display. */
	sessiond_log("SESSIOND HANDOFF logout at_ms=%lld", sessiond_milliseconds());
	error = sessiond_greeter_prepare(daemon);

	/* The session gives the display back. */
	count = write(control, "QUIT\n", 5U);
	released = 0;
	while (count == 5 && !released) {
		got = sessiond_read_line(control, line, sizeof(line), 5000);
		if (got <= 0)
			break;
		match = strcmp(line, "RELEASED");
		if (match == 0)
			released = 1;
	}

	/* What came of it. */
	sessiond_log("SESSIOND HANDOFF session released=%d at_ms=%lld", released, sessiond_milliseconds());

	/* The greeter takes it. */
	if (error == 0)
		sessiond_greeter_go(daemon);
}

/* Makes the user's runtime directory: 0700, the user's, and a directory (not a link someone left). */
static int
session_runtime(
	struct sessiond_account *account,
	char *directory,
	size_t size)
{
	struct stat status;
	int error;

	/* The root of the runtime directories, root's and readable by all. */
	(void)mkdir("/run", 0755);
	(void)mkdir(SESSION_RUNTIME_ROOT, 0755);

	/* The user's own. */
	snprintf(directory, size, "%s/%u", SESSION_RUNTIME_ROOT, (unsigned)account->passwd.pw_uid);
	error = mkdir(directory, 0700);
	if (error != 0 && errno != EEXIST) {
		sessiond_log("SESSIOND SESSION runtime=%s errno=%d", directory, errno);
		return errno;
	}

	/* What is there must be a directory (lstat: not a link to somewhere else). */
	error = lstat(directory, &status);
	if (error != 0) {
		sessiond_log("SESSIOND SESSION runtime=%s errno=%d", directory, errno);
		return errno;
	}

	/* Anything but a directory is refused. */
	if ((status.st_mode & S_IFMT) != S_IFDIR) {
		sessiond_log("SESSIOND SESSION runtime=%s not a directory", directory);
		return ENOTDIR;
	}

	/* The user's, and no one else's. */
	error = chown(directory, account->passwd.pw_uid, account->passwd.pw_gid);
	if (error != 0)
		return errno;
	error = chmod(directory, 0700);
	if (error != 0)
		return errno;

	/* Succeeded: the directory is ready. */
	return 0;
}

/*
 * Makes the home directory of an account that has none yet (ws035-p120):
 * an image can name a person's account (the demonstration's kei) but
 * cannot give a directory to its owner, so the first login makes it, 0700
 * and the user's.  Anything already at the path is left as it is, and a
 * home outside a root-owned directory that only root may write is not
 * made.  A failure leaves the session in / (session_child).
 */
static void
session_home(
	struct sessiond_account *account)
{
	struct stat status;
	char parent[256];
	char *slash;
	size_t length;
	int descriptor;
	int error;

	/* Only an absolute path below the root, which fits. */
	if (account->passwd.pw_dir == NULL || account->passwd.pw_dir[0] != '/')
		return;
	if (account->passwd.pw_dir[1] == '\0')
		return;
	length = strlen(account->passwd.pw_dir);
	if (length >= sizeof(parent))
		return;

	/* Something already there is the account's home, or not for sessiond to replace. */
	error = lstat(account->passwd.pw_dir, &status);
	if (error == 0)
		return;
	if (errno != ENOENT)
		return;

	/* The directory it goes in: root's, a directory, and writable by root alone (no one can swap the path). */
	snprintf(parent, sizeof(parent), "%s", account->passwd.pw_dir);
	slash = strrchr(parent, '/');
	if (slash == parent) {
		parent[1] = '\0';
	} else {
		*slash = '\0';
	}

	/* What is there now. */
	error = lstat(parent, &status);
	if (error != 0)
		return;

	/* A link or a file in its place is refused. */
	if ((status.st_mode & S_IFMT) != S_IFDIR) {
		sessiond_log("SESSIOND SESSION home=%s parent not a directory", account->passwd.pw_dir);
		return;
	}

	/* So is a directory someone else owns or may write. */
	if (status.st_uid != 0 || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		sessiond_log("SESSIOND SESSION home=%s parent not root's", account->passwd.pw_dir);
		return;
	}

	/* The directory, private to its user. */
	error = mkdir(account->passwd.pw_dir, 0700);
	if (error != 0) {
		sessiond_log("SESSIOND SESSION home=%s errno=%d", account->passwd.pw_dir, errno);
		return;
	}

	/* The directory just made, opened (not a path that could be followed elsewhere). */
	descriptor = open(account->passwd.pw_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
	if (descriptor < 0) {
		sessiond_log("SESSIOND SESSION home=%s open errno=%d", account->passwd.pw_dir, errno);
		return;
	}

	/* Given to the user. */
	error = fchown(descriptor, account->passwd.pw_uid, account->passwd.pw_gid);
	(void)close(descriptor);
	if (error != 0) {
		sessiond_log("SESSIOND SESSION home=%s chown errno=%d", account->passwd.pw_dir, errno);
		return;
	}

	/* Succeeded: the home is made and the user's. */
	sessiond_log("SESSIOND SESSION home=%s made uid=%u", account->passwd.pw_dir, (unsigned)account->passwd.pw_uid);
}

/* Removes the session's Wayland socket from its runtime directory. */
static void
session_runtime_clean(
	const char *directory)
{
	char path[96];

	/* The socket and its lock; the log stays for the user to read. */
	snprintf(path, sizeof(path), "%s/wayland-0", directory);
	(void)unlink(path);
	snprintf(path, sizeof(path), "%s/wayland-0.lock", directory);
	(void)unlink(path);
}

/* Becomes the session: the user's ids, the user's environment, then the script. */
static void
session_child(
	struct sessiond *daemon,
	struct sessiond_account *account,
	const char *directory,
	int control)
{
	char *arguments[4];
	char *environment[8];
	char home[320];
	char user[96];
	char logname[96];
	char runtime[96];
	char log_path[96];
	int descriptor;
	int error;

	/* Its own session, and the user's groups and ids (the last step as root). */
	(void)setsid();
	error = initgroups(account->passwd.pw_name, account->passwd.pw_gid);
	if (error != 0)
		_exit(126);

	/* The system bar reaches networkd through the group its socket admits. */
	session_network_group();

	/* The user's ids, which leave root behind. */
	error = setgid(account->passwd.pw_gid);
	if (error != 0)
		_exit(126);
	error = setuid(account->passwd.pw_uid);
	if (error != 0)
		_exit(126);

	/*
	 * Nothing to read, and the output in the runtime directory (opened as
	 * the user).  The log starts empty for each session and is written at
	 * its end: a program of the session before that still writes after the
	 * truncation adds to the end instead of leaving a hole of NUL bytes
	 * (ws099-p028, as the compositor's own log).
	 */
	descriptor = open("/dev/null", O_RDONLY);
	if (descriptor >= 0)
		(void)dup2(descriptor, STDIN_FILENO);
	snprintf(log_path, sizeof(log_path), "%s/session.log", directory);
	descriptor = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
	if (descriptor >= 0) {
		(void)dup2(descriptor, STDOUT_FILENO);
		(void)dup2(descriptor, STDERR_FILENO);
	}

	/* The socket to sessiond as descriptor 3, and no other descriptor of sessiond's. */
	if (control != SESSIOND_CONTROL_FD) {
		(void)dup2(control, SESSIOND_CONTROL_FD);
		(void)close(control);
	}

	/* Nothing above it. */
	for (descriptor = SESSIOND_CONTROL_FD + 1; descriptor < 256; descriptor++)
		(void)close(descriptor);

	/* The home directory, or / when it cannot be entered. */
	error = chdir(account->passwd.pw_dir);
	if (error != 0)
		(void)chdir("/");

	/* The environment login gives a shell, and the runtime directory. */
	snprintf(home, sizeof(home), "HOME=%s", account->passwd.pw_dir);
	snprintf(user, sizeof(user), "USER=%s", account->passwd.pw_name);
	snprintf(logname, sizeof(logname), "LOGNAME=%s", account->passwd.pw_name);
	snprintf(runtime, sizeof(runtime), "XDG_RUNTIME_DIR=%s", directory);
	environment[0] = home;
	environment[1] = user;
	environment[2] = logname;
	environment[3] = "PATH=/bin:/sbin:/usr/bin";
	environment[4] = "SHELL=/bin/sh";
	environment[5] = runtime;
	environment[6] = NULL;

	/* The session script, told the descriptor; only a failed exec comes back. */
	arguments[0] = "sh";
	arguments[1] = (char *)daemon->session;
	arguments[2] = "--control-fd=3";
	arguments[3] = NULL;
	(void)execve("/bin/sh", arguments, environment);
	_exit(127);
}

/* Adds the group networkd's socket admits to the groups initgroups gave the session. */
static void
session_network_group(
	void)
{
	gid_t groups[SESSION_GROUPS_MAX];
	struct group *network;
	int count;
	int index;
	int error;

	/* The group, which a system without networkd may not have. */
	network = getgrnam(SESSION_NETWORK_GROUP);
	if (network == NULL)
		return;

	/* The groups the user has now, with room for one more. */
	count = getgroups(SESSION_GROUPS_MAX - 1, groups);
	if (count < 0) {
		syslog(LOG_WARNING, "the session's groups could not be read: %s", strerror(errno));
		return;
	}

	/* A user who is a member already keeps the list as it is. */
	for (index = 0; index < count; index++) {
		if (groups[index] == network->gr_gid)
			return;
	}

	/*
	 * The group joins the list.  A failure leaves the session without the
	 * network menu, not without a login.
	 */
	groups[count] = network->gr_gid;
	error = setgroups((size_t)count + 1U, groups);
	if (error != 0)
		syslog(LOG_WARNING, "the session could not join the group %s: %s", SESSION_NETWORK_GROUP, strerror(errno));
}

/*
 * Tells networkd that the session opened or closed ("open" or "close"),
 * without waiting for its answer.
 *
 * A child starts a grandchild that runs the net command and ends at once,
 * so sessiond reaps the child now and the grandchild is init's.  sessiond is
 * root, which networkd lets name the session's account.
 */
static void
session_network_notify(
	const struct sessiond_account *account,
	const char *change)
{
	char *arguments[6];
	char uid[16];
	pid_t child;
	pid_t worker;
	int status;

	/* The account's number, as the command takes it. */
	(void)snprintf(uid, sizeof(uid), "%u", (unsigned)account->passwd.pw_uid);
	sessiond_log("SESSIOND NETWORK session %s uid=%s", change, uid);

	/* The child, which only starts the worker. */
	child = fork();
	if (child < 0) {
		sessiond_log("SESSIOND NETWORK fork errno=%d", errno);
		return;
	}

	/* The worker runs the command; the child ends at once. */
	if (child == 0) {
		worker = fork();
		if (worker == 0) {
			arguments[0] = "net";
			arguments[1] = "wifi";
			arguments[2] = "session";
			arguments[3] = (char *)change;
			arguments[4] = uid;
			arguments[5] = NULL;
			(void)execv(SESSION_NET_COMMAND, arguments);
			_exit(127);
		}
		_exit(worker < 0 ? 1 : 0);
	}

	/* The child is reaped; the worker is left to init. */
	(void)waitpid(child, &status, 0);
}

/* Writes the session's utmpx record: logged in, or ended. */
static void
session_record(
	int type,
	pid_t pid,
	const char *user)
{
	struct utmpx record;
	struct timespec now;
	int error;

	/* The seat's line, the session's process, and who. */
	memset(&record, 0, sizeof(record));
	record.ut_type = (short)type;
	record.ut_pid = pid;
	strncpy(record.ut_line, SESSION_LINE, sizeof(record.ut_line) - 1U);
	strncpy(record.ut_id, SESSION_LINE, sizeof(record.ut_id));
	strncpy(record.ut_user, user, sizeof(record.ut_user) - 1U);

	/* When. */
	error = clock_gettime(CLOCK_REALTIME, &now);
	if (error == 0) {
		record.ut_tv.tv_sec = now.tv_sec;
		record.ut_tv.tv_usec = (long)(now.tv_nsec / 1000L);
	}

	/* The record. */
	(void)pututxline(&record);
}

/* Ends what is left of a session: its process group, then every process of the user. */
static void
session_sweep(
	struct sessiond_account *account,
	pid_t leader)
{
	/* The session's process group (the script's children that stayed in it). */
	(void)kill(-leader, SIGTERM);

	/* Root's processes are the system's: only an ordinary user's all go. */
	if (account->passwd.pw_uid == 0)
		return;

	/* The user's other processes (applications started in their own sessions), asked, then made to go. */
	session_signal_user(account->passwd.pw_uid, SIGTERM);
	sleep(2);
	session_signal_user(account->passwd.pw_uid, SIGKILL);
	(void)kill(-leader, SIGKILL);
}

/* Sends a signal to every process of a user, from a child that has become that user. */
static void
session_signal_user(
	uid_t uid,
	int signal_number)
{
	pid_t child;
	int status;
	int error;

	/* kill(-1) as the user reaches exactly the user's processes. */
	child = fork();
	if (child == 0) {
		error = setuid(uid);
		if (error != 0)
			_exit(126);
		(void)kill(-1, signal_number);
		_exit(0);
	}

	/* Waits for the child that sent it. */
	if (child > 0)
		(void)waitpid(child, &status, 0);
}
