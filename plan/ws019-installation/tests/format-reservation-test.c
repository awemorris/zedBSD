/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises production formatter reservations, backing claims, and VM admission.
 * The fixture supplies a fragmented FAT file, host locks, and page allocation.
 */

#include <kern/backing-claim.h>
#include <kern/cred.h>
#include <kern/disk.h>
#include <kern/fat.h>
#include <kern/file.h>
#include <kern/kmem.h>
#include <kern/record-lock.h>
#include <kern/vm-commit.h>
#include <kern/vm-lock.h>
#include <kern/vm-reclaim.h>
#include <kern/vmspace.h>
#include <kern/uaccess.h>
#include <zedbsd/fcntl.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_BYTES 16384U
#define CHECK(expression) check_result((expression) != 0, #expression, __LINE__)

struct fixture {
	struct disk leaf;
	struct disk partition;
	struct disk partition_alias;
	struct mount mount;
	struct mount mount_alias;
	struct inode inode;
	struct inode alias;
	struct inode other;
	struct file *owner;
	struct file *foreign;
	unsigned char bytes[FILE_BYTES];
};

struct allocation_pause {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	int armed;
	int waiting;
	int resume;
};

struct mapping_thread {
	struct file *file;
	struct vm_object *object;
	int error;
};

#ifdef ZEDBSD_FILE_CACHE_HOST
int ws025_writeback_eligible(struct file *, off_t, size_t);
const struct filesystem_type drv_fat_filesystem_type = {
	.fs_name = "fat", .writeback_range = ws025_writeback_eligible
};
#else
const struct filesystem_type drv_fat_filesystem_type = { .fs_name = "fat" };
#endif

static unsigned checks;
static unsigned allocation_count;
static unsigned allocation_failure;
static unsigned extent_calls;
static unsigned extent_failure;
static unsigned extent_shape;
static int copyin_error;
static unsigned backend_writes;
static int backend_sync_error;
static __thread int execution_token;
static __thread int pause_this_thread;
static __thread unsigned metadata_depth;
static __thread unsigned vm_mutex_depth;
static struct allocation_pause allocation_pause = {
	PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0
};
static struct fixture *active_fixture;

static void check_result(int passed, const char *expression, unsigned line);
static ssize_t backend_pread(struct file *file, void *buffer, size_t size, off_t offset);
static ssize_t backend_pwrite(struct file *file, const void *buffer, size_t size, off_t offset);
static int backend_fsync(struct file *file);
static void make_fixture(struct fixture *fixture);
static void close_fixture(struct fixture *fixture);
static void test_validation(void);
static void test_ioctl(void);
static void test_extent_validation(void);
static void test_exclusion_and_lifetime(void);
static void test_failure_rollback(void);
static void test_vm_admission(void);
static void test_shared_content_coherence(void);
static void test_vmspace_entrypoints(void);
static void release_fixture_regions(struct vmspace *vm);
static void *mapping_worker(void *argument);

static const struct file_ops backend_ops = {
	NULL, NULL, NULL, backend_pread, backend_pwrite,
	NULL, NULL, NULL, NULL, NULL, NULL, backend_fsync, NULL
};

/*
 * Runs reservation checks against the production implementation.
 */
int
main(void)
{
	/* Exercise validation, ownership, rollback, and both VM publication orders. */
	test_validation();
	test_ioctl();
	test_extent_validation();
	test_exclusion_and_lifetime();
	test_failure_rollback();
	test_vm_admission();
	test_shared_content_coherence();
	test_vmspace_entrypoints();

	/* Require all production objects and open files to have been released. */
	CHECK(file_count() == 0);
	CHECK(vm_object_count() == 0);
	CHECK(vm_object_page_count() == 0);
	printf("WS019 formatter reservation: PASS (%u checks)\n", checks);

	/* Report successful completion. */
	return 0;
}

/*
 * Supplies a stable execution identity for nested backing mutations.
 */
struct thread *
thread_current(void)
{
	/* Distinguish concurrent fixture threads without a kernel scheduler. */
	return (struct thread *)&execution_token;
}

/*
 * Allocates production objects and pauses selected VM publication attempts.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *result;

	/* Stop the mapping after its admission guard and before object publication. */
	if (pause_this_thread && size == sizeof(struct vm_object)) {
		CHECK(pthread_mutex_lock(&allocation_pause.lock) == 0);

		/* Publish the checkpoint only for the armed mapping attempt. */
		if (allocation_pause.armed) {
			allocation_pause.waiting = 1;
			CHECK(pthread_cond_broadcast(&allocation_pause.changed) == 0);

			/* Wait until the main thread has attempted reservation admission. */
			while (!allocation_pause.resume) {
				CHECK(pthread_cond_wait(&allocation_pause.changed,
				    &allocation_pause.lock) == 0);
			}
		}
		CHECK(pthread_mutex_unlock(&allocation_pause.lock) == 0);
	}

	/* Fail one selected allocation to verify rollback and retry behavior. */
	(void)__atomic_add_fetch(&allocation_count, 1U, __ATOMIC_RELAXED);
	if (allocation_failure != 0 && allocation_count == allocation_failure)
		return NULL;

	/* Return ordinary host memory for the production object. */
	result = calloc(count, size);
	return result;
}

/*
 * Releases production object storage.
 */
