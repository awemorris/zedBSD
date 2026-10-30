/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The direct prediction of B slices of the software H.264 decoder
 * (8.4.1.2).
 */

#include "h264sw-direct.h"
#include "h264sw-context.h"
#include "h264sw-mvpred.h"

#include <errno.h>
#include <stddef.h>

/* The width and the height of a macroblock in luma samples (maxW and maxH of 6.4.12). */
#define DIRECT_MB_SIZE 16U

/* The range of a motion vector component as it is stored (int16_t). */
#define DIRECT_MV_MIN (-32768)
#define DIRECT_MV_MAX 32767

/*
 * The luma4x4BlkIdx of the co-located macroblock whose vector stands for
 * a whole 8x8 quadrant when direct_8x8_inference_flag is 1, by mbPartIdx.
 *
 * Values referenced: ITU-T H.264 (08/2021) 8.4.1.2.1 (luma4x4BlkIdx is
 * set equal to 5 * mbPartIdx when direct_8x8_inference_flag is equal to
 * 1: the corner 4x4 blocks 0, 5, 10 and 15 of the scan of 6.4.3).
 */
static const uint8_t direct_corner_blk[4] = { 0, 5, 10, 15 };

/*
 * What the co-located 4x4 block says (8.4.1.2.1).
 *
 * mv is mvCol ([0] horizontal, [1] vertical, quarter luma samples) and
 * ref_idx is refIdxCol, -1 for an intra macroblock or a block that uses
 * neither list; ref_id is the id of the frame that reference index
 * named when the co-located macroblock was decoded (0 when none).  One
 * instance lives in the stack frame of the function that derives a
 * quadrant.
 */
struct direct_col {
	int mv[2];
	int ref_idx;
	uint32_t ref_id;
};

/* Forward declarations of the static functions. */
static int64_t direct_floor_shift(int64_t value, unsigned shift);
static int direct_clip(int value, int low, int high);
static int16_t direct_clamp_mv(int value);
static int direct_min_positive(int first, int second);
static int direct_colocated(const struct h264sw_slice_ctx *ctx, unsigned blk, struct direct_col *colocated);
static int direct_neighbour_ref(const struct h264sw_slice_ctx *ctx, unsigned list, int xn, int yn, int *available);
static void direct_spatial_prepare(struct h264sw_slice_ctx *ctx);
static int direct_ref_id(const struct h264sw_slice_ctx *ctx, unsigned list, int ref_idx, uint32_t *ref_id);
static int direct_quadrant_spatial(struct h264sw_slice_ctx *ctx, unsigned quadrant, int inference);
static int direct_quadrant_temporal(struct h264sw_slice_ctx *ctx, unsigned quadrant, int inference);

/*
 * Prepares the direct prediction of a B slice.
 *
 * A missing direct structure is ignored.  Missing lists, or a list 1
 * without an entry, leave col NULL, so that every quadrant of the slice
 * is refused.
 */
