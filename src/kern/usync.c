/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/usync.h"
#include "kern/lock.h"
#include "kern/process.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"
#include <errno.h>
#include <hal/hal.h>

#define USYNC_BUCKETS 32U
struct usync_bucket { struct spinlock lock; struct wait_queue waiters; };
static struct usync_bucket buckets[USYNC_BUCKETS];

static struct usync_bucket *
usync_bucket_for(uintptr_t key_object, uintptr_t key_offset)
{
	uintptr_t key = (key_object >> 4) ^ (key_offset >> 2);
	return &buckets[key % USYNC_BUCKETS];
}

void usync_init(void)
{
	unsigned i;
	for (i = 0; i < USYNC_BUCKETS; i++) {
		spin_init(&buckets[i].lock, LOCK_RANK_USYNC, "usync");
		waitq_init(&buckets[i].waiters, "usync wait");
	}
}

int
usync_wait(uintptr_t address, uint32_t expected, uintptr_t process_key,
	uintptr_t key_offset, uint64_t deadline, int cancelable)
{
	struct usync_bucket *bucket;
	struct uaccess_pin pin;
	uint32_t actual;
	uint64_t sequence;
	unsigned long irq;
	int error;

	if (process_key == 0 || address == 0 || (address & 3U) != 0)
		return EINVAL;
	bucket = usync_bucket_for(process_key, key_offset);
	/*
	 * Observe the wake generation before reading the user word.  A wake in any
	 * later gap makes waitq_sleep() return EAGAIN instead of registering a lost
	 * waiter.  No user backing pin is held while sleeping.
	 */
	irq = spin_lock_irqsave(&bucket->lock);
	sequence = waitq_sequence(&bucket->waiters);
	spin_unlock_irqrestore(&bucket->lock, irq);
	error = uaccess_pin(address, sizeof(actual), HAL_SPACE_READ, &pin);
	if (error != 0)
		return error;
	error = copyin_pinned(&pin, 0, &actual, sizeof(actual));
	uaccess_unpin(&pin);
	if (error != 0)
		return error;
	if (actual != expected)
		return EAGAIN;
	irq = spin_lock_irqsave(&bucket->lock);
	error = waitq_sleep(&bucket->waiters, &bucket->lock, sequence,
	    deadline, WAITQ_INTERRUPTIBLE |
	    (cancelable ? WAITQ_CANCELABLE : 0));
	if (error == EAGAIN)
		error = 0;
	spin_unlock_irqrestore(&bucket->lock, irq);
	return error;
}

int
usync_wake(uintptr_t address, uintptr_t process_key, uintptr_t key_offset,
	unsigned count)
{
	struct usync_bucket *bucket;
	unsigned long irq;
	if (process_key == 0 || address == 0 || (address & 3U) != 0)
		return EINVAL;
	if (count == 0)
		return 0;
	bucket = usync_bucket_for(process_key, key_offset);
	irq = spin_lock_irqsave(&bucket->lock);
	/*
	 * Buckets deliberately combine multiple synchronization keys.  A
	 * single-waiter wake could therefore select a waiter for another key
	 * and leave the requested key asleep indefinitely.  Wake the bucket
	 * and let every waiter revalidate its own value instead.
	 */
	waitq_wake_all(&bucket->waiters);
	spin_unlock_irqrestore(&bucket->lock, irq);
	return 0;
}
