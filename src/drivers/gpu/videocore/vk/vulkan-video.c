/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Vulkan Video H.264 decode on the Raspberry Pi 4 (see vulkan-video.h).
 *
 * The wire is libvulkan's (userland/desktop/libvulkan/video.c), the same
 * the i915 executor reads (i915/vk/vk-video.c); the readers here follow
 * that file.  What differs is the decoder: the VideoCore firmware's, which
 * is stateful.  A decode command becomes one access unit for it -- the
 * sequence and picture parameter sets rebuilt from their StdVideo fields
 * (the SPS with VUI that asks for no reordering), then the application's
 * slices -- and the frame that comes back is the decode's output.  The
 * DPB pictures are kept only in form: the firmware keeps its own
 * references, and the output picture (which is also the setup slot's,
 * DPB_AND_OUTPUT_COINCIDE) receives every decoded frame.
 *
 * A decode runs in the native job like a CPU copy, and only sends its unit:
 * the decode is then in flight, and the next decodes are sent behind it,
 * so the firmware has several pictures at once.  The frames
 * come back in decode order and are taken whenever the session's decoder is
 * looked at: when a decode is sent, when a command reads (or writes) an
 * image whose frame has not come -- the image's readback waits for it, a
 * step at a time with the controller's window open (ioctls go on), so the
 * other sessions' work runs while the VPU decodes -- and at a reset.  A
 * frame comes into the decoder's cached memory; it is copied into its
 * output image with the firmware's footroom error undone, and stays there
 * as the image's cached copy, which the image's readback reads instead of
 * the image's uncached memory.  A decode's status query becomes available
 * when its frame is placed.
 *
 * A submission with a fence ends, and its fence signals, only when the
 * frames of its own decodes are in their images (an empty submission with
 * a fence, as vkQueueWaitIdle sends, waits for every decode in flight): the
 * host may read an image's memory once the fence has signaled.  The
 * execution leaves the session's queue as soon as its decodes are sent and
 * waits in the executor's settling list meanwhile, so the next decodes are
 * sent behind it (libvulkan gives every submission without a
 * fence a private one, so waiting in the queue held the decoder to one
 * picture).  A later submission's fence does not wait for an earlier
 * submission's decodes; every command that uses an image waits for its
 * frame first, so what the commands read is the same.
 *
 * A bitstream the firmware cannot take, or a frame that does not come,
 * skips the picture (the image keeps what it had) and marks the status
 * query ERROR; it does not lose the submission.
 */

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/bcm2711/vk/vulkan-video.h"
#include "drivers/gpu/bcm2711/vk/vulkan-command.h"
#include "drivers/gpu/bcm2711/vk/vulkan-input.h"
#include "drivers/gpu/bcm2711/vk/vulkan-resource.h"
#include "drivers/gpu/bcm2711/vk/vulkan-memory.h"
#include "drivers/gpu/bcm2711/v3d-memory.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/render-device.h"
#include "drivers/gpu/bcm2711/render-worker.h"
#include "drivers/platform/rpi4/rpi4-h264.h"
#include "drivers/platform/rpi4/rpi4-mmal.h"
#include "drivers/platform/rpi4/rpi4-mmal-wire.h"
#include "drivers/platform/rpi4/rpi4-vchiq.h"

/*
 * The profiles offered.  When the firmware is found to output in display
 * order despite the rewritten VUI (design §5, R1), only Baseline, which
 * never reorders, is offered.
 */
#define VIDEO_BASELINE_ONLY		0

/* The decodes a session has in flight (sent, frame not placed) at most. */
#define VIDEO_FLIGHTS			4U

/* The frames a session keeps in the decoder's memory as images' cached copies, at most. */
#define VIDEO_HELD			3U

/* The decoder's output buffers: the flights', the kept frames' and one for the push or a late frame. */
#define VIDEO_OUTPUTS			(VIDEO_FLIGHTS + VIDEO_HELD + 1U)

/* The end of the physical memory the VPU's window reaches: a direct run lies below it. */
#define VIDEO_DIRECT_LIMIT		0x40000000ULL

/* No time stamp: a collection that waits for no particular decode. */
#define VIDEO_NO_PTS			((int64_t)-1)

/* The H.264 decode limits the executor reports and holds sessions to. */
#define VIDEO_MAX_DPB_SLOTS		17U
#define VIDEO_MAX_REFERENCES		16U
#define VIDEO_MAX_WIDTH			1920U
#define VIDEO_MAX_HEIGHT		1088U
#define VIDEO_MB			16U
#define VIDEO_MAX_SLOTS_RECORDED	32U

/* The H.264 header the decoder was written against: VK_STD_vulkan_video_codec_h264_decode 1.0.0. */
#define VIDEO_STD_NAME			"VK_STD_vulkan_video_codec_h264_decode"
#define VIDEO_STD_VERSION		((1U << 22) | (0U << 12) | 0U)

/* The H.264 profiles, and the level reported: STD_VIDEO_H264_LEVEL_IDC_4_2. */
#define VIDEO_PROFILE_BASELINE		66U
#define VIDEO_PROFILE_MAIN		77U
#define VIDEO_PROFILE_HIGH		100U
#define VIDEO_LEVEL_4_2			12U

/* The parameter set id ranges. */
#define VIDEO_SPS_IDS			32U
#define VIDEO_PPS_IDS			256U

/* The deepest chain a structure may carry. */
#define VIDEO_MAX_CHAIN			8U

/*
 * The firmware decoder's input staging: the largest access unit taken.
 * Each of the decoder's staging areas has this much (a unit in flight
 * holds one), so four of them take as much memory as two of 4 MiB did.
 */
#define VIDEO_INPUT_BYTES		(2U * 1024U * 1024U)

/* How long a decode waits for its frame, and for room to be sent, over all the steps. */
#define VIDEO_FRAME_MS			500U

/* How long one step looks for a frame before the other sessions take their turns. */
#define VIDEO_POLL_MS			2U

/* The timeouts in a row after which a session stops waiting. */
#define VIDEO_MAX_TIMEOUTS		3U

/* The push picture's time stamp: decodes count from 1, so no decode has it (see send_push). */
#define VIDEO_PUSH_PTS			0

/* The lines a session writes about pushes and about late frames, at most. */
#define VIDEO_PUSH_LINES		4U
#define VIDEO_LATE_LINES		8U

/* How often the cost line is written while decodes run, and how many a boot writes at most. */
#define VIDEO_STATS_MS			2000U
#define VIDEO_STATS_REPORTS		64U

/* The session memory binding reported: one page the decoder never uses. */
#define VIDEO_BIND_BYTES		4096U

/* The most queries of a result status pool. */
#define VIDEO_QUERY_MAX			64U

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
#define VIDEO_PICTURE_IDR			0x4U
#define VIDEO_PICTURE_REFERENCE			0x10U

/* The query type of a result status pool, and its results. */
#define VIDEO_QUERY_RESULT_STATUS		1000023000U
#define VIDEO_STATUS_COMPLETE			1
#define VIDEO_STATUS_ERROR			(-1)
#define VIDEO_QUERY_RESULT_64			0x1U
#define VIDEO_QUERY_RESULT_AVAILABILITY		0x4U
#define VIDEO_QUERY_REPLY_MAX			4096U

/* The video results the executor replies (the pinned header's values). */
#define VIDEO_ERROR_IMAGE_USAGE			(-1000023000)
#define VIDEO_ERROR_LAYOUT			(-1000023001)
#define VIDEO_ERROR_OPERATION			(-1000023002)
#define VIDEO_ERROR_FORMAT			(-1000023003)
#define VIDEO_ERROR_CODEC			(-1000023004)
#define VIDEO_ERROR_STD_VERSION			(-1000023005)

/* One picture parameter set a parameters object holds; the sets of the object are a list. */
struct video_pps {
	struct video_pps *next;
	struct drv_rpi4_h264_pps fields;
};

/*
 * One VkVideoSessionParametersKHR: the H.264 sets of one session.
 *
 * Published from its creation to its destruction; the recorded begins that
 * name it retain it.  The update sequence orders the updates.
 */
struct video_parameters {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t max_sps;
	uint32_t max_pps;
	uint32_t update_sequence;
	struct drv_rpi4_h264_sps *sps[VIDEO_SPS_IDS];
	struct video_pps *pps;
};

/*
 * Where a frame placed in an image is in the decoder's cached memory: its
 * planes, their pitch, and the part of the image the frame covered (bytes
 * a row, luma rows).
 */
struct video_copy {
	const uint8_t *luma;
	const uint8_t *chroma;
	uint32_t pitch;
	uint32_t width;
	uint32_t rows;
};

/*
 * One decode whose unit the firmware has and whose frame has not been
 * placed.
 *
 * It retains the output picture's view (and through it the image) and the
 * status query's pool, if any, until the frame is placed or the decode is
 * given up.  command is the decode's record and image the view's image
 * object, only compared with the records of an ending execution and the
 * objects a command names.  target and cpu are resolved before each collection's
 * window and valid only in it; placed, lost and the frame's fields are
 * written in the window and taken after it, under the controller mutex.
 * direct is the image's memory run when the frame goes there from the
 * VPU (a sampled image), retained until the decoder's target
 * is removed; direct_frame says the frame came that way, so no frame of
 * the decoder's memory is the image's copy.
 */
struct video_flight {
	int64_t pts;
	const struct video_command *command;
	struct bcm2711_vulkan_object *view;
	const struct bcm2711_vulkan_object *image;
	struct bcm2711_vulkan_object *pool;
	uint32_t query;
	uint32_t generation;
	uint64_t started_at;
	uint64_t sent_at;
	uint64_t deadline;
	struct bcm2711_vulkan_resource *target;
	uint8_t *cpu;
	bool placed;
	bool lost;
	unsigned handle;
	uint32_t frame_flags;
	uint64_t received_at;
	uint64_t place_ticks;
	struct video_copy copy;
	struct bcm2711_buffer *direct;
	bool direct_frame;
};

/*
 * The run a decode's frame goes to directly, prepared before its unit is
 * sent: the image's memory run, retained while the decoder has the target,
 * and the target.  registered says the decoder has it.
 */
struct video_direct {
	struct bcm2711_buffer *buffer;
	struct drv_rpi4_mmal_target target;
	bool registered;
};

/*
 * One frame kept in the decoder's memory as an image's cached copy: the
 * image's view (retained, so the image stays), the image, the decoder's
 * handle of the frame, and where the frame is.
 */
struct video_held {
	struct bcm2711_vulkan_object *view;
	const struct bcm2711_vulkan_resource *image;
	unsigned handle;
	struct video_copy copy;
};

/*
 * One VkVideoSessionKHR and the firmware decoder behind it.
 *
 * Published from its creation; the recorded begins retain it, so a decode
 * always finds its decoder.  The last sets sent, the time stamps, the
 * flights (oldest first) and the kept frames (oldest first) change only in
 * the native job and at the session's release, under the controller mutex
 * (a collection's window writes only the flights' frame fields).  sending
 * is the decode being sent, from its first step (sending_since) until it
 * is sent or skipped.  self is the session's object, retained by a
 * collection that no command of the session retains.  generation tells this
 * session's decodes from another's in an execution's mark (the time stamps
 * of every session count from 1).  unit is the scratch a parameter set is
 * written into (kept off the kernel stack).
 */
struct video_session {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *self;
	uint64_t generation;
	uint32_t profile_idc;
	uint32_t width;
	uint32_t height;
	uint32_t max_dpb_slots;
	uint32_t max_references;
	struct drv_rpi4_mmal_decoder *decoder;
	bool broken;
	unsigned timeouts;
	bool started;
	int64_t sequence;
	const struct video_command *sending;
	uint64_t sending_since;
	struct video_flight flights[VIDEO_FLIGHTS];
	unsigned flight_count;
	struct video_held held[VIDEO_HELD];
	unsigned held_count;
	unsigned push_lines;
	unsigned late_lines;
	uint8_t sent_sps[DRV_RPI4_H264_SPS_MAX + 2048U];
	size_t sent_sps_bytes;
	uint8_t sent_pps[DRV_RPI4_H264_SPS_MAX + 2048U];
	size_t sent_pps_bytes;
	uint8_t unit[DRV_RPI4_H264_SPS_MAX + 2048U];
};

