/* Immutable journal views across blocked checkpoint, retirement and reuse. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/drivers/fs/ufs/ufs-consistency.h"
#include "plan/ws018-kernel-architecture/tests/mount-thread-host.h"

#define REQUIRE(x) do { __atomic_add_fetch(&checks,1U,__ATOMIC_RELAXED); if(!(x)){fprintf(stderr,"journal-view:%d: %s\n",__LINE__,#x);abort();} } while(0)
static unsigned checks,reads,writes,flushes,pause_home,fail_home,fail_flush;
static unsigned stop_readers,reader_progress;
static uint8_t media[64*512],image[UFS_JOURNAL_IMAGE_BYTES],a[1024],b[512];
static struct ufs_journal journal;
static int checkpoint_result;

static int read_media(void *context,uint64_t first,uint32_t count,void *buffer)
{
 (void)context;REQUIRE(first<64 && count<=64-first);reads++;
 memcpy(buffer,media+first*512,count*512);return 0;
}
static int write_media(void *context,uint64_t first,uint32_t count,const void *buffer)
{
 (void)context;REQUIRE(first<64 && count<=64-first);writes++;
 if(first<40 && pause_home){pause_home=0;host_gate_pause(1);}
 if(first<40 && fail_home){fail_home=0;return EIO;}
 memcpy(media+first*512,buffer,count*512);return 0;
}
static int flush_media(void *context)
{ (void)context;flushes++;if(fail_flush){fail_flush=0;return EIO;}return 0; }
static void checkpoint(void *context)
{ checkpoint_result=ufs_journal_checkpoint(context); }
static void check_copy(struct ufs_journal_view *view)
{
 uint8_t out[1536];unsigned n;
 memset(out,0xcc,sizeof(out));
 REQUIRE(ufs_journal_view_copy(view,7,2,out)==ENOENT);
 for(n=0;n<sizeof(out);n++)REQUIRE(out[n]==0xcc);
 REQUIRE(ufs_journal_view_copy(view,8,3,out)==0);
 for(n=0;n<1024;n++)REQUIRE(out[n]==0x31);
 for(;n<sizeof(out);n++)REQUIRE(out[n]==0x72);
 REQUIRE(ufs_journal_view_copy(view,38,2,out)==EINVAL);
}
static void racing_reader(void *context)
{
 struct ufs_journal_view view={0};int error;
 (void)context;
 while(!__atomic_load_n(&stop_readers,__ATOMIC_ACQUIRE)) {
  error=ufs_journal_view_acquire(&journal,&view);
  REQUIRE(error==0 || error==ENOENT);
  if(!error){check_copy(&view);ufs_journal_view_release(&view);}
  __atomic_add_fetch(&reader_progress,1U,__ATOMIC_RELEASE);
 }
}
static void scenario(unsigned failure)
{
 struct ufs_journal_io io={NULL,read_media,write_media,flush_media};
 struct ufs_journal_extent extents[2]={{10,1,b},{8,2,a}};
 struct ufs_journal_view pinned={0},other={0};void *worker,*reader;
 unsigned saved_reads,saved_writes,saved_flushes;
 memset(media,0,sizeof(media));memset(a,0x31,sizeof(a));memset(b,0x72,sizeof(b));
 REQUIRE(ufs_journal_init(&journal,&io,40,12,39)==0);
 REQUIRE(ufs_journal_bind_image(&journal,image,sizeof(image))==0);
 REQUIRE(ufs_journal_view_acquire(&journal,&pinned)==ENOENT);
 REQUIRE(ufs_journal_publishv(&journal,extents,2)==0);
 REQUIRE(ufs_journal_view_acquire(&journal,&pinned)==0 && pinned.sequence==1);
 REQUIRE(ufs_journal_view_acquire(&journal,&pinned)==EBUSY);
 REQUIRE(ufs_journal_views_busy(&journal));check_copy(&pinned);
 memset(a,0x99,sizeof(a));memset(b,0x99,sizeof(b));check_copy(&pinned);
 host_gate_reset(1);pause_home=1;fail_home=failure==1;fail_flush=failure==2;
 worker=host_thread_start(checkpoint,&journal);host_gate_wait(1);
 saved_reads=reads;saved_writes=writes;saved_flushes=flushes;
 REQUIRE(ufs_journal_view_acquire(&journal,&other)==0);check_copy(&other);
 REQUIRE(reads==saved_reads && writes==saved_writes && flushes==saved_flushes);
 ufs_journal_view_release(&other);
 __atomic_store_n(&stop_readers,0,__ATOMIC_RELEASE);__atomic_store_n(&reader_progress,0,__ATOMIC_RELEASE);
 reader=host_thread_start(racing_reader,NULL);
 while(__atomic_load_n(&reader_progress,__ATOMIC_ACQUIRE)<4)host_thread_yield();
 host_gate_release(1);host_thread_join(worker);
 REQUIRE(checkpoint_result==(failure?EIO:0));check_copy(&pinned);
 if(failure)REQUIRE(ufs_journal_replay(&journal)==0);
 REQUIRE(journal.pending_sequence==0 && !journal.image_valid);
 __atomic_store_n(&stop_readers,1,__ATOMIC_RELEASE);host_thread_join(reader);
 REQUIRE(ufs_journal_view_acquire(&journal,&other)==ENOENT);
 saved_reads=reads;saved_writes=writes;saved_flushes=flushes;
 REQUIRE(ufs_journal_bind_image(&journal,NULL,0)==EBUSY);
 REQUIRE(ufs_journal_publishv(&journal,extents,2)==EBUSY);
 REQUIRE(ufs_journal_replay(&journal)==EBUSY);
 REQUIRE(reads==saved_reads && writes==saved_writes && flushes==saved_flushes);
 check_copy(&pinned);ufs_journal_view_release(&pinned);ufs_journal_view_release(&pinned);
 REQUIRE(!ufs_journal_views_busy(&journal));
 REQUIRE(ufs_journal_publishv(&journal,extents,2)==0);
 REQUIRE(ufs_journal_view_acquire(&journal,&other)==0 && other.sequence==2);
 REQUIRE(ufs_journal_view_copy(&other,8,2,a)==0 && a[0]==0x99);
 ufs_journal_view_release(&other);REQUIRE(ufs_journal_checkpoint(&journal)==0);
 REQUIRE(ufs_journal_bind_image(&journal,NULL,0)==0);
}
int main(void)
{
 unsigned failure,repeat;
 for(repeat=0;repeat<12;repeat++)for(failure=0;failure<3;failure++)scenario(failure);
 printf("UFS immutable view/concurrent checkpoint/reuse: PASS (%u assertions)\n",checks);return 0;
}
