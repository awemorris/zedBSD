/* Production disk/BIO frontier, deterministic completion scheduling.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define STORAGE_FOUNDATION_CUSTOM_LOCK
#include "../../ws018/tests/mount-thread-host.h"
#define STORAGE_FOUNDATION_CUSTOM_WAIT
#define main retained_foundation_main
#include "../../ws019/tests/storage-foundation-test.c"
#undef main
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
static struct disk *target_disk;
static struct bio *wait_completion;
static struct bio later_write;
static unsigned barriers, inject_later, barrier_error, callbacks;
static unsigned parallel_flush, barrier_paused, flush_waiters;
static unsigned sync_calls, sync_mutation, sync_error;
unsigned long spin_lock_irqsave(struct spinlock *s)
{
 unsigned expected;
 for (;;) {
  expected=0;
  if (__atomic_compare_exchange_n(&s->held.value,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))break;
  host_thread_yield();
 }
 __atomic_fetch_add(&locked,1,__ATOMIC_RELAXED);return 0;
}
void spin_unlock_irqrestore(struct spinlock *s,unsigned long irq)
{(void)irq;__atomic_fetch_sub(&locked,1,__ATOMIC_RELAXED);__atomic_store_n(&s->held.value,0,__ATOMIC_RELEASE);}
static int logical_sync(struct mount *mountp)
{sync_calls++;if(sync_mutation)io_epoch_mark(&mountp->m_write_epoch);return sync_error;}
static const struct filesystem_type logical_type={.sync=logical_sync};
static void flusher(void *arg){(void)arg;CHECK(bio_flush(target_disk)==0);}

static uint8_t payload[512];
static struct bio *terminal_watch;
void waitq_wake_all(struct wait_queue *q)
{
 __atomic_fetch_add(&q->sequence,1,__ATOMIC_RELEASE);
 if(terminal_watch && q==&terminal_watch->b_waitq) {
  CHECK(target_disk->d_inflight==0);
  CHECK(target_disk->d_write_head==NULL);
 }
}
int waitq_sleep(struct wait_queue *q,struct spinlock *s,uint64_t old,uint64_t deadline,unsigned flags)
{
 struct bio *bio=wait_completion;(void)q;(void)old;(void)deadline;(void)flags;
 if(parallel_flush) {
  __atomic_fetch_add(&flush_waiters,1,__ATOMIC_RELAXED);
  spin_unlock_irqrestore(s,0);host_thread_yield();spin_lock_irqsave(s);return EAGAIN;
 }
 CHECK(bio!=NULL);wait_completion=NULL;
 spin_unlock_irqrestore(s,0);bio_complete(bio,0,512);spin_lock_irqsave(s);
 return EAGAIN;
}
static void initialize_write(struct bio *bio)
{memset(bio,0,sizeof(*bio));bio->b_op=BIO_WRITE;bio->b_block_count=1;bio->b_data=payload;}
static int delayed_submit(struct disk *disk,struct bio *bio)
{
 if(bio->b_op==BIO_FLUSH) {
  barriers++;
  if(parallel_flush && !__atomic_exchange_n(&barrier_paused,1,__ATOMIC_RELAXED))host_gate_pause(1);
  if(inject_later) {inject_later=0;initialize_write(&later_write);CHECK(bio_submit(disk,&later_write)==0);}
  bio_complete(bio,barrier_error,0);
 }
 return 0;
}
static const struct disk_ops delayed_ops={.submit=delayed_submit};
static void callback_free(struct bio *bio)
{callbacks++;free(bio);}
int main(void)
{
 struct bio first,second,third;struct bio *owned;unsigned before;
 struct mount mountp={0};void *threads[4];unsigned i;
 struct io_error_snapshot failure;
 volatile uint64_t disk_cursor=0, independent_cursor=0;
 target_disk=disk_alloc();CHECK(target_disk!=NULL);
 strcpy(target_disk->d_name,"proof0");target_disk->d_block_size=512;
 target_disk->d_block_count=16;target_disk->d_ops=&delayed_ops;
 CHECK(disk_create(target_disk)==0);
 initialize_write(&first);first.b_op=(enum bio_op)99;
 CHECK(bio_submit(target_disk,&first)==EINVAL && target_disk->d_inflight==0);
 initialize_write(&first);first.b_context.flags=0x80;
 CHECK(bio_submit(target_disk,&first)==EOPNOTSUPP && first.b_state==BIO_NEW);

 CHECK(bio_flush(target_disk)==0);CHECK(bio_flush(target_disk)==0);CHECK(barriers==2);
 target_disk->d_flags|=DISK_FLUSH_PROOF;
 before=barriers;CHECK(bio_flush(target_disk)==0);CHECK(barriers==before+1);
 initialize_write(&first);initialize_write(&second);initialize_write(&third);
 CHECK(bio_submit(target_disk,&first)==0);CHECK(bio_submit(target_disk,&second)==0);
 CHECK(bio_submit(target_disk,&third)==0);
 bio_complete(&second,0,512);bio_complete(&third,0,512);
 CHECK(target_disk->d_write_completed==0 && target_disk->d_write_accepted==3);
 wait_completion=&first;CHECK(bio_flush(target_disk)==0);
 CHECK(target_disk->d_write_completed==3 && target_disk->d_write_stable==3);
 before=barriers;CHECK(bio_flush(target_disk)==0);CHECK(barriers==before);
 initialize_write(&first);CHECK(bio_submit(target_disk,&first)==0);bio_complete(&first,0,512);
 inject_later=1;CHECK(bio_flush(target_disk)==0);
 CHECK(target_disk->d_write_stable==4 && target_disk->d_write_accepted==5);
 wait_completion=&later_write;CHECK(bio_flush(target_disk)==0);CHECK(target_disk->d_write_stable==5);
 initialize_write(&first);CHECK(bio_submit(target_disk,&first)==0);bio_complete(&first,0,256);
 CHECK(first.b_error==EIO);
 io_error_snapshot(&target_disk->d_write_error,&failure);
 CHECK(io_error_observe(&failure,&disk_cursor)==EIO);
 CHECK(io_error_observe(&failure,&disk_cursor)==0);
 CHECK(io_error_observe(&failure,&independent_cursor)==EIO);
 CHECK(first.b_context.media_disk==target_disk);
 CHECK(!target_disk->d_stable_valid);CHECK(bio_flush(target_disk)==0);
 disk_persistence_invalidate(target_disk);before=barriers;
 barrier_error=EIO;CHECK(bio_flush(target_disk)==EIO);CHECK(!target_disk->d_stable_valid);
 barrier_error=0;CHECK(bio_flush(target_disk)==0);CHECK(barriers==before+2);
 /* The terminal wake observes completed bookkeeping, including the final pin. */
 initialize_write(&first);CHECK(bio_submit(target_disk,&first)==0);terminal_watch=&first;
 bio_complete(&first,0,512);terminal_watch=NULL;CHECK(bio_wait(&first)==0);
 owned=malloc(sizeof(*owned));CHECK(owned!=NULL);initialize_write(owned);owned->b_done=callback_free;
 CHECK(bio_submit(target_disk,owned)==0);CHECK(bio_wait(owned)==EINVAL);
 bio_complete(owned,0,512);CHECK(callbacks==1);
 /* Concurrent fsync callers share the same successfully persisted target. */
 disk_persistence_invalidate(target_disk);before=barriers;
 parallel_flush=1;barrier_paused=0;flush_waiters=0;host_gate_reset(1);
 threads[0]=host_thread_start(flusher,NULL);host_gate_wait(1);
 for(i=1;i<4;i++)threads[i]=host_thread_start(flusher,NULL);
 while(!__atomic_load_n(&flush_waiters,__ATOMIC_RELAXED))host_thread_yield();
 host_gate_release(1);for(i=0;i<4;i++)host_thread_join(threads[i]);parallel_flush=0;
 CHECK(barriers==before+1);
 /* Logical dirty state is drained even with no additional accepted leaf write. */
 mountp.m_type=&logical_type;io_epoch_mark(&mountp.m_write_epoch);
 sync_mutation=1;CHECK(mount_sync(&mountp)==0);
 CHECK(mountp.m_write_epoch.stable==1 && mountp.m_write_epoch.dirty==2);
 sync_mutation=0;sync_error=EIO;CHECK(mount_sync(&mountp)==EIO);
 CHECK(mountp.m_write_epoch.stable==1 && mountp.m_write_epoch.dirty==3);
 sync_error=0;CHECK(mount_sync(&mountp)==0);
 CHECK(mountp.m_write_epoch.stable==3 && sync_calls==3);
 io_epoch_begin(&mountp.m_write_epoch);
 CHECK(mount_sync(&mountp)==0 && mountp.m_write_epoch.stable==3);
 io_epoch_end(&mountp.m_write_epoch);
 CHECK(mount_sync(&mountp)==0 && mountp.m_write_epoch.stable==4);
 /* A prior owner error is independently visible to the real mount observer. */
 io_error_record(&mountp.m_write_error,ENOSPC);
 io_error_snapshot(&mountp.m_write_error,&failure);
 independent_cursor=0;CHECK(io_error_observe(&failure,&independent_cursor)==ENOSPC);
 CHECK(mount_sync(&mountp)==ENOSPC);CHECK(mount_sync(&mountp)==0);
 sync_error=EIO;CHECK(mount_sync(&mountp)==EIO);CHECK(mount_sync(&mountp)==EIO);
 sync_error=0;CHECK(mount_sync(&mountp)==0);
 /* Internal barriers cannot certify VM data or consume the public error cursor. */
 before=(unsigned)mountp.m_write_epoch.stable;
 io_epoch_mark(&mountp.m_write_epoch);
 CHECK(mount_sync_backend(&mountp)==0);
 CHECK(mountp.m_write_epoch.stable==before);
 independent_cursor=mountp.m_write_error_cursor;
 sync_error=EIO;CHECK(mount_sync_backend(&mountp)==EIO);
 CHECK(mountp.m_write_error_cursor==independent_cursor);
 sync_error=0;CHECK(mount_sync(&mountp)==EIO);
 CHECK(mount_sync(&mountp)==0);
 before=(unsigned)mountp.m_write_epoch.stable;
 mountp.m_write_epoch.dirty=UINT64_MAX;io_epoch_mark(&mountp.m_write_epoch);
 CHECK(mount_sync(&mountp)==0 && mountp.m_write_epoch.stable==before);
 target_disk->d_persist_epoch=UINT64_MAX;before=barriers;
 CHECK(bio_flush(target_disk)==0);CHECK(bio_flush(target_disk)==0);CHECK(barriers==before+2);
 CHECK(target_disk->d_cache_users==0 && !target_disk->d_flush_busy);
 CHECK(disk_gone_if_idle(target_disk)==0);CHECK(disk_destroy(target_disk)==0);
 printf("flush frontier PASS: holes, captured target, repeats, unknown policy, short/error/reset, terminal lifetime (%u checks)\n",checks);
 return 0;
}
