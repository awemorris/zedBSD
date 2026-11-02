/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's side of the H.265 video wire round trip: the bytes
 * libvulkan's own encoders wrote (h265-wire-encode.c) are read by the
 * executor's video readers (src/drivers/gpu/nvrtx/vk/vk-video.c, included
 * so its static readers can be called), which must take every byte and
 * every field the decode keeps.  The node's and the registry's calls the
 * readers do not reach are stubs.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/drivers/gpu/nvrtx/vk/vk-video.c"

#include "plan/ws324/tests/h265-wire-test.h"

static void check(int ok, const char *what);
static void reader_on(struct i915_wire_reader *reader, const uint8_t *bytes, size_t length);
static void test_parameters(void);
static void test_decode(void);
static void test_profiles(void);

/* How many checks the run has made; it only grows, and the summary reports it. */
static int checks;

/* How many of the checks failed; zero means the run passed. */
static int failures;

/* Allocates for the executor's code as the host allocator does. */
void *
kern_malloc(
	size_t bytes)
{
	void *memory;

	/* Takes the memory from the host. */
	memory = malloc(bytes);

	/* Reports the host's allocation. */
	return memory;
}

/* Allocates zeroed memory for the executor's code as the host allocator does. */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *memory;

	/* Takes the zeroed memory from the host. */
	memory = calloc(count, bytes);

	/* Reports the host's allocation. */
	return memory;
}

/* Releases memory the executor's code allocated. */
void
kern_free(
	void *pointer)
{
	/* Gives the memory back to the host. */
	free(pointer);
}

/* Drops the kernel log, which this test does not read. */
void
kern_logf(
	const char *format,
	...)
{
	(void)format;
}

/* Reports the video queue as present, so the readers take video calls. */
bool
nvrtx_vk_video_available(
	void *owner)
{
	(void)owner;

	/* Succeeded: the stub always has the queue. */
	return true;
}

/* Refuses a decode, which the readers under test never submit. */
int
nvrtx_vk_video_decode(
	void *owner,
	uint64_t va,
	uint32_t bytes)
{
	(void)owner;
	(void)va;
	(void)bytes;

	/* Refuses the decode. */
	return EIO;
}

/* Refuses a system allocation, which the readers under test never make. */
int
nvrtx_vk_gpu_alloc_system(
	void *owner,
	uint64_t bytes,
	void **token,
	uint8_t **cpu,
	uint64_t *va)
{
	(void)owner;
	(void)bytes;
	(void)token;
	(void)cpu;
	(void)va;

	/* Refuses the allocation. */
	return ENOMEM;
}

/* Releases nothing, as the stub allocation never hands anything out. */
void
nvrtx_vk_gpu_free(
	void *owner,
	void *token)
{
	(void)owner;
	(void)token;
}

/* Refuses to resolve a buffer's address, which the readers under test never need. */
int
nvrtx_vk_buffer_address(
	const struct nvrtx_vk_buffer *buffer,
	uint64_t *va,
	uint8_t **cpu)
{
	(void)buffer;
	(void)va;
	(void)cpu;

	/* Refuses the resolution. */
	return EIO;
}

/* Refuses to resolve an image's address, which the readers under test never need. */
int
nvrtx_vk_image_address(
	const struct nvrtx_vk_image *image,
	uint64_t *va,
	uint8_t **cpu)
{
	(void)image;
	(void)va;
	(void)cpu;

	/* Refuses the resolution. */
	return EIO;
}


/* Reports that the boot leaves the H.265 slice header's skip to the driver. */
bool
nvrtx_vk_video_hevc_dxva(
	void *owner)
{
	(void)owner;

	/* The default. */
	return false;
}

/* The boot's choice of the decoder's log: off. */
bool
nvrtx_vk_video_hevc_trace(
	void *owner)
{
	(void)owner;

	/* The default. */
	return false;
}

