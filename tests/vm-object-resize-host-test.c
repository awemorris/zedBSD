/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Deterministic shared-object EOF transaction and pin lifetime tests. */
#include "kern/file.h"
#include "kern/cred.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/test-checkpoint.h"
#include "kern/vm-object.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <threads.h>

#define TEST_PAGE_SIZE 4096U
#define BACKING_CAPACITY (2U * TEST_PAGE_SIZE)

struct fake_backing {
	uint8_t bytes[BACKING_CAPACITY];
	off_t size;
	unsigned reads;
	unsigned writes;
};

struct fixture {
	struct inode inode;
	struct file file;
	struct fake_backing backing;
	struct vm_object *object;
};

static mtx_t checkpoint_lock;
static cnd_t checkpoint_condition;
static int block_pwrite;
static int pwrite_entered;
static int release_pwrite;
static size_t pwrite_limit;
static int pwrite_error;
static int clear_setid_error;
static unsigned clear_setid_calls;
static int require_clear_before_pwrite;
static int setid_clear_succeeded;
static int block_range_read;
static int range_read_entered;
static int release_range_read;
static int watch_regular_io_lock;
static int regular_io_lock_entered;
static int fail_truncate;
static int mount_sync_error;
static unsigned pages_allocated;
static unsigned pages_freed;
static unsigned stacked_legacy_write_calls;

ssize_t overlay_content_host_pwrite(struct file *, struct file *, const void *,
	size_t, off_t, unsigned, const struct ucred *);
int overlay_content_host_truncate(struct inode *, struct inode *,
	const struct inode_truncate_request *, struct inode_truncate_result *);
int overlay_content_host_layers_supported(int, int);
void exec_commit_host_release_lease(struct file_content_lease *);

unsigned vm_test_waitq_sleep_count(void);
void vm_test_waitq_wait_for_sleep(unsigned);

void
vm_object_read_checkpoint(struct inode *inode, size_t done, size_t length)
{
	(void)inode;
	if (!block_range_read || done >= length)
		return;
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	range_read_entered++;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	while (!release_range_read)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
}

void
file_regular_io_lock_checkpoint(struct inode *inode)
{
	(void)inode;
	if (!watch_regular_io_lock)
		return;
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	regular_io_lock_entered++;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
}

void *
kern_malloc(size_t size)
{
	return malloc(size);
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

void
hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "%s:%d: %s\n", file, line, message);
	abort();
}

bool
hal_irq_disable(void)
{
	return false;
}

void
hal_irq_enable(void)
{
}

int
hal_pmem_alloc(const struct hal_pmem_request *request, struct hal_pmem *memory)
{
	void *allocation;

	allocation = aligned_alloc(request->alignment, request->size);
	if (allocation == NULL)
		return HAL_ERR_NOMEM;
	memset(allocation, 0, request->size);
	memory->vaddr = allocation;
	memory->paddr = (hal_physaddr_t)(uintptr_t)allocation;
	memory->size = request->size;
	memory->type = request->type;
	memory->attr = request->attr;
	pages_allocated++;
	return HAL_OK;
}

int
hal_pmem_free(struct hal_pmem *memory)
{
	free(memory->vaddr);
	memset(memory, 0, sizeof(*memory));
	pages_freed++;
	return HAL_OK;
}

int
hal_page_query(hal_space_t space, void *address, uint32_t *flags)
{
	(void)space;
	(void)address;
	if (flags != NULL)
		*flags = 0;
	return HAL_OK;
}

int
hal_page_clear_flags(hal_space_t space, void *address, uint32_t flags)
{
	(void)space;
	(void)address;
	(void)flags;
	return HAL_OK;
}

int
hal_page_unmap(hal_space_t space, void *address, size_t size)
{
	(void)space;
	(void)address;
	(void)size;
	return HAL_OK;
}

void
vm_page_free_metadata(struct vm_page *page)
{
	/* This test faults object pages directly and never installs a mapping. */
	assert(page == NULL);
}

int
vm_reclaim_one(struct vm_page *page)
{
	(void)page;
	return ENOMEM;
}

void
path_release(struct path *path)
{
	/* Object-held file references never consume the fixture's base ref. */
	assert(path == NULL);
}

int
mount_sync(struct mount *mount)
{
	assert(mount != NULL);
	return mount_sync_error;
}

void
record_lock_inode_destroy(struct inode *inode)
{
	(void)inode;
}

void
clock_realtime(time_t *seconds, long *nanoseconds)
{
	if (seconds != NULL)
		*seconds = 1;
	if (nanoseconds != NULL)
		*nanoseconds = 0;
}

int
vfs_clear_setid_on_write(struct inode *inode, const struct ucred *credential)
{
	assert(inode != NULL && credential != NULL);
	assert(mutex_owned(&inode->i_io_lock));
	clear_setid_calls++;
	if (clear_setid_error != 0)
		return clear_setid_error;
	inode->i_mode &= ~(mode_t)(S_ISUID | S_ISGID);
	setid_clear_succeeded = 1;
	return 0;
}

int
vfs_clear_setid_on_content_change(struct inode *inode)
{
	assert(inode != NULL);
	assert(mutex_owned(&inode->i_io_lock));
	inode->i_mode &= ~(mode_t)(S_ISUID | S_ISGID);
	setid_clear_succeeded = 1;
	return 0;
}

static ssize_t
fake_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	struct fake_backing *backing = file->f_data;
	uint64_t end = (uint64_t)offset + length;

	assert(offset >= 0 && end >= (uint64_t)offset &&
	    end <= (uint64_t)backing->size);
	memcpy(buffer, backing->bytes + offset, length);
	backing->reads++;
	return (ssize_t)length;
}

