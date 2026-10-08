/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the ACPI temperature sensors (ws134-p009).
 *
 * The unchanged driver (src/drivers/acpi/acpi-thermal.c, compiled
 * freestanding) runs against a stand-in namespace: a thermal zone with
 * _TMP and _CRT, the Latitude 5330's kind of DPTF device TCPU (_TMP, _PSV,
 * _CRT, no _STA), an EC sensor TSKN whose _TMP answers 0 (not read yet),
 * an absent device with a _TMP, a device without one, and a method.  The
 * test checks:
 *   - attach takes the zone, TCPU and TSKN (not the absent device, the one
 *     without a _TMP, nor the method), lists itself in hw.thermal and
 *     starts its thread;
 *   - the entries: the paths, the kinds, TCPU marked the processor's, the
 *     values in thousandths of a degree Celsius, the trip points, TSKN
 *     without a temperature;
 *   - the AML is evaluated with the driver's lock free;
 *   - the thread reads every sensor again after THERMAL_PERIOD_MS (TSKN's
 *     temperature comes, TCPU's changes), not before.
 * The thread's endless loop is driven from its sleeps and left with a
 * longjmp.
 *
 *   plan/ws134/tests/run-host-acpi-thermal.sh
 */

#include <drivers/acpi/acpi.h>
#include <kern/clock.h>
#include <kern/sysctl.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* The nodes of the stand-in namespace. */
enum node_index {
	NODE_ZONE,
	NODE_TCPU,
	NODE_TSKN,
	NODE_ABSENT,
	NODE_PLAIN,
	NODE_METHOD,
	NODE_COUNT
};

/* One node: its type, path, and what its methods answer (0: not there). */
struct stand_in_node {
	enum drv_acpi_type type;
	const char *path;
	int has_status;
	uint64_t status;
	uint64_t tmp;
	uint64_t psv;
	uint64_t crt;
};

struct thread;
struct spinlock;
struct wait_queue;

void *kern_memcpy(void *destination, const void *source, size_t count);
void *kern_memset(void *destination, int value, size_t count);
size_t kern_strlen(const char *string);
int kern_strncmp(const char *left, const char *right, size_t count);
int kern_snprintf(char *buffer, size_t size, const char *format, ...);
void kern_logf(const char *format, ...);
uint64_t sched_ticks(void);
int kthread_create(void (*entry)(void *), void *argument, int priority, struct thread **result);
void thread_start(struct thread *thread);
void spin_init(struct spinlock *lock, int rank, const char *name);
unsigned long spin_lock_irqsave(struct spinlock *lock);
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled);
void waitq_init(struct wait_queue *queue, const char *name);
uint64_t waitq_sequence(const struct wait_queue *queue);
int waitq_sleep(struct wait_queue *queue, struct spinlock *lock, uint64_t observed, uint64_t deadline, unsigned flags);
static void check(int condition, const char *what);
static unsigned read_entries(struct thermal_entry *entries, unsigned capacity);
static const struct thermal_entry *find_entry(const struct thermal_entry *entries, unsigned count, const char *path);
static void script_step(uint64_t deadline);

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

/* The namespace (tenths of a kelvin: 3132 is 40.05 C, 3732 is 100.05 C). */
static struct stand_in_node nodes[NODE_COUNT] = {
	{ DRV_ACPI_TYPE_THERMAL_ZONE, "\\_TZ_.TZ00", 0, 0, 3132, 0, 3732 },
	{ DRV_ACPI_TYPE_DEVICE, "\\_SB_.PC00.TCPU", 0, 0, 3232, 3532, 3832 },
	{ DRV_ACPI_TYPE_DEVICE, "\\_SB_.PC00.LPCB.ECDV.TSKN", 1, 0x0f, 1, 3482, 4002 },
	{ DRV_ACPI_TYPE_DEVICE, "\\_SB_.PC00.LPCB.ECDV.GONE", 1, 0, 3000, 0, 0 },
	{ DRV_ACPI_TYPE_DEVICE, "\\_SB_.PC00.LPCB", 0, 0, 0, 0, 0 },
	{ DRV_ACPI_TYPE_METHOD, "\\_SB_.PC00.TCPU._TMP", 0, 0, 3000, 0, 0 }
};

/* The lock's depth, the clock in ticks, and the source listed in hw.thermal. */
static int lock_depth;
static uint64_t now_ticks = 1000;
static kern_thermal_read_t listed_read;
static void *listed_context;
static int listings;

/* The thread the driver made, where its loop is left, and the script's step. */
static void (*thread_entry)(void *);
static int thread_started;
static jmp_buf thread_exit;
static unsigned step;

/*
 * Attaches the driver, reads hw.thermal's entries, and runs the thread
 * through the script; the exit status is 1 when a check failed.
 */
