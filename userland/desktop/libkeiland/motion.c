/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Touch motion (WS081, plan/ws081/design.md sections 3 and 6): the point a
 * contact is drawn at, its velocity when it lifts, and the mapping of a
 * panel's Scan Time onto the host clock.
 *
 * The point is the design's "lsq12": a line and a parabola are fitted by
 * least squares to the contact's last reports and evaluated at a time
 * behind the frame, chosen so that the usual report needs no more than the
 * allowed extrapolation.  Behind the last report the parabola is used;
 * past it, of the line's and the parabola's prediction the one that goes
 * less far along the line's direction, and never backwards.  The velocity
 * is a constant-velocity Kalman filter's, unless a short line says the
 * finger had come to rest.  The numbers behind every choice and constant
 * are in the design and its host comparison, plan/ws081/tests.
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The reports a motion keeps: enough for the longest window at 120 Hz. */
#define MOTION_REPORTS_MAX		32U

/* The samples a fit may use: every report and one put back before each gap. */
#define MOTION_SAMPLES_MAX		(2U * MOTION_REPORTS_MAX)

/* The last intervals and delays the period and the delay are measured over. */
#define MOTION_HISTORY			8U

/* A stroke's own period is used once it has this many intervals that are not gaps. */
#define MOTION_INTERVALS_MIN		3U

/* The period assumed of a device not measured yet (60 Hz), in microseconds. */
#define MOTION_DEFAULT_INTERVAL		16667.0

/* The delay assumed of a device not measured yet, in microseconds. */
#define MOTION_DEFAULT_DELAY		2000.0

/* The noise assumed of a device not measured yet, in pixels. */
#define MOTION_DEFAULT_NOISE		1.0

/* The shortest period a gap is measured against (bursts of reports at one time). */
#define MOTION_INTERVAL_FLOOR		1000.0

/* An interval longer than this many periods is a gap: the panel said nothing while the finger stayed still. */
#define MOTION_GAP_PERIODS		2.5

/* The weight of a stroke's measurement when it is folded into its device. */
#define MOTION_FOLD_WEIGHT		0.25

/* How much the time behind the frame may change in one call, in microseconds. */
#define MOTION_BEHIND_STEP		500.0

/*
 * The longest delay the time behind the frame allows for: a period, or
 * this many microseconds when that is more (ws081-p010).  A longer delay is
 * the reader's (a program that read late, busy drawing), not the panel's
 * transport, and drawing further back than that only adds lag -- and puts
 * the drawn time where the fitted curve bends away from a finger that has
 * just started moving.
 */
#define MOTION_DELAY_LONGEST		25000.0

/* After this long without reports a motion forgets them, in microseconds. */
#define MOTION_FORGET			1000000U

/* The line's window: this many periods, and at least this many seconds. */
#define MOTION_LINE_PERIODS		2.5
#define MOTION_LINE_SECONDS		0.04

/* The parabola's window: this many periods, and at least this many seconds. */
#define MOTION_PARABOLA_PERIODS		3.5
#define MOTION_PARABOLA_SECONDS		0.06

/* A device noisier than this (pixels) gets windows this many times wider. */
#define MOTION_NOISY			1.5
#define MOTION_NOISY_SCALE		1.6

/* The noise of a stroke is measured on a parabola through this long a window, of at least six reports. */
#define MOTION_NOISE_SECONDS		0.15
#define MOTION_NOISE_REPORTS		6U

/* A stroke measures its noise every this many reports, and keeps this many measurements. */
#define MOTION_NOISE_EVERY		4U
#define MOTION_NOISE_SAMPLES		16U

/*
 * A device keeps its last strokes' noise, and takes the fourth smallest
 * once it has five (about the 20th percentile: a stroke's stops and turns
 * only ever make its measurement larger).
 */
#define MOTION_NOISE_STROKES		16U
#define MOTION_NOISE_STROKES_MIN	5U
#define MOTION_NOISE_RANK		3U

/* The velocity filter: white acceleration (px^2/s^3) and the least measurement noise (px^2). */
#define MOTION_FILTER_ACCELERATION	1.0e7
#define MOTION_FILTER_NOISE_MIN		2.0

/* The filter's first uncertainty of the position (px^2) and of the velocity ((px/s)^2). */
#define MOTION_FILTER_POSITION_START	4.0
#define MOTION_FILTER_VELOCITY_START	1.0e7

/* The rest gate: a line through this many periods (at least this many seconds, three reports). */
#define MOTION_REST_PERIODS		2.5
#define MOTION_REST_SECONDS		0.05

/* A finger slower than this (px/s), or than four standard errors of the line's slope, is at rest. */
#define MOTION_REST_SPEED		150.0
#define MOTION_REST_ERRORS		4.0

/* The last report is stale when the lift comes this many periods later (at least this many microseconds). */
#define MOTION_STALE_PERIODS		2.0
#define MOTION_STALE_MIN		50000.0

/* The fastest velocity reported, in px/s. */
#define MOTION_SPEED_MAX		8000.0

