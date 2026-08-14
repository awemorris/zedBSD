/* 8 KiB physical page allocator from the OpenBoot memory map. */
#include <hal/hal.h>
#include "bsp.h"
#include "defs.h"
#include "space.h"

#define MAX_PAGES (0x20000000UL/SPARCV9_PAGE_SIZE)
#define WORDS (MAX_PAGES/32U)
#define GET(m,n) ((m)[(n)>>5]&(1U<<((n)&31U)))
#define SET(m,n) ((m)[(n)>>5]|=1U<<((n)&31U))
#define CLEAR(m,n) ((m)[(n)>>5]&=~(1U<<((n)&31U)))
static uint32 used[WORDS],reserved[WORDS],phys_pages,reserved_pages,allocated_pages;

uintptr_t sparcv9_direct_to_phys(const void*p){return(uintptr_t)p-SPARCV9_DIRECT_BASE;}
void *sparcv9_phys_to_direct(uintptr_t p){return(void*)(SPARCV9_DIRECT_BASE+p);}
static void release(uint64 base,uint64 size){uint64 limit=(uint64)phys_pages*SPARCV9_PAGE_SIZE,end;uint32 first,last,p;
	if(!size||base>=limit)return;
	end=size>limit-base?limit:base+size;first=(uint32)((base+SPARCV9_PAGE_MASK)/SPARCV9_PAGE_SIZE);last=(uint32)(end/SPARCV9_PAGE_SIZE);
	for(p=first;p<last;p++)if(GET(used,p)){CLEAR(used,p);CLEAR(reserved,p);reserved_pages--;}}
static void reserve_range(uint64 base,uint64 size){uint64 limit=(uint64)phys_pages*SPARCV9_PAGE_SIZE,end;uint32 first,last,p;
	if(!size||base>=limit)return;
	end=size>limit-base?limit:base+size;first=(uint32)(base/SPARCV9_PAGE_SIZE);last=(uint32)((end+SPARCV9_PAGE_MASK)/SPARCV9_PAGE_SIZE);if(last>phys_pages)last=phys_pages;
	for(p=first;p<last;p++)if(!GET(used,p)){SET(used,p);SET(reserved,p);reserved_pages++;}}
void sparcv9_page_init(void){const struct zedbsd_sun4u_handoff*h=sun4u_boot_handoff();uint64 top=0;unsigned i;
	for(i=0;i<h->installed_count;i++){uint64 end=h->installed[i].base+h->installed[i].size;if(end>top)top=end;}if(top>0x08000000ULL)top=0x08000000ULL;phys_pages=(uint32)(top/SPARCV9_PAGE_SIZE);
	hal_memset(used,0xff,sizeof(used));hal_memset(reserved,0xff,sizeof(reserved));reserved_pages=phys_pages;allocated_pages=0;
	for(i=0;i<h->available_count;i++)release(h->available[i].base,h->available[i].size);
	/* Loader, firmware scratch, kernel locked window and handoff occupy low 8 MiB. */reserve_range(0,0x00800000UL);
	hal_printf("SPARCV9 MEMORY MAP PASS total=%llu MiB free=%llu MiB\n",(uint64)phys_pages*SPARCV9_PAGE_SIZE/(1024U*1024U),((uint64)phys_pages-reserved_pages)*SPARCV9_PAGE_SIZE/(1024U*1024U));}
void pmem_reserve(hal_physaddr_t p,size_t n){reserve_range(p,n);}
static int allocate(size_t size,uintptr_t above,uintptr_t below,struct pmem_desc*d){uint32 need,first,end,start,i;uintptr_t limit=(uintptr_t)phys_pages*SPARCV9_PAGE_SIZE;bool enabled;
	if(below>limit)below=limit;
	if(!d||!size||size>SIZE_MAX-SPARCV9_PAGE_MASK||above>=below)return PMEM_BADDESC;
	need=(uint32)((size+SPARCV9_PAGE_MASK)/SPARCV9_PAGE_SIZE);first=(uint32)((above+SPARCV9_PAGE_MASK)/SPARCV9_PAGE_SIZE);end=(uint32)(below/SPARCV9_PAGE_SIZE);if(!need||first>=end||need>end-first)return PMEM_NOSPACE;
	enabled=hal_irq_disable();for(start=first;start+need<=end;start++){for(i=0;i<need&&!GET(used,start+i);i++);if(i==need)break;start+=i;}if(start+need>end){if(enabled)hal_irq_enable();return PMEM_NOSPACE;}for(i=0;i<need;i++)SET(used,start+i);allocated_pages+=need;if(enabled)hal_irq_enable();
	d->paddr=(void*)((uintptr_t)start*SPARCV9_PAGE_SIZE);d->vaddr=sparcv9_phys_to_direct((uintptr_t)d->paddr);d->size=(size_t)need*SPARCV9_PAGE_SIZE;return PMEM_SUCCESS;}
