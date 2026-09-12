/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Original finite Vulkan graphics encoding over Venus wire format 1.
 * The public protocol provenance and scene contract are recorded in README.md.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <uapi/gpu.h>
#include "../../gpu/venus/client.h"
#include "../common/sha256.h"
#include "renderer.h"
#include "shaders.h"

#define TEXTURE_WIDTH 64U
#define TEXTURE_BYTES (TEXTURE_WIDTH * TEXTURE_WIDTH * 4U)
#define TEXTURE_OFFSET 4096U
#define UPLOAD_BYTES (TEXTURE_OFFSET + TEXTURE_BYTES)
#define VERTEX_COUNT 36U
#define VERTEX_STRIDE 20U
#define FORMAT_RGBA8 37U
#define FORMAT_DEPTH32 126U
#define VK_IGNORED 0xffffffffU

/* Stable command numbers from the pinned public Venus protocol schema. */
enum demo_command {
	CMD_FORMAT_PROPERTIES = 4,
	CMD_SUBMIT = 18,
	CMD_ALLOCATE_MEMORY = 21,
	CMD_FREE_MEMORY = 22,
	CMD_BIND_BUFFER = 28,
	CMD_BIND_IMAGE = 29,
	CMD_BUFFER_REQUIREMENTS = 30,
	CMD_IMAGE_REQUIREMENTS = 31,
	CMD_CREATE_FENCE = 35,
	CMD_DESTROY_FENCE = 36,
	CMD_RESET_FENCES = 37,
	CMD_FENCE_STATUS = 38,
	CMD_CREATE_BUFFER = 50,
	CMD_DESTROY_BUFFER = 51,
	CMD_CREATE_IMAGE = 54,
	CMD_DESTROY_IMAGE = 55,
	CMD_CREATE_IMAGE_VIEW = 57,
	CMD_DESTROY_IMAGE_VIEW = 58,
	CMD_CREATE_SHADER = 59,
	CMD_DESTROY_SHADER = 60,
	CMD_CREATE_PIPELINES = 65,
	CMD_DESTROY_PIPELINE = 67,
	CMD_CREATE_PIPELINE_LAYOUT = 68,
	CMD_DESTROY_PIPELINE_LAYOUT = 69,
	CMD_CREATE_SAMPLER = 70,
	CMD_DESTROY_SAMPLER = 71,
	CMD_CREATE_SET_LAYOUT = 72,
	CMD_DESTROY_SET_LAYOUT = 73,
	CMD_CREATE_DESCRIPTOR_POOL = 74,
	CMD_DESTROY_DESCRIPTOR_POOL = 75,
	CMD_ALLOCATE_SETS = 77,
	CMD_UPDATE_SETS = 79,
	CMD_CREATE_FRAMEBUFFER = 80,
	CMD_DESTROY_FRAMEBUFFER = 81,
	CMD_CREATE_RENDER_PASS = 82,
	CMD_DESTROY_RENDER_PASS = 83,
	CMD_CREATE_COMMAND_POOL = 85,
	CMD_DESTROY_COMMAND_POOL = 86,
	CMD_RESET_COMMAND_POOL = 87,
	CMD_ALLOCATE_COMMAND_BUFFERS = 88,
	CMD_BEGIN_COMMAND_BUFFER = 90,
	CMD_END_COMMAND_BUFFER = 91,
	CMD_BIND_PIPELINE = 93,
	CMD_BIND_DESCRIPTOR_SETS = 103,
	CMD_BIND_VERTEX_BUFFERS = 105,
	CMD_DRAW = 106,
	CMD_COPY_BUFFER_TO_IMAGE = 115,
	CMD_COPY_IMAGE_TO_BUFFER = 116,
	CMD_PIPELINE_BARRIER = 126,
	CMD_PUSH_CONSTANTS = 132,
	CMD_BEGIN_RENDER_PASS = 133,
	CMD_END_RENDER_PASS = 135
};

/* Application object identities remain private to this one Venus context. */
enum demo_object {
	OBJECT_COLOR = 16,
	OBJECT_COLOR_MEMORY,
	OBJECT_COLOR_VIEW,
	OBJECT_DEPTH,
	OBJECT_DEPTH_MEMORY,
	OBJECT_DEPTH_VIEW,
	OBJECT_TEXTURE,
	OBJECT_TEXTURE_MEMORY,
	OBJECT_TEXTURE_VIEW,
	OBJECT_UPLOAD,
	OBJECT_UPLOAD_MEMORY,
	OBJECT_READBACK,
	OBJECT_READBACK_MEMORY,
	OBJECT_SAMPLER,
	OBJECT_SET_LAYOUT,
	OBJECT_DESCRIPTOR_POOL,
	OBJECT_DESCRIPTOR_SET,
	OBJECT_PIPELINE_LAYOUT,
	OBJECT_RENDER_PASS,
	OBJECT_FRAMEBUFFER,
	OBJECT_VERTEX_SHADER,
	OBJECT_FRAGMENT_SHADER,
	OBJECT_PIPELINE,
	OBJECT_COMMAND_POOL,
	OBJECT_COMMAND_BUFFER,
	OBJECT_FENCE
};

/* One completed Vulkan creation contributes a reverse-order teardown entry. */
struct demo_release {
	uint32_t command;
	uint32_t object;
};

/* One process retains this context and its uploaded data across every frame. */
struct demo_renderer {
	struct venus_client client;
	struct demo_release releases[40];
	uint32_t release_count;
	uint64_t upload_allocation;
	uint64_t readback_allocation;
	uint64_t upload_blob;
	uint64_t readback_blob;
	uint64_t presentation;
	uint32_t failure_command;
	int opened;
	int ready;
	int failed;
	int pending;
	uint8_t upload[UPLOAD_BYTES];
	uint8_t pixels[VKDEMO_BYTES];
};

/* Main serializes this singleton; static frame storage avoids a large stack. */
static struct demo_renderer renderer;

/* Each face lists the positions for UV corners 00, 10, 11, 01 respectively. */
static const float face_corners[6][4][3] = {
	{{0.75f, 0.5f, 0.375f}, {0.75f, 0.5f, -0.375f},
	 {0.75f, -0.5f, -0.375f}, {0.75f, -0.5f, 0.375f}},
	{{-0.75f, 0.5f, -0.375f}, {-0.75f, 0.5f, 0.375f},
	 {-0.75f, -0.5f, 0.375f}, {-0.75f, -0.5f, -0.375f}},
	{{-0.75f, 0.5f, -0.375f}, {0.75f, 0.5f, -0.375f},
	 {0.75f, 0.5f, 0.375f}, {-0.75f, 0.5f, 0.375f}},
	{{-0.75f, -0.5f, 0.375f}, {0.75f, -0.5f, 0.375f},
	 {0.75f, -0.5f, -0.375f}, {-0.75f, -0.5f, -0.375f}},
	{{-0.75f, 0.5f, 0.375f}, {0.75f, 0.5f, 0.375f},
	 {0.75f, -0.5f, 0.375f}, {-0.75f, -0.5f, 0.375f}},
	{{0.75f, 0.5f, -0.375f}, {-0.75f, 0.5f, -0.375f},
	 {-0.75f, -0.5f, -0.375f}, {0.75f, -0.5f, -0.375f}}
};

static void wire_u32(uint32_t word);
static void wire_u64(uint64_t number);
static void wire_float(float number);
static void wire_structure(uint32_t type);
static void begin_command(uint32_t command);
static int finish_command(int has_result);
static int finish_creation(uint32_t object, uint32_t destroy);
static void create_output(uint32_t object);
static int query_format(uint32_t format, uint32_t required);
static int create_image(uint32_t image, uint32_t format, uint32_t width, uint32_t height, uint32_t usage);
static int create_buffer(uint32_t buffer, uint32_t bytes, uint32_t usage);
static int bind_memory(uint32_t object, uint32_t memory, int image, uint32_t flags, uint64_t *allocation);
static int create_view(uint32_t view, uint32_t image, uint32_t format, uint32_t aspect);
static int export_memory(uint32_t memory, uint64_t bytes, uint64_t *handle);
static int create_storage(void);
static void prepare_upload(void);
static void store_float(uint8_t *destination, float number);
static int create_descriptors(void);
static int create_render_pass(void);
static int create_framebuffer(void);
static int create_shader(uint32_t object, const uint32_t *words, uint32_t bytes);
static int create_pipeline(void);
static void pipeline_stage(uint32_t stage, uint32_t module);
static void pipeline_vertices(void);
static void pipeline_viewport(void);
static void pipeline_rasterization(void);
static void pipeline_depth(void);
static void pipeline_blending(void);
static int create_commands(void);
static int begin_recording(void);
static int submit_recording(void);
static int image_barrier(uint32_t image, uint32_t old_layout, uint32_t new_layout, uint32_t source_access, uint32_t destination_access, uint32_t source_stage, uint32_t destination_stage);
static int buffer_barrier(uint32_t buffer, uint32_t bytes, uint32_t source_access, uint32_t destination_access, uint32_t source_stage, uint32_t destination_stage);
static int upload_texture(void);
static int reset_commands(void);
static int record_frame(uint32_t milliseconds);
static int begin_render_pass(void);
static int copy_frame(void);
static int hash_pixels(char digest[65]);
static int present_pixels(uint32_t frame);
static int release_resources(void);

