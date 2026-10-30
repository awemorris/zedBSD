/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Video Player (WS122): opens a file, plays it, pauses, stops and seeks.
 * The container is read and decoded by libmedia (ws177-p031): its reader
 * (mediafile, WS122 p003) and its decoders (the add-in that opens FFmpeg's
 * libavcodec, libavutil and libswscale, when the system has them, with
 * dlopen and without FFmpeg's headers, p004).  Without them the player says
 * that playing needs libavcodec.  The sound goes to libkeiland's sound
 * stream (audio.c, WS191); the window, its menu and the controls are
 * libkeiland's.
 *
 * Two threads: the window's (main.c), which draws the picture whose time
 * has come and takes the input, and the media thread (media.c), which reads
 * and decodes ahead, keeps a few pictures and writes the sound.  The clock
 * is the sound's (the position the stream has played) when there is sound,
 * otherwise the monotonic clock.
 */

#ifndef VIDEOPLAYER_VIDEOPLAYER_H
#define VIDEOPLAYER_VIDEOPLAYER_H

#include "userland/desktop/libmedia/media.h"
#include "userland/desktop/media-app/decoder.h"

#include <keiland/keiland_thread.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

/* How many decoded pictures wait at most. */
#define VP_PICTURES		8U

/* The media's state, as the window shows it. */
#define VP_EMPTY		0U	/* nothing open */
#define VP_PLAYING		1U
#define VP_PAUSED		2U
#define VP_FAILED        4U	/* decoding stopped with an error */
#define VP_ENDED		3U	/* played to its end */

struct kl_audio_stream;
struct kl_window;

/*
 * Whether the player draws with its own presenter and takes pictures the
 * GPU holds: the CPU cannot convert and scale a picture at its rate on the
 * Raspberry Pi 4, nor read one out of a discrete GPU's memory fast enough.
 * A device that cannot import memory gets pictures the CPU scaled instead.
 * Without the presenter the frame is drawn by the CPU and shown by
 * libkeiland.
 */
#define VP_GPU_PRESENT		1

/*
 * With the presenter, the pictures decoded ahead at most, and the pictures
 * the player holds at once from the decoder: those, one held back by the
 * reader, the one shown, and those the frames still on the GPU drew.  Two
 * ahead, not three, leaves the decoder four pictures in flight within the
 * 17 slots of a High-profile stream (ws236-p011: 1080p60 decoded at about
 * 41 fps with three).
 */
#define VP_GPU_PICTURES		2U
#define VP_GPU_HOLD		(VP_GPU_PICTURES + 2U + VP_PRESENT_FRAMES)

/* The frames the presenter has on the GPU at once. */
#define VP_PRESENT_FRAMES	2U

/* The decoder images the presenter imports at most (one for each image of a decoding context). */
#define VP_PRESENT_IMPORTS	18U

/*
 * One decoder image the presenter imported: the image over the decoder's
 * memory, that memory, the views of its luma (R8) and chroma (RG8) planes,
 * the set that binds them, and whether it is in the shader's layout.
 */
struct vp_present_import {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView luma;
	VkImageView chroma;
	VkDescriptorSet set;
	int ready;
};

/* One swapchain image's view, framebuffer and the semaphore its present waits for. */
struct vp_present_target {
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/*
 * Where a frame shows a picture the GPU holds: the picture, and its
 * rectangle in window pixels.
 */
struct vp_present_quad {
	struct media_gpu_picture picture;
	int x;
	int y;
	int width;
	int height;
};

/*
 * The player's presenter (present.c): its Vulkan device, the swapchain of
 * the window, the overlay drawn by the CPU (bar, notices, pictures the CPU
 * scaled) and its image, the pipelines of the picture and the overlay, and
 * the decoder images imported (of the decoding context identity).  Each
 * frame on the GPU has a slot (command buffer, fence, acquire semaphore,
 * and the serial of the frame in it, 0 for none); serial is the newest
 * frame submitted and completed the newest known done.  acquire_us,
 * submit_us and wait_us add up what the frames waited for (the caller
 * reads and clears them).  operation names the last Vulkan call, for the
 * failure's log line.
 */
struct vp_present {
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical;
	uint32_t family;
	VkDevice device;
	VkQueue queue;
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	struct vp_present_target *targets;
	uint32_t count;
	VkRenderPass pass;
	VkCommandPool pool;
	VkCommandBuffer commands[VP_PRESENT_FRAMES];
	VkFence fences[VP_PRESENT_FRAMES];
	VkSemaphore acquired[VP_PRESENT_FRAMES];
	uint64_t slot_serial[VP_PRESENT_FRAMES];
	uint64_t serial;
	uint64_t completed;
	uint64_t acquire_us;
	uint64_t submit_us;
	uint64_t wait_us;
	VkSampler nearest;
	VkSampler smooth;
	VkDescriptorSetLayout picture_set_layout;
	VkDescriptorSetLayout overlay_set_layout;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet overlay_set;
	VkPipelineLayout picture_layout;
	VkPipelineLayout overlay_layout;
	VkPipeline picture_pipeline;
	VkPipeline overlay_pipeline;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	float *vertex_map;
	VkImage overlay;
	VkDeviceMemory overlay_memory;
	VkImageView overlay_view;
	unsigned char *overlay_map;
	size_t overlay_pitch;
	int overlay_ready;
	int overlay_written;
	int imports_supported;
	uint64_t identity;
	struct vp_present_import imports[VP_PRESENT_IMPORTS];
	const char *operation;
};

/*
 * The player's playback stream (audio.c): libkeiland's sound stream
 * (kl_audio_stream_*, WS191), its format, and whether it was made and
 * runs.  created and rate are read by the players' clocks; running is the
 * state the last start or stop left.
 */
struct vp_audio {
	struct kl_audio_stream *stream;
	uint32_t rate;
	uint32_t channels;
	uint32_t capacity;
	int created;
	int running;
};

/*
 * The media (media.c): the file, what is known of it, the pictures decoded
 * ahead, the clock's anchors and the requests to the media thread.  The
 * lock covers every field after it; the condition wakes the thread when a
 * picture is taken or a request comes.
 */
struct vp_media {
	struct kl_thread *thread;
	int thread_started;
	struct vp_audio *audio;

	struct kl_mutex lock;
	struct kl_cond wake;

	/* What is open: the path, the picture's size, the length (seconds), and whether there is sound. */
	char path[1024];
	int width;
	int height;
	double duration;
	int has_audio;
	int error;

	/* The state, and the end of the file reached by the reader. */
	unsigned state;
	int eof;

	/* Why the last open failed (MEDIA_PROBLEM_*, 0 for another reason or none). */
	int codec_problem;
	int failure;

	/* The pictures decoded ahead (a ring of references), with their times (seconds). */
	struct app_frame *pictures[VP_PICTURES];
	double picture_times[VP_PICTURES];
	unsigned picture_first;
	unsigned picture_count;
	unsigned picture_limit;	/* The pictures the ring holds at most (VP_PICTURES, or VP_GPU_PICTURES with the presenter). */
	unsigned late;	/* Due pictures superseded before presentation; the window owns this count. */

	/* The clock: the time at an anchor, and the anchor (the stream's position heard, or the monotonic time). */
	double clock_time;
	uint64_t clock_frames;
	uint64_t clock_us;

	/* Requests: a seek (to a time), and the end of the thread. */
	int seek_wanted;
	double seek_to;
	int quit;
};

/* The sound (audio.c). */
int vp_audio_open(struct vp_audio *audio);
void vp_audio_close(struct vp_audio *audio);
int vp_audio_start(struct vp_audio *audio);
int vp_audio_stop(struct vp_audio *audio);
int vp_audio_flush(struct vp_audio *audio);
uint64_t vp_audio_read_position(const struct vp_audio *audio);
uint64_t vp_audio_write_position(const struct vp_audio *audio);
uint64_t vp_audio_clock_position(const struct vp_audio *audio);
void vp_audio_renew(struct vp_audio *audio);
int vp_audio_lost(struct vp_audio *audio);
size_t vp_audio_write(struct vp_audio *audio, const int16_t *samples, size_t frames);

/* The media (media.c). */
void vp_media_init(struct vp_media *media, struct vp_audio *audio);
int vp_media_open(struct vp_media *media, const char *path);
void vp_media_close(struct vp_media *media);
void vp_media_play(struct vp_media *media);
void vp_media_pause(struct vp_media *media);
void vp_media_seek(struct vp_media *media, double seconds);
double vp_media_clock(struct vp_media *media);
struct app_frame *vp_media_take(struct vp_media *media, double clock, double *time, double *next);

/* The presenter (present.c). */
VkResult vp_present_open(struct vp_present *present, struct kl_window *window);
VkResult vp_present_resize(struct vp_present *present, uint32_t width, uint32_t height);
VkResult vp_present_frame(struct vp_present *present, const uint32_t *pixels, size_t stride, int overlay_shown, int overlay_changed, const struct vp_present_quad *quad);
uint64_t vp_present_done(const struct vp_present *present);
VkResult vp_present_idle(struct vp_present *present);
void vp_present_close(struct vp_present *present);

/* The log the tests read (main.c). */
void vp_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif
