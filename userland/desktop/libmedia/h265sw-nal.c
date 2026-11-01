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

#include <errno.h>

/*
 * Reads a NAL unit's header and copies its whole RBSP into buffer (7.3.1,
 * 7.4.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_parse(
	const uint8_t *nal,
	size_t size,
	uint8_t *buffer,
	size_t capacity,
	struct h265sw_nal *out)
{
	UNUSED_PARAMETER(nal);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(capacity);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a NAL unit's header and copies the RBSP of at most the first limit
 * payload bytes.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
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
	UNUSED_PARAMETER(nal);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(limit);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(capacity);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports a VCL NAL unit type (0..31).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_vcl(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports an IRAP picture's type (16..23): BLA, IDR, CRA and the reserved
 * IRAP types.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_irap(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports an IDR type (IDR_W_RADL, IDR_N_LP).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_idr(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports a BLA type (16..18).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_bla(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports a RASL type (RASL_N, RASL_R).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_rasl(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports a RADL type (RADL_N, RADL_R).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_radl(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports a sub-layer non-reference picture's type (Table 7-1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_slnr(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports a VCL type that is decoded: 0..9 and 16..21.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_is_decodable(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports whether a non-VCL NAL unit starts a new access unit when it
 * follows the last VCL NAL unit of a picture (7.4.2.4.4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-nal.h.
 */
int
h265sw_nal_starts_access_unit(
	unsigned type)
{
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
