/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What vkvideo-probe's parts share: the options of a run, the Vulkan
 * objects of a decode (struct probe), the frames printed in display
 * order, and the steps main.c makes for either codec (the device, the
 * pictures, the bitstream buffer, the session, the commands) that the
 * H.265 decode (h265.c) uses too.
 */

#ifndef VKVIDEO_PROBE_PROBE_H
#define VKVIDEO_PROBE_PROBE_H

#include "dpb.h"

#include <vulkan/vulkan.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The codecs the probe decodes. */
#define PROBE_H264		0
#define PROBE_H265		1

/* The exit status of a stream whose H.264 profile the decoder does not take. */
#define PROBE_REFUSED		6

/* How long a decode may take before the probe gives up, in nanoseconds. */
#define PROBE_WAIT_NS		5000000000ULL

/* The most session memory bindings the probe binds. */
#define PROBE_BINDINGS		32U

/* The most frames waiting for their place in the display order. */
#define PROBE_PENDING		256U

/* The most families and extensions the list shows. */
#define PROBE_FAMILIES		16U
#define PROBE_EXTENSIONS	512U

/* One decoded frame waiting for its place in the display order: its order count and hash. */
struct probe_frame {
	int32_t poc;
	char hash[65];
};

/* The frames decoded and not yet printed, what was printed and matched, and the decodes that did not complete. */
struct probe_output {
	struct probe_frame pending[PROBE_PENDING];
	uint32_t pending_count;
	uint32_t printed;
	uint32_t matched;
	uint32_t failed;
	FILE *expect;
};

/* What one run asked for. */
struct probe_options {
	int list;
	const char *stream;
	const char *expect;
	uint32_t frames;
	int time;
	const char *dump;
	int sampled;
	int reset_at_idr;
	int no_hash;
	int h265;
};

/* The decode times of a run with --time, in nanoseconds. */
struct probe_timing {
	uint32_t count;
	uint64_t total;
	uint64_t longest;
};

/*
 * The Vulkan objects of a decode, made in order and destroyed in reverse
 * by probe_close; a handle never made is VK_NULL_HANDLE.
 */
struct probe {
	VkInstance instance;
	VkPhysicalDevice physical;
	uint32_t family;

	/* Whether decoded planes are linear rows (any decoder but Intel's, whose OPTIMAL planes are Y-tiled). */
	int linear;
	VkDevice device;
	VkQueue queue;
	VkPhysicalDeviceMemoryProperties memory;

	/*
	 * The codec (PROBE_H264 or PROBE_H265), its decode profile (the
	 * H.264 or the H.265 record chained to the profile), and the list of
	 * it buffers and images name.
	 */
	int codec;
	VkVideoDecodeH264ProfileInfoKHR h264_profile;
	VkVideoDecodeH265ProfileInfoKHR h265_profile;
	VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR profile_list;

	/* The largest coded picture of the stream. */
	VkExtent2D extent;

	/* The session, its memory and its parameters. */
	VkVideoSessionKHR session;
	VkDeviceMemory session_memory[PROBE_BINDINGS];
	uint32_t session_memory_count;
	VkVideoSessionParametersKHR parameters;

	/* The stream's generation of parameter sets the parameters hold, and how many parameters were made. */
	uint32_t parameters_generation;
	uint32_t parameters_made;

	/*
	 * Whether the pictures are sampled ones read back by a copy (--sampled),
	 * and then the buffer of the image's layout the copy writes, its memory
	 * (mapped) and its size.
	 */
	int sampled;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *readback_map;
	VkDeviceSize readback_size;

	/* The pictures, one a DPB slot: each image, its memory (mapped, or the readback's) and its view; the references the session reads. */
	uint32_t slots;
	uint32_t max_references;
	VkImage image[DPB_SLOTS];
	VkDeviceMemory image_memory[DPB_SLOTS];
	uint8_t *image_map[DPB_SLOTS];
	VkImageView view[DPB_SLOTS];

	/* The bitstream buffer and its memory (mapped). */
	VkBuffer buffer;
	VkDeviceMemory buffer_memory;
	uint8_t *buffer_map;
	VkDeviceSize buffer_size;

	/* The command buffer of the video family, and the fence a decode is waited on with. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;

	/*
	 * Whether the video family reports result status, the pool of the one
	 * query each decode is in (VK_NULL_HANDLE without one), and the status
	 * of the last decode (COMPLETE without a pool).
	 */
	VkBool32 status_supported;
	VkQueryPool status_pool;
	int32_t status;
};

int probe_device(struct probe *probe);
int probe_capabilities(struct probe *probe);
int probe_pictures(struct probe *probe);
int probe_bitstream(struct probe *probe, size_t bytes);
int probe_session(struct probe *probe);
int probe_commands(struct probe *probe);
void probe_output_add(struct probe_output *output, int32_t poc, const char *hash);
void probe_output_flush(struct probe_output *output, uint32_t keep);
int probe_failed(VkResult result, const char *what);
uint64_t probe_now(void);
void probe_timing_print(const struct probe_timing *timing);
int probe_h265_run(struct probe *probe, const struct probe_options *options, uint8_t *data, size_t size);

#endif
