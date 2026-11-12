/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the CPU performance framework (src/kern/cpuperf.c).
 *
 * The kernel's services the framework uses are replaced by small models:
 * a cross-CPU notification runs the handler at once as the target CPU,
 * unless the test made that CPU deaf; the tick moves one on every read;
 * a sleep on a CPU's box runs out its deadline.  A mock provider stands
 * for a hybrid processor with ranges (the anchors of the Latitude 5330's
 * cores) or for a processor with a list of steps.  Each scenario runs in
 * its own process, since the framework takes one provider per boot.
 */

#include <kern/cpuperf.h>
#include <kern/thermal.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <kern/clock.h>
#include <kern/file.h>
#include <hal/hal.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/thermal.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_CPU_MAX	16U

/* The machine the models stand for. */
static unsigned test_cpu_count = 12U;
static unsigned test_current;
static bool test_deaf[TEST_CPU_MAX];
static uint64_t test_ticks;
static unsigned test_notifications;
static struct sched_cpu_time test_times[TEST_CPU_MAX];
static bool test_verbose;

/* What the thermal core would have seen. */
static const struct kern_thermal_receiver_ops *test_receiver;
static unsigned test_events;

/* What the mock provider saw and does. */
static struct kern_cpuperf_request test_request[TEST_CPU_MAX];
static unsigned test_apply_count[TEST_CPU_MAX];
static bool test_started[TEST_CPU_MAX];
static unsigned test_wrong_cpu;
static unsigned test_guaranteed_p = 21U;
static bool test_feedback_changed;
static unsigned test_measure_highest;

/*
 * The scheduler's moves of a measuring thread: the next test_move_starts
 * threads run on the CPU after the one they were placed on, and while
 * test_move_on_call is nonzero the measuring thread's call of that number
 * finds itself on that CPU (a move during its busy time).
 */
static unsigned test_move_starts;
static unsigned test_move_on_call;
static bool test_in_measure;
static unsigned test_measure_starts;
static int test_steps;
static int test_module_share;	/* the E-cores of a module share one clock */

/* The governor's thread, which the test runs one pass at a time. */
static struct thread *test_governor;
static jmp_buf test_governor_return;
static bool test_in_governor;

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
	}
}

/* The kernel services, as models. */

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)rank;
	(void)name;
	mutex->locked = 0;
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	if (mutex->locked != 0)
		printf("FAIL: mutex taken twice\n");
	mutex->locked = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	mutex->locked = 0;
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
	return queue->sequence;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock, uint64_t observed, uint64_t deadline, unsigned flags)
{
	(void)lock;
	(void)flags;
	if (test_in_governor && strcmp(queue->name, "cpuperf-governor") == 0)
		longjmp(test_governor_return, 1);
	if (queue->sequence != observed)
		return EAGAIN;
	if (deadline != 0 && test_ticks < deadline)
		test_ticks = deadline;
	return ETIMEDOUT;
}

uint64_t
sched_ticks(void)
{
	test_ticks++;
	return test_ticks;
}

uint64_t
kern_ms_to_ticks(uint64_t milliseconds)
{
	return milliseconds;
}

uint64_t
kern_ticks_to_ns(uint64_t ticks)
{
	return ticks * 1000000U;
}

unsigned
kern_cpu_count(void)
{
	return test_cpu_count;
}

unsigned
kern_cpu_current(void)
{
	if (test_in_measure && test_move_on_call != 0U) {
		test_move_on_call--;
		if (test_move_on_call == 0U)
			return (test_current + 1U) % test_cpu_count;
	}
	return test_current;
}

hal_cpu_id_t
hal_cpu_current(void)
{
	return test_current;
}

bool
hal_irq_disable(void)
{
	return false;
}

void
hal_irq_enable(void)
{
}

