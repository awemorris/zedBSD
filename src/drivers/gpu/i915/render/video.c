/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video decode part of the Vulkan executor (see video.h), ws083-p003b.
 *
 * The wire is libvulkan's (userland/desktop/libvulkan/video.c): a structure
 * is its sType, its chain, then its fields; a chain link is a 64-bit
 * presence word followed by the next structure, nested; a pointer is a
 * presence word and what it points to; an array is a 64-bit count and its
 * elements; the H.264 flags are one word packed by name.
 *
 * What the application gives is checked in two ways.  A use that breaks
 * the API's rules (a session not bound, a slot not active, a command on the
 * wrong queue family) refuses the whole submission with
 * VK_ERROR_DEVICE_LOST, decided for the whole submission before anything
 * runs.  A bitstream value the decoder cannot take (a parameter set out of
 * range, a slice past the buffer) only skips the picture, with a log line:
 * an application may hand an untrusted stream to the decoder, and the
 * decode of a bad stream is undefined.  Skipped pictures still move the
 * DPB slots, so the next good picture finds its references.
 *
 * A decode that passes every check is resolved -- its sets, its picture,
 * the GPU addresses of its output, references, bitstream and session
 * memory -- and the MFX commands of the picture (video-mfx.c) are written
 * into the session's own batch, which runs to its end on VCS0 before the
 * next operation.
 */

#include "video.h"
#include "video-mfx.h"
#include "batch.h"
#include "codec.h"
#include "draw.h"
#include "fence.h"
#include "gfx.h"
#include "heap.h"
#include "object.h"

#include "../memory.h"
#include "../session.h"
#include "../worker.h"
#include "../i915.h"

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../intel/genxml.h"

/* The H.264 decode limits the executor reports and holds sessions to (design §3.3). */
#define I915_VIDEO_MAX_DPB_SLOTS	17U
#define I915_VIDEO_MAX_REFERENCES	16U
#define I915_VIDEO_MAX_EXTENT		4096U
#define I915_VIDEO_MB			16U

/* The most slices of a picture the decoder takes (N4), and of slots one begin binds. */
#define I915_VIDEO_MAX_SLICES		256U
#define I915_VIDEO_MAX_BEGIN_SLOTS	32U

/* The most video sessions one submission names; a submission naming more is refused. */
#define I915_VIDEO_SUBMIT_SESSIONS	4U

/* The H.264 parameter set id ranges (seq_parameter_set_id 0..31, pic_parameter_set_id 0..255). */
#define I915_VIDEO_SPS_IDS		32U
#define I915_VIDEO_PPS_IDS		256U

/*
 * The bindings of a session: four row stores, one motion vector buffer for
 * each DPB slot, and one for a picture without a setup slot.
 */
#define I915_VIDEO_ROW_STORES		4U
#define I915_VIDEO_MAX_BINDS		(I915_VIDEO_ROW_STORES + I915_VIDEO_MAX_DPB_SLOTS + 1U)

/* The alignment of every binding and decode surface, and the one memory type. */
#define I915_VIDEO_ALIGN		4096U
#define I915_VIDEO_MEMORY_TYPES		1U

/*
 * The bytes of a session's batch: one decode at a time, at most
 * I915_VIDEO_MFX_MAX_DWORDS and the batch's end, with room to spare.
 */
#define I915_VIDEO_BATCH_BYTES		32768U

/* The deepest chain a structure may carry. */
#define I915_VIDEO_MAX_CHAIN		8U

/* The H.264 header the decoder was written against: VK_STD_vulkan_video_codec_h264_decode 1.0.0. */
#define I915_VIDEO_STD_NAME		"VK_STD_vulkan_video_codec_h264_decode"
#define I915_VIDEO_STD_VERSION		((1U << 22) | (0U << 12) | 0U)

/* The H.264 profiles the decoder takes: Baseline, Main and High. */
#define I915_VIDEO_PROFILE_BASELINE	66U
#define I915_VIDEO_PROFILE_MAIN		77U
#define I915_VIDEO_PROFILE_HIGH		100U

/* The level the decoder reports: STD_VIDEO_H264_LEVEL_IDC_5_1. */
#define I915_VIDEO_LEVEL_5_1		14U

/* The structure types the video wire carries (Vulkan 1.3 registry values). */
#define I915_VIDEO_TYPE_PROFILE			1000023000U
#define I915_VIDEO_TYPE_CAPABILITIES		1000023001U
#define I915_VIDEO_TYPE_PICTURE_RESOURCE	1000023002U
#define I915_VIDEO_TYPE_MEMORY_REQUIREMENTS	1000023003U
#define I915_VIDEO_TYPE_BIND_MEMORY		1000023004U
#define I915_VIDEO_TYPE_SESSION_CREATE		1000023005U
#define I915_VIDEO_TYPE_PARAMETERS_CREATE	1000023006U
#define I915_VIDEO_TYPE_PARAMETERS_UPDATE	1000023007U
#define I915_VIDEO_TYPE_BEGIN			1000023008U
#define I915_VIDEO_TYPE_END			1000023009U
#define I915_VIDEO_TYPE_CONTROL			1000023010U
#define I915_VIDEO_TYPE_REFERENCE_SLOT		1000023011U
#define I915_VIDEO_TYPE_PROFILE_LIST		1000023013U
#define I915_VIDEO_TYPE_FORMAT_INFO		1000023014U
#define I915_VIDEO_TYPE_FORMAT_PROPERTIES	1000023015U
#define I915_VIDEO_TYPE_DECODE			1000024000U
#define I915_VIDEO_TYPE_DECODE_CAPABILITIES	1000024001U
#define I915_VIDEO_TYPE_DECODE_USAGE		1000024002U
#define I915_VIDEO_TYPE_H264_CAPABILITIES	1000040000U
#define I915_VIDEO_TYPE_H264_PICTURE		1000040001U
#define I915_VIDEO_TYPE_H264_PROFILE		1000040003U
#define I915_VIDEO_TYPE_H264_PARAMETERS_CREATE	1000040004U
#define I915_VIDEO_TYPE_H264_PARAMETERS_ADD	1000040005U
#define I915_VIDEO_TYPE_H264_DPB_SLOT		1000040006U

/* The flag and enumerant values of the video API the executor answers with or checks. */
#define I915_VIDEO_OP_DECODE_H264		0x1U
#define I915_VIDEO_CHROMA_420			0x2U
#define I915_VIDEO_DEPTH_8			0x1U
#define I915_VIDEO_LAYOUT_PROGRESSIVE		0U
#define I915_VIDEO_SEPARATE_REFERENCE_IMAGES	0x2U
#define I915_VIDEO_DPB_AND_OUTPUT_COINCIDE	0x1U
#define I915_VIDEO_USAGE_DECODE_DST		0x400U
#define I915_VIDEO_USAGE_DECODE_DPB		0x1000U
#define I915_VIDEO_CONTROL_RESET		0x1U

/* VkQueryResultStatusKHR: a decode that ran, and one that was skipped (ws083-p008). */
#define I915_VIDEO_STATUS_COMPLETE		1
#define I915_VIDEO_STATUS_ERROR			(-1)
#define I915_VIDEO_FORMAT_NV12			1000156003U

/* The video results the executor replies (the pinned header's values). */
#define I915_VIDEO_ERROR_IMAGE_USAGE		(-1000023000)
#define I915_VIDEO_ERROR_LAYOUT			(-1000023001)
#define I915_VIDEO_ERROR_OPERATION		(-1000023002)
#define I915_VIDEO_ERROR_FORMAT			(-1000023003)
#define I915_VIDEO_ERROR_CODEC			(-1000023004)
#define I915_VIDEO_ERROR_STD_VERSION		(-1000023005)

/*
 * One VkVideoSessionParametersKHR: the H.264 parameter sets of one session.
 *
 * It is published from its creation to its destruction or the session's
 * close, which free every set it holds.
 */
struct i915_video_parameters {
	/* The video session the object was created for. */
	uint64_t session;

	/* How many sets it may hold, and the sequence number of its last update. */
	uint32_t max_sps;
	uint32_t max_pps;
	uint32_t update_sequence;

	/* The sets, by sequence set id and by picture set id. */
	struct i915_video_sps *sps[I915_VIDEO_SPS_IDS];
	struct i915_video_pps *pps[I915_VIDEO_PPS_IDS];
};

/* One DPB slot of a session: whether it holds a reference picture, and which view and layer. */
struct i915_video_slot {
	int active;
	uint64_t view;
	uint32_t layer;
};

/* One binding of a session's memory, by the memory's identity (looked up again at each submit). */
struct i915_video_bind {
	uint64_t memory;
	uint64_t offset;
	uint64_t size;
};

/*
 * One VkVideoSessionKHR.
 *
 * It is published from its creation to its destruction or its session's
 * close.  Its DPB slots move only when a submission runs (their simulation
 * runs on a copy).  The batch is made on the first decode that writes MFX
 * commands; the batch of a session a video engine hang quarantined is
 * retained with the session's quarantine, not freed (design §6.1).  The slice bounds are the scratch of the decode
 * being written (submissions of a session run one at a time).
 */
struct i915_video_session {
	/* The H.264 profile, the largest picture in macroblocks, and the slot and reference limits. */
	uint32_t profile_idc;
	uint32_t width_mbs;
	uint32_t height_mbs;
	uint32_t max_dpb_slots;
	uint32_t max_references;

	/* How many bindings the session has, what is bound to each and which are bound. */
	uint32_t bind_count;
	struct i915_video_bind binds[I915_VIDEO_MAX_BINDS];
	uint32_t bound_mask;

	/* The DPB slots and whether a reset ran since the creation. */
	struct i915_video_slot slots[I915_VIDEO_MAX_DPB_SLOTS];
	int reset_done;

	/* The batch the session's decodes are written into, NULL until the first one, and its cursor. */
	struct i915_gem_object *batch;
	struct i915_gfx_batch cursor;

	/* Where each slice of the decode being written begins (after its start code) and ends, from the range's start. */
	uint32_t slice_starts[I915_VIDEO_MAX_SLICES];
	uint32_t slice_ends[I915_VIDEO_MAX_SLICES];
};

/* The picture resource of a decode or a slot, as recorded. */
struct i915_video_resource {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t layer;
	uint64_t view;
};

/* One reference slot of a begin or a decode, as recorded. */
struct i915_video_slot_info {
	int32_t index;
	int has_picture;
	struct i915_video_resource picture;
	int has_reference;
	uint32_t reference_flags;
	uint32_t frame_num;
	int32_t poc[2];
};

/*
 * One recorded video command: a begin, a control, a decode or an end.
 *
 * It is made when the command is recorded and freed with the operation
 * that holds it (a command buffer begin, reset or free).  `oversize` marks
 * a command that carried more slots or references than an operation holds,
 * which refuses its submission.
 */
struct i915_video_command {
	uint32_t opcode;
	int oversize;

	/* A begin: the session, the parameters and the bound slots; a control or end: the flags. */
	uint64_t session;
	uint64_t parameters;
	uint32_t flags;
	uint32_t slot_count;
	struct i915_video_slot_info slots[I915_VIDEO_MAX_BEGIN_SLOTS];

	/* A decode: the bitstream range, the output, the setup slot and the references. */
	uint64_t buffer;
	uint64_t offset;
	uint64_t range;
	struct i915_video_resource destination;
	int has_setup;
	struct i915_video_slot_info setup;
	uint32_t reference_count;
	struct i915_video_slot_info references[I915_VIDEO_MAX_REFERENCES];

	/* A decode's H.264 picture: its flags, parameter set ids, numbers and slice offsets. */
	int has_picture;
	uint32_t picture_flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t frame_num;
	uint32_t idr_pic_id;
	int32_t poc[2];
	uint32_t slice_count;
	uint32_t slices[I915_VIDEO_MAX_SLICES];
};

/*
 * What the chained records of one structure carried, for the chain reader.
 *
 * It lives on the stack of one command's decoding.  The H.264 add record's
 * sets are the parameters object's to take: a create or update links them
 * into the object or frees them.
 */
struct i915_video_chain {
	/* An H.264 profile record. */
	int has_h264_profile;
	uint32_t profile_idc;
	uint32_t picture_layout;

	/* A profile list: how many profiles, and whether every one of them is H.264 decode. */
	int has_profile_list;
	uint32_t profile_count;
	int profiles_supported;
	int profile_error;

	/* An H.264 parameters create record. */
	int has_h264_create;
	uint32_t max_sps;
	uint32_t max_pps;

	/* An H.264 add record: its sets, newest first, not yet linked anywhere. */
	int has_add;
	struct i915_video_sps *new_sps[I915_VIDEO_SPS_IDS];
	uint32_t new_sps_count;
	struct i915_video_pps *new_pps;
	uint32_t new_pps_count;
	int add_error;

	/* A DPB slot record: the reference information. */
	int has_reference;
	uint32_t reference_flags;
	uint32_t frame_num;
	int32_t poc[2];