static ssize_t
fake_pwrite(struct file *file, const void *buffer, size_t length, off_t offset)
{
	struct fake_backing *backing = file->f_data;
	uint64_t end = (uint64_t)offset + length;

	if (offset < 0 || end < (uint64_t)offset || end > BACKING_CAPACITY)
		return -EFBIG;
	if (require_clear_before_pwrite)
		assert(setid_clear_succeeded);
	if (pwrite_error != 0)
		return -pwrite_error;
	if (pwrite_limit != 0 && length > pwrite_limit) {
		length = pwrite_limit;
		end = (uint64_t)offset + length;
	}
	if (block_pwrite) {
		assert(mtx_lock(&checkpoint_lock) == thrd_success);
		pwrite_entered++;
		assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
		while (!release_pwrite)
			assert(cnd_wait(&checkpoint_condition,
			    &checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	}
	if (offset > backing->size)
		memset(backing->bytes + backing->size, 0,
		    (size_t)(offset - backing->size));
	memcpy(backing->bytes + offset, buffer, length);
	if ((off_t)end > backing->size)
		backing->size = (off_t)end;
	if ((off_t)end > file->f_inode->i_size)
		file->f_inode->i_size = (off_t)end;
	backing->writes++;
	return (ssize_t)length;
}

static int
fake_fsync(struct file *file)
{
	(void)file;
	return 0;
}

static ssize_t
fake_write(struct file *file, const void *buffer, size_t length)
{
	ssize_t count = fake_pwrite(file, buffer, length, file->f_offset);

	if (count > 0)
		file->f_offset += count;
	return count;
}

static const struct file_ops fake_file_ops = {
	.write = fake_write,
	.pread = fake_pread,
	.pwrite = fake_pwrite,
	.fsync = fake_fsync,
};

static int
fake_truncate(struct inode *inode, off_t size)
{
	struct fake_backing *backing = inode->i_data;
	off_t old_size = backing->size;

	if (fail_truncate)
		return EIO;
	assert(size >= 0 && (uint64_t)size <= BACKING_CAPACITY);
	if (size < old_size)
		memset(backing->bytes + size, 0, (size_t)(old_size - size));
	else if (size > old_size)
		memset(backing->bytes + old_size, 0, (size_t)(size - old_size));
	backing->size = size;
	inode->i_size = size;
	return 0;
}

static int
fake_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	(void)inode;
	(void)status;
	(void)mask;
	return 0;
}

static const struct inode_ops fake_inode_ops = {
	.truncate = fake_truncate,
	.setattr = fake_setattr,
};

static void
fixture_init(struct fixture *fixture, off_t size, uint8_t fill)
{
	memset(fixture, 0, sizeof(*fixture));
	assert(size >= 0 && (uint64_t)size <= BACKING_CAPACITY);
	memset(fixture->backing.bytes, fill, (size_t)size);
	fixture->backing.size = size;
	fixture->inode.i_type = INODE_REG;
	fixture->inode.i_size = size;
	fixture->inode.i_op = &fake_inode_ops;
	fixture->inode.i_fop = &fake_file_ops;
	fixture->inode.i_data = &fixture->backing;
	assert(mutex_init(&fixture->inode.i_io_lock, LOCK_RANK_INODE_IO,
	    "test inode I/O") == 0);
	spin_init(&fixture->inode.i_vm_lock, LOCK_RANK_VM_RESIZE,
	    "test inode resize");
	waitq_init(&fixture->inode.i_vm_waitq, "test inode resize");
	fixture->file.f_inode = &fixture->inode;
	fixture->file.f_vm_inode = &fixture->inode;
	fixture->file.f_ops = &fake_file_ops;
	fixture->file.f_data = &fixture->backing;
	atomic_store_release(&fixture->file.f_flags, O_RDWR);
	refcount_init(&fixture->file.f_refs, 1);
	assert(mutex_init(&fixture->file.f_lock, LOCK_RANK_FILE,
	    "test file") == 0);
	assert(vm_object_get_shared(&fixture->file, &fixture->object) == 0);
	assert(fixture->object->logical_size == size);
}

static void
fixture_close(struct fixture *fixture)
{
	vm_object_put(fixture->object);
	fixture->object = NULL;
	assert(vm_object_count() == 0);
	assert(vm_object_page_count() == 0);
	assert(refcount_load(&fixture->file.f_refs) == 1);
}

struct stacked_file_info {
	struct file *lower;
};

static ssize_t
stacked_pread_internal(struct file *file, void *buffer, size_t length,
	off_t offset, unsigned flags)
{
	struct stacked_file_info *info = file->f_data;

	assert(info != NULL && info->lower != NULL);
	return file_pread_internal(info->lower, buffer, length, offset,
	    flags | FILE_IO_VM_OBJECT);
}

static ssize_t
stacked_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	return stacked_pread_internal(file, buffer, length, offset, 0);
}

static ssize_t
stacked_pwrite_internal(struct file *file, const void *buffer, size_t length,
	off_t offset, unsigned flags, const struct ucred *credential)
{
	struct stacked_file_info *info = file->f_data;
	ssize_t result;

	assert(info != NULL && info->lower != NULL);
	result = overlay_content_host_pwrite(file, info->lower, buffer, length,
	    offset, flags, credential);
	return result;
}

static ssize_t
stacked_pwrite(struct file *file, const void *buffer, size_t length,
	off_t offset)
{
	return stacked_pwrite_internal(file, buffer, length, offset, 0, NULL);
}

static ssize_t
stacked_write(struct file *file, const void *buffer, size_t length)
{
	(void)file;
	(void)buffer;
	(void)length;
	stacked_legacy_write_calls++;
	return -EIO;
}

static const struct file_ops stacked_file_ops = {
	.write = stacked_write,
	.pread = stacked_pread,
	.pwrite = stacked_pwrite,
	.pread_internal = stacked_pread_internal,
	.pwrite_internal = stacked_pwrite_internal,
};

static void
stacked_file_init(struct fixture *fixture, struct inode *outer_inode,
	struct file *outer_file, struct stacked_file_info *info)
{
	memset(outer_inode, 0, sizeof(*outer_inode));
	memset(outer_file, 0, sizeof(*outer_file));
	outer_inode->i_type = INODE_REG;
	outer_inode->i_size = fixture->inode.i_size;
	outer_inode->i_mode = S_IFREG | S_ISUID | S_ISGID | 0755;
	outer_inode->i_op = &fake_inode_ops;
	assert(mutex_init(&outer_inode->i_io_lock, LOCK_RANK_INODE_IO,
	    "stacked outer inode I/O") == 0);
	spin_init(&outer_inode->i_vm_lock, LOCK_RANK_VM_RESIZE,
	    "stacked outer inode resize");
	waitq_init(&outer_inode->i_vm_waitq, "stacked outer inode resize");
	info->lower = &fixture->file;
	outer_file->f_inode = outer_inode;
	outer_file->f_vm_inode = &fixture->inode;
	outer_file->f_ops = &stacked_file_ops;
	outer_file->f_data = info;
	atomic_store_release(&outer_file->f_flags, O_RDWR);
	refcount_init(&outer_file->f_refs, 1);
	assert(mutex_init(&outer_file->f_lock, LOCK_RANK_FILE,
	    "stacked outer file") == 0);
}

static struct vm_object_page *
fixture_fault(struct fixture *fixture)
{
	struct vm_object_page *page = NULL;

	assert(vm_object_fault(fixture->object, 0, &page) == 0);
	assert(page != NULL);
	return page;
}

static void
fake_backing_resize(struct fixture *fixture, off_t size)
{
	off_t old_size = fixture->backing.size;

	assert(size >= 0 && (uint64_t)size <= BACKING_CAPACITY);
	if (size < old_size)
		memset(fixture->backing.bytes + size, 0,
		    (size_t)(old_size - size));
	else if (size > old_size)
		memset(fixture->backing.bytes + old_size, 0,
		    (size_t)(size - old_size));
	fixture->backing.size = size;
	fixture->inode.i_size = size;
}

static int
fixture_resize(struct fixture *fixture, off_t size, int fail_mutation)
{
	struct vm_object_resize resize;
	int error;

	mutex_lock(&fixture->inode.i_io_lock);
	error = vm_object_resize_begin(&fixture->inode, size, &resize);
	assert(error == 0 && (resize.active || size == fixture->inode.i_size));
	if (!resize.active) {
		mutex_unlock(&fixture->inode.i_io_lock);
		return fail_mutation ? EIO : 0;
	}
	mutex_unlock(&fixture->inode.i_io_lock);
	error = vm_object_resize_prepare(&resize);
	mutex_lock(&fixture->inode.i_io_lock);
	if (error != 0 || fail_mutation) {
		vm_object_resize_abort(&resize);
		mutex_unlock(&fixture->inode.i_io_lock);
		return error != 0 ? error : EIO;
	}
	fake_backing_resize(fixture, size);
	vm_object_resize_commit(&resize, size);
	mutex_unlock(&fixture->inode.i_io_lock);
	return 0;
}

