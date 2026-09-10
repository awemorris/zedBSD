/* Real file/cache/VM/private-COW owners with a checked host PTE backend. */
#define FILE_CACHE_MAIN retained_file_cache_main
#define vm_reclaim_private_one legacy_reclaim_private_one
#define vm_reclaim_one legacy_reclaim_one
#define vm_page_untrack legacy_page_untrack
#define vm_commit_reserve legacy_commit_reserve
#define vm_commit_release legacy_commit_release
#define hal_space_prot_query legacy_page_prot_query
#define hal_space_unmap legacy_page_unmap
#define hal_space_destroy legacy_page_destroy_space
#include "file-cache-host.c"
#undef vm_reclaim_private_one
#undef vm_reclaim_one
#undef vm_page_untrack
#undef vm_commit_reserve
#undef vm_commit_release
#undef hal_space_prot_query
#undef hal_space_unmap
#undef hal_space_destroy
#include <kern/swap.h>

struct host_pte { uintptr_t address;hal_physaddr_t physical;uint32_t prot;unsigned used; };
struct host_space { struct host_pte entries[64]; };
static size_t committed;
static unsigned map_fail,protect_fail,commit_fail;
static unsigned lease_unmap_failure,lease_protect_failure,map_count;
static unsigned protection_calls,partial_protection;
static size_t largest_protection;
int vm_metadata_owned(void) { return metadata_depth != 0; }
void *kern_malloc(size_t bytes) { return kern_calloc(1,bytes); }
int vm_commit_reserve(size_t bytes)
{ if(commit_fail)return ENOMEM;CHECK(committed<=SIZE_MAX-bytes);committed+=bytes;return 0; }
void vm_commit_release(size_t bytes)
{ CHECK(bytes<=committed);committed-=bytes; }
hal_space_t hal_space_create(void) { return calloc(1,sizeof(struct host_space)); }
static struct host_pte *pte(hal_space_t handle,uintptr_t address)
{
 struct host_space *space=handle;unsigned n;
 for(n=0;n<64;n++)if(space->entries[n].used && space->entries[n].address==address)return &space->entries[n];
 return NULL;
}
int hal_space_map(hal_space_t handle,void *address,hal_physaddr_t physical,size_t size,uint32_t prot)
{
 struct host_space *space=handle;struct host_pte *entry;unsigned n;
 map_count++;
 CHECK(size==4096 && ((uintptr_t)address%4096)==0);
 if(map_fail){map_fail--;return HAL_ERR_NOMEM;}
 entry=pte(handle,(uintptr_t)address);
 if(!entry)for(n=0;n<64;n++)if(!space->entries[n].used){entry=&space->entries[n];break;}
 CHECK(entry!=NULL);*entry=(struct host_pte){(uintptr_t)address,physical,prot,1};return HAL_OK;
}
int hal_space_unmap(hal_space_t handle,void *address,size_t size)
{
 struct host_pte *entry;size_t n;
 if(lease_unmap_failure && --lease_unmap_failure==0)return HAL_ERR_STATE;
 for(n=0;n<size;n+=4096){entry=pte(handle,(uintptr_t)address+n);if(entry)entry->used=0;}
 return HAL_OK;
}
int hal_space_prot(hal_space_t handle,void *address,size_t size,uint32_t prot)
{
 struct host_pte *entry;size_t n;
 protection_calls++;
 if(size>largest_protection)largest_protection=size;
 if(partial_protection) {
  partial_protection=0;entry=pte(handle,(uintptr_t)address);CHECK(entry);entry->prot=prot;
  return HAL_ERR_STATE;
 }
 if(lease_protect_failure && --lease_protect_failure==0)return HAL_ERR_STATE;
 if(protect_fail){protect_fail--;return HAL_ERR_NOMEM;}
 for(n=0;n<size;n+=4096){entry=pte(handle,(uintptr_t)address+n);CHECK(entry!=NULL);entry->prot=prot;}
 return HAL_OK;
}
int hal_space_prot_query(hal_space_t handle,void *address,size_t size,uint32_t prot,uint32_t *flags)
{ *flags=HAL_SPACE_PAGE_ACCESSED;return hal_space_prot(handle,address,size,prot); }
int hal_space_query(hal_space_t handle,void *address,hal_physaddr_t *physical, uint32_t *flags)
{
 if (physical != NULL) *physical = 0;
 CHECK(pte(handle,(uintptr_t)address)!=NULL);*flags=HAL_SPACE_PAGE_ACCESSED;return HAL_OK; }
