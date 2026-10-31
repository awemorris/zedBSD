/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conventions shared by libmedia's software video codecs (the H.264
 * and H.265 decoders and encoders) and the arithmetic helpers they all
 * need to stay clear of undefined and implementation-defined behaviour.
 *
 * The error values, used alike by every part of every codec:
 *
 *   0        Succeeded.
 *   EINVAL   The bitstream breaks the syntax or the semantics of its
 *            specification (a value out of its range, a read past the end
 *            of a NAL unit, a reference to an absent parameter set), or a
 *            caller broke a documented precondition.
 *   ENOTSUP  The bitstream is valid but uses a tool or a format outside
 *            the range the codec takes.
 *   ENOMEM   An allocation failed, or a pool reached its limit.
 *   ENOBUFS  A buffer or queue the caller sized is too small.
 *   EBUSY    The codec holds output the caller must take first.
 *
 * Arithmetic: no signed overflow (products that can exceed int32_t are
 * computed in int64_t); no left shift of a negative value (a formula's
 * "x << n" on a signed x is written x * (1 << n) or with
 * vcodec_shl32); no right shift of a negative value (a formula's
 * "x >> n" on a value that can be negative uses vcodec_asr32 or
 * vcodec_asr64, which give the floor the specifications mean); a value
 * is clamped (vcodec_clip3) or wrapped by explicit arithmetic before it
 * is stored in a narrower type.
 *
 * No part keeps global state other than constant tables.
 */

#ifndef LIBMEDIA_VCODEC_H
#define LIBMEDIA_VCODEC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Marks a parameter a function does not use, so the compiler does not
 * warn about it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name) ((void)(name))
#endif

/*
 * Returns Floor( value / 2^shift ), the meaning of "value >> shift" in
 * ITU-T H.264 5.7 and ITU-T H.265 5.7 for a value of any sign (two's
 * complement arithmetic right shift).  shift is 0..31; a larger shift
 * gives 0 for value >= 0 and -1 for value < 0.
 */
int32_t vcodec_asr32(int32_t value, unsigned shift);

/* As vcodec_asr32 for 64-bit values; shift 0..63, larger as above. */
int64_t vcodec_asr64(int64_t value, unsigned shift);

/*
 * Returns value * 2^shift, the meaning of "value << shift" for a value
 * of any sign, computed by multiplication.  The caller guarantees that
 * the result fits in int32_t (shift 0..30).
 */
int32_t vcodec_shl32(int32_t value, unsigned shift);

/* Returns Clip3( low, high, value ) (ITU-T H.265 5.8): low when value < low, high when value > high, else value.  low <= high. */
int32_t vcodec_clip3(int32_t low, int32_t high, int32_t value);

/* Returns Clip3( 0, 255, value ) as a sample of 8 bits (Clip1Y and Clip1C with a bit depth of 8). */
uint8_t vcodec_clip_pixel(int32_t value);

#endif
