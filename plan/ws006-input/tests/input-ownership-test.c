/* WS006 IN-T30: per-source ownership and internal subscriber contract. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/input-capability.h"
#include "kern/input-device.h"
#include "kern/input-keymap.h"
#include "kern/input-queue.h"
#include "kern/lock.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int callback_entered;
static atomic_int callback_release;
static atomic_int removal_returned;
static unsigned callback_count;

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	assert(pthread_mutex_lock(&host_lock) == 0);
	return 1;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)lock;
	assert(enabled == 1);
	assert(pthread_mutex_unlock(&host_lock) == 0);
}

static void
count_callback(void *context, const struct input_report *report)
{
	unsigned *count = context;
	assert(report != NULL);
	(*count)++;
}

static void
blocking_callback(void *context, const struct input_report *report)
{
	(void)context;
	assert(report != NULL);
	callback_count++;
	atomic_store_explicit(&callback_entered, 1, memory_order_release);
	while (!atomic_load_explicit(&callback_release, memory_order_acquire))
		sched_yield();
}

static void *
publisher(void *argument)
{
	input_subscriber_publish(argument);
	return NULL;
}

static void *
remover(void *argument)
{
	input_unsubscribe(argument);
	atomic_store_explicit(&removal_returned, 1, memory_order_release);
	return NULL;
}

static void
test_subscriber_join(void)
{
	struct input_subscription subscription = {0};
	struct input_report report = {0};
	pthread_t publish_thread, remove_thread;
	unsigned ordinary_count = 0;

	input_subscriber_init();
	assert(input_subscribe(NULL, count_callback, &ordinary_count) == EINVAL);
	assert(input_subscribe(&subscription, NULL, &ordinary_count) == EINVAL);
	assert(input_subscribe(&subscription, count_callback,
	    &ordinary_count) == 0);
	assert(input_subscribe(&subscription, count_callback,
	    &ordinary_count) == EBUSY);
	input_subscriber_publish(&report);
	assert(ordinary_count == 1);
	input_unsubscribe(&subscription);
	input_subscriber_publish(&report);
	assert(ordinary_count == 1);

	memset(&subscription, 0, sizeof(subscription));
	assert(input_subscribe(&subscription, blocking_callback, NULL) == 0);
	atomic_store(&callback_entered, 0);
	atomic_store(&callback_release, 0);
	atomic_store(&removal_returned, 0);
	callback_count = 0;
	assert(pthread_create(&publish_thread, NULL, publisher, &report) == 0);
	while (!atomic_load_explicit(&callback_entered, memory_order_acquire))
		sched_yield();
	assert(pthread_create(&remove_thread, NULL, remover, &subscription) == 0);
	for (unsigned spin = 0; spin < 10000U; spin++)
		sched_yield();
	assert(!atomic_load_explicit(&removal_returned, memory_order_acquire));
	atomic_store_explicit(&callback_release, 1, memory_order_release);
	assert(pthread_join(publish_thread, NULL) == 0);
	assert(pthread_join(remove_thread, NULL) == 0);
	assert(atomic_load(&removal_returned) == 1 && callback_count == 1);
	input_subscriber_publish(&report);
	assert(callback_count == 1);
}

static void
key_event(struct hal_key_event *event, const char *symbol, uint32_t flags)
{
	memset(event, 0, sizeof(*event));
	assert(strlen(symbol) < sizeof(event->symbol));
	strcpy(event->symbol, symbol);
	event->flags = flags;
}

static void
test_two_keyboards(void)
{
	static const char shifted[] = "!@#$%^&*()";
	static const uint16_t digits[] = {
	    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5,
	    KEY_6, KEY_7, KEY_8, KEY_9, KEY_0};
	struct input_keymap_state first, second;
	struct hal_key_event event;
	uint32_t translated;
	unsigned index;

	for (index = 0; index < sizeof(shifted) - 1U; index++) {
		char symbol[2] = {shifted[index], '\0'};
		assert(input_key_from_symbol(symbol) == digits[index]);
	}

	input_keymap_init(&first);
	input_keymap_init(&second);
	key_event(&event, "leftshift", HAL_KEY_EVENT_PRESS);
	assert(input_keymap_translate(&first, &event, &translated));
	key_event(&event, "a", HAL_KEY_EVENT_PRESS);
	assert(input_keymap_translate(&second, &event, &translated));
	assert((translated & INPUT_KEY_MASK) == 'a');
	assert((translated & INPUT_KEY_SHIFT) == 0);
	key_event(&event, "a", HAL_KEY_EVENT_PRESS);
	assert(input_keymap_translate(&first, &event, &translated));
	assert((translated & INPUT_KEY_MASK) == 'A');
	memset(&first, 0, sizeof(first)); /* Detach clears only this source. */
	key_event(&event, "b", HAL_KEY_EVENT_PRESS);
	assert(input_keymap_translate(&second, &event, &translated));
	assert((translated & INPUT_KEY_MASK) == 'b');
}

