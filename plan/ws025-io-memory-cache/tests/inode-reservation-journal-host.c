/* Actual initialized inode reservation with stale-slot and quota recovery checks. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#include "allocation-journal-host.c"
static struct ufs_journal *reservation_journal;
static struct inode_creation_request reservation_request;
static unsigned reservation_snapshot;
static int reservation_result;
static void reservation_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==32 || first==8 || first==UFS_SBLOCK_OFFSET/512) {
  REQUIRE(reservation_journal->pending_ready && reservation_journal->image_valid);
  REQUIRE(ufs_get32(reservation_journal->image,16,0)==3);
  if(reservation_snapshot)REQUIRE(snapshot_calls==3);
 }
}
static void perform_reservation(void *argument)
{ reservation_result=reserve_inode_group(argument,&reservation_request); }
static void reservation_scenario(unsigned directory,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record record;
 uint8_t *cg,*raw;uint8_t before[4096];unsigned n,committed,retained;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 memset(&node,0,sizeof(node));node.inode.i_mount=&mountp;
 cg=storage+32*512;for(n=0;n<3;n++)bit_set(cg+256,n);
 ufs_put32(cg,UFS_CG_NIFREE,29,0);ufs_put32(cg,UFS_CG_NDIR,1,0);
 fs.super.cstotal_nifree=29;fs.super.cstotal_ndir=1;
 memset(storage+8*512,0xa7,4096);raw=storage+8*512+3*UFS_DINODE_SIZE;
 ufs_put32(raw,UFS_DI_GEN,41,0);memcpy(before,storage+8*512,4096);
 REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 memset(&reservation_request,0,sizeof(reservation_request));reservation_request.type=directory?INODE_DIR:INODE_REG;
 reservation_request.mode=0751;reservation_request.uid=123;reservation_request.gid=456;
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=reservation_snapshot;
 reservation_journal=&fs.journal;group_write_check=reservation_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;reservation_result=EIO;
 (void)host_crash_run(perform_reservation,&node.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(reservation_result==0 || reservation_result==EIO || (media_override_error && reservation_result==media_override_error));
 if(!crashed) {
  committed=fs.journal.committed_sequence!=0;retained=committed || fs.journal.poisoned || fs.journal.pending_sequence!=0;
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(node.inode.i_ino==(committed?3:0) && node.inode.i_linkcount==0);
  REQUIRE(fs.super.cstotal_nifree==29-committed && fs.super.cstotal_ndir==1U+(committed && directory));
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,123,&record)==0 && record.inodes==retained && record.blocks==0);
 }
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 reservation_journal=&recovered;REQUIRE(ufs_journal_replay(&recovered)==0);
 committed=bit_test(cg+256,3)!=0;
 REQUIRE(ufs_get32(cg,UFS_CG_NIFREE,0)==29-committed);
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NIFREE,0)==29-committed);
 REQUIRE(ufs_get32(cg,UFS_CG_NDIR,0)==1U+(committed && directory));
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NDIR,0)==1U+(committed && directory));
 if(committed) {
  REQUIRE(ufs_get16(raw,UFS_DI_MODE,0)==(inode_type_mode(reservation_request.type)|0751));
  REQUIRE(ufs_get16(raw,UFS_DI_NLINK,0)==0 && ufs_get64(raw,UFS_DI_SIZE,0)==0);
  REQUIRE(ufs_get32(raw,UFS_DI_UID,0)==123 && ufs_get32(raw,UFS_DI_GID,0)==456);
  REQUIRE(ufs_get64(raw,UFS_DI_BLOCKS,0)==0 && ufs_get32(raw,UFS_DI_EXTSIZE,0)==0);
  REQUIRE(ufs_get32(raw,UFS_DI_GEN,0)==42);
  for(n=0;n<UFS_NDADDR;n++)REQUIRE(ufs_get64(raw,UFS_DI_DB+n*8,0)==0);
  for(n=0;n<UFS_NIADDR;n++)REQUIRE(ufs_get64(raw,UFS_DI_IB+n*8,0)==0);
  for(n=0;n<UFS_NXADDR;n++)REQUIRE(ufs_get64(raw,UFS_DI_EXTB+n*8,0)==0);
 } else REQUIRE(memcmp(raw,before+3*UFS_DINODE_SIZE,UFS_DINODE_SIZE)==0);
 REQUIRE(memcmp(storage+8*512,before,3*UFS_DINODE_SIZE)==0);
 REQUIRE(memcmp(raw+UFS_DINODE_SIZE,before+4*UFS_DINODE_SIZE,4096-4*UFS_DINODE_SIZE)==0);
 if(reservation_result==0)REQUIRE(committed);
 group_write_check=NULL;free(fs.cg);
}
static void reservation_refusals(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;struct ufs_journal_io io;
 struct quota_record record;struct quota_charge charge;unsigned n;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);disk.d_block_size=512;disk.d_block_count=512;
 memset(&node,0,sizeof(node));node.inode.i_mount=&mountp;
 for(n=0;n<3;n++)bit_set(storage+32*512+256,n);
 ufs_put32(storage+32*512,UFS_CG_NIFREE,29,0);fs.super.cstotal_nifree=29;
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 memset(&reservation_request,0,sizeof(reservation_request));reservation_request.type=INODE_NONE;
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 storage_writes=storage_syncs=0;
 REQUIRE(reserve_inode_group(&node.inode,&reservation_request)==EINVAL);reservation_request.type=INODE_REG;
 node.inode.i_ino=3;REQUIRE(reserve_inode_group(&node.inode,&reservation_request)==EINVAL);node.inode.i_ino=0;
 allocation_failure_size=2*4096+UFS_SBLOCK_SIZE;
 REQUIRE(reserve_inode_group(&node.inode,&reservation_request)==ENOMEM);
 failure_read=storage_reads+1;REQUIRE(reserve_inode_group(&node.inode,&reservation_request)==EIO);failure_read=0;
 for(n=3;n<32;n++)bit_set(storage+32*512+256,n);
 REQUIRE(reserve_inode_group(&node.inode,&reservation_request)==ENOSPC);
 for(n=3;n<32;n++)bit_clear(storage+32*512+256,n);
 REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.inodes==0);
 record.inode_hard=1;REQUIRE(quota_set(&fs.quota,QUOTA_USER,&record)==0);
 REQUIRE(quota_reserve(&fs.quota,0,0,0,1,0,&charge)==0);quota_commit(&charge);
 REQUIRE(reserve_inode_group(&node.inode,&reservation_request)==EDQUOT);
 REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.inodes==1);
 REQUIRE(storage_writes==0 && storage_syncs==0 && node.inode.i_ino==0 && fs.writable);
 free(fs.cg);
}
int main(void)
{
 unsigned directory,n,mode;
 reservation_refusals();
 for(directory=0;directory<2;directory++) {
  reservation_scenario(directory,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=22;n++)reservation_scenario(directory,n,0,mode,0);
  for(n=1;n<=14;n++)reservation_scenario(directory,0,n,0,0);
  for(n=1;n<=22;n++){second_failure=n+1;reservation_scenario(directory,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=32;n++)reservation_scenario(directory,0,0,0,n);}crash_mode=0;
  reservation_snapshot=1;reservation_scenario(directory,0,0,0,0);
  for(n=1;n<=3;n++){snapshot_fail=n;reservation_scenario(directory,0,0,0,0);}reservation_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;reservation_scenario(directory,0,1,0,0);media_override_error=0;
 }
 printf("UFS zero-link inode reservation/quota/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