int hal_space_clear_flags(hal_space_t handle,void *address,uint32_t flags)
{ (void)flags;CHECK(pte(handle,(uintptr_t)address)!=NULL);return HAL_OK; }
void hal_space_destroy(hal_space_t handle)
{ struct host_space *space=handle;unsigned n;for(n=0;n<64;n++)CHECK(!space->entries[n].used);free(space); }
struct swap_backend *swap_system_backend(void) { return NULL; }
int swap_alloc_slot(struct swap_backend *backend,uint32_t *slot)
{ (void)backend;(void)slot;return ENOSPC; }
void swap_free_slot(struct swap_backend *backend,uint32_t slot)
{ (void)backend;(void)slot;CHECK(0); }
int swap_read_page(struct swap_backend *backend,uint32_t slot,void *page)
{ (void)backend;(void)slot;(void)page;return EIO; }
int swap_write_page(struct swap_backend *backend,uint32_t slot,const void *page)
{ (void)backend;(void)slot;(void)page;return EIO; }

#include <kern/elf.h>
#include <kern/exec.h>

static unsigned elf_text_reads;
static ssize_t elf_pread(struct file *file,void *buffer,size_t size,off_t offset)
{ if(offset==0 || offset==8192)elf_text_reads++;return cache_pread(file,buffer,size,offset); }

static void test_elf(unsigned immutable, unsigned interpreter, unsigned fail)
{
 struct fixture fixture;struct disk disk={0};struct elf64_ehdr *header;
 struct elf64_phdr *ph;struct elf64_image_info info;
 struct vmspace *first,*second;struct vm_region *region;
 unsigned char byte;unsigned reads,text_reads,shared;uintptr_t bias;int error;
 make_fixture(&fixture);cache_ops=backend_ops;cache_ops.pread=elf_pread;
 fixture.inode.i_fop=fixture.owner->f_ops=&cache_ops;
 disk.d_block_size=512;disk.d_block_count=4096;fixture.mount.m_disk=&disk;
 if(immutable){fixture.mount.m_flags|=MOUNT_READ_ONLY;disk.d_flags=DISK_READ_ONLY;}
 header=(void *)fixture.bytes;memset(header,0,sizeof(*header));
 memcpy(header->e_ident,"\177ELF",4);header->e_ident[EI_CLASS]=ELFCLASS64;
 header->e_ident[EI_DATA]=ELFDATA2LSB;header->e_ident[EI_VERSION]=EV_CURRENT;
 header->e_type=interpreter?ET_DYN:ET_EXEC;header->e_machine=EM_X86_64;
 header->e_version=EV_CURRENT;header->e_entry=0x10100;header->e_ehsize=sizeof(*header);
 header->e_phoff=sizeof(*header);header->e_phentsize=sizeof(*ph);header->e_phnum=interpreter?4:3;
 ph=(void *)(fixture.bytes+header->e_phoff);memset(ph,0,4*sizeof(*ph));
 ph[0]=(struct elf64_phdr){PT_LOAD,PF_R|PF_X,0,0x10000,0,4096,4096,4096};
 ph[1]=(struct elf64_phdr){PT_LOAD,PF_R|PF_X,4113,0x18011,0,8192,12288,4096};
 ph[2]=(struct elf64_phdr){PT_LOAD,PF_R|PF_W,14336,0x24800,0,64,4096,4096};
 if(interpreter)ph[3].p_type=PT_DYNAMIC;
 first=vmspace_create();second=vmspace_create();CHECK(first && second);
 if(fail==1)map_fail=1;
 if(fail>1)allocation_failure=allocation_count+fail-1;
 if(interpreter)error=elf64_load_interpreter(fixture.owner,first,&info);
 else error=elf64_load(fixture.owner,first,&info);
 allocation_failure=0;
 if(fail){CHECK(error==0 || error==ENOMEM);if(error)CHECK(first->regions==NULL);goto finish;}
 CHECK(error==0);
 bias=info.entry-0x10100;
 reads=cache_backend_reads;text_reads=elf_text_reads;
 if(interpreter)CHECK(elf64_load_interpreter(fixture.owner,second,&info)==0);
 else CHECK(elf64_load(fixture.owner,second,&info)==0);
 if(immutable){CHECK(elf_text_reads==text_reads);printf("ELF warm text reads=0; private edge/data reads=%u\n",cache_backend_reads-reads);}
 shared=0;for(region=first->regions;region;region=region->next)if(region->snapshot)shared++;
 CHECK(shared==(immutable?2U:0U));
 CHECK(vmspace_fault(first,bias+0x10100,HAL_SPACE_EXEC)==0);
 CHECK(vmspace_fault(second,bias+0x10100,HAL_SPACE_EXEC)==0);
 CHECK((pte(first->space,bias+0x10000)->physical==pte(second->space,bias+0x10000)->physical)==immutable);
 CHECK(vmspace_copy_from(first,&byte,bias+0x18010,1)==0 && byte==0);
 CHECK(vmspace_copy_from(first,&byte,bias+0x18011,1)==0 && byte==0xa5);
 CHECK(vmspace_copy_from(first,&byte,bias+0x1a010,1)==0 && byte==0xa5);
 CHECK(vmspace_copy_from(first,&byte,bias+0x1a011,1)==0 && byte==0);
 CHECK(vmspace_copy_from(first,&byte,bias+0x1b010,1)==0 && byte==0);
 CHECK(vmspace_copy_from(first,&byte,bias+0x24840,1)==0 && byte==0);
 byte=0x37;CHECK(vmspace_copy_to(first,bias+0x24800,&byte,1)==0);
 CHECK(vmspace_copy_from(second,&byte,bias+0x24800,1)==0 && byte==0xa5);
 CHECK(fixture.bytes[14336]==0xa5);
 if(!immutable){
  byte=0x78;CHECK(file_pwrite(fixture.owner,&byte,1,409)==1);
  CHECK(vmspace_copy_from(first,&byte,bias+0x10199,1)==0 && byte==0xa5);
 }
finish:
 vmspace_put(first);vmspace_put(second);
 CHECK(committed==0 && cache_disk_users==0 && fixture.inode.i_vm_content_readers==0);
 (void)vm_object_cache_drain(NULL);
 fixture.mount.m_flags&=~MOUNT_READ_ONLY;fixture.mount.m_disk=NULL;close_fixture(&fixture);
 CHECK(file_count()==0 && vm_object_count()==0 && vm_object_page_count()==0);
}

