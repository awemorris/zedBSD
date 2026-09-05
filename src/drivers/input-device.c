/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-device.h"
#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/file.h"
#include "kern/input-capability.h"
#include "kern/input-keymap.h"
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
	int producer_opened;
};

struct input_device {
	struct spinlock lock;
	struct spinlock publication_lock;
	struct wait_queue waitq;
	struct input_queue queue;
	struct input_reader *readers;
	struct input_reader *grabber;
	struct input_id id;
	char name[INPUT_TEXT_MAX];
	char physical_path[INPUT_TEXT_MAX];
	char unique_id[INPUT_TEXT_MAX];
	struct input_capability_state capability_state;
	unsigned long resync_key_state[INPUT_BIT_WORDS(KEY_MAX)];
	int (*open)(void *);
	void (*close)(void *);
	void *context;
	struct cdev *cdev;
	refcount_t refs;
	unsigned number;
	unsigned flags;
	unsigned producer_callbacks;
	int registered;
	int retiring;
	int resyncing;
	int owner_released;
};

static struct spinlock registry_lock;
static struct input_device *input_device_reserved[INPUT_DEVICE_MAX];

static int input_device_tryref(struct input_device *device);
static void input_device_ref(struct input_device *device);
static void input_device_release(struct input_device *device);
static void input_cdev_finalize(void *data);

static void
report_timestamp(struct input_event *event, uint64_t milliseconds)
{
	memset(event, 0, sizeof(*event));
	event->time.tv_sec = (time_t)(milliseconds / 1000U);
	event->time.tv_usec = (int64_t)((milliseconds % 1000U) * 1000U);
}

static void
report_init(struct input_report *report, struct input_device *device)
{
	memset(report, 0, sizeof(*report));
	report->device = device;
	report->device_id = device->number;
}

static void
report_event(struct input_report *report, uint64_t milliseconds,
	uint16_t type, uint16_t code, int32_t value,
	const struct hal_key_event *key_event)
{
	struct input_report_event *item;
	size_t index;

	if (report->event_count == INPUT_REPORT_EVENT_MAX)
		return;
	item = &report->events[report->event_count++];
	report_timestamp(&item->event, milliseconds);
	item->event.type = type;
	item->event.code = code;
	item->event.value = value;
	if (key_event == NULL)
		return;
	for (index = 0; index < HAL_KEY_SYMBOL_SIZE; index++)
		item->symbol[index] = key_event->symbol[index];
	item->key_flags = key_event->flags;
}


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
producer_callback_enter(struct input_device *device)
{
	unsigned long irq = spin_lock_irqsave(&device->lock);

	if (!device->registered || device->retiring) {
		spin_unlock_irqrestore(&device->lock, irq);
		return ENODEV;
	}
	device->producer_callbacks++;
	spin_unlock_irqrestore(&device->lock, irq);
	return 0;
}

static void
producer_callback_leave(struct input_device *device)
{
	unsigned long irq = spin_lock_irqsave(&device->lock);

	if (device->producer_callbacks != 0)
		device->producer_callbacks--;
	waitq_wake_all(&device->waitq);
	spin_unlock_irqrestore(&device->lock, irq);
}

static int
input_open(struct file *file)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader;
	unsigned long irq;
	int error, attached = 0;

	if (device == NULL)
		return ENODEV;
	if ((file_status_flags_get(file) & O_ACCMODE) == O_WRONLY)
		return EACCES;
	reader = kern_calloc(1, sizeof(*reader));
	if (reader == NULL)
		return ENOMEM;
	error = producer_callback_enter(device);
	if (error != 0) {
		kern_free(reader);
		return error;
	}
	if (device->open != NULL) {
		error = device->open(device->context);
		if (error != 0) {
			producer_callback_leave(device);
			kern_free(reader);
			return error;
		}
	}
	irq = spin_lock_irqsave(&device->lock);
	if (device->registered && !device->retiring) {
		input_queue_reader_init(&device->queue, &reader->cursor);
		reader->producer_opened = device->close != NULL;
		reader->next = device->readers;
		device->readers = reader;
		file->f_data = reader;
		attached = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);
	/* Keep the admission held through the compensating close. */
	if (!attached && device->close != NULL)
		device->close(device->context);
	producer_callback_leave(device);
	if (!attached) {
		kern_free(reader);
		return ENODEV;
	}
	return 0;
}

