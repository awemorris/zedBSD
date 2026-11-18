/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The raw HID devices' class (drivers/generic/hidraw.h, ws161-p002).
 *
 * Each registered device gets the lowest free number N and the character
 * device hidrawN (devfs: /dev/input/hidrawN, mode 0600; sessiond gives it
 * to the seat's user).  An open gets a reader with a ring of HIDRAW_QUEUE
 * input reports; drv_hidraw_input() copies a report into every reader's
 * ring under the device's spinlock and wakes the readers.  A write sends
 * one output report through the transport under the output mutex, which
 * also keeps the outputs in order; the withdrawal takes the transport away
 * under the same mutex, so no output runs after it returns.  The record
 * goes with the device's last reference.
 */

#include <drivers/generic/hidraw.h>

#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"
#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>
#include <uapi/poll.h>
#include <uapi/system.h>

/* The device numbers of the class: one major, the minor is N. */
#define HIDRAW_DEVICE_BASE	0x000f0000U

/*
 * One open of a raw device: the ring of the input reports not yet read
 * (each slot holds a length and up to slot_size bytes), where the oldest
 * is and how many there are.  Guarded by the device's spinlock.
 */
struct hidraw_reader {
	struct hidraw_reader *next;
	uint8_t *slots;
	size_t *lengths;
	size_t slot_size;
	unsigned head;
	unsigned count;
};

/* One published raw device. */
struct drv_hidraw {
	/*
	 * Guards the readers, their rings and registered.  The waiters on
	 * waitq sleep under it: readers for a report, and nobody else.
	 */
	struct spinlock lock;
	struct wait_queue waitq;
	struct hidraw_reader *readers;
	unsigned registered;
	/* The open that holds the device alone (HIDRAW_GRAB), or NULL. */
	struct hidraw_reader *grabber;

	/*
	 * Serializes the outputs with each other and with the withdrawal,
	 * which clears ops: an output that finds ops NULL answers ENODEV.
	 */
	struct mutex output_lock;
	const struct drv_hidraw_ops *ops;
	void *context;

	/* What the requests give out; fixed at registration. */
	struct hidraw_info info;
	char name[HIDRAW_TEXT_MAX];
	char physical_path[HIDRAW_TEXT_MAX];
	uint8_t *descriptor;
	size_t descriptor_size;

	/* The slot the device holds and the device's reference the registration owns. */
	unsigned number;
	struct cdev *node;
};

/*
 * The system's events, posted when a device comes and goes.  Weak, so a
 * kernel without the events links; a null test there posts nothing.
 */
extern void kern_system_event_post(uint32_t, uint32_t, int32_t, const char *, const char *) __attribute__((weak));

static int hidraw_open(struct file *file);
static int hidraw_close(struct file *file);
static ssize_t hidraw_read(struct file *file, void *buffer, size_t size);
static ssize_t hidraw_write(struct file *file, const void *buffer, size_t size);
static int hidraw_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int hidraw_poll(struct file *file, short requested, short *returned);
static struct drv_hidraw *hidraw_file_device(struct file *file);
static int hidraw_slot_claim(struct drv_hidraw *hidraw);
static void hidraw_slot_release(struct drv_hidraw *hidraw);
static void hidraw_finalize(void *data);
static void hidraw_post(const struct drv_hidraw *hidraw, uint32_t action);
static void hidraw_copy_text(char *target, const char *source);
static int hidraw_grab(struct drv_hidraw *hidraw, struct hidraw_reader *reader, int grab);

/* The operations of every raw device. */
static const struct cdev_ops hidraw_cdev_ops = {
	.open = hidraw_open,
	.close = hidraw_close,
	.read = hidraw_read,
	.write = hidraw_write,
	.ioctl = hidraw_ioctl,
	.poll = hidraw_poll
};

/* The device holding each number, NULL for a free one; guarded by hidraw_registry_lock. */
static struct drv_hidraw *hidraw_slots[DRV_HIDRAW_MAX];

/* Serializes the claiming and the release of the numbers for the kernel's life. */
static struct spinlock hidraw_registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "hidraw registry", 0, 0
};

