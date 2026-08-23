/* HAL-private SPARC V9 software address maps and MMU contexts. */
#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "space.h"
#include "tte.h"

_Static_assert(SPARCV9_USER_LIMIT - 1U <= (uintptr_t)INTPTR_MAX,
    "user pointers must not overlap the negative syscall errno window");

static struct sparcv9_space *current_space;
static struct sparcv9_space *space_registry;
static uint32_t next_context=1,space_count,page_table_count;
static int space_lock_handle(hal_space_t h,struct sparcv9_space **result,bool *irq_enabled){struct sparcv9_space*s;bool enabled=hal_irq_disable();for(s=space_registry;s;s=s->registry_next)if((hal_space_t)s==h)break;if(!s||s->destroying){if(enabled)hal_irq_enable();return 0;}if(s->lock)HAL_FATAL("recursive SPARC V9 space operation");s->lock=1U;*result=s;*irq_enabled=enabled;return 1;}
static void space_unlock(struct sparcv9_space*s,bool enabled){if(s->lock!=1U)HAL_FATAL("invalid SPARC V9 space unlock");s->lock=0;if(enabled)hal_irq_enable();}
static int valid_range(uintptr_t a,size_t n){return n&&!(a&SPARCV9_PAGE_MASK)&&!(n&SPARCV9_PAGE_MASK)&&a>=SPARCV9_PAGE_SIZE&&a<SPARCV9_USER_LIMIT&&n<=SPARCV9_USER_LIMIT-a;}
static struct sparcv9_mapping *find(struct sparcv9_space*s,uintptr_t a){struct sparcv9_mapping*m;for(m=s->mappings;m;m=m->next)if(m->virtual_address==a)return m;return NULL;}

static void flush_unlocked(void){unsigned i;uint64_t tte,tag;for(i=0;i<64;i++){uintptr_t slot=(uintptr_t)i*8U;
	__asm__ volatile("ldxa [%1] 0x5d, %0":"=r"(tte):"r"(slot));if((tte&SPARCV9_TTE_VALID)&&!(tte&SPARCV9_TTE_LOCKED)){__asm__ volatile("ldxa [%1] 0x5e, %0":"=r"(tag):"r"(slot));if(!((tte&(SPARCV9_TTE_GLOBAL|SPARCV9_TTE_PRIVILEGED))==(SPARCV9_TTE_GLOBAL|SPARCV9_TTE_PRIVILEGED)&&tag>=SPARCV9_DIRECT_BASE&&tag<SPARCV9_DIRECT_BASE+0x08000000UL)){tte=0;__asm__ volatile("stxa %0, [%1] 0x5d"::"r"(tte),"r"(slot):"memory");}}
	__asm__ volatile("ldxa [%1] 0x55, %0":"=r"(tte):"r"(slot));if((tte&SPARCV9_TTE_VALID)&&!(tte&SPARCV9_TTE_LOCKED)){tte=0;__asm__ volatile("stxa %0, [%1] 0x55"::"r"(tte),"r"(slot):"memory");}}
	__asm__ volatile("membar #Sync":::"memory");}