int
main(void)
{
	struct thermal_entry entries[8];
	const struct thermal_entry *entry;
	unsigned count;
	int error;

	/* Attach. */
	error = drv_acpi_thermal_attach();
	check(error == 0, "attach finds sensors");
	check(listings == 1 && listed_read != NULL, "the sensors are listed in hw.thermal once");
	check(thread_started == 1, "the thread is started");

	/* The entries. */
	count = read_entries(entries, 8U);
	check(count == 3U, "the zone, TCPU and TSKN; not the absent device, the device without _TMP, nor the method");
	entry = find_entry(entries, count, "\\_TZ_.TZ00");
	check(entry != NULL && entry->kind == THERMAL_KIND_ZONE && entry->flags == 0U, "the zone, not the processor's");
	check(entry != NULL && entry->valid == (THERMAL_HAVE_TEMPERATURE | THERMAL_HAVE_CRITICAL), "the zone: temperature and critical");
	check(entry != NULL && entry->milli_celsius == 40050 && entry->critical_milli_celsius == 100050, "the zone: 40.05 C, critical 100.05 C");
	entry = find_entry(entries, count, "\\_SB_.PC00.TCPU");
	check(entry != NULL && entry->kind == THERMAL_KIND_DEVICE && entry->flags == THERMAL_FLAG_CPU, "TCPU: a device, the processor's");
	check(entry != NULL && entry->valid == (THERMAL_HAVE_TEMPERATURE | THERMAL_HAVE_PASSIVE | THERMAL_HAVE_CRITICAL),
	      "TCPU: all three values");
	check(entry != NULL && entry->milli_celsius == 50050 && entry->passive_milli_celsius == 80050 &&
	      entry->critical_milli_celsius == 110050, "TCPU: 50.05 C, passive 80.05 C, critical 110.05 C");
	check(entry != NULL && entry->time_ns != 0U, "TCPU: the time it was read");
	entry = find_entry(entries, count, "\\_SB_.PC00.LPCB.ECDV.TSKN");
	check(entry != NULL && (entry->valid & THERMAL_HAVE_TEMPERATURE) == 0U, "TSKN: no temperature while the EC answers 0");
	check(entry != NULL && (entry->valid & THERMAL_HAVE_PASSIVE) != 0U && entry->passive_milli_celsius == 75050,
	      "TSKN: its passive trip point");
	count = read_entries(entries, 2U);
	check(count == 2U, "a smaller room takes as many as fit");

	/* The thread, through the script. */
	if (setjmp(thread_exit) == 0)
		thread_entry(NULL);
	check(step == 2U, "the script ran to its end");

	/* The verdict. */
	printf("host-acpi-thermal: %d checks, %d failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Reads the listed source as hw.thermal does: zeroed entries, at most capacity. */
static unsigned
read_entries(
	struct thermal_entry *entries,
	unsigned capacity)
{
	unsigned count;
	int error;

	/* The source's copy. */
	memset(entries, 0, capacity * sizeof(*entries));
	count = 0;
	error = listed_read(listed_context, entries, capacity, &count);
	check(error == 0, "the copy succeeds");
	check(lock_depth == 0, "the copy lets the lock go");
	return count;
}

/* Finds an entry by its path. */
static const struct thermal_entry *
find_entry(
	const struct thermal_entry *entries,
	unsigned count,
	const char *path)
{
	unsigned index;
	int same;

	/* Each entry. */
	for (index = 0; index < count; index++) {
		same = strcmp(entries[index].name, path);
		if (same == 0)
			return &entries[index];
	}

	/* None. */
	return NULL;
}

/* Runs one step of the script at each of the thread's sleeps. */
static void
script_step(
	uint64_t deadline)
{
	struct thermal_entry entries[8];
	const struct thermal_entry *entry;
	unsigned count;

	/* The steps. */
	switch (step) {
	case 0:
		/* The first sleep is for the period; the EC reads TSKN and TCPU warms, then the period passes. */
		check(deadline == now_ticks + KERN_MS_TO_TICKS(5000U), "the thread sleeps for 5 seconds");
		nodes[NODE_TSKN].tmp = 3052;
		nodes[NODE_TCPU].tmp = 3332;
		count = read_entries(entries, 8U);
		entry = find_entry(entries, count, "\\_SB_.PC00.TCPU");
		check(entry != NULL && entry->milli_celsius == 50050, "not read again before the period");
		now_ticks = deadline;
		break;
	case 1:
		/* Read again: TSKN has its temperature, TCPU the new one. */
		count = read_entries(entries, 8U);
		entry = find_entry(entries, count, "\\_SB_.PC00.LPCB.ECDV.TSKN");
		check(entry != NULL && (entry->valid & THERMAL_HAVE_TEMPERATURE) != 0U && entry->milli_celsius == 32050,
		      "TSKN read after the period: 32.05 C");
		entry = find_entry(entries, count, "\\_SB_.PC00.TCPU");
		check(entry != NULL && entry->milli_celsius == 60050, "TCPU read after the period: 60.05 C");
		step++;
		longjmp(thread_exit, 1);
	default:
		check(0, "no step past the script");
		longjmp(thread_exit, 1);
	}

	/* The next step. */
	step++;
}

/* Counts one check, and reports it when it fails. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Failed. */
	failures++;
	printf("FAILED: %s\n", what);
}

void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t count)
{
	/* The host C library. */
	return memcpy(destination, source, count);
}

