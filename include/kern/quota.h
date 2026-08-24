/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_QUOTA_H
#define ZEDBSD_KERN_QUOTA_H

#include "kern/lock.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define QUOTA_MAX_RECORDS		64U
#define QUOTA_DEFAULT_GRACE_SECONDS	(7U * 24U * 60U * 60U)

enum quota_type {
	QUOTA_USER = 0,
	QUOTA_GROUP = 1,
	QUOTA_TYPES = 2,
};

struct quota_record {
	uint32_t id;
	uint64_t block_soft;
	uint64_t block_hard;
	uint64_t inode_soft;
	uint64_t inode_hard;
	uint64_t blocks;
	uint64_t inodes;
	uint64_t block_deadline;
	uint64_t inode_deadline;
	unsigned present;
};

struct quota_state {
	struct mutex lock;
	struct quota_record records[QUOTA_TYPES][QUOTA_MAX_RECORDS];
	uint64_t grace_seconds;
	unsigned enabled[QUOTA_TYPES];
};

struct quota_charge {
	struct quota_state *state;
	uid_t uid;
	gid_t gid;
	uint64_t blocks;
	uint64_t inodes;
	unsigned active;
};

struct quota_transfer {
	struct quota_state *state;
	uid_t old_uid;
	gid_t old_gid;
	uid_t new_uid;
	gid_t new_gid;
	uint64_t blocks;
	uint64_t inodes;
	uint64_t now;
	unsigned active;
};

void
quota_state_init(
	struct quota_state *state);

int
quota_enable(
	struct quota_state *state,
	enum quota_type type,
	int enabled);

int
quota_enabled(
	struct quota_state *state,
	enum quota_type type,
	int *enabled);

int
quota_get_grace(
	struct quota_state *state,
	uint64_t *seconds);

int
quota_set_grace(
	struct quota_state *state,
	uint64_t seconds);

int
quota_get(
	struct quota_state *state,
	enum quota_type type,
	uint32_t id,
	struct quota_record *result);

int
quota_set(
	struct quota_state *state,
	enum quota_type type,
	const struct quota_record *source);

int
quota_reserve(
	struct quota_state *state,
	uid_t uid,
	gid_t gid,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now,
	struct quota_charge *charge);

void
quota_commit(
	struct quota_charge *charge);

void
quota_rollback(
	struct quota_charge *charge);

int
quota_release(
	struct quota_state *state,
	uid_t uid,
	gid_t gid,
	uint64_t blocks,
	uint64_t inodes);

int
quota_transfer_begin(
	struct quota_state *state,
	uid_t old_uid,
	gid_t old_gid,
	uid_t new_uid,
	gid_t new_gid,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now,
	struct quota_transfer *transfer);

void
quota_transfer_commit(
	struct quota_transfer *transfer);

void
quota_transfer_rollback(
	struct quota_transfer *transfer);

int
quota_transfer(
	struct quota_state *state,
	uid_t old_uid,
	gid_t old_gid,
	uid_t new_uid,
	gid_t new_gid,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now);

int
quota_rebuild_add(
	struct quota_state *state,
	uid_t uid,
	gid_t gid,
	uint64_t blocks,
	uint64_t inodes);

int
quota_export_config(
	struct quota_state *state,
	void *buffer,
	size_t capacity,
	size_t *length);

int
quota_import_config(
	struct quota_state *state,
	const void *buffer,
	size_t length);

#endif
