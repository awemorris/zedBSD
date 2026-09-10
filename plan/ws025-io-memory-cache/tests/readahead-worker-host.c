/* Exercise the actual asynchronous service with controlled HAL, VM and backend edges. */
#include <kern/readahead.h>
#include <kern/system-device.h>
#include <kern/writeback.h>
#include <kern/cache-memory.h>
#include <kern/file.h>
#include <kern/vm-object.h>
#include <kern/thread.h>
#include <kern/sched.h>
#include <kern/page.h>
#include <hal/hal.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../ws018-kernel-architecture/tests/mount-thread-host.h"
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED); if(!(x)){fprintf(stderr,"readahead:%u %s\n",__LINE__,#x);abort();} } while(0)
extern void async_host_exit(void);
static unsigned checks,stopping,created,allocations;
static unsigned allocation_fail,account_fail,thread_fail,free_fail;
static uint64_t ticks,charged;
static void (*entries[4])(void *);
static void *arguments[4],*threads[4];
static struct thread thread_records[4], submit_thread;
struct thread *thread_current(void) { return &submit_thread; }
static struct disk disks[5];
static struct mount mounts[5];
static struct inode inodes[5];
static struct file files[5];
static struct vm_object objects[5];
static unsigned ready[4],live[5],reads[5],published[5];
static unsigned prepare_gate,read_gate,complete_gate,abort_gate,read_error,short_read,stale;
static __thread int worker_index=-1;
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
void spin_init(struct spinlock *lock,enum lock_rank rank,const char *name)
{ memset(lock,0,sizeof(*lock));lock->rank=rank;lock->name=name; }
unsigned long spin_lock_irqsave(struct spinlock *lock)
{
 unsigned expected;
 for(;;){expected=0;if(__atomic_compare_exchange_n(&lock->held.value,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;host_thread_yield();}
}
void spin_unlock_irqrestore(struct spinlock *lock,unsigned long irq)
{ (void)irq;__atomic_store_n(&lock->held.value,0,__ATOMIC_RELEASE); }
int mutex_init(struct mutex *mutex,enum lock_rank rank,const char *name)
{ memset(mutex,0,sizeof(*mutex));spin_init(&mutex->guard,rank,name);return 0; }
void mutex_lock(struct mutex *mutex) { (void)spin_lock_irqsave(&mutex->guard); }
void mutex_unlock(struct mutex *mutex) { spin_unlock_irqrestore(&mutex->guard,0); }
void waitq_init(struct wait_queue *wait,const char *name)
{ memset(wait,0,sizeof(*wait));wait->name=name; }
uint64_t waitq_sequence(const struct wait_queue *wait)
{ return __atomic_load_n(&wait->sequence,__ATOMIC_ACQUIRE); }
void waitq_wake_all(struct wait_queue *wait)
{ __atomic_fetch_add(&wait->sequence,1,__ATOMIC_RELEASE); }
static int interrupt_wait;
int waitq_sleep(struct wait_queue *wait,struct spinlock *lock,uint64_t sequence,uint64_t deadline,unsigned flags)
{
 (void)wait;(void)sequence;(void)deadline;
 if((flags&WAITQ_INTERRUPTIBLE)!=0 && interrupt_wait){interrupt_wait=0;return EINTR;}
 if(worker_index>=0)__atomic_store_n(&ready[worker_index],1,__ATOMIC_RELEASE);
 if(worker_index<0 && (flags&WAITQ_INTERRUPTIBLE)!=0)host_gate_signal(5);
 spin_unlock_irqrestore(lock,0);
 if(__atomic_load_n(&stopping,__ATOMIC_ACQUIRE))async_host_exit();
 host_thread_yield();spin_lock_irqsave(lock);return EAGAIN;
}
uint64_t clock_ticks(void) { return __atomic_load_n(&ticks,__ATOMIC_ACQUIRE); }
int kthread_create(void (*entry)(void *),void *argument,int priority,struct thread **result)
{
 CHECK(priority==SCHED_PRIORITY_DEFAULT);
 if(thread_fail)return ENOMEM;
 CHECK(created<4);entries[created]=entry;arguments[created]=argument;
 *result=&thread_records[created++];return 0;
}
static void run_worker(void *argument)
{ unsigned index=(unsigned)(uintptr_t)argument;worker_index=(int)index;entries[index](arguments[index]); }
void thread_start(struct thread *thread)
{ unsigned index=(unsigned)(thread-thread_records);threads[index]=host_thread_start(run_worker,(void *)(uintptr_t)index); }
int hal_pmem_alloc(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr,struct hal_pmem *memory)
{
	(void)request_paddr; (void)request_alignment; (void)request_type; (void)request_attr;

 memset(memory,0,sizeof(*memory));if(allocation_fail)return HAL_ERR_NOMEM;
 memory->size=(request_size+4095)&~(size_t)4095;
 memory->vaddr=aligned_alloc(4096,memory->size);CHECK(memory->vaddr!=NULL);allocations++;return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *memory)
{ if(free_fail)return HAL_ERR_NOMEM;CHECK(allocations>0);allocations--;free(memory->vaddr);return HAL_OK; }
int cache_memory_reserve(enum cache_memory_kind kind,size_t bytes,int optional)
{ CHECK(kind==CACHE_MEMORY_WORKER && optional && bytes>=65536);return account_fail?ENOMEM:0; }
void cache_memory_commit(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_WORKER);charged+=bytes; }
void cache_memory_release(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_WORKER && charged>=bytes);charged-=bytes; }

int mutex_trylock(struct mutex *mutex)
{ unsigned expected=0;return __atomic_compare_exchange_n(&mutex->guard.held.value,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); }
int writeback_domain_acquire(struct disk *disk,struct disk **leaf)
{ __atomic_add_fetch(&disk->d_cache_users,1,__ATOMIC_RELAXED);*leaf=disk;return 0; }
void disk_cache_release(struct disk *disk)
{ CHECK(__atomic_fetch_sub(&disk->d_cache_users,1,__ATOMIC_RELAXED)>0); }
int vm_object_prefetch_prepare(struct inode *inode,off_t offset,size_t length,struct vm_object_prefetch *fill)
{
 unsigned index=(unsigned)(inode-inodes);CHECK(index<5 && fill->object==NULL);
 fill->object=&objects[index];fill->offset=offset;fill->length=length;
 __atomic_add_fetch(&live[index],1,__ATOMIC_RELEASE);
 if(__atomic_exchange_n(&prepare_gate,0,__ATOMIC_ACQ_REL))host_gate_pause(1);
 return 0;
}
void vm_object_prefetch_abort(struct vm_object_prefetch *fill)
{
 unsigned index;
 if(fill->object==NULL)return;
 index=(unsigned)(fill->object-objects);CHECK(index<5);
 if(__atomic_exchange_n(&abort_gate,0,__ATOMIC_ACQ_REL))host_gate_pause(4);
 CHECK(__atomic_fetch_sub(&live[index],1,__ATOMIC_RELEASE)>0);memset(fill,0,sizeof(*fill));
}
int vm_object_prefetch_complete(struct vm_object_prefetch *fill,const void *bytes,ssize_t received,size_t *count)
{
 unsigned index=(unsigned)(fill->object-objects);int error=0;
 CHECK(index<5 && bytes!=NULL);*count=0;
 if(__atomic_exchange_n(&complete_gate,0,__ATOMIC_ACQ_REL))host_gate_pause(3);
 if(received<0)error=(int)-received;
 else if((size_t)received!=fill->length)error=EIO;
 else if(__atomic_load_n(&stale,__ATOMIC_ACQUIRE))error=EAGAIN;
 else {*count=fill->length;__atomic_add_fetch(&published[index],1,__ATOMIC_RELEASE);}
 vm_object_prefetch_abort(fill);return error;
}
ssize_t file_pread_internal(struct file *file,void *buffer,size_t length,off_t offset,unsigned flags)
{
 unsigned index=(unsigned)(file-files);CHECK(index<5 && buffer!=NULL && length<=65536 && offset>=0 && flags==FILE_IO_VM_OBJECT);
 __atomic_add_fetch(&reads[index],1,__ATOMIC_RELEASE);
 if(__atomic_exchange_n(&read_gate,0,__ATOMIC_ACQ_REL))host_gate_pause(2);
 if(__atomic_load_n(&read_error,__ATOMIC_ACQUIRE))return -EIO;
 if(__atomic_load_n(&short_read,__ATOMIC_ACQUIRE))return (ssize_t)length-1;
 memset(buffer,0x53,length);return (ssize_t)length;
}
static void reset_origin(unsigned index)
{
 mutex_lock(&files[index].f_lock);readahead_reset(&files[index].f_readahead);readahead_cancel(&files[index]);mutex_unlock(&files[index].f_lock);
}
static int submit(unsigned index)
{
 struct readahead_request request={0};
 mutex_lock(&files[index].f_lock);files[index].f_readahead.valid=1;
 if(files[index].f_readahead.generation==0)files[index].f_readahead.generation=1;
 request.generation=files[index].f_readahead.generation;request.offset=4096;request.length=65536;
 mutex_unlock(&files[index].f_lock);
 return readahead_submit(&files[index],&inodes[index],&request);
}
static void wait_idle(void)
{
 struct readahead_stats stats;
 do{readahead_snapshot(&stats);host_thread_yield();}while(stats.jobs!=0);
}
static struct readahead_boundary boundary;
static unsigned boundary_done;
static int boundary_result,submit_result;
static int shutdown_sync_error;
static unsigned shutdown_commits, shutdown_aborts, shutdown_devices;
void shutdown_test_yield(void) { host_thread_yield(); }
int writeback_shutdown_begin(void)
{
 struct readahead_stats stats;readahead_snapshot(&stats);
 CHECK(stats.jobs==0 && stats.memory_bytes==0);return 0;
}
int mount_sync_all(void) { return shutdown_sync_error; }
void writeback_shutdown_finish(int committed)
{ if(committed)shutdown_commits++;else shutdown_aborts++; }
void drv_usb_shutdown(void)
{ CHECK(shutdown_commits==1);shutdown_devices++; }
static void do_shutdown(void *unused)
{
 (void)unused;boundary_result=system_shutdown_prepare();
 __atomic_store_n(&boundary_done,1,__ATOMIC_RELEASE);
}
static void do_boundary(void *arg)
{ boundary_result=readahead_boundary_begin(&boundary,arg);__atomic_store_n(&boundary_done,1,__ATOMIC_RELEASE); }
static void do_submit(void *arg)
{ submit_result=submit((unsigned)(uintptr_t)arg); }
int main(void)
{
 struct readahead_stats stats;struct readahead_request old={1,4096,65536};
 struct readahead_report report;
 struct readahead_boundary nested={0};unsigned index,before;void *controller,*submitter;
 readahead_report(&report);CHECK(sizeof(report)==88 && offsetof(struct readahead_report,version)==72);
 CHECK(report.version==READAHEAD_REPORT_VERSION && report.jobs==0 && report.memory_bytes==0);
 for(index=0;index<5;index++){
  mounts[index].m_disk=&disks[index];inodes[index].i_mount=&mounts[index];objects[index].file=&files[index];
  CHECK(mutex_init(&files[index].f_lock,LOCK_RANK_FILE,"test origin")==0);
 }
 /* Bootstrap/idle callers cannot wait for asynchronous filesystem ownership. */
 submit_thread.flags=THREAD_FLAG_IDLE;CHECK(submit(0)==EAGAIN && allocations==0);
 submit_thread.flags=0;
 /* Failed admission cannot strand a disk token; failed HAL free remains charged. */
 allocation_fail=1;CHECK(submit(0)==ENOMEM);allocation_fail=0;
 account_fail=1;CHECK(submit(0)==ENOMEM);account_fail=0;
 thread_fail=1;CHECK(submit(0)==ENOMEM);thread_fail=0;
 CHECK(disks[0].d_cache_users==0 && allocations==1);
 free_fail=1;CHECK(readahead_trim()==EIO);free_fail=0;
 readahead_snapshot(&stats);CHECK(stats.memory_bytes==69632 && stats.jobs==0);
 CHECK(readahead_trim()==0 && charged==0);
 /* Two slots per physical device remain bounded and no I/O starts during demand. */
 CHECK(readahead_demand_begin()==0);CHECK(submit(0)==0);CHECK(submit(0)==0);CHECK(submit(0)==EAGAIN);
 readahead_snapshot(&stats);CHECK(stats.jobs==2 && stats.running==0 && reads[0]==0);
 reset_origin(0);wait_idle();CHECK(reads[0]==0 && live[0]==0 && disks[0].d_cache_users==0);
 CHECK(readahead_submit(&files[0],&inodes[0],&old)==EAGAIN);
 readahead_demand_end();
 /* All four workers are reusable; a fifth independent physical device is refused. */
 CHECK(readahead_demand_begin()==0);
 for(index=0;index<4;index++){CHECK(submit(index)==0);CHECK(submit(index)==0);}
 CHECK(submit(4)==EAGAIN);readahead_snapshot(&stats);CHECK(stats.jobs==8 && created==4);
 for(index=0;index<4;index++)reset_origin(index);
 wait_idle();readahead_demand_end();
 /* A running read retains all ownership after close, then discards the result. */
 host_gate_reset(2);read_gate=1;before=published[0];CHECK(submit(0)==0);host_gate_wait(2);
 reset_origin(0);CHECK(live[0]==1 && disks[0].d_cache_users==1);
 CHECK(submit(1)==0);
 while(__atomic_load_n(&published[1],__ATOMIC_ACQUIRE)==0)host_thread_yield();
 CHECK(live[0]==1);host_gate_release(2);wait_idle();CHECK(published[0]==before);
 /* A boundary must join preparation, not merely queued or running device work. */
 host_gate_reset(1);prepare_gate=1;submitter=host_thread_start(do_submit,NULL);host_gate_wait(1);
 host_gate_reset(5);boundary_done=0;controller=host_thread_start(do_boundary,&mounts[0]);
 host_gate_wait(5);
 CHECK(!__atomic_load_n(&boundary_done,__ATOMIC_ACQUIRE));
 host_gate_release(1);host_thread_join(submitter);host_thread_join(controller);
 CHECK(submit_result==0 && boundary_result==0 && live[0]==0 && disks[0].d_cache_users==0);
 CHECK(submit(0)==EAGAIN);CHECK(submit(1)==0);wait_idle();readahead_boundary_end(&boundary);
 /* An adoption already claimed must finish before a mount boundary returns. */
 host_gate_reset(3);complete_gate=1;CHECK(submit(0)==0);host_gate_wait(3);
 host_gate_reset(5);boundary_done=0;controller=host_thread_start(do_boundary,&mounts[0]);
 host_gate_wait(5);
 CHECK(!__atomic_load_n(&boundary_done,__ATOMIC_ACQUIRE));
 host_gate_release(3);host_thread_join(controller);CHECK(boundary_result==0);readahead_boundary_end(&boundary);
 /* Cleanup remains visible until the final VM and physical references are released. */
 host_gate_reset(4);abort_gate=1;CHECK(submit(0)==0);host_gate_wait(4);
 host_gate_reset(5);boundary_done=0;controller=host_thread_start(do_boundary,&mounts[0]);
 host_gate_wait(5);
 CHECK(!__atomic_load_n(&boundary_done,__ATOMIC_ACQUIRE) && disks[0].d_cache_users==1);
 host_gate_release(4);host_thread_join(controller);CHECK(boundary_result==0);readahead_boundary_end(&boundary);
 /* Errors and stale completions never become successful speculative publication. */
 before=published[0];read_error=1;CHECK(submit(0)==0);wait_idle();read_error=0;
 short_read=1;CHECK(submit(0)==0);wait_idle();short_read=0;
 stale=1;CHECK(submit(0)==0);wait_idle();stale=0;CHECK(published[0]==before);
 readahead_snapshot(&stats);CHECK(stats.errors==2);
 /* Nested global/mount boundaries compose, and interrupted waits reopen admission. */
 CHECK(readahead_boundary_begin(&boundary,NULL)==0);CHECK(readahead_boundary_begin(&nested,&mounts[0])==0);
 CHECK(submit(0)==EAGAIN && submit(1)==EAGAIN);readahead_boundary_end(&boundary);CHECK(submit(0)==EAGAIN);
 readahead_boundary_end(&nested);CHECK(submit(0)==0);wait_idle();
 host_gate_reset(2);read_gate=1;CHECK(submit(0)==0);host_gate_wait(2);
 interrupt_wait=1;CHECK(readahead_boundary_begin(&boundary,&mounts[0])==EINTR);CHECK(!boundary.active);
 host_gate_release(2);wait_idle();CHECK(submit(0)==0);wait_idle();
 /* Real shutdown joins an in-flight reader, restores a failed barrier, then retries. */
 host_gate_reset(2);read_gate=1;CHECK(submit(0)==0);host_gate_wait(2);
 shutdown_sync_error=EIO;host_gate_reset(5);boundary_done=0;
 controller=host_thread_start(do_shutdown,NULL);host_gate_wait(5);
 CHECK(!__atomic_load_n(&boundary_done,__ATOMIC_ACQUIRE) && disks[0].d_cache_users==1);
 host_gate_release(2);host_thread_join(controller);
 CHECK(boundary_result==EIO && shutdown_devices==0 && shutdown_aborts==1 && charged==0);
 CHECK(submit(0)==0);wait_idle();shutdown_sync_error=0;
 CHECK(system_shutdown_prepare()==0 && shutdown_devices==1 && charged==0 && allocations==0);
 CHECK(submit(0)==EAGAIN);CHECK(system_shutdown_prepare()==0 && shutdown_devices==1);
 readahead_consumed(19,23);readahead_report(&report);
 CHECK(report.confirmed_useful_bytes==19 && report.retired_uncredited_bytes==23);
 CHECK(report.errors==2 && report.queue_refusals>0 && report.requested_bytes>0);
 CHECK(report.jobs==0 && report.running==0 && report.memory_bytes==0 && report.demand==0);
 for(index=0;index<5;index++)CHECK(live[index]==0 && disks[index].d_cache_users==0);
 __atomic_store_n(&stopping,1,__ATOMIC_RELEASE);
 for(index=0;index<created;index++)host_thread_join(threads[index]);
 printf("readahead worker: %u checks PASS\n",checks);return 0;
}

size_t hal_space_get_page_size(int level) { (void)level;return 4096; }
