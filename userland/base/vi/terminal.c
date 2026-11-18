/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminal under vi: its modes, its size, its control strings, the
 * output it is sent and the keys read from it.
 */

#include "userland/base/vi/vi.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/*
 * How long a key sequence may pause after ESC, in milliseconds.  A longer
 * pause means ESC was pressed on its own.
 */
#define TERMINAL_ESCAPE_WAIT 50

/* The longest key sequence read after ESC [ or ESC O. */
#define TERMINAL_SEQUENCE_LENGTH 8

/*
 * One control string vi uses, with the VT100 string that stands in for
 * it when the terminal has no description.
 */
struct terminal_capability {
	const char *name;
	const char *fallback;
	size_t field;
};

/*
 * The control strings vi uses.
 *
 * The VT100 strings serve every terminal zedBSD describes, so they are
 * the right guess when TERM names a terminal without a description.
 */
static const struct terminal_capability terminal_capabilities[] = {
	{ "cup", "\033[%i%p1%d;%p2%dH", offsetof(struct vi_terminal, move_string) },
	{ "el", "\033[K", offsetof(struct vi_terminal, clear_line_string) },
	{ "clear", "\033[H\033[2J", offsetof(struct vi_terminal, clear_string) },
	{ "smcup", "", offsetof(struct vi_terminal, enter_string) },
	{ "rmcup", "", offsetof(struct vi_terminal, leave_string) },
	{ "civis", "", offsetof(struct vi_terminal, hide_cursor_string) },
	{ "cnorm", "", offsetof(struct vi_terminal, show_cursor_string) },
	{ "sgr0", "\033[m", offsetof(struct vi_terminal, normal_string) },
	{ "bel", "\007", offsetof(struct vi_terminal, bell_string) }
};

/*
 * The size the terminal description gives, used when the kernel does not
 * know the window size.  Set once when the terminal is opened.
 */
static int terminal_described_rows;
static int terminal_described_columns;

static int load_description(struct vi_terminal *terminal);
static int read_byte(struct vi_terminal *terminal, int wait, unsigned char *byte);
static int read_sequence(struct vi_terminal *terminal, unsigned char introducer);
static int sequence_key(const unsigned char *sequence, size_t length);

/*
 * Checks that vi runs on a terminal and learns how to drive it.
 *
 * Returns 0; ENOTTY when the standard input or output is not a terminal;
 * or ENOTSUP when the terminal is described but cannot move its cursor.
 */
int
vi_terminal_open(
	struct vi_terminal *terminal)
{
	int is_terminal;
	int error;

	/* Starts from a closed terminal on the standard descriptors. */
	memset(terminal, 0, sizeof(*terminal));
	terminal->input = STDIN_FILENO;
	terminal->output = STDOUT_FILENO;

	/* vi reads keys from the standard input and draws on the standard output. */
	is_terminal = isatty(terminal->input);
	if (!is_terminal)
		return ENOTTY;

	/* The drawing goes to the standard output, which must be a terminal as well. */
	is_terminal = isatty(terminal->output);
	if (!is_terminal)
		return ENOTTY;

	/* Learns the control strings of the terminal TERM names. */
	error = load_description(terminal);
	if (error != 0)
		return error;

	/* Learns the window size. */
	vi_terminal_measure(terminal);

	/* Succeeded. */
	return 0;
}

/*
 * Puts the terminal into the mode vi edits in.
 *
 * Keys arrive one at a time and unechoed, and the keys that would send a
 * signal or stop the output arrive as keys.  Returns 0 or an errno value.
 */
int
vi_terminal_enter(
	struct vi_terminal *terminal)
{
	struct termios modes;
	int status;

	/* The modes to give back are read once, before vi first changes them. */
	if (!terminal->modes_saved) {
		status = tcgetattr(terminal->input, &terminal->saved_modes);
		if (status != 0)
			return errno;

		/* Remembers that the modes to give back are now known. */
		terminal->modes_saved = 1;
	}

	/* Turns off line editing, echo, signal keys, flow control and CR translation. */
	modes = terminal->saved_modes;
	modes.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
	modes.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | IGNCR | ISTRIP);
	modes.c_cc[VMIN] = 1;
	modes.c_cc[VTIME] = 0;

	/* Applies the editing mode. */
	status = tcsetattr(terminal->input, TCSANOW, &modes);
	if (status != 0)
		return errno;

	/* Remembers that the terminal has to be given back. */
	terminal->raw = 1;

	/* Switches to the terminal's editing screen, where it has one. */
	vi_terminal_command(terminal, terminal->enter_string);
	vi_terminal_flush(terminal);
	terminal->screen_entered = 1;

	/* Succeeded. */
	return 0;
}

