/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_KERN_KERNEL_H
#define BOOTS_KERN_KERNEL_H

#include "kern/boot.h"

void kernel_main(const struct boots_handoff *handoff,
		 const struct boots_device *devices, unsigned device_count);

#endif
