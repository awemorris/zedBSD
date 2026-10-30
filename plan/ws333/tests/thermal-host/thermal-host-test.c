/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the thermal core (src/kern/thermal.c).
 *
 * The core runs with its three threads as host threads.  The kernel's
 * services are small models: a spinlock is a host mutex, a wait queue is
 * one host condition variable with the queue's sequence, and the clock
 * runs twenty times faster than real time (a tick is a real millisecond
 * and a kernel second is fifty ticks), so the five-second waits and the
 * thirty-second age take a fraction of a second.  Mock receivers, fans and
 * meters stand for the drivers.  Each scenario runs in its own process,
 * since the core's tables are for the life of the kernel.
 */

/* The kernel's sched_yield() is not the host's; the kernel's is renamed while its headers are read. */
#define sched_yield test_kernel_sched_yield
#include <kern/thermal.h>
#include <kern/thermal_private.h>
#include <kern/clock.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/sysctl.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>
#undef sched_yield

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* A kernel second in real milliseconds. */
#define TEST_SPEEDUP		20U

static bool test_verbose;
static unsigned test_failures;
static unsigned test_checks;

#define CHECK(condition) test_check((condition) ? 1 : 0, #condition, __LINE__)

static void
test_check(int ok, const char *text, int line)
{
	test_checks++;
	if (!ok) {
		test_failures++;
		printf("FAIL line %d: %s\n", line, text);
		fflush(stdout);
	}
}

/* ---- The kernel's services, as models. ---- */

#define TEST_LOCKS	64U

static struct {
	struct spinlock *lock;
	pthread_mutex_t mutex;
} test_locks[TEST_LOCKS];
static unsigned test_lock_count;
static pthread_mutex_t test_lock_table = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t test_wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_wait_cond = PTHREAD_COND_INITIALIZER;
static struct timespec test_start;

static pthread_mutex_t *
test_mutex(struct spinlock *lock)
{
	unsigned index;
	pthread_mutex_t *found;

	found = NULL;
	pthread_mutex_lock(&test_lock_table);
	for (index = 0; index < test_lock_count; index++) {
		if (test_locks[index].lock == lock)
			found = &test_locks[index].mutex;
	}
	if (found == NULL) {
		if (test_lock_count >= TEST_LOCKS) {
			printf("FAIL: too many locks\n");
			exit(2);
		}
		test_locks[test_lock_count].lock = lock;
		pthread_mutex_init(&test_locks[test_lock_count].mutex, NULL);
		found = &test_locks[test_lock_count].mutex;
		test_lock_count++;
	}
	pthread_mutex_unlock(&test_lock_table);
	return found;
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
	(void)test_mutex(lock);
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	pthread_mutex_lock(test_mutex(lock));
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	pthread_mutex_unlock(test_mutex(lock));
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	memset(queue, 0, sizeof(*queue));
	queue->sequence = 1;
	queue->name = name;
}

uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	uint64_t sequence;

	pthread_mutex_lock(&test_wait_mutex);
	sequence = queue->sequence;
	pthread_mutex_unlock(&test_wait_mutex);
	return sequence;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	pthread_mutex_lock(&test_wait_mutex);
	queue->sequence++;
	pthread_cond_broadcast(&test_wait_cond);
	pthread_mutex_unlock(&test_wait_mutex);
}

uint64_t
sched_ticks(void)
{
	struct timespec now;
	uint64_t ms;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (uint64_t)(now.tv_sec - test_start.tv_sec) * 1000U;
	ms += (uint64_t)(now.tv_nsec / 1000000) - (uint64_t)(test_start.tv_nsec / 1000000);
	return ms + 1000U;
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock, uint64_t observed, uint64_t deadline, unsigned flags)
{
	struct timespec until;
	uint64_t now;
	uint64_t wait_ms;
	int error;

	(void)flags;
	error = 0;
	pthread_mutex_lock(&test_wait_mutex);
	if (queue->sequence != observed) {
		pthread_mutex_unlock(&test_wait_mutex);
		return EAGAIN;
	}
	pthread_mutex_unlock(test_mutex(lock));
	if (deadline == 0) {
		pthread_cond_wait(&test_wait_cond, &test_wait_mutex);
	} else {
		now = sched_ticks();
		wait_ms = deadline > now ? deadline - now : 0;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += (time_t)(wait_ms / 1000U);
		until.tv_nsec += (long)(wait_ms % 1000U) * 1000000L;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		if (pthread_cond_timedwait(&test_wait_cond, &test_wait_mutex, &until) != 0)
			error = ETIMEDOUT;
	}
	pthread_mutex_unlock(&test_wait_mutex);
	pthread_mutex_lock(test_mutex(lock));
	return error;
}

uint64_t
kern_ms_to_ticks(uint64_t milliseconds)
{
	uint64_t ticks;

	ticks = milliseconds / TEST_SPEEDUP;
	if (ticks == 0)
		ticks = 1;
	return ticks;
}

uint64_t
kern_ticks_to_ms(uint64_t ticks)
{
	return ticks * TEST_SPEEDUP;
}

uint64_t
kern_ticks_to_ns(uint64_t ticks)
{
	return ticks * TEST_SPEEDUP * 1000000U;
}

static void *
test_thread_main(void *argument)
{
	struct thread *thread;

	thread = argument;
	thread->entry(thread->argument);
	return NULL;
}

static unsigned test_threads;

int
kthread_create(void (*entry)(void *), void *arg, int priority, struct thread **result)
{
	struct thread *thread;

	(void)priority;
	thread = calloc(1, sizeof(*thread));
	thread->entry = entry;
	thread->argument = arg;
	*result = thread;
	return 0;
}

void
thread_start(struct thread *thread)
{
	pthread_t host;

	thread->started = 1;
	test_threads++;
	pthread_create(&host, NULL, test_thread_main, thread);
	pthread_detach(host);
}

void
kern_logf(const char *format, ...)
{
	va_list arguments;

	if (!test_verbose)
		return;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

/* hw.thermal's sources, as the sysctl would keep them. */
static kern_thermal_read_t test_sysctl_read[4];
static void *test_sysctl_context[4];
static unsigned test_sysctl_count;

int
kern_thermal_register(kern_thermal_read_t read, void *context)
{
	test_sysctl_read[test_sysctl_count] = read;
	test_sysctl_context[test_sysctl_count] = context;
	test_sysctl_count++;
	return 0;
}

/* ---- Helpers. ---- */

static void
test_sleep_ms(unsigned milliseconds)
{
	usleep(milliseconds * 1000U);
}

/* Waits up to a real second for a condition the threads make true. */
#define WAIT_FOR(condition) do { \
	unsigned waited_; \
	for (waited_ = 0; waited_ < 2000U && !(condition); waited_ += 5U) \
		test_sleep_ms(5U); \
} while (0)

