/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac laptop's fan controller, as the kernel uses it.
 *
 * The PowerBook G4 and the iBook G4 cool themselves with an ADT7460 or an
 * ADT7467 on the system controller's I2C bus, which runs the fans by its
 * own automatic control.  The firmware names the chip, its bus and its
 * address; the driver checks the chip's ids, logs what it holds, and lets
 * the control cool the machine: limits higher than the driver's are
 * lowered and each fan follows every temperature, writes that can only
 * make a fan turn faster (mac99-adt746x.c).  The chip's own control stays
 * in charge, so the fans keep being run if the kernel stops.  The 12-inch
 * PowerBook G4 has an ADM1030 there instead (mac99-adm1030.c), whose
 * control the driver only makes start the fan earlier.
 *
 * The temperatures (three, or the ADM1030's two) are pushed to the thermal core every five
 * seconds and the fans are read by it once a second, so /dev/thermal (the
 * thermal command) shows them.  The log holds the chip's settings before
 * and after the set-up, and every register written.
 */

#include "drivers/platform/mac99/mac99-fan.h"
#include "drivers/platform/mac99/mac99-adm1030.h"
#include "drivers/platform/mac99/mac99-adt746x.h"
#include "drivers/platform/mac99/mac99-i2c.h"
#include <drivers/i2c/i2c.h>
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#if CONFIG_KERN_THERMAL
#include <kern/thermal.h>
#endif

#include <stddef.h>
#include <stdint.h>

/*
 * The words of the hardware layer's "mac99.fan": the I2C controller's
 * registers, their step, the channel, the chip's bus address, the chip,
 * the bus's rate in kHz, the sensor parameters' version and the length of
 * the sensors' places, which follow as bytes.
 */
#define FAN_TABLE_REGISTERS		0U
#define FAN_TABLE_STEP			1U
#define FAN_TABLE_CHANNEL		2U
#define FAN_TABLE_ADDRESS		3U
#define FAN_TABLE_CHIP			4U
#define FAN_TABLE_RATE			5U
#define FAN_TABLE_VERSION		6U
#define FAN_TABLE_PLACES_LENGTH		7U
#define FAN_TABLE_PLACES		8U

/* The chip the handoff names for an ADM1030 (1 and 2 are the ADT7460 and the ADT7467). */
#define FAN_CHIP_ADM1030		3U

/* The sensor parameters' version the driver understands, as Apple's firmware writes it on these machines. */
#define FAN_PARAMETERS_VERSION		1U

/* The bus's speed the registry is told (the controller keeps the firmware's rate). */
#define FAN_BUS_SPEED			DRV_I2C_SPEED_STANDARD

/* How often the temperatures are pushed to the thermal core. */
#define FAN_READ_PERIOD_MS		5000U

/* The levels a fan's duty cycle is reported in: 0 to 255. */
#define FAN_LEVELS			256U

/* The longest line of the sensors' places logged. */
#define FAN_PLACES_LINE			80U

/* Marks a parameter a function takes but does not use. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The chip, identified once by drv_mac99_fan_init() and only read after:
 * the chip's registers are reached through the I2C registry, which keeps
 * the thread's and the thermal core's reads apart.
 */
static struct drv_adt746x chip;

/* The ADM1030, when the firmware named one instead; filled and used the same way. */
static struct drv_adm1030 adm1030;
static bool is_adm1030;

/* The bus the chip is on, and its bus address; set once before the chip is used. */
static struct drv_i2c_bus *bus;
static uint16_t chip_address;

/* The thread's sleep between two reads of the temperatures. */
static struct spinlock sleep_lock;

#if CONFIG_KERN_THERMAL
/* The thermal core's handles of the three temperatures, filled once before the thread starts. */
static struct kern_thermal_sensor *sensors[DRV_ADT746X_CHANNELS];

/* The thermal core's names of the temperatures and the fans. */
static const char *const sensor_names[DRV_ADT746X_CHANNELS] = {
	"adt746x-local",
	"adt746x-remote1",
	"adt746x-remote2"
};
static const char *const fan_names[DRV_ADT746X_FANS_MAX] = {
	"adt746x-fan1",
	"adt746x-fan2"
};

/* The thermal core's names of the ADM1030's temperatures and fan. */
static const char *const adm1030_sensor_names[DRV_ADM1030_CHANNELS] = {
	"adm1030-local",
	"adm1030-remote"
};
static const char *const adm1030_fan_name = "adm1030-fan";

/* Each fan's index, the argument the thermal core hands its read. */
static unsigned fan_indexes[DRV_ADT746X_FANS_MAX] = {
	0U,
	1U
};

static int fan_read(void *context, struct kern_thermal_fan_state *state);
static void worker(void *argument);

/* How the thermal core reads a fan. */
static const struct kern_thermal_fan_ops fan_ops = {
	.read = fan_read,
	.boost = NULL
};
#endif

static int register_read(void *context, uint8_t number, uint8_t *value);
static int register_write(void *context, uint8_t number, uint8_t value);
static void log_places(const uint32_t *table);
static void log_settings(const char *when);
static void log_report(const struct drv_adt746x_report *report);
static int start_adm1030(const uint32_t *table, bool may_write);
static void log_adm1030_report(const struct drv_adm1030_report *report);
static void log_adm1030_settings(const char *when);
static int publish(void);

/*
 * Finds the fan controller, logs it, sets its control up, and publishes
 * its temperatures and fans.
 */
int
drv_mac99_fan_init(
	bool may_write)
{
	struct drv_adt746x_access access;
	struct drv_adt746x_report report;
	const uint32_t *table;
	const char *name;
	int error;

	/* The fan controller the firmware named; a board without one has nothing to drive. */
	table = kern_boot_handoff("mac99.fan");
	if (table == NULL || table[FAN_TABLE_REGISTERS] == 0) {
		kern_logf("mac99-fan: the firmware names no fan controller\n");
		return ENODEV;
	}

	/* Sensor parameters of another version are not the layout the driver knows (the ADM1030's node has none). */
	if (table[FAN_TABLE_CHIP] != FAN_CHIP_ADM1030 && table[FAN_TABLE_VERSION] != FAN_PARAMETERS_VERSION) {
		kern_logf("mac99-fan: sensor parameters version %u not known; the fan controller is left alone\n",
			  (unsigned)table[FAN_TABLE_VERSION]);
		return ENODEV;
	}

	/* The bus the chip is on. */
	error = drv_mac99_i2c_attach(table[FAN_TABLE_REGISTERS],
				     table[FAN_TABLE_STEP],
				     table[FAN_TABLE_CHANNEL],
				     table[FAN_TABLE_RATE],
				     &bus);
	if (error != 0) {
		kern_logf("mac99-fan: I2C controller at %x unavailable (%d)\n",
			  (unsigned)table[FAN_TABLE_REGISTERS],
			  error);
		return error;
	}
	chip_address = (uint16_t)table[FAN_TABLE_ADDRESS];
	spin_init(&sleep_lock, LOCK_RANK_DEVICE, "mac99 fan thread");

	/* The ADM1030 has its own set-up. */
	if (table[FAN_TABLE_CHIP] == FAN_CHIP_ADM1030) {
		error = start_adm1030(table, may_write);
		if (error != 0)
			return error;
		return 0;
	}

	/* The chip, by its ids; another chip than the one named is left alone. */
	access.read = register_read;
	access.write = register_write;
	access.context = NULL;
	error = drv_adt746x_identify(&access, table[FAN_TABLE_CHIP], &chip);
	if (error != 0) {
		kern_logf("mac99-fan: no chip the firmware named at I2C %x channel %u address %02x (%d); left alone\n",
			  (unsigned)table[FAN_TABLE_REGISTERS],
			  (unsigned)table[FAN_TABLE_CHANNEL],
			  (unsigned)chip_address,
			  error);
		return error;
	}

	/* Where it is and what it holds. */
	name = "ADT7467";
	if (chip.chip == DRV_ADT746X_ADT7460)
		name = "ADT7460";
	kern_logf("mac99-fan: %s rev %02x at I2C %x channel %u address %02x, %s temperatures\n",
		  name,
		  (unsigned)chip.revision,
		  (unsigned)table[FAN_TABLE_REGISTERS],
		  (unsigned)table[FAN_TABLE_CHANNEL],
		  (unsigned)chip_address,
		  chip.offset64 ? "offset-64" : "two's complement");
	log_places(table);
	log_settings("before");

	/* Lets the chip's control cool the machine, unless the command line asked for reading only. */
	if (!may_write) {
		kern_logf("mac99-fan: reading only, by the command line; nothing written\n");
	} else {
		error = drv_adt746x_set_up(&chip, &report);
		log_report(&report);
		if (error != 0)
			kern_logf("mac99-fan: the set-up stopped (%d)\n", error);
		if (report.change_count != 0)
			log_settings("after");
	}

	/* The temperatures and the fans, to the thermal core. */
	error = publish();
	if (error != 0)
		return error;

	/* Succeeded: the chip's control runs the fans, and the kernel watches them. */
	return 0;
}

/* Reads one of the chip's registers: its number written, then the byte read after a repeated start. */
static int
register_read(
	void *context,
	uint8_t number,
	uint8_t *value)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The transfer on the chip's bus. */
	error = drv_i2c_transfer(bus, chip_address, FAN_BUS_SPEED, &number, 1, value, 1);
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	return 0;
}

