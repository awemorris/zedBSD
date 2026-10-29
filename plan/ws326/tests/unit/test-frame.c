/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the frame pool of the software H.264 decoder: the argument
 * ranges of a pool, the layout of a frame's planes and motion, the
 * reference counts, the reuse of a free frame with a new id, the limit,
 * the grey fill, and the lifetime of frames held past the destruction of
 * their pool (the address sanitizer's leak check is the proof that
 * nothing is left behind).
 */

#include "check.h"

#include "userland/desktop/libmedia/h264sw-frame.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The size of the pictures most checks use: 4 by 3 macroblocks, 64 by 48 samples. */
#define TEST_WIDTH_MBS 4U
#define TEST_HEIGHT_MBS 3U
#define TEST_WIDTH 64U
#define TEST_HEIGHT 48U

/* The cropping window most checks use. */
#define TEST_CROP_X 2U
#define TEST_CROP_Y 4U
#define TEST_CROP_WIDTH 60U
#define TEST_CROP_HEIGHT 40U

/* The pool limit most checks use. */
#define TEST_LIMIT 3U

/* The sample value of a grey frame. */
#define TEST_GREY 128U

/*
 * Makes the pool most checks use and checks that it was made.
 */
static struct h264sw_frame_pool *
make_pool(
	unsigned limit)
{
	struct h264sw_frame_pool *pool;
	int error;

	/* Makes a pool of 4 by 3 macroblocks with the usual window. */
	pool = NULL;
	error = h264sw_frame_pool_create(TEST_WIDTH_MBS, TEST_HEIGHT_MBS, TEST_CROP_X, TEST_CROP_Y, TEST_CROP_WIDTH, TEST_CROP_HEIGHT, limit, &pool);
	CHECK(error == 0);
	CHECK(pool != NULL);

	/* Hands the pool to the check. */
	return pool;
}

/*
 * Checks that every macroblock of a frame is intra with unused lists and zero vectors.
 */
static void
check_motion_intra(
	const struct h264sw_frame *frame)
{
	unsigned index;
	unsigned list;
	unsigned part;
	unsigned block;
	int clean;

	/* Looks at every field of every macroblock's record. */
	clean = 1;
	for (index = 0U;
	     index < frame->size_mbs;
	     index++) {
		if (frame->motion[index].intra != 1U)
			clean = 0;
		for (list = 0U;
		     list < 2U;
		     list++) {
			for (part = 0U;
			     part < 4U;
			     part++) {
				if (frame->motion[index].ref_idx[list][part] != -1)
					clean = 0;
				if (frame->motion[index].ref_id[list][part] != 0U)
					clean = 0;
			}
			for (block = 0U;
			     block < 16U;
			     block++) {
				if (frame->motion[index].mv[list][block][0] != 0)
					clean = 0;
				if (frame->motion[index].mv[list][block][1] != 0)
					clean = 0;
			}
		}
	}

	/* Reports the whole frame as one check. */
	CHECK(clean);
}

/*
 * Checks that a range of bytes all hold one value.
 */
static int
bytes_are(
	const uint8_t *data,
	size_t size,
	unsigned value)
{
	size_t index;

	/* Compares each byte. */
	for (index = 0U;
	     index < size;
	     index++) {
		if (data[index] != (uint8_t)value)
			return 0;
	}

	/* All bytes matched. */
	return 1;
}

/*
 * Checks the argument ranges of a pool.
 */
