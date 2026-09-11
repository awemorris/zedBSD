/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/console
 */

#ifndef KERN_UAPI_CONSOLE_H
#define KERN_UAPI_CONSOLE_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <zedbsd/types.h>

#define KERN_CONSOLE_IOC_GROUP	'c'

struct console_size {
	uint32_t rows;
	uint32_t columns;
};

struct console_cursor {
	uint32_t row;
	uint32_t column;
	uint32_t visible;
};

struct console_row {
	uint32_t row;
};

struct console_position {
	uint32_t row;
	uint32_t column;
};

struct console_write_at {
	uint32_t row;
	uint32_t column;
	uint32_t attribute;
	uapi_ptr_t address;
	uint32_t length;
};

#define KERN_CONSOLE_GET_SIZE		_IOR(KERN_CONSOLE_IOC_GROUP, 1, struct console_size)
#define KERN_CONSOLE_CLEAR		_IO(KERN_CONSOLE_IOC_GROUP, 2)
#define KERN_CONSOLE_CLEAR_ROW		_IOW(KERN_CONSOLE_IOC_GROUP, 3, struct console_row)
#define KERN_CONSOLE_CLEAR_TO_EOL	_IOW(KERN_CONSOLE_IOC_GROUP, 4, struct console_position)
#define KERN_CONSOLE_GET_CURSOR		_IOR(KERN_CONSOLE_IOC_GROUP, 5, struct console_cursor)
#define KERN_CONSOLE_SET_CURSOR		_IOW(KERN_CONSOLE_IOC_GROUP, 6, struct console_cursor)
#define KERN_CONSOLE_SHOW_CURSOR	_IOW(KERN_CONSOLE_IOC_GROUP, 7, struct console_cursor)
#define KERN_CONSOLE_WRITE_AT		_IOW(KERN_CONSOLE_IOC_GROUP, 8, struct console_write_at)
#define KERN_CONSOLE_ISATTY		_IO(KERN_CONSOLE_IOC_GROUP, 13)

#endif
