/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws122-p004: checks that the decoding add-in's view of the first fields
 * of AVPacket, AVFrame, AVCodecParameters and AVCodec
 * (userland/desktop/media-app/avcodec-layout.h)
 * matches FFmpeg's public headers of one version.  Compiled once for each
 * version the add-in knows, against that version's headers (the host's
 * Debian 13 FFmpeg 7, libavcodec 61; the image's FFmpeg 9.0.2 package,
 * libavcodec 63), with the major version it must find.  Only the test
 * includes FFmpeg's headers; the player does not.
 *
 *   host-layout MAJOR      prints "host-layout: PASS major=N" or FAIL
 */

#include "userland/desktop/media-app/avcodec-layout.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Counts the fields whose offset or size differs. */
static int failures;

static void same(const char *what, size_t ours, size_t theirs);

int
main(
	int argc,
	char **argv)
{
	unsigned wanted;

	/* The major version the headers must be. */
	if (argc != 2) {
		fprintf(stderr, "usage: host-layout MAJOR\n");
		return 2;
	}
	wanted = (unsigned)strtoul(argv[1], NULL, 10);
	same("libavcodec major", wanted, LIBAVCODEC_VERSION_MAJOR);
	same("AV_NUM_DATA_POINTERS", CODEC_DATA_POINTERS, AV_NUM_DATA_POINTERS);
	same("AV_PKT_FLAG_KEY", 1U, AV_PKT_FLAG_KEY);

	/* AVPacket's head. */
	same("AVPacket.buf", offsetof(struct codec_packet, buffer), offsetof(AVPacket, buf));
	same("AVPacket.pts", offsetof(struct codec_packet, pts), offsetof(AVPacket, pts));
	same("AVPacket.dts", offsetof(struct codec_packet, dts), offsetof(AVPacket, dts));
	same("AVPacket.data", offsetof(struct codec_packet, data), offsetof(AVPacket, data));
	same("AVPacket.size", offsetof(struct codec_packet, size), offsetof(AVPacket, size));
	same("AVPacket.stream_index", offsetof(struct codec_packet, stream_index), offsetof(AVPacket, stream_index));
	same("AVPacket.flags", offsetof(struct codec_packet, flags), offsetof(AVPacket, flags));

	/* AVFrame's head. */
	same("AVFrame.data", offsetof(struct codec_frame, data), offsetof(AVFrame, data));
	same("AVFrame.linesize", offsetof(struct codec_frame, linesize), offsetof(AVFrame, linesize));
	same("AVFrame.extended_data", offsetof(struct codec_frame, extended_data), offsetof(AVFrame, extended_data));
	same("AVFrame.width", offsetof(struct codec_frame, width), offsetof(AVFrame, width));
	same("AVFrame.height", offsetof(struct codec_frame, height), offsetof(AVFrame, height));
	same("AVFrame.nb_samples", offsetof(struct codec_frame, nb_samples), offsetof(AVFrame, nb_samples));
	same("AVFrame.format", offsetof(struct codec_frame, format), offsetof(AVFrame, format));

	/* AVCodecParameters' and AVCodec's first fields (ws177-p031: the extradata of Vorbis and Theora). */
	same("AVCodecParameters.codec_type", offsetof(struct codec_parameters, codec_type), offsetof(AVCodecParameters, codec_type));
	same("AVCodecParameters.codec_id", offsetof(struct codec_parameters, codec_id), offsetof(AVCodecParameters, codec_id));
	same("AVCodecParameters.codec_tag", offsetof(struct codec_parameters, codec_tag), offsetof(AVCodecParameters, codec_tag));
	same("AVCodecParameters.extradata", offsetof(struct codec_parameters, extradata), offsetof(AVCodecParameters, extradata));
	same("AVCodecParameters.extradata_size", offsetof(struct codec_parameters, extradata_size), offsetof(AVCodecParameters, extradata_size));
	same("AVCodec.name", offsetof(struct codec_head, name), offsetof(AVCodec, name));
	same("AVCodec.long_name", offsetof(struct codec_head, long_name), offsetof(AVCodec, long_name));
	same("AVCodec.type", offsetof(struct codec_head, type), offsetof(AVCodec, type));
	same("AVCodec.id", offsetof(struct codec_head, id), offsetof(AVCodec, id));
	same("enum AVMediaType", sizeof(int), sizeof(enum AVMediaType));
	same("enum AVCodecID", sizeof(int), sizeof(enum AVCodecID));

	/* The verdict. */
	if (failures != 0) {
		printf("host-layout: FAIL major=%u failures=%d\n", wanted, failures);
		return 1;
	}
	printf("host-layout: PASS major=%u\n", wanted);
	return 0;
}

/* Reports one comparison. */
static void
same(
	const char *what,
	size_t ours,
	size_t theirs)
{
	/* A difference is counted. */
	if (ours != theirs) {
		printf("FAIL %s: ours %zu, FFmpeg's %zu\n", what, ours, theirs);
		failures++;
		return;
	}
	printf("ok %s %zu\n", what, ours);
}
