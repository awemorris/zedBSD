/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The thermal devices: /dev/thermal, through which a process reads the
 * thermal core's copies and a root chooses the strategy and the fans'
 * boost, and /dev/thermal-inject, which a kernel built for tests adds so
 * that a virtual machine can make up sensors and receivers and choose how
 * an emergency stops it.
 *
 * The device turns the core's own records into the records of the
 * interface (<uapi/thermal.h>) and back.  It owns the core's ioctl numbers
 * 1..31; the CPU framework registers 32..63 with the core and the core's
 * dispatch hands those on.  The injection device has an entry of its own
 * and answers its numbers, 64 and up, only there.
 *
 * An open that subscribes to events gets a ring in the core at its first
 * subscription; the open's own lock serializes its subscriptions, so two
 * threads of one process subscribing at once attach one ring.
 */

#include <kern/cdev.h>
#include <kern/clock.h>
#include <kern/cred.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/process.h>
#include <kern/sched.h>
#include <kern/thermal.h>
#include <kern/thermal_private.h>
#include <kern/thread.h>
#include <kern/uaccess.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/poll.h>
#include <uapi/thermal.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Marks an argument an operation's signature has and the operation does not use. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* The device numbers of the two nodes. */
#define THERMAL_DEVICE_RDEV		0x00010008U
#define THERMAL_INJECT_RDEV		0x00010009U

/* The records one read takes from the core at a time. */
#define THERMAL_READ_BATCH		8U

/* The made-up receivers a test may add; the core keeps sixteen in all. */
#define THERMAL_INJECT_RECEIVER_MAX	4U

/* The longest a made-up receiver waits before it answers, in milliseconds. */
#define THERMAL_INJECT_DELAY_MAX	60000U

/*
 * What one open of /dev/thermal keeps: the ring the core fills once the
 * open subscribed, and the lock that makes the open's subscriptions one
 * at a time.  Allocated at open and freed at close, when no thread of the
 * open can be in a read any more.
 */
struct thermal_open {
	struct mutex lock;
	struct kern_thermal_reader reader;
};

/*
 * One made-up sensor: the name a test gave it and the core's handle.
 * Filled under the injection lock and never taken out, as the core keeps
 * its sensors for good.
 */
struct thermal_inject_sensor_entry {
	char name[KERN_THERMAL_NAME_MAX];
	struct kern_thermal_sensor *sensor;
};

/*
 * One made-up receiver: the name a test gave it and the answer it gives
 * every strategy.  The answer changes under the injection lock, which the
 * receiver's apply also takes to read it; the entry is the receiver's
 * context and stays for good.
 */
struct thermal_inject_receiver_entry {
	char name[KERN_THERMAL_RECEIVER_NAME_MAX];
	int error;
	uint32_t flags;
	uint32_t delay_ms;
};

/*
 * The CPU framework's counts for the status.
 *
 * They are weak because a kernel built without the CPU framework does not
 * link it; a null test then reports no domains, no packages and no
 * generation.
 */
extern uint32_t kern_cpuperf_domain_count(void) __attribute__((weak));
extern uint32_t kern_cpuperf_package_count(void) __attribute__((weak));
extern uint32_t kern_cpuperf_generation(void) __attribute__((weak));

static int thermal_open_file(struct file *file);
static int thermal_close(struct file *file);
static ssize_t thermal_read(struct file *file, void *buffer, size_t size);
static int thermal_poll(struct file *file, short requested, short *returned);
static int thermal_ioctl(struct file *file, unsigned long request, uintptr_t argument);

/* The operations of /dev/thermal. */
static const struct cdev_ops thermal_ops = {
	.open = thermal_open_file,
	.close = thermal_close,
	.read = thermal_read,
	.ioctl = thermal_ioctl,
	.poll = thermal_poll
};

#ifdef CONFIG_THERMAL_TEST_INJECT
static int thermal_inject_open(struct file *file);
static int thermal_inject_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int thermal_inject_apply(void *context, const struct kern_thermal_strategy *strategy, uint32_t *flags);

/* The operations of /dev/thermal-inject, a test kernel's only. */
static const struct cdev_ops thermal_inject_ops = {
	.open = thermal_inject_open,
	.ioctl = thermal_inject_ioctl
};

