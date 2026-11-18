/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The standard data sources, as libjpeg's: bytes in memory, or a stdio
 * stream read a buffer at a time.  At the end of the data both warn and
 * give a fake EOI marker, so that a cut file decodes what it has.  The
 * source managers live in the permanent pool and are reused by a later
 * call of the same kind.
 */

#include "internal.h"

#include <string.h>

/* The size of a stdio source's buffer. */
#define JPEG_STDIO_BUFFER	4096U

/*
 * A stdio source: the manager, the stream, its buffer, and whether
 * nothing has been read yet (an empty file is an error, not a cut one).
 */
struct jpeg_stdio_source {
	struct jpeg_source_mgr pub;
	FILE *file;
	JOCTET *buffer;
	boolean start_of_file;
};

/*
 * A memory source: the manager and the fake EOI it gives at the end.
 */
struct jpeg_memory_source {
	struct jpeg_source_mgr pub;
	JOCTET eoi[2];
};

static void jpeg_compat_init_source(j_decompress_ptr cinfo);
static boolean jpeg_compat_fill_memory(j_decompress_ptr cinfo);
static boolean jpeg_compat_fill_stdio(j_decompress_ptr cinfo);
static void jpeg_compat_skip(j_decompress_ptr cinfo, long num_bytes);
static void jpeg_compat_term_source(j_decompress_ptr cinfo);

/*
 * Reads the JPEG data from a stdio stream the program opened (and closes).
 */
void
jpeg_stdio_src(
	j_decompress_ptr cinfo,
	FILE *infile)
{
	struct jpeg_stdio_source *source;

	/* The manager, made once (a memory source's is replaced). */
	source = (struct jpeg_stdio_source *)cinfo->src;
	if (cinfo->src == NULL || cinfo->src->fill_input_buffer != jpeg_compat_fill_stdio) {
		source = jpeg_compat_alloc(cinfo, JPOOL_PERMANENT, sizeof(*source));
		source->buffer = jpeg_compat_alloc(cinfo, JPOOL_PERMANENT, JPEG_STDIO_BUFFER);
		cinfo->src = &source->pub;
	}

	/* The methods and the stream, with nothing read yet. */
	source->pub.init_source = jpeg_compat_init_source;
	source->pub.fill_input_buffer = jpeg_compat_fill_stdio;
	source->pub.skip_input_data = jpeg_compat_skip;
	source->pub.resync_to_restart = jpeg_resync_to_restart;
	source->pub.term_source = jpeg_compat_term_source;
	source->pub.next_input_byte = NULL;
	source->pub.bytes_in_buffer = 0;
	source->file = infile;
	source->start_of_file = TRUE;
}

/*
 * Reads the JPEG data from bytes in memory (which must stay until the
 * image is finished).
 */
void
jpeg_mem_src(
	j_decompress_ptr cinfo,
	const unsigned char *inbuffer,
	unsigned long insize)
{
	struct jpeg_memory_source *source;

	/* No data at all is an error, as in libjpeg. */
	if (inbuffer == NULL || insize == 0)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_INPUT_EMPTY);

	/* The manager, made once (a stdio source's is replaced). */
	source = (struct jpeg_memory_source *)cinfo->src;
	if (cinfo->src == NULL || cinfo->src->fill_input_buffer != jpeg_compat_fill_memory) {
		source = jpeg_compat_alloc(cinfo, JPOOL_PERMANENT, sizeof(*source));
		cinfo->src = &source->pub;
	}

	/* The methods and the bytes. */
	source->pub.init_source = jpeg_compat_init_source;
	source->pub.fill_input_buffer = jpeg_compat_fill_memory;
	source->pub.skip_input_data = jpeg_compat_skip;
	source->pub.resync_to_restart = jpeg_resync_to_restart;
	source->pub.term_source = jpeg_compat_term_source;
	source->pub.next_input_byte = inbuffer;
	source->pub.bytes_in_buffer = (size_t)insize;
	source->eoi[0] = 0xFF;
	source->eoi[1] = JPEG_EOI;
}

/*
 * The default recovery when the restart marker found is not the one
 * expected: the decoder keeps going from where it is (the blocks up to
 * the next marker it meets are decoded as they come).  Returns TRUE: the
 * decoder may continue.
 */
boolean
jpeg_resync_to_restart(
	j_decompress_ptr cinfo,
	int desired)
{
	/* The mismatch is reported; the data after it is used as it is. */
	cinfo->err->msg_parm.i[0] = cinfo->master->marker;
	cinfo->err->msg_parm.i[1] = desired;
	jpeg_compat_warn((j_common_ptr)cinfo, JWRN_MUST_RESYNC);
	return TRUE;
}

/* Starts reading (nothing to do: the bytes are there, or the first fill reads them). */
static void
jpeg_compat_init_source(
	j_decompress_ptr cinfo)
{
	(void)cinfo;
}

/* The end of a memory source: a warning, then a fake EOI marker. */
static boolean
jpeg_compat_fill_memory(
	j_decompress_ptr cinfo)
{
	struct jpeg_memory_source *source;

	/* The cut data is reported, and EOI ends it. */
	source = (struct jpeg_memory_source *)cinfo->src;
	jpeg_compat_warn((j_common_ptr)cinfo, JWRN_JPEG_EOF);
	source->pub.next_input_byte = source->eoi;
	source->pub.bytes_in_buffer = sizeof(source->eoi);
	return TRUE;
}

/* Reads the next buffer of a stdio source; at the end, a warning and a fake EOI. */
static boolean
jpeg_compat_fill_stdio(
	j_decompress_ptr cinfo)
{
	struct jpeg_stdio_source *source;
	size_t count;

	/* The next bytes of the stream. */
	source = (struct jpeg_stdio_source *)cinfo->src;
	count = fread(source->buffer, 1, JPEG_STDIO_BUFFER, source->file);

	/* An empty file is an error; a cut one ends with a fake EOI. */
	if (count == 0) {
		if (source->start_of_file)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_INPUT_EMPTY);
		jpeg_compat_warn((j_common_ptr)cinfo, JWRN_JPEG_EOF);
		source->buffer[0] = 0xFF;
		source->buffer[1] = JPEG_EOI;
		count = 2;
	}

	/* Succeeded: the buffer holds the next bytes. */
	source->pub.next_input_byte = source->buffer;
	source->pub.bytes_in_buffer = count;
	source->start_of_file = FALSE;
	return TRUE;
}

/* Skips bytes (a segment the decoder does not read), filling as needed. */
static void
jpeg_compat_skip(
	j_decompress_ptr cinfo,
	long num_bytes)
{
	struct jpeg_source_mgr *source;
	size_t left;

	/* Whole buffers, then the part of the last. */
	source = cinfo->src;
	if (num_bytes <= 0)
		return;
	left = (size_t)num_bytes;
	while (left > source->bytes_in_buffer) {
		left -= source->bytes_in_buffer;
		source->bytes_in_buffer = 0;
		source->fill_input_buffer(cinfo);
	}

	/* The rest of the skip in the current buffer. */
	source->next_input_byte += left;
	source->bytes_in_buffer -= left;
}

/* Ends reading (the program closes its stream; memory is the program's). */
static void
jpeg_compat_term_source(
	j_decompress_ptr cinfo)
{
	(void)cinfo;
}
