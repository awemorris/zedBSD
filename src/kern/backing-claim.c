/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backing-store claim registry.
 *
 * A swap, loop, or formatting owner claims the FAT file or raw disk range backing
 * it, and every filesystem or raw write reserves the object or range it is
 * about to mutate.  The registry rejects a mutation that would alias a live
 * claim, so a backing object cannot change underneath its owner, and it
 * rejects a claim that would alias a mutation already in flight.
 */

#include <kern/backing-claim.h>

#include <kern/disk.h>
#include <kern/fat.h>
#include <kern/inode.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/mount.h>

#include <errno.h>
#include <string.h>

#define BACKING_CLAIM_MAX 16U
#define BACKING_MUTATION_MAX 64U

struct backing_object_key {
	struct disk *leaf;
	uint64_t volume_first;
	uint64_t volume_last;
	uint64_t object;
};

struct backing_range {
	struct disk *leaf;
	uint64_t first;
	uint64_t last;
};

struct backing_claim {
	refcount_t refs;
	struct disk *pinned_leaf;
	enum backing_claim_owner owner;
	struct backing_object_key key;
	struct backing_range *ranges;
	unsigned range_count;
	unsigned key_valid;
	unsigned preparing;
	unsigned registered;
};

struct backing_mutation {
	struct backing_object_key key;
	struct backing_range range;
	struct backing_range filesystem_volume;
	const struct backing_claim *owner;
	const void *execution;
	uint64_t generation;
	unsigned key_valid;
	unsigned range_valid;
	unsigned filesystem_volume_valid;
	unsigned filesystem;
	unsigned used;
};

extern int vm_object_backing_busy(const struct backing_claim *) __attribute__((weak));

static struct backing_claim *claims[BACKING_CLAIM_MAX];
static struct backing_mutation mutations[BACKING_MUTATION_MAX];
static uint64_t mutation_generation;

static struct spinlock claim_lock = {
	{0},
	LOCK_RANK_BACKING_CLAIM,
	"backing claims",
	0,
	0
};

/*
 * Host fixtures and early boot may not provide a current thread.
 */
struct thread;
extern struct thread *thread_current(void) __attribute__((weak));
extern int fat_file_backing_identity(struct inode *, struct disk **, uint64_t *) __attribute__((weak));

/*
 * Before the scheduler publishes a current thread, the kernel
 * executes boot filesystem I/O serially.  Give that execution context
 * a stable identity so nested FAT -> buffer-cache -> direct-I/O
 * mutations can retain ownership.
 */
static const unsigned char early_boot_execution_token;

/*
 * Forward declarations.
 */
static const void *current_execution(void);
static int canonical_range(struct disk *disk, uint64_t block, uint64_t count, struct backing_range *result);
static int inode_key(struct inode *inode, struct backing_object_key *key);
static int range_overlap(const struct backing_range *left, const struct backing_range *right);
static int range_contains(const struct backing_range *outer, const struct backing_range *inner);
static int range_equal(const struct backing_range *left, const struct backing_range *right);
static int claim_contains_range(const struct backing_claim *claim, const struct backing_range *range);
static int key_equal(const struct backing_object_key *left, const struct backing_object_key *right);
static struct backing_range key_volume(const struct backing_object_key *key);
static int claim_insert(struct backing_claim *claim);
static int mutation_reserve(const struct backing_object_key *key, const struct backing_range *range, const struct backing_range *filesystem_volume, const struct backing_claim *owner, int filesystem, struct backing_mutation_guard *guard);

/*
 * Compares an inode against a retained claim's canonical backing identity.
 *
 * The caller keeps the claim alive throughout this sleeping identity query.
 */
int
backing_claim_inode_matches(
	const struct backing_claim *claim,
	struct inode *inode,
	int *matched)
{
	struct backing_object_key key;
	int error;

	/* Rejects an incomplete query before resolving filesystem identity. */
	if (claim == NULL || inode == NULL || matched == NULL)
		return EINVAL;
	*matched = 0;

	/* Limits inode comparisons to file-backed claims. */
	if (!claim->registered || !claim->key_valid)
		return EINVAL;

	/* Treats unsupported filesystems as unrelated backing objects. */
	error = inode_key(inode, &key);
	if (error == EOPNOTSUPP)
		return 0;

	/* Propagates a failed identity query rather than overlooking an alias. */
	if (error != 0)
		return error;

	/* Publishes the physical-volume and directory-entry comparison. */
	*matched = key_equal(&claim->key, &key);
	return 0;
}

/*
 * Registers a preparing claim on the FAT file behind an inode.
 *
 * The claim is registered before its extents are known, so no mutation of
 * the file, and none of the volume while the claim is still preparing, can
 * slip in while the owner resolves the file layout.  backing_claim_finalize()
 * publishes the extents and backing_claim_release() withdraws the claim.
 */