/* Runs the three round trips. */
int
main(void)
{
	/* Reads a parameters create, a decode and two profiles that libvulkan encoded. */
	test_parameters();
	test_decode();
	test_profiles();

	/* Tells the caller how many checks ran and how many failed. */
	printf("h265 video wire round trip: %d checks, %d failed\n", checks, failures);

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

/* Points a reader at a run of bytes. */
static void
reader_on(
	struct i915_wire_reader *reader,
	const uint8_t *bytes,
	size_t length)
{
	/* Starts the reader at the first byte of the run. */
	memset(reader, 0, sizeof(*reader));
	reader->base = bytes;
	reader->size = length;
}

/* An H.265 parameters create with every pointer of its three sets filled. */
static void
test_parameters(void)
{
	static uint8_t bytes[65536];
	struct i915_wire_reader reader;
	struct video_chain chain;
	struct video_parameters *parameters;
	const struct nvrtx_video_h265_sps *sps;
	const struct nvrtx_video_h265_pps *pps;
	uint64_t present;
	uint64_t session_id;
	uint32_t type;
	size_t length;
	int error;

	/* Has libvulkan encode the parameters create. */
	length = wire_encode_h265_parameters(bytes, sizeof(bytes));
	check(length != 0U, "parameters: libvulkan encoded it");

	/* Reads the create's header, its chain and the session it names. */
	reader_on(&reader, bytes, length);
	memset(&chain, 0, sizeof(chain));
	present = drv_i915_wire_read_u64(&reader);
	type = drv_i915_wire_read_u32(&reader);
	video_read_chain(&reader, &chain, 0U);
	(void)drv_i915_wire_read_u32(&reader);
	(void)drv_i915_wire_read_u64(&reader);
	session_id = drv_i915_wire_read_u64(&reader);

	/* Every byte was taken and the H.265 create arrived. */
	check(present == 1U && type == VIDEO_TYPE_PARAMETERS_CREATE && reader.error == 0, "parameters: read");
	check(reader.offset == length && session_id == 0x70U, "parameters: every byte taken");
	check(chain.has_h265_create && !chain.has_h264_create, "parameters: an H.265 create");
	check(chain.max_vps == 2U && chain.max_sps == 4U && chain.max_pps == 8U && chain.add_error == 0, "parameters: capacity");
	check(chain.new_h265_vps == (1U << 3), "parameters: the video set's id");
	check(chain.new_h265_sps != NULL && chain.new_h265_sps->next == NULL, "parameters: one sequence set");
	check(chain.new_h265_pps != NULL && chain.new_h265_pps->next == NULL, "parameters: one picture set");

	/* The sets, when both arrived. */
	if (chain.new_h265_sps != NULL && chain.new_h265_pps != NULL) {
		sps = &chain.new_h265_sps->sps;
		pps = &chain.new_h265_pps->pps;
		check(sps->vps_id == 3U && sps->id == 5U && sps->chroma_format_idc == 1U, "parameters: SPS key");
		check(sps->width == 1920U && sps->height == 1080U, "parameters: SPS size");
		check((sps->flags & NVRTX_VIDEO_H265_SPS_SCALING) != 0U && (sps->flags & NVRTX_VIDEO_H265_SPS_SCALING_DATA) != 0U, "parameters: SPS scaling flags");
		check((sps->flags & NVRTX_VIDEO_H265_SPS_SAO) != 0U && (sps->flags & NVRTX_VIDEO_H265_SPS_PCM) != 0U, "parameters: SPS tools");
		check((sps->flags & NVRTX_VIDEO_H265_SPS_LONG_TERM) != 0U && (sps->flags & NVRTX_VIDEO_H265_SPS_TEMPORAL_MVP) != 0U &&
		      (sps->flags & NVRTX_VIDEO_H265_SPS_STRONG_INTRA) != 0U && (sps->flags & NVRTX_VIDEO_H265_SPS_AMP) == 0U, "parameters: SPS more tools");
		check(sps->log2_max_poc_lsb_minus4 == 4U && sps->log2_diff_cb == 3U && sps->log2_diff_tb == 3U, "parameters: SPS sizes");
		check(sps->depth_inter == 2U && sps->depth_intra == 1U, "parameters: SPS depths");
		check(sps->short_term_sets == 2U && sps->long_term_sps == 2U, "parameters: SPS reference sets");
		check(sps->pcm_depth_luma_minus1 == 7U && sps->pcm_depth_chroma_minus1 == 6U && sps->log2_min_pcm_minus3 == 1U && sps->log2_diff_pcm == 1U, "parameters: SPS PCM");
		check(sps->has_scaling && sps->scaling.list4[2][15] == 16U && sps->scaling.list8[5][63] == 65U, "parameters: SPS 4x4 and 8x8 lists");
		check(sps->scaling.list16[1][0] == 3U && sps->scaling.list32[1][63] == 67U, "parameters: SPS 16x16 and 32x32 lists");
		check(sps->scaling.dc16[4] == 21U && sps->scaling.dc32[1] == 22U, "parameters: SPS DC values");
		check(pps->id == 33U && pps->sps_id == 5U && pps->vps_id == 3U, "parameters: PPS key");
		check(pps->extra_bits == 2U && pps->l0_minus1 == 3U && pps->l1_minus1 == 1U, "parameters: PPS counts");
		check(pps->init_qp_minus26 == -4 && pps->cb_qp_offset == -3 && pps->cr_qp_offset == 5, "parameters: PPS quantizers");
		check(pps->beta_div2 == -2 && pps->tc_div2 == 1 && pps->log2_merge_minus2 == 1U && pps->cu_qp_delta_depth == 2U, "parameters: PPS offsets");
		check((pps->flags & NVRTX_VIDEO_H265_PPS_TILES) != 0U && (pps->flags & NVRTX_VIDEO_H265_PPS_ACROSS_TILES) != 0U, "parameters: PPS tile flags");
		check((pps->flags & NVRTX_VIDEO_H265_PPS_SCALING_DATA) != 0U && (pps->flags & NVRTX_VIDEO_H265_PPS_DEBLOCKING_CONTROL) != 0U &&
		      (pps->flags & NVRTX_VIDEO_H265_PPS_CU_QP_DELTA) != 0U, "parameters: PPS flags");
		check(pps->tile_columns_minus1 == 2U && pps->tile_rows_minus1 == 1U, "parameters: PPS tile counts");
		check(pps->column_width_minus1[0] == 9U && pps->column_width_minus1[18] == 18U, "parameters: PPS column widths");
		check(pps->row_height_minus1[0] == 7U && pps->row_height_minus1[20] == 20U, "parameters: PPS row heights");
		check(pps->has_scaling && pps->scaling.list4[0][0] == 77U, "parameters: PPS scaling lists");
	}

	/* The sets go into a parameters object: the create replaces, an update of the same keys is refused. */
	parameters = calloc(1U, sizeof(*parameters));
	if (parameters == NULL) {
		check(0, "parameters: object allocated");
		video_chain_free(&chain);
		return;
	}
	parameters->codec = VIDEO_CODEC_H265;
	parameters->max_vps = chain.max_vps;
	parameters->max_sps = chain.max_sps;
	parameters->max_pps = chain.max_pps;
	error = video_h265_take(parameters, &chain, true);
	check(error == 0 && parameters->h265_vps == (1U << 3), "parameters: taken");
	check(video_find_h265_sps(parameters, 3U, 5U) != NULL && video_find_h265_sps(parameters, 2U, 5U) == NULL, "parameters: SPS found by its key");
	check(video_find_h265_pps(parameters, 3U, 5U, 33U) != NULL && video_find_h265_pps(parameters, 3U, 5U, 32U) == NULL, "parameters: PPS found by its key");
	video_chain_free(&chain);

	/* The same sets again as an update: refused whole. */
	reader_on(&reader, bytes, length);
	memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(&reader);
	(void)drv_i915_wire_read_u32(&reader);
	video_read_chain(&reader, &chain, 0U);
	error = video_h265_take(parameters, &chain, false);
	check(error == EINVAL, "parameters: an update repeating keys is refused");
	video_chain_free(&chain);

	/* Frees the object's sets. */
	video_h265_free(parameters);
	check(parameters->h265_sps == NULL && parameters->h265_pps == NULL && parameters->h265_vps == 0U, "parameters: freed");
	free(parameters);
}

/* An H.265 decode with its picture, slice segments, setup slot and two references. */
static void
test_decode(void)
{
	static uint8_t bytes[4096];
	struct i915_wire_reader reader;
	struct video_record *video;
	size_t length;
	int error;

	/* Has libvulkan encode the decode. */
	length = wire_encode_h265_decode(bytes, sizeof(bytes));
	check(length != 0U, "decode: libvulkan encoded it");

	/* Allocates the record the reader fills. */
	video = calloc(1U, sizeof(*video));
	if (video == NULL) {
		check(0, "decode: record allocated");
		return;
	}

	/* Reads the bytes back into the record. */
	video->opcode = GPU_OP_CMD_DECODE_VIDEO;
	reader_on(&reader, bytes, length);
	error = video_record_coding(NULL, NULL, GPU_OP_CMD_DECODE_VIDEO, &reader, video);
	check(error == 0 && reader.error == 0, "decode: read");

	/* Every byte was taken and every field arrived. */
	check(reader.offset == length, "decode: every byte taken");
	check(video->offset == 512U && video->range == 2048U, "decode: bitstream range");
	check(video->has_setup && video->setup.index == 7, "decode: setup slot");
	check(video->slot_count == 2U && video->slots[0].index == 1 && video->slots[1].index == 2, "decode: references");
	check(video->slots[0].has_h265_reference && video->slots[0].h265_reference.poc == -10, "decode: reference 0");
	check(video->slots[1].has_h265_reference && video->slots[1].h265_reference.poc == -9 &&
	      (video->slots[1].h265_reference.flags & NVRTX_VIDEO_H265_REFERENCE_LONG_TERM) != 0U, "decode: reference 1 long term");
	check(video->has_h265_picture && !video->has_picture, "decode: an H.265 picture");
	check(video->h265_picture.vps_id == 3U && video->h265_picture.sps_id == 5U && video->h265_picture.pps_id == 33U, "decode: picture ids");
	check(video->h265_picture.poc == -6 && video->h265_picture.delta_pocs == 2U && video->h265_picture.short_term_bits == 11U, "decode: picture counts");
	check((video->h265_picture.flags & NVRTX_VIDEO_H265_PICTURE_REFERENCE) != 0U &&
	      (video->h265_picture.flags & NVRTX_VIDEO_H265_PICTURE_SPS_SET) != 0U &&
	      (video->h265_picture.flags & NVRTX_VIDEO_H265_PICTURE_IDR) == 0U, "decode: picture flags");
	check(video->h265_picture.before[0] == 1U && video->h265_picture.before[1] == 0xffU, "decode: set before");
	check(video->h265_picture.after[0] == 0xffU && video->h265_picture.long_term[0] == 2U, "decode: sets after and long term");
	check(video->slice_count == 3U && video->slices[1] == 700U && video->slices[2] == 1500U, "decode: slice segments");
	check(video->skip == NULL, "decode: not skipped");

	/* Releases the record. */
	free(video);
}

/* The H.265 Main profile is taken, Main 10 is refused as a codec the decoder does not take. */
static void
test_profiles(void)
{
	static uint8_t bytes[256];
	struct i915_wire_reader reader;
	uint32_t codec;
	uint32_t idc;
	int32_t result;
	size_t length;

	/* Main. */
	length = wire_encode_h265_profile(bytes, sizeof(bytes), 1U);
	reader_on(&reader, bytes, length);
	result = video_read_profile(&reader, &codec, &idc);
	check(length != 0U && reader.error == 0 && reader.offset == length, "profile: Main read whole");
	check(result == 0 && codec == VIDEO_CODEC_H265 && idc == 1U, "profile: Main taken as H.265");

	/* Main 10. */
	length = wire_encode_h265_profile(bytes, sizeof(bytes), 2U);
	reader_on(&reader, bytes, length);
	result = video_read_profile(&reader, &codec, &idc);
	check(reader.error == 0 && result == VIDEO_ERROR_CODEC && codec == 0U, "profile: Main 10 refused");
}