static void
test_pool_create_ranges(void)
{
	struct h264sw_frame_pool *pool;

	/* A pool needs somewhere to be reported. */
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 64U, 48U, 3U, NULL) == EINVAL);

	/* A side of 0 macroblocks, or above the largest side, is refused. */
	pool = NULL;
	CHECK(h264sw_frame_pool_create(0U, 3U, 0U, 0U, 16U, 16U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(3U, 0U, 0U, 0U, 16U, 16U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(H264SW_MAX_SIDE_MBS + 1U, 1U, 0U, 0U, 16U, 16U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(1U, H264SW_MAX_SIDE_MBS + 1U, 0U, 0U, 16U, 16U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(0xFFFFFFFFU, 0xFFFFFFFFU, 0U, 0U, 16U, 16U, 3U, &pool) == EINVAL);
	CHECK(pool == NULL);

	/* Sides that are each allowed but whose product passes the largest picture are refused (1055 * 132 = 139260 is allowed, 1055 * 133 = 140315 is not). */
	CHECK(h264sw_frame_pool_create(1055U, 133U, 0U, 0U, 16U, 16U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(H264SW_MAX_SIDE_MBS, H264SW_MAX_SIDE_MBS, 0U, 0U, 16U, 16U, 3U, &pool) == EINVAL);
	CHECK(pool == NULL);

	/* A window that is empty, leaves the frame, or overflows a sum is refused. */
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 0U, 48U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 64U, 0U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 1U, 0U, 64U, 48U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 1U, 64U, 48U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 64U, 0U, 1U, 48U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 65U, 48U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 0xFFFFFFFFU, 0U, 2U, 48U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0xFFFFFFFFU, 64U, 2U, 3U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 0xFFFFFFFFU, 48U, 3U, &pool) == EINVAL);
	CHECK(pool == NULL);

	/* A limit of 0, or above the most a pool makes, is refused. */
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 64U, 48U, 0U, &pool) == EINVAL);
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 64U, 48U, H264SW_FRAME_POOL_MAX + 1U, &pool) == EINVAL);
	CHECK(pool == NULL);

	/* The edges of every range are allowed: the whole frame as the window, one sample as the window, and the largest limit. */
	CHECK(h264sw_frame_pool_create(4U, 3U, 0U, 0U, 64U, 48U, 1U, &pool) == 0);
	h264sw_frame_pool_destroy(pool);
	pool = NULL;
	CHECK(h264sw_frame_pool_create(4U, 3U, 63U, 47U, 1U, 1U, H264SW_FRAME_POOL_MAX, &pool) == 0);
	h264sw_frame_pool_destroy(pool);
	pool = NULL;
	CHECK(h264sw_frame_pool_create(1U, 1U, 0U, 0U, 16U, 16U, 1U, &pool) == 0);
	h264sw_frame_pool_destroy(pool);
	pool = NULL;
	CHECK(h264sw_frame_pool_create(1055U, 132U, 0U, 0U, 16880U, 2112U, 1U, &pool) == 0);
	h264sw_frame_pool_destroy(pool);
}

/*
 * Checks what a new pool and a new frame contain.
 */