int
backing_claim_prepare_inode(
	struct inode *inode,
	enum backing_claim_owner owner,
	struct backing_claim **result)
{
	struct backing_claim *claim;
	struct backing_claim *existing;
	struct backing_range volume;
	struct backing_range br;
	unsigned i;
	unsigned j;
	unsigned long irq;
	int error;

	/* Rejects a missing result pointer or an unknown owner kind. */
	if (result == NULL ||
	    (owner != BACKING_CLAIM_SWAP && owner != BACKING_CLAIM_LOOP &&
	     owner != BACKING_CLAIM_FORMAT))
		return EINVAL;

	*result = NULL;

	/* Allocates the claim record. */
	claim = kern_calloc(1, sizeof(*claim));
	if (claim == NULL)
		return ENOMEM;
	refcount_init(&claim->refs, 1);

	/* Resolves the canonical identity of the backing file. */
	error = inode_key(inode, &claim->key);
	if (error != 0) {
		kern_free(claim);
		return error;
	}

	/* Describes a keyed claim whose extents are still pending. */
	claim->owner = owner;
	claim->key_valid = 1;
	claim->preparing = 1;
	volume = key_volume(&claim->key);

	irq = spin_lock_irqsave(&claim_lock);

	/* Rejects every registered claim that aliases the same file. */
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		existing = claims[i];
		if (existing == NULL)
			continue;

		br.leaf = existing->key.leaf;
		br.first = existing->key.volume_first;
		br.last = existing->key.volume_last;

		/* An identical keyed claim always conflicts. */
		if (existing->key_valid &&
		    key_equal(&claim->key, &existing->key)) {
			error = EBUSY;
			goto out_locked;
		}

		/* A keyed claim that is still preparing owns its whole volume. */
		if (existing->preparing &&
		    existing->key_valid &&
		    range_overlap(&volume, &br)) {
			error = EBUSY;
			goto out_locked;
		}

		/* A raw claim conflicts through any extent inside the volume. */
		if (!existing->key_valid) {
			for (j = 0; j < existing->range_count; j++) {
				if (range_overlap(&volume, &existing->ranges[j])) {
					error = EBUSY;
					goto out_locked;
				}
			}
		}
	}

	/* Rejects every reserved mutation that aliases the file or volume. */
	for (i = 0; i < BACKING_MUTATION_MAX; i++) {
		if (!mutations[i].used)
			continue;

		/* A keyed mutation of the same file conflicts. */
		if (mutations[i].key_valid &&
		    key_equal(&claim->key, &mutations[i].key)) {
			error = EBUSY;
			goto out_locked;
		}

		/* A raw mutation inside the volume conflicts. */
		if (!mutations[i].key_valid &&
		    mutations[i].range_valid &&
		    range_overlap(&volume, &mutations[i].range)) {
			error = EBUSY;
			goto out_locked;
		}
	}

	/* Publishes the preparing claim. */
	error = claim_insert(claim);

out_locked:
	spin_unlock_irqrestore(&claim_lock, irq);

	/* Releases a claim that could not be registered. */
	if (error != 0) {
		kern_free(claim);
		return error;
	}

	/* Tests VM ownership after closing admission, outside the claim spinlock. */
	if (vm_object_backing_busy != NULL) {
		error = vm_object_backing_busy(claim);
		if (error != 0) {
			backing_claim_release(claim);
			return error;
		}
	}

	*result = claim;

	/* Reports the registered preparing claim. */
	return 0;
}

/*
 * Publishes the extents of a preparing claim.
 *
 * Every extent must lie inside the claimed file's volume, must not touch an
 * extent of another claim, and must not touch a reserved raw mutation.
 * Success ends the preparing state; failure leaves the claim preparing so
 * that the owner can still release it.
 */
