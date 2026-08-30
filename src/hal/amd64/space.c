/* amd64 four-level page-table implementation. */
#include <hal/hal.h>
#include "defs.h"
#include "asm.h"
#include "percpu.h"
#include "space.h"
#include "smp.h"
#include "acpi-window.h"
#include "bsp.h"
#include "bsp-pcat/lapic.h"
#include "bootloader/include/amd64-handoff.h"

#define AMD64_USER_LIMIT 0x0000800000000000ULL
_Static_assert(AMD64_USER_LIMIT - 1U <= (uintptr_t)INTPTR_MAX,
    "user pointers must not overlap the negative syscall errno window");

static uint64_t system_pml4[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t system_pdpt[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t system_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t system_kernel_pt[8][512]
	__attribute__((aligned(PAGE_SIZE)));
static uint64_t system_mmio_pd[512] __attribute__((aligned(PAGE_SIZE)));
#define AMD64_ACPI_PDPT_INDEX 509U
#define AMD64_ACPI_WINDOW_BASE 0xffffffff40000000ULL
static uint64_t system_acpi_pd[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t
    system_acpi_pt[AMD64_ACPI_WINDOW_PT_COUNT][512]
	__attribute__((aligned(PAGE_SIZE)));
static struct amd64_acpi_window acpi_window;
static paddr_t acpi_physical_max;
#define AMD64_ECAM_PD_FIRST 128U
#define AMD64_ECAM_PD_COUNT 128U
#define AMD64_FRAMEBUFFER_PD_FIRST 16U
#define AMD64_FRAMEBUFFER_PD_COUNT \
	(AMD64_ECAM_PD_FIRST - AMD64_FRAMEBUFFER_PD_FIRST)
#define AMD64_ECAM_VIRTUAL_BASE 0xffffffffd0000000ULL
static unsigned ecam_pd_used;
static uintptr_t system_cr3;
#define AMD64_CURRENT_SPACE (amd64_percpu_current()->current_space)
static int next_space_id = 1;
static uint32_t space_count;
static uint32_t page_table_count;
static volatile unsigned space_registry_lock;
static struct amd64_space *space_registry;

struct amd64_shootdown_request {
	volatile unsigned active;
	hal_space_t space;
	void *vaddr;
	size_t size;
	volatile uint64_t pending;
};
#define AMD64_SHOOTDOWN_REQUESTS (AMD64_SMP_MAX_CPUS * 4U)
static struct amd64_shootdown_request shootdowns[AMD64_SHOOTDOWN_REQUESTS];

static void shootdown(hal_space_t handle, void *vaddr, size_t size);
static void service_shootdowns(hal_cpu_id_t cpu);

static paddr_t
cpu_physical_max(void)
{
	uint32_t eax = 0x80000000U, ebx, ecx, edx;
	unsigned bits = 36U;

	__asm__ volatile("cpuid"
	    : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	if (eax >= 0x80000008U) {
		eax = 0x80000008U;
		__asm__ volatile("cpuid"
		    : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
		bits = eax & 0xffU;
	}
	if (bits < 32U || bits > 52U)
		bits = 36U;
	return ((paddr_t)1U << bits) - 1U;
}

static void
table_count_drop(void)
{
	uint32_t old = __atomic_fetch_sub(&page_table_count, 1U,
	    __ATOMIC_RELAXED);
	if (old == 0)
		HAL_FATAL("amd64 page-table counter underflow");
}

static int
alloc_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, PAGE_SIZE, PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
}

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

uintptr_t amd64_system_cr3(void) { return system_cr3; }

void
amd64_space_init(void)
{
	const struct zbl6_framebuffer *framebuffer =
	    hal_get_arch_handoff("pcat.framebuffer");
	uintptr_t kernel_start = (uintptr_t)__kernel_phys_start;
	uintptr_t kernel_end = (uintptr_t)__kernel_phys_end;
	unsigned index, first_chunk, chunks, chunk;
	uint64_t efer;
	uintptr_t cr0, cr4;

	space_registry_lock = 0;
	space_registry = NULL;

	hal_memset(system_pml4, 0, sizeof(system_pml4));
	hal_memset(system_pdpt, 0, sizeof(system_pdpt));
	hal_memset(system_pd, 0, sizeof(system_pd));
	hal_memset(system_kernel_pt, 0, sizeof(system_kernel_pt));
	hal_memset(system_mmio_pd, 0, sizeof(system_mmio_pd));
	hal_memset(system_acpi_pd, 0, sizeof(system_acpi_pd));
	hal_memset(system_acpi_pt, 0, sizeof(system_acpi_pt));
	amd64_acpi_window_init(&acpi_window);
	acpi_physical_max = cpu_physical_max();
	ecam_pd_used = 0;
	for (index = 0; index < 512; index++)
		system_pd[index] = (uint64_t)index * 0x200000ULL |
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
			uint64_t flags = AMD64_PTE_PRESENT | AMD64_PTE_GLOBAL |
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
		    (uint64_t)index * 0x200000ULL) | AMD64_PTE_PRESENT |
		    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	/* Dedicated uncached windows for the Local APIC and I/O APIC. */
	system_mmio_pd[8] = 0xfee00000ULL | AMD64_PTE_PRESENT |
	    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
	    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	system_mmio_pd[9] = 0xfec00000ULL | AMD64_PTE_PRESENT |
	    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
	    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	if (framebuffer != NULL) {
		uint64_t base = framebuffer->physical_base & ~0x1fffffULL;
		uint64_t end = framebuffer->physical_base + framebuffer->size;
		unsigned count = (unsigned)((end - base + 0x1fffffULL) /
		    0x200000ULL);
		if (count == 0 || count > AMD64_FRAMEBUFFER_PD_COUNT)
			HAL_FATAL("amd64 framebuffer MMIO window exceeded");
		for (index = 0; index < count; index++)
			system_mmio_pd[AMD64_FRAMEBUFFER_PD_FIRST + index] = (base +
			    (uint64_t)index * 0x200000ULL) |
			    AMD64_PTE_PRESENT | AMD64_PTE_WRITE |
			    AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
			    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	}
	/* APs enter long mode at the low trampoline before jumping high. */
	system_pd[0] &= ~AMD64_PTE_NX;
	system_pdpt[0] = amd64_direct_to_phys(system_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	for (index = 0; index < AMD64_ACPI_WINDOW_PT_COUNT; index++)
		system_acpi_pd[index] =
		    amd64_direct_to_phys(system_acpi_pt[index]) |
		    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[AMD64_ACPI_PDPT_INDEX] =
	    amd64_direct_to_phys(system_acpi_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[510] = amd64_direct_to_phys(system_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pdpt[511] = amd64_direct_to_phys(system_mmio_pd) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pml4[511] = amd64_direct_to_phys(system_pdpt) |
	    AMD64_PTE_PRESENT | AMD64_PTE_WRITE;
	system_pml4[0] = amd64_direct_to_phys(system_pdpt) |
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
	__atomic_store_n(&AMD64_CURRENT_SPACE, HAL_SPACE_SYS, __ATOMIC_RELEASE);
}

const void *
amd64_acpi_map_physical(paddr_t physical, size_t size)
{
	unsigned first, new_first, new_count, index;
	paddr_t page_physical;
	size_t offset, page_span;

	page_physical = physical & ~(paddr_t)(PAGE_SIZE - 1U);
	offset = (size_t)(physical - page_physical);
	if (size == 0 || size > SIZE_MAX - offset ||
	    size + offset >
		(size_t)AMD64_ACPI_WINDOW_SLOTS * PAGE_SIZE)
		return NULL;
	page_span = size + offset;
	if (page_span > SIZE_MAX - (PAGE_SIZE - 1U))
		return NULL;
	page_span = (page_span + PAGE_SIZE - 1U) & ~(size_t)(PAGE_SIZE - 1U);
	if (page_physical > acpi_physical_max ||
	    page_span - 1U > acpi_physical_max - page_physical ||
	    !bsp_physical_range_mappable(page_physical, page_span) ||
	    !amd64_acpi_window_reserve(&acpi_window, physical, size, &first,
				       &offset, &new_first, &new_count))
		return NULL;
	for (index = 0; index < new_count; index++) {
		unsigned slot = new_first + index;
		paddr_t page = acpi_window.slot_physical[slot];

		system_acpi_pt[slot / 512U][slot % 512U] =
		    (uint64_t)page | AMD64_PTE_PRESENT | AMD64_PTE_NX;
	}
	if (new_count != 0) {
		__atomic_thread_fence(__ATOMIC_RELEASE);
		asm_flush_tlb();
	}
	return (const void *)(uintptr_t)(AMD64_ACPI_WINDOW_BASE +
		(uint64_t)first * PAGE_SIZE + offset);
}

int
amd64_mmio_map_ecam(paddr_t physical, size_t size, void **result)
{
	const uint64_t page_size = 0x200000ULL;
	uint64_t aligned, end, offset;
	unsigned count, index, first;
	if (result == NULL || size == 0 || physical > UINT64_MAX - size)
		return HAL_ERR_INVALID;
	aligned = (uint64_t)physical & ~(page_size - 1U);
	offset = (uint64_t)physical - aligned;
	end = offset + size;
	count = (unsigned)((end + page_size - 1U) / page_size);
	if (count == 0 || count > AMD64_ECAM_PD_COUNT - ecam_pd_used)
		return HAL_ERR_UNSUPPORTED;
	first = AMD64_ECAM_PD_FIRST + ecam_pd_used;
	for (index = 0; index < count; index++)
		system_mmio_pd[first + index] = (aligned +
		    (uint64_t)index * page_size) | AMD64_PTE_PRESENT |
		    AMD64_PTE_WRITE | AMD64_PTE_NOCACHE | AMD64_PTE_LARGE |
		    AMD64_PTE_GLOBAL | AMD64_PTE_NX;
	ecam_pd_used += count;
	*result = (void *)(uintptr_t)(AMD64_ECAM_VIRTUAL_BASE +
	    (uint64_t)(first - AMD64_ECAM_PD_FIRST) * page_size + offset);
	return HAL_OK;
}

static bool
registry_lock_enter(void)
{
	bool enabled = hal_irq_disable();

	while (__atomic_exchange_n(&space_registry_lock, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	return enabled;
}

static void
registry_lock_leave(bool enabled)
{
	__atomic_store_n(&space_registry_lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

/* Register an operation before touching a space which may be retired. */
static int
space_op_enter(struct amd64_space *space)
{
	struct amd64_space *item;
	bool enabled = registry_lock_enter();

	for (item = space_registry; item != NULL; item = item->registry_next)
		if (item == space)
			break;
	if (item == NULL || item->destroying) {
		registry_lock_leave(enabled);
		return 0;
	}
	item->active_ops++;
	registry_lock_leave(enabled);
	return 1;
}

static void
space_op_leave(struct amd64_space *space)
{
	bool enabled = registry_lock_enter();

	if (space->active_ops == 0)
		HAL_FATAL("amd64 space operation counter underflow");
	space->active_ops--;
	registry_lock_leave(enabled);
}

static bool
space_lock_enter(struct amd64_space *space)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&space->lock, 1U, __ATOMIC_ACQUIRE) != 0) {
		/* A target spinning with IRQs masked must still acknowledge a
		 * shootdown issued by the CPU which owns this serializer. */
		service_shootdowns(hal_cpu_current());
		__asm__ volatile("pause");
	}
	return enabled;
}

static void
space_lock_leave(struct amd64_space *space, bool enabled)
{
	__atomic_store_n(&space->lock, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
}

static int valid_user_range(uintptr_t address, size_t size)
{
	return size != 0 && (address & (PAGE_SIZE - 1U)) == 0 &&
	    (size & (PAGE_SIZE - 1U)) == 0 && address >= PAGE_SIZE &&
	    address < 0x0000800000000000ULL &&
	    size <= 0x0000800000000000ULL - address;
}

static struct amd64_table_page *
allocate_table(struct amd64_space *space, uint64_t *parent,
	unsigned parent_index)
{
	struct amd64_table_page *page = hal_malloc(sizeof(*page));
	if (page == NULL) return NULL;
	if (alloc_page(&page->memory) != HAL_OK) {
		hal_free(page);
		return NULL;
	}
	hal_memset(page->memory.vaddr, 0, PAGE_SIZE);
	page->parent = parent;
	page->parent_index = parent_index;
	page->next = space->tables;
	space->tables = page;
	(void)__atomic_fetch_add(&page_table_count, 1U, __ATOMIC_RELAXED);
	return page;
}

static uint64_t *
walk_leaf(struct amd64_space *space, uintptr_t address, int create)
{
	unsigned shifts[3] = { 39, 30, 21 };
	uint64_t *table = space->pml4;
	unsigned level;

	for (level = 0; level < 3; level++) {
		unsigned index = (unsigned)(address >> shifts[level]) & 511U;
		uint64_t entry = table[index];
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
table_is_empty(const uint64_t *table)
{
	unsigned index;
	for (index = 0; index < 512; index++)
		if (table[index] & AMD64_PTE_PRESENT)
			return 0;
	return 1;
}

/*
 * Remove every empty table from the hardware tree without releasing its
 * storage.  A remote CPU can begin a new page walk as soon as it acknowledges
 * a leaf shootdown, so parent entries must be disconnected before that
 * acknowledgement.  The returned pages remain valid until the caller has
 * completed a full shootdown.
 */
static struct amd64_table_page *
detach_empty_tables(struct amd64_space *space)
{
	struct amd64_table_page *detached = NULL;
	int reclaimed;

	do {
		struct amd64_table_page **link = &space->tables;
		reclaimed = 0;
		while (*link != NULL) {
			struct amd64_table_page *page = *link;
			uint64_t expected = (uintptr_t)page->memory.paddr;
			uint64_t parent_entry;

			if (!table_is_empty(page->memory.vaddr)) {
				link = &page->next;
				continue;
			}
			parent_entry = page->parent[page->parent_index];
			if (!(parent_entry & AMD64_PTE_PRESENT) ||
			    (parent_entry & AMD64_PTE_ADDR_MASK) != expected)
				HAL_FATAL("detaching an unlinked amd64 page table");
			page->parent[page->parent_index] = 0;
			*link = page->next;
			page->next = detached;
			detached = page;
			reclaimed = 1;
		}
	} while (reclaimed);
	return detached;
}

static void
free_detached_tables(struct amd64_table_page *page)
{
	while (page != NULL) {
		struct amd64_table_page *next = page->next;

		(void)hal_pmem_free(&page->memory);
		hal_free(page);
		table_count_drop();
		page = next;
	}
}

hal_space_t
hal_mem_create_space(void)
{
	struct amd64_space *space = hal_malloc(sizeof(*space));
	bool enabled;
	if (space == NULL) return NULL;
	hal_memset(space, 0, sizeof(*space));
	if (alloc_page(&space->pml4_memory) != HAL_OK) {
		hal_free(space);
		return NULL;
	}
	space->pml4 = space->pml4_memory.vaddr;
	hal_memset(space->pml4, 0, PAGE_SIZE);
	space->pml4[511] = system_pml4[511];
	space->magic = AMD64_SPACE_MAGIC;
	space->space_id = __atomic_fetch_add(&next_space_id, 1,
	    __ATOMIC_RELAXED);
	(void)__atomic_fetch_add(&space_count, 1U, __ATOMIC_RELAXED);
	enabled = registry_lock_enter();
	space->registry_next = space_registry;
	space_registry = space;
	registry_lock_leave(enabled);
	return space;
}

void
hal_page_destroy_space(hal_space_t handle)
{
	struct amd64_space *space = handle;
	struct amd64_space **link;
	struct amd64_table_page *page;
	struct hal_cpu_mask ready;
	hal_cpu_id_t cpu;
	bool enabled;
	if (space == NULL) return;
	enabled = registry_lock_enter();
	for (link = &space_registry; *link != NULL && *link != space;
	    link = &(*link)->registry_next)
		;
	if (*link == NULL || space->destroying)
		HAL_FATAL("invalid amd64 space destroy");
	space->destroying = 1U;
	*link = space->registry_next;
	registry_lock_leave(enabled);

	/* Operations admitted before retirement own the storage until they leave. */
	for (;;) {
		unsigned active;

		enabled = registry_lock_enter();
		active = space->active_ops;
		registry_lock_leave(enabled);
		if (active == 0)
			break;
		service_shootdowns(hal_cpu_current());
		__asm__ volatile("pause");
	}
	shootdown(space, NULL, 0);
	/*
	 * Address-space ownership is enforced by the generic kernel.  The HAL
	 * closes hardware translation windows but must never detach a task from
	 * its space implicitly.  Catch a violated lifetime invariant before the
	 * page-table storage is released.
	 */
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		struct amd64_percpu *target;

		if (!hal_cpu_mask_test(&ready, cpu))
			continue;
		target = amd64_percpu_get(cpu);
		if (__atomic_load_n(&target->current_space, __ATOMIC_ACQUIRE) == space)
			HAL_FATAL("destroying an active amd64 space");
	}
	while ((page = space->tables) != NULL) {
		space->tables = page->next;
		(void)hal_pmem_free(&page->memory);
		hal_free(page);
		table_count_drop();
	}
	space->magic = 0;
	(void)hal_pmem_free(&space->pml4_memory);
	hal_free(space);
	if (__atomic_fetch_sub(&space_count, 1U, __ATOMIC_RELAXED) == 0)
		HAL_FATAL("amd64 space counter underflow");
}

void
hal_page_switch_space(hal_space_t handle)
{
	struct amd64_space *space;
	uintptr_t cr3;
	bool enabled;
	if (__atomic_load_n(&AMD64_CURRENT_SPACE, __ATOMIC_ACQUIRE) == handle)
		return;
	if (handle == HAL_SPACE_SYS) {
		enabled = hal_irq_disable();
		asm_load_cr3(system_cr3);
		__atomic_store_n(&AMD64_CURRENT_SPACE, handle, __ATOMIC_RELEASE);
		if (enabled)
			hal_irq_enable();
		return;
	}
	space = handle;
	if (!space_op_enter(space))
		HAL_FATAL("invalid amd64 space switch");
	enabled = space_lock_enter(space);
	cr3 = (uintptr_t)space->pml4_memory.paddr;
	asm_load_cr3(cr3);
	__atomic_store_n(&AMD64_CURRENT_SPACE, handle, __ATOMIC_RELEASE);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

static uint64_t leaf_flags(uint32_t attr)
{
	uint64_t flags = AMD64_PTE_PRESENT | AMD64_PTE_USER;
	if (attr & HAL_SPACE_WRITE) flags |= AMD64_PTE_WRITE;
	if (attr & HAL_SPACE_NOCACHE) flags |= AMD64_PTE_NOCACHE;
	if (attr & HAL_SPACE_WRITETHRU) flags |= AMD64_PTE_WRITETHRU;
	if (attr & HAL_SPACE_DEVICE) flags |= AMD64_PTE_NOCACHE;
	if (!(attr & HAL_SPACE_EXEC)) flags |= AMD64_PTE_NX;
	return flags;
}

int
hal_page_map(hal_space_t handle, void *pointer, hal_physaddr_t physical,
	size_t size, uint32_t attr)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	bool enabled;
	if (space == NULL ||
	    !valid_user_range(address, size) ||
	    (physical & (PAGE_SIZE - 1U)) != 0 ||
	    physical >= AMD64_DIRECT_LIMIT || size > AMD64_DIRECT_LIMIT - physical ||
	    !(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64_t *leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL && (*leaf & AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64_t *leaf = walk_leaf(space, address + offset, 1);
		if (leaf == NULL) {
			struct amd64_table_page *detached;
			uintptr_t rollback;

			for (rollback = 0; rollback < offset; rollback += PAGE_SIZE) {
				leaf = walk_leaf(space, address + rollback, 0);
				if (leaf != NULL) *leaf = 0;
			}
			detached = detach_empty_tables(space);
			if (detached != NULL)
				shootdown(space, NULL, 0);
			else if (offset != 0)
				shootdown(space, pointer, offset);
			free_detached_tables(detached);
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_NOMEM;
		}
		*leaf = (physical + offset) | leaf_flags(attr);
	}
	/* A different CPU may currently execute in this address space. */
	shootdown(space, pointer, size);
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

int
hal_page_prot(hal_space_t handle, void *pointer, size_t size, uint32_t attr)
{
	return hal_page_prot_query(handle, pointer, size, attr, NULL);
}

int
hal_page_prot_query(hal_space_t handle, void *pointer, size_t size,
	uint32_t attr, uint32_t *flags)
{
	struct amd64_space *space = handle;
	uintptr_t address = (uintptr_t)pointer, offset;
	uint32_t observed = 0;
	bool enabled;
	if (space == NULL ||
	    !valid_user_range(address, size) ||
	    !(attr & (HAL_SPACE_READ | HAL_SPACE_WRITE | HAL_SPACE_EXEC)))
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	/* Validate the complete range before publishing any permission change. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64_t *leaf = walk_leaf(space, address + offset, 0);
		if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_INVALID;
		}
	}
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64_t *leaf = walk_leaf(space, address + offset, 0);
		uint64_t old = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
		uint64_t desired;

		do {
			desired = (old & AMD64_PTE_ADDR_MASK) | leaf_flags(attr) |
			    (old & (AMD64_PTE_ACCESSED | AMD64_PTE_DIRTY));
		} while (!__atomic_compare_exchange_n(leaf, &old, desired, false,
		    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
		if (old & AMD64_PTE_ACCESSED)
			observed |= HAL_PAGE_ACCESSED;
		if (old & AMD64_PTE_DIRTY)
			observed |= HAL_PAGE_DIRTY;
	}
	shootdown(space, pointer, size);

	/* A remote CPU may have set A/D through its old translation before it
	 * acknowledged the shootdown.  The replacement is now stable with
	 * respect to those old translations. */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64_t *leaf = walk_leaf(space, address + offset, 0);
		uint64_t entry;

		if (leaf == NULL ||
		    !((entry = __atomic_load_n(leaf, __ATOMIC_ACQUIRE)) &
		    AMD64_PTE_PRESENT)) {
			space_lock_leave(space, enabled);
			space_op_leave(space);
			return HAL_ERR_STATE;
		}
		observed |= HAL_PAGE_PRESENT;
		if (entry & AMD64_PTE_ACCESSED)
			observed |= HAL_PAGE_ACCESSED;
		if (entry & AMD64_PTE_DIRTY)
			observed |= HAL_PAGE_DIRTY;
	}
	space_lock_leave(space, enabled);
	space_op_leave(space);
	if (flags != NULL)
		*flags = observed;
	return HAL_OK;
}

int
hal_page_unmap(hal_space_t handle, void *pointer, size_t size)
{
	struct amd64_space *space = handle;
	struct amd64_table_page *detached;
	uintptr_t address = (uintptr_t)pointer, offset;
	bool enabled;
	if (size == 0) return HAL_OK;
	if (space == NULL ||
	    !valid_user_range(address, size)) return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		uint64_t *leaf = walk_leaf(space, address + offset, 0);
		if (leaf != NULL) *leaf = 0;
	}
	/*
	 * Disconnect empty tables before the shootdown, but keep their storage
	 * alive through acknowledgement.  This closes both stale translations
	 * and page walks which began through the old parent entries.
	 */
	detached = detach_empty_tables(space);
	if (detached != NULL)
		shootdown(space, NULL, 0);
	else
		shootdown(space, pointer, size);
	free_detached_tables(detached);
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

int
hal_page_query(hal_space_t handle, void *pointer, uint32_t *flags)
{
	struct amd64_space *space = handle;
	uint64_t *leaf;
	bool enabled;
	if (space == NULL || flags == NULL ||
	    !valid_user_range((uintptr_t)pointer, PAGE_SIZE))
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);
	*flags = leaf != NULL && (*leaf & AMD64_PTE_PRESENT) ? HAL_PAGE_PRESENT : 0;
	if (leaf != NULL && (*leaf & AMD64_PTE_ACCESSED)) *flags |= HAL_PAGE_ACCESSED;
	if (leaf != NULL && (*leaf & AMD64_PTE_DIRTY)) *flags |= HAL_PAGE_DIRTY;
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

int
hal_page_clear_flags(hal_space_t handle, void *pointer, uint32_t flags)
{
	struct amd64_space *space = handle;
	uint64_t *leaf, mask = 0, old, desired;
	bool enabled;
	if (space == NULL ||
	    !valid_user_range((uintptr_t)pointer, PAGE_SIZE) ||
	    (flags & ~(HAL_PAGE_ACCESSED | HAL_PAGE_DIRTY)) != 0)
		return HAL_ERR_INVALID;
	if (!space_op_enter(space))
		return HAL_ERR_STATE;
	enabled = space_lock_enter(space);
	leaf = walk_leaf(space, (uintptr_t)pointer, 0);
	if (leaf == NULL || !(*leaf & AMD64_PTE_PRESENT)) {
		space_lock_leave(space, enabled);
		space_op_leave(space);
		return HAL_ERR_INVALID;
	}
	if (flags & HAL_PAGE_ACCESSED) mask |= AMD64_PTE_ACCESSED;
	if (flags & HAL_PAGE_DIRTY) mask |= AMD64_PTE_DIRTY;
	old = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
	do {
		desired = old & ~mask;
	} while (!__atomic_compare_exchange_n(leaf, &old, desired, false,
	    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	shootdown(space, pointer, PAGE_SIZE);
	space_lock_leave(space, enabled);
	space_op_leave(space);
	return HAL_OK;
}

static void
shootdown(hal_space_t handle, void *vaddr, size_t size)
{
	hal_cpu_id_t sender = hal_cpu_current(), cpu;
	struct amd64_shootdown_request *request = NULL;
	struct hal_cpu_mask ready;
	unsigned slot;
	uint64_t pending = 0;

	/*
	 * A timer interrupt may preempt a thread that is waiting here and run a
	 * second thread on the same CPU.  Requests therefore cannot be indexed
	 * by sender CPU.  Reserve a pool entry before publishing its fields.
	 */
	for (slot = 0; slot < AMD64_SHOOTDOWN_REQUESTS; slot++) {
		unsigned expected = 0;
		if (__atomic_compare_exchange_n(&shootdowns[slot].active,
		    &expected, 2U, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			request = &shootdowns[slot];
			break;
		}
	}
	if (request == NULL)
		HAL_FATAL("amd64 TLB shootdown request pool exhausted");
	hal_cpu_ready_mask(&ready);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++) {
		struct amd64_percpu *target;
		if (cpu == sender || cpu >= AMD64_SMP_MAX_CPUS ||
		    !hal_cpu_mask_test(&ready, cpu))
			continue;
		target = amd64_percpu_get(cpu);
		if (handle == HAL_SPACE_SYS ||
		    __atomic_load_n(&target->current_space, __ATOMIC_ACQUIRE) ==
		    handle)
			pending |= (uint64_t)1U << cpu;
	}
	request->space = handle;
	request->vaddr = vaddr;
	request->size = size;
	__atomic_store_n(&request->pending, pending, __ATOMIC_RELEASE);
	__atomic_store_n(&request->active, 1U, __ATOMIC_RELEASE);
	for (cpu = 0; cpu < hal_cpu_count(); cpu++)
		if ((pending & ((uint64_t)1U << cpu)) != 0 &&
		    amd64_lapic_send_vector(amd64_smp_apic_id(cpu),
		    AMD64_VECTOR_TLB) != HAL_OK)
			HAL_FATAL("amd64 TLB shootdown delivery failed");
	if (handle == HAL_SPACE_SYS || AMD64_CURRENT_SPACE == handle)
		asm_flush_tlb();
	while (__atomic_load_n(&request->pending, __ATOMIC_ACQUIRE) != 0) {
		/*
		 * Page operations are legal with local interrupts disabled.  Polling
		 * incoming requests here breaks the reciprocal-shootdown deadlock
		 * without enabling arbitrary interrupt handlers inside a caller's
		 * critical section.
		 */
		service_shootdowns(sender);
		__asm__ volatile("pause");
	}
	__atomic_store_n(&request->active, 0U, __ATOMIC_RELEASE);
}

static void
service_shootdowns(hal_cpu_id_t cpu)
{
	uint64_t bit = (uint64_t)1U << cpu;
	unsigned slot;

	for (slot = 0; slot < AMD64_SHOOTDOWN_REQUESTS; slot++) {
		struct amd64_shootdown_request *request = &shootdowns[slot];
		if (__atomic_load_n(&request->active, __ATOMIC_ACQUIRE) == 1U &&
		    (__atomic_load_n(&request->pending, __ATOMIC_ACQUIRE) & bit) != 0) {
			asm_flush_tlb();
			(void)__atomic_fetch_and(&request->pending, ~bit,
			    __ATOMIC_RELEASE);
		}
	}
}

void
amd64_tlb_interrupt(void)
{
	hal_irq_ack_t acknowledge = amd64_irq_ack_begin(AMD64_VECTOR_TLB, -1);
	service_shootdowns(hal_cpu_current());
	hal_irq_send_eoi(acknowledge);
}

void hal_page_flush_tlb(hal_space_t handle)
{
	struct amd64_space *space = handle;
	bool enabled;

	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, NULL, 0);
		return;
	}
	if (!space_op_enter(space))
		HAL_FATAL("invalid amd64 space flush");
	enabled = space_lock_enter(space);
	shootdown(space, NULL, 0);
	space_lock_leave(space, enabled);
	space_op_leave(space);
}

void hal_page_flush_tlb_range(hal_space_t handle, void *vaddr, size_t size)
{
	struct amd64_space *space = handle;
	bool enabled;

	if (size == 0)
		return;
	if (handle == HAL_SPACE_SYS) {
		shootdown(handle, vaddr, size);
		return;
	}
	if (!space_op_enter(space))
		HAL_FATAL("invalid amd64 space range flush");
	enabled = space_lock_enter(space);
	shootdown(space, vaddr, size);
	space_lock_leave(space, enabled);
	space_op_leave(space);
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
	if (limit != NULL) *limit = AMD64_USER_LIMIT;
}

void hal_amd64_space_memory_stats(uint32_t *spaces, uint32_t *tables)
{
	if (spaces != NULL)
		*spaces = __atomic_load_n(&space_count, __ATOMIC_RELAXED);
	if (tables != NULL)
		*tables = __atomic_load_n(&page_table_count, __ATOMIC_RELAXED);
}
