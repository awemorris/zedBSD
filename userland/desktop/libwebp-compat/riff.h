/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The WebP container: finding, in a file's bytes, the image's size and
 * features, its bitstream (lossy VP8 or lossless VP8L) and the alpha
 * chunk of a lossy image.  Nothing is decoded here beyond the first bytes
 * of the bitstream that give its size.
 */

#ifndef LIBWEBP_COMPAT_RIFF_H
#define LIBWEBP_COMPAT_RIFF_H

#include "common.h"

/* The formats of a bitstream, valued as WebPBitstreamFeatures' format. */
#define WEBPC_FORMAT_UNDEFINED		0
#define WEBPC_FORMAT_LOSSY		1
#define WEBPC_FORMAT_LOSSLESS		2

/* The VP8X flags (RFC 9649 2.7, Figure 7, in the first byte of the chunk's payload). */
#define WEBPC_VP8X_ANIMATION		0x02U
#define WEBPC_VP8X_XMP			0x04U
#define WEBPC_VP8X_EXIF			0x08U
#define WEBPC_VP8X_ALPHA		0x10U
#define WEBPC_VP8X_ICC			0x20U

/* The largest chunk payload size accepted (libwebp's limit: 2^32 - 10). */
#define WEBPC_CHUNK_SIZE_MAX		0xfffffff6U

/*
 * What the container says about an image, and where its parts are.  It
 * points into the caller's data and owns nothing; it stays valid as long
 * as that data does.
 *
 *   format         WEBPC_FORMAT_*: UNDEFINED for an animation, or while
 *                  the bitstream chunk has not been reached (an incomplete
 *                  extended file, see webpc_riff_parse).
 *   width, height  The image's size, 1 or more: the canvas of an extended
 *                  file (for a still image, also the bitstream's,
 *                  checked), else the bitstream's.
 *   has_alpha      1 or 0: the VP8X Alpha flag of an extended file; then,
 *                  once a lossless bitstream's header is read, its
 *                  alpha_is_used bit instead (RFC 9649 3.4); then 1 as
 *                  well when an ALPH chunk was found.
 *   has_animation  The VP8X Animation flag (0 without VP8X).
 *   extended       1 when the file has a VP8X chunk.
 *   bitstream      The VP8 frame (from its frame tag, RFC 6386 9.1) or the
 *                  VP8L stream (from its signature byte 0x2f, RFC 9649
 *                  3.4), or NULL when not reached.  bitstream_size is the
 *                  chunk's declared size (for a bare bitstream, the rest of
 *                  the data); with complete 1 it is known to fit in the
 *                  data; with complete 0 it may not, and only
 *                  available_size bytes are there.
 *   alpha          The payload of the ALPH chunk of an extended file (its
 *                  header byte first, RFC 9649 2.7.1.2), alpha_size bytes,
 *                  or NULL.  When several come before the bitstream, the
 *                  last one is kept.  alpha_size may be 0 or 1 (the
 *                  decoding then fails on it).  Only a lossy image uses
 *                  it; with a lossless bitstream it only sets has_alpha
 *                  (RFC 9649 2.7.1.2: the VP8L chunk carries the alpha).
 */
struct webpc_container {
	int format;
	unsigned width;
	unsigned height;
	int has_alpha;
	int has_animation;
	int extended;
	const uint8_t *bitstream;
	size_t bitstream_size;
	size_t available_size;
	const uint8_t *alpha;
	size_t alpha_size;
};

/*
 * Parses a file's container (data, size bytes; data may be NULL only when
 * size is 0).
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/webp_dec.c,
 * ParseHeadersInternal and the parsers it calls: the order of the checks
 * below and the failure each gives are those libwebp reports for the same
 * bytes (the statuses of a compatible interface), within RFC 9649
 * section 2's rules.  complete is 1 when data is the whole file (decoding), 0 when
 * it may be its first bytes only (reading the features).
 *
 * The steps, each failing as said (EINVAL: the file breaks the format;
 * ENOBUFS: the data ends too soon):
 *
 *   1. Fewer than 12 bytes: ENOBUFS, whatever they are.
 *   2. RIFF header (RFC 9649 2.4) when the data starts with "RIFF": "WEBP"
 *      must follow File Size (EINVAL); File Size at least 12 and at most
 *      WEBPC_CHUNK_SIZE_MAX (EINVAL); with complete 1, File Size + 8 at
 *      most size (ENOBUFS).  Bytes after File Size + 8 are never used.
 *      Without "RIFF" the data is parsed from its first byte (step 3 on).
 *   3. VP8X (2.7): fewer than 8 bytes left is ENOBUFS.  When the next tag
 *      is "VP8X": its size must be exactly 10 (EINVAL), the 18 bytes must
 *      be there (ENOBUFS), canvas width times height must be below 2^32
 *      (EINVAL), and the file must have a RIFF header (EINVAL).  The flags
 *      give has_alpha and has_animation, the canvas width and height.  An
 *      animation ends the parse here: it returns 0 with format UNDEFINED
 *      (the decoding layer refuses it).
 *   4. Fewer than 4 bytes left: ENOBUFS.
 *   5. With RIFF and VP8X, the chunks before the bitstream (2.7, 2.3): for
 *      each, fewer than 8 bytes left is ENOBUFS; a size above
 *      WEBPC_CHUNK_SIZE_MAX is EINVAL; the running total of the chunks
 *      (starting at 22 for "WEBP" and the VP8X chunk, each chunk adding 8
 *      plus its size rounded up to even) above File Size is EINVAL; a
 *      "VP8 " or "VP8L" tag ends the loop; otherwise the whole padded
 *      chunk must be there (ENOBUFS), an "ALPH" chunk is kept, and any
 *      chunk is skipped.
 *   6. The bitstream chunk: fewer than 8 bytes left is ENOBUFS.  A "VP8 "
 *      or "VP8L" chunk: its size above File Size - 12 (with RIFF) is
 *      EINVAL; with complete 1, above what is left after its header is
 *      ENOBUFS.  Any other bytes are a bare bitstream: lossless when there
 *      are at least 5 bytes, the first is 0x2f and the top 3 bits of the
 *      fifth are 0, else lossy, its size the rest of the data.  format is
 *      set.
 *   7. The bitstream's header.  Lossy: fewer than 10 bytes there (the
 *      bytes the data holds, available_size, not the declared size) is
 *      ENOBUFS; then (each EINVAL) the start code 9d 01 2a at byte 3, a
 *      key frame (bit 0 of the tag 0), version (bits 1..3) at most 3,
 *      show_frame (bit 4) set, the first partition's size (bits 5..23)
 *      below bitstream_size, width and height (the low 14 bits of the
 *      16-bit values at bytes 6 and 8) not 0.  Lossless: fewer than 5 bytes
 *      there (available_size) is ENOBUFS; signature 0x2f and version_number 0 (EINVAL);
 *      width, height and has_alpha from it.
 *   8. With VP8X, the bitstream's size must equal the canvas (EINVAL).
 *
 * With complete 0, an ENOBUFS met after a VP8X chunk was read in step 3 is
 * not a failure: the parse returns 0 with the canvas size, the flags, the
 * format if step 6 set it, has_alpha including an ALPH chunk found so far,
 * and bitstream NULL (the features of a file still arriving, as libwebp
 * reports them).
 *
 * Returns 0 and fills out (cleared first); on failure out is unspecified.
 */
int webpc_riff_parse(const uint8_t *data, size_t size, int complete, struct webpc_container *out);

#endif
