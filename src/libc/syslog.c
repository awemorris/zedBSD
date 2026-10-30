/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Process-wide UNIX-datagram syslog client.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static char identity[64];
static int options, facility = LOG_USER, mask = LOG_UPTO(LOG_DEBUG),
		    descriptor = -1;
static volatile unsigned log_lock;

/*
 * How long a message waits for room in the log daemon's queue, in ms.
 *
 * The daemon drains its socket at once when it is well; a full queue means it
 * is behind, and a caller such as sshd or su must not be held until it
 * catches up.  A message that still finds no room is dropped and counted.
 */
#define LOG_ROOM_WAIT_MS 100

/*
 * The messages dropped since one last reached the daemon, protected by
 * log_lock.  The next message that gets through is followed by a line
 * with this count, so the loss shows in the log where it happened.
 */
static unsigned long dropped_messages;

static int send_message(const char *message, size_t length);

static void
lock_log(void)
{
	while (__atomic_exchange_n(&log_lock, 1U, __ATOMIC_ACQUIRE))
		;
}
static void
unlock_log(void)
{
	__atomic_store_n(&log_lock, 0U, __ATOMIC_RELEASE);
}

static int
connect_logger(void)
{
	struct sockaddr_un address;
	if (descriptor >= 0)
		return 0;
	descriptor = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, "/run/log");
	if (connect(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
	    0) {
		close(descriptor);
		descriptor = -1;
		return -1;
	}
	return 0;
}

void
openlog(const char *ident, int option, int selected_facility)
{
	lock_log();
	if (ident != NULL) {
		strncpy(identity, ident, sizeof(identity) - 1);
		identity[sizeof(identity) - 1] = '\0';
	}
	options = option;
	if (selected_facility)
		facility = selected_facility;
	if (options & LOG_NDELAY)
		(void)connect_logger();
	unlock_log();
}

void
closelog(void)
{
	lock_log();
	if (descriptor >= 0) {
		close(descriptor);
		descriptor = -1;
	}
	unlock_log();
}

int
setlogmask(int new_mask)
{
	int old;
	lock_log();
	old = mask;
	if (new_mask)
		mask = new_mask;
	unlock_log();
	return old;
}

void
vsyslog(int priority, const char *format, va_list arguments)
{
	char body[768], message[1024], timestamp[32];
	struct tm broken;
	time_t now;
	int length, saved_errno = errno, console;
	lock_log();
	if (!(mask & LOG_MASK(priority & LOG_PRIMASK))) {
		unlock_log();
		return;
	}
	if (!(priority & ~LOG_PRIMASK))
		priority |= facility;
	(void)vsnprintf(body, sizeof(body), format, arguments);
	now = time(NULL);
	if (localtime_r(&now, &broken) != NULL)
		(void)strftime(timestamp, sizeof(timestamp), "%b %e %T",
			       &broken);
	else
		strcpy(timestamp, "Jan  1 00:00:00");
	if (options & LOG_PID)
		length = snprintf(message, sizeof(message),
				  "<%d>%s %s[%ld]: %s", priority, timestamp,
				  identity[0] ? identity : "zedbsd",
				  (long)getpid(), body);
	else
		length = snprintf(message, sizeof(message), "<%d>%s %s: %s",
				  priority, timestamp,
				  identity[0] ? identity : "zedbsd", body);
	if (length < 0)
		length = 0;
	if ((size_t)length >= sizeof(message))
		length = (int)sizeof(message) - 1;
	if (connect_logger() != 0 ||
	    send_message(message, (size_t)length) != 0) {
		if (options & LOG_CONS) {
			console = open("/dev/console", O_WRONLY | O_CLOEXEC);
			if (console >= 0) {
				(void)write(console, message, (size_t)length);
				(void)write(console, "\n", 1);
				close(console);
			}
		}
	}
	if (options & LOG_PERROR) {
		(void)write(STDERR_FILENO, message, (size_t)length);
		(void)write(STDERR_FILENO, "\n", 1);
	}
	unlock_log();
	errno = saved_errno;
}

void
syslog(int priority, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vsyslog(priority, format, arguments);
	va_end(arguments);
}

/*
 * Sends one message without waiting long for room at the daemon.
 *
 * A full queue is waited on for LOG_ROOM_WAIT_MS at most; a message that
 * still does not fit is dropped and counted.  After a message gets through,
 * a count of those dropped before it is sent as a line of its own.  Called
 * with log_lock held and the socket connected.  Returns 0, or -1 with errno
 * set when the message was not delivered.
 */
static int
send_message(
	const char *message,
	size_t length)
{
	struct pollfd room;
	char note[160];
	ssize_t sent;
	int written;

	/* Sends the message if the daemon has room for it now. */
	sent = send(descriptor, message, length, MSG_NOSIGNAL | MSG_DONTWAIT);

	/* Waits a little for room in a full queue, then tries once more. */
	if (sent < 0 &&
	    (errno == EAGAIN ||
	     errno == EWOULDBLOCK ||
	     errno == ENOBUFS)) {
		room.fd = descriptor;
		room.events = POLLOUT;
		room.revents = 0;
		(void)poll(&room, 1, LOG_ROOM_WAIT_MS);
		sent = send(descriptor, message, length, MSG_NOSIGNAL | MSG_DONTWAIT);
	}

	/* Counts a message the daemon had no room for. */
	if (sent < 0) {
		if (errno == EAGAIN ||
		    errno == EWOULDBLOCK ||
		    errno == ENOBUFS)
			dropped_messages++;
		return -1;
	}

	/* Reports the earlier losses after a message that got through, keeping the count if this does not. */
	if (dropped_messages != 0) {
		written = snprintf(note, sizeof(note),
				   "<%d>%s[%ld]: syslog: %lu messages were dropped because the log daemon was behind",
				   LOG_USER | LOG_WARNING,
				   identity[0] ? identity : "zedbsd",
				   (long)getpid(),
				   dropped_messages);
		if (written > 0 && (size_t)written < sizeof(note)) {
			sent = send(descriptor, note, (size_t)written, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (sent >= 0)
				dropped_messages = 0;
		}
	}

	/* Succeeded: the message is in the daemon's queue. */
	return 0;
}
