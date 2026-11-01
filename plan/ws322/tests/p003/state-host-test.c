/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the pure parts of ws322-p003: NV12's layout and its
 * planes (image-state.c, powervr_image_lay_out_planes), the words of a
 * plane's view, and what a session offers of a video engine's operations
 * and one query's record (video-state.c).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libc/vulkan/vulkan_video.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/video-state.h"

static int failures;
static int passes;

/* One expectation. */
static void
expect(
	int good,
	const char *what)
{
	if (good) {
		passes++;
		return;
	}
	failures++;
	printf("FAIL %s\n", what);
}

/* Lays NV12 of some edges out. */
static int
lay(
	uint32_t width,
	uint32_t height,
	uint32_t levels,
	struct powervr_image_plane planes[POWERVR_IMAGE_PLANES_MAX],
	uint32_t *count,
	struct powervr_image_layout *layout)
{
	struct powervr_image_shape shape;

	memset(&shape, 0, sizeof(shape));
	shape.width = width;
	shape.height = height;
	shape.depth = 1;
	shape.levels = levels;
	shape.layers = 1;
	return powervr_image_lay_out_planes(&shape, powervr_format_find(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM), planes, count, layout);
}

/* Checks one size's planes: rows of the width rounded to 32 bytes, 32-row tiles, the chroma plane on 4 KiB. */
static void
check_size(
	uint32_t width,
	uint32_t height)
{
	struct powervr_image_plane planes[POWERVR_IMAGE_PLANES_MAX];
	struct powervr_image_layout layout;
	uint64_t luma;
	uint64_t chroma_offset;
	uint64_t chroma;
	uint32_t pitch;
	uint32_t count;
	char what[96];
	int error;

	pitch = (width + 31U) & ~31U;
	luma = (uint64_t)pitch * ((height + 31U) & ~31U);
	chroma_offset = (luma + 4095U) & ~(uint64_t)4095U;
	chroma = (uint64_t)pitch * ((height / 2U + 31U) & ~31U);
	error = lay(width, height, 1, planes, &count, &layout);
	snprintf(what, sizeof(what), "nv12 %ux%u lays out", width, height);
	expect(error == 0 && count == 2, what);
	snprintf(what, sizeof(what), "nv12 %ux%u planes", width, height);
	expect(planes[0].format->vk_format == VK_FORMAT_R8_UNORM && planes[1].format->vk_format == VK_FORMAT_R8G8_UNORM, what);
	expect(planes[0].width == width && planes[0].height == height && planes[1].width == width / 2U && planes[1].height == height / 2U, what);
	snprintf(what, sizeof(what), "nv12 %ux%u rows", width, height);
	expect(planes[0].layout.row_pitch == pitch && planes[1].layout.row_pitch == pitch && layout.row_pitch == pitch, what);
	snprintf(what, sizeof(what), "nv12 %ux%u places", width, height);
	expect(planes[0].offset == 0 && planes[0].layout.bytes == luma && planes[1].offset == chroma_offset && planes[1].layout.bytes == chroma, what);
	expect(layout.bytes == chroma_offset + chroma && layout.layer_bytes == layout.bytes, what);
}