static int
input_close(struct file *file)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file), **link;
	unsigned long irq;
	int close_producer = 0;
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
	if (reader->producer_opened) {
		reader->producer_opened = 0;
		device->producer_callbacks++;
		close_producer = 1;
	}
	file->f_data = NULL;
	spin_unlock_irqrestore(&device->lock, irq);
	if (close_producer)
		device->close(device->context);
	if (close_producer)
		producer_callback_leave(device);
	kern_free(reader);
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
	input_subscriber_init();
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
	unsigned slot;
	int error;

	if (result != NULL)
		*result = NULL;
	if (info == NULL || result == NULL || info->name == NULL)
		return EINVAL;
	if ((info->open == NULL) != (info->close == NULL))
		return EINVAL;
	if ((info->flags & ~(INPUT_DEVICE_KEY_MOMENTARY |
	    INPUT_DEVICE_KEY_REPEAT)) != 0 ||
	    (info->flags & (INPUT_DEVICE_KEY_MOMENTARY |
	    INPUT_DEVICE_KEY_REPEAT)) == (INPUT_DEVICE_KEY_MOMENTARY |
	    INPUT_DEVICE_KEY_REPEAT))
		return EINVAL;
	device = kern_calloc(1, sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	refcount_init(&device->refs, 1);
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
	device->flags = info->flags;
	spin_init(&device->lock, LOCK_RANK_DEVICE, "input device");
	spin_init(&device->publication_lock, LOCK_RANK_DEVICE,
	    "input publication");
	waitq_init(&device->waitq, "input event");
	input_queue_init(&device->queue);
	irq = spin_lock_irqsave(&registry_lock);
	for (slot = 0; slot < INPUT_DEVICE_MAX; slot++)
		if (input_device_reserved[slot] == NULL)
			break;
	if (slot == INPUT_DEVICE_MAX) {
		spin_unlock_irqrestore(&registry_lock, irq);
		input_device_release(device);
		return ENOSPC;
	}
	input_device_reserved[slot] = device;
	device->number = slot;
	spin_unlock_irqrestore(&registry_lock, irq);
	(void)snprintf(node, sizeof(node), "event%u", device->number);

	/* Gives the future cdev finalizer its device-lifetime reference. */
	input_device_ref(device);

	/* Managed publication exposes only completely initialized state. */
	device->registered = 1;
	error = cdev_register_managed(node,
	    (dev_t)(0x00030000U + device->number), &input_ops, device,
	    input_cdev_finalize, &device->cdev);
	if (error != 0) {
		device->registered = 0;
		input_device_release(device);
		input_device_release(device);
		return error;
	}
	*result = device;
	hal_printf("input: /dev/input/%s: %s\n", node, device->name);
	return 0;
}

