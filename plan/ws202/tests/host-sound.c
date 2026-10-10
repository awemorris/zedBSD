/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Independent tones exercise the production continuous resampler across source-block boundaries. */
#include "userland/desktop/libmedia/sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int
main(
	int argc,
	char **argv)
{
	struct media_pcm *sound;
	float source[2048];
	int16_t output[16384];
	FILE *file;
	uint32_t rate;
	double frequency;
	unsigned block;
	unsigned sample;
	size_t count;
	size_t written;
	uint64_t total;
	int64_t time_us;
	int error;
	int ready;

	/* The runner selects a source clock, analytical tone and output artifact. */
	if (argc != 4)
		return 2;
	rate = (uint32_t)strtoul(argv[1], NULL, 10);
	frequency = strtod(argv[2], NULL);
	file = fopen(argv[3], "wb");
	if (file == NULL)
		return 2;
	sound = calloc(1U, sizeof(*sound));
	if (sound == NULL)
		return 2;
	error = media_pcm_init(sound, rate, 0);
	if (error != 0)
		return 1;
	total = 0U;
	for (block = 0U; block <= 24U; block++) {
		if (block == 24U) {
			sound->drained = 1;
		} else {
			for (sample = 0U; sample < 1024U; sample++) {
				source[sample * 2U] = (float)(0.5 * sin(6.2831853071795864769 * frequency * (block * 1024U + sample) / rate));
				source[sample * 2U + 1U] = source[sample * 2U];
			}

			/* Each block retains a known analytical time across the production filter's lookahead. */
			error = media_pcm_push(sound, source, 1024U, (int64_t)block * 1024000000LL / rate);
			if (error != 0)
				return 1;
		}

		/* Receive and convert exactly as an application drains its audio decoder. */
		for (;;) {
			ready = media_pcm_receive(sound, &time_us);
			if (ready == 0)
				break;
			count = media_pcm_read(sound, output, 8192U, 48000U);
			written = fwrite(output, sizeof(int16_t) * 2U, count, file);
			if (written != count)
				return 1;
			total += count;
		}
	}

	/* The output clock must account for every real input sample once. */
	media_pcm_close(sound);
	free(sound);
	fclose(file);
	if (total != (24U * 1024U * 48000ULL + rate - 1U) / rate)
		return 1;
	printf("PCM clock PASS rate=%u output=%llu\n", rate, (unsigned long long)total);
	return 0;
}
