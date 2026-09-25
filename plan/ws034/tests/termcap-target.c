/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Target test of the termcap interface in the curses library (ws034-p041).
 * Answers are checked against /lib/terminfo/xterm-256color.zti, the
 * description the library loads.  Prints one PASS or FAIL line.
 */

#include <curses.h>
#include <stdio.h>
#include <string.h>
#include <term.h>

static char written[64];
static size_t written_length;

static int
collect(int c)
{
	if (written_length + 1 < sizeof(written))
		written[written_length++] = (char)c;
	written[written_length] = '\0';
	return c;
}

#define CHECK(condition, what) \
	do { if (!(condition)) { printf("TERMCAP FAIL %s\n", what); return 1; } } while (0)

int
main(void)
{
	char area[256];
	char *cursor = area;
	char *cm, *so, *string;

	CHECK(tgetent(NULL, "no-such-terminal") == 0, "unknown terminal");
	CHECK(tgetent(NULL, "xterm-256color") == 1, "tgetent");
	CHECK(tgetnum("co") == 80 && tgetnum("li") == 24, "co/li");
	CHECK(tgetnum("Co") == 256, "Co");
	CHECK(tgetflag("am") == 1, "am");
	CHECK(tgetflag("zz") == 0 && tgetnum("zz") == -1 && tgetstr("zz", NULL) == NULL,
	      "unknown code");

	/* cm is "\E[%i%p1%d;%p2%dH"; tgoto takes column then row. */
	cm = tgetstr("cm", &cursor);
	CHECK(cm != NULL && cm == area && cursor > area, "tgetstr area");
	CHECK(strcmp(tgoto(cm, 5, 2), "\033[3;6H") == 0, "tgoto");
	so = tgetstr("mr", NULL);
	CHECK(so != NULL && strcmp(so, "\033[7m") == 0, "mr");
	CHECK(tgetstr("so", NULL) == NULL, "so is not in this description");
	string = tgetstr("AF", NULL);
	CHECK(string != NULL, "AF");
	CHECK(strcmp(tparm(string, 1L, 0L, 0L, 0L, 0L, 0L, 0L, 0L, 0L), "\033[38;5;1m") == 0,
	      "tparm setaf 1");
	CHECK(strcmp(tiparm(string, 9), "\033[38;5;9m") == 0, "tiparm setaf 9");

	/* Padding is left out. */
	CHECK(tputs("a$<5>b$<10/>c", 1, collect) == OK && strcmp(written, "abc") == 0,
	      "tputs padding");
	written_length = 0;
	CHECK(tputs(tgoto(cm, 0, 0), 1, collect) == OK &&
	      strcmp(written, "\033[1;1H") == 0, "tputs cm");
	CHECK(UP != NULL && strcmp(UP, "\033[A") == 0, "UP");
	printf("TERMCAP PASS\n");
	return 0;
}
