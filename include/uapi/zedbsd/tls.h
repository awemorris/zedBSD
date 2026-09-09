/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_UAPI_TLS_H
#define ZEDBSD_UAPI_TLS_H

#include <stddef.h>
#include <stdint.h>

#define ZEDBSD_TLS_MEMORY_MAX (1024U * 1024U)
#define ZEDBSD_TLS_ALIGN_MAX 4096U
#define ZEDBSD_TLS_TCB_RESERVE 4096U

/* Variant II: the compiler loads this first word through FS:0 or GS:0. */
struct zedbsd_tls_prefix {
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