void
kern_free(
	void *pointer)
{
	/* Return the allocation to the host allocator. */
	free(pointer);
}

/*
 * Initializes a host-backed production spinlock.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Establish an unlocked object with its production diagnostic metadata. */
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

/*
 * Acquires a production spinlock using host atomic operations.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* Wait for exclusive ownership of the selected lock. */
	while (!atomic_try_acquire_zero(&lock->held))
		sched_yield();

	/* Host execution does not need an interrupt restore token. */
	return 0;
}

/*
 * Releases a host-backed production spinlock.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Publish all protected writes before admitting the next owner. */
	(void)enabled;
	atomic_store_release(&lock->held, 0);
}

/*
 * Initializes a production mutex for host execution.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Initialize the mutex metadata and its atomic admission field. */
	memset(mutex, 0, sizeof(*mutex));
	spin_init(&mutex->guard, rank, name);

	/* Report successful initialization. */
	return 0;
}

/*
 * Acquires a mutex without requiring a kernel scheduler.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	unsigned expected;

	/* Wait for the current owner to publish its unlock. */
	expected = 0;
	while (!atomic_raw_compare_exchange(&mutex->locked, &expected, 1)) {
		expected = 0;
		sched_yield();
	}
	mutex->owner = thread_current();

	/* Track address-space lock ownership for FAT identity assertions. */
	if (mutex->guard.rank == LOCK_RANK_VMSPACE)
		vm_mutex_depth++;
}

/*
 * Attempts an immediate mutex acquisition.
 */
int
mutex_trylock(
	struct mutex *mutex)
{
	unsigned expected;
	int acquired;

	/* Claim an unlocked mutex with the same atomic gate as blocking lock. */
	expected = 0;
	acquired = atomic_raw_compare_exchange(&mutex->locked, &expected, 1);
	if (acquired) {
		mutex->owner = thread_current();

		/* Track successful address-space lock acquisition. */
		if (mutex->guard.rank == LOCK_RANK_VMSPACE)
			vm_mutex_depth++;
	}

	/* Return the actual acquisition result. */
	return acquired;
}

/*
 * Releases a mutex and publishes the protected writes.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* Verify that only the current owner releases the lock. */
	assert(mutex->owner == thread_current());

	/* Remove address-space ownership before publishing the unlocked gate. */
	if (mutex->guard.rank == LOCK_RANK_VMSPACE) {
		CHECK(vm_mutex_depth != 0);
		vm_mutex_depth--;
	}
	mutex->owner = NULL;
	atomic_raw_store_release(&mutex->locked, 0);
}

/*
 * Initializes a wait queue for production VM bookkeeping.
 */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	/* Establish an empty queue and its wake sequence. */
	memset(queue, 0, sizeof(*queue));
	queue->name = name;
}

/*
 * Reads a wait queue wake sequence.
 */
uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	/* Return the sequence used by production retry paths. */
	return queue->sequence;
}

/*
 * Advances a wait queue after a completed production transition.
 */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	/* Record the completed transition; fixture threads use explicit barriers. */
	queue->sequence++;
}

/*
 * Rejects an unexpected uncoordinated production wait.
 */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	/* Permit a completed wake, otherwise fail instead of hiding a deadlock. */
	(void)lock;
	(void)deadline;
	(void)flags;
	if (queue->sequence != observed)
		return EAGAIN;

	/* An intentional fixture interleaving never requires this wait path. */
	CHECK(0);
	return EINTR;
}

/*
 * Maps partition-relative ranges onto their physical disk identity.
 */
int
disk_resolve_range(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	struct disk **leaf_out,
	uint64_t *mapped_out)
{
	uint64_t mapped;
	struct disk *leaf;

	/* Reject out-of-bounds ranges before following partition aliases. */
	if (disk == NULL || count == 0 || block >= disk->d_block_count ||
	    count > disk->d_block_count - block)
		return EINVAL;

	/* Collapse all partition offsets into one physical disk range. */
	mapped = block;
	leaf = disk;
	while (leaf->d_parent != NULL) {
		mapped += leaf->d_parent_offset;
		leaf = leaf->d_parent;
	}
	*leaf_out = leaf;
	*mapped_out = mapped;

	/* Report the canonical range. */
	return 0;
}

/*
 * Supplies stable FAT directory-entry identities for separately mounted aliases.
 */
int
drv_fat_file_backing_identity(
	struct inode *inode,
	struct disk **disk,
	uint64_t *object)
{
	/* Require sleeping FAT identity resolution outside higher-ranked VM locks. */
	CHECK(metadata_depth == 0);
	CHECK(vm_mutex_depth == 0);

	/* Use the fixture inode number as the FAT directory-entry offset. */
	*disk = inode->i_mount->m_disk;
	*object = (uint64_t)inode->i_ino;

	/* Report successful identity resolution. */
	return 0;
}

/*
 * Enumerates two noncontiguous extents of the fixed-size fixture file.
 */
