/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_APPLET_H
#define ZEDBSD_UAPI_APPLET_H

#include <stdint.h>

#define ZEDBSD_APPLET_MAGIC 0x41383942U /* "B98A" */

struct zedbsd_applet_header {
	uint32_t magic;
	uint16_t abi_version;
	uint16_t header_size;
	uint32_t image_size;
	uint16_t entry_offset;
	uint16_t flags;
	uint32_t crc32;
	char name[16];
} __attribute__((packed));

struct zedbsd_applet_services {
	uint16_t abi_version;
	uint16_t size;
	void (*putc)(char c);
	void (*puts)(const char *s);
	uint32_t (*key_read)(void);
};

typedef uint32_t (*zedbsd_applet_entry_t)(
	const struct zedbsd_applet_services *, uint32_t,
	const char *const *);

_Static_assert(sizeof(struct zedbsd_applet_header) == 36,
	"zedBSD applet header must remain 36 bytes");

#endif
