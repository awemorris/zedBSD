/*
 * Kernel main
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_KERNEL_H
#define ZEDBSD_KERN_KERNEL_H

#include "kern/boot.h"

void kernel_main(const struct boot_handoff *handoff,
		 const struct boot_device *devices, unsigned device_count);

#endif
