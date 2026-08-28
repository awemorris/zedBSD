/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/backing-claim.h>

#include <kern/disk.h>
#include <kern/fat-vfs.h>
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

static struct backing_claim *claims[BACKING_CLAIM_MAX];
static struct backing_mutation mutations[BACKING_MUTATION_MAX];
static uint64_t mutation_generation;
static struct spinlock claim_lock = {
    {0}, LOCK_RANK_BACKING_CLAIM, "backing claims", 0, 0};

/* Host fixtures and early boot may not provide a current thread. */
struct thread;
extern struct thread *thread_current(void) __attribute__((weak));
extern int fat_file_backing_identity(struct inode *, struct disk **, uint64_t *)
    __attribute__((weak));

/* Before the scheduler publishes a current thread, the kernel executes boot
 * filesystem I/O serially.  Give that execution context a stable identity so
 * nested FAT -> buffer-cache -> direct-I/O mutations can retain ownership.
 */
static const unsigned char early_boot_execution_token;

static const void *
current_execution(void)
{
	struct thread *thread = thread_current != NULL ? thread_current() : NULL;

	return thread != NULL ? (const void *)thread
			      : (const void *)&early_boot_execution_token;
}

static int
canonical_range(struct disk *disk, uint64_t block, uint64_t count,
		struct backing_range *result)
{
	struct disk *first_leaf, *last_leaf;
	uint64_t first, last;
	int error;

	if (disk == NULL || result == NULL || count == 0 ||
	    block >= disk->d_block_count || count > disk->d_block_count - block)
		return EINVAL;
	error = disk_resolve_range(disk, block, 1, &first_leaf, &first);
	if (error != 0)
		return error;
	error =
	    disk_resolve_range(disk, block + count - 1U, 1, &last_leaf, &last);
	if (error != 0)
		return error;
	if (first_leaf != last_leaf || last < first || last == UINT64_MAX)
		return EIO;
	result->leaf = first_leaf;
	result->first = first;
	result->last = last + 1U;
	return 0;
}

static int
inode_key(struct inode *inode, struct backing_object_key *key)
{
	struct backing_range volume;
	struct disk *disk;
	uint64_t object;
	int error;

	if (inode == NULL || key == NULL || inode->i_type != INODE_REG)
		return EINVAL;
	if (inode->i_mount == NULL || inode->i_mount->m_disk == NULL)
		return EOPNOTSUPP;
	if (inode->i_mount->m_type != &fat_filesystem_type)
		return EOPNOTSUPP;
	if (fat_file_backing_identity != NULL)
		error = fat_file_backing_identity(inode, &disk, &object);
	else {
		/* Compatibility for focused host fixtures that predate the FAT
		 * identity helper. Production kernels always provide the
		 * helper.
		 */
		disk = inode->i_mount->m_disk;
		object = inode->i_ino;
		error = 0;
	}
	if (error != 0)
		return error;
	error = canonical_range(disk, 0, disk->d_block_count, &volume);
	if (error != 0)
		return error;
	key->leaf = volume.leaf;
	key->volume_first = volume.first;
	key->volume_last = volume.last;
	key->object = object;
	return 0;
}

static int
range_overlap(const struct backing_range *left,
	      const struct backing_range *right)
{
	return left->leaf == right->leaf && left->first < right->last &&
	       right->first < left->last;
}

static int
range_contains(const struct backing_range *outer,
	       const struct backing_range *inner)
{
	return outer->leaf == inner->leaf && outer->first <= inner->first &&
	       inner->last <= outer->last;
}

static int
range_equal(const struct backing_range *left,
	    const struct backing_range *right)
{
	return left->leaf == right->leaf && left->first == right->first &&
	       left->last == right->last;
}

static int
claim_contains_range(const struct backing_claim *claim,
		     const struct backing_range *range)
{
	unsigned i;

	for (i = 0; i < claim->range_count; i++)
		if (range_contains(&claim->ranges[i], range))
			return 1;
	return 0;
}

static int
key_equal(const struct backing_object_key *left,
	  const struct backing_object_key *right)
{
	return left->leaf == right->leaf &&
	       left->volume_first == right->volume_first &&
	       left->volume_last == right->volume_last &&
	       left->object == right->object;
}

static struct backing_range
key_volume(const struct backing_object_key *key)
{
	struct backing_range range;
	range.leaf = key->leaf;
	range.first = key->volume_first;
	range.last = key->volume_last;
	return range;
}

