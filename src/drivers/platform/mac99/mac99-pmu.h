/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's power manager (PMU) as the kernel uses it: the
 * keyboard and the mouse on its ADB.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_PMU_H
#define KERN_DRIVERS_PLATFORM_MAC99_PMU_H

/*
 * Publishes the ADB keyboard and mouse behind the PMU the firmware
 * described as input devices, and starts reading them.  Reports ENODEV
 * when the board has no PMU.
 */
int
drv_mac99_pmu_init(void);

#endif
