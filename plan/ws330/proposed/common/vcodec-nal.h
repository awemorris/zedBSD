/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of libmedia's software video decoders, the parts both
 * codecs share: finding NAL units in a byte stream (ITU-T H.264 Annex B
 * and ITU-T H.265 Annex B, the same format) and removing the emulation
 * prevention bytes of a NAL unit's payload (H.264 7.4.1, H.265 7.4.2,
 * the same rule).  The NAL unit header (one byte in H.264, two in H.265)
 * is read by each codec.
 *
 * The input is always a byte stream: libmedia's bitstream conversion
 * (media_bitstream_convert) turns a container's length-prefixed NAL
 * units into start-code-prefixed ones before a decoder sees them.
 */

#ifndef LIBMEDIA_VCODEC_NAL_H
#define LIBMEDIA_VCODEC_NAL_H

#include "vcodec.h"

/*
 * Finds the next NAL unit of a byte stream (H.264 B.1.1 and B.2, H.265
 * B.2 and B.3).
 *
 * Starting at *cursor in data[0..size), skips leading zero bytes and
 * finds a start code prefix (0x000001, which a zero_byte may precede).
 * On success, *nal and *nal_size name the NAL unit's bytes after the
 * start code up to (not including) the next start code prefix or the end
 * of data, with the trailing zero bytes before the next start code (and
 * the zero_byte of a four-byte start code) removed; *cursor moves past
 * the bytes consumed.
 *
 * Returns 1 when a NAL unit was found, 0 when no start code is left
 * (then *cursor is size); never an error: bytes before the first start
 * code are skipped, and a NAL unit of 0 bytes is skipped.  The returned
 * bytes point into data.
 */
int vcodec_nal_find(const uint8_t *data, size_t size, size_t *cursor, const uint8_t **nal, size_t *nal_size);

/*
 * Copies a NAL unit's payload (the bytes after its header) into buffer
 * with every emulation_prevention_three_byte removed.
 *
 * payload and size are the bytes after the NAL unit header; buffer has
 * capacity bytes (at least size; the RBSP is never longer).  A payload
 * that ends in 0x000003 is valid (the 0x03 appended after a final
 * cabac_zero_word, H.264 7.4.1 and H.265 7.4.2): that final 0x03 is
 * dropped.  Writes *rbsp_size.
 *
 * Returns 0; EINVAL when the payload holds a three-byte sequence that
 * cannot occur in a NAL unit (0x000000, 0x000001, 0x000002, or 0x000003
 * followed by a byte above 0x03); ENOBUFS when capacity is smaller than
 * size (a caller's mistake).  On an error *rbsp_size is 0.
 */
int vcodec_nal_unescape(const uint8_t *payload, size_t size, uint8_t *buffer, size_t capacity, size_t *rbsp_size);

#endif
