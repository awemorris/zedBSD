/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Host metadata and admission checks use independently encoded pictures; they do not claim hardware pixel decoding. */
#include "userland/desktop/libmedia/h264-dpb.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Parses a complete independent stream and verifies that every physical reference points at an admitted picture.
 */
int
main(
	int argc,
	char **argv)
{
	FILE *file;
	long length;
	uint8_t *data;
	struct h264_stream *stream;
	struct h264_picture *picture;
	struct h264_dpb dpb;
	struct h264_dpb_plan plan;
	const char *reason;
	const StdVideoH264SequenceParameterSet *sps;
	unsigned pictures;
	unsigned dropped;
	unsigned b;
	unsigned index;
	int found;
	int error;

	/* Read only the fixture named by this bounded host test. */
	assert(argc == 2);
	file = fopen(argv[1], "rb");
	assert(file != NULL);
	error = fseek(file, 0, SEEK_END);
	assert(error == 0);
	length = ftell(file);
	assert(length > 0);
	rewind(file);
	data = malloc((size_t)length);
	assert(data != NULL);
	assert(fread(data, 1U, (size_t)length, file) == (size_t)length);
	fclose(file);
	stream = calloc(1U, sizeof(*stream));
	assert(stream != NULL);
	picture = calloc(1U, sizeof(*picture));
	assert(picture != NULL);

	/* Every picture is admitted and marked using the production parser and logical DPB. */
	error = h264_open(stream, data, (size_t)length);
	assert(error == 0);
	pictures = 0U;
	dropped = 0U;
	b = 0U;
	for (;;) {
		found = h264_next_picture(stream, picture, &reason);
		if (found == 0)
			break;
		if (found < 0) {
			fprintf(stderr, "H264 parser picture %u: %s errno %d\n", pictures, reason, stream->error);
			return 1;
		}

		/* Prepare references using the actual sequence associated with this parsed picture. */
		sps = &stream->sps[picture->info.seq_parameter_set_id];
		if (pictures == 0U)
			h264_dpb_init(&dpb, sps->max_num_ref_frames);
		error = h264_dpb_prepare(&dpb, stream, picture, &plan);
		if (error != 0) {
			fprintf(stderr, "H264 prepare picture %u errno %d\n", pictures, error);
			return 1;
		}

		/* Count recoverable drops separately from successfully admitted pictures. */
		if (!plan.decode)
			dropped++;
		if (picture->slice_type == H264_SLICE_B)
			b++;
		printf("picture %u frame %u poc %d type %u slices %u decode %d refs %u\n", pictures, picture->info.frame_num, picture->info.PicOrderCnt[0], picture->slice_type, picture->slice_count, plan.decode, plan.reference_count);
		for (index = 0U; index < plan.reference_count; index++) {
			assert(plan.references[index] >= 0);
			assert((unsigned)plan.references[index] < dpb.slots);
			assert(dpb.active[plan.references[index]]);
		}

		/* Commit marking after checking every physical reference slot. */
		error = h264_dpb_mark(&dpb, sps, picture, &plan);
		assert(error == 0);
		pictures++;
	}

	/* A complete ordinary stream must not drop any picture because of its reference bookkeeping. */
	assert(pictures > 0U);
	assert(dropped == 0U);
	printf("H264 host PASS pictures=%u B=%u dropped=%u\n", pictures, b, dropped);
	free(picture);
	free(stream);
	free(data);
	return 0;
}
