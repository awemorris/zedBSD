/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/curses/curses.h"

#include <stdio.h>

int
main(void)
{
	if (initscr() == NULL || COLS != 90 || LINES != 30 ||
	    tigetflag("am") != 1 || tigetnum("colors") != 8)
		return 1;
	if (clear() == ERR || move(1, 2) == ERR ||
	    addstr("zedBSD-POSIX-PHASE85-CURSES-PASS\n") == ERR ||
	    refresh() == ERR || endwin() == ERR)
		return 1;
	return 0;
}
