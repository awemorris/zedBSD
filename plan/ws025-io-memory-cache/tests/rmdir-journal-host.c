/* Parent entry/count and empty directory retirement marker in one redo group. */
#define UFS_UNLINK_JOURNAL_LIBRARY
#include "unlink-journal-host.c"
static struct componentname dot={".",1,0},dotdot={"..",2,0};
static unsigned rmdir_shared,rmdir_snapshot,refusal;
static int rmdir_result;
static void rmdir_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 unsigned expected=rmdir_shared?12:268;
 (void)count;(void)buffer;
 if(first==176 || first==8 || first==16) {
  REQUIRE(namespace_journal->pending_ready && namespace_journal->image_valid);
  REQUIRE(drv_ufs_get32(namespace_journal->image,16,0)==(rmdir_shared?2U:3U));
  if(rmdir_snapshot)REQUIRE((snapshot_mask&expected)==expected);
 }
}
static void perform_rmdir(void *argument)
{ rmdir_result=ufs_rmdir(argument,refusal==3?&dot:(refusal==4?&dotdot:&victim)); }
static void child_record(unsigned offset,unsigned length,unsigned number,const char *name)
{
 uint8_t *p=storage+160*512+offset;
 drv_ufs_put32(p,0,number,0);drv_ufs_put16(p,4,length,0);p[6]=4;p[7]=strlen(name);memcpy(p+8,name,strlen(name));
}
static void rmdir_scenario(unsigned shared,unsigned previous,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node,parent;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct inode *cached;
 unsigned n,remaining,child_links,parent_links;uint8_t *child_raw,*parent_raw;uint8_t child_before[4096];
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_type=refusal==2?INODE_REG:INODE_DIR;node.inode.i_mode=(refusal==2?S_IFREG:S_IFDIR)|0755;
 node.inode.i_linkcount=2;node.inode.i_size=512;
 REQUIRE(persist_inode(&node.inode)==0);
 memset(&parent,0,sizeof(parent));parent.inode.i_mount=&mountp;parent.inode.i_ino=shared?3:18;
 parent.inode.i_type=INODE_DIR;parent.inode.i_mode=S_IFDIR|0755;
 parent.inode.i_linkcount=3;parent.inode.i_size=512;parent.direct[0]=176;parent.blocks=8;
 atomic_u64_store_release(&parent.inode.i_dirseq,1);
 REQUIRE(persist_inode(&parent.inode)==0);
 memset(storage+176*512,0,4096);memset(storage+160*512,0,4096);
 if(previous){record(0,12,parent.inode.i_ino,".");record(12,12,parent.inode.i_ino,"..");record(24,488,2,"victim");}
 else {record(0,16,2,"victim");record(16,12,parent.inode.i_ino,".");record(28,484,parent.inode.i_ino,"..");}
 child_record(0,12,2,".");child_record(12,refusal==1?12:500,parent.inode.i_ino,"..");
 if(refusal==1)child_record(24,488,4,"busy");
 memcpy(child_before,storage+160*512,sizeof(child_before));REQUIRE(disk_sync(&disk)==0);
 cached_node=node;allocations=1;
 REQUIRE(namecache_enter(&parent.inode,&victim,&cached_node.inode,1)==0);
 REQUIRE(namecache_enter(&parent.inode,&dot,&parent.inode,1)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=rmdir_snapshot;
 namespace_journal=&fs.journal;rmdir_shared=shared;group_write_check=rmdir_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=dir_changes=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;rmdir_result=EIO;
 (void)host_crash_run(perform_rmdir,&parent.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 if(refusal) {
  REQUIRE(rmdir_result==(refusal==1?ENOTEMPTY:(refusal==2?ENOTDIR:EINVAL)));
  REQUIRE(storage_writes==0 && storage_syncs==0);
 } else REQUIRE(rmdir_result==0 || rmdir_result==EIO || (media_override_error && rmdir_result==media_override_error));
 if(!crashed) {
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(parent.inode.i_linkcount==(fs.journal.committed_sequence?2:3));
  REQUIRE(cached_node.inode.i_linkcount==(fs.journal.committed_sequence?0:2));
  if(rmdir_result && (fs.journal.committed_sequence || fs.journal.poisoned)) {
   REQUIRE(dir_changes==1);
   REQUIRE(namecache_lookup(&parent.inode,&dot,&cached)==ENOENT);
   REQUIRE(namecache_lookup(&parent.inode,&victim,&cached)==ENOENT);
  }
 }
 namecache_purge_mount(&mountp);
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(drv_ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 namespace_journal=&recovered;REQUIRE(drv_ufs_journal_replay(&recovered)==0);
 child_raw=storage+8*512+2*UFS_DINODE_SIZE;
 parent_raw=storage+(shared?8:16)*512+(shared?3:2)*UFS_DINODE_SIZE;
 remaining=names(2);child_links=drv_ufs_get16(child_raw,UFS_DI_NLINK,0);parent_links=drv_ufs_get16(parent_raw,UFS_DI_NLINK,0);
 REQUIRE(remaining<=1 && child_links==remaining*2 && parent_links==2+remaining);
 REQUIRE(drv_ufs_get64(child_raw,UFS_DI_DB,0)==160 && drv_ufs_get64(child_raw,UFS_DI_BLOCKS,0)==8);
 REQUIRE(drv_ufs_get64(parent_raw,UFS_DI_DB,0)==176 && drv_ufs_get64(parent_raw,UFS_DI_SIZE,0)==512);
 REQUIRE(memcmp(child_before,storage+160*512,sizeof(child_before))==0);
 if(rmdir_result==0)REQUIRE(remaining==0);
 if(refusal)REQUIRE(remaining==1);
 group_write_check=NULL;free(fs.cg);
}
int main(void)
{
 unsigned shared,previous,n,mode;
 for(shared=0;shared<2;shared++)for(previous=0;previous<2;previous++) {
  rmdir_scenario(shared,previous,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=20;n++)rmdir_scenario(shared,previous,n,0,mode,0);
  for(n=1;n<=12;n++)rmdir_scenario(shared,previous,0,n,0,0);
  for(n=1;n<=20;n++){second_failure=n+1;rmdir_scenario(shared,previous,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=28;n++)rmdir_scenario(shared,previous,0,0,0,n);}
  crash_mode=0;rmdir_snapshot=1;snapshot_fail=0;rmdir_scenario(shared,previous,0,0,0,0);
  for(n=1;n<=(shared?2U:3U);n++){snapshot_fail=n;rmdir_scenario(shared,previous,0,0,0,0);}rmdir_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;rmdir_scenario(shared,previous,0,1,0,0);media_override_error=0;
  for(refusal=1;refusal<=4;refusal++)rmdir_scenario(shared,previous,0,0,0,0);
  refusal=0;
 }
 printf("UFS grouped rmdir/shared-parent recovery: PASS (%u checks)\n",functional_checks);return 0;
}
