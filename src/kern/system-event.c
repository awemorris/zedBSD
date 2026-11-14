/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's events (ws132-p002): power buttons, the lid, the AC
 * adapter, batteries, and devices coming and going, posted by the drivers
 * and read by whoever opened /dev/system and subscribed to their classes
 * (the desktop, which acts on them; plan/ws132/phase001).
 *
 * Each subscriber keeps a ring of KERN_SYSTEM_EVENT_QUEUE records.  A post
 * puts its record in the ring of every subscriber of its class, under one
 * spinlock taken with interrupts off, and allocates nothing, so a driver
 * may post from an interrupt or from the ACPI event thread.  A full ring
 * drops its oldest record and counts it; the next read starts with an
 * OVERFLOW record of the count, which tells the reader to ask for the
 * present state again.  The sequence number goes up by one for each post,
 * whoever receives it.
 */

#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/poll.h>
#include <kern/system-event.h>
#include <kern/waitq.h>
#include <uapi/errno.h>


/*
 * One subscriber: the classes it wants, its ring (the oldest record at
 * head, count of them), how many records it lost since its last read, and
 * the queue its blocked reads sleep on.  It is linked into the subscriber
 * list from open to close; event_lock protects every field but the queue.
 */
/* The subscribers one event's polls are woken for by name; past them every poll is woken. */
#define EVENT_NOTIFY_MAX	16U

struct kern_system_subscriber {
	struct kern_system_subscriber *next;
	uint32_t classes;
	unsigned head;
	unsigned count;
	uint32_t lost;
	struct system_event ring[KERN_SYSTEM_EVENT_QUEUE];
	struct wait_queue waitq;
};

/*
 * The subscribers, newest first, and the number given to the next post.
 *
 * event_lock (taken with interrupts off) protects both and every
 * subscriber's ring; the list is empty and the sequence 0 until the first
 * open and post.  The lock is made by the first open or post;
 * event_lock_state is 0 before, 1 while it is being made, and 2 after.
 */
static struct kern_system_subscriber *subscribers;
static uint64_t next_sequence;
static struct spinlock event_lock;
static atomic_uint_t event_lock_state;

static void lock_ready(void);
static void copy_text(char *destination, const char *source, size_t size);
static void ring_push(struct kern_system_subscriber *subscriber, const struct system_event *event);

/*
 * Posts one event to every subscriber of its class.
 */
void
kern_system_event_post(
	uint32_t class_bit,
	uint32_t action,
	int32_t value,
	const char *subject,
	const char *detail)
{
	struct kern_system_subscriber *told[EVENT_NOTIFY_MAX];
	struct kern_system_subscriber *subscriber;
	struct system_event event;
	unsigned long irq;
	unsigned delivered;
	unsigned index;

	/* The record, but for its sequence number. */
	kern_memset(&event, 0, sizeof(event));
	event.size = sizeof(event);
	event.class_bit = class_bit;
	event.action = action;
	event.value = value;
	event.time_ns = clock_milliseconds(NULL) * 1000000U;
	copy_text(event.subject, subject, sizeof(event.subject));
	copy_text(event.detail, detail, sizeof(event.detail));

	/* Puts it in the ring of every subscriber of its class. */
	lock_ready();
	irq = spin_lock_irqsave(&event_lock);

	event.sequence = next_sequence;
	next_sequence++;
	delivered = 0U;
	for (subscriber = subscribers; subscriber != NULL; subscriber = subscriber->next) {
		/* A subscriber of other classes does not hear it. */
		if ((subscriber->classes & class_bit) == 0U)
			continue;

		/* Its ring takes the record, and its reads wake. */
		ring_push(subscriber, &event);
		waitq_wake_all(&subscriber->waitq);
		if (delivered < EVENT_NOTIFY_MAX)
			told[delivered] = subscriber;
		delivered++;
	}

	spin_unlock_irqrestore(&event_lock, irq);

	/* The polls waiting for those subscribers look again (all polls, past what the list holds). */
	if (delivered > EVENT_NOTIFY_MAX) {
		poll_notify();
		return;
	}

	/* The polls following each told subscriber look again. */
	for (index = 0; index < delivered; index++)
		poll_notify_object(told[index]);
}

