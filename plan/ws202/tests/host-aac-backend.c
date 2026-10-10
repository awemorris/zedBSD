/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Invoke the production native operations over ADTS and ASC raw packets, including drain. */
#include "userland/desktop/libmedia/media-private.h"
#include "userland/desktop/libmedia/aac-input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int
main(
	int argc,
	char **argv)
{
	struct media_track track;
	struct media_packet packet;
	struct media_aac_adts transport;
	unsigned char asc[2];
	unsigned char data[65536];
	int16_t samples[16384];
	FILE *input;
	FILE *output;
	void *state;
	size_t bytes;
	size_t offset;
	size_t count;
	size_t written;
	uint32_t rate;
	unsigned frames;
	unsigned blocks;
	int64_t time_us;
	int status;
	int raw;

	/* The runner explicitly chooses framing and the requested output rate. */
	if (argc != 5)
		return 2;
	rate = (uint32_t)strtoul(argv[3], NULL, 10);
	raw = atoi(argv[4]);
	input = fopen(argv[1], "rb");
	if (input == NULL)
		return 2;
	bytes = fread(data, 1U, sizeof(data), input);
	fclose(input);
	status = media_aac_adts_parse(data, bytes, &transport);
	if (status != 0)
		return 1;
	memset(&track, 0, sizeof(track));
	track.codec = MEDIA_CODEC_AAC;
	if (raw != 0) {
		asc[0] = (unsigned char)(0x10U | (transport.config.rate_index >> 1U));
		asc[1] = (unsigned char)((transport.config.rate_index << 7U) | (transport.config.channel_configuration << 3U));
		track.private_data = asc;
		track.private_size = sizeof(asc);
	}

	/* Admission uses production operations without installing a decoder or FFmpeg library. */
	status = media_aac_ops.open(&track, &state);
	if (status != 0)
		return 1;
	output = fopen(argv[2], "wb");
	if (output == NULL)
		return 2;
	memset(&packet, 0, sizeof(packet));
	offset = 0U;
	frames = 0U;
	blocks = 0U;
	while (offset < bytes) {
		status = media_aac_adts_parse(data + offset, bytes - offset, &transport);
		if (status != 0)
			return 1;
		packet.data = data + offset;
		packet.size = transport.frame_size;
		if (raw != 0) {
			packet.data += transport.header_size;
			packet.size -= transport.header_size;
		}

		/* Frame timestamps are independent of the decoder's own sample clock. */
		packet.pts_us = (int64_t)frames * 1024000000LL / transport.config.rate;
		status = media_aac_ops.send(state, &packet);
		if (status != 0) {
			fprintf(stderr, "send frame=%u error=%d\n", frames, status);
			return 1;
		}

		/* Drain all currently exposed audio before submitting another packet. */
		for (;;) {
			status = media_aac_ops.receive(state, &time_us);
			if (status == 0)
				break;
			if (status < 0)
				return 1;
			count = media_aac_ops.sound(state, samples, 8192U, rate);
			written = fwrite(samples, 2U * sizeof(int16_t), count, output);
			if (written != count)
				return 1;
			blocks += (unsigned)count;
		}

		/* Transport boundaries determine the next independently admitted packet. */
		offset += transport.frame_size;
		frames++;
	}

	/* End-of-input releases the converter's final real samples. */
	status = media_aac_ops.send(state, NULL);
	if (status != 0)
		return 1;
	for (;;) {
		status = media_aac_ops.receive(state, &time_us);
		if (status == 0)
			break;
		if (status < 0)
			return 1;
		count = media_aac_ops.sound(state, samples, 8192U, rate);
		written = fwrite(samples, 2U * sizeof(int16_t), count, output);
		if (written != count)
			return 1;
		blocks += (unsigned)count;
	}

	/* Close must release all syntax, overlap and filter resources. */
	media_aac_ops.close(state);
	fclose(output);
	printf("AAC backend PASS raw=%d frames=%u output=%u rate=%u\n", raw, frames, blocks, rate);
	return 0;
}
