/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

int
tcgetattr(int descriptor, struct termios *termios)
{
	if (termios == NULL) { errno = EINVAL; return -1; }
	return ioctl(descriptor, TCGETS, termios);
}

int
tcsetattr(int descriptor, int action, const struct termios *termios)
{
	unsigned long request;
	if (termios == NULL) { errno = EINVAL; return -1; }
	if (action == TCSANOW) request = TCSETS;
	else if (action == TCSADRAIN) request = TCSETSW;
	else if (action == TCSAFLUSH) request = TCSETSF;
	else { errno = EINVAL; return -1; }
	return ioctl(descriptor, request, termios);
}

int
tcdrain(int descriptor)
{
	return ioctl(descriptor, TIOCDRAIN);
}

int
tcflush(int descriptor, int queue)
{
	if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH) {
		errno = EINVAL; return -1;
	}
	return ioctl(descriptor, TIOCFLUSH, &queue);
}

int
tcflow(int descriptor, int action)
{
	if (action != TCOOFF && action != TCOON && action != TCIOFF &&
	    action != TCION) { errno = EINVAL; return -1; }
	return ioctl(descriptor, TCXONC, &action);
}

speed_t cfgetispeed(const struct termios *termios)
{ return termios != NULL ? termios->c_ispeed : 0; }
speed_t cfgetospeed(const struct termios *termios)
{ return termios != NULL ? termios->c_ospeed : 0; }

static int
speed_valid(speed_t speed)
{ return speed == B0 || speed == B9600 || speed == B19200 || speed == B38400; }

int
cfsetispeed(struct termios *termios, speed_t speed)
{
	if (termios == NULL || !speed_valid(speed)) { errno = EINVAL; return -1; }
	termios->c_ispeed = speed == B0 ? termios->c_ospeed : speed;
	return 0;
}

int
cfsetospeed(struct termios *termios, speed_t speed)
{
	if (termios == NULL || !speed_valid(speed)) { errno = EINVAL; return -1; }
	termios->c_ospeed = speed;
	return 0;
}

pid_t
tcgetpgrp(int descriptor)
{
	pid_t pgrp;
	return ioctl(descriptor, TIOCGPGRP, &pgrp) == 0 ? pgrp : (pid_t)-1;
}

pid_t
tcgetsid(int descriptor)
{
	pid_t session;
	return ioctl(descriptor, TIOCGSID, &session) == 0 ? session : (pid_t)-1;
}

int
tcsetpgrp(int descriptor, pid_t pgrp)
{ return ioctl(descriptor, TIOCSPGRP, &pgrp); }

int
tcsendbreak(int descriptor, int duration)
{
	if (duration < 0) { errno = EINVAL; return -1; }
	return ioctl(descriptor, TCSBRK, &duration);
}

char *
ctermid(char *buffer)
{
	static char path[L_ctermid] = "/dev/console";
	if (buffer == NULL) return path;
	memcpy(buffer, path, sizeof(path));
	return buffer;
}

int
posix_openpt(int flags)
{
	if ((flags & O_ACCMODE) != O_RDWR ||
	    (flags & ~(O_ACCMODE | O_NOCTTY | O_CLOEXEC | O_NONBLOCK)) != 0) {
		errno = EINVAL;
		return -1;
	}
	return open("/dev/ptmx", flags);
}

int
grantpt(int descriptor)
{
	uint32_t number;
	return ioctl(descriptor, TIOCGPTN, &number);
}

int
unlockpt(int descriptor)
{
	int unlocked = 0;
	return ioctl(descriptor, TIOCSPTLCK, &unlocked);
}

int
ptsname_r(int descriptor, char *buffer, size_t size)
{
	uint32_t number;
	int length;
	if (buffer == NULL || size == 0) {
		errno = EINVAL;
		return EINVAL;
	}
	if (ioctl(descriptor, TIOCGPTN, &number) != 0)
		return errno;
	length = snprintf(buffer, size, "/dev/pts/%u", number);
	if (length < 0 || (size_t)length >= size) {
		errno = ERANGE;
		return ERANGE;
	}
	return 0;
}

char *
ptsname(int descriptor)
{
	static char bootstrap_buffer[32];
	size_t size = sizeof(bootstrap_buffer);
	char *buffer = __pthread_ptsname_buffer != NULL ?
	    __pthread_ptsname_buffer(&size) : bootstrap_buffer;
	if (buffer == NULL)
		buffer = bootstrap_buffer;
	return ptsname_r(descriptor, buffer, size) == 0 ? buffer : NULL;
}

int
openpty(int *master, int *slave, char *name, const struct termios *termios,
	const struct winsize *winsize)
{
	char path[32];
	int m, s, error = 0;
	if (master == NULL || slave == NULL) {
		errno = EINVAL;
		return -1;
	}
	m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (m < 0)
		return -1;
	if (grantpt(m) != 0 || unlockpt(m) != 0 ||
	    (error = ptsname_r(m, path, sizeof(path))) != 0) {
		if (error != 0) errno = error;
		(void)close(m);
		return -1;
	}
	s = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (s < 0) {
		(void)close(m);
		return -1;
	}
	if ((termios != NULL && tcsetattr(s, TCSANOW, termios) != 0) ||
	    (winsize != NULL && ioctl(s, TIOCSWINSZ, winsize) != 0)) {
		(void)close(s);
		(void)close(m);
		return -1;
	}
	if (name != NULL)
		strcpy(name, path);
	*master = m;
	*slave = s;
	return 0;
}

int
login_tty(int descriptor)
{
	if (setsid() < 0 || ioctl(descriptor, TIOCSCTTY, 0) != 0 ||
	    dup2(descriptor, 0) < 0 || dup2(descriptor, 1) < 0 ||
	    dup2(descriptor, 2) < 0 ||
	    fcntl(0, F_SETFD, 0) < 0 || fcntl(1, F_SETFD, 0) < 0 ||
	    fcntl(2, F_SETFD, 0) < 0)
		return -1;
	if (descriptor > 2)
		(void)close(descriptor);
	return 0;
}

pid_t
forkpty(int *master, char *name, const struct termios *termios,
	const struct winsize *winsize)
{
	int slave;
	pid_t child;
	if (openpty(master, &slave, name, termios, winsize) != 0)
		return -1;
	child = fork();
	if (child < 0) {
		(void)close(slave);
		(void)close(*master);
		return -1;
	}
	if (child == 0) {
		(void)close(*master);
		if (login_tty(slave) != 0)
			_exit(127);
		return 0;
	}
	(void)close(slave);
	return child;
}
