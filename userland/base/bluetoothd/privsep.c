/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's privilege separation (ws143-p004, see privsep.h and
 * plan/ws143/phase004/phase.md section 4).
 *
 * The parent keeps nothing but the channel to the child and its end of the
 * liveness stream.  It ends when the child ends (it looks with waitpid at
 * least once a second, since a datagram socket gives no end of file), and
 * passes SIGTERM and SIGINT on to the child before it ends (the service
 * manager signals the parent alone).  The child sees the parent go as the
 * end of the liveness stream.
 *
 * The child's loop reports ALIVE once a round. Firmware startup reports
 * STARTING for its longer finite deadline. When progress stops, this
 * parent terminates and reaps its own child, then exits with failure to
 * request the service manager's existing on-failure restart.
 */

#include "userland/base/bluetoothd/privsep.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* How many nodes there can be, and the longest request or answer on the channel. */
#define PRIVSEP_NODES		16U
#define PRIVSEP_MESSAGE_MAX	96U

/* How often the parent looks at its child, and how long a child told to end may take. */
#define PRIVSEP_LOOK_MS		1000
#define PRIVSEP_END_SECONDS 3U

/* How long the child waits for the parent's answer. */
#define PRIVSEP_ANSWER_MS	2000

/* A stalled loop is restarted; firmware initialization gets a separate finite budget. */
#define PRIVSEP_STALL_MS 15000U
#define PRIVSEP_START_MS 90000U

/*
 * The signal the parent was asked to end with (0: none).  Set by the
 * signal handler, read by the parent's loop.
 */
static volatile sig_atomic_t privsep_ending;

static void privsep_no_account(void);
static int privsep_folders(const char *keys_folder, uid_t uid, gid_t gid);
static void privsep_parent(pid_t child, int channel, const char *node);
static void privsep_end(int signal_number);
static unsigned privsep_answer(int channel, const char *node);
static int privsep_clock(uint64_t *now);
static int privsep_open_node(const char *node, char *path, size_t size, int *descriptor);
static void privsep_send(int channel, const char *text, int descriptor);
static void privsep_wait_child(pid_t child);
static int privsep_ask(const struct btd_privsep *privsep, const char *request, char *path, size_t size, int *descriptor);

/*
 * Separates the daemon: the folders made for the account, the channel and
 * the liveness stream, then a fork.  The child drops to the account and
 * returns 0 with its ends; the parent never returns (it serves the child's
 * requests and ends with it).  listener is the socket the child serves,
 * which the parent closes.  Returns an errno value when the separation
 * could not be set up.
 */
int
btd_privsep_start(
	const char *node,
	const char *keys_folder,
	int listener,
	struct btd_privsep *privsep)
{
	struct passwd *account;
	uid_t uid;
	gid_t gid;
	pid_t child;
	int channel[2];
	int liveness[2];
	int status;
	int error;

	/* The daemon's account; without it nothing is separated (decision Q4). */
	account = getpwnam(BTD_ACCOUNT);
	if (account == NULL)
		privsep_no_account();
	uid = account->pw_uid;
	gid = account->pw_gid;

	/* The bonds' folder, the account's alone. */
	error = privsep_folders(keys_folder, uid, gid);
	if (error != 0)
		return error;

