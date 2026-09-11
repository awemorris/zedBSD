/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_KERN_WRITEBACK_H
#define KERN_KERN_WRITEBACK_H
#include <stdint.h>
#include <stddef.h>
#include <uapi/writeback.h>

struct disk;
struct mount;
#define WRITEBACK_DEVICE_MAX 4U
#define WRITEBACK_TICKET_BYTES (64U * 1024U)

/* Owned by a policy worker; detach refuses live tickets or dirty credits. */
struct writeback_budget {
	struct disk *disk;
	uint64_t dirty;
	uint64_t reserved;
	uint64_t tickets;
	uint64_t refusals;
	unsigned quiescing;
};
struct writeback_ticket {
	struct writeback_budget *budget;
	uint64_t reserved;
};
struct writeback_budget_stats {
	uint64_t high;
	uint64_t low;
	uint64_t device_high;
	uint64_t dirty;
	uint64_t reserved;
	uint64_t tickets;
	uint64_t refusals;
	unsigned devices;
};

/* Returns one physical cache token; release with disk_cache_release. */
int writeback_domain_acquire(struct disk *disk, struct disk **leaf);

struct writeback_policy_stats {
	uint64_t memory_bytes;
	uint64_t passes;
	uint64_t errors;
	unsigned mounts;
	unsigned workers;
	unsigned busy;
	int last_error;
};
/* Zero-initialize; retain the mount until finish, and do not copy a live token. */
struct writeback_unmount {
	struct mount *mount;
	void *worker;
};
int writeback_unmount_begin(struct mount *mount, struct writeback_unmount *token);
/* Explicit lost-media boundary: caller must dispose dirty owners before commit. */
int writeback_unmount_begin_revoked(struct mount *mount, struct writeback_unmount *token);
void writeback_unmount_finish(struct writeback_unmount *token, int committed);
int writeback_shutdown_begin(void);
void writeback_shutdown_finish(int committed);
int writeback_mount_set(struct mount *mount, int enabled);
/* Query under the filesystem owner, before device locks; sync must take that owner. */
int writeback_mount_active(struct mount *mount);
int writeback_mount_admit(struct mount *mount, struct writeback_ticket *ticket);
void writeback_pressure(struct writeback_budget *budget);
void writeback_policy_snapshot(struct writeback_policy_stats *stats);
/* Fills sizeof(struct writeback_report) bytes; permits unaligned sysctl buffers. */
void writeback_policy_report(void *buffer);
int writeback_budget_read(struct writeback_budget *budget, struct writeback_budget *snapshot);

void writeback_budget_limits(uint64_t cache_target, uint64_t *high, uint64_t *low, uint64_t *device_high);
int writeback_budget_attach(struct writeback_budget *budget, struct disk *leaf);
int writeback_budget_detach(struct writeback_budget *budget);
int writeback_budget_quiesce(struct writeback_budget *budget, int enabled);
/* Zero-initialize tickets. Reserve before position/inode/content leases; never waits. */
int writeback_ticket_reserve(struct writeback_budget *budget, struct writeback_ticket *ticket);
/* Converts reserved bytes into retained dirty ownership, without allocating. */
void writeback_ticket_commit(struct writeback_ticket *ticket, size_t bytes);
void writeback_ticket_release(struct writeback_ticket *ticket);
void writeback_budget_clean(struct writeback_budget *budget, size_t bytes);
void writeback_budget_snapshot(struct writeback_budget_stats *stats);
#endif