int
backing_claim_finalize(
	struct backing_claim *claim,
	const struct backing_claim_extent *extents,
	unsigned count)
{
	struct backing_range *ranges;
	struct backing_claim *existing;
	unsigned i;
	unsigned j;
	unsigned k;
	unsigned long irq;
	int error;

	ranges = NULL;
	error = 0;

	/* Rejects an unregistered, already final, or inconsistent request. */
	if (claim == NULL ||
	    !claim->registered ||
	    !claim->preparing ||
	    (count != 0 && extents == NULL))
		return EINVAL;

	/* Canonicalizes every extent and confines it to the claimed volume. */
	if (count != 0) {
		ranges = kern_calloc(count, sizeof(*ranges));
		if (ranges == NULL)
			return ENOMEM;

		for (i = 0; i < count; i++) {
			error = canonical_range(
				extents[i].disk,
				extents[i].block,
				extents[i].block_count,
				&ranges[i]);
			if (error != 0) {
				kern_free(ranges);
				return error;
			}

			/* An extent outside the claimed volume is a foreign device. */
			if (ranges[i].leaf != claim->key.leaf ||
			    ranges[i].first < claim->key.volume_first ||
			    ranges[i].last > claim->key.volume_last) {
				kern_free(ranges);
				return EXDEV;
			}
		}
	}

	irq = spin_lock_irqsave(&claim_lock);

	/* Rejects an extent that overlaps another claim's published extent. */
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		existing = claims[i];
		if (existing == NULL || existing == claim)
			continue;

		for (j = 0; j < count; j++) {
			for (k = 0; k < existing->range_count; k++) {
				if (range_overlap(&ranges[j], &existing->ranges[k])) {
					error = EBUSY;
					goto out_locked;
				}
			}
		}
	}

	/* Rejects an extent that overlaps a reserved raw mutation. */
	for (i = 0; i < BACKING_MUTATION_MAX; i++) {
		if (!mutations[i].used ||
		    mutations[i].key_valid ||
		    !mutations[i].range_valid)
			continue;

		for (j = 0; j < count; j++) {
			if (range_overlap(&ranges[j], &mutations[i].range)) {
				error = EBUSY;
				goto out_locked;
			}
		}
	}

	/* Publishes the extents and ends the preparing state. */
	claim->ranges = ranges;
	claim->range_count = count;
	claim->preparing = 0;
	ranges = NULL;

out_locked:
	spin_unlock_irqrestore(&claim_lock, irq);

	/* Frees the extents only when they were not published. */
	kern_free(ranges);

	/* Reports the finalization result. */
	return error;
}

/*
 * Registers a final claim on one raw disk range.
 *
 * A raw claim needs no finalization because its single extent is canonical
 * from the start.  The range must not sit on a writable mount, alias a keyed
 * claim's volume, overlap another claim's extent, or overlap a reserved
 * mutation.
 */
int
backing_claim_prepare_disk(
	struct disk *disk,
	uint64_t block,
	uint64_t count,
	enum backing_claim_owner owner,
	struct backing_claim **result)
{
	struct backing_claim *claim;
	struct backing_claim *existing;
	struct backing_range volume;
	unsigned i;
	unsigned j;
	unsigned long irq;
	int error;

	/* Rejects a missing result pointer or an unknown owner kind. */
	if (result == NULL ||
	    (owner != BACKING_CLAIM_SWAP && owner != BACKING_CLAIM_LOOP))
		return EINVAL;

	*result = NULL;

	/* Refuses a disk that carries a writable mount. */
	error = mount_disk_writable_busy(disk);
	if (error != 0)
		return error;

	/* Allocates the claim record. */
	claim = kern_calloc(1, sizeof(*claim));
	if (claim == NULL)
		return ENOMEM;
	refcount_init(&claim->refs, 1);

	/* Allocates the single extent. */
	claim->ranges = kern_calloc(1, sizeof(*claim->ranges));
	if (claim->ranges == NULL) {
		kern_free(claim);
		return ENOMEM;
	}

	/* Canonicalizes the requested range. */
	error = canonical_range(disk, block, count, claim->ranges);
	if (error != 0) {
		kern_free(claim->ranges);
		kern_free(claim);
		return error;
	}

	claim->owner = owner;
	claim->range_count = 1;

	irq = spin_lock_irqsave(&claim_lock);

	/* Rejects every registered claim whose volume or extents overlap. */
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		existing = claims[i];
		if (existing == NULL)
			continue;

		/* A keyed claim owns its whole volume. */
		if (existing->key_valid) {
			volume = key_volume(&existing->key);
			if (range_overlap(&claim->ranges[0], &volume)) {
				error = EBUSY;
				goto out_locked;
			}
		}

		/* Every published extent excludes the range. */
		for (j = 0; j < existing->range_count; j++) {
			if (range_overlap(&claim->ranges[0], &existing->ranges[j])) {
				error = EBUSY;
				goto out_locked;
			}
		}
	}

	/* Rejects every reserved mutation that overlaps the range. */
	for (i = 0; i < BACKING_MUTATION_MAX; i++) {
		if (mutations[i].used &&
		    mutations[i].range_valid &&
		    range_overlap(&claim->ranges[0], &mutations[i].range)) {
			error = EBUSY;
			goto out_locked;
		}
	}

	/* Publishes the claim. */
	error = claim_insert(claim);