/*
 * Create the shaders, uploaded texture and retained Vulkan drawing resources.
 */
int
vkdemo_initialize(
	const char *device)
{
	int status;

	/* Mark partial initialization as failed until every resource is ready. */
	memset(&renderer, 0, sizeof(renderer));
	renderer.failed = 1;

	/* Acquire the ordinary GPU session before requesting Venus initialization. */
	status = venus_client_open(&renderer.client, device);
	if (status != 0)
		return -1;

	/* The descriptor now belongs to the renderer even if setup later fails. */
	renderer.opened = 1;
	status = venus_client_init_vulkan(&renderer.client);
	if (status != 0)
		return -1;

	/* Identify the actual transport and bootstrap used by this application. */
	printf(
		"VKDEMO VULKAN driver=%.32s wire=%u xml=%u family=%u\n",
		renderer.client.info.driver_name,
		renderer.client.wire_version,
		renderer.client.xml_version,
		renderer.client.queue_family);
	fflush(stdout);

	/* Require the concrete formats used for sampling, color and depth. */
	status = query_format(FORMAT_RGBA8, 0xc081U);
	if (status != 0)
		return -1;

	/* Use a baseline depth format only after verifying attachment support. */
	status = query_format(FORMAT_DEPTH32, 0x200U);
	if (status != 0)
		return -1;

	/* Allocate images, upload/readback buffers and their one-time blob exports. */
	status = create_storage();
	if (status != 0)
		return -1;

	/* Fill the original geometry and texture before making them GPU-visible. */
	prepare_upload();
	status = venus_client_resource_copy(
		&renderer.client,
		renderer.upload_blob,
		0,
		renderer.upload,
		UPLOAD_BYTES,
		1);
	if (status != 0)
		return -1;

	/* Connect one combined image sampler to the fragment stage. */
	status = create_descriptors();
	if (status != 0)
		return -1;

	/* Define color and depth attachment transitions for every frame. */
	status = create_render_pass();
	if (status != 0)
		return -1;

	/* Bind the retained color and depth views to the render pass. */
	status = create_framebuffer();
	if (status != 0)
		return -1;

	/* Upload the validated original vertex shader module. */
	status = create_shader(
		OBJECT_VERTEX_SHADER,
		vkdemo_vertex_shader,
		sizeof(vkdemo_vertex_shader));
	if (status != 0)
		return -1;

	/* Upload the validated original texture-sampling fragment module. */
	status = create_shader(
		OBJECT_FRAGMENT_SHADER,
		vkdemo_fragment_shader,
		sizeof(vkdemo_fragment_shader));
	if (status != 0)
		return -1;

	/* Link both shaders with the explicit vertex and depth state. */
	status = create_pipeline();
	if (status != 0)
		return -1;

	/* Allocate a reusable primary command buffer and completion fence. */
	status = create_commands();
	if (status != 0)
		return -1;

	/* Transfer the checker image once before its first sampled use. */
	status = upload_texture();
	if (status != 0)
		return -1;

	/* Permit ordinary frames only after the initial upload fence completed. */
	renderer.ready = 1;
	renderer.failed = 0;
	printf("VKDEMO READY vertices=36 texture=64x64 shaders=vertex,fragment\n");
	fflush(stdout);

	/* Succeeded: subsequent frames reuse all uploaded and allocated resources. */
	return 0;
}

/*
 * Render one shader time, read its actual pixels and present those same bytes.
 */
int
vkdemo_render(
	uint32_t milliseconds,
	uint32_t frame,
	char digest[65])
{
	int status;

	/* Refuse use of a partial or already failed Vulkan session. */
	if (renderer.ready == 0) {
		errno = EINVAL;
		return -1;
	}

	/* Failed command streams cannot accept another application frame. */
	if (renderer.failed != 0) {
		errno = EINVAL;
		return -1;
	}

	/* Any early exit from this frame requires context-owned failure teardown. */
	renderer.failed = 1;

	/* Reuse the pool and fence only after the preceding GPU work completed. */
	status = reset_commands();
	if (status != 0)
		return -1;

	/* Record real shader execution followed by color-image readback. */
	status = record_frame(milliseconds);
	if (status != 0)
		return -1;

	/* Wait for Vulkan completion independently of command decoding. */
	status = submit_recording();
	if (status != 0)
		return -1;

	/* Read from the retained exported allocation without re-exporting memory. */
	status = venus_client_resource_copy(
		&renderer.client,
		renderer.readback_blob,
		0,
		renderer.pixels,
		VKDEMO_BYTES,
		0);
	if (status != 0)
		return -1;

	/* Hash unmodified RGB bytes after checking the opaque readback contract. */
	status = hash_pixels(digest);
	if (status != 0)
		return -1;

	/* Present the exact bytes whose RGB digest the caller will report. */
	status = present_pixels(frame);
	if (status != 0)
		return -1;

	/* This completed frame leaves the retained resources available for reuse. */
	renderer.failed = 0;

	/* Succeeded: the visible frame was produced by the vertex and fragment shaders. */
	return 0;
}

/*
 * Release normal Vulkan objects or let a failed context own its teardown.
 */
int
vkdemo_close(
	void)
{
	int status;
	int closed;
	int saved_errno;

	/* A failed open transfers no descriptor and needs no device cleanup. */
	if (renderer.opened == 0)
		return 0;

	/* Preserve the failing command before normal cleanup changes diagnostics. */
	renderer.failure_command = renderer.client.active_command;
	status = 0;
	saved_errno = 0;

	/* Retire explicit resources only when no failed or pending work owns them. */
	if (renderer.failed == 0) {
		/* Only the signaled fence returns resource ownership for explicit cleanup. */
		if (renderer.pending == 0) {
			status = release_resources();
			if (status != 0)
				saved_errno = errno;
		}
	}

	/* Always consume the descriptor once, including partial initialization. */
	renderer.opened = 0;
	renderer.ready = 0;
	closed = venus_client_close(&renderer.client);
	if (status != 0) {
		errno = saved_errno;
		return -1;
	}

	/* Expose a failed close independently of the normal object cleanup. */
	if (closed != 0)
		return -1;

	/* Succeeded: the session no longer owns a device descriptor or Vulkan objects. */
	return 0;
}

/*
 * Preserve the last application command for a failure marker after close.
 */
uint32_t
vkdemo_active_command(
	void)
{
	/* Preserve the pre-close operation after teardown sends other commands. */
	if (renderer.failure_command != 0)
		return renderer.failure_command;

	/* Succeeded: return the current command while the session remains open. */
	return renderer.client.active_command;
}

/* Append a Vulkan scalar to the one serialized application command stream. */
static void
wire_u32(
	uint32_t word)
{
	/* Delegate little-endian encoding and bounds checks to the shared client. */
	venus_client_wire_u32(&renderer.client, word);

	/* Succeeded: the word is appended or the stream retains its sticky error. */
	return;
}

/* Append a Vulkan handle, pointer cardinality or device-size scalar. */
static void
wire_u64(
	uint64_t number)
{
	/* Preserve the public wire width independently of the guest C ABI. */
	venus_client_wire_u64(&renderer.client, number);

	/* Succeeded: the scalar is appended or the stream retains its sticky error. */
	return;
}

