/* WS006 IN-T32: production input-device report and detach ownership. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/input-device.h"
#include "kern/lock.h"
#include "kern/waitq.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#define CAPTURED_MAX 32U
#define REGISTERED_MAX 8U
#define TEST_INPUT_DEVICE_MAX 8U

static struct input_report captured[CAPTURED_MAX];
static size_t captured_count;
static uint64_t now = 1000;
static struct cdev registered_cdevs[REGISTERED_MAX];
static size_t registered_count;
static unsigned cdev_failures;
static unsigned cdev_probe_open;
static int cdev_probe_error;

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	lock->held.value = 0;
	(void)rank;
	(void)name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	while (__atomic_exchange_n(&lock->held.value, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		sched_yield();
	return 1;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	assert(enabled == 1);
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

uint64_t
waitq_sequence(const struct wait_queue *queue)
{
	return __atomic_load_n(&queue->sequence, __ATOMIC_ACQUIRE);
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *lock,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	(void)deadline;
	(void)flags;
	spin_unlock_irqrestore(lock, 1);
	while (__atomic_load_n(&queue->sequence, __ATOMIC_ACQUIRE) == observed)
		sched_yield();
	(void)spin_lock_irqsave(lock);
	return 0;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	(void)__atomic_add_fetch(&queue->sequence, 1U, __ATOMIC_RELEASE);
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

uint64_t
clock_milliseconds(void *context)
{
	(void)context;
	return now++;
}

int
cdev_register(const char *name, dev_t rdev, const struct cdev_ops *ops,
	void *data)
{
	if (cdev_failures != 0) {
		cdev_failures--;
		return EIO;
	}
	if (cdev_probe_open) {
		struct cdev probe;
		struct inode inode;
		struct file file;

		memset(&probe, 0, sizeof(probe));
		memset(&inode, 0, sizeof(inode));
		memset(&file, 0, sizeof(file));
		probe.ops = ops;
		probe.data = data;
		inode.i_data = &probe;
		file.f_inode = &inode;
		file.f_flags.value = O_RDONLY;
		cdev_probe_error = ops->open(&file);
		if (cdev_probe_error == 0)
			assert(ops->close(&file) == 0);
	}
	assert(registered_count < REGISTERED_MAX);
	memset(&registered_cdevs[registered_count], 0,
	    sizeof(registered_cdevs[registered_count]));
	strncpy(registered_cdevs[registered_count].name, name,
	    sizeof(registered_cdevs[registered_count].name) - 1U);
	registered_cdevs[registered_count].rdev = rdev;
	registered_cdevs[registered_count].ops = ops;
	registered_cdevs[registered_count].data = data;
	registered_count++;
	return 0;
}

static void
test_registration_publication_and_rollback(void)
{
	static const struct input_capability capabilities[] = {
	    {EV_SYN, SYN_REPORT}};
	const struct input_device_info info = {
	    .name = "registration rollback",
	    .capabilities = capabilities,
	    .capability_count = sizeof(capabilities) / sizeof(capabilities[0]),
	};
	struct input_device *device;
	unsigned index;

	cdev_failures = TEST_INPUT_DEVICE_MAX + 1U;
	for (index = 0; index < TEST_INPUT_DEVICE_MAX + 1U; index++) {
		device = NULL;
		assert(input_device_register(&info, &device) == EIO);
		assert(device == NULL);
	}
	assert(registered_count == 0);
	cdev_probe_open = 1;
	cdev_probe_error = -1;
}

int
copyin(uintptr_t source, void *destination, size_t size)
{
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	memcpy((void *)destination, source, size);
	return 0;
}

int
user_address_add(uintptr_t address, size_t delta, uintptr_t *result)
{
	if (UINTPTR_MAX - address < delta)
		return EFAULT;
	*result = address + delta;
	return 0;
}

void
poll_notify(void)
{
}

int
hal_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

static void
capture(void *context, const struct input_report *report)
{
	(void)context;
	assert(captured_count < CAPTURED_MAX);
	captured[captured_count++] = *report;
}

static void
clear_capture(void)
{
	memset(captured, 0, sizeof(captured));
	captured_count = 0;
}

static struct hal_key_event
key_event(const char *symbol, uint32_t flags)
{
	struct hal_key_event event;

	memset(&event, 0, sizeof(event));
	assert(strlen(symbol) < sizeof(event.symbol));
	strcpy(event.symbol, symbol);
	event.flags = flags;
	return event;
}

static struct input_device *
register_keyboard(unsigned flags)
{
	static const struct input_capability capabilities[] = {
	    {EV_SYN, SYN_REPORT}, {EV_KEY, KEY_A},
	    {EV_KEY, KEY_LEFTSHIFT}};
	const struct input_device_info info = {
	    .name = "host keyboard",
	    .physical_path = "host/input",
	    .capabilities = capabilities,
	    .capability_count = sizeof(capabilities) / sizeof(capabilities[0]),
	    .flags = flags,
	};
	struct input_device *device = NULL;

	assert(input_device_register(&info, &device) == 0);
	assert(device != NULL);
	return device;
}

static struct input_device *
register_keyboard_callbacks(int (*open_callback)(void *),
	void (*close_callback)(void *), void *context)
{
	static const struct input_capability capabilities[] = {
	    {EV_SYN, SYN_REPORT}, {EV_KEY, KEY_A},
	    {EV_KEY, KEY_LEFTSHIFT}};
	const struct input_device_info info = {
	    .name = "host callback keyboard",
	    .physical_path = "host/callback-input",
	    .capabilities = capabilities,
	    .capability_count = sizeof(capabilities) / sizeof(capabilities[0]),
	    .flags = INPUT_DEVICE_KEY_REPEAT,
	    .open = open_callback,
	    .close = close_callback,
	    .context = context,
	};
	struct input_device *device = NULL;

	assert(input_device_register(&info, &device) == 0);
	return device;
}

struct test_file {
	struct inode inode;
	struct file file;
};

static const struct cdev *
device_cdev(struct input_device *device)
{
	size_t index;

	for (index = 0; index < registered_count; index++)
		if (registered_cdevs[index].data == device)
			return &registered_cdevs[index];
	return NULL;
}

static void
test_file_init(struct test_file *test, struct input_device *device)
{
	const struct cdev *cdev = device_cdev(device);

	assert(cdev != NULL);
	memset(test, 0, sizeof(*test));
	test->inode.i_data = (void *)cdev;
	test->file.f_inode = &test->inode;
	test->file.f_flags.value = O_RDONLY;
}

static int
key_bit(const unsigned long *bits, unsigned code)
{
	return (bits[code / INPUT_BITS_PER_WORD] &
	    (1UL << (code % INPUT_BITS_PER_WORD))) != 0;
}

static struct input_device *
register_pointer(uint16_t button)
{
	struct input_capability capabilities[] = {
	    {EV_SYN, SYN_REPORT}, {EV_KEY, button},
	    {EV_REL, REL_X}, {EV_REL, REL_Y}};
	const struct input_device_info info = {
	    .name = "host pointer",
	    .physical_path = "host/pointer",
	    .capabilities = capabilities,
	    .capability_count = sizeof(capabilities) / sizeof(capabilities[0]),
	};
	struct input_device *device = NULL;

	assert(input_device_register(&info, &device) == 0);
	assert(device != NULL);
	return device;
}

static void
expect_key_report(size_t index, const struct input_device *device,
	uint16_t code, int32_t value)
{
	assert(index < captured_count);
	assert(captured[index].device == device);
	assert(captured[index].flags == 0);
	assert(captured[index].event_count == 2);
	assert(captured[index].events[0].event.type == EV_KEY);
	assert(captured[index].events[0].event.code == code);
	assert(captured[index].events[0].event.value == value);
	assert(captured[index].events[1].event.type == EV_SYN);
	assert(captured[index].events[1].event.code == SYN_REPORT);
}

static void
test_momentary(void)
{
	struct input_device *device =
	    register_keyboard(INPUT_DEVICE_KEY_MOMENTARY);
	struct hal_key_event press = key_event("a", HAL_KEY_EVENT_PRESS);

	clear_capture();
	input_device_emit_key_event(device, &press);
	assert(captured_count == 1);
	assert(captured[0].device == device && captured[0].event_count == 3);
	assert(captured[0].events[0].event.type == EV_KEY &&
	    captured[0].events[0].event.code == KEY_A &&
	    captured[0].events[0].event.value == 1);
	assert(captured[0].events[1].event.type == EV_KEY &&
	    captured[0].events[1].event.code == KEY_A &&
	    captured[0].events[1].event.value == 0);
	assert(captured[0].events[2].event.type == EV_SYN);
	assert(captured[0].events[0].event.time.tv_sec ==
	    captured[0].events[1].event.time.tv_sec);
	assert(captured[0].events[0].event.time.tv_usec ==
	    captured[0].events[1].event.time.tv_usec);

	/* The synthetic release cleared state: detach has no extra key release. */
	clear_capture();
	input_device_unregister(device);
	assert(captured_count == 1);
	assert(captured[0].device == device &&
	    captured[0].flags == INPUT_REPORT_DETACH &&
	    captured[0].event_count == 0);
}

