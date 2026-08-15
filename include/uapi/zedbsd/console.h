/*
 * /dev/console
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_CONSOLE_H
#define ZEDBSD_UAPI_CONSOLE_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <zedbsd/types.h>

#define ZEDBSD_CONSOLE_IOC_GROUP 'c'

/* Encoding returned by ZEDBSD_CONSOLE_{POLL,READ}_EVENT. */
#define ZEDBSD_CONSOLE_EVENT_KEY_MASK 0x000001ffU
#define ZEDBSD_CONSOLE_EVENT_SHIFT    0x00010000U
#define ZEDBSD_CONSOLE_EVENT_CTRL     0x00020000U
#define ZEDBSD_CONSOLE_EVENT_GRAPH    0x00040000U

struct zedbsd_console_size {
	uint32_t rows;
	uint32_t columns;
};

struct zedbsd_console_cursor {
	uint32_t row;
	uint32_t column;
	uint32_t visible;
};

struct zedbsd_console_row {
	uint32_t row;
};

struct zedbsd_console_position {
	uint32_t row;
	uint32_t column;
};

struct zedbsd_console_write_at {
	uint32_t row;
	uint32_t column;
	uint32_t attribute;
	uapi_ptr_t address;
	uint32_t length;
};

struct zedbsd_console_event {
	uint32_t value;
};

struct zedbsd_console_key_state {
	uint32_t key;
	int32_t down;
};

#define ZEDBSD_CONSOLE_GET_SIZE \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 1, struct zedbsd_console_size)
#define ZEDBSD_CONSOLE_CLEAR _IO(ZEDBSD_CONSOLE_IOC_GROUP, 2)
#define ZEDBSD_CONSOLE_CLEAR_ROW \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 3, struct zedbsd_console_row)
#define ZEDBSD_CONSOLE_CLEAR_TO_EOL \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 4, struct zedbsd_console_position)
#define ZEDBSD_CONSOLE_GET_CURSOR \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 5, struct zedbsd_console_cursor)
#define ZEDBSD_CONSOLE_SET_CURSOR \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 6, struct zedbsd_console_cursor)
#define ZEDBSD_CONSOLE_SHOW_CURSOR \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 7, struct zedbsd_console_cursor)
#define ZEDBSD_CONSOLE_WRITE_AT \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 8, struct zedbsd_console_write_at)
#define ZEDBSD_CONSOLE_POLL_EVENT \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 9, struct zedbsd_console_event)
#define ZEDBSD_CONSOLE_READ_EVENT \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 10, struct zedbsd_console_event)
#define ZEDBSD_CONSOLE_KEY_STATE \
	_IOWR(ZEDBSD_CONSOLE_IOC_GROUP, 11, struct zedbsd_console_key_state)
#define ZEDBSD_CONSOLE_DRAIN_INPUT _IO(ZEDBSD_CONSOLE_IOC_GROUP, 12)
/* Terminal-capability probe used by isatty(). */
#define ZEDBSD_CONSOLE_ISATTY _IO(ZEDBSD_CONSOLE_IOC_GROUP, 13)

#endif
