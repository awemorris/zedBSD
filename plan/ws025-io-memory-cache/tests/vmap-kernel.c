#include <kern/vm-kernel-map.h>
static int test_kernel_page_lookup(const void *address, hal_physaddr_t *physical)
{
 uint32_t flags;int error=hal_space_query(HAL_SPACE_SYS,(void *)address,physical,&flags);
 return error!=HAL_OK?error:(flags&HAL_SPACE_PAGE_PRESENT)?HAL_OK:HAL_ERR_INVALID;
}
/* Link-only probe: actual owned pages, shared page tables and SMP shootdowns. */
#include <hal/hal.h>
#include <kern/io-scratch.h>
#include "src/hal/amd64/space.h"

#define REQUIRE(x) do { if (!(x)) HAL_FATAL("VMAP probe: " #x); } while (0)
static volatile unsigned probe_active;
static volatile unsigned acknowledgements[HAL_CPU_MAX];
static volatile unsigned char *probe_address;
static unsigned char probe_value;
static unsigned allocation_index;
static int allocating;
void __real_io_pool_init(void);
void __real_kernel_cpu_notify_handler(hal_cpu_id_t, hal_irq_ack_t);
int __real_hal_pmem_alloc_range(hal_physaddr_t, size_t, size_t, uint32_t, uint32_t,uint64_t,uint64_t,uint64_t,struct hal_pmem *);

static unsigned force_fragmentation;
int __real_hal_pmem_alloc(hal_physaddr_t, size_t, size_t, uint32_t, uint32_t,struct hal_pmem *);
int __real_io_scratch_alloc(size_t,int,struct io_scratch *);
int __wrap_hal_pmem_alloc(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr,struct hal_pmem *memory)
{
 if(force_fragmentation && request_size>=65536)return HAL_ERR_NOMEM;
 return __real_hal_pmem_alloc(request_paddr, request_size, request_alignment, request_type, request_attr,memory);
}
int __wrap_io_scratch_alloc(size_t size,int allow,struct io_scratch *scratch)
{
 int error;
 force_fragmentation=allow;error=__real_io_scratch_alloc(size,allow,scratch);force_fragmentation=0;
 if(error==HAL_OK && allow && size>=65536){
  REQUIRE(scratch->mapping!=NULL && scratch->physical.size==0);
  hal_printf("VMAP SCRATCH PASS size=%llu\n",(unsigned long long)scratch->size);
 }
 return error;
}

static unsigned table_failure,table_calls;
void *__real_hal_malloc(size_t);
void *__wrap_hal_malloc(size_t size)
{
 if(allocating && size==sizeof(struct amd64_table_page) && ++table_calls==table_failure)return NULL;
 return __real_hal_malloc(size);
}

int
__wrap_hal_pmem_alloc_range(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr,
    uint64_t minimum,uint64_t maximum,uint64_t boundary,struct hal_pmem *memory)
{
 if(allocating){minimum=UINT64_C(0x110000000)+(uint64_t)allocation_index++*65536;maximum=minimum+4095;}
 return __real_hal_pmem_alloc_range(request_paddr, request_size, request_alignment, request_type, request_attr,minimum,maximum,boundary,memory);
}

void
__wrap_kernel_cpu_notify_handler(hal_cpu_id_t cpu,hal_irq_ack_t ack)
{
 if(__atomic_load_n(&probe_active,__ATOMIC_ACQUIRE)){
  REQUIRE(probe_address[0]==probe_value && probe_address[65535]==probe_value);
  __atomic_store_n(&acknowledgements[cpu],1U,__ATOMIC_RELEASE);
 }
 __real_kernel_cpu_notify_handler(cpu,ack);
}

