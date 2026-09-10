/* Common VM mapping owner, with controlled generic HAL boundaries. */
#include <kern/vm-kernel-map.h>
#include <kern/io-scratch.h>
#include <kern/lock.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#define PAGE_SIZE 4096U
#define BASE ((uintptr_t)0x40000000)
#define CHECK(x) do { if(!(x)){fprintf(stderr,"map:%d %s\n",__LINE__,#x);abort();} } while(0)
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned locked,live,allocations,map_calls,fail_alloc,fail_map,unmaps,supported=1;
static unsigned contiguous;
static hal_physaddr_t entries[8192];
static unsigned attributes[8192];
unsigned long spin_lock_irqsave(struct spinlock *l) {(void)l;CHECK(!pthread_mutex_lock(&mutex));locked=1;return 0;}
void spin_unlock_irqrestore(struct spinlock *l,unsigned long irq) {(void)l;(void)irq;locked=0;CHECK(!pthread_mutex_unlock(&mutex));}
void hal_fatal(const char *f,int l,const char *m) {fprintf(stderr,"%s:%d %s\n",f,l,m);abort();}
void __attribute__((weak)) hal_space_get_kernel_range(uintptr_t *lo,uintptr_t *hi) {*lo=supported?BASE:0;*hi=supported?BASE+8192*4096:0;}
size_t hal_space_get_page_size(int level) {return level==1?4096:0;}
int hal_pmem_alloc(hal_physaddr_t pa,size_t size,size_t alignment,uint32_t type,uint32_t attr,struct hal_pmem *m)
{(void)pa;(void)size;(void)alignment;(void)type;(void)attr;(void)m;return HAL_ERR_NOMEM;}
int hal_pmem_alloc_range(hal_physaddr_t pa,size_t size,size_t alignment,uint32_t type,uint32_t attr,uint64_t lo,uint64_t hi,uint64_t boundary,struct hal_pmem *m)
{
 CHECK(!locked && pa==HAL_PMEM_PADDR_ANY && size==4096 && alignment==4096 && type==HAL_PMEM_TYPE_RAM && !attr && !boundary);
 if(++allocations==fail_alloc)return HAL_ERR_NOMEM;
 m->paddr=UINT64_C(0x100000000)+allocations*(contiguous?4096:8192);CHECK(m->paddr>=lo && m->paddr+4095<=hi);
 m->vaddr=malloc(4096);CHECK(m->vaddr);m->size=4096;m->type=type;m->attr=0;live++;return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *m)
{
 unsigned i;CHECK(!locked && live);
 for(i=0;i<8192;i++)CHECK(entries[i]!=m->paddr);
 live--;free(m->vaddr);memset(m,0,sizeof(*m));return HAL_OK;
}
int hal_space_map(hal_space_t s,void *v,hal_physaddr_t p,size_t size,uint32_t attr)
{
 size_t i=((uintptr_t)v-BASE)/4096,n;CHECK(!locked && s==HAL_SPACE_SYS && size && size%4096==0 && i+size/4096<=8192);
 for(n=0;n<size/4096;n++)CHECK(!entries[i+n]);
 if(++map_calls==fail_map)return HAL_ERR_NOMEM;
 if(p%4096)return HAL_ERR_INVALID;
 for(n=0;n<size/4096;n++){entries[i+n]=p+n*4096;attributes[i+n]=attr;}return HAL_OK;
}
int hal_space_unmap(hal_space_t s,void *v,size_t size)
{
 size_t i=((uintptr_t)v-BASE)/4096,n;CHECK(!locked && s==HAL_SPACE_SYS && size%4096==0 && i+size/4096<=8192);
 for(n=0;n<size/4096;n++)entries[i+n]=attributes[i+n]=0;
 unmaps++;return HAL_OK;
}

