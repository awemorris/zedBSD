/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CPU performance framework.
 *
 * Each CPU has a box: the latest intent for it (the strategy, a
 * direct setting, the thermal ceiling and the step the governor chose),
 * the work waiting for it, and what it last reported.  A thread that
 * wants something done on a CPU puts the work in that CPU's box and
 * notifies the CPU; the CPU's notification handler finds the work,
 * samples the CPU, turns the intent into a request against the
 * capabilities it has just read, hands the request to the provider and
 * wakes whoever waits.  The latest intent wins: a CPU that was busy
 * applies only the newest one.
 *
 * CPUs that share one frequency form a domain, and every member of a
 * domain carries the same intent.  Domains, intents and the relative
 * performance are changed under one mutex; a box is read and written
 * under its own spinlock, which is a leaf and is also taken in the
 * notification handler.  Nobody waits for a CPU with the mutex held,
 * except the copy of the hardware feedback, whose waits are bounded.
 *
 * Without a provider every CPU is its own core and its own domain, the
 * relative performance is unknown and nothing can be set.
 */

#include <kern/cpuperf.h>
#include <kern/thermal.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/uaccess.h>
#include <kern/waitq.h>
#include <hal/hal.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/thermal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The CPU requests of /dev/thermal copy the framework's codes into the
 * records a process reads; the two sets of names carry the same values.
 */
_Static_assert(KERN_THERMAL_CPU_MAX == KERN_CPU_MAX, "the CPU mask of a domain record and of the kernel differ");
_Static_assert(KERN_THERMAL_CPU_IDLE_STATE_MAX == KERN_CPUPERF_IDLE_STATE_MAX, "idle state counts differ");
_Static_assert(KERN_THERMAL_ALL == KERN_CPUPERF_ALL, "the every-domain value differs");
_Static_assert(KERN_THERMAL_UNKNOWN == KERN_CPUPERF_UNKNOWN, "the unknown value differs");
_Static_assert(KERN_THERMAL_CPU_CORE_PERFORMANCE == KERN_CPUPERF_CORE_PERFORMANCE, "core classes differ");
_Static_assert(KERN_THERMAL_CPU_CORE_EFFICIENCY == KERN_CPUPERF_CORE_EFFICIENCY, "core classes differ");
_Static_assert(KERN_THERMAL_CPU_SHARE_THREAD == KERN_CPUPERF_SHARE_THREAD, "sharing codes differ");
_Static_assert(KERN_THERMAL_CPU_SHARE_PACKAGE == KERN_CPUPERF_SHARE_PACKAGE, "sharing codes differ");
_Static_assert(KERN_THERMAL_CPU_LEVELS_RANGE == KERN_CPUPERF_LEVELS_RANGE, "level shapes differ");
_Static_assert(KERN_THERMAL_CPU_LEVELS_STEPS == KERN_CPUPERF_LEVELS_STEPS, "level shapes differ");
_Static_assert(KERN_THERMAL_CPU_RELATIVE_FREQUENCY == KERN_CPUPERF_RELATIVE_FREQUENCY, "relative ways differ");
_Static_assert(KERN_THERMAL_CPU_RELATIVE_FEEDBACK == KERN_CPUPERF_RELATIVE_FEEDBACK, "relative ways differ");
_Static_assert(KERN_THERMAL_CPU_RELATIVE_PROVIDER == KERN_CPUPERF_RELATIVE_PROVIDER, "relative ways differ");
_Static_assert(KERN_THERMAL_CPU_SET_MINIMUM == KERN_CPUPERF_SET_MINIMUM, "setting fields differ");
_Static_assert(KERN_THERMAL_CPU_SET_PREFERENCE == KERN_CPUPERF_SET_PREFERENCE, "setting fields differ");
_Static_assert(KERN_THERMAL_CPU_HAS_CONTROL == KERN_CPUPERF_CPU_HAS_CONTROL, "CPU flags differ");
_Static_assert(KERN_THERMAL_CPU_CUSTOM == KERN_CPUPERF_CPU_CUSTOM, "CPU flags differ");
_Static_assert(KERN_THERMAL_CPU_DOMAIN_PENDING == KERN_CPUPERF_DOMAIN_PENDING, "domain flags differ");
_Static_assert(KERN_THERMAL_CPU_READ_SAMPLE == KERN_CPUPERF_READ_SAMPLE, "read requests differ");

/* The setting fields a direct setting may name. */
#define CPUPERF_SET_FIELDS		(KERN_CPUPERF_SET_MINIMUM | KERN_CPUPERF_SET_MAXIMUM | \
					 KERN_CPUPERF_SET_DESIRED | KERN_CPUPERF_SET_PREFERENCE)

/* How long a thread waits for a CPU to confirm work, in milliseconds. */
#define CPUPERF_CONFIRM_MS		1000U

/* The gap between the two samples of a delivered rate, in milliseconds. */
#define CPUPERF_SAMPLE_GAP_MS		100U

/* How long a CPU runs busy at its highest level to measure its rate, in milliseconds. */
#define CPUPERF_MEASURE_MS		50U

/* How many measuring threads a CPU gets before its rate is left unmeasured. */
#define CPUPERF_MEASURE_TRIES		3U

/* The governor's period, in milliseconds. */
#define CPUPERF_GOVERNOR_PERIOD_MS	100U

/* The busy share, in percent, above which the governor raises a step. */
#define CPUPERF_GOVERNOR_UP_PERCENT	80U

/* The busy share, in percent, below which the governor lowers a step. */
#define CPUPERF_GOVERNOR_DOWN_PERCENT	30U

/* The energy preferences the strategies ask for (0 performance, 255 energy). */
#define CPUPERF_PREFERENCE_PERFORMANCE	0U
#define CPUPERF_PREFERENCE_BALANCED	0x80U
#define CPUPERF_PREFERENCE_POWERSAVE	0xC0U
#define CPUPERF_PREFERENCE_MAX		255U

/* The manual noise from which a STEPS CPU may use its turbo steps. */
#define CPUPERF_STEPS_TURBO_NOISE	50U

/* The scale of a relative performance and of a ceiling. */
#define CPUPERF_RELATIVE_MAX		100U
#define CPUPERF_PERMILLE_MAX		1000U

/* The work a box can hold for its CPU, as bits. */
#define CPUPERF_WORK_TOPOLOGY		0x1U
#define CPUPERF_WORK_APPLY		0x2U
#define CPUPERF_WORK_SAMPLE		0x4U
#define CPUPERF_WORK_IDLE_CORE		0x8U
#define CPUPERF_WORK_IDLE_PACKAGE	0x10U
#define CPUPERF_WORK_FEEDBACK		0x20U
#define CPUPERF_WORK_MEASURE_BEGIN	0x40U
#define CPUPERF_WORK_MEASURE_END	0x80U

/* What a computed request says beyond its numbers, as bits. */
#define CPUPERF_REQUEST_WRITE		0x1U	/* there is something to write */
#define CPUPERF_REQUEST_LIMITED		0x2U	/* the ceiling cut the maximum */
#define CPUPERF_REQUEST_GOVERNED	0x4U	/* the governor picks desired */

/*
 * The intent for one domain, as the threads that set policy leave it.
 *
 * The strategy and its axes come from the thermal core, the direct
 * fields from SET_CPU_DOMAIN, the ceiling from the passive cooling, the governor
 * step from the governor and the measuring mark from the measurement of
 * the highest rate.  A field that nobody set holds its neutral value:
 * FIRMWARE, no direct field, KERN_THERMAL_LIMIT_NONE, KERN_CPUPERF_UNKNOWN
 * and false.  Direct bounds given for every domain stay permille until
 * the CPU turns them into units against its anchors.
 */
struct cpuperf_intent {
	uint32_t strategy;
	uint32_t speed;
	uint32_t noise;
	uint32_t direct_fields;
	bool direct_permille;
	uint32_t direct_minimum;
	uint32_t direct_maximum;
	uint32_t direct_desired;
	uint32_t direct_preference;
	uint32_t limit_permille;
	uint32_t governor_desired;
	bool measuring;
};

/*
 * One CPU's box: the work posted for it, the intent it applies and what
 * it reported back.
 *
 * Everything in it but pending is guarded by its lock, which the
 * notification handler takes as well, so every holder masks interrupts.
 * pending is set after work is added and lets the handler skip the lock
 * when nothing was posted.  posted counts the posts and completed the
 * posts the handler has served; a poster waits until completed reaches
 * the count its post made.
 */
struct cpuperf_box {
	struct spinlock lock;
	struct wait_queue done;
	atomic_uint_t pending;
	uint32_t work;
	uint64_t posted;
	uint64_t completed;
	struct cpuperf_intent intent;
	struct kern_cpuperf_topology topology;
	int topology_error;
	struct kern_cpuperf_levels levels;
	bool control;
	bool started;
	uint32_t domain;
	struct kern_cpuperf_request request;
	uint32_t request_flags;
	int apply_error;
	struct kern_cpuperf_sample sample;
	struct kern_cpuperf_idle idle;
	int idle_error;
	bool feedback_changed;
	int feedback_error;
	uint32_t measured_mhz;
	int measure_error;
	uint32_t relative;
	uint32_t available;
	uint32_t efficiency;
};

/*
 * One frequency domain: the CPUs that share a frequency, the levels its
 * first member had when the domains were grouped and the intent every
 * member carries.  Guarded by the framework's mutex.
 */
struct cpuperf_domain {
	uint64_t cpus[KERN_CPU_MASK_WORDS];
	uint32_t cpu_count;
	kern_cpu_id_t first_cpu;
	struct kern_cpuperf_levels levels;
	bool control;
	struct cpuperf_intent intent;
};

/*
 * One measurement of a CPU's highest rate, handed to the thread that runs
 * it on that CPU.  The waiter frees it once done is set; a waiter that
 * gave up leaves it to the thread, which then frees it.
 */
struct cpuperf_measure {
	kern_cpu_id_t cpu;
	struct spinlock lock;
	struct wait_queue queue;
	bool done;
	bool abandoned;
	int error;
	uint32_t mhz;
};

/*
 * Serializes every change of policy: the provider's registration, the
 * domains, the intents, the relative performance, the feedback copies and
 * the governor's passes.
 */
static struct mutex cpuperf_mutex;

/*
 * The registered provider, or NULL.
 *
 * It is set once, under the mutex, and never withdrawn.  A holder of the
 * mutex reads it directly, and so does the notification handler, which
 * reads it under a box lock that the poster took after setting it.  Any
 * other reader goes through cpuperf_ops(), which checks
 * cpuperf_provider_ready first.
 */
static const struct kern_cpuperf_provider_ops *cpuperf_provider;

/* Nonzero once cpuperf_provider is set; stored with release order. */
static atomic_uint_t cpuperf_provider_ready;

/*
 * The number of CPUs the framework serves, fixed by kern_cpuperf_init().
 * Zero until then, and zero for good if its tables could not be allocated.
 */
static uint32_t cpuperf_cpu_count;

/* The boxes, one per CPU, allocated once and kept for the kernel's life. */
static struct cpuperf_box *cpuperf_boxes;

/*
 * The domains, at most one per CPU, allocated once.  Only the first
 * cpuperf_domain_total entries are in use; they are grouped under the
 * mutex when the provider registers.
 */
static struct cpuperf_domain *cpuperf_domains;
static uint32_t cpuperf_domain_total;

/* The way relative performance is measured now, a KERN_CPUPERF_RELATIVE_* bit. */
static uint32_t cpuperf_relative_source = KERN_CPUPERF_RELATIVE_FREQUENCY;

/*
 * Whether the highest rates have been measured.  Set under the mutex by
 * the first strategy that the CPUs confirmed, so the measurement runs
 * once per boot.
 */
static bool cpuperf_measured;

/*
 * Moves whenever the topology, the domains, the capabilities, the
 * relative performance or the feedback change, so a reader that remembers
 * it knows when to read the CPUs again.  It wraps; only inequality means
 * anything.
 */
static atomic_uint_t cpuperf_generation_count;

/*
 * Nonzero while a direct setting is in force anywhere.  The thermal
 * core reads it without a lock to mark its strategy CUSTOM; the next
 * strategy clears it.
 */
static atomic_uint_t cpuperf_custom;

/*
 * The governor of STEPS domains: its thread (NULL until a STEPS domain
 * first needs it), the queue it sleeps on between passes and the lock of
 * that queue, and each CPU's scheduler time at the previous pass, from
 * which it works out how busy the CPU was.  The thread and the times are
 * guarded by the mutex.
 */
static struct thread *cpuperf_governor_thread;
static struct wait_queue cpuperf_governor_queue;
static struct spinlock cpuperf_governor_lock;
static struct sched_cpu_time *cpuperf_governor_times;

static int cpuperf_receiver_apply(void *context, const struct kern_thermal_strategy *strategy, uint32_t *flags);
static int cpuperf_receiver_limit(void *context, uint32_t domain, uint32_t permille);
static void cpuperf_receiver_poll(void *context);
static bool cpuperf_receiver_is_custom(void *context);

/*
 * The framework as a receiver of the thermal core: the strategy and the
 * thermal ceiling of the CPUs, and the copy of the hardware feedback.
 */
static const struct kern_thermal_receiver_ops cpuperf_receiver = {
	cpuperf_receiver_apply,
	cpuperf_receiver_limit,
	cpuperf_receiver_poll,
	cpuperf_receiver_is_custom,
	NULL
};

static const struct kern_cpuperf_provider_ops *cpuperf_ops(void);
static void cpuperf_intent_reset(struct cpuperf_intent *intent);
static void cpuperf_topology_default(kern_cpu_id_t cpu, struct kern_cpuperf_topology *topology);
static void cpuperf_levels_none(struct kern_cpuperf_levels *levels);
static bool cpuperf_anchors_known(const struct kern_cpuperf_levels *levels);
static bool cpuperf_anchors_unknown(const struct kern_cpuperf_levels *levels);
static bool cpuperf_levels_usable(struct kern_cpuperf_levels *levels);
static bool cpuperf_shares(const struct kern_cpuperf_topology *first, const struct kern_cpuperf_topology *other, uint32_t share);
static void cpuperf_domains_build(void);
static void cpuperf_domain_add(uint32_t domain, kern_cpu_id_t cpu);
static bool cpuperf_domain_has(const struct cpuperf_domain *domain, kern_cpu_id_t cpu);
static uint64_t cpuperf_post(kern_cpu_id_t cpu, uint32_t work, const struct cpuperf_intent *intent);
static void cpuperf_deliver(kern_cpu_id_t cpu);
static int cpuperf_wait(kern_cpu_id_t cpu, uint64_t ticket, uint64_t deadline);
static uint64_t cpuperf_deadline(void);
static void cpuperf_sleep_ms(uint32_t milliseconds);
static kern_cpu_id_t cpuperf_package_cpu(uint32_t package);
static kern_cpu_id_t cpuperf_package_first(uint32_t package);
static void cpuperf_domain_post(uint32_t domain, uint64_t *tickets);
static int cpuperf_domain_wait(uint32_t domain, const uint64_t *tickets, uint64_t deadline, int *apply_error);
static void cpuperf_run(struct cpuperf_box *box, kern_cpu_id_t cpu, uint32_t work);
static void cpuperf_run_apply(struct cpuperf_box *box, const struct kern_cpuperf_provider_ops *provider, kern_cpu_id_t cpu);
static void cpuperf_caps_refresh(struct kern_cpuperf_levels *levels, const struct kern_cpuperf_sample *sample);
static uint32_t cpuperf_compute(const struct cpuperf_intent *intent, const struct kern_cpuperf_levels *levels, struct kern_cpuperf_request *request);
static void cpuperf_compute_range(const struct cpuperf_intent *intent, const struct kern_cpuperf_levels *levels, struct kern_cpuperf_request *request);
static uint32_t cpuperf_compute_steps(const struct cpuperf_intent *intent, const struct kern_cpuperf_levels *levels, struct kern_cpuperf_request *request);
static void cpuperf_compute_direct(const struct cpuperf_intent *intent, const struct kern_cpuperf_levels *levels, struct kern_cpuperf_request *request, uint32_t *flags);
static void cpuperf_compute_bounds(const struct cpuperf_intent *intent, const struct kern_cpuperf_levels *levels, struct kern_cpuperf_request *request, uint32_t *flags);
static uint32_t cpuperf_ceiling(const struct kern_cpuperf_levels *levels, uint32_t permille);
static uint32_t cpuperf_scale(uint32_t low, uint32_t high, uint32_t part, uint32_t whole);
static uint32_t cpuperf_step_floor(const struct kern_cpuperf_levels *levels, uint32_t value);
static uint32_t cpuperf_step_index(const struct kern_cpuperf_levels *levels, uint32_t value);
static bool cpuperf_governed(const struct cpuperf_intent *intent, const struct kern_cpuperf_levels *levels);
static int cpuperf_setting_check(const struct kern_cpuperf_levels *levels, const struct kern_cpuperf_setting *setting, bool permille);
static void cpuperf_setting_store(struct cpuperf_intent *intent, const struct kern_cpuperf_setting *setting, bool permille);
static void cpuperf_relative_refresh(void);
static int cpuperf_relative_raw(const struct kern_cpuperf_provider_ops *provider, kern_cpu_id_t cpu, uint32_t *raw);
static void cpuperf_feedback_copy(void);
static void cpuperf_feedback_store(uint32_t package, kern_cpu_id_t first);
static void cpuperf_describe_all(void);
static void cpuperf_publish_change(void);
static void cpuperf_measure_all(void);
static int cpuperf_measure_domain(kern_cpu_id_t cpu, uint32_t *mhz);
static int cpuperf_measure_cpu(kern_cpu_id_t cpu, uint32_t *mhz);
static int cpuperf_measure_once(kern_cpu_id_t cpu, uint32_t *mhz);
static void cpuperf_measure_main(void *argument);
static void cpuperf_governor_start(void);
static void cpuperf_governor_main(void *argument);
static bool cpuperf_governor_pass(void);
static uint32_t cpuperf_busy_percent(kern_cpu_id_t cpu);
static bool cpuperf_any_control(void);
static int cpuperf_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static bool cpuperf_writable(const struct file *file);
static int cpuperf_ioctl_cpu(struct file *file, uintptr_t argument);
static int cpuperf_ioctl_domain(uintptr_t argument);
static int cpuperf_ioctl_domain_set(struct file *file, uintptr_t argument);
static int cpuperf_ioctl_relative_get(uintptr_t argument);
static int cpuperf_ioctl_relative_set(struct file *file, uintptr_t argument);
static int cpuperf_ioctl_idle(struct file *file, uintptr_t argument, bool package);

