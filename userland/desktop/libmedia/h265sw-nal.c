/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of H.265: the NAL unit header, the RBSP copy and the
 * NAL unit type classes (7.3.1, 7.4.2).
 */

#include "h265sw-nal.h"
#include "vcodec-nal.h"

#include <errno.h>

/* The size of the NAL unit header in bytes (7.3.1.2: 1 + 6 + 6 + 3 bits). */
#define NAL_HEADER_BYTES 2U

/* The lowest and highest nal_unit_type of an IRAP picture (Table 7-1: BLA_W_LP to RSV_IRAP_VCL23). */
#define NAL_IRAP_FIRST 16U
#define NAL_IRAP_LAST 23U

/* The highest nal_unit_type of the VCL class (Table 7-1). */
#define NAL_VCL_LAST 31U

/* The highest nal_unit_type of a sub-layer non-reference picture, an even one (Table 7-1: RSV_VCL_N14). */
#define NAL_SLNR_LAST 14U

/* The reserved non-VCL types that start an access unit (7.4.2.4.4: RSV_NVCL41..44, UNSPEC48..55). */
#define NAL_RESERVED_START_FIRST 41U
#define NAL_RESERVED_START_LAST 44U
#define NAL_UNSPECIFIED_START_FIRST 48U
#define NAL_UNSPECIFIED_START_LAST 55U

/* Static function declarations. */
static int nal_header_read(const uint8_t *nal, size_t size, struct h265sw_nal *header);

/*
 * Reads a NAL unit's header and copies its whole RBSP into buffer (7.3.1,
 * 7.4.2).
 *
 * The header is checked first, so a broken header is EINVAL whatever the
 * buffer is; the capacity is compared with the largest the RBSP can be
 * before any byte is unescaped.
 */
int
h265sw_nal_parse(
	const uint8_t *nal,
	size_t size,
	uint8_t *buffer,
	size_t capacity,
	struct h265sw_nal *out)
{
	struct h265sw_nal header;
	size_t rbsp_size;
	int error;

	/* Without a place to report to, nothing can be delivered. */
	if (out == NULL)
		return EINVAL;

	/* Reads and checks the two header bytes. */
	error = nal_header_read(nal, size, &header);
	if (error != 0)
		return error;

	/* Refuses a buffer the RBSP might not fit in, whatever the emulation prevention bytes are. */
	if (capacity < size - NAL_HEADER_BYTES)
		return ENOBUFS;

	/* Copies the payload as the RBSP, removing the emulation prevention bytes. */
	rbsp_size = 0U;
	error = vcodec_nal_unescape(nal + NAL_HEADER_BYTES, size - NAL_HEADER_BYTES, buffer, capacity, &rbsp_size);
	if (error != 0)
		return error;

	/* Reports the header fields and the RBSP the caller's buffer now holds. */
	header.rbsp = buffer;
	header.rbsp_size = rbsp_size;
	header.complete = 1;
	*out = header;

	/* Succeeded: the whole RBSP is in buffer. */
	return 0;
}

/*
 * Reads a NAL unit's header and copies the RBSP of at most the first limit
 * payload bytes.
 *
 * A payload no longer than limit is the whole NAL unit.  A longer one is
 * cut after limit payload bytes; when the cut ends in 0x000003 the 0x03 is
 * dropped by the unescaping like a final one, so the copy is always the
 * start of the RBSP the whole payload gives.
 */
int
h265sw_nal_parse_prefix(
	const uint8_t *nal,
	size_t size,
	size_t limit,
	uint8_t *buffer,
	size_t capacity,
	struct h265sw_nal *out)
{
	struct h265sw_nal header;
	size_t payload_size;
	size_t cut_size;
	size_t rbsp_size;
	int complete;
	int error;

	/* Without a place to report to, nothing can be delivered. */
	if (out == NULL)
		return EINVAL;

	/* A limit of 0 would copy nothing and still claim to have read a header. */
	if (limit == 0U)
		return EINVAL;

