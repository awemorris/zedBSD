/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SYS_MOUNT_H
#define ZEDBSD_SYS_MOUNT_H

#include <stdint.h>

#define MNT_RDONLY 0x00000001U
#define ZEDBSD_MOUNT_ARGS_VERSION 1U
#define ZEDBSD_MOUNT_FSPEC_MAX 32U

struct mount_args {
	uint32_t size;
	uint32_t version;
	char fspec[ZEDBSD_MOUNT_FSPEC_MAX];
};

int mount(const char *, const char *, int, void *);
int unmount(const char *, int);

#endif
