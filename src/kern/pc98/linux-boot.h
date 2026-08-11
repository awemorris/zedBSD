/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_PC98_LINUX_BOOT_H
#define ZEDBSD_KERN_PC98_LINUX_BOOT_H

#include <kern/boot.h>
#include <kern/fs.h>

int pc98_linux_boot(struct zedbsd_filesystem *filesystem, const char *path,
		    const char *arguments, const struct zedbsd_device *devices,
		    unsigned device_count, int boot_device);

#endif