void
h264sw_direct_setup(
	struct h264sw_direct *direct,
	const struct h264sw_reflist *lists,
	int32_t poc)
{
	unsigned count0;
	unsigned i;
	int64_t diff_tb;
	int64_t diff_td;
	int tb;
	int td;
	int half_td;
	int tx;
	int64_t scaled;

	/* Ignores a missing output. */
	if (direct == NULL)
		return;

	/* Starts from a state in which nothing is prepared and every list 0 entry takes the vector of the co-located block unscaled. */
	direct->col = NULL;
	direct->col_long_term = 0;
	for (i = 0; i < H264SW_MAX_LIST; i++) {
		direct->dist_scale[i] = 0;
		direct->long_term0[i] = 1;
	}
	direct->spatial_ready = 0;
	direct->spatial_ref[0] = -1;
	direct->spatial_ref[1] = -1;
	direct->spatial_mv[0][0] = 0;
	direct->spatial_mv[0][1] = 0;
	direct->spatial_mv[1][0] = 0;
	direct->spatial_mv[1][1] = 0;
	direct->direct_zero = 0;

	/* Leaves the state unprepared when there are no lists or list 1 is empty (it never is in a B slice). */
	if (lists == NULL)
		return;
	if (lists->count[1] == 0U)
		return;

	/* Takes the first entry of list 1 as colPic and remembers whether it is a long-term reference. */
	direct->col = lists->list[1][0].frame;
	direct->col_long_term = lists->list[1][0].long_term;

	/* Limits the entries of list 0 that are prepared to the size of the array. */
	count0 = lists->count[0];
	if (count0 > H264SW_MAX_LIST)
		count0 = H264SW_MAX_LIST;

	/* Derives DistScaleFactor for every entry of list 0 (8-197, 8-198, 8-201, 8-202). */
	for (i = 0; i < count0; i++) {
		/* Takes the differences of picture order counts in 64 bits so that no difference overflows. */
		diff_tb = (int64_t)poc - (int64_t)lists->list[0][i].poc;
		diff_td = (int64_t)lists->list[1][0].poc - (int64_t)lists->list[0][i].poc;

		/* Leaves mvL0 = mvCol and mvL1 = 0 for a long-term reference or a distance of 0 (8-195, 8-196). */
		if (lists->list[0][i].long_term != 0)
			continue;
		if (diff_td == 0)
			continue;

		/* Clips the two distances to -128..127 (8-201, 8-202). */
		if (diff_tb < -128)
			diff_tb = -128;
		if (diff_tb > 127)
			diff_tb = 127;
		if (diff_td < -128)
			diff_td = -128;
		if (diff_td > 127)
			diff_td = 127;
		tb = (int)diff_tb;
		td = (int)diff_td;

		/* Derives tx with the truncating division of the specification (8-197). */
		half_td = td / 2;
		if (half_td < 0)
			half_td = -half_td;
		tx = (16384 + half_td) / td;

		/* Derives DistScaleFactor, shifting a negative value as an arithmetic right shift (8-198). */
		scaled = direct_floor_shift((int64_t)tb * (int64_t)tx + 32, 6U);
		if (scaled < -1024)
			scaled = -1024;
		if (scaled > 1023)
			scaled = 1023;
		direct->dist_scale[i] = (int32_t)scaled;
		direct->long_term0[i] = 0;
	}
}

/*
 * Derives the motion of one 8x8 quadrant of the current macroblock in direct mode (8.4.1.2).
 *
 * A missing context, a context without a current macroblock, a slice
 * header, a sequence parameter set or a co-located frame, and a
 * quadrant above 3 give EINVAL; so does a reference index that the
 * slice's lists do not hold.  Nothing is written for a refused
 * quadrant.
 */
int
h264sw_direct_quadrant(
	struct h264sw_slice_ctx *ctx,
	unsigned quadrant)
{
	int inference;
	int error;

	/* Refuses a missing context and the parts of it that the derivation reads. */
	if (ctx == NULL)
		return EINVAL;
	if (ctx->picture == NULL)
		return EINVAL;
	if (ctx->picture->sps == NULL)
		return EINVAL;
	if (ctx->header == NULL)
		return EINVAL;
	if (ctx->n.current == NULL)
		return EINVAL;

	/* Refuses a quadrant that a macroblock does not have. */
	if (quadrant > 3U)
		return EINVAL;

	/* Refuses the derivation when the slice has no co-located frame or the frame holds no motion for this macroblock. */
	if (ctx->direct.col == NULL)
		return EINVAL;
	if (ctx->direct.col->motion == NULL)
		return EINVAL;
	if (ctx->mb_addr >= ctx->direct.col->size_mbs)
		return EINVAL;

	/* Refuses a slice whose lists lack the entry that every direct block refers to. */
	if (ctx->lists.count[0] == 0U)
		return EINVAL;
	if (ctx->lists.count[1] == 0U)
		return EINVAL;

	/* Takes direct_8x8_inference_flag, which chooses the co-located block of each 4x4 block. */
	inference = ctx->picture->sps->direct_8x8_inference;

	/* Derives the quadrant in the mode of the slice header (8.4.1.2). */
	if (ctx->header->direct_spatial != 0) {
		error = direct_quadrant_spatial(ctx, quadrant, inference);
	} else {
		error = direct_quadrant_temporal(ctx, quadrant, inference);
	}
	if (error != 0)
		return error;

	/* Marks the quadrant as predicted in direct mode, for the contexts of ref_idx and mvd. */
	ctx->n.current->direct |= (uint8_t)(1U << quadrant);

	/* Succeeded: the motion of the quadrant is in the current macroblock. */
	return 0;
}

