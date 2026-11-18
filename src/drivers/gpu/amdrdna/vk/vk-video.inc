/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's Vulkan Video: H.264 and H.265 decode on the GPU's video
 * engine (VCN 2.0), in the universal queue family (included by
 * vk-resource.c).
 *
 * libvulkan carries Vulkan Video as the protocol's video commands
 * (uapi/gpu-op.h, 0x10000 on).  The ones that answer at once (the
 * capabilities, the format, the family's codecs, the sessions, their
 * memory and their parameters) run here when they come; the four coding
 * commands and the query commands are recorded into command buffers and
 * run in their submission's order (vk-execute.c), which ends the GPU's
 * earlier work before a decode and runs the decode before the work after
 * it.
 *
 * A session holds what the engine keeps of a stream outside the
 * application's memory: a DPB the firmware manages, in VRAM only the GPU
 * reaches, which keeps the reference pictures under tags (each the DPB
 * slot of its picture), and the decoder's context, both sized for the
 * session's codec, largest picture and slots; and a run of system
 * memory for the messages and the indirect buffers.  The application's
 * picture is the decode's output (the DPB slot and the output coincide),
 * and the application binds the session context the firmware keeps
 * between pictures.  Every decode is checked before its message is built
 * (the sets, the sizes, the slots, the bitstream, the output); the engine
 * runs it in the session's address space, so it reaches only the
 * session's memory, and the run is waited for at most VIDEO_RUN_MS.  The
 * open result status query gets the decode's status.
 */

#include "drivers/gpu/amdrdna/amdrdna-vcn.h"

/* The readers of the H.264 records, which the decoders of the commands below use. */
#include "drivers/gpu/amdrdna/vk/vk-video-h264.inc"

/* A session's codec: the codec operation of its profile (VkVideoCodecOperationFlagBitsKHR). */
#define VIDEO_OP_DECODE_H264			0x1U
#define VIDEO_OP_DECODE_H265			0x2U

/* The structure types the video commands carry (VkStructureType). */
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
#define VIDEO_TYPE_H265_CAPABILITIES		1000187000U
#define VIDEO_TYPE_H265_PARAMETERS_CREATE	1000187001U
#define VIDEO_TYPE_H265_PARAMETERS_ADD		1000187002U
#define VIDEO_TYPE_H265_PROFILE			1000187003U
#define VIDEO_TYPE_H265_PICTURE			1000187004U
#define VIDEO_TYPE_H265_DPB_SLOT		1000187005U

/* The chroma subsampling and bit depth bits, the layout, and the capability and control flags. */
#define VIDEO_CHROMA_420			0x2U
#define VIDEO_DEPTH_8				0x1U
#define VIDEO_LAYOUT_PROGRESSIVE		0U
#define VIDEO_SEPARATE_REFERENCE_IMAGES		0x2U
#define VIDEO_DPB_AND_OUTPUT_COINCIDE		0x1U
#define VIDEO_CONTROL_RESET			0x1U

/* The H.264 profiles the engine decodes (profile_idc): Baseline, Main and High. */
#define VIDEO_PROFILE_BASELINE			66U
#define VIDEO_PROFILE_MAIN			77U
#define VIDEO_PROFILE_HIGH			100U

/* The H.265 profile the engine decodes (general_profile_idc): Main. */
#define VIDEO_PROFILE_H265_MAIN			1U

/* The video results a refused profile or session reports (VkResult). */
#define VIDEO_ERROR_IMAGE_USAGE			(-1000023000)
#define VIDEO_ERROR_LAYOUT			(-1000023001)
#define VIDEO_ERROR_OPERATION			(-1000023002)
#define VIDEO_ERROR_FORMAT			(-1000023003)
#define VIDEO_ERROR_CODEC			(-1000023004)
#define VIDEO_ERROR_STD_VERSION			(-1000023005)
#define VIDEO_ERROR_STD_PARAMETERS		(-1000299000)

/* The status a decode reports to the open result status query. */
#define VIDEO_STATUS_COMPLETE			1
#define VIDEO_STATUS_ERROR			(-1)

/*
 * What a session decodes: H.264 pictures of 16 by 16 to 4096 by 2304
 * samples on 16-sample boundaries, up to level 5.1 (whose largest frame,
 * 8704 macroblocks, the largest size holds), 17 slots and 16 references,
 * a bitstream on 128-byte boundaries.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (radv_GetPhysicalDeviceVideoCapabilitiesKHR: level 5.1 for VCN 2, the
 * granularity and the minimum of 16, the bitstream alignments of 128);
 * Recommendation ITU-T H.264 (Table A-1, MaxFS of level 5.1).
 */
#define VIDEO_GRANULARITY			16U
#define VIDEO_MIN_SIDE				16U
#define VIDEO_MAX_WIDTH				4096U
#define VIDEO_MAX_HEIGHT			2304U
#define VIDEO_LEVEL_5_1				14U
#define VIDEO_BITSTREAM_ALIGN			128U
#define VIDEO_MB				16U

/*
 * What an H.265 session decodes besides: Main pictures up to 4096 by
 * 2176 samples (level 5.1's largest picture, 8912896 samples, at the
 * widest), 16 slots and 15 references.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_video.c
 * (radv_GetPhysicalDeviceVideoCapabilitiesKHR: level 5.1 for VCN 2 and
 * STD_VIDEO_H265_PROFILE_IDC_MAIN); Recommendation ITU-T H.265 (Table A.8,
 * MaxLumaPs of level 5.1; A.4.2, MaxDpbSize at most 16).
 */
#define VIDEO_H265_MAX_HEIGHT			2176U
#define VIDEO_H265_LEVEL_5_1			8U

/* The H.264 and H.265 decode headers a session names, and their versions (1.0.0). */
#define VIDEO_STD_NAME				"VK_STD_vulkan_video_codec_h264_decode"
#define VIDEO_STD_VERSION			((1U << 22) | (0U << 12) | 0U)
#define VIDEO_H265_STD_NAME			"VK_STD_vulkan_video_codec_h265_decode"
#define VIDEO_H265_STD_VERSION			((1U << 22) | (0U << 12) | 0U)

/* The parameter set id ranges of H.264, and the deepest chain libvulkan writes. */
#define VIDEO_SPS_IDS				32U
#define VIDEO_PPS_IDS				256U
#define VIDEO_MAX_CHAIN				8U

/* The slots a begin lists at most (each DPB slot, and the setup without one). */
#define VIDEO_BEGIN_SLOTS			(AMDRDNA_VCN_H264_SLOTS + 1U)

/* The one memory binding of a session: the firmware's session context, on 4 KiB, of either memory type. */
#define VIDEO_BINDINGS				1U
#define VIDEO_BIND_ALIGN			4096U
#define VIDEO_MEMORY_TYPES			((1U << AMDRDNA_VK_MEMORY_TYPES) - 1U)

/*
 * The session's run of system memory, a page for each part: the decode's
 * indirect buffer (and the creation's after it), the decode message, the
 * creation or destruction message, the feedback, the scaling table and
 * the slice of a frame a gap in frame_num implies.
 */
#define VIDEO_RUN_PAGE				4096U
#define VIDEO_RUN_DECODE_AT			0U
#define VIDEO_RUN_CREATE_RUN_AT			2048U
#define VIDEO_RUN_MESSAGE_AT			(1U * VIDEO_RUN_PAGE)
#define VIDEO_RUN_CREATE_AT			(2U * VIDEO_RUN_PAGE)
#define VIDEO_RUN_FEEDBACK_AT			(3U * VIDEO_RUN_PAGE)
#define VIDEO_RUN_SCALING_AT			(4U * VIDEO_RUN_PAGE)
#define VIDEO_RUN_GAP_AT			(5U * VIDEO_RUN_PAGE)
#define VIDEO_RUN_BYTES				(6U * VIDEO_RUN_PAGE)

/*
 * The longest one run of the engine is waited for, in milliseconds: about
 * twice the longest a picture the decoder takes can need.  Measured on the
 * RX 5600 XT, the engine takes about 14 ms for each megabyte of an H.264
 * High picture's CABAC data (H.265 about 8), so the largest picture the
 * reported level 5.1 allows (its coded picture buffer, 240000 kbit times
 * 1.25 for High, 37.5 MB) needs about 0.55 s, and a slice cut short takes
 * the firmware about 0.22 s to give up on.  The node's lock is held meanwhile, so a run that never ends stops
 * every client's frame for this long once.
 */
#define VIDEO_RUN_MS				1000U

/* The engine's memory one session takes at most (its DPB and its decoder context). */
#define VIDEO_SESSION_BYTES_MAX			(512ULL * 1024U * 1024U)

/* The skipped pictures logged at most, and the pictures whose feedback is logged at the start. */
#define VIDEO_SKIP_LINES			32U
#define VIDEO_TRACE_PICTURES			3U

/* One H.264 picture parameter set a parameters object holds; the object's sets are a list. */
struct video_pps_node {
	struct video_pps_node *next;
	struct amdrdna_vcn_h264_pps pps;
};

/* One H.265 sequence parameter set a parameters object holds; the object's sets are a list. */
struct video_h265_sps_node {
	struct video_h265_sps_node *next;
	struct amdrdna_vcn_h265_sps sps;
};

/* One H.265 picture parameter set a parameters object holds; the object's sets are a list. */
struct video_h265_pps_node {
	struct video_h265_pps_node *next;
	struct amdrdna_vcn_h265_pps pps;
};

/*
 * One VkVideoSessionParametersKHR: the sets of one session of its
 * session's codec (H.264 by id; H.265 the video sets' ids, the others by
 * key), the capacity of each kind, and the number of the last update
 * taken.  It holds its session's object; the recorded decodes that use
 * it hold it.
 */
struct video_parameters {
	struct amdrdna_vk_object *session;
	uint32_t max_vps;
	uint32_t max_sps;
	uint32_t max_pps;
	uint32_t update_sequence;
	struct amdrdna_vcn_h264_sps *sps[VIDEO_SPS_IDS];
	struct video_pps_node *pps;
	uint32_t h265_vps;
	struct video_h265_sps_node *h265_sps;
	struct video_h265_pps_node *h265_pps;
};

/*
 * One VkVideoSessionKHR.
 *
 * It keeps its codec (the profile's codec operation), the limits it was
 * made with, the engine's handle of its
 * stream and the engine's generation the firmware's session was created
 * in (created clear until a reset or the first decode creates it), its
 * DPB and decoder context in VRAM (with the bytes counted against the
 * device's limits), its run of system memory, the session context the
 * application bound (the memory held), which slots hold a picture, the
 * last H.264 reference picture the engine decoded since the firmware's
 * session was made (its frame_num, its tag and its first order count, for
 * finding a gap in frame_num), the number of the next picture, and the
 * counts of its pictures and of the time they took, and of the pictures whose feedback carried a status other than
 * 0 (with the last such status).  lost says a run of the engine did not end or faulted:
 * the firmware's state of the stream is gone until a reset.  Changed
 * only while the executor runs the session's commands, under the node's
 * lock.
 */
