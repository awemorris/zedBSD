/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the HCP builder (src/drivers/gpu/i915/vk/vk-video-hcp.c): a
 * resolved decode of made-up sets and slices is built, then the batch is
 * walked by each command's DWord Length: every command must be one of the
 * HCP, VD, MI and MFX_WAIT commands with the packet length Mesa 25.0.7's
 * genxml gives it (gen90.xml, gen110.xml, gen120.xml), in the order of a
 * picture, and named fields are checked at their dwords and bits.
 * Last line: hcp: PASS or hcp: FAIL.
 */

#include <stdio.h>
#include <string.h>

#include "drivers/gpu/i915/vk/vk-video-hcp.h"
#include "drivers/gpu/i915/intel/genxml-video.h"

static unsigned failures;
static unsigned checks;

void
drv_i915_batch_emit(
	struct i915_gfx_batch *batch,
	uint32_t dword)
{
	if (batch->count < batch->capacity)
		batch->cmds[batch->count] = dword;
	else
		batch->overflow = 1;
	batch->count++;
}

static void
check(const char *what, uint32_t got, uint32_t want)
{
	checks++;
	if (got != want) {
		failures++;
		printf("FAIL %s: 0x%08x, expected 0x%08x\n", what, got, want);
	}
}

/* Mesa genxml's lengths of the commands the builder writes (header opcode, dwords). */
struct known {
	uint32_t header;
	unsigned dwords;
	const char *name;
};

static const struct known knowns[] = {
	{ 0x73800000U, 7U, "HCP_PIPE_MODE_SELECT" },
	{ 0x73810000U, 5U, "HCP_SURFACE_STATE" },
	{ 0x73820000U, 121U, "HCP_PIPE_BUF_ADDR_STATE" },
	{ 0x73830000U, 29U, "HCP_IND_OBJ_BASE_ADDR_STATE" },
	{ 0x73840000U, 18U, "HCP_QM_STATE" },
	{ 0x73900000U, 41U, "HCP_PIC_STATE" },
	{ 0x73910000U, 17U, "HCP_TILE_STATE" },
	{ 0x73920000U, 18U, "HCP_REF_IDX_STATE" },
	{ 0x73930000U, 42U, "HCP_WEIGHTOFFSET_STATE" },
	{ 0x73940000U, 13U, "HCP_SLICE_STATE" },
	{ 0x73a00000U, 3U, "HCP_BSD_OBJECT" },
	{ 0x738a0000U, 3U, "VD_CONTROL_STATE" },
	{ 0x77800000U, 2U, "VD_PIPELINE_FLUSH" },
};

/* Walks a batch: returns the number of commands, or 0 on an unknown one; the names go to order. */
static unsigned
walk(const uint32_t *cmds, unsigned count, const char **order, unsigned *at, unsigned room)
{
	unsigned index;
	unsigned commands;
	unsigned known;
	unsigned dwords;

	index = 0U;
	commands = 0U;
	while (index < count) {
		uint32_t header = cmds[index];
		const char *name = NULL;
		if (header == 0x13000003U || header == (0x13000003U | (1U << 7))) {
			name = "MI_FLUSH_DW";
			dwords = 5U;
		} else if (header == 0x0e800000U) {
			name = "MI_FORCE_WAKEUP";
			dwords = 2U;
		} else if (header == 0x68000100U) {
			name = "MFX_WAIT";
			dwords = 1U;
		} else {
			for (known = 0U; known < sizeof(knowns) / sizeof(knowns[0]); known++) {
				if ((header & 0xffff0000U) == knowns[known].header) {
					name = knowns[known].name;
					dwords = knowns[known].dwords;
					check(name, (header & 0xfffU) + 2U, dwords);
					break;
				}
			}
		}
		if (name == NULL) {
			printf("FAIL unknown command 0x%08x at dword %u\n", header, index);
			failures++;
			return 0U;
		}
		if (commands < room)
			order[commands] = name;
		at[commands < room ? commands : room - 1U] = index;
		index += dwords;
		commands++;
	}
	check("batch ends at a command's end", index, count);
	return commands;
}