/* Observe a real VM wait before allowing the lease owner to release. */
static pthread_mutex_t lease_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lease_event = PTHREAD_COND_INITIALIZER;
static unsigned lease_waiting;
struct lease_mutator {
 struct vmspace *vm,*forked;
 unsigned mode;
 int error;
 unsigned done;
};
static void lease_wait_observer(struct wait_queue *queue)
{
 (void)queue;
 CHECK(pthread_mutex_lock(&lease_gate)==0);
 lease_waiting=1;
 CHECK(pthread_cond_signal(&lease_event)==0);
 CHECK(pthread_mutex_unlock(&lease_gate)==0);
}
static void *lease_mutate(void *argument)
{
 struct lease_mutator *m=argument;
 unsigned char byte=0x73;
 if(m->mode==0)m->error=vmspace_protect(m->vm,0x10000,4096,HAL_SPACE_READ|HAL_SPACE_WRITE);
 else if(m->mode==1)m->error=vmspace_unmap(m->vm,0x10000,4096);
 else if(m->mode==2)m->error=vmspace_copy_to(m->vm,0x10000,&byte,1);
 else m->error=vmspace_fork(m->vm,&m->forked);
 __atomic_store_n(&m->done,1,__ATOMIC_RELEASE);
 return NULL;
}
static void test_user_alias_lease(int input)
{
 struct vmspace *left,*child;
 struct vmspace_pinned_page pin;
 struct vmspace_user_lease *lease;
 struct lease_mutator mutator;
 struct timespec deadline;
 pthread_t worker;
 unsigned mode;
 unsigned char byte;
 hal_physaddr_t original;
 for(mode=0;mode<6;mode++) {
  left=vmspace_create();CHECK(left);
  CHECK(vmspace_map_anon(left,0x10000,4096,HAL_SPACE_READ|HAL_SPACE_WRITE,NULL)==0);
  byte=0x29;CHECK(vmspace_copy_to(left,0x10000,&byte,1)==0);
  CHECK(vmspace_fork(left,&child)==0);
  CHECK(vmspace_pin_user_pages(left,0x10000,4096,HAL_SPACE_READ,&pin,1)==0);
  original=pin.memory.paddr;
  if(mode==5) {
   /* A second-alias HAL failure must leave real mappings able to refault. */
   if(input)lease_protect_failure=2;else lease_unmap_failure=2;
   CHECK((input?vmspace_user_input_lease_acquire(&pin,1,&lease):vmspace_user_lease_acquire(&pin,1,&lease))==EIO && lease==NULL);
   CHECK(lease_unmap_failure==0);
   vmspace_unpin_user_pages(&pin,1);
   CHECK(vmspace_copy_from(left,&byte,0x10000,1)==0 && byte==0x29);
   CHECK(vmspace_copy_from(child,&byte,0x10000,1)==0 && byte==0x29);
   byte=0x73;CHECK(vmspace_copy_to(child,0x10000,&byte,1)==0);
   CHECK(vmspace_copy_from(left,&byte,0x10000,1)==0 && byte==0x29);
   vmspace_put(child);vmspace_put(left);CHECK(committed==0);
   continue;
  }
  CHECK((input?vmspace_user_input_lease_acquire(&pin,1,&lease):vmspace_user_lease_acquire(&pin,1,&lease))==0);
  if(input) {
   CHECK(pte(left->space,0x10000) && pte(child->space,0x10000));
   CHECK(!(pte(left->space,0x10000)->prot&HAL_SPACE_WRITE));
   CHECK(!(pte(child->space,0x10000)->prot&HAL_SPACE_WRITE));
  } else CHECK(pte(left->space,0x10000)==NULL && pte(child->space,0x10000)==NULL);
  if(mode==4) {
   /* Drop the child owner's last reference while the lease retains it. */
   vmspace_put(child);child=NULL;
  } else {
   memset(&mutator,0,sizeof(mutator));mutator.vm=child;mutator.mode=mode;
   lease_waiting=0;cache_wait_observer=lease_wait_observer;
   CHECK(pthread_create(&worker,NULL,lease_mutate,&mutator)==0);
   CHECK(clock_gettime(CLOCK_REALTIME,&deadline)==0);deadline.tv_sec+=5;
   CHECK(pthread_mutex_lock(&lease_gate)==0);
   while(!lease_waiting)CHECK(pthread_cond_timedwait(&lease_event,&lease_gate,&deadline)==0);
   CHECK(pthread_mutex_unlock(&lease_gate)==0);
   CHECK(!__atomic_load_n(&mutator.done,__ATOMIC_ACQUIRE));
   CHECK(*(unsigned char *)(uintptr_t)original==0x29);
  }
  vmspace_user_lease_release(lease);
  vmspace_unpin_user_pages(&pin,1);
  if(mode!=4) {
   CHECK(pthread_join(worker,NULL)==0);cache_wait_observer=NULL;
   CHECK(mutator.error==0 && mutator.done);
   if(mutator.forked)vmspace_put(mutator.forked);
   if(mode==1)CHECK(pte(child->space,0x10000)==NULL);
   else {
    CHECK(vmspace_copy_from(child,&byte,0x10000,1)==0);
    CHECK(byte==(mode==2?0x73:0x29));
    if(mode==2)CHECK(pte(child->space,0x10000)->physical!=original);
   }
  }
  CHECK(vmspace_copy_from(left,&byte,0x10000,1)==0 && byte==0x29);
  if(child)vmspace_put(child);
  vmspace_put(left);CHECK(committed==0);
 }
 puts("User lease actual protect/unmap/COW/fork waits and final-space release: PASS");
}

