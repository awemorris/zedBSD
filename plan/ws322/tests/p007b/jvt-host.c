/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host side of the JVT check of the VF2's VPU (ws322-p007b): reads a
 * stream with vkvideo-probe's own reader and DPB (h264.c, dpb.c) as the
 * probe does, and writes the bytes the Wave5 engine puts in its ring for
 * each picture (wave5-video.c's put_picture: the SPS and the PPS written
 * back by vpu-h264-ps.c when they differ from the ones sent last or the
 * picture is an IDR picture, then the slices behind three-byte start
 * codes), so that a reference decoder can decode what the firmware gets.
 *
 *   jvt-host STREAM.h264 OUT.h264 [vui]
 *
 * One line per picture goes to the standard output (P): its decode index,
 * order count, IDR flag, slice type, frame_num, references, its place in
 * the display order (the smaller count after an operation 5) and its
 * period (IDR pictures and operations 5 start one); then O lines give the
 * decode indices in the order the probe prints them (main.c's
 * probe_output_add and probe_output_flush).  The probe's reason ends the
 * stream as it ends the probe's run.  With vui, the SPS carries the
 * no-reorder VUI the engine writes; without, none (a reference decoder
 * then keeps the stream's own display order).
 */

#include "h264.h"
#include "dpb.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/vpu/vpu-codec.h"

void *kern_memset(void *to, int value, size_t bytes) { return memset(to, value, bytes); }
void *kern_memcpy(void *to, const void *from, size_t bytes) { return memcpy(to, from, bytes); }

/* StdVideoH264ScalingLists into the writer's form. */
static void
convert_scaling(const StdVideoH264ScalingLists *from, struct vpu_h264_scaling *to)
{
	to->present_mask = from->scaling_list_present_mask;
	to->default_mask = from->use_default_scaling_matrix_mask;
	memcpy(to->list4, from->ScalingList4x4, sizeof(to->list4));
	memcpy(to->list8, from->ScalingList8x8, sizeof(to->list8));
}

/* The SPS as libvulkan sends it and vk-video.c reads it. */
static void
convert_sps(const StdVideoH264SequenceParameterSet *from, struct vpu_h264_sps *to)
{
	uint32_t index;

	memset(to, 0, sizeof(*to));
	to->flags = (uint32_t)from->flags.constraint_set0_flag | ((uint32_t)from->flags.constraint_set1_flag << 1) |
		((uint32_t)from->flags.constraint_set2_flag << 2) | ((uint32_t)from->flags.constraint_set3_flag << 3) |
		((uint32_t)from->flags.constraint_set4_flag << 4) | ((uint32_t)from->flags.constraint_set5_flag << 5) |
		((uint32_t)from->flags.direct_8x8_inference_flag << 6) | ((uint32_t)from->flags.mb_adaptive_frame_field_flag << 7) |
		((uint32_t)from->flags.frame_mbs_only_flag << 8) | ((uint32_t)from->flags.delta_pic_order_always_zero_flag << 9) |
		((uint32_t)from->flags.separate_colour_plane_flag << 10) |
		((uint32_t)from->flags.gaps_in_frame_num_value_allowed_flag << 11) |
		((uint32_t)from->flags.qpprime_y_zero_transform_bypass_flag << 12) |
		((uint32_t)from->flags.frame_cropping_flag << 13) | ((uint32_t)from->flags.seq_scaling_matrix_present_flag << 14);
	to->profile_idc = from->profile_idc;
	to->level = from->level_idc;
	to->chroma_format_idc = from->chroma_format_idc;
	to->id = from->seq_parameter_set_id;
	to->bit_depth_luma_minus8 = from->bit_depth_luma_minus8;
	to->bit_depth_chroma_minus8 = from->bit_depth_chroma_minus8;
	to->log2_max_frame_num_minus4 = from->log2_max_frame_num_minus4;
	to->pic_order_cnt_type = from->pic_order_cnt_type;
	to->offset_for_non_ref_pic = from->offset_for_non_ref_pic;
	to->offset_for_top_to_bottom_field = from->offset_for_top_to_bottom_field;
	to->log2_max_pic_order_cnt_lsb_minus4 = from->log2_max_pic_order_cnt_lsb_minus4;
	to->max_num_ref_frames = from->max_num_ref_frames;
	to->pic_width_in_mbs_minus1 = from->pic_width_in_mbs_minus1;
	to->pic_height_in_map_units_minus1 = from->pic_height_in_map_units_minus1;
	to->crop[0] = from->frame_crop_left_offset;
	to->crop[1] = from->frame_crop_right_offset;
	to->crop[2] = from->frame_crop_top_offset;
	to->crop[3] = from->frame_crop_bottom_offset;
	to->offset_count = from->num_ref_frames_in_pic_order_cnt_cycle;
	for (index = 0; index < to->offset_count && from->pOffsetForRefFrame != NULL; index++)
		to->offsets[index] = from->pOffsetForRefFrame[index];
	if (from->pScalingLists != NULL) {
		to->has_scaling = 1;
		convert_scaling(from->pScalingLists, &to->scaling);
	}
}

/* The PPS as libvulkan sends it. */
static void
convert_pps(const StdVideoH264PictureParameterSet *from, struct vpu_h264_pps *to)
{
	memset(to, 0, sizeof(*to));
	to->flags = (uint32_t)from->flags.transform_8x8_mode_flag | ((uint32_t)from->flags.redundant_pic_cnt_present_flag << 1) |
		((uint32_t)from->flags.constrained_intra_pred_flag << 2) |
		((uint32_t)from->flags.deblocking_filter_control_present_flag << 3) |
		((uint32_t)from->flags.weighted_pred_flag << 4) |
		((uint32_t)from->flags.bottom_field_pic_order_in_frame_present_flag << 5) |
		((uint32_t)from->flags.entropy_coding_mode_flag << 6) | ((uint32_t)from->flags.pic_scaling_matrix_present_flag << 7);
	to->sps_id = from->seq_parameter_set_id;
	to->pps_id = from->pic_parameter_set_id;
	to->num_ref_idx_l0_default_active_minus1 = from->num_ref_idx_l0_default_active_minus1;
	to->num_ref_idx_l1_default_active_minus1 = from->num_ref_idx_l1_default_active_minus1;
	to->weighted_bipred_idc = from->weighted_bipred_idc;
	to->pic_init_qp_minus26 = from->pic_init_qp_minus26;
	to->pic_init_qs_minus26 = from->pic_init_qs_minus26;
	to->chroma_qp_index_offset = from->chroma_qp_index_offset;
	to->second_chroma_qp_index_offset = from->second_chroma_qp_index_offset;
	if (from->pScalingLists != NULL) {
		to->has_scaling = 1;
		convert_scaling(from->pScalingLists, &to->scaling);
	}
}

static uint8_t *
read_all(const char *path, size_t *size)
{
	FILE *file;
	uint8_t *data;
	long length;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	data = malloc((size_t)length + 1U);
	if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
		fclose(file);
		free(data);
		return NULL;
	}
	fclose(file);
	*size = (size_t)length;
	return data;
}