/*
 * Brings the framework up once every CPU takes notifications.
 *
 * Every CPU starts as its own core and its own domain with nothing to
 * control; a provider that registers later replaces that picture.  The
 * framework registers itself with the thermal core as the receiver "cpu".
 */
void
kern_cpuperf_init(void)
{
	struct cpuperf_box *box;
	uint32_t count;
	kern_cpu_id_t cpu;
	int error;

	/* Counts the CPUs the platform brought up. */
	count = kern_cpu_count();
	if (count == 0U || count > KERN_CPU_MAX)
		return;

	/* Allocates a box for every CPU. */
	cpuperf_boxes = kern_calloc(count, sizeof(*cpuperf_boxes));
	if (cpuperf_boxes == NULL) {
		kern_logf("cpuperf: no memory for %u CPUs; CPU performance is unavailable\n", count);
		return;
	}

	/* Allocates room for a domain per CPU. */
	cpuperf_domains = kern_calloc(count, sizeof(*cpuperf_domains));
	if (cpuperf_domains == NULL) {
		kern_logf("cpuperf: no memory for %u domains; CPU performance is unavailable\n", count);
		return;
	}

	/* Allocates the governor's record of each CPU's time. */
	cpuperf_governor_times = kern_calloc(count, sizeof(*cpuperf_governor_times));
	if (cpuperf_governor_times == NULL) {
		kern_logf("cpuperf: no memory for the governor; CPU performance is unavailable\n");
		return;
	}

	/* Prepares the framework's mutex. */
	error = mutex_init(&cpuperf_mutex, LOCK_RANK_DEVICE, "cpuperf");
	if (error != 0) {
		kern_logf("cpuperf: lock initialization failed (%d); CPU performance is unavailable\n", error);
		return;
	}

	/* Prepares the governor's lock and queue. */
	spin_init(&cpuperf_governor_lock, LOCK_RANK_DEVICE, "cpuperf-governor");
	waitq_init(&cpuperf_governor_queue, "cpuperf-governor");

	/* Gives every CPU the picture of a machine without a provider. */
	for (cpu = 0; cpu < count; cpu++) {
		box = &cpuperf_boxes[cpu];
		spin_init(&box->lock, LOCK_RANK_DEVICE, "cpuperf-box");
		waitq_init(&box->done, "cpuperf-box");
		cpuperf_intent_reset(&box->intent);
		cpuperf_topology_default(cpu, &box->topology);
		cpuperf_levels_none(&box->levels);
		box->control = false;
		box->started = false;
		box->relative = KERN_CPUPERF_UNKNOWN;
		box->available = KERN_CPUPERF_UNKNOWN;
		box->efficiency = KERN_CPUPERF_UNKNOWN;
	}

	/*
	 * Publishes the count last: every other entry point refuses to work
	 * while it is zero, so nothing reaches a half-built table.
	 */
	cpuperf_cpu_count = count;

	/* Builds one domain per CPU. */
	mutex_lock(&cpuperf_mutex);

	cpuperf_domains_build();

	mutex_unlock(&cpuperf_mutex);

	/* Offers the strategy and the ceiling of the CPUs to the thermal core. */
	error = kern_thermal_receiver_register("cpu", &cpuperf_receiver, NULL);
	if (error != 0)
		kern_logf("cpuperf: the thermal core refused the receiver (%d)\n", error);

	/* Answers the CPU requests of /dev/thermal. */
	error = kern_thermal_ioctl_range_register(KERN_THERMAL_IOCTL_CPU_FIRST, KERN_THERMAL_IOCTL_CPU_LAST, cpuperf_ioctl);
	if (error != 0)
		kern_logf("cpuperf: the thermal core refused the CPU requests (%d)\n", error);
}

/*
 * Registers the one provider.
 *
 * Reads every CPU's topology on that CPU, asks for every CPU's levels,
 * groups the CPUs into domains and measures their relative performance.
 * Nothing is written to the processor here: the first request comes with
 * the first strategy or direct setting.
 */
int
kern_cpuperf_provider_register(
	const struct kern_cpuperf_provider_ops *ops)
{
	uint64_t deadline;
	uint64_t ticket;
	kern_cpu_id_t cpu;
	uint32_t answered;
	int error;

	/* Refuses a provider that cannot describe the machine. */
	if (ops == NULL)
		return EINVAL;
	if (ops->topology == NULL)
		return EINVAL;
	if (ops->describe == NULL)
		return EINVAL;

	/* Refuses a provider before the framework is up. */
	if (cpuperf_cpu_count == 0U)
		return ENODEV;

	/* Serializes the registration against other changes of policy. */
	mutex_lock(&cpuperf_mutex);

	/* Refuses a second provider. */
	if (cpuperf_provider != NULL) {
		mutex_unlock(&cpuperf_mutex);
		return EBUSY;
	}

	/*
	 * Publishes the provider before any CPU is asked, so each CPU's
	 * notification handler finds it.  The release orders the provider's
	 * own tables, filled before this call, ahead of the flag.
	 */
	cpuperf_provider = ops;
	atomic_store_release(&cpuperf_provider_ready, 1U);

	/* Asks every CPU for its topology and waits a bounded time for all. */
	deadline = cpuperf_deadline();
	answered = 0U;
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		ticket = cpuperf_post(cpu, CPUPERF_WORK_TOPOLOGY, NULL);
		error = cpuperf_wait(cpu, ticket, deadline);
		if (error == 0)
			answered++;
	}

	/* Asks for the levels, groups the domains and measures. */
	cpuperf_describe_all();
	cpuperf_domains_build();
	cpuperf_relative_refresh();

	mutex_unlock(&cpuperf_mutex);

	/* Tells the readers the CPUs changed. */
	cpuperf_publish_change();

	/* Reports how many CPUs answered and how many domains they form. */
	kern_logf("cpuperf: provider registered; %u of %u CPUs answered, %u domains\n",
		  answered, cpuperf_cpu_count, cpuperf_domain_total);

	/* Succeeded: the provider serves the CPUs from now on. */
	return 0;
}

/*
 * Tells the framework that a provider's answers changed.
 *
 * The provider calls it from a thread when the capabilities it describes
 * change or when it can measure relative performance in a new way.
 */
void
kern_cpuperf_provider_changed(
	uint32_t what)
{
	const struct kern_cpuperf_provider_ops *provider;

	/* Ignores a call before a provider exists. */
	provider = cpuperf_ops();
	if (provider == NULL)
		return;

	/* Takes the provider's changed answers into the CPUs. */
	mutex_lock(&cpuperf_mutex);

	/* Takes the new capabilities into the CPUs; the domains keep their members. */
	if ((what & KERN_CPUPERF_CHANGED_CAPABILITIES) != 0U)
		cpuperf_describe_all();

	/* Measures the relative performance again. */
	cpuperf_relative_refresh();

	mutex_unlock(&cpuperf_mutex);

	/* Tells the readers the CPUs changed. */
	cpuperf_publish_change();
}

/*
 * Runs the framework's part of a cross-CPU notification on this CPU.
 *
 * Called from the notification handler after the interrupt is
 * acknowledged and before the scheduler looks for work, with interrupts
 * masked.  Most notifications are the scheduler's and find nothing
 * pending; those return without a lock.
 */
void
kern_cpuperf_notify(
	kern_cpu_id_t cpu)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	uint32_t work;
	uint32_t pending;

	/* Refuses a CPU the framework does not serve. */
	if (cpu >= cpuperf_cpu_count)
		return;

	/* Returns at once when nothing was posted for this CPU. */
	box = &cpuperf_boxes[cpu];
	pending = atomic_load_acquire(&box->pending);
	if (pending == 0U)
		return;

	/*
	 * Clears the mark before taking the work, so a post that lands after
	 * the work is taken sets it again and is served by its own
	 * notification.
	 */
	atomic_store_release(&box->pending, 0U);

	/* Serves whatever was posted for this CPU and wakes the posters. */
	enabled = spin_lock_irqsave(&box->lock);

	/* Takes the posted work, leaving the box empty. */
	work = box->work;
	box->work = 0U;

	/* Runs the work when there is some and a provider to run it on. */
	if (work != 0U && cpuperf_provider != NULL)
		cpuperf_run(box, cpu, work);

	/* Records that every post made so far is served and wakes the posters. */
	box->completed = box->posted;
	waitq_wake_all(&box->done);

	spin_unlock_irqrestore(&box->lock, enabled);
}

/*
 * Reports one CPU: its place, its domain, its relative performance and
 * its levels.
 */
int
kern_cpuperf_cpu(
	kern_cpu_id_t cpu,
	uint32_t flags,
	struct kern_cpuperf_cpu *info)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct cpuperf_box *box;
	struct kern_cpuperf_levels levels;
	unsigned long enabled;
	uint64_t ticket;
	uint64_t deadline;
	uint32_t measured;
	uint32_t custom;
	int error;

	/* Refuses a CPU that does not exist. */
	if (info == NULL)
		return EINVAL;
	if (cpu >= cpuperf_cpu_count)
		return EINVAL;

	/*
	 * Samples the CPU twice, a short gap apart, when the caller asked for
	 * the rate it delivers: the second sample reports the rate over the
	 * gap, whoever sampled the CPU before.
	 */
	error = 0;
	provider = cpuperf_ops();
	if ((flags & KERN_CPUPERF_READ_SAMPLE) != 0U &&
	    provider != NULL &&
	    provider->sample != NULL) {
		ticket = cpuperf_post(cpu, CPUPERF_WORK_SAMPLE, NULL);
		deadline = cpuperf_deadline();
		error = cpuperf_wait(cpu, ticket, deadline);
		if (error == 0) {
			cpuperf_sleep_ms(CPUPERF_SAMPLE_GAP_MS);
			ticket = cpuperf_post(cpu, CPUPERF_WORK_SAMPLE, NULL);
			deadline = cpuperf_deadline();
			error = cpuperf_wait(cpu, ticket, deadline);
		}
	}

	/* Copies the CPU's report out of its box. */
	box = &cpuperf_boxes[cpu];
	enabled = spin_lock_irqsave(&box->lock);

	info->topology = box->topology;
	info->domain = box->domain;
	levels = box->levels;
	measured = box->measured_mhz;
	info->relative = box->relative;
	info->available = box->available;
	info->efficiency = box->efficiency;
	info->delivered_mhz = box->sample.delivered_mhz;
	info->flags = 0U;

	/* Marks a CPU the framework can control. */
	if (box->control)
		info->flags |= KERN_CPUPERF_CPU_HAS_CONTROL;

	/* Marks a CPU whose maximum the thermal ceiling cut. */
	if ((box->request_flags & CPUPERF_REQUEST_LIMITED) != 0U)
		info->flags |= KERN_CPUPERF_CPU_LIMITED;

	/* Marks a CPU the platform holds below its request. */
	if ((box->sample.flags & KERN_CPUPERF_SAMPLE_PLATFORM_LIMIT) != 0U)
		info->flags |= KERN_CPUPERF_CPU_PLATFORM_LIMIT;

	spin_unlock_irqrestore(&box->lock, enabled);

	/* Reports the levels the CPU offers. */
	info->voltage_group = levels.voltage_group;
	info->lowest = levels.lowest;
	info->efficient = levels.efficient;
	info->guaranteed = levels.guaranteed;
	info->highest = levels.highest;
	info->highest_mhz = levels.highest_mhz;

	/* Prefers the measured highest rate to the provider's estimate. */
	if (measured != 0U)
		info->highest_mhz = measured;

	/* Marks the MHz as estimates when there are any. */
	if (info->highest_mhz != 0U)
		info->flags |= KERN_CPUPERF_CPU_ESTIMATED_MHZ;
	if (info->delivered_mhz != 0U)
		info->flags |= KERN_CPUPERF_CPU_ESTIMATED_MHZ;

	/* Marks a direct setting in force. */
	custom = atomic_load_acquire(&cpuperf_custom);
	if (custom != 0U)
		info->flags |= KERN_CPUPERF_CPU_CUSTOM;

	/* Reports a sample the CPU did not confirm in time. */
	if (error != 0)
		return error;

	/* Succeeded: the report is complete. */
	return 0;
}

/*
 * Reports one domain: its members, its levels, the request its intent
 * asks for before the thermal ceiling, and the ceiling.
 */
int
kern_cpuperf_domain(
	uint32_t domain,
	struct kern_cpuperf_domain *info)
{
	struct cpuperf_domain *entry;
	struct cpuperf_intent unlimited;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	uint32_t word;
	bool pending;
	bool known;
	bool member;

	/* Refuses a missing answer or a framework that is not up. */
	if (info == NULL)
		return EINVAL;
	if (cpuperf_cpu_count == 0U)
		return EINVAL;

	/* Reads the domain under the mutex that groups the domains. */
	mutex_lock(&cpuperf_mutex);

	/* Refuses a domain that does not exist. */
	if (domain >= cpuperf_domain_total) {
		mutex_unlock(&cpuperf_mutex);
		return EINVAL;
	}

	/* Copies the count of members and the mask that names them. */
	entry = &cpuperf_domains[domain];
	info->count = cpuperf_domain_total;
	info->cpu_count = entry->cpu_count;
	info->flags = 0U;
	for (word = 0; word < KERN_CPU_MASK_WORDS; word++)
		info->cpus[word] = entry->cpus[word];

	/* Copies the levels the first member knows now. */
	box = &cpuperf_boxes[entry->first_cpu];
	enabled = spin_lock_irqsave(&box->lock);

	info->levels = box->levels;

	spin_unlock_irqrestore(&box->lock, enabled);

	/* Works out the request the intent asks for before the ceiling. */
	unlimited = entry->intent;
	unlimited.limit_permille = KERN_THERMAL_LIMIT_NONE;
	(void)cpuperf_compute(&unlimited, &info->levels, &info->request);

	/* Reports the ceiling in units when one is set and the anchors are known. */
	info->limit = KERN_CPUPERF_UNKNOWN;
	known = cpuperf_anchors_known(&info->levels);
	if (entry->control &&
	    known &&
	    entry->intent.limit_permille < KERN_THERMAL_LIMIT_NONE) {
		info->limit = cpuperf_ceiling(&info->levels, entry->intent.limit_permille);
		info->flags |= KERN_CPUPERF_DOMAIN_LIMITED;
	}

	/* Marks a direct setting in force. */
	if (entry->intent.direct_fields != 0U)
		info->flags |= KERN_CPUPERF_DOMAIN_CUSTOM;

	/* Finds a member that has not confirmed its latest request. */
	pending = false;
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		/* Skips a CPU that is not a member. */
		member = cpuperf_domain_has(entry, cpu);
		if (!member)
			continue;
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		/* Notes a post the CPU has not served yet. */
		if (box->completed < box->posted)
			pending = true;

