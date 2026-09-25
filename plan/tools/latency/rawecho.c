/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * ws041: echoes each byte of a raw terminal until 'q' (rawecho), or spins
 * forever (rawecho spin), to time the echo apart from the shell.
 */
#include <string.h>
#include <termios.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	struct termios saved;
	struct termios raw;
	volatile unsigned long spin;
	char byte;

	if (argc > 1 && strcmp(argv[1], "spin") == 0) {
		for (spin = 0;; spin++)
			;
	}
	tcgetattr(0, &saved);
	raw = saved;
	raw.c_lflag &= ~(ICANON | ECHO | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	while (read(0, &byte, 1) == 1 && byte != 'q')
		(void)write(1, &byte, 1);
	tcsetattr(0, TCSANOW, &saved);
	return 0;
}