static void flush_locked(hal_space_t h){if(h==HAL_SPACE_SYS||h==current_space)flush_unlocked();}
static void map_direct_window(void){uintptr_t physical,slot;unsigned index=0;uint64_t old,tag,tte;for(physical=0x00800000UL;physical<0x02000000UL;physical+=0x00400000UL){for(;;){if(index>=64)return;slot=(uintptr_t)index++*8U;__asm__ volatile("ldxa [%1] 0x5d, %0":"=r"(old):"r"(slot));if(!(old&SPARCV9_TTE_LOCKED))break;}tag=SPARCV9_DIRECT_BASE+physical;tte=sparcv9_tte(physical,SPARCV9_TTE_SIZE_4M|SPARCV9_TTE_CP|SPARCV9_TTE_CV|SPARCV9_TTE_LOCKED|SPARCV9_TTE_PRIVILEGED|SPARCV9_TTE_WRITE|SPARCV9_TTE_GLOBAL);__asm__ volatile("stxa %0, [%1] 0x58\n\tstxa %2, [%3] 0x5d"::"r"(tag),"r"((uintptr_t)SPARCV9_MMU_TAG_ACCESS),"r"(tte),"r"(slot):"memory");}__asm__ volatile("membar #Sync":::"memory");}
void sparcv9_space_init(void){if(hal_cpu_count()!=1)HAL_FATAL("SPARC V9 space implementation is UP-only");current_space=NULL;space_registry=NULL;sparcv9_set_primary_context(0);flush_unlocked();map_direct_window();hal_puts("SPARCV9 PAGING PASS\nSPARCV9 W^X PASS\n");}
hal_space_t hal_mem_create_space(void){struct sparcv9_space*s=hal_malloc(sizeof(*s));bool enabled;if(!s)return NULL;hal_memset(s,0,sizeof(*s));s->magic=SPARCV9_SPACE_MAGIC;enabled=hal_irq_disable();s->context=next_context++;if(next_context>=8192)next_context=1;s->registry_next=space_registry;space_registry=s;space_count++;if(enabled)hal_irq_enable();return s;}
void hal_page_destroy_space(hal_space_t h){struct sparcv9_space*s=h,**link;struct sparcv9_mapping*m;bool enabled;if(!s)return;enabled=hal_irq_disable();for(link=&space_registry;*link&&*link!=s;link=&(*link)->registry_next);if(*link==NULL||s->destroying)HAL_FATAL("invalid SPARC V9 space");if(current_space==s)HAL_FATAL("destroying an active SPARC V9 space");if(s->lock)HAL_FATAL("destroying a busy SPARC V9 space");s->destroying=1U;*link=s->registry_next;while((m=s->mappings)){s->mappings=m->next;hal_free(m);if(page_table_count)page_table_count--;}s->magic=0;hal_free(s);if(space_count)space_count--;if(enabled)hal_irq_enable();}
void hal_page_switch_space(hal_space_t h){struct sparcv9_space*s;bool enabled;if(h==(hal_space_t)current_space)return;if(h==HAL_SPACE_SYS){enabled=hal_irq_disable();flush_unlocked();current_space=NULL;sparcv9_set_primary_context(0);if(enabled)hal_irq_enable();return;}if(!space_lock_handle(h,&s,&enabled))HAL_FATAL("invalid SPARC V9 space switch");flush_unlocked();current_space=s;sparcv9_set_primary_context(s->context);space_unlock(s,enabled);}
int hal_page_map(hal_space_t h,void*v,hal_physaddr_t p,size_t n,uint32_t attr)
{
	struct sparcv9_space*s=h;uintptr_t a=(uintptr_t)v,o;bool enabled;
	if(!s||!valid_range(a,n)||(p&SPARCV9_PAGE_MASK)||p>=hal_pmem_get_total_size()||n>hal_pmem_get_total_size()-p||!(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;
	if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;
	for(o=0;o<n;o+=SPARCV9_PAGE_SIZE)if(find(s,a+o)){space_unlock(s,enabled);return HAL_ERR_INVALID;}
	for(o=0;o<n;o+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping*m=hal_malloc(sizeof(*m));if(!m){struct sparcv9_mapping*garbage=NULL;uintptr_t rollback;for(rollback=0;rollback<o;rollback+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping**link=&s->mappings;while(*link&&(*link)->virtual_address!=a+rollback)link=&(*link)->next;if(*link){struct sparcv9_mapping*old=*link;*link=old->next;old->next=garbage;garbage=old;}}flush_locked(s);while(garbage){m=garbage;garbage=m->next;hal_free(m);if(page_table_count)page_table_count--;}space_unlock(s,enabled);return HAL_ERR_NOMEM;}m->virtual_address=a+o;m->physical_address=p+o;m->attributes=attr;m->flags=HAL_PAGE_PRESENT;m->next=s->mappings;s->mappings=m;page_table_count++;}
	flush_locked(s);space_unlock(s,enabled);return HAL_OK;
}
int hal_page_prot_query(hal_space_t h,void*v,size_t n,uint32_t attr,uint32_t*f){struct sparcv9_space*s=h;uintptr_t a=(uintptr_t)v,o;uint32_t observed=0;bool enabled;if(!s||!valid_range(a,n)||!(attr&(HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC))||((attr&HAL_SPACE_WRITE)&&(attr&HAL_SPACE_EXEC)))return HAL_ERR_INVALID;if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;for(o=0;o<n;o+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping*m=find(s,a+o);if(!m){space_unlock(s,enabled);return HAL_ERR_INVALID;}}for(o=0;o<n;o+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping*m=find(s,a+o);m->attributes=attr;observed|=m->flags;}flush_locked(s);for(o=0;o<n;o+=SPARCV9_PAGE_SIZE)observed|=find(s,a+o)->flags;if(f)*f=observed;space_unlock(s,enabled);return HAL_OK;}
int hal_page_prot(hal_space_t h,void*v,size_t n,uint32_t attr){return hal_page_prot_query(h,v,n,attr,NULL);}
int hal_page_unmap(hal_space_t h,void*v,size_t n){struct sparcv9_space*s=h;struct sparcv9_mapping*garbage=NULL;uintptr_t a=(uintptr_t)v,o;bool enabled;if(!n)return HAL_OK;if(!s||!valid_range(a,n))return HAL_ERR_INVALID;if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;for(o=0;o<n;o+=SPARCV9_PAGE_SIZE){struct sparcv9_mapping**link=&s->mappings;while(*link&&(*link)->virtual_address!=a+o)link=&(*link)->next;if(*link){struct sparcv9_mapping*m=*link;*link=m->next;m->next=garbage;garbage=m;}}flush_locked(s);while(garbage){struct sparcv9_mapping*m=garbage;garbage=m->next;hal_free(m);if(page_table_count)page_table_count--;}space_unlock(s,enabled);return HAL_OK;}
int hal_page_query(hal_space_t h,void*v,uint32_t*f){struct sparcv9_space*s=h;struct sparcv9_mapping*m;bool enabled;if(!s||!f||!valid_range((uintptr_t)v,SPARCV9_PAGE_SIZE))return HAL_ERR_INVALID;if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;m=find(s,(uintptr_t)v);*f=m?m->flags:0;space_unlock(s,enabled);return HAL_OK;}
int hal_page_clear_flags(hal_space_t h,void*v,uint32_t f){struct sparcv9_space*s=h;struct sparcv9_mapping*m;bool enabled;if(!s||!valid_range((uintptr_t)v,SPARCV9_PAGE_SIZE)||(f&~(HAL_PAGE_ACCESSED|HAL_PAGE_DIRTY)))return HAL_ERR_INVALID;if(!space_lock_handle(h,&s,&enabled))return HAL_ERR_STATE;m=find(s,(uintptr_t)v);if(!m){space_unlock(s,enabled);return HAL_ERR_INVALID;}m->flags&=~f;flush_locked(s);space_unlock(s,enabled);return HAL_OK;}
void hal_page_flush_tlb(hal_space_t h){struct sparcv9_space*s;bool enabled;if(h==HAL_SPACE_SYS){enabled=hal_irq_disable();flush_locked(h);if(enabled)hal_irq_enable();return;}if(!space_lock_handle(h,&s,&enabled))HAL_FATAL("invalid SPARC V9 space flush");flush_locked(s);space_unlock(s,enabled);}
void hal_page_flush_tlb_range(hal_space_t h,void*v,size_t n){(void)v;if(n)hal_page_flush_tlb(h);}
size_t hal_page_get_page_size(int level){return level==1?SPARCV9_PAGE_SIZE:0;}
void hal_page_get_user_range(uintptr_t *minimum,uintptr_t *limit){if(minimum)*minimum=SPARCV9_PAGE_SIZE;if(limit)*limit=SPARCV9_USER_LIMIT;}
void hal_sparcv9_space_memory_stats(uint32_t*s,uint32_t*t){bool enabled=hal_irq_disable();if(s)*s=space_count;if(t)*t=page_table_count;if(enabled)hal_irq_enable();}

static int
resolve_miss_locked(struct sparcv9_space *space,uintptr_t address,
    int instruction,int write)
{
	struct sparcv9_mapping*m;uint64_t flags,tte,old;uintptr_t page=address&~SPARCV9_PAGE_MASK,slot;unsigned count;
	if((m=find(space,page))==NULL)return 0;
	if((instruction&&!(m->attributes&HAL_SPACE_EXEC))||(write&&!(m->attributes&HAL_SPACE_WRITE))||(!instruction&&!write&&!(m->attributes&HAL_SPACE_READ)))return 0;
	flags=SPARCV9_TTE_CP|SPARCV9_TTE_CV;
	if(m->attributes&HAL_SPACE_DEVICE)flags=SPARCV9_TTE_SIDE_EFFECT|SPARCV9_TTE_IE;
	if(m->attributes&HAL_SPACE_WRITE)flags|=SPARCV9_TTE_WRITE;
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

int sparcv9_resolve_miss(uintptr_t address,int instruction,int write)
{
	struct sparcv9_space *space=current_space;bool enabled;int result;
	if(space==NULL||!space_lock_handle(space,&space,&enabled))return 0;
	if(space!=current_space){space_unlock(space,enabled);return 0;}
	result=resolve_miss_locked(space,address,instruction,write);
	space_unlock(space,enabled);return result;
}

int sparcv9_prime_mapping(uintptr_t address,int instruction,int write)
{
	struct sparcv9_space *space=current_space;bool enabled;int result;uint64_t tag;
	uintptr_t reg=SPARCV9_MMU_TAG_ACCESS;
	if(space==NULL||!space_lock_handle(space,&space,&enabled))return 0;
	if(space!=current_space){space_unlock(space,enabled);return 0;}
	tag=(address&~SPARCV9_PAGE_MASK)|(uint64_t)space->context;
	if(instruction)
		__asm__ volatile("stxa %0, [%1] 0x50\n\tmembar #Sync"::"r"(tag),"r"(reg):"memory");
	else
		__asm__ volatile("stxa %0, [%1] 0x58\n\tmembar #Sync"::"r"(tag),"r"(reg):"memory");
	result=resolve_miss_locked(space,address,instruction,write);
	space_unlock(space,enabled);return result;
}