static int
claim_insert(struct backing_claim *claim)
{
	unsigned index;
	for (index = 0; index < BACKING_CLAIM_MAX; index++)
		if (claims[index] == NULL) {
			claims[index] = claim;
			claim->registered = 1;
			return 0;
		}
	return ENOSPC;
}

int
backing_claim_prepare_inode(struct inode *inode, enum backing_claim_owner owner,
			    struct backing_claim **result)
{
	struct backing_claim *claim;
	struct backing_range volume;
	unsigned i, j;
	unsigned long irq;
	int error;

	if (result == NULL ||
	    (owner != BACKING_CLAIM_SWAP && owner != BACKING_CLAIM_LOOP))
		return EINVAL;
	*result = NULL;
	claim = kern_calloc(1, sizeof(*claim));
	if (claim == NULL)
		return ENOMEM;
	error = inode_key(inode, &claim->key);
	if (error != 0) {
		kern_free(claim);
		return error;
	}
	claim->owner = owner;
	claim->key_valid = 1;
	claim->preparing = 1;
	volume = key_volume(&claim->key);
	irq = spin_lock_irqsave(&claim_lock);
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		struct backing_claim *existing = claims[i];
		if (existing == NULL)
			continue;
		if ((existing->key_valid &&
		     key_equal(&claim->key, &existing->key)) ||
		    (existing->preparing && existing->key_valid &&
		     range_overlap(&volume, &(struct backing_range){
						existing->key.leaf,
						existing->key.volume_first,
						existing->key.volume_last}))) {
			error = EBUSY;
			goto out_locked;
		}
		for (j = 0; j < existing->range_count; j++)
			if (!existing->key_valid &&
			    range_overlap(&volume, &existing->ranges[j])) {
				error = EBUSY;
				goto out_locked;
			}
	}
	for (i = 0; i < BACKING_MUTATION_MAX; i++) {
		if (!mutations[i].used)
			continue;
		if ((mutations[i].key_valid &&
		     key_equal(&claim->key, &mutations[i].key)) ||
		    (!mutations[i].key_valid && mutations[i].range_valid &&
		     range_overlap(&volume, &mutations[i].range))) {
			error = EBUSY;
			goto out_locked;
		}
	}
	error = claim_insert(claim);
out_locked:
	spin_unlock_irqrestore(&claim_lock, irq);
	if (error != 0) {
		kern_free(claim);
		return error;
	}
	*result = claim;
	return 0;
}

int
backing_claim_finalize(struct backing_claim *claim,
		       const struct backing_claim_extent *extents,
		       unsigned count)
{
	struct backing_range *ranges = NULL;
	unsigned i, j, k;
	unsigned long irq;
	int error = 0;

	if (claim == NULL || !claim->registered || !claim->preparing ||
	    (count != 0 && extents == NULL))
		return EINVAL;
	if (count != 0) {
		ranges = kern_calloc(count, sizeof(*ranges));
		if (ranges == NULL)
			return ENOMEM;
		for (i = 0; i < count; i++) {
			error =
			    canonical_range(extents[i].disk, extents[i].block,
					    extents[i].block_count, &ranges[i]);
			if (error != 0 || ranges[i].leaf != claim->key.leaf ||
			    ranges[i].first < claim->key.volume_first ||
			    ranges[i].last > claim->key.volume_last) {
				if (error == 0)
					error = EXDEV;
				kern_free(ranges);
				return error;
			}
		}
	}
	irq = spin_lock_irqsave(&claim_lock);
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		struct backing_claim *existing = claims[i];
		if (existing == NULL || existing == claim)
			continue;
		for (j = 0; j < count; j++)
			for (k = 0; k < existing->range_count; k++)
				if (range_overlap(&ranges[j],
						  &existing->ranges[k])) {
					error = EBUSY;
					goto out_locked;
				}
	}
	for (i = 0; i < BACKING_MUTATION_MAX; i++) {
		if (!mutations[i].used || mutations[i].key_valid ||
		    !mutations[i].range_valid)
			continue;
		for (j = 0; j < count; j++)
			if (range_overlap(&ranges[j], &mutations[i].range)) {
				error = EBUSY;
				goto out_locked;
			}
	}
	claim->ranges = ranges;
	claim->range_count = count;
	claim->preparing = 0;
	ranges = NULL;
out_locked:
	spin_unlock_irqrestore(&claim_lock, irq);
	kern_free(ranges);
	return error;
}