struct fault_args {
	struct vm_object *object;
	struct vm_object_page *page;
	int error;
};

static int
fault_thread(void *opaque)
{
	struct fault_args *args = opaque;

	args->error = vm_object_fault(args->object, 0, &args->page);
	return 0;
}

struct prepare_args {
	struct vm_object_resize *resize;
	int error;
};

static int
prepare_thread(void *opaque)
{
	struct prepare_args *args = opaque;

	args->error = vm_object_resize_prepare(args->resize);
	return 0;
}

struct pin_write_args {
	struct vm_object_page *page;
	size_t offset;
	uint8_t value;
	int error;
};

static int
pin_write_thread(void *opaque)
{
	struct pin_write_args *args = opaque;

	args->error = vm_object_page_pin_write(args->page, args->offset,
	    &args->value, 1);
	return 0;
}

static void
start_resize(struct fixture *fixture, off_t size,
	struct vm_object_resize *resize)
{
	mutex_lock(&fixture->inode.i_io_lock);
	assert(vm_object_resize_begin(&fixture->inode, size, resize) == 0);
	assert(resize->active);
	mutex_unlock(&fixture->inode.i_io_lock);
}

static void
commit_resize(struct fixture *fixture, struct vm_object_resize *resize,
	off_t size)
{
	mutex_lock(&fixture->inode.i_io_lock);
	fake_backing_resize(fixture, size);
	vm_object_resize_commit(resize, size);
	mutex_unlock(&fixture->inode.i_io_lock);
}

static void
test_fault_waits_for_resize(void)
{
	struct fixture fixture;
	struct vm_object_resize resize;
	struct prepare_args unused;
	struct fault_args fault = { 0 };
	thrd_t thread;
	unsigned before_sleeps;

	(void)unused;
	fixture_init(&fixture, 128, 0x31);
	start_resize(&fixture, 256, &resize);
	fault.object = fixture.object;
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&thread, fault_thread, &fault) == thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	assert(vm_object_resize_prepare(&resize) == 0);
	commit_resize(&fixture, &resize, 256);
	assert(thrd_join(thread, NULL) == thrd_success);
	assert(fault.error == 0 && fault.page != NULL);
	vm_object_fault_release(fault.page);
	fixture_close(&fixture);
}

static void
test_shrink_grow_zero(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	uint8_t *bytes;
	unsigned i;

	fixture_init(&fixture, 300, 0x5a);
	page = fixture_fault(&fixture);
	vm_object_fault_release(page);
	assert(fixture_resize(&fixture, 100, 0) == 0);
	assert(fixture_resize(&fixture, 300, 0) == 0);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	for (i = 0; i < 100; i++)
		assert(bytes[i] == 0x5a);
	for (; i < 300; i++)
		assert(bytes[i] == 0);
	vm_object_fault_release(page);
	fixture_close(&fixture);
}

static void
test_mmap_tail_does_not_reappear(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	uint8_t *bytes;

	fixture_init(&fixture, 100, 0x41);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	bytes[50] = 0x50;
	bytes[200] = 0xee;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	assert(fixture_resize(&fixture, 300, 0) == 0);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	assert(bytes[50] == 0x50);
	assert(bytes[200] == 0);
	vm_object_fault_release(page);
	fixture_close(&fixture);
}

static void
test_failed_resize_aborts(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	uint8_t *bytes;

	fixture_init(&fixture, 100, 0x42);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	bytes[20] = 0x64;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	assert(fixture_resize(&fixture, 300, 1) == EIO);
	assert(fixture.inode.i_size == 100 && fixture.backing.size == 100);
	assert(fixture.object->logical_size == 100);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	assert(bytes[20] == 0x64 && bytes[200] == 0);
	vm_object_fault_release(page);
	fixture_close(&fixture);
}

static void
test_generic_truncate_and_write_routes(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	uint8_t *bytes;
	uint8_t value = 0x77;

	fixture_init(&fixture, 100, 0x45);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	bytes[80] = 0x51;
	bytes[120] = 0xee;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	assert(inode_truncate(&fixture.inode, 50) == 0);
	assert(fixture.inode.i_size == 50 && fixture.backing.size == 50 &&
	    fixture.object->logical_size == 50);
	assert(inode_truncate(&fixture.inode, 100) == 0);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	assert(bytes[49] == 0x45 && bytes[80] == 0);
	bytes[120] = 0xef;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	assert(file_pwrite(&fixture.file, &value, 1, 150) == 1);
	assert(fixture.inode.i_size == 151 && fixture.backing.size == 151 &&
	    fixture.object->logical_size == 151);
	{
		int limit_exceeded = -1;

		/* Lowering a limit below the existing size must still permit a
		 * shrink, but not a subsequent extension which remains above it. */
		assert(inode_truncate_limited(&fixture.inode, 120, 50,
		    &limit_exceeded) == 0);
		assert(limit_exceeded == 0 && fixture.inode.i_size == 120);
		assert(inode_truncate_limited(&fixture.inode, 121, 50,
		    &limit_exceeded) == EFBIG);
		assert(limit_exceeded == 1 && fixture.inode.i_size == 120);
	}
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	assert(bytes[119] == 0);
	vm_object_fault_release(page);
	fail_truncate = 1;
	assert(inode_truncate(&fixture.inode, 25) == EIO);
	fail_truncate = 0;
	assert(fixture.inode.i_size == 120 && fixture.backing.size == 120 &&
	    fixture.object->logical_size == 120);
	fixture_close(&fixture);
}

