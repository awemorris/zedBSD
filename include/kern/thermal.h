/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The thermal core's interfaces for the rest of the kernel and for the
 * drivers: the strategy and the receivers that carry it out, the
 * temperature sensors, the fans, the energy meters, the ioctl ranges of
 * /dev/thermal and the events its readers see.
 *
 * The core keeps a copy of everything and hands out the policy; the
 * drivers read and write the hardware.  The meaning of a strategy is the
 * core's, how a receiver maps it onto its hardware is the receiver's.
 *
 * The core has three threads: the strategy thread, which calls the
 * receivers and may wait on firmware; the poll thread, which reads the
 * fans and the meters and passes the passive ceiling, once a second; and
 * the guard, which watches the sensors and calls no driver.
 */

#ifndef KERN_KERN_THERMAL_H
#define KERN_KERN_THERMAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct file;
struct kern_thermal_sensor;

/*
 * The strategies, with the values the readers of /dev/thermal see.
 * FIRMWARE is the state from boot until the first choice and is never
 * chosen; a receiver given it writes nothing.
 */
#define KERN_THERMAL_STRATEGY_FIRMWARE		0U
#define KERN_THERMAL_STRATEGY_PERFORMANCE	1U
#define KERN_THERMAL_STRATEGY_BALANCED		2U
#define KERN_THERMAL_STRATEGY_POWERSAVE		3U
#define KERN_THERMAL_STRATEGY_MANUAL		4U

/* The largest value of a manual axis. */
#define KERN_THERMAL_AXIS_MAX			100U

/* What a receiver's apply adds to its answer, as bits. */
#define KERN_THERMAL_APPLY_UNCONFIRMED		0x1U	/* asked for, not yet confirmed in force */

/* The domain argument of a ceiling that stands for every domain. */
#define KERN_THERMAL_DOMAIN_ALL			0xffffffffU

/* A ceiling, in permille, that does not cut anything. */
#define KERN_THERMAL_LIMIT_NONE			1000U

/* The kinds of temperature sensor. */
#define KERN_THERMAL_KIND_ZONE			1U	/* a firmware thermal zone with trip points */
#define KERN_THERMAL_KIND_DEVICE		2U	/* a device's own reading */

/* What a sensor stands for, as bits. */
#define KERN_THERMAL_SENSOR_CPU			0x1U

/* What a fan reports, as bits. */
#define KERN_THERMAL_FAN_HAS_RPM		0x1U
#define KERN_THERMAL_FAN_HAS_LEVEL		0x2U

/* The kinds of energy meter. */
#define KERN_THERMAL_METER_PACKAGE		1U
#define KERN_THERMAL_METER_CORES		2U
#define KERN_THERMAL_METER_GRAPHICS		3U
#define KERN_THERMAL_METER_PLATFORM		4U
#define KERN_THERMAL_METER_DRAM			5U

/* The kinds of event, with the values the readers of /dev/thermal see. */
#define KERN_THERMAL_EVENT_STRATEGY		1U
#define KERN_THERMAL_EVENT_BOOST		2U
#define KERN_THERMAL_EVENT_PASSIVE		3U
#define KERN_THERMAL_EVENT_CRITICAL		4U
#define KERN_THERMAL_EVENT_EMERGENCY		5U
#define KERN_THERMAL_EVENT_FAN_STALLED		6U
#define KERN_THERMAL_EVENT_CPU			7U
#define KERN_THERMAL_EVENT_OVERFLOW		8U

/* The ioctl numbers each part of /dev/thermal owns. */
#define KERN_THERMAL_IOCTL_CORE_FIRST		1U
#define KERN_THERMAL_IOCTL_CORE_LAST		31U
#define KERN_THERMAL_IOCTL_CPU_FIRST		32U
#define KERN_THERMAL_IOCTL_CPU_LAST		63U

/*
 * One strategy as the core hands it to a receiver.  The three axes are
 * 0..KERN_THERMAL_AXIS_MAX for MANUAL, where 100 is the most of the thing
 * (the fastest, the loudest allowed, the finest gating), and 0 otherwise.
 */
struct kern_thermal_strategy {
	uint32_t value;			/* KERN_THERMAL_STRATEGY_* */
	uint8_t speed;
	uint8_t noise;
	uint8_t gating;
	uint8_t reserved;
};

