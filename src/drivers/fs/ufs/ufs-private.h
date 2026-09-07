/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UFS_PRIVATE_H
#define ZEDBSD_UFS_PRIVATE_H

#include <kern/buf.h>
#include <hal/hal.h>
#include <kern/inode.h>
#include <kern/quota.h>
#include "ufs-disk.h"
#include "ufs-consistency.h"

struct ufs_io_owner {
	struct disk *disk;
	const struct io_context *context;
};

/* Share internal object layouts with production-linked lifetime fixtures. */
struct ufs_mount_state {
	/* Each borrowed context is protected by its corresponding I/O lock. */
	struct ufs_io_owner journal_io;
	struct ufs_io_owner snapshot_io;
	struct ufs_super super;
	struct mutex namespace_lock;
	struct mutex lock;
	struct mutex journal_lock;
	uint8_t *cg;
	struct buf_view cg_view;
	unsigned cg_valid;
	unsigned cg_dirty;
	uint32_t cg_iusedoff;
	uint32_t cg_freeoff;
	uint32_t cg_nextfreeoff;
	uint32_t active_cg;
	uint32_t rotor_cg;
	struct ufs_journal journal;
	struct hal_pmem journal_memory;
	struct ufs_snapshot snapshot;
	struct ufs_snapshot_entry *snapshot_map;
	struct disk *snapshot_disk;
	struct mutex snapshot_lock;
	struct quota_state quota;
	int journal_enabled;
	int snapshot_available;
	int writable;
};

struct ufs_inode_info {
	struct inode inode;
	uint64_t extattr[UFS_NXADDR];
	uint32_t extattr_size;
	uint64_t direct[UFS_NDADDR];
	uint64_t indirect[UFS_NIADDR];
	uint32_t disk_flags;
	uint64_t blocks;
	uint32_t generation;
	uint8_t shortlink[120];
};

#endif
