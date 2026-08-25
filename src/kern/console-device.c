/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/console-device.h"
#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/file.h"
#include "kern/input-device.h"
#include "kern/input-keymap.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/poll.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/tty.h"
#include "kern/uaccess.h"
#include "kern/waitq.h"

#include <zedbsd/console.h>
#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <string.h>

#define CONSOLE_WRITE_MAX 512U
#define CONSOLE_INPUT_EVENTS 64U

static uint32_t input_events[CONSOLE_INPUT_EVENTS];
static unsigned input_head, input_tail, input_used;
static unsigned input_started;
static struct spinlock input_lock;
static struct wait_queue input_waitq;
static struct input_device *keyboard_input;
static struct input_keymap_state console_keymap;

#define CONSOLE_EVENT_RECORDS 64U

struct console_open {
	unsigned vt;
	unsigned input_mode;
};

static struct console_open *event_owner;
static struct console_input_event event_records[CONSOLE_EVENT_RECORDS];
static unsigned event_head, event_tail, event_used, event_sequence;

static struct console_open *
console_open_state(struct file *file)
{
	return file != NULL ? file->f_data : NULL;
}

static unsigned
console_file_vt(struct file *file)
{
	struct console_open *state = console_open_state(file);
	return state != NULL ? state->vt : 0U;
}

static int
console_open_file(struct file *file)
{
	struct console_open *state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	state->vt = 0;
	state->input_mode = ZEDBSD_CONSOLE_INPUT_TEXT;
	file->f_data = state;
	return 0;
}

static int
console_close_file(struct file *file)
{
	struct console_open *state = console_open_state(file);
	unsigned long irq;
	if (state == NULL)
		return 0;
	irq = spin_lock_irqsave(&input_lock);
	if (event_owner == state) {
		event_owner = NULL;
		event_head = event_tail = event_used = 0;
		waitq_wake_all(&input_waitq);
	}
	spin_unlock_irqrestore(&input_lock, irq);
	kern_free(state);
	file->f_data = NULL;
	poll_notify();
	return 0;
}

static int
console_input_take(int consume, int wait)
{
	unsigned long irq;
	int result;

	if (!input_started) {
		struct hal_key_event event;
		struct input_keymap_state state = console_keymap;
		uint32_t translated;
		int available = wait ? hal_cons_read_event(&event) :
		    hal_cons_poll_event(&event);
		if (!available || !input_keymap_translate(&state, &event,
		    &translated))
			return -1;
		if (consume)
			console_keymap = state;
		return (int)translated;
	}
	irq = spin_lock_irqsave(&input_lock);
	while (input_used == 0) {
		uint64_t sequence;
		int error;
		if (!wait) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -1;
		}
		sequence = waitq_sequence(&input_waitq);
		error = waitq_sleep(&input_waitq, &input_lock, sequence, 0,
				    WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -EINTR;
		}
	}
	result = (int)input_events[input_tail];
	if (consume) {
		input_tail = (input_tail + 1U) % CONSOLE_INPUT_EVENTS;
		input_used--;
	}
	spin_unlock_irqrestore(&input_lock, irq);
	return result;
}

int
console_input_poll_event(void)
{
	return console_input_take(0, 0);
}
int
console_input_read_event(void)
{
	return console_input_take(1, 1);
}