		spin_unlock_irqrestore(&box->lock, enabled);
	}

	mutex_unlock(&cpuperf_mutex);

	/* Marks a request still on its way. */
	if (pending)
		info->flags |= KERN_CPUPERF_DOMAIN_PENDING;

	/* Succeeded: the report is complete. */
	return 0;
}

/*
 * Reports the number of domains.
 */
uint32_t
kern_cpuperf_domain_count(void)
{
	uint32_t total;

	/* Has no domains before the framework is up. */
	if (cpuperf_cpu_count == 0U)
		return 0U;

	/* Reads the count under the lock that groups the domains. */
	mutex_lock(&cpuperf_mutex);

	total = cpuperf_domain_total;

	mutex_unlock(&cpuperf_mutex);

	/* Reports the number of domains. */
	return total;
}

/*
 * Reports the number of packages the CPUs belong to.
 */
uint32_t
kern_cpuperf_package_count(void)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	kern_cpu_id_t first;
	uint32_t package;
	uint32_t count;

	/* Counts each CPU whose package no earlier CPU has. */
	count = 0;
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		package = box->topology.package;

		spin_unlock_irqrestore(&box->lock, enabled);

		/* The first CPU of the package is the one that counts it. */
		first = cpuperf_package_first(package);
		if (first == cpu)
			count++;
	}

	/* Succeeded: the number of packages, 0 before the framework is up. */
	return count;
}

/*
 * Reports the generation of the CPUs' answers.
 */
uint32_t
kern_cpuperf_generation(void)
{
	uint32_t generation;

	/* Reads the counter that every change moves. */
	generation = atomic_load_acquire(&cpuperf_generation_count);

	/* Reports the generation. */
	return generation;
}

/*
 * Applies a direct setting to one domain or to every domain.
 *
 * The fields named in the setting replace the strategy's in each domain
 * it reaches until the next strategy.
 */
int
kern_cpuperf_domain_set(
	uint32_t domain,
	const struct kern_cpuperf_setting *setting)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct kern_cpuperf_levels levels;
	struct cpuperf_box *box;
	unsigned long enabled;
	uint64_t *tickets;
	uint64_t deadline;
	uint32_t index;
	uint32_t first;
	uint32_t last;
	bool every;
	bool any;
	int apply_error;
	int error;

	/* Refuses an empty or unknown setting. */
	if (setting == NULL)
		return EINVAL;
	if (setting->fields == 0U)
		return EINVAL;
	if ((setting->fields & ~(KERN_CPUPERF_SET_MINIMUM |
				 KERN_CPUPERF_SET_MAXIMUM |
				 KERN_CPUPERF_SET_DESIRED |
				 KERN_CPUPERF_SET_PREFERENCE)) != 0U)
		return EINVAL;

	/* Refuses a machine where nothing can be set. */
	provider = cpuperf_ops();
	if (provider == NULL)
		return EOPNOTSUPP;
	if (provider->apply == NULL)
		return EOPNOTSUPP;

	/* Allocates a ticket per CPU for the confirmation. */
	tickets = kern_calloc(cpuperf_cpu_count, sizeof(*tickets));
	if (tickets == NULL)
		return ENOMEM;

	/* Holds the domains still while the setting is checked and stored. */
	mutex_lock(&cpuperf_mutex);

	/* Picks the domains the setting reaches. */
	every = false;
	first = domain;
	last = domain + 1U;
	if (domain == KERN_CPUPERF_ALL) {
		every = true;
		first = 0U;
		last = cpuperf_domain_total;
	}

	/* Refuses a domain that does not exist or cannot be controlled. */
	error = 0;
	any = cpuperf_any_control();
	if (!every && domain >= cpuperf_domain_total)
		error = EINVAL;
	else if (!every && !cpuperf_domains[domain].control)
		error = EOPNOTSUPP;
	else if (every && !any)
		error = EOPNOTSUPP;

	/* Checks the values against every domain they reach before changing any. */
	for (index = first; error == 0 && index < last; index++) {
		/* Skips a domain with nothing to control. */
		if (!cpuperf_domains[index].control)
			continue;
		box = &cpuperf_boxes[cpuperf_domains[index].first_cpu];
		enabled = spin_lock_irqsave(&box->lock);

		levels = box->levels;

		spin_unlock_irqrestore(&box->lock, enabled);

		/* Checks the setting against this domain's levels. */
		error = cpuperf_setting_check(&levels, setting, every);
	}

	/* Refuses a setting that does not fit a domain it reaches. */
	if (error != 0) {
		mutex_unlock(&cpuperf_mutex);
		kern_free(tickets);
		return error;
	}

	/*
	 * Stores the setting and posts it.  CUSTOM tells the thermal core
	 * that a direct setting overrides its strategy until the next one.
	 */
	for (index = first; index < last; index++) {
		/* Skips a domain with nothing to control. */
		if (!cpuperf_domains[index].control)
			continue;
		cpuperf_setting_store(&cpuperf_domains[index].intent, setting, every);
		cpuperf_domain_post(index, tickets);
	}

	/* Marks the direct setting in force and makes sure the governor runs. */
	atomic_store_release(&cpuperf_custom, 1U);
	cpuperf_governor_start();

	mutex_unlock(&cpuperf_mutex);

	/* Waits for the CPUs to confirm, without the mutex. */
	deadline = cpuperf_deadline();
	apply_error = 0;
	error = 0;
	for (index = first; error == 0 && index < last; index++) {
		/* Skips a domain with nothing to control. */
		if (!cpuperf_domains[index].control)
			continue;
		error = cpuperf_domain_wait(index, tickets, deadline, &apply_error);
	}

	/* Releases the tickets. */
	kern_free(tickets);

	/* Reports a CPU that did not confirm in time; the setting stays. */
	if (error != 0)
		return error;

	/* Reports a request the provider refused. */
	if (apply_error != 0)
		return apply_error;

	/* Succeeded: every CPU of the domains runs with the setting. */
	return 0;
}

/*
 * Reports the relative ways that can be chosen and the one in force.
 */
void
kern_cpuperf_relative_get(
	uint32_t *available,
	uint32_t *source)
{
	const struct kern_cpuperf_provider_ops *provider;

	/* Reports the provider's ways, and FREQUENCY when the rate can be measured. */
	*available = 0U;
	provider = cpuperf_ops();
	if (provider != NULL) {
		*available = provider->relative_sources &
			     (KERN_CPUPERF_RELATIVE_FEEDBACK | KERN_CPUPERF_RELATIVE_PROVIDER);
		if (provider->measure != NULL)
			*available |= KERN_CPUPERF_RELATIVE_FREQUENCY;
	}

	/* Reports the way in force. */
	*source = cpuperf_relative_source;
}

/*
 * Chooses the way relative performance is measured, until the next boot.
 */
int
kern_cpuperf_relative_select(
	uint32_t source)
{
	uint32_t available;
	uint32_t current;

	/* Refuses anything but one known way. */
	if (source != KERN_CPUPERF_RELATIVE_FREQUENCY &&
	    source != KERN_CPUPERF_RELATIVE_FEEDBACK &&
	    source != KERN_CPUPERF_RELATIVE_PROVIDER)
		return EINVAL;

	/* Refuses a way that cannot be measured here. */
	kern_cpuperf_relative_get(&available, &current);
	if ((available & source) == 0U)
		return EOPNOTSUPP;

	/* Measures every CPU again by the new way. */
	mutex_lock(&cpuperf_mutex);

	cpuperf_relative_source = source;
	cpuperf_relative_refresh();

	mutex_unlock(&cpuperf_mutex);

	/* Tells the readers the CPUs changed. */
	cpuperf_publish_change();

	/* Succeeded: the new way is in force. */
	return 0;
}

/*
 * Reads the idle residency of a CPU's core, on that CPU.
 */
int
kern_cpuperf_idle_cpu(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_idle *idle)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct cpuperf_box *box;
	unsigned long enabled;
	uint64_t ticket;
	uint64_t deadline;
	uint64_t ticks;
	int error;

	/* Refuses a CPU that does not exist or a provider without the counters. */
	if (idle == NULL)
		return EINVAL;
	if (cpu >= cpuperf_cpu_count)
		return EINVAL;
	provider = cpuperf_ops();
	if (provider == NULL)
		return EOPNOTSUPP;
	if (provider->idle_core == NULL)
		return EOPNOTSUPP;

	/* Reads the counters on the CPU. */
	ticket = cpuperf_post(cpu, CPUPERF_WORK_IDLE_CORE, NULL);
	deadline = cpuperf_deadline();
	error = cpuperf_wait(cpu, ticket, deadline);
	if (error != 0)
		return error;

	/* Copies what the CPU read. */
	box = &cpuperf_boxes[cpu];
	enabled = spin_lock_irqsave(&box->lock);

	*idle = box->idle;
	error = box->idle_error;

	spin_unlock_irqrestore(&box->lock, enabled);

	/* Reports counters the provider could not read. */
	if (error != 0)
		return error;

	/* Stamps the read with the kernel's tick time. */
	ticks = sched_ticks();
	idle->time_ns = kern_ticks_to_ns(ticks);

	/* Succeeded: the residency of the core. */
	return 0;
}

/*
 * Reads the idle residency of a package, on the calling CPU when it
 * belongs to the package and otherwise on the package's first CPU, so a
 * machine of one package wakes no other CPU for it.
 */
int
kern_cpuperf_idle_package(
	uint32_t package,
	struct kern_cpuperf_idle *idle)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t target;
	uint64_t ticket;
	uint64_t deadline;
	uint64_t ticks;
	int error;

	/* Refuses a provider without the counters. */
	if (idle == NULL)
		return EINVAL;
	provider = cpuperf_ops();
	if (provider == NULL)
		return EOPNOTSUPP;
	if (provider->idle_package == NULL)
		return EOPNOTSUPP;

	/* Picks a CPU of the package. */
	target = cpuperf_package_cpu(package);
	if (target == KERN_CPUPERF_UNKNOWN)
		return EINVAL;

	/* Reads the counters on that CPU. */
	ticket = cpuperf_post(target, CPUPERF_WORK_IDLE_PACKAGE, NULL);
	deadline = cpuperf_deadline();
	error = cpuperf_wait(target, ticket, deadline);
	if (error != 0)
		return error;

	/* Copies what the CPU read. */
	box = &cpuperf_boxes[target];
	enabled = spin_lock_irqsave(&box->lock);

	*idle = box->idle;
	error = box->idle_error;

	spin_unlock_irqrestore(&box->lock, enabled);

	/* Reports counters the provider could not read. */
	if (error != 0)
		return error;

	/* Stamps the read with the kernel's tick time. */
	ticks = sched_ticks();
	idle->time_ns = kern_ticks_to_ns(ticks);

	/* Succeeded: the residency of the package. */
	return 0;
}

/*
 * Carries a strategy out on every domain that can be controlled.
 *
 * Runs in the thermal core's strategy thread and may sleep.
 */
static int
cpuperf_receiver_apply(
	void *context,
	const struct kern_thermal_strategy *strategy,
	uint32_t *flags)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct cpuperf_intent *intent;
	uint64_t *tickets;
	uint64_t deadline;
	uint32_t index;
	bool measure;
	bool any;
	int apply_error;
	int error;

	UNUSED_PARAMETER(context);

	/* Refuses a strategy this file does not know. */
	if (strategy == NULL)
		return EINVAL;
	if (strategy->value > KERN_THERMAL_STRATEGY_MANUAL)
		return EINVAL;

	/* Refuses manual axes out of range. */
	if (strategy->value == KERN_THERMAL_STRATEGY_MANUAL &&
	    (strategy->speed > KERN_THERMAL_AXIS_MAX ||
	     strategy->noise > KERN_THERMAL_AXIS_MAX ||
	     strategy->gating > KERN_THERMAL_AXIS_MAX))
		return EINVAL;

	/* Has nothing to act on without a provider that can write a request. */
	provider = cpuperf_ops();
	if (provider == NULL)
		return EOPNOTSUPP;
	if (provider->apply == NULL)
		return EOPNOTSUPP;

	/* Leaves the firmware's settings alone for FIRMWARE. */
	if (strategy->value == KERN_THERMAL_STRATEGY_FIRMWARE)
		return 0;

	/* Allocates a ticket per CPU for the confirmation. */
	tickets = kern_calloc(cpuperf_cpu_count, sizeof(*tickets));
	if (tickets == NULL)
		return ENOMEM;

	/* Holds the domains still while the strategy is stored. */
	mutex_lock(&cpuperf_mutex);

	/* Refuses a machine where no domain can be controlled. */
	any = cpuperf_any_control();
	if (!any) {
		mutex_unlock(&cpuperf_mutex);
		kern_free(tickets);
		return EOPNOTSUPP;
	}

	/*
	 * Replaces every domain's intent with the strategy and posts it.  A
	 * new strategy ends any direct setting, so CUSTOM is cleared.
	 */
	for (index = 0; index < cpuperf_domain_total; index++) {
		/* Skips a domain with nothing to control. */
		if (!cpuperf_domains[index].control)
			continue;
		intent = &cpuperf_domains[index].intent;
		intent->strategy = strategy->value;
		intent->speed = 0U;
		intent->noise = 0U;

		/* Takes the manual axes only for the manual strategy. */
		if (strategy->value == KERN_THERMAL_STRATEGY_MANUAL) {
			intent->speed = strategy->speed;
			intent->noise = strategy->noise;
		}

		/* Ends any direct setting the domain carried. */
		intent->direct_fields = 0U;
		intent->direct_permille = false;
		intent->governor_desired = KERN_CPUPERF_UNKNOWN;
		cpuperf_domain_post(index, tickets);
	}

	/* Clears the direct-setting mark and makes sure the governor runs. */
	atomic_store_release(&cpuperf_custom, 0U);
	cpuperf_governor_start();

	mutex_unlock(&cpuperf_mutex);

	/* Waits for the CPUs to confirm, without the mutex. */
	deadline = cpuperf_deadline();
	apply_error = 0;
	error = 0;
	for (index = 0; error == 0 && index < cpuperf_domain_total; index++) {
		/* Skips a domain with nothing to control. */
		if (!cpuperf_domains[index].control)
			continue;
		error = cpuperf_domain_wait(index, tickets, deadline, &apply_error);
	}

	/* Releases the tickets. */
	kern_free(tickets);

	/*
	 * A CPU that did not confirm in time keeps the request in its box and
	 * applies it when it next takes a notification, so this is not a
	 * failure: the thermal core is told it is unconfirmed.
	 */
	if (error == ETIMEDOUT) {
		*flags |= KERN_THERMAL_APPLY_UNCONFIRMED;
		return 0;
	}

	/* Reports a request the provider refused. */
	if (apply_error != 0)
		return apply_error;

	/*
	 * Measures the highest rates once the control has started, which the
	 * first confirmed strategy just did: the measurement needs the CPUs
	 * taking requests.
	 */
	measure = false;
	mutex_lock(&cpuperf_mutex);

	/* Claims the measurement for this strategy when nobody has run it. */
	if (!cpuperf_measured && provider->measure != NULL) {
		cpuperf_measured = true;
		measure = true;
	}

	mutex_unlock(&cpuperf_mutex);

	/* Measures the highest rates, without the mutex. */
	if (measure)
		cpuperf_measure_all();

	/* Succeeded: every CPU runs with the strategy. */
	return 0;
}

/*
 * Sets the thermal ceiling of one domain or of every domain.
 *
 * Runs in the thermal core's poll thread, so it posts the new requests
 * and does not wait for them.
 */
static int
cpuperf_receiver_limit(
	void *context,
	uint32_t domain,
	uint32_t permille)
{
	const struct kern_cpuperf_provider_ops *provider;
	uint64_t *tickets;
	uint32_t index;

	UNUSED_PARAMETER(context);

	/* Refuses a ceiling out of range. */
	if (permille > CPUPERF_PERMILLE_MAX)
		return EINVAL;

	/* Has nothing to act on without a provider that can write a request. */
	provider = cpuperf_ops();
	if (provider == NULL)
		return EOPNOTSUPP;
	if (provider->apply == NULL)
		return EOPNOTSUPP;

	/* Allocates tickets the posts fill and nobody waits on. */
	tickets = kern_calloc(cpuperf_cpu_count, sizeof(*tickets));
	if (tickets == NULL)
		return ENOMEM;

	/* Holds the domains still while the ceiling is stored. */
	mutex_lock(&cpuperf_mutex);

	/* Refuses a domain that does not exist. */
	if (domain != KERN_THERMAL_DOMAIN_ALL && domain >= cpuperf_domain_total) {
		mutex_unlock(&cpuperf_mutex);
		kern_free(tickets);
		return EINVAL;
	}

	/* Stores the ceiling in the domains it reaches and posts it. */
	for (index = 0; index < cpuperf_domain_total; index++) {
		/* Skips a domain the ceiling does not name. */
		if (domain != KERN_THERMAL_DOMAIN_ALL && index != domain)
			continue;

		/* Skips a domain with nothing to control. */
		if (!cpuperf_domains[index].control)
			continue;
		cpuperf_domains[index].intent.limit_permille = permille;
		cpuperf_domain_post(index, tickets);
	}

	mutex_unlock(&cpuperf_mutex);

	/* Releases the tickets. */
	kern_free(tickets);

	/* Succeeded: the ceiling is on its way to the CPUs. */
	return 0;
}

