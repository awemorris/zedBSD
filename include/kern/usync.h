/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_USYNC_H
#define ZEDBSD_KERN_USYNC_H

#include <stdint.h>

void
usync_init(void);

/*
 * Wake absolute CLOCK_REALTIME waiters after the realtime offset changes.
 * Waiters revalidate both their user-space predicate and absolute deadline.
 */
void
usync_realtime_changed(void);

int
usync_wait(
	uintptr_t address,
	uint32_t expected,
	uintptr_t process_key,
	uintptr_t key_offset,
	uint64_t deadline,
	int cancelable);

int
usync_wake(
	uintptr_t address,
	uintptr_t process_key,
	uintptr_t key_offset,
	unsigned count);

#endif