/* What a made-up receiver does for the core: it only answers. */
static const struct kern_thermal_receiver_ops thermal_inject_receiver_ops = {
	.apply = thermal_inject_apply
};

/*
 * The injection lock: the made-up sensors' and receivers' tables and the
 * receivers' answers.  Made when the device is registered.
 */
static struct mutex thermal_inject_lock;

/* The made-up sensors in the order they came, and how many there are; under the injection lock. */
static struct thermal_inject_sensor_entry thermal_inject_sensors[KERN_THERMAL_SENSOR_MAX];
static unsigned thermal_inject_sensor_count;

/* The made-up receivers in the order they came, and how many there are; under the injection lock. */
static struct thermal_inject_receiver_entry thermal_inject_receivers[THERMAL_INJECT_RECEIVER_MAX];
static unsigned thermal_inject_receiver_count;
#endif

static int thermal_core_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static bool thermal_superuser(void);
static bool thermal_words_zero(const uint32_t *words, size_t count);
static int thermal_get_status(uintptr_t argument);
static int thermal_get_sensor(uintptr_t argument);
static int thermal_get_fan(uintptr_t argument);
static int thermal_get_meter(uintptr_t argument);
static int thermal_get_strategy(uintptr_t argument);
static int thermal_set_strategy(uintptr_t argument);
static int thermal_get_receiver(uintptr_t argument);
static int thermal_set_boost(uintptr_t argument);
static int thermal_subscribe(struct file *file, uintptr_t argument);
static int thermal_emergency_answer(void);
static void thermal_event_fill(const struct kern_thermal_record *record, struct thermal_event *event);
static uint32_t thermal_fan_flags(const struct kern_thermal_fan_info *fan);
#ifdef CONFIG_THERMAL_TEST_INJECT
static bool thermal_name_valid(const char *name, size_t size);
static int thermal_inject_sensor(uintptr_t argument);
static int thermal_inject_receiver(uintptr_t argument);
static int thermal_inject_path(uintptr_t argument);
#endif

/*
 * Publishes /dev/thermal, and /dev/thermal-inject in a test kernel, and
 * takes the core's ioctl numbers.
 */
int
kern_thermal_device_register(void)
{
	int error;

	/* Takes the core's numbers of /dev/thermal. */
	error = kern_thermal_ioctl_range_register(
		KERN_THERMAL_IOCTL_CORE_FIRST,
		KERN_THERMAL_IOCTL_CORE_LAST,
		thermal_core_ioctl);
	if (error != 0)
		return error;

	/* Publishes the node every process reads. */
	error = cdev_register("thermal", THERMAL_DEVICE_RDEV, &thermal_ops, NULL);
	if (error != 0)
		return error;

#ifdef CONFIG_THERMAL_TEST_INJECT
	/* Makes the lock of the made-up sensors and receivers. */
	error = mutex_init(&thermal_inject_lock, LOCK_RANK_DEVICE, "thermal inject");
	if (error != 0)
		return error;

	/* Publishes the node a test makes things up through. */
	error = cdev_register("thermal-inject", THERMAL_INJECT_RDEV, &thermal_inject_ops, NULL);
	if (error != 0)
		return error;
#endif

	/* Succeeded: the nodes are published. */
	return 0;
}

/* Gives an open of /dev/thermal its ring, not attached until it subscribes. */
static int
thermal_open_file(
	struct file *file)
{
	struct thermal_open *state;
	int error;

	/* Allocates what the open keeps. */
	state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* Prepares the subscription's lock and the ring. */
	kern_memset(state, 0, sizeof(*state));
	error = mutex_init(&state->lock, LOCK_RANK_DEVICE, "thermal open");
	if (error != 0) {
		kern_free(state);
		return error;
	}
	kern_thermal_reader_init(&state->reader);

	/* Succeeded: the file holds the state until it closes. */
	file->f_data = state;
	return 0;
}

/* Takes an open's ring off the core and frees what the open kept. */
static int
thermal_close(
	struct file *file)
{
	struct thermal_open *state;

	/* Stops the events for the open, then frees it. */
	state = file->f_data;
	if (state != NULL) {
		kern_thermal_reader_detach(&state->reader);
		kern_free(state);
	}

	/* The file holds nothing any more. */
	file->f_data = NULL;

