/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_IO_ERROR_H
#define KERN_KERN_IO_ERROR_H

#include <kern/atomic.h>

/* Zero-initialized owners have no recorded failure. */
struct io_error_state {
	atomic_uint_t guard;
	uint64_t sequence;
	int error;
};

struct io_error_snapshot {
	uint64_t sequence;
	int error;
};

void io_error_record(struct io_error_state *state, int error);
void io_error_snapshot(struct io_error_state *state, struct io_error_snapshot *snapshot);
int io_error_observe(const struct io_error_snapshot *snapshot, volatile uint64_t *cursor);

#endif
