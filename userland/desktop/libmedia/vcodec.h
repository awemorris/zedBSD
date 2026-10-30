/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The conventions shared by every part of libmedia's software video
 * codecs: the parts that do not depend on one codec (the vcodec-* files)
 * and the parts of each codec.
 *
 * Every function that reads a bitstream treats it as untrusted: it never
 * reads outside its buffers, never indexes an array with a value it has
 * not checked against the array, and never loops without a bound that the
 * buffer sizes or the codec's limits give.  A failure is returned, never
 * asserted.
 *
 * Arithmetic never relies on undefined or implementation-defined
 * behaviour, whatever the bitstream holds: no signed overflow (products
 * of levels and scales, sums of picture order counts, scaled motion
 * vectors are computed in int64_t where int32_t could overflow); no left
 * shift of a negative value (a formula's "x << n" on a signed x is
 * written as x * (1 << n)); and a value is clamped or wrapped by
 * explicit arithmetic to the range of a narrower type before it is
 * stored in it.
 *
 * The error values, used alike by every part:
 *
 *   0        Succeeded.
 *   EINVAL   The bitstream breaks the syntax or the semantics of the
 *            specification (a value out of its range, a read past the end
 *            of a NAL unit, a reference to an absent parameter set).
 *   ENOTSUP  The bitstream is valid but uses a tool or a format outside
 *            the range the codec takes.
 *   ENOMEM   An allocation failed, or a frame pool reached its limit.
 *   ENOBUFS  A buffer or queue the caller sized is too small (a caller's
 *            mistake, or an output queue the caller has not emptied).
 *   EBUSY    The decoder holds output the caller must take first.
 *
 * No part keeps global state: everything lives in the structures a
 * decoder or an encoder owns, so two of them run on two threads without
 * locks.
 */

#ifndef LIBMEDIA_VCODEC_H
#define LIBMEDIA_VCODEC_H

#include <stddef.h>
#include <stdint.h>

/*
 * The clause, table and equation numbers in the comments of the vcodec
 * files are those of ITU-T H.264 (08/2021) where no codec is named, and
 * those of ITU-T H.265 (V11) (01/2026) where H.265 is named.
 */

/*
 * Marks a parameter a function does not use, so the compiler does not
 * warn about it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name) ((void)(name))
#endif

#endif