int
hal_cpu_notify(hal_cpu_id_t cpu)
{
	unsigned saved;

	test_notifications++;
	if (cpu >= test_cpu_count)
		return HAL_ERR_INVALID;
	if (test_deaf[cpu])
		return HAL_OK;
	saved = test_current;
	test_current = cpu;
	kern_cpuperf_notify(cpu);
	test_current = saved;
	return HAL_OK;
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

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

int
kthread_create(void (*entry)(void *), void *arg, int priority, struct thread **result)
{
	struct thread *thread;

	(void)priority;
	thread = calloc(1, sizeof(*thread));
	thread->entry = entry;
	thread->argument = arg;
	thread->bound_cpu = test_current;
	*result = thread;
	return 0;
}

int
thread_abort_new(struct thread *thread)
{
	free(thread);
	return 0;
}

int
sched_set_cpu(struct thread *thread, kern_cpu_id_t cpu)
{
	thread->bound_cpu = cpu;
	return 0;
}

/*
 * Starts a thread: the governor is kept for the test to drive, any other
 * thread (the measurement) runs to its end at once on its bound CPU.
 */
void
thread_start(struct thread *thread)
{
	unsigned saved;

	thread->started = 1;
	if (test_governor == NULL && thread->argument == NULL) {
		test_governor = thread;
		return;
	}
	saved = test_current;
	test_current = thread->bound_cpu;
	test_measure_starts++;
	if (test_move_starts != 0U) {
		test_move_starts--;
		test_current = (thread->bound_cpu + 1U) % test_cpu_count;
	}
	test_in_measure = true;
	thread->entry(thread->argument);
	test_in_measure = false;
	test_current = saved;
	free(thread);
}

int
sched_cpu_time(kern_cpu_id_t cpu, struct sched_cpu_time *time)
{
	*time = test_times[cpu];
	return 0;
}

/* The thermal core, as far as the framework reaches it. */

int
kern_thermal_receiver_register(const char *name, const struct kern_thermal_receiver_ops *ops, void *context)
{
	(void)context;
	if (strcmp(name, "cpu") == 0)
		test_receiver = ops;
	return 0;
}

void
kern_thermal_event(uint32_t type, uint32_t index, int64_t value)
{
	(void)index;
	(void)value;
	if (type == KERN_THERMAL_EVENT_CPU)
		test_events++;
}

/* The CPU requests of /dev/thermal: the handler the framework registers, the caller and the copies. */

static kern_thermal_ioctl_t test_ioctl;
static unsigned test_ioctl_first;
static unsigned test_ioctl_last;
static struct file test_file = { O_RDWR };

int
kern_thermal_ioctl_range_register(unsigned first, unsigned last, kern_thermal_ioctl_t handler)
{
	test_ioctl_first = first;
	test_ioctl_last = last;
	test_ioctl = handler;
	return 0;
}

int
file_status_flags_get(const struct file *file)
{
	return file->f_flags;
}

int
copyin(uintptr_t source, void *destination, size_t size)
{
	if (source == 0U)
		return EFAULT;
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	if (destination == 0U)
		return EFAULT;
	memcpy((void *)destination, source, size);
	return 0;
}

static int
test_request_cpu(unsigned long request, void *argument)
{
	return test_ioctl(&test_file, request, (uintptr_t)argument);
}

/* The mock provider. */

static bool
mock_performance_core(kern_cpu_id_t cpu)
{
	return cpu < 4U;
}

static int
mock_topology(kern_cpu_id_t cpu, struct kern_cpuperf_topology *topology)
{
	if (test_current != cpu)
		test_wrong_cpu++;
	memset(topology, 0, sizeof(*topology));
	topology->firmware_id = KERN_CPUPERF_UNKNOWN;
	if (test_steps) {
		topology->core = cpu;
		topology->module = cpu;
		topology->core_class = KERN_CPUPERF_CORE_PERFORMANCE;
		return 0;
	}
	if (mock_performance_core(cpu)) {
		topology->core = cpu / 2U;
		topology->thread = cpu % 2U;
		topology->module = cpu / 2U;
		topology->core_class = KERN_CPUPERF_CORE_PERFORMANCE;
		topology->core_type = 0x40U;
	} else {
		topology->core = 2U + (cpu - 4U);
		topology->module = 2U + (cpu - 4U) / 4U;
		topology->core_class = KERN_CPUPERF_CORE_EFFICIENCY;
		topology->core_type = 0x20U;
	}
	return 0;
}

static int
mock_describe(kern_cpu_id_t cpu, struct kern_cpuperf_levels *levels)
{
	unsigned index;

	levels->firmware_id = 100U + cpu;
	if (test_steps) {
		levels->kind = KERN_CPUPERF_LEVELS_STEPS;
		levels->share = KERN_CPUPERF_SHARE_PACKAGE;
		levels->step_count = 10U;
		for (index = 0; index < 10U; index++)
			levels->steps[index] = 8U + 4U * index;
		levels->guaranteed = 36U;
		levels->efficient = 8U;
		return 0;
	}
	/* A RANGE CPU hides its anchors until its control starts. */
	levels->kind = KERN_CPUPERF_LEVELS_RANGE;
	levels->share = KERN_CPUPERF_SHARE_CORE;
	if (test_module_share && !mock_performance_core(cpu))
		levels->share = KERN_CPUPERF_SHARE_MODULE;
	return 0;
}

static void
mock_capabilities(kern_cpu_id_t cpu, struct kern_cpuperf_sample *sample)
{
	sample->flags |= KERN_CPUPERF_SAMPLE_CAPABILITIES;
	if (mock_performance_core(cpu)) {
		sample->lowest = 1U;
		sample->efficient = 20U;
		sample->guaranteed = test_guaranteed_p;
		sample->highest = 56U;
	} else {
		sample->lowest = 1U;
		sample->efficient = 15U;
		sample->guaranteed = 12U;
		sample->highest = 33U;
	}
}

static int
mock_start(kern_cpu_id_t cpu, struct kern_cpuperf_sample *sample)
{
	if (test_current != cpu)
		test_wrong_cpu++;
	test_started[cpu] = true;
	mock_capabilities(cpu, sample);
	return 0;
}

static void
mock_sample(kern_cpu_id_t cpu, struct kern_cpuperf_sample *sample)
{
	if (test_current != cpu)
		test_wrong_cpu++;
	sample->delivered_mhz = 1000U + cpu;
	if (!test_steps && test_started[cpu])
		mock_capabilities(cpu, sample);
}

static int
mock_apply(kern_cpu_id_t cpu, const struct kern_cpuperf_request *request)
{
	if (test_current != cpu)
		test_wrong_cpu++;
	test_request[cpu] = *request;
	test_apply_count[cpu]++;
	if (request->minimum == 56U && request->maximum == 56U)
		test_measure_highest++;
	return 0;
}

static int
mock_measure(kern_cpu_id_t cpu, bool begin, uint32_t *mhz)
{
	if (test_current != cpu)
		test_wrong_cpu++;
	if (begin)
		return 0;
	*mhz = 3292U;
	if (mock_performance_core(cpu))
		*mhz = 4388U;
	return 0;
}

static int
mock_relative(kern_cpu_id_t cpu, uint32_t source, uint32_t *value)
{
	if (source != KERN_CPUPERF_RELATIVE_FEEDBACK)
		return EOPNOTSUPP;
	*value = 180U;
	if (mock_performance_core(cpu))
		*value = 255U;
	return 0;
}

static int
mock_feedback(kern_cpu_id_t cpu, bool *changed)
{
	if (test_current != cpu)
		test_wrong_cpu++;
	*changed = test_feedback_changed;
	return 0;
}

static int
mock_feedback_read(kern_cpu_id_t cpu, uint32_t *available, uint32_t *efficiency)
{
	*available = 180U;
	*efficiency = 255U;
	if (mock_performance_core(cpu)) {
		*available = 255U;
		*efficiency = 100U;
	}
	return 0;
}

static int
mock_idle(kern_cpu_id_t cpu, struct kern_cpuperf_idle *idle)
{
	if (test_current != cpu)
		test_wrong_cpu++;
	memset(idle, 0, sizeof(*idle));
	idle->count = 1U;
	idle->available = 0x4cU;
	idle->state[0] = 6U;
	idle->residency_ns[0] = 5000U + cpu;
	return 0;
}

static struct kern_cpuperf_provider_ops mock_ops = {
	mock_topology,
	mock_describe,
	mock_start,
	mock_apply,
	mock_sample,
	mock_measure,
	KERN_CPUPERF_RELATIVE_FEEDBACK,
	mock_relative,
	mock_feedback,
	mock_feedback_read,
	mock_idle,
	mock_idle
};

/* Helpers of the scenarios. */

static int
test_strategy(uint32_t value, uint8_t speed, uint8_t noise, uint32_t *flags)
{
	struct kern_thermal_strategy strategy;

	memset(&strategy, 0, sizeof(strategy));
	strategy.value = value;
	strategy.speed = speed;
	strategy.noise = noise;
	*flags = 0U;
	return test_receiver->apply(NULL, &strategy, flags);
}

static void
test_expect(unsigned cpu, uint32_t minimum, uint32_t maximum, uint32_t preference, int line)
{
	struct kern_cpuperf_request *request;

	request = &test_request[cpu];
	test_checks++;
	if (request->minimum != minimum ||
	    request->maximum != maximum ||
	    request->preference != preference) {
		test_failures++;
		printf("FAIL line %d: cpu %u got min %u max %u pref %u, expected %u %u %u\n",
		       line, cpu, request->minimum, request->maximum, request->preference,
		       minimum, maximum, preference);
	}
}

#define EXPECT(cpu, minimum, maximum, preference) test_expect((cpu), (minimum), (maximum), (preference), __LINE__)

static void
test_governor_pass(void)
{
	test_in_governor = true;
	if (setjmp(test_governor_return) == 0)
		test_governor->entry(test_governor->argument);
	test_in_governor = false;
}

static void
test_busy(unsigned cpu, unsigned busy, unsigned idle)
{
	test_times[cpu].user += busy;
	test_times[cpu].idle += idle;
}

/* The scenarios. */

static void
scenario_no_provider(void)
{
	struct kern_cpuperf_cpu info;
	struct kern_cpuperf_domain domain;
	uint32_t flags;
	int error;

	kern_cpuperf_init();
	CHECK(test_receiver != NULL);
	CHECK(kern_cpuperf_domain_count() == 12U);

	/* Every CPU is its own core of package 0, unknown kind, nothing to control. */
	error = kern_cpuperf_cpu(5, 0, &info);
	CHECK(error == 0);
	CHECK(info.topology.core == 5U);
	CHECK(info.topology.module == 5U);
	CHECK(info.topology.package == 0U);
	CHECK(info.topology.core_class == KERN_CPUPERF_CORE_UNKNOWN);
	CHECK(info.relative == KERN_CPUPERF_UNKNOWN);
	CHECK((info.flags & KERN_CPUPERF_CPU_HAS_CONTROL) == 0U);
	CHECK(kern_cpuperf_cpu(12, 0, &info) == EINVAL);

	error = kern_cpuperf_domain(3, &domain);
	CHECK(error == 0);
	CHECK(domain.cpu_count == 1U);
	CHECK(domain.cpus[0] == (1U << 3));

	/* The strategy and the ceiling have nothing to act on. */
	error = test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags);
	CHECK(error == EOPNOTSUPP);
	CHECK(test_receiver->limit(NULL, KERN_THERMAL_DOMAIN_ALL, 500U) == EOPNOTSUPP);
	CHECK(test_receiver->is_custom(NULL) == false);

	/* A provider without describe is refused; a second one too. */
	{
		struct kern_cpuperf_provider_ops broken;

		broken = mock_ops;
		broken.describe = NULL;
		CHECK(kern_cpuperf_provider_register(&broken) == EINVAL);
	}
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);
	CHECK(kern_cpuperf_provider_register(&mock_ops) == EBUSY);
	CHECK(test_notifications == 11U);
}