static struct kern_thermal_strategy
test_strategy(uint32_t value)
{
	struct kern_thermal_strategy strategy;

	memset(&strategy, 0, sizeof(strategy));
	strategy.value = value;
	return strategy;
}

/* ---- Mock receivers. ---- */

struct test_receiver {
	int answer;
	uint32_t flags;
	unsigned delay_ms;
	unsigned slow_calls;		/* the first calls that take delay_ms */
	unsigned calls;
	uint32_t values[16];
	bool custom;
	const char *firmware;
	int firmware_error;
	unsigned polls;
};

static int
test_apply(void *context, const struct kern_thermal_strategy *strategy, uint32_t *flags)
{
	struct test_receiver *receiver;
	unsigned call;

	receiver = context;
	call = __atomic_fetch_add(&receiver->calls, 1U, __ATOMIC_SEQ_CST);
	if (call < 16U)
		receiver->values[call] = strategy->value;
	if (call < receiver->slow_calls)
		test_sleep_ms(receiver->delay_ms);
	*flags = receiver->flags;
	return receiver->answer;
}

static bool
test_is_custom(void *context)
{
	struct test_receiver *receiver;

	receiver = context;
	return receiver->custom;
}

static int
test_firmware_name(void *context, char *name, size_t size)
{
	struct test_receiver *receiver;

	receiver = context;
	if (receiver->firmware_error != 0)
		return receiver->firmware_error;
	snprintf(name, size, "%s", receiver->firmware);
	return 0;
}

static void
test_poll(void *context)
{
	struct test_receiver *receiver;

	receiver = context;
	__atomic_fetch_add(&receiver->polls, 1U, __ATOMIC_SEQ_CST);
}

static const struct kern_thermal_receiver_ops test_receiver_ops = {
	test_apply, NULL, test_poll, test_is_custom, test_firmware_name,
};

static const struct kern_thermal_receiver_ops test_receiver_plain = {
	test_apply, NULL, NULL, NULL, NULL,
};

static uint32_t
test_receiver_generation(unsigned index)
{
	struct kern_thermal_receiver_info info;

	if (kern_thermal_receiver_get(index, &info) != 0)
		return 0xffffffffU;
	return info.generation;
}

/* ---- Mock fans and meters. ---- */

struct test_fan {
	struct kern_thermal_fan_state state;
	int read_error;
	int boost_error;
	uint32_t boosted;
	unsigned boosts;
	unsigned reads;
};

static int
test_fan_read(void *context, struct kern_thermal_fan_state *state)
{
	struct test_fan *fan;

	fan = context;
	__atomic_fetch_add(&fan->reads, 1U, __ATOMIC_SEQ_CST);
	if (fan->read_error != 0)
		return fan->read_error;
	*state = fan->state;
	return 0;
}

static int
test_fan_boost(void *context, uint32_t level)
{
	struct test_fan *fan;

	fan = context;
	fan->boosts++;
	if (fan->boost_error != 0)
		return fan->boost_error;
	fan->boosted = level;
	return 0;
}

static const struct kern_thermal_fan_ops test_fan_ops = { test_fan_read, test_fan_boost };
static const struct kern_thermal_fan_ops test_fan_read_only = { test_fan_read, NULL };

struct test_meter {
	volatile uint64_t count;
	int error;
	unsigned reads;
};

static int
test_meter_read(void *context, uint64_t *count)
{
	struct test_meter *meter;

	meter = context;
	__atomic_fetch_add(&meter->reads, 1U, __ATOMIC_SEQ_CST);
	if (meter->error != 0)
		return meter->error;
	*count = meter->count;
	return 0;
}

static int test_ioctl_calls;

static int
test_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	(void)file;
	(void)argument;
	test_ioctl_calls++;
	return (int)(request & 0xffU);
}

/* ---- Scenarios. ---- */

