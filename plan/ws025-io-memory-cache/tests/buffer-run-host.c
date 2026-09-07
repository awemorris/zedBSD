/* Production buffer cache and verbatim direct-transfer helper, host fault model.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kern/disk.h>
#include <kern/backing-claim.h>
#include "plan/ws018-kernel-architecture/tests/mount-thread-host.h"
#include "src/kern/buf.c"
#include "direct-transfer.inc"
#undef assert
#define assert(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

static unsigned char medium[262144];
static unsigned calls, reads, writes, fault_at, short_at, live_pages;
static unsigned fail_alloc, alloc_hook, check_preparation;
static unsigned long held_locks;
static struct disk device;
static struct disk nested_device;
static unsigned char nested_medium[262144];
static unsigned nesting;
static unsigned concurrency;
static unsigned paused;
static unsigned guard_error;
static unsigned wait_error, redirty;
static unsigned char input[65536], output[65536];

void spin_init(struct spinlock *s, enum lock_rank r, const char *n)
{ memset(s, 0, sizeof(*s)); s->rank = r; s->name = n; }
unsigned long spin_lock_irqsave(struct spinlock *s)
{
 unsigned expected;
 do {
  expected = 0;
  if (__atomic_compare_exchange_n(&s->held.value, &expected, 1, 0,
      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
  host_thread_yield();
 } while (1);
 __atomic_fetch_add(&held_locks, 1, __ATOMIC_RELAXED); return 0;
}
void spin_unlock_irqrestore(struct spinlock *s, unsigned long f)
{ (void)f; __atomic_fetch_sub(&held_locks, 1, __ATOMIC_RELAXED); __atomic_store_n(&s->held.value, 0, __ATOMIC_RELEASE); }
int mutex_init(struct mutex *m, enum lock_rank r, const char *n)
{ memset(m,0,sizeof(*m)); spin_init(&m->guard,r,n); return 0; }
void mutex_lock(struct mutex *m) { spin_lock_irqsave(&m->guard); }
void mutex_unlock(struct mutex *m) { spin_unlock_irqrestore(&m->guard,0); }
void waitq_init(struct wait_queue *q, const char *n) { memset(q,0,sizeof(*q)); q->name=n; }
uint64_t waitq_sequence(const struct wait_queue *q) { return __atomic_load_n(&q->sequence,__ATOMIC_RELAXED); }
void waitq_wake_all(struct wait_queue *q) { __atomic_fetch_add(&q->sequence,1,__ATOMIC_RELAXED); }
int waitq_sleep(struct wait_queue *q, struct spinlock *s, uint64_t seq, uint64_t d, unsigned f)
{ (void)q;(void)seq;(void)d;(void)f; spin_unlock_irqrestore(s,0); host_thread_yield(); spin_lock_irqsave(s); return EAGAIN; }
struct thread *thread_current(void) { return (struct thread *)1; }
int hal_printf(const char *f, ...) { (void)f; return 0; }
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
void disk_ref(struct disk *d) { (void)d; }
void disk_release(struct disk *d) { (void)d; }
int disk_buffer_acquire(struct disk *d)
{ if(d->d_media_revoked)return ENXIO;d->d_buffer_refs++;return 0; }
void disk_buffer_release(struct disk *d)
{ assert(d->d_buffer_refs);d->d_buffer_refs--; }
size_t hal_pmem_get_total_size(void) { return 64U*1024U*1024U; }
int hal_pmem_alloc(const struct hal_pmem_request *r, struct hal_pmem *m)
{
 unsigned bucket; struct buf *b;
 if (check_preparation) {
  for (bucket=0;bucket<BUF_HASH_BUCKETS;bucket++)
   for (b=cache_hash[bucket];b;b=b->b_hash_next) assert(!b->b_busy);
 }
 if (fail_alloc) { fail_alloc--; if (!fail_alloc) return HAL_ERR_NOMEM; }
 if (alloc_hook) {
  alloc_hook=0;
  (void)buf_reclaim(4096,BUF_RECLAIM_WRITE);
  assert(buf_set_max_bytes(65536)==0);
  assert(buf_read(&nested_device,0,8,output)==0);
 }
 m->size=(r->size+4095)&~(size_t)4095; m->vaddr=aligned_alloc(4096,m->size);
 if (!m->vaddr) return HAL_ERR_NOMEM;
 __atomic_fetch_add(&live_pages,1,__ATOMIC_RELAXED); return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *m)
{ __atomic_fetch_sub(&live_pages,1,__ATOMIC_RELAXED); free(m->vaddr); return HAL_OK; }
int disk_resolve_range(struct disk *d,uint64_t b,uint32_t n,struct disk **leaf,uint64_t *mapped)
{ if (b>d->d_block_count || n>d->d_block_count-b) return EIO; *leaf=d;*mapped=b;return 0; }
int backing_mutation_begin_disk(struct disk *d,uint64_t b,uint64_t n,
 const struct backing_claim *c,struct backing_mutation_guard *g)
{ (void)d;(void)b;(void)n;(void)c;memset(g,0,sizeof(*g));return guard_error; }
void backing_mutation_end(struct backing_mutation_guard *g) { (void)g; }
static const struct io_context *expected_context;
int bio_submit(struct disk *d,struct bio *b)
{
 size_t size=(size_t)b->b_block_count*d->d_block_size;
 unsigned serial=__atomic_add_fetch(&calls,1,__ATOMIC_RELAXED);
 unsigned char *media=d==&nested_device?nested_medium:medium;
 assert(io_context_validate(&b->b_context)==0);
 if(expected_context && b->b_op==BIO_WRITE) {
  assert(b->b_context.origin_inode==expected_context->origin_inode);
  assert(b->b_context.claim==expected_context->claim);
  assert(b->b_context.content_generation==expected_context->content_generation);
  assert((b->b_context.flags & (IO_CONTEXT_THROUGH|IO_CONTEXT_DRAIN))==(IO_CONTEXT_THROUGH|IO_CONTEXT_DRAIN));
 }
 assert(concurrency || held_locks==0);
 assert(b->b_block*512+size<=sizeof(medium));
 if (concurrency && !__atomic_exchange_n(&paused,1,__ATOMIC_RELAXED)) host_gate_pause(1);
 if (redirty) {
  struct buf *changed=hash_find_locked(d,b->b_block);
  redirty=0;assert(changed);buf_mark_dirty(changed);
 }
 if (b->b_op==BIO_WRITE) __atomic_fetch_add(&writes,1,__ATOMIC_RELAXED);
 else __atomic_fetch_add(&reads,1,__ATOMIC_RELAXED);
 if (serial==fault_at) { b->b_transferred=size; return EIO; }
 if (serial==short_at) size/=2;
 if (nesting && d==&device) {
  assert(buf_write_context(&nested_device,b->b_block,b->b_block_count,b->b_data,&b->b_context)==0);
 }
 if (b->b_op==BIO_READ) memcpy(b->b_data,media+b->b_block*512,size);
 else memcpy(media+b->b_block*512,b->b_data,size);
 b->b_transferred=size;return 0;
}
int bio_wait(struct bio *b) { (void)b;return wait_error ? EIO : 0; }

static void clean(void)
{
 assert(buf_invalidate_disk(&device,BUF_INVALIDATE_DISCARD)==0);
 assert(buf_invalidate_disk(&nested_device,BUF_INVALIDATE_DISCARD)==0);
 calls=reads=writes=fault_at=short_at=0;
 device.d_max_transfer_blocks=0;
}
static void writer(void *arg)
{ unsigned char data[65536]; memset(data,(unsigned)(uintptr_t)arg,sizeof(data));assert(buf_write(&device,0,128,data)==0); }
int main(void)
{
 struct buf_view view = {0};
 struct io_context context;
 struct buf *b; unsigned i; uint32_t transferred; void *t1,*t2;
 memset(&device,0,sizeof(device));device.d_block_size=512;device.d_block_count=512;
 nested_device=device;
 for(i=0;i<sizeof(input);i++) input[i]=(unsigned char)(i*13+i/4096);
 memcpy(medium,input,sizeof(input));
 assert(buf_init()==0);
 check_preparation=1;
 assert(io_context_child(&context,NULL,IO_CONTEXT_ORDERED)==0);
 context.origin_inode=(struct inode *)(uintptr_t)0x1234;
 context.claim=(struct backing_claim *)(uintptr_t)0x5678;
 context.content_generation=73;
 expected_context=&context;nesting=1;check_preparation=0;
 assert(buf_write_context(&device,0,128,input,&context)==0);
 assert(buf_write_context(&device,0,1,input,&context)==0);
 expected_context=NULL;nesting=0;clean();check_preparation=1;
 context.flags=0x80;
 assert(buf_write_context(&device,0,128,input,&context)==EOPNOTSUPP);
 assert(writes==0 && device.d_dirty_buffers==NULL);

 /* Metadata views pin identities but never busy ownership across allocation. */
 assert(buf_read_view(&device,0,16,output,&view)==0);
 assert(view.count==2 && buf_view_matches(&view));
 assert(!memcmp(input,output,8192));
 assert(buf_invalidate_disk(&device,BUF_INVALIDATE_DISCARD)==EBUSY);
 assert(buf_write(&device,0,8,input)==0);
 assert(!buf_view_matches(&view));
 buf_view_release(&view);buf_view_release(&view);clean();
 assert(buf_read_view(&device,0,8,output,&view)==0);
 b=view.lines[0];b->b_generation=UINT64_MAX;
 assert(buf_write(&device,0,8,input)==0);
 assert(b->b_flags&BUF_GENERATION_EXHAUSTED);
 assert(!buf_view_matches(&view));
 buf_view_release(&view);
 assert(buf_read_view(&device,0,8,output,&view)==0);
 assert(!view.valid && view.count==0);clean();
 fault_at=1;assert(buf_read_view(&device,0,8,output,&view)==EIO);
 assert(calls==1 && view.count==0 && !view.disk);clean();
 fail_alloc=1;assert(buf_read_view(&device,0,128,output,&view)==0);
 assert(!memcmp(input,output,sizeof(input)));buf_view_release(&view);clean();
 assert(buf_read(&device,0,128,output)==0); assert(reads==1);assert(!memcmp(input,output,sizeof(input)));
 assert(buf_read(&device,0,128,output)==0);assert(reads==1);
 clean(); assert(buf_write(&device,0,128,input)==0);assert(writes==1&&reads==0);
 assert(!memcmp(medium,input,sizeof(input)));assert(cache_dirty_bytes==0);
 clean(); assert(buf_read(&device,32,8,output)==0);calls=reads=0;
 assert(buf_read(&device,0,128,output)==0);assert(reads==2);assert(!memcmp(input,output,sizeof(input)));
 clean(); memset(medium,0x91,sizeof(medium));
 assert(buf_write(&device,1,126,input)==0);assert(writes==3&&reads==2);
 for(i=0;i<512;i++) assert(medium[i]==0x91&&medium[127*512+i]==0x91);
 assert(!memcmp(medium+512,input,126*512));
 /* Confirm one prefix BIO, reject all bytes claimed by the failing BIO. */
 for(i=0;i<3;i++) {
  clean();device.d_max_transfer_blocks=i==2?12:16;if(i==1)short_at=2;else fault_at=2;
  assert(buf_write(&device,0,128,input)==EIO);
  assert(cache_dirty_bytes==(i==2?15:14)*4096);
  assert(buf_get(&device,0,&b)==0);assert(!(b->b_flags&BUF_DIRTY));buf_release(b);
  assert(buf_get(&device,16,&b)==0);assert(b->b_flags&BUF_DIRTY);buf_release(b);
  short_at=fault_at=0;assert(buf_sync(&device)==0);assert(cache_dirty_bytes==0);
  assert(!memcmp(medium,input,sizeof(input)));
 }
 clean();redirty=1;assert(buf_write(&device,0,128,input)==0);
 assert(cache_dirty_bytes==4096);assert(buf_sync(&device)==0);assert(cache_dirty_bytes==0);
 clean();wait_error=1;assert(buf_write(&device,0,128,input)==EIO);
 assert(cache_dirty_bytes==65536);wait_error=0;assert(buf_sync(&device)==0);
 clean();guard_error=EBUSY;assert(buf_write(&device,0,128,input)==EBUSY);
 assert(cache_dirty_bytes==65536);guard_error=0;assert(buf_sync(&device)==0);
 clean();device.d_max_transfer_blocks=16;fault_at=2;
 assert(buf_read(&device,0,128,output)==EIO);fault_at=0;calls=reads=0;
 assert(buf_read(&device,0,128,output)==0);assert(reads==7);
 /* Holding a later line cannot retain the earlier line on run acquisition failure. */
 clean();assert(buf_get(&device,8,&b)==0);check_preparation=0;
 assert(transfer_run(&device,0,128,input,1,&transferred,NULL)==0&&transferred==0);
 assert(hash_find_locked(&device,0)->b_busy==0);buf_release(b);check_preparation=1;
 /* Capacity failure and reentrant preparation must fall back without losing data. */
 clean();fail_alloc=3;assert(buf_write(&device,0,128,input)==0);assert(!memcmp(medium,input,sizeof(input)));
 clean();alloc_hook=1;assert(buf_write(&device,0,128,input)==0);assert(cache_current_bytes<=cache_max_bytes);
 assert(!memcmp(medium,input,sizeof(input)));assert(buf_set_max_bytes(4*1024*1024)==0);
 clean();check_preparation=0;nesting=1;assert(buf_write(&device,0,128,input)==0);nesting=0;
 assert(!memcmp(nested_medium,input,sizeof(input)));
 /* An overlapping writer waits only after all attempted run ownership is released. */
 clean();concurrency=1;paused=0;host_gate_reset(1);
 t1=host_thread_start(writer,(void *)0x32);host_gate_wait(1);
 t2=host_thread_start(writer,(void *)0x64);host_gate_release(1);
 host_thread_join(t1);host_thread_join(t2);concurrency=0;
 assert(buf_read(&device,0,128,output)==0);assert(!memcmp(output,medium,sizeof(output)));
 assert(cache_dirty_bytes==0);clean();buf_reset();assert(live_pages==1&&cache_data_bytes==0&&stat_buffers==0);
 puts("buffer runs PASS: counts, partial/hit, prefix/fault/short, pressure/reentry, concurrent writers");
 return 0;
}
