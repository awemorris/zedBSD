/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The accent colours the user chooses from (ws179-p001, plan/ws179/design.md
 * section 9.1), for the programs that draw with them: libkeiland's theme
 * (and through it every application) and the compositor's own interface.
 * The one table lives here, in a header both include, so that the
 * compositor does not call into libkeiland for it (the boundary of
 * plan/guardrail.md D4).
 *
 * Each colour has a value in the light appearance and one in the dark: the
 * accent itself, the ink of text and marks drawn on it, the accent as the
 * colour of text on the window's ground, and the selection's ground (the
 * accent with an opacity).  Colours are 0xAARRGGBB.
 */

#ifndef KEILAND_ARTWORK_ACCENT_H
#define KEILAND_ARTWORK_ACCENT_H

#include <stdint.h>

/* How many accents there are; the index 0, blue, is the default. */
#define KA_ACCENTS		8U

/* The appearances a value is for (the same numbers as KL_APPEARANCE_*). */
#define KA_LIGHT		0U
#define KA_DARK			1U

/* One accent in one appearance. */
struct ka_accent {
	uint32_t accent;
	uint32_t ink;
	uint32_t text;
	uint32_t selection;
};

/* The black of the ink on a light accent, the white of the ink on a dark one. */
#define KA_INK_BLACK		0xff16191fU
#define KA_INK_WHITE		0xffffffffU

/*
 * Reports an accent's values in an appearance (an index past the last is
 * the default, an appearance other than dark is light).
 */
static inline void
ka_accent_values(
	unsigned index,
	unsigned appearance,
	struct ka_accent *out)
{
	/*
	 * The table, by index and then light and dark: the accent, its ink,
	 * the accent as text, and the selection's opacity.  Blue keeps the
	 * values the desktop had before the choice (design.md section 10).
	 * The table is constant for the life of the program.
	 */
	static const uint32_t table[KA_ACCENTS][2][4] = {
		{ { 0x2f7cf6U, KA_INK_WHITE, 0x2f7cf6U, 40U }, { 0x2f7cf6U, KA_INK_WHITE, 0x2f7cf6U, 70U } },
		{ { 0x8553f5U, KA_INK_WHITE, 0x6526f2U, 40U }, { 0x9367f7U, KA_INK_BLACK, 0xc0a6faU, 70U } },
		{ { 0xdb2676U, KA_INK_WHITE, 0xa91c5aU, 40U }, { 0xe1488cU, KA_INK_BLACK, 0xee95bcU, 70U } },
		{ { 0xdc2f3cU, KA_INK_WHITE, 0xaf1d28U, 40U }, { 0xe1505aU, KA_INK_BLACK, 0xee999fU, 70U } },
		{ { 0xd4690bU, KA_INK_BLACK, 0x934908U, 40U }, { 0xe8730cU, KA_INK_BLACK, 0xf7a65eU, 70U } },
		{ { 0xa48207U, KA_INK_BLACK, 0x735b05U, 40U }, { 0xf5c518U, KA_INK_BLACK, 0xf6cc32U, 70U } },
		{ { 0x1e9a53U, KA_INK_BLACK, 0x156b3aU, 40U }, { 0x1f9d55U, KA_INK_BLACK, 0x29cf70U, 70U } },
		{ { 0x5b6472U, KA_INK_WHITE, 0x4d5460U, 60U }, { 0x798494U, KA_INK_BLACK, 0xb9bfc7U, 90U } }
	};
	const uint32_t *row;
	unsigned side;

	/* The row: the default for an index it does not have, light for anything but dark. */
	if (index >= KA_ACCENTS)
		index = 0U;
	side = 0U;
	if (appearance == KA_DARK)
		side = 1U;
	row = table[index][side];

	/* The values, opaque but for the selection. */
	out->accent = 0xff000000U | row[0];
	out->ink = row[1];
	out->text = 0xff000000U | row[2];
	out->selection = (row[3] << 24) | row[0];
}

#endif
