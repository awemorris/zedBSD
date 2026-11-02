/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Vulkan Video H.265 decode on the Raspberry Pi 5 (see vulkan-video.h),
 * with the SoC's HEVC decoder block doing the decode.
 *
 * The wire is libvulkan's (userland/desktop/libvulkan/video.c), the same
 * the nvrtx executor reads for H.265 (nvrtx/vk/vk-video.c); the readers
 * here follow that file.  The block is stateless: the executor reads every
 * slice segment header itself (vulkan-video-hevc-slice.c) and gives the
 * block a command list per picture.
 *
 * Each DPB slot of a session has a picture of its own in the session's
 * memory (the bindings the application binds): the block's SAND output of
 * the last decode that set the slot up, with its motion vectors, which
 * later decodes read as references.  After a decode the V3D's texture
 * formatting unit writes the slot's picture into the output image in the
 * image's NV12 layout, so the image is what Vulkan's DPB_AND_OUTPUT_COINCIDE
 * promises and the CPU never touches a pixel.  A decode without a setup
 * slot uses the session's spare picture.
 *
 * A decode runs in the native job like a CPU copy, a step at a time: the
 * first step reads the headers, builds the command list and starts phase
 * 1; each step looks at the block with the controller's window open for a
 * short while and returns EINPROGRESS until phase 2 is done; the last
 * converts the picture and completes the status query.  One decode is in
 * the block at a time; a decode of another session waits for it.  A
 * bitstream the block or the executor cannot take marks the status ERROR
 * and leaves the image as it was; it does not fail the submission.
 */

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/bcm2711/vk/vulkan-video.h"
#include "drivers/gpu/bcm2711/vk/vulkan-video-hevc.h"
#include "drivers/gpu/bcm2711/vk/vulkan-command.h"
#include "drivers/gpu/bcm2711/vk/vulkan-input.h"
#include "drivers/gpu/bcm2711/vk/vulkan-resource.h"
#include "drivers/gpu/bcm2711/vk/vulkan-memory.h"
#include "drivers/gpu/bcm2711/v3d-memory.h"
#include "drivers/gpu/bcm2711/v3d-job.h"
#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/hevc-decoder.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/render-worker.h"

/* The decode limits the executor reports and holds sessions to: one picture of each slot and a spare, at most 4096 a side. */
#define VIDEO_MAX_DPB_SLOTS		BCM2712_HEVC_SLOTS
#define VIDEO_MAX_REFERENCES		BCM2712_HEVC_REFERENCES
#define VIDEO_MAX_SIDE			BCM2712_HEVC_MAX_SIDE
#define VIDEO_MIN_SIDE			32U
#define VIDEO_GRANULARITY		16U
#define VIDEO_SPARE			VIDEO_MAX_DPB_SLOTS
#define VIDEO_PICTURES			(VIDEO_MAX_DPB_SLOTS + 1U)

/* The bitstream's offset and size alignment reported. */
#define VIDEO_BITSTREAM_ALIGN		64U

/* The H.265 header the decoder was written against: VK_STD_vulkan_video_codec_h265_decode 1.0.0. */
#define VIDEO_STD_NAME			"VK_STD_vulkan_video_codec_h265_decode"
#define VIDEO_STD_VERSION		((1U << 22) | (0U << 12) | 0U)

/* The H.265 profile taken (Main) and the level reported (STD_VIDEO_H265_LEVEL_IDC_5_1). */
#define VIDEO_PROFILE_MAIN		1U
#define VIDEO_LEVEL_5_1			8U

/* The parameter set id ranges, and the most sets of each kind an object keeps. */
#define VIDEO_VPS_IDS			16U
#define VIDEO_SPS_IDS			16U
#define VIDEO_PPS_IDS			64U

/* The array sizes of the StdVideo H.265 structures the wire carries. */
#define VIDEO_SUB_LAYERS		7U
#define VIDEO_CPB_COUNT			32U
#define VIDEO_SHORT_TERM_SETS		64U
#define VIDEO_LONG_TERM_PICS		32U
#define VIDEO_SET_PICTURES		16U
#define VIDEO_PALETTE_ENTRIES		384U
#define VIDEO_CHROMA_OFFSETS		6U

/* The deepest chain a structure may carry. */
#define VIDEO_MAX_CHAIN			8U

/* The command list's capacity (pairs), the most bytes of a segment's header read, and a page. */
#define VIDEO_COMMAND_PAIRS		(128U * 1024U)
#define VIDEO_HEADER_BYTES		(64U * 1024U)
#define VIDEO_PAGE			4096U

/* The highest bus address the decoder's private buffers are given (it reaches 2^38). */
#define VIDEO_BUFFER_LIMIT		0xffffffffULL

/* How long a decode may take in the block over all its steps, and how long one step looks with the window open. */
#define VIDEO_DECODE_MS			500U
#define VIDEO_POLL_US			2000U

/* How often a list that ran out is grown for one picture at most. */
#define VIDEO_GROWS			4U

/* The lines a session writes about skipped pictures, at most. */
#define VIDEO_SKIP_LINES		8U

/* The most queries of a result status pool, and the reply's limit. */
#define VIDEO_QUERY_MAX			64U
#define VIDEO_QUERY_REPLY_MAX		4096U

/* The structure types the video wire carries (Vulkan 1.3 registry values). */
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
#define VIDEO_TYPE_H265_CAPABILITIES		1000187000U
#define VIDEO_TYPE_H265_PARAMETERS_CREATE	1000187001U
#define VIDEO_TYPE_H265_PARAMETERS_ADD		1000187002U
#define VIDEO_TYPE_H265_PROFILE			1000187003U
#define VIDEO_TYPE_H265_PICTURE			1000187004U
#define VIDEO_TYPE_H265_DPB_SLOT		1000187005U

/* The flags and enumerants of the video API answered or checked. */
#define VIDEO_OP_DECODE_H265			0x2U
#define VIDEO_CHROMA_420			0x2U
#define VIDEO_DEPTH_8				0x1U
#define VIDEO_SEPARATE_REFERENCE_IMAGES		0x2U
#define VIDEO_DPB_AND_OUTPUT_COINCIDE		0x1U
#define VIDEO_CONTROL_RESET			0x1U

/* The query type of a result status pool, and its results. */
#define VIDEO_QUERY_RESULT_STATUS		1000023000U
#define VIDEO_STATUS_COMPLETE			1
#define VIDEO_STATUS_ERROR			(-1)
#define VIDEO_QUERY_RESULT_64			0x1U
#define VIDEO_QUERY_RESULT_AVAILABILITY		0x4U

/* The video results the executor replies (the pinned header's values). */
#define VIDEO_ERROR_IMAGE_USAGE			(-1000023000)
#define VIDEO_ERROR_OPERATION			(-1000023002)
#define VIDEO_ERROR_FORMAT			(-1000023003)
#define VIDEO_ERROR_CODEC			(-1000023004)
#define VIDEO_ERROR_STD_VERSION			(-1000023005)

/*
 * The V3D 7.1 texture formatting unit's words for a SAND input written as
 * raster: the input configuration (SAND input, 128-byte columns, no
 * swizzle, eight-bit samples, the texel type at bit 16) and the output
 * control (raster, its stride in texels at bit 16).
 *
 * Values referenced: Mesa src/broadcom/common/v3d_tfu.h (V3D71_TFU_ICFG_*
 * IFORMAT_SHIFT 23, FORMAT_SAND 1, SVTWID_SHIFT 26 and 128 = 0,
 * SVSWIZ_SHIFT 28 and NONE 15, ICFMT_SHIFT 8 and 8BIT 1, OTYPE_SHIFT 16;
 * V3D71_TFU_IOC_FORMAT_SHIFT 12 and RASTER 0, STRIDE_SHIFT 16), and
 * src/gallium/drivers/v3d/v3dx_tfu.c tfu_sand (the input stride is the
 * column's height in rows).
 */
#define TFU_SAND_CONFIGURATION		((15U << 28) | (0U << 26) | (1U << 23) | (1U << 8))
#define TFU_TYPE_SHIFT			16U
#define TFU_OUTPUT_RASTER		(0U << 12)
#define TFU_STRIDE_SHIFT		16U

/* One sequence parameter set an object holds; the sets are a list. */
struct video_sps_node {
	struct video_sps_node *next;
	struct bcm2712_hevc_sps sps;
};

/* One picture parameter set an object holds; the sets are a list. */
struct video_pps_node {
	struct video_pps_node *next;
	struct bcm2712_hevc_pps pps;
};

/*
 * One VkVideoSessionParametersKHR: the H.265 sets of one session, the
 * video sets as their ids (vps, one bit each), the capacity asked for,
 * and the update sequence.
 */
struct video_parameters {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t max_vps;
	uint32_t max_sps;
	uint32_t max_pps;
	uint32_t update_sequence;
	uint32_t vps;
	struct video_sps_node *sps;
	struct video_pps_node *pps;
};

/*
 * One picture of a session: the memory object bound to its binding
 * (retained), and once resolved at a decode, its bus address and V3D
 * address.  Each picture is a SAND luma plane, its chroma plane, and its
 * motion vector store, in that order.
 */
struct video_picture {
	struct bcm2711_vulkan_object *memory;
};

/*
 * A decode in the block: its record, its phase (1 or 2), when it started,
 * how often a list was grown, the picture it writes, and the phase 2
 * values.
 */
struct video_running {
	const struct video_command *command;
	struct video_session *video;
	uint32_t phase;
	uint64_t started;
	uint32_t grows;
	uint32_t picture;
	uint32_t command_count;
	struct bcm2712_hevc_phase2 phase2;
	struct bcm2712_hevc_phase2_buffers buffers;
};

/*
 * One VkVideoSessionKHR: its limits, its pictures' layout and bindings,
 * the phase 1 lists and command list (the executor's own, retained runs),
 * whether it holds the block's clock, and whether it broke (a decode
 * that never ended).  Published from its creation; the recorded begins
 * retain it.
 */
struct video_session {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t width;
	uint32_t height;
	uint32_t max_dpb_slots;
	uint32_t max_references;
	struct bcm2712_hevc_sand sand;
	uint32_t motion_stride;
	uint64_t motion_bytes;
	uint64_t picture_bytes;
	uint32_t pictures;
	struct video_picture picture[VIDEO_PICTURES];
	struct bcm2711_buffer *pu;
	struct bcm2711_buffer *coefficients;
	struct bcm2711_buffer *commands;
	bool holding;
	bool broken;
	unsigned skip_lines;
};

/* One result status query pool (see the Pi 4's vulkan-video.c). */
struct video_pool {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t count;
	int32_t status[VIDEO_QUERY_MAX];
	uint8_t available[VIDEO_QUERY_MAX];
};

/* A picture resource as recorded. */
struct video_resource {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t layer;
	uint64_t view;
};

/* A reference slot as recorded: its index and its H.265 information, if any. */
struct video_slot {
	int32_t index;
	bool has_reference;
	uint32_t flags;
	int32_t poc;
};

/*
 * One recorded video or query command.  It starts with the common
 * record; it retains every object it names until its command buffer is
 * reset or its last submission retires.  A decode borrows the begin of its
 * coding scope and the begin query open around it (status).
 */
struct video_command {
	struct bcm2711_vulkan_record record;
	struct bcm2711_vulkan_object *session;
	struct bcm2711_vulkan_object *parameters;
	struct bcm2711_vulkan_object *buffer;
	struct bcm2711_vulkan_object *view;
	struct bcm2711_vulkan_object *pool;
	const struct video_command *begin;
	const struct video_command *status;
	uint32_t flags;
	uint64_t offset;
	uint64_t range;
	struct video_resource destination;
	bool has_setup;
	struct video_slot setup;
	uint32_t slot_count;
	struct video_slot slots[VIDEO_MAX_DPB_SLOTS];
	bool has_picture;
	struct bcm2712_hevc_picture picture;
	uint32_t slice_count;
	bool slices_overflow;
	uint32_t slices[BCM2712_HEVC_SLICES];
	uint32_t query;
	uint32_t query_count;
};

/*
 * What the chained records of one structure carried.  It lives on the
 * stack of one command's decoding; the add record's sets are the
 * parameters object's to take, or freed with the chain.
 */
struct video_chain {
	bool has_h265_profile;
	uint32_t profile_idc;
	bool has_profile_list;
	uint32_t profile_count;
	bool profiles_supported;
	int32_t profile_error;
	bool has_h265_create;
	uint32_t max_vps;
	uint32_t max_sps;
	uint32_t max_pps;
	uint32_t new_vps;
	struct video_sps_node *new_sps;
	struct video_pps_node *new_pps;
	int add_error;
	bool has_reference;
	uint32_t reference_flags;
	int32_t reference_poc;
	bool has_picture;
	struct bcm2712_hevc_picture picture;
	uint32_t *slices;
	uint32_t slice_count;
	bool slices_overflow;
};

