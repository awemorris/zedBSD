/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Begin consolidated input-capability.c. */
/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-capability.h"

#include <errno.h>
#include <string.h>

/* Supports the bit test operation. */
static int bit_test(const unsigned long *bits, unsigned bit);

/* Supports the bit test operation. */
static int
bit_test(
	const unsigned long *bits,
	unsigned bit)
{
	/* Returns the computed result. */
	return (bits[bit / INPUT_BITS_PER_WORD] &
		(1UL << (bit % INPUT_BITS_PER_WORD))) != 0;
}

/* Supports the bit set operation. */
static void bit_set(unsigned long *bits, unsigned bit);

/* Supports the bit set operation. */
static void
bit_set(
	unsigned long *bits,
	unsigned bit)
{
	bits[bit / INPUT_BITS_PER_WORD] |= 1UL << (bit % INPUT_BITS_PER_WORD);
}

/* Supports the bit clear operation. */
static void bit_clear(unsigned long *bits, unsigned bit);

/* Supports the bit clear operation. */
static void
bit_clear(
	unsigned long *bits,
	unsigned bit)
{
	bits[bit / INPUT_BITS_PER_WORD] &=
		~(1UL << (bit % INPUT_BITS_PER_WORD));
}

/* Supports the capability bits mutable operation. */
static int capability_bits_mutable(struct input_capability_state *state, unsigned type, unsigned long **bits, size_t *size);

/* Supports the capability bits mutable operation. */
static int
capability_bits_mutable(
	struct input_capability_state *state,
	unsigned type,
	unsigned long **bits,
	size_t *size)
{
	/* Handles the state availability. */
	if (state == NULL || bits == NULL || size == NULL)
		return EINVAL;
	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case EV_KEY:
		*bits = state->key_bits;
		*size = sizeof(state->key_bits);
		/* Reports successful completion. */
		return 0;
	case EV_REL:
		*bits = state->rel_bits;
		*size = sizeof(state->rel_bits);
		/* Reports successful completion. */
		return 0;
	case EV_ABS:
		*bits = state->abs_bits;
		*size = sizeof(state->abs_bits);
		/* Reports successful completion. */
		return 0;
	default:
		/* Returns the computed result. */
		return EINVAL;
	}
}

/* Supports the capability code valid operation. */
static int capability_code_valid(unsigned type, unsigned code);

/* Supports the capability code valid operation. */
static int
capability_code_valid(
	unsigned type,
	unsigned code)
{
	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case EV_SYN:
		/* Returns the computed result. */
		return code == SYN_REPORT;
	case EV_KEY:
		/* Returns the computed result. */
		return code <= KEY_MAX;
	case EV_REL:
		/* Returns the computed result. */
		return code <= REL_MAX;
	case EV_ABS:
		/* Returns the computed result. */
		return code <= ABS_MAX;
	default:
		/* Reports successful completion. */
		return 0;
	}
}

/*
 * Implements the drv input capability state init operation.
 */
/*
 * Implements the drv input capability state init operation.
 */
int
drv_input_capability_state_init(
	struct input_capability_state *state,
	const struct input_capability *capabilities,
	size_t capability_count,
	const struct input_abs_axis *absolute_axes,
	size_t absolute_axis_count)
{
	const struct input_capability *capability;
	unsigned long *bits;
	size_t size;
	const struct input_abs_axis *axis;
	const struct input_absinfo *info;
	size_t i;

	/* Handles the state availability. */
	if (state == NULL || (capability_count != 0 && capabilities == NULL) ||
	    (absolute_axis_count != 0 && absolute_axes == NULL) ||
	    capability_count > INPUT_CAPABILITY_COUNT_MAX ||
	    absolute_axis_count > ABS_MAX + 1U)

		/* Returns the computed result. */
		return EINVAL;
	memset(state, 0, sizeof(*state));
	/* Process each remaining element. */
	for (i = 0; i < capability_count; i++) {
		capability = &capabilities[i];

		/* Checks the capability code valid result. */
		if (!capability_code_valid(capability->type, capability->code))
			return EINVAL;

		/* Handles the capability condition. */
		if (capability->type == EV_SYN) {
			/* Checks the bit test result. */
			if (bit_test(state->event_bits, EV_SYN))
				return EINVAL;
			bit_set(state->event_bits, EV_SYN);
			continue;
		}

		/* Checks the capability bits mutable result. */
		if (capability_bits_mutable(state, capability->type, &bits,
					    &size) != 0 ||
		    capability->code >= size * 8U ||
		    bit_test(bits, capability->code))

			/* Returns the computed result. */
			return EINVAL;
		bit_set(state->event_bits, capability->type);
		bit_set(bits, capability->code);
	}
	/* Process each remaining element. */
	for (i = 0; i < absolute_axis_count; i++) {
		axis = &absolute_axes[i];
		info = &axis->info;

		/* Checks the bit test result. */
		if (axis->code > ABS_MAX ||
		    !bit_test(state->abs_bits, axis->code) ||
		    bit_test(state->abs_configured, axis->code) ||
		    info->minimum > info->maximum ||
		    info->value < info->minimum ||
		    info->value > info->maximum || info->fuzz < 0 ||
		    info->flat < 0 || info->resolution < 0)

			/* Returns the computed result. */
			return EINVAL;
		state->abs_info[axis->code] = *info;
		bit_set(state->abs_configured, axis->code);
	}
	/* Process each element required by the operation. */
	for (i = 0; i <= ABS_MAX; i++) {
		/* Checks the bit test result. */
		if (bit_test(state->abs_bits, (unsigned)i) &&
		    !bit_test(state->abs_configured, (unsigned)i))

			/* Returns the computed result. */
			return EINVAL;
	}

	/* Checks the bit test result. */
	if (!bit_test(state->event_bits, EV_SYN))
		return EINVAL;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input capability bits operation.
 */
/*
 * Implements the drv input capability bits operation.
 */
int
drv_input_capability_bits(
	const struct input_capability_state *state,
	unsigned type,
	const uint8_t **bits,
	size_t *size)
{
	/* Handles the state availability. */
	if (state == NULL || bits == NULL || size == NULL)
		return EINVAL;

	/* Handles the type condition. */
	if (type == EV_SYN) {
		*bits = (const uint8_t *)state->event_bits;
		*size = sizeof(state->event_bits);
		/* Reports successful completion. */
		return 0;
	}
	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case EV_KEY:
		*bits = (const uint8_t *)state->key_bits;
		*size = sizeof(state->key_bits);
		/* Reports successful completion. */
		return 0;
	case EV_REL:
		*bits = (const uint8_t *)state->rel_bits;
		*size = sizeof(state->rel_bits);
		/* Reports successful completion. */
		return 0;
	case EV_ABS:
		*bits = (const uint8_t *)state->abs_bits;
		*size = sizeof(state->abs_bits);
		/* Reports successful completion. */
		return 0;
	default:
		/* Returns the computed result. */
		return EINVAL;
	}
}

/*
 * Implements the drv input capability key state operation.
 */
/*
 * Implements the drv input capability key state operation.
 */