/* Writes one of the chip's registers: its number, then the byte. */
static int
register_write(
	void *context,
	uint8_t number,
	uint8_t value)
{
	uint8_t bytes[2];
	int error;

	UNUSED_PARAMETER(context);

	/* The transfer on the chip's bus. */
	bytes[0] = number;
	bytes[1] = value;
	error = drv_i2c_transfer(bus, chip_address, FAN_BUS_SPEED, bytes, sizeof(bytes), NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded: the register was written. */
	return 0;
}

/* Logs the places of the sensors the firmware lists, NUL-separated in the handoff, on one line. */
static void
log_places(
	const uint32_t *table)
{
	char line[FAN_PLACES_LINE];
	const char *places;
	uint32_t length;
	uint32_t index;

	/* The places' bytes, cut at what a line holds. */
	places = (const char *)&table[FAN_TABLE_PLACES];
	length = table[FAN_TABLE_PLACES_LENGTH];
	if (length >= sizeof(line))
		length = sizeof(line) - 1U;
	if (length == 0)
		return;

	/* Each NUL between two places becomes a comma. */
	for (index = 0; index < length; index++) {
		line[index] = places[index];
		if (line[index] == '\0')
			line[index] = ',';
	}

	/* The trailing NULs end the line. */
	while (length != 0 && line[length - 1U] == ',')
		length--;
	line[length] = '\0';

	/* The places, in the firmware's order. */
	kern_logf("mac99-fan: the firmware's sensor places: %s\n", line);
}

/* Logs the chip's configuration, temperatures, limits and fans. */
static void
log_settings(
	const char *when)
{
	struct drv_adt746x_settings settings;
	unsigned fan;
	uint32_t rpm;
	uint8_t duty;
	int error;

	/* The settings; a chip that does not answer says so. */
	error = drv_adt746x_read_settings(&chip, &settings);
	if (error != 0) {
		kern_logf("mac99-fan: %s: the chip did not answer (%d)\n", when, error);
		return;
	}

	/* The temperatures and the two limits, local, remote 1, remote 2, in degrees Celsius. */
	kern_logf("mac99-fan: %s: config %02x temp %d/%d/%d start %d/%d/%d full %d/%d/%d\n",
		  when,
		  (unsigned)settings.configuration,
		  (int)settings.temperature[DRV_ADT746X_LOCAL],
		  (int)settings.temperature[DRV_ADT746X_REMOTE1],
		  (int)settings.temperature[DRV_ADT746X_REMOTE2],
		  (int)settings.start[DRV_ADT746X_LOCAL],
		  (int)settings.start[DRV_ADT746X_REMOTE1],
		  (int)settings.start[DRV_ADT746X_REMOTE2],
		  (int)settings.full[DRV_ADT746X_LOCAL],
		  (int)settings.full[DRV_ADT746X_REMOTE1],
		  (int)settings.full[DRV_ADT746X_REMOTE2]);

	/* Each fan: its behaviour register, duty cycle, minimum, and speed. */
	for (fan = 0; fan < chip.fan_count; fan++) {
		rpm = 0;
		duty = 0;
		error = drv_adt746x_read_fan(&chip, fan, &rpm, &duty);
		if (error != 0)
			rpm = 0;
		kern_logf("mac99-fan: %s: fan%u behaviour %02x duty %u minimum %u speed %u rpm\n",
			  when,
			  fan + 1U,
			  (unsigned)settings.behaviour[fan],
			  (unsigned)settings.duty[fan],
			  (unsigned)settings.minimum[fan],
			  (unsigned)rpm);
	}
}

/*
 * Identifies the ADM1030 the firmware named, logs what it holds, lets its
 * automatic control start the fan earlier (unless may_write is false),
 * and publishes its temperatures and fan.
 */
static int
start_adm1030(
	const uint32_t *table,
	bool may_write)
{
	struct drv_adm1030_access access;
	struct drv_adm1030_report report;
	int error;

	/* The chip, by its ids; another chip is left alone. */
	access.read = register_read;
	access.write = register_write;
	access.context = NULL;
	error = drv_adm1030_identify(&access, &adm1030);
	if (error != 0) {
		kern_logf("mac99-fan: no ADM1030 at I2C %x channel %u address %02x (%d); left alone\n",
			  (unsigned)table[FAN_TABLE_REGISTERS],
			  (unsigned)table[FAN_TABLE_CHANNEL],
			  (unsigned)chip_address,
			  error);
		return error;
	}
	is_adm1030 = true;

	/* Where it is and what it holds. */
	kern_logf("mac99-fan: ADM1030 rev %02x at I2C %x channel %u address %02x\n",
		  (unsigned)adm1030.revision,
		  (unsigned)table[FAN_TABLE_REGISTERS],
		  (unsigned)table[FAN_TABLE_CHANNEL],
		  (unsigned)chip_address);
	log_adm1030_settings("before");

	/* Lets the chip's control start the fan earlier, unless the command line asked for reading only. */
	if (!may_write) {
		kern_logf("mac99-fan: reading only, by the command line; nothing written\n");
	} else {
		error = drv_adm1030_set_up(&adm1030, &report);
		log_adm1030_report(&report);
		if (error != 0)
			kern_logf("mac99-fan: the set-up stopped (%d)\n", error);
		if (report.change_count != 0)
			log_adm1030_settings("after");
	}

	/* The temperatures and the fan, to the thermal core. */
	error = publish();
	if (error != 0)
		return error;

	/* Succeeded: the kernel watches the chip. */
	return 0;
}

/* Logs the ADM1030's configuration, temperatures, limits and fan. */
static void
log_adm1030_settings(
	const char *when)
{
	struct drv_adm1030_settings settings;
	uint32_t rpm;
	int error;

	/* The settings; a chip that does not answer says so. */
	error = drv_adm1030_read_settings(&adm1030, &settings);
	if (error != 0) {
		kern_logf("mac99-fan: %s: the chip did not answer (%d)\n", when, error);
		return;
	}

	/* The configuration and each channel (local/remote) in degrees Celsius. */
	kern_logf("mac99-fan: %s: config %02x %02x temp %d/%d offset %d/%d high %d/%d low %d/%d therm %d/%d auto %02x/%02x\n",
		  when,
		  (unsigned)settings.configuration1,
		  (unsigned)settings.configuration2,
		  (int)settings.temperature[DRV_ADM1030_LOCAL],
		  (int)settings.temperature[DRV_ADM1030_REMOTE],
		  (int)settings.offset[DRV_ADM1030_LOCAL],
		  (int)settings.offset[DRV_ADM1030_REMOTE],
		  (int)settings.high[DRV_ADM1030_LOCAL],
		  (int)settings.high[DRV_ADM1030_REMOTE],
		  (int)settings.low[DRV_ADM1030_LOCAL],
		  (int)settings.low[DRV_ADM1030_REMOTE],
		  (int)settings.therm[DRV_ADM1030_LOCAL],
		  (int)settings.therm[DRV_ADM1030_REMOTE],
		  (unsigned)settings.automatic[DRV_ADM1030_LOCAL],
		  (unsigned)settings.automatic[DRV_ADM1030_REMOTE]);

	/* The fan's registers and its speed. */
	rpm = 0;
	error = drv_adm1030_read_fan(&adm1030, &rpm);
	if (error != 0)
		rpm = 0;
	kern_logf("mac99-fan: %s: fan tach %02x limit %02x char %02x speed %02x filter %02x therm-fan %02x %u rpm\n",
		  when,
		  (unsigned)settings.tachometer,
		  (unsigned)settings.tachometer_limit,
		  (unsigned)settings.characteristics,
		  (unsigned)settings.speed,
		  (unsigned)settings.filter,
		  (unsigned)settings.behaviour,
		  (unsigned)rpm);
}

/* Logs how the ADM1030's set-up ended and every register it wrote. */
static void
log_adm1030_report(
	const struct drv_adm1030_report *report)
{
	const struct drv_adm1030_change *change;
	const char *outcome;
	unsigned index;

	/* How it ended. */
	switch (report->outcome) {
	case DRV_ADM1030_SET_UP:
		outcome = "set up";
		break;
	case DRV_ADM1030_SOFTWARE:
		outcome = "left alone: the fan is under software control";
		break;
	case DRV_ADM1030_NOT_MONITORING:
		outcome = "left alone: monitoring is off";
		break;
	case DRV_ADM1030_IMPLAUSIBLE:
		outcome = "left alone: a temperature is not one a running machine has";
		break;
	case DRV_ADM1030_UNREADABLE:
		outcome = "left alone: the chip did not answer";
		break;
	case DRV_ADM1030_NOT_KEPT:
		outcome = "stopped: a register did not keep what was written";
		break;
	default:
		outcome = "ended";
		break;
	}
	kern_logf("mac99-fan: %s, %u registers written\n", outcome, report->change_count);

	/* Each register written, in order. */
	for (index = 0; index < report->change_count; index++) {
		change = &report->changes[index];
		kern_logf("mac99-fan: register %02x %02x -> %02x\n",
			  (unsigned)change->number,
			  (unsigned)change->before,
			  (unsigned)change->after);
	}
}

/* Logs how the set-up ended and every register it wrote. */
static void
log_report(
	const struct drv_adt746x_report *report)
{
	const struct drv_adt746x_change *change;
	const char *outcome;
	unsigned index;

	/* How it ended. */
	switch (report->outcome) {
	case DRV_ADT746X_SET_UP:
		outcome = "set up";
		break;
	case DRV_ADT746X_NOT_STARTED:
		outcome = "left alone: monitoring is off";
		break;
	case DRV_ADT746X_LOCKED:
		outcome = "left alone: the limits are locked";
		break;
	case DRV_ADT746X_IMPLAUSIBLE:
		outcome = "left alone: a temperature is not one a running machine has";
		break;
	case DRV_ADT746X_UNREADABLE:
		outcome = "left alone: the chip did not answer";
		break;
	case DRV_ADT746X_NOT_KEPT:
		outcome = "stopped: a register did not keep what was written";
		break;
	default:
		outcome = "ended";
		break;
	}
	kern_logf("mac99-fan: %s, %u registers written\n", outcome, report->change_count);

	/* Each register written, in order. */
	for (index = 0; index < report->change_count; index++) {
		change = &report->changes[index];
		kern_logf("mac99-fan: register %02x %02x -> %02x\n",
			  (unsigned)change->number,
			  (unsigned)change->before,
			  (unsigned)change->after);
	}
}

/*
 * Registers the temperatures and the fans with the thermal core and starts
 * the thread that pushes the temperatures; without the thermal core
 * nothing is published and the log is all there is.
 */
static int
publish(
	void)
{
#if CONFIG_KERN_THERMAL
	const char *const *names;
	struct thread *thread;
	unsigned channel_count;
	unsigned channel;
	unsigned fan;
	int error;

	/* The chip's temperatures: the ADT746x's three, or the ADM1030's two. */
	names = sensor_names;
	channel_count = DRV_ADT746X_CHANNELS;
	if (is_adm1030) {
		names = adm1030_sensor_names;
		channel_count = DRV_ADM1030_CHANNELS;
	}

	/* Each temperature, a device's own reading. */
	for (channel = 0; channel < channel_count; channel++) {
		error = kern_thermal_sensor_register(names[channel], KERN_THERMAL_KIND_DEVICE, 0, &sensors[channel]);
		if (error != 0) {
			kern_logf("mac99-fan: temperature %s not published (%d)\n", names[channel], error);
			sensors[channel] = NULL;
		}
	}

	/* The fans the chip drives here, their highest speed not known. */
	if (is_adm1030) {
		error = kern_thermal_fan_register(adm1030_fan_name, 0, &fan_ops, &fan_indexes[0]);
		if (error != 0)
			kern_logf("mac99-fan: fan %s not published (%d)\n", adm1030_fan_name, error);
	} else {
		for (fan = 0; fan < chip.fan_count; fan++) {
			error = kern_thermal_fan_register(fan_names[fan], 0, &fan_ops, &fan_indexes[fan]);
			if (error != 0)
				kern_logf("mac99-fan: fan %s not published (%d)\n", fan_names[fan], error);
		}
	}

	/* The thread that pushes the temperatures. */
	error = kthread_create(worker, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("mac99-fan: the temperatures' thread did not start (%d)\n", error);
		return error;
	}
	thread_start(thread);
#endif

	/* Succeeded: what the kernel has is published. */
	return 0;
}

#if CONFIG_KERN_THERMAL
/* Reads one fan for the thermal core: its speed, and for an ADT746x its duty cycle as the level. */
static int
fan_read(
	void *context,
	struct kern_thermal_fan_state *state)
{
	const unsigned *fan;
	uint32_t rpm;
	uint8_t duty;
	int error;

	/* The ADM1030's fan: its speed only. */
	if (is_adm1030) {
		error = drv_adm1030_read_fan(&adm1030, &rpm);
		if (error != 0)
			return error;
		state->flags = KERN_THERMAL_FAN_HAS_RPM;
		state->rpm = rpm;
		state->level = 0;
		state->level_count = 0;
		state->boost_count = 0;
		return 0;
	}

	/* An ADT746x's fan: its speed and duty cycle. */
	fan = context;
	error = drv_adt746x_read_fan(&chip, *fan, &rpm, &duty);
	if (error != 0)
		return error;

	/* Succeeded: the speed, and the duty cycle the chip's control chose. */
	state->flags = KERN_THERMAL_FAN_HAS_RPM | KERN_THERMAL_FAN_HAS_LEVEL;
	state->rpm = rpm;
	state->level = duty;
	state->level_count = FAN_LEVELS;
	state->boost_count = 0;
	return 0;
}

/*
 * Pushes the temperatures to the thermal core every five seconds, for the
 * life of the system.
 */
static void
worker(
	void *argument)
{
	int32_t millicelsius[DRV_ADT746X_CHANNELS];
	bool valid[DRV_ADT746X_CHANNELS];
	unsigned channel_count;
	unsigned channel;
	unsigned long irq;
	int error;

	UNUSED_PARAMETER(argument);

	/* Reads and pushes, then sleeps, for the life of the system. */
	for (;;) {
		/* The chip's temperatures; the ADM1030 has no fault code, so its readings are taken as they are. */
		if (is_adm1030) {
			channel_count = DRV_ADM1030_CHANNELS;
			error = drv_adm1030_read_temperatures(&adm1030, millicelsius);
			valid[DRV_ADM1030_LOCAL] = true;
			valid[DRV_ADM1030_REMOTE] = true;
		} else {
			channel_count = DRV_ADT746X_CHANNELS;
			error = drv_adt746x_read_temperatures(&chip, millicelsius, valid);
		}

		/* Each one, none when the chip did not answer. */
		for (channel = 0; channel < channel_count; channel++) {
			if (error != 0) {
				millicelsius[channel] = 0;
				valid[channel] = false;
			}
			kern_thermal_sensor_update(sensors[channel], millicelsius[channel], valid[channel]);
		}

		/* Sleeps to the next read. */
		irq = spin_lock_irqsave(&sleep_lock);
		sched_sleep_locked(sched_ticks() + kern_ms_to_ticks(FAN_READ_PERIOD_MS), &sleep_lock);
		spin_unlock_irqrestore(&sleep_lock, irq);
	}
}
#endif