/*
 * Copies the hardware feedback; the thermal core's poll thread calls it
 * once a second.
 */
static void
cpuperf_receiver_poll(
	void *context)
{
	const struct kern_cpuperf_provider_ops *provider;
	uint32_t before;
	uint32_t after;

	UNUSED_PARAMETER(context);

	/* Does nothing without a provider that keeps feedback. */
	provider = cpuperf_ops();
	if (provider == NULL)
		return;
	if (provider->feedback == NULL)
		return;

	/* Copies every package's table and measures again on a change. */
	mutex_lock(&cpuperf_mutex);

	before = atomic_load_acquire(&cpuperf_generation_count);
	cpuperf_feedback_copy();
	after = atomic_load_acquire(&cpuperf_generation_count);

	mutex_unlock(&cpuperf_mutex);

	/* Tells the readers about a new table. */
	if (before != after)
		cpuperf_publish_change();
}

/* Reports, without a lock, whether a direct setting is in force. */
static bool
cpuperf_receiver_is_custom(
	void *context)
{
	uint32_t custom;

	UNUSED_PARAMETER(context);

	/* A direct setting is in force until the next strategy. */
	custom = atomic_load_acquire(&cpuperf_custom);
	if (custom != 0U)
		return true;

	/* No direct setting is in force. */
	return false;
}

/* Reports the provider to a reader that does not hold the mutex, or NULL. */
static const struct kern_cpuperf_provider_ops *
cpuperf_ops(void)
{
	uint32_t ready;

	/* Refuses before the framework is up. */
	if (cpuperf_cpu_count == 0U)
		return NULL;

	/* The flag orders the provider's tables before the pointer is used. */
	ready = atomic_load_acquire(&cpuperf_provider_ready);
	if (ready == 0U)
		return NULL;

	/* Reports the registered provider. */
	return cpuperf_provider;
}

/* Gives an intent the values that ask for nothing. */
static void
cpuperf_intent_reset(
	struct cpuperf_intent *intent)
{
	/* The firmware's settings, no direct field, no ceiling, no governor step. */
	intent->strategy = KERN_THERMAL_STRATEGY_FIRMWARE;
	intent->speed = 0U;
	intent->noise = 0U;
	intent->direct_fields = 0U;
	intent->direct_permille = false;
	intent->direct_minimum = 0U;
	intent->direct_maximum = 0U;
	intent->direct_desired = 0U;
	intent->direct_preference = 0U;
	intent->limit_permille = KERN_THERMAL_LIMIT_NONE;
	intent->governor_desired = KERN_CPUPERF_UNKNOWN;
	intent->measuring = false;
}

/* Describes a CPU of a machine without a provider: its own core of package 0. */
static void
cpuperf_topology_default(
	kern_cpu_id_t cpu,
	struct kern_cpuperf_topology *topology)
{
	/* One core per CPU, all in one package, of an unknown kind. */
	topology->package = 0U;
	topology->die = 0U;
	topology->module = cpu;
	topology->core = cpu;
	topology->thread = 0U;
	topology->core_class = KERN_CPUPERF_CORE_UNKNOWN;
	topology->core_type = 0U;
	topology->firmware_id = KERN_CPUPERF_UNKNOWN;
}

/* Gives levels the values of a CPU that cannot be controlled. */
static void
cpuperf_levels_none(
	struct kern_cpuperf_levels *levels)
{
	uint32_t index;

	/* No kind, no sharing, no anchors and no steps. */
	levels->kind = 0U;
	levels->share = KERN_CPUPERF_SHARE_THREAD;
	levels->lowest = KERN_CPUPERF_UNKNOWN;
	levels->efficient = KERN_CPUPERF_UNKNOWN;
	levels->guaranteed = KERN_CPUPERF_UNKNOWN;
	levels->highest = KERN_CPUPERF_UNKNOWN;
	levels->step_count = 0U;

	/* Empties the list of steps. */
	for (index = 0; index < KERN_CPUPERF_STEP_MAX; index++) {
		levels->steps[index] = 0U;
		levels->step_mhz[index] = 0U;
	}

	/* Leaves the rate and the voltage group unknown. */
	levels->highest_mhz = 0U;
	levels->voltage_group = KERN_CPUPERF_UNKNOWN;
	levels->firmware_id = KERN_CPUPERF_UNKNOWN;
}

/* Reports whether all four anchors of some levels are known. */
static bool
cpuperf_anchors_known(
	const struct kern_cpuperf_levels *levels)
{
	/* Any unknown anchor leaves no range to compute against. */
	if (levels->lowest == KERN_CPUPERF_UNKNOWN)
		return false;
	if (levels->efficient == KERN_CPUPERF_UNKNOWN)
		return false;
	if (levels->guaranteed == KERN_CPUPERF_UNKNOWN)
		return false;
	if (levels->highest == KERN_CPUPERF_UNKNOWN)
		return false;

	/* Succeeded: all four anchors are known. */
	return true;
}

/* Reports whether all four anchors of some levels are unknown. */
static bool
cpuperf_anchors_unknown(
	const struct kern_cpuperf_levels *levels)
{
	/* Any known anchor means the provider gave some. */
	if (levels->lowest != KERN_CPUPERF_UNKNOWN)
		return false;
	if (levels->efficient != KERN_CPUPERF_UNKNOWN)
		return false;
	if (levels->guaranteed != KERN_CPUPERF_UNKNOWN)
		return false;
	if (levels->highest != KERN_CPUPERF_UNKNOWN)
		return false;

	/* Succeeded: all four anchors are unknown. */
	return true;
}

/*
 * Checks a provider's levels and pulls the inner anchors into the range.
 *
 * Reports whether the CPU can be controlled.  A RANGE CPU whose anchors
 * are all unknown can: it learns them when its control starts.
 */
static bool
cpuperf_levels_usable(
	struct kern_cpuperf_levels *levels)
{
	uint32_t index;
	bool known;
	bool unknown;

	/* Refuses a kind this file does not know. */
	if (levels->kind != KERN_CPUPERF_LEVELS_RANGE &&
	    levels->kind != KERN_CPUPERF_LEVELS_STEPS)
		return false;

	/* A sharing this file does not know is treated as none. */
	if (levels->share < KERN_CPUPERF_SHARE_THREAD ||
	    levels->share > KERN_CPUPERF_SHARE_PACKAGE)
		levels->share = KERN_CPUPERF_SHARE_THREAD;

	/* Takes the ends of a list from the list itself. */
	if (levels->kind == KERN_CPUPERF_LEVELS_STEPS) {
		/* Refuses steps that are not a short ascending list. */
		if (levels->step_count == 0U || levels->step_count > KERN_CPUPERF_STEP_MAX)
			return false;

		/* Refuses a step that does not rise above the one before it. */
		for (index = 1; index < levels->step_count; index++) {
			if (levels->steps[index] <= levels->steps[index - 1U])
				return false;
		}

		/* Takes the ends of the list as the lowest and the highest level. */
		levels->lowest = levels->steps[0];
		levels->highest = levels->steps[levels->step_count - 1U];
	}

	/* Accepts a RANGE CPU that will report its anchors once started. */
	unknown = cpuperf_anchors_unknown(levels);
	if (unknown && levels->kind == KERN_CPUPERF_LEVELS_RANGE)
		return true;

	/* Refuses levels with some anchors missing. */
	known = cpuperf_anchors_known(levels);
	if (!known)
		return false;

	/* Refuses an empty range. */
	if (levels->lowest > levels->highest)
		return false;

	/* Pulls the guaranteed anchor into the range. */
	if (levels->guaranteed < levels->lowest)
		levels->guaranteed = levels->lowest;
	if (levels->guaranteed > levels->highest)
		levels->guaranteed = levels->highest;

	/* Pulls the efficient anchor into the range. */
	if (levels->efficient < levels->lowest)
		levels->efficient = levels->lowest;
	if (levels->efficient > levels->highest)
		levels->efficient = levels->highest;

	/* Lands the inner anchors of a list on steps. */
	if (levels->kind == KERN_CPUPERF_LEVELS_STEPS) {
		levels->guaranteed = cpuperf_step_floor(levels, levels->guaranteed);
		levels->efficient = cpuperf_step_floor(levels, levels->efficient);
	}

	/* Succeeded: the CPU can be controlled with these levels. */
	return true;
}

/* Reports whether two CPUs share one frequency at the given sharing. */
static bool
cpuperf_shares(
	const struct kern_cpuperf_topology *first,
	const struct kern_cpuperf_topology *other,
	uint32_t share)
{
	/* Every sharing beyond a thread stays inside one package. */
	if (share == KERN_CPUPERF_SHARE_THREAD)
		return false;
	if (first->package != other->package)
		return false;
	if (share == KERN_CPUPERF_SHARE_PACKAGE)
		return true;

	/* A module and a core stay inside one die. */
	if (first->die != other->die)
		return false;
	if (share == KERN_CPUPERF_SHARE_MODULE && first->module == other->module)
		return true;
	if (share == KERN_CPUPERF_SHARE_CORE && first->core == other->core)
		return true;

	/* The two CPUs do not share a frequency. */
	return false;
}

/*
 * Groups the CPUs into domains, in CPU order, and tells each CPU's box
 * its domain.  Called with the mutex held.
 *
 * A CPU joins the first domain whose first member it shares a frequency
 * with at the same sharing, the same kind of levels and the same
 * controllability; otherwise it starts a domain.  Each new domain takes
 * the intent its first member carried.  Moves the generation.
 */
static void
cpuperf_domains_build(void)
{
	struct cpuperf_domain *entry;
	struct cpuperf_box *first;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	uint32_t index;
	uint32_t word;
	bool joined;

	/* Forgets the old domains. */
	cpuperf_domain_total = 0U;

	/* Places every CPU in the first domain it shares with. */
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		box = &cpuperf_boxes[cpu];

		/* Looks for a domain whose first member shares with this CPU. */
		joined = false;
		for (index = 0; index < cpuperf_domain_total; index++) {
			entry = &cpuperf_domains[index];
			first = &cpuperf_boxes[entry->first_cpu];

			/* Skips a domain whose first member differs in sharing, kind or control. */
			if (first->levels.share != box->levels.share)
				continue;
			if (first->levels.kind != box->levels.kind)
				continue;
			if (first->control != box->control)
				continue;
			joined = cpuperf_shares(&first->topology, &box->topology, box->levels.share);

			/* Joins the domain whose first member shares with this CPU. */
			if (joined) {
				cpuperf_domain_add(index, cpu);
				break;
			}
		}

		/* Starts a domain with this CPU as its first member. */
		if (!joined) {
			index = cpuperf_domain_total;
			entry = &cpuperf_domains[index];

			/* Empties the new domain's member mask. */
			for (word = 0; word < KERN_CPU_MASK_WORDS; word++)
				entry->cpus[word] = 0U;

			/* Starts the new domain empty and gives it its first member's state. */
			entry->cpu_count = 0U;
			entry->first_cpu = cpu;
			entry->levels = box->levels;
			entry->control = box->control;
			entry->intent = box->intent;
			cpuperf_domain_total++;
			cpuperf_domain_add(index, cpu);
		}

		/* Tells the box which domain it is in. */
		enabled = spin_lock_irqsave(&box->lock);

		box->domain = index;

		spin_unlock_irqrestore(&box->lock, enabled);
	}

	/* Moves the generation for the readers. */
	(void)atomic_fetch_add_relaxed(&cpuperf_generation_count, 1U);
}

/* Adds a CPU to a domain's members. */
static void
cpuperf_domain_add(
	uint32_t domain,
	kern_cpu_id_t cpu)
{
	struct cpuperf_domain *entry;

	/* Sets the member's bit and counts it. */
	entry = &cpuperf_domains[domain];
	entry->cpus[cpu / 64U] |= (uint64_t)1U << (cpu % 64U);
	entry->cpu_count++;
}

/* Reports whether a CPU is a member of a domain. */
static bool
cpuperf_domain_has(
	const struct cpuperf_domain *domain,
	kern_cpu_id_t cpu)
{
	/* Tests the member's bit. */
	if ((domain->cpus[cpu / 64U] & ((uint64_t)1U << (cpu % 64U))) != 0U)
		return true;

	/* The CPU is not a member. */
	return false;
}

/*
 * Posts work, and with it the latest intent when one is given, in a CPU's
 * box and has the CPU serve it.
 *
 * Reports the ticket the poster waits for.
 */
static uint64_t
cpuperf_post(
	kern_cpu_id_t cpu,
	uint32_t work,
	const struct cpuperf_intent *intent)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	uint64_t ticket;

	/* Leaves the work and the intent for the CPU; the latest intent wins. */
	box = &cpuperf_boxes[cpu];
	enabled = spin_lock_irqsave(&box->lock);

	box->work |= work;

	/* Replaces the box's intent with the newest one. */
	if (intent != NULL)
		box->intent = *intent;

	/* Counts the post; the poster waits until the CPU has served this count. */
	box->posted++;
	ticket = box->posted;

	spin_unlock_irqrestore(&box->lock, enabled);

	/* Marks the box for the handler, after the work is in it. */
	atomic_store_release(&box->pending, 1U);

	/* Brings the CPU to its box. */
	cpuperf_deliver(cpu);

	/* Reports the ticket of this post. */
	return ticket;
}

/*
 * Has a CPU serve its box: directly when it is the CPU the caller runs
 * on, otherwise through a cross-CPU notification.
 *
 * A notification that cannot be sent leaves the work in the box, where
 * the CPU finds it at its next notification of any kind.
 */
static void
cpuperf_deliver(
	kern_cpu_id_t cpu)
{
	bool enabled;
	int status;
	hal_cpu_id_t current;

	/*
	 * Serves the box here when the caller runs on the CPU.  Interrupts
	 * stay masked from the test to the end, so the caller cannot move.
	 */
	enabled = hal_irq_disable();
	current = hal_cpu_current();
	if (current == cpu) {
		kern_cpuperf_notify(cpu);

		/* Restores the interrupt state the caller had. */
		if (enabled)
			hal_irq_enable();

		/* Succeeded: the box was served on this CPU, so no notification is needed. */
		return;
	}

	/* Restores the interrupt state the caller had. */
	if (enabled)
		hal_irq_enable();

	/* Notifies the other CPU. */
	status = hal_cpu_notify(cpu);
	if (status != HAL_OK)
		kern_logf("cpuperf: cpu %u not notified (%d); its work waits\n", cpu, status);
}

/*
 * Waits until a CPU has served the post of a ticket, or until a deadline.
 *
 * Reports 0 or ETIMEDOUT.
 */
static int
cpuperf_wait(
	kern_cpu_id_t cpu,
	uint64_t ticket,
	uint64_t deadline)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	uint64_t observed;
	uint64_t now;
	int error;

	/* Sleeps on the box until the CPU has served the ticket. */
	box = &cpuperf_boxes[cpu];
	error = 0;
	enabled = spin_lock_irqsave(&box->lock);

	while (box->completed < ticket) {
		now = sched_ticks();

		/* Gives up once the deadline has passed. */
		if (now >= deadline) {
			error = ETIMEDOUT;
			break;
		}

		/* Sleeps until the CPU serves the box or the deadline passes. */
		observed = waitq_sequence(&box->done);
		(void)waitq_sleep(&box->done, &box->lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&box->lock, enabled);

	/* Reports a CPU that did not answer in time. */
	if (error != 0)
		return error;

	/* Succeeded: the CPU served the post. */
	return 0;
}

/* Reports the tick by which a CPU must have confirmed work posted now. */
static uint64_t
cpuperf_deadline(void)
{
	uint64_t deadline;

	/* The confirmation time from now. */
	deadline = sched_ticks() + kern_ms_to_ticks(CPUPERF_CONFIRM_MS);

	/* Reports the deadline. */
	return deadline;
}

/* Sleeps the calling thread for about the given time. */
static void
cpuperf_sleep_ms(
	uint32_t milliseconds)
{
	struct wait_queue queue;
	struct spinlock lock;
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;

	/* Sleeps on a queue nobody wakes until the deadline passes. */
	waitq_init(&queue, "cpuperf-sleep");
	spin_init(&lock, LOCK_RANK_DEVICE, "cpuperf-sleep");
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);
	enabled = spin_lock_irqsave(&lock);

	for (;;) {
		now = sched_ticks();

		/* Wakes once the deadline has passed. */
		if (now >= deadline)
			break;
		observed = waitq_sequence(&queue);
		(void)waitq_sleep(&queue, &lock, observed, deadline, 0U);
	}

	spin_unlock_irqrestore(&lock, enabled);
}

