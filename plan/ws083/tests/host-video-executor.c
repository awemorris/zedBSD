/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor half of the video round trip (ws083-p003b).
 *
 * Feeds the streams host-video-wire.c wrote with libvulkan's own encoders
 * to the i915 Vulkan executor (linked as the kernel builds it, with the
 * services of i915-vk-render-stubs.inc), and checks the replies, the
 * capset's native word, the families, the slot rules a submission is held
 * to (refused with VK_ERROR_DEVICE_LOST) and the bitstream checks that skip
 * a picture; and the result status queries of decodes (ws083-p008), whose
 * pool and query records are built here by hand.
 */

#include "../../ws031/tests/i915-vk-render-stubs.inc"

#include "../../../src/drivers/gpu/i915/render/instance.h"
#include "../../../src/drivers/gpu/i915/render/video.h"

#define FIXTURE_QUEUE_SUBMIT			18U
#define FIXTURE_ALLOCATE_MEMORY			21U
#define FIXTURE_CREATE_BUFFER			50U
#define FIXTURE_BIND_BUFFER_MEMORY		28U
#define FIXTURE_GET_DEVICE_QUEUE2		155U
#define FIXTURE_CREATE_COMMAND_POOL		85U
#define FIXTURE_ALLOCATE_COMMAND_BUFFERS	88U
#define FIXTURE_BEGIN_COMMAND_BUFFER		90U
#define FIXTURE_END_COMMAND_BUFFER		91U
#define FIXTURE_CREATE_QUERY_POOL		47U
#define FIXTURE_GET_QUERY_POOL_RESULTS		49U
#define FIXTURE_CMD_BEGIN_QUERY			127U
#define FIXTURE_CMD_END_QUERY			128U
#define FIXTURE_CMD_RESET_QUERY_POOL		129U

/* VkQueryResultStatusKHR (vulkan_video.h, which the executor's headers do not hold). */
#define FIXTURE_STATUS_COMPLETE			1
#define FIXTURE_STATUS_ERROR			(-1)

/* The identities shared with host-video-wire.c, and the fixture's own. */
#define FIXTURE_DEVICE		0xd0ULL
#define FIXTURE_QUEUE		0xd1ULL
#define FIXTURE_VIDEO_QUEUE	0xd2ULL
#define FIXTURE_MEMORY		0x100ULL
#define FIXTURE_BUFFER		0x200ULL
#define FIXTURE_IMAGE_A		0x300ULL
#define FIXTURE_IMAGE_B		0x301ULL
#define FIXTURE_VIEW_A		0x400ULL
#define FIXTURE_VIEW_B		0x401ULL
#define FIXTURE_POOL		0x900ULL
#define FIXTURE_CMDBUF		0xa00ULL
#define FIXTURE_STATUS_POOL	0xb00ULL
#define FIXTURE_OCCLUSION_POOL	0xb01ULL

/* The storage of the memory and where its parts are. */
#define FIXTURE_STORAGE_BYTES	262144U
#define FIXTURE_STORAGE_VA	0x200000000ULL
#define FIXTURE_BUFFER_OFFSET	0x20000U
#define FIXTURE_IMAGE_A_OFFSET	0x10000U
#define FIXTURE_IMAGE_B_OFFSET	0x18000U

/* The memory's storage, the blob that is it, and the wire of each stream. */
static uint8_t fixture_storage[FIXTURE_STORAGE_BYTES] __attribute__((aligned(4096)));
static struct i915_gem_object fixture_storage_object;
static struct stub_wire fixture_wire;

/* The directory host-video-wire.c wrote into. */
static const char *fixture_directory;

static void fixture_append(const char *name);
static size_t fixture_run(const char *name);
static void fixture_queue(uint64_t identity, uint32_t family);
static void fixture_objects(void);
static void fixture_picture(uint64_t image_identity, uint64_t view_identity, uint64_t offset);
static void fixture_record(const char *const *names, unsigned count);
static uint32_t fixture_submit(uint64_t queue);
static void test_queries(void);
static void test_session(void);
static void test_submissions(void);
static void fixture_query_pool(uint64_t identity, uint32_t type);
static void fixture_query_record(const char *name, uint32_t opcode, uint64_t pool, uint32_t first, uint32_t count);
static uint32_t fixture_statuses(int64_t *statuses);
static uint32_t fixture_status32(uint32_t *status);
static void test_status_queries(void);
static void fixture_batch_expect(void);
static void test_layouts(void);
static void fixture_layout(uint32_t width, uint32_t height, uint32_t pitch, uint32_t rows, uint64_t chroma_offset, uint64_t bytes);
static void fixture_check(FILE *expect, unsigned instruction, const char *field, unsigned long long value);
static void fixture_named(FILE *expect, unsigned instruction, const char *name);