int
drv_fat_file_extents(
	struct file *file,
	fat_extent_cb callback,
	void *argument)
{
	int error;

	/* Inject a collection failure after reservation publication. */
	extent_calls++;
	if (extent_failure == extent_calls)
		return EIO;

	/* Return empty or incomplete maps without changing any contents. */
	if (extent_shape == 1)
		return 0;

	/* Exercise malformed coverage and an out-of-range physical extent. */
	if (extent_shape == 2) {
		error = callback(1, 20, 16, argument);

		/* Propagate the production extent validator result. */
		return error;
	}

	/* Reject a zero-length extent before accepting its physical location. */
	if (extent_shape == 3) {
		error = callback(0, 20, 0, argument);

		/* Propagate the production extent validator result. */
		return error;
	}

	/* Reject excess logical coverage. */
	if (extent_shape == 4) {
		error = callback(0, 20, 33, argument);

		/* Propagate the production extent validator result. */
		return error;
	}

	/* Reject a data extent outside the partition. */
	if (extent_shape == 5) {
		error = callback(0, 999, 32, argument);

		/* Propagate the production extent validator result. */
		return error;
	}

	/* Detect an impossible size change before owner capability publication. */
	if (extent_shape == 6 && extent_calls == 2)
		file->f_inode->i_size -= 512;

	/* Report the first half of the file. */
	error = callback(0, 20, 16, argument);
	if (error != 0)
		return error;

	/* Return a truncated extent map before logical EOF. */
	if (extent_shape == 7)
		return 0;

	/* Report the second half at a disjoint physical location. */
	error = callback(16, 60, 16, argument);
	if (error != 0)
		return error;

	/* Finish the extent enumeration. */
	return 0;
}

/*
 * Reports that the fixture has no unrelated writable disk mounts.
 */
int
mount_disk_writable_busy(
	struct disk *disk)
{
	/* Disk mutation exclusion is exercised by production backing claims. */
	(void)disk;
	return 0;
}

/*
 * Retains a fixture inode reference.
 */
void
inode_ref(
	struct inode *inode)
{
	/* Keep the inode alive for production path and VM snapshots. */
	refcount_get(&inode->i_refs);
}

/*
 * Releases a fixture inode reference.
 */
void
inode_release(
	struct inode *inode)
{
	/* Fixture-owned storage survives until all production references drain. */
	CHECK(!refcount_put(&inode->i_refs));
}

/*
 * Retains the inode represented by a resolved path.
 */
void
path_set(
	struct path *path,
	struct mount *mount,
	struct inode *inode)
{
	/* Record and retain the resolved fixture path. */
	path->p_mount = mount;
	path->p_inode = inode;
	inode_ref(inode);
}

/*
 * Releases the fixture reference owned by a path.
 */
void
path_release(
	struct path *path)
{
	/* Drain the retained inode and clear the released path. */
	inode_release(path->p_inode);
	memset(path, 0, sizeof(*path));
}

/*
 * Accepts fixture timestamp updates after completed I/O.
 */
void
inode_touch(
	struct inode *inode,
	unsigned flags)
{
	/* Timestamp durability does not participate in reservation admission. */
	(void)inode;
	(void)flags;
}

/*
 * Supplies a successful fixture inode durability barrier.
 */
int
inode_sync(
	struct inode *inode)
{
	/* Keep fault injection at the backend file durability boundary. */
	(void)inode;
	return 0;
}

/*
 * Releases advisory locks when the final production descriptor closes.
 */
void
record_lock_release_file(
	struct file *file)
{
	/* This fixture does not allocate advisory record locks. */
	(void)file;
}

/*
 * Accepts the set-id transition for the fixture's unprivileged file mode.
 */
int
vfs_clear_setid_on_write(
	struct inode *inode,
	const struct ucred *credential)
{
	/* The fixture does not carry set-id metadata. */
	(void)inode;
	(void)credential;
	return 0;
}

/*
 * Accepts an internal content-change metadata transition.
 */
int
vfs_clear_setid_on_content_change(
	struct inode *inode)
{
	/* The fixture does not carry set-id metadata. */
	(void)inode;
	return 0;
}

/*
 * Copies an ioctl request from the fixture's simulated user address space.
 */
int
copyin(
	uintptr_t source,
	void *destination,
	size_t size)
{
	/* Preserve an injected user-copy failure before touching request memory. */
	if (copyin_error != 0)
		return copyin_error;

	/* Copy the caller-owned request into the production stack record. */
	memcpy(destination, (const void *)source, size);

	/* Report successful request transfer. */
	return 0;
}

/*
 * Reports a production invariant failure with the original source location.
 */
void
hal_fatal(
	const char *file,
	int line,
	const char *message)
{
	/* Preserve the kernel diagnostic before terminating the fixture. */
	fprintf(stderr, "%s:%d: %s\n", file, line, message);
	abort();
}

/*
 * Allocates host memory for a production shared-cache page.
 */
int
hal_pmem_alloc(
	const struct hal_pmem_request *request,
	struct hal_pmem *memory)
{
	/* Allocate a fresh zeroed page with ordinary host storage. */
	memset(memory, 0, sizeof(*memory));
	memory->vaddr = calloc(1, request->size);
	if (memory->vaddr == NULL)
		return ENOMEM;

	/* Publish the page descriptor consumed by production VM code. */
	memory->size = request->size;
	memory->paddr = (hal_physaddr_t)(uintptr_t)memory->vaddr;
	memory->type = request->type;

	/* Report successful allocation. */
	return 0;
}

/*
 * Releases a production shared-cache page from host memory.
 */
int
hal_pmem_free(
	struct hal_pmem *memory)
{
	/* Release and invalidate the host page descriptor. */
	free(memory->vaddr);
	memset(memory, 0, sizeof(*memory));