/*
 * Registers a raw device and publishes it.
 *
 * Returns 0 with the device's record in *result, EINVAL for a missing or
 * oversized part, ENOSPC when every number is taken, ENOMEM, or the error
 * of publishing the device.
 */
int
drv_hidraw_register(
	const struct drv_hidraw_description *description,
	const struct drv_hidraw_ops *ops,
	void *context,
	struct drv_hidraw **result)
{
	struct drv_hidraw *hidraw;
	struct cdev *node;
	char node_name[32];
	int error;

	/* A transport describes its device, sends its output, and its reports fit. */
	if (description == NULL || ops == NULL || ops->output == NULL || result == NULL)
		return EINVAL;
	if (description->descriptor_size > HIDRAW_DESCRIPTOR_MAX)
		return EINVAL;
	if (description->info.input_size == 0U || description->info.input_size > HIDRAW_REPORT_MAX)
		return EINVAL;
	if (description->info.output_size > HIDRAW_REPORT_MAX)
		return EINVAL;

	/* The device's record. */
	hidraw = kern_calloc(1U, sizeof(*hidraw));
	if (hidraw == NULL)
		return ENOMEM;

	/* The report descriptor's copy. */
	if (description->descriptor_size != 0U) {
		hidraw->descriptor = kern_malloc(description->descriptor_size);
		if (hidraw->descriptor == NULL) {
			kern_free(hidraw);
			return ENOMEM;
		}

		/* The bytes. */
		kern_memcpy(hidraw->descriptor, description->descriptor, description->descriptor_size);
		hidraw->descriptor_size = description->descriptor_size;
	}

	/* The output's lock. */
	error = mutex_init(&hidraw->output_lock, LOCK_RANK_DEVICE, "hidraw output");
	if (error != 0) {
		kern_free(hidraw->descriptor);
		kern_free(hidraw);
		return error;
	}

	/* The transport, what the device is, and its readers' lock. */
	spin_init(&hidraw->lock, LOCK_RANK_DEVICE, "hidraw");
	waitq_init(&hidraw->waitq, "hidraw report");
	hidraw->ops = ops;
	hidraw->context = context;
	hidraw->info = description->info;
	hidraw_copy_text(hidraw->name, description->name);
	hidraw_copy_text(hidraw->physical_path, description->physical_path);
	hidraw->registered = 1U;

	/* The lowest free number. */
	error = hidraw_slot_claim(hidraw);
	if (error != 0) {
		kern_free(hidraw->descriptor);
		kern_free(hidraw);
		return error;
	}

	/* Publishes hidrawN; the record lives as long as the device does. */
	kern_snprintf(node_name, sizeof(node_name), "hidraw%u", hidraw->number);
	error = cdev_register_managed(node_name,
	    (dev_t)(HIDRAW_DEVICE_BASE + hidraw->number),
	    &hidraw_cdev_ops,
	    hidraw,
	    hidraw_finalize,
	    &node);
	if (error != 0) {
		hidraw_slot_release(hidraw);
		kern_free(hidraw->descriptor);
		kern_free(hidraw);
		return error;
	}

	/* The registration's reference, given back at the withdrawal. */
	hidraw->node = node;
	kern_logf("hidraw: /dev/input/%s: %s usage=%04x:%04x\n", node_name, hidraw->name,
		  (unsigned)hidraw->info.usage_page, (unsigned)hidraw->info.usage);

	/* The desktop hears the new device. */
	hidraw_post(hidraw, KERN_SYSTEM_EVENT_ADD);

	/* Succeeded: the device is published and the record is the transport's handle. */
	*result = hidraw;
	return 0;
}

/*
 * Hands one input report to every open of the device: the oldest report
 * of a full ring goes to make room.  Any context but an interrupt handler
 * (the readers are woken).
 */
