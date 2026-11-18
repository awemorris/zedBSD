/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ADM1030 fan controller of the 12-inch PowerBook G4.
 *
 * The chip measures its own temperature and a remote diode's, and runs
 * the fan either by its automatic control (off below a channel's TMIN,
 * then from the minimum duty cycle up to full speed over TRANGE; the
 * fastest of the two channels, or the remote one alone) or at a duty
 * cycle software chose.  Above a channel's THERM limit it can run the fan
 * at full speed.  The driver keeps that control and only makes it start
 * the fan earlier (drv_adm1030_set_up()).  The status registers are left
 * unread, since reading them clears them.
 *
 * Temperatures are two's complement degrees in one register each, with
 * quarter degrees (local) and eighth degrees (remote) in the extended
 * resolution register.  The fan's speed is the tachometer's count between
 * pulses of a 11.25 kHz clock, divided by the speed range N; 0xff is a
 * fan that does not turn.
 */

#include "drivers/platform/mac99/mac99-adm1030.h"
#include <uapi/errno.h>

#include <stddef.h>

/*
 * The registers.
 *
 * Values referenced: onsemi ADM1030 data sheet, rev. 3 (Table 16, the
 * register map; Table 17, the value and limit registers; Table 22, the
 * extended resolution bits; Table 23, the speed range; Table 28, the THERM
 * behaviour); Linux v6.12 drivers/hwmon/adm1031.c (the tachometer's
 * clock times sixty, 11250 * 60).
 */
#define ADM_CONFIGURATION1		0x00U
#define ADM_CONFIGURATION2		0x01U
#define ADM_EXTENDED			0x06U
#define ADM_TACHOMETER			0x08U
#define ADM_TEMPERATURE_LOCAL		0x0aU
#define ADM_TEMPERATURE_REMOTE		0x0bU
#define ADM_OFFSET_LOCAL		0x0dU
#define ADM_OFFSET_REMOTE		0x0eU
#define ADM_TACHOMETER_LIMIT		0x10U
#define ADM_HIGH_LOCAL			0x14U
#define ADM_LOW_LOCAL			0x15U
#define ADM_THERM_LOCAL			0x16U
#define ADM_HIGH_REMOTE			0x18U
#define ADM_LOW_REMOTE			0x19U
#define ADM_THERM_REMOTE		0x1aU
#define ADM_CHARACTERISTICS		0x20U
#define ADM_SPEED			0x22U
#define ADM_FILTER			0x23U
#define ADM_AUTOMATIC_LOCAL		0x24U
#define ADM_AUTOMATIC_REMOTE		0x25U
#define ADM_DEVICE_ID			0x3dU
#define ADM_COMPANY_ID			0x3eU
#define ADM_BEHAVIOUR			0x3fU

/* The company id of Analog Devices, and the ADM1030's device id. */
#define ADM_COMPANY_ANALOG_DEVICES	0x41U
#define ADM_DEVICE_ADM1030		0x30U

/* The extended resolution bits: local in bits 6 and 7 (quarter degrees), remote in bits 0 to 2 (eighth degrees). */
#define ADM_EXTENDED_LOCAL_SHIFT	6U
#define ADM_EXTENDED_LOCAL_MASK		0x3U
#define ADM_EXTENDED_REMOTE_MASK	0x7U

/* The speed range N in bits 6 and 7 of the characteristics register (N = 1, 2, 4 or 8). */
#define ADM_RANGE_SHIFT			6U
#define ADM_RANGE_MASK			0x3U

/* The tachometer's clock times sixty, and its count for a fan that does not turn. */
#define ADM_TACHOMETER_PER_MINUTE	(11250U * 60U)
#define ADM_TACHOMETER_STILL		0xffU

/* Configuration register 1: automatic control (clear: software), the fan's channels (bits 5 and 6), monitoring. */
#define ADM_CONFIGURATION1_AUTOMATIC	0x80U
#define ADM_CONFIGURATION1_CHANNELS	0x60U
#define ADM_CONFIGURATION1_REMOTE	0x00U
#define ADM_CONFIGURATION1_FASTEST	0x60U
#define ADM_CONFIGURATION1_MONITOR	0x01U

/* A TMIN/TRANGE register: TMIN in bits 3 to 7, in steps of 4 degrees; TRANGE in bits 0 to 2. */
#define ADM_TMIN_SHIFT			3U
#define ADM_TMIN_STEP			4
#define ADM_TRANGE_MASK			0x07U