	/* Report successful release. */
	return 0;
}

/*
 * Reports that this fixture has no reclaimable private pages.
 */
int
vm_reclaim_private_one(
	struct vm_page *avoid)
{
	/* Shared-cache allocation uses ordinary host memory. */
	(void)avoid;
	return EAGAIN;
}

/*
 * Reports that the fixture has no reclaim fallback.
 */
int
vm_reclaim_one(
	struct vm_page *avoid)
{
	/* Allocation failure must remain visible to the caller. */
	(void)avoid;
	return EAGAIN;
}

/*
 * Verifies that shared file objects never consume anonymous commit accounting.
 */
void
vm_commit_release(
	size_t size)
{
	/* This fixture creates no anonymous committed objects. */
	(void)size;
	CHECK(0);
}

/*
 * Enters the fixture's single-threaded reverse-mapping metadata path.
 */
void
vm_metadata_enter(void)
{
	/* Track outer metadata ownership for the lock-order regression checks. */
	metadata_depth++;
}

/*
 * Leaves the fixture's single-threaded reverse-mapping metadata path.
 */
void
vm_metadata_leave(void)
{
	/* Require balanced outer metadata ownership. */
	CHECK(metadata_depth != 0);
	metadata_depth--;
}

/*
 * Initializes metadata tracking before the first address-space operation.
 */
void
vm_metadata_init(void)
{
	/* Require initialization outside the tracked metadata critical section. */
	CHECK(metadata_depth == 0);
}

/*
 * Supplies a bounded host user-address range for production region validation.
 */
void
hal_page_get_user_range(
	uintptr_t *minimum,
	uintptr_t *limit)
{
	/* Keep the LP64 mmap base inside the simulated user range. */
	*minimum = 4096;
	*limit = (uintptr_t)1 << 47;
}

/*
 * Rejects an unexpected hardware page-protection request.
 */
