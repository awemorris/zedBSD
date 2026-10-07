/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminal's primary selection through libkeiland's window (WS131
 * p018; the compositor's since ws035-p100): the text selected with the pointer
 * becomes the primary selection, and a middle click pastes the primary
 * selection into the shell.  While the terminal's own text is it, a paste
 * takes it directly (the window does).  Without the compositor's primary
 * selection the primary selection is the terminal's own.
 */

#include "terminal.h"

#include <stdio.h>

/*
 * Makes the selected text the primary selection (the window keeps its own
 * copy).
 */
void
terminal_primary_set(
	struct terminal_window *window,
	const char *text,
	size_t length)
{
	/* The primary selection, from the window's last input. */
	kl_window_select(window->kui, text, length);
	printf("ZTERM PRIMARY set bytes=%lu\n", (unsigned long)length);
	fflush(stdout);
}

/*
 * Receives the primary selection's text into a buffer (a middle click):
 * the terminal's own, or another client's.  Returns the bytes (0 for none).
 */
size_t
terminal_primary_receive(
	struct terminal_window *window,
	char *text,
	size_t size)
{
	size_t length;
	int own;

	/* Whose it is, and its text. */
	own = kl_window_selection_own(window->kui, KL_SELECTION_PRIMARY);
	length = kl_window_paste_primary(window->kui, text, size);

	/* Succeeded: the log line the tests read says whose. */
	if (own) {
		printf("ZTERM PRIMARY paste own bytes=%lu\n", (unsigned long)length);
	} else {
		printf("ZTERM PRIMARY paste received bytes=%lu\n", (unsigned long)length);
	}
	fflush(stdout);
	return length;
}