void
drv_hidraw_input(
	struct drv_hidraw *hidraw,
	const uint8_t *report,
	size_t length)
{
	struct hidraw_reader *reader;
	unsigned long irq;
	unsigned slot;
	size_t kept;

	/* Nothing to hand on. */
	if (hidraw == NULL || report == NULL || length == 0U)
		return;

	/* Copies the report into every reader's ring. */
	irq = spin_lock_irqsave(&hidraw->lock);

	for (reader = hidraw->readers; reader != NULL; reader = reader->next) {
		/* While one open holds the device, only it hears the reports. */
		if (hidraw->grabber != NULL && reader != hidraw->grabber)
			continue;

		/* A full ring gives up its oldest report. */
		if (reader->count == HIDRAW_QUEUE) {
			reader->head = (reader->head + 1U) % HIDRAW_QUEUE;
			reader->count--;
		}

		/* The report, as much of it as a slot holds. */
		slot = (reader->head + reader->count) % HIDRAW_QUEUE;
		kept = length;
		if (kept > reader->slot_size)
			kept = reader->slot_size;
		kern_memcpy(reader->slots + (size_t)slot * reader->slot_size, report, kept);
		reader->lengths[slot] = kept;
		reader->count++;
	}

	spin_unlock_irqrestore(&hidraw->lock, irq);

	/* The readers waiting, and the pollers. */
	waitq_wake_all(&hidraw->waitq);
	poll_notify();
}

/*
 * Withdraws a raw device: the readers waiting answer ENODEV, no output
 * runs after this returns, and the node goes.
 */
void
drv_hidraw_unregister(
	struct drv_hidraw *hidraw)
{
	struct cdev *node;
	unsigned long irq;

	/* Nothing was registered. */
	if (hidraw == NULL)
		return;

	/* registered tells the readers and the pollers that the device is gone. */
	irq = spin_lock_irqsave(&hidraw->lock);

	hidraw->registered = 0U;

	spin_unlock_irqrestore(&hidraw->lock, irq);
	waitq_wake_all(&hidraw->waitq);
	poll_notify();

	/* Takes the transport away after the output running now, if any. */
	mutex_lock(&hidraw->output_lock);

	hidraw->ops = NULL;
	hidraw->context = NULL;

	mutex_unlock(&hidraw->output_lock);

	/* The desktop hears the device go, and the number may be given again. */
	hidraw_post(hidraw, KERN_SYSTEM_EVENT_REMOVE);
	hidraw_slot_release(hidraw);

	/*
	 * Unpublishes the device and gives back the registration's reference;
	 * the record may be freed by then (the device's last reference), so it
	 * is not touched after this.
	 */
	node = hidraw->node;
	(void)cdev_unregister(node);
	cdev_release(node);
}

/* Opens a raw device: a reader with an empty ring of reports as large as the device's. */
static int
hidraw_open(
	struct file *file)
{
	struct drv_hidraw *hidraw;
	struct hidraw_reader *reader;
	unsigned long irq;
	int attached;

	/* The device the node was opened on. */
	hidraw = hidraw_file_device(file);
	if (hidraw == NULL)
		return ENODEV;

	/* The reader. */
	reader = kern_calloc(1U, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;

	/* Its ring: a slot for each report, as large as the largest input report and its ID. */
	reader->slot_size = (size_t)hidraw->info.input_size + 1U;
	reader->slots = kern_calloc(HIDRAW_QUEUE, reader->slot_size);
	if (reader->slots == NULL) {
		kern_free(reader);
		return ENOMEM;
	}

	/* The lengths of the reports in the slots. */
	reader->lengths = kern_calloc(HIDRAW_QUEUE, sizeof(reader->lengths[0]));
	if (reader->lengths == NULL) {
		kern_free(reader->slots);
		kern_free(reader);
		return ENOMEM;
	}

	/* Joins the device's readers while it is there. */
	attached = 0;
	irq = spin_lock_irqsave(&hidraw->lock);

	if (hidraw->registered) {
		reader->next = hidraw->readers;
		hidraw->readers = reader;
		file->f_data = reader;
		attached = 1;
	}

	spin_unlock_irqrestore(&hidraw->lock, irq);

	/* A device withdrawn meanwhile. */
	if (!attached) {
		kern_free(reader->lengths);
		kern_free(reader->slots);
		kern_free(reader);
		return ENODEV;
	}

	/* Succeeded: the open sees every report from now on. */
	return 0;
}

/* Closes an open: its reader leaves the device and its ring goes. */
static int
hidraw_close(
	struct file *file)
{
	struct drv_hidraw *hidraw;
	struct hidraw_reader *reader;
	struct hidraw_reader **link;
	unsigned long irq;