int
hal_page_prot_query(
	hal_space_t space,
	void *address,
	size_t size,
	uint32_t protection,
	uint32_t *flags)
{
	/* Region-only admission cells never install hardware translations. */
	(void)space;
	(void)address;
	(void)size;
	(void)protection;
	(void)flags;
	CHECK(0);

	/* Preserve a visible failure if assertions are adapted later. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Rejects an unexpected hardware page-unmap request.
 */
int
hal_page_unmap(
	hal_space_t space,
	void *address,
	size_t size)
{
	/* Region-only admission cells never install hardware translations. */
	(void)space;
	(void)address;
	(void)size;
	CHECK(0);

	/* Preserve a visible failure if assertions are adapted later. */
	return HAL_ERR_UNSUPPORTED;
}

/*
 * Rejects an unexpected hardware address-space destruction.
 */
void
hal_page_destroy_space(
	hal_space_t space)
{
	/* Fixture address spaces use caller-owned stack metadata. */
	(void)space;
	CHECK(0);
}

/*
 * Rejects private-page accounting from shared region admission.
 */
void
vm_page_untrack(
	struct vm_page *page)
{
	/* This fixture has no private pages to reclaim. */
	(void)page;
	CHECK(0);
}

/*
 * Rejects anonymous commit charges from shared file region admission.
 */
int
vm_commit_reserve(
	size_t size)
{
	/* Shared file mappings require no anonymous commit reservation. */
	(void)size;
	CHECK(0);

	/* Preserve a visible failure if assertions are adapted later. */
	return ENOMEM;
}

/* Checks one observable result and aborts with the failing source location. */
static void
check_result(
	int passed,
	const char *expression,
	unsigned line)
{
	/* Count checks atomically because the VM admission cell uses two threads. */
	(void)__atomic_add_fetch(&checks, 1U, __ATOMIC_RELAXED);
	if (!passed) {
		fprintf(stderr, "format-reservation:%u: failed: %s\n", line,
		    expression);
		abort();
	}
}

/* Reads fixed-size fixture contents through the production file layer. */
static ssize_t
backend_pread(
	struct file *file,
	void *buffer,
	size_t size,
	off_t offset)
{
	/* Confine the fake backend to the preallocated file. */
	(void)file;
	if (offset < 0 || (uint64_t)offset > FILE_BYTES ||
	    size > FILE_BYTES - (uint64_t)offset)
		return -EFBIG;

	/* Return the requested file bytes. */
	memcpy(buffer, active_fixture->bytes + offset, size);
	return (ssize_t)size;
}

/* Writes fixture bytes only while the production claim authorizes FAT I/O. */
static ssize_t
backend_pwrite(
	struct file *file,
	const void *buffer,
	size_t size,
	off_t offset)
{
	struct backing_mutation_guard guard;
	int error;

	/* Reject writes beyond the fixture storage before copying any bytes. */
	if (offset < 0 || (uint64_t)offset > FILE_BYTES ||
	    size > FILE_BYTES - (uint64_t)offset)
		return -EFBIG;

	/* Verify production ownership propagation into a claimed FAT extent. */
	error = backing_mutation_begin_disk_filesystem(
		file->f_inode->i_mount->m_disk, 20, 1, &guard);
	if (error != 0)
		return -error;

	/* Publish the accepted content mutation and release its lower-layer guard. */
	backend_writes++;
	memcpy(active_fixture->bytes + offset, buffer, size);
	backing_mutation_end(&guard);

	/* Report exactly the accepted byte count. */
	return (ssize_t)size;
}

/* Returns the configured durability outcome. */
static int
backend_fsync(
	struct file *file)
{
	/* Let retained-cache tests inject a failed durability barrier. */
	(void)file;
	return backend_sync_error;
}

/* Constructs two independently opened aliases of one fragmented FAT file. */
static void
make_fixture(
	struct fixture *fixture)
{
	struct path path;

	/* Initialize the shared physical disk and two equivalent partition views. */
	memset(fixture, 0, sizeof(*fixture));
	fixture->leaf.d_block_size = 512;
	fixture->leaf.d_block_count = 4096;
	fixture->partition.d_block_size = 512;
	fixture->partition.d_block_count = 1000;
	fixture->partition.d_parent = &fixture->leaf;
	fixture->partition.d_parent_offset = 100;
	fixture->partition_alias = fixture->partition;
	fixture->mount.m_disk = &fixture->partition;
	fixture->mount.m_type = &drv_fat_filesystem_type;
	fixture->mount_alias.m_disk = &fixture->partition_alias;
	fixture->mount_alias.m_type = &drv_fat_filesystem_type;

	/* Initialize separate inodes with one canonical FAT directory-entry key. */
	fixture->inode.i_type = INODE_REG;
	fixture->inode.i_size = FILE_BYTES;
	fixture->inode.i_ino = 77;
	fixture->inode.i_mount = &fixture->mount;
	fixture->inode.i_fop = &backend_ops;
	refcount_init(&fixture->inode.i_refs, 1);
	fixture->alias = fixture->inode;
	fixture->alias.i_mount = &fixture->mount_alias;
	fixture->other = fixture->alias;
	fixture->other.i_ino = 78;

	/* Open real production file descriptions for the owner and foreign alias. */
	path.p_mount = &fixture->mount;
	path.p_inode = &fixture->inode;
	CHECK(file_open_resolved(&path, O_RDWR | O_NOFOLLOW,
	    &fixture->owner) == 0);
	path.p_mount = &fixture->mount_alias;
	path.p_inode = &fixture->alias;
	CHECK(file_open_resolved(&path, O_RDWR | O_NOFOLLOW,
	    &fixture->foreign) == 0);

	/* Reset all backend fault controls and install the active byte storage. */
	active_fixture = fixture;
	allocation_count = 0;
	allocation_failure = 0;
	extent_calls = 0;
	extent_failure = 0;
	extent_shape = 0;
	copyin_error = 0;
	backend_writes = 0;
	backend_sync_error = 0;
	memset(fixture->bytes, 0xa5, sizeof(fixture->bytes));
}

/* Closes all fixture descriptors and verifies their inode references drain. */
static void
close_fixture(
	struct fixture *fixture)
{
	/* Drop each descriptor that the current cell still owns. */
	if (fixture->owner != NULL)
		CHECK(file_close(fixture->owner) == 0);

	/* Drop the independently opened canonical alias. */
	if (fixture->foreign != NULL)
		CHECK(file_close(fixture->foreign) == 0);

	/* Require balanced path and VM snapshot references. */
	CHECK(refcount_load(&fixture->inode.i_refs) == 1);
	CHECK(refcount_load(&fixture->alias.i_refs) == 1);
	CHECK(vm_object_count() == 0);
}

/* Rejects unsupported and unstable formatter descriptors without writing. */
static void
test_validation(void)
{
	struct fixture fixture;
	unsigned original;

	/* Reject invalid sizes and descriptors before acquiring a lease. */
	make_fixture(&fixture);
	CHECK(file_format_reserve(NULL, FILE_BYTES) != 0);
	CHECK(file_format_reserve(fixture.owner, 0) != 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES - 1U) != 0);
	CHECK(file_format_reserve(fixture.owner, UINT64_MAX) != 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES - 512U) == ESTALE);

	/* Require a read/write no-follow description with append disabled. */
	original = (unsigned)file_status_flags_get(fixture.owner);
	atomic_store_release(&fixture.owner->f_flags, O_RDONLY | O_NOFOLLOW);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) != 0);
	atomic_store_release(&fixture.owner->f_flags, O_WRONLY | O_NOFOLLOW);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) != 0);
	atomic_store_release(&fixture.owner->f_flags, O_RDWR);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) != 0);
	atomic_store_release(&fixture.owner->f_flags, original | O_APPEND);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) != 0);
	atomic_store_release(&fixture.owner->f_flags, original);

	/* Require a supported regular file on a writable FAT mount. */
	fixture.inode.i_type = INODE_BLOCK;
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) != 0);
	fixture.inode.i_type = INODE_REG;
	fixture.mount.m_flags = MOUNT_READ_ONLY;
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EROFS);
	fixture.mount.m_flags = 0;
	fixture.partition.d_flags = DISK_READ_ONLY;
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EROFS);
	fixture.partition.d_flags = 0;
	fixture.mount.m_type = NULL;
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EOPNOTSUPP);
	fixture.mount.m_type = &drv_fat_filesystem_type;
	fixture.inode.i_flags = INODE_ROOT;
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	fixture.inode.i_flags = INODE_SWAPFILE;
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	fixture.inode.i_flags = INODE_LOOPFILE;
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	fixture.inode.i_flags = 0;
	CHECK(backend_writes == 0);
	close_fixture(&fixture);
}

