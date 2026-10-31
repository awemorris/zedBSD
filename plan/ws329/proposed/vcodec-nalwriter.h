/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of libmedia's software video encoders: an RBSP turned
 * into a NAL unit's payload by inserting emulation prevention bytes
 * (H.264 7.4.1; H.265 7.4.2 is the same rule), and NAL units appended
 * to an access unit in the byte stream format of Annex B (a zero_byte
 * and a start code prefix before each NAL unit, H.264 B.1; H.265 B.2).
 *
 * The NAL unit header is given by the codec (one byte for H.264, two for
 * H.265); it never contains a zero byte, so the counting of zero bytes
 * for the emulation prevention starts again after it.
 */

#ifndef LIBMEDIA_VCODEC_NALWRITER_H
#define LIBMEDIA_VCODEC_NALWRITER_H

#include <stddef.h>
#include <stdint.h>

/*
 * One access unit (or any run of NAL units) in byte stream format.
 *
 * data has capacity bytes, size of them used; limit is the most bytes it
 * may grow to.  Owned by its maker (init allocates nothing, the first
 * append allocates, free frees).
 */
struct vcodec_au {
	uint8_t *data;
	size_t size;
	size_t capacity;
	size_t limit;
};

/* Makes an empty access unit buffer that may grow to limit bytes. */
void vcodec_au_init(struct vcodec_au *au, size_t limit);

/* Frees the buffer (NULL is ignored); the access unit is empty as after init. */
void vcodec_au_free(struct vcodec_au *au);

/* Empties the access unit for the next one; the buffer is kept. */
void vcodec_au_reset(struct vcodec_au *au);

/*
 * Returns the size of an RBSP after emulation prevention (7.4.1):
 * rbsp_size plus one byte for each place where two zero bytes are
 * followed by a byte of 0x00..0x03, and one more when the RBSP ends with
 * a zero byte (the 0x03 appended after a final cabac_zero_word).
 */
size_t vcodec_nal_escaped_size(const uint8_t *rbsp, size_t rbsp_size);

/*
 * Appends one NAL unit: 0x00 0x00 0x00 0x01 (zero_byte and
 * start_code_prefix_one_3bytes), header_size header bytes (1 or 2, each
 * non-zero), the RBSP with emulation prevention bytes inserted, then
 * zero_words times the three bytes 0x00 0x00 0x03 (cabac_zero_word with
 * its emulation prevention byte, H.264 9.3.4.6; the RBSP itself ends with
 * rbsp_trailing_bits, so its last byte is not zero and these follow
 * without another 0x03).
 *
 * Emulation prevention: scanning the RBSP from its first byte with a
 * count of consecutive zero bytes written, a 0x03 is written before any
 * byte of value 0x00..0x03 that follows two zero bytes, and the count
 * restarts.  The NAL unit may be empty of RBSP (rbsp_size 0, an end of
 * sequence).
 *
 * Returns 0; EINVAL (header_size not 1 or 2, a zero header byte, an
 * RBSP whose last byte is 0 with zero_words above 0); ENOBUFS when limit
 * would be passed; ENOMEM.  On failure the access unit is as before.
 */
int vcodec_nal_append(struct vcodec_au *au, const uint8_t *header, size_t header_size, const uint8_t *rbsp, size_t rbsp_size, unsigned zero_words);

#endif
