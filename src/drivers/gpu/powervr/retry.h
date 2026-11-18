/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A bounded retry of a step that may report EBUSY.
 *
 * Registering a device can collide with another registration or removal
 * and is then refused with EBUSY; the collision is short-lived, so the
 * step is tried again a bounded number of times with a pause between.
 * Any other answer ends the retry at once.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RETRY_H
#define KERN_DRIVERS_GPU_POWERVR_RETRY_H

/*
 * One step to retry, and the pause between attempts.
 *
 * Both are called with the same context.
 */
struct powervr_retry {
	int (*attempt)(void *context);
	void (*pause)(void *context);
	void *context;
};

/*
 * Runs a step until it stops reporting EBUSY or the attempts are spent.
 *
 * Returns the step's last answer: 0, an error other than EBUSY, or EBUSY
 * when every one of the attempts was refused.  *tries reports how many
 * attempts were made.
 */
int powervr_retry_busy(const struct powervr_retry *retry, unsigned attempts, unsigned *tries);

#endif
