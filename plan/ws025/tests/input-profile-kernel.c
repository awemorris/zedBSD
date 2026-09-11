/* Link-only input view operation counts; includes concurrent external calls. */
#include <hal/hal.h>
#include <kern/vmspace.h>
#include <kern/vm-kernel-map.h>
static unsigned active;
static uint64_t counts[4];
static uint64_t operations[4], pages_seen[4];
static uint64_t stamp(uint64_t *frequency)
{
 *frequency=0;
 return 0;
}
static void record(unsigned stage, uint64_t start, uint64_t frequency)
{
 uint64_t count;
 unsigned index;
 (void)start; (void)frequency;
 count=__atomic_add_fetch(&counts[stage],1,__ATOMIC_RELAXED);
 if (stage!=3 || count%256!=0) return;
 for(index=0;index<4;index++)
  hal_printf("INPUT-OPS leases=%llu kind=%u calls=%llu pages=%llu\n",
   (unsigned long long)count,index,
   (unsigned long long)__atomic_load_n(&operations[index],__ATOMIC_RELAXED),
   (unsigned long long)__atomic_load_n(&pages_seen[index],__ATOMIC_RELAXED));
}
static void operation(unsigned kind, size_t size)
{
 if (!__atomic_load_n(&active,__ATOMIC_RELAXED)) return;
 __atomic_add_fetch(&operations[kind],1,__ATOMIC_RELAXED);
 __atomic_add_fetch(&pages_seen[kind],size/4096,__ATOMIC_RELAXED);
}

int __real_vmspace_user_input_lease_acquire(const struct vmspace_pinned_page *pages, size_t count, struct vmspace_user_lease **result);
int __wrap_vmspace_user_input_lease_acquire(const struct vmspace_pinned_page *pages, size_t count, struct vmspace_user_lease **result)
{
 uint64_t start, frequency;
 int error;
 __atomic_store_n(&active,1,__ATOMIC_RELAXED);
 if (!__atomic_load_n(&active,__ATOMIC_RELAXED)) {
  return __real_vmspace_user_input_lease_acquire(pages, count, result);
 }
 start=stamp(&frequency);
 error = __real_vmspace_user_input_lease_acquire(pages, count, result);
 record(0, start, frequency);
 return error;
}

int __real_vm_kernel_map_borrow(struct vm_kernel_map *map, const hal_physaddr_t *pages, size_t count, int writable);
int __wrap_vm_kernel_map_borrow(struct vm_kernel_map *map, const hal_physaddr_t *pages, size_t count, int writable)
{
 uint64_t start, frequency;
 int error;
 if (!__atomic_load_n(&active,__ATOMIC_RELAXED)) {
  return __real_vm_kernel_map_borrow(map, pages, count, writable);
 }
 start=stamp(&frequency);
 error = __real_vm_kernel_map_borrow(map, pages, count, writable);
 record(1, start, frequency);
 return error;
}

int __real_vm_kernel_map_release(struct vm_kernel_map *map);
int __wrap_vm_kernel_map_release(struct vm_kernel_map *map)
{
 uint64_t start, frequency;
 int error;
 if (!__atomic_load_n(&active,__ATOMIC_RELAXED)) {
  return __real_vm_kernel_map_release(map);
 }
 start=stamp(&frequency);
 error = __real_vm_kernel_map_release(map);
 record(2, start, frequency);
 return error;
}

void __real_vmspace_user_lease_release(struct vmspace_user_lease *lease);
void __wrap_vmspace_user_lease_release(struct vmspace_user_lease *lease)
{
 uint64_t start, frequency;
 if (!__atomic_load_n(&active,__ATOMIC_RELAXED)) {
  __real_vmspace_user_lease_release(lease);
  return;
 }
 start=stamp(&frequency);
 __real_vmspace_user_lease_release(lease);
 record(3, start, frequency);
}

int __real_hal_space_map(hal_space_t,void *,hal_physaddr_t,size_t,uint32_t);
int __wrap_hal_space_map(hal_space_t space,void *address,hal_physaddr_t pa,size_t size,uint32_t attr)
{
 if (space==HAL_SPACE_SYS) operation(0,size);
 return __real_hal_space_map(space,address,pa,size,attr);
}
int __real_hal_space_prot(hal_space_t,void *,size_t,uint32_t);
int __wrap_hal_space_prot(hal_space_t space,void *address,size_t size,uint32_t attr)
{
 if (space!=HAL_SPACE_SYS) operation(1,size);
 return __real_hal_space_prot(space,address,size,attr);
}
int __real_hal_space_unmap(hal_space_t,void *,size_t);
int __wrap_hal_space_unmap(hal_space_t space,void *address,size_t size)
{
 operation(space==HAL_SPACE_SYS?2:3,size);
 return __real_hal_space_unmap(space,address,size);
}
