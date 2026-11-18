/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminal's clipboard, drops and drags through libkeiland's window
 * (WS131 p018; the compositor's clipboard since WS035 p079): Edit > Copy makes
 * the terminal's text the selection, Edit > Paste receives the
 * selection's text (the terminal's own directly).  A drag of text or of
 * file names dropped on the window (ws035-p088) is pasted into the shell
 * like a paste: file names (from "text/uri-list") each quoted and followed
 * by a space, text as it is.  Selected text dragged out of the window
 * (ws035-p093) is a drag of libkeiland's window.
 *
 * Without the compositor's clipboard the clipboard is the window's own, as
 * before.  The lines the tests read are printed here and in window.c.
 */

#include "terminal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t clipboard_paths(char *text, size_t length, size_t size);
static int clipboard_hex(int character);

/*
 * Sets the terminal's text as the selection (Edit > Copy); the window
 * keeps its own copy.
 */
void
terminal_clipboard_set(
	struct terminal_window *window,
	const char *text,
	size_t length)
{
	/* The selection, from the window's last input. */
	kl_window_copy(window->kui, text, length);
	printf("ZTERM CLIPBOARD set bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
}

/*
 * Starts a drag of text out of the window (ws035-p093), from the press
 * with the serial.
 */
void
terminal_clipboard_drag(
	struct terminal_window *window,
	const char *text,
	size_t length,
	uint32_t serial)
{
	int error;

	/* Only with the compositor's drag and drop, and one drag at a time. */
	error = kl_window_drag_text(window->kui, text, length, serial);
	if (error != 0)
		return;

	/* Succeeded: the drag starts from the window. */
	printf("ZTERM DRAG start bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
}

/*
 * Tells whether the terminal's own text is the selection (a paste then
 * takes it directly).
 */
int
terminal_clipboard_own(
	const struct terminal_window *window)
{
	int own;

	/* The window's answer. */
	own = kl_window_selection_own(window->kui, KL_SELECTION_CLIPBOARD);
	return own;
}

/*
 * Tells whether a paste has text: the terminal's own, or another client's
 * selection with a text type.
 */
int
terminal_clipboard_has_text(
	const struct terminal_window *window)
{
	int text;

	/* The window's answer. */
	text = kl_window_can_paste(window->kui);
	return text;
}

/*
 * Receives the selection's text into a buffer (Edit > Paste).  Returns the
 * bytes received (0 for none).
 */
size_t
terminal_clipboard_receive(
	struct terminal_window *window,
	char *text,
	size_t size)
{
	size_t length;

	/* The text, through a pipe from another client. */
	length = kl_window_paste(window->kui, text, size);

	/* Succeeded: the text received. */
	printf("ZTERM CLIPBOARD received bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
	return length;
}

/*
 * Receives what was dropped on the window into a buffer: file names as
 * quoted words (from "text/uri-list"), or text.  The drop is then
 * finished.  Returns the bytes to paste (0 for none).
 */
size_t
terminal_clipboard_drop(
	struct terminal_window *window,
	char *text,
	size_t size)
{
	unsigned type;
	size_t length;
	int uris;

	/* What was dropped; the file names become words. */
	window->drop_pending = 0;
	length = kl_window_take_drop(window->kui, text, size, &type);
	uris = 0;
	if (type == KL_DROP_URIS) {
		uris = 1;
		length = clipboard_paths(text, length, size);
	}

	/* Succeeded: what to paste. */
	printf("ZTERM DROP bytes=%lu uris=%d\n", (unsigned long)length, uris);
	fflush(stdout);
	return length;
}

/*
 * Turns a "text/uri-list" in a buffer into the words a shell takes: each
 * file:// line's path, %XX decoded, in single quotes and followed by a
 * space.  Returns the new length (the buffer is written over).
 */
static size_t
clipboard_paths(
	char *text,
	size_t length,
	size_t size)
{
	char *copy;
	const char *line;
	const char *end;
	const char *from;
	size_t used;
	int high;
	int low;
	int file;

	/* A copy to read from while the buffer is written. */
	copy = malloc(length + 1U);
	if (copy == NULL)
		return 0;
	memcpy(copy, text, length);
	copy[length] = '\0';

	/* Each line. */
	used = 0;
	line = copy;
	while (line < copy + length) {
		/* The line's end. */
		end = line;
		while (end < copy + length && *end != '\n' && *end != '\r')
			end++;

		/* A file URI's path (after "file://" and an optional host). */
		from = NULL;
		file = (size_t)(end - line) > 7U;
		if (file)
			file = memcmp(line, "file://", 7U) == 0;
		if (file) {
			from = line + 7;
			while (from < end && *from != '/')
				from++;
		}

		/* The path in single quotes (a quote in it closes, escapes and reopens), and a space. */
		if (from != NULL && from < end && used + 3U < size) {
			text[used++] = '\'';
			while (from < end && used + 6U < size) {
				/* A %XX byte, or the character itself. */
				high = -1;
				low = -1;
				if (*from == '%' && from + 2 < end) {
					high = clipboard_hex(from[1]);
					low = clipboard_hex(from[2]);
				}

				/* The byte a quote is written as, or itself. */
				if (high >= 0 && low >= 0) {
					text[used] = (char)(high * 16 + low);
					from += 3;
				} else {
					text[used] = *from;
					from++;
				}

				/* A quote closes, escapes and reopens. */
				if (text[used] == '\'') {
					memcpy(text + used, "'\\''", 4U);
					used += 3U;
				}

				/* The next byte. */
				used++;
			}

			/* The closing quote and a space. */
			text[used++] = '\'';
			text[used++] = ' ';
		}

		/* The next line. */
		line = end + 1;
	}

	/* Succeeded: the words. */
	free(copy);
	return used;
}

/* Returns a hexadecimal digit's value, or -1. */
static int
clipboard_hex(
	int character)
{
	/* The three ranges. */
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Not a digit. */
	return -1;
}