/*
 * One result status query pool.
 *
 * Published from its creation; the recorded query commands and the
 * flights retain it.  The statuses and availabilities change only in the
 * native job.  pending counts a query's decodes in flight: its end makes it
 * available only when none is left (ended remembers the end meanwhile).
 * generation changes at each reset, so a flight of the query before the
 * reset leaves it alone.
 */
struct video_pool {
	struct bcm2711_vulkan_input_owner owner;
	uint32_t count;
	int32_t status[VIDEO_QUERY_MAX];
	uint8_t available[VIDEO_QUERY_MAX];
	uint8_t pending[VIDEO_QUERY_MAX];
	uint8_t ended[VIDEO_QUERY_MAX];
	uint32_t generation[VIDEO_QUERY_MAX];
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

/*
 * One recorded video or query command.
 *
 * It starts with the common record, which the command buffer's walks read.
 * It retains every object it names until its command buffer is reset or
 * its last submission retires.  A decode and a begin query borrow the
 * begin of their coding scope, and a decode the begin query open around
 * it (status); both live in the same command buffer, which retains the
 * pool.
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
	bool has_picture;
	uint32_t picture_flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t frame_num;
	int32_t poc;
	uint32_t query;
	uint32_t query_count;
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
	struct drv_rpi4_h264_sps *new_sps[VIDEO_SPS_IDS];
	uint32_t new_sps_count;
	struct video_pps *new_pps;
	int add_error;
	bool has_picture;
	uint32_t picture_flags;
	uint32_t sps_id;
	uint32_t pps_id;
	uint32_t frame_num;
	int32_t poc;
};

/*
 * What the decodes and readbacks of the last few seconds cost, for the
 * line written every VIDEO_STATS_MS: counts, and sums and the largest of
 * the times in ticks (send: from a decode's first step to its unit sent,
 * which includes waiting for room; frame: from then to its frame received,
 * which is the VPU's decode behind the decodes before it, the bulk and the
 * cache invalidation; place: the corrected copy into the image; readback:
 * an NV12 image's copy to a buffer, from the cached copy or the image's
 * uncached memory; wait: a command's wait for its image's frame).  depth
 * sums the flights after each send.  Changed under the controller mutex.
 */
struct video_stats {
	uint64_t window_start;
	uint32_t reports;
	uint32_t decodes;
	uint32_t skipped;
	uint32_t pushes;
	uint32_t steps;
	uint32_t sends;
	uint64_t depth;
	uint32_t waits;
	uint64_t wait_ticks;
	uint64_t send_ticks;
	uint64_t frame_ticks;
	uint64_t frame_max;
	uint64_t place_ticks;
	uint32_t cached_readbacks;
	uint64_t cached_ticks;
	uint32_t uncached_readbacks;
	uint64_t uncached_ticks;
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
static int retain_command(struct bcm2711_vulkan_session *session, struct video_command *command, struct bcm2711_vulkan_object *device, uint64_t session_id, uint64_t parameters_id, uint64_t buffer_id, uint64_t view_id, uint64_t pool_id);
static int release_command(struct bcm2711_vulkan_command_node *node);
static const struct video_command *open_scope(const struct bcm2711_vulkan_command_buffer *command);
static const struct video_command *open_query(const struct bcm2711_vulkan_command_buffer *command);
static void read_chain(struct i915_wire_reader *reader, struct video_chain *chain, uint32_t depth);
static void read_fields(struct i915_wire_reader *reader, uint32_t type, struct video_chain *chain, uint32_t depth);
static int32_t read_profile(struct i915_wire_reader *reader, uint32_t *profile_idc);
static int32_t profile_result(uint32_t operation, uint32_t chroma, uint32_t luma, uint32_t chroma_depth, const struct video_chain *chain);
static void read_resource(struct i915_wire_reader *reader, struct video_resource *resource);
static void read_slot(struct i915_wire_reader *reader, struct video_resource *resource);
static void read_slots(struct i915_wire_reader *reader);
static void read_add(struct i915_wire_reader *reader, struct video_chain *chain);
static void read_scaling(struct i915_wire_reader *reader, struct drv_rpi4_h264_scaling *scaling);
static void read_extension(struct i915_wire_reader *reader, char *name, uint32_t *version);
static void chain_free(struct video_chain *chain);
static int parameters_take(struct video_parameters *parameters, struct video_chain *chain, bool replace);
static int parameters_copy(struct video_parameters *destination, const struct video_parameters *source);
static void parameters_free(struct video_parameters *parameters);
static const struct video_pps *find_pps(const struct video_parameters *parameters, uint32_t sps_id, uint32_t pps_id);
static int pool_create(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int pool_results(struct bcm2711_vulkan_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int record_query(struct bcm2711_vulkan_session *session, uint32_t opcode, struct i915_wire_reader *reader);
static int run_control(struct bcm2711_render_device *controller, const struct video_command *command);
static int run_decode(struct bcm2711_render_device *controller, const struct video_command *command);
static int begin_decode(struct video_session *video, const struct video_parameters *parameters, const struct video_command *command, struct video_direct *direct);
static void prepare_direct(struct bcm2711_vulkan_resource *image, struct video_direct *direct);
static void release_direct(struct video_session *video, struct bcm2711_buffer **buffer, int64_t pts, bool registered);
static void mark_written(struct video_flight *flight);
static int send_push(struct video_session *video, const struct video_parameters *parameters, const struct video_command *command);
static int add_flight(struct video_session *video, const struct video_command *command, struct video_direct *direct);
static int wait_or_skip(struct video_session *video, const struct video_command *command, const char *reason);
static int collect(struct bcm2711_render_device *controller, struct video_session *video, unsigned wait_ms, int64_t until);
static bool take_frame(struct video_session *video, const struct drv_rpi4_mmal_frame *frame, const uint8_t *pixels, int64_t until);
static int retire_flights(struct video_session *video, uint64_t now);
static int finish_flight(struct video_flight *flight, int32_t status);
static int keep_held(struct video_session *video, struct video_flight *flight);
static int drop_held(struct video_session *video, unsigned index);
static int drop_all(struct video_session *video);
static bool flight_pending(const struct video_session *video, int64_t until);
static unsigned poll_ms(const struct bcm2711_render_device *controller);
static uint32_t unused_pps_id(const struct video_parameters *parameters);
static int build_unit(struct video_session *video, const struct video_parameters *parameters, const struct video_command *command, uint8_t *staging, uint32_t capacity, uint32_t *bytes);
static int append_set(uint8_t *staging, uint32_t capacity, uint32_t *used, const uint8_t *unit, size_t bytes, uint8_t *sent, size_t *sent_bytes, bool force);
static int image_target(const struct bcm2711_vulkan_object *view_object, struct bcm2711_vulkan_resource **image, uint8_t **cpu);
static void relayout(const struct drv_rpi4_mmal_frame *frame, const uint8_t *pixels, const struct bcm2711_vulkan_resource *image, uint8_t *cpu, bool correct, struct video_copy *copy);
static void note_decode(const struct video_flight *flight, uint64_t now);
static void report_stats(uint64_t now);
static uint64_t average_us(uint64_t ticks, uint32_t count);
static uint64_t average_tenths(uint64_t sum, uint32_t count);
static void set_status(const struct video_command *command, int32_t status);
static void skip(struct video_session *video, const struct video_command *command, const char *reason, int error);
static void skip_line(const struct video_session *video, int64_t pts, const char *reason, int error);

/*
 * Whether any session has been promised video, for the log line said once.
 */
static bool video_announced;

/*
 * The session whose firmware decoder exists (the firmware client has one
 * decoder at a time), whose flights and kept frames the commands that use
 * an image look at.  Set when the decoder is made and cleared when the
 * session is released, under the controller mutex; NULL while there is
 * none.
 */
static struct video_session *video_current;

/* The costs of the current window (see struct video_stats); under the controller mutex. */
static struct video_stats video_stats;

/*
 * The generation the last session with a decoder took; the next takes one
 * more, so no two sessions share one and a mark's zero means none.  Under
 * the controller mutex.
 */
static uint64_t video_generation;

/*
 * Reports whether video decode can be offered now.
 */
bool
bcm2711_vulkan_video_available(void)
{
	bool connected;

	/* The firmware's link must be up; the decoder itself is made with each session. */
	connected = drv_rpi4_vchiq_connected();
	if (!connected)
		return false;

	/* Succeeded: the capset may promise video decode. */
	return true;
}

/*
 * Reports whether a session promised video decode in its capset.
 */
bool
bcm2711_vulkan_video_session(
	const struct bcm2711_vulkan_session *session)
{
	/* Latched once by the capset (design §14, M2). */
	return session->video;
}

/*
 * Records that a session's capset promised video decode.
 *
 * The promise is kept for the session's life: its queue family keeps the
 * decode flag and its video commands are taken even if the link breaks
 * later (decodes then fail one by one).
 */
void
bcm2711_vulkan_video_promise(
	struct bcm2711_vulkan_session *session)
{
	/* Latches the promise, and says so once a boot. */
	session->video = true;
	if (!video_announced) {
		video_announced = true;
		kern_logf("v3d: vk: capset declares H.264 video decode (VideoCore firmware decoder)\n");
	}
}

/*
 * Routes the video commands.
 *
 * A session whose capset did not promise video decode refuses them all,
 * which fails the stream (libvulkan never sends them there).
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

	/* Without video there are no query pools, as before. */
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
 * A begin binds nothing (the decode finds its scope's begin itself); a
 * control with RESET waits for the frames in flight and drops the
 * decoder's pictures; a decode sends its unit; an end does nothing.  A
 * query reset makes its queries unavailable; a begin query names the
 * status the next decodes of its scope write; an end query makes it
 * available, or leaves that to its last decode in flight.
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
	case GPU_OP_CMD_CONTROL_VIDEO_CODING:
		error = run_control(controller, command);
		break;
	case GPU_OP_CMD_DECODE_VIDEO:
		error = run_decode(controller, command);
		break;
	case GPU_OP_CMD_RESET_QUERY_POOL:
		/* Every query of the range becomes unavailable; a decode still in flight for one leaves it alone. */
		pool = command->pool->payload;
		for (index = command->query; index < command->query + command->query_count; index++) {
			pool->available[index] = 0;
			pool->status[index] = 0;
			pool->pending[index] = 0;
			pool->ended[index] = 0;
			pool->generation[index]++;
		}

		break;
	case GPU_OP_CMD_BEGIN_QUERY:
		/* The decodes inside write this query's status; until one does it is complete. */
		pool = command->pool->payload;
		pool->status[command->query] = VIDEO_STATUS_COMPLETE;
		break;
	case GPU_OP_CMD_END_QUERY:
		/* The query is available now, or when its last decode in flight has its frame. */
		pool = command->pool->payload;
		if (pool->pending[command->query] != 0) {
			pool->ended[command->query] = 1;
		} else {
			pool->available[command->query] = 1;
		}

		break;
	default:
		/* A begin or an end of a coding scope. */
		break;
	}

	/* Reports an event that could not run. */
	if (error != 0)
		return error;

	/* Succeeded: the event ran. */
	return 0;
}

/*
 * Lays out an NV12 image: rows of the width rounded up to 32 bytes, the
 * luma plane over the height rounded up to 16 rows, then the interleaved
 * chroma plane of half as many rows.  This is the layout the firmware's
 * decoder writes, so a frame lands in the image as it comes.
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
 * Reports whether a decoded NV12 image holds the firmware's samples as they
 * are.
 *
 * An image the GPU samples does (the footroom error makes no
 * visible difference, and correcting it would cost a CPU copy), and its
 * readback corrects them; any other decode output holds them corrected.
 */
bool
bcm2711_vulkan_video_image_raw(
	const struct bcm2711_vulkan_resource *image)
{
	/* Only an NV12 decode output. */
	if (image == NULL || image->format != (VkFormat)BCM2711_VIDEO_FORMAT_NV12)
		return false;
	if ((image->usage & BCM2711_VIDEO_USAGE_DECODE_DST) == 0)
		return false;

	/* A sampled one keeps the samples as they come. */
	if ((image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0)
		return true;

	/* Succeeded: any other holds corrected samples. */
	return false;
}

/*
 * Copies rows of a decoded NV12 image to a buffer from the image's cached
 * copy, when it has one.
 *
 * offset is the first row's byte offset in the image's layout (in either
 * plane), bytes the bytes a row and rows the rows; destination's rows are
 * destination_pitch apart.  The copy comes from the frame last placed in
 * the image while it is still in the decoder's memory, corrected as the
 * image's own bytes were, and only when every row lies in the part the
 * frame covered.  Returns false, copying nothing, otherwise; the caller
 * then copies from the image.  The caller holds the controller mutex.
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
	const struct video_copy *copy;
	const uint8_t *plane;
	uint64_t chroma;
	uint64_t total;
	uint64_t base;
	uint64_t row;
	uint64_t column;
	uint32_t pitch;
	uint32_t plane_rows;
	uint32_t index;
	unsigned held;

	/* Only an image whose last frame the decoder keeps. */
	copy = NULL;
	if (video_current != NULL) {
		for (held = 0; held < video_current->held_count; held++) {
			if (video_current->held[held].image == image)
				copy = &video_current->held[held].copy;
		}
	}

	/* No cached copy of the image. */
	if (copy == NULL)
		return false;

	/* The plane the rows are in, its start, and the rows of it the frame covered. */
	bcm2711_vulkan_video_nv12_layout(image->width, image->height, &pitch, &chroma, &total);
	if (offset >= chroma) {
		base = chroma;
		plane = copy->chroma;
		plane_rows = copy->rows / 2U;
	} else {
		base = 0;
		plane = copy->luma;
		plane_rows = copy->rows;
	}

	/* The first row and column in the plane; every row must lie in the covered part. */
	row = (offset - base) / pitch;
	column = (offset - base) % pitch;
	if (bytes > copy->width || column > copy->width - bytes)
		return false;
	if (rows > plane_rows || row > plane_rows - rows)
		return false;

	/* Each row from the copy, corrected. */
	for (index = 0; index < rows; index++) {
		drv_rpi4_mmal_copy_samples(destination + (size_t)index * destination_pitch,
					   plane + (size_t)(row + index) * copy->pitch + column,
					   (size_t)bytes);
	}

	/* Succeeded: the rows came from the cached copy. */
	return true;
}

/*
 * Waits for the frames of the images a command names.
 *
 * A copy, a blit or an upload that reads or writes an image whose decode
 * is in flight looks for the frames up to that decode's for VIDEO_POLL_MS
 * with the controller's window open; EINPROGRESS while it has not come and
 * its deadline has not passed (the job's step ends at the command and the
 * next step comes back here).  The caller holds the controller mutex.
 */
int
bcm2711_vulkan_video_settle(
	struct bcm2711_render_device *controller,
	const struct bcm2711_vulkan_record *record)
{
	struct video_session *video;
	struct bcm2711_vulkan_object *self;
	uint64_t started;
	int64_t until;
	unsigned slot;
	unsigned flight;
	bool pending;
	int error;
	int released;

	/* Only the session with a decoder has flights. */
	video = video_current;
	if (video == NULL || video->flight_count == 0)
		return 0;

	/* The newest flight whose image the command names. */
	until = VIDEO_NO_PTS;
	for (slot = 0; slot < 2U; slot++) {
		if (record->objects[slot] == NULL || record->objects[slot]->kind != I915_VK_OBJ_IMAGE)
			continue;
		for (flight = 0; flight < video->flight_count; flight++) {
			if (video->flights[flight].image == record->objects[slot] && video->flights[flight].pts > until)
				until = video->flights[flight].pts;
		}
	}

	/* No image of the command is waited for. */
	if (until == VIDEO_NO_PTS)
		return 0;

	/* The session stays while its decoder is looked at with the window open; no command of it retains it here. */
	self = video->self;
	error = bcm2711_vulkan_object_retain(self);
	if (error != 0)
		return error;

	/* The frames up to that decode's. */
	started = clock_ticks();
	error = collect(controller, video, poll_ms(controller), until);
	pending = flight_pending(video, until);
	video_stats.waits++;
	video_stats.wait_ticks += clock_ticks() - started;
	video_stats.steps++;
	released = bcm2711_vulkan_object_release(self);

	/* Reports a collection or a release that failed. */
	if (error != 0)
		return error;
	if (released != 0)
		return released;

	/* The frame has not come: the next step looks again. */
	if (pending)
		return EINPROGRESS;

	/* Succeeded: the command's images hold their frames. */
	return 0;
}

/*
 * Adds the newest decode in flight that a command buffer recorded to an
 * execution's mark.
 *
 * Called when the command buffer's job has run, so its decodes were sent;
 * a decode whose frame has already been placed is not in flight and adds
 * nothing.  The caller holds the controller mutex.
 */
void
bcm2711_vulkan_video_mark_commands(
	struct bcm2711_vulkan_video_mark *mark,
	const struct bcm2711_vulkan_command_buffer *command)
{
	const struct bcm2711_vulkan_command_node *node;
	const struct bcm2711_vulkan_record *record;
	struct video_session *video;
	unsigned flight;

	/* Only the session with a decoder has flights. */
	video = video_current;
	if (video == NULL || video->flight_count == 0)
		return;

	/* Each decode the command buffer recorded, and its flight if it is in flight. */
	for (node = command->first; node != NULL; node = node->next) {
		record = (const struct bcm2711_vulkan_record *)node;
		if (record->opcode != GPU_OP_CMD_DECODE_VIDEO)
			continue;
		for (flight = 0; flight < video->flight_count; flight++) {
			if (video->flights[flight].command != (const struct video_command *)record)
				continue;

			/* A newer decode of this session, or the first of the mark. */
			if (mark->generation != video->generation || video->flights[flight].pts > mark->until) {
				mark->generation = video->generation;
				mark->until = video->flights[flight].pts;
			}
		}
	}

	/* Succeeded: the mark covers the command buffer's decodes in flight. */
	return;
}

/*
 * Makes an execution's mark cover every decode in flight, for a submission
 * that only waits for the queue (an empty submit with a fence, as
 * vkQueueWaitIdle sends).  The caller holds the controller mutex.
 */
void
bcm2711_vulkan_video_mark_all(
	struct bcm2711_vulkan_video_mark *mark)
{
	struct video_session *video;

	/* Only the session with a decoder has flights. */
	video = video_current;
	if (video == NULL || video->flight_count == 0)
		return;

	/* The newest flight is the last sent. */
	mark->generation = video->generation;
	mark->until = video->flights[video->flight_count - 1U].pts;

	/* Succeeded: the mark covers every decode in flight. */
	return;
}

/*
 * Waits for the frames of the decodes an execution's mark covers, before
 * the execution ends and its fence signals: the host may read the images
 * (or the status queries) once the fence has signaled.
 *
 * Zero when they are placed or given up, or the session is gone;
 * EINPROGRESS while one has not come.  With wait, the frames are looked
 * for up to VIDEO_POLL_MS with the controller's window open; without, only
 * the frames already there are taken.  The caller holds the controller
 * mutex.
 */
int
bcm2711_vulkan_video_settle_mark(
	struct bcm2711_render_device *controller,
	const struct bcm2711_vulkan_video_mark *mark,
	bool wait)
{
	struct video_session *video;
	struct bcm2711_vulkan_object *self;
	unsigned wait_ms;
	bool pending;
	int error;
	int released;

	/* A mark of no decode, or of a session that is gone, waits for nothing. */
	video = video_current;
	if (mark->generation == 0 ||
	    video == NULL ||
	    video->generation != mark->generation)
		return 0;

	/* The covered decodes may all have their frames already. */
	pending = flight_pending(video, mark->until);
	if (!pending)
		return 0;

	/* The session stays while its decoder is looked at with the window open. */
	self = video->self;
	error = bcm2711_vulkan_object_retain(self);
	if (error != 0)
		return error;

	/* The frames up to the mark's newest decode, waiting a little only when asked. */
	wait_ms = 0;
	if (wait)
		wait_ms = VIDEO_POLL_MS;
	error = collect(controller, video, wait_ms, mark->until);
	pending = flight_pending(video, mark->until);
	video_stats.steps++;
	released = bcm2711_vulkan_object_release(self);

	/* Reports a collection or a release that failed. */
	if (error != 0)
		return error;
	if (released != 0)
		return released;

	/* The frame has not come: the execution keeps settling. */
	if (pending)
		return EINPROGRESS;

	/* Succeeded: the covered decodes are in their images. */
	return 0;
}

/*
 * Adds the time of an NV12 image's readback to the cost line: from the
 * cached copy or from the image's uncached memory.
 */
void
bcm2711_vulkan_video_readback_done(
	bool cached,
	uint64_t ticks)
{
	/* The readback's kind and time. */
	if (cached) {
		video_stats.cached_readbacks++;
		video_stats.cached_ticks += ticks;
	} else {
		video_stats.uncached_readbacks++;
		video_stats.uncached_ticks += ticks;
	}
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
		/* Separate reference images; offsets of 32 bytes, any size; 16x16 to 1920x1088; the slot limits. */
		drv_i915_wire_reply_u32(reply, VIDEO_SEPARATE_REFERENCE_IMAGES);
		drv_i915_wire_reply_u64(reply, 32U);
		drv_i915_wire_reply_u64(reply, 1U);
		drv_i915_wire_reply_u32(reply, VIDEO_MB);
		drv_i915_wire_reply_u32(reply, VIDEO_MB);
		drv_i915_wire_reply_u32(reply, VIDEO_MB);
		drv_i915_wire_reply_u32(reply, VIDEO_MB);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_WIDTH);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_HEIGHT);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_DPB_SLOTS);
		drv_i915_wire_reply_u32(reply, VIDEO_MAX_REFERENCES);