	/* Succeeded: nothing of the open is left. */
	return 0;
}

/* Reads whole event records of the types the open subscribed to. */
static ssize_t
thermal_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct kern_thermal_record records[THERMAL_READ_BATCH];
	struct thermal_event *events;
	struct thermal_open *state;
	size_t capacity;
	size_t filled;
	unsigned want;
	unsigned taken;
	unsigned index;
	bool nonblock;
	int flags;
	int error;

	/* Refuses a buffer too small for one record. */
	state = file->f_data;
	if (size < sizeof(struct thermal_event))
		return -EINVAL;

	/* Whether the open waits for a record. */
	flags = file_status_flags_get(file);
	nonblock = false;
	if ((flags & O_NONBLOCK) != 0)
		nonblock = true;

	/* Waits for a record; an open that did not subscribe is refused. */
	error = kern_thermal_reader_wait(&state->reader, nonblock);
	if (error != 0)
		return -error;

	/* Moves the records into the buffer, a batch at a time, while they come and fit. */
	events = buffer;
	capacity = size / sizeof(struct thermal_event);
	filled = 0;
	while (filled < capacity) {
		want = THERMAL_READ_BATCH;
		if (capacity - filled < (size_t)want)
			want = (unsigned)(capacity - filled);
		taken = kern_thermal_reader_take(&state->reader, records, want);
		if (taken == 0U)
			break;

		/* Turns each record into the interface's. */
		for (index = 0; index < taken; index++) {
			thermal_event_fill(&records[index], &events[filled]);
			filled++;
		}
	}

	/* Succeeded: the bytes of the records taken. */
	return (ssize_t)(filled * sizeof(struct thermal_event));
}

/* Says whether an open has event records to read. */
static int
thermal_poll(
	struct file *file,
	short requested,
	short *returned)
{
	struct thermal_open *state;
	bool ready;

	/* Refuses a poll with nowhere to answer. */
	if (returned == NULL)
		return EINVAL;

	/* Readable while a record waits. */
	*returned = 0;
	state = file->f_data;
	ready = kern_thermal_reader_ready(&state->reader);
	if (ready)
		*returned = (short)(requested & (POLLIN | POLLRDNORM));

	/* Succeeded: the answer is in *returned. */
	return 0;
}

/* Hands an ioctl to the part of the kernel that owns its number. */
static int
thermal_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	/* The core serves 1..31 and the CPU framework 32..63; any other number is ENOTTY. */
	error = kern_thermal_ioctl_dispatch(file, request, argument);
	if (error != 0)
		return error;

	/* Succeeded: the owner served the request. */
	return 0;
}