int
backing_claim_prepare_disk(struct disk *disk, uint64_t block, uint64_t count,
			   enum backing_claim_owner owner,
			   struct backing_claim **result)
{
	struct backing_claim *claim;
	unsigned i, j;
	unsigned long irq;
	int error;

	if (result == NULL ||
	    (owner != BACKING_CLAIM_SWAP && owner != BACKING_CLAIM_LOOP))
		return EINVAL;
	*result = NULL;
	error = mount_disk_writable_busy(disk);
	if (error != 0)
		return error;
	claim = kern_calloc(1, sizeof(*claim));
	if (claim == NULL)
		return ENOMEM;
	claim->ranges = kern_calloc(1, sizeof(*claim->ranges));
	if (claim->ranges == NULL) {
		kern_free(claim);
		return ENOMEM;
	}
	error = canonical_range(disk, block, count, claim->ranges);
	if (error != 0) {
		kern_free(claim->ranges);
		kern_free(claim);
		return error;
	}
	claim->owner = owner;
	claim->range_count = 1;
	irq = spin_lock_irqsave(&claim_lock);
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		struct backing_claim *existing = claims[i];
		struct backing_range volume;
		if (existing == NULL)
			continue;
		if (existing->key_valid) {
			volume = key_volume(&existing->key);
			if (range_overlap(&claim->ranges[0], &volume)) {
				error = EBUSY;
				goto out_locked;
			}
		}
		for (j = 0; j < existing->range_count; j++)
			if (range_overlap(&claim->ranges[0],
					  &existing->ranges[j])) {
				error = EBUSY;
				goto out_locked;
			}
	}
	for (i = 0; i < BACKING_MUTATION_MAX; i++)
		if (mutations[i].used && mutations[i].range_valid &&
		    range_overlap(&claim->ranges[0], &mutations[i].range)) {
			error = EBUSY;
			goto out_locked;
		}
	error = claim_insert(claim);
out_locked:
	spin_unlock_irqrestore(&claim_lock, irq);
	if (error != 0) {
		kern_free(claim->ranges);
		kern_free(claim);
		return error;
	}
	/* Retain the LIVE-mount rescan as a defensive validation.  A writable
	 * mount that is still PREPARING holds a whole-volume mutation reservation,
	 * so claim insertion above either observes that reservation or precedes it.
	 */
	error = mount_disk_writable_busy(disk);
	if (error != 0) {
		backing_claim_release(claim);
		return error;
	}
	*result = claim;
	return 0;
}

void
backing_claim_release(struct backing_claim *claim)
{
	unsigned i;
	unsigned long irq;
	if (claim == NULL)
		return;
	irq = spin_lock_irqsave(&claim_lock);
	for (i = 0; i < BACKING_CLAIM_MAX; i++)
		if (claims[i] == claim) {
			claims[i] = NULL;
			claim->registered = 0;
			break;
		}
	spin_unlock_irqrestore(&claim_lock, irq);
	kern_free(claim->ranges);
	kern_free(claim);
}

static int
mutation_reserve(const struct backing_object_key *key,
		 const struct backing_range *range,
		 const struct backing_range *filesystem_volume,
		 const struct backing_claim *owner,
		 int filesystem,
		 struct backing_mutation_guard *guard)
{
	const struct backing_claim *effective_owner = owner;
	const struct backing_mutation *inherited = NULL;
	const void *execution = current_execution();
	struct backing_range effective_volume;
	unsigned free_slot = BACKING_MUTATION_MAX, i, j;
	unsigned long irq;
	int effective_filesystem = filesystem;
	int effective_volume_valid = filesystem_volume != NULL;
	int owner_registered = 0;
	int error = 0;

	if (filesystem_volume != NULL)
		effective_volume = *filesystem_volume;