int
drv_input_capability_key_state(
	const struct input_capability_state *state,
	const uint8_t **bits,
	size_t *size)
{
	/* Handles the state availability. */
	if (state == NULL || bits == NULL || size == NULL)
		return EINVAL;
	*bits = (const uint8_t *)state->key_state;
	*size = sizeof(state->key_state);
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input capability copy operation.
 */
/*
 * Implements the drv input capability copy operation.
 */
int
drv_input_capability_copy(
	const uint8_t *source,
	size_t source_size,
	size_t offset,
	uint8_t *destination,
	size_t capacity)
{
	size_t index;
	size_t i;

	/* Handles the source availability. */
	if ((source_size != 0 && source == NULL) ||
	    (capacity != 0 && destination == NULL) ||
	    capacity > SIZE_MAX - offset)

		/* Returns the computed result. */
		return EINVAL;
	/* Process each element required by the operation. */
	for (i = 0; i < capacity; i++) {
		index = offset + i;
		destination[i] = index < source_size ? source[index] : 0;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input capability abs info operation.
 */
/*
 * Implements the drv input capability abs info operation.
 */
int
drv_input_capability_abs_info(
	const struct input_capability_state *state,
	unsigned axis,
	struct input_absinfo *info)
{
	/* Handles the state availability. */
	if (state == NULL || info == NULL || axis > ABS_MAX)
		return EINVAL;

	/* Checks the bit test result. */
	if (!bit_test(state->abs_bits, axis))
		return ENOENT;
	*info = state->abs_info[axis];
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input capability event operation.
 */
/*
 * Implements the drv input capability event operation.
 */
int
drv_input_capability_event(
	struct input_capability_state *state,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	int function_result;

	/* Handles the state availability. */
	if (state == NULL)
		return 0;

	/* Handles the type condition. */
	if (type == EV_SYN) {
		/* Computes the function result. */
		function_result = code == SYN_REPORT && value == 0 &&
				  bit_test(state->event_bits, EV_SYN);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the bit test result. */
	if (type == EV_KEY && code <= KEY_MAX &&
	    bit_test(state->key_bits, code)) {
		/* Validates the current value. */
		if (value == 0)
			bit_clear(state->key_state, code);
		else if (value == 1 || value == 2)
			bit_set(state->key_state, code);
		else

			/* Reports successful completion. */
			return 0;

		/* Reports operation failure. */
		return 1;
	} else if (type == EV_REL && code <= REL_MAX &&
		   bit_test(state->rel_bits, code)) {
		/* Reports operation failure. */
		return 1;
	} else if (type == EV_ABS && code <= ABS_MAX &&
		   bit_test(state->abs_bits, code)) {
		state->abs_info[code].value = value;

		/* Reports operation failure. */
		return 1;
	}

	/* Reports successful completion. */
	return 0;
}
/* End consolidated input-capability.c. */

/* Begin consolidated input-device.c. */
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

static void report_timestamp(struct input_event *event, uint64_t milliseconds);

/* Supports the report timestamp operation. */
static void
report_timestamp(
	struct input_event *event,
	uint64_t milliseconds)
{
	memset(event, 0, sizeof(*event));
	event->time.tv_sec = (time_t)(milliseconds / 1000U);
	event->time.tv_usec = (int64_t)((milliseconds % 1000U) * 1000U);
}

static void report_init(struct input_report *report, struct input_device *device);

/* Supports the report init operation. */
static void
report_init(
	struct input_report *report,
	struct input_device *device)
{
	memset(report, 0, sizeof(*report));
	report->device = device;
	report->device_id = device->number;
}

static void report_event(struct input_report *report, uint64_t milliseconds, uint16_t type, uint16_t code, int32_t value, const struct hal_key_event *key_event);

/* Supports the report event operation. */
static void
report_event(
	struct input_report *report,
	uint64_t milliseconds,
	uint16_t type,
	uint16_t code,
	int32_t value,
	const struct hal_key_event *key_event)
{
	struct input_report_event *item;
	size_t index;

	/* Handles the report condition. */
	if (report->event_count == INPUT_REPORT_EVENT_MAX)
		return;
	item = &report->events[report->event_count++];
	report_timestamp(&item->event, milliseconds);
	item->event.type = type;
	item->event.code = code;
	item->event.value = value;

	/* Handles the key event availability. */
	if (key_event == NULL)
		return;
	/* Process each remaining element. */
	for (index = 0; index < HAL_KEY_SYMBOL_SIZE; index++)
		item->symbol[index] = key_event->symbol[index];
	item->key_flags = key_event->flags;
}

static struct input_device *file_device(struct file *file);

/* Supports the file device operation. */
static struct input_device *
file_device(
	struct file *file)
{
	/* Returns the computed result. */
	return file != NULL && file->f_inode != NULL &&
			       file->f_inode->i_data != NULL
		       ? ((const struct cdev *)file->f_inode->i_data)->data
		       : NULL;
}

static struct input_reader *file_reader(struct file *file);

/* Supports the file reader operation. */
static struct input_reader *
file_reader(
	struct file *file)
{
	/* Returns the computed result. */
	return file != NULL ? file->f_data : NULL;
}

static int producer_callback_enter(struct input_device *device);

/* Supports the producer callback enter operation. */
static int
producer_callback_enter(
	struct input_device *device)
{
	unsigned long irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (!device->registered || device->retiring) {
		spin_unlock_irqrestore(&device->lock, irq);

		/* Returns the computed result. */
		return ENODEV;
	}
	device->producer_callbacks++;
	spin_unlock_irqrestore(&device->lock, irq);

	/* Reports successful completion. */
	return 0;
}

static void producer_callback_leave(struct input_device *device);

/* Supports the producer callback leave operation. */
static void
producer_callback_leave(
	struct input_device *device)
{
	unsigned long irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->producer_callbacks != 0)
		device->producer_callbacks--;
	waitq_wake_all(&device->waitq);
	spin_unlock_irqrestore(&device->lock, irq);
}

static int input_open(struct file *file);

/* Supports the input open operation. */
static int
input_open(
	struct file *file)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader;
	unsigned long irq;
	int error, attached = 0;

	/* Handles the device availability. */
	if (device == NULL)
		return ENODEV;

	/* Checks the file status flags get result. */
	if ((file_status_flags_get(file) & O_ACCMODE) == O_WRONLY)
		return EACCES;
	reader = kern_calloc(1, sizeof(*reader));

	/* Handles the reader availability. */
	if (reader == NULL)
		return ENOMEM;
	error = producer_callback_enter(device);

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(reader);

		/* Returns the computed result. */
		return error;
	}

	/* Handles the open availability. */
	if (device->open != NULL) {
		error = device->open(device->context);

		/* Checks the operation status. */
		if (error != 0) {
			producer_callback_leave(device);
			kern_free(reader);

			/* Returns the computed result. */
			return error;
		}
	}
	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->registered && !device->retiring) {
		drv_input_queue_reader_init(&device->queue, &reader->cursor);
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

	/* Handles the attached condition. */
	if (!attached) {
		kern_free(reader);

		/* Returns the computed result. */
		return ENODEV;
	}

	/* Reports successful completion. */
	return 0;
}

static int input_close(struct file *file);

/* Supports the input close operation. */
static int
input_close(
	struct file *file)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file), **link;
	unsigned long irq;
	int close_producer = 0;

	/* Handles the device availability. */
	if (device == NULL || reader == NULL)
		return 0;
	irq = spin_lock_irqsave(&device->lock);
	/* Process each linked entry. */
	for (link = &device->readers; *link != NULL; link = &(*link)->next) {
		/* Handles the link condition. */
		if (*link == reader) {
			*link = reader->next;
			break;
		}
	}

	/* Handles the device condition. */
	if (device->grabber == reader)
		device->grabber = NULL;

	/* Handles the reader condition. */
	if (reader->producer_opened) {
		reader->producer_opened = 0;
		device->producer_callbacks++;
		close_producer = 1;
	}
	file->f_data = NULL;
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the close producer condition. */
	if (close_producer)
		device->close(device->context);

	/* Handles the close producer condition. */
	if (close_producer)
		producer_callback_leave(device);
	kern_free(reader);

	/* Reports successful completion. */
	return 0;
}

static ssize_t input_read(struct file *file, void *buffer, size_t size);

/* Supports the input read operation. */
static ssize_t
input_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	ssize_t function_result;
	uint64_t sequence;
	int error;
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file);
	size_t capacity, count;
	unsigned long irq;

	/* Handles the device availability. */
	if (device == NULL || reader == NULL)
		return -ENODEV;

	/* Checks the current data size. */
	if (size < sizeof(struct input_event))
		return -EINVAL;
	capacity = size / sizeof(struct input_event);
	irq = spin_lock_irqsave(&device->lock);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the grabber availability. */
		if (device->grabber == NULL || device->grabber == reader) {
			count = drv_input_queue_read(&device->queue,
						     &reader->cursor, buffer,
						     capacity);

			/* Checks the remaining item count. */
			if (count != 0) {
				spin_unlock_irqrestore(&device->lock, irq);

				/* Computes the function result. */
				function_result =
					(ssize_t)(count *
						  sizeof(struct input_event));

				/* Returns the computed result. */
				return function_result;
			}
		}

		/* Handles the device condition. */
		if (!device->registered) {
			spin_unlock_irqrestore(&device->lock, irq);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the file status flags get result. */
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&device->lock, irq);

			/* Returns the computed result. */
			return -EAGAIN;
		}
		sequence = waitq_sequence(&device->waitq);
		error = waitq_sleep(&device->waitq, &device->lock, sequence, 0,
				    WAITQ_INTERRUPTIBLE);

		/* Checks the operation status. */
		if (error == EINTR) {
			spin_unlock_irqrestore(&device->lock, irq);

			/* Returns the computed result. */
			return -EINTR;
		}
	}
}

static int input_poll(struct file *file, short requested, short *returned);