	/* The channel of requests and descriptors, and the stream whose end tells the child that the parent went. */
	status = socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, channel);
	if (status != 0)
		return errno;
	status = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, liveness);
	if (status != 0) {
		error = errno;
		(void)close(channel[0]);
		(void)close(channel[1]);
		return error;
	}

	/* The child. */
	child = fork();
	if (child < 0) {
		error = errno;
		(void)close(channel[0]);
		(void)close(channel[1]);
		(void)close(liveness[0]);
		(void)close(liveness[1]);
		return error;
	}

	/* The parent keeps its two ends only, and serves the child until it ends. */
	if (child != 0) {
		(void)close(listener);
		(void)close(channel[1]);
		(void)close(liveness[1]);
		privsep_parent(child, channel[0], node);
		_exit(1);
	}

	/* The child keeps its two ends. */
	(void)close(channel[0]);
	(void)close(liveness[0]);

	/* No supplementary group, then the account's group and user. */
	status = setgroups(0U, NULL);
	if (status != 0)
		_exit(1);
	status = setgid(gid);
	if (status != 0)
		_exit(1);
	status = setuid(uid);
	if (status != 0)
		_exit(1);

	/* There is no way back to root (a drop that could be undone would be no drop). */
	status = setuid(0);
	if (status == 0)
		_exit(1);

	/* Succeeded: the child runs as the account. */
	privsep->channel = channel[1];
	privsep->liveness = liveness[1];
	return 0;
}

/*
 * Asks the parent for the controller's node (the one given at the start,
 * or the lowest that opens).  Returns 0 with the node's path and
 * descriptor, or the errno value of the open (EBUSY: another program has
 * it; ENOENT: none), or the channel's error.
 */
