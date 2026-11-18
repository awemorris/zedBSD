/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The theme of the library: the Kei look's colours and sizes, the values
 * the file manager's specification set (plan/ws071/spec.md, Files'
 * files.h) and Settings' (plan/ws089, settings.h and widgets.c: the cards,
 * the controls, the switches), so that every widget drawn with it matches
 * Files and Settings.  The accent, the selection, the ink on the accent and
 * the accent as text come from the accent the user chose (ws179-p001,
 * artwork/accent.h); the tables below hold blue's.
 */

#include <keiland/keiland.h>

#include "internal.h"

#include "../../artwork/accent.h"

/*
 * The light theme, the program's until the compositor tells the dark
 * appearance (ws089-p017).
 */
static const struct kl_theme theme_light = {
	KL_RGB(0xeef2f7),		/* ground_top */
	KL_RGB(0xe6ebf3),		/* ground_bottom */
	KL_RGB(0xffffff),		/* panel */
	KL_RGB(0xe2e7ef),		/* panel_edge */
	KL_RGBA(0x1f3a66, 34),		/* shadow */
	KL_RGBA(0xffffff, 40),		/* glass_sidebar */
	KL_RGBA(0xffffff, 60),		/* glass_content */
	KL_RGBA(0xffffff, 120),	/* sidebar */
	KL_RGB(0x1e2632),		/* text */
	KL_RGB(0x56606f),		/* text_secondary */
	KL_RGB(0xa3abb8),		/* text_faint */
	KL_RGB(0x46526a),		/* icon */
	KL_RGB(0x2f7cf6),		/* accent */
	KL_RGBA(0x2f7cf6, 40),		/* selection */
	KL_RGBA(0x7a8699, 38),		/* selection_inactive */
	KL_RGBA(0x5a6b85, 18),		/* hover */
	KL_RGB(0xe8ecf2),		/* separator */
	KL_RGB(0x5aa2f5),		/* folder */
	KL_RGB(0xe5484d),		/* danger */
	16.0f,				/* card_radius */
	8.0f,				/* control_radius */
	28,				/* row_height */
	13U,				/* text_body */
	12U,				/* text_small */
	15U,				/* text_title */
	KL_RGBA(0xffffff, 150),	/* card */
	KL_RGBA(0xffffff, 190),	/* card_edge */
	KL_RGBA(0x8a96aa, 60),		/* row_separator */
	KL_RGBA(0xffffff, 225),	/* control */
	KL_RGBA(0x8a96aa, 70),		/* control_edge */
	KL_RGB(0xc9d1dc),		/* track */
	KL_RGB(0x2fb45a),		/* good */
	KL_RGB(0xe0533d),		/* bad */
	32,				/* control_height */
	44,				/* switch_width */
	24,				/* switch_height */
	KL_RGB(0xffffff),		/* accent_ink */
	KL_RGB(0x2f7cf6)		/* accent_text */
};

/*
 * The dark theme (ws089-p017): the light one's greys turned over -- a dark
 * ground and cards, light text -- with the accent, the selection, the
 * folder and the news kept; the sizes are the light theme's.  Its text on
 * its panel, its cards and its controls keeps a contrast of 4.5 or more
 * (plan/ws089/tests/host-dark.c).
 */
