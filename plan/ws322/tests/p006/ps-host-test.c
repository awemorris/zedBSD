/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the H.265 decode path's parameter sets and picture
 * records (ws322-p006), over one Annex B bitstream (a file, or the
 * standard input with "-"):
 *
 * - every VPS, SPS and PPS a picture uses, when new or changed, made into
 *   StdVideo by libmedia's h265-std, encoded by libvulkan's own encoder
 *   (encode.c), read by the PowerVR executor's readers and written back
 *   into NAL units by the engine's writers (executor.c), then read again
 *   by libmedia's parse and compared with the first parse: every field
 *   the decoding reads must be equal (the VUI, the timing and HRD, and
 *   the signalled form of predicted short-term sets are not written back,
 *   and the compatibility flags and an off-table level are normalized);
 * - every picture's StdVideo picture information (with the DPB's plan,
 *   h265-dpb, 17 slots) and its slots, encoded as a decode and recorded by
 *   the executor: the flags, the ids, PicOrderCntVal, the setup slot and
 *   the reference slots must be as given.
 *
 * Last line: "ps: <sets> sets, <pictures> pictures, <checks> checks, <failed> failed".
 *
 *   ps-host-test STREAM|-
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "userland/desktop/libmedia/h265-dpb.h"
#include "userland/desktop/libmedia/h265-std.h"
#include "userland/desktop/libmedia/h265-stream.h"
#include "userland/desktop/libmedia/vcodec-bitreader.h"

#include "plan/ws322/tests/p006/ps-test.h"

/* The DPB's slots (as vkvideo-probe makes them). */
#define TEST_SLOTS		17U

/* The largest bitstream read. */
#define TEST_MAX_INPUT		(256U * 1024U * 1024U)

/* The levels StdVideoH265LevelIdc names, as general_level_idc. */
static const unsigned levels[13] = { 30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186 };

static unsigned long checks;
static unsigned long failures;
static unsigned long sets;
static unsigned long pictures;

/* The last checked copy of each set. */
static int vps_seen[H265SW_VPS_IDS];
static int sps_seen[H265SW_SPS_IDS];
static int pps_seen[H265SW_PPS_IDS];
static struct h265sw_vps vps_last[H265SW_VPS_IDS];
static struct h265sw_sps sps_last[H265SW_SPS_IDS];
static struct h265sw_pps pps_last[H265SW_PPS_IDS];

/* The work areas, kept off the stack. */
static uint8_t wire[TEST_WIRE_BYTES];
static uint8_t rbsp[TEST_UNIT_BYTES];
static struct test_units units;
static struct h265sw_ps_store store;
static struct h265_std_vps std_vps;
static struct h265_std_sps std_sps;
static struct h265_std_pps std_pps;

static void check(int ok, const char *what, uint64_t index);
static int read_input(const char *path, uint8_t **data, size_t *size);
static size_t unit_rbsp(const uint8_t *unit, size_t bytes, unsigned type, uint8_t *out);
static void check_ptl(const struct h265sw_ptl *orig, const struct h265sw_ptl *back, const StdVideoH265ProfileTierLevel *std, uint64_t index);
static void roundtrip_sets(const struct h265sw_ps_store *source, const struct h265_picture *picture);
static void roundtrip_picture(const struct h265_picture *picture, const struct h265_dpb_plan *plan);

/* Counts a check; a failed one is named with the picture it was made at. */
static void
check(
	int ok,
	const char *what,
	uint64_t index)
{
	checks++;
	if (ok)
		return;
	failures++;
	if (failures <= 40U)
		printf("FAIL picture %llu: %s\n", (unsigned long long)index, what);
}

