/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The entry of terminal's run (run.c), which a program calls with what it
 * chose: the window's names, its fonts and size, the tabs' connector
 * (connection.h) and its own ways of opening a new window and of saying
 * how the run ended.
 */

#ifndef TERMINAL_RUN_H
#define TERMINAL_RUN_H

#include "terminal.h"

struct terminal_connector;
struct terminal_page_program;

/*
 * How a run ended: whether it failed (then operation names what failed,
 * with the display's result of it and errno as it was), and why it ended
 * normally (reason).
 */
struct terminal_run_result {
	int failed;
	const char *operation;
	int result;
	int error;
	const char *reason;
};

/*
 * What a run is made of.
 *
 * application names the program to the window system ("terminal") and
 * title is the window's title when its tab has none; present is how the
 * frames are shown (KL_PRESENT_NONE for the drawing's own Vulkan,
 * KL_PRESENT_SHM for frames painted on the processor).  font, companion
 * (the monospaced font for the signs and the box drawing, NULL for none),
 * fallback (CJK, NULL for none) and emoji (the colour emoji font, read the
 * first time a character only it has is drawn, NULL for none) are font
 * files and last as long as the run; font_default is the font opened in
 * font's place when font cannot be (one the user kept that is gone; NULL:
 * the run then fails); pixels is the font's
 * size at 96 dots an inch (pixels_given when the user chose it, which
 * wins over the size kept), scale the screen's in 96ths (0 for 96).
 * columns and rows are the grid the window opens with; timeout ends the
 * run after so many seconds (0 for none); token names the run in the log
 * lines; about is what Help > About writes on the screen (NULL for
 * nothing); input_method_menu gives
 * the Session menu Disable Input Method, for connections that cannot tell
 * when the remote side reads a password (0: no such item);
 * ambiguous_wide_key makes Alt+F11 the shortcut of View > Treat
 * Ambiguous-Width Characters as Wide (0: it has none); fullscreen_alt_enter
 * makes Alt+Enter the shortcut of View > Fullscreen in the place of F11,
 * and F11 alone then reaches the connection as a key (0: F11, zedBSD's
 * own).  forget_passphrases gives the Session menu Forget Remembered
 * Passphrases and is called with data when it is chosen (NULL: no such
 * item).  page is what the settings' tab needs of the program
 * (settings-page.h; NULL: the program has no settings tab, and Shell >
 * Settings is not there), drawn with the fonts page_font and
 * page_fallback.  Whether a selection is copied as well is a setting
 * the program keeps (terminal_settings_*).  connector
 * opens each tab's connection; new_window opens a new
 * window from this one (NULL: the program has none); ended is told how the
 * run ended before anything is torn down (NULL: nobody is told).  data is
 * the program's own, given to new_window and ended.
 */
struct terminal_run_options {
	const char *application;
	const char *display;
	const char *title;
	unsigned present;
	const char *font;
	const char *font_default;
	const char *companion;
	const char *fallback;
	const char *emoji;
	unsigned pixels;
	int pixels_given;
	unsigned scale;
	unsigned columns;
	unsigned rows;
	unsigned timeout;
	const char *token;
	const char *about;
	int input_method_menu;
	int ambiguous_wide_key;
	int fullscreen_alt_enter;
	void (*forget_passphrases)(void *data);
	const struct terminal_page_program *page;
	const char *page_font;
	const char *page_fallback;
	const struct terminal_connector *connector;
	void (*new_window)(void *data, struct kl_app *app, struct kl_window *from);
	void (*ended)(void *data, const struct terminal_run_result *result);
	int refuse_terminal_drop;
	int open_sftp_menu;
	int (*can_open_sftp)(void *data, const struct terminal_connection *connection);
	int (*open_sftp)(void *data, struct terminal_connection *connection);
	int (*confirm_close)(void *data, struct terminal_window *window, struct terminal_connection *connection);
	void *data;
};

int terminal_run(const struct terminal_run_options *options, struct terminal_run_result *result);

#endif
