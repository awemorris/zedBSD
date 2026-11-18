/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ADT7460 and ADT7467 fan controllers of the PowerBook G4 and the
 * iBook G4.
 *
 * The chip measures its own temperature and two remote diodes', and runs
 * each fan by itself: under its automatic control a fan is off (or at its
 * minimum duty cycle) below the TMIN limit of the channels it follows,
 * speeds up over the range above it, and runs at full speed once any
 * channel passes its THERM limit.  The driver keeps that control.  It only
 * lowers the limits that are higher than its own and widens the channels
 * a fan follows, so every write can only make a fan turn faster; it never
 * writes a duty cycle, never turns a fan off, and never takes a fan from
 * the chip's control.
 *
 * Temperatures and limits are one byte each, in two's complement, or on
 * the ADT7467 also offset by 64 (its fifth configuration register tells
 * which); a channel whose diode is open or shorted reads the lowest code.
 * A fan's speed is the period between its tachometer's pulses, counted in
 * the chip's 90 kHz clock, low byte first (reading it holds the high byte
 * for the next read).
 */

#include "drivers/platform/mac99/mac99-adt746x.h"
#include <uapi/errno.h>

#include <stddef.h>

/*
 * The registers: temperatures, the tachometers' low bytes, the present
 * duty cycles, the identification, the first configuration register, the
 * fans' behaviour, their minimum duty cycles, the TMIN and THERM limits,
 * and the ADT7467's fifth configuration register.
 *
 * Values referenced: onsemi ADT7467 data sheet, rev. 6 (Table 42, the
 * register map; Table 51, configuration register 1; Table 59, the PWM
 * configuration registers; Table 84, configuration register 5); Linux
 * v6.12 drivers/macintosh/therm_adt746x.c (the temperature, limit,
 * behaviour, tachometer and duty cycle registers the ADT7460 shares);
 * NetBSD sys/dev/i2c/dbcool_reg.h, trunk (the identification registers
 * and the company and device ids of both chips).
 */
#define ADT_TEMPERATURE_REMOTE1		0x25U
#define ADT_TEMPERATURE_LOCAL		0x26U
#define ADT_TEMPERATURE_REMOTE2		0x27U
#define ADT_TACHOMETER1_LOW		0x28U
#define ADT_TACHOMETER2_LOW		0x2aU
#define ADT_DUTY1			0x30U
#define ADT_DUTY2			0x31U
#define ADT_DEVICE_ID			0x3dU
#define ADT_COMPANY_ID			0x3eU
#define ADT_REVISION			0x3fU
#define ADT_CONFIGURATION1		0x40U
#define ADT_BEHAVIOUR1			0x5cU
#define ADT_BEHAVIOUR2			0x5dU
#define ADT_MINIMUM1			0x64U
#define ADT_MINIMUM2			0x65U
#define ADT_START_REMOTE1		0x67U
#define ADT_START_LOCAL			0x68U
#define ADT_START_REMOTE2		0x69U
#define ADT_FULL_REMOTE1		0x6aU
#define ADT_FULL_LOCAL			0x6bU
#define ADT_FULL_REMOTE2		0x6cU
#define ADT_CONFIGURATION5		0x7cU

/* The company id of Analog Devices, and the device ids of the two chips. */
#define ADT_COMPANY_ANALOG_DEVICES	0x41U
#define ADT_DEVICE_ADT7460		0x27U
#define ADT_DEVICE_ADT7467		0x68U

/* Configuration register 1: monitoring started, limits locked. */
#define ADT_CONFIGURATION1_STARTED	0x01U
#define ADT_CONFIGURATION1_LOCKED	0x02U

/* Configuration register 5: temperatures in two's complement (clear: offset by 64). */
#define ADT_CONFIGURATION5_TWOS		0x01U

/*
 * A fan's behaviour, bits 5 to 7 of its configuration register: which
 * channels its automatic control follows, full speed, off, or manual.
 */
#define ADT_BEHAVIOUR_SHIFT		5U
#define ADT_BEHAVIOUR_MASK		0xe0U
#define ADT_BEHAVIOUR_REMOTE1		0U
#define ADT_BEHAVIOUR_LOCAL		1U
#define ADT_BEHAVIOUR_REMOTE2		2U
#define ADT_BEHAVIOUR_FULL		3U
#define ADT_BEHAVIOUR_OFF		4U
#define ADT_BEHAVIOUR_LOCAL_REMOTE2	5U
#define ADT_BEHAVIOUR_ALL		6U
#define ADT_BEHAVIOUR_MANUAL		7U

