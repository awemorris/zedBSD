/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI discovery entry for the Intel i915 native GPU backend.
 */

#ifndef DRIVERS_I915_H
#define DRIVERS_I915_H

int
drv_pci_i915_driver_register(void);

/*
 * Reports that the kernel can run the device bring-up.
 *
 * Called once from the boot path after regular threads, timer wakeups and
 * the VFS are up.  Devices that attached before it start now; later ones
 * start as they attach.  Returns without waiting.
 */
void
drv_i915_runtime_ready(void);

#endif
