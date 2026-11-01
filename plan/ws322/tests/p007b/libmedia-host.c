/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host side of the JVT check of libmedia's H.264 reader and DPB
 * (ws322-p007b): reads a stream with libmedia's h264.c and h264-dpb.c as
 * its Vulkan Video backend does (vkvideo.c), and writes the bytes the Wave5
 * engine puts in its ring for each picture decoded (the sets written back
 * by vpu-h264-ps.c, then the slices), so that a reference decoder can
 * decode what the firmware gets.
 *
 *   libmedia-host STREAM.h264 OUT.h264
 *
 * One line per picture: its decode index, order counts, frame_num,
 * whether the plan decodes it, the setup slot and the references; the
 * reason ends the stream as it ends libmedia's.
 */

#include "h264.h"
#include "h264-dpb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/vpu/vpu-codec.h"

void *kern_memset(void *to, int value, size_t bytes) { return memset(to, value, bytes); }
void *kern_memcpy(void *to, const void *from, size_t bytes) { return memcpy(to, from, bytes); }

void convert_sps(const StdVideoH264SequenceParameterSet *from, struct vpu_h264_sps *to);
void convert_pps(const StdVideoH264PictureParameterSet *from, struct vpu_h264_pps *to);

int
main(int argc, char **argv)
{
	static struct h264_stream stream;
	static struct h264_picture picture;
	static struct h264_dpb dpb;
	static uint8_t sps_unit[VPU_H264_SET_MAX], pps_unit[VPU_H264_SET_MAX];
	static uint8_t sent_sps[VPU_H264_SET_MAX], sent_pps[VPU_H264_SET_MAX];
	static const uint8_t start[3] = { 0, 0, 1 };
	struct vpu_h264_sps sps_fields;
	struct vpu_h264_pps pps_fields;
	const StdVideoH264SequenceParameterSet *sps;
	struct h264_dpb_plan plan;
	const char *reason;
	uint8_t *data;
	size_t size, sps_bytes, pps_bytes, sent_sps_bytes = 0, sent_pps_bytes = 0;
	unsigned frames = 0, slice, index, max_references = 0;
	int found, error, idr;
	long length;
	FILE *file, *out;

	if (argc < 3)
		return 2;
	file = fopen(argv[1], "rb");
	if (file == NULL)
		return 2;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)length);
	if (fread(data, 1, (size_t)length, file) != (size_t)length)
		return 2;
	fclose(file);
	size = (size_t)length;
	if (h264_open(&stream, data, size) != 0) {
		printf("STOP open\n");
		return 3;
	}
	for (index = 0; index < H264_SPS_IDS; index++) {
		if (stream.has_sps[index] && stream.sps[index].max_num_ref_frames > max_references)
			max_references = stream.sps[index].max_num_ref_frames;
	}
	h264_dpb_init(&dpb, max_references);
	out = fopen(argv[2], "wb");
	for (;;) {
		found = h264_next_picture(&stream, &picture, &reason);
		if (found == 0)
			break;
		if (found < 0) {
			printf("STOP picture %u: %s (error %d)\n", frames, reason, stream.error);
			break;
		}
		error = h264_dpb_prepare(&dpb, &stream, &picture, &plan);
		if (error != 0) {
			printf("STOP picture %u: prepare %d\n", frames, error);
			break;
		}
		sps = &stream.sps[picture.info.seq_parameter_set_id];
		printf("P %u poc %d %d frame_num %u idr %u ref %u mmco5 %d decode %d setup %d refs %u\n", frames,
		       picture.info.PicOrderCnt[0], picture.info.PicOrderCnt[1], picture.info.frame_num,
		       (unsigned)picture.info.flags.IdrPicFlag, (unsigned)picture.info.flags.is_reference, picture.mmco5,
		       plan.decode, plan.setup, plan.reference_count);
		if (plan.decode) {
			convert_sps(sps, &sps_fields);
			convert_pps(&stream.pps[picture.info.pic_parameter_set_id], &pps_fields);
			if (vpu_h264_write_sps(&sps_fields, 0, sps_unit, sizeof(sps_unit), &sps_bytes) != 0 ||
			    vpu_h264_write_pps(&pps_fields, &sps_fields, pps_unit, sizeof(pps_unit), &pps_bytes) != 0) {
				printf("STOP picture %u: sets\n", frames);
				break;
			}
			idr = picture.info.flags.IdrPicFlag;
			if (idr || sps_bytes != sent_sps_bytes || memcmp(sps_unit, sent_sps, sps_bytes) != 0) {
				fwrite(sps_unit, 1, sps_bytes, out);
				fwrite(pps_unit, 1, pps_bytes, out);
			} else if (pps_bytes != sent_pps_bytes || memcmp(pps_unit, sent_pps, pps_bytes) != 0) {
				fwrite(pps_unit, 1, pps_bytes, out);
			}
			memcpy(sent_sps, sps_unit, sps_bytes);
			sent_sps_bytes = sps_bytes;
			memcpy(sent_pps, pps_unit, pps_bytes);
			sent_pps_bytes = pps_bytes;
			for (slice = 0; slice < picture.slice_count; slice++) {
				fwrite(start, 1, 3, out);
				fwrite(data + picture.slice_offsets[slice], 1, picture.slice_sizes[slice], out);
			}
		}
		error = h264_dpb_mark(&dpb, sps, &picture, &plan);
		if (error != 0) {
			printf("STOP picture %u: mark %d\n", frames, error);
			break;
		}
		frames++;
	}
	fclose(out);
	printf("END %u pictures\n", frames);
	return 0;
}