out_locked:
	spin_unlock_irqrestore(&claim_lock, irq);

	/* Releases a claim that could not be registered. */
	if (error != 0) {
		kern_free(claim->ranges);
		kern_free(claim);
		return error;
	}

	/*
	 * Retain the LIVE-mount rescan as a defensive validation.  A
	 * writable mount that is still PREPARING holds a whole-volume
	 * mutation reservation, so claim insertion above either
	 * observes that reservation or precedes it.
	 */
	error = mount_disk_writable_busy(disk);
	if (error != 0) {
		backing_claim_release(claim);
		return error;
	}

	*result = claim;

	/* Reports the registered claim. */
	return 0;
}

/*
 * Withdraws a claim and frees its record.
 *
 * A NULL claim is ignored so that error paths can release unconditionally.
 */
/* Retains a claim already owned by the caller across deferred I/O. */
void
backing_claim_ref(
	const struct backing_claim *claim)
{
	if (claim != NULL)
		refcount_get(&((struct backing_claim *)claim)->refs);
}

void
backing_claim_release(
	struct backing_claim *claim)
{
	unsigned i;
	unsigned long irq;

	/* Ignores a missing claim. */
	if (claim == NULL || !refcount_put(&claim->refs))
		return;

	/* Unregisters the claim from the registry slot that holds it. */
	irq = spin_lock_irqsave(&claim_lock);
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		if (claims[i] == claim) {
			claims[i] = NULL;
			claim->registered = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&claim_lock, irq);

	/* The claim keeps its canonical device identity alive through final release. */
	disk_release(claim->pinned_leaf);
	/* Frees the extents and the record. */
	kern_free(claim->ranges);
	kern_free(claim);
}

/*
 * Reserves an unowned mutation of the FAT file behind an inode.
 */
int
backing_mutation_begin_inode(
	struct inode *inode,
	struct backing_mutation_guard *guard)
{
	int error;

	/* Delegates to the owner-aware form without an owner. */
	error = backing_mutation_begin_inode_claimed(inode, NULL, guard);

	/* Reports the reservation result. */
	return error;
}

/*
 * Reserves a mutation of the FAT file behind an inode on behalf of an
 * optional owning claim.
 *
 * A file that cannot back a swap object needs no reservation: the guard is
 * left inactive and the mutation may proceed.
 */
int
backing_mutation_begin_inode_claimed(
	struct inode *inode,
	const struct backing_claim *owner,
	struct backing_mutation_guard *guard)
{
	struct backing_object_key key;
	int error;

	/* Rejects a missing guard. */
	if (guard == NULL)
		return EINVAL;

	/* Resolves the canonical identity of the backing file. */
	error = inode_key(inode, &key);

	/*
	 * Swap files are FAT-only.  Other files retain their existing
	 * local INODE_LOOPFILE exclusion and cannot alias a FAT swap
	 * claim.
	 */
	if (error == EOPNOTSUPP || error == EINVAL) {
		memset(guard, 0, sizeof(*guard));
		return 0;
	}

	/* Propagates any other identity failure. */
	if (error != 0)
		return error;

	/* Reserves the keyed mutation. */
	error = mutation_reserve(&key, NULL, NULL, owner, 0, guard);

	/* Reports the reservation result. */
	return error;
}

/*
 * Reserves a raw mutation of one disk range on behalf of an optional
 * owning claim.
 */
int
backing_mutation_begin_disk(
	struct disk *disk,
	uint64_t block,
	uint64_t count,
	const struct backing_claim *owner,
	struct backing_mutation_guard *guard)
{
	struct backing_range range;
	int error;

	/* Canonicalizes the range. */
	error = canonical_range(disk, block, count, &range);
	if (error != 0)
		return error;

	/* Reserves the raw mutation. */
	error = mutation_reserve(NULL, &range, NULL, owner, 0, guard);

	/* Reports the reservation result. */
	return error;
}

/*
 * Reserves teardown of a retained, revoked physical medium.
 *
 * Its retained geometry is used only for exclusion, never data access.
 */
int
backing_mutation_begin_retired_disk(
	struct disk *disk,
	struct backing_mutation_guard *guard)
{
	struct backing_range range;

	if (disk == NULL || disk->d_parent != NULL || disk->d_block_count == 0 ||
	    !atomic_raw_load_acquire(&disk->d_media_revoked))
		return EINVAL;
	range.leaf = disk;
	range.first = 0;
	range.last = disk->d_block_count;
	return mutation_reserve(NULL, &range, NULL, NULL, 0, guard);
}

/*
 * Reserves a filesystem-initiated mutation of one disk range.
 *
 * The whole volume accompanies the range so that a nested raw write on the
 * same execution can inherit the filesystem's ownership.
 */