/* Encode an IEEE-754 float without aliasing it through an integer pointer. */
static void
wire_float(
	float number)
{
	uint32_t bits;

	/* Preserve shader and Vulkan float bits before little-endian serialization. */
	memcpy(&bits, &number, sizeof(bits));
	wire_u32(bits);

	/* Succeeded: the floating-point scalar is appended to the command. */
	return;
}

/* Begin a known Vulkan structure with no optional extension chain. */
static void
wire_structure(
	uint32_t type)
{
	/* Reuse the shared schema tag and null-pNext encoding. */
	venus_client_wire_structure(&renderer.client, type);

	/* Succeeded: subsequent fields belong to this unextended structure. */
	return;
}

/* Start one single-flight Vulkan call in this application's context. */
static void
begin_command(
	uint32_t command)
{
	/* Install the reply stream and command identity through the shared codec. */
	venus_client_command_begin(&renderer.client, command);

	/* Succeeded: the application may append the selected call's arguments. */
	return;
}

/* Finish one Vulkan call and require successful stream decoding and result. */
static int
finish_command(
	int has_result)
{
	int status;

	/* Wait for the renderer trailer before consuming any response fields. */
	status = venus_client_command_finish(&renderer.client, has_result, 0);
	if (status != 0)
		return -1;

	/* Succeeded: the Vulkan result or void command completed successfully. */
	return 0;
}

/* Finish creation and retain its destructor only after validating the handle. */
static int
finish_creation(
	uint32_t object,
	uint32_t destroy)
{
	int status;

	/* Require a successful Vulkan result before trusting an output identity. */
	status = finish_command(1);
	if (status != 0)
		return -1;

	/* Reject a reply that does not acknowledge the requested guest object ID. */
	status = venus_client_reply_handle(&renderer.client, object);
	if (status != 0)
		return -1;

	/* Parent-owned command buffers and descriptor sets need no separate entry. */
	if (destroy != 0) {
		/* Bound this finite scene's teardown ledger before adding an entry. */
		if (renderer.release_count >= 40U) {
			errno = EOVERFLOW;
			return -1;
		}

		/* Each count increment keeps one successfully created Vulkan object alive. */
		renderer.releases[renderer.release_count].command = destroy;
		renderer.releases[renderer.release_count].object = object;
		renderer.release_count++;
	}

	/* Succeeded: the object is registered for normal lifetime management. */
	return 0;
}

/* Append the common null allocator and one caller-chosen creation output. */
static void
create_output(
	uint32_t object)
{
	/* The finite API uses the renderer allocator and one explicit output ID. */
	wire_u64(0);
	wire_u64(1);
	wire_u64(object);

	/* Succeeded: the creation call now includes its complete output contract. */
	return;
}

/* Verify the exact optimal-image format features required by this scene. */
static int
query_format(
	uint32_t format,
	uint32_t required)
{
	uint64_t present;
	uint32_t optimal;
	int status;

	/* Request the public format-properties record from the selected device. */
	begin_command(CMD_FORMAT_PROPERTIES);
	wire_u64(VENUS_OBJECT_PHYSICAL_DEVICE);
	wire_u32(format);
	wire_u64(1);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Skip linear and buffer capabilities after retaining optimal-image support. */
	present = venus_client_reply_u64(&renderer.client);
	(void)venus_client_reply_u32(&renderer.client);
	optimal = venus_client_reply_u32(&renderer.client);
	(void)venus_client_reply_u32(&renderer.client);
	if (present != 1) {
		errno = EIO;
		return -1;
	}

	/* Refuse fields decoded beyond the bounded response. */
	if (renderer.client.wire_error != 0) {
		errno = EIO;
		return -1;
	}

	/* A different format would change the documented pixel and depth contract. */
	if ((optimal & required) != required) {
		errno = ENOTSUP;
		return -1;
	}

	/* Succeeded: the host supports this scene's exact image usage. */
	return 0;
}

/* Create one optimal two-dimensional image with a single mip and array layer. */
static int
create_image(
	uint32_t image,
	uint32_t format,
	uint32_t width,
	uint32_t height,
	uint32_t usage)
{
	int status;

	/* Specify one exclusive, initially undefined, single-sample image. */
	begin_command(CMD_CREATE_IMAGE);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(14);
	wire_u32(0);
	wire_u32(1);
	wire_u32(format);
	wire_u32(width);
	wire_u32(height);
	wire_u32(1);
	wire_u32(1);
	wire_u32(1);
	wire_u32(1);
	wire_u32(0);
	wire_u32(usage);
	wire_u32(0);
	wire_u32(0);
	wire_u64(0);
	wire_u32(0);
	create_output(image);
	status = finish_creation(image, CMD_DESTROY_IMAGE);
	if (status != 0)
		return -1;

	/* Succeeded: the image can now acquire compatible memory. */
	return 0;
}

/* Create one exclusive linear buffer for upload or GPU-produced readback. */
static int
create_buffer(
	uint32_t buffer,
	uint32_t bytes,
	uint32_t usage)
{
	int status;

	/* Keep the buffer's purpose explicit in its Vulkan usage flags. */
	begin_command(CMD_CREATE_BUFFER);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(12);
	wire_u32(0);
	wire_u64(bytes);
	wire_u32(usage);
	wire_u32(0);
	wire_u32(0);
	wire_u64(0);
	create_output(buffer);
	status = finish_creation(buffer, CMD_DESTROY_BUFFER);
	if (status != 0)
		return -1;

	/* Succeeded: the buffer awaits a compatible memory allocation. */
	return 0;
}

/* Allocate compatible memory at offset zero and bind it to its one object. */
static int
bind_memory(
	uint32_t object,
	uint32_t memory,
	int image,
	uint32_t flags,
	uint64_t *allocation)
{
	uint64_t present;
	uint64_t bytes;
	uint64_t alignment;
	uint32_t bits;
	uint32_t selected;
	uint32_t index;
	uint32_t command;
	int status;

	/* Select requirements by the actual Vulkan object class. */
	command = CMD_BUFFER_REQUIREMENTS;
	if (image != 0)
		command = CMD_IMAGE_REQUIREMENTS;

	/* Request size, alignment and compatible memory types from Vulkan. */
	begin_command(command);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(object);
	wire_u64(1);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Validate the requirements response before making an allocation choice. */
	present = venus_client_reply_u64(&renderer.client);
	bytes = venus_client_reply_u64(&renderer.client);
	alignment = venus_client_reply_u64(&renderer.client);
	bits = venus_client_reply_u32(&renderer.client);
	if (present != 1) {
		errno = EIO;
		return -1;
	}

	/* Require a complete bounded requirements response. */
	if (renderer.client.wire_error != 0) {
		errno = EIO;
		return -1;
	}

	/* Vulkan allocations must contain at least one byte. */
	if (bytes == 0) {
		errno = EIO;
		return -1;
	}

	/* Bound individual allocations within this finite scene's resource budget. */
	if (bytes > 4U * 1024U * 1024U) {
		errno = EOVERFLOW;
		return -1;
	}

	/* A zero alignment cannot describe a valid Vulkan allocation requirement. */
	if (alignment == 0) {
		errno = EIO;
		return -1;
	}

	/* Choose only a compatible type with the requested host coherence. */
	selected = VK_IGNORED;
	for (index = 0; index < renderer.client.memory_count; index++) {
		/* Ignore memory heaps that cannot back this Vulkan object. */
		if ((bits & (1U << index)) == 0)
			continue;

		/* Require visibility and coherence for exported upload/readback buffers. */
		if ((renderer.client.memory_flags[index] & flags) != flags)
			continue;

		/* The first compatible type is sufficient for this finite scene. */
		selected = index;
		break;
	}

	/* Fail explicitly when the host cannot provide the selected memory contract. */
	if (selected == VK_IGNORED) {
		errno = ENOTSUP;
		return -1;
	}

	/* Align exports to a page while offset zero satisfies every object alignment. */
	bytes = (bytes + 4095U) & ~(uint64_t)4095U;
	begin_command(CMD_ALLOCATE_MEMORY);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(5);
	wire_u64(bytes);
	wire_u32(selected);
	create_output(memory);
	status = finish_creation(memory, CMD_FREE_MEMORY);
	if (status != 0)
		return -1;

	/* Bind the newly allocated memory to the matching object class. */
	command = CMD_BIND_BUFFER;
	if (image != 0)
		command = CMD_BIND_IMAGE;

	/* Offset zero obeys the queried alignment without suballocation. */
	begin_command(command);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(object);
	wire_u64(memory);
	wire_u64(0);
	status = finish_command(1);
	if (status != 0)
		return -1;

	/* Retain allocation size only for buffers that will become exported blobs. */
	if (allocation != NULL)
		*allocation = bytes;

	/* Succeeded: this image or buffer owns its fully bound memory allocation. */
	return 0;
}

