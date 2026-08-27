/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_SWAP_SOURCE_H
#define ZEDBSD_KERN_SWAP_SOURCE_H

#include <stdint.h>
#include <kern/swap.h>

#define KERN_SWAP_SOURCE_COUNT SWAP_SOURCE_COUNT

struct disk;
struct inode;
struct path;

struct kern_swap_source {
	const struct swap_backend_ops *ops;
	void *data;
	struct disk *identity_disk;
	struct inode *identity_inode;
	uint32_t slot_count;
	unsigned parameter_index;
};

struct kern_swap_source_range {
	struct kern_swap_source source;
};

struct kern_swap_source_set {
	struct swap_backend backend;
	struct kern_swap_source_range range[KERN_SWAP_SOURCE_COUNT];
	unsigned count;
	unsigned active;
	/* Serializes every operation which changes range/backend ownership. */
	unsigned control_busy;
};

void
kern_swap_source_init(
	struct kern_swap_source *source);

int
kern_swap_source_prepare_file(
	const struct path *path,
	unsigned parameter_index,
	struct kern_swap_source *source);

int
kern_swap_source_prepare_raw(
	struct disk *disk,
	unsigned parameter_index,
	struct kern_swap_source *source);

void
kern_swap_source_destroy(
	struct kern_swap_source *source);

void
kern_swap_source_set_init(
	struct kern_swap_source_set *set);

int
kern_swap_source_set_add(
	struct kern_swap_source_set *set,
	struct kern_swap_source *source);

/*
 * Reject a block-backed swap source which aliases any part of a native root
 * partition.  File-backed sources are intentionally permitted on the same
 * filesystem: their inode/extents, rather than the whole partition, are the
 * write target.
 */
int
kern_swap_source_set_validate_native_root(
	const struct kern_swap_source_set *set,
	struct disk *root_disk);

int
kern_swap_source_set_activate(
	struct kern_swap_source_set *set);

/*
 * Activation publishes the manager even when it is empty.  Runtime
 * publication chooses the lowest unused source ID.  On success the set owns
 * source and clears the caller object; every failure leaves it caller-owned.
 * Removal is synchronous: it returns only after commitment permits the shrink,
 * every target page is resident, the backing is flushed, and the source claim
 * is released.
 */
int
kern_swap_source_set_runtime_add(
	struct kern_swap_source_set *set,
	struct kern_swap_source *source,
	unsigned *source_id);

int
kern_swap_source_set_runtime_remove(
	struct kern_swap_source_set *set,
	unsigned source_id);

int
kern_swap_source_set_abort(
	struct kern_swap_source_set *set);

int
kern_swap_source_set_map(
	const struct kern_swap_source_set *set,
	uint32_t global_slot,
	unsigned *source_index,
	uint32_t *local_slot);

unsigned
kern_swap_source_file_extent_count(void);

#endif
