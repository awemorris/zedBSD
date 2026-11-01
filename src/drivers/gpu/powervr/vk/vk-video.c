/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's Vulkan Video H.264 decode on the board's video engine
 * (drv_powervr_video_ops, video-state.c).
 *
 * The wire is libvulkan's (userland/desktop/libvulkan/video.c), the same
 * the i915, bcm2711 and nvrtx executors read: a structure is its type, its
 * chain (a presence word and the next structure, nested), then its
 * fields; a pointer is a presence word and what it points to; an array is
 * a 64-bit count and its elements; the H.264 flags are one word packed by
 * name.
 *
 * The capabilities, the formats, the sessions and the parameters are
 * answered at once.  The four video commands are kept in the command
 * buffer's records, each in a structure of its own (entry->data) that
 * holds the objects it names, copied out of the wire.  A submission runs
 * a decode in its place among the records (vk-queue.c ends the session's
 * GPU work before it): the engine decodes the picture into a picture of
 * the session's pool and waits for it, then the transfer copies that
 * picture's two planes into the decode's output image, which, the DPB and
 * the output coinciding, is its setup slot's.  The engine is stateful: it
 * keeps the references itself, and is told only which slots each decode
 * names, so the picture goes into a pool picture no live slot holds.
 *
 * A use that breaks the API's rules (a decode outside a coding scope, too
 * many references) fails the recording.  A bitstream value the engine
 * cannot take (a set the parameters do not hold, a field picture, a range
 * past the buffer) only makes the picture an error, with a log line and
 * the ERROR status: an application may hand an untrusted stream to the
 * decoder.
 *
 * The pool is the session's: NV12 pictures in buffers of the render node
 * mapped around the system level cache (the engine's core writes memory
 * the GPU then reads) and below 4 GiB, made when the engine knows the
 * sequence.  It goes when the session goes, once the session's GPU work
 * that may read it has finished, or is kept when the engine says its core
 * may still write it.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/nvrtx/vk/vk-video.c and
 * src/drivers/gpu/bcm2711/vk/vulkan-video.c (the same wire read the same
 * way, the parameters' sets kept the same way, the records held the same
 * way).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/powervr.h"
#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/video-state.h"
#include "drivers/gpu/powervr/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The structure types the video wire carries (the Vulkan registry's values). */
#define VIDEO_TYPE_PROFILE			1000023000U
#define VIDEO_TYPE_CAPABILITIES			1000023001U
#define VIDEO_TYPE_PICTURE_RESOURCE		1000023002U
#define VIDEO_TYPE_MEMORY_REQUIREMENTS		1000023003U
#define VIDEO_TYPE_BIND_MEMORY			1000023004U
#define VIDEO_TYPE_SESSION_CREATE		1000023005U
#define VIDEO_TYPE_PARAMETERS_CREATE		1000023006U
#define VIDEO_TYPE_PARAMETERS_UPDATE		1000023007U
#define VIDEO_TYPE_BEGIN			1000023008U
#define VIDEO_TYPE_END				1000023009U
#define VIDEO_TYPE_CONTROL			1000023010U
#define VIDEO_TYPE_REFERENCE_SLOT		1000023011U
#define VIDEO_TYPE_PROFILE_LIST			1000023013U
#define VIDEO_TYPE_FORMAT_INFO			1000023014U
#define VIDEO_TYPE_FORMAT_PROPERTIES		1000023015U
#define VIDEO_TYPE_DECODE			1000024000U
#define VIDEO_TYPE_DECODE_CAPABILITIES		1000024001U
#define VIDEO_TYPE_DECODE_USAGE			1000024002U
#define VIDEO_TYPE_H264_CAPABILITIES		1000040000U
#define VIDEO_TYPE_H264_PICTURE			1000040001U
#define VIDEO_TYPE_H264_PROFILE			1000040003U
#define VIDEO_TYPE_H264_PARAMETERS_CREATE	1000040004U
#define VIDEO_TYPE_H264_PARAMETERS_ADD		1000040005U
#define VIDEO_TYPE_H264_DPB_SLOT		1000040006U

/* The flags and enumerants of the video API answered or checked. */
#define VIDEO_OP_DECODE_H264			0x1U
#define VIDEO_CHROMA_420			0x2U
#define VIDEO_DEPTH_8				0x1U
#define VIDEO_LAYOUT_PROGRESSIVE		0U
#define VIDEO_SEPARATE_REFERENCE_IMAGES		0x2U
#define VIDEO_DPB_AND_OUTPUT_COINCIDE		0x1U
#define VIDEO_CONTROL_RESET			0x1U

/* The H.264 profiles taken. */
#define VIDEO_PROFILE_BASELINE			66U
#define VIDEO_PROFILE_MAIN			77U
#define VIDEO_PROFILE_HIGH			100U

/* The video results the executor replies (the pinned header's values). */
#define VIDEO_ERROR_IMAGE_USAGE			(-1000023000)
#define VIDEO_ERROR_LAYOUT			(-1000023001)
#define VIDEO_ERROR_OPERATION			(-1000023002)
#define VIDEO_ERROR_FORMAT			(-1000023003)
#define VIDEO_ERROR_CODEC			(-1000023004)
#define VIDEO_ERROR_STD_VERSION			(-1000023005)

/* The result statuses a decode reports into its query. */
#define VIDEO_STATUS_COMPLETE			1
#define VIDEO_STATUS_ERROR			(-1)

/* The picture flags of libvulkan's packing (video_picture_flags): field, intra, IDR, bottom, reference, pair. */
#define VIDEO_PICTURE_FIELD			(1U << 0)
#define VIDEO_PICTURE_INTRA			(1U << 1)
#define VIDEO_PICTURE_IDR			(1U << 2)
#define VIDEO_PICTURE_PAIR			(1U << 5)

/*
 * The decode limits the executor reports and holds sessions to: any
 * bitstream offset, sizes in 16 bytes, 16x16 pictures, 64x64 to the
 * largest picture of level 5.1, 17 slots and 16 references, level 5.1
 * (StdVideoH264LevelIdc 14).
 *
 * Values referenced:
 * ITU-T H.264 (08/2021) A.3.1 Table A-1 (level 5.1: MaxFS 36864
 * macroblocks, 4096x2304), A.3.1 (MaxDpbFrames at most 16).
 */
#define VIDEO_OFFSET_ALIGN			1U
#define VIDEO_SIZE_ALIGN			16U
#define VIDEO_GRANULARITY			16U
#define VIDEO_MIN_SIDE				64U
#define VIDEO_MAX_WIDTH				4096U
#define VIDEO_MAX_HEIGHT			2304U
#define VIDEO_SLOTS				17U
#define VIDEO_REFERENCES			16U
#define VIDEO_LEVEL_5_1				14U
#define VIDEO_MB				16U

/* The H.264 header the decoder was written against: VK_STD_vulkan_video_codec_h264_decode 1.0.0. */
#define VIDEO_STD_NAME				"VK_STD_vulkan_video_codec_h264_decode"
#define VIDEO_STD_VERSION			((1U << 22) | (0U << 12) | 0U)

/* The parameter set id ranges, and the deepest chain a structure may carry. */
#define VIDEO_SPS_IDS				32U
#define VIDEO_PPS_IDS				256U
#define VIDEO_MAX_CHAIN				8U

/* The slots a begin record lists at most (each slot and one picture without a slot). */
#define VIDEO_BEGIN_SLOTS			(VIDEO_SLOTS + 1U)

/* The session's one memory binding, which nothing uses: the engine keeps its memory itself. */
#define VIDEO_BINDING_BYTES			4096U
#define VIDEO_BIND_ALIGN			256U

/* The most pictures of a pool (the engine's core registers at most 31 frame buffers). */
#define VIDEO_POOL_MAX				32U

/* The rows the transfer copies at most at once. */
#define VIDEO_COPY_ROWS				2048U

/* The log lines of pictures in error a boot writes at most. */
#define VIDEO_SKIP_LINES			32U

/* One picture parameter set a parameters object holds; the object's sets are a list. */
struct video_pps_node {
	struct video_pps_node *next;
	struct vpu_h264_pps pps;
};

/*
 * One VkVideoSessionParametersKHR: the H.264 sets of one session.  It
 * holds its session's object; the recorded commands that use it hold it.
 */
struct video_parameters {
	struct powervr_vk_object *session;
	uint32_t max_sps;
	uint32_t max_pps;
	uint32_t update_sequence;
	struct vpu_h264_sps *sps[VIDEO_SPS_IDS];
	struct video_pps_node *pps;
};

/*
 * One picture of a session's pool: the render node's buffer, the
 * session's mapping of it (its GPU address) and where its chroma plane
 * starts.
 */
struct video_pool_picture {
	struct powervr_buffer *buffer;
	struct powervr_resource *resource;
	uint64_t chroma_offset;
};

/*
 * One VkVideoSessionKHR.
 *
 * It keeps the limits it was made with, the engine's stream, its one
 * memory binding, the active slots of the coding scope its last begin
 * opened, and its pool: count pictures of one pitch for the coded size,
 * made by the engine's call from a decode.  kept says the pool stays
 * with the core when the session goes.  Everything changes only under
 * the session's serialized ioctls.
 */
struct video_session {
	struct powervr_vk_session *owner;
	uint32_t profile_idc;
	uint32_t width;
	uint32_t height;
	uint32_t slots;
	uint32_t references;
	void *stream;
	struct powervr_vk_object *binding;
	uint32_t active;
	uint32_t count;
	uint32_t pitch;
	struct video_pool_picture pool[VIDEO_POOL_MAX];
	bool kept;
};

/* A picture resource as recorded: its coded area, its layer and its view's identity. */
struct video_resource {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t layer;
	uint64_t view;
};

/*
 * One reference slot as recorded: its index (-1 for none), whether it
 * names a picture, the picture's view (held while the record lives).
 */
struct video_slot {
	int32_t index;
	bool has_picture;
	struct video_resource resource;
	struct powervr_vk_object *view;
};

/* The H.264 information of the picture a decode writes: its flags, the sets it names, its order counts. */
struct video_picture {
	uint32_t flags;
	uint32_t sps_id;
	uint32_t pps_id;
	int32_t poc[2];
};

