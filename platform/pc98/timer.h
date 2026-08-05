/*
 * Boots PC-98 polled interval timer
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_PC98_TIMER_H
#define BOOTS_PC98_TIMER_H

#include <stdint.h>

/*
 * Millisecond clock for the BeUI clock HAL.  Time is derived from polled
 * i8253 counter deltas, so the value only advances while callers keep
 * polling: at least one call per counter period (26.7ms on 2.4576MHz
 * systems, 32.8ms on 1.9968MHz systems) is required or elapsed time is
 * lost.  The first call programs system timer channel 0 and returns 0.
 */
uint64_t boots_pc98_timer_milliseconds(void *context);

#endif