	/* An H.264 picture record. */
	int has_picture;
	uint32_t picture_flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t picture_frame_num;
	uint32_t idr_pic_id;
	int32_t picture_poc[2];
	uint32_t slice_count;
	uint32_t slices[I915_VIDEO_MAX_SLICES];
};

/* The state of one session while a submission is simulated: a copy of its slots and reset. */
struct i915_video_simulation {
	uint64_t identity;
	struct i915_video_session *session;
	struct i915_video_slot slots[I915_VIDEO_MAX_DPB_SLOTS];
	int reset_done;
};

static int i915_video_capabilities(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_format_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_family_properties(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_session_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_session_destroy(struct i915_render_session *session, struct i915_wire_reader *reader);
static int i915_video_requirements(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_bind(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_parameters_create(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_parameters_update(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int i915_video_parameters_destroy(struct i915_render_session *session, struct i915_wire_reader *reader);
static void i915_video_read_chain(struct i915_wire_reader *reader, struct i915_video_chain *chain, uint32_t depth);
static void i915_video_read_fields(struct i915_wire_reader *reader, uint32_t type, struct i915_video_chain *chain, uint32_t depth);
static int32_t i915_video_read_profile(struct i915_wire_reader *reader, uint32_t *profile_idc);
static void i915_video_read_resource(struct i915_wire_reader *reader, struct i915_video_resource *resource);
static void i915_video_read_slot(struct i915_wire_reader *reader, struct i915_video_slot_info *slot);
static void i915_video_read_add(struct i915_wire_reader *reader, struct i915_video_chain *chain);
static void i915_video_read_scaling(struct i915_wire_reader *reader, struct i915_video_scaling *scaling);
static void i915_video_read_extension(struct i915_wire_reader *reader, char *name, uint32_t *version);
static void i915_video_chain_free(struct i915_video_chain *chain);
static int32_t i915_video_profile_result(uint32_t operation, uint32_t chroma, uint32_t luma, uint32_t chroma_depth, const struct i915_video_chain *chain);
static void i915_video_reply_capabilities(struct i915_wire_writer *reply, const uint32_t *types, uint32_t count);
static void i915_video_session_sizes(const struct i915_video_session *video, uint32_t index, uint64_t *size);
static void i915_video_parameters_free(struct i915_video_parameters *parameters);
static int i915_video_parameters_copy(struct i915_video_parameters *destination, const struct i915_video_parameters *source);
static int i915_video_parameters_take(struct i915_video_parameters *parameters, struct i915_video_chain *chain, int replace);
static void i915_video_session_free(struct i915_render_session *session, struct i915_video_session *video);
static void i915_video_record_slots(struct i915_wire_reader *reader, struct i915_video_command *command, struct i915_video_slot_info *slots, uint32_t limit, uint32_t *count);
static struct i915_video_simulation *i915_video_simulated(struct i915_video_simulation *table, uint32_t *count, struct i915_render_session *session, uint64_t identity);
static int i915_video_simulate(struct i915_render_session *session, const struct i915_gfx_op *const *lists, const uint32_t *counts, uint32_t list_count, int apply);
static int i915_video_simulate_begin(struct i915_render_session *session, const struct i915_video_command *command, struct i915_video_simulation *state);
static int i915_video_simulate_decode(const struct i915_video_command *command, const struct i915_video_command *begin, struct i915_video_simulation *state);
static int i915_video_bound(struct i915_render_session *session, const struct i915_video_session *video);
static const struct i915_video_slot_info *i915_video_begin_slot(const struct i915_video_command *begin, int32_t index);
static int i915_video_begin_picture(const struct i915_video_command *begin, const struct i915_video_resource *picture);
static int i915_video_same_picture(const struct i915_video_resource *first, const struct i915_video_resource *second);
static const char *i915_video_check(struct i915_render_session *session, const struct i915_video_command *command, const struct i915_video_command *begin, struct i915_video_session *video, struct i915_video_mfx_decode *decode);
static const char *i915_video_check_slices(struct i915_render_session *session, const struct i915_video_command *command, struct i915_video_session *video, struct i915_video_mfx_decode *decode);
static const char *i915_video_check_picture(struct i915_render_session *session, const struct i915_video_resource *picture, const struct i915_video_sps *sps, const struct i915_gfx_image *reference);
static const char *i915_video_check_binds(struct i915_render_session *session, const struct i915_video_session *video);
static const struct i915_gfx_image *i915_video_view_image(struct i915_render_session *session, uint64_t view);
static const struct i915_video_pps *i915_video_find_pps(const struct i915_video_parameters *parameters, uint32_t sps_id, uint32_t pps_id);
static void i915_video_skip(const char *reason);
static uint32_t i915_video_mbs(uint32_t pixels);
static int i915_video_run(struct i915_render_session *session, struct i915_video_session *video);
static void i915_video_resolve(struct i915_render_session *session, const struct i915_video_command *command, const struct i915_video_command *begin, const struct i915_video_session *video, struct i915_video_mfx_decode *decode);
static uint64_t i915_video_bind_address(struct i915_render_session *session, const struct i915_video_session *video, uint32_t index);
static uint64_t i915_video_picture_address(struct i915_render_session *session, uint64_t view);
static int i915_video_write(struct i915_render_session *session, struct i915_video_session *video, const struct i915_video_mfx_decode *decode);

/*
 * Runs one of the video commands the dispatcher routes here: the physical
 * device queries, the sessions and the parameters objects.
 *
 * A device that does not offer video decode refuses all of them, which
 * fails the stream (libvulkan never sends them there).
 */
int
drv_i915_video_dispatch(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	int error;

	/* Refuses video commands on a device without video decode. */
	if (!session->vk->video) {
		reader->error = 1;
		return ENOTSUP;
	}

	/* Runs the command the opcode names. */
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES:
		error = i915_video_capabilities(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES:
		error = i915_video_format_properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES:
		error = i915_video_family_properties(session, reader, reply);
		break;
	case GPU_OP_CREATE_VIDEO_SESSION:
		error = i915_video_session_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_VIDEO_SESSION:
		error = i915_video_session_destroy(session, reader);
		break;
	case GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS:
		error = i915_video_requirements(session, reader, reply);
		break;
	case GPU_OP_BIND_VIDEO_SESSION_MEMORY:
		error = i915_video_bind(session, reader, reply);
		break;
	case GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS:
		error = i915_video_parameters_create(session, reader, reply);
		break;
	case GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS:
		error = i915_video_parameters_update(session, reader, reply);
		break;
	case GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS:
		error = i915_video_parameters_destroy(session, reader);
		break;
	default:
		/* A recording opcode is the command buffer part's; anything else is unknown. */
		reader->error = 1;
		error = ENOTSUP;
		break;
	}

	/* Reports why the command was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and replied. */
	return 0;
}

/*
 * Decodes one recorded video command into a new record.
 *
 * The reader stands after the command buffer's identity.  The record is
 * the caller's to keep in an operation or to free.  Returns ENOMEM when the
 * record cannot be made (the command is still decoded to its end), EINVAL
 * for a malformed command, and ENOTSUP on a device without video decode.
 */
int
drv_i915_video_record(
	struct i915_render_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_video_command **result)
{
	struct i915_video_command scratch;
	struct i915_video_command *command;
	struct i915_video_chain chain;
	uint64_t present;
	uint32_t type;
	uint32_t index;

	/* Nothing is handed out on failure. */
	*result = NULL;

	/* Refuses video commands on a device without video decode. */
	if (!session->vk->video) {
		reader->error = 1;
		return ENOTSUP;
	}

	/* Makes the record; without one the command is decoded into scratch and dropped. */
	command = kern_calloc(1U, sizeof(*command));
	if (command == NULL) {
		kern_memset(&scratch, 0, sizeof(scratch));
		command = &scratch;
	}
	command->opcode = opcode;

	/* Reads the structure's presence, its type and its chain. */
	kern_memset(&chain, 0, sizeof(chain));
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);

	/* Reads the fields of the command's structure. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
		/* [flags][session][parameters][count][array]{slot}. */
		if (type != I915_VIDEO_TYPE_BEGIN)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		command->session = drv_i915_wire_read_u64(reader);
		command->parameters = drv_i915_wire_read_u64(reader);
		i915_video_record_slots(reader, command, command->slots, I915_VIDEO_MAX_BEGIN_SLOTS, &command->slot_count);
		break;
	case GPU_OP_CMD_END_VIDEO_CODING:
		/* [flags]. */
		if (type != I915_VIDEO_TYPE_END)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
		/* [flags]. */
		if (type != I915_VIDEO_TYPE_CONTROL)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		break;
	default:
		/* vkCmdDecodeVideoKHR: [flags][buffer][offset][range][resource][present]{setup}[count][array]{slot}. */
		if (type != I915_VIDEO_TYPE_DECODE)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		command->buffer = drv_i915_wire_read_u64(reader);
		command->offset = drv_i915_wire_read_u64(reader);
		command->range = drv_i915_wire_read_u64(reader);
		i915_video_read_resource(reader, &command->destination);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			command->has_setup = 1;
			i915_video_read_slot(reader, &command->setup);
		}
		i915_video_record_slots(reader, command, command->references, I915_VIDEO_MAX_REFERENCES, &command->reference_count);

		/* Keeps the H.264 picture the chain carried. */
		command->has_picture = chain.has_picture;
		command->picture_flags = chain.picture_flags;
		command->sps_id = chain.sps_id;
		command->pps_id = chain.pps_id;
		command->frame_num = chain.picture_frame_num;
		command->idr_pic_id = chain.idr_pic_id;
		command->poc[0] = chain.picture_poc[0];
		command->poc[1] = chain.picture_poc[1];
		command->slice_count = chain.slice_count;
		for (index = 0U; index < chain.slice_count && index < I915_VIDEO_MAX_SLICES; index++)
			command->slices[index] = chain.slices[index];
		break;
	}

	/* A chain that carried parameter sets here is not this command's. */
	i915_video_chain_free(&chain);

	/* Refuses a malformed command. */
	if (reader->error != 0) {
		if (command != &scratch)
			kern_free(command);
		return EINVAL;
	}

	/* Without a record the command was decoded but cannot be kept. */
	if (command == &scratch)
		return ENOMEM;

	/* Succeeded: the caller holds the record. */
	*result = command;
	return 0;
}

/*
 * Frees one recorded video command.
 */
void
drv_i915_video_command_free(
	struct i915_video_command *command)
{
	/* The record is one allocation. */
	kern_free(command);
}

/*
 * Runs the command buffers of a submission on the video decode queue family.
 *
 * Every operation of every command buffer is checked first, and the slot
 * transitions simulated on a copy of the sessions' state; a use against
 * the API's rules refuses the submission before anything runs.  Then the
 * operations run in order: the slots move, and each decode is checked
 * against what the decoder takes and skipped when it does not.  Returns
 * the VkResult of the submission.
 */
uint32_t
drv_i915_video_submit(
	struct i915_render_session *session,
	const struct i915_gfx_op *const *lists,
	const uint32_t *counts,
	uint32_t list_count)
{
	int error;
	int state;

	/* A video engine that hung takes nothing more: the device is lost for video. */
	state = drv_i915_worker_video_state(session->vk->i915);
	if (state != 0)
		return (uint32_t)VK_ERROR_DEVICE_LOST;

	/*
	 * A session quarantined by a hang of its own is lost: it decodes
	 * nothing more, so one program cannot spend the device's engine resets
	 * (ws083 R-S3).
	 */
	if (session->gpu->quarantined != 0U) {
		kern_logf("i915: video: submission refused: the session is quarantined after a hang\n");
		return (uint32_t)VK_ERROR_DEVICE_LOST;
	}

	/* Checks and simulates the whole submission before anything runs. */
	error = i915_video_simulate(session, lists, counts, list_count, 0);
	if (error != 0)
		return (uint32_t)VK_ERROR_DEVICE_LOST;

	/* Runs it: the slots move, and every decode is checked and skipped or written and run. */
	error = i915_video_simulate(session, lists, counts, list_count, 1);
	if (error == ENOMEM)
		return (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY;
	if (error != 0)
		return (uint32_t)VK_ERROR_DEVICE_LOST;

	/* Succeeded: every operation ran. */
	return 0U;
}

/*
 * Frees every video session and parameters object a closing session left.
 */
void
drv_i915_video_objects_release(
	struct i915_render_session *session)
{
	struct i915_video_session *video;
	struct i915_video_parameters *parameters;

	/* The parameters objects, with their sets. */
	for (;;) {
		parameters = drv_i915_object_take(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, NULL, NULL);
		if (parameters == NULL)
			break;

		/* Frees the one taken. */
		i915_video_parameters_free(parameters);
	}

	/* The sessions, with their batches unless the video engine may still read them. */
	for (;;) {
		video = drv_i915_object_take(session, I915_VK_OBJ_VIDEO_SESSION, NULL, NULL);
		if (video == NULL)
			break;

		/* Frees the one taken. */
		i915_video_session_free(session, video);
	}
}

/*
 * vkGetPhysicalDeviceVideoCapabilitiesKHR: [physical][present]{profile}
 * [present][shape: type, link...] -> [result][present]{capabilities, chain
 * nested in the shape's order}.
 */
static int
i915_video_capabilities(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t types[I915_VIDEO_MAX_CHAIN];
	uint32_t count;
	uint32_t idc;
	uint64_t present;
	int32_t result;

	UNUSED_PARAMETER(session);

	/* Skips the physical device and reads the profile, which decides the result. */
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	result = I915_VIDEO_ERROR_OPERATION;
	if (present != 0U)
		result = i915_video_read_profile(reader, &idc);

	/* Reads the shape of the output: the capabilities record and what is chained to it. */
	count = 0U;
	present = drv_i915_wire_read_u64(reader);
	while (present != 0U && reader->error == 0) {
		/* Keeps each type, the first being the capabilities record's own. */
		if (count >= I915_VIDEO_MAX_CHAIN) {
			reader->error = 1;
			break;
		}
		types[count] = drv_i915_wire_read_u32(reader);
		count++;
		present = drv_i915_wire_read_u64(reader);
	}
	if (reader->error != 0)
		return EINVAL;

	/* The output must be a capabilities record. */
	if (count == 0U || types[0] != I915_VIDEO_TYPE_CAPABILITIES)
		return EINVAL;

	/* A profile the decoder does not take replies its reason only. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Replies the result and the records in the shape's order. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	i915_video_reply_capabilities(reply, types, count);

	/* Succeeded: the capabilities are reported. */
	return 0;
}

/* Replies one output record of the capabilities' shape and, nested before its fields, the rest. */
static void
i915_video_reply_capabilities(
	struct i915_wire_writer *reply,
	const uint32_t *types,
	uint32_t count)
{
	char name[VK_MAX_EXTENSION_NAME_SIZE];

	/* The record's type, then the link to the rest of the shape. */
	drv_i915_wire_reply_u32(reply, types[0]);
	if (count > 1U) {
		drv_i915_wire_reply_u64(reply, 1U);
		i915_video_reply_capabilities(reply, types + 1, count - 1U);
	} else {
		drv_i915_wire_reply_u64(reply, 0U);
	}

	/* The record's fields. */
	switch (types[0]) {
	case I915_VIDEO_TYPE_CAPABILITIES:
		/* The general limits (design §3.3). */
		drv_i915_wire_reply_u32(reply, I915_VIDEO_SEPARATE_REFERENCE_IMAGES);
		drv_i915_wire_reply_u64(reply, 32U);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MB);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MB);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MB);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MB);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MAX_EXTENT);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MAX_EXTENT);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MAX_DPB_SLOTS);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MAX_REFERENCES);

		/* The H.264 header name, padded to its array, and version. */
		kern_memset(name, 0, sizeof(name));
		kern_memcpy(name, I915_VIDEO_STD_NAME, sizeof(I915_VIDEO_STD_NAME));
		drv_i915_wire_reply_u64(reply, VK_MAX_EXTENSION_NAME_SIZE);
		drv_i915_wire_reply_bytes(reply, name, sizeof(name));
		drv_i915_wire_reply_u32(reply, I915_VIDEO_STD_VERSION);
		break;
	case I915_VIDEO_TYPE_DECODE_CAPABILITIES:
		/* The output picture is the reference picture. */
		drv_i915_wire_reply_u32(reply, I915_VIDEO_DPB_AND_OUTPUT_COINCIDE);
		break;
	case I915_VIDEO_TYPE_H264_CAPABILITIES:
		/* Level 5.1 and no field offset (progressive only). */
		drv_i915_wire_reply_u32(reply, I915_VIDEO_LEVEL_5_1);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
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
i915_video_format_properties(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_video_chain chain;
	uint64_t present;
	uint32_t type;
	uint32_t usage;
	uint32_t video_usage;
	int32_t result;

	UNUSED_PARAMETER(session);

	/* Reads the format info: its type, its chain (the profile list) and the usage. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != I915_VIDEO_TYPE_FORMAT_INFO)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);
	usage = drv_i915_wire_read_u32(reader);

	/* Reads the capacity, which the one format always fits. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_video_chain_free(&chain);
	if (reader->error != 0)
		return EINVAL;

	/* The profiles must all be H.264 decode, and the usage only decode output and reference. */
	result = 0;
	video_usage = I915_VIDEO_USAGE_DECODE_DST | I915_VIDEO_USAGE_DECODE_DPB;
	if (!chain.has_profile_list || chain.profile_count == 0U) {
		result = I915_VIDEO_ERROR_OPERATION;
	} else if (!chain.profiles_supported) {
		result = chain.profile_error;
	} else if ((usage & ~video_usage) != 0U) {
		result = I915_VIDEO_ERROR_IMAGE_USAGE;
	}

	/* An unsupported query replies its reason only. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Replies the one format: NV12, identity swizzle, 2D, optimal (Tile Y), decode output and reference. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, I915_VIDEO_TYPE_FORMAT_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0U);
	drv_i915_wire_reply_u32(reply, I915_VIDEO_FORMAT_NV12);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_TYPE_2D);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_TILING_OPTIMAL);
	drv_i915_wire_reply_u32(reply, video_usage);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * The codec operations of each queue family: [physical][count] ->
 * [present][count]{operations}.  Family 1 decodes H.264.
 */
static int
i915_video_family_properties(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t count;
	uint32_t index;

	UNUSED_PARAMETER(session);

	/* Skips the physical device and reads the family count, which is the two families. */
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || count != 2U)
		return EINVAL;

	/* Replies one word for each family: none for graphics, H.264 decode for the video family. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, count);
	for (index = 0U; index < count; index++) {
		/* Only the video family decodes. */
		if (index == 1U)
			drv_i915_wire_reply_u32(reply, I915_VIDEO_OP_DECODE_H264);
		else
			drv_i915_wire_reply_u32(reply, 0U);
	}

	/* Succeeded: the families' codec operations are reported. */
	return 0;
}