static void
test_new_frame(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *frame;

	/* A new pool has made nothing and knows its size and window. */
	pool = make_pool(TEST_LIMIT);
	CHECK(pool->allocated == 0U);
	CHECK(pool->free == NULL);
	CHECK(pool->dead == 0);
	CHECK(pool->limit == TEST_LIMIT);
	CHECK(pool->width == TEST_WIDTH);
	CHECK(pool->height == TEST_HEIGHT);
	CHECK(pool->size_mbs == TEST_WIDTH_MBS * TEST_HEIGHT_MBS);

	/* The first frame has one reference and the first id. */
	frame = h264sw_frame_get(pool);
	CHECK(frame != NULL);
	CHECK(frame->refs == 1U);
	CHECK(frame->id == 1U);
	CHECK(frame->pool == pool);
	CHECK(frame->next_free == NULL);
	CHECK(pool->allocated == 1U);

	/* The frame has the size and window of the pool. */
	CHECK(frame->width == TEST_WIDTH);
	CHECK(frame->height == TEST_HEIGHT);
	CHECK(frame->size_mbs == 12U);
	CHECK(frame->crop_x == TEST_CROP_X);
	CHECK(frame->crop_y == TEST_CROP_Y);
	CHECK(frame->crop_width == TEST_CROP_WIDTH);
	CHECK(frame->crop_height == TEST_CROP_HEIGHT);

	/* The planes are 4:2:0 without borders: the strides are the widths. */
	CHECK(frame->stride[0] == 64U);
	CHECK(frame->stride[1] == 32U);
	CHECK(frame->stride[2] == 32U);
	CHECK(frame->plane[0] != NULL);
	CHECK(frame->plane[1] == frame->plane[0] + 64U * 48U);
	CHECK(frame->plane[2] == frame->plane[1] + 32U * 24U);

	/* A new frame's memory is cleared. */
	CHECK(bytes_are(frame->plane[0], 64U * 48U, 0U));
	CHECK(bytes_are(frame->plane[1], 32U * 24U, 0U));
	CHECK(bytes_are(frame->plane[2], 32U * 24U, 0U));

	/* A new frame's motion is all intra with ref_idx -1. */
	check_motion_intra(frame);

	/* The picture's fields start cleared. */
	CHECK(frame->poc == 0);
	CHECK(frame->top_poc == 0);
	CHECK(frame->bottom_poc == 0);
	CHECK(frame->frame_num == 0U);
	CHECK(frame->idr == 0);
	CHECK(frame->time_us == 0);
	CHECK(frame->concealed == 0U);
	CHECK(frame->non_existing == 0);
	CHECK(frame->sar_width == 0U);
	CHECK(frame->sar_height == 0U);
	CHECK(frame->signal_present == 0);
	CHECK(frame->full_range == 0);
	CHECK(frame->colour_present == 0);
	CHECK(frame->matrix_coefficients == 0U);

	/* Writes the last byte of each plane and of the motion, which the sanitizer checks are inside the allocation. */
	frame->plane[0][64U * 48U - 1U] = 1U;
	frame->plane[1][32U * 24U - 1U] = 2U;
	frame->plane[2][32U * 24U - 1U] = 3U;
	frame->motion[11].intra = 0U;
	frame->motion[11].mv[1][15][1] = 7;
	CHECK(frame->plane[0][64U * 48U - 1U] == 1U);
	CHECK(frame->plane[1][32U * 24U - 1U] == 2U);
	CHECK(frame->plane[2][32U * 24U - 1U] == 3U);

	/* Releases the frame and the pool. */
	h264sw_frame_unref(frame);
	h264sw_frame_pool_destroy(pool);
}

/*
 * Checks the reference counts and the reuse of a free frame.
 */
