/* Real rename preparation and journal recovery across namespace topologies. */
#define UFS_UNLINK_JOURNAL_LIBRARY
#include "unlink-journal-host.c"
static struct componentname renamed={"renamed",7,0},parent_name={"..",2,0};
static struct inode *rename_old,*rename_new,*rename_source,*rename_target;
static unsigned rename_snapshot,rename_extents,rename_mask;
static int rename_result,rename_handled;
static void rename_record(uint64_t fragment,unsigned offset,unsigned length,unsigned number,const char *name)
{
 uint8_t *p=storage+fragment*512+offset;
 ufs_put32(p,0,number,0);ufs_put16(p,4,length,0);p[6]=4;p[7]=strlen(name);memcpy(p+8,name,strlen(name));
}
static unsigned rename_number(uint64_t fragment,const char *name)
{
 unsigned pos=0,length;uint8_t *p=storage+fragment*512;
 while(pos<512) {
  length=ufs_get16(p,pos+4,0);REQUIRE(length>=8 && !(length&3) && length<=512-pos);
  if(ufs_get32(p,pos,0) && p[pos+7]==strlen(name) && !memcmp(p+pos+8,name,strlen(name)))return ufs_get32(p,pos,0);
  pos+=length;
 }
 return 0;
}
static void rename_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==176 || first==184 || first==160 || first==8 || first==16) {
  REQUIRE(namespace_journal->pending_ready && namespace_journal->image_valid);
  REQUIRE(ufs_get32(namespace_journal->image,16,0)==rename_extents);
  if(rename_snapshot){REQUIRE((snapshot_mask&rename_mask)==rename_mask);REQUIRE(snapshot_calls==rename_extents);}
 }
}
static void perform_rename(void *argument)
{
 struct ufs_mount_state *fs=argument;
 mutex_lock(&fs->namespace_lock);
 rename_result=rename_group(rename_old,&victim,rename_new,&renamed,rename_source,rename_target,&rename_handled);
 mutex_unlock(&fs->namespace_lock);
 if(rename_result==0) {
  namecache_remove(rename_old,&victim);namecache_remove(rename_new,&renamed);
  inode_dir_changed(rename_old);
  if(rename_new!=rename_old)inode_dir_changed(rename_new);
  if(rename_source->i_type==INODE_DIR && rename_new!=rename_old)inode_dir_changed(rename_source);
 }
}
static uint8_t *rename_raw(struct inode *node)
{ return storage+inode_fragment(node)*512+((unsigned)node->i_ino%16)*UFS_DINODE_SIZE; }
static void rename_scenario(unsigned cross,unsigned directory,unsigned replacing,unsigned shared,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node,old,new,target;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct inode *cached;
 unsigned n,old_number,new_number,committed,old_links,new_links,target_links;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_type=directory?INODE_DIR:INODE_REG;node.inode.i_mode=(directory?S_IFDIR:S_IFREG)|0755;
 node.inode.i_linkcount=directory?2:1;node.inode.i_size=512;
 old=node;old.inode.i_ino=4;old.inode.i_type=INODE_DIR;old.inode.i_mode=S_IFDIR|0755;
 old.inode.i_linkcount=2+directory+(directory && replacing && !cross);old.direct[0]=176;
 new=old;new.inode.i_ino=shared?5:18;new.inode.i_linkcount=2+(directory && replacing);new.direct[0]=184;
 target=node;target.inode.i_ino=shared?3:19;target.direct[0]=168;
 rename_old=&old.inode;rename_new=cross?&new.inode:&old.inode;rename_source=&node.inode;rename_target=replacing?&target.inode:NULL;
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(persist_inode(&old.inode)==0);
 if(cross)REQUIRE(persist_inode(&new.inode)==0);
 if(replacing)REQUIRE(persist_inode(&target.inode)==0);
 memset(storage+160*512,0,32*512);
 rename_record(176,0,replacing && !cross?16:512,2,"victim");
 if(replacing && !cross)rename_record(176,16,496,target.inode.i_ino,"renamed");
 rename_record(184,0,512,replacing?target.inode.i_ino:0,replacing?"renamed":"");
 rename_record(160,0,12,2,".");rename_record(160,12,500,old.inode.i_ino,"..");
 REQUIRE(disk_sync(&disk)==0);
 atomic_u64_store_release(&old.inode.i_dirseq,1);atomic_u64_store_release(&new.inode.i_dirseq,1);atomic_u64_store_release(&node.inode.i_dirseq,1);
 REQUIRE(namecache_enter(rename_old,&victim,&node.inode,1)==0);
 if(replacing)REQUIRE(namecache_enter(rename_new,&renamed,&target.inode,1)==0);
 if(directory && cross)REQUIRE(namecache_enter(&node.inode,&parent_name,&old.inode,1)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=rename_snapshot;
 rename_extents=2+cross+(directory && cross)+(!shared && (cross || replacing));
 rename_mask=4|8|(cross?32:0)|(!shared && (cross || replacing)?256:0);
 namespace_journal=&fs.journal;group_write_check=rename_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=dir_changes=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;rename_result=EIO;rename_handled=0;
 (void)host_crash_run(perform_rename,&fs);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(rename_result==0 || rename_result==EIO || (media_override_error && rename_result==media_override_error));
 if(!crashed) {
  REQUIRE(rename_handled && fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  committed=fs.journal.committed_sequence!=0;
  REQUIRE(old.inode.i_linkcount==2+directory+(directory && replacing && !cross)-(committed && directory && (cross || replacing)));
  if(cross)REQUIRE(new.inode.i_linkcount==2U+(directory && replacing)+(committed && directory && !replacing));
  if(replacing)REQUIRE(target.inode.i_linkcount==(committed?0:(directory?2:1)));
  if(committed || fs.journal.poisoned) {
   REQUIRE(namecache_lookup(rename_old,&victim,&cached)==ENOENT);
   if(replacing)REQUIRE(namecache_lookup(rename_new,&renamed,&cached)==ENOENT);
   if(directory && cross)REQUIRE(namecache_lookup(&node.inode,&parent_name,&cached)==ENOENT);
   REQUIRE(dir_changes==1+cross+(directory && cross));
  }
 }
 namecache_purge_mount(&mountp);
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 namespace_journal=&recovered;REQUIRE(ufs_journal_replay(&recovered)==0);
 old_number=rename_number(176,"victim");new_number=rename_number(cross?184:176,"renamed");
 committed=new_number==2;
 REQUIRE(old_number==(committed?0:2));
 REQUIRE(new_number==(committed?2:(replacing?(unsigned)target.inode.i_ino:0)));
 old_links=ufs_get16(rename_raw(&old.inode),UFS_DI_NLINK,0);
 new_links=ufs_get16(rename_raw(rename_new),UFS_DI_NLINK,0);
 REQUIRE(old_links==2+directory+(directory && replacing && !cross)-(committed && directory && (cross || replacing)));
 if(cross)REQUIRE(new_links==2U+(directory && replacing)+(committed && directory && !replacing));
 if(replacing){target_links=ufs_get16(rename_raw(&target.inode),UFS_DI_NLINK,0);REQUIRE(target_links==(committed?0:(directory?2U:1U)));}
 REQUIRE(ufs_get16(rename_raw(&node.inode),UFS_DI_NLINK,0)==(directory?2:1));
 REQUIRE(ufs_get64(rename_raw(&node.inode),UFS_DI_DB,0)==160);
 REQUIRE(rename_number(160,"..")==((committed && directory && cross)?(unsigned)new.inode.i_ino:4U));
 if(rename_result==0)REQUIRE(committed);
 group_write_check=NULL;free(fs.cg);
}
static void rename_refusals(void)
{
 AUDIT_STATE fs;AUDIT_INODE node,old,new;struct mount mountp;struct disk disk;
 struct ufs_journal_io io;struct componentname invalid={"a/b",3,0};
 int handled;unsigned reads;uint8_t block[4096];
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_type=INODE_DIR;node.inode.i_mode=S_IFDIR|0755;node.inode.i_linkcount=2;node.inode.i_size=512;
 old=node;old.inode.i_ino=4;old.inode.i_linkcount=3;old.direct[0]=176;
 new=old;new.inode.i_ino=5;new.inode.i_linkcount=2;new.direct[0]=184;
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(persist_inode(&old.inode)==0);REQUIRE(persist_inode(&new.inode)==0);
 rename_record(176,0,512,2,"victim");rename_record(184,0,512,0,"");
 rename_record(160,0,12,2,".");rename_record(160,12,500,4,"..");
 REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 storage_writes=storage_syncs=0;reads=storage_reads;
 mutex_lock(&fs.namespace_lock);
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&renamed,&old.inode,NULL,&handled)==EINVAL && handled);
 node.inode.i_linkcount=0;
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&renamed,&node.inode,NULL,&handled)==EIO && handled);
 node.inode.i_linkcount=2;new.inode.i_linkcount=UINT16_MAX;
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&renamed,&node.inode,NULL,&handled)==EMLINK && handled);
 new.inode.i_linkcount=2;old.inode.i_linkcount=0;
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&renamed,&node.inode,NULL,&handled)==EIO && handled);
 old.inode.i_linkcount=3;REQUIRE(storage_reads==reads);
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&invalid,&node.inode,NULL,&handled)==EINVAL && handled);
 allocation_failure_size=4*4096;
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&renamed,&node.inode,NULL,&handled)==ENOMEM && handled);
 failure_read=storage_reads+1;
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&renamed,&node.inode,NULL,&handled)==EIO && handled);failure_read=0;
 fs.journal.sector_count=26;
 REQUIRE(rename_group(&old.inode,&victim,&new.inode,&renamed,&node.inode,NULL,&handled)==0 && !handled);
 fs.journal.sector_count=130;
 memcpy(block,storage+176*512,4096);
 ufs_put16(block,4,16,0);ufs_put16(block,20,4,0);
 REQUIRE(directory_image_change(&old.inode,block,&victim,2,0,0)==EIO);
 REQUIRE(ufs_get32(block,0,0)==2);
 REQUIRE(storage_writes==0 && storage_syncs==0 && fs.writable);
 REQUIRE(rename_number(176,"victim")==2 && rename_number(184,"renamed")==0);
 REQUIRE(old.inode.i_linkcount==3 && new.inode.i_linkcount==2);
 mutex_unlock(&fs.namespace_lock);free(fs.cg);
}
int main(void)
{
 unsigned cross,directory,replacing,shared,n,mode;
 rename_refusals();
 for(cross=0;cross<2;cross++)for(directory=0;directory<2;directory++)for(replacing=0;replacing<2;replacing++)for(shared=0;shared<2;shared++) {
  rename_scenario(cross,directory,replacing,shared,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=26;n++)rename_scenario(cross,directory,replacing,shared,n,0,mode,0);
  for(n=1;n<=14;n++)rename_scenario(cross,directory,replacing,shared,0,n,0,0);
  for(n=1;n<=26;n++){second_failure=n+1;rename_scenario(cross,directory,replacing,shared,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=40;n++)rename_scenario(cross,directory,replacing,shared,0,0,0,n);}
  crash_mode=0;rename_snapshot=1;snapshot_fail=0;rename_scenario(cross,directory,replacing,shared,0,0,0,0);
  for(n=1;n<=rename_extents;n++){snapshot_fail=n;rename_scenario(cross,directory,replacing,shared,0,0,0,0);}
  rename_snapshot=snapshot_fail=0;media_override_error=EOPNOTSUPP;
  rename_scenario(cross,directory,replacing,shared,0,1,0,0);media_override_error=0;
 }
 printf("UFS grouped rename/cache/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
