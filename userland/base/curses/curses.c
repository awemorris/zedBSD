/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "userland/base/curses/curses.h"

#include "userland/base/common/terminfo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static WINDOW screen;
static struct terminfo terminal;
static int terminal_ready;
static int attributes;
static struct termios saved_modes;
static struct termios active_modes;
static int modes_saved;

WINDOW *stdscr;
int LINES;
int COLS;
int COLORS;
int COLOR_PAIRS;

static const struct terminfo_capability *
capability(const char *name)
{
	if (!terminal_ready)
		return NULL;
	return terminfo_find(&terminal, name);
}

static int
emit(const char *name, long first, long second)
{
	const struct terminfo_capability *item = capability(name);
	long parameters[9] = {first, second};
	char expanded[1024];
	size_t length;

	if (item == NULL || item->kind != TERMINFO_STRING)
		return ERR;
	if (terminfo_expand(item->string, parameters, expanded,
			    sizeof(expanded)) < 0)
		return ERR;
	length = strlen(expanded);
	return fwrite(expanded, 1, length, stdout) == length ? OK : ERR;
}

int
setupterm(const char *type, int descriptor, int *error_return)
{
	const struct terminfo_capability *item;
	const char *directory = getenv("TERMINFO");

	(void)descriptor;
	if (type == NULL)
		type = getenv("TERM");
	if (type == NULL || *type == '\0' ||
	    terminfo_load(&terminal, type, directory) != 0) {
		terminal_ready = 0;
		if (error_return != NULL)
			*error_return = 0;
		return ERR;
	}
	terminal_ready = 1;
	if (error_return != NULL)
		*error_return = 1;
	item = capability("lines");
	LINES =
	    item != NULL && item->kind == TERMINFO_NUMBER ? item->number : 24;
	item = capability("cols");
	COLS =
	    item != NULL && item->kind == TERMINFO_NUMBER ? item->number : 80;
	item = capability("colors");
	COLORS =
	    item != NULL && item->kind == TERMINFO_NUMBER ? item->number : 0;
	item = capability("pairs");
	COLOR_PAIRS =
	    item != NULL && item->kind == TERMINFO_NUMBER ? item->number : 0;
	return OK;
}

WINDOW *
initscr(void)
{
	if (setupterm(NULL, STDOUT_FILENO, NULL) == ERR)
		return NULL;
	screen.cursor_y = 0;
	screen.cursor_x = 0;
	screen.max_y = LINES;
	screen.max_x = COLS;
	stdscr = &screen;
	if (tcgetattr(STDIN_FILENO, &saved_modes) == 0) {
		active_modes = saved_modes;
		modes_saved = 1;
	}
	(void)emit("smcup", 0, 0);
	(void)emit("init", 0, 0);
	return stdscr;
}

int
endwin(void)
{
	if (modes_saved)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_modes);
	(void)emit("sgr0", 0, 0);
	(void)emit("cnorm", 0, 0);
	(void)emit("rmcup", 0, 0);
	stdscr = NULL;
	return fflush(stdout) == EOF ? ERR : OK;
}

int
wrefresh(WINDOW *window)
{
	return window == NULL || fflush(stdout) == EOF ? ERR : OK;
}

int
refresh(void)
{
	return wrefresh(stdscr);
}

int
clear(void)
{
	if (stdscr == NULL || emit("clear", 0, 0) == ERR)
		return ERR;
	stdscr->cursor_y = 0;
	stdscr->cursor_x = 0;
	return OK;
}

int
erase(void)
{
	return clear();
}

int
wmove(WINDOW *window, int y, int x)
{
	if (window == NULL || y < 0 || x < 0 || y >= window->max_y ||
	    x >= window->max_x || emit("cup", y, x) == ERR)
		return ERR;
	window->cursor_y = y;
	window->cursor_x = x;
	return OK;
}

int
move(int y, int x)
{
	return wmove(stdscr, y, x);
}