		/* The H.264 header name, padded to its array, and version. */
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
	case VIDEO_TYPE_H264_CAPABILITIES:
		/* Level 4.2 (1080p60) and no field offset (progressive only). */
		drv_i915_wire_reply_u32(reply, VIDEO_LEVEL_4_2);
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

	/* The profiles must all be H.264 decode; usages include picture readback and the sampling of its planes. */
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

	/*
	 * Replies the one format: NV12, identity swizzle, the create flags
	 * that let its planes be viewed as R8 and RG8, 2D, optimal (linear
	 * rows), decode output, reference, readback and sampling.
	 */
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
 * [present][count]{operations}.  The one family decodes H.264.
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

	/* Replies H.264 decode for family 0. */
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u32(reply, VIDEO_OP_DECODE_H264);

	/* Succeeded: the family's codec operations are reported. */
	return 0;
}

/*
 * vkCreateVideoSessionKHR: [device][present]{create info}[allocator][present]
 * [identity] -> [result][present][identity].
 *
 * The firmware's decoder is made here, so a session that cannot decode
 * fails at once (design §14, M8).
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
	} else if (width == 0U || height == 0U || width > VIDEO_MAX_WIDTH || height > VIDEO_MAX_HEIGHT) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (slots > VIDEO_MAX_DPB_SLOTS || references > VIDEO_MAX_REFERENCES) {
		result = VK_ERROR_INITIALIZATION_FAILED;
	} else if (different != 0 || version > VIDEO_STD_VERSION) {
		result = VIDEO_ERROR_STD_VERSION;
	}

	/* Reports a profile or header version the decoder does not take. */
	if (result != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)result);
		return 0;
	}

	/* Allocates the session with its limits. */
	video = kern_calloc(1U, sizeof(*video));
	if (video == NULL) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY);
		return 0;
	}

	/* The session's profile and limits. */
	video->profile_idc = profile_idc;
	video->width = width;
	video->height = height;
	video->max_dpb_slots = slots;
	video->max_references = references;

	/* The firmware's decoder of the session's largest picture; none means no session. */
	error = ENODEV;
	available = bcm2711_vulkan_video_available();
	if (available)
		error = drv_rpi4_mmal_decoder_create(width, height, VIDEO_INPUT_BYTES, VIDEO_OUTPUTS, &video->decoder);
	if (error != 0) {
		kern_free(video);
		kern_logf("v3d: video: no firmware decoder for a %ux%u session (%d)\n", (unsigned)width, (unsigned)height, error);
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	/* The session keeps its device. */
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		drv_rpi4_mmal_decoder_destroy(video->decoder);
		kern_free(video);
		return error;
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

	/* The session's own object, its generation, and the session whose decoder the commands look at. */
	video->self = object;
	video_generation++;
	video->generation = video_generation;
	video_current = video;

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
 * The decoder's memory is the firmware's; one page is asked for so the
 * application's bind follows its ordinary path.
 */
static int
session_requirements(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
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

	/* An unknown session, or no room for the one binding, is refused. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	if (object == NULL || capacity < 1U) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_INITIALIZATION_FAILED);
		return 0;
	}

	/* One binding: its type, no chain, index 0, one page, page alignment, memory type 0. */
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, 1U);
	drv_i915_wire_reply_u64(reply, 1U);
	drv_i915_wire_reply_u32(reply, VIDEO_TYPE_MEMORY_REQUIREMENTS);
	drv_i915_wire_reply_u64(reply, 0U);
	drv_i915_wire_reply_u32(reply, 0U);
	drv_i915_wire_reply_u64(reply, VIDEO_BIND_BYTES);
	drv_i915_wire_reply_u64(reply, VIDEO_BIND_BYTES);
	drv_i915_wire_reply_u32(reply, 1U);

	/* Succeeded: the binding is reported. */
	return 0;
}