int
main(
	int argc,
	char **argv)
{
	struct h265_stream *stream;
	struct h265_picture *picture;
	struct h265_dpb dpb;
	struct h265_dpb_plan plan;
	struct h265_dpb_output output[H265_DPB_MAX_OUTPUT];
	const struct h265sw_sps *sps;
	uint8_t *data;
	size_t size;
	unsigned count;
	unsigned index;
	int error;

	/* The input and the reader. */
	if (argc != 2) {
		fprintf(stderr, "usage: ps-host-test STREAM|-\n");
		return 2;
	}
	error = read_input(argv[1], &data, &size);
	if (error != 0) {
		fprintf(stderr, "ps-host-test: %s: %s\n", argv[1], strerror(error));
		return 2;
	}
	stream = calloc(1U, sizeof(*stream));
	picture = calloc(1U, sizeof(*picture));
	if (stream == NULL || picture == NULL)
		return 2;
	h265_stream_init(stream);
	h265_stream_input(stream, data, size);
	h265_dpb_init(&dpb, TEST_SLOTS);

	/* Each picture in decoding order. */
	for (;;) {
		error = h265_stream_next(stream, picture);
		if (error == ENOENT)
			break;
		if (error != 0)
			continue;
		pictures++;

		/* Its sets, when new or changed. */
		roundtrip_sets(&stream->store, picture);

		/* Its plan and its record (as vkvideo-probe makes them), then the DPB after it. */
		sps = &stream->store.sps[picture->sps_id];
		error = h265_dpb_begin(&dpb, sps, &picture->rps, picture->irap != 0 && picture->no_rasl_output != 0,
				       picture->no_output_of_prior_pics, &plan);
		for (index = 0; index < plan.output_count && (error == 0 || error == EINVAL); index++)
			h265_dpb_release(&dpb, plan.output[index].slot);
		if (error != 0)
			continue;
		error = h265_dpb_take_slot(&dpb, &plan);
		if (error != 0) {
			h265_dpb_abort(&dpb);
			continue;
		}
		roundtrip_picture(picture, &plan);
		error = h265_dpb_end(&dpb, &plan, picture->poc, picture->pic_output, picture->index, output, &count);
		if (error != 0) {
			h265_dpb_abort(&dpb);
			continue;
		}
		for (index = 0; index < count; index++)
			h265_dpb_release(&dpb, output[index].slot);
	}

	/* The counts. */
	printf("ps: %lu sets, %lu pictures, %lu checks, %lu failed\n", sets, pictures, checks, failures);
	h265_stream_destroy(stream);
	free(picture);
	free(stream);
	free(data);
	if (failures != 0 || pictures == 0)
		return 1;
	return 0;
}

/* Reads a whole file, or the standard input for "-". */
static int
read_input(
	const char *path,
	uint8_t **data,
	size_t *size)
{
	FILE *file;
	uint8_t *buffer;
	size_t got;

	file = stdin;
	if (strcmp(path, "-") != 0)
		file = fopen(path, "rb");
	if (file == NULL)
		return errno;
	buffer = malloc(TEST_MAX_INPUT);
	if (buffer == NULL)
		return ENOMEM;
	got = fread(buffer, 1U, TEST_MAX_INPUT, file);
	if (file != stdin)
		fclose(file);
	*data = buffer;
	*size = got;
	return 0;
}

/*
 * Takes a written unit apart: the four-byte start code and the two-byte
 * header of nal_unit_type type, nuh_layer_id 0 and TemporalId 0, then the
 * RBSP with its emulation prevention bytes removed.  0 for a unit that
 * does not start so.
 */
static size_t
unit_rbsp(
	const uint8_t *unit,
	size_t bytes,
	unsigned type,
	uint8_t *out)
{
	size_t index;
	size_t length;
	unsigned zeros;

	/* The start code and the header. */
	if (bytes < 7U || unit[0] != 0 || unit[1] != 0 || unit[2] != 0 || unit[3] != 1)
		return 0;
	if (unit[4] != (uint8_t)(type << 1) || unit[5] != 1U)
		return 0;

	/* The body without its emulation prevention bytes. */
	length = 0;
	zeros = 0;
	for (index = 6U; index < bytes; index++) {
		if (zeros >= 2U && unit[index] == 3U) {
			zeros = 0;
			continue;
		}
		out[length] = unit[index];
		length++;
		if (unit[index] == 0)
			zeros++;
		else
			zeros = 0;
	}
	return length;
}

