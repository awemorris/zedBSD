/* Real metadata/CG read paths, writer backpressure and mount-image retirement. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#define UFS_AUDIT_CUSTOM_WRITEBACK
#include "allocation-journal-host.c"
#include "src/kern/io-error.c"
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }

static int policy_active;
int writeback_mount_active(struct mount *mountp)
{ REQUIRE(mutex_owned(&state(mountp)->lock));return policy_active; }
static unsigned pause_checkpoint,freed;
static int writer_result,reader_result;
static uint8_t cg_payload[4096],inode_payload[4096],output[4096];
static void blocked_home(uint64_t first,uint32_t count,const void *data)
{
 (void)count;(void)data;
 if(first==32 && pause_checkpoint){pause_checkpoint=0;host_gate_pause(1);}
}
static void checkpoint_worker(void *argument)
{
 struct ufs_mount_state *fs=argument;
 mutex_lock(&fs->journal_lock);writer_result=ufs_journal_checkpoint(&fs->journal);mutex_unlock(&fs->journal_lock);
}
static void missing_reader(void *argument)
{ reader_result=read_block(argument,176,output); }
static void publish_worker(void *argument)
{ writer_result=write_block(argument,8,inode_payload); }
static void retire_worker(void *argument)
{ host_gate_signal(3);journal_image_free(argument); }
int hal_pmem_free(struct hal_pmem *memory)
{
 REQUIRE(memory->vaddr==redo && memory->size==sizeof(redo));
 memset(redo,0xed,sizeof(redo));__atomic_store_n(&freed,1,__ATOMIC_RELEASE);return HAL_OK;
}
void cache_memory_release(enum cache_memory_kind kind,size_t size)
{ REQUIRE(kind==CACHE_MEMORY_BUF_META && size==sizeof(redo)); }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }

int main(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;
 struct ufs_journal_io io;struct ufs_journal_extent extents[2];
 struct ufs_journal_view retained={0};void *worker,*reader;unsigned n,before;
 storage_fixture(&fs,&node,&mountp,&disk,0,1);disk.d_block_size=512;disk.d_block_count=512;
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 memcpy(cg_payload,storage+32*512,4096);cg_payload[400]=0xa5;memset(inode_payload,0x39,sizeof(inode_payload));
 extents[0]=(struct ufs_journal_extent){32,8,cg_payload};extents[1]=(struct ufs_journal_extent){8,8,inode_payload};
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 host_gate_reset(1);host_gate_reset(2);pause_checkpoint=1;group_write_check=blocked_home;
 worker=host_thread_start(checkpoint_worker,&fs);host_gate_wait(1);
 before=storage_reads;
 REQUIRE(read_block(&mountp,8,output)==0 && memcmp(output,inode_payload,4096)==0);
 REQUIRE(load_cg_locked(&mountp,0)==0 && fs.cg[400]==0xa5);
 REQUIRE(storage[32*512+400]!=0xa5 && storage_reads==before);
 reader=host_thread_start(missing_reader,&mountp);host_gate_wait(2);
 REQUIRE(!ufs_journal_views_busy(&fs.journal));
 host_gate_release(1);host_thread_join(worker);host_thread_join(reader);
 REQUIRE(writer_result==0 && reader_result==0);group_write_check=NULL;

 /* Retired readers must drain before the VFS writer can publish its next image. */
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 REQUIRE(ufs_journal_view_acquire(&fs.journal,&retained)==0);
 REQUIRE(ufs_journal_checkpoint(&fs.journal)==0);
 before=storage_writes;worker=host_thread_start(publish_worker,&mountp);
 while(!__atomic_load_n(&fs.journal_lock.locked,__ATOMIC_ACQUIRE))host_thread_yield();
 REQUIRE(storage_writes==before && fs.journal.pending_sequence==0);
 REQUIRE(ufs_journal_view_copy(&retained,8,8,output)==0 && memcmp(output,inode_payload,4096)==0);
 ufs_journal_view_release(&retained);host_thread_join(worker);REQUIRE(writer_result==0);

 /* A synchronous writer drains a prior committed slot before claiming its own. */
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 REQUIRE(write_block(&mountp,8,inode_payload)==0);
 REQUIRE(fs.journal.pending_sequence==0 && !fs.journal.poisoned && fs.writable);
 REQUIRE(memcmp(durable+32*512,cg_payload,4096)==0);

 /* Mount sync installs retained homes and releases the borrowed drain context. */
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 REQUIRE(ufs_sync(&mountp)==0 && fs.journal.pending_sequence==0);
 REQUIRE(fs.journal_io.context==NULL && fs.writable);

 /* Successful recovery cannot hide the first failure from the sync observer. */
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 failure_sync=storage_syncs+1;
 REQUIRE(ufs_sync(&mountp)==EIO);
 REQUIRE(mountp.m_metadata_error.sequence==1 && mountp.m_metadata_error.error==EIO);
 REQUIRE(fs.journal.pending_sequence==0 && !fs.journal.poisoned && fs.writable);
 REQUIRE(fs.journal_io.context==NULL);
 failure_sync=0;REQUIRE(ufs_sync(&mountp)==0);
 REQUIRE(mountp.m_metadata_error.sequence==1 && mountp.m_write_error.sequence==1);

 /* Failed prefix drain must not publish the new caller's prepared image. */
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 {
  struct ufs_transaction_outcome outcome;uint64_t next=fs.journal.next_sequence;
  failure_sync=storage_syncs+1;mutex_lock(&fs.lock);
  REQUIRE(metadata_group_commit(&mountp,extents,2,NULL,&outcome)==EIO);
  mutex_unlock(&fs.lock);
  REQUIRE(!outcome.committed && !outcome.uncertain && fs.journal.next_sequence==next);
  REQUIRE(fs.journal.pending_sequence==0 && fs.journal_io.context==NULL);
  failure_sync=0;
 }

 /* Opt-in publication retains one durable slot; through calls still drain it. */
 {
  struct ufs_transaction_outcome outcome;struct io_context through;
  memset(inode_payload,0x73,sizeof(inode_payload));policy_active=1;
  mutex_lock(&fs.lock);
  REQUIRE(metadata_group_commit(&mountp,extents,2,NULL,&outcome)==0);
  mutex_unlock(&fs.lock);
  REQUIRE(outcome.committed && !outcome.uncertain && fs.writable);
  REQUIRE(fs.journal.pending_ready && fs.journal.pending_sequence!=0);
  REQUIRE(memcmp(durable+8*512,inode_payload,4096)!=0);
  REQUIRE(read_block(&mountp,8,output)==0 && memcmp(output,inode_payload,4096)==0);
  REQUIRE(fs.journal_io.context==NULL);
  REQUIRE(io_context_child(&through,NULL,IO_CONTEXT_THROUGH)==0);
  mutex_lock(&fs.lock);
  REQUIRE(metadata_group_commit(&mountp,extents,2,&through,&outcome)==0);
  mutex_unlock(&fs.lock);
  REQUIRE(outcome.committed && !outcome.uncertain && fs.journal.pending_sequence==0);
  REQUIRE(memcmp(durable+8*512,inode_payload,4096)==0);
  mutex_lock(&fs.lock);
  REQUIRE(metadata_group_commit(&mountp,extents,2,NULL,&outcome)==0);
  mutex_unlock(&fs.lock);
  policy_active=0;REQUIRE(ufs_sync(&mountp)==0 && fs.journal.pending_sequence==0);
 }

 /* Sector-sized indirect lookup must see redo, not an obsolete home pointer. */
 {
  uint64_t pointer;
  ufs_put64(inode_payload,0,176,0);
  REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
  REQUIRE(indirect_entry(&mountp,8,0,&pointer)==0 && pointer==176);
  REQUIRE(ufs_sync(&mountp)==0);
 }

 /* A second failure poisons admission; only remount recovery reopens it. */
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 failure_write=storage_writes+1;failure_write_again=storage_writes+2;
 REQUIRE(ufs_sync(&mountp)==EIO && fs.journal.poisoned && !fs.writable);
 REQUIRE(fs.journal_io.context==NULL);
 REQUIRE(read_block(&mountp,8,output)==EIO);
 failure_write=failure_write_again=0;
 REQUIRE(ufs_sync(&mountp)==EIO);
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 REQUIRE(ufs_journal_replay(&fs.journal)==0);fs.writable=1;

 /* Teardown closes acquisition, keeps existing bytes pinned, then releases backing. */
 REQUIRE(ufs_journal_publishv(&fs.journal,extents,2)==0);
 REQUIRE(ufs_journal_view_acquire(&fs.journal,&retained)==0);
 fs.journal_memory.vaddr=redo;fs.journal_memory.size=sizeof(redo);
 host_gate_reset(3);worker=host_thread_start(retire_worker,&fs);host_gate_wait(3);
 for(n=0;n<1000;n++)host_thread_yield();
 REQUIRE(!__atomic_load_n(&freed,__ATOMIC_ACQUIRE));
 REQUIRE(ufs_journal_view_copy(&retained,8,8,output)==0 && memcmp(output,inode_payload,4096)==0);
 ufs_journal_view_release(&retained);host_thread_join(worker);
 REQUIRE(__atomic_load_n(&freed,__ATOMIC_ACQUIRE) && !fs.journal.image && !fs.journal_memory.size);
 free(fs.cg);
 printf("UFS metadata/CG pinned reads, writer drain and teardown: PASS (%u checks)\n",functional_checks);return 0;
}