static void
test_ref_unref_reuse(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *first;
	struct h264sw_frame *second;
	struct h264sw_frame *third;
	uint32_t first_id;

	/* A frame with a second reference stays out of the free list after one unref. */
	pool = make_pool(TEST_LIMIT);
	first = h264sw_frame_get(pool);
	CHECK(first != NULL);
	h264sw_frame_ref(first);
	CHECK(first->refs == 2U);
	h264sw_frame_unref(first);
	CHECK(first->refs == 1U);
	CHECK(pool->free == NULL);

	/* NULL is ignored by both. */
	h264sw_frame_ref(NULL);
	h264sw_frame_unref(NULL);

	/* The picture's fields and the samples are set so a reuse shows what it keeps. */
	first->poc = 11;
	first->top_poc = 12;
	first->bottom_poc = 13;
	first->frame_num = 14U;
	first->idr = 1;
	first->time_us = 15;
	first->concealed = 16U;
	first->non_existing = 1;
	first->sar_width = 17U;
	first->sar_height = 18U;
	first->signal_present = 1;
	first->full_range = 1;
	first->colour_present = 1;
	first->matrix_coefficients = 9U;
	first->crop_x = 0U;
	first->crop_width = 1U;
	first->plane[0][5] = 77U;
	first->plane[2][3] = 78U;
	first->motion[4].intra = 0U;
	first->motion[4].mv[0][3][0] = -9;
	first->motion[4].ref_idx[0][2] = 1;
	first->motion[4].ref_id[0][2] = 99U;
	first_id = first->id;

	/* The last unref puts the frame on the free list. */
	h264sw_frame_unref(first);
	CHECK(first->refs == 0U);
	CHECK(pool->free == first);

	/* A second unref of a frame already returned changes nothing. */
	h264sw_frame_unref(first);
	CHECK(first->refs == 0U);
	CHECK(pool->free == first);
	CHECK(first->next_free == NULL);

	/* The next get reuses the frame with a new id and cleared fields. */
	second = h264sw_frame_get(pool);
	CHECK(second == first);
	CHECK(pool->allocated == 1U);
	CHECK(pool->free == NULL);
	CHECK(second->refs == 1U);
	CHECK(second->id == first_id + 1U);
	CHECK(second->next_free == NULL);
	CHECK(second->poc == 0);
	CHECK(second->top_poc == 0);
	CHECK(second->bottom_poc == 0);
	CHECK(second->frame_num == 0U);
	CHECK(second->idr == 0);
	CHECK(second->time_us == 0);
	CHECK(second->concealed == 0U);
	CHECK(second->non_existing == 0);
	CHECK(second->sar_width == 0U);
	CHECK(second->sar_height == 0U);
	CHECK(second->signal_present == 0);
	CHECK(second->full_range == 0);
	CHECK(second->colour_present == 0);
	CHECK(second->matrix_coefficients == 0U);
	CHECK(second->crop_x == TEST_CROP_X);
	CHECK(second->crop_width == TEST_CROP_WIDTH);

	/* The samples and the motion are left as they were. */
	CHECK(second->plane[0][5] == 77U);
	CHECK(second->plane[2][3] == 78U);
	CHECK(second->motion[4].intra == 0U);
	CHECK(second->motion[4].mv[0][3][0] == -9);
	CHECK(second->motion[4].ref_idx[0][2] == 1);
	CHECK(second->motion[4].ref_id[0][2] == 99U);

	/* A free list is last in, first out and each frame comes back once. */
	third = h264sw_frame_get(pool);
	CHECK(third != NULL);
	CHECK(third != second);
	CHECK(third->id == second->id + 1U);
	h264sw_frame_unref(second);
	h264sw_frame_unref(third);
	CHECK(pool->free == third);
	CHECK(third->next_free == second);
	CHECK(second->next_free == NULL);
	CHECK(h264sw_frame_get(pool) == third);
	CHECK(h264sw_frame_get(pool) == second);
	CHECK(pool->allocated == 2U);

	/* Releases both frames and the pool. */
	h264sw_frame_unref(second);
	h264sw_frame_unref(third);
	h264sw_frame_pool_destroy(pool);
}

/*
 * Checks that ids increase on every get and skip 0 when the counter wraps.
 */
static void
test_ids(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *frame;
	uint32_t previous;
	unsigned count;
	int increasing;

	/* Hands one frame out and takes it back many times: each id is above the last. */
	pool = make_pool(1U);
	previous = 0U;
	increasing = 1;
	for (count = 0U;
	     count < 100U;
	     count++) {
		frame = h264sw_frame_get(pool);
		CHECK(frame != NULL);
		if (frame->id <= previous)
			increasing = 0;
		previous = frame->id;
		h264sw_frame_unref(frame);
	}
	CHECK(increasing);
	CHECK(previous == 100U);

	/* The counter wraps past 0xFFFFFFFF to 1, never 0. */
	pool->next_id = 0xFFFFFFFEU;
	frame = h264sw_frame_get(pool);
	CHECK(frame->id == 0xFFFFFFFEU);
	h264sw_frame_unref(frame);
	frame = h264sw_frame_get(pool);
	CHECK(frame->id == 0xFFFFFFFFU);
	h264sw_frame_unref(frame);
	frame = h264sw_frame_get(pool);
	CHECK(frame->id == 1U);
	h264sw_frame_unref(frame);

	/* Releases the pool. */
	h264sw_frame_pool_destroy(pool);
}