/* Runs the round trip. */
int
main(
	int argc,
	char **argv)
{
	unsigned long mark;

	/* The streams' directory. */
	assert(argc == 2);
	fixture_directory = argv[1];

	/* A device whose boot asked for video and whose GT has a usable VCS0. */
	mark = stub_allocation_mark();
	stub_video_state = 0;
	stub_objects_on = 1;
	drv_i915_render_video_request(1);
	memset(&fixture_storage_object, 0, sizeof(fixture_storage_object));
	fixture_storage_object.slot = 7U;
	fixture_storage_object.bytes = sizeof(fixture_storage);
	fixture_storage_object.run.paddr = (hal_physaddr_t)(uintptr_t)fixture_storage;
	fixture_storage_object.va = FIXTURE_STORAGE_VA;
	stub_session_open(&fixture_storage_object);

	/* The capset carries the native word, so libvulkan names the video extensions. */
	assert(stub_vk->video == 1);
	assert(stub_vk->capset_bytes == 176U);
	assert(stub_vk->capset[42] == 0x5a4e4154U && stub_vk->capset[43] == 1U);

	/* The scenarios. */
	test_queries();
	test_session();
	test_status_queries();
	test_submissions();
	test_layouts();

	/* Closing frees every video object and record; nothing is left. */
	stub_session_close();
	assert(stub_live_since(mark) == 0U);
	printf("ws083 video executor host test PASS\n");
	return 0;
}

/* Appends one stream host-video-wire.c wrote. */
static void
fixture_append(
	const char *name)
{
	char path[512];
	FILE *file;
	size_t read;

	/* <directory>/<name>.bin, after what the wire holds. */
	snprintf(path, sizeof(path), "%s/%s.bin", fixture_directory, name);
	file = fopen(path, "rb");
	assert(file != NULL);
	read = fread(fixture_wire.bytes + fixture_wire.size, 1, sizeof(fixture_wire.bytes) - fixture_wire.size, file);
	assert(read > 0U);
	fixture_wire.size += read;
	assert(fclose(file) == 0);
}

/* Runs one stream alone and reports the length of its reply. */
static size_t
fixture_run(
	const char *name)
{
	size_t reply_bytes;

	/* The reply selector, then the stream. */
	stub_wire_begin(&fixture_wire);
	fixture_append(name);
	reply_bytes = stub_execute_ok(&fixture_wire);
	return reply_bytes;
}