static void
test_stacked_truncate_uses_final_limit_domain(void)
{
	struct inode_truncate_request request;
	struct inode_truncate_result result;
	struct fixture fixture;
	struct inode outer_inode;
	struct file outer_file;
	struct stacked_file_info info;
	struct ucred credential;

	memset(&credential, 0, sizeof(credential));
	credential.euid = 1000;
	memset(&request, 0, sizeof(request));
	request.growth_limit = 50;
	request.credential = &credential;
	assert(overlay_content_host_layers_supported(0, 0) == 0);
	assert(overlay_content_host_layers_supported(1, 0) == EOPNOTSUPP);
	assert(overlay_content_host_layers_supported(0, 1) == EOPNOTSUPP);

	/* The visible wrapper is stale-small, but the requested size shrinks the
	 * authoritative final inode and therefore cannot violate RLIMIT_FSIZE. */
	fixture_init(&fixture, TEST_PAGE_SIZE, 0x45);
	stacked_file_init(&fixture, &outer_inode, &outer_file, &info);
	outer_inode.i_size = 7;
	request.size = 100;
	assert(overlay_content_host_truncate(&outer_inode, &fixture.inode,
	    &request, &result) == 0);
	assert(!result.limit_exceeded && result.actual_size == 100);
	assert(fixture.inode.i_size == 100 && fixture.backing.size == 100);
	assert(outer_inode.i_size == 100);
	fixture_close(&fixture);

	/* The opposite stale mirror must not allow growth beyond the limit.  The
	 * error path refreshes the visible size from the final inode. */
	fixture_init(&fixture, 7, 0x46);
	stacked_file_init(&fixture, &outer_inode, &outer_file, &info);
	outer_inode.i_size = TEST_PAGE_SIZE;
	request.size = 100;
	assert(overlay_content_host_truncate(&outer_inode, &fixture.inode,
	    &request, &result) == EFBIG);
	assert(result.limit_exceeded && result.actual_size == 7);
	assert(fixture.inode.i_size == 7 && fixture.backing.size == 7);
	assert(outer_inode.i_size == 7);
	fixture_close(&fixture);

	/* A successful backend truncate followed by a durability error has still
	 * committed both EOF and set-id removal.  The outer resize abort path must
	 * not restore its stale pre-call metadata. */
	fixture_init(&fixture, 100, 0x47);
	stacked_file_init(&fixture, &outer_inode, &outer_file, &info);
	fixture.inode.i_mode = S_IFREG | S_ISUID | S_ISGID | 0755;
	outer_inode.i_mode = fixture.inode.i_mode;
	outer_inode.i_size = 7;
	request.size = 50;
	request.growth_limit = UINT64_MAX;
	mount_sync_error = EIO;
	assert(overlay_content_host_truncate(&outer_inode, &fixture.inode,
	    &request, &result) == EIO);
	mount_sync_error = 0;
	assert(result.actual_size == 50 && !result.limit_exceeded);
	assert(fixture.inode.i_size == 50 && fixture.backing.size == 50);
	assert(outer_inode.i_size == 50);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	assert((outer_inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	fixture_close(&fixture);
}

static void
test_regular_io_uses_shared_cache_domain(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	uint8_t write_bytes[4] = { 0x70, 0x71, 0x72, 0x73 };
	uint8_t read_byte = 0;
	uint8_t *bytes;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x41);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	bytes[10] = 0x55;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	/* The old dirty page is committed before the non-overlapping pwrite, so
	 * its whole-page writeback cannot later overwrite the pwrite bytes. */
	assert(file_pwrite(&fixture.file, write_bytes, sizeof(write_bytes), 100) ==
	    (ssize_t)sizeof(write_bytes));
	assert(fixture.backing.bytes[10] == 0x55);
	assert(memcmp(fixture.backing.bytes + 100, write_bytes,
	    sizeof(write_bytes)) == 0);
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	assert(bytes[10] == 0x55);
	assert(memcmp(bytes + 100, write_bytes, sizeof(write_bytes)) == 0);
	bytes[25] = 0x66;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	/* read/pread observes dirty MAP_SHARED cache data, not stale backend data. */
	assert(file_pread(&fixture.file, &read_byte, 1, 25) == 1);
	assert(read_byte == 0x66);
	fixture_close(&fixture);
}

static void
test_regular_write_short_and_error_prefix(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	const uint8_t write_bytes[4] = { 0x61, 0x62, 0x63, 0x64 };
	uint8_t *bytes;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x40);
	page = fixture_fault(&fixture);
	vm_object_fault_release(page);
	pwrite_limit = 2;
	assert(file_pwrite(&fixture.file, write_bytes, sizeof(write_bytes), 200) ==
	    2);
	pwrite_limit = 0;
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	assert(bytes[200] == 0x61 && bytes[201] == 0x62);
	assert(bytes[202] == 0x40 && bytes[203] == 0x40);
	vm_object_fault_release(page);
	pwrite_error = EIO;
	assert(file_pwrite(&fixture.file, write_bytes, sizeof(write_bytes), 300) ==
	    -EIO);
	pwrite_error = 0;
	page = fixture_fault(&fixture);
	bytes = page->pmem.vaddr;
	assert(bytes[300] == 0x40 && bytes[303] == 0x40);
	vm_object_fault_release(page);
	fixture_close(&fixture);
}

static void
test_regular_write_setid_transition_precedes_backend(void)
{
	struct fixture fixture;
	struct file_io io;
	struct ucred credential;
	uint8_t first = 0x71, second = 0x72;
	unsigned calls_before;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x40);
	memset(&credential, 0, sizeof(credential));
	credential.euid = 1000;
	fixture.inode.i_mode = S_IFREG | S_ISUID | S_ISGID | 0755;
	clear_setid_error = EIO;
	clear_setid_calls = 0;
	setid_clear_succeeded = 0;
	require_clear_before_pwrite = 1;
	assert(file_io_begin_cred(&fixture.file, FILE_IO_PWRITE, 0, 0,
	    &credential, &io) == 0);
	assert(file_io_transfer(&io, &first, 1) == -EIO);
	file_io_end(&io);
	assert(clear_setid_calls == 1 && fixture.backing.writes == 0);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) ==
	    (S_ISUID | S_ISGID));

	/* A successful transition is performed once for the whole file_io even
	 * when it contains multiple syscall/iovec chunks. */
	clear_setid_error = 0;
	calls_before = clear_setid_calls;
	assert(file_io_begin_cred(&fixture.file, FILE_IO_PWRITE, 0, 0,
	    &credential, &io) == 0);
	assert(file_io_transfer(&io, &first, 1) == 1);
	assert(file_io_transfer(&io, &second, 1) == 1);
	file_io_end(&io);
	assert(clear_setid_calls == calls_before + 1U);
	assert(fixture.backing.writes == 2);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	require_clear_before_pwrite = 0;
	setid_clear_succeeded = 0;
	fixture_close(&fixture);
}

