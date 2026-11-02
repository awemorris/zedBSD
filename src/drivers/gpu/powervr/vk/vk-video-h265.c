/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 records of the executor's video wire (see vk-video-h265.h).
 *
 * The wire is libvulkan's (userland/desktop/libvulkan/video.c): every
 * field of the StdVideoH265 structures in declaration order, each number
 * widened to a word (signed ones with their sign), the one-bit flags of a
 * structure packed into one word by name from bit 0, the HRD's 8-bit flags
 * a word each, a fixed array as its 64-bit count and its elements (bytes
 * padded to four, wider numbers a word each), a pointer as a presence word
 * and what it points to.  The fields the engine writes back into the
 * parameter sets are kept; the timing, HRD, VUI and palette parts are read
 * and dropped (they do not change the decoding).  A count that disagrees
 * with the structure fails the stream.
 *
 * Procedure referenced: zedBSD userland/desktop/libvulkan/video.c
 * (video_encode_h265_*: the order and the packing).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/vk/vk-video-h265.h"

/* The structures' fixed array lengths (vulkan_video_codec_h265std.h). */
#define WIRE_SUB_LAYERS			7U
#define WIRE_CPB			32U
#define WIRE_SCALING_4X4		96U
#define WIRE_SCALING_8X8		384U
#define WIRE_SCALING_32X32		128U
#define WIRE_SCALING_DC16		6U
#define WIRE_SCALING_DC32		2U
#define WIRE_PALETTE			384U
#define WIRE_ST_ENTRIES			16U
#define WIRE_LT_PICTURES		32U
#define WIRE_QP_LIST			6U
#define WIRE_TILE_COLUMNS		19U
#define WIRE_TILE_ROWS			21U
#define WIRE_HRD_DURATIONS		7U
#define WIRE_HRD_RESERVED		3U
#define WIRE_REFERENCE_LIST		8U

/* The fields read and dropped: the VPS's after its flags and three numbers, the VUI's after its flags, the PPS's. */
#define WIRE_VPS_TIMING			5U
#define WIRE_HRD_NUMBERS		9U
#define WIRE_VUI_NUMBERS		24U
#define WIRE_SPS_RESERVED		6U
#define WIRE_PPS_SCC			6U

static void read_skip(struct i915_wire_reader *reader, uint32_t words);
static void read_count(struct i915_wire_reader *reader, uint64_t expected);
static void read_bytes(struct i915_wire_reader *reader, void *bytes, uint32_t count);
static void read_halves(struct i915_wire_reader *reader, uint16_t *halves, uint32_t count);
static void read_words(struct i915_wire_reader *reader, uint32_t *words, uint32_t count);
static void read_ptl(struct i915_wire_reader *reader, struct vpu_h265_ptl *ptl);
static void read_ordering(struct i915_wire_reader *reader, struct vpu_h265_ordering *ordering);
static void read_hrd(struct i915_wire_reader *reader, uint32_t sub_layers);
static void read_sub_layers(struct i915_wire_reader *reader, uint32_t sub_layers);
static void read_scaling(struct i915_wire_reader *reader, struct vpu_h265_scaling *scaling);
static void read_vui(struct i915_wire_reader *reader, uint32_t sub_layers);
static void read_short_term(struct i915_wire_reader *reader, struct vpu_h265_st_rps *rps);
static void read_vps(struct i915_wire_reader *reader, struct vpu_h265_vps *vps);
static void read_sps(struct i915_wire_reader *reader, struct vpu_h265_sps *sps);
static void read_pps(struct i915_wire_reader *reader, struct vpu_h265_pps *pps);
static uint32_t sps_key(uint32_t vps_id, uint32_t sps_id);
static struct video_h265_pps_node *find_pps(const struct video_h265_sets *sets, const struct vpu_h265_pps *key);

/* Reads an H.265 add record's VPSs, SPSs and PPSs. */
void
powervr_vk_video_h265_read_add(
	struct i915_wire_reader *reader,
	struct video_h265_added *added)
{
	struct vpu_h265_vps *vps;
	struct vpu_h265_sps *sps;
	struct video_h265_pps_node *pps;
	uint64_t count;
	uint64_t item;