/* The drift of a panel's clock the mapping follows. */
#define MOTION_CLOCK_DRIFT		200.0e-6

/* A mapping that falls this far (microseconds) behind the host time starts again. */
#define MOTION_CLOCK_RESYNC		20000U

/* The Scan Time is checked over this much host time, and trusted within these rates of the host's. */
#define MOTION_CLOCK_CHECK		500000U
#define MOTION_CLOCK_RATE_LOW		0.9
#define MOTION_CLOCK_RATE_HIGH		1.1

/*
 * One report of a stroke: when the finger was there, when the caller read
 * it, and where the finger was.
 */
struct motion_report {
	uint64_t stamp;
	uint64_t arrival;
	double x;
	double y;
};

/*
 * One point a fit uses: its time in seconds from the stroke's last report
 * (zero or negative) and its position.
 */
struct motion_sample {
	double t;
	double x;
	double y;
};

/*
 * The velocity filter of one axis: the estimated position and velocity and
 * the covariance of their errors (p00 of the position, p11 of the velocity,
 * p01 between them).
 */
struct motion_axis {
	double position;
	double velocity;
	double p00;
	double p01;
	double p11;
};

/*
 * What is learned of one device across strokes.
 *
 * interval and delay are zero until a stroke has measured them.  The noise
 * ring holds the last strokes' measurements.  The clock fields map the
 * panel's Scan Time onto the host clock: the unwrapped device time, the
 * offset from it to the host clock (the lower envelope of host minus
 * device), and the sums of device and host time over the check window
 * that decide whether the Scan Time is trusted.
 */
struct kl_motion_device {
	double interval;
	double delay;
	double noise[MOTION_NOISE_STROKES];
	unsigned noise_count;
	unsigned noise_next;
	int clock_started;
	int clock_trusted;
	uint32_t clock_raw;
	uint64_t clock_device;
	uint64_t clock_host;
	int64_t clock_offset;
	uint64_t clock_stamp;
	uint64_t check_device;
	uint64_t check_host;
};

/*
 * One contact's stroke: its last reports (oldest first), the velocity
 * filter of each axis, the time behind the frame it is drawn at, and the
 * noise measured so far.
 *
 * behind is settled once the stroke has measured its own period; until
 * then it follows the wanted value at once, afterwards by at most
 * MOTION_BEHIND_STEP a call.
 */
struct kl_motion {
	struct kl_motion_device *device;
	struct motion_report reports[MOTION_REPORTS_MAX];
	unsigned count;
	unsigned added;
	struct motion_axis axes[2];
	uint64_t filter_stamp;
	int filtering;
	double behind;
	int behind_settled;
	double noise[MOTION_NOISE_SAMPLES];
	unsigned noise_count;
	unsigned noise_next;
};

static double seconds_between(uint64_t later, uint64_t earlier);
static void sort_values(double *values, unsigned count);
static double median(double *values, unsigned count);
static double stroke_interval(const struct kl_motion *motion, int *measured);
static double stroke_delay(const struct kl_motion *motion);
static double device_noise(const struct kl_motion_device *device);
static unsigned build_samples(const struct kl_motion *motion, double interval, int fill_gaps, struct motion_sample *samples);
static unsigned window_first(const struct motion_sample *samples, unsigned count, double window, unsigned minimum);
static int fit(const struct motion_sample *samples, unsigned first, unsigned count, int degree, double *cx, double *cy);
static void filter_start(struct motion_axis *axis, double position);
static void filter_update(struct motion_axis *axis, double dt, double measure, double noise);
static void stroke_noise_measure(struct kl_motion *motion);
static void clock_restart(struct kl_motion_device *device, uint64_t host_us, uint32_t device_us);
static void clock_check(struct kl_motion_device *device, uint64_t device_step, uint64_t host_step);

/*
 * Creates a device, knowing nothing of it yet.
 */
struct kl_motion_device *
kl_motion_device_create(
	void)
{
	struct kl_motion_device *device;

	/* Everything starts at zero: nothing measured, no clock. */
	device = calloc(1, sizeof(*device));
	if (device == NULL)
		return NULL;

	/* Succeeded: the new device. */
	return device;
}

/*
 * Destroys a device.
 */
void
kl_motion_device_destroy(
	struct kl_motion_device *device)
{
	/* The device owns nothing else. */
	free(device);
}

/*
 * Maps a report's Scan Time onto the host clock.
 *
 * The host time is the Scan Time plus a constant offset plus the delay of
 * the transfer, which is never negative, so the offset is the lower
 * envelope of host minus device time, allowed to rise with the drift of
 * the panel's clock.
 */