struct video_session {
	void *owner;
	uint32_t operation;
	uint32_t profile_idc;
	uint32_t width;
	uint32_t height;
	uint32_t slots;
	uint32_t references;
	uint32_t handle;
	uint32_t generation;
	bool created;
	bool lost;
	bool counted;
	uint64_t dpb_bytes;
	uint64_t context_bytes;
	void *dpb_backing;
	uint64_t dpb_va;
	void *context_backing;
	uint64_t context_va;
	void *run_token;
	uint8_t *run_cpu;
	uint64_t run_va;
	struct amdrdna_vk_object *binding;
	uint64_t binding_offset;
	bool slot_valid[AMDRDNA_VCN_H264_SLOTS];
	bool h264_previous;
	uint32_t h264_previous_frame_num;
	uint32_t h264_previous_tag;
	int32_t h264_previous_poc;
	uint32_t serial;
	uint64_t pictures;
	uint64_t errors;
	uint64_t run_us;
	uint64_t run_us_max;
	uint64_t wait_us;
	uint64_t feedback_marked;
	uint32_t feedback_last;
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
 * names a picture and which, and the slot's H.264 or H.265 reference
 * information.
 */
struct video_slot {
	int32_t index;
	bool has_picture;
	struct video_resource resource;
	bool has_reference;
	struct amdrdna_vcn_h264_reference reference;
	bool has_h265_reference;
	struct amdrdna_vcn_h265_reference h265_reference;
};

/*
 * One recorded video or query command (the record's regions).  The
 * objects it names are held in the record's objects; the pointers here
 * are those holds: a begin's session and parameters, a decode's session
 * and parameters (its coding scope's), bitstream buffer and output view,
 * a query command's pool.  skip says why a decode is run as an error
 * without the engine.
 */
struct video_record {
	struct amdrdna_vk_object *session;
	struct amdrdna_vk_object *parameters;
	struct amdrdna_vk_object *buffer;
	struct amdrdna_vk_object *view;
	struct amdrdna_vk_object *pool;
	uint32_t flags;
	uint64_t offset;
	uint64_t range;
	struct video_resource destination;
	bool has_setup;
	struct video_slot setup;
	uint32_t slot_count;
	struct video_slot slots[VIDEO_BEGIN_SLOTS];
	bool has_picture;
	struct amdrdna_vcn_h264_picture picture;
	bool has_h265_picture;
	struct amdrdna_vcn_h265_picture h265_picture;
	uint32_t slice_count;
	uint32_t first_slice;
	uint32_t query;
	uint32_t query_count;
	const char *skip;
};

/*
 * What the chained records of one structure carried (of H.264 or of
 * H.265).  It lives on the stack of one command's decoding; the add
 * record's sets are the parameters object's to take, or freed with the
 * chain.
 */
struct video_chain {
	bool has_h264_profile;
	bool has_h265_profile;
	uint32_t profile_idc;
	uint32_t picture_layout;
	bool has_profile_list;
	uint32_t profile_count;
	bool profiles_supported;
	int32_t profile_error;
	bool has_h264_create;
	bool has_h265_create;
	uint32_t max_vps;
	uint32_t max_sps;
	uint32_t max_pps;
	struct amdrdna_vcn_h264_sps *new_sps[VIDEO_SPS_IDS];
	uint32_t new_sps_count;
	struct video_pps_node *new_pps;
	uint32_t new_h265_vps;
	struct video_h265_sps_node *new_h265_sps;
	struct video_h265_pps_node *new_h265_pps;
	int add_error;
	bool has_picture;
	struct amdrdna_vcn_h264_picture picture;
	bool has_h265_picture;
	struct amdrdna_vcn_h265_picture h265_picture;
	uint32_t slice_count;
	uint32_t first_slice;
	bool slices_disordered;
	bool has_reference;
	struct amdrdna_vcn_h264_reference reference;
	bool has_h265_reference;
	struct amdrdna_vcn_h265_reference h265_reference;
};

/*
 * The skipped pictures logged so far, and the pictures whose feedback
 * was logged, under the node's lock (counted over every session, only to
 * bound the log).
 */
static uint32_t video_skip_lines;
static uint32_t video_trace_lines;

static int video_capabilities(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void video_reply_capabilities(struct i915_wire_writer *reply, const uint32_t *types, uint32_t count, uint32_t operation);
static int video_format_properties(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_family_properties(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_session_create(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int32_t video_session_memory(struct video_session *state);
static int video_requirements(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_bind(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_parameters_create(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_parameters_update(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_session_destroy(struct amdrdna_vk_session *session, void *payload);
static int video_parameters_destroy(struct amdrdna_vk_session *session, void *payload);
static void video_read_chain(struct i915_wire_reader *reader, struct video_chain *chain, uint32_t depth);
static void video_read_fields(struct i915_wire_reader *reader, uint32_t type, struct video_chain *chain, uint32_t depth);
static int32_t video_read_profile(struct i915_wire_reader *reader, uint32_t *operation, uint32_t *profile_idc);
static int32_t video_profile_result(uint32_t operation, uint32_t chroma, uint32_t luma, uint32_t chroma_depth, const struct video_chain *chain);
static void video_read_resource(struct i915_wire_reader *reader, struct video_resource *resource);
static void video_read_slot(struct i915_wire_reader *reader, struct video_slot *slot);
static void video_read_slots(struct i915_wire_reader *reader, struct video_record *video, uint32_t limit);
static void video_read_add(struct i915_wire_reader *reader, struct video_chain *chain);
static void video_read_offsets(struct i915_wire_reader *reader, struct video_chain *chain);
static void video_read_extension(struct i915_wire_reader *reader, char *name, uint32_t *version);
static void video_chain_free(struct video_chain *chain);
static bool video_sizes_fit(const struct video_parameters *parameters, const struct video_chain *chain);
static int video_parameters_take(struct video_parameters *parameters, struct video_chain *chain, bool replace);
static int video_parameters_copy(struct video_parameters *destination, const struct video_parameters *source);
static void video_parameters_free(struct video_parameters *parameters);
static const struct amdrdna_vcn_h264_pps *video_find_pps(const struct video_parameters *parameters, uint32_t sps_id, uint32_t pps_id);
static int video_record_coding(struct amdrdna_vk_session *session, struct amdrdna_vk_command_buffer *command, uint32_t opcode, struct i915_wire_reader *reader, struct amdrdna_vk_record *record, struct video_record *video);
static int video_record_query(struct amdrdna_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct amdrdna_vk_record *record, struct video_record *video);
static int video_hold(struct amdrdna_vk_session *session, struct amdrdna_vk_record *record, enum i915_vk_object_kind kind, uint64_t identity, struct amdrdna_vk_object **holder);
static int video_keep(struct amdrdna_vk_record *record, struct amdrdna_vk_object *object, struct amdrdna_vk_object **holder);
static int video_scope(struct amdrdna_vk_command_buffer *command, struct amdrdna_vk_record *record, struct video_record *video);
static void video_run_control(struct amdrdna_vk_session *session, const struct video_record *video);
static int video_run_decode(struct amdrdna_vk_session *session, const struct video_record *video, uint64_t wait_us, int32_t *status);
static const char *video_check(const struct video_record *video, const struct video_session *state, const struct video_parameters *parameters, struct amdrdna_vcn_h264_decode *h264);
static const char *video_check_output(const struct video_record *video, const struct video_session *state, uint32_t width, uint32_t height);
static const char *video_check_bitstream(const struct video_record *video);
static int video_create(struct video_session *state);
static uint32_t video_h264_gap(const struct video_session *state, const struct amdrdna_vcn_h264_decode *h264, uint32_t *first, int32_t *poc);
static int video_h264_fill_gap(struct video_session *state, const struct amdrdna_vcn_h264_decode *h264, const struct amdrdna_vcn_frame *base, const struct amdrdna_vcn_buffers *buffers, uint32_t frames, uint32_t first, int32_t poc);
static void video_h264_decoded(struct video_session *state, const struct amdrdna_vcn_h264_decode *h264, uint32_t frames);
static int video_session_context(const struct video_session *state, uint64_t *va);
static void video_skip(const char *reason);
static uint32_t video_mbs(uint32_t pixels);

/* The H.265 side: its readers, its sets and its checks, which use the structures above. */
#include "drivers/gpu/amdrdna/vk/vk-video-h265.inc"

/*
 * Routes the video commands that run at once (the queries, the sessions
 * and parameters); *handled stays clear for an opcode of another
 * handler's.
 */
int
amdrdna_vk_video_route(
	struct amdrdna_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Only the video commands that are not recorded. */
	*handled = 0;
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES:
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES:
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES:
	case GPU_OP_CREATE_VIDEO_SESSION:
	case GPU_OP_DESTROY_VIDEO_SESSION:
	case GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS:
	case GPU_OP_BIND_VIDEO_SESSION_MEMORY:
	case GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS:
	case GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS:
	case GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS:
		break;
	default:
		return 0;
	}

	/* Tells the dispatcher that the video code took the opcode. */
	*handled = 1;

	/* Runs the command the opcode names. */
	switch (opcode) {
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES:
		error = video_capabilities(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES:
		error = video_format_properties(session, reader, reply);
		break;
	case GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES:
		error = video_family_properties(session, reader, reply);
		break;
	case GPU_OP_CREATE_VIDEO_SESSION:
		error = video_session_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_VIDEO_SESSION:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_VIDEO_SESSION);
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
	default:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS);
		break;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/* Reports whether an opcode is a recorded video or query command this file keeps. */
bool
amdrdna_vk_video_recorded(
	uint32_t opcode)
{
	/* The four video commands and the begin and end of a query. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
	case GPU_OP_CMD_DECODE_VIDEO:
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
		return true;
	default:
		break;
	}

	/* Another handler's. */
	return false;
}

/*
 * Decodes one recorded video or query command into a structure of its own
 * kept in the record's regions, holding the objects it names in the
 * record's objects.
 *
 * command is the command buffer being recorded (NULL when there is none:
 * the command is decoded and dropped).  Returns EINVAL when the arguments
 * did not decode (the stream is then unreadable), another error when they
 * decoded but cannot be run (the recording fails with it).
 */
int
amdrdna_vk_video_record(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_command_buffer *command,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record)
{
	struct video_record *video;
	int error;

	/* The command's own structure, which the record keeps. */
	video = kern_calloc(1U, sizeof(*video));
	if (video == NULL)
		return ENOMEM;
	record->regions = video;
	record->region_count = 0U;

	/* The command's arguments. */
	if (opcode == GPU_OP_CMD_BEGIN_QUERY || opcode == GPU_OP_CMD_END_QUERY) {
		error = video_record_query(session, opcode, reader, record, video);
	} else {
		error = video_record_coding(session, command, opcode, reader, record, video);
	}

	/* A command that decoded but breaks the API's rules fails the recording, not the stream. */
	if (error == EINVAL && reader->error == 0)
		error = ENOTSUP;
	if (error != 0)
		return error;

	/* Succeeded: the record keeps the command. */
	return 0;
}

/* Reports whether a recorded command is a decode, which needs the GPU's earlier work of the session done. */
bool
amdrdna_vk_video_waits(
	const struct amdrdna_vk_record *record)
{
	/* Only a decode reads and writes memory. */
	return record->opcode == GPU_OP_CMD_DECODE_VIDEO;
}

/*
 * Runs one recorded video or query command.  query is the status query
 * the submission has open (the pool and the query, NULL pool for none);
 * a begin of a coding scope closes it, a begin query opens it, an end
 * query makes it available.  The executor has ended the GPU's earlier
 * work before a decode (wait_us is how long that took); the decode has
 * ended when this returns.  A decode the engine cannot run reports the
 * error status; the submission goes on.  Returns 0.
 */
int
amdrdna_vk_video_run(
	struct amdrdna_vk_session *session,
	const struct amdrdna_vk_record *record,
	struct amdrdna_vk_video_query *query,
	uint64_t wait_us)
{
	const struct video_record *video;
	int32_t status;
	int error;

	/* Takes the structure the record kept; one that kept nothing does nothing. */
	video = record->regions;
	if (video == NULL)
		return 0;

	/* Each command's work. */
	switch (record->opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
		/* No query is open at the start of a scope. */
		query->pool = NULL;
		break;
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
		video_run_control(session, video);
		break;
	case GPU_OP_CMD_DECODE_VIDEO:
		/* The decode, whose status goes to the open query. */
		status = VIDEO_STATUS_ERROR;
		error = video_run_decode(session, video, wait_us, &status);
		if (error != 0)
			status = VIDEO_STATUS_ERROR;
		if (query->pool != NULL)
			query->pool->status[query->query] = status;
		break;
	case GPU_OP_CMD_BEGIN_QUERY:
		/* The decodes report into the query from now on, not ready until one does. */
		query->pool = video->pool->payload;
		query->query = video->query;
		query->pool->status[query->query] = 0;
		break;
	case GPU_OP_CMD_END_QUERY:
		/* The query is available with what the decodes reported. */
		((struct amdrdna_vk_query_pool *)video->pool->payload)->available[video->query] = 1U;
		query->pool = NULL;
		break;
	default:
		break;
	}

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * vkGetPhysicalDeviceVideoCapabilitiesKHR: [physical][present]{profile}
 * [present][shape: type, link...] -> [result][present]{capabilities}.
 */
static int
video_capabilities(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t types[VIDEO_MAX_CHAIN];
	uint32_t count;
	uint32_t operation;
	uint32_t idc;
	uint64_t present;
	int32_t result;
	bool available;

	/* Skips the physical device and reads the profile, which decides the result and the codec. */
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	result = VIDEO_ERROR_OPERATION;
	operation = 0U;
	if (present != 0U)
		result = video_read_profile(reader, &operation, &idc);

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

	/* A device whose video engine is not up takes no profile. */
	available = amdrdna_vk_vcn_available(session->owner);
	if (!available)
		result = VIDEO_ERROR_OPERATION;

	/* A profile the engine does not take replies its reason only. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Replies the result and the records in the shape's order, for the profile's codec. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	video_reply_capabilities(reply, types, count, operation);

	/* Succeeded: the capabilities are reported. */
	return 0;
}

/*
 * Replies one output record of the capabilities' shape and, nested
 * before its fields, the rest; operation is the profile's codec.
 */
static void
video_reply_capabilities(
	struct i915_wire_writer *reply,
	const uint32_t *types,
	uint32_t count,
	uint32_t operation)
{
	char name[VK_MAX_EXTENSION_NAME_SIZE];
	uint32_t height;
	uint32_t slots;
	uint32_t references;
	uint32_t version;

	/* The record's type, then the link to the rest of the shape. */
	drv_i915_wire_reply_u32(reply, types[0]);
	if (count > 1U) {
		drv_i915_wire_reply_u64(reply, 1U);
		video_reply_capabilities(reply, types + 1, count - 1U, operation);
	} else {
		drv_i915_wire_reply_u64(reply, 0U);
	}

	/* The codec's largest picture, slots, references and header. */
	kern_memset(name, 0, sizeof(name));
	if (operation == VIDEO_OP_DECODE_H265) {
		/* H.265: level 5.1's largest picture, its DPB of 16. */
		height = VIDEO_H265_MAX_HEIGHT;
		slots = AMDRDNA_VCN_H265_SLOTS;
		references = AMDRDNA_VCN_H265_REFERENCES;
		kern_memcpy(name, VIDEO_H265_STD_NAME, sizeof(VIDEO_H265_STD_NAME));
		version = VIDEO_H265_STD_VERSION;
	} else {
		/* H.264: the hidden DPB's 17 pictures. */
		height = VIDEO_MAX_HEIGHT;
		slots = AMDRDNA_VCN_H264_SLOTS;
		references = AMDRDNA_VCN_H264_REFERENCES;
		kern_memcpy(name, VIDEO_STD_NAME, sizeof(VIDEO_STD_NAME));
		version = VIDEO_STD_VERSION;
	}

	/* The record's fields. */
	switch (types[0]) {
	case VIDEO_TYPE_CAPABILITIES:
		/* Separate reference images, the bitstream's boundaries. */
		drv_i915_wire_reply_u32(reply, VIDEO_SEPARATE_REFERENCE_IMAGES);
		drv_i915_wire_reply_u64(reply, VIDEO_BITSTREAM_ALIGN);
		drv_i915_wire_reply_u64(reply, VIDEO_BITSTREAM_ALIGN);

		/* The granularity, the smallest and largest pictures, the slots and references. */
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_WIDTH);
		drv_i915_wire_reply_u32(reply, height);
		drv_i915_wire_reply_u32(reply, slots);
		drv_i915_wire_reply_u32(reply, references);

		/* The header's name and version. */
		drv_i915_wire_reply_u64(reply, VK_MAX_EXTENSION_NAME_SIZE);
		drv_i915_wire_reply_bytes(reply, name, sizeof(name));
		drv_i915_wire_reply_u32(reply, version);
		break;
	case VIDEO_TYPE_DECODE_CAPABILITIES:
		/* The output picture is the reference picture. */
		drv_i915_wire_reply_u32(reply, VIDEO_DPB_AND_OUTPUT_COINCIDE);
		break;
	case VIDEO_TYPE_H264_CAPABILITIES:
		/* Level 5.1 and no field offset (progressive only). */
		drv_i915_wire_reply_u32(reply, VIDEO_LEVEL_5_1);
		drv_i915_wire_reply_u32(reply, 0U);
		drv_i915_wire_reply_u32(reply, 0U);
		break;
	case VIDEO_TYPE_H265_CAPABILITIES:
		/* Level 5.1. */
		drv_i915_wire_reply_u32(reply, VIDEO_H265_LEVEL_5_1);
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
	struct amdrdna_vk_session *session,
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

	/* Reads the format info: its type, its chain (the profile list) and the usage, then the capacity. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_FORMAT_INFO)
		reader->error = 1;
	video_read_chain(reader, &chain, 0U);
	usage = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	video_chain_free(&chain);
	if (reader->error != 0)
		return EINVAL;

	/* The profiles must all be ones the engine takes; usages are the decode's output, reference, readback and sampling. */
	result = 0;
	video_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_SAMPLED_BIT;
	available = amdrdna_vk_vcn_available(session->owner);
	if (!available) {
		result = VIDEO_ERROR_OPERATION;
	} else if (!chain.has_profile_list || chain.profile_count == 0U) {
		result = VIDEO_ERROR_OPERATION;
	} else if (!chain.profiles_supported) {
		result = chain.profile_error;
	} else if ((usage & ~video_usage) != 0U) {
		result = VIDEO_ERROR_IMAGE_USAGE;
	}

	/* Reports a format the engine does not offer. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/*
	 * Replies the one format: NV12, identity swizzle, its planes viewable
	 * in their own formats (mutable format, extended usage), 2D, optimal
	 * tiling (its planes linear), decode output, reference, readback and
	 * sampled.
	 */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VIDEO_TYPE_FORMAT_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0U);
	drv_i915_wire_reply_u32(reply, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);
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
 * [present][count]{operations}.  The universal family decodes H.264 and
 * H.265 when the video engine is up; the compute family decodes nothing.
 */
static int
video_family_properties(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t count;
	uint32_t family;
	uint32_t operations;
	bool available;

	/* Skips the physical device and reads the family count, at most the families there are. */
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || count == 0U || count > AMDRDNA_VK_FAMILIES)
		return EINVAL;

	/* H.264 and H.265 decode for the universal family with the engine, nothing for the others. */
	available = amdrdna_vk_vcn_available(session->owner);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, count);
	for (family = 0U; family < count; family++) {
		operations = 0U;
		if (available && family == AMDRDNA_VK_FAMILY_UNIVERSAL)
			operations = VIDEO_OP_DECODE_H264 | VIDEO_OP_DECODE_H265;
		drv_i915_wire_reply_u32(reply, operations);
	}

	/* Succeeded: the families' codec operations are reported. */
	return 0;
}

/*
 * vkCreateVideoSessionKHR: [device][present]{create info}[allocator][present]
 * [identity] -> [result][present][identity].
 *
 * The session's DPB, decoder context and run are taken here, so a session
 * that cannot decode fails at once.
 */
static int
video_session_create(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_session *state;
	struct video_chain chain;
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
	uint32_t operation;
	uint32_t profile_idc;
	uint32_t max_height;
	uint32_t max_slots;
	uint32_t max_references;
	uint32_t std_version;
	int32_t result;
	bool available;
	int different;

	/* Reads the create info: [type][chain][family][flags][profile][formats and limits][header]. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(name, 0, sizeof(name));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_SESSION_CREATE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0U);
	family = drv_i915_wire_read_u32(reader);
	flags = drv_i915_wire_read_u32(reader);
	result = VIDEO_ERROR_OPERATION;
	operation = 0U;
	profile_idc = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		result = video_read_profile(reader, &operation, &profile_idc);
	picture_format = drv_i915_wire_read_u32(reader);
	width = drv_i915_wire_read_u32(reader);
	height = drv_i915_wire_read_u32(reader);
	reference_format = drv_i915_wire_read_u32(reader);
	slots = drv_i915_wire_read_u32(reader);
	references = drv_i915_wire_read_u32(reader);
	version = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_extension(reader, name, &version);
	identity = amdrdna_vk_create_tail(reader);
	video_chain_free(&chain);
	if (reader->error != 0 || identity == 0U)
		return EINVAL;

	/* The codec's limits and header. */
	if (operation == VIDEO_OP_DECODE_H265) {
		/* H.265. */
		max_height = VIDEO_H265_MAX_HEIGHT;
		max_slots = AMDRDNA_VCN_H265_SLOTS;
		max_references = AMDRDNA_VCN_H265_REFERENCES;
		different = kern_strcmp(name, VIDEO_H265_STD_NAME);
		std_version = VIDEO_H265_STD_VERSION;
	} else {
		/* H.264. */
		max_height = VIDEO_MAX_HEIGHT;
		max_slots = AMDRDNA_VCN_H264_SLOTS;
		max_references = AMDRDNA_VCN_H264_REFERENCES;
		different = kern_strcmp(name, VIDEO_STD_NAME);
		std_version = VIDEO_STD_VERSION;
	}

	/* Checks the session against what the engine takes; the first reason is the result. */
	available = amdrdna_vk_vcn_available(session->owner);
	if (result != 0) {
		/* The profile's own reason. */
	} else if (!available) {
		result = VIDEO_ERROR_OPERATION;
	} else if (family != AMDRDNA_VK_FAMILY_UNIVERSAL) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (flags != 0U) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
	} else if (picture_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || reference_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
		result = VIDEO_ERROR_FORMAT;
	} else if (width < VIDEO_MIN_SIDE ||
	    height < VIDEO_MIN_SIDE ||
	    width > VIDEO_MAX_WIDTH ||
	    height > max_height) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (slots > max_slots || references > max_references) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (different != 0 || version > std_version) {
		result = VIDEO_ERROR_STD_VERSION;
	}

	/* Reports a session the engine cannot run. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* The session with its limits. */
	state = kern_calloc(1U, sizeof(*state));
	if (state == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}
	state->owner = session->owner;
	state->operation = operation;
	state->profile_idc = profile_idc;
	state->width = width;
	state->height = height;
	state->slots = slots;
	state->references = references;

	/* The engine's memory for it, counted against the device's limits. */
	result = video_session_memory(state);
	if (result != 0) {
		(void)video_session_destroy(session, state);
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Publishes the session and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_VIDEO_SESSION, identity, state, video_session_destroy, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Takes a new session's memory of the engine: the hidden DPB and the
 * decoder context for its codec, its largest picture and its slots (in
 * VRAM only
 * the GPU reaches, zeroed), counted against the session's and the
 * device's limits, and its run of system memory.  Returns 0, or the
 * VkResult that refuses the session (what was taken stays in the state
 * for its destruction).
 */
static int32_t
video_session_memory(
	struct video_session *state)
{
	uint64_t bytes;
	int error;

	/* The DPB and the context the engine needs for the session's codec, largest picture and slots. */
	if (state->operation == VIDEO_OP_DECODE_H265) {
		/* H.265. */
		state->dpb_bytes = amdrdna_vcn_h265_dpb_bytes(state->width, state->height, state->slots);
		state->context_bytes = amdrdna_vcn_h265_context_bytes(state->width, state->height, state->slots);
	} else {
		/* H.264. */
		state->dpb_bytes = amdrdna_vcn_h264_dpb_bytes(state->width, state->height, state->slots);
		state->context_bytes = amdrdna_vcn_h264_context_bytes(state->width, state->height, state->slots);
	}
	bytes = state->dpb_bytes + state->context_bytes;
	if (bytes > VIDEO_SESSION_BYTES_MAX)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Counted against the device's sessions and memory, under a handle of the engine's. */
	error = amdrdna_vk_vcn_take(state->owner, bytes, &state->handle);
	if (error == EMFILE)
		return VK_ERROR_TOO_MANY_OBJECTS;
	if (error != 0)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	state->counted = true;

	/* The DPB, then the context, in VRAM; then the run in system memory. */
	error = amdrdna_vk_memory_create(state->owner, state->dpb_bytes, &state->dpb_va, &state->dpb_backing);
	if (error == 0)
		error = amdrdna_vk_memory_create(state->owner, state->context_bytes, &state->context_va, &state->context_backing);
	if (error == 0)
		error = amdrdna_vk_gpu_alloc_system(state->owner, VIDEO_RUN_BYTES, &state->run_token, &state->run_cpu, &state->run_va);
	if (error != 0) {
		kern_logf("amdrdna: vk: video: no memory for a session of %ux%u (%d)\n", (unsigned)state->width, (unsigned)state->height, error);
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}

	/* Succeeded: the session has its memory. */
	return 0;
}

/*
 * vkGetVideoSessionMemoryRequirementsKHR: [device][session][present][capacity]
 * [array] -> [result][present][count][array]{binding}.  The one binding:
 * the firmware's session context, on 4 KiB, of either memory type.
 */
static int
video_requirements(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct video_session *state;
	uint64_t identity;
	uint64_t present;
	uint32_t capacity;

	/* Skips the device, reads the session, whether the array is there and its capacity. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	capacity = drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* An unknown session is refused. */
	state = amdrdna_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (state == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	/* The count alone when no array was given. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VIDEO_BINDINGS);
	if (present == 0U || capacity < VIDEO_BINDINGS) {
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* The binding: its type, no chain, its index, its size, its boundary, its memory types. */
	drv_i915_wire_reply_u64(reply, VIDEO_BINDINGS);
	drv_i915_wire_reply_u32(reply, VIDEO_TYPE_MEMORY_REQUIREMENTS);
	drv_i915_wire_reply_u64(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, AMDRDNA_VCN_SESSION_CONTEXT_BYTES);
	drv_i915_wire_reply_u64(reply, VIDEO_BIND_ALIGN);
	drv_i915_wire_reply_u32(reply, VIDEO_MEMORY_TYPES);

	/* Succeeded: the binding is reported. */
	return 0;
}

/*
 * vkBindVideoSessionMemoryKHR: [device][session][count][array]{bind} ->
 * [result].  The session context's binding holds its memory, on its
 * boundary and large enough; it is bound once.
 */
static int
video_bind(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_session *state;
	struct amdrdna_vk_object *memory;
	const struct amdrdna_vk_memory *storage;
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

	/* Reads every binding: [type][chain][index][memory][offset][size]. */
	state = amdrdna_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	result = 0;
	if (state == NULL)
		result = VK_ERROR_INITIALIZATION_FAILED;
	for (item = 0U; item < count && reader->error == 0; item++) {
		kern_memset(&chain, 0, sizeof(chain));
		type = drv_i915_wire_read_u32(reader);
		if (type != VIDEO_TYPE_BIND_MEMORY)
			reader->error = 1;
		video_read_chain(reader, &chain, 0U);
		video_chain_free(&chain);
		index = drv_i915_wire_read_u32(reader);
		memory_id = drv_i915_wire_read_u64(reader);
		offset = drv_i915_wire_read_u64(reader);
		size = drv_i915_wire_read_u64(reader);

		/* The session's binding, not bound yet, on its boundary, of memory large enough. */
		memory = amdrdna_vk_object_find(session, I915_VK_OBJ_MEMORY, memory_id);
		storage = NULL;
		if (memory != NULL)
			storage = memory->payload;
		if (state == NULL || result != 0) {
			continue;
		} else if (index >= VIDEO_BINDINGS || storage == NULL || state->binding != NULL) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		} else if ((offset & (VIDEO_BIND_ALIGN - 1U)) != 0U ||
		    size < AMDRDNA_VCN_SESSION_CONTEXT_BYTES ||
		    offset > storage->bytes ||
		    size > storage->bytes - offset) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		}

		/* The binding holds its memory. */
		error = amdrdna_vk_object_retain(memory);
		if (error != 0) {
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
			continue;
		}

		/* Binds the memory at its offset. */
		state->binding = memory;
		state->binding_offset = offset;
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
 * (the H.264 or H.265 create and add), flags, template, session}
 * [allocator][present][identity] -> [result][present][identity].  The
 * create record must be of the session's codec; a sequence set larger
 * than the session's largest picture refuses the object.
 */
static int
video_parameters_create(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_parameters *parameters;
	const struct video_parameters *source;
	const struct video_session *state;
	struct amdrdna_vk_object *owner;
	struct amdrdna_vk_object *template_object;
	struct video_chain chain;
	uint64_t present;
	uint64_t identity;
	uint64_t template_identity;
	uint64_t session_identity;
	uint32_t type;
	bool fits;
	bool h265;
	bool created;
	int error;

	/* Reads the create info: [type][chain][flags][template][session], then the identity. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_PARAMETERS_CREATE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0U);
	(void)drv_i915_wire_read_u32(reader);
	template_identity = drv_i915_wire_read_u64(reader);
	session_identity = drv_i915_wire_read_u64(reader);
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0 || identity == 0U) {
		video_chain_free(&chain);
		return EINVAL;
	}

	/* The session's codec decides which create record must be given. */
	h265 = false;
	owner = amdrdna_vk_object_find(session, I915_VK_OBJ_VIDEO_SESSION, session_identity);
	if (owner != NULL) {
		state = owner->payload;
		h265 = state->operation == VIDEO_OP_DECODE_H265;
	}
	created = chain.has_h264_create;
	if (h265)
		created = chain.has_h265_create;

	/* The session must exist and its codec's create record be given; a template must be of the same session. */
	error = 0;
	source = NULL;
	template_object = NULL;
	if (template_identity != 0U)
		template_object = amdrdna_vk_object_find(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, template_identity);
	if (template_object != NULL)
		source = template_object->payload;
	if (owner == NULL) {
		error = EINVAL;
	} else if (!created || chain.add_error != 0) {
		error = EINVAL;
	} else if (template_identity != 0U && (source == NULL || source->session != owner)) {
		error = EINVAL;
	}

	/* The object, which holds its session, with the template's sets and then the new ones. */
	parameters = NULL;
	if (error == 0) {
		parameters = kern_calloc(1U, sizeof(*parameters));
		if (parameters == NULL)
			error = ENOMEM;
	}

	/* Holds the session the parameters belong to, and records the capacity. */
	if (error == 0)
		error = amdrdna_vk_object_retain(owner);
	if (error == 0) {
		parameters->session = owner;
		parameters->max_vps = chain.max_vps;
		parameters->max_sps = chain.max_sps;
		parameters->max_pps = chain.max_pps;
	}

	/* A sequence set larger than the session's largest picture is not a set the session can decode with. */
	if (error == 0) {
		if (h265) {
			fits = video_h265_sizes_fit(parameters, &chain);
		} else {
			fits = video_sizes_fit(parameters, &chain);
		}
		if (!fits) {
			video_chain_free(&chain);
			(void)video_parameters_destroy(session, parameters);
			drv_i915_wire_reply_u32(reply, (uint32_t)VIDEO_ERROR_STD_PARAMETERS);
			return 0;
		}
	}

	/* Copies the template's sets, then adds the new ones, of the session's codec. */
	if (error == 0 && source != NULL && h265) {
		error = video_h265_copy(parameters, source);
	} else if (error == 0 && source != NULL) {
		error = video_parameters_copy(parameters, source);
	}
	if (error == 0 && h265) {
		error = video_h265_take(parameters, &chain, true);
	} else if (error == 0) {
		error = video_parameters_take(parameters, &chain, true);
	}
	video_chain_free(&chain);

	/* A refused object goes with what it took. */
	if (error != 0 && parameters != NULL) {
		(void)video_parameters_destroy(session, parameters);
		parameters = NULL;
	}

	/* Publishes the object and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity, parameters, video_parameters_destroy, error);

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
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_parameters *parameters;
	const struct video_session *state;
	struct video_chain chain;
	uint64_t identity;
	uint64_t present;
	uint32_t type;
	uint32_t sequence;
	int32_t result;
	bool fits;
	bool h265;
	int error;

	/* Reads the object and the update info: [type][chain][sequence count]. */
	kern_memset(&chain, 0, sizeof(chain));
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U || type != VIDEO_TYPE_PARAMETERS_UPDATE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0U);
	sequence = drv_i915_wire_read_u32(reader);
	if (reader->error != 0) {
		video_chain_free(&chain);
		return EINVAL;
	}

	/* The object must exist and the update be the next one; a malformed set refuses it. */
	result = 0;
	parameters = amdrdna_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity);
	if (parameters == NULL ||
	    sequence != parameters->update_sequence + 1U ||
	    chain.add_error != 0)
		result = VK_ERROR_INITIALIZATION_FAILED;

	/* The session's codec decides which sets the update adds. */
	h265 = false;
	if (result == 0) {
		state = parameters->session->payload;
		h265 = state->operation == VIDEO_OP_DECODE_H265;
	}

	/* A sequence set larger than the session's largest picture refuses the update. */
	if (result == 0) {
		if (h265) {
			fits = video_h265_sizes_fit(parameters, &chain);
		} else {
			fits = video_sizes_fit(parameters, &chain);
		}
		if (!fits)
			result = VIDEO_ERROR_STD_PARAMETERS;
	}

	/* Adds the sets; a key already held refuses the whole update. */
	if (result == 0) {
		if (h265) {
			error = video_h265_take(parameters, &chain, false);
		} else {
			error = video_parameters_take(parameters, &chain, false);
		}
		if (error == ENOMEM) {
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
		} else if (error != 0) {
			result = VK_ERROR_INITIALIZATION_FAILED;
		}
	}

	/* Records the update's sequence number when it is taken. */
	video_chain_free(&chain);
	if (result == 0)
		parameters->update_sequence = sequence;

	/* Replies the result. */
	drv_i915_wire_reply_u32(reply, (uint32_t)result);

	/* Succeeded: the command was decoded; the reply carries its result. */
	return 0;
}

/*
 * Destroys a session once nothing holds it: the firmware's session (when
 * the engine still has it), its binding's memory, its DPB, context and
 * run (kept by the node when the engine may still touch them), and its
 * counts; the session's pictures and their time go to the log.
 */
static int
video_session_destroy(
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct video_session *state;
	uint64_t session_context;
	uint64_t elapsed_us;
	uint32_t generation;
	unsigned count;
	int error;

	UNUSED_PARAMETER(session);

	/* Takes the state the registry kept. */
	state = payload;
	error = 0;

	/* The firmware lets go of the stream, when it still has it. */
	generation = 0U;
	if (state->created)
		generation = amdrdna_vk_vcn_generation(state->owner);
	if (state->created && !state->lost && generation == state->generation && video_session_context(state, &session_context) == 0) {
		(void)amdrdna_vcn_message_destroy(state->run_cpu + VIDEO_RUN_CREATE_AT, VIDEO_RUN_PAGE, state->handle);
		count = amdrdna_vcn_create_run((uint32_t *)(void *)(state->run_cpu + VIDEO_RUN_CREATE_RUN_AT), session_context, state->run_va + VIDEO_RUN_CREATE_AT);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		(void)amdrdna_vk_vcn_run(state->owner, state->run_va + VIDEO_RUN_CREATE_RUN_AT, count, VIDEO_RUN_MS, &elapsed_us);
	}

	/* The session's pictures and their time, when it decoded any. */
	if (state->pictures != 0U)
		kern_logf("amdrdna: vk: video: session %u: %llu pictures (%llu errors), engine %llu us (longest %llu us), waits for the GPU %llu us, feedback status on %llu (last 0x%x)\n",
		    (unsigned)state->handle,
		    (unsigned long long)state->pictures,
		    (unsigned long long)state->errors,
		    (unsigned long long)state->run_us,
		    (unsigned long long)state->run_us_max,
		    (unsigned long long)state->wait_us,
		    (unsigned long long)state->feedback_marked,
		    (unsigned)state->feedback_last);

	/* The binding lets go of its memory. */
	if (state->binding != NULL)
		error = amdrdna_vk_object_release(state->binding);

	/* The DPB, the context and the run (the node keeps them while the engine may touch them). */
	if (state->dpb_backing != NULL)
		amdrdna_vk_memory_detach(state->owner, state->dpb_backing, state->dpb_va, state->dpb_bytes);
	if (state->context_backing != NULL)
		amdrdna_vk_memory_detach(state->owner, state->context_backing, state->context_va, state->context_bytes);
	if (state->run_token != NULL)
		amdrdna_vk_gpu_free(state->owner, state->run_token);

	/* The device's counts. */
	if (state->counted)
		amdrdna_vk_vcn_give(state->owner, state->dpb_bytes + state->context_bytes);

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
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct video_parameters *parameters;
	int error;

	UNUSED_PARAMETER(session);

	/* Takes the parameters the registry kept. */
	parameters = payload;

	/* The sets of either codec, then the session it held. */
	video_parameters_free(parameters);
	video_h265_free(parameters);
	error = 0;
	if (parameters->session != NULL)
		error = amdrdna_vk_object_release(parameters->session);

	/* The object. */
	kern_free(parameters);
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
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
	uint32_t operation;
	int32_t result;

	/* Reads each chained type the library writes; any other fails the stream. */
	switch (type) {
	case VIDEO_TYPE_H264_PROFILE:
		/* The H.264 profile and the picture layout. */
		chain->has_h264_profile = true;
		chain->profile_idc = drv_i915_wire_read_u32(reader);
		chain->picture_layout = drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_H265_PROFILE:
		/* The H.265 profile. */
		chain->has_h265_profile = true;
		chain->profile_idc = drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_DECODE_USAGE:
		/* Usage hints, which the engine does not use. */
		(void)drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_PROFILE_LIST:
		/* [count][array]{profile}: every profile must be one the engine takes; the first refusal is kept. */
		chain->has_profile_list = true;
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (count > (uint64_t)(reader->size - reader->offset))
			reader->error = 1;
		chain->profile_count = (uint32_t)count;
		chain->profiles_supported = true;
		for (item = 0U; item < count && reader->error == 0; item++) {
			result = video_read_profile(reader, &operation, &value);
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
		/* [present]{reference}: the reference's H.264 information. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_reference = true;
			video_h264_read_reference(reader, &chain->reference);
		}

		break;
	case VIDEO_TYPE_H264_PICTURE:
		/* [present]{picture}[slice count][array]{offset}. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_picture = true;
			video_h264_read_picture(reader, &chain->picture);
		}

		/* The slices' offsets. */
		video_read_offsets(reader, chain);
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
			video_read_chain(reader, &nested, depth + 1U);
			video_chain_free(&nested);
			video_h265_read_add(reader, chain);
		}

		break;
	case VIDEO_TYPE_H265_PARAMETERS_ADD:
		/* The sets added. */
		video_h265_read_add(reader, chain);
		break;
	case VIDEO_TYPE_H265_DPB_SLOT:
		/* [present]{reference}: the reference's H.265 information. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_h265_reference = true;
			video_h265_read_reference(reader, &chain->h265_reference);
		}

		break;
	case VIDEO_TYPE_H265_PICTURE:
		/* [present]{picture}[slice segment count][array]{offset}. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_h265_picture = true;
			video_h265_read_picture(reader, &chain->h265_picture);
		}

		/* The slice segments' offsets. */
		video_read_offsets(reader, chain);
		break;
	default:
		/* A structure the library does not write here. */
		reader->error = 1;
		break;
	}
}

/*
 * Reads a profile structure: its type, its chain and its four fields.
 * Returns 0 for a profile the engine takes, or the standard reason
 * another is refused; its codec operation goes to operation, the codec's
 * profile it names to profile_idc.
 */
static int32_t
video_read_profile(
	struct i915_wire_reader *reader,
	uint32_t *operation,
	uint32_t *profile_idc)
{
	struct video_chain chain;
	uint32_t type;
	uint32_t chroma;
	uint32_t luma;
	uint32_t chroma_depth;
	int32_t result;

	/* The type, then the chain that carries the codec's profile, then the fields. */
	kern_memset(&chain, 0, sizeof(chain));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_PROFILE)
		reader->error = 1;
	video_read_chain(reader, &chain, 0U);
	*operation = drv_i915_wire_read_u32(reader);
	chroma = drv_i915_wire_read_u32(reader);
	luma = drv_i915_wire_read_u32(reader);
	chroma_depth = drv_i915_wire_read_u32(reader);
	video_chain_free(&chain);

	/* Decides the profile. */
	result = video_profile_result(*operation, chroma, luma, chroma_depth, &chain);
	*profile_idc = chain.profile_idc;

	/* Succeeded: reports the profile's result. */
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
	/* H.264 or H.265 decode is the operation. */
	if (operation != VIDEO_OP_DECODE_H264 && operation != VIDEO_OP_DECODE_H265)
		return VIDEO_ERROR_OPERATION;

	/* 8-bit 4:2:0 is the one format. */
	if (chroma != VIDEO_CHROMA_420 ||
	    luma != VIDEO_DEPTH_8 ||
	    chroma_depth != VIDEO_DEPTH_8)
		return VIDEO_ERROR_FORMAT;

	/* H.265: the H.265 record must be there, of Main. */
	if (operation == VIDEO_OP_DECODE_H265) {
		if (!chain->has_h265_profile)
			return VIDEO_ERROR_CODEC;
		if (chain->profile_idc != VIDEO_PROFILE_H265_MAIN)
			return VIDEO_ERROR_CODEC;
		return 0;
	}

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
	video_read_chain(reader, &chain, 0U);
	video_chain_free(&chain);

	/* The coded area, the layer and the view. */
	resource->x = (int32_t)drv_i915_wire_read_u32(reader);
	resource->y = (int32_t)drv_i915_wire_read_u32(reader);
	resource->width = drv_i915_wire_read_u32(reader);
	resource->height = drv_i915_wire_read_u32(reader);
	resource->layer = drv_i915_wire_read_u32(reader);
	resource->view = drv_i915_wire_read_u64(reader);
}

/* Reads a reference slot structure: [type][chain: DPB slot of H.264 or H.265][index][present]{resource}. */
static void
video_read_slot(
	struct i915_wire_reader *reader,
	struct video_slot *slot)
{
	struct video_chain chain;
	uint64_t present;
	uint32_t type;

	/* The type and the chain, which carries the H.264 or H.265 reference information. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(slot, 0, sizeof(*slot));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_REFERENCE_SLOT)
		reader->error = 1;
	video_read_chain(reader, &chain, 0U);
	video_chain_free(&chain);
	slot->has_reference = chain.has_reference;
	slot->reference = chain.reference;
	slot->has_h265_reference = chain.has_h265_reference;
	slot->h265_reference = chain.h265_reference;

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
	video->slot_count = 0U;
	for (item = 0U; item < total && reader->error == 0; item++) {
		video_read_slot(reader, &video->slots[video->slot_count]);
		video->slot_count++;
	}
}

/*
 * Reads an H.264 add record's fields: [count][array]{SPS}[count][array]{PPS}.
 * Each set is a new allocation kept on the chain; a set the executor
 * cannot keep marks add_error.
 */
static void
video_read_add(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	struct amdrdna_vcn_h264_sps scratch_sps;
	struct video_pps_node scratch_pps;
	struct amdrdna_vcn_h264_sps *sps;
	struct video_pps_node *pps;
	uint64_t count;
	uint64_t item;

	/* The sequence parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* A set past the id range, or one that cannot be allocated, is read into scratch. */
		sps = NULL;
		if (chain->new_sps_count < VIDEO_SPS_IDS)
			sps = kern_calloc(1U, sizeof(*sps));
		if (sps == NULL) {
			sps = &scratch_sps;
			chain->add_error = ENOMEM;
		}

		/* The set, kept on the chain; an id outside H.264 marks the add malformed. */
		video_h264_read_sps(reader, sps);
		if (sps->id >= VIDEO_SPS_IDS)
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
			pps = &scratch_pps;
			chain->add_error = ENOMEM;
		}

		/* The set, kept on the chain; ids outside H.264 mark the add malformed. */
		video_h264_read_pps(reader, &pps->pps);
		if (pps->pps.sps_id >= VIDEO_SPS_IDS || pps->pps.pps_id >= VIDEO_PPS_IDS)
			chain->add_error = EINVAL;
		if (pps != &scratch_pps) {
			pps->next = chain->new_pps;
			chain->new_pps = pps;
		}
	}
}

/*
 * Reads a picture's slice offsets: [count][array]{offset}.  The engine
 * finds the slices by their start codes from the first one: only the
 * count, the first offset and whether they rise are kept.
 */
static void
video_read_offsets(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	uint64_t count;
	uint64_t item;
	uint32_t value;
	uint32_t last;

	/* The count, which the bytes left must hold. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;

	/* Each offset: the first kept, each later one past the one before. */
	chain->slice_count = 0U;
	last = 0U;
	for (item = 0U; item < count && reader->error == 0; item++) {
		value = drv_i915_wire_read_u32(reader);
		if (item == 0U)
			chain->first_slice = value;
		if (item > 0U && value <= last)
			chain->slices_disordered = true;
		last = value;
		chain->slice_count++;
	}
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

	/* The H.265 sets. */
	video_h265_chain_free(chain);
}

/*
 * Reports whether every new H.264 sequence set of a chain fits its
 * parameters object's session: its frame no wider and no taller than the
 * session's largest picture on whole macroblocks.
 */
static bool
video_sizes_fit(
	const struct video_parameters *parameters,
	const struct video_chain *chain)
{
	const struct video_session *state;
	const struct amdrdna_vcn_h264_sps *sps;
	uint32_t index;
	uint32_t width;
	uint32_t height;

	/* The session's largest picture on whole macroblocks. */
	state = parameters->session->payload;

	/* Each new set's frame. */
	for (index = 0U; index < chain->new_sps_count; index++) {
		/* The width in macroblocks, the height in map units (two macroblocks each for field pictures). */
		sps = chain->new_sps[index];
		width = (sps->pic_width_in_mbs_minus1 + 1U) * VIDEO_MB;
		height = (sps->pic_height_in_map_units_minus1 + 1U) * VIDEO_MB;
		if ((sps->flags & AMDRDNA_VCN_SPS_FRAME_MBS_ONLY) == 0U)
			height *= 2U;

		/* Refuses a frame larger than the session's. */
		if (width > video_mbs(state->width) * VIDEO_MB || height > video_mbs(state->height) * VIDEO_MB)
			return false;
	}

	/* Succeeded: every set fits. */
	return true;
}

/*
 * Moves a chain's new sets into a parameters object.
 *
 * With replace, a set replaces the object's of the same key (a create);
 * without, a key already held refuses the whole add (an update) and
 * nothing is taken.  The object's capacity bounds the sets.
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
	const struct amdrdna_vcn_h264_pps *held;
	uint32_t index;
	uint32_t id;
	uint32_t sps_count;
	uint32_t pps_count;

	/* An update must not repeat a key the object holds. */
	if (!replace) {
		/* Nor a sequence set id it holds. */
		for (index = 0U; index < chain->new_sps_count; index++) {
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

	/* The sequence sets held and the new ones of new ids. */
	sps_count = 0U;
	for (index = 0U; index < VIDEO_SPS_IDS; index++) {
		if (parameters->sps[index] != NULL)
			sps_count++;
	}
	for (index = 0U; index < chain->new_sps_count; index++) {
		if (parameters->sps[chain->new_sps[index]->id] == NULL)
			sps_count++;
	}

	/* The picture sets held, and the new ones of new keys. */
	pps_count = 0U;
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
	for (index = 0U; index < chain->new_sps_count; index++) {
		id = chain->new_sps[index]->id;
		kern_free(parameters->sps[id]);
		parameters->sps[id] = chain->new_sps[index];
		chain->new_sps[index] = NULL;
	}
	chain->new_sps_count = 0U;

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
	for (index = 0U; index < VIDEO_SPS_IDS; index++) {
		if (source->sps[index] == NULL)
			continue;
		destination->sps[index] = kern_calloc(1U, sizeof(*destination->sps[index]));
		if (destination->sps[index] == NULL)
			return ENOMEM;
		*destination->sps[index] = *source->sps[index];
	}

	/* The picture sets. */
	for (pps = source->pps; pps != NULL; pps = pps->next) {
		copy = kern_calloc(1U, sizeof(*copy));
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
	for (index = 0U; index < VIDEO_SPS_IDS; index++) {
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
static const struct amdrdna_vcn_h264_pps *
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

/*
 * Decodes one of the four video commands into its structure and holds
 * what it names.  A decode, a control and an end take the session and
 * parameters of the coding scope their command buffer has open.
 */
static int
video_record_coding(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_command_buffer *command,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record,
	struct video_record *video)
{
	struct video_chain chain;
	uint64_t present;
	uint64_t session_id;
	uint64_t parameters_id;
	uint64_t buffer_id;
	uint32_t type;
	int error;

	/* Reads the structure's presence, its type and its chain (a decode's carries its picture and slices). */
	kern_memset(&chain, 0, sizeof(chain));
	present = drv_i915_wire_read_u64(reader);
	type = drv_i915_wire_read_u32(reader);
	if (present != 1U)
		reader->error = 1;
	video_read_chain(reader, &chain, 0U);
	video_chain_free(&chain);
	session_id = 0U;
	parameters_id = 0U;
	buffer_id = 0U;

	/* Reads the fields of the command's structure. */
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

		/* Reads the reference slots, then the picture and the slices the chain carried. */
		video_read_slots(reader, video, AMDRDNA_VCN_H264_REFERENCES);
		video->has_picture = chain.has_picture;
		video->picture = chain.picture;
		video->has_h265_picture = chain.has_h265_picture;
		video->h265_picture = chain.h265_picture;
		video->slice_count = chain.slice_count;
		video->first_slice = chain.first_slice;
		if (chain.slices_disordered)
			video->skip = "slices out of order";
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
		error = video_hold(session, record, I915_VK_OBJ_VIDEO_SESSION, session_id, &video->session);
		if (error == 0 && parameters_id != 0U)
			error = video_hold(session, record, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, parameters_id, &video->parameters);
	} else {
		error = video_scope(command, record, video);
	}

	/* Propagates a refused hold. */
	if (error != 0)
		return error;

	/* A decode holds its bitstream and its output's view (the references' pictures are the engine's DPB's). */
	if (opcode == GPU_OP_CMD_DECODE_VIDEO) {
		error = video_hold(session, record, I915_VK_OBJ_BUFFER, buffer_id, &video->buffer);
		if (error == 0)
			error = video_hold(session, record, I915_VK_OBJ_IMAGE_VIEW, video->destination.view, &video->view);
		if (error != 0)
			return error;
	}

	/* Succeeded: the command holds what it names. */
	return 0;
}

/*
 * Decodes the begin or the end of a query: vkCmdBeginQuery [pool][query]
 * [flags], vkCmdEndQuery [pool][query]; the command holds its pool (its
 * object 0), whose query it names (also the record's first, a begin's
 * flags its value 0).  A timestamp pool's queries are not begun or ended.
 */
static int
video_record_query(
	struct amdrdna_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_record *record,
	struct video_record *video)
{
	const struct amdrdna_vk_query_pool *pool;
	uint64_t pool_id;
	int error;

	/* The pool and the query, kept in the record too, and a begin's flags (an occlusion query's precision). */
	pool_id = drv_i915_wire_read_u64(reader);
	video->query = drv_i915_wire_read_u32(reader);
	video->query_count = 1U;
	record->first = video->query;
	if (opcode == GPU_OP_CMD_BEGIN_QUERY)
		record->values[0] = drv_i915_wire_read_u32(reader);

	/* A stream that did not decode is unreadable. */
	if (reader->error != 0)
		return EINVAL;

	/* The pool, held; the query must be its. */
	error = video_hold(session, record, I915_VK_OBJ_QUERY_POOL, pool_id, &video->pool);
	if (error != 0)
		return error;
	pool = video->pool->payload;
	if (video->query >= pool->count || pool->type == VK_QUERY_TYPE_TIMESTAMP)
		return EINVAL;

	/* Succeeded: the command holds its pool. */
	return 0;
}

/* Finds a published object of a kind and holds it in the record's objects and in *holder; EINVAL for none. */
static int
video_hold(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_record *record,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	struct amdrdna_vk_object **holder)
{
	struct amdrdna_vk_object *object;

	/* The object. */
	object = amdrdna_vk_object_find(session, kind, identity);
	if (object == NULL)
		return EINVAL;

	/* Held while the record lives. */
	return video_keep(record, object, holder);
}

/* Holds an object in the record's objects (released when the record is dropped) and in *holder. */
static int
video_keep(
	struct amdrdna_vk_record *record,
	struct amdrdna_vk_object *object,
	struct amdrdna_vk_object **holder)
{
	int error;

	/* The record's objects have room for every object a video command holds. */
	if (record->object_count >= AMDRDNA_VK_SETS_MAX)
		return ENOTSUP;

	/* Held while the record lives. */
	error = amdrdna_vk_object_retain(object);
	if (error != 0)
		return error;
	record->objects[record->object_count] = object;
	record->object_count++;

	/* Succeeded: the holder has it. */
	*holder = object;
	return 0;
}

/*
 * Takes the session and parameters of the coding scope the command
 * buffer has open (its last begin with no end after it), held by this
 * record too.  EINVAL outside a scope: the API's rules put these
 * commands inside one.
 */
static int
video_scope(
	struct amdrdna_vk_command_buffer *command,
	struct amdrdna_vk_record *record,
	struct video_record *video)
{
	const struct video_record *begin;
	const struct amdrdna_vk_record *earlier;
	uint32_t index;
	int error;

	/* The last begin or end, from the newest record back. */
	begin = NULL;
	for (index = command->record_count; index > 0U; index--) {
		earlier = &command->records[index - 1U];
		if (earlier->opcode == GPU_OP_CMD_END_VIDEO_CODING)
			break;
		if (earlier->opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING) {
			begin = earlier->regions;
			break;
		}
	}

	/* Outside a scope, or a scope that kept no session. */
	if (begin == NULL || begin->session == NULL)
		return EINVAL;

	/* The scope's session, and its parameters when the begin names them. */
	error = video_keep(record, begin->session, &video->session);
	if (error == 0 && begin->parameters != NULL)
		error = video_keep(record, begin->parameters, &video->parameters);
	if (error != 0)
		return error;

	/* Succeeded: the command has its scope's objects. */
	return 0;
}

/*
 * Runs a control: a reset starts the stream over, in the firmware too
 * (its session is created again), and no slot holds a picture any more.
 */
static void
video_run_control(
	struct amdrdna_vk_session *session,
	const struct video_record *video)
{
	struct video_session *state;
	uint32_t slot;
	int error;

	UNUSED_PARAMETER(session);

	/* Only a reset changes anything. */
	if ((video->flags & VIDEO_CONTROL_RESET) == 0U)
		return;
	state = video->session->payload;

	/* No slot holds a picture, no reference picture is known, and the pictures are numbered from 0. */
	for (slot = 0U; slot < AMDRDNA_VCN_H264_SLOTS; slot++)
		state->slot_valid[slot] = false;
	state->h264_previous = false;
	state->serial = 0U;

	/* The firmware's session made anew; a failure is told by the next decode. */
	error = video_create(state);
	if (error != 0)
		video_skip("the engine did not take the session's reset");
}

/*
 * Runs a decode of the session's codec: checks it, builds its message,
 * scaling table, feedback and indirect buffer in the session's run, and
 * has the engine run it in the session's address space.  *status gets
 * the result status: complete when the engine ended the picture, else an
 * error (a picture the checks refuse is not run).  Returns 0.
 */
static int
video_run_decode(
	struct amdrdna_vk_session *session,
	const struct video_record *video,
	uint64_t wait_us,
	int32_t *status)
{
	struct video_session *state;
	const struct video_parameters *parameters;
	const struct amdrdna_vk_image *image;
	const struct amdrdna_vk_buffer *buffer;
	struct amdrdna_vcn_h264_decode h264;
	struct amdrdna_vcn_h265_decode h265;
	struct amdrdna_vcn_frame frame;
	struct amdrdna_vcn_buffers buffers;
	uint64_t elapsed_us;
	uint64_t target;
	uint64_t bitstream;
	uint8_t *cpu;
	uint32_t generation;
	uint32_t gap_frames;
	uint32_t gap_first;
	int32_t gap_poc;
	uint32_t feedback;
	uint32_t width;
	uint32_t height;
	unsigned count;
	const char *reason;
	int error;

	UNUSED_PARAMETER(session);

	/* The session's state and its parameters; the picture counts as the session's. */
	state = video->session->payload;
	parameters = NULL;
	if (video->parameters != NULL)
		parameters = video->parameters->payload;
	state->pictures++;
	state->wait_us += wait_us;
	*status = VIDEO_STATUS_ERROR;

	/* The setup slot's picture goes now: it is the picture's once it is decoded. */
	if (video->has_setup && video->setup.index >= 0 && video->setup.index < (int32_t)AMDRDNA_VCN_H264_SLOTS)
		state->slot_valid[video->setup.index] = false;

	/* A firmware session the engine lost with a restart is made again; its slots' pictures are gone. */
	generation = amdrdna_vk_vcn_generation(state->owner);
	if (!state->created || generation != state->generation) {
		kern_memset(state->slot_valid, 0, sizeof(state->slot_valid));
		state->h264_previous = false;
		error = video_create(state);
		if (error != 0) {
			state->errors++;
			video_skip("the engine did not create the session");
			return 0;
		}
	}

	/* Checks what the engine needs of the session's codec; a picture it cannot take is an error, not run. */
	kern_memset(&h264, 0, sizeof(h264));
	kern_memset(&h265, 0, sizeof(h265));
	reason = video->skip;
	if (reason == NULL && state->lost)
		reason = "the session lost its stream; a reset is needed";
	if (reason == NULL && state->operation == VIDEO_OP_DECODE_H265) {
		reason = video_h265_check(video, state, parameters, &h265);
	} else if (reason == NULL) {
		reason = video_check(video, state, parameters, &h264);
	}
	if (reason != NULL) {
		state->errors++;
		video_skip(reason);
		return 0;
	}

	/* The output's and the bitstream's addresses (the checks found them bound). */
	image = ((const struct amdrdna_vk_view *)video->view->payload)->image->payload;
	buffer = video->buffer->payload;
	(void)amdrdna_vk_image_address(image, &target, &cpu);
	(void)amdrdna_vk_buffer_address(buffer, &bitstream, &cpu);

	/* The picture's size: an H.265 sequence's in samples, an H.264 sequence's in whole macroblocks. */
	if (state->operation == VIDEO_OP_DECODE_H265) {
		width = h265.sps->width;
		height = h265.sps->height;
	} else {
		width = (h264.sps->pic_width_in_mbs_minus1 + 1U) * VIDEO_MB;
		height = (h264.sps->pic_height_in_map_units_minus1 + 1U) * VIDEO_MB;
	}

	/* What the decode body says of the picture and its buffers. */
	kern_memset(&frame, 0, sizeof(frame));
	frame.handle = state->handle;
	frame.width = width;
	frame.height = height;
	frame.max_width = state->width;
	frame.max_height = state->height;
	frame.bitstream_bytes = (uint32_t)(video->range - video->first_slice);
	frame.dpb_bytes = (uint32_t)state->dpb_bytes;
	frame.context_bytes = (uint32_t)state->context_bytes;
	frame.target_bytes = (uint32_t)image->bytes;
	frame.pitch = image->pitch;
	frame.chroma_offset = (uint32_t)image->chroma_offset;

	/* The buffers every decode of the picture names besides its message's bitstream. */
	(void)video_session_context(state, &buffers.session_context);
	buffers.message = state->run_va + VIDEO_RUN_MESSAGE_AT;
	buffers.dpb = state->dpb_va;
	buffers.context = state->context_va;
	buffers.bitstream = bitstream + video->offset + video->first_slice;
	buffers.target = target;
	buffers.feedback = state->run_va + VIDEO_RUN_FEEDBACK_AT;
	buffers.scaling = state->run_va + VIDEO_RUN_SCALING_AT;

	/*
	 * The frames a gap in frame_num before an H.264 picture implies, put
	 * into the engine's DPB first: the engine keeps the reference marking
	 * from the slices and does not fill a gap itself.
	 */
	gap_frames = 0U;
	if (state->operation != VIDEO_OP_DECODE_H265) {
		gap_frames = video_h264_gap(state, &h264, &gap_first, &gap_poc);
		error = video_h264_fill_gap(state, &h264, &frame, &buffers, gap_frames, gap_first, gap_poc);
		if (error != 0) {
			if (error == ETIMEDOUT || error == EFAULT)
				state->lost = true;
			state->errors++;
			video_skip("the engine did not decode the frames of a gap in frame_num");
			return 0;
		}
	}
	frame.serial = state->serial;
	state->serial++;

	/* The message of the session's codec and the scaling table, then the feedback cleared. */
	if (state->operation == VIDEO_OP_DECODE_H265) {
		error = amdrdna_vcn_message_decode_h265(state->run_cpu + VIDEO_RUN_MESSAGE_AT, VIDEO_RUN_PAGE, &frame, &h265,
		    state->run_cpu + VIDEO_RUN_SCALING_AT, AMDRDNA_VCN_SCALING_BYTES);
	} else {
		error = amdrdna_vcn_message_decode_h264(state->run_cpu + VIDEO_RUN_MESSAGE_AT, VIDEO_RUN_PAGE, &frame, &h264,
		    state->run_cpu + VIDEO_RUN_SCALING_AT, AMDRDNA_VCN_SCALING_BYTES);
	}
	if (error != 0) {
		state->errors++;
		video_skip("the picture's message could not be built");
		return 0;
	}
	amdrdna_vcn_feedback_init(state->run_cpu + VIDEO_RUN_FEEDBACK_AT);

	/* The indirect buffer that names the decode's buffers. */
	count = amdrdna_vcn_decode_run((uint32_t *)(void *)(state->run_cpu + VIDEO_RUN_DECODE_AT), &buffers);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The engine runs it in the session's address space, waited for at most VIDEO_RUN_MS. */
	error = amdrdna_vk_vcn_run(state->owner, state->run_va + VIDEO_RUN_DECODE_AT, count, VIDEO_RUN_MS, &elapsed_us);
	state->run_us += elapsed_us;
	if (elapsed_us > state->run_us_max)
		state->run_us_max = elapsed_us;

	/* The first pictures' feedback, for the log, and the pictures whose feedback carries a status, counted. */
	feedback = amdrdna_vcn_feedback_status(state->run_cpu + VIDEO_RUN_FEEDBACK_AT);
	if (video_trace_lines < VIDEO_TRACE_PICTURES) {
		video_trace_lines++;
		kern_logf("amdrdna: vk: video: picture %u of session %u: %d in %llu us, feedback status 0x%x\n",
		    (unsigned)frame.serial,
		    (unsigned)state->handle,
		    error,
		    (unsigned long long)elapsed_us,
		    (unsigned)feedback);
	}
	if (error == 0 && feedback != 0U) {
		state->feedback_marked++;
		state->feedback_last = feedback;
	}

	/* A run that did not end or faulted loses the stream's state until a reset. */
	if (error == ETIMEDOUT || error == EFAULT)
		state->lost = true;
	if (error != 0) {
		state->errors++;
		video_skip("the engine did not decode the picture");
		return 0;
	}

	/* The setup slot holds the picture now, and an H.264 reference picture is the last one. */
	if (video->has_setup)
		state->slot_valid[video->setup.index] = true;
	if (state->operation != VIDEO_OP_DECODE_H265)
		video_h264_decoded(state, &h264, gap_frames);

	/* Succeeded: the picture is in its image. */
	*status = VIDEO_STATUS_COMPLETE;
	return 0;
}

/*
 * Finds the frames a gap in frame_num before an H.264 picture implies
 * (7.4.3, 8.2.5.2): a picture other than an IDR picture of a sequence that
 * allows gaps, whose frame_num is neither the last reference picture's
 * (PrevRefFrameNum) nor the next.  Only the last max_num_ref_frames of
 * them can stay in the DPB, and only those are counted.  *first gets the
 * frame_num of the first counted, *poc the order count they are given:
 * of a type 0 sequence the last reference picture's (its low bits leave
 * the next picture's count as it was), else the counts frames of those
 * numbers would have as references (two a frame).  Returns how many
 * frames, 0 for no gap.
 */
static uint32_t
video_h264_gap(
	const struct video_session *state,
	const struct amdrdna_vcn_h264_decode *h264,
	uint32_t *first,
	int32_t *poc)
{
	const struct amdrdna_vcn_h264_reference *reference;
	uint32_t max_frame_num;
	uint32_t previous;
	uint32_t count;
	uint32_t keep;
	uint32_t index;
	int32_t previous_poc;
	int32_t current_poc;

	/* No gap until one is found. */
	*first = 0U;
	*poc = 0;

	/* An IDR picture, a sequence without gaps, or no reference picture decoded yet: no gap. */
	if ((h264->picture.flags & AMDRDNA_VCN_PICTURE_IDR) != 0U)
		return 0U;
	if ((h264->sps->flags & AMDRDNA_VCN_SPS_GAPS) == 0U)
		return 0U;
	if (!state->h264_previous)
		return 0U;

	/*
	 * PrevRefFrameNum: the last reference picture's frame_num, or 0 when
	 * the picture's own operation 5 made it 0 (it is then a short-term
	 * reference of FrameNum 0 under its tag); its order count likewise as
	 * the picture names it.
	 */
	previous = state->h264_previous_frame_num;
	previous_poc = state->h264_previous_poc;
	for (index = 0U; index < h264->reference_count; index++) {
		reference = &h264->references[index];
		if (reference->tag != state->h264_previous_tag)
			continue;
		if ((reference->flags & (AMDRDNA_VCN_REFERENCE_LONG_TERM | AMDRDNA_VCN_REFERENCE_MISSING)) != 0U)
			continue;
		if (reference->frame_num == 0U)
			previous = 0U;
		previous_poc = reference->poc[0];
	}

	/* The same frame_num as the last reference picture's, or the next: no gap. */
	max_frame_num = 1U << (h264->sps->log2_max_frame_num_minus4 + 4U);
	if (h264->picture.frame_num == previous || h264->picture.frame_num == (previous + 1U) % max_frame_num)
		return 0U;

	/* The numbers skipped, the last of them the DPB keeps. */
	count = (h264->picture.frame_num + max_frame_num - previous - 1U) % max_frame_num;
	keep = h264->sps->max_num_ref_frames;
	if (keep < 1U)
		keep = 1U;
	if (count > keep)
		count = keep;
	*first = (h264->picture.frame_num + max_frame_num - count) % max_frame_num;

	/* Their order count: the last reference picture's for type 0, else as references counted back from the picture's. */
	if (h264->sps->pic_order_cnt_type == 0U) {
		*poc = previous_poc;
	} else {
		current_poc = h264->picture.poc[0];
		if ((h264->picture.flags & AMDRDNA_VCN_PICTURE_REFERENCE) == 0U)
			current_poc++;
		*poc = current_poc - 2 * (int32_t)count;
	}

	/* Succeeded: the frames of the gap. */
	return count;
}

/*
 * Decodes the frames a gap implies (video_h264_gap: frames of them from
 * the frame_num first on, with the order count poc and two more for each
 * after it) before an H.264 picture: for each, a slice whose macroblocks
 * are all skipped as a reference P frame, under a tag none of the
 * picture's references and the picture use, into the picture's target
 * (the picture overwrites it), with the picture's message otherwise
 * (base and buffers, the bitstream the slice's).  Returns 0, or the
 * error of a slice that could not be written or a run of the engine.
 */
static int
video_h264_fill_gap(
	struct video_session *state,
	const struct amdrdna_vcn_h264_decode *h264,
	const struct amdrdna_vcn_frame *base,
	const struct amdrdna_vcn_buffers *buffers,
	uint32_t frames,
	uint32_t first,
	int32_t poc)
{
	struct amdrdna_vcn_h264_pps pps;
	struct amdrdna_vcn_h264_decode gap;
	struct amdrdna_vcn_frame frame;
	struct amdrdna_vcn_buffers run;
	uint64_t elapsed_us;
	uint32_t max_frame_num;
	uint32_t used;
	uint32_t index;
	uint32_t tag;
	size_t length;
	unsigned count;
	int error;

	/* No frames, nothing to decode. */
	if (frames == 0U)
		return 0;

	/* The tags the picture and its references use. */
	used = 1U << h264->tag;
	for (index = 0U; index < h264->reference_count; index++)
		used |= 1U << h264->references[index].tag;

	/* The frames' picture set, and their message apart from the frame number, the order count and the tag. */
	amdrdna_vcn_h264_gap_pps(h264->pps, &pps);
	kern_memset(&gap, 0, sizeof(gap));
	gap.sps = h264->sps;
	gap.pps = &pps;
	gap.picture.flags = AMDRDNA_VCN_PICTURE_REFERENCE;
	gap.picture.sps_id = h264->picture.sps_id;
	gap.picture.pps_id = h264->picture.pps_id;
	max_frame_num = 1U << (h264->sps->log2_max_frame_num_minus4 + 4U);
	frame = *base;
	run = *buffers;
	run.bitstream = state->run_va + VIDEO_RUN_GAP_AT;
	tag = 0U;

	/* Each frame in frame_num order. */
	for (index = 0U; index < frames; index++) {
		/* Its number and its order count (two more a frame after the first, but for type 0). */
		gap.picture.frame_num = (first + index) % max_frame_num;
		gap.picture.poc[0] = poc;
		if (h264->sps->pic_order_cnt_type != 0U)
			gap.picture.poc[0] = poc + 2 * (int32_t)index;
		gap.picture.poc[1] = gap.picture.poc[0];

		/* Its tag: the next one no one uses, or the picture's own when every tag is used. */
		while (tag < AMDRDNA_VCN_H264_LOOSE_TAG && (used & (1U << tag)) != 0U)
			tag++;
		gap.tag = h264->tag;
		if (tag < AMDRDNA_VCN_H264_LOOSE_TAG)
			gap.tag = tag;
		used |= 1U << gap.tag;

		/* Its slice. */
		length = amdrdna_vcn_h264_gap_slice(state->run_cpu + VIDEO_RUN_GAP_AT, VIDEO_RUN_PAGE, h264->sps, &pps,
		    gap.picture.frame_num, gap.picture.poc[0]);
		if (length == 0U)
			return EINVAL;

		/* Its message, the feedback cleared, and the indirect buffer. */
		frame.serial = state->serial;
		state->serial++;
		frame.bitstream_bytes = (uint32_t)length;
		error = amdrdna_vcn_message_decode_h264(state->run_cpu + VIDEO_RUN_MESSAGE_AT, VIDEO_RUN_PAGE, &frame, &gap,
		    state->run_cpu + VIDEO_RUN_SCALING_AT, AMDRDNA_VCN_SCALING_BYTES);
		if (error != 0)
			return error;
		amdrdna_vcn_feedback_init(state->run_cpu + VIDEO_RUN_FEEDBACK_AT);
		count = amdrdna_vcn_decode_run((uint32_t *)(void *)(state->run_cpu + VIDEO_RUN_DECODE_AT), &run);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);

		/* The engine decodes it, waited for as a picture is. */
		error = amdrdna_vk_vcn_run(state->owner, state->run_va + VIDEO_RUN_DECODE_AT, count, VIDEO_RUN_MS, &elapsed_us);
		state->run_us += elapsed_us;
		if (elapsed_us > state->run_us_max)
			state->run_us_max = elapsed_us;
		if (error != 0)
			return error;
	}

	/* Succeeded: the engine's DPB holds the frames of the gap. */
	return 0;
}

/*
 * Notes an H.264 picture the engine decoded: a reference picture is the
 * last one (its frame_num, tag and first order count); after a gap
 * (frames of it decoded first) a picture that is not one leaves the
 * gap's last frame as the last reference picture.
 */
static void
video_h264_decoded(
	struct video_session *state,
	const struct amdrdna_vcn_h264_decode *h264,
	uint32_t frames)
{
	uint32_t max_frame_num;

	/* A reference picture: the last one now. */
	if ((h264->picture.flags & (AMDRDNA_VCN_PICTURE_REFERENCE | AMDRDNA_VCN_PICTURE_IDR)) != 0U) {
		state->h264_previous = true;
		state->h264_previous_frame_num = h264->picture.frame_num;
		state->h264_previous_tag = h264->tag;
		state->h264_previous_poc = h264->picture.poc[0];
		return;
	}

	/* A picture that is not one after a gap: the gap's last frame, which no tag names. */
	if (frames != 0U) {
		max_frame_num = 1U << (h264->sps->log2_max_frame_num_minus4 + 4U);
		state->h264_previous_frame_num = (h264->picture.frame_num + max_frame_num - 1U) % max_frame_num;
		state->h264_previous_tag = AMDRDNA_VCN_H264_LOOSE_TAG + 1U;
	}
}

/*
 * Checks an H.264 decode against what the engine takes and the
 * session's limits, and fills what the H.264 body is made from.  Returns NULL when
 * it can run, else why the picture is not run.
 */
static const char *
video_check(
	const struct video_record *video,
	const struct video_session *state,
	const struct video_parameters *parameters,
	struct amdrdna_vcn_h264_decode *h264)
{
	const struct amdrdna_vcn_h264_sps *sps;
	const struct amdrdna_vcn_h264_pps *pps;
	const struct video_slot *slot;
	const char *reason;
	uint32_t profile;
	uint32_t index;
	uint32_t width;
	uint32_t height;
	int error;

	/* The picture's information and its sets. */
	if (!video->has_picture)
		return "no H.264 picture information";
	if (parameters == NULL || video->picture.sps_id >= VIDEO_SPS_IDS)
		return "no parameter sets";
	sps = parameters->sps[video->picture.sps_id];
	pps = video_find_pps(parameters, video->picture.sps_id, video->picture.pps_id);
	if (sps == NULL || pps == NULL)
		return "a parameter set the session does not hold";

	/* A progressive 8-bit 4:2:0 frame of a profile and a level the engine takes. */
	if (sps->chroma_format_idc != 1U ||
	    sps->bit_depth_luma_minus8 != 0U ||
	    sps->bit_depth_chroma_minus8 != 0U ||
	    (sps->flags & AMDRDNA_VCN_SPS_SEPARATE_PLANES) != 0U)
		return "not 8-bit 4:2:0";
	if ((sps->flags & AMDRDNA_VCN_SPS_FRAME_MBS_ONLY) == 0U)
		return "a sequence of field pictures";
	if ((video->picture.flags & (AMDRDNA_VCN_PICTURE_FIELD | AMDRDNA_VCN_PICTURE_PAIR)) != 0U)
		return "a field picture";
	error = amdrdna_vcn_h264_profile(sps, &profile);
	if (error != 0)
		return "a profile the engine does not decode";
	if (amdrdna_vcn_h264_level(sps->level_idc) == 0U)
		return "a level outside the standard's";

	/* Within the session's largest picture. */
	width = (sps->pic_width_in_mbs_minus1 + 1U) * VIDEO_MB;
	height = (sps->pic_height_in_map_units_minus1 + 1U) * VIDEO_MB;
	if (width > video_mbs(state->width) * VIDEO_MB || height > video_mbs(state->height) * VIDEO_MB)
		return "a picture larger than the session's";

	/* The bitstream and the output. */
	reason = video_check_bitstream(video);
	if (reason == NULL)
		reason = video_check_output(video, state, width, height);
	if (reason != NULL)
		return reason;

	/* The picture's tag: its setup slot, or the loose tag without one. */
	h264->sps = sps;
	h264->pps = pps;
	h264->picture = video->picture;
	h264->tag = AMDRDNA_VCN_H264_LOOSE_TAG;
	if (video->has_setup)
		h264->tag = (uint32_t)video->setup.index;

	/* The references: within the session's, each of a slot that holds a picture (or one the stream lost). */
	if (video->slot_count > state->references)
		return "more references than the session's";
	for (index = 0U; index < video->slot_count; index++) {
		/* A slot of the session's, with its H.264 information. */
		slot = &video->slots[index];
		if (slot->index < 0 || slot->index >= (int32_t)state->slots)
			return "a reference outside the slots";
		if (!slot->has_reference)
			return "a reference without its H.264 information";
		if (!state->slot_valid[slot->index] && (slot->reference.flags & AMDRDNA_VCN_REFERENCE_MISSING) == 0U)
			return "a reference to a slot that holds no picture";

		/* The reference under its slot's tag. */
		h264->references[index] = slot->reference;
		h264->references[index].tag = (uint32_t)slot->index;
	}
	h264->reference_count = video->slot_count;

	/* Succeeded: the decode can run. */
	return NULL;
}

/*
 * Checks a decode's output: a two-plane video image bound to memory that
 * holds a width by height picture (its rows past its height are the
 * engine's, AMDRDNA_VK_VIDEO_ROWS at a time), and, with a setup slot, the
 * same picture resource as the setup slot's (the output is the reference
 * picture).  Returns NULL when it can be written, else why not.
 */
static const char *
video_check_output(
	const struct video_record *video,
	const struct video_session *state,
	uint32_t width,
	uint32_t height)
{
	const struct amdrdna_vk_view *view;
	const struct amdrdna_vk_image *image;
	const struct video_resource *destination;
	const struct video_resource *setup;
	uint64_t va;
	uint8_t *cpu;
	uint32_t rows;
	int error;

	/* A two-plane image the engine writes, bound to memory. */
	view = video->view->payload;
	image = view->image->payload;
	if (!image->planar || (image->usage & VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR) == 0U)
		return "an output that is not a video decode's picture";
	error = amdrdna_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return "an output without memory";

	/* Its rows hold the picture's whole macroblocks, its rows' bytes the width. */
	rows = (image->height + AMDRDNA_VK_VIDEO_ROWS - 1U) & ~(AMDRDNA_VK_VIDEO_ROWS - 1U);
	if (height > rows || width > image->pitch || image->bytes > 0xffffffffU)
		return "an output smaller than the picture";

	/* With a setup slot: the setup's picture is the output, a slot of the session's. */
	destination = &video->destination;
	if (video->has_setup) {
		setup = &video->setup.resource;
		if (video->setup.index < 0 || video->setup.index >= (int32_t)state->slots)
			return "a setup slot outside the slots";
		if (!video->setup.has_picture ||
		    setup->view != destination->view ||
		    setup->x != destination->x ||
		    setup->y != destination->y ||
		    setup->width != destination->width ||
		    setup->height != destination->height ||
		    setup->layer != destination->layer)
			return "a setup picture that is not the output";
	}

	/* The coded area starts at the picture's corner of the one layer. */
	if (destination->x != 0 || destination->y != 0 || destination->layer != 0U)
		return "an output area off the picture's corner";

	/* Succeeded: the output can be written. */
	return NULL;
}

/*
 * Checks a decode's bitstream: a bound buffer, a range inside it from an
 * offset on its boundary, at least one slice, the first one inside the
 * range.  Returns NULL when it can be read, else why not.
 */
static const char *
video_check_bitstream(
	const struct video_record *video)
{
	const struct amdrdna_vk_buffer *buffer;
	uint64_t va;
	uint8_t *cpu;
	int error;

	/* A bound buffer. */
	buffer = video->buffer->payload;
	error = amdrdna_vk_buffer_address(buffer, &va, &cpu);
	if (error != 0)
		return "a bitstream without memory";

	/* A range inside the buffer, from an offset on its boundary. */
	if (video->range == 0U || video->range > 0xffffffffU)
		return "a bitstream range the engine cannot take";
	if (video->offset > buffer->size || video->range > buffer->size - video->offset)
		return "a bitstream past its buffer";
	if ((video->offset & (VIDEO_BITSTREAM_ALIGN - 1U)) != 0U)
		return "a bitstream off its boundary";

	/* At least one slice, the first inside the range. */
	if (video->slice_count == 0U || video->first_slice >= video->range)
		return "no slice inside the bitstream's range";

	/* Succeeded: the bitstream can be read. */
	return NULL;
}

/*
 * Creates the firmware's session of a stream (again, after a reset or a
 * restart of the engine): the creation message for the session's largest
 * picture, run with the session context.  Returns 0, or EIO when the
 * session context is not bound or the engine did not take the message.
 */
static int
video_create(
	struct video_session *state)
{
	uint64_t session_context;
	uint64_t elapsed_us;
	uint32_t generation;
	uint32_t stream;
	unsigned count;
	int error;

	/* The session context the firmware keeps the stream in. */
	error = video_session_context(state, &session_context);
	if (error != 0)
		return EIO;

	/* The kind of stream the session's codec makes. */
	stream = AMDRDNA_VCN_STREAM_H264;
	if (state->operation == VIDEO_OP_DECODE_H265)
		stream = AMDRDNA_VCN_STREAM_H265;

	/* The creation message and the indirect buffer that hands it over. */
	generation = amdrdna_vk_vcn_generation(state->owner);
	error = amdrdna_vcn_message_create(state->run_cpu + VIDEO_RUN_CREATE_AT, VIDEO_RUN_PAGE, state->handle, stream, state->width, state->height);
	if (error != 0)
		return EIO;
	count = amdrdna_vcn_create_run((uint32_t *)(void *)(state->run_cpu + VIDEO_RUN_CREATE_RUN_AT), session_context, state->run_va + VIDEO_RUN_CREATE_AT);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The engine runs it in the session's address space. */
	error = amdrdna_vk_vcn_run(state->owner, state->run_va + VIDEO_RUN_CREATE_RUN_AT, count, VIDEO_RUN_MS, &elapsed_us);
	if (error != 0) {
		state->created = false;
		if (error == ETIMEDOUT || error == EFAULT)
			state->lost = true;
		return EIO;
	}

	/* The firmware has the stream, in this generation of the engine. */
	state->created = true;
	state->lost = false;
	state->generation = generation;

	/* Succeeded: the session is created. */
	return 0;
}

/* Reports the GPU address of a session's bound session context; EIO when it is not bound or has no storage. */
static int
video_session_context(
	const struct video_session *state,
	uint64_t *va)
{
	const struct amdrdna_vk_memory *memory;

	/* A bound binding of memory with storage. */
	if (state->binding == NULL)
		return EIO;
	memory = state->binding->payload;
	if (!memory->attached)
		return EIO;

	/* Succeeded: the memory's address moved by the binding's offset. */
	*va = memory->va + state->binding_offset;
	return 0;
}

/* Logs a skipped picture's reason, the first few times. */
static void
video_skip(
	const char *reason)
{
	/* Only the first ones. */
	if (video_skip_lines >= VIDEO_SKIP_LINES)
		return;
	video_skip_lines++;

	/* The reason. */
	kern_logf("amdrdna: vk: video: a picture is not decoded: %s\n", reason);
}

/* Reports how many macroblocks cover a number of pixels. */
static uint32_t
video_mbs(
	uint32_t pixels)
{
	/* Succeeded: whole macroblocks. */
	return (pixels + VIDEO_MB - 1U) / VIDEO_MB;
}
