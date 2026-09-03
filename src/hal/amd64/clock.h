/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 PC/AT scheduler-clock contract.
 */

#ifndef ZEDBSD_HAL_AMD64_CLOCK_H
#define ZEDBSD_HAL_AMD64_CLOCK_H

int
bsp_timer_init(void);

void
clock_handler(void);

#endif