static void
test_two_physical_keyboards(void)
{
	struct input_device *first =
	    register_keyboard(INPUT_DEVICE_KEY_REPEAT);
	struct input_device *second =
	    register_keyboard(INPUT_DEVICE_KEY_REPEAT);
	struct hal_key_event shift =
	    key_event("leftshift", HAL_KEY_EVENT_PRESS);
	struct hal_key_event press = key_event("a", HAL_KEY_EVENT_PRESS);
	struct hal_key_event repeat = key_event("a", HAL_KEY_EVENT_REPEAT);
	struct hal_key_event release = key_event("a", HAL_KEY_EVENT_RELEASE);

	clear_capture();
	input_device_emit_key_event(first, &shift);
	input_device_emit_key_event(second, &press);
	input_device_emit_key_event(second, &repeat);
	input_device_emit_key_event(second, &release);
	assert(captured_count == 4);
	expect_key_report(0, first, KEY_LEFTSHIFT, 1);
	expect_key_report(1, second, KEY_A, 1);
	expect_key_report(2, second, KEY_A, 2);
	expect_key_report(3, second, KEY_A, 0);
	assert(captured[0].device_id != captured[1].device_id);

	/* Only the first device still holds a key at detach. */
	clear_capture();
	input_device_unregister(first);
	assert(captured_count == 2);
	expect_key_report(0, first, KEY_LEFTSHIFT, 0);
	assert(captured[1].device == first &&
	    captured[1].flags == INPUT_REPORT_DETACH);
	clear_capture();
	input_device_unregister(second);
	assert(captured_count == 1 &&
	    captured[0].flags == INPUT_REPORT_DETACH);
}