int
backing_mutation_begin_disk_filesystem(
	struct disk *disk,
	uint64_t block,
	uint64_t count,
	struct backing_mutation_guard *guard)
{
	struct backing_range range;
	struct backing_range volume;
	int error;

	/* Canonicalizes the range. */
	error = canonical_range(disk, block, count, &range);
	if (error != 0)
		return error;

	/* Canonicalizes the whole volume that contains it. */
	error = canonical_range(disk, 0, disk->d_block_count, &volume);
	if (error != 0)
		return error;

	/* Reserves the filesystem mutation. */
	error = mutation_reserve(NULL, &range, &volume, NULL, 1, guard);

	/* Reports the reservation result. */
	return error;
}

/*
 * Releases a mutation reservation.
 *
 * The slot is cleared only while it still carries this guard's generation,
 * so a stale guard can never release a later reservation.
 */
void
backing_mutation_end(
	struct backing_mutation_guard *guard)
{
	unsigned long irq;

	/* Ignores a missing or inactive guard. */
	if (guard == NULL ||
	    !guard->active ||
	    guard->slot >= BACKING_MUTATION_MAX)
		return;

	/* Clears the slot that still belongs to this guard. */
	irq = spin_lock_irqsave(&claim_lock);
	if (mutations[guard->slot].used &&
	    mutations[guard->slot].generation == guard->generation) {
		memset(&mutations[guard->slot], 0, sizeof(mutations[guard->slot]));
	}
	spin_unlock_irqrestore(&claim_lock, irq);

	/* Deactivates the guard. */
	memset(guard, 0, sizeof(*guard));
}

/*
 * Checks whether a raw range could be mutated on behalf of an owner.
 *
 * The check reserves and immediately releases the mutation, so it reports
 * the verdict a real mutation would receive at this moment.
 */
int
backing_claim_check_disk(
	struct disk *disk,
	uint64_t block,
	uint64_t count,
	const struct backing_claim *owner)
{
	struct backing_mutation_guard guard;
	int error;

	/* Probes the reservation and releases it again. */
	error = backing_mutation_begin_disk(disk, block, count, owner, &guard);
	if (error == 0)
		backing_mutation_end(&guard);

	/* Reports the probe result. */
	return error;
}

/*
 * Checks whether a disk may be mounted with the requested flags.
 *
 * A read-only mount never conflicts.  A writable mount conflicts with any
 * keyed claim on the volume and with any claim extent inside it.
 */
int
backing_claim_check_mount(
	struct disk *disk,
	unsigned flags)
{
	struct backing_claim *claim;
	struct backing_range range;
	unsigned i;
	unsigned j;
	unsigned long irq;
	int error;

	/* Permits a read-only mount unconditionally. */
	if ((flags & MOUNT_READ_ONLY) != 0)
		return 0;

	/* Canonicalizes the whole volume. */
	error = canonical_range(disk, 0, disk->d_block_count, &range);
	if (error != 0)
		return error;

	/* Rejects the mount while any claim lives on the volume. */
	irq = spin_lock_irqsave(&claim_lock);
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		claim = claims[i];
		if (claim == NULL)
			continue;

		/* A keyed claim on the same leaf conflicts through its volume. */
		if (claim->key_valid &&
		    claim->key.leaf == range.leaf &&
		    claim->key.volume_first < range.last &&
		    range.first < claim->key.volume_last) {
			error = EBUSY;
			goto out;
		}

		/* Any published extent inside the volume conflicts. */
		for (j = 0; j < claim->range_count; j++) {
			if (range_overlap(&range, &claim->ranges[j])) {
				error = EBUSY;
				goto out;
			}
		}
	}

	error = 0;

out:
	spin_unlock_irqrestore(&claim_lock, irq);

	/* Reports the mount verdict. */
	return error;
}

/*
 * Checks whether a disk may be torn down.
 *
 * Teardown has exactly the conflicts of a writable mount.
 */
int
backing_claim_check_teardown(
	struct disk *disk)
{
	int error;

	/* Reuses the writable-mount verdict. */
	error = backing_claim_check_mount(disk, 0);

	/* Reports the teardown verdict. */
	return error;
}

/* Identifies the executing context that owns nested mutations. */
static const void *
current_execution(
	void)
{
	struct thread *thread;

	/* Reads the current thread only while the scheduler provides one. */
	thread = NULL;
	if (thread_current != NULL)
		thread = thread_current();

	/* Uses the thread as the execution identity. */
	if (thread != NULL)
		return (const void *)thread;

	/* Falls back to the serial early-boot identity. */
	return (const void *)&early_boot_execution_token;
}

/* Resolves a disk range to one half-open range on a single leaf device. */
static int
canonical_range(
	struct disk *disk,
	uint64_t block,
	uint64_t count,
	struct backing_range *result)
{
	struct disk *first_leaf;
	struct disk *last_leaf;
	uint64_t first;
	uint64_t last;
	int error;

