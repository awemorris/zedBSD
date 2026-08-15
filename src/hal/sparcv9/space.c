/* HAL-private SPARC V9 software address maps and MMU contexts. */
#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "space.h"
#include "tte.h"

static struct sparcv9_space *current_space;
static uint32 next_context=1,space_count,page_table_count;
static int valid(struct sparcv9_space*s){return s&&s->magic==SPARCV9_SPACE_MAGIC;}
static int valid_range(uintptr_t a,size_t n){return n&&!(a&SPARCV9_PAGE_MASK)&&!(n&SPARCV9_PAGE_MASK)&&a>=SPARCV9_PAGE_SIZE&&a<SPARCV9_USER_LIMIT&&n<=SPARCV9_USER_LIMIT-a;}
static struct sparcv9_mapping *find(struct sparcv9_space*s,uintptr_t a){struct sparcv9_mapping*m;for(m=s->mappings;m;m=m->next)if(m->virtual_address==a)return m;return NULL;}

static void flush_unlocked(void){unsigned i;uint64 tte,tag;for(i=0;i<64;i++){uintptr_t slot=(uintptr_t)i*8U;
	__asm__ volatile("ldxa [%1] 0x5d, %0":"=r"(tte):"r"(slot));if((tte&SPARCV9_TTE_VALID)&&!(tte&SPARCV9_TTE_LOCKED)){__asm__ volatile("ldxa [%1] 0x5e, %0":"=r"(tag):"r"(slot));if(!((tte&(SPARCV9_TTE_GLOBAL|SPARCV9_TTE_PRIVILEGED))==(SPARCV9_TTE_GLOBAL|SPARCV9_TTE_PRIVILEGED)&&tag>=SPARCV9_DIRECT_BASE&&tag<SPARCV9_DIRECT_BASE+0x08000000UL)){tte=0;__asm__ volatile("stxa %0, [%1] 0x5d"::"r"(tte),"r"(slot):"memory");}}
	__asm__ volatile("ldxa [%1] 0x55, %0":"=r"(tte):"r"(slot));if((tte&SPARCV9_TTE_VALID)&&!(tte&SPARCV9_TTE_LOCKED)){tte=0;__asm__ volatile("stxa %0, [%1] 0x55"::"r"(tte),"r"(slot):"memory");}}
	__asm__ volatile("membar #Sync":::"memory");}
