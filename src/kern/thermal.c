/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The thermal core: the copy of every temperature sensor, fan and energy
 * meter the drivers registered, the strategy and the receivers that carry
 * it out, the boost of the fans, the events the readers of /dev/thermal
 * see, the ioctl ranges of that device, and sysctl hw.thermal, which it
 * fills from the copy.
 *
 * The drivers push their sensors' readings and the core reads the fans and
 * the meters itself.  Three threads do the work that may wait: the
 * strategy thread calls the receivers and the fans' boost, which may wait
 * on the firmware; the poll thread reads the meters and the receivers'
 * copies every second and the fans every five; the guard looks at the
 * sensors' copy every second and calls no driver.  A thread that asks for
 * a strategy or a boost waits for the strategy thread with a limit, and
 * never holds a lock of the core while a driver runs.
 *
 * Lock order: the copy lock, then the event lock, which is a leaf.  The
 * drivers' own locks are never held when the core is called, and the core
 * holds none of its own when it calls a driver.
 */

#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/sysctl.h>
#include <kern/thermal.h>
#include <kern/thermal_private.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long a choice of strategy or of boost waits for the drivers. */
#define THERMAL_ANSWER_WAIT_MS		5000U

/* How often the poll thread runs, and how many of its passes go between two reads of the fans. */
#define THERMAL_POLL_PERIOD_MS		1000U
#define THERMAL_FAN_POLLS		5U

/* How often the guard looks at the sensors, and how long a sensor may go without a reading before it is stale. */
#define THERMAL_GUARD_PERIOD_MS		1000U
#define THERMAL_STALE_MS		30000U

/* The guard runs ahead of ordinary kernel threads, so a busy machine still watches its temperature. */
#define THERMAL_GUARD_PRIORITY		(SCHED_PRIORITY_DEFAULT - 2)

/* How far below a level a temperature must fall before the level counts as left, in thousandths of a degree. */
#define THERMAL_HYSTERESIS		3000

/*
 * The temperatures a reading may have, in thousandths of a degree: a
 * value outside them is no reading.  A reading that jumps by the jump or
 * more from the last one taken is dropped once, so one bad read does not
 * move the copy, while a real step is taken at the next read.
 */
#define THERMAL_READING_LOW		(-40000)
#define THERMAL_READING_HIGH		150000
#define THERMAL_READING_JUMP		20000

/* A passive or critical level at or above this many thousandths of a degree is a firmware's way of saying none. */
#define THERMAL_LEVEL_NONE		127000

/* The fan reads in a row with no turn, while it should turn, that make it stalled. */
#define THERMAL_STALL_READS		3U

/* The group letter of /dev/thermal's ioctls, and where an ioctl number keeps its group and its number. */
#define THERMAL_IOCTL_GROUP		'T'
#define THERMAL_IOCTL_GROUP_SHIFT	8U
#define THERMAL_IOCTL_BYTE_MASK		0xffUL
#define THERMAL_IOCTL_NUMBER_MAX	255U

/* The sensors one source of hw.thermal lists; the core lists two sources to cover its table. */
#define THERMAL_SYSCTL_SOURCE_SENSORS	16U
#define THERMAL_SYSCTL_SOURCES		(KERN_THERMAL_SENSOR_MAX / THERMAL_SYSCTL_SOURCE_SENSORS)

/* How far the core's tables are prepared: not yet, being prepared by one caller, or ready. */
#define THERMAL_STATE_NONE		0U
#define THERMAL_STATE_PREPARING		1U
#define THERMAL_STATE_READY		2U

/*
 * One temperature sensor's copy.
 *
 * The name, kind and flags are set at registration and never change; the
 * rest is the copy lock's.  valid tells that the latest reading was one;
 * accepted that a reading was ever taken, and the last one taken is
 * millicelsius at taken_tick.  watch_tick is where the guard starts to
 * count a sensor's age after a system sleep.  The two crossed flags are
 * the guard's memory of the levels the temperature is at or past.
 */
struct kern_thermal_sensor {
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t kind;
	uint32_t flags;
	bool valid;
	bool accepted;
	bool jump_dropped;
	bool stale;
	bool passive_crossed;
	bool critical_crossed;
	int32_t millicelsius;
	int32_t passive;
	int32_t critical;
	uint64_t taken_tick;
	uint64_t watch_tick;
};

/*
 * One receiver of the strategy and its last answer.
 *
 * The name, operations and context are set at registration and never
 * change; the answer is written by the strategy thread under the copy
 * lock.  generation is the strategy's generation the receiver last
 * carried out, 0 before the first.
 */
struct thermal_receiver {
	char name[KERN_THERMAL_RECEIVER_NAME_MAX];
	const struct kern_thermal_receiver_ops *ops;
	void *context;
	uint32_t generation;
	int error;
	uint32_t flags;
	uint32_t duration_ms;
	uint64_t ended_tick;
};

/* One range of /dev/thermal's ioctl numbers and its handler, fixed at registration. */
struct thermal_range {
	unsigned first;
	unsigned last;
	kern_thermal_ioctl_t handler;
};

/*
 * One fan's copy.
 *
 * The name, maximum, operations and context are fixed at registration;
 * the rest is the copy lock's.  The poll thread writes what the fan's
 * driver read, and zero_reads counts its reads in a row that found the fan
 * still while it should turn.  A boost request sets boost_wanted and
 * stamps boost_ticket; the strategy thread hands it to the driver and
 * copies the stamp into boost_served, so the two differ while a request
 * waits, and keeps the driver's answer in boost_error.  boost_applied is
 * the level the driver last accepted.
 */
struct thermal_fan {
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t rpm_max;
	const struct kern_thermal_fan_ops *ops;
	void *context;
	uint32_t copy;
	struct kern_thermal_fan_state state;
	uint64_t read_tick;
	unsigned zero_reads;
	uint32_t boost_wanted;
	uint32_t boost_applied;
	uint32_t boost_ticket;
	uint32_t boost_served;
	int boost_error;
};

/*
 * One energy meter's copy.
 *
 * The name, kind, operations and context are fixed at registration.
 * Only the poll thread writes the rest, inside an odd sequence; a reader
 * copies it between two equal even values of the sequence, so a 64-bit
 * sum is never seen half written on a 32-bit processor.  last_count is the
 * counter's last raw value, and remainder carries the part of a nanojoule
 * a division left over to the next read.
 */
struct thermal_meter {
	char name[KERN_THERMAL_NAME_MAX];
	uint32_t kind;
	const struct kern_thermal_meter_ops *ops;
	void *context;
	volatile unsigned sequence;
	uint32_t copy;
	uint64_t last_count;
	uint64_t energy_nj;
	uint64_t remainder;
	uint64_t read_tick;
};

/*
 * How far the tables, locks and queues are prepared.
 *
 * Drivers register before kern_thermal_init() runs, so the first caller
 * of any entry point prepares them, and a caller that finds another one
 * preparing waits for it.  It only moves forward.
 */
static volatile unsigned thermal_state;

/*
 * The copy lock: every sensor's, fan's and receiver's copy, the strategy,
 * the boost requests and the strategy thread's work count.  It is held
 * with interrupts off and only for copying; no driver is called under it.
 */
static struct spinlock thermal_lock;

/*
 * The event lock: the list of open files' rings and the rings.  It is a
 * leaf, taken under the copy lock or under a driver's lock, and nothing
 * is taken under it.
 */
static struct spinlock thermal_event_lock;

/*
 * The receivers, in the order they registered, which is the order the
 * strategy thread calls them in.  An entry is filled under the copy lock
 * and published by raising the count with release; nothing is ever taken
 * out, so a reader that loaded the count with acquire may use the entries
 * below it and their fixed fields without the lock.
 */
static struct thermal_receiver thermal_receivers[KERN_THERMAL_RECEIVER_MAX];
static atomic_uint_t thermal_receiver_count;

/* The ioctl ranges, published as the receivers are. */
static struct thermal_range thermal_ranges[KERN_THERMAL_IOCTL_RANGE_MAX];
static atomic_uint_t thermal_range_count;

/* The sensors, published as the receivers are; a driver keeps a pointer to its own entry. */
static struct kern_thermal_sensor thermal_sensors[KERN_THERMAL_SENSOR_MAX];
static atomic_uint_t thermal_sensor_count;

/* The fans, published as the receivers are. */
static struct thermal_fan thermal_fans[KERN_THERMAL_FAN_MAX];
static atomic_uint_t thermal_fan_count;

/* The meters, published as the receivers are. */
static struct thermal_meter thermal_meters[KERN_THERMAL_METER_MAX];
static atomic_uint_t thermal_meter_count;

/*
 * The strategy in force and its generation, under the copy lock.  The
 * generation rises at every choice, skipping 0, which means no choice
 * yet; a receiver's generation tells which choice it last carried out.
 */
static struct kern_thermal_strategy thermal_strategy;
static uint32_t thermal_generation;

/*
 * The outcome of the last generation the strategy thread finished, under
 * the copy lock: which generation, the errno its chooser gets and the
 * flags (PARTIAL).  thermal_done_queue is woken when it changes.
 */
static uint32_t thermal_summary_generation;
static int thermal_summary_error;
static uint32_t thermal_summary_flags;