/*
 * One recorded video command, kept in its record's data.
 *
 * It holds every object it names until its record is dropped: a begin its
 * session and parameters, a decode the session and parameters of its
 * coding scope, its bitstream buffer, its output view and its references'
 * views.  skip says why a decode the engine cannot take is an error.
 */
struct video_record {
	uint32_t opcode;
	struct powervr_vk_object *session;
	struct powervr_vk_object *parameters;
	struct powervr_vk_object *buffer;
	struct powervr_vk_object *view;
	uint32_t flags;
	uint64_t offset;
	uint64_t range;
	struct video_resource destination;
	bool has_setup;
	struct video_slot setup;
	uint32_t slot_count;
	struct video_slot slots[VIDEO_BEGIN_SLOTS];
	bool has_picture;
	struct video_picture picture;
	const char *skip;
};

/*
 * What the chained records of one structure carried.  It lives on the
 * stack of one command's decoding; the add record's sets are the
 * parameters object's to take, or freed with the chain.
 */
struct video_chain {
	bool has_h264_profile;
	uint32_t profile_idc;
	uint32_t picture_layout;
	bool has_profile_list;
	uint32_t profile_count;
	bool profiles_supported;
	int32_t profile_error;
	bool has_h264_create;
	uint32_t max_sps;
	uint32_t max_pps;
	struct vpu_h264_sps *new_sps[VIDEO_SPS_IDS];
	uint32_t new_sps_count;
	struct video_pps_node *new_pps;
	int add_error;
	bool has_picture;
	struct video_picture picture;
};

/* The pictures in error logged so far (a count only, a race costs a line at most). */
static uint32_t video_skip_lines;