int
kl_motion_device_time(
	struct kl_motion_device *device,
	uint64_t host_us,
	uint32_t device_us,
	uint64_t *stamp_us)
{
	uint64_t device_step;
	uint64_t host_step;
	uint64_t stamp;
	int64_t candidate;
	int64_t allowed;
	int32_t step;
	int restart;

	/* Refuses a missing device or result. */
	if (device == NULL)
		return EINVAL;
	if (stamp_us == NULL)
		return EINVAL;

	/*
	 * The first report, a report after a second of silence, a host time
	 * that went back and a Scan Time that went back (the kernel starts it
	 * at zero again after a pause) all start the mapping again.
	 */
	restart = 0;
	step = (int32_t)(device_us - device->clock_raw);
	if (!device->clock_started) {
		restart = 1;
	} else if (host_us < device->clock_host) {
		restart = 1;
	} else if (host_us - device->clock_host > MOTION_FORGET) {
		restart = 1;
	} else if (step < 0) {
		restart = 1;
	}

	/* A restart maps this report onto its own host time. */
	if (restart) {
		clock_restart(device, host_us, device_us);
		*stamp_us = device->clock_stamp;
		return 0;
	}

	/* Unwraps the Scan Time and remembers this report. */
	device_step = (uint64_t)(uint32_t)step;
	host_step = host_us - device->clock_host;
	device->clock_device += device_step;
	device->clock_raw = device_us;
	device->clock_host = host_us;

	/* Checks that the Scan Time advances with the host clock. */
	clock_check(device, device_step, host_step);

	/* Follows the lower envelope of host minus device time. */
	candidate = (int64_t)host_us - (int64_t)device->clock_device;
	allowed = device->clock_offset + (int64_t)ceil(MOTION_CLOCK_DRIFT * (double)device_step);
	if (candidate < allowed) {
		device->clock_offset = candidate;
	} else {
		device->clock_offset = allowed;
	}

	/* The report's time on the host clock. */
	stamp = (uint64_t)((int64_t)device->clock_device + device->clock_offset);

	/* A mapping fallen far behind the host (a slow panel clock) starts again from this report. */
	if (host_us - stamp > MOTION_CLOCK_RESYNC) {
		device->clock_offset = candidate;
		stamp = host_us;
	}

	/* A Scan Time not trusted yet gives the host time. */
	if (!device->clock_trusted)
		stamp = host_us;

	/* The stamps never go back. */
	if (stamp < device->clock_stamp)
		stamp = device->clock_stamp;
	device->clock_stamp = stamp;

	/* Succeeded: the time the panel scanned the report. */
	*stamp_us = stamp;
	return 0;
}

/*
 * Reports the device's measured report period in microseconds.
 */
uint32_t
kl_motion_device_interval(
	const struct kl_motion_device *device)
{
	/* Nothing is known of a missing device. */
	if (device == NULL)
		return 0;

	/* Succeeded: the period, rounded (0 until a stroke measured it). */
	return (uint32_t)(device->interval + 0.5);
}

/*
 * Reports the device's measured noise in pixels.
 */
double
kl_motion_device_noise(
	const struct kl_motion_device *device)
{
	double noise;

	/* Nothing is known of a missing device. */
	if (device == NULL)
		return MOTION_DEFAULT_NOISE;
	noise = device_noise(device);

	/* Succeeded: the noise the motions use. */
	return noise;
}

/*
 * Creates a motion for contacts of a device.
 */
struct kl_motion *
kl_motion_create(
	struct kl_motion_device *device)
{
	struct kl_motion *motion;

	/* A motion learns into a device. */
	if (device == NULL)
		return NULL;

	/* Everything starts at zero: no reports. */
	motion = calloc(1, sizeof(*motion));
	if (motion == NULL)
		return NULL;
	motion->device = device;

	/* Succeeded: the new motion. */
	return motion;
}

/*
 * Destroys a motion.
 */
void
kl_motion_destroy(
	struct kl_motion *motion)
{
	/* The motion owns nothing else. */
	free(motion);
}

/*
 * Starts a stroke.
 */
void
kl_motion_begin(
	struct kl_motion *motion)
{
	/* Nothing to start. */
	if (motion == NULL)
		return;

	/* Forgets the last stroke without teaching the device. */
	motion->count = 0;
	motion->added = 0;
	motion->filtering = 0;
	motion->behind = 0.0;
	motion->behind_settled = 0;
	motion->noise_count = 0;
	motion->noise_next = 0;
}

/*
 * Adds one report of the stroke.
 */
int
kl_motion_add(
	struct kl_motion *motion,
	uint64_t stamp_us,
	uint64_t arrival_us,
	double x,
	double y)
{
	struct motion_report *last;
	struct motion_report *report;
	double dt;
	double noise;

	/* Refuses a missing motion. */
	if (motion == NULL)
		return EINVAL;

	/* Refuses a report older than the last one; forgets reports a second old. */
	if (motion->count != 0) {
		last = &motion->reports[motion->count - 1U];
		if (stamp_us < last->stamp)
			return EINVAL;
		if (stamp_us - last->stamp > MOTION_FORGET) {
			motion->count = 0;
			motion->filtering = 0;
			motion->behind_settled = 0;
		}
	}

	/* The oldest report makes room for the new one. */
	if (motion->count == MOTION_REPORTS_MAX) {
		memmove(&motion->reports[0], &motion->reports[1],
			(MOTION_REPORTS_MAX - 1U) * sizeof(motion->reports[0]));
		motion->count--;
	}

	/* Appends the report. */
	report = &motion->reports[motion->count];
	report->stamp = stamp_us;
	report->arrival = arrival_us;
	report->x = x;
	report->y = y;
	motion->count++;
	motion->added++;

	/* Feeds the velocity filter, starting it with the stroke's first report. */
	if (!motion->filtering) {
		filter_start(&motion->axes[0], x);
		filter_start(&motion->axes[1], y);
		motion->filter_stamp = stamp_us;
		motion->filtering = 1;
	} else {
		dt = seconds_between(stamp_us, motion->filter_stamp);
		noise = device_noise(motion->device);
		filter_update(&motion->axes[0], dt, x, noise);
		filter_update(&motion->axes[1], dt, y, noise);
		motion->filter_stamp = stamp_us;
	}

	/* Measures the stroke's noise every few reports. */
	if (motion->added % MOTION_NOISE_EVERY == 0U)
		stroke_noise_measure(motion);

	/* Succeeded: the report is part of the stroke. */
	return 0;
}