/* The temperatures a running machine's sensors read, in degrees Celsius. */
#define PLAUSIBLE_LOW			0
#define PLAUSIBLE_HIGH			120

/* A byte's sign bit and span, for two's complement. */
#define BYTE_SIGN			0x80U
#define BYTE_SPAN			256

/*
 * The highest TMIN the driver leaves in place, in degrees Celsius (local,
 * remote): the temperatures from which Linux's driver for the PowerBook
 * G4's ADT746x starts the fans (70 for the board, 50 for the processor),
 * rounded down to the ADM1030's 4-degree steps.
 *
 * Values referenced: Linux v6.12 drivers/macintosh/therm_adt746x.c
 * (default_limits_local).
 */
static const int32_t start_ceilings[DRV_ADM1030_CHANNELS] = {
	68,
	48
};

/* Each channel's TMIN/TRANGE register: local, remote. */
static const uint8_t automatic_registers[DRV_ADM1030_CHANNELS] = {
	ADM_AUTOMATIC_LOCAL,
	ADM_AUTOMATIC_REMOTE
};

static int read_register(const struct drv_adm1030 *device, uint8_t number, uint8_t *value);
static int change_register(const struct drv_adm1030 *device, uint8_t number, uint8_t before, uint8_t after, struct drv_adm1030_report *report);
static int read_celsius(const struct drv_adm1030 *device, uint8_t number, int32_t *celsius);
static int32_t signed_byte(uint8_t value);

/*
 * Identifies the chip by its company and device ids.
 */
int
drv_adm1030_identify(
	const struct drv_adm1030_access *access,
	struct drv_adm1030 *device)
{
	uint8_t company;
	uint8_t identity;
	int error;

	/* Refuses a missing way to the chip. */
	if (access == NULL || device == NULL)
		return EINVAL;
	if (access->read == NULL)
		return EINVAL;
	device->access = *access;

	/* The company: Analog Devices, else it is not the chip. */
	error = read_register(device, ADM_COMPANY_ID, &company);
	if (error != 0)
		return error;
	if (company != ADM_COMPANY_ANALOG_DEVICES)
		return ENODEV;

	/* The device: the ADM1030, else another chip. */
	error = read_register(device, ADM_DEVICE_ID, &identity);
	if (error != 0)
		return error;
	if (identity != ADM_DEVICE_ADM1030)
		return ENODEV;

	/* The revision and the THERM behaviour, for the log. */
	error = read_register(device, ADM_BEHAVIOUR, &device->revision);
	if (error != 0)
		return error;

	/* Succeeded: the chip is an ADM1030. */
	return 0;
}

/*
 * Lowers each channel's TMIN to the driver's when it is higher, and has a
 * fan that follows the remote channel alone follow the faster of both,
 * each write read back.
 */
int
drv_adm1030_set_up(
	const struct drv_adm1030 *device,
	struct drv_adm1030_report *report)
{
	struct drv_adm1030_settings settings;
	unsigned channel;
	uint8_t before;
	uint8_t after;
	int32_t start;
	int32_t temperature;
	int error;

	/* Nothing written yet; a driver that cannot write does not set up. */
	report->outcome = DRV_ADM1030_SET_UP;
	report->change_count = 0;
	if (device->access.write == NULL)
		return EINVAL;

	/* What the chip holds now; a chip that does not answer is left alone. */
	error = drv_adm1030_read_settings(device, &settings);
	if (error != 0) {
		report->outcome = DRV_ADM1030_UNREADABLE;
		return error;
	}

	/* A fan under software control is someone else's choice. */
	if ((settings.configuration1 & ADM_CONFIGURATION1_AUTOMATIC) == 0) {
		report->outcome = DRV_ADM1030_SOFTWARE;
		return 0;
	}

	/* A chip that does not monitor has no temperature to control by. */
	if ((settings.configuration1 & ADM_CONFIGURATION1_MONITOR) == 0) {
		report->outcome = DRV_ADM1030_NOT_MONITORING;
		return 0;
	}

	/* Both channels must read a temperature a running machine has, else the chip is not understood. */
	for (channel = 0; channel < DRV_ADM1030_CHANNELS; channel++) {
		temperature = settings.temperature[channel];
		if (temperature < PLAUSIBLE_LOW || temperature > PLAUSIBLE_HIGH) {
			report->outcome = DRV_ADM1030_IMPLAUSIBLE;
			return 0;
		}
	}

