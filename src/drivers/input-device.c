/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/input-device.h"
#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/file.h"
#include "kern/input-capability.h"
#include "kern/input-queue.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <stdio.h>
#include <string.h>

#define INPUT_DEVICE_MAX 8U
#define INPUT_TEXT_MAX 64U

struct input_reader {
	struct input_queue_reader cursor;
	struct input_reader *next;
};

struct input_device {
	struct spinlock lock;
	struct wait_queue waitq;
	struct input_queue queue;
	struct input_reader *readers;
	struct input_reader *grabber;
	struct input_id id;
	char name[INPUT_TEXT_MAX];
	char physical_path[INPUT_TEXT_MAX];
	char unique_id[INPUT_TEXT_MAX];
	struct input_capability_state capability_state;
	int (*open)(void *);
	void (*close)(void *);
	void *context;
	unsigned number;
	int registered;
};

static struct spinlock registry_lock;
static unsigned input_device_count;

static struct input_device *
file_device(struct file *file)
{
	return file != NULL && file->f_inode != NULL &&
		       file->f_inode->i_data != NULL
		   ? ((const struct cdev *)file->f_inode->i_data)->data
		   : NULL;
}

static struct input_reader *
file_reader(struct file *file)
{
	return file != NULL ? file->f_data : NULL;
}

static int
input_open(struct file *file)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader;
	unsigned long irq;
	if (device == NULL || !device->registered)
		return ENODEV;
	if ((file_status_flags_get(file) & O_ACCMODE) == O_WRONLY)
		return EACCES;
	reader = kern_calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	if (device->open != NULL) {
		int error = device->open(device->context);
		if (error != 0) {
			kern_free(reader);
			return error;
		}
	}
	irq = spin_lock_irqsave(&device->lock);
	if (!device->registered) {
		spin_unlock_irqrestore(&device->lock, irq);
		if (device->close != NULL)
			device->close(device->context);
		kern_free(reader);
		return ENODEV;
	}
	input_queue_reader_init(&device->queue, &reader->cursor);
	reader->next = device->readers;
	device->readers = reader;
	file->f_data = reader;
	spin_unlock_irqrestore(&device->lock, irq);
	return 0;
}

static int
input_close(struct file *file)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file), **link;
	unsigned long irq;
	if (device == NULL || reader == NULL)
		return 0;
	irq = spin_lock_irqsave(&device->lock);
	for (link = &device->readers; *link != NULL; link = &(*link)->next)
		if (*link == reader) {
			*link = reader->next;
			break;
		}
	if (device->grabber == reader)
		device->grabber = NULL;
	file->f_data = NULL;
	spin_unlock_irqrestore(&device->lock, irq);
	kern_free(reader);
	if (device->close != NULL)
		device->close(device->context);
	return 0;
}

static ssize_t
input_read(struct file *file, void *buffer, size_t size)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file);
	size_t capacity, count;
	unsigned long irq;
	if (device == NULL || reader == NULL)
		return -ENODEV;
	if (size < sizeof(struct input_event))
		return -EINVAL;
	capacity = size / sizeof(struct input_event);
	irq = spin_lock_irqsave(&device->lock);
	for (;;) {
		if (device->grabber == NULL || device->grabber == reader) {
			count = input_queue_read(
			    &device->queue, &reader->cursor, buffer, capacity);
			if (count != 0) {
				spin_unlock_irqrestore(&device->lock, irq);
				return (ssize_t)(count *
						 sizeof(struct input_event));
			}
		}
		if (!device->registered) {
			spin_unlock_irqrestore(&device->lock, irq);
			return 0;
		}
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&device->lock, irq);
			return -EAGAIN;
		}
		{
			uint64_t sequence = waitq_sequence(&device->waitq);
			int error =
			    waitq_sleep(&device->waitq, &device->lock, sequence,
					0, WAITQ_INTERRUPTIBLE);
			if (error == EINTR) {
				spin_unlock_irqrestore(&device->lock, irq);
				return -EINTR;
			}
		}
	}
}

static int
input_poll(struct file *file, short requested, short *returned)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file);
	unsigned long irq;
	short result = 0;
	if (returned == NULL)
		return EINVAL;
	if (device == NULL || reader == NULL) {
		*returned = POLLERR | POLLHUP;
		return 0;
	}
	irq = spin_lock_irqsave(&device->lock);
	if ((device->grabber == NULL || device->grabber == reader) &&
	    input_queue_readable(&device->queue, &reader->cursor))
		result |= requested & (POLLIN | POLLRDNORM);
	if (!device->registered)
		result |= POLLHUP;
	spin_unlock_irqrestore(&device->lock, irq);
	*returned = result;
	return 0;
}