int main(void)
{
 struct vm_kernel_map *m,*slots[128];struct io_scratch scratch;
 hal_physaddr_t pages[16];void *address;unsigned i,j,before;
 supported=0;CHECK(vm_kernel_map_reserve(65536,&m)==HAL_ERR_UNSUPPORTED);supported=1;
 for(i=0;i<16;i++)pages[i]=UINT64_C(0x200000000)+i*8192;
 for(i=1;i<=16;i++) {
  CHECK(vm_kernel_map_reserve(65536,&m)==HAL_OK);fail_alloc=allocations+i;
  CHECK(vm_kernel_map_populate(m,0,UINT64_MAX)==HAL_ERR_NOMEM && !live);
  fail_alloc=0;CHECK(vm_kernel_map_release(m)==HAL_OK);
  CHECK(vm_kernel_map_reserve(65536,&m)==HAL_OK);fail_map=map_calls+i;
  CHECK(vm_kernel_map_populate(m,0,UINT64_MAX)==HAL_ERR_NOMEM && !live);
  fail_map=0;CHECK(vm_kernel_map_release(m)==HAL_OK);
  CHECK(vm_kernel_map_reserve(65536,&m)==HAL_OK);before=allocations;fail_map=map_calls+i;
  CHECK(vm_kernel_map_borrow(m,pages,16,0)==HAL_ERR_NOMEM && allocations==before && !live);
  fail_map=0;CHECK(vm_kernel_map_borrow(m,pages,16,0)==HAL_OK);
  CHECK(vm_kernel_map_pin(m,&address)==HAL_OK);
  for(j=0;j<16;j++)CHECK(entries[((uintptr_t)address-BASE)/4096+j]==pages[j] && attributes[((uintptr_t)address-BASE)/4096+j]==HAL_SPACE_READ);
  CHECK(vm_kernel_map_release(m)==HAL_ERR_BUSY);vm_kernel_map_unpin(m);CHECK(vm_kernel_map_release(m)==HAL_OK);
 }
 CHECK(vm_kernel_map_reserve(65536,&m)==HAL_OK);
 CHECK(vm_kernel_map_borrow(m,NULL,16,1)==HAL_ERR_INVALID);
 CHECK(vm_kernel_map_borrow(m,pages,15,1)==HAL_ERR_INVALID);
 CHECK(vm_kernel_map_borrow(m,pages,16,2)==HAL_ERR_INVALID);
 pages[5]++;CHECK(vm_kernel_map_borrow(m,pages,16,1)==HAL_ERR_INVALID);pages[5]--;
 CHECK(vm_kernel_map_borrow(m,pages,16,1)==HAL_OK);
 CHECK(vm_kernel_map_pin(m,&address)==HAL_OK && (attributes[((uintptr_t)address-BASE)/4096]&HAL_SPACE_WRITE));
 vm_kernel_map_unpin(m);CHECK(vm_kernel_map_release(m)==HAL_OK);
 /* One contiguous owned vector publishes through one HAL call. */
 contiguous=1;before=map_calls;
 CHECK(vm_kernel_map_reserve(65536,&m)==HAL_OK);
 CHECK(vm_kernel_map_populate(m,0,UINT64_MAX)==HAL_OK && map_calls==before+1);
 CHECK(vm_kernel_map_release(m)==HAL_OK && !live);contiguous=0;
 /* Borrowed contiguous pages retain one call and exact PA translations. */
 for(i=0;i<16;i++)pages[i]=UINT64_C(0x200000000)+i*4096;
 CHECK(vm_kernel_map_reserve(65536,&m)==HAL_OK);before=map_calls;
 CHECK(vm_kernel_map_borrow(m,pages,16,0)==HAL_OK && map_calls==before+1);
 CHECK(vm_kernel_map_pin(m,&address)==HAL_OK);
 for(j=0;j<16;j++)CHECK(entries[((uintptr_t)address-BASE)/4096+j]==pages[j]);
 vm_kernel_map_unpin(m);CHECK(vm_kernel_map_release(m)==HAL_OK);
 /* A repeated physical run must not be merged; later failure retires prefix. */
 for(i=8;i<16;i++)pages[i]=pages[i-8];
 CHECK(vm_kernel_map_reserve(65536,&m)==HAL_OK);fail_map=map_calls+2;
 CHECK(vm_kernel_map_borrow(m,pages,16,1)==HAL_ERR_NOMEM);
 for(j=0;j<8192;j++)CHECK(!entries[j]);
 fail_map=0;before=map_calls;
 CHECK(vm_kernel_map_borrow(m,pages,16,1)==HAL_OK && map_calls==before+2);
 CHECK(vm_kernel_map_release(m)==HAL_OK);
 for(i=0;i<128;i++)CHECK(vm_kernel_map_reserve(4096,&slots[i])==HAL_OK);
 CHECK(vm_kernel_map_reserve(4096,&m)==HAL_ERR_NOMEM);
 for(i=0;i<128;i++)CHECK(vm_kernel_map_release(slots[i])==HAL_OK);
 CHECK(io_scratch_alloc(65536,1,&scratch)==HAL_OK && live==16);
 CHECK(io_scratch_free(&scratch)==HAL_OK && !live);
 for(i=0;i<8192;i++)CHECK(!entries[i]);
 CHECK(unmaps>0 && !locked);
 puts("PASS common kernel map: allocation/map failure, borrowed ownership, pins, slots, scratch");
}