	/* Each channel's TMIN, lowered to the driver's when higher, its TRANGE kept. */
	for (channel = 0; channel < DRV_ADM1030_CHANNELS; channel++) {
		before = settings.automatic[channel];
		start = (int32_t)(before >> ADM_TMIN_SHIFT) * ADM_TMIN_STEP;
		if (start <= start_ceilings[channel])
			continue;
		after = (uint8_t)(((uint32_t)(start_ceilings[channel] / ADM_TMIN_STEP) << ADM_TMIN_SHIFT) | (before & ADM_TRANGE_MASK));
		error = change_register(device, automatic_registers[channel], before, after, report);
		if (error != 0)
			return error;
		if (report->outcome != DRV_ADM1030_SET_UP)
			return 0;
	}

	/* A fan that follows the remote channel alone follows the faster of both, its other bits kept. */
	before = settings.configuration1;
	if ((before & ADM_CONFIGURATION1_CHANNELS) == ADM_CONFIGURATION1_REMOTE) {
		after = (uint8_t)(before | ADM_CONFIGURATION1_FASTEST);
		error = change_register(device, ADM_CONFIGURATION1, before, after, report);
		if (error != 0)
			return error;
	}

	/* Succeeded: the chip's control starts the fan at the driver's temperatures, or the report says why not. */
	return 0;
}

/*
 * Reads the configuration, the temperatures, the limits and the fan's
 * registers, leaving the status registers alone.
 */
int
drv_adm1030_read_settings(
	const struct drv_adm1030 *device,
	struct drv_adm1030_settings *settings)
{
	static const uint8_t temperature_registers[DRV_ADM1030_CHANNELS] = {
		ADM_TEMPERATURE_LOCAL, ADM_TEMPERATURE_REMOTE
	};
	static const uint8_t offset_registers[DRV_ADM1030_CHANNELS] = {
		ADM_OFFSET_LOCAL, ADM_OFFSET_REMOTE
	};
	static const uint8_t high_registers[DRV_ADM1030_CHANNELS] = {
		ADM_HIGH_LOCAL, ADM_HIGH_REMOTE
	};
	static const uint8_t low_registers[DRV_ADM1030_CHANNELS] = {
		ADM_LOW_LOCAL, ADM_LOW_REMOTE
	};
	static const uint8_t therm_registers[DRV_ADM1030_CHANNELS] = {
		ADM_THERM_LOCAL, ADM_THERM_REMOTE
	};
	unsigned channel;
	int error;

	/* The two configuration registers. */
	error = read_register(device, ADM_CONFIGURATION1, &settings->configuration1);
	if (error != 0)
		return error;
	error = read_register(device, ADM_CONFIGURATION2, &settings->configuration2);
	if (error != 0)
		return error;

	/* Each channel's temperature, offset, limits and automatic control. */
	for (channel = 0; channel < DRV_ADM1030_CHANNELS; channel++) {
		error = read_celsius(device, temperature_registers[channel], &settings->temperature[channel]);
		if (error != 0)
			return error;
		error = read_celsius(device, offset_registers[channel], &settings->offset[channel]);
		if (error != 0)
			return error;
		error = read_celsius(device, high_registers[channel], &settings->high[channel]);
		if (error != 0)
			return error;
		error = read_celsius(device, low_registers[channel], &settings->low[channel]);
		if (error != 0)
			return error;
		error = read_celsius(device, therm_registers[channel], &settings->therm[channel]);
		if (error != 0)
			return error;
		error = read_register(device, automatic_registers[channel], &settings->automatic[channel]);
		if (error != 0)
			return error;
	}

	/* The fan's registers and the THERM behaviour. */
	error = read_register(device, ADM_TACHOMETER, &settings->tachometer);
	if (error != 0)
		return error;
	error = read_register(device, ADM_TACHOMETER_LIMIT, &settings->tachometer_limit);
	if (error != 0)
		return error;
	error = read_register(device, ADM_CHARACTERISTICS, &settings->characteristics);
	if (error != 0)
		return error;
	error = read_register(device, ADM_SPEED, &settings->speed);
	if (error != 0)
		return error;
	error = read_register(device, ADM_FILTER, &settings->filter);
	if (error != 0)
		return error;
	error = read_register(device, ADM_BEHAVIOUR, &settings->behaviour);
	if (error != 0)
		return error;