	/* Reads and checks the two header bytes. */
	error = nal_header_read(nal, size, &header);
	if (error != 0)
		return error;

	/* Chooses how much of the payload is copied: all of it when it fits the limit. */
	payload_size = size - NAL_HEADER_BYTES;
	cut_size = payload_size;
	complete = 1;
	if (payload_size > limit) {
		cut_size = limit;
		complete = 0;
	}

	/* Refuses a buffer the copy might not fit in, whatever the emulation prevention bytes are. */
	if (capacity < cut_size)
		return ENOBUFS;

	/* Copies the chosen payload bytes as the RBSP, removing the emulation prevention bytes. */
	rbsp_size = 0U;
	error = vcodec_nal_unescape(nal + NAL_HEADER_BYTES, cut_size, buffer, capacity, &rbsp_size);
	if (error != 0)
		return error;

	/* Reports the header fields, the copy and whether it is the whole RBSP. */
	header.rbsp = buffer;
	header.rbsp_size = rbsp_size;
	header.complete = complete;
	*out = header;

	/* Succeeded: the start of the RBSP is in buffer. */
	return 0;
}

/*
 * Reports a VCL NAL unit type (0..31).
 *
 * Table 7-1 gives every type up to 31 the VCL class, reserved ones included.
 */
int
h265sw_nal_is_vcl(
	unsigned type)
{
	/* Types above 31 are the non-VCL class. */
	if (type > NAL_VCL_LAST)
		return 0;

	/* Succeeded: a VCL type. */
	return 1;
}

/*
 * Reports an IRAP picture's type (16..23): BLA, IDR, CRA and the reserved
 * IRAP types.
 */
int
h265sw_nal_is_irap(
	unsigned type)
{
	/* Below BLA_W_LP the types are not IRAP. */
	if (type < NAL_IRAP_FIRST)
		return 0;

	/* Above RSV_IRAP_VCL23 the types are not IRAP. */
	if (type > NAL_IRAP_LAST)
		return 0;

	/* Succeeded: an IRAP type. */
	return 1;
}

/*
 * Reports an IDR type (IDR_W_RADL, IDR_N_LP).
 */
int
h265sw_nal_is_idr(
	unsigned type)
{
	/* The two IDR types. */
	if (type == H265SW_NAL_IDR_W_RADL)
		return 1;
	if (type == H265SW_NAL_IDR_N_LP)
		return 1;

	/* Any other type is not IDR. */
	return 0;
}

/*
 * Reports a BLA type (16..18).
 */
int
h265sw_nal_is_bla(
	unsigned type)
{
	/* The three BLA types are consecutive. */
	if (type < H265SW_NAL_BLA_W_LP)
		return 0;
	if (type > H265SW_NAL_BLA_N_LP)
		return 0;

	/* Succeeded: a BLA type. */
	return 1;
}

/*
 * Reports a RASL type (RASL_N, RASL_R).
 */
int
h265sw_nal_is_rasl(
	unsigned type)
{
	/* The two RASL types. */
	if (type == H265SW_NAL_RASL_N)
		return 1;
	if (type == H265SW_NAL_RASL_R)
		return 1;

	/* Any other type is not RASL. */
	return 0;
}

/*
 * Reports a RADL type (RADL_N, RADL_R).
 */
int
h265sw_nal_is_radl(
	unsigned type)
{
	/* The two RADL types. */
	if (type == H265SW_NAL_RADL_N)
		return 1;
	if (type == H265SW_NAL_RADL_R)
		return 1;

	/* Any other type is not RADL. */
	return 0;
}

/*
 * Reports a sub-layer non-reference picture's type (Table 7-1).
 *
 * The SLNR types are the even types from 0 to 14 (7.4.2.2).
 */