	/* The device and the open's reader. */
	hidraw = hidraw_file_device(file);
	reader = file->f_data;
	if (hidraw == NULL || reader == NULL)
		return 0;

	/* Takes the reader off the device's list, and the grab with it. */
	irq = spin_lock_irqsave(&hidraw->lock);

	if (hidraw->grabber == reader)
		hidraw->grabber = NULL;
	for (link = &hidraw->readers; *link != NULL; link = &(*link)->next) {
		if (*link == reader) {
			*link = reader->next;
			break;
		}
	}

	spin_unlock_irqrestore(&hidraw->lock, irq);

	/* The ring and the reader go. */
	file->f_data = NULL;
	kern_free(reader->lengths);
	kern_free(reader->slots);
	kern_free(reader);

	/* Succeeded: the open is closed. */
	return 0;
}

/* Reads the oldest input report of the open, waiting for one unless the file does not wait. */
static ssize_t
hidraw_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct drv_hidraw *hidraw;
	struct hidraw_reader *reader;
	unsigned long irq;
	uint64_t sequence;
	size_t length;
	unsigned slot;
	int flags;
	int error;

	/* The device and the open's reader. */
	hidraw = hidraw_file_device(file);
	reader = file->f_data;
	if (hidraw == NULL || reader == NULL)
		return -ENODEV;
	if (size == 0U)
		return 0;

	/* Waits until a report is there, the device goes, or a signal comes. */
	irq = spin_lock_irqsave(&hidraw->lock);

	for (;;) {
		/* The oldest report, as much of it as the buffer holds. */
		if (reader->count != 0U) {
			slot = reader->head;
			length = reader->lengths[slot];
			if (length > size)
				length = size;
			kern_memcpy(buffer, reader->slots + (size_t)slot * reader->slot_size, length);
			reader->head = (reader->head + 1U) % HIDRAW_QUEUE;
			reader->count--;
			spin_unlock_irqrestore(&hidraw->lock, irq);
			return (ssize_t)length;
		}

		/* A device that is gone has no more reports. */
		if (!hidraw->registered) {
			spin_unlock_irqrestore(&hidraw->lock, irq);
			return -ENODEV;
		}

		/* A file that does not wait. */
		flags = file_status_flags_get(file);
		if ((flags & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&hidraw->lock, irq);
			return -EAGAIN;
		}

		/* Sleeps for the next report. */
		sequence = waitq_sequence(&hidraw->waitq);
		error = waitq_sleep(&hidraw->waitq, &hidraw->lock, sequence, 0, WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&hidraw->lock, irq);
			return -EINTR;
		}
	}
}

/* Sends one output report (its ID's byte first) through the transport. */
static ssize_t
hidraw_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct drv_hidraw *hidraw;
	unsigned long irq;
	int held;
	int error;

	/* The device, and a report that fits: the ID's byte and at most the largest output report. */
	hidraw = hidraw_file_device(file);
	if (hidraw == NULL || file->f_data == NULL)
		return -ENODEV;
	if (size < 2U || size > (size_t)hidraw->info.output_size + 1U)
		return -EINVAL;

	/* Another open holds the device. */
	irq = spin_lock_irqsave(&hidraw->lock);

	held = hidraw->grabber != NULL && hidraw->grabber != file->f_data;

	spin_unlock_irqrestore(&hidraw->lock, irq);
	if (held)
		return -EBUSY;

	/* The transport sends it, one output at a time. */
	error = mutex_lock_interruptible(&hidraw->output_lock);
	if (error != 0)
		return -EINTR;

	/* A transport that was taken away answers ENODEV. */
	error = ENODEV;
	if (hidraw->ops != NULL)
		error = hidraw->ops->output(hidraw->context, buffer, size);

	mutex_unlock(&hidraw->output_lock);

	/* Reports why the device did not take it. */
	if (error != 0)
		return -error;

	/* Succeeded: the whole report was taken. */
	return (ssize_t)size;
}