	if (guard == NULL)
		return EINVAL;
	memset(guard, 0, sizeof(*guard));
	irq = spin_lock_irqsave(&claim_lock);
	/* buf_writeback enters the direct-I/O layer below disk_write_filesystem().
	 * Its cache-line write can be wider than the initiating sector, so inherit
	 * the newest overlapping filesystem range from this execution.  A raw
	 * write cannot borrow an unrelated inode claim merely because it runs on
	 * the same thread.
	 */
	if (range != NULL && !filesystem && owner == NULL)
		for (i = 0; i < BACKING_MUTATION_MAX; i++)
			if (mutations[i].used && mutations[i].filesystem &&
			    mutations[i].range_valid &&
			    mutations[i].execution == execution &&
			    range_overlap(&mutations[i].range, range) &&
			    (inherited == NULL || mutations[i].generation >
						  inherited->generation))
				inherited = &mutations[i];
	if (inherited != NULL) {
		effective_filesystem = 1;
		effective_owner = inherited->owner;
		if (inherited->filesystem_volume_valid) {
			effective_volume = inherited->filesystem_volume;
			effective_volume_valid = 1;
		}
	}
	/* A loop backing write owns the claimed inode before entering FAT.  Carry
	 * that owner only into an explicitly marked filesystem write on the same
	 * canonical volume.
	 */
	if (effective_filesystem && effective_owner == NULL && range != NULL &&
	    effective_volume_valid) {
		uint64_t selected_generation = 0;

		for (i = 0; i < BACKING_MUTATION_MAX; i++)
			if (mutations[i].used && mutations[i].key_valid &&
			    mutations[i].execution == execution &&
			    mutations[i].owner != NULL &&
			    range_equal(&(struct backing_range){
				mutations[i].key.leaf,
				mutations[i].key.volume_first,
				mutations[i].key.volume_last},
				&effective_volume) &&
			    mutations[i].generation > selected_generation) {
				effective_owner = mutations[i].owner;
				selected_generation = mutations[i].generation;
			}
	}
	if (effective_owner != NULL) {
		for (i = 0; i < BACKING_CLAIM_MAX; i++)
			if (claims[i] == effective_owner) {
				owner_registered = 1;
				break;
			}
		if (!owner_registered) {
			error = EINVAL;
			goto out;
		}
		if (key != NULL &&
		    (!effective_owner->key_valid ||
		     !key_equal(key, &effective_owner->key))) {
			error = EBUSY;
			goto out;
		}
		/* A directly supplied owner authorizes only its published extents.
		 * Filesystem ownership inferred from an inode mutation is intentionally
		 * broader because FAT must also update allocation metadata.
		 */
		if (range != NULL && owner != NULL &&
		    !claim_contains_range(effective_owner, range)) {
			error = EBUSY;
			goto out;
		}
		if (range != NULL && effective_filesystem &&
		    effective_owner->key_valid &&
		    !range_contains(&(struct backing_range){
			effective_owner->key.leaf,
			effective_owner->key.volume_first,
			effective_owner->key.volume_last}, range)) {
			error = EBUSY;
			goto out;
		}
	}
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		struct backing_claim *claim = claims[i];
		struct backing_range volume;
		if (claim == NULL || claim == effective_owner)
			continue;
		if (key != NULL && claim->key_valid &&
		    key_equal(key, &claim->key)) {
			error = EBUSY;
			goto out;
		}
		if (claim->key_valid && range != NULL) {
			volume = key_volume(&claim->key);
			/* Truly unowned raw aliases may change FAT allocation metadata,
			 * not merely the requested data sectors, so reject their complete
			 * volume.  An explicitly supplied owner was already restricted to
			 * its own published extents above; let that direct I/O coexist with
			 * disjoint inode claims while still rejecting another claim's exact
			 * data extents.  Trusted filesystem writes have the same exact-
			 * extent exclusion while retaining their metadata-update latitude.
			 */
			if (range_overlap(range, &volume) && !effective_filesystem &&
			    effective_owner == NULL) {
				error = EBUSY;
				goto out;
			}
			for (j = 0; j < claim->range_count; j++)
				if (range_overlap(range, &claim->ranges[j])) {
					error = EBUSY;
					goto out;
				}
		} else if (range != NULL)
			for (j = 0; j < claim->range_count; j++)
				if (range_overlap(range, &claim->ranges[j])) {
					error = EBUSY;
					goto out;
				}
		if (key != NULL && !claim->key_valid)
			for (j = 0; j < claim->range_count; j++) {
				volume = key_volume(key);
				if (range_overlap(&volume, &claim->ranges[j])) {
					error = EBUSY;
					goto out;
				}
			}
	}
	for (i = 0; i < BACKING_MUTATION_MAX; i++)
		if (!mutations[i].used) {
			free_slot = i;
			break;
		}
	if (free_slot == BACKING_MUTATION_MAX) {
		error = EAGAIN;
		goto out;
	}
	mutations[free_slot].used = 1;
	mutations[free_slot].owner = effective_owner;
	mutations[free_slot].execution = execution;
	mutations[free_slot].filesystem = effective_filesystem;
	if (effective_volume_valid) {
		mutations[free_slot].filesystem_volume = effective_volume;
		mutations[free_slot].filesystem_volume_valid = 1;
	}
	mutations[free_slot].generation = ++mutation_generation;
	if (mutations[free_slot].generation == 0)
		mutations[free_slot].generation = ++mutation_generation;
	if (key != NULL) {
		mutations[free_slot].key = *key;
		mutations[free_slot].key_valid = 1;
		mutations[free_slot].range = key_volume(key);
		mutations[free_slot].range_valid = 1;
	} else if (range != NULL) {
		mutations[free_slot].range = *range;
		mutations[free_slot].range_valid = 1;
	}
	guard->slot = free_slot;
	guard->generation = mutations[free_slot].generation;
	guard->active = 1;
