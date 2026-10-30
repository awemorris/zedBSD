/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of the software H.264 decoder: reading the NAL unit
 * header (7.3.1) and giving the RBSP (7.4.1).  Finding NAL units in a
 * byte stream (Annex B) and removing the emulation prevention bytes are
 * the codec-independent part (vcodec-nal.h).
 *
 * The input is always a byte stream: libmedia's bitstream conversion
 * (media_bitstream_convert) turns a container's length-prefixed NAL units
 * into start-code-prefixed ones before the decoder sees them.
 */

#ifndef LIBMEDIA_H264SW_NAL_H
#define LIBMEDIA_H264SW_NAL_H

#include "h264sw.h"
#include "vcodec-nal.h"

/* The nal_unit_type values the decoder acts on (Table 7-1). */
#define H264SW_NAL_SLICE 1U
#define H264SW_NAL_PARTITION_A 2U
#define H264SW_NAL_PARTITION_B 3U
#define H264SW_NAL_PARTITION_C 4U
#define H264SW_NAL_IDR 5U
#define H264SW_NAL_SEI 6U
#define H264SW_NAL_SPS 7U
#define H264SW_NAL_PPS 8U
#define H264SW_NAL_AUD 9U
#define H264SW_NAL_END_SEQUENCE 10U
#define H264SW_NAL_END_STREAM 11U
#define H264SW_NAL_FILLER 12U
#define H264SW_NAL_SPS_EXTENSION 13U
#define H264SW_NAL_PREFIX 14U
#define H264SW_NAL_SUBSET_SPS 15U
#define H264SW_NAL_DEPTH_PS 16U
#define H264SW_NAL_AUXILIARY 19U
#define H264SW_NAL_SLICE_EXTENSION 20U
#define H264SW_NAL_SLICE_DEPTH 21U

/*
 * One NAL unit after its header is read and its emulation prevention
 * bytes are removed.
 *
 * type and ref_idc are nal_unit_type and nal_ref_idc (7.4.1).  rbsp and
 * rbsp_size name the RBSP: the bytes after the one-byte NAL unit header
 * (the decoder reads no NAL unit with the longer headers of types 14, 20
 * and 21) with every emulation_prevention_three_byte removed.  rbsp points
 * into the buffer the caller gave h264sw_nal_parse and lives as long as
 * that buffer holds these bytes.
 */
struct h264sw_nal {
	unsigned type;
	unsigned ref_idc;
	const uint8_t *rbsp;
	size_t rbsp_size;
};

/*
 * Reads a NAL unit's header and copies its RBSP into buffer (7.3.1, 7.4.1).
 *
 * nal and size are one NAL unit's bytes from vcodec_nal_find.  buffer has
 * capacity bytes, at least size; the RBSP is never longer than the NAL
 * unit.  Fills out: type, ref_idc and the RBSP in buffer.
 *
 * Emulation prevention: the bytes after the header are unescaped by
 * vcodec_nal_unescape (each 0x000003 has its 0x03 removed; a final
 * 0x000003 after a cabac_zero_word is valid, 7.4.1).
 *
 * Returns 0; EINVAL when size is 0, forbidden_zero_bit is 1, or the bytes
 * hold a three-byte sequence that cannot occur in a NAL unit (0x000000,
 * 0x000001, 0x000002, or 0x000003 followed by a byte above 3); ENOBUFS
 * when capacity is smaller than the RBSP (a caller's mistake).  For types
 * 14, 20 and 21 (header extensions, which this decoder ignores) only the
 * header is read: out->rbsp_size is 0.
 */
int h264sw_nal_parse(const uint8_t *nal, size_t size, uint8_t *buffer, size_t capacity, struct h264sw_nal *out);

#endif