/*
 * Adds a subscriber of no class yet.
 */
int
kern_system_event_open(
	struct kern_system_subscriber **result)
{
	struct kern_system_subscriber *subscriber;
	unsigned long irq;

	/* The subscriber's state. */
	subscriber = kern_calloc(1U, sizeof(*subscriber));
	if (subscriber == NULL)
		return ENOMEM;
	waitq_init(&subscriber->waitq, "system event");

	/* Links it into the list. */
	lock_ready();
	irq = spin_lock_irqsave(&event_lock);

	subscriber->next = subscribers;
	subscribers = subscriber;

	spin_unlock_irqrestore(&event_lock, irq);

	/* Succeeded: the subscriber hears nothing until it subscribes. */
	*result = subscriber;
	return 0;
}

/*
 * Removes a subscriber and frees it.
 */
void
kern_system_event_close(
	struct kern_system_subscriber *subscriber)
{
	struct kern_system_subscriber **link;
	unsigned long irq;

	/* Nothing was opened. */
	if (subscriber == NULL)
		return;

	/* Unlinks it. */
	lock_ready();
	irq = spin_lock_irqsave(&event_lock);

	for (link = &subscribers; *link != NULL; link = &(*link)->next) {
		/* Its link is replaced by the next one. */
		if (*link == subscriber) {
			*link = subscriber->next;
			break;
		}
	}

	spin_unlock_irqrestore(&event_lock, irq);

	/* No post can reach it any more. */
	kern_free(subscriber);
}

/*
 * Sets the classes a subscriber hears from now on.
 */
int
kern_system_event_subscribe(
	struct kern_system_subscriber *subscriber,
	uint32_t classes)
{
	unsigned long irq;

	/* Refuses an unknown class, and a subscription to nothing. */
	if ((classes & ~KERN_SYSTEM_EVENT_CLASSES) != 0U)
		return EINVAL;
	if (classes == 0U)
		return EINVAL;

	/* The classes, and the overflow notice that always comes. */
	irq = spin_lock_irqsave(&event_lock);

	subscriber->classes = classes | KERN_SYSTEM_EVENT_OVERFLOW;

	spin_unlock_irqrestore(&event_lock, irq);

	/* Succeeded: the subscriber hears these classes. */
	return 0;
}

/*
 * Takes up to capacity records out of a subscriber's ring, waiting for one
 * unless nonblock is set.  Reports the number taken, or a negative errno:
 * EINVAL before a subscription, EAGAIN when nothing waits and nonblock is
 * set, EINTR when a signal ends the wait.
 */
