/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's property mailbox, which the HAL asks for the framebuffer.
 *
 * Each board has its own mailbox.c: the VideoCore sees the ARM's memory at
 * a different bus address on each.
 */

#ifndef KERN_HAL_ARM64_MAILBOX_H
#define KERN_HAL_ARM64_MAILBOX_H

#include <hal/types.h>

int rpi_mailbox_property(uintptr_t mailbox_phys, uint32_t *message, size_t bytes);

#endif
