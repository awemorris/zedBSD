/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/lock.h"
#include "kern/uaccess.h"
#include "kern/usync.h"
#include "kern/waitq.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uintptr_t fake_address = 0x4000;
static const uintptr_t fake_process_key = 0x8000;
static const uintptr_t fake_key_offset = 0x40;
static uint32_t fake_word;
static int pin_active;
static int inject_wake_on_unpin;
static unsigned early_generation_changes;
static unsigned registered_sleeps;
static unsigned wake_calls;
static unsigned last_wait_flags;

int
uaccess_pin(uintptr_t address, size_t size, uint32_t prot,
	struct uaccess_pin *pin)
{
	assert(address == fake_address && size == sizeof(fake_word));
	assert((prot & HAL_SPACE_READ) != 0 && pin != NULL && !pin_active);
	memset(pin, 0, sizeof(*pin));
	pin->active = 1;
	pin->size = size;
	pin->prot = prot;
	pin_active = 1;
	return 0;
}

int
copyin_pinned(const struct uaccess_pin *pin, size_t offset,
	void *destination, size_t size)
{
	assert(pin != NULL && pin->active && pin_active && offset == 0 &&
	    size == sizeof(fake_word));
	memcpy(destination, &fake_word, size);
	return 0;
}

void
uaccess_unpin(struct uaccess_pin *pin)
{
	assert(pin != NULL && pin->active && pin_active);
	pin->active = 0;
	pin_active = 0;
	if (inject_wake_on_unpin) {
		inject_wake_on_unpin = 0;
		assert(usync_wake(fake_address, fake_process_key, fake_key_offset,
		    1) == 0);
	}
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

void
spin_lock(struct spinlock *lock)
{
	assert(lock->held.value == 0);
	lock->held.value = 1;
}

int
spin_trylock(struct spinlock *lock)
{
	if (lock->held.value != 0)
		return 0;
	lock->held.value = 1;
	return 1;
}

void spin_unlock(struct spinlock *lock)
{
	assert(lock->held.value == 1);
	lock->held.value = 0;
}

unsigned long spin_lock_irqsave(struct spinlock *lock)
{
	spin_lock(lock);
	return 0;
}

void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{
	(void)irq;
	spin_unlock(lock);
}

void
waitq_init(struct wait_queue *queue, const char *name)
{
	memset(queue, 0, sizeof(*queue));
	queue->sequence = 1;
	queue->name = name;
}

uint64_t waitq_sequence(const struct wait_queue *queue)
{
	return queue->sequence;
}

int
waitq_sleep(struct wait_queue *queue, struct spinlock *condition_lock,
	uint64_t observed, uint64_t deadline, unsigned flags)
{
	(void)deadline;
	assert(condition_lock != NULL && condition_lock->held.value == 1);
	assert(flags == WAITQ_INTERRUPTIBLE ||
	    flags == (WAITQ_INTERRUPTIBLE | WAITQ_CANCELABLE));
	last_wait_flags = flags;
	/* The compared user backing must already have been released. */
	assert(!pin_active);
	if (queue->sequence != observed) {
		early_generation_changes++;
		return EAGAIN;
	}
	registered_sleeps++;
	/* Simulate a wake after registration without changing lock ownership. */
	queue->sequence++;
	return 0;
}

void
waitq_wake_one(struct wait_queue *queue)
{
	queue->sequence++;
}

void
waitq_wake_all(struct wait_queue *queue)
{
	queue->sequence++;
	wake_calls++;
}

int
main(void)
{
	usync_init();

	/* A mismatching value never reaches wait registration, but is unpinned. */
	fake_word = 7;
	assert(usync_wait(fake_address, 8, fake_process_key, fake_key_offset, 0,
	    0) ==
	    EAGAIN);
	assert(!pin_active && registered_sleeps == 0);

	/* Wake after compare/unpin is observed through the old sequence snapshot. */
	fake_word = 9;
	inject_wake_on_unpin = 1;
	assert(usync_wait(fake_address, 9, fake_process_key, fake_key_offset, 0,
	    0) ==
	    0);
	assert(!pin_active && wake_calls == 1 && early_generation_changes == 1 &&
	    registered_sleeps == 0);

	/* With no intervening wake, registration happens only after unpin. */
	assert(usync_wait(fake_address, 9, fake_process_key, fake_key_offset, 0,
	    0) ==
	    0);
	assert(!pin_active && registered_sleeps == 1 &&
	    last_wait_flags == WAITQ_INTERRUPTIBLE);

	/* Only explicitly marked pthread cancellation points expose cancellation
	 * to the bucket wait; internal usync-based locks remain non-cancelable. */
	assert(usync_wait(fake_address, 9, fake_process_key, fake_key_offset, 0,
	    1) == 0);
	assert(registered_sleeps == 2 && last_wait_flags ==
	    (WAITQ_INTERRUPTIBLE | WAITQ_CANCELABLE));

	/* A realtime adjustment invalidates every bucket deadline. */
	usync_realtime_changed();
	assert(wake_calls == 1 + 32);

	puts("zedBSD usync pin/wake host tests: PASS");
	return 0;
}