/* Checks NV12's layouts and refusals. */
static void
check_layouts(void)
{
	struct powervr_image_plane planes[POWERVR_IMAGE_PLANES_MAX];
	struct powervr_image_layout layout;
	struct powervr_image_shape shape;
	const struct powervr_format *nv12;
	uint32_t count;
	int error;

	/* The table's entry: copied out, neither unit reads it whole. */
	nv12 = powervr_format_find(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	expect(nv12 != NULL && (nv12->kind & POWERVR_FORMAT_PLANAR) != 0, "nv12 is in the table, planar");
	expect(nv12 != NULL && nv12->optimal_features == VK_FORMAT_FEATURE_TRANSFER_SRC_BIT && nv12->linear_features == VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "nv12 is copied out only");

	/* Sizes a decoder writes. */
	check_size(1920, 1080);
	check_size(1280, 720);
	check_size(3840, 2160);
	check_size(352, 288);
	check_size(64, 64);
	check_size(2, 2);

	/* 1080p by hand: 1920 x 1088 luma, chroma at the same offset (a 4 KiB multiple), 1920 x 544. */
	error = lay(1920, 1080, 1, planes, &count, &layout);
	expect(error == 0 && planes[1].offset == 2088960U && layout.bytes == 3133440U, "nv12 1920x1080 by hand");

	/* Refusals: odd edges, zero, too large, levels, a format not planar, the plane layout of a plain format. */
	error = lay(1919, 1080, 1, planes, &count, &layout);
	expect(error == EINVAL && count == 0, "an odd width is refused");
	error = lay(1920, 1079, 1, planes, &count, &layout);
	expect(error == EINVAL, "an odd height is refused");
	error = lay(0, 2, 1, planes, &count, &layout);
	expect(error == EINVAL, "a zero width is refused");
	error = lay(POWERVR_IMAGE_EDGE_MAX + 2U, 2, 1, planes, &count, &layout);
	expect(error == EINVAL, "too wide is refused");
	error = lay(64, 64, 2, planes, &count, &layout);
	expect(error == ENOTSUP, "levels are refused");
	memset(&shape, 0, sizeof(shape));
	shape.width = 64;
	shape.height = 64;
	shape.depth = 1;
	shape.levels = 1;
	shape.layers = 1;
	error = powervr_image_lay_out_planes(&shape, powervr_format_find(VK_FORMAT_R8_UNORM), planes, &count, &layout);
	expect(error == ENOTSUP, "a plain format has no planes");
	error = powervr_image_lay_out(&shape, nv12, &layout);
	expect(error == ENOTSUP, "nv12 is not laid out as one format");
}

/* Checks a plane's view words: a strided texture of the plane's format at the plane's address. */
static void
check_views(void)
{
	struct powervr_image_plane planes[POWERVR_IMAGE_PLANES_MAX];
	struct powervr_image_layout layout;
	struct powervr_view_shape view;
	uint64_t words[2];
	uint32_t count;
	int error;

	error = lay(1920, 1080, 1, planes, &count, &layout);
	expect(error == 0, "1080p for the views");
	memset(&view, 0, sizeof(view));
	view.format = planes[1].format;
	view.layout = &planes[1].layout;
	view.address = 0x8000100000ULL + planes[1].offset;
	view.width = planes[1].width;
	view.height = planes[1].height;
	view.image_levels = 1;
	view.level_count = 1;
	view.layer_count = 1;
	view.linear = true;
	error = powervr_view_words(&view, words);
	expect(error == 0, "the chroma view's words");

	/*
	 * Word 0: strided (4), R8G8's texture format 7, width 959, height 539;
	 * word 1: the row in texels less one (959), the address in 4-byte
	 * units, one level.
	 */
	expect((words[0] & 7U) == 4U, "the chroma view is strided");
	expect(((words[0] >> 27) & 0x7FU) == 7U, "the chroma view reads R8G8");
	expect(((words[0] >> 34) & 0x3FFFU) == 959U && ((words[0] >> 48) & 0x3FFFU) == 539U, "the chroma view's edges");
	expect((words[1] & 0x7FFFU) == 959U, "the chroma view's row: 1920 bytes of 2-byte texels");
	expect(((words[1] >> 16) & 0x3FFFFFFFFFULL) == ((0x8000100000ULL + 2088960U) >> 2), "the chroma view's address");
	expect((words[1] >> 60) == 1U, "the chroma view's one level");
}

/* Checks what a session offers of an engine's operations. */
static void
check_reports(void)
{
	uint32_t all;

	all = DRV_POWERVR_VIDEO_DECODE_H264 | DRV_POWERVR_VIDEO_DECODE_H265 | DRV_POWERVR_VIDEO_ENCODE_H265;
	expect(powervr_video_capset_flags(0) == 0, "no capset flag without an engine");
	expect(powervr_video_capset_flags(DRV_POWERVR_VIDEO_DECODE_H265) == 0, "no capset flag for H.265 alone");
	expect(powervr_video_capset_flags(DRV_POWERVR_VIDEO_DECODE_H264) == 1U, "the H.264 decode capset flag");
	expect(powervr_video_queue_flags(0) == 0, "no queue flag without an engine");
	expect(powervr_video_queue_flags(DRV_POWERVR_VIDEO_DECODE_H265) == VK_QUEUE_VIDEO_DECODE_BIT_KHR, "the decode queue flag");
	expect(powervr_video_queue_flags(all) == (VK_QUEUE_VIDEO_DECODE_BIT_KHR | 0x40U), "the decode and encode queue flags");
	expect(powervr_video_codec_operations(all) == (VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR | VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR | VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR), "the codec operations");
	expect(powervr_video_codec_operations(0) == 0, "no codec without an engine");
	expect(powervr_video_format_features(DRV_POWERVR_VIDEO_ENCODE_H265) == 0, "no decode features for an encoder");
	expect(powervr_video_format_features(DRV_POWERVR_VIDEO_DECODE_H264) == (VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR | VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR), "the decode features");
	expect(powervr_video_image_usages(DRV_POWERVR_VIDEO_DECODE_H264) == (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR), "the decode usages");
	expect(powervr_video_image_usages(0) == 0, "no decode usage without an engine");
}

/* Checks one query's record in each of its forms. */
static void
check_records(void)
{
	static const uint8_t status32[4] = { 1, 0, 0, 0 };
	static const uint8_t failed32_available[8] = { 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0 };
	static const uint8_t failed64_available[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0, 0, 0, 0, 0 };
	uint8_t record[POWERVR_VIDEO_QUERY_RECORD_MAX];
	uint32_t bytes;

	memset(record, 0xAA, sizeof(record));
	bytes = powervr_video_query_record(1, 1, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR, record);
	expect(bytes == 4 && memcmp(record, status32, 4) == 0, "a 32-bit status alone");
	bytes = powervr_video_query_record(-1, 7, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, record);
	expect(bytes == 8 && memcmp(record, failed32_available, 8) == 0, "a 32-bit failure and its availability");
	bytes = powervr_video_query_record(-1, 1, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_64_BIT, record);
	expect(bytes == 16 && memcmp(record, failed64_available, 16) == 0, "a 64-bit failure sign-extended and its availability");
}

int
main(void)
{
	check_layouts();
	check_views();
	check_reports();
	check_records();
	printf("powervr nv12 state host test: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
