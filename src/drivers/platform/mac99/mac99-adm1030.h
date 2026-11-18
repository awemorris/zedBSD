/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fan controller of the 12-inch PowerBook G4: an Analog Devices
 * ADM1030 on an I2C bus, which measures its own temperature and one
 * remote diode's and drives one fan.  This part knows the chip and
 * nothing of the bus: it reaches the chip's registers through the
 * functions it is given.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_ADM1030_H
#define KERN_DRIVERS_PLATFORM_MAC99_ADM1030_H

#include <stdbool.h>
#include <stdint.h>

/* The temperature channels, in the order they are reported: the chip's own, then the remote diode. */
#define DRV_ADM1030_LOCAL		0U
#define DRV_ADM1030_REMOTE		1U
#define DRV_ADM1030_CHANNELS		2U

/* The most registers one set-up changes: a TMIN a channel and the configuration. */
#define DRV_ADM1030_CHANGES_MAX		(DRV_ADM1030_CHANNELS + 1U)

/*
 * How a set-up ended: what it needed is written (possibly nothing), or
 * why it wrote nothing more: the fan is under software control,
 * monitoring is off, a temperature is not one a running machine has, the
 * chip did not answer, or a register did not keep what was written.
 */
enum drv_adm1030_outcome {
	DRV_ADM1030_SET_UP,
	DRV_ADM1030_SOFTWARE,
	DRV_ADM1030_NOT_MONITORING,
	DRV_ADM1030_IMPLAUSIBLE,
	DRV_ADM1030_UNREADABLE,
	DRV_ADM1030_NOT_KEPT
};

/*
 * How the driver reaches the chip's registers: a read and a write of one
 * register, each reporting 0 or an errno, and their argument.  A driver
 * that only reads may leave the write NULL.
 */
struct drv_adm1030_access {
	int (*read)(void *context, uint8_t number, uint8_t *value);
	int (*write)(void *context, uint8_t number, uint8_t value);
	void *context;
};

/* One register a set-up wrote: its number, and its value before and after. */
struct drv_adm1030_change {
	uint8_t number;
	uint8_t before;
	uint8_t after;
};

/* What a set-up did: how it ended, and the registers it wrote, in order. */
struct drv_adm1030_report {
	enum drv_adm1030_outcome outcome;
	unsigned change_count;
	struct drv_adm1030_change changes[DRV_ADM1030_CHANGES_MAX];
};

/* One chip the driver identified: how to reach it, and its revision register (bit 7: THERM runs the fan at full speed). */
struct drv_adm1030 {
	struct drv_adm1030_access access;
	uint8_t revision;
};

/*
 * What decides the fan, as read from the chip (the status registers are
 * not read: reading them clears them): the two configuration registers,
 * each channel's temperature in degrees Celsius, its offset, its high,
 * low and THERM limits, its TMIN/TRANGE register, the fan's tachometer,
 * its tachometer limit, its characteristics, speed and filter registers,
 * and the THERM behaviour and revision register.
 */
struct drv_adm1030_settings {
	uint8_t configuration1;
	uint8_t configuration2;
	int32_t temperature[DRV_ADM1030_CHANNELS];
	int32_t offset[DRV_ADM1030_CHANNELS];
	int32_t high[DRV_ADM1030_CHANNELS];
	int32_t low[DRV_ADM1030_CHANNELS];
	int32_t therm[DRV_ADM1030_CHANNELS];
	uint8_t automatic[DRV_ADM1030_CHANNELS];
	uint8_t tachometer;
	uint8_t tachometer_limit;
	uint8_t characteristics;
	uint8_t speed;
	uint8_t filter;
	uint8_t behaviour;
};

/*
 * Identifies the chip by its company and device ids.  Writes nothing.
 * Returns 0, ENODEV for another chip, EINVAL for a missing way to it, or
 * the bus's errno.
 */
int drv_adm1030_identify(const struct drv_adm1030_access *access, struct drv_adm1030 *device);

/*
 * Lets the chip's automatic control start the fan earlier, by writes that
 * can only make the fan turn faster, never slower, at every temperature:
 * a channel's TMIN is lowered to the driver's when it is higher (its
 * TRANGE kept), and a fan that follows the remote channel alone follows
 * the faster of both.  The minimum speed, the THERM limits and the other
 * configuration are left as they are.  Nothing is written while the fan
 * is under software control, while monitoring is off, or while a
 * temperature is out of a running machine's range; a write the register
 * does not keep ends the set-up.  Returns 0 with the report, EINVAL
 * without a write, or the bus's errno.
 */
int drv_adm1030_set_up(const struct drv_adm1030 *device, struct drv_adm1030_report *report);

/* Reads what decides the fan.  Returns 0 or the bus's errno. */
int drv_adm1030_read_settings(const struct drv_adm1030 *device, struct drv_adm1030_settings *settings);

/*
 * Reads the two temperatures in millidegrees Celsius, with the extended
 * resolution's bits.  Returns 0 or the bus's errno.
 */
int drv_adm1030_read_temperatures(const struct drv_adm1030 *device, int32_t *millicelsius);

/*
 * Reads the fan's speed in revolutions a minute (0 when it stands still
 * or is not measured).  Returns 0 or the bus's errno.
 */
int drv_adm1030_read_fan(const struct drv_adm1030 *device, uint32_t *rpm);

#endif
