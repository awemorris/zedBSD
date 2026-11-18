/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zterm - compact Unicode VT100 terminal for Xzed
 *
 * Ctrl+Shift+C copies the screen's text to CLIPBOARD (zterm owns it and
 * answers the requests for it); Ctrl+Shift+V pastes CLIPBOARD's text into
 * the shell.  Through xserver's bridge the desktop's clipboard is
 * CLIPBOARD too (ws035-p087).
 *
 * A double click selects the word under the pointer as PRIMARY, and the
 * middle button pastes PRIMARY's text; through the bridge that is the
 * desktop's primary selection (ws035-p103).
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "userland/x11/libX11/Xzed.h"
#include <X11/keysym.h>

#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CELL_WIDTH 8U
#define CELL_HEIGHT 16U

/* The pointer's buttons as the event's detail has them, and a double click's longest gap (ms). */
#define BUTTON_LEFT 1U
#define BUTTON_MIDDLE 2U
#define DOUBLE_CLICK_MS 400UL
#define MAX_COLUMNS 160U
#define MAX_ROWS 64U
#define CSI_PARAMETERS 8

struct cell {
	uint32_t codepoint;
	uint32_t foreground;
	uint32_t background;
	/* The second cell occupied by a wide glyph has no glyph of its own. */
	uint8_t continuation;
};

struct terminal {
	Display *display;
	Window window;
	GC gc;
	XFontStruct *font;
	int master;
	pid_t child;
	unsigned columns;
	unsigned rows;
	unsigned cursor_column;
	unsigned cursor_row;
	unsigned saved_column;
	unsigned saved_row;
	uint32_t foreground;
	uint32_t background;
	struct cell cells[MAX_COLUMNS * MAX_ROWS];
	/* Each row tracks the smallest damaged inclusive column range. */
	uint8_t dirty[MAX_ROWS];
	uint16_t dirty_first[MAX_ROWS];
	uint16_t dirty_last[MAX_ROWS];
	unsigned drawn_cursor_column;
	unsigned drawn_cursor_row;
	int cursor_drawn;
	int parser_state;
	int parameters[CSI_PARAMETERS];
	int parameter_index;
	uint32_t utf8_value;
	uint32_t utf8_minimum;
	unsigned utf8_remaining;
	int request_budget;

	/* The clipboard: its atoms, the text zterm owns CLIPBOARD with (NULL when it does not), and its length. */
	Atom atom_clipboard;
	Atom atom_utf8;
	Atom atom_targets;
	Atom atom_paste;
	char *clip;
	size_t clip_length;

	/* PRIMARY (ws035-p103): the word zterm owns it with (NULL when it does not), and the last left press (for a double click). */
	char *primary;
	size_t primary_length;
	Time click_time;
	unsigned click_column;
	unsigned click_row;
};

static const uint32_t ansi_colors[16] = {
    0x000000, 0xaa0000, 0x00aa00, 0xaa5500, 0x0000aa, 0xaa00aa,
    0x00aaaa, 0xc0c0c0, 0x555555, 0xff5555, 0x55ff55, 0xffff55,
    0x5555ff, 0xff55ff, 0x55ffff, 0xffffff};

static int initialize(struct terminal *terminal, unsigned want_columns, unsigned want_rows);
static int geometry(int argc, char **argv, unsigned *columns, unsigned *rows);
static void clear_screen(struct terminal *terminal);
static void erase_range(struct terminal *terminal, unsigned row, unsigned first, unsigned last);
static void blank_cell(struct terminal *terminal, unsigned column, unsigned row);
static struct cell *cell_at(struct terminal *terminal, unsigned column, unsigned row);
static void damage(struct terminal *terminal, unsigned row, unsigned first, unsigned last);
static void terminal_message(struct terminal *terminal, const char *message);
static void terminal_byte(struct terminal *terminal, unsigned char byte);
static void csi_dispatch(struct terminal *terminal, unsigned char final);
static int parameter(const struct terminal *terminal, int index, int fallback);
static void line_feed(struct terminal *terminal);
static void scroll_up(struct terminal *terminal);
static void damage_all(struct terminal *terminal);
static void utf8_byte(struct terminal *terminal, unsigned char byte);
static void put_codepoint(struct terminal *terminal, uint32_t codepoint);
static int wide_codepoint(uint32_t codepoint);
static void redraw(struct terminal *terminal);
static void draw_row(struct terminal *terminal, unsigned row, unsigned first, unsigned last);
static void x_request(struct terminal *terminal);
static void x_finish(struct terminal *terminal);
static int resize_terminal(struct terminal *terminal, unsigned width, unsigned height);
static int send_key(struct terminal *terminal, XKeyEvent *event);
static void clip_copy(struct terminal *terminal);
static void clip_paste(struct terminal *terminal);
static void clip_request(struct terminal *terminal, const XSelectionRequestEvent *request);
static void clip_notify(struct terminal *terminal, const XSelectionEvent *notify);
static size_t clip_utf8(uint32_t codepoint, char *out);
static void button_press(struct terminal *terminal, const XButtonEvent *event);
static void primary_select(struct terminal *terminal, unsigned column, unsigned row);
static int word_cell(struct terminal *terminal, unsigned column, unsigned row);

/*
 * Runs the zterm command: zterm [-geometry COLUMNSxROWS] (without it, the
 * root window's size less a margin).
 */
