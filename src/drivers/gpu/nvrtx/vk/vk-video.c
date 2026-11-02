/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's Vulkan Video H.264 and H.265 decode on the video decoder
 * (NVDEC).
 *
 * The wire is libvulkan's (userland/desktop/libvulkan/video.c), the same
 * the i915 and bcm2711 executors read: a structure is its type, its chain
 * (a presence word and the next structure, nested), then its fields; a
 * pointer is a presence word and what it points to; an array is a 64-bit
 * count and its elements; the one-bit flags of a StdVideo structure are
 * one word packed by name, a wider field a word of its own.
 *
 * The queries, sessions, parameters and result status query pools run at
 * once.  The four video commands and the three query commands are kept
 * in the command buffer's records, each in a structure of its own that
 * holds the objects it names (copied out of the wire: the wire's scratch
 * is reused by the next command).  A decode is run by the executor in its
 * place among the records: the graphics work before it has ended, and
 * the decode has ended before the next record runs, so what the commands
 * around it read and write is in order.  The decoder is stateless: each
 * decode is resolved here into the decoder's picture structure, slice
 * table and push buffer, in a run of the executor's memory the session
 * keeps, and the decoder's status says whether it went well.
 *
 * A use that breaks the API's rules (a decode outside a coding scope, too
 * many references) fails the recording.  A bitstream value the decoder
 * cannot take (a set it does not hold, a field picture, a slice past the
 * buffer) only skips the picture, with a log line and the ERROR status: an
 * application may hand an untrusted stream to the decoder.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-video.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * The structure types the video wire carries (the Vulkan registry's
 * values).
 */
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

/* The flags and enumerants of the video API answered or checked. */
#define VIDEO_OP_DECODE_H264			0x1U
#define VIDEO_OP_DECODE_H265			0x2U
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

/* The H.265 profile taken: Main. */
#define VIDEO_PROFILE_H265_MAIN			1U

/* The codec of a session: H.264 or H.265 decode. */
#define VIDEO_CODEC_H264			1U
#define VIDEO_CODEC_H265			2U

/* The video results the executor replies (the pinned header's values). */
#define VIDEO_ERROR_IMAGE_USAGE			(-1000023000)
#define VIDEO_ERROR_LAYOUT			(-1000023001)
#define VIDEO_ERROR_OPERATION			(-1000023002)
#define VIDEO_ERROR_FORMAT			(-1000023003)
#define VIDEO_ERROR_CODEC			(-1000023004)
#define VIDEO_ERROR_STD_VERSION			(-1000023005)

/* The query type of a result status pool, its results and the flags of its readback. */
#define VIDEO_QUERY_RESULT_STATUS		1000023000U
#define VIDEO_STATUS_COMPLETE			1
#define VIDEO_STATUS_ERROR			(-1)
#define VIDEO_QUERY_RESULT_64			0x1U
#define VIDEO_QUERY_RESULT_AVAILABILITY		0x4U
#define VIDEO_QUERY_REPLY_MAX			4096U
#define VIDEO_QUERY_MAX				64U

/*
 * The decode limits the executor reports and holds sessions to.
 *
 * Values referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/nvk_video_session.c and nvk_physical_device.c (the
 * video capabilities: 256-byte bitstream alignment, 16x16 granularity,
 * 48x16 to 4096x4096, 17 slots and 16 references, level 5.2).
 */
#define VIDEO_BITSTREAM_ALIGN			256U
#define VIDEO_GRANULARITY			16U
#define VIDEO_MIN_WIDTH				48U
#define VIDEO_MIN_HEIGHT			16U
#define VIDEO_MAX_SIDE				4096U
#define VIDEO_LEVEL_5_2				15U
#define VIDEO_MB				16U

/* The H.264 header the decoder was written against: VK_STD_vulkan_video_codec_h264_decode 1.0.0. */
#define VIDEO_STD_NAME				"VK_STD_vulkan_video_codec_h264_decode"
#define VIDEO_STD_VERSION			((1U << 22) | (0U << 12) | 0U)

/*
 * The H.265 decode's limits: the CTB's largest size as the access
 * granularity, the smallest picture, level 5.1 (the pinned header's
 * STD_VIDEO_H265_LEVEL_IDC_5_1), and the header it was written against:
 * VK_STD_vulkan_video_codec_h265_decode 1.0.0.
 */
#define VIDEO_H265_GRANULARITY			64U
#define VIDEO_H265_MIN_SIDE			64U
#define VIDEO_H265_LEVEL_5_1			8U
#define VIDEO_H265_STD_NAME			"VK_STD_vulkan_video_codec_h265_decode"
#define VIDEO_H265_STD_VERSION			((1U << 22) | (0U << 12) | 0U)

/* The parameter set id ranges, and the deepest chain a structure may carry. */
#define VIDEO_SPS_IDS				32U
#define VIDEO_PPS_IDS				256U
#define VIDEO_MAX_CHAIN				8U

/* The H.265 parameter set id ranges. */
#define VIDEO_H265_VPS_IDS			16U
#define VIDEO_H265_SPS_IDS			16U
#define VIDEO_H265_PPS_IDS			64U

/*
 * The sizes of the H.265 structures' fixed arrays as the wire sends them:
 * the sub-layers, the coded picture buffers of a sub-layer's HRD, the
 * short-term reference picture sets of a sequence, its long-term
 * pictures, a short-term set's pictures, the palette's entries, the
 * chroma offset lists and a picture's reference sets.
 *
 * Values referenced: Khronos Vulkan-Headers 1.4.309
 * vk_video/vulkan_video_codec_h265std.h (STD_VIDEO_H265_SUBLAYERS_LIST_SIZE,
 * STD_VIDEO_H265_CPB_CNT_LIST_SIZE, STD_VIDEO_H265_MAX_SHORT_TERM_REF_PIC_SETS,
 * STD_VIDEO_H265_MAX_LONG_TERM_REF_PICS_SPS, STD_VIDEO_H265_MAX_DPB_SIZE,
 * STD_VIDEO_H265_PREDICTOR_PALETTE_COMPONENTS_LIST_SIZE times
 * STD_VIDEO_H265_PREDICTOR_PALETTE_COMP_ENTRIES_LIST_SIZE,
 * STD_VIDEO_H265_CHROMA_QP_OFFSET_LIST_SIZE).
 */
#define VIDEO_H265_SUB_LAYERS			7U
#define VIDEO_H265_CPB_COUNT			32U
#define VIDEO_H265_SHORT_TERM_SETS		64U
#define VIDEO_H265_LONG_TERM_PICS		32U
#define VIDEO_H265_SET_PICTURES			16U
#define VIDEO_H265_PALETTE_ENTRIES		384U
#define VIDEO_H265_CHROMA_OFFSETS		6U

/* The slots a begin record lists at most (each slot and one picture without a slot). */
#define VIDEO_BEGIN_SLOTS			(NVRTX_VIDEO_SLOTS + 1U)

/*
 * A session's memory bindings at most: an H.264 session's three (the
 * co-located data, the macroblock history, the history), an H.265
 * session's two (the co-located data, the filter buffer).
 */
#define VIDEO_BINDINGS				3U
#define VIDEO_H264_BINDINGS			3U
#define VIDEO_H265_BINDINGS			2U
#define VIDEO_BIND_ALIGN			256U

/*
 * The session's run of the executor's memory, and where its parts are:
 * the picture structure, the slice table, the decoder's status, the
 * H.265 scaling lists and tile sizes, and the push buffer of one decode
 * (the decodes run one at a time).
 *
 * Value referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs (the 4096 bytes of the
 * status).
 */
#define VIDEO_RUN_BYTES				16384U
#define VIDEO_RUN_PIC_AT			0U
#define VIDEO_RUN_SLICES_AT			2048U
#define VIDEO_RUN_STATUS_AT			3072U
#define VIDEO_RUN_STATUS_BYTES			4096U
#define VIDEO_RUN_SCALING_AT			7168U
#define VIDEO_RUN_TILES_AT			8192U
#define VIDEO_RUN_PUSH_AT			12288U

/* The bytes of a slice segment the header's skip is counted in at most. */
#define VIDEO_H265_HEADER_BYTES			640U

/* How long the executor waits for one picture's decode. */
#define VIDEO_DECODE_US				2000000U

/* The log lines of skipped pictures a boot writes at most. */
#define VIDEO_SKIP_LINES			32U

/* The first pictures of a session whose decode is logged with nvrtx.hevc.trace=1, and the bytes of a line of it. */
#define VIDEO_TRACE_PICTURES		3U
#define VIDEO_TRACE_LINE		32U

/* One picture parameter set a parameters object holds; the object's sets are a list. */
struct video_pps_node {
	struct video_pps_node *next;
	struct nvrtx_video_pps pps;
};

/* One H.265 sequence parameter set a parameters object holds; the object's sets are a list. */
struct video_h265_sps_node {
	struct video_h265_sps_node *next;
	struct nvrtx_video_h265_sps sps;
};

/* One H.265 picture parameter set a parameters object holds; the object's sets are a list. */
struct video_h265_pps_node {
	struct video_h265_pps_node *next;
	struct nvrtx_video_h265_pps pps;
};

/*
 * One VkVideoSessionParametersKHR: the H.264 or H.265 sets of one
 * session (its codec's).  The H.265 video parameter sets are kept only
 * as the ids held (h265_vps, one bit each).
 *
 * It holds its session's object; the recorded decodes that use it hold it.
 */
struct video_parameters {
	struct nvrtx_vk_object *session;
	uint32_t codec;
	uint32_t max_vps;
	uint32_t max_sps;
	uint32_t max_pps;
	uint32_t update_sequence;
	struct nvrtx_video_sps *sps[VIDEO_SPS_IDS];
	struct video_pps_node *pps;
	uint32_t h265_vps;
	struct video_h265_sps_node *h265_sps;
	struct video_h265_pps_node *h265_pps;
};

/* One memory binding of a session: the memory it holds, where in it, and how many bytes. */
struct video_binding {
	struct nvrtx_vk_object *memory;
	uint64_t offset;
	uint64_t size;
};

/*
 * One VkVideoSessionKHR.
 *
 * It keeps its codec and the limits it was made with, the sizes and
 * bindings of its work buffers (and an H.265 session where its filter
 * buffer's parts are), the decoder's reference entries of its DPB slots
 * (H.264) and the number of the next picture, its run of the executor's
 * memory, and the scratch an H.265 decode resolves its scaling lists and
 * copies its slice header into.  The
 * entries and the number change only when the executor runs the
 * session's commands, under the node's lock.
 */
struct video_session {
	void *owner;
	uint32_t codec;
	uint32_t profile_idc;
	uint32_t width;
	uint32_t height;
	uint32_t slots;
	uint32_t references;
	uint32_t binding_count;
	struct nvrtx_nvdec_hevc_buffers hevc;
	uint32_t sizes[VIDEO_BINDINGS];
	struct video_binding bindings[VIDEO_BINDINGS];
	struct nvrtx_video_dpb dpb;
	uint32_t serial;
	struct nvrtx_nvdec_hevc_scaling scaling;
	uint8_t header[VIDEO_H265_HEADER_BYTES];
	void *run_token;
	uint8_t *run_cpu;
	uint64_t run_va;
};

/*
 * One result status query pool: each query's status and whether it is
 * available.  Changed only when the executor runs the commands that name
 * it, and read by vkGetQueryPoolResults, both under the node's lock.
 */
