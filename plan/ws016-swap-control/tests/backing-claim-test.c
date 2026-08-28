/* Production-linked SWAP-T003/T004 backing-claim fixture. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kern/backing-claim.h>
#include <kern/disk.h>
#include <kern/fat-vfs.h>
#include <kern/inode.h>
#include <kern/mount.h>

struct thread;

const struct filesystem_type fat_filesystem_type = {
    .fs_name = "fat",
};

static _Thread_local int execution_token;
static _Thread_local int null_execution;
static struct disk *writable_mount_disk;
static pthread_mutex_t claim_test_lock = PTHREAD_MUTEX_INITIALIZER;

struct thread *
thread_current(void)
{
	return null_execution ? NULL : (struct thread *)&execution_token;
}

int
mount_disk_writable_busy(struct disk *disk)
{
	return disk == writable_mount_disk ? EBUSY : 0;
}

int
fat_file_backing_identity(struct inode *inode, struct disk **disk,
			  uint64_t *object)
{
	if (inode == NULL || inode->i_mount == NULL ||
	    inode->i_mount->m_disk == NULL || disk == NULL || object == NULL)
		return EINVAL;
	*disk = inode->i_mount->m_disk;
	*object = inode->i_ino;
	return 0;
}

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	assert(pthread_mutex_lock(&claim_test_lock) == 0);
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long state)
{
	(void)lock;
	(void)state;
	assert(pthread_mutex_unlock(&claim_test_lock) == 0);
}

int
disk_resolve_range(struct disk *disk, uint64_t block, uint32_t count,
		   struct disk **leaf_out, uint64_t *mapped_out)
{
	uint64_t mapped = block;
	struct disk *leaf = disk;

	if (disk == NULL || leaf_out == NULL || mapped_out == NULL ||
	    count == 0 || block >= disk->d_block_count ||
	    count > disk->d_block_count - block)
		return EINVAL;
	while (leaf->d_parent != NULL) {
		mapped += leaf->d_parent_offset;
		leaf = leaf->d_parent;
	}
	if (mapped >= leaf->d_block_count ||
	    count > leaf->d_block_count - mapped)
		return EOVERFLOW;
	*leaf_out = leaf;
	*mapped_out = mapped;
	return 0;
}

static void
make_disk(struct disk *disk, uint64_t blocks, struct disk *parent,
	  uint64_t offset)
{
	memset(disk, 0, sizeof(*disk));
	disk->d_block_size = 512;
	disk->d_block_count = blocks;
	disk->d_parent = parent;
	disk->d_parent_offset = offset;
}

static void
make_inode(struct inode *inode, struct mount *mountp, ino_t number)
{
	memset(inode, 0, sizeof(*inode));
	inode->i_type = INODE_REG;
	inode->i_mount = mountp;
	inode->i_ino = number;
	inode->i_size = 4096 * 4;
}

struct mutation_race {
	struct disk *disk;
	pthread_mutex_t lock;
	pthread_cond_t condition;
	int held;
	int release;
	int error;
};

static void *
hold_whole_disk_mutation(void *argument)
{
	struct mutation_race *race = argument;
	struct backing_mutation_guard guard;

	race->error = backing_mutation_begin_disk(
	    race->disk, 0, race->disk->d_block_count, NULL, &guard);
	assert(pthread_mutex_lock(&race->lock) == 0);
	race->held = 1;
	assert(pthread_cond_signal(&race->condition) == 0);
	while (!race->release)
		assert(pthread_cond_wait(&race->condition, &race->lock) == 0);
	assert(pthread_mutex_unlock(&race->lock) == 0);
	if (race->error == 0)
		backing_mutation_end(&guard);
	return NULL;
}

int
main(void)
{
	struct disk leaf, partition, disjoint;
	struct mount mount_a, mount_b, mount_parent;
	struct inode inode_a, inode_alias, inode_other, inode_parent;
	struct inode inode_runtime;
	struct backing_claim *file_claim = NULL, *overlap = NULL;
	struct backing_claim *raw_claim = NULL, *second_claim = NULL;
	struct backing_claim *parent_claim = NULL;
	struct backing_claim *loop_claim0 = NULL, *loop_claim1 = NULL;
	struct backing_claim *runtime_claim = NULL;
	struct backing_claim_extent extent;
	struct backing_mutation_guard guard;
	struct backing_mutation_guard inode_guard, filesystem_guard;
	struct backing_mutation_guard second_inode_guard;
	struct mutation_race race;
	pthread_t race_thread;

	/* A PREPARING claim excludes raw writes but permits lower disk writes
	 * belonging to an already accepted, unrelated inode mutation.
	 */
	make_disk(&leaf, 4096, NULL, 0);
	make_disk(&partition, 1000, &leaf, 100);
	memset(&mount_a, 0, sizeof(mount_a));
	mount_a.m_disk = &partition;
	mount_a.m_type = &fat_filesystem_type;
	make_inode(&inode_a, &mount_a, 77);
	make_inode(&inode_other, &mount_a, 78);
	assert(backing_claim_prepare_inode(&inode_a, BACKING_CLAIM_SWAP,
					   &file_claim) == 0);
	assert(backing_mutation_begin_disk(&partition, 40, 1, NULL, &guard) ==
	       EBUSY);
	assert(backing_mutation_begin_inode(&inode_other, &inode_guard) == 0);
	assert(backing_mutation_begin_disk_filesystem(&partition, 40, 1,
							 &filesystem_guard) == 0);
	backing_mutation_end(&filesystem_guard);
	backing_mutation_end(&inode_guard);
	backing_claim_release(file_claim);
	file_claim = NULL;

	make_disk(&leaf, 4096, NULL, 0);
	make_disk(&partition, 1000, &leaf, 100);
	make_disk(&disjoint, 500, &leaf, 1500);
	memset(&mount_a, 0, sizeof(mount_a));
	memset(&mount_b, 0, sizeof(mount_b));
	mount_a.m_disk = mount_b.m_disk = &partition;
	mount_a.m_type = mount_b.m_type = &fat_filesystem_type;
	make_inode(&inode_a, &mount_a, 77);
	make_inode(&inode_alias, &mount_b, 77);
	make_inode(&inode_other, &mount_b, 78);

	assert(backing_claim_prepare_inode(&inode_a, BACKING_CLAIM_SWAP,
					   &file_claim) == 0);
	extent.disk = &partition;
	extent.block = 20;
	extent.block_count = 8;
	assert(backing_claim_finalize(file_claim, &extent, 1) == 0);

	assert(backing_mutation_begin_inode(&inode_alias, &guard) == EBUSY);
	assert(backing_mutation_begin_inode(&inode_other, &guard) == 0);
	backing_mutation_end(&guard);
	assert(backing_mutation_begin_disk(&leaf, 120, 1, NULL, &guard) ==
	       EBUSY);
	/* Raw aliases anywhere in the containing FAT volume can alter allocation
	 * metadata and therefore remain excluded, even off the data extent.
	 */
	assert(backing_mutation_begin_disk(&leaf, 119, 1, NULL, &guard) ==
	       EBUSY);
	assert(backing_mutation_begin_disk(&leaf, 99, 1, NULL, &guard) == 0);
	backing_mutation_end(&guard);
	/* Trusted filesystem writes may update unrelated metadata, but not the
	 * exact claimed data sectors without matching execution ownership.
	 */
	assert(backing_mutation_begin_disk_filesystem(&partition, 19, 1,
							 &filesystem_guard) == 0);
	backing_mutation_end(&filesystem_guard);
	assert(backing_mutation_begin_disk_filesystem(&partition, 20, 1,
							 &filesystem_guard) == EBUSY);
	assert(backing_mutation_begin_disk(&partition, 20, 8, file_claim,
					   &guard) == 0);
	backing_mutation_end(&guard);
	assert(backing_mutation_begin_disk(&partition, 19, 1, file_claim,
					   &guard) == EBUSY);
	assert(backing_mutation_begin_inode_claimed(&inode_a, file_claim,
						    &inode_guard) == 0);
	/* A raw path cannot borrow inode ownership merely by sharing a thread. */
	assert(backing_mutation_begin_disk(&partition, 20, 1, NULL, &guard) ==
	       EBUSY);
	assert(backing_mutation_begin_disk_filesystem(&partition, 20, 1,
							 &filesystem_guard) == 0);
	assert(backing_mutation_begin_disk(&partition, 19, 1, file_claim,
					   &guard) == EBUSY);
	/* Buffer writeback enters the raw direct-I/O layer and inherits only the
	 * enclosing filesystem context and its owned claim.
	 */
	assert(backing_mutation_begin_disk(&partition, 20, 1, NULL, &guard) == 0);
	backing_mutation_end(&guard);
	backing_mutation_end(&filesystem_guard);
	backing_mutation_end(&inode_guard);

	/* Early boot has no thread object, but its serial execution still carries
	 * ownership across the same inode -> filesystem nesting.
	 */
	null_execution = 1;
	assert(backing_mutation_begin_inode_claimed(&inode_a, file_claim,
						    &inode_guard) == 0);
	assert(backing_mutation_begin_disk_filesystem(&partition, 20, 1,
							 &filesystem_guard) == 0);
	backing_mutation_end(&filesystem_guard);
	backing_mutation_end(&inode_guard);
	null_execution = 0;
	assert(backing_claim_check_mount(&partition, 0) == EBUSY);
	assert(backing_claim_check_mount(&partition, MOUNT_READ_ONLY) == 0);
	assert(backing_claim_check_teardown(&partition) == EBUSY);

	assert(backing_claim_prepare_inode(&inode_other, BACKING_CLAIM_SWAP,
					   &overlap) == 0);
	assert(backing_claim_finalize(overlap, &extent, 1) == EBUSY);
	backing_claim_release(overlap);
	overlap = NULL;
	/* Finalized extents must remain within the claimed inode's volume. */
	assert(backing_claim_prepare_inode(&inode_other, BACKING_CLAIM_LOOP,
					   &overlap) == 0);
	extent.disk = &leaf;
	extent.block = 99;
	extent.block_count = 1;
	assert(backing_claim_finalize(overlap, &extent, 1) == EXDEV);
	backing_claim_release(overlap);
	overlap = NULL;

	/* When an execution owns several claims on one volume, the innermost
	 * (greatest-generation) inode mutation supplies filesystem ownership.
	 */
	extent.disk = &partition;
	extent.block = 40;
	extent.block_count = 8;
	assert(backing_claim_prepare_inode(&inode_other, BACKING_CLAIM_LOOP,
					   &second_claim) == 0);
	assert(backing_claim_finalize(second_claim, &extent, 1) == 0);
	assert(backing_mutation_begin_inode_claimed(&inode_a, file_claim,
						    &inode_guard) == 0);
	assert(backing_mutation_begin_inode_claimed(&inode_other, second_claim,
						    &second_inode_guard) == 0);
	assert(backing_mutation_begin_disk_filesystem(&partition, 40, 1,
							 &filesystem_guard) == 0);
	backing_mutation_end(&filesystem_guard);
	backing_mutation_end(&second_inode_guard);
	backing_mutation_end(&inode_guard);
	backing_claim_release(second_claim);
	second_claim = NULL;

	assert(backing_claim_prepare_disk(&partition, 20, 8, BACKING_CLAIM_SWAP,
					  &raw_claim) == EBUSY);
	writable_mount_disk = &disjoint;
	assert(backing_claim_prepare_disk(&disjoint, 0, disjoint.d_block_count,
					  BACKING_CLAIM_SWAP,
					  &raw_claim) == EBUSY);
	writable_mount_disk = NULL;
	assert(backing_claim_prepare_disk(&disjoint, 0, disjoint.d_block_count,
					  BACKING_CLAIM_SWAP, &raw_claim) == 0);
	assert(backing_mutation_begin_disk(&disjoint, 0, 1, NULL, &guard) ==
	       EBUSY);
	backing_claim_release(raw_claim);

	backing_claim_release(file_claim);
	assert(backing_mutation_begin_inode(&inode_alias, &guard) == 0);
	backing_mutation_end(&guard);
	assert(backing_mutation_begin_disk(&leaf, 120, 1, NULL, &guard) == 0);
	backing_mutation_end(&guard);
	assert(backing_claim_check_mount(&partition, 0) == 0);

	/* Runtime ordering differs from boot activation: the root/data loop
	 * claims already exist when swapon publishes a third, disjoint inode
	 * claim.  Direct swap I/O carrying that owner must remain confined to its
	 * own exact extents, but must not be rejected merely because the two loop
	 * claims occupy the same FAT volume.
	 */
	make_inode(&inode_a, &mount_a, 201);
	make_inode(&inode_other, &mount_a, 202);
	make_inode(&inode_runtime, &mount_a, 203);
	extent.disk = &partition;
	extent.block = 20;
	extent.block_count = 8;
	assert(backing_claim_prepare_inode(&inode_a, BACKING_CLAIM_LOOP,
					   &loop_claim0) == 0);
	assert(backing_claim_finalize(loop_claim0, &extent, 1) == 0);
	extent.block = 40;
	assert(backing_claim_prepare_inode(&inode_other, BACKING_CLAIM_LOOP,
					   &loop_claim1) == 0);
	assert(backing_claim_finalize(loop_claim1, &extent, 1) == 0);
	extent.block = 60;
	assert(backing_claim_prepare_inode(&inode_runtime, BACKING_CLAIM_SWAP,
					   &runtime_claim) == 0);
	assert(backing_claim_finalize(runtime_claim, &extent, 1) == 0);
	assert(backing_mutation_begin_disk(&partition, 60, 8, runtime_claim,
					   &guard) == 0);
	backing_mutation_end(&guard);
	/* An owner cannot authorize a write into a sibling loop extent. */
	assert(backing_mutation_begin_disk(&partition, 20, 1, runtime_claim,
					   &guard) == EBUSY);
	/* Truly unowned raw access remains volume-wide excluded, both away from
	 * and directly on a claimed extent.
	 */
	assert(backing_mutation_begin_disk(&partition, 80, 1, NULL, &guard) ==
	       EBUSY);
	assert(backing_mutation_begin_disk(&partition, 60, 1, NULL, &guard) ==
	       EBUSY);
	backing_claim_release(runtime_claim);
	backing_claim_release(loop_claim1);
	backing_claim_release(loop_claim0);
	runtime_claim = loop_claim1 = loop_claim0 = NULL;

	/* An owned inode on an overlapping parent disk must not authorize a
	 * filesystem write through a different canonical volume alias.
	 */
	memset(&mount_parent, 0, sizeof(mount_parent));
	mount_parent.m_disk = &leaf;
	mount_parent.m_type = &fat_filesystem_type;
	make_inode(&inode_parent, &mount_parent, 99);
	assert(backing_claim_prepare_inode(&inode_parent, BACKING_CLAIM_LOOP,
					   &parent_claim) == 0);
	extent.disk = &leaf;
	extent.block = 120;
	extent.block_count = 1;
	assert(backing_claim_finalize(parent_claim, &extent, 1) == 0);
	assert(backing_mutation_begin_inode_claimed(&inode_parent, parent_claim,
						    &inode_guard) == 0);
	assert(backing_mutation_begin_disk_filesystem(&partition, 20, 1,
							 &filesystem_guard) == EBUSY);
	backing_mutation_end(&inode_guard);
	backing_claim_release(parent_claim);
	parent_claim = NULL;

	/* A whole-range mount/teardown reservation and claim publication are
	 * serialized by the backing registry, independent of scan timing.
	 */
	memset(&race, 0, sizeof(race));
	race.disk = &partition;
	assert(pthread_mutex_init(&race.lock, NULL) == 0);
	assert(pthread_cond_init(&race.condition, NULL) == 0);
	assert(pthread_create(&race_thread, NULL, hold_whole_disk_mutation,
			      &race) == 0);
	assert(pthread_mutex_lock(&race.lock) == 0);
	while (!race.held)
		assert(pthread_cond_wait(&race.condition, &race.lock) == 0);
	assert(race.error == 0);
	assert(pthread_mutex_unlock(&race.lock) == 0);
	assert(backing_claim_prepare_disk(&partition, 0, partition.d_block_count,
					  BACKING_CLAIM_SWAP,
					  &raw_claim) == EBUSY);
	assert(pthread_mutex_lock(&race.lock) == 0);
	race.release = 1;
	assert(pthread_cond_signal(&race.condition) == 0);
	assert(pthread_mutex_unlock(&race.lock) == 0);
	assert(pthread_join(race_thread, NULL) == 0);
	assert(pthread_cond_destroy(&race.condition) == 0);
	assert(pthread_mutex_destroy(&race.lock) == 0);
	assert(backing_claim_prepare_disk(&partition, 0, partition.d_block_count,
					  BACKING_CLAIM_SWAP,
					  &raw_claim) == 0);
	backing_claim_release(raw_claim);

	puts("SWAP-T003/T004 backing claims: PASS");
	return 0;
}
