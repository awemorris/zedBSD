/* Production policy, worker and dirty-credit ownership under concurrent control. */
#include <kern/writeback.h>
#include <kern/cache-memory.h>
#include <kern/disk.h>
#include <kern/mount.h>
#include <kern/thread.h>
#include <kern/sched.h>
#include <kern/page.h>
#include <zedbsd/sysctl.h>
#include <hal/hal.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../ws018/tests/mount-thread-host.h"

#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED); if(!(x)){fprintf(stderr,"policy:%u %s\n",__LINE__,#x);abort();} } while(0)
extern void async_host_exit(void);
static unsigned checks,stopping,created,allocations;
static unsigned allocation_fail,account_fail,thread_fail,free_fail;
static uint64_t ticks,charged;
static void (*entries[4])(void *);
static void *arguments[4],*threads[4];
static struct thread thread_records[4];
static struct disk disks[5];
static struct mount mounts[6];
static struct writeback_budget *owners[6];
static uint64_t dirty[6];
static unsigned sync_calls[6],sync_fail[6],block_first;
static int off_result;
int disk_media_status(const struct disk *disk)
{ return disk == NULL || disk->d_media_revoked ? ENXIO : 0; }
static unsigned ready[4];
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
{ CHECK(kind==CACHE_MEMORY_WORKER && !optional && bytes>=65536);return account_fail?ENOMEM:0; }
void cache_memory_commit(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_WORKER);charged+=bytes; }
void cache_memory_release(enum cache_memory_kind kind,size_t bytes)
{ CHECK(kind==CACHE_MEMORY_WORKER && charged>=bytes);charged-=bytes; }
void cache_memory_get_stats(struct cache_memory_stats *stats)
{ memset(stats,0,sizeof(*stats));stats->target_bytes=64U*1024U*1024U; }
void disk_ref(struct disk *disk) { refcount_get(&disk->d_refs); }
void disk_release(struct disk *disk) { CHECK(!refcount_put(&disk->d_refs)); }
int disk_cache_acquire(struct disk *disk,struct disk **leaf)
{ disk_ref(disk);__atomic_add_fetch(&disk->d_cache_users,1,__ATOMIC_RELAXED);*leaf=disk;return 0; }
void disk_cache_release(struct disk *disk)
{ CHECK(__atomic_fetch_sub(&disk->d_cache_users,1,__ATOMIC_RELAXED)>0);disk_release(disk); }
void mount_ref(struct mount *mount) { refcount_get(&mount->m_refs); }
void mount_release(struct mount *mount) { CHECK(!refcount_put(&mount->m_refs)); }
int vm_object_sync_mount_buffer(struct mount *mount,void *scratch,size_t capacity)
{
 unsigned index=(unsigned)(mount-mounts);uint64_t bytes;
 CHECK(index<6 && scratch!=NULL && capacity==65536);
 if(index==0 && __atomic_exchange_n(&block_first,0,__ATOMIC_ACQ_REL))host_gate_pause(1);
 __atomic_add_fetch(&sync_calls[index],1,__ATOMIC_RELEASE);
 if(__atomic_load_n(&sync_fail[index],__ATOMIC_ACQUIRE))return EIO;
 bytes=__atomic_exchange_n(&dirty[index],0,__ATOMIC_ACQ_REL);
 if(bytes)writeback_budget_clean(owners[index],(size_t)bytes);
 return 0;
}
int mount_sync_backend(struct mount *mount)
{ CHECK(mount>=mounts && mount<mounts+6);return 0; }
struct mount *mount_find_ref(const char *path)
{
 unsigned index;
 for(index=0;index<6;index++) {
  if(strcmp(path,mounts[index].m_path)==0) {mount_ref(&mounts[index]);return &mounts[index];}
 }
 return NULL;
}
#include "writeback-sysctl.inc"