static void
scenario_describe_only(void)
{
	struct kern_cpuperf_provider_ops ops;
	struct kern_cpuperf_cpu info;
	uint32_t flags;

	/* A provider that only describes the machine, as without the MSR path. */
	ops = mock_ops;
	ops.apply = NULL;
	ops.start = NULL;
	ops.measure = NULL;
	kern_cpuperf_init();
	CHECK(kern_cpuperf_provider_register(&ops) == 0);
	CHECK(kern_cpuperf_cpu(6, 0, &info) == 0);
	CHECK(info.topology.core_class == KERN_CPUPERF_CORE_EFFICIENCY);
	CHECK(info.topology.module == 2U);
	CHECK(info.topology.firmware_id == 106U);
	CHECK((info.flags & KERN_CPUPERF_CPU_HAS_CONTROL) == 0U);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == EOPNOTSUPP);
	CHECK(test_wrong_cpu == 0U);
}

static void
scenario_range(void)
{
	struct kern_cpuperf_cpu info;
	struct kern_cpuperf_domain domain;
	struct kern_cpuperf_setting setting;
	struct kern_cpuperf_idle idle;
	uint32_t available;
	uint32_t source;
	uint32_t generation;
	uint32_t flags;
	unsigned notifications;
	unsigned cpu;
	int error;

	kern_cpuperf_init();
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);

	/* Topology and domains: the SMT pairs share, each E-core is its own. */
	CHECK(kern_cpuperf_domain_count() == 10U);
	CHECK(kern_cpuperf_domain(0, &domain) == 0);
	CHECK(domain.cpus[0] == 0x3U);
	CHECK(kern_cpuperf_domain(1, &domain) == 0);
	CHECK(domain.cpus[0] == 0xcU);
	CHECK(kern_cpuperf_domain(9, &domain) == 0);
	CHECK(domain.cpus[0] == (1U << 11));
	CHECK(kern_cpuperf_domain(10, &domain) == EINVAL);
	CHECK(kern_cpuperf_cpu(3, 0, &info) == 0);
	CHECK(info.domain == 1U);
	CHECK(info.topology.thread == 1U);
	CHECK(info.lowest == KERN_CPUPERF_UNKNOWN);
	CHECK((info.flags & KERN_CPUPERF_CPU_HAS_CONTROL) != 0U);
	CHECK(info.relative == KERN_CPUPERF_UNKNOWN);

	/* Nothing is written before the first strategy. */
	for (cpu = 0; cpu < 12U; cpu++)
		CHECK(test_apply_count[cpu] == 0U && !test_started[cpu]);

	/* The first strategy starts the control, then the rates are measured. */
	generation = kern_cpuperf_generation();
	error = test_strategy(KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0, &flags);
	CHECK(error == 0);
	CHECK(flags == 0U);
	CHECK(test_started[0] && test_started[11]);
	EXPECT(0, 21, 56, 0);
	EXPECT(1, 21, 56, 0);
	EXPECT(4, 12, 33, 0);
	CHECK(test_measure_highest >= 2U);
	CHECK(kern_cpuperf_generation() != generation);
	CHECK(kern_cpuperf_cpu(0, 0, &info) == 0);
	CHECK(info.relative == 100U);
	CHECK(info.highest_mhz == 4388U);
	CHECK(info.guaranteed == 21U);
	CHECK(kern_cpuperf_cpu(7, 0, &info) == 0);
	CHECK(info.relative == 75U);
	CHECK(info.highest_mhz == 3292U);

	/* The strategies of ranges. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	EXPECT(0, 1, 56, 128);
	EXPECT(5, 1, 33, 128);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_POWERSAVE, 0, 0, &flags) == 0);
	EXPECT(0, 1, 21, 192);
	EXPECT(5, 1, 15, 192);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_MANUAL, 50, 100, &flags) == 0);
	EXPECT(0, 1, 38, 127);
	EXPECT(5, 1, 24, 127);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_MANUAL, 100, 0, &flags) == 0);
	EXPECT(0, 1, 21, 0);
	EXPECT(5, 1, 15, 0);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_MANUAL, 0, 0, &flags) == 0);
	EXPECT(0, 1, 20, 255);
	EXPECT(5, 1, 15, 255);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_MANUAL, 101, 0, &flags) == EINVAL);
	CHECK(test_strategy(9, 0, 0, &flags) == EINVAL);

	/* The guaranteed level read at apply time sets PERFORMANCE's minimum. */
	test_guaranteed_p = 18U;
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0, &flags) == 0);
	EXPECT(0, 18, 56, 0);
	test_guaranteed_p = 21U;

	/* The thermal ceiling, never below guaranteed. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	CHECK(test_receiver->limit(NULL, KERN_THERMAL_DOMAIN_ALL, 500U) == 0);
	EXPECT(0, 1, 28, 128);
	EXPECT(5, 1, 17, 128);
	CHECK(kern_cpuperf_cpu(0, 0, &info) == 0);
	CHECK((info.flags & KERN_CPUPERF_CPU_LIMITED) != 0U);
	CHECK(kern_cpuperf_domain(0, &domain) == 0);
	CHECK(domain.limit == 28U);
	CHECK(domain.request.maximum == 56U);
	CHECK((domain.flags & KERN_CPUPERF_DOMAIN_LIMITED) != 0U);
	CHECK(test_receiver->limit(NULL, KERN_THERMAL_DOMAIN_ALL, 0U) == 0);
	EXPECT(0, 1, 21, 128);
	EXPECT(5, 1, 12, 128);
	CHECK(test_receiver->limit(NULL, 4U, 1000U) == 0);
	EXPECT(6, 1, 33, 128);
	EXPECT(5, 1, 12, 128);
	CHECK(test_receiver->limit(NULL, 10U, 500U) == EINVAL);
	CHECK(test_receiver->limit(NULL, KERN_THERMAL_DOMAIN_ALL, 1001U) == EINVAL);
	CHECK(test_receiver->limit(NULL, KERN_THERMAL_DOMAIN_ALL, 1000U) == 0);
	EXPECT(5, 1, 33, 128);

	/* A root's direct setting of one domain, then the next strategy ends it. */
	memset(&setting, 0, sizeof(setting));
	setting.fields = KERN_CPUPERF_SET_MAXIMUM;
	setting.maximum = 30U;
	CHECK(kern_cpuperf_domain_set(0, &setting) == 0);
	EXPECT(0, 1, 30, 128);
	EXPECT(1, 1, 30, 128);
	EXPECT(2, 1, 56, 128);
	CHECK(test_receiver->is_custom(NULL) == true);
	CHECK(kern_cpuperf_cpu(0, 0, &info) == 0);
	CHECK((info.flags & KERN_CPUPERF_CPU_CUSTOM) != 0U);
	setting.maximum = 57U;
	CHECK(kern_cpuperf_domain_set(0, &setting) == EINVAL);
	setting.fields = KERN_CPUPERF_SET_MINIMUM | KERN_CPUPERF_SET_MAXIMUM;
	setting.minimum = 40U;
	setting.maximum = 30U;
	CHECK(kern_cpuperf_domain_set(0, &setting) == EINVAL);
	setting.fields = 0x10U;
	CHECK(kern_cpuperf_domain_set(0, &setting) == EINVAL);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0, &flags) == 0);
	CHECK(test_receiver->is_custom(NULL) == false);
	EXPECT(0, 21, 56, 0);

	/* A direct setting of every domain is permille of each range. */
	memset(&setting, 0, sizeof(setting));
	setting.fields = KERN_CPUPERF_SET_MAXIMUM | KERN_CPUPERF_SET_PREFERENCE;
	setting.maximum = 500U;
	setting.preference = 64U;
	CHECK(kern_cpuperf_domain_set(KERN_CPUPERF_ALL, &setting) == 0);
	EXPECT(0, 21, 28, 64);
	EXPECT(5, 12, 17, 64);
	setting.maximum = 1001U;
	CHECK(kern_cpuperf_domain_set(KERN_CPUPERF_ALL, &setting) == EINVAL);

	/* A CPU that does not answer: the strategy is unconfirmed, not failed. */
	test_deaf[5] = true;
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	CHECK(flags == KERN_THERMAL_APPLY_UNCONFIRMED);
	CHECK(kern_cpuperf_domain(3, &domain) == 0);
	CHECK((domain.flags & KERN_CPUPERF_DOMAIN_PENDING) != 0U);
	EXPECT(5, 12, 17, 64);
	test_deaf[5] = false;
	test_current = 5U;
	kern_cpuperf_notify(5);
	test_current = 0U;
	CHECK(kern_cpuperf_domain(3, &domain) == 0);
	CHECK((domain.flags & KERN_CPUPERF_DOMAIN_PENDING) == 0U);

	/* A notification with nothing posted touches nothing. */
	kern_cpuperf_notify(7);

	/* Relative performance by the hardware feedback. */
	kern_cpuperf_relative_get(&available, &source);
	CHECK(available == (KERN_CPUPERF_RELATIVE_FREQUENCY | KERN_CPUPERF_RELATIVE_FEEDBACK));
	CHECK(source == KERN_CPUPERF_RELATIVE_FREQUENCY);
	CHECK(kern_cpuperf_relative_select(KERN_CPUPERF_RELATIVE_PROVIDER) == EOPNOTSUPP);
	CHECK(kern_cpuperf_relative_select(0x8U) == EINVAL);
	CHECK(kern_cpuperf_relative_select(KERN_CPUPERF_RELATIVE_FEEDBACK) == 0);
	CHECK(kern_cpuperf_cpu(9, 0, &info) == 0);
	CHECK(info.relative == 71U);

	/* The feedback copy comes with the poll and wakes no other CPU. */
	notifications = test_notifications;
	test_current = 2U;
	test_feedback_changed = false;
	generation = kern_cpuperf_generation();
	test_receiver->poll(NULL);
	CHECK(kern_cpuperf_generation() == generation);
	CHECK(kern_cpuperf_cpu(4, 0, &info) == 0);
	CHECK(info.available == KERN_CPUPERF_UNKNOWN);
	test_feedback_changed = true;
	test_receiver->poll(NULL);
	CHECK(kern_cpuperf_generation() != generation);
	CHECK(kern_cpuperf_cpu(4, 0, &info) == 0);
	CHECK(info.available == 180U);
	CHECK(info.efficiency == 255U);
	CHECK(kern_cpuperf_cpu(1, 0, &info) == 0);
	CHECK(info.available == 255U);
	CHECK(test_notifications == notifications);

	/* The idle counters, on the CPU and on the package. */
	CHECK(kern_cpuperf_idle_cpu(3, &idle) == 0);
	CHECK(idle.residency_ns[0] == 5003U);
	CHECK(idle.time_ns != 0U);
	notifications = test_notifications;
	CHECK(kern_cpuperf_idle_package(0, &idle) == 0);
	CHECK(idle.residency_ns[0] == 5002U);
	CHECK(test_notifications == notifications);
	CHECK(kern_cpuperf_idle_package(1, &idle) == EINVAL);

	/* The delivered rate, read on the CPU. */
	CHECK(kern_cpuperf_cpu(3, KERN_CPUPERF_READ_SAMPLE, &info) == 0);
	CHECK(info.delivered_mhz == 1003U);
	test_deaf[3] = true;
	CHECK(kern_cpuperf_cpu(3, KERN_CPUPERF_READ_SAMPLE, &info) == ETIMEDOUT);
	test_deaf[3] = false;
	test_current = 0U;

	/* Every hardware operation ran on its own CPU. */
	CHECK(test_wrong_cpu == 0U);
	CHECK(test_events != 0U);
}