static void test_input_read_alias(void)
{
 struct vmspace *vm;
 struct vmspace_pinned_page pin;
 struct vmspace_user_lease *lease;
 unsigned before;
 unsigned char byte=0x45;
 vm=vmspace_create();CHECK(vm);
 CHECK(vmspace_map_anon(vm,0x10000,4096,HAL_SPACE_READ|HAL_SPACE_WRITE,NULL)==0);
 CHECK(vmspace_copy_to(vm,0x10000,&byte,1)==0);
 CHECK(pte(vm->space,0x10000)->prot&HAL_SPACE_WRITE);
 CHECK(vmspace_pin_user_pages(vm,0x10000,4096,HAL_SPACE_READ,&pin,1)==0);
 CHECK(vmspace_user_input_lease_acquire(&pin,1,&lease)==0);
 CHECK(pte(vm->space,0x10000) && !(pte(vm->space,0x10000)->prot&HAL_SPACE_WRITE));
 vmspace_user_lease_release(lease);vmspace_unpin_user_pages(&pin,1);
 before=map_count;
 CHECK(vmspace_pin_user_pages(vm,0x10000,4096,HAL_SPACE_READ,&pin,1)==0);
 CHECK(map_count==before);
 vmspace_unpin_user_pages(&pin,1);
 protect_fail=1;CHECK(vmspace_fault(vm,0x10000,HAL_SPACE_WRITE)==EIO);
 CHECK(!(pte(vm->space,0x10000)->prot&HAL_SPACE_WRITE));
 CHECK(vmspace_fault(vm,0x10000,HAL_SPACE_WRITE)==0);
 CHECK(pte(vm->space,0x10000)->prot&HAL_SPACE_WRITE);
 /* Captured pins survive a later permission change, including write-only. */
 CHECK(vmspace_pin_user_pages(vm,0x10000,4096,HAL_SPACE_READ,&pin,1)==0);
 CHECK(vmspace_protect(vm,0x10000,4096,HAL_SPACE_WRITE)==0);
 CHECK(vmspace_user_input_lease_acquire(&pin,1,&lease)==0);
 CHECK(pte(vm->space,0x10000)==NULL);
 vmspace_user_lease_release(lease);vmspace_unpin_user_pages(&pin,1);
 CHECK(vmspace_protect(vm,0x10000,4096,HAL_SPACE_READ|HAL_SPACE_WRITE)==0);
 CHECK(vmspace_copy_from(vm,&byte,0x10000,1)==0 && byte==0x45);
 vmspace_put(vm);CHECK(committed==0);
 puts("Input lease readable alias reuse/write-fault/write-only fallback: PASS");
}