/* The registrations: their checks and the tables' limits. */
static void
scenario_registration(void)
{
	static struct test_receiver receivers[17];
	static struct kern_thermal_meter_ops meter_ops;
	static struct test_meter meter;
	static struct test_fan fan;
	struct kern_thermal_sensor *sensor;
	struct kern_thermal_status status;
	unsigned index;
	char name[16];
	int error;

	kern_thermal_init();

	/* Receivers. */
	CHECK(kern_thermal_receiver_register(NULL, &test_receiver_plain, NULL) == EINVAL);
	CHECK(kern_thermal_receiver_register("", &test_receiver_plain, NULL) == EINVAL);
	CHECK(kern_thermal_receiver_register("x", NULL, NULL) == EINVAL);
	for (index = 0; index < 16U; index++) {
		snprintf(name, sizeof(name), "receiver-%u", index);
		CHECK(kern_thermal_receiver_register(name, &test_receiver_plain, &receivers[index]) == 0);
	}
	CHECK(kern_thermal_receiver_register("one-too-many", &test_receiver_plain, &receivers[16]) == ENOSPC);

	/* A long name is cut. */
	{
		struct kern_thermal_receiver_info info;

		CHECK(kern_thermal_receiver_get(0, &info) == 0);
		CHECK(strcmp(info.name, "receiver-0") == 0);
		CHECK(kern_thermal_receiver_get(16, &info) == ENOENT);
	}

	/* ioctl ranges. */
	CHECK(kern_thermal_ioctl_range_register(0, 5, test_ioctl) == EINVAL);
	CHECK(kern_thermal_ioctl_range_register(6, 5, test_ioctl) == EINVAL);
	CHECK(kern_thermal_ioctl_range_register(1, 256, test_ioctl) == EINVAL);
	CHECK(kern_thermal_ioctl_range_register(1, 5, NULL) == EINVAL);
	CHECK(kern_thermal_ioctl_range_register(KERN_THERMAL_IOCTL_CPU_FIRST, KERN_THERMAL_IOCTL_CPU_LAST, test_ioctl) == 0);
	CHECK(kern_thermal_ioctl_range_register(60, 70, test_ioctl) == EBUSY);
	CHECK(kern_thermal_ioctl_range_register(20, 32, test_ioctl) == EBUSY);
	CHECK(kern_thermal_ioctl_range_register(64, 70, test_ioctl) == 0);
	CHECK(kern_thermal_ioctl_range_register(71, 80, test_ioctl) == 0);
	CHECK(kern_thermal_ioctl_range_register(81, 90, test_ioctl) == 0);
	CHECK(kern_thermal_ioctl_range_register(91, 100, test_ioctl) == ENOSPC);

	/* Dispatch: the group 'T', the number in a range. */
	CHECK(kern_thermal_ioctl_dispatch(NULL, ('T' << 8) | 33U, 0) == 33);
	CHECK(test_ioctl_calls == 1);
	CHECK(kern_thermal_ioctl_dispatch(NULL, 0xc0105400UL | 40U, 0) == 40);
	CHECK(kern_thermal_ioctl_dispatch(NULL, ('T' << 8) | 1U, 0) == ENOTTY);
	CHECK(kern_thermal_ioctl_dispatch(NULL, ('S' << 8) | 33U, 0) == ENOTTY);
	CHECK(kern_thermal_ioctl_dispatch(NULL, ('T' << 8) | 120U, 0) == ENOTTY);
	CHECK(test_ioctl_calls == 2);

	/* Sensors. */
	CHECK(kern_thermal_sensor_register("s", 3U, 0, &sensor) == EINVAL);
	CHECK(kern_thermal_sensor_register("s", KERN_THERMAL_KIND_ZONE, 0x2U, &sensor) == EINVAL);
	CHECK(kern_thermal_sensor_register("s", KERN_THERMAL_KIND_ZONE, 0, NULL) == EINVAL);
	CHECK(kern_thermal_sensor_register(NULL, KERN_THERMAL_KIND_ZONE, 0, &sensor) == EINVAL);
	for (index = 0; index < KERN_THERMAL_SENSOR_MAX; index++) {
		snprintf(name, sizeof(name), "sensor-%u", index);
		error = kern_thermal_sensor_register(name, KERN_THERMAL_KIND_DEVICE, 0, &sensor);
		CHECK(error == 0);
	}
	CHECK(kern_thermal_sensor_register("full", KERN_THERMAL_KIND_DEVICE, 0, &sensor) == ENOSPC);
	kern_thermal_sensor_update(NULL, 1000, true);
	kern_thermal_sensor_levels(NULL, 1000, 2000);

	/* Fans. */
	memset(&fan, 0, sizeof(fan));
	CHECK(kern_thermal_fan_register("", 0, &test_fan_ops, &fan) == EINVAL);
	CHECK(kern_thermal_fan_register("fan", 0, NULL, &fan) == EINVAL);
	for (index = 0; index < KERN_THERMAL_FAN_MAX; index++)
		CHECK(kern_thermal_fan_register("fan", 5000, &test_fan_read_only, &fan) == 0);
	CHECK(kern_thermal_fan_register("fan", 5000, &test_fan_ops, &fan) == ENOSPC);

	/* Meters. */
	memset(&meter, 0, sizeof(meter));
	meter_ops.count_bits = 32;
	meter_ops.unit_numerator = 1;
	meter_ops.unit_denominator = 1;
	meter_ops.read = test_meter_read;
	CHECK(kern_thermal_meter_register("m", 0, &meter_ops, &meter) == EINVAL);
	CHECK(kern_thermal_meter_register("m", 6, &meter_ops, &meter) == EINVAL);
	meter_ops.count_bits = 33;
	CHECK(kern_thermal_meter_register("m", KERN_THERMAL_METER_PACKAGE, &meter_ops, &meter) == EINVAL);
	meter_ops.count_bits = 32;
	meter_ops.unit_denominator = 0;
	CHECK(kern_thermal_meter_register("m", KERN_THERMAL_METER_PACKAGE, &meter_ops, &meter) == EINVAL);
	meter_ops.unit_denominator = 1;
	for (index = 0; index < KERN_THERMAL_METER_MAX; index++)
		CHECK(kern_thermal_meter_register("m", KERN_THERMAL_METER_PACKAGE, &meter_ops, &meter) == 0);
	CHECK(kern_thermal_meter_register("m", KERN_THERMAL_METER_PACKAGE, &meter_ops, &meter) == ENOSPC);

	/* The counts. */
	kern_thermal_status_get(&status);
	CHECK(status.receiver_count == 16U);
	CHECK(status.sensor_count == KERN_THERMAL_SENSOR_MAX);
	CHECK(status.fan_count == KERN_THERMAL_FAN_MAX);
	CHECK(status.meter_count == KERN_THERMAL_METER_MAX);
	CHECK(status.strategy == KERN_THERMAL_STRATEGY_FIRMWARE);
	CHECK(status.generation == 0U);
	CHECK(status.strategy_flags == 0U);
	CHECK(test_threads == 3U);
	CHECK(test_sysctl_count == 2U);
}

/* The checks of a strategy, and the answer with no receiver. */
static void
scenario_strategy_checks(void)
{
	struct kern_thermal_strategy_info info;
	struct kern_thermal_strategy strategy;
	uint32_t flags;

	kern_thermal_init();

	strategy = test_strategy(KERN_THERMAL_STRATEGY_FIRMWARE);
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EINVAL);
	strategy = test_strategy(5U);
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EINVAL);
	strategy = test_strategy(KERN_THERMAL_STRATEGY_BALANCED);
	strategy.speed = 10;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EINVAL);
	strategy = test_strategy(KERN_THERMAL_STRATEGY_BALANCED);
	strategy.reserved = 1;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EINVAL);
	strategy = test_strategy(KERN_THERMAL_STRATEGY_MANUAL);
	strategy.noise = 101;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EINVAL);
	kern_thermal_strategy_get(&info);
	CHECK(info.generation == 0U);

	/* No receiver: EOPNOTSUPP, and the strategy is kept. */
	strategy = test_strategy(KERN_THERMAL_STRATEGY_MANUAL);
	strategy.speed = 100;
	strategy.noise = 0;
	strategy.gating = 50;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EOPNOTSUPP);
	CHECK(flags == 0U);
	kern_thermal_strategy_get(&info);
	CHECK(info.strategy.value == KERN_THERMAL_STRATEGY_MANUAL);
	CHECK(info.strategy.speed == 100U);
	CHECK(info.strategy.gating == 50U);
	CHECK(info.generation == 1U);
	CHECK(info.flags == 0U);
}

/* The answers of the receivers turned into the chooser's answer. */
static void
scenario_strategy_answers(void)
{
	static struct test_receiver first;
	static struct test_receiver second;
	struct kern_thermal_receiver_info info;
	struct kern_thermal_strategy_info strategy_info;
	struct kern_thermal_strategy strategy;
	uint32_t flags;

	kern_thermal_init();
	CHECK(kern_thermal_receiver_register("first", &test_receiver_plain, &first) == 0);
	CHECK(kern_thermal_receiver_register("second", &test_receiver_plain, &second) == 0);

	/* Both carry it out. */
	strategy = test_strategy(KERN_THERMAL_STRATEGY_PERFORMANCE);
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == 0);
	CHECK(flags == 0U);
	CHECK(first.calls == 1U && second.calls == 1U);
	CHECK(first.values[0] == KERN_THERMAL_STRATEGY_PERFORMANCE);
	CHECK(test_receiver_generation(0) == 1U && test_receiver_generation(1) == 1U);

	/* Both say it does not apply. */
	first.answer = EOPNOTSUPP;
	second.answer = EOPNOTSUPP;
	strategy = test_strategy(KERN_THERMAL_STRATEGY_BALANCED);
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EOPNOTSUPP);

	/* One does not apply, one carries it out. */
	second.answer = 0;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == 0);
	CHECK(flags == 0U);

	/* Every acting receiver fails: the first errno. */
	first.answer = EIO;
	second.answer = EBUSY;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EIO);
	CHECK(flags == 0U);

	/* One does not apply, the other fails: that one's errno. */
	first.answer = EOPNOTSUPP;
	second.answer = EBUSY;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == EBUSY);

	/* One fails, one carries it out: PARTIAL. */
	first.answer = EIO;
	second.answer = 0;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == 0);
	CHECK(flags == KERN_THERMAL_STRATEGY_PARTIAL);
	kern_thermal_strategy_get(&strategy_info);
	CHECK(strategy_info.flags == KERN_THERMAL_STRATEGY_PARTIAL);
	CHECK(kern_thermal_receiver_get(0, &info) == 0);
	CHECK(info.error == EIO);

	/* Unconfirmed is no failure. */
	first.answer = 0;
	first.flags = KERN_THERMAL_APPLY_UNCONFIRMED;
	second.answer = 0;
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == 0);
	CHECK(flags == 0U);
	CHECK(kern_thermal_receiver_get(0, &info) == 0);
	CHECK(info.flags == KERN_THERMAL_APPLY_UNCONFIRMED);
	CHECK(info.generation == 7U);
	CHECK(info.time_ns != 0U);
}

