/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/console-device.h"
#include "kern/cdev.h"
#include "kern/uaccess.h"

#include <zedbsd/console.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define CONSOLE_WRITE_MAX 512U

static ssize_t console_read(struct file *file, void *buffer, size_t size)
{
	uint8_t *bytes = buffer;
	int event;
	(void)file;
	if (size == 0)
		return 0;
	for (;;) {
		event = hal_cons_read_event();
		event &= HAL_KEY_EVENT_KEY_MASK;
		if (event > 0 && event <= 0xff)
			break;
	}
	bytes[0] = (uint8_t)event;
	return 1;
}

static ssize_t console_write(struct file *file, const void *buffer, size_t size)
{
	(void)file;
	if (buffer == NULL)
		return -EINVAL;
	if (size > UINT32_MAX)
		return -EOVERFLOW;
	hal_cons_write_n(buffer, (unsigned)size);
	hal_cons_update_cursor();
	return (ssize_t)size;
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
			hal_cons_poll_event() : hal_cons_read_event();
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
		hal_cons_drain_input();
		return 0;
	case ZEDBSD_CONSOLE_ISATTY:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static const struct cdev_ops console_ops = {
	.read = console_read,
	.write = console_write,
	.ioctl = console_ioctl,
};

int console_device_register(void)
{
	int error = cdev_register("console", 0x00010000U, &console_ops, NULL);

	if (error == 0)
		hal_cons_set_mode(HAL_CONS_TERMINAL);
	return error;
}