/*
 * Gives the point to draw at a frame.
 */
int
kl_motion_point(
	struct kl_motion *motion,
	uint64_t now_us,
	uint32_t extrapolation_us,
	double *x,
	double *y)
{
	struct motion_sample samples[MOTION_SAMPLES_MAX];
	const struct motion_report *last;
	double interval;
	double delay;
	double longest;
	double wanted;
	double target;
	double limit;
	double start;
	double scale;
	double window;
	double noise;
	double line_x[3];
	double line_y[3];
	double parabola_x[3];
	double parabola_y[3];
	double point_x;
	double point_y;
	double advance_line;
	double advance_parabola;
	unsigned count;
	unsigned first;
	int measured;

	/* Refuses a missing motion or result. */
	if (motion == NULL)
		return EINVAL;
	if (x == NULL)
		return EINVAL;
	if (y == NULL)
		return EINVAL;

	/* Nothing to draw before the first report. */
	if (motion->count == 0)
		return ENOENT;

	/* A single report is drawn where it is. */
	last = &motion->reports[motion->count - 1U];
	*x = last->x;
	*y = last->y;
	if (motion->count == 1U)
		return 0;

	/* The period and the delay (no more than a period, or MOTION_DELAY_LONGEST), the stroke's own once it has them. */
	interval = stroke_interval(motion, &measured);
	delay = stroke_delay(motion);
	longest = interval;
	if (longest < MOTION_DELAY_LONGEST)
		longest = MOTION_DELAY_LONGEST;
	if (delay > longest)
		delay = longest;

	/*
	 * Far enough behind the frame that the usual report needs no more than
	 * the allowed extrapolation.  Once the stroke has its own period the
	 * time behind moves slowly, so that the drawn point does not jump when
	 * the delay, measured in whole milliseconds, flips.
	 */
	wanted = interval + delay - (double)extrapolation_us;
	if (wanted < 0.0)
		wanted = 0.0;
	if (!motion->behind_settled) {
		motion->behind = wanted;
		if (measured)
			motion->behind_settled = 1;
	} else if (wanted > motion->behind + MOTION_BEHIND_STEP) {
		motion->behind += MOTION_BEHIND_STEP;
	} else if (wanted < motion->behind - MOTION_BEHIND_STEP) {
		motion->behind -= MOTION_BEHIND_STEP;
	} else {
		motion->behind = wanted;
	}

	/*
	 * The time evaluated, in seconds from the last report: not past the
	 * allowed extrapolation (half a period more for a late report), and
	 * not before the stroke's first report.
	 */
	target = seconds_between(now_us, last->stamp) - motion->behind / 1.0e6;
	limit = ((double)extrapolation_us + interval / 2.0) / 1.0e6;
	if (target > limit)
		target = limit;
	start = seconds_between(motion->reports[0].stamp, last->stamp);
	if (target < start)
		target = start;

	/* The samples, a report put back before each gap. */
	count = build_samples(motion, interval, 1, samples);

	/* A noisy device gets wider windows. */
	scale = 1.0;
	noise = device_noise(motion->device);
	if (noise > MOTION_NOISY)
		scale = MOTION_NOISY_SCALE;

	/* The line through its window. */
	window = MOTION_LINE_PERIODS * interval / 1.0e6;
	if (window < MOTION_LINE_SECONDS)
		window = MOTION_LINE_SECONDS;
	first = window_first(samples, count, window * scale, 2U);
	(void)fit(samples, first, count, 1, line_x, line_y);

	/* The parabola through its window. */
	window = MOTION_PARABOLA_PERIODS * interval / 1.0e6;
	if (window < MOTION_PARABOLA_SECONDS)
		window = MOTION_PARABOLA_SECONDS;
	first = window_first(samples, count, window * scale, 3U);
	(void)fit(samples, first, count, 2, parabola_x, parabola_y);

	/* Behind the last report the parabola follows the stroke, curves included. */
	point_x = parabola_x[0] + parabola_x[1] * target + parabola_x[2] * target * target;
	point_y = parabola_y[0] + parabola_y[1] * target + parabola_y[2] * target * target;
	if (target <= 0.0) {
		*x = point_x;
		*y = point_y;
		return 0;
	}

	/*
	 * Past it, how far each prediction goes along the line's direction:
	 * the parabola sees a finger slowing down, the line is steadier.
	 */
	advance_parabola = (point_x - parabola_x[0]) * line_x[1] + (point_y - parabola_y[0]) * line_y[1];
	advance_line = (line_x[1] * line_x[1] + line_y[1] * line_y[1]) * target;

	/* A parabola that turns back predicts nothing: the point stays at the last report's fit. */
	if (advance_parabola <= 0.0) {
		*x = parabola_x[0];
		*y = parabola_y[0];
		return 0;
	}

	/* Otherwise the prediction that goes less far. */
	if (advance_parabola < advance_line) {
		*x = point_x;
		*y = point_y;
	} else {
		*x = parabola_x[0] + line_x[1] * target;
		*y = parabola_y[0] + line_y[1] * target;
	}

	/* Succeeded: the point to draw. */
	return 0;
}