/* A temperature byte's offset in the ADT7467's offset form, and each form's code for a broken diode. */
#define ADT_OFFSET64			64
#define ADT_FAULT_TWOS			0x80U
#define ADT_FAULT_OFFSET64		0x00U

/* The tachometer's clock (90 kHz) times the seconds of a minute, and its counts for a fan standing still. */
#define ADT_TACHOMETER_PER_MINUTE	(90000U * 60U)
#define ADT_TACHOMETER_STILL		0xffffU

/* The temperatures a running machine's sensors read, in degrees Celsius. */
#define PLAUSIBLE_LOW			0
#define PLAUSIBLE_HIGH			120

/* The fans each chip drives on these machines. */
#define FANS_ADT7460			2U
#define FANS_ADT7467			1U

/* A byte's sign bit and its span, for the two's complement form. */
#define BYTE_SIGN			0x80U
#define BYTE_SPAN			256

/* Each channel's temperature register, in the report's order: local, remote 1, remote 2. */
static const uint8_t temperature_registers[DRV_ADT746X_CHANNELS] = {
	ADT_TEMPERATURE_LOCAL,
	ADT_TEMPERATURE_REMOTE1,
	ADT_TEMPERATURE_REMOTE2
};

/* Each channel's TMIN register, in the same order. */
static const uint8_t start_registers[DRV_ADT746X_CHANNELS] = {
	ADT_START_LOCAL,
	ADT_START_REMOTE1,
	ADT_START_REMOTE2
};

/* Each channel's THERM register, in the same order. */
static const uint8_t full_registers[DRV_ADT746X_CHANNELS] = {
	ADT_FULL_LOCAL,
	ADT_FULL_REMOTE1,
	ADT_FULL_REMOTE2
};

/*
 * The highest TMIN the driver leaves in place, in degrees Celsius: the
 * temperatures from which Linux's driver for these machines starts the
 * fans (local, remote 1, remote 2).
 *
 * Values referenced: Linux v6.12 drivers/macintosh/therm_adt746x.c
 * (default_limits_local: 70, 50 and 70).
 */
static const int32_t start_ceilings[DRV_ADT746X_CHANNELS] = {
	70,
	50,
	70
};

/*
 * The highest THERM limit the driver leaves in place, in degrees Celsius:
 * the ones Linux's driver writes to the chip on these machines (local,
 * remote 1, remote 2).
 *
 * Values referenced: Linux v6.12 drivers/macintosh/therm_adt746x.c
 * (default_limits_chip: 80, 65 and 80).
 */
static const int32_t full_ceilings[DRV_ADT746X_CHANNELS] = {
	80,
	65,
	80
};

/* Each fan's configuration (behaviour) register. */
static const uint8_t behaviour_registers[DRV_ADT746X_FANS_MAX] = {
	ADT_BEHAVIOUR1,
	ADT_BEHAVIOUR2
};

/* Each fan's present duty cycle register. */
static const uint8_t duty_registers[DRV_ADT746X_FANS_MAX] = {
	ADT_DUTY1,
	ADT_DUTY2
};

/* Each fan's minimum duty cycle register. */
static const uint8_t minimum_registers[DRV_ADT746X_FANS_MAX] = {
	ADT_MINIMUM1,
	ADT_MINIMUM2
};

/* Each fan's tachometer, its low byte (the high byte follows it). */
static const uint8_t tachometer_registers[DRV_ADT746X_FANS_MAX] = {
	ADT_TACHOMETER1_LOW,
	ADT_TACHOMETER2_LOW
};

static int read_register(const struct drv_adt746x *device, uint8_t number, uint8_t *value);
static int32_t decode_temperature(const struct drv_adt746x *device, uint8_t value);
static int32_t decode_limit(const struct drv_adt746x *device, uint8_t value);
static uint8_t encode(const struct drv_adt746x *device, int32_t celsius);
static bool behaviour_widens(uint8_t behaviour);
static int lower_limit(const struct drv_adt746x *device, uint8_t number, int32_t ceiling, struct drv_adt746x_report *report);
static int change_register(const struct drv_adt746x *device, uint8_t number, uint8_t before, uint8_t after, struct drv_adt746x_report *report);

/*
 * Identifies the chip the firmware named by its company and device ids.
 */
