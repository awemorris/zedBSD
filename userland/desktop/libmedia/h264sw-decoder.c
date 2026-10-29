/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The software H.264 decoder as a whole: NAL units in, frames out.
 */

#include "h264sw-decoder.h"

#include <errno.h>
#include <stddef.h>

/*
 * Makes a decoder in its start state.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-decoder.h.
 */
int
h264sw_decoder_create(
	struct h264sw_decoder **decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Frees a decoder: the picture in progress is dropped, the DPB and the output queue cleared, the pool destroyed.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-decoder.h.
 */
void
h264sw_decoder_destroy(
	struct h264sw_decoder *decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Does nothing yet. */
	return;
}

/*
 * Decodes one NAL unit.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-decoder.h.
 */
int
h264sw_decoder_nal(
	struct h264sw_decoder *decoder,
	const uint8_t *nal,
	size_t size,
	int64_t time_us)
{
	UNUSED_PARAMETER(decoder);
	UNUSED_PARAMETER(nal);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(time_us);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Finishes the picture in progress, for a caller that knows the access unit has ended (a container's packet holds one access unit), so the picture does not wait for the next NAL unit.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-decoder.h.
 */
int
h264sw_decoder_end_access_unit(
	struct h264sw_decoder *decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Ends the stream: finishes the picture in progress and outputs every frame still waiting in the DPB (h264sw_dpb_flush), in output order.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-decoder.h.
 */
int
h264sw_decoder_flush(
	struct h264sw_decoder *decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Takes the next output frame with its reference (the caller unrefs it), or NULL when none is ready.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-decoder.h.
 */
struct h264sw_frame *
h264sw_decoder_output(
	struct h264sw_decoder *decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Reports that nothing is made yet. */
	return NULL;
}

/*
 * Drops the picture in progress, the DPB, the output queue and the order count state, for a seek; the parameter sets and the frame pool are kept.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-decoder.h.
 */
void
h264sw_decoder_reset(
	struct h264sw_decoder *decoder)
{
	UNUSED_PARAMETER(decoder);

	/* Does nothing yet. */
	return;
}