void
input_device_unregister(struct input_device *device)
{
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
	struct input_report report;
	struct input_reader *reader;
	struct cdev *publication;
	uint64_t milliseconds;
	unsigned long irq, publication_irq;
	unsigned code, close_count = 0;
	int drop_owner;
	int released = 0, was_resyncing;

	if (device == NULL || !input_device_tryref(device))
		return;

	publication = NULL;
	drop_owner = 0;

	/*
	 * Retire callback admission before touching publication.  An open that
	 * was admitted first either installs its reader or performs its matching
	 * close before leaving; unregister joins it here.  Existing readers have
	 * their matching close transferred to this call, so no producer callback
	 * can run after unregister returns.
	 */
	irq = spin_lock_irqsave(&device->lock);
	/*
	 * Every remover joins the one thread which owns terminal publication.
	 * In particular, registered becomes false before DETACH is published, so
	 * observing that bit alone is not a sufficient unregister completion
	 * condition.
	 */
	while (device->retiring) {
		uint64_t sequence = waitq_sequence(&device->waitq);

		(void)waitq_sleep(&device->waitq, &device->lock, sequence, 0, 0);
	}
	if (!device->registered) {
		spin_unlock_irqrestore(&device->lock, irq);
		input_device_release(device);
		return;
	}
	device->retiring = 1;
	while (device->producer_callbacks != 0) {
		uint64_t sequence = waitq_sequence(&device->waitq);

		(void)waitq_sleep(&device->waitq, &device->lock, sequence, 0, 0);
	}
	for (reader = device->readers; reader != NULL; reader = reader->next)
		if (reader->producer_opened) {
			reader->producer_opened = 0;
			close_count++;
		}
	spin_unlock_irqrestore(&device->lock, irq);
	while (close_count != 0) {
		device->close(device->context);
		close_count--;
	}

	/* Removes the pathname before publishing terminal events to stale fds. */
	irq = spin_lock_irqsave(&device->lock);
	publication = device->cdev;
	device->cdev = NULL;
	spin_unlock_irqrestore(&device->lock, irq);
	if (publication != NULL)
		(void)cdev_unregister(publication);

	milliseconds = clock_milliseconds(NULL);
	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);
	was_resyncing = device->resyncing;
	device->resyncing = 0;
	memcpy(held, device->capability_state.key_state, sizeof(held));
	if (was_resyncing) {
		struct input_event event;

		report_timestamp(&event, milliseconds);
		event.type = EV_SYN;
		event.code = SYN_DROPPED;
		input_queue_push(&device->queue, &event);
	}
	for (code = 0; code <= KEY_MAX; code++) {
		struct input_event event;
		if ((held[code / INPUT_BITS_PER_WORD] &
		    (1UL << (code % INPUT_BITS_PER_WORD))) == 0)
			continue;
		report_timestamp(&event, milliseconds);
		event.type = EV_KEY;
		event.code = (uint16_t)code;
		event.value = 0;
		(void)input_capability_event(&device->capability_state,
		    EV_KEY, (uint16_t)code, 0);
		input_queue_push(&device->queue, &event);
		released = 1;
	}
	if (released || was_resyncing) {
		struct input_event event;
		report_timestamp(&event, milliseconds);
		event.type = EV_SYN;
		event.code = SYN_REPORT;
		input_queue_push(&device->queue, &event);
	}
	device->registered = 0;
	input_queue_detach(&device->queue);
	waitq_wake_all(&device->waitq);
	spin_unlock_irqrestore(&device->lock, irq);

	if (!was_resyncing) {
		report_init(&report, device);
		for (code = 0; code <= KEY_MAX; code++) {
			if ((held[code / INPUT_BITS_PER_WORD] &
			    (1UL << (code % INPUT_BITS_PER_WORD))) == 0)
				continue;
			if (report.event_count == INPUT_REPORT_EVENT_MAX) {
				input_subscriber_publish(&report);
				report_init(&report, device);
			}
			report_event(&report, milliseconds, EV_KEY,
			    (uint16_t)code, 0, NULL);
		}
		if (released) {
			if (report.event_count == INPUT_REPORT_EVENT_MAX) {
				input_subscriber_publish(&report);
				report_init(&report, device);
			}
			report_event(&report, milliseconds, EV_SYN, SYN_REPORT, 0,
			    NULL);
		}
		if (report.event_count != 0)
			input_subscriber_publish(&report);
	}
	report_init(&report, device);
	report.flags = INPUT_REPORT_DETACH;
	input_subscriber_publish(&report);
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
	poll_notify();

	/* DETACH and every transferred producer close are now terminal. */
	irq = spin_lock_irqsave(&device->lock);
	device->retiring = 0;
	if (!device->owner_released) {
		device->owner_released = 1;
		drop_owner = 1;
	}
	waitq_wake_all(&device->waitq);
	spin_unlock_irqrestore(&device->lock, irq);

	/* Releases publication, returned-owner, and this call's temporary refs. */
	if (publication != NULL)
		cdev_release(publication);
	if (drop_owner)
		input_device_release(device);
	input_device_release(device);
}