int
main(void)
{
	static uint32_t cmds[I915_VIDEO_HCP_MAX_DWORDS + 16U];
	static struct i915_video_h265_slice slices[3];
	static uint32_t starts[3] = { 100U, 2000U, 4000U };
	static uint32_t ends[3] = { 2000U, 4000U, 9000U };
	struct i915_video_h265_sps sps;
	struct i915_video_h265_pps pps;
	struct i915_video_hcp_decode decode;
	struct i915_gfx_batch batch;
	const char *order[256];
	unsigned at[256];
	unsigned commands;
	unsigned index;
	unsigned slice_state;
	unsigned bsd;

	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	memset(&decode, 0, sizeof(decode));
	memset(slices, 0, sizeof(slices));

	/* 1920x1080, CTB 64, min CB 8, TB 4..32, SAO, AMP, temporal MVP, tiles 2x2. */
	sps.chroma_format_idc = 1U;
	sps.width = 1920U;
	sps.height = 1080U;
	sps.log2_min_cb_minus3 = 0U;
	sps.log2_diff_cb = 3U;
	sps.log2_min_tb_minus2 = 0U;
	sps.log2_diff_tb = 3U;
	sps.depth_inter = 2U;
	sps.depth_intra = 1U;
	sps.flags = I915_VIDEO_H265_SPS_SAO | I915_VIDEO_H265_SPS_AMP | I915_VIDEO_H265_SPS_TEMPORAL_MVP;
	pps.flags = I915_VIDEO_H265_PPS_TILES | I915_VIDEO_H265_PPS_UNIFORM | I915_VIDEO_H265_PPS_CU_QP_DELTA | I915_VIDEO_H265_PPS_WEIGHTED;
	pps.tile_columns_minus1 = 1U;
	pps.tile_rows_minus1 = 1U;
	pps.cb_qp_offset = -2;
	pps.cu_qp_delta_depth = 1U;

	/* Slices: I, P (weighted), B; the second at CTB 30 + 1 row. */
	slices[0].slice_type = I915_VIDEO_H265_SLICE_I;
	slices[0].qp = 30;
	slices[0].max_merge_candidates = 5U;
	slices[0].collocated_from_l0 = true;
	slices[0].header_bytes = 9U;
	slices[1].slice_type = I915_VIDEO_H265_SLICE_P;
	slices[1].address = 31U;
	slices[1].slice_address = 31U;
	slices[1].qp = 27;
	slices[1].references[0] = 2U;
	slices[1].list[0][0] = 3U;
	slices[1].list[0][1] = 5U;
	slices[1].temporal_mvp = true;
	slices[1].collocated_from_l0 = true;
	slices[1].collocated_ref_idx = 1U;
	slices[1].weighted = true;
	slices[1].weights.luma_log2_denom = 6U;
	slices[1].weights.chroma_log2_denom = 5U;
	slices[1].weights.luma_sent[0][1] = true;
	slices[1].weights.luma_weight[0][0] = 64;
	slices[1].weights.luma_weight[0][1] = 70;
	slices[1].weights.luma_offset[0][1] = -3;
	slices[1].weights.chroma_weight[0][0][0] = 32;
	slices[1].weights.chroma_weight[0][0][1] = 32;
	slices[1].weights.chroma_weight[0][1][0] = 32;
	slices[1].weights.chroma_weight[0][1][1] = 32;
	slices[1].max_merge_candidates = 3U;
	slices[1].tc_div2 = -2;
	slices[1].beta_div2 = 3;
	slices[1].header_bytes = 14U;
	slices[2] = slices[1];
	slices[2].slice_type = I915_VIDEO_H265_SLICE_B;
	slices[2].address = 62U;
	slices[2].slice_address = 62U;
	slices[2].weighted = false;
	slices[2].references[1] = 1U;
	slices[2].list[1][0] = 7U;

	decode.sps = &sps;
	decode.pps = &pps;
	decode.poc = 8;
	decode.destination = 0x100000U;
	decode.pitch = 1920U;
	decode.chroma_rows = 1088U;
	for (index = 0U; index < I915_VIDEO_HCP_LINE_BUFFERS; index++)
		decode.line_buffers[index] = 0x200000U + index * 0x10000U;
	decode.motion_write = 0x400000U;
	decode.reference_count = 3U;
	for (index = 0U; index < 3U; index++) {
		decode.references[index].picture = 0x1000000U * (index + 1U);
		decode.references[index].motion = 0x500000U + index * 0x10000U;
	}
	for (index = 0U; index < I915_VIDEO_H265_SLOTS; index++)
		decode.place[index] = I915_VIDEO_H265_NO_SLOT;
	decode.place[3] = 0U;
	decode.place[5] = 1U;
	decode.place[7] = 2U;
	decode.slots[3].present = true;
	decode.slots[3].poc = 4;
	decode.slots[5].present = true;
	decode.slots[5].poc = 6;
	decode.slots[7].present = true;
	decode.slots[7].poc = 12;
	decode.slots[7].long_term = true;
	decode.bitstream_base = 0x800000U;
	decode.bitstream_end = 0x900000U;
	decode.skew = 0x40U;
	decode.slice_count = 3U;
	decode.slices = slices;
	decode.slice_starts = starts;
	decode.slice_ends = ends;
	decode.mocs = 2U;
	drv_i915_video_h265_matrices(&sps, &pps, &decode.matrices);

	memset(&batch, 0, sizeof(batch));
	batch.cmds = cmds;
	batch.capacity = sizeof(cmds) / sizeof(cmds[0]);
	drv_i915_video_hcp_build(&batch, &decode);
	check("no overflow", (uint32_t)batch.overflow, 0U);
	commands = walk(cmds, batch.count, order, at, 256U);
	printf("%u dwords, %u commands\n", batch.count, commands);

	/* The order: flush, wake, VD init, wait, mode, wait, 2 surfaces, buffers, bitstream, 20 QM, picture, tiles. */
	if (commands > 32U) {
		const char *expected[] = { "MI_FLUSH_DW", "MI_FORCE_WAKEUP", "VD_CONTROL_STATE", "MFX_WAIT", "HCP_PIPE_MODE_SELECT", "MFX_WAIT",
			"HCP_SURFACE_STATE", "HCP_SURFACE_STATE", "HCP_PIPE_BUF_ADDR_STATE", "HCP_IND_OBJ_BASE_ADDR_STATE" };
		for (index = 0U; index < 10U; index++) {
			checks++;
			if (strcmp(order[index], expected[index]) != 0) {
				failures++;
				printf("FAIL command %u is %s, expected %s\n", index, order[index], expected[index]);
			}
		}
		for (index = 10U; index < 30U; index++) {
			checks++;
			if (strcmp(order[index], "HCP_QM_STATE") != 0) {
				failures++;
				printf("FAIL command %u is %s, expected HCP_QM_STATE\n", index, order[index]);
			}
		}
		checks += 2U;
		if (strcmp(order[30], "HCP_PIC_STATE") != 0 || strcmp(order[31], "HCP_TILE_STATE") != 0) {
			failures++;
			printf("FAIL picture and tile state order\n");
		}
	}

	/* Fields: the buffer state's addresses at their dwords. */
	{
		unsigned b = at[8];
		check("decoded picture", cmds[b + GEN12_VIDEO_HCP_BUF_DECODED], 0x100000U);
		check("deblocking line", cmds[b + GEN12_VIDEO_HCP_BUF_LINE_BUFFERS], 0x200000U);
		check("SAO tile column", cmds[b + GEN12_VIDEO_HCP_BUF_LINE_BUFFERS + 8U * 3U], 0x280000U);
		check("current MV", cmds[b + GEN12_VIDEO_HCP_BUF_CURRENT_MV], 0x400000U);
		check("reference 0", cmds[b + GEN12_VIDEO_HCP_BUF_REFERENCES], 0x1000000U);
		check("reference 2", cmds[b + GEN12_VIDEO_HCP_BUF_REFERENCES + 4U], 0x3000000U);
		check("reference 3 repeats 0", cmds[b + GEN12_VIDEO_HCP_BUF_REFERENCES + 6U], 0x1000000U);
		check("reference attributes", cmds[b + GEN12_VIDEO_HCP_BUF_REFERENCE_ATTRIBUTES], 2U);
		check("collocated MV 1", cmds[b + GEN12_VIDEO_HCP_BUF_COLLOCATED_MV + 2U], 0x510000U);
		check("collocated attributes", cmds[b + GEN12_VIDEO_HCP_BUF_COLLOCATED_ATTRIBUTES], 2U);
	}
	/* The picture state: 1920/8-1, 1080/8-1; LCU 64 (3), max TU 32 (3). */
	{
		unsigned p = at[30];
		check("picture size", cmds[p + 1U], (239U) | (134U << 16));
		check("block sizes", cmds[p + 2U] & 0xfffU, 0U | (3U << 2) | (0U << 4) | (3U << 6));
		check("chroma 4:2:0", cmds[p + 2U] >> 29, 1U);
		check("SAO, CU QP, tiles, weighted", cmds[p + 4U] & ((1U << 3) | (1U << 5) | (1U << 17) | (1U << 19)), (1U << 3) | (1U << 5) | (1U << 17) | (1U << 19));
		check("Cb offset -2", cmds[p + 5U] & 0x1fU, 0x1eU);
	}
	/* The tile state: 2x2, column starts 0 and 15, row starts 0 and 8 (17 CTB rows, uniform). */
	{
		unsigned t = at[31];
		check("tile counts", cmds[t + 1U], 1U | (1U << 5));
		check("column starts", cmds[t + 2U], 0U | (15U << 8));
		check("row starts", cmds[t + 7U], 0U | (8U << 8));
	}
	/* The slices: slice state, then (P) ref idx, weights, BSD; (B) two ref idx, BSD. */
	slice_state = 32U;
	check("slice 0 position", cmds[at[slice_state] + 1U], 0U);
	check("slice 0 next position", cmds[at[slice_state] + 2U], 1U | (1U << 16));
	check("slice 0 type I QP 30", cmds[at[slice_state] + 3U] & 0xfc3U, 2U | (30U << 6));
	check("slice 0 header length", cmds[at[slice_state] + 5U], 9U);
	bsd = slice_state + 1U;
	check("slice 0 BSD length", cmds[at[bsd] + 1U], 1900U);
	check("slice 0 BSD start", cmds[at[bsd] + 2U], 0x40U + 100U);
	slice_state = bsd + 1U;
	check("slice 1 position (31 = 1,1)", cmds[at[slice_state] + 1U], 1U | (1U << 16));
	check("slice 1 collocated place 1, L0, low delay", cmds[at[slice_state] + 4U] & ((7U << 26) | (1U << 15) | (1U << 14)), (1U << 26) | (1U << 15) | (1U << 14));
	check("slice 1 tc -2 beta 3", cmds[at[slice_state] + 4U] & 0x1feU, (0xeU << 1) | (3U << 5));
	check("slice 1 denominators", (cmds[at[slice_state] + 4U] >> 16) & 0x3fU, 5U | (6U << 3));
	check("slice 1 max merge 3", (cmds[at[slice_state] + 4U] >> 23) & 7U, 2U);
	check("slice 1 ref idx L0 active 2", cmds[at[slice_state + 1U] + 1U], 0U | (1U << 1));
	check("slice 1 L0[0] place 0 tb 4", cmds[at[slice_state + 1U] + 2U] & 0x7ffU, 4U | (0U << 8));
	check("slice 1 L0[1] place 1 tb 2 luma weighted", cmds[at[slice_state + 1U] + 3U] & 0x17ffU, 2U | (1U << 8) | (1U << 12));
	check("slice 1 weights L0[1] delta 6 offset -3", cmds[at[slice_state + 2U] + 3U] & 0xffffU, 6U | (0xfdU << 8));
	slice_state += 4U;
	check("slice 2 type B", cmds[at[slice_state] + 3U] & 3U, 0U);
	check("slice 2 not low delay (L1 POC 12 after 8)", cmds[at[slice_state] + 4U] & (1U << 14), 0U);
	check("slice 2 last", cmds[at[slice_state] + 3U] & (1U << 2), 1U << 2);
	check("slice 2 L1[0] long-term place 2 tb -4", cmds[at[slice_state + 2U] + 2U] & 0x27ffU, 0xfcU | (2U << 8) | (1U << 13));
	check("closing VD flush", cmds[at[commands - 2U] + 1U], 1U | (1U << 4) | (1U << 16));

	/* A picture of 256 slices with both lists and weights fits the batch. */
	printf("%u checks, %u failed\n", checks, failures);
	printf("hcp: %s\n", failures == 0U ? "PASS" : "FAIL");
	return failures == 0U ? 0 : 1;
}