/* The probe's frames waiting for their place in the display order: their order counts and decode indices. */
static int32_t waiting_poc[256];
static uint32_t waiting_index[256];
static uint32_t waiting;

/* Prints the waiting frames by their counts as the probe does, but for the `keep` of the largest. */
static void
flush(uint32_t keep)
{
	uint32_t a, b, t, printing;
	int32_t p;

	for (a = 1; a < waiting; a++) {
		for (b = a; b > 0 && waiting_poc[b - 1] > waiting_poc[b]; b--) {
			p = waiting_poc[b]; waiting_poc[b] = waiting_poc[b - 1]; waiting_poc[b - 1] = p;
			t = waiting_index[b]; waiting_index[b] = waiting_index[b - 1]; waiting_index[b - 1] = t;
		}
	}
	printing = waiting > keep ? waiting - keep : 0;
	for (a = 0; a < printing; a++)
		printf("O %u\n", waiting_index[a]);
	memmove(waiting_poc, waiting_poc + printing, (waiting - printing) * sizeof(waiting_poc[0]));
	memmove(waiting_index, waiting_index + printing, (waiting - printing) * sizeof(waiting_index[0]));
	waiting -= printing;
}

int
main(int argc, char **argv)
{
	static struct h264_stream stream;
	static struct h264_picture picture;
	static struct dpb dpb;
	static uint8_t sps_unit[VPU_H264_SET_MAX], pps_unit[VPU_H264_SET_MAX];
	static uint8_t sent_sps[VPU_H264_SET_MAX], sent_pps[VPU_H264_SET_MAX];
	static const uint8_t start[3] = { 0, 0, 1 };
	struct vpu_h264_sps sps_fields;
	struct vpu_h264_pps pps_fields;
	const StdVideoH264SequenceParameterSet *sps;
	struct dpb_plan plan;
	const char *reason;
	uint8_t *data;
	size_t size, sps_bytes, pps_bytes, sent_sps_bytes, sent_pps_bytes;
	uint32_t frames, slice, period;
	int found, vui, idr, sps_new, pps_new, error;
	FILE *out;

	if (argc < 3) {
		fprintf(stderr, "usage: jvt-host STREAM.h264 OUT.h264 [vui]\n");
		return 2;
	}
	vui = argc > 3 && strcmp(argv[3], "vui") == 0;
	data = read_all(argv[1], &size);
	if (data == NULL || h264_open(&stream, data, size) != 0) {
		printf("STOP a parameter set the probe cannot read\n");
		return 3;
	}
	dpb_init(&dpb, stream.max_references);
	out = fopen(argv[2], "wb");
	if (out == NULL)
		return 2;
	sent_sps_bytes = 0;
	sent_pps_bytes = 0;
	frames = 0;
	period = 0;
	error = 0;
	for (;;) {
		found = h264_next_picture(&stream, &picture, &reason);
		if (found == 0)
			break;
		if (found < 0) {
			printf("STOP picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}
		if (picture.info.flags.IdrPicFlag || picture.restart) {
			flush(0);
			period++;
		}
		sps = &stream.sps[picture.info.seq_parameter_set_id];
		reason = dpb_plan(&dpb, sps, &picture, &plan);
		if (reason != NULL) {
			printf("STOP picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}

		/* The engine's put_picture. */
		convert_sps(sps, &sps_fields);
		convert_pps(&stream.pps[picture.info.pic_parameter_set_id], &pps_fields);
		if (vpu_h264_write_sps(&sps_fields, vui, sps_unit, sizeof(sps_unit), &sps_bytes) != 0 ||
		    vpu_h264_write_pps(&pps_fields, &sps_fields, pps_unit, sizeof(pps_unit), &pps_bytes) != 0) {
			printf("STOP picture %u: the sets were not written\n", frames);
			error = 3;
			break;
		}
		idr = picture.info.flags.IdrPicFlag;
		sps_new = sps_bytes != sent_sps_bytes || memcmp(sps_unit, sent_sps, sps_bytes) != 0;
		pps_new = pps_bytes != sent_pps_bytes || memcmp(pps_unit, sent_pps, pps_bytes) != 0;
		if (sps_new || idr)
			fwrite(sps_unit, 1, sps_bytes, out);
		if (pps_new || sps_new || idr)
			fwrite(pps_unit, 1, pps_bytes, out);
		for (slice = 0; slice < picture.slice_count; slice++) {
			fwrite(start, 1, 3, out);
			fwrite(data + picture.slice_offsets[slice], 1, picture.slice_sizes[slice], out);
		}
		memcpy(sent_sps, sps_unit, sps_bytes);
		sent_sps_bytes = sps_bytes;
		memcpy(sent_pps, pps_unit, pps_bytes);
		sent_pps_bytes = pps_bytes;

		printf("P %u poc %d idr %u type %u frame_num %u ref %u slices %u refs %u setup %d mmco %u pps %u display %d period %u generation %u\n",
		       frames, picture.info.PicOrderCnt[0], (unsigned)idr, picture.slice_type, picture.info.frame_num,
		       (unsigned)picture.info.flags.is_reference, picture.slice_count, plan.reference_count, plan.setup,
		       picture.adaptive_marking ? picture.mmco_count : 0U, picture.info.pic_parameter_set_id, picture.display_poc,
		       period, stream.generation);
		reason = dpb_mark(&dpb, sps, &picture, &plan);
		if (reason != NULL) {
			printf("STOP picture %u: %s\n", frames, reason);
			error = 3;
			break;
		}
		if (waiting >= 256U)
			flush(128U);
		waiting_poc[waiting] = picture.display_poc;
		waiting_index[waiting] = frames;
		waiting++;
		frames++;
	}
	flush(0);
	fclose(out);
	printf("END %u pictures\n", frames);
	free(data);
	return error;
}