/*
 * Checks that a pool does not make more frames than its limit.
 */
static void
test_limit(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *frames[TEST_LIMIT];
	struct h264sw_frame *extra;
	unsigned index;

	/* Takes as many frames as the limit allows. */
	pool = make_pool(TEST_LIMIT);
	for (index = 0U;
	     index < TEST_LIMIT;
	     index++) {
		frames[index] = h264sw_frame_get(pool);
		CHECK(frames[index] != NULL);
	}
	CHECK(pool->allocated == TEST_LIMIT);

	/* One more fails, and keeps failing. */
	CHECK(h264sw_frame_get(pool) == NULL);
	CHECK(h264sw_frame_get(pool) == NULL);
	CHECK(pool->allocated == TEST_LIMIT);

	/* A returned frame makes room again, as the same frame, and the limit holds again. */
	h264sw_frame_unref(frames[1]);
	extra = h264sw_frame_get(pool);
	CHECK(extra == frames[1]);
	CHECK(h264sw_frame_get(pool) == NULL);

	/* A NULL pool hands out nothing. */
	CHECK(h264sw_frame_get(NULL) == NULL);

	/* Releases everything. */
	for (index = 0U;
	     index < TEST_LIMIT;
	     index++)
		h264sw_frame_unref(frames[index]);
	h264sw_frame_pool_destroy(pool);

	/* A limit of 1 gives exactly one frame at a time. */
	pool = make_pool(1U);
	extra = h264sw_frame_get(pool);
	CHECK(extra != NULL);
	CHECK(h264sw_frame_get(pool) == NULL);
	h264sw_frame_unref(extra);
	extra = h264sw_frame_get(pool);
	CHECK(extra != NULL);
	h264sw_frame_unref(extra);
	h264sw_frame_pool_destroy(pool);
}

/*
 * Checks the grey fill.
 */
static void
test_fill_grey(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *frame;
	unsigned index;

	/* Makes a frame with non-grey samples and motion in every part. */
	pool = make_pool(TEST_LIMIT);
	frame = h264sw_frame_get(pool);
	CHECK(frame != NULL);
	memset(frame->plane[0], 1, 64U * 48U);
	memset(frame->plane[1], 2, 32U * 24U);
	memset(frame->plane[2], 3, 32U * 24U);
	for (index = 0U;
	     index < frame->size_mbs;
	     index++) {
		frame->motion[index].intra = 0U;
		frame->motion[index].mv[1][15][0] = 5;
		frame->motion[index].mv[0][0][1] = -5;
		frame->motion[index].ref_idx[0][0] = 2;
		frame->motion[index].ref_idx[1][3] = 1;
		frame->motion[index].ref_id[0][0] = 41U;
		frame->motion[index].ref_id[1][3] = 42U;
	}

	/* Fills it grey. */
	h264sw_frame_fill_grey(frame);

	/* Every sample of the three planes is 128, including the last byte of each. */
	CHECK(bytes_are(frame->plane[0], 64U * 48U, TEST_GREY));
	CHECK(bytes_are(frame->plane[1], 32U * 24U, TEST_GREY));
	CHECK(bytes_are(frame->plane[2], 32U * 24U, TEST_GREY));

	/* Every macroblock is intra again with no reference. */
	check_motion_intra(frame);

	/* The identity and the count are not touched. */
	CHECK(frame->id == 1U);
	CHECK(frame->refs == 1U);

	/* A missing frame is ignored. */
	h264sw_frame_fill_grey(NULL);

	/* Releases the frame and the pool. */
	h264sw_frame_unref(frame);
	h264sw_frame_pool_destroy(pool);
}

/*
 * Checks that frames outlive the destruction of their pool.
 */
