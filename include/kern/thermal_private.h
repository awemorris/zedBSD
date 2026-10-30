/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The thermal core's interface to the device that serves /dev/thermal:
 * the copies a reader asks for, the strategy and the boost a root sets,
 * the dispatch of the ioctl ranges, and the rings that carry the events
 * to each open file.  Only the kernel's core includes this header; the
 * drivers use <kern/thermal.h>.
 *
 * The structures here are the kernel's own and are not the layout a
 * process sees; the device copies them into the records of its interface.
 */

#ifndef KERN_KERN_THERMAL_PRIVATE_H
#define KERN_KERN_THERMAL_PRIVATE_H

#include <kern/lock.h>
#include <kern/thermal.h>
#include <kern/waitq.h>
#include <stdbool.h>
#include <stdint.h>

/* The longest names the copies carry, with the terminating zero. */
#define KERN_THERMAL_NAME_MAX			32U
#define KERN_THERMAL_RECEIVER_NAME_MAX		16U
#define KERN_THERMAL_FIRMWARE_NAME_MAX		16U

/* The most of each thing the core keeps. */
#define KERN_THERMAL_RECEIVER_MAX		16U
#define KERN_THERMAL_IOCTL_RANGE_MAX		4U
#define KERN_THERMAL_SENSOR_MAX			32U
#define KERN_THERMAL_FAN_MAX			8U
#define KERN_THERMAL_METER_MAX			8U

/* The fan argument of a boost that stands for every fan. */
#define KERN_THERMAL_FAN_ALL			0xffffffffU

/* What the strategy's copy says beyond its value, as bits. */
#define KERN_THERMAL_STRATEGY_CUSTOM		0x1U	/* a root's direct setting is in force */
#define KERN_THERMAL_STRATEGY_PARTIAL		0x2U	/* some receivers failed */
#define KERN_THERMAL_STRATEGY_PENDING		0x4U	/* the receivers have not all answered */
#define KERN_THERMAL_STRATEGY_SUPERSEDED	0x8U	/* a later choice replaced this one */

/* What a sensor's copy says beyond its numbers, as bits. */
#define KERN_THERMAL_READING_VALID		0x1U	/* the temperature is a reading */
#define KERN_THERMAL_READING_PASSIVE		0x2U	/* the passive level is known */
#define KERN_THERMAL_READING_CRITICAL		0x4U	/* the critical level is known */
#define KERN_THERMAL_READING_STALE		0x8U	/* no reading came for a while */

/* What a fan's copy says beyond its numbers, as bits. */
#define KERN_THERMAL_FAN_COPY_KNOWN		0x1U	/* the fan has been read once */
#define KERN_THERMAL_FAN_COPY_STALE		0x2U	/* the last read failed */
#define KERN_THERMAL_FAN_COPY_STALLED		0x4U	/* it should turn and does not */
#define KERN_THERMAL_FAN_COPY_BOOST		0x8U	/* its driver can boost it */

/* What a meter's copy says beyond its numbers, as bits. */
#define KERN_THERMAL_METER_COPY_KNOWN		0x1U	/* the counter has been read once */
#define KERN_THERMAL_METER_COPY_STALE		0x2U	/* the last read failed */

/* What a boost request reports beyond its errno, as bits. */
#define KERN_THERMAL_BOOST_PENDING		0x1U	/* the fan's driver has not answered yet */

/* The events one open file holds before the oldest are dropped. */
#define KERN_THERMAL_READER_RING		64U

/* The core's counts and the strategy in short. */
struct kern_thermal_status {
	uint32_t sensor_count;
	uint32_t fan_count;
	uint32_t meter_count;
	uint32_t receiver_count;
	uint32_t strategy;		/* KERN_THERMAL_STRATEGY_* */
	uint32_t strategy_flags;	/* KERN_THERMAL_STRATEGY_CUSTOM and the others */
	uint32_t generation;
};

/* One temperature sensor as the core last saw it. */
struct kern_thermal_sensor_info {
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t kind;			/* KERN_THERMAL_KIND_* */
	uint32_t flags;			/* KERN_THERMAL_SENSOR_* */
	uint32_t reading;		/* KERN_THERMAL_READING_* */
	int32_t millicelsius;
	int32_t passive;		/* 0: none */
	int32_t critical;		/* 0: none */
	uint64_t time_ns;		/* when the temperature was taken, 0: never */
};

/* One fan as the core last read it. */
struct kern_thermal_fan_info {
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t copy;			/* KERN_THERMAL_FAN_COPY_* */
	uint32_t rpm_max;
	struct kern_thermal_fan_state state;
	uint32_t boost;			/* the boost level in force */
	uint64_t time_ns;		/* when it was last read, 0: never */
};

/* One energy meter as the core last summed it. */
struct kern_thermal_meter_info {
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t kind;			/* KERN_THERMAL_METER_PACKAGE and the others */
	uint32_t copy;			/* KERN_THERMAL_METER_COPY_* */
	uint64_t energy_nj;		/* since the first read */
	uint64_t time_ns;		/* when it was last read, 0: never */
};

/* The strategy in force and how far the receivers carried it out. */
struct kern_thermal_strategy_info {
	struct kern_thermal_strategy strategy;
	uint32_t flags;			/* KERN_THERMAL_STRATEGY_CUSTOM and the others */
	uint32_t generation;
	char firmware_name[KERN_THERMAL_FIRMWARE_NAME_MAX];
};