/*
 * Gives the velocity the finger had when it lifted.
 */
int
kl_motion_velocity(
	struct kl_motion *motion,
	uint64_t lift_us,
	double *vx,
	double *vy)
{
	struct motion_sample samples[MOTION_SAMPLES_MAX];
	const struct motion_report *last;
	double interval;
	double stale;
	double window;
	double threshold;
	double noise;
	double speed;
	double line_x[3];
	double line_y[3];
	unsigned count;
	unsigned first;
	int measured;

	/* Refuses a missing motion or result. */
	if (motion == NULL)
		return EINVAL;
	if (vx == NULL)
		return EINVAL;
	if (vy == NULL)
		return EINVAL;

	/* A stroke of fewer than two reports did not move. */
	*vx = 0.0;
	*vy = 0.0;
	if (motion->count < 2U)
		return 0;
	last = &motion->reports[motion->count - 1U];
	interval = stroke_interval(motion, &measured);

	/* A finger whose last report is long before the lift had stopped (a panel silent at rest). */
	stale = MOTION_STALE_PERIODS * interval;
	if (stale < MOTION_STALE_MIN)
		stale = MOTION_STALE_MIN;
	if (lift_us > last->stamp && (double)(lift_us - last->stamp) >= stale)
		return 0;

	/* The short line through the last periods. */
	window = MOTION_REST_PERIODS * interval / 1.0e6;
	if (window < MOTION_REST_SECONDS)
		window = MOTION_REST_SECONDS;
	count = build_samples(motion, interval, 1, samples);
	first = window_first(samples, count, window, 3U);
	(void)fit(samples, first, count, 1, line_x, line_y);
	speed = sqrt(line_x[1] * line_x[1] + line_y[1] * line_y[1]);

	/*
	 * A finger slower than the rest speed, or than four standard errors of
	 * the line's slope for the device's noise, was at rest.
	 */
	noise = device_noise(motion->device);
	threshold = MOTION_REST_ERRORS * noise * sqrt(2.0) / window;
	if (threshold < MOTION_REST_SPEED)
		threshold = MOTION_REST_SPEED;
	if (speed < threshold)
		return 0;

	/* The filter's velocity, no faster than the most reported. */
	*vx = motion->axes[0].velocity;
	*vy = motion->axes[1].velocity;
	speed = sqrt(*vx * *vx + *vy * *vy);
	if (speed > MOTION_SPEED_MAX) {
		*vx *= MOTION_SPEED_MAX / speed;
		*vy *= MOTION_SPEED_MAX / speed;
	}

	/* Succeeded: the velocity at the lift. */
	return 0;
}

/*
 * Ends a stroke and teaches its device.
 */
void
kl_motion_end(
	struct kl_motion *motion)
{
	struct kl_motion_device *device;
	double interval;
	double delay;
	double noise;
	int measured;

	/* Nothing to end. */
	if (motion == NULL)
		return;
	device = motion->device;

	/* Folds the stroke's own period into the device's. */
	interval = stroke_interval(motion, &measured);
	if (measured) {
		if (device->interval > 0.0) {
			device->interval += MOTION_FOLD_WEIGHT * (interval - device->interval);
		} else {
			device->interval = interval;
		}
	}

	/* And its delay, once it had enough reports to measure one. */
	if (motion->count >= MOTION_INTERVALS_MIN) {
		delay = stroke_delay(motion);
		if (device->delay > 0.0) {
			device->delay += MOTION_FOLD_WEIGHT * (delay - device->delay);
		} else {
			device->delay = delay;
		}
	}

	/* The stroke's noise is the median of its measurements; the device keeps the last strokes'. */
	if (motion->noise_count != 0U) {
		noise = median(motion->noise, motion->noise_count);
		device->noise[device->noise_next] = noise;
		device->noise_next = (device->noise_next + 1U) % MOTION_NOISE_STROKES;
		if (device->noise_count < MOTION_NOISE_STROKES)
			device->noise_count++;
	}

	/* The motion is empty for the next stroke. */
	kl_motion_begin(motion);
}

