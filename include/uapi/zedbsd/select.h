/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_SELECT_H
#define ZEDBSD_UAPI_SELECT_H

#include <stdint.h>

#define ZEDBSD_FD_SETSIZE 32
typedef struct fd_set {
	uint32_t bits[1];
} fd_set;

#endif
