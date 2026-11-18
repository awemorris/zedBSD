/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 part of the executor's Vulkan Video decode (vk-video.c): the
 * wire's H.265 records read into the engine's forms (vpu-codec.h), and the
 * video, sequence and picture parameter sets a parameters object holds by
 * their keys (VPS id; VPS and SPS ids; VPS, SPS and PPS ids).
 */

#ifndef DRIVERS_GPU_POWERVR_VK_VK_VIDEO_H265_H
#define DRIVERS_GPU_POWERVR_VK_VK_VIDEO_H265_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/i915/vk/vk-codec.h"
#include "drivers/vpu/vpu-codec.h"

/* The id ranges of the three kinds of set. */
#define VIDEO_H265_VPS_IDS		16U
#define VIDEO_H265_SPS_IDS		16U
#define VIDEO_H265_PPS_IDS		64U

/* One picture parameter set a parameters object or an add record holds; the sets are a list. */
struct video_h265_pps_node {
	struct video_h265_pps_node *next;
	struct vpu_h265_pps pps;
};

/*
 * The H.265 sets of a parameters object: the VPSs by id, the SPSs by
 * their VPS's id and their own, the PPSs as a list (each knows its key).
 */
struct video_h265_sets {
	struct vpu_h265_vps *vps[VIDEO_H265_VPS_IDS];
	struct vpu_h265_sps *sps[VIDEO_H265_VPS_IDS * VIDEO_H265_SPS_IDS];
	struct video_h265_pps_node *pps;
};

/*
 * The sets one add record carried, each a new allocation until a
 * parameters object takes it: error is EINVAL for a set with an id out of
 * range, ENOMEM for one that could not be kept (both read in full).
 */
struct video_h265_added {
	struct vpu_h265_vps *vps[VIDEO_H265_VPS_IDS];
	uint32_t vps_count;
	struct vpu_h265_sps *sps[VIDEO_H265_VPS_IDS * VIDEO_H265_SPS_IDS];
	uint32_t sps_count;
	struct video_h265_pps_node *pps;
	int error;
};

/* The picture flags (StdVideoDecodeH265PictureInfoFlags in declaration order). */
#define VIDEO_H265_PICTURE_IRAP		(1U << 0)
#define VIDEO_H265_PICTURE_IDR		(1U << 1)

/* The H.265 information of the picture a decode writes: its flags, the sets it names, PicOrderCntVal. */
struct video_h265_picture {
	uint32_t flags;
	uint32_t vps_id;
	uint32_t sps_id;
	uint32_t pps_id;
	int32_t poc;
};

/*
 * Reads an H.265 add record's fields ([count][array]{VPS}, then the SPSs,
 * then the PPSs, libvulkan's video_encode_h265_parameters_add) into added.
 */
void powervr_vk_video_h265_read_add(struct i915_wire_reader *reader, struct video_h265_added *added);

/*
 * Reads an H.265 picture record's fields after its chain: [present]{the
 * picture information}[count][array]{slice segment offset}.  has_picture
 * says whether the information was there.
 */
void powervr_vk_video_h265_read_picture(struct i915_wire_reader *reader, bool *has_picture, struct video_h265_picture *picture);

/* Reads an H.265 DPB slot record's fields: [present]{flags, PicOrderCntVal}; the engine keeps its references itself. */
void powervr_vk_video_h265_read_slot(struct i915_wire_reader *reader);

/* Frees the sets an add record carried and nobody took. */
void powervr_vk_video_h265_added_free(struct video_h265_added *added);

/*
 * Moves an add record's sets into a parameters object's.  With replace, a
 * set replaces the object's of the same key (a create); without, a key
 * already held refuses the whole add (an update, EINVAL) and nothing is
 * taken.  The capacities bound the sets of each kind (EINVAL).
 */
int powervr_vk_video_h265_take(struct video_h265_sets *sets, struct video_h265_added *added, bool replace, uint32_t max_vps, uint32_t max_sps, uint32_t max_pps);

/* Copies a template's sets into a new parameters object's (ENOMEM). */
int powervr_vk_video_h265_copy(struct video_h265_sets *destination, const struct video_h265_sets *source);

/* Frees a parameters object's sets. */
void powervr_vk_video_h265_free(struct video_h265_sets *sets);

/*
 * Finds the sets a picture names: its PPS by the full key, the SPS and
 * the VPS it belongs to.  Reports false when any of them is not held.
 */
bool powervr_vk_video_h265_find(const struct video_h265_sets *sets, const struct video_h265_picture *picture,
				const struct vpu_h265_vps **vps, const struct vpu_h265_sps **sps, const struct vpu_h265_pps **pps);

#endif