/* Answers the requests: the information, the report descriptor, the name and the place. */
static int
hidraw_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct hidraw_descriptor *descriptor;
	struct hidraw_text text;
	struct drv_hidraw *hidraw;
	int grab;
	int error;

	/* The device the node was opened on. */
	hidraw = hidraw_file_device(file);
	if (hidraw == NULL)
		return ENODEV;

	/* Each request. */
	switch (request) {
	case HIDRAW_GET_INFO:
		/* What the device is. */
		error = copyout(&hidraw->info, argument, sizeof(hidraw->info));
		break;
	case HIDRAW_GET_DESCRIPTOR:
		/* The report descriptor, built in a buffer of its own (it is larger than a stack frame should hold). */
		descriptor = kern_calloc(1U, sizeof(*descriptor));
		if (descriptor == NULL)
			return ENOMEM;
		descriptor->size = (uint32_t)hidraw->descriptor_size;
		if (hidraw->descriptor_size != 0U)
			kern_memcpy(descriptor->value, hidraw->descriptor, hidraw->descriptor_size);
		error = copyout(descriptor, argument, sizeof(*descriptor));
		kern_free(descriptor);
		break;
	case HIDRAW_GET_NAME:
		/* The product's name. */
		kern_memset(&text, 0, sizeof(text));
		kern_memcpy(text.value, hidraw->name, sizeof(text.value));
		error = copyout(&text, argument, sizeof(text));
		break;
	case HIDRAW_GET_PHYS:
		/* Where the device is. */
		kern_memset(&text, 0, sizeof(text));
		kern_memcpy(text.value, hidraw->physical_path, sizeof(text.value));
		error = copyout(&text, argument, sizeof(text));
		break;
	case HIDRAW_GRAB:
		/* Takes the device for this open, or gives it back. */
		error = copyin(argument, &grab, sizeof(grab));
		if (error == 0)
			error = hidraw_grab(hidraw, file->f_data, grab);
		break;
	default:
		/* Anything else is not a raw device's request. */
		return ENOTTY;
	}

	/* Reports a buffer the caller gave that could not be filled. */
	if (error != 0)
		return error;

	/* Succeeded: the caller has the answer. */
	return 0;
}

/* Says what an open can do now: read a report, write, or nothing more (the device is gone). */
static int
hidraw_poll(
	struct file *file,
	short requested,
	short *returned)
{
	struct drv_hidraw *hidraw;
	struct hidraw_reader *reader;
	unsigned long irq;
	short result;

	/* The device and the open's reader. */
	if (returned == NULL)
		return EINVAL;
	hidraw = hidraw_file_device(file);
	reader = file->f_data;
	if (hidraw == NULL || reader == NULL) {
		*returned = POLLERR | POLLHUP;
		return 0;
	}

	/* A report waiting, a device that takes output, or one that is gone. */
	result = 0;
	irq = spin_lock_irqsave(&hidraw->lock);

	if (reader->count != 0U)
		result |= requested & (POLLIN | POLLRDNORM);
	if (hidraw->registered)
		result |= requested & (POLLOUT | POLLWRNORM);
	else
		result |= POLLHUP;

	spin_unlock_irqrestore(&hidraw->lock, irq);

	/* Succeeded: the open's state. */
	*returned = result;
	return 0;
}

/* Finds the device an open file is on. */
static struct drv_hidraw *
hidraw_file_device(
	struct file *file)
{
	const struct cdev *node;

	/* A file of a device node carries the device's generation. */
	if (file == NULL || file->f_inode == NULL || file->f_inode->i_data == NULL)
		return NULL;
	node = file->f_inode->i_data;

	/* The record the generation was published with. */
	return node->data;
}

