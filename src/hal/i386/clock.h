/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board-provided i386 interval-timer contract.
 */

#ifndef _SYS_ARCH_X86_CLOCK_H_
#define _SYS_ARCH_X86_CLOCK_H_

#include <hal/hal.h>

void bsp_timer_init(void);
void clock_handler(void);	/* tick, from irq.c */

#endif