/* Serves the core's requests, matched on the whole request number. */
static int
thermal_core_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	/* Carries out the request the whole number names. */
	switch (request) {
	case KERN_THERMAL_GET_STATUS:
		error = thermal_get_status(argument);
		break;
	case KERN_THERMAL_GET_SENSOR:
		error = thermal_get_sensor(argument);
		break;
	case KERN_THERMAL_GET_FAN:
		error = thermal_get_fan(argument);
		break;
	case KERN_THERMAL_GET_METER:
		error = thermal_get_meter(argument);
		break;
	case KERN_THERMAL_GET_STRATEGY:
		error = thermal_get_strategy(argument);
		break;
	case KERN_THERMAL_SET_STRATEGY:
		error = thermal_set_strategy(argument);
		break;
	case KERN_THERMAL_GET_RECEIVER:
		error = thermal_get_receiver(argument);
		break;
	case KERN_THERMAL_SET_BOOST:
		error = thermal_set_boost(argument);
		break;
	case KERN_THERMAL_SUBSCRIBE:
		error = thermal_subscribe(file, argument);
		break;
	case KERN_THERMAL_EMERGENCY_ACK:
		error = thermal_emergency_answer();
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

/* Tells whether the calling process is the superuser's. */
static bool
thermal_superuser(void)
{
	struct ucred *credential;
	int superuser;

	/* Asks the caller's credentials. */
	credential = cred_current_ref();
	superuser = cred_is_superuser(credential);
	cred_release(credential);
	if (!superuser)
		return false;

	/* Succeeded: the caller is root. */
	return true;
}

/* Tells whether every reserved word is zero. */
static bool
thermal_words_zero(
	const uint32_t *words,
	size_t count)
{
	size_t index;

	/* Any nonzero word makes the request one this kernel does not know. */
	for (index = 0; index < count; index++) {
		if (words[index] != 0U)
			return false;
	}

	/* Succeeded: all zero. */
	return true;
}

/* Copies out the machine's thermal state at a glance. */
static int
thermal_get_status(
	uintptr_t argument)
{
	struct kern_thermal_status status;
	struct thermal_status answer;
	int error;

	/* Reads the core's counts, the strategy and the emergency. */
	kern_thermal_status_get(&status);

	/* Turns them into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.sensor_count = status.sensor_count;
	answer.fan_count = status.fan_count;
	answer.meter_count = status.meter_count;
	answer.receiver_count = status.receiver_count;
	answer.strategy = status.strategy;
	answer.strategy_flags = status.strategy_flags;
	answer.generation = status.generation;
	answer.passive_permille = status.passive_permille;
	answer.emergency = status.emergency;
	answer.emergency_sensor = status.emergency_sensor;
	answer.emergency_millicelsius = status.emergency_millicelsius;
	answer.strategy_error = status.strategy_error;
	answer.emergency_time_ns = status.emergency_time_ns;

	/* Adds the CPUs; the framework, where it is built, counts the domains and the packages. */
	answer.cpu_count = kern_cpu_count();
	if (kern_cpuperf_domain_count != NULL)
		answer.cpu_domain_count = kern_cpuperf_domain_count();
	if (kern_cpuperf_package_count != NULL)
		answer.cpu_package_count = kern_cpuperf_package_count();
	if (kern_cpuperf_generation != NULL)
		answer.cpu_generation = kern_cpuperf_generation();

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the state. */
	return 0;
}

/* Copies out one temperature sensor and the number of sensors. */
static int
thermal_get_sensor(
	uintptr_t argument)
{
	struct kern_thermal_sensor_info info;
	struct kern_thermal_status status;
	struct thermal_sensor_info answer;
	uint32_t index;
	int error;

	/* Reads which sensor is asked for. */
	error = copyin(argument, &answer, sizeof(answer));
	if (error != 0)
		return error;
	index = answer.index;

	/* Copies the sensor; one past the last is ENOENT. */
	error = kern_thermal_sensor_get(index, &info);
	if (error != 0)
		return error;
	kern_thermal_status_get(&status);

	/* Turns it into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.index = index;
	answer.count = status.sensor_count;
	kern_memcpy(answer.name, info.name, sizeof(answer.name));
	answer.kind = info.kind;
	answer.flags = info.flags;
	answer.reading = info.reading;
	answer.millicelsius = info.millicelsius;
	answer.passive = info.passive;
	answer.critical = info.critical;
	answer.time_ns = info.time_ns;

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the sensor. */
	return 0;
}

/* Copies out one fan and the number of fans. */
static int
thermal_get_fan(
	uintptr_t argument)
{
	struct kern_thermal_fan_info info;
	struct kern_thermal_status status;
	struct thermal_fan_info answer;
	uint32_t index;
	int error;

	/* Reads which fan is asked for. */
	error = copyin(argument, &answer, sizeof(answer));
	if (error != 0)
		return error;
	index = answer.index;

	/* Copies the fan; one past the last is ENOENT. */
	error = kern_thermal_fan_get(index, &info);
	if (error != 0)
		return error;
	kern_thermal_status_get(&status);

	/* Turns it into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.index = index;
	answer.count = status.fan_count;
	kern_memcpy(answer.name, info.name, sizeof(answer.name));
	answer.flags = thermal_fan_flags(&info);
	answer.rpm = info.state.rpm;
	answer.rpm_max = info.rpm_max;
	answer.level = info.state.level;
	answer.level_count = info.state.level_count;
	answer.boost = info.boost;
	answer.boost_count = info.state.boost_count;
	answer.time_ns = info.time_ns;

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the fan. */
	return 0;
}

/*
 * Copies out one energy meter and the number of meters.
 *
 * Anyone may read it: the core reads the counter once a second and a
 * reader only sees that copy, never a reading of its own.
 */
static int
thermal_get_meter(
	uintptr_t argument)
{
	struct kern_thermal_meter_info info;
	struct kern_thermal_status status;
	struct thermal_meter_info answer;
	uint32_t index;
	uint32_t flags;
	int error;

	/* Reads which meter is asked for. */
	error = copyin(argument, &answer, sizeof(answer));
	if (error != 0)
		return error;
	index = answer.index;

	/* Copies the meter; one past the last is ENOENT. */
	error = kern_thermal_meter_get(index, &info);
	if (error != 0)
		return error;
	kern_thermal_status_get(&status);

	/* Says whether it has been read and whether the last read failed. */
	flags = 0;
	if ((info.copy & KERN_THERMAL_METER_COPY_KNOWN) != 0U)
		flags |= KERN_THERMAL_METER_KNOWN;
	if ((info.copy & KERN_THERMAL_METER_COPY_STALE) != 0U)
		flags |= KERN_THERMAL_METER_STALE;

	/* Turns it into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.index = index;
	answer.count = status.meter_count;
	kern_memcpy(answer.name, info.name, sizeof(answer.name));
	answer.kind = info.kind;
	answer.flags = flags;
	answer.energy_nj = info.energy_nj;
	answer.time_ns = info.time_ns;

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the meter. */
	return 0;
}

/* Copies out the strategy in force. */
static int
thermal_get_strategy(
	uintptr_t argument)
{
	struct kern_thermal_strategy_info info;
	struct thermal_strategy answer;
	int error;

	/* Reads the strategy, its outcome and the firmware's mode. */
	kern_thermal_strategy_get(&info);

	/* Turns it into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.value = info.strategy.value;
	answer.speed = info.strategy.speed;
	answer.noise = info.strategy.noise;
	answer.gating = info.strategy.gating;
	answer.flags = info.flags;
	answer.generation = info.generation;
	kern_memcpy(answer.firmware_name, info.firmware_name, sizeof(answer.firmware_name));
	answer.receiver_count = info.receiver_count;
	answer.error = info.error;

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the strategy. */
	return 0;
}

/*
 * Chooses a strategy for a root and answers how far the receivers carried
 * it out.  A refused choice copies nothing out.
 */
static int
thermal_set_strategy(
	uintptr_t argument)
{
	struct kern_thermal_strategy_info info;
	struct kern_thermal_strategy strategy;
	struct thermal_strategy request;
	uint32_t generation;
	uint32_t flags;
	bool superuser;
	bool zero;
	int error;

	/* Only root chooses. */
	superuser = thermal_superuser();
	if (!superuser)
		return EPERM;

	/* Reads the choice and refuses reserved fields that are not zero. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	zero = thermal_words_zero(request.reserved, sizeof(request.reserved) / sizeof(request.reserved[0]));
	if (!zero || request.reserved0 != 0U)
		return EINVAL;

	/* Hands the value and the axes to the core, which checks them and waits for the receivers. */
	kern_memset(&strategy, 0, sizeof(strategy));
	strategy.value = request.value;
	strategy.speed = request.speed;
	strategy.noise = request.noise;
	strategy.gating = request.gating;
	error = kern_thermal_strategy_set(&strategy, &flags, &generation);
	if (error != 0)
		return error;

	/* Reads the firmware's mode and the receivers' count the answer carries. */
	kern_thermal_strategy_get(&info);

	/* Answers the outcome in the caller's record. */
	request.flags = flags;
	request.generation = generation;
	kern_memcpy(request.firmware_name, info.firmware_name, sizeof(request.firmware_name));
	request.receiver_count = info.receiver_count;
	request.error = 0;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: the strategy is in force. */
	return 0;
}

/* Copies out one receiver of the strategy and its last answer. */
static int
thermal_get_receiver(
	uintptr_t argument)
{
	struct kern_thermal_receiver_info info;
	struct kern_thermal_status status;
	struct thermal_receiver_info answer;
	uint32_t index;
	int error;

	/* Reads which receiver is asked for. */
	error = copyin(argument, &answer, sizeof(answer));
	if (error != 0)
		return error;
	index = answer.index;

	/* Copies the receiver; one past the last is ENOENT. */
	error = kern_thermal_receiver_get(index, &info);
	if (error != 0)
		return error;
	kern_thermal_status_get(&status);

	/* Turns it into the interface's record. */
	kern_memset(&answer, 0, sizeof(answer));
	answer.index = index;
	answer.count = status.receiver_count;
	kern_memcpy(answer.name, info.name, sizeof(answer.name));
	answer.error = info.error;
	answer.flags = info.flags;
	answer.generation = info.generation;
	answer.duration_ms = info.duration_ms;
	answer.time_ns = info.time_ns;

	/* Hands the record to the caller. */
	error = copyout(&answer, argument, sizeof(answer));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the receiver. */
	return 0;
}

/* Sets a fan's boost for a root and answers whether its driver took it in time. */
static int
thermal_set_boost(
	uintptr_t argument)
{
	struct thermal_boost request;
	uint32_t flags;
	bool superuser;
	int error;

	/* Only root raises a fan. */
	superuser = thermal_superuser();
	if (!superuser)
		return EPERM;

	/* Reads the request and refuses a reserved field that is not zero. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	if (request.reserved != 0U)
		return EINVAL;

	/* Hands it to the core, which checks every fan named and waits for the drivers. */
	error = kern_thermal_boost_set(request.fan, request.level, &flags);
	if (error != 0)
		return error;

	/* Answers whether a driver was still at work. */
	request.flags = flags;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0)
		return error;

	/* Succeeded: the fans are at the level, or will be. */
	return 0;
}