/*
 * Reports a CPU of a package: the calling CPU when it belongs to it,
 * otherwise the package's first CPU, or KERN_CPUPERF_UNKNOWN when no CPU
 * does.  The caller may have moved by the time it posts; that only costs
 * a notification.
 */
static kern_cpu_id_t
cpuperf_package_cpu(
	uint32_t package)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t current;
	kern_cpu_id_t first;
	uint32_t member;

	/* Prefers the CPU the caller runs on. */
	current = kern_cpu_current();
	if (current < cpuperf_cpu_count) {
		box = &cpuperf_boxes[current];
		enabled = spin_lock_irqsave(&box->lock);

		member = box->topology.package;

		spin_unlock_irqrestore(&box->lock, enabled);

		/* Takes the calling CPU when it belongs to the package. */
		if (member == package)
			return current;
	}

	/* Falls back to the package's first CPU, or to none. */
	first = cpuperf_package_first(package);
	return first;
}

/* Finds the first CPU of a package, or KERN_CPUPERF_UNKNOWN when no CPU belongs to it. */
static kern_cpu_id_t
cpuperf_package_first(
	uint32_t package)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	uint32_t member;

	/* Walks the CPUs in order until one is in the package. */
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		member = box->topology.package;

		spin_unlock_irqrestore(&box->lock, enabled);

		/* Takes the first CPU that belongs to the package. */
		if (member == package)
			return cpu;
	}

	/* No CPU belongs to the package. */
	return KERN_CPUPERF_UNKNOWN;
}

/* Posts a domain's intent to each of its members.  Called with the mutex held. */
static void
cpuperf_domain_post(
	uint32_t domain,
	uint64_t *tickets)
{
	struct cpuperf_domain *entry;
	kern_cpu_id_t cpu;
	bool member;

	/* Leaves the same intent with every member. */
	entry = &cpuperf_domains[domain];
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		/* Skips a CPU that is not a member. */
		member = cpuperf_domain_has(entry, cpu);
		if (!member)
			continue;
		tickets[cpu] = cpuperf_post(cpu, CPUPERF_WORK_APPLY, &entry->intent);
	}
}

/*
 * Waits for every member of a domain to confirm its post.
 *
 * Reports 0 or ETIMEDOUT, and the first refusal of the provider in
 * *apply_error.
 */
static int
cpuperf_domain_wait(
	uint32_t domain,
	const uint64_t *tickets,
	uint64_t deadline,
	int *apply_error)
{
	struct cpuperf_domain *entry;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	int error;
	bool member;

	/* Waits for each member in turn against the one deadline. */
	entry = &cpuperf_domains[domain];
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		/* Skips a CPU that is not a member. */
		member = cpuperf_domain_has(entry, cpu);
		if (!member)
			continue;

		/* Reports a member that did not confirm in time. */
		error = cpuperf_wait(cpu, tickets[cpu], deadline);
		if (error != 0)
			return error;

		/* Keeps the first refusal of the provider. */
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		/* Remembers the first refusal only. */
		if (*apply_error == 0)
			*apply_error = box->apply_error;

		spin_unlock_irqrestore(&box->lock, enabled);
	}

	/* Succeeded: every member confirmed. */
	return 0;
}

/*
 * Does the posted work on the current CPU, with the box locked and
 * interrupts masked.
 */
static void
cpuperf_run(
	struct cpuperf_box *box,
	kern_cpu_id_t cpu,
	uint32_t work)
{
	const struct kern_cpuperf_provider_ops *provider;

	/* Takes the provider the box lock keeps from changing. */
	provider = cpuperf_provider;

	/* Reads where this CPU sits in the machine. */
	if ((work & CPUPERF_WORK_TOPOLOGY) != 0U) {
		box->topology_error = provider->topology(cpu, &box->topology);

		/* Falls back to the default place when the provider cannot say. */
		if (box->topology_error != 0)
			cpuperf_topology_default(cpu, &box->topology);
	}

	/* Samples the CPU for a reader. */
	if ((work & CPUPERF_WORK_SAMPLE) != 0U &&
	    (work & CPUPERF_WORK_APPLY) == 0U &&
	    provider->sample != NULL) {
		box->sample.flags = 0U;
		provider->sample(cpu, &box->sample);

		/* Takes the fresh anchors into the levels. */
		if ((box->sample.flags & KERN_CPUPERF_SAMPLE_CAPABILITIES) != 0U)
			cpuperf_caps_refresh(&box->levels, &box->sample);
	}

	/* Applies the intent. */
	if ((work & CPUPERF_WORK_APPLY) != 0U)
		cpuperf_run_apply(box, provider, cpu);

	/* Marks the start of a measurement of the delivered rate. */
	if ((work & CPUPERF_WORK_MEASURE_BEGIN) != 0U) {
		box->measure_error = EOPNOTSUPP;

		/* Starts the measurement when the provider can. */
		if (provider->measure != NULL)
			box->measure_error = provider->measure(cpu, true, &box->measured_mhz);
	}

	/* Ends the measurement and keeps the rate. */
	if ((work & CPUPERF_WORK_MEASURE_END) != 0U) {
		box->measure_error = EOPNOTSUPP;

		/* Ends the measurement when the provider can. */
		if (provider->measure != NULL)
			box->measure_error = provider->measure(cpu, false, &box->measured_mhz);

		/* Drops the rate of a measurement that failed. */
		if (box->measure_error != 0)
			box->measured_mhz = 0U;
	}

	/* Copies the hardware feedback of the package. */
	if ((work & CPUPERF_WORK_FEEDBACK) != 0U) {
		box->feedback_changed = false;
		box->feedback_error = EOPNOTSUPP;

		/* Copies the feedback when the provider keeps one. */
		if (provider->feedback != NULL)
			box->feedback_error = provider->feedback(cpu, &box->feedback_changed);
	}

	/* Reads the residency of the core. */
	if ((work & CPUPERF_WORK_IDLE_CORE) != 0U) {
		box->idle_error = EOPNOTSUPP;

		/* Reads the counters when the provider has them. */
		if (provider->idle_core != NULL)
			box->idle_error = provider->idle_core(cpu, &box->idle);
	}

	/* Reads the residency of the package. */
	if ((work & CPUPERF_WORK_IDLE_PACKAGE) != 0U) {
		box->idle_error = EOPNOTSUPP;

		/* Reads the counters when the provider has them. */
		if (provider->idle_package != NULL)
			box->idle_error = provider->idle_package(cpu, &box->idle);
	}
}

/*
 * Applies the box's intent on the current CPU: starts the control before
 * the first request, samples the anchors, works out the request and
 * writes it.  A refused request is not retried until the next post.
 */
static void
cpuperf_run_apply(
	struct cpuperf_box *box,
	const struct kern_cpuperf_provider_ops *provider,
	kern_cpu_id_t cpu)
{
	struct kern_cpuperf_sample started;
	struct kern_cpuperf_request request;
	uint32_t request_flags;
	bool known;
	int error;

	/* A CPU that cannot be controlled takes no request. */
	box->apply_error = 0;
	if (!box->control)
		return;
	if (provider->apply == NULL)
		return;

	/* Works out whether the intent asks for anything at all. */
	request_flags = cpuperf_compute(&box->intent, &box->levels, &request);
	if ((request_flags & CPUPERF_REQUEST_WRITE) == 0U)
		return;

	/* Starts the control before the CPU's first request; its anchors come with it. */
	if (!box->started && provider->start != NULL) {
		started.flags = 0U;
		error = provider->start(cpu, &started);

		/* Reports a control that would not start. */
		if (error != 0) {
			box->apply_error = error;
			return;
		}

		/* Takes the anchors the start reported into the levels. */
		if ((started.flags & KERN_CPUPERF_SAMPLE_CAPABILITIES) != 0U)
			cpuperf_caps_refresh(&box->levels, &started);
	}

	/* Remembers that the control has started. */
	box->started = true;

	/* Samples the CPU, which also refreshes its anchors. */
	if (provider->sample != NULL) {
		box->sample.flags = 0U;
		provider->sample(cpu, &box->sample);

		/* Takes the fresh anchors into the levels. */
		if ((box->sample.flags & KERN_CPUPERF_SAMPLE_CAPABILITIES) != 0U)
			cpuperf_caps_refresh(&box->levels, &box->sample);
	}

	/* Refuses to write a request without anchors to measure it against. */
	known = cpuperf_anchors_known(&box->levels);
	if (!known) {
		box->apply_error = EAGAIN;
		return;
	}

	/* Writes the request against the anchors just read. */
	request_flags = cpuperf_compute(&box->intent, &box->levels, &request);
	box->apply_error = provider->apply(cpu, &request);

	/* Keeps the request the CPU accepted. */
	if (box->apply_error == 0) {
		box->request = request;
		box->request_flags = request_flags;
	}
}

/*
 * Takes the anchors of a fresh sample into a RANGE CPU's levels.
 *
 * A sample that does not make a usable, fully known range is ignored.
 */
static void
cpuperf_caps_refresh(
	struct kern_cpuperf_levels *levels,
	const struct kern_cpuperf_sample *sample)
{
	struct kern_cpuperf_levels fresh;
	bool usable;
	bool known;

	/* A list does not move at run time. */
	if (levels->kind != KERN_CPUPERF_LEVELS_RANGE)
		return;

	/* Checks the sample's anchors before they replace the old ones. */
	fresh = *levels;
	fresh.lowest = sample->lowest;
	fresh.efficient = sample->efficient;
	fresh.guaranteed = sample->guaranteed;
	fresh.highest = sample->highest;
	known = cpuperf_anchors_known(&fresh);
	if (!known)
		return;

	/* Takes the sample's anchors when they make a usable range. */
	usable = cpuperf_levels_usable(&fresh);
	if (usable)
		*levels = fresh;
}

/*
 * Turns an intent into the request it asks of a CPU with these levels.
 *
 * Reports CPUPERF_REQUEST_* flags; without CPUPERF_REQUEST_WRITE there is
 * nothing to write and the firmware's settings stay.  With unknown
 * anchors the request is only the flag and must not be written.
 */
static uint32_t
cpuperf_compute(
	const struct cpuperf_intent *intent,
	const struct kern_cpuperf_levels *levels,
	struct kern_cpuperf_request *request)
{
	uint32_t flags;
	bool known;

	/* Starts from the whole range and no preference. */
	request->minimum = levels->lowest;
	request->maximum = levels->highest;
	request->desired = 0U;
	request->preference = KERN_CPUPERF_UNKNOWN;

	/* Asks for nothing while every part of the intent is neutral. */
	if (intent->strategy == KERN_THERMAL_STRATEGY_FIRMWARE &&
	    intent->direct_fields == 0U &&
	    intent->limit_permille >= KERN_THERMAL_LIMIT_NONE &&
	    !intent->measuring)
		return 0U;

	/* Has nothing to compute against without the anchors. */
	flags = CPUPERF_REQUEST_WRITE;
	known = cpuperf_anchors_known(levels);
	if (!known)
		return flags;

	/* Runs a CPU being measured at its highest level and nothing else. */
	if (intent->measuring) {
		request->minimum = levels->highest;
		request->maximum = levels->highest;
		request->desired = levels->highest;
		request->preference = CPUPERF_PREFERENCE_PERFORMANCE;
		if (levels->kind == KERN_CPUPERF_LEVELS_STEPS)
			request->preference = KERN_CPUPERF_UNKNOWN;
		return flags;
	}

	/* Maps the strategy onto the kind of levels. */
	if (levels->kind == KERN_CPUPERF_LEVELS_STEPS)
		flags |= cpuperf_compute_steps(intent, levels, request);
	else
		cpuperf_compute_range(intent, levels, request);

	/* Lets a direct setting replace what the strategy chose. */
	cpuperf_compute_direct(intent, levels, request, &flags);

	/* Fits the bounds to the levels and the ceiling. */
	cpuperf_compute_bounds(intent, levels, request, &flags);

	/* Reports what the request is. */
	return flags;
}

/*
 * Maps a strategy onto a RANGE CPU: bounds for the processor's own choice
 * and the energy preference.
 */
static void
cpuperf_compute_range(
	const struct cpuperf_intent *intent,
	const struct kern_cpuperf_levels *levels,
	struct kern_cpuperf_request *request)
{
	uint32_t sustained;
	uint32_t speed_bound;
	uint32_t noise_bound;

	/*
	 * The higher of guaranteed and efficient: the most the CPU sustains
	 * that it also runs at efficiently.  A request below efficient may
	 * not be honoured, so no strategy's maximum goes below it.
	 */
	sustained = levels->guaranteed;
	if (levels->efficient > sustained)
		sustained = levels->efficient;

	/* Picks the bounds and the preference of each strategy. */
	switch (intent->strategy) {
	case KERN_THERMAL_STRATEGY_PERFORMANCE:
		/*
		 * A minimum of the guaranteed level keeps the CPU from dipping
		 * while busy; above guaranteed the processor reports it cannot
		 * hold the minimum, so it is not higher.  The guaranteed level
		 * is the one the CPU reported just now.
		 */
		request->minimum = levels->guaranteed;
		request->maximum = levels->highest;
		request->preference = CPUPERF_PREFERENCE_PERFORMANCE;
		break;

	case KERN_THERMAL_STRATEGY_BALANCED:
		request->minimum = levels->lowest;
		request->maximum = levels->highest;
		request->preference = CPUPERF_PREFERENCE_BALANCED;
		break;

	case KERN_THERMAL_STRATEGY_POWERSAVE:
		request->minimum = levels->lowest;
		request->maximum = sustained;
		request->preference = CPUPERF_PREFERENCE_POWERSAVE;
		break;

	case KERN_THERMAL_STRATEGY_MANUAL:
		/*
		 * Speed spreads the maximum from efficient to highest and the
		 * preference from energy to performance.  Noise spreads a second
		 * bound from the sustained level to highest, since a faster CPU
		 * draws more power and the fan follows; the lower bound wins.
		 */
		speed_bound = cpuperf_scale(levels->efficient, levels->highest,
					    intent->speed, KERN_THERMAL_AXIS_MAX);
		noise_bound = cpuperf_scale(sustained, levels->highest,
					    intent->noise, KERN_THERMAL_AXIS_MAX);
		request->minimum = levels->lowest;
		request->maximum = speed_bound;

		/* Lets the lower of the two bounds win. */
		if (noise_bound < request->maximum)
			request->maximum = noise_bound;

		/* Spreads the preference from energy to performance with the speed. */
		request->preference = CPUPERF_PREFERENCE_MAX * (KERN_THERMAL_AXIS_MAX - intent->speed) / KERN_THERMAL_AXIS_MAX;
		break;

	default:
		/* FIRMWARE with a ceiling or a direct setting: the full range, no preference. */
		request->minimum = levels->lowest;
		request->maximum = levels->highest;
		break;
	}
}

/*
 * Maps a strategy onto a STEPS CPU: the steps the governor may pick from
 * and the step it picked.
 *
 * Reports CPUPERF_REQUEST_GOVERNED when the governor picks desired.
 */
static uint32_t
cpuperf_compute_steps(
	const struct cpuperf_intent *intent,
	const struct kern_cpuperf_levels *levels,
	struct kern_cpuperf_request *request)
{
	uint32_t top;
	uint32_t flags;

	/* Picks the steps of each strategy. */
	flags = 0U;
	switch (intent->strategy) {
	case KERN_THERMAL_STRATEGY_PERFORMANCE:
		/* Always the top step. */
		request->minimum = levels->highest;
		request->maximum = levels->highest;
		request->desired = levels->highest;
		break;

	case KERN_THERMAL_STRATEGY_BALANCED:
		/* Every step, picked by load. */
		request->minimum = levels->lowest;
		request->maximum = levels->highest;
		flags |= CPUPERF_REQUEST_GOVERNED;
		break;

	case KERN_THERMAL_STRATEGY_POWERSAVE:
		/* The steps up to the highest sustained one, picked by load. */
		request->minimum = levels->lowest;
		request->maximum = levels->guaranteed;
		flags |= CPUPERF_REQUEST_GOVERNED;
		break;

	case KERN_THERMAL_STRATEGY_MANUAL:
		/*
		 * Noise decides whether the turbo steps are allowed at all, and
		 * speed how far up the allowed steps the governor may go.
		 */
		top = levels->guaranteed;

		/* Allows the turbo steps from the manual noise that permits them. */
		if (intent->noise >= CPUPERF_STEPS_TURBO_NOISE)
			top = levels->highest;

		/* Bounds the steps and leaves the choice to the governor. */
		request->minimum = levels->lowest;
		request->maximum = cpuperf_scale(levels->lowest, top,
						 intent->speed, KERN_THERMAL_AXIS_MAX);
		flags |= CPUPERF_REQUEST_GOVERNED;
		break;

	default:
		/* FIRMWARE with a ceiling or a direct setting: as high as allowed. */
		request->minimum = levels->lowest;
		request->maximum = levels->highest;
		break;
	}

