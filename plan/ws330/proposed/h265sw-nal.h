/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of the software H.265 decoder: the two-byte NAL unit
 * header (7.3.1.2, 7.4.2.2), the NAL unit types and their classes
 * (Table 7-1), the RBSP of a NAL unit (vcodec_nal_unescape after the
 * header), and which NAL units start an access unit (7.4.2.4.4).
 *
 * Finding NAL units in a byte stream is vcodec_nal_find (vcodec-nal.h).
 */

#ifndef LIBMEDIA_H265SW_NAL_H
#define LIBMEDIA_H265SW_NAL_H

#include "h265sw.h"

/* The nal_unit_type values (Table 7-1). */
#define H265SW_NAL_TRAIL_N 0U
#define H265SW_NAL_TRAIL_R 1U
#define H265SW_NAL_TSA_N 2U
#define H265SW_NAL_TSA_R 3U
#define H265SW_NAL_STSA_N 4U
#define H265SW_NAL_STSA_R 5U
#define H265SW_NAL_RADL_N 6U
#define H265SW_NAL_RADL_R 7U
#define H265SW_NAL_RASL_N 8U
#define H265SW_NAL_RASL_R 9U
#define H265SW_NAL_RSV_VCL_N14 14U
#define H265SW_NAL_BLA_W_LP 16U
#define H265SW_NAL_BLA_W_RADL 17U
#define H265SW_NAL_BLA_N_LP 18U
#define H265SW_NAL_IDR_W_RADL 19U
#define H265SW_NAL_IDR_N_LP 20U
#define H265SW_NAL_CRA 21U
#define H265SW_NAL_RSV_IRAP_23 23U
#define H265SW_NAL_RSV_VCL_31 31U
#define H265SW_NAL_VPS 32U
#define H265SW_NAL_SPS 33U
#define H265SW_NAL_PPS 34U
#define H265SW_NAL_AUD 35U
#define H265SW_NAL_EOS 36U
#define H265SW_NAL_EOB 37U
#define H265SW_NAL_FD 38U
#define H265SW_NAL_PREFIX_SEI 39U
#define H265SW_NAL_SUFFIX_SEI 40U

/*
 * One NAL unit after its header is read and its emulation prevention
 * bytes are removed.
 *
 * type is nal_unit_type, layer_id nuh_layer_id, temporal_id TemporalId
 * (nuh_temporal_id_plus1 - 1, 7-1).  rbsp and rbsp_size name the RBSP in
 * the buffer the caller gave h265sw_nal_parse (the bytes after the
 * two-byte header with every emulation_prevention_three_byte removed).
 */
struct h265sw_nal {
	unsigned type;
	unsigned layer_id;
	unsigned temporal_id;
	const uint8_t *rbsp;
	size_t rbsp_size;
};

/*
 * Reads a NAL unit's header and copies its RBSP into buffer (7.3.1,
 * 7.4.2).
 *
 * nal and size are one NAL unit's bytes from vcodec_nal_find.  buffer has
 * capacity bytes, at least size - 2.  Fills out.
 *
 * Returns 0; EINVAL when size is below 2, forbidden_zero_bit is 1,
 * nuh_temporal_id_plus1 is 0, TemporalId is not 0 for an IRAP type
 * (16..23) or is 0 for a TSA type (7.4.2.2), or vcodec_nal_unescape
 * refuses the payload; ENOBUFS when capacity is too small.  A NAL unit
 * with layer_id above 0 is read like any other (the caller skips it).
 */
int h265sw_nal_parse(const uint8_t *nal, size_t size, uint8_t *buffer, size_t capacity, struct h265sw_nal *out);

/* Reports a VCL NAL unit type (0..31). */
int h265sw_nal_is_vcl(unsigned type);

/* Reports an IRAP picture's type (16..23): BLA, IDR, CRA and the reserved IRAP types. */
int h265sw_nal_is_irap(unsigned type);

/* Reports an IDR type (IDR_W_RADL, IDR_N_LP). */
int h265sw_nal_is_idr(unsigned type);

/* Reports a BLA type (16..18). */
int h265sw_nal_is_bla(unsigned type);

/* Reports a RASL type (RASL_N, RASL_R). */
int h265sw_nal_is_rasl(unsigned type);

/* Reports a RADL type (RADL_N, RADL_R). */
int h265sw_nal_is_radl(unsigned type);

/*
 * Reports a sub-layer non-reference picture's type (Table 7-1: TRAIL_N,
 * TSA_N, STSA_N, RADL_N, RASL_N, RSV_VCL_N10, _N12, _N14: an even type
 * at most 14).
 */
int h265sw_nal_is_slnr(unsigned type);

/*
 * Reports a VCL type this decoder decodes: 0..9 and 16..21.  The
 * reserved VCL types (10..15, 22..31) are skipped (7.4.2.2: decoders
 * ignore their contents).
 */
int h265sw_nal_is_decodable(unsigned type);

/*
 * Reports whether a non-VCL NAL unit of nuh_layer_id 0 starts a new
 * access unit when it follows the last VCL NAL unit of a picture
 * (7.4.2.4.4): AUD, VPS, SPS, PPS, prefix SEI, 41..44 and 48..55.  The
 * first VCL NAL unit of a picture (first_slice_segment_in_pic_flag 1)
 * also starts one; the caller checks that from the slice header.  End of
 * sequence and end of bitstream end the access unit they are in.
 */
int h265sw_nal_starts_access_unit(unsigned type);

#endif
