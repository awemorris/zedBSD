#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "space.h"

#define PTE_VALID (1ULL << 0)
#define PTE_TABLE (1ULL << 1)
#define PTE_ATTR(n) ((uint64)(n) << 2)
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

static uint64 system_user_l0[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64 system_kernel_l0[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64 system_kernel_l1[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64 system_kernel_l2_low[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64 system_kernel_l3_low[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uint64 system_kernel_l2_high[512] __attribute__((aligned(ARM64_PAGE_SIZE)));
static uintptr_t system_ttbr0;
static hal_space_t current_space;
static int next_space_id=1;
static uint32 space_count, page_table_count;
extern char __kernel_text_start[],__kernel_text_end[];
extern char __kernel_rodata_start[],__kernel_rodata_end[];
extern char __kernel_data_start[],__kernel_data_end[];

uintptr_t arm64_direct_to_phys(const void *p) { return (uintptr_t)p-ARM64_DIRECT_BASE; }
void *arm64_phys_to_direct(uintptr_t p) { return (void *)(ARM64_DIRECT_BASE+p); }

static uint64 table_desc(const void *table)
{ return arm64_direct_to_phys(table)|PTE_VALID|PTE_TABLE; }

static void
map_device_block(uint64 physical)
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
	uint64 total=hal_pmem_get_total_size(); unsigned i;
	uintptr_t text_start=arm64_direct_to_phys(__kernel_text_start);
	uintptr_t text_end=arm64_direct_to_phys(__kernel_text_end);
	uintptr_t rodata_start=arm64_direct_to_phys(__kernel_rodata_start);
	uintptr_t rodata_end=arm64_direct_to_phys(__kernel_rodata_end);
	uintptr_t data_start=arm64_direct_to_phys(__kernel_data_start);
	hal_memset(system_user_l0,0,sizeof(system_user_l0));
	hal_memset(system_kernel_l0,0,sizeof(system_kernel_l0));
	hal_memset(system_kernel_l1,0,sizeof(system_kernel_l1));
	hal_memset(system_kernel_l2_low,0,sizeof(system_kernel_l2_low));
	hal_memset(system_kernel_l3_low,0,sizeof(system_kernel_l3_low));
	hal_memset(system_kernel_l2_high,0,sizeof(system_kernel_l2_high));
	system_kernel_l0[0]=table_desc(system_kernel_l1);
	for(i=0;i<512;i++) {
		uint64 physical=(uint64)i<<30;
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
		uint64 physical=0xc0000000ULL+((uint64)i<<21);
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
		system_kernel_l2_low[i]=((uint64)i<<21)|BLOCK_FLAGS|PTE_PXN|PTE_UXN;
	system_kernel_l2_low[0]=table_desc(system_kernel_l3_low);
	for(i=0;i<512;i++) {
		uint64 p=(uint64)i*ARM64_PAGE_SIZE;
		uint64 flags=PAGE_FLAGS|PTE_PXN|PTE_UXN;
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

static int valid_space(hal_space_t h)
{ return h==HAL_SPACE_SYS||(h&&((struct arm64_space *)h)->magic==ARM64_SPACE_MAGIC); }
static int valid_user(uintptr_t a,size_t n)
{ return n&&(a&4095)==0&&(n&4095)==0&&a>=4096&&a<0x0001000000000000ULL&&n<=0x0001000000000000ULL-a; }

static struct arm64_table_page *allocate_table(struct arm64_space *s,uint64 *parent,unsigned index)
{
	struct arm64_table_page *p=hal_malloc(sizeof(*p));
	if(!p)return NULL;
	if(pmem_alloc_lo(ARM64_PAGE_SIZE,&p->memory)){hal_free(p);return NULL;}
	hal_memset(p->memory.vaddr,0,ARM64_PAGE_SIZE);p->parent=parent;p->parent_index=index;
	p->next=s->tables;s->tables=p;page_table_count++;return p;
}
static uint64 *walk_leaf(struct arm64_space *s,uintptr_t a,int create)
{
	static const unsigned shifts[3]={39,30,21};uint64 *table=s->l0;unsigned level;
	for(level=0;level<3;level++){
		unsigned index=(unsigned)(a>>shifts[level])&511;uint64 e=table[index];
		if(!(e&PTE_VALID)){struct arm64_table_page *p;if(!create)return NULL;
			p=allocate_table(s,table,index);if(!p)return NULL;e=(uintptr_t)p->memory.paddr|PTE_VALID|PTE_TABLE;table[index]=e;}
		if((e&(PTE_VALID|PTE_TABLE))!=(PTE_VALID|PTE_TABLE))return NULL;
		table=arm64_phys_to_direct((uintptr_t)(e&PTE_ADDR));
	}return &table[(a>>12)&511];
}
static int table_empty(const uint64 *p){unsigned i;for(i=0;i<512;i++)if(p[i]&PTE_VALID)return 0;return 1;}
static void reclaim(struct arm64_space *s)
{
	int again;do{struct arm64_table_page **link=&s->tables;again=0;while(*link){struct arm64_table_page *p=*link;
		if(!table_empty(p->memory.vaddr)){link=&p->next;continue;}
		if((p->parent[p->parent_index]&PTE_ADDR)==(uintptr_t)p->memory.paddr)p->parent[p->parent_index]=0;
		*link=p->next;(void)pmem_free(&p->memory);hal_free(p);if(page_table_count)page_table_count--;again=1;}}while(again);
}
hal_space_t hal_mem_create_space(void)
{
	struct arm64_space *s=hal_malloc(sizeof(*s));if(!s)return NULL;hal_memset(s,0,sizeof(*s));
	if(pmem_alloc_lo(ARM64_PAGE_SIZE,&s->l0_memory)){hal_free(s);return NULL;}
	s->l0=s->l0_memory.vaddr;hal_memset(s->l0,0,ARM64_PAGE_SIZE);s->magic=ARM64_SPACE_MAGIC;s->space_id=next_space_id++;space_count++;return s;
}
void hal_page_destroy_space(hal_space_t h)
{
	struct arm64_space *s=h;struct arm64_table_page *p;if(!s)return;
	if(!valid_space(s))HAL_FATAL("invalid arm64 space destroy");
	if(current_space==s)hal_page_switch_space(NULL);
	while((p=s->tables)){s->tables=p->next;(void)pmem_free(&p->memory);hal_free(p);if(page_table_count)page_table_count--;}
	s->magic=0;(void)pmem_free(&s->l0_memory);hal_free(s);if(space_count)space_count--;
}
void hal_page_switch_space(hal_space_t h)
{
	uintptr_t ttbr;if(!valid_space(h))HAL_FATAL("invalid arm64 space switch");if(h==current_space)return;
	ttbr=h?(uintptr_t)((struct arm64_space *)h)->l0_memory.paddr:system_ttbr0;
	arm64_write_ttbr0(ttbr);arm64_flush_tlb();current_space=h;
}
static uint64 leaf_flags(uint32 attr)
{
	uint64 f=PAGE_FLAGS|PTE_USER|PTE_PXN;
	if(!(attr&HAL_SPACE_WRITE))f|=PTE_RO;else f|=PTE_SW_DIRTY;
	if(!(attr&HAL_SPACE_EXEC))f|=PTE_UXN;
	if(attr&HAL_SPACE_DEVICE)f|=PTE_ATTR(1);else if(attr&HAL_SPACE_NOCACHE)f|=PTE_ATTR(2);
	return f;
}
int hal_page_map(hal_space_t h,void *v,uintptr_t p,size_t n,uint32 attr)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;
	if(!s||!valid_space(s)||!valid_user(a,n)||(p&4095)||p>=hal_pmem_get_total_size()||n>hal_pmem_get_total_size()-p||
	   !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_PMEM_BADDESC;
	for(o=0;o<n;o+=4096){uint64 *l=walk_leaf(s,a+o,0);if(l&&(*l&PTE_VALID))return HAL_PMEM_BADDESC;}
	for(o=0;o<n;o+=4096){uint64 *l=walk_leaf(s,a+o,1);if(!l){(void)hal_page_unmap(s,v,o);reclaim(s);return HAL_PMEM_NOSPACE;}*l=(p+o)|leaf_flags(attr);}
	if(current_space==s)hal_page_flush_tlb(s);
	return HAL_PMEM_SUCCESS;
}
int hal_page_prot(hal_space_t h,void *v,size_t n,uint32 attr)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;if(!s||!valid_space(s)||!valid_user(a,n)||
	 !(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_PMEM_BADDESC;
	for(o=0;o<n;o+=4096){uint64 *l=walk_leaf(s,a+o,0),p;if(!l||!(*l&PTE_VALID))return HAL_PMEM_BADDESC;p=*l&PTE_ADDR;*l=p|leaf_flags(attr);}
	if(current_space==s)hal_page_flush_tlb(s);
	return HAL_PMEM_SUCCESS;
}
int hal_page_unmap(hal_space_t h,void *v,size_t n)
{
	struct arm64_space *s=h;uintptr_t a=(uintptr_t)v,o;if(!n)return HAL_PMEM_SUCCESS;if(!s||!valid_space(s)||!valid_user(a,n))return HAL_PMEM_BADDESC;
	for(o=0;o<n;o+=4096){uint64 *l=walk_leaf(s,a+o,0);if(l)*l=0;}reclaim(s);if(current_space==s)hal_page_flush_tlb(s);return HAL_PMEM_SUCCESS;
}
int hal_page_query(hal_space_t h,void *v,uint32 *flags)
{
	struct arm64_space *s=h;uint64 *l;if(!s||!valid_space(s)||!flags||!valid_user((uintptr_t)v,4096))return HAL_PMEM_BADDESC;
	l=walk_leaf(s,(uintptr_t)v,0);*flags=l&&(*l&PTE_VALID)?HAL_PAGE_PRESENT|HAL_PAGE_ACCESSED:0;if(l&&(*l&PTE_SW_DIRTY))*flags|=HAL_PAGE_DIRTY;return HAL_PMEM_SUCCESS;
}
int hal_page_clear_flags(hal_space_t h,void *v,uint32 flags)
{
	struct arm64_space *s=h;uint64 *l;if(!s||!valid_space(s)||!valid_user((uintptr_t)v,4096)||(flags&~(HAL_PAGE_ACCESSED|HAL_PAGE_DIRTY)))return HAL_PMEM_BADDESC;
	l=walk_leaf(s,(uintptr_t)v,0);if(!l||!(*l&PTE_VALID))return HAL_PMEM_BADDESC;if(flags&HAL_PAGE_DIRTY)*l&=~PTE_SW_DIRTY;if(current_space==s)hal_page_flush_tlb(s);return HAL_PMEM_SUCCESS;
}
void hal_page_flush_tlb(hal_space_t h){if(h==HAL_SPACE_SYS||h==current_space)arm64_flush_tlb();}
size_t hal_page_get_page_size(int level){if(level==1)return 4096;if(level==2)return 0x200000;if(level==3)return 0x40000000;return 0;}
void hal_page_get_user_range(uintptr_t *minimum,uintptr_t *limit){if(minimum)*minimum=4096;if(limit)*limit=0x0001000000000000ULL;}
void hal_arm64_space_memory_stats(uint32 *s,uint32 *t){if(s)*s=space_count;if(t)*t=page_table_count;}
