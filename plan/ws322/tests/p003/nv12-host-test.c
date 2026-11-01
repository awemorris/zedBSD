/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host check of the PowerVR executor's NV12 images, plane views, plane
 * copies, external memory queries, result status queries and the video
 * engine's reports: frames written by libvulkan's own encoders
 * (nv12-frames.c) go through the executor with the render node replaced
 * by the executor host check's stand-ins (plan/tools/powervr/p009/
 * vk-host-test.c, included here with its main renamed, so its whole
 * check runs first as the regression of the rest of the executor).  The
 * transfers the executor asks for are kept and compared.
 *
 *   nv12-host-test DIR    (DIR holds the p009 check's fill.spv and reject-size.spv)
 */

#define main p009_main
#define powervr_transfer_run_async p009_transfer_run_async
#include "vk-host-test.c"
#undef powervr_transfer_run_async
#undef main

#include <libc/vulkan/vulkan_video.h>

#include "drivers/gpu/powervr/video-state.h"
#include "nv12-frames.h"

/* The most transfers of the command buffers the check keeps. */
#define NV12_TRANSFERS		8U

/* The transfers of a command buffer the executor asked for, in order. */
static struct powervr_transfer_request nv12_transfers[NV12_TRANSFERS];
static struct powervr_view_shape nv12_sources[NV12_TRANSFERS];
static struct powervr_image_layout nv12_source_layouts[NV12_TRANSFERS];
static unsigned nv12_transfer_count;

int powervr_transfer_run_async(struct powervr_render *render, struct powervr_session *session, const struct powervr_transfer_request *request);

/* The command buffers' transfers, kept with copies of their source views, then handed to the p009 check's own stand-in. */
int
powervr_transfer_run_async(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct powervr_transfer_request *request)
{
	if (nv12_transfer_count < NV12_TRANSFERS) {
		nv12_transfers[nv12_transfer_count] = *request;
		if (request->source != NULL) {
			nv12_sources[nv12_transfer_count] = *request->source;
			nv12_source_layouts[nv12_transfer_count] = *request->source->layout;
		}
		nv12_transfer_count++;
	}
	return p009_transfer_run_async(render, session, request);
}

/* A fake engine's operations: the word its engine pointer names. */
static uint32_t
engine_operations(
	void *engine)
{
	return *(const uint32_t *)engine;
}

/* Runs one frame and checks it ran. */
static void
run(
	struct powervr_session *session,
	uint8_t *frame,
	size_t bytes,
	const char *what)
{
	int error;

	error = powervr_vk_execute(session->vk, frame, (uint32_t)bytes);
	free(frame);
	check(error == 0, what);
}

/* Opens a session over the node with the video operations it latched, and runs the first frame. */
static struct powervr_resource *
open_session(
	struct powervr_render *render,
	struct powervr_session *session,
	uint32_t video_operations)
{
	struct powervr_buffer *buffer;
	struct powervr_resource *reply;
	uint8_t *frame;
	size_t bytes;
	int error;

	session->render = render;
	session->video_operations = video_operations;
	error = powervr_vk_open(session, &session->vk);
	check(error == 0, "nv12: executor opens");
	powervr_buffer_create(render, FRAME_REPLY_BYTES, &buffer);
	reply = make_blob(session, buffer);
	bytes = frame_setup(reply->identifier, &frame);
	run(session, frame, bytes, "nv12: setup frame runs");
	return reply;
}

/* Checks the reports of a session without video and of one whose engine decodes H.264. */
static void
check_reports(
	struct powervr_render *render)
{
	static struct powervr_session plain;
	static struct powervr_session video;
	struct powervr_resource *reply;
	struct nv12_expect expect;
	uint8_t *frame;
	size_t bytes;

	/* Without an engine: no video flag, no codec, NV12 only copied out. */
	reply = open_session(render, &plain, 0);
	memset(&expect, 0, sizeof(expect));
	expect.nv12_optimal_features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	bytes = nv12_frame_reports(reply->identifier, &frame);
	run(&plain, frame, bytes, "nv12: report frame runs without video");
	nv12_replies_reports((uint8_t *)reply->buffer->view, &expect);

	/* With an H.264 decoder: the decode flag, H.264, NV12 decoded into and referenced. */
	reply = open_session(render, &video, DRV_POWERVR_VIDEO_DECODE_H264);
	expect.queue_flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
	expect.codecs = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	expect.nv12_optimal_features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_VIDEO_DECODE_OUTPUT_BIT_KHR | VK_FORMAT_FEATURE_VIDEO_DECODE_DPB_BIT_KHR;
	expect.video_usage_allowed = 1;
	bytes = nv12_frame_reports(reply->identifier, &frame);
	run(&video, frame, bytes, "nv12: report frame runs with video");
	nv12_replies_reports((uint8_t *)reply->buffer->view, &expect);
}

/* Checks the image, its views, the set written with them, the plane copies and the queries. */
static void
check_images(
	struct powervr_render *render)
{
	static struct powervr_session session;
	static const uint8_t complete_with_availability[8] = { 0, 0, 0, 0, 1, 0, 0, 0 };
	static const uint8_t failed_64[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0, 0, 0, 0, 0 };
	static const uint8_t unavailable[4] = { 0, 0, 0, 0 };
	struct powervr_resource *reply;
	struct powervr_vk_image *image;
	struct powervr_vk_image_view *luma;
	struct powervr_vk_image_view *chroma;
	struct powervr_vk_descriptor_set *set;
	struct powervr_view_shape shape;
	uint64_t expected_words[2];
	uint64_t image_bytes;
	uint64_t address;
	uint8_t *frame;
	size_t bytes;
	int error;

	/* The images and their requirements. */
	reply = open_session(render, &session, 0);
	bytes = nv12_frame_image(reply->identifier, &frame);
	run(&session, frame, bytes, "nv12: image frame runs");
	image_bytes = 0;
	nv12_replies_image((uint8_t *)reply->buffer->view, &image_bytes);

	/* The planes the executor laid out. */
	image = powervr_vk_payload(session.vk, I915_VK_OBJ_IMAGE, NV12_ID_IMAGE);
	check(image != NULL && image->plane_count == 2, "nv12: two planes");
	if (image == NULL)
		return;
	check(image->planes[0].format->vk_format == VK_FORMAT_R8_UNORM && image->planes[1].format->vk_format == VK_FORMAT_R8G8_UNORM, "nv12: R8 and R8G8 planes");
	check(image->planes[1].width == NV12_WIDTH / 2U && image->planes[1].height == NV12_HEIGHT / 2U, "nv12: a half-size chroma plane");

	/* The memory, the layouts, the views and the set. */
	bytes = nv12_frame_memory(reply->identifier, image_bytes, &frame);
	run(&session, frame, bytes, "nv12: memory frame runs");
	nv12_replies_memory((uint8_t *)reply->buffer->view);

	/* Each view's words are those of its plane as a linear image of its own, and its descriptor's format and layers are the plane's. */
	luma = powervr_vk_payload(session.vk, I915_VK_OBJ_IMAGE_VIEW, NV12_ID_LUMA);
	chroma = powervr_vk_payload(session.vk, I915_VK_OBJ_IMAGE_VIEW, NV12_ID_CHROMA);
	error = powervr_vk_image_address(image, &address);
	check(error == 0 && luma != NULL && chroma != NULL, "nv12: the views exist over a bound image");
	if (error != 0 || luma == NULL || chroma == NULL)
		return;
	memset(&shape, 0, sizeof(shape));
	shape.format = image->planes[0].format;
	shape.layout = &image->planes[0].layout;
	shape.address = address;
	shape.width = NV12_WIDTH;
	shape.height = NV12_HEIGHT;
	shape.image_levels = 1;
	shape.level_count = 1;
	shape.layer_count = 1;
	shape.linear = true;
	powervr_view_words(&shape, expected_words);
	check(memcmp(luma->words, expected_words, sizeof(expected_words)) == 0, "nv12: the luma view's words");
	check(luma->format->vk_format == VK_FORMAT_R8_UNORM && luma->layer_bytes == image->planes[0].layout.layer_bytes, "nv12: the luma view's format and layers");
	shape.format = image->planes[1].format;
	shape.layout = &image->planes[1].layout;
	shape.address = address + image->planes[1].offset;
	shape.width = NV12_WIDTH / 2U;
	shape.height = NV12_HEIGHT / 2U;
	powervr_view_words(&shape, expected_words);
	check(memcmp(chroma->words, expected_words, sizeof(expected_words)) == 0, "nv12: the chroma view's words, at the plane's offset");
	check(chroma->format->vk_format == VK_FORMAT_R8G8_UNORM, "nv12: the chroma view's format");
	set = powervr_vk_payload(session.vk, I915_VK_OBJ_DESCRIPTOR_SET, NV12_ID_SET);
	check(set != NULL && set->bytes != NULL && memcmp(set->bytes, luma->words, sizeof(luma->words)) == 0, "nv12: binding 0's descriptor holds the luma view's words");

	/* The commands: the queries and the plane copies. */
	nv12_transfer_count = 0;
	kick_recording = 1;
	bytes = nv12_frame_commands(reply->identifier, &frame);
	run(&session, frame, bytes, "nv12: command frame runs");
	kick_recording = 0;
	nv12_replies_commands((uint8_t *)reply->buffer->view);

	/* The two copies: each plane's rows read as 32-bit words (the transfer copies no smaller raw texel), into the buffer's tight rows. */
	check(nv12_transfer_count == 2, "nv12: one transfer a plane");
	if (nv12_transfer_count != 2)
		return;
	check(nv12_sources[0].format->texel_bytes == 4U && nv12_sources[0].address == address && nv12_sources[0].linear, "nv12: the luma plane's copy reads the luma plane as words");
	check(nv12_source_layouts[0].row_pitch == NV12_WIDTH && nv12_source_layouts[0].padded_width == NV12_WIDTH / 4U, "nv12: the luma plane's rows as words");
	check(nv12_sources[0].width == NV12_WIDTH / 4U && nv12_sources[0].height == NV12_HEIGHT, "nv12: the luma plane's words");
	check(nv12_transfers[0].destination.format->texel_bytes == 4U && nv12_transfers[0].destination.row_pitch == NV12_WIDTH, "nv12: the luma plane's buffer rows");
	check(nv12_transfers[0].area.width == NV12_WIDTH / 4U && nv12_transfers[0].area.height == NV12_HEIGHT, "nv12: the luma plane's area in words");
	check(nv12_sources[1].format->texel_bytes == 4U && nv12_sources[1].address == address + image->planes[1].offset, "nv12: the chroma plane's copy reads the chroma plane");
	check(nv12_sources[1].width == NV12_WIDTH / 4U && nv12_source_layouts[1].row_pitch == NV12_WIDTH, "nv12: the chroma plane's pairs as words");
	check(nv12_transfers[1].destination.row_pitch == NV12_WIDTH, "nv12: the chroma plane's buffer rows");
	check(nv12_transfers[1].destination.address == nv12_transfers[0].destination.address + (uint64_t)NV12_WIDTH * NV12_HEIGHT, "nv12: the chroma plane after the luma plane in the buffer");
	check(nv12_transfers[1].area.width == NV12_WIDTH / 4U && nv12_transfers[1].area.height == NV12_HEIGHT / 2U, "nv12: the chroma plane's area in words");
	check(session.vk->query_pool == NULL, "nv12: no query is open after the end");

	/* Query 0: ended with no decode, available and not ready (status 0) with its availability. */
	bytes = nv12_frame_results(reply->identifier, 0, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 8, 8, &frame);
	run(&session, frame, bytes, "nv12: results frame runs");
	nv12_replies_results((uint8_t *)reply->buffer->view, VK_SUCCESS, complete_with_availability, 8, "nv12: query 0 available");

	/* Query 1: reset and never begun, unavailable. */
	bytes = nv12_frame_results(reply->identifier, 1, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR, 4, 4, &frame);
	run(&session, frame, bytes, "nv12: results frame runs");
	nv12_replies_results((uint8_t *)reply->buffer->view, VK_NOT_READY, unavailable, 4, "nv12: query 1 not ready");

	/* A decode's reports into an open query 2 (not ended): a failure stays over a later success; a 64-bit word. */
	session.vk->query_pool = powervr_vk_object_find(session.vk, I915_VK_OBJ_QUERY_POOL, NV12_ID_POOL);
	session.vk->query = 2;
	powervr_vk_query_report(session.vk, 1);
	powervr_vk_query_report(session.vk, -1);
	powervr_vk_query_report(session.vk, 1);
	session.vk->query_pool = NULL;
	bytes = nv12_frame_results(reply->identifier, 2, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_64_BIT, 8, 8, &frame);
	run(&session, frame, bytes, "nv12: results frame runs");
	nv12_replies_results((uint8_t *)reply->buffer->view, VK_NOT_READY, failed_64, 8, "nv12: a decode's failure in query 2, not yet ended");
}

/* Checks the engine's registration. */
static void
check_registration(void)
{
	static const struct drv_powervr_video_ops empty = { .operations = NULL };
	static const struct drv_powervr_video_ops ops = { .operations = engine_operations };
	static uint32_t first = DRV_POWERVR_VIDEO_DECODE_H264 | 0x80U;
	static uint32_t second = DRV_POWERVR_VIDEO_DECODE_H265;
	int error;

	check(powervr_video_operations() == 0, "nv12: no engine, no video");
	error = drv_powervr_video_register(&empty, &first);
	check(error == EINVAL, "nv12: a table without its operations is refused");
	error = drv_powervr_video_register(&ops, &first);
	check(error == 0, "nv12: the engine registers");
	check(powervr_video_operations() == DRV_POWERVR_VIDEO_DECODE_H264, "nv12: the engine's operations, of those the node knows");
	error = drv_powervr_video_register(&ops, &second);
	check(error == EBUSY && powervr_video_operations() == DRV_POWERVR_VIDEO_DECODE_H264, "nv12: a second engine is refused and changes nothing");
}

int
main(
	int argc,
	char **argv)
{
	static struct powervr_core core;
	static struct powervr_render render;
	int frame_passed;
	int frame_failed;
	int status;

	/* The executor host check first: the rest of the executor is unchanged. */
	status = p009_main(argc, argv);
	check(status == 0, "nv12: the executor host check passes");

	/* The NV12, query and video report checks over a node of their own. */
	render.core = &core;
	check_registration();
	check_reports(&render);
	check_images(&render);

	nv12_frame_counts(&frame_passed, &frame_failed);
	passes += frame_passed;
	failures += frame_failed;
	printf("powervr nv12 host test: %d passed, %d failed\n", passes, failures);
	return failures == 0 ? 0 : 1;
}