/* A slow receiver: PENDING, then the outcome as an event. */
static void
scenario_strategy_pending(void)
{
	static struct test_receiver slow;
	static struct kern_thermal_reader reader;
	struct kern_thermal_strategy_info info;
	struct kern_thermal_strategy strategy;
	struct kern_thermal_record records[4];
	uint32_t flags;
	unsigned taken;

	kern_thermal_init();
	kern_thermal_reader_attach(&reader, 1U << KERN_THERMAL_EVENT_STRATEGY);
	slow.delay_ms = 600;		/* past the five kernel seconds (250 ms) */
	slow.slow_calls = 1;
	CHECK(kern_thermal_receiver_register("slow", &test_receiver_plain, &slow) == 0);

	strategy = test_strategy(KERN_THERMAL_STRATEGY_POWERSAVE);
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == 0);
	CHECK(flags == KERN_THERMAL_STRATEGY_PENDING);
	kern_thermal_strategy_get(&info);
	CHECK(info.flags == KERN_THERMAL_STRATEGY_PENDING);
	CHECK(!kern_thermal_reader_ready(&reader));

	/* The answer comes later, with an event. */
	WAIT_FOR(kern_thermal_reader_ready(&reader));
	kern_thermal_strategy_get(&info);
	CHECK(info.flags == 0U);
	taken = kern_thermal_reader_take(&reader, records, 4);
	CHECK(taken == 1U);
	CHECK(records[0].type == KERN_THERMAL_EVENT_STRATEGY);
	CHECK(records[0].index == KERN_THERMAL_STRATEGY_POWERSAVE);
	CHECK(records[0].value == 1);
	kern_thermal_reader_detach(&reader);
}

struct test_chooser {
	uint32_t value;
	int error;
	uint32_t flags;
};

static void *
test_choose(void *argument)
{
	struct test_chooser *chooser;
	struct kern_thermal_strategy strategy;

	chooser = argument;
	strategy = test_strategy(chooser->value);
	chooser->error = kern_thermal_strategy_set(&strategy, &chooser->flags);
	return NULL;
}

/* A choice made while the receivers work on an earlier one: SUPERSEDED, and a second pass for all. */
static void
scenario_strategy_superseded(void)
{
	static struct test_receiver first;
	static struct test_receiver second;
	struct test_chooser early;
	struct test_chooser late;
	pthread_t early_thread;
	pthread_t late_thread;

	kern_thermal_init();
	first.delay_ms = 100;		/* two kernel seconds, under the wait */
	first.slow_calls = 1;
	CHECK(kern_thermal_receiver_register("first", &test_receiver_plain, &first) == 0);
	CHECK(kern_thermal_receiver_register("second", &test_receiver_plain, &second) == 0);

	early.value = KERN_THERMAL_STRATEGY_PERFORMANCE;
	late.value = KERN_THERMAL_STRATEGY_POWERSAVE;
	pthread_create(&early_thread, NULL, test_choose, &early);
	WAIT_FOR(__atomic_load_n(&first.calls, __ATOMIC_SEQ_CST) == 1U);
	pthread_create(&late_thread, NULL, test_choose, &late);
	pthread_join(early_thread, NULL);
	pthread_join(late_thread, NULL);

	CHECK(early.error == 0);
	CHECK(early.flags == KERN_THERMAL_STRATEGY_SUPERSEDED);
	CHECK(late.error == 0);
	CHECK(late.flags == 0U);
	CHECK(first.calls == 2U);
	CHECK(first.values[0] == KERN_THERMAL_STRATEGY_PERFORMANCE);
	CHECK(first.values[1] == KERN_THERMAL_STRATEGY_POWERSAVE);
	CHECK(second.calls == 2U);
	CHECK(second.values[0] == KERN_THERMAL_STRATEGY_PERFORMANCE);
	CHECK(second.values[1] == KERN_THERMAL_STRATEGY_POWERSAVE);
	CHECK(test_receiver_generation(0) == 2U && test_receiver_generation(1) == 2U);
}

/* A receiver registered after a choice is handed it; CUSTOM, the firmware's name and poll. */
static void
scenario_strategy_late_and_custom(void)
{
	static struct test_receiver early;
	static struct test_receiver late;
	static struct test_receiver named;
	struct kern_thermal_strategy_info info;
	struct kern_thermal_status status;
	struct kern_thermal_strategy strategy;
	uint32_t flags;

	kern_thermal_init();
	early.firmware_error = ENOENT;
	CHECK(kern_thermal_receiver_register("early", &test_receiver_ops, &early) == 0);
	strategy = test_strategy(KERN_THERMAL_STRATEGY_BALANCED);
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == 0);

	/* Late registration is handed the strategy in force. */
	CHECK(kern_thermal_receiver_register("late", &test_receiver_plain, &late) == 0);
	WAIT_FOR(test_receiver_generation(1) == 1U);
	CHECK(late.calls == 1U);
	CHECK(late.values[0] == KERN_THERMAL_STRATEGY_BALANCED);
	CHECK(early.calls == 1U);

	/* The firmware's name from the first receiver that gives one; CUSTOM. */
	named.firmware = "Optimized";
	named.custom = true;
	CHECK(kern_thermal_receiver_register("named", &test_receiver_ops, &named) == 0);
	kern_thermal_strategy_get(&info);
	CHECK(strcmp(info.firmware_name, "Optimized") == 0);
	CHECK((info.flags & KERN_THERMAL_STRATEGY_CUSTOM) != 0U);
	kern_thermal_status_get(&status);
	CHECK((status.strategy_flags & KERN_THERMAL_STRATEGY_CUSTOM) != 0U);
	named.custom = false;
	kern_thermal_strategy_get(&info);
	CHECK((info.flags & KERN_THERMAL_STRATEGY_CUSTOM) == 0U);

	/* poll runs every kernel second (50 ms). */
	early.polls = 0;
	test_sleep_ms(400);
	CHECK(early.polls >= 4U);
	CHECK(early.polls <= 10U);
}

