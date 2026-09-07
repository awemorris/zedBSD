/* Real vmap owner with deterministic allocation/table/retirement boundaries. */
#include <hal/hal.h>
#include <kern/io-scratch.h>
#include "src/hal/amd64/defs.h"
#include "src/hal/amd64/space.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"vmap:%u: %s\n",__LINE__,#x);abort();}}while(0)
static unsigned checks,allocations,live,allocation_failure,walks,walk_failure,flushes,freed_after;
static int ram_active=1,ram_builder;
static uint64_t system_pml4[512],leaves[128*64];
static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static bool space_lock_enter(struct amd64_space *space)
{ (void)space;CHECK(pthread_mutex_lock(&table_lock)==0);return 1; }
static void space_lock_leave(struct amd64_space *space,bool enabled)
{ (void)space;(void)enabled;CHECK(pthread_mutex_unlock(&table_lock)==0); }
void *hal_memset(void *p,int byte,size_t n) { return memset(p,byte,n); }
int hal_pmem_alloc_range(const struct hal_pmem_request *request,uint64_t low,uint64_t high,uint64_t boundary,struct hal_pmem *memory)
{
 (void)boundary;allocations++;
 if(allocations==allocation_failure)return HAL_ERR_NOMEM;
 CHECK(request->size==4096);memory->paddr=UINT64_C(0x100000000)+(uint64_t)allocations*8192;
 CHECK(memory->paddr>=low && memory->paddr+4095<=high);
 memory->vaddr=malloc(4096);CHECK(memory->vaddr);memory->size=4096;live++;return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *memory)
{ CHECK(live>0);if(memory->paddr>UINT32_MAX)CHECK(flushes>freed_after);live--;free(memory->vaddr);memset(memory,0,sizeof(*memory));return HAL_OK; }
static int contiguous_fail=1;
size_t hal_page_get_page_size(int level) { (void)level;return 4096; }
int hal_pmem_alloc(const struct hal_pmem_request *request,struct hal_pmem *memory)
{
 if(contiguous_fail)return HAL_ERR_NOMEM;
 memory->paddr=4096;memory->size=request->size;memory->vaddr=malloc(request->size);
 CHECK(memory->vaddr);live++;return HAL_OK;
}
static int amd64_ram_lookup(void *builder,uint64_t physical,uint64_t *entry)
{ (void)builder;CHECK(physical>=UINT64_C(0x100000000));*entry=AMD64_PTE_PRESENT|AMD64_PTE_WRITE;return 1; }
static uint64_t *walk_leaf(struct amd64_space *space,uintptr_t address,int create)
{
 size_t index=(address-UINT64_C(0xffffc00000000000))/4096;(void)space;
 CHECK(index<128*64);if(create && ++walks==walk_failure)return NULL;return &leaves[index];
}
static struct amd64_table_page *detach_empty_tables(struct amd64_space *space)
{ (void)space;return NULL; }
static void free_detached_tables(struct amd64_table_page *page) { CHECK(page==NULL); }
static void shootdown(hal_space_t space,void *address,size_t size)
{ (void)address;(void)size;CHECK(space==HAL_SPACE_SYS);flushes++; }
uintptr_t amd64_image_to_phys(const void *p) { (void)p;return UINTPTR_MAX; }
uintptr_t amd64_direct_to_phys(const void *p) { (void)p;return UINTPTR_MAX; }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"fatal %s:%d %s\n",file,line,message);abort(); }
#include "src/hal/amd64/space-vmap.inc"
int main(void)
{
 struct hal_vmap *map,*slots[128];void *address;hal_physaddr_t physical,previous;
 struct io_scratch scratch;void *extra;unsigned i,j,before;
 CHECK(hal_vmap_reserve(0,&map)==HAL_ERR_INVALID);
 CHECK(hal_vmap_reserve(65537,&map)==HAL_ERR_INVALID);
 CHECK(hal_vmap_reserve(1,&map)==HAL_ERR_INVALID);
 ram_active=0;CHECK(hal_vmap_reserve(4096,&map)==HAL_ERR_UNSUPPORTED);ram_active=1;
 for(i=0;i<128;i++)CHECK(hal_vmap_reserve(65536,&slots[i])==HAL_OK);
 CHECK(live==0 && allocations==0 && walks==0);
 CHECK(hal_vmap_reserve(4096,&map)==HAL_ERR_NOMEM);
 for(i=0;i<128;i++)CHECK(hal_vmap_release(slots[i])==HAL_OK);
 for(i=1;i<=33;i++) {
  CHECK(hal_vmap_reserve(65536,&map)==HAL_OK);freed_after=flushes;
  if(i<=16)allocation_failure=allocations+i;
  else if(i<=32)walk_failure=walks+i-16;
  before=flushes;
  CHECK(hal_vmap_populate(map,UINT64_C(0x100000000),UINT64_MAX)==(i==33?HAL_OK:HAL_ERR_NOMEM));
  CHECK(flushes>before);allocation_failure=walk_failure=0;
  if(i!=33){CHECK(live==0);CHECK(hal_vmap_populate(map,UINT64_C(0x100000000),UINT64_MAX)==HAL_OK);}
  CHECK(live==16);CHECK(hal_vmap_pin(map,&address)==HAL_OK);
  CHECK(hal_vmap_release(map)==HAL_ERR_BUSY);
  previous=0;
  for(j=0;j<16;j++){
   CHECK(hal_kernel_page_lookup((char *)address+j*4096+31,&physical)==HAL_OK);
   CHECK(physical>UINT32_MAX && physical!=previous+4096);previous=physical;
   CHECK((leaves[((uintptr_t)address-VMAP_BASE)/4096+j]&AMD64_PTE_USER)==0);
   CHECK(leaves[((uintptr_t)address-VMAP_BASE)/4096+j]&AMD64_PTE_NX);
  }
  CHECK(hal_kernel_page_lookup((char *)address+65536,&physical)==HAL_ERR_INVALID);
  hal_vmap_unpin(map);freed_after=flushes;CHECK(hal_vmap_release(map)==HAL_OK);
  CHECK(live==0);CHECK(hal_kernel_page_lookup(address,&physical)==HAL_ERR_INVALID);
 }
 /* Scratch owner keeps a pin until drain, and restores it on busy release. */
 CHECK(io_scratch_alloc(4096,0,&scratch)==HAL_ERR_NOMEM);
 contiguous_fail=0;CHECK(io_scratch_alloc(4097,0,&scratch)==HAL_OK);
 CHECK(scratch.size==8192 && scratch.mapping==NULL);
 CHECK(io_scratch_free(&scratch)==HAL_OK && live==0);contiguous_fail=1;
 CHECK(io_scratch_alloc(65536+4096,1,&scratch)==HAL_OK);
 CHECK(scratch.size==69632 && scratch.mapping!=NULL && scratch.physical.size==0);
 CHECK(hal_vmap_pin(scratch.mapping,&extra)==HAL_OK);
 CHECK(io_scratch_free(&scratch)==HAL_ERR_BUSY && scratch.vaddr==extra);
 hal_vmap_unpin(scratch.mapping);freed_after=flushes;
 CHECK(io_scratch_free(&scratch)==HAL_OK && live==0 && scratch.size==0);
 allocation_failure=allocations+3;freed_after=flushes;
 CHECK(io_scratch_alloc(65536,1,&scratch)==HAL_ERR_NOMEM && live==0);
 allocation_failure=0;
 printf("vmap reservation/vector/failure/pin/retirement: PASS (%u checks)\n",checks);
 return 0;
}