int
btd_privsep_open(
	const struct btd_privsep *privsep,
	char *path,
	size_t size,
	int *descriptor)
{
	int error;

	/* The parent's answer to OPEN. */
	error = privsep_ask(privsep, "OPEN", path, size, descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the node. */
	return 0;
}

/*
 * Asks the parent for an open of /dev/input/bridge, for one HID device
 * (ws143-p005).  Returns 0 with the descriptor, or the errno value of the
 * open (EBUSY: the kernel's opens are all in use), or the channel's error.
 */
int
btd_privsep_open_bridge(
	const struct btd_privsep *privsep,
	int *descriptor)
{
	char path[PRIVSEP_MESSAGE_MAX];
	int error;

	/* The parent's answer to OPEN-HID. */
	error = privsep_ask(privsep, "OPEN-HID", path, sizeof(path), descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the open. */
	return 0;
}

/*
 * Reports progress to the supervisor without waiting for a reply.
 */
int
btd_privsep_progress(
	const struct btd_privsep *privsep,
	int starting)
{
	const char *message;
	size_t length;
	ssize_t sent;

	/* Firmware startup is bounded separately from the normal event loop. */
	message = "ALIVE";
	if (starting)
		message = "STARTING";
	length = strlen(message);
	sent = send(privsep->channel, message, length, MSG_DONTWAIT);
	if (sent < 0)
		return errno;
	if ((size_t)sent != length)
		return EIO;

	/* Succeeded: the parent can observe this loop's progress. */
	return 0;
}

/*
 * Sends a request to the parent and reads its answer: "OK PATH" with a
 * descriptor gives 0, the path and the descriptor; "ERR ERRNO" gives the
 * errno value; anything else EBADMSG, no answer ETIMEDOUT, a channel that
 * went EPIPE.
 */
static int
privsep_ask(
	const struct btd_privsep *privsep,
	const char *request,
	char *path,
	size_t size,
	int *descriptor)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct pollfd answer_ready;
	struct cmsghdr *message_control;
	struct msghdr header;
	struct iovec vector;
	char answer[PRIVSEP_MESSAGE_MAX];
	size_t least;
	size_t request_length;
	ssize_t length;
	unsigned long value;
	char *end;
	int ready;
	int same;

	/* The request. */
	*descriptor = -1;
	request_length = strlen(request);
	length = send(privsep->channel, request, request_length, 0);
	if (length != (ssize_t)request_length)
		return EPIPE;

	/* The answer, within PRIVSEP_ANSWER_MS. */
	answer_ready.fd = privsep->channel;
	answer_ready.events = POLLIN;
	answer_ready.revents = 0;
	ready = poll(&answer_ready, 1U, PRIVSEP_ANSWER_MS);
	if (ready <= 0)
		return ETIMEDOUT;

	/* The answer's text and the descriptor it may carry. */
	memset(&header, 0, sizeof(header));
	memset(&control, 0, sizeof(control));
	vector.iov_base = answer;
	vector.iov_len = sizeof(answer) - 1U;
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control.space;
	header.msg_controllen = sizeof(control.space);
	length = recvmsg(privsep->channel, &header, MSG_CMSG_CLOEXEC);
	if (length <= 0)
		return EPIPE;
	answer[length] = '\0';

	/* The descriptor, when one came. */
	message_control = CMSG_FIRSTHDR(&header);
	least = CMSG_LEN(sizeof(int));
	if (message_control != NULL &&
	    message_control->cmsg_level == SOL_SOCKET &&
	    message_control->cmsg_type == SCM_RIGHTS &&
	    message_control->cmsg_len >= least)
		memcpy(descriptor, CMSG_DATA(message_control), sizeof(int));

	/* "OK PATH" with the descriptor. */
	same = strncmp(answer, "OK ", 3U);
	if (same == 0 && *descriptor >= 0) {
		(void)snprintf(path, size, "%s", answer + 3);
		return 0;
	}

	/* A descriptor that came with anything else is not kept. */
	if (*descriptor >= 0) {
		(void)close(*descriptor);
		*descriptor = -1;
	}

	/* "ERR ERRNO". */
	same = strncmp(answer, "ERR ", 4U);
	if (same != 0)
		return EBADMSG;
	errno = 0;
	value = strtoul(answer + 4, &end, 10);
	if (errno != 0 || end == answer + 4 || value == 0U || value > 255U)
		return EBADMSG;

	/* Reports the parent's error. */
	return (int)value;
}

/*
 * Handles an install without the daemon's account (decision Q4, the
 * user's to make: add the account to existing installs, refuse to start,
 * or run unseparated).  Until it is made bluetoothd does not start: it is
 * in no default image yet, so no install that runs it lacks the account.
 * It ends with 0 so the service manager does not start it again and
 * again.
 */
static void
privsep_no_account(
	void)
{
	/* Logged, and not started. */
	syslog(LOG_ERR, "no %s account; not starting", BTD_ACCOUNT);
	(void)fprintf(stderr, "bluetoothd: no %s account; not starting\n", BTD_ACCOUNT);
	exit(0);
}

/*
 * Makes the bonds' folder the account's alone (0700) under /var/db (made
 * 0755 when it is not there).  A folder that is a symbolic link, or not a
 * folder, is refused.  Returns 0 or an errno value.
 */
static int
privsep_folders(
	const char *keys_folder,
	uid_t uid,
	gid_t gid)
{
	struct stat status;
	int folder;
	int result;

	/* /var/db, made when it is not there. */
	result = mkdir(BTD_DATA_PARENT, 0755);
	if (result != 0 && errno != EEXIST)
		return errno;

	/* The bonds' folder, made when it is not there. */
	result = mkdir(keys_folder, 0700);
	if (result != 0 && errno != EEXIST)
		return errno;

	/* A real folder, not a link to somewhere else. */
	result = lstat(keys_folder, &status);
	if (result != 0)
		return errno;
	folder = S_ISDIR(status.st_mode);
	if (!folder)
		return ENOTDIR;

	/* The account's, and nobody else's. */
	result = chown(keys_folder, uid, gid);
	if (result != 0)
		return errno;
	result = chmod(keys_folder, 0700);
	if (result != 0)
		return errno;

	/* Succeeded: the folder is ready. */
	return 0;
}

/*
 * The parent's loop: answers the child's requests, ends with the child, and
 * passes a request to end on to it.  It does not return.
 */
static void
privsep_parent(
	pid_t child,
	int channel,
	const char *node)
{
	struct sigaction action;
	struct pollfd request;
	uint64_t now;
	uint64_t deadline;
	pid_t ended;
	unsigned budget;
	int exited;
	int status;
	int ready;
	int error;

	/* SIGTERM and SIGINT end the child too; they interrupt the wait (no restart). */
	memset(&action, 0, sizeof(action));
	action.sa_handler = privsep_end;
	(void)sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	(void)sigaction(SIGTERM, &action, NULL);
	(void)sigaction(SIGINT, &action, NULL);

	/* The first firmware load must finish before normal loop supervision begins. */
	error = privsep_clock(&now);
	if (error != 0) {
		(void)kill(child, SIGTERM);
		privsep_wait_child(child);
		_exit(1);
	}

	/* Arms the first initialization deadline after obtaining a valid clock. */
	deadline = now + PRIVSEP_START_MS;

	/* Each round: the child's end, a request to end, a request of the child's. */
	for (;;) {
		/* A child that ended ends the parent with its status. */
		ended = waitpid(child, &status, WNOHANG);
		if (ended == child) {
			exited = WIFEXITED(status);
			if (exited)
				_exit(WEXITSTATUS(status));
			_exit(1);
		}

		/* A request to end: passed on, and the child awaited. */
		if (privsep_ending != 0) {
			(void)kill(child, SIGTERM);
			privsep_wait_child(child);
			_exit(0);
		}

		/* A child blocked away from its loop cannot feed the supervisor. */
		error = privsep_clock(&now);
		if (error != 0 || now >= deadline) {
			syslog(LOG_ERR, "WATCHDOG child-unresponsive; restarting bluetoothd");
			(void)kill(child, SIGTERM);
			privsep_wait_child(child);
			_exit(1);
		}

		/* A request, or the next look. */
		request.fd = channel;
		request.events = POLLIN;
		request.revents = 0;
		ready = poll(&request, 1U, PRIVSEP_LOOK_MS);
		if (ready > 0 && (request.revents & POLLIN) != 0) {
			budget = privsep_answer(channel, node);
			if (budget == 0U)
				continue;

			/* Only explicit progress changes the deadline; OPEN replies do not extend it. */
			error = privsep_clock(&now);
			if (error == 0)
				deadline = now + budget;
		}
	}
}

/* Reads the monotonic clock used for process supervision. */
static int
privsep_clock(
	uint64_t *now)
{
	struct timespec clock;
	int status;

	/* Wall-clock changes cannot extend or shorten the child's progress budget. */
	status = clock_gettime(CLOCK_MONOTONIC, &clock);
	if (status != 0)
		return errno;
	*now = (uint64_t)clock.tv_sec * 1000U + (uint64_t)clock.tv_nsec / 1000000U;

	/* Succeeded: an elapsed-time value for the supervisor. */
	return 0;
}

/* Notes a request to end (the signal handler). */
static void
privsep_end(
	int signal_number)
{
	/* The loop passes it on. */
	privsep_ending = signal_number;
}

/*
 * Answers one request of the child's: "OPEN" gets the node's descriptor,
 * "OPEN-HID" an open of /dev/input/bridge (ws143-p005), or why not.
 */
static unsigned
privsep_answer(
	int channel,
	const char *node)
{
	char request[PRIVSEP_MESSAGE_MAX];
	char path[64];
	char answer[PRIVSEP_MESSAGE_MAX];
	ssize_t length;
	int descriptor;
	int same;
	int error;

	/*
	 * The request, without waiting: a datagram socket whose peer closed
	 * is readable to poll while recv would wait for ever (it gives no end
	 * of file), so the loop must get back to waitpid (T1-405).
	 */
	length = recv(channel, request, sizeof(request) - 1U, MSG_DONTWAIT);
	if (length <= 0)
		return 0U;
	request[length] = '\0';

	/* Progress has no reply, so it cannot be mistaken for an OPEN descriptor. */
	same = strcmp(request, "ALIVE");
	if (same == 0)
		return PRIVSEP_STALL_MS;
	same = strcmp(request, "STARTING");
	if (same == 0)
		return PRIVSEP_START_MS;

	/* OPEN-HID: the bridge, opened here as root (only root may), sent, and the parent's copy closed. */
	same = strcmp(request, "OPEN-HID");
	if (same == 0) {
		descriptor = open(BTD_BRIDGE_PATH, O_RDWR | O_CLOEXEC);
		if (descriptor < 0) {
			(void)snprintf(answer, sizeof(answer), "ERR %d", errno);
			privsep_send(channel, answer, -1);
			return 0U;
		}

		/* Sent with its path. */
		privsep_send(channel, "OK " BTD_BRIDGE_PATH, descriptor);
		(void)close(descriptor);
		return 0U;
	}

	/* Otherwise only OPEN is known. */
	same = strcmp(request, "OPEN");
	if (same != 0) {
		privsep_send(channel, "ERR 22", -1);
		return 0U;
	}

	/* The node, opened here, as root. */
	error = privsep_open_node(node, path, sizeof(path), &descriptor);
	if (error != 0) {
		(void)snprintf(answer, sizeof(answer), "ERR %d", error);
		privsep_send(channel, answer, -1);
		return 0U;
	}

	/* Sent with its path; the parent's copy is closed, so the node is the child's alone. */
	(void)snprintf(answer, sizeof(answer), "OK %s", path);
	privsep_send(channel, answer, descriptor);
	(void)close(descriptor);

	/* Succeeded: the descriptor reply did not change the progress budget. */
	return 0U;
}

/*
 * Opens the controller's node: the one given at the start, or the lowest of
 * /dev/bluetooth0 to /dev/bluetooth15 that opens.  Returns 0 with the path and the
 * descriptor, EBUSY when a node is another program's, or ENOENT.
 */
static int
privsep_open_node(
	const char *node,
	char *path,
	size_t size,
	int *descriptor)
{
	unsigned index;
	int error;

	/* The node given. */
	if (node != NULL) {
		(void)snprintf(path, size, "%s", node);
		*descriptor = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (*descriptor < 0)
			return errno;
		return 0;
	}

	/* The lowest that opens; a busy one is remembered. */
	error = ENOENT;
	for (index = 0U; index < PRIVSEP_NODES; index++) {
		(void)snprintf(path, size, "/dev/bluetooth%u", index);
		*descriptor = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (*descriptor >= 0)
			return 0;
		if (errno == EBUSY)
			error = EBUSY;
	}

	/* None opened. */
	return error;
}

/* Sends an answer to the child, with a descriptor when one is given (>= 0). */
static void
privsep_send(
	int channel,
	const char *text,
	int descriptor)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct cmsghdr *message_control;
	struct msghdr header;
	struct iovec vector;

	/* The text. */
	memset(&header, 0, sizeof(header));
	vector.iov_base = (void *)text;
	vector.iov_len = strlen(text);
	header.msg_iov = &vector;
	header.msg_iovlen = 1;

	/* The descriptor. */
	if (descriptor >= 0) {
		memset(&control, 0, sizeof(control));
		header.msg_control = control.space;
		header.msg_controllen = sizeof(control.space);
		message_control = CMSG_FIRSTHDR(&header);
		message_control->cmsg_level = SOL_SOCKET;
		message_control->cmsg_type = SCM_RIGHTS;
		message_control->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(message_control), &descriptor, sizeof(int));
	}

	/* One datagram (a child that went will not read it). */
	(void)sendmsg(channel, &header, 0);
}

/* Waits for a child told to end, PRIVSEP_END_SECONDS at most, then kills it. */
static void
privsep_wait_child(
	pid_t child)
{
	unsigned second;
	pid_t ended;
	int status;

	/* Each second, a look. */
	for (second = 0U; second < PRIVSEP_END_SECONDS; second++) {
		ended = waitpid(child, &status, WNOHANG);
		if (ended == child || ended < 0)
			return;
		(void)sleep(1U);
	}

	/* It did not end: killed. */
	(void)kill(child, SIGKILL);
	(void)waitpid(child, &status, 0);
}
