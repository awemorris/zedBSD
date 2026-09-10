/* Production endpoint, BIO and frontier with deterministic worker scheduling.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define STORAGE_FOUNDATION_CUSTOM_LOCK
#define STORAGE_FOUNDATION_CUSTOM_WAIT
#define STORAGE_FOUNDATION_CUSTOM_CLAIM_RELEASE
#include "../../ws018-kernel-architecture/tests/mount-thread-host.h"
#define main foundation_main
#include "../../ws019-installation/tests/storage-foundation-test.c"
#undef main
#include <kern/loop.h>
#include <kern/writeback.h>
#include <kern/file.h>
#include <fcntl.h>
#include "loop-submit.inc"

extern void async_host_exit(void);
static unsigned stopping, created, callbacks, memory_count;
static int memory_fail, credit_fail, thread_fail, free_fail;
static size_t memory_bytes, charged_bytes;
static void *host_workers[ASYNC_ENDPOINTS];
static struct thread fake_threads[ASYNC_ENDPOINTS];
static void (*worker_entries[ASYNC_ENDPOINTS])(void *);
static void *worker_arguments[ASYNC_ENDPOINTS];
static struct bio *late_bio;
static unsigned char media[2][65536];
static struct disk *devices[2];
static unsigned pause_first;
static int cancel_result;

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
unsigned long spin_lock_irqsave(struct spinlock *lock)
{
 unsigned expected;
 for (;;) {
  expected=0;
  if (__atomic_compare_exchange_n(&lock->held.value,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
  host_thread_yield();
 }
}
void spin_unlock_irqrestore(struct spinlock *lock,unsigned long irq)
{ (void)irq;__atomic_store_n(&lock->held.value,0,__ATOMIC_RELEASE); }
void waitq_wake_all(struct wait_queue *wait)
{ __atomic_fetch_add(&wait->sequence,1,__ATOMIC_RELEASE); }
int waitq_sleep(struct wait_queue *wait,struct spinlock *lock,uint64_t sequence,uint64_t deadline,unsigned flags)
{
 (void)wait;(void)sequence;(void)deadline;(void)flags;
 spin_unlock_irqrestore(lock,0);
 if (__atomic_load_n(&stopping,__ATOMIC_ACQUIRE))async_host_exit();
 host_thread_yield();spin_lock_irqsave(lock);return EAGAIN;
}
int hal_pmem_alloc(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr,struct hal_pmem *memory)
{
	(void)request_paddr; (void)request_alignment; (void)request_type; (void)request_attr;

 memset(memory,0,sizeof(*memory));if(memory_fail)return HAL_ERR_NOMEM;
 memory->size=(request_size+4095)&~(size_t)4095;
 memory->vaddr=aligned_alloc(4096,memory->size);CHECK(memory->vaddr!=NULL);
 memory_count++;memory_bytes+=memory->size;return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *memory)
{
 if(free_fail)return HAL_ERR_NOMEM;
 CHECK(memory_count && memory_bytes>=memory->size);
 memory_count--;memory_bytes-=memory->size;free(memory->vaddr);return HAL_OK;
}
int cache_memory_reserve(enum cache_memory_kind kind,size_t bytes,int optional)
{ CHECK(kind==CACHE_MEMORY_IO_POOL && !optional);(void)bytes;return credit_fail ? ENOMEM : 0; }
void cache_memory_commit(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_IO_POOL);charged_bytes+=bytes; }
void cache_memory_release(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_IO_POOL && charged_bytes>=bytes);charged_bytes-=bytes; }
int kthread_create(void (*entry)(void *),void *arg,int priority,struct thread **result)
{
 (void)priority;if(thread_fail)return ENOMEM;CHECK(created<ASYNC_ENDPOINTS);
 worker_entries[created]=entry;worker_arguments[created]=arg;
 *result=&fake_threads[created++];return 0;
}
void thread_start(struct thread *thread)
{
 unsigned index=(unsigned)(thread-fake_threads);
 host_workers[index]=host_thread_start(worker_entries[index],worker_arguments[index]);
}
void backing_claim_ref(const struct backing_claim *claim) { CHECK(claim==NULL); }
void backing_claim_release(struct backing_claim *claim) { CHECK(claim==NULL); }

static struct inode loop_origin;
int hal_printf(const char *format,...) { (void)format;return 0; }
ssize_t file_pread(struct file *file,void *data,size_t length,off_t offset)
{
 int error;(void)file;
 error=disk_read_direct(devices[1],(uint64_t)offset/512,(uint32_t)(length/512),data);
 return error ? -error : (ssize_t)length;
}
ssize_t file_pwrite_context(struct file *file,const void *data,size_t length,off_t offset,
 unsigned flags,const struct ucred *credential,const struct io_context *context)
{
 int error;(void)file;(void)credential;
 CHECK(flags==(FILE_IO_LOOP_BACKING|FILE_IO_DRAIN));
 CHECK(context->origin_inode==&loop_origin && context->content_generation==73);
 CHECK(context->flags & IO_CONTEXT_DRAIN);
 error=disk_write_direct_context(devices[1],(uint64_t)offset/512,(uint32_t)(length/512),data,context);
 return error ? -error : (ssize_t)length;
}
int file_fsync_backend(struct file *file) { (void)file;return bio_flush(devices[1]); }
static void test_writeback_domain(void);
static void test_loop_worker(void)
{
 static const struct disk_ops ops={.submit=loop_submit};
 struct loop_device loop={0};struct file file={0};
 struct disk *disk;struct bio_async_request *request;
 struct io_context context;unsigned char bytes[512];
 disk=disk_alloc();CHECK(disk!=NULL);strcpy(disk->d_name,"asyncloop");
 disk->d_block_size=512;disk->d_block_count=16;disk->d_ops=&ops;disk->d_data=&loop;
 loop.attached=true;loop.backing=&file;loop.flags=LOOP_READ_WRITE;loop.size_bytes=8192;
 CHECK(disk_create(disk)==0 && bio_async_enable(disk)==0);
 CHECK(io_context_child(&context,NULL,0)==0);context.origin_inode=&loop_origin;
 context.content_generation=73;refcount_init(&loop_origin.i_refs,1);
 memset(bytes,0x5a,sizeof(bytes));
 CHECK(bio_async_prepare(disk,BIO_WRITE,2,1,bytes,NULL,&context,NULL,NULL,&request)==0);
 CHECK(refcount_load(&loop_origin.i_refs)==2);
 CHECK(bio_async_submit(request)==0 && bio_async_wait(request)==0);
 CHECK(media[1][1024]==0x5a);bio_async_release(request);
 for(;;) { int error=bio_async_disable(disk);if(error==0)break;CHECK(error==EBUSY);host_thread_yield(); }
 CHECK(refcount_load(&loop_origin.i_refs)==1);
 CHECK(disk_gone_if_idle(disk)==0 && disk_destroy(disk)==0);
}
static void complete_callback(struct bio_async_request *request,void *argument)
{
 (void)request_paddr; (void)request_size; (void)request_alignment; (void)request_type; (void)request_attr;(void)argument;__atomic_fetch_add(&callbacks,1,__ATOMIC_RELEASE);
}
static void release_callback(struct bio_async_request *request,void *argument)
{
 (void)argument;__atomic_fetch_add(&callbacks,1,__ATOMIC_RELEASE);
 bio_async_release(request);
}
static int controlled_submit(struct disk *disk,struct bio *bio)
{
 unsigned index=disk==devices[0]?0:1;
 size_t bytes=(size_t)bio->b_block_count*512;
 if(index==0 && bio->b_block==0 && __atomic_exchange_n(&pause_first,0,__ATOMIC_RELAXED))host_gate_pause(1);
 if(bio->b_block==10) {
  __atomic_store_n(&late_bio,bio,__ATOMIC_RELEASE);return 0;
 }
 if(bio->b_block==11)return EIO;
 if(bio->b_block==13){bio_complete(bio,0,256);return 0;}
 CHECK(bio->b_block*512+bytes<=sizeof(media[index]));
 if(bio->b_op==BIO_READ)memcpy(bio->b_data,media[index]+bio->b_block*512,bytes);
 else if(bio->b_op==BIO_WRITE)memcpy(media[index]+bio->b_block*512,bio->b_data,bytes);
 if(index==0 && bio->b_op==BIO_FLUSH)CHECK(media[0][1024]==0x31);
 bio_complete(bio,0,bytes);return 0;
}
static const struct disk_ops controlled_ops={.submit=controlled_submit};
static void wait_idle(struct disk *disk)
{
 struct bio_async_endpoint *endpoint;
 unsigned index,used;unsigned long irq;
 for(;;) {
  used=0;
  for(index=0;index<ASYNC_ENDPOINTS;index++) {
   endpoint=&async_endpoints[index];irq=spin_lock_irqsave(&endpoint->lock);
   if(endpoint->leaf==disk)used+=endpoint->used;
   spin_unlock_irqrestore(&endpoint->lock,irq);
  }
  if(!used)return;
  host_thread_yield();
 }
}
static void cancel_worker(void *argument)
{
 struct bio_async_request *request=argument;
 cancel_result=bio_async_cancel(request);bio_async_release(request);
}
int main(void)
{
 struct bio_async_request *requests[5],*other,*late,*discard;
 struct bio *pending;
 unsigned char input[512];const void *data;size_t transferred;
 unsigned index,before,round;void *canceller;int completion;
 memset(input,0x31,sizeof(input));memset(media[1],0x72,sizeof(media[1]));
 for(index=0;index<2;index++) {
  devices[index]=disk_alloc();CHECK(devices[index]);
  strcpy(devices[index]->d_name,index?"async1":"async0");
  devices[index]->d_block_size=512;devices[index]->d_block_count=128;
  devices[index]->d_ops=&controlled_ops;CHECK(disk_create(devices[index])==0);
 }
 memory_fail=1;CHECK(bio_async_enable(devices[0])==ENOMEM);memory_fail=0;
 credit_fail=1;CHECK(bio_async_enable(devices[0])==ENOMEM);credit_fail=0;
 thread_fail=1;CHECK(bio_async_enable(devices[0])==ENOMEM);thread_fail=0;
 CHECK(memory_count==0 && charged_bytes==0 && devices[0]->d_cache_users==0);
 CHECK(bio_async_enable(devices[0])==0);CHECK(bio_async_enable(devices[1])==0);
 CHECK(charged_bytes==memory_bytes && memory_count==2);
 for(index=0;index<4;index++)
  CHECK(bio_async_prepare(devices[0],index==3?BIO_FLUSH:BIO_WRITE,index,index==3?0:1,index==3?NULL:input,NULL,NULL,complete_callback,NULL,&requests[index])==0);
 CHECK(bio_async_prepare(devices[0],BIO_WRITE,4,1,input,NULL,NULL,NULL,NULL,&requests[4])==EAGAIN);
 memset(input,0x99,sizeof(input));
 host_gate_reset(1);pause_first=1;
 CHECK(bio_async_submit(requests[0])==0);host_gate_wait(1);
 CHECK(bio_async_cancel(requests[0])==EBUSY);
 CHECK(bio_async_submit(requests[1])==0);CHECK(bio_async_submit(requests[2])==0);
 CHECK(bio_async_submit(requests[3])==EAGAIN);CHECK(bio_async_disable(devices[0])==EBUSY);
 CHECK(bio_async_cancel(requests[1])==0);CHECK(bio_async_wait(requests[1])==ECANCELED);
 CHECK(bio_async_cancel(requests[1])==EALREADY);
 CHECK(bio_async_submit(requests[3])==0);
 CHECK(bio_async_prepare(devices[1],BIO_READ,0,1,NULL,NULL,NULL,NULL,NULL,&other)==0);
 CHECK(bio_async_submit(other)==0);CHECK(bio_async_wait(other)==0);
 CHECK(bio_async_result(other,&data,&transferred)==0 && transferred==512);
 CHECK(((const unsigned char *)data)[0]==0x72);bio_async_release(other);
 CHECK(media[0][0]==0);host_gate_release(1);
 for(index=0;index<4;index++) {
  CHECK(bio_async_wait(requests[index])==(index==1?ECANCELED:0));bio_async_release(requests[index]);
 }
 wait_idle(devices[0]);CHECK(callbacks==4 && media[0][0]==0x31 && media[0][512]==0);
 devices[1]->d_max_transfer_blocks=2;
 CHECK(bio_async_prepare(devices[1],BIO_READ,0,3,NULL,NULL,NULL,NULL,NULL,&other)==E2BIG);
 devices[1]->d_max_transfer_blocks=0;
 CHECK(bio_async_prepare(devices[1],BIO_READ,0,129,NULL,NULL,NULL,NULL,NULL,&other)==EINVAL);
 CHECK(bio_async_prepare(devices[1],BIO_READ,0,128,NULL,NULL,NULL,NULL,NULL,&other)==0);
 CHECK(bio_async_submit(other)==0 && bio_async_wait(other)==0);
 CHECK(bio_async_result(other,&data,&transferred)==0 && transferred==65536);
 CHECK(memcmp(data,media[1],65536)==0);bio_async_release(other);wait_idle(devices[1]);
 test_loop_worker();
 test_writeback_domain();
 /* A same-medium recovery retires proof, not the successful request's identity. */
 CHECK(bio_async_prepare(devices[0],BIO_READ,10,1,NULL,NULL,NULL,complete_callback,NULL,&late)==0);
 CHECK(bio_async_submit(late)==0);
 while((pending=__atomic_load_n(&late_bio,__ATOMIC_ACQUIRE))==NULL)host_thread_yield();
 disk_persistence_forget(devices[0]);
 bio_complete(pending,0,512);CHECK(bio_async_wait(late)==0);
 CHECK(bio_async_result(late,&data,&transferred)==0 && data!=NULL && transferred==512);
 bio_async_release(late);wait_idle(devices[0]);
 __atomic_store_n(&late_bio,NULL,__ATOMIC_RELEASE);
 CHECK(bio_async_prepare(devices[0],BIO_READ,10,1,NULL,NULL,NULL,complete_callback,NULL,&late)==0);
 CHECK(bio_async_submit(late)==0);
 while((pending=__atomic_load_n(&late_bio,__ATOMIC_ACQUIRE))==NULL)host_thread_yield();
 CHECK(bio_async_cancel(late)==EBUSY);disk_persistence_invalidate(devices[0]);
 bio_complete(pending,0,512);CHECK(bio_async_wait(late)==ESTALE);
 CHECK(bio_async_result(late,&data,&transferred)==ESTALE && data==NULL);bio_async_release(late);
 CHECK(bio_async_prepare(devices[1],BIO_READ,11,1,NULL,NULL,NULL,complete_callback,NULL,&other)==0);
 CHECK(bio_async_submit(other)==0);CHECK(bio_async_wait(other)==EIO);bio_async_release(other);
 CHECK(bio_async_prepare(devices[1],BIO_READ,13,1,NULL,NULL,NULL,NULL,NULL,&other)==0);
 CHECK(bio_async_submit(other)==0);CHECK(bio_async_wait(other)==EIO);bio_async_release(other);
 for(round=0;round<100;round++) {
  before=__atomic_load_n(&callbacks,__ATOMIC_ACQUIRE);
  CHECK(bio_async_prepare(devices[1],BIO_READ,0,1,NULL,NULL,NULL,complete_callback,NULL,&other)==0);
  bio_async_ref(other);CHECK(bio_async_submit(other)==0);
  canceller=host_thread_start(cancel_worker,other);
  completion=bio_async_wait(other);host_thread_join(canceller);
  CHECK(completion==0 || completion==ECANCELED);
  CHECK(cancel_result==0 || cancel_result==EBUSY || cancel_result==EALREADY);
  if(cancel_result==0)CHECK(completion==ECANCELED);
  bio_async_release(other);wait_idle(devices[1]);
  CHECK(__atomic_load_n(&callbacks,__ATOMIC_ACQUIRE)==before+1);
 }
 before=__atomic_load_n(&callbacks,__ATOMIC_ACQUIRE);
 CHECK(bio_async_prepare(devices[1],BIO_READ,12,1,NULL,NULL,NULL,release_callback,NULL,&discard)==0);
 CHECK(bio_async_submit(discard)==0);
 while(__atomic_load_n(&callbacks,__ATOMIC_ACQUIRE)==before)host_thread_yield();
 wait_idle(devices[0]);wait_idle(devices[1]);
 free_fail=1;CHECK(bio_async_disable(devices[1])==EIO);free_fail=0;
 CHECK(memory_count==2 && charged_bytes==memory_bytes);
 CHECK(bio_async_disable(devices[0])==0);CHECK(bio_async_disable(devices[1])==0);
 CHECK(memory_count==0 && memory_bytes==0 && charged_bytes==0);
 CHECK(bio_async_enable(devices[1])==0 && created==3);
 late_bio=NULL;
 CHECK(bio_async_prepare(devices[1],BIO_READ,10,1,NULL,NULL,NULL,NULL,NULL,&late)==0);
 CHECK(bio_async_submit(late)==0);
 while((pending=__atomic_load_n(&late_bio,__ATOMIC_ACQUIRE))==NULL)host_thread_yield();
 CHECK(bio_async_prepare(devices[1],BIO_READ,0,1,NULL,NULL,NULL,NULL,NULL,&other)==0);
 CHECK(bio_async_submit(other)==0);disk_gone(devices[1]);
 bio_complete(pending,0,512);CHECK(bio_async_wait(late)==ESTALE);
 CHECK(bio_async_wait(other)==ENXIO);bio_async_release(late);bio_async_release(other);
 wait_idle(devices[1]);CHECK(bio_async_disable(devices[1])==0);
 CHECK(bio_async_enable(devices[1])==ENXIO);
 CHECK(memory_bytes==0 && charged_bytes==0);
 __atomic_store_n(&stopping,1,__ATOMIC_RELEASE);
 for(index=0;index<created;index++)host_thread_join(host_workers[index]);
 for(index=0;index<2;index++) {
  CHECK(devices[index]->d_cache_users==0 && devices[index]->d_inflight==0);
  if(index==0)CHECK(disk_gone_if_idle(devices[index])==0);
  CHECK(disk_destroy(devices[index])==0);
 }
 printf("owned asynchronous BIO PASS: return-before-device, full/cancel, independent device, snapshot, late epoch, callback release, rollback (%u checks)\n",checks);
 return 0;
}