	/* Rejects an empty range or one that leaves the disk. */
	if (disk == NULL ||
	    result == NULL ||
	    count == 0 ||
	    block >= disk->d_block_count ||
	    count > disk->d_block_count - block)
		return EINVAL;

	/* Resolves the first block. */
	error = disk_resolve_range(disk, block, 1, &first_leaf, &first);
	if (error != 0)
		return error;

	/* Resolves the last block. */
	error = disk_resolve_range(disk, block + count - 1U, 1, &last_leaf, &last);
	if (error != 0)
		return error;

	/* Rejects a range that spans leaves or cannot be represented. */
	if (first_leaf != last_leaf || last < first || last == UINT64_MAX)
		return EIO;

	/* Publishes the half-open leaf range. */
	result->leaf = first_leaf;
	result->first = first;
	result->last = last + 1U;

	/* Reports the canonical range. */
	return 0;
}

/* Derives the backing object key of a FAT regular file. */
static int
inode_key(
	struct inode *inode,
	struct backing_object_key *key)
{
	struct backing_range volume;
	struct disk *disk;
	uint64_t object;
	int error;

	/* Rejects anything but a regular file. */
	if (inode == NULL || key == NULL || inode->i_type != INODE_REG)
		return EINVAL;

	/* Rejects a file without a disk-backed mount. */
	if (inode->i_mount == NULL || inode->i_mount->m_disk == NULL)
		return EOPNOTSUPP;

	/* Rejects a file outside FAT. */
	if (inode->i_mount->m_type != &fat_filesystem_type)
		return EOPNOTSUPP;

	/* Resolves the file identity through the FAT helper when present. */
	if (fat_file_backing_identity != NULL) {
		error = fat_file_backing_identity(inode, &disk, &object);
	} else {
		/*
		 * Compatibility for focused host fixtures that predate the FAT
		 * identity helper.  Production kernels always provide the helper.
		 */
		disk = inode->i_mount->m_disk;
		object = inode->i_ino;
		error = 0;
	}

	/* Propagates an identity failure. */
	if (error != 0)
		return error;

	/* Canonicalizes the whole volume that holds the file. */
	error = canonical_range(disk, 0, disk->d_block_count, &volume);
	if (error != 0)
		return error;

	/* Publishes the key. */
	key->leaf = volume.leaf;
	key->volume_first = volume.first;
	key->volume_last = volume.last;
	key->object = object;

	/* Reports the derived key. */
	return 0;
}

/* Tests whether two leaf ranges share at least one block. */
static int
range_overlap(
	const struct backing_range *left,
	const struct backing_range *right)
{
	/* Ranges on different leaves never overlap. */
	if (left->leaf != right->leaf)
		return 0;

	/* A range that ends before the other begins does not overlap. */
	if (left->first >= right->last)
		return 0;
	if (right->first >= left->last)
		return 0;

	/* Reports an overlap. */
	return 1;
}

/* Tests whether one leaf range lies completely inside another. */
static int
range_contains(
	const struct backing_range *outer,
	const struct backing_range *inner)
{
	/* Ranges on different leaves never contain each other. */
	if (outer->leaf != inner->leaf)
		return 0;

	/* Both ends of the inner range must stay inside the outer range. */
	if (outer->first > inner->first)
		return 0;
	if (inner->last > outer->last)
		return 0;

	/* Reports containment. */
	return 1;
}

/* Tests whether two leaf ranges are identical. */
static int
range_equal(
	const struct backing_range *left,
	const struct backing_range *right)
{
	/* Every field must match. */
	if (left->leaf != right->leaf)
		return 0;
	if (left->first != right->first)
		return 0;
	if (left->last != right->last)
		return 0;

	/* Reports equality. */
	return 1;
}

/* Tests whether one of a claim's published extents contains a range. */
static int
claim_contains_range(
	const struct backing_claim *claim,
	const struct backing_range *range)
{
	unsigned i;

	/* Searches the published extents for one that contains the range. */
	for (i = 0; i < claim->range_count; i++) {
		if (range_contains(&claim->ranges[i], range))
			return 1;
	}

	/* Reports that no extent contains the range. */
	return 0;
}

/* Tests whether two backing object keys name the same object. */
static int
key_equal(
	const struct backing_object_key *left,
	const struct backing_object_key *right)
{
	/* Every field must match. */
	if (left->leaf != right->leaf)
		return 0;
	if (left->volume_first != right->volume_first)
		return 0;
	if (left->volume_last != right->volume_last)
		return 0;
	if (left->object != right->object)
		return 0;

	/* Reports equality. */
	return 1;
}