/* One receiver and the answer it gave last. */
struct kern_thermal_receiver_info {
	char name[KERN_THERMAL_RECEIVER_NAME_MAX];
	int32_t error;			/* 0, EOPNOTSUPP or an errno; 0 before any */
	uint32_t flags;			/* KERN_THERMAL_APPLY_UNCONFIRMED */
	uint32_t generation;		/* the generation it last carried out, 0: none */
	uint32_t duration_ms;		/* how long its last apply took */
	uint64_t time_ns;		/* when its last apply ended, 0: never */
};

/* One event as an open file receives it. */
struct kern_thermal_record {
	uint32_t type;			/* KERN_THERMAL_EVENT_* */
	uint32_t index;
	int64_t value;
	uint64_t time_ns;
};

/*
 * The events of one open file of /dev/thermal.
 *
 * The device embeds one in what it keeps for the open file, attaches it
 * at open and detaches it before freeing it.  While it is attached the
 * core appends the events its mask chooses, dropping the oldest when the
 * ring is full and counting them; the next take reports the drop as one
 * OVERFLOW event.  queue is woken at every event appended; its condition
 * lock is the core's event lock, which the take and the ready test take.
 */
struct kern_thermal_reader {
	struct kern_thermal_reader *next;
	uint32_t mask;			/* bit (1 << type) for each type taken */
	unsigned head;
	unsigned count;
	uint32_t dropped;
	bool attached;
	struct wait_queue queue;
	struct kern_thermal_record ring[KERN_THERMAL_READER_RING];
};

/* Reports the counts and the strategy in short.  Thread context. */
void
kern_thermal_status_get(
	struct kern_thermal_status *status);

/* Copies one sensor.  Thread context.  Returns 0 or ENOENT. */
int
kern_thermal_sensor_get(
	uint32_t index,
	struct kern_thermal_sensor_info *info);

/* Copies one fan.  Thread context.  Returns 0 or ENOENT. */
int
kern_thermal_fan_get(
	uint32_t index,
	struct kern_thermal_fan_info *info);

/* Copies one meter.  Thread context.  Returns 0 or ENOENT. */
int
kern_thermal_meter_get(
	uint32_t index,
	struct kern_thermal_meter_info *info);

/* Copies one receiver's last answer.  Thread context.  Returns 0 or ENOENT. */
int
kern_thermal_receiver_get(
	uint32_t index,
	struct kern_thermal_receiver_info *info);

/*
 * Reports the strategy in force.  Thread context; may sleep, since a
 * receiver may ask the firmware for the name of its mode.
 */
void
kern_thermal_strategy_get(
	struct kern_thermal_strategy_info *info);

/*
 * Chooses a strategy and waits up to five seconds for the receivers.
 *
 * Thread context; the caller has checked that a root asks.  Returns
 * EINVAL for FIRMWARE, an unknown value or axes that do not fit the
 * value; EOPNOTSUPP when no receiver acts on it (it is kept all the
 * same); the first errno when every receiver that acts on it failed;
 * otherwise 0, with *flags telling PARTIAL, PENDING or SUPERSEDED.
 */
int
kern_thermal_strategy_set(
	const struct kern_thermal_strategy *strategy,
	uint32_t *flags);

/*
 * Sets the boost level of one fan or of KERN_THERMAL_FAN_ALL and waits up
 * to five seconds for the fans' drivers.
 *
 * Thread context; the caller has checked that a root asks.  Returns
 * ENOENT for no such fan, EOPNOTSUPP when no fan named can be boosted,
 * EINVAL for a level past a fan's boost levels, the first errno of a
 * driver, or 0, with *flags telling KERN_THERMAL_BOOST_PENDING.
 */
int
kern_thermal_boost_set(
	uint32_t fan,
	uint32_t level,
	uint32_t *flags);

/*
 * Hands an ioctl of /dev/thermal to the handler of the range its number
 * falls in.  Thread context.  Returns ENOTTY for another group or a number
 * no range holds, or what the handler returns.
 */
int
kern_thermal_ioctl_dispatch(
	struct file *file,
	unsigned long request,
	uintptr_t argument);

/* Starts taking the events a mask chooses for an open file.  Thread context. */
void
kern_thermal_reader_attach(
	struct kern_thermal_reader *reader,
	uint32_t mask);

/* Changes the events an attached open file takes.  Thread context. */
void
kern_thermal_reader_subscribe(
	struct kern_thermal_reader *reader,
	uint32_t mask);

/* Stops taking events for an open file before it is freed.  Thread context. */
void
kern_thermal_reader_detach(
	struct kern_thermal_reader *reader);

/*
 * Moves the oldest events of an open file into records, at most capacity
 * of them, a drop first reported as one OVERFLOW event.  Thread context,
 * does not sleep.  Returns how many were moved.
 */
unsigned
kern_thermal_reader_take(
	struct kern_thermal_reader *reader,
	struct kern_thermal_record *records,
	unsigned capacity);

/* Tells whether an open file has an event to take.  Thread context. */
bool
kern_thermal_reader_ready(
	struct kern_thermal_reader *reader);

#endif