static int video_capabilities(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void reply_capabilities(struct i915_wire_writer *reply, const uint32_t *types, uint32_t count);
static int video_format_properties(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_family_properties(struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int session_create(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int session_requirements(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int session_bind(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int parameters_create(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int parameters_update(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int destroy_object(struct bcm2711_vulkan_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader);
static int release_session(struct bcm2711_vulkan_session *session, void *payload);
static int release_parameters(struct bcm2711_vulkan_session *session, void *payload);
static int release_pool(struct bcm2711_vulkan_session *session, void *payload);
static int record_command(struct bcm2711_vulkan_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static int decode_command(struct bcm2711_vulkan_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct video_command *command);
static int retain_command(struct bcm2711_vulkan_session *session, struct video_command *command, uint64_t session_id, uint64_t parameters_id, uint64_t buffer_id, uint64_t view_id, uint64_t pool_id);
static int release_command(struct bcm2711_vulkan_command_node *node);
static const struct video_command *open_scope(const struct bcm2711_vulkan_command_buffer *command);
static const struct video_command *open_query(const struct bcm2711_vulkan_command_buffer *command);
static void read_chain(struct i915_wire_reader *reader, struct video_chain *chain, uint32_t depth);
static void read_fields(struct i915_wire_reader *reader, uint32_t type, struct video_chain *chain, uint32_t depth);
static int32_t read_profile(struct i915_wire_reader *reader, uint32_t *profile_idc);
static int32_t profile_result(uint32_t operation, uint32_t chroma, uint32_t luma, uint32_t chroma_depth, const struct video_chain *chain);
static void read_resource(struct i915_wire_reader *reader, struct video_resource *resource);
static void read_slot(struct i915_wire_reader *reader, struct video_slot *slot, struct video_resource *resource);
static void read_slots(struct i915_wire_reader *reader, struct video_command *command);
static void read_extension(struct i915_wire_reader *reader, char *name, uint32_t *version);
static void read_offsets(struct i915_wire_reader *reader, struct video_chain *chain);
static void read_picture(struct i915_wire_reader *reader, struct bcm2712_hevc_picture *picture);
static void read_add(struct i915_wire_reader *reader, struct video_chain *chain);
static uint32_t read_vps(struct i915_wire_reader *reader);
static void read_sps(struct i915_wire_reader *reader, struct bcm2712_hevc_sps *sps);
static void read_pps(struct i915_wire_reader *reader, struct bcm2712_hevc_pps *pps);
static void read_dpb(struct i915_wire_reader *reader);
static void read_hrd(struct i915_wire_reader *reader, uint32_t sub_layers);
static void read_sub_layers(struct i915_wire_reader *reader, uint32_t sub_layers);
static void read_scaling(struct i915_wire_reader *reader, struct bcm2712_hevc_scaling *scaling);
static void read_words(struct i915_wire_reader *reader, uint32_t *words, uint32_t expected);
static void read_fixed(struct i915_wire_reader *reader, void *bytes, uint32_t expected);
static void skip_words(struct i915_wire_reader *reader, uint32_t count);
static void chain_free(struct video_chain *chain);
static int parameters_take(struct video_parameters *parameters, struct video_chain *chain, bool replace);
static int parameters_copy(struct video_parameters *destination, const struct video_parameters *source);
static void parameters_free(struct video_parameters *parameters);
static const struct bcm2712_hevc_sps *find_sps(const struct video_parameters *parameters, uint32_t vps_id, uint32_t sps_id);
static const struct bcm2712_hevc_pps *find_pps(const struct video_parameters *parameters, uint32_t vps_id, uint32_t sps_id, uint32_t pps_id);
static int pool_create(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int pool_results(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int record_query(struct bcm2711_vulkan_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static int run_decode(struct bcm2711_render_device *controller, const struct video_command *command);
static const char *start_decode(struct bcm2711_render_device *controller, struct video_session *video, const struct video_command *command);
static int picture_address(const struct video_session *video, uint32_t index, uint64_t *physical, uint32_t *native);
static int image_target(const struct bcm2711_vulkan_object *view_object, struct bcm2711_vulkan_resource **image, uint32_t *native, uint32_t *pitch, uint64_t *chroma);
static int grow_list(struct bcm2711_buffer **list);
static void phase1_buffers(const struct video_session *video, uint32_t command_count, uint32_t ctb_rows, struct bcm2712_hevc_phase1_buffers *buffers);
static int convert(struct bcm2711_render_device *controller, const struct video_running *running);
static void finish(const struct video_command *command, int32_t status);
static void skip(struct video_session *video, const struct video_command *command, const char *reason, int error);
static void wait_idle(void);

/* Whether any session has been promised video, for the log line said once. */
static bool video_announced;

/*
 * The decode in the block, if any; under the controller mutex.  Its
 * command field is NULL while the block is free.
 */
static struct video_running video_running;

/* Whether the block stopped answering: no decode is started on it again this boot.  Under the controller mutex. */
static bool video_block_lost;

/* The slice segments of the decode being started (too large for the stack); under the controller mutex. */
static struct bcm2712_hevc_slice video_slices[BCM2712_HEVC_SLICES];

/* The header bytes of the segment being read, copied out of the application's memory; under the controller mutex. */
static uint8_t video_header[VIDEO_HEADER_BYTES];

/*
 * Reports whether video decode can be offered now: the HEVC block was
 * found.
 */
bool
bcm2711_vulkan_video_available(void)
{
	bool present;

	/* The block must be on the board and still answer. */
	present = bcm2712_hevc_decoder_present();
	if (!present || video_block_lost)
		return false;

	/* Succeeded: the capset may promise H.265 decode. */
	return true;
}

/*
 * Reports whether a session promised video decode in its capset.
 */
bool
bcm2711_vulkan_video_session(
	const struct bcm2711_vulkan_session *session)
{
	/* Latched once by the capset. */
	return session->video;
}

/*
 * Records that a session's capset promised video decode.
 */
void
bcm2711_vulkan_video_promise(
	struct bcm2711_vulkan_session *session)
{
	/* Latches the promise, and says so once a boot. */
	session->video = true;
	if (!video_announced) {
		video_announced = true;
		kern_logf("v3d: vk: capset declares H.265 video decode (HEVC decoder block)\n");
	}
}

/*
 * Routes the video commands; a session without the promise refuses them.
 */
int
bcm2711_vulkan_video_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Only the video range belongs here. */
	*handled = 0;
	if (opcode < GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES || opcode > GPU_OP_CMD_DECODE_VIDEO)
		return 0;
	*handled = 1;

	/* A session without the promise takes none of them. */
	if (!session->video)
		return ENOTSUP;

	/* Runs the command the opcode names. */
	(void)requested;
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES:
		error = video_capabilities(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES:
		error = video_format_properties(reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES:
		error = video_family_properties(reader, reply);
		break;
	case GPU_OP_CREATE_VIDEO_SESSION:
		error = session_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_VIDEO_SESSION:
		error = destroy_object(session, I915_VK_OBJ_VIDEO_SESSION, reader);
		break;
	case GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS:
		error = session_requirements(session, reader, reply);
		break;
	case GPU_OP_BIND_VIDEO_SESSION_MEMORY:
		error = session_bind(session, reader, reply);
		break;
	case GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS:
		error = parameters_create(session, reader, reply);
		break;
	case GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS:
		error = parameters_update(session, reader, reply);
		break;
	case GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS:
		error = destroy_object(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, reader);
		break;
	default:
		/* The four recorded commands. */
		error = record_command(session, opcode, reader);
		break;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran or was recorded. */
	return 0;
}

/*
 * Routes the query pool commands: a result status pool on a session that
 * promised video, and its recorded begin, end and reset.
 */
int
bcm2711_vulkan_video_query_dispatch(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Only the pool commands belong here. */
	*handled = 0;
	if (opcode != GPU_OP_CREATE_QUERY_POOL &&
	    opcode != GPU_OP_DESTROY_QUERY_POOL &&
	    opcode != GPU_OP_GET_QUERY_POOL_RESULTS &&
	    opcode != GPU_OP_CMD_BEGIN_QUERY &&
	    opcode != GPU_OP_CMD_END_QUERY &&
	    opcode != GPU_OP_CMD_RESET_QUERY_POOL)
		return 0;
	*handled = 1;

	/* Without video there are no query pools. */
	if (!session->video)
		return ENOTSUP;

	/* Runs or records the command. */
	(void)requested;
	if (opcode == GPU_OP_CREATE_QUERY_POOL) {
		error = pool_create(session, reader, reply);
	} else if (opcode == GPU_OP_DESTROY_QUERY_POOL) {
		error = destroy_object(session, I915_VK_OBJ_QUERY_POOL, reader);
	} else if (opcode == GPU_OP_GET_QUERY_POOL_RESULTS) {
		error = pool_results(session, reader, reply);
	} else {
		error = record_query(session, opcode, reader);
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran or was recorded. */
	return 0;
}

/*
 * Reports whether a recorded opcode is one this file executes.
 */
bool
bcm2711_vulkan_video_event(
	uint32_t opcode)
{
	/* The four video commands and the three query commands. */
	if (opcode >= GPU_OP_CMD_BEGIN_VIDEO_CODING && opcode <= GPU_OP_CMD_DECODE_VIDEO)
		return true;
	if (opcode == GPU_OP_CMD_BEGIN_QUERY || opcode == GPU_OP_CMD_END_QUERY || opcode == GPU_OP_CMD_RESET_QUERY_POOL)
		return true;

	/* Succeeded: not one of them. */
	return false;
}

/*
 * Executes one recorded video or query event in the native job.
 *
 * A decode runs a step at a time (EINPROGRESS until its picture is in its
 * image); a query reset makes its queries unavailable; a begin query sets
 * the status the decodes inside write; an end query makes it available
 * (the decodes before it are complete).  The rest do nothing.
 */
int
bcm2711_vulkan_video_run(
	struct bcm2711_render_device *controller,
	const struct bcm2711_vulkan_record *record)
{
	const struct video_command *command;
	struct video_pool *pool;
	uint32_t index;
	int error;

	/* The record is a video command's. */
	command = (const struct video_command *)record;
	error = 0;

	/* Each event has its own effect. */
	switch (record->opcode) {
	case GPU_OP_CMD_DECODE_VIDEO:
		error = run_decode(controller, command);
		break;
	case GPU_OP_CMD_RESET_QUERY_POOL:
		/* Every query of the range becomes unavailable. */
		pool = command->pool->payload;
		for (index = command->query; index < command->query + command->query_count; index++) {
			pool->available[index] = 0;
			pool->status[index] = 0;
		}

		break;
	case GPU_OP_CMD_BEGIN_QUERY:
		/* The decodes inside write this query's status; until one does it is complete. */
		pool = command->pool->payload;
		pool->status[command->query] = VIDEO_STATUS_COMPLETE;
		break;
	case GPU_OP_CMD_END_QUERY:
		/* The decodes before it are done: the query is available. */
		pool = command->pool->payload;
		pool->available[command->query] = 1;
		break;
	default:
		/* A begin, an end, or a control: the block keeps nothing between pictures. */
		break;
	}

	/* Reports an event that could not run, or one still running. */
	if (error != 0)
		return error;

	/* Succeeded: the event ran. */
	return 0;
}

/*
 * Waits for the frames of the decodes in flight whose images a command
 * names: a decode here ends before its event does, so there are none.
 */
int
bcm2711_vulkan_video_settle(
	struct bcm2711_render_device *controller,
	const struct bcm2711_vulkan_record *record)
{
	/* Nothing is in flight past its event. */
	(void)controller;
	(void)record;
	return 0;
}

/*
 * Adds the newest decode in flight a command buffer recorded to a mark:
 * none outlives its event.
 */
void
bcm2711_vulkan_video_mark_commands(
	struct bcm2711_vulkan_video_mark *mark,
	const struct bcm2711_vulkan_command_buffer *command)
{
	/* The mark stays empty. */
	(void)mark;
	(void)command;
}

/*
 * Makes a mark cover every decode in flight: none outlives its event.
 */
void
bcm2711_vulkan_video_mark_all(
	struct bcm2711_vulkan_video_mark *mark)
{
	/* The mark stays empty. */
	(void)mark;
}

/*
 * Waits for the decodes a mark covers: none outlives its event.
 */
int
bcm2711_vulkan_video_settle_mark(
	struct bcm2711_render_device *controller,
	const struct bcm2711_vulkan_video_mark *mark,
	bool wait)
{
	/* Nothing to wait for. */
	(void)controller;
	(void)mark;
	(void)wait;
	return 0;
}

/*
 * Lays out an NV12 image: rows of the width rounded up to 32 bytes, the
 * luma plane over the height rounded up to 16 rows, then the interleaved
 * chroma plane of half as many rows (the Pi 4's layout, which the
 * sampling and the copies of the executor read).
 */
void
bcm2711_vulkan_video_nv12_layout(
	uint32_t width,
	uint32_t height,
	uint32_t *pitch,
	uint64_t *chroma,
	uint64_t *bytes)
{
	uint32_t rows;

	/* The pitch, the rounded height, the chroma plane after the luma rows. */
	*pitch = (width + 31U) & ~31U;
	rows = (height + 15U) & ~15U;
	*chroma = (uint64_t)*pitch * rows;
	*bytes = *chroma + (uint64_t)*pitch * (rows / 2U);
}

/*
 * Reports whether a decoded NV12 image holds samples that its readback
 * corrects: the block's samples are as H.265 defines them.
 */
bool
bcm2711_vulkan_video_image_raw(
	const struct bcm2711_vulkan_resource *image)
{
	/* Never. */
	(void)image;
	return false;
}

/*
 * Copies rows of a decoded NV12 image from a cached copy: the image is the
 * only copy, so the caller always reads the image.
 */
bool
bcm2711_vulkan_video_readback(
	const struct bcm2711_vulkan_resource *image,
	uint64_t offset,
	uint64_t bytes,
	uint32_t rows,
	uint8_t *destination,
	uint64_t destination_pitch)
{
	/* No cached copy. */
	(void)image;
	(void)offset;
	(void)bytes;
	(void)rows;
	(void)destination;
	(void)destination_pitch;
	return false;
}

/*
 * Adds the time of an NV12 image's readback to the cost line: none is
 * kept.
 */
void
bcm2711_vulkan_video_readback_done(
	bool cached,
	uint64_t ticks)
{
	/* Nothing is counted. */
	(void)cached;
	(void)ticks;
}

/*
 * vkGetPhysicalDeviceVideoCapabilitiesKHR: [physical][present]{profile}
 * [present][shape: type, link...] -> [result][present]{capabilities}.
 */
static int
video_capabilities(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t types[VIDEO_MAX_CHAIN];
	uint32_t count;
	uint32_t idc;
	uint64_t present;
	int32_t result;

	/* Skips the physical device and reads the profile, which decides the result. */
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	result = VIDEO_ERROR_OPERATION;
	if (present != 0U)
		result = read_profile(reader, &idc);

	/* Reads the shape of the output: the capabilities record and what is chained to it. */
	count = 0U;
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
	if (count == 0U || types[0] != VIDEO_TYPE_CAPABILITIES)
		return EINVAL;

	/* A profile the decoder does not take replies its reason only. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Replies the result and the records in the shape's order. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	reply_capabilities(reply, types, count);

	/* Succeeded: the capabilities are reported. */
	return 0;
}

/* Replies one output record of the capabilities' shape and, nested before its fields, the rest. */
static void
reply_capabilities(
	struct i915_wire_writer *reply,
	const uint32_t *types,
	uint32_t count)
{
	char name[VK_MAX_EXTENSION_NAME_SIZE];

	/* The record's type, then the link to the rest of the shape. */
	drv_i915_wire_reply_u32(reply, types[0]);
	if (count > 1U) {
		drv_i915_wire_reply_u64(reply, 1U);
		reply_capabilities(reply, types + 1, count - 1U);
	} else {
		drv_i915_wire_reply_u64(reply, 0U);
	}

	/* The record's fields. */
	switch (types[0]) {
	case VIDEO_TYPE_CAPABILITIES:
		/* Separate reference images; 64-byte offsets and sizes; the extents and the slot limits. */
		drv_i915_wire_reply_u32(reply, VIDEO_SEPARATE_REFERENCE_IMAGES);
		drv_i915_wire_reply_u64(reply, VIDEO_BITSTREAM_ALIGN);
		drv_i915_wire_reply_u64(reply, VIDEO_BITSTREAM_ALIGN);
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_DPB_SLOTS);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_REFERENCES);

		/* The H.265 header name, padded to its array, and version. */
		kern_memset(name, 0, sizeof(name));
		kern_memcpy(name, VIDEO_STD_NAME, sizeof(VIDEO_STD_NAME));
		drv_i915_wire_reply_u64(reply, VK_MAX_EXTENSION_NAME_SIZE);
		drv_i915_wire_reply_bytes(reply, name, sizeof(name));
		drv_i915_wire_reply_u32(reply, VIDEO_STD_VERSION);
		break;
	case VIDEO_TYPE_DECODE_CAPABILITIES:
		/* The output picture is the reference picture. */
		drv_i915_wire_reply_u32(reply, VIDEO_DPB_AND_OUTPUT_COINCIDE);
		break;
	case VIDEO_TYPE_H265_CAPABILITIES:
		/* Level 5.1. */
		drv_i915_wire_reply_u32(reply, VIDEO_LEVEL_5_1);
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
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_chain chain;
	uint64_t present;
	uint32_t type;
	uint32_t usage;
	uint32_t video_usage;
	int32_t result;

	/* Reads the format info: its type, its chain (the profile list) and the usage, then the capacity. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_FORMAT_INFO)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	usage = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	chain_free(&chain);
	if (reader->error != 0)
		return EINVAL;

	/* The profiles must all be H.265 decode; usages include readback and the sampling of the planes. */
	result = 0;
	video_usage = BCM2711_VIDEO_USAGE_DECODE_DST | BCM2711_VIDEO_USAGE_DECODE_DPB | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!chain.has_profile_list || chain.profile_count == 0U) {
		result = VIDEO_ERROR_OPERATION;
	} else if (!chain.profiles_supported) {
		result = chain.profile_error;
	} else if ((usage & ~video_usage) != 0U) {
		result = VIDEO_ERROR_IMAGE_USAGE;
	}

	/* Reports a format the decoder does not offer. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Replies the one format: NV12, identity swizzle, plane-viewable, 2D, optimal, its usages. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VIDEO_TYPE_FORMAT_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0U);
	drv_i915_wire_reply_u32(reply, BCM2711_VIDEO_FORMAT_NV12);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_TYPE_2D);
	drv_i915_wire_reply_u32(reply, VK_IMAGE_TILING_OPTIMAL);
	drv_i915_wire_reply_u32(reply, video_usage);

	/* Succeeded: the format is reported. */
	return 0;
}

/*
 * The codec operations of each queue family: [physical][count] ->
 * [present][count]{operations}.  The one family decodes H.265.
 */
static int
video_family_properties(
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t count;

	/* Skips the physical device and reads the family count, which is the one family. */
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || count != 1U)
		return EINVAL;

	/* Replies H.265 decode for family 0. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u32(reply, VIDEO_OP_DECODE_H265);

	/* Succeeded: the family's codec operations are reported. */
	return 0;
}

/*
 * vkCreateVideoSessionKHR: [device][present]{create info}[allocator][present]
 * [identity] -> [result][present][identity].
 *
 * The session's pictures are laid out for its largest extent (their memory
 * is the application's, bound later); its phase 1 lists and command list
 * are the executor's.  The block's clock is taken for the session's life.
 */
static int
session_create(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_session *video;
	struct video_chain chain;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	char name[VK_MAX_EXTENSION_NAME_SIZE];
	uint64_t device_id;
	uint64_t present;
	uint64_t identity;
	uint64_t pu_bytes;
	uint64_t coefficient_bytes;
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
	int different;
	int error;
	int released;

	/* Reads the create info: [type][chain][family][flags][profile][formats and limits][header]. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(name, 0, sizeof(name));
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_SESSION_CREATE)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	family = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	result = VIDEO_ERROR_OPERATION;
	profile_idc = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		result = read_profile(reader, &profile_idc);
	picture_format = drv_i915_wire_read_u32(reader);
	width = drv_i915_wire_read_u32(reader);
	height = drv_i915_wire_read_u32(reader);
	reference_format = drv_i915_wire_read_u32(reader);
	slots = drv_i915_wire_read_u32(reader);
	references = drv_i915_wire_read_u32(reader);
	version = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_extension(reader, name, &version);

	/* Skips pAllocator and the output's present word, and reads the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	chain_free(&chain);
	if (reader->error != 0 || identity == 0)
		return EINVAL;
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (device == NULL)
		return EINVAL;

	/* Checks the session against what the decoder takes; the first reason is the result. */
	different = kern_strcmp(name, VIDEO_STD_NAME);
	if (result != 0) {
		/* The profile's own reason. */
	} else if (family != 0U) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (flags != 0U) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
	} else if (picture_format != BCM2711_VIDEO_FORMAT_NV12 || reference_format != BCM2711_VIDEO_FORMAT_NV12) {
		result = VIDEO_ERROR_FORMAT;
	} else if (width < VIDEO_MIN_SIDE || height < VIDEO_MIN_SIDE || width > VIDEO_MAX_SIDE || height > VIDEO_MAX_SIDE) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (slots > VIDEO_MAX_DPB_SLOTS || references > VIDEO_MAX_REFERENCES) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (different != 0 || version > VIDEO_STD_VERSION) {
		result = VIDEO_ERROR_STD_VERSION;
	} else if (!bcm2711_vulkan_video_available()) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Reports a session the decoder does not take. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Allocates the session with its limits and its pictures' layout. */
	video = kern_calloc(1U, sizeof(*video));
	if (video == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}
	video->width = width;
	video->height = height;
	video->max_dpb_slots = slots;
	video->max_references = references;
	bcm2712_hevc_sand_layout(width, height, &video->sand);
	bcm2712_hevc_motion_layout(width, height, &video->motion_stride, &video->motion_bytes);
	video->picture_bytes = (video->sand.luma_bytes + video->sand.chroma_bytes + video->motion_bytes + VIDEO_PAGE - 1U) & ~(uint64_t)(VIDEO_PAGE - 1U);
	video->pictures = slots + 1U;

	/* The phase 1 lists and the command list, and the block's clock. */
	bcm2712_hevc_list_sizes(width, height, &pu_bytes, &coefficient_bytes);
	error = bcm2711_buffer_create_uncached(pu_bytes, VIDEO_BUFFER_LIMIT, VIDEO_PAGE, &video->pu);
	if (error == 0)
		error = bcm2711_buffer_create_uncached(coefficient_bytes, VIDEO_BUFFER_LIMIT, VIDEO_PAGE, &video->coefficients);
	if (error == 0)
		error = bcm2711_buffer_create_uncached((uint64_t)VIDEO_COMMAND_PAIRS * 8U, VIDEO_BUFFER_LIMIT, VIDEO_PAGE, &video->commands);
	if (error == 0) {
		error = bcm2712_hevc_decoder_hold();
		if (error == 0)
			video->holding = true;
	}
	if (error == 0)
		error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_logf("v3d: video: no decoder for a %ux%u session (%d)\n", (unsigned)width, (unsigned)height, error);
		(void)release_session(session, video);
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	/* The session keeps its device. */
	video->owner.device = device;

	/* Publishes the session under its identity. */
	error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_VIDEO_SESSION, identity, video, release_session, &object);
	if (error != 0) {
		released = release_session(session, video);
		if (released != 0)
			return released;
		if (error != ENOMEM)
			return error;
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

/*
 * vkGetVideoSessionMemoryRequirementsKHR: [device][session][present][capacity]
 * [array] -> [result][present][count][array]{binding}.
 *
 * One binding for each picture of the session (one for each DPB slot and
 * the spare): its SAND planes and motion vectors, page aligned, of memory
 * type 0.
 */
static int
session_requirements(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct video_session *video;
	uint64_t identity;
	uint64_t present;
	uint32_t capacity;
	uint32_t count;
	uint32_t index;

	/* Skips the device, reads the session, whether the array is asked for, and the capacity. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown session is refused. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (object == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}
	video = object->payload;

	/* The bindings, as many as the capacity holds. */
	count = video->pictures;
	if (present != 0U && capacity < count)
		count = capacity;
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, count);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++) {
		drv_i915_wire_reply_u32(reply, VIDEO_TYPE_MEMORY_REQUIREMENTS);
		drv_i915_wire_reply_u64(reply, 0U);
		drv_i915_wire_reply_u32(reply, index);
		drv_i915_wire_reply_u64(reply, video->picture_bytes);
		drv_i915_wire_reply_u64(reply, VIDEO_PAGE);
		drv_i915_wire_reply_u32(reply, 1U);
	}

	/* Succeeded: the bindings are reported. */
	return 0;
}

/*
 * vkBindVideoSessionMemoryKHR: [device][session][count][array]{bind} -> [result].
 *
 * Each picture's binding keeps its memory object; an offset other than 0
 * or memory smaller than the binding is refused.
 */
static int
session_bind(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_object *bound;
	struct bcm2711_vulkan_memory *memory;
	struct video_session *video;
	struct video_chain chain;
	uint64_t identity;
	uint64_t count;
	uint64_t item;
	uint64_t memory_id;
	uint64_t offset;
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

	/* Reads every binding: [type][chain][index][memory][offset][size]. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	video = NULL;
	if (object != NULL)
		video = object->payload;
	result = 0;
	if (video == NULL)
		result = VK_ERROR_INITIALIZATION_FAILED;
	for (item = 0U; item < count && reader->error == 0; item++) {
		kern_memset(&chain, 0, sizeof(chain));
		type = drv_i915_wire_read_u32(reader);
		if (type != VIDEO_TYPE_BIND_MEMORY)
			reader->error = 1;
		read_chain(reader, &chain, 0U);
		chain_free(&chain);
		index = drv_i915_wire_read_u32(reader);
		memory_id = drv_i915_wire_read_u64(reader);
		offset = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || video == NULL)
			continue;

		/* The binding must be the session's, unbound, of whole memory large enough. */
		bound = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, memory_id);
		memory = NULL;
		if (bound != NULL)
			memory = bound->payload;
		if (index >= video->pictures || video->picture[index].memory != NULL || memory == NULL ||
		    offset != 0U || memory->bytes < video->picture_bytes) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		}

		/* The picture keeps its memory. */
		error = bcm2711_vulkan_object_retain(bound);
		if (error != 0)
			return error;
		video->picture[index].memory = bound;
	}

	/* Refuses a malformed request. */
	if (reader->error != 0)
		return EINVAL;

	/* Replies the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)result);

	/* Succeeded: the command was decoded; the reply carries its result. */
	return 0;
}

/*
 * vkCreateVideoSessionParametersKHR: [device][present]{create info: chain
 * (H.265 create and add), flags, template, session}[allocator][present]
 * [identity] -> [result][present][identity].
 */
static int
parameters_create(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_parameters *parameters;
	struct video_parameters *source;
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *owner;
	struct bcm2711_vulkan_object *template_object;
	struct bcm2711_vulkan_object *object;
	struct video_chain chain;
	uint64_t device_id;
	uint64_t present;
	uint64_t identity;
	uint64_t template_identity;
	uint64_t session_identity;
	uint32_t type;
	int32_t result;
	int error;
	int released;

	/* Reads the create info: [type][chain][flags][template][session], then the identity. */
	kern_memset(&chain, 0, sizeof(chain));
	device_id = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_PARAMETERS_CREATE)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	(void)drv_i915_wire_read_u32(reader);
	template_identity = drv_i915_wire_read_u64(reader);
	session_identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (reader->error != 0 || device == NULL || identity == 0) {
		chain_free(&chain);
		return EINVAL;
	}

	/* The session must exist and the H.265 create record be given; a template must be of the same session. */
	result = 0;
	source = NULL;
	template_object = NULL;
	owner = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION, session_identity);
	if (template_identity != 0U)
		template_object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, template_identity);
	if (template_object != NULL)
		source = template_object->payload;
	if (owner == NULL) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (!chain.has_h265_create || chain.add_error != 0) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (template_identity != 0U && (source == NULL || source->owner.parent != owner)) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	}

	/* Reports a request the object cannot serve. */
	if (result != 0) {
		chain_free(&chain);
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Allocates the object, which keeps its device and its session. */
	parameters = kern_calloc(1U, sizeof(*parameters));
	if (parameters == NULL) {
		chain_free(&chain);
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}

	/* Retains the device and the owner the object lives under. */
	error = bcm2711_vulkan_object_retain(device);
	if (error == 0) {
		parameters->owner.device = device;
		error = bcm2711_vulkan_object_retain(owner);
		if (error == 0)
			parameters->owner.parent = owner;
	}

	/* The capacity the application asked for. */
	parameters->max_vps = chain.max_vps;
	parameters->max_sps = chain.max_sps;
	parameters->max_pps = chain.max_pps;

	/* Copies the template's sets, then takes the new ones, which replace sets of the same key. */
	if (error == 0 && source != NULL)
		error = parameters_copy(parameters, source);
	if (error == 0)
		error = parameters_take(parameters, &chain, true);
	chain_free(&chain);

	/* Publishes the object under its identity. */
	if (error == 0)
		error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity, parameters, release_parameters, &object);
	if (error != 0) {
		released = release_parameters(session, parameters);
		if (released != 0)
			return released;
		if (error == EEXIST || error == ECANCELED)
			return error;
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
 * info: chain (add), sequence count} -> [result].  The update adds its
 * sets whole or not at all.
 */
static int
parameters_update(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct video_parameters *parameters;
	struct video_chain chain;
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
	if (present != 1U || type != VIDEO_TYPE_PARAMETERS_UPDATE)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	sequence = drv_i915_wire_read_u32(reader);
	if (reader->error != 0) {
		chain_free(&chain);
		return EINVAL;
	}

	/* The object must exist and the update be the next one; a malformed set refuses it. */
	result = 0;
	parameters = NULL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity);
	if (object != NULL)
		parameters = object->payload;
	if (parameters == NULL || sequence != parameters->update_sequence + 1U || chain.add_error != 0)
		result = VK_ERROR_INITIALIZATION_FAILED;

	/* Adds the sets; a key already held refuses the whole update. */
	if (result == 0) {
		error = parameters_take(parameters, &chain, false);
		if (error == ENOMEM) {
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
		} else if (error != 0) {
			result = VK_ERROR_INITIALIZATION_FAILED;
		}
	}

	/* Records the update's sequence number when it is taken. */
	chain_free(&chain);
	if (result == 0)
		parameters->update_sequence = sequence;

	/* Replies the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)result);

	/* Succeeded: the command was decoded; the reply carries its result. */
	return 0;
}

/*
 * vkDestroyVideoSessionKHR, vkDestroyVideoSessionParametersKHR and
 * vkDestroyQueryPool: [device][object][allocator], no reply body.
 */
static int
destroy_object(
	struct bcm2711_vulkan_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *object;
	uint64_t identity;
	int error;

	/* Skips the device, reads the object and skips pAllocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A null or unknown object is nothing. */
	object = bcm2711_vulkan_object_find(session, kind, identity);
	if (identity == 0 || object == NULL)
		return 0;
	error = bcm2711_vulkan_object_remove(session, kind, identity);
	if (error != 0)
		return error;

	/* Succeeded: the identity is gone. */
	return 0;
}

/*
 * Frees a session after its last owner: a decode of it still in the block
 * is waited for first (the block writes its memory), then its pictures'
 * memory, its lists, the clock and the device edge go.  A block that does
 * not finish keeps the session's memory for the boot.
 */
static int
release_session(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct video_session *video;
	uint32_t index;
	int error;
	int released;

	/* The block lets go of the session's memory first. */
	(void)session;
	video = payload;
	if (video_running.command != NULL && video_running.video == video)
		wait_idle();
	if (video_running.command != NULL && video_running.video == video) {
		kern_logf("v3d: video: the decoder kept a session's memory; it stays for the boot\n");
		return 0;
	}

	/* The pictures' memory objects. */
	error = 0;
	for (index = 0; index < VIDEO_PICTURES; index++) {
		if (video->picture[index].memory == NULL)
			continue;
		released = bcm2711_vulkan_object_release(video->picture[index].memory);
		if (error == 0)
			error = released;
	}

	/* The lists and the command list. */
	if (video->pu != NULL)
		bcm2711_buffer_release(video->pu);
	if (video->coefficients != NULL)
		bcm2711_buffer_release(video->coefficients);
	if (video->commands != NULL)
		bcm2711_buffer_release(video->commands);

	/* The clock and the device edge. */
	if (video->holding)
		bcm2712_hevc_decoder_release();
	if (video->owner.device != NULL) {
		released = bcm2711_vulkan_object_release(video->owner.device);
		if (error == 0)
			error = released;
	}

	/* Frees the session. */
	kern_free(video);
	if (error != 0)
		return error;

	/* Succeeded: the session is gone. */
	return 0;
}

/* Frees a parameters object after its last owner, with its sets and its edges. */
static int
release_parameters(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct video_parameters *parameters;
	int error;
	int released;

	/* The sets, then the session and device edges. */
	(void)session;
	parameters = payload;
	parameters_free(parameters);
	error = 0;
	if (parameters->owner.parent != NULL)
		error = bcm2711_vulkan_object_release(parameters->owner.parent);
	if (parameters->owner.device != NULL) {
		released = bcm2711_vulkan_object_release(parameters->owner.device);
		if (error == 0)
			error = released;
	}

	/* Frees the object. */
	kern_free(parameters);
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
}

/* Frees a query pool after its last owner. */
static int
release_pool(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct video_pool *pool;
	int error;

	/* The device edge, then the pool. */
	(void)session;
	pool = payload;
	error = 0;
	if (pool->owner.device != NULL)
		error = bcm2711_vulkan_object_release(pool->owner.device);
	kern_free(pool);
	if (error != 0)
		return error;

	/* Succeeded: the pool is gone. */
	return 0;
}

/*
 * Records one of the four video commands into its command buffer.
 *
 * The command is decoded whole, then the objects it names are retained;
 * a use the executor cannot run marks the recording failed instead of
 * failing the stream.
 */
static int
record_command(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct video_command *video;
	uint64_t identity;
	int error;
	int released;

	/* The recording primary. */
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (object == NULL)
		return EINVAL;
	command = object->payload;
	if (command->pending != 0 || command->state != BCM2711_VULKAN_COMMAND_RECORDING)
		return EBUSY;

	/* The node, with the common record first. */
	video = kern_calloc(1U, sizeof(*video));
	if (video == NULL)
		return ENOMEM;
	video->record.opcode = opcode;
	video->record.node.release = release_command;

	/* Decodes the whole command and retains what it names. */
	error = decode_command(session, opcode, reader, video);
	if (error == EINVAL && reader->error != 0) {
		(void)release_command(&video->record.node);
		return EINVAL;
	}

	/* A command buffer that failed recording takes nothing more. */
	if (command->recording_error != 0) {
		released = release_command(&video->record.node);
		if (released != 0)
			return released;
		return 0;
	}

	/* A decode, a control and an end belong to an open coding scope; a begin opens one. */
	if (error == 0 && opcode != GPU_OP_CMD_BEGIN_VIDEO_CODING) {
		video->begin = open_scope(command);
		if (video->begin == NULL)
			error = EINVAL;
	}

	/* A decode writes its status into the begin query open around it, if any. */
	if (error == 0 && opcode == GPU_OP_CMD_DECODE_VIDEO)
		video->status = open_query(command);
	if (error == 0 && command->render_open)
		error = ENOTSUP;
	if (error == 0 && sizeof(*video) > BCM2711_VULKAN_RECORD_BYTES - command->recorded_bytes)
		error = ENOMEM;
	if (error != 0) {
		command->recording_error = error;
		released = release_command(&video->record.node);
		if (released != 0)
			return released;
		return 0;
	}

	/* Appends the node to the primary's ordered list. */
	if (command->last == NULL) {
		command->first = &video->record.node;
	} else {
		command->last->next = &video->record.node;
	}

	/* The record is the command buffer's last. */
	command->last = &video->record.node;
	command->recorded_bytes += sizeof(*video);

	/* Succeeded: the command is recorded. */
	return 0;
}

/*
 * Decodes one video command's structure and retains the objects it names.
 * Returns EINVAL with the reader's error set for malformed framing, another
 * error for a use the executor refuses.
 */
static int
decode_command(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct video_command *command)
{
	struct video_chain chain;
	struct video_resource setup_resource;
	uint64_t present;
	uint64_t session_id;
	uint64_t parameters_id;
	uint64_t buffer_id;
	uint32_t type;
	int error;

	/* Reads the structure's presence, its type and its chain (with the slice offsets into the command). */
	kern_memset(&chain, 0, sizeof(chain));
	chain.slices = command->slices;
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	session_id = 0;
	parameters_id = 0;
	buffer_id = 0;

	/* Reads the fields of the command's structure. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
		/* [flags][session][parameters][count][array]{slot}. */
		if (type != VIDEO_TYPE_BEGIN)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		session_id = drv_i915_wire_read_u64(reader);
		parameters_id = drv_i915_wire_read_u64(reader);
		read_slots(reader, command);
		command->slot_count = 0;
		break;
	case GPU_OP_CMD_END_VIDEO_CODING:
		/* [flags]. */
		if (type != VIDEO_TYPE_END)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		break;
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
		/* [flags]. */
		if (type != VIDEO_TYPE_CONTROL)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		break;
	default:
		/* vkCmdDecodeVideoKHR: [flags][buffer][offset][range][resource][present]{setup}[count][array]{slot}. */
		if (type != VIDEO_TYPE_DECODE)
			reader->error = 1;
		command->flags = drv_i915_wire_read_u32(reader);
		buffer_id = drv_i915_wire_read_u64(reader);
		command->offset = drv_i915_wire_read_u64(reader);
		command->range = drv_i915_wire_read_u64(reader);
		read_resource(reader, &command->destination);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			command->has_setup = true;
			read_slot(reader, &command->setup, &setup_resource);
		}
		read_slots(reader, command);
		command->has_picture = chain.has_picture;
		command->picture = chain.picture;
		command->slice_count = chain.slice_count;
		command->slices_overflow = chain.slices_overflow;
		break;
	}

	/* Refuses a malformed record. */
	chain_free(&chain);
	if (reader->error != 0)
		return EINVAL;

	/* Retains the session and parameters of a begin, the bitstream and output of a decode. */
	error = retain_command(session, command, session_id, parameters_id, buffer_id, command->destination.view, 0);
	if (error != 0)
		return error;

	/* A decode needs its H.265 picture. */
	if (opcode == GPU_OP_CMD_DECODE_VIDEO && !command->has_picture)
		return EINVAL;

	/* Succeeded: the command is decoded and its objects retained. */
	return 0;
}

