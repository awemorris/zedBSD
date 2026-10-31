/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The host test's stand-in for the kernel's platform header: only the power-off and the halt. */

#ifndef KERN_KERN_PLATFORM_H
#define KERN_KERN_PLATFORM_H

void
kern_platform_halt(void) __attribute__((noreturn));

int
kern_platform_poweroff(void);

int
kern_platform_poweroff_emergency(void) __attribute__((weak));

#endif