static bool video_available(const struct powervr_vk_session *session);
static int video_capabilities(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void video_reply_capabilities(struct i915_wire_writer *reply, const uint32_t *types, uint32_t count);
static int video_format_properties(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_session_create(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_requirements(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_bind(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_parameters_create(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_parameters_update(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_session_destroy(struct powervr_vk_session *session, void *payload);
static int video_parameters_destroy(struct powervr_vk_session *session, void *payload);
static int video_pool_make(void *owner, uint32_t count, uint32_t width, uint32_t height, uint32_t *pitch, struct drv_powervr_video_picture *pictures);
static void video_pool_free(struct video_session *state);
static void video_read_chain(struct i915_wire_reader *reader, struct video_chain *chain, uint32_t depth);
static void video_read_fields(struct i915_wire_reader *reader, uint32_t type, struct video_chain *chain, uint32_t depth);
static int32_t video_read_profile(struct i915_wire_reader *reader, uint32_t *profile_idc);
static int32_t video_profile_result(uint32_t operation, uint32_t chroma, uint32_t luma, uint32_t chroma_depth, const struct video_chain *chain);
static void video_read_resource(struct i915_wire_reader *reader, struct video_resource *resource);
static void video_read_slot(struct i915_wire_reader *reader, struct video_slot *slot);
static void video_read_slots(struct i915_wire_reader *reader, struct video_record *video, uint32_t limit);
static void video_read_add(struct i915_wire_reader *reader, struct video_chain *chain);
static void video_read_sps(struct i915_wire_reader *reader, struct vpu_h264_sps *sps);
static void video_read_pps(struct i915_wire_reader *reader, struct vpu_h264_pps *pps);
static void video_read_scaling(struct i915_wire_reader *reader, struct vpu_h264_scaling *scaling);
static void video_read_extension(struct i915_wire_reader *reader, char *name, uint32_t *version);
static void video_chain_free(struct video_chain *chain);
static int video_parameters_take(struct video_parameters *parameters, struct video_chain *chain, bool replace);
static int video_parameters_copy(struct video_parameters *destination, const struct video_parameters *source);
static void video_parameters_free(struct video_parameters *parameters);
static const struct vpu_h264_pps *video_find_pps(const struct video_parameters *parameters, uint32_t sps_id, uint32_t pps_id);
static int video_hold(struct powervr_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity, struct powervr_vk_object **holder);
static int video_scope(const struct powervr_vk_command_buffer *command, struct video_record *video);
static void video_release(struct video_record *video);
static void video_run_begin(const struct video_record *video);
static void video_run_control(const struct video_record *video);
static int video_run_decode(struct powervr_vk_session *session, const struct video_record *video);
static const char *video_check(const struct video_record *video, const struct video_session *state, const struct video_parameters *parameters, const struct vpu_h264_sps **sps_out, const struct vpu_h264_pps **pps_out);
static const char *video_check_picture(const struct powervr_vk_object *view_object, const struct powervr_vk_image **image_out);
static int video_bitstream(const struct powervr_vk_buffer *buffer, uint64_t offset, const uint8_t **bytes);
static int video_copy(struct powervr_vk_session *session, const struct video_session *state, uint32_t index, const struct powervr_vk_image *image, const struct drv_powervr_video_result *result);
static void video_skip(const char *reason);

/*
 * Routes the video commands that run at once (the capabilities, the
 * formats, the sessions and the parameters); *handled stays clear for
 * another handler's opcode.
 */
int
powervr_vk_video_route(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	(void)requested;
	*handled = 1;
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES:
		error = video_capabilities(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES:
		error = video_format_properties(session, reader, reply);
		break;
	case GPU_OP_CREATE_VIDEO_SESSION:
		error = video_session_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_VIDEO_SESSION:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_VIDEO_SESSION);
		break;
	case GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS:
		error = video_requirements(session, reader, reply);
		break;
	case GPU_OP_BIND_VIDEO_SESSION_MEMORY:
		error = video_bind(session, reader, reply);
		break;
	case GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS:
		error = video_parameters_create(session, reader, reply);
		break;
	case GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS:
		error = video_parameters_update(session, reader, reply);
		break;
	case GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/* Reports whether an opcode is one of the recorded video commands. */
bool
powervr_vk_video_recorded(
	uint32_t opcode)
{
	/* The begin, the end, the control and the decode. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
	case GPU_OP_CMD_DECODE_VIDEO:
		return true;
	default:
		break;
	}

	/* Another handler's. */
	return false;
}

/*
 * Records one video command into a structure of its own kept in the
 * record's data, holding the objects it names.  command is the command
 * buffer being recorded (NULL when there is none: the command is decoded
 * and dropped).  EINVAL when the arguments did not decode, another error
 * when they decoded but cannot be run (the recording fails with it).
 */
int
powervr_vk_record_video(
	struct powervr_vk_session *session,
	const struct powervr_vk_command_buffer *command,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct video_record *video;
	struct video_chain chain;
	uint64_t present;
	uint64_t session_id;
	uint64_t parameters_id;
	uint64_t buffer_id;
	uint32_t type;
	uint32_t index;
	int error;

	/* The command's own structure, which the record keeps. */
	video = kern_calloc(1, sizeof(*video));
	if (video == NULL)
		return ENOMEM;
	video->opcode = opcode;
	entry->data = video;

	/* The structure's presence, its type and its chain (a decode's carries its picture). */
	kern_memset(&chain, 0, sizeof(chain));
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	video_chain_free(&chain);
	session_id = 0;
	parameters_id = 0;
	buffer_id = 0;

	/* The fields of the command's structure. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
		/* [flags][session][parameters][count][array]{slot}. */
		if (type != VIDEO_TYPE_BEGIN)
			reader->error = 1;
		video->flags = drv_i915_wire_read_u32(reader);
		session_id = drv_i915_wire_read_u64(reader);
		parameters_id = drv_i915_wire_read_u64(reader);
		video_read_slots(reader, video, VIDEO_BEGIN_SLOTS);
		break;
	case GPU_OP_CMD_END_VIDEO_CODING:
		/* [flags]. */
		if (type != VIDEO_TYPE_END)
			reader->error = 1;
		video->flags = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
		/* [flags]. */
		if (type != VIDEO_TYPE_CONTROL)
			reader->error = 1;
		video->flags = drv_i915_wire_read_u32(reader);
		break;
	default:
		/* vkCmdDecodeVideoKHR: [flags][buffer][offset][range][resource][present]{setup}[count][array]{slot}. */
		if (type != VIDEO_TYPE_DECODE)
			reader->error = 1;
		video->flags = drv_i915_wire_read_u32(reader);
		buffer_id = drv_i915_wire_read_u64(reader);
		video->offset = drv_i915_wire_read_u64(reader);
		video->range = drv_i915_wire_read_u64(reader);
		video_read_resource(reader, &video->destination);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			video->has_setup = true;
			video_read_slot(reader, &video->setup);
		}

		/* The reference slots, then the picture the chain carried. */
		video_read_slots(reader, video, VIDEO_REFERENCES);
		video->has_picture = chain.has_picture;
		video->picture = chain.picture;
		break;
	}

	/* A stream that did not decode is unreadable. */
	if (reader->error != 0)
		return EINVAL;

	/* Without a recording command buffer the command is decoded and dropped. */
	if (command == NULL)
		return 0;

	/* A begin holds its session and parameters; the others take them from the scope a begin opened. */
	if (opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING) {
		error = video_hold(session, I915_VK_OBJ_VIDEO_SESSION, session_id, &video->session);
		if (error == 0 && parameters_id != 0U)
			error = video_hold(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, parameters_id, &video->parameters);
	} else {
		error = video_scope(command, video);
	}
	if (error != 0)
		return ENOTSUP;

	/* Only a decode holds more. */
	if (opcode != GPU_OP_CMD_DECODE_VIDEO)
		return 0;

	/* Its bitstream and its output's view. */
	error = video_hold(session, I915_VK_OBJ_BUFFER, buffer_id, &video->buffer);
	if (error == 0)
		error = video_hold(session, I915_VK_OBJ_IMAGE_VIEW, video->destination.view, &video->view);
	if (error != 0)
		return ENOTSUP;

	/* Each reference's view: a reference names a picture. */
	for (index = 0; index < video->slot_count; index++) {
		if (!video->slots[index].has_picture)
			return ENOTSUP;
		error = video_hold(session, I915_VK_OBJ_IMAGE_VIEW, video->slots[index].resource.view, &video->slots[index].view);
		if (error != 0)
			return ENOTSUP;
	}

	/* Succeeded: the command holds what it names. */
	return 0;
}

/* Lets go of what a recorded video command holds, and frees its structure. */
void
powervr_vk_video_drop(
	struct powervr_vk_record *entry)
{
	struct video_record *video;

	/* Nothing was kept. */
	video = entry->data;
	if (video == NULL)
		return;

	/* Its objects, then its structure. */
	video_release(video);
	kern_free(video);
	entry->data = NULL;
}

/*
 * Runs one recorded video command in its submission's order; the
 * session's GPU work has ended before a decode (vk-queue.c).  EIO when
 * the engine stopped answering or a decode's copy could not run (the
 * submission is lost); a picture the engine cannot take is an error in
 * its query and 0 here.
 */
int
powervr_vk_video_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry)
{
	const struct video_record *video;
	int error;

	/* A record that kept nothing does nothing. */
	video = entry->data;
	if (video == NULL)
		return 0;

	/* Each command's work. */
	error = 0;
	switch (video->opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
		video_run_begin(video);
		break;
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
		video_run_control(video);
		break;
	case GPU_OP_CMD_DECODE_VIDEO:
		error = video_run_decode(session, video);
		break;
	default:
		break;
	}

	/* A decode the engine or the copy did not finish. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/* Reports whether the session has a video engine's H.264 decode, as it was when the session opened. */
static bool
video_available(
	const struct powervr_vk_session *session)
{
	/* Only with the decode the engine offered. */
	if ((session->owner->video_operations & DRV_POWERVR_VIDEO_DECODE_H264) == 0)
		return false;

	/* The session has it. */
	return true;
}

/*
 * vkGetPhysicalDeviceVideoCapabilitiesKHR: [physical][present]{profile}
 * [present][shape: type, link...] -> [result][present]{capabilities}.
 */
static int
video_capabilities(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t types[VIDEO_MAX_CHAIN];
	uint32_t count;
	uint32_t idc;
	uint64_t present;
	int32_t result;
	bool available;

	/* Skips the physical device and reads the profile, which decides the result. */
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	result = VIDEO_ERROR_OPERATION;
	if (present != 0U)
		result = video_read_profile(reader, &idc);

	/* The shape of the output: the capabilities record and what is chained to it. */
	count = 0;
	present = drv_i915_wire_read_u64(reader);
	while (present != 0U && reader->error == 0) {
		if (count >= VIDEO_MAX_CHAIN) {
			reader->error = 1;
			break;
		}

		/* The type of the next structure in the chain. */
		types[count] = drv_i915_wire_read_u32(reader);
		count++;
		present = drv_i915_wire_read_u64(reader);
	}

	/* Refuses a malformed chain. */
	if (reader->error != 0)
		return EINVAL;
	if (count == 0 || types[0] != VIDEO_TYPE_CAPABILITIES)
		return EINVAL;

	/* A session without the engine takes no profile. */
	available = video_available(session);
	if (!available)
		result = VIDEO_ERROR_OPERATION;

	/* A profile the engine does not take replies its reason only. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* The result and the records in the shape's order. */
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u64(reply, 1);
	video_reply_capabilities(reply, types, count);

	/* Succeeded: the capabilities are reported. */
	return 0;
}

/* Replies one output record of the capabilities' shape and, nested before its fields, the rest. */
static void
video_reply_capabilities(
	struct i915_wire_writer *reply,
	const uint32_t *types,
	uint32_t count)
{
	char name[VK_MAX_EXTENSION_NAME_SIZE];

	/* The record's type, then the link to the rest of the shape. */
	drv_i915_wire_reply_u32(reply, types[0]);
	if (count > 1U) {
		drv_i915_wire_reply_u64(reply, 1);
		video_reply_capabilities(reply, types + 1, count - 1U);
	} else {
		drv_i915_wire_reply_u64(reply, 0);
	}

	/* The record's fields. */
	switch (types[0]) {
	case VIDEO_TYPE_CAPABILITIES:
		/* Separate reference images; the bitstream's alignments; 16x16; the sizes and slot limits. */
		drv_i915_wire_reply_u32(reply, VIDEO_SEPARATE_REFERENCE_IMAGES);
		drv_i915_wire_reply_u64(reply, VIDEO_OFFSET_ALIGN);
		drv_i915_wire_reply_u64(reply, VIDEO_SIZE_ALIGN);
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_WIDTH);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_HEIGHT);
		drv_i915_wire_reply_u32(reply, VIDEO_SLOTS);
		drv_i915_wire_reply_u32(reply, VIDEO_REFERENCES);

		/* The H.264 header name, padded to its array, and version. */
		kern_memset(name, 0, sizeof(name));
		kern_memcpy(name, VIDEO_STD_NAME, sizeof(VIDEO_STD_NAME));
		drv_i915_wire_reply_u64(reply, VK_MAX_EXTENSION_NAME_SIZE);
		drv_i915_wire_reply_bytes(reply, name, sizeof(name));
		drv_i915_wire_reply_u32(reply, VIDEO_STD_VERSION);
		break;
	case VIDEO_TYPE_DECODE_CAPABILITIES:
		/* The output picture is the reference picture (its contents a copy of the engine's). */
		drv_i915_wire_reply_u32(reply, VIDEO_DPB_AND_OUTPUT_COINCIDE);
		break;
	case VIDEO_TYPE_H264_CAPABILITIES:
		/* Level 5.1 and no field offset (progressive only). */
		drv_i915_wire_reply_u32(reply, VIDEO_LEVEL_5_1);
		drv_i915_wire_reply_u32(reply, 0);
		drv_i915_wire_reply_u32(reply, 0);
		break;
	default:
		/* A record the executor does not know cannot be answered. */
		reply->error = 1;
		break;
	}
}

/*
 * vkGetPhysicalDeviceVideoFormatPropertiesKHR: [physical][present]{format
 * info and its profile list}[present][capacity][array] -> [result][present]
 * [count][array]{format}.
 */
static int
video_format_properties(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_chain chain;
	uint64_t present;
	uint32_t type;
	uint32_t usage;
	uint32_t video_usage;
	int32_t result;
	bool available;

	/* The format info: its type, its chain (the profile list) and the usage, then the capacity. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_FORMAT_INFO)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	usage = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	video_chain_free(&chain);
	if (reader->error != 0)
		return EINVAL;

	/* Every profile H.264 decode; usages the decode's output and reference, the readback and the planes' sampling. */
	result = 0;
	video_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_SAMPLED_BIT;
	available = video_available(session);
	if (!available) {
		result = VIDEO_ERROR_OPERATION;
	} else if (!chain.has_profile_list || chain.profile_count == 0) {
		result = VIDEO_ERROR_OPERATION;
	} else if (!chain.profiles_supported) {
		result = chain.profile_error;
	} else if ((usage & ~video_usage) != 0) {
		result = VIDEO_ERROR_IMAGE_USAGE;
	}

	/* A format the engine does not offer. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/*
	 * The one format: NV12, identity swizzle, its planes viewable in their
	 * own formats (mutable format, extended usage), 2D, optimal (linear
	 * planes inside), decode output, reference, readback and sampled.
	 */
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, VIDEO_TYPE_FORMAT_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0);
	drv_i915_wire_reply_u32(reply, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_TYPE_2D);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_TILING_OPTIMAL);
	drv_i915_wire_reply_u32(reply, video_usage);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * vkCreateVideoSessionKHR: [device][present]{create info}[allocator][present]
 * [identity] -> [result][present][identity].  The engine's stream is
 * opened here, so a session that cannot decode fails at once; its pool
 * comes with the first picture.
 */
static int
video_session_create(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct drv_powervr_video_ops *ops;
	struct drv_powervr_video_open open;
	struct video_session *state;
	struct video_chain chain;
	char name[VK_MAX_EXTENSION_NAME_SIZE];
	void *engine;
	uint64_t present;
	uint64_t identity;
	uint32_t type;
	uint32_t family;
	uint32_t flags;
	uint32_t picture_format;
	uint32_t reference_format;
	uint32_t width;
	uint32_t height;
	uint32_t slots;
	uint32_t references;
	uint32_t version;
	uint32_t profile_idc;
	int32_t result;
	bool available;
	int different;
	int error;

	/* The create info: [type][chain][family][flags][profile][formats and limits][header]. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(name, 0, sizeof(name));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_SESSION_CREATE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	family = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	result = VIDEO_ERROR_OPERATION;
	profile_idc = 0;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		result = video_read_profile(reader, &profile_idc);
	picture_format = drv_i915_wire_read_u32(reader);
	width = drv_i915_wire_read_u32(reader);
	height = drv_i915_wire_read_u32(reader);
	reference_format = drv_i915_wire_read_u32(reader);
	slots = drv_i915_wire_read_u32(reader);
	references = drv_i915_wire_read_u32(reader);
	version = 0;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_extension(reader, name, &version);
	identity = powervr_vk_create_tail(reader);
	video_chain_free(&chain);
	if (reader->error != 0 || identity == 0U)
		return EINVAL;

	/* Checks the session against what the engine takes; the first reason is the result. */
	different = kern_strcmp(name, VIDEO_STD_NAME);
	available = video_available(session);
	if (!available) {
		result = VIDEO_ERROR_OPERATION;
	} else if (result != 0) {
		/* The profile's own reason. */
	} else if (family != 0U) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (flags != 0U) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
	} else if (picture_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || reference_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
		result = VIDEO_ERROR_FORMAT;
	} else if (width < VIDEO_MIN_SIDE ||
		   height < VIDEO_MIN_SIDE ||
		   width > VIDEO_MAX_WIDTH ||
		   height > VIDEO_MAX_HEIGHT) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (slots > VIDEO_SLOTS || references > VIDEO_REFERENCES) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (different != 0 || version > VIDEO_STD_VERSION) {
		result = VIDEO_ERROR_STD_VERSION;
	}

	/* A session the engine cannot run. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* The session with its limits. */
	state = kern_calloc(1, sizeof(*state));
	if (state == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}
	state->owner = session;
	state->profile_idc = profile_idc;
	state->width = width;
	state->height = height;
	state->slots = slots;
	state->references = references;

	/* The engine's stream for the session's largest picture in whole macroblocks, with the pool made on its call. */
	ops = powervr_video_engine(&engine);
	error = ENODEV;
	if (ops != NULL && ops->open != NULL) {
		open.width = (width + VIDEO_MB - 1U) / VIDEO_MB * VIDEO_MB;
		open.height = (height + VIDEO_MB - 1U) / VIDEO_MB * VIDEO_MB;
		open.owner = state;
		open.pool = video_pool_make;
		error = ops->open(engine, DRV_POWERVR_VIDEO_DECODE_H264, &open, &state->stream);
	}
	if (error != 0) {
		kern_free(state);
		kern_logf("powervr: vk: video: the engine opened no stream (%d)\n", error);
		result = VK_ERROR_INITIALIZATION_FAILED;
		if (error == ENOMEM)
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Publishes the session and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_VIDEO_SESSION, identity, state, video_session_destroy, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkGetVideoSessionMemoryRequirementsKHR: [device][session][present][capacity]
 * [array] -> [result][present][count][array]{binding}.  One small binding
 * of either memory type: the engine keeps its own memory.
 */
static int
video_requirements(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct video_session *state;
	uint64_t identity;
	uint32_t capacity;

	/* Skips the device, reads the session and the capacity. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown session, or no room for the binding, is refused. */
	state = powervr_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (state == NULL || capacity < 1U) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	/* The binding: its type, no chain, index 0, its size, its boundary, both memory types. */
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, 1);
	drv_i915_wire_reply_u64(reply, 1);
	drv_i915_wire_reply_u32(reply, VIDEO_TYPE_MEMORY_REQUIREMENTS);
	drv_i915_wire_reply_u64(reply, 0);
	drv_i915_wire_reply_u32(reply, 0);
	drv_i915_wire_reply_u64(reply, VIDEO_BINDING_BYTES);
	drv_i915_wire_reply_u64(reply, VIDEO_BIND_ALIGN);
	drv_i915_wire_reply_u32(reply, (1U << POWERVR_VK_MEMORY_DEVICE) | (1U << POWERVR_VK_MEMORY_HOST));

	/* Succeeded: the binding is reported. */
	return 0;
}

/*
 * vkBindVideoSessionMemoryKHR: [device][session][count][array]{bind} ->
 * [result].  The one binding holds its memory, on its boundary and large
 * enough, once.
 */
static int
video_bind(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_session *state;
	struct powervr_vk_object *memory;
	const struct powervr_vk_memory *storage;
	struct video_chain chain;
	uint64_t identity;
	uint64_t count;
	uint64_t item;
	uint64_t memory_id;
	uint64_t offset;
	uint64_t size;
	uint32_t type;
	uint32_t index;
	int32_t result;
	int error;

	/* Skips the device, reads the session and the binding count. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Every binding: [type][chain][index][memory][offset][size]. */
	state = powervr_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	result = 0;
	if (state == NULL)
		result = VK_ERROR_INITIALIZATION_FAILED;
	for (item = 0; item < count && reader->error == 0; item++) {
		kern_memset(&chain, 0, sizeof(chain));
		type = drv_i915_wire_read_u32(reader);
		if (type != VIDEO_TYPE_BIND_MEMORY)
			reader->error = 1;
		video_read_chain(reader, &chain, 0);
		video_chain_free(&chain);
		index = drv_i915_wire_read_u32(reader);
		memory_id = drv_i915_wire_read_u64(reader);
		offset = drv_i915_wire_read_u64(reader);
		size = drv_i915_wire_read_u64(reader);

		/* The session's binding, not bound yet, on its boundary, of memory large enough. */
		memory = powervr_vk_object_find(session, I915_VK_OBJ_MEMORY, memory_id);
		storage = NULL;
		if (memory != NULL)
			storage = memory->payload;
		if (state == NULL || result != 0) {
			continue;
		} else if (index != 0U || storage == NULL || state->binding != NULL) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		} else if ((offset & (VIDEO_BIND_ALIGN - 1U)) != 0U ||
			   size < VIDEO_BINDING_BYTES ||
			   offset > storage->bytes ||
			   size > storage->bytes - offset) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		}

		/* The binding holds its memory. */
		error = powervr_vk_object_retain(memory);
		if (error != 0) {
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
			continue;
		}
		state->binding = memory;
	}

	/* Refuses a malformed request. */
	if (reader->error != 0)
		return EINVAL;

	/* The result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)result);

	/* Succeeded: the command was decoded; the reply carries its result. */
	return 0;
}

/*
 * vkCreateVideoSessionParametersKHR: [device][present]{create info: chain
 * (H.264 create and add), flags, template, session}[allocator][present]
 * [identity] -> [result][present][identity].
 */
static int
video_parameters_create(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_parameters *parameters;
	const struct video_parameters *source;
	struct powervr_vk_object *owner;
	struct powervr_vk_object *template_object;
	struct video_chain chain;
	uint64_t present;
	uint64_t identity;
	uint64_t template_identity;
	uint64_t session_identity;
	uint32_t type;
	int error;

	/* The create info: [type][chain][flags][template][session], then the identity. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_PARAMETERS_CREATE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	(void)drv_i915_wire_read_u32(reader);
	template_identity = drv_i915_wire_read_u64(reader);
	session_identity = drv_i915_wire_read_u64(reader);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0 || identity == 0U) {
		video_chain_free(&chain);
		return EINVAL;
	}

	/* The session must exist and the H.264 create record be given; a template must be of the same session. */
	error = 0;
	source = NULL;
	owner = powervr_vk_object_find(session, I915_VK_OBJ_VIDEO_SESSION, session_identity);
	template_object = NULL;
	if (template_identity != 0U)
		template_object = powervr_vk_object_find(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, template_identity);
	if (template_object != NULL)
		source = template_object->payload;
	if (owner == NULL) {
		error = EINVAL;
	} else if (!chain.has_h264_create || chain.add_error != 0) {
		error = EINVAL;
	} else if (template_identity != 0U && (source == NULL || source->session != owner)) {
		error = EINVAL;
	}

	/* The object. */
	parameters = NULL;
	if (error == 0) {
		parameters = kern_calloc(1, sizeof(*parameters));
		if (parameters == NULL)
			error = ENOMEM;
	}

	/* It holds the session it belongs to. */
	if (error == 0)
		error = powervr_vk_object_retain(owner);
	if (error == 0) {
		parameters->session = owner;
		parameters->max_sps = chain.max_sps;
		parameters->max_pps = chain.max_pps;
	}

	/* The template's sets, then the new ones. */
	if (error == 0 && source != NULL)
		error = video_parameters_copy(parameters, source);
	if (error == 0)
		error = video_parameters_take(parameters, &chain, true);
	video_chain_free(&chain);

	/* A refused object goes with what it took. */
	if (error != 0 && parameters != NULL) {
		(void)video_parameters_destroy(session, parameters);
		parameters = NULL;
	}

	/* Publishes the object and answers. */
	powervr_vk_create_reply(session, reply, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity, parameters, video_parameters_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkUpdateVideoSessionParametersKHR: [device][parameters][present]{update
 * info: chain (add), sequence count} -> [result].  The update must be the
 * next one; it adds its sets whole or not at all.
 */
static int
video_parameters_update(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_parameters *parameters;
	struct video_chain chain;
	uint64_t identity;
	uint64_t present;
	uint32_t type;
	uint32_t sequence;
	int32_t result;
	int error;

	/* The object and the update info: [type][chain][sequence count]. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_PARAMETERS_UPDATE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	sequence = drv_i915_wire_read_u32(reader);
	if (reader->error != 0) {
		video_chain_free(&chain);
		return EINVAL;
	}

	/* The object must exist and the update be the next one; a malformed set refuses it. */
	result = 0;
	parameters = powervr_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity);
	if (parameters == NULL ||
	    sequence != parameters->update_sequence + 1U ||
	    chain.add_error != 0)
		result = VK_ERROR_INITIALIZATION_FAILED;

	/* The sets; a key already held refuses the whole update. */
	if (result == 0) {
		error = video_parameters_take(parameters, &chain, false);
		if (error == ENOMEM) {
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
		} else if (error != 0) {
			result = VK_ERROR_INITIALIZATION_FAILED;
		}
	}

	/* The update's sequence number when it is taken. */
	video_chain_free(&chain);
	if (result == 0)
		parameters->update_sequence = sequence;

	/* The result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)result);

	/* Succeeded: the command was decoded; the reply carries its result. */
	return 0;
}

/*
 * Destroys a session once nothing holds it: the engine's stream, its
 * binding's memory and its pool.
 */
static int
video_session_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	const struct drv_powervr_video_ops *ops;
	struct video_session *state;
	void *engine;
	int closed;
	int error;

	/* The engine's stream; one whose core may still write the pool keeps it. */
	(void)session;
	state = payload;
	ops = powervr_video_engine(&engine);
	if (state->stream != NULL && ops != NULL && ops->close != NULL) {
		closed = ops->close(state->stream);
		if (closed != 0)
			state->kept = true;
	}

	/* The pool, once the GPU work that may read it has finished. */
	video_pool_free(state);

	/* The binding lets go of its memory. */
	error = 0;
	if (state->binding != NULL)
		error = powervr_vk_object_release(state->binding);

	/* The session. */
	kern_free(state);
	if (error != 0)
		return error;

	/* Succeeded: the session is gone. */
	return 0;
}

/* Destroys a parameters object once nothing holds it: its sets and its session. */
static int
video_parameters_destroy(
	struct powervr_vk_session *session,
	void *payload)
{
	struct video_parameters *parameters;
	int error;

	/* The sets, then the session it held. */
	(void)session;
	parameters = payload;
	video_parameters_free(parameters);
	error = 0;
	if (parameters->session != NULL)
		error = powervr_vk_object_release(parameters->session);

	/* The object. */
	kern_free(parameters);
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
}

/*
 * Makes a session's pool on the engine's call: count NV12 pictures of the
 * coded size laid out as the executor lays an NV12 image out (both planes
 * of one pitch), each a buffer of the render node below 4 GiB mapped for
 * the session around the system level cache; the old pool goes first.
 * Reports the pitch and the pictures' planes' bus addresses.
 */
static int
video_pool_make(
	void *owner,
	uint32_t count,
	uint32_t width,
	uint32_t height,
	uint32_t *pitch,
	struct drv_powervr_video_picture *pictures)
{
	struct powervr_image_plane planes[POWERVR_IMAGE_PLANES_MAX];
	struct powervr_image_layout layout;
	struct powervr_image_shape shape;
	const struct powervr_format *format;
	struct video_session *state;
	struct powervr_session *render_session;
	struct video_pool_picture *picture;
	uint32_t plane_count;
	uint32_t index;
	uint64_t bus;
	int error;

	/* A pool the engine's core can register. */
	state = owner;
	render_session = state->owner->owner;
	if (count == 0 || count > VIDEO_POOL_MAX || state->kept)
		return EINVAL;

	/* The old pool goes once its readers have finished. */
	video_pool_free(state);
	if (state->kept)
		return EBUSY;

	/* The NV12 layout of the coded size. */
	format = powervr_format_find(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
	if (format == NULL)
		return ENOTSUP;
	kern_memset(&shape, 0, sizeof(shape));
	shape.width = width;
	shape.height = height;
	shape.depth = 1;
	shape.levels = 1;
	shape.layers = 1;
	shape.linear = true;
	error = powervr_image_lay_out_planes(&shape, format, planes, &plane_count, &layout);
	if (error != 0)
		return error;
	if (plane_count != 2U || planes[0].layout.row_pitch != planes[1].layout.row_pitch)
		return ENOTSUP;

	/* Each picture: a buffer of the render node, the session's mapping of it (around the cache), its planes. */
	for (index = 0; index < count; index++) {
		picture = &state->pool[index];
		error = powervr_buffer_create(render_session->render, layout.bytes, &picture->buffer);
		if (error != 0)
			break;
		error = powervr_resource_make(render_session, picture->buffer, 1, 0, &picture->resource);
		if (error != 0) {
			powervr_buffer_put(render_session->render, picture->buffer);
			picture->buffer = NULL;
			break;
		}

		/* The core reaches 32 bits of address. */
		bus = picture->buffer->run.paddr;
		if (bus + layout.bytes - 1U > 0xffffffffULL) {
			error = ENOMEM;
			state->count = index + 1U;
			break;
		}
		picture->chroma_offset = planes[1].offset;
		pictures[index].luma = bus + planes[0].offset;
		pictures[index].chroma = bus + planes[1].offset;
		state->count = index + 1U;
	}

	/* A pool that could not be made goes again. */
	if (error != 0) {
		kern_logf("powervr: vk: video: no pool of %u pictures of %ux%u (%d)\n", (unsigned)count, (unsigned)width, (unsigned)height, error);
		video_pool_free(state);
		return ENOMEM;
	}

	/* Succeeded: the pool's pitch and pictures. */
	state->pitch = planes[0].layout.row_pitch;
	*pitch = state->pitch;
	return 0;
}

/*
 * Lets a session's pool go once the session's GPU work, which may still
 * copy from it, has finished; a pool the engine's core may still write,
 * or whose readers did not finish in time, is kept.
 */
static void
video_pool_free(
	struct video_session *state)
{
	struct powervr_session *render_session;
	struct video_pool_picture *picture;
	uint32_t index;
	int error;

	/* No pool. */
	if (state->count == 0)
		return;

	/* The GPU's last work of the session, which may read the pool. */
	render_session = state->owner->owner;
	error = 0;
	if (!state->kept)
		error = powervr_session_point_wait(render_session, render_session->last_kind, render_session->last_value, POWERVR_PROGRESS_MS);
	if (error != 0 || state->kept) {
		kern_logf("powervr: vk: video: a pool of %u pictures is kept (%d)\n", (unsigned)state->count, error);
		state->kept = true;
		return;
	}

	/* Each picture's mapping, which drops its buffer. */
	for (index = 0; index < state->count; index++) {
		picture = &state->pool[index];
		if (picture->resource != NULL)
			powervr_resource_unmake(render_session, picture->resource);
		picture->resource = NULL;
		picture->buffer = NULL;
	}

	/* No pool. */
	state->count = 0;
	state->pitch = 0;
}

/* Reads a chain: [present]{type, its own chain, its fields}, nested. */
static void
video_read_chain(
	struct i915_wire_reader *reader,
	struct video_chain *chain,
	uint32_t depth)
{
	uint64_t present;
	uint32_t type;

	/* An absent link ends the chain. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present == 0U)
		return;

	/* Refuses a chain deeper than any the library writes. */
	if (depth >= VIDEO_MAX_CHAIN) {
		reader->error = 1;
		return;
	}

	/* The linked structure: its type, its own chain, then its fields. */
	type = drv_i915_wire_read_u32(reader);
	video_read_chain(reader, chain, depth + 1U);
	video_read_fields(reader, type, chain, depth);
}

/* Reads the fields of one chained structure into the chain's record. */
static void
video_read_fields(
	struct i915_wire_reader *reader,
	uint32_t type,
	struct video_chain *chain,
	uint32_t depth)
{
	struct video_chain nested;
	uint64_t present;
	uint64_t count;
	uint64_t item;
	uint32_t value;
	int32_t result;

	/* Each chained type the library writes; any other fails the stream. */
	switch (type) {
	case VIDEO_TYPE_H264_PROFILE:
		/* The H.264 profile and the picture layout. */
		chain->has_h264_profile = true;
		chain->profile_idc = drv_i915_wire_read_u32(reader);
		chain->picture_layout = drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_DECODE_USAGE:
		/* Usage hints, which the engine does not use. */
		(void)drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_PROFILE_LIST:
		/* [count][array]{profile}: every profile must be H.264 decode; the first refusal is kept. */
		chain->has_profile_list = true;
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		chain->profile_count = (uint32_t)count;
		chain->profiles_supported = true;
		for (item = 0; item < count && reader->error == 0; item++) {
			result = video_read_profile(reader, &value);
			if (result != 0 && chain->profiles_supported) {
				chain->profiles_supported = false;
				chain->profile_error = result;
			}
		}

		break;
	case VIDEO_TYPE_H264_PARAMETERS_CREATE:
		/* The capacity, then the add record the create carries, if any. */
		chain->has_h264_create = true;
		chain->max_sps = drv_i915_wire_read_u32(reader);
		chain->max_pps = drv_i915_wire_read_u32(reader);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			value = drv_i915_wire_read_u32(reader);
			if (value != VIDEO_TYPE_H264_PARAMETERS_ADD)
				reader->error = 1;
			kern_memset(&nested, 0, sizeof(nested));
			video_read_chain(reader, &nested, depth + 1U);
			video_chain_free(&nested);
			video_read_add(reader, chain);
		}

		break;
	case VIDEO_TYPE_H264_PARAMETERS_ADD:
		/* The sets added. */
		video_read_add(reader, chain);
		break;
	case VIDEO_TYPE_H264_DPB_SLOT:
		/* [present]{flags, FrameNum, reserved, [2] order counts}: the engine keeps its references itself. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			count = drv_i915_wire_read_u64(reader);
			if (count != 2U)
				reader->error = 1;
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
		}

		break;
	case VIDEO_TYPE_H264_PICTURE:
		/*
		 * [present]{picture: flags, ids, reserved x2, frame_num,
		 * idr_pic_id, [2] order counts}[slice count][array]{offset}.
		 */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_picture = true;
			chain->picture.flags = drv_i915_wire_read_u32(reader);
			chain->picture.sps_id = drv_i915_wire_read_u32(reader);
			chain->picture.pps_id = drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			count = drv_i915_wire_read_u64(reader);
			if (count != 2U)
				reader->error = 1;
			chain->picture.poc[0] = (int32_t)drv_i915_wire_read_u32(reader);
			chain->picture.poc[1] = (int32_t)drv_i915_wire_read_u32(reader);
		}

		/* The slices' offsets, which the engine finds itself by their start codes. */
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (count > (uint64_t)(reader->size - reader->offset))
			reader->error = 1;
		for (item = 0; item < count && reader->error == 0; item++)
			(void)drv_i915_wire_read_u32(reader);

		break;
	default:
		/* A structure the library does not write here. */
		reader->error = 1;
		break;
	}
}