int
drv_adt746x_identify(
	const struct drv_adt746x_access *access,
	uint32_t chip,
	struct drv_adt746x *device)
{
	uint8_t company;
	uint8_t identity;
	uint8_t expected;
	uint8_t configuration;
	int error;

	/* Refuses a missing way to the chip, and a chip the driver does not know. */
	if (access == NULL || device == NULL)
		return EINVAL;
	if (access->read == NULL || access->write == NULL)
		return EINVAL;
	if (chip == DRV_ADT746X_ADT7460) {
		expected = ADT_DEVICE_ADT7460;
	} else if (chip == DRV_ADT746X_ADT7467) {
		expected = ADT_DEVICE_ADT7467;
	} else {
		return EINVAL;
	}

	/* Keeps the way to the chip. */
	device->access = *access;
	device->chip = chip;
	device->offset64 = false;

	/* The company: Analog Devices, else it is not the chip named. */
	error = read_register(device, ADT_COMPANY_ID, &company);
	if (error != 0)
		return error;
	if (company != ADT_COMPANY_ANALOG_DEVICES)
		return ENODEV;

	/* The device: the one named, else another chip of the family. */
	error = read_register(device, ADT_DEVICE_ID, &identity);
	if (error != 0)
		return error;
	if (identity != expected)
		return ENODEV;
	device->device_id = identity;

	/* The revision, for the log. */
	error = read_register(device, ADT_REVISION, &device->revision);
	if (error != 0)
		return error;

	/* The fans it drives here, and the ADT7467's form of temperatures. */
	if (chip == DRV_ADT746X_ADT7460) {
		device->fan_count = FANS_ADT7460;
	} else {
		device->fan_count = FANS_ADT7467;
		error = read_register(device, ADT_CONFIGURATION5, &configuration);
		if (error != 0)
			return error;
		if ((configuration & ADT_CONFIGURATION5_TWOS) == 0)
			device->offset64 = true;
	}

	/* Succeeded: the chip is the one named. */
	return 0;
}

/*
 * Reads the configuration, the temperatures, the limits and the fans'
 * registers.
 */
int
drv_adt746x_read_settings(
	const struct drv_adt746x *device,
	struct drv_adt746x_settings *settings)
{
	unsigned channel;
	unsigned fan;
	uint8_t value;
	int error;

	/* The first configuration register. */
	error = read_register(device, ADT_CONFIGURATION1, &settings->configuration);
	if (error != 0)
		return error;

	/* Each channel's temperature and its two limits. */
	for (channel = 0; channel < DRV_ADT746X_CHANNELS; channel++) {
		error = read_register(device, temperature_registers[channel], &value);
		if (error != 0)
			return error;
		settings->temperature[channel] = decode_temperature(device, value);

		error = read_register(device, start_registers[channel], &value);
		if (error != 0)
			return error;
		settings->start[channel] = decode_limit(device, value);

		error = read_register(device, full_registers[channel], &value);
		if (error != 0)
			return error;
		settings->full[channel] = decode_limit(device, value);
	}

	/* Each fan's behaviour, present duty cycle and minimum; none past the fans driven here. */
	for (fan = 0; fan < DRV_ADT746X_FANS_MAX; fan++) {
		settings->behaviour[fan] = 0;
		settings->duty[fan] = 0;
		settings->minimum[fan] = 0;
		if (fan >= device->fan_count)
			continue;

		error = read_register(device, behaviour_registers[fan], &settings->behaviour[fan]);
		if (error != 0)
			return error;
		error = read_register(device, duty_registers[fan], &settings->duty[fan]);
		if (error != 0)
			return error;
		error = read_register(device, minimum_registers[fan], &settings->minimum[fan]);
		if (error != 0)
			return error;
	}

	/* Succeeded: the settings as the chip holds them. */
	return 0;
}

/*
 * Lowers the limits that are higher than the driver's and widens the
 * channels each fan follows, each write read back.
 */
int
drv_adt746x_set_up(
	const struct drv_adt746x *device,
	struct drv_adt746x_report *report)
{
	struct drv_adt746x_settings settings;
	unsigned channel;
	unsigned fan;
	uint8_t behaviour;
	uint8_t widened;
	int32_t temperature;
	int error;

	/* Nothing written yet. */
	report->outcome = DRV_ADT746X_SET_UP;
	report->change_count = 0;