/* Finds and retains each object a recorded command names; zero identities name nothing. */
static int
retain_command(
	struct bcm2711_vulkan_session *session,
	struct video_command *command,
	uint64_t session_id,
	uint64_t parameters_id,
	uint64_t buffer_id,
	uint64_t view_id,
	uint64_t pool_id)
{
	struct bcm2711_vulkan_object *object;
	int error;

	/* The session of a begin. */
	if (session_id != 0) {
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION, session_id);
		if (object == NULL)
			return EINVAL;
		error = bcm2711_vulkan_object_retain(object);
		if (error != 0)
			return error;
		command->session = object;
	}

	/* The parameters object the scope uses. */
	if (parameters_id != 0) {
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, parameters_id);
		if (object == NULL)
			return EINVAL;
		error = bcm2711_vulkan_object_retain(object);
		if (error != 0)
			return error;
		command->parameters = object;
	}

	/* The bitstream buffer of a decode. */
	if (buffer_id != 0) {
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_BUFFER, buffer_id);
		if (object == NULL)
			return EINVAL;
		error = bcm2711_vulkan_object_retain(object);
		if (error != 0)
			return error;
		command->buffer = object;
	}

	/* The output picture's view of a decode. */
	if (view_id != 0) {
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_IMAGE_VIEW, view_id);
		if (object == NULL)
			return EINVAL;
		error = bcm2711_vulkan_object_retain(object);
		if (error != 0)
			return error;
		command->view = object;
	}

	/* The query pool of a query record. */
	if (pool_id != 0) {
		object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUERY_POOL, pool_id);
		if (object == NULL)
			return EINVAL;
		error = bcm2711_vulkan_object_retain(object);
		if (error != 0)
			return error;
		command->pool = object;
	}

	/* A begin needs a session. */
	if (command->record.opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING && command->session == NULL)
		return EINVAL;

	/* Succeeded: every named object is retained. */
	return 0;
}

