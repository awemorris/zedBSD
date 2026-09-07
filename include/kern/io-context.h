/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_IO_CONTEXT_H
#define ZEDBSD_KERN_IO_CONTEXT_H

#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#define IO_CONTEXT_THROUGH 0x01U
#define IO_CONTEXT_DRAIN 0x02U
#define IO_CONTEXT_ORDERED 0x04U
#define IO_CONTEXT_DEPTH_MAX 16U

struct inode;
struct disk;
struct backing_claim;

/*
 * Borrowed synchronous provenance: the parent operation retains inode/claim
 * ownership until all child I/O completes. Copies preserve identity, not refs.
 * Retained dirty buffers must create a fresh drain context instead of saving
 * these borrowed pointers. Asynchronous dispatch needs an owning envelope.
 */
struct io_context {
	struct inode *origin_inode;
	const struct backing_claim *claim;
	struct disk *media_disk;
	uint64_t content_generation;
	uint64_t media_generation;
	unsigned flags;
	unsigned depth;
};

/* Validates supported semantics; zero is the legacy synchronous through mode. */
static __inline int
io_context_validate(
	const struct io_context *context)
{
	if (context == NULL)
		return 0;
	if ((context->flags & ~(IO_CONTEXT_THROUGH | IO_CONTEXT_DRAIN |
	    IO_CONTEXT_ORDERED)) != 0)
		return EOPNOTSUPP;
	if (context->depth > IO_CONTEXT_DEPTH_MAX)
		return ELOOP;
	return 0;
}

/* Copies inherited provenance and strengthens flags for one synchronous child. */
static __inline int
io_context_child(
	struct io_context *child,
	const struct io_context *parent,
	unsigned flags)
{
	struct io_context next = { NULL, NULL, NULL, 0, 0, 0, 0 };
	int error;

	/* Rejects unsupported semantics before publishing a partially changed child. */
	if (child == NULL)
		return EINVAL;
	error = io_context_validate(parent);
	if (error != 0)
		return error;
	if (parent != NULL) {
		if (parent->depth == IO_CONTEXT_DEPTH_MAX)
			return ELOOP;
		next = *parent;
		next.depth++;
	}
	next.flags |= flags | IO_CONTEXT_THROUGH;
	if ((next.flags & IO_CONTEXT_ORDERED) != 0)
		next.flags |= IO_CONTEXT_DRAIN;
	error = io_context_validate(&next);
	if (error != 0)
		return error;
	*child = next;
	return 0;
}

#endif
