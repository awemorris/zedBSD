/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs the recording thread: it sends long recordings' batches and
 * fence-less submissions handed off by vkQueueSubmit, in order, while the
 * application goes on recording.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "internal.h"

/* The environment variable that turns the hand-off of fence-less submissions off (0). */
#define VULKAN_HANDOFF_VARIABLE "VULKAN_ZEDBSD_SUBMIT_HANDOFF"

/*
 * The thread's lock.  It guards the queue of waiting pieces (first and
 * last), each piece's waiting and next, the sleep and waiter counts below,
 * each command buffer's sent_pending and sent_error, and each hand-off
 * ring's busy and room_waiting.
 */
static pthread_mutex_t sender_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * ready wakes the thread when a piece is queued while it sleeps; done
 * wakes the callers waiting for a piece to have been sent (a batch's
 * place, a command buffer's batches, a drain).
 */
static pthread_cond_t sender_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t sender_done = PTHREAD_COND_INITIALIZER;

/* The pieces waiting, the first to be sent next; the lock's. */
static struct vulkan_sender_item *sender_first;
static struct vulkan_sender_item *sender_last;

/*
 * Whether the thread sleeps for want of a piece (only then does queueing
 * one signal ready), and how many callers wait on done (only then does a
 * sent piece broadcast it); the lock's.
 */
static VkBool32 sender_sleeping;
static uint32_t sender_done_waiters;

/* Starts the thread at most once in the process. */
static pthread_once_t sender_once = PTHREAD_ONCE_INIT;

/*
 * Whether the thread runs, set once when it started and cleared in a
 * forked child (which has no such thread); read atomically.
 */
static VkBool32 sender_started;

/* Whether this process is a child forked from one where the thread ran: its pieces are never sent, and nothing waits for them. */
static VkBool32 sender_forked;

/* Whether hand-off is allowed by the environment and the processors: 0 not yet read, 1 no, 2 yes; read atomically. */
static int sender_handoff_state;

/* Set on the recording thread itself, whose own transactions never wait for the pieces it is sending. */
static __thread VkBool32 sender_self;

static void sender_start(void);
static void *sender_main(void *argument);
static void sender_prepare(void);
static void sender_parent(void);
static void sender_child(void);
static VkResult handoff_send(struct vulkan_sender_item *item);
static void handoff_sent(struct vulkan_sender_item *item, VkResult status);

/*
 * Starts the recording thread once and reports whether it runs.
 */
VkBool32
vulkan_sender_running(
	void)
{
	VkBool32 started;

	/* The first caller starts it; a thread that cannot start leaves every piece to its caller. */
	(void)pthread_once(&sender_once, sender_start);
	started = __atomic_load_n(&sender_started, __ATOMIC_ACQUIRE);

	/* Succeeded: whether pieces may be handed over. */
	return started;
}

/*
 * Reports whether the recording thread runs, without starting it.
 */
VkBool32
vulkan_sender_started(
	void)
{
	VkBool32 started;

	/* Set once by the start, cleared in a forked child. */
	started = __atomic_load_n(&sender_started, __ATOMIC_ACQUIRE);

	/* Succeeded: whether pieces may have been handed over. */
	return started;
}

/*
 * Takes the recording thread's lock.
 */
void
vulkan_sender_lock(
	void)
{
	int locked;

	/* A lock that cannot be taken leaves no safe way on. */
	locked = pthread_mutex_lock(&sender_lock);
	if (locked != 0)
		abort();

	/* Succeeded: the caller holds the lock. */
	return;
}

/*
 * Releases the recording thread's lock.
 */
void
vulkan_sender_unlock(
	void)
{
	/* The caller took it with vulkan_sender_lock(). */
	(void)pthread_mutex_unlock(&sender_lock);

	/* Succeeded: the lock is free. */
	return;
}

/*
 * Queues a piece last for the recording thread, waking the thread when it
 * sleeps; the caller holds the lock and has filled the piece.
 */
void
vulkan_sender_queue_locked(
	struct vulkan_sender_item *item)
{
	uint64_t mark;

	/* The piece goes after every piece handed over before it. */
	item->waiting = VK_TRUE;
	item->next = NULL;
	if (sender_last != NULL)
		sender_last->next = item;
	else
		sender_first = item;
	sender_last = item;

	/* A thread sleeping for want of work is woken; a busy one finds the piece when it looks next. */
	if (sender_sleeping != VK_FALSE) {
		sender_sleeping = VK_FALSE;
		(void)pthread_cond_signal(&sender_ready);
		mark = vulkan_timing_start();
		vulkan_timing_add(VULKAN_TIMING_SENDER_WAKE, mark, 0);
	}

	/* Succeeded: the piece is the thread's to send. */
	return;
}

/*
 * Sleeps, the lock held, until the recording thread has sent a piece (the
 * caller tests what it waits for again).
 */
void
vulkan_sender_wait_locked(
	void)
{
	/* Counted, so the thread broadcasts only when someone waits. */
	sender_done_waiters++;
	(void)pthread_cond_wait(&sender_done, &sender_lock);
	sender_done_waiters--;

	/* Succeeded: a piece was sent, or the wake was spurious. */
	return;
}

/*
 * Reports whether a fence-less submission may be handed to the recording
 * thread.
 */
VkBool32
vulkan_handoff_allowed(
	void)
{
	const char *variable;
	long processors;
	VkBool32 forked;
	VkBool32 running;
	int state;

	/* The environment and the processors are read once. */
	state = __atomic_load_n(&sender_handoff_state, __ATOMIC_ACQUIRE);
	if (state == 0) {
		state = 2;
		variable = getenv(VULKAN_HANDOFF_VARIABLE);
		if (variable != NULL && strcmp(variable, "0") == 0)
			state = 1;
		processors = sysconf(_SC_NPROCESSORS_ONLN);
		if (processors < 2)
			state = 1;
		__atomic_store_n(&sender_handoff_state, state, __ATOMIC_RELEASE);
	}

	/* Turned off, or one processor only (the thread would only add its cost). */
	if (state != 2)
		return VK_FALSE;

	/* A forked child sends everything itself. */
	forked = __atomic_load_n(&sender_forked, __ATOMIC_ACQUIRE);
	if (forked != VK_FALSE)
		return VK_FALSE;

	/* Only with the thread running. */
	running = vulkan_sender_running();
	if (running == VK_FALSE)
		return VK_FALSE;

	/* Succeeded: the submission may be handed off. */
	return VK_TRUE;
}

/*
 * Makes a queue's places for handed-off submissions.
 */
VkResult
vulkan_handoff_ring_create(
	struct vulkan_handoff_ring **ring,
	const struct vulkan_allocator *allocator)
{
	struct vulkan_handoff_ring *made;
	uint32_t index;
	int error;

	/* The places, all free. */
	made = vulkan_allocate(allocator, sizeof(*made), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (made == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	memset(made, 0, sizeof(*made));

	/* The condition a submitter sleeps on while the ring is full. */
	error = pthread_cond_init(&made->room, NULL);
	if (error != 0) {
		vulkan_free(allocator, made);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Each place's transaction starts with no storage; the first submission's grows it. */
	for (index = 0U; index < VULKAN_HANDOFF_SLOTS; index++) {
		made->slots[index].ring = made;
		made->slots[index].item.send = handoff_send;
		made->slots[index].item.sent = handoff_sent;
		vulkan_writer_init(&made->slots[index].transaction);
	}

	/* Succeeded: the queue may hand submissions off. */
	*ring = made;
	return VK_SUCCESS;
}

/*
 * Releases a queue's places for handed-off submissions; the caller has
 * drained every context they were used for.
 */
void
vulkan_handoff_ring_destroy(
	struct vulkan_handoff_ring *ring,
	const struct vulkan_allocator *allocator)
{
	uint32_t index;

	/* No ring was ever made. */
	if (ring == NULL)
		return;

	/* Each place's kept storage. */
	for (index = 0U; index < VULKAN_HANDOFF_SLOTS; index++)
		vulkan_writer_finish(&ring->slots[index].transaction);

	/* The ring itself. */
	(void)pthread_cond_destroy(&ring->room);
	vulkan_free(allocator, ring);

	/* Succeeded: the queue holds no hand-off storage. */
	return;
}

/*
 * Returns the ring's next place once the recording thread has sent what
 * it held, sleeping while the ring is full until half of it is free.
 */
struct vulkan_handoff *
vulkan_handoff_take(
	struct vulkan_handoff_ring *ring)
{
	struct vulkan_handoff *slot;
	uint64_t mark;

	/* The places are used in turn, and sent in the same order. */
	slot = &ring->slots[ring->next];
	vulkan_sender_lock();

	/* A busy next place means a full ring: the thread signals room when half of it is free. */
	if (slot->item.waiting != VK_FALSE) {
		mark = vulkan_timing_start();
		while (slot->item.waiting != VK_FALSE) {
			ring->room_waiting = VK_TRUE;
			(void)pthread_cond_wait(&ring->room, &sender_lock);
		}
		vulkan_timing_add(VULKAN_TIMING_HANDOFF_ROOM, mark, 0);
	}

	vulkan_sender_unlock();

	/* Succeeded: the place is the caller's to fill. */
	return slot;
}

/*
 * Hands a filled place to the recording thread after every piece handed
 * over before it, counting it among its context's handed-off submissions.
 */
void
vulkan_handoff_publish(
	struct vulkan_handoff_ring *ring,
	struct vulkan_handoff *slot,
	struct vulkan_context *context)
{
	/* The place is queued, the ring's count and the context's grow together. */
	slot->context = context;
	vulkan_sender_lock();

	ring->busy++;
	(void)__atomic_add_fetch(&context->handoff_queued, 1U, __ATOMIC_ACQ_REL);
	vulkan_sender_queue_locked(&slot->item);

	vulkan_sender_unlock();

	/* The next submission takes the next place. */
	ring->next = (ring->next + 1U) % VULKAN_HANDOFF_SLOTS;

	/* Succeeded: the thread sends the submission. */
	return;
}

/*
 * Waits until the recording thread has sent every submission of the
 * context handed off before this call.
 */
void
vulkan_sender_drain(
	struct vulkan_context *context)
{
	uint64_t queued;
	uint64_t sent;
	uint64_t mark;
	VkBool32 forked;

	/* Nothing handed off is still unsent: the common case, two loads. */
	queued = __atomic_load_n(&context->handoff_queued, __ATOMIC_ACQUIRE);
	sent = __atomic_load_n(&context->handoff_sent, __ATOMIC_ACQUIRE);
	if (sent >= queued)
		return;

	/* The thread's own transactions are the ones being waited for. */
	if (sender_self != VK_FALSE)
		return;

	/* A forked child has no thread to send what its parent handed off. */
	forked = __atomic_load_n(&sender_forked, __ATOMIC_ACQUIRE);
	if (forked != VK_FALSE)
		return;

	/* Waiting with a context's mutex held could wait for a thread that waits for that mutex. */
	if (vulkan_context_held() != 0U)
		abort();

	/* Only up to what was handed off when the call began, so a drawing thread cannot keep the wait going. */
	mark = vulkan_timing_start();
	vulkan_sender_lock();

	sent = __atomic_load_n(&context->handoff_sent, __ATOMIC_ACQUIRE);
	while (sent < queued) {
		vulkan_sender_wait_locked();
		sent = __atomic_load_n(&context->handoff_sent, __ATOMIC_ACQUIRE);
	}

	vulkan_sender_unlock();

	vulkan_timing_add(VULKAN_TIMING_DRAIN, mark, 0);

	/* Succeeded: no earlier hand-off of the context can come after the caller's transaction. */
	return;
}

/* Starts the recording thread and the fork handlers that keep its lock usable in a child. */
static void
sender_start(
	void)
{
	pthread_attr_t attributes;
	pthread_t thread;
	int error;

	/* A child forked while the lock is held must not inherit it held. */
	error = pthread_atfork(sender_prepare, sender_parent, sender_child);
	if (error != 0)
		return;

	/* A detached thread for the life of the process. */
	error = pthread_attr_init(&attributes);
	if (error != 0)
		return;
	(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&thread, &attributes, sender_main, NULL);
	(void)pthread_attr_destroy(&attributes);
	if (error != 0)
		return;

	/* The thread takes pieces from now on. */
	__atomic_store_n(&sender_started, VK_TRUE, __ATOMIC_RELEASE);
}

/*
 * Sends the pieces handed over, one at a time in their order, for the
 * life of the process.
 */
static void *
sender_main(
	void *argument)
{
	struct vulkan_sender_item *item;
	VkResult status;
	uint64_t mark;

	(void)argument;

	/* Its own transactions must not wait for the pieces it sends. */
	sender_self = VK_TRUE;

	/* One piece after another, sleeping when there is none. */
	for (;;) {
		/* Takes the first piece waiting. */
		vulkan_sender_lock();

		/* Sleeps while no piece is handed over; a queueing caller wakes it. */
		if (sender_first == NULL) {
			mark = vulkan_timing_start();
			while (sender_first == NULL) {
				sender_sleeping = VK_TRUE;
				(void)pthread_cond_wait(&sender_ready, &sender_lock);
			}
			sender_sleeping = VK_FALSE;
			vulkan_timing_add(VULKAN_TIMING_SENDER_SLEEP, mark, 0);
		}
		item = sender_first;
		sender_first = item->next;
		if (sender_first == NULL)
			sender_last = NULL;

		vulkan_sender_unlock();

		/* The piece's transaction, without the lock. */
		status = item->send(item);

		/* Sent: its owner learns the result, its place is free again, its waiters are woken. */
		vulkan_sender_lock();

		item->sent(item, status);
		item->waiting = VK_FALSE;
		if (sender_done_waiters != 0U)
			(void)pthread_cond_broadcast(&sender_done);

		vulkan_sender_unlock();
	}

	/* Not reached: the thread lives as long as the process. */
	return NULL;
}

/* Takes the thread's lock before a fork, so the child does not inherit it held by another thread. */
static void
sender_prepare(
	void)
{
	/* The forking thread holds it across the fork. */
	vulkan_sender_lock();
}

/* Releases the thread's lock in the parent after a fork. */
static void
sender_parent(
	void)
{
	/* The parent goes on as before. */
	vulkan_sender_unlock();
}

/*
 * Releases the thread's lock in a forked child, which has no recording
 * thread: everything it sends goes synchronously, and nothing waits for
 * the pieces its parent had handed over.
 */
static void
sender_child(
	void)
{
	/* No thread, no hand-off, no waits for the parent's pieces. */
	__atomic_store_n(&sender_forked, VK_TRUE, __ATOMIC_RELEASE);
	__atomic_store_n(&sender_started, VK_FALSE, __ATOMIC_RELEASE);
	sender_first = NULL;
	sender_last = NULL;
	sender_sleeping = VK_FALSE;
	sender_done_waiters = 0U;
	vulkan_sender_unlock();
}

/*
 * Sends a handed-off submission's transaction and reads its replies: the
 * recordings' heads and Ends, then the submission's.  Any failure loses
 * the context (the application was told the submission succeeded), and
 * is said once to stderr.
 */
static VkResult
handoff_send(
	struct vulkan_sender_item *item)
{
	struct vulkan_handoff *slot;
	struct vulkan_reader reader;
	VkResult status;
	VkResult before;
	uint32_t index;
	uint32_t opcode;

	/* The transaction, with room for every reply. */
	slot = (struct vulkan_handoff *)item;
	before = __atomic_load_n(&slot->context->error, __ATOMIC_ACQUIRE);
	status = vulkan_context_execute(slot->context, &slot->transaction, (size_t)slot->replies * 8U, &reader);

	/* Each reply in order: its own opcode, then its result; the first failure stops the reading. */
	for (index = 0U; status == VK_SUCCESS && index < slot->replies; index++) {
		opcode = vulkan_read_u32(&reader);
		if (reader.error != VK_SUCCESS || opcode != slot->expected[index]) {
			status = VK_ERROR_DEVICE_LOST;
			break;
		}
		status = vulkan_read_result(&reader);
		if (reader.error != VK_SUCCESS)
			status = VK_ERROR_DEVICE_LOST;
	}
	vulkan_reader_finish(&reader);

	/* A failure after the application went on: the work did not run, and what follows needs it. */
	if (status != VK_SUCCESS) {
		if (before == VK_SUCCESS)
			fprintf(stderr, "VULKAN handoff failed result=%d device lost\n", (int)status);
		vulkan_context_error(slot->context, VK_ERROR_DEVICE_LOST);
		return status;
	}

	/* Succeeded: the submission ran. */
	return VK_SUCCESS;
}

/* Frees a sent hand-off's place, waking a submitter once half of its ring is free, and counts it sent. */
static void
handoff_sent(
	struct vulkan_sender_item *item,
	VkResult status)
{
	struct vulkan_handoff *slot;
	struct vulkan_handoff_ring *ring;

	(void)status;

	/* The ring has one place more; a sleeping submitter wakes when half are free. */
	slot = (struct vulkan_handoff *)item;
	ring = slot->ring;
	ring->busy--;
	if (ring->room_waiting != VK_FALSE && ring->busy <= VULKAN_HANDOFF_SLOTS / 2U) {
		ring->room_waiting = VK_FALSE;
		(void)pthread_cond_signal(&ring->room);
	}

	/* A drain waiting for this context may stop once the count reaches what it saw. */
	(void)__atomic_add_fetch(&slot->context->handoff_sent, 1U, __ATOMIC_ACQ_REL);

	/* Succeeded: the place may be taken again. */
	return;
}