static int
copy_text(const char *text, unsigned long request, uintptr_t argument)
{
	size_t capacity = (request >> 16) & 0x1fffU;
	size_t length = strlen(text) + 1U;
	if (capacity == 0)
		return EINVAL;
	if (length > capacity)
		length = capacity;
	return copyout(text, argument, length);
}

static size_t
ioctl_size(unsigned long request)
{
	return (request >> 16) & 0x1fffU;
}

static int
copy_bits(const uint8_t *bits, size_t bit_size, size_t capacity,
	  uintptr_t argument)
{
	uint8_t output[32];
	size_t copied = 0, count;
	uintptr_t address;
	int error;
	while (copied < capacity) {
		count = capacity - copied;
		if (count > sizeof(output))
			count = sizeof(output);
		error = input_capability_copy(bits, bit_size, copied, output,
					      count);
		if (error == 0)
			error = user_address_add(argument, copied, &address);
		if (error == 0)
			error = copyout(output, address, count);
		if (error != 0)
			return error;
		copied += count;
	}
	return 0;
}

static int
copy_capability_bits(const struct input_device *device, unsigned type,
		     size_t capacity, uintptr_t argument)
{
	const uint8_t *bits;
	size_t size;
	int error = input_capability_bits(&device->capability_state, type,
					  &bits, &size);
	if (error != 0)
		return ENOTTY;
	return copy_bits(bits, size, capacity, argument);
}

static int
copy_key_state(struct input_device *device, size_t capacity,
	       uintptr_t argument)
{
	uint8_t snapshot[INPUT_KEY_BITS_SIZE];
	const uint8_t *bits;
	size_t size;
	unsigned long irq;
	irq = spin_lock_irqsave(&device->lock);
	(void)input_capability_key_state(&device->capability_state, &bits,
					 &size);
	memcpy(snapshot, bits, sizeof(snapshot));
	spin_unlock_irqrestore(&device->lock, irq);
	return copy_bits(snapshot, size, capacity, argument);
}

static int
copy_abs_info(struct input_device *device, unsigned axis, uintptr_t argument)
{
	struct input_absinfo info;
	unsigned long irq;
	int error;
	irq = spin_lock_irqsave(&device->lock);
	error = input_capability_abs_info(&device->capability_state, axis,
					  &info);
	spin_unlock_irqrestore(&device->lock, irq);
	if (error == ENOENT)
		return ENOTTY;
	if (error != 0)
		return error;
	return copyout(&info, argument, sizeof(info));
}

static int
input_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file), *item;
	unsigned group = (unsigned)((request >> 8) & 0xffU);
	unsigned number = (unsigned)(request & 0xffU);
	size_t size = ioctl_size(request);
	unsigned long irq;
	int value, error = 0;
	if (device == NULL || reader == NULL)
		return ENODEV;
	if (request == EVIOCGVERSION) {
		value = EV_VERSION;
		return copyout(&value, argument, sizeof(value));
	}
	if (request == EVIOCGID)
		return copyout(&device->id, argument, sizeof(device->id));
	if (group == ZEDBSD_EVDEV_IOC_GROUP) {
		switch (number) {
		case 0x06:
			if (request != EVIOCGNAME(size))
				return ENOTTY;
			return copy_text(device->name, request, argument);
		case 0x07:
			if (request != EVIOCGPHYS(size))
				return ENOTTY;
			return copy_text(device->physical_path, request,
					 argument);
		case 0x08:
			if (request != EVIOCGUNIQ(size))
				return ENOTTY;
			return copy_text(device->unique_id, request, argument);
		default:
			break;
		}
	}
	if (group == ZEDBSD_EVDEV_IOC_GROUP && number == 0x18U &&
	    request == EVIOCGKEY(size))
		return copy_key_state(device, size, argument);
	if (group == ZEDBSD_EVDEV_IOC_GROUP && number >= 0x20U &&
	    number <= 0x20U + EV_MAX &&
	    request == EVIOCGBIT(number - 0x20U, size))
		return copy_capability_bits(device, number - 0x20U, size,
					    argument);
	if (group == ZEDBSD_EVDEV_IOC_GROUP && number >= 0x40U &&
	    number <= 0x40U + ABS_MAX &&
	    request == EVIOCGABS(number - 0x40U))
		return copy_abs_info(device, number - 0x40U, argument);
	if (request != EVIOCGRAB)
		return ENOTTY;
	if ((error = copyin(argument, &value, sizeof(value))) != 0)
		return error;
	irq = spin_lock_irqsave(&device->lock);
	if (value != 0) {
		if (device->grabber != NULL && device->grabber != reader)
			error = EBUSY;
		else
			device->grabber = reader;
	} else if (device->grabber != reader) {
		error = EINVAL;
	} else {
		device->grabber = NULL;
		for (item = device->readers; item != NULL; item = item->next)
			if (item != reader)
				item->cursor.sequence =
				    device->queue.next_sequence;
		waitq_wake_all(&device->waitq);
	}
	spin_unlock_irqrestore(&device->lock, irq);
	if (error == 0)
		poll_notify();
	return error;
}

