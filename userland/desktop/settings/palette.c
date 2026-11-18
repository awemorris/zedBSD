/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The colours of Settings in the light appearance and in the dark one
 * (ws089-p017).  The light set is the colours Settings always had; the
 * dark set turns the greys over -- a dark ground and cards, light text --
 * and keeps the accent and the news.  Its text on its cards and controls
 * keeps a contrast of 4.5 or more (plan/ws089/tests/host-dark.c).  The set
 * in use takes the accent the user chose (ws179-p001) from libkeiland's
 * theme over blue.
 */

#include "settings.h"

/* The light appearance's colours. */
static const struct se_palette palette_light = {
	KL_RGB(0xeef2f7),		/* background_top */
	KL_RGB(0xe6ebf3),		/* background_bottom */
	KL_RGBA(0xffffff, 150),		/* panel */
	KL_RGBA(0xffffff, 170),		/* panel_edge */
	KL_RGBA(0xffffff, 40),		/* glass_sidebar */
	KL_RGBA(0xffffff, 60),		/* glass_page */
	KL_RGBA(0xffffff, 150),		/* card */
	KL_RGBA(0xffffff, 190),		/* card_edge */
	KL_RGBA(0xf4f7fb, 190),		/* tile */
	KL_RGBA(0xdfe7f3, 230),		/* tile_hover */
	KL_RGB(0x1e2632),		/* text */
	KL_RGB(0x56606f),		/* text_secondary */
	KL_RGB(0xa3abb8),		/* text_faint */
	KL_RGB(0x2e3a4c),		/* title */
	KL_RGB(0x46526a),		/* icon */
	KL_RGB(0x2f7cf6),		/* accent */
	KL_RGBA(0x2f7cf6, 40),		/* selection */
	KL_RGBA(0x7a8699, 38),		/* selection_inactive */
	KL_RGBA(0x5a6b85, 18),		/* hover */
	KL_RGBA(0x8a96aa, 60),		/* separator */
	KL_RGB(0x2fb45a),		/* good */
	KL_RGB(0xe0533d),		/* bad */
	KL_RGBA(0xffffff, 225),		/* control */
	KL_RGBA(0x8a96aa, 70),		/* control_edge */
	KL_RGB(0xffffff),		/* field */
	KL_RGB(0xc9d1dc),		/* track */
	KL_RGB(0xd3d9e2),		/* rail */
	KL_RGB(0xeef1f5),		/* faded */
	KL_RGB(0x1e2632),		/* pressed */
	KL_RGB(0x2f7cf6),		/* accent_text */
};

/* The dark appearance's colours. */
static const struct se_palette palette_dark = {
	KL_RGB(0x1b1f26),		/* background_top */
	KL_RGB(0x16191f),		/* background_bottom */
	KL_RGBA(0x262b34, 150),		/* panel */
	KL_RGBA(0x3a404b, 170),		/* panel_edge */
	KL_RGBA(0x000000, 40),		/* glass_sidebar */
	KL_RGBA(0x000000, 60),		/* glass_page */
	KL_RGBA(0x2a2f38, 150),		/* card */
	KL_RGBA(0x3a404b, 190),		/* card_edge */
	KL_RGBA(0x2a2f38, 190),		/* tile */
	KL_RGBA(0x353c48, 230),		/* tile_hover */
	KL_RGB(0xe9edf3),		/* text */
	KL_RGB(0xa9b2bf),		/* text_secondary */
	KL_RGB(0x646d7a),		/* text_faint */
	KL_RGB(0xdde3ec),		/* title */
	KL_RGB(0xc1c9d6),		/* icon */
	KL_RGB(0x2f7cf6),		/* accent */
	KL_RGBA(0x2f7cf6, 70),		/* selection */
	KL_RGBA(0x8a96aa, 50),		/* selection_inactive */
	KL_RGBA(0xffffff, 18),		/* hover */
	KL_RGBA(0x8a96aa, 50),		/* separator */
	KL_RGB(0x2fb45a),		/* good */
	KL_RGB(0xe0533d),		/* bad */
	KL_RGBA(0x2c313b, 225),		/* control */
	KL_RGBA(0x8a96aa, 80),		/* control_edge */
	KL_RGB(0x1f232a),		/* field */
	KL_RGB(0x4a515d),		/* track */
	KL_RGB(0x3f4652),		/* rail */
	KL_RGB(0x2a2e36),		/* faded */
	KL_RGB(0xffffff),		/* pressed */
	KL_RGB(0x2f7cf6),		/* accent_text */
};

/*
 * The set in use, written by se_palette_set: the appearance's with the
 * accent's colours (the main thread alone writes it).
 */
static struct se_palette palette_now;

/* The set in use: the light one until the compositor tells the dark appearance. */
const struct se_palette *se_palette = &palette_light;

/*
 * Uses the set of an appearance (KL_APPEARANCE_*; any other value is
 * light) with the accent the user chose, as libkeiland's theme has it now.
 */
void
se_palette_set(
	unsigned appearance)
{
	const struct kl_theme *theme;

	/* The appearance's set. */
	palette_now = *se_palette_of(appearance);

	/* The accent's colours over its blue. */
	theme = kl_theme_default();
	palette_now.accent = theme->accent;
	palette_now.selection = theme->selection;
	palette_now.accent_text = theme->accent_text;
	se_palette = &palette_now;
}

/*
 * Reports the set of an appearance (KL_APPEARANCE_*; any other value is
 * light).
 */
const struct se_palette *
se_palette_of(
	unsigned appearance)
{
	/* The dark set. */
	if (appearance == KL_APPEARANCE_DARK)
		return &palette_dark;

	/* The light set. */
	return &palette_light;
}