/* Rejects malformed ioctl requests before invoking reservation admission. */
static void
test_ioctl(void)
{
	struct fixture fixture;
	struct zedbsd_file_format_reserve request;
	uintptr_t argument;

	/* Construct the complete version-one request with its fixed-width layout. */
	make_fixture(&fixture);
	memset(&request, 0, sizeof(request));
	request.version = ZEDBSD_FILE_FORMAT_VERSION;
	request.struct_size = sizeof(request);
	request.size_bytes = FILE_BYTES;
	argument = (uintptr_t)&request;
	CHECK(sizeof(request) == 32);
	CHECK(offsetof(struct zedbsd_file_format_reserve, size_bytes) == 8);

	/* Propagate copy faults and reject unknown versions and nonzero extensions. */
	copyin_error = EFAULT;
	CHECK(file_ioctl(fixture.owner, ZEDBSD_FILE_FORMAT_RESERVE,
	    argument) == EFAULT);
	copyin_error = 0;
	request.version++;
	CHECK(file_ioctl(fixture.owner, ZEDBSD_FILE_FORMAT_RESERVE,
	    argument) == EINVAL);
	request.version = ZEDBSD_FILE_FORMAT_VERSION;
	request.struct_size--;
	CHECK(file_ioctl(fixture.owner, ZEDBSD_FILE_FORMAT_RESERVE,
	    argument) == EINVAL);
	request.struct_size = sizeof(request);
	request.reserved[0] = 1;
	CHECK(file_ioctl(fixture.owner, ZEDBSD_FILE_FORMAT_RESERVE,
	    argument) == EINVAL);
	request.reserved[0] = 0;
	request.reserved[1] = 1;
	CHECK(file_ioctl(fixture.owner, ZEDBSD_FILE_FORMAT_RESERVE,
	    argument) == EINVAL);
	request.reserved[1] = 0;
	CHECK(backend_writes == 0);

	/* Admit exactly the validated request and release its claim on close. */
	CHECK(file_ioctl(fixture.owner, ZEDBSD_FILE_FORMAT_RESERVE,
	    argument) == 0);
	close_fixture(&fixture);
}

/* Rejects incomplete, malformed, and stale physical extent snapshots. */
static void
test_extent_validation(void)
{
	struct fixture fixture;
	struct backing_mutation_guard guard;
	unsigned shape;
	int error;

	/* Exercise each independent map defect and verify complete rollback. */
	for (shape = 1; shape <= 7; shape++) {
		make_fixture(&fixture);
		extent_shape = shape;
		error = file_format_reserve(fixture.owner, FILE_BYTES);
		CHECK(error != 0);
		CHECK(backend_writes == 0);
		CHECK(fixture.bytes[0] == 0xa5);
		CHECK(fixture.bytes[FILE_BYTES - 1U] == 0xa5);
		CHECK(backing_mutation_begin_inode(&fixture.alias, &guard) == 0);
		backing_mutation_end(&guard);
		extent_shape = 0;
		fixture.inode.i_size = FILE_BYTES;
		CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
		close_fixture(&fixture);
	}
}

/* Verifies exclusive mutation ownership, fixed EOF, and final-close release. */
static void
test_exclusion_and_lifetime(void)
{
	struct fixture fixture;
	struct backing_mutation_guard guard;
	struct backing_claim *claim;
	struct vm_object *object;
	unsigned char data[32];
	unsigned before;
	ssize_t transferred;

	/* Acquire ownership through the production formatter API. */
	make_fixture(&fixture);
	memset(data, 0x5a, sizeof(data));
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	CHECK(file_format_reserve(fixture.foreign, FILE_BYTES) == EBUSY);
	CHECK(backing_mutation_begin_inode(&fixture.alias, &guard) == EBUSY);
	CHECK(backing_mutation_begin_inode(&fixture.inode, &guard) == EBUSY);
	CHECK(backing_claim_prepare_inode(&fixture.alias, BACKING_CLAIM_SWAP,
	    &claim) == EBUSY);
	CHECK(backing_claim_prepare_inode(&fixture.alias, BACKING_CLAIM_LOOP,
	    &claim) == EBUSY);
	CHECK(backing_mutation_begin_disk(&fixture.leaf, 120, 1, NULL,
	    &guard) == EBUSY);
	CHECK(backing_claim_check_teardown(&fixture.partition) == EBUSY);
	CHECK(vm_object_get_shared(fixture.foreign, &object) == EBUSY);
	CHECK(vm_object_get_shared(fixture.owner, &object) == EBUSY);

	/* Accept unrelated inode mutations while keeping raw volume writes blocked. */
	CHECK(backing_mutation_begin_inode(&fixture.other, &guard) == 0);
	backing_mutation_end(&guard);
	CHECK(file_pwrite(fixture.foreign, data, sizeof(data), 0) == -EBUSY);
	CHECK(backend_writes == 0);
	CHECK(file_pwrite(fixture.owner, data, sizeof(data), 0) ==
	    (ssize_t)sizeof(data));
	CHECK(backend_writes == 1);
	CHECK(memcmp(fixture.bytes, data, sizeof(data)) == 0);
	CHECK(file_pread(fixture.foreign, data, sizeof(data), 0) ==
	    (ssize_t)sizeof(data));

	/* Refuse growth beyond the original EOF without issuing a backend write. */
	before = backend_writes;
	CHECK(file_pwrite(fixture.owner, data, 1, FILE_BYTES) == -EFBIG);
	transferred = file_pwrite(fixture.owner, data, sizeof(data),
	    FILE_BYTES - 1U);
	CHECK(transferred == -EFBIG);
	CHECK(backend_writes == before);
	CHECK(fixture.inode.i_size == FILE_BYTES);

	/* Refuse descriptor append changes and unexpected stale EOF during a lease. */
	file_status_flags_update(fixture.owner, O_APPEND, O_APPEND);
	CHECK(file_pwrite(fixture.owner, data, 1, 0) == -EINVAL);
	file_status_flags_update(fixture.owner, O_APPEND, 0);
	fixture.inode.i_size -= 512;
	CHECK(file_pwrite(fixture.owner, data, 1, 0) == -ESTALE);
	fixture.inode.i_size = FILE_BYTES;
	CHECK(backend_writes == before);

	/* Keep ownership across duplicated descriptors until the last close. */
	file_ref(fixture.owner);
	CHECK(file_close(fixture.owner) == 0);
	CHECK(file_pwrite(fixture.foreign, data, 1, 0) == -EBUSY);
	CHECK(file_close(fixture.owner) == 0);
	fixture.owner = NULL;
	CHECK(file_pwrite(fixture.foreign, data, 1, 0) == 1);
	CHECK(backing_claim_check_teardown(&fixture.partition) == 0);
	close_fixture(&fixture);
}

