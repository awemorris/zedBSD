/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef KERN_KERN_READAHEAD_H
#define KERN_KERN_READAHEAD_H

#include <stdint.h>
#include <stddef.h>
#include <zedbsd/readahead.h>

#define READAHEAD_MIN_WINDOW (64U * 1024U)
#define READAHEAD_MAX_WINDOW (128U * 1024U)
#define READAHEAD_REQUEST_MAX (64U * 1024U)

/* One open description owns this state; its caller serializes observations. */
struct readahead_state {
	uint64_t generation;
	uint64_t next;
	uint64_t issued_end;
	uint64_t useful;
	unsigned window;
	unsigned sequential;
	unsigned valid;
	unsigned exhausted;
};
struct readahead_request {
	uint64_t generation;
	uint64_t offset;
	size_t length;
};

void readahead_reset(struct readahead_state *state);
/* Zero length means no candidate; successful observations never perform I/O. */
int readahead_observe(struct readahead_state *state, uint64_t offset,
    size_t length, uint64_t eof, uint64_t useful, int pressure,
    struct readahead_request *request);
/* Tests cancellation ownership, not VM content/EOF validity. */
int readahead_current(const struct readahead_state *state,
    const struct readahead_request *request);

struct file;
struct inode;
struct mount;
/* The caller retains its mount and must finish every successful boundary. */
struct readahead_boundary {
	struct mount *mount;
	struct readahead_boundary *next;
	unsigned active;
};
struct readahead_stats {
	uint64_t requested_bytes;
	uint64_t started_bytes;
	uint64_t published_bytes;
	uint64_t useful_bytes;
	uint64_t unused_bytes;
	uint64_t discarded_bytes;
	uint64_t errors;
	uint64_t refusals;
	uint64_t memory_bytes;
	unsigned jobs;
	unsigned running;
	unsigned demand;
};
/* Submit is optional and nonblocking for queue admission; caller owns origin/inode. */
int readahead_submit(struct file *origin, struct inode *inode,
    const struct readahead_request *request);
/* Reset the stream and cancel under origin->f_lock before close/seek publication.
 * Workers use origin only as an identity, never dereference its description. */
void readahead_cancel(struct file *origin);
int readahead_demand_begin(void);
void readahead_demand_end(void);
/* NULL mount blocks all admission; joins preparation, I/O and cleanup. */
int readahead_boundary_begin(struct readahead_boundary *boundary, struct mount *mount);
void readahead_boundary_end(struct readahead_boundary *boundary);
void readahead_consumed(size_t useful, size_t unused);
void readahead_report(struct readahead_report *report);
void readahead_snapshot(struct readahead_stats *stats);
/* Frees idle scratch, retaining failed HAL frees as charged reusable owners. */
int readahead_trim(void);

#endif