/* Releases what a recorded command retained, then the node. */
static int
release_command(
	struct bcm2711_vulkan_command_node *node)
{
	struct video_command *command;
	int error;
	int released;

	/* A decode still in the block keeps its objects until the block lets go. */
	command = (struct video_command *)node;
	if (video_running.command == command)
		wait_idle();
	if (video_running.command == command) {
		kern_logf("v3d: video: the decoder kept a decode's objects; they stay for the boot\n");
		return 0;
	}

	/* Each retained object, keeping the first failure. */
	error = 0;
	if (command->session != NULL)
		error = bcm2711_vulkan_object_release(command->session);
	if (command->parameters != NULL) {
		released = bcm2711_vulkan_object_release(command->parameters);
		if (error == 0)
			error = released;
	}

	/* The bitstream buffer. */
	if (command->buffer != NULL) {
		released = bcm2711_vulkan_object_release(command->buffer);
		if (error == 0)
			error = released;
	}

	/* The output picture's view. */
	if (command->view != NULL) {
		released = bcm2711_vulkan_object_release(command->view);
		if (error == 0)
			error = released;
	}

	/* The query pool. */
	if (command->pool != NULL) {
		released = bcm2711_vulkan_object_release(command->pool);
		if (error == 0)
			error = released;
	}

	/* Frees the node. */
	kern_free(command);
	if (error != 0)
		return error;

	/* Succeeded: the node and its edges are gone. */
	return 0;
}

/* Finds the begin of the coding scope open at the end of a recording, or NULL. */
static const struct video_command *
open_scope(
	const struct bcm2711_vulkan_command_buffer *command)
{
	const struct bcm2711_vulkan_command_node *node;
	const struct bcm2711_vulkan_record *record;
	const struct video_command *begin;

	/* The last begin, cleared by any end after it. */
	begin = NULL;
	for (node = command->first; node != NULL; node = node->next) {
		record = (const struct bcm2711_vulkan_record *)node;
		if (record->opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING)
			begin = (const struct video_command *)record;
		if (record->opcode == GPU_OP_CMD_END_VIDEO_CODING)
			begin = NULL;
	}

	/* Succeeded: the open scope's begin, if any. */
	return begin;
}

/* Finds the begin query open at the end of a recording (not ended since), or NULL. */
static const struct video_command *
open_query(
	const struct bcm2711_vulkan_command_buffer *command)
{
	const struct bcm2711_vulkan_command_node *node;
	const struct bcm2711_vulkan_record *record;
	const struct video_command *query;
	const struct video_command *ended;

	/* The last begin query, cleared by an end query of the same pool and query. */
	query = NULL;
	for (node = command->first; node != NULL; node = node->next) {
		record = (const struct bcm2711_vulkan_record *)node;
		if (record->opcode == GPU_OP_CMD_BEGIN_QUERY)
			query = (const struct video_command *)record;
		if (record->opcode != GPU_OP_CMD_END_QUERY || query == NULL)
			continue;
		ended = (const struct video_command *)record;
		if (ended->pool == query->pool && ended->query == query->query)
			query = NULL;
	}

	/* Succeeded: the open begin query, if any. */
	return query;
}

