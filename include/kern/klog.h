/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_KLOG_H
#define ZEDBSD_KERN_KLOG_H

#include <stddef.h>
#include <stdint.h>

void
kern_log_init(void);

void
kern_log_write(
	const char *bytes,
	size_t length);

void
kern_logf(
	const char *format,
	...)
__attribute__((format(printf, 1, 2)));

size_t
kern_log_snapshot(
	char *buffer,
	size_t capacity,
	uint64_t *dropped);

size_t
kern_log_capacity(void);

#endif