/*
 * The strategy thread's work, under the copy lock: a request raises
 * posted and wakes thermal_work_queue, and the thread copies posted into
 * seen before each pass, so a request made during a pass brings another.
 */
static uint32_t thermal_work_posted;
static uint32_t thermal_work_seen;

/*
 * The boost requests, under the copy lock: the stamp of the latest
 * request, the latest stamp the strategy thread finished and the first
 * errno of that pass.  thermal_done_queue is woken when it finishes.
 */
static uint32_t thermal_boost_ticket;
static uint32_t thermal_boost_done;
static int thermal_boost_error;

/*
 * The wait queues, all with the copy lock as their condition lock: the
 * strategy thread's requests, the answers its choosers wait for, and the
 * periods of the poll thread and of the guard, which nothing wakes.
 */
static struct wait_queue thermal_work_queue;
static struct wait_queue thermal_done_queue;
static struct wait_queue thermal_poll_queue;
static struct wait_queue thermal_guard_queue;

/* The open files that take events, under the event lock. */
static struct kern_thermal_reader *thermal_readers;

/*
 * The first sensor each source of hw.thermal lists; the address of an
 * entry is the source's context, which the sysctl tells apart.  Never
 * written.
 */
static unsigned thermal_sysctl_firsts[THERMAL_SYSCTL_SOURCES] = {
	0U,
	THERMAL_SYSCTL_SOURCE_SENSORS,
};

/* Set once kern_thermal_init() has started the threads; only the boot path reads and writes it. */
static bool thermal_started;

static void thermal_prepare(void);
static void thermal_copy_name(char *target, size_t size, const char *name);
static uint64_t thermal_now_ns(uint64_t tick);
static uint64_t thermal_period(uint32_t milliseconds);
static void thermal_start_thread(void (*entry)(void *), int priority, const char *name);
static void thermal_post_work(void);
static void thermal_sleep_until(struct wait_queue *queue, uint64_t deadline);
static uint64_t thermal_next_deadline(uint64_t deadline, uint64_t period);
static int thermal_strategy_check(const struct kern_thermal_strategy *strategy);
static uint32_t thermal_strategy_flags(void);
static bool thermal_custom(void);
static void thermal_work_main(void *argument);
static void thermal_work_wait(void);
static void thermal_strategy_pass(void);
static void thermal_strategy_summarize(uint32_t generation, uint32_t value);
static void thermal_boost_pass(void);
static void thermal_poll_main(void *argument);
static void thermal_poll_receivers(void);
static void thermal_poll_meters(void);
static void thermal_poll_fans(void);
static void thermal_meter_read(struct thermal_meter *meter);
static void thermal_meter_write_begin(struct thermal_meter *meter);
static void thermal_meter_write_end(struct thermal_meter *meter);
static void thermal_fan_read(uint32_t index);
static void thermal_guard_main(void *argument);
static void thermal_guard_pass(void);
static void thermal_guard_sensor(struct kern_thermal_sensor *sensor, uint32_t index, uint64_t now, uint64_t stale_after);
static int32_t thermal_level(int32_t millicelsius);
static void thermal_ring_append(struct kern_thermal_reader *reader, const struct kern_thermal_record *record);
static int thermal_sysctl_copy(void *context, struct thermal_entry *entries, unsigned capacity, unsigned *count);

/*
 * Brings the thermal core up.
 *
 * Lists the copy's two halves in hw.thermal and starts the three
 * threads.  Registrations made before it are kept: the strategy thread
 * serves a strategy chosen later to every receiver, and the poll thread
 * reads every fan and meter in the table.  Called once from the boot path.
 */
void
kern_thermal_init(void)
{
	unsigned source;
	int error;

	/* Prepares the tables, unless a driver's registration already did. */
	thermal_prepare();

	/* Brings the core up only once. */
	if (thermal_started)
		return;
	thermal_started = true;

	/* Lists the sensors' copy in hw.thermal, sixteen to a source. */
	for (source = 0; source < THERMAL_SYSCTL_SOURCES; source++) {
		error = kern_thermal_register(thermal_sysctl_copy, &thermal_sysctl_firsts[source]);
		if (error != 0)
			kern_logf("thermal: hw.thermal does not list sensors from %u (%d)\n", thermal_sysctl_firsts[source], error);
	}

	/* Starts the thread that calls the receivers and the fans' boost. */
	thermal_start_thread(thermal_work_main, SCHED_PRIORITY_DEFAULT, "strategy");

	/* Starts the thread that reads the fans, the meters and the receivers' copies. */
	thermal_start_thread(thermal_poll_main, SCHED_PRIORITY_DEFAULT, "poll");

	/* Starts the guard, ahead of ordinary kernel threads. */
	thermal_start_thread(thermal_guard_main, THERMAL_GUARD_PRIORITY, "guard");
}

/*
 * Registers a receiver of the strategy and of the thermal ceiling.
 *
 * The name is copied and cut to the copy's room.  A receiver registered
 * while a strategy is in force is handed it by the strategy thread.
 */
int
kern_thermal_receiver_register(
	const char *name,
	const struct kern_thermal_receiver_ops *ops,
	void *context)
{
	struct thermal_receiver *receiver;
	unsigned long irq;
	unsigned count;

	/* Refuses a receiver without a name or without apply. */
	if (name == NULL || name[0] == '\0')
		return EINVAL;
	if (ops == NULL || ops->apply == NULL)
		return EINVAL;

	/* Prepares the tables, for a receiver that comes before kern_thermal_init(). */
	thermal_prepare();

	/* Appends the receiver and publishes it; a strategy in force is passed on to it. */
	irq = spin_lock_irqsave(&thermal_lock);

	count = atomic_load_acquire(&thermal_receiver_count);

	/* Refuses a full table. */
	if (count >= KERN_THERMAL_RECEIVER_MAX) {
		spin_unlock_irqrestore(&thermal_lock, irq);
		return ENOSPC;
	}

	/* Fills the entry, then publishes it by raising the count. */
	receiver = &thermal_receivers[count];
	kern_memset(receiver, 0, sizeof(*receiver));
	thermal_copy_name(receiver->name, sizeof(receiver->name), name);
	receiver->ops = ops;
	receiver->context = context;
	atomic_store_release(&thermal_receiver_count, count + 1U);

	/* Hands a strategy in force to the new receiver. */
	if (thermal_strategy.value != KERN_THERMAL_STRATEGY_FIRMWARE)
		thermal_post_work();

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the receiver is in the table for good. */
	return 0;
}

/*
 * Registers the handler of a range of /dev/thermal's ioctl numbers.
 */
int
kern_thermal_ioctl_range_register(
	unsigned first,
	unsigned last,
	kern_thermal_ioctl_t handler)
{
	struct thermal_range *range;
	unsigned long irq;
	unsigned count;
	unsigned index;

	/* Refuses a range that is empty, starts at 0 or leaves the group's numbers, or has no handler. */
	if (handler == NULL)
		return EINVAL;
	if (first == 0U || first > last)
		return EINVAL;
	if (last > THERMAL_IOCTL_NUMBER_MAX)
		return EINVAL;

	/* Prepares the tables, for a range registered before kern_thermal_init(). */
	thermal_prepare();

	/* Appends the range unless it overlaps one already there, and publishes it. */
	irq = spin_lock_irqsave(&thermal_lock);

	count = atomic_load_acquire(&thermal_range_count);
	for (index = 0; index < count; index++) {
		range = &thermal_ranges[index];
		if (first <= range->last && range->first <= last) {
			spin_unlock_irqrestore(&thermal_lock, irq);
			return EBUSY;
		}
	}

	/* Refuses a full table. */
	if (count >= KERN_THERMAL_IOCTL_RANGE_MAX) {
		spin_unlock_irqrestore(&thermal_lock, irq);
		return ENOSPC;
	}

	/* Fills the entry, then publishes it by raising the count. */
	range = &thermal_ranges[count];
	range->first = first;
	range->last = last;
	range->handler = handler;
	atomic_store_release(&thermal_range_count, count + 1U);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the numbers of the range reach the handler. */
	return 0;
}

/*
 * Registers a temperature sensor.
 *
 * The name is copied and cut to the copy's room.  Returns 0 with the
 * sensor's handle, EINVAL for a bad argument, or ENOSPC when the table is
 * full.
 */
int
kern_thermal_sensor_register(
	const char *name,
	uint32_t kind,
	uint32_t flags,
	struct kern_thermal_sensor **sensor)
{
	struct kern_thermal_sensor *entry;
	unsigned long irq;
	unsigned count;

	/* Refuses a sensor without a name or a handle to return. */
	if (name == NULL || name[0] == '\0')
		return EINVAL;
	if (sensor == NULL)
		return EINVAL;

	/* Refuses a kind the core does not know. */
	if (kind != KERN_THERMAL_KIND_ZONE && kind != KERN_THERMAL_KIND_DEVICE)
		return EINVAL;

	/* Refuses a flag the core does not know. */
	if ((flags & ~KERN_THERMAL_SENSOR_CPU) != 0U)
		return EINVAL;

	/* Prepares the tables, for a sensor registered before kern_thermal_init(). */
	thermal_prepare();