/* vkGetDeviceQueue2 of a queue of a family, as device.c sends it. */
static void
fixture_queue(
	uint64_t identity,
	uint32_t family)
{
	size_t reply_bytes;

	/* [device][present][sType][link][timeline sType][no link][timeline][flags][family][index][present][identity]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_DEVICE_QUEUE2);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1000145003U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 1000384005U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, family + 1U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, family);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 20U);
	assert(stub_get64(stub_reply, 12U) == identity);
}

/* Inserts an NV12 64x64 image in the memory and a view of it, as image.c will make them (p004). */
static void
fixture_picture(
	uint64_t image_identity,
	uint64_t view_identity,
	uint64_t offset)
{
	struct i915_gfx_image *image;
	struct i915_gfx_view *view;
	int error;

	/* The image: NV12, 64x64, laid out as image.c lays a decoder's picture out (pitch 128, CbCr from row 64), bound at the offset. */
	image = kern_calloc(1U, sizeof(*image));
	assert(image != NULL);
	image->format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	image->width = 64U;
	image->height = 64U;
	image->levels = 1U;
	image->layers = 1U;
	image->samples = 1U;
	image->type = VK_IMAGE_TYPE_2D;
	error = drv_i915_gfx_image_layout(image);
	assert(error == 0);
	assert(image->planar == 1U && image->pitch == 128U && image->chroma_rows == 64U);
	assert(image->chroma_offset == 128U * 64U && image->bytes == 3U * 4096U);
	image->memory = drv_i915_object_lookup(stub_session, I915_VK_OBJ_MEMORY, FIXTURE_MEMORY);
	image->offset = offset;
	assert(image->memory != NULL);
	error = drv_i915_object_insert(stub_session, I915_VK_OBJ_IMAGE, image_identity, image);
	assert(error == 0);

	/* The view of the whole image. */
	view = kern_calloc(1U, sizeof(*view));
	assert(view != NULL);
	view->image = image;
	view->format = image->format;
	view->level_count = 1U;
	error = drv_i915_object_insert(stub_session, I915_VK_OBJ_IMAGE_VIEW, view_identity, view);
	assert(error == 0);
}

/* Makes the memory with its storage, the bitstream buffer, the two pictures, the queues and the command buffer. */
static void
fixture_objects(void)
{
	size_t reply_bytes;
	int error;

	/* vkAllocateMemory of the storage, then vkCreateBuffer of 4 KiB and its bind. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 5U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_STORAGE_BYTES);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put32(&fixture_wire, FIXTURE_CREATE_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 12U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 4096U);
	stub_put32(&fixture_wire, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	stub_put32(&fixture_wire, VK_SHARING_MODE_EXCLUSIVE);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	stub_put32(&fixture_wire, FIXTURE_BIND_BUFFER_MEMORY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_BUFFER);
	stub_put64(&fixture_wire, FIXTURE_MEMORY);
	stub_put64(&fixture_wire, FIXTURE_BUFFER_OFFSET);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 2U * 24U + 8U);
	error = drv_i915_render_blob_attach(stub_vk, &stub_gpu, FIXTURE_MEMORY, &fixture_storage_object);
	assert(error == 0);

	/* The bitstream: start codes at the two slices, 0 and 64 (the second a 4-byte one). */
	memset(fixture_storage + FIXTURE_BUFFER_OFFSET, 0x55, 128U);
	fixture_storage[FIXTURE_BUFFER_OFFSET + 0U] = 0U;
	fixture_storage[FIXTURE_BUFFER_OFFSET + 1U] = 0U;
	fixture_storage[FIXTURE_BUFFER_OFFSET + 2U] = 1U;
	fixture_storage[FIXTURE_BUFFER_OFFSET + 64U] = 0U;
	fixture_storage[FIXTURE_BUFFER_OFFSET + 65U] = 0U;
	fixture_storage[FIXTURE_BUFFER_OFFSET + 66U] = 0U;
	fixture_storage[FIXTURE_BUFFER_OFFSET + 67U] = 1U;

	/* The two pictures. */
	fixture_picture(FIXTURE_IMAGE_A, FIXTURE_VIEW_A, FIXTURE_IMAGE_A_OFFSET);
	fixture_picture(FIXTURE_IMAGE_B, FIXTURE_VIEW_B, FIXTURE_IMAGE_B_OFFSET);

	/* A graphics queue and a video queue; a family past the two is refused. */
	fixture_queue(FIXTURE_QUEUE, 0U);
	fixture_queue(FIXTURE_VIDEO_QUEUE, 1U);
	assert(drv_i915_render_queue_family(stub_session, FIXTURE_QUEUE) == 0U);
	assert(drv_i915_render_queue_family(stub_session, FIXTURE_VIDEO_QUEUE) == 1U);

	/* vkCreateCommandPool and one primary command buffer. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_COMMAND_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 39U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, FIXTURE_ALLOCATE_COMMAND_BUFFERS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 40U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, FIXTURE_POOL);
	stub_put32(&fixture_wire, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CMDBUF);
	(void)stub_execute_ok(&fixture_wire);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_COMMAND_BUFFER, FIXTURE_CMDBUF) != NULL);
}

/* Records the named streams into the command buffer between a begin and an end. */
static void
fixture_record(
	const char *const *names,
	unsigned count)
{
	size_t reply_bytes;
	unsigned index;

	/* vkBeginCommandBuffer, the recordings, vkEndCommandBuffer. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_BEGIN_COMMAND_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CMDBUF);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 42U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	for (index = 0U; index < count; index++)
		fixture_append(names[index]);
	stub_put32(&fixture_wire, FIXTURE_END_COMMAND_BUFFER);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CMDBUF);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 12U) == VK_SUCCESS);
}

/* Submits the command buffer on a queue and reports the submission's VkResult. */
static uint32_t
fixture_submit(
	uint64_t queue)
{
	size_t reply_bytes;

	/* One VkSubmitInfo with the command buffer, no semaphores, no fence. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_QUEUE_SUBMIT);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, queue);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, 4U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_CMDBUF);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 8U);
	return stub_get32(stub_reply, 4U);
}

/* The physical-device queries libvulkan sends: capabilities and formats. */
static void
test_queries(void)
{
	size_t reply_bytes;
	size_t at;

	/* caps: VK_SUCCESS, then capabilities, decode and H.264 records nested in the asked order. */
	reply_bytes = fixture_run("caps");
	assert(stub_get32(stub_reply, 0U) == GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 8U) == 1U);
	assert(stub_get32(stub_reply, 16U) == VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR);
	assert(stub_get64(stub_reply, 20U) == 1U);
	assert(stub_get32(stub_reply, 28U) == VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR);
	assert(stub_get64(stub_reply, 32U) == 1U);
	assert(stub_get32(stub_reply, 40U) == VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR);
	assert(stub_get64(stub_reply, 44U) == 0U);
	assert(stub_get32(stub_reply, 52U) == 14U);
	at = 64U;
	assert(stub_get32(stub_reply, at) == 1U);
	at += 4U;
	assert(stub_get32(stub_reply, at) == 2U);
	assert(stub_get64(stub_reply, at + 4U) == 32U);
	assert(stub_get64(stub_reply, at + 12U) == 1U);
	assert(stub_get32(stub_reply, at + 36U) == 4096U && stub_get32(stub_reply, at + 40U) == 4096U);
	assert(stub_get32(stub_reply, at + 44U) == 17U && stub_get32(stub_reply, at + 48U) == 16U);
	assert(stub_get64(stub_reply, at + 52U) == 256U);
	assert(memcmp(stub_reply + at + 60U, "VK_STD_vulkan_video_codec_h264_decode", 38U) == 0);
	assert(stub_get32(stub_reply, at + 60U + 256U) == (1U << 22));
	assert(reply_bytes == at + 60U + 256U + 4U);

	/* format: one NV12 optimal format for decode output and reference. */
	reply_bytes = fixture_run("format");
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 16U) == 1U && stub_get64(stub_reply, 20U) == 1U);
	assert(stub_get32(stub_reply, 28U) == VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR);
	assert(stub_get32(stub_reply, 40U) == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	assert(stub_get32(stub_reply, 68U) == VK_IMAGE_TILING_OPTIMAL);
	assert(stub_get32(stub_reply, 72U) == (VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR));
	assert(reply_bytes == 76U);
}

/* The session, its memory and its parameters. */
static void
test_session(void)
{
	size_t reply_bytes;
	unsigned attaches;
	unsigned index;

	/* The objects the commands name. */
	fixture_objects();

	/* session: created under its identity, attaching the VCS0 context once. */
	attaches = stub_video_attaches;
	reply_bytes = fixture_run("session");
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(drv_i915_object_lookup(stub_session, I915_VK_OBJ_VIDEO_SESSION, stub_get64(stub_reply, 16U)) != NULL);
	assert(stub_video_attaches == attaches + 1U);

	/* requirements: four row stores and four motion vector buffers (three slots and one spare), a page each. */
	reply_bytes = fixture_run("requirements");
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get32(stub_reply, 16U) == 8U);
	for (index = 0U; index < 8U; index++) {
		assert(stub_get32(stub_reply, 28U + index * 36U) == VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR);
		assert(stub_get32(stub_reply, 28U + index * 36U + 12U) == index);
		assert(stub_get64(stub_reply, 28U + index * 36U + 16U) == 4096U);
		assert(stub_get64(stub_reply, 28U + index * 36U + 24U) == 4096U);
		assert(stub_get32(stub_reply, 28U + index * 36U + 32U) == 1U);
	}
	assert(reply_bytes == 28U + 8U * 36U);

	/* bind: every binding a page of the memory. */
	reply_bytes = fixture_run("bind");
	assert(reply_bytes == 8U && stub_get32(stub_reply, 4U) == VK_SUCCESS);

	/* parameters and update: SPS 0 with PPS (0,0), then PPS (0,1) as update 1. */
	reply_bytes = fixture_run("parameters");
	assert(reply_bytes == 24U && stub_get32(stub_reply, 4U) == VK_SUCCESS);
	reply_bytes = fixture_run("update");
	assert(reply_bytes == 8U && stub_get32(stub_reply, 4U) == VK_SUCCESS);

	/* The same update again is not the next one and adds a key held: refused. */
	reply_bytes = fixture_run("update");
	assert(stub_get32(stub_reply, 4U) == (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
}

/* Submissions on the video family: the rules a submission is held to and the skipped pictures. */
static void
test_submissions(void)
{
	static const char *const idr[] = { "begin1", "control", "decode1", "end", "begin2", "decode2", "end" };
	static const char *const stale[] = { "begin2", "decode3", "end" };
	static const char *const reference[] = { "begin2", "decode2", "end" };
	static const char *const deactivate[] = { "begin3", "end" };
	static const char *const open_scope[] = { "begin1", "control" };
	struct i915_gfx_image *image;
	uint32_t result;

	/* A reset, a reference IDR into slot 0, then a P picture reading it: every picture decodes, each in its own run on VCS0. */
	fixture_record(idr, 7U);
	stub_batch_count = 0U;
	stub_batch_runs = 0U;
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	assert(strstr(stub_log, "skip decode") == NULL);
	assert(stub_batch_runs == 2U);
	assert(stub_batch_engine == I915_ENGINE_VCS0);
	fixture_batch_expect();

	/* The same on the graphics family breaks the rules. */
	result = fixture_submit(FIXTURE_QUEUE);
	assert(result == (uint32_t)VK_ERROR_DEVICE_LOST);

	/* A picture reading slot 1, which the non-reference P picture left inactive, breaks the rules. */
	fixture_record(stale, 3U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "submission refused") == NULL);

	/* A picture reading slot 0, which still holds the IDR picture, decodes. */
	fixture_record(reference, 3U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);

	/* A second slice without its start code skips the IDR picture, and the submission succeeds. */
	fixture_storage[FIXTURE_BUFFER_OFFSET + 67U] = 0x55U;
	fixture_record(idr, 7U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	assert(strstr(stub_log, "skip decode: slice without a start code") != NULL);
	fixture_storage[FIXTURE_BUFFER_OFFSET + 67U] = 1U;

	/* An output that is not NV12 skips the picture. */
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE_A);
	assert(image != NULL);
	image->format = VK_FORMAT_R8G8B8A8_UNORM;
	fixture_record(idr, 7U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	assert(strstr(stub_log, "skip decode: picture not NV12") != NULL);
	image->format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

	/* A reference laid out unlike the output skips the picture that reads it. */
	image = drv_i915_object_lookup(stub_session, I915_VK_OBJ_IMAGE, FIXTURE_IMAGE_B);
	assert(image != NULL);
	image->pitch = 256U;
	fixture_record(reference, 3U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	assert(strstr(stub_log, "skip decode: reference laid out unlike the output") != NULL);
	image->pitch = 128U;

	/* A begin without a picture deactivates slot 0; reading it afterwards breaks the rules. */
	fixture_record(deactivate, 2U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	fixture_record(reference, 3U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == (uint32_t)VK_ERROR_DEVICE_LOST);

	/* A coding scope left open at the end of the command buffer breaks the rules. */
	fixture_record(open_scope, 2U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "coding scope is not ended") != NULL);

	/* A hung video engine loses every later video submission. */
	fixture_record(idr, 7U);
	stub_video_state = EIO;
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == (uint32_t)VK_ERROR_DEVICE_LOST);
	stub_video_state = 0;
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);

	/* A decode that hangs loses the device and quarantines the session (ws083-p007). */
	stub_batch_run_error = ETIMEDOUT;
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(stub_gpu.quarantined == 1U);
	assert(strstr(stub_log, "session quarantined") != NULL);

	/* destroy_parameters and destroy_session withdraw both; the quarantined session's batch is retained. */
	(void)fixture_run("destroy_parameters");
	assert(stub_gem_retained == 0U);
	(void)fixture_run("destroy_session");
	assert(stub_gem_retained == 1U);
}

/* vkCreateQueryPool of two queries of a type under an identity, as query.c sends it. */
static void
fixture_query_pool(
	uint64_t identity,
	uint32_t type)
{
	size_t reply_bytes;

	/* [device][present][sType][pNext][flags][queryType][queryCount][statistics][pAllocator][present][identity]. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_CREATE_QUERY_POOL);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, 1U);
	stub_put32(&fixture_wire, VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
	stub_put64(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, type);
	stub_put32(&fixture_wire, 2U);
	stub_put32(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 0U);
	stub_put64(&fixture_wire, 1U);
	stub_put64(&fixture_wire, identity);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 24U);
	assert(stub_get32(stub_reply, 4U) == VK_SUCCESS);
	assert(stub_get64(stub_reply, 16U) == identity);
}

/*
 * Writes one recorded query command into a stream file, as commands.c
 * records it: vkCmdBeginQuery [pool][query][flags], vkCmdEndQuery
 * [pool][query], vkCmdResetQueryPool [pool][first][count].
 */
static void
fixture_query_record(
	const char *name,
	uint32_t opcode,
	uint64_t pool,
	uint32_t first,
	uint32_t count)
{
	uint8_t bytes[32];
	char path[512];
	FILE *file;
	size_t size;

	/* The record's header: opcode, no reply, the command buffer; then the pool and the query. */
	memset(bytes, 0, sizeof(bytes));
	memcpy(bytes, &opcode, 4);
	memcpy(bytes + 8, &(uint64_t){ FIXTURE_CMDBUF }, 8);
	memcpy(bytes + 16, &pool, 8);
	memcpy(bytes + 24, &first, 4);
	size = 28U;
	if (opcode != FIXTURE_CMD_END_QUERY) {
		memcpy(bytes + 28, &count, 4);
		size = 32U;
	}

	/* <directory>/<name>.bin */
	snprintf(path, sizeof(path), "%s/%s.bin", fixture_directory, name);
	file = fopen(path, "wb");
	assert(file != NULL);
	assert(fwrite(bytes, 1, size, file) == size);
	assert(fclose(file) == 0);
}

/* vkGetQueryPoolResults of the status pool's two queries in 64 bits: the statuses, and the call's result. */
static uint32_t
fixture_statuses(
	int64_t *statuses)
{
	size_t reply_bytes;

	/* [device][pool][first][count][dataSize][bytes][stride][flags]: two records of a status and an availability. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_QUERY_POOL_RESULTS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_STATUS_POOL);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 2U);
	stub_put64(&fixture_wire, 32U);
	stub_put64(&fixture_wire, 32U);
	stub_put64(&fixture_wire, 16U);
	stub_put32(&fixture_wire, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U + 32U);
	assert(stub_get64(stub_reply, 8U) == 32U);

	/* Each status where its query is available, 0 where not. */
	statuses[0] = 0;
	if (stub_get64(stub_reply, 24U) != 0U)
		statuses[0] = (int64_t)stub_get64(stub_reply, 16U);
	statuses[1] = 0;
	if (stub_get64(stub_reply, 40U) != 0U)
		statuses[1] = (int64_t)stub_get64(stub_reply, 32U);
	return stub_get32(stub_reply, 4U);
}

/* vkGetQueryPoolResults of the status pool's first query in 32 bits: its status word, and the call's result. */
static uint32_t
fixture_status32(
	uint32_t *status)
{
	size_t reply_bytes;

	/* [device][pool][first][count][dataSize][bytes][stride][flags]: one record of a status and an availability. */
	stub_wire_begin(&fixture_wire);
	stub_put32(&fixture_wire, FIXTURE_GET_QUERY_POOL_RESULTS);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, FIXTURE_DEVICE);
	stub_put64(&fixture_wire, FIXTURE_STATUS_POOL);
	stub_put32(&fixture_wire, 0U);
	stub_put32(&fixture_wire, 1U);
	stub_put64(&fixture_wire, 8U);
	stub_put64(&fixture_wire, 8U);
	stub_put64(&fixture_wire, 8U);
	stub_put32(&fixture_wire, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
	reply_bytes = stub_execute_ok(&fixture_wire);
	assert(reply_bytes == 16U + 8U);
	assert(stub_get32(stub_reply, 20U) == 1U);
	*status = stub_get32(stub_reply, 16U);
	return stub_get32(stub_reply, 4U);
}

/*
 * Result status queries (ws083-p008): COMPLETE for a decode that ran,
 * ERROR for a skipped one, unavailable after a reset; the rules a query is
 * held to on the video family, and its begin on the graphics family.
 */
static void
test_status_queries(void)
{
	static const char *const both[] = {
		"q_reset", "begin1", "control", "q_begin0", "decode1", "q_end0", "end",
		"begin2", "q_begin1", "decode2", "q_end1", "end"
	};
	static const char *const reset_only[] = { "q_reset" };
	static const char *const outside[] = { "q_begin0", "begin1", "control", "decode1", "end" };
	static const char *const unended[] = { "begin1", "control", "q_begin0", "decode1", "end" };
	static const char *const twice[] = { "begin1", "control", "q_begin0", "q_begin1", "decode1", "q_end1", "q_end0", "end" };
	static const char *const reset_inside[] = { "begin1", "control", "q_reset", "decode1", "end" };
	static const char *const other_end[] = { "begin1", "control", "q_begin0", "decode1", "q_end1", "end" };
	static const char *const occlusion[] = { "begin1", "control", "o_begin0", "decode1", "o_end0", "end" };
	static const char *const graphics[] = { "q_reset", "q_begin0", "q_end0" };
	static const char *const two_decodes[] = { "begin1", "control", "q_begin0", "decode1", "decode1", "q_end0", "end" };
	static const char *const past[] = { "q_past" };
	static const char *const reset_after[] = { "q_reset", "begin1", "control", "q_begin0", "decode1", "q_end0", "end", "q_reset" };
	int64_t statuses[2];
	uint32_t status32;
	uint32_t result;

	/* A result status pool of two queries on the video device, an occlusion pool beside it. */
	fixture_query_pool(FIXTURE_STATUS_POOL, VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR);
	fixture_query_pool(FIXTURE_OCCLUSION_POOL, VK_QUERY_TYPE_OCCLUSION);
	fixture_query_record("q_reset", FIXTURE_CMD_RESET_QUERY_POOL, FIXTURE_STATUS_POOL, 0U, 2U);
	fixture_query_record("q_begin0", FIXTURE_CMD_BEGIN_QUERY, FIXTURE_STATUS_POOL, 0U, 0U);
	fixture_query_record("q_end0", FIXTURE_CMD_END_QUERY, FIXTURE_STATUS_POOL, 0U, 0U);
	fixture_query_record("q_begin1", FIXTURE_CMD_BEGIN_QUERY, FIXTURE_STATUS_POOL, 1U, 0U);
	fixture_query_record("q_end1", FIXTURE_CMD_END_QUERY, FIXTURE_STATUS_POOL, 1U, 0U);
	fixture_query_record("o_begin0", FIXTURE_CMD_BEGIN_QUERY, FIXTURE_OCCLUSION_POOL, 0U, 0U);
	fixture_query_record("o_end0", FIXTURE_CMD_END_QUERY, FIXTURE_OCCLUSION_POOL, 0U, 0U);
	fixture_query_record("q_past", FIXTURE_CMD_RESET_QUERY_POOL, FIXTURE_STATUS_POOL, 1U, 2U);

	/* A new pool's queries are unavailable: NOT_READY, no status. */
	result = fixture_statuses(statuses);
	assert(result == VK_NOT_READY);
	assert(statuses[0] == 0 && statuses[1] == 0);

	/* The IDR picture and the P picture both decode: COMPLETE twice. */
	fixture_record(both, 12U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	result = fixture_statuses(statuses);
	assert(result == VK_SUCCESS);
	assert(statuses[0] == FIXTURE_STATUS_COMPLETE && statuses[1] == FIXTURE_STATUS_COMPLETE);

	/* A reset alone on the video family makes both unavailable again. */
	fixture_record(reset_only, 1U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	result = fixture_statuses(statuses);
	assert(result == VK_NOT_READY);
	assert(statuses[0] == 0 && statuses[1] == 0);

	/* A second slice without its start code skips the IDR picture: ERROR for it, the submission succeeds. */
	fixture_storage[FIXTURE_BUFFER_OFFSET + 67U] = 0x55U;
	fixture_record(both, 12U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	assert(strstr(stub_log, "skip decode: slice without a start code") != NULL);
	result = fixture_statuses(statuses);
	assert(result == VK_SUCCESS);
	assert(statuses[0] == FIXTURE_STATUS_ERROR);
	result = fixture_status32(&status32);
	assert(result == VK_SUCCESS && status32 == 0xffffffffU);
	fixture_storage[FIXTURE_BUFFER_OFFSET + 67U] = 1U;

	/* A query begun outside a scope, one not ended in its scope, two at once, a reset within a scope, an end of another query, and an occlusion query in a scope break the rules. */
	fixture_record(outside, 5U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	fixture_record(unended, 5U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "a query not ended in its coding scope") != NULL);
	fixture_record(twice, 8U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	fixture_record(reset_inside, 5U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "a query reset within a coding scope") != NULL);
	fixture_record(other_end, 6U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	fixture_record(occlusion, 6U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);

	/* Two decodes within one query, and a reset past the pool, break the rules before anything runs. */
	fixture_record(two_decodes, 7U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "a second decode within one query") != NULL);
	fixture_record(past, 1U);
	assert(fixture_submit(FIXTURE_VIDEO_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "a query reset past its pool") != NULL);

	/* None of the refused submissions ran: the statuses are those of the skipped IDR and the decoded P picture. */
	result = fixture_statuses(statuses);
	assert(result == VK_SUCCESS);
	assert(statuses[0] == FIXTURE_STATUS_ERROR && statuses[1] == FIXTURE_STATUS_COMPLETE);

	/* A reset after the query's end in the same submission runs after it: the query is unavailable again. */
	fixture_record(reset_after, 8U);
	result = fixture_submit(FIXTURE_VIDEO_QUEUE);
	assert(result == VK_SUCCESS);
	result = fixture_statuses(statuses);
	assert(result == VK_NOT_READY);
	assert(statuses[0] == 0);

	/* On the graphics family the reset runs, but a begin of a status query loses the submission. */
	fixture_record(graphics, 3U);
	assert(fixture_submit(FIXTURE_QUEUE) == (uint32_t)VK_ERROR_DEVICE_LOST);
	assert(strstr(stub_log, "result status query begun or ended outside a video coding scope") != NULL);
	result = fixture_statuses(statuses);
	assert(result == VK_NOT_READY);
}

/* Writes a field check of the two decodes' batch for genxml-decode.py. */
static void
fixture_check(
	FILE *expect,
	unsigned instruction,
	const char *field,
	unsigned long long value)
{
	/* The instruction's index, the field and its value. */
	fprintf(expect, "F\t%u\t%s\t%llu\n", instruction, field, value);
}

/* Writes an instruction check of the two decodes' batch: the index-th instruction is the named one. */
static void
fixture_named(
	FILE *expect,
	unsigned instruction,
	const char *name)
{
	/* The instruction's index and name. */
	fprintf(expect, "I\t%u\t%s\n", instruction, name);
}

/*
 * Writes the batch the IDR and the P picture ran (idr.bin) and what
 * genxml-decode.py must read in it (idr.expect): the addresses the
 * executor resolved from the wire's objects -- the pictures A and B, the
 * session's eight bindings a page apart from the memory's start, the
 * bitstream buffer -- and the fields of the wire's sets and pictures.
 */
static void
fixture_batch_expect(void)
{
	static const char *const first[] = {
		"MI_FLUSH_DW", "MI_FORCE_WAKEUP", "MFX_WAIT", "MFX_PIPE_MODE_SELECT", "MFX_WAIT",
		"MFX_SURFACE_STATE", "MFX_PIPE_BUF_ADDR_STATE", "MFX_IND_OBJ_BASE_ADDR_STATE",
		"MFX_BSP_BUF_BASE_ADDR_STATE", "MFD_AVC_DPB_STATE", "MFD_AVC_PICID_STATE",
		"MFX_AVC_IMG_STATE", "MFX_QM_STATE", "MFX_QM_STATE", "MFX_AVC_DIRECTMODE_STATE"
	};
	char path[1024];
	FILE *file;
	FILE *expect;
	size_t written;
	unsigned base;
	unsigned index;

	/* The batch's dwords. */
	snprintf(path, sizeof(path), "%s/idr.bin", fixture_directory);
	file = fopen(path, "wb");
	assert(file != NULL);
	written = fwrite(stub_batch_words, 4U, stub_batch_count, file);
	assert(written == stub_batch_count);
	fclose(file);

	/* The checks. */
	snprintf(path, sizeof(path), "%s/idr.expect", fixture_directory);
	expect = fopen(path, "w");
	assert(expect != NULL);

	/* The IDR picture: the fixed commands, the next slice's address, two slices, the flush. */
	for (index = 0U; index < 15U; index++)
		fixture_named(expect, index, first[index]);
	fixture_named(expect, 15U, "MFD_AVC_SLICEADDR");
	fixture_named(expect, 16U, "MFD_AVC_BSD_OBJECT");
	fixture_named(expect, 17U, "MFD_AVC_BSD_OBJECT");
	fixture_named(expect, 18U, "MI_FLUSH_DW");

	/* Its surface: picture A, 64x64, pitch 128, CbCr from row 64. */
	fixture_check(expect, 5U, "Width", 63U);
	fixture_check(expect, 5U, "Height", 63U);
	fixture_check(expect, 5U, "Surface Pitch", 127U);
	fixture_check(expect, 5U, "Y Offset for U(Cb)", 64U);
	fixture_check(expect, 5U, "Y Offset for V(Cr)", 64U);

	/* Its buffers: A, row stores 0 and 1, every reference entry A itself (no reference). */
	fixture_check(expect, 6U, "Post Deblocking Destination - Address", FIXTURE_STORAGE_VA + FIXTURE_IMAGE_A_OFFSET);
	fixture_check(expect, 6U, "Intra Row Store Scratch Buffer - Address", FIXTURE_STORAGE_VA);
	fixture_check(expect, 6U, "Deblocking Filter Row Store Scratch - Address", FIXTURE_STORAGE_VA + 0x1000U);
	fixture_check(expect, 6U, "Reference Picture - Address[0]", FIXTURE_STORAGE_VA + FIXTURE_IMAGE_A_OFFSET);
	fixture_check(expect, 6U, "Reference Picture - Address[15]", FIXTURE_STORAGE_VA + FIXTURE_IMAGE_A_OFFSET);

	/* The bitstream: the buffer's page, bounded by its 4 KiB; row stores 2 and 3. */
	fixture_check(expect, 7U, "MFX Indirect Bitstream Object - Address", FIXTURE_STORAGE_VA + FIXTURE_BUFFER_OFFSET);
	fixture_check(expect, 7U, "MFX Indirect Bitstream Object - Upper Bound", FIXTURE_STORAGE_VA + FIXTURE_BUFFER_OFFSET + 4096U);
	fixture_check(expect, 8U, "BSD/MPC Row Store Scratch Buffer - Address", FIXTURE_STORAGE_VA + 0x2000U);
	fixture_check(expect, 8U, "MPR Row Store Scratch Buffer - Address", FIXTURE_STORAGE_VA + 0x3000U);

	/* No reference; the picture state of the wire's sets (CABAC, deblocking control, POC type 0, LSB width 6). */
	fixture_check(expect, 10U, "Picture ID[0]", 0xffffU);
	fixture_check(expect, 11U, "Frame Size", 16U);
	fixture_check(expect, 11U, "Frame Width", 3U);
	fixture_check(expect, 11U, "Frame Height", 3U);
	fixture_check(expect, 11U, "Non-Reference Picture", 0U);
	fixture_check(expect, 11U, "Entropy Coding Sync Enable", 1U);
	fixture_check(expect, 11U, "Deblocking Filter Control Present", 1U);
	fixture_check(expect, 11U, "Direct 8x8 Inference", 1U);
	fixture_check(expect, 11U, "Pic Order Count Type", 0U);
	fixture_check(expect, 11U, "Log2 Max Pic Order Count LSB", 2U);
	fixture_check(expect, 11U, "Number of Reference Frames", 0U);
	fixture_check(expect, 12U, "Forward Quantizer Matrix[0]", 16U);

	/* The reference IDR writes its slot 0's motion vectors (binding 4). */
	fixture_check(expect, 14U, "Direct MV Buffer (Write) - Address", FIXTURE_STORAGE_VA + 0x4000U);
	fixture_check(expect, 14U, "Direct MV Buffer - Address[0]", FIXTURE_STORAGE_VA + 0x4000U);

	/* The slices: start codes at 0 (three bytes) and 64 (four bytes), the range 128 bytes. */
	fixture_check(expect, 15U, "Indirect BSD Data Start Address", 68U);
	fixture_check(expect, 15U, "Indirect BSD Data Length", 60U);
	fixture_check(expect, 16U, "Indirect BSD Data Start Address", 3U);
	fixture_check(expect, 16U, "Indirect BSD Data Length", 61U);
	fixture_check(expect, 16U, "Inline Data.Last Slice", 0U);
	fixture_check(expect, 17U, "Indirect BSD Data Start Address", 68U);
	fixture_check(expect, 17U, "Inline Data.Last Slice", 1U);

	/* The P picture: the fixed commands, one slice, the flush. */
	base = 19U;
	for (index = 0U; index < 15U; index++)
		fixture_named(expect, base + index, first[index]);
	fixture_named(expect, base + 15U, "MFD_AVC_BSD_OBJECT");
	fixture_named(expect, base + 16U, "MI_FLUSH_DW");

	/* Into B, reading A (slot 0) as its one reference; the unused entries B itself. */
	fixture_check(expect, base + 6U, "Post Deblocking Destination - Address", FIXTURE_STORAGE_VA + FIXTURE_IMAGE_B_OFFSET);
	fixture_check(expect, base + 6U, "Reference Picture - Address[0]", FIXTURE_STORAGE_VA + FIXTURE_IMAGE_A_OFFSET);
	fixture_check(expect, base + 6U, "Reference Picture - Address[1]", FIXTURE_STORAGE_VA + FIXTURE_IMAGE_B_OFFSET);
	fixture_check(expect, base + 9U, "Used for Reference[0]", 3U);
	fixture_check(expect, base + 9U, "Used for Reference[1]", 0U);
	fixture_check(expect, base + 10U, "Picture ID[0]", 0U);
	fixture_check(expect, base + 10U, "Picture ID[1]", 0xffffU);
	fixture_check(expect, base + 11U, "Non-Reference Picture", 1U);
	fixture_check(expect, base + 11U, "Number of Reference Frames", 1U);
	fixture_check(expect, base + 11U, "Current Picture Frame Number", 1U);

	/* Slot 0's motion vectors read; a picture that is not a reference writes the spare buffer (binding 7). */
	fixture_check(expect, base + 14U, "Direct MV Buffer - Address[0]", FIXTURE_STORAGE_VA + 0x4000U);
	fixture_check(expect, base + 14U, "Direct MV Buffer - Address[1]", FIXTURE_STORAGE_VA + 0x7000U);
	fixture_check(expect, base + 14U, "Direct MV Buffer (Write) - Address", FIXTURE_STORAGE_VA + 0x7000U);
	fixture_check(expect, base + 14U, "POC List[32]", 2U);
	fixture_check(expect, base + 15U, "Indirect BSD Data Start Address", 3U);
	fixture_check(expect, base + 15U, "Indirect BSD Data Length", 125U);
	fixture_check(expect, base + 15U, "Inline Data.Last Slice", 1U);
	fprintf(expect, "N\t%u\n", base + 17U);
	fclose(expect);
}

/* Lays one NV12 picture out and checks its pitch, its planes and its size, and that no other use takes it. */
static void
fixture_layout(
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t rows,
	uint64_t chroma_offset,
	uint64_t bytes)
{
	struct i915_gfx_image image;
	struct i915_gfx_surface surface;
	int error;

	/* The picture as vkCreateImage makes it. */
	memset(&image, 0, sizeof(image));
	image.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	image.width = width;
	image.height = height;
	image.levels = 1U;
	image.layers = 1U;
	image.samples = 1U;
	image.type = VK_IMAGE_TYPE_2D;
	error = drv_i915_gfx_image_layout(&image);
	assert(error == 0);

	/* Y tiles: the pitch, the Y plane's rows, where the CbCr plane starts, whole pages. */
	assert(image.planar == 1U);
	assert(image.pitch == pitch);
	assert(image.chroma_rows == rows);
	assert(image.chroma_offset == chroma_offset);
	assert(image.bytes == bytes);

	/* No copy, blit, clear or sampling takes it. */
	error = drv_i915_gfx_image_slice(&image, 0U, 0U, &surface);
	assert(error == EINVAL);
}

/* The NV12 layouts of design §8.1: 16x16, 1920x1080 and 4096x4096. */
static void
test_layouts(void)
{
	/* 16x16: one tile across, 32 rows of Y, 32 of CbCr. */
	fixture_layout(16U, 16U, 128U, 32U, 4096U, 8192U);

	/* 1920x1080: 1088 rows of Y, 544 of CbCr, 3 MiB less 12 KiB. */
	fixture_layout(1920U, 1080U, 1920U, 1088U, 1920ULL * 1088U, 1920ULL * 1632U);

	/* 4096x4096: the largest picture. */
	fixture_layout(4096U, 4096U, 4096U, 4096U, 4096ULL * 4096U, 4096ULL * 6144U);

	/* An extent not of whole macroblocks is rounded up first: 100x50 is 112x64. */
	fixture_layout(100U, 50U, 128U, 64U, 128U * 64U, 128U * 96U);
}
