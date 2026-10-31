/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one SEI message the software H.265 decoder reads: the decoded
 * picture hash (D.2.20, D.3.20), a suffix SEI message that carries an
 * MD5, a CRC or a checksum of each colour component of the decoded
 * picture.  The decoder compares it with the picture it decoded and
 * counts matches and mismatches; the conformance tests use it as an
 * oracle that does not depend on another decoder.  Every other SEI
 * message is skipped.
 *
 * MD5 is computed as IETF RFC 1321 defines it, written from the RFC.
 */

#ifndef LIBMEDIA_H265SW_SEI_H
#define LIBMEDIA_H265SW_SEI_H

#include "h265sw.h"
#include "vcodec-frame.h"

/* The payloadType of the decoded picture hash (Annex D, D.2.1). */
#define H265SW_SEI_PICTURE_HASH 132U

/* hash_type (Table D.10). */
#define H265SW_HASH_MD5 0U
#define H265SW_HASH_CRC 1U
#define H265SW_HASH_CHECKSUM 2U

/*
 * One decoded picture hash: present 1 when the access unit had one with
 * a known hash_type; type is hash_type; value[cIdx] holds the 16 bytes
 * of picture_md5, or picture_crc in its first two bytes (most
 * significant first), or picture_checksum in its first four.
 */
struct h265sw_picture_hash {
	int present;
	unsigned type;
	uint8_t value[3][16];
};

/*
 * Walks the sei_rbsp() of a suffix SEI NAL unit (7.3.2.4, 7.3.5) and
 * takes the first decoded picture hash message in it.
 *
 * rbsp and size are the NAL unit's RBSP.  payloadType and payloadSize are
 * read as the 0xFF-extended sums of 7.3.5; each payload is skipped by its
 * size.  A hash message with a reserved hash_type is ignored (D.3.20).
 * Writes *hash (present 0 when there is none).
 *
 * Returns 0; EINVAL when a size runs past the RBSP or the hash message is
 * shorter than its hash_type needs (then hash->present is 0).
 */
int h265sw_sei_read_hash(const uint8_t *rbsp, size_t size, struct h265sw_picture_hash *hash);

/*
 * Computes the hash of hash_type type over a decoded frame's three
 * components, each of its full decoded size (pic_width_in_luma_samples by
 * pic_height_in_luma_samples, chroma half of each; not the cropping
 * window), as D.3.20 arranges them (8-bit samples: one byte each, D-22):
 * MD5 by D-23, the CRC by D-24, the checksum by D-25.  Writes value as
 * struct h265sw_picture_hash holds it.  Returns 0, or EINVAL for an
 * unknown type.
 */
int h265sw_sei_compute_hash(const struct vcodec_frame *frame, unsigned type, uint8_t value[3][16]);

/*
 * Compares a received hash with the frame: returns 1 when every
 * component matches, 0 when one differs, -1 when hash is not present.
 */
int h265sw_sei_check_hash(const struct vcodec_frame *frame, const struct h265sw_picture_hash *hash);

#endif