static void test_input_ranges(void)
{
 struct vmspace *vm;
 struct vmspace_pinned_page pins[4],ordered[4];
 struct vmspace_user_lease *lease;
 unsigned mode,i,before;
 unsigned char byte=0x27;
 for(mode=0;mode<5;mode++) {
  vm=vmspace_create();CHECK(vm);
  CHECK(vmspace_map_anon(vm,0x20000,16384,HAL_SPACE_READ|HAL_SPACE_WRITE|HAL_SPACE_EXEC,NULL)==0);
  for(i=0;i<4;i++)CHECK(vmspace_copy_to(vm,0x20000+i*4096,&byte,1)==0);
  CHECK(vmspace_pin_user_pages(vm,0x20000,16384,HAL_SPACE_READ,pins,4)==0);
  memcpy(ordered,pins,sizeof(pins));
  if(mode==1)CHECK(vmspace_protect(vm,0x21000,4096,HAL_SPACE_READ)==0);
  if(mode==2){ordered[1]=pins[2];ordered[2]=pins[1];}
  if(mode==3)CHECK(vmspace_protect(vm,0x21000,4096,0)==0);
  before=protection_calls;largest_protection=0;
  if(mode==4)partial_protection=1;
  CHECK(vmspace_user_input_lease_acquire(ordered,4,&lease)==(mode==4?EIO:0));
  CHECK(protection_calls-before==(mode==1?3U:mode==2?4U:mode==3?2U:1U));
  CHECK(largest_protection==(mode==0 || mode==4?16384U:mode==2?4096U:8192U));
  if(mode!=4) {
   for(i=0;i<4;i++) {
    if(mode==3 && i==1)CHECK(!pte(vm->space,0x20000+i*4096));
    else CHECK(!(pte(vm->space,0x20000+i*4096)->prot&HAL_SPACE_WRITE));
   }
   vmspace_user_lease_release(lease);
  } else CHECK(lease==NULL && !partial_protection);
  vmspace_unpin_user_pages(pins,4);
  /* A partially changed range still restores each writable page on fault. */
  if(mode==4)for(i=0;i<4;i++) {
   CHECK(vmspace_fault(vm,0x20000+i*4096,HAL_SPACE_WRITE)==0);
   CHECK(pte(vm->space,0x20000+i*4096)->prot&HAL_SPACE_WRITE);
  }
  vmspace_put(vm);CHECK(committed==0);
 }
 puts("Input protection ranges: PASS contiguous/protection/gap/absent/partial-HAL");
}

