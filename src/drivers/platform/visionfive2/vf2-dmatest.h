/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/vf2-dmatest: the board's DMA coherency check on the AXI DMA
 * controller, for the test kernel only.
 *
 * Reading the device from its start runs the check once and reads its
 * answer as text lines (vf2-dmatest-core.h).
 */

#ifndef KERN_DRIVERS_VF2_DMATEST_H
#define KERN_DRIVERS_VF2_DMATEST_H

#include <drivers/generic/fdt.h>

/*
 * Brings up the AXI DMA controller the tree names and publishes the
 * device.  Called once during the board's single-threaded bring-up; the
 * tree must stay valid for good.
 */
int drv_vf2_dmatest_publish(const struct drv_fdt *fdt);

#endif
