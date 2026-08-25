#include "kern/file.h"
#include "kern/cred.h"
#include "kern/kmem.h"
#include "kern/swap.h"
#include "kern/uaccess.h"
#include "kern/vmspace.h"
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
#define FAKE_PTE_SLOTS 64U

struct fake_pte {
	uintptr_t address;
	unsigned generation;
	int present;
};

struct fake_space {
	unsigned maps;
	struct fake_pte ptes[FAKE_PTE_SLOTS];
};
static unsigned spaces_created, spaces_destroyed;
static unsigned pages_allocated, pages_freed, pages_unmapped, pages_protected;
static int fail_space, fail_pages, fail_map, fail_protect;
static unsigned page_ins, swap_reads, swap_slots_freed;
static int fail_swap_read;
static struct swap_backend fake_swap;
static size_t commit_used;
static int fail_commit;
static unsigned pread_calls, pwrite_calls, fsync_calls;
static unsigned setid_clear_calls;
static int setid_clear_error, pwrite_saw_setid;
static int fail_pread_count, fail_pwrite_count, short_pwrite_count;
static int fail_fsync_count;
static int redirty_during_pwrite;
static int redirty_hardware_during_pwrite;
static int hardware_dirty;
static struct vm_object_page *redirty_page;
static mtx_t pread_checkpoint_lock;
static cnd_t pread_checkpoint_condition;
static int block_pread, pread_entered, release_pread;
static int block_pwrite, pwrite_entered, release_pwrite;
static int block_object_revoke, object_revoke_entered, release_object_revoke;
static int block_swap_read, swap_read_entered, release_swap_read;
static unsigned fake_map_generation;
static struct vmspace *unmap_checkpoint_vm;
static uintptr_t unmap_checkpoint_address;
static struct vm_private_page *unmap_checkpoint_private;
static struct vm_object_page *unmap_checkpoint_object;
static int unmap_checkpoint_armed, unmap_checkpoint_seen;
static int unmap_checkpoint_old_absent, unmap_checkpoint_reverse_detached;
static int unmap_checkpoint_map_error, unmap_checkpoint_fault_error;
static unsigned unmap_checkpoint_new_generation;
static mtx_t pin_checkpoint_lock;
static cnd_t pin_checkpoint_condition;
static int pin_checkpoint_mode, pin_checkpoint_seen, pin_peer_started;
static struct vmspace *pin_checkpoint_vm;
static struct vm_private_page *pin_rollback_first;
static struct vm_private_page *pin_rollback_second;
static struct vm_object_page *pin_rollback_object;
static unsigned pin_rollback_object_hold;
static unsigned reap_notify_count;
static int reap_notify_latched;

static void
fake_reaper_notify(void *argument)
{
	assert(argument == &reap_notify_count);
	reap_notify_count++;
	reap_notify_latched = 1;
}

/* uaccess_pin_vmspace() is exercised directly; the current-vm wrapper is not. */
struct thread;
struct thread *thread_current(void) { return NULL; }

static struct fake_pte *
fake_pte_find(struct fake_space *space, uintptr_t address)
{
	unsigned i;

	for (i = 0; i < FAKE_PTE_SLOTS; i++)
		if (space->ptes[i].present &&
		    space->ptes[i].address == address)
			return &space->ptes[i];
	return NULL;
}

static int
fake_pte_present(hal_space_t handle, uintptr_t address)
{
	return fake_pte_find(handle, address) != NULL;
}

unsigned vm_test_waitq_sleep_count(void);
void vm_test_waitq_wait_for_sleep(unsigned);

void *kern_malloc(size_t size) { return malloc(size); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }

int vm_commit_reserve(size_t size)
{
	if (fail_commit)
		return ENOMEM;
	commit_used += size;
	return 0;
}
void vm_commit_release(size_t size)
{
	assert(size <= commit_used);
	commit_used -= size;
}
void hal_fatal(const char *file, int line, const char *message)
{
	fprintf(stderr, "%s:%d: %s\n", file, line, message);
	abort();
}

static void
test_private_advance(struct vm_private_page *backing)
{
	if (++backing->generation == 0)
		backing->generation++;
}

void vm_private_page_init(struct vm_private_page *backing)
{
	refcount_init(&backing->refs, 1);
	spin_init(&backing->state_lock, LOCK_RANK_VM_OBJECT, "test backing");
	waitq_init(&backing->state_waitq, "test backing");
	backing->generation = 1;
	backing->swap_slot = SWAP_SLOT_NONE;
}
void vm_private_page_ref(struct vm_private_page *backing)
{ refcount_get(&backing->refs); }
void vm_private_page_put(struct vm_private_page *backing)
{
	if (!refcount_put(&backing->refs))
		return;
	assert(backing->mapping_count == 0 && backing->mappings == NULL &&
	    backing->active_operations == 0 && backing->pin_count == 0 &&
	    (backing->flags & VM_PAGE_BUSY) == 0);
	if (backing->pmem.size != 0)
		(void)hal_pmem_free(&backing->pmem);
	if (backing->swap_slot != SWAP_SLOT_NONE)
		swap_free_slot(&fake_swap, backing->swap_slot);
	free(backing);
}
int vm_private_page_io_acquire(struct vm_private_page *backing)
{
	for (;;) {
		uint64_t sequence;
		unsigned long irq = spin_lock_irqsave(&backing->state_lock);
		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 && backing->pin_count == 0) {
			backing->flags |= VM_PAGE_BUSY;
			test_private_advance(backing);
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return 0;
		}
		sequence = waitq_sequence(&backing->state_waitq);
		if (waitq_sleep(&backing->state_waitq, &backing->state_lock,
		    sequence, 0, 0) != 0 &&
		    waitq_sequence(&backing->state_waitq) == sequence) {
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return EIO;
		}
		spin_unlock_irqrestore(&backing->state_lock, irq);
	}
}
int vm_private_page_io_try_acquire(struct vm_private_page *backing)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 ||
	    backing->active_operations != 0 || backing->pin_count != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	refcount_get(&backing->refs);
	backing->flags |= VM_PAGE_BUSY;
	test_private_advance(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return 0;
}
void vm_private_page_io_release(struct vm_private_page *backing)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);
	assert((backing->flags & VM_PAGE_BUSY) != 0);
	backing->flags &= ~VM_PAGE_BUSY;
	test_private_advance(backing);
	waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
}
int vm_private_page_wait_idle(struct vm_private_page *backing)
{
	for (;;) {
		uint64_t sequence;
		unsigned long irq = spin_lock_irqsave(&backing->state_lock);
		if ((backing->flags & VM_PAGE_BUSY) == 0 &&
		    backing->active_operations == 0 && backing->pin_count == 0) {
			spin_unlock_irqrestore(&backing->state_lock, irq);
			return 0;
		}
		sequence = waitq_sequence(&backing->state_waitq);
		(void)waitq_sleep(&backing->state_waitq, &backing->state_lock,
		    sequence, 0, 0);
		spin_unlock_irqrestore(&backing->state_lock, irq);
	}
}
int vm_private_page_operation_try_begin(struct vm_private_page *backing)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & VM_PAGE_BUSY) != 0 || backing->pin_count != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	refcount_get(&backing->refs);
	backing->active_operations++;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return 0;
}
void vm_private_page_operation_end(struct vm_private_page *backing)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);
	assert(backing->active_operations != 0);
	backing->active_operations--;
	test_private_advance(backing);
	waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_private_page_put(backing);
}
void vm_private_page_mark_dirty(struct vm_private_page *backing)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);
	backing->flags |= VM_PAGE_DIRTY;
	test_private_advance(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
}
int vm_private_page_pin(struct vm_private_page *backing,
	struct hal_pmem *memory)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);
	if ((backing->flags & (VM_PAGE_BUSY | VM_PAGE_RESIDENT)) !=
	    VM_PAGE_RESIDENT || backing->active_operations != 0) {
		spin_unlock_irqrestore(&backing->state_lock, irq);
		return EBUSY;
	}
	refcount_get(&backing->refs);
	backing->pin_count++;
	*memory = backing->pmem;
	spin_unlock_irqrestore(&backing->state_lock, irq);
	return 0;
}
void vm_private_page_unpin(struct vm_private_page *backing)
{
	unsigned long irq = spin_lock_irqsave(&backing->state_lock);
	assert(backing->pin_count != 0);
	backing->pin_count--;
	waitq_wake_all(&backing->state_waitq);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	vm_private_page_put(backing);
}

void vm_page_track(struct vm_page *page) { (void)page; }
void vm_page_untrack(struct vm_page *page)
{
	struct vm_private_page *backing = page->private_page;
	struct vm_page **link;
	if (backing == NULL)
		return;
	for (link = &backing->mappings; *link != NULL;
	     link = &(*link)->private_next)
		if (*link == page) {
			*link = page->private_next;
			break;
		}
	page->private_page = NULL;
	page->private_next = NULL;
	assert(backing->mapping_count != 0);
	backing->mapping_count--;
	vm_private_page_put(backing);
}
void vm_page_replace_private(struct vm_page *page,
	struct vm_private_page *fresh)
{
	struct vm_private_page *old = page->private_page;
	struct vm_page **link = &old->mappings;
	while (*link != NULL && *link != page)
		link = &(*link)->private_next;
	assert(*link == page);
	*link = page->private_next;
	assert(old->mapping_count != 0 && fresh->mapping_count == 0);
	old->mapping_count--;
	fresh->mapping_count = 1;
	page->private_page = fresh;
	page->private_next = fresh->mappings;
	fresh->mappings = page;
	vm_private_page_put(old);
}
int vm_page_share_private(struct vm_page *source, struct vm_page *copy)
{
	struct vm_private_page *backing = source->private_page;
	int error = vm_private_page_operation_try_begin(backing);
	if (error != 0)
		return error;
	refcount_get(&backing->refs);
	backing->mapping_count++;
	copy->private_page = backing;
	copy->private_next = backing->mappings;
	backing->mappings = copy;
	source->flags |= VM_MAPPING_COW;
	copy->flags |= VM_MAPPING_COW;
	return 0;
}
void vm_page_note_in(struct vm_page *page) { (void)page; page_ins++; }
void vm_reclaim_note_fault(void) { }
int vm_reclaim_one(struct vm_page *page) { (void)page; return ENOMEM; }
struct swap_backend *swap_system_backend(void) { return &fake_swap; }
int swap_read_page(struct swap_backend *b, uint32_t s, void *p)
{
	assert(b == &fake_swap && s == 7);
	if (block_swap_read) {
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		swap_read_entered++;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		while (!release_swap_read)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
	}
	swap_reads++;
	if (fail_swap_read) {
		fail_swap_read = 0;
		return EIO;
	}
	memset(p, 0x6d, SWAP_PAGE_SIZE);
	return 0;
}
void swap_free_slot(struct swap_backend *b, uint32_t s)
{
	assert(b == &fake_swap && s == 7);
	swap_slots_freed++;
}