static void
scenario_steps(void)
{
	struct kern_cpuperf_domain domain;
	struct kern_cpuperf_setting setting;
	uint32_t flags;
	unsigned cpu;

	test_steps = 1;
	test_cpu_count = 4U;
	mock_ops.start = NULL;
	mock_ops.measure = NULL;
	kern_cpuperf_init();
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);
	CHECK(kern_cpuperf_domain_count() == 1U);
	CHECK(kern_cpuperf_domain(0, &domain) == 0);
	CHECK(domain.cpus[0] == 0xfU);

	/* PERFORMANCE: always the top step, no governor. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0, &flags) == 0);
	for (cpu = 0; cpu < 4U; cpu++)
		EXPECT(cpu, 44, 44, KERN_CPUPERF_UNKNOWN);
	CHECK(test_request[0].desired == 44U);
	CHECK(test_governor == NULL);

	/* BALANCED: every step, the governor moves by load. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	EXPECT(0, 8, 44, KERN_CPUPERF_UNKNOWN);
	CHECK(test_request[0].desired == 44U);
	CHECK(test_governor != NULL);
	for (cpu = 0; cpu < 4U; cpu++)
		test_busy(cpu, 1, 9);
	test_governor_pass();
	CHECK(test_request[0].desired == 40U);
	CHECK(test_request[3].desired == 40U);
	test_busy(2, 9, 1);
	test_governor_pass();
	CHECK(test_request[0].desired == 44U);
	test_governor_pass();
	CHECK(test_request[0].desired == 40U);

	/* POWERSAVE: up to the highest sustained step. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_POWERSAVE, 0, 0, &flags) == 0);
	EXPECT(0, 8, 36, KERN_CPUPERF_UNKNOWN);
	CHECK(test_request[0].desired == 36U);

	/* MANUAL: noise allows the turbo steps, speed how high. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_MANUAL, 50, 0, &flags) == 0);
	EXPECT(0, 8, 20, KERN_CPUPERF_UNKNOWN);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_MANUAL, 100, 60, &flags) == 0);
	EXPECT(0, 8, 44, KERN_CPUPERF_UNKNOWN);

	/* The ceiling of a list stops at the sustained step. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	CHECK(test_receiver->limit(NULL, KERN_THERMAL_DOMAIN_ALL, 0U) == 0);
	EXPECT(0, 8, 36, KERN_CPUPERF_UNKNOWN);
	CHECK(test_receiver->limit(NULL, KERN_THERMAL_DOMAIN_ALL, 1000U) == 0);

	/* A root's desired step stops the governor and lands on a step. */
	memset(&setting, 0, sizeof(setting));
	setting.fields = KERN_CPUPERF_SET_DESIRED;
	setting.desired = 23U;
	CHECK(kern_cpuperf_domain_set(0, &setting) == 0);
	CHECK(test_request[0].desired == 20U);
	for (cpu = 0; cpu < 4U; cpu++)
		test_busy(cpu, 10, 0);
	test_governor_pass();
	CHECK(test_request[0].desired == 20U);
	CHECK(test_wrong_cpu == 0U);
}