void *
kern_memset(
	void *destination,
	int value,
	size_t count)
{
	/* The host C library. */
	return memset(destination, value, count);
}

size_t
kern_strlen(
	const char *string)
{
	/* The host C library. */
	return strlen(string);
}

int
kern_strncmp(
	const char *left,
	const char *right,
	size_t count)
{
	/* The host C library. */
	return strncmp(left, right, count);
}

int
kern_snprintf(
	char *buffer,
	size_t size,
	const char *format,
	...)
{
	/* Not used by the driver's paths here. */
	(void)format;
	if (size != 0U)
		buffer[0] = '\0';
	return 0;
}

void
kern_logf(
	const char *format,
	...)
{
	/* The log is not checked. */
	(void)format;
}

uint64_t
sched_ticks(void)
{
	/* The test's clock. */
	return now_ticks;
}

int
kthread_create(
	void (*entry)(void *),
	void *argument,
	int priority,
	struct thread **result)
{
	/* Recorded; the test runs the thread itself. */
	(void)argument;
	(void)priority;
	thread_entry = entry;
	*result = (struct thread *)&thread_entry;
	return 0;
}

void
thread_start(
	struct thread *thread)
{
	/* The thread the driver made. */
	check(thread == (struct thread *)&thread_entry, "the thread started is the one made");
	thread_started++;
}

void
spin_init(
	struct spinlock *lock,
	int rank,
	const char *name)
{
	/* Nothing to make. */
	(void)lock;
	(void)rank;
	(void)name;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* One level. */
	(void)lock;
	check(lock_depth == 0, "the thermal lock is not taken twice");
	lock_depth++;
	return 0;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Back to none. */
	(void)lock;
	(void)enabled;
	check(lock_depth == 1, "the thermal lock is held at its release");
	lock_depth--;
}

void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* Nothing to make. */
	(void)queue;
	(void)name;
}

uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* Any value. */
	(void)queue;
	return 0;
}

int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	/* The thread sleeps holding the lock; the script's step runs with it free. */
	(void)queue;
	(void)lock;
	(void)observed;
	(void)flags;
	check(lock_depth == 1, "the thread sleeps holding the lock");
	lock_depth--;
	script_step(deadline);
	lock_depth++;
	return 0;
}

int
kern_thermal_register(
	kern_thermal_read_t read,
	void *context)
{
	/* Recorded. */
	listed_read = read;
	listed_context = context;
	listings++;
	return 0;
}

int
drv_acpi_walk(
	struct drv_acpi_node *scope,
	drv_acpi_walk_visitor_t visitor,
	void *argument)
{
	unsigned index;
	int stop;

	/* Each node, from the root. */
	check(scope == NULL, "the walk starts at the root");
	for (index = 0; index < NODE_COUNT; index++) {
		stop = visitor((struct drv_acpi_node *)&nodes[index], 2U, argument);
		if (stop != 0)
			return stop;
	}

	/* Succeeded. */
	return 0;
}

enum drv_acpi_type
drv_acpi_node_type(
	const struct drv_acpi_node *node)
{
	/* The node's. */
	return ((const struct stand_in_node *)(const void *)node)->type;
}

int
drv_acpi_node_path(
	const struct drv_acpi_node *node,
	char *buffer,
	size_t size)
{
	/* The node's. */
	snprintf(buffer, size, "%s", ((const struct stand_in_node *)(const void *)node)->path);
	return 0;
}

int
drv_acpi_lookup(
	struct drv_acpi_node *scope,
	const char *path,
	struct drv_acpi_node **result)
{
	const struct stand_in_node *node;

	/* Only _TMP is looked up: there when the node answers one. */
	node = (const struct stand_in_node *)(const void *)scope;
	check(strcmp(path, "_TMP") == 0, "only _TMP is looked up");
	if (node->tmp == 0U)
		return ENOENT;
	*result = (struct drv_acpi_node *)&nodes[NODE_METHOD];
	return 0;
}

int
drv_acpi_evaluate_integer(
	struct drv_acpi_node *scope,
	const char *path,
	uint64_t *value)
{
	const struct stand_in_node *node;
	uint64_t answer;
	int same;

	/* Evaluated with the driver's lock free. */
	check(lock_depth == 0, "the AML is evaluated with the thermal lock free");
	node = (const struct stand_in_node *)(const void *)scope;

	/* _STA, when the node has one. */
	same = strcmp(path, "_STA");
	if (same == 0) {
		if (!node->has_status)
			return ENOENT;
		*value = node->status;
		return 0;
	}

	/* The temperatures (0: not there; 1: the EC's 0). */
	answer = 0;
	same = strcmp(path, "_TMP");
	if (same == 0)
		answer = node->tmp;
	same = strcmp(path, "_PSV");
	if (same == 0)
		answer = node->psv;
	same = strcmp(path, "_CRT");
	if (same == 0)
		answer = node->crt;
	if (answer == 0U)
		return ENOENT;
	if (answer == 1U)
		answer = 0;

	/* Succeeded. */
	*value = answer;
	return 0;
}