/*
 * vkCreateVideoSessionKHR: [device][present]{create info}[allocator][present]
 * [identity] -> [result][present][identity].
 *
 * The first video session of a session gets the VCS0 hardware context.
 */
static int
i915_video_session_create(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_video_session *video;
	struct i915_video_chain chain;
	struct i915_device *device;
	char name[VK_MAX_EXTENSION_NAME_SIZE];
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
	int32_t profile;
	int32_t result;
	int different;
	int state;
	int error;

	/* Reads the create info: [type][chain][family][flags]. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(name, 0, sizeof(name));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != I915_VIDEO_TYPE_SESSION_CREATE)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);
	family = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);

	/* Reads the profile, the formats, the limits and the header's name and version. */
	profile = I915_VIDEO_ERROR_OPERATION;
	profile_idc = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		profile = i915_video_read_profile(reader, &profile_idc);
	picture_format = drv_i915_wire_read_u32(reader);
	width = drv_i915_wire_read_u32(reader);
	height = drv_i915_wire_read_u32(reader);
	reference_format = drv_i915_wire_read_u32(reader);
	slots = drv_i915_wire_read_u32(reader);
	references = drv_i915_wire_read_u32(reader);
	version = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_video_read_extension(reader, name, &version);

	/* Skips pAllocator and the output's present word, and reads the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	i915_video_chain_free(&chain);
	if (reader->error != 0)
		return EINVAL;

	/* Checks the session against what the decoder takes; the first reason is the result. */
	result = profile;
	different = kern_strcmp(name, I915_VIDEO_STD_NAME);
	if (result != 0) {
		/* The profile's own reason. */
	} else if (family != 1U) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (flags != 0U) {
		/* Protected content and inline queries are not offered. */
		result = VK_ERROR_FEATURE_NOT_PRESENT;
	} else if (picture_format != I915_VIDEO_FORMAT_NV12 || reference_format != I915_VIDEO_FORMAT_NV12) {
		result = I915_VIDEO_ERROR_FORMAT;
	} else if (width == 0U || height == 0U || width > I915_VIDEO_MAX_EXTENT || height > I915_VIDEO_MAX_EXTENT) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (slots > I915_VIDEO_MAX_DPB_SLOTS || references > I915_VIDEO_MAX_REFERENCES) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (different != 0 || version > I915_VIDEO_STD_VERSION) {
		result = I915_VIDEO_ERROR_STD_VERSION;
	}

	/* A hung video engine takes no new session, nor does a session quarantined by a hang of its own (ws083 R-S3). */
	state = drv_i915_worker_video_state(session->vk->i915);
	if (result == 0 && state != 0)
		result = VK_ERROR_INITIALIZATION_FAILED;
	if (result == 0 && session->gpu->quarantined != 0U)
		result = VK_ERROR_INITIALIZATION_FAILED;

	/* A refused session replies its reason only. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Allocates the session with its limits, every slot inactive and nothing bound. */
	video = kern_calloc(1U, sizeof(*video));
	if (video == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}
	video->profile_idc = profile_idc;
	video->width_mbs = i915_video_mbs(width);
	video->height_mbs = i915_video_mbs(height);
	video->max_dpb_slots = slots;
	video->max_references = references;
	video->bind_count = I915_VIDEO_ROW_STORES + slots + 1U;

	/*
	 * Gives the session's VCS0 context its hardware context, once: the
	 * device mutex makes the first attach the only one.
	 */
	device = session->vk->i915;
	mutex_lock(&device->mutex);

	error = drv_i915_worker_context_attach(device, &session->gpu->contexts[I915_ENGINE_VCS0]);

	mutex_unlock(&device->mutex);

	/* Without a VCS0 context the session cannot decode: the context table is full, or the engine went away. */
	if (error != 0) {
		kern_free(video);
		kern_logf("i915: video: no VCS0 context for a video session: error %d\n", error);
		result = VK_ERROR_INITIALIZATION_FAILED;
		if (error == ENOMEM)
			result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Publishes the session under its identity. */
	error = drv_i915_object_insert(session, I915_VK_OBJ_VIDEO_SESSION, identity, video);
	if (error != 0) {
		kern_free(video);
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}

	/* Replies VK_SUCCESS and the identity. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the session exists. */
	return 0;
}

/* vkDestroyVideoSessionKHR: [device][session][allocator], no reply body; bound memory stays the application's. */
static int
i915_video_session_destroy(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	struct i915_video_session *video;
	uint64_t identity;

	/* Skips the device, reads the session and skips pAllocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Forgets and frees the session; a null or unknown one is nothing. */
	video = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (video == NULL)
		return 0;
	drv_i915_object_remove(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	i915_video_session_free(session, video);

	/* Succeeded: the session is gone. */
	return 0;
}

/*
 * vkGetVideoSessionMemoryRequirementsKHR: [device][session][present][capacity]
 * [array] -> [result][present][count][array]{binding}.
 */
static int
i915_video_requirements(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_video_session *video;
	uint64_t identity;
	uint64_t size;
	uint32_t capacity;
	uint32_t index;

	/* Skips the device, reads the session and the capacity. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown session, or one whose bindings do not fit, is refused. */
	video = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (video == NULL || video->bind_count > capacity) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	/* Replies every binding with its size, the 4 KiB alignment and the one memory type. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, video->bind_count);
	drv_i915_wire_reply_u64(reply, video->bind_count);
	for (index = 0U; index < video->bind_count; index++) {
		/* One binding: its type, no chain, its index, its size, alignment and memory types. */
		i915_video_session_sizes(video, index, &size);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_TYPE_MEMORY_REQUIREMENTS);
		drv_i915_wire_reply_u64(reply, 0U);
		drv_i915_wire_reply_u32(reply, index);
		drv_i915_wire_reply_u64(reply, size);
		drv_i915_wire_reply_u64(reply, I915_VIDEO_ALIGN);
		drv_i915_wire_reply_u32(reply, I915_VIDEO_MEMORY_TYPES);
	}

	/* Succeeded: the bindings are reported. */
	return 0;
}

/*
 * The size of one binding of a session (design §1.5, §6.4): the four row
 * stores by the picture width in macroblocks, then a motion vector buffer
 * of 128 bytes a macroblock for each slot and for a picture without one,
 * each rounded up to 4 KiB.
 */
static void
i915_video_session_sizes(
	const struct i915_video_session *video,
	uint32_t index,
	uint64_t *size)
{
	uint64_t bytes;

	/* Picks the store or buffer the index names. */
	if (index == 0U) {
		/* The intra row store. */
		bytes = (uint64_t)video->width_mbs * 64U;
	} else if (index == 1U) {
		/* The deblocking filter row store. */
		bytes = (uint64_t)video->width_mbs * 64U * 4U;
	} else if (index == 2U || index == 3U) {
		/* The BSD/MPC and the MPR row stores. */
		bytes = (uint64_t)video->width_mbs * 64U * 2U;
	} else {
		/* A direct motion vector buffer. */
		bytes = (uint64_t)video->width_mbs * video->height_mbs * 128U;
	}

	/* Every binding is whole 4 KiB pages. */
	*size = (bytes + I915_VIDEO_ALIGN - 1U) & ~(uint64_t)(I915_VIDEO_ALIGN - 1U);
}

