/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <string.h>
#include <termios.h>
int main(int argc, char **argv)
{
	struct termios t; int i;
	if (tcgetattr(0, &t)) { command_error("stty", NULL); return 1; }
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-a"))) {
		printf("speed %u baud; ispeed %u baud;\n", (unsigned)cfgetospeed(&t),
		    (unsigned)cfgetispeed(&t));
		printf("%sicanon %secho %sisig %siexten %sixon %sopost\n",
		    t.c_lflag & ICANON ? "" : "-", t.c_lflag & ECHO ? "" : "-",
		    t.c_lflag & ISIG ? "" : "-", t.c_lflag & IEXTEN ? "" : "-",
		    t.c_iflag & IXON ? "" : "-", t.c_oflag & OPOST ? "" : "-");
		return 0;
	}
	for (i = 1; i < argc; ++i) {
		int on = argv[i][0] != '-'; const char *n = on ? argv[i] : argv[i] + 1;
		if (!strcmp(n, "echo")) { if (on) t.c_lflag |= ECHO; else t.c_lflag &= ~ECHO; }
		else if (!strcmp(n, "icanon")) { if (on) t.c_lflag |= ICANON; else t.c_lflag &= ~ICANON; }
		else if (!strcmp(n, "isig")) { if (on) t.c_lflag |= ISIG; else t.c_lflag &= ~ISIG; }
		else if (!strcmp(n, "ixon")) { if (on) t.c_iflag |= IXON; else t.c_iflag &= ~IXON; }
		else if (!strcmp(n, "opost")) { if (on) t.c_oflag |= OPOST; else t.c_oflag &= ~OPOST; }
		else if (!strcmp(n, "raw")) {
			t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN); t.c_iflag = 0;
			t.c_oflag = 0; t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
		} else { fprintf(stderr, "stty: invalid argument: %s\n", argv[i]); return 2; }
	}
	if (tcsetattr(0, TCSADRAIN, &t)) { command_error("stty", NULL); return 1; }
	return 0;
}