/* Registrations and a choice made before kern_thermal_init() are kept and served. */
static void
scenario_before_init(void)
{
	static struct test_receiver receiver;
	struct kern_thermal_strategy strategy;
	struct kern_thermal_status status;
	uint32_t flags;

	CHECK(kern_thermal_receiver_register("before", &test_receiver_plain, &receiver) == 0);
	strategy = test_strategy(KERN_THERMAL_STRATEGY_PERFORMANCE);
	CHECK(kern_thermal_strategy_set(&strategy, &flags) == 0);
	CHECK(flags == KERN_THERMAL_STRATEGY_PENDING);
	CHECK(receiver.calls == 0U);
	kern_thermal_init();
	WAIT_FOR(test_receiver_generation(0) == 1U);
	CHECK(receiver.calls == 1U);
	kern_thermal_status_get(&status);
	CHECK(status.strategy_flags == 0U);
}

/* Sensor readings: range, the jump, levels, hw.thermal's two halves. */
static void
scenario_sensors(void)
{
	struct kern_thermal_sensor *sensors[20];
	struct kern_thermal_sensor_info info;
	struct thermal_entry entries[16];
	unsigned filled;
	unsigned index;
	char name[64];

	kern_thermal_init();
	CHECK(kern_thermal_sensor_register("\\_SB_.PC00.TCPU", KERN_THERMAL_KIND_DEVICE, KERN_THERMAL_SENSOR_CPU, &sensors[0]) == 0);
	for (index = 1; index < 20U; index++) {
		snprintf(name, sizeof(name), "z%02u-sensor-with-a-name-longer-than-its-room", index);
		CHECK(kern_thermal_sensor_register(name, KERN_THERMAL_KIND_ZONE, 0, &sensors[index]) == 0);
	}

	/* No reading yet. */
	CHECK(kern_thermal_sensor_get(0, &info) == 0);
	CHECK(info.reading == 0U && info.time_ns == 0U);
	CHECK(strcmp(info.name, "\\_SB_.PC00.TCPU") == 0);
	CHECK(kern_thermal_sensor_get(1, &info) == 0);
	CHECK(strlen(info.name) == 31U);
	CHECK(kern_thermal_sensor_get(20, &info) == ENOENT);

	/* A reading. */
	kern_thermal_sensor_update(sensors[0], 40000, true);
	CHECK(kern_thermal_sensor_get(0, &info) == 0);
	CHECK(info.reading == KERN_THERMAL_READING_VALID);
	CHECK(info.millicelsius == 40000);
	CHECK(info.time_ns != 0U);

	/* A jump of 20 degrees or more is dropped once, then taken. */
	kern_thermal_sensor_update(sensors[0], 70000, true);
	CHECK(kern_thermal_sensor_get(0, &info) == 0 && info.millicelsius == 40000);
	kern_thermal_sensor_update(sensors[0], 71000, true);
	CHECK(kern_thermal_sensor_get(0, &info) == 0 && info.millicelsius == 71000);
	kern_thermal_sensor_update(sensors[0], 75000, true);
	CHECK(kern_thermal_sensor_get(0, &info) == 0 && info.millicelsius == 75000);

	/* Out of range and failed reads are no reading. */
	kern_thermal_sensor_update(sensors[0], 151000, true);
	CHECK(kern_thermal_sensor_get(0, &info) == 0 && (info.reading & KERN_THERMAL_READING_VALID) == 0U);
	kern_thermal_sensor_update(sensors[0], 76000, true);
	CHECK(kern_thermal_sensor_get(0, &info) == 0 && (info.reading & KERN_THERMAL_READING_VALID) != 0U);
	kern_thermal_sensor_update(sensors[0], -41000, true);
	CHECK(kern_thermal_sensor_get(0, &info) == 0 && (info.reading & KERN_THERMAL_READING_VALID) == 0U);
	kern_thermal_sensor_update(sensors[0], 76000, false);
	CHECK(kern_thermal_sensor_get(0, &info) == 0 && (info.reading & KERN_THERMAL_READING_VALID) == 0U);
	kern_thermal_sensor_update(sensors[0], 77000, true);

	/* Levels; 127 degrees and up, and 0, are none. */
	kern_thermal_sensor_levels(sensors[0], 110000, 127000);
	CHECK(kern_thermal_sensor_get(0, &info) == 0);
	CHECK(info.passive == 110000 && info.critical == 0);
	CHECK(info.reading == (KERN_THERMAL_READING_VALID | KERN_THERMAL_READING_PASSIVE));
	kern_thermal_sensor_levels(sensors[1], 0, 105000);
	kern_thermal_sensor_update(sensors[1], 50000, true);

	/* hw.thermal: two halves of sixteen. */
	CHECK(test_sysctl_count == 2U);
	CHECK(test_sysctl_read[0](test_sysctl_context[0], entries, 16, &filled) == 0);
	CHECK(filled == 16U);
	CHECK(strcmp(entries[0].name, "\\_SB_.PC00.TCPU") == 0);
	CHECK(entries[0].kind == THERMAL_KIND_DEVICE);
	CHECK(entries[0].flags == THERMAL_FLAG_CPU);
	CHECK(entries[0].valid == (THERMAL_HAVE_TEMPERATURE | THERMAL_HAVE_PASSIVE));
	CHECK(entries[0].milli_celsius == 77000);
	CHECK(entries[0].passive_milli_celsius == 110000);
	CHECK(entries[0].time_ns != 0U);
	CHECK(entries[1].kind == THERMAL_KIND_ZONE);
	CHECK(entries[1].valid == (THERMAL_HAVE_TEMPERATURE | THERMAL_HAVE_CRITICAL));
	CHECK(entries[1].critical_milli_celsius == 105000);
	CHECK(entries[2].valid == 0U && entries[2].time_ns == 0U);
	CHECK(test_sysctl_read[1](test_sysctl_context[1], entries, 16, &filled) == 0);
	CHECK(filled == 4U);
	CHECK(strncmp(entries[0].name, "z16-sensor-with-a-name-longer-t", 32) == 0);
	CHECK(test_sysctl_read[0](test_sysctl_context[0], entries, 3, &filled) == 0);
	CHECK(filled == 3U);
}

