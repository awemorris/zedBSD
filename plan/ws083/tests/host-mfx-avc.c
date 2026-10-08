/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host golden of the MFX AVC builder (ws083-p004, design §8.1, D24).
 *
 * Each case builds one decode with render/video-mfx.c and writes the batch
 * and the fields it expects to a directory: CASE.bin (the dwords) and
 * CASE.expect (genxml-decode.py's checks).  The expected values are worked
 * out here from the StdVideo inputs of the case, with their own zig-zag
 * scan and their own default lists, not from the builder's transcription;
 * genxml-decode.py reads the batch back through Mesa's genxml and compares.
 *
 *   host-mfx-avc DIRECTORY
 */

#include "../../../src/drivers/gpu/i915/render/video-mfx.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The room of a case's batch, in dwords. */
#define FIXTURE_BATCH_DWORDS	8192U

/* The MOCS of every buffer (the uncached entry 3, shifted left by one). */
#define FIXTURE_MOCS		6U

/* The addresses a case's buffers are at: distinct pages, above 4 GiB to exercise the high dword. */
#define FIXTURE_DESTINATION	0x123450000ULL
#define FIXTURE_ROW_STORE	0x200000000ULL
#define FIXTURE_MOTION_WRITE	0x210000000ULL
#define FIXTURE_REFERENCE	0x300000000ULL
#define FIXTURE_MOTION		0x310000000ULL
#define FIXTURE_BITSTREAM	0x400000000ULL

/* One case's output: the batch, and the checks written so far. */
struct fixture_case {
	const char *name;
	FILE *expect;
	uint32_t words[FIXTURE_BATCH_DWORDS];
	struct i915_gfx_batch batch;
	unsigned instructions;
};

/* Where the cases' files go; set once by main. */
static const char *fixture_directory;

/* Default_4x4_Intra, _Inter, Default_8x8_Intra, _Inter (H.264 Tables 7-3, 7-4), typed again for the golden. */
static const uint8_t golden_default4[2][16] = {
	{ 6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42 },
	{ 10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34 }
};
static const uint8_t golden_default8[2][64] = {
	{ 6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	  23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	  27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	  31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42 },
	{ 9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	  21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	  24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	  27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35 }
};

static void fixture_begin(struct fixture_case *test, const char *name);
static void fixture_end(struct fixture_case *test);
static void fixture_instruction(struct fixture_case *test, const char *name);
static void fixture_field(struct fixture_case *test, const char *field, long long value);
static void fixture_fieldf(struct fixture_case *test, long long value, const char *format, unsigned index);
static unsigned golden_scan_to_raster(unsigned size, unsigned scan);
static void golden_matrix4(struct fixture_case *test, const uint8_t lists[3][16]);
static void golden_matrix8(struct fixture_case *test, const uint8_t *list);
static void golden_head(struct fixture_case *test, uint32_t width, uint32_t height, uint32_t pitch, uint32_t rows);
static void golden_buffers(struct fixture_case *test, const struct i915_video_mfx_decode *decode);
static void golden_bitstream(struct fixture_case *test, const struct i915_video_mfx_decode *decode);
static void golden_slices(struct fixture_case *test, const struct i915_video_mfx_decode *decode);
static void golden_dpb(struct fixture_case *test, const struct i915_video_mfx_decode *decode);
static void golden_picture(struct fixture_case *test, const struct i915_video_mfx_decode *decode);
static void golden_direct(struct fixture_case *test, const struct i915_video_mfx_decode *decode);
static void golden_qm4(struct fixture_case *test, unsigned kind, const uint8_t lists[3][16]);
static void golden_qm8(struct fixture_case *test, unsigned kind, const uint8_t *list);
static void fixture_decode_base(struct i915_video_mfx_decode *decode, const struct i915_video_sps *sps, const struct i915_video_pps *pps);
static void case_intra_cqm(void);
static void case_references(void);
static void case_flat_and_defaults(void);
static void case_set_bounds(void);
static void bound_check(const struct i915_video_sps *sps, const struct i915_video_pps *pps, const char *expected);

/*
 * Runs the cases into the directory.
 */
int
main(
	int argc,
	char **argv)
{
	/* The output directory. */
	if (argc != 2) {
		fprintf(stderr, "usage: host-mfx-avc DIRECTORY\n");
		return 2;
	}
	fixture_directory = argv[1];

	/* An IDR picture of High with a picture scaling matrix, two slices. */
	case_intra_cqm();

	/* A P picture of three references, a sequence and a picture matrix, no 8x8 transform. */
	case_references();

	/* Flat matrices without any matrix, and default lists asked for by the flag. */
	case_flat_and_defaults();

	/* The checks of the sets' values at each bound (design §6.6, items 2, 3 and 4; ws083 R-S6). */
	case_set_bounds();

	/* Succeeded: every case's batch and checks are written. */
	printf("host-mfx-avc: cases written to %s\n", fixture_directory);
	return 0;
}

/* Starts a case: an empty batch and its checks file. */
static void
fixture_begin(
	struct fixture_case *test,
	const char *name)
{
	char path[1024];

	/* The batch over the case's words. */
	memset(test, 0, sizeof(*test));
	test->name = name;
	test->batch.cmds = test->words;
	test->batch.capacity = FIXTURE_BATCH_DWORDS;

	/* The checks file. */
	snprintf(path, sizeof(path), "%s/%s.expect", fixture_directory, name);
	test->expect = fopen(path, "w");
	assert(test->expect != NULL);
}

/* Ends a case: the instruction count, the batch file. */
static void
fixture_end(
	struct fixture_case *test)
{
	char path[1024];
	FILE *file;
	size_t written;

	/* The whole batch fitted. */
	assert(test->batch.overflow == 0);
	assert(test->batch.count <= I915_VIDEO_MFX_MAX_DWORDS);

	/* The count of instructions the checks named. */
	fprintf(test->expect, "N\t%u\n", test->instructions);
	fclose(test->expect);

	/* The dwords, little-endian as the host is. */
	snprintf(path, sizeof(path), "%s/%s.bin", fixture_directory, test->name);
	file = fopen(path, "wb");
	assert(file != NULL);
	written = fwrite(test->words, 4U, test->batch.count, file);
	assert(written == test->batch.count);
	fclose(file);
}

/* Expects the next instruction of the batch to be the named one. */
static void
fixture_instruction(
	struct fixture_case *test,
	const char *name)
{
	/* Later fields belong to it. */
	fprintf(test->expect, "I\t%u\t%s\n", test->instructions, name);
	test->instructions++;
}

/* Expects a field of the instruction named last. */
static void
fixture_field(
	struct fixture_case *test,
	const char *field,
	long long value)
{
	/* The check line. */
	fprintf(test->expect, "F\t%u\t%s\t%lld\n", test->instructions - 1U, field, value);
}

/* Expects a field whose name carries an index. */
static void
fixture_fieldf(
	struct fixture_case *test,
	long long value,
	const char *format,
	unsigned index)
{
	char name[256];

	/* The name, then the check. */
	snprintf(name, sizeof(name), format, index);
	fixture_field(test, name, value);
}

/*
 * The raster position of a scan index in the frame zig-zag of a size by
 * size block, walked here: the anti-diagonals in turn, the even ones from
 * the bottom left up, the odd ones from the top right down.
 */
static unsigned
golden_scan_to_raster(
	unsigned size,
	unsigned scan)
{
	unsigned diagonal;
	unsigned seen;
	unsigned step;
	unsigned row;
	unsigned column;

	/* Counts through the diagonals until the scan index is reached. */
	seen = 0U;
	for (diagonal = 0U; diagonal < 2U * size - 1U; diagonal++) {
		for (step = 0U; step <= diagonal; step++) {
			/* The step-th position of the diagonal in its direction. */
			if ((diagonal % 2U) == 0U) {
				row = diagonal - step;
				column = step;
			} else {
				row = step;
				column = diagonal - step;
			}
			if (row >= size || column >= size)
				continue;
			if (seen == scan)
				return row * size + column;
			seen++;
		}
	}

	/* Never past the block. */
	assert(0);
	return 0U;
}

/* Expects a 4x4 quantizer matrix's bytes: the three lists in scan order, each placed in raster order. */
static void
golden_matrix4(
	struct fixture_case *test,
	const uint8_t lists[3][16])
{
	unsigned list;
	unsigned scan;

	/* Each list's coefficients at their raster places; the rest zero. */
	for (list = 0U; list < 3U; list++) {
		for (scan = 0U; scan < 16U; scan++)
			fixture_fieldf(test, lists[list][scan], "Forward Quantizer Matrix[%u]", list * 16U + golden_scan_to_raster(4U, scan));
	}
	for (scan = 48U; scan < 64U; scan++)
		fixture_fieldf(test, 0, "Forward Quantizer Matrix[%u]", scan);
}

/* Expects an 8x8 quantizer matrix's bytes: the list in scan order placed in raster order. */
static void
golden_matrix8(
	struct fixture_case *test,
	const uint8_t *list)
{
	unsigned scan;

	/* Each coefficient at its raster place. */
	for (scan = 0U; scan < 64U; scan++)
		fixture_fieldf(test, list[scan], "Forward Quantizer Matrix[%u]", golden_scan_to_raster(8U, scan));
}

/* Expects the flush, the power well, the waits, the mode and the surface. */
static void
golden_head(
	struct fixture_case *test,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t rows)
{
	/* MI_FLUSH_DW invalidating the video pipeline's caches, no post-sync write. */
	fixture_instruction(test, "MI_FLUSH_DW");
	fixture_field(test, "Video Pipeline Cache Invalidate", 1);
	fixture_field(test, "Post-Sync Operation", 0);

	/* The MFX power well, under its mask 768. */
	fixture_instruction(test, "MI_FORCE_WAKEUP");
	fixture_field(test, "MFX Power Well Control", 1);
	fixture_field(test, "HEVC Power Well Control", 0);
	fixture_field(test, "Mask Bits", 768);

	/* A wait, the mode (AVC, decode, VLD, short format, deblocked output), a wait. */
	fixture_instruction(test, "MFX_WAIT");
	fixture_field(test, "MFX Sync Control Flag", 1);
	fixture_instruction(test, "MFX_PIPE_MODE_SELECT");
	fixture_field(test, "Standard Select", 2);
	fixture_field(test, "Codec Select", 0);
	fixture_field(test, "Pre Deblocking Output Enable", 0);
	fixture_field(test, "Post Deblocking Output Enable", 1);
	fixture_field(test, "Decoder Mode Select", 0);
	fixture_field(test, "Decoder Short Format Mode", 0);
	fixture_field(test, "Stream-Out Enable", 0);
	fixture_instruction(test, "MFX_WAIT");
	fixture_field(test, "MFX Sync Control Flag", 1);

	/* The output surface: NV12 (PLANAR_420_8, chroma interleaved) in Y tiles. */
	fixture_instruction(test, "MFX_SURFACE_STATE");
	fixture_field(test, "Surface ID", 0);
	fixture_field(test, "Width", width - 1U);
	fixture_field(test, "Height", height - 1U);
	fixture_field(test, "Tile Walk", 1);
	fixture_field(test, "Tiled Surface", 1);
	fixture_field(test, "Half Pitch for Chroma", 0);
	fixture_field(test, "Surface Pitch", pitch - 1U);
	fixture_field(test, "Interleave Chroma", 1);
	fixture_field(test, "Surface Format", 4);
	fixture_field(test, "Y Offset for U(Cb)", rows);
	fixture_field(test, "X Offset for U(Cb)", 0);
	fixture_field(test, "Y Offset for V(Cr)", rows);
	fixture_field(test, "X Offset for V(Cr)", 0);
}

/* Expects the pipeline's buffers: the output, the row stores, the references (the output for an unused one). */
static void
golden_buffers(
	struct fixture_case *test,
	const struct i915_video_mfx_decode *decode)
{
	unsigned index;
	uint64_t expected;

	/* The deblocked output and the two row stores, each with the MOCS. */
	fixture_instruction(test, "MFX_PIPE_BUF_ADDR_STATE");
	fixture_field(test, "Pre Deblocking Destination - Address", 0);
	fixture_field(test, "Post Deblocking Destination - Address", (long long)FIXTURE_DESTINATION);
	fixture_field(test, "Post Deblocking Destination - Attributes.MOCS", FIXTURE_MOCS);
	fixture_field(test, "Intra Row Store Scratch Buffer - Address", (long long)FIXTURE_ROW_STORE);
	fixture_field(test, "Intra Row Store Scratch Buffer - Attributes.MOCS", FIXTURE_MOCS);
	fixture_field(test, "Deblocking Filter Row Store Scratch - Address", (long long)(FIXTURE_ROW_STORE + 0x1000U));
	fixture_field(test, "Deblocking Filter Row Store Scratch - Attributes.MOCS", FIXTURE_MOCS);
	fixture_field(test, "MB Status Buffer - Address", 0);
	fixture_field(test, "Scaled Reference Surface - Address", 0);
	fixture_field(test, "Reference Picture - Attributes.MOCS", FIXTURE_MOCS);

	/* The references by their place in the decode, the output's own picture after them (D21). */
	for (index = 0U; index < 16U; index++) {
		expected = FIXTURE_DESTINATION;
		if (index < decode->reference_count)
			expected = FIXTURE_REFERENCE + (uint64_t)index * 0x100000U;
		fixture_fieldf(test, (long long)expected, "Reference Picture - Address[%u]", index);
		fixture_fieldf(test, 0, "Reference Picture - Memory Compression Enable[%u]", index);
	}
}

/* Expects the bitstream object at the range's page, bounded by the buffer's end, and the decoder's row stores. */
static void
golden_bitstream(
	struct fixture_case *test,
	const struct i915_video_mfx_decode *decode)
{
	/* The bitstream object. */
	fixture_instruction(test, "MFX_IND_OBJ_BASE_ADDR_STATE");
	fixture_field(test, "MFX Indirect Bitstream Object - Address", (long long)decode->bitstream_base);
	fixture_field(test, "MFX Indirect Bitstream Object - Attributes.MOCS", FIXTURE_MOCS);
	fixture_field(test, "MFX Indirect Bitstream Object - Upper Bound", (long long)decode->bitstream_end);
	fixture_field(test, "MFX Indirect MV Object - Address", 0);
	fixture_field(test, "MFC Indirect PAK-BSE Object - Upper Bound", 0);

	/* The BSD/MPC and the MPR row stores (bindings 2 and 3). */
	fixture_instruction(test, "MFX_BSP_BUF_BASE_ADDR_STATE");
	fixture_field(test, "BSD/MPC Row Store Scratch Buffer - Address", (long long)(FIXTURE_ROW_STORE + 0x2000U));
	fixture_field(test, "BSD/MPC Row Store Scratch Buffer - Attributes.MOCS", FIXTURE_MOCS);
	fixture_field(test, "MPR Row Store Scratch Buffer - Address", (long long)(FIXTURE_ROW_STORE + 0x3000U));
	fixture_field(test, "MPR Row Store Scratch Buffer - Attributes.MOCS", FIXTURE_MOCS);
	fixture_field(test, "Bitplane Read Buffer - Address", 0);
}

/* Expects the slices: the next slice's address before each BSD object but the last's, from the page plus the skew. */
static void
golden_slices(
	struct fixture_case *test,
	const struct i915_video_mfx_decode *decode)
{
	unsigned slice;

	/* One after the other. */
	for (slice = 0U; slice < decode->slice_count; slice++) {
		/* The next slice's address. */
		if (slice + 1U < decode->slice_count) {
			fixture_instruction(test, "MFD_AVC_SLICEADDR");
			fixture_field(test, "Indirect BSD Data Length", decode->slice_ends[slice + 1U] - decode->slice_starts[slice + 1U]);
			fixture_field(test, "Indirect BSD Data Start Address", decode->skew + decode->slice_starts[slice + 1U]);
			fixture_field(test, "AVC NAL Type First Byte Override", 0);
		}

		/* The slice. */
		fixture_instruction(test, "MFD_AVC_BSD_OBJECT");
		fixture_field(test, "Indirect BSD Data Length", decode->slice_ends[slice] - decode->slice_starts[slice]);
		fixture_field(test, "Indirect BSD Data Start Address", decode->skew + decode->slice_starts[slice]);
		fixture_field(test, "Inline Data.Last Slice", slice + 1U == decode->slice_count);
		fixture_field(test, "Inline Data.Fix Prev MB Skipped", 1);
		fixture_field(test, "Inline Data.Intra Prediction Error Control", 1);
		fixture_field(test, "Inline Data.Intra 8x8/4x4 Prediction Error Concealment Control", 1);
		fixture_field(test, "Inline Data.I Slice Concealment Mode", 1);
		fixture_field(test, "Inline Data.Emulation Prevention Byte Present", 0);
	}

	/* The closing flush. */
	fixture_instruction(test, "MI_FLUSH_DW");
	fixture_field(test, "Video Pipeline Cache Invalidate", 0);
}

/* Fills the parts of a decode every case shares: the addresses and the 64x64 output. */
static void
fixture_decode_base(
	struct i915_video_mfx_decode *decode,
	const struct i915_video_sps *sps,
	const struct i915_video_pps *pps)
{
	unsigned index;

	/* The sets and the output: 64x64 NV12, pitch 128, the CbCr plane from row 64. */
	memset(decode, 0, sizeof(*decode));
	decode->sps = sps;
	decode->pps = pps;
	decode->destination = FIXTURE_DESTINATION;
	decode->width = 64U;
	decode->height = 64U;
	decode->pitch = 128U;
	decode->chroma_rows = 64U;

	/* The row stores a page apart, the written motion vectors, the MOCS. */
	for (index = 0U; index < I915_VIDEO_MFX_ROW_STORES; index++)
		decode->row_stores[index] = FIXTURE_ROW_STORE + (uint64_t)index * 0x1000U;
	decode->motion_write = FIXTURE_MOTION_WRITE;
	decode->mocs = FIXTURE_MOCS;

	/* The bitstream: the range starts 0x60 into its page, the bound range ends 0x2345 later. */
	decode->bitstream_base = FIXTURE_BITSTREAM;
	decode->skew = 0x60U;
	decode->bitstream_end = FIXTURE_BITSTREAM + 0x2345U;
}


/* Expects the DPB entries and the picture IDs of a decode's references. */
static void
golden_dpb(
	struct fixture_case *test,
	const struct i915_video_mfx_decode *decode)
{
	const struct i915_video_mfx_reference *reference;
	long long use;
	unsigned index;

	/* Each entry's marks, use and number; an unused one is all zero. */
	fixture_instruction(test, "MFD_AVC_DPB_STATE");
	for (index = 0U; index < 16U; index++) {
		/* An unused entry. */
		if (index >= decode->reference_count) {
			fixture_fieldf(test, 0, "Non-Existing Frame[%u]", index);
			fixture_fieldf(test, 0, "Long Term Frame[%u]", index);
			fixture_fieldf(test, 0, "Used for Reference[%u]", index);
			fixture_fieldf(test, 0, "LTST Frame Number List[%u]", index);
			continue;
		}

		/* A reference: its flags by name, a frame when it names no field. */
		reference = &decode->references[index];
		fixture_fieldf(test, (reference->flags >> 3) & 1U, "Non-Existing Frame[%u]", index);
		fixture_fieldf(test, (reference->flags >> 2) & 1U, "Long Term Frame[%u]", index);
		use = reference->flags & 3U;
		if (use == 0)
			use = 3;
		fixture_fieldf(test, use, "Used for Reference[%u]", index);
		fixture_fieldf(test, reference->frame_num, "LTST Frame Number List[%u]", index);
		fixture_fieldf(test, 0, "View ID[%u]", index);
	}

	/* The picture IDs: each reference's slot, 0xffff after them. */
	fixture_instruction(test, "MFD_AVC_PICID_STATE");
	fixture_field(test, "PictureID Remapping Disable", 0);
	for (index = 0U; index < 16U; index++) {
		/* A reference's slot, or the unused mark. */
		if (index < decode->reference_count)
			fixture_fieldf(test, decode->references[index].slot_index, "Picture ID[%u]", index);
		else
			fixture_fieldf(test, 0xffff, "Picture ID[%u]", index);
	}
}

/* Expects the picture state from the sets and the picture, field by field. */
static void
golden_picture(
	struct fixture_case *test,
	const struct i915_video_mfx_decode *decode)
{
	const struct i915_video_sps *sps;
	const struct i915_video_pps *pps;
	long long width;
	long long height;

	/* The sets. */
	sps = decode->sps;
	pps = decode->pps;
	width = sps->pic_width_in_mbs_minus1 + 1U;
	height = sps->pic_height_in_map_units_minus1 + 1U;

	/* The extent and size, a frame picture. */
	fixture_instruction(test, "MFX_AVC_IMG_STATE");
	fixture_field(test, "Frame Size", width * height);
	fixture_field(test, "Frame Width", width - 1);
	fixture_field(test, "Frame Height", height - 1);
	fixture_field(test, "Image Structure", 0);
	fixture_field(test, "Field Picture", 0);
	fixture_field(test, "MBAFF Mode", 0);

	/* The picture set's fields. */
	fixture_field(test, "Weighted BiPrediction IDC", pps->weighted_bipred_idc);
	fixture_field(test, "Weighted Prediction Enable", (pps->flags >> 4) & 1U);
	fixture_field(test, "First Chroma QP Offset", pps->chroma_qp_index_offset);
	fixture_field(test, "Second Chroma QP Offset", pps->second_chroma_qp_index_offset);
	fixture_field(test, "8x8 IDCT Transform Mode", pps->flags & 1U);
	fixture_field(test, "Constrained Intra Prediction", (pps->flags >> 2) & 1U);
	fixture_field(test, "Entropy Coding Sync Enable", (pps->flags >> 6) & 1U);
	fixture_field(test, "Initial QP Value", pps->pic_init_qp_minus26);
	fixture_field(test, "Number of Active Reference Pictures from L0", pps->num_ref_idx_l0_default_active_minus1 + 1U);
	fixture_field(test, "Number of Active Reference Pictures from L1", pps->num_ref_idx_l1_default_active_minus1 + 1U);
	fixture_field(test, "Pic Order Present", (pps->flags >> 5) & 1U);
	fixture_field(test, "Redundant Pic Count Present", (pps->flags >> 1) & 1U);
	fixture_field(test, "Deblocking Filter Control Present", (pps->flags >> 3) & 1U);
	fixture_field(test, "Number of Slice Groups", 0);

	/* The sequence's fields. */
	fixture_field(test, "Frame MB Only", (sps->flags >> 8) & 1U);
	fixture_field(test, "Direct 8x8 Inference", (sps->flags >> 6) & 1U);
	fixture_field(test, "Chroma Format IDC", sps->chroma_format_idc);
	fixture_field(test, "Delta Pic Order Always Zero", (sps->flags >> 9) & 1U);
	fixture_field(test, "Pic Order Count Type", sps->pic_order_cnt_type);
	fixture_field(test, "Log2 Max Frame Number", sps->log2_max_frame_num_minus4);
	fixture_field(test, "Log2 Max Pic Order Count LSB", sps->log2_max_pic_order_cnt_lsb_minus4);

	/* The picture's own: not a reference, its frame_num, its references. */
	fixture_field(test, "Non-Reference Picture", ((decode->picture_flags >> 4) & 1U) == 0U);
	fixture_field(test, "Current Picture Frame Number", decode->frame_num);
	fixture_field(test, "Number of Reference Frames", decode->reference_count);
	fixture_field(test, "Trellis Quantization Chroma Disable", 1);
	fixture_field(test, "Trellis Quantization Enable", 0);
	fixture_field(test, "MV Unpacked Enable", 0);
}

/* Expects the direct mode state: the references' motion vectors (the written buffer for an unused one), the order counts. */
static void
golden_direct(
	struct fixture_case *test,
	const struct i915_video_mfx_decode *decode)
{
	unsigned index;

	/* The buffers. */
	fixture_instruction(test, "MFX_AVC_DIRECTMODE_STATE");
	for (index = 0U; index < 16U; index++) {
		/* A reference's slot buffer, or the one this picture writes (D21). */
		if (index < decode->reference_count)
			fixture_fieldf(test, (long long)(FIXTURE_MOTION + (uint64_t)index * 0x10000U), "Direct MV Buffer - Address[%u]", index);
		else
			fixture_fieldf(test, (long long)FIXTURE_MOTION_WRITE, "Direct MV Buffer - Address[%u]", index);
	}
	fixture_field(test, "Direct MV Buffer - Attributes.MOCS", FIXTURE_MOCS);
	fixture_field(test, "Direct MV Buffer (Write) - Address", (long long)FIXTURE_MOTION_WRITE);
	fixture_field(test, "Direct MV Buffer (Write) - Attributes.MOCS", FIXTURE_MOCS);

	/* The order counts as 32-bit words: the references', then this picture's at 32 and 33. */
	for (index = 0U; index < 16U; index++) {
		/* A reference's, or zero. */
		if (index < decode->reference_count) {
			fixture_fieldf(test, (uint32_t)decode->references[index].poc[0], "POC List[%u]", 2U * index);
			fixture_fieldf(test, (uint32_t)decode->references[index].poc[1], "POC List[%u]", 2U * index + 1U);
		} else {
			fixture_fieldf(test, 0, "POC List[%u]", 2U * index);
			fixture_fieldf(test, 0, "POC List[%u]", 2U * index + 1U);
		}
	}
	fixture_field(test, "POC List[32]", (uint32_t)decode->poc[0]);
	fixture_field(test, "POC List[33]", (uint32_t)decode->poc[1]);
}

/* Expects one MFX_QM_STATE of a 4x4 kind and its three lists. */
static void
golden_qm4(
	struct fixture_case *test,
	unsigned kind,
	const uint8_t lists[3][16])
{
	/* The kind, then the bytes. */
	fixture_instruction(test, "MFX_QM_STATE");
	fixture_field(test, "AVC", kind);
	golden_matrix4(test, lists);
}

/* Expects one MFX_QM_STATE of an 8x8 kind and its list. */
static void
golden_qm8(
	struct fixture_case *test,
	unsigned kind,
	const uint8_t *list)
{
	/* The kind, then the bytes. */
	fixture_instruction(test, "MFX_QM_STATE");
	fixture_field(test, "AVC", kind);
	golden_matrix8(test, list);
}

/*
 * An IDR picture of High, 64x64: CABAC, the 8x8 transform, chroma QP
 * offsets of -3 and 5, an initial QP of -4, two slices.  The picture set
 * sends a matrix whose lists 2 and 5 are missing (rule A: each is the list
 * before it), each sent coefficient its own value; the sequence has none.
 */
static void
case_intra_cqm(void)
{
	static const uint32_t starts[2] = { 4U, 0x401U };
	static const uint32_t ends[2] = { 0x3fdU, 0x900U };
	static struct fixture_case test;
	struct i915_video_sps sps;
	struct i915_video_pps pps;
	struct i915_video_mfx_decode decode;
	uint8_t intra[3][16];
	uint8_t inter[3][16];
	unsigned index;

	/* The sequence: 4:2:0, frames only, direct 8x8 inference, 4x4 macroblocks, POC type 2. */
	memset(&sps, 0, sizeof(sps));
	sps.flags = I915_VIDEO_SPS_FRAME_MBS_ONLY | I915_VIDEO_SPS_DIRECT_8X8_INFERENCE;
	sps.profile_idc = 100U;
	sps.chroma_format_idc = 1U;
	sps.log2_max_frame_num_minus4 = 2U;
	sps.pic_order_cnt_type = 2U;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 3U;
	sps.pic_width_in_mbs_minus1 = 3U;
	sps.pic_height_in_map_units_minus1 = 3U;

	/* The picture set: lists 0, 1, 3, 4, 6 and 7 sent, each coefficient its own value. */
	memset(&pps, 0, sizeof(pps));
	pps.flags = I915_VIDEO_PPS_TRANSFORM_8X8 | I915_VIDEO_PPS_CABAC | I915_VIDEO_PPS_SCALING_MATRIX | I915_VIDEO_PPS_DEBLOCKING_CONTROL;
	pps.pic_init_qp_minus26 = -4;
	pps.chroma_qp_index_offset = -3;
	pps.second_chroma_qp_index_offset = 5;
	pps.has_scaling = 1;
	pps.scaling.present_mask = 0xdbU;
	for (index = 0U; index < 96U; index++)
		pps.scaling.list4[index] = (uint8_t)(20U + index);
	for (index = 0U; index < 384U; index++)
		pps.scaling.list8[index] = (uint8_t)(100U + (index % 128U));

	/* The decode: a reference IDR picture, frame_num 0, order counts 0, two slices. */
	fixture_decode_base(&decode, &sps, &pps);
	decode.picture_flags = I915_VIDEO_PICTURE_IS_REFERENCE | (1U << 1) | (1U << 2);
	decode.slice_count = 2U;
	decode.slice_starts = starts;
	decode.slice_ends = ends;

	/* Builds it. */
	fixture_begin(&test, "intra-cqm");
	drv_i915_video_mfx_build(&test.batch, &decode);

	/* The lists the picture decodes with: 2 is 1's, 5 is 4's. */
	for (index = 0U; index < 16U; index++) {
		intra[0][index] = (uint8_t)(20U + index);
		intra[1][index] = (uint8_t)(36U + index);
		intra[2][index] = (uint8_t)(36U + index);
		inter[0][index] = (uint8_t)(68U + index);
		inter[1][index] = (uint8_t)(84U + index);
		inter[2][index] = (uint8_t)(84U + index);
	}

	/* The expected commands. */
	golden_head(&test, 64U, 64U, 128U, 64U);
	golden_buffers(&test, &decode);
	golden_bitstream(&test, &decode);
	golden_dpb(&test, &decode);
	golden_picture(&test, &decode);
	golden_qm4(&test, 0U, (const uint8_t (*)[16])intra);
	golden_qm4(&test, 1U, (const uint8_t (*)[16])inter);
	golden_qm8(&test, 2U, &pps.scaling.list8[0]);
	golden_qm8(&test, 3U, &pps.scaling.list8[64]);
	golden_direct(&test, &decode);
	golden_slices(&test, &decode);
	fixture_end(&test);
}

/*
 * A P picture that is not a reference, 1920x1088, with three references
 * in slots 5, 0 and 16: a short-term frame, a long-term top field and a
 * non-existing bottom field, negative order counts among them.  The
 * sequence's matrix sends lists 0 and 4 (rule A for the others); the
 * picture's sends list 1 (rule B: lists 0 and 3 are the sequence's).  No
 * 8x8 transform: two quantizer matrices only.  One slice.
 */
static void
case_references(void)
{
	static const uint32_t starts[1] = { 3U };
	static const uint32_t ends[1] = { 0x1234U };
	static struct fixture_case test;
	struct i915_video_sps sps;
	struct i915_video_pps pps;
	struct i915_video_mfx_decode decode;
	uint8_t intra[3][16];
	uint8_t inter[3][16];
	unsigned index;

	/* The sequence: frames only, POC type 0 with delta always zero, a matrix of lists 0 and 4. */
	memset(&sps, 0, sizeof(sps));
	sps.flags = I915_VIDEO_SPS_FRAME_MBS_ONLY | I915_VIDEO_SPS_DELTA_ALWAYS_ZERO | I915_VIDEO_SPS_SCALING_MATRIX;
	sps.profile_idc = 100U;
	sps.chroma_format_idc = 1U;
	sps.log2_max_frame_num_minus4 = 4U;
	sps.pic_order_cnt_type = 0U;
	sps.log2_max_pic_order_cnt_lsb_minus4 = 5U;
	sps.pic_width_in_mbs_minus1 = 119U;
	sps.pic_height_in_map_units_minus1 = 67U;
	sps.has_scaling = 1;
	sps.scaling.present_mask = 0x11U;
	for (index = 0U; index < 96U; index++)
		sps.scaling.list4[index] = (uint8_t)(150U + index);

	/* The picture set: CAVLC, weighted prediction, list 1 of a matrix, three and two default references. */
	memset(&pps, 0, sizeof(pps));
	pps.flags = I915_VIDEO_PPS_SCALING_MATRIX | I915_VIDEO_PPS_WEIGHTED_PRED | I915_VIDEO_PPS_CONSTRAINED_INTRA |
	    I915_VIDEO_PPS_REDUNDANT_PIC_COUNT | I915_VIDEO_PPS_BOTTOM_FIELD_POC;
	pps.weighted_bipred_idc = 2U;
	pps.num_ref_idx_l0_default_active_minus1 = 2U;
	pps.num_ref_idx_l1_default_active_minus1 = 1U;
	pps.pic_init_qp_minus26 = 7;
	pps.chroma_qp_index_offset = 12;
	pps.second_chroma_qp_index_offset = -12;
	pps.has_scaling = 1;
	pps.scaling.present_mask = 0x02U;
	for (index = 0U; index < 96U; index++)
		pps.scaling.list4[index] = (uint8_t)(1U + index);

	/* The decode: the 1920x1088 output, frame_num 9, order counts 20 and 21. */
	fixture_decode_base(&decode, &sps, &pps);
	decode.width = 1920U;
	decode.height = 1088U;
	decode.pitch = 1920U;
	decode.chroma_rows = 1088U;
	decode.frame_num = 9U;
	decode.poc[0] = 20;
	decode.poc[1] = 21;
	decode.slice_count = 1U;
	decode.slice_starts = starts;
	decode.slice_ends = ends;

	/* The three references, each a megabyte apart, their motion vectors 64 KiB apart. */
	decode.reference_count = 3U;
	for (index = 0U; index < 3U; index++) {
		decode.references[index].picture = FIXTURE_REFERENCE + (uint64_t)index * 0x100000U;
		decode.references[index].motion = FIXTURE_MOTION + (uint64_t)index * 0x10000U;
	}
	decode.references[0].slot_index = 5;
	decode.references[0].frame_num = 7U;
	decode.references[0].poc[0] = 10;
	decode.references[0].poc[1] = 11;
	decode.references[1].slot_index = 0;
	decode.references[1].flags = I915_VIDEO_REFERENCE_TOP_FIELD | I915_VIDEO_REFERENCE_LONG_TERM;
	decode.references[1].frame_num = 2U;
	decode.references[1].poc[0] = -4;
	decode.references[1].poc[1] = 3;
	decode.references[2].slot_index = 16;
	decode.references[2].flags = I915_VIDEO_REFERENCE_BOTTOM_FIELD | I915_VIDEO_REFERENCE_NON_EXISTING;
	decode.references[2].frame_num = 300U;
	decode.references[2].poc[0] = -100;
	decode.references[2].poc[1] = -99;

	/* Builds it. */
	fixture_begin(&test, "references");
	drv_i915_video_mfx_build(&test.batch, &decode);

	/* The lists: intra 0 the sequence's, 1 and 2 the picture's list 1; inter all Default_4x4_Inter. */
	for (index = 0U; index < 16U; index++) {
		intra[0][index] = (uint8_t)(150U + index);
		intra[1][index] = (uint8_t)(17U + index);
		intra[2][index] = (uint8_t)(17U + index);
		inter[0][index] = golden_default4[1][index];
		inter[1][index] = golden_default4[1][index];
		inter[2][index] = golden_default4[1][index];
	}

	/* The expected commands. */
	golden_head(&test, 1920U, 1088U, 1920U, 1088U);
	golden_buffers(&test, &decode);
	golden_bitstream(&test, &decode);
	golden_dpb(&test, &decode);
	golden_picture(&test, &decode);
	golden_qm4(&test, 0U, (const uint8_t (*)[16])intra);
	golden_qm4(&test, 1U, (const uint8_t (*)[16])inter);
	golden_direct(&test, &decode);
	golden_slices(&test, &decode);
	fixture_end(&test);
}

/*
 * Two pictures of the 8x8 transform: one without any matrix (every list
 * Flat_16), and one whose sequence matrix sends lists 0 and 6 asking for
 * the default (useDefaultScalingMatrixFlag) and nothing else, so every
 * list is a default one.
 */
static void
case_flat_and_defaults(void)
{
	static const uint32_t starts[1] = { 4U };
	static const uint32_t ends[1] = { 100U };
	static struct fixture_case test;
	struct i915_video_sps sps;
	struct i915_video_pps pps;
	struct i915_video_mfx_decode decode;
	uint8_t flat4[3][16];
	uint8_t flat8[64];
	uint8_t intra[3][16];
	uint8_t inter[3][16];
	unsigned list;
	unsigned index;

	/* The sets: no matrix, the 8x8 transform. */
	memset(&sps, 0, sizeof(sps));
	sps.flags = I915_VIDEO_SPS_FRAME_MBS_ONLY;
	sps.chroma_format_idc = 1U;
	sps.pic_width_in_mbs_minus1 = 3U;
	sps.pic_height_in_map_units_minus1 = 3U;
	memset(&pps, 0, sizeof(pps));
	pps.flags = I915_VIDEO_PPS_TRANSFORM_8X8;
	fixture_decode_base(&decode, &sps, &pps);
	decode.picture_flags = I915_VIDEO_PICTURE_IS_REFERENCE;
	decode.slice_count = 1U;
	decode.slice_starts = starts;
	decode.slice_ends = ends;

	/* Builds it and expects Flat_16 everywhere. */
	fixture_begin(&test, "flat");
	drv_i915_video_mfx_build(&test.batch, &decode);
	for (list = 0U; list < 3U; list++) {
		for (index = 0U; index < 16U; index++)
			flat4[list][index] = 16U;
	}
	for (index = 0U; index < 64U; index++)
		flat8[index] = 16U;
	golden_head(&test, 64U, 64U, 128U, 64U);
	golden_buffers(&test, &decode);
	golden_bitstream(&test, &decode);
	golden_dpb(&test, &decode);
	golden_picture(&test, &decode);
	golden_qm4(&test, 0U, (const uint8_t (*)[16])flat4);
	golden_qm4(&test, 1U, (const uint8_t (*)[16])flat4);
	golden_qm8(&test, 2U, flat8);
	golden_qm8(&test, 3U, flat8);
	golden_direct(&test, &decode);
	golden_slices(&test, &decode);
	fixture_end(&test);

	/* The sequence's matrix: lists 0 and 6 sent asking for the default, their values not the default. */
	sps.flags |= I915_VIDEO_SPS_SCALING_MATRIX;
	sps.has_scaling = 1;
	sps.scaling.present_mask = 0x41U;
	sps.scaling.default_mask = 0x41U;
	memset(sps.scaling.list4, 99, sizeof(sps.scaling.list4));
	memset(sps.scaling.list8, 99, sizeof(sps.scaling.list8));

	/* Builds it and expects the default lists everywhere. */
	fixture_begin(&test, "defaults");
	drv_i915_video_mfx_build(&test.batch, &decode);
	for (list = 0U; list < 3U; list++) {
		for (index = 0U; index < 16U; index++) {
			intra[list][index] = golden_default4[0][index];
			inter[list][index] = golden_default4[1][index];
		}
	}
	golden_head(&test, 64U, 64U, 128U, 64U);
	golden_buffers(&test, &decode);
	golden_bitstream(&test, &decode);
	golden_dpb(&test, &decode);
	golden_picture(&test, &decode);
	golden_qm4(&test, 0U, (const uint8_t (*)[16])intra);
	golden_qm4(&test, 1U, (const uint8_t (*)[16])inter);
	golden_qm8(&test, 2U, golden_default8[0]);
	golden_qm8(&test, 3U, golden_default8[1]);
	golden_direct(&test, &decode);
	golden_slices(&test, &decode);
	fixture_end(&test);
}

/* Requires the set check's answer: NULL (taken), or a reason containing the expected words. */
static void
bound_check(
	const struct i915_video_sps *sps,
	const struct i915_video_pps *pps,
	const char *expected)
{
	const char *reason;

	/* The answer. */
	reason = drv_i915_video_mfx_check_sets(sps, pps);
	if (expected == NULL) {
		if (reason != NULL) {
			fprintf(stderr, "host-mfx-avc: sets refused: %s\n", reason);
			abort();
		}
		return;
	}

	/* A refusal for the expected reason. */
	if (reason == NULL || strstr(reason, expected) == NULL) {
		fprintf(stderr, "host-mfx-avc: expected \"%s\", got \"%s\"\n", expected, reason == NULL ? "(taken)" : reason);
		abort();
	}
}

/*
 * The values the decoder takes, each at its last taken value and its first
 * refused one: chroma format, bit depths, frames only, the order count
 * type, the frame number and order count widths, the picture's sides and
 * macroblocks, the reference index counts, weighted bi-prediction, the
 * initial quantizer and the chroma quantizer offsets.
 */
static void
case_set_bounds(void)
{
	struct i915_video_sps sps;
	struct i915_video_pps pps;
	struct i915_video_sps edge;
	struct i915_video_pps pedge;

	/* A taken sequence and picture set. */
	memset(&sps, 0, sizeof(sps));
	sps.flags = I915_VIDEO_SPS_FRAME_MBS_ONLY;
	sps.chroma_format_idc = 1U;
	sps.pic_width_in_mbs_minus1 = 3U;
	sps.pic_height_in_map_units_minus1 = 3U;
	memset(&pps, 0, sizeof(pps));
	bound_check(&sps, &pps, NULL);

	/* Chroma format, bit depths, frames only. */
	edge = sps;
	edge.chroma_format_idc = 2U;
	bound_check(&edge, &pps, "4:2:0");
	edge = sps;
	edge.bit_depth_luma_minus8 = 1U;
	bound_check(&edge, &pps, "bit depth");
	edge = sps;
	edge.bit_depth_chroma_minus8 = 1U;
	bound_check(&edge, &pps, "bit depth");
	edge = sps;
	edge.flags = 0U;
	bound_check(&edge, &pps, "frames only");

	/* Order count type 2 taken, 3 refused; widths 12 taken, 13 refused. */
	edge = sps;
	edge.pic_order_cnt_type = 2U;
	bound_check(&edge, &pps, NULL);
	edge.pic_order_cnt_type = 3U;
	bound_check(&edge, &pps, "order count type");
	edge = sps;
	edge.log2_max_frame_num_minus4 = 12U;
	edge.log2_max_pic_order_cnt_lsb_minus4 = 12U;
	bound_check(&edge, &pps, NULL);
	edge.log2_max_frame_num_minus4 = 13U;
	bound_check(&edge, &pps, "16 bits");
	edge.log2_max_frame_num_minus4 = 12U;
	edge.log2_max_pic_order_cnt_lsb_minus4 = 13U;
	bound_check(&edge, &pps, "16 bits");

	/* Sides: 256 macroblocks taken, 257 refused; 36864 macroblocks taken, one row more refused. */
	edge = sps;
	edge.pic_width_in_mbs_minus1 = 255U;
	edge.pic_height_in_map_units_minus1 = 143U;
	bound_check(&edge, &pps, NULL);
	edge.pic_width_in_mbs_minus1 = 256U;
	bound_check(&edge, &pps, "4096");
	edge.pic_width_in_mbs_minus1 = 255U;
	edge.pic_height_in_map_units_minus1 = 256U;
	bound_check(&edge, &pps, "4096");
	edge.pic_height_in_map_units_minus1 = 144U;
	bound_check(&edge, &pps, "36864");

	/* Reference index counts 32 taken, 33 refused; weighted bi-prediction 2 taken, 3 refused. */
	pedge = pps;
	pedge.num_ref_idx_l0_default_active_minus1 = 31U;
	pedge.num_ref_idx_l1_default_active_minus1 = 31U;
	pedge.weighted_bipred_idc = 2U;
	bound_check(&sps, &pedge, NULL);
	pedge.num_ref_idx_l0_default_active_minus1 = 32U;
	bound_check(&sps, &pedge, "reference index");
	pedge.num_ref_idx_l0_default_active_minus1 = 31U;
	pedge.num_ref_idx_l1_default_active_minus1 = 32U;
	bound_check(&sps, &pedge, "reference index");
	pedge.num_ref_idx_l1_default_active_minus1 = 31U;
	pedge.weighted_bipred_idc = 3U;
	bound_check(&sps, &pedge, "bi-prediction");

	/* The initial quantizer -26 and 25 taken, -27 and 26 refused. */
	pedge = pps;
	pedge.pic_init_qp_minus26 = -26;
	bound_check(&sps, &pedge, NULL);
	pedge.pic_init_qp_minus26 = 25;
	bound_check(&sps, &pedge, NULL);
	pedge.pic_init_qp_minus26 = -27;
	bound_check(&sps, &pedge, "initial quantizer");
	pedge.pic_init_qp_minus26 = 26;
	bound_check(&sps, &pedge, "initial quantizer");

	/* The chroma offsets -12 and 12 taken, -13 and 13 refused, each of the two. */
	pedge = pps;
	pedge.chroma_qp_index_offset = -12;
	pedge.second_chroma_qp_index_offset = 12;
	bound_check(&sps, &pedge, NULL);
	pedge.chroma_qp_index_offset = -13;
	bound_check(&sps, &pedge, "chroma quantizer");
	pedge.chroma_qp_index_offset = 12;
	pedge.second_chroma_qp_index_offset = 13;
	bound_check(&sps, &pedge, "chroma quantizer");
	pedge.second_chroma_qp_index_offset = -13;
	bound_check(&sps, &pedge, "chroma quantizer");

	/* Every bound held. */
	printf("host-mfx-avc: set bounds checked\n");
}