/* A backend may dirty all requested bytes before returning short/error. */
static ssize_t prefix_hostile_read(struct file *file,void *buffer,size_t size,off_t offset)
{
 ssize_t count=cache_pread(file,buffer,size,offset);
 if(count<0)memset(buffer,0xce,size);
 else if((size_t)count<size)memset((unsigned char *)buffer+count,0xce,size-(size_t)count);
 return count;
}
static ssize_t prefix_sequential_read(struct file *file,void *buffer,size_t size)
{
 return prefix_hostile_read(file,buffer,size,file->f_offset);
}
static void test_coherent_prefix(void)
{
 struct fixture fixture;
 struct file_io io;
 unsigned char output[8192],scattered[5192],first;
 struct io_span spans[2];struct io_destination destination;
 unsigned mode,i,before,entry;
 ssize_t count;
 int error;
 for(entry=0;entry<6;entry++) for(mode=0;mode<5;mode++) {
  make_fixture(&fixture);cache_ops=backend_ops;cache_ops.pread=prefix_hostile_read;cache_ops.read=prefix_sequential_read;
  fixture.inode.i_fop=fixture.owner->f_ops=&cache_ops;
  vm_object_cache_prepare(fixture.owner);
  if(mode==3)CHECK(file_pread(fixture.owner,&first,1,0)==1);
  CHECK(file_io_begin(fixture.owner,(entry==2 || entry==5)?FILE_IO_READ:FILE_IO_PREAD,0,0,&io)==0);
  CHECK(io.held_content_read && io.read_object);
  if(entry==1 || entry==2) {
   unsigned saved=io.internal_flags;
   memset(output,0x66,sizeof(output));before=cache_backend_reads;
   io.internal_flags=FILE_IO_VM_OBJECT;
   CHECK(file_io_transfer_prefix(&io,output,sizeof(output))==-ENOTSUP);
   io.internal_flags=saved;io.coherent_read=0;
   CHECK(file_io_transfer_prefix(&io,output,sizeof(output))==-ENOTSUP);
   io.coherent_read=1;
   CHECK(io.offset==0 && fixture.owner->f_offset==0 && cache_backend_reads==before);
   for(i=0;i<sizeof(output);i++)CHECK(output[i]==0x66);
  }
  cache_read_error=mode==0 || mode==3?EIO:0;
  cache_short_limit=mode==1?5000:0;cache_pool_disabled=mode==2;
  memset(output,0x66,sizeof(output));count=-1;before=cache_backend_reads;
  if(entry==1 || entry==2) {
   count=file_io_transfer_prefix(&io,output,sizeof(output));
   error=count<0?(int)-count:0;
   CHECK(io.offset==(count>0?count:0));
   CHECK(fixture.owner->f_offset==(entry==2 && count>0?count:0));
   CHECK(io.transferred==(count>0));
  } else if(entry>=3) {
   spans[0].address=output;spans[0].size=3000;
   spans[1].address=scattered;spans[1].size=sizeof(scattered);
   destination.spans=spans;destination.count=2;
   memset(scattered,0x66,sizeof(scattered));
   CHECK(io_destination_validate(&destination,sizeof(output)+1)==EINVAL);
   spans[1].address=NULL;
   CHECK(vm_object_read_coherent_destination(io.content_inode,0,&destination,sizeof(output),&count,NULL)==EINVAL);
   for(i=0;i<sizeof(output);i++)CHECK(output[i]==0x66);
   CHECK(cache_backend_reads==before);
   spans[1].address=scattered;
   if(entry==3)error=vm_object_read_coherent_destination(io.content_inode,0,&destination,sizeof(output),&count,NULL);
   else {
    unsigned saved=io.internal_flags;
    io.internal_flags=FILE_IO_VM_OBJECT;
    CHECK(file_io_transfer_destination(&io,&destination,sizeof(output))==-ENOTSUP);
    io.internal_flags=saved;io.coherent_read=0;
    CHECK(file_io_transfer_destination(&io,&destination,sizeof(output))==-ENOTSUP);
    io.coherent_read=1;
    CHECK(io.offset==0 && cache_backend_reads==before);
    count=file_io_transfer_destination(&io,&destination,sizeof(output));
    error=count<0?(int)-count:0;
    CHECK(io.offset==(count>0?count:0));
    CHECK(fixture.owner->f_offset==(entry==5 && count>0?count:0));
    CHECK(io.transferred==(count>0));
   }
   memcpy(output+3000,scattered,sizeof(scattered));
   if(mode==4)CHECK(cache_backend_reads==before+1);
  } else error=vm_object_read_coherent_prefix(io.content_inode,0,output,sizeof(output),&count,NULL);
  if(mode==0 || mode==2) {
   CHECK(error==(mode==0?EIO:ENOMEM));
   for(i=0;i<sizeof(output);i++)CHECK(output[i]==0x66);
   if(mode==2) {
    CHECK(cache_backend_reads==before);
    /* Positive control: generic fallback exposes the hostile raw write. */
    cache_read_error=EIO;
    CHECK(vm_object_read_coherent_useful(io.content_inode,0,output,sizeof(output),&count,NULL)==EIO);
    CHECK(cache_backend_reads==before+1);
    for(i=0;i<sizeof(output);i++)CHECK(output[i]==0xce);
   }
  } else {
   CHECK(error==0 && count==(mode==1?5000:mode==3?4096:8192));
   CHECK(memcmp(output,fixture.bytes,(size_t)count)==0);
   for(i=(unsigned)count;i<sizeof(output);i++)CHECK(output[i]==0x66);
  }
  if(entry && entry!=3)CHECK(file_io_complete(&io,count)==count);
  else file_io_end(&io);
  cache_read_error=cache_short_limit=cache_pool_disabled=0;
  CHECK(!cache_pool_used && !cache_disk_users);
  (void)vm_object_cache_drain(NULL);close_fixture(&fixture);
  CHECK(file_count()==0 && vm_object_count()==0 && vm_object_page_count()==0);
 }
 puts("Strict coherent prefix: PASS hostile error/short, no-scratch, resident-prefix failure");
}

