/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminal's own settings, kept from one run to the next (ws128-p009),
 * through the desktop's settings (libkeiland's kl_settings_*, WS135): the
 * keys terminal.ambiguous-wide (0 or 1: View > Treat Ambiguous-Width
 * Characters as Wide), terminal.font-size (the font's size in pixels, 8 to
 * 32: View > Zoom and Text Size) and terminal.theme (0 dark, 1 light, 2
 * high contrast: View > Theme; ws128-p006), which libkeiland keeps in the
 * terminal's own file (~/.config/keiland/terminal.conf).  The terminal
 * opens no settings file itself.
 *
 * The windows and tabs of the terminal that changed it follow at once;
 * another terminal reads it when it starts (an application's own setting
 * is not told to other processes, WS135 D3).
 */

#include "terminal.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <string.h>

/* The keys of the Ambiguous-width setting, of the font's size and of the theme. */
#define SETTINGS_AMBIGUOUS_WIDE	"terminal.ambiguous-wide"
#define SETTINGS_FONT_SIZE	"terminal.font-size"
#define SETTINGS_THEME		"terminal.theme"

/* The font's size the desktop's table gives when none is kept (its default, as the terminal's MAIN_FONT_PIXELS). */
#define SETTINGS_FONT_DEFAULT	16U

/* The application's name, which names its settings. */
#define SETTINGS_APP		"terminal"

/*
 * Reads the settings into settings; without them (no home, no memory) the
 * defaults (everything off) stay.
 */
void
terminal_settings_load(
	struct terminal_settings *settings)
{
	struct kl_settings *desktop;
	int value;

	/* The defaults: the terminal as it was before the setting existed. */
	memset(settings, 0, sizeof(*settings));

	/* Opens the terminal's settings; its own keys need no display. */
	desktop = kl_settings_open(NULL, SETTINGS_APP);
	if (desktop == NULL)
		return;

	/* Reads whether Ambiguous-width characters are wide. */
	settings->ambiguous_wide = kl_settings_get_int(desktop, SETTINGS_AMBIGUOUS_WIDE, 0);

	/* The font's size (the table's default, 16, when it was never kept; 0 for a value out of the range). */
	value = kl_settings_get_int(desktop, SETTINGS_FONT_SIZE, 0);
	if (value >= (int)TERMINAL_PIXELS_MIN && value <= (int)TERMINAL_PIXELS_MAX)
		settings->font_size = (unsigned)value;

	/* The theme, the dark one unless another was kept. */
	value = kl_settings_get_int(desktop, SETTINGS_THEME, 0);
	if (value > 0 && value < (int)TERMINAL_THEMES)
		settings->theme = (unsigned)value;

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(desktop);
}

/*
 * Keeps the settings for the next run.  Returns 0, or an errno value; the
 * old setting is then kept.
 */
int
terminal_settings_save(
	const struct terminal_settings *settings)
{
	struct kl_settings *desktop;
	int wide;
	int error;

	/* Opens the terminal's settings. */
	desktop = kl_settings_open(NULL, SETTINGS_APP);
	if (desktop == NULL)
		return errno;

	/* Gives the setting as 0 or 1. */
	wide = 0;
	if (settings->ambiguous_wide)
		wide = 1;

	/* Keeps it in the terminal's file; a failure leaves the old setting. */
	error = kl_settings_set_int(desktop, SETTINGS_AMBIGUOUS_WIDE, wide, NULL);
	if (error != 0) {
		kl_settings_close(desktop);
		return error;
	}

	/* The font's size, when there is one to keep; the default size (16) leaves no line in the file. */
	error = 0;
	if (settings->font_size == SETTINGS_FONT_DEFAULT) {
		error = kl_settings_reset(desktop, SETTINGS_FONT_SIZE, NULL);
	} else if (settings->font_size != 0U) {
		error = kl_settings_set_int(desktop, SETTINGS_FONT_SIZE, (int)settings->font_size, NULL);
	}

	/* A failure leaves the old size. */
	if (error != 0) {
		kl_settings_close(desktop);
		return error;
	}

	/* The theme; the dark one is the default, which leaves no line in the file. */
	if (settings->theme == TERMINAL_THEME_DARK) {
		error = kl_settings_reset(desktop, SETTINGS_THEME, NULL);
	} else {
		error = kl_settings_set_int(desktop, SETTINGS_THEME, (int)settings->theme, NULL);
	}

	/* A failure leaves the old theme. */
	if (error != 0) {
		kl_settings_close(desktop);
		return error;
	}

	/* Closes the settings, which are not needed any more. */
	kl_settings_close(desktop);

	/* Succeeded: the next run reads it. */
	return 0;
}
