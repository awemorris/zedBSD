/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conventions shared by every part of libwebp-compat: the error
 * values, the limits, the rules every function keeps with input it cannot
 * trust, and the few helpers of arithmetic they all use.
 *
 * Every function that reads a file treats it as untrusted: it never reads
 * outside its buffers, never indexes an array with a value it has not
 * checked against the array, and never loops without a bound that the
 * buffer sizes or the format's limits give.  A failure is returned, never
 * asserted.
 *
 * Arithmetic never relies on undefined or implementation-defined
 * behaviour, whatever the file holds: no signed overflow (a product that
 * can leave int32_t is computed in a wider type, or its factors are first
 * reduced to the range the format gives them); no left shift of a
 * negative value (a formula's "x << n" on a signed x is written
 * x * (1 << n)); no right shift of a negative value (webpc_asr gives the
 * floor that the formats' ">>" means); and a value is wrapped or clamped
 * by explicit arithmetic (webpc_wrap16, webpc_clamp255) before it is
 * stored in a narrower type.
 *
 * The error values, used alike by every part:
 *
 *   0        Succeeded.
 *   EINVAL   The file breaks the format: a value out of its range, a
 *            size that contradicts another, a prefix code that is not
 *            complete, a copy from before the image's start.
 *   ENOTSUP  The file is valid but uses a part of the format this
 *            library does not decode (an animation).
 *   ENOMEM   An allocation failed, or a table would pass its limit.
 *   ENOBUFS  The data ends before the image does: a header, a chunk or a
 *            bitstream is cut short.
 *
 * No part keeps global state other than constant tables: everything lives
 * in the structures a call owns, so two decodings run on two threads
 * without locks.
 */

#ifndef LIBWEBP_COMPAT_COMMON_H
#define LIBWEBP_COMPAT_COMMON_H

#include <stddef.h>
#include <stdint.h>

/*
 * The section, figure and table numbers in the comments of these files are
 * those of RFC 9649 (WebP Image Format, November 2024) for the container,
 * the lossless format and the alpha chunk, and of RFC 6386 (VP8 Data
 * Format and Decoding Guide, November 2011) for the lossy format.  Where
 * RFC 6386's narrative and its reference decoder (its section 20) differ,
 * the reference decoder is correct (RFC 6386 section 1).
 */

/*
 * Marks a parameter a function does not use, so the compiler does not
 * warn about it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name) ((void)(name))
#endif

/* The widest and tallest lossy image: 14 bits of size (RFC 6386 9.1). */
#define WEBPC_VP8_SIDE_MAX		16383U

/* The widest and tallest lossless image: 14 bits of size minus one (RFC 9649 3.4). */
#define WEBPC_VP8L_SIDE_MAX		16384U

/* The widest and tallest canvas of an extended file: 24 bits of size minus one (RFC 9649 2.7). */
#define WEBPC_CANVAS_SIDE_MAX		16777216U

/* The largest canvas width times height an extended file may give (RFC 9649 2.7). */
#define WEBPC_CANVAS_AREA_MAX		0xffffffffU

/* The largest File Size of the RIFF header (RFC 9649 2.4: 2^32 minus 10). */
#define WEBPC_RIFF_SIZE_MAX		0xfffffff6U

/*
 * The most prefix-code table entries one lossless image (with its
 * subresolution images) may build, 64 MiB of tables.  A file can ask for
 * 65536 groups of five codes from a few hundred kilobytes; the limit keeps
 * such a file from taking memory out of all proportion to its size.  Files
 * from real encoders use a few thousand entries a group and few groups.
 * Passing it is ENOMEM.
 */
#define WEBPC_VP8L_TABLE_ENTRIES_MAX	(1UL << 24)

/*
 * Reads a little-endian unsigned integer of 2, 3 or 4 bytes at bytes; the
 * caller has checked that the bytes are there.
 */
uint32_t webpc_le16(const uint8_t *bytes);
uint32_t webpc_le24(const uint8_t *bytes);
uint32_t webpc_le32(const uint8_t *bytes);

/*
 * Clamps value to 0..255 (RFC 6386's clamping of a pixel, RFC 9649's
 * Clamp).
 */
int webpc_clamp255(int value);

/*
 * Shifts value right by shift (0..31) with the floor the formats' ">>" on
 * a signed value means: webpc_asr(-1, 1) is -1, webpc_asr(-3, 1) is -2.
 * No implementation-defined shift of a negative value is made.
 */
int32_t webpc_asr(int32_t value, unsigned shift);

/*
 * Wraps value to the 16-bit two's complement range -32768..32767, as RFC
 * 6386's reference decoder stores a coefficient in a short: the result is
 * value plus or minus a multiple of 65536.
 */
int32_t webpc_wrap16(int32_t value);

#endif