void
input_device_emit(struct input_device *device, uint16_t type, uint16_t code,
		  int32_t value)
{
	struct input_report report;
	uint64_t milliseconds;
	unsigned long irq, publication_irq;
	int published = 0;
	if (device == NULL)
		return;
	milliseconds = clock_milliseconds(NULL);
	report_init(&report, device);
	report_event(&report, milliseconds, type, code, value, NULL);
	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);
	if (device->registered && !device->retiring && !device->resyncing &&
	    input_capability_event(&device->capability_state, type, code,
				   value)) {
		input_queue_push(&device->queue, &report.events[0].event);
		waitq_wake_all(&device->waitq);
		published = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);
	if (published) {
		input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
	if (published)
		poll_notify();
}

static void
input_device_resync_begin(struct input_device *device, uint32_t key_flags)
{
	struct input_report report;
	unsigned long irq, publication_irq;
	int published = 0;

	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);
	if (device->registered && !device->retiring) {
		memset(device->resync_key_state, 0,
		    sizeof(device->resync_key_state));
		device->resyncing = 1;
		published = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);
	if (published) {
		report_init(&report, device);
		report.flags = INPUT_REPORT_RESYNC_BEGIN |
		    ((key_flags & HAL_KEY_EVENT_LOCK_CAPS) != 0 ?
		    INPUT_REPORT_LOCK_CAPS : 0U) |
		    ((key_flags & HAL_KEY_EVENT_LOCK_KANA) != 0 ?
		    INPUT_REPORT_LOCK_KANA : 0U);
		input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
}

static void
input_device_resync_snapshot(struct input_device *device,
	const struct hal_key_event *key_event)
{
	struct input_report report;
	uint64_t milliseconds = clock_milliseconds(NULL);
	unsigned long irq, publication_irq;
	uint16_t code = input_key_from_symbol(key_event->symbol);
	int logical_only = code == KEY_RESERVED &&
	    input_key_symbol_supported(key_event->symbol);
	int published = 0;

	if (code == KEY_RESERVED && !logical_only)
		return;
	report_init(&report, device);
	report.flags = INPUT_REPORT_SNAPSHOT;
	report_event(&report, milliseconds, EV_KEY, code, 1, key_event);
	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);
	if (device->registered && !device->retiring && device->resyncing) {
		if (logical_only) {
			published = 1;
		} else if ((device->capability_state.key_bits[
		    code / INPUT_BITS_PER_WORD] &
		    (1UL << (code % INPUT_BITS_PER_WORD))) != 0 &&
		    (device->resync_key_state[code / INPUT_BITS_PER_WORD] &
		    (1UL << (code % INPUT_BITS_PER_WORD))) == 0) {
			device->resync_key_state[code / INPUT_BITS_PER_WORD] |=
			    1UL << (code % INPUT_BITS_PER_WORD);
			published = 1;
		}
	}
	spin_unlock_irqrestore(&device->lock, irq);
	if (published)
		input_subscriber_publish(&report);
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
}

static void
input_device_resync_end(struct input_device *device)
{
	struct input_report report;
	struct input_event event;
	uint64_t milliseconds = clock_milliseconds(NULL);
	unsigned long irq, publication_irq;
	int published = 0;

	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);
	if (device->registered && !device->retiring && device->resyncing) {
		device->resyncing = 0;
		memcpy(device->capability_state.key_state,
		    device->resync_key_state,
		    sizeof(device->capability_state.key_state));
		report_timestamp(&event, milliseconds);
		event.type = EV_SYN;
		event.code = SYN_DROPPED;
		input_queue_push(&device->queue, &event);
		event.code = SYN_REPORT;
		input_queue_push(&device->queue, &event);
		waitq_wake_all(&device->waitq);
		published = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);
	if (published) {
		report_init(&report, device);
		report.flags = INPUT_REPORT_RESYNC_END;
		input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
	if (published)
		poll_notify();
}