/*
 * vkBindVideoSessionMemoryKHR: [device][session][count][array]{bind} -> [result].
 *
 * The memory is kept by its identity and looked up again at each submit.
 */
static int
i915_video_bind(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_video_session *video;
	struct i915_video_bind incoming;
	struct i915_video_bind kept[I915_VIDEO_MAX_BINDS];
	uint32_t indexes[I915_VIDEO_MAX_BINDS];
	struct i915_gfx_memory *memory;
	struct i915_video_chain chain;
	uint64_t identity;
	uint64_t count;
	uint64_t item;
	uint64_t size;
	uint32_t type;
	uint32_t index;
	uint32_t taken;
	int32_t result;

	/* Skips the device, reads the session and the binding count. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Reads every binding: [type][chain][index][memory][offset][size]; the first wrong one decides. */
	video = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	result = 0;
	if (video == NULL)
		result = VK_ERROR_INITIALIZATION_FAILED;
	taken = 0U;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* One binding. */
		kern_memset(&chain, 0, sizeof(chain));
		type = drv_i915_wire_read_u32(reader);
		if (type != I915_VIDEO_TYPE_BIND_MEMORY)
			reader->error = 1;
		i915_video_read_chain(reader, &chain, 0U);
		i915_video_chain_free(&chain);
		index = drv_i915_wire_read_u32(reader);
		incoming.memory = drv_i915_wire_read_u64(reader);
		incoming.offset = drv_i915_wire_read_u64(reader);
		incoming.size = drv_i915_wire_read_u64(reader);

		/* After a refusal the rest is only read. */
		if (result != 0)
			continue;

		/* A binding past the session's, or more of them than it has, is refused. */
		if (index >= video->bind_count || taken >= I915_VIDEO_MAX_BINDS) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		}

		/* The memory must exist, and the range be in it, page-aligned and large enough. */
		memory = drv_i915_object_lookup(session, I915_VK_OBJ_MEMORY, incoming.memory);
		i915_video_session_sizes(video, index, &size);
		if (memory == NULL ||
		    (incoming.offset % I915_VIDEO_ALIGN) != 0U ||
		    incoming.offset > memory->size ||
		    incoming.size > memory->size - incoming.offset ||
		    incoming.size < size) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		}

		/* Keeps the binding until every one is read. */
		kept[taken] = incoming;
		indexes[taken] = index;
		taken++;
	}
	if (reader->error != 0)
		return EINVAL;

	/* Binds them all, or none when one was refused. */
	if (result == 0) {
		for (index = 0U; index < taken; index++) {
			video->binds[indexes[index]] = kept[index];
			video->bound_mask |= 1U << indexes[index];
		}
	}

	/* Replies the result. */
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
i915_video_parameters_create(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_video_parameters *parameters;
	struct i915_video_parameters *source;
	struct i915_video_chain chain;
	uint64_t present;
	uint64_t identity;
	uint64_t template_identity;
	uint64_t session_identity;
	uint32_t type;
	int32_t result;
	int error;

	/* Reads the create info: [type][chain][flags][template][session], then the identity. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != I915_VIDEO_TYPE_PARAMETERS_CREATE)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);
	(void)drv_i915_wire_read_u32(reader);
	template_identity = drv_i915_wire_read_u64(reader);
	session_identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0) {
		i915_video_chain_free(&chain);
		return EINVAL;
	}

	/* The session must exist and the H.264 create record be given; a template must be of the same session. */
	result = 0;
	source = NULL;
	if (template_identity != 0U)
		source = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, template_identity);
	if (drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION, session_identity) == NULL) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (!chain.has_h264_create || chain.add_error != 0) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (template_identity != 0U && (source == NULL || source->session != session_identity)) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	}

	/* A refused object replies its reason only. */
	if (result != 0) {
		i915_video_chain_free(&chain);
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Allocates the object. */
	parameters = kern_calloc(1U, sizeof(*parameters));
	if (parameters == NULL) {
		i915_video_chain_free(&chain);
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}
	parameters->session = session_identity;
	parameters->max_sps = chain.max_sps;
	parameters->max_pps = chain.max_pps;

	/* Copies the template's sets, then takes the new ones, which replace sets of the same key. */
	error = 0;
	if (source != NULL)
		error = i915_video_parameters_copy(parameters, source);
	if (error == 0)
		error = i915_video_parameters_take(parameters, &chain, 1);
	i915_video_chain_free(&chain);

	/* Publishes the object under its identity. */
	if (error == 0)
		error = drv_i915_object_insert(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity, parameters);
	if (error != 0) {
		i915_video_parameters_free(parameters);
		result = VK_ERROR_OUT_OF_HOST_MEMORY;
		if (error == EINVAL)
			result = VK_ERROR_INITIALIZATION_FAILED;
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Replies VK_SUCCESS and the identity. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the object exists. */
	return 0;
}

/*
 * vkUpdateVideoSessionParametersKHR: [device][parameters][present]{update
 * info: chain (add), sequence count} -> [result].
 *
 * The update adds its sets whole or not at all.
 */
static int
i915_video_parameters_update(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_video_parameters *parameters;
	struct i915_video_chain chain;
	uint64_t identity;
	uint64_t present;
	uint32_t type;
	uint32_t sequence;
	int32_t result;
	int error;

	/* Reads the object and the update info: [type][chain][sequence count]. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != I915_VIDEO_TYPE_PARAMETERS_UPDATE)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);
	sequence = drv_i915_wire_read_u32(reader);
	if (reader->error != 0) {
		i915_video_chain_free(&chain);
		return EINVAL;
	}

	/* The object must exist and the update be the next one; a malformed set refuses it. */
	result = 0;
	parameters = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity);
	if (parameters == NULL || sequence != parameters->update_sequence + 1U || chain.add_error != 0)
		result = VK_ERROR_INITIALIZATION_FAILED;

	/* Adds the sets; a key already held refuses the whole update. */
	if (result == 0) {
		error = i915_video_parameters_take(parameters, &chain, 0);
		if (error == ENOMEM)
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
		else if (error != 0)
			result = VK_ERROR_INITIALIZATION_FAILED;
	}
	i915_video_chain_free(&chain);

	/* The update counts only when it was taken. */
	if (result == 0)
		parameters->update_sequence = sequence;

	/* Replies the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)result);

	/* Succeeded: the command was decoded; the reply carries its result. */
	return 0;
}

/* vkDestroyVideoSessionParametersKHR: [device][parameters][allocator], no reply body. */
static int
i915_video_parameters_destroy(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	struct i915_video_parameters *parameters;
	uint64_t identity;

	/* Skips the device, reads the object and skips pAllocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Forgets and frees the object with its sets; a null or unknown one is nothing. */
	parameters = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity);
	if (parameters == NULL)
		return 0;
	drv_i915_object_remove(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity);
	i915_video_parameters_free(parameters);

	/* Succeeded: the object is gone. */
	return 0;
}

/*
 * Reads a chain: an absent link ends it; a present one carries a structure
 * whose own chain comes before its fields.
 */
static void
i915_video_read_chain(
	struct i915_wire_reader *reader,
	struct i915_video_chain *chain,
	uint32_t depth)
{
	uint64_t present;
	uint32_t type;

	/* An absent link ends the chain. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present == 0U)
		return;

	/* Refuses a chain deeper than any the library writes. */
	if (depth >= I915_VIDEO_MAX_CHAIN) {
		reader->error = 1;
		return;
	}

	/* The linked structure: its type, its own chain, then its fields. */
	type = drv_i915_wire_read_u32(reader);
	i915_video_read_chain(reader, chain, depth + 1U);
	i915_video_read_fields(reader, type, chain, depth);
}

/* Reads the fields of one chained structure into the chain's record. */
static void
i915_video_read_fields(
	struct i915_wire_reader *reader,
	uint32_t type,
	struct i915_video_chain *chain,
	uint32_t depth)
{
	struct i915_video_chain nested;
	uint64_t present;
	uint64_t count;
	uint64_t item;
	uint32_t value;
	int32_t result;

	/* Reads each chained type the library writes; any other fails the stream. */
	switch (type) {
	case I915_VIDEO_TYPE_H264_PROFILE:
		/* The H.264 profile and the picture layout. */
		chain->has_h264_profile = 1;
		chain->profile_idc = drv_i915_wire_read_u32(reader);
		chain->picture_layout = drv_i915_wire_read_u32(reader);
		break;
	case I915_VIDEO_TYPE_DECODE_USAGE:
		/* Usage hints, which the decoder does not use. */
		(void)drv_i915_wire_read_u32(reader);
		break;
	case I915_VIDEO_TYPE_PROFILE_LIST:
		/* [count][array]{profile}: every profile must be H.264 decode; the first refusal is kept. */
		chain->has_profile_list = 1;
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		chain->profile_count = (uint32_t)count;
		chain->profiles_supported = 1;
		for (item = 0U; item < count && reader->error == 0; item++) {
			/* One profile structure. */
			result = i915_video_read_profile(reader, &value);
			if (result != 0 && chain->profiles_supported) {
				chain->profiles_supported = 0;
				chain->profile_error = result;
			}
		}
		break;
	case I915_VIDEO_TYPE_H264_PARAMETERS_CREATE:
		/* The capacity, then the add record the create carries, if any. */
		chain->has_h264_create = 1;
		chain->max_sps = drv_i915_wire_read_u32(reader);
		chain->max_pps = drv_i915_wire_read_u32(reader);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			/* The add record is a whole structure: its type, chain and fields. */
			value = drv_i915_wire_read_u32(reader);
			if (value != I915_VIDEO_TYPE_H264_PARAMETERS_ADD)
				reader->error = 1;
			kern_memset(&nested, 0, sizeof(nested));
			i915_video_read_chain(reader, &nested, depth + 1U);
			i915_video_chain_free(&nested);
			i915_video_read_add(reader, chain);
		}
		break;
	case I915_VIDEO_TYPE_H264_PARAMETERS_ADD:
		/* The sets added. */
		i915_video_read_add(reader, chain);
		break;
	case I915_VIDEO_TYPE_H264_DPB_SLOT:
		/* [present][flags][FrameNum][reserved][2][PicOrderCnt x2]. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_reference = 1;
			chain->reference_flags = drv_i915_wire_read_u32(reader);
			chain->frame_num = drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			count = drv_i915_wire_read_u64(reader);
			if (count != 2U)
				reader->error = 1;
			chain->poc[0] = (int32_t)drv_i915_wire_read_u32(reader);
			chain->poc[1] = (int32_t)drv_i915_wire_read_u32(reader);
		}
		break;
	case I915_VIDEO_TYPE_H264_PICTURE:
		/* [present]{picture}[slice count][array]{offset}: the slices past the record's room are read and dropped. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_picture = 1;
			chain->picture_flags = drv_i915_wire_read_u32(reader);
			chain->sps_id = drv_i915_wire_read_u32(reader);
			chain->pps_id = drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			chain->picture_frame_num = drv_i915_wire_read_u32(reader);
			chain->idr_pic_id = drv_i915_wire_read_u32(reader);
			count = drv_i915_wire_read_u64(reader);
			if (count != 2U)
				reader->error = 1;
			chain->picture_poc[0] = (int32_t)drv_i915_wire_read_u32(reader);
			chain->picture_poc[1] = (int32_t)drv_i915_wire_read_u32(reader);
		}
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		chain->slice_count = (uint32_t)count;
		if (count > (uint64_t)(reader->size - reader->offset))
			reader->error = 1;
		for (item = 0U; item < count && reader->error == 0; item++) {
			/* Keeps the offsets that fit. */
			value = drv_i915_wire_read_u32(reader);
			if (item < I915_VIDEO_MAX_SLICES)
				chain->slices[item] = value;
		}
		break;
	default:
		/* A structure the library does not write here. */
		reader->error = 1;
		break;
	}
}

/*
 * Reads a profile structure: its type, its chain and its four fields.
 * Returns 0 for the H.264 decode profile, or the standard reason another is
 * refused; the H.264 profile it names goes to `profile_idc`.
 */