/* Supports the input poll operation. */
static int
input_poll(
	struct file *file,
	short requested,
	short *returned)
{
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file);
	unsigned long irq;
	short result = 0;

	/* Handles the returned availability. */
	if (returned == NULL)
		return EINVAL;

	/* Handles the device availability. */
	if (device == NULL || reader == NULL) {
		*returned = POLLERR | POLLHUP;
		/* Reports successful completion. */
		return 0;
	}
	irq = spin_lock_irqsave(&device->lock);

	/* Checks the drv input queue readable result. */
	if ((device->grabber == NULL || device->grabber == reader) &&
	    drv_input_queue_readable(&device->queue, &reader->cursor))
		result |= requested & (POLLIN | POLLRDNORM);

	/* Handles the device condition. */
	if (!device->registered)
		result |= POLLHUP;
	spin_unlock_irqrestore(&device->lock, irq);
	*returned = result;
	/* Reports successful completion. */
	return 0;
}

static int copy_text(const char *text, unsigned long request, uintptr_t argument);

/* Supports the copy text operation. */
static int
copy_text(
	const char *text,
	unsigned long request,
	uintptr_t argument)
{
	int function_result;
	size_t capacity = (request >> 16) & 0x1fffU;
	size_t length = strlen(text) + 1U;

	/* Handles the capacity condition. */
	if (capacity == 0)
		return EINVAL;

	/* Checks the current data length. */
	if (length > capacity)
		length = capacity;

	/* Obtains the copyout result. */
	function_result = copyout(text, argument, length);

	/* Returns the computed result. */
	return function_result;
}

static size_t ioctl_size(unsigned long request);

/* Supports the ioctl size operation. */
static size_t
ioctl_size(
	unsigned long request)
{
	/* Returns the computed result. */
	return (request >> 16) & 0x1fffU;
}

static int copy_bits(const uint8_t *bits, size_t bit_size, size_t capacity, uintptr_t argument);

/* Supports the copy bits operation. */
static int
copy_bits(
	const uint8_t *bits,
	size_t bit_size,
	size_t capacity,
	uintptr_t argument)
{
	uint8_t output[32];
	size_t copied = 0, count;
	uintptr_t address;
	int error;

	/* Continue while the operation condition remains true. */
	while (copied < capacity) {
		count = capacity - copied;

		/* Checks the remaining item count. */
		if (count > sizeof(output))
			count = sizeof(output);
		error = drv_input_capability_copy(bits, bit_size, copied,
						  output, count);

		/* Checks the operation status. */
		if (error == 0)
			error = user_address_add(argument, copied, &address);

		/* Checks the operation status. */
		if (error == 0)
			error = copyout(output, address, count);

		/* Checks the operation status. */
		if (error != 0)
			return error;
		copied += count;
	}

	/* Reports successful completion. */
	return 0;
}

static int copy_capability_bits(const struct input_device *device, unsigned type, size_t capacity, uintptr_t argument);

/* Supports the copy capability bits operation. */
static int
copy_capability_bits(
	const struct input_device *device,
	unsigned type,
	size_t capacity,
	uintptr_t argument)
{
	int function_result;
	const uint8_t *bits;
	size_t size;
	int error = drv_input_capability_bits(&device->capability_state, type,
					      &bits, &size);

	/* Checks the operation status. */
	if (error != 0)
		return ENOTTY;

	/* Obtains the copy bits result. */
	function_result = copy_bits(bits, size, capacity, argument);

	/* Returns the computed result. */
	return function_result;
}

static int copy_key_state(struct input_device *device, size_t capacity, uintptr_t argument);

/* Supports the copy key state operation. */
static int
copy_key_state(
	struct input_device *device,
	size_t capacity,
	uintptr_t argument)
{
	int function_result;
	uint8_t snapshot[INPUT_KEY_BITS_SIZE];
	const uint8_t *bits;
	size_t size;
	unsigned long irq;

	irq = spin_lock_irqsave(&device->lock);
	(void)drv_input_capability_key_state(&device->capability_state, &bits,
					     &size);
	memcpy(snapshot, bits, sizeof(snapshot));
	spin_unlock_irqrestore(&device->lock, irq);

	/* Obtains the copy bits result. */
	function_result = copy_bits(snapshot, size, capacity, argument);

	/* Returns the computed result. */
	return function_result;
}

static int copy_abs_info(struct input_device *device, unsigned axis, uintptr_t argument);

/* Supports the copy abs info operation. */
static int
copy_abs_info(
	struct input_device *device,
	unsigned axis,
	uintptr_t argument)
{
	int function_result;
	struct input_absinfo info;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&device->lock);
	error = drv_input_capability_abs_info(&device->capability_state, axis,
					      &info);
	spin_unlock_irqrestore(&device->lock, irq);

	/* Checks the operation status. */
	if (error == ENOENT)
		return ENOTTY;

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Obtains the copyout result. */
	function_result = copyout(&info, argument, sizeof(info));

	/* Returns the computed result. */
	return function_result;
}

static int input_ioctl(struct file *file, unsigned long request, uintptr_t argument);

