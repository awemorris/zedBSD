/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The settings tab of terminal (settings-page.c) and the SSH keys it makes
 * and shows (settings-key.c).
 *
 * The settings are a tab of the window like the connections' tabs: a page
 * of libkeiland's widgets with a sidebar of four panes (the hosts of
 * ssh_config, the appearance, the behaviour, and the keys with
 * known_hosts).  What the page cannot know -- where the program keeps
 * its files, how it writes them, and the settings only it has -- the
 * program gives it (struct terminal_page_program).  The settings the run
 * carries out at once (the text's size, the theme, the ambiguous width,
 * copy-on-select) go to the run as the menus' actions.
 */

#ifndef TERMINAL_SETTINGS_PAGE_H
#define TERMINAL_SETTINGS_PAGE_H

#include "terminal.h"

/* The panes of the sidebar, in its order. */
#define TERMINAL_PANE_HOSTS		0U
#define TERMINAL_PANE_APPEARANCE	1U
#define TERMINAL_PANE_BEHAVIOR		2U
#define TERMINAL_PANE_KEYS		3U
#define TERMINAL_PANES			4U

/* What a program's setting holds: text, or a whole number. */
#define TERMINAL_ITEM_TEXT		0U
#define TERMINAL_ITEM_NUMBER		1U

/* When a program's setting takes effect: from the next connection, or from the program's next start. */
#define TERMINAL_EFFECT_CONNECTION	0U
#define TERMINAL_EFFECT_START		1U

/* The most settings of a program, and the longest value of one with its NUL. */
#define TERMINAL_ITEMS_MAX		8U
#define TERMINAL_ITEM_VALUE		512U

/* The longest file name of a key made, with its NUL, and the longest one-line public key. */
#define TERMINAL_KEY_NAME_MAX		64U
#define TERMINAL_KEY_LINE_MAX		(TERMINAL_ITEM_VALUE + 128U)

/* The most known_hosts lines the page lists, and the longest names field of one kept. */
#define TERMINAL_KNOWN_MAX		128U
#define TERMINAL_KNOWN_NAMES		96U

/*
 * One setting of the program's own, shown in a pane: the keyword it is
 * kept under, the label shown, the pane (TERMINAL_PANE_APPEARANCE or
 * _BEHAVIOR), what it holds (TERMINAL_ITEM_*) and when it takes effect
 * (TERMINAL_EFFECT_*).
 */
struct terminal_page_item {
	const char *keyword;
	const char *label;
	unsigned pane;
	unsigned kind;
	unsigned effect;
};

/*
 * What the program gives the settings tab.
 *
 * The paths are the program's own (what its file functions take): the
 * ssh_config the Hosts pane edits, the directory keys are made in (with
 * the separator the program's paths use at its end), and known_hosts;
 * each *_shown is the same as the user sees it.  read gives a file's
 * bytes (malloc'd; ENOENT when it is not there), replace writes a file
 * whole in place of the old one, create makes a file that must not be
 * there yet (EEXIST when it is; secret: only the user may read it), and
 * exists says whether a file is there.  check reads an ssh_config's text
 * and writes what is wrong with it, a line for each problem (empty when
 * nothing is; NULL: the program checks nothing).  items are the
 * program's own settings (item_count of them), whose values item_get and
 * item_set read and keep (item_set writes why a value was refused, and
 * the program takes a setting of the next connection at once).  comment
 * is the comment a new key gets (user@host).  font names the font the
 * terminal draws with, for a program without a Font setting (NULL: none
 * shown).  data is the program's own, given to every call.
 */
struct terminal_page_program {
	const char *config_path;
	const char *config_shown;
	const char *key_directory;
	const char *key_directory_shown;
	const char *known_hosts_path;
	const char *known_hosts_shown;
	int (*read)(void *data, const char *path, size_t limit, char **text, size_t *size);
	int (*replace)(void *data, const char *path, const void *bytes, size_t size);
	int (*create)(void *data, const char *path, const void *bytes, size_t size, int secret);
	int (*exists)(void *data, const char *path);
	int (*check)(void *data, const char *text, size_t size, char *report, size_t capacity);
	const struct terminal_page_item *items;
	unsigned item_count;
	int (*item_get)(void *data, const char *keyword, char *value, size_t size);
	int (*item_set)(void *data, const char *keyword, const char *value, char *why, size_t size);
	const char *comment;
	const char *font;
	void *data;
};

/*
 * What the page shows of the run's settings that take effect at once:
 * the text's size (pixels at 96 dots an inch), the theme
 * (TERMINAL_THEME_*), whether ambiguous-width characters are wide, and
 * whether a selection is copied.
 */
struct terminal_page_state {
	unsigned pixels;
	unsigned theme;
	int ambiguous_wide;
	int copy_on_select;
};

/*
 * One line of known_hosts as the Keys pane lists it: its number (from 1),
 * its marker ("@revoked", "@cert-authority" or empty), its names field
 * (cut to fit), its key's type and the key's fingerprint (empty for a key
 * the terminal does not read).
 */
struct terminal_known_line {
	unsigned line;
	char marker[20];
	char names[TERMINAL_KNOWN_NAMES];
	char type[32];
	char fingerprint[64];
};

/* The settings tab (settings-page.c). */
int terminal_page_open(const struct terminal_page_program *program, const char *font, const char *fallback);
int terminal_page_input(struct terminal_window *window, const struct kl_window_event *event);
int terminal_page_draw(struct terminal_window *window, const struct terminal_page_state *state, uint32_t width, uint32_t height, const uint32_t **pixels);
int terminal_page_moving(void);
void terminal_page_paste(const char *text, size_t length);
size_t terminal_page_copy_text(char *text, size_t size);
void terminal_page_shown(struct terminal_window *window);
void terminal_page_close(void);

/* The keys and known_hosts (settings-key.c). */
int terminal_key_make(const struct terminal_page_program *program, const char *name, const char *comment, const char *passphrase, char *line, size_t line_size, char *why, size_t why_size);
int terminal_key_public(const struct terminal_page_program *program, const char *name, char *line, size_t line_size, char *fingerprint, size_t fingerprint_size);
int terminal_key_name_valid(const char *name);
unsigned terminal_known_list(const char *text, size_t size, struct terminal_known_line *lines, unsigned capacity, unsigned *total);
int terminal_known_remove(const char *text, size_t size, unsigned line, char **out, size_t *out_size);

#endif