/* Expose one full image layer for color, depth or descriptor sampling. */
static int
create_view(
	uint32_t view,
	uint32_t image,
	uint32_t format,
	uint32_t aspect)
{
	int status;

	/* Preserve identity component swizzles and the complete single-mip layer. */
	begin_command(CMD_CREATE_IMAGE_VIEW);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(15);
	wire_u32(0);
	wire_u64(image);
	wire_u32(1);
	wire_u32(format);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(aspect);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0);
	wire_u32(1);
	create_output(view);
	status = finish_creation(view, CMD_DESTROY_IMAGE_VIEW);
	if (status != 0)
		return -1;

	/* Succeeded: the image view can be referenced by framebuffer or descriptor. */
	return 0;
}

/* Export a Vulkan allocation once and retain the resulting ordinary GPU handle. */
static int
export_memory(
	uint32_t memory,
	uint64_t bytes,
	uint64_t *handle)
{
	struct gpu_blob_create request;
	int status;

	/* Nonzero blob identity names the already created Vulkan device memory. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.bytes = bytes;
	request.blob_id = memory;
	request.flags = GPU_BLOB_MAPPABLE;
	status = ioctl(renderer.client.fd, GPU_BLOB_CREATE, &request);
	if (status != 0)
		return -1;

	/* Succeeded: reuse this handle for every later copy; do not export again. */
	*handle = request.handle;
	return 0;
}

/* Allocate all scene storage before descriptors and drawing commands refer to it. */
static int
create_storage(
	void)
{
	struct gpu_resource_create presentation;
	int status;

	/* The color attachment becomes the transfer source after each render pass. */
	status = create_image(OBJECT_COLOR, FORMAT_RGBA8, VKDEMO_WIDTH, VKDEMO_HEIGHT, 0x11U);
	if (status != 0)
		return -1;

	/* Keep the color allocation private to the image. */
	status = bind_memory(OBJECT_COLOR, OBJECT_COLOR_MEMORY, 1, 0, NULL);
	if (status != 0)
		return -1;

	/* Publish the color attachment view for framebuffer creation. */
	status = create_view(OBJECT_COLOR_VIEW, OBJECT_COLOR, FORMAT_RGBA8, 1);
	if (status != 0)
		return -1;

	/* Depth resolves overlapping cuboid faces instead of relying on draw order. */
	status = create_image(OBJECT_DEPTH, FORMAT_DEPTH32, VKDEMO_WIDTH, VKDEMO_HEIGHT, 0x20U);
	if (status != 0)
		return -1;

	/* Bind independent storage for the depth attachment. */
	status = bind_memory(OBJECT_DEPTH, OBJECT_DEPTH_MEMORY, 1, 0, NULL);
	if (status != 0)
		return -1;

	/* Expose only the depth aspect of the depth-format image. */
	status = create_view(OBJECT_DEPTH_VIEW, OBJECT_DEPTH, FORMAT_DEPTH32, 2);
	if (status != 0)
		return -1;

	/* The uploaded checker will be sampled by the real fragment shader. */
	status = create_image(OBJECT_TEXTURE, FORMAT_RGBA8, TEXTURE_WIDTH, TEXTURE_WIDTH, 6);
	if (status != 0)
		return -1;

	/* Keep sampled texture memory separate from its transient upload source. */
	status = bind_memory(OBJECT_TEXTURE, OBJECT_TEXTURE_MEMORY, 1, 0, NULL);
	if (status != 0)
		return -1;

	/* Publish the texture view used by the combined image descriptor. */
	status = create_view(OBJECT_TEXTURE_VIEW, OBJECT_TEXTURE, FORMAT_RGBA8, 1);
	if (status != 0)
		return -1;

	/* One buffer holds vertex input and the staging source of the texture. */
	status = create_buffer(OBJECT_UPLOAD, UPLOAD_BYTES, 0x81U);
	if (status != 0)
		return -1;

	/* Host-visible coherent memory permits ordinary copied upload access. */
	status = bind_memory(OBJECT_UPLOAD, OBJECT_UPLOAD_MEMORY, 0, 6, &renderer.upload_allocation);
	if (status != 0)
		return -1;

	/* Export the upload allocation once for the lifetime of this session. */
	status = export_memory(OBJECT_UPLOAD_MEMORY, renderer.upload_allocation, &renderer.upload_blob);
	if (status != 0)
		return -1;

	/* GPU image copies alone populate this full-frame readback buffer. */
	status = create_buffer(OBJECT_READBACK, VKDEMO_BYTES, 2);
	if (status != 0)
		return -1;

	/* Coherence removes the need for unsupported host invalidation commands. */
	status = bind_memory(OBJECT_READBACK, OBJECT_READBACK_MEMORY, 0, 6, &renderer.readback_allocation);
	if (status != 0)
		return -1;

	/* Retain one readback blob rather than re-exporting it for each frame. */
	status = export_memory(OBJECT_READBACK_MEMORY, renderer.readback_allocation, &renderer.readback_blob);
	if (status != 0)
		return -1;

	/* Allocate ordinary packed-pixel storage for the established copy-based presentation interface. */
	memset(&presentation, 0, sizeof(presentation));
	presentation.version = GPU_ABI_VERSION;
	presentation.size = sizeof(presentation);
	presentation.bytes = VKDEMO_BYTES;
	presentation.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(renderer.client.fd, GPU_RESOURCE_CREATE, &presentation);
	if (status != 0)
		return -1;

	/* Preserve scanout storage until every frame and capture has finished. */
	renderer.presentation = presentation.handle;
	printf("VKDEMO STORAGE upload=%u readback=%u blob_exports=2\n", UPLOAD_BYTES, VKDEMO_BYTES);
	fflush(stdout);

	/* Succeeded: every buffer and image has its retained backing allocation. */
	return 0;
}

/* Store one shader input float in little-endian Vulkan vertex-buffer order. */
static void
store_float(
	uint8_t *destination,
	float number)
{
	uint32_t bits;

	/* Preserve float representation without unaligned or aliased stores. */
	memcpy(&bits, &number, sizeof(bits));
	destination[0] = (uint8_t)bits;
	destination[1] = (uint8_t)(bits >> 8);
	destination[2] = (uint8_t)(bits >> 16);
	destination[3] = (uint8_t)(bits >> 24);

	/* Succeeded: the vertex attribute now has the required byte representation. */
	return;
}