static void
test_two_pointers(void)
{
	struct input_device *first = register_pointer(BTN_LEFT);
	struct input_device *second = register_pointer(BTN_RIGHT);

	input_device_emit(first, EV_KEY, BTN_LEFT, 1);
	input_device_emit(second, EV_KEY, BTN_RIGHT, 1);
	clear_capture();
	input_device_unregister(first);
	assert(captured_count == 2);
	expect_key_report(0, first, BTN_LEFT, 0);
	assert(captured[1].flags == INPUT_REPORT_DETACH);
	clear_capture();
	input_device_unregister(second);
	assert(captured_count == 2);
	expect_key_report(0, second, BTN_RIGHT, 0);
	assert(captured[1].flags == INPUT_REPORT_DETACH);
}

static void
test_resync_transaction(void)
{
	struct input_device *device =
	    register_keyboard(INPUT_DEVICE_KEY_REPEAT);
	struct test_file test;
	const struct cdev *cdev = device_cdev(device);
	struct hal_key_event press = key_event("a", HAL_KEY_EVENT_PRESS);
	struct hal_key_event begin = key_event("", HAL_KEY_EVENT_RESYNC |
	    HAL_KEY_EVENT_LOCK_CAPS | HAL_KEY_EVENT_LOCK_KANA);
	struct hal_key_event snapshot = key_event("leftshift",
	    HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT);
	struct hal_key_event end = key_event("", HAL_KEY_EVENT_RESYNC_END);
	struct input_event events[4];
	unsigned long bits[INPUT_BIT_WORDS(KEY_MAX)];
	ssize_t count;

	test_file_init(&test, device);
	assert(cdev->ops->open(&test.file) == 0);
	input_device_emit_key_event(device, &press);
	count = cdev->ops->read(&test.file, events, sizeof(events));
	assert(count == (ssize_t)(2U * sizeof(events[0])));

	clear_capture();
	input_device_emit_key_event(device, &begin);
	/* Normal transitions inside an incomplete stream fail closed. */
	input_device_emit_key_event(device, &press);
	memset(bits, 0, sizeof(bits));
	assert(cdev->ops->ioctl(&test.file, EVIOCGKEY(sizeof(bits)),
	    (uintptr_t)bits) == 0);
	assert(key_bit(bits, KEY_A) && !key_bit(bits, KEY_LEFTSHIFT));
	input_device_emit_key_event(device, &snapshot);
	/* A partial snapshot is never exposed through EVIOCGKEY. */
	memset(bits, 0, sizeof(bits));
	assert(cdev->ops->ioctl(&test.file, EVIOCGKEY(sizeof(bits)),
	    (uintptr_t)bits) == 0);
	assert(key_bit(bits, KEY_A) && !key_bit(bits, KEY_LEFTSHIFT));
	input_device_emit_key_event(device, &snapshot); /* duplicate: ignored */
	input_device_emit_key_event(device, &end);
	memset(bits, 0, sizeof(bits));
	assert(cdev->ops->ioctl(&test.file, EVIOCGKEY(sizeof(bits)),
	    (uintptr_t)bits) == 0);
	assert(!key_bit(bits, KEY_A) && key_bit(bits, KEY_LEFTSHIFT));
	count = cdev->ops->read(&test.file, events, sizeof(events));
	assert(count == (ssize_t)(2U * sizeof(events[0])));
	assert(events[0].type == EV_SYN && events[0].code == SYN_DROPPED);
	assert(events[1].type == EV_SYN && events[1].code == SYN_REPORT);
	assert(captured_count == 3);
	assert(captured[0].flags == (INPUT_REPORT_RESYNC_BEGIN |
	    INPUT_REPORT_LOCK_CAPS | INPUT_REPORT_LOCK_KANA));
	assert(captured[0].event_count == 0);
	assert(captured[1].flags == INPUT_REPORT_SNAPSHOT &&
	    captured[1].event_count == 1 &&
	    captured[1].events[0].event.code == KEY_LEFTSHIFT);
	assert(captured[2].flags == INPUT_REPORT_RESYNC_END &&
	    captured[2].event_count == 0);
	assert(cdev->ops->close(&test.file) == 0);
	clear_capture();
	input_device_unregister(device);
	assert(captured_count == 2);
	expect_key_report(0, device, KEY_LEFTSHIFT, 0);
	assert(captured[1].flags == INPUT_REPORT_DETACH);
}