/* Measures the signed time from one microsecond count to a later one, in seconds. */
static double
seconds_between(
	uint64_t later,
	uint64_t earlier)
{
	int64_t difference;

	/* The difference as a signed count, then in seconds. */
	difference = (int64_t)(later - earlier);

	/* Succeeded: the time between them. */
	return (double)difference / 1.0e6;
}

/* Sorts a few values in place, smallest first (insertion sort: at most sixteen). */
static void
sort_values(
	double *values,
	unsigned count)
{
	double swap;
	unsigned i;
	unsigned j;

	/* Moves each value back past the larger ones before it. */
	for (i = 1U; i < count; i++) {
		for (j = i; j > 0U && values[j - 1U] > values[j]; j--) {
			swap = values[j];
			values[j] = values[j - 1U];
			values[j - 1U] = swap;
		}
	}
}

/* Finds the median of a few values, sorting them in place. */
static double
median(
	double *values,
	unsigned count)
{
	/* No values have no median. */
	if (count == 0U)
		return 0.0;

	/* Puts them in order. */
	sort_values(values, count);

	/* Succeeded: the middle value (the upper of two). */
	return values[count / 2U];
}

/*
 * Measures the stroke's report period in microseconds: the mean of its
 * last intervals that are not gaps.  The mean is not fooled by a slow
 * poll's aliases, as the median is.  Until the stroke has three such
 * intervals it is the device's period, and *measured says which.
 */
static double
stroke_interval(
	const struct kl_motion *motion,
	int *measured)
{
	double intervals[MOTION_HISTORY];
	double sorted[MOTION_HISTORY];
	double reference;
	double sum;
	unsigned count;
	unsigned used;
	unsigned index;

	/* Collects the last intervals, newest first. */
	count = 0U;
	for (index = motion->count; index > 1U && count < MOTION_HISTORY; index--) {
		intervals[count] = (double)(motion->reports[index - 1U].stamp - motion->reports[index - 2U].stamp);
		count++;
	}

	/* A gap is measured against the device's period, or the stroke's median. */
	reference = motion->device->interval;
	if (reference <= 0.0 && count != 0U) {
		memcpy(sorted, intervals, count * sizeof(intervals[0]));
		reference = median(sorted, count);
	}

	/* Reports at one time do not make every interval a gap. */
	if (reference < MOTION_INTERVAL_FLOOR)
		reference = MOTION_INTERVAL_FLOOR;

	/* Adds up the intervals that are not gaps. */
	sum = 0.0;
	used = 0U;
	for (index = 0U; index < count; index++) {
		if (intervals[index] > MOTION_GAP_PERIODS * reference)
			continue;
		sum += intervals[index];
		used++;
	}

	/* Too few of the stroke's own: the device's period, or the default. */
	if (used < MOTION_INTERVALS_MIN) {
		*measured = 0;
		if (motion->device->interval > 0.0)
			return motion->device->interval;
		return MOTION_DEFAULT_INTERVAL;
	}

	/* Succeeded: the stroke's mean period. */
	*measured = 1;
	return sum / (double)used;
}

/*
 * Measures how late the caller reads a report, in microseconds: the median
 * of the last reports' time from their stamp to their arrival.  With too
 * few reports it is the device's delay, or the default.
 */
static double
stroke_delay(
	const struct kl_motion *motion)
{
	const struct motion_report *report;
	double delays[MOTION_HISTORY];
	double delay;
	unsigned count;
	unsigned index;

	/* Too few reports: the device's delay, or the default. */
	if (motion->count < MOTION_INTERVALS_MIN) {
		if (motion->device->delay > 0.0)
			return motion->device->delay;
		return MOTION_DEFAULT_DELAY;
	}

	/* Collects the last delays; a report read before its stamp counts as no delay. */
	count = 0U;
	for (index = motion->count; index > 0U && count < MOTION_HISTORY; index--) {
		report = &motion->reports[index - 1U];
		delays[count] = 0.0;
		if (report->arrival > report->stamp)
			delays[count] = (double)(report->arrival - report->stamp);
		count++;
	}

	/* The middle one. */
	delay = median(delays, count);

	/* Succeeded: the median delay. */
	return delay;
}

/*
 * Gives the device's noise in pixels: the fourth smallest of its last
 * strokes' measurements, or the default until it has five.
 */
static double
device_noise(
	const struct kl_motion_device *device)
{
	double sorted[MOTION_NOISE_STROKES];
	double noise;

	/* Too few strokes to know. */
	if (device->noise_count < MOTION_NOISE_STROKES_MIN)
		return MOTION_DEFAULT_NOISE;

	/* The fourth smallest. */
	memcpy(sorted, device->noise, device->noise_count * sizeof(device->noise[0]));
	sort_values(sorted, device->noise_count);
	noise = sorted[MOTION_NOISE_RANK];

	/* Succeeded: the device's noise. */
	return noise;
}

/*
 * Turns the stroke's reports into samples, in seconds from the last
 * report.  When asked, a report is put back before each gap: a panel that
 * said nothing while the finger stayed still is taken to have had it where
 * it last said until one period before it spoke again.  Returns the number
 * of samples.
 */