static void
test_stacked_write_uses_final_content_and_setid_domain(void)
{
	struct fixture fixture;
	struct inode outer_inode;
	struct file outer_file;
	struct stacked_file_info info;
	struct file_io io;
	struct ucred credential;
	struct stat status;
	uint8_t first = 0x51, second = 0x52, content = 0x53, extension = 0x54;
	unsigned calls;
	off_t extension_offset = TEST_PAGE_SIZE + 10;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x40);
	stacked_file_init(&fixture, &outer_inode, &outer_file, &info);
	memset(&credential, 0, sizeof(credential));
	credential.euid = 1000;
	fixture.inode.i_mode = S_IFREG | S_ISUID | S_ISGID | 0755;
	clear_setid_error = 0;
	clear_setid_calls = 0;
	setid_clear_succeeded = 0;
	require_clear_before_pwrite = 1;

	/* The visible layer stays locked for the whole operation.  Every nested
	 * final-layer transfer nevertheless removes privilege immediately before
	 * its own backend mutation. */
	assert(file_io_begin_cred(&outer_file, FILE_IO_PWRITE, 0, 0,
	    &credential, &io) == 0);
	assert(file_io_transfer(&io, &first, 1) == 1);
	assert((outer_inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);

	/* Only the final content inode clears privilege bits.  The stacked layer
	 * mirrors that authoritative metadata after the nested backend mutation. */
	assert(clear_setid_calls == 1);

	/* The final i_io lock spans every outer syscall chunk; nested transfers
	 * still perform their own final-backend transition under that ownership. */
	calls = clear_setid_calls;
	assert(file_io_transfer(&io, &second, 1) == 1);
	file_io_end(&io);
	assert(clear_setid_calls == calls + 1U);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);

	/* A direct alias can restore set-id only after the whole outer operation.
	 * The next outer mutation then removes it before touching backend bytes. */
	memset(&status, 0, sizeof(status));
	status.st_mode = fixture.inode.i_mode | S_ISUID | S_ISGID;
	assert(inode_setattr(&fixture.inode, &status, INODE_ATTR_MODE) == 0);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) ==
	    (S_ISUID | S_ISGID));
	setid_clear_succeeded = 0;
	assert(file_io_begin_cred(&outer_file, FILE_IO_PWRITE, 2, 0,
	    &credential, &io) == 0);
	assert(file_io_transfer(&io, &second, 1) == 1);
	file_io_end(&io);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);

	/* Growth-limit replacement is judged against the authoritative final EOF.
	 * A stale, smaller overlay mirror must not turn an in-range overwrite into
	 * EFBIG. */
	outer_inode.i_size = 7;
	assert(file_io_begin_cred(&outer_file, FILE_IO_PWRITE, 100, 0,
	    &credential, &io) == 0);
	file_io_set_growth_limit(&io, 8);
	assert(file_io_transfer(&io, &second, 1) == 1);
	assert(!file_io_take_growth_limit_hit(&io));
	file_io_end(&io);
	assert(fixture.backing.bytes[100] == second);

	/* A stale-large visible EOF likewise cannot authorize growth past the
	 * final inode's limit. */
	assert(inode_truncate(&fixture.inode, 7) == 0);
	outer_inode.i_size = TEST_PAGE_SIZE;
	assert(file_io_begin_cred(&outer_file, FILE_IO_PWRITE, 10, 0,
	    &credential, &io) == 0);
	file_io_set_growth_limit(&io, 8);
	assert(file_io_transfer(&io, &second, 1) == -EFBIG);
	assert(file_io_take_growth_limit_hit(&io));
	file_io_end(&io);
	assert(fixture.inode.i_size == 7 && fixture.backing.size == 7);
	assert(inode_truncate(&fixture.inode, TEST_PAGE_SIZE) == 0);
	outer_inode.i_size = TEST_PAGE_SIZE;

	/* Set-id removal is irreversible even if the final backend reports an
	 * error; the actual overlay callback must refresh its visible metadata. */
	fixture.inode.i_mode |= S_ISUID | S_ISGID;
	outer_inode.i_mode |= S_ISUID | S_ISGID;
	setid_clear_succeeded = 0;
	pwrite_error = EIO;
	assert(file_pwrite_internal_cred(&outer_file, &second, 1, 101, 0,
	    &credential) == -EIO);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	assert((outer_inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	pwrite_error = 0;

	/* O_APPEND is based on the final content inode, not a stale overlay mirror,
	 * and the outer open-file description receives the committed final offset. */
	outer_inode.i_size = 7;
	outer_file.f_offset = 3;
	atomic_store_release(&outer_file.f_flags, O_RDWR | O_APPEND);
	assert(file_io_begin_cred(&outer_file, FILE_IO_WRITE, 0, 0,
	    &credential, &io) == 0);
	assert(file_io_transfer(&io, &first, 1) == 1);
	file_io_end(&io);
	assert(fixture.backing.bytes[TEST_PAGE_SIZE] == first);
	assert(fixture.inode.i_size == TEST_PAGE_SIZE + 1);
	assert(outer_inode.i_size == TEST_PAGE_SIZE + 1);
	assert(outer_file.f_offset == TEST_PAGE_SIZE + 1);
	atomic_store_release(&outer_file.f_flags, O_RDWR);

	/* Ordinary write/writev uses the credential-aware positional stacking
	 * callback while the outer file_io continues to own f_offset. */
	fixture.inode.i_mode |= S_ISUID | S_ISGID;
	outer_inode.i_mode |= S_ISUID | S_ISGID;
	stacked_legacy_write_calls = 0;
	outer_file.f_offset = 8;
	assert(file_io_begin_cred(&outer_file, FILE_IO_WRITE, 0, 0,
	    &credential, &io) == 0);
	assert(file_io_transfer(&io, &second, 1) == 1);
	file_io_end(&io);
	assert(stacked_legacy_write_calls == 0);
	assert(outer_file.f_offset == 9);
	assert(fixture.backing.bytes[8] == second);
	assert((outer_inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);

	/* MAP_SHARED writeback carries no user credential.  CONTENT_CHANGE passes
	 * through every stacking layer and clears only at the final content inode. */
	fixture.inode.i_mode |= S_ISUID | S_ISGID;
	setid_clear_succeeded = 0;
	assert(file_pwrite_internal(&outer_file, &content, 1, 2,
	    FILE_IO_VM_OBJECT | FILE_IO_CONTENT_CHANGE) == 1);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);

	/* Generic EOF/content publication is keyed by f_vm_inode, not the visible
	 * wrapper inode.  An extending stacked write must update the shared object. */
	fixture.inode.i_mode |= S_ISUID | S_ISGID;
	setid_clear_succeeded = 0;
	assert(file_pwrite_internal_cred(&outer_file, &extension, 1,
	    extension_offset, 0, &credential) == 1);
	assert(fixture.inode.i_size == extension_offset + 1);
	assert(fixture.object->logical_size == extension_offset + 1);
	assert(outer_inode.i_size == extension_offset + 1);
	assert((fixture.inode.i_mode & (S_ISUID | S_ISGID)) == 0);
	require_clear_before_pwrite = 0;
	setid_clear_succeeded = 0;
	fixture_close(&fixture);
}

struct regular_write_args {
	struct file *file;
	uint8_t value;
	ssize_t result;
};

struct exec_waiter_args {
	struct file *file;
	uint8_t value;
	ssize_t result;
	int done;
};

struct exec_boundary_state {
	struct file_content_lease *lease;
	struct exec_waiter_args *waiter;
	unsigned released;
	unsigned retirement;
};

struct append_write_args {
	struct file *file;
	const struct ucred *credential;
	uint8_t value;
	ssize_t result;
};

struct sync_args {
	struct vm_object *object;
	int result;
};

static int
regular_write_thread(void *opaque)
{
	struct regular_write_args *args = opaque;

	args->result = file_pwrite(args->file, &args->value, 1, 100);
	return 0;
}

static int
exec_waiter_thread(void *opaque)
{
	struct exec_waiter_args *args = opaque;

	args->result = file_pwrite(args->file, &args->value, 1, 100);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	args->done = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	return 0;
}

static void
exec_boundary_checkpoint(enum kern_test_checkpoint_id id, void *object,
	void *opaque)
{
	struct exec_boundary_state *state = opaque;

	(void)object;
	if (id == KERN_TEST_EXEC_LEASE_RELEASED) {
		assert(state->lease != NULL && !state->lease->active);
		assert(mtx_lock(&checkpoint_lock) == thrd_success);
		while (!state->waiter->done)
			assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
			    thrd_success);
		state->released++;
		assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	} else if (id == KERN_TEST_EXEC_RETIREMENT_BEGIN) {
		assert(state->released == 1U && state->waiter->done);
		assert(state->lease != NULL && !state->lease->active);
		state->retirement++;
	}
}