/*
 * Reads a profile structure: its type, its chain and its four fields.
 * Returns 0 for a profile the engine takes, or the standard reason another
 * is refused; the H.264 profile it names goes to profile_idc.
 */
static int32_t
video_read_profile(
	struct i915_wire_reader *reader,
	uint32_t *profile_idc)
{
	struct video_chain chain;
	uint32_t type;
	uint32_t operation;
	uint32_t chroma;
	uint32_t luma;
	uint32_t chroma_depth;
	int32_t result;

	/* The type, then the chain that carries the H.264 profile, then the fields. */
	kern_memset(&chain, 0, sizeof(chain));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_PROFILE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	operation = drv_i915_wire_read_u32(reader);
	chroma = drv_i915_wire_read_u32(reader);
	luma = drv_i915_wire_read_u32(reader);
	chroma_depth = drv_i915_wire_read_u32(reader);
	video_chain_free(&chain);

	/* Decides the profile. */
	result = video_profile_result(operation, chroma, luma, chroma_depth, &chain);
	*profile_idc = chain.profile_idc;

	/* Succeeded: the profile's result. */
	return result;
}

/* Decides whether a profile is one the engine takes, or why not, in the standard order. */
static int32_t
video_profile_result(
	uint32_t operation,
	uint32_t chroma,
	uint32_t luma,
	uint32_t chroma_depth,
	const struct video_chain *chain)
{
	/* H.264 decode is the one operation. */
	if (operation != VIDEO_OP_DECODE_H264)
		return VIDEO_ERROR_OPERATION;

	/* 8-bit 4:2:0 is the one format. */
	if (chroma != VIDEO_CHROMA_420 ||
	    luma != VIDEO_DEPTH_8 ||
	    chroma_depth != VIDEO_DEPTH_8)
		return VIDEO_ERROR_FORMAT;

	/* The H.264 record must be there and progressive. */
	if (!chain->has_h264_profile)
		return VIDEO_ERROR_CODEC;
	if (chain->picture_layout != VIDEO_LAYOUT_PROGRESSIVE)
		return VIDEO_ERROR_LAYOUT;

	/* Baseline, Main or High. */
	if (chain->profile_idc != VIDEO_PROFILE_BASELINE &&
	    chain->profile_idc != VIDEO_PROFILE_MAIN &&
	    chain->profile_idc != VIDEO_PROFILE_HIGH)
		return VIDEO_ERROR_CODEC;

	/* Succeeded: the profile is supported. */
	return 0;
}

