/*
 * Kernel main
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_KERNEL_H
#define ZEDBSD_KERN_KERNEL_H

#include "kern/boot.h"

void kernel_main(const struct zedbsd_handoff *handoff,
		 const struct zedbsd_device *devices, unsigned device_count);

#endif