out:
	spin_unlock_irqrestore(&claim_lock, irq);
	return error;
}

int
backing_mutation_begin_inode(struct inode *inode,
			     struct backing_mutation_guard *guard)
{
	return backing_mutation_begin_inode_claimed(inode, NULL, guard);
}

int
backing_mutation_begin_inode_claimed(struct inode *inode,
				     const struct backing_claim *owner,
				     struct backing_mutation_guard *guard)
{
	struct backing_object_key key;
	if (guard == NULL)
		return EINVAL;
	int error = inode_key(inode, &key);
	/* Swap files are FAT-only.  Other files retain their existing local
	 * INODE_LOOPFILE exclusion and cannot alias a FAT swap claim. */
	if (error == EOPNOTSUPP || error == EINVAL) {
		memset(guard, 0, sizeof(*guard));
		return 0;
	}
	return error != 0 ? error :
	       mutation_reserve(&key, NULL, NULL, owner, 0, guard);
}

int
backing_mutation_begin_disk(struct disk *disk, uint64_t block, uint64_t count,
			    const struct backing_claim *owner,
			    struct backing_mutation_guard *guard)
{
	struct backing_range range;
	int error = canonical_range(disk, block, count, &range);
	return error != 0 ? error
			  : mutation_reserve(NULL, &range, NULL, owner, 0, guard);
}

int
backing_mutation_begin_disk_filesystem(struct disk *disk, uint64_t block,
				       uint64_t count,
				       struct backing_mutation_guard *guard)
{
	struct backing_range range, volume;
	int error = canonical_range(disk, block, count, &range);
	if (error == 0)
		error = canonical_range(disk, 0, disk->d_block_count, &volume);
	return error != 0
		   ? error
		   : mutation_reserve(NULL, &range, &volume, NULL, 1, guard);
}

void
backing_mutation_end(struct backing_mutation_guard *guard)
{
	unsigned long irq;
	if (guard == NULL || !guard->active ||
	    guard->slot >= BACKING_MUTATION_MAX)
		return;
	irq = spin_lock_irqsave(&claim_lock);
	if (mutations[guard->slot].used &&
	    mutations[guard->slot].generation == guard->generation)
		memset(&mutations[guard->slot], 0,
		       sizeof(mutations[guard->slot]));
	spin_unlock_irqrestore(&claim_lock, irq);
	memset(guard, 0, sizeof(*guard));
}

int
backing_claim_check_disk(struct disk *disk, uint64_t block, uint64_t count,
			 const struct backing_claim *owner)
{
	struct backing_mutation_guard guard;
	int error =
	    backing_mutation_begin_disk(disk, block, count, owner, &guard);
	if (error == 0)
		backing_mutation_end(&guard);
	return error;
}

int
backing_claim_check_mount(struct disk *disk, unsigned flags)
{
	struct backing_range range;
	unsigned i, j;
	unsigned long irq;
	int error;
	if ((flags & MOUNT_READ_ONLY) != 0)
		return 0;
	error = canonical_range(disk, 0, disk->d_block_count, &range);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&claim_lock);
	for (i = 0; i < BACKING_CLAIM_MAX; i++) {
		struct backing_claim *claim = claims[i];
		if (claim == NULL)
			continue;
		if (claim->key_valid && claim->key.leaf == range.leaf &&
		    claim->key.volume_first < range.last &&
		    range.first < claim->key.volume_last) {
			error = EBUSY;
			goto out;
		}
		for (j = 0; j < claim->range_count; j++)
			if (range_overlap(&range, &claim->ranges[j])) {
				error = EBUSY;
				goto out;
			}
	}
	error = 0;
out:
	spin_unlock_irqrestore(&claim_lock, irq);
	return error;
}

int
backing_claim_check_teardown(struct disk *disk)
{
	return backing_claim_check_mount(disk, 0);
}