/*
 * Shifts a value right as an arithmetic shift does, without relying on the behaviour of a right shift of a negative value.
 *
 * The result is the floor of value / 2^shift.
 */
static int64_t
direct_floor_shift(
	int64_t value,
	unsigned shift)
{
	/* Shifts a non-negative value directly. */
	if (value >= 0)
		return value >> shift;

	/* Shifts a negative value through its complement, which rounds toward negative infinity. */
	return -((-value - 1) >> shift) - 1;
}

/* Limits a value to the range from low to high (Clip3, 5-9). */
static int
direct_clip(
	int value,
	int low,
	int high)
{
	/* Raises a value below the range to its lower end. */
	if (value < low)
		return low;

	/* Lowers a value above the range to its upper end. */
	if (value > high)
		return high;

	/* The value is already inside the range. */
	return value;
}

/* Limits a vector component to the range of the stored type and gives it in that type. */
static int16_t
direct_clamp_mv(
	int value)
{
	int clamped;

	/* Keeps a derived component inside what a conforming stream stays within. */
	clamped = direct_clip(value, DIRECT_MV_MIN, DIRECT_MV_MAX);

	/* Succeeded: the component fits the stored type. */
	return (int16_t)clamped;
}

/* Gives MinPositive( x, y ) of equation 8-187: the smaller of two values that are zero or more, else the larger. */
static int
direct_min_positive(
	int first,
	int second)
{
	/* Takes the smaller value when both are zero or more. */
	if (first >= 0 && second >= 0) {
		if (first < second)
			return first;
		return second;
	}

	/* Takes the larger value otherwise, which is the one that is zero or more, or -1. */
	if (first > second)
		return first;
	return second;
}

/*
 * Derives mvCol and refIdxCol of the co-located 4x4 block (8.4.1.2.1).
 *
 * Both pictures are frames, so the co-located macroblock is the one at
 * the current address and the block is the one at the same position.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-6 (field_pic_flag 0,
 * RefPicList1[ 0 ] a decoded frame, mb_field_decoding_flag 0: colPic is
 * RefPicList1[ 0 ]) and Table 8-8 (PicCodingStruct( CurrPic ) FRM and
 * PicCodingStruct( colPic ) FRM: mbAddrCol is CurrMbAddr, yM is yCol,
 * vertMvScale is One_To_One).
 */
static int
direct_colocated(
	const struct h264sw_slice_ctx *ctx,
	unsigned blk,
	struct direct_col *colocated)
{
	const struct h264sw_motion *motion;
	unsigned quadrant;
	unsigned list;

	/* Starts with the answer for an intra macroblock: no vector and no reference. */
	colocated->mv[0] = 0;
	colocated->mv[1] = 0;
	colocated->ref_idx = -1;
	colocated->ref_id = 0;

	/* Refuses a block that a macroblock does not have. */
	if (blk >= 16U)
		return EINVAL;

	/* Takes the motion of the co-located macroblock, the one at the same address in colPic. */
	motion = &ctx->direct.col->motion[ctx->mb_addr];

	/* Gives mvCol 0 and refIdxCol -1 for a co-located macroblock coded in an intra mode. */
	if (motion->intra != 0U)
		return 0;

	/* Finds the 8x8 quadrant that holds the co-located block, which carries the reference index. */
	quadrant = blk / 4U;

	/* Takes list 0 when the co-located partition used it, else list 1 (8.4.1.2.1). */
	if (motion->ref_idx[0][quadrant] >= 0) {
		list = 0;
	} else if (motion->ref_idx[1][quadrant] >= 0) {
		list = 1;
	} else {
		/* A block that used neither list (not produced by a decoded macroblock) counts as having no reference. */
		return 0;
	}

	/* Takes the vector, the reference index and the frame the index named. */
	colocated->mv[0] = motion->mv[list][blk][0];
	colocated->mv[1] = motion->mv[list][blk][1];
	colocated->ref_idx = motion->ref_idx[list][quadrant];
	colocated->ref_id = motion->ref_id[list][quadrant];

	/* Succeeded: the co-located block's vector and reference. */
	return 0;
}

/*
 * Gives refIdxLXN of the neighbouring partition holding a luma location (8.4.1.3.2).
 *
 * The location is relative to the upper-left sample of the current
 * macroblock.  *available is 1 when the partition is available, whether
 * or not it holds a vector.  The reference index is -1 when it is not
 * available, is intra or does not use the list.
 */