static int
append_write_thread(void *opaque)
{
	struct append_write_args *args = opaque;
	struct file_io io;
	int error;

	error = file_io_begin_cred(args->file, FILE_IO_WRITE, 0, 0,
	    args->credential, &io);
	if (error != 0) {
		args->result = -error;
		return 0;
	}
	args->result = file_io_transfer(&io, &args->value, 1);
	file_io_end(&io);
	return 0;
}

static void
test_stacked_append_retries_final_eof_after_alias_writer(void)
{
	struct fixture fixture;
	struct inode outer_inode;
	struct file outer_file;
	struct stacked_file_info info;
	struct append_write_args alias = { 0 };
	struct ucred credential;
	struct file_io io;
	thrd_t alias_thread;
	uint8_t first = 0x60, second = 0x61;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x40);
	stacked_file_init(&fixture, &outer_inode, &outer_file, &info);
	memset(&credential, 0, sizeof(credential));
	credential.euid = 1000;
	outer_inode.i_size = 7;
	outer_file.f_offset = 3;
	atomic_store_release(&outer_file.f_flags, O_RDWR | O_APPEND);
	atomic_store_release(&fixture.file.f_flags, O_RDWR | O_APPEND);
	assert(file_io_begin_cred(&outer_file, FILE_IO_WRITE, 0, 0,
	    &credential, &io) == 0);
	assert(file_io_transfer(&io, &first, 1) == 1);

	/* A direct alias reaches the final lock after the first outer chunk, but
	 * cannot select an append position until the whole outer file_io ends. */
	alias.file = &fixture.file;
	alias.credential = &credential;
	alias.value = 0x62;
	watch_regular_io_lock = 1;
	regular_io_lock_entered = 0;
	assert(thrd_create(&alias_thread, append_write_thread, &alias) ==
	    thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (regular_io_lock_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	assert(file_io_transfer(&io, &second, 1) == 1);
	file_io_end(&io);
	assert(thrd_join(alias_thread, NULL) == thrd_success);
	watch_regular_io_lock = 0;
	assert(alias.result == 1);
	assert(fixture.backing.bytes[TEST_PAGE_SIZE] == first);
	assert(fixture.backing.bytes[TEST_PAGE_SIZE + 1] == second);
	assert(fixture.backing.bytes[TEST_PAGE_SIZE + 2] == 0x62);
	assert(fixture.inode.i_size == TEST_PAGE_SIZE + 3);
	assert(outer_file.f_offset == TEST_PAGE_SIZE + 2);
	fixture_close(&fixture);
}

static int
sync_thread(void *opaque)
{
	struct sync_args *args = opaque;

	args->result = vm_object_sync_range(args->object, 0, TEST_PAGE_SIZE,
	    MS_SYNC);
	return 0;
}

static void
test_sync_and_regular_write_have_no_busy_io_cycle(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	struct sync_args sync = { 0 };
	struct regular_write_args write = { 0 };
	thrd_t sync_handle, write_handle;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x42);
	page = fixture_fault(&fixture);
	((uint8_t *)page->pmem.vaddr)[20] = 0x66;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);

	/* Sync owns i_io before publishing page BUSY.  A regular writer must wait
	 * at i_io, rather than publishing CONTENT and making sync wait on BUSY. */
	block_pwrite = 1;
	pwrite_entered = release_pwrite = 0;
	sync.object = fixture.object;
	assert(thrd_create(&sync_handle, sync_thread, &sync) == thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (pwrite_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);

	write.file = &fixture.file;
	write.value = 0x77;
	watch_regular_io_lock = 1;
	regular_io_lock_entered = 0;
	assert(thrd_create(&write_handle, regular_write_thread, &write) ==
	    thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (regular_io_lock_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	release_pwrite = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	assert(thrd_join(sync_handle, NULL) == thrd_success);
	assert(thrd_join(write_handle, NULL) == thrd_success);
	block_pwrite = 0;
	watch_regular_io_lock = 0;
	assert(sync.result == 0 && write.result == 1);
	assert(fixture.backing.bytes[20] == 0x66);
	assert(fixture.backing.bytes[100] == 0x77);
	fixture_close(&fixture);
}

static void
test_exclusive_content_lease_spans_positional_reads(void)
{
	struct fixture fixture;
	struct file_content_lease lease;
	struct vm_object_page *page;
	struct regular_write_args write = { 0 };
	thrd_t write_handle;
	unsigned before_sleeps;
	uint8_t first = 0, second = 0;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x41);
	page = fixture_fault(&fixture);
	((uint8_t *)page->pmem.vaddr)[20] = 0x66;
	vm_object_mark_dirty(page);
	vm_object_fault_release(page);
	assert(file_content_lease_begin(&fixture.file, &lease) == 0);
	/* Begin revoked the writable cache and made its dirty image the stable
	 * backend snapshot before returning. */
	assert(fixture.backing.bytes[20] == 0x66);
	assert(file_content_lease_pread(&lease, &first, 1, 20) == 1);

	write.file = &fixture.file;
	write.value = 0x77;
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&write_handle, regular_write_thread, &write) ==
	    thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	/* A second arbitrary-offset read belongs to the same lease; the writer
	 * cannot enter between ELF header/program/segment reads. */
	assert(fixture.backing.bytes[100] == 0x41);
	assert(file_content_lease_pread(&lease, &second, 1, 100) == 1);
	assert(first == 0x66 && second == 0x41);
	file_content_lease_end(&lease);
	assert(thrd_join(write_handle, NULL) == thrd_success);
	assert(write.result == 1 && fixture.backing.bytes[100] == 0x77);
	fixture_close(&fixture);
}

static void
test_exec_commit_releases_content_waiter_before_retirement(void)
{
	struct fixture fixture;
	struct file_content_lease lease;
	struct exec_waiter_args waiter = { 0 };
	struct exec_boundary_state boundary = { 0 };
	thrd_t writer;
	unsigned before_sleeps;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x41);
	assert(file_content_lease_begin(&fixture.file, &lease) == 0);
	waiter.file = &fixture.file;
	waiter.value = 0x7a;
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&writer, exec_waiter_thread, &waiter) ==
	    thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	assert(!waiter.done && fixture.backing.bytes[100] == 0x41);

	boundary.lease = &lease;
	boundary.waiter = &waiter;
	kern_test_checkpoint_set(exec_boundary_checkpoint, &boundary);
	/* This is the exact production helper called after final revalidation and
	 * HAL validation.  Its RELEASED checkpoint blocks until the real waiter
	 * completes, proving that RETIREMENT_BEGIN cannot retain the lease cycle. */
	exec_commit_host_release_lease(&lease);
	kern_test_checkpoint_set(NULL, NULL);
	assert(thrd_join(writer, NULL) == thrd_success);
	assert(waiter.result == 1 && waiter.done);
	assert(boundary.released == 1U && boundary.retirement == 1U);
	assert(fixture.backing.bytes[100] == 0x7a);
	fixture_close(&fixture);
}