/* Returns the whole-volume range recorded in a backing object key. */
static struct backing_range
key_volume(
	const struct backing_object_key *key)
{
	struct backing_range range;

	/* Copies the volume bounds. */
	range.leaf = key->leaf;
	range.first = key->volume_first;
	range.last = key->volume_last;

	/* Reports the volume range. */
	return range;
}

/* Stores a claim in the first free registry slot. */
static int
claim_insert(
	struct backing_claim *claim)
{
	unsigned index;

	/* Registers the claim in the first free slot. */
	for (index = 0; index < BACKING_CLAIM_MAX; index++) {
		if (claims[index] == NULL) {
			claim->pinned_leaf = claim->key_valid ? claim->key.leaf : claim->ranges[0].leaf;
			disk_ref(claim->pinned_leaf);
			claims[index] = claim;
			claim->registered = 1;
			return 0;
		}
	}

	/* Reports a full registry. */
	return ENOSPC;
}

/*
 * Reserves one mutation slot after checking it against every claim and
 * every reservation already in flight.
 */
static int
mutation_reserve(
	const struct backing_object_key *key,
	const struct backing_range *range,
	const struct backing_range *filesystem_volume,
	const struct backing_claim *owner,
	int filesystem,
	struct backing_mutation_guard *guard)
{
	const struct backing_claim *effective_owner;
	const struct backing_mutation *inherited;
	const void *execution;
	struct backing_claim *claim;
	struct backing_range effective_volume;
	struct backing_range volume;
	struct backing_range br;
	uint64_t selected_generation;
	unsigned free_slot;
	unsigned i;
	unsigned j;
	unsigned long irq;
	int effective_filesystem;
	int effective_volume_valid;
	int owner_registered;
	int error;

	/* Starts from the ownership the caller stated. */
	effective_owner = owner;
	inherited = NULL;
	execution = current_execution();
	free_slot = BACKING_MUTATION_MAX;
	effective_filesystem = filesystem;
	effective_volume_valid = filesystem_volume != NULL;
	owner_registered = 0;
	error = 0;
	if (filesystem_volume != NULL)
		effective_volume = *filesystem_volume;

	/* Rejects a missing guard. */
	if (guard == NULL)
		return EINVAL;

	memset(guard, 0, sizeof(*guard));

	irq = spin_lock_irqsave(&claim_lock);

	/*
	 * buf_writeback enters the direct-I/O layer below
	 * disk_write_filesystem().  Its cache-line write can be wider
	 * than the initiating sector, so inherit the newest
	 * overlapping filesystem range from this execution.  A raw
	 * write cannot borrow an unrelated inode claim merely because
	 * it runs on the same thread.
	 */
	if (range != NULL && !filesystem && owner == NULL) {
		for (i = 0; i < BACKING_MUTATION_MAX; i++) {
			if (!mutations[i].used ||
			    !mutations[i].filesystem ||
			    !mutations[i].range_valid ||
			    mutations[i].execution != execution)
				continue;
			if (!range_overlap(&mutations[i].range, range))
				continue;
			if (inherited != NULL &&
			    mutations[i].generation <= inherited->generation)
				continue;

			inherited = &mutations[i];
		}
	}

	/* Adopts the inherited filesystem ownership and volume. */
	if (inherited != NULL) {
		effective_filesystem = 1;
		effective_owner = inherited->owner;
		if (inherited->filesystem_volume_valid) {
			effective_volume = inherited->filesystem_volume;
			effective_volume_valid = 1;
		}
	}

	/*
	 * A loop backing write owns the claimed inode before entering
	 * FAT.  Carry that owner only into an explicitly marked
	 * filesystem write on the same canonical volume.
	 */
	if (effective_filesystem &&
	    effective_owner == NULL &&
	    range != NULL &&
	    effective_volume_valid) {
		selected_generation = 0;

		/* Selects the newest owned keyed mutation on the same volume. */
		for (i = 0; i < BACKING_MUTATION_MAX; i++) {
			if (!mutations[i].used ||
			    !mutations[i].key_valid ||
			    mutations[i].execution != execution ||
			    mutations[i].owner == NULL)
				continue;

			br.leaf = mutations[i].key.leaf;
			br.first = mutations[i].key.volume_first;
			br.last = mutations[i].key.volume_last;
			if (!range_equal(&br, &effective_volume))
				continue;
			if (mutations[i].generation <= selected_generation)
				continue;

			effective_owner = mutations[i].owner;
			selected_generation = mutations[i].generation;
		}
	}