/*
 * Reads a chain: an absent link ends it; a present one carries a structure
 * whose own chain comes before its fields.
 */
static void
read_chain(
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
	read_chain(reader, chain, depth + 1U);
	read_fields(reader, type, chain, depth);
}

/* Reads the fields of one chained structure into the chain's record. */
static void
read_fields(
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

	/* Reads each chained type the library writes; any other fails the stream. */
	switch (type) {
	case VIDEO_TYPE_H265_PROFILE:
		/* The H.265 profile. */
		chain->has_h265_profile = true;
		chain->profile_idc = drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_DECODE_USAGE:
		/* Usage hints, which the decoder does not use. */
		(void)drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_PROFILE_LIST:
		/* [count][array]{profile}: every profile must be H.265 decode; the first refusal is kept. */
		chain->has_profile_list = true;
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		chain->profile_count = (uint32_t)count;
		chain->profiles_supported = true;
		for (item = 0U; item < count && reader->error == 0; item++) {
			result = read_profile(reader, &value);
			if (result != 0 && chain->profiles_supported) {
				chain->profiles_supported = false;
				chain->profile_error = result;
			}
		}

		break;
	case VIDEO_TYPE_H265_PARAMETERS_CREATE:
		/* The capacity of each kind of set, then the add record the create carries, if any. */
		chain->has_h265_create = true;
		chain->max_vps = drv_i915_wire_read_u32(reader);
		chain->max_sps = drv_i915_wire_read_u32(reader);
		chain->max_pps = drv_i915_wire_read_u32(reader);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			value = drv_i915_wire_read_u32(reader);
			if (value != VIDEO_TYPE_H265_PARAMETERS_ADD)
				reader->error = 1;
			kern_memset(&nested, 0, sizeof(nested));
			read_chain(reader, &nested, depth + 1U);
			chain_free(&nested);
			read_add(reader, chain);
		}

		break;
	case VIDEO_TYPE_H265_PARAMETERS_ADD:
		/* The sets added. */
		read_add(reader, chain);
		break;
	case VIDEO_TYPE_H265_DPB_SLOT:
		/* [present]{flags, PicOrderCntVal}: the reference's H.265 information. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_reference = true;
			chain->reference_flags = drv_i915_wire_read_u32(reader);
			chain->reference_poc = (int32_t)drv_i915_wire_read_u32(reader);
		}

		break;
	case VIDEO_TYPE_H265_PICTURE:
		/* [present]{picture}[slice segment count][array]{offset}. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_picture = true;
			read_picture(reader, &chain->picture);
		}
		read_offsets(reader, chain);
		break;
	default:
		/* A structure the library does not write here. */
		reader->error = 1;
		break;
	}
}

/*
 * Reads a profile structure: its type, its chain and its four fields.
 * Returns 0 for a profile the decoder takes, or the standard reason another
 * is refused; the H.265 profile it names goes to profile_idc.
 */
static int32_t
read_profile(
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

	/* The type, then the chain that carries the H.265 profile, then the fields. */
	kern_memset(&chain, 0, sizeof(chain));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_PROFILE)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	operation = drv_i915_wire_read_u32(reader);
	chroma = drv_i915_wire_read_u32(reader);
	luma = drv_i915_wire_read_u32(reader);
	chroma_depth = drv_i915_wire_read_u32(reader);
	chain_free(&chain);

	/* Decides the profile. */
	result = profile_result(operation, chroma, luma, chroma_depth, &chain);
	*profile_idc = chain.profile_idc;

	/* Succeeded: reports the profile's result. */
	return result;
}

/* Decides whether a profile is one the decoder takes, or why not, in the standard order. */
static int32_t
profile_result(
	uint32_t operation,
	uint32_t chroma,
	uint32_t luma,
	uint32_t chroma_depth,
	const struct video_chain *chain)
{
	/* H.265 decode is the one operation. */
	if (operation != VIDEO_OP_DECODE_H265)
		return VIDEO_ERROR_OPERATION;

	/* 8-bit 4:2:0 is the one format. */
	if (chroma != VIDEO_CHROMA_420 || luma != VIDEO_DEPTH_8 || chroma_depth != VIDEO_DEPTH_8)
		return VIDEO_ERROR_FORMAT;

	/* The H.265 record must be there and name Main. */
	if (!chain->has_h265_profile || chain->profile_idc != VIDEO_PROFILE_MAIN)
		return VIDEO_ERROR_CODEC;

	/* Succeeded: the profile is supported. */
	return 0;
}

/* Reads a picture resource structure: [type][chain 0][x][y][width][height][layer][view]. */
static void
read_resource(
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
	read_chain(reader, &chain, 0U);
	chain_free(&chain);

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
read_slot(
	struct i915_wire_reader *reader,
	struct video_slot *slot,
	struct video_resource *resource)
{
	struct video_chain chain;
	uint64_t present;
	uint32_t type;

	/* The type and the chain, which carries the H.265 reference information. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(slot, 0, sizeof(*slot));
	kern_memset(resource, 0, sizeof(*resource));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_REFERENCE_SLOT)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	chain_free(&chain);
	slot->has_reference = chain.has_reference;
	slot->flags = chain.reference_flags;
	slot->poc = chain.reference_poc;

	/* The index and the picture, whose view the slot's own picture stands in for. */
	slot->index = (int32_t)drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_resource(reader, resource);
}

/* Reads an array of slots into the command, up to the DPB's slots (more fails the stream). */
static void
read_slots(
	struct i915_wire_reader *reader,
	struct video_command *command)
{
	struct video_resource resource;
	uint64_t total;
	uint64_t item;

	/* [count][array]: a count past the bytes left, or past the slots, is malformed. */
	(void)drv_i915_wire_read_u32(reader);
	total = drv_i915_wire_read_u64(reader);
	if (total > (uint64_t)(reader->size - reader->offset) || total > VIDEO_MAX_DPB_SLOTS)
		reader->error = 1;

	/* Each slot in order. */
	command->slot_count = 0U;
	for (item = 0U; item < total && reader->error == 0; item++) {
		read_slot(reader, &command->slots[command->slot_count], &resource);
		command->slot_count++;
	}
}

/* Reads an extension name and version: [256][name bytes][version]. */
static void
read_extension(
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

/* Reads the slice segment offsets: [count][array]; more than the table holds marks the decode for refusal. */
static void
read_offsets(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	uint64_t count;
	uint64_t item;
	uint32_t value;

	/* The count, which the bytes left must hold. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;

	/* Each offset, the ones past the table only counted as too many. */
	chain->slice_count = 0U;
	for (item = 0U; item < count && reader->error == 0; item++) {
		value = drv_i915_wire_read_u32(reader);
		if (chain->slices != NULL && item < BCM2712_HEVC_SLICES) {
			chain->slices[item] = value;
			chain->slice_count++;
		} else {
			chain->slices_overflow = true;
		}
	}
}

/*
 * Reads the H.265 information of the picture a decode writes: [flags]
 * [vps][sps][pps][NumDeltaPocsOfRefRpsIdx][PicOrderCntVal]
 * [NumBitsForSTRefPicSetInSlice][reserved] and the three sets of eight
 * slots (libvulkan's video_encode_h265_std_picture).
 */
static void
read_picture(
	struct i915_wire_reader *reader,
	struct bcm2712_hevc_picture *picture)
{
	/* The flags and the sets the picture names. */
	picture->flags = drv_i915_wire_read_u32(reader);
	picture->vps_id = drv_i915_wire_read_u32(reader);
	picture->sps_id = drv_i915_wire_read_u32(reader);
	picture->pps_id = drv_i915_wire_read_u32(reader);

	/* The header's own short-term set's size, and the order count. */
	picture->delta_pocs = drv_i915_wire_read_u32(reader);
	picture->poc = (int32_t)drv_i915_wire_read_u32(reader);
	picture->short_term_bits = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* The slots of the three current sets. */
	read_fixed(reader, picture->before, BCM2712_HEVC_SET_ENTRIES);
	read_fixed(reader, picture->after, BCM2712_HEVC_SET_ENTRIES);
	read_fixed(reader, picture->long_term, BCM2712_HEVC_SET_ENTRIES);
}

/*
 * Reads an H.265 add record's fields: [count][array]{VPS}[count][array]
 * {SPS}[count][array]{PPS} (libvulkan's video_encode_h265_parameters_add).
 * The video sets are kept as their ids, the others as new allocations on
 * the chain; a set the executor cannot keep marks add_error.
 */
static void
read_add(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	static struct video_sps_node scratch_sps;
	static struct video_pps_node scratch_pps;
	struct video_sps_node *sps;
	struct video_pps_node *pps;
	uint64_t count;
	uint64_t item;
	uint32_t id;

	/* The video parameter sets: only their ids are kept. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		id = read_vps(reader);
		if (id >= VIDEO_VPS_IDS) {
			chain->add_error = EINVAL;
			continue;
		}
		chain->new_vps |= 1U << id;
	}

	/* The sequence parameter sets; one that cannot be allocated is read into scratch. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		sps = kern_calloc(1U, sizeof(*sps));
		if (sps == NULL) {
			kern_memset(&scratch_sps, 0, sizeof(scratch_sps));
			sps = &scratch_sps;
			chain->add_error = ENOMEM;
		}

		/* The set; ids outside H.265 mark the add malformed. */
		read_sps(reader, &sps->sps);
		if (sps->sps.vps_id >= VIDEO_VPS_IDS || sps->sps.id >= VIDEO_SPS_IDS)
			chain->add_error = EINVAL;
		if (sps != &scratch_sps) {
			sps->next = chain->new_sps;
			chain->new_sps = sps;
		}
	}

	/* The picture parameter sets; one that cannot be allocated is read into scratch. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		pps = kern_calloc(1U, sizeof(*pps));
		if (pps == NULL) {
			kern_memset(&scratch_pps, 0, sizeof(scratch_pps));
			pps = &scratch_pps;
			chain->add_error = ENOMEM;
		}

		/* The set; ids outside H.265 mark the add malformed. */
		read_pps(reader, &pps->pps);
		if (pps->pps.vps_id >= VIDEO_VPS_IDS || pps->pps.sps_id >= VIDEO_SPS_IDS || pps->pps.id >= VIDEO_PPS_IDS)
			chain->add_error = EINVAL;
		if (pps != &scratch_pps) {
			pps->next = chain->new_pps;
			chain->new_pps = pps;
		}
	}
}

/*
 * Reads one H.265 video parameter set (libvulkan's video_encode_h265_vps):
 * [flags][id][max sub-layers minus 1][reserved x2][timing x3][reserved]
 * [present]{DPB}[present]{HRD}[present]{PTL}.  Reports its id.
 */
static uint32_t
read_vps(
	struct i915_wire_reader *reader)
{
	uint64_t present;
	uint32_t id;
	uint32_t sub_layers;

	/* The flags, the id and the sub-layers. */
	(void)drv_i915_wire_read_u32(reader);
	id = drv_i915_wire_read_u32(reader);
	sub_layers = drv_i915_wire_read_u32(reader) + 1U;
	if (sub_layers > VIDEO_SUB_LAYERS)
		reader->error = 1;

	/* The reserved words and the timing. */
	skip_words(reader, 6U);

	/* The buffering limits, the HRD and the profile, tier and level. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_dpb(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_hrd(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		skip_words(reader, 3U);

	/* Succeeded: the set's id. */
	return id;
}

/*
 * Reads one H.265 sequence parameter set (libvulkan's
 * video_encode_h265_sps): its fields in declared order, then the
 * structures it points to.  The decode keeps its sizes, tools, reference
 * set counts and scaling lists.
 */
static void
read_sps(
	struct i915_wire_reader *reader,
	struct bcm2712_hevc_sps *sps)
{
	uint64_t present;
	uint64_t count;
	uint64_t item;
	uint32_t sub_layers;

	/* The flags, the sampling and the size. */
	sps->flags = drv_i915_wire_read_u32(reader);
	sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
	sps->width = drv_i915_wire_read_u32(reader);
	sps->height = drv_i915_wire_read_u32(reader);

	/* The key, the sub-layers and the depths. */
	sps->vps_id = drv_i915_wire_read_u32(reader);
	sub_layers = drv_i915_wire_read_u32(reader) + 1U;
	if (sub_layers > VIDEO_SUB_LAYERS)
		reader->error = 1;
	sps->id = drv_i915_wire_read_u32(reader);
	sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
	sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);

	/* The order count's size and the block sizes. */
	sps->log2_max_poc_lsb_minus4 = drv_i915_wire_read_u32(reader);
	sps->log2_min_cb_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_cb = drv_i915_wire_read_u32(reader);
	sps->log2_min_tb_minus2 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_tb = drv_i915_wire_read_u32(reader);
	sps->depth_inter = drv_i915_wire_read_u32(reader);
	sps->depth_intra = drv_i915_wire_read_u32(reader);

	/* The reference picture sets the sequence signals. */
	sps->short_term_sets = drv_i915_wire_read_u32(reader);
	sps->long_term_sps = drv_i915_wire_read_u32(reader);

	/* The PCM depths and sizes, two reserved words. */
	sps->pcm_depth_luma_minus1 = drv_i915_wire_read_u32(reader);
	sps->pcm_depth_chroma_minus1 = drv_i915_wire_read_u32(reader);
	sps->log2_min_pcm_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_pcm = drv_i915_wire_read_u32(reader);
	skip_words(reader, 2U);

	/* The screen content sizes and the conformance window, which the decode does not use. */
	skip_words(reader, 8U);

	/* The profile, tier and level, and the buffering limits. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		skip_words(reader, 3U);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_dpb(reader);

	/* The scaling lists in use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_scaling = true;
		read_scaling(reader, &sps->scaling);
	}

	/* The short-term sets: as many as the sequence counts. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		count = drv_i915_wire_read_u64(reader);
		if (count != sps->short_term_sets || count > VIDEO_SHORT_TERM_SETS)
			reader->error = 1;
		for (item = 0U; item < count && reader->error == 0; item++) {
			skip_words(reader, 12U);
			read_words(reader, NULL, VIDEO_SET_PICTURES);
			read_words(reader, NULL, VIDEO_SET_PICTURES);
		}
	}

	/* The long-term pictures, the VUI and the palette's initializers. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		skip_words(reader, 1U);
		read_words(reader, NULL, VIDEO_LONG_TERM_PICS);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		skip_words(reader, 25U);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U)
			read_hrd(reader, sub_layers);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_words(reader, NULL, VIDEO_PALETTE_ENTRIES);
}

/*
 * Reads one H.265 picture parameter set (libvulkan's
 * video_encode_h265_pps): its fields in declared order, then the
 * structures it points to.
 */