/* Compares a profile, tier and level read back with the first parse, as StdVideo carried it. */
static void
check_ptl(
	const struct h265sw_ptl *orig,
	const struct h265sw_ptl *back,
	const StdVideoH265ProfileTierLevel *std,
	uint64_t index)
{
	check(back->profile_space == 0 && back->tier == orig->tier, "ptl space and tier", index);
	check(back->profile_idc == (unsigned)std->general_profile_idc, "ptl profile", index);
	check(back->progressive == orig->progressive && back->interlaced == orig->interlaced &&
	      back->non_packed == orig->non_packed && back->frame_only == orig->frame_only, "ptl source flags", index);
	check((unsigned)std->general_level_idc <= 12U && back->level_idc == levels[std->general_level_idc], "ptl level", index);
	check((back->compatibility & (0x80000000U >> back->profile_idc)) != 0 || (back->compatibility & (1U << back->profile_idc)) != 0,
	      "ptl own compatibility flag", index);
}

/*
 * The round trip of the picture's VPS, SPS and PPS when one is new or
 * changed: StdVideo, libvulkan's wire, the executor's sets, the engine's
 * units, the parse again.
 */
static void
roundtrip_sets(
	const struct h265sw_ps_store *source,
	const struct h265_picture *picture)
{
	VkVideoDecodeH265SessionParametersAddInfoKHR add;
	struct vcodec_bitreader bits;
	const struct h265sw_vps *vps;
	const struct h265sw_sps *sps;
	const struct h265sw_pps *pps;
	struct h265sw_sps copy;
	struct h265sw_pps pps_copy;
	size_t length;
	size_t bytes;
	unsigned sub;
	int has_vps;
	int error;

	/* Only sets new or changed since their last check. */
	vps = &source->vps[picture->vps_id];
	sps = &source->sps[picture->sps_id];
	pps = &source->pps[picture->pps_id];
	has_vps = source->vps_present[picture->vps_id];
	if (has_vps && vps_seen[picture->vps_id] && memcmp(&vps_last[picture->vps_id], vps, sizeof(*vps)) == 0 &&
	    sps_seen[picture->sps_id] && memcmp(&sps_last[picture->sps_id], sps, sizeof(*sps)) == 0 &&
	    pps_seen[picture->pps_id] && memcmp(&pps_last[picture->pps_id], pps, sizeof(*pps)) == 0)
		return;
	if (has_vps) {
		vps_last[picture->vps_id] = *vps;
		vps_seen[picture->vps_id] = 1;
	}
	sps_last[picture->sps_id] = *sps;
	sps_seen[picture->sps_id] = 1;
	pps_last[picture->pps_id] = *pps;
	pps_seen[picture->pps_id] = 1;
	sets++;

	/* StdVideo; a profile Vulkan does not name is not for this path. */
	error = 0;
	if (has_vps)
		error = h265_std_vps(vps, &std_vps);
	if (error == 0)
		error = h265_std_sps(sps, &std_sps);
	if (error == 0)
		error = h265_std_pps(pps, sps, &std_pps);
	if (error == ENOTSUP)
		return;
	check(error == 0, "h265-std made the sets", picture->index);
	if (error != 0)
		return;

	/* libvulkan's wire, the executor's sets and the engine's units. */
	memset(&add, 0, sizeof(add));
	add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
	add.stdVPSCount = has_vps ? 1U : 0U;
	add.pStdVPSs = &std_vps.vps;
	add.stdSPSCount = 1U;
	add.pStdSPSs = &std_sps.sps;
	add.stdPPSCount = 1U;
	add.pStdPPSs = &std_pps.pps;
	length = test_encode_parameters(&add, wire, sizeof(wire));
	check(length != 0, "libvulkan encoded the sets", picture->index);
	if (length == 0)
		return;
	error = test_executor_sets(wire, length, &units);
	check(error == 0, "the executor read the sets", picture->index);
	if (error != 0)
		return;
	check(units.vps_count == add.stdVPSCount && units.sps_count == 1U && units.pps_count == 1U, "the executor kept each set", picture->index);
	check(units.vps_error == 0 && units.sps_error == 0 && units.pps_error == 0, "the engine wrote each set", picture->index);
	if (units.vps_error != 0 || units.sps_error != 0 || units.pps_error != 0)
		return;

	/* The VPS read again: every field written from the first parse. */
	if (has_vps) {
		h265sw_ps_init(&store);
		bytes = unit_rbsp(units.vps, units.vps_bytes, 32U, rbsp);
		vcodec_bitreader_init(&bits, rbsp, bytes);
		error = h265sw_ps_read_vps(&store, &bits);
		check(error == 0 && store.vps_present[vps->id], "the VPS reads again", picture->index);
		if (error == 0 && store.vps_present[vps->id]) {
			const struct h265sw_vps *back = &store.vps[vps->id];

			check(back->max_sub_layers_minus1 == vps->max_sub_layers_minus1 && back->temporal_id_nesting == vps->temporal_id_nesting,
			      "VPS sub-layers", picture->index);
			check(back->sub_layer_ordering_info_present == vps->sub_layer_ordering_info_present, "VPS ordering present", picture->index);
			for (sub = 0; sub <= vps->max_sub_layers_minus1; sub++) {
				check(back->max_dec_pic_buffering_minus1[sub] == vps->max_dec_pic_buffering_minus1[sub] &&
				      back->max_num_reorder_pics[sub] == vps->max_num_reorder_pics[sub] &&
				      back->max_latency_increase_plus1[sub] == vps->max_latency_increase_plus1[sub], "VPS ordering", picture->index);
			}
			check_ptl(&vps->ptl, &back->ptl, &std_vps.ptl, picture->index);
		}
	}

	/* The SPS read again: the whole structure, but for what is not written back. */
	h265sw_ps_init(&store);
	bytes = unit_rbsp(units.sps, units.sps_bytes, 33U, rbsp);
	vcodec_bitreader_init(&bits, rbsp, bytes);
	error = h265sw_ps_read_sps(&store, &bits);
	check(error == 0 && store.sps_present[sps->id], "the SPS reads again", picture->index);
	if (error == 0 && store.sps_present[sps->id]) {
		const struct h265sw_sps *back = &store.sps[sps->id];

		check_ptl(&sps->ptl, &back->ptl, &std_sps.ptl, picture->index);
		check(back->vui_present == 0, "SPS without VUI", picture->index);
		check(back->extension_present == sps->range_extension_present, "SPS extension present only for the range extension", picture->index);
		memcpy(&copy, sps, sizeof(copy));
		copy.ptl = back->ptl;
		copy.vui_present = back->vui_present;
		copy.vui = back->vui;
		copy.extension_present = back->extension_present;
		memcpy(copy.st_syntax, back->st_syntax, sizeof(copy.st_syntax));
		check(memcmp(&copy, back, sizeof(copy)) == 0, "SPS fields", picture->index);
	}

	/* The PPS read again: the whole structure, but the extension flag. */
	h265sw_ps_init(&store);
	bytes = unit_rbsp(units.pps, units.pps_bytes, 34U, rbsp);
	vcodec_bitreader_init(&bits, rbsp, bytes);
	error = h265sw_ps_read_pps(&store, &bits);
	check(error == 0 && store.pps_present[pps->id], "the PPS reads again", picture->index);
	if (error == 0 && store.pps_present[pps->id]) {
		memcpy(&pps_copy, pps, sizeof(pps_copy));
		pps_copy.extension_present = store.pps[pps->id].extension_present;
		check(store.pps[pps->id].extension_present == pps->range_extension_present, "PPS extension present only for the range extension", picture->index);
		check(memcmp(&pps_copy, &store.pps[pps->id], sizeof(pps_copy)) == 0, "PPS fields", picture->index);
	}
}