	/* Validates the owner against its own registered claim. */
	if (effective_owner != NULL) {
		/* The owner must still be registered. */
		for (i = 0; i < BACKING_CLAIM_MAX; i++) {
			if (claims[i] == effective_owner) {
				owner_registered = 1;
				break;
			}
		}
		if (!owner_registered) {
			error = EINVAL;
			goto out;
		}

		/* A keyed mutation must name the owner's own file. */
		if (key != NULL &&
		    (!effective_owner->key_valid ||
		     !key_equal(key, &effective_owner->key))) {
			error = EBUSY;
			goto out;
		}

		/*
		 * A directly supplied owner authorizes only its
		 * published extents.  Filesystem ownership inferred
		 * from an inode mutation is intentionally broader
		 * because FAT must also update allocation metadata.
		 */
		if (range != NULL &&
		    owner != NULL &&
		    !claim_contains_range(effective_owner, range)) {
			error = EBUSY;
			goto out;
		}

		/* A filesystem write must stay inside the owner's volume. */
		br.leaf = effective_owner->key.leaf;
		br.first = effective_owner->key.volume_first;
		br.last = effective_owner->key.volume_last;
		if (range != NULL &&
		    effective_filesystem &&
		    effective_owner->key_valid &&
		    !range_contains(&br, range)) {
			error = EBUSY;
			goto out;
		}
	}

	/* Rejects the mutation when it aliases any other registered claim. */
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		claim = claims[i];
		if (claim == NULL || claim == effective_owner)
			continue;

		/* A keyed mutation of another claim's file conflicts. */
		if (key != NULL &&
		    claim->key_valid &&
		    key_equal(key, &claim->key)) {
			error = EBUSY;
			goto out;
		}

		/* A raw range is checked against the claim's volume and extents. */
		if (claim->key_valid && range != NULL) {
			volume = key_volume(&claim->key);

			/*
			 * Truly unowned raw aliases may change FAT
			 * allocation metadata, not merely the
			 * requested data sectors, so reject their
			 * complete volume.  An explicitly supplied
			 * owner was already restricted to its own
			 * published extents above; let that direct
			 * I/O coexist with disjoint inode claims
			 * while still rejecting another claim's exact
			 * data extents.  Trusted filesystem writes
			 * have the same exact- extent exclusion while
			 * retaining their metadata-update latitude.
			 */
			if (range_overlap(range, &volume) &&
			    !effective_filesystem &&
			    effective_owner == NULL) {
				error = EBUSY;
				goto out;
			}

			for (j = 0; j < claim->range_count; j++) {
				if (range_overlap(range, &claim->ranges[j])) {
					error = EBUSY;
					goto out;
				}
			}
		} else if (range != NULL) {
			for (j = 0; j < claim->range_count; j++) {
				if (range_overlap(range, &claim->ranges[j])) {
					error = EBUSY;
					goto out;
				}
			}
		}

		/* A keyed mutation conflicts with a raw claim inside its volume. */
		if (key != NULL && !claim->key_valid) {
			for (j = 0; j < claim->range_count; j++) {
				volume = key_volume(key);
				if (range_overlap(&volume, &claim->ranges[j])) {
					error = EBUSY;
					goto out;
				}
			}
		}
	}

	/* Finds a free reservation slot. */
	for (i = 0; i < BACKING_MUTATION_MAX; i++) {
		if (!mutations[i].used) {
			free_slot = i;
			break;
		}
	}
	if (free_slot == BACKING_MUTATION_MAX) {
		error = EAGAIN;
		goto out;
	}

	/* Records the effective ownership of the reservation. */
	mutations[free_slot].used = 1;
	mutations[free_slot].owner = effective_owner;
	mutations[free_slot].execution = execution;
	mutations[free_slot].filesystem = effective_filesystem;
	if (effective_volume_valid) {
		mutations[free_slot].filesystem_volume = effective_volume;
		mutations[free_slot].filesystem_volume_valid = 1;
	}

	/* Assigns a nonzero generation so a stale guard cannot match. */
	mutations[free_slot].generation = ++mutation_generation;
	if (mutations[free_slot].generation == 0)
		mutations[free_slot].generation = ++mutation_generation;

	/* Records the mutated object or range. */
	if (key != NULL) {
		mutations[free_slot].key = *key;
		mutations[free_slot].key_valid = 1;
		mutations[free_slot].range = key_volume(key);
		mutations[free_slot].range_valid = 1;
	} else if (range != NULL) {
		mutations[free_slot].range = *range;
		mutations[free_slot].range_valid = 1;
	}

	/* Hands the slot to the guard. */
	guard->slot = free_slot;
	guard->generation = mutations[free_slot].generation;
	guard->active = 1;

out:
	spin_unlock_irqrestore(&claim_lock, irq);

	/* Reports the reservation result. */
	return error;
}
