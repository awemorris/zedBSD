/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminal's settings kept from one run to the next (terminal.h's
 * terminal_settings_*), in mikuterm's folder: config\state.conf holds
 * what the View menu chose (FontSize, Theme, AmbiguousWide), and wins
 * over config\mikuterm.conf, which gives the values before the menu
 * chose any.  Removing state.conf brings the user's settings back.  A
 * folder mikuterm cannot write in keeps nothing (the choice holds for the
 * run).
 */

#include "mikuterm.h"

#include "userland/desktop/terminal/terminal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the View menu's settings are kept, inside the folder. */
#define SETTINGS_STATE		"config/state.conf"

/*
 * What config\mikuterm.conf gives for the View menu's settings, set once
 * before the run starts (mikuterm_settings_defaults) and read by the run.
 */
static struct mikuterm_conf settings_conf;

/* Takes the user's settings, which the kept ones win over. */
void
mikuterm_settings_defaults(
	const struct mikuterm_conf *conf)
{
	/* A copy for the run. */
	settings_conf = *conf;
}

/*
 * Reads the settings: state.conf's, else mikuterm.conf's.  The font's size
 * is only state.conf's (0 when it has none: the run then takes the size
 * mikuterm.conf gave it).
 */
void
terminal_settings_load(
	struct terminal_settings *settings)
{
	struct mikuterm_conf state;
	char path[MIKUTERM_PATH_MAX];
	char *text;
	size_t size;
	int error;

	/* The user's settings, no size kept. */
	memset(settings, 0, sizeof(*settings));
	settings->ambiguous_wide = settings_conf.ambiguous_wide;
	settings->theme = settings_conf.theme;
	settings->font_size = 0U;

	/* The kept settings, when there are some. */
	error = mikuterm_path(path, sizeof(path), SETTINGS_STATE);
	if (error != 0)
		return;
	error = mikuterm_file_read(path, MIKUTERM_CONF_MAX, &text, &size);
	if (error != 0)
		return;

	/* What they say, each value that was given winning. */
	mikuterm_conf_defaults(&state);
	(void)mikuterm_conf_read(&state, text, size, 1);
	free(text);
	if (state.font_size_given)
		settings->font_size = state.font_size;
	if (state.theme_given)
		settings->theme = state.theme;
	if (state.ambiguous_given)
		settings->ambiguous_wide = state.ambiguous_wide;
}

/*
 * Keeps the settings in state.conf for the next run.  Returns 0, or an
 * errno value (the file is then as it was).
 */
int
terminal_settings_save(
	const struct terminal_settings *settings)
{
	char path[MIKUTERM_PATH_MAX];
	char text[256];
	const char *wide;
	int written;
	int error;

	/* Ambiguous-width characters' setting as the file writes it. */
	wide = "no";
	if (settings->ambiguous_wide)
		wide = "yes";

	/* The file's text; a size of 0 is none chosen and leaves the line out. */
	if (settings->font_size != 0U) {
		written = snprintf(text,
				   sizeof(text),
				   "# What the View menu chose; mikuterm writes this file.  Remove it to go back to mikuterm.conf.\r\n"
				   "FontSize %u\r\nTheme %s\r\nAmbiguousWide %s\r\n",
				   settings->font_size,
				   mikuterm_theme_text(settings->theme),
				   wide);
	} else {
		written = snprintf(text,
				   sizeof(text),
				   "# What the View menu chose; mikuterm writes this file.  Remove it to go back to mikuterm.conf.\r\n"
				   "Theme %s\r\nAmbiguousWide %s\r\n",
				   mikuterm_theme_text(settings->theme),
				   wide);
	}
	if (written < 0 || (size_t)written >= sizeof(text))
		return EINVAL;

	/* The file, replaced whole. */
	error = mikuterm_path(path, sizeof(path), SETTINGS_STATE);
	if (error != 0)
		return error;
	error = mikuterm_file_replace(path, text, (size_t)written);
	if (error != 0)
		return error;

	/* Succeeded: the next run reads them. */
	return 0;
}