static int
direct_neighbour_ref(
	const struct h264sw_slice_ctx *ctx,
	unsigned list,
	int xn,
	int yn,
	int *available)
{
	struct h264sw_mb *found;
	unsigned xw;
	unsigned yw;
	unsigned blk;

	/* Starts with the answer for a partition that is not available. */
	*available = 0;

	/* Finds the macroblock holding the location, as Table 6-3 says. */
	found = h264sw_mb_locate(&ctx->n, xn, yn, DIRECT_MB_SIZE, DIRECT_MB_SIZE, &xw, &yw);
	if (found == NULL)
		return -1;

	/* The partition is available from here on, whatever it holds. */
	*available = 1;

	/* Gives -1 for an intra macroblock. */
	if (found->motion.intra != 0U)
		return -1;

	/* Takes the reference index of the 8x8 quadrant that holds the location. */
	blk = h264sw_blk4x4_at(xw, yw);

	/* Succeeded: the reference index, -1 when the list is not used. */
	return found->motion.ref_idx[list][blk / 4U];
}

/*
 * Derives what the spatial mode takes from the neighbours of the whole macroblock (8.4.1.2.2).
 *
 * The neighbours A, B and C (D in place of C) are those of the
 * macroblock as one 16x16 partition, the same for every 4x4 block.  The
 * reference indices and the predicted vectors are kept in the direct
 * structure for the macroblock's other direct quadrants.
 */
static void
direct_spatial_prepare(
	struct h264sw_slice_ctx *ctx)
{
	unsigned list;
	int ref_a;
	int ref_b;
	int ref_c;
	int available;
	int ref[2];
	int16_t mvp[2];

	/* Derives refIdxL0 and refIdxL1 as MinPositive of the neighbours A, B and C (8-184, 8-185). */
	for (list = 0; list < 2U; list++) {
		/* Takes the reference index of the partition to the left (A). */
		ref_a = direct_neighbour_ref(ctx, list, -1, 0, &available);

		/* Takes the reference index of the partition above (B). */
		ref_b = direct_neighbour_ref(ctx, list, 0, -1, &available);

		/* Takes the reference index of the partition above and to the right (C), or above and to the left (D) when C is not available (8.4.1.3.2). */
		ref_c = direct_neighbour_ref(ctx, list, (int)DIRECT_MB_SIZE, -1, &available);
		if (!available)
			ref_c = direct_neighbour_ref(ctx, list, -1, -1, &available);

		/* Combines the three with MinPositive. */
		ref[list] = direct_min_positive(ref_a, direct_min_positive(ref_b, ref_c));
	}

	/* Uses reference index 0 with zero vectors when neither list has a neighbour reference (8-186, 8-188 to 8-190). */
	ctx->direct.direct_zero = 0;
	if (ref[0] < 0 && ref[1] < 0) {
		ref[0] = 0;
		ref[1] = 0;
		ctx->direct.direct_zero = 1;
	}

	/* Derives the predicted vector of each list that is used; an unused list or the zero prediction has the vector 0. */
	for (list = 0; list < 2U; list++) {
		ctx->direct.spatial_ref[list] = ref[list];
		ctx->direct.spatial_mv[list][0] = 0;
		ctx->direct.spatial_mv[list][1] = 0;
		if (ctx->direct.direct_zero != 0)
			continue;
		if (ref[list] < 0)
			continue;

		/* Predicts the vector of the macroblock as one 16x16 partition (8.4.1.3). */
		h264sw_mvpred(ctx, list, 0, 0, DIRECT_MB_SIZE, DIRECT_MB_SIZE, ref[list], mvp);
		ctx->direct.spatial_mv[list][0] = mvp[0];
		ctx->direct.spatial_mv[list][1] = mvp[1];
	}

	/* Records that the macroblock's other direct quadrants can reuse the result. */
	ctx->direct.spatial_ready = 1;
}

/*
 * Gives the id of the frame RefPicListX[ refIdx ] names.
 *
 * Returns EINVAL when the index is outside the list or the entry holds
 * no frame.  A negative index means the list is not used, and gives
 * id 0.
 */
