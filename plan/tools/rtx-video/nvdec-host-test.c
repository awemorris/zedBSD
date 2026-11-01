/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the nvrtx video decoder's parts that touch no hardware:
 * the H.264 picture structure, the slice table, the decode push buffer,
 * the work buffers' sizes and the NV12 layout, the scaling matrices and
 * their fall-backs, the DPB entries, the copy engine's plane copy, and
 * the resource manager's parameters and the device table's pick.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-nvdec.h"
#include "drivers/gpu/nvrtx/nvrtx-work.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-video.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-exec.h"

static void check(int ok, const char *what);
static uint32_t word(const uint8_t *bytes, size_t at);
static uint32_t bits(const uint8_t *bytes, size_t at, uint32_t low, uint32_t count);
static void test_pic(void);
static void test_slices(void);
static void test_push(void);
static void test_sizes(void);
static void test_scaling(void);
static void test_dpb(void);
static void test_copy(void);
static void test_rm(void);

/* The failed checks so far. */
static int failures;

/* The checks run so far. */
static int checks;

/* Runs every test and reports the count. */
int
main(void)
{
	/* Runs every group of checks. */
	test_pic();
	test_slices();
	test_push();
	test_sizes();
	test_scaling();
	test_dpb();
	test_copy();
	test_rm();

	/* Tells the caller how many checks ran and how many failed. */
	printf("nvdec host test: %d checks, %d failed\n", checks, failures);

	/* Fails the run when any check failed. */
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Counts one check and reports it when it fails. */
static void
check(
	int ok,
	const char *what)
{
	/* Counts the check, so a run that skipped checks shows in the total. */
	checks++;

	/* A passing check needs nothing more. */
	if (ok)
		return;

	/* Counts the failure and names the check that failed. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Reads a little-endian word. */
static uint32_t
word(
	const uint8_t *bytes,
	size_t at)
{
	uint32_t value;

	/* Reads the word through the driver's own accessor. */
	value = nvrtx_get32(bytes, at);

	/* Reports the word. */
	return value;
}

/* Reads a bit field of a little-endian word. */
static uint32_t
bits(
	const uint8_t *bytes,
	size_t at,
	uint32_t low,
	uint32_t count)
{
	uint32_t value;

	/* Extracts the bit field from the word. */
	value = word(bytes, at) >> low;

	/* Masks the field to its width, unless it takes the whole word. */
	if (count < 32U)
		value &= (1U << count) - 1U;

	/* Reports the field. */
	return value;
}

/* The picture structure's fields land at the places the published structure has them. */
static void
test_pic(void)
{
	static const uint8_t eos[16] = { 0, 0, 1, 0x0b, 0, 0, 0, 0, 0, 0, 1, 0x0b, 0, 0, 0, 0 };
	struct nvrtx_nvdec_h264_picture picture;
	uint8_t pic[NVRTX_NVDEC_PIC_BYTES];
	uint32_t index;

	/* Describes a picture and encodes it into the hardware's picture structure. */
	memset(&picture, 0, sizeof(picture));
	picture.log2_max_poc_lsb_minus4 = 2U;
	picture.delta_poc_always_zero = 1U;
	picture.frame_mbs_only = 1U;
	picture.width_mbs = 120U;
	picture.height_mbs = 68U;
	picture.direct_8x8_inference = 1U;
	picture.log2_max_frame_num_minus4 = 5U;
	picture.chroma_format_idc = 1U;
	picture.poc_type = 2U;
	picture.transform_bypass = 1U;
	picture.entropy_coding = 1U;
	picture.ref_l0_minus1 = 3U;
	picture.ref_l1_minus1 = 1U;
	picture.transform_8x8 = 1U;
	picture.weighted_bipred_idc = 2U;
	picture.pic_init_qp_minus26 = -3;
	picture.chroma_qp_offset = -2;
	picture.second_chroma_qp_offset = 5;
	picture.is_reference = 1U;
	picture.frame_num = 0x1234U;
	picture.field_order[0] = 10;
	picture.field_order[1] = 11;
	picture.surface = 9U;
	picture.reference_count = 2U;
	picture.references[0].surface = 3U;
	picture.references[0].field_order[0] = 4;
	picture.references[0].field_order[1] = 5;
	picture.references[0].frame_index = 7U;
	picture.dpb_entry[0] = 0U;
	picture.references[1].surface = 16U;
	picture.references[1].long_term = 1U;
	picture.references[1].non_existing = 1U;
	picture.references[1].frame_index = 2U;
	picture.dpb_entry[1] = 5U;

	/* Gives the 4x4 and the 8x8 scaling lists distinguishable values. */
	for (index = 0U; index < 16U; index++)
		picture.scaling_4x4[2][index] = (uint8_t)(index + 1U);
	for (index = 0U; index < 64U; index++)
		picture.scaling_8x8[1][index] = (uint8_t)(index + 100U);
	picture.stream_bytes = 5000U;
	picture.slice_count = 4U;
	picture.mbhist_bytes = 12544U;
	picture.history_bytes = 92160U;
	picture.pitch = 1920U;
	memset(pic, 0xa5, sizeof(pic));
	nvrtx_nvdec_h264_pic(pic, &picture);

	/* Checks the fields of the encoded picture structure against the hardware layout. */
	check(memcmp(pic + 52, eos, sizeof(eos)) == 0, "pic: end of stream at 52");
	check(pic[68] == 1U, "pic: explicit end of stream at 68");
	check(word(pic, 72) == 5016U, "pic: stream_len is the range and the end of stream");
	check(word(pic, 76) == 4U, "pic: slice_count at 76");
	check(word(pic, 80) == 12544U, "pic: mbhist size at 80");
	check(word(pic, 84) == 81000000U, "pic: timeout at 84");
	check(word(pic, 88) == 2U && word(pic, 92) == 1U && word(pic, 96) == 1U, "pic: sequence fields 88 to 96");
	check(word(pic, 100) == 120U && word(pic, 104) == 68U, "pic: macroblocks at 100 and 104");
	check(bits(pic, 108, 0U, 2U) == 1U && bits(pic, 108, 2U, 3U) == 0U, "pic: KBL tiling, GOB 2");
	check(word(pic, 112) == 1U && word(pic, 120) == 3U && word(pic, 124) == 1U && word(pic, 136) == 1U, "pic: picture set fields");
	check(word(pic, 140) == 1920U && word(pic, 144) == 1920U, "pic: pitches");
	check(word(pic, 152) == 120U * 16U && word(pic, 164) == 960U, "pic: bottom field offsets");
	check(word(pic, 172) == 92160U / 256U, "pic: history in 256-byte units");
	check(bits(pic, 176, 0U, 1U) == 0U && bits(pic, 176, 1U, 1U) == 1U && bits(pic, 176, 4U, 1U) == 1U, "pic: flags 0, 1 and 4");
	check(bits(pic, 176, 8U, 4U) == 5U && bits(pic, 176, 12U, 2U) == 1U && bits(pic, 176, 14U, 2U) == 2U, "pic: frame num, chroma, poc type");
	check(bits(pic, 176, 16U, 6U) == 0x3dU && bits(pic, 176, 22U, 5U) == 0x1eU && bits(pic, 176, 27U, 5U) == 5U, "pic: signed quantizers");
	check(bits(pic, 180, 0U, 2U) == 2U && bits(pic, 180, 2U, 7U) == 9U && bits(pic, 180, 9U, 5U) == 9U, "pic: bipred and the current surface");
	check(bits(pic, 180, 14U, 16U) == 0x1234U && bits(pic, 180, 30U, 2U) == 0U, "pic: frame_num, NV12");
	check(word(pic, 184) == 10U && word(pic, 188) == 11U, "pic: order counts at 184");
	check(bits(pic, 192, 0U, 7U) == 3U && bits(pic, 192, 7U, 5U) == 3U && bits(pic, 192, 12U, 2U) == 3U, "pic: dpb 0 surface and state");
	check(bits(pic, 192, 14U, 1U) == 0U && bits(pic, 192, 17U, 4U) == 1U && bits(pic, 192, 21U, 4U) == 1U, "pic: dpb 0 short-term markings");
	check(word(pic, 196) == 4U && word(pic, 200) == 5U && word(pic, 204) == 7U, "pic: dpb 0 order counts and frame");
	check(bits(pic, 192 + 5 * 16, 0U, 7U) == 16U && bits(pic, 192 + 5 * 16, 14U, 2U) == 3U, "pic: dpb 5 long term, non-existing");
	check(bits(pic, 192 + 5 * 16, 17U, 4U) == 2U && bits(pic, 192 + 5 * 16, 21U, 4U) == 2U, "pic: dpb 5 long-term markings");
	check(word(pic, 192 + 16) == 0U, "pic: unused dpb entry is zero");
	check(pic[448 + 2 * 16 + 5] == 6U && pic[544 + 64 + 63] == 163U, "pic: scaling matrices in place");
	check(bits(pic, 720, 1U, 1U) == 1U, "pic: transform bypass at 720 bit 1");
	check(word(pic, 724) == 0U && pic[763] == 0U, "pic: display and tail zero");
}

/* The slice table: the starts, then the end; more than the table is cut. */
static void
test_slices(void)
{
	uint32_t table[NVRTX_NVDEC_SLICE_WORDS];
	uint32_t offsets[300];
	uint32_t index;

	/* Packs three slices into the table. */
	offsets[0] = 0U;
	offsets[1] = 100U;
	offsets[2] = 250U;
	nvrtx_nvdec_slices(table, offsets, 3U, 400U);
	check(table[0] == 0U && table[1] == 100U && table[2] == 250U && table[3] == 400U && table[4] == 0U, "slices: three and the end");

	/* Offers more slices than the table holds. */
	for (index = 0U; index < 300U; index++)
		offsets[index] = index;
	nvrtx_nvdec_slices(table, offsets, 300U, 9999U);
	check(table[254] == 254U && table[255] == 9999U, "slices: 255 at most, the end in the last word");
}

/* The decode push buffer: the methods in order on subchannel 4, addresses shifted by 8. */
static void
test_push(void)
{
	struct nvrtx_nvdec_frame frame;
	uint8_t push[NVRTX_NVDEC_DECODE_PUSH_BYTES + 64U];
	uint8_t object[16];
	size_t length;
	uint32_t index;

	/* Describes one decode frame and its surfaces. */
	memset(&frame, 0, sizeof(frame));
	frame.pic = 0x123456700ULL;
	frame.bitstream = 0x200000100ULL;
	frame.slices = 0x300000200ULL;
	frame.coloc = 0x400000000ULL;
	frame.history = 0x500000000ULL;
	frame.status = 0x600000000ULL;
	frame.mbhist = 0x700000000ULL;
	frame.serial = 42U;
	frame.error_frame = 2U;
	for (index = 0U; index < NVRTX_NVDEC_SURFACES; index++) {
		frame.luma[index] = 0x10000000ULL + (uint64_t)index * 0x100000ULL;
		frame.chroma[index] = 0x20000000ULL + (uint64_t)index * 0x100000ULL;
	}

	/* Encodes the decode push buffer and checks each method against the hardware layout. */
	length = nvrtx_nvdec_push_decode(push, &frame);
	check(length <= NVRTX_NVDEC_DECODE_PUSH_BYTES, "push: within its bound");
	check(word(push, 0) == 0x20018080U && word(push, 4) == 3U, "push: application id H.264 on subchannel 4");
	check(word(push, 8) == (0x20018000U | (0x400U >> 2)) && word(push, 12) == (3U | (1U << 4) | (1U << 6) | (2U << 7) | (1U << 13)), "push: control word");
	check(word(push, 16) == (0x20018000U | (0x404U >> 2)) && word(push, 20) == 0x01234567U, "push: pic setup >> 8");
	check(word(push, 24) == (0x20018000U | (0x408U >> 2)) && word(push, 28) == 0x02000001U, "push: bitstream >> 8");
	check(word(push, 32) == (0x20018000U | (0x40cU >> 2)) && word(push, 36) == 42U, "push: picture index");
	check(word(push, 40) == (0x20018000U | (0x410U >> 2)) && word(push, 44) == 0x03000002U, "push: slice offsets");
	check(word(push, 48) == (0x20018000U | (0x414U >> 2)) && word(push, 56) == (0x20018000U | (0x418U >> 2)), "push: coloc then history");
	check(word(push, 64) == (0x20018000U | (0x424U >> 2)) && word(push, 68) == 0x06000000U, "push: status");
	check(word(push, 72) == (0x20000000U | (17U << 16) | 0x8000U | (0x430U >> 2)), "push: 17 luma offsets");
	check(word(push, 76 + 5 * 4) == (uint32_t)((0x10000000ULL + 5U * 0x100000ULL) >> 8), "push: luma 5");
	check(word(push, 144) == (0x20000000U | (17U << 16) | 0x8000U | (0x474U >> 2)), "push: 17 chroma offsets");
	check(word(push, 216) == (0x20018000U | (0x500U >> 2)) && word(push, 220) == 0x07000000U, "push: mbhist");
	check(word(push, 224) == (0x20018000U | (0x300U >> 2)) && word(push, 228) == 0U && length == 232U, "push: execute last");

	/* Encodes the object push buffer for the Turing class and checks the class by family. */
	length = nvrtx_nvdec_push_object(object, NVRTX_NVDEC_CLASS_TURING);
	check(length == 8U && word(object, 0) == 0x20018000U && word(object, 4) == 0xc4b0U, "push: the class on subchannel 4");
	check(nvrtx_nvdec_class(NVRTX_FAMILY_TURING) == 0xc4b0U && nvrtx_nvdec_class(NVRTX_FAMILY_AMPERE) == 0xc7b0U &&
	      nvrtx_nvdec_class(NVRTX_FAMILY_ADA) == 0xc9b0U && nvrtx_nvdec_class(NVRTX_FAMILY_HOPPER) == 0U, "push: the classes by family");
}

/* The work buffers' sizes and the NV12 layout for 1080p and 720p. */
static void
test_sizes(void)
{
	struct nvrtx_nvdec_buffers buffers;
	struct nvrtx_video_layout layout;

	/* Sizes the work buffers for 1080p and 720p streams. */
	nvrtx_nvdec_buffer_sizes(120U, 68U, 16U, &buffers);
	check(buffers.coloc == 522240U * 17U, "sizes: 1080p coloc for 17 surfaces");
	check(buffers.mbhist == 12544U && buffers.history == 92160U, "sizes: 1080p histories");
	nvrtx_nvdec_buffer_sizes(80U, 45U, 4U, &buffers);
	check(buffers.coloc == ((46U * 80U * 64U - 63U + 255U) & ~255U) * 5U, "sizes: 720p coloc rounds the map units to pairs");
	check(buffers.mbhist == 8448U && buffers.history == 61440U, "sizes: 720p histories");

	/* Lays out the 1080p NV12 surface. */
	nvrtx_video_layout(1920U, 1080U, &layout);
	check(layout.pitch == 1920U && layout.plane_rows[0] == 1088U && layout.plane_rows[1] == 544U, "layout: 1080p rows in 64-row CTBs");
	check(layout.plane_offset[1] == 1920U * 1088U && layout.bytes == 1920U * 1632U, "layout: 1080p planes");
	check((layout.plane_offset[1] & 255U) == 0U, "layout: chroma on 256 bytes");
	nvrtx_video_layout(1000U, 720U, &layout);
	check(layout.pitch == 1024U && layout.plane_rows[0] == 768U && layout.plane_rows[1] == 384U, "layout: width rounded to GOBs, 720 rows to 768, 360 to 384");
}

/* The scaling matrices: flat, the sets' own in raster order, the defaults and the fall-backs. */
static void
test_scaling(void)
{
	struct nvrtx_video_sps sps;
	struct nvrtx_video_pps pps;
	uint8_t s4[6][16];
	uint8_t s8[2][64];
	uint32_t index;

	/* Resolves the scaling matrices of a stream that carries none. */
	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	nvrtx_video_scaling_resolve(&sps, &pps, s4, s8);
	check(s4[0][0] == 16U && s4[5][15] == 16U && s8[1][63] == 16U, "scaling: flat without matrices");

	/* A sequence matrix: list 0 its own (scan index i holds i), list 3 the default, the rest by rule A. */
	sps.flags = NVRTX_VIDEO_SPS_SCALING;
	sps.has_scaling = true;
	sps.scaling.present_mask = (1U << 0) | (1U << 3) | (1U << 6);
	sps.scaling.default_mask = (1U << 3);

	/* Gives the sequence's 4x4 and 8x8 lists distinguishable values. */
	for (index = 0U; index < 16U; index++)
		sps.scaling.list4[0][index] = (uint8_t)index;
	for (index = 0U; index < 64U; index++)
		sps.scaling.list8[0][index] = (uint8_t)(index + 1U);
	nvrtx_video_scaling_resolve(&sps, &pps, s4, s8);
	check(s4[0][0] == 0U && s4[0][1] == 1U && s4[0][4] == 2U && s4[0][8] == 3U && s4[0][15] == 15U, "scaling: zig-zag to raster");
	check(memcmp(s4[1], s4[0], 16) == 0 && memcmp(s4[2], s4[0], 16) == 0, "scaling: rule A, lists 1 and 2 from list 0");
	check(s4[3][0] == 10U && s4[3][15] == 34U && memcmp(s4[4], s4[3], 16) == 0, "scaling: default inter, list 4 from 3");
	check(s8[0][0] == 1U && s8[0][1] == 2U && s8[0][8] == 3U, "scaling: 8x8 zig-zag to raster");
	check(s8[1][0] == 9U && s8[1][63] == 35U, "scaling: absent 8x8 inter is the default");

	/* A picture matrix: list 0 absent takes the sequence's (rule B), list 1 its own, list 2 from list 1. */
	pps.flags = NVRTX_VIDEO_PPS_SCALING;
	pps.has_scaling = true;
	pps.scaling.present_mask = (1U << 1);

	/* Gives the picture set's second 4x4 list a value of its own. */
	for (index = 0U; index < 16U; index++)
		pps.scaling.list4[1][index] = 50U;
	nvrtx_video_scaling_resolve(&sps, &pps, s4, s8);
	check(s4[0][4] == 2U && s4[1][0] == 50U && s4[2][7] == 50U, "scaling: rule B");
	check(s4[3][0] == 10U && s8[0][8] == 3U, "scaling: rule B takes the sequence's 3 and 8x8");
}

/* The DPB entries: kept per slot, the lowest free one, cleared when the slot is not bound. */
static void
test_dpb(void)
{
	struct nvrtx_video_dpb dpb;
	bool active[NVRTX_VIDEO_SLOTS];
	uint32_t entry;
	uint32_t referenced[2];
	uint32_t index;
	int error;

	/* Hands out DPB entries to slots in order. */
	nvrtx_video_dpb_reset(&dpb);
	error = nvrtx_video_dpb_entry(&dpb, 5U, &entry);
	check(error == 0 && entry == 0U, "dpb: first slot takes entry 0");
	error = nvrtx_video_dpb_entry(&dpb, 2U, &entry);
	check(error == 0 && entry == 1U, "dpb: next slot takes entry 1");
	error = nvrtx_video_dpb_entry(&dpb, 5U, &entry);
	check(error == 0 && entry == 0U, "dpb: a slot keeps its entry");

	/* Keeps only slot 2 bound and checks that a freed entry is reused. */
	for (index = 0U; index < NVRTX_VIDEO_SLOTS; index++)
		active[index] = false;
	active[2] = true;
	nvrtx_video_dpb_keep(&dpb, active);
	error = nvrtx_video_dpb_entry(&dpb, 7U, &entry);
	check(error == 0 && entry == 0U, "dpb: an unbound slot's entry is free again");
	nvrtx_video_dpb_forget(&dpb, 2U);
	check(dpb.entry[2] == -1, "dpb: a new picture forgets the slot's entry");

	/* Picks the lowest free surface for a picture that has no slot. */
	referenced[0] = 0U;
	referenced[1] = 1U;
	nvrtx_video_dpb_reset(&dpb);
	(void)nvrtx_video_dpb_entry(&dpb, 2U, &entry);
	index = nvrtx_video_free_surface(&dpb, referenced, 2U);
	check(index == 3U, "dpb: a picture without a slot takes the lowest free surface");

	/* Fills all sixteen entries and checks the next one is refused. */
	nvrtx_video_dpb_reset(&dpb);
	for (index = 0U; index < 16U; index++)
		(void)nvrtx_video_dpb_entry(&dpb, index, &entry);
	error = nvrtx_video_dpb_entry(&dpb, 16U, &entry);
	check(error == ENOSPC, "dpb: no seventeenth entry");
}

/* The copy engine's plane copy: the methods on subchannel 4 and the launch. */
static void
test_copy(void)
{
	struct nvrtx_exec_plane_copy copy;
	uint8_t push[NVRTX_EXEC_PLANE_COPY_BYTES];
	size_t at;

	/* Describes a plane copy and encodes its push buffer. */
	memset(&copy, 0, sizeof(copy));
	copy.source = 0x123456000ULL;
	copy.source_pitch = 1920U;
	copy.source_rows = 544U;
	copy.x_bytes = 64U;
	copy.y = 8U;
	copy.destination = 0x200001000ULL;
	copy.destination_pitch = 1920U;
	copy.line_bytes = 1856U;
	copy.lines = 532U;
	at = nvrtx_exec_copy_plane(push, 0U, &copy);
	check(at <= NVRTX_EXEC_PLANE_COPY_BYTES && at == 84U, "copy: within its bound");
	check(word(push, 0) == (0x20000000U | (4U << 16) | (4U << 13) | (0x400U >> 2)), "copy: offsets on subchannel 4");
	check(word(push, 4) == 1U && word(push, 8) == 0x23456000U && word(push, 12) == 2U && word(push, 16) == 0x00001000U, "copy: addresses");
	check(word(push, 20) == (0x20000000U | (4U << 16) | (4U << 13) | (0x410U >> 2)), "copy: pitches and lines");
	check(word(push, 24) == 1920U && word(push, 28) == 1920U && word(push, 32) == 1856U && word(push, 36) == 532U, "copy: pitch, row, lines");
	check(word(push, 40) == (0x20000000U | (5U << 16) | (4U << 13) | (0x728U >> 2)) && word(push, 44) == 0x1010U, "copy: block two GOBs, Fermi GOB");
	check(word(push, 48) == 1920U && word(push, 52) == 544U && word(push, 56) == 1U && word(push, 60) == 0U, "copy: source surface");
	check(word(push, 64) == (0x20000000U | (2U << 16) | (4U << 13) | (0x744U >> 2)) && word(push, 68) == 64U && word(push, 72) == 8U, "copy: origin");
	check(word(push, 76) == (0x20000000U | (1U << 16) | (4U << 13) | (0x300U >> 2)) && word(push, 80) == 0x306U, "copy: launch");
}

/* The resource manager's parameters, the falcons' list and the device table's pick. */
static void
test_rm(void)
{
	struct nvrtx_engine_pick pick;
	uint8_t params[NVRTX_NVDEC_PROMOTE_BYTES];
	uint8_t falcons[NVRTX_FALCON_INFO_PARAMS_BYTES];
	uint8_t table[NVRTX_DEVINFO_PARAMS_BYTES];
	uint32_t bytes;
	int error;

	/* Encodes the channel promote parameters and checks each word. */
	nvrtx_nvdec_promote_params(params, 0xc1d00000U, 16U, 0xf1f00010U, 0x28200000ULL, 0x3000ULL);
	check(word(params, 0) == 0x13U && word(params, 4) == 0xc1d00000U && word(params, 8) == 16U, "rm: promote engine, client, chid");
	check(word(params, 12) == 0xc1d00000U && word(params, 16) == 0xf1f00010U && word(params, 20) == 0U, "rm: promote channel");
	check(word(params, 24) == 0x28200000U && word(params, 28) == 0U && word(params, 32) == 0x3000U && word(params, 40) == 0U, "rm: promote address, size, no entries");

	/* Encodes the decoder object parameters. */
	nvrtx_nvdec_alloc_params(params);
	check(word(params, 0) == 12U && word(params, 4) == 0U && word(params, 8) == 0U, "rm: decoder object parameters");

	/* Builds a falcon list and looks the decoder's context size up by descriptor. */
	memset(falcons, 0, sizeof(falcons));
	nvrtx_put32(falcons, 0U, 2U);
	nvrtx_put32(falcons, 4U, 0x111U);
	nvrtx_put32(falcons, 12U, 0x1000U);
	nvrtx_put32(falcons, 24U, 0x222U);
	nvrtx_put32(falcons, 32U, 0x6000U);
	error = nvrtx_nvdec_context_bytes(falcons, sizeof(falcons), 0x222U, &bytes);
	check(error == 0 && bytes == 0x6000U, "rm: the decoder's context size by its descriptor");
	error = nvrtx_nvdec_context_bytes(falcons, sizeof(falcons), 0x333U, &bytes);
	check(error == ENOENT, "rm: no falcon of another descriptor");
	error = nvrtx_nvdec_context_bytes(falcons, 20U, 0x222U, &bytes);
	check(error == EINVAL, "rm: a list cut short");

	/* Graphics on runlist 0 with copy 1, copy 2 on runlist 3, the decoder (0x1d) on runlist 5. */
	memset(table, 0, sizeof(table));
	nvrtx_put32(table, 4U, 4U);
	nvrtx_put32(table, 12U + 0U * 100U + 8U, 0x01U);
	nvrtx_put32(table, 12U + 0U * 100U + 12U, 0U);
	nvrtx_put32(table, 12U + 1U * 100U + 8U, 0x0aU);
	nvrtx_put32(table, 12U + 1U * 100U + 12U, 0U);
	nvrtx_put32(table, 12U + 2U * 100U + 8U, 0x0bU);
	nvrtx_put32(table, 12U + 2U * 100U + 12U, 3U);
	nvrtx_put32(table, 12U + 3U * 100U + 0U, 0x84U);
	nvrtx_put32(table, 12U + 3U * 100U + 8U, 0x1dU);
	nvrtx_put32(table, 12U + 3U * 100U + 12U, 5U);
	error = nvrtx_devinfo_pick(table, sizeof(table), &pick);
	check(error == 0 && pick.engine == 0x0bU && pick.runlist == 3U, "rm: the copy engine of its own runlist");
	check(pick.graphics_copy == 0x0aU, "rm: the copy engine on the graphics runlist");
	check(pick.nvdec_found == 1U && pick.nvdec_runlist == 5U && pick.nvdec_descriptor == 0x84U, "rm: the decoder's runlist and descriptor");
}