	/* What the chip holds now; a chip that does not answer is left alone. */
	error = drv_adt746x_read_settings(device, &settings);
	if (error != 0) {
		report->outcome = DRV_ADT746X_UNREADABLE;
		return error;
	}

	/* A chip that does not monitor runs its power-up behaviour; the driver does not start it. */
	if ((settings.configuration & ADT_CONFIGURATION1_STARTED) == 0) {
		report->outcome = DRV_ADT746X_NOT_STARTED;
		return 0;
	}

	/* Locked limits cannot be written. */
	if ((settings.configuration & ADT_CONFIGURATION1_LOCKED) != 0) {
		report->outcome = DRV_ADT746X_LOCKED;
		return 0;
	}

	/* Every channel must read a temperature a running machine has, else the chip is not understood. */
	for (channel = 0; channel < DRV_ADT746X_CHANNELS; channel++) {
		temperature = settings.temperature[channel];
		if (temperature == DRV_ADT746X_NO_READING) {
			report->outcome = DRV_ADT746X_IMPLAUSIBLE;
			return 0;
		}
		if (temperature < PLAUSIBLE_LOW || temperature > PLAUSIBLE_HIGH) {
			report->outcome = DRV_ADT746X_IMPLAUSIBLE;
			return 0;
		}
	}

	/* Each channel's full-speed limit, then its start limit, lowered to the driver's when higher. */
	for (channel = 0; channel < DRV_ADT746X_CHANNELS; channel++) {
		error = lower_limit(device, full_registers[channel], full_ceilings[channel], report);
		if (error != 0)
			return error;
		if (report->outcome != DRV_ADT746X_SET_UP)
			return 0;

		error = lower_limit(device, start_registers[channel], start_ceilings[channel], report);
		if (error != 0)
			return error;
		if (report->outcome != DRV_ADT746X_SET_UP)
			return 0;
	}

	/* Each fan whose control is off or follows fewer channels follows all three, its other bits kept. */
	for (fan = 0; fan < device->fan_count; fan++) {
		behaviour = settings.behaviour[fan];
		if (!behaviour_widens(behaviour))
			continue;
		widened = (uint8_t)((behaviour & ~ADT_BEHAVIOUR_MASK) | (ADT_BEHAVIOUR_ALL << ADT_BEHAVIOUR_SHIFT));
		error = change_register(device, behaviour_registers[fan], behaviour, widened, report);
		if (error != 0)
			return error;
		if (report->outcome != DRV_ADT746X_SET_UP)
			return 0;
	}

	/* Succeeded: the chip's control has the driver's limits and follows every channel. */
	return 0;
}

/*
 * Reads the three temperatures in millidegrees Celsius.
 */
int
drv_adt746x_read_temperatures(
	const struct drv_adt746x *device,
	int32_t *millicelsius,
	bool *valid)
{
	unsigned channel;
	uint8_t value;
	int32_t celsius;
	int error;

	/* Each channel; a broken diode reads as no temperature. */
	for (channel = 0; channel < DRV_ADT746X_CHANNELS; channel++) {
		error = read_register(device, temperature_registers[channel], &value);
		if (error != 0)
			return error;
		celsius = decode_temperature(device, value);
		millicelsius[channel] = 0;
		valid[channel] = false;
		if (celsius != DRV_ADT746X_NO_READING) {
			millicelsius[channel] = celsius * 1000;
			valid[channel] = true;
		}
	}

	/* Succeeded: every channel read. */
	return 0;
}

/*
 * Reads one fan's speed and its present duty cycle.
 */
int
drv_adt746x_read_fan(
	const struct drv_adt746x *device,
	unsigned fan,
	uint32_t *rpm,
	uint8_t *duty)
{
	uint8_t low;
	uint8_t high;
	uint32_t period;
	int error;

	/* Refuses a fan the chip does not drive here. */
	if (fan >= device->fan_count)
		return EINVAL;

	/* The period between the tachometer's pulses, low byte first so the high byte is held for it. */
	error = read_register(device, tachometer_registers[fan], &low);
	if (error != 0)
		return error;
	error = read_register(device, (uint8_t)(tachometer_registers[fan] + 1U), &high);
	if (error != 0)
		return error;
	period = ((uint32_t)high << 8) | low;

	/* The speed: none for a fan standing still (no pulse in the whole count) or no count at all. */
	*rpm = 0;
	if (period != 0 && period != ADT_TACHOMETER_STILL)
		*rpm = ADT_TACHOMETER_PER_MINUTE / period;

	/* The duty cycle the chip drives the fan with now. */
	error = read_register(device, duty_registers[fan], duty);
	if (error != 0)
		return error;

	/* Succeeded: the fan's speed and duty cycle. */
	return 0;
}