static ssize_t destination_batch_read(struct file *file,void *buffer,size_t size,off_t offset)
{
 size_t i;(void)file;cache_backend_reads++;
 for(i=0;i<size;i++)((unsigned char *)buffer)[i]=(unsigned char)(((size_t)offset+i)%251);
 return (ssize_t)size;
}
static void test_destination_batch(void)
{
 struct fixture fixture;struct file_io io;
 unsigned char data[16][4096];struct io_span spans[16];
 struct io_destination destination;unsigned i,j,before;
 make_fixture(&fixture);fixture.inode.i_size=65536;
 cache_ops=backend_ops;cache_ops.pread=destination_batch_read;
 fixture.inode.i_fop=fixture.owner->f_ops=&cache_ops;
 vm_object_cache_prepare(fixture.owner);
 for(i=0;i<16;i++){spans[i].address=data[15-i];spans[i].size=4096;}
 destination.spans=spans;destination.count=16;
 CHECK(file_io_begin(fixture.owner,FILE_IO_PREAD,0,0,&io)==0);
 before=cache_backend_reads;
 CHECK(file_io_transfer_destination(&io,&destination,65536)==65536);
 CHECK(cache_backend_reads==before+1 && io.offset==65536 && fixture.owner->f_offset==0);
 for(i=0;i<16;i++)for(j=0;j<4096;j++)CHECK(data[15-i][j]==(unsigned char)((i*4096+j)%251));
 CHECK(file_io_complete(&io,65536)==65536);
 (void)vm_object_cache_drain(NULL);close_fixture(&fixture);
 CHECK(file_count()==0 && vm_object_count()==0 && vm_object_page_count()==0);
 puts("destination batch: PASS 64 KiB, 16 reversed spans, one backend call");
}