static unsigned
build_samples(
	const struct kl_motion *motion,
	double interval,
	int fill_gaps,
	struct motion_sample *samples)
{
	const struct motion_report *last;
	const struct motion_report *report;
	const struct motion_report *previous;
	unsigned count;
	unsigned index;

	/* Every report, oldest first. */
	last = &motion->reports[motion->count - 1U];
	count = 0U;
	for (index = 0U; index < motion->count; index++) {
		report = &motion->reports[index];

		/* A gap before this report gets the finger put back where it was. */
		if (fill_gaps && index != 0U) {
			previous = &motion->reports[index - 1U];
			if ((double)(report->stamp - previous->stamp) > MOTION_GAP_PERIODS * interval) {
				samples[count].t = seconds_between(report->stamp, last->stamp) - interval / 1.0e6;
				samples[count].x = previous->x;
				samples[count].y = previous->y;
				count++;
			}
		}

		/* The report itself. */
		samples[count].t = seconds_between(report->stamp, last->stamp);
		samples[count].x = report->x;
		samples[count].y = report->y;
		count++;
	}

	/* Succeeded: the samples. */
	return count;
}

/*
 * Finds the first sample of a window before the last sample: the samples
 * within the window, and at least the minimum number of them.
 */
static unsigned
window_first(
	const struct motion_sample *samples,
	unsigned count,
	double window,
	unsigned minimum)
{
	unsigned first;

	/* Walks back while the samples are inside the window. */
	first = count - 1U;
	while (first > 0U && samples[count - 1U].t - samples[first - 1U].t <= window)
		first--;

	/* Takes more samples when the window holds too few. */
	if (count - first < minimum) {
		first = 0U;
		if (count > minimum)
			first = count - minimum;
	}

	/* Succeeded: the window's first sample. */
	return first;
}

/*
 * Fits a polynomial of a degree (0 to 2) to samples by least squares.
 *
 * The coefficients of x and y (constant, linear, quadratic in the sample
 * time) go to cx and cy.  Returns the degree fitted: lower when the
 * samples are too few or all at one time.
 */
static int
fit(
	const struct motion_sample *samples,
	unsigned first,
	unsigned count,
	int degree,
	double *cx,
	double *cy)
{
	double matrix[3][3];
	double bx[3];
	double by[3];
	double powers[5];
	double factor;
	double pivot;
	double magnitude;
	unsigned index;
	int fitted;
	int size;
	int i;
	int j;
	int k;

	/* A fit needs one sample more than its degree. */
	if (degree > (int)(count - first) - 1)
		degree = (int)(count - first) - 1;
	if (degree < 0)
		degree = 0;
	size = degree + 1;

	/* Sums the normal equations. */
	memset(matrix, 0, sizeof(matrix));
	memset(bx, 0, sizeof(bx));
	memset(by, 0, sizeof(by));
	for (index = first; index < count; index++) {
		powers[0] = 1.0;
		for (i = 1; i < 5; i++)
			powers[i] = powers[i - 1] * samples[index].t;
		for (i = 0; i < size; i++) {
			for (j = 0; j < size; j++)
				matrix[i][j] += powers[i + j];
			bx[i] += powers[i] * samples[index].x;
			by[i] += powers[i] * samples[index].y;
		}
	}

	/* Eliminates below the diagonal; a vanishing pivot fits one degree lower. */
	for (i = 0; i < size; i++) {
		pivot = matrix[i][i];
		magnitude = fabs(pivot);
		if (magnitude < 1.0e-18) {
			fitted = fit(samples, first, count, degree - 1, cx, cy);
			return fitted;
		}

		/* Subtracts this row from the rows below it. */
		for (j = i + 1; j < size; j++) {
			factor = matrix[j][i] / pivot;
			for (k = i; k < size; k++)
				matrix[j][k] -= factor * matrix[i][k];
			bx[j] -= factor * bx[i];
			by[j] -= factor * by[i];
		}
	}

	/* Substitutes back from the last coefficient. */
	for (i = size - 1; i >= 0; i--) {
		for (j = i + 1; j < size; j++) {
			bx[i] -= matrix[i][j] * cx[j];
			by[i] -= matrix[i][j] * cy[j];
		}

		/* What is left is this coefficient times the diagonal. */
		cx[i] = bx[i] / matrix[i][i];
		cy[i] = by[i] / matrix[i][i];
	}

	/* The coefficients above the degree are zero. */
	for (i = size; i < 3; i++) {
		cx[i] = 0.0;
		cy[i] = 0.0;
	}

	/* Succeeded: the degree fitted. */
	return degree;
}

/* Starts one axis of the velocity filter at a position, at rest, with a wide velocity. */
static void
filter_start(
	struct motion_axis *axis,
	double position)
{
	/* Certain of the position within 2 px, and of nothing about the velocity. */
	axis->position = position;
	axis->velocity = 0.0;
	axis->p00 = MOTION_FILTER_POSITION_START;
	axis->p01 = 0.0;
	axis->p11 = MOTION_FILTER_VELOCITY_START;
}