static void
read_pps(
	struct i915_wire_reader *reader,
	struct bcm2712_hevc_pps *pps)
{
	uint8_t offsets[VIDEO_CHROMA_OFFSETS];
	uint64_t present;

	/* The flags and the key. */
	pps->flags = drv_i915_wire_read_u32(reader);
	pps->id = drv_i915_wire_read_u32(reader);
	pps->sps_id = drv_i915_wire_read_u32(reader);
	pps->vps_id = drv_i915_wire_read_u32(reader);

	/* The slice header's extra bits and the default reference counts. */
	pps->extra_bits = drv_i915_wire_read_u32(reader);
	pps->l0_minus1 = drv_i915_wire_read_u32(reader);
	pps->l1_minus1 = drv_i915_wire_read_u32(reader);

	/* The quantizer, the deblocking offsets and the merge level, the signed ones widened with their sign. */
	pps->init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->cu_qp_delta_depth = drv_i915_wire_read_u32(reader);
	pps->cb_qp_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->cr_qp_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->beta_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->tc_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->log2_merge_minus2 = drv_i915_wire_read_u32(reader);

	/* The range extension's fields and chroma offset lists, which Main does not use. */
	skip_words(reader, 3U);
	read_fixed(reader, offsets, VIDEO_CHROMA_OFFSETS);
	read_fixed(reader, offsets, VIDEO_CHROMA_OFFSETS);
	skip_words(reader, 2U);

	/* The screen content fields, which Main does not use. */
	skip_words(reader, 6U);

	/* The tiles: their counts, two reserved words, each column's width and row's height minus 1, a reserved word. */
	pps->tile_columns_minus1 = drv_i915_wire_read_u32(reader);
	pps->tile_rows_minus1 = drv_i915_wire_read_u32(reader);
	skip_words(reader, 2U);
	read_words(reader, pps->column_width_minus1, BCM2712_HEVC_COLUMN_WIDTHS);
	read_words(reader, pps->row_height_minus1, BCM2712_HEVC_ROW_HEIGHTS);
	skip_words(reader, 1U);

	/* The picture's own scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		pps->has_scaling = true;
		read_scaling(reader, &pps->scaling);
	}

	/* The palette's initializers, which Main does not use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_words(reader, NULL, VIDEO_PALETTE_ENTRIES);
}

/* Reads H.265 buffering limits: [7]{latency}[7 bytes]{buffering}[7 bytes]{reordering}. */
static void
read_dpb(
	struct i915_wire_reader *reader)
{
	uint8_t bytes[VIDEO_SUB_LAYERS];

	/* The latency words, then the two byte arrays. */
	read_words(reader, NULL, VIDEO_SUB_LAYERS);
	read_fixed(reader, bytes, VIDEO_SUB_LAYERS);
	read_fixed(reader, bytes, VIDEO_SUB_LAYERS);
}

/*
 * Reads H.265 HRD parameters (libvulkan's video_encode_h265_hrd): the
 * flags and lengths, the buffer counts, the durations, the reserved words,
 * then the NAL and VCL sub-layer arrays.
 */
static void
read_hrd(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint8_t counts[VIDEO_SUB_LAYERS];
	uint64_t present;

	/* The flags and the masks, the lengths and the scales. */
	skip_words(reader, 13U);

	/* The buffer counts, the durations and the reserved words. */
	read_fixed(reader, counts, VIDEO_SUB_LAYERS);
	read_words(reader, NULL, VIDEO_SUB_LAYERS);
	read_words(reader, NULL, 3U);

	/* The NAL and the VCL sub-layers. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_sub_layers(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_sub_layers(reader, sub_layers);
}

/* Reads an array of H.265 sub-layer HRD parameters: [count]{four arrays of 32 words and the CBR mask}. */
static void
read_sub_layers(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint64_t count;
	uint64_t item;

	/* One entry for each sub-layer. */
	count = drv_i915_wire_read_u64(reader);
	if (count != sub_layers)
		reader->error = 1;

	/* Each entry's rates and sizes, then its CBR mask. */
	for (item = 0U; item < count && reader->error == 0; item++) {
		read_words(reader, NULL, VIDEO_CPB_COUNT);
		read_words(reader, NULL, VIDEO_CPB_COUNT);
		read_words(reader, NULL, VIDEO_CPB_COUNT);
		read_words(reader, NULL, VIDEO_CPB_COUNT);
		skip_words(reader, 1U);
	}
}

/* Reads H.265 scaling lists: the four sizes' lists and the two DC arrays, each a byte array. */
static void
read_scaling(
	struct i915_wire_reader *reader,
	struct bcm2712_hevc_scaling *scaling)
{
	/* The lists, list after list, then the DC values. */
	read_fixed(reader, scaling->list4, sizeof(scaling->list4));
	read_fixed(reader, scaling->list8, sizeof(scaling->list8));
	read_fixed(reader, scaling->list16, sizeof(scaling->list16));
	read_fixed(reader, scaling->list32, sizeof(scaling->list32));
	read_fixed(reader, scaling->dc16, sizeof(scaling->dc16));
	read_fixed(reader, scaling->dc32, sizeof(scaling->dc32));
}

/* Reads an array of words that must have expected elements: [count][words]; into words when not NULL. */
static void
read_words(
	struct i915_wire_reader *reader,
	uint32_t *words,
	uint32_t expected)
{
	uint64_t count;
	uint32_t index;
	uint32_t value;

	/* The count must be the array's. */
	count = drv_i915_wire_read_u64(reader);
	if (count != expected) {
		reader->error = 1;
		return;
	}

	/* Each word. */
	for (index = 0U; index < expected; index++) {
		value = drv_i915_wire_read_u32(reader);
		if (words != NULL)
			words[index] = value;
	}
}

/* Reads a byte array that must have expected bytes: [count][bytes]. */
static void
read_fixed(
	struct i915_wire_reader *reader,
	void *bytes,
	uint32_t expected)
{
	uint64_t count;

	/* The count must be the array's. */
	count = drv_i915_wire_read_u64(reader);
	if (count != expected) {
		reader->error = 1;
		return;
	}

	/* The bytes. */
	i915_vkc_read_bytes(reader, bytes, expected);
}

/* Reads and drops count words the decode does not use. */
static void
skip_words(
	struct i915_wire_reader *reader,
	uint32_t count)
{
	uint32_t index;

	/* Each word. */
	for (index = 0U; index < count; index++)
		(void)drv_i915_wire_read_u32(reader);
}

/* Frees the parameter sets a chain read and nobody took. */
static void
chain_free(
	struct video_chain *chain)
{
	struct video_sps_node *sps;
	struct video_pps_node *pps;

	/* The sequence sets, then the picture sets. */
	while (chain->new_sps != NULL) {
		sps = chain->new_sps;
		chain->new_sps = sps->next;
		kern_free(sps);
	}
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;
		kern_free(pps);
	}
}

/*
 * Moves a chain's new sets into a parameters object.
 *
 * With replace, a set replaces the object's of the same key (a create);
 * without, a key already held refuses the whole add (an update) and
 * nothing is taken.  The object's capacity bounds the sets.
 */
static int
parameters_take(
	struct video_parameters *parameters,
	struct video_chain *chain,
	bool replace)
{
	struct video_sps_node *sps;
	struct video_sps_node **sps_link;
	struct video_sps_node *old_sps;
	struct video_pps_node *pps;
	struct video_pps_node **pps_link;
	struct video_pps_node *old_pps;
	uint32_t vps;
	uint32_t vps_count;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t index;

	/* An update must not repeat a key the object holds. */
	if (!replace) {
		if ((chain->new_vps & parameters->vps) != 0U)
			return EINVAL;
		for (sps = chain->new_sps; sps != NULL; sps = sps->next) {
			if (find_sps(parameters, sps->sps.vps_id, sps->sps.id) != NULL)
				return EINVAL;
		}
		for (pps = chain->new_pps; pps != NULL; pps = pps->next) {
			if (find_pps(parameters, pps->pps.vps_id, pps->pps.sps_id, pps->pps.id) != NULL)
				return EINVAL;
		}
	}

	/* The video sets held after the add. */
	vps = parameters->vps | chain->new_vps;
	vps_count = 0U;
	for (index = 0U; index < VIDEO_VPS_IDS; index++) {
		if ((vps & (1U << index)) != 0U)
			vps_count++;
	}

	/* The sequence sets held, and the new ones of new keys. */
	sps_count = 0U;
	for (sps = parameters->sps; sps != NULL; sps = sps->next)
		sps_count++;
	for (sps = chain->new_sps; sps != NULL; sps = sps->next) {
		if (find_sps(parameters, sps->sps.vps_id, sps->sps.id) == NULL)
			sps_count++;
	}

	/* The picture sets held, and the new ones of new keys. */
	pps_count = 0U;
	for (pps = parameters->pps; pps != NULL; pps = pps->next)
		pps_count++;
	for (pps = chain->new_pps; pps != NULL; pps = pps->next) {
		if (find_pps(parameters, pps->pps.vps_id, pps->pps.sps_id, pps->pps.id) == NULL)
			pps_count++;
	}

	/* Refuses sets beyond the object's capacity. */
	if (vps_count > parameters->max_vps || sps_count > parameters->max_sps || pps_count > parameters->max_pps)
		return EINVAL;

	/* The video sets' ids. */
	parameters->vps = vps;
	chain->new_vps = 0U;

	/* The sequence sets, each replacing any of its key. */
	while (chain->new_sps != NULL) {
		sps = chain->new_sps;
		chain->new_sps = sps->next;
		sps_link = &parameters->sps;
		while (*sps_link != NULL) {
			if ((*sps_link)->sps.vps_id == sps->sps.vps_id && (*sps_link)->sps.id == sps->sps.id) {
				old_sps = *sps_link;
				*sps_link = old_sps->next;
				kern_free(old_sps);
				continue;
			}
			sps_link = &(*sps_link)->next;
		}
		sps->next = parameters->sps;
		parameters->sps = sps;
	}

	/* The picture sets, each replacing any of its key. */
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;
		pps_link = &parameters->pps;
		while (*pps_link != NULL) {
			if ((*pps_link)->pps.vps_id == pps->pps.vps_id &&
			    (*pps_link)->pps.sps_id == pps->pps.sps_id &&
			    (*pps_link)->pps.id == pps->pps.id) {
				old_pps = *pps_link;
				*pps_link = old_pps->next;
				kern_free(old_pps);
				continue;
			}
			pps_link = &(*pps_link)->next;
		}
		pps->next = parameters->pps;
		parameters->pps = pps;
	}

	/* Succeeded: the sets are the object's. */
	return 0;
}

/* Copies a template's sets into a new parameters object. */
static int
parameters_copy(
	struct video_parameters *destination,
	const struct video_parameters *source)
{
	const struct video_sps_node *sps;
	const struct video_pps_node *pps;
	struct video_sps_node *sps_copy;
	struct video_pps_node *pps_copy;

	/* The video sets' ids, then the sequence sets. */
	destination->vps = source->vps;
	for (sps = source->sps; sps != NULL; sps = sps->next) {
		sps_copy = kern_calloc(1U, sizeof(*sps_copy));
		if (sps_copy == NULL)
			return ENOMEM;
		sps_copy->sps = sps->sps;
		sps_copy->next = destination->sps;
		destination->sps = sps_copy;
	}

	/* The picture sets. */
	for (pps = source->pps; pps != NULL; pps = pps->next) {
		pps_copy = kern_calloc(1U, sizeof(*pps_copy));
		if (pps_copy == NULL)
			return ENOMEM;
		pps_copy->pps = pps->pps;
		pps_copy->next = destination->pps;
		destination->pps = pps_copy;
	}

	/* Succeeded: the template's sets are copied. */
	return 0;
}

/* Frees a parameters object's sets. */
static void
parameters_free(
	struct video_parameters *parameters)
{
	struct video_sps_node *sps;
	struct video_pps_node *pps;

	/* The sequence sets, then the picture sets. */
	while (parameters->sps != NULL) {
		sps = parameters->sps;
		parameters->sps = sps->next;
		kern_free(sps);
	}
	while (parameters->pps != NULL) {
		pps = parameters->pps;
		parameters->pps = pps->next;
		kern_free(pps);
	}
	parameters->vps = 0U;
}

/* Finds the sequence set of a key in a parameters object. */
static const struct bcm2712_hevc_sps *
find_sps(
	const struct video_parameters *parameters,
	uint32_t vps_id,
	uint32_t sps_id)
{
	const struct video_sps_node *sps;

	/* Walks the list for the key. */
	for (sps = parameters->sps; sps != NULL; sps = sps->next) {
		if (sps->sps.vps_id == vps_id && sps->sps.id == sps_id)
			return &sps->sps;
	}

	/* No set of the key. */
	return NULL;
}

/* Finds the picture set of a key in a parameters object. */
static const struct bcm2712_hevc_pps *
find_pps(
	const struct video_parameters *parameters,
	uint32_t vps_id,
	uint32_t sps_id,
	uint32_t pps_id)
{
	const struct video_pps_node *pps;

	/* Walks the list for the key. */
	for (pps = parameters->pps; pps != NULL; pps = pps->next) {
		if (pps->pps.vps_id == vps_id && pps->pps.sps_id == sps_id && pps->pps.id == pps_id)
			return &pps->pps;
	}

	/* No set of the key. */
	return NULL;
}

/*
 * vkCreateQueryPool: [device][present][type][chain 0][flags][query type]
 * [count][statistics][allocator][present][identity] -> [result][present][pool].
 * Only a result status pool of at most VIDEO_QUERY_MAX queries is made.
 */
