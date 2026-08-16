/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_FCNTL_H
#define ZEDBSD_UAPI_FCNTL_H

#include <stdint.h>

struct zedbsd_flock_request {
	int16_t type;
	int16_t whence;
	int32_t reserved0;
	int64_t start;
	int64_t length;
	int32_t pid;
	uint32_t reserved1;
};

#endif
