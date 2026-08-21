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
#define ZEDBSD_CONSOLE_EVENT_RELEASE  0x00080000U

#define ZEDBSD_CONSOLE_INPUT_TEXT  0U
#define ZEDBSD_CONSOLE_INPUT_EVENT 1U

#define ZEDBSD_CONSOLE_INPUT_EVENT_KEY 1U
#define ZEDBSD_CONSOLE_KEY_RELEASE 0
#define ZEDBSD_CONSOLE_KEY_PRESS   1
#define ZEDBSD_CONSOLE_KEY_REPEAT  2

#define ZEDBSD_CONSOLE_INPUT_FLAG_OVERFLOW 0x0001U

/*
 * Fixed-size record returned by read(2) in event input mode.  Printable
 * keys use their Unicode/ASCII value; non-printable keys use the normalized
 * ZEDBSD_CONSOLE_KEY_* values below.
 */
struct console_input_event {
	uint64_t timestamp_ns;
	uint32_t sequence;
	uint16_t type;
	uint16_t flags;
	uint32_t device_id;
	uint32_t key;
	uint32_t modifiers;
	int32_t state;
};

struct console_input_mode {
	uint32_t mode;
	uint32_t flags;
};

#define ZEDBSD_CONSOLE_KEY_PAGE_UP   0x136U
#define ZEDBSD_CONSOLE_KEY_PAGE_DOWN 0x137U
#define ZEDBSD_CONSOLE_KEY_INSERT    0x138U
#define ZEDBSD_CONSOLE_KEY_DELETE    0x139U
#define ZEDBSD_CONSOLE_KEY_UP        0x13aU
#define ZEDBSD_CONSOLE_KEY_LEFT      0x13bU
#define ZEDBSD_CONSOLE_KEY_RIGHT     0x13cU
#define ZEDBSD_CONSOLE_KEY_DOWN      0x13dU
#define ZEDBSD_CONSOLE_KEY_HOME      0x13eU
#define ZEDBSD_CONSOLE_KEY_END       0x13fU
#define ZEDBSD_CONSOLE_KEY_F1        0x162U
#define ZEDBSD_CONSOLE_KEY_F10       0x16bU
#define ZEDBSD_CONSOLE_KEY_SHIFT     0x170U
#define ZEDBSD_CONSOLE_KEY_CAPS_LOCK 0x171U
#define ZEDBSD_CONSOLE_KEY_KANA      0x172U
#define ZEDBSD_CONSOLE_KEY_GRAPH     0x173U
#define ZEDBSD_CONSOLE_KEY_CTRL      0x174U

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

struct console_event {
	uint32_t value;
};

struct console_key_state {
	uint32_t key;
	int32_t down;
};

#define ZEDBSD_CONSOLE_GET_SIZE \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 1, struct console_size)
#define ZEDBSD_CONSOLE_CLEAR _IO(ZEDBSD_CONSOLE_IOC_GROUP, 2)
#define ZEDBSD_CONSOLE_CLEAR_ROW \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 3, struct console_row)
#define ZEDBSD_CONSOLE_CLEAR_TO_EOL \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 4, struct console_position)
#define ZEDBSD_CONSOLE_GET_CURSOR \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 5, struct console_cursor)
#define ZEDBSD_CONSOLE_SET_CURSOR \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 6, struct console_cursor)
#define ZEDBSD_CONSOLE_SHOW_CURSOR \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 7, struct console_cursor)
#define ZEDBSD_CONSOLE_WRITE_AT \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 8, struct console_write_at)
#define ZEDBSD_CONSOLE_POLL_EVENT \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 9, struct console_event)
#define ZEDBSD_CONSOLE_READ_EVENT \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 10, struct console_event)
#define ZEDBSD_CONSOLE_KEY_STATE \
	_IOWR(ZEDBSD_CONSOLE_IOC_GROUP, 11, struct console_key_state)
#define ZEDBSD_CONSOLE_DRAIN_INPUT _IO(ZEDBSD_CONSOLE_IOC_GROUP, 12)
/* Terminal-capability probe used by isatty(). */
#define ZEDBSD_CONSOLE_ISATTY _IO(ZEDBSD_CONSOLE_IOC_GROUP, 13)
#define ZEDBSD_CONSOLE_GET_INPUT_MODE \
	_IOR(ZEDBSD_CONSOLE_IOC_GROUP, 14, struct console_input_mode)
#define ZEDBSD_CONSOLE_SET_INPUT_MODE \
	_IOW(ZEDBSD_CONSOLE_IOC_GROUP, 15, struct console_input_mode)

#endif
