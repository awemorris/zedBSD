/* Real lease and backing functions, controlled VM locks and HAL retirement. */
#define main private_upgrade_main
#include "private-upgrade-host.c"
#undef main
#include <kern/kmem.h>

static pthread_mutex_t metadata = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t vm_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned metadata_depth,vm_depth,unmaps,fail_unmap,alloc_live;
static int fail_alloc;
static unsigned protections, fail_protection, unmapped_pages;
static unsigned restore_enabled, restores, restore_pages, restore_fail;
static struct vm_page *test_aliases;
static unsigned test_alias_count;
void vm_metadata_enter(void)
{ CHECK(pthread_mutex_lock(&metadata)==0);metadata_depth++; }
void vm_metadata_leave(void)
{ CHECK(metadata_depth==1);metadata_depth--;CHECK(pthread_mutex_unlock(&metadata)==0); }
void mutex_lock(struct mutex *m)
{ (void)m;CHECK(pthread_mutex_lock(&vm_mutex)==0);vm_depth++; }
void mutex_unlock(struct mutex *m)
{ (void)m;CHECK(vm_depth==1);vm_depth--;CHECK(pthread_mutex_unlock(&vm_mutex)==0); }
void *kern_calloc(size_t n,size_t size)
{ void *p;CHECK(!metadata_depth && !vm_depth);if(fail_alloc)return NULL;p=calloc(n,size);CHECK(p);alloc_live++;return p; }
void kern_free(void *p)
{ CHECK(!metadata_depth && !vm_depth && alloc_live);alloc_live--;free(p); }
void vmspace_put(struct vmspace *vm)
{ CHECK(!metadata_depth && !vm_depth);CHECK(!refcount_put(&vm->refs)); }
int hal_space_unmap(hal_space_t space,void *address,size_t size)
{
 unsigned i;
 (void)space;(void)address;CHECK(size!=0 && size%4096==0);
 CHECK(!metadata_depth && !vm_depth);
 for(i=0;i<test_alias_count;i++) {
  CHECK(test_aliases[i].flags&VM_MAPPING_BUSY);
  CHECK(test_aliases[i].region->hold_count!=0);
 }
 unmaps++;
 if(unmaps!=fail_unmap)unmapped_pages+=size/4096;
 return unmaps==fail_unmap?HAL_ERR_STATE:HAL_OK;
}
int hal_space_map(hal_space_t space,void *address,hal_physaddr_t pa,size_t size,uint32_t prot)
{
 unsigned i,j;
 (void)space;
 CHECK(!metadata_depth && !vm_depth);
 if(!restore_enabled)return HAL_ERR_NOMEM;
 restores++;
 for(i=0;i<test_alias_count;i++) {
  CHECK(test_aliases[i].flags&VM_MAPPING_BUSY);
  CHECK(test_aliases[i].region->hold_count!=0);
 }
 for(j=0;j<size/4096;j++) {
  for(i=0;i<test_alias_count;i++)if(test_aliases[i].address==(uintptr_t)address+j*4096)break;
  CHECK(i<test_alias_count);
  CHECK(!(test_aliases[i].flags&VM_MAPPING_MAPPED));
  CHECK(test_aliases[i].private_page->pmem.paddr==pa+j*4096);
  CHECK(prot==(test_aliases[i].region->prot & ((test_aliases[i].flags&VM_MAPPING_COW)?~(uint32_t)HAL_SPACE_WRITE:~0U)));
 }
 if(restores==restore_fail)return HAL_ERR_NOMEM;
 restore_pages+=size/4096;
 return HAL_OK;
}
int hal_space_prot(hal_space_t s,void *a,size_t n,uint32_t p)
{
 unsigned i;
 (void)s;(void)a;CHECK(n==4096 && p==HAL_SPACE_READ);
 CHECK(!metadata_depth && !vm_depth);
 for(i=0;i<test_alias_count;i++) {
  CHECK(test_aliases[i].flags&VM_MAPPING_BUSY);
  CHECK(test_aliases[i].region->hold_count!=0);
 }
 protections++;
 return protections==fail_protection?HAL_ERR_STATE:HAL_OK;
}
static void setup(struct vm_private_page *p,struct vmspace *vm,
    struct vm_region *r,struct vm_page *a,struct vmspace_pinned_page *pins,
    unsigned aliases,unsigned char *bytes)
{
 unsigned i;
 memset(p,0,2*sizeof(*p));memset(vm,0,2*sizeof(*vm));
 memset(r,0,2*sizeof(*r));memset(a,0,65*sizeof(*a));
 memset(pins,0,3*sizeof(*pins));
 for(i=0;i<2;i++) {
  vm_private_page_init(&p[i]);p[i].flags=VM_PAGE_RESIDENT;
  p[i].pmem.vaddr=bytes+i*4096;p[i].pmem.size=4096;p[i].pmem.paddr=4096*(i+1);
  refcount_init(&vm[i].refs,1);r[i].prot=HAL_SPACE_READ|HAL_SPACE_WRITE;
  vm[i].regions=&r[i];
 }
 for(i=0;i<aliases;i++) {
  a[i].vm=&vm[i%2];a[i].region=&r[i%2];a[i].address=4096*(i+1);
  a[i].flags=VM_MAPPING_MAPPED|VM_MAPPING_COW;a[i].private_page=&p[0];
  if(i+1<aliases)a[i].private_next=&a[i+1];
 }
 p[0].mappings=aliases?a:NULL;p[0].mapping_count=aliases;
 for(i=0;i<3;i++) {
  pins[i].kind=VMSPACE_PINNED_PRIVATE;pins[i].owner.private_page=&p[i==2];
  CHECK(vm_private_page_pin(pins[i].owner.private_page,&pins[i].memory)==0);
 }
 test_aliases=a;test_alias_count=aliases;unmaps=fail_unmap=unmapped_pages=0;
}
static void balanced(struct vm_private_page *p,struct vmspace *vm,
    struct vm_region *r,struct vm_page *a,unsigned aliases)
{
 unsigned i;
 CHECK(!alloc_live && !metadata_depth && !vm_depth);
 CHECK(p[0].pin_count==2 && p[1].pin_count==1);
 CHECK(refcount_load(&p[0].refs)==3 && refcount_load(&p[1].refs)==2);
 for(i=0;i<2;i++) {
  CHECK(!(p[i].flags&VM_PAGE_BUSY));CHECK(r[i].hold_count==0);
  CHECK(refcount_load(&vm[i].refs)==1);
 }
 for(i=0;i<aliases;i++)CHECK(!(a[i].flags&VM_MAPPING_BUSY));
 vm_private_page_unpin(&p[0]);vm_private_page_unpin(&p[0]);vm_private_page_unpin(&p[1]);
}
int main(void)
{
 struct vm_private_page p[2];struct vmspace vm[2];struct vm_region r[2];
 struct vm_page a[65];struct vmspace_pinned_page pins[3];
 struct vmspace_user_lease *lease;unsigned char bytes[8192];
 unsigned i,j;
 memset(bytes,0x5a,sizeof(bytes));
 CHECK(vmspace_user_lease_acquire(NULL,0,&lease)==EINVAL && lease==NULL);
 for(i=0;i<12;i++) {
  setup(p,vm,r,a,pins,4,bytes);
  if(i==0)fail_alloc=1;
  if(i==1)pins[2].kind=VMSPACE_PINNED_OBJECT;
  if(i==2)pins[2].memory.paddr++;
  if(i==3)p[1].pin_count++;
  if(i==4)a[3].flags|=VM_MAPPING_BUSY;
  if(i==5)refcount_init(&vm[1].refs,0);
  if(i>=6 && i<=9)fail_unmap=i-5;
  if(i==10)a[1].flags&=~VM_MAPPING_MAPPED;
  if(i<10) {
   CHECK(vmspace_user_lease_acquire(pins,3,&lease)!=0 && lease==NULL);
   if(i<6)CHECK(unmaps==0);
   if(i>=6)for(j=0;j<i-6;j++)CHECK(!(a[j].flags&VM_MAPPING_MAPPED));
  } else {
   CHECK(vmspace_user_lease_acquire(pins,3,&lease)==0 && lease);
   CHECK(unmaps==(i==10?3:4));
   for(j=0;j<4;j++) {
    CHECK(!(a[j].flags&VM_MAPPING_MAPPED));
    CHECK((a[j].flags&(VM_MAPPING_BUSY|VM_MAPPING_COW))==(VM_MAPPING_BUSY|VM_MAPPING_COW));
   }
   CHECK(p[0].flags&VM_PAGE_DIRTY);
   CHECK(vm_private_page_io_try_upgrade(&p[0],2)==EBUSY);
   vmspace_user_lease_release(lease);
  }
  fail_alloc=0;
  if(i==3)p[1].pin_count--;
  if(i==4)a[3].flags&=~VM_MAPPING_BUSY;
  if(i==5)refcount_init(&vm[1].refs,1);
  balanced(p,vm,r,a,4);
 }
 setup(p,vm,r,a,pins,65,bytes);
 CHECK(vmspace_user_lease_acquire(pins,3,&lease)==EBUSY && lease==NULL);
 CHECK(unmaps==0);balanced(p,vm,r,a,65);
 /* Exact alias capacity and detached-but-pinned backing both remain valid. */
 for(i=0;i<=64;i+=64) {
  setup(p,vm,r,a,pins,i,bytes);
  CHECK(vmspace_user_lease_acquire(pins,3,&lease)==0 && lease);
  CHECK(unmaps==i);vmspace_user_lease_release(lease);
  balanced(p,vm,r,a,i);
 }
 setup(p,vm,r,a,pins,4,bytes);
 r[1].hold_count=(unsigned)-1;
 CHECK(vmspace_user_lease_acquire(pins,3,&lease)==EBUSY && lease==NULL);
 CHECK(unmaps==0 && r[1].hold_count==(unsigned)-1);
 r[1].hold_count=0;balanced(p,vm,r,a,4);
 /* Same-space adjacent output aliases retire as a run; failed runs stay mapped. */
 for(i=0;i<4;i++) {
  setup(p,vm,r,a,pins,4,bytes);
  for(j=0;j<4;j++){a[j].vm=&vm[0];a[j].region=&r[0];}
  if(i==1)fail_unmap=1;
  if(i==2){a[2].address+=4096;a[3].address+=4096;fail_unmap=2;}
  if(i==3)a[1].flags&=~VM_MAPPING_MAPPED;
  if(i==1 || i==2) {
   CHECK(vmspace_user_lease_acquire(pins,3,&lease)==EIO && lease==NULL);
   CHECK(unmaps==fail_unmap && unmapped_pages==(i==1?0:2));
   for(j=0;j<4;j++)CHECK(!!(a[j].flags&VM_MAPPING_MAPPED)==(i==1 || j>=2));
  } else {
   CHECK(vmspace_user_lease_acquire(pins,3,&lease)==0 && lease);
   CHECK(unmaps==(i==0?1:2) && unmapped_pages==(i==0?4:3));
   for(j=0;j<4;j++)CHECK(!(a[j].flags&VM_MAPPING_MAPPED));
   vmspace_user_lease_release(lease);
  }
  balanced(p,vm,r,a,4);
 }
 puts("output alias batches: PASS (adjacent, gap, hole, first/later refusal)");
 /* A protection failure leaves a recovery flag, not proof of readonly PTEs. */
 setup(p,vm,r,a,pins,4,bytes);
 protections=0;fail_protection=1;
 CHECK(vmspace_user_input_lease_acquire(pins,3,&lease)==EIO && lease==NULL);
 CHECK(protections==1 && unmaps==0);
 CHECK(a[0].flags&VM_MAPPING_INPUT_PROTECTED);
 CHECK(!(a[0].flags&VM_MAPPING_BUSY));
 CHECK(r[0].hold_count==0 && r[1].hold_count==0);
 fail_protection=0;protections=0;
 CHECK(vmspace_user_input_lease_acquire(pins,3,&lease)==0 && lease);
 CHECK(protections==4 && unmaps==0);
 for(i=0;i<4;i++)CHECK(a[i].flags&VM_MAPPING_INPUT_PROTECTED);
 vmspace_user_lease_release(lease);
 balanced(p,vm,r,a,4);
 puts("input protection failure/retry: PASS (retained flag does not skip HAL)");
 /* Only output-revoked aliases restore, preserving COW and failure fallback. */
 restore_enabled=1;
 for(i=0;i<4;i++) {
  setup(p,vm,r,a,pins,2,bytes);
  a[1].vm=&vm[0];a[1].region=&r[0];a[0].private_next=NULL;
  a[1].private_page=&p[1];p[0].mapping_count=1;p[1].mappings=&a[1];p[1].mapping_count=1;
  if(i==2)a[1].flags&=~VM_MAPPING_MAPPED;
  if(i==3)a[1].flags&=~VM_MAPPING_COW;
  restores=restore_pages=0;restore_fail=i==1?1:0;
  CHECK(vmspace_user_lease_acquire(pins,3,&lease)==0 && lease);
  CHECK(!(a[0].flags&VM_MAPPING_MAPPED) && !(a[1].flags&VM_MAPPING_MAPPED));
  vmspace_user_lease_release(lease);
  CHECK(restores==(i==3?2:1));
  CHECK(restore_pages==(i==1?0:i==2?1:2));
  CHECK(!!(a[0].flags&VM_MAPPING_MAPPED)==(i!=1));
  CHECK(!!(a[1].flags&VM_MAPPING_MAPPED)==(i!=1 && i!=2));
  balanced(p,vm,r,a,2);
 }
 restore_enabled=0;
 puts("output restore: PASS (contiguous frames, COW, absent alias, refusal)");
 for(i=0;i<sizeof(bytes);i++)CHECK(bytes[i]==0x5a);
 puts("user alias lease: PASS (duplicate pins, cross-space aliases, refusal, partial revoke, release)");
 return 0;
}
