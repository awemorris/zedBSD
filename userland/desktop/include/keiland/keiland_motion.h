/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The touch motion, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_MOTION_H
#define KEILAND_MOTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The touch motion.
 *
 * A cheap touch screen reports 30 to 60 times a second, unevenly, and with
 * noise.  Drawing the last report at each frame makes a scroll or a dragged
 * window judder: some frames get no report and stand still, the next jumps.
 * A motion instead fits a line and a parabola to the contact's last reports
 * and evaluates them a little behind the frame's time, so that the point
 * advances smoothly with the frames and is never extrapolated far past the
 * last report.
 *
 * A device (one touch screen in the compositor, one seat's touch in a client)
 * keeps what is learned across strokes: the report period, how late
 * reports arrive, the noise, and the mapping of the panel's Scan Time
 * (evdev MSC_TIMESTAMP) onto the host clock.  A motion is one contact's
 * stroke from touch-down to lift.  Neither is shared between threads.
 *
 * Times are CLOCK_MONOTONIC microseconds; positions are logical pixels.
 * The compositor's wl_touch times are the same clock's milliseconds (the low 32
 * bits), from the time the panel scanned the report when it has a Scan
 * Time, so a client can compare them with its own clock.  Calls that can
 * fail return 0 or an errno value.
 */

/* How far past the last report content that follows a finger may be extrapolated (12 ms). */
#define KL_MOTION_EXTRAPOLATION_CONTENT	12000U

/* How far past the last report the provisional tail of a drawn line may be extrapolated (16 ms). */
#define KL_MOTION_EXTRAPOLATION_INK	16000U

/* A device's timing and noise, learned across strokes (opaque). */
struct kl_motion_device;

/* One contact's stroke (opaque). */
struct kl_motion;

/*
 * Creates a device, knowing nothing of it yet.
 *
 * Returns NULL when memory is short.
 */
struct kl_motion_device *kl_motion_device_create(void);

/*
 * Destroys a device.  Its motions must be destroyed first.
 */
void kl_motion_device_destroy(struct kl_motion_device *device);

/*
 * Turns a report's host time and its panel's Scan Time into the time the
 * panel scanned it, on the host clock.
 *
 * host_us is the evdev time of the report; device_us is its MSC_TIMESTAMP
 * (microseconds, wrapping at 2^32, restarting at 0 after a pause).  The
 * result is never later than host_us and never goes back.  While the Scan
 * Time has not proved itself (it must advance at the host's rate, within
 * 10%, over half a second) the result is host_us.  A device without Scan
 * Time does not call this and uses the host time.
 */
int kl_motion_device_time(struct kl_motion_device *device, uint64_t host_us, uint32_t device_us,
    uint64_t *stamp_us);

/* Reports the device's measured report period in microseconds (0: not measured yet). */
uint32_t kl_motion_device_interval(const struct kl_motion_device *device);

/* Reports the device's measured noise in pixels (the default until five strokes have been seen). */
double kl_motion_device_noise(const struct kl_motion_device *device);

/*
 * Creates a motion for contacts of a device.
 *
 * Returns NULL for a missing device or when memory is short.
 */
struct kl_motion *kl_motion_create(struct kl_motion_device *device);

/* Destroys a motion. */
void kl_motion_destroy(struct kl_motion *motion);

/*
 * Starts a stroke: a finger touched.  Whatever the motion held is
 * forgotten, without teaching the device.
 */
void kl_motion_begin(struct kl_motion *motion);

/*
 * Adds one report of the stroke.
 *
 * stamp_us is when the finger was there (the evdev time, or the result of
 * kl_motion_device_time); arrival_us is when the caller read the
 * report, on the same clock.  A report older than the last one is refused
 * with EINVAL.  After a second without reports the older ones are dropped.
 */
int kl_motion_add(struct kl_motion *motion, uint64_t stamp_us, uint64_t arrival_us, double x, double y);

/*
 * Gives the point to draw at a frame.
 *
 * now_us is the time the frame is being drawn; extrapolation_us is how far
 * past the last report the point may be predicted
 * (KL_MOTION_EXTRAPOLATION_CONTENT or _INK).  Call it once a frame for
 * each contact: the time it evaluates the stroke at moves at most half a
 * millisecond a call.  Returns ENOENT before the first report.
 */
int kl_motion_point(struct kl_motion *motion, uint64_t now_us, uint32_t extrapolation_us, double *x,
    double *y);

/*
 * Gives the velocity (pixels a second) the finger had when it lifted.
 *
 * lift_us is the time of the lift.  The velocity is zero when the finger
 * had come to rest, and when its last report is older than the lift by
 * more than two periods (50 ms at least).  It is at most 8000 px/s.
 * Whether it starts a fling is the caller's decision.
 */
int kl_motion_velocity(struct kl_motion *motion, uint64_t lift_us, double *vx, double *vy);

/*
 * Ends a stroke: the finger lifted.  What the stroke measured (period,
 * delay, noise) is folded into the device.
 */
void kl_motion_end(struct kl_motion *motion);

#ifdef __cplusplus
}
#endif

#endif
