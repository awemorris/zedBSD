/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mouse's pointer acceleration (ws089-p024).
 *
 * A report's motion is first scaled by the user's speed (a percentage of
 * the counts, 100 moving one pixel a count), then by a gain in 1/256 that
 * depends on how fast the mouse moved: 1 up to ACCEL_SLOW counts a second,
 * rising in proportion to the level's most at ACCEL_FAST and above.  The
 * mouse's speed is the motion over the time since its last moving report,
 * never less than ACCEL_FRAME_LEAST nor more than ACCEL_FRAME_MOST (the
 * first report after a pause is slow).  With no acceleration the gain is
 * always 1, as before (only the speed).
 *
 * A mouse of 800 to 1000 counts an inch moves about 400 counts a second
 * when it is placed with care and 4000 or more when it is thrown across the
 * screen; the strongest level then triples the motion.
 */

#include "pointer-accel.h"

/* The speeds (counts a second) the gain starts to rise at and stops rising at. */
#define ACCEL_SLOW		400
#define ACCEL_FAST		4000

/* The time between reports the speed is measured over, at least and at most (microseconds). */
#define ACCEL_FRAME_LEAST	1000U
#define ACCEL_FRAME_MOST	50000U

/* The units of the gain, and of the speed's percentage times the gain. */
#define ACCEL_ONE		256
#define ACCEL_SCALE		25600

/* The most gain of each level, in 1/256: none, mild, medium, strong. */
static const int64_t accel_most[] = { 256, 384, 576, 768 };

static int64_t accel_magnitude(int64_t value);

/*
 * Starts a mouse's acceleration: nothing left over, no report yet.
 */
void
kwl_pointer_accel_init(
	struct kwl_pointer_accel *accel)
{
	/* Nothing left over and no time yet. */
	accel->remainder_x = 0;
	accel->remainder_y = 0;
	accel->last_us = 0;
	accel->started = 0;
}

/*
 * Gives the gain (in 1/256) of a level at a mouse's speed in counts a
 * second; a level out of range is taken as the nearest one.
 */
int64_t
kwl_pointer_accel_gain(
	int64_t counts_per_second,
	int32_t level)
{
	int64_t most;

	/* The level, kept in the table. */
	if (level < KWL_ACCEL_NONE)
		level = KWL_ACCEL_NONE;
	if (level > KWL_ACCEL_STRONG)
		level = KWL_ACCEL_STRONG;
	most = accel_most[level];

	/* Slow, fast, or between them in proportion. */
	if (counts_per_second <= ACCEL_SLOW)
		return ACCEL_ONE;
	if (counts_per_second >= ACCEL_FAST)
		return most;

	/* Succeeded: the gain on the line between the two. */
	return ACCEL_ONE + (most - ACCEL_ONE) * (counts_per_second - ACCEL_SLOW) / (ACCEL_FAST - ACCEL_SLOW);
}

/*
 * Turns one report's motion (counts, at time_us) into whole pixels at a
 * speed (percent) and an acceleration level, keeping the fractions for the
 * next report.
 */
void
kwl_pointer_accel_move(
	struct kwl_pointer_accel *accel,
	int64_t dx,
	int64_t dy,
	uint64_t time_us,
	int32_t speed,
	int32_t level,
	int64_t *pixels_x,
	int64_t *pixels_y)
{
	uint64_t elapsed;
	int64_t counts_per_second;
	int64_t gain;
	int64_t x;
	int64_t y;

	/* No motion moves nothing and leaves the time alone. */
	*pixels_x = 0;
	*pixels_y = 0;
	if (dx == 0 && dy == 0)
		return;

	/* The time since the last moving report, within reason (the first is slow). */
	elapsed = ACCEL_FRAME_MOST;
	if (accel->started && time_us >= accel->last_us)
		elapsed = time_us - accel->last_us;
	if (elapsed < ACCEL_FRAME_LEAST)
		elapsed = ACCEL_FRAME_LEAST;
	if (elapsed > ACCEL_FRAME_MOST)
		elapsed = ACCEL_FRAME_MOST;
	accel->last_us = time_us;
	accel->started = 1;

	/* The mouse's speed and the gain it gives. */
	counts_per_second = (accel_magnitude(dx) + accel_magnitude(dy)) * 1000000 / (int64_t)elapsed;
	gain = kwl_pointer_accel_gain(counts_per_second, level);

	/* The motion in 1/25600 pixels, with what the last reports left over. */
	x = dx * speed * gain + accel->remainder_x;
	y = dy * speed * gain + accel->remainder_y;

	/* Whole pixels move the pointer; the fractions wait. */
	accel->remainder_x = x % ACCEL_SCALE;
	accel->remainder_y = y % ACCEL_SCALE;
	*pixels_x = x / ACCEL_SCALE;
	*pixels_y = y / ACCEL_SCALE;
}

/* Gives a number's size without its sign. */
static int64_t
accel_magnitude(
	int64_t value)
{
	/* A negative number turned round. */
	if (value < 0)
		return -value;

	/* Succeeded: the number itself. */
	return value;
}
