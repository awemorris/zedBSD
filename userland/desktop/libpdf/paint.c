/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writer's compressed painting (ws216-p002, plan/ws216/phase002/phase.md
 * section 2): the pages' content compressed with Flate, and the grains that
 * Notes' pencil paints with.
 *
 * Both compress through libz-compat's deflate (update.c's
 * pdf_writer_pack()), which the writer itself (writer.c) does not link: a
 * program that only writes plain pages does without the compressor.  Each
 * call here gives the writer the compressor, which it then also uses for
 * the colour images of the grains' patterns.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <pdf.h>

#include "writer.h"

/*
 * Compresses the content of every page with Flate when the document is
 * saved, or stops doing so.
 *
 * A page whose content would not shrink, or is larger than the reader's
 * limit of the decoded content it hashes (64 MiB), is written as it is.
 * pdf_writer_get_page_content_hash() reports the hash of the content as it
 * was drawn, which the reader's hash of the decoded stream matches.
 */
int
pdf_writer_set_content_flate(
	struct pdf_writer *writer,
	int enabled)
{
	/* Refuses a missing writer. */
	if (writer == NULL)
		return EINVAL;

	/* The writer compresses through libz-compat from now on. */
	writer->pack = pdf_writer_pack;

	/* Whether the saves compress the pages' content. */
	writer->content_flate = 0;
	if (enabled)
		writer->content_flate = 1;

	/* Succeeded: the next save writes the content so. */
	return 0;
}

/*
 * Adds a grain to the document: an 8-bit coverage, width times height
 * bytes, the first row the top one.
 *
 * The writer keeps the coverage, compressed, as the one alpha mask that
 * every colour the grain is painted in (pdf_writer_set_fill_grain())
 * shares.  grain gets the grain's index.  Returns 0, EINVAL for a missing
 * or empty coverage or one past 1024 a side, ENOSPC past eight grains, or
 * ENOMEM.
 */
int
pdf_writer_add_grain(
	struct pdf_writer *writer,
	const unsigned char *coverage,
	size_t width,
	size_t height,
	size_t *grain)
{
	struct pdf_writer_grain *added;
	unsigned char *copy;
	unsigned char *packed;
	size_t packed_size;
	size_t size;
	int error;

	/* Refuses a missing writer, coverage or result. */
	if (writer == NULL || coverage == NULL)
		return EINVAL;
	if (grain == NULL)
		return EINVAL;

	/* Refuses a grain without area or past the side limit. */
	if (width == 0 || height == 0)
		return EINVAL;
	if (width > PDF_WRITER_GRAIN_SIDE_MAX || height > PDF_WRITER_GRAIN_SIDE_MAX)
		return EINVAL;

	/* Refuses a grain past the document's limit. */
	if (writer->grains_count == PDF_WRITER_GRAIN_MAX)
		return ENOSPC;

	/* Compresses the coverage. */
	size = width * height;
	error = pdf_writer_pack(coverage, size, &packed, &packed_size);
	if (error != 0)
		return error;

	/* Describes the grain, as a zlib stream or, when that would not be shorter, a copy of the bytes. */
	added = &writer->grains[writer->grains_count];
	memset(added, 0, sizeof(*added));
	added->width = width;
	added->height = height;
	if (packed != NULL) {
		added->data = packed;
		added->size = packed_size;
		added->flate = 1;
	} else {
		copy = malloc(size);
		if (copy == NULL)
			return ENOMEM;
		memcpy(copy, coverage, size);
		added->data = copy;
		added->size = size;
	}

	/* The writer compresses the grain's colours through libz-compat too. */
	writer->pack = pdf_writer_pack;

	/* Succeeded: the grain is the document's. */
	*grain = writer->grains_count;
	writer->grains_count++;
	return 0;
}