/* Verifies that failed reservations release every partially acquired claim. */
static void
test_failure_rollback(void)
{
	struct fixture fixture;
	struct backing_mutation_guard guard;
	unsigned failure;
	int error;

	/* Fail both passes of extent enumeration and retry the same descriptor. */
	for (failure = 1; failure <= 2; failure++) {
		make_fixture(&fixture);
		extent_failure = failure;
		CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EIO);
		CHECK(backing_mutation_begin_inode(&fixture.alias, &guard) == 0);
		backing_mutation_end(&guard);
		CHECK(backend_writes == 0);
		extent_failure = 0;
		CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
		close_fixture(&fixture);
	}

	/* Fail every allocation along a successful acquisition and require retry. */
	for (failure = 1; failure <= 8; failure++) {
		make_fixture(&fixture);
		allocation_failure = failure;
		error = file_format_reserve(fixture.owner, FILE_BYTES);

		/* Stop after the first index beyond the acquisition's allocation count. */
		if (error == 0) {
			close_fixture(&fixture);
			break;
		}
		CHECK(error == ENOMEM);
		CHECK(backing_mutation_begin_inode(&fixture.alias, &guard) == 0);
		backing_mutation_end(&guard);
		CHECK(backend_writes == 0);
		allocation_failure = 0;
		CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
		close_fixture(&fixture);
	}
	CHECK(failure <= 8);
}

