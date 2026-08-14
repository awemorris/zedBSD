/*
 * Linux chain loader
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_BOOT_H
#define ZEDBSD_UAPI_BOOT_H

#include <stdint.h>
#include <sys/ioctl.h>

struct zedbsd_boot_linux {
	int32_t kernel_fd;
	int32_t boot_device_index;
	uint32_t command_line;
	uint32_t command_line_length;
	uint32_t flags;
	uint32_t reserved[3];
};

#define ZEDBSD_BOOT_IOC_GROUP 'b'
#define ZEDBSD_BOOT_LINUX _IOW(ZEDBSD_BOOT_IOC_GROUP, 1, struct zedbsd_boot_linux)

#endif