static int32_t
i915_video_read_profile(
	struct i915_wire_reader *reader,
	uint32_t *profile_idc)
{
	struct i915_video_chain chain;
	uint32_t type;
	uint32_t operation;
	uint32_t chroma;
	uint32_t luma;
	uint32_t chroma_depth;
	int32_t result;

	/* The type, then the chain that carries the H.264 profile. */
	kern_memset(&chain, 0, sizeof(chain));
	type = drv_i915_wire_read_u32(reader);
	if (type != I915_VIDEO_TYPE_PROFILE)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);
	operation = drv_i915_wire_read_u32(reader);
	chroma = drv_i915_wire_read_u32(reader);
	luma = drv_i915_wire_read_u32(reader);
	chroma_depth = drv_i915_wire_read_u32(reader);
	i915_video_chain_free(&chain);

	/* Decides the profile. */
	result = i915_video_profile_result(operation, chroma, luma, chroma_depth, &chain);
	*profile_idc = chain.profile_idc;

	/* Succeeded: reports the profile's result. */
	return result;
}

/* Decides whether a profile is the H.264 decode profile, or why not, in the standard order. */
static int32_t
i915_video_profile_result(
	uint32_t operation,
	uint32_t chroma,
	uint32_t luma,
	uint32_t chroma_depth,
	const struct i915_video_chain *chain)
{
	/* H.264 decode is the one operation. */
	if (operation != I915_VIDEO_OP_DECODE_H264)
		return I915_VIDEO_ERROR_OPERATION;

	/* 8-bit 4:2:0 is the one format. */
	if (chroma != I915_VIDEO_CHROMA_420 || luma != I915_VIDEO_DEPTH_8 || chroma_depth != I915_VIDEO_DEPTH_8)
		return I915_VIDEO_ERROR_FORMAT;

	/* The H.264 record must be there and progressive. */
	if (!chain->has_h264_profile)
		return I915_VIDEO_ERROR_CODEC;
	if (chain->picture_layout != I915_VIDEO_LAYOUT_PROGRESSIVE)
		return I915_VIDEO_ERROR_LAYOUT;

	/* Baseline, Main and High. */
	if (chain->profile_idc != I915_VIDEO_PROFILE_BASELINE &&
	    chain->profile_idc != I915_VIDEO_PROFILE_MAIN &&
	    chain->profile_idc != I915_VIDEO_PROFILE_HIGH)
		return I915_VIDEO_ERROR_CODEC;

	/* Succeeded: the profile is supported. */
	return 0;
}

/* Reads a picture resource structure: [type][chain 0][x][y][width][height][layer][view]. */
static void
i915_video_read_resource(
	struct i915_wire_reader *reader,
	struct i915_video_resource *resource)
{
	struct i915_video_chain chain;
	uint32_t type;

	/* The type and an empty chain. */
	kern_memset(&chain, 0, sizeof(chain));
	type = drv_i915_wire_read_u32(reader);
	if (type != I915_VIDEO_TYPE_PICTURE_RESOURCE)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);
	i915_video_chain_free(&chain);

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
i915_video_read_slot(
	struct i915_wire_reader *reader,
	struct i915_video_slot_info *slot)
{
	struct i915_video_chain chain;
	uint64_t present;
	uint32_t type;

	/* The type and the chain, which may carry the H.264 reference. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(slot, 0, sizeof(*slot));
	type = drv_i915_wire_read_u32(reader);
	if (type != I915_VIDEO_TYPE_REFERENCE_SLOT)
		reader->error = 1;
	i915_video_read_chain(reader, &chain, 0U);
	i915_video_chain_free(&chain);
	slot->has_reference = chain.has_reference;
	slot->reference_flags = chain.reference_flags;
	slot->frame_num = chain.frame_num;
	slot->poc[0] = chain.poc[0];
	slot->poc[1] = chain.poc[1];

	/* The slot index and the picture, whose absence deactivates the slot in a begin. */
	slot->index = (int32_t)drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		slot->has_picture = 1;
		i915_video_read_resource(reader, &slot->picture);
	}
}

/* Reads an array of slots into a record, marking one that carried more than it holds. */
static void
i915_video_record_slots(
	struct i915_wire_reader *reader,
	struct i915_video_command *command,
	struct i915_video_slot_info *slots,
	uint32_t limit,
	uint32_t *count)
{
	struct i915_video_slot_info dropped;
	uint64_t total;
	uint64_t item;

	/* [count][array]: a count past the bytes left is malformed. */
	(void)drv_i915_wire_read_u32(reader);
	total = drv_i915_wire_read_u64(reader);
	if (total > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;

	/* Reads every slot; those past the room are read and dropped, and refuse the submission. */
	*count = 0U;
	for (item = 0U; item < total && reader->error == 0; item++) {
		/* One slot. */
		if (item < limit) {
			i915_video_read_slot(reader, &slots[item]);
			*count = (uint32_t)item + 1U;
		} else {
			i915_video_read_slot(reader, &dropped);
			command->oversize = 1;
		}
	}
}

/*
 * Reads an H.264 add record's fields: [count][array]{SPS}[count][array]{PPS}.
 * Each set is a new allocation kept on the chain; a set the executor cannot
 * keep marks add_error.
 */
static void
i915_video_read_add(
	struct i915_wire_reader *reader,
	struct i915_video_chain *chain)
{
	struct i915_video_sps scratch_sps;
	struct i915_video_pps scratch_pps;
	struct i915_video_sps *sps;
	struct i915_video_pps *pps;
	uint64_t count;
	uint64_t item;
	uint64_t offsets;
	uint64_t present;
	uint32_t index;
	uint32_t value;

	/* The sequence parameter sets. */
	chain->has_add = 1;
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* A set past the id range, or one that cannot be allocated, is read into scratch. */
		sps = NULL;
		if (chain->new_sps_count < I915_VIDEO_SPS_IDS)
			sps = kern_calloc(1U, sizeof(*sps));
		if (sps == NULL) {
			kern_memset(&scratch_sps, 0, sizeof(scratch_sps));
			sps = &scratch_sps;
			chain->add_error = ENOMEM;
		}

		/* The fields in declared order (libvulkan video_encode_sps). */
		sps->flags = drv_i915_wire_read_u32(reader);
		sps->profile_idc = drv_i915_wire_read_u32(reader);
		sps->level_idc = drv_i915_wire_read_u32(reader);
		sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
		sps->id = drv_i915_wire_read_u32(reader);
		sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
		sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);
		sps->log2_max_frame_num_minus4 = drv_i915_wire_read_u32(reader);
		sps->pic_order_cnt_type = drv_i915_wire_read_u32(reader);
		sps->offset_for_non_ref_pic = (int32_t)drv_i915_wire_read_u32(reader);
		sps->offset_for_top_to_bottom_field = (int32_t)drv_i915_wire_read_u32(reader);
		sps->log2_max_pic_order_cnt_lsb_minus4 = drv_i915_wire_read_u32(reader);
		sps->num_ref_frames_in_pic_order_cnt_cycle = drv_i915_wire_read_u32(reader);
		sps->max_num_ref_frames = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		sps->pic_width_in_mbs_minus1 = drv_i915_wire_read_u32(reader);
		sps->pic_height_in_map_units_minus1 = drv_i915_wire_read_u32(reader);
		for (index = 0U; index < 4U; index++)
			sps->crop[index] = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);

		/* The picture order count cycle's offsets, at most 255. */
		offsets = drv_i915_wire_read_u64(reader);
		if (offsets > 255U)
			reader->error = 1;
		sps->offset_count = (uint32_t)offsets;
		for (index = 0U; index < (uint32_t)offsets && reader->error == 0; index++)
			sps->offsets[index] = (int32_t)drv_i915_wire_read_u32(reader);

		/* The scaling lists, then the VUI, which is never sent. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			sps->has_scaling = 1;
			i915_video_read_scaling(reader, &sps->scaling);
		}
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U)
			reader->error = 1;

		/* Keeps the set on the chain; an id outside H.264 marks the add malformed. */
		if (sps->id >= I915_VIDEO_SPS_IDS)
			chain->add_error = EINVAL;
		if (sps != &scratch_sps) {
			chain->new_sps[chain->new_sps_count] = sps;
			chain->new_sps_count++;
		}
	}

	/* The picture parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* A set that cannot be allocated is read into scratch. */
		pps = kern_calloc(1U, sizeof(*pps));
		if (pps == NULL) {
			kern_memset(&scratch_pps, 0, sizeof(scratch_pps));
			pps = &scratch_pps;
			chain->add_error = ENOMEM;
		}

		/* The fields in declared order (libvulkan video_encode_pps). */
		pps->flags = drv_i915_wire_read_u32(reader);
		pps->sps_id = drv_i915_wire_read_u32(reader);
		pps->pps_id = drv_i915_wire_read_u32(reader);
		pps->num_ref_idx_l0_default_active_minus1 = drv_i915_wire_read_u32(reader);
		pps->num_ref_idx_l1_default_active_minus1 = drv_i915_wire_read_u32(reader);
		pps->weighted_bipred_idc = drv_i915_wire_read_u32(reader);
		pps->pic_init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
		pps->pic_init_qs_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
		pps->chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
		pps->second_chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			pps->has_scaling = 1;
			i915_video_read_scaling(reader, &pps->scaling);
		}

		/* Keeps the set on the chain; ids outside H.264 mark the add malformed. */
		value = pps->pps_id;
		if (pps->sps_id >= I915_VIDEO_SPS_IDS || value >= I915_VIDEO_PPS_IDS)
			chain->add_error = EINVAL;
		if (pps != &scratch_pps) {
			pps->next = chain->new_pps;
			chain->new_pps = pps;
			chain->new_pps_count++;
		}
	}
}

/* Reads H.264 scaling lists: [present mask][default mask][96]{4x4}[384]{8x8}. */
static void
i915_video_read_scaling(
	struct i915_wire_reader *reader,
	struct i915_video_scaling *scaling)
{
	uint64_t count;

	/* The two masks. */
	scaling->present_mask = drv_i915_wire_read_u32(reader);
	scaling->default_mask = drv_i915_wire_read_u32(reader);

	/* The 4x4 lists, list after list. */
	count = drv_i915_wire_read_u64(reader);
	if (count != sizeof(scaling->list4))
		reader->error = 1;
	i915_vkc_read_bytes(reader, scaling->list4, sizeof(scaling->list4));

	/* The 8x8 lists, list after list. */
	count = drv_i915_wire_read_u64(reader);
	if (count != sizeof(scaling->list8))
		reader->error = 1;
	i915_vkc_read_bytes(reader, scaling->list8, sizeof(scaling->list8));
}

/* Reads an extension name and version: [256][name bytes][version]. */
static void
i915_video_read_extension(
	struct i915_wire_reader *reader,
	char *name,
	uint32_t *version)
{
	uint64_t count;

	/* The name, whose array is always the full extent. */
	count = drv_i915_wire_read_u64(reader);
	if (count != VK_MAX_EXTENSION_NAME_SIZE)
		reader->error = 1;
	i915_vkc_read_bytes(reader, name, VK_MAX_EXTENSION_NAME_SIZE);
	name[VK_MAX_EXTENSION_NAME_SIZE - 1U] = '\0';

	/* The version. */
	*version = drv_i915_wire_read_u32(reader);
}

/* Frees the parameter sets a chain read and nobody took. */
static void
i915_video_chain_free(
	struct i915_video_chain *chain)
{
	struct i915_video_pps *pps;
	uint32_t index;

	/* The sequence sets. */
	for (index = 0U; index < chain->new_sps_count; index++) {
		kern_free(chain->new_sps[index]);
		chain->new_sps[index] = NULL;
	}
	chain->new_sps_count = 0U;

	/* The picture sets. */
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;
		kern_free(pps);
	}
	chain->new_pps_count = 0U;
}

/* Finds the picture set of a key in a parameters object. */
static const struct i915_video_pps *
i915_video_find_pps(
	const struct i915_video_parameters *parameters,
	uint32_t sps_id,
	uint32_t pps_id)
{
	const struct i915_video_pps *pps;

	/* An id outside H.264 names nothing. */
	if (pps_id >= I915_VIDEO_PPS_IDS)
		return NULL;

	/* The sets of the picture id, by their sequence id. */
	for (pps = parameters->pps[pps_id]; pps != NULL; pps = pps->next) {
		if (pps->sps_id == sps_id)
			return pps;
	}

	/* No set has the key. */
	return NULL;
}

/*
 * Moves the sets a chain read into a parameters object.
 *
 * With `replace`, a set replaces one of the same key (a creation); without
 * it a key the object holds refuses the whole move (EEXIST) and nothing
 * changes (an update).  The object's capacity bounds what it holds
 * (EINVAL).  The chain is left with the sets nobody took.
 */