/*
 * Gives the terminal back as vi found it.
 *
 * Safe to call more than once; only the first call after vi_terminal_enter
 * does anything.
 */
void
vi_terminal_leave(
	struct vi_terminal *terminal)
{
	/* Leaves the editing screen with the cursor on a clean last line. */
	if (terminal->screen_entered) {
		vi_terminal_command(terminal, terminal->normal_string);
		vi_terminal_move(terminal, terminal->rows - 1, 0);
		vi_terminal_command(terminal, terminal->clear_line_string);
		vi_terminal_command(terminal, terminal->show_cursor_string);
		vi_terminal_command(terminal, terminal->leave_string);
		vi_terminal_flush(terminal);
		terminal->screen_entered = 0;
	}

	/* Restores the line mode the shell expects, keeping keys typed ahead for the shell. */
	if (terminal->raw) {
		(void)tcsetattr(terminal->input, TCSADRAIN, &terminal->saved_modes);
		terminal->raw = 0;
	}
}

/*
 * Learns the window size of the terminal.
 *
 * The kernel's size wins; the terminal description comes next, and 24 by
 * 80 last.  A window smaller than vi can draw in is treated as the
 * smallest one it can.
 */
void
vi_terminal_measure(
	struct vi_terminal *terminal)
{
	struct winsize size;
	int status;

	/* Starts from the size the description gives. */
	terminal->rows = terminal_described_rows;
	terminal->columns = terminal_described_columns;

	/* Asks the kernel for the real window size. */
	memset(&size, 0, sizeof(size));
	status = ioctl(terminal->output, TIOCGWINSZ, &size);
	if (status == 0) {
		/* A zero means the kernel does not know that side. */
		if (size.ws_row != 0)
			terminal->rows = size.ws_row;
		if (size.ws_col != 0)
			terminal->columns = size.ws_col;
	}

	/* Keeps the window at least as large as vi can draw in. */
	if (terminal->rows < VI_MINIMUM_ROWS)
		terminal->rows = VI_MINIMUM_ROWS;
	if (terminal->columns < VI_MINIMUM_COLUMNS)
		terminal->columns = VI_MINIMUM_COLUMNS;
}

/*
 * Adds bytes to the output that the next flush writes.
 */
void
vi_terminal_put(
	struct vi_terminal *terminal,
	const char *bytes,
	size_t length)
{
	size_t room;
	size_t part;

	/* Copies the bytes, writing the buffer out whenever it fills. */
	while (length != 0) {
		room = sizeof(terminal->output_buffer) - terminal->output_length;
		if (room == 0) {
			vi_terminal_flush(terminal);
			continue;
		}

		/* Copies as much as fits. */
		part = length;
		if (part > room)
			part = room;
		memcpy(terminal->output_buffer + terminal->output_length, bytes, part);
		terminal->output_length += part;
		bytes += part;
		length -= part;
	}
}

/*
 * Moves the cursor to a row and a column, both counted from zero.
 */
void
vi_terminal_move(
	struct vi_terminal *terminal,
	int row,
	int column)
{
	long parameters[9];
	char sequence[TERMINFO_VALUE_LENGTH];
	int status;

	/* The cursor address takes the row first, then the column. */
	memset(parameters, 0, sizeof(parameters));
	parameters[0] = row;
	parameters[1] = column;

	/* Expands the terminal's cursor address string for this place; a negative length is a malformed string. */
	status = terminfo_expand(terminal->move_string, parameters, sequence, sizeof(sequence));
	if (status < 0)
		return;

	/* Adds the expanded string to the output. */
	vi_terminal_command(terminal, sequence);
}

/*
 * Adds one control string to the output.  An empty string adds nothing.
 */
void
vi_terminal_command(
	struct vi_terminal *terminal,
	const char *capability)
{
	size_t length;

	/* The string goes out as it is. */
	length = strlen(capability);
	vi_terminal_put(terminal, capability, length);
}

/*
 * Writes the collected output to the terminal.
 *
 * A write the terminal refuses drops the output; the next full redraw
 * repairs the screen.
 */
void
vi_terminal_flush(
	struct vi_terminal *terminal)
{
	size_t written;
	ssize_t count;

	/* Writes until every byte is out, resuming after a signal. */
	written = 0;
	while (written < terminal->output_length) {
		count = write(terminal->output, terminal->output_buffer + written, terminal->output_length - written);
		if (count < 0) {
			/* A signal only interrupts the write. */
			if (errno == EINTR)
				continue;
			break;
		}

		/* Counts the bytes the write took. */
		written += (size_t)count;
	}

	/* The buffer is empty again. */
	terminal->output_length = 0;
}

/*
 * Reads one key.
 *
 * Returns the key's byte, one of the VI_KEY values for a key that sends a
 * sequence, -1 when a signal arrived before a key, or VI_KEY_STOP when the
 * terminal is gone.
 */