/* Reads a picture resource structure: [type][chain 0][x][y][width][height][layer][view]. */
static void
video_read_resource(
	struct i915_wire_reader *reader,
	struct video_resource *resource)
{
	struct video_chain chain;
	uint32_t type;

	/* The type and an empty chain. */
	kern_memset(&chain, 0, sizeof(chain));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_PICTURE_RESOURCE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	video_chain_free(&chain);

	/* The coded area, the layer and the view. */
	resource->x = (int32_t)drv_i915_wire_read_u32(reader);
	resource->y = (int32_t)drv_i915_wire_read_u32(reader);
	resource->width = drv_i915_wire_read_u32(reader);
	resource->height = drv_i915_wire_read_u32(reader);
	resource->layer = drv_i915_wire_read_u32(reader);
	resource->view = drv_i915_wire_read_u64(reader);
}

/* Reads a reference slot structure: [type][chain: DPB slot][index][present]{resource}. */
static void
video_read_slot(
	struct i915_wire_reader *reader,
	struct video_slot *slot)
{
	struct video_chain chain;
	uint64_t present;
	uint32_t type;

	/* The type and the chain, which carries the H.264 reference information. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(slot, 0, sizeof(*slot));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_REFERENCE_SLOT)
		reader->error = 1;
	video_read_chain(reader, &chain, 0);
	video_chain_free(&chain);

	/* The index and the picture. */
	slot->index = (int32_t)drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		slot->has_picture = true;
		video_read_resource(reader, &slot->resource);
	}
}

