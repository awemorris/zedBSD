/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD public ABI scalar types
 */

#ifndef ZEDBSD_UAPI_TYPES_H
#define ZEDBSD_UAPI_TYPES_H

#include <stdint.h>

/*
 * The pointed-to address belongs to the calling user ABI, not the kernel.
 */
#ifdef ZEDBSD_USER_ABI_LP64
typedef uint64_t uapi_ptr_t;
#else
typedef uint32_t uapi_ptr_t;
#endif

#endif