int pmem_alloc_lo(size_t n,struct pmem_desc*d){return allocate(n,SPARCV9_PAGE_SIZE,(uintptr_t)phys_pages*SPARCV9_PAGE_SIZE,d);}
int pmem_free(struct pmem_desc*d){uint32 first,count,i;bool enabled;if(!d||!d->size||((uintptr_t)d->paddr&SPARCV9_PAGE_MASK)||(d->size&SPARCV9_PAGE_MASK)||d->vaddr!=sparcv9_phys_to_direct((uintptr_t)d->paddr))return PMEM_BADDESC;first=(uint32)((uintptr_t)d->paddr/SPARCV9_PAGE_SIZE);count=(uint32)(d->size/SPARCV9_PAGE_SIZE);if(first>=phys_pages||count>phys_pages-first)return PMEM_BADDESC;enabled=hal_irq_disable();for(i=0;i<count;i++)if(!GET(used,first+i)||GET(reserved,first+i)){if(enabled)hal_irq_enable();return PMEM_BADDESC;}for(i=0;i<count;i++)CLEAR(used,first+i);allocated_pages-=count;if(enabled)hal_irq_enable();d->vaddr=d->paddr=NULL;d->size=0;return PMEM_SUCCESS;}
int hal_pmem_alloc(size_t n,struct hal_pmem*d,uint32 f){struct pmem_desc p;int e;if(!d||(f&~(HAL_PMEM_ATTR_NOCACHE|HAL_PMEM_ATTR_WRITETHRU)))return HAL_PMEM_BADDESC;e=pmem_alloc_lo(n,&p);if(e)return e;d->vaddr=(uintptr_t)p.vaddr;d->paddr=(uintptr_t)p.paddr;d->size=p.size;return 0;}
int hal_pmem_alloc_limited(size_t n,uintptr_t a,uintptr_t b,struct hal_pmem*d){struct pmem_desc p;int e;if(!d)return HAL_PMEM_BADDESC;e=allocate(n,a,b,&p);if(e)return e;d->vaddr=(uintptr_t)p.vaddr;d->paddr=(uintptr_t)p.paddr;d->size=p.size;return 0;}
int hal_pmem_free(struct hal_pmem*d){struct pmem_desc p;int e;if(!d)return HAL_PMEM_BADDESC;p.vaddr=(void*)d->vaddr;p.paddr=(void*)d->paddr;p.size=d->size;e=pmem_free(&p);if(!e)d->vaddr=d->paddr=d->size=0;return e;}
size_t hal_pmem_get_total_size(void){return(size_t)phys_pages*SPARCV9_PAGE_SIZE;}
void hal_mem_get_memory_map(int*b,struct hal_memory_map_entry*e,size_t n){const struct zedbsd_sun4u_handoff*h=sun4u_boot_handoff();unsigned i;if(b)*b=h->installed_count;for(i=0;e&&i<h->installed_count&&i<n;i++){e[i].base=h->installed[i].base;e[i].size=h->installed[i].size;e[i].flags=HAL_PAGE_ENTRY_RAM;}}
void __attribute__((weak))hal_sparcv9_task_memory_stats(uint32*c,size_t*s){if(c)*c=0;if(s)*s=0;}void hal_sparcv9_space_memory_stats(uint32*,uint32*);
void hal_memory_get_stats(struct hal_memory_stats*s){if(!s)return;hal_memset(s,0,sizeof(*s));s->physical_total=(size_t)phys_pages*SPARCV9_PAGE_SIZE;s->physical_reserved=(size_t)reserved_pages*SPARCV9_PAGE_SIZE;s->physical_allocated=(size_t)allocated_pages*SPARCV9_PAGE_SIZE;s->physical_free=s->physical_total-s->physical_reserved-s->physical_allocated;hal_sparcv9_task_memory_stats(&s->task_count,&s->task_stack_bytes);hal_sparcv9_space_memory_stats(&s->space_count,&s->page_table_count);}