/* Exercises actual loop resolution and disk lifecycle admission together. */
static void test_writeback_domain(void)
{
 struct disk *outer,*inner,*partition,*leaf;
 struct mount mount[2];struct inode inode[2];
 unsigned before,index;
 memset(mount,0,sizeof(mount));memset(inode,0,sizeof(inode));
 partition=disk_alloc();CHECK(partition!=NULL);strcpy(partition->d_name,"wbpart");
 partition->d_block_size=512;partition->d_block_count=64;
 partition->d_parent=devices[1];partition->d_ops=&controlled_ops;
 CHECK(disk_create(partition)==0);
 inner=disk_alloc();outer=disk_alloc();CHECK(inner!=NULL && outer!=NULL);
 strcpy(inner->d_name,"wbinner");strcpy(outer->d_name,"wbouter");
 for(index=0;index<2;index++) {
  struct disk *disk=index?outer:inner;
  disk->d_block_size=512;disk->d_block_count=32;disk->d_ops=&loop_disk_ops;
  CHECK(disk_create(disk)==0);
  loops[index].attached=true;loops[index].disk=disk;
  loops[index].backing_inode=&inode[index];inode[index].i_mount=&mount[index];
 }
 mount[0].m_disk=partition;mount[1].m_disk=inner;
 before=devices[1]->d_cache_users;
 CHECK(writeback_domain_acquire(outer,&leaf)==0 && leaf==devices[1]);
 CHECK(outer->d_cache_users==0 && inner->d_cache_users==0);
 CHECK(devices[1]->d_cache_users==before+1);disk_cache_release(leaf);
 CHECK(writeback_domain_acquire(partition,&leaf)==0 && leaf==devices[1]);
 disk_cache_release(leaf);
 loops[0].detaching=true;
 CHECK(writeback_domain_acquire(outer,&leaf)==ENXIO && leaf==NULL);
 CHECK(outer->d_cache_users==0 && inner->d_cache_users==0);
 loops[0].detaching=false;
 mount[0].m_disk=outer;
 CHECK(writeback_domain_acquire(outer,&leaf)==ELOOP && leaf==NULL);
 CHECK(outer->d_cache_users==0 && inner->d_cache_users==0);
 CHECK(devices[1]->d_cache_users==before);
 memset(loops,0,sizeof(loops));
 CHECK(disk_gone_if_idle(outer)==0 && disk_destroy(outer)==0);
 CHECK(disk_gone_if_idle(inner)==0 && disk_destroy(inner)==0);
 CHECK(disk_gone_if_idle(partition)==0 && disk_destroy(partition)==0);
}
