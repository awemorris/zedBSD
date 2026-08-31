/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD userland curses component.
 */

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

static const struct terminfo_capability *capability(const char *name);
static int emit(const char *name, long first, long second);

/*
 * Implements the setupterm operation.
 */
int
setupterm(
	const char *type,
	int descriptor,
	int *error_return)
{
	const struct terminfo_capability *item;
	const char *directory;

	directory = getenv("TERMINFO");

	(void)descriptor;

	/* Handles the type availability. */
	if (type == NULL)
		type = getenv("TERM");

	/* Handles a failed terminfo load operation. */
	if (type == NULL || *type == '\0' ||
	    terminfo_load(&terminal, type, directory) != 0) {
		terminal_ready = 0;

		/* Handles an operation failure. */
		if (error_return != NULL)
			*error_return = 0;
		/* Returns the computed result. */
		return ERR;
	}
	terminal_ready = 1;

	/* Handles an operation failure. */
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

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the initscr operation.
 */
WINDOW *
initscr(
	void)
{
	/* Handles a failed setupterm operation. */
	if (setupterm(NULL, STDOUT_FILENO, NULL) == ERR)
		return NULL;
	screen.cursor_y = 0;
	screen.cursor_x = 0;
	screen.max_y = LINES;
	screen.max_x = COLS;
	stdscr = &screen;

	/* Handles a failed tcgetattr operation. */
	if (tcgetattr(STDIN_FILENO, &saved_modes) == 0) {
		active_modes = saved_modes;
		modes_saved = 1;
	}
	(void)emit("smcup", 0, 0);
	(void)emit("init", 0, 0);

	/* Returns the computed result. */
	return stdscr;
}

/*
 * Implements the endwin operation.
 */
int
endwin(
	void)
{
	int function_result;

	/* Handles the modes saved condition. */
	if (modes_saved)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_modes);
	(void)emit("sgr0", 0, 0);
	(void)emit("cnorm", 0, 0);
	(void)emit("rmcup", 0, 0);
	stdscr = NULL;

	/* Computes the function result. */
	function_result = fflush(stdout) == EOF ? ERR : OK;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the wrefresh operation.
 */
int
wrefresh(
	WINDOW *window)
{
	int function_result;

	/* Computes the function result. */
	function_result = window == NULL || fflush(stdout) == EOF ? ERR : OK;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the refresh operation.
 */
int
refresh(
	void)
{
	int function_result;

	/* Obtains the wrefresh result. */
	function_result = wrefresh(stdscr);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the clear operation.
 */
int
clear(
	void)
{
	/* Handles a failed emit operation. */
	if (stdscr == NULL || emit("clear", 0, 0) == ERR)
		return ERR;
	stdscr->cursor_y = 0;
	stdscr->cursor_x = 0;

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the erase operation.
 */
int
erase(
	void)
{
	int function_result;

	/* Obtains the clear result. */
	function_result = clear();

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the wmove operation.
 */
int
wmove(
	WINDOW *window,
	int y,
	int x)
{
	/* Handles a failed emit operation. */
	if (window == NULL || y < 0 || x < 0 || y >= window->max_y ||
	    x >= window->max_x || emit("cup", y, x) == ERR)

		/* Returns the computed result. */
		return ERR;
	window->cursor_y = y;
	window->cursor_x = x;

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the move operation.
 */
int
move(
	int y,
	int x)
{
	int function_result;

	/* Obtains the wmove result. */
	function_result = wmove(stdscr, y, x);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the waddch operation.
 */
int
waddch(
	WINDOW *window,
	const chtype character)
{
	unsigned char value;

	value = (unsigned char)(character & A_CHARTEXT);

	/* Handles a failed fwrite operation. */
	if (window == NULL || fwrite(&value, 1, 1, stdout) != 1)
		return ERR;

	/* Validates the current value. */
	if (value == '\n') {
		window->cursor_y++;
		window->cursor_x = 0;
	} else {
		window->cursor_x++;
	}

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the addch operation.
 */
int
addch(
	const chtype character)
{
	int function_result;

	/* Obtains the waddch result. */
	function_result = waddch(stdscr, character);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the waddstr operation.
 */
int
waddstr(
	WINDOW *window,
	const char *text)
{
	/* Handles the window availability. */
	if (window == NULL || text == NULL)
		return ERR;

	/* Continue while the operation condition remains true. */
	while (*text != '\0') {
		/* Handles a failed waddch operation. */
		if (waddch(window, (unsigned char)*text++) == ERR)
			return ERR;
	}

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the addstr operation.
 */
int
addstr(
	const char *text)
{
	int function_result;

	/* Obtains the waddstr result. */
	function_result = waddstr(stdscr, text);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the wgetch operation.
 */
int
wgetch(
	WINDOW *window)
{
	int function_result;

	/* Computes the function result. */
	function_result = window == NULL ? ERR : getchar();

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the getch operation.
 */
int
getch(
	void)
{
	int function_result;

	/* Obtains the wgetch result. */
	function_result = wgetch(stdscr);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the cbreak operation.
 */
int
cbreak(
	void)
{
	/* Handles the modes saved condition. */
	if (modes_saved) {
		active_modes.c_lflag &= ~(tcflag_t)ICANON;
		active_modes.c_cc[VMIN] = 1;
		active_modes.c_cc[VTIME] = 0;

		/* Handles a failed tcsetattr operation. */
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the nocbreak operation.
 */
int
nocbreak(
	void)
{
	/* Handles the modes saved condition. */
	if (modes_saved) {
		active_modes.c_lflag |= ICANON;

		/* Handles a failed tcsetattr operation. */
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the echo operation.
 */
int
echo(
	void)
{
	/* Handles the modes saved condition. */
	if (modes_saved) {
		active_modes.c_lflag |= ECHO;

		/* Handles a failed tcsetattr operation. */
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the noecho operation.
 */
int
noecho(
	void)
{
	/* Handles the modes saved condition. */
	if (modes_saved) {
		active_modes.c_lflag &= ~(tcflag_t)ECHO;

		/* Handles a failed tcsetattr operation. */
		if (tcsetattr(STDIN_FILENO, TCSANOW, &active_modes) != 0)
			return ERR;
	}

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the keypad operation.
 */
int
keypad(
	WINDOW *window,
	int enabled)
{
	int function_result;

	/* Handles the window availability. */
	if (window == NULL)
		return ERR;

	/* Handles a failed capability operation. */
	if (capability(enabled ? "smkx" : "rmkx") != NULL) {
		/* Obtains the emit result. */
		function_result = emit(enabled ? "smkx" : "rmkx", 0, 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the curs set operation.
 */
int
curs_set(
	int visibility)
{
	int function_result;

	/* Handles the visibility condition. */
	if (visibility == 0) {
		/* Obtains the emit result. */
		function_result = emit("civis", 0, 0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the visibility condition. */
	if (visibility == 1 || visibility == 2) {
		/* Obtains the emit result. */
		function_result = emit("cnorm", 0, 0);

		/* Returns the computed result. */
		return function_result;
	}

	errno = EINVAL;

	/* Returns the computed result. */
	return ERR;
}

/*
 * Implements the attrset operation.
 */
int
attrset(
	int new_attributes)
{
	attributes = new_attributes;

	/* Handles a failed emit operation. */
	if (emit("sgr0", 0, 0) == ERR)
		return ERR;

	/* Handles the attributes condition. */
	if ((attributes & (int)A_BOLD) != 0)
		(void)emit("bold", 0, 0);

	/* Handles the attributes condition. */
	if ((attributes & (int)A_REVERSE) != 0)
		(void)emit("rev", 0, 0);

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the attron operation.
 */
int
attron(
	int enabled)
{
	int function_result;

	/* Obtains the attrset result. */
	function_result = attrset(attributes | enabled);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the attroff operation.
 */
int
attroff(
	int disabled)
{
	int function_result;

	/* Obtains the attrset result. */
	function_result = attrset(attributes & ~disabled);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the start color operation.
 */
int
start_color(
	void)
{
	int function_result;

	/* Computes the function result. */
	function_result = has_colors() ? OK : ERR;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the has colors operation.
 */
int
has_colors(
	void)
{
	/* Returns the computed result. */
	return COLORS > 0;
}

/*
 * Implements the init pair operation.
 */
int
init_pair(
	short pair,
	short foreground,
	short background)
{
	/* Handles the pair condition. */
	if (pair <= 0 || pair >= COLOR_PAIRS || foreground < 0 ||
	    foreground >= COLORS || background < 0 || background >= COLORS)

		/* Returns the computed result. */
		return ERR;

	/* Returns the computed result. */
	return OK;
}

/*
 * Implements the tigetflag operation.
 */
int
tigetflag(
	const char *name)
{
	const struct terminfo_capability *item;

	item = capability(name);

	/* Handles the item availability. */
	if (item == NULL)
		return -1;

	/* Returns the computed result. */
	return item->kind == TERMINFO_BOOLEAN ? !!item->number : -1;
}

/*
 * Implements the tigetnum operation.
 */
int
tigetnum(
	const char *name)
{
	const struct terminfo_capability *item;

	item = capability(name);

	/* Handles the item availability. */
	if (item == NULL)
		return -2;

	/* Returns the computed result. */
	return item->kind == TERMINFO_NUMBER ? (int)item->number : -2;
}

/*
 * Implements the tigetstr operation.
 */
char *
tigetstr(
	const char *name)
{
	const struct terminfo_capability *item;

	item = capability(name);

	/* Handles the item availability. */
	if (item == NULL)
		return (char *)-1;

	/* Returns the computed result. */
	return item->kind == TERMINFO_STRING ? (char *)item->string
					     : (char *)-1;
}

/*
 * Implements the putp operation.
 */
int
putp(
	const char *text)
{
	int function_result;
	size_t length;

	/* Handles the text availability. */
	if (text == NULL)
		return ERR;
	length = strlen(text);

	/* Computes the function result. */
	function_result = fwrite(text, 1, length, stdout) == length ? OK : ERR;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the capability operation. */
static const struct terminfo_capability *
capability(
	const char *name)
{
	const struct terminfo_capability *function_result;

	/* Handles the terminal ready condition. */
	if (!terminal_ready)
		return NULL;

	/* Obtains the terminfo find result. */
	function_result = terminfo_find(&terminal, name);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the emit operation. */
static int
emit(
	const char *name,
	long first,
	long second)
{
	int function_result;
	const struct terminfo_capability *item;
	long parameters[9] = {first, second};
	char expanded[1024];
	size_t length;

	item = capability(name);

	/* Handles the item availability. */
	if (item == NULL || item->kind != TERMINFO_STRING)
		return ERR;

	/* Handles a failed terminfo expand operation. */
	if (terminfo_expand(item->string, parameters, expanded,
			    sizeof(expanded)) < 0)

		/* Returns the computed result. */
		return ERR;
	length = strlen(expanded);

	/* Computes the function result. */
	function_result = fwrite(expanded, 1, length, stdout) == length ? OK : ERR;

	/* Returns the computed result. */
	return function_result;
}
