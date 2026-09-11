/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_MOUNT_H
#define LIBC_SYS_MOUNT_H

#include <stdint.h>
#include <zedbsd/unmount.h>

#define MNT_RDONLY 0x00000001U
#define MNT_NOSUID  0x00000002U
#define KERN_MOUNT_ARGS_VERSION 1U
#define KERN_MOUNT_FSPEC_MAX 32U

struct mount_args {
	uint32_t size;
	uint32_t version;
	char fspec[KERN_MOUNT_FSPEC_MAX];
};

int mount(const char *, const char *, int, void *);
int unmount(const char *, int);

#endif