/* Reads an array of slots into the record, up to limit of them (more fails the stream). */
static void
video_read_slots(
	struct i915_wire_reader *reader,
	struct video_record *video,
	uint32_t limit)
{
	uint64_t total;
	uint64_t item;

	/* [count][array]: a count past the bytes left, or past the limit, is malformed. */
	(void)drv_i915_wire_read_u32(reader);
	total = drv_i915_wire_read_u64(reader);
	if (total > (uint64_t)(reader->size - reader->offset) || total > limit)
		reader->error = 1;

	/* Each slot in order. */
	video->slot_count = 0;
	for (item = 0; item < total && reader->error == 0; item++) {
		video_read_slot(reader, &video->slots[video->slot_count]);
		video->slot_count++;
	}
}

/*
 * Reads an H.264 add record's fields: [count][array]{SPS}[count][array]{PPS}
 * (libvulkan's video_encode_sps and video_encode_pps).  Each set is a new
 * allocation kept on the chain; a set the executor cannot keep marks
 * add_error.
 */
static void
video_read_add(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	struct vpu_h264_sps *scratch_sps;
	struct video_pps_node *scratch_pps;
	struct vpu_h264_sps *sps;
	struct video_pps_node *pps;
	uint64_t count;
	uint64_t item;

	/* The sequence parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0; item < count && reader->error == 0; item++) {
		/* A set past the id range, or one that cannot be allocated, is read and dropped. */
		sps = NULL;
		if (chain->new_sps_count < VIDEO_SPS_IDS)
			sps = kern_calloc(1, sizeof(*sps));
		scratch_sps = NULL;
		if (sps == NULL) {
			scratch_sps = kern_calloc(1, sizeof(*scratch_sps));
			chain->add_error = ENOMEM;
			if (scratch_sps == NULL) {
				reader->error = 1;
				break;
			}
			sps = scratch_sps;
		}

		/* The fields; an id outside H.264 marks the add malformed. */
		video_read_sps(reader, sps);
		if (sps->id >= VIDEO_SPS_IDS)
			chain->add_error = EINVAL;

		/* Keeps the set on the chain. */
		if (scratch_sps != NULL) {
			kern_free(scratch_sps);
		} else {
			chain->new_sps[chain->new_sps_count] = sps;
			chain->new_sps_count++;
		}
	}

	/* The picture parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0; item < count && reader->error == 0; item++) {
		/* A set that cannot be allocated is read and dropped. */
		pps = kern_calloc(1, sizeof(*pps));
		scratch_pps = NULL;
		if (pps == NULL) {
			scratch_pps = kern_calloc(1, sizeof(*scratch_pps));
			chain->add_error = ENOMEM;
			if (scratch_pps == NULL) {
				reader->error = 1;
				break;
			}
			pps = scratch_pps;
		}

		/* The fields; ids outside H.264 mark the add malformed. */
		video_read_pps(reader, &pps->pps);
		if (pps->pps.sps_id >= VIDEO_SPS_IDS || pps->pps.pps_id >= VIDEO_PPS_IDS)
			chain->add_error = EINVAL;

		/* Keeps the set on the chain. */
		if (scratch_pps != NULL) {
			kern_free(scratch_pps);
		} else {
			pps->next = chain->new_pps;
			chain->new_pps = pps;
		}
	}
}

/*
 * Reads one SPS in libvulkan's order (video_encode_sps): every field of
 * StdVideoH264SequenceParameterSet, the reserved words skipped, then the
 * order count cycle's offsets, the scaling lists and the VUI (never sent).
 */
static void
video_read_sps(
	struct i915_wire_reader *reader,
	struct vpu_h264_sps *sps)
{
	uint64_t offsets;
	uint64_t present;
	uint32_t index;

	/* The flags, the profile and level, the chroma format, the id and the bit depths. */
	sps->flags = drv_i915_wire_read_u32(reader);
	sps->profile_idc = drv_i915_wire_read_u32(reader);
	sps->level = drv_i915_wire_read_u32(reader);
	sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
	sps->id = drv_i915_wire_read_u32(reader);
	sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
	sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);

	/* The frame numbering and the order count rules; the cycle's length comes with its offsets. */
	sps->log2_max_frame_num_minus4 = drv_i915_wire_read_u32(reader);
	sps->pic_order_cnt_type = drv_i915_wire_read_u32(reader);
	sps->offset_for_non_ref_pic = (int32_t)drv_i915_wire_read_u32(reader);
	sps->offset_for_top_to_bottom_field = (int32_t)drv_i915_wire_read_u32(reader);
	sps->log2_max_pic_order_cnt_lsb_minus4 = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	sps->max_num_ref_frames = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The size in macroblocks and the cropping. */
	sps->pic_width_in_mbs_minus1 = drv_i915_wire_read_u32(reader);
	sps->pic_height_in_map_units_minus1 = drv_i915_wire_read_u32(reader);
	for (index = 0; index < 4U; index++)
		sps->crop[index] = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The order count cycle's offsets, at most 255. */
	offsets = drv_i915_wire_read_u64(reader);
	if (offsets > 255U)
		reader->error = 1;
	sps->offset_count = 0;
	for (index = 0; index < (uint32_t)offsets && reader->error == 0; index++) {
		sps->offsets[index] = (int32_t)drv_i915_wire_read_u32(reader);
		sps->offset_count++;
	}

	/* The scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_scaling = 1;
		video_read_scaling(reader, &sps->scaling);
	}

	/* The VUI, which libvulkan never sends. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		reader->error = 1;
}

/* Reads one PPS in libvulkan's order (video_encode_pps). */
static void
video_read_pps(
	struct i915_wire_reader *reader,
	struct vpu_h264_pps *pps)
{
	uint64_t present;

	/* The flags, the ids, the default references and the prediction mode. */
	pps->flags = drv_i915_wire_read_u32(reader);
	pps->sps_id = drv_i915_wire_read_u32(reader);
	pps->pps_id = drv_i915_wire_read_u32(reader);
	pps->num_ref_idx_l0_default_active_minus1 = drv_i915_wire_read_u32(reader);
	pps->num_ref_idx_l1_default_active_minus1 = drv_i915_wire_read_u32(reader);
	pps->weighted_bipred_idc = drv_i915_wire_read_u32(reader);

	/* The quantizer offsets, each widened with its sign. */
	pps->pic_init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->pic_init_qs_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->second_chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);

	/* The picture's own scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		pps->has_scaling = 1;
		video_read_scaling(reader, &pps->scaling);
	}
}

/* Reads H.264 scaling lists: [present mask][default mask][96]{4x4}[384]{8x8}. */
static void
video_read_scaling(
	struct i915_wire_reader *reader,
	struct vpu_h264_scaling *scaling)
{
	uint64_t count;

	/* The two masks, then the lists, list after list. */
	scaling->present_mask = drv_i915_wire_read_u32(reader);
	scaling->default_mask = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count != sizeof(scaling->list4))
		reader->error = 1;
	i915_vkc_read_bytes(reader, scaling->list4, sizeof(scaling->list4));
	count = drv_i915_wire_read_u64(reader);
	if (count != sizeof(scaling->list8))
		reader->error = 1;
	i915_vkc_read_bytes(reader, scaling->list8, sizeof(scaling->list8));
}

/* Reads an extension name and version: [256][name bytes][version]. */
static void
video_read_extension(
	struct i915_wire_reader *reader,
	char *name,
	uint32_t *version)
{
	uint64_t count;

	/* The name, whose array is always the full extent, then the version. */
	count = drv_i915_wire_read_u64(reader);
	if (count != VK_MAX_EXTENSION_NAME_SIZE)
		reader->error = 1;
	i915_vkc_read_bytes(reader, name, VK_MAX_EXTENSION_NAME_SIZE);
	name[VK_MAX_EXTENSION_NAME_SIZE - 1U] = '\0';
	*version = drv_i915_wire_read_u32(reader);
}

/* Frees the parameter sets a chain read and nobody took. */
static void
video_chain_free(
	struct video_chain *chain)
{
	struct video_pps_node *pps;
	uint32_t index;

	/* The sequence sets. */
	for (index = 0; index < chain->new_sps_count; index++) {
		kern_free(chain->new_sps[index]);
		chain->new_sps[index] = NULL;
	}
	chain->new_sps_count = 0;

