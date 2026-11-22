/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The screen a new tab shows before it connects (launch.c): a list of
 * rows under a heading (the hosts of ssh\config, WSL and the settings; or
 * WSL's distributions), one of them chosen and shown in reverse, and the
 * ">>> " line under them, which holds the chosen row's text and can be
 * edited into a host typed by hand.
 *
 * The keys the tab sends come here as they are: Up and Down choose a row,
 * a row's own key (its number, or a letter) chooses it while the line is
 * not edited, another character or a paste starts a line typed by hand in
 * the label's place, Backspace and Ctrl+U edit the line, Enter takes it,
 * Escape and Ctrl+C or Ctrl+D end it.  The screen is drawn whole each time, as
 * the text and escape sequences a terminal shows.
 *
 * Nothing here reads a file or talks to Windows, so it is tried on the
 * host as it is.
 */

#ifndef MIKUTERM_LAUNCH_H
#define MIKUTERM_LAUNCH_H

#include <stddef.h>

/* The most rows a screen has (the hosts or the distributions, and the rows after them). */
#define LAUNCH_ROWS_MAX		40U

/* The longest text of a row, and of the note after it, with their NULs. */
#define LAUNCH_LABEL_MAX	320U
#define LAUNCH_NOTE_MAX		96U

/* The longest line typed, and the longest message over it, with their NULs. */
#define LAUNCH_LINE_MAX		320U
#define LAUNCH_STATUS_MAX	256U

/* The longest key of a row ("12", "w"), with its NUL. */
#define LAUNCH_KEY_MAX		4U

/* The most distributions read from WSL's list, and the longest name of one with its NUL. */
#define LAUNCH_NAMES_MAX	32U
#define LAUNCH_NAME_MAX		64U

/*
 * The colours of the chosen row: the green of the reverse (#39C5BB) and
 * the dark text on it, the same in every theme.
 */
#define LAUNCH_CHOSEN_BACK	"57;197;187"
#define LAUNCH_CHOSEN_TEXT	"16;20;24"

/* What a row is: a host, WSL, the settings, a distribution of WSL, the way back. */
#define LAUNCH_HOST		1U
#define LAUNCH_WSL		2U
#define LAUNCH_SETTINGS		3U
#define LAUNCH_DISTRIBUTION	4U
#define LAUNCH_BACK		5U

/* What the keys came to: nothing yet, Enter, the user giving up (Ctrl+C, Ctrl+D), Escape. */
#define LAUNCH_EVENT_NONE	0U
#define LAUNCH_EVENT_ENTER	1U
#define LAUNCH_EVENT_CANCEL	2U
#define LAUNCH_EVENT_ESCAPE	3U

/* Where the keys are in an escape sequence: outside, after ESC, inside a CSI or SS3 one. */
#define LAUNCH_ESCAPE_NONE	0U
#define LAUNCH_ESCAPE_START	1U
#define LAUNCH_ESCAPE_INSIDE	2U

/*
 * One row: the key that chooses it (a number or a letter, shown in
 * brackets), what it is, the text shown and put on the line when it is
 * chosen, and a note shown after it (empty: none).
 */
struct launch_row {
	char key[LAUNCH_KEY_MAX];
	unsigned kind;
	char label[LAUNCH_LABEL_MAX];
	char note[LAUNCH_NOTE_MAX];
};

/*
 * One screen: its heading, its rows and the chosen one, the line (the
 * chosen row's label until edited), the number typed so far as a row's
 * key, where the keys are in an escape sequence and whether they are
 * pasted (text, not keys), and the message shown over the line.
 */
struct launch_screen {
	char heading[LAUNCH_LABEL_MAX];
	struct launch_row rows[LAUNCH_ROWS_MAX];
	unsigned count;
	unsigned chosen;
	char line[LAUNCH_LINE_MAX];
	size_t line_length;
	int edited;
	unsigned number;
	unsigned escape;
	char sequence[16];
	size_t sequence_length;
	int pasting;
	char status[LAUNCH_STATUS_MAX];
};

void launch_init(struct launch_screen *screen, const char *heading);
int launch_add(struct launch_screen *screen, const char *key, unsigned kind, const char *label, const char *note);
void launch_choose(struct launch_screen *screen, unsigned index);
void launch_say(struct launch_screen *screen, const char *text);
unsigned launch_keys(struct launch_screen *screen, const unsigned char *bytes, size_t size, size_t *used);
int launch_row_taken(const struct launch_screen *screen);
size_t launch_draw(const struct launch_screen *screen, const char *title, const char *notice, unsigned columns, unsigned rows, char *out, size_t size);
size_t launch_leave(char *out, size_t size);
void launch_output_text(const unsigned char *bytes, size_t size, char *text, size_t text_size);
unsigned launch_names(const char *text, char names[][LAUNCH_NAME_MAX], unsigned most);
int launch_first_line(const char *text, char *line, size_t size);

#endif