static int
key_down(const struct input_capability_state *state, unsigned code)
{
	const uint8_t *bits;
	size_t size;
	unsigned long word;
	assert(input_capability_key_state(state, &bits, &size) == 0);
	assert((code / INPUT_BITS_PER_WORD + 1U) * sizeof(word) <= size);
	memcpy(&word, bits + code / INPUT_BITS_PER_WORD * sizeof(word),
	    sizeof(word));
	return (word & (1UL << (code % INPUT_BITS_PER_WORD))) != 0;
}

static void
test_two_pointers_and_momentary(void)
{
	static const struct input_capability capabilities[] = {
	    {EV_SYN, SYN_REPORT}, {EV_KEY, BTN_LEFT}, {EV_KEY, BTN_RIGHT},
	    {EV_REL, REL_X}, {EV_REL, REL_Y}};
	struct input_capability_state first, second;
	struct input_report report = {0};

	assert(input_capability_state_init(&first, capabilities,
	    sizeof(capabilities) / sizeof(capabilities[0]), NULL, 0) == 0);
	assert(input_capability_state_init(&second, capabilities,
	    sizeof(capabilities) / sizeof(capabilities[0]), NULL, 0) == 0);
	assert(input_capability_event(&first, EV_KEY, BTN_LEFT, 1));
	assert(input_capability_event(&second, EV_KEY, BTN_RIGHT, 1));
	assert(key_down(&first, BTN_LEFT) && !key_down(&first, BTN_RIGHT));
	assert(!key_down(&second, BTN_LEFT) && key_down(&second, BTN_RIGHT));
	assert(input_capability_event(&first, EV_KEY, BTN_LEFT, 0));
	assert(!key_down(&first, BTN_LEFT) && key_down(&second, BTN_RIGHT));

	/* A character-only adapter publishes one indivisible report. */
	report.event_count = 3;
	report.events[0].event =
	    (struct input_event){.type = EV_KEY, .code = KEY_A, .value = 1};
	report.events[1].event =
	    (struct input_event){.type = EV_KEY, .code = KEY_A, .value = 0};
	report.events[2].event =
	    (struct input_event){.type = EV_SYN, .code = SYN_REPORT, .value = 0};
	assert(report.event_count <= INPUT_REPORT_EVENT_MAX);
	assert(report.events[0].event.value == 1 &&
	    report.events[1].event.value == 0 &&
	    report.events[2].event.type == EV_SYN);
}

static void
test_reader_overflow(void)
{
	struct input_queue queue;
	struct input_queue_reader reader;
	struct input_event event = {.type = EV_KEY, .code = KEY_A, .value = 1};
	struct input_event output[4];
	size_t count;

	input_queue_init(&queue);
	input_queue_reader_init(&queue, &reader);
	for (unsigned index = 0; index < INPUT_QUEUE_CAPACITY + 9U; index++)
		input_queue_push(&queue, &event);
	count = input_queue_read(&queue, &reader, output,
	    sizeof(output) / sizeof(output[0]));
	assert(count == 4);
	assert(output[0].type == EV_SYN && output[0].code == SYN_DROPPED);
}

int
main(void)
{
	test_subscriber_join();
	test_two_keyboards();
	test_two_pointers_and_momentary();
	test_reader_overflow();
	puts("WS006 input ownership/subscriber: PASS");
	return 0;
}