/*
 * vkBindVideoSessionMemoryKHR: [device][session][count][array]{bind} -> [result].
 *
 * The binding is checked and not kept: nothing reads the page.
 */
static int
session_bind(
	struct bcm2711_vulkan_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct bcm2711_vulkan_object *object;
	struct bcm2711_vulkan_object *bound;
	struct video_chain chain;
	uint64_t identity;
	uint64_t count;
	uint64_t item;
	uint64_t memory;
	uint32_t type;
	uint32_t index;
	int32_t result;

	/* Skips the device, reads the session and the binding count. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Reads every binding: [type][chain][index][memory][offset][size]; only binding 0 of real memory is taken. */
	object = bcm2711_vulkan_object_find(session, I915_VK_OBJ_VIDEO_SESSION, identity);
	result = 0;
	if (object == NULL)
		result = VK_ERROR_INITIALIZATION_FAILED;
	for (item = 0U; item < count && reader->error == 0; item++) {
		kern_memset(&chain, 0, sizeof(chain));
		type = drv_i915_wire_read_u32(reader);
		if (type != VIDEO_TYPE_BIND_MEMORY)
			reader->error = 1;
		read_chain(reader, &chain, 0U);
		chain_free(&chain);
		index = drv_i915_wire_read_u32(reader);
		memory = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u64(reader);
		bound = bcm2711_vulkan_object_find(session, I915_VK_OBJ_MEMORY, memory);
		if (index != 0U || bound == NULL)
			result = VK_ERROR_INITIALIZATION_FAILED;
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
 * (H.264 create and add), flags, template, session}[allocator][present]
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

	/* The session must exist and the H.264 create record be given; a template must be of the same session. */
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
	} else if (!chain.has_h264_create || chain.add_error != 0) {
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
 * vkDestroyQueryPool: [device][object][allocator], no reply body.  The
 * registry lets go of the object; recorded commands that name it keep it.
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

/* Frees a session after its last owner: the firmware decoder, then the device edge. */
static int
release_session(
	struct bcm2711_vulkan_session *session,
	void *payload)
{
	struct video_session *video;
	int error;
	int released;

	/*
	 * The decodes in flight are given up and the kept frames dropped, then
	 * the decoder goes; its memory stays if a bulk may still move it.
	 */
	(void)session;
	video = payload;
	error = drop_all(video);
	if (video_current == video)
		video_current = NULL;
	if (video->decoder != NULL)
		drv_rpi4_mmal_decoder_destroy(video->decoder);
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
 * a use the executor cannot run marks the recording failed (reported at
 * vkEndCommandBuffer) instead of failing the stream.
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
	struct video_resource setup;
	uint64_t present;
	uint64_t session_id;
	uint64_t parameters_id;
	uint64_t buffer_id;
	uint32_t type;
	int error;

	/* Reads the structure's presence, its type and its chain. */
	kern_memset(&chain, 0, sizeof(chain));
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
		read_slots(reader);
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
		if (present != 0U)
			read_slot(reader, &setup);
		read_slots(reader);
		command->has_picture = chain.has_picture;
		command->picture_flags = chain.picture_flags;
		command->sps_id = chain.sps_id;
		command->pps_id = chain.pps_id;
		command->frame_num = chain.frame_num;
		command->poc = chain.poc;
		break;
	}

	/* Refuses a malformed record. */
	chain_free(&chain);
	if (reader->error != 0)
		return EINVAL;

	/* Retains the session and parameters of a begin, the bitstream and output of a decode. */
	error = retain_command(session, command, NULL, session_id, parameters_id, buffer_id, command->destination.view, 0);
	if (error != 0)
		return error;

	/* A decode needs its H.264 picture. */
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
	struct bcm2711_vulkan_object *device,
	uint64_t session_id,
	uint64_t parameters_id,
	uint64_t buffer_id,
	uint64_t view_id,
	uint64_t pool_id)
{
	struct bcm2711_vulkan_object *object;
	int error;

	/* Each identity, if given, must name an object of its kind; the node keeps it. */
	(void)device;
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

	/* A begin needs a session; the parameters may come later (a reset-only scope). */
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

	/* Each retained object, keeping the first failure. */
	command = (struct video_command *)node;
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

	/* The last begin query, cleared by an end query of the same pool and query. */
	query = NULL;
	for (node = command->first; node != NULL; node = node->next) {
		record = (const struct bcm2711_vulkan_record *)node;
		if (record->opcode == GPU_OP_CMD_BEGIN_QUERY)
			query = (const struct video_command *)record;
		if (record->opcode == GPU_OP_CMD_END_QUERY &&
		    query != NULL &&
		    ((const struct video_command *)record)->pool == query->pool &&
		    ((const struct video_command *)record)->query == query->query)
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
	case VIDEO_TYPE_PROFILE_LIST:
		/* [count][array]{profile}: every profile must be H.264 decode; the first refusal is kept. */
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
			read_chain(reader, &nested, depth + 1U);
			chain_free(&nested);
			read_add(reader, chain);
		}

		break;
	case VIDEO_TYPE_H264_PARAMETERS_ADD:
		/* The sets added. */
		read_add(reader, chain);
		break;
	case VIDEO_TYPE_H264_DPB_SLOT:
		/* [present][flags][FrameNum][reserved][2][PicOrderCnt x2], read and dropped. */
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
		 * idr_pic_id, [2] order counts}[slice count][array]{offset}: the
		 * push after a first picture needs its frame_num and order count;
		 * the slice offsets are not needed (the range is sent whole).
		 */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			chain->has_picture = true;
			chain->picture_flags = drv_i915_wire_read_u32(reader);
			chain->sps_id = drv_i915_wire_read_u32(reader);
			chain->pps_id = drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			chain->frame_num = drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
			count = drv_i915_wire_read_u64(reader);
			if (count != 2U)
				reader->error = 1;
			chain->poc = (int32_t)drv_i915_wire_read_u32(reader);
			(void)drv_i915_wire_read_u32(reader);
		}

		/* The slices' offsets, which follow. */
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (count > (uint64_t)(reader->size - reader->offset))
			reader->error = 1;
		for (item = 0U; item < count && reader->error == 0; item++)
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
 * Returns 0 for a profile the decoder takes, or the standard reason another
 * is refused; the H.264 profile it names goes to profile_idc.
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

	/* The type, then the chain that carries the H.264 profile, then the fields. */
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
	/* H.264 decode is the one operation. */
	if (operation != VIDEO_OP_DECODE_H264)
		return VIDEO_ERROR_OPERATION;

	/* 8-bit 4:2:0 is the one format. */
	if (chroma != VIDEO_CHROMA_420 || luma != VIDEO_DEPTH_8 || chroma_depth != VIDEO_DEPTH_8)
		return VIDEO_ERROR_FORMAT;

	/* The H.264 record must be there and progressive. */
	if (!chain->has_h264_profile)
		return VIDEO_ERROR_CODEC;
	if (chain->picture_layout != VIDEO_LAYOUT_PROGRESSIVE)
		return VIDEO_ERROR_LAYOUT;

	/* Baseline always; Main and High unless only Baseline is offered. */
	if (chain->profile_idc == VIDEO_PROFILE_BASELINE)
		return 0;
	if (VIDEO_BASELINE_ONLY)
		return VIDEO_ERROR_CODEC;
	if (chain->profile_idc != VIDEO_PROFILE_MAIN && chain->profile_idc != VIDEO_PROFILE_HIGH)
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
	struct video_resource *resource)
{
	struct video_chain chain;
	uint64_t present;
	uint32_t type;

	/* The type, the chain, the index and the picture, which the decoder does not use. */
	kern_memset(&chain, 0, sizeof(chain));
	kern_memset(resource, 0, sizeof(*resource));
	type = drv_i915_wire_read_u32(reader);
	if (type != VIDEO_TYPE_REFERENCE_SLOT)
		reader->error = 1;
	read_chain(reader, &chain, 0U);
	chain_free(&chain);
	(void)drv_i915_wire_read_u32(reader);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_resource(reader, resource);
}

/* Reads an array of slots, which the decoder does not use. */
static void
read_slots(
	struct i915_wire_reader *reader)
{
	struct video_resource slot;
	uint64_t total;
	uint64_t item;

	/* [count][array]: a count past the bytes left is malformed. */
	(void)drv_i915_wire_read_u32(reader);
	total = drv_i915_wire_read_u64(reader);
	if (total > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0U; item < total && reader->error == 0; item++)
		read_slot(reader, &slot);
}

/*
 * Reads an H.264 add record's fields: [count][array]{SPS}[count][array]{PPS}
 * (libvulkan's video_encode_sps and video_encode_pps).  Each set is a new
 * allocation kept on the chain; a set the executor cannot keep marks
 * add_error.
 */
static void
read_add(
	struct i915_wire_reader *reader,
	struct video_chain *chain)
{
	struct drv_rpi4_h264_sps scratch_sps;
	struct video_pps scratch_pps;
	struct drv_rpi4_h264_sps *sps;
	struct video_pps *pps;
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
		sps->level = drv_i915_wire_read_u32(reader);
		sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
		sps->id = drv_i915_wire_read_u32(reader);
		sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
		sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);
		sps->log2_max_frame_num_minus4 = drv_i915_wire_read_u32(reader);
		sps->pic_order_cnt_type = drv_i915_wire_read_u32(reader);
		sps->offset_for_non_ref_pic = (int32_t)drv_i915_wire_read_u32(reader);
		sps->offset_for_top_to_bottom_field = (int32_t)drv_i915_wire_read_u32(reader);
		sps->log2_max_pic_order_cnt_lsb_minus4 = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
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
			read_scaling(reader, &sps->scaling);
		}

		/* The VUI, which is never sent. */
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U)
			reader->error = 1;

		/* Keeps the set on the chain; an id outside H.264 marks the add malformed. */
		if (sps->id >= VIDEO_SPS_IDS || sps->level > 18U)
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
		pps->fields.flags = drv_i915_wire_read_u32(reader);
		pps->fields.sps_id = drv_i915_wire_read_u32(reader);
		pps->fields.pps_id = drv_i915_wire_read_u32(reader);
		pps->fields.num_ref_idx_l0_default_active_minus1 = drv_i915_wire_read_u32(reader);
		pps->fields.num_ref_idx_l1_default_active_minus1 = drv_i915_wire_read_u32(reader);
		pps->fields.weighted_bipred_idc = drv_i915_wire_read_u32(reader);
		pps->fields.pic_init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
		pps->fields.pic_init_qs_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
		pps->fields.chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
		pps->fields.second_chroma_qp_index_offset = (int32_t)drv_i915_wire_read_u32(reader);
		present = drv_i915_wire_read_u64(reader);
		if (present != 0U) {
			pps->fields.has_scaling = 1;
			read_scaling(reader, &pps->fields.scaling);
		}

		/* Keeps the set on the chain; ids outside H.264 mark the add malformed. */
		if (pps->fields.sps_id >= VIDEO_SPS_IDS || pps->fields.pps_id >= VIDEO_PPS_IDS)
			chain->add_error = EINVAL;
		if (pps != &scratch_pps) {
			pps->next = chain->new_pps;
			chain->new_pps = pps;
		}
	}
}