/* Gives a device the lowest free number; returns 0 or ENOSPC. */
static int
hidraw_slot_claim(
	struct drv_hidraw *hidraw)
{
	unsigned long irq;
	unsigned number;
	int error;

	/* The first free slot. */
	error = ENOSPC;
	irq = spin_lock_irqsave(&hidraw_registry_lock);

	for (number = 0U; number < DRV_HIDRAW_MAX; number++) {
		if (hidraw_slots[number] == NULL) {
			hidraw_slots[number] = hidraw;
			hidraw->number = number;
			error = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&hidraw_registry_lock, irq);

	/* Reports whether a number was free. */
	return error;
}

/* Gives a device's number back. */
static void
hidraw_slot_release(
	struct drv_hidraw *hidraw)
{
	unsigned long irq;

	/* The slot is free when it still holds this device. */
	irq = spin_lock_irqsave(&hidraw_registry_lock);

	if (hidraw->number < DRV_HIDRAW_MAX && hidraw_slots[hidraw->number] == hidraw)
		hidraw_slots[hidraw->number] = NULL;

	spin_unlock_irqrestore(&hidraw_registry_lock, irq);
}

/* Frees a device's record with the last reference of its node. */
static void
hidraw_finalize(
	void *data)
{
	struct drv_hidraw *hidraw;

	/* The descriptor's copy and the record. */
	hidraw = data;
	kern_free(hidraw->descriptor);
	kern_free(hidraw);
}

/* Tells the system's events that a raw device came or went (the class of the input devices). */
static void
hidraw_post(
	const struct drv_hidraw *hidraw,
	uint32_t action)
{
	char subject[KERN_SYSTEM_EVENT_SUBJECT_MAX];
	char detail[KERN_SYSTEM_EVENT_DETAIL_MAX];

	/* A kernel without the system's events posts nothing. */
	if (kern_system_event_post == NULL)
		return;

	/* The node, the detail, then the event. */
	kern_snprintf(subject, sizeof(subject), "hidraw%u", hidraw->number);
	kern_snprintf(detail, sizeof(detail), "bus=%u usage=%04x:%04x name=%s",
		      (unsigned)hidraw->info.bus, (unsigned)hidraw->info.usage_page,
		      (unsigned)hidraw->info.usage, hidraw->name);
	kern_system_event_post(KERN_SYSTEM_EVENT_INPUT, action, 0, subject, detail);
}

/* Copies a text into a field of HIDRAW_TEXT_MAX bytes, cut and ended by a NUL. */
static void
hidraw_copy_text(
	char *target,
	const char *source)
{
	size_t length;

	/* Nothing is an empty text. */
	target[0] = '\0';
	if (source == NULL)
		return;

	/* As much as fits. */
	length = kern_strlen(source);
	if (length >= HIDRAW_TEXT_MAX)
		length = HIDRAW_TEXT_MAX - 1U;
	kern_memcpy(target, source, length);
	target[length] = '\0';
}

/*
 * Takes the device for one open (grab nonzero) or gives it back (0).
 * Taking it also drops the reports the other opens have not read yet, so
 * that from the grab on they read nothing of the device's.  Returns 0, or
 * EBUSY when another open holds it.
 */
static int
hidraw_grab(
	struct drv_hidraw *hidraw,
	struct hidraw_reader *reader,
	int grab)
{
	struct hidraw_reader *other;
	unsigned long irq;
	int error;

	/* The grab changes only for its holder, or a free device; a new grab empties the others' rings. */
	error = 0;
	irq = spin_lock_irqsave(&hidraw->lock);

	if (hidraw->grabber != NULL && hidraw->grabber != reader) {
		error = EBUSY;
	} else if (grab) {
		hidraw->grabber = reader;
		for (other = hidraw->readers; other != NULL; other = other->next) {
			if (other == reader)
				continue;
			other->head = 0U;
			other->count = 0U;
		}
	} else {
		hidraw->grabber = NULL;
	}

	spin_unlock_irqrestore(&hidraw->lock, irq);

	/* Reports a device another open holds. */
	if (error != 0)
		return error;

	/* Succeeded: the device is this open's alone, or free. */
	return 0;
}
