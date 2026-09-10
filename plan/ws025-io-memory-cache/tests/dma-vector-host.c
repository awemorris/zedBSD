#include <kern/vm-kernel-map.h>
/* Actual DMA vector owner/accounting; controlled optional vmap boundary. */
#define DMA_CACHE_MAIN retained_dma_accounting_main
#include "cache-dma-host.c"
struct vm_kernel_map { void *bytes;size_t size;unsigned pins,populated; };
static struct vm_kernel_map *live_map;
static unsigned capability=1,fail_reserve,fail_populate,fail_lookup,fail_release,shape;
unsigned vm_kernel_map_capabilities(void) { return capability; }
int vm_kernel_map_reserve(size_t size,struct vm_kernel_map **result)
{
 *result=NULL;if(fail_reserve)return HAL_ERR_NOMEM;assert(!live_map);
 live_map=calloc(1,sizeof(*live_map));assert(live_map);live_map->size=size;
 *result=live_map;return HAL_OK;
}
int vm_kernel_map_populate(struct vm_kernel_map *map,uint64_t low,uint64_t high)
{
 (void)low;(void)high;if(fail_populate)return HAL_ERR_NOMEM;
 map->bytes=aligned_alloc(4096,map->size);assert(map->bytes);map->populated=1;return HAL_OK;
}
int vm_kernel_map_pin(struct vm_kernel_map *map,void **address)
{ assert(map->populated);map->pins++;*address=map->bytes;return HAL_OK; }
void vm_kernel_map_unpin(struct vm_kernel_map *map) { assert(map->pins);map->pins--; }
int vm_kernel_map_release(struct vm_kernel_map *map)
{
 if(fail_release || map->pins)return HAL_ERR_BUSY;
 assert(map==live_map);free(map->bytes);free(map);live_map=NULL;return HAL_OK;
}
int hal_space_query(hal_space_t space,void *address,hal_physaddr_t *physical,uint32_t *flags)
{
 assert(space==HAL_SPACE_SYS);*flags=HAL_SPACE_PAGE_PRESENT;
 uintptr_t offset=(uintptr_t)address-(uintptr_t)live_map->bytes;
 assert(offset<live_map->size);
 if(fail_lookup && offset>=4096)return HAL_ERR_INVALID;
 *physical=(shape==2?UINT64_C(0x200000):UINT64_C(0x100001000))+
     (offset/4096)*(shape==1?4096:8192)+offset%4096;
 return HAL_OK;
}
static void reset_pools(void)
{
 unsigned i;
 for(i=0;i<2;i++)assert(amd64_pmem_extent_init(&pools[i],i?UINT64_C(0x100000000):0x100000,
     512*4096,metadata[i],sizeof(metadata[i]))==AMD64_PMEM_OK);
}
int main(void)
{
 struct drv_dma_constraints constraints={64,65536,65536,1};
 struct drv_dma_device *device;struct drv_dma_vector *vector;
 struct drv_dma_segment segment;struct cache_memory_stats stats;
 unsigned test,index;size_t total;
 reset_pools();cache_memory_init();
 for(test=0;test<9;test++){
  capability=test!=2;fail_reserve=test==3;fail_populate=test==4;fail_lookup=test==5;
  shape=test==1?1:(test==8?2:0);constraints.address_bits=test>=7?32:64;
  assert(drv_dma_device_create(&constraints,&device)==0);
  assert(drv_dma_vector_create(device,0,&vector)==EINVAL && vector==NULL);
  assert(drv_dma_vector_create(device,65537,&vector)==EINVAL && vector==NULL);
  assert(drv_dma_vector_create(device,65536,&vector)==0);
  assert(drv_dma_vector_address(vector)!=NULL);
  assert(drv_dma_vector_count(vector)==((test==0 || test==6 || test==8)?16U:(test==1?2U:1U)));
  total=0;
  for(index=0;index<drv_dma_vector_count(vector);index++){
   assert(drv_dma_vector_segment(vector,index,&segment)==0);
   assert(segment.length && segment.length<=65536 && segment.length<=65536-segment.address%65536);
   if(test>=7)assert(segment.address+segment.length-1<=UINT32_MAX);
   total+=segment.length;
  }
  assert(total==65536);assert(drv_dma_vector_segment(vector,index,&segment)==EINVAL);
  cache_memory_get_stats(&stats);assert(stats.resident_bytes>65536 && stats.pending_bytes==0);
  assert(drv_dma_device_destroy(device)==EBUSY);
  if(test==6){fail_release=1;assert(drv_dma_vector_free(vector)==EBUSY);assert(live_map->pins==1);fail_release=0;}
  assert(drv_dma_vector_free(vector)==0 && live_map==NULL);
  assert(drv_dma_device_destroy(device)==0);
  cache_memory_get_stats(&stats);assert(stats.resident_bytes==0 && stats.pending_bytes==0);
 }
 capability=1;shape=0;constraints=(struct drv_dma_constraints){64,512,512,1};
 assert(drv_dma_device_create(&constraints,&device)==0);
 assert(drv_dma_vector_create(device,65536,&vector)!=0 && vector==NULL && !live_map);
 assert(drv_dma_device_destroy(device)==0);
 constraints.coherent=0;assert(drv_dma_device_create(&constraints,&device)==0);
 assert(drv_dma_vector_create(device,4096,&vector)==EOPNOTSUPP);
 assert(drv_dma_device_destroy(device)==0);
 puts("DMA vector PASS: high scatter, merged boundary, mask fallback, failed preparation/retirement, accounting");
 return 0;
}