/* Construct the original cuboid vertices and asymmetric checker texture. */
static void
prepare_upload(
	void)
{
	static const uint32_t triangles[6] = {0, 1, 2, 0, 2, 3};
	static const float coordinates[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	uint32_t face;
	uint32_t vertex;
	uint32_t corner;
	uint32_t component;
	uint32_t offset;
	uint32_t x;
	uint32_t y;
	uint32_t checker;

	/* Expand six independent face quads into thirty-six position/UV vertices. */
	memset(renderer.upload, 0, sizeof(renderer.upload));
	for (face = 0; face < 6U; face++) {
		/* Duplicate face corners so interpolation never joins unrelated face UVs. */
		for (vertex = 0; vertex < 6U; vertex++) {
			/* Write the original local-space position before the shader rotates it. */
			corner = triangles[vertex];
			offset = (face * 6U + vertex) * VERTEX_STRIDE;
			for (component = 0; component < 3U; component++) {
				store_float(
					&renderer.upload[offset + component * 4U],
					face_corners[face][corner][component]);
			}

			/* Store the face UV endpoints in the following two float attributes. */
			store_float(&renderer.upload[offset + 12U], coordinates[corner][0]);
			store_float(&renderer.upload[offset + 16U], coordinates[corner][1]);
		}
	}

	/* Give the checker distinct U/V gradients so the oracle can detect bad UVs. */
	for (y = 0; y < TEXTURE_WIDTH; y++) {
		/* Fill texture rows in exactly the same order used by buffer-to-image copy. */
		for (x = 0; x < TEXTURE_WIDTH; x++) {
			/* Alternate the red channel in eight-texel checker squares. */
			offset = TEXTURE_OFFSET + (y * TEXTURE_WIDTH + x) * 4U;
			checker = (x / 8U + y / 8U) & 1U;
			renderer.upload[offset] = 32;
			if (checker != 0)
				renderer.upload[offset] = 224;

			/* Encode horizontal and vertical texture location without changing alpha. */
			renderer.upload[offset + 1U] = (uint8_t)(32U + x * 3U);
			renderer.upload[offset + 2U] = (uint8_t)(32U + y * 3U);
			renderer.upload[offset + 3U] = 255;
		}
	}

	/* Succeeded: only geometry and texture inputs were painted by the CPU. */
	return;
}

/* Connect one nearest-sampled texture and one vertex-stage time constant. */
static int
create_descriptors(
	void)
{
	int status;

	/* Use normalized nearest sampling, edge clamping and only mip level zero. */
	begin_command(CMD_CREATE_SAMPLER);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(31);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(2);
	wire_u32(2);
	wire_u32(2);
	wire_float(0);
	wire_u32(0);
	wire_float(1);
	wire_u32(0);
	wire_u32(7);
	wire_float(0);
	wire_float(0);
	wire_u32(0);
	wire_u32(0);
	create_output(OBJECT_SAMPLER);
	status = finish_creation(OBJECT_SAMPLER, CMD_DESTROY_SAMPLER);
	if (status != 0)
		return -1;

	/* Expose one combined image sampler at set zero, binding zero, fragment stage. */
	begin_command(CMD_CREATE_SET_LAYOUT);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(32);
	wire_u32(0);
	wire_u32(1);
	wire_u64(1);
	wire_u32(0);
	wire_u32(1);
	wire_u32(1);
	wire_u32(0x10U);
	wire_u64(0);
	create_output(OBJECT_SET_LAYOUT);
	status = finish_creation(OBJECT_SET_LAYOUT, CMD_DESTROY_SET_LAYOUT);
	if (status != 0)
		return -1;

	/* Allocate one descriptor set from one combined-image-sampler pool entry. */
	begin_command(CMD_CREATE_DESCRIPTOR_POOL);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(33);
	wire_u32(0);
	wire_u32(1);
	wire_u32(1);
	wire_u64(1);
	wire_u32(1);
	wire_u32(1);
	create_output(OBJECT_DESCRIPTOR_POOL);
	status = finish_creation(OBJECT_DESCRIPTOR_POOL, CMD_DESTROY_DESCRIPTOR_POOL);
	if (status != 0)
		return -1;

	/* The pool owns the descriptor set's lifetime. */
	begin_command(CMD_ALLOCATE_SETS);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(34);
	wire_u64(OBJECT_DESCRIPTOR_POOL);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_SET_LAYOUT);
	wire_u64(1);
	wire_u64(OBJECT_DESCRIPTOR_SET);
	status = finish_creation(OBJECT_DESCRIPTOR_SET, 0);
	if (status != 0)
		return -1;

	/* Point the binding at the real uploaded image and nearest sampler. */
	begin_command(CMD_UPDATE_SETS);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u32(1);
	wire_u64(1);
	wire_structure(35);
	wire_u64(OBJECT_DESCRIPTOR_SET);
	wire_u32(0);
	wire_u32(0);
	wire_u32(1);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_SAMPLER);
	wire_u64(OBJECT_TEXTURE_VIEW);
	wire_u32(5);
	wire_u64(0);
	wire_u64(0);
	wire_u32(0);
	wire_u64(0);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Reserve one float of push constants for the vertex shader's time input. */
	begin_command(CMD_CREATE_PIPELINE_LAYOUT);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(30);
	wire_u32(0);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_SET_LAYOUT);
	wire_u32(1);
	wire_u64(1);
	wire_u32(1);
	wire_u32(0);
	wire_u32(4);
	create_output(OBJECT_PIPELINE_LAYOUT);
	status = finish_creation(OBJECT_PIPELINE_LAYOUT, CMD_DESTROY_PIPELINE_LAYOUT);
	if (status != 0)
		return -1;

	/* Succeeded: shader inputs have explicit descriptor and push-constant layouts. */
	return 0;
}

/* Define the color/depth pass and its attachment-to-transfer dependency. */
static int
create_render_pass(
	void)
{
	int status;

	/* Declare two single-sample attachments cleared at the start of each frame. */
	begin_command(CMD_CREATE_RENDER_PASS);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(38);
	wire_u32(0);
	wire_u32(2);
	wire_u64(2);

	/* Store the color attachment and finish in transfer-source layout. */
	wire_u32(0);
	wire_u32(FORMAT_RGBA8);
	wire_u32(1);
	wire_u32(1);
	wire_u32(0);
	wire_u32(2);
	wire_u32(1);
	wire_u32(0);
	wire_u32(6);

	/* Depth is cleared and tested, then discarded after resolving visible faces. */
	wire_u32(0);
	wire_u32(FORMAT_DEPTH32);
	wire_u32(1);
	wire_u32(1);
	wire_u32(1);
	wire_u32(2);
	wire_u32(1);
	wire_u32(0);
	wire_u32(3);

	/* One graphics subpass writes color attachment zero and depth attachment one. */
	wire_u32(1);
	wire_u64(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u64(0);
	wire_u32(1);
	wire_u64(1);
	wire_u32(0);
	wire_u32(2);
	wire_u64(0);
	wire_u64(1);
	wire_u32(1);
	wire_u32(3);
	wire_u32(0);
	wire_u64(0);

	/* Make attachment writes available before the subsequent image readback. */
	wire_u32(2);
	wire_u64(2);
	wire_u32(VK_IGNORED);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0x700U);
	wire_u32(0);
	wire_u32(0x500U);
	wire_u32(0);
	wire_u32(0);
	wire_u32(VK_IGNORED);
	wire_u32(0x400U);
	wire_u32(0x1000U);
	wire_u32(0x100U);
	wire_u32(0x800U);
	wire_u32(0);
	create_output(OBJECT_RENDER_PASS);
	status = finish_creation(OBJECT_RENDER_PASS, CMD_DESTROY_RENDER_PASS);
	if (status != 0)
		return -1;

	/* Succeeded: color output is synchronized with the following transfer. */
	return 0;
}

/* Attach the retained color and depth image views to the fixed-size pass. */
static int
create_framebuffer(
	void)
{
	int status;

	/* Match framebuffer attachment order to the render-pass descriptions. */
	begin_command(CMD_CREATE_FRAMEBUFFER);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(37);
	wire_u32(0);
	wire_u64(OBJECT_RENDER_PASS);
	wire_u32(2);
	wire_u64(2);
	wire_u64(OBJECT_COLOR_VIEW);
	wire_u64(OBJECT_DEPTH_VIEW);
	wire_u32(VKDEMO_WIDTH);
	wire_u32(VKDEMO_HEIGHT);
	wire_u32(1);
	create_output(OBJECT_FRAMEBUFFER);
	status = finish_creation(OBJECT_FRAMEBUFFER, CMD_DESTROY_FRAMEBUFFER);
	if (status != 0)
		return -1;

	/* Succeeded: the render pass now targets the retained frame images. */
	return 0;
}

/* Upload one validated original SPIR-V module using its explicit byte size. */
static int
create_shader(
	uint32_t object,
	const uint32_t *words,
	uint32_t bytes)
{
	uint32_t index;
	int status;

	/* codeSize is bytes, while the following array cardinality is uint32 words. */
	begin_command(CMD_CREATE_SHADER);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(16);
	wire_u32(0);
	wire_u64(bytes);
	wire_u64(bytes / 4U);

	/* Encode each SPIR-V word through the shared endian-aware scalar codec. */
	for (index = 0; index < bytes / 4U; index++)
		wire_u32(words[index]);

	/* Register the shader object only after Vulkan accepts the complete module. */
	create_output(object);
	status = finish_creation(object, CMD_DESTROY_SHADER);
	if (status != 0)
		return -1;

	/* Succeeded: the original SPIR-V is available for graphics pipeline linkage. */
	return 0;
}