struct video_pool {
	uint32_t count;
	int32_t status[VIDEO_QUERY_MAX];
	uint8_t available[VIDEO_QUERY_MAX];
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
 * names a picture, the picture's view (held while the record lives) and
 * the slot's H.264 or H.265 reference information.
 */
struct video_slot {
	int32_t index;
	bool has_picture;
	struct video_resource resource;
	struct nvrtx_vk_object *view;
	bool has_reference;
	struct nvrtx_video_reference reference;
	bool has_h265_reference;
	struct nvrtx_video_h265_reference h265_reference;
};

/*
 * One recorded video or query command, kept in its record's regions.
 *
 * It holds every object it names until its record is dropped: a begin its
 * session and parameters, a decode the session and parameters of its
 * coding scope, its bitstream buffer, its output view and its references'
 * views, a query command its pool.  skip is set for a decode the decoder
 * cannot take, which runs as a skipped picture.
 */
struct video_record {
	uint32_t opcode;
	struct nvrtx_vk_object *session;
	struct nvrtx_vk_object *parameters;
	struct nvrtx_vk_object *buffer;
	struct nvrtx_vk_object *view;
	struct nvrtx_vk_object *pool;
	uint32_t flags;
	uint64_t offset;
	uint64_t range;
	struct video_resource destination;
	bool has_setup;
	struct video_slot setup;
	uint32_t slot_count;
	struct video_slot slots[VIDEO_BEGIN_SLOTS];
	bool has_picture;
	struct nvrtx_video_picture picture;
	bool has_h265_picture;
	struct nvrtx_video_h265_picture h265_picture;
	uint32_t slice_count;
	uint32_t slices[NVRTX_NVDEC_SLICES_MAX];
	uint32_t query;
	uint32_t query_count;
	const char *skip;
};

/*
 * What the chained records of one structure carried.  It lives on the
 * stack of one command's decoding; the add record's sets are the
 * parameters object's to take, or freed with the chain.
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
	uint32_t max_sps;
	uint32_t max_pps;
	struct nvrtx_video_sps *new_sps[VIDEO_SPS_IDS];
	uint32_t new_sps_count;
	struct video_pps_node *new_pps;
	int add_error;
	bool has_picture;
	struct nvrtx_video_picture picture;
	uint32_t slice_count;
	uint32_t *slices;
	bool slices_overflow;
	bool has_reference;
	struct nvrtx_video_reference reference;
	bool has_h265_create;
	uint32_t max_vps;
	uint32_t new_h265_vps;
	struct video_h265_sps_node *new_h265_sps;
	struct video_h265_pps_node *new_h265_pps;
	bool has_h265_picture;
	struct nvrtx_video_h265_picture h265_picture;
	bool has_h265_reference;
	struct nvrtx_video_h265_reference h265_reference;
};

/*
 * The status query a coding scope has open while the executor runs a
 * command buffer: its pool's payload and its query, or NULL.  The records
 * run one command buffer at a time under the node's lock; a begin of a
 * coding scope clears it, a begin query sets it, an end query clears it.
 */
static struct video_pool *video_open_pool;

/* The open status query's index in video_open_pool; meaningful only while video_open_pool is not NULL. */
static uint32_t video_open_query;

/* The skipped pictures logged so far (guarded by the node's lock). */
static uint32_t video_skip_lines;

/*
 * The objects held for the boot after the decoder did not finish a
 * picture, and the bytes of memory they keep (guarded by the node's
 * lock): the decoder may still write them, so they are never freed.
 */
static uint32_t video_isolated_objects;
static uint64_t video_isolated_bytes;

static int video_capabilities(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static void video_reply_capabilities(struct i915_wire_writer *reply, const uint32_t *types, uint32_t count, uint32_t codec);
static int video_format_properties(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_family_properties(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_session_create(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_requirements(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_bind(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_parameters_create(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_parameters_update(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_pool_create(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_pool_results(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int video_session_destroy(struct nvrtx_vk_session *session, void *payload);
static int video_parameters_destroy(struct nvrtx_vk_session *session, void *payload);
static void video_read_chain(struct i915_wire_reader *reader, struct video_chain *chain, uint32_t depth);
static void video_read_fields(struct i915_wire_reader *reader, uint32_t type, struct video_chain *chain, uint32_t depth);
static int32_t video_read_profile(struct i915_wire_reader *reader, uint32_t *codec, uint32_t *profile_idc);
static int32_t video_profile_result(uint32_t operation, uint32_t chroma, uint32_t luma, uint32_t chroma_depth, const struct video_chain *chain);
static void video_read_resource(struct i915_wire_reader *reader, struct video_resource *resource);
static void video_read_slot(struct i915_wire_reader *reader, struct video_slot *slot);
static void video_read_slots(struct i915_wire_reader *reader, struct video_record *video, uint32_t limit);
static void video_read_add(struct i915_wire_reader *reader, struct video_chain *chain);
static void video_read_scaling(struct i915_wire_reader *reader, struct nvrtx_video_scaling *scaling);
static void video_read_extension(struct i915_wire_reader *reader, char *name, uint32_t *version);
static void video_read_offsets(struct i915_wire_reader *reader, struct video_chain *chain);
static void video_read_h265_picture(struct i915_wire_reader *reader, struct nvrtx_video_h265_picture *picture);
static void video_read_h265_add(struct i915_wire_reader *reader, struct video_chain *chain);
static uint32_t video_read_h265_vps(struct i915_wire_reader *reader);
static void video_read_h265_sps(struct i915_wire_reader *reader, struct nvrtx_video_h265_sps *sps);
static void video_read_h265_pps(struct i915_wire_reader *reader, struct nvrtx_video_h265_pps *pps);
static void video_read_h265_ptl(struct i915_wire_reader *reader);
static void video_read_h265_dpb(struct i915_wire_reader *reader);
static void video_read_h265_hrd(struct i915_wire_reader *reader, uint32_t sub_layers);
static void video_read_h265_sub_layers(struct i915_wire_reader *reader, uint32_t sub_layers);
static void video_read_h265_scaling(struct i915_wire_reader *reader, struct nvrtx_video_h265_scaling *scaling);
static void video_read_h265_vui(struct i915_wire_reader *reader, uint32_t sub_layers);
static void video_read_h265_short_term(struct i915_wire_reader *reader);
static void video_read_h265_long_term(struct i915_wire_reader *reader);
static void video_read_words(struct i915_wire_reader *reader, uint32_t *words, uint32_t expected);
static void video_read_fixed(struct i915_wire_reader *reader, void *bytes, uint32_t expected);
static void video_skip_words(struct i915_wire_reader *reader, uint32_t count);
static void video_chain_free(struct video_chain *chain);
static int video_parameters_take(struct video_parameters *parameters, struct video_chain *chain, bool replace);
static int video_h265_take(struct video_parameters *parameters, struct video_chain *chain, bool replace);
static int video_h265_copy(struct video_parameters *destination, const struct video_parameters *source);
static void video_h265_free(struct video_parameters *parameters);
static const struct nvrtx_video_h265_sps *video_find_h265_sps(const struct video_parameters *parameters, uint32_t vps_id, uint32_t sps_id);
static const struct nvrtx_video_h265_pps *video_find_h265_pps(const struct video_parameters *parameters, uint32_t vps_id, uint32_t sps_id, uint32_t pps_id);
static int video_parameters_copy(struct video_parameters *destination, const struct video_parameters *source);
static void video_parameters_free(struct video_parameters *parameters);
static const struct nvrtx_video_pps *video_find_pps(const struct video_parameters *parameters, uint32_t sps_id, uint32_t pps_id);
static int video_record_coding(struct nvrtx_vk_session *session, struct nvrtx_vk_command_buffer *command, uint32_t opcode, struct i915_wire_reader *reader, struct video_record *video);
static int video_record_query(struct nvrtx_vk_session *session, uint32_t opcode, struct i915_wire_reader *reader, struct video_record *video);
static int video_hold(struct nvrtx_vk_session *session, enum i915_vk_object_kind kind, uint64_t identity, struct nvrtx_vk_object **holder);
static int video_scope(struct nvrtx_vk_command_buffer *command, struct video_record *video);
static void video_release(struct video_record *video);
static void video_run_begin(const struct video_record *video);
static void video_run_control(const struct video_record *video);
static int video_run_decode(struct nvrtx_vk_session *session, const struct video_record *video);
static int video_run_decode_h265(struct nvrtx_vk_session *session, const struct video_record *video);
static const char *video_check_h265(const struct video_record *video, const struct video_session *state, const struct video_parameters *parameters, const struct nvrtx_video_h265_sps **sps_out, const struct nvrtx_video_h265_pps **pps_out);
static const char *video_check_resources(const struct video_record *video, const struct video_session *state, uint32_t width, uint32_t height, uint32_t granularity, uint32_t slots);
static int video_decoded(struct nvrtx_vk_session *session, const struct video_record *video, uint64_t push_va, size_t length);
static void video_isolate(const struct video_record *video);
static void video_hold_for_boot(struct nvrtx_vk_object *object, uint64_t bytes);
static const char *video_check(const struct video_record *video, const struct video_session *state, const struct video_parameters *parameters, const struct nvrtx_video_sps **sps_out, const struct nvrtx_video_pps **pps_out);
static const char *video_check_picture(const struct nvrtx_vk_object *view_object, const struct video_session *state, const struct nvrtx_vk_image **image_out);
static int video_plane_addresses(const struct nvrtx_vk_image *image, uint64_t *luma, uint64_t *chroma);
static int video_binding_address(const struct video_session *state, uint32_t index, uint64_t *va);
static void video_status(int32_t status);
static void video_skip(const char *reason);
static void video_trace(const char *what, uint32_t serial, const uint8_t *bytes, size_t length);
static uint32_t video_mbs(uint32_t pixels);

/*
 * Routes the video commands that run at once (the queries, the sessions
 * and parameters) and the result status query pools; *handled stays
 * clear for an opcode of another handler's.
 */
int
nvrtx_vk_video_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Only the video commands that are not recorded, and the pools. */
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
	case GPU_OP_CREATE_QUERY_POOL:
	case GPU_OP_DESTROY_QUERY_POOL:
	case GPU_OP_GET_QUERY_POOL_RESULTS:
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
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_VIDEO_SESSION);
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
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS);
		break;
	case GPU_OP_CREATE_QUERY_POOL:
		error = video_pool_create(session, reader, reply);
		break;
	case GPU_OP_DESTROY_QUERY_POOL:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_QUERY_POOL);
		break;
	default:
		error = video_pool_results(session, reader, reply);
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
nvrtx_vk_video_recorded(
	uint32_t opcode)
{
	/* The four video commands and the three query commands. */
	switch (opcode) {
	case GPU_OP_CMD_BEGIN_VIDEO_CODING:
	case GPU_OP_CMD_END_VIDEO_CODING:
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
	case GPU_OP_CMD_DECODE_VIDEO:
	case GPU_OP_CMD_BEGIN_QUERY:
	case GPU_OP_CMD_END_QUERY:
	case GPU_OP_CMD_RESET_QUERY_POOL:
		return true;
	default:
		break;
	}

	/* Another handler's. */
	return false;
}

/*
 * Decodes one recorded video or query command into a structure of its own
 * kept in the record's regions, holding the objects it names.
 *
 * command is the command buffer being recorded (NULL when there is none:
 * the command is decoded and dropped).  Returns EINVAL when the arguments
 * did not decode (the stream is then unreadable), another error when they
 * decoded but cannot be run (the recording fails with it).
 */
int
nvrtx_vk_video_record(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_command_buffer *command,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_record *record)
{
	struct video_record *video;
	int error;

	/* The command's own structure, which the record keeps. */
	video = kern_calloc(1U, sizeof(*video));
	if (video == NULL)
		return ENOMEM;

	/* Records the opcode and keeps the structure with the record. */
	video->opcode = opcode;
	record->regions = video;
	record->region_count = 0U;

	/* The command's arguments. */
	if (opcode == GPU_OP_CMD_BEGIN_QUERY ||
	    opcode == GPU_OP_CMD_END_QUERY ||
	    opcode == GPU_OP_CMD_RESET_QUERY_POOL) {
		error = video_record_query(session, opcode, reader, video);
	} else {
		error = video_record_coding(session, command, opcode, reader, video);
	}

	/* A command that decoded but breaks the API's rules fails the recording, not the stream. */
	if (error == EINVAL && reader->error == 0)
		error = ENOTSUP;
	if (error != 0)
		return error;

	/* Succeeded: the record keeps the command. */
	return 0;
}

/* Lets go of what a recorded video or query command holds, and frees its structure. */
void
nvrtx_vk_video_drop(
	struct nvrtx_vk_record *record)
{
	struct video_record *video;

	/* Takes the structure the record kept. */
	video = record->regions;

	/* Nothing was kept. */
	if (video == NULL)
		return;

	/* Its objects, then its structure. */
	video_release(video);
	kern_free(video);
	record->regions = NULL;
}

/*
 * Runs one recorded video or query command.  The executor has ended the
 * graphics work before a decode; the decode has ended when this returns.
 * Returns EIO when the decoder did not finish (the session's work is
 * lost), else 0: a picture the decoder cannot take is skipped.
 */
int
nvrtx_vk_video_run(
	struct nvrtx_vk_session *session,
	const struct nvrtx_vk_record *record)
{
	const struct video_record *video;
	struct video_pool *pool;
	uint32_t query;
	int error;

	/* Takes the structure the record kept. */
	video = record->regions;

	/* A record that kept nothing does nothing. */
	if (video == NULL)
		return 0;

	/* Each command's work. */
	error = 0;
	pool = NULL;
	if (video->pool != NULL)
		pool = video->pool->payload;

	/* Runs the command. */
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
	case GPU_OP_CMD_RESET_QUERY_POOL:
		/* The queries become unavailable, their statuses not ready. */
		for (query = video->query; pool != NULL && query < video->query + video->query_count; query++) {
			pool->status[query] = 0;
			pool->available[query] = 0U;
		}

		break;
	case GPU_OP_CMD_BEGIN_QUERY:
		/* The decodes of the scope report into the query from now on. */
		video_open_pool = pool;
		video_open_query = video->query;
		break;
	case GPU_OP_CMD_END_QUERY:
		/* The query is available with what the decodes reported. */
		if (pool != NULL)
			pool->available[video->query] = 1U;
		video_open_pool = NULL;
		break;
	default:
		break;
	}

	/* Reports a decoder that did not finish. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * vkGetPhysicalDeviceVideoCapabilitiesKHR: [physical][present]{profile}
 * [present][shape: type, link...] -> [result][present]{capabilities}.
 */
static int
video_capabilities(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t types[VIDEO_MAX_CHAIN];
	uint32_t count;
	uint32_t idc;
	uint32_t codec;
	uint64_t present;
	int32_t result;
	bool available;

	/* Skips the physical device and reads the profile, which decides the result and the codec. */
	(void)drv_i915_wire_read_u64(reader);
	present = drv_i915_wire_read_u64(reader);
	result = VIDEO_ERROR_OPERATION;
	codec = 0U;
	if (present != 0U)
		result = video_read_profile(reader, &codec, &idc);

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

	/* A device without the decoder takes no profile. */
	available = nvrtx_vk_video_available(session->owner);
	if (!available)
		result = VIDEO_ERROR_OPERATION;

	/* A profile the decoder does not take replies its reason only. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Replies the result and the records in the shape's order. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	video_reply_capabilities(reply, types, count, codec);

	/* Succeeded: the capabilities are reported. */
	return 0;
}

/*
 * Replies one output record of the capabilities' shape and, nested before
 * its fields, the rest; codec is the profile's.
 */
static void
video_reply_capabilities(
	struct i915_wire_writer *reply,
	const uint32_t *types,
	uint32_t count,
	uint32_t codec)
{
	char name[VK_MAX_EXTENSION_NAME_SIZE];

	/* The record's type, then the link to the rest of the shape. */
	drv_i915_wire_reply_u32(reply, types[0]);
	if (count > 1U) {
		drv_i915_wire_reply_u64(reply, 1U);
		video_reply_capabilities(reply, types + 1, count - 1U, codec);
	} else {
		drv_i915_wire_reply_u64(reply, 0U);
	}

	/* The record's fields. */
	switch (types[0]) {
	case VIDEO_TYPE_CAPABILITIES:
		/* Separate reference images, 256-byte bitstream offsets and sizes. */
		drv_i915_wire_reply_u32(reply, VIDEO_SEPARATE_REFERENCE_IMAGES);
		drv_i915_wire_reply_u64(reply, VIDEO_BITSTREAM_ALIGN);
		drv_i915_wire_reply_u64(reply, VIDEO_BITSTREAM_ALIGN);

		/* H.265: the CTB granularity, its sizes and slot limits, and its header's name. */
		kern_memset(name, 0, sizeof(name));
		if (codec == VIDEO_CODEC_H265) {
			drv_i915_wire_reply_u32(reply, VIDEO_H265_GRANULARITY);
			drv_i915_wire_reply_u32(reply, VIDEO_H265_GRANULARITY);
			drv_i915_wire_reply_u32(reply, VIDEO_H265_MIN_SIDE);
			drv_i915_wire_reply_u32(reply, VIDEO_H265_MIN_SIDE);
			drv_i915_wire_reply_u32(reply, VIDEO_MAX_SIDE);
			drv_i915_wire_reply_u32(reply, VIDEO_MAX_SIDE);
			drv_i915_wire_reply_u32(reply, NVRTX_VIDEO_H265_SLOTS);
			drv_i915_wire_reply_u32(reply, NVRTX_VIDEO_H265_REFERENCES);
			kern_memcpy(name, VIDEO_H265_STD_NAME, sizeof(VIDEO_H265_STD_NAME));
			drv_i915_wire_reply_u64(reply, VK_MAX_EXTENSION_NAME_SIZE);
			drv_i915_wire_reply_bytes(reply, name, sizeof(name));
			drv_i915_wire_reply_u32(reply, VIDEO_H265_STD_VERSION);
			break;
		}

		/* H.264: 16x16, its sizes and slot limits, and its header's name. */
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_GRANULARITY);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_WIDTH);
		drv_i915_wire_reply_u32(reply, VIDEO_MIN_HEIGHT);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_SIDE);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_SIDE);
		drv_i915_wire_reply_u32(reply, NVRTX_VIDEO_SLOTS);
		drv_i915_wire_reply_u32(reply, NVRTX_VIDEO_REFERENCES);
		kern_memcpy(name, VIDEO_STD_NAME, sizeof(VIDEO_STD_NAME));
		drv_i915_wire_reply_u64(reply, VK_MAX_EXTENSION_NAME_SIZE);
		drv_i915_wire_reply_bytes(reply, name, sizeof(name));
		drv_i915_wire_reply_u32(reply, VIDEO_STD_VERSION);
		break;
	case VIDEO_TYPE_DECODE_CAPABILITIES:
		/* The output picture is the reference picture. */
		drv_i915_wire_reply_u32(reply, VIDEO_DPB_AND_OUTPUT_COINCIDE);
		break;
	case VIDEO_TYPE_H264_CAPABILITIES:
		/* Level 5.2 and no field offset (progressive only). */
		drv_i915_wire_reply_u32(reply, VIDEO_LEVEL_5_2);
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
	struct nvrtx_vk_session *session,
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

	/* The profiles must all be ones the decoder takes; usages are the decode's output, reference and readback. */
	result = 0;
	video_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_SAMPLED_BIT;
	available = nvrtx_vk_video_available(session->owner);
	if (!available) {
		result = VIDEO_ERROR_OPERATION;
	} else if (!chain.has_profile_list || chain.profile_count == 0U) {
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

	/*
	 * Replies the one format: NV12, identity swizzle, its planes viewable
	 * in their own formats (mutable format, extended usage), 2D, optimal
	 * (block linear), decode output, reference, readback and sampled.
	 */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VIDEO_TYPE_FORMAT_PROPERTIES);
	drv_i915_wire_reply_u64(reply, 0U);
	drv_i915_wire_reply_u32(reply, NVRTX_VK_FORMAT_NV12);
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
 * [present][count]{operations}.  The one family decodes H.264 and H.265
 * when the device has the decoder.
 */
static int
video_family_properties(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint32_t count;
	uint32_t operations;
	bool available;

	/* Skips the physical device and reads the family count, which is the one family. */
	(void)drv_i915_wire_read_u64(reader);
	count = drv_i915_wire_read_u32(reader);
	if (reader->error != 0 || count != 1U)
		return EINVAL;

	/* H.264 and H.265 decode for family 0 with the decoder, nothing without it. */
	operations = 0U;
	available = nvrtx_vk_video_available(session->owner);
	if (available)
		operations = VIDEO_OP_DECODE_H264 | VIDEO_OP_DECODE_H265;
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u32(reply, operations);

	/* Succeeded: the family's codec operations are reported. */
	return 0;
}

/*
 * vkCreateVideoSessionKHR: [device][present]{create info}[allocator][present]
 * [identity] -> [result][present][identity].
 *
 * The session's work buffers are sized for its largest picture and its
 * slots; its run of the executor's memory is taken here, so a session
 * that cannot decode fails at once.
 */
static int
video_session_create(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_session *state;
	struct video_chain chain;
	struct nvrtx_nvdec_buffers buffers;
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
	uint32_t surfaces;
	uint32_t width_mbs;
	uint32_t height_mbs;
	uint32_t version;
	uint32_t profile_idc;
	uint32_t codec;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t max_slots;
	uint32_t max_references;
	uint32_t std_version;
	const char *std_name;
	int32_t result;
	int different;
	int error;

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
	profile_idc = 0U;
	codec = 0U;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		result = video_read_profile(reader, &codec, &profile_idc);
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
	identity = nvrtx_vk_create_tail(reader);
	video_chain_free(&chain);
	if (reader->error != 0 || identity == 0U)
		return EINVAL;

	/* The codec's limits and header. */
	min_width = VIDEO_MIN_WIDTH;
	min_height = VIDEO_MIN_HEIGHT;
	max_slots = NVRTX_VIDEO_SLOTS;
	max_references = NVRTX_VIDEO_REFERENCES;
	std_name = VIDEO_STD_NAME;
	std_version = VIDEO_STD_VERSION;
	if (codec == VIDEO_CODEC_H265) {
		min_width = VIDEO_H265_MIN_SIDE;
		min_height = VIDEO_H265_MIN_SIDE;
		max_slots = NVRTX_VIDEO_H265_SLOTS;
		max_references = NVRTX_VIDEO_H265_REFERENCES;
		std_name = VIDEO_H265_STD_NAME;
		std_version = VIDEO_H265_STD_VERSION;
	}

	/* Checks the session against what the decoder takes; the first reason is the result. */
	different = kern_strcmp(name, std_name);
	if (result != 0) {
		/* The profile's own reason. */
	} else if (family != 0U) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (flags != 0U) {
		result = VK_ERROR_FEATURE_NOT_PRESENT;
	} else if (picture_format != NVRTX_VK_FORMAT_NV12 || reference_format != NVRTX_VK_FORMAT_NV12) {
		result = VIDEO_ERROR_FORMAT;
	} else if (width < min_width ||
	    height < min_height ||
	    width > VIDEO_MAX_SIDE ||
	    height > VIDEO_MAX_SIDE) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (slots > max_slots || references > max_references) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (different != 0 || version > std_version) {
		result = VIDEO_ERROR_STD_VERSION;
	}

	/* Reports a session the decoder cannot run. */
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

	/* Fills in the session from the create call's fields. */
	state->owner = session->owner;
	state->codec = codec;
	state->profile_idc = profile_idc;
	state->width = width;
	state->height = height;
	state->slots = slots;
	state->references = references;
	nvrtx_video_dpb_reset(&state->dpb);

	/*
	 * The work buffers for the largest picture: co-located data for every
	 * surface a picture may be decoded into (the slots, or the references
	 * and the picture), the histories for its width.
	 */
	surfaces = slots;
	if (references + 1U > surfaces)
		surfaces = references + 1U;
	width_mbs = video_mbs(width);
	height_mbs = video_mbs(height);
	nvrtx_nvdec_buffer_sizes(width_mbs, height_mbs, surfaces - 1U, &buffers);
	state->binding_count = VIDEO_H264_BINDINGS;
	state->sizes[0] = buffers.coloc;
	state->sizes[1] = buffers.mbhist;
	state->sizes[2] = buffers.history;

	/*
	 * An H.265 session's: co-located data for each of the sixteen
	 * surfaces and one more, and the filter buffer for its largest
	 * picture.
	 */
	if (codec == VIDEO_CODEC_H265) {
		nvrtx_nvdec_hevc_buffer_sizes(width, height, NVRTX_NVDEC_HEVC_SURFACES + 1U, &state->hevc);
		state->binding_count = VIDEO_H265_BINDINGS;
		state->sizes[0] = state->hevc.coloc;
		state->sizes[1] = state->hevc.filter;
		state->sizes[2] = 0U;
	}

	/* The run the decodes' structures and push buffers are written into. */
	error = nvrtx_vk_gpu_alloc_system(session->owner, VIDEO_RUN_BYTES, &state->run_token, &state->run_cpu, &state->run_va);
	if (error != 0) {
		kern_free(state);
		kern_logf("nvrtx: vk: video: no memory for a session's run (%d)\n", error);
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_DEVICE_MEMORY);
		return 0;
	}

	/* Publishes the session and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_VIDEO_SESSION, identity, state, video_session_destroy, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkGetVideoSessionMemoryRequirementsKHR: [device][session][present][capacity]
 * [array] -> [result][present][count][array]{binding}.  The session's
 * work buffers (three for H.264, two for H.265), each on 256 bytes, of
 * the one memory type.
 */
static int
video_requirements(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct video_session *state;
	uint64_t identity;
	uint64_t bytes;
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

	/* An unknown session, or no room for the bindings, is refused. */
	state = nvrtx_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (state == NULL || capacity < state->binding_count) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	/* The bindings: each its type, no chain, its index, its size on the boundary, the boundary, memory type 0. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, state->binding_count);
	drv_i915_wire_reply_u64(reply, state->binding_count);
	for (index = 0U; index < state->binding_count; index++) {
		bytes = ((uint64_t)state->sizes[index] + VIDEO_BIND_ALIGN - 1U) & ~((uint64_t)VIDEO_BIND_ALIGN - 1U);
		drv_i915_wire_reply_u32(reply, VIDEO_TYPE_MEMORY_REQUIREMENTS);
		drv_i915_wire_reply_u64(reply, 0U);
		drv_i915_wire_reply_u32(reply, index);
		drv_i915_wire_reply_u64(reply, bytes);
		drv_i915_wire_reply_u64(reply, VIDEO_BIND_ALIGN);
		drv_i915_wire_reply_u32(reply, 1U << NVRTX_VK_MEMORY_TYPE);
	}

	/* Succeeded: the bindings are reported. */
	return 0;
}

/*
 * vkBindVideoSessionMemoryKHR: [device][session][count][array]{bind} ->
 * [result].  Each binding holds its memory, on its boundary and large
 * enough; a binding is bound once.
 */
static int
video_bind(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_session *state;
	struct nvrtx_vk_object *memory;
	const struct nvrtx_vk_memory *storage;
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
	state = nvrtx_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION, identity);
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

		/* A binding of the session's, not bound yet, on its boundary, of memory large enough. */
		memory = nvrtx_vk_object_find(session, I915_VK_OBJ_MEMORY, memory_id);
		storage = NULL;
		if (memory != NULL)
			storage = memory->payload;
		if (state == NULL || result != 0) {
			continue;
		} else if (index >= state->binding_count ||
		    storage == NULL ||
		    state->bindings[index].memory != NULL) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		} else if ((offset & (VIDEO_BIND_ALIGN - 1U)) != 0U ||
		    size < state->sizes[index] ||
		    offset > storage->bytes ||
		    size > storage->bytes - offset) {
			result = VK_ERROR_INITIALIZATION_FAILED;
			continue;
		}

		/* The binding holds its memory. */
		error = nvrtx_vk_object_retain(memory);
		if (error != 0) {
			result = VK_ERROR_OUT_OF_HOST_MEMORY;
			continue;
		}

		/* Binds the memory at its offset and size. */
		state->bindings[index].memory = memory;
		state->bindings[index].offset = offset;
		state->bindings[index].size = size;
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
 * create record must be of the session's codec.
 */
static int
video_parameters_create(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_parameters *parameters;
	const struct video_parameters *source;
	struct nvrtx_vk_object *owner;
	struct nvrtx_vk_object *template_object;
	const struct video_session *state;
	struct video_chain chain;
	uint64_t present;
	uint64_t identity;
	uint64_t template_identity;
	uint64_t session_identity;
	uint32_t type;
	uint32_t codec;
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
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0 || identity == 0U) {
		video_chain_free(&chain);
		return EINVAL;
	}

	/* The session must exist and its codec's create record be given; a template must be of the same session. */
	error = 0;
	source = NULL;
	codec = 0U;
	owner = nvrtx_vk_object_find(session, I915_VK_OBJ_VIDEO_SESSION, session_identity);
	if (owner != NULL) {
		state = owner->payload;
		codec = state->codec;
	}
	created = chain.has_h264_create;
	if (codec == VIDEO_CODEC_H265)
		created = chain.has_h265_create;
	template_object = NULL;
	if (template_identity != 0U)
		template_object = nvrtx_vk_object_find(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, template_identity);
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

	/* Holds the session the parameters belong to. */
	if (error == 0)
		error = nvrtx_vk_object_retain(owner);

	/* Records the session, its codec and the parameters' capacity once the session is held. */
	if (error == 0) {
		parameters->session = owner;
		parameters->codec = codec;
		parameters->max_vps = chain.max_vps;
		parameters->max_sps = chain.max_sps;
		parameters->max_pps = chain.max_pps;
	}

	/* Copies the template's sets, then adds the new ones, of the session's codec. */
	if (error == 0 && source != NULL && codec == VIDEO_CODEC_H265)
		error = video_h265_copy(parameters, source);
	if (error == 0 && source != NULL && codec != VIDEO_CODEC_H265)
		error = video_parameters_copy(parameters, source);
	if (error == 0 && codec == VIDEO_CODEC_H265)
		error = video_h265_take(parameters, &chain, true);
	if (error == 0 && codec != VIDEO_CODEC_H265)
		error = video_parameters_take(parameters, &chain, true);
	video_chain_free(&chain);

	/* A refused object goes with what it took. */
	if (error != 0 && parameters != NULL) {
		(void)video_parameters_destroy(session, parameters);
		parameters = NULL;
	}

	/* Publishes the object and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity, parameters, video_parameters_destroy, error);

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
	struct nvrtx_vk_session *session,
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
	parameters = nvrtx_vk_payload(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, identity);
	if (parameters == NULL ||
	    sequence != parameters->update_sequence + 1U ||
	    chain.add_error != 0)
		result = VK_ERROR_INITIALIZATION_FAILED;

	/* Adds the sets of the object's codec; a key already held refuses the whole update. */
	if (result == 0) {
		if (parameters->codec == VIDEO_CODEC_H265) {
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
 * vkCreateQueryPool: [device][present][type][chain 0][flags][query type]
 * [count][statistics][allocator][present][identity] -> [result][present]
 * [pool].  Only a result status pool of at most VIDEO_QUERY_MAX queries
 * is made; another kind is refused as a missing feature.
 */
static int
video_pool_create(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct video_pool *pool;
	uint64_t identity;
	uint32_t type;
	uint32_t count;
	int error;

	/* Decodes the type, the count and the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0 || identity == 0U)
		return EINVAL;

	/* Only a result status pool of a bounded size. */
	error = 0;
	if (type != VIDEO_QUERY_RESULT_STATUS ||
	    count == 0U ||
	    count > VIDEO_QUERY_MAX) {
		kern_logf("nvrtx: vk: vkCreateQueryPool refused: type %u, %u queries (result status only)\n", (unsigned)type, (unsigned)count);
		error = ENOTSUP;
	}

	/* The pool, every query unavailable. */
	pool = NULL;
	if (error == 0) {
		pool = kern_calloc(1U, sizeof(*pool));
		if (pool != NULL)
			pool->count = count;
	}

	/* Publishes the pool and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_QUERY_POOL, identity, pool, nvrtx_vk_free_payload, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkGetQueryPoolResults: [device][pool][first][count][dataSize][bytes]
 * [stride][flags] -> [result][bytes]{bytes}.  Each query's record is its
 * status and its availability, in words of 4 or 8 bytes, stride apart.
 * The decodes run before their submission ends, so a wait has nothing
 * more to wait for.
 */
static int
video_pool_results(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct video_pool *pool;
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
	pool = nvrtx_vk_payload(session, I915_VK_OBJ_QUERY_POOL, identity);
	if (pool == NULL ||
	    first > pool->count ||
	    count > pool->count - first ||
	    bytes > VIDEO_QUERY_REPLY_MAX ||
	    stride < record ||
	    stride > bytes ||
	    (count != 0U && (uint64_t)(count - 1U) * stride + record > bytes)) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_DEVICE_LOST);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* NOT_READY when any query of the range is unavailable. */
	result = 0U;
	for (query = first; query < first + count; query++) {
		if (pool->available[query] == 0U)
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

		/* The record's stride ends where the next record starts, or at the end of the data. */
		end = written - record + stride;
		if (end > bytes)
			end = bytes;

		/* Pads the stride with zeros, a chunk at a time. */
		while (written < end) {
			value = end - written;
			if (value > sizeof(zeros))
				value = sizeof(zeros);
			drv_i915_wire_reply_bytes(reply, zeros, (size_t)value);
			written += value;
		}
	}

	/* Zeros to the end of the data. */
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

/* Destroys a session once nothing holds it: its bindings' memory and its run. */
static int
video_session_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct video_session *state;
	uint32_t index;
	int error;
	int released;

	UNUSED_PARAMETER(session);

	/* Takes the state the registry kept. */
	state = payload;
	error = 0;

	/* The bindings let go of their memory. */
	for (index = 0U; index < VIDEO_BINDINGS; index++) {
		if (state->bindings[index].memory == NULL)
			continue;
		released = nvrtx_vk_object_release(state->bindings[index].memory);
		if (error == 0)
			error = released;
	}

	/* The run goes back once the session's runs have ended. */
	if (state->run_token != NULL)
		nvrtx_vk_gpu_free(state->owner, state->run_token);

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
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct video_parameters *parameters;
	int error;

	UNUSED_PARAMETER(session);

	/* Takes the parameters the registry kept. */
	parameters = payload;

	/* The sets, then the session it held. */
	video_parameters_free(parameters);
	video_h265_free(parameters);
	error = 0;
	if (parameters->session != NULL)
		error = nvrtx_vk_object_release(parameters->session);

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
	uint32_t codec;
	int32_t result;

	/* Reads each chained type the library writes; any other fails the stream. */
	switch (type) {
	case VIDEO_TYPE_H264_PROFILE:
		/* The H.264 profile and the picture layout. */
		chain->has_h264_profile = true;
		chain->profile_idc = drv_i915_wire_read_u32(reader);
		chain->picture_layout = drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_DECODE_USAGE:
		/* Usage hints, which the decoder does not use. */
		(void)drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_H265_PROFILE:
		/* The H.265 profile. */
		chain->has_h265_profile = true;
		chain->profile_idc = drv_i915_wire_read_u32(reader);
		break;
	case VIDEO_TYPE_PROFILE_LIST:
		/* [count][array]{profile}: every profile must be one the decoder takes; the first refusal is kept. */
		chain->has_profile_list = true;
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		chain->profile_count = (uint32_t)count;
		chain->profiles_supported = true;
		for (item = 0U; item < count && reader->error == 0; item++) {
			result = video_read_profile(reader, &codec, &value);
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
		/* [present]{flags, FrameNum, reserved, [2] order counts}: the reference's H.264 information. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_reference = true;
			chain->reference.flags = drv_i915_wire_read_u32(reader);
			chain->reference.frame_num = drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			count = drv_i915_wire_read_u64(reader);
			if (count != 2U)
				reader->error = 1;
			chain->reference.poc[0] = (int32_t)drv_i915_wire_read_u32(reader);
			chain->reference.poc[1] = (int32_t)drv_i915_wire_read_u32(reader);
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
			chain->picture.frame_num = drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			count = drv_i915_wire_read_u64(reader);
			if (count != 2U)
				reader->error = 1;
			chain->picture.poc[0] = (int32_t)drv_i915_wire_read_u32(reader);
			chain->picture.poc[1] = (int32_t)drv_i915_wire_read_u32(reader);
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
			video_read_h265_add(reader, chain);
		}

		break;
	case VIDEO_TYPE_H265_PARAMETERS_ADD:
		/* The sets added. */
		video_read_h265_add(reader, chain);
		break;
	case VIDEO_TYPE_H265_DPB_SLOT:
		/* [present]{flags, PicOrderCntVal}: the reference's H.265 information. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_h265_reference = true;
			chain->h265_reference.flags = drv_i915_wire_read_u32(reader);
			chain->h265_reference.poc = (int32_t)drv_i915_wire_read_u32(reader);
		}

		break;
	case VIDEO_TYPE_H265_PICTURE:
		/* [present]{picture}[slice segment count][array]{offset}. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_h265_picture = true;
			video_read_h265_picture(reader, &chain->h265_picture);
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
 * Returns 0 for a profile the decoder takes, or the standard reason another
 * is refused; its codec (0 for none taken) goes to codec, the H.264 or
 * H.265 profile it names to profile_idc.
 */
static int32_t
video_read_profile(
	struct i915_wire_reader *reader,
	uint32_t *codec,
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
	video_read_chain(reader, &chain, 0U);
	operation = drv_i915_wire_read_u32(reader);
	chroma = drv_i915_wire_read_u32(reader);
	luma = drv_i915_wire_read_u32(reader);
	chroma_depth = drv_i915_wire_read_u32(reader);
	video_chain_free(&chain);

	/* Decides the profile. */
	result = video_profile_result(operation, chroma, luma, chroma_depth, &chain);
	*profile_idc = chain.profile_idc;

	/* The codec of a profile taken. */
	*codec = 0U;
	if (result == 0 && operation == VIDEO_OP_DECODE_H264)
		*codec = VIDEO_CODEC_H264;
	if (result == 0 && operation == VIDEO_OP_DECODE_H265)
		*codec = VIDEO_CODEC_H265;

	/* Succeeded: reports the profile's result. */
	return result;
}

/* Decides whether a profile is one the decoder takes, or why not, in the standard order. */
static int32_t
video_profile_result(
	uint32_t operation,
	uint32_t chroma,
	uint32_t luma,
	uint32_t chroma_depth,
	const struct video_chain *chain)
{
	/* H.264 and H.265 decode are the operations. */
	if (operation != VIDEO_OP_DECODE_H264 && operation != VIDEO_OP_DECODE_H265)
		return VIDEO_ERROR_OPERATION;

	/* 8-bit 4:2:0 is the one format. */
	if (chroma != VIDEO_CHROMA_420 ||
	    luma != VIDEO_DEPTH_8 ||
	    chroma_depth != VIDEO_DEPTH_8)
		return VIDEO_ERROR_FORMAT;

	/* The H.265 record must be there and name Main. */
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

/* Reads a reference slot structure: [type][chain: DPB slot][index][present]{resource}. */
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
	struct nvrtx_video_sps scratch_sps;
	struct video_pps_node scratch_pps;
	struct nvrtx_video_sps *sps;
	struct video_pps_node *pps;
	uint64_t count;
	uint64_t item;
	uint64_t offsets;
	uint64_t present;
	uint32_t index;

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
			kern_memset(&scratch_sps, 0, sizeof(scratch_sps));
			sps = &scratch_sps;
			chain->add_error = ENOMEM;
		}

		/* The fields in declared order. */
		sps->flags = drv_i915_wire_read_u32(reader);
		sps->profile_idc = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
		sps->id = drv_i915_wire_read_u32(reader);
		sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
		sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);
		sps->log2_max_frame_num_minus4 = drv_i915_wire_read_u32(reader);
		sps->pic_order_cnt_type = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		sps->log2_max_pic_order_cnt_lsb_minus4 = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		sps->max_num_ref_frames = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		sps->pic_width_in_mbs_minus1 = drv_i915_wire_read_u32(reader);
		sps->pic_height_in_map_units_minus1 = drv_i915_wire_read_u32(reader);
		for (index = 0U; index < 4U; index++)
			(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);

		/* The picture order count cycle's offsets, at most 255, which the decoder does not need. */
		offsets = drv_i915_wire_read_u64(reader);
		if (offsets > 255U)
			reader->error = 1;
		for (index = 0U; index < (uint32_t)offsets && reader->error == 0; index++)
			(void)drv_i915_wire_read_u32(reader);

		/* The scaling lists. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			sps->has_scaling = true;
			video_read_scaling(reader, &sps->scaling);
		}

		/* The VUI, which is never sent. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U)
			reader->error = 1;

		/* Keeps the set on the chain; an id outside H.264 marks the add malformed. */
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
			kern_memset(&scratch_pps, 0, sizeof(scratch_pps));
			pps = &scratch_pps;
			chain->add_error = ENOMEM;
		}

		/* The fields in declared order. */
		pps->pps.flags = drv_i915_wire_read_u32(reader);
		pps->pps.sps_id = drv_i915_wire_read_u32(reader);
		pps->pps.pps_id = drv_i915_wire_read_u32(reader);
		pps->pps.num_ref_idx_l0_default_active_minus1 = drv_i915_wire_read_u32(reader);
		pps->pps.num_ref_idx_l1_default_active_minus1 = drv_i915_wire_read_u32(reader);
		pps->pps.weighted_bipred_idc = drv_i915_wire_read_u32(reader);
		pps->pps.pic_init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		pps->pps.chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
		pps->pps.second_chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			pps->pps.has_scaling = true;
			video_read_scaling(reader, &pps->pps.scaling);
		}

		/* Keeps the set on the chain; ids outside H.264 mark the add malformed. */
		if (pps->pps.sps_id >= VIDEO_SPS_IDS || pps->pps.pps_id >= VIDEO_PPS_IDS)
			chain->add_error = EINVAL;
		if (pps != &scratch_pps) {
			pps->next = chain->new_pps;
			chain->new_pps = pps;
		}
	}
}

/* Reads H.264 scaling lists: [present mask][default mask][96]{4x4}[384]{8x8}. */
static void
video_read_scaling(
	struct i915_wire_reader *reader,
	struct nvrtx_video_scaling *scaling)
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
	struct video_h265_sps_node *h265_sps;
	struct video_h265_pps_node *h265_pps;
	uint32_t index;

	/* The sequence sets. */
	for (index = 0U; index < chain->new_sps_count; index++) {
		kern_free(chain->new_sps[index]);
		chain->new_sps[index] = NULL;
	}

	/* The chain holds no sequence set any more; then the picture sets. */
	chain->new_sps_count = 0U;
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;
		kern_free(pps);
	}

	/* The H.265 sequence sets. */
	while (chain->new_h265_sps != NULL) {
		h265_sps = chain->new_h265_sps;
		chain->new_h265_sps = h265_sps->next;
		kern_free(h265_sps);
	}

	/* The H.265 picture sets. */
	while (chain->new_h265_pps != NULL) {
		h265_pps = chain->new_h265_pps;
		chain->new_h265_pps = h265_pps->next;
		kern_free(h265_pps);
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
video_parameters_take(
	struct video_parameters *parameters,
	struct video_chain *chain,
	bool replace)
{
	struct video_pps_node *pps;
	struct video_pps_node *old;
	struct video_pps_node **link;
	const struct nvrtx_video_pps *held;
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

	/* The sequence sets held and the new ones of new keys. */
	sps_count = 0U;
	for (index = 0U; index < VIDEO_SPS_IDS; index++) {
		if (parameters->sps[index] != NULL)
			sps_count++;
	}

	/* The new sequence sets of ids not held yet. */
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

	/* The chain holds no sequence set any more; the picture sets replace any of the same key. */
	chain->new_sps_count = 0U;
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
static const struct nvrtx_video_pps *
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
 * what it names.  A decode takes the session and parameters of the
 * coding scope its command buffer has open.
 */
static int
video_record_coding(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_command_buffer *command,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct video_record *video)
{
	struct video_chain chain;
	uint64_t present;
	uint64_t session_id;
	uint64_t parameters_id;
	uint64_t buffer_id;
	uint32_t type;
	uint32_t index;
	int error;

	/* Reads the structure's presence, its type and its chain (a decode's carries its picture and slices). */
	kern_memset(&chain, 0, sizeof(chain));
	chain.slices = video->slices;
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
		video_read_slots(reader, video, NVRTX_VIDEO_REFERENCES);
		video->has_picture = chain.has_picture;
		video->picture = chain.picture;
		video->has_h265_picture = chain.has_h265_picture;
		video->h265_picture = chain.h265_picture;
		video->slice_count = chain.slice_count;

		/*
		 * An H.264 decode with more slices than the table holds is
		 * skipped; an H.265 decode keeps the first ones (the decoder
		 * finds the segments by their start codes).
		 */
		if (chain.slices_overflow && !chain.has_h265_picture)
			video->skip = "more slices than the decoder's table";
		break;
	}

	/* A stream that did not decode is unreadable. */
	if (reader->error != 0)
		return EINVAL;

	/* Without a recording command buffer the command is decoded and dropped. */
	if (command == NULL)
		return 0;

	/* A begin holds its session and parameters; the others take them from the scope a begin opened. */
	error = 0;
	if (opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING) {
		error = video_hold(session, I915_VK_OBJ_VIDEO_SESSION, session_id, &video->session);
		if (error == 0 && parameters_id != 0U)
			error = video_hold(session, I915_VK_OBJ_VIDEO_SESSION_PARAMETERS, parameters_id, &video->parameters);
	} else {
		error = video_scope(command, video);
	}

	/* Propagates a refused hold. */
	if (error != 0)
		return error;

	/* A decode holds its bitstream, its output's view and its references' views. */
	if (opcode == GPU_OP_CMD_DECODE_VIDEO) {
		error = video_hold(session, I915_VK_OBJ_BUFFER, buffer_id, &video->buffer);
		if (error == 0)
			error = video_hold(session, I915_VK_OBJ_IMAGE_VIEW, video->destination.view, &video->view);

		/* Holds each reference's view, while nothing has failed. */
		for (index = 0U; error == 0 && index < video->slot_count; index++) {
			if (!video->slots[index].has_picture)
				return EINVAL;
			error = video_hold(session, I915_VK_OBJ_IMAGE_VIEW, video->slots[index].resource.view, &video->slots[index].view);
		}

		/* Propagates a refused hold. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the command holds what it names. */
	return 0;
}

/*
 * Decodes one of the three query commands: vkCmdBeginQuery [pool][query]
 * [flags], vkCmdEndQuery [pool][query], vkCmdResetQueryPool [pool][first]
 * [count]; the command holds its pool, within whose queries it lies.
 */
static int
video_record_query(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct video_record *video)
{
	const struct video_pool *pool;
	uint64_t pool_id;
	int error;

	/* The pool and the queries. */
	pool_id = drv_i915_wire_read_u64(reader);
	video->query = drv_i915_wire_read_u32(reader);
	video->query_count = 1U;

	/* A begin's flags word is not used. */
	if (opcode == GPU_OP_CMD_BEGIN_QUERY)
		(void)drv_i915_wire_read_u32(reader);

	/* A reset names how many queries it covers. */
	if (opcode == GPU_OP_CMD_RESET_QUERY_POOL)
		video->query_count = drv_i915_wire_read_u32(reader);

	/* A stream that did not decode is unreadable. */
	if (reader->error != 0)
		return EINVAL;

	/* The pool, held; the queries must be its. */
	error = video_hold(session, I915_VK_OBJ_QUERY_POOL, pool_id, &video->pool);
	if (error != 0)
		return error;

	/* Refuses a range of queries the pool does not have. */
	pool = video->pool->payload;
	if (video->query > pool->count || video->query_count > pool->count - video->query)
		return EINVAL;
	if (opcode != GPU_OP_CMD_RESET_QUERY_POOL && video->query >= pool->count)
		return EINVAL;

	/* Succeeded: the command holds its pool. */
	return 0;
}

/* Finds a published object of a kind and holds it in *holder; EINVAL for none. */
static int
video_hold(
	struct nvrtx_vk_session *session,
	enum i915_vk_object_kind kind,
	uint64_t identity,
	struct nvrtx_vk_object **holder)
{
	struct nvrtx_vk_object *object;
	int error;

	/* The object. */
	object = nvrtx_vk_object_find(session, kind, identity);
	if (object == NULL)
		return EINVAL;

	/* Held while the record lives. */
	error = nvrtx_vk_object_retain(object);
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
	struct nvrtx_vk_command_buffer *command,
	struct video_record *video)
{
	const struct video_record *begin;
	const struct nvrtx_vk_record *record;
	uint32_t index;
	int error;

	/* The last begin or end, from the newest record back. */
	begin = NULL;
	for (index = command->record_count; index > 0U; index--) {
		record = &command->records[index - 1U];
		if (record->opcode == GPU_OP_CMD_END_VIDEO_CODING)
			break;
		if (record->opcode == GPU_OP_CMD_BEGIN_VIDEO_CODING) {
			begin = record->regions;
			break;
		}
	}

	/* Outside a scope, or a scope that kept no session. */
	if (begin == NULL || begin->session == NULL)
		return EINVAL;

	/* The scope's session and parameters, held by this command too. */
	error = nvrtx_vk_object_retain(begin->session);
	if (error != 0)
		return error;

	/* Keeps the session, and the parameters when the begin names them. */
	video->session = begin->session;
	if (begin->parameters != NULL) {
		error = nvrtx_vk_object_retain(begin->parameters);
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

	/* The scope's session. */
	if (video->session != NULL)
		(void)nvrtx_vk_object_release(video->session);

	/* The scope's parameters. */
	if (video->parameters != NULL)
		(void)nvrtx_vk_object_release(video->parameters);

	/* The bitstream's buffer. */
	if (video->buffer != NULL)
		(void)nvrtx_vk_object_release(video->buffer);

	/* The output's view. */
	if (video->view != NULL)
		(void)nvrtx_vk_object_release(video->view);

	/* The query pool. */
	if (video->pool != NULL)
		(void)nvrtx_vk_object_release(video->pool);

	/* The references' views. */
	for (index = 0U; index < video->slot_count; index++) {
		if (video->slots[index].view != NULL)
			(void)nvrtx_vk_object_release(video->slots[index].view);
	}

	/* Holds nothing. */
	video->session = NULL;
	video->parameters = NULL;
	video->buffer = NULL;
	video->view = NULL;
	video->pool = NULL;
	video->slot_count = 0U;
}

/* Runs a begin: the slots the scope binds keep their reference entries, the others lose them. */
static void
video_run_begin(
	const struct video_record *video)
{
	struct video_session *state;
	bool active[NVRTX_VIDEO_SLOTS];
	uint32_t index;
	int32_t slot;

	/* No query is open at the start of a scope. */
	video_open_pool = NULL;

	/* A begin that kept no session binds nothing. */
	if (video->session == NULL)
		return;

	/* Takes the session's state. */
	state = video->session->payload;

	/* The slots listed with a picture are bound. */
	for (index = 0U; index < NVRTX_VIDEO_SLOTS; index++)
		active[index] = false;
	for (index = 0U; index < video->slot_count; index++) {
		slot = video->slots[index].index;
		if (slot >= 0 &&
		    slot < (int32_t)NVRTX_VIDEO_SLOTS &&
		    video->slots[index].has_picture)
			active[slot] = true;
	}

	/* Every other slot holds no reference any more. */
	nvrtx_video_dpb_keep(&state->dpb, active);
}

/* Runs a control: a reset clears the session's references and numbering. */
static void
video_run_control(
	const struct video_record *video)
{
	struct video_session *state;

	/* Only a reset changes anything, and only with a session. */
	if ((video->flags & VIDEO_CONTROL_RESET) == 0U || video->session == NULL)
		return;

	/* Takes the session's state. */
	state = video->session->payload;

	/* No slot holds a reference, and the pictures are numbered from 0. */
	nvrtx_video_dpb_reset(&state->dpb);
	state->serial = 0U;
}

/*
 * Runs a decode: checks it, resolves its surfaces and references, fills
 * the picture structure, the slice table and the push buffer in the
 * session's run, and has the decoder run it; the decoder's status gives
 * the open query its result.  A picture the decoder cannot take is
 * skipped with the ERROR status.  Returns EIO when the decoder did not
 * finish.
 *
 * Procedure referenced: Mesa main 921d028a5fff2beb4ec90451913b5fcc05fb1e6a
 * src/nouveau/vulkan/rust/video/decode/h264.rs (Decoder::decode and
 * set_reference_frames: a reference's surface is its slot, a picture
 * without a slot takes a free one, each reference keeps its entry).
 */
static int
video_run_decode(
	struct nvrtx_vk_session *session,
	const struct video_record *video)
{
	struct video_session *state;
	const struct video_parameters *parameters;
	const struct nvrtx_video_sps *sps;
	const struct nvrtx_video_pps *pps;
	const struct nvrtx_vk_image *image;
	const struct nvrtx_vk_buffer *buffer;
	const struct video_slot *slot;
	struct nvrtx_nvdec_h264_picture description;
	struct nvrtx_nvdec_frame frame;
	struct nvrtx_nvdec_h264_reference *reference;
	uint32_t surfaces[NVRTX_VIDEO_REFERENCES];
	uint32_t entry;
	uint32_t index;
	uint32_t errors;
	uint64_t bitstream;
	uint8_t *buffer_cpu;
	const char *reason;
	size_t length;
	int error;

	/* Takes the session's state; an H.265 session's decodes are the H.265 run's. */
	state = video->session->payload;
	if (state->codec == VIDEO_CODEC_H265)
		return video_run_decode_h265(session, video);
	parameters = NULL;
	if (video->parameters != NULL)
		parameters = video->parameters->payload;

	/* Checks what the decoder needs; a picture it cannot take is skipped. */
	reason = video->skip;
	if (reason == NULL)
		reason = video_check(video, state, parameters, &sps, &pps);
	if (reason != NULL) {
		video_skip(reason);
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* The picture's own description from its sets. */
	kern_memset(&description, 0, sizeof(description));
	kern_memset(&frame, 0, sizeof(frame));
	nvrtx_video_describe(sps, pps, &video->picture, &description);

	/* A picture into a slot is a new picture there: the slot's old entry goes. */
	if (video->has_setup && video->setup.index >= 0)
		nvrtx_video_dpb_forget(&state->dpb, (uint32_t)video->setup.index);

	/* Each reference: its slot is its surface, with its planes and its entry. */
	for (index = 0U; index < video->slot_count; index++) {
		slot = &video->slots[index];
		reference = &description.references[index];
		error = nvrtx_video_dpb_entry(&state->dpb, (uint32_t)slot->index, &entry);
		if (error == 0) {
			image = ((const struct nvrtx_vk_view *)slot->view->payload)->image->payload;
			error = video_plane_addresses(image, &frame.luma[slot->index], &frame.chroma[slot->index]);
		}

		/* Refuses a decode whose reference has no entry or no memory. */
		if (error != 0) {
			video_skip("a reference has no entry or no memory");
			video_status(VIDEO_STATUS_ERROR);
			return 0;
		}

		/* The reference as the picture structure lists it. */
		surfaces[index] = (uint32_t)slot->index;
		reference->surface = (uint32_t)slot->index;
		reference->long_term = 0U;
		if ((slot->reference.flags & NVRTX_VIDEO_REFERENCE_LONG_TERM) != 0U)
			reference->long_term = 1U;
		reference->non_existing = 0U;
		if ((slot->reference.flags & NVRTX_VIDEO_REFERENCE_MISSING) != 0U)
			reference->non_existing = 1U;
		reference->field_order[0] = nvrtx_video_order(slot->reference.poc[0], slot->reference.poc[1]);
		reference->field_order[1] = nvrtx_video_order(slot->reference.poc[1], slot->reference.poc[0]);
		reference->frame_index = slot->reference.frame_num;
		description.dpb_entry[index] = entry;
	}

	/* Counts the references the picture structure lists. */
	description.reference_count = video->slot_count;

	/* The picture's surface: its slot's, or a free one; the output's planes there. */
	if (video->has_setup && video->setup.index >= 0) {
		description.surface = (uint32_t)video->setup.index;
	} else {
		description.surface = nvrtx_video_free_surface(&state->dpb, surfaces, video->slot_count);
	}

	/* Resolves the output's planes in the picture's surface. */
	image = ((const struct nvrtx_vk_view *)video->view->payload)->image->payload;
	error = video_plane_addresses(image, &frame.luma[description.surface], &frame.chroma[description.surface]);
	if (error != 0) {
		video_skip("the output has no memory");
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* The bitstream, its slices, and the planes' rows. */
	buffer = video->buffer->payload;
	error = nvrtx_vk_buffer_address(buffer, &bitstream, &buffer_cpu);
	if (error != 0) {
		video_skip("the bitstream has no memory");
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* Describes the stream, its slices and the output's pitch. */
	description.stream_bytes = (uint32_t)video->range;
	description.slice_count = video->slice_count;
	description.pitch = image->pitch;

	/* The structures in the session's run. */
	nvrtx_nvdec_h264_pic(state->run_cpu + VIDEO_RUN_PIC_AT, &description);
	nvrtx_nvdec_slices((uint32_t *)(state->run_cpu + VIDEO_RUN_SLICES_AT), video->slices, video->slice_count, (uint32_t)video->range);
	kern_memset(state->run_cpu + VIDEO_RUN_STATUS_AT, 0, VIDEO_RUN_STATUS_BYTES);

	/* The frame's addresses: the structures, the bitstream, the session's buffers. */
	frame.pic = state->run_va + VIDEO_RUN_PIC_AT;
	frame.slices = state->run_va + VIDEO_RUN_SLICES_AT;
	frame.status = state->run_va + VIDEO_RUN_STATUS_AT;
	frame.bitstream = bitstream + video->offset;
	error = video_binding_address(state, 0U, &frame.coloc);
	if (error == 0)
		error = video_binding_address(state, 1U, &frame.mbhist);
	if (error == 0)
		error = video_binding_address(state, 2U, &frame.history);
	if (error != 0) {
		video_skip("the session's memory is not bound");
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* The picture's number, and the surface errors are concealed from. */
	frame.serial = state->serial;
	frame.error_frame = 0U;
	if (sps->max_num_ref_frames != 0U)
		frame.error_frame = state->serial % sps->max_num_ref_frames;

	/* The push buffer, run on the decoder and waited for. */
	length = nvrtx_nvdec_push_decode(state->run_cpu + VIDEO_RUN_PUSH_AT, &frame);
	error = video_decoded(session, video, state->run_va + VIDEO_RUN_PUSH_AT, length);
	if (error != 0)
		return error;

	/* The decoder's status: macroblocks in error make the picture's result an error. */
	errors = nvrtx_get32(state->run_cpu + VIDEO_RUN_STATUS_AT, NVRTX_NVDEC_STATUS_ERRORS_AT);
	if (errors != 0U) {
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* Succeeded: the picture is in its image. */
	video_status(VIDEO_STATUS_COMPLETE);
	return 0;
}

/*
 * Checks a decode against what the decoder takes, and finds its sets.
 * Returns NULL when it can run, else why the picture is skipped.
 */
static const char *
video_check(
	const struct video_record *video,
	const struct video_session *state,
	const struct video_parameters *parameters,
	const struct nvrtx_video_sps **sps_out,
	const struct nvrtx_video_pps **pps_out)
{
	const struct nvrtx_video_sps *sps;
	const struct nvrtx_video_pps *pps;
	const char *reason;
	uint32_t width;
	uint32_t height;
	uint32_t limit_width;
	uint32_t limit_height;

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
	if ((sps->flags & NVRTX_VIDEO_SPS_FRAME_MBS_ONLY) == 0U)
		return "a sequence of field pictures";
	if ((video->picture.flags & (NVRTX_VIDEO_PICTURE_FIELD | NVRTX_VIDEO_PICTURE_PAIR)) != 0U)
		return "a field picture";

	/* Within the session's largest picture. */
	width = (sps->pic_width_in_mbs_minus1 + 1U) * VIDEO_MB;
	height = (sps->pic_height_in_map_units_minus1 + 1U) * VIDEO_MB;
	limit_width = video_mbs(state->width) * VIDEO_MB;
	limit_height = video_mbs(state->height) * VIDEO_MB;
	if (width > limit_width || height > limit_height)
		return "a picture larger than the session's";

	/* The slices, the bitstream, the output and the references. */
	reason = video_check_resources(video, state, width, height, 1U, NVRTX_VIDEO_SLOTS);
	if (reason != NULL)
		return reason;

	/* Succeeded: the decode can run with these sets. */
	*sps_out = sps;
	*pps_out = pps;
	return NULL;
}

/*
 * Checks what every decode names: one to the table's slices, rising,
 * inside the range; the range inside its buffer, on its boundary; an
 * output video image of at least width by height once its sides are on
 * granularity (a power of two: the planes' rows are laid out on it);
 * references and a setup slot within slots, of video images.  Returns
 * NULL when they can be decoded, else why the picture is skipped.
 */
static const char *
video_check_resources(
	const struct video_record *video,
	const struct video_session *state,
	uint32_t width,
	uint32_t height,
	uint32_t granularity,
	uint32_t slots)
{
	const struct nvrtx_vk_buffer *buffer;
	const struct nvrtx_vk_image *image;
	const char *reason;
	uint32_t index;
	uint32_t image_width;
	uint32_t image_height;

	/* One to the table's slices, rising, inside the range. */
	if (video->slice_count == 0U || video->slice_count > NVRTX_NVDEC_SLICES_MAX)
		return "no slices, or more than the decoder's table";
	for (index = 0U; index < video->slice_count; index++) {
		if (video->slices[index] >= video->range)
			return "a slice past the bitstream's range";
		if (index > 0U && video->slices[index] <= video->slices[index - 1U])
			return "slices out of order";
	}

	/* Checks the bitstream range against the buffer. */
	buffer = video->buffer->payload;
	if (video->range == 0U || video->range > 0xffffffffU - 16U)
		return "a bitstream range the decoder cannot take";
	if (video->offset > buffer->size || video->range > buffer->size - video->offset)
		return "a bitstream past its buffer";
	if ((video->offset & (VIDEO_BITSTREAM_ALIGN - 1U)) != 0U)
		return "a bitstream off its boundary";

	/* The output and the references: video images that hold the picture. */
	reason = video_check_picture(video->view, state, &image);
	if (reason != NULL)
		return reason;
	image_width = (image->width + granularity - 1U) & ~(granularity - 1U);
	image_height = (image->height + granularity - 1U) & ~(granularity - 1U);
	if (image_width < width || image_height < height)
		return "an output smaller than the picture";
	for (index = 0U; index < video->slot_count; index++) {
		if (video->slots[index].index < 0 || video->slots[index].index >= (int32_t)slots)
			return "a reference outside the slots";
		reason = video_check_picture(video->slots[index].view, state, &image);
		if (reason != NULL)
			return reason;
	}

	/* Refuses a setup slot outside the slots. */
	if (video->has_setup && video->setup.index >= (int32_t)slots)
		return "a setup slot outside the slots";

	/* Succeeded: the resources can be decoded with. */
	return NULL;
}

/* Checks that a view is of a bound video image; NULL when it is, else why not. */
static const char *
video_check_picture(
	const struct nvrtx_vk_object *view_object,
	const struct video_session *state,
	const struct nvrtx_vk_image **image_out)
{
	const struct nvrtx_vk_view *view;
	const struct nvrtx_vk_image *image;

	UNUSED_PARAMETER(state);

	/* The view's image must be a bound video image. */
	view = view_object->payload;
	image = view->image->payload;
	if (!image->video)
		return "a picture that is not a video image";
	if (image->memory == NULL)
		return "a picture without memory";

	/* Succeeded: the image holds pictures. */
	*image_out = image;
	return NULL;
}

/* Reports the GPU addresses of a video image's luma and chroma planes. */
static int
video_plane_addresses(
	const struct nvrtx_vk_image *image,
	uint64_t *luma,
	uint64_t *chroma)
{
	uint64_t va;
	uint8_t *cpu;
	int error;

	/* The image's first byte. */
	error = nvrtx_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;

	/* Succeeded: each plane at its offset. */
	*luma = va + image->plane_offset[0];
	*chroma = va + image->plane_offset[1];
	return 0;
}

/* Reports the GPU address of one of a session's bound work buffers; EIO when it is not bound. */
static int
video_binding_address(
	const struct video_session *state,
	uint32_t index,
	uint64_t *va)
{
	const struct nvrtx_vk_memory *memory;

	/* A bound binding of memory with storage. */
	if (state->bindings[index].memory == NULL)
		return EIO;
	memory = state->bindings[index].memory->payload;
	if (!memory->attached)
		return EIO;

	/* Succeeded: the memory's address moved by the binding's offset. */
	*va = memory->va + state->bindings[index].offset;
	return 0;
}

/* Gives the open status query, if any, a decode's result. */
static void
video_status(
	int32_t status)
{
	/* No query is open. */
	if (video_open_pool == NULL)
		return;

	/* The query's status. */
	video_open_pool->status[video_open_query] = status;
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
	kern_logf("nvrtx: vk: video: a picture is skipped: %s\n", reason);
}

/* Reports how many macroblocks cover a number of pixels. */
static uint32_t
video_mbs(
	uint32_t pixels)
{
	/* Succeeded: whole macroblocks. */
	return (pixels + VIDEO_MB - 1U) / VIDEO_MB;
}

/* Reads a picture's slice offsets: [count][array]{offset}, kept up to the decoder's table (more marks the chain). */
static void
video_read_offsets(
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
		if (chain->slices != NULL && item < NVRTX_NVDEC_SLICES_MAX) {
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
video_read_h265_picture(
	struct i915_wire_reader *reader,
	struct nvrtx_video_h265_picture *picture)
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
	video_read_fixed(reader, picture->before, NVRTX_VIDEO_H265_SET_ENTRIES);
	video_read_fixed(reader, picture->after, NVRTX_VIDEO_H265_SET_ENTRIES);
	video_read_fixed(reader, picture->long_term, NVRTX_VIDEO_H265_SET_ENTRIES);
}

/*
 * Reads an H.265 add record's fields: [count][array]{VPS}[count][array]
 * {SPS}[count][array]{PPS} (libvulkan's video_encode_h265_parameters_add).
 * The video sets are kept as their ids, the others as new allocations on
 * the chain; a set the executor cannot keep marks add_error.
 */
static void
video_read_h265_add(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	struct video_h265_sps_node scratch_sps;
	struct video_h265_pps_node scratch_pps;
	struct video_h265_sps_node *sps;
	struct video_h265_pps_node *pps;
	uint64_t count;
	uint64_t item;
	uint32_t id;

	/* The video parameter sets: only their ids are kept. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		id = video_read_h265_vps(reader);
		if (id >= VIDEO_H265_VPS_IDS) {
			chain->add_error = EINVAL;
			continue;
		}
		chain->new_h265_vps |= 1U << id;
	}

	/* The sequence parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < count && reader->error == 0; item++) {
		/* A set that cannot be allocated is read into scratch. */
		sps = kern_calloc(1U, sizeof(*sps));
		if (sps == NULL) {
			kern_memset(&scratch_sps, 0, sizeof(scratch_sps));
			sps = &scratch_sps;
			chain->add_error = ENOMEM;
		}

		/* The set; ids outside H.265 mark the add malformed. */
		video_read_h265_sps(reader, &sps->sps);
		if (sps->sps.vps_id >= VIDEO_H265_VPS_IDS || sps->sps.id >= VIDEO_H265_SPS_IDS)
			chain->add_error = EINVAL;
		if (sps != &scratch_sps) {
			sps->next = chain->new_h265_sps;
			chain->new_h265_sps = sps;
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

		/* The set; ids outside H.265 mark the add malformed. */
		video_read_h265_pps(reader, &pps->pps);
		if (pps->pps.vps_id >= VIDEO_H265_VPS_IDS ||
		    pps->pps.sps_id >= VIDEO_H265_SPS_IDS ||
		    pps->pps.id >= VIDEO_H265_PPS_IDS)
			chain->add_error = EINVAL;
		if (pps != &scratch_pps) {
			pps->next = chain->new_h265_pps;
			chain->new_h265_pps = pps;
		}
	}
}

/*
 * Reads one H.265 video parameter set (libvulkan's video_encode_h265_vps):
 * [flags][id][max sub-layers minus 1][reserved x2][timing x3][reserved]
 * [present]{DPB}[present]{HRD}[present]{PTL}.  Reports its id; the decode
 * needs nothing else of it.
 */
static uint32_t
video_read_h265_vps(
	struct i915_wire_reader *reader)
{
	uint64_t present;
	uint32_t id;
	uint32_t sub_layers;

	/* The flags, the id and the sub-layers. */
	(void)drv_i915_wire_read_u32(reader);
	id = drv_i915_wire_read_u32(reader);
	sub_layers = drv_i915_wire_read_u32(reader) + 1U;
	if (sub_layers > VIDEO_H265_SUB_LAYERS)
		reader->error = 1;

	/* The reserved words and the timing. */
	video_skip_words(reader, 6U);

	/* The buffering limits, the HRD and the profile, tier and level. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_dpb(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_hrd(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_ptl(reader);

	/* Succeeded: the set's id. */
	return id;
}

/*
 * Reads one H.265 sequence parameter set (libvulkan's
 * video_encode_h265_sps): its fields in declared order, then the
 * structures it points to.  The decode keeps its sizes, tools, reference
 * set counts and own scaling lists.
 */
static void
video_read_h265_sps(
	struct i915_wire_reader *reader,
	struct nvrtx_video_h265_sps *sps)
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
	if (sub_layers > VIDEO_H265_SUB_LAYERS)
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
	video_skip_words(reader, 2U);

	/* The screen content sizes and the conformance window, which the decode does not use. */
	video_skip_words(reader, 8U);

	/* The profile, tier and level, and the buffering limits. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_ptl(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_dpb(reader);

	/* The sequence's own scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_scaling = true;
		video_read_h265_scaling(reader, &sps->scaling);
	}

	/* The short-term sets: as many as the sequence counts. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		count = drv_i915_wire_read_u64(reader);
		if (count != sps->short_term_sets || count > VIDEO_H265_SHORT_TERM_SETS)
			reader->error = 1;
		for (item = 0U; item < count && reader->error == 0; item++)
			video_read_h265_short_term(reader);
	}

	/* The long-term pictures, the VUI and the palette's initializers. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_long_term(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_vui(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_words(reader, NULL, VIDEO_H265_PALETTE_ENTRIES);
}

/*
 * Reads one H.265 picture parameter set (libvulkan's
 * video_encode_h265_pps): its fields in declared order, then the
 * structures it points to.  The decode keeps its flags, keys, offsets,
 * tiles and own scaling lists.
 */
static void
video_read_h265_pps(
	struct i915_wire_reader *reader,
	struct nvrtx_video_h265_pps *pps)
{
	uint8_t offsets[VIDEO_H265_CHROMA_OFFSETS];
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
	video_skip_words(reader, 3U);
	video_read_fixed(reader, offsets, VIDEO_H265_CHROMA_OFFSETS);
	video_read_fixed(reader, offsets, VIDEO_H265_CHROMA_OFFSETS);
	video_skip_words(reader, 2U);

	/* The screen content fields, which Main does not use. */
	video_skip_words(reader, 6U);

	/* The tiles: their counts, two reserved words, each column's width and row's height minus 1, a reserved word. */
	pps->tile_columns_minus1 = drv_i915_wire_read_u32(reader);
	pps->tile_rows_minus1 = drv_i915_wire_read_u32(reader);
	video_skip_words(reader, 2U);
	video_read_words(reader, pps->column_width_minus1, NVRTX_VIDEO_H265_COLUMN_WIDTHS);
	video_read_words(reader, pps->row_height_minus1, NVRTX_VIDEO_H265_ROW_HEIGHTS);
	video_skip_words(reader, 1U);

	/* The picture's own scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		pps->has_scaling = true;
		video_read_h265_scaling(reader, &pps->scaling);
	}

	/* The palette's initializers, which Main does not use. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_words(reader, NULL, VIDEO_H265_PALETTE_ENTRIES);
}

/* Reads an H.265 profile, tier and level: [flags][profile][level]. */
static void
video_read_h265_ptl(
	struct i915_wire_reader *reader)
{
	/* The three words, which the decode does not use (the session's profile is checked). */
	video_skip_words(reader, 3U);
}

/* Reads H.265 buffering limits: [7]{latency}[7 bytes]{buffering}[7 bytes]{reordering}. */
static void
video_read_h265_dpb(
	struct i915_wire_reader *reader)
{
	uint8_t bytes[VIDEO_H265_SUB_LAYERS];

	/* The latency words, then the two byte arrays. */
	video_read_words(reader, NULL, VIDEO_H265_SUB_LAYERS);
	video_read_fixed(reader, bytes, VIDEO_H265_SUB_LAYERS);
	video_read_fixed(reader, bytes, VIDEO_H265_SUB_LAYERS);
}

/*
 * Reads H.265 HRD parameters (libvulkan's video_encode_h265_hrd): the
 * one-bit flags' word and the three per-sub-layer masks, nine lengths and
 * scales, the buffer counts, the durations, three reserved words, then
 * the NAL and VCL sub-layer arrays of sub_layers entries.
 */
static void
video_read_h265_hrd(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint8_t counts[VIDEO_H265_SUB_LAYERS];
	uint64_t present;

	/* The flags and the masks, the lengths and the scales. */
	video_skip_words(reader, 13U);

	/* The buffer counts, the durations and the reserved words. */
	video_read_fixed(reader, counts, VIDEO_H265_SUB_LAYERS);
	video_read_words(reader, NULL, VIDEO_H265_SUB_LAYERS);
	video_read_words(reader, NULL, 3U);

	/* The NAL and the VCL sub-layers. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_sub_layers(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_sub_layers(reader, sub_layers);
}

/* Reads an array of H.265 sub-layer HRD parameters: [count]{four arrays of 32 words and the CBR mask}. */
static void
video_read_h265_sub_layers(
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
		video_read_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_read_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_read_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_read_words(reader, NULL, VIDEO_H265_CPB_COUNT);
		video_skip_words(reader, 1U);
	}
}

/*
 * Reads H.265 scaling lists (libvulkan's video_encode_h265_scaling): the
 * 4x4, 8x8, 16x16 and 32x32 lists and the 16x16 and 32x32 DC values, each
 * a byte array.
 */
static void
video_read_h265_scaling(
	struct i915_wire_reader *reader,
	struct nvrtx_video_h265_scaling *scaling)
{
	/* The lists, list after list, then the DC values. */
	video_read_fixed(reader, scaling->list4, sizeof(scaling->list4));
	video_read_fixed(reader, scaling->list8, sizeof(scaling->list8));
	video_read_fixed(reader, scaling->list16, sizeof(scaling->list16));
	video_read_fixed(reader, scaling->list32, sizeof(scaling->list32));
	video_read_fixed(reader, scaling->dc16, sizeof(scaling->dc16));
	video_read_fixed(reader, scaling->dc32, sizeof(scaling->dc32));
}

/* Reads an H.265 VUI (libvulkan's video_encode_h265_vui): 25 words, then its HRD of sub_layers. */
static void
video_read_h265_vui(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint64_t present;

	/* The flags, the aspect, the signal, the window, the timing and the restrictions. */
	video_skip_words(reader, 25U);

	/* The HRD. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		video_read_h265_hrd(reader, sub_layers);
}

/* Reads one H.265 short-term set of a sequence: twelve words, then the two arrays of sixteen. */
static void
video_read_h265_short_term(
	struct i915_wire_reader *reader)
{
	/* The flags, the prediction, the used masks, the reserved words and the counts. */
	video_skip_words(reader, 12U);

	/* The order count differences before and after. */
	video_read_words(reader, NULL, VIDEO_H265_SET_PICTURES);
	video_read_words(reader, NULL, VIDEO_H265_SET_PICTURES);
}

/* Reads a sequence's H.265 long-term pictures: the used mask, then the 32 order counts. */
static void
video_read_h265_long_term(
	struct i915_wire_reader *reader)
{
	/* The mask, then the array. */
	video_skip_words(reader, 1U);
	video_read_words(reader, NULL, VIDEO_H265_LONG_TERM_PICS);
}

/*
 * Reads an array of words that must have expected elements:
 * [count][words]; into words when it is not NULL.
 */
static void
video_read_words(
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
video_read_fixed(
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
video_skip_words(
	struct i915_wire_reader *reader,
	uint32_t count)
{
	uint32_t index;

	/* Each word. */
	for (index = 0U; index < count; index++)
		(void)drv_i915_wire_read_u32(reader);
}

/*
 * Moves a chain's new H.265 sets into a parameters object.
 *
 * With replace, a set replaces the object's of the same key (a create);
 * without, a key already held refuses the whole add (an update) and
 * nothing is taken.  The object's capacity bounds the sets.
 */
static int
video_h265_take(
	struct video_parameters *parameters,
	struct video_chain *chain,
	bool replace)
{
	struct video_h265_sps_node *sps;
	struct video_h265_sps_node **sps_link;
	struct video_h265_sps_node *old_sps;
	struct video_h265_pps_node *pps;
	struct video_h265_pps_node **pps_link;
	struct video_h265_pps_node *old_pps;
	uint32_t vps;
	uint32_t vps_count;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t index;

	/* An update must not repeat a key the object holds. */
	if (!replace) {
		/* Nor a video set id. */
		if ((chain->new_h265_vps & parameters->h265_vps) != 0U)
			return EINVAL;

		/* Nor a sequence set key. */
		for (sps = chain->new_h265_sps; sps != NULL; sps = sps->next) {
			if (video_find_h265_sps(parameters, sps->sps.vps_id, sps->sps.id) != NULL)
				return EINVAL;
		}

		/* Nor a picture set key. */
		for (pps = chain->new_h265_pps; pps != NULL; pps = pps->next) {
			if (video_find_h265_pps(parameters, pps->pps.vps_id, pps->pps.sps_id, pps->pps.id) != NULL)
				return EINVAL;
		}
	}

	/* The video sets held after the add. */
	vps = parameters->h265_vps | chain->new_h265_vps;
	vps_count = 0U;
	for (index = 0U; index < VIDEO_H265_VPS_IDS; index++) {
		if ((vps & (1U << index)) != 0U)
			vps_count++;
	}

	/* The sequence sets held, and the new ones of new keys. */
	sps_count = 0U;
	for (sps = parameters->h265_sps; sps != NULL; sps = sps->next)
		sps_count++;
	for (sps = chain->new_h265_sps; sps != NULL; sps = sps->next) {
		if (video_find_h265_sps(parameters, sps->sps.vps_id, sps->sps.id) == NULL)
			sps_count++;
	}

	/* The picture sets held, and the new ones of new keys. */
	pps_count = 0U;
	for (pps = parameters->h265_pps; pps != NULL; pps = pps->next)
		pps_count++;
	for (pps = chain->new_h265_pps; pps != NULL; pps = pps->next) {
		if (video_find_h265_pps(parameters, pps->pps.vps_id, pps->pps.sps_id, pps->pps.id) == NULL)
			pps_count++;
	}

	/* Refuses sets beyond the object's capacity. */
	if (vps_count > parameters->max_vps ||
	    sps_count > parameters->max_sps ||
	    pps_count > parameters->max_pps)
		return EINVAL;

	/* The video sets' ids. */
	parameters->h265_vps = vps;
	chain->new_h265_vps = 0U;

	/* The sequence sets, each replacing any of its key. */
	while (chain->new_h265_sps != NULL) {
		sps = chain->new_h265_sps;
		chain->new_h265_sps = sps->next;

		/* Drops the set of the same key the object holds. */
		sps_link = &parameters->h265_sps;
		while (*sps_link != NULL) {
			if ((*sps_link)->sps.vps_id == sps->sps.vps_id && (*sps_link)->sps.id == sps->sps.id) {
				old_sps = *sps_link;
				*sps_link = old_sps->next;
				kern_free(old_sps);
				continue;
			}

			/* Moves on to the next set. */
			sps_link = &(*sps_link)->next;
		}

		/* The set joins the object's list. */
		sps->next = parameters->h265_sps;
		parameters->h265_sps = sps;
	}

	/* The picture sets, each replacing any of its key. */
	while (chain->new_h265_pps != NULL) {
		pps = chain->new_h265_pps;
		chain->new_h265_pps = pps->next;

		/* Drops the set of the same key the object holds. */
		pps_link = &parameters->h265_pps;
		while (*pps_link != NULL) {
			if ((*pps_link)->pps.vps_id == pps->pps.vps_id &&
			    (*pps_link)->pps.sps_id == pps->pps.sps_id &&
			    (*pps_link)->pps.id == pps->pps.id) {
				old_pps = *pps_link;
				*pps_link = old_pps->next;
				kern_free(old_pps);
				continue;
			}

			/* Moves on to the next set. */
			pps_link = &(*pps_link)->next;
		}

		/* The set joins the object's list. */
		pps->next = parameters->h265_pps;
		parameters->h265_pps = pps;
	}

	/* Succeeded: the sets are the object's. */
	return 0;
}

/* Copies a template's H.265 sets into a new parameters object. */
static int
video_h265_copy(
	struct video_parameters *destination,
	const struct video_parameters *source)
{
	const struct video_h265_sps_node *sps;
	const struct video_h265_pps_node *pps;
	struct video_h265_sps_node *sps_copy;
	struct video_h265_pps_node *pps_copy;

	/* The video sets' ids. */
	destination->h265_vps = source->h265_vps;

	/* The sequence sets. */
	for (sps = source->h265_sps; sps != NULL; sps = sps->next) {
		sps_copy = kern_calloc(1U, sizeof(*sps_copy));
		if (sps_copy == NULL)
			return ENOMEM;
		sps_copy->sps = sps->sps;
		sps_copy->next = destination->h265_sps;
		destination->h265_sps = sps_copy;
	}

	/* The picture sets. */
	for (pps = source->h265_pps; pps != NULL; pps = pps->next) {
		pps_copy = kern_calloc(1U, sizeof(*pps_copy));
		if (pps_copy == NULL)
			return ENOMEM;
		pps_copy->pps = pps->pps;
		pps_copy->next = destination->h265_pps;
		destination->h265_pps = pps_copy;
	}

	/* Succeeded: the template's sets are copied. */
	return 0;
}

/* Frees a parameters object's H.265 sets. */
static void
video_h265_free(
	struct video_parameters *parameters)
{
	struct video_h265_sps_node *sps;
	struct video_h265_pps_node *pps;

	/* The sequence sets. */
	while (parameters->h265_sps != NULL) {
		sps = parameters->h265_sps;
		parameters->h265_sps = sps->next;
		kern_free(sps);
	}

	/* The picture sets, and the video sets' ids. */
	while (parameters->h265_pps != NULL) {
		pps = parameters->h265_pps;
		parameters->h265_pps = pps->next;
		kern_free(pps);
	}
	parameters->h265_vps = 0U;
}

/* Finds the H.265 sequence set of a key in a parameters object. */
static const struct nvrtx_video_h265_sps *
video_find_h265_sps(
	const struct video_parameters *parameters,
	uint32_t vps_id,
	uint32_t sps_id)
{
	const struct video_h265_sps_node *sps;

	/* Walks the list for the key. */
	for (sps = parameters->h265_sps; sps != NULL; sps = sps->next) {
		if (sps->sps.vps_id == vps_id && sps->sps.id == sps_id)
			return &sps->sps;
	}

	/* No set of the key. */
	return NULL;
}

/* Finds the H.265 picture set of a key in a parameters object. */
static const struct nvrtx_video_h265_pps *
video_find_h265_pps(
	const struct video_parameters *parameters,
	uint32_t vps_id,
	uint32_t sps_id,
	uint32_t pps_id)
{
	const struct video_h265_pps_node *pps;

	/* Walks the list for the key. */
	for (pps = parameters->h265_pps; pps != NULL; pps = pps->next) {
		if (pps->pps.vps_id == vps_id && pps->pps.sps_id == sps_id && pps->pps.id == pps_id)
			return &pps->pps;
	}

	/* No set of the key. */
	return NULL;
}

/*
 * Runs an H.265 decode: checks it, resolves its surfaces (the DPB slot is
 * the surface) and references, counts its slice header's skip, fills the
 * picture structure, the scaling lists, the tile sizes and the push
 * buffer in the session's run, and has the decoder run it; the decoder's
 * status gives the open query its result.  A picture the decoder cannot
 * take is skipped with the ERROR status.  Returns EIO when the decoder
 * did not finish.
 *
 * Procedure referenced: averne's FFmpeg nvtegra libavcodec/nvtegra_hevc.c
 * (nvtegra_hevc_prepare_frame_setup and nvtegra_hevc_prepare_cmdbuf: a
 * surface without a reference takes the planes of the first current
 * reference, or of the picture itself).
 */
static int
video_run_decode_h265(
	struct nvrtx_vk_session *session,
	const struct video_record *video)
{
	struct video_session *state;
	const struct video_parameters *parameters;
	const struct nvrtx_video_h265_sps *sps;
	const struct nvrtx_video_h265_pps *pps;
	const struct nvrtx_vk_image *image;
	const struct nvrtx_vk_buffer *buffer;
	const struct video_slot *slot;
	struct nvrtx_nvdec_hevc_picture description;
	struct nvrtx_nvdec_hevc_tiles tiles;
	struct nvrtx_nvdec_frame frame;
	struct nvrtx_nvdec_hevc_frame hevc;
	struct nvrtx_video_h265_reference references[NVRTX_VIDEO_H265_REFERENCES];
	bool named[NVRTX_NVDEC_HEVC_SURFACES];
	uint32_t count;
	uint32_t index;
	uint32_t substitute;
	uint32_t bits;
	uint32_t ctbs;
	uint32_t errors;
	uint32_t header;
	uint64_t bitstream;
	uint64_t header_bytes;
	uint8_t *buffer_cpu;
	const char *reason;
	size_t length;
	bool dxva;
	bool trace;
	int error;

	/* Takes the session's state and the scope's parameters. */
	state = video->session->payload;
	parameters = NULL;
	if (video->parameters != NULL)
		parameters = video->parameters->payload;

	/* Checks what the decoder needs; a picture it cannot take is skipped. */
	reason = video->skip;
	if (reason == NULL)
		reason = video_check_h265(video, state, parameters, &sps, &pps);
	if (reason != NULL) {
		video_skip(reason);
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* The picture's own description from its sets. */
	kern_memset(&description, 0, sizeof(description));
	kern_memset(&frame, 0, sizeof(frame));
	kern_memset(&hevc, 0, sizeof(hevc));
	nvrtx_video_h265_describe(sps, pps, &video->h265_picture, &description);

	/* The references: each one's slot, flags and order count. */
	count = 0U;
	for (index = 0U; index < video->slot_count; index++) {
		slot = &video->slots[index];
		if (!slot->has_h265_reference || count >= NVRTX_VIDEO_H265_REFERENCES) {
			video_skip("a reference without its H.265 information, or too many");
			video_status(VIDEO_STATUS_ERROR);
			return 0;
		}
		references[count].slot = (uint32_t)slot->index;
		references[count].flags = slot->h265_reference.flags;
		references[count].poc = slot->h265_reference.poc;
		count++;
	}

	/* The picture's surface: its slot's, or one no reference holds. */
	if (video->has_setup && video->setup.index >= 0) {
		description.surface = (uint32_t)video->setup.index;
	} else {
		description.surface = nvrtx_video_h265_free_surface(references, count);
	}

	/* The references by surface, the lists and the substitute for the surfaces without one. */
	error = nvrtx_video_h265_references(&video->h265_picture, references, count, &description, &substitute);
	if (error != 0) {
		video_skip("a reference picture set names a slot the decode does not reference");
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* Each reference's planes in its surface. */
	for (index = 0U; index < NVRTX_NVDEC_HEVC_SURFACES; index++)
		named[index] = false;
	for (index = 0U; index < video->slot_count; index++) {
		slot = &video->slots[index];
		image = ((const struct nvrtx_vk_view *)slot->view->payload)->image->payload;
		error = video_plane_addresses(image, &frame.luma[slot->index], &frame.chroma[slot->index]);
		if (error != 0) {
			video_skip("a reference has no memory");
			video_status(VIDEO_STATUS_ERROR);
			return 0;
		}
		named[slot->index] = true;
	}

	/* The output's planes in the picture's surface. */
	image = ((const struct nvrtx_vk_view *)video->view->payload)->image->payload;
	error = video_plane_addresses(image, &frame.luma[description.surface], &frame.chroma[description.surface]);
	if (error != 0) {
		video_skip("the output has no memory");
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}
	named[description.surface] = true;

	/* Every other surface: the substitute's planes. */
	for (index = 0U; index < NVRTX_NVDEC_HEVC_SURFACES; index++) {
		if (named[index])
			continue;
		frame.luma[index] = frame.luma[substitute];
		frame.chroma[index] = frame.chroma[substitute];
	}

	/* The bitstream. */
	buffer = video->buffer->payload;
	error = nvrtx_vk_buffer_address(buffer, &bitstream, &buffer_cpu);
	if (error != 0) {
		video_skip("the bitstream has no memory");
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/*
	 * How the decoder gets past the slice header's reference sets: told
	 * the counts to parse them with, or (by default) told how many bits
	 * to skip, counted here from the first slice segment's header.
	 */
	dxva = nvrtx_vk_video_hevc_dxva(session->owner);
	description.dxva = 0U;
	if (dxva)
		description.dxva = 1U;
	if (!dxva) {
		/* The bitstream in the CPU's window, put there when it has no place yet. */
		if (buffer_cpu == NULL) {
			error = nvrtx_vk_buffer_cpu(session, buffer, &buffer_cpu);
			if (error != 0) {
				video_skip("the bitstream cannot be put in the CPU's view");
				video_status(VIDEO_STATUS_ERROR);
				return 0;
			}
		}

		/* The header's first bytes, copied out of the bitstream. */
		header_bytes = video->range - video->slices[0];
		if (header_bytes > VIDEO_H265_HEADER_BYTES)
			header_bytes = VIDEO_H265_HEADER_BYTES;
		kern_memcpy(state->header, buffer_cpu + video->offset + video->slices[0], (size_t)header_bytes);

		/* The bits to skip. */
		error = nvrtx_video_h265_skip_bits(state->header, (size_t)header_bytes, sps, pps, &video->h265_picture, &bits);
		if (error != 0) {
			video_skip("a slice segment header the driver cannot read");
			video_status(VIDEO_STATUS_ERROR);
			return 0;
		}
		description.skip_bits = bits;
	}

	/* The stream, the planes' rows and the session's buffers' parts in 256-byte units. */
	description.stream_bytes = (uint32_t)video->range;
	description.pitch = image->pitch;
	description.coloc_units = state->hevc.coloc_picture >> 8;
	description.sao_units = state->hevc.sao_at >> 8;
	description.bsd_units = state->hevc.bsd_at >> 8;
	description.filter_above_units = state->hevc.filter_above_at >> 8;
	description.sao_above_units = state->hevc.sao_above_at >> 8;

	/* The picture structure, the scaling lists (when the sequence uses them) and the tile sizes in the session's run. */
	nvrtx_nvdec_hevc_pic(state->run_cpu + VIDEO_RUN_PIC_AT, &description);
	if (description.scaling_enabled != 0U) {
		nvrtx_video_h265_scaling(sps, pps, &state->scaling);
		nvrtx_nvdec_hevc_scaling(state->run_cpu + VIDEO_RUN_SCALING_AT, &state->scaling);
	}
	nvrtx_video_h265_tiles(sps, pps, &tiles);
	nvrtx_nvdec_hevc_tiles(state->run_cpu + VIDEO_RUN_TILES_AT, &tiles, description.tiles != 0U);
	kern_memset(state->run_cpu + VIDEO_RUN_STATUS_AT, 0, VIDEO_RUN_STATUS_BYTES);

	/* The frame's addresses: the structures, the bitstream, the session's buffers. */
	frame.pic = state->run_va + VIDEO_RUN_PIC_AT;
	frame.status = state->run_va + VIDEO_RUN_STATUS_AT;
	frame.bitstream = bitstream + video->offset;
	hevc.scaling = state->run_va + VIDEO_RUN_SCALING_AT;
	hevc.tiles = state->run_va + VIDEO_RUN_TILES_AT;
	error = video_binding_address(state, 0U, &frame.coloc);
	if (error == 0)
		error = video_binding_address(state, 1U, &hevc.filter);
	if (error != 0) {
		video_skip("the session's memory is not bound");
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* The picture's number, then the push buffer, run on the decoder and waited for. */
	frame.serial = state->serial;
	length = nvrtx_nvdec_push_hevc(state->run_cpu + VIDEO_RUN_PUSH_AT, &frame, &hevc);
	trace = nvrtx_vk_video_hevc_trace(session->owner) && state->serial < VIDEO_TRACE_PICTURES;
	if (trace) {
		kern_logf("nvrtx: vk: video: trace %u: bitstream %llx offset %llu range %llu slices %u first %u pitch %u surface %u substitute %u\n",
			  (unsigned)state->serial,
			  (unsigned long long)bitstream,
			  (unsigned long long)video->offset,
			  (unsigned long long)video->range,
			  (unsigned)video->slice_count,
			  (unsigned)video->slices[0],
			  (unsigned)image->pitch,
			  (unsigned)description.surface,
			  (unsigned)substitute);
		video_trace("pic", state->serial, state->run_cpu + VIDEO_RUN_PIC_AT, NVRTX_NVDEC_HEVC_PIC_BYTES);
		video_trace("tiles", state->serial, state->run_cpu + VIDEO_RUN_TILES_AT, 32U);
		if (description.scaling_enabled != 0U)
			video_trace("scaling", state->serial, state->run_cpu + VIDEO_RUN_SCALING_AT, NVRTX_NVDEC_HEVC_SCALING_BYTES);
		video_trace("push", state->serial, state->run_cpu + VIDEO_RUN_PUSH_AT, length);
		if (buffer_cpu != NULL)
			video_trace("stream", state->serial, buffer_cpu + video->offset, 64U);
	}
	error = video_decoded(session, video, state->run_va + VIDEO_RUN_PUSH_AT, length);
	if (error != 0)
		return error;
	if (trace)
		video_trace("status", state->serial - 1U, state->run_cpu + VIDEO_RUN_STATUS_AT, 56U);

	/* The decoder's status: CTBs in error, a decode error or a slice header error make the result an error. */
	ctbs = nvrtx_get32(state->run_cpu + VIDEO_RUN_STATUS_AT, NVRTX_NVDEC_STATUS_ERRORS_AT);
	errors = nvrtx_get32(state->run_cpu + VIDEO_RUN_STATUS_AT, NVRTX_NVDEC_STATUS_ERROR_AT);
	header = nvrtx_get32(state->run_cpu + VIDEO_RUN_STATUS_AT, NVRTX_NVDEC_STATUS_HEADER_AT);
	if (ctbs != 0U || errors != 0U || header != 0U) {
		/* The first few of them are logged with what the decoder reported and what it was told. */
		if (video_skip_lines < VIDEO_SKIP_LINES) {
			video_skip_lines++;
			kern_logf("nvrtx: vk: video: an H.265 picture in error: %u blocks of %u, error %08x at block %08x slice %08x, slice header %08x (skip %u bits, %u references)\n",
				  (unsigned)ctbs,
				  (unsigned)(ctbs + nvrtx_get32(state->run_cpu + VIDEO_RUN_STATUS_AT, NVRTX_NVDEC_STATUS_DECODED_AT)),
				  (unsigned)errors,
				  (unsigned)nvrtx_get32(state->run_cpu + VIDEO_RUN_STATUS_AT, NVRTX_NVDEC_STATUS_CTB_AT),
				  (unsigned)nvrtx_get32(state->run_cpu + VIDEO_RUN_STATUS_AT, NVRTX_NVDEC_STATUS_SLICE_AT),
				  (unsigned)header,
				  (unsigned)description.skip_bits,
				  (unsigned)description.reference_count);
		}
		video_status(VIDEO_STATUS_ERROR);
		return 0;
	}

	/* Succeeded: the picture is in its image. */
	video_status(VIDEO_STATUS_COMPLETE);
	return 0;
}

/*
 * Checks an H.265 decode against what the decoder takes, and finds its
 * sets.  Returns NULL when it can run, else why the picture is skipped.
 */
static const char *
video_check_h265(
	const struct video_record *video,
	const struct video_session *state,
	const struct video_parameters *parameters,
	const struct nvrtx_video_h265_sps **sps_out,
	const struct nvrtx_video_h265_pps **pps_out)
{
	const struct nvrtx_video_h265_picture *picture;
	const struct nvrtx_video_h265_sps *sps;
	const struct nvrtx_video_h265_pps *pps;
	const char *reason;
	uint32_t log2_ctb;
	uint32_t limit_width;
	uint32_t limit_height;

	/* The picture's information and its sets, the video set among them. */
	if (!video->has_h265_picture)
		return "no H.265 picture information";
	if (parameters == NULL || parameters->codec != VIDEO_CODEC_H265)
		return "no parameter sets";
	picture = &video->h265_picture;
	if (picture->vps_id >= VIDEO_H265_VPS_IDS || (parameters->h265_vps & (1U << picture->vps_id)) == 0U)
		return "a parameter set the session does not hold";
	sps = video_find_h265_sps(parameters, picture->vps_id, picture->sps_id);
	pps = video_find_h265_pps(parameters, picture->vps_id, picture->sps_id, picture->pps_id);
	if (sps == NULL || pps == NULL)
		return "a parameter set the session does not hold";

	/* An 8-bit 4:2:0 picture of CTBs of 16 to 64. */
	if (sps->chroma_format_idc != 1U ||
	    sps->bit_depth_luma_minus8 != 0U ||
	    sps->bit_depth_chroma_minus8 != 0U)
		return "not 8-bit 4:2:0";
	log2_ctb = sps->log2_min_cb_minus3 + 3U + sps->log2_diff_cb;
	if (log2_ctb < 4U || log2_ctb > 6U)
		return "a CTB size the decoder does not take";

	/* Tiles within the decoder's. */
	if ((pps->flags & NVRTX_VIDEO_H265_PPS_TILES) != 0U &&
	    (pps->tile_columns_minus1 + 1U > NVRTX_NVDEC_HEVC_TILE_COLUMNS ||
	     pps->tile_rows_minus1 + 1U > NVRTX_NVDEC_HEVC_TILE_ROWS))
		return "more tiles than the decoder takes";

	/* Within the session's largest picture, on whole CTBs (its buffers' size). */
	limit_width = (state->width + VIDEO_H265_GRANULARITY - 1U) & ~(VIDEO_H265_GRANULARITY - 1U);
	limit_height = (state->height + VIDEO_H265_GRANULARITY - 1U) & ~(VIDEO_H265_GRANULARITY - 1U);
	if (sps->width == 0U || sps->height == 0U || sps->width > limit_width || sps->height > limit_height)
		return "a picture larger than the session's";

	/* The slices, the bitstream, the output and the references. */
	reason = video_check_resources(video, state, sps->width, sps->height, VIDEO_H265_GRANULARITY, NVRTX_VIDEO_H265_SLOTS);
	if (reason != NULL)
		return reason;

	/* Succeeded: the decode can run with these sets. */
	*sps_out = sps;
	*pps_out = pps;
	return NULL;
}

/*
 * Has the decoder run a push buffer and waits for it, numbering the
 * session's pictures.  When the decoder did not finish, what the decode
 * names is held for the boot (the decoder may still write it), the open
 * query is given ERROR, and EIO is returned.
 */
static int
video_decoded(
	struct nvrtx_vk_session *session,
	const struct video_record *video,
	uint64_t push_va,
	size_t length)
{
	struct video_session *state;
	int error;

	/* Runs the push buffer and numbers the next picture. */
	state = video->session->payload;
	error = nvrtx_vk_video_decode(session->owner, push_va, (uint32_t)length);
	state->serial++;

	/* A decoder that did not finish may still write what the decode names. */
	if (error == ETIMEDOUT)
		video_isolate(video);

	/* Reports a picture the decoder did not finish. */
	if (error != 0) {
		video_status(VIDEO_STATUS_ERROR);
		kern_logf("nvrtx: vk: video: the decoder did not finish a picture (%d)\n", error);
		return EIO;
	}

	/* Succeeded: the decoder has finished. */
	return 0;
}

/*
 * Holds for the boot every object a decode the decoder did not finish
 * names: its session (with its bound work buffers and its run), its
 * parameters, its bitstream's buffer, and its output's and references'
 * views (each with its image and the image's memory).  Nothing lets them
 * go, so their memory is not used again while the decoder may write it.
 */
static void
video_isolate(
	const struct video_record *video)
{
	const struct video_session *state;
	const struct nvrtx_vk_buffer *buffer;
	const struct nvrtx_vk_view *view;
	const struct nvrtx_vk_image *image;
	uint64_t bytes;
	uint32_t index;

	/* The session, with the bytes of its bound work buffers. */
	state = video->session->payload;
	bytes = 0U;
	for (index = 0U; index < state->binding_count; index++)
		bytes += state->bindings[index].size;
	video_hold_for_boot(video->session, bytes + VIDEO_RUN_BYTES);

	/* The parameters and the bitstream's buffer. */
	video_hold_for_boot(video->parameters, 0U);
	if (video->buffer != NULL) {
		buffer = video->buffer->payload;
		video_hold_for_boot(video->buffer, buffer->size);
	}

	/* The output's view, with its image's bytes. */
	if (video->view != NULL) {
		view = video->view->payload;
		image = view->image->payload;
		video_hold_for_boot(video->view, image->bytes);
	}

	/* The references' views, with their images' bytes. */
	for (index = 0U; index < video->slot_count; index++) {
		if (video->slots[index].view == NULL)
			continue;
		view = video->slots[index].view->payload;
		image = view->image->payload;
		video_hold_for_boot(video->slots[index].view, image->bytes);
	}

	/* Tells how much the boot keeps. */
	kern_logf("nvrtx: vk: video: %u objects (%llu bytes) are held for the boot: the decoder may still write them\n",
		  (unsigned)video_isolated_objects,
		  (unsigned long long)video_isolated_bytes);
}

/* Holds one object for the boot (one more reference nothing lets go), counting it and its bytes. */
static void
video_hold_for_boot(
	struct nvrtx_vk_object *object,
	uint64_t bytes)
{
	int error;

	/* Nothing to hold. */
	if (object == NULL)
		return;

	/* One more reference, which is never let go. */
	error = nvrtx_vk_object_retain(object);
	if (error != 0)
		return;

	/* Counts it. */
	video_isolated_objects++;
	video_isolated_bytes += bytes;
}

/* Logs bytes the decoder is given or reports, 32 to a line in hexadecimal. */
static void
video_trace(
	const char *what,
	uint32_t serial,
	const uint8_t *bytes,
	size_t length)
{
	static const char digits[] = "0123456789abcdef";
	char line[VIDEO_TRACE_LINE * 2U + 1U];
	size_t at;
	size_t index;
	size_t count;

	/* Each line: its offset and its bytes. */
	for (at = 0U; at < length; at += VIDEO_TRACE_LINE) {
		count = length - at;
		if (count > VIDEO_TRACE_LINE)
			count = VIDEO_TRACE_LINE;
		for (index = 0U; index < count; index++) {
			line[index * 2U] = digits[bytes[at + index] >> 4];
			line[index * 2U + 1U] = digits[bytes[at + index] & 15U];
		}
		line[count * 2U] = '\0';
		kern_logf("nvrtx: vk: video: trace %u %s %04x: %s\n", (unsigned)serial, what, (unsigned)at, line);
	}
}
