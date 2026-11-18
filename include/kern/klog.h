/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_KLOG_H
#define KERN_KERN_KLOG_H

#include <stddef.h>
#include <stdint.h>

void
kern_log_init(void);

void
kern_log_write(
	const char *bytes,
	size_t length);

void
kern_logf(
	const char *format,
	...)
__attribute__((format(printf, 1, 2)));

size_t
kern_log_snapshot(
	char *buffer,
	size_t capacity,
	uint64_t *dropped);

size_t
kern_log_capacity(void);

/*
 * The quiet log (the boot parameter kmsg=quiet): records go to
 * the ring (dmesg) and the platform debug port, not to the console.
 */
void
kern_log_set_quiet(
	int quiet);

int
kern_log_quiet(void);

/*
 * Keeps the text of a fatal stop in the ring only: no console is
 * written, and the ring's lock is taken only when this CPU does not already
 * hold it and another CPU gives it up within a bound.  Called with
 * interrupts disabled on the way to a stop.
 */
void
kern_log_record_fatal(
	const char *text);

/*
 * Logs one step of the boot with the milliseconds since the kernel's clock
 * started ("boot: at N ms: STEP"), so that dmesg shows where a boot spends
 * its time, also when the log is quiet.
 */
void
kern_log_boot_step(
	const char *step);

#endif