/* Encode one unspecialized shader stage using its ordinary main entry point. */
static void
pipeline_stage(
	uint32_t stage,
	uint32_t module)
{
	/* Encode the five-byte main string with its mandatory four-byte padding. */
	wire_structure(18);
	wire_u32(0);
	wire_u32(stage);
	wire_u64(module);
	wire_u64(5);
	wire_u32(0x6e69616dU);
	wire_u32(0);
	wire_u64(0);

	/* Succeeded: this stage references its module and complete entry-point name. */
	return;
}

/* Describe interleaved three-float positions and two-float texture coordinates. */
static void
pipeline_vertices(
	void)
{
	/* Binding zero advances one twenty-byte record per vertex. */
	wire_u64(1);
	wire_structure(19);
	wire_u32(0);
	wire_u32(1);
	wire_u64(1);
	wire_u32(0);
	wire_u32(VERTEX_STRIDE);
	wire_u32(0);

	/* Location zero is R32G32B32_SFLOAT position at byte zero. */
	wire_u32(2);
	wire_u64(2);
	wire_u32(0);
	wire_u32(0);
	wire_u32(106);
	wire_u32(0);

	/* Location one is R32G32_SFLOAT UV at byte twelve. */
	wire_u32(1);
	wire_u32(0);
	wire_u32(103);
	wire_u32(12);

	/* Assemble the thirty-six explicit vertices as twelve independent triangles. */
	wire_u64(1);
	wire_structure(20);
	wire_u32(0);
	wire_u32(3);
	wire_u32(0);

	/* Omit tessellation state because this pipeline uses only triangles. */
	wire_u64(0);

	/* Succeeded: the input layout matches the original uploaded cuboid records. */
	return;
}

/* Bake one full-frame Vulkan viewport and scissor into the graphics pipeline. */
static void
pipeline_viewport(
	void)
{
	/* Use positive viewport height; the vertex shader explicitly flips clip Y. */
	wire_u64(1);
	wire_structure(22);
	wire_u32(0);
	wire_u32(1);
	wire_u64(1);
	wire_float(0);
	wire_float(0);
	wire_float(VKDEMO_WIDTH);
	wire_float(VKDEMO_HEIGHT);
	wire_float(0);
	wire_float(1);

	/* Restrict fragments to the complete fixed-size output image. */
	wire_u32(1);
	wire_u64(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(VKDEMO_WIDTH);
	wire_u32(VKDEMO_HEIGHT);

	/* Succeeded: every framebuffer pixel follows the documented viewport mapping. */
	return;
}

/* Select ordinary filled triangles without optional rasterization features. */
static void
pipeline_rasterization(
	void)
{
	/* Cull neither face; depth testing alone chooses the visible box surfaces. */
	wire_u64(1);
	wire_structure(23);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_float(0);
	wire_float(0);
	wire_float(0);
	wire_float(1);

	/* Use one sample with no sample mask, sample shading or alpha coverage. */
	wire_u64(1);
	wire_structure(24);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0);
	wire_float(0);
	wire_u64(0);
	wire_u32(0);
	wire_u32(0);

	/* Succeeded: rasterization needs no optional Vulkan device feature. */
	return;
}

/* Resolve frontmost surfaces with LESS depth comparison and depth writes. */
static void
pipeline_depth(
	void)
{
	uint32_t face;

	/* Enable ordinary depth tests while leaving bounds and stencil disabled. */
	wire_u64(1);
	wire_structure(25);
	wire_u32(0);
	wire_u32(1);
	wire_u32(1);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);

	/* Encode valid inert front and back stencil state despite stencil being off. */
	for (face = 0; face < 2U; face++) {
		wire_u32(0);
		wire_u32(0);
		wire_u32(0);
		wire_u32(7);
		wire_u32(0);
		wire_u32(0);
		wire_u32(0);
	}

	/* Retain the conventional zero-to-one depth interval. */
	wire_float(0);
	wire_float(1);

	/* Succeeded: cuboid visibility is independent of triangle submission order. */
	return;
}

/* Store unblended fragment colors with all four output components enabled. */
static void
pipeline_blending(
	void)
{
	/* Disable logic operations and blending for the one color attachment. */
	wire_u64(1);
	wire_structure(26);
	wire_u32(0);
	wire_u32(0);
	wire_u32(3);
	wire_u32(1);
	wire_u64(1);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(15);

	/* Fixed-size arrays still carry their cardinality on the Venus wire. */
	wire_u64(4);
	wire_float(0);
	wire_float(0);
	wire_float(0);
	wire_float(0);

	/* Succeeded: stored RGBA bytes directly represent the fragment shader output. */
	return;
}

/* Link the original vertex and fragment modules into one depth-tested pipeline. */
static int
create_pipeline(
	void)
{
	int status;

	/* Build one graphics pipeline without a cache or optional extension chains. */
	begin_command(CMD_CREATE_PIPELINES);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(0);
	wire_u32(1);
	wire_u64(1);
	wire_structure(28);
	wire_u32(0);
	wire_u32(2);
	wire_u64(2);
	pipeline_stage(1, OBJECT_VERTEX_SHADER);
	pipeline_stage(0x10U, OBJECT_FRAGMENT_SHADER);

	/* Describe the complete fixed-function state used by the scene. */
	pipeline_vertices();
	pipeline_viewport();
	pipeline_rasterization();
	pipeline_depth();
	pipeline_blending();

	/* All viewport and scissor state is baked; no dynamic-state record is needed. */
	wire_u64(0);
	wire_u64(OBJECT_PIPELINE_LAYOUT);
	wire_u64(OBJECT_RENDER_PASS);
	wire_u32(0);
	wire_u64(0);
	wire_u32(VK_IGNORED);
	create_output(OBJECT_PIPELINE);
	status = finish_creation(OBJECT_PIPELINE, CMD_DESTROY_PIPELINE);
	if (status != 0)
		return -1;

	/* Report actual successful linkage rather than just shader file presence. */
	printf(
		"VKDEMO PIPELINE vertex_bytes=%u fragment_bytes=%u linked=1\n",
		(unsigned)sizeof(vkdemo_vertex_shader),
		(unsigned)sizeof(vkdemo_fragment_shader));
	fflush(stdout);

	/* Succeeded: the graphics pipeline can execute both original shaders. */
	return 0;
}

/* Allocate one reusable primary command buffer and one real completion fence. */
static int
create_commands(
	void)
{
	int status;

	/* The selected graphics family owns this command pool. */
	begin_command(CMD_CREATE_COMMAND_POOL);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(39);
	wire_u32(0);
	wire_u32(renderer.client.queue_family);
	create_output(OBJECT_COMMAND_POOL);
	status = finish_creation(OBJECT_COMMAND_POOL, CMD_DESTROY_COMMAND_POOL);
	if (status != 0)
		return -1;

	/* The pool owns the lifetime of the one primary command buffer. */
	begin_command(CMD_ALLOCATE_COMMAND_BUFFERS);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(40);
	wire_u64(OBJECT_COMMAND_POOL);
	wire_u32(0);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_COMMAND_BUFFER);
	status = finish_creation(OBJECT_COMMAND_BUFFER, 0);
	if (status != 0)
		return -1;

	/* Start unsignaled so the first texture upload has a genuine completion gate. */
	begin_command(CMD_CREATE_FENCE);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(1);
	wire_structure(8);
	wire_u32(0);
	create_output(OBJECT_FENCE);
	status = finish_creation(OBJECT_FENCE, CMD_DESTROY_FENCE);
	if (status != 0)
		return -1;

	/* Succeeded: upload and all later frames can share the same pool and fence. */
	return 0;
}

/* Begin recording one complete upload or graphics submission. */
static int
begin_recording(
	void)
{
	int status;

	/* Record one-time-submit commands after the pool is new or reset. */
	begin_command(CMD_BEGIN_COMMAND_BUFFER);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u64(1);
	wire_structure(42);
	wire_u32(1);
	wire_u64(0);
	status = finish_command(1);
	if (status != 0)
		return -1;

	/* Succeeded: subsequent commands belong to this primary recording. */
	return 0;
}