/* The CPU requests of /dev/thermal on the hybrid machine. */
static void
scenario_ioctl(void)
{
	struct thermal_cpu_relative relative;
	struct thermal_cpu_setting setting;
	struct thermal_cpu_domain domain;
	struct thermal_cpu_info cpu;
	struct thermal_cpu_idle idle;
	uint32_t flags;

	kern_cpuperf_init();
	CHECK(test_ioctl != NULL && test_ioctl_first == 32U && test_ioctl_last == 63U);
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_PERFORMANCE, 0, 0, &flags) == 0);
	CHECK(kern_cpuperf_package_count() == 1U);

	/* Another size of a known number, and a number nobody has. */
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU + (1UL << 16), &cpu) == ENOTTY);
	CHECK(test_request_cpu(_IOR('T', 39, struct thermal_cpu_idle), &idle) == ENOTTY);

	/* A CPU: topology, class, anchors and the measured rate; any open reads it. */
	test_file.f_flags = O_RDONLY;
	memset(&cpu, 0, sizeof(cpu));
	cpu.cpu = 3;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU, &cpu) == 0);
	CHECK(cpu.cpu == 3U && cpu.count == 12U && cpu.thread == 1U && cpu.core == 1U && cpu.domain == 1U);
	CHECK(cpu.core_class == KERN_THERMAL_CPU_CORE_PERFORMANCE && cpu.core_type == 0x40U);
	CHECK(cpu.highest == 56U && cpu.guaranteed == 21U && cpu.highest_mhz == 4388U && cpu.relative == 100U);
	CHECK((cpu.flags & KERN_THERMAL_CPU_HAS_CONTROL) != 0U);
	CHECK((cpu.flags & KERN_THERMAL_CPU_SAMPLE_TIMEOUT) == 0U);
	cpu.cpu = 7;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU, &cpu) == 0);
	CHECK(cpu.core_class == KERN_THERMAL_CPU_CORE_EFFICIENCY && cpu.relative == 75U);

	/* A sample needs an open for writing; past the last CPU is ENOENT; an unknown request bit is EINVAL. */
	cpu.cpu = 2;
	cpu.request = KERN_THERMAL_CPU_READ_SAMPLE;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU, &cpu) == EPERM);
	test_file.f_flags = O_RDWR;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU, &cpu) == 0);
	CHECK(cpu.delivered_mhz == 1002U && cpu.request == KERN_THERMAL_CPU_READ_SAMPLE);
	cpu.request = 2;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU, &cpu) == EINVAL);
	cpu.request = 0;
	cpu.cpu = 12;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU, &cpu) == ENOENT);

	/* A CPU that does not answer the sample: 0 and SAMPLE_TIMEOUT with the rest filled. */
	test_deaf[5] = true;
	cpu.cpu = 5;
	cpu.request = KERN_THERMAL_CPU_READ_SAMPLE;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU, &cpu) == 0);
	CHECK((cpu.flags & KERN_THERMAL_CPU_SAMPLE_TIMEOUT) != 0U && cpu.core_class == KERN_THERMAL_CPU_CORE_EFFICIENCY);
	test_deaf[5] = false;

	/* A domain: members, a RANGE as its ends, the request; past the last is ENOENT. */
	memset(&domain, 0, sizeof(domain));
	domain.domain = 1;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_DOMAIN, &domain) == 0);
	CHECK(domain.count == 10U && domain.cpu_count == 2U && domain.cpus[0] == 0xcU);
	CHECK(domain.kind == KERN_THERMAL_CPU_LEVELS_RANGE && domain.share == KERN_THERMAL_CPU_SHARE_CORE);
	CHECK(domain.step_count == 2U && domain.steps[0] == 1U && domain.steps[1] == 56U);
	CHECK(domain.minimum == 21U && domain.maximum == 56U && domain.limit == KERN_THERMAL_UNKNOWN);
	domain.domain = 10;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_DOMAIN, &domain) == ENOENT);

	/* The direct setting: an open for writing's, known fields only, a domain or ALL; CUSTOM follows. */
	memset(&setting, 0, sizeof(setting));
	setting.domain = 0;
	setting.fields = KERN_THERMAL_CPU_SET_MAXIMUM;
	setting.maximum = 40;
	test_file.f_flags = O_RDONLY;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_DOMAIN, &setting) == EPERM);
	test_file.f_flags = O_RDWR;
	setting.fields = 0x10;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_DOMAIN, &setting) == EINVAL);
	setting.fields = KERN_THERMAL_CPU_SET_MAXIMUM;
	setting.reserved[1] = 1;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_DOMAIN, &setting) == EINVAL);
	setting.reserved[1] = 0;
	setting.domain = 10;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_DOMAIN, &setting) == ENOENT);
	setting.domain = 0;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_DOMAIN, &setting) == 0);
	CHECK(test_request[0].maximum == 40U);
	domain.domain = 0;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_DOMAIN, &domain) == 0);
	CHECK((domain.flags & KERN_THERMAL_CPU_DOMAIN_CUSTOM) != 0U && domain.maximum == 40U);
	setting.domain = KERN_THERMAL_ALL;
	setting.maximum = 500;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_DOMAIN, &setting) == 0);

	/* The relative ways: read on any open, chosen on an open for writing. */
	memset(&relative, 0, sizeof(relative));
	test_file.f_flags = O_RDONLY;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_RELATIVE, &relative) == 0);
	CHECK(relative.available == (KERN_THERMAL_CPU_RELATIVE_FREQUENCY | KERN_THERMAL_CPU_RELATIVE_FEEDBACK));
	relative.source = KERN_THERMAL_CPU_RELATIVE_FEEDBACK;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_RELATIVE, &relative) == EPERM);
	test_file.f_flags = O_RDWR;
	relative.source = KERN_THERMAL_CPU_RELATIVE_PROVIDER;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_RELATIVE, &relative) == EOPNOTSUPP);
	relative.source = KERN_THERMAL_CPU_RELATIVE_FEEDBACK;
	CHECK(test_request_cpu(KERN_THERMAL_SET_CPU_RELATIVE, &relative) == 0);
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_RELATIVE, &relative) == 0);
	CHECK(relative.source == KERN_THERMAL_CPU_RELATIVE_FEEDBACK);

	/* The idle residency: a core needs an open for writing, a package any open; numbers nobody has are ENOENT. */
	memset(&idle, 0, sizeof(idle));
	idle.target = 4;
	test_file.f_flags = O_RDONLY;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_IDLE, &idle) == EPERM);
	test_file.f_flags = O_RDWR;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_IDLE, &idle) == 0);
	CHECK(idle.target == 4U && idle.count == 12U && idle.state_count == 1U && idle.available == 0x4cU);
	CHECK(idle.state[0] == 6U && idle.residency_ns[0] == 5004U && idle.scope[0] == KERN_THERMAL_CPU_IDLE_CORE);
	idle.target = 12;
	CHECK(test_request_cpu(KERN_THERMAL_GET_CPU_IDLE, &idle) == ENOENT);
	test_file.f_flags = O_RDONLY;
	idle.target = 0;
	CHECK(test_request_cpu(KERN_THERMAL_GET_PACKAGE_IDLE, &idle) == 0);
	CHECK(idle.state_count == 1U && idle.scope[0] == KERN_THERMAL_CPU_IDLE_PACKAGE);
	idle.target = 1;
	CHECK(test_request_cpu(KERN_THERMAL_GET_PACKAGE_IDLE, &idle) == ENOENT);
	CHECK(test_wrong_cpu == 0U);
}