struct callback_context {
	volatile unsigned entered;
	volatile unsigned release;
	volatile unsigned close_calls;
	volatile unsigned block_close;
	volatile unsigned close_entered;
	volatile unsigned close_release;
	volatile unsigned unregister_started;
	volatile unsigned unregister_returned;
	volatile unsigned callback_after_return;
	struct input_device *device;
	struct test_file *file;
	int open_result;
};

static int
producer_open(void *argument)
{
	struct callback_context *context = argument;

	__atomic_store_n(&context->entered, 1U, __ATOMIC_RELEASE);
	while (__atomic_load_n(&context->release, __ATOMIC_ACQUIRE) == 0)
		sched_yield();
	if (__atomic_load_n(&context->unregister_returned, __ATOMIC_ACQUIRE))
		__atomic_store_n(&context->callback_after_return, 1U,
		    __ATOMIC_RELEASE);
	return 0;
}

static void
producer_close(void *argument)
{
	struct callback_context *context = argument;

	if (__atomic_load_n(&context->unregister_returned, __ATOMIC_ACQUIRE))
		__atomic_store_n(&context->callback_after_return, 1U,
		    __ATOMIC_RELEASE);
	(void)__atomic_add_fetch(&context->close_calls, 1U, __ATOMIC_RELEASE);
	if (__atomic_load_n(&context->block_close, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&context->close_entered, 1U, __ATOMIC_RELEASE);
		while (!__atomic_load_n(&context->close_release,
		    __ATOMIC_ACQUIRE))
			sched_yield();
	}
	if (__atomic_load_n(&context->unregister_returned, __ATOMIC_ACQUIRE))
		__atomic_store_n(&context->callback_after_return, 1U,
		    __ATOMIC_RELEASE);
}

static void *
open_worker(void *argument)
{
	struct callback_context *context = argument;
	const struct cdev *cdev = device_cdev(context->device);

	context->open_result = cdev->ops->open(&context->file->file);
	return NULL;
}

static void *
unregister_worker(void *argument)
{
	struct callback_context *context = argument;

	__atomic_store_n(&context->unregister_started, 1U, __ATOMIC_RELEASE);
	input_device_unregister(context->device);
	__atomic_store_n(&context->unregister_returned, 1U, __ATOMIC_RELEASE);
	return NULL;
}

struct unregister_call {
	struct input_device *device;
	volatile unsigned started;
	volatile unsigned returned;
};

static void *
concurrent_unregister_worker(void *argument)
{
	struct unregister_call *call = argument;

	__atomic_store_n(&call->started, 1U, __ATOMIC_RELEASE);
	input_device_unregister(call->device);
	__atomic_store_n(&call->returned, 1U, __ATOMIC_RELEASE);
	return NULL;
}

