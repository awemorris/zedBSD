#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/swap.h"
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

struct fake_space { unsigned maps; };
static unsigned spaces_created, spaces_destroyed;
static unsigned pages_allocated, pages_freed, pages_unmapped, pages_protected;
static int fail_space, fail_pages, fail_map, fail_protect;
static unsigned page_ins, swap_reads, swap_slots_freed;
static struct swap_backend fake_swap;
static size_t commit_used;
static int fail_commit;
static unsigned pwrite_calls;

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

void vm_page_track(struct vm_page *page) { (void)page; }
void vm_page_untrack(struct vm_page *page) { (void)page; }
void vm_page_note_in(struct vm_page *page) { (void)page; page_ins++; }
void vm_reclaim_note_fault(void) { }
int vm_reclaim_one(struct vm_page *page) { (void)page; return ENOMEM; }
struct swap_backend *swap_system_backend(void) { return &fake_swap; }
int swap_read_page(struct swap_backend *b, uint32_t s, void *p)
{
	assert(b == &fake_swap && s == 7);
	memset(p, 0x6d, SWAP_PAGE_SIZE);
	swap_reads++;
	return 0;
}
void swap_free_slot(struct swap_backend *b, uint32_t s)
{
	assert(b == &fake_swap && s == 7);
	swap_slots_freed++;
}