void file_ref(struct file *file) { refcount_get(&file->f_refs); }
int file_close(struct file *file) { (void)refcount_put(&file->f_refs); return 0; }
int vfs_clear_setid_on_content_change(struct inode *inode)
{
	setid_clear_calls++;
	if (setid_clear_error != 0)
		return setid_clear_error;
	inode->i_mode &= ~(mode_t)(S_ISUID | S_ISGID);
	return 0;
}
struct inode *file_vm_inode(struct file *file)
{
	return file->f_vm_inode != NULL ? file->f_vm_inode : file->f_inode;
}
int file_io_begin(struct file *file, enum file_io_kind kind, off_t offset,
	unsigned internal_flags, struct file_io *io)
{
	if (file == NULL || io == NULL || offset < 0)
		return EINVAL;
	memset(io, 0, sizeof(*io));
	io->file = file;
	io->kind = kind;
	io->offset = offset;
	io->internal_flags = internal_flags;
	return 0;
}
ssize_t file_io_transfer(struct file_io *io, void *buffer, size_t length)
{
	ssize_t result;

	assert(io != NULL && io->file != NULL);
	if (io->kind == FILE_IO_PREAD)
		result = file_pread(io->file, buffer, length, io->offset);
	else if (io->kind == FILE_IO_PWRITE) {
		if ((io->internal_flags & FILE_IO_CONTENT_CHANGE) != 0) {
			int error = vfs_clear_setid_on_content_change(
			    io->file->f_inode);

			if (error != 0)
				return -error;
		}
		result = file_pwrite(io->file, buffer, length, io->offset);
	} else
		return -EINVAL;
	if (result > 0)
		io->offset += result;
	return result;
}
void file_io_end(struct file_io *io)
{
	if (io != NULL)
		memset(io, 0, sizeof(*io));
}
ssize_t file_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	const uint8_t *data = file->f_data;
	pread_calls++;
	if (block_pread) {
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		pread_entered++;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		while (!release_pread)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
	}
	if (fail_pread_count != 0) {
		fail_pread_count--;
		return -EIO;
	}
	memcpy(buffer, data + offset, length);
	return (ssize_t)length;
}
ssize_t file_pwrite(struct file *file, const void *buffer, size_t length,
		    off_t offset)
{
	uint8_t *data = file->f_data;
	pwrite_calls++;
	if (file->f_inode != NULL &&
	    (file->f_inode->i_mode & (S_ISUID | S_ISGID)) != 0)
		pwrite_saw_setid = 1;
	if (block_pwrite) {
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		pwrite_entered++;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		while (!release_pwrite)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
	}
	if (fail_pwrite_count != 0) {
		fail_pwrite_count--;
		return -EIO;
	}
	if (short_pwrite_count != 0) {
		short_pwrite_count--;
		assert(length != 0);
		memcpy(data + offset, buffer, length - 1);
		return (ssize_t)length - 1;
	}
	memcpy(data + offset, buffer, length);
	if (redirty_during_pwrite != 0) {
		redirty_during_pwrite = 0;
		vm_object_mark_dirty(redirty_page);
	}
	return (ssize_t)length;
}
static int
fake_fsync_op(struct file *file)
{
	(void)file;
	fsync_calls++;
	if (fail_fsync_count != 0) {
		fail_fsync_count--;
		return EIO;
	}
	return 0;
}

int
file_fsync(struct file *file)
{
	return fake_fsync_op(file);
}

static ssize_t
fake_pwrite_op(struct file *file, const void *buffer, size_t length,
	       off_t offset)
{
	(void)file;
	(void)buffer;
	(void)offset;
	return (ssize_t)length;
}

static const struct file_ops shared_file_ops = {
	.pwrite = fake_pwrite_op,
	.fsync = fake_fsync_op,
};

struct fault_thread_args {
	struct vmspace *vm;
	uintptr_t address;
	int result;
};

static int
fault_thread(void *opaque)
{
	struct fault_thread_args *args = opaque;
	args->result = vmspace_fault(args->vm, args->address, HAL_SPACE_READ);
	return 0;
}

struct access_fault_thread_args {
	struct vmspace *vm;
	uintptr_t address;
	uint32_t access;
	int result;
};

static int
access_fault_thread(void *opaque)
{
	struct access_fault_thread_args *args = opaque;

	args->result = vmspace_fault(args->vm, args->address, args->access);
	return 0;
}

struct fork_thread_args {
	struct vmspace *source;
	struct vmspace *copy;
	int result;
};

static int
fork_thread(void *opaque)
{
	struct fork_thread_args *args = opaque;

	args->result = vmspace_fork(args->source, &args->copy);
	return 0;
}

struct truncate_thread_args {
	struct inode *inode;
};

static int
truncate_thread(void *opaque)
{
	struct truncate_thread_args *args = opaque;
	vm_object_truncate_inode(args->inode, 0);
	return 0;
}

struct unmap_thread_args {
	struct vmspace *vm;
	uintptr_t address;
	size_t size;
	int result;
};

struct sync_thread_args {
	struct vmspace *vm;
	uintptr_t address;
	size_t size;
	int flags;
	int result;
};

static int
sync_thread(void *opaque)
{
	struct sync_thread_args *args = opaque;

	args->result = vmspace_sync(args->vm, args->address, args->size,
	    args->flags);
	return 0;
}

static int
unmap_thread(void *opaque)
{
	struct unmap_thread_args *args = opaque;

	args->result = vmspace_unmap(args->vm, args->address, args->size);
	return 0;
}

struct shared_map_thread_args {
	struct vmspace *vm;
	struct file *file;
	uintptr_t address;
	size_t size;
	struct vm_region *region;
	int result;
};

static int
shared_map_thread(void *opaque)
{
	struct shared_map_thread_args *args = opaque;

	args->result = vmspace_map_file_shared(args->vm, args->address,
	    args->size, HAL_SPACE_READ | HAL_SPACE_WRITE, args->file, 0,
	    args->size, &args->region);
	return 0;
}

hal_space_t hal_mem_create_space(void)
{
	struct fake_space *space;
	if (fail_space) return NULL;
	space = calloc(1, sizeof(*space));
	if (space != NULL) spaces_created++;
	return space;
}

void hal_page_destroy_space(hal_space_t handle)
{
	spaces_destroyed++;
	free(handle);
}

void hal_page_get_user_range(uintptr_t *minimum, uintptr_t *limit)
{
	if (minimum != NULL)
		*minimum = 4096U;
	if (limit != NULL)
		*limit = 0x80000000U;
}

int hal_pmem_alloc(const struct hal_pmem_request *request,
	struct hal_pmem *memory)
{
	if (fail_pages) return HAL_ERR_NOMEM;
	memory->vaddr = calloc(1, request->size);
	if (memory->vaddr == NULL) return HAL_ERR_NOMEM;
	memory->paddr = (hal_physaddr_t)(uintptr_t)memory->vaddr &
	    ~(hal_physaddr_t)4095U;
	memory->size = request->size;
	memory->type = request->type;
	memory->attr = request->attr;
	pages_allocated++;
	return HAL_OK;
}

int hal_pmem_free(struct hal_pmem *memory)
{
	free(memory->vaddr);
	pages_freed++;
	memset(memory, 0, sizeof(*memory));
	return HAL_OK;
}

int hal_page_map(hal_space_t handle, void *address, hal_physaddr_t paddr,
		 size_t size, uint32_t attr)
{
	struct fake_space *space = handle;
	struct fake_pte *pte = fake_pte_find(space, (uintptr_t)address);
	unsigned i;

	(void)paddr;
	(void)attr;
	assert(size == TEST_PAGE_SIZE);
	if (fail_map) return HAL_ERR_INVALID;
	if (pte == NULL) {
		for (i = 0; i < FAKE_PTE_SLOTS; i++)
			if (!space->ptes[i].present) {
				pte = &space->ptes[i];
				break;
			}
	}
	assert(pte != NULL);
	pte->address = (uintptr_t)address;
	pte->generation = ++fake_map_generation;
	pte->present = 1;
	space->maps++;
	return HAL_OK;
}

int hal_page_unmap(hal_space_t handle, void *address, size_t size)
{
	struct fake_pte *pte = fake_pte_find(handle, (uintptr_t)address);

	assert(size == TEST_PAGE_SIZE);
	if (pte != NULL)
		pte->present = 0;
	hardware_dirty = 0;
	pages_unmapped++;
	return HAL_OK;
}

int hal_page_prot(hal_space_t handle, void *address, size_t size, uint32_t attr)
{
	(void)handle; (void)address; (void)size; (void)attr;
	if (fail_protect) return HAL_ERR_INVALID;
	pages_protected++;
	return HAL_OK;
}

int hal_page_prot_query(hal_space_t handle, void *address, size_t size,
	uint32_t attr, uint32_t *flags)
{
	int error = hal_page_prot(handle, address, size, attr);

	/* Deterministically model a CPU store through its stale writable TLB
	 * between protection publication and the remote acknowledgement. */
	if (error == HAL_OK && redirty_hardware_during_pwrite != 0) {
		redirty_hardware_during_pwrite = 0;
		hardware_dirty = 1;
	}
	if (error == HAL_OK && flags != NULL)
		*flags = hardware_dirty ? HAL_PAGE_DIRTY : 0;
	return error;
}

int hal_page_query(hal_space_t handle, void *address, uint32_t *flags)
{
	(void)handle; (void)address;
	if (flags != NULL)
		*flags = hardware_dirty ? HAL_PAGE_DIRTY : 0;
	return HAL_OK;
}
int hal_page_clear_flags(hal_space_t handle, void *address, uint32_t flags)
{
	(void)handle; (void)address;
	if ((flags & HAL_PAGE_DIRTY) != 0)
		hardware_dirty = 0;
	return HAL_OK;
}

static void
force_shared_backing_swapped(struct vm_page *first, struct vm_page *second)
{
	struct vm_private_page *backing;
	struct hal_pmem resident;
	unsigned long irq;

	assert(first != NULL && second != NULL);
	backing = first->private_page;
	assert(backing != NULL && second->private_page == backing &&
	    backing->mapping_count == 2);
	if ((first->flags & VM_MAPPING_MAPPED) != 0) {
		assert(hal_page_unmap(first->vm->space, (void *)first->address,
		    TEST_PAGE_SIZE) == HAL_OK);
		first->flags &= ~VM_MAPPING_MAPPED;
	}
	if ((second->flags & VM_MAPPING_MAPPED) != 0) {
		assert(hal_page_unmap(second->vm->space, (void *)second->address,
		    TEST_PAGE_SIZE) == HAL_OK);
		second->flags &= ~VM_MAPPING_MAPPED;
	}
	irq = spin_lock_irqsave(&backing->state_lock);
	assert((backing->flags & VM_PAGE_RESIDENT) != 0 &&
	    (backing->flags & (VM_PAGE_BUSY | VM_PAGE_SWAPPED)) == 0);
	resident = backing->pmem;
	memset(&backing->pmem, 0, sizeof(backing->pmem));
	backing->flags &= ~(VM_PAGE_RESIDENT | VM_PAGE_DIRTY);
	backing->flags |= VM_PAGE_SWAPPED;
	backing->swap_slot = 7;
	test_private_advance(backing);
	spin_unlock_irqrestore(&backing->state_lock, irq);
	assert(hal_pmem_free(&resident) == HAL_OK);
}