/* Reads one of the chip's registers through the bus. */
static int
read_register(
	const struct drv_adt746x *device,
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

/* Turns a measured temperature's byte into degrees Celsius, or DRV_ADT746X_NO_READING for a broken diode's code. */
static int32_t
decode_temperature(
	const struct drv_adt746x *device,
	uint8_t value)
{
	int32_t celsius;

	/* The lowest code of either form is a broken diode. */
	if (device->offset64 && value == ADT_FAULT_OFFSET64)
		return DRV_ADT746X_NO_READING;
	if (!device->offset64 && value == ADT_FAULT_TWOS)
		return DRV_ADT746X_NO_READING;

	/* Succeeded: the temperature. */
	celsius = decode_limit(device, value);
	return celsius;
}

/* Turns a limit's byte into degrees Celsius; every code is a temperature. */
static int32_t
decode_limit(
	const struct drv_adt746x *device,
	uint8_t value)
{
	/* The ADT7467's offset form. */
	if (device->offset64)
		return (int32_t)value - ADT_OFFSET64;

	/* Two's complement below 0 degrees. */
	if ((value & BYTE_SIGN) != 0)
		return (int32_t)value - BYTE_SPAN;

	/* A temperature at or above 0 degrees. */
	return (int32_t)value;
}

/* Turns degrees Celsius (0 to 127) into the chip's temperature byte. */
static uint8_t
encode(
	const struct drv_adt746x *device,
	int32_t celsius)
{
	/* The ADT7467's offset form. */
	if (device->offset64)
		return (uint8_t)(celsius + ADT_OFFSET64);

	/* Two's complement, the same byte for a temperature at or above 0. */
	return (uint8_t)celsius;
}

/*
 * Tells whether a fan's behaviour is one the driver widens: off, or an
 * automatic control that follows fewer than all three channels.  Full
 * speed, all three channels and manual control are left as they are.
 */
static bool
behaviour_widens(
	uint8_t behaviour)
{
	unsigned mode;

	/* The behaviour's three bits. */
	mode = (behaviour & ADT_BEHAVIOUR_MASK) >> ADT_BEHAVIOUR_SHIFT;

	/* The modes that only gain from following all three channels. */
	switch (mode) {
	case ADT_BEHAVIOUR_REMOTE1:
	case ADT_BEHAVIOUR_LOCAL:
	case ADT_BEHAVIOUR_REMOTE2:
	case ADT_BEHAVIOUR_OFF:
	case ADT_BEHAVIOUR_LOCAL_REMOTE2:
		return true;
	default:
		break;
	}

	/* Full speed, all channels already, or manual. */
	return false;
}

/*
 * Lowers one limit register to the ceiling when the chip holds a higher
 * temperature in it; a lower or equal one is left.
 */
static int
lower_limit(
	const struct drv_adt746x *device,
	uint8_t number,
	int32_t ceiling,
	struct drv_adt746x_report *report)
{
	uint8_t before;
	uint8_t after;
	int32_t present;
	int error;

	/* The limit the chip holds now. */
	error = read_register(device, number, &before);
	if (error != 0)
		return error;
	present = decode_limit(device, before);

	/* A limit at or below the ceiling already makes the fan turn as early. */
	if (present <= ceiling)
		return 0;

	/* The ceiling, read back. */
	after = encode(device, ceiling);
	error = change_register(device, number, before, after, report);
	if (error != 0)
		return error;

	/* Succeeded: the limit is the ceiling, or the report says why not. */
	return 0;
}

/*
 * Writes one register, reads it back and records the change; a register
 * that does not keep the value ends the set-up.
 */
static int
change_register(
	const struct drv_adt746x *device,
	uint8_t number,
	uint8_t before,
	uint8_t after,
	struct drv_adt746x_report *report)
{
	struct drv_adt746x_change *change;
	uint8_t kept;
	int error;

	/* The write. */
	error = device->access.write(device->access.context, number, after);
	if (error != 0)
		return error;

	/* Records it, as written, before it is checked. */
	if (report->change_count < DRV_ADT746X_CHANGES_MAX) {
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
		report->outcome = DRV_ADT746X_NOT_KEPT;

	/* Succeeded: the write was made and checked. */
	return 0;
}
