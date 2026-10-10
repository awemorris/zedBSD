#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "space.h"

#define ARM64_USER_LIMIT 0x0001000000000000ULL
_Static_assert(ARM64_USER_LIMIT - 1U <= (uintptr_t)INTPTR_MAX,
    "user pointers must not overlap the negative syscall errno window");

#define PTE_VALID (1ULL << 0)
#define PTE_TABLE (1ULL << 1)
#define PTE_ATTR(n) ((uint64_t)(n) << 2)
#define PTE_USER (1ULL << 6)
#define PTE_RO (1ULL << 7)
#define PTE_SH_INNER (3ULL << 8)
#define PTE_AF (1ULL << 10)
#define PTE_SW_DIRTY (1ULL << 55)
#define PTE_PXN (1ULL << 53)
#define PTE_UXN (1ULL << 54)
#define PTE_ADDR 0x0000fffffffff000ULL
#define BLOCK_FLAGS (PTE_VALID | PTE_AF | PTE_SH_INNER)
#define PAGE_FLAGS (PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER)

static uint64_t system_user_l0[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l0[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l1[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l2_low[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l3_low[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64_t system_kernel_l2_high[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uintptr_t system_ttbr0;
static hal_space_t current_space;
static int next_space_id=1;
static uint32_t space_count, page_table_count;
static struct arm64_space *space_registry;

/* The size of one level-2 block of the kernel's direct map. */
#define ARM64_DEVICE_BLOCK_SIZE 0x200000ULL

/* The physical span one level-1 entry of the direct map covers. */
#define ARM64_DIRECT_L1_SPAN 0x40000000ULL

/* The physical span the direct map's level-1 table can reach. */
#define ARM64_DIRECT_LIMIT (512ULL * ARM64_DIRECT_L1_SPAN)

/* The system half's level-0 entry that holds the uncached window. */
#define ARM64_UNCACHED_L0_INDEX 1U

/* Where the uncached window starts and how large it is (one level-0 entry). */
#define ARM64_UNCACHED_BASE (ARM64_DIRECT_BASE + ((uint64_t)ARM64_UNCACHED_L0_INDEX << 39))
#define ARM64_UNCACHED_SIZE (1ULL << 39)

/* The MAIR index locore.S gives Normal non-cacheable memory. */
#define ARM64_ATTR_NORMAL_NC 3U

/*
 * The first unused address of the uncached window.
 *
 * Views are handed out upward and their addresses are not reused: DMA
 * buffers are taken at attach and returned at detach, which never comes near
 * the window's 512 GiB.  Zero means the window is not yet in use.  Changed
 * only with interrupts disabled; this HAL runs on one CPU.
 */
static uint64_t uncached_next;

static bool range_touches_ram(uint64_t physical, uint64_t size);
static int uncached_leaf(uint64_t address, bool create, uint64_t **leaf);
static int uncached_table(uint64_t *table, unsigned index, bool create, uint64_t **next);
static bool range_is_ram(uint64_t physical, uint64_t size);
static int device_block_table(unsigned l1_index, uint64_t **table);
static void map_one_device_block(uint64_t *table, unsigned l2_index, uint64_t physical);

/* Allocates one zero-owner physical page for page-table use. */
static int alloc_page(hal_physaddr_t *memory)
{
	return hal_pmem_alloc(ARM64_PAGE_SIZE, ARM64_PAGE_SIZE, memory);
}
extern char __kernel_text_start[],__kernel_text_end[];
extern char __kernel_rodata_start[],__kernel_rodata_end[];
extern char __kernel_data_start[],__kernel_data_end[];

uintptr_t arm64_direct_to_phys(const void *p) { return (uintptr_t)p-ARM64_DIRECT_BASE; }
void *arm64_phys_to_direct(uintptr_t p) { return (void *)(ARM64_DIRECT_BASE+p); }

static uint64_t table_desc(const void *table)
{ return arm64_direct_to_phys(table)|PTE_VALID|PTE_TABLE; }

static void
map_device_block(uint64_t physical)
{
	unsigned index;
	if (physical < 0xc0000000ULL || physical >= 0x100000000ULL)
		return;
	index = (unsigned)((physical - 0xc0000000ULL) >> 21);
	system_kernel_l2_high[index] =
	    (physical & ~0x1fffffULL) | BLOCK_FLAGS | PTE_ATTR(1) |
	    PTE_PXN | PTE_UXN;
}

void
arm64_space_init(void)
{
	uint64_t total=hal_pmem_get_total_size(); unsigned i;
	uintptr_t text_start=arm64_direct_to_phys(__kernel_text_start);
	uintptr_t text_end=arm64_direct_to_phys(__kernel_text_end);
	uintptr_t rodata_start=arm64_direct_to_phys(__kernel_rodata_start);
	uintptr_t rodata_end=arm64_direct_to_phys(__kernel_rodata_end);
	uintptr_t data_start=arm64_direct_to_phys(__kernel_data_start);
	if(hal_cpu_count()!=1)HAL_FATAL("arm64 space implementation is UP-only");
	space_registry=NULL;
	hal_memset(system_user_l0,0,sizeof(system_user_l0));
	hal_memset(system_kernel_l0,0,sizeof(system_kernel_l0));
	hal_memset(system_kernel_l1,0,sizeof(system_kernel_l1));
	hal_memset(system_kernel_l2_low,0,sizeof(system_kernel_l2_low));
	hal_memset(system_kernel_l3_low,0,sizeof(system_kernel_l3_low));
	hal_memset(system_kernel_l2_high,0,sizeof(system_kernel_l2_high));
	system_kernel_l0[0]=table_desc(system_kernel_l1);
	for(i=0;i<512;i++) {
		uint64_t physical=(uint64_t)i<<30;
		if(physical>=total) break;
		system_kernel_l1[i]=physical|BLOCK_FLAGS|PTE_PXN|PTE_UXN;
	}
	/*
	 * Do not turn the complete 3--4 GiB aperture into Device memory: a
	 * 4 GiB Pi has ordinary RAM there.  Split that L1 entry and replace only
	 * the FDT-discovered peripheral blocks with Device-nGnRE mappings.
	 */
	system_kernel_l1[3]=table_desc(system_kernel_l2_high);
	for(i=0;i<512;i++) {
		uint64_t physical=0xc0000000ULL+((uint64_t)i<<21);
		if(physical<total)
			system_kernel_l2_high[i]=physical|BLOCK_FLAGS|PTE_PXN|PTE_UXN;
	}
	{
		const struct rpi4_fdt_info *info=rpi4_boot_info();
		map_device_block(info->uart_base);
		map_device_block(info->mailbox_base);
		map_device_block(info->gic_dist_base);
		map_device_block(info->gic_cpu_base);
		map_device_block(info->sdhci_base);
	}
	system_kernel_l1[0]=table_desc(system_kernel_l2_low);
	for(i=0;i<512;i++)
		system_kernel_l2_low[i]=((uint64_t)i<<21)|BLOCK_FLAGS|PTE_PXN|PTE_UXN;
	system_kernel_l2_low[0]=table_desc(system_kernel_l3_low);
	for(i=0;i<512;i++) {
		uint64_t p=(uint64_t)i*ARM64_PAGE_SIZE;
		uint64_t flags=PAGE_FLAGS|PTE_PXN|PTE_UXN;
		if(p>=text_start && p<text_end)
			flags=(flags|PTE_RO)&~PTE_PXN;
		else if(p>=rodata_start && p<rodata_end)
			flags|=PTE_RO;
		system_kernel_l3_low[i]=p|flags;
	}
	system_ttbr0=arm64_direct_to_phys(system_user_l0);
	arm64_write_ttbr1(arm64_direct_to_phys(system_kernel_l0));
	arm64_write_ttbr0(system_ttbr0);
	arm64_flush_tlb();
	current_space=HAL_SPACE_SYS;
	if ((system_kernel_l3_low[(text_start>>12)&511] & PTE_PXN) ||
	    !(system_kernel_l3_low[(rodata_start>>12)&511] & PTE_PXN) ||
	    !(system_kernel_l3_low[(data_start>>12)&511] & PTE_PXN))
		HAL_FATAL("arm64 kernel W^X table validation failed");
	hal_puts("ARM64 PAGING PASS\nARM64 W^X PASS\n");
}

static int
space_lock_handle(hal_space_t handle, struct arm64_space **result,
    bool *irq_enabled)
{
	struct arm64_space *space;
	bool enabled=hal_irq_disable();

	for(space=space_registry;space!=NULL;space=space->registry_next)
		if((hal_space_t)space==handle)break;
	if(space==NULL||space->destroying){if(enabled)hal_irq_enable();return 0;}
	if(space->lock!=0)HAL_FATAL("recursive arm64 space operation");
	space->lock=1U;*result=space;*irq_enabled=enabled;return 1;
}
static void
space_unlock(struct arm64_space *space,bool enabled)
{
	if(space->lock!=1U)HAL_FATAL("invalid arm64 space unlock");
	space->lock=0;if(enabled)hal_irq_enable();
}
static void
flush_locked(hal_space_t handle)
{
	if(handle==HAL_SPACE_SYS||handle==current_space)arm64_flush_tlb();
}
static int valid_user(uintptr_t a,size_t n)
{ return n&&(a&4095)==0&&(n&4095)==0&&a>=4096&&a<ARM64_USER_LIMIT&&n<=ARM64_USER_LIMIT-a; }

static struct arm64_table_page *allocate_table(struct arm64_space *s,uint64_t *parent,unsigned index)
{
	struct arm64_table_page *p=kernel_alloc(sizeof(*p));
	if(!p)return NULL;
	if(alloc_page(&p->memory)!=HAL_OK){kernel_free(p);return NULL;}
	hal_memset(hal_pmem_to_kernel(p->memory),0,ARM64_PAGE_SIZE);p->parent=parent;p->parent_index=index;
	p->next=s->tables;s->tables=p;page_table_count++;return p;
}
static uint64_t *walk_leaf(struct arm64_space *s,uintptr_t a,int create)
{
	static const unsigned shifts[3]={39,30,21};uint64_t *table=s->l0;unsigned level;
	for(level=0;level<3;level++){
		unsigned index=(unsigned)(a>>shifts[level])&511;uint64_t e=table[index];
		if(!(e&PTE_VALID)){struct arm64_table_page *p;if(!create)return NULL;
			p=allocate_table(s,table,index);if(!p)return NULL;e=(uintptr_t)p->memory|PTE_VALID|PTE_TABLE;table[index]=e;}
		if((e&(PTE_VALID|PTE_TABLE))!=(PTE_VALID|PTE_TABLE))return NULL;
		table=arm64_phys_to_direct((uintptr_t)(e&PTE_ADDR));
	}return &table[(a>>12)&511];
}
static int table_empty(const uint64_t *p){unsigned i;for(i=0;i<512;i++)if(p[i]&PTE_VALID)return 0;return 1;}
static struct arm64_table_page *detach_empty_tables(struct arm64_space *s)
{
	struct arm64_table_page *detached=NULL;int again;
	do{struct arm64_table_page **link=&s->tables;again=0;while(*link){struct arm64_table_page *p=*link;
			if(!table_empty(hal_pmem_to_kernel(p->memory))){link=&p->next;continue;}
			if(!(p->parent[p->parent_index]&PTE_VALID)||
			   (p->parent[p->parent_index]&PTE_ADDR)!=(uintptr_t)p->memory)
				HAL_FATAL("detaching an unlinked arm64 page table");
			p->parent[p->parent_index]=0;*link=p->next;p->next=detached;
			detached=p;again=1;}}while(again);
	return detached;
}
static void free_detached_tables(struct arm64_table_page *p)
{
	while(p){struct arm64_table_page *next=p->next;(void)hal_pmem_free(&p->memory,ARM64_PAGE_SIZE);
		kernel_free(p);if(page_table_count)page_table_count--;p=next;}
}
hal_space_t hal_space_create(void)
{
	struct arm64_space *s=kernel_alloc(sizeof(*s));bool enabled;if(!s)return NULL;hal_memset(s,0,sizeof(*s));
	if(alloc_page(&s->l0_memory)!=HAL_OK){kernel_free(s);return NULL;}
	s->l0=hal_pmem_to_kernel(s->l0_memory);hal_memset(s->l0,0,ARM64_PAGE_SIZE);s->magic=ARM64_SPACE_MAGIC;
	enabled=hal_irq_disable();s->space_id=next_space_id++;s->registry_next=space_registry;space_registry=s;space_count++;if(enabled)hal_irq_enable();return s;
}
void hal_space_destroy(hal_space_t h)
{
	struct arm64_space *s=h,**link;struct arm64_table_page *p;bool enabled;if(!s)return;
	enabled=hal_irq_disable();for(link=&space_registry;*link&&*link!=s;link=&(*link)->registry_next);
	if(*link==NULL||s->destroying)HAL_FATAL("invalid arm64 space destroy");
	if(current_space==s)HAL_FATAL("destroying an active arm64 space");
	if(s->lock!=0)HAL_FATAL("destroying a busy arm64 space");
	s->destroying=1U;*link=s->registry_next;
	while((p=s->tables)){s->tables=p->next;(void)hal_pmem_free(&p->memory,ARM64_PAGE_SIZE);kernel_free(p);if(page_table_count)page_table_count--;}
	s->magic=0;(void)hal_pmem_free(&s->l0_memory,ARM64_PAGE_SIZE);kernel_free(s);if(space_count)space_count--;if(enabled)hal_irq_enable();
}
void hal_space_switch(hal_space_t h)
{
	struct arm64_space *s;uintptr_t ttbr;bool enabled;if(h==current_space)return;
	if(h==HAL_SPACE_SYS){enabled=hal_irq_disable();arm64_write_ttbr0(system_ttbr0);arm64_flush_tlb();current_space=h;if(enabled)hal_irq_enable();return;}
	if(!space_lock_handle(h,&s,&enabled))HAL_FATAL("invalid arm64 space switch");
	ttbr=(uintptr_t)s->l0_memory;arm64_write_ttbr0(ttbr);arm64_flush_tlb();current_space=h;space_unlock(s,enabled);
}
/*
 * Makes code written through the data side visible to instruction fetch
 * before a user page becomes executable.  The Cortex-A72's instruction
 * cache does not snoop its data cache, so a page filled by ordinary stores
 * (a file read into memory, or code patched before mprotect) must be
 * cleaned to the point of unification and dropped from the instruction
 * cache, or the CPU fetches stale bytes.
 */
static void
sync_executable(
	hal_physaddr_t physical,
	size_t size,
	uint32_t attr)
{
	/* Only cacheable memory that is to be executed. */
	if ((attr & HAL_SPACE_EXEC) == 0)
		return;
	if ((attr & (HAL_SPACE_DEVICE | HAL_SPACE_NOCACHE)) != 0)
		return;

	/* Its bytes through the kernel's direct map. */
	hal_sync_instruction_stream(arm64_phys_to_direct((uintptr_t)physical), size);
}

static uint64_t leaf_flags(uint32_t attr)
{
	uint64_t f=PAGE_FLAGS|PTE_USER|PTE_PXN;
	if(!(attr&HAL_SPACE_WRITE))f|=PTE_RO;else f|=PTE_SW_DIRTY;
	if(!(attr&HAL_SPACE_EXEC))f|=PTE_UXN;
	if(attr&HAL_SPACE_DEVICE)f|=PTE_ATTR(1);else if(attr&HAL_SPACE_NOCACHE)f|=PTE_ATTR(2);
	return f;
}
int hal_space_map(hal_space_t h,void *v,hal_physaddr_t p,size_t n,uint32_t attr)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;bool enabled;
	if(!s||!valid_user(a,n)||(p&4095)||p>=hal_pmem_get_total_size()||n>hal_pmem_get_total_size()-p||
	   !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(l&&(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}}
	/* Code in the pages is fetched correctly once they are executable. */
	sync_executable(p,n,attr);
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,1);if(!l){struct arm64_table_page *detached;uintptr_t rollback;for(rollback=0;rollback<o;rollback+=4096){l=walk_leaf(s,a+rollback,0);if(l)*l=0;}detached=detach_empty_tables(s);hal_wmb();flush_locked(s);free_detached_tables(detached);space_unlock(s,enabled);return HAL_ERR_NOMEM;}*l=(p+o)|leaf_flags(attr);}
	flush_locked(s);space_unlock(s,enabled);return HAL_OK;
}
int hal_space_prot_query(hal_space_t h,void *v,size_t n,uint32_t attr,uint32_t *flags)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;uint32_t observed=0;bool enabled;if(!s||!valid_user(a,n)||
	 !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(!l||!(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}}
	/* Use break-before-make for the complete range.  This also covers callers
	 * which change the AttrIndx, not merely the permission bits. */
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),old=*l;observed|=HAL_SPACE_PAGE_PRESENT;if(old&PTE_AF)observed|=HAL_SPACE_PAGE_ACCESSED;
		/* This profile has no hardware dirty management.  Any writable
		 * translation is conservatively dirty, including stores made before
		 * the TLB invalidation completes. */
		if((old&PTE_RO)==0)old|=PTE_SW_DIRTY;
		if(old&PTE_SW_DIRTY)observed|=HAL_SPACE_PAGE_DIRTY;
		*l=old&~PTE_VALID;}
	hal_wmb();flush_locked(s);
	/* Pages that become executable get their code synchronized first. */
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);sync_executable((hal_physaddr_t)(*l&PTE_ADDR),4096,attr);}
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),old=*l;*l=(old&PTE_ADDR)|leaf_flags(attr)|(old&PTE_SW_DIRTY);}
	hal_wmb();flush_locked(s);
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),entry=*l;if(entry&PTE_AF)observed|=HAL_SPACE_PAGE_ACCESSED;if(entry&PTE_SW_DIRTY)observed|=HAL_SPACE_PAGE_DIRTY;}
	if(flags)*flags=observed;
	space_unlock(s,enabled);return HAL_OK;
}
int hal_space_prot(hal_space_t h,void *v,size_t n,uint32_t attr){return hal_space_prot_query(h,v,n,attr,NULL);}
int hal_space_unmap(hal_space_t h,void *v,size_t n)
{
	struct arm64_space *s=h;struct arm64_table_page *detached;uintptr_t a=(uintptr_t)v,o;bool enabled;if(!n)return HAL_OK;if(!s||!valid_user(a,n))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(l)*l=0;}
	detached=detach_empty_tables(s);hal_wmb();flush_locked(s);
	free_detached_tables(detached);space_unlock(s,enabled);return HAL_OK;
}
int hal_space_query(hal_space_t h,void *v,uint32_t *flags)
{
	struct arm64_space *s=h;uint64_t *l;bool enabled;if(!s||!flags||!valid_user((uintptr_t)v,4096))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	l=walk_leaf(s,(uintptr_t)v,0);*flags=l&&(*l&PTE_VALID)?HAL_SPACE_PAGE_PRESENT|HAL_SPACE_PAGE_ACCESSED:0;if(l&&(*l&PTE_SW_DIRTY))*flags|=HAL_SPACE_PAGE_DIRTY;space_unlock(s,enabled);return HAL_OK;
}
int hal_space_clear_flags(hal_space_t h,void *v,uint32_t flags)
{
	struct arm64_space *s=h;uint64_t *l;bool enabled;if(!s||!valid_user((uintptr_t)v,4096)||(flags&~(HAL_SPACE_PAGE_ACCESSED|HAL_SPACE_PAGE_DIRTY)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	l=walk_leaf(s,(uintptr_t)v,0);if(!l||!(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}
	/* AF is deliberately conservative.  DIRTY can become clean only after
	 * write permission has already been revoked. */
	if((flags&HAL_SPACE_PAGE_DIRTY)&&(*l&PTE_RO))*l&=~PTE_SW_DIRTY;
	hal_wmb();flush_locked(s);space_unlock(s,enabled);return HAL_OK;
}
void hal_space_flush_tlb(hal_space_t h){struct arm64_space *s;bool enabled;if(h==HAL_SPACE_SYS){enabled=hal_irq_disable();flush_locked(h);if(enabled)hal_irq_enable();return;}if(!space_lock_handle(h,&s,&enabled))HAL_FATAL("invalid arm64 space flush");flush_locked(s);space_unlock(s,enabled);}
void hal_space_flush_tlb_range(hal_space_t h,void*v,size_t n){(void)v;if(n)hal_space_flush_tlb(h);}
size_t hal_space_get_page_size(int level){if(level==1)return 4096;if(level==2)return 0x200000;if(level==3)return 0x40000000;return 0;}
void hal_space_get_user_range(uintptr_t *minimum,uintptr_t *limit){if(minimum)*minimum=4096;if(limit)*limit=ARM64_USER_LIMIT;}
void hal_arm64_space_memory_stats(uint32_t *s,uint32_t *t){bool enabled=hal_irq_disable();if(s)*s=space_count;if(t)*t=page_table_count;if(enabled)hal_irq_enable();}

/*
 * Maps one device range into the kernel's direct map.
 *
 * arm64 aliases physical space into the kernel half at a fixed offset, so
 * the address the caller receives is always the direct-map alias.  RAM is
 * already mapped there as cacheable memory and is returned as it is.  A
 * device range gets Device-nGnRE, never-execute entries for every 2 MiB
 * block it touches; the boot mapping covers only the peripherals the HAL
 * itself uses, and on a Pi with 4 GiB or more the peripheral hole below
 * 4 GiB starts out mapped as cacheable memory.
 */
int
hal_space_map_device(
	hal_physaddr_t paddr,
	size_t size,
	uint32_t attr,
	void **vaddr)
{
	uint64_t *table;
	uint64_t first;
	uint64_t end;
	uint64_t block;
	bool ram;
	bool touches_ram;
	bool enabled;
	int error;

	/* Requires a destination and a non-empty range the direct map reaches. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if ((uint64_t)paddr >= ARM64_DIRECT_LIMIT)
		return HAL_ERR_INVALID;
	if ((uint64_t)size > ARM64_DIRECT_LIMIT - (uint64_t)paddr)
		return HAL_ERR_INVALID;

	/* Refuses an executable device window and a write-combining one. */
	if ((attr & HAL_SPACE_EXEC) != 0)
		return HAL_ERR_INVALID;
	if ((attr & HAL_SPACE_WC) != 0)
		return HAL_ERR_UNSUPPORTED;

	/* Returns RAM's existing cacheable alias. */
	ram = range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (ram) {
		*vaddr = arm64_phys_to_direct((uintptr_t)paddr);
		return HAL_OK;
	}

	/* Refuses a range that mixes RAM and device space. */
	touches_ram = range_touches_ram((uint64_t)paddr, (uint64_t)size);
	if (touches_ram)
		return HAL_ERR_INVALID;

	/* Maps every 2 MiB block the range touches, with interrupts held off. */
	first = (uint64_t)paddr & ~(ARM64_DEVICE_BLOCK_SIZE - 1U);
	end = (uint64_t)paddr + (uint64_t)size;
	enabled = hal_irq_disable();
	for (block = first; block < end; block += ARM64_DEVICE_BLOCK_SIZE) {
		/* Finds or creates the level-2 table that holds the block. */
		error = device_block_table((unsigned)(block / ARM64_DIRECT_L1_SPAN), &table);
		if (error != HAL_OK) {
			if (enabled)
				hal_irq_enable();

			/* Reports why the block's table could not be made. */
			return error;
		}

		/* Points the block at the device. */
		map_one_device_block(table, (unsigned)((block % ARM64_DIRECT_L1_SPAN) / ARM64_DEVICE_BLOCK_SIZE), block);
	}

	/* Makes the new entries visible to the table walker and drops stale ones. */
	arm64_flush_tlb();
	if (enabled)
		hal_irq_enable();

	/* Succeeded: the device is reachable at its direct-map alias. */
	*vaddr = arm64_phys_to_direct((uintptr_t)paddr);
	return HAL_OK;
}

/*
 * Maps managed RAM a second time, uncached, for a device that does not snoop.
 *
 * The view lives in its own level-0 entry of the system half, so making it
 * never touches a live translation.  The direct map's cached lines of the
 * range are written back and discarded first.
 */
int
hal_pmem_map_uncached(
	hal_physaddr_t paddr,
	size_t size,
	void **vaddr)
{
	uint64_t *leaf;
	uint64_t start;
	uint64_t offset;
	uint64_t undo;
	bool ram;
	bool enabled;
	int error;

	/* Requires a destination and a page-aligned, non-empty range. */
	if (vaddr == NULL || size == 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)paddr & (ARM64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (((uint64_t)size & (ARM64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;

	/* Refuses anything but RAM. */
	ram = range_is_ram((uint64_t)paddr, (uint64_t)size);
	if (!ram)
		return HAL_ERR_INVALID;

	/* Writes back and drops the direct map's lines before they could shadow the view. */
	hal_dcache_invalidate_range((uintptr_t)arm64_phys_to_direct((uintptr_t)paddr), size);

	/* Takes addresses for the view from the window. */
	enabled = hal_irq_disable();
	if (uncached_next == 0)
		uncached_next = ARM64_UNCACHED_BASE;
	if ((uint64_t)size > ARM64_UNCACHED_BASE + ARM64_UNCACHED_SIZE - uncached_next) {
		if (enabled)
			hal_irq_enable();

		/* Reports a window that is used up. */
		return HAL_ERR_NOMEM;
	}

	/* Maps each page as Normal non-cacheable, never executable. */
	start = uncached_next;
	for (offset = 0; offset < (uint64_t)size; offset += ARM64_PAGE_SIZE) {
		/* Finds or makes the page's entry; a failure unmaps what was mapped. */
		error = uncached_leaf(start + offset, true, &leaf);
		if (error != HAL_OK) {
			for (undo = 0; undo < offset; undo += ARM64_PAGE_SIZE) {
				(void)uncached_leaf(start + undo, false, &leaf);
				*leaf = 0;
			}

			/* Drops the translations of the pages already mapped. */
			arm64_flush_tlb();
			if (enabled)
				hal_irq_enable();

			/* Reports why the view could not be made. */
			return error;
		}

		/* Points the page at the RAM. */
		*leaf = ((uint64_t)paddr + offset) | PAGE_FLAGS | PTE_ATTR(ARM64_ATTR_NORMAL_NC) | PTE_PXN | PTE_UXN;
	}

	/* Publishes the entries to the walker and takes the addresses. */
	arm64_flush_tlb();
	uncached_next = start + (uint64_t)size;
	if (enabled)
		hal_irq_enable();

	/* Succeeded: the RAM is reachable uncached at the view. */
	*vaddr = (void *)(uintptr_t)start;
	return HAL_OK;
}

/*
 * Removes an uncached view made by hal_pmem_map_uncached().
 *
 * The direct map's lines of the range are discarded afterwards, so a line a
 * speculative read brought in while the view existed is not read later.
 */
int
hal_pmem_unmap_uncached(
	void *vaddr,
	size_t size)
{
	uint64_t *leaf;
	uint64_t start;
	uint64_t offset;
	uint64_t physical;
	bool enabled;
	int error;

	/* Requires a page-aligned range inside the used part of the window. */
	start = (uint64_t)(uintptr_t)vaddr;
	if (size == 0)
		return HAL_ERR_INVALID;
	if ((start & (ARM64_PAGE_SIZE - 1U)) != 0 || ((uint64_t)size & (ARM64_PAGE_SIZE - 1U)) != 0)
		return HAL_ERR_INVALID;
	if (start < ARM64_UNCACHED_BASE || uncached_next == 0)
		return HAL_ERR_INVALID;
	if (start >= uncached_next || (uint64_t)size > uncached_next - start)
		return HAL_ERR_INVALID;

	/* Removes every page's entry and drops the translations. */
	enabled = hal_irq_disable();
	for (offset = 0; offset < (uint64_t)size; offset += ARM64_PAGE_SIZE) {
		/* Refuses a page that is not mapped. */
		error = uncached_leaf(start + offset, false, &leaf);
		if (error != HAL_OK || (*leaf & PTE_VALID) == 0) {
			arm64_flush_tlb();
			if (enabled)
				hal_irq_enable();

			/* Reports the hole in the view. */
			return HAL_ERR_INVALID;
		}

		/* Unmaps the page and drops the direct map's lines of it. */
		physical = *leaf & PTE_ADDR;
		*leaf = 0;
		arm64_flush_tlb();
		hal_dcache_invalidate_range((uintptr_t)arm64_phys_to_direct((uintptr_t)physical), ARM64_PAGE_SIZE);
	}

	/* Lets interrupts in again. */
	if (enabled)
		hal_irq_enable();

	/* Succeeded: the RAM is reachable through the direct map only. */
	return HAL_OK;
}

/*
 * Releases a caller's use of the shared direct device mapping.
 *
 * Device blocks are permanent kernel aliases shared by boot code and drivers.
 * Removing a whole block for one caller would revoke other live peripherals.
 */
int
hal_space_unmap_device(
	void *vaddr,
	size_t size)
{
	uint64_t address;
	uint64_t physical;

	/* Requires a nonempty range in the device mapping's direct window. */
	address = (uint64_t)(uintptr_t)vaddr;
	if (size == 0 || address < ARM64_DIRECT_BASE)
		return HAL_ERR_INVALID;

	/* Excludes uncached RAM views and ranges extending beyond the direct map. */
	physical = address - ARM64_DIRECT_BASE;
	if (physical >= ARM64_DIRECT_LIMIT)
		return HAL_ERR_INVALID;

	/* Checks the length without wrapping the address at the window's end. */
	if ((uint64_t)size > ARM64_DIRECT_LIMIT - physical)
		return HAL_ERR_INVALID;

	/* Succeeded: this caller has released its handle; shared aliases remain. */
	return HAL_OK;
}

/* Reports whether a physical range overlaps any RAM the firmware described. */
static bool
range_touches_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct rpi4_fdt_info *info;
	uint64_t ram_end;
	uint64_t end;
	unsigned i;

	/* Compares the range with every memory range of the firmware's tree. */
	info = rpi4_boot_info();
	end = physical + size;
	for (i = 0; i < info->memory_count; i++) {
		/* Skips a memory range that ends before the range starts. */
		ram_end = info->memory[i].base + info->memory[i].size;
		if (ram_end <= physical)
			continue;

		/* Skips a memory range that starts after the range ends. */
		if (info->memory[i].base >= end)
			continue;

		/* Reports the overlap. */
		return true;
	}

	/* Reports a range entirely outside RAM. */
	return false;
}

/* Reports whether a physical range lies entirely inside one RAM range. */
static bool
range_is_ram(
	uint64_t physical,
	uint64_t size)
{
	const struct rpi4_fdt_info *info;
	uint64_t ram_end;
	unsigned i;

	/* Looks for a memory range of the firmware's tree that holds it all. */
	info = rpi4_boot_info();
	for (i = 0; i < info->memory_count; i++) {
		/* Skips a memory range that starts after the range. */
		if (info->memory[i].base > physical)
			continue;

		/* Skips a bank below this address before subtracting its end. */
		ram_end = info->memory[i].base + info->memory[i].size;
		if (physical >= ram_end)
			continue;

		/* Skips a bank that contains the start but not the entire range. */
		if (ram_end - physical < size)
			continue;

		/* Reports the memory range that holds it. */
		return true;
	}

	/* Reports a range that is not wholly RAM. */
	return false;
}

/*
 * Finds the level-2 table of the direct map for one level-1 entry.
 *
 * An empty entry lies beyond RAM and gets a fresh table.  An entry that
 * maps a whole gigabyte of RAM as one block cannot hold a device.
 */
static int
device_block_table(
	unsigned l1_index,
	uint64_t **table)
{
	hal_physaddr_t memory;
	uint64_t entry;
	uint64_t *fresh;
	int error;

	/* Uses the table the entry already points at. */
	entry = system_kernel_l1[l1_index];
	if ((entry & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE)) {
		*table = arm64_phys_to_direct((uintptr_t)(entry & PTE_ADDR));
		return HAL_OK;
	}

	/* Refuses to split a gigabyte block of RAM. */
	if ((entry & PTE_VALID) != 0)
		return HAL_ERR_INVALID;

	/* Allocates an empty table for the unmapped gigabyte. */
	error = alloc_page(&memory);
	if (error != HAL_OK)
		return error;

	/* Clears the table so every block of the gigabyte starts unmapped. */
	fresh = arm64_phys_to_direct((uintptr_t)memory);
	hal_memset(fresh, 0, ARM64_PAGE_SIZE);
	page_table_count++;

	/*
	 * Links the table.  The entry was invalid, so no stale translation of it
	 * can exist and no break-before-make is needed; the store is ordered
	 * before the walker's next use by the barrier in arm64_flush_tlb().
	 */
	system_kernel_l1[l1_index] = (uint64_t)memory | PTE_VALID | PTE_TABLE;

	/* Succeeded: the caller fills blocks of the new table. */
	*table = fresh;
	return HAL_OK;
}

/*
 * Makes one level-2 entry a Device-nGnRE block for the given physical block.
 *
 * An entry that already maps the block that way is left alone.  An entry
 * that maps anything else is broken before it is remade, as the
 * architecture requires when attributes change.
 */
static void
map_one_device_block(
	uint64_t *table,
	unsigned l2_index,
	uint64_t physical)
{
	uint64_t wanted;

	/* Keeps a block that is already the wanted device mapping. */
	wanted = physical | BLOCK_FLAGS | PTE_ATTR(1) | PTE_PXN | PTE_UXN;
	if (table[l2_index] == wanted)
		return;

	/* Breaks a valid entry and drops its translation before remaking it. */
	if ((table[l2_index] & PTE_VALID) != 0) {
		table[l2_index] = 0;
		arm64_flush_tlb();
	}

	/* Makes the block a device mapping. */
	table[l2_index] = wanted;
}

/*
 * Finds the level-3 entry of one page of the uncached window.
 *
 * With create, missing tables are allocated on the way down.
 */
static int
uncached_leaf(
	uint64_t address,
	bool create,
	uint64_t **leaf)
{
	uint64_t *level1;
	uint64_t *level2;
	uint64_t *level3;
	int error;

	/* Walks from the window's level-0 entry down to the page's table. */
	error = uncached_table(system_kernel_l0, ARM64_UNCACHED_L0_INDEX, create, &level1);
	if (error != HAL_OK)
		return error;

	/* Descends to the gigabyte's table. */
	error = uncached_table(level1, (unsigned)((address >> 30) & 511U), create, &level2);
	if (error != HAL_OK)
		return error;

	/* Descends to the 2 MiB block's table. */
	error = uncached_table(level2, (unsigned)((address >> 21) & 511U), create, &level3);
	if (error != HAL_OK)
		return error;

	/* Succeeded: the page's entry in its level-3 table. */
	*leaf = &level3[(address >> 12) & 511U];
	return HAL_OK;
}

/*
 * Finds the table one entry points at, creating it when asked.
 *
 * A new table is cleared before it is linked, so no stale entry is ever
 * visible to the walker.
 */
static int
uncached_table(
	uint64_t *table,
	unsigned index,
	bool create,
	uint64_t **next)
{
	hal_physaddr_t memory;
	uint64_t *fresh;
	int error;

	/* Uses the table the entry already points at. */
	if ((table[index] & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE)) {
		*next = arm64_phys_to_direct((uintptr_t)(table[index] & PTE_ADDR));
		return HAL_OK;
	}

	/* Refuses an entry that is a block, or a missing one without create. */
	if ((table[index] & PTE_VALID) != 0 || !create)
		return HAL_ERR_INVALID;

	/* Allocates and clears the table. */
	error = alloc_page(&memory);
	if (error != HAL_OK)
		return HAL_ERR_NOMEM;

	/* Clears it so every entry starts invalid. */
	fresh = arm64_phys_to_direct((uintptr_t)memory);
	hal_memset(fresh, 0, ARM64_PAGE_SIZE);
	page_table_count++;

	/* Links it; the entry was invalid, so nothing stale can be cached. */
	table[index] = (uint64_t)memory | PTE_VALID | PTE_TABLE;

	/* Succeeded: the caller continues into the new table. */
	*next = fresh;
	return HAL_OK;
}