static const struct cdev_ops input_ops = {
    .open = input_open,
    .close = input_close,
    .read = input_read,
    .ioctl = input_ioctl,
    .poll = input_poll,
};

void
input_core_init(void)
{
	spin_init(&registry_lock, LOCK_RANK_DEVICE, "input registry");
	input_device_count = 0;
}

static int
copy_info_text(char *destination, const char *source)
{
	if (source == NULL)
		source = "";
	if (strlen(source) >= INPUT_TEXT_MAX)
		return ENAMETOOLONG;
	strcpy(destination, source);
	return 0;
}

int
input_device_register(const struct input_device_info *info,
		      struct input_device **result)
{
	struct input_device *device;
	char node[16];
	unsigned long irq;
	int error;
	if (info == NULL || result == NULL || info->name == NULL)
		return EINVAL;
	device = kern_calloc(1, sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	if ((error = copy_info_text(device->name, info->name)) != 0 ||
	    (error = copy_info_text(device->physical_path,
				    info->physical_path)) != 0 ||
	    (error = copy_info_text(device->unique_id, info->unique_id)) != 0) {
		kern_free(device);
		return error;
	}
	device->id = info->id;
	error = input_capability_state_init(
	    &device->capability_state, info->capabilities,
	    info->capability_count, info->absolute_axes,
	    info->absolute_axis_count);
	if (error != 0) {
		kern_free(device);
		return error;
	}
	device->open = info->open;
	device->close = info->close;
	device->context = info->context;
	spin_init(&device->lock, LOCK_RANK_DEVICE, "input device");
	waitq_init(&device->waitq, "input event");
	input_queue_init(&device->queue);
	irq = spin_lock_irqsave(&registry_lock);
	if (input_device_count == INPUT_DEVICE_MAX) {
		spin_unlock_irqrestore(&registry_lock, irq);
		kern_free(device);
		return ENOSPC;
	}
	device->number = input_device_count++;
	spin_unlock_irqrestore(&registry_lock, irq);
	(void)snprintf(node, sizeof(node), "event%u", device->number);
	error = cdev_register(node, (dev_t)(0x00030000U + device->number),
			      &input_ops, device);
	if (error != 0) {
		kern_free(device);
		return error;
	}
	device->registered = 1;
	*result = device;
	hal_printf("input: /dev/input/%s: %s\n", node, device->name);
	return 0;
}

void
input_device_unregister(struct input_device *device)
{
	unsigned long irq;
	if (device == NULL)
		return;
	irq = spin_lock_irqsave(&device->lock);
	device->registered = 0;
	input_queue_detach(&device->queue);
	waitq_wake_all(&device->waitq);
	spin_unlock_irqrestore(&device->lock, irq);
	poll_notify();
}

void
input_device_emit(struct input_device *device, uint16_t type, uint16_t code,
		  int32_t value)
{
	struct input_event event;
	uint64_t milliseconds;
	unsigned long irq;
	int published = 0;
	if (device == NULL)
		return;
	milliseconds = clock_milliseconds(NULL);
	memset(&event, 0, sizeof(event));
	event.time.tv_sec = (time_t)(milliseconds / 1000U);
	event.time.tv_usec = (int64_t)((milliseconds % 1000U) * 1000U);
	event.type = type;
	event.code = code;
	event.value = value;
	irq = spin_lock_irqsave(&device->lock);
	if (device->registered &&
	    input_capability_event(&device->capability_state, type, code,
				   value)) {
		input_queue_push(&device->queue, &event);
		waitq_wake_all(&device->waitq);
		published = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);
	if (published)
		poll_notify();
}