static int eligible(struct file *file,off_t offset,size_t size)
{ (void)file;(void)offset;(void)size;return 1; }
static const struct filesystem_type type={.fs_name="policy-test",.writeback_range=eligible};
static void commit_dirty(unsigned index)
{
 struct writeback_ticket ticket={0};
 CHECK(writeback_mount_admit(&mounts[index],&ticket)==0);
 owners[index]=ticket.budget;writeback_ticket_commit(&ticket,4096);
 __atomic_add_fetch(&dirty[index],4096,__ATOMIC_RELEASE);writeback_ticket_release(&ticket);
}
static void disable_first(void *unused)
{ (void)unused;off_result=writeback_mount_set(&mounts[0],0); }
int main(void)
{
 struct writeback_policy_stats stats;struct writeback_ticket ticket={0},probe={0};
 struct writeback_budget snapshot;unsigned index,before;void *controller;
 struct writeback_control control;struct writeback_report report;
 struct writeback_unmount unmount={0},other={0};
 unsigned char unaligned[sizeof(report)+1];size_t size;
 const int control_oid[]={CTL_VFS,VFS_WRITEBACK,VFS_WRITEBACK_CONTROL};
 const int stats_oid[]={CTL_VFS,VFS_WRITEBACK,VFS_WRITEBACK_STATS};
 for(index=0;index<5;index++){refcount_init(&disks[index].d_refs,1);snprintf(disks[index].d_name,sizeof(disks[index].d_name),"disk%u",index);}
 for(index=0;index<6;index++){
  refcount_init(&mounts[index].m_refs,1);mounts[index].m_state=MOUNT_STATE_LIVE;
  snprintf(mounts[index].m_path,sizeof(mounts[index].m_path),"/wb%u",index);
  mounts[index].m_type=&type;mounts[index].m_disk=&disks[index<5?index:0];
 }
 memset(&control,0,sizeof(control));control.version=WRITEBACK_REPORT_VERSION;
 control.enabled=1;strcpy(control.path,"/wb0");
 CHECK(sysctl_writeback(control_oid,NULL,NULL,&control,sizeof(control),0)==EPERM);
 CHECK(sysctl_writeback(control_oid,NULL,NULL,&control,sizeof(control)-1,1)==EINVAL);
 control.version++;CHECK(sysctl_writeback(control_oid,NULL,NULL,&control,sizeof(control),1)==EINVAL);control.version--;
 memset(control.path,'x',sizeof(control.path));CHECK(sysctl_writeback(control_oid,NULL,NULL,&control,sizeof(control),1)==EINVAL);
 strcpy(control.path,"/missing");CHECK(sysctl_writeback(control_oid,NULL,NULL,&control,sizeof(control),1)==ENOENT);
 strcpy(control.path,"/wb0");control.enabled=2;CHECK(sysctl_writeback(control_oid,NULL,NULL,&control,sizeof(control),1)==EINVAL);control.enabled=1;
 size=0;CHECK(sysctl_writeback(stats_oid,NULL,&size,NULL,0,0)==0 && size==sizeof(report));
 memset(unaligned,0x5a,sizeof(unaligned));size=sizeof(report)-1;
 CHECK(sysctl_writeback(stats_oid,unaligned+1,&size,NULL,0,0)==ENOMEM && size==sizeof(report));
 CHECK(unaligned[1]==0x5a);
 CHECK(sysctl_writeback(stats_oid,NULL,NULL,&control,sizeof(control),1)==EPERM);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==EAGAIN);
 CHECK(!writeback_mount_active(NULL) && !writeback_mount_active(&mounts[0]));
 CHECK(writeback_mount_set(NULL,1)==EINVAL);
 mounts[0].m_flags=MOUNT_READ_ONLY;CHECK(writeback_mount_set(&mounts[0],1)==EROFS);mounts[0].m_flags=0;
 allocation_fail=1;CHECK(writeback_mount_set(&mounts[0],1)==ENOMEM);allocation_fail=0;
 account_fail=1;CHECK(writeback_mount_set(&mounts[0],1)==ENOMEM);account_fail=0;
 thread_fail=1;CHECK(writeback_mount_set(&mounts[0],1)==ENOMEM);thread_fail=0;
 CHECK(allocations==0 && charged==0 && disks[0].d_cache_users==0);
 CHECK(refcount_load(&mounts[0].m_refs)==1 && refcount_load(&disks[0].d_refs)==1);
 /* Failed setup plus failed resource release remains bounded and reusable. */
 thread_fail=1;free_fail=1;CHECK(writeback_mount_set(&mounts[0],1)==ENOMEM);
 thread_fail=0;free_fail=0;
 writeback_policy_snapshot(&stats);CHECK(stats.mounts==0 && stats.workers==1);
 CHECK(allocations==1 && charged==65536+4096);
 CHECK(writeback_mount_set(&mounts[0],1)==0 && allocations==1 && created==1);
 CHECK(writeback_mount_active(&mounts[0]));
 CHECK(writeback_mount_set(&mounts[0],0)==0 && allocations==0);
 CHECK(!writeback_mount_active(&mounts[0]));
 for(index=0;index<4;index++)CHECK(writeback_mount_set(&mounts[index],1)==0);
 CHECK(writeback_mount_set(&mounts[4],1)==EAGAIN);
 CHECK(writeback_mount_set(&mounts[5],1)==0);
 CHECK(writeback_mount_set(&mounts[0],1)==0 && created==4);
 writeback_policy_snapshot(&stats);CHECK(stats.workers==4 && stats.mounts==5);
 CHECK(stats.memory_bytes==charged && charged==4*(65536+4096));
 size=sizeof(report);CHECK(sysctl_writeback(stats_oid,unaligned+1,&size,NULL,0,0)==0);
 memcpy(&report,unaligned+1,sizeof(report));
 CHECK(report.header.version==WRITEBACK_REPORT_VERSION && report.header.count==5 && report.header.workers==4);
 CHECK(report.header.memory_bytes==charged);
 CHECK(strcmp(report.mounts[0].path,"/wb0")==0 && strcmp(report.mounts[0].device,"disk0")==0);
 CHECK(report.mounts[0].state==WRITEBACK_STATE_LIVE && report.mounts[0].padding==0);
 CHECK(sysctl_writeback(control_oid,NULL,NULL,&control,sizeof(control),1)==0 && created==4);

 /* A reversible unmount drains but retains policy and refuses competing controls. */
 commit_dirty(0);sync_fail[0]=1;
 CHECK(writeback_unmount_begin_revoked(NULL,&unmount)==EINVAL);
 CHECK(writeback_unmount_begin_revoked(&mounts[0],&unmount)==EINVAL);
 disks[0].d_media_revoked=1;before=sync_calls[0];
 CHECK(writeback_unmount_begin_revoked(&mounts[0],&unmount)==0);
 CHECK(dirty[0]==4096 && sync_calls[0]==before && unmount.mount==&mounts[0]);
 CHECK(!writeback_mount_active(&mounts[0]) && writeback_mount_active(&mounts[1]));
 CHECK(writeback_unmount_begin_revoked(&mounts[5],&other)==EBUSY && other.mount==NULL);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==EAGAIN);
 writeback_unmount_finish(&unmount,0);
 CHECK(unmount.mount==NULL && dirty[0]==4096 && writeback_mount_active(&mounts[0]));
 disks[0].d_media_revoked=0; /* Reset the host-only eligibility fixture. */
 CHECK(writeback_unmount_begin(&mounts[0],&unmount)==EIO);
 CHECK(unmount.mount==NULL&&dirty[0]==4096);sync_fail[0]=0;
 CHECK(writeback_unmount_begin(&mounts[0],&unmount)==0&&dirty[0]==0);
 CHECK(!writeback_mount_active(&mounts[0]) && writeback_mount_active(&mounts[1]));
 CHECK(unmount.mount==&mounts[0]&&refcount_load(&mounts[0].m_refs)==2);
 CHECK(writeback_unmount_begin(&mounts[0],&unmount)==EINVAL);
 CHECK(writeback_unmount_begin(&mounts[5],&other)==EBUSY&&other.mount==NULL);
 CHECK(writeback_mount_set(&mounts[0],1)==EBUSY);
 CHECK(writeback_mount_set(&mounts[5],0)==EBUSY);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==EAGAIN);
 CHECK(writeback_mount_admit(&mounts[1],&ticket)==0);writeback_ticket_release(&ticket);
 writeback_unmount_finish(&unmount,0);
 CHECK(writeback_mount_active(&mounts[0]));
 CHECK(unmount.mount==NULL&&refcount_load(&mounts[0].m_refs)==2);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==0);writeback_ticket_release(&ticket);
 CHECK(writeback_unmount_begin(&mounts[5],&unmount)==0);
 writeback_unmount_finish(&unmount,1);
 CHECK(refcount_load(&mounts[5].m_refs)==1&&allocations==4);
 CHECK(writeback_mount_set(&mounts[5],1)==0);
 /* A committed last mount can retain a failed clean free, then reuse its budget. */
 CHECK(writeback_unmount_begin(&mounts[1],&unmount)==0);
 free_fail=1;writeback_unmount_finish(&unmount,1);free_fail=0;
 CHECK(refcount_load(&mounts[1].m_refs)==1&&allocations==4);
 CHECK(writeback_mount_set(&mounts[1],1)==0&&allocations==4);
 CHECK(writeback_mount_admit(&mounts[1],&ticket)==0);writeback_ticket_release(&ticket);
 CHECK(writeback_mount_admit(NULL,&ticket)==EINVAL);
 CHECK(writeback_mount_admit(&mounts[0],NULL)==EINVAL);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==0);
 CHECK(writeback_mount_admit(&mounts[5],&probe)==0 && probe.budget==ticket.budget);
 CHECK(writeback_budget_read(ticket.budget,&snapshot)==0 && snapshot.tickets==2);
 CHECK(writeback_budget_read(ticket.budget,ticket.budget)==EINVAL);
 writeback_ticket_release(&probe);writeback_ticket_release(&ticket);
 /* An interrupted control wait restores admission without consuming the live ticket. */
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==0);
 interrupt_wait=1;CHECK(writeback_mount_set(&mounts[0],0)==EINTR);
 CHECK(writeback_budget_read(ticket.budget,&snapshot)==0 && snapshot.tickets==1 && snapshot.quiescing==0);
 interrupt_wait=1;CHECK(writeback_unmount_begin(&mounts[0],&unmount)==EINTR && unmount.mount==NULL);
 interrupt_wait=1;CHECK(writeback_shutdown_begin()==EINTR);
 CHECK(writeback_mount_set(&mounts[0],1)==0);
 CHECK(writeback_budget_read(ticket.budget,&snapshot)==0 && snapshot.tickets==1 && snapshot.quiescing==0);
 writeback_ticket_release(&ticket);
 commit_dirty(0);commit_dirty(5);
 CHECK(writeback_mount_set(&mounts[5],0)==0 && dirty[5]==0 && dirty[0]==4096);
 CHECK(refcount_load(&mounts[5].m_refs)==1 && allocations==4);
 sync_fail[0]=1;CHECK(writeback_mount_set(&mounts[0],0)==EIO);
 CHECK(dirty[0]==4096 && refcount_load(&mounts[0].m_refs)==2);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==0);writeback_ticket_release(&ticket);
 sync_fail[0]=0;free_fail=1;CHECK(writeback_mount_set(&mounts[0],0)==EIO);free_fail=0;
 CHECK(dirty[0]==0 && allocations==4 && charged==4*(65536+4096));
 CHECK(writeback_mount_set(&mounts[0],0)==0 && allocations==3);
 CHECK(writeback_mount_set(&mounts[0],0)==0);
 CHECK(writeback_mount_set(&mounts[0],1)==0 && created==4 && allocations==4);

 /* Pressure wakes a device before the age deadline, using its existing reserve. */
 before=__atomic_load_n(&sync_calls[0],__ATOMIC_ACQUIRE);
 for(index=0;index<1024;index++)commit_dirty(0);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==0);
 writeback_pressure(ticket.budget);writeback_ticket_release(&ticket);
 while(__atomic_load_n(&sync_calls[0],__ATOMIC_ACQUIRE)==before)host_thread_yield();
 /* Wait for the drain body, not merely its entry counter. */
 while(__atomic_load_n(&dirty[0],__ATOMIC_ACQUIRE)!=0)host_thread_yield();
 /* Wait for the pass deadline to be installed before advancing the fake clock. */
 do { writeback_policy_snapshot(&stats);if(stats.busy)host_thread_yield(); } while(stats.busy);

 /* A blocked physical device cannot stop another worker, or lose an off waiter. */
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==0);
 for(index=0;index<created;index++)while(!__atomic_load_n(&ready[index],__ATOMIC_ACQUIRE))host_thread_yield();
 block_first=1;host_gate_reset(1);before=sync_calls[1];
 __atomic_store_n(&ticks,200,__ATOMIC_RELEASE);host_gate_wait(1);
 while(__atomic_load_n(&sync_calls[1],__ATOMIC_ACQUIRE)==before)host_thread_yield();
 controller=host_thread_start(disable_first,NULL);
 for(;;){int error=writeback_mount_admit(&mounts[0],&probe);if(error==EAGAIN)break;CHECK(error==0);writeback_ticket_release(&probe);host_thread_yield();}
 CHECK(writeback_mount_admit(&mounts[1],&probe)==0);writeback_ticket_release(&probe);
 writeback_ticket_release(&ticket);host_gate_release(1);host_thread_join(controller);
 CHECK(off_result==0 && refcount_load(&mounts[0].m_refs)==1);
 for(index=1;index<4;index++)CHECK(writeback_mount_set(&mounts[index],0)==0);
 CHECK(allocations==0 && charged==0);
 writeback_policy_snapshot(&stats);CHECK(stats.mounts==0 && stats.workers==0 && stats.busy==0);
 for(index=0;index<5;index++)CHECK(disks[index].d_cache_users==0 && refcount_load(&disks[index].d_refs)==1);
 /* Final shutdown is reversible until all filesystem barriers succeed. */
 block_first=0;
 CHECK(writeback_mount_set(&mounts[0],1)==0);
 CHECK(writeback_mount_set(&mounts[1],1)==0);
 CHECK(writeback_unmount_begin(&mounts[0],&unmount)==0);
 CHECK(writeback_shutdown_begin()==EBUSY);writeback_unmount_finish(&unmount,0);
 commit_dirty(0);sync_fail[0]=1;
 CHECK(writeback_shutdown_begin()==EIO && dirty[0]==4096);sync_fail[0]=0;
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==0);writeback_ticket_release(&ticket);
 CHECK(writeback_shutdown_begin()==0 && dirty[0]==0);
 CHECK(writeback_mount_admit(&mounts[0],&ticket)==EAGAIN);
 CHECK(writeback_mount_set(&mounts[4],1)==EBUSY);
 CHECK(writeback_unmount_begin(&mounts[1],&unmount)==EBUSY);
 writeback_shutdown_finish(0);
 CHECK(writeback_mount_admit(&mounts[1],&ticket)==0);writeback_ticket_release(&ticket);
 commit_dirty(1);CHECK(writeback_shutdown_begin()==0 && dirty[1]==0);
 writeback_shutdown_finish(1);
 CHECK(allocations==0 && charged==0);
 writeback_policy_snapshot(&stats);CHECK(stats.mounts==0 && stats.workers==0 && stats.busy==0);
 CHECK(writeback_mount_set(&mounts[0],1)==EBUSY);
 for(index=0;index<5;index++)CHECK(disks[index].d_cache_users==0 && refcount_load(&disks[index].d_refs)==1);
 __atomic_store_n(&stopping,1,__ATOMIC_RELEASE);
 for(index=0;index<created;index++)host_thread_join(threads[index]);
 printf("writeback policy PASS: shared leaf, bounded workers, rollback, failed off, retained credits, independent progress (%u checks)\n",checks);
 return 0;
}

size_t hal_space_get_page_size(int level) { (void)level;return 4096; }