	/* Gives a governed request the step the governor chose. */
	if ((flags & CPUPERF_REQUEST_GOVERNED) != 0U &&
	    intent->governor_desired != KERN_CPUPERF_UNKNOWN)
		request->desired = intent->governor_desired;

	/* Reports whether the governor picks the step. */
	return flags;
}

/*
 * Lets a direct setting replace the fields the strategy chose,
 * turning permille of the range into units first when the setting was
 * given for every domain.
 */
static void
cpuperf_compute_direct(
	const struct cpuperf_intent *intent,
	const struct kern_cpuperf_levels *levels,
	struct kern_cpuperf_request *request,
	uint32_t *flags)
{
	uint32_t value;

	/* Replaces the minimum. */
	if ((intent->direct_fields & KERN_CPUPERF_SET_MINIMUM) != 0U) {
		value = intent->direct_minimum;

		/* Turns a permille of the range into units. */
		if (intent->direct_permille)
			value = cpuperf_scale(levels->lowest, levels->highest, value, CPUPERF_PERMILLE_MAX);
		request->minimum = value;
	}

	/* Replaces the maximum. */
	if ((intent->direct_fields & KERN_CPUPERF_SET_MAXIMUM) != 0U) {
		value = intent->direct_maximum;

		/* Turns a permille of the range into units. */
		if (intent->direct_permille)
			value = cpuperf_scale(levels->lowest, levels->highest, value, CPUPERF_PERMILLE_MAX);
		request->maximum = value;
	}

	/* Replaces the preference, which is never a permille. */
	if ((intent->direct_fields & KERN_CPUPERF_SET_PREFERENCE) != 0U)
		request->preference = intent->direct_preference;

	/* Replaces desired, which then stays where the direct setting put it. */
	if ((intent->direct_fields & KERN_CPUPERF_SET_DESIRED) != 0U) {
		value = intent->direct_desired;

		/* Turns a permille of the range into units; 0 hands the choice back. */
		if (intent->direct_permille && value != 0U)
			value = cpuperf_scale(levels->lowest, levels->highest, value, CPUPERF_PERMILLE_MAX);

		/* Stores the desired value and takes the step out of the governor's hands. */
		request->desired = value;
		*flags &= ~CPUPERF_REQUEST_GOVERNED;
	}
}

/*
 * Fits a request to the levels: the bounds inside the range, the maximum
 * under the thermal ceiling, the minimum under the maximum, every value
 * of a list on a step and desired inside the bounds.
 */
static void
cpuperf_compute_bounds(
	const struct cpuperf_intent *intent,
	const struct kern_cpuperf_levels *levels,
	struct kern_cpuperf_request *request,
	uint32_t *flags)
{
	uint32_t ceiling;

	/* Keeps the maximum inside the levels. */
	if (request->maximum > levels->highest)
		request->maximum = levels->highest;
	if (request->maximum < levels->lowest)
		request->maximum = levels->lowest;

	/* Keeps the minimum inside the levels. */
	if (request->minimum < levels->lowest)
		request->minimum = levels->lowest;

	/* Cuts the maximum to the thermal ceiling. */
	if (intent->limit_permille < KERN_THERMAL_LIMIT_NONE) {
		ceiling = cpuperf_ceiling(levels, intent->limit_permille);

		/* Marks a maximum the ceiling cut. */
		if (request->maximum > ceiling) {
			request->maximum = ceiling;
			*flags |= CPUPERF_REQUEST_LIMITED;
		}
	}

	/* Keeps the minimum at or below the maximum. */
	if (request->minimum > request->maximum)
		request->minimum = request->maximum;

	/* Lands every bound of a list on a step. */
	if (levels->kind == KERN_CPUPERF_LEVELS_STEPS) {
		request->minimum = cpuperf_step_floor(levels, request->minimum);
		request->maximum = cpuperf_step_floor(levels, request->maximum);

		/* Lands an explicit desired value too. */
		if (request->desired != 0U)
			request->desired = cpuperf_step_floor(levels, request->desired);
	}

	/* Keeps an explicit desired value inside the bounds. */
	if (request->desired != 0U && request->desired < request->minimum)
		request->desired = request->minimum;

	/* Keeps desired from rising above the maximum. */
	if (request->desired > request->maximum)
		request->desired = request->maximum;

	/* A list has no choice of its own: desired is always a step. */
	if (levels->kind == KERN_CPUPERF_LEVELS_STEPS && request->desired == 0U)
		request->desired = request->maximum;

	/* A list has no energy preference. */
	if (levels->kind == KERN_CPUPERF_LEVELS_STEPS)
		request->preference = KERN_CPUPERF_UNKNOWN;
}

/*
 * Reports the ceiling, in units, that permille of a CPU's range is.
 *
 * The ceiling never goes below the guaranteed level, and for a list it
 * lands on a step.
 */
static uint32_t
cpuperf_ceiling(
	const struct kern_cpuperf_levels *levels,
	uint32_t permille)
{
	uint32_t ceiling;

	/* Scales the range and keeps the guaranteed level. */
	ceiling = cpuperf_scale(levels->lowest, levels->highest, permille, CPUPERF_PERMILLE_MAX);

	/* Never goes below the guaranteed level. */
	if (ceiling < levels->guaranteed)
		ceiling = levels->guaranteed;

	/* Lands the ceiling on a step of a list. */
	if (levels->kind == KERN_CPUPERF_LEVELS_STEPS)
		ceiling = cpuperf_step_floor(levels, ceiling);

	/* Reports the ceiling. */
	return ceiling;
}

/*
 * Reports the value part / whole of the way from low to high, rounded
 * down.  Units and parts are small, so the product fits 64 bits.
 */
static uint32_t
cpuperf_scale(
	uint32_t low,
	uint32_t high,
	uint32_t part,
	uint32_t whole)
{
	uint64_t span;

	/* A reversed range has no way to go. */
	if (high <= low)
		return low;

	/* Clamps the part to the whole. */
	if (part > whole)
		part = whole;

	/* Walks the share of the span from low. */
	span = (uint64_t)(high - low) * part / whole;

	/* Reports the value. */
	return low + (uint32_t)span;
}

/* Reports the highest step at or below a value, or the lowest step. */
static uint32_t
cpuperf_step_floor(
	const struct kern_cpuperf_levels *levels,
	uint32_t value)
{
	uint32_t index;

	/* Finds the step's place in the list. */
	index = cpuperf_step_index(levels, value);

	/* Reports the step. */
	return levels->steps[index];
}

/* Reports the index of the highest step at or below a value, or 0. */
static uint32_t
cpuperf_step_index(
	const struct kern_cpuperf_levels *levels,
	uint32_t value)
{
	uint32_t index;

	/* Walks up the list while the next step still fits. */
	index = 0U;
	while (index + 1U < levels->step_count && levels->steps[index + 1U] <= value)
		index++;

	/* Reports the index. */
	return index;
}

/* Reports whether the governor picks the step of a domain with this intent. */
static bool
cpuperf_governed(
	const struct cpuperf_intent *intent,
	const struct kern_cpuperf_levels *levels)
{
	/* Only a list needs the operating system to pick. */
	if (levels->kind != KERN_CPUPERF_LEVELS_STEPS)
		return false;

	/* A directly set desired step is not the governor's to change. */
	if ((intent->direct_fields & KERN_CPUPERF_SET_DESIRED) != 0U)
		return false;

	/* The strategies that leave the step to the load. */
	if (intent->strategy == KERN_THERMAL_STRATEGY_BALANCED)
		return true;
	if (intent->strategy == KERN_THERMAL_STRATEGY_POWERSAVE)
		return true;
	if (intent->strategy == KERN_THERMAL_STRATEGY_MANUAL)
		return true;

	/* The other strategies name the step themselves. */
	return false;
}

/*
 * Checks a direct setting against a domain's levels, or, for every domain,
 * its permille values.  Units are checked only against known anchors; the
 * CPU clamps what cannot be checked here.
 */
static int
cpuperf_setting_check(
	const struct kern_cpuperf_levels *levels,
	const struct kern_cpuperf_setting *setting,
	bool permille)
{
	uint32_t low;
	uint32_t high;
	bool known;

	/* The preference is 0..255 either way. */
	if ((setting->fields & KERN_CPUPERF_SET_PREFERENCE) != 0U &&
	    setting->preference > CPUPERF_PREFERENCE_MAX)
		return EINVAL;

	/* Picks the range the bounds must lie in. */
	low = 0U;
	high = CPUPERF_PERMILLE_MAX;
	known = cpuperf_anchors_known(levels);
	if (!permille && known) {
		low = levels->lowest;
		high = levels->highest;
	} else if (!permille) {
		high = KERN_CPUPERF_UNKNOWN - 1U;
	}

	/* Refuses a minimum outside the range. */
	if ((setting->fields & KERN_CPUPERF_SET_MINIMUM) != 0U &&
	    (setting->minimum < low || setting->minimum > high))
		return EINVAL;

	/* Refuses a maximum outside the range. */
	if ((setting->fields & KERN_CPUPERF_SET_MAXIMUM) != 0U &&
	    (setting->maximum < low || setting->maximum > high))
		return EINVAL;

	/* Refuses a minimum above the maximum set with it. */
	if ((setting->fields & KERN_CPUPERF_SET_MINIMUM) != 0U &&
	    (setting->fields & KERN_CPUPERF_SET_MAXIMUM) != 0U &&
	    setting->minimum > setting->maximum)
		return EINVAL;

	/* Refuses a desired value outside the range; 0 hands the choice back. */
	if ((setting->fields & KERN_CPUPERF_SET_DESIRED) != 0U &&
	    setting->desired != 0U &&
	    (setting->desired < low || setting->desired > high))
		return EINVAL;

	/* Succeeded: the setting fits the levels. */
	return 0;
}

/*
 * Stores a checked direct setting in a domain's intent, on top of the
 * fields set directly before.
 */
static void
cpuperf_setting_store(
	struct cpuperf_intent *intent,
	const struct kern_cpuperf_setting *setting,
	bool permille)
{
	/*
	 * A setting for every domain is kept in permille until each CPU turns
	 * it into units; a setting in the other form replaces the fields set
	 * before instead of mixing with them.
	 */
	if (permille != intent->direct_permille)
		intent->direct_fields = 0U;
	intent->direct_permille = permille;

	/* Stores the minimum when the setting names it. */
	if ((setting->fields & KERN_CPUPERF_SET_MINIMUM) != 0U)
		intent->direct_minimum = setting->minimum;

	/* Stores the maximum when the setting names it. */
	if ((setting->fields & KERN_CPUPERF_SET_MAXIMUM) != 0U)
		intent->direct_maximum = setting->maximum;

	/* Stores the desired value when the setting names it. */
	if ((setting->fields & KERN_CPUPERF_SET_DESIRED) != 0U)
		intent->direct_desired = setting->desired;

	/* Stores the preference when the setting names it. */
	if ((setting->fields & KERN_CPUPERF_SET_PREFERENCE) != 0U)
		intent->direct_preference = setting->preference;

	/* Remembers which fields were set directly. */
	intent->direct_fields |= setting->fields;
}

/*
 * Measures every CPU's relative performance by the way in force and
 * scales the fastest to 100.  Called with the mutex held.  Moves the
 * generation.
 *
 * When any CPU cannot be measured, every CPU's value is unknown: a scale
 * with a missing CPU could name the wrong one the fastest.
 */
static void
cpuperf_relative_refresh(void)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	uint32_t largest;
	uint32_t raw;
	uint32_t scaled;
	bool known;
	int error;

	/* Measures every CPU and finds the fastest. */
	provider = cpuperf_provider;
	known = false;
	if (provider != NULL)
		known = true;
	largest = 0U;
	for (cpu = 0; known && cpu < cpuperf_cpu_count; cpu++) {
		error = cpuperf_relative_raw(provider, cpu, &raw);

		/* Gives up the scale on a CPU that cannot be measured; keeps the largest otherwise. */
		if (error != 0)
			known = false;
		else if (raw > largest)
			largest = raw;
	}

	/* A machine whose fastest CPU measures 0 has no scale. */
	if (largest == 0U)
		known = false;

	/* Scales each CPU to the fastest, rounding to the nearest. */
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		scaled = KERN_CPUPERF_UNKNOWN;

		/* Leaves the value unknown unless the whole machine has a scale. */
		if (known) {
			error = cpuperf_relative_raw(provider, cpu, &raw);

			/* Leaves the value unknown for a CPU that cannot be measured. */
			if (error == 0)
				scaled = (uint32_t)(((uint64_t)raw * CPUPERF_RELATIVE_MAX + largest / 2U) / largest);
		}

		/* Stores the value in the CPU's box. */
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		box->relative = scaled;

		spin_unlock_irqrestore(&box->lock, enabled);
	}

	/* Moves the generation for the readers. */
	(void)atomic_fetch_add_relaxed(&cpuperf_generation_count, 1U);
}

/*
 * Reports one CPU's raw performance by the way in force.  Called with the
 * mutex held.
 *
 * FREQUENCY is the highest rate the framework measured; the other ways
 * are the provider's.
 */
static int
cpuperf_relative_raw(
	const struct kern_cpuperf_provider_ops *provider,
	kern_cpu_id_t cpu,
	uint32_t *raw)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	int error;

	/* Reads the measured rate for FREQUENCY. */
	if (cpuperf_relative_source == KERN_CPUPERF_RELATIVE_FREQUENCY) {
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		*raw = box->measured_mhz;

		spin_unlock_irqrestore(&box->lock, enabled);

		/* A CPU not measured yet has no value. */
		if (*raw == 0U)
			return ENODATA;

		/* Succeeded: the measured rate is the raw value. */
		return 0;
	}

	/* Refuses a way the provider does not offer. */
	if (provider->relative == NULL)
		return EOPNOTSUPP;
	if ((provider->relative_sources & cpuperf_relative_source) == 0U)
		return EOPNOTSUPP;

	/* Asks the provider. */
	error = provider->relative(cpu, cpuperf_relative_source, raw);
	if (error != 0)
		return error;

	/* Succeeded: the provider's raw value. */
	return 0;
}

/*
 * Copies every package's hardware feedback into the CPUs' boxes.  Called
 * with the mutex held.  Moves the generation when a table changed.
 *
 * Each package's table is copied on a CPU of that package, the calling
 * one when it belongs to it.  The waits are bounded and the mutex is held
 * through them, which only delays other changes of policy.
 */
static void
cpuperf_feedback_copy(void)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t target;
	kern_cpu_id_t cpu;
	kern_cpu_id_t first;
	uint64_t ticket;
	uint64_t deadline;
	uint32_t package;
	bool changed;
	bool moved;
	bool seen;
	int error;

	/* Does nothing without a provider that keeps feedback. */
	provider = cpuperf_provider;
	if (provider == NULL)
		return;
	if (provider->feedback == NULL)
		return;
	if (provider->feedback_read == NULL)
		return;

	/* Copies the table of each package once. */
	moved = false;
	for (first = 0; first < cpuperf_cpu_count; first++) {
		package = cpuperf_boxes[first].topology.package;

		/* Skips a package an earlier CPU stood for. */
		seen = false;
		for (cpu = 0; cpu < first; cpu++) {
			if (cpuperf_boxes[cpu].topology.package == package) {
				seen = true;
				break;
			}
		}

		/* Moves on when an earlier CPU already copied the package. */
		if (seen)
			continue;

		/* Picks a CPU of the package to copy on; a package without CPUs is skipped. */
		target = cpuperf_package_cpu(package);
		if (target == KERN_CPUPERF_UNKNOWN)
			continue;

		/* Copies the package's table on that CPU and skips a CPU that did not answer. */
		ticket = cpuperf_post(target, CPUPERF_WORK_FEEDBACK, NULL);
		deadline = cpuperf_deadline();
		error = cpuperf_wait(target, ticket, deadline);
		if (error != 0)
			continue;

		/* Takes the answer of the copy. */
		box = &cpuperf_boxes[target];
		enabled = spin_lock_irqsave(&box->lock);

		changed = box->feedback_changed;
		error = box->feedback_error;

		spin_unlock_irqrestore(&box->lock, enabled);

		/* Gives each CPU of the package its values from a new copy. */
		if (error == 0 && changed) {
			cpuperf_feedback_store(package, first);
			moved = true;
		}
	}

	/* Measures again when the feedback is the way in force. */
	if (moved && cpuperf_relative_source == KERN_CPUPERF_RELATIVE_FEEDBACK)
		cpuperf_relative_refresh();
	else if (moved)
		(void)atomic_fetch_add_relaxed(&cpuperf_generation_count, 1U);
}

