/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's clock and reset generators (SYS, STG, AON and VOUT).
 *
 * A device's node names its clocks and resets by the phandle of a
 * generator and the generator's number, as the clocks and resets
 * properties do.  The calls here look those up and switch the gates, set
 * the dividers and move the resets of the clocks and resets the board's
 * drivers use.  A clock the table does not know is refused, so a driver
 * cannot stop a clock it does not own, in particular the CPUs', the
 * memory's or the console's.  The display's bus clocks in the SYS
 * generator are known but shared, so they are never stopped from here.
 *
 * Rates are computed from the PLL settings in the SYS system control
 * block and the dividers and multiplexers on the way; the PLLs are only
 * read, never changed.
 */

#ifndef KERN_DRIVERS_VF2_CRG_H
#define KERN_DRIVERS_VF2_CRG_H

#include <stdint.h>

#include <drivers/generic/fdt.h>

/*
 * One of the four generators.
 *
 * The VOUT generator sits in the display's power domain; its registers
 * answer only while that domain is on.
 *
 * The numbers index the generators' records; COUNT is not a generator.
 */
enum drv_vf2_crg {
	DRV_VF2_CRG_SYS = 0,
	DRV_VF2_CRG_STG = 1,
	DRV_VF2_CRG_AON = 2,
	DRV_VF2_CRG_VOUT = 3,
	DRV_VF2_CRG_COUNT = 4
};

/*
 * One clock: its generator and the generator's number for it.
 *
 * The number is the one the device tree's clocks property gives.
 */
struct drv_vf2_clock {
	enum drv_vf2_crg crg;
	uint32_t index;
};

/*
 * One reset line: its generator and the generator's number for it.
 *
 * The number is the one the device tree's resets property gives.
 */
struct drv_vf2_reset {
	enum drv_vf2_crg crg;
	uint32_t index;
};

/*
 * Finds the four generators in the device tree and maps them.
 *
 * Touches no register.  drv_vf2_syscon_init() must have run, for the PLL
 * settings.  Reports ENODEV when the SYS generator is missing.
 */
int drv_vf2_crg_init(const struct drv_fdt *fdt);

/*
 * Finds a clock of a device by its clock-names entry.
 *
 * A NULL name takes the first entry of the clocks property.  ENOENT means
 * the device lacks the clock; ENODEV that its phandle names no generator.
 */
int drv_vf2_clock_lookup(const struct drv_fdt *fdt, uint32_t node, const char *name, struct drv_vf2_clock *clock);

/*
 * Starts a clock and every gate above it on its current path.
 *
 * A gate already open is left alone, so this may be repeated.
 */
int drv_vf2_clock_enable(const struct drv_vf2_clock *clock);

/*
 * Stops a clock's own gate.
 *
 * The gates above it stay open.  A bus clock other devices share is
 * refused with EBUSY.
 */
int drv_vf2_clock_disable(const struct drv_vf2_clock *clock);

/*
 * Reports whether a clock's own gate is open.
 *
 * Only reads.  A clock without a gate of its own reports running.  ENODEV
 * means its generator is absent; a generator in a powered-down domain
 * must not be asked.
 */
int drv_vf2_clock_running(const struct drv_vf2_clock *clock, int *running);

/*
 * Computes a clock's rate in hertz from the registers.
 *
 * ENOTSUP means the table does not know the clock's source.
 */
int drv_vf2_clock_get_rate(const struct drv_vf2_clock *clock, uint64_t *rate);

/*
 * Computes the rate in hertz of one of the three PLLs (0 to 2).
 *
 * A powered-down PLL reports zero.
 */
int drv_vf2_pll_get_rate(unsigned pll, uint64_t *rate);

/*
 * Sets a clock's divider so that its rate is as close as possible to the
 * requested one without exceeding it.
 *
 * Only clocks whose divider a driver may own accept this; others report
 * ENOTSUP.  ERANGE means even the largest divider exceeds the rate.
 */
int drv_vf2_clock_set_rate(const struct drv_vf2_clock *clock, uint64_t rate);

/*
 * Applies a device's assigned-clocks and assigned-clock-rates.
 *
 * A rate of zero leaves its clock alone.  ENOENT means the device has no
 * assigned clocks.
 */
int drv_vf2_clock_apply_assigned(const struct drv_fdt *fdt, uint32_t node);

/*
 * Makes a multiplexed clock run from one of its sources.
 *
 * ENOTSUP means the clock has no selector or other devices share it;
 * ENOENT that the parent is not one of its sources.
 */
int drv_vf2_clock_set_parent(const struct drv_vf2_clock *clock, const struct drv_vf2_clock *parent);

/*
 * Makes a multiplexed clock run from an input of its generator, a clock
 * from outside the generators named by the generator node's clock-names
 * (for example the HDMI transmitter's hdmitx0_pixelclk).
 *
 * ENOTSUP means the clock has no selector or other devices share it;
 * ENOENT that the input is not one of its sources.
 */
int drv_vf2_clock_set_input_parent(const struct drv_vf2_clock *clock, const char *input);

/*
 * Makes a multiplexed clock run from one of the three PLLs (0 to 2).
 *
 * ENOTSUP means the clock has no selector or other devices share it;
 * ENOENT that the PLL is not one of its sources.
 */
int drv_vf2_clock_set_pll(const struct drv_vf2_clock *clock, unsigned pll);

/*
 * Applies a device's assigned-clocks and assigned-clock-parents.
 *
 * ENOENT means the device has no assigned parents.
 */
int drv_vf2_clock_apply_assigned_parents(const struct drv_fdt *fdt, uint32_t node);

/*
 * Finds a reset line of a device by its reset-names entry.
 *
 * A NULL name takes the first entry of the resets property.
 */
int drv_vf2_reset_lookup(const struct drv_fdt *fdt, uint32_t node, const char *name, struct drv_vf2_reset *reset);

/*
 * Finds a reset line of a device by its position in the resets property,
 * for a device whose node names none of them.  ENOENT means the device
 * has fewer lines.
 */
int drv_vf2_reset_lookup_index(const struct drv_fdt *fdt, uint32_t node, unsigned position, struct drv_vf2_reset *reset);

/*
 * Holds a device in reset.
 */
int drv_vf2_reset_assert(const struct drv_vf2_reset *reset);

/*
 * Releases a device from reset and waits until the generator reports it
 * released.
 *
 * ETIMEDOUT means the status did not follow, usually because the
 * device's clock is stopped.
 */
int drv_vf2_reset_deassert(const struct drv_vf2_reset *reset);

/*
 * Reports whether the generator's status shows a line released.
 */
int drv_vf2_reset_released(const struct drv_vf2_reset *reset, int *released);

#endif
