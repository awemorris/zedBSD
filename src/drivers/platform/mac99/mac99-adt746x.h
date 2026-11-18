/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fan controller of the PowerBook G4 and the iBook G4: an Analog
 * Devices ADT7460 or ADT7467 on an I2C bus, which measures three
 * temperatures and drives the fans by itself.  This part knows the chip
 * and nothing of the bus: it reads and writes the chip's registers through
 * the functions it is given.
 */

#ifndef KERN_DRIVERS_PLATFORM_MAC99_ADT746X_H
#define KERN_DRIVERS_PLATFORM_MAC99_ADT746X_H

#include <stdbool.h>
#include <stdint.h>

/* The chips, as the board's firmware names them. */
#define DRV_ADT746X_ADT7460		1U
#define DRV_ADT746X_ADT7467		2U

/* The temperature channels, in the order they are reported: the chip's own, then the two remote diodes. */
#define DRV_ADT746X_LOCAL		0U
#define DRV_ADT746X_REMOTE1		1U
#define DRV_ADT746X_REMOTE2		2U
#define DRV_ADT746X_CHANNELS		3U

/* The most fans the driver reports. */
#define DRV_ADT746X_FANS_MAX		2U

/* The most registers one set-up changes: two limits a channel and a behaviour a fan. */
#define DRV_ADT746X_CHANGES_MAX		(DRV_ADT746X_CHANNELS * 2U + DRV_ADT746X_FANS_MAX)

/* A temperature the chip could not measure (a broken or missing diode). */
#define DRV_ADT746X_NO_READING		(-1000)

/*
 * How a set-up ended: what it needed is written (possibly nothing), or
 * why it wrote nothing more: monitoring is off, the limits are locked, a
 * temperature could not be read or is not one a running machine has, the
 * chip did not answer, or a register did not keep what was written.
 */
enum drv_adt746x_outcome {
	DRV_ADT746X_SET_UP,
	DRV_ADT746X_NOT_STARTED,
	DRV_ADT746X_LOCKED,
	DRV_ADT746X_IMPLAUSIBLE,
	DRV_ADT746X_UNREADABLE,
	DRV_ADT746X_NOT_KEPT
};

/*
 * How the driver reaches the chip's registers: a read and a write of one
 * register, each reporting 0 or an errno, and their argument.
 */
struct drv_adt746x_access {
	int (*read)(void *context, uint8_t number, uint8_t *value);
	int (*write)(void *context, uint8_t number, uint8_t value);
	void *context;
};

/*
 * One chip the driver identified: how to reach it, which chip it is, how
 * many fans it drives here, how it encodes temperatures, and its
 * identification registers.
 */
struct drv_adt746x {
	struct drv_adt746x_access access;
	uint32_t chip;
	unsigned fan_count;
	bool offset64;
	uint8_t device_id;
	uint8_t revision;
};

/*
 * What decides the fans, as read from the chip: the first configuration
 * register, each channel's temperature and its two limits (the
 * temperature at which the automatic control starts the fans, TMIN, and
 * the one at which it runs them at full speed, THERM) in degrees Celsius,
 * and each fan's behaviour register, present duty cycle and minimum duty
 * cycle (255 is 100%).
 */
struct drv_adt746x_settings {
	uint8_t configuration;
	int32_t temperature[DRV_ADT746X_CHANNELS];
	int32_t start[DRV_ADT746X_CHANNELS];
	int32_t full[DRV_ADT746X_CHANNELS];
	uint8_t behaviour[DRV_ADT746X_FANS_MAX];
	uint8_t duty[DRV_ADT746X_FANS_MAX];
	uint8_t minimum[DRV_ADT746X_FANS_MAX];
};

/* One register a set-up wrote: its number, and its value before and after. */
struct drv_adt746x_change {
	uint8_t number;
	uint8_t before;
	uint8_t after;
};

/*
 * What a set-up did: how it ended, and the registers it wrote, in order.
 */
struct drv_adt746x_report {
	enum drv_adt746x_outcome outcome;
	unsigned change_count;
	struct drv_adt746x_change changes[DRV_ADT746X_CHANGES_MAX];
};

/*
 * Identifies the chip the firmware named: its company and device ids must
 * be the named chip's.  Writes nothing.  Returns 0, ENODEV for another
 * chip, EINVAL for an unknown name, or the bus's errno.
 */
int drv_adt746x_identify(const struct drv_adt746x_access *access, uint32_t chip, struct drv_adt746x *device);

/*
 * Reads what decides the fans.  Writes nothing.  Returns 0 or the bus's
 * errno.
 */
int drv_adt746x_read_settings(const struct drv_adt746x *device, struct drv_adt746x_settings *settings);

/*
 * Lets the chip's automatic control cool the machine, by writes that can
 * only make a fan turn faster, never slower, at every temperature: a
 * channel's TMIN and THERM limits are lowered to the ones the driver
 * keeps when they are higher, and a fan whose control is off or follows
 * fewer than all three channels follows all three.  A fan at full speed
 * or under manual control is left as it is.  Nothing is written while
 * monitoring is off, while the limits are locked, or while a temperature
 * cannot be read or is out of a running machine's range; a write that the
 * register does not keep ends the set-up.  Returns 0 with the report, or
 * the bus's errno (the report says what was written before it failed).
 */
int drv_adt746x_set_up(const struct drv_adt746x *device, struct drv_adt746x_report *report);

/*
 * Reads the three temperatures in millidegrees Celsius; a channel the chip
 * could not measure is marked not valid.  Returns 0 or the bus's errno.
 */
int drv_adt746x_read_temperatures(const struct drv_adt746x *device, int32_t *millicelsius, bool *valid);

/*
 * Reads one fan: its speed in revolutions a minute (0 when it stands
 * still) and its present duty cycle (255 is 100%).  Returns 0, EINVAL for
 * a fan the chip does not drive here, or the bus's errno.
 */
int drv_adt746x_read_fan(const struct drv_adt746x *device, unsigned fan, uint32_t *rpm, uint8_t *duty);

#endif