/* Invoked after region-list publication and before old backing retirement. */
void
vmspace_object_revoke_checkpoint(struct vmspace *vm, uintptr_t address)
{
	(void)vm;
	(void)address;
	if (!block_object_revoke)
		return;
	assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
	object_revoke_entered++;
	assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
	while (!release_object_revoke)
		assert(cnd_wait(&pread_checkpoint_condition,
		    &pread_checkpoint_lock) == thrd_success);
	assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
}

/* Invoked after region-list publication and before old backing retirement. */
void
vmspace_unmap_retire_checkpoint(struct vmspace *vm, uintptr_t start,
	size_t size)
{
	struct fake_pte *pte;

	if (!unmap_checkpoint_armed)
		return;
	assert(vm == unmap_checkpoint_vm);
	assert(unmap_checkpoint_address >= start &&
	    unmap_checkpoint_address < start + size);
	unmap_checkpoint_armed = 0;
	unmap_checkpoint_seen++;
	/* Once munmap is visible, neither its region nor its PTE may remain. */
	assert(vmspace_find_region(vm, unmap_checkpoint_address, 1) == NULL);
	unmap_checkpoint_old_absent =
	    !fake_pte_present(vm->space, unmap_checkpoint_address);
	if (unmap_checkpoint_private != NULL)
		unmap_checkpoint_reverse_detached =
		    unmap_checkpoint_private->mappings == NULL;
	else {
		assert(unmap_checkpoint_object != NULL);
		unmap_checkpoint_reverse_detached =
		    unmap_checkpoint_object->mappings == NULL &&
		    unmap_checkpoint_object->mapping_count == 0;
	}
	unmap_checkpoint_private = NULL;
	unmap_checkpoint_object = NULL;

	/* Reuse and fault the just-published VA before old resources retire. */
	unmap_checkpoint_map_error = vmspace_map_anon_fixed_noreplace(vm,
	    unmap_checkpoint_address, TEST_PAGE_SIZE,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, NULL);
	unmap_checkpoint_fault_error = unmap_checkpoint_map_error == 0 ?
	    vmspace_fault(vm, unmap_checkpoint_address, HAL_SPACE_WRITE) : -1;
	pte = fake_pte_find(vm->space, unmap_checkpoint_address);
	unmap_checkpoint_new_generation = pte != NULL ? pte->generation : 0;
}

/* Invoked with vm_metadata and vm->lock held after each backing pin. */
void
vmspace_pin_page_checkpoint(struct vmspace *vm, size_t index,
	size_t page_count)
{
	int mode;

	assert(mtx_lock(&pin_checkpoint_lock) == thrd_success);
	mode = pin_checkpoint_mode;
	if (mode == 0 || index != 0) {
		assert(mtx_unlock(&pin_checkpoint_lock) == thrd_success);
		return;
	}
	assert(vm == pin_checkpoint_vm && page_count == 2);
	if (mode == 2) {
		assert(pin_rollback_second != NULL);
		assert(vm_private_page_operation_try_begin(pin_rollback_second) == 0);
	} else if (mode == 3) {
		unsigned long irq;

		assert(pin_rollback_object != NULL);
		irq = spin_lock_irqsave(&pin_rollback_object->owner->lock);
		pin_rollback_object_hold = pin_rollback_object->hold_count;
		pin_rollback_object->hold_count = UINT_MAX;
		spin_unlock_irqrestore(&pin_rollback_object->owner->lock, irq);
		pin_checkpoint_mode = 0;
		assert(mtx_unlock(&pin_checkpoint_lock) == thrd_success);
		return;
	}
	pin_checkpoint_seen = 1;
	assert(cnd_broadcast(&pin_checkpoint_condition) == thrd_success);
	while (!pin_peer_started)
		assert(cnd_wait(&pin_checkpoint_condition, &pin_checkpoint_lock) ==
		    thrd_success);
	pin_checkpoint_mode = 0;
	assert(mtx_unlock(&pin_checkpoint_lock) == thrd_success);
}

struct pin_remap_args {
	struct vmspace *vm;
	uintptr_t address;
	size_t size;
	int unmap_error;
	int map_error;
	int fault_error;
};

static int
pin_remap_thread(void *opaque)
{
	struct pin_remap_args *args = opaque;
	uintptr_t page;

	assert(mtx_lock(&pin_checkpoint_lock) == thrd_success);
	while (!pin_checkpoint_seen)
		assert(cnd_wait(&pin_checkpoint_condition, &pin_checkpoint_lock) ==
		    thrd_success);
	pin_peer_started = 1;
	assert(cnd_broadcast(&pin_checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&pin_checkpoint_lock) == thrd_success);
	args->unmap_error = vmspace_unmap(args->vm, args->address, args->size);
	args->map_error = args->unmap_error == 0 ?
	    vmspace_map_anon_fixed_noreplace(args->vm, args->address, args->size,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, NULL) : -1;
	args->fault_error = args->map_error;
	if (args->map_error == 0)
		for (page = args->address; page < args->address + args->size;
		     page += TEST_PAGE_SIZE) {
			args->fault_error = vmspace_fault(args->vm, page,
			    HAL_SPACE_WRITE);
			if (args->fault_error != 0)
				break;
		}
	return 0;
}

struct pin_rollback_args {
	unsigned before_sleeps;
	int rollback_seen;
};

static int
pin_rollback_thread(void *opaque)
{
	struct pin_rollback_args *args = opaque;
	unsigned long irq;

	assert(mtx_lock(&pin_checkpoint_lock) == thrd_success);
	while (!pin_checkpoint_seen)
		assert(cnd_wait(&pin_checkpoint_condition, &pin_checkpoint_lock) ==
		    thrd_success);
	pin_peer_started = 1;
	assert(cnd_broadcast(&pin_checkpoint_condition) == thrd_success);
	assert(mtx_unlock(&pin_checkpoint_lock) == thrd_success);
	vm_test_waitq_wait_for_sleep(args->before_sleeps + 1U);
	irq = spin_lock_irqsave(&pin_rollback_first->state_lock);
	args->rollback_seen = pin_rollback_first->pin_count == 0;
	spin_unlock_irqrestore(&pin_rollback_first->state_lock, irq);
	vm_private_page_operation_end(pin_rollback_second);
	return 0;
}

