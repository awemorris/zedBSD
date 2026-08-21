#include "kern/kmem.h"
#include "kern/swap.h"
#include "kern/vm-reclaim.h"
#include "kern/vmspace.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t query_flags;
static unsigned unmaps, frees;
static uint8_t swap_data[SWAP_PAGE_SIZE];

void *kern_calloc(size_t n, size_t s) { return calloc(n, s); }
void kern_free(void *p) { free(p); }
void vm_page_free_metadata(struct vm_page *page) { free(page); }

int hal_page_query(hal_space_t s, void *v, uint32_t *flags)
{ (void)s; (void)v; *flags = query_flags; return HAL_OK; }
int hal_page_clear_flags(hal_space_t s, void *v, uint32_t flags)
{ (void)s; (void)v; query_flags &= ~flags; return HAL_OK; }
int hal_page_unmap(hal_space_t s, void *v, size_t z)
{ (void)s; (void)v; (void)z; unmaps++; return HAL_OK; }
int hal_page_map(hal_space_t s, void *v, hal_physaddr_t p, size_t z, uint32_t a)
{ (void)s; (void)v; (void)p; (void)z; (void)a; return HAL_OK; }
int hal_page_prot(hal_space_t s, void *v, size_t z, uint32_t a)
{ (void)s; (void)v; (void)z; (void)a; return HAL_OK; }
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d: %s\n", file, line, message); abort(); }
int hal_pmem_free(struct hal_pmem *p)
{ free((void *)p->vaddr); memset(p, 0, sizeof(*p)); frees++; return 0; }

static int swap_read(void *d, uint32_t slot, void *page)
{ (void)d; (void)slot; memcpy(page, swap_data, sizeof(swap_data)); return 0; }
static int swap_write(void *d, uint32_t slot, const void *page)
{ (void)d; (void)slot; memcpy(swap_data, page, sizeof(swap_data)); return 0; }

static struct vm_page *make_page(struct vmspace *vm, struct vm_region *region,
				 unsigned flags, uint8_t fill)
{
	struct vm_page *page = calloc(1, sizeof(*page));
	struct vm_private_page *backing = calloc(1, sizeof(*backing));
	assert(page != NULL && backing != NULL);
	refcount_init(&backing->refs, 1);
	backing->pmem.vaddr = malloc(4096);
	backing->pmem.paddr = (hal_physaddr_t)(uintptr_t)backing->pmem.vaddr &
	    ~(hal_physaddr_t)4095U;
	backing->pmem.size = 4096;
	backing->pmem.type = HAL_PMEM_TYPE_RAM;
	backing->flags = flags | VM_PAGE_RESIDENT;
	backing->swap_slot = SWAP_SLOT_NONE;
	memset(backing->pmem.vaddr, fill, 4096);
	page->address = region->start;
	page->vm = vm;
	page->region = region;
	page->flags = VM_MAPPING_MAPPED;
	page->private_page = backing;
	backing->mappings = page;
	region->pages = page;
	vm_page_track(page);
	return page;
}

int main(void)
{
	static const struct swap_backend_ops ops = {
		.read_page = swap_read, .write_page = swap_write,
	};
	struct swap_backend backend;
	struct vmspace vm = { .space = (hal_space_t)1 };
	struct vm_region clean = { .start = 0x400000, .size = 4096,
		.prot = HAL_SPACE_READ, .backing = VM_BACKING_FILE };
	struct vm_region dirty = { .start = 0x500000, .size = 4096,
		.prot = HAL_SPACE_READ | HAL_SPACE_WRITE,
		.backing = VM_BACKING_ANON };
	struct vm_page *page;
	struct vm_reclaim_stats stats;

	vm_reclaim_init();
	page = make_page(&vm, &clean, 0, 0x11);
	query_flags = HAL_PAGE_PRESENT | HAL_PAGE_ACCESSED;
	assert(vm_reclaim_one(NULL) == 0);
	assert(clean.pages == NULL && unmaps == 1 && frees == 1);
	(void)page;

	swap_init(&backend);
	assert(swap_activate(&backend, &ops, NULL, SWAP_PAGE_SIZE, 2) == 0);
	swap_set_system_backend(&backend);
	page = make_page(&vm, &dirty, VM_PAGE_DIRTY, 0x5a);
	query_flags = HAL_PAGE_PRESENT;
	assert(vm_reclaim_one(NULL) == 0);
	assert((page->private_page->flags & VM_PAGE_SWAPPED) != 0);
	assert((page->private_page->flags & VM_PAGE_RESIDENT) == 0);
	assert(page->private_page->swap_slot != SWAP_SLOT_NONE &&
	    swap_data[123] == 0x5a);
	vm_reclaim_get_stats(&stats);
	assert(stats.page_outs == 1 && stats.swapped == 1);
	vm_page_untrack(page);
	vm_page_free_metadata(page);
	assert(swap_shutdown(&backend) == 0);
	puts("zedBSD VM reclaim/swap host tests: PASS");
	return 0;
}
