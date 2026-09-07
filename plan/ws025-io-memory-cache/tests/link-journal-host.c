/* Shared/distinct dinode hard-link publication with real VFS, redo and name cache. */
#define UFS_UNLINK_JOURNAL_LIBRARY
#include "unlink-journal-host.c"
static struct componentname added={"new",3,0},old_alias={"old",3,0};
static struct inode *link_target;
static unsigned link_shared,link_snapshot;
static int link_result;
static void link_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 unsigned expected=link_shared?12:268;
 (void)count;(void)buffer;
 if(first==176 || first==8 || first==16) {
  REQUIRE(namespace_journal->pending_ready && namespace_journal->image_valid);
  REQUIRE(ufs_get32(namespace_journal->image,16,0)==(link_shared?2U:3U));
  if(link_snapshot)REQUIRE((snapshot_mask&expected)==expected);
 }
}
static void perform_link(void *argument)
{
 link_result=ufs_link(argument,&added,link_target);
 /* The generic wrapper increments only after a successful backend callback. */
 if(link_result==0){REQUIRE(link_target->i_linkcount==1);link_target->i_linkcount++;inode_dir_changed(argument);}
}
static unsigned added_names(unsigned size)
{
 unsigned pos=0,found=0;uint8_t *p=storage+176*512;
 while(pos<size) {
  unsigned length=ufs_get16(p,pos+4,0);
  REQUIRE(length>=8 && !(length&3) && length<=size-pos && pos%512+length<=512);
  if(ufs_get32(p,pos,0)==2 && p[pos+7]==3 && !memcmp(p+pos+8,"new",3))found++;
  pos+=length;
 }
 return found;
}
static void link_scenario(unsigned shared,unsigned layout,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node,directory;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct inode *cached;struct componentname cached_name=old_alias;
 uint8_t *raw_target,*raw_directory;uint64_t size;unsigned n,found,links;char longname[249];
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 memset(&directory,0,sizeof(directory));directory.inode.i_mount=&mountp;
 directory.inode.i_ino=shared?3:18;directory.inode.i_type=INODE_DIR;directory.inode.i_mode=S_IFDIR|0755;
 directory.inode.i_linkcount=2;directory.inode.i_size=layout==3?0:512;
 directory.direct[0]=176;directory.blocks=8;atomic_u64_store_release(&directory.inode.i_dirseq,1);
 REQUIRE(persist_inode(&directory.inode)==0);
 memset(storage+176*512,0,4096);
 if(layout==0)record(0,512,2,"old");
 if(layout==1){record(0,12,2,"old");record(12,500,0,"");}
 if(layout==2){memset(longname,'x',248);longname[248]=0;record(0,256,2,longname);longname[0]='y';record(256,256,4,longname);}
 REQUIRE(disk_sync(&disk)==0);
 if(layout==2){longname[0]='x';cached_name.cn_nameptr=longname;cached_name.cn_namelen=248;}
 if(layout!=3) {
  REQUIRE(namecache_enter(&directory.inode,&cached_name,&node.inode,1)==0);
  REQUIRE(namecache_lookup(&directory.inode,&cached_name,&cached)==0);
 }
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=link_snapshot;
 namespace_journal=&fs.journal;link_shared=shared;link_target=&node.inode;group_write_check=link_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=dir_changes=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;link_result=EIO;
 (void)host_crash_run(perform_link,&directory.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(link_result==0 || link_result==EIO || (media_override_error && link_result==media_override_error));
 if(!crashed) {
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(node.inode.i_linkcount==(fs.journal.committed_sequence?2:1));
  if(fs.journal.committed_sequence || fs.journal.poisoned) {
   REQUIRE(dir_changes==1);
   if(layout!=3)REQUIRE(namecache_lookup(&directory.inode,&cached_name,&cached)==ENOENT);
  }
 }
 namecache_purge_mount(&mountp);
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 namespace_journal=&recovered;REQUIRE(ufs_journal_replay(&recovered)==0);
 raw_target=storage+8*512+2*UFS_DINODE_SIZE;
 raw_directory=storage+(shared?8:16)*512+(shared?3:2)*UFS_DINODE_SIZE;
 links=ufs_get16(raw_target,UFS_DI_NLINK,0);size=ufs_get64(raw_directory,UFS_DI_SIZE,0);
 REQUIRE(links==1 || links==2);
 REQUIRE(size==(layout==3?(links==2?512U:0U):(layout==2 && links==2?1024U:512U)));
 found=added_names((unsigned)size);REQUIRE(found==links-1);
 REQUIRE(ufs_get16(raw_directory,UFS_DI_NLINK,0)==2);
 REQUIRE(ufs_get64(raw_directory,UFS_DI_DB,0)==176);
 REQUIRE(ufs_get64(raw_target,UFS_DI_DB,0)==160);
 REQUIRE(ufs_get64(raw_target,UFS_DI_SIZE,0)==13*4096);
 if(link_result==0)REQUIRE(links==2);
 group_write_check=NULL;free(fs.cg);
}
static void link_refusals(void)
{
 AUDIT_STATE fs;AUDIT_INODE node,directory;struct mount mountp;struct disk disk;
 struct ufs_journal_io io;struct componentname invalid={"a/b",3,0};
 unsigned n;int handled;char name[249];
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 memset(&directory,0,sizeof(directory));directory.inode.i_mount=&mountp;
 directory.inode.i_ino=3;directory.inode.i_type=INODE_DIR;directory.inode.i_mode=S_IFDIR|0755;
 directory.inode.i_size=512;directory.direct[0]=176;directory.blocks=8;
 REQUIRE(persist_inode(&directory.inode)==0);record(0,512,2,"old");REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,18,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 storage_writes=storage_syncs=0;
 mutex_lock(&fs.namespace_lock);
 node.inode.i_linkcount=UINT16_MAX;
 REQUIRE(link_group(&directory.inode,&added,&node.inode,&handled)==EMLINK && handled);
 node.inode.i_linkcount=1;
 REQUIRE(link_group(&directory.inode,&invalid,&node.inode,&handled)==EINVAL && handled);
 REQUIRE(link_group(&directory.inode,&old_alias,&node.inode,&handled)==EEXIST && handled);
 ufs_put16(storage+176*512,4,4,0);
 REQUIRE(link_group(&directory.inode,&added,&node.inode,&handled)==EIO && handled);
 directory.inode.i_size=4096;memset(name,'x',248);name[248]=0;
 for(n=0;n<16;n++){name[0]='a'+n;record(n*256,256,4+n,name);}
 REQUIRE(link_group(&directory.inode,&added,&node.inode,&handled)==ENOSPC && handled);
 directory.inode.i_size=512;record(0,512,2,"old");
 allocation_failure_size=8192;
 REQUIRE(link_group(&directory.inode,&added,&node.inode,&handled)==ENOMEM && handled);
 directory.inode.i_ino=18;
 REQUIRE(link_group(&directory.inode,&added,&node.inode,&handled)==0 && !handled);
 directory.inode.i_ino=3;
 REQUIRE(storage_writes==0 && storage_syncs==0 && node.inode.i_linkcount==1 && fs.writable);
 /* Exactly two shared-block extents fit a slot that cannot hold three. */
 REQUIRE(link_group(&directory.inode,&added,&node.inode,&handled)==0 && handled);
 REQUIRE(ufs_sync(&mountp)==0);
 REQUIRE(node.inode.i_linkcount==1 && ufs_get16(storage+8*512+2*UFS_DINODE_SIZE,UFS_DI_NLINK,0)==2);
 REQUIRE(added_names(512)==1);
 mutex_unlock(&fs.namespace_lock);free(fs.cg);
}
int main(void)
{
 unsigned shared,layout,n,mode;
 for(shared=0;shared<2;shared++)for(layout=0;layout<4;layout++) {
  link_scenario(shared,layout,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=20;n++)link_scenario(shared,layout,n,0,mode,0);
  for(n=1;n<=12;n++)link_scenario(shared,layout,0,n,0,0);
  for(n=1;n<=20;n++){second_failure=n+1;link_scenario(shared,layout,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=28;n++)link_scenario(shared,layout,0,0,0,n);}
  crash_mode=0;link_snapshot=1;snapshot_fail=0;link_scenario(shared,layout,0,0,0,0);
  for(n=1;n<=(shared?2U:3U);n++){snapshot_fail=n;link_scenario(shared,layout,0,0,0,0);}link_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;link_scenario(shared,layout,0,1,0,0);media_override_error=0;
 }
 link_refusals();
 printf("UFS grouped hard-link/shared-dinode recovery: PASS (%u checks)\n",functional_checks);return 0;
}