/* Checks mapping-first, reservation-first, and paused mapping admission orders. */
static void
test_vm_admission(void)
{
	struct fixture fixture;
	struct vm_object *object;
	struct mapping_thread mapping;
	struct vm_object_page *page;
	pthread_t worker;

	/* A separately mounted alias with a published object blocks formatting. */
	make_fixture(&fixture);
	CHECK(vm_object_get_shared(fixture.foreign, &object) == 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	vm_object_put(object);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
	close_fixture(&fixture);

	/* Retained writeback state also blocks formatting until its retry succeeds. */
	make_fixture(&fixture);
	CHECK(vm_object_get_shared(fixture.foreign, &object) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	memset(page->pmem.vaddr, 0x6c, 512);
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	backend_sync_error = EIO;
	vm_object_put(object);
	CHECK(vm_object_retained_count() == 1);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	backend_sync_error = 0;
	CHECK(vm_object_get_shared(fixture.foreign, &object) == 0);
	vm_object_put(object);
	CHECK(vm_object_retained_count() == 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
	close_fixture(&fixture);

	/* Pause a mapper after admission but before it can publish an object. */
	make_fixture(&fixture);
	memset(&mapping, 0, sizeof(mapping));
	mapping.file = fixture.foreign;
	allocation_pause.armed = 1;
	allocation_pause.waiting = 0;
	allocation_pause.resume = 0;
	CHECK(pthread_create(&worker, NULL, mapping_worker, &mapping) == 0);
	CHECK(pthread_mutex_lock(&allocation_pause.lock) == 0);

	/* Wait for the precise production allocation checkpoint. */
	while (!allocation_pause.waiting) {
		CHECK(pthread_cond_wait(&allocation_pause.changed,
		    &allocation_pause.lock) == 0);
	}
	CHECK(pthread_mutex_unlock(&allocation_pause.lock) == 0);
	CHECK(vm_object_count() == 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);

	/* Allow publication and verify the resulting object continues exclusion. */
	CHECK(pthread_mutex_lock(&allocation_pause.lock) == 0);
	allocation_pause.resume = 1;
	CHECK(pthread_cond_broadcast(&allocation_pause.changed) == 0);
	CHECK(pthread_mutex_unlock(&allocation_pause.lock) == 0);
	CHECK(pthread_join(worker, NULL) == 0);
	allocation_pause.armed = 0;
	CHECK(mapping.error == 0);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	vm_object_put(mapping.object);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
	close_fixture(&fixture);
}

/*
 * Preserves the regular-I/O/shared-cache regression from the selected
 * vm-object-resize-host-test.c test_regular_io_uses_shared_cache_domain case.
 * Only this case is adapted; the legacy fixture suite is not a dependency.
 */
static void
test_shared_content_coherence(void)
{
	struct fixture fixture;
	struct vm_object *object;
	struct vm_object_page *page;
	const unsigned char written[4] = { 0x70, 0x71, 0x72, 0x73 };
	unsigned char observed;
	unsigned char *bytes;

	/* Admit an ordinary shared cache before any formatter owns the file. */
	make_fixture(&fixture);
	CHECK(vm_object_get_shared(fixture.owner, &object) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	bytes = page->pmem.vaddr;
	bytes[10] = 0x55;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);

	/* Flush the older dirty page before a disjoint positional write commits. */
	CHECK(file_pwrite(fixture.owner, written, sizeof(written), 100) ==
	    (ssize_t)sizeof(written));
	CHECK(fixture.bytes[10] == 0x55);
	CHECK(memcmp(fixture.bytes + 100, written, sizeof(written)) == 0);
	CHECK(vm_object_fault(object, 0, &page) == 0);
	bytes = page->pmem.vaddr;
	CHECK(bytes[10] == 0x55);
	CHECK(memcmp(bytes + 100, written, sizeof(written)) == 0);

	/* Read the dirty shared-cache byte through ordinary positional I/O. */
	bytes[25] = 0x66;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	observed = 0;
	CHECK(file_pread(fixture.owner, &observed, 1, 25) == 1);
	CHECK(observed == 0x66);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);

	/* Release the ordinary cache before permitting a formatter reservation. */
	vm_object_put(object);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
	close_fixture(&fixture);
}

/* Attempts one production mapping with the allocation checkpoint enabled. */
static void *
mapping_worker(
	void *argument)
{
	struct mapping_thread *mapping;

	/* Arm the checkpoint only for this worker's VM object allocation. */
	mapping = argument;
	pause_this_thread = 1;
	mapping->error = vm_object_get_shared(mapping->file, &mapping->object);
	pause_this_thread = 0;

	/* Publish completion through pthread_join. */
	return NULL;
}

/* Releases fixture-owned regions without introducing page-table machinery. */
static void
release_fixture_regions(
	struct vmspace *vm)
{
	struct vm_region *region;

	/* These cells create region metadata without faulting hardware mappings. */
	while (vm->regions != NULL) {
		region = vm->regions;
		vm->regions = region->next;
		CHECK(region->pages == NULL);
		vm_object_put(region->object);
		CHECK(file_close(region->file) == 0);
		kern_free(region);
	}
	vm->mapped_virtual_bytes = 0;
}

/* Exercises public shared mapping entrypoints with FAT lock-order assertions. */
static void
test_vmspace_entrypoints(void)
{
	struct fixture fixture;
	struct vmspace vm;
	struct vm_region *region;
	uintptr_t mapped;

	/* Initialize the production user layout before taking VM metadata locks. */
	make_fixture(&fixture);
	vmspace_layout_init();
	memset(&vm, 0, sizeof(vm));
	CHECK(mutex_init(&vm.lock, LOCK_RANK_VMSPACE, "fixture VM") == 0);
	vm.address_limit = UINT64_MAX;
	vm.data_limit = UINT64_MAX;
	vm.stack_limit = UINT64_MAX;

	/* Publish an explicit-address mapping through the real public entrypoint. */
	CHECK(vmspace_map_file_shared(&vm, 0x10000U, 4096,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, fixture.foreign, 0, 4096,
	    &region) == 0);
	CHECK(region == vm.regions);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	release_fixture_regions(&vm);

	/* Publish an automatically placed mapping through the other entrypoint. */
	CHECK(vmspace_map_file_shared_find(&vm, 0, 4096,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, fixture.foreign, 0, 4096,
	    &mapped) == 0);
	CHECK(vm.regions != NULL);
	CHECK(vm.regions->start == mapped);
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == EBUSY);
	release_fixture_regions(&vm);

	/* Release prepared objects after publication errors without holding VM locks. */
	CHECK(vmspace_map_file_shared(&vm, 1, 4096, HAL_SPACE_READ,
	    fixture.foreign, 0, 4096, &region) == EINVAL);
	CHECK(vm_object_count() == 0);
	CHECK(vmspace_map_file_shared_find(&vm, 0, 4096, HAL_SPACE_READ,
	    fixture.foreign, 1, 4096, &mapped) == EINVAL);
	CHECK(vm_object_count() == 0);

	/* A lease blocks both mapping entrypoints before they acquire VM locks. */
	CHECK(file_format_reserve(fixture.owner, FILE_BYTES) == 0);
	CHECK(vmspace_map_file_shared(&vm, 0x10000U, 4096, HAL_SPACE_READ,
	    fixture.foreign, 0, 4096, &region) == EBUSY);
	CHECK(vmspace_map_file_shared_find(&vm, 0, 4096, HAL_SPACE_READ,
	    fixture.foreign, 0, 4096, &mapped) == EBUSY);
	CHECK(metadata_depth == 0);
	CHECK(vm_mutex_depth == 0);
	close_fixture(&fixture);
}

/* This fixture models storage identity without a live disk registry. */
void disk_ref(struct disk *disk) { (void)disk; }
void disk_release(struct disk *disk) { (void)disk; }