static void
test_late_pin_write_follows_regular_write_commit(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	struct regular_write_args write = { 0 };
	struct pin_write_args pin = { 0 };
	thrd_t write_thread, pin_thread;
	unsigned before_sleeps;

	fixture_init(&fixture, TEST_PAGE_SIZE, 0x42);
	page = fixture_fault(&fixture);
	assert(vm_object_page_pin(page) == 0);
	vm_object_fault_release(page);
	block_pwrite = 1;
	pwrite_entered = release_pwrite = 0;
	write.file = &fixture.file;
	write.value = 0x77;
	assert(thrd_create(&write_thread, regular_write_thread, &write) ==
	    thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (pwrite_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	pin.page = page;
	pin.offset = 200;
	pin.value = 0x78;
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&pin_thread, pin_write_thread, &pin) == thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	release_pwrite = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	assert(thrd_join(write_thread, NULL) == thrd_success);
	assert(thrd_join(pin_thread, NULL) == thrd_success);
	block_pwrite = 0;
	assert(write.result == 1 && pin.error == 0);
	assert(((uint8_t *)page->pmem.vaddr)[100] == 0x77);
	assert(((uint8_t *)page->pmem.vaddr)[200] == 0x78);
	assert(fixture.backing.bytes[200] == 0x42);
	vm_object_page_unpin(page);
	assert(vm_object_sync_range(fixture.object, 0, TEST_PAGE_SIZE,
	    MS_SYNC) == 0);
	assert(fixture.backing.bytes[200] == 0x78);
	fixture_close(&fixture);
}

struct range_read_args {
	struct file *file;
	uint8_t *buffer;
	ssize_t result;
};

static int
range_read_thread(void *opaque)
{
	struct range_read_args *args = opaque;

	args->result = file_pread(args->file, args->buffer, BACKING_CAPACITY, 0);
	return 0;
}

struct range_write_args {
	struct file *file;
	off_t offset;
	uint8_t value;
	ssize_t result;
};

static int
chunked_read_thread(void *opaque)
{
	struct range_read_args *args = opaque;
	struct file_io io;
	ssize_t first, second;

	assert(file_io_begin(args->file, FILE_IO_PREAD, 0, 0, &io) == 0);
	first = file_io_transfer(&io, args->buffer, TEST_PAGE_SIZE);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	range_read_entered++;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	while (!release_range_read)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	second = file_io_transfer(&io, args->buffer + TEST_PAGE_SIZE,
	    TEST_PAGE_SIZE);
	file_io_end(&io);
	args->result = first < 0 ? first : second < 0 ? second : first + second;
	return 0;
}

static int
range_write_thread(void *opaque)
{
	struct range_write_args *args = opaque;

	args->result = file_pwrite(args->file, &args->value, 1, args->offset);
	return 0;
}

static void
test_multpage_read_holds_content_lease(void)
{
	struct fixture fixture;
	struct range_read_args read = { 0 };
	struct range_write_args write = { 0 };
	thrd_t read_thread, write_thread;
	unsigned before_sleeps;
	uint8_t *buffer = malloc(BACKING_CAPACITY);

	assert(buffer != NULL);
	fixture_init(&fixture, BACKING_CAPACITY, 0x31);
	read.file = &fixture.file;
	read.buffer = buffer;
	write.file = &fixture.file;
	write.offset = TEST_PAGE_SIZE + 10;
	write.value = 0x7a;
	block_range_read = 1;
	range_read_entered = release_range_read = 0;
	assert(thrd_create(&read_thread, range_read_thread, &read) ==
	    thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (range_read_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&write_thread, range_write_thread, &write) ==
	    thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	release_range_read = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	assert(thrd_join(read_thread, NULL) == thrd_success);
	assert(thrd_join(write_thread, NULL) == thrd_success);
	block_range_read = 0;
	assert(read.result == BACKING_CAPACITY && write.result == 1);
	assert(buffer[write.offset] == 0x31);
	assert(fixture.backing.bytes[write.offset] == write.value);
	free(buffer);
	fixture_close(&fixture);
}

static void
test_chunked_read_keeps_content_lease_to_end(void)
{
	struct fixture fixture;
	struct range_read_args read = { 0 };
	struct range_write_args write = { 0 };
	thrd_t read_thread, write_thread;
	unsigned before_sleeps;
	uint8_t *buffer = malloc(BACKING_CAPACITY);

	assert(buffer != NULL);
	fixture_init(&fixture, BACKING_CAPACITY, 0x35);
	read.file = &fixture.file;
	read.buffer = buffer;
	write.file = &fixture.file;
	write.offset = TEST_PAGE_SIZE + 20;
	write.value = 0x7b;
	range_read_entered = release_range_read = 0;
	assert(thrd_create(&read_thread, chunked_read_thread, &read) ==
	    thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (range_read_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&write_thread, range_write_thread, &write) ==
	    thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	release_range_read = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	assert(thrd_join(read_thread, NULL) == thrd_success);
	assert(thrd_join(write_thread, NULL) == thrd_success);
	assert(read.result == BACKING_CAPACITY && write.result == 1);
	assert(buffer[write.offset] == 0x35);
	assert(fixture.backing.bytes[write.offset] == write.value);
	free(buffer);
	fixture_close(&fixture);
}

static void
test_stacked_backend_read_excludes_final_alias_writer(void)
{
	struct fixture fixture;
	struct inode outer_inode;
	struct file outer_file;
	struct stacked_file_info info;
	struct range_read_args read = { 0 };
	struct range_write_args write = { 0 };
	thrd_t read_thread, write_thread;
	unsigned before_sleeps;
	uint8_t *buffer = malloc(BACKING_CAPACITY);

	assert(buffer != NULL);
	fixture_init(&fixture, BACKING_CAPACITY, 0x36);
	/* Exercise the backend-only branch: no shared object exists when the read
	 * begins, but direct access to the final inode must still be serialized. */
	vm_object_put(fixture.object);
	fixture.object = NULL;
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);
	stacked_file_init(&fixture, &outer_inode, &outer_file, &info);
	read.file = &outer_file;
	read.buffer = buffer;
	write.file = &fixture.file;
	write.offset = TEST_PAGE_SIZE + 30;
	write.value = 0x7c;
	range_read_entered = release_range_read = 0;
	assert(thrd_create(&read_thread, chunked_read_thread, &read) ==
	    thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (range_read_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&write_thread, range_write_thread, &write) ==
	    thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	release_range_read = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	assert(thrd_join(read_thread, NULL) == thrd_success);
	assert(thrd_join(write_thread, NULL) == thrd_success);
	assert(read.result == BACKING_CAPACITY && write.result == 1);
	assert(buffer[write.offset] == 0x36);
	assert(fixture.backing.bytes[write.offset] == write.value);
	assert(refcount_load(&fixture.file.f_refs) == 1);
	free(buffer);
}

static void
run_late_pin_write_case(int abort_resize)
{
	struct fixture fixture;
	struct vm_object_resize resize;
	struct vm_object_page *page;
	struct prepare_args prepare = { 0 };
	struct pin_write_args late = { 0 };
	thrd_t prepare_handle, write_handle;
	uint8_t prefix = 0x70;
	uint8_t tail = 0x74;
	uint8_t *bytes;
	unsigned before_sleeps;

	fixture_init(&fixture, abort_resize ? 100 : 300, 0x41);
	page = fixture_fault(&fixture);
	assert(vm_object_page_pin(page) == 0);
	vm_object_fault_release(page);
	assert(vm_object_page_pin_write(page, 50, &prefix, 1) == 0);
	if (!abort_resize)
		assert(vm_object_page_pin_write(page, 250, &tail, 1) == 0);
	start_resize(&fixture, abort_resize ? 300 : 100, &resize);
	prepare.resize = &resize;
	block_pwrite = 1;
	pwrite_entered = release_pwrite = 0;
	assert(thrd_create(&prepare_handle, prepare_thread, &prepare) ==
	    thrd_success);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	while (pwrite_entered == 0)
		assert(cnd_wait(&checkpoint_condition, &checkpoint_lock) ==
		    thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	late.page = page;
	late.offset = 60;
	late.value = 0x6c;
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&write_handle, pin_write_thread, &late) ==
	    thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	assert(mtx_lock(&checkpoint_lock) == thrd_success);
	release_pwrite = 1;
	assert(cnd_broadcast(&checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&checkpoint_lock) == thrd_success);
	assert(thrd_join(prepare_handle, NULL) == thrd_success);
	assert(thrd_join(write_handle, NULL) == thrd_success);
	block_pwrite = 0;
	assert(prepare.error == 0 && late.error == 0);
	mutex_lock(&fixture.inode.i_io_lock);
	if (abort_resize) {
		vm_object_resize_abort(&resize);
	} else {
		fake_backing_resize(&fixture, 100);
		vm_object_resize_commit(&resize, 100);
	}
	mutex_unlock(&fixture.inode.i_io_lock);
	vm_object_page_unpin(page);
	if (abort_resize) {
		assert(fixture.object->logical_size == 100);
		page = fixture_fault(&fixture);
		bytes = page->pmem.vaddr;
		assert(bytes[50] == prefix && bytes[60] == late.value);
		vm_object_fault_release(page);
	} else {
		assert(fixture_resize(&fixture, 300, 0) == 0);
		page = fixture_fault(&fixture);
		bytes = page->pmem.vaddr;
		assert(bytes[50] == prefix);
		assert(bytes[60] == 0x41); /* Late orphan write was discarded. */
		assert(bytes[250] == 0);   /* Truncated dirty tail stayed zero. */
		vm_object_fault_release(page);
	}
	fixture_close(&fixture);
}

struct pinned_io_args {
	struct inode *inode;
	struct vm_object_page *page;
	mtx_t lock;
	cnd_t condition;
	int pinned;
	int enter_io;
	int wait_error;
};

static int
pinned_io_thread(void *opaque)
{
	struct pinned_io_args *args = opaque;

	assert(vm_object_page_pin(args->page) == 0);
	assert(mtx_lock(&args->lock) == thrd_success);
	args->pinned = 1;
	assert(cnd_broadcast(&args->condition) == thrd_success);
	while (!args->enter_io)
		assert(cnd_wait(&args->condition, &args->lock) == thrd_success);
	assert(mtx_unlock(&args->lock) == thrd_success);
	args->wait_error = vm_object_inode_io_wait(args->inode);
	vm_object_page_unpin(args->page);
	return 0;
}

static void
test_pin_before_resize_io_has_no_cycle(void)
{
	struct fixture fixture;
	struct vm_object_resize resize;
	struct vm_object_page *page;
	struct pinned_io_args args = { 0 };
	thrd_t holder;
	unsigned before_sleeps;

	fixture_init(&fixture, 100, 0x43);
	page = fixture_fault(&fixture);
	vm_object_fault_release(page);
	args.inode = &fixture.inode;
	args.page = page;
	assert(mtx_init(&args.lock, mtx_plain) == thrd_success);
	assert(cnd_init(&args.condition) == thrd_success);
	assert(thrd_create(&holder, pinned_io_thread, &args) == thrd_success);
	assert(mtx_lock(&args.lock) == thrd_success);
	while (!args.pinned)
		assert(cnd_wait(&args.condition, &args.lock) == thrd_success);
	assert(mtx_unlock(&args.lock) == thrd_success);
	start_resize(&fixture, 300, &resize);
	before_sleeps = vm_test_waitq_sleep_count();
	assert(mtx_lock(&args.lock) == thrd_success);
	args.enter_io = 1;
	assert(cnd_broadcast(&args.condition) == thrd_success);
	assert(mtx_unlock(&args.lock) == thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	/* prepare must orphan the pin, not wait for its holder. */
	assert(vm_object_resize_prepare(&resize) == 0);
	commit_resize(&fixture, &resize, 300);
	assert(thrd_join(holder, NULL) == thrd_success);
	assert(args.wait_error == 0);
	fixture_close(&fixture);
	cnd_destroy(&args.condition);
	mtx_destroy(&args.lock);
}

struct put_args {
	struct vm_object *object;
};

static int
put_thread(void *opaque)
{
	struct put_args *args = opaque;

	vm_object_put(args->object);
	return 0;
}

static void
test_pin_holds_owner_across_final_put(void)
{
	struct fixture fixture;
	struct vm_object_page *page;
	struct put_args args;
	thrd_t thread;
	unsigned before_sleeps;

	fixture_init(&fixture, 100, 0x44);
	page = fixture_fault(&fixture);
	assert(vm_object_page_pin(page) == 0);
	vm_object_fault_release(page);
	args.object = fixture.object;
	before_sleeps = vm_test_waitq_sleep_count();
	assert(thrd_create(&thread, put_thread, &args) == thrd_success);
	vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
	vm_object_page_unpin(page);
	assert(thrd_join(thread, NULL) == thrd_success);
	fixture.object = NULL;
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);
	assert(refcount_load(&fixture.file.f_refs) == 1);
}

int
main(void)
{
	assert(mtx_init(&checkpoint_lock, mtx_plain) == thrd_success);
	assert(cnd_init(&checkpoint_condition) == thrd_success);
	test_fault_waits_for_resize();
	test_shrink_grow_zero();
	test_mmap_tail_does_not_reappear();
	test_failed_resize_aborts();
	test_generic_truncate_and_write_routes();
	test_stacked_truncate_uses_final_limit_domain();
	test_regular_io_uses_shared_cache_domain();
	test_regular_write_short_and_error_prefix();
	test_regular_write_setid_transition_precedes_backend();
	test_stacked_write_uses_final_content_and_setid_domain();
	test_stacked_append_retries_final_eof_after_alias_writer();
	test_sync_and_regular_write_have_no_busy_io_cycle();
	test_exclusive_content_lease_spans_positional_reads();
	test_exec_commit_releases_content_waiter_before_retirement();
	test_late_pin_write_follows_regular_write_commit();
	test_multpage_read_holds_content_lease();
	test_chunked_read_keeps_content_lease_to_end();
	test_stacked_backend_read_excludes_final_alias_writer();
	run_late_pin_write_case(0);
	run_late_pin_write_case(1);
	test_pin_before_resize_io_has_no_cycle();
	test_pin_holds_owner_across_final_put();
	assert(pages_allocated == pages_freed);
	cnd_destroy(&checkpoint_condition);
	mtx_destroy(&checkpoint_lock);
	puts("zedBSD VM object resize host tests: PASS");
	return 0;
}