/*
 * The measurement of the highest rates when the scheduler moves the
 * measuring thread: a thread that starts on another CPU, or moves during
 * its busy time, is replaced; one that never stays leaves the rate
 * unknown and the strategy in force.
 */
static void
scenario_measure_moved(void)
{
	struct kern_cpuperf_cpu info;
	uint32_t flags;

	/* The P-core's first two threads start elsewhere: the third measures it. */
	kern_cpuperf_init();
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);
	test_move_starts = 2U;
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	CHECK(test_move_starts == 0U);
	CHECK(test_measure_starts == 4U);
	CHECK(test_wrong_cpu == 0U);
	CHECK(kern_cpuperf_cpu(0, 0, &info) == 0);
	CHECK(info.highest_mhz == 4388U);
	CHECK(kern_cpuperf_cpu(4, 0, &info) == 0);
	CHECK(info.highest_mhz == 3292U);
	CHECK(info.relative == 75U);
}

/* A measuring thread that moves during its busy time is not believed. */
static void
scenario_measure_moved_busy(void)
{
	struct kern_cpuperf_cpu info;
	uint32_t flags;

	/* The first thread finds itself elsewhere at its second look (after the busy time). */
	kern_cpuperf_init();
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);
	test_move_on_call = 2U;
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	CHECK(test_measure_starts == 3U);
	CHECK(kern_cpuperf_cpu(1, 0, &info) == 0);
	CHECK(info.highest_mhz == 4388U);
}

