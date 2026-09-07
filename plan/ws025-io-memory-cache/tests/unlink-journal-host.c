/* Real UFS unlink, journal and name cache across failed publication and replay. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#define UFS_AUDIT_CUSTOM_NAMESPACE
#include "allocation-journal-host.c"
static struct ufs_journal *namespace_journal;
static unsigned namespace_snapshot,dir_changes;
static int unlink_result;
static struct componentname victim={"victim",6,0};
static struct inode *unlink_directory;
unsigned long spin_lock_irqsave(struct spinlock *lock)
{ REQUIRE(__atomic_exchange_n(&lock->held.value,1,__ATOMIC_ACQUIRE)==0);return 1; }
void spin_unlock_irqrestore(struct spinlock *lock,unsigned long enabled)
{ REQUIRE(enabled==1);REQUIRE(__atomic_exchange_n(&lock->held.value,0,__ATOMIC_RELEASE)==1); }
void inode_ref(struct inode *inode) { (void)inode; }
void inode_dir_changed(struct inode *inode)
{ dir_changes++;atomic_u64_store_release(&inode->i_dirseq,atomic_u64_load_acquire(&inode->i_dirseq)+1); }
static void namespace_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==176 || first==8) {
  REQUIRE(namespace_journal->pending_ready);
  REQUIRE(namespace_journal->committed_sequence==namespace_journal->pending_sequence);
  if(namespace_snapshot)REQUIRE((snapshot_mask&12)==12);
 }
}
static void perform_unlink(void *argument)
{ unlink_result=ufs_unlink(argument,&victim); }
static void record(unsigned offset,unsigned length,unsigned number,const char *name)
{
 uint8_t *p=storage+176*512+offset;
 ufs_put32(p,0,number,0);ufs_put16(p,4,length,0);p[6]=8;p[7]=strlen(name);
 memcpy(p+8,name,strlen(name));
}
static unsigned names(unsigned number)
{
 unsigned pos=0,found=0;uint8_t *p=storage+176*512;
 while(pos<512) {
  unsigned length=ufs_get16(p,pos+4,0);
  REQUIRE(length>=8 && !(length&3) && length<=512-pos);
  if(ufs_get32(p,pos,0)==number)found++;
  pos+=length;
 }
 return found;
}
static void namespace_scenario(unsigned links,unsigned previous,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node,directory;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;
 struct inode *cached=NULL;unsigned n,remaining;uint8_t before_directory[UFS_DINODE_SIZE];
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;node.inode.i_linkcount=links;
 REQUIRE(persist_inode(&node.inode)==0);
 memset(&directory,0,sizeof(directory));directory.inode.i_mount=&mountp;
 directory.inode.i_ino=3;directory.inode.i_type=INODE_DIR;directory.inode.i_mode=S_IFDIR|0755;
 directory.inode.i_linkcount=2;directory.inode.i_size=512;directory.direct[0]=176;directory.blocks=8;
 atomic_u64_store_release(&directory.inode.i_dirseq,1);
 REQUIRE(persist_inode(&directory.inode)==0);
 memset(storage+176*512,0,4096);
 if(previous)record(0,12,3,".");
 record(previous?12:0,links==2?16:(previous?500:512),2,"victim");
 if(links==2)record(previous?28:16,previous?484:496,2,"alias");
 memcpy(before_directory,storage+8*512+3*UFS_DINODE_SIZE,UFS_DINODE_SIZE);
 REQUIRE(disk_sync(&disk)==0);
 cached_node=node;allocations=1;
 REQUIRE(namecache_enter(&directory.inode,&victim,&cached_node.inode,1)==0);
 REQUIRE(namecache_lookup(&directory.inode,&victim,&cached)==0 && cached==&cached_node.inode);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=namespace_snapshot;
 namespace_journal=&fs.journal;group_write_check=namespace_write_check;unlink_directory=&directory.inode;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=dir_changes=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;unlink_result=EIO;
 (void)host_crash_run(perform_unlink,unlink_directory);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(unlink_result==0 || unlink_result==EIO || (media_override_error && unlink_result==media_override_error));
 if(!crashed) {
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  if(fs.journal.committed_sequence)REQUIRE(cached_node.inode.i_linkcount==links-1);
  else REQUIRE(cached_node.inode.i_linkcount==links);
  if(unlink_result && (fs.journal.committed_sequence || fs.journal.poisoned)) {
   REQUIRE(namecache_lookup(&directory.inode,&victim,&cached)==ENOENT);
   REQUIRE(dir_changes==1);
  }
  if(fs.journal.poisoned) {
   off_t cursor=0;uint32_t number;uint8_t type;char name[NAME_MAX+1];
   REQUIRE(next_dirent(&directory.inode,&cursor,&number,&type,name)==EIO);
  }
 }
 namecache_purge_mount(&mountp);
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 namespace_journal=&recovered;REQUIRE(ufs_journal_replay(&recovered)==0);
 remaining=ufs_get16(storage+8*512+2*UFS_DINODE_SIZE,UFS_DI_NLINK,0);
 REQUIRE(remaining==links || remaining==links-1);
 REQUIRE(names(2)==remaining);
 REQUIRE(memcmp(before_directory,storage+8*512+3*UFS_DINODE_SIZE,UFS_DINODE_SIZE)==0);
 if(unlink_result==0)REQUIRE(remaining==links-1);
 group_write_check=NULL;free(fs.cg);
}
#ifdef UFS_UNLINK_JOURNAL_LIBRARY
int retained_unlink_main(void)
#else
int main(void)
#endif
{
 unsigned links,previous,n,mode;
 for(links=1;links<=2;links++)for(previous=0;previous<2;previous++) {
  namespace_scenario(links,previous,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=18;n++)namespace_scenario(links,previous,n,0,mode,0);
  for(n=1;n<=12;n++)namespace_scenario(links,previous,0,n,0,0);
  for(n=1;n<=18;n++){second_failure=n+1;namespace_scenario(links,previous,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=24;n++)namespace_scenario(links,previous,0,0,0,n);}
  crash_mode=0;namespace_snapshot=1;snapshot_fail=0;namespace_scenario(links,previous,0,0,0,0);
  for(n=1;n<=2;n++){snapshot_fail=n;namespace_scenario(links,previous,0,0,0,0);}namespace_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;namespace_scenario(links,previous,0,1,0,0);media_override_error=0;
 }
 printf("UFS grouped unlink/cache/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
