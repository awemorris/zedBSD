/* Production-linked SWAP-T003/T004 backing-claim fixture. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kern/backing-claim.h>
#include <kern/disk.h>
#include <kern/fat.h>
#include <kern/inode.h>
#include <kern/mount.h>

struct thread;

static unsigned metadata_mode;
static unsigned metadata_calls;
static unsigned fail_next_allocation;

static int test_data_extents(struct file *file, file_extent_cb callback, void *context)
{ (void)file; (void)callback; (void)context; return EOPNOTSUPP; }

static int test_identity(struct inode *inode, struct disk **disk, uint64_t *object)
{ *disk = inode->i_mount->m_disk; *object = inode->i_ino; return 0; }

static int test_metadata_extents(struct file *file, file_metadata_extent_cb callback, void *context)
{
	int error;
	(void)file;
	metadata_calls++;
	if (metadata_mode == 0 || (metadata_mode == 4 && metadata_calls == 2) ||
	    (metadata_mode == 5 && metadata_calls == 1))
		return 0;
	if (metadata_mode == 6 || (metadata_mode == 9 && metadata_calls == 2))
		return EIO;
	error = callback(metadata_mode == 2 ? 20 : metadata_mode == 7 ? 1000 : 60, 8, context);
	if (error != 0)
		return error;
	if (metadata_mode == 3 || (metadata_mode == 10 && metadata_calls == 2))
		return callback(60, 8, context);
	return 0;
}

const struct filesystem_type drv_fat_filesystem_type = {
    .fs_name = "fat",
    .file_extents = test_data_extents,
    .file_metadata_extents = test_metadata_extents,
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
drv_fat_file_backing_identity(struct inode *inode, struct disk **disk,
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
	if (fail_next_allocation) {
		fail_next_allocation = 0;
		return NULL;
	}
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

static void
test_identity_and_snapshot_admission(void)
{
	struct disk leaf;
	struct disk partition;
	struct disk alias;
	struct mount mountp;
	struct mount other_mount;
	struct inode inode;
	struct inode other_inode;
	struct filesystem_type type;
	struct backing_claim *claim;
	struct backing_claim *second;
	struct backing_claim_extent data;
	struct backing_mutation_guard guard;
	int matches;

	make_disk(&leaf, 4096, NULL, 0);
	make_disk(&partition, 1000, &leaf, 100);
	make_disk(&alias, 1000, &leaf, 100);
	memset(&type, 0, sizeof(type));
	type.fs_name = "identity-provider";
	type.file_backing_identity = test_identity;
	memset(&mountp, 0, sizeof(mountp));
	mountp.m_disk = &partition;
	mountp.m_type = &type;
	other_mount = mountp;
	other_mount.m_disk = &alias;
	make_inode(&inode, &mountp, 300);
	make_inode(&other_inode, &other_mount, 300);
	assert(backing_mutation_begin_disk(&partition, 0, 1000, NULL, &guard) == 0);
	assert(backing_claim_prepare_inode(&other_inode, BACKING_CLAIM_SWAP, &claim) == EBUSY);
	backing_mutation_end(&guard);
	assert(backing_claim_prepare_inode(&inode, BACKING_CLAIM_SWAP, &claim) == 0);
	assert(backing_claim_inode_matches(claim, &other_inode, &matches) == 0 && matches);
	assert(backing_claim_prepare_inode(&other_inode, BACKING_CLAIM_SWAP, &second) == EBUSY);
	assert(backing_mutation_begin_disk(&alias, 0, 1000, NULL, &guard) == EBUSY);
	data.disk = &partition;
	data.block = 20;
	data.block_count = 8;
	assert(backing_claim_finalize(claim, &data, 1) == 0);
	assert(backing_mutation_begin_disk(&alias, 0, 1000, NULL, &guard) == EBUSY);
	backing_claim_release(claim);
	assert(backing_mutation_begin_disk(&alias, 0, 1000, NULL, &guard) == 0);
	backing_mutation_end(&guard);
	assert(refcount_load(&leaf.d_refs) == 0);
	puts("canonical identity and snapshot admission orders: PASS");
}

static void
test_file_metadata(void)
{
	struct disk leaf;
	struct disk partition;
	struct mount mountp;
	struct inode inode;
	struct inode other;
	struct file file;
	struct backing_claim *claim;
	struct backing_claim_extent data;
	struct backing_mutation_guard guard;
	unsigned mode;
	int error;

	make_disk(&leaf, 4096, NULL, 0);
	make_disk(&partition, 1000, &leaf, 100);
	memset(&mountp, 0, sizeof(mountp));
	mountp.m_disk = &partition;
	mountp.m_type = &drv_fat_filesystem_type;
	make_inode(&inode, &mountp, 300);
	make_inode(&other, &mountp, 301);
	memset(&file, 0, sizeof(file));
	data.disk = &partition;
	data.block = 20;
	data.block_count = 8;
	for (mode = 1; mode <= 12; mode++) {
		metadata_mode = mode;
		metadata_calls = 0;
		file.f_inode = mode == 8 ? &other : &inode;
		assert(backing_claim_prepare_inode(&inode, BACKING_CLAIM_SWAP, &claim) == 0);
		fail_next_allocation = mode == 11;
		error = backing_claim_finalize_file(claim, &file, &data,
		    mode == 12 ? UINT32_MAX : 1);
		assert((error == 0) == (mode == 1));
		if (mode != 1) {
			assert(backing_mutation_begin_disk(&partition, 900, 1, NULL, &guard) == EBUSY);
			if (mode == 2 || mode == 3)
				assert(error == EINVAL);
			if (mode == 4 || mode == 5 || mode == 10)
				assert(error == EAGAIN);
			if (mode == 8)
				assert(error == EXDEV && metadata_calls == 0);
			if (mode == 11)
				assert(error == ENOMEM && fail_next_allocation == 0);
			if (mode == 12)
				assert(error == EOVERFLOW);
			metadata_mode = 1;
			metadata_calls = 0;
			file.f_inode = &inode;
			assert(backing_claim_finalize_file(claim, &file, &data, 1) == 0);
		}
		assert(backing_mutation_begin_disk(&partition, 20, 1, NULL, &guard) == EBUSY);
		assert(backing_mutation_begin_disk(&partition, 60, 1, NULL, &guard) == EBUSY);
		backing_claim_release(claim);
		assert(backing_mutation_begin_disk(&partition, 60, 1, NULL, &guard) == 0);
		backing_mutation_end(&guard);
		assert(refcount_load(&leaf.d_refs) == 0);
	}
	metadata_mode = 0;
	puts("file metadata claims: PASS 12 protection and failure scenarios");
}

static void
test_self_overlap(void)
{
	struct disk leaf;
	struct disk partition;
	struct mount mountp;
	struct inode inode;
	struct backing_claim *claim;
	struct backing_claim_extent extents[32];
	struct backing_mutation_guard guard;
	uint64_t starts[32];
	uint64_t ends[32];
	uint32_t seed;
	unsigned trial;
	unsigned count;
	unsigned i;
	unsigned j;
	int overlap;

	make_disk(&leaf, 4096, NULL, 0);
	make_disk(&partition, 1000, &leaf, 100);
	memset(&mountp, 0, sizeof(mountp));
	mountp.m_disk = &partition;
	mountp.m_type = &drv_fat_filesystem_type;
	make_inode(&inode, &mountp, 300);
	seed = 37;
	for (trial = 0; trial < 512; trial++) {
		count = 1U + trial % 32U;
		for (i = 0; i < count; i++) {
			seed = seed * 1664525U + 1013904223U;
			starts[i] = 100U + (seed >> 16) % 128U;
			ends[i] = starts[i] + 1U + (seed >> 24) % 12U;
			/* Include touching and reversed disjoint layouts, too. */
			if (trial % 4U == 0) {
				starts[i] = 100U + (count - i) * 4U;
				ends[i] = starts[i] + 4U;
			}
			extents[i].disk = i % 2U == 0 ? &leaf : &partition;
			extents[i].block = starts[i] - (i % 2U == 0 ? 0 : 100);
			extents[i].block_count = ends[i] - starts[i];
		}
		overlap = 0;
		for (i = 0; i < count; i++) {
			for (j = i + 1; j < count; j++) {
				if (starts[i] < ends[j] && starts[j] < ends[i])
					overlap = 1;
			}
		}
		assert(backing_claim_prepare_inode(&inode, BACKING_CLAIM_SWAP, &claim) == 0);
		assert(backing_claim_finalize(claim, extents, count) == (overlap ? EINVAL : 0));
		if (overlap) {
			/* Failure retains preparing exclusion and permits explicit retry. */
			assert(backing_mutation_begin_disk(&partition, 900, 1, NULL, &guard) == EBUSY);
			assert(backing_claim_finalize(claim, extents, 1) == 0);
		}
		backing_claim_release(claim);
		assert(refcount_load(&leaf.d_refs) == 0);
	}
	puts("backing self-overlap: PASS 512 oracle layouts and physical aliases");
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

	test_self_overlap();
	test_file_metadata();
	test_identity_and_snapshot_admission();

	/* A PREPARING claim excludes raw writes but permits lower disk writes
	 * belonging to an already accepted, unrelated inode mutation.
	 */
	make_disk(&leaf, 4096, NULL, 0);
	make_disk(&partition, 1000, &leaf, 100);
	memset(&mount_a, 0, sizeof(mount_a));
	mount_a.m_disk = &partition;
	mount_a.m_type = &drv_fat_filesystem_type;
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
	mount_a.m_type = mount_b.m_type = &drv_fat_filesystem_type;
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

	/* ADMIN uses the same raw-range exclusion; only its owner may mutate. */
	assert(backing_claim_prepare_disk(&disjoint, 0, disjoint.d_block_count,
	    BACKING_CLAIM_ADMIN, &raw_claim) == 0);
	assert(backing_mutation_begin_disk(&disjoint, 0, 1, NULL, &guard) == EBUSY);
	assert(backing_mutation_begin_disk(&disjoint, 0, 1, raw_claim, &guard) == 0);
	backing_mutation_end(&guard);
	assert(backing_claim_prepare_disk(&disjoint, 0, 1,
	    BACKING_CLAIM_SWAP, &second_claim) == EBUSY);
	backing_claim_release(raw_claim);
	assert(backing_mutation_begin_disk(&disjoint, 0, 1, NULL, &guard) == 0);
	backing_mutation_end(&guard);

	backing_claim_release(file_claim);
	assert(backing_mutation_begin_inode(&inode_alias, &guard) == 0);
	backing_mutation_end(&guard);
	assert(backing_mutation_begin_disk(&leaf, 120, 1, NULL, &guard) == 0);
	backing_mutation_end(&guard);
	assert(backing_claim_check_mount(&partition, 0) == 0);

	puts("S26 PASS production claim excludes ordinary inode mutation and raw aliases until release");

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
	mount_parent.m_type = &drv_fat_filesystem_type;
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
	assert(refcount_load(&leaf.d_refs) == 1);
	backing_claim_ref(raw_claim);
	backing_claim_release(raw_claim);
	assert(refcount_load(&leaf.d_refs) == 1);
	backing_claim_release(raw_claim);
	assert(refcount_load(&leaf.d_refs) == 0);


	assert(backing_mutation_begin_retired_disk(&leaf, &guard) == EINVAL);
	leaf.d_media_revoked = 1;
	assert(backing_claim_prepare_disk(&partition, 0, 1, BACKING_CLAIM_SWAP, &raw_claim) == 0);
	assert(backing_mutation_begin_retired_disk(&leaf, &guard) == EBUSY);
	backing_claim_release(raw_claim);
	assert(backing_mutation_begin_retired_disk(&leaf, &guard) == 0);
	assert(backing_claim_prepare_disk(&partition, 0, 1, BACKING_CLAIM_SWAP, &raw_claim) == EBUSY);
	backing_mutation_end(&guard);
	assert(refcount_load(&leaf.d_refs) == 0);
	puts("SWAP-T003/T004 backing claims and revoked-media exclusion: PASS");
	return 0;
}

/* Claim-held physical identities remain pinned through their final owner. */
void disk_ref(struct disk *disk)
{
	if (disk != NULL)
		__atomic_add_fetch(&disk->d_refs.value, 1, __ATOMIC_RELAXED);
}
void disk_release(struct disk *disk)
{
	if (disk != NULL) {
		if (__atomic_fetch_sub(&disk->d_refs.value, 1, __ATOMIC_RELAXED) == 0)
			abort();
	}
}