static int
i915_video_parameters_take(
	struct i915_video_parameters *parameters,
	struct i915_video_chain *chain,
	int replace)
{
	struct i915_video_pps *pps;
	struct i915_video_pps **link;
	struct i915_video_pps *next;
	const struct i915_video_pps *held;
	uint32_t index;
	uint32_t later;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t id;

	/* A malformed add takes nothing. */
	if (chain->add_error != 0)
		return chain->add_error;

	/* An update cannot add a key the object holds, nor the same key twice. */
	if (!replace) {
		for (index = 0U; index < chain->new_sps_count; index++) {
			/* The object's sets. */
			if (parameters->sps[chain->new_sps[index]->id] != NULL)
				return EEXIST;

			/* The update's own later sets. */
			for (later = index + 1U; later < chain->new_sps_count; later++) {
				if (chain->new_sps[later]->id == chain->new_sps[index]->id)
					return EEXIST;
			}
		}
		for (pps = chain->new_pps; pps != NULL; pps = pps->next) {
			/* The object's sets. */
			held = i915_video_find_pps(parameters, pps->sps_id, pps->pps_id);
			if (held != NULL)
				return EEXIST;

			/* The update's own other sets. */
			for (next = pps->next; next != NULL; next = next->next) {
				if (next->sps_id == pps->sps_id && next->pps_id == pps->pps_id)
					return EEXIST;
			}
		}
	}

	/* An update may not take the object past its capacity (a creation's sets are counted by the API). */
	if (!replace) {
		sps_count = 0U;
		for (index = 0U; index < I915_VIDEO_SPS_IDS; index++) {
			if (parameters->sps[index] != NULL)
				sps_count++;
		}
		pps_count = 0U;
		for (index = 0U; index < I915_VIDEO_PPS_IDS; index++) {
			for (held = parameters->pps[index]; held != NULL; held = held->next)
				pps_count++;
		}
		if (sps_count + chain->new_sps_count > parameters->max_sps)
			return EINVAL;
		if (pps_count + chain->new_pps_count > parameters->max_pps)
			return EINVAL;
	}

	/* Moves the sequence sets, each replacing the set of its id. */
	for (index = 0U; index < chain->new_sps_count; index++) {
		id = chain->new_sps[index]->id;
		kern_free(parameters->sps[id]);
		parameters->sps[id] = chain->new_sps[index];
		chain->new_sps[index] = NULL;
	}
	chain->new_sps_count = 0U;

	/* Moves the picture sets, each replacing the set of its key. */
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;

		/* Unlinks a held set of the same key. */
		link = &parameters->pps[pps->pps_id];
		while (*link != NULL) {
			/* The held set of the key goes. */
			if ((*link)->sps_id == pps->sps_id) {
				next = *link;
				*link = next->next;
				kern_free(next);
				break;
			}
			link = &(*link)->next;
		}

		/* Links the new set first. */
		pps->next = parameters->pps[pps->pps_id];
		parameters->pps[pps->pps_id] = pps;
	}
	chain->new_pps_count = 0U;

	/* Succeeded: the object holds the sets. */
	return 0;
}

/* Copies every set of a template into a new parameters object. */
static int
i915_video_parameters_copy(
	struct i915_video_parameters *destination,
	const struct i915_video_parameters *source)
{
	struct i915_video_sps *sps;
	struct i915_video_pps *pps;
	const struct i915_video_pps *held;
	uint32_t index;

	/* The sequence sets. */
	for (index = 0U; index < I915_VIDEO_SPS_IDS; index++) {
		if (source->sps[index] == NULL)
			continue;

		/* One copy. */
		sps = kern_calloc(1U, sizeof(*sps));
		if (sps == NULL)
			return ENOMEM;
		*sps = *source->sps[index];
		destination->sps[index] = sps;
	}

	/* The picture sets. */
	for (index = 0U; index < I915_VIDEO_PPS_IDS; index++) {
		for (held = source->pps[index]; held != NULL; held = held->next) {
			/* One copy, linked first. */
			pps = kern_calloc(1U, sizeof(*pps));
			if (pps == NULL)
				return ENOMEM;
			*pps = *held;
			pps->next = destination->pps[index];
			destination->pps[index] = pps;
		}
	}

	/* Succeeded: the new object holds the template's sets. */
	return 0;
}

/* Frees a parameters object and every set it holds. */
static void
i915_video_parameters_free(
	struct i915_video_parameters *parameters)
{
	struct i915_video_pps *pps;
	uint32_t index;

	/* The sequence sets. */
	for (index = 0U; index < I915_VIDEO_SPS_IDS; index++)
		kern_free(parameters->sps[index]);

	/* The picture sets. */
	for (index = 0U; index < I915_VIDEO_PPS_IDS; index++) {
		while (parameters->pps[index] != NULL) {
			pps = parameters->pps[index];
			parameters->pps[index] = pps->next;
			kern_free(pps);
		}
	}

	/* The object. */
	kern_free(parameters);
}

/*
 * Frees a video session.  Its batch, when it has one, is destroyed, unless
 * a video engine hang quarantined the session: then the batch joins the
 * session's quarantine and the checked reset frees it (design §6.1,
 * ws083-p007).  The engine runs one request at a time and the one that
 * hung was the quarantined session's, so the batch of any other session
 * never ran on a hung engine and goes at once, whatever the engine's state.
 */
static void
i915_video_session_free(
	struct i915_render_session *session,
	struct i915_video_session *video)
{
	struct i915_device *device;

	/* Retains a quarantined session's batch on the registry, as the session's other objects are. */
	if (video->batch != NULL && session->gpu->quarantined != 0U) {
		device = session->vk->i915;
		mutex_lock(&device->mutex);

		video->batch->quarantined = 1U;
		drv_i915_gem_destroy(&device->gem, video->batch);

		mutex_unlock(&device->mutex);

		kern_logf("i915: video: batch of a quarantined session retained for the checked reset\n");
	} else if (video->batch != NULL) {
		/* Any other batch is unbound and freed. */
		drv_i915_gfx_object_destroy(session, video->batch);
	}

	/* The session. */
	kern_free(video);
}

/* Finds the simulated state of a session in a submission's table, adding it the first time. */
static struct i915_video_simulation *
i915_video_simulated(
	struct i915_video_simulation *table,
	uint32_t *count,
	struct i915_render_session *session,
	uint64_t identity)
{
	struct i915_video_session *video;
	uint32_t index;

	/* A session met before in the submission. */
	for (index = 0U; index < *count; index++) {
		if (table[index].identity == identity)
			return &table[index];
	}

	/* An unknown session, or one past the table's room, is refused. */
	video = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (video == NULL || *count >= I915_VIDEO_SUBMIT_SESSIONS)
		return NULL;

	/* Starts from the session's own state. */
	table[*count].identity = identity;
	table[*count].session = video;
	kern_memcpy(table[*count].slots, video->slots, sizeof(video->slots));
	table[*count].reset_done = video->reset_done;
	*count = *count + 1U;

	/* Succeeded: reports the session's simulated state. */
	return &table[*count - 1U];
}

/*
 * Walks the operations of a video submission.
 *
 * Without `apply` it is the simulation: every rule of the API is checked
 * and the slots move on copies; the first broken rule returns EBADMSG.  With
 * `apply` it runs: the same walk on the sessions themselves, where every
 * decode is checked against what the decoder takes and skipped, or written
 * and run; a batch that cannot be made returns ENOMEM, a run that fails on
 * the GPU its error.  A result status query (ws083-p008) is begun and ended
 * within a coding scope, one at a time; running, its end writes COMPLETE,
 * or ERROR when a decode within it was skipped.
 */
static int
i915_video_simulate(
	struct i915_render_session *session,
	const struct i915_gfx_op *const *lists,
	const uint32_t *counts,
	uint32_t list_count,
	int apply)
{
	struct i915_video_simulation table[I915_VIDEO_SUBMIT_SESSIONS];
	struct i915_video_mfx_decode decode;
	struct i915_video_simulation *state;
	const struct i915_video_command *begin;
	const struct i915_video_command *command;
	const struct i915_gfx_op *op;
	struct i915_gfx_query_pool *query_pool;
	const char *reason;
	uint32_t simulated;
	uint32_t list;
	uint32_t index;
	uint32_t slot;
	uint32_t query_index;
	uint32_t query_decodes;
	int32_t query_status;
	int query_active;
	int status_pool;
	int in_range;
	int error;

	/* Starts with no session met. */
	simulated = 0U;
	kern_memset(table, 0, sizeof(table));

	/* Walks every command buffer; a coding scope starts and ends within one. */
	for (list = 0U; list < list_count; list++) {
		begin = NULL;
		state = NULL;
		query_pool = NULL;
		query_index = 0U;
		query_status = I915_VIDEO_STATUS_COMPLETE;
		query_decodes = 0U;
		query_active = 0;
		for (index = 0U; index < counts[list]; index++) {
			op = &lists[list][index];

			/* Only video commands and the queries run on the video family. */
			if (op->kind == I915_GFX_OP_QUERY_RESET) {
				/* A reset belongs outside a coding scope, and within its pool. */
				if (begin != NULL) {
					kern_logf("i915: video: submission refused: a query reset within a coding scope\n");
					return EBADMSG;
				}
				in_range = drv_i915_gfx_query_in_range(op->u.query.pool, op->u.query.first, op->u.query.count);
				if (!in_range) {
					kern_logf("i915: video: submission refused: a query reset past its pool\n");
					return EBADMSG;
				}

				/* An occlusion pool's reset runs on the render engine, a result status pool's on the CPU now. */
				if (apply) {
					error = drv_i915_gfx_query_execute(session, op);
					if (error != 0)
						return error;
				}
				continue;
			}
			if (op->kind == I915_GFX_OP_QUERY_BEGIN) {
				/* A result status query of its pool, begun in a scope, while no other is (ws083-p008). */
				status_pool = drv_i915_gfx_query_status_pool(op->u.query.pool, op->u.query.first);
				if (begin == NULL ||
				    !status_pool ||
				    query_active) {
					kern_logf("i915: video: submission refused: a query begun outside a coding scope, of another type, or within another\n");
					return EBADMSG;
				}

				/* The query takes what the decode within it does, COMPLETE unless it is skipped. */
				query_pool = op->u.query.pool;
				query_index = op->u.query.first;
				query_status = I915_VIDEO_STATUS_COMPLETE;
				query_decodes = 0U;
				query_active = 1;
				continue;
			}
			if (op->kind == I915_GFX_OP_QUERY_END) {
				/* The end of the query begun in this scope. */
				if (begin == NULL ||
				    !query_active ||
				    op->u.query.pool != query_pool ||
				    op->u.query.first != query_index) {
					kern_logf("i915: video: submission refused: a query ended that was not begun in the coding scope\n");
					return EBADMSG;
				}

				/*
				 * Running, the decode within it has run on VCS0 by now (each
				 * decode's batch runs and is waited for before the next
				 * operation, i915_video_run), and its status is written on the
				 * CPU.  A walk that batched several decodes into one run would
				 * have to run the batch here first.
				 */
				if (apply)
					drv_i915_gfx_query_status_end(query_pool, query_index, query_status);

				/* No query is active in the scope from here. */
				query_active = 0;
				continue;
			}
			if (op->kind != I915_GFX_OP_VIDEO_BEGIN &&
			    op->kind != I915_GFX_OP_VIDEO_CONTROL &&
			    op->kind != I915_GFX_OP_VIDEO_DECODE &&
			    op->kind != I915_GFX_OP_VIDEO_END) {
				kern_logf("i915: video: submission refused: operation kind %u on the video family\n", (unsigned)op->kind);
				return EBADMSG;
			}

			/* A command that carried more than it could hold breaks the API's limits. */
			command = op->u.video;
			if (command == NULL || command->oversize) {
				kern_logf("i915: video: submission refused: too many slots or references\n");
				return EBADMSG;
			}

			/* Moves the scope and the slots by the command. */
			switch (op->kind) {
			case I915_GFX_OP_VIDEO_BEGIN:
				/* A begin inside a scope breaks the rules; a new scope starts. */
				if (begin != NULL)
					return EBADMSG;
				state = i915_video_simulated(table, &simulated, session, command->session);
				if (state == NULL) {
					kern_logf("i915: video: submission refused: unknown video session\n");
					return EBADMSG;
				}
				error = i915_video_simulate_begin(session, command, state);
				if (error != 0)
					return error;
				begin = command;

				/* Running, the session's slots are the simulated ones. */
				if (apply)
					kern_memcpy(state->session->slots, state->slots, sizeof(state->slots));
				break;
			case I915_GFX_OP_VIDEO_CONTROL:
				/* A control outside a scope breaks the rules; a reset makes every slot inactive. */
				if (begin == NULL)
					return EBADMSG;
				if ((command->flags & I915_VIDEO_CONTROL_RESET) != 0U) {
					for (slot = 0U; slot < I915_VIDEO_MAX_DPB_SLOTS; slot++)
						state->slots[slot].active = 0;
					state->reset_done = 1;
				}

				/* Running, the session takes the reset. */
				if (apply) {
					kern_memcpy(state->session->slots, state->slots, sizeof(state->slots));
					state->session->reset_done = state->reset_done;
				}
				break;
			case I915_GFX_OP_VIDEO_DECODE:
				/* A decode outside a scope, a second one within a query, or one breaking the slot rules, refuses the submission. */
				if (begin == NULL)
					return EBADMSG;
				if (query_active && query_decodes != 0U) {
					kern_logf("i915: video: submission refused: a second decode within one query\n");
					return EBADMSG;
				}
				if (query_active)
					query_decodes++;
				error = i915_video_simulate_decode(command, begin, state);
				if (error != 0)
					return error;

				/*
				 * Running, the decode is checked against what the decoder
				 * takes and written when it passes; a skipped decode still
				 * moves the slots.
				 */
				if (apply) {
					reason = i915_video_check(session, command, begin, state->session, &decode);
					if (reason != NULL) {
						i915_video_skip(reason);
						query_status = I915_VIDEO_STATUS_ERROR;
					} else {
						i915_video_resolve(session, command, begin, state->session, &decode);
						error = i915_video_write(session, state->session, &decode);
						if (error != 0)
							return error;
					}
					kern_memcpy(state->session->slots, state->slots, sizeof(state->slots));

					/* Runs what the decode wrote on VCS0; a hang loses the submission. */
					error = i915_video_run(session, state->session);
					if (error != 0)
						return error;
				}
				break;
			default:
				/* An end closes the scope, and the queries begun in it must have ended. */
				if (begin == NULL)
					return EBADMSG;
				if (query_active) {
					kern_logf("i915: video: submission refused: a query not ended in its coding scope\n");
					return EBADMSG;
				}
				begin = NULL;
				state = NULL;
				break;
			}
		}

		/* A scope left open at the end of a command buffer breaks the rules. */
		if (begin != NULL) {
			kern_logf("i915: video: submission refused: a coding scope is not ended\n");
			return EBADMSG;
		}
	}

	/* Succeeded: the submission follows the rules, or has run. */
	return 0;
}

