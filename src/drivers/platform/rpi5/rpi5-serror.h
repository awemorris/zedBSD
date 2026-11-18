/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A check, between the Pi 5 drivers' start-up steps, of whether a device
 * access of the step left an SError pending.
 *
 * An access no device answers raises an asynchronous SError some time
 * after the access.  Kernel code that runs with SErrors masked (a system
 * call, an interrupt) holds it until the next return to user mode, where
 * the HAL reports it without saying which access raised it.  The check
 * waits for the step's accesses to finish, looks at the pending bit, names
 * the step in the log, and lets the HAL take the SError at once so that
 * the next check starts clean.
 */

#ifndef KERN_DRIVERS_PLATFORM_RPI5_SERROR_H
#define KERN_DRIVERS_PLATFORM_RPI5_SERROR_H

/*
 * Reports whether an SError is pending after a step, logging the step's
 * name when one is.
 */
int drv_rpi5_serror_check(const char *step);

#endif /* KERN_DRIVERS_PLATFORM_RPI5_SERROR_H */
