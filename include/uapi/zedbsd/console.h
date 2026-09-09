/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/console
 */

#ifndef ZEDBSD_UAPI_CONSOLE_H
#define ZEDBSD_UAPI_CONSOLE_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <zedbsd/types.h>

#define ZEDBSD_CONSOLE_IOC_GROUP	'c'

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

#define ZEDBSD_CONSOLE_GET_SIZE	\
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 1, struct console_size)
#define ZEDBSD_CONSOLE_CLEAR	_IO(ZEDBSD_CONSOLE_IOC_GROUP, 2)
#define ZEDBSD_CONSOLE_CLEAR_ROW	\
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 3, struct console_row)
#define ZEDBSD_CONSOLE_CLEAR_TO_EOL	\
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 4, struct console_position)
#define ZEDBSD_CONSOLE_GET_CURSOR	\
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 5, struct console_cursor)
#define ZEDBSD_CONSOLE_SET_CURSOR	\
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 6, struct console_cursor)
#define ZEDBSD_CONSOLE_SHOW_CURSOR	\
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 7, struct console_cursor)
#define ZEDBSD_CONSOLE_WRITE_AT	\
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 8, struct console_write_at)
/* Removed input-event request numbers 9--12 remain reserved. */
/*
 * Terminal-capability probe used by isatty().
 */
#define ZEDBSD_CONSOLE_ISATTY	_IO(ZEDBSD_CONSOLE_IOC_GROUP, 13)
/* Removed input-mode request numbers 14--15 remain reserved. */

#endif
