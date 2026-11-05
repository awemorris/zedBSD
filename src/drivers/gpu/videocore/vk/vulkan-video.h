/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Vulkan Video H.264 decode on the Raspberry Pi 4, with the
 * VideoCore firmware's decoder doing the decode.
 *
 * The V3D device's one queue family also decodes: the video commands,
 * the result status query pools and the decode's CPU-side execution in the
 * native job live here.  Built only with CONFIG_DRIVER_BCM2711_VIDEO.
 */

#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_VIDEO_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_VIDEO_H

#include "drivers/gpu/bcm2711/vk/vulkan-private.h"
#include "drivers/gpu/bcm2711/vk/vulkan-record.h"

struct bcm2711_render_device;
struct bcm2711_vulkan_resource;
struct bcm2711_vulkan_command_buffer;

/* The NV12 format and the video image usages and layouts the executor accepts. */
#define BCM2711_VIDEO_FORMAT_NV12		1000156003U
#define BCM2711_VIDEO_USAGE_DECODE_DST		0x400U
#define BCM2711_VIDEO_USAGE_DECODE_SRC		0x800U
#define BCM2711_VIDEO_USAGE_DECODE_DPB		0x1000U
#define BCM2711_VIDEO_BUFFER_USAGE_DECODE_SRC	0x2000U
#define BCM2711_VIDEO_LAYOUT_DECODE_DST		1000024000U
#define BCM2711_VIDEO_LAYOUT_DECODE_SRC		1000024001U
#define BCM2711_VIDEO_LAYOUT_DECODE_DPB		1000024002U
#define BCM2711_VIDEO_QUEUE_DECODE		0x20U
#define BCM2711_VIDEO_FORMAT_FEATURES		(0x02000000U | 0x04000000U)

/* Reports whether video decode can be offered now: the firmware's link is up. */
bool bcm2711_vulkan_video_available(void);

/* Reports whether a session promised video decode in its capset. */
bool bcm2711_vulkan_video_session(const struct bcm2711_vulkan_session *session);

/* Records that a session's capset promised video decode. */
void bcm2711_vulkan_video_promise(struct bcm2711_vulkan_session *session);

/* Routes the video commands: the queries, the sessions, the parameters and the four recorded ones. */
int bcm2711_vulkan_video_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* Routes the query pool commands (result status pools only). */
int bcm2711_vulkan_video_query_dispatch(struct bcm2711_vulkan_session *session, uint32_t opcode, uint32_t requested, struct i915_wire_reader *reader, struct i915_wire_writer *reply, int *handled);

/* Reports whether a recorded opcode is one this file executes in the native job. */
bool bcm2711_vulkan_video_event(uint32_t opcode);

/*
 * Executes one recorded video or query event in the native job, with the
 * controller mutex held.  EINPROGRESS: a decode that has no room in the
 * decoder yet, or a reset whose decodes' frames have not come; the job's
 * step ends at the event and the next step runs it again.
 */
int bcm2711_vulkan_video_run(struct bcm2711_render_device *controller, const struct bcm2711_vulkan_record *record);

/*
 * Waits for the frames of the decodes in flight whose images a copy, a
 * blit or an upload names, with the controller mutex held: zero when they
 * are placed (or given up), EINPROGRESS while they have not come; the
 * job's step then ends at the command and the next step calls again.
 */
int bcm2711_vulkan_video_settle(struct bcm2711_render_device *controller, const struct bcm2711_vulkan_record *record);

/*
 * The decodes in flight an execution with a fence waits for before it ends:
 * those up to time stamp until of the session of generation generation.
 * Zero generation: none.
 */
struct bcm2711_vulkan_video_mark {
	uint64_t generation;
	int64_t until;
};

/* Adds the newest decode in flight that a command buffer recorded to a mark, after its job ran; with the controller mutex held. */
void bcm2711_vulkan_video_mark_commands(struct bcm2711_vulkan_video_mark *mark, const struct bcm2711_vulkan_command_buffer *command);

/* Makes a mark cover every decode in flight, for an empty submission with a fence; with the controller mutex held. */
void bcm2711_vulkan_video_mark_all(struct bcm2711_vulkan_video_mark *mark);

/*
 * Waits for the frames of the decodes a mark covers, with the controller
 * mutex held: zero when they are placed (or given up, or the session is
 * gone), EINPROGRESS while one has not come.  wait lets it look for them a
 * little with the controller's window open.
 */
int bcm2711_vulkan_video_settle_mark(struct bcm2711_render_device *controller, const struct bcm2711_vulkan_video_mark *mark, bool wait);

/* Lays out an NV12 image: the row pitch, the chroma plane's offset and the bytes. */
void bcm2711_vulkan_video_nv12_layout(uint32_t width, uint32_t height, uint32_t *pitch, uint64_t *chroma, uint64_t *bytes);

/*
 * Reports whether a decoded NV12 image holds the firmware's samples as they
 * are (a sampled one), so its readback corrects them.
 */
bool bcm2711_vulkan_video_image_raw(const struct bcm2711_vulkan_resource *image);

/*
 * Copies rows of a decoded NV12 image to a buffer from the image's cached
 * copy of its last decoded frame; false, copying nothing, when it has none
 * for those rows.  With the controller mutex held.
 */
bool bcm2711_vulkan_video_readback(const struct bcm2711_vulkan_resource *image, uint64_t offset, uint64_t bytes, uint32_t rows, uint8_t *destination, uint64_t destination_pitch);

/* Adds the time of an NV12 image's readback (from the cached copy or the image) to the periodic cost line. */
void bcm2711_vulkan_video_readback_done(bool cached, uint64_t ticks);

#endif