/* Supports the input ioctl operation. */
static int
input_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int function_result;
	struct input_device *device = file_device(file);
	struct input_reader *reader = file_reader(file), *item;
	unsigned group = (unsigned)((request >> 8) & 0xffU);
	unsigned number = (unsigned)(request & 0xffU);
	size_t size = ioctl_size(request);
	unsigned long irq;
	int value, error = 0;

	/* Handles the device availability. */
	if (device == NULL || reader == NULL)
		return ENODEV;

	/* Handles the request condition. */
	if (request == EVIOCGVERSION) {
		value = EV_VERSION;

		/* Obtains the copyout result. */
		function_result = copyout(&value, argument, sizeof(value));

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the request condition. */
	if (request == EVIOCGID) {
		/* Obtains the copyout result. */
		function_result =
			copyout(&device->id, argument, sizeof(device->id));

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the group condition. */
	if (group == ZEDBSD_EVDEV_IOC_GROUP) {
		/* Dispatch the selected operation case. */
		switch (number) {
		case 0x06:
			/* Checks the EVIOCGNAME result. */
			if (request != EVIOCGNAME(size))
				return ENOTTY;

			/* Obtains the copy text result. */
			function_result =
				copy_text(device->name, request, argument);

			/* Returns the computed result. */
			return function_result;
		case 0x07:
			/* Checks the EVIOCGPHYS result. */
			if (request != EVIOCGPHYS(size))
				return ENOTTY;

			/* Obtains the copy text result. */
			function_result = copy_text(device->physical_path,
						    request, argument);

			/* Returns the computed result. */
			return function_result;
		case 0x08:
			/* Checks the EVIOCGUNIQ result. */
			if (request != EVIOCGUNIQ(size))
				return ENOTTY;

			/* Obtains the copy text result. */
			function_result =
				copy_text(device->unique_id, request, argument);

			/* Returns the computed result. */
			return function_result;
		default:
			break;
		}
	}

	/* Checks the EVIOCGKEY result. */
	if (group == ZEDBSD_EVDEV_IOC_GROUP && number == 0x18U &&
	    request == EVIOCGKEY(size)) {
		/* Obtains the copy key state result. */
		function_result = copy_key_state(device, size, argument);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the EVIOCGBIT result. */
	if (group == ZEDBSD_EVDEV_IOC_GROUP && number >= 0x20U &&
	    number <= 0x20U + EV_MAX &&
	    request == EVIOCGBIT(number - 0x20U, size)) {
		/* Obtains the copy capability bits result. */
		function_result = copy_capability_bits(device, number - 0x20U,
						       size, argument);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the EVIOCGABS result. */
	if (group == ZEDBSD_EVDEV_IOC_GROUP && number >= 0x40U &&
	    number <= 0x40U + ABS_MAX && request == EVIOCGABS(number - 0x40U)) {
		/* Obtains the copy abs info result. */
		function_result =
			copy_abs_info(device, number - 0x40U, argument);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the request condition. */
	if (request != EVIOCGRAB)
		return ENOTTY;

	/* Checks the operation status. */
	if ((error = copyin(argument, &value, sizeof(value))) != 0)
		return error;
	irq = spin_lock_irqsave(&device->lock);

	/* Validates the current value. */
	if (value != 0) {
		/* Handles the grabber availability. */
		if (device->grabber != NULL && device->grabber != reader)
			error = EBUSY;
		else
			device->grabber = reader;
	} else if (device->grabber != reader) {
		error = EINVAL;
	} else {
		device->grabber = NULL;
		/* Process each linked entry. */
		for (item = device->readers; item != NULL; item = item->next) {
			/* Handles the item condition. */
			if (item != reader) {
				item->cursor.sequence =
					device->queue.next_sequence;
			}
		}
		waitq_wake_all(&device->waitq);
	}
	spin_unlock_irqrestore(&device->lock, irq);

	/* Checks the operation status. */
	if (error == 0)
		poll_notify();

	/* Returns the computed result. */
	return error;
}

static const struct cdev_ops input_ops = {
	.open = input_open,
	.close = input_close,
	.read = input_read,
	.ioctl = input_ioctl,
	.poll = input_poll,
};

/*
 * Implements the drv input core init operation.
 */
void
drv_input_core_init(
	void)
{
	spin_init(&registry_lock, LOCK_RANK_DEVICE, "input registry");
	drv_input_subscriber_init();
}

static int copy_info_text(char *destination, const char *source);

/* Supports the copy info text operation. */
static int
copy_info_text(
	char *destination,
	const char *source)
{
	/* Handles the source availability. */
	if (source == NULL)
		source = "";

	/* Checks the strlen result. */
	if (strlen(source) >= INPUT_TEXT_MAX)
		return ENAMETOOLONG;
	strcpy(destination, source);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input device register operation.
 */
int
drv_input_device_register(
	const struct input_device_info *info,
	struct input_device **result)
{
	struct input_device *device;
	char node[16];
	unsigned long irq;
	unsigned slot;
	int error;

	/* Handles the result availability. */
	if (result != NULL)
		*result = NULL;
	/* Handles the info availability. */
	if (info == NULL || result == NULL || info->name == NULL)
		return EINVAL;

	/* Handles the open availability. */
	if ((info->open == NULL) != (info->close == NULL))
		return EINVAL;

	/* Handles the info condition. */
	if ((info->flags &
	     ~(INPUT_DEVICE_KEY_MOMENTARY | INPUT_DEVICE_KEY_REPEAT)) != 0 ||
	    (info->flags &
	     (INPUT_DEVICE_KEY_MOMENTARY | INPUT_DEVICE_KEY_REPEAT)) ==
		    (INPUT_DEVICE_KEY_MOMENTARY | INPUT_DEVICE_KEY_REPEAT))

		/* Returns the computed result. */
		return EINVAL;
	device = kern_calloc(1, sizeof(*device));

	/* Handles the device availability. */
	if (device == NULL)
		return ENOMEM;
	refcount_init(&device->refs, 1);

	/* Checks the operation status. */
	if ((error = copy_info_text(device->name, info->name)) != 0 ||
	    (error = copy_info_text(device->physical_path,
				    info->physical_path)) != 0 ||
	    (error = copy_info_text(device->unique_id, info->unique_id)) != 0) {
		kern_free(device);

		/* Returns the computed result. */
		return error;
	}
	device->id = info->id;
	error = drv_input_capability_state_init(
		&device->capability_state, info->capabilities,
		info->capability_count, info->absolute_axes,
		info->absolute_axis_count);

	/* Checks the operation status. */
	if (error != 0) {
		kern_free(device);

		/* Returns the computed result. */
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
	drv_input_queue_init(&device->queue);
	irq = spin_lock_irqsave(&registry_lock);
	/* Process each element required by the operation. */
	for (slot = 0; slot < INPUT_DEVICE_MAX; slot++) {
		/* Handles the input device reserved condition. */
		if (input_device_reserved[slot] == NULL)
			break;
	}

	/* Handles the slot condition. */
	if (slot == INPUT_DEVICE_MAX) {
		spin_unlock_irqrestore(&registry_lock, irq);
		input_device_release(device);

		/* Returns the computed result. */
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
	error = cdev_register_managed(
		node, (dev_t)(0x00030000U + device->number), &input_ops, device,
		input_cdev_finalize, &device->cdev);

	/* Checks the operation status. */
	if (error != 0) {
		device->registered = 0;
		input_device_release(device);
		input_device_release(device);

		/* Returns the computed result. */
		return error;
	}
	*result = device;
	hal_printf("input: /dev/input/%s: %s\n", node, device->name);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input device unregister operation.
 */
void
drv_input_device_unregister(
	struct input_device *device)
{
	uint64_t sequence_local;
	uint64_t sequence_local1;
	struct input_event event_local;
	struct input_event event_local2;
	struct input_event event_local3;
	unsigned long held[INPUT_BIT_WORDS(KEY_MAX)];
	struct input_report report;
	struct input_reader *reader;
	struct cdev *publication;
	uint64_t milliseconds;
	unsigned long irq, publication_irq;
	unsigned code, close_count = 0;
	int drop_owner;
	int released = 0, was_resyncing;

	/* Checks the input device tryref result. */
	if (device == NULL || !input_device_tryref(device))
		return;

	publication = NULL;
	drop_owner = 0;

	/*
	 * Retire callback admission before touching publication.  An open that
	 * was admitted first either installs its reader or performs its
	 * matching close before leaving; unregister joins it here.  Existing
	 * readers have their matching close transferred to this call, so no
	 * producer callback can run after unregister returns.
	 */
	irq = spin_lock_irqsave(&device->lock);

	/*
	 * Every remover joins the one thread which owns terminal publication.
	 * In particular, registered becomes false before DETACH is published,
	 * so observing that bit alone is not a sufficient unregister completion
	 * condition.
	 */
	/* Continue while the operation condition remains true. */
	while (device->retiring) {
		sequence_local = waitq_sequence(&device->waitq);

		(void)waitq_sleep(&device->waitq, &device->lock, sequence_local,
				  0, 0);
	}

	/* Handles the device condition. */
	if (!device->registered) {
		spin_unlock_irqrestore(&device->lock, irq);
		input_device_release(device);

		/* Returns the computed result. */
		return;
	}
	device->retiring = 1;
	/* Continue while the operation condition remains true. */
	while (device->producer_callbacks != 0) {
		sequence_local1 = waitq_sequence(&device->waitq);

		(void)waitq_sleep(&device->waitq, &device->lock,
				  sequence_local1, 0, 0);
	}
	/* Process each linked entry. */
	for (reader = device->readers; reader != NULL; reader = reader->next) {
		/* Handles the reader condition. */
		if (reader->producer_opened) {
			reader->producer_opened = 0;
			close_count++;
		}
	}
	spin_unlock_irqrestore(&device->lock, irq);
	/* Process each remaining element. */
	while (close_count != 0) {
		device->close(device->context);
		close_count--;
	}

	/*
 * Removes the pathname before publishing terminal events to stale fds.
	 */
	irq = spin_lock_irqsave(&device->lock);
	publication = device->cdev;
	device->cdev = NULL;
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the publication availability. */
	if (publication != NULL)
		(void)cdev_unregister(publication);

	/* Takes the held keys under both device locks. */
	milliseconds = clock_milliseconds(NULL);
	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);
	was_resyncing = device->resyncing;
	device->resyncing = 0;
	memcpy(held, device->capability_state.key_state, sizeof(held));

	/* Handles the was resyncing condition. */
	if (was_resyncing) {
		report_timestamp(&event_local, milliseconds);
		event_local.type = EV_SYN;
		event_local.code = SYN_DROPPED;
		drv_input_queue_push(&device->queue, &event_local);
	}
	/* Process each element required by the operation. */
	for (code = 0; code <= KEY_MAX; code++) {
		/* Handles the held condition. */
		if ((held[code / INPUT_BITS_PER_WORD] &
		     (1UL << (code % INPUT_BITS_PER_WORD))) == 0)
			continue;
		report_timestamp(&event_local2, milliseconds);
		event_local2.type = EV_KEY;
		event_local2.code = (uint16_t)code;
		event_local2.value = 0;
		(void)drv_input_capability_event(&device->capability_state,
						 EV_KEY, (uint16_t)code, 0);
		drv_input_queue_push(&device->queue, &event_local2);
		released = 1;
	}

	/* Handles the released condition. */
	if (released || was_resyncing) {
		report_timestamp(&event_local3, milliseconds);
		event_local3.type = EV_SYN;
		event_local3.code = SYN_REPORT;
		drv_input_queue_push(&device->queue, &event_local3);
	}
	device->registered = 0;
	drv_input_queue_detach(&device->queue);
	waitq_wake_all(&device->waitq);
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the was resyncing condition. */
	if (!was_resyncing) {
		report_init(&report, device);
		/* Process each element required by the operation. */
		for (code = 0; code <= KEY_MAX; code++) {
			/* Handles the held condition. */
			if ((held[code / INPUT_BITS_PER_WORD] &
			     (1UL << (code % INPUT_BITS_PER_WORD))) == 0)
				continue;

			/* Handles the report condition. */
			if (report.event_count == INPUT_REPORT_EVENT_MAX) {
				drv_input_subscriber_publish(&report);
				report_init(&report, device);
			}
			report_event(&report, milliseconds, EV_KEY,
				     (uint16_t)code, 0, NULL);
		}

		/* Handles the released condition. */
		if (released) {
			/* Handles the report condition. */
			if (report.event_count == INPUT_REPORT_EVENT_MAX) {
				drv_input_subscriber_publish(&report);
				report_init(&report, device);
			}
			report_event(&report, milliseconds, EV_SYN, SYN_REPORT,
				     0, NULL);
		}

		/* Handles the report condition. */
		if (report.event_count != 0)
			drv_input_subscriber_publish(&report);
	}
	report_init(&report, device);
	report.flags = INPUT_REPORT_DETACH;
	drv_input_subscriber_publish(&report);
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
	poll_notify();

	/* DETACH and every transferred producer close are now terminal. */
	irq = spin_lock_irqsave(&device->lock);
	device->retiring = 0;

	/* Handles the device condition. */
	if (!device->owner_released) {
		device->owner_released = 1;
		drop_owner = 1;
	}
	waitq_wake_all(&device->waitq);
	spin_unlock_irqrestore(&device->lock, irq);

	/*
 * Releases publication, returned-owner, and this call's temporary refs.
	 */
	if (publication != NULL)
		cdev_release(publication);

	/* Handles the drop owner condition. */
	if (drop_owner)
		input_device_release(device);
	input_device_release(device);
}

/*
 * Implements the drv input device emit operation.
 */
void
drv_input_device_emit(
	struct input_device *device,
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	struct input_report report;
	uint64_t milliseconds;
	unsigned long irq, publication_irq;
	int published = 0;

	/* Handles the device availability. */
	if (device == NULL)
		return;
	milliseconds = clock_milliseconds(NULL);
	report_init(&report, device);
	report_event(&report, milliseconds, type, code, value, NULL);
	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);

	/* Checks the drv input capability event result. */
	if (device->registered && !device->retiring && !device->resyncing &&
	    drv_input_capability_event(&device->capability_state, type, code,
				       value)) {
		drv_input_queue_push(&device->queue, &report.events[0].event);
		waitq_wake_all(&device->waitq);
		published = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the published condition. */
	if (published) {
		drv_input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);

	/* Handles the published condition. */
	if (published)
		poll_notify();
}

static void input_device_resync_begin(struct input_device *device, uint32_t key_flags);

/* Supports the input device resync begin operation. */
static void
input_device_resync_begin(
	struct input_device *device,
	uint32_t key_flags)
{
	struct input_report report;
	unsigned long irq, publication_irq;
	int published = 0;

	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->registered && !device->retiring) {
		memset(device->resync_key_state, 0,
		       sizeof(device->resync_key_state));
		device->resyncing = 1;
		published = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the published condition. */
	if (published) {
		report_init(&report, device);
		report.flags = INPUT_REPORT_RESYNC_BEGIN |
			       ((key_flags & HAL_KEY_EVENT_LOCK_CAPS) != 0
					? INPUT_REPORT_LOCK_CAPS
					: 0U) |
			       ((key_flags & HAL_KEY_EVENT_LOCK_KANA) != 0
					? INPUT_REPORT_LOCK_KANA
					: 0U);
		drv_input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
}

static void input_device_resync_snapshot(struct input_device *device, const struct hal_key_event *key_event);

/* Supports the input device resync snapshot operation. */
static void
input_device_resync_snapshot(
	struct input_device *device,
	const struct hal_key_event *key_event)
{
	struct input_report report;
	uint64_t milliseconds = clock_milliseconds(NULL);
	unsigned long irq, publication_irq;
	uint16_t code = drv_input_key_from_symbol(key_event->symbol);
	int logical_only = code == KEY_RESERVED &&
			   drv_input_key_symbol_supported(key_event->symbol);
	int published = 0;

	/* Handles the code condition. */
	if (code == KEY_RESERVED && !logical_only)
		return;
	report_init(&report, device);
	report.flags = INPUT_REPORT_SNAPSHOT;
	report_event(&report, milliseconds, EV_KEY, code, 1, key_event);
	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->registered && !device->retiring && device->resyncing) {
		/* Handles the logical only condition. */
		if (logical_only) {
			published = 1;
		} else if ((device->capability_state
				    .key_bits[code / INPUT_BITS_PER_WORD] &
			    (1UL << (code % INPUT_BITS_PER_WORD))) != 0 &&
			   (device->resync_key_state[code /
						     INPUT_BITS_PER_WORD] &
			    (1UL << (code % INPUT_BITS_PER_WORD))) == 0) {
			device->resync_key_state[code / INPUT_BITS_PER_WORD] |=
				1UL << (code % INPUT_BITS_PER_WORD);
			published = 1;
		}
	}
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the published condition. */
	if (published)
		drv_input_subscriber_publish(&report);
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);
}

static void input_device_resync_end(struct input_device *device);

/* Supports the input device resync end operation. */
static void
input_device_resync_end(
	struct input_device *device)
{
	struct input_report report;
	struct input_event event;
	uint64_t milliseconds = clock_milliseconds(NULL);
	unsigned long irq, publication_irq;
	int published = 0;

	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->registered && !device->retiring && device->resyncing) {
		device->resyncing = 0;
		memcpy(device->capability_state.key_state,
		       device->resync_key_state,
		       sizeof(device->capability_state.key_state));
		report_timestamp(&event, milliseconds);
		event.type = EV_SYN;
		event.code = SYN_DROPPED;
		drv_input_queue_push(&device->queue, &event);
		event.code = SYN_REPORT;
		drv_input_queue_push(&device->queue, &event);
		waitq_wake_all(&device->waitq);
		published = 1;
	}
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the published condition. */
	if (published) {
		report_init(&report, device);
		report.flags = INPUT_REPORT_RESYNC_END;
		drv_input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);

	/* Handles the published condition. */
	if (published)
		poll_notify();
}

/*
 * Implements the drv input device emit key event operation.
 */
void
drv_input_device_emit_key_event(
	struct input_device *device,
	const struct hal_key_event *key_event)
{
	struct hal_key_event release;
	struct input_report report;
	uint64_t milliseconds;
	unsigned long irq, publication_irq;
	uint16_t code;
	int value, momentary, logical_only, published = 0;

	/* Handles the device availability. */
	if (device == NULL || key_event == NULL ||
	    key_event->symbol[HAL_KEY_SYMBOL_SIZE - 1U] != '\0')

		/* Returns the computed result. */
		return;

	/* Handles the key event condition. */
	if ((key_event->flags &
	     ~(HAL_KEY_EVENT_RESYNC | HAL_KEY_EVENT_LOCK_CAPS |
	       HAL_KEY_EVENT_LOCK_KANA)) == 0 &&
	    (key_event->flags & HAL_KEY_EVENT_RESYNC) != 0 &&
	    key_event->symbol[0] == '\0') {
		input_device_resync_begin(device, key_event->flags);

		/* Returns the computed result. */
		return;
	}

	/* Handles the key event condition. */
	if (key_event->flags ==
	    (HAL_KEY_EVENT_PRESS | HAL_KEY_EVENT_SNAPSHOT)) {
		input_device_resync_snapshot(device, key_event);

		/* Returns the computed result. */
		return;
	}

	/* Handles the key event condition. */
	if (key_event->flags == HAL_KEY_EVENT_RESYNC_END &&
	    key_event->symbol[0] == '\0') {
		input_device_resync_end(device);

		/* Returns the computed result. */
		return;
	}

	/* Handles the key event condition. */
	if (key_event->flags == HAL_KEY_EVENT_PRESS)
		value = 1;
	else if (key_event->flags == HAL_KEY_EVENT_RELEASE)
		value = 0;
	else if (key_event->flags == HAL_KEY_EVENT_REPEAT)
		value = 2;
	else

		/* Returns the computed result. */
		return;
	momentary = (device->flags & INPUT_DEVICE_KEY_MOMENTARY) != 0;

	/* Handles the momentary condition. */
	if ((momentary && value != 1) ||
	    (value == 2 && (device->flags & INPUT_DEVICE_KEY_REPEAT) == 0))

		/* Returns the computed result. */
		return;
	code = drv_input_key_from_symbol(key_event->symbol);
	logical_only = code == KEY_RESERVED &&
		       drv_input_key_symbol_supported(key_event->symbol);

	/* Handles the code condition. */
	if (code == KEY_RESERVED && !logical_only)
		return;
	milliseconds = clock_milliseconds(NULL);
	report_init(&report, device);
	report_event(&report, milliseconds, EV_KEY, code, value, key_event);

	/* Handles the momentary condition. */
	if (momentary) {
		release = *key_event;
		release.flags = HAL_KEY_EVENT_RELEASE;
		report_event(&report, milliseconds, EV_KEY, code, 0, &release);
	}
	report_event(&report, milliseconds, EV_SYN, SYN_REPORT, 0, NULL);

	publication_irq = spin_lock_irqsave(&device->publication_lock);
	irq = spin_lock_irqsave(&device->lock);

	/* Handles the device condition. */
	if (device->registered && !device->retiring && !device->resyncing) {
		/* Handles the logical only condition. */
		if (logical_only) {
			published = 1;
		} else if (drv_input_capability_event(&device->capability_state,
						      EV_KEY, code, value)) {
			drv_input_queue_push(&device->queue,
					     &report.events[0].event);

			/* Handles the momentary condition. */
			if (momentary) {
				(void)drv_input_capability_event(
					&device->capability_state, EV_KEY, code,
					0);
				drv_input_queue_push(&device->queue,
						     &report.events[1].event);
			}
			drv_input_queue_push(
				&device->queue,
				&report.events[report.event_count - 1U].event);
			waitq_wake_all(&device->waitq);
			published = 1;
		}
	}
	spin_unlock_irqrestore(&device->lock, irq);

	/* Handles the published condition. */
	if (published) {
		drv_input_subscriber_publish(&report);
	}
	spin_unlock_irqrestore(&device->publication_lock, publication_irq);

	/* Handles the published condition. */
	if (published)
		poll_notify();
}

/* Tries to retain an input generation across concurrent terminal removal. */
static int
input_device_tryref(
	struct input_device *device)
{
	int function_result;

	/* Computes the function result. */
	function_result = device != NULL && refcount_tryget(&device->refs);

	/* Returns the computed result. */
	return function_result;
}

/* Retains one input generation for an owned subsystem reference. */
static void
input_device_ref(
	struct input_device *device)
{
	/* Handles the device availability. */
	if (device != NULL)
		refcount_get(&device->refs);
}

/* Releases the event number only when the complete generation is gone. */
static void
input_device_release(
	struct input_device *device)
{
	unsigned long irq;

	/* Checks the refcount put result. */
	if (device == NULL || !refcount_put(&device->refs))
		return;

	irq = spin_lock_irqsave(&registry_lock);

	/* Handles the device condition. */
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
/* End consolidated input-device.c. */

/* Begin consolidated input-keymap.c. */
/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-keymap.h"

#include <string.h>
#include <zedbsd/input.h>

struct symbol_entry {
	const char *name;
	uint16_t evdev;
	uint16_t legacy;
	char normal;
	char shifted;
};

static const uint16_t letter_codes[26] = {
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
	KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
	KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
};

static const uint16_t digit_codes[10] = {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
};

static const char shifted_digits[10] = {
	')', '!', '@', '#', '$', '%', '^', '&', '*', '(',
};

static const struct symbol_entry symbols[] = {
	{"esc", KEY_ESC, INPUT_KEY_ESCAPE, 0, 0},
	{"backspace", KEY_BACKSPACE, INPUT_KEY_BACKSPACE, 0, 0},
	{"tab", KEY_TAB, INPUT_KEY_TAB, 0, 0},
	{"enter", KEY_ENTER, INPUT_KEY_ENTER, 0, 0},
	{"space", KEY_SPACE, 0, ' ', ' '},
	{"minus", KEY_MINUS, 0, '-', '_'},
	{"equal", KEY_EQUAL, 0, '=', '+'},
	{"leftbrace", KEY_LEFTBRACE, 0, '[', '{'},
	{"rightbrace", KEY_RIGHTBRACE, 0, ']', '}'},
	{"semicolon", KEY_SEMICOLON, 0, ';', ':'},
	{"apostrophe", KEY_APOSTROPHE, 0, '\'', '"'},
	{"grave", KEY_GRAVE, 0, '`', '~'},
	{"backslash", KEY_BACKSLASH, 0, '\\', '|'},
	{"comma", KEY_COMMA, 0, ',', '<'},
	{"dot", KEY_DOT, 0, '.', '>'},
	{"slash", KEY_SLASH, 0, '/', '?'},
	{"jis-1", KEY_1, 0, '1', '!'},
	{"jis-2", KEY_2, 0, '2', '"'},
	{"jis-3", KEY_3, 0, '3', '#'},
	{"jis-4", KEY_4, 0, '4', '$'},
	{"jis-5", KEY_5, 0, '5', '%'},
	{"jis-6", KEY_6, 0, '6', '&'},
	{"jis-7", KEY_7, 0, '7', '\''},
	{"jis-8", KEY_8, 0, '8', '('},
	{"jis-9", KEY_9, 0, '9', ')'},
	{"jis-0", KEY_0, 0, '0', '0'},
	{"jis-minus", KEY_MINUS, 0, '-', '='},
	{"jis-caret", KEY_EQUAL, 0, '^', '~'},
	{"jis-yen", KEY_RESERVED, 0, '\\', '|'},
	{"jis-at", KEY_LEFTBRACE, 0, '@', '`'},
	{"jis-lbrace", KEY_RIGHTBRACE, 0, '[', '{'},
	{"jis-semi", KEY_SEMICOLON, 0, ';', '+'},
	{"jis-colon", KEY_APOSTROPHE, 0, ':', '*'},
	{"jis-rbrace", KEY_BACKSLASH, 0, ']', '}'},
	{"jis-comma", KEY_COMMA, 0, ',', '<'},
	{"jis-dot", KEY_DOT, 0, '.', '>'},
	{"jis-slash", KEY_SLASH, 0, '/', '?'},
	{"jis-ro", KEY_RESERVED, 0, '\\', '_'},
	{"jis-kp-slash", KEY_RESERVED, 0, '/', '/'},
	{"jis-kp-star", KEY_RESERVED, 0, '*', '*'},
	{"jis-kp-minus", KEY_RESERVED, 0, '-', '-'},
	{"jis-kp-7", KEY_RESERVED, 0, '7', '7'},
	{"jis-kp-8", KEY_RESERVED, 0, '8', '8'},
	{"jis-kp-9", KEY_RESERVED, 0, '9', '9'},
	{"jis-kp-plus", KEY_RESERVED, 0, '+', '+'},
	{"jis-kp-4", KEY_RESERVED, 0, '4', '4'},
	{"jis-kp-5", KEY_RESERVED, 0, '5', '5'},
	{"jis-kp-6", KEY_RESERVED, 0, '6', '6'},
	{"jis-kp-equal", KEY_RESERVED, 0, '=', '='},
	{"jis-kp-1", KEY_RESERVED, 0, '1', '1'},
	{"jis-kp-2", KEY_RESERVED, 0, '2', '2'},
	{"jis-kp-3", KEY_RESERVED, 0, '3', '3'},
	{"jis-kp-enter", KEY_RESERVED, INPUT_KEY_ENTER, 0, 0},
	{"jis-kp-0", KEY_RESERVED, 0, '0', '0'},
	{"jis-kp-comma", KEY_RESERVED, 0, ',', ','},
	{"jis-kp-dot", KEY_RESERVED, 0, '.', '.'},
	{"leftshift", KEY_LEFTSHIFT, INPUT_KEY_SHIFT_SYMBOL, 0, 0},
	{"rightshift", KEY_RIGHTSHIFT, INPUT_KEY_SHIFT_SYMBOL, 0, 0},
	{"leftctrl", KEY_LEFTCTRL, INPUT_KEY_CTRL_SYMBOL, 0, 0},
	{"rightctrl", KEY_RIGHTCTRL, INPUT_KEY_CTRL_SYMBOL, 0, 0},
	{"leftalt", KEY_LEFTALT, INPUT_KEY_GRAPH_SYMBOL, 0, 0},
	{"rightalt", KEY_RIGHTALT, INPUT_KEY_GRAPH_SYMBOL, 0, 0},
	{"capslock", KEY_CAPSLOCK, INPUT_KEY_CAPS_LOCK, 0, 0},
	{"kana", KEY_RESERVED, INPUT_KEY_KANA, 0, 0},
	{"home", KEY_HOME, INPUT_KEY_HOME, 0, 0},
	{"up", KEY_UP, INPUT_KEY_UP, 0, 0},
	{"pageup", KEY_PAGEUP, INPUT_KEY_PAGE_UP, 0, 0},
	{"left", KEY_LEFT, INPUT_KEY_LEFT, 0, 0},
	{"right", KEY_RIGHT, INPUT_KEY_RIGHT, 0, 0},
	{"end", KEY_END, INPUT_KEY_END, 0, 0},
	{"down", KEY_DOWN, INPUT_KEY_DOWN, 0, 0},
	{"pagedown", KEY_PAGEDOWN, INPUT_KEY_PAGE_DOWN, 0, 0},
	{"insert", KEY_INSERT, INPUT_KEY_INSERT, 0, 0},
	{"delete", KEY_DELETE, INPUT_KEY_DELETE, 0, 0},
};

static const struct symbol_entry *find_symbol(const char *name);

/* Supports the find symbol operation. */
static const struct symbol_entry *
find_symbol(
	const char *name)
{
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(symbols) / sizeof(symbols[0]); index++) {
		/* Selects the matching value. */
		if (strcmp(name, symbols[index].name) == 0)
			return &symbols[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

static int function_number(const char *symbol);

/* Supports the function number operation. */
static int
function_number(
	const char *symbol)
{
	/* Handles the symbol condition. */
	if (symbol[0] != 'f')
		return 0;

	/* Handles the symbol condition. */
	if (symbol[1] >= '1' && symbol[1] <= '9' && symbol[2] == '\0')
		return symbol[1] - '0';

	/* Selects the matching value. */
	if (strcmp(symbol, "f10") == 0)
		return 10;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input keymap init operation.
 */
void
drv_input_keymap_init(
	struct input_keymap_state *state)
{
	memset(state, 0, sizeof(*state));
}

/*
 * Implements the drv input key from symbol operation.
 */
uint16_t
drv_input_key_from_symbol(
	const char *symbol)
{
	unsigned char original;
	char value;
	unsigned index_for;
	const struct symbol_entry *entry;
	int number;

	/* Handles the symbol availability. */
	if (symbol == NULL || symbol[0] == '\0')
		return KEY_RESERVED;

	/* Handles the symbol condition. */
	if (symbol[1] == '\0') {
		original = (unsigned char)symbol[0];
		value = symbol[0];
		/* Dispatch the selected operation case. */
		switch (original) {
		case 0x08:
			/* Returns the computed result. */
			return KEY_BACKSPACE;
		case 0x09:
			/* Returns the computed result. */
			return KEY_TAB;
		case 0x0a:
		case 0x0d:
			/* Returns the computed result. */
			return KEY_ENTER;
		case 0x1b:
			/* Returns the computed result. */
			return KEY_ESC;
		default:
			break;
		}

		/* Validates the current value. */
		if (value >= 'A' && value <= 'Z')
			value = (char)(value - 'A' + 'a');

		/* Validates the current value. */
		if (value >= 'a' && value <= 'z')
			return letter_codes[value - 'a'];

		/* Validates the current value. */
		if (value >= '0' && value <= '9')
			return digit_codes[value - '0'];
		/* Process each remaining element. */
		for (index_for = 0;
		     index_for <
		     sizeof(shifted_digits) / sizeof(shifted_digits[0]);
		     index_for++) {
			/* Validates the current value. */
			if (value == shifted_digits[index_for])
				return digit_codes[index_for];
		}
		/* Dispatch the selected operation case. */
		switch (value) {
		case ' ':
			/* Returns the computed result. */
			return KEY_SPACE;
		case '-':
		case '_':
			/* Returns the computed result. */
			return KEY_MINUS;
		case '=':
		case '+':
			/* Returns the computed result. */
			return KEY_EQUAL;
		case '[':
		case '{':
			/* Returns the computed result. */
			return KEY_LEFTBRACE;
		case ']':
		case '}':
			/* Returns the computed result. */
			return KEY_RIGHTBRACE;
		case ';':
		case ':':
			/* Returns the computed result. */
			return KEY_SEMICOLON;
		case '\'':
		case '"':
			/* Returns the computed result. */
			return KEY_APOSTROPHE;
		case '`':
		case '~':
			/* Returns the computed result. */
			return KEY_GRAVE;
		case '\\':
		case '|':
			/* Returns the computed result. */
			return KEY_BACKSLASH;
		case ',':
		case '<':
			/* Returns the computed result. */
			return KEY_COMMA;
		case '.':
		case '>':
			/* Returns the computed result. */
			return KEY_DOT;
		case '/':
		case '?':
			/* Returns the computed result. */
			return KEY_SLASH;
		default:
			break;
		}
	}
	number = function_number(symbol);

	/* Handles the number condition. */
	if (number != 0)
		return (uint16_t)(KEY_F1 + number - 1);
	entry = find_symbol(symbol);

	/* Returns the computed result. */
	return entry != NULL ? entry->evdev : KEY_RESERVED;
}

/*
 * Implements the drv input key symbol supported operation.
 */
int
drv_input_key_symbol_supported(
	const char *symbol)
{
	int function_result;

	/* Handles the symbol availability. */
	if (symbol == NULL || symbol[0] == '\0')
		return 0;

	/* Handles the symbol condition. */
	if (symbol[1] == '\0')
		return 1;

	/* Computes the function result. */
	function_result =
		function_number(symbol) != 0 || find_symbol(symbol) != NULL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv input keymap event from code operation.
 */
int
drv_input_keymap_event_from_code(
	uint16_t code,
	int32_t value,
	struct hal_key_event *event)
{
	static const char *const functions[] = {"f1", "f2", "f3", "f4", "f5",
						"f6", "f7", "f8", "f9", "f10"};
	const struct symbol_entry *entry = NULL;
	const char *symbol = NULL;
	char character[2];
	unsigned index;

	/* Handles the event availability. */
	if (event == NULL || (value != 0 && value != 1 && value != 2))
		return 0;
	memset(event, 0, sizeof(*event));
	/* Process each remaining element. */
	for (index = 0; index < sizeof(letter_codes) / sizeof(letter_codes[0]);
	     index++) {
		/* Handles the letter codes condition. */
		if (letter_codes[index] == code) {
			character[0] = (char)('a' + index);
			character[1] = '\0';
			symbol = character;
			break;
		}
	}

	/* Handles the symbol availability. */
	if (symbol == NULL) {
		/* Process each remaining element. */
		for (index = 0;
		     index < sizeof(digit_codes) / sizeof(digit_codes[0]);
		     index++) {
			/* Handles the digit codes condition. */
			if (digit_codes[index] == code) {
				character[0] = (char)('0' + index);
				character[1] = '\0';
				symbol = character;
				break;
			}
		}
	}

	/* Handles the symbol availability. */
	if (symbol == NULL && code >= KEY_F1 && code <= KEY_F10) {
		symbol = functions[code - KEY_F1];
	}

	/* Handles the symbol availability. */
	if (symbol == NULL) {
		/* Process each remaining element. */
		for (index = 0; index < sizeof(symbols) / sizeof(symbols[0]);
		     index++) {
			/* Handles the symbols condition. */
			if (symbols[index].evdev == code) {
				entry = &symbols[index];
				break;
			}
		}
		symbol = entry != NULL ? entry->name : NULL;
	}

	/* Handles the symbol availability. */
	if (symbol == NULL)
		return 0;
	/* Process each remaining element. */
	for (index = 0;
	     index + 1U < HAL_KEY_SYMBOL_SIZE && symbol[index] != '\0'; index++)
		event->symbol[index] = symbol[index];
	event->flags = value == 0   ? HAL_KEY_EVENT_RELEASE
		       : value == 2 ? HAL_KEY_EVENT_REPEAT
				    : HAL_KEY_EVENT_PRESS;

	/* Reports operation failure. */
	return 1;
}

static void update_modifier(struct input_keymap_state *state, const char *symbol, int down, int press);

/* Supports the update modifier operation. */
static void
update_modifier(
	struct input_keymap_state *state,
	const char *symbol,
	int down,
	int press)
{
	/* Selects the matching value. */
	if (strcmp(symbol, "leftshift") == 0)
		state->left_shift = (uint8_t)down;
	else if (strcmp(symbol, "rightshift") == 0)
		state->right_shift = (uint8_t)down;
	else if (strcmp(symbol, "leftctrl") == 0)
		state->left_control = (uint8_t)down;
	else if (strcmp(symbol, "rightctrl") == 0)
		state->right_control = (uint8_t)down;
	else if (strcmp(symbol, "leftalt") == 0)
		state->left_graph = (uint8_t)down;
	else if (strcmp(symbol, "rightalt") == 0)
		state->right_graph = (uint8_t)down;
	else if (strcmp(symbol, "capslock") == 0 && press)
		state->caps_lock ^= 1U;
	else if (strcmp(symbol, "kana") == 0 && press)
		state->kana_lock ^= 1U;
}

/*
 * Implements the drv input keymap translate operation.
 */
int
drv_input_keymap_translate(
	struct input_keymap_state *state,
	const struct hal_key_event *event,
	uint32_t *result)
{
	const struct symbol_entry *entry;
	uint32_t key = 0, modifiers;
	int number, release, press, shift, control, graph;
	char value;

	/* Handles the state availability. */
	if (state == NULL || event == NULL || result == NULL ||
	    event->symbol[HAL_KEY_SYMBOL_SIZE - 1U] != '\0')

		/* Reports successful completion. */
		return 0;
	release = (event->flags & HAL_KEY_EVENT_RELEASE) != 0;
	press = (event->flags & HAL_KEY_EVENT_PRESS) != 0;

	/* Handles the event condition. */
	if (event->flags != HAL_KEY_EVENT_PRESS &&
	    event->flags != HAL_KEY_EVENT_RELEASE &&
	    event->flags != HAL_KEY_EVENT_REPEAT)

		/* Reports successful completion. */
		return 0;
	update_modifier(state, event->symbol, !release, press);
	shift = state->left_shift || state->right_shift;
	control = state->left_control || state->right_control;
	graph = state->left_graph || state->right_graph;

	/* Handles the event condition. */
	if (event->symbol[0] != '\0' && event->symbol[1] == '\0') {
		value = event->symbol[0];

		/* Validates the current value. */
		if (value >= 'a' && value <= 'z' && (shift ^ state->caps_lock))
			value = (char)(value - 'a' + 'A');
		else if (value >= '0' && value <= '9' && shift)
			value = shifted_digits[value - '0'];
		key = (uint8_t)value;
	} else if ((number = function_number(event->symbol)) != 0) {
		key = INPUT_KEY_F1 + (uint32_t)number - 1U;
	} else if ((entry = find_symbol(event->symbol)) != NULL) {
		key = entry->legacy != 0
			      ? entry->legacy
			      : (uint8_t)(shift && entry->shifted != 0
						  ? entry->shifted
						  : entry->normal);
	}

	/* Handles the selected key. */
	if (key == 0)
		return 0;
	modifiers = (shift ? INPUT_KEY_SHIFT : 0U) |
		    (control ? INPUT_KEY_CTRL : 0U) |
		    (graph ? INPUT_KEY_GRAPH : 0U);
	*result = (key & INPUT_KEY_MASK) | modifiers |
		  (release ? INPUT_KEY_RELEASE : 0U);

	/* Reports operation failure. */
	return 1;
}
/* End consolidated input-keymap.c. */

/* Begin consolidated input-queue.c. */
/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-queue.h"

#include <string.h>

/*
 * Implements the drv input queue init operation.
 */
void
drv_input_queue_init(
	struct input_queue *queue)
{
	memset(queue, 0, sizeof(*queue));
}

/*
 * Implements the drv input queue reader init operation.
 */
void
drv_input_queue_reader_init(
	const struct input_queue *queue,
	struct input_queue_reader *reader)
{
	reader->sequence = queue->next_sequence;
}

/*
 * Implements the drv input queue push operation.
 */
void
drv_input_queue_push(
	struct input_queue *queue,
	const struct input_event *event)
{
	queue->events[queue->next_sequence % INPUT_QUEUE_CAPACITY] = *event;
	queue->next_sequence++;

	/* Handles the queue condition. */
	if (queue->next_sequence - queue->first_sequence >
	    INPUT_QUEUE_CAPACITY) {
		queue->first_sequence =
			queue->next_sequence - INPUT_QUEUE_CAPACITY;
	}
}

/*
 * Implements the drv input queue read operation.
 */
size_t
drv_input_queue_read(
	const struct input_queue *queue,
	struct input_queue_reader *reader,
	struct input_event *events,
	size_t capacity)
{
	size_t count = 0;

	/* Handles the capacity condition. */
	if (capacity == 0)
		return 0;

	/* Handles the reader condition. */
	if (reader->sequence < queue->first_sequence) {
		memset(&events[count], 0, sizeof(events[count]));
		events[count].type = EV_SYN;
		events[count].code = SYN_DROPPED;
		count++;
		reader->sequence = queue->first_sequence;
	}
	while (count < capacity && reader->sequence < queue->next_sequence) {
		events[count++] =
			queue->events[reader->sequence % INPUT_QUEUE_CAPACITY];
		reader->sequence++;
	}

	/* Returns the computed result. */
	return count;
}

/*
 * Implements the drv input queue readable operation.
 */
int
drv_input_queue_readable(
	const struct input_queue *queue,
	const struct input_queue_reader *reader)
{
	/* Returns the computed result. */
	return reader->sequence < queue->first_sequence ||
	       reader->sequence < queue->next_sequence;
}

/*
 * Implements the drv input queue detach operation.
 */
void
drv_input_queue_detach(
	struct input_queue *queue)
{
	queue->detached = 1;
}
/* End consolidated input-queue.c. */

/* Begin consolidated input-subscriber.c. */
/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/input-device.h"
#include "kern/lock.h"

#include <errno.h>

#define INPUT_SUBSCRIBER_MAX 8U

static struct spinlock subscriber_lock;
static struct input_subscription *subscribers[INPUT_SUBSCRIBER_MAX];

/*
 * Implements the drv input subscriber init operation.
 */
void
drv_input_subscriber_init(
	void)
{
	unsigned index;

	spin_init(&subscriber_lock, LOCK_RANK_DEVICE, "input subscribers");
	/* Process each remaining element. */
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++)
		subscribers[index] = NULL;
}

/*
 * Implements the drv input subscribe operation.
 */
int
drv_input_subscribe(
	struct input_subscription *subscription,
	input_subscriber_callback_t callback,
	void *context)
{
	unsigned long irq;
	unsigned index;

	/* Handles the subscription availability. */
	if (subscription == NULL || callback == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&subscriber_lock);
	/* Process each remaining element. */
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++) {
		/* Handles the subscribers condition. */
		if (subscribers[index] == subscription) {
			spin_unlock_irqrestore(&subscriber_lock, irq);

			/* Returns the computed result. */
			return EBUSY;
		}
	}
	/* Process each remaining element. */
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++) {
		/* Handles the subscribers condition. */
		if (subscribers[index] == NULL)
			break;
	}

	/* Checks the current index. */
	if (index == INPUT_SUBSCRIBER_MAX) {
		spin_unlock_irqrestore(&subscriber_lock, irq);

		/* Returns the computed result. */
		return ENOSPC;
	}
	subscription->callback = callback;
	subscription->context = context;
	subscription->registered = 1;
	subscribers[index] = subscription;
	spin_unlock_irqrestore(&subscriber_lock, irq);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv input unsubscribe operation.
 */
void
drv_input_unsubscribe(
	struct input_subscription *subscription)
{
	unsigned long irq;
	unsigned index;

	/* Handles the subscription availability. */
	if (subscription == NULL)
		return;
	irq = spin_lock_irqsave(&subscriber_lock);
	/* Process each remaining element. */
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++) {
		/* Handles the subscribers condition. */
		if (subscribers[index] == subscription) {
			subscribers[index] = NULL;
			break;
		}
	}
	subscription->registered = 0;
	subscription->callback = NULL;
	subscription->context = NULL;
	spin_unlock_irqrestore(&subscriber_lock, irq);
}

/*
 * The callback must be bounded and nonblocking.  Publication never holds an input-device state lock.  Keeping this registry lock across the callback makes drv_input_unsubscribe() a join point for every already-admitted call.
 */
void
drv_input_subscriber_publish(
	const struct input_report *report)
{
	struct input_subscription *subscription;
	unsigned long irq;
	unsigned index;

	/* Handles the report availability. */
	if (report == NULL)
		return;
	irq = spin_lock_irqsave(&subscriber_lock);
	/* Process each remaining element. */
	for (index = 0; index < INPUT_SUBSCRIBER_MAX; index++) {
		subscription = subscribers[index];

		/* Handles the subscription availability. */
		if (subscription != NULL && subscription->registered)
			subscription->callback(subscription->context, report);
	}
	spin_unlock_irqrestore(&subscriber_lock, irq);
}
/* End consolidated input-subscriber.c. */