/* Threads that never stay on the CPU: the rates stay unknown, the strategy holds. */
static void
scenario_measure_never(void)
{
	struct kern_cpuperf_cpu info;
	uint32_t flags;

	/* Every thread starts elsewhere. */
	kern_cpuperf_init();
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);
	test_move_starts = 1000U;
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	CHECK(test_measure_starts == 6U);
	CHECK(kern_cpuperf_cpu(0, 0, &info) == 0);
	CHECK(info.highest_mhz == 0U);
	CHECK(kern_cpuperf_cpu(11, 0, &info) == 0);
	CHECK(info.highest_mhz == 0U);
	EXPECT(0, 1, 56, 128);
	EXPECT(4, 1, 33, 128);
}

/* E-cores described as sharing their module's clock: one domain per module, every member written. */
static void
scenario_module_share(void)
{
	struct kern_cpuperf_domain domain;
	struct kern_cpuperf_setting setting;
	struct kern_cpuperf_cpu info;
	uint32_t flags;
	unsigned cpu;

	test_module_share = 1;
	kern_cpuperf_init();
	CHECK(kern_cpuperf_provider_register(&mock_ops) == 0);

	/* The two P-cores' thread pairs and the two E-core modules. */
	CHECK(kern_cpuperf_domain_count() == 4U);
	CHECK(kern_cpuperf_domain(0, &domain) == 0);
	CHECK(domain.cpus[0] == 0x3U);
	CHECK(kern_cpuperf_domain(1, &domain) == 0);
	CHECK(domain.cpus[0] == 0xcU);
	CHECK(kern_cpuperf_domain(2, &domain) == 0);
	CHECK(domain.cpus[0] == 0xf0U);
	CHECK(kern_cpuperf_domain(3, &domain) == 0);
	CHECK(domain.cpus[0] == 0xf00U);
	CHECK(kern_cpuperf_domain(4, &domain) == EINVAL);
	CHECK(kern_cpuperf_cpu(9, 0, &info) == 0);
	CHECK(info.domain == 3U);

	/* A strategy reaches every E-core. */
	CHECK(test_strategy(KERN_THERMAL_STRATEGY_BALANCED, 0, 0, &flags) == 0);
	CHECK(flags == 0U);
	for (cpu = 4; cpu < 12U; cpu++)
		EXPECT(cpu, 1, 33, 128);

	/* A direct setting of one module writes each of its four cores and no other. */
	memset(&setting, 0, sizeof(setting));
	setting.fields = KERN_CPUPERF_SET_MAXIMUM;
	setting.maximum = 12U;
	CHECK(kern_cpuperf_domain_set(3, &setting) == 0);
	for (cpu = 8; cpu < 12U; cpu++)
		EXPECT(cpu, 1, 12, 128);
	for (cpu = 4; cpu < 8U; cpu++)
		EXPECT(cpu, 1, 33, 128);
	EXPECT(0, 1, 56, 128);
	CHECK(test_wrong_cpu == 0U);
}

static void
run(const char *name, void (*scenario)(void))
{
	pid_t child;
	int status;

	fflush(stdout);
	child = fork();
	if (child == 0) {
		scenario();
		printf("%s: %u checks, %u failures\n", name, test_checks, test_failures);
		exit(test_failures == 0 ? 0 : 1);
	}
	waitpid(child, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		printf("%s: FAIL\n", name);
		test_failures++;
	}
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "-v") == 0)
		test_verbose = true;
	run("no-provider", scenario_no_provider);
	run("describe-only", scenario_describe_only);
	run("range", scenario_range);
	run("steps", scenario_steps);
	run("ioctl", scenario_ioctl);
	run("measure-moved", scenario_measure_moved);
	run("measure-moved-busy", scenario_measure_moved_busy);
	run("measure-never", scenario_measure_never);
	run("module-share", scenario_module_share);
	if (test_failures != 0) {
		printf("cpuperf host test: FAIL\n");
		return 1;
	}
	printf("cpuperf host test: PASS\n");
	return 0;
}
