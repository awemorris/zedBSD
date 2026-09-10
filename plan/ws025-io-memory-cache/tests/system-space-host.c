/* Actual amd64 page operations with controlled table allocation and TLB backend. */
#include <hal/hal.h>
#include "src/hal/amd64/defs.h"
#include "src/hal/amd64/space.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define SYSTEM_DYNAMIC_BASE UINT64_C(0xffffc00000000000)
#define SYSTEM_DYNAMIC_LIMIT UINT64_C(0xffffe00000000000)
static uint64_t system_pml4[512],query_tables[3][512] __attribute__((aligned(4096)));
static struct amd64_space system_space;
static uint64_t leaves[16];
static int ram_active=1,ram_builder,locked;
static unsigned creates,fail_create,flushes;
static int valid_user_range(uintptr_t a,size_t n) {return a>=4096 && a<0x100000 && n && !(a%4096) && !(n%4096) && n<=0x100000-a;}
static int amd64_ram_lookup(void *b,uint64_t p,uint64_t *e) {(void)b;if(p>=0x100000)return 0;*e=AMD64_PTE_PRESENT|AMD64_PTE_WRITE;return 1;}
void *amd64_phys_to_direct(uintptr_t p) {return (void *)p;}
static int space_op_enter(struct amd64_space *s) {return s!=NULL;}
static void space_op_leave(struct amd64_space *s) {(void)s;}
static bool space_lock_enter(struct amd64_space *s) {(void)s;assert(!locked);locked=1;return true;}
static void space_lock_leave(struct amd64_space *s,bool enabled) {(void)s;(void)enabled;assert(locked);locked=0;}
static uint64_t *walk_leaf(struct amd64_space *s,uintptr_t a,int create)
{
 size_t i=(a-(s==&system_space?SYSTEM_DYNAMIC_BASE:4096))/4096;
 assert(locked && i<16);
 if(create && ++creates==fail_create)return NULL;
 return &leaves[i];
}
static struct amd64_table_page *detach_empty_tables(struct amd64_space *s) {(void)s;return NULL;}
static void free_detached_tables(struct amd64_table_page *p) {assert(!p);}
static void shootdown(hal_space_t h,void *p,size_t n) {(void)h;(void)p;(void)n;assert(locked);flushes++;}
void hal_fatal(const char *f,int l,const char *m) {fprintf(stderr,"%s:%d %s\n",f,l,m);abort();}
#include "system-space-extracted.h"
int main(void)
{
 uintptr_t lo,hi,a=SYSTEM_DYNAMIC_BASE;
 hal_physaddr_t pa;
 uint32_t flags;
 unsigned i,before;
 struct amd64_space user={0};
 hal_space_get_kernel_range(&lo,&hi);assert(lo==a && hi==SYSTEM_DYNAMIC_LIMIT);
 assert(hal_space_map(HAL_SPACE_SYS,(void *)4096,4096,4096,HAL_SPACE_READ)==HAL_ERR_INVALID);
 before=flushes;
 assert(hal_space_map(HAL_SPACE_SYS,(void *)a,4096,8192,HAL_SPACE_READ|HAL_SPACE_WRITE)==HAL_OK);
 assert(flushes==before);
 assert(!(leaves[0]&AMD64_PTE_USER) && (leaves[0]&AMD64_PTE_NX));
 assert(hal_space_map(HAL_SPACE_SYS,(void *)a,4096,4096,HAL_SPACE_READ)==HAL_ERR_INVALID);
 leaves[0]|=AMD64_PTE_DIRTY|AMD64_PTE_ACCESSED;
 assert(hal_space_prot_query(HAL_SPACE_SYS,(void *)a,8192,HAL_SPACE_READ,&flags)==HAL_OK);
 assert((flags&(HAL_SPACE_PAGE_DIRTY|HAL_SPACE_PAGE_ACCESSED))==(HAL_SPACE_PAGE_DIRTY|HAL_SPACE_PAGE_ACCESSED));
 assert(!(leaves[0]&(AMD64_PTE_USER|AMD64_PTE_WRITE)));
 /* Repeated permission-only calls need no new retirement, even with A/D. */
 before=flushes;
 assert(hal_space_prot(HAL_SPACE_SYS,(void *)a,8192,HAL_SPACE_READ)==HAL_OK);
 assert(flushes==before && (leaves[0]&AMD64_PTE_DIRTY));
 assert(hal_space_prot_query(HAL_SPACE_SYS,(void *)a,8192,HAL_SPACE_READ,&flags)==HAL_OK);
 assert(flushes==before+1 && (flags&HAL_SPACE_PAGE_DIRTY));
 /* A later hole must reject the whole request before changing an earlier PTE. */
 before=flushes;
 assert(hal_space_prot(HAL_SPACE_SYS,(void *)a,12288,HAL_SPACE_READ|HAL_SPACE_WRITE)==HAL_ERR_INVALID);
 assert(flushes==before && !(leaves[0]&AMD64_PTE_WRITE));
 leaves[1]|=AMD64_PTE_WRITE;
 assert(hal_space_prot(HAL_SPACE_SYS,(void *)a,8192,HAL_SPACE_READ)==HAL_OK);
 assert(flushes==before+1 && !(leaves[1]&AMD64_PTE_WRITE));
 assert(hal_space_clear_flags(HAL_SPACE_SYS,(void *)a,HAL_SPACE_PAGE_DIRTY)==HAL_OK);
 assert(!(leaves[0]&AMD64_PTE_DIRTY));
 before=flushes;
 assert(hal_space_unmap(HAL_SPACE_SYS,(void *)a,8192)==HAL_OK && !leaves[0] && !leaves[1]);
 assert(flushes==before+1);
 before=flushes;
 fail_create=creates+2;
 assert(hal_space_map(HAL_SPACE_SYS,(void *)a,4096,8192,HAL_SPACE_READ)==HAL_ERR_NOMEM);
 assert(!leaves[0] && !leaves[1]);fail_create=0;
 assert(flushes==before+1);
 before=flushes;
 assert(hal_space_map(HAL_SPACE_SYS,(void *)a,12288,4096,HAL_SPACE_READ|HAL_SPACE_EXEC)==HAL_OK);
 assert(flushes==before+1);
 assert(hal_space_unmap(HAL_SPACE_SYS,(void *)a,4096)==HAL_OK);
 before=flushes;
 assert(hal_space_map(HAL_SPACE_SYS,(void *)a,16384,4096,HAL_SPACE_READ)==HAL_OK);
 assert(flushes==before && (leaves[0]&AMD64_PTE_ADDR_MASK)==16384);
 assert(hal_space_unmap(HAL_SPACE_SYS,(void *)a,4096)==HAL_OK);
 assert(hal_space_map(&user,(void *)4096,8192,4096,HAL_SPACE_READ)==HAL_OK && (leaves[0]&AMD64_PTE_USER));
 assert(hal_space_query(&user,(void *)4096,&pa,&flags)==HAL_OK && pa==8192 && (flags&HAL_SPACE_PAGE_PRESENT));
 before=flushes;
 assert(hal_space_prot(&user,(void *)4096,4096,HAL_SPACE_READ)==HAL_OK);
 assert(flushes==before);
 assert(hal_space_prot(&user,(void *)4096,4096,HAL_SPACE_READ|HAL_SPACE_EXEC)==HAL_OK);
 assert(flushes==before+1 && !(leaves[0]&AMD64_PTE_NX));
 assert(hal_space_unmap(&user,(void *)4096,4096)==HAL_OK);
 /* Actual query walker resolves offsets for 1 GiB, 2 MiB and 4 KiB leaves. */
 system_pml4[(a>>39)&511]=(uintptr_t)query_tables[0]|AMD64_PTE_PRESENT;
 for(i=0;i<3;i++) {
  unsigned shift=i==0?30:i==1?21:12;
  memset(query_tables,0,sizeof(query_tables));
  if(i>0)query_tables[0][0]=(uintptr_t)query_tables[1]|AMD64_PTE_PRESENT;
  if(i>1)query_tables[1][0]=(uintptr_t)query_tables[2]|AMD64_PTE_PRESENT;
  query_tables[i][0]=UINT64_C(0x80000000)|AMD64_PTE_PRESENT|AMD64_PTE_DIRTY|(i<2?AMD64_PTE_LARGE:0);
  assert(hal_space_query(HAL_SPACE_SYS,(void *)(a+123),&pa,&flags)==HAL_OK);
  assert(pa==UINT64_C(0x80000000)+123 && (flags&HAL_SPACE_PAGE_DIRTY));
  (void)shift;
 }
 memset(query_tables,0,sizeof(query_tables));
 assert(hal_space_query(HAL_SPACE_SYS,(void *)a,&pa,&flags)==HAL_OK && !flags && !pa);
 ram_active=0;hal_space_get_kernel_range(&lo,&hi);assert(!lo && !hi);
 assert(!locked && flushes>0);
 puts("PASS system/user mapping isolation, permissions, rollback, query page sizes");
}