	/* Appends the sensor without a reading and publishes it. */
	irq = spin_lock_irqsave(&thermal_lock);

	count = atomic_load_acquire(&thermal_sensor_count);

	/* Refuses a full table. */
	if (count >= KERN_THERMAL_SENSOR_MAX) {
		spin_unlock_irqrestore(&thermal_lock, irq);
		return ENOSPC;
	}

	/* Fills the entry, then publishes it by raising the count. */
	entry = &thermal_sensors[count];
	kern_memset(entry, 0, sizeof(*entry));
	thermal_copy_name(entry->name, sizeof(entry->name), name);
	entry->kind = kind;
	entry->flags = flags;
	entry->watch_tick = sched_ticks();
	atomic_store_release(&thermal_sensor_count, count + 1U);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the driver pushes its readings through the handle. */
	*sensor = entry;
	return 0;
}

/*
 * Pushes a sensor's latest reading.
 *
 * A reading the driver could not make, or one outside the temperatures a
 * sensor may have, leaves the copy without a valid reading.  A reading
 * that jumps far from the last one taken is dropped once.
 */
void
kern_thermal_sensor_update(
	struct kern_thermal_sensor *sensor,
	int32_t millicelsius,
	bool valid)
{
	unsigned long irq;
	int32_t distance;
	uint64_t now;

	/* Ignores a missing handle. */
	if (sensor == NULL)
		return;

	/* Notes when the reading came. */
	now = sched_ticks();

	/* Takes or drops the reading. */
	irq = spin_lock_irqsave(&thermal_lock);

	if (!valid) {
		/* The driver could not read the sensor. */
		sensor->valid = false;
	} else if (millicelsius < THERMAL_READING_LOW || millicelsius > THERMAL_READING_HIGH) {
		/* A value no sensor has is no reading. */
		sensor->valid = false;
	} else {
		/* Measures the jump from the last reading taken. */
		distance = millicelsius - sensor->millicelsius;
		if (distance < 0)
			distance = -distance;

		if (sensor->accepted &&
		    distance >= THERMAL_READING_JUMP &&
		    !sensor->jump_dropped) {
			/* Drops the first far jump; the next reading is taken even if it jumps too. */
			sensor->jump_dropped = true;
		} else {
			/* Takes the reading. */
			sensor->millicelsius = millicelsius;
			sensor->taken_tick = now;
			sensor->valid = true;
			sensor->accepted = true;
			sensor->jump_dropped = false;
		}
	}

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Sets a sensor's passive and critical levels.
 *
 * A level of 0 or below, or one at or above 127 degrees (how firmware
 * writes none), is kept as none.
 */
void
kern_thermal_sensor_levels(
	struct kern_thermal_sensor *sensor,
	int32_t passive,
	int32_t critical)
{
	unsigned long irq;
	int32_t passive_level;
	int32_t critical_level;

	/* Ignores a missing handle. */
	if (sensor == NULL)
		return;

	/* Turns the levels that mean none into 0. */
	passive_level = thermal_level(passive);
	critical_level = thermal_level(critical);

	/* Stores them. */
	irq = spin_lock_irqsave(&thermal_lock);

	sensor->passive = passive_level;
	sensor->critical = critical_level;

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Registers a fan.
 *
 * The name is copied and cut to the copy's room.  The poll thread reads
 * it from its next fan pass on.  Returns 0, EINVAL or ENOSPC.
 */
int
kern_thermal_fan_register(
	const char *name,
	uint32_t rpm_max,
	const struct kern_thermal_fan_ops *ops,
	void *context)
{
	struct thermal_fan *fan;
	unsigned long irq;
	unsigned count;

	/* Refuses a fan without a name or a way to read it. */
	if (name == NULL || name[0] == '\0')
		return EINVAL;
	if (ops == NULL || ops->read == NULL)
		return EINVAL;

	/* Prepares the tables, for a fan registered before kern_thermal_init(). */
	thermal_prepare();

	/* Appends the fan, not read yet, and publishes it. */
	irq = spin_lock_irqsave(&thermal_lock);

	count = atomic_load_acquire(&thermal_fan_count);

	/* Refuses a full table. */
	if (count >= KERN_THERMAL_FAN_MAX) {
		spin_unlock_irqrestore(&thermal_lock, irq);
		return ENOSPC;
	}

	/* Fills the entry, then publishes it by raising the count. */
	fan = &thermal_fans[count];
	kern_memset(fan, 0, sizeof(*fan));
	thermal_copy_name(fan->name, sizeof(fan->name), name);
	fan->rpm_max = rpm_max;
	fan->ops = ops;
	fan->context = context;

	/* Says whether the fan's driver can boost it. */
	if (ops->boost != NULL)
		fan->copy = KERN_THERMAL_FAN_COPY_BOOST;
	atomic_store_release(&thermal_fan_count, count + 1U);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the fan is in the table for good. */
	return 0;
}

/*
 * Registers an energy meter.
 *
 * The name is copied and cut to the copy's room.  The poll thread's first
 * read of it sets where its sum starts.  Returns 0, EINVAL or ENOSPC.
 */
int
kern_thermal_meter_register(
	const char *name,
	uint32_t kind,
	const struct kern_thermal_meter_ops *ops,
	void *context)
{
	struct thermal_meter *meter;
	unsigned long irq;
	unsigned count;

	/* Refuses a meter without a name or a way to read it. */
	if (name == NULL || name[0] == '\0')
		return EINVAL;
	if (ops == NULL || ops->read == NULL)
		return EINVAL;

	/* Refuses a kind the core does not know. */
	if (kind < KERN_THERMAL_METER_PACKAGE || kind > KERN_THERMAL_METER_DRAM)
		return EINVAL;

	/* Refuses a counter wider than 32 bits or a unit with a zero. */
	if (ops->count_bits == 0U || ops->count_bits > 32U)
		return EINVAL;
	if (ops->unit_numerator == 0U || ops->unit_denominator == 0U)
		return EINVAL;

	/* Prepares the tables, for a meter registered before kern_thermal_init(). */
	thermal_prepare();

	/* Appends the meter, not read yet, and publishes it. */
	irq = spin_lock_irqsave(&thermal_lock);

	count = atomic_load_acquire(&thermal_meter_count);

	/* Refuses a full table. */
	if (count >= KERN_THERMAL_METER_MAX) {
		spin_unlock_irqrestore(&thermal_lock, irq);
		return ENOSPC;
	}

	/* Fills the entry, then publishes it by raising the count. */
	meter = &thermal_meters[count];
	kern_memset(meter, 0, sizeof(*meter));
	thermal_copy_name(meter->name, sizeof(meter->name), name);
	meter->kind = kind;
	meter->ops = ops;
	meter->context = context;
	atomic_store_release(&thermal_meter_count, count + 1U);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the meter is in the table for good. */
	return 0;
}

/*
 * Posts an event to the readers of /dev/thermal.
 *
 * Every attached open file whose mask takes the type gets a copy with the
 * time; a full ring drops its oldest event.  A type past the mask's bits
 * is ignored.
 */
void
kern_thermal_event(
	uint32_t type,
	uint32_t index,
	int64_t value)
{
	struct kern_thermal_reader *reader;
	struct kern_thermal_record record;
	unsigned long irq;

	/* Ignores a type no mask can choose. */
	if (type == 0U || type >= 32U)
		return;

	/* Prepares the tables, for an event posted before kern_thermal_init(). */
	thermal_prepare();

	/* Builds the record once for every reader. */
	record.type = type;
	record.index = index;
	record.value = value;
	record.time_ns = thermal_now_ns(sched_ticks());

	/* Appends it to every ring that takes the type and wakes the reader. */
	irq = spin_lock_irqsave(&thermal_event_lock);

	for (reader = thermal_readers; reader != NULL; reader = reader->next) {
		if ((reader->mask & (1U << type)) == 0U)
			continue;
		thermal_ring_append(reader, &record);
		waitq_wake_all(&reader->queue);
	}

	spin_unlock_irqrestore(&thermal_event_lock, irq);
}

/*
 * Restarts the watch after a system sleep.
 *
 * The time asleep is not time a sensor went unread, so every sensor's age
 * is counted again from now.
 */
void
kern_thermal_resume(void)
{
	struct kern_thermal_sensor *sensor;
	unsigned long irq;
	unsigned count;
	unsigned index;
	uint64_t now;

	/* Prepares the tables, for a resume before kern_thermal_init(). */
	thermal_prepare();

	/* Counts every sensor's age from now. */
	now = sched_ticks();
	count = atomic_load_acquire(&thermal_sensor_count);
	irq = spin_lock_irqsave(&thermal_lock);

	for (index = 0; index < count; index++) {
		sensor = &thermal_sensors[index];
		sensor->watch_tick = now;
		sensor->stale = false;
	}

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Reports the counts and the strategy in short.
 */
void
kern_thermal_status_get(
	struct kern_thermal_status *status)
{
	unsigned long irq;
	uint32_t flags;
	bool custom;

	/* Prepares the tables, so the lock is ready. */
	thermal_prepare();

	/* Reads the receivers' direct settings without a lock. */
	custom = thermal_custom();

	/* Copies the counts and the strategy. */
	kern_memset(status, 0, sizeof(*status));
	irq = spin_lock_irqsave(&thermal_lock);

	status->sensor_count = atomic_load_acquire(&thermal_sensor_count);
	status->fan_count = atomic_load_acquire(&thermal_fan_count);
	status->meter_count = atomic_load_acquire(&thermal_meter_count);
	status->receiver_count = atomic_load_acquire(&thermal_receiver_count);
	status->strategy = thermal_strategy.value;
	status->generation = thermal_generation;
	flags = thermal_strategy_flags();

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Adds the direct setting to the flags. */
	if (custom)
		flags |= KERN_THERMAL_STRATEGY_CUSTOM;
	status->strategy_flags = flags;
}

/*
 * Copies one sensor.
 */
int
kern_thermal_sensor_get(
	uint32_t index,
	struct kern_thermal_sensor_info *info)
{
	struct kern_thermal_sensor *sensor;
	unsigned long irq;
	unsigned count;
	uint32_t reading;

	/* Refuses a sensor past the table. */
	thermal_prepare();
	count = atomic_load_acquire(&thermal_sensor_count);
	if (index >= count)
		return ENOENT;

	/* Copies the sensor and says what its copy holds. */
	sensor = &thermal_sensors[index];
	kern_memset(info, 0, sizeof(*info));
	irq = spin_lock_irqsave(&thermal_lock);

	kern_memcpy(info->name, sensor->name, sizeof(info->name));
	info->kind = sensor->kind;
	info->flags = sensor->flags;
	reading = 0;
	if (sensor->valid)
		reading |= KERN_THERMAL_READING_VALID;
	if (sensor->passive != 0)
		reading |= KERN_THERMAL_READING_PASSIVE;
	if (sensor->critical != 0)
		reading |= KERN_THERMAL_READING_CRITICAL;
	if (sensor->stale)
		reading |= KERN_THERMAL_READING_STALE;
	info->reading = reading;
	info->millicelsius = sensor->millicelsius;
	info->passive = sensor->passive;
	info->critical = sensor->critical;
	if (sensor->accepted)
		info->time_ns = thermal_now_ns(sensor->taken_tick);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the sensor's copy. */
	return 0;
}

/*
 * Copies one fan.
 */
int
kern_thermal_fan_get(
	uint32_t index,
	struct kern_thermal_fan_info *info)
{
	struct thermal_fan *fan;
	unsigned long irq;
	unsigned count;

	/* Refuses a fan past the table. */
	thermal_prepare();
	count = atomic_load_acquire(&thermal_fan_count);
	if (index >= count)
		return ENOENT;

	/* Copies the fan. */
	fan = &thermal_fans[index];
	kern_memset(info, 0, sizeof(*info));
	irq = spin_lock_irqsave(&thermal_lock);

	kern_memcpy(info->name, fan->name, sizeof(info->name));
	info->copy = fan->copy;
	info->rpm_max = fan->rpm_max;
	info->state = fan->state;
	info->boost = fan->boost_applied;
	if ((fan->copy & KERN_THERMAL_FAN_COPY_KNOWN) != 0U)
		info->time_ns = thermal_now_ns(fan->read_tick);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the fan's copy. */
	return 0;
}

/*
 * Copies one meter.
 *
 * The copy is taken between two equal even values of the meter's
 * sequence, retried while the poll thread writes it.
 */
int
kern_thermal_meter_get(
	uint32_t index,
	struct kern_thermal_meter_info *info)
{
	struct thermal_meter *meter;
	unsigned count;
	unsigned begin;
	unsigned end;
	uint32_t copy;
	uint64_t energy;
	uint64_t read_tick;

	/* Refuses a meter past the table. */
	thermal_prepare();
	count = atomic_load_acquire(&thermal_meter_count);
	if (index >= count)
		return ENOENT;

	/* Copies the fixed fields. */
	meter = &thermal_meters[index];
	kern_memset(info, 0, sizeof(*info));
	kern_memcpy(info->name, meter->name, sizeof(info->name));
	info->kind = meter->kind;

	/* Copies the sum between two equal even sequences. */
	for (;;) {
		begin = atomic_raw_load_acquire(&meter->sequence);
		if ((begin & 1U) != 0U) {
			atomic_spin_hint();
			continue;
		}
		copy = meter->copy;
		energy = meter->energy_nj;
		read_tick = meter->read_tick;
		atomic_acquire_fence();
		end = atomic_raw_load_relaxed(&meter->sequence);
		if (begin == end)
			break;
	}

	/* Fills in the sum and when it was read. */
	info->copy = copy;
	info->energy_nj = energy;
	if ((copy & KERN_THERMAL_METER_COPY_KNOWN) != 0U)
		info->time_ns = thermal_now_ns(read_tick);

	/* Succeeded: the meter's copy. */
	return 0;
}

/*
 * Copies one receiver's last answer.
 */
int
kern_thermal_receiver_get(
	uint32_t index,
	struct kern_thermal_receiver_info *info)
{
	struct thermal_receiver *receiver;
	unsigned long irq;
	unsigned count;

	/* Refuses a receiver past the table. */
	thermal_prepare();
	count = atomic_load_acquire(&thermal_receiver_count);
	if (index >= count)
		return ENOENT;

	/* Copies the receiver's answer. */
	receiver = &thermal_receivers[index];
	kern_memset(info, 0, sizeof(*info));
	irq = spin_lock_irqsave(&thermal_lock);

	kern_memcpy(info->name, receiver->name, sizeof(info->name));
	info->error = receiver->error;
	info->flags = receiver->flags;
	info->generation = receiver->generation;
	info->duration_ms = receiver->duration_ms;
	if (receiver->generation != 0U)
		info->time_ns = thermal_now_ns(receiver->ended_tick);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: the receiver's copy. */
	return 0;
}

/*
 * Reports the strategy in force.
 *
 * The name of the firmware's mode comes from the first receiver that
 * gives one.
 */
void
kern_thermal_strategy_get(
	struct kern_thermal_strategy_info *info)
{
	struct thermal_receiver *receiver;
	char name[KERN_THERMAL_FIRMWARE_NAME_MAX];
	unsigned long irq;
	unsigned count;
	unsigned index;
	uint32_t flags;
	bool custom;
	int error;

	/* Prepares the tables, so the lock is ready. */
	thermal_prepare();

	/* Reads the receivers' direct settings without a lock. */
	custom = thermal_custom();

	/* Copies the strategy, its generation and its flags. */
	kern_memset(info, 0, sizeof(*info));
	irq = spin_lock_irqsave(&thermal_lock);

	info->strategy = thermal_strategy;
	info->generation = thermal_generation;
	flags = thermal_strategy_flags();

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Adds the direct setting to the flags. */
	if (custom)
		flags |= KERN_THERMAL_STRATEGY_CUSTOM;
	info->flags = flags;

	/* Asks the receivers, in order, for the name of the firmware's mode. */
	count = atomic_load_acquire(&thermal_receiver_count);
	for (index = 0; index < count; index++) {
		receiver = &thermal_receivers[index];
		if (receiver->ops->firmware_name == NULL)
			continue;

		/* Takes the first name a receiver gives. */
		kern_memset(name, 0, sizeof(name));
		error = receiver->ops->firmware_name(receiver->context, name, sizeof(name));
		if (error != 0 || name[0] == '\0')
			continue;
		thermal_copy_name(info->firmware_name, sizeof(info->firmware_name), name);
		break;
	}
}

/*
 * Chooses a strategy and waits up to five seconds for the receivers.
 *
 * The choice is recorded as a new generation and handed to the strategy
 * thread, which calls every receiver without a lock of the core held.  A
 * later choice made before the receivers finish replaces this one.
 */
int
kern_thermal_strategy_set(
	const struct kern_thermal_strategy *strategy,
	uint32_t *flags)
{
	unsigned long irq;
	uint32_t generation;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	int error;

	/* Refuses a strategy that cannot be chosen. */
	*flags = 0;
	error = thermal_strategy_check(strategy);
	if (error != 0)
		return error;

	/* Prepares the tables, so the lock is ready. */
	thermal_prepare();

	/* Records the choice as a new generation and hands it to the strategy thread. */
	deadline = sched_ticks() + thermal_period(THERMAL_ANSWER_WAIT_MS);
	irq = spin_lock_irqsave(&thermal_lock);

	generation = thermal_generation + 1U;
	if (generation == 0U)
		generation = 1U;
	thermal_generation = generation;
	thermal_strategy = *strategy;
	thermal_post_work();

	/* Waits for this generation's outcome, a later choice, or the deadline. */
	for (;;) {
		if (thermal_summary_generation == generation) {
			/* The receivers answered this choice. */
			error = thermal_summary_error;
			*flags = thermal_summary_flags;
			break;
		}

		if (thermal_generation != generation) {
			/* A later choice replaced this one before the receivers answered. */
			error = 0;
			*flags = KERN_THERMAL_STRATEGY_SUPERSEDED;
			break;
		}

		now = sched_ticks();
		if (now >= deadline) {
			/* The receivers are still at work; the outcome comes as an event. */
			error = 0;
			*flags = KERN_THERMAL_STRATEGY_PENDING;
			break;
		}

		observed = waitq_sequence(&thermal_done_queue);
		(void)waitq_sleep(&thermal_done_queue, &thermal_lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Reports a choice no receiver took, or one every receiver refused. */
	if (error != 0) {
		*flags = 0;
		return error;
	}

	/* Succeeded: the flags tell how far the receivers carried it out. */
	return 0;
}

/*
 * Sets the boost level of one fan or of every fan, and waits up to five
 * seconds for the fans' drivers.
 *
 * Every fan named is checked before any is changed, so a refused request
 * changes nothing.  Under KERN_THERMAL_FAN_ALL a fan that cannot be
 * boosted is left alone.
 */
int
kern_thermal_boost_set(
	uint32_t fan,
	uint32_t level,
	uint32_t *flags)
{
	struct thermal_fan *entry;
	unsigned long irq;
	unsigned count;
	unsigned first;
	unsigned last;
	unsigned index;
	unsigned targets;
	uint32_t ticket;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	int error;

	/* Finds the fans the request names. */
	*flags = 0;
	thermal_prepare();
	count = atomic_load_acquire(&thermal_fan_count);
	if (fan == KERN_THERMAL_FAN_ALL) {
		first = 0;
		last = count;
	} else {
		if (fan >= count)
			return ENOENT;
		first = fan;
		last = fan + 1U;
	}

	/* Checks every fan named, then stamps the request on each and hands it to the strategy thread. */
	deadline = sched_ticks() + thermal_period(THERMAL_ANSWER_WAIT_MS);
	irq = spin_lock_irqsave(&thermal_lock);

	targets = 0;
	for (index = first; index < last; index++) {
		entry = &thermal_fans[index];
		if (entry->ops->boost == NULL)
			continue;
		if (level > entry->state.boost_count) {
			spin_unlock_irqrestore(&thermal_lock, irq);
			return EINVAL;
		}
		targets++;
	}

	if (targets == 0U) {
		spin_unlock_irqrestore(&thermal_lock, irq);
		return EOPNOTSUPP;
	}

	ticket = thermal_boost_ticket + 1U;
	if (ticket == 0U)
		ticket = 1U;
	thermal_boost_ticket = ticket;
	for (index = first; index < last; index++) {
		entry = &thermal_fans[index];
		if (entry->ops->boost == NULL)
			continue;
		entry->boost_wanted = level;
		entry->boost_ticket = ticket;
	}
	thermal_post_work();

	/* Waits for the strategy thread to pass this request, or for the deadline. */
	error = 0;
	for (;;) {
		if ((int32_t)(thermal_boost_done - ticket) >= 0) {
			/* This request, or a later one that replaced it, has been handed to the drivers. */
			if (thermal_boost_done == ticket)
				error = thermal_boost_error;
			break;
		}

		now = sched_ticks();
		if (now >= deadline) {
			/* A driver is still at work. */
			*flags = KERN_THERMAL_BOOST_PENDING;
			break;
		}

		observed = waitq_sequence(&thermal_done_queue);
		(void)waitq_sleep(&thermal_done_queue, &thermal_lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Reports a driver that refused the level. */
	if (error != 0)
		return error;

	/* Succeeded: the fans are at the level, or will be. */
	return 0;
}

/*
 * Hands an ioctl of /dev/thermal to the handler of the range its number
 * falls in.
 */
int
kern_thermal_ioctl_dispatch(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	const struct thermal_range *range;
	kern_thermal_ioctl_t handler;
	unsigned long group;
	unsigned number;
	unsigned count;
	unsigned index;
	int error;

	/* Refuses an ioctl of another group. */
	group = (request >> THERMAL_IOCTL_GROUP_SHIFT) & THERMAL_IOCTL_BYTE_MASK;
	if (group != (unsigned long)THERMAL_IOCTL_GROUP)
		return ENOTTY;

	/* Finds the range that holds the number. */
	number = (unsigned)(request & THERMAL_IOCTL_BYTE_MASK);
	handler = NULL;
	count = atomic_load_acquire(&thermal_range_count);
	for (index = 0; index < count; index++) {
		range = &thermal_ranges[index];
		if (number >= range->first && number <= range->last) {
			handler = range->handler;
			break;
		}
	}

	/* Refuses a number no range holds. */
	if (handler == NULL)
		return ENOTTY;

	/* Hands the request to the range's handler. */
	error = handler(file, request, argument);
	if (error != 0)
		return error;

	/* Succeeded: the handler served the request. */
	return 0;
}

/*
 * Starts taking the events a mask chooses for an open file.
 */
void
kern_thermal_reader_attach(
	struct kern_thermal_reader *reader,
	uint32_t mask)
{
	unsigned long irq;

	/* Prepares the tables, so the event lock is ready. */
	thermal_prepare();

	/* Starts with an empty ring. */
	kern_memset(reader, 0, sizeof(*reader));
	reader->mask = mask;
	waitq_init(&reader->queue, "thermal reader");

	/* Puts the open file on the list the events go to. */
	irq = spin_lock_irqsave(&thermal_event_lock);

	reader->next = thermal_readers;
	thermal_readers = reader;
	reader->attached = true;

	spin_unlock_irqrestore(&thermal_event_lock, irq);
}

/*
 * Changes the events an attached open file takes.
 */
void
kern_thermal_reader_subscribe(
	struct kern_thermal_reader *reader,
	uint32_t mask)
{
	unsigned long irq;

	/* Stores the mask the next event is tested against. */
	irq = spin_lock_irqsave(&thermal_event_lock);

	reader->mask = mask;

	spin_unlock_irqrestore(&thermal_event_lock, irq);
}

/*
 * Stops taking events for an open file before it is freed.
 */
void
kern_thermal_reader_detach(
	struct kern_thermal_reader *reader)
{
	struct kern_thermal_reader **link;
	unsigned long irq;

	/* Takes the open file off the list, so no event reaches it again. */
	irq = spin_lock_irqsave(&thermal_event_lock);

	for (link = &thermal_readers; *link != NULL; link = &(*link)->next) {
		if (*link == reader) {
			*link = reader->next;
			break;
		}
	}
	reader->next = NULL;
	reader->attached = false;

	spin_unlock_irqrestore(&thermal_event_lock, irq);
}

/*
 * Moves the oldest events of an open file into records.
 */
unsigned
kern_thermal_reader_take(
	struct kern_thermal_reader *reader,
	struct kern_thermal_record *records,
	unsigned capacity)
{
	struct kern_thermal_record *record;
	unsigned long irq;
	unsigned moved;

	/* Nothing fits in no room. */
	if (capacity == 0U)
		return 0;

	/* Reports a drop first, then the oldest events in order. */
	moved = 0;
	irq = spin_lock_irqsave(&thermal_event_lock);

	if (reader->dropped != 0U) {
		record = &records[moved];
		record->type = KERN_THERMAL_EVENT_OVERFLOW;
		record->index = 0;
		record->value = (int64_t)reader->dropped;
		record->time_ns = thermal_now_ns(sched_ticks());
		reader->dropped = 0;
		moved++;
	}

	while (moved < capacity && reader->count > 0U) {
		records[moved] = reader->ring[reader->head];
		reader->head = (reader->head + 1U) % KERN_THERMAL_READER_RING;
		reader->count--;
		moved++;
	}

	spin_unlock_irqrestore(&thermal_event_lock, irq);

	/* Succeeded: how many records were filled. */
	return moved;
}

/*
 * Tells whether an open file has an event to take.
 */
bool
kern_thermal_reader_ready(
	struct kern_thermal_reader *reader)
{
	unsigned long irq;
	bool ready;

	/* An event waits, or a drop waits to be reported. */
	ready = false;
	irq = spin_lock_irqsave(&thermal_event_lock);

	if (reader->count > 0U || reader->dropped != 0U)
		ready = true;

	spin_unlock_irqrestore(&thermal_event_lock, irq);

	/* Succeeded: whether a take would move something. */
	return ready;
}

/*
 * Prepares the locks and the queues once, for whichever entry point runs
 * first.
 */
static void
thermal_prepare(void)
{
	unsigned state;
	unsigned expected;
	int won;

	/* Nothing to do once the tables are ready. */
	state = atomic_raw_load_acquire(&thermal_state);
	if (state == THERMAL_STATE_READY)
		return;

	/* Claims the preparation, or waits for the caller that claimed it. */
	expected = THERMAL_STATE_NONE;
	won = atomic_raw_compare_exchange(&thermal_state, &expected, THERMAL_STATE_PREPARING);
	if (!won) {
		for (;;) {
			state = atomic_raw_load_acquire(&thermal_state);
			if (state == THERMAL_STATE_READY)
				break;
			atomic_spin_hint();
		}
		return;
	}

	/* Prepares the locks. */
	spin_init(&thermal_lock, LOCK_RANK_DEVICE, "thermal copy");
	spin_init(&thermal_event_lock, LOCK_RANK_POLL, "thermal events");

	/* Prepares the queues. */
	waitq_init(&thermal_work_queue, "thermal work");
	waitq_init(&thermal_done_queue, "thermal done");
	waitq_init(&thermal_poll_queue, "thermal poll");
	waitq_init(&thermal_guard_queue, "thermal guard");

	/* Publishes the prepared tables to every other caller. */
	atomic_raw_store_release(&thermal_state, THERMAL_STATE_READY);
}

/* Copies a name into a copy's room, cut to fit and always ended with a zero. */
static void
thermal_copy_name(
	char *target,
	size_t size,
	const char *name)
{
	/* Clears the room, then copies all but its last byte. */
	kern_memset(target, 0, size);
	kern_strncpy(target, name, size - 1U);
}

/* Turns a tick of the kernel's clock into nanoseconds since boot. */
static uint64_t
thermal_now_ns(
	uint64_t tick)
{
	uint64_t nanoseconds;

	/* Succeeded: the tick's time. */
	nanoseconds = kern_ticks_to_ns(tick);
	return nanoseconds;
}

/* Turns a period in milliseconds into ticks of the kernel's clock. */
static uint64_t
thermal_period(
	uint32_t milliseconds)
{
	uint64_t ticks;

	/* Succeeded: at least one tick, as the conversion rounds up. */
	ticks = kern_ms_to_ticks(milliseconds);
	return ticks;
}

/* Starts one of the core's threads; without it the core keeps its copies but that thread's work is not done. */
static void
thermal_start_thread(
	void (*entry)(void *),
	int priority,
	const char *name)
{
	struct thread *thread;
	int error;

	/* Creates the thread and lets it run. */
	error = kthread_create(entry, NULL, priority, &thread);
	if (error != 0) {
		kern_logf("thermal: the %s thread did not start (%d)\n", name, error);
		return;
	}
	thread_start(thread);
}

/* Asks the strategy thread for a pass.  The caller holds the copy lock. */
static void
thermal_post_work(void)
{
	/*
	 * A pass reads every request present when it starts; a request made
	 * during a pass raises posted past seen and brings another pass.
	 */
	thermal_work_posted++;
	waitq_wake_all(&thermal_work_queue);
}

/* Sleeps on a queue nothing wakes until the clock reaches a deadline. */
static void
thermal_sleep_until(
	struct wait_queue *queue,
	uint64_t deadline)
{
	unsigned long irq;
	uint64_t observed;
	uint64_t now;

	/* Sleeps until the deadline, again after any early return. */
	irq = spin_lock_irqsave(&thermal_lock);

	for (;;) {
		now = sched_ticks();
		if (now >= deadline)
			break;
		observed = waitq_sequence(queue);
		(void)waitq_sleep(queue, &thermal_lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Gives a periodic thread's next deadline: one period after the last, so
 * the period does not drift, or one period from now when the thread fell
 * a whole period behind.
 */
static uint64_t
thermal_next_deadline(
	uint64_t deadline,
	uint64_t period)
{
	uint64_t next;
	uint64_t now;

	/* One period after the last deadline. */
	next = deadline + period;

	/* Starts again from now when the thread fell behind. */
	now = sched_ticks();
	if (next <= now)
		next = now + period;

	/* Succeeded: the next deadline. */
	return next;
}

/*
 * Checks that a strategy can be chosen: a value the core knows other than
 * FIRMWARE, and axes that fit it.  Returns 0 or EINVAL.
 */
static int
thermal_strategy_check(
	const struct kern_thermal_strategy *strategy)
{
	/* FIRMWARE is where boot starts and is never chosen. */
	if (strategy->value == KERN_THERMAL_STRATEGY_FIRMWARE)
		return EINVAL;

	/* Refuses a value past the last strategy. */
	if (strategy->value > KERN_THERMAL_STRATEGY_MANUAL)
		return EINVAL;

	/* The reserved byte stays 0, for a meaning it may have later. */
	if (strategy->reserved != 0U)
		return EINVAL;

	/* Only MANUAL has axes, each up to the largest value. */
	if (strategy->value == KERN_THERMAL_STRATEGY_MANUAL) {
		if (strategy->speed > KERN_THERMAL_AXIS_MAX)
			return EINVAL;
		if (strategy->noise > KERN_THERMAL_AXIS_MAX)
			return EINVAL;
		if (strategy->gating > KERN_THERMAL_AXIS_MAX)
			return EINVAL;
	} else {
		if (strategy->speed != 0U)
			return EINVAL;
		if (strategy->noise != 0U)
			return EINVAL;
		if (strategy->gating != 0U)
			return EINVAL;
	}

	/* Succeeded: the strategy can be chosen. */
	return 0;
}

/*
 * Gives the strategy's flags but CUSTOM: PENDING while the receivers have
 * not answered the generation in force, and what they answered it.  The
 * caller holds the copy lock.
 */
static uint32_t
thermal_strategy_flags(void)
{
	uint32_t flags;

	/* Nothing to tell before the first choice. */
	flags = 0;
	if (thermal_generation == 0U)
		return flags;

	if (thermal_summary_generation == thermal_generation) {
		/* The receivers answered the choice in force. */
		flags = thermal_summary_flags;
	} else {
		/* They have not all answered it yet. */
		flags = KERN_THERMAL_STRATEGY_PENDING;
	}

	/* Succeeded: the flags of the choice in force. */
	return flags;
}

/* Tells whether a receiver has a root's direct setting in force, reading each without a lock. */
static bool
thermal_custom(void)
{
	struct thermal_receiver *receiver;
	unsigned count;
	unsigned index;
	bool custom;

	/* Asks every receiver that can tell. */
	count = atomic_load_acquire(&thermal_receiver_count);
	for (index = 0; index < count; index++) {
		receiver = &thermal_receivers[index];
		if (receiver->ops->is_custom == NULL)
			continue;
		custom = receiver->ops->is_custom(receiver->context);
		if (custom)
			return true;
	}

	/* No direct setting anywhere. */
	return false;
}

/* Calls the receivers and the fans' boost whenever asked, for as long as the kernel runs. */
static void
thermal_work_main(
	void *argument)
{
	/* No argument: the work is in the core's tables. */
	(void)argument;

	/* Makes a pass for every request, and another for a request made during a pass. */
	for (;;) {
		thermal_work_wait();
		thermal_strategy_pass();
		thermal_boost_pass();
	}
}

/* Sleeps until a request for a pass comes, and takes every request present. */
static void
thermal_work_wait(void)
{
	unsigned long irq;
	uint64_t observed;

	/* Sleeps while every request has been passed. */
	irq = spin_lock_irqsave(&thermal_lock);

	while (thermal_work_posted == thermal_work_seen) {
		observed = waitq_sequence(&thermal_work_queue);
		(void)waitq_sleep(&thermal_work_queue, &thermal_lock, observed, 0, 0U);
	}
	thermal_work_seen = thermal_work_posted;

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Hands the generation in force to every receiver that has not carried it
 * out, in the order they registered, then records the outcome.
 *
 * The pass works on the generation it found when it started; a choice
 * made during the pass brings another pass, which hands the newer one to
 * every receiver again.
 */
static void
thermal_strategy_pass(void)
{
	struct kern_thermal_strategy strategy;
	struct thermal_receiver *receiver;
	unsigned long irq;
	uint32_t generation;
	uint32_t flags;
	uint64_t started;
	uint64_t ended;
	unsigned count;
	unsigned index;
	int error;

	/* Takes the generation in force. */
	irq = spin_lock_irqsave(&thermal_lock);

	strategy = thermal_strategy;
	generation = thermal_generation;

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Nothing to hand out before the first choice. */
	if (strategy.value == KERN_THERMAL_STRATEGY_FIRMWARE)
		return;

	/* Hands it to every receiver that has not carried it out, with no lock held. */
	count = atomic_load_acquire(&thermal_receiver_count);
	for (index = 0; index < count; index++) {
		receiver = &thermal_receivers[index];

		/* Only this thread writes the receiver's generation, so it reads it without the lock. */
		if (receiver->generation == generation)
			continue;

		/* Calls the receiver and times it. */
		flags = 0;
		started = sched_ticks();
		error = receiver->ops->apply(receiver->context, &strategy, &flags);
		ended = sched_ticks();

		/* Records its answer. */
		irq = spin_lock_irqsave(&thermal_lock);

		receiver->generation = generation;
		receiver->error = error;
		receiver->flags = flags;
		receiver->duration_ms = (uint32_t)kern_ticks_to_ms(ended - started);
		receiver->ended_tick = ended;

		spin_unlock_irqrestore(&thermal_lock, irq);
	}

	/* Records the outcome of the generation and tells the chooser. */
	thermal_strategy_summarize(generation, strategy.value);
}

/*
 * Records the outcome of a generation from the answers of the receivers
 * that carried it out, wakes its chooser and tells the readers when the
 * outcome changed.  A generation a later choice replaced has no outcome;
 * its chooser is only woken, to learn it was replaced.
 *
 * A receiver for which the strategy does not apply (EOPNOTSUPP) counts
 * neither way, nor does one that asked but could not confirm.  With no
 * receiver acting the outcome is EOPNOTSUPP; when every acting receiver
 * failed it is the first errno; when some failed it is PARTIAL.
 */
static void
thermal_strategy_summarize(
	uint32_t generation,
	uint32_t value)
{
	struct thermal_receiver *receiver;
	unsigned long irq;
	unsigned count;
	unsigned index;
	unsigned acting;
	unsigned failed;
	uint32_t flags;
	bool changed;
	int first;
	int error;

	/* Counts the acting receivers of the generation and their failures. */
	count = atomic_load_acquire(&thermal_receiver_count);
	irq = spin_lock_irqsave(&thermal_lock);

	/* Leaves a replaced generation without an outcome and wakes its chooser. */
	if (thermal_generation != generation) {
		waitq_wake_all(&thermal_done_queue);
		spin_unlock_irqrestore(&thermal_lock, irq);
		return;
	}

	acting = 0;
	failed = 0;
	first = 0;
	for (index = 0; index < count; index++) {
		receiver = &thermal_receivers[index];
		if (receiver->generation != generation)
			continue;
		if (receiver->error == EOPNOTSUPP)
			continue;
		acting++;
		if (receiver->error != 0) {
			failed++;
			if (first == 0)
				first = receiver->error;
		}
	}

	/* Turns the counts into the chooser's answer. */
	error = 0;
	flags = 0;
	if (acting == 0U) {
		/* No receiver acts on the strategy here; it is kept all the same. */
		error = EOPNOTSUPP;
	} else if (failed == acting) {
		/* Every receiver that acts on it failed. */
		error = first;
	} else if (failed != 0U) {
		/* Some failed and the others carried it out. */
		flags = KERN_THERMAL_STRATEGY_PARTIAL;
	}

	/* Publishes the outcome and wakes the chooser waiting for it. */
	changed = false;
	if (thermal_summary_generation != generation)
		changed = true;
	else if (thermal_summary_error != error || thermal_summary_flags != flags)
		changed = true;
	thermal_summary_generation = generation;
	thermal_summary_error = error;
	thermal_summary_flags = flags;
	waitq_wake_all(&thermal_done_queue);

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Tells the readers the strategy and its generation when the outcome is new. */
	if (changed)
		kern_thermal_event(KERN_THERMAL_EVENT_STRATEGY, value, (int64_t)generation);
}

/*
 * Hands every waiting boost request to its fan's driver, then records the
 * outcome of the latest request: the first errno of the fans it stamped.
 *
 * A request made during the pass may be handed out in it already; the
 * pass it brings then finds nothing waiting and records that request's
 * outcome from the answers kept on its fans.
 */
static void
thermal_boost_pass(void)
{
	struct thermal_fan *fan;
	unsigned long irq;
	uint32_t latest;
	uint32_t ticket;
	uint32_t level;
	unsigned count;
	unsigned index;
	bool waiting;
	int first;
	int error;

	/* Takes the latest request's stamp; the pass records that request's outcome. */
	irq = spin_lock_irqsave(&thermal_lock);

	latest = thermal_boost_ticket;

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Hands each fan's waiting level to its driver, with no lock held. */
	count = atomic_load_acquire(&thermal_fan_count);
	for (index = 0; index < count; index++) {
		fan = &thermal_fans[index];

		/* Takes the fan's waiting request, if it has one. */
		irq = spin_lock_irqsave(&thermal_lock);

		waiting = false;
		if (fan->boost_ticket != fan->boost_served)
			waiting = true;
		level = fan->boost_wanted;
		ticket = fan->boost_ticket;

		spin_unlock_irqrestore(&thermal_lock, irq);

		/* Leaves a fan with nothing waiting. */
		if (!waiting)
			continue;

		/* Asks the driver for the level. */
		error = fan->ops->boost(fan->context, level);

		/* Records what the driver did with it. */
		irq = spin_lock_irqsave(&thermal_lock);

		fan->boost_served = ticket;
		fan->boost_error = error;
		if (error == 0)
			fan->boost_applied = level;

		spin_unlock_irqrestore(&thermal_lock, irq);

		/* Tells the readers the fan's new boost level. */
		if (error == 0)
			kern_thermal_event(KERN_THERMAL_EVENT_BOOST, index, (int64_t)level);
	}

	/* Publishes the latest request's outcome and wakes its requester. */
	irq = spin_lock_irqsave(&thermal_lock);

	if (thermal_boost_done != latest) {
		first = 0;
		for (index = 0; index < count; index++) {
			fan = &thermal_fans[index];
			if (fan->boost_served != latest)
				continue;
			if (fan->boost_error != 0 && first == 0)
				first = fan->boost_error;
		}
		thermal_boost_done = latest;
		thermal_boost_error = first;
		waitq_wake_all(&thermal_done_queue);
	}

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Reads the receivers' copies and the meters every second and the fans
 * every fifth second, for as long as the kernel runs.
 */
static void
thermal_poll_main(
	void *argument)
{
	uint64_t deadline;
	uint64_t period;
	unsigned pass;

	/* No argument: the work is in the core's tables. */
	(void)argument;

	/* Reads the fans at the first pass, so their copy fills early. */
	period = thermal_period(THERMAL_POLL_PERIOD_MS);
	deadline = sched_ticks();
	pass = 0;

	/* Polls once a period. */
	for (;;) {
		thermal_poll_receivers();
		thermal_poll_meters();
		if (pass % THERMAL_FAN_POLLS == 0U)
			thermal_poll_fans();
		pass++;

		/* Sleeps until the next period. */
		deadline = thermal_next_deadline(deadline, period);
		thermal_sleep_until(&thermal_poll_queue, deadline);
	}
}

/* Lets every receiver that keeps a copy of the hardware refresh it. */
static void
thermal_poll_receivers(void)
{
	struct thermal_receiver *receiver;
	unsigned count;
	unsigned index;

	/* Calls each receiver's poll, with no lock held. */
	count = atomic_load_acquire(&thermal_receiver_count);
	for (index = 0; index < count; index++) {
		receiver = &thermal_receivers[index];
		if (receiver->ops->poll == NULL)
			continue;
		receiver->ops->poll(receiver->context);
	}
}

/* Reads every meter and adds what it counted to its sum. */
static void
thermal_poll_meters(void)
{
	unsigned count;
	unsigned index;

	/* Reads each meter. */
	count = atomic_load_acquire(&thermal_meter_count);
	for (index = 0; index < count; index++)
		thermal_meter_read(&thermal_meters[index]);
}

/* Reads every fan into its copy. */
static void
thermal_poll_fans(void)
{
	unsigned count;
	unsigned index;

	/* Reads each fan. */
	count = atomic_load_acquire(&thermal_fan_count);
	for (index = 0; index < count; index++)
		thermal_fan_read(index);
}

/*
 * Reads one meter's counter and adds the energy since the last read to
 * its sum, counting the counter's wrap.  The first read only sets where
 * the sum starts.
 */
static void
thermal_meter_read(
	struct thermal_meter *meter)
{
	const struct kern_thermal_meter_ops *ops;
	uint64_t count;
	uint64_t mask;
	uint64_t delta;
	uint64_t scaled;
	uint64_t now;
	int error;

	/* Reads the counter, with no lock held. */
	ops = meter->ops;
	count = 0;
	error = ops->read(meter->context, &count);
	now = sched_ticks();

	/* The counter's width, as a mask of its bits. */
	mask = ((uint64_t)1 << ops->count_bits) - 1U;

	/* Updates the copy inside an odd sequence. */
	thermal_meter_write_begin(meter);

	if (error != 0) {
		/* Keeps the sum and says the read failed. */
		meter->copy |= KERN_THERMAL_METER_COPY_STALE;
	} else if ((meter->copy & KERN_THERMAL_METER_COPY_KNOWN) == 0U) {
		/* The first read sets where the sum starts. */
		meter->last_count = count & mask;
		meter->copy = KERN_THERMAL_METER_COPY_KNOWN;
		meter->read_tick = now;
	} else {
		/*
		 * The count since the last read, modulo the counter's width, so a
		 * wrap counts as the distance across it.  The header bounds the
		 * count and the unit so that a second's count times the numerator,
		 * with the remainder below the denominator, fits 64 bits.
		 */
		delta = (count - meter->last_count) & mask;
		scaled = delta * ops->unit_numerator + meter->remainder;
		meter->energy_nj += scaled / ops->unit_denominator;
		meter->remainder = scaled % ops->unit_denominator;
		meter->last_count = count & mask;
		meter->copy &= ~KERN_THERMAL_METER_COPY_STALE;
		meter->read_tick = now;
	}

	thermal_meter_write_end(meter);
}

/*
 * Makes a meter's sequence odd before the poll thread writes its copy.
 * The exchange orders the writes that follow after it.
 */
static void
thermal_meter_write_begin(
	struct thermal_meter *meter)
{
	unsigned expected;

	/* Only the poll thread writes, so the exchange finds what it read. */
	expected = atomic_raw_load_relaxed(&meter->sequence);
	(void)atomic_raw_compare_exchange(&meter->sequence, &expected, expected + 1U);
}

/* Makes a meter's sequence even again once its copy is written, publishing the copy. */
static void
thermal_meter_write_end(
	struct thermal_meter *meter)
{
	unsigned sequence;

	/* Releases the copy's writes before the sequence that tells they are whole. */
	sequence = atomic_raw_load_relaxed(&meter->sequence);
	atomic_raw_store_release(&meter->sequence, sequence + 1U);
}

/*
 * Reads one fan into its copy and tells the readers when it stalls: it
 * reports both its speed and its level, it runs at a level above 0 (the
 * automatic one or the boost, whichever is higher), and its speed has
 * been 0 for three reads in a row.
 */
static void
thermal_fan_read(
	uint32_t index)
{
	struct kern_thermal_fan_state state;
	struct thermal_fan *fan;
	unsigned long irq;
	uint32_t level;
	uint32_t both;
	bool stalled;
	uint64_t now;
	int error;

	/* Reads the fan, with no lock held. */
	fan = &thermal_fans[index];
	kern_memset(&state, 0, sizeof(state));
	error = fan->ops->read(fan->context, &state);
	now = sched_ticks();

	/* Updates the copy. */
	stalled = false;
	irq = spin_lock_irqsave(&thermal_lock);

	if (error != 0) {
		/* Keeps the last copy and says the read failed. */
		fan->copy |= KERN_THERMAL_FAN_COPY_STALE;
	} else {
		/* Takes the new reading. */
		fan->state = state;
		fan->copy |= KERN_THERMAL_FAN_COPY_KNOWN;
		fan->copy &= ~KERN_THERMAL_FAN_COPY_STALE;
		fan->read_tick = now;

		/* The level the fan runs at: the automatic one or the boost, whichever is higher. */
		level = state.level;
		if (fan->boost_applied > level)
			level = fan->boost_applied;

		/* Counts the reads in a row that find a fan still while it should turn. */
		both = KERN_THERMAL_FAN_HAS_RPM | KERN_THERMAL_FAN_HAS_LEVEL;
		if ((state.flags & both) == both &&
		    level > 0U &&
		    state.rpm == 0U) {
			fan->zero_reads++;
		} else {
			fan->zero_reads = 0;
			fan->copy &= ~KERN_THERMAL_FAN_COPY_STALLED;
		}

		/* Marks the stall once, when it begins. */
		if (fan->zero_reads >= THERMAL_STALL_READS && (fan->copy & KERN_THERMAL_FAN_COPY_STALLED) == 0U) {
			fan->copy |= KERN_THERMAL_FAN_COPY_STALLED;
			stalled = true;
		}
	}

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Tells the readers the fan stalled. */
	if (stalled)
		kern_thermal_event(KERN_THERMAL_EVENT_FAN_STALLED, index, 0);
}

/* Looks at the sensors every second, for as long as the kernel runs. */
static void
thermal_guard_main(
	void *argument)
{
	uint64_t deadline;
	uint64_t period;

	/* No argument: the work is in the core's tables. */
	(void)argument;

	/* Starts a period after boot. */
	period = thermal_period(THERMAL_GUARD_PERIOD_MS);
	deadline = sched_ticks() + period;

	/* Looks once a period. */
	for (;;) {
		thermal_sleep_until(&thermal_guard_queue, deadline);
		thermal_guard_pass();
		deadline = thermal_next_deadline(deadline, period);
	}
}

/* Looks at every sensor's copy under the copy lock, calling no driver. */
static void
thermal_guard_pass(void)
{
	unsigned long irq;
	unsigned count;
	unsigned index;
	uint64_t stale_after;
	uint64_t now;

	/* Looks at each sensor, with the age past which it is stale. */
	stale_after = thermal_period(THERMAL_STALE_MS);
	now = sched_ticks();
	count = atomic_load_acquire(&thermal_sensor_count);
	irq = spin_lock_irqsave(&thermal_lock);

	for (index = 0; index < count; index++)
		thermal_guard_sensor(&thermal_sensors[index], index, now, stale_after);

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Updates one sensor's age and the levels it is past, and tells the
 * readers when it crosses one: the critical level of a firmware zone, and
 * the passive level of the processor's sensor.  A level counts as left
 * once the temperature falls three degrees below it.  The event's value is
 * 1 when the level is reached and 0 when it is left.  The caller holds the
 * copy lock; the event lock is a leaf under it.
 */
static void
thermal_guard_sensor(
	struct kern_thermal_sensor *sensor,
	uint32_t index,
	uint64_t now,
	uint64_t stale_after)
{
	uint64_t since;
	bool usable;

	/* A sensor is stale when no reading came for a while since the watch began. */
	since = sensor->taken_tick;
	if (sensor->watch_tick > since)
		since = sensor->watch_tick;
	sensor->stale = false;
	if (now - since >= stale_after)
		sensor->stale = true;

	/* Only a fresh, valid reading crosses a level. */
	usable = false;
	if (sensor->valid && !sensor->stale)
		usable = true;
	if (!usable)
		return;

	/* The critical level of a firmware zone. */
	if (sensor->kind == KERN_THERMAL_KIND_ZONE && sensor->critical != 0) {
		if (!sensor->critical_crossed && sensor->millicelsius >= sensor->critical) {
			sensor->critical_crossed = true;
			kern_thermal_event(KERN_THERMAL_EVENT_CRITICAL, index, 1);
		} else if (sensor->critical_crossed && sensor->millicelsius < sensor->critical - THERMAL_HYSTERESIS) {
			sensor->critical_crossed = false;
			kern_thermal_event(KERN_THERMAL_EVENT_CRITICAL, index, 0);
		}
	}

	/* The passive level of the processor's sensor. */
	if ((sensor->flags & KERN_THERMAL_SENSOR_CPU) != 0U && sensor->passive != 0) {
		if (!sensor->passive_crossed && sensor->millicelsius >= sensor->passive) {
			sensor->passive_crossed = true;
			kern_thermal_event(KERN_THERMAL_EVENT_PASSIVE, index, 1);
		} else if (sensor->passive_crossed && sensor->millicelsius < sensor->passive - THERMAL_HYSTERESIS) {
			sensor->passive_crossed = false;
			kern_thermal_event(KERN_THERMAL_EVENT_PASSIVE, index, 0);
		}
	}
}

/* Turns a passive or critical level that means none into 0. */
static int32_t
thermal_level(
	int32_t millicelsius)
{
	/* A level at or below 0, or at or above 127 degrees, is none. */
	if (millicelsius <= 0)
		return 0;
	if (millicelsius >= THERMAL_LEVEL_NONE)
		return 0;

	/* Succeeded: a real level. */
	return millicelsius;
}

/*
 * Appends a record to an open file's ring, dropping the oldest when the
 * ring is full.  The caller holds the event lock.
 */
static void
thermal_ring_append(
	struct kern_thermal_reader *reader,
	const struct kern_thermal_record *record)
{
	unsigned slot;

	/* Drops the oldest record of a full ring and counts it. */
	if (reader->count == KERN_THERMAL_READER_RING) {
		reader->head = (reader->head + 1U) % KERN_THERMAL_READER_RING;
		reader->count--;
		reader->dropped++;
	}

	/* Writes the record after the newest. */
	slot = (reader->head + reader->count) % KERN_THERMAL_READER_RING;
	reader->ring[slot] = *record;
	reader->count++;
}

/*
 * Copies one half of the sensors' copy for hw.thermal (kern_thermal_read_t):
 * the context is the first sensor of the half.
 */
static int
thermal_sysctl_copy(
	void *context,
	struct thermal_entry *entries,
	unsigned capacity,
	unsigned *count)
{
	const struct kern_thermal_sensor *sensor;
	struct thermal_entry *entry;
	const unsigned *first;
	unsigned long irq;
	unsigned sensors;
	unsigned filled;
	unsigned index;

	/* The half the context names, as far as the table and the room go. */
	first = context;
	sensors = atomic_load_acquire(&thermal_sensor_count);
	filled = 0;

	/* Turns each sensor of the half into an entry. */
	irq = spin_lock_irqsave(&thermal_lock);

	for (index = *first;
	     index < sensors &&
	     filled < capacity &&
	     filled < THERMAL_SYSCTL_SOURCE_SENSORS;
	     index++) {
		sensor = &thermal_sensors[index];
		entry = &entries[filled];
		kern_memset(entry, 0, sizeof(*entry));
		kern_memcpy(entry->name, sensor->name, sizeof(entry->name));
		entry->name[sizeof(entry->name) - 1U] = '\0';

		/* The kind and the processor's flag, in hw.thermal's values. */
		entry->kind = THERMAL_KIND_DEVICE;
		if (sensor->kind == KERN_THERMAL_KIND_ZONE)
			entry->kind = THERMAL_KIND_ZONE;
		if ((sensor->flags & KERN_THERMAL_SENSOR_CPU) != 0U)
			entry->flags |= THERMAL_FLAG_CPU;

		/* The temperature, with when it was taken, while the latest reading is valid. */
		if (sensor->valid) {
			entry->valid |= THERMAL_HAVE_TEMPERATURE;
			entry->milli_celsius = sensor->millicelsius;
			entry->time_ns = thermal_now_ns(sensor->taken_tick);
		}

		/* The levels the sensor has. */
		if (sensor->passive != 0) {
			entry->valid |= THERMAL_HAVE_PASSIVE;
			entry->passive_milli_celsius = sensor->passive;
		}
		if (sensor->critical != 0) {
			entry->valid |= THERMAL_HAVE_CRITICAL;
			entry->critical_milli_celsius = sensor->critical;
		}

		filled++;
	}

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: how many entries were filled. */
	*count = filled;
	return 0;
}
