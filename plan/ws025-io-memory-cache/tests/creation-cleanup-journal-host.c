/* Complete failed-creation cleanup with persistent ownership after every cut. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#define UFS_AUDIT_GROUPED_FREE
#include "allocation-journal-host.c"
static struct ufs_journal *cleanup_journal;
static unsigned cleanup_directory,cleanup_writes,cleanup_syncs,cleanup_ops,cleanup_reads,cleanup_snapshot,cleanup_snapshots,cleanup_read_fail;
static int cleanup_result;
static void cleanup_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==32 || first==8 || first==UFS_SBLOCK_OFFSET/512)
  REQUIRE(cleanup_journal->pending_ready && cleanup_journal->image_valid);
}
static void perform_cleanup(void *argument)
{ cleanup_result=discard_new_inode(argument,cleanup_directory); }
static void cleanup_scenario(unsigned directory,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record record;struct quota_charge charge;
 uint8_t *cg,*raw,*attr;unsigned n,allocated,data_owned,attr_owned,links;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_ino=3;node.inode.i_type=directory?INODE_DIR:INODE_REG;node.inode.i_mode=(directory?S_IFDIR:S_IFREG)|0700;
 node.inode.i_linkcount=directory?2:1;node.inode.i_size=4096;
 node.extattr_size=16;node.extattr[0]=168;node.blocks=16;
 cg=storage+32*512;for(n=0;n<4;n++)bit_set(cg+256,n);
 ufs_put32(cg,UFS_CG_NIFREE,28,0);ufs_put32(cg,UFS_CG_NDIR,1+directory,0);
 fs.super.cstotal_nifree=28;fs.super.cstotal_ndir=1+directory;
 attr=storage+168*512;ufs_put32(attr,0,16,0);attr[4]=UFS_EXTATTR_NAMESPACE_USER;attr[6]=1;attr[7]='x';memcpy(attr+8,"retained",8);
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);REQUIRE(quota_reserve(&fs.quota,0,0,2,1,0,&charge)==0);quota_commit(&charge);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=cleanup_snapshot;cleanup_journal=&fs.journal;cleanup_directory=directory;group_write_check=cleanup_write_check;
 storage_writes=storage_syncs=storage_reads=0;snapshot_calls=0;failure_read=cleanup_read_fail;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;cleanup_result=EIO;
 (void)host_crash_run(perform_cleanup,&node.inode);
 cleanup_writes=storage_writes;cleanup_syncs=storage_syncs;cleanup_ops=crash_ops;cleanup_reads=storage_reads;cleanup_snapshots=snapshot_calls;
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(cleanup_result==0 || cleanup_result==EIO || (media_override_error && cleanup_result==media_override_error));
 if(!crashed) {
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0);
  REQUIRE(record.blocks==node.blocks/8 && record.inodes==(node.inode.i_type==INODE_NONE?0:1));
  if(cleanup_result==0)REQUIRE(node.inode.i_ino==0 && node.blocks==0);
 }
 failure_write=failure_write_again=failure_sync=failure_read=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 cleanup_journal=&recovered;REQUIRE(ufs_journal_replay(&recovered)==0);
 raw=storage+8*512+3*UFS_DINODE_SIZE;
 allocated=bit_test(cg+256,3)!=0;data_owned=ufs_get64(raw,UFS_DI_DB,0)!=0;attr_owned=ufs_get64(raw,UFS_DI_EXTB,0)!=0;
 links=ufs_get16(raw,UFS_DI_NLINK,0);
 REQUIRE(links==0 || links==(directory?2U:1U));
 if(links!=0)REQUIRE(allocated && data_owned && attr_owned);
 REQUIRE((ufs_get16(raw,UFS_DI_MODE,0)!=0)==allocated);
 REQUIRE(ufs_get64(raw,UFS_DI_DB,0)==(data_owned?160:0));
 REQUIRE(ufs_get64(raw,UFS_DI_EXTB,0)==(attr_owned?168:0));
 REQUIRE(ufs_get32(raw,UFS_DI_EXTSIZE,0)==(attr_owned?16:0));
 REQUIRE(ufs_get64(raw,UFS_DI_BLOCKS,0)==8*(data_owned+attr_owned));
 for(n=0;n<8;n++){REQUIRE((unsigned)bit_test(cg+264,160+n)==!data_owned);REQUIRE((unsigned)bit_test(cg+264,168+n)==!attr_owned);}
 REQUIRE(ufs_get32(cg,UFS_CG_NBFREE,0)==2-data_owned-attr_owned);
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0)==2-data_owned-attr_owned);
 REQUIRE(ufs_get32(cg,UFS_CG_NIFREE,0)==29-allocated);
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NIFREE,0)==29-allocated);
 REQUIRE(ufs_get32(cg,UFS_CG_NDIR,0)==1+directory*allocated);
 if(!allocated)REQUIRE(!data_owned && !attr_owned && links==0);
 if(cleanup_result==0)REQUIRE(!allocated);
 group_write_check=NULL;free(fs.cg);
}
int main(void)
{
 unsigned directory,n,mode,writes,syncs,ops,reads,snapshots;
 for(directory=0;directory<2;directory++) {
  cleanup_scenario(directory,0,0,0,0);writes=cleanup_writes;syncs=cleanup_syncs;ops=cleanup_ops;reads=cleanup_reads;
  for(mode=0;mode<2;mode++)for(n=1;n<=writes;n++)cleanup_scenario(directory,n,0,mode,0);
  for(n=1;n<=syncs;n++)cleanup_scenario(directory,0,n,0,0);
  for(n=1;n<=writes;n++){second_failure=n+1;cleanup_scenario(directory,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=ops;n++)cleanup_scenario(directory,0,0,0,n);}crash_mode=0;
  for(n=1;n<=reads;n++){cleanup_read_fail=n;cleanup_scenario(directory,0,0,0,0);}cleanup_read_fail=0;
  cleanup_snapshot=1;cleanup_scenario(directory,0,0,0,0);snapshots=cleanup_snapshots;
  for(n=1;n<=snapshots;n++){snapshot_fail=n;cleanup_scenario(directory,0,0,0,0);}cleanup_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;cleanup_scenario(directory,0,1,0,0);media_override_error=0;
 }
 printf("UFS checked creation cleanup/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