static const struct kl_theme theme_dark = {
	KL_RGB(0x1b1f26),		/* ground_top */
	KL_RGB(0x16191f),		/* ground_bottom */
	KL_RGB(0x23272f),		/* panel */
	KL_RGB(0x343a45),		/* panel_edge */
	KL_RGBA(0x000000, 70),		/* shadow */
	KL_RGBA(0x000000, 40),		/* glass_sidebar */
	KL_RGBA(0x000000, 60),		/* glass_content */
	KL_RGBA(0x1b1f26, 120),	/* sidebar */
	KL_RGB(0xe9edf3),		/* text */
	KL_RGB(0xa9b2bf),		/* text_secondary */
	KL_RGB(0x646d7a),		/* text_faint */
	KL_RGB(0xc1c9d6),		/* icon */
	KL_RGB(0x2f7cf6),		/* accent */
	KL_RGBA(0x2f7cf6, 70),		/* selection */
	KL_RGBA(0x8a96aa, 50),		/* selection_inactive */
	KL_RGBA(0xffffff, 18),		/* hover */
	KL_RGB(0x2e333c),		/* separator */
	KL_RGB(0x5aa2f5),		/* folder */
	KL_RGB(0xe5484d),		/* danger */
	16.0f,				/* card_radius */
	8.0f,				/* control_radius */
	28,				/* row_height */
	13U,				/* text_body */
	12U,				/* text_small */
	15U,				/* text_title */
	KL_RGBA(0x2a2f38, 150),	/* card */
	KL_RGBA(0x3a404b, 190),	/* card_edge */
	KL_RGBA(0x8a96aa, 50),		/* row_separator */
	KL_RGBA(0x2c313b, 225),	/* control */
	KL_RGBA(0x8a96aa, 80),		/* control_edge */
	KL_RGB(0x4a515d),		/* track */
	KL_RGB(0x2fb45a),		/* good */
	KL_RGB(0xe0533d),		/* bad */
	32,				/* control_height */
	44,				/* switch_width */
	24,				/* switch_height */
	KL_RGB(0xffffff),		/* accent_ink */
	KL_RGB(0x2f7cf6)		/* accent_text */
};

/*
 * The theme handed out: a copy of the light or the dark one, made when
 * first asked for and again when the appearance changes, so that the
 * pointer applications keep shows the appearance of now.
 */
static struct kl_theme theme_now;
static int theme_made;

/*
 * Reports the theme applications draw with: the light one, or the dark
 * one once the compositor told the dark appearance (appearance.c).
 */
const struct kl_theme *
kl_theme_default(void)
{
	/* The light theme until an appearance is told. */
	if (!theme_made) {
		theme_now = theme_light;
		theme_made = 1;
	}

	/* Succeeded. */
	return &theme_now;
}

/*
 * Makes the theme handed out the appearance's (KL_APPEARANCE_*; any other
 * value is light) with an accent's colours (KL_ACCENT_*; past the last is
 * blue).
 */
void
keiui_theme_set(
	unsigned appearance,
	unsigned accent)
{
	/* The appearance's theme, with the accent's colours. */
	keiui_theme_with(appearance, accent, &theme_now);
	theme_made = 1;
}

/*
 * Writes the theme of an appearance with an accent's colours (for the
 * theme handed out, and for the library's tests).
 */
void
keiui_theme_with(
	unsigned appearance,
	unsigned accent,
	struct kl_theme *theme)
{
	struct kl_accent values;

	/* The appearance's theme. */
	*theme = theme_light;
	if (appearance == KL_APPEARANCE_DARK)
		*theme = theme_dark;

	/* The accent's colours over its blue. */
	kl_accent_values(accent, appearance, &values);
	theme->accent = values.accent;
	theme->selection = values.selection;
	theme->accent_ink = values.ink;
	theme->accent_text = values.text;
}

/*
 * Writes an accent's colours in an appearance (keiland.h).
 */
void
kl_accent_values(
	unsigned accent,
	unsigned appearance,
	struct kl_accent *values)
{
	struct ka_accent table;

	/* The shared table's row. */
	ka_accent_values(accent, appearance, &table);
	values->accent = table.accent;
	values->ink = table.ink;
	values->text = table.text;
	values->selection = table.selection;
}

/*
 * The light theme or the dark one, for the library's tests of the
 * contrast (NULL for an unknown appearance).
 */
const struct kl_theme *
keiui_theme_of(
	unsigned appearance)
{
	/* The light theme. */
	if (appearance == KL_APPEARANCE_LIGHT)
		return &theme_light;

	/* The dark theme. */
	if (appearance == KL_APPEARANCE_DARK)
		return &theme_dark;

	/* None. */
	return NULL;
}

/*
 * Chooses a program's colour by the appearance told last (keiland.h).
 */
kl_color
kl_theme_choose(
	kl_color light,
	kl_color dark)
{
	unsigned appearance;

	/* The dark colour in the dark appearance. */
	appearance = kl_appearance_get(NULL);
	if (appearance == KL_APPEARANCE_DARK)
		return dark;

	/* The light one otherwise. */
	return light;
}
