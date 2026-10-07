/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shell of browser: the compositor window, its titlebar and
 * toolbar, the tabs and the input.
 *
 * main.c hands the window mode here.  The host tests build the engine
 * without this directory and supply their own shell_run that refuses.
 * The shell and main.c use the engine through <browser/browser.h> only (the
 * engine is libbrowser.so, ws074-p057).
 */

#ifndef KEILAND_BROWSER_SHELL_H
#define KEILAND_BROWSER_SHELL_H

#include <browser/browser.h>

/* Marks a parameter a function has to take but does not use (the engine's base.h has the same). */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name)	((void)(name))
#endif

/*
 * What the command line asked of the window: the display, the page to
 * open, the window's size and the fonts.
 */
struct shell_options {
	const char *display;
	const char *start;
	unsigned width;
	unsigned height;
	const struct browser_fonts *fonts;
};

int shell_run(const struct shell_options *options);

#endif