static void
test_destroy_with_frames_held(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *held;
	struct h264sw_frame *shared;
	struct h264sw_frame *spare;

	/* A pool with a frame held, a frame held twice and a free frame. */
	pool = make_pool(TEST_LIMIT);
	held = h264sw_frame_get(pool);
	shared = h264sw_frame_get(pool);
	spare = h264sw_frame_get(pool);
	CHECK(held != NULL);
	CHECK(shared != NULL);
	CHECK(spare != NULL);
	h264sw_frame_ref(shared);
	h264sw_frame_unref(spare);
	CHECK(pool->allocated == 3U);

	/* Destroying frees the free frame and keeps the pool for the others. */
	h264sw_frame_pool_destroy(pool);
	CHECK(pool->dead == 1);
	CHECK(pool->free == NULL);
	CHECK(pool->allocated == 2U);

	/* A dead pool makes and reuses no frame. */
	CHECK(h264sw_frame_get(pool) == NULL);

	/* The held frames are still valid. */
	held->plane[0][100] = 9U;
	shared->plane[2][7] = 8U;
	shared->motion[3].intra = 0U;
	CHECK(held->plane[0][100] == 9U);
	CHECK(shared->plane[2][7] == 8U);
	CHECK(shared->refs == 2U);

	/* Dropping one of two references frees nothing. */
	h264sw_frame_unref(shared);
	CHECK(shared->refs == 1U);
	CHECK(pool->allocated == 2U);

	/* The last unrefs free the frames, and the pool with the last of them (the sanitizer reports a leak or a use after free). */
	h264sw_frame_unref(held);
	CHECK(pool->allocated == 1U);
	h264sw_frame_unref(shared);
}

/*
 * Checks the destruction of pools that hold no frame out, and of NULL.
 */
static void
test_destroy_without_frames(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *first;
	struct h264sw_frame *second;

	/* A pool that never made a frame. */
	pool = make_pool(TEST_LIMIT);
	h264sw_frame_pool_destroy(pool);

	/* A pool whose frames are all free. */
	pool = make_pool(TEST_LIMIT);
	first = h264sw_frame_get(pool);
	second = h264sw_frame_get(pool);
	h264sw_frame_unref(first);
	h264sw_frame_unref(second);
	h264sw_frame_pool_destroy(pool);

	/* NULL is ignored. */
	h264sw_frame_pool_destroy(NULL);

	/* The sanitizer's leak check proves the memory was returned. */
	CHECK(1);
}

/*
 * Checks a frame of the largest picture level 6.2 allows.
 */
static void
test_largest_frame(void)
{
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *frame;
	size_t luma;
	int error;

	/* 1055 by 132 macroblocks is 139260 of the 139264 the level allows. */
	pool = NULL;
	error = h264sw_frame_pool_create(1055U, 132U, 0U, 0U, 16880U, 2112U, 1U, &pool);
	CHECK(error == 0);
	frame = h264sw_frame_get(pool);
	CHECK(frame != NULL);
	if (frame != NULL) {
		/* The last sample of each plane and the last macroblock's motion are inside the allocation. */
		luma = (size_t)16880U * 2112U;
		CHECK(frame->stride[0] == 16880U);
		CHECK(frame->plane[1] == frame->plane[0] + luma);
		CHECK(frame->plane[2] == frame->plane[1] + luma / 4U);
		frame->plane[0][luma - 1U] = 1U;
		frame->plane[1][luma / 4U - 1U] = 2U;
		frame->plane[2][luma / 4U - 1U] = 3U;
		frame->motion[139259].ref_id[1][3] = 5U;
		h264sw_frame_fill_grey(frame);
		CHECK(frame->plane[2][luma / 4U - 1U] == TEST_GREY);
		h264sw_frame_unref(frame);
	}
	h264sw_frame_pool_destroy(pool);
}

/*
 * Runs every check and prints the summary line.
 */
int
main(void)
{
	test_pool_create_ranges();
	test_new_frame();
	test_ref_unref_reuse();
	test_ids();
	test_limit();
	test_fill_grey();
	test_destroy_with_frames_held();
	test_destroy_without_frames();
	test_largest_frame();

	/* Reports the checks. */
	return check_finish("frame");
}