void file_ref(struct file *file) { file->f_usecount++; }
int file_close(struct file *file) { file->f_usecount--; return 0; }
struct inode *file_vm_inode(struct file *file)
{
	return file->f_vm_inode != NULL ? file->f_vm_inode : file->f_inode;
}
ssize_t file_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	const uint8_t *data = file->f_data;
	memcpy(buffer, data + offset, length);
	return (ssize_t)length;
}
ssize_t file_pwrite(struct file *file, const void *buffer, size_t length,
		    off_t offset)
{
	uint8_t *data = file->f_data;
	memcpy(data + offset, buffer, length);
	pwrite_calls++;
	return (ssize_t)length;
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

int hal_pmem_alloc(size_t size, struct hal_pmem *memory, uint32_t flags)
{
	(void)flags;
	if (fail_pages) return HAL_PMEM_NOSPACE;
	memory->vaddr = (uintptr_t)calloc(1, size);
	if (memory->vaddr == 0) return HAL_PMEM_NOSPACE;
	memory->paddr = memory->vaddr & ~(uintptr_t)4095U;
	memory->size = size;
	pages_allocated++;
	return HAL_PMEM_SUCCESS;
}

int hal_pmem_free(struct hal_pmem *memory)
{
	free((void *)memory->vaddr);
	pages_freed++;
	return HAL_PMEM_SUCCESS;
}

int hal_page_map(hal_space_t handle, void *address, uintptr_t paddr,
		 size_t size, uint32_t attr)
{
	struct fake_space *space = handle;
	(void)address; (void)paddr; (void)size; (void)attr;
	if (fail_map) return HAL_PMEM_BADDESC;
	space->maps++;
	return HAL_PMEM_SUCCESS;
}

int hal_page_unmap(hal_space_t handle, void *address, size_t size)
{
	(void)handle; (void)address; (void)size;
	pages_unmapped++;
	return HAL_PMEM_SUCCESS;
}

int hal_page_prot(hal_space_t handle, void *address, size_t size, uint32_t attr)
{
	(void)handle; (void)address; (void)size; (void)attr;
	if (fail_protect) return HAL_PMEM_BADDESC;
	pages_protected++;
	return HAL_PMEM_SUCCESS;
}

int hal_page_query(hal_space_t handle, void *address, uint32_t *flags)
{
	(void)handle; (void)address;
	if (flags != NULL)
		*flags = 0;
	return HAL_PMEM_SUCCESS;
}
int hal_page_clear_flags(hal_space_t handle, void *address, uint32_t flags)
{
	(void)handle; (void)address; (void)flags;
	return HAL_PMEM_SUCCESS;
}

int main(void)
{
	static const uint8_t file_data[] = "xxELF!tail";
	struct vmspace *vm;
	struct vm_region *region;
	struct file file = { .f_usecount = 1, .f_data = (void *)file_data };
	struct vm_page *page;
	uintptr_t mapped;
	char buffer[8];
	static uint8_t shared_data[4096];
	struct inode shared_inode = {
		.i_type = INODE_REG, .i_size = sizeof(shared_data)
	};
	struct file shared_file = {
		.f_usecount = 1, .f_data = shared_data, .f_inode = &shared_inode,
		.f_flags = O_RDWR
	};

	vm = vmspace_create();
	assert(vm != NULL && spaces_created == 1);
	assert(vmspace_map_anon(vm, 0x400000, 8192,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &region) == 0);
	assert(commit_used == 8192);
	assert(region->start == 0x400000 && region->size == 8192);
	assert(region->pages == NULL && pages_allocated == 0);
	assert(vmspace_fault(vm, 0x400123, HAL_SPACE_READ) == 0);
	assert(pages_allocated == 1 && region->pages != NULL);
	assert(vmspace_copy_to(vm, 0x400ffe, "abcd", 4) == 0);
	assert(pages_allocated == 2);
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
	region->pages->flags &= ~VM_PAGE_RESIDENT;
	region->pages->flags |= VM_PAGE_SWAPPED;
	assert(vmspace_protect(vm, 0x400000, 8192, HAL_SPACE_READ) == 0);
	assert(pages_protected == 1);
	region->pages->flags &= ~VM_PAGE_SWAPPED;
	region->pages->flags |= VM_PAGE_RESIDENT;
	fail_protect = 1;
	assert(vmspace_protect(vm, 0x400000, 8192,
		HAL_SPACE_READ | HAL_SPACE_WRITE) == EINVAL);
	fail_protect = 0;
	assert(vmspace_copy_to(vm, 0x400000, "x", 1) == EFAULT);
	assert(vmspace_unmap(vm, 0x400000, 8192) == 0);
	assert(pages_unmapped == 2 && pages_freed == 2);
	assert(commit_used == 0);

	assert(vmspace_map_anon(vm, 0x800000, 4096, 0, &region) == 0);
	assert(commit_used == 0 && region->commit_size == 0);
	assert(vmspace_protect(vm, 0x800000, 4096, HAL_SPACE_READ) == 0);
	assert(commit_used == 4096 && region->commit_size == 4096);
	assert(vmspace_protect(vm, 0x800000, 4096, 0) == 0);
	assert(commit_used == 4096);
	assert(vmspace_unmap(vm, 0x800000, 4096) == 0);
	assert(commit_used == 0);

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
	assert(file.f_usecount == 2 && region->pages == NULL);
	assert(vmspace_fault(vm, 0x600003, HAL_SPACE_READ) == 0);
	memset(buffer, 0xaa, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x600000, 8) == 0);
	assert(buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0);
	assert(!memcmp(buffer + 3, "ELF!t", 5));
	assert(vmspace_map_file(vm, 0x900000, 4096,
		HAL_SPACE_READ | HAL_SPACE_WRITE, &file, 2, 0x900003, 5,
		&region) == 0);
	assert(region->commit_size == 4096 && commit_used == 8192);

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
	assert(vmspace_sync(vm, 0x00a00000, 4096, MS_SYNC) == 0);
	assert(pwrite_calls == 1 && !memcmp(shared_data, "object", 6));
	assert(vm_object_reclaim_one() == 0 && vm_object_page_count() == 0);
	memset(buffer, 0, sizeof(buffer));
	assert(vmspace_copy_from(vm, buffer, 0x00a01000, 6) == 0);
	assert(!memcmp(buffer, "object", 6) && vm_object_page_count() == 1);
	assert(vmspace_unmap(vm, 0x00a00000, 8192) == 0);
	assert(vm_object_count() == 0 && vm_object_page_count() == 0);

	assert(vmspace_set_brk_start(vm, 0x01000000U) == 0);
	assert(vmspace_brk(vm, 0, &mapped) == 0 && mapped == 0x01000000U);
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
	page = calloc(1, sizeof(*page));
	assert(page != NULL);
	page->address = region->start;
	page->vm = vm;
	page->region = region;
	page->flags = VM_PAGE_SWAPPED;
	page->swap_slot = 7;
	region->pages = page;
	assert(vmspace_fault(vm, region->start, HAL_SPACE_READ) == 0);
	assert((page->flags & VM_PAGE_RESIDENT) != 0);
	assert((page->flags & VM_PAGE_DIRTY) != 0);
	assert((page->flags & VM_PAGE_SWAPPED) == 0);
	assert(page->swap_slot == SWAP_SLOT_NONE);
	assert(((uint8_t *)page->pmem.vaddr)[123] == 0x6d);
	assert(page_ins == 1 && swap_reads == 1 && swap_slots_freed == 1);

	vmspace_free(vm);
	assert(file.f_usecount == 1);
	assert(spaces_destroyed == 1 && pages_allocated == pages_freed);
	assert(commit_used == 0);

	fail_space = 1;
	assert(vmspace_create() == NULL);
	puts("zedBSD demand-paged vmspace host tests: PASS");
	return 0;
}
