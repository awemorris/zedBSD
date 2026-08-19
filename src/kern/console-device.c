/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/console-device.h"
#include "kern/cdev.h"
#include "kern/file.h"
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

static int
console_input_take(int consume, int wait)
{
	unsigned long irq;
	int result;

	if (!input_started)
		return wait ? hal_cons_read_event() : hal_cons_poll_event();
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

int console_input_poll_event(void)
{ return console_input_take(0, 0); }
int console_input_read_event(void)
{ return console_input_take(1, 1); }

static void
console_input_worker(void *argument)
{
	(void)argument;
	for (;;) {
		int notify = 0;
		for (;;) {
			int event;
			unsigned long irq = spin_lock_irqsave(&input_lock);
			if (hal_cons_poll_event() < 0) {
				spin_unlock_irqrestore(&input_lock, irq);
				break;
			}
			event = hal_cons_read_event();
			if (event < 0)
				{
					spin_unlock_irqrestore(&input_lock, irq);
					break;
				}
			if (input_used == CONSOLE_INPUT_EVENTS) {
				input_tail = (input_tail + 1U) % CONSOLE_INPUT_EVENTS;
				input_used--;
			}
			input_events[input_head] = (uint32_t)event;
			input_head = (input_head + 1U) % CONSOLE_INPUT_EVENTS;
			input_used++;
			notify = 1;
			waitq_wake_all(&input_waitq);
			spin_unlock_irqrestore(&input_lock, irq);
			tty_console_input_event((uint32_t)event);
		}
		if (notify)
			poll_notify();
		sched_sleep(sched_ticks() + 1U);
	}
}

static ssize_t console_read(struct file *file, void *buffer, size_t size)
{
	unsigned vt = file->f_data != NULL ?
	    (unsigned)((uintptr_t)file->f_data - 1U) : 0U;
	return tty_vt_read(vt, file, buffer, size);
}

static ssize_t console_write(struct file *file, const void *buffer, size_t size)
{
	unsigned vt = file->f_data != NULL ?
	    (unsigned)((uintptr_t)file->f_data - 1U) : 0U;
	ssize_t result = tty_vt_write(vt, file, buffer, size);
	if (result < 0)
		return result;
	hal_cons_update_cursor();
	return result;
}

static int console_write_at(uintptr_t argument)
{
	struct zedbsd_console_write_at request;
	char text[CONSOLE_WRITE_MAX + 1U];
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	if (request.row >= HAL_CONS_ROWS || request.column >= HAL_CONS_COLUMNS ||
	    request.length > CONSOLE_WRITE_MAX)
		return EINVAL;
	error = copyin(request.address, text, request.length);
	if (error != 0)
		return error;
	text[request.length] = '\0';
	return hal_cons_write_n_at(request.row, request.column, text,
		request.length, (uint8_t)request.attribute) < 0 ? EIO : 0;
}

static int console_ioctl(struct file *file, unsigned long request,
			 uintptr_t argument)
{
	struct hal_cons_state state;
	int error;
	(void)file;
	switch (request) {
	case ZEDBSD_CONSOLE_GET_SIZE: {
		const struct zedbsd_console_size size = {
			HAL_CONS_ROWS, HAL_CONS_COLUMNS
		};
		return copyout(&size, argument, sizeof(size));
	}
	case ZEDBSD_CONSOLE_CLEAR:
		hal_cons_clear();
		return 0;
	case ZEDBSD_CONSOLE_CLEAR_ROW: {
		struct zedbsd_console_row row;
		error = copyin(argument, &row, sizeof(row));
		if (error != 0) return error;
		if (row.row >= HAL_CONS_ROWS) return EINVAL;
		hal_cons_clear_row(row.row);
		return 0;
	}
	case ZEDBSD_CONSOLE_CLEAR_TO_EOL: {
		struct zedbsd_console_position position;
		error = copyin(argument, &position, sizeof(position));
		if (error != 0) return error;
		return hal_cons_clear_to_eol_at(position.row, position.column) ? 0 : EINVAL;
	}
	case ZEDBSD_CONSOLE_GET_CURSOR: {
		struct zedbsd_console_cursor cursor;
		hal_cons_save_state(&state);
		cursor.row = state.row;
		cursor.column = state.column;
		cursor.visible = state.cursor_visible != 0;
		return copyout(&cursor, argument, sizeof(cursor));
	}
	case ZEDBSD_CONSOLE_SET_CURSOR: {
		struct zedbsd_console_cursor cursor;
		error = copyin(argument, &cursor, sizeof(cursor));
		if (error != 0) return error;
		return hal_cons_set_cursor(cursor.row, cursor.column) ? 0 : EINVAL;
	}
	case ZEDBSD_CONSOLE_SHOW_CURSOR: {
		struct zedbsd_console_cursor cursor;
		error = copyin(argument, &cursor, sizeof(cursor));
		if (error != 0) return error;
		hal_cons_show_cursor(cursor.visible != 0);
		return 0;
	}
	case ZEDBSD_CONSOLE_WRITE_AT:
		return console_write_at(argument);
	case ZEDBSD_CONSOLE_POLL_EVENT:
	case ZEDBSD_CONSOLE_READ_EVENT: {
		struct zedbsd_console_event event;
		int value = request == ZEDBSD_CONSOLE_POLL_EVENT ?
			console_input_take(0, 0) : console_input_take(1, 1);
		if (value == -EINTR) return EINTR;
		if (value < 0) return EAGAIN;
		event.value = (uint32_t)value;
		return copyout(&event, argument, sizeof(event));
	}
	case ZEDBSD_CONSOLE_KEY_STATE: {
		struct zedbsd_console_key_state key;
		error = copyin(argument, &key, sizeof(key));
		if (error != 0) return error;
		key.down = hal_cons_key_state((int)key.key);
		return copyout(&key, argument, sizeof(key));
	}
	case ZEDBSD_CONSOLE_DRAIN_INPUT:
		{
			unsigned long irq = spin_lock_irqsave(&input_lock);
			input_head = input_tail = input_used = 0;
			hal_cons_drain_input();
			spin_unlock_irqrestore(&input_lock, irq);
			poll_notify();
		}
		return 0;
	case ZEDBSD_CONSOLE_ISATTY:
		return 0;
	default:
		return tty_vt_ioctl(file->f_data != NULL ?
		    (unsigned)((uintptr_t)file->f_data - 1U) : 0U,
		    file, request, argument);
	}
}

static int
console_poll(struct file *file, short events, short *revents)
{
	unsigned vt = file->f_data != NULL ?
	    (unsigned)((uintptr_t)file->f_data - 1U) : 0U;
	return tty_vt_poll(vt, file, events, revents);
}

static int vt_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	unsigned vt = (unsigned)((uintptr_t)file->f_data - 1U);
	return tty_vt_ioctl(vt, file, request, argument);
}

static const struct cdev_ops vt_ops = {
	.read = console_read, .write = console_write,
	.ioctl = vt_ioctl, .poll = console_poll,
};

static const struct cdev_ops console_ops = {
	.read = console_read,
	.write = console_write,
	.ioctl = console_ioctl,
	.poll = console_poll,
};

int console_device_register(void)
{
	struct thread *worker;
	int error;
	error = tty_console_init();
	if (error != 0)
		return error;
	spin_init(&input_lock, LOCK_RANK_DEVICE, "console input");
	waitq_init(&input_waitq, "console input");
	input_head = input_tail = input_used = 0;
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
		if (error != 0) { input_started = 0; return error; }
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
