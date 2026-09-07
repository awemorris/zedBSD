/* Actual first xattr publication with allocation, payload, inode and quota. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#include "allocation-journal-host.c"
static struct ufs_journal *attribute_journal;
static unsigned attribute_snapshot,attribute_second;
static int attribute_result;
static uint8_t attribute_area[16];
static void attribute_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==32 || first==224 || first==160 || first==352 || first==8 || first==UFS_SBLOCK_OFFSET/512) {
  REQUIRE(attribute_journal->pending_ready && attribute_journal->image_valid);
  REQUIRE(ufs_get32(attribute_journal->image,16,0)==4);
  if(attribute_snapshot)REQUIRE(snapshot_calls==4);
 }
}
static void perform_attribute(void *argument)
{
 struct inode *inode=argument;
 mutex_lock(&inode->i_lock);attribute_result=extattr_publish(inode,attribute_area,sizeof(attribute_area));mutex_unlock(&inode->i_lock);
}
static void attribute_scenario(unsigned second,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record record;struct quota_charge charge;
 uint8_t *cg,*raw;uint64_t fragment=second?352:160;unsigned n,committed,retained;
 storage_fixture(&fs,&node,&mountp,&disk,0,1);
 disk.d_block_size=512;disk.d_block_count=512;
 node.direct[0]=176;node.blocks=8;
 cg=storage+32*512;
 if(second) {
  fs.super.ncg=2;fs.super.fpg=192;fs.super.size=384;ufs_put32(cg,UFS_CG_NDBLK,192,0);
  memcpy(storage+224*512,cg,4096);ufs_put32(storage+224*512,UFS_CG_CGX,1,0);
  for(n=160;n<168;n++)bit_clear(cg+264,n);
  ufs_put32(cg,UFS_CG_NBFREE,0,0);cg=storage+224*512;
 }
 memset(storage+fragment*512,0xd7,4096);
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 REQUIRE(quota_reserve(&fs.quota,0,0,1,1,0,&charge)==0);quota_commit(&charge);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=attribute_snapshot;
 attribute_journal=&fs.journal;attribute_second=second;group_write_check=attribute_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;attribute_result=EIO;
 (void)host_crash_run(perform_attribute,&node.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(attribute_result==0 || attribute_result==EIO || (media_override_error && attribute_result==media_override_error));
 if(!crashed) {
  committed=fs.journal.committed_sequence!=0;retained=committed || fs.journal.poisoned || fs.journal.pending_sequence!=0;
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(node.extattr_size==(committed?16:0));REQUIRE(node.extattr[0]==(committed?fragment:0));
  REQUIRE(node.blocks==(committed?16:8) && fs.super.cstotal_nbfree==1-committed);
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.blocks==1+retained && record.inodes==1);
 }
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);attribute_journal=&recovered;
 REQUIRE(ufs_journal_replay(&recovered)==0);
 raw=storage+8*512+2*UFS_DINODE_SIZE;committed=ufs_get32(raw,UFS_DI_EXTSIZE,0)==16;
 REQUIRE(ufs_get32(raw,UFS_DI_EXTSIZE,0)==(committed?16:0));
 REQUIRE(ufs_get64(raw,UFS_DI_EXTB,0)==(committed?fragment:0));
 REQUIRE(ufs_get64(raw,UFS_DI_EXTB+8,0)==0);
 REQUIRE(ufs_get64(raw,UFS_DI_BLOCKS,0)==(committed?16:8));
 REQUIRE(ufs_get64(raw,UFS_DI_DB,0)==176);
 for(n=160;n<168;n++)REQUIRE((unsigned)bit_test(cg+264,n)==1-committed);
 REQUIRE(ufs_get32(cg,UFS_CG_NBFREE,0)==1-committed);
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0)==1-committed);
 if(committed){REQUIRE(memcmp(storage+fragment*512,attribute_area,16)==0);REQUIRE(storage[fragment*512+4095]==0);}
 else REQUIRE(storage[fragment*512]==0xd7);
 if(attribute_result==0)REQUIRE(committed);
 group_write_check=NULL;free(fs.cg);
}
static void attribute_refusals(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;struct ufs_journal_io io;
 struct quota_record record;struct quota_charge charge;int handled;
 storage_fixture(&fs,&node,&mountp,&disk,0,1);disk.d_block_size=512;disk.d_block_count=512;
 node.direct[0]=176;node.blocks=8;
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,42,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);REQUIRE(quota_reserve(&fs.quota,0,0,1,1,0,&charge)==0);quota_commit(&charge);
 storage_writes=storage_syncs=0;
 mutex_lock(&node.inode.i_lock);
 REQUIRE(xattr_allocate_group(&node.inode,NULL,16,&handled)==EINVAL && handled);
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,4097,&handled)==EINVAL && handled);
 node.extattr[1]=168;
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,16,&handled)==EIO && handled);node.extattr[1]=0;
 node.blocks=UINT64_MAX;
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,16,&handled)==EIO && handled);node.blocks=8;
 allocation_failure_size=3*4096+UFS_SBLOCK_SIZE;
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,16,&handled)==ENOMEM && handled);
 failure_read=storage_reads+1;
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,16,&handled)==EIO && handled);failure_read=0;
 bit_clear(storage+32*512+264,160);
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,16,&handled)==ENOSPC && handled);bit_set(storage+32*512+264,160);
 REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.blocks==1 && record.inodes==1);
 record.block_hard=1;REQUIRE(quota_set(&fs.quota,QUOTA_USER,&record)==0);
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,16,&handled)==EDQUOT && handled);
 record.block_hard=0;REQUIRE(quota_set(&fs.quota,QUOTA_USER,&record)==0);
 fs.journal.sector_count=41;
 REQUIRE(xattr_allocate_group(&node.inode,attribute_area,16,&handled)==0 && !handled);fs.journal.sector_count=42;
 REQUIRE(storage_writes==0 && storage_syncs==0 && node.extattr_size==0 && node.blocks==8 && fs.writable);
 REQUIRE(extattr_publish(&node.inode,attribute_area,16)==0);
 REQUIRE(node.extattr[0]==160 && node.extattr_size==16 && node.blocks==16);
 mutex_unlock(&node.inode.i_lock);free(fs.cg);
}
int main(void)
{
 unsigned second,n,mode;
 memset(attribute_area,0,sizeof(attribute_area));ufs_put32(attribute_area,0,16,0);
 attribute_area[4]=UFS_EXTATTR_NAMESPACE_USER;attribute_area[6]=1;attribute_area[7]='x';memcpy(attribute_area+8,"created!",8);
 attribute_refusals();
 for(second=0;second<2;second++) {
  attribute_scenario(second,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=24;n++)attribute_scenario(second,n,0,mode,0);
  for(n=1;n<=14;n++)attribute_scenario(second,0,n,0,0);
  for(n=1;n<=24;n++){second_failure=n+1;attribute_scenario(second,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=36;n++)attribute_scenario(second,0,0,0,n);}crash_mode=0;
  attribute_snapshot=1;attribute_scenario(second,0,0,0,0);
  for(n=1;n<=4;n++){snapshot_fail=n;attribute_scenario(second,0,0,0,0);}attribute_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;attribute_scenario(second,0,1,0,0);media_override_error=0;
 }
 printf("UFS grouped first xattr allocation/quota/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