	/* The video parameter sets: one past the ids, or one that cannot be kept, is read into a scratch and dropped. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0; item < count && reader->error == 0; item++) {
		vps = kern_calloc(1, sizeof(*vps));
		if (vps == NULL) {
			reader->error = 1;
			added->error = ENOMEM;
			break;
		}
		read_vps(reader, vps);
		if (vps->id >= VIDEO_H265_VPS_IDS || added->vps_count >= VIDEO_H265_VPS_IDS) {
			added->error = EINVAL;
			kern_free(vps);
			continue;
		}
		added->vps[added->vps_count] = vps;
		added->vps_count++;
	}

	/* The sequence parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0; item < count && reader->error == 0; item++) {
		sps = kern_calloc(1, sizeof(*sps));
		if (sps == NULL) {
			reader->error = 1;
			added->error = ENOMEM;
			break;
		}
		read_sps(reader, sps);
		if (sps->vps_id >= VIDEO_H265_VPS_IDS || sps->id >= VIDEO_H265_SPS_IDS ||
		    added->sps_count >= VIDEO_H265_VPS_IDS * VIDEO_H265_SPS_IDS) {
			added->error = EINVAL;
			kern_free(sps);
			continue;
		}
		added->sps[added->sps_count] = sps;
		added->sps_count++;
	}

	/* The picture parameter sets. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0; item < count && reader->error == 0; item++) {
		pps = kern_calloc(1, sizeof(*pps));
		if (pps == NULL) {
			reader->error = 1;
			added->error = ENOMEM;
			break;
		}
		read_pps(reader, &pps->pps);
		if (pps->pps.vps_id >= VIDEO_H265_VPS_IDS || pps->pps.sps_id >= VIDEO_H265_SPS_IDS || pps->pps.id >= VIDEO_H265_PPS_IDS) {
			added->error = EINVAL;
			kern_free(pps);
			continue;
		}
		pps->next = added->pps;
		added->pps = pps;
	}
}

/*
 * Reads an H.265 picture record after its chain: [present]{flags, the
 * three ids, NumDeltaPocsOfRefRpsIdx, PicOrderCntVal,
 * NumBitsForSTRefPicSetInSlice, reserved, the three reference lists}
 * [count][array]{offset}.  The engine reads the slice headers itself, so
 * only the flags, the ids and the order count are kept.
 */
void
powervr_vk_video_h265_read_picture(
	struct i915_wire_reader *reader,
	bool *has_picture,
	struct video_h265_picture *picture)
{
	uint8_t lists[WIRE_REFERENCE_LIST];
	uint64_t present;
	uint64_t count;
	uint64_t item;
	uint32_t list;

	/* The picture information. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		*has_picture = true;
		picture->flags = drv_i915_wire_read_u32(reader);
		picture->vps_id = drv_i915_wire_read_u32(reader);
		picture->sps_id = drv_i915_wire_read_u32(reader);
		picture->pps_id = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		picture->poc = (int32_t)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		for (list = 0; list < 3U; list++)
			read_bytes(reader, lists, WIRE_REFERENCE_LIST);
	}

	/* The slice segments' offsets, which the engine finds itself by their start codes. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (count > (uint64_t)(reader->size - reader->offset))
		reader->error = 1;
	for (item = 0; item < count && reader->error == 0; item++)
		(void)drv_i915_wire_read_u32(reader);
}

/* Reads an H.265 DPB slot record: [present]{flags, PicOrderCntVal}. */
void
powervr_vk_video_h265_read_slot(
	struct i915_wire_reader *reader)
{
	uint64_t present;

	/* The reference information, which the engine does not need. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_skip(reader, 2U);
}

/* Frees what an add record carried. */
void
powervr_vk_video_h265_added_free(
	struct video_h265_added *added)
{
	struct video_h265_pps_node *pps;
	uint32_t index;

	/* The video and sequence sets. */
	for (index = 0; index < added->vps_count; index++) {
		kern_free(added->vps[index]);
		added->vps[index] = NULL;
	}
	added->vps_count = 0;
	for (index = 0; index < added->sps_count; index++) {
		kern_free(added->sps[index]);
		added->sps[index] = NULL;
	}
	added->sps_count = 0;