static void
console_input_worker(void *argument)
{
	(void)argument;
	for (;;) {
		struct hal_key_event event;
		uint32_t translated;
		uint16_t key;
		int value;
		unsigned long irq;

		if (!hal_cons_read_event(&event))
			continue;
		key = input_key_from_symbol(event.symbol);
		value = (event.flags & HAL_KEY_EVENT_RELEASE) != 0 ? 0 :
		    (event.flags & HAL_KEY_EVENT_REPEAT) != 0 ? 2 : 1;
		if (key != KEY_RESERVED) {
			input_device_emit(keyboard_input, EV_KEY, key, value);
			input_device_emit(keyboard_input, EV_SYN, SYN_REPORT, 0);
		}
		if (!input_keymap_translate(&console_keymap, &event, &translated))
			continue;
		irq = spin_lock_irqsave(&input_lock);
		if (event_owner != NULL) {
			struct console_input_event *record;
			unsigned flags = 0;
			if (event_used == CONSOLE_EVENT_RECORDS) {
				event_tail =
				    (event_tail + 1U) % CONSOLE_EVENT_RECORDS;
				event_used--;
				flags = ZEDBSD_CONSOLE_INPUT_FLAG_OVERFLOW;
			}
			record = &event_records[event_head];
			memset(record, 0, sizeof(*record));
			record->timestamp_ns =
			    clock_milliseconds(NULL) * 1000000ULL;
			record->sequence = ++event_sequence;
			record->type = ZEDBSD_CONSOLE_INPUT_EVENT_KEY;
			record->flags = (uint16_t)flags;
			record->device_id = 0;
			record->key = translated & INPUT_KEY_MASK;
			record->modifiers = translated &
			    (INPUT_KEY_SHIFT | INPUT_KEY_CTRL | INPUT_KEY_GRAPH);
			record->state =
			    (translated & INPUT_KEY_RELEASE) != 0
				? ZEDBSD_CONSOLE_KEY_RELEASE
				: ZEDBSD_CONSOLE_KEY_PRESS;
			event_head = (event_head + 1U) % CONSOLE_EVENT_RECORDS;
			event_used++;
			waitq_wake_all(&input_waitq);
			spin_unlock_irqrestore(&input_lock, irq);
		} else if ((translated & INPUT_KEY_RELEASE) == 0) {
			if (input_used == CONSOLE_INPUT_EVENTS) {
				input_tail =
				    (input_tail + 1U) % CONSOLE_INPUT_EVENTS;
				input_used--;
			}
			input_events[input_head] = translated;
			input_head = (input_head + 1U) % CONSOLE_INPUT_EVENTS;
			input_used++;
			waitq_wake_all(&input_waitq);
			spin_unlock_irqrestore(&input_lock, irq);
			tty_console_input_event(translated);
		} else {
			spin_unlock_irqrestore(&input_lock, irq);
		}
		poll_notify();
	}
}

static ssize_t
console_event_read(struct file *file, void *buffer, size_t size)
{
	size_t capacity, count = 0;
	unsigned long irq;
	if (size < sizeof(struct console_input_event))
		return -EINVAL;
	capacity = size / sizeof(struct console_input_event);
	irq = spin_lock_irqsave(&input_lock);
	while (event_used == 0) {
		uint64_t sequence;
		int error;
		if ((file_status_flags_get(file) & O_NONBLOCK) != 0) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -EAGAIN;
		}
		sequence = waitq_sequence(&input_waitq);
		error = waitq_sleep(&input_waitq, &input_lock, sequence, 0,
				    WAITQ_INTERRUPTIBLE);
		if (error == EINTR) {
			spin_unlock_irqrestore(&input_lock, irq);
			return -EINTR;
		}
	}
	while (count < capacity && event_used != 0) {
		((struct console_input_event *)buffer)[count++] =
		    event_records[event_tail];
		event_tail = (event_tail + 1U) % CONSOLE_EVENT_RECORDS;
		event_used--;
	}
	spin_unlock_irqrestore(&input_lock, irq);
	return (ssize_t)(count * sizeof(struct console_input_event));
}

static ssize_t
console_read(struct file *file, void *buffer, size_t size)
{
	struct console_open *state = console_open_state(file);
	if (state != NULL && state->input_mode == ZEDBSD_CONSOLE_INPUT_EVENT)
		return console_event_read(file, buffer, size);
	return tty_vt_read(console_file_vt(file), file, buffer, size);
}

static ssize_t
console_write(struct file *file, const void *buffer, size_t size)
{
	ssize_t result =
	    tty_vt_write(console_file_vt(file), file, buffer, size);
	if (result < 0)
		return result;
	hal_cons_update_cursor();
	return result;
}

static int
console_write_at(uintptr_t argument)
{
	struct console_write_at request;
	char text[CONSOLE_WRITE_MAX + 1U];
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	if (request.row >= HAL_CONS_ROWS ||
	    request.column >= HAL_CONS_COLUMNS ||
	    request.length > CONSOLE_WRITE_MAX)
		return EINVAL;
	error = copyin(request.address, text, request.length);
	if (error != 0)
		return error;
	text[request.length] = '\0';
	return hal_cons_write_n_at(request.row, request.column, text,
				   request.length,
				   (uint8_t)request.attribute) < 0
		   ? EIO
		   : 0;
}