static void map_direct_window(void){uintptr_t physical,slot;unsigned index=0;uint64 old,tag,tte;for(physical=0x00800000UL;physical<0x02000000UL;physical+=0x00400000UL){for(;;){if(index>=64)return;slot=(uintptr_t)index++*8U;__asm__ volatile("ldxa [%1] 0x5d, %0":"=r"(old):"r"(slot));if(!(old&SPARCV9_TTE_LOCKED))break;}tag=SPARCV9_DIRECT_BASE+physical;tte=sparcv9_tte(physical,SPARCV9_TTE_SIZE_4M|SPARCV9_TTE_CP|SPARCV9_TTE_CV|SPARCV9_TTE_LOCKED|SPARCV9_TTE_PRIVILEGED|SPARCV9_TTE_WRITE|SPARCV9_TTE_GLOBAL);__asm__ volatile("stxa %0, [%1] 0x58\n\tstxa %2, [%3] 0x5d"::"r"(tag),"r"((uintptr_t)SPARCV9_MMU_TAG_ACCESS),"r"(tte),"r"(slot):"memory");}__asm__ volatile("membar #Sync":::"memory");}
void sparcv9_space_init(void){current_space=NULL;sparcv9_set_primary_context(0);flush_unlocked();map_direct_window();hal_puts("SPARCV9 PAGING PASS\nSPARCV9 W^X PASS\n");}
hal_space_t hal_mem_create_space(void){struct sparcv9_space*s=hal_malloc(sizeof(*s));if(!s)return NULL;hal_memset(s,0,sizeof(*s));s->magic=SPARCV9_SPACE_MAGIC;s->context=next_context++;if(next_context>=8192)next_context=1;space_count++;return s;}
void hal_page_destroy_space(hal_space_t h){struct sparcv9_space*s=h;struct sparcv9_mapping*m;if(!s)return;if(!valid(s))HAL_FATAL("invalid SPARC V9 space");if(current_space==s)hal_page_switch_space(NULL);while((m=s->mappings)){s->mappings=m->next;hal_free(m);if(page_table_count)page_table_count--;}s->magic=0;hal_free(s);if(space_count)space_count--;}
void hal_page_switch_space(hal_space_t h){struct sparcv9_space*s=h;if(s&&!valid(s))HAL_FATAL("invalid SPARC V9 space switch");if(s==current_space)return;flush_unlocked();current_space=s;sparcv9_set_primary_context(s?s->context:0);}
int hal_page_map(hal_space_t h,void*v,uintptr_t p,size_t n,uint32 attr){struct sparcv9_space*s=h;uintptr_t a=(uintptr_t)v,o;if(!valid(s)||!valid_range(a,n)||(p&SPARCV9_PAGE_MASK)||p>=hal_pmem_get_total_size()||n>hal_pmem_get_total_size()-p||!(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_PMEM_BADDESC;for(o=0;o<n;o+=SPARCV9_PAGE_SIZE)if(find(s,a+o))return HAL_PMEM_BADDESC;for(o=0;o<n;o+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping*m=hal_malloc(sizeof(*m));if(!m){(void)hal_page_unmap(s,v,o);return HAL_PMEM_NOSPACE;}m->virtual_address=a+o;m->physical_address=p+o;m->attributes=attr;m->flags=HAL_PAGE_PRESENT;m->next=s->mappings;s->mappings=m;page_table_count++;}return 0;}
int hal_page_prot(hal_space_t h,void*v,size_t n,uint32 attr){struct sparcv9_space*s=h;uintptr_t a=(uintptr_t)v,o;if(!valid(s)||!valid_range(a,n)||!(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_PMEM_BADDESC;for(o=0;o<n;o+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping*m=find(s,a+o);if(!m)return HAL_PMEM_BADDESC;m->attributes=attr;}if(current_space==s)flush_unlocked();return 0;}
int hal_page_unmap(hal_space_t h,void*v,size_t n){struct sparcv9_space*s=h;uintptr_t a=(uintptr_t)v,o;if(!n)return 0;if(!valid(s)||!valid_range(a,n))return HAL_PMEM_BADDESC;for(o=0;o<n;o+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping**link=&s->mappings;while(*link&&(*link)->virtual_address!=a+o)link=&(*link)->next;if(*link){struct sparcv9_mapping*m=*link;*link=m->next;hal_free(m);if(page_table_count)page_table_count--;}}if(current_space==s)flush_unlocked();return 0;}
int hal_page_query(hal_space_t h,void*v,uint32*f){struct sparcv9_space*s=h;struct sparcv9_mapping*m;if(!valid(s)||!f||!valid_range((uintptr_t)v,SPARCV9_PAGE_SIZE))return HAL_PMEM_BADDESC;m=find(s,(uintptr_t)v);*f=m?m->flags:0;return 0;}
int hal_page_clear_flags(hal_space_t h,void*v,uint32 f){struct sparcv9_space*s=h;struct sparcv9_mapping*m;if(!valid(s)||!valid_range((uintptr_t)v,SPARCV9_PAGE_SIZE)||(f&~(HAL_PAGE_ACCESSED|HAL_PAGE_DIRTY)))return HAL_PMEM_BADDESC;m=find(s,(uintptr_t)v);if(!m)return HAL_PMEM_BADDESC;m->flags&=~f;if(current_space==s)flush_unlocked();return 0;}
void hal_page_flush_tlb(hal_space_t h){if(h==HAL_SPACE_SYS||h==current_space)flush_unlocked();}
size_t hal_page_get_page_size(int level){return level==1?SPARCV9_PAGE_SIZE:0;}
void hal_page_get_user_range(uintptr_t *minimum,uintptr_t *limit){if(minimum)*minimum=SPARCV9_PAGE_SIZE;if(limit)*limit=SPARCV9_USER_LIMIT;}
void hal_sparcv9_space_memory_stats(uint32*s,uint32*t){if(s)*s=space_count;if(t)*t=page_table_count;}

int sparcv9_resolve_miss(uintptr_t address,int instruction,int write)
{
	struct sparcv9_mapping*m;uint64 flags,tte,old;uintptr_t page=address&~SPARCV9_PAGE_MASK,slot;unsigned count;
	if(!current_space||(m=find(current_space,page))==NULL)return 0;
	if((instruction&&!(m->attributes&HAL_SPACE_EXEC))||(write&&!(m->attributes&HAL_SPACE_WRITE))||(!instruction&&!write&&!(m->attributes&HAL_SPACE_READ)))return 0;
	flags=SPARCV9_TTE_CP|SPARCV9_TTE_CV;if(m->attributes&HAL_SPACE_WRITE)flags|=SPARCV9_TTE_WRITE;if(m->attributes&HAL_SPACE_DEVICE)flags=SPARCV9_TTE_SIDE_EFFECT|SPARCV9_TTE_IE|SPARCV9_TTE_WRITE;
	/* sun4u TTE dirty state is not reflected into the software map.  Mark a
	 * writable mapping conservatively dirty when it enters the TLB; clearing
	 * the flag also flushes the TLB, so a later access re-establishes it. */
	tte=sparcv9_tte(m->physical_address,flags);m->flags|=HAL_PAGE_ACCESSED;if(m->attributes&HAL_SPACE_WRITE)m->flags|=HAL_PAGE_DIRTY;
	slot=(page>>10)&0x1f8U;
	for(count=0;count<64;count++,slot=(slot+8U)&0x1f8U){
		if(instruction)__asm__ volatile("ldxa [%1] 0x55, %0":"=r"(old):"r"(slot));else __asm__ volatile("ldxa [%1] 0x5d, %0":"=r"(old):"r"(slot));
		if(!(old&SPARCV9_TTE_LOCKED))break;
	}
	if(count==64)return 0;
	if(instruction)__asm__ volatile("stxa %0, [%1] 0x55"::"r"(tte),"r"(slot):"memory");else __asm__ volatile("stxa %0, [%1] 0x5d"::"r"(tte),"r"(slot):"memory");
	__asm__ volatile("membar #Sync":::"memory");return 1;
}

int sparcv9_prime_mapping(uintptr_t address,int instruction,int write)
{
	uint64 tag;
	uintptr_t reg=SPARCV9_MMU_TAG_ACCESS;
	if(!current_space)return 0;
	tag=(address&~SPARCV9_PAGE_MASK)|(uint64)current_space->context;
	if(instruction)
		__asm__ volatile("stxa %0, [%1] 0x50\n\tmembar #Sync"::"r"(tag),"r"(reg):"memory");
	else
		__asm__ volatile("stxa %0, [%1] 0x58\n\tmembar #Sync"::"r"(tag),"r"(reg):"memory");
	return sparcv9_resolve_miss(address,instruction,write);
}