/* Submit the recorded command buffer and wait for its actual Vulkan fence. */
static int
submit_recording(
	void)
{
	uint32_t attempt;
	int status;

	/* Seal the command buffer before the queue can execute it. */
	begin_command(CMD_END_COMMAND_BUFFER);
	wire_u64(OBJECT_COMMAND_BUFFER);
	status = finish_command(1);
	if (status != 0)
		return -1;

	/* Submit one command buffer without semaphore dependencies. */
	begin_command(CMD_SUBMIT);
	wire_u64(VENUS_OBJECT_QUEUE);
	wire_u32(1);
	wire_u64(1);
	wire_structure(4);
	wire_u32(0);
	wire_u64(0);
	wire_u64(0);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u32(0);
	wire_u64(0);
	wire_u64(OBJECT_FENCE);

	/* Pending forbids normal teardown even if the submit response times out. */
	renderer.pending = 1;
	status = finish_command(1);
	if (status != 0)
		return -1;

	/* Observe Vulkan execution independently of the Venus stream completion. */
	for (attempt = 0; attempt < VENUS_CLIENT_POLL_LIMIT; attempt++) {
		/* Poll the one fence attached to this exact submission. */
		begin_command(CMD_FENCE_STATUS);
		wire_u64(VENUS_OBJECT_DEVICE);
		wire_u64(OBJECT_FENCE);
		status = venus_client_command_finish(&renderer.client, 1, 1);
		if (status == 0)
			break;

		/* Only VK_NOT_READY means that the GPU still owns the resources. */
		if (status < 0)
			return -1;

		/* A positive non-pending result is not a valid fence response. */
		if (status != 1) {
			errno = EIO;
			return -1;
		}

		/* Yield before another bounded fence query. */
		status = venus_client_poll_pause(&renderer.client);
		if (status != 0)
			return -1;
	}

	/* A bounded wait cannot turn an unfinished submission into a valid frame. */
	if (attempt == VENUS_CLIENT_POLL_LIMIT) {
		errno = ETIMEDOUT;
		return -1;
	}

	/* Fence completion returns ownership of the command pool and resources. */
	renderer.pending = 0;

	/* Succeeded: all commands in this submission have completed on the GPU. */
	return 0;
}

/* Record one color-image layout transition with explicit access dependencies. */
static int
image_barrier(
	uint32_t image,
	uint32_t old_layout,
	uint32_t new_layout,
	uint32_t source_access,
	uint32_t destination_access,
	uint32_t source_stage,
	uint32_t destination_stage)
{
	int status;

	/* Select one complete color mip and layer without changing queue ownership. */
	begin_command(CMD_PIPELINE_BARRIER);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u32(source_stage);
	wire_u32(destination_stage);
	wire_u32(0);
	wire_u32(0);
	wire_u64(0);
	wire_u32(0);
	wire_u64(0);
	wire_u32(1);
	wire_u64(1);
	wire_structure(45);
	wire_u32(source_access);
	wire_u32(destination_access);
	wire_u32(old_layout);
	wire_u32(new_layout);
	wire_u32(VK_IGNORED);
	wire_u32(VK_IGNORED);
	wire_u64(image);
	wire_u32(1);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0);
	wire_u32(1);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Succeeded: the recording contains the requested image dependency. */
	return 0;
}

/* Record visibility between host, transfer and vertex uses of one buffer. */
static int
buffer_barrier(
	uint32_t buffer,
	uint32_t bytes,
	uint32_t source_access,
	uint32_t destination_access,
	uint32_t source_stage,
	uint32_t destination_stage)
{
	int status;

	/* Limit the dependency to this buffer's valid application byte range. */
	begin_command(CMD_PIPELINE_BARRIER);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u32(source_stage);
	wire_u32(destination_stage);
	wire_u32(0);
	wire_u32(0);
	wire_u64(0);
	wire_u32(1);
	wire_u64(1);
	wire_structure(44);
	wire_u32(source_access);
	wire_u32(destination_access);
	wire_u32(VK_IGNORED);
	wire_u32(VK_IGNORED);
	wire_u64(buffer);
	wire_u64(0);
	wire_u64(bytes);
	wire_u32(0);
	wire_u64(0);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Succeeded: buffer users have an explicit memory visibility dependency. */
	return 0;
}

/* Transfer the original checker staging bytes into the actual sampled image. */
static int
upload_texture(
	void)
{
	int status;

	/* Record the texture upload in the newly allocated command buffer. */
	status = begin_recording();
	if (status != 0)
		return -1;

	/* Publish coherent CPU uploads to transfer reads and vertex attribute fetch. */
	status = buffer_barrier(
		OBJECT_UPLOAD,
		UPLOAD_BYTES,
		0x4000U,
		0x804U,
		0x4000U,
		0x1004U);
	if (status != 0)
		return -1;

	/* Transition the new texture before its one initial transfer write. */
	status = image_barrier(OBJECT_TEXTURE, 0, 7, 0, 0x1000U, 1, 0x1000U);
	if (status != 0)
		return -1;

	/* Copy the full sixty-four-square checker from its aligned staging offset. */
	begin_command(CMD_COPY_BUFFER_TO_IMAGE);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u64(OBJECT_UPLOAD);
	wire_u64(OBJECT_TEXTURE);
	wire_u32(7);
	wire_u32(1);
	wire_u64(1);
	wire_u64(TEXTURE_OFFSET);
	wire_u32(TEXTURE_WIDTH);
	wire_u32(TEXTURE_WIDTH);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(TEXTURE_WIDTH);
	wire_u32(TEXTURE_WIDTH);
	wire_u32(1);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Make the completed transfer available to fragment-stage texture sampling. */
	status = image_barrier(OBJECT_TEXTURE, 7, 5, 0x1000U, 0x20U, 0x1000U, 0x80U);
	if (status != 0)
		return -1;

	/* Finish the initial upload before any draw references the texture. */
	status = submit_recording();
	if (status != 0)
		return -1;

	/* Succeeded: every later frame samples the retained GPU texture image. */
	return 0;
}

/* Reuse the existing command pool and fence after their preceding completion. */
static int
reset_commands(
	void)
{
	int status;

	/* Never reset an object still owned by a pending GPU submission. */
	if (renderer.pending != 0) {
		errno = EBUSY;
		return -1;
	}

	/* Reset the completed fence to guard this frame's future submission. */
	begin_command(CMD_RESET_FENCES);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_FENCE);
	status = finish_command(1);
	if (status != 0)
		return -1;

	/* Return the primary command buffer to initial state without reallocating it. */
	begin_command(CMD_RESET_COMMAND_POOL);
	wire_u64(VENUS_OBJECT_DEVICE);
	wire_u64(OBJECT_COMMAND_POOL);
	wire_u32(0);
	status = finish_command(1);
	if (status != 0)
		return -1;

	/* Succeeded: the retained pool and fence are ready for a fresh frame. */
	return 0;
}

/* Start the color/depth pass with the documented opaque background. */
static int
begin_render_pass(
	void)
{
	int status;

	/* Cover the entire framebuffer with two clear-value records. */
	begin_command(CMD_BEGIN_RENDER_PASS);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u64(1);
	wire_structure(43);
	wire_u64(OBJECT_RENDER_PASS);
	wire_u64(OBJECT_FRAMEBUFFER);
	wire_u32(0);
	wire_u32(0);
	wire_u32(VKDEMO_WIDTH);
	wire_u32(VKDEMO_HEIGHT);
	wire_u32(2);
	wire_u64(2);

	/* VkClearValue color selects a nested float32[4] color union. */
	wire_u32(0);
	wire_u32(0);
	wire_u64(4);
	wire_float(16.0f / 255.0f);
	wire_float(24.0f / 255.0f);
	wire_float(40.0f / 255.0f);
	wire_float(1);

	/* VkClearValue depth/stencil needs a depth of one for LESS comparison. */
	wire_u32(1);
	wire_float(1);
	wire_u32(0);
	wire_u32(0);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Succeeded: subsequent graphics commands target the cleared attachments. */
	return 0;
}

/* Copy the rendered color image into the retained coherent readback buffer. */
static int
copy_frame(
	void)
{
	int status;

	/* The render-pass dependency makes its final transfer-source image readable. */
	begin_command(CMD_COPY_IMAGE_TO_BUFFER);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u64(OBJECT_COLOR);
	wire_u32(6);
	wire_u64(OBJECT_READBACK);
	wire_u32(1);
	wire_u64(1);
	wire_u64(0);
	wire_u32(VKDEMO_WIDTH);
	wire_u32(VKDEMO_HEIGHT);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);
	wire_u32(0);
	wire_u32(VKDEMO_WIDTH);
	wire_u32(VKDEMO_HEIGHT);
	wire_u32(1);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Coherent host reads become valid after both this barrier and the fence. */
	status = buffer_barrier(
		OBJECT_READBACK,
		VKDEMO_BYTES,
		0x1000U,
		0x2000U,
		0x1000U,
		0x4000U);
	if (status != 0)
		return -1;

	/* Succeeded: the frame recording includes full image readback and visibility. */
	return 0;
}