/*
 * Gives each CPU of a package, from its first CPU on, its values from the
 * provider's latest feedback copy.  Called with the mutex held.
 */
static void
cpuperf_feedback_store(
	uint32_t package,
	kern_cpu_id_t first)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	uint32_t available;
	uint32_t efficiency;
	int error;

	/* Reads each CPU's row of the copy. */
	provider = cpuperf_provider;
	for (cpu = first; cpu < cpuperf_cpu_count; cpu++) {
		/* Skips a CPU of another package. */
		if (cpuperf_boxes[cpu].topology.package != package)
			continue;
		available = KERN_CPUPERF_UNKNOWN;
		efficiency = KERN_CPUPERF_UNKNOWN;
		error = provider->feedback_read(cpu, &available, &efficiency);

		/* Leaves the values unknown for a row the provider cannot read. */
		if (error != 0) {
			available = KERN_CPUPERF_UNKNOWN;
			efficiency = KERN_CPUPERF_UNKNOWN;
		}

		/* Stores the row in the CPU's box. */
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		box->available = available;
		box->efficiency = efficiency;

		spin_unlock_irqrestore(&box->lock, enabled);
	}
}

/*
 * Asks the provider for every CPU's levels and decides which CPUs can be
 * controlled.  Called with the mutex held.  Moves the generation.
 */
static void
cpuperf_describe_all(void)
{
	const struct kern_cpuperf_provider_ops *provider;
	struct kern_cpuperf_levels levels;
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	bool usable;
	bool unknown;
	int error;

	/* Describes each CPU in turn. */
	provider = cpuperf_provider;
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		cpuperf_levels_none(&levels);
		error = provider->describe(cpu, &levels);

		/* A CPU the provider cannot describe is shown but not controlled. */
		usable = false;
		if (error == 0)
			usable = cpuperf_levels_usable(&levels);

		/* Strips the kind and the sharing from a CPU that is not controlled. */
		if (!usable) {
			levels.kind = 0U;
			levels.share = KERN_CPUPERF_SHARE_THREAD;
		}

		/* Notes whether the provider gave no anchors. */
		unknown = cpuperf_anchors_unknown(&levels);

		/* Stores the description in the CPU's box. */
		box = &cpuperf_boxes[cpu];
		enabled = spin_lock_irqsave(&box->lock);

		/* Keeps anchors a running CPU already reported over unknown ones. */
		if (box->started && unknown && levels.kind == KERN_CPUPERF_LEVELS_RANGE) {
			levels.lowest = box->levels.lowest;
			levels.efficient = box->levels.efficient;
			levels.guaranteed = box->levels.guaranteed;
			levels.highest = box->levels.highest;
		}

		/* Stores the levels and the control that goes with them. */
		box->levels = levels;
		box->control = usable;

		/* Controls nothing when the provider cannot write a request. */
		if (provider->apply == NULL)
			box->control = false;

		/* Takes the firmware's name for the CPU when the provider gave one. */
		if (levels.firmware_id != KERN_CPUPERF_UNKNOWN)
			box->topology.firmware_id = levels.firmware_id;

		spin_unlock_irqrestore(&box->lock, enabled);
	}

	/* Moves the generation for the readers. */
	(void)atomic_fetch_add_relaxed(&cpuperf_generation_count, 1U);
}

/* Tells the readers of /dev/thermal that the CPUs changed.  Thread context only. */
static void
cpuperf_publish_change(void)
{
	uint32_t generation;

	/* Posts the event with the generation the readers compare. */
	generation = atomic_load_acquire(&cpuperf_generation_count);
	kern_thermal_event(KERN_THERMAL_EVENT_CPU, KERN_CPUPERF_ALL, (int64_t)generation);
}

/*
 * Measures the highest rate of one CPU of each kind and gives it to every
 * CPU of that kind, then measures the relative performance again.
 *
 * Thread context, without the mutex.  The CPU being measured runs its
 * domain at the highest level for the time of the measurement; the other
 * CPUs are not held idle, so a busy machine measures low.
 */
static void
cpuperf_measure_all(void)
{
	struct cpuperf_box *box;
	unsigned long enabled;
	kern_cpu_id_t cpu;
	kern_cpu_id_t other;
	uint32_t kind;
	uint32_t mhz;
	uint32_t measured;
	bool seen;
	int error;

	/* Measures the first controllable CPU of each kind. */
	measured = 0U;
	for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
		/* Skips a CPU that cannot be controlled. */
		box = &cpuperf_boxes[cpu];
		if (!box->control)
			continue;
		kind = box->topology.core_class;

		/* Skips a kind an earlier CPU stood for. */
		seen = false;
		for (other = 0; other < cpu; other++) {
			if (cpuperf_boxes[other].control &&
			    cpuperf_boxes[other].topology.core_class == kind) {
				seen = true;
				break;
			}
		}

		/* Moves on when an earlier CPU of the kind was measured. */
		if (seen)
			continue;

		/* Measures the CPU at its highest level. */
		mhz = 0U;
		error = cpuperf_measure_domain(cpu, &mhz);

		/* Reports a CPU that could not be measured and goes on to the next. */
		if (error != 0) {
			kern_logf("cpuperf: cpu %u not measured (%d)\n", cpu, error);
			continue;
		}

		/* Gives every CPU of the kind the measured rate. */
		for (other = 0; other < cpuperf_cpu_count; other++) {
			box = &cpuperf_boxes[other];
			enabled = spin_lock_irqsave(&box->lock);

			/* Gives the rate to a CPU of the same kind. */
			if (box->topology.core_class == kind)
				box->measured_mhz = mhz;

			spin_unlock_irqrestore(&box->lock, enabled);
		}

		/* Counts the kind and reports its rate. */
		measured++;
		kern_logf("cpuperf: cpu %u (kind %u) runs at most about %u MHz\n", cpu, kind, mhz);
	}

	/* Measures the relative performance from the rates. */
	mutex_lock(&cpuperf_mutex);

	cpuperf_relative_refresh();

	mutex_unlock(&cpuperf_mutex);

	/* Tells the readers the CPUs changed. */
	if (measured != 0U)
		cpuperf_publish_change();
}

/*
 * Runs a CPU's domain at its highest level, measures the CPU's rate and
 * gives the domain its intent back.  Thread context, without the mutex.
 */
static int
cpuperf_measure_domain(
	kern_cpu_id_t cpu,
	uint32_t *mhz)
{
	uint64_t *tickets;
	uint64_t deadline;
	uint32_t domain;
	int apply_error;
	int error;

	/* Allocates a ticket per CPU for the domain's requests. */
	tickets = kern_calloc(cpuperf_cpu_count, sizeof(*tickets));
	if (tickets == NULL)
		return ENOMEM;

	/* Runs the domain at its highest level. */
	mutex_lock(&cpuperf_mutex);

	domain = cpuperf_boxes[cpu].domain;
	cpuperf_domains[domain].intent.measuring = true;
	cpuperf_domain_post(domain, tickets);

	mutex_unlock(&cpuperf_mutex);

	/* Waits for every member of the domain to confirm, without the mutex. */
	apply_error = 0;
	deadline = cpuperf_deadline();
	error = cpuperf_domain_wait(domain, tickets, deadline, &apply_error);
	if (error == 0)
		error = apply_error;

	/* Measures the rate on the CPU. */
	if (error == 0)
		error = cpuperf_measure_cpu(cpu, mhz);

	/* Gives the domain its intent back, whatever the measurement found. */
	mutex_lock(&cpuperf_mutex);

	cpuperf_domains[domain].intent.measuring = false;
	cpuperf_domain_post(domain, tickets);

	mutex_unlock(&cpuperf_mutex);

	/* Releases the tickets. */
	kern_free(tickets);

	/* Reports a measurement that did not complete. */
	if (error != 0)
		return error;

	/* Reports a measurement that read no rate. */
	if (*mhz == 0U)
		return ENODATA;

	/* Succeeded: the CPU's rate at its highest level. */
	return 0;
}

/*
 * Measures one CPU's delivered rate, with a new thread each time the last
 * one did not run on the CPU throughout.
 *
 * The thread is bound to the CPU before it first runs, so no idle CPU
 * takes it from that CPU's queue.  It still checks where it runs, before
 * its busy time and after it, and reports EAGAIN when it finds itself
 * elsewhere: a rate read from another CPU is never kept, and a later
 * thread gets another try.
 *
 * Thread context; waits a bounded time.  Reports 0 or an errno.
 */
static int
cpuperf_measure_cpu(
	kern_cpu_id_t cpu,
	uint32_t *mhz)
{
	uint32_t tries;
	int error;

	/* Measures until a thread stayed on the CPU, or the tries run out. */
	error = EAGAIN;
	for (tries = 0; tries < CPUPERF_MEASURE_TRIES; tries++) {
		error = cpuperf_measure_once(cpu, mhz);

		/* Stops on a measurement, or on a failure another try would repeat. */
		if (error != EAGAIN)
			break;
	}

	/* Reports the last try. */
	return error;
}

/*
 * Measures one CPU's delivered rate in a thread bound to that CPU, which
 * marks the start, runs busy and reads the rate.
 *
 * Thread context; waits a bounded time.  Reports 0, EAGAIN when the
 * thread did not run on the CPU throughout, or another errno.
 */
static int
cpuperf_measure_once(
	kern_cpu_id_t cpu,
	uint32_t *mhz)
{
	struct cpuperf_measure *measure;
	struct thread *thread;
	unsigned long enabled;
	uint64_t deadline;
	uint64_t observed;
	uint64_t now;
	bool done;
	int error;

	/* Allocates the record the thread fills. */
	measure = kern_calloc(1U, sizeof(*measure));
	if (measure == NULL)
		return ENOMEM;

	/* Prepares the record for the thread that will fill it. */
	measure->cpu = cpu;
	spin_init(&measure->lock, LOCK_RANK_DEVICE, "cpuperf-measure");
	waitq_init(&measure->queue, "cpuperf-measure");
	measure->done = false;
	measure->abandoned = false;

	/* Creates the thread and binds it to the CPU before it first runs. */
	error = kthread_create(cpuperf_measure_main, measure, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_free(measure);
		return error;
	}

	/* Binds the new thread to the CPU, where it stays until it ends. */
	error = sched_set_cpu(thread, cpu, SCHED_CPU_BOUND);
	if (error != 0) {
		(void)thread_abort_new(thread);
		kern_free(measure);
		return error;
	}

	/* Lets the thread release itself when it ends, and starts it. */
	thread->detached = 1;
	thread_start(thread);

	/*
	 * Waits for the thread, with room for the busy time and two
	 * confirmations.  A waiter that gives up leaves the record to the
	 * thread, which frees it when it ends.
	 */
	deadline = sched_ticks() + kern_ms_to_ticks(CPUPERF_MEASURE_MS + 2U * CPUPERF_CONFIRM_MS);
	enabled = spin_lock_irqsave(&measure->lock);

	while (!measure->done) {
		now = sched_ticks();

		/* Gives up once the deadline has passed. */
		if (now >= deadline)
			break;
		observed = waitq_sequence(&measure->queue);
		(void)waitq_sleep(&measure->queue, &measure->lock, observed, deadline, 0U);
	}

	/* Notes whether the thread finished. */
	done = measure->done;

	/* Leaves the record to the thread, which frees it when it ends. */
	if (!done)
		measure->abandoned = true;
	error = measure->error;
	*mhz = measure->mhz;

	spin_unlock_irqrestore(&measure->lock, enabled);

	/* Reports a thread that did not finish in time. */
	if (!done)
		return ETIMEDOUT;

	/* Frees the record the finished thread no longer touches. */
	kern_free(measure);

	/* Reports the thread's failure. */
	if (error != 0)
		return error;

	/* Succeeded: the CPU's rate at its highest level. */
	return 0;
}

/*
 * Runs on the CPU being measured: marks the start, keeps the CPU busy
 * for the measuring time, then reads the rate.  Reports EAGAIN when it
 * runs on another CPU at the start or at the end of the busy time.
 */
static void
cpuperf_measure_main(
	void *argument)
{
	struct cpuperf_measure *measure;
	struct cpuperf_box *box;
	unsigned long enabled;
	uint64_t ticket;
	uint64_t deadline;
	uint64_t end;
	uint64_t now;
	kern_cpu_id_t cpu;
	kern_cpu_id_t current;
	uint32_t mhz;
	bool abandoned;
	int error;

	/* Takes the record the waiter handed over. */
	measure = argument;
	cpu = measure->cpu;

	/* Refuses to measure from the wrong CPU. */
	error = 0;
	current = kern_cpu_current();
	if (current != cpu)
		error = EAGAIN;

	/* Marks the start on this CPU. */
	if (error == 0) {
		ticket = cpuperf_post(cpu, CPUPERF_WORK_MEASURE_BEGIN, NULL);
		deadline = cpuperf_deadline();
		error = cpuperf_wait(cpu, ticket, deadline);
	}

	/* Keeps the CPU busy so it runs at the level it was given. */
	if (error == 0) {
		end = sched_ticks() + kern_ms_to_ticks(CPUPERF_MEASURE_MS);
		for (;;) {
			now = sched_ticks();

			/* Stops once the measuring time has passed. */
			if (now >= end)
				break;
			atomic_spin_hint();
		}

		/* Refuses a rate the CPU did not run busy for, after a move. */
		current = kern_cpu_current();
		if (current != cpu)
			error = EAGAIN;
	}

	/* Reads the rate since the start. */
	if (error == 0) {
		ticket = cpuperf_post(cpu, CPUPERF_WORK_MEASURE_END, NULL);
		deadline = cpuperf_deadline();
		error = cpuperf_wait(cpu, ticket, deadline);
	}

	/* Takes the answer out of the box. */
	box = &cpuperf_boxes[cpu];
	enabled = spin_lock_irqsave(&box->lock);

	/* Keeps the provider's failure when nothing failed before. */
	if (error == 0)
		error = box->measure_error;
	mhz = box->measured_mhz;

	spin_unlock_irqrestore(&box->lock, enabled);

	/* Hands the answer to the waiter and wakes it. */
	enabled = spin_lock_irqsave(&measure->lock);

	measure->error = error;
	measure->mhz = mhz;
	measure->done = true;
	abandoned = measure->abandoned;
	waitq_wake_all(&measure->queue);

	spin_unlock_irqrestore(&measure->lock, enabled);

	/* Frees the record a waiter gave up on. */
	if (abandoned)
		kern_free(measure);
}

/*
 * Starts the governor, or wakes it, when a domain needs it.  Called with
 * the mutex held.
 */
static void
cpuperf_governor_start(void)
{
	struct thread *thread;
	unsigned long enabled;
	uint32_t index;
	bool needed;
	int error;

	/* Looks for a domain whose step the governor picks. */
	needed = false;
	for (index = 0; index < cpuperf_domain_total; index++) {
		/* Skips a domain with nothing to control. */
		if (!cpuperf_domains[index].control)
			continue;
		needed = cpuperf_governed(&cpuperf_domains[index].intent, &cpuperf_domains[index].levels);

		/* Stops at the first domain the governor serves. */
		if (needed)
			break;
	}

	/* Leaves the governor alone when no domain needs it. */
	if (!needed)
		return;

	/* Starts the thread the first time it is needed. */
	if (cpuperf_governor_thread == NULL) {
		error = kthread_create(cpuperf_governor_main, NULL, SCHED_PRIORITY_DEFAULT, &thread);
		if (error != 0) {
			kern_logf("cpuperf: the governor did not start (%d); steps stay where they are\n", error);
			return;
		}

		/* Records the thread, lets it release itself when it ends, and starts it. */
		cpuperf_governor_thread = thread;
		thread->detached = 1;
		thread_start(thread);

		/* Succeeded: a new governor runs its first pass. */
		return;
	}

	/* Wakes a governor that sleeps without a deadline. */
	enabled = spin_lock_irqsave(&cpuperf_governor_lock);

	waitq_wake_all(&cpuperf_governor_queue);

	spin_unlock_irqrestore(&cpuperf_governor_lock, enabled);
}

/*
 * Runs the governor: a pass every period while a domain needs it, and a
 * sleep without a deadline while none does, so a machine with no list of
 * steps to govern is never woken by it.
 */