static int
pool_create(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *device;
	struct bcm2711_vulkan_object *object;
	struct video_pool *pool;
	uint64_t device_id;
	uint64_t identity;
	uint32_t type;
	uint32_t count;
	int error;
	int released;

	/* Decodes the type, the count and the identity. */
	device_id = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	device = bcm2711_vulkan_object_find(session, I915_VK_OBJ_DEVICE, device_id);
	if (reader->error != 0 || device == NULL || identity == 0)
		return EINVAL;

	/* Only a result status pool of a bounded size. */
	if (type != VIDEO_QUERY_RESULT_STATUS || count == 0U || count > VIDEO_QUERY_MAX) {
		kern_logf("v3d: vk: vkCreateQueryPool refused: type %u, %u queries (result status only)\n", (unsigned)type, (unsigned)count);
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_FEATURE_NOT_PRESENT);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* The pool, every query unavailable, keeping its device. */
	pool = kern_calloc(1U, sizeof(*pool));
	if (pool == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}
	pool->count = count;
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(pool);
		return error;
	}
	pool->owner.device = device;

	/* Publishes the pool under its identity. */
	error = bcm2711_vulkan_object_publish(session, I915_VK_OBJ_QUERY_POOL, identity, pool, release_pool, &object);
	if (error != 0) {
		released = release_pool(session, pool);
		if (released != 0)
			return released;
		if (error != ENOMEM)
			return error;
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* Replies VK_SUCCESS and the identity. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u64(reply, identity);

	/* Succeeded: the pool exists. */
	return 0;
}

/*
 * vkGetQueryPoolResults: [device][pool][first][count][dataSize][bytes]
 * [stride][flags] -> [result][bytes]{bytes}.  Each query's record is its
 * status and its availability, in words of 4 or 8 bytes, stride apart.
 */
static int
pool_results(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct video_pool *pool;
	uint64_t identity;
	uint64_t bytes;
	uint64_t stride;
	uint64_t record;
	uint64_t written;
	uint64_t end;
	uint64_t value;
	uint64_t available;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t width;
	uint32_t query;
	uint32_t result;
	uint8_t data[16];
	uint8_t zeros[64];

	/* Decodes the range, the size, the stride and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	bytes = drv_i915_wire_read_u64(reader);
	stride = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A record is one or two words of four or eight bytes. */
	width = 4U;
	if ((flags & VIDEO_QUERY_RESULT_64) != 0U)
		width = 8U;
	record = width;
	if ((flags & VIDEO_QUERY_RESULT_AVAILABILITY) != 0U)
		record += width;

	/* Refuses an unknown pool, a range past it, and records that do not fit the reply. */
	pool = NULL;
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_QUERY_POOL, identity);
	if (object != NULL)
		pool = object->payload;
	if (pool == NULL || first > pool->count || count > pool->count - first ||
	    bytes > VIDEO_QUERY_REPLY_MAX || stride < record || stride > bytes ||
	    (count != 0U && (uint64_t)(count - 1U) * stride + record > bytes)) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_DEVICE_LOST);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* NOT_READY when any query of the range is unavailable. */
	result = 0U;
	for (query = first; query < first + count; query++) {
		if (pool->available[query] == 0)
			result = (uint32_t)VK_NOT_READY;
	}

	/* The result, then the bytes the records take. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, bytes);

	/* Each record at its stride, then zeros to the next record and to the end. */
	written = 0U;
	kern_memset(zeros, 0, sizeof(zeros));
	for (query = 0U; query < count; query++) {
		value = (uint64_t)(int64_t)pool->status[first + query];
		available = pool->available[first + query];
		kern_memset(data, 0, sizeof(data));
		kern_memcpy(data, &value, width);
		kern_memcpy(data + width, &available, width);
		drv_i915_wire_reply_bytes(reply, data, (size_t)record);
		written += record;
		end = written - record + stride;
		if (end > bytes)
			end = bytes;
		while (written < end) {
			value = end - written;
			if (value > sizeof(zeros))
				value = sizeof(zeros);
			drv_i915_wire_reply_bytes(reply, zeros, (size_t)value);
			written += value;
		}
	}
	while (written < bytes) {
		value = bytes - written;
		if (value > sizeof(zeros))
			value = sizeof(zeros);
		drv_i915_wire_reply_bytes(reply, zeros, (size_t)value);
		written += value;
	}

	/* Succeeded: the records are written. */
	return 0;
}

/*
 * Records a query command: vkCmdBeginQuery [cmd][pool][query][flags],
 * vkCmdEndQuery [cmd][pool][query], vkCmdResetQueryPool [cmd][pool][first]
 * [count].  A begin or end of a status query belongs to a coding scope.
 */
static int
record_query(
	struct bcm2711_vulkan_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_command_buffer *command;
	struct video_command *query;
	struct video_pool *pool;
	uint64_t identity;
	uint64_t pool_id;
	int error;
	int released;

	/* The recording primary and the command's fields. */
	identity = drv_i915_wire_read_u64(reader);
	pool_id = drv_i915_wire_read_u64(reader);
	query = kern_calloc(1U, sizeof(*query));
	if (query == NULL)
		return ENOMEM;
	query->record.opcode = opcode;
	query->record.node.release = release_command;
	query->query = drv_i915_wire_read_u32(reader);
	query->query_count = 1U;
	if (opcode == GPU_OP_CMD_BEGIN_QUERY)
		(void)drv_i915_wire_read_u32(reader);
	if (opcode == GPU_OP_CMD_RESET_QUERY_POOL)
		query->query_count = drv_i915_wire_read_u32(reader);
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_COMMAND_BUFFER, identity);
	if (reader->error != 0 || object == NULL) {
		kern_free(query);
		return EINVAL;
	}

	/* The command buffer the query is recorded into. */
	command = object->payload;
	if (command->pending != 0 || command->state != BCM2711_VULKAN_COMMAND_RECORDING) {
		kern_free(query);
		return EBUSY;
	}

	/* A command buffer that failed recording takes nothing more. */
	if (command->recording_error != 0) {
		kern_free(query);
		return 0;
	}

	/* The pool, the range within it, and the scope of a begin or end. */
	error = retain_command(session, query, 0, 0, 0, 0, pool_id);
	pool = NULL;
	if (query->pool != NULL)
		pool = query->pool->payload;
	if (error == 0 && (pool == NULL || query->query > pool->count || query->query_count > pool->count - query->query))
		error = EINVAL;
	if (error == 0 && opcode != GPU_OP_CMD_RESET_QUERY_POOL) {
		if (query->query_count != 1U || query->query >= pool->count)
			error = EINVAL;
		query->begin = open_scope(command);
		if (query->begin == NULL)
			error = EINVAL;
	}

	/* A query inside a render pass is not supported. */
	if (error == 0 && command->render_open)
		error = ENOTSUP;
	if (error == 0 && sizeof(*query) > BCM2711_VULKAN_RECORD_BYTES - command->recorded_bytes)
		error = ENOMEM;
	if (error != 0) {
		command->recording_error = error;
		released = release_command(&query->record.node);
		if (released != 0)
			return released;
		return 0;
	}

	/* Appends the node to the primary's ordered list. */
	if (command->last == NULL) {
		command->first = &query->record.node;
	} else {
		command->last->next = &query->record.node;
	}

	/* The record is the command buffer's last. */
	command->last = &query->record.node;
	command->recorded_bytes += sizeof(*query);

	/* Succeeded: the command is recorded. */
	return 0;
}

/*
 * Runs one decode a step at a time.
 *
 * The first step starts the picture in the block (a picture that cannot
 * start is skipped: status ERROR, the image unchanged).  Each step looks
 * at the block with the controller's window open for a short while: phase
 * 1 done starts phase 2, a list that ran out is grown and phase 1 runs
 * again, phase 2 done converts the picture into its image and ends the
 * decode.  EINPROGRESS while the block works; a decode that runs past its
 * time breaks the block for the boot.
 */
static int
run_decode(
	struct bcm2711_render_device *controller,
	const struct video_command *command)
{
	struct video_session *video;
	struct bcm2712_hevc_phase1_buffers phase1;
	enum bcm2712_hevc_result state;
	const char *reason;
	uint64_t limit;
	int error;
	int closed;

	/* The scope's session; a broken session or block decodes nothing more. */
	video = command->begin->session->payload;
	if (video->broken || video_block_lost) {
		finish(command, VIDEO_STATUS_ERROR);
		return 0;
	}

	/* Another decode in the block goes first. */
	if (video_running.command != NULL && video_running.command != command)
		return EINPROGRESS;

	/* A decode's first step starts it, or skips it. */
	if (video_running.command == NULL) {
		reason = start_decode(controller, video, command);
		if (reason != NULL) {
			skip(video, command, reason, 0);
			return 0;
		}
	}

	/* Without the controller mutex: the block, for a short while, unless other sessions wait. */
	state = BCM2712_HEVC_RUNNING;
	bcm2711_render_window_open(controller);
	limit = clock_ticks() + kern_ms_to_ticks(1U);
	if (!controller->worker.contended)
		limit = clock_ticks() + kern_ms_to_ticks(VIDEO_POLL_US / 1000U);
	for (;;) {
		/* The phase's state. */
		if (video_running.phase == 1U) {
			state = bcm2712_hevc_phase1_poll();
			if (state == BCM2712_HEVC_DONE) {
				/* Phase 2 runs on what phase 1 wrote. */
				video_running.phase = 2U;
				bcm2712_hevc_phase2_start(&video_running.buffers);
				state = BCM2712_HEVC_RUNNING;
			}
		} else {
			state = bcm2712_hevc_phase2_poll();
		}

		/* Anything but running ends the look. */
		if (state != BCM2712_HEVC_RUNNING || clock_ticks() >= limit)
			break;
		kern_usleep_range(50U, 100U);
	}
	closed = bcm2711_render_window_close(controller);
	if (closed != 0)
		return closed;

	/* Still running: the next step looks again, until the decode's time is over. */
	if (state == BCM2712_HEVC_RUNNING) {
		if (clock_ticks() - video_running.started < kern_ms_to_ticks(VIDEO_DECODE_MS))
			return EINPROGRESS;
		kern_logf("v3d: video: the decoder did not finish a picture in %u ms (phase %u); no more decodes this boot\n",
			  (unsigned)VIDEO_DECODE_MS, (unsigned)video_running.phase);
		video_block_lost = true;
		video->broken = true;
		finish(command, VIDEO_STATUS_ERROR);
		return 0;
	}

	/* A list that ran out grows, and phase 1 runs again. */
	if (state == BCM2712_HEVC_PU_FULL || state == BCM2712_HEVC_COEFFICIENTS_FULL) {
		error = ENOSPC;
		if (video_running.grows < VIDEO_GROWS) {
			if (state == BCM2712_HEVC_PU_FULL) {
				error = grow_list(&video->pu);
			} else {
				error = grow_list(&video->coefficients);
			}
		}
		if (error != 0) {
			video_running.command = NULL;
			skip(video, command, "the decoder's lists could not grow", error);
			return 0;
		}
		video_running.grows++;
		phase1_buffers(video, video_running.command_count, video_running.phase2.ctb_rows, &phase1);
		video_running.buffers.pu = phase1.pu;
		video_running.buffers.pu_bytes = phase1.pu_bytes;
		video_running.buffers.coefficients = phase1.coefficients;
		video_running.buffers.coefficient_bytes = phase1.coefficient_bytes;
		video_running.phase = 1U;
		bcm2712_hevc_phase1_start(&phase1);
		return EINPROGRESS;
	}

	/* A bitstream phase 1 could not decode. */
	if (state == BCM2712_HEVC_FAILED) {
		video_running.command = NULL;
		skip(video, command, "the decoder stopped on the bitstream", EIO);
		return 0;
	}

	/* Phase 2 done: the picture goes into its image. */
	error = convert(controller, &video_running);
	video_running.command = NULL;
	if (error != 0) {
		skip(video, command, "the picture could not be written into its image", error);
		return 0;
	}

	/* Succeeded: the picture is decoded into its image. */
	finish(command, VIDEO_STATUS_COMPLETE);
	return 0;
}

/*
 * Starts a decode in the block: finds its sets and checks them against the
 * decoder and the session, resolves its pictures and bitstream, reads every
 * segment's header, builds the command list and the phase 2 values, and
 * starts phase 1.  Returns NULL when started, else why the picture is
 * skipped.
 */