/*
 * What a receiver does for the core.
 *
 * The core calls every member with none of its own locks held, and a
 * receiver calls no function of the core from them but
 * kern_thermal_event().
 *
 * apply runs in the strategy thread and may sleep.  *flags is 0 on entry
 * and may get KERN_THERMAL_APPLY_UNCONFIRMED.  It returns 0, EOPNOTSUPP
 * when the strategy has nothing to act on here, or an errno; the core
 * records the answer and does not retry.
 *
 * limit runs in the poll thread and sets a ceiling in permille of the
 * receiver's range on one of its own domains or on
 * KERN_THERMAL_DOMAIN_ALL; it must not wait for long.  An error is logged
 * once and the core keeps stepping.
 *
 * poll runs in the poll thread once a second, for a receiver that keeps a
 * copy of something the hardware updates on its own.
 *
 * is_custom is read without a lock and must be an atomic read.
 *
 * firmware_name runs in the thread of a GET ioctl and may sleep.
 *
 * Any member but apply may be NULL.
 */
struct kern_thermal_receiver_ops {
	int (*apply)(void *context, const struct kern_thermal_strategy *strategy, uint32_t *flags);
	int (*limit)(void *context, uint32_t domain, uint32_t permille);
	void (*poll)(void *context);
	bool (*is_custom)(void *context);
	int (*firmware_name)(void *context, char *name, size_t size);
};

/* What one fan reports when the core reads it. */
struct kern_thermal_fan_state {
	uint32_t flags;			/* KERN_THERMAL_FAN_HAS_* */
	uint32_t rpm;
	uint32_t level;			/* the automatic control's level */
	uint32_t level_count;
	uint32_t boost_count;		/* 0: no boost */
};

/*
 * What a fan's driver does for the core.  read runs in the poll thread;
 * boost runs in the strategy thread and never sets the fan below the
 * automatic control's level.  boost may be NULL.
 */
struct kern_thermal_fan_ops {
	int (*read)(void *context, struct kern_thermal_fan_state *state);
	int (*boost)(void *context, uint32_t level);
};

/*
 * What an energy meter's driver does for the core.
 *
 * read runs in the poll thread, once a second, on whichever CPU that
 * thread is on, and reports the raw count of a counter count_bits wide
 * (at most 32).  One count is unit_numerator / unit_denominator
 * nanojoules, with both below 2^32, so a second's difference times the
 * numerator fits 64 bits.  The core keeps the 64-bit sum, so a reader
 * sees energy at most a second old.  A counter that only one package's
 * CPUs can read is registered only on a machine of one package.
 */
struct kern_thermal_meter_ops {
	uint32_t count_bits;
	uint32_t unit_numerator;
	uint32_t unit_denominator;
	int (*read)(void *context, uint64_t *count);
};

/* Handles one ioctl of /dev/thermal in a registered range. */
typedef int (*kern_thermal_ioctl_t)(struct file *file, unsigned long request, uintptr_t argument);

/*
 * Brings the thermal core up.  Registrations made before it are kept and
 * served once it runs.
 */
void
kern_thermal_init(void);

/*
 * Registers a receiver of the strategy and of the thermal ceiling.
 *
 * Thread context.  The name is copied.  A receiver registered while a
 * strategy other than FIRMWARE is in force is given it.  Returns 0,
 * EINVAL or ENOSPC.
 */
int
kern_thermal_receiver_register(
	const char *name,
	const struct kern_thermal_receiver_ops *ops,
	void *context);

/*
 * Registers the handler of a range of /dev/thermal's ioctl numbers.
 * Thread context.  Returns 0, EINVAL, EBUSY for an overlap, or ENOSPC.
 */
int
kern_thermal_ioctl_range_register(
	unsigned first,
	unsigned last,
	kern_thermal_ioctl_t handler);

/* Registers a temperature sensor; the name is copied.  Thread context. */
int
kern_thermal_sensor_register(
	const char *name,
	uint32_t kind,
	uint32_t flags,
	struct kern_thermal_sensor **sensor);

/*
 * Pushes a sensor's latest reading.  Thread context, with no lock of the
 * sensor's source held.
 */
void
kern_thermal_sensor_update(
	struct kern_thermal_sensor *sensor,
	int32_t millicelsius,
	bool valid);

/* Sets a sensor's passive and critical levels; 0 is none.  Thread context. */
void
kern_thermal_sensor_levels(
	struct kern_thermal_sensor *sensor,
	int32_t passive,
	int32_t critical);

/* Registers a fan; the name is copied.  Thread context. */
int
kern_thermal_fan_register(
	const char *name,
	uint32_t rpm_max,
	const struct kern_thermal_fan_ops *ops,
	void *context);

/* Registers an energy meter; the name is copied.  Thread context. */
int
kern_thermal_meter_register(
	const char *name,
	uint32_t kind,
	const struct kern_thermal_meter_ops *ops,
	void *context);

/*
 * Posts an event to the readers of /dev/thermal.  Thread context; takes
 * only the leaf lock of the readers' rings, so it may be called with
 * other locks held.
 */
void
kern_thermal_event(
	uint32_t type,
	uint32_t index,
	int64_t value);

/* Restarts the watch after a system sleep.  Thread context. */
void
kern_thermal_resume(void);

#endif
