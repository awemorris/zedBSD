/* Actual mount redo-image allocation, accounting and failed-admission cleanup. */
#include <stdio.h>
#include <stdlib.h>
#include "src/drivers/fs/ufs/ufs-vfs.c"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"journal-image-owner:%d: %s\n",__LINE__,#x);abort(); } } while(0)
static unsigned allocations,frees,fail_alloc,fail_reserve;
static size_t charged,pending;
static unsigned char backing[73728];
int hal_pmem_alloc(const struct hal_pmem_request *request,struct hal_pmem *memory)
{
 CHECK(request->size==UFS_JOURNAL_IMAGE_BYTES);
 if(fail_alloc)return HAL_ERR_NOMEM;
 CHECK(allocations==frees);allocations++;
 memory->vaddr=backing;memory->size=sizeof(backing);return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *memory)
{ CHECK(memory->vaddr==backing && allocations==frees+1);frees++;return HAL_OK; }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
int cache_memory_reserve(enum cache_memory_kind kind,size_t bytes,int optional)
{
 CHECK(kind==CACHE_MEMORY_BUF_META && bytes==sizeof(backing) && !optional);
 CHECK(charged==0 && pending==0);
 if(fail_reserve)return ENOMEM;
 pending=bytes;return 0;
}
void cache_memory_commit(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_BUF_META && pending==bytes);pending=0;charged=bytes; }
void cache_memory_release(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_BUF_META && charged==bytes);charged=0; }
int main(void)
{
 struct ufs_mount_state ms={0};
 fail_alloc=1;CHECK(journal_image_alloc(&ms)==ENOMEM);
 CHECK(!ms.journal_memory.size && !charged && !pending);
 fail_alloc=0;fail_reserve=1;CHECK(journal_image_alloc(&ms)==ENOMEM);
 CHECK(allocations==frees && !ms.journal_memory.size && !charged && !pending);
 fail_reserve=0;ms.journal.pending_sequence=1;
 CHECK(journal_image_alloc(&ms)==EBUSY);
 CHECK(allocations==frees && !ms.journal_memory.size && !charged && !pending);
 ms.journal.pending_sequence=0;
 CHECK(journal_image_alloc(&ms)==0);
 CHECK(charged==sizeof(backing) && ms.journal.image==backing);
 /* Failed mount recovery has no readers even when durable redo remains pending. */
 ms.journal.pending_sequence=2;ms.journal.image_valid=1;
 journal_image_free(&ms);
 CHECK(allocations==frees && !charged && !pending && !ms.journal.image);
 journal_image_free(&ms);CHECK(allocations==frees);
 puts("UFS journal image owner/accounting: PASS");return 0;
}