void
__wrap_io_pool_init(void)
{
 struct vm_kernel_map *map;struct hal_pmem_stats before,after;
 hal_space_t first,second,new_space;hal_physaddr_t pa,previous;
 unsigned round,index,timeout;void *address;
 void *previous_address=NULL;hal_physaddr_t previous_first=0;
 /* Fail each real subordinate table allocation before any vmap table exists. */
 for(table_failure=1;table_failure<=2;table_failure++){
  hal_pmem_get_stats(&before);REQUIRE(vm_kernel_map_reserve(65536,&map)==HAL_OK);
  table_calls=0;allocating=1;REQUIRE(vm_kernel_map_populate(map,0,UINT64_MAX)==HAL_ERR_NOMEM);allocating=0;
  REQUIRE(vm_kernel_map_release(map)==HAL_OK);
  hal_pmem_get_stats(&after);REQUIRE(before.physical_free==after.physical_free);
  hal_printf("VMAP TABLE ROLLBACK PASS step=%u\n",table_failure);
 }
 table_failure=0;
 first=hal_space_create();second=hal_space_create();REQUIRE(first && second);
 for(round=0;round<3;round++){
  hal_pmem_get_stats(&before);
  REQUIRE(vm_kernel_map_reserve(65536,&map)==HAL_OK);
  hal_pmem_get_stats(&after);REQUIRE(before.physical_free==after.physical_free);
  allocating=1;REQUIRE(vm_kernel_map_populate(map,0,UINT64_MAX)==HAL_OK);allocating=0;
  REQUIRE(vm_kernel_map_pin(map,&address)==HAL_OK);
  if(round!=0)REQUIRE(address==previous_address);
  REQUIRE(test_kernel_page_lookup(address,&pa)==HAL_OK);
  if(round!=0)REQUIRE(pa!=previous_first);
  previous_address=address;previous_first=pa;
  REQUIRE(vm_kernel_map_release(map)==HAL_ERR_BUSY);
  previous=0;
  for(index=0;index<16;index++){
   REQUIRE(test_kernel_page_lookup((char *)address+index*4096,&pa)==HAL_OK);
   REQUIRE(pa>UINT32_MAX && pa!=previous+4096);previous=pa;
  }
  probe_address=address;probe_value=(unsigned char)(0x31+round);
  hal_memset(address,probe_value,65536);
  hal_space_switch(first);REQUIRE(probe_address[0]==probe_value);
  hal_space_switch(second);REQUIRE(probe_address[65535]==probe_value);
  hal_space_switch(HAL_SPACE_SYS);
  new_space=hal_space_create();REQUIRE(new_space!=NULL);
  hal_space_switch(new_space);REQUIRE(probe_address[4096]==probe_value);
  hal_space_switch(HAL_SPACE_SYS);hal_space_destroy(new_space);
  for(index=0;index<HAL_CPU_MAX;index++)acknowledgements[index]=0;
  __atomic_store_n(&probe_active,1U,__ATOMIC_RELEASE);
  for(index=1;index<hal_cpu_count();index++)REQUIRE(hal_cpu_notify(index)==HAL_OK);
  for(timeout=0;timeout<100000000U;timeout++){
   for(index=1;index<hal_cpu_count();index++)if(!__atomic_load_n(&acknowledgements[index],__ATOMIC_ACQUIRE))break;
   if(index==hal_cpu_count())break;
   hal_compiler_barrier();
  }
  REQUIRE(index==hal_cpu_count());__atomic_store_n(&probe_active,0U,__ATOMIC_RELEASE);
  vm_kernel_map_unpin(map);REQUIRE(vm_kernel_map_release(map)==HAL_OK);
  REQUIRE(test_kernel_page_lookup(address,&pa)==HAL_ERR_INVALID);
  hal_pmem_get_stats(&after);REQUIRE(before.physical_free==after.physical_free);
  hal_printf("VMAP PASS round=%u pages=16 high=1 fragmented=1 cpus=%u free=%llu\n",round,hal_cpu_count(),(unsigned long long)after.physical_free);
 }
 hal_space_destroy(first);hal_space_destroy(second);
 __real_io_pool_init();
}