int
vi_terminal_read_key(
	struct vi_terminal *terminal)
{
	unsigned char byte;
	unsigned char next;
	int status;
	int key;

	/* Waits for the first byte of the key. */
	status = read_byte(terminal, -1, &byte);
	if (status < 0)
		return -1;
	if (status == 0)
		return VI_KEY_STOP;

	/* Any byte but ESC is a key of its own. */
	if (byte != VI_KEY_ESCAPE)
		return byte;

	/* ESC alone: nothing follows soon. */
	status = read_byte(terminal, TERMINAL_ESCAPE_WAIT, &next);
	if (status <= 0)
		return VI_KEY_ESCAPE;

	/* ESC [ and ESC O start the sequences the special keys send. */
	if (next == '[') {
		key = read_sequence(terminal, next);
		return key;
	}

	/* ESC O starts the sequences some terminals send for the same keys. */
	if (next == 'O') {
		key = read_sequence(terminal, next);
		return key;
	}

	/*
	 * ESC and then another key typed quickly: the ESC ends what was being
	 * typed, and the other byte is kept to be read as the next key.
	 */
	terminal->pushed_byte = next;
	terminal->has_pushed_byte = 1;

	/* Reports the ESC that ended what was being typed. */
	return VI_KEY_ESCAPE;
}

/*
 * Tells whether a key is already waiting, so that drawing can wait until
 * typed-ahead or pasted keys are taken.
 */
int
vi_terminal_input_pending(
	struct vi_terminal *terminal)
{
	struct pollfd poll_entry;
	int ready;

	/* A byte kept from the ESC check is waiting. */
	if (terminal->has_pushed_byte)
		return 1;

	/* Asks without waiting whether the terminal has input. */
	memset(&poll_entry, 0, sizeof(poll_entry));
	poll_entry.fd = terminal->input;
	poll_entry.events = POLLIN;
	ready = poll(&poll_entry, 1, 0);
	if (ready > 0)
		return 1;

	/* Nothing is waiting. */
	return 0;
}

/* Reads the control strings of the terminal TERM names into the terminal. */
static int
load_description(
	struct vi_terminal *terminal)
{
	struct terminfo *description;
	const struct terminfo_capability *capability;
	const struct terminal_capability *entry;
	const char *type;
	const char *directory;
	char *field;
	size_t index;
	int loaded;
	int status;

	/* The description is large, so it lives on the heap while it is read. */
	description = malloc(sizeof(*description));
	if (description == NULL)
		return ENOMEM;

	/* Reads the description of the terminal TERM names, if there is one. */
	loaded = 0;
	type = getenv("TERM");
	directory = getenv("TERMINFO");
	if (type != NULL) {
		status = terminfo_load(description, type, directory);
		if (status == 0)
			loaded = 1;
	}

	/* A described terminal must be able to place its cursor and clear a line. */
	if (loaded) {
		capability = terminfo_find(description, "cup");
		if (capability == NULL) {
			free(description);
			return ENOTSUP;
		}

		/* The line clearing string is needed as well. */
		capability = terminfo_find(description, "el");
		if (capability == NULL) {
			free(description);
			return ENOTSUP;
		}
	}

	/* Copies each control string, or its VT100 stand-in. */
	for (index = 0; index < sizeof(terminal_capabilities) / sizeof(terminal_capabilities[0]); index++) {
		entry = &terminal_capabilities[index];
		field = (char *)terminal + entry->field;
		strcpy(field, entry->fallback);

		/* The described string replaces the stand-in when the terminal has one. */
		if (loaded) {
			capability = terminfo_find(description, entry->name);
			if (capability != NULL) {
				if (capability->kind == TERMINFO_STRING)
					strcpy(field, capability->string);
			}
		}
	}

	/* Keeps the described size for when the kernel does not know one. */
	terminal_described_rows = 24;
	terminal_described_columns = 80;
	if (loaded) {
		capability = terminfo_find(description, "lines");
		if (capability != NULL) {
			if (capability->kind == TERMINFO_NUMBER)
				terminal_described_rows = (int)capability->number;
		}

		/* The number of columns the description gives. */
		capability = terminfo_find(description, "cols");
		if (capability != NULL) {
			if (capability->kind == TERMINFO_NUMBER)
				terminal_described_columns = (int)capability->number;
		}
	}

	/* Releases the description, whose strings are now copied. */
	free(description);

	/* Succeeded: every control string vi uses is set. */
	return 0;
}

/*
 * Reads one byte, waiting at most the given milliseconds (or forever when
 * the wait is negative).  Returns 1 with the byte, 0 when the terminal
 * is gone or the wait ran out, or -1 when a signal arrived.
 */