ssize_t
kern_system_event_read(
	struct kern_system_subscriber *subscriber,
	struct system_event *events,
	size_t capacity,
	int nonblock)
{
	struct system_event *overflow;
	uint64_t observed;
	unsigned long irq;
	size_t taken;
	int error;

	/* Refuses a read of no record. */
	if (capacity == 0U)
		return -EINVAL;

	/* Waits until records or a lost count wait. */
	irq = spin_lock_irqsave(&event_lock);

	for (;;) {
		/* A subscriber of nothing reads nothing. */
		if (subscriber->classes == 0U) {
			spin_unlock_irqrestore(&event_lock, irq);
			return -EINVAL;
		}

		/* Something to read. */
		if (subscriber->count != 0U || subscriber->lost != 0U)
			break;

		/* Nothing, and the reader does not wait. */
		if (nonblock) {
			spin_unlock_irqrestore(&event_lock, irq);
			return -EAGAIN;
		}

		/* Sleeps until a post wakes it, or a signal ends the wait. */
		observed = waitq_sequence(&subscriber->waitq);
		error = waitq_sleep(&subscriber->waitq, &event_lock, observed, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&event_lock, irq);
			return -EINTR;
		}
	}

	/* The records lost come first, as an OVERFLOW record. */
	taken = 0;
	if (subscriber->lost != 0U) {
		overflow = &events[0];
		kern_memset(overflow, 0, sizeof(*overflow));
		overflow->size = sizeof(*overflow);
		overflow->class_bit = KERN_SYSTEM_EVENT_OVERFLOW;
		overflow->action = KERN_SYSTEM_EVENT_CHANGE;
		overflow->value = (int32_t)subscriber->lost;
		overflow->sequence = next_sequence;
		copy_text(overflow->subject, "overflow", sizeof(overflow->subject));
		subscriber->lost = 0;
		taken = 1;
	}

	/* Then the oldest records, as many as fit. */
	while (taken < capacity && subscriber->count != 0U) {
		events[taken] = subscriber->ring[subscriber->head];
		subscriber->head = (subscriber->head + 1U) % KERN_SYSTEM_EVENT_QUEUE;
		subscriber->count--;
		taken++;
	}

	spin_unlock_irqrestore(&event_lock, irq);

	/* Succeeded: the number of records taken. */
	return (ssize_t)taken;
}

/*
 * Tells whether a subscriber has records (or a lost count) to read.
 */
int
kern_system_event_readable(
	struct kern_system_subscriber *subscriber)
{
	unsigned long irq;
	int readable;

	/* Looks at its ring. */
	irq = spin_lock_irqsave(&event_lock);

	readable = 0;
	if (subscriber->count != 0U || subscriber->lost != 0U)
		readable = 1;

	spin_unlock_irqrestore(&event_lock, irq);

	/* Succeeded: whether a read would take something now. */
	return readable;
}

/*
 * Makes the event lock the first time it is needed.  Drivers on several
 * CPUs may post their first events at once: one makes the lock, and the
 * others wait until it is made.
 */
static void
lock_ready(void)
{
	unsigned expected;
	unsigned state;
	int won;

	/* Made already, the usual case. */
	state = atomic_load_acquire(&event_lock_state);
	if (state == 2U)
		return;

	/* The caller that moves the state from 0 makes the lock. */
	expected = 0U;
	won = atomic_compare_exchange(&event_lock_state, &expected, 1U);
	if (won) {
		spin_init(&event_lock, LOCK_RANK_DEVICE, "system events");
		atomic_store_release(&event_lock_state, 2U);
		return;
	}

	/* The others wait for it, which takes a few instructions. */
	do {
		state = atomic_load_acquire(&event_lock_state);
	} while (state != 2U);
}

/* Copies a text into a record's field, cut short to fit and always ended. */
static void
copy_text(
	char *destination,
	const char *source,
	size_t size)
{
	size_t index;

	/* No text is an empty one. */
	index = 0;
	if (source != NULL) {
		/* As much as fits, with room for the end. */
		while (source[index] != '\0' && index + 1U < size) {
			destination[index] = source[index];
			index++;
		}
	}

	/* The text's end. */
	destination[index] = '\0';
}

/* Puts a record at the end of a ring, dropping the oldest when the ring is full. */
static void
ring_push(
	struct kern_system_subscriber *subscriber,
	const struct system_event *event)
{
	unsigned tail;

	/* A full ring loses its oldest record, and counts it. */
	if (subscriber->count == KERN_SYSTEM_EVENT_QUEUE) {
		subscriber->head = (subscriber->head + 1U) % KERN_SYSTEM_EVENT_QUEUE;
		subscriber->count--;
		subscriber->lost++;
	}

	/* The record after the newest. */
	tail = (subscriber->head + subscriber->count) % KERN_SYSTEM_EVENT_QUEUE;
	subscriber->ring[tail] = *event;
	subscriber->count++;
}
