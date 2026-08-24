/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <utmpx.h>

static void
record_login_process(
	const char *device)
{
	struct utmpx record;
	const char *line = strrchr(device, '/');

	memset(&record, 0, sizeof(record));
	record.ut_type = LOGIN_PROCESS;
	record.ut_pid = getpid();
	line = line != NULL ? line + 1 : device;
	strncpy(record.ut_line, line, sizeof(record.ut_line) - 1);
	strncpy(record.ut_id, line, sizeof(record.ut_id));
	(void)pututxline(&record);
}

int
main(
	int argc,
	char **argv)
{
	char path[96], hostname[256];
	struct termios attributes;
	int descriptor;
	char *login_arguments[] = {"login", NULL};

	if (argc != 2 || strchr(argv[1], '/') != NULL) {
		fprintf(stderr, "usage: getty device\n");
		return 2;
	}

	if (snprintf(path, sizeof(path), "/dev/%s", argv[1]) >= (int)sizeof(path)) {
		fprintf(stderr, "getty: device name is too long\n");
		return 2;
	}

	descriptor = open(path, O_RDWR | O_NOCTTY);
	if (descriptor < 0) {
		fprintf(stderr, "getty: %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (tcgetattr(descriptor, &attributes) == 0) {
		attributes.c_iflag = BRKINT | ICRNL | IXON;
		attributes.c_oflag = OPOST | ONLCR;
		attributes.c_cflag = CREAD | CS8 | CLOCAL;
		attributes.c_lflag =
		    ECHO | ECHOE | ECHOK | ICANON | IEXTEN | ISIG;
		attributes.c_cc[VINTR] = 3;
		attributes.c_cc[VQUIT] = 28;
		attributes.c_cc[VERASE] = 127;
		attributes.c_cc[VKILL] = 21;
		attributes.c_cc[VEOF] = 4;
		attributes.c_cc[VMIN] = 1;
		attributes.c_cc[VTIME] = 0;
		(void)cfsetispeed(&attributes, B38400);
		(void)cfsetospeed(&attributes, B38400);
		(void)tcsetattr(descriptor, TCSAFLUSH, &attributes);
	}

	if (login_tty(descriptor) != 0) {
		fprintf(stderr, "getty: cannot acquire %s: %s\n", path,
			strerror(errno));
		return 1;
	}

	record_login_process(path);
	if (gethostname(hostname, sizeof(hostname)) != 0)
		strcpy(hostname, "zedbsd");

	hostname[sizeof(hostname) - 1] = '\0';

//	printf("\n%s console\n\n", hostname);
	printf("\n");

	execv("/bin/login", login_arguments);

	fprintf(stderr, "getty: /bin/login: %s\n", strerror(errno));

	return 1;
}
