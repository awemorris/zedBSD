/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library termios support.
 */

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

extern char *__pthread_ptsname_buffer(size_t *) __attribute__((weak));

static int speed_valid(speed_t speed);

/*
 * Implements the tcgetattr operation.
 */
int
tcgetattr(
	int descriptor,
	struct termios *termios)
{
	int function_result;

	/* Handles the termios availability. */
	if (termios == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TCGETS, termios);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcgetwinsize operation.
 */
int
tcgetwinsize(
	int descriptor,
	struct winsize *winsize)
{
	int function_result;

	/* Handles the winsize availability. */
	if (winsize == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TIOCGWINSZ, winsize);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcsetattr operation.
 */
int
tcsetattr(
	int descriptor,
	int action,
	const struct termios *termios)
{
	int function_result;
	unsigned long request;

	/* Handles the termios availability. */
	if (termios == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the action condition. */
	if (action == TCSANOW)
		request = TCSETS;
	else if (action == TCSADRAIN)
		request = TCSETSW;
	else if (action == TCSAFLUSH) {
		request = TCSETSF;
	} else {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, request, termios);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcsetwinsize operation.
 */
int
tcsetwinsize(
	int descriptor,
	const struct winsize *winsize)
{
	int function_result;

	/* Handles the winsize availability. */
	if (winsize == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TIOCSWINSZ, winsize);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcdrain operation.
 */
int
tcdrain(
	int descriptor)
{
	int function_result;

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TIOCDRAIN);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcflush operation.
 */
int
tcflush(
	int descriptor,
	int queue)
{
	int function_result;

	/* Handles the queue condition. */
	if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TIOCFLUSH, &queue);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcflow operation.
 */
int
tcflow(
	int descriptor,
	int action)
{
	int function_result;

	/* Handles the action condition. */
	if (action != TCOOFF && action != TCOON && action != TCIOFF &&
	    action != TCION) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TCXONC, &action);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the cfgetispeed operation.
 */
speed_t
cfgetispeed(
	const struct termios *termios)
{
	/* Returns the computed result. */
	return termios != NULL ? termios->c_ispeed : 0;
}

/*
 * Implements the cfgetospeed operation.
 */
speed_t
cfgetospeed(
	const struct termios *termios)
{
	/* Returns the computed result. */
	return termios != NULL ? termios->c_ospeed : 0;
}

/*
 * Implements the cfsetispeed operation.
 */
int
cfsetispeed(
	struct termios *termios,
	speed_t speed)
{
	/* Handles a failed speed valid operation. */
	if (termios == NULL || !speed_valid(speed)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	termios->c_ispeed = speed == B0 ? termios->c_ospeed : speed;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the cfsetospeed operation.
 */
int
cfsetospeed(
	struct termios *termios,
	speed_t speed)
{
	/* Handles a failed speed valid operation. */
	if (termios == NULL || !speed_valid(speed)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	termios->c_ospeed = speed;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the tcgetpgrp operation.
 */
pid_t
tcgetpgrp(
	int descriptor)
{
	pid_t function_result;
	pid_t pgrp;

	/* Computes the function result. */
	function_result = ioctl(descriptor, TIOCGPGRP, &pgrp) == 0 ? pgrp : (pid_t)-1;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcgetsid operation.
 */
pid_t
tcgetsid(
	int descriptor)
{
	pid_t function_result;
	pid_t session;

	/* Computes the function result. */
	function_result = ioctl(descriptor, TIOCGSID, &session) == 0 ? session : (pid_t)-1;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcsetpgrp operation.
 */
int
tcsetpgrp(
	int descriptor,
	pid_t pgrp)
{
	int function_result;

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TIOCSPGRP, &pgrp);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tcsendbreak operation.
 */
int
tcsendbreak(
	int descriptor,
	int duration)
{
	int function_result;

	/* Handles the duration condition. */
	if (duration < 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TCSBRK, &duration);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ctermid operation.
 */
char *
ctermid(
	char *buffer)
{
	static char path[L_ctermid] = "/dev/console";

	/* Handles the buffer availability. */
	if (buffer == NULL)
		return path;
	memcpy(buffer, path, sizeof(path));

	/* Returns the computed result. */
	return buffer;
}

/*
 * Implements the posix openpt operation.
 */
int
posix_openpt(
	int flags)
{
	int function_result;

	/* Checks the active flags. */
	if ((flags & O_ACCMODE) != O_RDWR ||
	    (flags & ~(O_ACCMODE | O_NOCTTY | O_CLOEXEC | O_NONBLOCK)) != 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the open result. */
	function_result = open("/dev/ptmx", flags);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the grantpt operation.
 */
int
grantpt(
	int descriptor)
{
	int function_result;
	uint32_t number;

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TIOCGPTN, &number);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the unlockpt operation.
 */
int
unlockpt(
	int descriptor)
{
	int function_result;
	int unlocked;

	unlocked = 0;

	/* Obtains the ioctl result. */
	function_result = ioctl(descriptor, TIOCSPTLCK, &unlocked);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the ptsname r operation.
 */
int
ptsname_r(
	int descriptor,
	char *buffer,
	size_t size)
{
	uint32_t number;
	int length;

	/* Handles the buffer availability. */
	if (buffer == NULL || size == 0) {
		errno = EINVAL;

		/* Returns the computed result. */
		return EINVAL;
	}

	/* Handles a failed ioctl operation. */
	if (ioctl(descriptor, TIOCGPTN, &number) != 0)
		return errno;
	length = snprintf(buffer, size, "/dev/pts/%u", number);

	/* Checks the current data length. */
	if (length < 0 || (size_t)length >= size) {
		errno = ERANGE;

		/* Returns the computed result. */
		return ERANGE;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the ptsname operation.
 */
char *
ptsname(
	int descriptor)
{
	char *function_result;
	static char bootstrap_buffer[32];
	size_t size;
	char *buffer = __pthread_ptsname_buffer != NULL
			   ? __pthread_ptsname_buffer(&size)
			   : bootstrap_buffer;

	size = sizeof(bootstrap_buffer);

	/* Handles the buffer availability. */
	if (buffer == NULL)
		buffer = bootstrap_buffer;

	/* Computes the function result. */
	function_result = ptsname_r(descriptor, buffer, size) == 0 ? buffer : NULL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the openpty operation.
 */
int
openpty(
	int *master,
	int *slave,
	char *name,
	const struct termios *termios,
	const struct winsize *winsize)
{
	char path[32];
	int m, s, error;

	error = 0;

	/* Handles the master availability. */
	if (master == NULL || slave == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);

	/* Handles the m condition. */
	if (m < 0)
		return -1;

	/* Handles an operation failure. */
	if (grantpt(m) != 0 || unlockpt(m) != 0 ||
	    (error = ptsname_r(m, path, sizeof(path))) != 0) {
		/* Handles an operation failure. */
		if (error != 0)
			errno = error;
		(void)close(m);

		/* Reports operation failure. */
		return -1;
	}
	s = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);

	/* Checks the current string state. */
	if (s < 0) {
		(void)close(m);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed tcsetattr operation. */
	if ((termios != NULL && tcsetattr(s, TCSANOW, termios) != 0) ||
	    (winsize != NULL && ioctl(s, TIOCSWINSZ, winsize) != 0)) {
		(void)close(s);
		(void)close(m);

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the name availability. */
	if (name != NULL)
		strcpy(name, path);
	*master = m;
	*slave = s;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the login tty operation.
 */
int
login_tty(
	int descriptor)
{
	/* Handles a failed setsid operation. */
	if (setsid() < 0 || ioctl(descriptor, TIOCSCTTY, 0) != 0 ||
	    dup2(descriptor, 0) < 0 || dup2(descriptor, 1) < 0 ||
	    dup2(descriptor, 2) < 0 || fcntl(0, F_SETFD, 0) < 0 ||
	    fcntl(1, F_SETFD, 0) < 0 || fcntl(2, F_SETFD, 0) < 0)

		/* Reports operation failure. */
		return -1;

	/* Checks the file descriptor. */
	if (descriptor > 2)
		(void)close(descriptor);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the forkpty operation.
 */
pid_t
forkpty(
	int *master,
	char *name,
	const struct termios *termios,
	const struct winsize *winsize)
{
	int slave;
	pid_t child;

	/* Handles a failed openpty operation. */
	if (openpty(master, &slave, name, termios, winsize) != 0)
		return -1;
	child = fork();

	/* Checks the child process state. */
	if (child < 0) {
		(void)close(slave);
		(void)close(*master);

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the child process state. */
	if (child == 0) {
		(void)close(*master);

		/* Handles a failed login tty operation. */
		if (login_tty(slave) != 0)
			_exit(127);

		/* Reports successful completion. */
		return 0;
	}
	(void)close(slave);

	/* Returns the computed result. */
	return child;
}

/* Supports the speed valid operation. */
static int
speed_valid(
	speed_t speed)
{
	/* Returns the computed result. */
	return speed == B0 || speed == B9600 || speed == B19200 ||
	       speed == B38400;
}
