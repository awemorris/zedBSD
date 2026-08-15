/* amd64 four-level page-table implementation. */
#include <hal/hal.h>
#include "defs.h"
#include "asm.h"
#include "space.h"

static uint64 system_pml4[512] __attribute__((aligned(PAGE_SIZE)));
static uint64 system_pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64 system_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64 system_kernel_pt[8][512]
	__attribute__((aligned(PAGE_SIZE)));
static uint64 system_mmio_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uintptr_t system_cr3;
static hal_space_t current_space;
static int next_space_id = 1;
static uint32 space_count;
static uint32 page_table_count;

extern char __kernel_phys_start[], __kernel_phys_end[];
extern char __kernel_text_phys_start[], __kernel_text_phys_end[];
extern char __kernel_rodata_phys_start[], __kernel_rodata_phys_end[];
extern char __kernel_data_phys_start[], __kernel_data_phys_end[];

uintptr_t amd64_direct_to_phys(const void *address)
{
	return (uintptr_t)address - (uintptr_t)AMD64_DIRECT_BASE;
}

void *amd64_phys_to_direct(uintptr_t address)
{
	return (void *)((uintptr_t)AMD64_DIRECT_BASE + address);
}

void
amd64_space_init(void)
{
	uintptr_t kernel_start = (uintptr_t)__kernel_phys_start;
	uintptr_t kernel_end = (uintptr_t)__kernel_phys_end;
	unsigned index, first_chunk, chunks, chunk;
	uint64 efer;
	uintptr_t cr0, cr4;

	hal_memset(system_pml4, 0, sizeof(system_pml4));
	hal_memset(system_pdpt, 0, sizeof(system_pdpt));
	hal_memset(system_pd, 0, sizeof(system_pd));
	hal_memset(system_kernel_pt, 0, sizeof(system_kernel_pt));
	hal_memset(system_mmio_pd, 0, sizeof(system_mmio_pd));
	for (index = 0; index < 512; index++)
		system_pd[index] = (uint64)index * 0x200000ULL |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	first_chunk = (unsigned)(kernel_start / 0x200000U);
	chunks = (unsigned)((kernel_end + 0x1fffffU) / 0x200000U) -
	    first_chunk;
	if (chunks == 0 || chunks > 8)
		HAL_FATAL("amd64 kernel W^X window exceeded");
	for (chunk = 0; chunk < chunks; chunk++) {
		uintptr_t base = (uintptr_t)(first_chunk + chunk) * 0x200000U;
		for (index = 0; index < 512; index++) {
			uintptr_t physical = base + (uintptr_t)index * PAGE_SIZE;
			uint64 flags = AMD64_PTE_PRESENT | AMD64_PTE_GLOBAL |
			    AMD64_PTE_NX;
			if (physical >= (uintptr_t)__kernel_text_phys_start &&
			    physical < (uintptr_t)__kernel_text_phys_end)
				flags &= ~AMD64_PTE_NX;
			else if (physical < (uintptr_t)__kernel_rodata_phys_start ||
			    physical >= (uintptr_t)__kernel_rodata_phys_end)
				flags |= AMD64_PTE_WRITE;
			system_kernel_pt[chunk][index] = physical | flags;
		}
		system_pd[first_chunk + chunk] =
		    amd64_direct_to_phys(system_kernel_pt[chunk]) |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	}
	for (index = 0; index < 8; index++)
		system_mmio_pd[index] = (0xf0000000ULL +
		    (uint64)index * 0x200000ULL) | AMD64_PTE_PRESENT |
		    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	system_pdpt[510] = amd64_direct_to_phys(system_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[511] = amd64_direct_to_phys(system_mmio_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pml4[511] = amd64_direct_to_phys(system_pdpt) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	efer = asm_read_msr(0xc0000080U);
	asm_write_msr(0xc0000080U, efer | (1ULL << 11));
	__asm__ volatile("movq %%cr4,%0" : "=r"(cr4));
	cr4 |= 0x80U;
	__asm__ volatile("movq %0,%%cr4" : : "r"(cr4) : "memory");
	__asm__ volatile("movq %%cr0,%0" : "=r"(cr0));
	cr0 |= 0x10000U;
	__asm__ volatile("movq %0,%%cr0" : : "r"(cr0) : "memory");
	system_cr3 = amd64_direct_to_phys(system_pml4);
	asm_load_cr3(system_cr3);
	current_space = HAL_SPACE_SYS;
}

static int valid_space(hal_space_t handle)
{
	return handle == HAL_SPACE_SYS ||
	    (handle != NULL && ((struct amd64_space *)handle)->magic ==
	    AMD64_SPACE_MAGIC);
}

static int valid_user_range(uintptr_t address, size_t size)
{
	return size != 0 && (address & (PAGE_SIZE - 1U)) == 0 &&
	    (size & (PAGE_SIZE - 1U)) == 0 && address >= PAGE_SIZE &&
	    address < 0x0000800000000000ULL &&
	    size <= 0x0000800000000000ULL - address;
}

static struct amd64_table_page *
allocate_table(struct amd64_space *space, uint64 *parent,
	unsigned parent_index)
{
	struct amd64_table_page *page = hal_malloc(sizeof(*page));
	if (page == NULL) return NULL;
	if (pmem_alloc_lo(PAGE_SIZE, &page->memory) != PMEM_SUCCESS) {
		hal_free(page);
		return NULL;
	}
	hal_memset(page->memory.vaddr, 0, PAGE_SIZE);
	page->parent = parent;
	page->parent_index = parent_index;
	page->next = space->tables;
	space->tables = page;
	page_table_count++;
	return page;
}

static uint64 *
walk_leaf(struct amd64_space *space, uintptr_t address, int create)
{
	unsigned shifts[3] = { 39, 30, 21 };
	uint64 *table = space->pml4;
	unsigned level;

	for (level = 0; level < 3; level++) {
		unsigned index = (unsigned)(address >> shifts[level]) & 511U;
		uint64 entry = table[index];
		if (!(entry & AMD64_PTE_PRESENT)) {
			struct amd64_table_page *page;
			if (!create) return NULL;
			page = allocate_table(space, table, index);
			if (page == NULL) return NULL;
			entry = (uintptr_t)page->memory.paddr |
			    AMD64_PTE_PRESENT | AMD64_PTE_WRITE | AMD64_PTE_USER;
			table[index] = entry;
		}
		if (entry & AMD64_PTE_LARGE) return NULL;
		table = amd64_phys_to_direct((uintptr_t)(entry &
		    AMD64_PTE_ADDR_MASK));
	}
	return &table[(address >> 12) & 511U];
}

static int
table_is_empty(const uint64 *table)
{
	unsigned index;
	for (index = 0; index < 512; index++)
		if (table[index] & AMD64_PTE_PRESENT)
			return 0;
	return 1;
}

static void
reclaim_empty_tables(struct amd64_space *space)
{
	int reclaimed;
	do {
		struct amd64_table_page **link = &space->tables;
		reclaimed = 0;
		while (*link != NULL) {
			struct amd64_table_page *page = *link;
			uint64 expected = (uintptr_t)page->memory.paddr;
			if (!table_is_empty(page->memory.vaddr)) {
				link = &page->next;
				continue;
			}
			if ((page->parent[page->parent_index] &
			    AMD64_PTE_ADDR_MASK) == expected)
				page->parent[page->parent_index] = 0;
			*link = page->next;
			(void)pmem_free(&page->memory);
			hal_free(page);
			if (page_table_count != 0) page_table_count--;
			reclaimed = 1;
		}
	} while (reclaimed);
}

hal_space_t
hal_mem_create_space(void)
{
	struct amd64_space *space = hal_malloc(sizeof(*space));
	if (space == NULL) return NULL;
	hal_memset(space, 0, sizeof(*space));
	if (pmem_alloc_lo(PAGE_SIZE, &space->pml4_memory) != PMEM_SUCCESS) {
		hal_free(space);
		return NULL;
	}
	space->pml4 = space->pml4_memory.vaddr;
	hal_memset(space->pml4, 0, PAGE_SIZE);
	space->pml4[511] = system_pml4[511];
	space->magic = AMD64_SPACE_MAGIC;
	space->space_id = next_space_id++;
	space_count++;
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	struct amd64_space *space = handle;
	struct amd64_table_page *page;
	if (space == NULL) return;
	if (!valid_space(space)) HAL_FATAL("invalid amd64 space destroy");
	if (current_space == space) hal_page_switch_space(HAL_SPACE_SYS);
	while ((page = space->tables) != NULL) {
		space->tables = page->next;
		(void)pmem_free(&page->memory);
		hal_free(page);
		if (page_table_count != 0) page_table_count--;
	}
	space->magic = 0;
	(void)pmem_free(&space->pml4_memory);
	hal_free(space);
	if (space_count != 0) space_count--;
}

void
hal_page_switch_space(hal_space_t handle)
{
	uintptr_t cr3;
	if (!valid_space(handle)) HAL_FATAL("invalid amd64 space switch");
	if (handle == current_space) return;
	cr3 = handle == HAL_SPACE_SYS ? system_cr3 :
	    (uintptr_t)((struct amd64_space *)handle)->pml4_memory.paddr;
	asm_load_cr3(cr3);
	current_space = handle;
}

static uint64 leaf_flags(uint32 attr)
{
	uint64 flags = AMD64_PTE_PRESENT | AMD64_PTE_USER;
	if (attr & HAL_SPACE_WRITE) flags |= AMD64_PTE_WRITE;
	if (attr & HAL_SPACE_NOCACHE) flags |= AMD64_PTE_NOCACHE;
	if (attr & HAL_SPACE_WRITETHRU) flags |= AMD64_PTE_WRITETHRU;
	if (attr & HAL_SPACE_DEVICE) flags |= AMD64_PTE_NOCACHE;
	if (!(attr & HAL_SPACE_EXEC)) flags |= AMD64_PTE_NX;
	return flags;
}

int
hal_page_map(hal_space_t handle, void *pointer, uintptr_t physical,
	size_t size, uint32 attr)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size) ||
	    (physical & (PAGE_SIZE - 1U)) != 0 ||
	    physical >= AMD64_DIRECT_LIMIT || size > AMD64_DIRECT_LIMIT - physical ||
	    !(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_PMEM_BADDESC;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & AMD64_PTE_PRESENT))
			return HAL_PMEM_BADDESC;
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			(void)hal_page_unmap(space, pointer, offset);
			reclaim_empty_tables(space);
			return HAL_PMEM_NOSPACE;
		}
		*leaf = (physical + offset) | leaf_flags(attr);
	}
	if (current_space == space) hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