static int
read_byte(
	struct vi_terminal *terminal,
	int wait,
	unsigned char *byte)
{
	struct pollfd poll_entry;
	struct timespec timeout;
	const struct timespec *limit;
	const sigset_t *mask;
	ssize_t count;
	int ready;

	/* A byte read too early by the ESC check is the next byte. */
	if (terminal->has_pushed_byte) {
		*byte = terminal->pushed_byte;
		terminal->has_pushed_byte = 0;
		return 1;
	}

	/*
	 * Waits for a byte with the editor's signals let through only during
	 * the wait, so a signal either arrives before the wait and is seen by
	 * the caller, or ends the wait.
	 */
	memset(&poll_entry, 0, sizeof(poll_entry));
	poll_entry.fd = terminal->input;
	poll_entry.events = POLLIN;
	limit = NULL;
	if (wait >= 0) {
		timeout.tv_sec = wait / 1000;
		timeout.tv_nsec = (long)(wait % 1000) * 1000000L;
		limit = &timeout;
	}

	/* Waits with the signal mask that lets the editor's signals through, when one is set. */
	mask = NULL;
	if (terminal->wait_mask_set)
		mask = &terminal->wait_mask;
	ready = ppoll(&poll_entry, 1, limit, mask);
	if (ready < 0) {
		/* A signal ended an endless wait; a bounded one just ends. */
		if (errno == EINTR && wait < 0)
			return -1;
		return 0;
	}

	/* The wait ran out without a byte. */
	if (ready == 0)
		return 0;

	/* Reads the byte. */
	count = read(terminal->input, byte, 1);
	if (count < 0) {
		/* A signal arrived before the byte. */
		if (errno == EINTR)
			return -1;
		return 0;
	}

	/* End of input: the terminal hung up. */
	if (count == 0)
		return 0;

	/* Succeeded: one byte read. */
	return 1;
}

/*
 * Reads the rest of a key sequence after ESC [ or ESC O and names the key.
 * An unknown sequence is read to its end, so none of it is taken as keys.
 */
static int
read_sequence(
	struct vi_terminal *terminal,
	unsigned char introducer)
{
	unsigned char sequence[TERMINAL_SEQUENCE_LENGTH + 1];
	unsigned char byte;
	size_t length;
	int status;
	int key;

	/* The sequence is named with its introducer first. */
	sequence[0] = introducer;
	length = 1;

	/* Reads until the final byte, which is a letter or a tilde; a cut sequence is dropped. */
	for (;;) {
		status = read_byte(terminal, TERMINAL_ESCAPE_WAIT, &byte);
		if (status <= 0)
			return VI_KEY_UNKNOWN;

		/* A sequence longer than any key vi knows is skipped. */
		if (length < TERMINAL_SEQUENCE_LENGTH) {
			sequence[length] = byte;
			length++;
		}

		/* Parameters and separators continue the sequence; anything else ends it. */
		if (byte >= 0x40 && byte <= 0x7e)
			break;
	}

	/* Names the key the whole sequence stands for. */
	key = sequence_key(sequence, length);

	/* Succeeded: the key, or VI_KEY_UNKNOWN for one vi does not know. */
	return key;
}

/* Names the key a sequence after ESC stands for, or VI_KEY_UNKNOWN when vi does not know it. */
static int
sequence_key(
	const unsigned char *sequence,
	size_t length)
{
	unsigned char final;

	/* The last byte of the sequence says which key it is. */
	final = sequence[length - 1];

	/* The keys that send one letter after the introducer. */
	if (length == 2) {
		/* Chooses the key by its letter. */
		switch (final) {
		case 'A':
			return VI_KEY_UP;
		case 'B':
			return VI_KEY_DOWN;
		case 'C':
			return VI_KEY_RIGHT;
		case 'D':
			return VI_KEY_LEFT;
		case 'H':
			return VI_KEY_HOME;
		case 'F':
			return VI_KEY_END;
		default:
			return VI_KEY_UNKNOWN;
		}
	}

	/* The keys that send ESC [ number ~. */
	if (length == 3) {
		if (sequence[0] != '[')
			return VI_KEY_UNKNOWN;
		if (final != '~')
			return VI_KEY_UNKNOWN;

		/* Chooses the key by its number. */
		switch (sequence[1]) {
		case '1':
		case '7':
			return VI_KEY_HOME;
		case '4':
		case '8':
			return VI_KEY_END;
		case '3':
			return VI_KEY_DELETE;
		case '5':
			return VI_KEY_PAGE_UP;
		case '6':
			return VI_KEY_PAGE_DOWN;
		default:
			return VI_KEY_UNKNOWN;
		}
	}

	/* Any longer sequence is a key vi does not use. */
	return VI_KEY_UNKNOWN;
}