static int
console_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	struct hal_cons_state state;
	int error;
	switch (request) {
	case ZEDBSD_CONSOLE_GET_SIZE: {
		const struct console_size size = {HAL_CONS_ROWS,
						  HAL_CONS_COLUMNS};
		return copyout(&size, argument, sizeof(size));
	}
	case ZEDBSD_CONSOLE_CLEAR:
		hal_cons_clear();
		return 0;
	case ZEDBSD_CONSOLE_CLEAR_ROW: {
		struct console_row row;
		error = copyin(argument, &row, sizeof(row));
		if (error != 0)
			return error;
		if (row.row >= HAL_CONS_ROWS)
			return EINVAL;
		hal_cons_clear_row(row.row);
		return 0;
	}
	case ZEDBSD_CONSOLE_CLEAR_TO_EOL: {
		struct console_position position;
		error = copyin(argument, &position, sizeof(position));
		if (error != 0)
			return error;
		return hal_cons_clear_to_eol_at(position.row, position.column)
			   ? 0
			   : EINVAL;
	}
	case ZEDBSD_CONSOLE_GET_CURSOR: {
		struct console_cursor cursor;
		hal_cons_save_state(&state);
		cursor.row = state.row;
		cursor.column = state.column;
		cursor.visible = state.cursor_visible != 0;
		return copyout(&cursor, argument, sizeof(cursor));
	}
	case ZEDBSD_CONSOLE_SET_CURSOR: {
		struct console_cursor cursor;
		error = copyin(argument, &cursor, sizeof(cursor));
		if (error != 0)
			return error;
		return hal_cons_set_cursor(cursor.row, cursor.column) ? 0
								      : EINVAL;
	}
	case ZEDBSD_CONSOLE_SHOW_CURSOR: {
		struct console_cursor cursor;
		error = copyin(argument, &cursor, sizeof(cursor));
		if (error != 0)
			return error;
		hal_cons_show_cursor(cursor.visible != 0);
		return 0;
	}
	case ZEDBSD_CONSOLE_WRITE_AT:
		return console_write_at(argument);
	case ZEDBSD_CONSOLE_POLL_EVENT:
	case ZEDBSD_CONSOLE_READ_EVENT: {
		struct console_event event;
		int value = request == ZEDBSD_CONSOLE_POLL_EVENT
				? console_input_take(0, 0)
				: console_input_take(1, 1);
		if (value == -EINTR)
			return EINTR;
		if (value < 0)
			return EAGAIN;
		event.value = (uint32_t)value;
		return copyout(&event, argument, sizeof(event));
	}
	case ZEDBSD_CONSOLE_GET_INPUT_MODE: {
		struct console_open *open = console_open_state(file);
		struct console_input_mode mode;
		if (open == NULL)
			return ENODEV;
		mode.mode = open->input_mode;
		mode.flags = 0;
		return copyout(&mode, argument, sizeof(mode));
	}
	case ZEDBSD_CONSOLE_SET_INPUT_MODE: {
		struct console_open *open = console_open_state(file);
		struct console_input_mode mode;
		unsigned long irq;
		if (open == NULL)
			return ENODEV;
		error = copyin(argument, &mode, sizeof(mode));
		if (error != 0)
			return error;
		if ((mode.mode != ZEDBSD_CONSOLE_INPUT_TEXT &&
		     mode.mode != ZEDBSD_CONSOLE_INPUT_EVENT) ||
		    mode.flags != 0)
			return EINVAL;
		irq = spin_lock_irqsave(&input_lock);
		if (mode.mode == ZEDBSD_CONSOLE_INPUT_EVENT &&
		    event_owner != NULL && event_owner != open) {
			spin_unlock_irqrestore(&input_lock, irq);
			return EBUSY;
		}
		if (mode.mode == ZEDBSD_CONSOLE_INPUT_EVENT)
			event_owner = open;
		else if (event_owner == open)
			event_owner = NULL;
		open->input_mode = mode.mode;
		event_head = event_tail = event_used = 0;
		waitq_wake_all(&input_waitq);
		spin_unlock_irqrestore(&input_lock, irq);
		poll_notify();
		return 0;
	}
	case ZEDBSD_CONSOLE_KEY_STATE: {
		struct console_key_state key;
		error = copyin(argument, &key, sizeof(key));
		if (error != 0)
			return error;
		key.down = hal_cons_key_state((int)key.key);
		return copyout(&key, argument, sizeof(key));
	}
	case ZEDBSD_CONSOLE_DRAIN_INPUT: {
		unsigned long irq = spin_lock_irqsave(&input_lock);
		input_head = input_tail = input_used = 0;
		event_head = event_tail = event_used = 0;
		hal_cons_drain_input();
		spin_unlock_irqrestore(&input_lock, irq);
		poll_notify();
	}
		return 0;
	case ZEDBSD_CONSOLE_ISATTY:
		return 0;
	default:
		return tty_vt_ioctl(console_file_vt(file), file, request,
				    argument);
	}
}