/* Sets the event types an open reads, attaching its ring the first time and detaching it at 0. */
static int
thermal_subscribe(
	struct file *file,
	uintptr_t argument)
{
	struct thermal_subscription request;
	struct thermal_open *state;
	bool zero;
	int error;

	/* Reads the mask and refuses bits no event has, or reserved words that are not zero. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	zero = thermal_words_zero(request.reserved, sizeof(request.reserved) / sizeof(request.reserved[0]));
	if (!zero)
		return EINVAL;
	if ((request.mask & ~KERN_THERMAL_EVENT_MASK) != 0U)
		return EINVAL;

	/* Changes the open's subscription, one at a time. */
	state = file->f_data;
	error = 0;
	mutex_lock(&state->lock);

	if (request.mask == 0U) {
		/* Mask 0 stops the subscription and gives its place back. */
		kern_thermal_reader_detach(&state->reader);
	} else if (state->reader.attached) {
		/* A subscribed open only changes its mask. */
		kern_thermal_reader_subscribe(&state->reader, request.mask);
	} else {
		/* The first subscription attaches the ring, while fewer than sixteen opens have one. */
		error = kern_thermal_reader_attach(&state->reader, request.mask);
	}

	mutex_unlock(&state->lock);

	/* Reports an open that found every place taken. */
	if (error != 0)
		return error;

	/* Succeeded: the open reads those types from now on. */
	return 0;
}

