/* Internal HAL console wait-list helpers. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_CONS_WAIT_H
#define ZEDBSD_HAL_CONS_WAIT_H

#include <hal/hal.h>

struct hal_cons_wait_entry {
	hal_task_t task;
	struct hal_cons_wait_entry *next;
	int queued;
};

struct hal_cons_wait_queue {
	volatile unsigned lock;
	struct hal_cons_wait_entry *head;
	struct hal_cons_wait_entry *tail;
};

static inline void
hal_cons_wait_queue_init(struct hal_cons_wait_queue *queue)
{
	queue->lock = 0;
	queue->head = queue->tail = NULL;
}

static inline bool
hal_cons_wait_queue_lock(struct hal_cons_wait_queue *queue)
{
	bool enabled = hal_irq_disable();

	while (__atomic_exchange_n(&queue->lock, 1U, __ATOMIC_ACQUIRE) != 0)
		hal_compiler_barrier();
	return enabled;
}

static inline void
hal_cons_wait_queue_unlock(struct hal_cons_wait_queue *queue, bool enabled)
{
	__atomic_store_n(&queue->lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

/* Caller holds the queue lock. */
static inline void
hal_cons_wait_queue_add(struct hal_cons_wait_queue *queue,
	struct hal_cons_wait_entry *entry)
{
	if (entry->queued)
		return;
	entry->next = NULL;
	entry->queued = 1;
	if (queue->tail != NULL)
		queue->tail->next = entry;
	else
		queue->head = entry;
	queue->tail = entry;
}

/* Caller holds the queue lock. */
static inline struct hal_cons_wait_entry *
hal_cons_wait_queue_detach_all(struct hal_cons_wait_queue *queue)
{
	struct hal_cons_wait_entry *entries = queue->head;
	struct hal_cons_wait_entry *entry;

	queue->head = queue->tail = NULL;
	for (entry = entries; entry != NULL; entry = entry->next)
		entry->queued = 0;
	return entries;
}

/* Call after releasing the queue lock. */
static inline void
hal_cons_wait_queue_notify_all(struct hal_cons_wait_entry *entries)
{
	while (entries != NULL) {
		struct hal_cons_wait_entry *next = entries->next;

		kernel_notify_task(entries->task);
		entries = next;
	}
}

#endif
