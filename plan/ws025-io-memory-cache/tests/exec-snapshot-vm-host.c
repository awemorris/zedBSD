/* Real file/cache/VM/private-COW owners with a checked host PTE backend. */
#define FILE_CACHE_MAIN retained_file_cache_main
#define vm_reclaim_private_one legacy_reclaim_private_one
#define vm_reclaim_one legacy_reclaim_one
#define vm_page_untrack legacy_page_untrack
#define vm_commit_reserve legacy_commit_reserve
#define vm_commit_release legacy_commit_release
#define hal_page_prot_query legacy_page_prot_query
#define hal_page_unmap legacy_page_unmap
#define hal_page_destroy_space legacy_page_destroy_space
#include "file-cache-host.c"
#undef vm_reclaim_private_one
#undef vm_reclaim_one
#undef vm_page_untrack
#undef vm_commit_reserve
#undef vm_commit_release
#undef hal_page_prot_query
#undef hal_page_unmap
#undef hal_page_destroy_space
#include <kern/swap.h>

struct host_pte { uintptr_t address;hal_physaddr_t physical;uint32_t prot;unsigned used; };
struct host_space { struct host_pte entries[64]; };
static size_t committed;
static unsigned map_fail,protect_fail,commit_fail;
int vm_metadata_owned(void) { return metadata_depth != 0; }
void *kern_malloc(size_t bytes) { return kern_calloc(1,bytes); }
int vm_commit_reserve(size_t bytes)
{ if(commit_fail)return ENOMEM;CHECK(committed<=SIZE_MAX-bytes);committed+=bytes;return 0; }
void vm_commit_release(size_t bytes)
{ CHECK(bytes<=committed);committed-=bytes; }
hal_space_t hal_mem_create_space(void) { return calloc(1,sizeof(struct host_space)); }
static struct host_pte *pte(hal_space_t handle,uintptr_t address)
{
 struct host_space *space=handle;unsigned n;
 for(n=0;n<64;n++)if(space->entries[n].used && space->entries[n].address==address)return &space->entries[n];
 return NULL;
}
int hal_page_map(hal_space_t handle,void *address,hal_physaddr_t physical,size_t size,uint32_t prot)
{
 struct host_space *space=handle;struct host_pte *entry;unsigned n;
 CHECK(size==4096 && ((uintptr_t)address%4096)==0);
 if(map_fail){map_fail--;return HAL_ERR_NOMEM;}
 entry=pte(handle,(uintptr_t)address);
 if(!entry)for(n=0;n<64;n++)if(!space->entries[n].used){entry=&space->entries[n];break;}
 CHECK(entry!=NULL);*entry=(struct host_pte){(uintptr_t)address,physical,prot,1};return HAL_OK;
}
int hal_page_unmap(hal_space_t handle,void *address,size_t size)
{
 struct host_pte *entry;size_t n;
 for(n=0;n<size;n+=4096){entry=pte(handle,(uintptr_t)address+n);if(entry)entry->used=0;}
 return HAL_OK;
}
int hal_page_prot(hal_space_t handle,void *address,size_t size,uint32_t prot)
{
 struct host_pte *entry;size_t n;
 if(protect_fail){protect_fail--;return HAL_ERR_NOMEM;}
 for(n=0;n<size;n+=4096){entry=pte(handle,(uintptr_t)address+n);CHECK(entry!=NULL);entry->prot=prot;}
 return HAL_OK;
}
int hal_page_prot_query(hal_space_t handle,void *address,size_t size,uint32_t prot,uint32_t *flags)
{ *flags=HAL_PAGE_ACCESSED;return hal_page_prot(handle,address,size,prot); }
int hal_page_query(hal_space_t handle,void *address,uint32_t *flags)
{ CHECK(pte(handle,(uintptr_t)address)!=NULL);*flags=HAL_PAGE_ACCESSED;return HAL_OK; }
int hal_page_clear_flags(hal_space_t handle,void *address,uint32_t flags)
{ (void)flags;CHECK(pte(handle,(uintptr_t)address)!=NULL);return HAL_OK; }
void hal_page_destroy_space(hal_space_t handle)
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

int main(void)
{
 struct fixture fixture;struct disk disk={0};struct file_content_lease input;
 struct file_exec_snapshot *snapshot;struct vmspace *left,*right,*child;
 unsigned fail_index;struct vm_page *page;unsigned char byte;hal_physaddr_t original;unsigned reads;
 cache_memory_init();vm_reclaim_init();
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