static int
direct_ref_id(
	const struct h264sw_slice_ctx *ctx,
	unsigned list,
	int ref_idx,
	uint32_t *ref_id)
{
	const struct h264sw_ref *entry;

	/* Gives id 0 for a list that is not used. */
	*ref_id = 0;
	if (ref_idx < 0)
		return 0;

	/* Refuses an index that the slice's list does not hold. */
	if ((unsigned)ref_idx >= ctx->lists.count[list])
		return EINVAL;
	if ((unsigned)ref_idx >= H264SW_MAX_LIST)
		return EINVAL;

	/* Refuses an entry without a frame. */
	entry = &ctx->lists.list[list][ref_idx];
	if (entry->frame == NULL)
		return EINVAL;

	/* Succeeded: the id of the frame. */
	*ref_id = entry->frame->id;
	return 0;
}

/*
 * Derives one quadrant in the spatial direct mode (8.4.1.2.2).
 *
 * The reference indices and the predicted vectors are those of the
 * macroblock; each 4x4 block of the quadrant takes the vector 0 of a
 * list whose reference index is 0 when colZeroFlag is 1.
 */
static int
direct_quadrant_spatial(
	struct h264sw_slice_ctx *ctx,
	unsigned quadrant,
	int inference)
{
	struct h264sw_motion *motion;
	struct direct_col colocated;
	unsigned sub;
	unsigned blk;
	unsigned blk_col;
	unsigned list;
	int error;
	int col_zero;
	int ref_idx[2];
	uint32_t ref_id[2];
	int16_t mv[2][4][2];

	/* Derives the macroblock's reference indices and predicted vectors once. */
	if (!ctx->direct.spatial_ready)
		direct_spatial_prepare(ctx);

	/* Resolves the frames of the two reference indices, refusing an index the lists do not hold. */
	for (list = 0; list < 2U; list++) {
		ref_idx[list] = ctx->direct.spatial_ref[list];
		error = direct_ref_id(ctx, list, ref_idx[list], &ref_id[list]);
		if (error != 0)
			return error;
	}

	/* Derives the vectors of the four 4x4 blocks of the quadrant. */
	for (sub = 0; sub < 4U; sub++) {
		/* Chooses the co-located block: the corner of the quadrant with direct_8x8_inference_flag 1, else the block's own position. */
		blk = 4U * quadrant + sub;
		blk_col = blk;
		if (inference != 0)
			blk_col = direct_corner_blk[quadrant];

		/* Derives mvCol and refIdxCol. */
		error = direct_colocated(ctx, blk_col, &colocated);
		if (error != 0)
			return error;

		/* Derives colZeroFlag: RefPicList1[ 0 ] is short-term, refIdxCol is 0 and both components of mvCol are within -1..1. */
		col_zero = 0;
		if (ctx->direct.col_long_term == 0 &&
		    colocated.ref_idx == 0 &&
		    colocated.mv[0] >= -1 && colocated.mv[0] <= 1 &&
		    colocated.mv[1] >= -1 && colocated.mv[1] <= 1)
			col_zero = 1;

		/* Gives each list the predicted vector, or 0 for an unused list or a colZeroFlag with reference index 0. */
		for (list = 0; list < 2U; list++) {
			mv[list][sub][0] = ctx->direct.spatial_mv[list][0];
			mv[list][sub][1] = ctx->direct.spatial_mv[list][1];
			if (ref_idx[list] < 0 || (ref_idx[list] == 0 && col_zero != 0)) {
				mv[list][sub][0] = 0;
				mv[list][sub][1] = 0;
			}
		}
	}

	/* Writes the quadrant's reference indices, frames and vectors into the current macroblock's motion. */
	motion = &ctx->n.current->motion;
	for (list = 0; list < 2U; list++) {
		motion->ref_idx[list][quadrant] = (int8_t)ref_idx[list];
		motion->ref_id[list][quadrant] = ref_id[list];
		for (sub = 0; sub < 4U; sub++) {
			motion->mv[list][4U * quadrant + sub][0] = mv[list][sub][0];
			motion->mv[list][4U * quadrant + sub][1] = mv[list][sub][1];
		}
	}

	/* Succeeded: the quadrant's motion is written. */
	return 0;
}

/*
 * Derives one quadrant in the temporal direct mode (8.4.1.2.3).
 *
 * refIdxL0 comes from the frame the co-located quadrant referred to;
 * the two vectors of each 4x4 block are scaled copies of mvCol.
 */
