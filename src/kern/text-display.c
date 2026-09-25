/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's character-output face of the display device.
 *
 * This holds one table and forwards to it. The table is published once
 * during the display driver's bring-up and read on every console write,
 * so it is stored and loaded atomically rather than under a lock: taking
 * a lock here would put one on the path of every character.
 */

#include <uapi/errno.h>
#include <stddef.h>

#include "kern/text-display.h"
#include <kern/lock.h>
#include <kern/waitq.h>

/* The published backend is immutable and outlives every atomic reader. */
static const struct kern_text_ops *text_ops;

/*
 * This unsigned counter changes after completed text mutations and backend replacement.
 * Equality is a redraw hint between observations, not a globally unique lifetime token.
 */
static uint32_t text_generation;

/* Notification links retire under this lock before their caller-owned storage disappears. */
static struct spinlock text_observer_lock = {
	{ 0U }, LOCK_RANK_CONSOLE_TEXT, "text observers", 0U, 0U
};

/* Only active snapshot consumers subscribe, so graphics ownership need not wake them. */
static struct kern_text_observer *text_observers;

static const struct kern_text_ops *ops(void);
static void text_changed(void);

/*
 * Publishes one board's character output.
 */
void
kern_text_register(
	const struct kern_text_ops *ops)
{
	/* Releases the completed table to every reader. */
	__atomic_store_n(&text_ops, ops, __ATOMIC_RELEASE);

	/* Snapshot consumers must repaint even when the replacement backend kept the same text. */
	text_changed();

	/* Succeeded: readers can observe the new immutable backend and its redraw generation. */
	return;
}

/*
 * Reports whether a board has published character output.
 */
int
kern_text_ready(
	void)
{
	/* Reports the published table's presence. */
	return __atomic_load_n(&text_ops, __ATOMIC_ACQUIRE) != NULL;
}

/*
 * Reports the grid size in character cells.
 */
void
kern_text_get_size(
	unsigned *columns,
	unsigned *rows)
{
	const struct kern_text_ops *table;

	/* Reports an empty grid when no display is published. */
	table = ops();
	if (table == NULL) {
		if (columns != NULL)
			*columns = 0;
		if (rows != NULL)
			*rows = 0;
		return;
	}
	table->get_size(columns, rows);
}

/*
 * Writes one character at the cursor.
 */
