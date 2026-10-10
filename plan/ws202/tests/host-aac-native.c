/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Production AAC frame/reconstruction checks over independent encoder fixtures. */
#include "userland/desktop/libmedia/aac-frame.h"
#include "userland/desktop/libmedia/aac-synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/*
 * Parse all AAC payloads and verify their exact transport boundaries.
 */
int
main(
	int argc,
	char **argv)
{
	struct media_aac_frame *frame;
	struct media_aac_transform *transform;
	struct media_aac_history *history;
	float *pcm;
	float interleaved[8192];
	uint32_t random;
	struct media_aac_adts transport;
	struct media_bits bits;
	FILE *input;
	FILE *output;
	unsigned char data[65536];
	size_t size;
	size_t written;
	size_t offset;
	unsigned frames;
	unsigned short_windows;
	unsigned noise;
	unsigned tns;
	unsigned ms;
	unsigned channel;
	unsigned group;
	unsigned band;
	unsigned sample;
	unsigned intensity;
	unsigned window;
	unsigned channel_tns;
	int error;
	const char *trace;

	/* The runner supplies one small independently encoded ADTS stream. */
	if (argc < 2)
		return 2;
	input = fopen(argv[1], "rb");
	if (input == NULL)
		return 2;

	/* Keep parsed frame storage off the decoder thread's stack. */
	frame = calloc(1U, sizeof(*frame));
	if (frame == NULL) {
		fclose(input);
		return 2;
	}

	/* PCM comparison retains overlap independently for every transmitted channel. */
	transform = calloc(1U, sizeof(*transform));
	history = calloc(8U, sizeof(*history));
	pcm = calloc(8192U, sizeof(*pcm));
	if (transform == NULL || history == NULL || pcm == NULL)
		return 2;
	output = NULL;
	if (argc > 2) {
		output = fopen(argv[2], "wb");
		if (output == NULL)
			return 2;
	}

	/* The original PNS generator is decoder-local and starts reproducibly. */
	random = 1U;
	trace = getenv("WS202_AAC_TRACE");

	/* Each fixture fits in the fixed input span; no historical build output is consumed. */
	size = fread(data, 1U, sizeof(data), input);
	fclose(input);
	offset = 0U;
	frames = 0U;
	short_windows = 0U;
	noise = 0U;
	tns = 0U;
	ms = 0U;

	/* Parse each actual payload, including all tool syntax and terminal padding. */
	while (offset < size) {
		error = media_aac_adts_parse(data + offset, size - offset, &transport);
		if (error != 0) {
			fprintf(stderr, "ADTS at %lu: error %d\n", (unsigned long)offset, error);
			free(frame);
			return 1;
		}

		/* Normal encoder output contains one unprotected raw data block per frame. */
		media_bits_init(&bits, data + offset + transport.header_size, transport.frame_size - transport.header_size);
		error = media_aac_frame_parse(&bits, &transport.config, frame);
		if (error != 0) {
			fprintf(stderr, "AAC frame %u byte %lu bit %lu: error %d\n", frames, (unsigned long)offset, (unsigned long)bits.position, error);
			free(frame);
			return 1;
		}

		/* No unconsumed payload may be hidden by the framing test. */
		if (bits.position != bits.count) {
			fprintf(stderr, "AAC frame %u: consumed %lu of %lu bits\n", frames, (unsigned long)bits.position, (unsigned long)bits.count);
			free(frame);
			return 1;
		}

		/* Observe actual short-window, noise, stereo and TNS use for later reconstruction comparisons. */
		for (channel = 0U; channel < frame->channels; channel++) {
			intensity = 0U;
			channel_tns = 0U;
			for (window = 0U; window < frame->channel[channel].info.windows; window++)
				channel_tns += frame->channel[channel].tns_count[window];
			/* Count short windows independently of the parser's band traversal. */
			if (frame->channel[channel].info.sequence == 2U)
				short_windows++;
			tns += channel_tns;

			/* Count signalled pseudo books and stereo masks in every window group. */
			for (group = 0U; group < frame->channel[channel].info.groups; group++) {
				/* Each band retains its signalled book after Huffman syntax has ended. */
				for (band = 0U; band < frame->channel[channel].info.max_sfb; band++) {
					/* Noise bands cannot be mistaken for ordinary spectral codewords. */
					if (frame->channel[channel].books[group][band] == 13U)
						noise++;
					ms += frame->channel[channel].ms[group][band];
					if (frame->channel[channel].books[group][band] >= 14U)
						intensity++;
				}
			}

			/* Optional fixture diagnostics reveal the actual tools used by each channel. */
			if (trace != NULL)
				fprintf(stderr, "frame=%u channel=%u sequence=%u shape=%u gain=%u ms=%u intensity=%u tns=%u\n", frames, channel, frame->channel[channel].info.sequence, frame->channel[channel].info.shape, frame->channel[channel].gain, frame->channel[channel].ms_mode, intensity, channel_tns);
		}

		/* Compare production reconstruction rather than a second test-only inverse transform. */
		error = media_aac_reconstruct(frame, &random);
		if (error != 0)
			return 1;
		for (channel = 0U; channel < frame->channels; channel++) {
			error = media_aac_synthesize(&frame->channel[channel], &history[channel], transform, pcm + channel * 1024U);
			if (error != 0)
				return 1;
		}

		/* Reference ADTS decoding includes all priming and trailing samples in the same order. */
		for (sample = 0U; sample < 1024U; sample++) {
			for (channel = 0U; channel < frame->channels; channel++)
				interleaved[sample * frame->channels + channel] = pcm[channel * 1024U + sample];
		}

		/* Write interleaved PCM only when the comparison runner requests an output artifact. */
		if (output != NULL) {
			written = fwrite(interleaved, sizeof(float), 1024U * frame->channels, output);
			if (written != 1024U * frame->channels)
				return 1;
		}

		/* Continue at the next complete transport frame. */
		offset += transport.frame_size;
		frames++;
	}

	/* Release scratch syntax after all complete payloads have been checked. */
	free(frame);
	free(transform);
	free(history);
	free(pcm);
	if (output != NULL)
		fclose(output);

	/* Empty or truncated fixtures cannot pass as successfully decoded input. */
	if (frames == 0U)
		return 1;

	/* Succeeded: all actual frames and payload endpoints were parsed. */
	printf("AAC raw syntax PASS frames=%u short=%u noise=%u tns=%u ms=%u\n", frames, short_windows, noise, tns, ms);
	return 0;
}