/* The guard: CRITICAL of a zone, PASSIVE of the processor, with hysteresis; stale and resume. */
static void
scenario_guard(void)
{
	static struct kern_thermal_reader reader;
	struct kern_thermal_sensor *zone;
	struct kern_thermal_sensor *device;
	struct kern_thermal_sensor *cpu;
	struct kern_thermal_sensor_info info;
	struct kern_thermal_record records[8];
	unsigned taken;

	kern_thermal_init();
	kern_thermal_reader_attach(&reader, (1U << KERN_THERMAL_EVENT_CRITICAL) | (1U << KERN_THERMAL_EVENT_PASSIVE));
	CHECK(kern_thermal_sensor_register("zone", KERN_THERMAL_KIND_ZONE, 0, &zone) == 0);
	CHECK(kern_thermal_sensor_register("device", KERN_THERMAL_KIND_DEVICE, 0, &device) == 0);
	CHECK(kern_thermal_sensor_register("cpu", KERN_THERMAL_KIND_DEVICE, KERN_THERMAL_SENSOR_CPU, &cpu) == 0);
	kern_thermal_sensor_levels(zone, 0, 90000);
	kern_thermal_sensor_levels(device, 0, 90000);
	kern_thermal_sensor_levels(cpu, 80000, 0);
	kern_thermal_sensor_update(zone, 85000, true);
	kern_thermal_sensor_update(device, 85000, true);
	kern_thermal_sensor_update(cpu, 70000, true);
	test_sleep_ms(150);
	CHECK(!kern_thermal_reader_ready(&reader));

	/* A zone past its critical level; a device's critical is not watched. */
	kern_thermal_sensor_update(zone, 95000, true);
	kern_thermal_sensor_update(device, 95000, true);
	WAIT_FOR(kern_thermal_reader_ready(&reader));
	taken = kern_thermal_reader_take(&reader, records, 8);
	CHECK(taken == 1U);
	CHECK(records[0].type == KERN_THERMAL_EVENT_CRITICAL && records[0].index == 0U && records[0].value == 1);

	/* Within the hysteresis nothing; below it the level is left. */
	kern_thermal_sensor_update(zone, 88000, true);
	test_sleep_ms(150);
	CHECK(!kern_thermal_reader_ready(&reader));
	kern_thermal_sensor_update(zone, 86000, true);
	WAIT_FOR(kern_thermal_reader_ready(&reader));
	taken = kern_thermal_reader_take(&reader, records, 8);
	CHECK(taken == 1U);
	CHECK(records[0].type == KERN_THERMAL_EVENT_CRITICAL && records[0].value == 0);

	/* The processor's passive level. */
	kern_thermal_sensor_update(cpu, 81000, true);
	WAIT_FOR(kern_thermal_reader_ready(&reader));
	taken = kern_thermal_reader_take(&reader, records, 8);
	CHECK(taken == 1U);
	CHECK(records[0].type == KERN_THERMAL_EVENT_PASSIVE && records[0].index == 2U && records[0].value == 1);

	/* Thirty kernel seconds (1.5 s) without a reading: stale. */
	test_sleep_ms(1700);
	CHECK(kern_thermal_sensor_get(0, &info) == 0);
	CHECK((info.reading & KERN_THERMAL_READING_STALE) != 0U);
	CHECK((info.reading & KERN_THERMAL_READING_VALID) != 0U);

	/* A resume counts the age again from now. */
	kern_thermal_resume();
	CHECK(kern_thermal_sensor_get(0, &info) == 0);
	CHECK((info.reading & KERN_THERMAL_READING_STALE) == 0U);
	test_sleep_ms(150);
	CHECK(kern_thermal_sensor_get(0, &info) == 0);
	CHECK((info.reading & KERN_THERMAL_READING_STALE) == 0U);

	/* A new reading keeps it fresh. */
	kern_thermal_sensor_update(zone, 86500, true);
	test_sleep_ms(150);
	CHECK(kern_thermal_sensor_get(0, &info) == 0);
	CHECK((info.reading & KERN_THERMAL_READING_STALE) == 0U);
	kern_thermal_reader_detach(&reader);
}

/* Fans: the copy, STALE, STALLED and its event; boost. */
static void
scenario_fans(void)
{
	static struct test_fan pi;
	static struct test_fan dell;
	static struct test_fan plain;
	static struct kern_thermal_reader reader;
	struct kern_thermal_fan_info info;
	struct kern_thermal_record records[8];
	uint32_t flags;
	unsigned taken;

	kern_thermal_reader_attach(&reader, (1U << KERN_THERMAL_EVENT_FAN_STALLED) | (1U << KERN_THERMAL_EVENT_BOOST));
	pi.state.flags = KERN_THERMAL_FAN_HAS_RPM | KERN_THERMAL_FAN_HAS_LEVEL;
	pi.state.level = 2;
	pi.state.level_count = 5;
	pi.state.boost_count = 4;
	pi.state.rpm = 2000;
	dell.state.flags = KERN_THERMAL_FAN_HAS_RPM;
	dell.state.rpm = 0;
	dell.state.boost_count = 2;
	plain.state.flags = KERN_THERMAL_FAN_HAS_RPM;
	plain.state.rpm = 3000;
	CHECK(kern_thermal_fan_register("pi", 8000, &test_fan_ops, &pi) == 0);
	CHECK(kern_thermal_fan_register("dell", 5000, &test_fan_ops, &dell) == 0);
	CHECK(kern_thermal_fan_register("plain", 5000, &test_fan_read_only, &plain) == 0);

	/* Not read before the core runs. */
	CHECK(kern_thermal_fan_get(0, &info) == 0);
	CHECK(info.copy == KERN_THERMAL_FAN_COPY_BOOST && info.time_ns == 0U);
	CHECK(kern_thermal_fan_get(2, &info) == 0 && info.copy == 0U);
	CHECK(kern_thermal_fan_get(3, &info) == ENOENT);
	kern_thermal_init();
	WAIT_FOR(pi.reads >= 1U);
	test_sleep_ms(20);
	CHECK(kern_thermal_fan_get(0, &info) == 0);
	CHECK(info.copy == (KERN_THERMAL_FAN_COPY_BOOST | KERN_THERMAL_FAN_COPY_KNOWN));
	CHECK(info.state.rpm == 2000 && info.state.level == 2U && info.rpm_max == 8000U);
	CHECK(strcmp(info.name, "pi") == 0);

	/* Boost: checks. */
	CHECK(kern_thermal_boost_set(3, 1, &flags) == ENOENT);
	CHECK(kern_thermal_boost_set(2, 1, &flags) == EOPNOTSUPP);
	CHECK(kern_thermal_boost_set(0, 5, &flags) == EINVAL);
	CHECK(kern_thermal_boost_set(KERN_THERMAL_FAN_ALL, 3, &flags) == EINVAL);	/* dell has 2 */
	CHECK(pi.boosts == 0U && dell.boosts == 0U);

	/* Boost one fan. */
	CHECK(kern_thermal_boost_set(0, 3, &flags) == 0);
	CHECK(flags == 0U);
	CHECK(pi.boosted == 3U && dell.boosts == 0U);
	CHECK(kern_thermal_fan_get(0, &info) == 0 && info.boost == 3U);
	WAIT_FOR(kern_thermal_reader_ready(&reader));
	taken = kern_thermal_reader_take(&reader, records, 8);
	CHECK(taken == 1U);
	CHECK(records[0].type == KERN_THERMAL_EVENT_BOOST && records[0].index == 0U && records[0].value == 3);

	/* Boost all: a driver's refusal comes back. */
	dell.boost_error = EIO;
	CHECK(kern_thermal_boost_set(KERN_THERMAL_FAN_ALL, 1, &flags) == EIO);
	CHECK(pi.boosted == 1U && dell.boosts == 1U);
	CHECK(kern_thermal_fan_get(1, &info) == 0 && info.boost == 0U);
	dell.boost_error = 0;
	CHECK(kern_thermal_boost_set(KERN_THERMAL_FAN_ALL, 0, &flags) == 0);
	CHECK(pi.boosted == 0U && dell.boosted == 0U);
	(void)kern_thermal_reader_take(&reader, records, 8);

	/* A fan at level 2 that reads 0 rpm three times stalls; dell without a level does not. */
	pi.state.rpm = 0;
	WAIT_FOR(kern_thermal_reader_ready(&reader));
	taken = kern_thermal_reader_take(&reader, records, 8);
	CHECK(taken == 1U);
	CHECK(records[0].type == KERN_THERMAL_EVENT_FAN_STALLED && records[0].index == 0U);
	CHECK(kern_thermal_fan_get(0, &info) == 0 && (info.copy & KERN_THERMAL_FAN_COPY_STALLED) != 0U);
	CHECK(kern_thermal_fan_get(1, &info) == 0 && (info.copy & KERN_THERMAL_FAN_COPY_STALLED) == 0U);

	/* Turning again clears it; a failed read is STALE with the last copy. */
	pi.state.rpm = 1500;
	WAIT_FOR(kern_thermal_fan_get(0, &info) == 0 && (info.copy & KERN_THERMAL_FAN_COPY_STALLED) == 0U);
	CHECK((info.copy & KERN_THERMAL_FAN_COPY_STALLED) == 0U);
	pi.read_error = EIO;
	WAIT_FOR(kern_thermal_fan_get(0, &info) == 0 && (info.copy & KERN_THERMAL_FAN_COPY_STALE) != 0U);
	CHECK((info.copy & KERN_THERMAL_FAN_COPY_STALE) != 0U);
	CHECK(info.state.rpm == 1500U);
	pi.read_error = 0;
	WAIT_FOR(kern_thermal_fan_get(0, &info) == 0 && (info.copy & KERN_THERMAL_FAN_COPY_STALE) == 0U);
	CHECK((info.copy & KERN_THERMAL_FAN_COPY_STALE) == 0U);

	/* The automatic level at 0 but a boost of 1: still should turn. */
	CHECK(kern_thermal_boost_set(0, 1, &flags) == 0);
	pi.state.level = 0;
	pi.state.rpm = 0;
	(void)kern_thermal_reader_take(&reader, records, 8);
	WAIT_FOR(kern_thermal_fan_get(0, &info) == 0 && (info.copy & KERN_THERMAL_FAN_COPY_STALLED) != 0U);
	CHECK((info.copy & KERN_THERMAL_FAN_COPY_STALLED) != 0U);
	kern_thermal_reader_detach(&reader);
}