/* Reads H.264 scaling lists: [present mask][default mask][96]{4x4}[384]{8x8}. */
static void
read_scaling(
	struct i915_wire_reader *reader,
	struct drv_rpi4_h264_scaling *scaling)
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

/* Frees the parameter sets a chain read and nobody took. */
static void
chain_free(
	struct video_chain *chain)
{
	struct video_pps *pps;
	uint32_t index;

	/* The sequence sets, then the picture sets. */
	for (index = 0U; index < chain->new_sps_count; index++) {
		kern_free(chain->new_sps[index]);
		chain->new_sps[index] = NULL;
	}

	/* The chain holds no sequence set any more. */
	chain->new_sps_count = 0U;
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
	struct video_pps *pps;
	struct video_pps *old;
	const struct video_pps *held;
	struct video_pps **link;
	uint32_t index;
	uint32_t id;
	uint32_t sps_count;
	uint32_t pps_count;

	/* An update must not repeat a key the object holds. */
	if (!replace) {
		for (index = 0U; index < chain->new_sps_count; index++) {
			if (parameters->sps[chain->new_sps[index]->id] != NULL)
				return EINVAL;
		}

		/* Nor a picture set key it holds. */
		for (pps = chain->new_pps; pps != NULL; pps = pps->next) {
			held = find_pps(parameters, pps->fields.sps_id, pps->fields.pps_id);
			if (held != NULL)
				return EINVAL;
		}
	}

	/* Refuses, before taking anything, sets beyond the object's capacity (new keys counted as added). */
	sps_count = 0U;
	for (index = 0U; index < VIDEO_SPS_IDS; index++) {
		if (parameters->sps[index] != NULL)
			sps_count++;
	}

	/* The new sequence sets of new keys. */
	for (index = 0U; index < chain->new_sps_count; index++) {
		if (parameters->sps[chain->new_sps[index]->id] == NULL)
			sps_count++;
	}