/*
 * Moves one axis of the velocity filter to a new report: predicts over dt
 * seconds with white acceleration, then corrects by the measurement with
 * the device's noise (never less than MOTION_FILTER_NOISE_MIN).
 */
static void
filter_update(
	struct motion_axis *axis,
	double dt,
	double measure,
	double noise)
{
	double q;
	double r;
	double p00;
	double p01;
	double p11;
	double innovation;
	double gain_position;
	double gain_velocity;
	double residual;

	/* A report at the same time or earlier predicts nothing. */
	if (dt < 0.0)
		dt = 0.0;

	/* Predicts the state and its uncertainty over dt. */
	q = MOTION_FILTER_ACCELERATION;
	axis->position += axis->velocity * dt;
	p00 = axis->p00 + dt * (2.0 * axis->p01 + dt * axis->p11) + q * dt * dt * dt / 3.0;
	p01 = axis->p01 + dt * axis->p11 + q * dt * dt / 2.0;
	p11 = axis->p11 + q * dt;

	/* The measurement noise: the device's, never less than the least. */
	r = noise * noise;
	if (r < MOTION_FILTER_NOISE_MIN)
		r = MOTION_FILTER_NOISE_MIN;

	/* Corrects by the report. */
	innovation = p00 + r;
	gain_position = p00 / innovation;
	gain_velocity = p01 / innovation;
	residual = measure - axis->position;
	axis->position += gain_position * residual;
	axis->velocity += gain_velocity * residual;
	axis->p00 = (1.0 - gain_position) * p00;
	axis->p01 = (1.0 - gain_position) * p01;
	axis->p11 = p11 - gain_velocity * p01;
}

/*
 * Measures the stroke's noise once: the rms distance of the reports of the
 * last 150 ms (at least six) from a parabola through them, corrected for
 * the three degrees of freedom the fit takes, per axis.
 */
static void
stroke_noise_measure(
	struct kl_motion *motion)
{
	struct motion_sample samples[MOTION_SAMPLES_MAX];
	double cx[3];
	double cy[3];
	double dx;
	double dy;
	double sum;
	unsigned count;
	unsigned first;
	unsigned index;

	/* The window needs enough reports to leave residuals. */
	if (motion->count < MOTION_NOISE_REPORTS)
		return;
	count = build_samples(motion, 0.0, 0, samples);
	first = window_first(samples, count, MOTION_NOISE_SECONDS, MOTION_NOISE_REPORTS);

	/* The parabola through the window. */
	(void)fit(samples, first, count, 2, cx, cy);

	/* Adds up the squared distances. */
	sum = 0.0;
	for (index = first; index < count; index++) {
		dx = samples[index].x - (cx[0] + cx[1] * samples[index].t + cx[2] * samples[index].t * samples[index].t);
		dy = samples[index].y - (cy[0] + cy[1] * samples[index].t + cy[2] * samples[index].t * samples[index].t);
		sum += dx * dx + dy * dy;
	}

	/* Keeps the measurement in the stroke's ring. */
	motion->noise[motion->noise_next] = sqrt(sum / (2.0 * (double)(count - first - 3U)));
	motion->noise_next = (motion->noise_next + 1U) % MOTION_NOISE_SAMPLES;
	if (motion->noise_count < MOTION_NOISE_SAMPLES)
		motion->noise_count++;
}

/*
 * Starts the Scan Time mapping at a report: the report maps onto its own
 * host time, and the check window starts empty.  Whether the Scan Time is
 * trusted carries over (a pause does not change the panel).
 */
static void
clock_restart(
	struct kl_motion_device *device,
	uint64_t host_us,
	uint32_t device_us)
{
	/* The unwrapped device time starts at the raw value. */
	device->clock_started = 1;
	device->clock_raw = device_us;
	device->clock_device = device_us;
	device->clock_host = host_us;
	device->clock_offset = (int64_t)host_us - (int64_t)device_us;

	/* The stamps never go back, even across a restart. */
	if (host_us > device->clock_stamp)
		device->clock_stamp = host_us;

	/* The check starts again. */
	device->check_device = 0;
	device->check_host = 0;
}

/*
 * Adds one step to the check of the Scan Time: over each half second of
 * host time, the Scan Time must advance by 0.9 to 1.1 times as much.  A
 * panel whose Scan Time stands still, or that was given the wrong unit,
 * falls back to the host time.
 */
static void
clock_check(
	struct kl_motion_device *device,
	uint64_t device_step,
	uint64_t host_step)
{
	double rate;

	/* Adds the step to the window. */
	device->check_device += device_step;
	device->check_host += host_step;
	if (device->check_host < MOTION_CLOCK_CHECK)
		return;

	/* Judges the window and starts the next. */
	rate = (double)device->check_device / (double)device->check_host;
	device->clock_trusted = 0;
	if (rate >= MOTION_CLOCK_RATE_LOW && rate <= MOTION_CLOCK_RATE_HIGH)
		device->clock_trusted = 1;
	device->check_device = 0;
	device->check_host = 0;
}
