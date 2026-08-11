/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef BOOTS_UAPI_CONSOLE_H
#define BOOTS_UAPI_CONSOLE_H

#include <stdint.h>
#include <sys/ioctl.h>

#define BOOTS_CONSOLE_IOC_GROUP 'c'

/* Encoding returned by BOOTS_CONSOLE_{POLL,READ}_EVENT. */
#define BOOTS_CONSOLE_EVENT_KEY_MASK 0x000001ffU
#define BOOTS_CONSOLE_EVENT_SHIFT    0x00010000U
#define BOOTS_CONSOLE_EVENT_CTRL     0x00020000U
#define BOOTS_CONSOLE_EVENT_GRAPH    0x00040000U

struct boots_console_size {
	uint32_t rows;
	uint32_t columns;
};

struct boots_console_cursor {
	uint32_t row;
	uint32_t column;
	uint32_t visible;
};

struct boots_console_row {
	uint32_t row;
};

struct boots_console_position {
	uint32_t row;
	uint32_t column;
};

struct boots_console_write_at {
	uint32_t row;
	uint32_t column;
	uint32_t attribute;
	uint32_t address;
	uint32_t length;
};

struct boots_console_event {
	uint32_t value;
};

struct boots_console_key_state {
	uint32_t key;
	int32_t down;
};

#define BOOTS_CONSOLE_GET_SIZE \
	_IOR(BOOTS_CONSOLE_IOC_GROUP, 1, struct boots_console_size)
#define BOOTS_CONSOLE_CLEAR _IO(BOOTS_CONSOLE_IOC_GROUP, 2)
#define BOOTS_CONSOLE_CLEAR_ROW \
	_IOW(BOOTS_CONSOLE_IOC_GROUP, 3, struct boots_console_row)
#define BOOTS_CONSOLE_CLEAR_TO_EOL \
	_IOW(BOOTS_CONSOLE_IOC_GROUP, 4, struct boots_console_position)
#define BOOTS_CONSOLE_GET_CURSOR \
	_IOR(BOOTS_CONSOLE_IOC_GROUP, 5, struct boots_console_cursor)
#define BOOTS_CONSOLE_SET_CURSOR \
	_IOW(BOOTS_CONSOLE_IOC_GROUP, 6, struct boots_console_cursor)
#define BOOTS_CONSOLE_SHOW_CURSOR \
	_IOW(BOOTS_CONSOLE_IOC_GROUP, 7, struct boots_console_cursor)
#define BOOTS_CONSOLE_WRITE_AT \
	_IOW(BOOTS_CONSOLE_IOC_GROUP, 8, struct boots_console_write_at)
#define BOOTS_CONSOLE_POLL_EVENT \
	_IOR(BOOTS_CONSOLE_IOC_GROUP, 9, struct boots_console_event)
#define BOOTS_CONSOLE_READ_EVENT \
	_IOR(BOOTS_CONSOLE_IOC_GROUP, 10, struct boots_console_event)
#define BOOTS_CONSOLE_KEY_STATE \
	_IOWR(BOOTS_CONSOLE_IOC_GROUP, 11, struct boots_console_key_state)
#define BOOTS_CONSOLE_DRAIN_INPUT _IO(BOOTS_CONSOLE_IOC_GROUP, 12)

#endif
