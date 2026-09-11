/* Real final inode retirement, allocation map and directory totals after replay. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#include "allocation-journal-host.c"
static struct ufs_journal *retire_journal;
static unsigned retire_snapshot;
static int retire_result,retire_handled;
static void retire_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==32 || first==UFS_SBLOCK_OFFSET/512 || first==8) {
  REQUIRE(retire_journal->pending_ready && retire_journal->image_valid);
  if(retire_snapshot)REQUIRE(snapshot_calls==3 && (snapshot_mask&7)==7);
 }
}
static void perform_retire(void *argument)
{ retire_result=retire_inode_group(argument,&retire_handled); }
static void retire_scenario(unsigned directory,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record record;
 struct quota_charge charge;uint8_t *raw,*cg;unsigned n,committed,allocated,writes;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_ino=3;node.inode.i_linkcount=0;node.inode.i_size=0;node.direct[0]=0;node.blocks=0;
 node.inode.i_type=directory?INODE_DIR:INODE_REG;node.inode.i_mode=(directory?S_IFDIR:S_IFREG)|0600;
 cg=storage+32*512;for(n=0;n<4;n++)bit_set(cg+256,n);
 drv_ufs_put32(cg,UFS_CG_NIFREE,28,0);drv_ufs_put32(cg,UFS_CG_NDIR,1+directory,0);
 fs.super.cstotal_nifree=28;fs.super.cstotal_ndir=1+directory;
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 REQUIRE(quota_reserve(&fs.quota,0,0,0,1,0,&charge)==0);quota_commit(&charge);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=retire_snapshot;
 retire_journal=&fs.journal;group_write_check=retire_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;retire_result=EIO;retire_handled=0;
 (void)host_crash_run(perform_retire,&node.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(retire_result==0 || retire_result==EIO || (media_override_error && retire_result==media_override_error));
 if(!crashed) {
  committed=fs.journal.committed_sequence!=0;
  REQUIRE(retire_handled && fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE((node.inode.i_mode==0)==committed);
  if(committed){REQUIRE(node.inode.i_ino==0);writes=storage_writes;ufs_reclaim(&node.inode);REQUIRE(ufs_inode_sync(&node.inode)==0);REQUIRE(storage_writes==writes);}
  REQUIRE(fs.super.cstotal_nifree==28+committed);
  REQUIRE(fs.super.cstotal_ndir==1+directory-(committed && directory));
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.inodes==1-committed);
 }
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(drv_ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);retire_journal=&recovered;
 REQUIRE(drv_ufs_journal_replay(&recovered)==0);
 raw=storage+8*512+3*UFS_DINODE_SIZE;cg=storage+32*512;
 allocated=bit_test(cg+256,3)!=0;committed=!allocated;
 REQUIRE((drv_ufs_get16(raw,UFS_DI_MODE,0)!=0)==allocated);
 REQUIRE(drv_ufs_get32(cg,UFS_CG_NIFREE,0)==28+committed);
 REQUIRE(drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NIFREE,0)==28+committed);
 REQUIRE(drv_ufs_get32(cg,UFS_CG_NDIR,0)==1+directory-(committed && directory));
 REQUIRE(drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NDIR,0)==1+directory-(committed && directory));
 REQUIRE(drv_ufs_get16(raw,UFS_DI_NLINK,0)==0 && drv_ufs_get64(raw,UFS_DI_BLOCKS,0)==0);
 REQUIRE(bit_test(cg+256,2) && !bit_test(cg+256,4));
 if(retire_result==0)REQUIRE(committed);
 group_write_check=NULL;free(fs.cg);
}
static void retire_refusals(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;
 struct ufs_journal_io io;unsigned n;int handled;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);fs.journal_enabled=1;
 node.inode.i_ino=3;node.inode.i_linkcount=0;node.inode.i_size=0;node.blocks=0;node.direct[0]=0;
 storage_writes=storage_syncs=0;
 for(n=0;n<7;n++) {
  if(n==0)node.inode.i_linkcount=1;
  if(n==1)node.inode.i_size=1;
  if(n==2)node.blocks=8;
  if(n==3)node.direct[11]=160;
  if(n==4)node.indirect[2]=160;
  if(n==5)node.extattr[1]=160;
  if(n==6)node.extattr_size=1;
  REQUIRE(retire_inode_group(&node.inode,&handled)==EIO && handled);
  node.inode.i_linkcount=0;node.inode.i_size=0;node.blocks=0;node.direct[11]=0;node.indirect[2]=0;node.extattr[1]=0;node.extattr_size=0;
 }
 node.inode.i_ino=UFS_ROOT_INO;
 REQUIRE(retire_inode_group(&node.inode,&handled)==EIO && handled);
 node.inode.i_ino=(uint64_t)1<<40;
 REQUIRE(retire_inode_group(&node.inode,&handled)==EIO && handled);
 node.inode.i_ino=3;
 REQUIRE(retire_inode_group(&node.inode,&handled)==EIO && handled);
 allocation_failure_size=2*4096+UFS_SBLOCK_SIZE;
 REQUIRE(retire_inode_group(&node.inode,&handled)==ENOMEM && handled);
 fs.journal.sector_count=10;
 REQUIRE(retire_inode_group(&node.inode,&handled)==0 && !handled);
 REQUIRE(storage_writes==0 && storage_syncs==0 && fs.writable);
 free(fs.cg);
}
int main(void)
{
 unsigned directory,n,mode;
 retire_refusals();
 for(directory=0;directory<2;directory++) {
  retire_scenario(directory,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=22;n++)retire_scenario(directory,n,0,mode,0);
  for(n=1;n<=14;n++)retire_scenario(directory,0,n,0,0);
  for(n=1;n<=22;n++){second_failure=n+1;retire_scenario(directory,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=32;n++)retire_scenario(directory,0,0,0,n);}crash_mode=0;
  retire_snapshot=1;retire_scenario(directory,0,0,0,0);
  for(n=1;n<=3;n++){snapshot_fail=n;retire_scenario(directory,0,0,0,0);}retire_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;retire_scenario(directory,0,1,0,0);media_override_error=0;
 }
 printf("UFS grouped inode retirement/quota/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
