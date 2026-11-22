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
 * files and last as long as the run; pixels is the font's
 * size at 96 dots an inch (pixels_given when the user chose it, which
 * wins over the size kept), scale the screen's in 96ths (0 for 96).
 * columns and rows are the grid the window opens with; timeout ends the
 * run after so many seconds (0 for none); token names the run in the log
 * lines; about is what Help > About writes on the screen (NULL for
 * nothing); copy_on_select puts the text the pointer selects on the
 * clipboard as well (0: only Edit > Copy does).  connector opens each tab's connection; new_window opens a new
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
	int copy_on_select;
	const struct terminal_connector *connector;
	void (*new_window)(void *data, struct kl_app *app, struct kl_window *from);
	void (*ended)(void *data, const struct terminal_run_result *result);
	void *data;
};

int terminal_run(const struct terminal_run_options *options, struct terminal_run_result *result);

#endif