static void
test_callback_retirement(void)
{
	struct callback_context context;
	struct test_file test;
	struct unregister_call first_call, second_call;
	pthread_t opener, unregisterer, first_remover, second_remover;
	const struct cdev *cdev;
	unsigned spins;

	memset(&context, 0, sizeof(context));
	context.file = &test;
	context.device = register_keyboard_callbacks(producer_open,
	    producer_close, &context);
	test_file_init(&test, context.device);
	assert(pthread_create(&opener, NULL, open_worker, &context) == 0);
	while (!__atomic_load_n(&context.entered, __ATOMIC_ACQUIRE))
		sched_yield();
	assert(pthread_create(&unregisterer, NULL, unregister_worker,
	    &context) == 0);
	while (!__atomic_load_n(&context.unregister_started, __ATOMIC_ACQUIRE))
		sched_yield();
	for (spins = 0; spins < 10000U; spins++)
		sched_yield();
	assert(!__atomic_load_n(&context.unregister_returned, __ATOMIC_ACQUIRE));
	__atomic_store_n(&context.release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(opener, NULL) == 0);
	assert(pthread_join(unregisterer, NULL) == 0);
	assert(context.open_result == ENODEV);
	assert(context.close_calls == 1);
	assert(context.callback_after_return == 0);

	/*
	 * An already-open reader transfers its close to one unregister exactly
	 * once.  A concurrent remover must join that close and terminal DETACH,
	 * rather than returning while the first remover still owns the context.
	 */
	memset(&context, 0, sizeof(context));
	context.release = 1;
	context.block_close = 1;
	context.file = &test;
	context.device = register_keyboard_callbacks(producer_open,
	    producer_close, &context);
	test_file_init(&test, context.device);
	cdev = device_cdev(context.device);
	assert(cdev->ops->open(&test.file) == 0);
	clear_capture();
	memset(&first_call, 0, sizeof(first_call));
	memset(&second_call, 0, sizeof(second_call));
	first_call.device = context.device;
	second_call.device = context.device;
	assert(pthread_create(&first_remover, NULL,
	    concurrent_unregister_worker, &first_call) == 0);
	while (!__atomic_load_n(&context.close_entered, __ATOMIC_ACQUIRE))
		sched_yield();
	assert(pthread_create(&second_remover, NULL,
	    concurrent_unregister_worker, &second_call) == 0);
	while (!__atomic_load_n(&second_call.started, __ATOMIC_ACQUIRE))
		sched_yield();
	for (spins = 0; spins < 10000U; spins++)
		sched_yield();
	assert(!__atomic_load_n(&first_call.returned, __ATOMIC_ACQUIRE));
	assert(!__atomic_load_n(&second_call.returned, __ATOMIC_ACQUIRE));
	assert(__atomic_load_n(&context.close_calls, __ATOMIC_ACQUIRE) == 1);
	assert(captured_count == 0);
	__atomic_store_n(&context.close_release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(first_remover, NULL) == 0);
	assert(pthread_join(second_remover, NULL) == 0);
	assert(first_call.returned && second_call.returned);
	context.unregister_returned = 1;
	assert(context.close_calls == 1);
	assert(captured_count == 1 &&
	    captured[0].flags == INPUT_REPORT_DETACH);
	assert(cdev->ops->close(&test.file) == 0);
	assert(context.close_calls == 1 && context.callback_after_return == 0);
}

static void
test_callback_pair_validation(void)
{
	static const struct input_capability capabilities[] = {
	    {EV_SYN, SYN_REPORT}};
	struct input_device *device = NULL;
	struct input_device_info info = {
	    .name = "invalid callback pair",
	    .capabilities = capabilities,
	    .capability_count = sizeof(capabilities) / sizeof(capabilities[0]),
	    .open = producer_open,
	};

	assert(input_device_register(&info, &device) == EINVAL);
	info.open = NULL;
	info.close = producer_close;
	assert(input_device_register(&info, &device) == EINVAL);
}

int
main(void)
{
	struct input_subscription subscription;

	memset(&subscription, 0, sizeof(subscription));
	registered_count = 0;
	input_core_init();
	assert(input_subscribe(&subscription, capture, NULL) == 0);
	test_callback_pair_validation();
	test_registration_publication_and_rollback();
	test_momentary();
	assert(cdev_probe_error == 0);
	cdev_probe_open = 0;
	test_two_physical_keyboards();
	test_two_pointers();
	test_resync_transaction();
	test_callback_retirement();
	input_unsubscribe(&subscription);
	puts("WS006 production input-device ownership: PASS");
	return 0;
}