int
h265sw_nal_is_slnr(
	unsigned type)
{
	/* Only types up to RSV_VCL_N14 can be SLNR. */
	if (type > NAL_SLNR_LAST)
		return 0;

	/* An odd type is a sub-layer reference picture. */
	if ((type % 2U) != 0U)
		return 0;

	/* Succeeded: TRAIL_N, TSA_N, STSA_N, RADL_N, RASL_N or a reserved _N type. */
	return 1;
}

/*
 * Reports a VCL type that is decoded: 0..9 and 16..21.
 */
int
h265sw_nal_is_decodable(
	unsigned type)
{
	/* The non-IRAP types with a defined slice segment: TRAIL to RASL. */
	if (type <= H265SW_NAL_RASL_R)
		return 1;

	/* The IRAP types with a defined slice segment: BLA, IDR and CRA. */
	if (type >= H265SW_NAL_BLA_W_LP && type <= H265SW_NAL_CRA)
		return 1;

	/* The reserved types are ignored (7.4.2.2). */
	return 0;
}

/*
 * Reports whether a non-VCL NAL unit starts a new access unit when it
 * follows the last VCL NAL unit of a picture (7.4.2.4.4).
 */
int
h265sw_nal_starts_access_unit(
	unsigned type)
{
	/* The access unit delimiter, the parameter sets and the prefix SEI. */
	if (type == H265SW_NAL_AUD)
		return 1;
	if (type == H265SW_NAL_VPS)
		return 1;
	if (type == H265SW_NAL_SPS)
		return 1;
	if (type == H265SW_NAL_PPS)
		return 1;
	if (type == H265SW_NAL_PREFIX_SEI)
		return 1;

	/* The reserved non-VCL types 41..44. */
	if (type >= NAL_RESERVED_START_FIRST && type <= NAL_RESERVED_START_LAST)
		return 1;

	/* The unspecified types 48..55. */
	if (type >= NAL_UNSPECIFIED_START_FIRST && type <= NAL_UNSPECIFIED_START_LAST)
		return 1;

	/* Every other type (VCL, end of sequence and bitstream, filler data, suffix SEI, 45..47, 56..63) does not. */
	return 0;
}

/*
 * Reads and checks the two-byte NAL unit header into header (7.3.1.2,
 * 7.4.2.2).
 *
 * Only the type, layer_id and temporal_id of header are written.
 */
static int
nal_header_read(
	const uint8_t *nal,
	size_t size,
	struct h265sw_nal *header)
{
	unsigned first;
	unsigned second;
	unsigned type;
	unsigned temporal_id_plus1;
	int irap;

	/* A NAL unit is at least its header; a NULL pointer with a size is a caller's mistake. */
	if (size < NAL_HEADER_BYTES)
		return EINVAL;
	if (nal == NULL)
		return EINVAL;

	/* Refuses a NAL unit larger than the RBSP buffers this code is sized for. */
	if (size > H265SW_MAX_NAL_BYTES)
		return EINVAL;

	/* Reads the two header bytes. */
	first = nal[0];
	second = nal[1];

	/* forbidden_zero_bit shall be 0. */
	if ((first & 0x80U) != 0U)
		return EINVAL;

	/* Splits the fields: type u(6), layer id u(6), temporal id plus 1 u(3). */
	type = (first >> 1) & 0x3FU;
	temporal_id_plus1 = second & 0x07U;

	/* nuh_temporal_id_plus1 shall not be 0. */
	if (temporal_id_plus1 == 0U)
		return EINVAL;

	/* TemporalId shall be 0 for an IRAP picture. */
	irap = h265sw_nal_is_irap(type);
	if (irap != 0 && temporal_id_plus1 != 1U)
		return EINVAL;

	/* TemporalId shall not be 0 for a TSA picture. */
	if ((type == H265SW_NAL_TSA_N || type == H265SW_NAL_TSA_R) && temporal_id_plus1 == 1U)
		return EINVAL;

	/* Reports the fields. */
	header->type = type;
	header->layer_id = ((first & 0x01U) << 5) | (second >> 3);
	header->temporal_id = temporal_id_plus1 - 1U;

	/* Succeeded: a valid header. */
	return 0;
}