/* Record a real textured draw at the caller's exact reported shader time. */
static int
record_frame(
	uint32_t milliseconds)
{
	float seconds;
	int status;

	/* Begin a fresh recording in the retained, completed command pool. */
	status = begin_recording();
	if (status != 0)
		return -1;

	/* Keep vertex fetch visibility explicit when reusing the coherent upload. */
	status = buffer_barrier(OBJECT_UPLOAD, UPLOAD_BYTES, 0x4000U, 4, 0x4000U, 4);
	if (status != 0)
		return -1;

	/* Clear color and depth through the real Vulkan render pass. */
	status = begin_render_pass();
	if (status != 0)
		return -1;

	/* Select the linked vertex/fragment graphics pipeline. */
	begin_command(CMD_BIND_PIPELINE);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u32(0);
	wire_u64(OBJECT_PIPELINE);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Bind the actual uploaded position and face-UV vertex records. */
	begin_command(CMD_BIND_VERTEX_BUFFERS);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u32(0);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_UPLOAD);
	wire_u64(1);
	wire_u64(0);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Make the combined image sampler visible to the fragment shader. */
	begin_command(CMD_BIND_DESCRIPTOR_SETS);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u32(0);
	wire_u64(OBJECT_PIPELINE_LAYOUT);
	wire_u32(0);
	wire_u32(1);
	wire_u64(1);
	wire_u64(OBJECT_DESCRIPTOR_SET);
	wire_u32(0);
	wire_u64(0);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Pass elapsed time directly; the vertex shader performs rotation and projection. */
	seconds = (float)milliseconds / 1000.0f;
	begin_command(CMD_PUSH_CONSTANTS);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u64(OBJECT_PIPELINE_LAYOUT);
	wire_u32(1);
	wire_u32(0);
	wire_u32(4);
	wire_u64(4);
	wire_float(seconds);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Execute twelve textured triangles with the real vertex and fragment stages. */
	begin_command(CMD_DRAW);
	wire_u64(OBJECT_COMMAND_BUFFER);
	wire_u32(VERTEX_COUNT);
	wire_u32(1);
	wire_u32(0);
	wire_u32(0);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* End attachment use and perform the pass's final transfer-source transition. */
	begin_command(CMD_END_RENDER_PASS);
	wire_u64(OBJECT_COMMAND_BUFFER);
	status = finish_command(0);
	if (status != 0)
		return -1;

	/* Record the complete GPU-generated frame into the retained readback buffer. */
	status = copy_frame();
	if (status != 0)
		return -1;

	/* Succeeded: this recording contains shader execution and its output copy. */
	return 0;
}

/* Hash opaque GPU pixels in the same RGB row order used by the host capture. */
static int
hash_pixels(
	char digest[65])
{
	static const char hexadecimal[] = "0123456789abcdef";
	struct command_sha256_context hash;
	uint8_t row[VKDEMO_WIDTH * 3U];
	uint8_t binary[32];
	uint32_t x;
	uint32_t y;
	uint32_t offset;
	uint32_t index;
	int status;

	/* Start an independent SHA256 over RGB only, excluding stored alpha. */
	command_sha256_init(&hash);

	/* Preserve top-to-bottom, left-to-right framebuffer order. */
	for (y = 0; y < VKDEMO_HEIGHT; y++) {
		/* Extract RGB without repairing, recoloring or substituting any GPU byte. */
		for (x = 0; x < VKDEMO_WIDTH; x++) {
			/* Both the background and fragment shader must remain fully opaque. */
			offset = (y * VKDEMO_WIDTH + x) * 4U;
			if (renderer.pixels[offset + 3U] != 255) {
				errno = EIO;
				return -1;
			}

			/* Feed the exact displayed color components into the row digest. */
			row[x * 3U] = renderer.pixels[offset];
			row[x * 3U + 1U] = renderer.pixels[offset + 1U];
			row[x * 3U + 2U] = renderer.pixels[offset + 2U];
		}

		/* Hash one complete RGB row using the existing base SHA256 implementation. */
		status = command_sha256_update(&hash, row, sizeof(row));
		if (status != 0)
			return -1;
	}

	/* Finalize the actual frame digest before formatting its portable text form. */
	command_sha256_final(&hash, binary);

	/* Encode exactly sixty-four lowercase hexadecimal digits for capture matching. */
	for (index = 0; index < 32U; index++) {
		digest[index * 2U] = hexadecimal[binary[index] >> 4];
		digest[index * 2U + 1U] = hexadecimal[binary[index] & 15U];
	}

	/* Terminate the printable digest without modifying the framebuffer bytes. */
	digest[64] = '\0';

	/* Succeeded: the digest identifies every RGB byte of the real GPU readback. */
	return 0;
}

/* Present the same completed GPU pixels whose digest was just calculated. */
static int
present_pixels(
	uint32_t frame)
{
	struct gpu_present request;
	int status;

	/* Copy unmodified readback bytes into retained packed-pixel scanout storage. */
	status = venus_client_resource_copy(
		&renderer.client,
		renderer.presentation,
		0,
		renderer.pixels,
		VKDEMO_BYTES,
		1);
	if (status != 0)
		return -1;

	/* Publish the whole fixed-size image through the existing copy-based presentation interface. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = renderer.presentation;
	request.width = VKDEMO_WIDTH;
	request.height = VKDEMO_HEIGHT;
	request.stride = VKDEMO_WIDTH * 4U;
	request.format = GPU_PIXEL_RGBA8888;
	request.frame = frame;
	status = ioctl(renderer.client.fd, GPU_PRESENT, &request);
	if (status != 0)
		return -1;

	/* Succeeded: scanout now contains the exact completed Vulkan framebuffer. */
	return 0;
}

/* Retire completed exports, dependent Vulkan objects, then their bound memory. */
static int
release_resources(
	void)
{
	struct gpu_resource_destroy request;
	uint64_t handles[3];
	uint32_t index;
	uint32_t command;
	uint32_t object;
	uint32_t pass;
	int status;

	/* Retire application copies while the reply blob remains alive for cleanup. */
	handles[0] = renderer.presentation;
	handles[1] = renderer.readback_blob;
	handles[2] = renderer.upload_blob;

	/* Drop each exported allocation once before freeing its Vulkan memory. */
	for (index = 0; index < 3U; index++) {
		/* Destroy only handles acquired by this successfully initialized session. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		request.handle = handles[index];
		status = ioctl(renderer.client.fd, GPU_RESOURCE_DESTROY, &request);
		if (status != 0)
			return -1;
	}

	/* Clear consumed handles before retiring the Vulkan ownership graph. */
	renderer.presentation = 0;
	renderer.readback_blob = 0;
	renderer.upload_blob = 0;

	/* Bound resources must all be destroyed before any allocation is freed. */
	for (pass = 0; pass < 2U; pass++) {
		/* Reverse creation order handles pipelines, pools, views and images safely. */
		for (index = renderer.release_count; index != 0; index--) {
			/* Select non-memory objects first and memory allocations second. */
			command = renderer.releases[index - 1U].command;
			object = renderer.releases[index - 1U].object;
			if (pass == 0) {
				/* Keep bound allocations alive until all non-memory objects are gone. */
				if (command == CMD_FREE_MEMORY)
					continue;
			}

			/* Keep the allocation pass limited to the now-unbound Vulkan memory. */
			if (pass == 1) {
				/* Non-memory objects were already consumed by the first pass. */
				if (command != CMD_FREE_MEMORY)
					continue;
			}

			/* All retained destructors take device, object and a null allocator. */
			begin_command(command);
			wire_u64(VENUS_OBJECT_DEVICE);
			wire_u64(object);
			wire_u64(0);
			status = finish_command(0);
			if (status != 0)
				return -1;
		}
	}

	/* Zero means that no application Vulkan object remains in the cleanup ledger. */
	renderer.release_count = 0;

	/* Succeeded: only the shared bootstrap context remains for client_close. */
	return 0;
}