int
hal_page_prot(hal_space_t handle, void *pointer, size_t size, uint32 attr)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size) ||
	    !(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_PMEM_BADDESC;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 0);
		uint64 physical;
		if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT))
			return HAL_PMEM_BADDESC;
		physical = *leaf & AMD64_PTE_ADDR_MASK;
		*leaf = physical | leaf_flags(attr);
	}
	if (current_space == space) hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

int
hal_page_unmap(hal_space_t handle, void *pointer, size_t size)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	if (size == 0) return HAL_PMEM_SUCCESS;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range(address, size)) return HAL_PMEM_BADDESC;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64 *leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL) *leaf = 0;
	}
	reclaim_empty_tables(space);
	if (current_space == space) hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

int
hal_page_query(hal_space_t handle, void *pointer, uint32 *flags)
{
	struct amd64_space *space = handle;
	uint64 *leaf;
	if (space == NULL || !valid_space(space) || flags == NULL ||
	    !valid_user_range((uintptr_t)pointer, PAGE_SIZE))
		return HAL_PMEM_BADDESC;
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);
	*flags = leaf != NULL && (*leaf & AMD64_PTE_PRESENT) ? HAL_PAGE_PRESENT : 0;
	if (leaf != NULL && (*leaf & AMD64_PTE_ACCESSED)) *flags |= HAL_PAGE_ACCESSED;
	if (leaf != NULL && (*leaf & AMD64_PTE_DIRTY)) *flags |= HAL_PAGE_DIRTY;
	return HAL_PMEM_SUCCESS;
}