static const char *
start_decode(
	struct bcm2711_render_device *controller,
	struct video_session *video,
	const struct video_command *command)
{
	static struct bcm2712_hevc_slot slots[BCM2712_HEVC_SLOTS];
	struct bcm2712_hevc_commands commands;
	struct bcm2712_hevc_phase1_buffers phase1;
	struct bcm2712_hevc_phase2_buffers *buffers;
	const struct video_parameters *parameters;
	const struct bcm2712_hevc_sps *sps;
	const struct bcm2712_hevc_pps *pps;
	const struct bcm2712_hevc_slice *independent;
	struct bcm2711_vulkan_resource *bitstream;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_v3d_view *backing;
	const uint8_t *cpu;
	uint64_t physical;
	uint64_t setup_physical;
	uint64_t chroma;
	uint64_t start;
	uint64_t end;
	uint64_t length;
	uint32_t native;
	uint32_t pitch;
	uint32_t setup;
	uint32_t index;
	uint32_t slot;
	int error;

	(void)controller;

	/* The sets the picture names, which the decoder must take within the session's extent. */
	if (command->begin->parameters == NULL)
		return "no parameters bound";
	parameters = command->begin->parameters->payload;
	sps = find_sps(parameters, command->picture.vps_id, command->picture.sps_id);
	pps = find_pps(parameters, command->picture.vps_id, command->picture.sps_id, command->picture.pps_id);
	if (sps == NULL || pps == NULL)
		return "a parameter set the picture names is missing";
	error = bcm2712_hevc_check(sps, pps);
	if (error != 0)
		return "the stream's parameter sets are outside the decoder";
	if (sps->width > video->width || sps->height > video->height)
		return "the picture is larger than the session";
	if (command->slice_count == 0U || command->slices_overflow)
		return "no slice segments, or more than the decoder takes";

	/* The picture the decode writes: the setup slot's, or the spare. */
	setup = video->pictures - 1U;
	if (command->has_setup) {
		if (command->setup.index < 0 || (uint32_t)command->setup.index >= video->max_dpb_slots)
			return "the setup slot is outside the session";
		setup = (uint32_t)command->setup.index;
	}
	error = picture_address(video, setup, &setup_physical, &native);
	if (error != 0)
		return "the session's memory is not bound";

	/* The output image must be an NV12 decode output with memory. */
	error = image_target(command->view, &image, &native, &pitch, &chroma);
	if (error != 0)
		return "the output picture is not usable";

	/* The references: the slots the decode names, each with its picture. */
	kern_memset(slots, 0, sizeof(slots));
	for (index = 0; index < command->slot_count; index++) {
		if (command->slots[index].index < 0 || (uint32_t)command->slots[index].index >= video->max_dpb_slots)
			return "a reference slot is outside the session";
		slot = (uint32_t)command->slots[index].index;
		if (!command->slots[index].has_reference)
			continue;
		error = picture_address(video, slot, &physical, &native);
		if (error != 0)
			return "the session's memory is not bound";
		slots[slot].present = true;
		slots[slot].poc = command->slots[index].poc;
		slots[slot].long_term = (command->slots[index].flags & BCM2712_HEVC_REFERENCE_LONG_TERM) != 0U;
	}

	/* The bitstream's bytes and bus address. */
	if (command->buffer == NULL || command->range == 0U)
		return "no bitstream";
	bitstream = command->buffer->payload;
	error = bcm2711_vulkan_resource_backing(bitstream, command->offset, command->range, &backing, &native, (void **)&cpu);
	if (error != 0)
		return "the bitstream is not in memory";
	physical = backing->buffer->memory.paddr + (uint64_t)(cpu - (const uint8_t *)backing->buffer->address);

	/* Each segment's header, read from a copy of its first bytes, the dependent ones after their independent one. */
	independent = NULL;
	for (index = 0; index < command->slice_count; index++) {
		/* The segment: from its offset to the next one's, or to the range's end. */
		start = command->slices[index];
		end = command->range;
		if (index + 1U < command->slice_count)
			end = command->slices[index + 1U];
		if (start >= end || end > command->range)
			return "the slice segment offsets are out of order";
		length = end - start;
		if (length > VIDEO_HEADER_BYTES)
			length = VIDEO_HEADER_BYTES;
		kern_memcpy(video_header, cpu + start, (size_t)length);

		/* The header, then the data's bytes in the whole segment and its bus address. */
		error = bcm2712_hevc_slice_read(video_header, (size_t)length, sps, pps, &command->picture, slots, independent, &video_slices[index]);
		if (error != 0)
			return "a slice segment header does not read";
		video_slices[index].data_bytes = (size_t)(end - start) - video_slices[index].data_offset;
		video_slices[index].segment_physical = physical + start;
		if (!video_slices[index].dependent)
			independent = &video_slices[index];
	}

	/* The command list and the phase 2 values. */
	commands.pairs = video->commands->address;
	commands.capacity = VIDEO_COMMAND_PAIRS;
	commands.count = 0;
	commands.overflow = false;
	kern_memset(&video_running, 0, sizeof(video_running));
	error = bcm2712_hevc_program(sps, pps, &command->picture, slots, video_slices, command->slice_count, &commands, &video_running.phase2);
	if (error != 0)
		return "the picture's commands do not fit or do not form a picture";

	/* Phase 2's buffers: the lists, the output, the references (the output itself where none), the motion vectors. */
	buffers = &video_running.buffers;
	buffers->pu = video->pu->memory.paddr;
	buffers->pu_bytes = video->pu->bytes;
	buffers->coefficients = video->coefficients->memory.paddr;
	buffers->coefficient_bytes = video->coefficients->bytes;
	buffers->luma = setup_physical;
	buffers->chroma = setup_physical + video->sand.luma_bytes;
	buffers->luma_stride = video->sand.column_rows * 128U;
	buffers->chroma_stride = video->sand.column_rows * 64U;
	for (slot = 0; slot < BCM2712_HEVC_SLOTS; slot++) {
		buffers->reference_luma[slot] = buffers->luma;
		buffers->reference_chroma[slot] = buffers->chroma;
		if (!slots[slot].present)
			continue;
		(void)picture_address(video, slot, &physical, &native);
		buffers->reference_luma[slot] = physical;
		buffers->reference_chroma[slot] = physical + video->sand.luma_bytes;
	}
	buffers->motion_stride = video->motion_stride;
	buffers->motion_write = 0;
	if (video_running.phase2.write_motion)
		buffers->motion_write = setup_physical + video->sand.luma_bytes + video->sand.chroma_bytes;
	buffers->motion_read = 0;
	slot = video_running.phase2.collocated_slot;
	if (slot < BCM2712_HEVC_SLOTS && slots[slot].present) {
		(void)picture_address(video, slot, &physical, &native);
		buffers->motion_read = physical + video->sand.luma_bytes + video->sand.chroma_bytes;
	}
	buffers->config2 = video_running.phase2.config2;
	buffers->frame_size = video_running.phase2.frame_size;
	buffers->current_poc = video_running.phase2.current_poc;
	buffers->ctb_rows = video_running.phase2.ctb_rows;

	/* The decode is the block's; phase 1 starts. */
	video_running.command = command;
	video_running.video = video;
	video_running.phase = 1U;
	video_running.started = clock_ticks();
	video_running.picture = setup;
	video_running.command_count = commands.count;
	kern_io_write_barrier();
	phase1_buffers(video, commands.count, video_running.phase2.ctb_rows, &phase1);
	bcm2712_hevc_phase1_start(&phase1);

	/* Succeeded: the block decodes the picture. */
	return NULL;
}

/*
 * Reports a session picture's bus address and V3D address, from the memory
 * its binding names.
 */
static int
picture_address(
	const struct video_session *video,
	uint32_t index,
	uint64_t *physical,
	uint32_t *native)
{
	const struct bcm2711_vulkan_memory *memory;
	const struct bcm2711_v3d_view *view;

	/* The binding's memory, mapped and large enough. */
	if (index >= video->pictures || video->picture[index].memory == NULL)
		return EINVAL;
	memory = video->picture[index].memory->payload;
	view = memory->view;
	if (view == NULL || view->quarantined || view->buffer->bytes < video->picture_bytes)
		return EIO;

	/* Succeeded: its start on the bus and in the V3D's space. */
	*physical = view->buffer->memory.paddr;
	*native = view->address;
	return 0;
}

/*
 * Resolves a decode's output image: an NV12 decode output with memory, its
 * V3D address, its pitch and its chroma plane's offset.
 */
static int
image_target(
	const struct bcm2711_vulkan_object *view_object,
	struct bcm2711_vulkan_resource **image,
	uint32_t *native,
	uint32_t *pitch,
	uint64_t *chroma)
{
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *backing;
	uint64_t bytes;
	void *mapped;
	int error;

	/* The view's image must be an NV12 decode output. */
	if (view_object == NULL)
		return EINVAL;
	view = view_object->payload;
	if (view->owner.parent == NULL)
		return EINVAL;
	resource = view->owner.parent->payload;
	if (resource->format != (VkFormat)BCM2711_VIDEO_FORMAT_NV12 || (resource->usage & BCM2711_VIDEO_USAGE_DECODE_DST) == 0)
		return EINVAL;

	/* Its whole bound memory. */
	bcm2711_vulkan_video_nv12_layout(resource->width, resource->height, pitch, chroma, &bytes);
	error = bcm2711_vulkan_resource_backing(resource, 0, bytes, &backing, native, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the image and its place in the V3D's space. */
	*image = resource;
	return 0;
}

/* Replaces a phase 1 list with one of the next size. */
static int
grow_list(
	struct bcm2711_buffer **list)
{
	struct bcm2711_buffer *larger;
	int error;

	/* The larger list, then the old one goes. */
	error = bcm2711_buffer_create_uncached(bcm2712_hevc_list_grow((*list)->bytes), VIDEO_BUFFER_LIMIT, VIDEO_PAGE, &larger);
	if (error != 0)
		return error;
	bcm2711_buffer_release(*list);
	*list = larger;

	/* Succeeded: the list is larger. */
	return 0;
}

/* Fills phase 1's buffers from the session's lists and command list. */
static void
phase1_buffers(
	const struct video_session *video,
	uint32_t command_count,
	uint32_t ctb_rows,
	struct bcm2712_hevc_phase1_buffers *buffers)
{
	/* The command list, then the two lists. */
	buffers->commands = video->commands->memory.paddr;
	buffers->command_count = command_count;
	buffers->pu = video->pu->memory.paddr;
	buffers->pu_bytes = video->pu->bytes;
	buffers->coefficients = video->coefficients->memory.paddr;
	buffers->coefficient_bytes = video->coefficients->bytes;
	buffers->ctb_rows = ctb_rows;
}

/*
 * Writes a decoded picture into the decode's output image: the texture
 * formatting unit reads the SAND luma plane as R8 and the chroma plane as
 * RG8 and writes each as the image's raster plane; the image's run then
 * counts as written, so its sampled textures are made again.
 */
static int
convert(
	struct bcm2711_render_device *controller,
	const struct video_running *running)
{
	const struct video_session *video;
	const struct bcm2712_hevc_sps *sps;
	const struct video_parameters *parameters;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_memory *memory;
	struct bcm2711_v3d_job job;
	struct bcm2711_v3d_job_result result;
	uint64_t physical;
	uint64_t chroma;
	uint32_t source;
	uint32_t output;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t texel_bytes;
	uint32_t type;
	int error;

	/* The picture's planes in the V3D's space, and the image's. */
	video = running->video;
	error = picture_address(video, running->picture, &physical, &source);
	if (error != 0)
		return error;
	error = image_target(running->command->view, &image, &output, &pitch, &chroma);
	if (error != 0)
		return error;

	/* The area written: the coded picture, within the image. */
	parameters = running->command->begin->parameters->payload;
	sps = find_sps(parameters, running->command->picture.vps_id, running->command->picture.sps_id);
	if (sps == NULL)
		return EINVAL;
	width = sps->width;
	height = sps->height;
	if (width > image->width)
		width = image->width;
	if (height > image->height)
		height = image->height;

	/* The luma plane: SAND R8 into raster R8. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	error = bcm2711_native_texel_layout(BCM2711_NATIVE_TEXEL_R8, &texel_bytes, &type);
	if (error != 0)
		return error;
	job.command.tfu.input = source;
	job.command.tfu.input_bytes = (uint32_t)video->sand.luma_bytes;
	job.command.tfu.stride = video->sand.column_rows;
	job.command.tfu.output = output;
	job.command.tfu.output_bytes = pitch * (height - 1U) + width;
	job.command.tfu.output_format = TFU_OUTPUT_RASTER | (pitch << TFU_STRIDE_SHIFT);
	job.command.tfu.size = (height << 16) | width;
	job.command.tfu.configuration = TFU_SAND_CONFIGURATION | (type << TFU_TYPE_SHIFT);
	error = bcm2711_v3d_job_run(controller->space.native, &job, &result);
	if (error != 0)
		return error;

	/* The chroma plane: SAND RG8 into raster RG8, half the size each way. */
	error = bcm2711_native_texel_layout(BCM2711_NATIVE_TEXEL_RG8, &texel_bytes, &type);
	if (error != 0)
		return error;
	width = (width + 1U) / 2U;
	height = (height + 1U) / 2U;
	job.command.tfu.input = source + (uint32_t)video->sand.luma_bytes;
	job.command.tfu.input_bytes = (uint32_t)video->sand.chroma_bytes;
	job.command.tfu.stride = video->sand.column_rows / 2U;
	job.command.tfu.output = output + (uint32_t)chroma;
	job.command.tfu.output_bytes = pitch * (height - 1U) + width * texel_bytes;
	job.command.tfu.output_format = TFU_OUTPUT_RASTER | ((pitch / texel_bytes) << TFU_STRIDE_SHIFT);
	job.command.tfu.size = (height << 16) | width;
	job.command.tfu.configuration = TFU_SAND_CONFIGURATION | (type << TFU_TYPE_SHIFT);
	error = bcm2711_v3d_job_run(controller->space.native, &job, &result);
	if (error != 0)
		return error;

	/* The image's run is written: textures sampled from it are made again. */
	view = running->command->view->payload;
	(void)view;
	memory = image->memory->payload;
	bcm2711_v3d_memory_written(memory->view->buffer, image->offset, NULL);

	/* Succeeded: the image holds the picture. */
	return 0;
}

/* Writes a decode's status into the begin query open around it, if any. */
static void
finish(
	const struct video_command *command,
	int32_t status)
{
	struct video_pool *pool;

	/* Only a decode inside a begin query has a status to write. */
	if (command == NULL || command->status == NULL)
		return;
	pool = command->status->pool->payload;
	if (command->status->query >= pool->count)
		return;
	pool->status[command->status->query] = status;
}

/* Skips a picture: logs why (a few times a session) and marks its status ERROR. */
static void
skip(
	struct video_session *video,
	const struct video_command *command,
	const char *reason,
	int error)
{
	/* The line, while the session has lines left. */
	if (video->skip_lines < VIDEO_SKIP_LINES) {
		video->skip_lines++;
		kern_logf("v3d: video: picture skipped: %s (%d)\n", reason, error);
	}

	/* The status. */
	finish(command, VIDEO_STATUS_ERROR);
}

/*
 * Waits, with the controller mutex held, for the block to finish the decode
 * it has (phase 2 is not started after phase 1); the decode is then given
 * up.  A block that does not finish in time keeps the decode.
 */
static void
wait_idle(void)
{
	enum bcm2712_hevc_result state;
	uint64_t limit;

	/* Polls the phase until it ends or the time is over. */
	limit = clock_ticks() + kern_ms_to_ticks(VIDEO_DECODE_MS);
	while (video_running.command != NULL && clock_ticks() < limit) {
		if (video_running.phase == 1U) {
			state = bcm2712_hevc_phase1_poll();
		} else {
			state = bcm2712_hevc_phase2_poll();
		}
		if (state != BCM2712_HEVC_RUNNING) {
			video_running.command = NULL;
			break;
		}
		kern_usleep_range(100U, 200U);
	}

	/* A block still busy is not used again. */
	if (video_running.command != NULL)
		video_block_lost = true;
}