/* Takes init's word that it began stopping the machine for the emergency. */
static int
thermal_emergency_answer(void)
{
	pid_t pid;
	int error;

	/* Only init may answer, and only while the guard waits for it. */
	pid = curthread->proc->pid;
	error = kern_thermal_emergency_ack((int)pid);
	if (error != 0)
		return error;

	/* Succeeded: the guard gives init its two minutes. */
	return 0;
}

/* Turns one of the core's event records into the interface's. */
static void
thermal_event_fill(
	const struct kern_thermal_record *record,
	struct thermal_event *event)
{
	/* Every value the events carry fits 32 bits; a generation is its bit pattern. */
	kern_memset(event, 0, sizeof(*event));
	event->size = sizeof(*event);
	event->type = record->type;
	event->index = record->index;
	event->value = (int32_t)record->value;
	event->sequence = record->sequence;
	event->time_ns = record->time_ns;
	kern_memcpy(event->name, record->name, sizeof(event->name));
}

/* Gathers what a fan has and how its last reading went into the interface's flags. */
static uint32_t
thermal_fan_flags(
	const struct kern_thermal_fan_info *fan)
{
	uint32_t flags;

	/* What the fan's driver said it has. */
	flags = 0;
	if ((fan->state.flags & KERN_THERMAL_FAN_HAS_RPM) != 0U)
		flags |= KERN_THERMAL_FAN_HAS_RPM;
	if ((fan->state.flags & KERN_THERMAL_FAN_HAS_LEVEL) != 0U)
		flags |= KERN_THERMAL_FAN_HAS_LEVEL;

	/* How the core's last reading of it went. */
	if ((fan->copy & KERN_THERMAL_FAN_COPY_KNOWN) != 0U)
		flags |= KERN_THERMAL_FAN_KNOWN;
	if ((fan->copy & KERN_THERMAL_FAN_COPY_STALE) != 0U)
		flags |= KERN_THERMAL_FAN_STALE;
	if ((fan->copy & KERN_THERMAL_FAN_COPY_STALLED) != 0U)
		flags |= KERN_THERMAL_FAN_STALLED;

	/* Whether a root can raise it. */
	if ((fan->copy & KERN_THERMAL_FAN_COPY_BOOST) != 0U)
		flags |= KERN_THERMAL_FAN_CAN_BOOST;

	/* Succeeded: the fan's flags. */
	return flags;
}