int
main(
	int argc,
	char **argv)
{
	ssize_t i;
	int received;
	XEvent event;
	char message[96];
	int status;
	int result;
	uint8_t input[4096];
	ssize_t count;
	struct terminal terminal;
	struct pollfd descriptors[2];
	unsigned want_columns;
	unsigned want_rows;
	int running;

	running = 1;

	/* The size asked for, if any. */
	status = geometry(argc, argv, &want_columns, &want_rows);
	if (status != 0) {
		fprintf(stderr, "usage: zterm [-geometry COLUMNSxROWS]\n");
		return 2;
	}

	/* Handles a failed initialize operation. */
	if (initialize(&terminal, want_columns, want_rows) != 0) {
		fprintf(stderr, "zterm: initialization failed: %s\n",
			strerror(errno));

		/* Reports operation failure. */
		return 1;
	}
	while (running) {
		/*
 * Drive X events and shell output from the same nonblocking
		 * loop. */
		descriptors[0] = (struct pollfd){
		    ConnectionNumber(terminal.display), POLLIN, 0};
		descriptors[1] = (struct pollfd){terminal.master, POLLIN, 0};
		result = poll(descriptors, 2, 100);

		/* Handles the reported system error. */
		if (result < 0 && errno != EINTR)
			break;

		/* Checks the terminal state. */
		if (terminal.master >= 0 &&
		    (descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
			received = 0;
			count = read(terminal.master, input, sizeof(input));

			/* Checks the remaining item count. */
			if (count > 0) {
				/* Process each remaining element. */
				received = 1;
				for (i = 0; i < count; i++)
					terminal_byte(&terminal, input[i]);
			}

			/* Handles the received condition. */
			if (received)
				redraw(&terminal);
		}
		while (XPending(terminal.display)) {
			/* Handles a failed XNextEvent operation. */
			if (XNextEvent(terminal.display, &event) < 0) {
				running = 0;
				break;
			}

			/* Handles the event condition. */
			if (event.type == Expose) {
				damage_all(&terminal);
				redraw(&terminal);
			} else if (event.type == ConfigureNotify) {
				/* Handles a failed resize terminal operation. */
				if (resize_terminal(
					&terminal,
					(unsigned)event.xconfigure.width,
					(unsigned)event.xconfigure.height) == 0)
					redraw(&terminal);
			} else if (event.type == KeyPress) {
				(void)send_key(&terminal, &event.xkey);
			} else if (event.type == ButtonPress) {
				button_press(&terminal, &event.xbutton);
			} else if (event.type == SelectionRequest) {
				clip_request(&terminal, &event.xselectionrequest);
			} else if (event.type == SelectionNotify) {
				clip_notify(&terminal, &event.xselection);
			} else if (event.type == SelectionClear && event.xselectionclear.selection == XA_PRIMARY) {
				free(terminal.primary);
				terminal.primary = NULL;
				terminal.primary_length = 0;
			} else if (event.type == SelectionClear) {
				free(terminal.clip);
				terminal.clip = NULL;
				terminal.clip_length = 0;
			}
		}

		/* Handles a failed waitpid operation. */
		if (terminal.child > 0 && waitpid(terminal.child, &status,
						  WNOHANG) == terminal.child) {
			/* Checks the operation status. */
			if (WIFSIGNALED(status)) {
				snprintf(message, sizeof(message),
					 "\r\nzterm: /bin/sh terminated by "
					 "signal %d\r\n",
					 WTERMSIG(status));
			} else {
				snprintf(message, sizeof(message),
					 "\r\nzterm: /bin/sh exited (%d)\r\n",
					 WIFEXITED(status) ? WEXITSTATUS(status)
							   : -1);
			}
			terminal_message(&terminal, message);
			redraw(&terminal);
			close(terminal.master);
			terminal.master = -1;
			terminal.child = -1;
		}
	}

	/* Checks the terminal state. */
	if (terminal.master >= 0)
		close(terminal.master);
	XDestroyWindow(terminal.display, terminal.window);
	XFreeGC(terminal.display, terminal.gc);
	XFreeFont(terminal.display, terminal.font);
	XCloseDisplay(terminal.display);

	/* Reports successful completion. */
	return 0;
}

/* Supports the initialize operation. */
static int
initialize(
	struct terminal *terminal,
	unsigned want_columns,
	unsigned want_rows)
{
	char message_local[96];
	char message_local1[96];
	XClassHint class_hint;
	int saved;
	Window root_return;
	Window root;
	unsigned root_width, root_height, border, depth;
	unsigned width, height;
	int x, y;
	struct winsize winsize;
	char *arguments[2];

	extern char **environ;

	memset(terminal, 0, sizeof(*terminal));
	terminal->master = -1;
	terminal->foreground = 0xdcdde5;
	terminal->display = XOpenDisplay(NULL);

	/* Handles the display availability. */
	if (terminal->display == NULL)
		return -1;
	root = DefaultRootWindow(terminal->display);

	/* Handles a failed XGetGeometry operation. */
	if (!XGetGeometry(terminal->display, root, &root_return, &x, &y,
			  &root_width, &root_height, &border, &depth))

		/* Reports operation failure. */
		return -1;
	width = root_width > 40U ? root_width - 40U : root_width;
	height = root_height > 80U ? root_height - 80U : root_height;
	width = (width / CELL_WIDTH) * CELL_WIDTH;
	height = (height / CELL_HEIGHT) * CELL_HEIGHT;
	terminal->columns = width / CELL_WIDTH;
	terminal->rows = height / CELL_HEIGHT;

	/* Checks the terminal state. */
	if (terminal->columns > MAX_COLUMNS)
		terminal->columns = MAX_COLUMNS;

	/* Checks the terminal state. */
	if (terminal->rows > MAX_ROWS)
		terminal->rows = MAX_ROWS;

	/* The size asked for (-geometry), within the root. */
	if (want_columns != 0U && want_columns < terminal->columns)
		terminal->columns = want_columns;
	if (want_rows != 0U && want_rows < terminal->rows)
		terminal->rows = want_rows;
	width = terminal->columns * CELL_WIDTH;
	height = terminal->rows * CELL_HEIGHT;
	clear_screen(terminal);
	terminal->window = XCreateSimpleWindow(terminal->display, root, 20, 8,
					       width, height, 0, 0, 0x000000);
	XStoreName(terminal->display, terminal->window, "zterm");

	/* The class the desktop knows the window's application by. */
	class_hint.res_name = "zterm";
	class_hint.res_class = "XTerminal";
	(void)XSetClassHint(terminal->display, terminal->window, &class_hint);
	XzedSetIconPath(terminal->display, terminal->window,
			"/usr/share/zterm/icons/app-icon.xpm");
	terminal->gc = XCreateGC(terminal->display, terminal->window, 0, NULL);
	terminal->font = XLoadQueryFont(terminal->display, "zed-unicode");

	/* Handles the font availability. */
	if (terminal->font == NULL)
		return -1;
	XSetFont(terminal->display, terminal->gc, terminal->font->fid);
	terminal->atom_clipboard = XInternAtom(terminal->display, "CLIPBOARD", False);
	terminal->atom_utf8 = XInternAtom(terminal->display, "UTF8_STRING", False);
	terminal->atom_targets = XInternAtom(terminal->display, "TARGETS", False);
	terminal->atom_paste = XInternAtom(terminal->display, "ZTERM_PASTE", False);
	XSelectInput(terminal->display, terminal->window,
		     ExposureMask | KeyPressMask | ButtonPressMask |
			 StructureNotifyMask);
	XMapWindow(terminal->display, terminal->window);
	XSync(terminal->display, False);
	memset(&winsize, 0, sizeof(winsize));
	winsize.ws_row = (unsigned short)terminal->rows;
	winsize.ws_col = (unsigned short)terminal->columns;

	/*
 * forkpty supplies both the shell's controlling terminal and our PTY
	 * master. */
	terminal->child = forkpty(&terminal->master, NULL, NULL, &winsize);

	/* Checks the terminal state. */
	if (terminal->child < 0) {
		snprintf(message_local, sizeof(message_local),
			 "zterm: cannot start /bin/sh: %s\r\n",
			 strerror(errno));
		terminal_message(terminal, message_local);
		damage_all(terminal);

		/* Reports successful completion. */
		return 0;
	}

	/* Checks the terminal state. */
	if (terminal->child == 0) {
		arguments[0] = "/bin/sh";
		arguments[1] = NULL;
		close(ConnectionNumber(terminal->display));
		execve(arguments[0], arguments, environ);
		saved = errno;
		snprintf(message_local1, sizeof(message_local1),
			 "zterm: exec /bin/sh: %s\r\n", strerror(saved));
		(void)write(STDERR_FILENO, message_local1, strlen(message_local1));
		_exit(127);
	}
	damage_all(terminal);

	/* Reports successful completion. */
	return 0;
}

/* Supports the clear screen operation. */
static void
clear_screen(
	struct terminal *terminal)
{
	unsigned row;

	/* Process each element required by the operation. */
	for (row = 0; row < terminal->rows; row++)
		erase_range(terminal, row, 0, terminal->columns - 1U);
}

/* Supports the erase range operation. */
static void
erase_range(
	struct terminal *terminal,
	unsigned row,
	unsigned first,
	unsigned last)
{
	unsigned column;

	/* Handles the row condition. */
	if (row >= terminal->rows || first >= terminal->columns)
		return;

	/* Handles the last condition. */
	if (last >= terminal->columns)

	/* Process each element required by the operation. */
		last = terminal->columns - 1U;
	for (column = first; column <= last; column++)
		blank_cell(terminal, column, row);
	damage(terminal, row, first, last);
}

/* Cell mutation and damage tracking. */
static void
blank_cell(
	struct terminal *terminal,
	unsigned column,
	unsigned row)
{
	struct cell *cell;

	cell = cell_at(terminal, column, row);

	cell->codepoint = ' ';
	cell->foreground = terminal->foreground;
	cell->background = terminal->background;
	cell->continuation = 0;
}

/* Screen model and X request accounting. */
static struct cell *
cell_at(
	struct terminal *terminal,
	unsigned column,
	unsigned row)
{
	/* Returns the computed result. */
	return &terminal->cells[row * terminal->columns + column];
}

/* Supports the damage operation. */
static void
damage(
	struct terminal *terminal,
	unsigned row,
	unsigned first,
	unsigned last)
{
	/* Handles the row condition. */
	if (row >= terminal->rows || first >= terminal->columns)
		return;

	/* Handles the last condition. */
	if (last >= terminal->columns)
		last = terminal->columns - 1U;

	/* Checks the terminal state. */
	if (!terminal->dirty[row]) {
		terminal->dirty[row] = 1;
		terminal->dirty_first[row] = (uint16_t)first;
		terminal->dirty_last[row] = (uint16_t)last;
	} else {
		/* Handles the first condition. */
		if (first < terminal->dirty_first[row])
			terminal->dirty_first[row] = (uint16_t)first;

		/* Handles the last condition. */
		if (last > terminal->dirty_last[row])
			terminal->dirty_last[row] = (uint16_t)last;
	}
}

/* Supports the terminal message operation. */
static void
terminal_message(
	struct terminal *terminal,
	const char *message)
{
	/* Continue while the operation condition remains true. */
	while (*message != '\0')
		terminal_byte(terminal, (unsigned char)*message++);
}

/* Supports the terminal byte operation. */
static void
terminal_byte(
	struct terminal *terminal,
	unsigned char byte)
{
	int i;
	int *value;
	unsigned next;

	/*
 * parser_state: 0 is text, 1 follows ESC, and 2 parses a CSI sequence.
	 */
	if (terminal->parser_state == 1) {
		terminal->parser_state = 0;

		/* Classifies the current byte. */
		if (byte == '[') {
			/* Process each element required by the operation. */
			terminal->parser_state = 2;
			terminal->parameter_index = 0;
			for (i = 0; i < CSI_PARAMETERS; i++)
				terminal->parameters[i] = -1;
		} else if (byte == '7') {
			terminal->saved_column = terminal->cursor_column;
			terminal->saved_row = terminal->cursor_row;
		} else if (byte == '8') {
			terminal->cursor_column = terminal->saved_column;
			terminal->cursor_row = terminal->saved_row;
		} else if (byte == 'c') {
			terminal->foreground = 0xdcdde5;
			terminal->background = 0;
			clear_screen(terminal);
			terminal->cursor_column = terminal->cursor_row = 0;
		}

		/* Returns the computed result. */
		return;
	}

	/* Checks the terminal state. */
	if (terminal->parser_state == 2) {
		/* Classifies the current byte. */
		if (byte >= '0' && byte <= '9') {
			value = &terminal->parameters[terminal->parameter_index];

			/* Validates the current value. */
			if (*value < 0)
				*value = 0;
			*value = *value * 10 + byte - '0';
		} else if (byte == ';' &&
			   terminal->parameter_index + 1 < CSI_PARAMETERS)
			terminal->parameter_index++;
		else if (byte == '?') {
			/* Returns the computed result. */
			return;
		} else {
			csi_dispatch(terminal, byte);
			terminal->parser_state = 0;
		}

		/* Returns the computed result. */
		return;
	}

	/* Classifies the current byte. */
	if (byte == 0x1b) {
		terminal->utf8_remaining = 0;
		terminal->parser_state = 1;
	} else if (byte == '\r')
		terminal->cursor_column = 0;
	else if (byte == '\n')
		line_feed(terminal);
	else if (byte == '\b') {
		/* Checks the terminal state. */
		if (terminal->cursor_column != 0)
			terminal->cursor_column--;
	} else if (byte == '\t') {
		next = (terminal->cursor_column + 8U) & ~7U;
		terminal->cursor_column =
		    next < terminal->columns ? next : terminal->columns - 1U;
	} else if (byte >= 0x20)
		utf8_byte(terminal, byte);
}

/* Supports the csi dispatch operation. */
static void
csi_dispatch(
	struct terminal *terminal,
	unsigned char final)
{
	uint32_t swap;
	int value;
	unsigned row;
	unsigned column;
	int i;

	value = parameter(terminal, 0, 1);

	/* Unsupported CSI commands are intentionally ignored. */
	switch (final) {
	case 'A':
		terminal->cursor_row =
		    value > (int)terminal->cursor_row
			? 0
			: terminal->cursor_row - (unsigned)value;
		break;
	case 'B':
		row = terminal->cursor_row + (unsigned)value;
		terminal->cursor_row =
		    row < terminal->rows ? row : terminal->rows - 1U;
		break;
	case 'C':
		column = terminal->cursor_column + (unsigned)value;
		terminal->cursor_column = column < terminal->columns
					      ? column
					      : terminal->columns - 1U;
		break;
	case 'D':
		terminal->cursor_column =
		    value > (int)terminal->cursor_column
			? 0
			: terminal->cursor_column - (unsigned)value;
		break;
	case 'H':
	case 'f':
		row = (unsigned)parameter(terminal, 0, 1);
		column = (unsigned)parameter(terminal, 1, 1);
		terminal->cursor_row = row > 0 ? row - 1U : 0;
		terminal->cursor_column = column > 0 ? column - 1U : 0;

		/* Checks the terminal state. */
		if (terminal->cursor_row >= terminal->rows)
			terminal->cursor_row = terminal->rows - 1U;

		/* Checks the terminal state. */
		if (terminal->cursor_column >= terminal->columns)
			terminal->cursor_column = terminal->columns - 1U;
		break;
	case 'J':
		/* Handles a failed parameter operation. */
		if (parameter(terminal, 0, 0) == 2) {
			clear_screen(terminal);
			terminal->cursor_column = terminal->cursor_row = 0;
		} else {
			erase_range(terminal, terminal->cursor_row,
				    terminal->cursor_column,
				    terminal->columns - 1U);

			/* Process each element required by the operation. */
			for (row = terminal->cursor_row + 1U;
			     row < terminal->rows; row++) {
				erase_range(terminal, row, 0,
					    terminal->columns - 1U);
			}
		}
		break;
	case 'K':
		value = parameter(terminal, 0, 0);

		/* Validates the current value. */
		if (value == 1) {
			erase_range(terminal, terminal->cursor_row, 0,
				    terminal->cursor_column);
		} else if (value == 2) {
			erase_range(terminal, terminal->cursor_row, 0,
				    terminal->columns - 1U);
		} else {
			erase_range(terminal, terminal->cursor_row,
				    terminal->cursor_column,
				    terminal->columns - 1U);
		}
		break;
	case 'm':
		/* Process each remaining element. */
		for (i = 0; i <= terminal->parameter_index; i++) {
			value = terminal->parameters[i] < 0
				    ? 0
				    : terminal->parameters[i];

			/* Validates the current value. */
			if (value == 0) {
				terminal->foreground = 0xdcdde5;
				terminal->background = 0x000000;
			} else if (value == 7) {
				swap = terminal->foreground;
				terminal->foreground = terminal->background;
				terminal->background = swap;
			} else if (value >= 30 && value <= 37)
				terminal->foreground = ansi_colors[value - 30];
			else if (value >= 40 && value <= 47)
				terminal->background = ansi_colors[value - 40];
			else if (value >= 90 && value <= 97) {
				terminal->foreground =
				    ansi_colors[value - 90 + 8];
			} else if (value >= 100 && value <= 107) {
				terminal->background =
				    ansi_colors[value - 100 + 8];
			} else if (value == 39)
				terminal->foreground = 0xdcdde5;
			else if (value == 49)
				terminal->background = 0x000000;
		}
		break;
	default:
		break;
	}
}

/* VT100/ANSI and UTF-8 parsing. */
static int
parameter(
	const struct terminal *terminal,
	int index,
	int fallback)
{
	/* Checks the current index. */
	if (index > terminal->parameter_index ||
	    terminal->parameters[index] < 0)

		/* Returns the computed result. */
		return fallback;

	/* Returns the computed result. */
	return terminal->parameters[index];
}

/* Supports the line feed operation. */
static void
line_feed(
	struct terminal *terminal)
{
	/* Checks the terminal state. */
	if (terminal->cursor_row + 1U < terminal->rows)
		terminal->cursor_row++;
	else
		scroll_up(terminal);
}

/* Supports the scroll up operation. */
static void
scroll_up(
	struct terminal *terminal)
{
	memmove(terminal->cells, terminal->cells + terminal->columns,
		(terminal->rows - 1U) * terminal->columns *
		    sizeof(terminal->cells[0]));
	erase_range(terminal, terminal->rows - 1U, 0, terminal->columns - 1U);
	damage_all(terminal);
}

/* Supports the damage all operation. */
static void
damage_all(
	struct terminal *terminal)
{
	unsigned row;

	/* Process each element required by the operation. */
	for (row = 0; row < terminal->rows; row++)
		damage(terminal, row, 0, terminal->columns - 1U);
}

/* Supports the utf8 byte operation. */
static void
utf8_byte(
	struct terminal *terminal,
	unsigned char byte)
{
	uint32_t codepoint;

	/* Reject overlong encodings, surrogates and values beyond Unicode. */
	if (terminal->utf8_remaining == 0) {
		/* Classifies the current byte. */
		if (byte < 0x80) {
			put_codepoint(terminal, byte);
		} else if (byte >= 0xc2 && byte <= 0xdf) {
			terminal->utf8_value = byte & 0x1fU;
			terminal->utf8_minimum = 0x80;
			terminal->utf8_remaining = 1;
		} else if (byte >= 0xe0 && byte <= 0xef) {
			terminal->utf8_value = byte & 0x0fU;
			terminal->utf8_minimum = 0x800;
			terminal->utf8_remaining = 2;
		} else if (byte >= 0xf0 && byte <= 0xf4) {
			terminal->utf8_value = byte & 0x07U;
			terminal->utf8_minimum = 0x10000;
			terminal->utf8_remaining = 3;
		} else {
			put_codepoint(terminal, 0xfffd);
		}

		/* Returns the computed result. */
		return;
	}

	/* Classifies the current byte. */
	if ((byte & 0xc0U) != 0x80U) {
		terminal->utf8_remaining = 0;
		put_codepoint(terminal, 0xfffd);
		utf8_byte(terminal, byte);

		/* Returns the computed result. */
		return;
	}
	terminal->utf8_value = (terminal->utf8_value << 6) | (byte & 0x3fU);

	/* Checks the terminal state. */
	if (--terminal->utf8_remaining == 0) {
		codepoint = terminal->utf8_value;

		/* Handles the codepoint condition. */
		if (codepoint < terminal->utf8_minimum ||
		    codepoint > 0x10ffffU ||
		    (codepoint >= 0xd800U && codepoint <= 0xdfffU))
			codepoint = 0xfffd;
		put_codepoint(terminal, codepoint);
	}
}

/* Supports the put codepoint operation. */
static void
put_codepoint(
	struct terminal *terminal,
	uint32_t codepoint)
{
	unsigned width;
	struct cell *cell;

	width = wide_codepoint(codepoint) ? 2U : 1U;

	/* XDrawString16 limits the rendered repertoire to the Unicode BMP. */
	if (codepoint > 0xffffU)
		codepoint = 0xfffdU;

	/* Checks the terminal state. */
	if (terminal->cursor_column + width > terminal->columns) {
		terminal->cursor_column = 0;
		line_feed(terminal);
	}
	cell = cell_at(terminal, terminal->cursor_column, terminal->cursor_row);
	cell->codepoint = codepoint;
	cell->foreground = terminal->foreground;
	cell->background = terminal->background;
	cell->continuation = 0;

	/* Handles the width condition. */
	if (width == 2U) {
		cell = cell_at(terminal, terminal->cursor_column + 1U,
			       terminal->cursor_row);
		cell->codepoint = 0;
		cell->foreground = terminal->foreground;
		cell->background = terminal->background;
		cell->continuation = 1;
	}
	damage(terminal, terminal->cursor_row, terminal->cursor_column,
	       terminal->cursor_column + width - 1U);
	terminal->cursor_column += width;

	/* Checks the terminal state. */
	if (terminal->cursor_column >= terminal->columns) {
		terminal->cursor_column = 0;
		line_feed(terminal);
	}
}

/* Supports the wide codepoint operation. */
static int
wide_codepoint(
	uint32_t codepoint)
{
	/* Returns the computed result. */
	return (codepoint >= 0x1100 && codepoint <= 0x115f) ||
	       codepoint == 0x2329 || codepoint == 0x232a ||
	       (codepoint >= 0x2e80 && codepoint <= 0xa4cf) ||
	       (codepoint >= 0xac00 && codepoint <= 0xd7a3) ||
	       (codepoint >= 0xf900 && codepoint <= 0xfaff) ||
	       (codepoint >= 0xfe10 && codepoint <= 0xfe6f) ||
	       (codepoint >= 0xff01 && codepoint <= 0xff60) ||
	       (codepoint >= 0xffe0 && codepoint <= 0xffe6);
}

/* Supports the redraw operation. */
static void
redraw(
	struct terminal *terminal)
{
	unsigned row;

	/*
 * First restore the old cursor cell, then draw the cursor at its new
	 * site. */
	if (terminal->cursor_drawn) {
		damage(terminal, terminal->drawn_cursor_row,
		       terminal->drawn_cursor_column,
		       terminal->drawn_cursor_column);
	}

	/* Process each element required by the operation. */
	for (row = 0; row < terminal->rows; row++) {
		/* Checks the terminal state. */
		if (terminal->dirty[row]) {
			draw_row(terminal, row, terminal->dirty_first[row],
				 terminal->dirty_last[row]);
		}
	}
	XSetForeground(terminal->display, terminal->gc, 0xffffff);
	x_request(terminal);
	XFillRectangle(terminal->display, terminal->window, terminal->gc,
		       (int)(terminal->cursor_column * CELL_WIDTH),
		       (int)((terminal->cursor_row + 1U) * CELL_HEIGHT - 2U),
		       CELL_WIDTH, 2);
	x_request(terminal);
	terminal->drawn_cursor_column = terminal->cursor_column;
	terminal->drawn_cursor_row = terminal->cursor_row;
	terminal->cursor_drawn = 1;
	x_finish(terminal);
}

/* Rendering. */
static void
draw_row(
	struct terminal *terminal,
	unsigned row,
	unsigned first,
	unsigned last)
{
	unsigned first_local;
	unsigned first_local1;
	uint32_t background;
	struct cell *cell;
	unsigned scan;
	int count;
	uint32_t foreground;
	unsigned column;

	XChar2b text[MAX_COLUMNS];

	/*
 * Damage touching either half of a wide glyph must repaint both cells.
	 */
	if (first != 0U && cell_at(terminal, first, row)->continuation)
		first--;

	/* Handles a failed cell at operation. */
	if (last + 1U < terminal->columns &&
	    cell_at(terminal, last + 1U, row)->continuation)
		last++;

	XSetForeground(terminal->display, terminal->gc, 0x000000);
	x_request(terminal);
	XFillRectangle(terminal->display, terminal->window, terminal->gc,
		       (int)(first * CELL_WIDTH), (int)(row * CELL_HEIGHT),
		       (last - first + 1U) * CELL_WIDTH, CELL_HEIGHT);
	x_request(terminal);

	/* Process each element required by the operation. */
	for (column = first; column <= last;) {
		/* Continue while the operation condition remains true. */
		first_local = column;
		background = cell_at(terminal, column, row)->background;
		while (column <= last &&
		       cell_at(terminal, column, row)->background == background)
			column++;

		/* Handles the background condition. */
		if (background != 0) {
			XSetForeground(terminal->display, terminal->gc,
				       background);
			x_request(terminal);
			XFillRectangle(
			    terminal->display, terminal->window, terminal->gc,
			    (int)(first_local * CELL_WIDTH), (int)(row * CELL_HEIGHT),
			    (column - first_local) * CELL_WIDTH, CELL_HEIGHT);
			x_request(terminal);
		}
	}

	/* Process each element required by the operation. */
	for (column = first; column <= last;) {
		count = 0;

		/*
 * The background pass already represents blank cells.  Sending
		 * them as ImageText16 would make Xzed ask /dev/graphics for one
		 * glyph per cell, stalling the whole display server during the
		 * initial clear of a terminal window. */
		if (cell_at(terminal, column, row)->continuation ||
		    cell_at(terminal, column, row)->codepoint == ' ') {
			column++;
			continue;
		}

		/* Continue while the operation condition remains true. */
		first_local1 = column;
		foreground = cell_at(terminal, column, row)->foreground;
		scan = column;
		while (scan <= last) {
			cell = cell_at(terminal, scan, row);

			/* Handles the cell condition. */
			if (!cell->continuation &&
			    (cell->codepoint == ' ' ||
			     cell->foreground != foreground))
				break;

			/* Handles the cell condition. */
			if (!cell->continuation) {
				text[count].byte1 =
				    (unsigned char)(cell->codepoint >> 8);
				text[count].byte2 =
				    (unsigned char)cell->codepoint;
				count++;
			}
			scan++;
		}
		XSetForeground(terminal->display, terminal->gc, foreground);
		x_request(terminal);
		XDrawString16(terminal->display, terminal->window, terminal->gc,
			      (int)(first_local1 * CELL_WIDTH),
			      (int)((row + 1U) * CELL_HEIGHT), text, count);
		x_request(terminal);
		column = scan;
	}
	terminal->dirty[row] = 0;
}

/* Supports the x request operation. */
static void
x_request(
	struct terminal *terminal)
{
	/*
 * Bound the amount of drawing queued while processing large PTY bursts.
	 */
	if (++terminal->request_budget >= 5) {
		XSync(terminal->display, False);
		terminal->request_budget = 0;
	}
}

/* Supports the x finish operation. */
static void
x_finish(
	struct terminal *terminal)
{
	/* Checks the terminal state. */
	if (terminal->request_budget != 0) {
		XSync(terminal->display, False);
		terminal->request_budget = 0;
	}
}

/* Supports the resize terminal operation. */
static int
resize_terminal(
	struct terminal *terminal,
	unsigned width,
	unsigned height)
{
	struct cell *old;
	unsigned old_columns, old_rows;
	unsigned columns, rows;
	unsigned row, column, copy_columns, copy_rows;
	struct winsize winsize;

	old_columns = terminal->columns;
	old_rows = terminal->rows;
	columns = width / CELL_WIDTH;
	rows = height / CELL_HEIGHT;

	/* Handles the columns condition. */
	if (columns == 0)
		columns = 1;

	/* Handles the rows condition. */
	if (rows == 0)
		rows = 1;

	/* Handles the columns condition. */
	if (columns > MAX_COLUMNS)
		columns = MAX_COLUMNS;

	/* Handles the rows condition. */
	if (rows > MAX_ROWS)
		rows = MAX_ROWS;

	/* Handles the columns condition. */
	if (columns == old_columns && rows == old_rows)
		return 0;

	/*
 * Preserve the overlapping top-left region and blank newly exposed
	 * cells. */
	old = malloc((size_t)old_columns * old_rows * sizeof(*old));

	/* Handles the old availability. */
	if (old == NULL)
		return -1;
	memcpy(old, terminal->cells,
	       (size_t)old_columns * old_rows * sizeof(*old));

	/* Process each element required by the operation. */
	terminal->columns = columns;
	terminal->rows = rows;
	for (row = 0; row < rows; row++) {
		/* Process each element required by the operation. */
		for (column = 0; column < columns; column++)
			blank_cell(terminal, column, row);
	}

	/* Process each element required by the operation. */
	copy_columns = columns < old_columns ? columns : old_columns;
	copy_rows = rows < old_rows ? rows : old_rows;
	for (row = 0; row < copy_rows; row++) {
		memcpy(&terminal->cells[row * columns], &old[row * old_columns],
		       (size_t)copy_columns * sizeof(*old));
	}
	free(old);

	/* Checks the terminal state. */
	if (terminal->cursor_column >= columns)
		terminal->cursor_column = columns - 1U;

	/* Checks the terminal state. */
	if (terminal->cursor_row >= rows)
		terminal->cursor_row = rows - 1U;
	memset(&winsize, 0, sizeof(winsize));
	winsize.ws_row = (unsigned short)rows;
	winsize.ws_col = (unsigned short)columns;

	/* Checks the terminal state. */
	if (terminal->master >= 0)
		(void)ioctl(terminal->master, TIOCSWINSZ, &winsize);
	damage_all(terminal);

	/* Reports successful completion. */
	return 0;
}

/* Keyboard translation, terminal resizing and PTY lifecycle. */
static int
send_key(
	struct terminal *terminal,
	XKeyEvent *event)
{
	int function_result;
	KeySym symbol;
	const char *sequence;
	char byte;
	size_t length;

	symbol = XLookupKeysym(event, 0);
	sequence = NULL;
	length = 1;

	/* Ctrl+Shift+C copies the screen, Ctrl+Shift+V pastes CLIPBOARD. */
	if ((event->state & ControlMask) != 0 && (event->state & ShiftMask) != 0) {
		if (symbol == 'C' || symbol == 'c') {
			clip_copy(terminal);
			return 1;
		}

		/* The paste. */
		if (symbol == 'V' || symbol == 'v') {
			clip_paste(terminal);
			return 1;
		}
	}

	/* Dispatch the selected operation case. */
	switch (symbol) {
	case XK_Up:
		sequence = "\033[A";
		length = 3;
		break;
	case XK_Down:
		sequence = "\033[B";
		length = 3;
		break;
	case XK_Right:
		sequence = "\033[C";
		length = 3;
		break;
	case XK_Left:
		sequence = "\033[D";
		length = 3;
		break;
	case XK_Home:
		sequence = "\033[H";
		length = 3;
		break;
	case XK_End:
		sequence = "\033[F";
		length = 3;
		break;
	case XK_Delete:
		sequence = "\033[3~";
		length = 4;
		break;
	case XK_Page_Up:
		sequence = "\033[5~";
		length = 4;
		break;
	case XK_Page_Down:
		sequence = "\033[6~";
		length = 4;
		break;
	case XK_Return:
		byte = '\r';
		sequence = &byte;
		break;
	case XK_BackSpace:
		byte = 0x7f;
		sequence = &byte;
		break;
	default:
		/* Handles the symbol condition. */
		if (symbol > 0 && symbol < 0x80) {
			byte = (char)symbol;

			/* Handles the event condition. */
			if ((event->state & ControlMask) != 0 &&
			    ((byte >= 'a' && byte <= 'z') ||
			     (byte >= 'A' && byte <= 'Z')))
				byte = (char)((byte & 0x1f));
			sequence = &byte;
		} else {
			/* Reports successful completion. */
			return 0;
		}
		break;
	}

	/* Computes the function result. */
	function_result = write(terminal->master, sequence, length) == (ssize_t)length;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Copies the screen's text (each row without its trailing blanks, a line
 * break after each but the last text) and owns CLIPBOARD with it.
 */
static void
clip_copy(
	struct terminal *terminal)
{
	struct cell *cell;
	char *text;
	size_t used;
	size_t row_end;
	unsigned row;
	unsigned column;

	/* Room for every cell as four bytes and a line break a row. */
	text = malloc((size_t)terminal->columns * terminal->rows * 4U + terminal->rows + 1U);
	if (text == NULL)
		return;

	/* Each row. */
	used = 0;
	for (row = 0; row < terminal->rows; row++) {
		/* Its characters, and where its last non-blank one ends. */
		row_end = used;
		for (column = 0; column < terminal->columns; column++) {
			/* A wide character's second cell adds nothing. */
			cell = cell_at(terminal, column, row);
			if (cell->continuation)
				continue;

			/* The character as UTF-8 (a blank as a space). */
			if (cell->codepoint == 0U || cell->codepoint == ' ') {
				text[used++] = ' ';
			} else {
				used += clip_utf8(cell->codepoint, text + used);
				row_end = used;
			}
		}

		/* The row's blanks go, and a line break follows it. */
		used = row_end;
		text[used++] = '\n';
	}

	/* The last rows' line breaks go. */
	while (used > 0U && text[used - 1U] == '\n')
		used--;
	text[used] = '\0';

	/* zterm owns CLIPBOARD with it. */
	free(terminal->clip);
	terminal->clip = text;
	terminal->clip_length = used;
	(void)XSetSelectionOwner(terminal->display, terminal->atom_clipboard, terminal->window, CurrentTime);
	printf("ZTERM-X COPY bytes=%lu\n", (unsigned long)used);
	fflush(stdout);
}

/* Asks for CLIPBOARD's text as UTF8_STRING in a property of the window; its SelectionNotify pastes it. */
static void
clip_paste(
	struct terminal *terminal)
{
	/* The request. */
	(void)XConvertSelection(terminal->display, terminal->atom_clipboard, terminal->atom_utf8, terminal->atom_paste, terminal->window, CurrentTime);
	printf("ZTERM-X PASTE asked\n");
	fflush(stdout);
}

/*
 * Answers a request for the text zterm owns: TARGETS lists the types, a
 * text type puts the text in the requestor's property, anything else is
 * refused (property None); the requestor is told with SelectionNotify.
 */
static void
clip_request(
	struct terminal *terminal,
	const XSelectionRequestEvent *request)
{
	XEvent answer;
	Atom targets[3];
	Atom property;
	unsigned long given;
	const char *text;
	size_t length;

	/* The selection's text: CLIPBOARD's or PRIMARY's; none, or another selection, is refused. */
	property = request->property;
	text = NULL;
	length = 0;
	if (request->selection == terminal->atom_clipboard) {
		text = terminal->clip;
		length = terminal->clip_length;
	} else if (request->selection == XA_PRIMARY) {
		text = terminal->primary;
		length = terminal->primary_length;
	}

	/* No text is refused. */
	if (text == NULL)
		property = None;

	/* TARGETS: the types given. */
	if (property != None && request->target == terminal->atom_targets) {
		targets[0] = terminal->atom_targets;
		targets[1] = terminal->atom_utf8;
		targets[2] = XA_STRING;
		(void)XChangeProperty(terminal->display, request->requestor, property, XA_ATOM, 32, PropModeReplace, (const unsigned char *)targets, 3);
	} else if (property != None && (request->target == terminal->atom_utf8 || request->target == XA_STRING)) {
		/* The text, as the type asked. */
		(void)XChangeProperty(terminal->display, request->requestor, property, request->target, 8, PropModeReplace, (const unsigned char *)text, (int)length);
	} else {
		/* Another type is refused. */
		property = None;
	}

	/* The requestor is told. */
	memset(&answer, 0, sizeof(answer));
	answer.xselection.type = SelectionNotify;
	answer.xselection.requestor = request->requestor;
	answer.xselection.selection = request->selection;
	answer.xselection.target = request->target;
	answer.xselection.property = property;
	answer.xselection.time = request->time;
	(void)XSendEvent(terminal->display, request->requestor, False, NoEventMask, &answer);

	/* The log line the tests read (the bytes given, 0 when refused). */
	given = 0;
	if (property != None)
		given = (unsigned long)length;
	printf("ZTERM-X SELECTION answered requestor=0x%x bytes=%lu\n", (unsigned)request->requestor, given);
	fflush(stdout);
}

/*
 * Pastes the text a conversion put in the window's property: to the shell,
 * line breaks as carriage returns (what Enter sends); the property goes.
 */
static void
clip_notify(
	struct terminal *terminal,
	const XSelectionEvent *notify)
{
	unsigned long count;
	unsigned long after;
	unsigned long index;
	unsigned char *data;
	ssize_t written;
	Atom type;
	int format;
	int failed;

	/* Nothing to paste. */
	if (notify->property == None || terminal->master < 0) {
		printf("ZTERM-X PASTE none\n");
		fflush(stdout);
		return;
	}

	/* The property's text (deleted as it is read). */
	failed = XGetWindowProperty(terminal->display, terminal->window, notify->property, 0L, 262144L, True, AnyPropertyType, &type, &format, &count, &after, &data);
	if (failed || data == NULL || format != 8)
		return;

	/* Line breaks as carriage returns, to the shell. */
	for (index = 0; index < count; index++) {
		/* A line break is what Enter sends. */
		if (data[index] == '\n')
			data[index] = '\r';
	}

	/* The text to the shell. */
	written = 0;
	if (count > 0UL)
		written = write(terminal->master, data, (size_t)count);
	if (written < 0)
		count = 0;

	/* The log line the tests read, and the data goes. */
	printf("ZTERM-X PASTE bytes=%lu\n", count);
	fflush(stdout);
	XFree(data);
}

/*
 * A pointer button in the window: a double click of the left one selects
 * the word under it as PRIMARY, the middle one pastes PRIMARY.  The event's
 * detail (its keycode field in this Xlib) is the button.
 */
static void
button_press(
	struct terminal *terminal,
	const XButtonEvent *event)
{
	unsigned column;
	unsigned row;
	int twice;

	/* The middle button asks for PRIMARY's text; its SelectionNotify pastes it. */
	if (event->keycode == BUTTON_MIDDLE) {
		(void)XConvertSelection(terminal->display, XA_PRIMARY, terminal->atom_utf8, terminal->atom_paste, terminal->window, CurrentTime);
		printf("ZTERM-X PRIMARY paste asked\n");
		fflush(stdout);
		return;
	}

	/* Only the left button selects, inside the grid. */
	if (event->keycode != BUTTON_LEFT || event->x < 0 || event->y < 0)
		return;
	column = (unsigned)event->x / CELL_WIDTH;
	row = (unsigned)event->y / CELL_HEIGHT;
	if (column >= terminal->columns || row >= terminal->rows)
		return;

	/* The second press on the same cell soon after the first selects its word. */
	twice = 0;
	if (terminal->click_time != 0 && event->time - terminal->click_time <= DOUBLE_CLICK_MS &&
	    column == terminal->click_column && row == terminal->click_row)
		twice = 1;
	terminal->click_time = event->time;
	terminal->click_column = column;
	terminal->click_row = row;
	if (twice) {
		terminal->click_time = 0;
		primary_select(terminal, column, row);
	}
}

/* Owns PRIMARY with the word (the run of non-blank characters) at a cell. */
static void
primary_select(
	struct terminal *terminal,
	unsigned column,
	unsigned row)
{
	struct cell *cell;
	unsigned first;
	unsigned last;
	unsigned index;
	size_t used;
	char *text;
	int word;

	/* A blank cell selects nothing. */
	word = word_cell(terminal, column, row);
	if (!word)
		return;

	/* The word's first cell. */
	first = column;
	while (first > 0U) {
		word = word_cell(terminal, first - 1U, row);
		if (!word)
			break;
		first--;
	}

	/* Its last. */
	last = column;
	while (last + 1U < terminal->columns) {
		word = word_cell(terminal, last + 1U, row);
		if (!word)
			break;
		last++;
	}

	/* Its characters as UTF-8. */
	text = malloc((size_t)(last - first + 1U) * 4U + 1U);
	if (text == NULL)
		return;
	used = 0;
	for (index = first; index <= last; index++) {
		/* A wide character's second cell adds nothing. */
		cell = cell_at(terminal, index, row);
		if (!cell->continuation)
			used += clip_utf8(cell->codepoint, text + used);
	}

	/* A NUL ends it. */
	text[used] = '\0';

	/* Succeeded: zterm owns PRIMARY with it. */
	free(terminal->primary);
	terminal->primary = text;
	terminal->primary_length = used;
	(void)XSetSelectionOwner(terminal->display, XA_PRIMARY, terminal->window, CurrentTime);
	printf("ZTERM-X PRIMARY set bytes=%lu\n", (unsigned long)used);
	fflush(stdout);
}

/* Reports whether a cell is part of a word (neither blank nor a space; a wide character's second cell is). */
static int
word_cell(
	struct terminal *terminal,
	unsigned column,
	unsigned row)
{
	struct cell *cell;

	/* The cell. */
	cell = cell_at(terminal, column, row);
	if (cell->continuation)
		return 1;

	/* A blank or a space is not. */
	if (cell->codepoint == 0U || cell->codepoint == ' ')
		return 0;

	/* Anything else is. */
	return 1;
}

/* Writes a code point as UTF-8; returns how many bytes (at most four). */
static size_t
clip_utf8(
	uint32_t codepoint,
	char *out)
{
	/* One byte. */
	if (codepoint < 0x80U) {
		out[0] = (char)codepoint;
		return 1;
	}

	/* Two. */
	if (codepoint < 0x800U) {
		out[0] = (char)(0xc0U | (codepoint >> 6));
		out[1] = (char)(0x80U | (codepoint & 0x3fU));
		return 2;
	}

	/* Three. */
	if (codepoint < 0x10000U) {
		out[0] = (char)(0xe0U | (codepoint >> 12));
		out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
		out[2] = (char)(0x80U | (codepoint & 0x3fU));
		return 3;
	}

	/* Four. */
	out[0] = (char)(0xf0U | (codepoint >> 18));
	out[1] = (char)(0x80U | ((codepoint >> 12) & 0x3fU));
	out[2] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
	out[3] = (char)(0x80U | (codepoint & 0x3fU));
	return 4;
}

/*
 * Reads -geometry COLUMNSxROWS from the arguments (0 and 0 without it).
 * Returns 0, or -1 for arguments zterm does not know.
 */
static int
geometry(
	int argc,
	char **argv,
	unsigned *columns,
	unsigned *rows)
{
	char extra;
	int same;
	int read;

	/* Without arguments, the root's size. */
	*columns = 0U;
	*rows = 0U;
	if (argc == 1)
		return 0;

	/* Only -geometry and its value. */
	if (argc != 3)
		return -1;
	same = strcmp(argv[1], "-geometry");
	if (same != 0)
		return -1;

	/* Two numbers joined by an x, both at least 1. */
	read = sscanf(argv[2], "%ux%u%c", columns, rows, &extra);
	if (read != 2 || *columns == 0U || *rows == 0U)
		return -1;

	/* Succeeded. */
	return 0;
}
