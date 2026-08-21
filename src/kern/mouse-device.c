/*
 * Generic relative mouse character device
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/mouse-device.h"
#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/file.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/waitq.h"

#include <zedbsd/mouse.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#define MOUSE_EVENT_COUNT 128U

static struct mouse_event events[MOUSE_EVENT_COUNT];
static unsigned event_head, event_tail, event_used, event_sequence;
static struct spinlock event_lock;
static struct wait_queue event_waitq;
static int mouse_ready;
static int (*backend_start)(void);
static void (*backend_stop)(void);
static unsigned open_count;

int
mouse_device_set_backend(int (*start)(void), void (*stop)(void))
{
	unsigned long irq;
	if (start == NULL || stop == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&event_lock);
	if (backend_start != NULL || open_count != 0) {
		spin_unlock_irqrestore(&event_lock, irq);
		return EBUSY;
	}
	backend_start = start;
	backend_stop = stop;
	spin_unlock_irqrestore(&event_lock, irq);
	return 0;
}

void
mouse_input_report(uint32_t device_id, int32_t dx, int32_t dy,
    uint32_t buttons)
{
	struct mouse_event *event;
	unsigned long irq;
	unsigned overflow = 0;
	if (!mouse_ready)
		return;
	irq = spin_lock_irqsave(&event_lock);
	if (open_count == 0) {
		spin_unlock_irqrestore(&event_lock, irq);
		return;
	}
	if (event_used == MOUSE_EVENT_COUNT) {
		event_tail = (event_tail + 1U) % MOUSE_EVENT_COUNT;
		event_used--;
		overflow = ZEDBSD_MOUSE_EVENT_OVERFLOW;
	}
	event = &events[event_head];
	memset(event, 0, sizeof(*event));
	event->timestamp_ns = clock_milliseconds(NULL) * 1000000ULL;
	event->sequence = ++event_sequence;
	event->flags = (uint16_t)overflow;
	event->device_id = device_id;
	event->dx = dx;
	event->dy = dy;
	event->buttons = buttons;
	event_head = (event_head + 1U) % MOUSE_EVENT_COUNT;
	event_used++;
	waitq_wake_all(&event_waitq);
	spin_unlock_irqrestore(&event_lock, irq);
	poll_notify();
}

static int
mouse_open(struct file *file)
{
	unsigned long irq;
	int activate, error;
	if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		return EACCES;
	irq = spin_lock_irqsave(&event_lock);
	if (backend_start == NULL) {
		spin_unlock_irqrestore(&event_lock, irq);
		return ENODEV;
	}
	activate = open_count++ == 0;
	spin_unlock_irqrestore(&event_lock, irq);
	if (!activate)
		return 0;
	error = backend_start();
	if (error == 0)
		return 0;
	irq = spin_lock_irqsave(&event_lock);
	open_count--;
	spin_unlock_irqrestore(&event_lock, irq);
	return error;
}

static int
mouse_close(struct file *file)
{
	unsigned long irq;
	int deactivate = 0;
	(void)file;
	irq = spin_lock_irqsave(&event_lock);
	if (open_count != 0 && --open_count == 0) {
		event_head = event_tail = event_used = 0;
		deactivate = 1;
	}
	spin_unlock_irqrestore(&event_lock, irq);
	if (deactivate)
		backend_stop();
	return 0;
}

static ssize_t
mouse_read(struct file *file, void *buffer, size_t size)
{
	size_t capacity, count = 0;
	unsigned long irq;
	if (size < sizeof(struct mouse_event))
		return -EINVAL;
	capacity = size / sizeof(struct mouse_event);
	irq = spin_lock_irqsave(&event_lock);
	while (event_used == 0) {
		uint64_t sequence;
		int error;
		if ((file->f_flags & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&event_lock, irq);
			return -EAGAIN;
		}
		sequence = waitq_sequence(&event_waitq);
		error = waitq_sleep(&event_waitq, &event_lock, sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&event_lock, irq);
			return -EINTR;
		}
	}
	while (count < capacity && event_used != 0) {
		((struct mouse_event *)buffer)[count++] = events[event_tail];
		event_tail = (event_tail + 1U) % MOUSE_EVENT_COUNT;
		event_used--;
	}
	spin_unlock_irqrestore(&event_lock, irq);
	return (ssize_t)(count * sizeof(struct mouse_event));
}

static int
mouse_poll(struct file *file, short requested, short *returned)
{
	unsigned long irq;
	short result = 0;
	(void)file;
	if (returned == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&event_lock);
	if (event_used != 0)
		result |= requested & (POLLIN | POLLRDNORM);
	spin_unlock_irqrestore(&event_lock, irq);
	*returned = result;
	return 0;
}

static const struct cdev_ops mouse_ops = {
	.open = mouse_open,
	.close = mouse_close,
	.read = mouse_read,
	.poll = mouse_poll,
};

int
mouse_device_register(void)
{
	spin_init(&event_lock, LOCK_RANK_DEVICE, "mouse input");
	waitq_init(&event_waitq, "mouse input");
	event_head = event_tail = event_used = event_sequence = 0;
	backend_start = NULL;
	backend_stop = NULL;
	open_count = 0;
	mouse_ready = 1;
	return cdev_register("mouse", 0x00010002U, &mouse_ops, NULL);
}