int main(void)
{
 struct fixture fixture;struct disk disk={0};struct file_content_lease input;
 struct file_exec_snapshot *snapshot;struct vmspace *left,*right,*child;
 unsigned fail_index;struct vm_page *page;unsigned char byte;hal_physaddr_t original;unsigned reads;
 cache_memory_init();vm_reclaim_init();
 if(getenv("WS025_PREFIX_ONLY")){test_coherent_prefix();test_destination_batch();return 0;}
 test_user_alias_lease(0);test_user_alias_lease(1);test_input_read_alias();test_input_ranges();test_coherent_prefix();
 make_fixture(&fixture);cache_ops=backend_ops;cache_ops.pread=cache_pread;
 fixture.inode.i_fop=fixture.owner->f_ops=&cache_ops;
 disk.d_block_size=512;disk.d_block_count=4096;fixture.mount.m_disk=&disk;fixture.mount.m_flags|=MOUNT_READ_ONLY;disk.d_flags=DISK_READ_ONLY;
 CHECK(file_exec_snapshot_begin(fixture.owner,&input)==0);
 CHECK(file_exec_snapshot_create(&input,0,8192,&snapshot)==0);
 left=vmspace_create();right=vmspace_create();CHECK(left && right);
 CHECK(vmspace_map_exec_snapshot(left,0x10000,HAL_SPACE_READ|HAL_SPACE_EXEC,snapshot)==0);
 CHECK(vmspace_map_exec_snapshot(right,0x10000,HAL_SPACE_READ|HAL_SPACE_EXEC,snapshot)==0);
 file_exec_snapshot_put(snapshot);file_content_lease_end(&input);
 reads=cache_backend_reads;
 CHECK(vmspace_fault(left,0x10000,HAL_SPACE_EXEC)==0);
 CHECK(vmspace_fault(right,0x10000,HAL_SPACE_EXEC)==0);
 original=pte(left->space,0x10000)->physical;
 CHECK(original==pte(right->space,0x10000)->physical && cache_backend_reads==reads);
 CHECK((pte(left->space,0x10000)->prot&HAL_SPACE_WRITE)==0 && committed==0);

 /* mprotect reserves private commit but keeps cache translations read-only. */
 commit_fail=1;
 CHECK(vmspace_protect(left,0x10000,8192,HAL_SPACE_READ|HAL_SPACE_WRITE)==ENOMEM);
 CHECK(committed==0);commit_fail=0;
 protect_fail=1;
 CHECK(vmspace_protect(left,0x10000,8192,HAL_SPACE_READ|HAL_SPACE_WRITE)!=0);
 CHECK(committed==0 && !(left->regions->prot&HAL_SPACE_WRITE));
 CHECK(vmspace_protect(left,0x10000,8192,HAL_SPACE_READ|HAL_SPACE_WRITE)==0);
 CHECK(committed==8192 && !(pte(left->space,0x10000)->prot&HAL_SPACE_WRITE));
 byte=0x31;CHECK(vmspace_copy_to(left,0x10000,&byte,1)==0);
 CHECK(pte(left->space,0x10000)->physical!=original);
 CHECK(*(unsigned char *)(uintptr_t)original==0xa5 && fixture.bytes[0]==0xa5);
 byte=0;CHECK(vmspace_copy_from(right,&byte,0x10000,1)==0 && byte==0xa5);

 /* Fork preserves earlier private changes and keeps untouched cache pages shared. */
 CHECK(vmspace_fork(left,&child)==0);
 CHECK(committed==16384);
 byte=0;CHECK(vmspace_copy_from(child,&byte,0x10000,1)==0 && byte==0x31);
 byte=0x72;CHECK(vmspace_copy_to(child,0x10000,&byte,1)==0);
 CHECK(vmspace_copy_from(left,&byte,0x10000,1)==0 && byte==0x31);
 CHECK(vmspace_fault(child,0x11000,HAL_SPACE_READ)==0);
 CHECK(vmspace_fault(right,0x11000,HAL_SPACE_READ)==0);
 CHECK(pte(child->space,0x11000)->physical==pte(right->space,0x11000)->physical);

 /* First access through a writable uaccess path must COW before returning. */
 byte=0x54;CHECK(vmspace_copy_to(child,0x11000,&byte,1)==0);
 CHECK(vmspace_copy_from(right,&byte,0x11000,1)==0 && byte==0xa5);
 page=child->regions->pages;while(page && page->address!=0x11000)page=page->next;
 CHECK(page && page->private_page && !page->object_page);

 /* Failed replacement leaves the old snapshot readable on a later retry. */
 CHECK(vmspace_fault(left,0x11000,HAL_SPACE_READ)==0);
 map_fail=1;CHECK(vmspace_copy_to(left,0x11000,&byte,1)!=0);
 map_fail=0;CHECK(vmspace_copy_from(left,&byte,0x11000,1)==0 && byte==0xa5);
 CHECK(vmspace_unmap(child,0x11000,4096)==0);
 CHECK(vmspace_map_exec_snapshot(child,0x11000,HAL_SPACE_READ,
     left->regions->snapshot)==0);
 CHECK(vmspace_protect(child,0x11000,8192,HAL_SPACE_READ|HAL_SPACE_WRITE)==0);
 /* No preceding read fault: writable uaccess must create a private frame. */
 byte=0x65;CHECK(vmspace_copy_to(child,0x11000,&byte,1)==0);
 CHECK(vmspace_copy_from(right,&byte,0x10000,1)==0 && byte==0xa5);
 CHECK(vmspace_protect(right,0x10000,4096,HAL_SPACE_READ)==0);
 CHECK(right->regions->next!=NULL);
 CHECK(vmspace_unmap(right,0x10000,4096)==0);
 CHECK(vmspace_copy_from(right,&byte,0x11000,1)==0 && byte==0xa5);
 vmspace_put(child);vmspace_put(left);vmspace_put(right);
 CHECK(committed==0 && cache_disk_users==0 && fixture.inode.i_vm_content_readers==0);
 (void)vm_object_cache_drain(NULL);
 fixture.mount.m_flags&=~MOUNT_READ_ONLY;fixture.mount.m_disk=NULL;close_fixture(&fixture);
 CHECK(file_count()==0 && vm_object_count()==0 && vm_object_page_count()==0);
 test_elf(1,0,0);test_elf(0,0,0);test_elf(1,1,0);test_elf(1,0,1);
 for(fail_index=2;fail_index<34;fail_index++)test_elf(1,0,fail_index);
 printf("Exec private cache sharing/COW/fork/split/cleanup: PASS (%u checks)\n",checks);
 return 0;
}