/*
 * Simulates a begin: the session must be bound and its parameters its own;
 * the slots it binds must be in range and given once, and a slot that holds
 * a picture must be bound to that picture; a slot without a picture is
 * deactivated.
 */
static int
i915_video_simulate_begin(
	struct i915_render_session *session,
	const struct i915_video_command *command,
	struct i915_video_simulation *state)
{
	const struct i915_video_parameters *parameters;
	const struct i915_video_slot_info *slot;
	uint32_t index;
	uint32_t other;
	int bound;

	/* Every binding of the session must hold memory that still has storage. */
	bound = i915_video_bound(session, state->session);
	if (!bound) {
		kern_logf("i915: video: submission refused: the video session's memory is not bound\n");
		return EBADMSG;
	}

	/* Parameters, when given, must be of the session. */
	if (command->parameters != 0U) {
		parameters = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, command->parameters);
		if (parameters == NULL || parameters->session != command->session) {
			kern_logf("i915: video: submission refused: parameters not of the video session\n");
			return EBADMSG;
		}
	}

	/* Checks and applies every slot the begin names. */
	for (index = 0U; index < command->slot_count; index++) {
		slot = &command->slots[index];

		/* A slot index past the session's, or below the no-slot -1, breaks the rules. */
		if (slot->index < -1 || slot->index >= (int32_t)state->session->max_dpb_slots)
			return EBADMSG;

		/* A picture without a slot is a setup picture; it moves no slot. */
		if (slot->index < 0) {
			if (!slot->has_picture)
				return EBADMSG;
			continue;
		}

		/* A slot index given twice breaks the rules. */
		for (other = 0U; other < index; other++) {
			if (command->slots[other].index == slot->index)
				return EBADMSG;
		}

		/* A slot without a picture is deactivated. */
		if (!slot->has_picture) {
			state->slots[slot->index].active = 0;
			continue;
		}

		/* An active slot must be bound to the picture it holds. */
		if (state->slots[slot->index].active &&
		    (state->slots[slot->index].view != slot->picture.view ||
		     state->slots[slot->index].layer != slot->picture.layer)) {
			kern_logf("i915: video: submission refused: slot %d bound to a picture it does not hold\n", (int)slot->index);
			return EBADMSG;
		}
	}

	/* Succeeded: the begin follows the rules. */
	return 0;
}

/*
 * Simulates a decode: the session must have been reset; every reference
 * must be an active slot the begin bound, given once; a setup slot must be
 * a slot the begin bound to the decode's output.  After the decode the
 * setup slot holds the output when the picture is a reference, and is
 * inactive otherwise (design §6.5).
 */
static int
i915_video_simulate_decode(
	const struct i915_video_command *command,
	const struct i915_video_command *begin,
	struct i915_video_simulation *state)
{
	const struct i915_video_slot_info *reference;
	const struct i915_video_slot_info *bound;
	uint32_t index;
	uint32_t other;
	int found;

	/* A decode before the session's first reset breaks the rules. */
	if (!state->reset_done) {
		kern_logf("i915: video: submission refused: decode before the video session was reset\n");
		return EBADMSG;
	}

	/* The references must not be more than the session takes. */
	if (command->reference_count > state->session->max_references)
		return EBADMSG;

	/* Every reference is an active slot bound in the begin, given once. */
	for (index = 0U; index < command->reference_count; index++) {
		reference = &command->references[index];
		if (reference->index < 0 || reference->index >= (int32_t)state->session->max_dpb_slots)
			return EBADMSG;
		if (!state->slots[reference->index].active)
			return EBADMSG;
		bound = i915_video_begin_slot(begin, reference->index);
		if (bound == NULL || !bound->has_picture)
			return EBADMSG;
		for (other = 0U; other < index; other++) {
			if (command->references[other].index == reference->index)
				return EBADMSG;
		}
	}

	/* A decode without a setup slot moves no slot. */
	if (!command->has_setup)
		return 0;

	/* The setup slot is a slot of the session whose picture is the output and was bound in the begin. */
	if (command->setup.index < 0 || command->setup.index >= (int32_t)state->session->max_dpb_slots)
		return EBADMSG;
	if (!command->setup.has_picture || !i915_video_same_picture(&command->setup.picture, &command->destination))
		return EBADMSG;
	found = i915_video_begin_picture(begin, &command->destination);
	if (!found)
		return EBADMSG;

	/* A reference picture fills the setup slot; any other leaves it inactive. */
	state->slots[command->setup.index].active = 0;
	if (command->has_picture && (command->picture_flags & I915_VIDEO_PICTURE_IS_REFERENCE) != 0U) {
		state->slots[command->setup.index].active = 1;
		state->slots[command->setup.index].view = command->destination.view;
		state->slots[command->setup.index].layer = command->destination.layer;
	}

	/* Succeeded: the decode follows the rules. */
	return 0;
}

/* Reports whether every binding of a session holds memory that still has storage. */
static int
i915_video_bound(
	struct i915_render_session *session,
	const struct i915_video_session *video)
{
	struct i915_gfx_memory *memory;
	uint32_t index;

	/* Every binding was bound. */
	if (video->bound_mask != (1U << video->bind_count) - 1U)
		return 0;

	/* Each one's memory still exists and has its storage. */
	for (index = 0U; index < video->bind_count; index++) {
		memory = drv_i915_object_lookup(session, I915_VK_OBJ_MEMORY, video->binds[index].memory);
		if (memory == NULL || memory->object == NULL)
			return 0;
	}

	/* Succeeded: the session is bound. */
	return 1;
}

/* Finds the slot of an index a begin bound, or NULL. */
static const struct i915_video_slot_info *
i915_video_begin_slot(
	const struct i915_video_command *begin,
	int32_t index)
{
	uint32_t item;

	/* The begin's slots. */
	for (item = 0U; item < begin->slot_count; item++) {
		if (begin->slots[item].index == index)
			return &begin->slots[item];
	}

	/* The begin did not bind the slot. */
	return NULL;
}

/* Reports whether a begin bound a picture, with or without a slot. */
static int
i915_video_begin_picture(
	const struct i915_video_command *begin,
	const struct i915_video_resource *picture)
{
	uint32_t item;

	/* The begin's slots that carry a picture. */
	for (item = 0U; item < begin->slot_count; item++) {
		if (begin->slots[item].has_picture && i915_video_same_picture(&begin->slots[item].picture, picture))
			return 1;
	}

	/* The begin did not bind the picture. */
	return 0;
}

/* Reports whether two picture resources name the same view and layer. */
static int
i915_video_same_picture(
	const struct i915_video_resource *first,
	const struct i915_video_resource *second)
{
	/* The view and the layer decide. */
	if (first->view != second->view || first->layer != second->layer)
		return 0;

	/* Succeeded: the same picture. */
	return 1;
}

/*
 * Checks a decode against what the decoder takes (design §6.6, D17):
 * returns NULL when it can be decoded, or why it is skipped.  A decode that
 * passes has its sets, its slices and its bitstream in `decode`.
 */
static const char *
i915_video_check(
	struct i915_render_session *session,
	const struct i915_video_command *command,
	const struct i915_video_command *begin,
	struct i915_video_session *video,
	struct i915_video_mfx_decode *decode)
{
	const struct i915_video_parameters *parameters;
	const struct i915_video_sps *sps;
	const struct i915_video_pps *pps;
	const struct i915_gfx_image *destination;
	const struct i915_gfx_image *reference;
	const struct i915_video_slot_info *bound;
	const char *reason;
	uint32_t index;

	/* 1: the picture's sets must be in the scope's parameters. */
	parameters = NULL;
	if (begin->parameters != 0U)
		parameters = drv_i915_object_lookup(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, begin->parameters);
	if (parameters == NULL || !command->has_picture)
		return "no parameters or picture information";
	pps = i915_video_find_pps(parameters, command->sps_id, command->pps_id);
	if (pps == NULL)
		return "picture parameter set not found";
	if (pps->sps_id >= I915_VIDEO_SPS_IDS || parameters->sps[pps->sps_id] == NULL)
		return "sequence parameter set not found";
	sps = parameters->sps[pps->sps_id];

	/* 2 and 4: the sets' values must be ones the decoder takes. */
	reason = drv_i915_video_mfx_check_sets(sps, pps);
	if (reason != NULL)
		return reason;

	/* 3: the picture must fit the session. */
	if (sps->pic_width_in_mbs_minus1 + 1U > video->width_mbs ||
	    sps->pic_height_in_map_units_minus1 + 1U > video->height_mbs)
		return "picture larger than the video session";

	/* 5, 8 and 9: the slices must lie in the bitstream range, each with its start code. */
	reason = i915_video_check_slices(session, command, video, decode);
	if (reason != NULL)
		return reason;

	/* 3, 6 and 7: the output must hold the picture; every reference must be laid out as it. */
	destination = i915_video_view_image(session, command->destination.view);
	reason = i915_video_check_picture(session, &command->destination, sps, NULL);
	if (reason != NULL)
		return reason;
	for (index = 0U; index < command->reference_count; index++) {
		bound = i915_video_begin_slot(begin, command->references[index].index);
		if (bound == NULL)
			return "reference not bound";
		reference = i915_video_view_image(session, bound->picture.view);
		if (reference == NULL || destination == NULL)
			return "reference picture without an image";
		reason = i915_video_check_picture(session, &bound->picture, sps, destination);
		if (reason != NULL)
			return reason;
	}

	/* 7: the session's bindings must be page-aligned addresses. */
	reason = i915_video_check_binds(session, video);
	if (reason != NULL)
		return reason;

	/* Keeps the sets the picture decodes with. */
	decode->sps = sps;
	decode->pps = pps;

	/* Succeeded: the decoder takes the picture. */
	return NULL;
}

/*
 * Checks a decode's slices (design §6.6, items 5, 8 and 9): one to 256 of
 * them, strictly increasing offsets at least four bytes apart, inside the
 * range, which is inside the bitstream buffer, and each starting with a
 * start code (three or four bytes) in its first four bytes.  Each slice's
 * bounds go into the session's scratch, and the bitstream's addresses into
 * `decode`: the page the range starts in, the bytes into that page, and
 * the end of the buffer's bound range rounded up to its page (the MFX's
 * upper bound is a 4 KiB-aligned address, ws083 R-S1; the memory is bound
 * in whole pages, so the rest of that page is the buffer's memory).
 */
static const char *
i915_video_check_slices(
	struct i915_render_session *session,
	const struct i915_video_command *command,
	struct i915_video_session *video,
	struct i915_video_mfx_decode *decode)
{
	const struct i915_gfx_buffer *buffer;
	const uint8_t *bytes;
	uint64_t end;
	uint64_t start;
	uint64_t buffer_va;
	uint32_t index;
	uint32_t next;
	uint32_t at;
	int found;

	/* One to 256 slices. */
	if (command->slice_count == 0U)
		return "no slice";
	if (command->slice_count > I915_VIDEO_MAX_SLICES)
		return "more than 256 slices";

	/* The range must lie in a bound buffer. */
	buffer = drv_i915_object_lookup(session, I915_VK_OBJ_BUFFER, command->buffer);
	if (buffer == NULL || buffer->memory == NULL)
		return "bitstream buffer not bound";
	if (command->offset > buffer->size || command->range > buffer->size - command->offset)
		return "bitstream range past the buffer";
	if ((command->offset % 32U) != 0U)
		return "bitstream offset not a multiple of 32";