/* The round trip of a picture's record: its StdVideo picture and slots, libvulkan's wire, the executor's record. */
static void
roundtrip_picture(
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan)
{
	StdVideoDecodeH265PictureInfo info;
	StdVideoDecodeH265ReferenceInfo references[H265_DPB_PICTURES];
	StdVideoDecodeH265ReferenceInfo setup_info;
	VkVideoDecodeH265DpbSlotInfoKHR reference_dpb[H265_DPB_PICTURES];
	VkVideoDecodeH265DpbSlotInfoKHR setup_dpb;
	VkVideoReferenceSlotInfoKHR slots[H265_DPB_PICTURES];
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoPictureResourceInfoKHR resource;
	VkVideoDecodeH265PictureInfoKHR h265;
	VkVideoDecodeInfoKHR decode;
	struct test_picture read;
	uint32_t offsets[4] = { 0U, 100U, 200U, 300U };
	uint32_t flags;
	unsigned index;
	size_t length;
	int error;

	/* The StdVideo records. */
	error = h265_std_picture(picture, plan, &info);
	if (error == ENOTSUP)
		return;
	check(error == 0, "h265-std made the picture information", picture->index);
	if (error != 0)
		return;
	h265_std_references(plan, references);
	h265_std_setup(picture->poc, &setup_info);

	/* The decode with its slots, as vkvideo-probe records it. */
	memset(&resource, 0, sizeof(resource));
	resource.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
	resource.codedExtent.width = 416U;
	resource.codedExtent.height = 240U;
	resource.imageViewBinding = (VkImageView)(uintptr_t)0x50U;
	for (index = 0; index < plan->reference_count; index++) {
		memset(&reference_dpb[index], 0, sizeof(reference_dpb[index]));
		reference_dpb[index].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
		reference_dpb[index].pStdReferenceInfo = &references[index];
		memset(&slots[index], 0, sizeof(slots[index]));
		slots[index].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		slots[index].pNext = &reference_dpb[index];
		slots[index].slotIndex = plan->reference_slot[index];
		slots[index].pPictureResource = &resource;
	}
	memset(&setup_dpb, 0, sizeof(setup_dpb));
	setup_dpb.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
	setup_dpb.pStdReferenceInfo = &setup_info;
	memset(&setup, 0, sizeof(setup));
	setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	setup.pNext = &setup_dpb;
	setup.slotIndex = plan->setup_slot;
	setup.pPictureResource = &resource;
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR;
	h265.pStdPictureInfo = &info;
	h265.sliceSegmentCount = picture->slice_count < 4U ? picture->slice_count : 4U;
	h265.pSliceSegmentOffsets = offsets;
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
	decode.pNext = &h265;
	decode.srcBuffer = (VkBuffer)(uintptr_t)0x40U;
	decode.srcBufferRange = 4096U;
	decode.dstPictureResource = resource;
	decode.pSetupReferenceSlot = &setup;
	decode.referenceSlotCount = plan->reference_count;
	decode.pReferenceSlots = slots;

	/* Encoded, recorded, compared. */
	length = test_encode_decode(&decode, wire, sizeof(wire));
	check(length != 0, "libvulkan encoded the decode", picture->index);
	if (length == 0)
		return;
	error = test_executor_decode(wire, length, &read);
	check(error == 0 && read.has_picture, "the executor recorded the decode", picture->index);
	if (error != 0 || !read.has_picture)
		return;
	flags = 0;
	if (info.flags.IrapPicFlag)
		flags |= 1U;
	if (info.flags.IdrPicFlag)
		flags |= 2U;
	check((read.flags & 3U) == flags, "picture IRAP and IDR flags", picture->index);
	check(read.vps_id == info.sps_video_parameter_set_id && read.sps_id == info.pps_seq_parameter_set_id &&
	      read.pps_id == info.pps_pic_parameter_set_id, "picture ids", picture->index);
	check(read.poc == info.PicOrderCntVal && read.poc == picture->poc, "picture order count", picture->index);
	check(read.setup == plan->setup_slot, "setup slot", picture->index);
	check(read.reference_count == plan->reference_count, "reference count", picture->index);
	for (index = 0; index < plan->reference_count && index < read.reference_count; index++)
		check(read.references[index] == plan->reference_slot[index], "reference slot", picture->index);
}