	/* The picture sets held, and the new ones of new keys. */
	pps_count = 0U;
	for (pps = parameters->pps; pps != NULL; pps = pps->next)
		pps_count++;
	for (pps = chain->new_pps; pps != NULL; pps = pps->next) {
		held = find_pps(parameters, pps->fields.sps_id, pps->fields.pps_id);
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

	/* The chain holds no sequence set any more. */
	chain->new_sps_count = 0U;

	/* The picture sets, replacing any of the same key. */
	while (chain->new_pps != NULL) {
		pps = chain->new_pps;
		chain->new_pps = pps->next;
		link = &parameters->pps;
		while (*link != NULL) {
			if ((*link)->fields.sps_id == pps->fields.sps_id && (*link)->fields.pps_id == pps->fields.pps_id) {
				old = *link;
				*link = old->next;
				kern_free(old);
				continue;
			}

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
parameters_copy(
	struct video_parameters *destination,
	const struct video_parameters *source)
{
	const struct video_pps *pps;
	struct video_pps *copy;
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
		copy->fields = pps->fields;
		copy->next = destination->pps;
		destination->pps = copy;
	}

	/* Succeeded: the template's sets are copied. */
	return 0;
}

/* Frees a parameters object's sets. */
static void
parameters_free(
	struct video_parameters *parameters)
{
	struct video_pps *pps;
	uint32_t index;

	/* The sequence sets, then the picture sets. */
	for (index = 0U; index < VIDEO_SPS_IDS; index++) {
		kern_free(parameters->sps[index]);
		parameters->sps[index] = NULL;
	}
	while (parameters->pps != NULL) {
		pps = parameters->pps;
		parameters->pps = pps->next;
		kern_free(pps);
	}
}

/* Finds the picture set of a key in a parameters object. */
static const struct video_pps *
find_pps(
	const struct video_parameters *parameters,
	uint32_t sps_id,
	uint32_t pps_id)
{
	const struct video_pps *pps;

	/* Walks the list for the key. */
	for (pps = parameters->pps; pps != NULL; pps = pps->next) {
		if (pps->fields.sps_id == sps_id && pps->fields.pps_id == pps_id)
			return pps;
	}

	/* Succeeded: no set of the key. */
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

	/* The pool's size, and the device it lives under. */
	pool->count = count;
	error = bcm2711_vulkan_object_retain(device);
	if (error != 0) {
		kern_free(pool);
		return error;
	}

	/* The pool keeps its device. */
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
	error = retain_command(session, query, NULL, 0, 0, 0, 0, pool_id);
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
 * Runs a coding control: a reset waits for the frames in flight, a step at
 * a time, then drops every kept frame, picture and reference of the
 * decoder, and sends the sets again.
 */
static int
run_control(
	struct bcm2711_render_device *controller,
	const struct video_command *command)
{
	struct video_session *video;
	int64_t newest;
	int error;
	int closed;

	/* Only a reset of a working session does anything. */
	video = command->begin->session->payload;
	if ((command->flags & VIDEO_CONTROL_RESET) == 0 || video->broken)
		return 0;

	/* The decodes before the reset complete first: their frames are looked for until they come or their time is over. */
	if (video->flight_count != 0) {
		newest = video->flights[video->flight_count - 1U].pts;
		error = collect(controller, video, poll_ms(controller), newest);
		video_stats.steps++;
		if (error != 0)
			return error;
		if (video->flight_count != 0)
			return EINPROGRESS;
	}

	/* The flush gives every kept frame's buffer back: the copies go first. */
	error = drop_all(video);
	if (error != 0)
		return error;

	/* Drops the decoder's pictures without the controller mutex. */
	bcm2711_render_window_open(controller);
	error = drv_rpi4_mmal_decoder_flush(video->decoder);
	closed = bcm2711_render_window_close(controller);

	/* The next decode starts again, with its sets. */
	video->sent_sps_bytes = 0;
	video->sent_pps_bytes = 0;
	video->started = false;
	video->sending = NULL;
	if (error != 0)
		skip(video, NULL, "flush failed", error);

	/* Reports a window that did not close. */
	if (closed != 0)
		return closed;

	/* Succeeded: the decoder starts again at the next key frame. */
	return 0;
}

/*
 * Sends one picture's decode to the firmware, a step at a time.
 *
 * Each step first takes the frames that have come.  When every flight is
 * taken, or the staging the unit goes into is still the firmware's, the
 * step waits up to VIDEO_POLL_MS for a frame and ends with EINPROGRESS, so
 * the other sessions take their turns; the next step comes back here.
 * Once sent, the decode is in flight and the event is over: its frame is
 * placed in the output image when it comes (see collect).
 */
static int
run_decode(
	struct bcm2711_render_device *controller,
	const struct video_command *command)
{
	struct video_session *video;
	struct video_parameters *parameters;
	struct bcm2711_vulkan_resource *image;
	struct video_direct direct;
	uint8_t *cpu;
	unsigned wait_ms;
	int64_t until;
	int error;
	int closed;

	/* The scope's session and parameters; a decode's first step starts its clock. */
	video = command->begin->session->payload;
	parameters = NULL;
	if (command->begin->parameters != NULL)
		parameters = command->begin->parameters->payload;
	if (video->sending != command) {
		video->sending = command;
		video->sending_since = clock_ticks();
	}

	/* A broken session decodes nothing more. */
	if (video->broken) {
		video->sending = NULL;
		set_status(command, VIDEO_STATUS_ERROR);
		return 0;
	}

	/* A decode needs parameters. */
	if (parameters == NULL) {
		video->sending = NULL;
		skip(video, command, "no parameters bound", EINVAL);
		return 0;
	}

	/* The output picture must be an NV12 decode output with memory. */
	error = image_target(command->view, &image, &cpu);
	if (error != 0) {
		video->sending = NULL;
		skip(video, command, "output picture not usable", error);
		return 0;
	}

	/* Takes the frames that came; with every flight taken, waits a little for the oldest's. */
	wait_ms = 0;
	until = VIDEO_NO_PTS;
	if (video->flight_count == VIDEO_FLIGHTS) {
		wait_ms = poll_ms(controller);
		until = video->flights[0].pts;
	}

	/* The collection. */
	error = collect(controller, video, wait_ms, until);
	video_stats.steps++;
	if (error != 0) {
		video->sending = NULL;
		return error;
	}

	/* The collection may have found the session broken. */
	if (video->broken) {
		video->sending = NULL;
		set_status(command, VIDEO_STATUS_ERROR);
		return 0;
	}

	/* Still no room: the next step looks again, until the decode's time is over. */
	if (video->flight_count == VIDEO_FLIGHTS) {
		error = wait_or_skip(video, command, "no room in the decoder");
		return error;
	}

	/* A sampled image's run, which the frame goes to directly, stays while the decoder may write it. */
	prepare_direct(image, &direct);

	/* Without the controller mutex: the unit is built and sent. */
	bcm2711_render_window_open(controller);
	error = begin_decode(video, parameters, command, &direct);
	closed = bcm2711_render_window_close(controller);

	/* A unit not sent leaves no target; the run goes. */
	if (error != 0 || closed != 0)
		release_direct(video, &direct.buffer, direct.target.pts, direct.registered);

	/* Reports a window that did not close. */
	if (closed != 0) {
		video->sending = NULL;
		return closed;
	}

	/* The staging is still the firmware's: waits a little for a frame and looks again. */
	if (error == ETIMEDOUT) {
		until = VIDEO_NO_PTS;
		if (video->flight_count != 0)
			until = video->flights[0].pts;
		error = collect(controller, video, poll_ms(controller), until);
		if (error != 0) {
			video->sending = NULL;
			return error;
		}

		/* The next step looks again, until the decode's time is over. */
		error = wait_or_skip(video, command, "no staging for the access unit");
		return error;
	}

	/* A unit that could not be built or sent skips the picture; the firmware may lack the sets, so they go again next time. */
	if (error != 0) {
		video->sending = NULL;
		video->sent_sps_bytes = 0;
		video->sent_pps_bytes = 0;
		video_stats.skipped++;
		skip(video, command, "access unit not sent", error);
		return 0;
	}

	/* The decode is in flight; the flight takes the run. */
	error = add_flight(video, command, &direct);
	video->sending = NULL;
	if (error != 0)
		return error;

	/* Succeeded: the unit is the firmware's; its frame comes later. */
	return 0;
}

/*
 * Builds and sends a decode's access unit into a staging area the firmware
 * has given back (ETIMEDOUT when it has not), and after the first unit of
 * a start the push picture.  A prepared direct run becomes the decode's
 * target first.  The caller has the window open.
 */
static int
begin_decode(
	struct video_session *video,
	const struct video_parameters *parameters,
	const struct video_command *command,
	struct video_direct *direct)
{
	uint8_t *staging;
	uint32_t capacity;
	uint32_t bytes;
	int error;

	/* The staging, only if the firmware has given it back; the executor does not wait here. */
	error = drv_rpi4_mmal_decoder_input(video->decoder, 0, &staging, &capacity);
	if (error != 0)
		return error;

	/* The decode's time stamp: its number in the session (the push's 0 is never one). */
	video->sequence++;

	/* The unit, in the staging. */
	error = build_unit(video, parameters, command, staging, capacity, &bytes);
	if (error != 0)
		return error;

	/*
	 * A sampled image's frame goes to its run directly; the decoder has the
	 * target before the unit can bring the frame.  A target the decoder
	 * cannot take leaves the frame to the bounce memory.
	 */
	if (direct->buffer != NULL) {
		direct->target.pts = video->sequence;
		error = drv_rpi4_mmal_decoder_target(video->decoder, &direct->target);
		if (error == 0)
			direct->registered = true;
	}

	/* Sends it with its time stamp. */
	error = drv_rpi4_mmal_decoder_send(video->decoder, bytes, video->sequence, 0);
	if (error != 0)
		return error;

	/* The first unit of a start is followed by the push; a push that cannot go only leaves the first picture held. */
	if (!video->started) {
		video->started = true;
		error = send_push(video, parameters, command);
		if (error != 0 && video->push_lines < VIDEO_PUSH_LINES) {
			video->push_lines++;
			kern_logf("v3d: video: decode %lld: no push picture after the first (%d); the first picture may be held\n",
				  (long long)video->sequence, error);
		}
	}

	/* Succeeded: the unit is the firmware's. */
	return 0;
}

/*
 * Sends the push picture after the first unit of a start.
 *
 * The firmware holds the first picture after a start until the next
 * picture's slices come, even with the end-of-frame flag (later pictures
 * come out at their own end of frame); the parameter sets again, an access
 * unit delimiter and an empty end of frame did not give it out, and an end
 * of stream lost the references.  A Vulkan decode has no
 * next picture to give, so a grey non-reference picture of the push's own
 * (drv_rpi4_h264_write_push) follows the first one: it changes no
 * reference and no order count, and its frame comes with time stamp
 * VIDEO_PUSH_PTS and is dropped.  Only a reference first picture is pushed
 * (the push's frame_num follows it).
 */
static int
send_push(
	struct video_session *video,
	const struct video_parameters *parameters,
	const struct video_command *command)
{
	const struct drv_rpi4_h264_sps *sps;
	uint8_t *staging;
	uint32_t capacity;
	uint32_t pps_id;
	size_t written;
	int error;

	/* The picture's SPS (build_unit found it), and a reference picture to follow. */
	sps = parameters->sps[command->sps_id];
	if (sps == NULL)
		return ENOENT;
	if ((command->picture_flags & VIDEO_PICTURE_REFERENCE) == 0)
		return ENOTSUP;
	pps_id = unused_pps_id(parameters);

	/* The next staging area (the others are free at a start): the push in its first half, the slice's RBSP in its second. */
	error = drv_rpi4_mmal_decoder_input(video->decoder, VIDEO_POLL_MS, &staging, &capacity);
	if (error != 0)
		return error;
	error = drv_rpi4_h264_write_push(sps, pps_id, command->frame_num + 1U, (uint32_t)command->poc + 1U,
					 staging + capacity / 2U, capacity / 2U, staging, capacity / 2U, &written);
	if (error != 0)
		return error;

	/* Sends it as a frame of its own. */
	error = drv_rpi4_mmal_decoder_send(video->decoder, (uint32_t)written, VIDEO_PUSH_PTS, 0);
	if (error != 0)
		return error;

	/* Succeeded: the push follows the first picture. */
	video_stats.pushes++;
	return 0;
}

/*
 * Puts a sent decode in flight: its time stamp, its output picture's view
 * and its status query, retained until its frame is placed or it is given
 * up, its direct run, and its deadline.
 */
static int
add_flight(
	struct video_session *video,
	const struct video_command *command,
	struct video_direct *direct)
{
	struct video_flight *flight;
	struct video_pool *pool;
	struct bcm2711_vulkan_image_view *view;
	uint64_t now;
	int error;

	/* The next flight, emptied. */
	flight = &video->flights[video->flight_count];
	kern_memset(flight, 0, sizeof(*flight));
	flight->pts = video->sequence;
	flight->command = command;

	/* The output picture's view, and through it its image, stay while the frame may come. */
	error = bcm2711_vulkan_object_retain(command->view);
	if (error != 0) {
		release_direct(video, &direct->buffer, flight->pts, direct->registered);
		return error;
	}

	/* The flight holds the view and knows the image behind it. */
	flight->view = command->view;
	view = command->view->payload;
	flight->image = view->owner.parent;

	/* The status query open around the decode waits for the frame: its end does not make it available before. */
	if (command->status != NULL) {
		error = bcm2711_vulkan_object_retain(command->status->pool);
		if (error != 0) {
			release_direct(video, &direct->buffer, flight->pts, direct->registered);
			(void)bcm2711_vulkan_object_release(flight->view);
			return error;
		}

		/* The query counts the decode as pending, until its frame. */
		flight->pool = command->status->pool;
		flight->query = command->status->query;
		pool = flight->pool->payload;
		if (flight->query < pool->count) {
			flight->generation = pool->generation[flight->query];
			pool->pending[flight->query]++;
		}
	}

	/* The run the frame goes to directly, when the decoder took the target; else the run goes now. */
	if (direct->registered) {
		flight->direct = direct->buffer;
		direct->buffer = NULL;
	} else {
		release_direct(video, &direct->buffer, flight->pts, false);
	}

	/* Its times: the decode's first step, the send, and how long the frame is waited for. */
	now = clock_ticks();
	flight->started_at = video->sending_since;
	flight->sent_at = now;
	flight->deadline = now + kern_ms_to_ticks(VIDEO_FRAME_MS);
	video->flight_count++;
	video_stats.sends++;
	video_stats.depth += video->flight_count;

	/* Succeeded: the decode is in flight. */
	return 0;
}

/*
 * Ends a decode's step that found no room: EINPROGRESS while the decode's
 * time lasts, else the picture is skipped and the event is over.
 */
static int
wait_or_skip(
	struct video_session *video,
	const struct video_command *command,
	const char *reason)
{
	uint64_t deadline;
	uint64_t now;

	/* The decode's time lasts: the next step looks again. */
	deadline = video->sending_since + kern_ms_to_ticks(VIDEO_FRAME_MS);
	now = clock_ticks();
	if (now < deadline)
		return EINPROGRESS;

	/* Its time is over: the picture is skipped. */
	video->sending = NULL;
	video_stats.skipped++;
	skip(video, command, reason, ETIMEDOUT);

	/* Succeeded: the event is over without the picture. */
	return 0;
}

/*
 * Takes the frames that have come, placing each in its flight's image.
 *
 * Under the controller mutex, every flight's image is resolved; then, with
 * the window open, frames are received: until the frame of decode until
 * has come, each receive waits up to wait_ms; after it (or with
 * VIDEO_NO_PTS) only the frames already there are taken.  After the
 * window, the placed flights become kept frames, and flights whose frame
 * was lost or whose time is over are given up.  Returns a failure of the
 * window or of a release; a decoder failure gives the flights up.
 */
static int
collect(
	struct bcm2711_render_device *controller,
	struct video_session *video,
	unsigned wait_ms,
	int64_t until)
{
	struct drv_rpi4_mmal_frame frame;
	struct video_flight *flight;
	const uint8_t *pixels;
	uint64_t limit;
	uint64_t now;
	unsigned timeout;
	unsigned index;
	unsigned taken;
	bool reached;
	bool awaited;
	bool failed;
	int error;
	int closed;
	int retired;

	/* Each flight's image and CPU view, for the window (an image that cannot be reached loses its frame). */
	for (index = 0; index < video->flight_count; index++) {
		flight = &video->flights[index];
		error = image_target(flight->view, &flight->target, &flight->cpu);
		if (error != 0) {
			flight->target = NULL;
			flight->cpu = NULL;
		}
	}

	/* Without the controller mutex: the frames, until the awaited one or none comes in time. */
	bcm2711_render_window_open(controller);
	reached = false;
	if (until == VIDEO_NO_PTS)
		reached = true;
	failed = false;
	limit = clock_ticks() + kern_ms_to_ticks(wait_ms);
	for (taken = 0; taken < 2U * VIDEO_OUTPUTS; taken++) {
		/* The wait: none once the awaited frame is here, else what is left of wait_ms. */
		timeout = 0;
		now = clock_ticks();
		if (!reached && now < limit)
			timeout = (unsigned)kern_ticks_to_ms(limit - now) + 1U;

		/* The next frame, or none in time. */
		error = drv_rpi4_mmal_decoder_receive(video->decoder, timeout, &frame, &pixels);
		if (error == ETIMEDOUT)
			break;
		if (error != 0) {
			failed = true;
			break;
		}

		/* Placed in its flight's image, or dropped; the awaited frame ends the waiting. */
		awaited = take_frame(video, &frame, pixels, until);
		if (awaited)
			reached = true;
	}

	/* The window closes. */
	closed = bcm2711_render_window_close(controller);

	/* A decoder that failed gives every flight up; the firmware may lack the sets, so they go again. */
	if (failed) {
		for (index = 0; index < video->flight_count; index++) {
			if (!video->flights[index].placed)
				video->flights[index].lost = true;
		}

		/* The sets go again, and the line says why. */
		video->sent_sps_bytes = 0;
		video->sent_pps_bytes = 0;
		skip_line(video, VIDEO_NO_PTS, "frame not received", error);
	}

	/* The flights whose frames came, were lost or are overdue. */
	retired = retire_flights(video, clock_ticks());

	/* Reports a window that did not close, or a release that failed. */
	if (closed != 0)
		return closed;
	if (retired != 0)
		return retired;

	/* Succeeded: the frames that came are placed. */
	return 0;
}

/*
 * Takes one received frame in the window: the push's frame, an empty
 * frame and a frame of no flight go back to the firmware; a decode's frame
 * is copied, corrected, into its image, and the flights before it that
 * had no frame have lost theirs (the frames come in decode order).
 * Reports whether the frame is that of decode until or a later one.
 */
static bool
take_frame(
	struct video_session *video,
	const struct drv_rpi4_mmal_frame *frame,
	const uint8_t *pixels,
	int64_t until)
{
	struct video_flight *flight;
	uint64_t received;
	unsigned first;
	unsigned found;
	unsigned index;
	bool raw;

	/* An empty frame carries no picture; the push's frame is not one of the stream's. */
	if (frame->length == 0 || frame->pts == VIDEO_PUSH_PTS) {
		(void)drv_rpi4_mmal_decoder_release(video->decoder, frame->handle);
		return false;
	}

	/* The oldest flight still waiting for its frame. */
	for (first = 0; first < video->flight_count; first++) {
		flight = &video->flights[first];
		if (!flight->placed && !flight->lost)
			break;
	}

	/* The frame's flight: by its time stamp, or the oldest waiting one when it has none. */
	found = video->flight_count;
	if (frame->pts == DRV_RPI4_MMAL_FRAME_NO_TIME) {
		found = first;
	} else {
		for (index = first; index < video->flight_count; index++) {
			if (video->flights[index].pts == frame->pts) {
				found = index;
				break;
			}
		}
	}

	/* A frame of no waiting flight (one given up, or one the decoder made up) goes back, said a few times. */
	if (found == video->flight_count) {
		if (video->late_lines < VIDEO_LATE_LINES) {
			video->late_lines++;
			kern_logf("v3d: video: frame %lld of no decode in flight dropped\n", (long long)frame->pts);
		}

		/* Its buffer goes back. */
		(void)drv_rpi4_mmal_decoder_release(video->decoder, frame->handle);
		return false;
	}

	/* The flights before it never get a frame. */
	for (index = first; index < found; index++)
		video->flights[index].lost = true;

	/* A frame that is not NV12, or whose image cannot be reached, is not the picture. */
	flight = &video->flights[found];
	if (frame->encoding != DRV_RPI4_MMAL_FRAME_NV12 || flight->target == NULL) {
		flight->lost = true;
		(void)drv_rpi4_mmal_decoder_release(video->decoder, frame->handle);
		if (until != VIDEO_NO_PTS && frame->pts >= until)
			return true;
		return false;
	}

	/*
	 * A frame that went to its image directly is in place already, and its
	 * buffer goes back at once.  Any other is copied into its image --
	 * corrected unless the image holds the firmware's samples as they are
	 * (a sampled one) -- and stays in its buffer as the image's cached copy.
	 */
	received = clock_ticks();
	if (frame->direct) {
		(void)drv_rpi4_mmal_decoder_release(video->decoder, frame->handle);
		flight->direct_frame = true;
		kern_memset(&flight->copy, 0, sizeof(flight->copy));
	} else {
		raw = bcm2711_vulkan_video_image_raw(flight->target);
		relayout(frame, pixels, flight->target, flight->cpu, !raw, &flight->copy);
	}

	/* The flight has its frame, and the frame's place among the arrivals and costs. */
	flight->placed = true;
	flight->handle = frame->handle;
	flight->frame_flags = frame->flags;
	flight->received_at = received;
	flight->place_ticks = clock_ticks() - received;

	/* The awaited decode's frame, or a later one, ends the waiting. */
	if (until != VIDEO_NO_PTS && frame->pts >= until)
		return true;

	/* Succeeded: an earlier frame is placed. */
	return false;
}

/*
 * Retires the flights after a collection, oldest first: a placed one
 * writes its status and its frame becomes its image's kept copy; a lost
 * one, or the oldest waiting one whose deadline has passed, is given up
 * (too many timeouts in a row stop the session).  The others stay, in
 * order.
 */
static int
retire_flights(
	struct video_session *video,
	uint64_t now)
{
	struct video_flight *flight;
	unsigned index;
	unsigned kept;
	int32_t status;
	int error;
	int result;

	/* Each flight in order. */
	result = 0;
	kept = 0;
	for (index = 0; index < video->flight_count; index++) {
		flight = &video->flights[index];

		/* A placed frame: its status, its costs, and its copy kept. */
		if (flight->placed) {
			status = VIDEO_STATUS_COMPLETE;
			if ((flight->frame_flags & DRV_RPI4_MMAL_FRAME_CORRUPTED) != 0)
				status = VIDEO_STATUS_ERROR;
			video->timeouts = 0;
			note_decode(flight, now);
			mark_written(flight);
			release_direct(video, &flight->direct, flight->pts, true);
			error = finish_flight(flight, status);
			if (result == 0)
				result = error;
			error = keep_held(video, flight);
			if (result == 0)
				result = error;
			continue;
		}

		/* A lost frame, or the oldest waiting frame past its deadline: the picture is skipped. */
		if (flight->lost || (kept == 0 && now >= flight->deadline)) {
			if (!flight->lost) {
				video->timeouts++;
				if (video->timeouts >= VIDEO_MAX_TIMEOUTS)
					video->broken = true;
				skip_line(video, flight->pts, "no frame from the decoder", ETIMEDOUT);
			} else {
				skip_line(video, flight->pts, "frame lost", EIO);
			}

			/* The sets go again; the target goes, the query says ERROR and the view goes. */
			video->sent_sps_bytes = 0;
			video->sent_pps_bytes = 0;
			video_stats.skipped++;
			release_direct(video, &flight->direct, flight->pts, true);
			error = finish_flight(flight, VIDEO_STATUS_ERROR);
			if (result == 0)
				result = error;
			error = bcm2711_vulkan_object_release(flight->view);
			if (result == 0)
				result = error;
			continue;
		}

		/* Still waiting: it moves up behind the ones kept before it. */
		if (kept != index)
			video->flights[kept] = *flight;
		kept++;
	}

	/* The flights left. */
	video->flight_count = kept;

	/* Reports a release that failed. */
	if (result != 0)
		return result;

	/* Succeeded: the flights left wait for their frames. */
	return 0;
}

/*
 * Writes a decode's status into its status query, if the query has not
 * been reset since, and makes an ended query available with its last
 * decode; then lets the pool go.
 */
static int
finish_flight(
	struct video_flight *flight,
	int32_t status)
{
	struct video_pool *pool;
	uint32_t query;
	int error;

	/* A decode outside a status query has none to write. */
	if (flight->pool == NULL)
		return 0;

	/* The query, unless it was reset since the decode was sent. */
	pool = flight->pool->payload;
	query = flight->query;
	if (query < pool->count && pool->generation[query] == flight->generation) {
		/* One failed decode makes the query's status ERROR. */
		if (status == VIDEO_STATUS_ERROR)
			pool->status[query] = VIDEO_STATUS_ERROR;

		/* The decode is no longer pending; the last one makes an ended query available. */
		if (pool->pending[query] != 0)
			pool->pending[query]--;
		if (pool->pending[query] == 0 && pool->ended[query] != 0) {
			pool->ended[query] = 0;
			pool->available[query] = 1;
		}
	}

	/* The pool goes. */
	error = bcm2711_vulkan_object_release(flight->pool);
	flight->pool = NULL;
	if (error != 0)
		return error;

	/* Succeeded: the query has the decode's status. */
	return 0;
}

/*
 * Keeps a placed flight's frame as its image's cached copy: the image's
 * older copy goes, and the oldest copy goes when there is no room.  The
 * flight's view moves to the kept frame.
 */
static int
keep_held(
	struct video_session *video,
	struct video_flight *flight)
{
	struct video_held *held;
	unsigned index;
	int error;
	int result;

	/* The image's older copy is not its content any more. */
	result = 0;
	index = 0;
	while (index < video->held_count) {
		if (video->held[index].image == flight->target) {
			error = drop_held(video, index);
			if (result == 0)
				result = error;
			continue;
		}

		/* Another image's copy stays. */
		index++;
	}

	/* A frame that went to its image directly leaves no copy; the view goes. */
	if (flight->direct_frame) {
		error = bcm2711_vulkan_object_release(flight->view);
		flight->view = NULL;
		if (result == 0)
			result = error;

		/* Reports a release that failed. */
		if (result != 0)
			return result;

		/* Succeeded: the image's readback reads the image. */
		return 0;
	}

	/* No room: the oldest copy goes (its image has the frame in its own memory). */
	if (video->held_count == VIDEO_HELD) {
		error = drop_held(video, 0);
		if (result == 0)
			result = error;
	}

	/* The copy, with the flight's view. */
	held = &video->held[video->held_count];
	held->view = flight->view;
	held->image = flight->target;
	held->handle = flight->handle;
	held->copy = flight->copy;
	video->held_count++;
	flight->view = NULL;

	/* Reports a release that failed. */
	if (result != 0)
		return result;

	/* Succeeded: the image's readback reads the copy. */
	return 0;
}

/* Drops a kept frame: its buffer goes back to the firmware and its view goes. */
static int
drop_held(
	struct video_session *video,
	unsigned index)
{
	struct bcm2711_vulkan_object *view;
	int error;

	/* The buffer back to the firmware (a broken decoder takes it back at its destruction). */
	(void)drv_rpi4_mmal_decoder_release(video->decoder, video->held[index].handle);
	view = video->held[index].view;

	/* The later ones move up. */
	for (; index + 1U < video->held_count; index++)
		video->held[index] = video->held[index + 1U];
	video->held_count--;

	/* The view goes. */
	error = bcm2711_vulkan_object_release(view);
	if (error != 0)
		return error;

	/* Succeeded: the frame is no image's copy. */
	return 0;
}

/*
 * Gives every flight up (its status ERROR) and drops every kept frame, at
 * a reset or the session's release.
 */
static int
drop_all(
	struct video_session *video)
{
	struct video_flight *flight;
	unsigned index;
	int error;
	int result;

	/* The flights, in order. */
	result = 0;
	for (index = 0; index < video->flight_count; index++) {
		flight = &video->flights[index];
		if (flight->placed && !flight->direct_frame)
			(void)drv_rpi4_mmal_decoder_release(video->decoder, flight->handle);
		release_direct(video, &flight->direct, flight->pts, true);
		error = finish_flight(flight, VIDEO_STATUS_ERROR);
		if (result == 0)
			result = error;
		error = bcm2711_vulkan_object_release(flight->view);
		if (result == 0)
			result = error;
	}

	/* None is in flight. */
	video->flight_count = 0;

	/* The kept frames. */
	while (video->held_count != 0) {
		error = drop_held(video, 0);
		if (result == 0)
			result = error;
	}

	/* Reports a release that failed. */
	if (result != 0)
		return result;

	/* Succeeded: nothing of the decoder is in flight or kept. */
	return 0;
}

/* Reports whether a flight up to decode until still waits for its frame. */
static bool
flight_pending(
	const struct video_session *video,
	int64_t until)
{
	unsigned index;

	/* The flights in order; the oldest is the earliest. */
	for (index = 0; index < video->flight_count; index++) {
		if (video->flights[index].pts <= until)
			return true;
	}

	/* Succeeded: none. */
	return false;
}

/*
 * Builds one access unit: the picture's SPS (VUI asking for no
 * reordering) and PPS when they differ from those last sent or the picture
 * is an IDR, then the application's bitstream range (its slices, each with
 * a start code).
 */
static int
build_unit(
	struct video_session *video,
	const struct video_parameters *parameters,
	const struct video_command *command,
	uint8_t *staging,
	uint32_t capacity,
	uint32_t *bytes)
{
	struct bcm2711_vulkan_resource *buffer;
	struct bcm2711_v3d_view *view;
	const struct drv_rpi4_h264_sps *sps;
	const struct video_pps *pps;
	uint8_t *unit;
	void *source;
	size_t written;
	uint32_t used;
	uint32_t address;
	bool idr;
	int error;

	/* The picture's sets. */
	if (command->sps_id >= VIDEO_SPS_IDS)
		return EINVAL;
	sps = parameters->sps[command->sps_id];
	pps = find_pps(parameters, command->sps_id, command->pps_id);
	if (sps == NULL || pps == NULL)
		return ENOENT;
	idr = false;
	if ((command->picture_flags & VIDEO_PICTURE_IDR) != 0)
		idr = true;

	/* The SPS and the PPS, sent when new or at an IDR. */
	unit = video->unit;
	used = 0;
	error = drv_rpi4_h264_write_sps(sps, 1, unit, sizeof(video->unit), &written);
	if (error == 0)
		error = append_set(staging, capacity, &used, unit, written, video->sent_sps, &video->sent_sps_bytes, idr);
	if (error == 0)
		error = drv_rpi4_h264_write_pps(&pps->fields, sps, unit, sizeof(video->unit), &written);
	if (error == 0)
		error = append_set(staging, capacity, &used, unit, written, video->sent_pps, &video->sent_pps_bytes, idr);
	if (error != 0)
		return error;

	/* The bitstream range, copied from the application's buffer. */
	if (command->buffer == NULL || command->range == 0 || command->range > capacity - used)
		return ENOSPC;
	buffer = command->buffer->payload;
	error = bcm2711_vulkan_resource_backing(buffer, command->offset, command->range, &view, &address, &source);
	if (error != 0)
		return error;
	kern_memcpy(staging + used, source, (size_t)command->range);
	used += (uint32_t)command->range;

	/* Succeeded: the unit is in the staging. */
	*bytes = used;
	return 0;
}

/* Appends a parameter set to the unit when it differs from the one last sent, or when forced. */
static int
append_set(
	uint8_t *staging,
	uint32_t capacity,
	uint32_t *used,
	const uint8_t *unit,
	size_t bytes,
	uint8_t *sent,
	size_t *sent_bytes,
	bool force)
{
	bool same;
	int difference;

	/* The set is the same as the one sent before when its bytes are. */
	same = false;
	if (bytes == *sent_bytes) {
		difference = kern_memcmp(unit, sent, bytes);
		if (difference == 0)
			same = true;
	}

	/* An unchanged set is not sent again unless forced. */
	if (same && !force)
		return 0;
	if (bytes > capacity - *used)
		return ENOSPC;

	/* Appends it and remembers it. */
	kern_memcpy(staging + *used, unit, bytes);
	*used += (uint32_t)bytes;
	kern_memcpy(sent, unit, bytes);
	*sent_bytes = bytes;

	/* Succeeded: the set is in the unit. */
	return 0;
}

/* Resolves a decode's output picture view to its NV12 image and CPU view. */
static int
image_target(
	const struct bcm2711_vulkan_object *view_object,
	struct bcm2711_vulkan_resource **image,
	uint8_t **cpu)
{
	struct bcm2711_vulkan_image_view *view;
	struct bcm2711_vulkan_resource *resource;
	struct bcm2711_v3d_view *backing;
	uint64_t chroma;
	uint64_t bytes;
	uint32_t pitch;
	uint32_t address;
	void *mapped;
	int error;

	/* The view's image must be an NV12 decode output. */
	if (view_object == NULL)
		return EINVAL;
	view = view_object->payload;
	if (view->owner.parent == NULL)
		return EINVAL;
	resource = view->owner.parent->payload;
	if (resource->format != (VkFormat)BCM2711_VIDEO_FORMAT_NV12 ||
	    (resource->usage & BCM2711_VIDEO_USAGE_DECODE_DST) == 0)
		return EINVAL;

	/* Its whole bound memory, mapped for the CPU. */
	bcm2711_vulkan_video_nv12_layout(resource->width, resource->height, &pitch, &chroma, &bytes);
	error = bcm2711_vulkan_resource_backing(resource, 0, bytes, &backing, &address, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the image and where the CPU reaches it. */
	*image = resource;
	*cpu = mapped;
	return 0;
}

/*
 * Prepares the direct run of a decode's output image: a sampled NV12 image
 * whose memory run lies in the VPU's window gets its frame there, without
 * the CPU's copy.  direct has no run otherwise.  The caller holds the
 * controller mutex.
 */
static void
prepare_direct(
	struct bcm2711_vulkan_resource *image,
	struct video_direct *direct)
{
	struct bcm2711_v3d_view *backing;
	uint64_t chroma;
	uint64_t bytes;
	uint64_t physical;
	uint32_t pitch;
	uint32_t address;
	void *mapped;
	bool raw;
	int error;

	/* No run until one is found. */
	kern_memset(direct, 0, sizeof(*direct));

	/* Only an image the GPU samples holds the firmware's samples as they come. */
	raw = bcm2711_vulkan_video_image_raw(image);
	if (!raw)
		return;

	/* The image's run, which must be uncached for the CPU. */
	bcm2711_vulkan_video_nv12_layout(image->width, image->height, &pitch, &chroma, &bytes);
	error = bcm2711_vulkan_resource_backing(image, 0, bytes, &backing, &address, &mapped);
	if (error != 0)
		return;
	if (!backing->buffer->uncached)
		return;

	/* Where the image starts in physical memory; the VPU reaches only the first 1 GiB. */
	physical = backing->buffer->memory.paddr + (address - backing->address);
	if (physical + bytes > VIDEO_DIRECT_LIMIT)
		return;

	/* The run stays until the decoder's target is removed. */
	bcm2711_buffer_retain(backing->buffer);
	direct->buffer = backing->buffer;
	direct->target.physical = physical;
	direct->target.bytes = (uint32_t)bytes;
	direct->target.pitch = pitch;
	direct->target.chroma = (uint32_t)chroma;
}

/*
 * Lets a direct run go: the decoder's target of decode pts is removed when
 * registered says it has one, and the run is released unless a transfer
 * into it did not complete, in which case it is kept for good.
 */
static void
release_direct(
	struct video_session *video,
	struct bcm2711_buffer **buffer,
	int64_t pts,
	bool registered)
{
	bool kept;

	/* No run, nothing to let go. */
	if (*buffer == NULL)
		return;

	/* The decoder's target goes first, so no later frame goes to the run. */
	kept = false;
	if (registered && video->decoder != NULL)
		drv_rpi4_mmal_decoder_untarget(video->decoder, pts, &kept);

	/* A run the VPU may still write is never freed. */
	if (kept) {
		kern_logf("v3d: video: decode %lld: its image's memory kept for good: a transfer into it did not complete\n", (long long)pts);
		*buffer = NULL;
		return;
	}

	/* The run goes. */
	bcm2711_buffer_release(*buffer);
	*buffer = NULL;
}

/*
 * Tells the image's run that its frame changed it, so a texture made from
 * it earlier is made again.  The caller holds the controller mutex.
 */
static void
mark_written(
	struct video_flight *flight)
{
	struct bcm2711_v3d_view *backing;
	uint64_t chroma;
	uint64_t bytes;
	uint32_t pitch;
	uint32_t address;
	void *mapped;
	int error;

	/* An image that cannot be reached has no run to tell. */
	if (flight->target == NULL)
		return;

	/* The image's run and where the image starts in it. */
	bcm2711_vulkan_video_nv12_layout(flight->target->width, flight->target->height, &pitch, &chroma, &bytes);
	error = bcm2711_vulkan_resource_backing(flight->target, 0, bytes, &backing, &address, &mapped);
	if (error != 0)
		return;

	/* The whole image changed. */
	bcm2711_v3d_memory_written(backing->buffer, address - backing->address, NULL);
}

/*
 * Copies a frame from the decoder's cached memory into the image's layout,
 * row by row, with the firmware's footroom error undone when correct says
 * so: the luma rows, then the chroma rows, each as wide as both layouts
 * allow.  copy receives
 * where the part copied is in the decoder's memory, for the image's
 * cached copy.
 */
static void
relayout(
	const struct drv_rpi4_mmal_frame *frame,
	const uint8_t *pixels,
	const struct bcm2711_vulkan_resource *image,
	uint8_t *cpu,
	bool correct,
	struct video_copy *copy)
{
	const uint8_t *luma;
	const uint8_t *chroma_plane;
	uint64_t chroma;
	uint64_t bytes;
	uint32_t pitch;
	uint32_t source_pitch;
	uint32_t source_chroma;
	uint32_t width;
	uint32_t rows;
	uint32_t row;

	/* The two layouts. */
	bcm2711_vulkan_video_nv12_layout(image->width, image->height, &pitch, &chroma, &bytes);
	source_pitch = frame->plane_pitch[0];
	source_chroma = frame->plane_offset[1];
	if (frame->planes < 2U || source_pitch == 0) {
		source_pitch = frame->width;
		source_chroma = frame->width * frame->height;
	}
	luma = pixels + frame->plane_offset[0];
	chroma_plane = pixels + source_chroma;

	/* The rows and the bytes of each row both layouts have. */
	width = image->width;
	if (width > source_pitch)
		width = source_pitch;
	rows = image->height;
	if (rows > frame->height)
		rows = frame->height;

	/* The luma rows, corrected or as they are. */
	for (row = 0; row < rows; row++) {
		if (correct) {
			drv_rpi4_mmal_copy_samples(cpu + (size_t)row * pitch, luma + (size_t)row * source_pitch, width);
		} else {
			kern_memcpy(cpu + (size_t)row * pitch, luma + (size_t)row * source_pitch, width);
		}
	}

	/* The chroma rows, half as many. */
	for (row = 0; row < rows / 2U; row++) {
		if (correct) {
			drv_rpi4_mmal_copy_samples(cpu + chroma + (size_t)row * pitch, chroma_plane + (size_t)row * source_pitch, width);
		} else {
			kern_memcpy(cpu + chroma + (size_t)row * pitch, chroma_plane + (size_t)row * source_pitch, width);
		}
	}

	/* The part copied, for the image's readback. */
	copy->luma = luma;
	copy->chroma = chroma_plane;
	copy->pitch = source_pitch;
	copy->width = width;
	copy->rows = rows;
}

/*
 * Gives how long a step may wait for the decoder: VIDEO_POLL_MS, or nothing
 * while another session's execution waits behind the step, so the executor
 * goes on with it and the step looks again at its next turn.
 */
static unsigned
poll_ms(
	const struct bcm2711_render_device *controller)
{
	/* Others wait: only look. */
	if (controller->worker.contended)
		return 0U;

	/* Succeeded: the executor has nothing else to do meanwhile. */
	return VIDEO_POLL_MS;
}

/* Reports the highest PPS id no set of a parameters object uses, for the push's own PPS. */
static uint32_t
unused_pps_id(
	const struct video_parameters *parameters)
{
	const struct video_pps *pps;
	uint32_t id;
	bool used;

	/* From 255 down, the first id no set has (an object holds at most 256 sets). */
	for (id = VIDEO_PPS_IDS - 1U; id > 0; id--) {
		/* Whether any set of the object has the id. */
		used = false;
		for (pps = parameters->pps; pps != NULL; pps = pps->next) {
			if (pps->fields.pps_id == id)
				used = true;
		}

		/* The id is free. */
		if (!used)
			break;
	}

	/* Succeeded: the id (0 only when every other one is taken). */
	return id;
}

/* Adds a placed decode to the cost line, and writes the line when its window is over. */
static void
note_decode(
	const struct video_flight *flight,
	uint64_t now)
{
	uint64_t frame;

	/* The decode's times: to its unit sent, to its frame received, and its placing. */
	frame = flight->received_at - flight->sent_at;
	video_stats.decodes++;
	video_stats.send_ticks += flight->sent_at - flight->started_at;
	video_stats.frame_ticks += frame;
	if (frame > video_stats.frame_max)
		video_stats.frame_max = frame;
	video_stats.place_ticks += flight->place_ticks;

	/* The line, when the window is over. */
	report_stats(now);
}

/*
 * Writes the cost line of the window when VIDEO_STATS_MS have passed since
 * it opened (the first VIDEO_STATS_REPORTS windows of a boot), and opens
 * the next.
 */
static void
report_stats(
	uint64_t now)
{
	uint64_t elapsed;
	uint32_t reports;

	/* The first decode opens the first window. */
	if (video_stats.window_start == 0) {
		video_stats.window_start = now;
		return;
	}

	/* A window shorter than VIDEO_STATS_MS keeps counting. */
	elapsed = kern_ticks_to_ms(now - video_stats.window_start);
	if (elapsed < VIDEO_STATS_MS)
		return;

	/* The averages of the window, while the boot's lines last. */
	if (video_stats.reports < VIDEO_STATS_REPORTS) {
		kern_logf("v3d: video: %u decodes in %llu ms (%u skipped, %u pushes, %u steps, depth %llu.%llu); avg_us send %llu frame %llu place %llu, max_us frame %llu; readbacks %u cached avg_us %llu, %u uncached avg_us %llu; waits %u avg_us %llu\n",
			  (unsigned)video_stats.decodes,
			  (unsigned long long)elapsed,
			  (unsigned)video_stats.skipped,
			  (unsigned)video_stats.pushes,
			  (unsigned)video_stats.steps,
			  (unsigned long long)(average_tenths(video_stats.depth, video_stats.sends) / 10U),
			  (unsigned long long)(average_tenths(video_stats.depth, video_stats.sends) % 10U),
			  (unsigned long long)average_us(video_stats.send_ticks, video_stats.decodes),
			  (unsigned long long)average_us(video_stats.frame_ticks, video_stats.decodes),
			  (unsigned long long)average_us(video_stats.place_ticks, video_stats.decodes),
			  (unsigned long long)average_us(video_stats.frame_max, 1U),
			  (unsigned)video_stats.cached_readbacks,
			  (unsigned long long)average_us(video_stats.cached_ticks, video_stats.cached_readbacks),
			  (unsigned)video_stats.uncached_readbacks,
			  (unsigned long long)average_us(video_stats.uncached_ticks, video_stats.uncached_readbacks),
			  (unsigned)video_stats.waits,
			  (unsigned long long)average_us(video_stats.wait_ticks, video_stats.waits));
	}

	/* The next window starts empty; the count of lines written stays. */
	reports = video_stats.reports + 1U;
	kern_memset(&video_stats, 0, sizeof(video_stats));
	video_stats.reports = reports;
	video_stats.window_start = now;
}

/*
 * Reports the mean of a tick sum in microseconds.  The ticks are coarse
 * (a millisecond), but the sum of many independently phased times still
 * estimates the mean.
 */
static uint64_t
average_us(
	uint64_t ticks,
	uint32_t count)
{
	uint64_t microseconds;

	/* An empty sum has no mean. */
	if (count == 0)
		return 0;

	/* Succeeded: the mean. */
	microseconds = kern_ticks_to_rate(ticks, 1000000U);
	return microseconds / count;
}

/* Reports the mean of a sum in tenths. */
static uint64_t
average_tenths(
	uint64_t sum,
	uint32_t count)
{
	/* An empty sum has no mean. */
	if (count == 0)
		return 0;

	/* Succeeded: the mean, times ten. */
	return sum * 10U / count;
}

/* Writes a decode's status into the status query open around it, if any. */
static void
set_status(
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
	/* The line, then the status. */
	skip_line(video, video->sequence, reason, error);
	set_status(command, VIDEO_STATUS_ERROR);
}

/* Logs why a picture was skipped, a few times a session. */
static void
skip_line(
	const struct video_session *video,
	int64_t pts,
	const char *reason,
	int error)
{
	/* The first lines of a session say why; later ones only count. */
	if (video->timeouts < VIDEO_MAX_TIMEOUTS || video->sequence < 4)
		kern_logf("v3d: video: picture %lld skipped: %s (%d)\n", (long long)pts, reason, error);
}
