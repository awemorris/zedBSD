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

static int alloc_page(struct hal_pmem *memory)
{
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, ARM64_PAGE_SIZE, ARM64_PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	return hal_pmem_alloc(&request, memory);
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
	struct arm64_table_page *p=hal_malloc(sizeof(*p));
	if(!p)return NULL;
	if(alloc_page(&p->memory)!=HAL_OK){hal_free(p);return NULL;}
	hal_memset(p->memory.vaddr,0,ARM64_PAGE_SIZE);p->parent=parent;p->parent_index=index;
	p->next=s->tables;s->tables=p;page_table_count++;return p;
}
static uint64_t *walk_leaf(struct arm64_space *s,uintptr_t a,int create)
{
	static const unsigned shifts[3]={39,30,21};uint64_t *table=s->l0;unsigned level;
	for(level=0;level<3;level++){
		unsigned index=(unsigned)(a>>shifts[level])&511;uint64_t e=table[index];
		if(!(e&PTE_VALID)){struct arm64_table_page *p;if(!create)return NULL;
			p=allocate_table(s,table,index);if(!p)return NULL;e=(uintptr_t)p->memory.paddr|PTE_VALID|PTE_TABLE;table[index]=e;}
		if((e&(PTE_VALID|PTE_TABLE))!=(PTE_VALID|PTE_TABLE))return NULL;
		table=arm64_phys_to_direct((uintptr_t)(e&PTE_ADDR));
	}return &table[(a>>12)&511];
}
static int table_empty(const uint64_t *p){unsigned i;for(i=0;i<512;i++)if(p[i]&PTE_VALID)return 0;return 1;}
static struct arm64_table_page *detach_empty_tables(struct arm64_space *s)
{
	struct arm64_table_page *detached=NULL;int again;
	do{struct arm64_table_page **link=&s->tables;again=0;while(*link){struct arm64_table_page *p=*link;
			if(!table_empty(p->memory.vaddr)){link=&p->next;continue;}
			if(!(p->parent[p->parent_index]&PTE_VALID)||
			   (p->parent[p->parent_index]&PTE_ADDR)!=(uintptr_t)p->memory.paddr)
				HAL_FATAL("detaching an unlinked arm64 page table");
			p->parent[p->parent_index]=0;*link=p->next;p->next=detached;
			detached=p;again=1;}}while(again);
	return detached;
}
static void free_detached_tables(struct arm64_table_page *p)
{
	while(p){struct arm64_table_page *next=p->next;(void)hal_pmem_free(&p->memory);
		hal_free(p);if(page_table_count)page_table_count--;p=next;}
}
hal_space_t hal_mem_create_space(void)
{
	struct arm64_space *s=hal_malloc(sizeof(*s));bool enabled;if(!s)return NULL;hal_memset(s,0,sizeof(*s));
	if(alloc_page(&s->l0_memory)!=HAL_OK){hal_free(s);return NULL;}
	s->l0=s->l0_memory.vaddr;hal_memset(s->l0,0,ARM64_PAGE_SIZE);s->magic=ARM64_SPACE_MAGIC;
	enabled=hal_irq_disable();s->space_id=next_space_id++;s->registry_next=space_registry;space_registry=s;space_count++;if(enabled)hal_irq_enable();return s;
}
void hal_page_destroy_space(hal_space_t h)
{
	struct arm64_space *s=h,**link;struct arm64_table_page *p;bool enabled;if(!s)return;
	enabled=hal_irq_disable();for(link=&space_registry;*link&&*link!=s;link=&(*link)->registry_next);
	if(*link==NULL||s->destroying)HAL_FATAL("invalid arm64 space destroy");
	if(current_space==s)HAL_FATAL("destroying an active arm64 space");
	if(s->lock!=0)HAL_FATAL("destroying a busy arm64 space");
	s->destroying=1U;*link=s->registry_next;
	while((p=s->tables)){s->tables=p->next;(void)hal_pmem_free(&p->memory);hal_free(p);if(page_table_count)page_table_count--;}
	s->magic=0;(void)hal_pmem_free(&s->l0_memory);hal_free(s);if(space_count)space_count--;if(enabled)hal_irq_enable();
}
void hal_page_switch_space(hal_space_t h)
{
	struct arm64_space *s;uintptr_t ttbr;bool enabled;if(h==current_space)return;
	if(h==HAL_SPACE_SYS){enabled=hal_irq_disable();arm64_write_ttbr0(system_ttbr0);arm64_flush_tlb();current_space=h;if(enabled)hal_irq_enable();return;}
	if(!space_lock_handle(h,&s,&enabled))HAL_FATAL("invalid arm64 space switch");
	ttbr=(uintptr_t)s->l0_memory.paddr;arm64_write_ttbr0(ttbr);arm64_flush_tlb();current_space=h;space_unlock(s,enabled);
}
static uint64_t leaf_flags(uint32_t attr)
{
	uint64_t f=PAGE_FLAGS|PTE_USER|PTE_PXN;
	if(!(attr&HAL_SPACE_WRITE))f|=PTE_RO;else f|=PTE_SW_DIRTY;
	if(!(attr&HAL_SPACE_EXEC))f|=PTE_UXN;
	if(attr&HAL_SPACE_DEVICE)f|=PTE_ATTR(1);else if(attr&HAL_SPACE_NOCACHE)f|=PTE_ATTR(2);
	return f;
}
int hal_page_map(hal_space_t h,void *v,hal_physaddr_t p,size_t n,uint32_t attr)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;bool enabled;
	if(!s||!valid_user(a,n)||(p&4095)||p>=hal_pmem_get_total_size()||n>hal_pmem_get_total_size()-p||
	   !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(l&&(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}}
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,1);if(!l){struct arm64_table_page *detached;uintptr_t rollback;for(rollback=0;rollback<o;rollback+=4096){l=walk_leaf(s,a+rollback,0);if(l)*l=0;}detached=detach_empty_tables(s);hal_wmb();flush_locked(s);free_detached_tables(detached);space_unlock(s,enabled);return HAL_ERR_NOMEM;}*l=(p+o)|leaf_flags(attr);}
	flush_locked(s);space_unlock(s,enabled);return HAL_OK;
}
int hal_page_prot_query(hal_space_t h,void *v,size_t n,uint32_t attr,uint32_t *flags)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;uint32_t observed=0;bool enabled;if(!s||!valid_user(a,n)||
	 !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(!l||!(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}}
	/* Use break-before-make for the complete range.  This also covers callers
	 * which change the AttrIndx, not merely the permission bits. */
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),old=*l;observed|=HAL_PAGE_PRESENT;if(old&PTE_AF)observed|=HAL_PAGE_ACCESSED;
		/* This profile has no hardware dirty management.  Any writable
		 * translation is conservatively dirty, including stores made before
		 * the TLB invalidation completes. */
		if((old&PTE_RO)==0)old|=PTE_SW_DIRTY;
		if(old&PTE_SW_DIRTY)observed|=HAL_PAGE_DIRTY;
		*l=old&~PTE_VALID;}
	hal_wmb();flush_locked(s);
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),old=*l;*l=(old&PTE_ADDR)|leaf_flags(attr)|(old&PTE_SW_DIRTY);}
	hal_wmb();flush_locked(s);
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0),entry=*l;if(entry&PTE_AF)observed|=HAL_PAGE_ACCESSED;if(entry&PTE_SW_DIRTY)observed|=HAL_PAGE_DIRTY;}
	if(flags)*flags=observed;
	space_unlock(s,enabled);return HAL_OK;
}
int hal_page_prot(hal_space_t h,void *v,size_t n,uint32_t attr){return hal_page_prot_query(h,v,n,attr,NULL);}
int hal_page_unmap(hal_space_t h,void *v,size_t n)
{
	struct arm64_space *s=h;struct arm64_table_page *detached;uintptr_t a=(uintptr_t)v,o;bool enabled;if(!n)return HAL_OK;if(!s||!valid_user(a,n))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=4096){uint64_t *l=walk_leaf(s,a+o,0);if(l)*l=0;}
	detached=detach_empty_tables(s);hal_wmb();flush_locked(s);
	free_detached_tables(detached);space_unlock(s,enabled);return HAL_OK;
}
int hal_page_query(hal_space_t h,void *v,uint32_t *flags)
{
	struct arm64_space *s=h;uint64_t *l;bool enabled;if(!s||!flags||!valid_user((uintptr_t)v,4096))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	l=walk_leaf(s,(uintptr_t)v,0);*flags=l&&(*l&PTE_VALID)?HAL_PAGE_PRESENT|HAL_PAGE_ACCESSED:0;if(l&&(*l&PTE_SW_DIRTY))*flags|=HAL_PAGE_DIRTY;space_unlock(s,enabled);return HAL_OK;
}
int hal_page_clear_flags(hal_space_t h,void *v,uint32_t flags)
{
	struct arm64_space *s=h;uint64_t *l;bool enabled;if(!s||!valid_user((uintptr_t)v,4096)||(flags&~(HAL_PAGE_ACCESSED|HAL_PAGE_DIRTY)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	l=walk_leaf(s,(uintptr_t)v,0);if(!l||!(*l&PTE_VALID)){space_unlock(s,enabled);return HAL_ERR_INVALID;}
	/* AF is deliberately conservative.  DIRTY can become clean only after
	 * write permission has already been revoked. */
	if((flags&HAL_PAGE_DIRTY)&&(*l&PTE_RO))*l&=~PTE_SW_DIRTY;
	hal_wmb();flush_locked(s);space_unlock(s,enabled);return HAL_OK;
}
void hal_page_flush_tlb(hal_space_t h){struct arm64_space *s;bool enabled;if(h==HAL_SPACE_SYS){enabled=hal_irq_disable();flush_locked(h);if(enabled)hal_irq_enable();return;}if(!space_lock_handle(h,&s,&enabled))HAL_FATAL("invalid arm64 space flush");flush_locked(s);space_unlock(s,enabled);}
void hal_page_flush_tlb_range(hal_space_t h,void*v,size_t n){(void)v;if(n)hal_page_flush_tlb(h);}
size_t hal_page_get_page_size(int level){if(level==1)return 4096;if(level==2)return 0x200000;if(level==3)return 0x40000000;return 0;}
void hal_page_get_user_range(uintptr_t *minimum,uintptr_t *limit){if(minimum)*minimum=4096;if(limit)*limit=ARM64_USER_LIMIT;}
void hal_arm64_space_memory_stats(uint32_t *s,uint32_t *t){bool enabled=hal_irq_disable();if(s)*s=space_count;if(t)*t=page_table_count;if(enabled)hal_irq_enable();}