static void
cpuperf_governor_main(
	void *argument)
{
	unsigned long enabled;
	uint64_t observed;
	uint64_t deadline;
	bool active;

	UNUSED_PARAMETER(argument);

	/* Passes, then sleeps until the next period or until woken. */
	for (;;) {
		enabled = spin_lock_irqsave(&cpuperf_governor_lock);

		observed = waitq_sequence(&cpuperf_governor_queue);

		spin_unlock_irqrestore(&cpuperf_governor_lock, enabled);

		/* Moves the steps of the governed domains. */
		mutex_lock(&cpuperf_mutex);

		active = cpuperf_governor_pass();

		mutex_unlock(&cpuperf_mutex);

		/* Sleeps a period while needed, otherwise until woken. */
		deadline = 0U;
		if (active)
			deadline = sched_ticks() + kern_ms_to_ticks(CPUPERF_GOVERNOR_PERIOD_MS);
		enabled = spin_lock_irqsave(&cpuperf_governor_lock);

		(void)waitq_sleep(&cpuperf_governor_queue, &cpuperf_governor_lock, observed, deadline, 0U);

		spin_unlock_irqrestore(&cpuperf_governor_lock, enabled);
	}
}

/*
 * Moves each governed domain one step toward its load.  Called with the
 * mutex held.
 *
 * The busiest member decides: above the upper share the step goes up,
 * below the lower share it goes down, inside the bounds of the strategy.
 * Reports whether any domain is governed.
 */
static bool
cpuperf_governor_pass(void)
{
	struct cpuperf_domain *entry;
	struct kern_cpuperf_request request;
	uint64_t *tickets;
	kern_cpu_id_t cpu;
	uint32_t index;
	uint32_t busy;
	uint32_t busiest;
	uint32_t current;
	uint32_t low;
	uint32_t high;
	uint32_t next;
	bool active;
	bool governed;
	bool member;

	/* Allocates tickets the posts fill and nobody waits on. */
	tickets = kern_calloc(cpuperf_cpu_count, sizeof(*tickets));
	if (tickets == NULL)
		return true;

	/* Looks at every governed domain. */
	active = false;
	for (index = 0; index < cpuperf_domain_total; index++) {
		/* Skips a domain with nothing to control. */
		entry = &cpuperf_domains[index];
		if (!entry->control)
			continue;

		/* Skips a domain whose step the governor does not pick. */
		governed = cpuperf_governed(&entry->intent, &entry->levels);
		if (!governed)
			continue;
		active = true;

		/* Finds the busiest member since the last pass. */
		busiest = 0U;
		for (cpu = 0; cpu < cpuperf_cpu_count; cpu++) {
			/* Skips a CPU that is not a member. */
			member = cpuperf_domain_has(entry, cpu);
			if (!member)
				continue;
			busy = cpuperf_busy_percent(cpu);

			/* Keeps the busiest member's share. */
			if (busy > busiest)
				busiest = busy;
		}

		/* Works out the bounds and the step in force. */
		(void)cpuperf_compute(&entry->intent, &entry->levels, &request);
		low = cpuperf_step_index(&entry->levels, request.minimum);
		high = cpuperf_step_index(&entry->levels, request.maximum);
		current = cpuperf_step_index(&entry->levels, request.desired);

		/* Moves one step toward the load. */
		next = current;
		if (busiest > CPUPERF_GOVERNOR_UP_PERCENT && current < high)
			next = current + 1U;
		else if (busiest < CPUPERF_GOVERNOR_DOWN_PERCENT && current > low)
			next = current - 1U;

		/* Posts a changed step to the members. */
		if (next != current) {
			entry->intent.governor_desired = entry->levels.steps[next];
			cpuperf_domain_post(index, tickets);
		}
	}

	/* Releases the tickets. */
	kern_free(tickets);

	/* Reports whether the governor is still needed. */
	return active;
}

/*
 * Reports how busy a CPU was, in percent, since the governor last asked,
 * from the scheduler's count of its ticks.  Called with the mutex held.
 */
static uint32_t
cpuperf_busy_percent(
	kern_cpu_id_t cpu)
{
	struct sched_cpu_time now;
	struct sched_cpu_time *before;
	uint64_t total;
	uint64_t idle;
	uint64_t busy;
	int error;

	/* Reads the CPU's ticks without waking it. */
	error = sched_cpu_time(cpu, &now);
	if (error != 0)
		return 0U;

	/* Takes the difference from the last pass and remembers this one. */
	before = &cpuperf_governor_times[cpu];
	total = (now.user - before->user) +
		(now.system - before->system) +
		(now.idle - before->idle) +
		(now.other - before->other);
	idle = now.idle - before->idle;
	*before = now;

	/* A CPU that saw no tick says nothing about its load. */
	if (total == 0U)
		return 0U;

	/* Reports the share of the ticks that were not idle. */
	busy = (total - idle) * 100U / total;
	return (uint32_t)busy;
}

/* Reports whether any domain can be controlled.  Called with the mutex held. */
static bool
cpuperf_any_control(void)
{
	uint32_t index;

	/* Looks for one controllable domain. */
	for (index = 0; index < cpuperf_domain_total; index++) {
		if (cpuperf_domains[index].control)
			return true;
	}

	/* No domain can be controlled. */
	return false;
}

/*
 * Serves the CPU requests of /dev/thermal (32..63), which the thermal
 * core's dispatch hands here, matched on the whole request number.
 */
static int
cpuperf_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	/* Carries out the request the whole number names. */
	switch (request) {
	case KERN_THERMAL_GET_CPU:
		error = cpuperf_ioctl_cpu(file, argument);
		break;
	case KERN_THERMAL_GET_CPU_DOMAIN:
		error = cpuperf_ioctl_domain(argument);
		break;
	case KERN_THERMAL_SET_CPU_DOMAIN:
		error = cpuperf_ioctl_domain_set(file, argument);
		break;
	case KERN_THERMAL_GET_CPU_RELATIVE:
		error = cpuperf_ioctl_relative_get(argument);
		break;
	case KERN_THERMAL_SET_CPU_RELATIVE:
		error = cpuperf_ioctl_relative_set(file, argument);
		break;
	case KERN_THERMAL_GET_CPU_IDLE:
		error = cpuperf_ioctl_idle(file, argument, false);
		break;
	case KERN_THERMAL_GET_PACKAGE_IDLE:
		error = cpuperf_ioctl_idle(file, argument, true);
		break;
	default:
		error = ENOTTY;
		break;
	}

	/* Reports why the request was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the request was served. */
	return 0;
}

/*
 * Tells whether an open of /dev/thermal may change the CPUs or wake one:
 * opened for writing, which the node's mode and group allow.
 */
static bool
cpuperf_writable(
	const struct file *file)
{
	int access;

	/* The node's mode decided at the open who may write. */
	access = file_status_flags_get(file) & O_ACCMODE;
	if (access == O_RDONLY)
		return false;

	/* Succeeded: the open writes. */
	return true;
}

/*
 * Copies out one CPU and the number of CPUs.  A request to sample the CPU
 * needs an open for writing, since it wakes the CPU; a CPU that does not
 * answer the sample is reported with SAMPLE_TIMEOUT and the rest of its
 * fields.
 */
static int
cpuperf_ioctl_cpu(
	struct file *file,
	uintptr_t argument)
{
	struct thermal_cpu_info answer;
	struct kern_cpuperf_cpu info;
	uint32_t request;
	uint32_t cpu;
	bool writable;
	bool timed_out;
	int error;

	/* Reads which CPU is asked for and how. */
	error = copyin(argument, &answer, sizeof(answer));
	if (error != 0)
		return error;
	cpu = answer.cpu;
	request = answer.request;

	/* Refuses an unknown request. */
	if ((request & ~KERN_THERMAL_CPU_READ_SAMPLE) != 0U)
		return EINVAL;

	/* Refuses a sample on an open for reading only. */
	if ((request & KERN_THERMAL_CPU_READ_SAMPLE) != 0U) {
		writable = cpuperf_writable(file);
		if (!writable)
			return EPERM;
	}

	/* Refuses a CPU past the last. */
	if (cpu >= cpuperf_cpu_count)
		return ENOENT;

	/* Reports the CPU; a sample the CPU did not confirm still fills the rest. */
	kern_memset(&info, 0, sizeof(info));
	timed_out = false;
	error = kern_cpuperf_cpu(cpu, request, &info);
	if (error == ETIMEDOUT) {
		timed_out = true;
	} else if (error == EINVAL) {
		return ENOENT;
	} else if (error != 0) {
		return error;
	}

	/* Turns the report into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.cpu = cpu;
	answer.count = cpuperf_cpu_count;
	answer.request = request;
	answer.package = info.topology.package;
	answer.die = info.topology.die;
	answer.module = info.topology.module;
	answer.core = info.topology.core;
	answer.thread = info.topology.thread;
	answer.core_class = info.topology.core_class;
	answer.core_type = info.topology.core_type;
	answer.firmware_id = info.topology.firmware_id;
	answer.domain = info.domain;
	answer.voltage_group = info.voltage_group;
	answer.relative = info.relative;
	answer.available = info.available;
	answer.efficiency = info.efficiency;
	answer.lowest = info.lowest;
	answer.efficient = info.efficient;
	answer.guaranteed = info.guaranteed;
	answer.highest = info.highest;
	answer.highest_mhz = info.highest_mhz;
	answer.delivered_mhz = info.delivered_mhz;
	answer.flags = info.flags;

	/* Marks a sample the CPU did not confirm in time. */
	if (timed_out)
		answer.flags |= KERN_THERMAL_CPU_SAMPLE_TIMEOUT;

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the CPU. */
	return 0;
}

/* Copies out one domain and the number of domains. */
static int
cpuperf_ioctl_domain(
	uintptr_t argument)
{
	struct thermal_cpu_domain answer;
	struct kern_cpuperf_domain info;
	uint32_t domain;
	uint32_t total;
	uint32_t index;
	int error;

	/* Reads which domain is asked for. */
	error = copyin(argument, &answer, sizeof(answer));
	if (error != 0)
		return error;
	domain = answer.domain;

	/* Refuses a domain past the last. */
	total = kern_cpuperf_domain_count();
	if (domain >= total)
		return ENOENT;

	/* Reports the domain. */
	kern_memset(&info, 0, sizeof(info));
	error = kern_cpuperf_domain(domain, &info);
	if (error == EINVAL)
		return ENOENT;
	if (error != 0)
		return error;

	/* Turns the members and the request into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.domain = domain;
	answer.count = info.count;
	answer.cpu_count = info.cpu_count;
	answer.kind = info.levels.kind;
	answer.share = info.levels.share;
	answer.minimum = info.request.minimum;
	answer.maximum = info.request.maximum;
	answer.desired = info.request.desired;
	answer.preference = info.request.preference;
	answer.limit = info.limit;
	answer.flags = info.flags;

	/* Copies the mask of members. */
	for (index = 0; index < KERN_CPU_MASK_WORDS; index++)
		answer.cpus[index] = info.cpus[index];

	/* A RANGE is its two ends; STEPS are the list itself. */
	if (info.levels.kind == KERN_CPUPERF_LEVELS_RANGE) {
		answer.step_count = 2;
		answer.steps[0] = info.levels.lowest;
		answer.steps[1] = info.levels.highest;
	} else if (info.levels.kind == KERN_CPUPERF_LEVELS_STEPS) {
		answer.step_count = info.levels.step_count;

		/* Keeps the count inside the record's list. */
		if (answer.step_count > KERN_CPUPERF_STEP_MAX)
			answer.step_count = KERN_CPUPERF_STEP_MAX;

		/* Copies each step and its rate. */
		for (index = 0; index < answer.step_count; index++) {
			answer.steps[index] = info.levels.steps[index];
			answer.step_mhz[index] = info.levels.step_mhz[index];
		}
	}

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the domain. */
	return 0;
}

/*
 * Applies a direct setting to one domain or to every domain.  A setting
 * the CPUs did not confirm in time stays and answers ETIMEDOUT.
 */
static int
cpuperf_ioctl_domain_set(
	struct file *file,
	uintptr_t argument)
{
	struct thermal_cpu_setting request;
	struct kern_cpuperf_setting setting;
	uint32_t total;
	bool writable;
	int error;

	/* Only an open for writing sets the CPUs directly. */
	writable = cpuperf_writable(file);
	if (!writable)
		return EPERM;

	/* Reads the setting and refuses unknown fields or reserved words that are not zero. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	if ((request.fields & ~CPUPERF_SET_FIELDS) != 0U)
		return EINVAL;

	/* Refuses reserved words that are not zero. */
	if (request.reserved[0] != 0U || request.reserved[1] != 0U)
		return EINVAL;

	/* Refuses a domain past the last. */
	total = kern_cpuperf_domain_count();
	if (request.domain != KERN_CPUPERF_ALL && request.domain >= total)
		return ENOENT;

	/* Hands the setting to the framework. */
	kern_memset(&setting, 0, sizeof(setting));
	setting.fields = request.fields;
	setting.minimum = request.minimum;
	setting.maximum = request.maximum;
	setting.desired = request.desired;
	setting.preference = request.preference;

	/* Applies the setting. */
	error = kern_cpuperf_domain_set(request.domain, &setting);
	if (error != 0)
		return error;

	/* Succeeded: the setting is in force until the next strategy. */
	return 0;
}

/* Copies out the ways the relative performance can be measured and the one in force. */
static int
cpuperf_ioctl_relative_get(
	uintptr_t argument)
{
	struct thermal_cpu_relative answer;
	uint32_t available;
	uint32_t source;
	int error;

	/* Reads the ways from the framework. */
	kern_cpuperf_relative_get(&available, &source);

	/* Hands them to the caller. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.available = available;
	answer.source = source;

	/* Copies the record out. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the ways. */
	return 0;
}

/* Chooses the way the relative performance is measured, for an open for writing. */
static int
cpuperf_ioctl_relative_set(
	struct file *file,
	uintptr_t argument)
{
	struct thermal_cpu_relative request;
	bool writable;
	int error;

	/* Only an open for writing chooses. */
	writable = cpuperf_writable(file);
	if (!writable)
		return EPERM;

	/* Reads the way and refuses reserved words that are not zero. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	if (request.reserved[0] != 0U || request.reserved[1] != 0U)
		return EINVAL;

	/* Has the framework measure the CPUs again by that way. */
	error = kern_cpuperf_relative_select(request.source);
	if (error != 0)
		return error;

	/* Succeeded: the way is in force until the next boot. */
	return 0;
}

/*
 * Copies out the idle residency of one CPU's core (for an open for
 * writing, since it is read on that CPU) or of one package.
 */
static int
cpuperf_ioctl_idle(
	struct file *file,
	uintptr_t argument,
	bool package)
{
	struct thermal_cpu_idle answer;
	struct kern_cpuperf_idle idle;
	kern_cpu_id_t first;
	uint32_t target;
	uint32_t index;
	uint8_t scope;
	bool writable;
	int error;

	/* Reading a core wakes its CPU, which only an open for writing may do. */
	if (!package) {
		writable = cpuperf_writable(file);
		if (!writable)
			return EPERM;
	}

	/* Reads which CPU or package is asked for. */
	error = copyin(argument, &answer, sizeof(answer));
	if (error != 0)
		return error;
	target = answer.target;

	/* Refuses a CPU past the last. */
	if (!package && target >= cpuperf_cpu_count)
		return ENOENT;

	/* Refuses a package no CPU belongs to. */
	if (package) {
		first = cpuperf_package_first(target);
		if (first == KERN_CPUPERF_UNKNOWN)
			return ENOENT;
	}

	/* Reads the counters where they are counted. */
	kern_memset(&idle, 0, sizeof(idle));
	scope = KERN_THERMAL_CPU_IDLE_CORE;
	if (package) {
		scope = KERN_THERMAL_CPU_IDLE_PACKAGE;
		error = kern_cpuperf_idle_package(target, &idle);
	} else {
		error = kern_cpuperf_idle_cpu(target, &idle);
	}

	/* Reports a target that vanished as absent. */
	if (error == EINVAL)
		return ENOENT;

	/* Reports counters that could not be read. */
	if (error != 0)
		return error;

	/* Turns the residency into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.target = target;
	answer.count = cpuperf_cpu_count;
	answer.state_count = idle.count;
	answer.available = idle.available;

	/* Keeps the count inside the record's list. */
	if (answer.state_count > KERN_CPUPERF_IDLE_STATE_MAX)
		answer.state_count = KERN_CPUPERF_IDLE_STATE_MAX;

	/* Copies each state with the scope it was read at. */
	for (index = 0; index < answer.state_count; index++) {
		answer.state[index] = idle.state[index];
		answer.residency_ns[index] = idle.residency_ns[index];
		answer.scope[index] = scope;
	}

	/* Stamps the record with the time of the read. */
	answer.time_ns = idle.time_ns;

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the residency. */
	return 0;
}
