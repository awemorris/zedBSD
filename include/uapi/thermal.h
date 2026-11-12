/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/thermal and /dev/thermal-inject
 *
 * /dev/thermal (node root:desktop 0664, KERN_THERMAL_NODE_GROUP and
 * KERN_THERMAL_NODE_MODE) shows the machine's temperatures, fans, energy
 * meters and CPUs, and takes the strategy that every part of the machine
 * follows (the CPUs' requests, a laptop's firmware mode, a fan's curve).
 * Anyone may open it for reading and read; a change (SET_*) is refused
 * with EPERM unless the descriptor was opened for writing, which the
 * node's mode allows root and the desktop group's members.
 * EMERGENCY_ACK is taken only from init.
 *
 * read() gives whole struct thermal_event records of the types the open
 * subscribed to (KERN_THERMAL_SUBSCRIBE; an open that has not subscribed is
 * refused with EINVAL, a buffer too small for one record with EINVAL).  It
 * waits for one unless the file is non-blocking (EAGAIN); poll() is readable
 * while one waits.  Each open keeps up to KERN_THERMAL_EVENT_QUEUE records:
 * when they overflow the oldest go, and the next read starts with an
 * OVERFLOW event whose value is how many went.
 *
 * /dev/thermal-inject (node 0600) exists only in a kernel built for tests.
 * It adds made-up sensors and strategy receivers, and chooses how a thermal
 * emergency stops the machine, so that a virtual machine can exercise the
 * paths a real one takes only when it overheats.
 *
 * Every structure has the same size and layout on ILP32 and LP64.  Fields
 * named reserved are written as zero by the caller and by the kernel; a SET
 * with a nonzero reserved field is refused with EINVAL.  Fields marked out
 * are ignored on the way in.
 *
 * The rules every request follows:
 *  - The kernel matches the whole request number (direction, size, group
 *    and number); any other value is refused with ENOTTY, so a structure
 *    that grows later gets a new number and an old program keeps working.
 *  - A request that fails copies nothing out.  The few outcomes a caller
 *    needs back are answered with 0 and a flag instead of an errno.
 *  - An index (in) past the last one is refused with ENOENT; the counts are
 *    in struct thermal_status, and every GET_* answers its count as well.
 *  - /dev/thermal answers numbers 1..63 only; /dev/thermal-inject has its
 *    own entry and answers 64.. only.
 *  - Anyone may read the temperatures, fans, energy meters and CPUs, but a
 *    request that wakes a CPU (KERN_THERMAL_CPU_READ_SAMPLE, GET_CPU_IDLE)
 *    needs a descriptor opened for writing, as a change does: it disturbs
 *    the very idle state it measures.
 */

#ifndef KERN_UAPI_THERMAL_H
#define KERN_UAPI_THERMAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <uapi/ioctl.h>

/* The group letter that every ioctl of /dev/thermal and /dev/thermal-inject carries. */
#define KERN_THERMAL_IOC_GROUP		'T'

/*
 * The group /dev/thermal belongs to, the desktop group of /etc/group, and
 * the node's mode: root and the group's members open it for writing,
 * anyone for reading.  /etc/thermal.conf, where /sbin/thermal keeps the
 * chosen strategy, has the same group and mode.
 */
#define KERN_THERMAL_NODE_GROUP			45U
#define KERN_THERMAL_NODE_MODE			0664U

/* The lengths of the names, with the terminating NUL. */
#define KERN_THERMAL_NAME_MAX			32U
#define KERN_THERMAL_RECEIVER_NAME_MAX		16U
#define KERN_THERMAL_FIRMWARE_NAME_MAX		16U

/* The CPUs the CPU requests can name: struct thermal_cpu_domain's cpus holds this many bits. */
#define KERN_THERMAL_CPU_MAX			512U

/* The index that asks for every fan or every CPU domain at once, or an event about no one thing. */
#define KERN_THERMAL_ALL			0xffffffffU

/* A value the kernel does not know (a relative performance, an anchor). */
#define KERN_THERMAL_UNKNOWN			0xffffffffU

/* The records each open keeps before the oldest go. */
#define KERN_THERMAL_EVENT_QUEUE		64U

/* The opens that may subscribe at once; one more is refused with ENOSPC. */
#define KERN_THERMAL_READER_MAX			16U

/*
 * The strategies.  FIRMWARE is the state from boot until a strategy is
 * chosen: nothing is written and the firmware's own setting stays.  It can
 * be read but not chosen.  MANUAL alone carries the three axes.
 */
#define KERN_THERMAL_STRATEGY_FIRMWARE		0U
#define KERN_THERMAL_STRATEGY_PERFORMANCE	1U
#define KERN_THERMAL_STRATEGY_BALANCED		2U
#define KERN_THERMAL_STRATEGY_POWERSAVE		3U
#define KERN_THERMAL_STRATEGY_MANUAL		4U

/* The largest value of a MANUAL axis: 100 is the most of that thing. */
#define KERN_THERMAL_AXIS_MAX			100U

/* What a strategy's outcome says (struct thermal_strategy's flags). */
#define KERN_THERMAL_STRATEGY_CUSTOM		0x1U	/* a direct CPU setting is in force */
#define KERN_THERMAL_STRATEGY_PARTIAL		0x2U	/* some receivers failed */
#define KERN_THERMAL_STRATEGY_PENDING		0x4U	/* the receivers have not all answered in 5 s */
#define KERN_THERMAL_STRATEGY_SUPERSEDED	0x8U	/* a later choice replaced this one */

/* What a receiver's last answer says (struct thermal_receiver_info's flags). */
#define KERN_THERMAL_APPLY_UNCONFIRMED		0x1U	/* asked for, not yet confirmed in force */

/* The kinds of sensor. */
#define KERN_THERMAL_KIND_ZONE			1U	/* a firmware thermal zone with trip points */
#define KERN_THERMAL_KIND_DEVICE		2U	/* a device's own reading */

/* What a sensor is (struct thermal_sensor_info's flags). */
#define KERN_THERMAL_SENSOR_CPU			0x1U	/* it follows the CPUs (passive cooling acts on it) */

/* What is known of a sensor's reading (struct thermal_sensor_info's reading). */
#define KERN_THERMAL_READING_VALID		0x1U	/* the temperature is a reading */
#define KERN_THERMAL_READING_PASSIVE		0x2U	/* the passive level is known */
#define KERN_THERMAL_READING_CRITICAL		0x4U	/* the critical level is known */
#define KERN_THERMAL_READING_STALE		0x8U	/* no reading came for 30 s */
#define KERN_THERMAL_READING_ARMED		0x10U	/* the critical level passed the kernel's checks and stops the machine */

/* What a fan has and how its last reading went (struct thermal_fan_info's flags). */
#define KERN_THERMAL_FAN_HAS_RPM		0x1U	/* its speed can be read */
#define KERN_THERMAL_FAN_HAS_LEVEL		0x2U	/* its automatic control has levels */
#define KERN_THERMAL_FAN_KNOWN			0x10U	/* it has been read once */
#define KERN_THERMAL_FAN_STALE			0x20U	/* the last read failed */
#define KERN_THERMAL_FAN_STALLED		0x40U	/* it should turn and has not in 3 readings (15 s) */
#define KERN_THERMAL_FAN_CAN_BOOST		0x80U	/* SET_BOOST can raise it */

/* The kinds of energy meter. */
#define KERN_THERMAL_METER_PACKAGE		1U
#define KERN_THERMAL_METER_CORES		2U
#define KERN_THERMAL_METER_GRAPHICS		3U
#define KERN_THERMAL_METER_PLATFORM		4U
#define KERN_THERMAL_METER_DRAM			5U

/* How a meter's last reading went (struct thermal_meter_info's flags). */
#define KERN_THERMAL_METER_KNOWN		0x1U	/* it has been read once */
#define KERN_THERMAL_METER_STALE		0x2U	/* the last read failed */

/* What a boost request says when it returns (struct thermal_boost's flags). */
#define KERN_THERMAL_BOOST_PENDING		0x1U	/* the fan's driver has not answered in 5 s */

/* The states of a thermal emergency (struct thermal_status's emergency). */
#define KERN_THERMAL_EMERGENCY_NONE		0U
#define KERN_THERMAL_EMERGENCY_SIGNALED		1U	/* it began; init is or is being told and has not answered */
#define KERN_THERMAL_EMERGENCY_INIT		2U	/* init acknowledged and stops the machine */
#define KERN_THERMAL_EMERGENCY_CUTTING		3U	/* the kernel cuts the power itself */
#define KERN_THERMAL_EMERGENCY_HALTED		4U	/* the power stayed on; the machine halted */

/*
 * The types of event; a subscription's mask has bit (1 << type) for each
 * type it takes.  OVERFLOW comes whether or not it is in the mask; after it
 * the reader asks for the present state again with the GET_* requests.
 */
#define KERN_THERMAL_EVENT_STRATEGY		1U	/* the receivers answered; index: the strategy, value: its generation */
#define KERN_THERMAL_EVENT_BOOST		2U	/* a boost took effect; index: fan, value: level */
#define KERN_THERMAL_EVENT_PASSIVE		3U	/* index: sensor, value: 1 reached, 0 left 3 °C below */
#define KERN_THERMAL_EVENT_CRITICAL		4U	/* index: sensor, value: 1 reached, 0 left 3 °C below */
#define KERN_THERMAL_EVENT_EMERGENCY		5U	/* once, when it begins; index: sensor, value: millicelsius */
#define KERN_THERMAL_EVENT_FAN_STALLED		6U	/* once, when it stalls; index: fan, value: 0 */
#define KERN_THERMAL_EVENT_CPU			7U	/* index: KERN_THERMAL_ALL, value: the CPUs' new generation */
#define KERN_THERMAL_EVENT_OVERFLOW		8U	/* index: 0, value: how many records went */
#define KERN_THERMAL_EVENT_MASK			0x1feU	/* the bits SUBSCRIBE takes; any other is EINVAL */

/* The kinds of a CPU (struct thermal_cpu_info's core_class). */
#define KERN_THERMAL_CPU_CORE_UNKNOWN		0U
#define KERN_THERMAL_CPU_CORE_PERFORMANCE	1U
#define KERN_THERMAL_CPU_CORE_EFFICIENCY	2U

/* What a CPU report should do (struct thermal_cpu_info's request). */
#define KERN_THERMAL_CPU_READ_SAMPLE		0x1U	/* wake the CPU to measure its delivered rate */

/* What a CPU has (struct thermal_cpu_info's flags). */
#define KERN_THERMAL_CPU_HAS_CONTROL		0x1U	/* a request can be written */
#define KERN_THERMAL_CPU_ESTIMATED_MHZ		0x2U	/* the MHz fields are estimates */
#define KERN_THERMAL_CPU_LIMITED		0x4U	/* the thermal ceiling cuts the maximum */
#define KERN_THERMAL_CPU_PLATFORM_LIMIT		0x8U	/* the platform holds the CPU below the request */
#define KERN_THERMAL_CPU_CUSTOM			0x10U	/* a direct setting is in force */
#define KERN_THERMAL_CPU_SAMPLE_TIMEOUT		0x20U	/* READ_SAMPLE: the CPU did not answer; the rest is filled */

/* How a domain's levels are counted (struct thermal_cpu_domain's kind). */
#define KERN_THERMAL_CPU_LEVELS_NONE		0U	/* the domain cannot be controlled */
#define KERN_THERMAL_CPU_LEVELS_RANGE		1U	/* every whole number from steps[0] to steps[1] */
#define KERN_THERMAL_CPU_LEVELS_STEPS		2U	/* only the step_count values of steps[] */

/* What a domain's members share (struct thermal_cpu_domain's share). */
#define KERN_THERMAL_CPU_SHARE_THREAD		1U
#define KERN_THERMAL_CPU_SHARE_CORE		2U
#define KERN_THERMAL_CPU_SHARE_MODULE		3U
#define KERN_THERMAL_CPU_SHARE_PACKAGE		4U

/* What a domain's request says (struct thermal_cpu_domain's flags). */
#define KERN_THERMAL_CPU_DOMAIN_CUSTOM		0x1U	/* a direct setting is in force */
#define KERN_THERMAL_CPU_DOMAIN_LIMITED		0x2U	/* the thermal ceiling cuts the maximum */
#define KERN_THERMAL_CPU_DOMAIN_PENDING		0x4U	/* a member has not confirmed the request */

/* Which fields of a direct setting to change (struct thermal_cpu_setting's fields). */
#define KERN_THERMAL_CPU_SET_MINIMUM		0x1U
#define KERN_THERMAL_CPU_SET_MAXIMUM		0x2U
#define KERN_THERMAL_CPU_SET_DESIRED		0x4U
#define KERN_THERMAL_CPU_SET_PREFERENCE		0x8U

/* The ways the relative performance is measured (struct thermal_cpu_relative). */
#define KERN_THERMAL_CPU_RELATIVE_FREQUENCY	0x1U	/* the highest rate the kernel measured */
#define KERN_THERMAL_CPU_RELATIVE_FEEDBACK	0x2U	/* the hardware feedback's performance */
#define KERN_THERMAL_CPU_RELATIVE_PROVIDER	0x4U	/* the driver's own table */

/* The idle states one report holds at most. */
#define KERN_THERMAL_CPU_IDLE_STATE_MAX		8U

/* Where an idle state's residency is counted (struct thermal_cpu_idle's scope). */
#define KERN_THERMAL_CPU_IDLE_CORE		1U
#define KERN_THERMAL_CPU_IDLE_MODULE		2U
#define KERN_THERMAL_CPU_IDLE_PACKAGE		3U

/* The ways a thermal emergency may stop a machine under test. */
#define KERN_THERMAL_INJECT_PATH_INIT		0U	/* as on a real machine: SIGUSR2 to init first */
#define KERN_THERMAL_INJECT_PATH_DIRECT		1U	/* skip init: the kernel syncs and cuts the power */

/*
 * The machine's thermal state at a glance.
 *
 * The counts bound the indexes of GET_SENSOR, GET_FAN, GET_METER,
 * GET_RECEIVER, GET_CPU and GET_CPU_DOMAIN; they only grow while the system
 * runs.  strategy_error is the last strategy's outcome as SET_STRATEGY
 * returned it (a strategy every receiver failed is still the one in force).
 * The emergency fields stay once an emergency began, so a reader that
 * missed the event still learns of it.
 */
struct thermal_status {
	uint32_t sensor_count;
	uint32_t fan_count;
	uint32_t meter_count;
	uint32_t receiver_count;
	uint32_t strategy;		/* KERN_THERMAL_STRATEGY_* in force */
	uint32_t strategy_flags;	/* KERN_THERMAL_STRATEGY_CUSTOM and the others */
	uint32_t generation;		/* the strategy's generation */
	uint32_t passive_permille;	/* the passive ceiling in force, 1000: none */
	uint32_t emergency;		/* KERN_THERMAL_EMERGENCY_* */
	uint32_t emergency_sensor;	/* the sensor that began it, while it is not NONE */
	int32_t emergency_millicelsius;	/* that sensor's temperature then */
	int32_t strategy_error;		/* 0, EOPNOTSUPP or the first receiver's errno */
	uint64_t emergency_time_ns;	/* when it began, CLOCK_MONOTONIC; 0: never */
	uint32_t cpu_count;
	uint32_t cpu_domain_count;
	uint32_t cpu_package_count;
	uint32_t cpu_generation;	/* changes with the CPUs' topology, capabilities or relative performance */
	uint32_t reserved[2];
};

/*
 * One temperature sensor, asked for by index (in) and answered with the
 * number of sensors (count, so a reader can start at index 0 and walk).
 *
 * passive and critical are the sensor's levels, meaningful only when their
 * READING_ bits are set.  The kernel caps the CPUs above passive only on a
 * KERN_THERMAL_SENSOR_CPU sensor, and stops the machine only at the
 * critical level of a ZONE that passed its checks (READING_ARMED).
 */
struct thermal_sensor_info {
	uint32_t index;			/* in */
	uint32_t count;
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t kind;			/* KERN_THERMAL_KIND_* */
	uint32_t flags;			/* KERN_THERMAL_SENSOR_* */
	uint32_t reading;		/* KERN_THERMAL_READING_* */
	int32_t millicelsius;
	int32_t passive;		/* millicelsius */
	int32_t critical;		/* millicelsius */
	uint64_t time_ns;		/* when the temperature was taken, CLOCK_MONOTONIC; 0: never */
	uint32_t reserved[6];
};

/*
 * One fan, asked for by index.
 *
 * level is where the firmware's or the driver's automatic control has it,
 * boost the lower bound SET_BOOST asked for; the fan turns at the higher of
 * the two.  A fan without levels reports 0 for both counts.
 */
struct thermal_fan_info {
	uint32_t index;			/* in */
	uint32_t count;
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t flags;			/* KERN_THERMAL_FAN_* */
	uint32_t rpm;
	uint32_t rpm_max;		/* 0: not known */
	uint32_t level;
	uint32_t level_count;
	uint32_t boost;			/* the boost level in force, 0: none */
	uint32_t boost_count;		/* the levels SET_BOOST takes, 0: no boost */
	uint32_t reserved0;
	uint64_t time_ns;		/* when it was last read, CLOCK_MONOTONIC; 0: never */
	uint32_t reserved[4];
};

/*
 * One energy meter, asked for by index.
 *
 * energy_nj only grows: it is the energy since the kernel first read the
 * meter, kept in 64 bits across the hardware counter's wraps.  The power
 * over an interval is the difference of two readings over the difference
 * of their times.
 */
struct thermal_meter_info {
	uint32_t index;			/* in */
	uint32_t count;
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t kind;			/* KERN_THERMAL_METER_* */
	uint32_t flags;			/* KERN_THERMAL_METER_KNOWN, _STALE */
	uint64_t energy_nj;
	uint64_t time_ns;		/* when it was last read, CLOCK_MONOTONIC; 0: never */
	uint32_t reserved[4];
};

/*
 * The strategy: GET reads the one in force, SET chooses one.
 *
 * SET takes value and the three axes (each 0..100, nonzero only for
 * MANUAL; FIRMWARE is refused with EINVAL), hands them to every receiver
 * and waits for them for 5 s.  The strategy is kept in force whatever the
 * receivers answer.  SET returns EOPNOTSUPP when no receiver acts on a
 * strategy here, the first receiver's error when all that act on it
 * failed, and 0 otherwise, answering every out field with PARTIAL, PENDING
 * or SUPERSEDED telling what happened.  After a failure GET_STRATEGY reads
 * the generation and the error.  firmware_name is the firmware's present
 * mode as the first receiver that knows one names it ("Optimized"), empty
 * when none does.
 */
struct thermal_strategy {
	uint32_t value;			/* KERN_THERMAL_STRATEGY_* */
	uint8_t speed;			/* MANUAL: 100 is the fastest */
	uint8_t noise;			/* MANUAL: 100 allows the loudest fans */
	uint8_t gating;			/* MANUAL: 100 gates power the most finely */
	uint8_t reserved0;
	uint32_t flags;			/* out: KERN_THERMAL_STRATEGY_CUSTOM and the others */
	uint32_t generation;		/* out */
	char firmware_name[KERN_THERMAL_FIRMWARE_NAME_MAX];	/* out */
	uint32_t receiver_count;	/* out */
	int32_t error;			/* out: the outcome the last SET returned (GET) or 0 (SET) */
	uint32_t reserved[2];
};

/*
 * One receiver of the strategy (the CPUs, a laptop's firmware mode, a fan's
 * curve), asked for by index, with the outcome of its last strategy.
 */
struct thermal_receiver_info {
	uint32_t index;			/* in */
	uint32_t count;
	char name[KERN_THERMAL_RECEIVER_NAME_MAX];
	int32_t error;			/* 0, EOPNOTSUPP (does not apply here) or an errno */
	uint32_t flags;			/* KERN_THERMAL_APPLY_UNCONFIRMED */
	uint32_t generation;		/* the strategy generation it last carried out, 0: none */
	uint32_t duration_ms;		/* how long it took */
	uint64_t time_ns;		/* when it ended, CLOCK_MONOTONIC; 0: never */
	uint32_t reserved[4];
};

/*
 * A fan's boost: a lower bound on its level, kept until it is set back to
 * 0, across strategy changes.  fan is an index or KERN_THERMAL_ALL (which
 * skips the fans that cannot be boosted).  Returns EINVAL for a level
 * beyond the fan's boost_count (0 until the fan has been read once),
 * EOPNOTSUPP for a fan, or for ALL when no fan, that can be boosted, ENOENT
 * for no such fan.  With PENDING the driver's later answer is not returned:
 * the reader sees it in GET_FAN's boost and the BOOST event.
 */
struct thermal_boost {
	uint32_t fan;
	uint32_t level;
	uint32_t flags;			/* out: KERN_THERMAL_BOOST_PENDING */
	uint32_t reserved;
};

/*
 * The event types an open takes; replaces the previous mask.  A mask with a
 * bit outside KERN_THERMAL_EVENT_MASK is refused with EINVAL; mask 0 stops
 * the subscription, and read() is then refused with EINVAL.
 */
struct thermal_subscription {
	uint32_t mask;			/* bit (1 << KERN_THERMAL_EVENT_*) */
	uint32_t reserved[3];
};

/*
 * One event, as read() gives it.
 *
 * The record has struct system_event's size, and its size, value, sequence
 * and time at the same offsets (0, 12, 16, 24), so that one reader can take
 * both.  sequence counts every event the kernel posted, so it skips the
 * types an open does not take.  name is the sensor's or the fan's name when
 * index names one, empty otherwise.
 */
struct thermal_event {
	uint32_t size;			/* sizeof(struct thermal_event) */
	uint32_t type;			/* KERN_THERMAL_EVENT_* */
	uint32_t index;			/* see KERN_THERMAL_EVENT_* */
	int32_t value;			/* see KERN_THERMAL_EVENT_*; a generation is its bit pattern */
	uint64_t sequence;
	uint64_t time_ns;		/* when it was posted, CLOCK_MONOTONIC */
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t reserved[16];
};

/*
 * One CPU, asked for by cpu (in) and answered with the number of CPUs.
 *
 * The topology and the core's class come from the processor itself and are
 * there on every machine the driver knows; the rest is KERN_THERMAL_UNKNOWN
 * (or 0 for an MHz) until the driver can read it.  A request with
 * READ_SAMPLE (on a descriptor opened for writing only) wakes the CPU to
 * measure delivered_mhz and sleeps about 100 ms; without it nothing is
 * woken and delivered_mhz is the last sample.
 */
struct thermal_cpu_info {
	uint32_t cpu;			/* in */
	uint32_t count;
	uint32_t request;		/* in: KERN_THERMAL_CPU_READ_SAMPLE */
	uint32_t package;
	uint32_t die;
	uint32_t module;		/* cores that share a cache or a clock */
	uint32_t core;
	uint32_t thread;
	uint32_t core_class;		/* KERN_THERMAL_CPU_CORE_* */
	uint32_t core_type;		/* the processor's own code for its kind, 0: unknown */
	uint32_t firmware_id;		/* the firmware's processor number */
	uint32_t domain;		/* the domain its requests go to */
	uint32_t voltage_group;		/* CPUs that share a voltage */
	uint32_t relative;		/* 0..100, the fastest CPU is 100 */
	uint32_t available;		/* 0..255, the hardware feedback's performance */
	uint32_t efficiency;		/* 0..255, the hardware feedback's efficiency */
	uint32_t lowest;		/* the anchors in the processor's own units */
	uint32_t efficient;
	uint32_t guaranteed;
	uint32_t highest;
	uint32_t highest_mhz;		/* estimate, 0: unknown */
	uint32_t delivered_mhz;		/* estimate, 0: unknown */
	uint32_t flags;			/* KERN_THERMAL_CPU_HAS_CONTROL and the others */
	uint32_t reserved[9];
};

/*
 * One domain: the CPUs that take one performance request together, asked
 * for by domain (in) and answered with the number of domains.
 *
 * minimum, maximum, desired and preference are the request last asked for,
 * before the thermal ceiling (limit) cuts it; PENDING says a member has not
 * confirmed it yet.
 */
struct thermal_cpu_domain {
	uint32_t domain;		/* in */
	uint32_t count;
	uint64_t cpus[KERN_THERMAL_CPU_MAX / 64U];	/* CPU n is bit n % 64 of word n / 64 */
	uint32_t cpu_count;
	uint32_t kind;			/* KERN_THERMAL_CPU_LEVELS_* */
	uint32_t share;			/* KERN_THERMAL_CPU_SHARE_* */
	uint32_t step_count;		/* RANGE: 2; STEPS: 1..16 */
	uint32_t steps[16];		/* RANGE: steps[0] lowest, steps[1] highest; STEPS: ascending */
	uint32_t step_mhz[16];		/* estimate, 0: unknown */
	uint32_t minimum;
	uint32_t maximum;
	uint32_t desired;		/* 0 hands the choice back to the processor */
	uint32_t preference;		/* 0 performance .. 255 energy */
	uint32_t limit;			/* the thermal ceiling in units, KERN_THERMAL_UNKNOWN: none */
	uint32_t flags;			/* KERN_THERMAL_CPU_DOMAIN_* */
	uint32_t reserved[2];
};

/*
 * A direct setting of one domain, or of every domain at once
 * (domain KERN_THERMAL_ALL: minimum, maximum and a nonzero desired are then
 * permille of each domain's range; preference stays in its own 0..255).
 * It stays until the next strategy, and makes the strategy read CUSTOM
 * meanwhile.  A diagnostic, not a policy.
 */
struct thermal_cpu_setting {
	uint32_t domain;
	uint32_t fields;		/* KERN_THERMAL_CPU_SET_* */
	uint32_t minimum;
	uint32_t maximum;
	uint32_t desired;
	uint32_t preference;
	uint32_t reserved[2];
};

/* The ways the relative performance can be measured and the one in force. */
struct thermal_cpu_relative {
	uint32_t available;		/* out: KERN_THERMAL_CPU_RELATIVE_* bits */
	uint32_t source;		/* the one in force; SET: the one to choose */
	uint32_t reserved[2];
};

/*
 * The idle residency of one CPU's core (GET_CPU_IDLE, on a descriptor
 * opened for writing only, target a CPU; read on that CPU) or of one
 * package (GET_PACKAGE_IDLE, target a package number as GET_CPU reports
 * it; a number no CPU has is ENOENT, and count is then the number of CPUs,
 * the bound to walk).
 *
 * Each of the state_count entries is a state number as the processor
 * counts it (6 for C6), where it is counted (scope) and the time spent in
 * it since the counter started.  The difference of two reports over the
 * difference of their times is the share of time in that state.  available
 * has bit n set for each state n the processor offers, counted or not.
 */
struct thermal_cpu_idle {
	uint32_t target;		/* in: the CPU or the package */
	uint32_t count;			/* the number of CPUs or of packages */
	uint32_t state_count;
	uint32_t available;
	uint32_t state[KERN_THERMAL_CPU_IDLE_STATE_MAX];
	uint64_t residency_ns[KERN_THERMAL_CPU_IDLE_STATE_MAX];
	uint64_t time_ns;		/* when it was read, CLOCK_MONOTONIC */
	uint8_t scope[KERN_THERMAL_CPU_IDLE_STATE_MAX];	/* KERN_THERMAL_CPU_IDLE_* */
};

/*
 * A made-up sensor (/dev/thermal-inject).  The first injection of a name
 * registers the sensor; later ones with the same name update it, so a
 * test raises a temperature step by step.  Sensors are never removed and
 * share the 32 places of real ones (ENOSPC beyond).  An injected critical
 * level goes through the same checks as a real one.
 */
struct thermal_inject_sensor {
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t kind;			/* KERN_THERMAL_KIND_* */
	uint32_t flags;			/* KERN_THERMAL_SENSOR_* */
	int32_t millicelsius;
	int32_t passive;		/* millicelsius, 0: none */
	int32_t critical;		/* millicelsius, 0: none */
	uint32_t valid;			/* 0: the reading failed */
	uint32_t reserved[2];
};

/*
 * A made-up strategy receiver (/dev/thermal-inject): it answers every
 * strategy with error after delay_ms.  The same name updates the answer.
 */
struct thermal_inject_receiver {
	char name[KERN_THERMAL_RECEIVER_NAME_MAX];
	int32_t error;			/* 0, EOPNOTSUPP or an errno */
	uint32_t flags;			/* KERN_THERMAL_APPLY_UNCONFIRMED */
	uint32_t delay_ms;
	uint32_t reserved[5];
};

/* How the next thermal emergency stops the machine under test. */
struct thermal_inject_path {
	uint32_t path;			/* KERN_THERMAL_INJECT_PATH_* */
	uint32_t reserved[3];
};

/*
 * /dev/thermal: the machine's thermal state, sensors, fans, meters and the
 * strategy (1..31).  EMERGENCY_ACK is init's alone and only while the
 * emergency is SIGNALED (EPERM, EINVAL otherwise); init opens /dev/thermal
 * when it starts, so that it can answer within the kernel's 10 s.
 */
#define KERN_THERMAL_GET_STATUS \
	_IOR(KERN_THERMAL_IOC_GROUP, 1, struct thermal_status)
#define KERN_THERMAL_GET_SENSOR \
	_IOWR(KERN_THERMAL_IOC_GROUP, 2, struct thermal_sensor_info)
#define KERN_THERMAL_GET_FAN \
	_IOWR(KERN_THERMAL_IOC_GROUP, 3, struct thermal_fan_info)
#define KERN_THERMAL_GET_METER \
	_IOWR(KERN_THERMAL_IOC_GROUP, 4, struct thermal_meter_info)
#define KERN_THERMAL_GET_STRATEGY \
	_IOR(KERN_THERMAL_IOC_GROUP, 5, struct thermal_strategy)
#define KERN_THERMAL_SET_STRATEGY \
	_IOWR(KERN_THERMAL_IOC_GROUP, 6, struct thermal_strategy)
#define KERN_THERMAL_GET_RECEIVER \
	_IOWR(KERN_THERMAL_IOC_GROUP, 7, struct thermal_receiver_info)
#define KERN_THERMAL_SET_BOOST \
	_IOWR(KERN_THERMAL_IOC_GROUP, 8, struct thermal_boost)
#define KERN_THERMAL_SUBSCRIBE \
	_IOW(KERN_THERMAL_IOC_GROUP, 9, struct thermal_subscription)
#define KERN_THERMAL_EMERGENCY_ACK \
	_IO(KERN_THERMAL_IOC_GROUP, 10)

/* /dev/thermal: the CPUs (32..63). */
#define KERN_THERMAL_GET_CPU \
	_IOWR(KERN_THERMAL_IOC_GROUP, 32, struct thermal_cpu_info)
#define KERN_THERMAL_GET_CPU_DOMAIN \
	_IOWR(KERN_THERMAL_IOC_GROUP, 33, struct thermal_cpu_domain)
#define KERN_THERMAL_SET_CPU_DOMAIN \
	_IOW(KERN_THERMAL_IOC_GROUP, 34, struct thermal_cpu_setting)
#define KERN_THERMAL_GET_CPU_RELATIVE \
	_IOR(KERN_THERMAL_IOC_GROUP, 35, struct thermal_cpu_relative)
#define KERN_THERMAL_SET_CPU_RELATIVE \
	_IOW(KERN_THERMAL_IOC_GROUP, 36, struct thermal_cpu_relative)
#define KERN_THERMAL_GET_CPU_IDLE \
	_IOWR(KERN_THERMAL_IOC_GROUP, 37, struct thermal_cpu_idle)
#define KERN_THERMAL_GET_PACKAGE_IDLE \
	_IOWR(KERN_THERMAL_IOC_GROUP, 38, struct thermal_cpu_idle)

/* /dev/thermal-inject: tests only (64..). */
#define KERN_THERMAL_INJECT_SENSOR \
	_IOW(KERN_THERMAL_IOC_GROUP, 64, struct thermal_inject_sensor)
#define KERN_THERMAL_INJECT_RECEIVER \
	_IOW(KERN_THERMAL_IOC_GROUP, 65, struct thermal_inject_receiver)
#define KERN_THERMAL_INJECT_PATH \
	_IOW(KERN_THERMAL_IOC_GROUP, 66, struct thermal_inject_path)

/* Pins the size and the field offsets every process relies on, the same on ILP32 and LP64. */
_Static_assert(sizeof(struct thermal_status) == 80U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_status, emergency_time_ns) == 48U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_sensor_info) == 96U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_sensor_info, time_ns) == 64U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_fan_info) == 96U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_fan_info, time_ns) == 72U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_meter_info) == 80U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_meter_info, energy_nj) == 48U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_strategy) == 48U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_receiver_info) == 64U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_receiver_info, time_ns) == 40U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_boost) == 16U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_subscription) == 16U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_event) == 128U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_event, value) == 12U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_event, sequence) == 16U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_event, time_ns) == 24U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_cpu_info) == 128U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_cpu_domain) == 248U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_cpu_domain, cpus) == 8U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_cpu_setting) == 32U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_cpu_relative) == 16U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_cpu_idle) == 128U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct thermal_cpu_idle, residency_ns) == 48U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_inject_sensor) == 64U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_inject_receiver) == 48U, "thermal ABI must be identical on ILP32 and LP64");
_Static_assert(sizeof(struct thermal_inject_path) == 16U, "thermal ABI must be identical on ILP32 and LP64");

#ifdef __cplusplus
}
#endif

#endif
