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

/*
 * The names of the program.  SHELL_PRODUCT_NAME is what the program is
 * called to the user (the Vulkan application's name); SHELL_APPLICATION_ID
 * is the application ID libkeiland's application gets (what the desktop
 * knows the program by, and the name of its data directory).  A build may
 * replace the ID.
 */
#define SHELL_PRODUCT_NAME	"OmniGrammaton"
#ifndef SHELL_APPLICATION_ID
#define SHELL_APPLICATION_ID	"browser"
#endif

/* Marks a parameter a function has to take but does not use (the engine's base.h has the same). */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name)	((void)(name))
#endif

/*
 * What the command line asked of the window: the display, the page to
 * open, the window's size, the fonts, and whether to draw with the CPU
 * and not try the GPU (no_gpu; a window whose GPU cannot be used draws with
 * the CPU anyway).
 */
struct shell_options {
	int no_gpu;
	const char *display;
	const char *start;
	unsigned width;
	unsigned height;
	const struct browser_fonts *fonts;
};

int shell_run(const struct shell_options *options);

#endif
