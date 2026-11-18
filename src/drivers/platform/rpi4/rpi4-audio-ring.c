/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The headphone jack's arithmetic (rpi4-audio-ring.h, ws207-p002).
 *
 * The PWM (in its PWM algorithm mode, not mark-space) gives each channel a
 * duty of DAT out of RNG clocks each sample; a 16-bit sample maps
 * linearly onto 0 .. RNG.  With both channels taking the FIFO, the FIFO's
 * words go to channel 1 and channel 2 in turn, so a stereo frame is two
 * words, left then right.
 */

#include "drivers/platform/rpi4/rpi4-audio-ring.h"

/*
 * Maps a signed 16-bit sample to a duty word of a range: -32768 to 0,
 * 32767 to just under the range.
 */
uint32_t
rpi4_audio_duty(
	int16_t sample,
	uint32_t range)
{
	uint64_t offset;

	/* The sample as 0 .. 65535, scaled to the range. */
	offset = (uint64_t)((int32_t)sample + 32768);

	/* Succeeded: the duty. */
	return (uint32_t)((offset * range) >> 16);
}

/*
 * Converts stereo frames (left, right) into FIFO words (channel 1,
 * channel 2); right_first puts the right sample on channel 1.
 */
void
rpi4_audio_convert(
	const int16_t *pcm,
	uint32_t *words,
	size_t frames,
	uint32_t range,
	int right_first)
{
	size_t first;
	size_t index;

	/* The sample of each frame that goes to channel 1. */
	first = 0U;
	if (right_first != 0)
		first = 1U;

	/* Each frame, channel 1 then channel 2. */
	for (index = 0U; index < frames; index++) {
		words[index * 2U] = rpi4_audio_duty(pcm[index * 2U + first], range);
		words[index * 2U + 1U] = rpi4_audio_duty(pcm[index * 2U + (1U - first)], range);
	}
}

/*
 * Builds a ring of count control blocks, block i moving fragment i of the
 * data (fragment_bytes from data_bus) to the PWM's FIFO, paced by its
 * DREQ, raising an interrupt at its end, and going on to block i + 1 (the
 * last back to the first).  cb_bus is the bus address of cbs[0].
 */
void
rpi4_audio_chain(
	struct rpi4_dma_cb *cbs,
	unsigned count,
	uint32_t cb_bus,
	uint32_t data_bus,
	uint32_t fragment_bytes,
	uint32_t fifo_bus)
{
	unsigned index;
	unsigned next;

	/* Each block. */
	for (index = 0U; index < count; index++) {
		next = index + 1U;
		if (next == count)
			next = 0U;
		cbs[index].ti = RPI4_DMA_TI_INTEN | RPI4_DMA_TI_WAIT_RESP | RPI4_DMA_TI_DEST_DREQ | RPI4_DMA_TI_SRC_INC |
		    (RPI4_DMA_DREQ_PWM1 << RPI4_DMA_TI_PERMAP_SHIFT);
		cbs[index].source = data_bus + index * fragment_bytes;
		cbs[index].destination = fifo_bus;
		cbs[index].length = fragment_bytes;
		cbs[index].stride = 0U;
		cbs[index].next = cb_bus + next * (uint32_t)sizeof(struct rpi4_dma_cb);
		cbs[index].reserved[0] = 0U;
		cbs[index].reserved[1] = 0U;
	}
}

/*
 * Tells which fragment the DMA reads from its source address: the end of
 * the last fragment is the start of the first, and an address outside the
 * data gives count.
 */
unsigned
rpi4_audio_fragment(
	uint32_t source_bus,
	uint32_t data_bus,
	uint32_t fragment_bytes,
	unsigned count)
{
	uint32_t offset;

	/* Outside the ring. */
	if (source_bus < data_bus)
		return count;
	offset = source_bus - data_bus;
	if (offset > fragment_bytes * count)
		return count;

	/* The end of the ring wraps to its start. */
	if (offset == fragment_bytes * count)
		return 0U;

	/* Succeeded: the fragment. */
	return offset / fragment_bytes;
}

/*
 * Tells where the DMA reads in the 16-bit stereo ring the duty words come
 * from: a duty word is twice a sample's bytes.  An address outside the
 * data gives 0.
 */
uint32_t
rpi4_audio_position(
	uint32_t source_bus,
	uint32_t data_bus,
	uint32_t fragment_bytes,
	unsigned count)
{
	uint32_t offset;

	/* Outside the ring, or at its end, which is its start. */
	if (source_bus < data_bus)
		return 0U;
	offset = source_bus - data_bus;
	if (offset >= fragment_bytes * count)
		return 0U;

	/* Succeeded: the sample byte the DMA has reached. */
	return offset / 2U;
}