/* Boost with a slow driver: PENDING. */
static int
test_slow_boost(void *context, uint32_t level)
{
	(void)context;
	(void)level;
	test_sleep_ms(500);
	return 0;
}

static const struct kern_thermal_fan_ops test_slow_fan_ops = { test_fan_read, test_slow_boost };

static void
scenario_boost_pending(void)
{
	static struct test_fan fan;
	uint32_t flags;

	fan.state.flags = KERN_THERMAL_FAN_HAS_RPM;
	fan.state.boost_count = 1;
	CHECK(kern_thermal_fan_register("slow", 5000, &test_slow_fan_ops, &fan) == 0);
	kern_thermal_init();
	WAIT_FOR(fan.reads >= 1U);
	test_sleep_ms(20);
	CHECK(kern_thermal_boost_set(0, 1, &flags) == 0);
	CHECK(flags == KERN_THERMAL_BOOST_PENDING);
}

/* Meters: the first read, the wrap, the remainder, a failed read; the sequence under a reader. */
struct test_meter_watch {
	unsigned index;
	volatile bool stop;
	unsigned reads;
	unsigned backwards;
};

static void *
test_meter_watch_main(void *argument)
{
	struct test_meter_watch *watch;
	struct kern_thermal_meter_info info;
	uint64_t last;

	watch = argument;
	last = 0;
	while (!watch->stop) {
		if (kern_thermal_meter_get(watch->index, &info) == 0) {
			if (info.energy_nj < last)
				watch->backwards++;
			last = info.energy_nj;
			watch->reads++;
		}
	}
	return NULL;
}