	/* The picture sets. */
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;
		kern_free(pps);
	}
}

/*
 * Moves a chain's new sets into a parameters object.  With replace, a set
 * replaces the object's of the same key (a create); without, a key
 * already held refuses the whole add (an update) and nothing is taken.
 * The object's capacity bounds the sets.
 */
static int
video_parameters_take(
	struct video_parameters *parameters,
	struct video_chain *chain,
	bool replace)
{
	struct video_pps_node *pps;
	struct video_pps_node *old;
	struct video_pps_node **link;
	const struct vpu_h264_pps *held;
	uint32_t index;
	uint32_t id;
	uint32_t sps_count;
	uint32_t pps_count;

	/* An update must not repeat a key the object holds. */
	if (!replace) {
		/* Nor a sequence set id it holds. */
		for (index = 0; index < chain->new_sps_count; index++) {
			if (parameters->sps[chain->new_sps[index]->id] != NULL)
				return EINVAL;
		}

		/* Nor a picture set key it holds. */
		for (pps = chain->new_pps; pps != NULL; pps = pps->next) {
			held = video_find_pps(parameters, pps->pps.sps_id, pps->pps.pps_id);
			if (held != NULL)
				return EINVAL;
		}
	}

	/* The sequence sets held, and the new ones of ids not held yet. */
	sps_count = 0;
	for (index = 0; index < VIDEO_SPS_IDS; index++) {
		if (parameters->sps[index] != NULL)
			sps_count++;
	}
	for (index = 0; index < chain->new_sps_count; index++) {
		if (parameters->sps[chain->new_sps[index]->id] == NULL)
			sps_count++;
	}

	/* The picture sets held, and the new ones of new keys. */
	pps_count = 0;
	for (pps = parameters->pps; pps != NULL; pps = pps->next)
		pps_count++;
	for (pps = chain->new_pps; pps != NULL; pps = pps->next) {
		held = video_find_pps(parameters, pps->pps.sps_id, pps->pps.pps_id);
		if (held == NULL)
			pps_count++;
	}

	/* Refuses sets beyond the object's capacity. */
	if (sps_count > parameters->max_sps || pps_count > parameters->max_pps)
		return EINVAL;

	/* The sequence sets, each in its id's place. */
	for (index = 0; index < chain->new_sps_count; index++) {
		id = chain->new_sps[index]->id;
		kern_free(parameters->sps[id]);
		parameters->sps[id] = chain->new_sps[index];
		chain->new_sps[index] = NULL;
	}
	chain->new_sps_count = 0;

	/* The picture sets replace any of the same key. */
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;

		/* Drops the picture sets of the same key the object holds. */
		link = &parameters->pps;
		while (*link != NULL) {
			if ((*link)->pps.sps_id == pps->pps.sps_id && (*link)->pps.pps_id == pps->pps.pps_id) {
				old = *link;
				*link = old->next;
				kern_free(old);
				continue;
			}

			/* Moves on to the next picture set. */
			link = &(*link)->next;
		}

		/* The set joins the object's list. */
		pps->next = parameters->pps;
		parameters->pps = pps;
	}

	/* Succeeded: the sets are the object's. */
	return 0;
}

/* Copies a template's sets into a new parameters object. */
static int
video_parameters_copy(
	struct video_parameters *destination,
	const struct video_parameters *source)
{
	const struct video_pps_node *pps;
	struct video_pps_node *copy;
	uint32_t index;

	/* The sequence sets. */
	for (index = 0; index < VIDEO_SPS_IDS; index++) {
		if (source->sps[index] == NULL)
			continue;
		destination->sps[index] = kern_calloc(1, sizeof(*destination->sps[index]));
		if (destination->sps[index] == NULL)
			return ENOMEM;
		*destination->sps[index] = *source->sps[index];
	}

	/* The picture sets. */
	for (pps = source->pps; pps != NULL; pps = pps->next) {
		copy = kern_calloc(1, sizeof(*copy));
		if (copy == NULL)
			return ENOMEM;
		copy->pps = pps->pps;
		copy->next = destination->pps;
		destination->pps = copy;
	}

	/* Succeeded: the template's sets are copied. */
	return 0;
}

/* Frees a parameters object's sets. */
static void
video_parameters_free(
	struct video_parameters *parameters)
{
	struct video_pps_node *pps;
	uint32_t index;

	/* The sequence sets. */
	for (index = 0; index < VIDEO_SPS_IDS; index++) {
		kern_free(parameters->sps[index]);
		parameters->sps[index] = NULL;
	}

	/* The picture sets. */
	while (parameters->pps != NULL) {
		pps = parameters->pps;
		parameters->pps = pps->next;
		kern_free(pps);
	}
}

/* Finds the picture set of a key in a parameters object. */
static const struct vpu_h264_pps *
video_find_pps(
	const struct video_parameters *parameters,
	uint32_t sps_id,
	uint32_t pps_id)
{
	const struct video_pps_node *pps;

	/* Walks the list for the key. */
	for (pps = parameters->pps; pps != NULL; pps = pps->next) {
		if (pps->pps.sps_id == sps_id && pps->pps.pps_id == pps_id)
			return &pps->pps;
	}

	/* No set of the key. */
	return NULL;
}

/* Finds a published object of a kind and holds it in *holder; EINVAL for none. */
static int
video_hold(
	struct powervr_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	struct powervr_vk_object **holder)
{
	struct powervr_vk_object *object;
	int error;

	/* The object. */
	object = powervr_vk_object_find(session, kind, identity);
	if (object == NULL)
		return EINVAL;

	/* Held while the record lives. */
	error = powervr_vk_object_retain(object);
	if (error != 0)
		return error;

	/* Succeeded: the holder has it. */
	*holder = object;
	return 0;
}

/*
 * Takes the session and parameters of the coding scope the command
 * buffer has open (its last begin with no end after it).  EINVAL outside
 * a scope: the API's rules put these commands inside one.
 */
static int
video_scope(
	const struct powervr_vk_command_buffer *command,
	struct video_record *video)
{
	const struct video_record *begin;
	const struct powervr_vk_record *entry;
	uint32_t index;
	int error;

	/* The last begin or end, from the newest record back. */
	begin = NULL;
	for (index = command->record_count; index > 0; index--) {
		entry = &command->records[index - 1U];
		if (entry->opcode == GPU_OP_CMD_END_VIDEO_CODING)
			break;
		if (entry->opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING) {
			begin = entry->data;
			break;
		}
	}

	/* Outside a scope, or a scope that kept no session. */
	if (begin == NULL || begin->session == NULL)
		return EINVAL;

	/* The scope's session, held by this command too. */
	error = powervr_vk_object_retain(begin->session);
	if (error != 0)
		return error;
	video->session = begin->session;

	/* And its parameters, when the begin names them. */
	if (begin->parameters != NULL) {
		error = powervr_vk_object_retain(begin->parameters);
		if (error != 0)
			return error;
		video->parameters = begin->parameters;
	}

	/* Succeeded: the command has its scope's objects. */
	return 0;
}

/* Lets go of every object a recorded command holds. */
static void
video_release(
	struct video_record *video)
{
	uint32_t index;

	/* The scope's session and parameters. */
	if (video->session != NULL)
		(void)powervr_vk_object_release(video->session);
	if (video->parameters != NULL)
		(void)powervr_vk_object_release(video->parameters);

	/* The bitstream's buffer and the output's view. */
	if (video->buffer != NULL)
		(void)powervr_vk_object_release(video->buffer);
	if (video->view != NULL)
		(void)powervr_vk_object_release(video->view);

	/* The references' views. */
	for (index = 0; index < video->slot_count; index++) {
		if (video->slots[index].view != NULL)
			(void)powervr_vk_object_release(video->slots[index].view);
		video->slots[index].view = NULL;
	}

	/* Holds nothing. */
	video->session = NULL;
	video->parameters = NULL;
	video->buffer = NULL;
	video->view = NULL;
	video->slot_count = 0;
}

/* Runs a begin: the slots it lists with a picture are the scope's active ones; the engine forgets the others. */
static void
video_run_begin(
	const struct video_record *video)
{
	const struct drv_powervr_video_ops *ops;
	struct video_session *state;
	void *engine;
	uint32_t active;
	uint32_t index;
	int32_t slot;

	/* A begin that kept no session binds nothing. */
	if (video->session == NULL)
		return;
	state = video->session->payload;

	/* The slots listed with a picture. */
	active = 0;
	for (index = 0; index < video->slot_count; index++) {
		slot = video->slots[index].index;
		if (slot >= 0 && slot < (int32_t)VIDEO_SLOTS && video->slots[index].has_picture)
			active |= 1U << (uint32_t)slot;
	}
	state->active = active;

	/* The engine lets the others' pictures go. */
	ops = powervr_video_engine(&engine);
	if (ops != NULL && ops->keep != NULL && state->stream != NULL)
		ops->keep(state->stream, active);
}

/* Runs a control: a reset makes the engine forget the stream's pictures. */
static void
video_run_control(
	const struct video_record *video)
{
	const struct drv_powervr_video_ops *ops;
	struct video_session *state;
	void *engine;

	/* Only a reset changes anything, and only with a session. */
	if ((video->flags & VIDEO_CONTROL_RESET) == 0U || video->session == NULL)
		return;
	state = video->session->payload;

	/* The engine's stream starts again at an I picture. */
	ops = powervr_video_engine(&engine);
	if (ops != NULL && ops->reset != NULL && state->stream != NULL)
		ops->reset(state->stream);
}

/*
 * Runs a decode: checks it, has the engine decode the picture into a
 * picture of the pool, and copies that into the output image; the result
 * goes to the open query.  EIO when the engine stopped answering or the
 * copy could not run.
 */
