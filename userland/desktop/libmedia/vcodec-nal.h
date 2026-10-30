/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of libmedia's software video decoders, apart from their
 * headers: finding them in a byte stream (H.264 B.1 to B.3; H.265 B.2
 * to B.4 are the same) and removing the emulation prevention bytes to give the
 * RBSP (H.264 7.4.1; H.265 7.4.2 is the same rule).  The NAL unit header
 * (one byte in H.264, two in H.265) is read by the codec.
 */

#ifndef LIBMEDIA_VCODEC_NAL_H
#define LIBMEDIA_VCODEC_NAL_H

#include "vcodec.h"

/*
 * Finds the next NAL unit of a byte stream (B.1.1, B.2).
 *
 * Starting at *cursor in data[0..size), skips leading_zero_8bits and
 * finds a start code prefix (0x000001, which a zero_byte may precede).
 * On success, *nal and *nal_size name the NAL unit's bytes after the
 * start code up to (not including) the next start code prefix or the end
 * of data, with the trailing_zero_8bits before the next start code (and
 * the zero_byte of a four-byte start code) removed; *cursor is moved to
 * the end of the bytes consumed.
 *
 * Returns 1 when a NAL unit was found, 0 when no start code is left from
 * *cursor (then *cursor is size), never an error: bytes before the first
 * start code are skipped (B.3, a decoder starting in the middle of a
 * stream).  A NAL unit of 0 bytes (two start codes in a row) is skipped.
 * The returned bytes point into data and live as long as data does.
 */
int vcodec_nal_find(const uint8_t *data, size_t size, size_t *cursor, const uint8_t **nal, size_t *nal_size);

/*
 * Copies the payload of a NAL unit (its bytes after the NAL unit header)
 * into buffer as the RBSP, removing the emulation prevention bytes (7.4.1).
 *
 * escaped has size bytes (size 0 is valid and gives an empty RBSP);
 * buffer has capacity bytes, at least size, since the RBSP is never
 * longer.  Each 0x000003 has its 0x03 removed.  A payload that ends in
 * 0x000003 is valid (7.4.1: the 0x03 appended after a final
 * cabac_zero_word); its final 0x03 is dropped before the rest is
 * unescaped.  The unescaping uses media_rbsp_unescape (bits.h), which
 * refuses such a final 0x03, so the drop comes first.
 *
 * Returns 0 and the RBSP's length in *rbsp_size; EINVAL when the bytes
 * hold a three-byte sequence that cannot occur in a NAL unit (0x000000,
 * 0x000001, 0x000002, or 0x000003 followed by a byte above 3), or when
 * rbsp_size is NULL; ENOBUFS when capacity is smaller than the RBSP (a
 * caller's mistake).
 */
int vcodec_nal_unescape(const uint8_t *escaped, size_t size, uint8_t *buffer, size_t capacity, size_t *rbsp_size);

#endif