void
kern_text_putc(
	int character)
{
	const struct kern_text_ops *table;

	/* Discards output until a display is published. */
	table = ops();
	if (table != NULL) {
		table->putc(character);

		/* Publishes the appended character and updated cursor to snapshot consumers. */
		text_changed();
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Writes a terminated string at one cell with one attribute.
 */
void
kern_text_write(
	unsigned row,
	unsigned column,
	uint8_t attribute,
	const char *utf8)
{
	const struct kern_text_ops *table;

	/* Discards output until a display is published. */
	table = ops();
	if (table != NULL) {
		table->write(row, column, attribute, utf8);

		/* Publishes the replaced text span to snapshot consumers. */
		text_changed();
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Blanks the grid and homes the cursor.
 */
void
kern_text_clear(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL) {
		table->clear();

		/* Publishes the cleared retained grid to snapshot consumers. */
		text_changed();
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Moves the cursor.
 */
int
kern_text_set_cursor(
	unsigned row,
	unsigned column)
{
	const struct kern_text_ops *table;
	int placement;

	/* Reports failure without a display. */
	table = ops();
	if (table == NULL)
		return 0;

	/* Retains the backend's cursor convention while notifying snapshot consumers. */
	placement = table->set_cursor(row, column);
	text_changed();

	/* Succeeded: reports the backend's placement answer. */
	return placement;
}

/*
 * Reports the cursor position and whether it is shown.
 */
void
kern_text_get_cursor(
	unsigned *row,
	unsigned *column,
	int *visible)
{
	const struct kern_text_ops *table;

	/* Reports a hidden home cursor without a display. */
	table = ops();
	if (table == NULL) {
		if (row != NULL)
			*row = 0;
		if (column != NULL)
			*column = 0;
		if (visible != NULL)
			*visible = 0;
		return;
	}
	table->get_cursor(row, column, visible);
}

/*
 * Shows or hides the cursor.
 */
void
kern_text_show_cursor(
	int visible)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL) {
		table->show_cursor(visible);

		/* Publishes the changed cursor visibility to snapshot consumers. */
		text_changed();
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Repaints the cursor after a stream of writes.
 */
void
kern_text_update_cursor(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL) {
		table->update_cursor();

		/* Publishes the completed cursor repaint to snapshot consumers. */
		text_changed();
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Stops output while a graphics mode owns the screen.
 */
void
kern_text_suspend(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL) {
		table->suspend();

		/* Publishes the backend's suspended rendering state to snapshot consumers. */
		text_changed();
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Restarts output and repaints the retained screen.
 */
void
kern_text_resume(
	void)
{
	const struct kern_text_ops *table;

	/* Does nothing without a display. */
	table = ops();
	if (table != NULL) {
		table->resume();

		/* Publishes the resumed retained text to snapshot consumers. */
		text_changed();
	}

	/* Succeeded: the available backend completed its text operation. */
	return;
}

/*
 * Renders the published backend's retained cells into caller-owned ordinary RAM.
 */
int
kern_text_snapshot(
	struct kern_text_snapshot *snapshot)
{
	const struct kern_text_ops *table;
	int error;

	/* An absent request cannot receive geometry or pixels. */
	if (snapshot == NULL)
		return EINVAL;

	/* Backends without retained-cell rendering keep their existing text behavior. */
	table = ops();
	if (table == NULL)
		return ENOTSUP;

	/* The optional tail callback allows legacy platform tables to remain unchanged. */
	if (table->snapshot == NULL)
		return ENOTSUP;

	/* Only the renderer reads its retained character state and owns its text lock. */
	error = table->snapshot(snapshot);
	if (error != 0)
		return error;

	/* Succeeded: the caller owns either exact geometry or a complete independent image. */
	return 0;
}

/*
 * Observes completed text mutations without entering any display or console lock.
 */
uint32_t
kern_text_generation(
	void)
{
	uint32_t generation;

	/* Acquire ordering observes retained text writes which preceded this redraw hint. */
	generation = __atomic_load_n(&text_generation, __ATOMIC_ACQUIRE);

	/* Succeeded: the caller may compare this generation with its last displayed snapshot. */
	return generation;
}

/*
 * Publishes a caller-owned wake destination without transferring its lifetime.
 */
int
kern_text_observe(
	struct kern_text_observer *observer,
	struct spinlock *lock,
	struct wait_queue *queue)
{
	struct kern_text_observer *entry;
	unsigned long irq;

	/* A notification must own a complete condition lock and queue pair. */
	if (observer == NULL ||
	    lock == NULL ||
	    queue == NULL)
		return EINVAL;

	/* Reject reverse lock ordering before an interrupt can encounter the destination. */
	if (lock->rank <= LOCK_RANK_CONSOLE_TEXT || lock->rank >= LOCK_RANK_SCHEDULER)
		return EINVAL;

	/* The registry lock serializes duplicate registration with mutation and removal. */
	irq = spin_lock_irqsave(&text_observer_lock);

	/* An already linked object cannot be repurposed while notifications reference it. */
	for (entry = text_observers; entry != NULL; entry = entry->next) {
		/* Each caller-owned link may occur in the registry at most once. */
		if (entry == observer) {
			spin_unlock_irqrestore(&text_observer_lock, irq);
			return EBUSY;
		}
	}

	/* Publish the complete destination before releasing the registry lock. */
	observer->lock = lock;
	observer->queue = queue;
	observer->next = text_observers;
	text_observers = observer;

	spin_unlock_irqrestore(&text_observer_lock, irq);

	/* Succeeded: subsequent text changes notify this destination until removal. */
	return 0;
}

/*
 * Ends a notification lifetime before its owner can retire the condition queue.
 */
void
kern_text_unobserve(
	struct kern_text_observer *observer)
{
	struct kern_text_observer **link;
	unsigned long irq;

	/* A missing subscription owns no notification lifetime. */
	if (observer == NULL)
		return;

	/* Taking the registry lock also waits for any earlier complete wake operation. */
	irq = spin_lock_irqsave(&text_observer_lock);

	/* Unlink by identity so repeated removal never dereferences retired destinations. */
	for (link = &text_observers; *link != NULL; link = &(*link)->next) {
		/* Only this exact link loses its registered wake destination. */
		if (*link == observer) {
			*link = observer->next;
			observer->next = NULL;
			observer->lock = NULL;
			observer->queue = NULL;
			break;
		}
	}

	spin_unlock_irqrestore(&text_observer_lock, irq);

	/* Succeeded: no notification can access the caller-owned link after return. */
	return;
}

/* Reads the published table, or NULL before any board registers. */
static const struct kern_text_ops *
ops(void)
{
	/* Pairs with the release in the register path. */
	return __atomic_load_n(&text_ops, __ATOMIC_ACQUIRE);
}

/* Publishes retained text before waking only the consumers currently displaying it. */
static void
text_changed(
	void)
{
	struct kern_text_observer *observer;
	unsigned long irq;

	/* Text backend locks have already been released before this notification boundary. */
	__atomic_add_fetch(&text_generation, 1U, __ATOMIC_RELEASE);
	irq = spin_lock_irqsave(&text_observer_lock);

	/* Holding the registry excludes removal until every destination wake has finished. */
	for (observer = text_observers; observer != NULL; observer = observer->next) {
		/* The subscriber lock serializes sleep registration with this IRQ-safe notification. */
		spin_lock(observer->lock);

		waitq_wake_all(observer->queue);

		spin_unlock(observer->lock);
	}

	spin_unlock_irqrestore(&text_observer_lock, irq);

	/* Succeeded: each subscribed consumer can observe this text generation. */
	return;
}
