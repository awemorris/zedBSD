/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_BACKING_CLAIM_H
#define ZEDBSD_KERN_BACKING_CLAIM_H

#include <stdint.h>

struct backing_claim;
struct disk;
struct inode;

enum backing_claim_owner {
	BACKING_CLAIM_SWAP = 1,
	BACKING_CLAIM_LOOP = 2,
};

struct backing_claim_extent {
	struct disk *disk;
	uint64_t block;
	uint64_t block_count;
};

/* Caller-owned token.  Its fields are private to backing-claim.c. */
struct backing_mutation_guard {
	unsigned slot;
	uint64_t generation;
	unsigned active;
};

int backing_claim_prepare_inode(struct inode *, enum backing_claim_owner,
				struct backing_claim **);
int backing_claim_finalize(struct backing_claim *,
			   const struct backing_claim_extent *, unsigned);
int backing_claim_prepare_disk(struct disk *, uint64_t, uint64_t,
			       enum backing_claim_owner,
			       struct backing_claim **);
void backing_claim_release(struct backing_claim *);

int backing_mutation_begin_inode(struct inode *,
				 struct backing_mutation_guard *);
int backing_mutation_begin_inode_claimed(struct inode *,
					 const struct backing_claim *,
					 struct backing_mutation_guard *);
int backing_mutation_begin_disk(struct disk *, uint64_t, uint64_t,
				const struct backing_claim *,
				struct backing_mutation_guard *);
int backing_mutation_begin_disk_filesystem(
	struct disk *, uint64_t, uint64_t, struct backing_mutation_guard *);
void backing_mutation_end(struct backing_mutation_guard *);

int backing_claim_check_disk(struct disk *, uint64_t, uint64_t,
			     const struct backing_claim *);
int backing_claim_check_mount(struct disk *, unsigned);
int backing_claim_check_teardown(struct disk *);

#endif