	/* The CPU view of the range, to find the start codes. */
	bytes = drv_i915_gfx_memory_cpu(buffer->memory, buffer->offset + command->offset, command->range);
	if (bytes == NULL)
		return "bitstream buffer has no storage";

	/* Each slice: increasing offsets at least 4 bytes apart, in the range, with a start code. */
	for (index = 0U; index < command->slice_count; index++) {
		/* Where the next slice starts, or the range's end for the last one. */
		end = command->range;
		if (index + 1U < command->slice_count)
			end = command->slices[index + 1U];
		if ((uint64_t)command->slices[index] + 4U > end)
			return "slice shorter than 4 bytes or out of order";

		/* A start code, 00 00 01, in the slice's first four bytes; the slice begins after it. */
		found = 0;
		for (at = 0U; at + 2U < 4U && !found; at++) {
			next = command->slices[index] + at;
			if (bytes[next] == 0U && bytes[next + 1U] == 0U && bytes[next + 2U] == 1U) {
				found = 1;
				video->slice_starts[index] = next + 3U;
				video->slice_ends[index] = (uint32_t)end;
			}
		}
		if (!found)
			return "slice without a start code";
	}

	/* The GPU addresses of the range and of the buffer's end. */
	buffer_va = drv_i915_gfx_memory_va(buffer->memory, buffer->offset);
	if (buffer_va == 0U)
		return "bitstream buffer without an address";
	start = buffer_va + command->offset;
	decode->bitstream_base = start & ~(uint64_t)(I915_VIDEO_ALIGN - 1U);
	decode->skew = (uint32_t)(start - decode->bitstream_base);
	decode->bitstream_end = (buffer_va + buffer->size + I915_VIDEO_ALIGN - 1U) & ~(uint64_t)(I915_VIDEO_ALIGN - 1U);

	/* The slices, from the session's scratch. */
	decode->slice_count = command->slice_count;
	decode->slice_starts = video->slice_starts;
	decode->slice_ends = video->slice_ends;

	/* Succeeded: the slices lie in the bitstream. */
	return NULL;
}

/*
 * Checks one picture of a decode (design §6.6, items 3, 6 and 7): its
 * view's image must be NV12, large enough for the sequence, at a coded
 * offset and layer of zero, page-aligned, and, for a reference, laid out as
 * the output is.
 */
static const char *
i915_video_check_picture(
	struct i915_render_session *session,
	const struct i915_video_resource *picture,
	const struct i915_video_sps *sps,
	const struct i915_gfx_image *reference)
{
	const struct i915_gfx_image *image;
	uint64_t va;

	/* The picture's image must exist, be an NV12 picture of the decoder and bound. */
	image = i915_video_view_image(session, picture->view);
	if (image == NULL || image->memory == NULL)
		return "picture without a bound image";
	if (image->format != I915_VIDEO_FORMAT_NV12 || image->planar == 0U)
		return "picture not NV12";

	/* The whole picture from the image's origin. */
	if (picture->x != 0 || picture->y != 0 || picture->layer != 0U)
		return "coded offset or layer not zero";
	if (sps->pic_width_in_mbs_minus1 + 1U > i915_video_mbs(image->width) ||
	    sps->pic_height_in_map_units_minus1 + 1U > i915_video_mbs(image->height))
		return "picture larger than its image";

	/* A reference must be laid out as the output: same format, pitch and CbCr plane row. */
	if (reference != NULL &&
	    (image->format != reference->format ||
	     image->pitch != reference->pitch ||
	     image->chroma_rows != reference->chroma_rows))
		return "reference laid out unlike the output";

	/* The image's address must be page-aligned. */
	va = drv_i915_gfx_memory_va(image->memory, image->offset);
	if (va == 0U || (va % I915_VIDEO_ALIGN) != 0U)
		return "picture address not page-aligned";

	/* Succeeded: the decoder takes the picture. */
	return NULL;
}

/* Checks that every binding of a session has a page-aligned GPU address (design §6.6, item 7). */
static const char *
i915_video_check_binds(
	struct i915_render_session *session,
	const struct i915_video_session *video)
{
	struct i915_gfx_memory *memory;
	uint64_t va;
	uint32_t index;

	/* Each binding's address. */
	for (index = 0U; index < video->bind_count; index++) {
		memory = drv_i915_object_lookup(session, I915_VK_OBJ_MEMORY, video->binds[index].memory);
		va = drv_i915_gfx_memory_va(memory, video->binds[index].offset);
		if (va == 0U || (va % I915_VIDEO_ALIGN) != 0U)
			return "session memory address not page-aligned";
	}

	/* Succeeded: every binding is addressable. */
	return NULL;
}

/* Finds the image of a view identity, or NULL. */
static const struct i915_gfx_image *
i915_video_view_image(
	struct i915_render_session *session,
	uint64_t view)
{
	const struct i915_gfx_view *found;

	/* The view, then its image. */
	found = drv_i915_object_lookup(session, I915_VK_OBJ_IMAGE_VIEW, view);
	if (found == NULL)
		return NULL;

	/* Succeeded: reports the view's image. */
	return found->image;
}

/* Logs a skipped decode, the first 32 of them. */
static void
i915_video_skip(
	const char *reason)
{
	static unsigned said;

	/* Only the first ones. */
	if (said >= 32U)
		return;
	said++;
	kern_logf("i915: video: skip decode: %s\n", reason);
}

/* The macroblocks that cover a number of pixels. */
static uint32_t
i915_video_mbs(
	uint32_t pixels)
{
	/* Rounds up to whole macroblocks. */
	return (pixels + I915_VIDEO_MB - 1U) / I915_VIDEO_MB;
}

/*
 * Runs what a video session's batch holds on the video decode engine: the
 * decode just written, or nothing after a skipped one.
 *
 * A run that hung or failed has already been handled by the worker, which
 * retained the hardware context and reset the video engine (or stopped
 * video for good when the reset failed, ws083-p007); here the session is
 * quarantined, so its address space, objects and batch stay for the
 * checked reset (design §6.1).
 */
static int
i915_video_run(
	struct i915_render_session *session,
	struct i915_video_session *video)
{
	struct i915_device *device;
	unsigned long irq;
	int error;

	/* An empty batch has nothing to run. */
	if (video->batch == NULL || video->cursor.count == 0U)
		return 0;

	/* Runs the batch to its end in the session's VCS0 context, then empties it. */
	error = drv_i915_gfx_batch_run(session, &video->cursor, video->batch->va, I915_ENGINE_VCS0);
	video->cursor.count = 0U;
	video->cursor.overflow = 0;

	/*
	 * A decode that hung or failed quarantines the session; the device's
	 * video was reset or stopped.  A video engine stopped before this
	 * request (ECANCELED: nothing ran) loses the submission without
	 * quarantining a session that did not hang it (ws083 R-S2).
	 */
	if (error == ECANCELED) {
		kern_logf("i915: video: the video engine is stopped; the decode did not run\n");
		return EIO;
	}
	if (error == ETIMEDOUT || error == EIO) {
		device = session->vk->i915;
		irq = spin_lock_irqsave(&device->irq_lock);

		session->gpu->quarantined = 1U;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		kern_logf("i915: video: decode failed on VCS0 (error %d): session quarantined\n", error);
		return EIO;
	}

	/* Reports another failure of the run. */
	if (error != 0)
		return error;

	/* Succeeded: the decodes ran. */
	return 0;
}

/*
 * Resolves a checked decode into what the MFX commands are built from: its
 * picture information, the output's address and layout, the session's row
 * stores, the references' pictures and motion vector buffers, and the
 * motion vector buffer the picture writes (its setup slot's when the
 * picture is a reference, the spare one otherwise).  Every address was
 * found to exist by the checks before.
 */
static void
i915_video_resolve(
	struct i915_render_session *session,
	const struct i915_video_command *command,
	const struct i915_video_command *begin,
	const struct i915_video_session *video,
	struct i915_video_mfx_decode *decode)
{
	const struct i915_gfx_image *destination;
	const struct i915_video_slot_info *slot;
	const struct i915_video_slot_info *bound;
	struct i915_video_mfx_reference *reference;
	uint32_t motion;
	uint32_t index;

	/* The picture information. */
	decode->picture_flags = command->picture_flags;
	decode->frame_num = command->frame_num;
	decode->poc[0] = command->poc[0];
	decode->poc[1] = command->poc[1];

	/* The output: its address, extent, pitch and the row its CbCr plane starts at. */
	destination = i915_video_view_image(session, command->destination.view);
	decode->destination = i915_video_picture_address(session, command->destination.view);
	decode->width = destination->width;
	decode->height = destination->height;
	decode->pitch = destination->pitch;
	decode->chroma_rows = destination->chroma_rows;

	/* The four row stores, the session's first bindings. */
	for (index = 0U; index < I915_VIDEO_MFX_ROW_STORES; index++)
		decode->row_stores[index] = i915_video_bind_address(session, video, index);

	/* The motion vectors the picture writes: its setup slot's for a reference picture, else the spare buffer. */
	motion = I915_VIDEO_ROW_STORES + video->max_dpb_slots;
	if (command->has_setup && (command->picture_flags & I915_VIDEO_PICTURE_IS_REFERENCE) != 0U)
		motion = I915_VIDEO_ROW_STORES + (uint32_t)command->setup.index;
	decode->motion_write = i915_video_bind_address(session, video, motion);

	/* The references in the decode's order: their information, their pictures as the begin bound them, their slots' motion vectors. */
	decode->reference_count = command->reference_count;
	for (index = 0U; index < command->reference_count; index++) {
		slot = &command->references[index];
		bound = i915_video_begin_slot(begin, slot->index);
		reference = &decode->references[index];
		reference->slot_index = slot->index;
		reference->flags = slot->reference_flags;
		reference->frame_num = slot->frame_num;
		reference->poc[0] = slot->poc[0];
		reference->poc[1] = slot->poc[1];
		reference->picture = i915_video_picture_address(session, bound->picture.view);
		reference->motion = i915_video_bind_address(session, video, I915_VIDEO_ROW_STORES + (uint32_t)slot->index);
	}

	/* Every buffer is uncached, as the executor's surfaces are. */
	decode->mocs = GEN12_MOCS(I915_MOCS_UNCACHED_INDEX);
}

/* Reports the GPU address of one binding of a session's memory; 0 when its memory has none. */
static uint64_t
i915_video_bind_address(
	struct i915_render_session *session,
	const struct i915_video_session *video,
	uint32_t index)
{
	struct i915_gfx_memory *memory;
	uint64_t address;

	/* The binding's memory, looked up by its identity, then the address at the binding's offset. */
	memory = drv_i915_object_lookup(session, I915_VK_OBJ_MEMORY, video->binds[index].memory);
	address = drv_i915_gfx_memory_va(memory, video->binds[index].offset);

	/* Succeeded: reports the address. */
	return address;
}

/* Reports the GPU address of the picture of a view (its image's start, the Y plane); 0 without one. */
static uint64_t
i915_video_picture_address(
	struct i915_render_session *session,
	uint64_t view)
{
	const struct i915_gfx_image *image;
	uint64_t address;

	/* The view's image. */
	image = i915_video_view_image(session, view);
	if (image == NULL)
		return 0U;

	/* The image's address in its memory. */
	address = drv_i915_gfx_memory_va(image->memory, image->offset);

	/* Succeeded: reports the address. */
	return address;
}

/*
 * Writes the MFX commands of one resolved decode into the session's batch,
 * made on the session's first decode.  Returns ENOMEM when the batch cannot
 * be made, ENOSPC when the decode does not fit it (never with the limits
 * the checks hold decodes to).
 */
static int
i915_video_write(
	struct i915_render_session *session,
	struct i915_video_session *video,
	const struct i915_video_mfx_decode *decode)
{
	int error;

	/* Makes the batch on the session's first decode. */
	if (video->batch == NULL) {
		error = drv_i915_gfx_object_create(session, I915_VIDEO_BATCH_BYTES, &video->batch);
		if (error != 0) {
			video->batch = NULL;
			kern_logf("i915: video: no batch for a video session: error %d\n", error);
			return ENOMEM;
		}
		video->cursor.cmds = video->batch->address;
		video->cursor.capacity = I915_VIDEO_BATCH_BYTES / 4U;
	}

	/* The decode's commands from the batch's start (each decode runs before the next is written). */
	video->cursor.count = 0U;
	video->cursor.overflow = 0;
	drv_i915_video_mfx_build(&video->cursor, decode);

	/* A decode that did not fit, with the batch's end after it, is not run. */
	if (video->cursor.overflow != 0 || video->cursor.count + 2U > video->cursor.capacity) {
		kern_logf("i915: video: decode of %u dwords does not fit the batch\n", video->cursor.count);
		video->cursor.count = 0U;
		video->cursor.overflow = 0;
		return ENOSPC;
	}

	/* Succeeded: the batch holds the decode. */
	return 0;
}
