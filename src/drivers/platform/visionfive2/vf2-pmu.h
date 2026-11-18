/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's power management unit.
 *
 * The unit switches the SoC's power domains.  Only the GPU's outer domain,
 * the display's domain and the video decoder's and encoder's domains are
 * switched from here, each by its own driver; every other domain (the
 * CPUs', the camera's) stays as the firmware left it.
 */

#ifndef KERN_DRIVERS_VF2_PMU_H
#define KERN_DRIVERS_VF2_PMU_H

#include <drivers/generic/fdt.h>

/*
 * The GPU's outer power domain, as the device tree's power-domains
 * property numbers it.  The inner domain is the GPU's own firmware's.
 *
 * Value referenced:
 * Linux v6.12 include/dt-bindings/power/starfive,jh7110-pmu.h
 * (JH7110_PD_GPUA 2).
 */
#define DRV_VF2_PD_GPU	2U

/*
 * The display's power domain (the display controller, the HDMI and DSI
 * transmitters and the VOUT clock generator), as the device tree's
 * power-domains property numbers it.
 *
 * Value referenced:
 * Linux v6.12 include/dt-bindings/power/starfive,jh7110-pmu.h
 * (JH7110_PD_VOUT 4).
 */
#define DRV_VF2_PD_VOUT	4U

/*
 * The video decoder's and the video encoder's power domains, as the
 * device tree's power-domains property numbers them.
 *
 * Values referenced:
 * Linux v6.12 include/dt-bindings/power/starfive,jh7110-pmu.h
 * (JH7110_PD_VDEC 3, JH7110_PD_VENC 6).
 */
#define DRV_VF2_PD_VDEC	3U
#define DRV_VF2_PD_VENC	6U

/*
 * Finds the power management unit in the device tree and maps it.
 *
 * Touches no register.  ENODEV means the tree has no unit.
 */
int drv_vf2_pmu_init(const struct drv_fdt *fdt);

/*
 * Turns a power domain on or off and waits until the unit reports it so.
 *
 * Only DRV_VF2_PD_GPU, DRV_VF2_PD_VOUT, DRV_VF2_PD_VDEC and
 * DRV_VF2_PD_VENC are accepted (EPERM for the others).  ETIMEDOUT means
 * the unit's state did not follow.
 */
int drv_vf2_pmu_set_power(unsigned domain, int on);

/*
 * Reports whether a power domain is on.
 */
int drv_vf2_pmu_powered(unsigned domain, int *on);

/* Sets the hardware event turn-off mask (a set bit keeps that domain's event from turning it off). */
int drv_vf2_pmu_event_off_mask(uint32_t mask);

#endif
