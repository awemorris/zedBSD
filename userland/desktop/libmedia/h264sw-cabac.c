/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC arithmetic decoding engine of the software H.264 decoder
 * (9.3.1, 9.3.3.2).
 */

#include "h264sw-cabac.h"

#include <errno.h>
#include <stddef.h>

/*
 * Starts CABAC for a slice: the context variables and then the engine (9.3.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac.h.
 */
int
h264sw_cabac_start(
	struct h264sw_cabac *cabac,
	struct h264sw_bits *bits,
	unsigned slice_type,
	unsigned cabac_init_idc,
	int qp)
{
	UNUSED_PARAMETER(cabac);
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(slice_type);
	UNUSED_PARAMETER(cabac_init_idc);
	UNUSED_PARAMETER(qp);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Decodes one bin with context ctx_idx (DecodeDecision, 9.3.3.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac.h.
 */
unsigned
h264sw_cabac_decision(
	struct h264sw_cabac *cabac,
	unsigned ctx_idx)
{
	UNUSED_PARAMETER(cabac);
	UNUSED_PARAMETER(ctx_idx);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Decodes one bin with equal probabilities (DecodeBypass, 9.3.3.2.3); returns 0 or 1.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac.h.
 */
unsigned
h264sw_cabac_bypass(
	struct h264sw_cabac *cabac)
{
	UNUSED_PARAMETER(cabac);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Decodes the bin of end_of_slice_flag or of mb_type I_PCM (DecodeTerminate, 9.3.3.2.4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac.h.
 */
unsigned
h264sw_cabac_terminate(
	struct h264sw_cabac *cabac)
{
	UNUSED_PARAMETER(cabac);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Reads the samples of an I_PCM macroblock and restarts the engine (7.3.5, 9.3.1.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cabac.h.
 */
int
h264sw_cabac_pcm(
	struct h264sw_cabac *cabac,
	uint8_t *samples,
	size_t count)
{
	UNUSED_PARAMETER(cabac);
	UNUSED_PARAMETER(samples);
	UNUSED_PARAMETER(count);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
