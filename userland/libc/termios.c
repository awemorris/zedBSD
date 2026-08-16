/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>

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
	struct termios termios;
	return tcgetattr(descriptor, &termios);
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
	struct termios termios;
	if (action != TCOOFF && action != TCOON && action != TCIOFF &&
	    action != TCION) { errno = EINVAL; return -1; }
	return tcgetattr(descriptor, &termios);
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

int
tcsetpgrp(int descriptor, pid_t pgrp)
{ return ioctl(descriptor, TIOCSPGRP, &pgrp); }

char *
ctermid(char *buffer)
{
	static char path[L_ctermid] = "/dev/console";
	if (buffer == NULL) return path;
	memcpy(buffer, path, sizeof(path));
	return buffer;
}
