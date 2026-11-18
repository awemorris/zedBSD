/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The colours of Files in the light appearance and in the dark one
 * (ws089-p017).  The light set is the colours Files always had; the dark
 * set turns the greys over -- a dark ground and panels, light text -- and
 * keeps the accent and the folders' blue.  Its text on its panels keeps a
 * contrast of 4.5 or more (plan/ws089/tests/host-dark.c).  The set in use
 * takes the accent the user chose (ws179-p001) over blue: the accent, the
 * selection, the ink on the accent and the accent as text.
 */

#include "files.h"

#include <keiland/keiland.h>

/* The light appearance's colours. */
static const struct fm_palette palette_light = {
	KL_RGB(0xeef2f7),		/* background_top */
	KL_RGB(0xe6ebf3),		/* background_bottom */
	KL_RGB(0xffffff),		/* panel */
	KL_RGB(0xe2e7ef),		/* panel_edge */
	KL_RGBA(0xffffff, 170),		/* panel_rim */
	KL_RGBA(0xffffff, 120),		/* sidebar */
	KL_RGBA(0xffffff, 40),		/* glass_sidebar */
	KL_RGBA(0xffffff, 60),		/* glass_content */
	KL_RGBA(0x1f3a66, 34),		/* shadow */
	KL_RGB(0x1e2632),		/* text */
	KL_RGB(0x56606f),		/* text_secondary */
	KL_RGB(0xa3abb8),		/* text_faint */
	KL_RGB(0x2e3a4c),		/* title */
	KL_RGB(0x46526a),		/* icon */
	KL_RGB(0x2f7cf6),		/* accent */
	KL_RGBA(0x2f7cf6, 40),		/* selection */
	KL_RGBA(0x7a8699, 38),		/* selection_inactive */
	KL_RGBA(0x5a6b85, 18),		/* hover */
	KL_RGB(0x5aa2f5),		/* folder */
	KL_RGB(0xe8ecf2),		/* separator */
	KL_RGB(0xeef1f6),		/* button */
	KL_RGB(0xe2e7ef),		/* button_lit */
	KL_RGB(0xf4f6fa),		/* inner */
	KL_RGBA(0xffffff, 150),		/* tile */
	KL_RGB(0xe6ebf2),		/* rail */
	KL_RGB(0xffffff),		/* accent_ink */
	KL_RGB(0x2f7cf6),		/* accent_text */
};

/* The dark appearance's colours. */
static const struct fm_palette palette_dark = {
	KL_RGB(0x1b1f26),		/* background_top */
	KL_RGB(0x16191f),		/* background_bottom */
	KL_RGB(0x23272f),		/* panel */
	KL_RGB(0x343a45),		/* panel_edge */
	KL_RGBA(0x3a404b, 170),		/* panel_rim */
	KL_RGBA(0x1b1f26, 120),		/* sidebar */
	KL_RGBA(0x000000, 40),		/* glass_sidebar */
	KL_RGBA(0x000000, 60),		/* glass_content */
	KL_RGBA(0x000000, 70),		/* shadow */
	KL_RGB(0xe9edf3),		/* text */
	KL_RGB(0xa9b2bf),		/* text_secondary */
	KL_RGB(0x646d7a),		/* text_faint */
	KL_RGB(0xdde3ec),		/* title */
	KL_RGB(0xc1c9d6),		/* icon */
	KL_RGB(0x2f7cf6),		/* accent */
	KL_RGBA(0x2f7cf6, 70),		/* selection */
	KL_RGBA(0x8a96aa, 50),		/* selection_inactive */
	KL_RGBA(0xffffff, 18),		/* hover */
	KL_RGB(0x5aa2f5),		/* folder */
	KL_RGB(0x2e333c),		/* separator */
	KL_RGB(0x2f3540),		/* button */
	KL_RGB(0x3a414d),		/* button_lit */
	KL_RGB(0x2a2f38),		/* inner */
	KL_RGBA(0x2a2f38, 150),		/* tile */
	KL_RGB(0x3f4652),		/* rail */
	KL_RGB(0xffffff),		/* accent_ink */
	KL_RGB(0x2f7cf6),		/* accent_text */
};

/*
 * The set in use, written by fm_palette_set and fm_palette_take: the
 * appearance's with the accent's colours (the main thread alone writes it).
 */
static struct fm_palette palette_now;

/* The set in use: the light one until the compositor tells the dark appearance. */
const struct fm_palette *fm_palette = &palette_light;

/*
 * Uses the set of an appearance (KL_APPEARANCE_*; any other value is
 * light) with the accent the user chose, as libkeiland's theme has it now.
 */
void
fm_palette_set(
	unsigned appearance)
{
	const struct kl_theme *theme;
	struct kl_accent values;

	/* The theme's accent colours (the theme follows the appearance and the accent told). */
	theme = kl_theme_default();
	values.accent = theme->accent;
	values.ink = theme->accent_ink;
	values.text = theme->accent_text;
	values.selection = theme->selection;

	/* The appearance's set with them. */
	fm_palette_take(appearance, &values);
}

/*
 * Uses the set of an appearance with an accent's colours (the desktop's
 * icons, which stay light, take the accent's light colours this way).
 */
void
fm_palette_take(
	unsigned appearance,
	const struct kl_accent *values)
{
	/* The appearance's set, and the accent's colours over its blue. */
	palette_now = *fm_palette_of(appearance);
	palette_now.accent = values->accent;
	palette_now.selection = values->selection;
	palette_now.accent_ink = values->ink;
	palette_now.accent_text = values->text;
	fm_palette = &palette_now;
}

/*
 * Reports the set of an appearance (KL_APPEARANCE_*; any other value is
 * light).
 */
const struct fm_palette *
fm_palette_of(
	unsigned appearance)
{
	/* The dark set. */
	if (appearance == KL_APPEARANCE_DARK)
		return &palette_dark;

	/* The light set. */
	return &palette_light;
}