static int
video_run_decode(
	struct powervr_vk_session *session,
	const struct video_record *video)
{
	const struct drv_powervr_video_ops *ops;
	struct drv_powervr_video_decode job;
	struct drv_powervr_video_result result;
	struct video_session *state;
	const struct video_parameters *parameters;
	const struct vpu_h264_sps *sps;
	const struct vpu_h264_pps *pps;
	const struct powervr_vk_image *image;
	const uint8_t *bytes;
	const char *reason;
	void *engine;
	uint32_t index;
	int error;

	/* The session and its parameters. */
	state = video->session->payload;
	sps = NULL;
	pps = NULL;
	image = NULL;
	parameters = NULL;
	if (video->parameters != NULL)
		parameters = video->parameters->payload;

	/* What the engine needs; a picture it cannot take is an error. */
	reason = video->skip;
	if (reason == NULL)
		reason = video_check(video, state, parameters, &sps, &pps);
	if (reason == NULL)
		reason = video_check_picture(video->view, &image);
	bytes = NULL;
	if (reason == NULL) {
		error = video_bitstream(video->buffer->payload, video->offset, &bytes);
		if (error != 0)
			reason = "the bitstream has no memory";
	}
	ops = powervr_video_engine(&engine);
	if (reason == NULL && (ops == NULL || ops->decode == NULL || state->stream == NULL))
		reason = "no engine";
	if (reason != NULL) {
		video_skip(reason);
		powervr_vk_query_report(session, VIDEO_STATUS_ERROR);
		return 0;
	}

	/* The picture as the engine takes it: its bytes, its sets, its flags and order count. */
	kern_memset(&job, 0, sizeof(job));
	job.bitstream = bytes;
	job.length = (size_t)video->range;
	job.sps = sps;
	job.pps = pps;
	if ((video->picture.flags & VIDEO_PICTURE_IDR) != 0)
		job.flags |= DRV_POWERVR_VIDEO_PICTURE_IDR;
	if ((video->picture.flags & VIDEO_PICTURE_INTRA) != 0)
		job.flags |= DRV_POWERVR_VIDEO_PICTURE_INTRA;
	job.order = video->picture.poc[0];
	if (video->picture.poc[1] < job.order)
		job.order = video->picture.poc[1];

	/* The slot it becomes, the slots it references and those its scope keeps. */
	job.setup = -1;
	if (video->has_setup && video->setup.index >= 0)
		job.setup = video->setup.index;
	for (index = 0; index < video->slot_count; index++)
		job.references |= 1U << (uint32_t)video->slots[index].index;
	job.active = state->active;

	/* The engine decodes and waits. */
	error = ops->decode(state->stream, &job, &result);
	if (error != 0) {
		powervr_vk_query_report(session, VIDEO_STATUS_ERROR);
		kern_logf("powervr: vk: video: the engine did not finish a picture (%d)\n", error);
		return EIO;
	}

	/* A picture in error, or one with nothing to copy. */
	if (result.status != DRV_POWERVR_VIDEO_COMPLETE) {
		powervr_vk_query_report(session, VIDEO_STATUS_ERROR);
		return 0;
	}
	if (result.index < 0) {
		powervr_vk_query_report(session, VIDEO_STATUS_COMPLETE);
		return 0;
	}

	/* The pool's picture into the output image. */
	error = video_copy(session, state, (uint32_t)result.index, image, &result);
	if (error != 0) {
		powervr_vk_query_report(session, VIDEO_STATUS_ERROR);
		kern_logf("powervr: vk: video: a picture's copy did not run (%d)\n", error);
		return EIO;
	}

	/* Succeeded: the picture is in its image once the copy has run. */
	powervr_vk_query_report(session, VIDEO_STATUS_COMPLETE);
	return 0;
}

/*
 * Checks a decode against what the engine takes, and finds its sets.
 * Returns NULL when it can run, else why the picture is an error.
 */
static const char *
video_check(
	const struct video_record *video,
	const struct video_session *state,
	const struct video_parameters *parameters,
	const struct vpu_h264_sps **sps_out,
	const struct vpu_h264_pps **pps_out)
{
	const struct vpu_h264_sps *sps;
	const struct vpu_h264_pps *pps;
	const struct powervr_vk_buffer *buffer;
	uint32_t index;
	uint32_t width;
	uint32_t height;

	/* The picture's information and its sets. */
	if (!video->has_picture)
		return "no H.264 picture information";
	if (parameters == NULL || video->picture.sps_id >= VIDEO_SPS_IDS)
		return "no parameter sets";
	sps = parameters->sps[video->picture.sps_id];
	pps = video_find_pps(parameters, video->picture.sps_id, video->picture.pps_id);
	if (sps == NULL || pps == NULL)
		return "a parameter set the session does not hold";

	/* A progressive 8-bit 4:2:0 frame. */
	if (sps->chroma_format_idc != 1U ||
	    sps->bit_depth_luma_minus8 != 0U ||
	    sps->bit_depth_chroma_minus8 != 0U)
		return "not 8-bit 4:2:0";
	if ((sps->flags & VPU_H264_SPS_FRAME_MBS_ONLY) == 0U)
		return "a sequence of field pictures";
	if ((video->picture.flags & (VIDEO_PICTURE_FIELD | VIDEO_PICTURE_PAIR)) != 0U)
		return "a field picture";

	/* Within the session's largest picture. */
	width = (sps->pic_width_in_mbs_minus1 + 1U) * VIDEO_MB;
	height = (sps->pic_height_in_map_units_minus1 + 1U) * VIDEO_MB;
	if (width > state->width + VIDEO_MB - 1U || height > state->height + VIDEO_MB - 1U)
		return "a picture larger than the session's";

	/* The bitstream range inside its buffer. */
	buffer = video->buffer->payload;
	if (video->range == 0 || video->range > 0x7fffffffU)
		return "a bitstream range the engine cannot take";
	if (video->offset > buffer->size || video->range > buffer->size - video->offset)
		return "a bitstream past its buffer";

	/* The slots inside the session's. */
	for (index = 0; index < video->slot_count; index++) {
		if (video->slots[index].index < 0 || video->slots[index].index >= (int32_t)VIDEO_SLOTS)
			return "a reference outside the slots";
	}
	if (video->has_setup && video->setup.index >= (int32_t)VIDEO_SLOTS)
		return "a setup slot outside the slots";

	/* Succeeded: the decode can run with these sets. */
	*sps_out = sps;
	*pps_out = pps;
	return NULL;
}

/* Checks that a view is of a bound NV12 image a decode writes; NULL when it is, else why not. */
static const char *
video_check_picture(
	const struct powervr_vk_object *view_object,
	const struct powervr_vk_image **image_out)
{
	const struct powervr_vk_image_view *view;
	const struct powervr_vk_image *image;

	/* The view's image: two planes, decoded into, bound. */
	view = view_object->payload;
	image = view->image->payload;
	if (image->plane_count != 2U)
		return "an output that is not NV12";
	if ((image->usage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) == 0)
		return "an output not made to be decoded into";
	if (image->memory == NULL)
		return "an output without memory";

	/* Succeeded: the image holds pictures. */
	*image_out = image;
	return NULL;
}

/* Reports where the processor reads a bound buffer's bytes from an offset on. */
static int
video_bitstream(
	const struct powervr_vk_buffer *buffer,
	uint64_t offset,
	const uint8_t **bytes)
{
	const struct powervr_vk_memory *memory;
	const struct powervr_resource *storage;

	/* A buffer bound to memory with a buffer behind it. */
	if (buffer->memory == NULL)
		return EIO;
	memory = buffer->memory->payload;
	storage = memory->storage;
	if (storage == NULL || storage->buffer == NULL || storage->buffer->view == NULL)
		return EIO;

	/* The buffer's bytes at its offset in the memory, then the decode's offset. */
	*bytes = (const uint8_t *)storage->buffer->view + buffer->offset + offset;

	/* Succeeded: the bytes' kernel view. */
	return 0;
}

/*
 * Copies a pool picture into an output image: the luma rows, then the
 * chroma rows, of the coded size or the image's, whichever is smaller, as
 * 32-bit words (both are linear rows), in pieces of at most
 * VIDEO_COPY_ROWS rows.
 */
static int
video_copy(
	struct powervr_vk_session *session,
	const struct video_session *state,
	uint32_t index,
	const struct powervr_vk_image *image,
	const struct drv_powervr_video_result *result)
{
	const struct video_pool_picture *picture;
	uint64_t source;
	uint64_t destination;
	uint64_t image_address;
	uint32_t row_bytes;
	uint32_t rows;
	uint32_t done;
	uint32_t piece;
	uint32_t plane;
	int error;

	/* The pool's picture and the image's memory. */
	if (index >= state->count)
		return EINVAL;
	picture = &state->pool[index];
	error = powervr_vk_image_address(image, &image_address);
	if (error != 0)
		return error;

	/* Each plane: its rows of the coded width, no more than the image's or the pitches hold. */
	for (plane = 0; plane < 2U; plane++) {
		row_bytes = result->width;
		if (row_bytes > image->planes[plane].layout.row_pitch)
			row_bytes = image->planes[plane].layout.row_pitch;
		if (row_bytes > state->pitch)
			row_bytes = state->pitch;
		row_bytes &= ~3U;
		rows = result->height;
		if (plane == 1U)
			rows = (rows + 1U) / 2U;
		if (rows > image->planes[plane].height)
			rows = image->planes[plane].height;

		/* Where the plane starts in the pool's picture and in the image. */
		source = picture->resource->address;
		if (plane == 1U)
			source += picture->chroma_offset;
		destination = image_address + image->planes[plane].offset;

		/* The rows, a piece at a time. */
		for (done = 0; done < rows; done += piece) {
			piece = rows - done;
			if (piece > VIDEO_COPY_ROWS)
				piece = VIDEO_COPY_ROWS;
			error = powervr_vk_transfer_rows(session,
							 source + (uint64_t)done * state->pitch,
							 state->pitch,
							 destination + (uint64_t)done * image->planes[plane].layout.row_pitch,
							 image->planes[plane].layout.row_pitch,
							 row_bytes,
							 piece);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the copies run after the decode, the session's last work. */
	return 0;
}

/* Logs a picture in error's reason, the first few times. */
static void
video_skip(
	const char *reason)
{
	/* Only the first ones. */
	if (video_skip_lines >= VIDEO_SKIP_LINES)
		return;
	video_skip_lines++;

	/* The reason. */
	kern_logf("powervr: vk: video: a picture is an error: %s\n", reason);
}