static int
console_poll(struct file *file, short events, short *revents)
{
	struct console_open *state = console_open_state(file);
	if (state != NULL && state->input_mode == ZEDBSD_CONSOLE_INPUT_EVENT) {
		unsigned long irq;
		short result = events & (POLLOUT | POLLWRNORM);
		irq = spin_lock_irqsave(&input_lock);
		if (event_used != 0)
			result |= events & (POLLIN | POLLRDNORM);
		spin_unlock_irqrestore(&input_lock, irq);
		*revents = result;
		return 0;
	}
	return tty_vt_poll(console_file_vt(file), file, events, revents);
}

static ssize_t
vt_read(struct file *file, void *buffer, size_t size)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	return tty_vt_read(vt, file, buffer, size);
}

static ssize_t
vt_write(struct file *file, const void *buffer, size_t size)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	ssize_t result = tty_vt_write(vt, file, buffer, size);
	if (result >= 0)
		hal_cons_update_cursor();
	return result;
}

static int
vt_poll(struct file *file, short events, short *revents)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	return tty_vt_poll(vt, file, events, revents);
}

static int
vt_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	return tty_vt_ioctl(vt, file, request, argument);
}

static const struct cdev_ops vt_ops = {
    .read = vt_read,
    .write = vt_write,
    .ioctl = vt_ioctl,
    .poll = vt_poll,
};

static const struct cdev_ops console_ops = {
    .open = console_open_file,
    .close = console_close_file,
    .read = console_read,
    .write = console_write,
    .ioctl = console_ioctl,
    .poll = console_poll,
};

int
console_device_register(void)
{
	const struct input_device_info keyboard_info = {
	    .name = "zedBSD console keyboard",
	    .physical_path = "console/input0",
	    .id = {.bustype = BUS_HOST, .product = 1, .version = 1},
	};
	struct thread *worker;
	int error;
	error = input_device_register(&keyboard_info, &keyboard_input);
	if (error != 0)
		return error;
	error = tty_console_init();
	if (error != 0)
		return error;
	spin_init(&input_lock, LOCK_RANK_DEVICE, "console input");
	waitq_init(&input_waitq, "console input");
	input_head = input_tail = input_used = 0;
	event_head = event_tail = event_used = event_sequence = 0;
	event_owner = NULL;
	input_keymap_init(&console_keymap);
	input_started = 1;
	error = kthread_create(console_input_worker, NULL,
			       SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0) {
		input_started = 0;
		return error;
	}
	error = cdev_register("console", 0x00010000U, &console_ops,
			      (void *)(uintptr_t)1U);
	if (error != 0) {
		input_started = 0;
		return error;
	}
	for (unsigned i = 0; i < tty_vt_count(); i++) {
		char name[] = "ttyv0";
		name[4] = (char)('0' + i);
		error = cdev_register(name, (dev_t)(0x00010010U + i), &vt_ops,
				      (void *)(uintptr_t)(i + 1U));
		if (error != 0) {
			input_started = 0;
			return error;
		}
	}
	error = tty_pty_register();
	if (error != 0) {
		input_started = 0;
		return error;
	}
	thread_start(worker);
	hal_cons_set_mode(HAL_CONS_TERMINAL);
	return 0;
}
