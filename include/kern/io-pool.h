/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef KERN_KERN_IO_POOL_H
#define KERN_KERN_IO_POOL_H

#include <stddef.h>
#include <stdint.h>

#define KERN_IO_BATCH_MAX (64U * 1024U)
#define KERN_IO_SMALL_SIZE 4096U

struct io_pool_stats {
	size_t budget_bytes;
	size_t resident_bytes;
	unsigned large_count;
	unsigned small_count;
	unsigned in_use;
};

void io_pool_init(void);
/* Nonblocking borrow; contents are unspecified. NULL means use caller storage. */
void *io_pool_borrow(size_t wanted, size_t *capacity);
void io_pool_release(void *buffer);
void io_pool_get_stats(struct io_pool_stats *stats);

#endif
