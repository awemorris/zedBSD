/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_UAPI_TLS_H
#define KERN_UAPI_TLS_H

#include <stddef.h>
#include <stdint.h>

#define KERN_TLS_MEMORY_MAX	(1024U * 1024U)
#define KERN_TLS_ALIGN_MAX	4096U
#define KERN_TLS_TCB_RESERVE	4096U

/*
 * The compiler loads this first word through FS:0 or GS:0.
 */
struct kern_tls_prefix {
	uintptr_t self;
	uintptr_t mapping_base;
	size_t mapping_size;
	uintptr_t template_address;
	size_t template_size;
	size_t memory_size;
	size_t alignment;
	size_t distance;
};

#endif