void
input_device_emit_key_event(struct input_device *device,
	const struct hal_key_event *key_event)
{
	struct input_report report;
	uint64_t milliseconds;
	unsigned long irq, publication_irq;
	uint16_t code;
	int value, momentary, logical_only, published = 0;

	if (device == NULL || key_event == NULL ||
	    key_event->symbol[HAL_KEY_SYMBOL_SIZE - 1U] != '\0')
		return;
	if ((key_event->flags & ~(HAL_KEY_EVENT_RESYNC |
	    HAL_KEY_EVENT_LOCK_CAPS | HAL_KEY_EVENT_LOCK_KANA)) == 0 &&
	    (key_event->flags & HAL_KEY_EVENT_RESYNC) != 0 &&
	    key_event->symbol[0] == '\0') {
		input_device_resync_begin(device, key_event->flags);
		return;
	}
	if (key_event->flags == (HAL_KEY_EVENT_PRESS |
	    HAL_KEY_EVENT_SNAPSHOT)) {
		input_device_resync_snapshot(device, key_event);
		return;
	}
	if (key_event->flags == HAL_KEY_EVENT_RESYNC_END &&
	    key_event->symbol[0] == '\0') {
		input_device_resync_end(device);
		return;
	}
	if (key_event->flags == HAL_KEY_EVENT_PRESS)
		value = 1;
	else if (key_event->flags == HAL_KEY_EVENT_RELEASE)
		value = 0;
	else if (key_event->flags == HAL_KEY_EVENT_REPEAT)
		value = 2;
	else
		return;
	momentary = (device->flags & INPUT_DEVICE_KEY_MOMENTARY) != 0;
	if ((momentary && value != 1) ||
	    (value == 2 && (device->flags & INPUT_DEVICE_KEY_REPEAT) == 0))
		return;
	code = input_key_from_symbol(key_event->symbol);
	logical_only = code == KEY_RESERVED &&
	    input_key_symbol_supported(key_event->symbol);
	if (code == KEY_RESERVED && !logical_only)
		return;
	milliseconds = clock_milliseconds(NULL);
	report_init(&report, device);
	report_event(&report, milliseconds, EV_KEY, code, value, key_event);
	if (momentary) {
		struct hal_key_event release = *key_event;
		release.flags = HAL_KEY_EVENT_RELEASE;
		report_event(&report, milliseconds, EV_KEY, code, 0, &release);
	}
	report_event(&report, milliseconds, EV_SYN, SYN_REPORT, 0, NULL);

	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);
	if (device->registered && !device->retiring && !device->resyncing) {
		if (logical_only) {
			published = 1;
		} else if (input_capability_event(&device->capability_state,
		    EV_KEY, code, value)) {
			input_queue_push(&device->queue,
			    &report.events[0].event);
			if (momentary) {
				(void)input_capability_event(
				    &device->capability_state, EV_KEY, code, 0);
				input_queue_push(&device->queue,
				    &report.events[1].event);
			}
			input_queue_push(&device->queue,
			    &report.events[report.event_count - 1U].event);
			waitq_wake_all(&device->waitq);
			published = 1;
		}
	}
	spin_unlock_irqrestore(&device->lock, irq);
	if (published) {
		input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
	if (published)
		poll_notify();
}

/* Tries to retain an input generation across concurrent terminal removal. */
static int
input_device_tryref(
	struct input_device *device)
{
	return device != NULL && refcount_tryget(&device->refs);
}

/* Retains one input generation for an owned subsystem reference. */
static void
input_device_ref(
	struct input_device *device)
{
	if (device != NULL)
		refcount_get(&device->refs);
}

/* Releases the event number only when the complete generation is gone. */
static void
input_device_release(
	struct input_device *device)
{
	unsigned long irq;

	if (device == NULL || !refcount_put(&device->refs))
		return;

	irq = spin_lock_irqsave(&registry_lock);
	if (device->number < INPUT_DEVICE_MAX &&
	    input_device_reserved[device->number] == device)
		input_device_reserved[device->number] = NULL;
	spin_unlock_irqrestore(&registry_lock, irq);

	kern_free(device);
}

/* Releases the input reference owned by one terminal cdev generation. */
static void
input_cdev_finalize(
	void *data)
{
	input_device_release(data);
}
