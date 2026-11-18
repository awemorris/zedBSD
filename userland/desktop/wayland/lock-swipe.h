/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lock screen's unlocking moves and its grace (lock-swipe.c,
 * ws187-p002): a swipe up from the lower part of the output (a finger, or
 * the pointer dragged), two fingers up on a touch pad, or the wheel turned
 * up; and whether a lock is recent enough to open on such a move alone.
 *
 * It knows nothing of the server: the caller hands it the places, the
 * travel, the wheel's notches and the times, and acts on what it says
 * (greeter.c).  So the host tests run it alone.  The places are in logical
 * pixels.
 */

#ifndef KWL_LOCK_SWIPE_H
#define KWL_LOCK_SWIPE_H

#include <stdint.h>

/*
 * The moves being followed: a press in the lower part (its place, and how
 * far it has gone up and across since), the wheel's notches turned up and
 * when the last came, and two fingers' travel up on a touch pad (and
 * whether this touch has unlocked already).
 */
struct kwl_lock_swipe {
	unsigned pressing;
	int32_t start_x;
	int32_t start_y;
	int32_t up;
	int32_t across;
	unsigned wheel_notches;
	uint64_t wheel_ms;
	int64_t pad_up_um;
	unsigned pad_spent;
};

void kwl_lock_swipe_reset(struct kwl_lock_swipe *swipe);
int kwl_lock_swipe_press(struct kwl_lock_swipe *swipe, int32_t x, int32_t y, int32_t height);
void kwl_lock_swipe_motion(struct kwl_lock_swipe *swipe, int32_t x, int32_t y);
int kwl_lock_swipe_release(struct kwl_lock_swipe *swipe, int32_t height);
int32_t kwl_lock_swipe_distance(int32_t height);
int kwl_lock_swipe_wheel(struct kwl_lock_swipe *swipe, int32_t vertical, uint64_t now_ms);
int kwl_lock_swipe_pad(struct kwl_lock_swipe *swipe, int64_t down_um);
int kwl_lock_swipe_pad_gesture(struct kwl_lock_swipe *swipe, int64_t travel_um);
void kwl_lock_swipe_pad_end(struct kwl_lock_swipe *swipe);
int kwl_lock_reason_manual(const char *reason);
int kwl_lock_grace(unsigned manual, int64_t locked_at, int64_t now, int64_t grace);

#endif
