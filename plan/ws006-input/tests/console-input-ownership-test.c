/* WS006 IN-T34: production console broker ownership and resync semantics. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/input-device.h"
#include "kern/input-keymap.h"
#include "kern/lock.h"
#include "kern/waitq.h"

#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void console_input_ownership_test_reset(void);
void console_input_ownership_test_publish(const struct input_report *);
int console_input_ownership_test_pop(uint32_t *, unsigned *, unsigned *);
int console_input_ownership_test_state(struct input_device *, unsigned,
    unsigned *, unsigned *, unsigned *, uint16_t *, int *);
void console_input_ownership_test_drain(int);

static unsigned hal_drain_calls;

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
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

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
}

void
hal_cons_drain_input(void)
{
	hal_drain_calls++;
}

static struct input_report
marker(struct input_device *device, unsigned id, unsigned flags)
{
	struct input_report report;

	memset(&report, 0, sizeof(report));
	report.device = device;
	report.device_id = id;
	report.flags = flags;
	return report;
}

static struct input_report
key_report(struct input_device *device, unsigned id, const char *symbol,
	uint16_t code, int value, uint32_t key_flags, unsigned report_flags)
{
	struct input_report report = marker(device, id, report_flags);

	report.event_count = 1;
	report.events[0].event.type = EV_KEY;
	report.events[0].event.code = code;
	report.events[0].event.value = value;
	assert(strlen(symbol) < sizeof(report.events[0].symbol));
	strcpy(report.events[0].symbol, symbol);
	report.events[0].key_flags = key_flags;
	return report;
}

static void
expect_dispatch(uint32_t expected_key, unsigned expected_id,
	unsigned expected_repeat)
{
	uint32_t translated;
	unsigned id, repeat;

	assert(console_input_ownership_test_pop(&translated, &id, &repeat));
	assert((translated & INPUT_KEY_MASK) == expected_key);
	assert(id == expected_id && repeat == expected_repeat);
}

static void
test_state_only_resync(void)
{
	struct input_device *device = (struct input_device *)(uintptr_t)1U;
	struct input_report report;
	unsigned caps, kana, shift;
	uint16_t active;
	int resyncing;

	console_input_ownership_test_reset();
	report = marker(device, 11, INPUT_REPORT_RESYNC_BEGIN |
	    INPUT_REPORT_LOCK_CAPS | INPUT_REPORT_LOCK_KANA);
	console_input_ownership_test_publish(&report);
	report = key_report(device, 11, "leftshift", KEY_LEFTSHIFT, 1,
	    HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT,
	    INPUT_REPORT_SNAPSHOT);
	console_input_ownership_test_publish(&report);
	report = key_report(device, 11, "capslock", KEY_CAPSLOCK, 1,
	    HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT,
	    INPUT_REPORT_SNAPSHOT);
	console_input_ownership_test_publish(&report);
	report = key_report(device, 11, "kana", KEY_RESERVED, 1,
	    HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT,
	    INPUT_REPORT_SNAPSHOT);
	console_input_ownership_test_publish(&report);
	report = key_report(device, 11, "jis-2", KEY_2, 1,
	    HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT,
	    INPUT_REPORT_SNAPSHOT);
	console_input_ownership_test_publish(&report);
	assert(!console_input_ownership_test_pop(NULL, NULL, NULL));
	assert(console_input_ownership_test_state(device, KEY_2, &caps, &kana,
	    &shift, &active, &resyncing));
	assert(caps == 1 && kana == 1 && shift == 1 && active == '"' &&
	    resyncing == 1);
	report = marker(device, 11, INPUT_REPORT_RESYNC_END);
	console_input_ownership_test_publish(&report);
	assert(!console_input_ownership_test_pop(NULL, NULL, NULL));

	/* A modifier change makes repeat use and remember current translation. */
	report = key_report(device, 11, "leftshift", KEY_LEFTSHIFT, 0,
	    HAL_KEY_EVENT_RELEASE, 0);
	console_input_ownership_test_publish(&report);
	expect_dispatch(INPUT_KEY_SHIFT_SYMBOL, 11, 0);
	report = key_report(device, 11, "jis-2", KEY_2, 2,
	    HAL_KEY_EVENT_REPEAT, 0);
	console_input_ownership_test_publish(&report);
	expect_dispatch('2', 11, 1);
	report = key_report(device, 11, "jis-2", KEY_2, 0,
	    HAL_KEY_EVENT_RELEASE, 0);
	console_input_ownership_test_publish(&report);
	expect_dispatch('2', 11, 0);
	assert(console_input_ownership_test_state(device, KEY_2, &caps, &kana,
	    &shift, &active, &resyncing));
	assert(caps == 1 && kana == 1 && shift == 0 && active == 0 &&
	    resyncing == 0);
}

static void
test_console_only_detach_and_drain(void)
{
	struct input_device *device = (struct input_device *)(uintptr_t)2U;
	struct input_report report;
	uint32_t translated;
	unsigned id, repeat;

	console_input_ownership_test_reset();
	report = key_report(device, 22, "jis-yen", KEY_RESERVED, 1,
	    HAL_KEY_EVENT_PRESS, 0);
	console_input_ownership_test_publish(&report);
	expect_dispatch('\\', 22, 0);
	report = marker(device, 22, INPUT_REPORT_DETACH);
	console_input_ownership_test_publish(&report);
	assert(console_input_ownership_test_pop(&translated, &id, &repeat));
	assert((translated & INPUT_KEY_MASK) == '\\');
	assert((translated & INPUT_KEY_RELEASE) != 0);
	assert(id == 22 && repeat == 0);

	hal_drain_calls = 0;
	console_input_ownership_test_drain(1);
	assert(hal_drain_calls == 0);
	console_input_ownership_test_drain(0);
	assert(hal_drain_calls == 1);
}

int
main(void)
{
	test_state_only_resync();
	test_console_only_detach_and_drain();
	puts("WS006 production console broker ownership: PASS");
	return 0;
}