int main(void)
{
	static const uint8_t file_data[] = "xxELF!tail";
	struct vmspace *vm;
	struct vm_region *region;
	struct file file = { .f_data = (void *)file_data };
	struct vm_page *page;
	uintptr_t mapped;
	char buffer[8];
	static uint8_t shared_data[4096];
	struct inode shared_inode = {
		.i_type = INODE_REG, .i_size = sizeof(shared_data)
	};
	struct file shared_file = {
		.f_data = shared_data, .f_inode = &shared_inode,
		.f_ops = &shared_file_ops, .f_flags = { O_RDWR }
	};

	assert(mtx_init(&pread_checkpoint_lock, mtx_plain) == thrd_success);
	assert(cnd_init(&pread_checkpoint_condition) == thrd_success);
	assert(mtx_init(&pin_checkpoint_lock, mtx_plain) == thrd_success);
	assert(cnd_init(&pin_checkpoint_condition) == thrd_success);
	refcount_init(&file.f_refs,1);
	refcount_init(&shared_file.f_refs,1);
	vm = vmspace_create();
	assert(vm != NULL && spaces_created == 1);
	assert(vmspace_map_anon(vm, 0x400000, 8192,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &region) == 0);
	assert(commit_used == 8192);
	assert(region->start == 0x400000 && region->size == 8192);
	assert(region->pages == NULL && pages_allocated == 0);
	assert(vmspace_fault(vm, 0x400123, HAL_SPACE_READ) == 0);
	/* One persistent physical page backs the vm_page metadata slab. */
	assert(pages_allocated == 2 && region->pages != NULL);
	assert(vmspace_copy_to(vm, 0x400ffe, "abcd", 4) == 0);
	assert(pages_allocated == 3);
	memset(buffer, 0, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x400ffe, 4) == 0);
	assert(!memcmp(buffer, "abcd", 4));
	assert(vmspace_wire_range(vm, 0x400ffe, 4, HAL_SPACE_READ) == 0);
	for (struct vm_page *page = region->pages; page != NULL;
	     page = page->next)
		assert(page->wire_count == 1);
	vmspace_unwire_range(vm, 0x400ffe, 4);
	for (struct vm_page *page = region->pages; page != NULL;
	     page = page->next)
		assert(page->wire_count == 0);
	/* A swapped page has no PTE to protect; its region policy still changes. */
	region->pages->flags &= ~VM_MAPPING_MAPPED;
	region->pages->private_page->flags &= ~VM_PAGE_RESIDENT;
	region->pages->private_page->flags |= VM_PAGE_SWAPPED;
	assert(vmspace_protect(vm, 0x400000, 8192, HAL_SPACE_READ) == 0);
	assert(pages_protected == 1);
	region->pages->private_page->flags &= ~VM_PAGE_SWAPPED;
	region->pages->private_page->flags |= VM_PAGE_RESIDENT;
	region->pages->flags |= VM_MAPPING_MAPPED;
	fail_protect = 1;
	assert(vmspace_protect(vm, 0x400000, 8192,
		HAL_SPACE_READ | HAL_SPACE_WRITE) == EINVAL);
	fail_protect = 0;
	assert(vmspace_copy_to(vm, 0x400000, "x", 1) == EFAULT);
	assert(vmspace_unmap(vm, 0x400000, 8192) == 0);
	assert(pages_unmapped == 2 && pages_freed == 2);
	assert(commit_used == 0);

	/*
	 * The unmap publication checkpoint runs after the VA becomes reusable but
	 * before old backing retirement.  Reusing and faulting the same address in
	 * that window must not let old cleanup erase the new PTE.
	 */
	assert(vmspace_map_anon(vm, 0x400000, 3U * TEST_PAGE_SIZE,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, &region) == 0);
	assert(vmspace_fault(vm, 0x400000, HAL_SPACE_WRITE) == 0);
	assert(vmspace_fault(vm, 0x401000, HAL_SPACE_WRITE) == 0);
	assert(vmspace_fault(vm, 0x402000, HAL_SPACE_WRITE) == 0);
	assert(fake_pte_present(vm->space, 0x400000) &&
	    fake_pte_present(vm->space, 0x401000) &&
	    fake_pte_present(vm->space, 0x402000));
	unmap_checkpoint_vm = vm;
	unmap_checkpoint_address = 0x401000;
	for (page = region->pages; page != NULL && page->address != 0x401000;
	     page = page->next)
		;
	assert(page != NULL);
	unmap_checkpoint_private = page->private_page;
	unmap_checkpoint_object = NULL;
	unmap_checkpoint_seen = 0;
	unmap_checkpoint_old_absent = 0;
	unmap_checkpoint_reverse_detached = 0;
	unmap_checkpoint_map_error = unmap_checkpoint_fault_error = -1;
	unmap_checkpoint_new_generation = 0;
	unmap_checkpoint_armed = 1;
	assert(vmspace_unmap(vm, 0x401000, TEST_PAGE_SIZE) == 0);
	assert(!unmap_checkpoint_armed && unmap_checkpoint_seen == 1);
	assert(unmap_checkpoint_old_absent &&
	    unmap_checkpoint_reverse_detached);
	assert(unmap_checkpoint_map_error == 0 &&
	    unmap_checkpoint_fault_error == 0);
	{
		struct fake_pte *pte = fake_pte_find(vm->space, 0x401000);
		assert(pte != NULL && pte->generation ==
		    unmap_checkpoint_new_generation);
	}
	assert(fake_pte_present(vm->space, 0x400000) &&
	    fake_pte_present(vm->space, 0x402000));
	assert(vmspace_unmap(vm, 0x400000, 3U * TEST_PAGE_SIZE) == 0);
	assert(commit_used == 0);

	/*
	 * MAP_FIXED prepares its replacement before removing the old pages.  A
	 * middle-page replacement preserves both fragments and failure to reserve
	 * the new mapping leaves the original region and contents untouched.
	 */
	assert(vmspace_map_anon(vm, 0x00400000, 3U * TEST_PAGE_SIZE,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, &region) == 0);
	assert(vmspace_copy_to(vm, 0x00400000, "L", 1) == 0);
	assert(vmspace_copy_to(vm, 0x00401000, "M", 1) == 0);
	assert(vmspace_copy_to(vm, 0x00402000, "R", 1) == 0);
	fail_commit = 1;
	assert(vmspace_map_anon_fixed(vm, 0x00401000, TEST_PAGE_SIZE,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, 0, NULL) == ENOMEM);
	fail_commit = 0;
	memset(buffer, 0, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x00401000, 1) == 0 &&
	    buffer[0] == 'M');
	assert(vmspace_find_region(vm, 0x00400000, 3U * TEST_PAGE_SIZE) ==
	    region);
	assert(vmspace_map_anon_fixed(vm, 0x00401000, TEST_PAGE_SIZE,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, 0, NULL) == 0);
	memset(buffer, 0xff, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x00400000, 1) == 0 &&
	    buffer[0] == 'L');
	assert(vmspace_copy_from(vm, buffer, 0x00401000, 1) == 0 &&
	    buffer[0] == 0);
	assert(vmspace_copy_from(vm, buffer, 0x00402000, 1) == 0 &&
	    buffer[0] == 'R');
	assert(vmspace_unmap(vm, 0x00400000, 3U * TEST_PAGE_SIZE) == 0);
	assert(commit_used == 0);

	/* Object-backed reverse mappings obey the same pre-publication rule. */
	assert(vmspace_map_file_shared(vm, 0x00a00000, TEST_PAGE_SIZE,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
	    sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	unmap_checkpoint_vm = vm;
	unmap_checkpoint_address = 0x00a00000;
	unmap_checkpoint_private = NULL;
	unmap_checkpoint_object = region->pages->object_page;
	unmap_checkpoint_seen = 0;
	unmap_checkpoint_old_absent = 0;
	unmap_checkpoint_reverse_detached = 0;
	unmap_checkpoint_map_error = unmap_checkpoint_fault_error = -1;
	unmap_checkpoint_new_generation = 0;
	unmap_checkpoint_armed = 1;
	assert(vmspace_unmap(vm, 0x00a00000, TEST_PAGE_SIZE) == 0);
	assert(!unmap_checkpoint_armed && unmap_checkpoint_seen == 1);
	assert(unmap_checkpoint_old_absent &&
	    unmap_checkpoint_reverse_detached);
	assert(unmap_checkpoint_map_error == 0 &&
	    unmap_checkpoint_fault_error == 0);
	{
		struct fake_pte *pte = fake_pte_find(vm->space, 0x00a00000);
		assert(pte != NULL && pte->generation ==
		    unmap_checkpoint_new_generation);
	}
	assert(vmspace_unmap(vm, 0x00a00000, TEST_PAGE_SIZE) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);
	assert(commit_used == 0);

	assert(vmspace_map_anon(vm, 0x800000, 4096, 0, &region) == 0);
	assert(commit_used == 0 && region->commit_size == 0);
	assert(vmspace_protect(vm, 0x800000, 4096, HAL_SPACE_READ) == 0);
	assert(commit_used == 4096 && region->commit_size == 4096);
	assert(vmspace_fault(vm, 0x800000, HAL_SPACE_READ) == 0);
	assert(fake_pte_present(vm->space, 0x800000));
	assert(vmspace_protect(vm, 0x800000, 4096, 0) == 0);
	assert(!fake_pte_present(vm->space, 0x800000));
	assert(vmspace_fault(vm, 0x800000, HAL_SPACE_READ) == EFAULT);
	assert(vmspace_protect(vm, 0x800000, 4096, HAL_SPACE_READ) == 0);
	assert(fake_pte_present(vm->space, 0x800000));
	assert(commit_used == 4096);
	assert(vmspace_unmap(vm, 0x800000, 4096) == 0);
	assert(commit_used == 0);

	/* mprotect may change permissions only within the rights captured when
	 * the mapping was created.  Reject a write upgrade derived from an
	 * O_RDONLY MAP_SHARED descriptor before splitting or changing any region
	 * in the requested range. */
	atomic_store_release(&shared_file.f_flags, O_RDONLY);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
	    sizeof(shared_data), NULL) == EACCES);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
	    HAL_SPACE_READ, &shared_file, 0, sizeof(shared_data), &region) == 0);
	assert((region->max_prot & HAL_SPACE_WRITE) == 0);
	assert(vmspace_map_anon_fixed_noreplace(vm, 0x00a01000, 4096,
	    HAL_SPACE_READ, NULL) == 0);
	assert(vmspace_protect(vm, 0x00a00000, 8192,
	    HAL_SPACE_READ | HAL_SPACE_WRITE) == EACCES);
	assert(region->start == 0x00a00000 && region->size == 4096 &&
	    region->prot == HAL_SPACE_READ && region->next != NULL &&
	    region->next->start == 0x00a01000 &&
	    region->next->prot == HAL_SPACE_READ);
	assert(vmspace_unmap(vm, 0x00a00000, 8192) == 0);
	atomic_store_release(&shared_file.f_flags, O_RDWR);

	fail_commit = 1;
	assert(vmspace_map_anon(vm, 0x810000, 4096, HAL_SPACE_READ, NULL) ==
		ENOMEM);
	assert(vmspace_find_region(vm, 0x810000, 1) == NULL);
	fail_commit = 0;

	fail_pages = 1;
	assert(vmspace_map_anon(vm, 0x500000, 4096, HAL_SPACE_READ, NULL) == 0);
	assert(vmspace_fault(vm, 0x500000, HAL_SPACE_READ) == ENOMEM);
	fail_pages = 0;
	fail_map = 1;
	assert(vmspace_fault(vm, 0x500000, HAL_SPACE_READ) == ENOMEM);
	fail_map = 0;

	assert(vmspace_map_file(vm, 0x600000, 4096, HAL_SPACE_READ, &file,
		2, 0x600003, 5, &region) == 0);
	assert(commit_used == 4096); /* 0x500000 anonymous mapping */
	assert(refcount_load(&file.f_refs) == 2 && region->pages == NULL);
	assert(vmspace_fault(vm, 0x600003, HAL_SPACE_READ) == 0);
	memset(buffer, 0xaa, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x600000, 8) == 0);
	assert(buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0);
	assert(!memcmp(buffer + 3, "ELF!t", 5));
	/* GNU_RELRO may end inside the final page-rounded ELF mapping.  Splitting
	 * must preserve an unaligned ELF data subrange instead of rejecting it. */
	assert(vmspace_map_file(vm, 0x700000, 8192,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &file, 2, 0x700003, 5,
		&region) == 0);
	region->flags |= VM_REGION_ELF_ZERO_TAIL;
	assert(vmspace_protect(vm, 0x700000, 4096, HAL_SPACE_READ) == 0);
	assert(region->start == 0x700000 && region->size == 4096 &&
		region->data_start == 0x700003 && region->data_size == 5);
	assert(region->next != NULL && region->next->start == 0x701000 &&
		region->next->size == 4096 && region->next->data_size == 0);
	assert(vmspace_unmap(vm, 0x700000, 8192) == 0);
	assert(vmspace_map_file(vm, 0x900000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &file, 2, 0x900003, 5,
		&region) == 0);
	assert(region->commit_size == 4096 && commit_used == 8192);

	/* A failed PTE publication releases its transient object-page hold. */
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	{
		unsigned before_reads = pread_calls;
		fail_map = 1;
		assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == ENOMEM);
		fail_map = 0;
		assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
		assert(pread_calls == before_reads + 1);
	}
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);

	/* A failed object read is published as ERROR and retried without UAF. */
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	{
		unsigned before_reads = pread_calls;
		fail_pread_count = 1;
		assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == EIO);
		assert(vm_object_page_count() == 1 && region->pages == NULL);
		assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
		assert(pread_calls == before_reads + 2U);
	}
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);

	/* Two CPUs faulting one object offset issue exactly one backend read. */
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), NULL) == 0);
	assert(vmspace_map_file_shared(vm, 0x00a01000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), NULL) == 0);
	{
		struct fault_thread_args first = { vm, 0x00a00000, -1 };
		struct fault_thread_args second = { vm, 0x00a01000, -1 };
		thrd_t first_thread, second_thread;
		unsigned before_reads = pread_calls;
		unsigned before_sleeps = vm_test_waitq_sleep_count();

		block_pread = 1;
		pread_entered = release_pread = 0;
		assert(thrd_create(&first_thread, fault_thread, &first) ==
		    thrd_success);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		while (pread_entered == 0)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_create(&second_thread, fault_thread, &second) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		release_pread = 1;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_join(first_thread, NULL) == thrd_success);
		assert(thrd_join(second_thread, NULL) == thrd_success);
		block_pread = 0;
		assert(first.result == 0 && second.result == 0);
		assert(pread_calls == before_reads + 1U);
	}
	assert(vmspace_unmap(vm, 0x00a00000, 8192) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);

	/* Truncate waits for fault I/O and for the fault-to-map transient hold. */
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	{
		struct fault_thread_args fault = { vm, 0x00a00000, -1 };
		struct truncate_thread_args truncate = { &shared_inode };
		thrd_t fault_handle, truncate_handle;
		unsigned before_sleeps = vm_test_waitq_sleep_count();

		block_pread = 1;
		pread_entered = release_pread = 0;
		assert(thrd_create(&fault_handle, fault_thread, &fault) ==
		    thrd_success);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		while (pread_entered == 0)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		shared_inode.i_size = 0;
		assert(thrd_create(&truncate_handle, truncate_thread, &truncate) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		release_pread = 1;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_join(fault_handle, NULL) == thrd_success);
		assert(thrd_join(truncate_handle, NULL) == thrd_success);
		block_pread = 0;
		assert(fault.result == 0);
		assert(region->pages == NULL && vm_object_page_count() == 0);
		shared_inode.i_size = sizeof(shared_data);
	}
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);

	/* MAP_SHARED mappings of one inode use one physical object page. */
	memcpy(shared_data, "shared", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_map_file_shared(vm, 0x00a01000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), NULL) == 0);
	assert(vm_object_count() == 1 && vm_object_page_count() == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_fault(vm, 0x00a01000, HAL_SPACE_READ) == 0);
	assert(vm_object_page_count() == 1);
	assert(vmspace_copy_to(vm, 0x00a00000, "object", 6) == 0);
	memset(buffer, 0, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x00a01000, 6) == 0);
	assert(!memcmp(buffer, "object", 6));
	assert(!memcmp(shared_data, "shared", 6));
	shared_inode.i_mode = S_IFREG | S_ISUID | S_ISGID | 0755U;
	setid_clear_error = EACCES;
	{
		unsigned before_writes = pwrite_calls;

		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == EACCES);
		assert(pwrite_calls == before_writes &&
		    (shared_inode.i_mode & (S_ISUID | S_ISGID)) != 0);
		setid_clear_error = 0;
		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
		assert(pwrite_calls == before_writes + 1U);
	}
	assert(setid_clear_calls >= 2U && !pwrite_saw_setid &&
	    (shared_inode.i_mode & (S_ISUID | S_ISGID)) == 0 &&
	    !memcmp(shared_data, "object", 6));
	assert(vm_object_reclaim_one() == 0 && vm_object_page_count() == 0);
	memset(buffer, 0, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x00a01000, 6) == 0);
	assert(!memcmp(buffer, "object", 6) && vm_object_page_count() == 1);
	assert(vmspace_unmap(vm, 0x00a00000, 8192) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);
	assert(vm_object_retained_count() == 0 && refcount_load(&shared_file.f_refs) == 1);

	/* Reverse mapping metadata remains pinned while the acknowledged PTE
	 * revoke runs without VM locks; a concurrent unmap must wait for BUSY. */
	memcpy(shared_data, "window", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "revoke", 6) == 0);
	{
		struct sync_thread_args sync = {
			vm, 0x00a00000, 4096, MS_SYNC, -1
		};
		struct unmap_thread_args unmap = {
			vm, 0x00a00000, 4096, -1
		};
		thrd_t sync_handle, unmap_handle;
		unsigned before_sleeps;

		block_object_revoke = 1;
		object_revoke_entered = release_object_revoke = 0;
		assert(thrd_create(&sync_handle, sync_thread, &sync) ==
		    thrd_success);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		while (object_revoke_entered == 0)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(region->pages != NULL &&
		    (region->pages->flags & VM_MAPPING_BUSY) != 0);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		before_sleeps = vm_test_waitq_sleep_count();
		assert(thrd_create(&unmap_handle, unmap_thread, &unmap) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		release_object_revoke = 1;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_join(sync_handle, NULL) == thrd_success);
		assert(thrd_join(unmap_handle, NULL) == thrd_success);
		block_object_revoke = 0;
		assert(sync.result == 0 && unmap.result == 0);
	}
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);

	/* A page write failure is reported, remains dirty, and is retried. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "retry1", 6) == 0);
	fail_pwrite_count = 1;
	assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == EIO);
	assert(!memcmp(shared_data, "before", 6));
	assert(vm_object_count() == 1 && vm_object_page_count() == 1);
	assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
	assert(!memcmp(shared_data, "retry1", 6));
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);

	/* A short write is EIO and the next attempt rewrites the whole page. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "short1", 6) == 0);
	short_pwrite_count = 1;
	assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == EIO);
	assert(vm_object_page_count() == 1);
	assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
	assert(!memcmp(shared_data, "short1", 6));
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);

	/* A backend flush failure keeps the page dirty for a full rewrite. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "retry2", 6) == 0);
	{
		unsigned before_writes = pwrite_calls;
		fail_fsync_count = 1;
		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == EIO);
		assert(pwrite_calls == before_writes + 1);
		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
		assert(pwrite_calls == before_writes + 2);
	}
	assert(!memcmp(shared_data, "retry2", 6));
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);

	/* A write racing successful writeback advances the dirty generation. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "first1", 6) == 0);
	redirty_page = region->pages->object_page;
	redirty_during_pwrite = 1;
	{
		unsigned before_writes = pwrite_calls;
		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
		assert(pwrite_calls == before_writes + 1);
		/* The first success must not clear the racing dirty generation. */
		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
		assert(pwrite_calls == before_writes + 2);
	}
	redirty_page = NULL;
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);

	/* A stale-TLB store during write revoke is included in the acknowledged
	 * dirty snapshot and therefore in this writeback. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "first2", 6) == 0);
	redirty_hardware_during_pwrite = 1;
	{
		unsigned before_writes = pwrite_calls;
		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
		assert(pwrite_calls == before_writes + 1);
		assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
		assert(pwrite_calls == before_writes + 1);
	}
	assert(!hardware_dirty);
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);

	/*
	 * A lookup racing the last mapping's writeback waits for DETACHING to
	 * finish, then obtains a fresh object instead of reviving freed storage.
	 */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "detach", 6) == 0);
	{
		struct unmap_thread_args unmap = {
			vm, 0x00a00000, 4096, -1
		};
		struct shared_map_thread_args remap = {
			vm, &shared_file, 0x00a00000, 4096, NULL, -1
		};
		thrd_t unmap_handle, remap_handle;
		unsigned before_sleeps = vm_test_waitq_sleep_count();

		block_pwrite = 1;
		pwrite_entered = release_pwrite = 0;
		assert(thrd_create(&unmap_handle, unmap_thread, &unmap) ==
		    thrd_success);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		while (pwrite_entered == 0)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);

		assert(thrd_create(&remap_handle, shared_map_thread, &remap) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);

		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		release_pwrite = 1;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_join(unmap_handle, NULL) == thrd_success);
		assert(thrd_join(remap_handle, NULL) == thrd_success);
		block_pwrite = 0;
		assert(unmap.result == 0 && remap.result == 0 &&
		    remap.region != NULL);
	}
	assert(vm_object_count() == 1 && vm_object_page_count() == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	memset(buffer, 0, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x00a00000, 6) == 0);
	assert(!memcmp(buffer, "detach", 6));
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);
	assert(refcount_load(&shared_file.f_refs) == 1);

	/* Final put retains data and file references until an inode retry. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "retain", 6) == 0);
	fail_pwrite_count = 1;
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);
	assert(vm_object_count() == 1 && vm_object_page_count() == 1);
	assert(vm_object_retained_count() == 1);
	assert(refcount_load(&shared_file.f_refs) == 3);
	assert(vm_object_sync_inode(&shared_inode) == 0);
	assert(!memcmp(shared_data, "retain", 6));
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);
	assert(vm_object_retained_count() == 0 && refcount_load(&shared_file.f_refs) == 1);

	/* A new mapping revives the retained object and sees its latest page. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "revive", 6) == 0);
	fail_pwrite_count = 1;
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);
	assert(vm_object_retained_count() == 1);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vm_object_count() == 1 && vm_object_retained_count() == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	memset(buffer, 0, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x00a00000, 6) == 0);
	assert(!memcmp(buffer, "revive", 6));
	assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);

	/* MS_INVALIDATE never drops a page whose writeback failed. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "invald", 6) == 0);
	fail_pwrite_count = 1;
	assert(vmspace_sync(vm, 0x00a00000, 4096,
		MS_SYNC | MS_INVALIDATE) == EIO);
	/* The failed dirty cache page is retained, but its revoked PTE is not
	 * resurrected implicitly.  A subsequent access refaults the same cache
	 * identity before retrying writeback. */
	assert(region->pages == NULL && vm_object_page_count() == 1);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(region->pages != NULL && vm_object_page_count() == 1);
	assert(vmspace_sync(vm, 0x00a00000, 4096,
		MS_SYNC | MS_INVALIDATE) == 0);
	assert(region->pages == NULL && vm_object_page_count() == 0);
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);

	/* Reclaim skips a failed page and frees it only after a successful retry. */
	memcpy(shared_data, "before", 7);
	assert(vmspace_map_file_shared(vm, 0x00a00000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
		sizeof(shared_data), &region) == 0);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vmspace_copy_to(vm, 0x00a00000, "reclm1", 6) == 0);
	fail_pwrite_count = 1;
	assert(vm_object_reclaim_one() == ENOMEM);
	assert(region->pages == NULL && vm_object_page_count() == 1);
	assert(vmspace_fault(vm, 0x00a00000, HAL_SPACE_READ) == 0);
	assert(vm_object_reclaim_one() == 0);
	assert(region->pages == NULL && vm_object_page_count() == 0);
	assert(vmspace_unmap(vm, 0x00a00000, 4096) == 0);

	/* vmspace teardown may retain an object; a later inode sync reaps it. */
	{
		struct vmspace *teardown_vm = vmspace_create();
		assert(teardown_vm != NULL);
		memcpy(shared_data, "before", 7);
		assert(vmspace_map_file_shared(teardown_vm, 0x00a00000, 4096,
			HAL_SPACE_READ | HAL_SPACE_WRITE, &shared_file, 0,
			sizeof(shared_data), &region) == 0);
		assert(vmspace_fault(teardown_vm, 0x00a00000, HAL_SPACE_READ) == 0);
		assert(vmspace_copy_to(teardown_vm, 0x00a00000, "exitwb", 6) == 0);
		fail_fsync_count = 1;
		vmspace_put(teardown_vm);
		assert(vm_object_retained_count() == 1);
		assert(vm_object_sync_inode(&shared_inode) == 0);
		assert(!memcmp(shared_data, "exitwb", 6));
		assert(vm_object_count() == 0 && vm_object_page_count() == 0);
	}
	assert(refcount_load(&shared_file.f_refs) == 1);

	assert(vmspace_set_brk_start(vm, 0x01000000U, 1024) == 0);
	assert(vmspace_brk(vm, 0, &mapped) == 0 && mapped == 0x01000000U);
	assert(vmspace_set_data_limit(vm, 4096) == 0);
	assert(vmspace_brk(vm, 0x01000c01U, &mapped) == ENOMEM);
	assert(vmspace_brk(vm, 0x01000c00U, &mapped) == 0);
	assert(vmspace_brk(vm, 0x01000000U, &mapped) == 0);
	assert(vmspace_brk(vm, 0x01001001U, &mapped) == ENOMEM);
	assert(vmspace_set_data_limit(vm, vmspace_address_cap()) == 0);
	assert(vmspace_brk(vm, 0x01001001U, &mapped) == 0 &&
		mapped == 0x01001001U);
	region = vmspace_find_region(vm, 0x01000000U, 1);
	assert(region != NULL && region->size == 8192 &&
		(region->flags & VM_REGION_BRK) != 0);
	assert(commit_used == 16384);
	fail_commit = 1;
	assert(vmspace_brk(vm, 0x01002001U, &mapped) == ENOMEM);
	assert(vm->brk_current == 0x01001001U && region->size == 8192 &&
		commit_used == 16384);
	fail_commit = 0;
	assert(vmspace_fault(vm, 0x01001000U, HAL_SPACE_WRITE) == 0);
	assert(vmspace_brk(vm, 0x01000001U, &mapped) == 0);
	assert(region->size == 4096 && commit_used == 12288);
	assert(vmspace_brk(vm, 0x01000000U, &mapped) == 0);
	assert(vmspace_find_region(vm, 0x01000000U, 1) == NULL);
	assert(commit_used == 8192);
	assert(vmspace_brk(vm, vm_layout.brk_limit, &mapped) == ENOMEM);

	assert(vmspace_map_stack(vm, 0x7ffff000U, 1024U * 1024U, 4096) == 0);
	assert(vm->stack_guard_bottom == 0x7fefe000U);
	assert(vm->stack_bottom == 0x7feff000U);
	assert(vm->stack_top == 0x7ffff000U);
	region = vmspace_find_region(vm, vm->stack_guard_bottom, 1);
	assert(region != NULL && region->prot == 0 &&
		(region->flags & (VM_REGION_GUARD | VM_REGION_IMMUTABLE)) ==
		(VM_REGION_GUARD | VM_REGION_IMMUTABLE));
	assert(vmspace_fault(vm, vm->stack_guard_bottom, HAL_SPACE_WRITE) ==
		EFAULT);
	assert(vmspace_unmap(vm, vm->stack_guard_bottom, 4096) == EACCES);
	assert(vmspace_protect(vm, vm->stack_bottom, 1024U * 1024U,
		HAL_SPACE_READ) == EACCES);
	assert(vmspace_map_find(vm, 0, vm->stack_guard_bottom - 0x10000000U +
		4096U, HAL_SPACE_READ, &mapped) == ENOMEM);

	/* A page-in whose slot is released must remain reclaimable as dirty. */
	assert(vmspace_map_anon(vm, 0x700000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &region) == 0);
	page = vm_page_alloc_metadata();
	assert(page != NULL);
	page->address = region->start;
	page->vm = vm;
	page->region = region;
	page->private_page = calloc(1, sizeof(*page->private_page));
	assert(page->private_page != NULL);
	vm_private_page_init(page->private_page);
	page->private_page->mapping_count = 1;
	page->private_page->mappings = page;
	page->private_page->flags = VM_PAGE_SWAPPED;
	page->private_page->swap_slot = 7;
	region->pages = page;
	{
		unsigned before_allocated = pages_allocated;
		unsigned before_freed = pages_freed;
		unsigned before_reads = swap_reads;
		unsigned before_slot_frees = swap_slots_freed;

		fail_swap_read = 1;
		assert(vmspace_fault(vm, region->start, HAL_SPACE_READ) == EIO);
		assert((page->private_page->flags & VM_PAGE_SWAPPED) != 0 &&
		    (page->private_page->flags &
		    (VM_PAGE_RESIDENT | VM_PAGE_BUSY)) == 0);
		assert(page->private_page->swap_slot == 7 &&
		    page->private_page->pmem.size == 0);
		assert((page->flags & VM_MAPPING_BUSY) == 0 &&
		    region->hold_count == 0);
		assert(pages_allocated == before_allocated + 1U &&
		    pages_freed == before_freed + 1U &&
		    swap_reads == before_reads + 1U &&
		    swap_slots_freed == before_slot_frees);
	}
	assert(vmspace_fault(vm, region->start, HAL_SPACE_READ) == 0);
	assert((page->private_page->flags & VM_PAGE_RESIDENT) != 0);
	assert((page->private_page->flags & VM_PAGE_DIRTY) != 0);
	assert((page->private_page->flags & VM_PAGE_SWAPPED) == 0);
	assert(page->private_page->swap_slot == SWAP_SLOT_NONE);
	assert(((uint8_t *)page->private_page->pmem.vaddr)[123] == 0x6d);
	assert(page_ins == 1 && swap_reads == 2 && swap_slots_freed == 1);

	vmspace_put(vm);
	assert(refcount_load(&file.f_refs) == 1);
	assert(spaces_created == spaces_destroyed);
	assert(pages_allocated == pages_freed + 1);
	assert(commit_used == 0);

	/* fork shares private storage read-only and separates it on first write. */
	{
		struct vmspace *parent = vmspace_create();
		struct vmspace *child = NULL;
		struct vm_region *parent_region;
		struct vm_page *parent_page, *child_page;
		char parent_value = 0, child_value = 0;
		assert(parent != NULL);
		assert(vmspace_map_anon(parent, 0x600000, 4096,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &parent_region) == 0);
		assert(vmspace_copy_to(parent, 0x600000, "P", 1) == 0);
		parent_page = parent_region->pages;
		assert(vmspace_fork(parent, &child) == 0 && child != NULL);
		child_page = vmspace_find_region(child, 0x600000, 1)->pages;
		assert(child_page->private_page == parent_page->private_page);
		assert(refcount_load(&parent_page->private_page->refs) == 2);
		assert((parent_page->flags & VM_MAPPING_COW) != 0);
		assert((child_page->flags & VM_MAPPING_COW) != 0);
		assert(vmspace_copy_to(child, 0x600000, "C", 1) == 0);
		assert(child_page->private_page != parent_page->private_page);
		assert(vmspace_copy_from(parent, &parent_value, 0x600000, 1) == 0);
		assert(vmspace_copy_from(child, &child_value, 0x600000, 1) == 0);
		assert(parent_value == 'P' && child_value == 'C');
		vmspace_put(child);
		vmspace_put(parent);
		assert(commit_used == 0);
	}

	/* MAP_SHARED|MAP_ANONYMOUS retains one object across fork. */
	{
		struct vmspace *parent = vmspace_create();
		struct vmspace *child = NULL;
		uintptr_t shared_address;
		char parent_value = 0, child_value = 0;

		assert(parent != NULL);
		assert(vmspace_map_anon_shared_find(parent, 0x00600000,
		    TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE,
		    &shared_address) == 0);
		assert(shared_address >= vm_layout.mmap_base &&
		    commit_used == TEST_PAGE_SIZE);
		assert(vmspace_copy_to(parent, shared_address, "P", 1) == 0);
		assert(vmspace_fork(parent, &child) == 0 && child != NULL);
		assert(commit_used == TEST_PAGE_SIZE);
		assert(vmspace_copy_from(child, &child_value, shared_address, 1) == 0 &&
		    child_value == 'P');
		assert(vmspace_copy_to(child, shared_address, "C", 1) == 0);
		assert(vmspace_copy_from(parent, &parent_value, shared_address, 1) == 0 &&
		    parent_value == 'C');
		vmspace_put(child);
		assert(commit_used == TEST_PAGE_SIZE);
		vmspace_put(parent);
		assert(commit_used == 0);
	}

	/*
	 * Two vmspaces faulting one swapped private backing have one page-in
	 * owner.  The waiter may publish its own PTE only after that owner has
	 * committed the resident frame and released the swap slot.
	 */
	{
		struct vmspace *parent = vmspace_create();
		struct vmspace *child = NULL;
		struct vm_region *parent_region;
		struct vm_page *parent_page, *child_page;
		struct fault_thread_args first, second;
		thrd_t first_handle, second_handle;
		unsigned before_ins = page_ins;
		unsigned before_reads = swap_reads;
		unsigned before_frees = swap_slots_freed;
		unsigned before_sleeps;
		char parent_value = 0, child_value = 0;

		assert(parent != NULL);
		assert(vmspace_map_anon(parent, 0x620000, TEST_PAGE_SIZE,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &parent_region) == 0);
		assert(vmspace_copy_to(parent, 0x620000, "S", 1) == 0);
		parent_page = parent_region->pages;
		assert(vmspace_fork(parent, &child) == 0 && child != NULL);
		child_page = vmspace_find_region(child, 0x620000, 1)->pages;
		force_shared_backing_swapped(parent_page, child_page);

		first = (struct fault_thread_args){ parent, 0x620000, -1 };
		second = (struct fault_thread_args){ child, 0x620000, -1 };
		before_sleeps = vm_test_waitq_sleep_count();
		block_swap_read = 1;
		swap_read_entered = release_swap_read = 0;
		assert(thrd_create(&first_handle, fault_thread, &first) ==
		    thrd_success);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		while (swap_read_entered == 0)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_create(&second_handle, fault_thread, &second) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		release_swap_read = 1;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_join(first_handle, NULL) == thrd_success);
		assert(thrd_join(second_handle, NULL) == thrd_success);
		block_swap_read = 0;
		assert(first.result == 0 && second.result == 0);
		assert(swap_reads == before_reads + 1U &&
		    swap_slots_freed == before_frees + 1U &&
		    page_ins == before_ins + 1U);
		assert(parent_page->private_page == child_page->private_page);
		assert((parent_page->private_page->flags & VM_PAGE_RESIDENT) != 0 &&
		    (parent_page->private_page->flags & VM_PAGE_SWAPPED) == 0);
		assert(fake_pte_present(parent->space, 0x620000) &&
		    fake_pte_present(child->space, 0x620000));
		assert(vmspace_copy_from(parent, &parent_value, 0x620000, 1) == 0);
		assert(vmspace_copy_from(child, &child_value, 0x620000, 1) == 0);
		assert(parent_value == 0x6d && child_value == 0x6d);
		vmspace_put(child);
		vmspace_put(parent);
		assert(commit_used == 0);
	}

	/* A swapped shared backing is paged in once before both COW copies. */
	{
		struct vmspace *parent = vmspace_create();
		struct vmspace *child = NULL;
		struct vm_region *parent_region;
		struct vm_page *parent_page, *child_page;
		struct access_fault_thread_args first, second;
		thrd_t first_handle, second_handle;
		unsigned before_reads = swap_reads;
		unsigned before_frees = swap_slots_freed;
		unsigned before_sleeps;
		char parent_value = 0, child_value = 0;

		assert(parent != NULL);
		assert(vmspace_map_anon(parent, 0x630000, TEST_PAGE_SIZE,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &parent_region) == 0);
		assert(vmspace_copy_to(parent, 0x630000, "S", 1) == 0);
		parent_page = parent_region->pages;
		assert(vmspace_fork(parent, &child) == 0 && child != NULL);
		child_page = vmspace_find_region(child, 0x630000, 1)->pages;
		force_shared_backing_swapped(parent_page, child_page);

		first = (struct access_fault_thread_args){
			parent, 0x630000, HAL_SPACE_WRITE, -1
		};
		second = (struct access_fault_thread_args){
			child, 0x630000, HAL_SPACE_WRITE, -1
		};
		before_sleeps = vm_test_waitq_sleep_count();
		block_swap_read = 1;
		swap_read_entered = release_swap_read = 0;
		assert(thrd_create(&first_handle, access_fault_thread, &first) ==
		    thrd_success);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		while (swap_read_entered == 0)
			assert(cnd_wait(&pread_checkpoint_condition,
			    &pread_checkpoint_lock) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_create(&second_handle, access_fault_thread, &second) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
		assert(mtx_lock(&pread_checkpoint_lock) == thrd_success);
		release_swap_read = 1;
		assert(cnd_broadcast(&pread_checkpoint_condition) == thrd_success);
		assert(mtx_unlock(&pread_checkpoint_lock) == thrd_success);
		assert(thrd_join(first_handle, NULL) == thrd_success);
		assert(thrd_join(second_handle, NULL) == thrd_success);
		block_swap_read = 0;
		assert(first.result == 0 && second.result == 0);
		assert(swap_reads == before_reads + 1U &&
		    swap_slots_freed == before_frees + 1U);
		assert(parent_page->private_page != child_page->private_page);
		assert((parent_page->flags & VM_MAPPING_COW) == 0 &&
		    (child_page->flags & VM_MAPPING_COW) == 0);
		assert(vmspace_copy_from(parent, &parent_value, 0x630000, 1) == 0);
		assert(vmspace_copy_from(child, &child_value, 0x630000, 1) == 0);
		assert(parent_value == 0x6d && child_value == 0x6d);
		vmspace_put(child);
		vmspace_put(parent);
		assert(commit_used == 0);
	}

	/* Multiple syscall outputs may pin disjoint ranges on one stack page. */
	{
		struct vmspace *pinned_vm = vmspace_create();
		struct vm_region *region;
		struct uaccess_pin first, second;

		assert(pinned_vm != NULL);
		assert(vmspace_map_anon(pinned_vm, 0x650000, TEST_PAGE_SIZE,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &region) == 0);
		assert(vmspace_copy_to(pinned_vm, 0x650000, "A", 1) == 0);
		assert(uaccess_pin_vmspace(pinned_vm, 0x650000, 32,
		    HAL_SPACE_WRITE, &first) == 0);
		assert(uaccess_pin_vmspace(pinned_vm, 0x650080, 16,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &second) == 0);
		assert(first.pages[0].owner.private_page ==
		    second.pages[0].owner.private_page);
		assert(region->pages->private_page->pin_count == 2);
		uaccess_unpin(&second);
		uaccess_unpin(&first);
		vmspace_put(pinned_vm);
		assert(commit_used == 0);
	}

	/* A published pin owns the old backing after the VA is unmapped/reused. */
	{
		struct vmspace *pinned_vm = vmspace_create();
		struct vm_region *old_region, *new_region;
		struct vm_private_page *old_backing, *new_backing;
		struct uaccess_pin pinned;
		unsigned new_flags;
		uint64_t new_generation;

		assert(pinned_vm != NULL);
		assert(vmspace_map_anon(pinned_vm, 0x650000, TEST_PAGE_SIZE,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &old_region) == 0);
		assert(vmspace_copy_to(pinned_vm, 0x650000, "A", 1) == 0);
		old_backing = old_region->pages->private_page;
		assert(uaccess_pin_vmspace(pinned_vm, 0x650000, 1,
		    HAL_SPACE_WRITE, &pinned) == 0);
		assert(pinned.pages[0].owner.private_page == old_backing &&
		    old_backing->pin_count == 1);
		assert(vmspace_unmap(pinned_vm, 0x650000, TEST_PAGE_SIZE) == 0);
		assert(old_backing->mapping_count == 0 && old_backing->pin_count == 1);
		assert(vmspace_map_anon_fixed_noreplace(pinned_vm, 0x650000,
		    TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE, &new_region) == 0);
		assert(vmspace_copy_to(pinned_vm, 0x650000, "N", 1) == 0);
		new_backing = new_region->pages->private_page;
		assert(new_backing != old_backing);
		new_flags = new_backing->flags;
		new_generation = new_backing->generation;
		assert(copyout_pinned(&pinned, 0, "O", 1) == 0);
		assert(((char *)old_backing->pmem.vaddr)[0] == 'O');
		assert(((char *)new_backing->pmem.vaddr)[0] == 'N');
		assert(new_backing->flags == new_flags &&
		    new_backing->generation == new_generation);
		uaccess_unpin(&pinned);
		vmspace_put(pinned_vm);
		assert(commit_used == 0);
	}

	/*
	 * An object-backed pin also owns the old cache page after its VA is
	 * unmapped and reused.  A second mapping keeps final object teardown from
	 * waiting for the deliberately outstanding pin.
	 */
	{
		static uint8_t object_data[TEST_PAGE_SIZE];
		struct inode object_inode = {
			.i_type = INODE_REG, .i_size = sizeof(object_data)
		};
		struct file object_file = {
			.f_data = object_data, .f_inode = &object_inode,
			.f_ops = &shared_file_ops, .f_flags = { O_RDWR }
		};
		struct vmspace *pinned_vm = vmspace_create();
		struct vm_region *old_region, *shadow_region, *new_region;
		struct vm_object_page *old_page;
		struct vm_private_page *new_backing;
		struct uaccess_pin pinned;
		unsigned new_flags;
		uint64_t old_dirty_generation, new_generation;
		char value = 0;

		assert(pinned_vm != NULL);
		refcount_init(&object_file.f_refs, 1);
		memcpy(object_data, "A", 1);
		assert(vmspace_map_file_shared(pinned_vm, 0x690000,
		    TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE, &object_file,
		    0, sizeof(object_data), &old_region) == 0);
		assert(vmspace_map_file_shared(pinned_vm, 0x6a0000,
		    TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE, &object_file,
		    0, sizeof(object_data), &shadow_region) == 0);
		assert(vmspace_fault(pinned_vm, 0x690000, HAL_SPACE_WRITE) == 0);
		assert(vmspace_fault(pinned_vm, 0x6a0000, HAL_SPACE_READ) == 0);
		old_page = old_region->pages->object_page;
		assert(old_page != NULL && shadow_region->pages->object_page == old_page &&
		    (old_page->flags & VM_OBJECT_PAGE_DIRTY) == 0);
		old_dirty_generation = old_page->dirty_generation;
		assert(uaccess_pin_vmspace(pinned_vm, 0x690000, 1,
		    HAL_SPACE_WRITE, &pinned) == 0);
		assert(pinned.pages[0].kind == VMSPACE_PINNED_OBJECT &&
		    pinned.pages[0].owner.object_page == old_page &&
		    old_page->pin_count == 1);
		assert(vmspace_unmap(pinned_vm, 0x690000, TEST_PAGE_SIZE) == 0);
		assert(old_page->mapping_count == 1 && old_page->pin_count == 1);
		assert(vmspace_map_anon_fixed_noreplace(pinned_vm, 0x690000,
		    TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE, &new_region) == 0);
		assert(vmspace_copy_to(pinned_vm, 0x690000, "N", 1) == 0);
		new_backing = new_region->pages->private_page;
		assert(new_backing != NULL);
		new_flags = new_backing->flags;
		new_generation = new_backing->generation;
		assert(copyout_pinned(&pinned, 0, "O", 1) == 0);
		assert(((char *)old_page->pmem.vaddr)[0] == 'O' &&
		    (old_page->flags & VM_OBJECT_PAGE_DIRTY) != 0 &&
		    old_page->dirty_generation != old_dirty_generation);
		assert(((char *)new_backing->pmem.vaddr)[0] == 'N' &&
		    new_backing->flags == new_flags &&
		    new_backing->generation == new_generation);
		assert(vmspace_copy_from(pinned_vm, &value, 0x6a0000, 1) == 0 &&
		    value == 'O');
		uaccess_unpin(&pinned);
		assert(vmspace_unmap(pinned_vm, 0x690000, TEST_PAGE_SIZE) == 0);
		assert(vmspace_unmap(pinned_vm, 0x6a0000, TEST_PAGE_SIZE) == 0);
		assert(object_data[0] == 'O' && refcount_load(&object_file.f_refs) == 1);
		assert(vm_object_count() == 0 && vm_object_page_count() == 0);
		vmspace_put(pinned_vm);
		assert(commit_used == 0);
	}

	/* The range snapshot is atomic: remap cannot interleave between its pins. */
	{
		struct vmspace *pinned_vm = vmspace_create();
		struct vm_region *old_region, *new_region;
		struct vm_page *old_first_page, *old_second_page;
		struct vm_private_page *old_first, *old_second;
		struct vm_private_page *new_first, *new_second;
		struct pin_remap_args remap = {
			.vm = pinned_vm, .address = 0x660000,
			.size = 2U * TEST_PAGE_SIZE,
			.unmap_error = -1, .map_error = -1, .fault_error = -1
		};
		struct uaccess_pin pinned;
		thrd_t remap_handle;
		char value = 0;

		assert(pinned_vm != NULL);
		assert(vmspace_map_anon(pinned_vm, remap.address, remap.size,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &old_region) == 0);
		assert(vmspace_copy_to(pinned_vm, remap.address, "A", 1) == 0);
		assert(vmspace_copy_to(pinned_vm, remap.address + TEST_PAGE_SIZE,
		    "B", 1) == 0);
		for (old_first_page = old_region->pages; old_first_page != NULL &&
		    old_first_page->address != remap.address;
		    old_first_page = old_first_page->next)
			;
		for (old_second_page = old_region->pages; old_second_page != NULL &&
		    old_second_page->address != remap.address + TEST_PAGE_SIZE;
		    old_second_page = old_second_page->next)
			;
		assert(old_first_page != NULL && old_second_page != NULL);
		old_first = old_first_page->private_page;
		old_second = old_second_page->private_page;
		pin_checkpoint_vm = pinned_vm;
		pin_checkpoint_mode = 1;
		pin_checkpoint_seen = pin_peer_started = 0;
		assert(thrd_create(&remap_handle, pin_remap_thread, &remap) ==
		    thrd_success);
		assert(uaccess_pin_vmspace(pinned_vm, remap.address, remap.size,
		    HAL_SPACE_WRITE, &pinned) == 0);
		assert(thrd_join(remap_handle, NULL) == thrd_success);
		assert(remap.unmap_error == 0 && remap.map_error == 0 &&
		    remap.fault_error == 0);
		assert(pinned.page_count == 2 &&
		    pinned.pages[0].owner.private_page == old_first &&
		    pinned.pages[1].owner.private_page == old_second);
		new_region = vmspace_find_region(pinned_vm, remap.address, remap.size);
		assert(new_region != NULL);
		for (old_first_page = new_region->pages; old_first_page != NULL &&
		    old_first_page->address != remap.address;
		    old_first_page = old_first_page->next)
			;
		for (old_second_page = new_region->pages; old_second_page != NULL &&
		    old_second_page->address != remap.address + TEST_PAGE_SIZE;
		    old_second_page = old_second_page->next)
			;
		assert(old_first_page != NULL && old_second_page != NULL);
		new_first = old_first_page->private_page;
		new_second = old_second_page->private_page;
		assert(new_first != old_first && new_second != old_second);
		assert(vmspace_copy_to(pinned_vm, remap.address, "N", 1) == 0);
		assert(vmspace_copy_to(pinned_vm, remap.address + TEST_PAGE_SIZE,
		    "M", 1) == 0);
		assert(copyout_pinned(&pinned, 0, "X", 1) == 0);
		assert(copyout_pinned(&pinned, TEST_PAGE_SIZE, "Y", 1) == 0);
		assert(((char *)old_first->pmem.vaddr)[0] == 'X' &&
		    ((char *)old_second->pmem.vaddr)[0] == 'Y');
		assert(vmspace_copy_from(pinned_vm, &value, remap.address, 1) == 0 &&
		    value == 'N');
		assert(vmspace_copy_from(pinned_vm, &value,
		    remap.address + TEST_PAGE_SIZE, 1) == 0 && value == 'M');
		uaccess_unpin(&pinned);
		vmspace_put(pinned_vm);
		assert(commit_used == 0);
	}

	/*
	 * If a later object pin fails, an earlier private pin and every object
	 * lifetime counter remain at their pre-attempt values.  Saturating the
	 * page hold count exercises vm_object_page_pin()'s real overflow path.
	 */
	{
		static uint8_t object_data[TEST_PAGE_SIZE];
		struct inode object_inode = {
			.i_type = INODE_REG, .i_size = sizeof(object_data)
		};
		struct file object_file = {
			.f_data = object_data, .f_inode = &object_inode,
			.f_ops = &shared_file_ops, .f_flags = { O_RDWR }
		};
		struct vmspace *pinned_vm = vmspace_create();
		struct vm_region *private_region, *object_region;
		struct vm_private_page *private_page;
		struct vm_object_page *object_page;
		struct vm_object *object;
		struct uaccess_pin pinned;
		unsigned baseline_active, baseline_hold, baseline_pin;
		unsigned baseline_refs;
		unsigned long irq;

		assert(pinned_vm != NULL);
		refcount_init(&object_file.f_refs, 1);
		assert(vmspace_map_anon_fixed_noreplace(pinned_vm, 0x6b0000,
		    TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE,
		    &private_region) == 0);
		assert(vmspace_map_file_shared(pinned_vm, 0x6b1000,
		    TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE, &object_file,
		    0, sizeof(object_data), &object_region) == 0);
		assert(vmspace_fault(pinned_vm, 0x6b0000, HAL_SPACE_WRITE) == 0);
		assert(vmspace_fault(pinned_vm, 0x6b1000, HAL_SPACE_WRITE) == 0);
		private_page = private_region->pages->private_page;
		object_page = object_region->pages->object_page;
		assert(private_page != NULL && object_page != NULL);
		object = object_page->owner;
		baseline_active = object->active_operations;
		baseline_refs = refcount_load(&object->refs);
		irq = spin_lock_irqsave(&object->lock);
		baseline_hold = object_page->hold_count;
		baseline_pin = object_page->pin_count;
		spin_unlock_irqrestore(&object->lock, irq);

		pin_checkpoint_vm = pinned_vm;
		pin_rollback_first = private_page;
		pin_rollback_object = object_page;
		pin_checkpoint_mode = 3;
		assert(uaccess_pin_vmspace(pinned_vm, 0x6b0000,
		    2U * TEST_PAGE_SIZE, HAL_SPACE_WRITE, &pinned) == EFAULT);
		assert(private_page->pin_count == 0 &&
		    object->active_operations == baseline_active &&
		    refcount_load(&object->refs) == baseline_refs &&
		    object_page->pin_count == baseline_pin);
		irq = spin_lock_irqsave(&object->lock);
		assert(object_page->hold_count == UINT_MAX);
		object_page->hold_count = pin_rollback_object_hold;
		assert(object_page->hold_count == baseline_hold &&
		    object_page->pin_count == baseline_pin);
		spin_unlock_irqrestore(&object->lock, irq);
		pin_rollback_first = NULL;
		pin_rollback_object = NULL;
		assert(vmspace_unmap(pinned_vm, 0x6b0000,
		    2U * TEST_PAGE_SIZE) == 0);
		assert(vm_object_count() == 0 && vm_object_page_count() == 0 &&
		    refcount_load(&object_file.f_refs) == 1);
		vmspace_put(pinned_vm);
		assert(commit_used == 0);
	}

	/* A failed Nth backing pin rolls every earlier page back before waiting. */
	{
		struct vmspace *pinned_vm = vmspace_create();
		struct vm_region *pin_region;
		struct vm_page *first_page, *second_page;
		struct pin_rollback_args rollback = { 0 };
		struct uaccess_pin pinned;
		thrd_t rollback_handle;

		assert(pinned_vm != NULL);
		assert(vmspace_map_anon(pinned_vm, 0x670000,
		    2U * TEST_PAGE_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE,
		    &pin_region) == 0);
		assert(vmspace_fault(pinned_vm, 0x670000, HAL_SPACE_WRITE) == 0);
		assert(vmspace_fault(pinned_vm, 0x671000, HAL_SPACE_WRITE) == 0);
		for (first_page = pin_region->pages; first_page != NULL &&
		    first_page->address != 0x670000; first_page = first_page->next)
			;
		for (second_page = pin_region->pages; second_page != NULL &&
		    second_page->address != 0x671000; second_page = second_page->next)
			;
		assert(first_page != NULL && second_page != NULL);
		pin_rollback_first = first_page->private_page;
		pin_rollback_second = second_page->private_page;
		pin_checkpoint_vm = pinned_vm;
		pin_checkpoint_mode = 2;
		pin_checkpoint_seen = pin_peer_started = 0;
		rollback.before_sleeps = vm_test_waitq_sleep_count();
		assert(thrd_create(&rollback_handle, pin_rollback_thread, &rollback) ==
		    thrd_success);
		assert(uaccess_pin_vmspace(pinned_vm, 0x670000,
		    2U * TEST_PAGE_SIZE, HAL_SPACE_WRITE, &pinned) == 0);
		assert(thrd_join(rollback_handle, NULL) == thrd_success);
		assert(rollback.rollback_seen && pin_rollback_first->pin_count == 1 &&
		    pin_rollback_second->pin_count == 1);
		uaccess_unpin(&pinned);
		assert(pin_rollback_first->pin_count == 0 &&
		    pin_rollback_second->pin_count == 0);
		pin_rollback_first = pin_rollback_second = NULL;
		vmspace_put(pinned_vm);
		assert(commit_used == 0);
	}

	/* A WRITE vector breaks pre-existing COW before direct frame access. */
	{
		struct vmspace *parent = vmspace_create();
		struct vmspace *child = NULL;
		struct vm_region *parent_region;
		struct vm_page *parent_page, *child_page;
		struct uaccess_pin pinned;
		char parent_value = 0, child_value = 0;

		assert(parent != NULL);
		assert(vmspace_map_anon(parent, 0x680000, TEST_PAGE_SIZE,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &parent_region) == 0);
		assert(vmspace_copy_to(parent, 0x680000, "P", 1) == 0);
		assert(vmspace_fork(parent, &child) == 0 && child != NULL);
		parent_page = parent_region->pages;
		child_page = vmspace_find_region(child, 0x680000, 1)->pages;
		assert(parent_page->private_page == child_page->private_page);
		assert(uaccess_pin_vmspace(parent, 0x680000, 1,
		    HAL_SPACE_WRITE, &pinned) == 0);
		assert((parent_page->flags & VM_MAPPING_COW) == 0 &&
		    parent_page->private_page != child_page->private_page);
		assert(copyout_pinned(&pinned, 0, "Q", 1) == 0);
		uaccess_unpin(&pinned);
		assert(vmspace_copy_from(parent, &parent_value, 0x680000, 1) == 0);
		assert(vmspace_copy_from(child, &child_value, 0x680000, 1) == 0);
		assert(parent_value == 'Q' && child_value == 'P');
		vmspace_put(child);
		vmspace_put(parent);
		assert(commit_used == 0);
	}

	/*
	 * fork must not publish a COW-shared child while a direct write pin can
	 * still modify the old frame.  It waits, then snapshots the pinned write.
	 */
	{
		struct vmspace *parent = vmspace_create();
		struct vm_region *parent_region;
		struct vm_page *parent_page, *child_page;
		struct uaccess_pin pinned;
		struct fork_thread_args fork_args = { parent, NULL, -1 };
		thrd_t fork_handle;
		unsigned before_sleeps;
		char parent_value = 0, child_value = 0;

		assert(parent != NULL);
		assert(vmspace_map_anon(parent, 0x640000, TEST_PAGE_SIZE,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &parent_region) == 0);
		assert(vmspace_copy_to(parent, 0x640000, "P", 1) == 0);
		parent_page = parent_region->pages;
		assert(uaccess_pin_vmspace(parent, 0x640000, 1,
		    HAL_SPACE_WRITE, &pinned) == 0);
		before_sleeps = vm_test_waitq_sleep_count();
		assert(thrd_create(&fork_handle, fork_thread, &fork_args) ==
		    thrd_success);
		vm_test_waitq_wait_for_sleep(before_sleeps + 1U);
		assert(copyout_pinned(&pinned, 0, "Q", 1) == 0);
		uaccess_unpin(&pinned);
		assert(thrd_join(fork_handle, NULL) == thrd_success);
		assert(fork_args.result == 0 && fork_args.copy != NULL);
		child_page = vmspace_find_region(fork_args.copy, 0x640000, 1)->pages;
		assert(child_page->private_page == parent_page->private_page);
		assert(vmspace_copy_from(fork_args.copy, &child_value,
		    0x640000, 1) == 0 && child_value == 'Q');
		assert(vmspace_copy_to(parent, 0x640000, "R", 1) == 0);
		assert(vmspace_copy_from(parent, &parent_value, 0x640000, 1) == 0);
		assert(vmspace_copy_from(fork_args.copy, &child_value,
		    0x640000, 1) == 0);
		assert(parent_value == 'R' && child_value == 'Q');
		vmspace_put(fork_args.copy);
		vmspace_put(parent);
		assert(commit_used == 0);
	}

	fail_space = 1;
	assert(vmspace_create() == NULL);
	fail_space = 0;
	{
		struct vmspace *deferred = vmspace_create();
		unsigned notification_before;

		assert(deferred != NULL);
		vmspace_set_reaper_notify(fake_reaper_notify,
		    &reap_notify_count);
		/* Model an exit notification which the reaper consumes before a VM
		 * reclaim path performs the final deferred put.  The enqueue itself
		 * must publish a second retained notification. */
		fake_reaper_notify(&reap_notify_count);
		assert(reap_notify_latched != 0);
		reap_notify_latched = 0;
		assert(vmspace_reap_pending() == 0);
		notification_before = reap_notify_count;
		vmspace_put_deferred(deferred);
		assert(reap_notify_count == notification_before + 1U);
		assert(reap_notify_latched != 0);
		assert(spaces_created == spaces_destroyed + 1);
		assert(vmspace_reap_pending() == 1);
		assert(vmspace_reap_pending() == 0);
		assert(spaces_created == spaces_destroyed);
		vmspace_set_reaper_notify(NULL, NULL);
	}
	cnd_destroy(&pin_checkpoint_condition);
	mtx_destroy(&pin_checkpoint_lock);
	puts("zedBSD demand-paged vmspace host tests: PASS");
	return 0;
}