int
hal_page_clear_flags(hal_space_t handle, void *pointer, uint32 flags)
{
	struct amd64_space *space = handle;
	uint64 *leaf, mask = 0;
	if (space == NULL || !valid_space(space) ||
	    !valid_user_range((uintptr_t)pointer, PAGE_SIZE) ||
	    (flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_PMEM_BADDESC;
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);
	if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT))
		return HAL_PMEM_BADDESC;
	if (flags & HAL_PAGE_ACCESSED) mask |= AMD64_PTE_ACCESSED;
	if (flags & HAL_PAGE_DIRTY) mask |= AMD64_PTE_DIRTY;
	*leaf &= ~mask;
	if (current_space == space) hal_page_flush_tlb(space);
	return HAL_PMEM_SUCCESS;
}

void hal_page_flush_tlb(hal_space_t handle)
{
	if (handle == HAL_SPACE_SYS || handle == current_space) asm_flush_tlb();
}

size_t hal_page_get_page_size(int level)
{
	if (level == 1) return PAGE_SIZE;
	if (level == 2) return 0x200000U;
	return 0;
}

void hal_page_get_user_range(uintptr_t *minimum, uintptr_t *limit)
{
	if (minimum != NULL) *minimum = PAGE_SIZE;
	if (limit != NULL) *limit = 0x0000800000000000ULL;
}

void hal_amd64_space_memory_stats(uint32 *spaces, uint32 *tables)
{
	if (spaces != NULL) *spaces = space_count;
	if (tables != NULL) *tables = page_table_count;
}