static int
direct_quadrant_temporal(
	struct h264sw_slice_ctx *ctx,
	unsigned quadrant,
	int inference)
{
	struct h264sw_motion *motion;
	struct direct_col colocated;
	unsigned sub;
	unsigned blk_col;
	unsigned list;
	int error;
	int ref_idx0;
	uint32_t ref_id[2];
	int mv_l0[2];
	int mv_l1[2];
	int component;
	int64_t scaled;
	int16_t mv[2][4][2];

	/* Derives the co-located block of the quadrant's first 4x4 block, whose reference decides refIdxL0 for the whole quadrant. */
	blk_col = 4U * quadrant;
	if (inference != 0)
		blk_col = direct_corner_blk[quadrant];
	error = direct_colocated(ctx, blk_col, &colocated);
	if (error != 0)
		return error;

	/* Gives refIdxL0 0 when the co-located block has no reference (8-191). */
	ref_idx0 = 0;
	if (colocated.ref_idx >= 0) {
		/* Finds the lowest index of list 0 holding the frame the co-located block referred to (MapColToList0). */
		ref_idx0 = h264sw_reflist_find(&ctx->lists, colocated.ref_id);
		if (ref_idx0 < 0) {
			/* The stream breaks the rule that list 0 holds that frame: uses index 0 and counts the macroblock as concealed. */
			ref_idx0 = 0;
			ctx->concealed++;
		}
	}

	/* Resolves the frames of refIdxL0 and of refIdxL1 (0), refusing an index the lists do not hold. */
	error = direct_ref_id(ctx, 0, ref_idx0, &ref_id[0]);
	if (error != 0)
		return error;
	error = direct_ref_id(ctx, 1, 0, &ref_id[1]);
	if (error != 0)
		return error;

	/* Derives the vectors of the four 4x4 blocks of the quadrant. */
	for (sub = 0; sub < 4U; sub++) {
		/* Chooses the co-located block: the corner of the quadrant with direct_8x8_inference_flag 1, else the block's own position. */
		blk_col = 4U * quadrant + sub;
		if (inference != 0)
			blk_col = direct_corner_blk[quadrant];

		/* Derives mvCol. */
		error = direct_colocated(ctx, blk_col, &colocated);
		if (error != 0)
			return error;

		/* Takes the vector unscaled for list 0 and 0 for list 1 for a long-term reference or a distance of 0 (8-195, 8-196). */
		if (ctx->direct.long_term0[ref_idx0] != 0) {
			mv_l0[0] = colocated.mv[0];
			mv_l0[1] = colocated.mv[1];
			mv_l1[0] = 0;
			mv_l1[1] = 0;
		} else {
			/* Scales mvCol by DistScaleFactor (8-199) and derives mvL1 as the difference (8-200). */
			for (component = 0; component < 2; component++) {
				scaled = (int64_t)ctx->direct.dist_scale[ref_idx0] * (int64_t)colocated.mv[component] + 128;
				scaled = direct_floor_shift(scaled, 8U);
				if (scaled < DIRECT_MV_MIN)
					scaled = DIRECT_MV_MIN;
				if (scaled > DIRECT_MV_MAX)
					scaled = DIRECT_MV_MAX;
				mv_l0[component] = (int)scaled;
				mv_l1[component] = mv_l0[component] - colocated.mv[component];
			}
		}

		/* Limits the vectors to the stored type. */
		mv[0][sub][0] = direct_clamp_mv(mv_l0[0]);
		mv[0][sub][1] = direct_clamp_mv(mv_l0[1]);
		mv[1][sub][0] = direct_clamp_mv(mv_l1[0]);
		mv[1][sub][1] = direct_clamp_mv(mv_l1[1]);
	}

	/* Writes the quadrant's reference indices (refIdxL1 is 0), frames and vectors into the current macroblock's motion. */
	motion = &ctx->n.current->motion;
	motion->ref_idx[0][quadrant] = (int8_t)ref_idx0;
	motion->ref_idx[1][quadrant] = 0;
	for (list = 0; list < 2U; list++) {
		motion->ref_id[list][quadrant] = ref_id[list];
		for (sub = 0; sub < 4U; sub++) {
			motion->mv[list][4U * quadrant + sub][0] = mv[list][sub][0];
			motion->mv[list][4U * quadrant + sub][1] = mv[list][sub][1];
		}
	}

	/* Succeeded: the quadrant's motion is written. */
	return 0;
}
