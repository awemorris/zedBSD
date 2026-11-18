/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The font's own measures, in its design units: the size of its em, its
 * ascent, descent and line gap, and each glyph's advance.
 *
 * A caller that lays text out at fractional sizes (the Web browser) scales
 * these itself instead of taking the whole-pixel measures of
 * truetype_metrics and truetype_glyph_metrics.
 */

#include "internal.h"

#include <errno.h>

/*
 * Reports the face's design measures: units per em, ascent, descent (a
 * negative number) and line gap.
 */
int
truetype_design_metrics(
	const struct truetype_face *face,
	struct truetype_design_metrics *metrics)
{
	/* Refuses missing arguments. */
	if (face == NULL || metrics == NULL)
		return EINVAL;

	/* Copies the head and hhea values the face read when it opened. */
	metrics->units_per_em = face->units_per_em;
	metrics->ascent = face->ascent;
	metrics->descent = face->descent;
	metrics->line_gap = face->line_gap;

	/* Succeeded: the measures are filled. */
	return 0;
}

/*
 * Reports a glyph's advance width in design units.
 */
int
truetype_glyph_design_advance(
	const struct truetype_face *face,
	unsigned glyph,
	int *advance)
{
	const struct truetype_face *drawn;
	unsigned index;
	long scaled;

	/* Refuses missing arguments. */
	if (face == NULL || advance == NULL)
		return EINVAL;

	/* The face among its companions that draws the glyph (companion.c), which must have advance widths. */
	index = glyph;
	drawn = truetype_resolve_const(face, &index);
	if (drawn->hmetric_count == 0)
		return EINVAL;

	/* Glyphs past the long metrics share the last advance. */
	if (index >= drawn->hmetric_count)
		index = (unsigned)(drawn->hmetric_count - 1U);

	/* The entry must be inside the table. */
	if ((uint32_t)index * 4U + 2U > drawn->hmtx_size)
		return EINVAL;

	/* The advance is the entry's first field, in the units of the face asked (a companion's em may be another). */
	scaled = (long)truetype_u16(drawn->hmtx + (size_t)index * 4U);
	if (drawn->units_per_em != face->units_per_em && drawn->units_per_em != 0U)
		scaled = (scaled * (long)face->units_per_em + (long)drawn->units_per_em / 2L) / (long)drawn->units_per_em;

	/* Succeeded: the advance. */
	*advance = (int)scaled;
	return 0;
}