	/* The picture sets. */
	while (added->pps != NULL) {
		pps = added->pps;
		added->pps = pps->next;
		kern_free(pps);
	}
}

/* Moves an add record's sets into a parameters object's. */
int
powervr_vk_video_h265_take(
	struct video_h265_sets *sets,
	struct video_h265_added *added,
	bool replace,
	uint32_t max_vps,
	uint32_t max_sps,
	uint32_t max_pps)
{
	struct video_h265_pps_node *pps;
	struct video_h265_pps_node *old;
	struct video_h265_pps_node **link;
	uint32_t vps_count;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t index;
	uint32_t key;

	/* An update must not repeat a key the object holds. */
	if (!replace) {
		for (index = 0; index < added->vps_count; index++) {
			if (sets->vps[added->vps[index]->id] != NULL)
				return EINVAL;
		}
		for (index = 0; index < added->sps_count; index++) {
			if (sets->sps[sps_key(added->sps[index]->vps_id, added->sps[index]->id)] != NULL)
				return EINVAL;
		}
		for (pps = added->pps; pps != NULL; pps = pps->next) {
			if (find_pps(sets, &pps->pps) != NULL)
				return EINVAL;
		}
	}

	/* The sets held, and the new ones of keys not held yet, within the capacities. */
	vps_count = 0;
	for (index = 0; index < VIDEO_H265_VPS_IDS; index++) {
		if (sets->vps[index] != NULL)
			vps_count++;
	}
	for (index = 0; index < added->vps_count; index++) {
		if (sets->vps[added->vps[index]->id] == NULL)
			vps_count++;
	}
	sps_count = 0;
	for (index = 0; index < VIDEO_H265_VPS_IDS * VIDEO_H265_SPS_IDS; index++) {
		if (sets->sps[index] != NULL)
			sps_count++;
	}
	for (index = 0; index < added->sps_count; index++) {
		if (sets->sps[sps_key(added->sps[index]->vps_id, added->sps[index]->id)] == NULL)
			sps_count++;
	}
	pps_count = 0;
	for (pps = sets->pps; pps != NULL; pps = pps->next)
		pps_count++;
	for (pps = added->pps; pps != NULL; pps = pps->next) {
		if (find_pps(sets, &pps->pps) == NULL)
			pps_count++;
	}
	if (vps_count > max_vps || sps_count > max_sps || pps_count > max_pps)
		return EINVAL;

	/* The video and sequence sets, each in its key's place (a repeated key in one add: the last wins). */
	for (index = 0; index < added->vps_count; index++) {
		key = added->vps[index]->id;
		kern_free(sets->vps[key]);
		sets->vps[key] = added->vps[index];
		added->vps[index] = NULL;
	}
	added->vps_count = 0;
	for (index = 0; index < added->sps_count; index++) {
		key = sps_key(added->sps[index]->vps_id, added->sps[index]->id);
		kern_free(sets->sps[key]);
		sets->sps[key] = added->sps[index];
		added->sps[index] = NULL;
	}
	added->sps_count = 0;

	/* The picture sets replace any of the same key. */
	while (added->pps != NULL) {
		pps = added->pps;
		added->pps = pps->next;
		link = &sets->pps;
		while (*link != NULL) {
			if ((*link)->pps.vps_id == pps->pps.vps_id && (*link)->pps.sps_id == pps->pps.sps_id && (*link)->pps.id == pps->pps.id) {
				old = *link;
				*link = old->next;
				kern_free(old);
				continue;
			}

			/* Moves on to the next picture set. */
			link = &(*link)->next;
		}
		pps->next = sets->pps;
		sets->pps = pps;
	}

	/* Succeeded: the sets are the object's. */
	return 0;
}

/* Copies a template's sets. */
int
powervr_vk_video_h265_copy(
	struct video_h265_sets *destination,
	const struct video_h265_sets *source)
{
	const struct video_h265_pps_node *pps;
	struct video_h265_pps_node *copy;
	uint32_t index;

	/* The video and sequence sets. */
	for (index = 0; index < VIDEO_H265_VPS_IDS; index++) {
		if (source->vps[index] == NULL)
			continue;
		destination->vps[index] = kern_calloc(1, sizeof(*destination->vps[index]));
		if (destination->vps[index] == NULL)
			return ENOMEM;
		*destination->vps[index] = *source->vps[index];
	}
	for (index = 0; index < VIDEO_H265_VPS_IDS * VIDEO_H265_SPS_IDS; index++) {
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
void
powervr_vk_video_h265_free(
	struct video_h265_sets *sets)
{
	struct video_h265_pps_node *pps;
	uint32_t index;

	/* The video and sequence sets. */
	for (index = 0; index < VIDEO_H265_VPS_IDS; index++) {
		kern_free(sets->vps[index]);
		sets->vps[index] = NULL;
	}
	for (index = 0; index < VIDEO_H265_VPS_IDS * VIDEO_H265_SPS_IDS; index++) {
		kern_free(sets->sps[index]);
		sets->sps[index] = NULL;
	}

	/* The picture sets. */
	while (sets->pps != NULL) {
		pps = sets->pps;
		sets->pps = pps->next;
		kern_free(pps);
	}
}

/* Finds a picture's sets by its keys. */
bool
powervr_vk_video_h265_find(
	const struct video_h265_sets *sets,
	const struct video_h265_picture *picture,
	const struct vpu_h265_vps **vps,
	const struct vpu_h265_sps **sps,
	const struct vpu_h265_pps **pps)
{
	struct vpu_h265_pps key;
	const struct video_h265_pps_node *node;

	/* Ids outside the ranges name nothing. */
	if (picture->vps_id >= VIDEO_H265_VPS_IDS || picture->sps_id >= VIDEO_H265_SPS_IDS || picture->pps_id >= VIDEO_H265_PPS_IDS)
		return false;

	/* The three sets. */
	kern_memset(&key, 0, sizeof(key));
	key.vps_id = picture->vps_id;
	key.sps_id = picture->sps_id;
	key.id = picture->pps_id;
	node = find_pps(sets, &key);
	*vps = sets->vps[picture->vps_id];
	*sps = sets->sps[sps_key(picture->vps_id, picture->sps_id)];
	if (node == NULL || *vps == NULL || *sps == NULL)
		return false;

	/* Succeeded: every set is held. */
	*pps = &node->pps;
	return true;
}

/* Reads and drops words. */
static void
read_skip(
	struct i915_wire_reader *reader,
	uint32_t words)
{
	uint32_t index;

	/* Each word. */
	for (index = 0; index < words && reader->error == 0; index++)
		(void)drv_i915_wire_read_u32(reader);
}

/* Reads a fixed array's count: one other than the structure's fails the stream. */
static void
read_count(
	struct i915_wire_reader *reader,
	uint64_t expected)
{
	uint64_t count;

	/* The count. */
	count = drv_i915_wire_read_u64(reader);
	if (count != expected)
		reader->error = 1;
}

/* Reads a fixed byte array: its count and its bytes, padded to four. */
static void
read_bytes(
	struct i915_wire_reader *reader,
	void *bytes,
	uint32_t count)
{
	/* The count, then the bytes. */
	read_count(reader, count);
	i915_vkc_read_bytes(reader, bytes, count);
}

/* Reads a fixed array of 16-bit numbers, a word each. */
static void
read_halves(
	struct i915_wire_reader *reader,
	uint16_t *halves,
	uint32_t count)
{
	uint32_t index;

	/* The count, then each number (NULL drops them). */
	read_count(reader, count);
	for (index = 0; index < count && reader->error == 0; index++) {
		if (halves != NULL)
			halves[index] = (uint16_t)drv_i915_wire_read_u32(reader);
		else
			(void)drv_i915_wire_read_u32(reader);
	}
}

/* Reads a fixed array of 32-bit numbers. */
static void
read_words(
	struct i915_wire_reader *reader,
	uint32_t *words,
	uint32_t count)
{
	uint32_t index;

	/* The count, then each number (NULL drops them). */
	read_count(reader, count);
	for (index = 0; index < count && reader->error == 0; index++) {
		if (words != NULL)
			words[index] = drv_i915_wire_read_u32(reader);
		else
			(void)drv_i915_wire_read_u32(reader);
	}
}

/* Reads a profile, tier and level: [flags][profile][level]. */
static void
read_ptl(
	struct i915_wire_reader *reader,
	struct vpu_h265_ptl *ptl)
{
	/* The three words. */
	ptl->flags = drv_i915_wire_read_u32(reader);
	ptl->profile_idc = drv_i915_wire_read_u32(reader);
	ptl->level = drv_i915_wire_read_u32(reader);
}

/* Reads the sub-layers' ordering information: the latencies, the buffering, the reordering. */
static void
read_ordering(
	struct i915_wire_reader *reader,
	struct vpu_h265_ordering *ordering)
{
	/* The three arrays of seven. */
	read_words(reader, ordering->latency_plus1, WIRE_SUB_LAYERS);
	read_bytes(reader, ordering->buffering_minus1, WIRE_SUB_LAYERS);
	read_bytes(reader, ordering->reorder, WIRE_SUB_LAYERS);
}

/* Reads and drops HRD parameters with their sub-layers' arrays. */
static void
read_hrd(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint8_t counts[WIRE_SUB_LAYERS];
	uint64_t present;

	/* The flag words, the numbers, the CPB counts, the durations and the reserved ones. */
	read_skip(reader, 4U);
	read_skip(reader, WIRE_HRD_NUMBERS);
	read_bytes(reader, counts, WIRE_SUB_LAYERS);
	read_halves(reader, NULL, WIRE_HRD_DURATIONS);
	read_halves(reader, NULL, WIRE_HRD_RESERVED);

	/* The NAL and VCL sub-layer arrays. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_sub_layers(reader, sub_layers);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_sub_layers(reader, sub_layers);
}

/* Reads and drops a sub-layer HRD array of the sub-layers' count. */
static void
read_sub_layers(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint32_t index;
	uint32_t array;

	/* [count] then each: four arrays of 32 and the CBR flags. */
	read_count(reader, sub_layers);
	for (index = 0; index < sub_layers && reader->error == 0; index++) {
		for (array = 0; array < 4U; array++)
			read_words(reader, NULL, WIRE_CPB);
		(void)drv_i915_wire_read_u32(reader);
	}
}

/* Reads scaling lists: the 4x4, 8x8, 16x16 and 32x32 lists, then the DC coefficients. */
static void
read_scaling(
	struct i915_wire_reader *reader,
	struct vpu_h265_scaling *scaling)
{
	/* Each array as bytes. */
	read_bytes(reader, scaling->list4, WIRE_SCALING_4X4);
	read_bytes(reader, scaling->list8, WIRE_SCALING_8X8);
	read_bytes(reader, scaling->list16, WIRE_SCALING_8X8);
	read_bytes(reader, scaling->list32, WIRE_SCALING_32X32);
	read_bytes(reader, scaling->dc16, WIRE_SCALING_DC16);
	read_bytes(reader, scaling->dc32, WIRE_SCALING_DC32);
}

/* Reads and drops a VUI with its HRD parameters. */
static void
read_vui(
	struct i915_wire_reader *reader,
	uint32_t sub_layers)
{
	uint64_t present;

	/* The flags, the numbers, then the HRD parameters. */
	read_skip(reader, 1U + WIRE_VUI_NUMBERS);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_hrd(reader, sub_layers);
}

/*
 * Reads a short-term set, keeping its explicit form: [flags][delta_idx]
 * [use_delta][abs_delta][used][used s0][used s1][reserved x3][negative]
 * [positive][16]{delta s0}[16]{delta s1}.
 */
static void
read_short_term(
	struct i915_wire_reader *reader,
	struct vpu_h265_st_rps *rps)
{
	/* The prediction syntax and the used masks of the explicit form. */
	read_skip(reader, 5U);
	rps->used_s0 = drv_i915_wire_read_u32(reader);
	rps->used_s1 = drv_i915_wire_read_u32(reader);
	read_skip(reader, 3U);

	/* The counts and the differences. */
	rps->negative = drv_i915_wire_read_u32(reader);
	rps->positive = drv_i915_wire_read_u32(reader);
	read_halves(reader, rps->delta_s0_minus1, WIRE_ST_ENTRIES);
	read_halves(reader, rps->delta_s1_minus1, WIRE_ST_ENTRIES);
}

/* Reads a VPS: its flags, id and sub-layers, the dropped timing, then its ordering, HRD and profile. */
static void
read_vps(
	struct i915_wire_reader *reader,
	struct vpu_h265_vps *vps)
{
	uint64_t present;

	/* The flags, the id, the sub-layers; the reserved words and the timing. */
	vps->flags = drv_i915_wire_read_u32(reader);
	vps->id = drv_i915_wire_read_u32(reader);
	vps->max_sub_layers_minus1 = drv_i915_wire_read_u32(reader);
	read_skip(reader, WIRE_VPS_TIMING + 1U);
	if (vps->max_sub_layers_minus1 >= WIRE_SUB_LAYERS)
		reader->error = 1;

	/* The ordering information, the HRD parameters (dropped) and the profile. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		vps->has_ordering = 1;
		read_ordering(reader, &vps->ordering);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U && reader->error == 0)
		read_hrd(reader, vps->max_sub_layers_minus1 + 1U);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		vps->has_ptl = 1;
		read_ptl(reader, &vps->ptl);
	}
}

/*
 * Reads an SPS: its 32 words of fields (the palette ones dropped), then
 * the profile, the ordering, the scaling lists, the short-term sets, the
 * long-term pictures, the VUI (dropped) and the palette (dropped).
 */
static void
read_sps(
	struct i915_wire_reader *reader,
	struct vpu_h265_sps *sps)
{
	uint64_t present;
	uint64_t count;
	uint32_t index;

	/* The flags, the format, the size, the ids and the bit depths. */
	sps->flags = drv_i915_wire_read_u32(reader);
	sps->chroma_format_idc = drv_i915_wire_read_u32(reader);
	sps->width = drv_i915_wire_read_u32(reader);
	sps->height = drv_i915_wire_read_u32(reader);
	sps->vps_id = drv_i915_wire_read_u32(reader);
	sps->max_sub_layers_minus1 = drv_i915_wire_read_u32(reader);
	sps->id = drv_i915_wire_read_u32(reader);
	sps->bit_depth_luma_minus8 = drv_i915_wire_read_u32(reader);
	sps->bit_depth_chroma_minus8 = drv_i915_wire_read_u32(reader);

	/* The order count's bits, the block sizes, the depths, the set counts. */
	sps->log2_max_poc_lsb_minus4 = drv_i915_wire_read_u32(reader);
	sps->log2_min_cb_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_cb = drv_i915_wire_read_u32(reader);
	sps->log2_min_tb_minus2 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_tb = drv_i915_wire_read_u32(reader);
	sps->depth_inter = drv_i915_wire_read_u32(reader);
	sps->depth_intra = drv_i915_wire_read_u32(reader);
	sps->st_count = drv_i915_wire_read_u32(reader);
	sps->lt_count = drv_i915_wire_read_u32(reader);

	/* The PCM fields; the reserved and palette words; the conformance window. */
	sps->pcm_luma_minus1 = drv_i915_wire_read_u32(reader);
	sps->pcm_chroma_minus1 = drv_i915_wire_read_u32(reader);
	sps->log2_min_pcm_minus3 = drv_i915_wire_read_u32(reader);
	sps->log2_diff_pcm = drv_i915_wire_read_u32(reader);
	read_skip(reader, WIRE_SPS_RESERVED);
	for (index = 0; index < 4U; index++)
		sps->window[index] = drv_i915_wire_read_u32(reader);
	if (sps->max_sub_layers_minus1 >= WIRE_SUB_LAYERS)
		reader->error = 1;

	/* The profile, the ordering, the scaling lists. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_ptl = 1;
		read_ptl(reader, &sps->ptl);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_ordering = 1;
		read_ordering(reader, &sps->ordering);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->has_scaling = 1;
		read_scaling(reader, &sps->scaling);
	}

	/* The short-term sets: as many as the SPS counts, at most 64. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		count = drv_i915_wire_read_u64(reader);
		if (count != sps->st_count || count > VPU_H265_ST_SETS)
			reader->error = 1;
		for (index = 0; index < (uint32_t)count && reader->error == 0; index++)
			read_short_term(reader, &sps->st[index]);
	} else if (sps->st_count != 0U) {
		reader->error = 1;
	}

	/* The long-term pictures. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		sps->lt_used = drv_i915_wire_read_u32(reader);
		read_words(reader, sps->lt_lsb, WIRE_LT_PICTURES);
	}

	/* The VUI and the palette, dropped. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U && reader->error == 0)
		read_vui(reader, sps->max_sub_layers_minus1 + 1U);
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_halves(reader, NULL, WIRE_PALETTE);
}

/*
 * Reads a PPS: its words of fields (the screen content ones dropped), the
 * chroma offset lists, the tiles, then the scaling lists and the palette
 * (dropped).
 */
static void
read_pps(
	struct i915_wire_reader *reader,
	struct vpu_h265_pps *pps)
{
	uint64_t present;

	/* The flags, the ids, the slice fields and the quantizers, each widened with its sign. */
	pps->flags = drv_i915_wire_read_u32(reader);
	pps->id = drv_i915_wire_read_u32(reader);
	pps->sps_id = drv_i915_wire_read_u32(reader);
	pps->vps_id = drv_i915_wire_read_u32(reader);
	pps->extra_bits = drv_i915_wire_read_u32(reader);
	pps->l0_minus1 = drv_i915_wire_read_u32(reader);
	pps->l1_minus1 = drv_i915_wire_read_u32(reader);
	pps->init_qp_minus26 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->qp_depth = drv_i915_wire_read_u32(reader);
	pps->cb_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->cr_offset = (int32_t)drv_i915_wire_read_u32(reader);
	pps->beta_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->tc_div2 = (int32_t)drv_i915_wire_read_u32(reader);
	pps->merge_level_minus2 = drv_i915_wire_read_u32(reader);

	/* The range extension's fields. */
	pps->skip_size_minus2 = drv_i915_wire_read_u32(reader);
	pps->chroma_qp_depth = drv_i915_wire_read_u32(reader);
	pps->chroma_qp_list_minus1 = drv_i915_wire_read_u32(reader);
	read_bytes(reader, pps->cb_list, WIRE_QP_LIST);
	read_bytes(reader, pps->cr_list, WIRE_QP_LIST);
	pps->sao_luma = drv_i915_wire_read_u32(reader);
	pps->sao_chroma = drv_i915_wire_read_u32(reader);

	/* The screen content fields, dropped; the tiles; the reserved word. */
	read_skip(reader, WIRE_PPS_SCC);
	pps->tile_columns_minus1 = drv_i915_wire_read_u32(reader);
	pps->tile_rows_minus1 = drv_i915_wire_read_u32(reader);
	read_skip(reader, 2U);
	read_halves(reader, pps->column_minus1, WIRE_TILE_COLUMNS);
	read_halves(reader, pps->row_minus1, WIRE_TILE_ROWS);
	(void)drv_i915_wire_read_u32(reader);

	/* The scaling lists, and the palette, dropped. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		pps->has_scaling = 1;
		read_scaling(reader, &pps->scaling);
	}
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		read_halves(reader, NULL, WIRE_PALETTE);
}

/* The place of an SPS key among the sets'. */
static uint32_t
sps_key(
	uint32_t vps_id,
	uint32_t sps_id)
{
	/* The VPS's row, the SPS's column. */
	return vps_id * VIDEO_H265_SPS_IDS + sps_id;
}

/* Finds the picture set of a PPS's key (its VPS, SPS and own ids). */
static struct video_h265_pps_node *
find_pps(
	const struct video_h265_sets *sets,
	const struct vpu_h265_pps *key)
{
	struct video_h265_pps_node *pps;

	/* Walks the list for the key. */
	for (pps = sets->pps; pps != NULL; pps = pps->next) {
		if (pps->pps.vps_id == key->vps_id && pps->pps.sps_id == key->sps_id && pps->pps.id == key->id)
			return pps;
	}

	/* No set of the key. */
	return NULL;
}