static void
scenario_meters(void)
{
	static struct test_meter wide;
	static struct test_meter narrow;
	static struct test_meter thirds;
	static struct kern_thermal_meter_ops wide_ops;
	static struct kern_thermal_meter_ops narrow_ops;
	static struct kern_thermal_meter_ops thirds_ops;
	struct kern_thermal_meter_info info;
	struct test_meter_watch watch;
	pthread_t watcher;
	unsigned reads;

	wide_ops.count_bits = 32;
	wide_ops.unit_numerator = 61035;	/* 2^-14 J in nJ, rounded */
	wide_ops.unit_denominator = 1;
	wide_ops.read = test_meter_read;
	narrow_ops = wide_ops;
	narrow_ops.count_bits = 16;
	narrow_ops.unit_numerator = 1;
	thirds_ops = wide_ops;
	thirds_ops.unit_numerator = 1;
	thirds_ops.unit_denominator = 3;
	wide.count = 0xffffff00U;
	narrow.count = 0xfff0U;
	thirds.count = 0;
	CHECK(kern_thermal_meter_register("package", KERN_THERMAL_METER_PACKAGE, &wide_ops, &wide) == 0);
	CHECK(kern_thermal_meter_register("narrow", KERN_THERMAL_METER_CORES, &narrow_ops, &narrow) == 0);
	CHECK(kern_thermal_meter_register("thirds", KERN_THERMAL_METER_DRAM, &thirds_ops, &thirds) == 0);
	CHECK(kern_thermal_meter_get(0, &info) == 0 && info.copy == 0U && info.time_ns == 0U);
	CHECK(kern_thermal_meter_get(3, &info) == ENOENT);
	kern_thermal_init();

	/* The first read only starts the sum. */
	WAIT_FOR(wide.reads >= 1U && narrow.reads >= 1U && thirds.reads >= 1U);
	test_sleep_ms(10);
	CHECK(kern_thermal_meter_get(0, &info) == 0);
	CHECK(info.copy == KERN_THERMAL_METER_COPY_KNOWN && info.energy_nj == 0U && info.time_ns != 0U);
	CHECK(info.kind == KERN_THERMAL_METER_PACKAGE && strcmp(info.name, "package") == 0);

	/* Across the wrap. */
	wide.count = 0x100U;
	narrow.count = 0x10U;
	reads = wide.reads;
	WAIT_FOR(wide.reads >= reads + 2U);
	CHECK(kern_thermal_meter_get(0, &info) == 0 && info.energy_nj == 0x200ULL * 61035U);
	CHECK(kern_thermal_meter_get(1, &info) == 0 && info.energy_nj == 0x20U);

	/* A third of a nanojoule a count: the remainder is carried. */
	thirds.count = 1;
	reads = thirds.reads;
	WAIT_FOR(thirds.reads >= reads + 2U);
	CHECK(kern_thermal_meter_get(2, &info) == 0 && info.energy_nj == 0U);
	thirds.count = 2;
	reads = thirds.reads;
	WAIT_FOR(thirds.reads >= reads + 2U);
	CHECK(kern_thermal_meter_get(2, &info) == 0 && info.energy_nj == 0U);
	thirds.count = 3;
	reads = thirds.reads;
	WAIT_FOR(thirds.reads >= reads + 2U);
	CHECK(kern_thermal_meter_get(2, &info) == 0 && info.energy_nj == 1U);

	/* A failed read keeps the sum and says STALE. */
	wide.error = EIO;
	reads = wide.reads;
	WAIT_FOR(wide.reads >= reads + 2U);
	CHECK(kern_thermal_meter_get(0, &info) == 0);
	CHECK(info.copy == (KERN_THERMAL_METER_COPY_KNOWN | KERN_THERMAL_METER_COPY_STALE));
	CHECK(info.energy_nj == 0x200ULL * 61035U);
	wide.error = 0;

	/* A reader that never stops sees the sum rise and never fall. */
	memset(&watch, 0, sizeof(watch));
	watch.index = 0;
	pthread_create(&watcher, NULL, test_meter_watch_main, &watch);
	for (reads = 0; reads < 10U; reads++) {
		wide.count = wide.count + 0x40000000U;
		wide.count &= 0xffffffffU;
		test_sleep_ms(55);
	}
	watch.stop = true;
	pthread_join(watcher, NULL);
	CHECK(watch.reads > 100U);
	CHECK(watch.backwards == 0U);
}

/* The rings: the mask, the drop and OVERFLOW, subscribe and detach. */
static void
scenario_events(void)
{
	static struct kern_thermal_reader strategy_reader;
	static struct kern_thermal_reader all_reader;
	struct kern_thermal_record records[80];
	unsigned taken;
	unsigned index;

	kern_thermal_init();
	kern_thermal_reader_attach(&strategy_reader, 1U << KERN_THERMAL_EVENT_STRATEGY);
	kern_thermal_reader_attach(&all_reader, 0xffffffffU);
	CHECK(kern_thermal_reader_take(&strategy_reader, records, 0) == 0U);

	/* Types outside the mask's bits are ignored. */
	kern_thermal_event(0, 0, 0);
	kern_thermal_event(32, 0, 0);
	CHECK(!kern_thermal_reader_ready(&all_reader));

	/* Seventy events into a ring of sixty-four: six dropped, reported once. */
	for (index = 0; index < 70U; index++)
		kern_thermal_event(KERN_THERMAL_EVENT_STRATEGY, index, (int64_t)index * 10);
	kern_thermal_event(KERN_THERMAL_EVENT_BOOST, 0, 0);
	taken = kern_thermal_reader_take(&strategy_reader, records, 80);
	CHECK(taken == 65U);
	CHECK(records[0].type == KERN_THERMAL_EVENT_OVERFLOW && records[0].value == 6);
	CHECK(records[1].type == KERN_THERMAL_EVENT_STRATEGY && records[1].index == 6U && records[1].value == 60);
	CHECK(records[64].index == 69U);
	CHECK(!kern_thermal_reader_ready(&strategy_reader));

	/* The other reader took the BOOST too; take in two parts. */
	taken = kern_thermal_reader_take(&all_reader, records, 10);
	CHECK(taken == 10U && records[0].type == KERN_THERMAL_EVENT_OVERFLOW && records[0].value == 7);
	taken = kern_thermal_reader_take(&all_reader, records, 80);
	CHECK(taken == 55U);
	CHECK(records[54].type == KERN_THERMAL_EVENT_BOOST);

	/* Subscribe and detach. */
	kern_thermal_reader_subscribe(&strategy_reader, 1U << KERN_THERMAL_EVENT_CPU);
	kern_thermal_event(KERN_THERMAL_EVENT_STRATEGY, 0, 0);
	CHECK(!kern_thermal_reader_ready(&strategy_reader));
	kern_thermal_event(KERN_THERMAL_EVENT_CPU, 0xffffffffU, 5);
	CHECK(kern_thermal_reader_ready(&strategy_reader));
	kern_thermal_reader_detach(&strategy_reader);
	kern_thermal_reader_detach(&all_reader);
	kern_thermal_event(KERN_THERMAL_EVENT_CPU, 0, 0);
	CHECK(!all_reader.attached);
	CHECK(all_reader.count == 2U);
}

struct test_scenario {
	const char *name;
	void (*run)(void);
};

static const struct test_scenario test_scenarios[] = {
	{ "registration", scenario_registration },
	{ "strategy-checks", scenario_strategy_checks },
	{ "strategy-answers", scenario_strategy_answers },
	{ "strategy-pending", scenario_strategy_pending },
	{ "strategy-superseded", scenario_strategy_superseded },
	{ "strategy-late-custom", scenario_strategy_late_and_custom },
	{ "before-init", scenario_before_init },
	{ "sensors", scenario_sensors },
	{ "guard", scenario_guard },
	{ "fans", scenario_fans },
	{ "boost-pending", scenario_boost_pending },
	{ "meters", scenario_meters },
	{ "events", scenario_events },
};

int
main(int argc, char **argv)
{
	unsigned index;
	unsigned failed;
	pid_t child;
	int status;

	if (argc > 1 && strcmp(argv[1], "-v") == 0)
		test_verbose = true;
	failed = 0;
	for (index = 0; index < sizeof(test_scenarios) / sizeof(test_scenarios[0]); index++) {
		fflush(stdout);
		child = fork();
		if (child == 0) {
			clock_gettime(CLOCK_MONOTONIC, &test_start);
			test_scenarios[index].run();
			printf("%-22s %s (%u checks, %u failed)\n", test_scenarios[index].name,
			    test_failures == 0 ? "PASS" : "FAIL", test_checks, test_failures);
			fflush(stdout);
			_exit(test_failures == 0 ? 0 : 1);
		}
		waitpid(child, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			if (!WIFEXITED(status))
				printf("%-22s CRASH\n", test_scenarios[index].name);
			failed++;
		}
	}
	printf("thermal-host: %u scenarios, %u failed\n", index, failed);
	return failed == 0 ? 0 : 1;
}