#ifdef CONFIG_THERMAL_TEST_INJECT
/* Tells whether a name is not empty and ends within its room. */
static bool
thermal_name_valid(
	const char *name,
	size_t size)
{
	const void *terminator;

	/* An empty name names nothing. */
	if (name[0] == '\0')
		return false;

	/* A name that fills its room without a terminator is cut off. */
	terminator = kern_memchr(name, '\0', size);
	if (terminator == NULL)
		return false;

	/* Succeeded: a whole name. */
	return true;
}

/* Admits root alone to the injection node, whatever the node's mode says. */
static int
thermal_inject_open(
	struct file *file)
{
	bool superuser;

	UNUSED_PARAMETER(file);

	/* A made-up sensor can stop the machine: only root may make one. */
	superuser = thermal_superuser();
	if (!superuser)
		return EPERM;

	/* Succeeded: the open keeps nothing of its own. */
	return 0;
}

/* Serves the injection requests, matched on the whole request number. */
static int
thermal_inject_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	UNUSED_PARAMETER(file);

	/* Carries out the request the whole number names. */
	switch (request) {
	case KERN_THERMAL_INJECT_SENSOR:
		error = thermal_inject_sensor(argument);
		break;
	case KERN_THERMAL_INJECT_RECEIVER:
		error = thermal_inject_receiver(argument);
		break;
	case KERN_THERMAL_INJECT_PATH:
		error = thermal_inject_path(argument);
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
 * Makes up a sensor, or updates the one of the same name, and pushes its
 * reading and levels to the core as a driver would.
 */
static int
thermal_inject_sensor(
	uintptr_t argument)
{
	struct thermal_inject_sensor request;
	struct thermal_inject_sensor_entry *entry;
	struct kern_thermal_sensor *sensor;
	unsigned index;
	bool named;
	bool valid;
	bool zero;
	int error;

	/* Reads the sensor and checks its name, kind, flags and reserved words. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	named = thermal_name_valid(request.name, sizeof(request.name));
	if (!named)
		return EINVAL;
	if (request.kind != KERN_THERMAL_KIND_ZONE && request.kind != KERN_THERMAL_KIND_DEVICE)
		return EINVAL;
	if ((request.flags & ~KERN_THERMAL_SENSOR_CPU) != 0U)
		return EINVAL;
	zero = thermal_words_zero(request.reserved, sizeof(request.reserved) / sizeof(request.reserved[0]));
	if (!zero)
		return EINVAL;

	/* Finds the sensor of that name, or registers it; the core's table is shared with the real ones. */
	sensor = NULL;
	error = 0;
	mutex_lock(&thermal_inject_lock);

	for (index = 0; index < thermal_inject_sensor_count; index++) {
		entry = &thermal_inject_sensors[index];
		if (kern_strcmp(entry->name, request.name) == 0) {
			sensor = entry->sensor;
			break;
		}
	}
	if (sensor == NULL) {
		error = kern_thermal_sensor_register(request.name, request.kind, request.flags, &sensor);
		if (error == 0) {
			entry = &thermal_inject_sensors[thermal_inject_sensor_count];
			kern_memcpy(entry->name, request.name, sizeof(entry->name));
			entry->sensor = sensor;
			thermal_inject_sensor_count++;
		}
	}

	mutex_unlock(&thermal_inject_lock);

	/* Reports a full table. */
	if (error != 0)
		return error;

	/* Pushes the levels first, then the reading, as the firmware zones' driver does. */
	kern_thermal_sensor_levels(sensor, request.passive, request.critical);
	valid = false;
	if (request.valid != 0U)
		valid = true;
	kern_thermal_sensor_update(sensor, request.millicelsius, valid);

	/* Succeeded: the core watches the reading. */
	return 0;
}

/*
 * Makes up a receiver of the strategy, or changes the answer of the one of
 * the same name.
 */
static int
thermal_inject_receiver(
	uintptr_t argument)
{
	struct thermal_inject_receiver request;
	struct thermal_inject_receiver_entry *entry;
	unsigned index;
	bool register_entry;
	bool named;
	bool zero;
	int error;

	/* Reads the answer and checks its name, flags, wait and reserved words. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	named = thermal_name_valid(request.name, sizeof(request.name));
	if (!named)
		return EINVAL;
	if ((request.flags & ~KERN_THERMAL_APPLY_UNCONFIRMED) != 0U)
		return EINVAL;
	if (request.delay_ms > THERMAL_INJECT_DELAY_MAX || request.error < 0)
		return EINVAL;
	zero = thermal_words_zero(request.reserved, sizeof(request.reserved) / sizeof(request.reserved[0]));
	if (!zero)
		return EINVAL;

	/* Changes the answer of the receiver of that name, or takes a new entry for it. */
	entry = NULL;
	register_entry = false;
	mutex_lock(&thermal_inject_lock);

	for (index = 0; index < thermal_inject_receiver_count; index++) {
		if (kern_strcmp(thermal_inject_receivers[index].name, request.name) == 0) {
			entry = &thermal_inject_receivers[index];
			break;
		}
	}
	if (entry == NULL && thermal_inject_receiver_count < THERMAL_INJECT_RECEIVER_MAX) {
		entry = &thermal_inject_receivers[thermal_inject_receiver_count];
		kern_memcpy(entry->name, request.name, sizeof(entry->name));
		thermal_inject_receiver_count++;
		register_entry = true;
	}
	if (entry != NULL) {
		entry->error = request.error;
		entry->flags = request.flags;
		entry->delay_ms = request.delay_ms;
	}

	mutex_unlock(&thermal_inject_lock);

	/* Refuses a receiver past the test's table. */
	if (entry == NULL)
		return ENOSPC;

	/* Registers a new receiver with the core; it is given the strategy in force. */
	if (register_entry) {
		error = kern_thermal_receiver_register(request.name, &thermal_inject_receiver_ops, entry);
		if (error != 0)
			return error;
	}

	/* Succeeded: the receiver answers the next strategy so. */
	return 0;
}

/* Chooses how the next emergency stops the machine under test. */
static int
thermal_inject_path(
	uintptr_t argument)
{
	struct thermal_inject_path request;
	bool zero;
	int error;

	/* Reads the path and refuses an unknown one or reserved words that are not zero. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	zero = thermal_words_zero(request.reserved, sizeof(request.reserved) / sizeof(request.reserved[0]));
	if (!zero)
		return EINVAL;

	/* Tells the guard whether to ask init first. */
	switch (request.path) {
	case KERN_THERMAL_INJECT_PATH_INIT:
		kern_thermal_emergency_direct(false);
		break;
	case KERN_THERMAL_INJECT_PATH_DIRECT:
		kern_thermal_emergency_direct(true);
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the next emergency takes that path. */
	return 0;
}

/* Answers a strategy as the test asked: after the wait, with the error and the flags. */
static int
thermal_inject_apply(
	void *context,
	const struct kern_thermal_strategy *strategy,
	uint32_t *flags)
{
	struct thermal_inject_receiver_entry *entry;
	uint32_t delay_ms;
	uint32_t answer_flags;
	int answer;

	UNUSED_PARAMETER(strategy);

	/* Reads the answer the test chose. */
	entry = context;
	mutex_lock(&thermal_inject_lock);

	answer = entry->error;
	answer_flags = entry->flags;
	delay_ms = entry->delay_ms;

	mutex_unlock(&thermal_inject_lock);

	/* Takes the time a slow firmware would. */
	if (delay_ms != 0U)
		kern_usleep_range(delay_ms * 1000U, delay_ms * 1000U);

	/* Reports the error the test chose. */
	*flags = answer_flags;
	if (answer != 0)
		return answer;

	/* Succeeded: the strategy is taken. */
	return 0;
}
#endif
