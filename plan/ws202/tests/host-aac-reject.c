/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Whole-packet rejection must precede output even after a complete valid LC raw block. */
#include "userland/desktop/libmedia/media-private.h"
#include "userland/desktop/libmedia/aac-input.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(
	int argc,
	char **argv)
{
	struct media_track track;
	struct media_packet packet;
	struct media_aac_adts transport;
	unsigned char data[16384];
	unsigned char he_asc[4];
	FILE *file;
	void *state;
	size_t bytes;
	size_t offset;
	int status;
	int64_t time_us;

	/* A real first LC frame makes the whole-packet rejection exercise nontrivial. */
	if (argc != 2)
		return 2;
	file = fopen(argv[1], "rb");
	if (file == NULL)
		return 2;
	bytes = fread(data, 1U, sizeof(data), file);
	fclose(file);
	status = media_aac_adts_parse(data, bytes, &transport);
	if (status != 0)
		return 1;
	offset = transport.frame_size;
	if (offset + 10U > sizeof(data))
		return 2;

	/* A second ADTS frame contains FIL with an SBR extension, followed by ID_END. */
	memcpy(data + offset, data, 7U);
	data[offset + 3U] &= 0xfcU;
	data[offset + 4U] = 1U;
	data[offset + 5U] = 0x5fU;
	data[offset + 6U] = 0xfcU;
	data[offset + 7U] = 0xc3U;
	data[offset + 8U] = 0xa1U;
	data[offset + 9U] = 0xc0U;
	memset(&track, 0, sizeof(track));
	track.codec = MEDIA_CODEC_AAC;
	status = media_aac_ops.open(&track, &state);
	if (status != 0)
		return 1;
	memset(&packet, 0, sizeof(packet));
	packet.data = data;
	packet.size = offset + 10U;
	status = media_aac_ops.send(state, &packet);
	if (status != ENOTSUP)
		return 1;
	status = media_aac_ops.receive(state, &time_us);
	if (status != -ENOTSUP)
		return 1;
	media_aac_ops.close(state);

	/* Explicit HE-AAC ASC is rejected at open rather than silently using its LC core. */
	he_asc[0] = 0x2bU;
	he_asc[1] = 0x92U;
	he_asc[2] = 0x08U;
	he_asc[3] = 0x00U;
	track.private_data = he_asc;
	track.private_size = sizeof(he_asc);
	status = media_aac_ops.open(&track, &state);
	if (status != MEDIA_PROBLEM_PROFILE)
		return 1;
	if (state != NULL)
		return 1;
	puts("AAC profile rejection PASS (explicit HE-AAC, atomic implicit FIL SBR)");
	return 0;
}
