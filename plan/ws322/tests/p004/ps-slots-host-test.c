/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the video engine's pure parts (ws322-p004): the H.264
 * parameter sets written back from their StdVideo fields
 * (src/drivers/vpu/vpu-h264-ps.c) byte for byte against the Raspberry
 * Pi 4 driver's writer the VideoCore decoder took (rpi4-h264.c, WS236),
 * over fixed and random sets with and without the no-reorder VUI; and
 * the ties of DPB slots to frame buffers (vpu-slots.c): the pick avoids
 * every live slot's frame buffer, takes the setup slot's old one, the
 * least recently used of the free ones, and reports ENOSPC when none is
 * free.
 *
 *   ps-slots-host-test
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/platform/rpi4/rpi4-h264.h"
#include "drivers/vpu/vpu-codec.h"

/* How many checks ran and failed. */
static int passes;
static int failures;

/* The kernel's memory calls, the host's. */
void *kern_memset(void *to, int value, size_t bytes) { return memset(to, value, bytes); }
void *kern_memcpy(void *to, const void *from, size_t bytes) { return memcpy(to, from, bytes); }

/* Counts a check, naming a failed one. */
static void
check(
	int ok,
	const char *what)
{
	if (ok) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* A random number below limit (a fixed sequence). */
static uint32_t
draw(
	uint32_t limit)
{
	static uint64_t state = 0x2545f4914f6cdd1dULL;

	state ^= state << 13;
	state ^= state >> 7;
	state ^= state << 17;
	if (limit == 0)
		return (uint32_t)state;
	return (uint32_t)(state % limit);
}

/* Fills both writers' scaling lists the same way. */
static void
fill_scaling(
	struct vpu_h264_scaling *mine,
	struct drv_rpi4_h264_scaling *theirs)
{
	unsigned list;
	unsigned entry;

	mine->present_mask = draw(4096);
	mine->default_mask = draw(4096) & mine->present_mask;
	for (list = 0; list < 6; list++) {
		for (entry = 0; entry < 16; entry++)
			mine->list4[list][entry] = (uint8_t)(1 + draw(255));
		for (entry = 0; entry < 64; entry++)
			mine->list8[list][entry] = (uint8_t)(1 + draw(255));
	}
	theirs->present_mask = mine->present_mask;
	theirs->default_mask = mine->default_mask;
	memcpy(theirs->list4, mine->list4, sizeof(theirs->list4));
	memcpy(theirs->list8, mine->list8, sizeof(theirs->list8));
}

/* Writes one random SPS and PPS with both writers and compares the bytes. */
static void
compare_random(
	unsigned round)
{
	static const uint32_t profiles[] = { 66, 77, 88, 100, 110, 122, 244 };
	struct vpu_h264_sps sps;
	struct vpu_h264_pps pps;
	struct drv_rpi4_h264_sps old_sps;
	struct drv_rpi4_h264_pps old_pps;
	uint8_t mine[VPU_H264_SET_MAX];
	uint8_t theirs[VPU_H264_SET_MAX];
	size_t mine_bytes;
	size_t theirs_bytes;
	unsigned index;
	int reorder;
	int a;
	int b;
	char what[96];

	/* The same fields for both. */
	memset(&sps, 0, sizeof(sps));
	memset(&old_sps, 0, sizeof(old_sps));
	sps.flags = draw(0x8000);
	sps.profile_idc = profiles[draw(7)];
	sps.level = draw(19);
	sps.chroma_format_idc = draw(4);
	sps.id = draw(32);
	sps.bit_depth_luma_minus8 = draw(7);
	sps.bit_depth_chroma_minus8 = draw(7);
	sps.log2_max_frame_num_minus4 = draw(13);
	sps.pic_order_cnt_type = draw(3);
	sps.offset_for_non_ref_pic = (int32_t)draw(2001) - 1000;
	sps.offset_for_top_to_bottom_field = (int32_t)draw(2001) - 1000;
	sps.log2_max_pic_order_cnt_lsb_minus4 = draw(13);
	sps.max_num_ref_frames = draw(17);
	sps.pic_width_in_mbs_minus1 = draw(256);
	sps.pic_height_in_map_units_minus1 = draw(256);
	for (index = 0; index < 4; index++)
		sps.crop[index] = draw(64);
	sps.offset_count = draw(8);
	for (index = 0; index < sps.offset_count; index++)
		sps.offsets[index] = (int32_t)draw(4001) - 2000;
	old_sps.flags = sps.flags;
	old_sps.profile_idc = sps.profile_idc;
	old_sps.level = sps.level;
	old_sps.chroma_format_idc = sps.chroma_format_idc;
	old_sps.id = sps.id;
	old_sps.bit_depth_luma_minus8 = sps.bit_depth_luma_minus8;
	old_sps.bit_depth_chroma_minus8 = sps.bit_depth_chroma_minus8;
	old_sps.log2_max_frame_num_minus4 = sps.log2_max_frame_num_minus4;
	old_sps.pic_order_cnt_type = sps.pic_order_cnt_type;
	old_sps.offset_for_non_ref_pic = sps.offset_for_non_ref_pic;
	old_sps.offset_for_top_to_bottom_field = sps.offset_for_top_to_bottom_field;
	old_sps.log2_max_pic_order_cnt_lsb_minus4 = sps.log2_max_pic_order_cnt_lsb_minus4;
	old_sps.max_num_ref_frames = sps.max_num_ref_frames;
	old_sps.pic_width_in_mbs_minus1 = sps.pic_width_in_mbs_minus1;
	old_sps.pic_height_in_map_units_minus1 = sps.pic_height_in_map_units_minus1;
	memcpy(old_sps.crop, sps.crop, sizeof(old_sps.crop));
	old_sps.offset_count = sps.offset_count;
	memcpy(old_sps.offsets, sps.offsets, sizeof(old_sps.offsets));
	fill_scaling(&sps.scaling, &old_sps.scaling);
	sps.has_scaling = old_sps.has_scaling = 1;

	/* The SPS, with or without the VUI. */
	reorder = (int)draw(2);
	a = vpu_h264_write_sps(&sps, reorder, mine, sizeof(mine), &mine_bytes);
	b = drv_rpi4_h264_write_sps(&old_sps, reorder, theirs, sizeof(theirs), &theirs_bytes);
	snprintf(what, sizeof(what), "round %u: SPS (reorder %d) %d/%d %zu/%zu bytes", round, reorder, a, b, a == 0 ? mine_bytes : 0, b == 0 ? theirs_bytes : 0);
	check(a == b && (a != 0 || (mine_bytes == theirs_bytes && memcmp(mine, theirs, mine_bytes) == 0)), what);

	/* The PPS. */
	memset(&pps, 0, sizeof(pps));
	memset(&old_pps, 0, sizeof(old_pps));
	pps.flags = draw(0x100);
	pps.sps_id = sps.id;
	pps.pps_id = draw(256);
	pps.num_ref_idx_l0_default_active_minus1 = draw(32);
	pps.num_ref_idx_l1_default_active_minus1 = draw(32);
	pps.weighted_bipred_idc = draw(3);
	pps.pic_init_qp_minus26 = (int32_t)draw(52) - 26;
	pps.pic_init_qs_minus26 = (int32_t)draw(52) - 26;
	pps.chroma_qp_index_offset = (int32_t)draw(25) - 12;
	pps.second_chroma_qp_index_offset = (int32_t)draw(25) - 12;
	old_pps.flags = pps.flags;
	old_pps.sps_id = pps.sps_id;
	old_pps.pps_id = pps.pps_id;
	old_pps.num_ref_idx_l0_default_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
	old_pps.num_ref_idx_l1_default_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
	old_pps.weighted_bipred_idc = pps.weighted_bipred_idc;
	old_pps.pic_init_qp_minus26 = pps.pic_init_qp_minus26;
	old_pps.pic_init_qs_minus26 = pps.pic_init_qs_minus26;
	old_pps.chroma_qp_index_offset = pps.chroma_qp_index_offset;
	old_pps.second_chroma_qp_index_offset = pps.second_chroma_qp_index_offset;
	fill_scaling(&pps.scaling, &old_pps.scaling);
	pps.has_scaling = old_pps.has_scaling = 1;
	a = vpu_h264_write_pps(&pps, &sps, mine, sizeof(mine), &mine_bytes);
	b = drv_rpi4_h264_write_pps(&old_pps, &old_sps, theirs, sizeof(theirs), &theirs_bytes);
	snprintf(what, sizeof(what), "round %u: PPS %d/%d", round, a, b);
	check(a == b && (a != 0 || (mine_bytes == theirs_bytes && memcmp(mine, theirs, mine_bytes) == 0)), what);
}

/* A fixed 1080p High SPS without VUI: its bytes as ITU-T H.264 7.3.2.1.1 codes them. */
static void
check_fixed(void)
{
	static const uint8_t expected[] = {
		0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0x78, 0x02, 0x27, 0xe5, 0x40
	};
	struct vpu_h264_sps sps;
	uint8_t out[VPU_H264_SET_MAX];
	size_t bytes;
	int error;

	/*
	 * profile 100, level 4.0, id 0, 4:2:0, 8 bits, log2_max_frame_num 4,
	 * order count type 0 with lsb 6 bits, 4 references, 120x68 macroblocks,
	 * frames, direct 8x8, crop bottom 4 (8 rows).
	 */
	memset(&sps, 0, sizeof(sps));
	sps.flags = VPU_H264_SPS_FRAME_MBS_ONLY | VPU_H264_SPS_DIRECT_8X8 | VPU_H264_SPS_CROPPING;
	sps.profile_idc = 100;
	sps.level = 10;
	sps.chroma_format_idc = 1;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 2;
	sps.max_num_ref_frames = 4;
	sps.pic_width_in_mbs_minus1 = 119;
	sps.pic_height_in_map_units_minus1 = 67;
	sps.crop[3] = 4;
	error = vpu_h264_write_sps(&sps, 0, out, sizeof(out), &bytes);
	check(error == 0, "fixed SPS written");
	if (error != 0)
		return;
	if (bytes != sizeof(expected) || memcmp(out, expected, bytes) != 0) {
		size_t i;
		printf("fixed SPS:");
		for (i = 0; i < bytes; i++)
			printf(" %02x", out[i]);
		printf("\n");
	}
	check(bytes == sizeof(expected) && memcmp(out, expected, bytes) == 0, "fixed SPS bytes");
}

/* Out-of-range fields are refused. */
static void
check_refusals(void)
{
	struct vpu_h264_sps sps;
	struct vpu_h264_pps pps;
	uint8_t out[VPU_H264_SET_MAX];
	size_t bytes;

	memset(&sps, 0, sizeof(sps));
	sps.id = 32;
	check(vpu_h264_write_sps(&sps, 0, out, sizeof(out), &bytes) == EINVAL, "SPS id 32 refused");
	sps.id = 0;
	sps.level = 19;
	check(vpu_h264_write_sps(&sps, 0, out, sizeof(out), &bytes) == EINVAL, "SPS level past 6.2 refused");
	sps.level = 0;
	check(vpu_h264_write_sps(&sps, 0, out, 4, &bytes) == ENOSPC, "SPS too small an output refused");
	memset(&pps, 0, sizeof(pps));
	pps.pic_init_qp_minus26 = 26;
	check(vpu_h264_write_pps(&pps, NULL, out, sizeof(out), &bytes) == EINVAL, "PPS qp 52 refused");
}

/* The slots' ties to frame buffers. */
static void
check_slots(void)
{
	struct vpu_slots slots;
	uint32_t index;
	uint32_t picture;
	int error;

	/* Five frame buffers; each picture becomes slot (picture % 4), referencing the slot before. */
	vpu_slots_reset(&slots, 5);
	error = vpu_slots_pick(&slots, 0, 0, 0, &index);
	check(error == 0 && index == 0, "first pick is frame buffer 0");
	for (picture = 0; picture < 40; picture++) {
		uint32_t setup = picture % 4U;
		uint32_t references = picture == 0 ? 0 : 1U << ((picture + 3U) % 4U);
		uint32_t active = 0xfU & ~(1U << setup);
		uint32_t slot;
		int clash = 0;

		if (picture < 4)
			active &= (1U << picture) - 1U;
		error = vpu_slots_pick(&slots, references, active | (1U << setup), (int32_t)setup, &index);
		if (error != 0) {
			check(0, "pick in the sequence");
			return;
		}
		for (slot = 0; slot < 4; slot++) {
			if (slot != setup && ((active | references) & (1U << slot)) && slots.buffer[slot] == (int32_t)index)
				clash = 1;
		}
		check(!clash && index < 5, "pick avoids live slots");
		vpu_slots_bind(&slots, (int32_t)setup, index);
		check(slots.buffer[setup] == (int32_t)index && slots.slot[index] == (int32_t)setup, "bind ties both ways");
	}

	/* Every frame buffer live: ENOSPC. */
	vpu_slots_reset(&slots, 3);
	for (index = 0; index < 3; index++)
		vpu_slots_bind(&slots, (int32_t)index, index);
	error = vpu_slots_pick(&slots, 0x1, 0x7, 3, &index);
	check(error == ENOSPC, "no free frame buffer is ENOSPC");

	/* The setup slot's own old frame buffer is free unless referenced. */
	error = vpu_slots_pick(&slots, 0x1, 0x7, 2, &index);
	check(error == 0 && index == 2, "setup slot's frame buffer reused");
	error = vpu_slots_pick(&slots, 0x5, 0x7, 2, &index);
	check(error == ENOSPC, "setup slot referenced too stays");

	/* Least recently used among free ones: bind 0,1,2 in order, free all: 0 is oldest. */
	vpu_slots_keep(&slots, 0);
	error = vpu_slots_pick(&slots, 0, 0, -1, &index);
	check(error == 0 && index == 0, "least recently used picked");
	vpu_slots_bind(&slots, -1, 0);
	error = vpu_slots_pick(&slots, 0, 0, -1, &index);
	check(error == 0 && index == 1, "next least recently used picked");
	check(slots.slot[0] == VPU_NONE, "a picture without a slot holds none");

	/* Keep forgets the others. */
	vpu_slots_bind(&slots, 5, 1);
	vpu_slots_bind(&slots, 6, 2);
	vpu_slots_keep(&slots, 1U << 6);
	check(slots.buffer[5] == VPU_NONE && slots.slot[1] == VPU_NONE && slots.buffer[6] == 2, "keep forgets the others only");
}

int
main(void)
{
	unsigned round;

	check_fixed();
	check_refusals();
	for (round = 0; round < 2000; round++)
		compare_random(round);
	check_slots();
	printf("ps-slots: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