int
waddch(WINDOW *window, const chtype character)
{
	unsigned char value = (unsigned char)(character & A_CHARTEXT);

	if (window == NULL || fwrite(&value, 1, 1, stdout) != 1)
		return ERR;
	if (value == '\n') {
		window->cursor_y++;
		window->cursor_x = 0;
	} else
		window->cursor_x++;
	return OK;
}

int
addch(const chtype character)
{
	return waddch(stdscr, character);
}

int
waddstr(WINDOW *window, const char *text)
{
	if (window == NULL || text == NULL)
		return ERR;
	while (*text != '\0')
		if (waddch(window, (unsigned char)*text++) == ERR)
			return ERR;
	return OK;
}

int
addstr(const char *text)
{
	return waddstr(stdscr, text);
}

int
wgetch(WINDOW *window)
{
	return window == NULL ? ERR : getchar();
}

int
getch(void)
{
	return wgetch(stdscr);
}

int
cbreak(void)
{
	if (modes_saved) {
		active_modes.c_lflag &= ~(tcflag_t)ICANON;
		active_modes.c_cc[VMIN] = 1;
		active_modes.c_cc[VTIME] = 0;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}
	return OK;
}

int
nocbreak(void)
{
	if (modes_saved) {
		active_modes.c_lflag |= ICANON;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}
	return OK;
}

int
echo(void)
{
	if (modes_saved) {
		active_modes.c_lflag |= ECHO;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}
	return OK;
}

int
noecho(void)
{
	if (modes_saved) {
		active_modes.c_lflag &= ~(tcflag_t)ECHO;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}
	return OK;
}

int
keypad(WINDOW *window, int enabled)
{
	if (window == NULL)
		return ERR;
	if (capability(enabled ? "smkx" : "rmkx") != NULL)
		return emit(enabled ? "smkx" : "rmkx", 0, 0);
	return OK;
}

int
curs_set(int visibility)
{
	if (visibility == 0)
		return emit("civis", 0, 0);
	if (visibility == 1 || visibility == 2)
		return emit("cnorm", 0, 0);
	errno = EINVAL;
	return ERR;
}

int
attrset(int new_attributes)
{
	attributes = new_attributes;
	if (emit("sgr0", 0, 0) == ERR)
		return ERR;
	if ((attributes & (int)A_BOLD) != 0)
		(void)emit("bold", 0, 0);
	if ((attributes & (int)A_REVERSE) != 0)
		(void)emit("rev", 0, 0);
	return OK;
}

int
attron(int enabled)
{
	return attrset(attributes | enabled);
}

int
attroff(int disabled)
{
	return attrset(attributes & ~disabled);
}

int
start_color(void)
{
	return has_colors() ? OK : ERR;
}

int
has_colors(void)
{
	return COLORS > 0;
}

int
init_pair(short pair, short foreground, short background)
{
	if (pair <= 0 || pair >= COLOR_PAIRS || foreground < 0 ||
	    foreground >= COLORS || background < 0 || background >= COLORS)
		return ERR;
	return OK;
}

int
tigetflag(const char *name)
{
	const struct terminfo_capability *item = capability(name);

	if (item == NULL)
		return -1;
	return item->kind == TERMINFO_BOOLEAN ? !!item->number : -1;
}

int
tigetnum(const char *name)
{
	const struct terminfo_capability *item = capability(name);

	if (item == NULL)
		return -2;
	return item->kind == TERMINFO_NUMBER ? (int)item->number : -2;
}

char *
tigetstr(const char *name)
{
	const struct terminfo_capability *item = capability(name);

	if (item == NULL)
		return (char *)-1;
	return item->kind == TERMINFO_STRING ? (char *)item->string
					     : (char *)-1;
}

int
putp(const char *text)
{
	size_t length;

	if (text == NULL)
		return ERR;
	length = strlen(text);
	return fwrite(text, 1, length, stdout) == length ? OK : ERR;
}