	/* Succeeded: the settings as the chip holds them. */
	return 0;
}

/*
 * Reads the two temperatures with their extended resolution.
 */
int
drv_adm1030_read_temperatures(
	const struct drv_adm1030 *device,
	int32_t *millicelsius)
{
	uint8_t extended;
	int32_t local;
	int32_t remote;
	int error;

	/* The whole degrees of each channel, then the fractions both share a register for. */
	error = read_celsius(device, ADM_TEMPERATURE_LOCAL, &local);
	if (error != 0)
		return error;
	error = read_celsius(device, ADM_TEMPERATURE_REMOTE, &remote);
	if (error != 0)
		return error;
	error = read_register(device, ADM_EXTENDED, &extended);
	if (error != 0)
		return error;

	/* Quarter degrees for the local channel, eighth degrees for the remote one. */
	millicelsius[DRV_ADM1030_LOCAL] = local * 1000 + (int32_t)((extended >> ADM_EXTENDED_LOCAL_SHIFT) & ADM_EXTENDED_LOCAL_MASK) * 250;
	millicelsius[DRV_ADM1030_REMOTE] = remote * 1000 + (int32_t)(extended & ADM_EXTENDED_REMOTE_MASK) * 125;

	/* Succeeded: both temperatures. */
	return 0;
}

/*
 * Reads the fan's speed from its tachometer and speed range.
 */
int
drv_adm1030_read_fan(
	const struct drv_adm1030 *device,
	uint32_t *rpm)
{
	uint8_t count;
	uint8_t characteristics;
	uint32_t range;
	int error;

	/* The tachometer's count and the speed range it was taken with. */
	error = read_register(device, ADM_TACHOMETER, &count);
	if (error != 0)
		return error;
	error = read_register(device, ADM_CHARACTERISTICS, &characteristics);
	if (error != 0)
		return error;
	range = 1U << ((characteristics >> ADM_RANGE_SHIFT) & ADM_RANGE_MASK);

	/* The speed: none for a fan that does not turn or no count. */
	*rpm = 0;
	if (count != 0 && count != ADM_TACHOMETER_STILL)
		*rpm = ADM_TACHOMETER_PER_MINUTE / ((uint32_t)count * range);

	/* Succeeded: the fan's speed. */
	return 0;
}

/* Reads one of the chip's registers through the bus. */
static int
read_register(
	const struct drv_adm1030 *device,
	uint8_t number,
	uint8_t *value)
{
	int error;

	/* The bus's read of the register. */
	error = device->access.read(device->access.context, number, value);
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	return 0;
}

/* Reads a register that holds degrees Celsius in two's complement. */
static int
read_celsius(
	const struct drv_adm1030 *device,
	uint8_t number,
	int32_t *celsius)
{
	uint8_t value;
	int error;

	/* The register. */
	error = read_register(device, number, &value);
	if (error != 0)
		return error;

	/* Succeeded: the degrees. */
	*celsius = signed_byte(value);
	return 0;
}

/* Turns a two's complement byte into a number. */
static int32_t
signed_byte(
	uint8_t value)
{
	/* Below zero. */
	if ((value & BYTE_SIGN) != 0)
		return (int32_t)value - BYTE_SPAN;

	/* Zero or above. */
	return (int32_t)value;
}

/*
 * Writes one register, reads it back and records the change; a register
 * that does not keep the value ends the set-up.
 */
static int
change_register(
	const struct drv_adm1030 *device,
	uint8_t number,
	uint8_t before,
	uint8_t after,
	struct drv_adm1030_report *report)
{
	struct drv_adm1030_change *change;
	uint8_t kept;
	int error;

	/* The write. */
	error = device->access.write(device->access.context, number, after);
	if (error != 0)
		return error;

	/* Records it, as written, before it is checked. */
	if (report->change_count < DRV_ADM1030_CHANGES_MAX) {
		change = &report->changes[report->change_count];
		change->number = number;
		change->before = before;
		change->after = after;
		report->change_count++;
	}

	/* The value the register holds now; another one ends the set-up. */
	error = read_register(device, number, &kept);
	if (error != 0)
		return error;
	if (kept != after)
		report->outcome = DRV_ADM1030_NOT_KEPT;

	/* Succeeded: the write was made and checked. */
	return 0;
}
