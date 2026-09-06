/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_UAPI_FCNTL_H
#define ZEDBSD_UAPI_FCNTL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

#define ZEDBSD_FILE_FORMAT_VERSION 1U

/* The opened description owns this mutation lease until its final close. */
struct zedbsd_file_format_reserve {
	uint32_t version;
	uint32_t struct_size;
	uint64_t size_bytes;
	uint64_t reserved[2];
};

typedef char zedbsd_file_format_size_check[
	sizeof(struct zedbsd_file_format_reserve) == 32U ? 1 : -1];
typedef char zedbsd_file_format_alignment_check[
	offsetof(struct zedbsd_file_format_reserve, size_bytes) == 8U ? 1 : -1];

#define ZEDBSD_FILE_FORMAT_RESERVE \
	_IOW('f', 1, struct zedbsd_file_format_reserve)

struct flock_record {
	int16_t type;
	int16_t whence;
	int32_t reserved0;
	int64_t start;
	int64_t length;
	int32_t pid;
	uint32_t reserved1;
};

#endif
