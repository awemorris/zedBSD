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

static struct zedbsd_mouse_event events[MOUSE_EVENT_COUNT];
static unsigned event_head, event_tail, event_used, event_sequence;
static struct spinlock event_lock;
static struct wait_queue event_waitq;
static int mouse_ready;

void
mouse_input_report(uint32_t device_id, int32_t dx, int32_t dy,
    uint32_t buttons)
{
	struct zedbsd_mouse_event *event;
	unsigned long irq;
	unsigned overflow = 0;
	if (!mouse_ready)
		return;
	irq = spin_lock_irqsave(&event_lock);
	if (event_used == MOUSE_EVENT_COUNT) {
		event_tail = (event_tail + 1U) % MOUSE_EVENT_COUNT;
		event_used--;
		overflow = ZEDBSD_MOUSE_EVENT_OVERFLOW;
	}
	event = &events[event_head];
	memset(event, 0, sizeof(*event));
	event->timestamp_ns = zedbsd_kernel_milliseconds(NULL) * 1000000ULL;
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
	return (file->f_flags & O_ACCMODE) == O_WRONLY ? EACCES : 0;
}

static ssize_t
mouse_read(struct file *file, void *buffer, size_t size)
{
	size_t capacity, count = 0;
	unsigned long irq;
	if (size < sizeof(struct zedbsd_mouse_event))
		return -EINVAL;
	capacity = size / sizeof(struct zedbsd_mouse_event);
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
		((struct zedbsd_mouse_event *)buffer)[count++] = events[event_tail];
		event_tail = (event_tail + 1U) % MOUSE_EVENT_COUNT;
		event_used--;
	}
	spin_unlock_irqrestore(&event_lock, irq);
	return (ssize_t)(count * sizeof(struct zedbsd_mouse_event));
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
	.read = mouse_read,
	.poll = mouse_poll,
};

int
mouse_device_register(void)
{
	spin_init(&event_lock, LOCK_RANK_DEVICE, "mouse input");
	waitq_init(&event_waitq, "mouse input");
	event_head = event_tail = event_used = event_sequence = 0;
	mouse_ready = 1;
	return cdev_register("mouse", 0x00010002U, &mouse_ops, NULL);
}
