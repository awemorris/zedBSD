/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI temperature sensors (ws134-p009): the thermal zones, and the
 * devices with a _TMP (an Intel laptop's DPTF participants: the Latitude
 * 5330 has no thermal zone, and tells its temperatures through
 * \_SB_.PC00.TCPU and the EC's TMEM, TSKN, NGFF and AMBF), listed in
 * sysctl hw.thermal.
 *
 * _TMP may read the EC and is not to be evaluated at every sysctl, so this
 * file's thread reads every sensor's _TMP, _PSV and _CRT every
 * THERMAL_PERIOD_MS and keeps them; the sysctl copies what was kept.  A
 * sensor named TCPU or B0D4 (the DPTF processor participant) is the
 * processor's.
 */

#include <drivers/acpi/acpi.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/sysctl.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

#include <stdbool.h>

/* The most sensors taken, and how often they are read. */
#define THERMAL_SENSORS_MAX	16U
#define THERMAL_PERIOD_MS	5000U

/* _STA: present. */
#define THERMAL_STATUS_PRESENT	0x01U

/*
 * A temperature in tenths of a kelvin that makes sense: from -73.15 to
 * 200.0 degrees Celsius (an EC that has not read its sensor answers 0, and
 * a missing one all ones).
 */
#define THERMAL_DECIKELVIN_LOW	2000U
#define THERMAL_DECIKELVIN_HIGH	4732U

/* 0 degrees Celsius in thousandths of a kelvin. */
#define THERMAL_ZERO_CELSIUS	273150

/* A millisecond in nanoseconds. */
#define THERMAL_NS_PER_MS	1000000ULL

/*
 * One sensor: its node, and what hw.thermal says of it (its path, kind and
 * flags fixed at attach, its values as the thread last read them).
 */
struct thermal_sensor {
	struct drv_acpi_node *node;
	struct thermal_entry entry;
};

/*
 * The sensors and their number, filled once by drv_acpi_thermal_attach()
 * before the thread starts and the sysctl may read them; thermal_lock
 * (with interrupts off) protects their values afterwards.  thermal_waitq is
 * only slept on, for the period.
 */
static struct thermal_sensor thermal_sensors[THERMAL_SENSORS_MAX];
static unsigned thermal_sensor_count;
static struct spinlock thermal_lock;
static struct wait_queue thermal_waitq;

static int thermal_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool thermal_present(struct drv_acpi_node *node);
static bool thermal_processor(const char *path);
static void thermal_thread(void *argument);
static void thermal_refresh(struct thermal_sensor *sensor);
static bool thermal_read(struct drv_acpi_node *node, const char *method, int32_t *milli_celsius);
static int thermal_copy(void *context, struct thermal_entry *entries, unsigned capacity, unsigned *count);

/*
 * Finds the thermal zones and the devices with a _TMP, reads them, lists
 * them in hw.thermal and starts the thread that reads them again.  Called
 * once the namespace is ready and the EC is attached.  Returns 0, or
 * ENODEV when the machine has no sensor.
 */
int
drv_acpi_thermal_attach(void)
{
	struct thread *thread;
	unsigned index;
	int error;

	/* The lock and the queue. */
	spin_init(&thermal_lock, LOCK_RANK_DEVICE, "acpi thermal");
	waitq_init(&thermal_waitq, "acpi thermal");

	/* The sensors. */
	(void)drv_acpi_walk(NULL, thermal_visitor, NULL);
	if (thermal_sensor_count == 0U)
		return ENODEV;

	/* Their values now. */
	for (index = 0; index < thermal_sensor_count; index++)
		thermal_refresh(&thermal_sensors[index]);

	/* Listed for the sysctl. */
	error = kern_thermal_register(thermal_copy, NULL);
	if (error != 0)
		kern_logf("acpi: the temperature sensors are not listed (%d)\n", error);

	/* The thread that reads them again. */
	error = kthread_create(thermal_thread, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;
	thread_start(thread);

	/* Succeeded: the log lists what was found. */
	for (index = 0; index < thermal_sensor_count; index++) {
		kern_logf("acpi: temperature sensor %s flags 0x%x valid 0x%x %d mC\n", thermal_sensors[index].entry.name,
		    thermal_sensors[index].entry.flags, thermal_sensors[index].entry.valid,
		    thermal_sensors[index].entry.milli_celsius);
	}

	/* Listed and read. */
	return 0;
}

/* Takes a thermal zone, or a present device with a _TMP. */
static int
thermal_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct thermal_sensor *sensor;
	struct drv_acpi_node *child;
	enum drv_acpi_type type;
	uint32_t kind;
	bool present;
	bool processor;
	int error;

	/* The depth and the argument do not matter. */
	(void)depth;
	(void)argument;

	/* A thermal zone or a device. */
	type = drv_acpi_node_type(node);
	if (type == DRV_ACPI_TYPE_THERMAL_ZONE)
		kind = THERMAL_KIND_ZONE;
	else if (type == DRV_ACPI_TYPE_DEVICE)
		kind = THERMAL_KIND_DEVICE;
	else
		return 0;

	/* With a _TMP, and present. */
	child = NULL;
	error = drv_acpi_lookup(node, "_TMP", &child);
	if (error != 0 || child == NULL)
		return 0;
	present = thermal_present(node);
	if (!present)
		return 0;

	/* A full table takes no more. */
	if (thermal_sensor_count >= THERMAL_SENSORS_MAX)
		return -1;

	/* The sensor, its path (cut to the entry's room), kind and flags; its values unknown until it is read. */
	sensor = &thermal_sensors[thermal_sensor_count];
	kern_memset(sensor, 0, sizeof(*sensor));
	sensor->node = node;
	(void)drv_acpi_node_path(node, sensor->entry.name, sizeof(sensor->entry.name));
	sensor->entry.name[sizeof(sensor->entry.name) - 1U] = '\0';
	sensor->entry.kind = kind;
	processor = thermal_processor(sensor->entry.name);
	if (processor)
		sensor->entry.flags |= THERMAL_FLAG_CPU;

	/* The table holds it. */
	thermal_sensor_count++;

	/* Goes on with the next node. */
	return 0;
}

/* Tells whether a node is present: its _STA's present bit, or no _STA at all. */
static bool
thermal_present(
	struct drv_acpi_node *node)
{
	uint64_t status;
	int error;

	/* Without a _STA a node is present. */
	error = drv_acpi_evaluate_integer(node, "_STA", &status);
	if (error != 0)
		return true;

	/* Succeeded: its present bit. */
	if ((status & THERMAL_STATUS_PRESENT) == 0U)
		return false;
	return true;
}

/* Tells whether a sensor's path names the DPTF processor participant (its last segment TCPU or B0D4). */
static bool
thermal_processor(
	const char *path)
{
	size_t length;
	int same;

	/* The last segment of four. */
	length = kern_strlen(path);
	if (length < 4U)
		return false;
	same = kern_strncmp(path + length - 4U, "TCPU", 4U);
	if (same == 0)
		return true;
	same = kern_strncmp(path + length - 4U, "B0D4", 4U);
	if (same == 0)
		return true;

	/* Another sensor. */
	return false;
}

/* Reads every sensor every THERMAL_PERIOD_MS, for as long as the kernel runs. */
static void
thermal_thread(
	void *argument)
{
	unsigned long irq;
	uint64_t observed;
	uint64_t deadline;
	uint64_t now;
	unsigned index;
	int error;

	/* No argument. */
	(void)argument;

	/* For as long as the kernel runs. */
	for (;;) {
		/* Sleeps for the period (nothing wakes the queue). */
		deadline = sched_ticks() + KERN_MS_TO_TICKS(THERMAL_PERIOD_MS);
		irq = spin_lock_irqsave(&thermal_lock);

		for (;;) {
			now = sched_ticks();
			if (now >= deadline)
				break;
			observed = waitq_sequence(&thermal_waitq);
			error = waitq_sleep(&thermal_waitq, &thermal_lock, observed, deadline, 0U);
			(void)error;
		}

		spin_unlock_irqrestore(&thermal_lock, irq);

		/* Each sensor read again. */
		for (index = 0; index < thermal_sensor_count; index++)
			thermal_refresh(&thermal_sensors[index]);
	}
}

/* Reads a sensor's _TMP, _PSV and _CRT, and keeps what it answered with the time. */
static void
thermal_refresh(
	struct thermal_sensor *sensor)
{
	unsigned long irq;
	uint64_t time_ns;
	int32_t temperature;
	int32_t passive;
	int32_t critical;
	uint32_t valid;
	bool read;

	/* The temperature and when it was read (0 for none), outside the lock: the AML may read the EC. */
	valid = 0;
	time_ns = 0;
	read = thermal_read(sensor->node, "_TMP", &temperature);
	if (read) {
		valid |= THERMAL_HAVE_TEMPERATURE;
		time_ns = kern_ticks_to_ms(sched_ticks()) * THERMAL_NS_PER_MS;
	}

	/* The trip points (a value not read is 0). */
	read = thermal_read(sensor->node, "_PSV", &passive);
	if (read)
		valid |= THERMAL_HAVE_PASSIVE;
	read = thermal_read(sensor->node, "_CRT", &critical);
	if (read)
		valid |= THERMAL_HAVE_CRITICAL;

	/* Kept. */
	irq = spin_lock_irqsave(&thermal_lock);

	sensor->entry.valid = valid;
	sensor->entry.milli_celsius = temperature;
	sensor->entry.passive_milli_celsius = passive;
	sensor->entry.critical_milli_celsius = critical;
	sensor->entry.time_ns = time_ns;

	spin_unlock_irqrestore(&thermal_lock, irq);
}

/*
 * Evaluates a temperature method of a sensor (tenths of a kelvin) into
 * thousandths of a degree Celsius; false when it is missing, fails, or
 * answers a value that makes no sense.
 */
static bool
thermal_read(
	struct drv_acpi_node *node,
	const char *method,
	int32_t *milli_celsius)
{
	uint64_t decikelvin;
	int error;

	/* The value (0 until it is read). */
	*milli_celsius = 0;
	error = drv_acpi_evaluate_integer(node, method, &decikelvin);
	if (error != 0)
		return false;
	if (decikelvin < THERMAL_DECIKELVIN_LOW || decikelvin > THERMAL_DECIKELVIN_HIGH)
		return false;

	/* Succeeded: in thousandths of a degree Celsius. */
	*milli_celsius = (int32_t)decikelvin * 100 - THERMAL_ZERO_CELSIUS;
	return true;
}

/* Copies the sensors as they were last read, for hw.thermal (kern_thermal_read_t). */
static int
thermal_copy(
	void *context,
	struct thermal_entry *entries,
	unsigned capacity,
	unsigned *count)
{
	unsigned long irq;
	unsigned index;

	/* No context. */
	(void)context;

	/* Each sensor, as far as there is room. */
	irq = spin_lock_irqsave(&thermal_lock);

	for (index = 0; index < thermal_sensor_count && index < capacity; index++)
		kern_memcpy(&entries[index], &thermal_sensors[index].entry, sizeof(entries[index]));

	spin_unlock_irqrestore(&thermal_lock, irq);

	/* Succeeded: how many were copied. */
	*count = index;
	return 0;
}
