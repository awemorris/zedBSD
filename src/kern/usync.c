/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * User-space synchronization waits.
 *
 * A wait compares one aligned user word against an expected value and
 * sleeps until a wake on the same key.  Keys are hashed into a small set of
 * buckets, each with its own lock and wait queue, so unrelated waiters
 * rarely share a queue but sharing is always safe.
 */

#include "kern/usync.h"
#include "kern/lock.h"
#include "kern/process.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"
#include <errno.h>
#include <hal/hal.h>

#define USYNC_BUCKETS 32U

struct usync_bucket {
	struct spinlock lock;
	struct wait_queue waiters;
};

static struct usync_bucket buckets[USYNC_BUCKETS];

static struct usync_bucket *usync_bucket_for(uintptr_t key_object, uintptr_t key_offset);

/*
 * Initializes every wait bucket.
 */
void
usync_init(
	void)
{
	unsigned i;

	/* Creates the lock and wait queue of each bucket. */
	for (i = 0; i < USYNC_BUCKETS; i++) {
		spin_init(&buckets[i].lock, LOCK_RANK_USYNC, "usync");
		waitq_init(&buckets[i].waiters, "usync wait");
	}
}

/*
 * Wakes every waiter after the realtime clock changed.
 *
 * Absolute realtime deadlines may have moved, so every sleeper must
 * re-evaluate its own timeout.
 */
void
usync_realtime_changed(
	void)
{
	struct usync_bucket *bucket;
	unsigned long irq;
	unsigned i;

	/* Wakes each bucket in turn. */
	for (i = 0; i < USYNC_BUCKETS; i++) {
		bucket = &buckets[i];
		irq = spin_lock_irqsave(&bucket->lock);
		waitq_wake_all(&bucket->waiters);
		spin_unlock_irqrestore(&bucket->lock, irq);
	}
}

/*
 * Sleeps until a wake on the key while the user word still holds the
 * expected value.
 *
 * A word that already differs returns EAGAIN without sleeping, as does a
 * wake that arrives between the value check and the sleep; the caller
 * re-evaluates its condition in both cases.
 */
int
usync_wait(
	uintptr_t address,
	uint32_t expected,
	uintptr_t process_key,
	uintptr_t key_offset,
	uint64_t deadline,
	int cancelable)
{
	struct usync_bucket *bucket;
	struct uaccess_pin pin;
	uint32_t actual;
	uint64_t sequence;
	unsigned flags;
	unsigned long irq;
	int error;

	/* Rejects a missing key or a misaligned word. */
	if (process_key == 0 || address == 0 || (address & 3U) != 0)
		return EINVAL;

	bucket = usync_bucket_for(process_key, key_offset);

	/*
	 * Observe the wake generation before reading the user word.  A wake in
	 * any later gap makes waitq_sleep() return EAGAIN instead of registering
	 * a lost waiter.  No user backing pin is held while sleeping.
	 */
	irq = spin_lock_irqsave(&bucket->lock);
	sequence = waitq_sequence(&bucket->waiters);
	spin_unlock_irqrestore(&bucket->lock, irq);

	/* Reads the user word through a bounded pin. */
	error = uaccess_pin(address, sizeof(actual), HAL_SPACE_READ, &pin);
	if (error != 0)
		return error;
	error = copyin_pinned(&pin, 0, &actual, sizeof(actual));
	uaccess_unpin(&pin);
	if (error != 0)
		return error;

	/* Refuses to sleep on a word that already changed. */
	if (actual != expected)
		return EAGAIN;

	/* Sleeps on the bucket; a lost-wake refusal counts as a wake. */
	flags = WAITQ_INTERRUPTIBLE;
	if (cancelable)
		flags |= WAITQ_CANCELABLE;
	irq = spin_lock_irqsave(&bucket->lock);
	error = waitq_sleep(&bucket->waiters, &bucket->lock, sequence, deadline, flags);
	if (error == EAGAIN)
		error = 0;
	spin_unlock_irqrestore(&bucket->lock, irq);

	/* Reports the sleep result. */
	return error;
}

/*
 * Wakes waiters on one key.
 */
int
usync_wake(
	uintptr_t address,
	uintptr_t process_key,
	uintptr_t key_offset,
	unsigned count)
{
	struct usync_bucket *bucket;
	unsigned long irq;

	/* Rejects a missing key or a misaligned word. */
	if (process_key == 0 || address == 0 || (address & 3U) != 0)
		return EINVAL;

	/* Wakes nobody when asked for nobody. */
	if (count == 0)
		return 0;

	bucket = usync_bucket_for(process_key, key_offset);

	/*
	 * Buckets deliberately combine multiple synchronization keys.  A
	 * single-waiter wake could therefore select a waiter for another key
	 * and leave the requested key asleep indefinitely.  Wake the bucket
	 * and let every waiter revalidate its own value instead.
	 */
	irq = spin_lock_irqsave(&bucket->lock);
	waitq_wake_all(&bucket->waiters);
	spin_unlock_irqrestore(&bucket->lock, irq);

	/* Reports the wake. */
	return 0;
}

/* Hashes a synchronization key to its bucket. */
static struct usync_bucket *
usync_bucket_for(
	uintptr_t key_object,
	uintptr_t key_offset)
{
	uintptr_t key;

	/* Mixes the object identity with the word offset. */
	key = (key_object >> 4) ^ (key_offset >> 2);

	/* Reports the bucket. */
	return &buckets[key % USYNC_BUCKETS];
}
