/* Actual empty directory backing allocation with preserved xattrs and zero links. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#include "allocation-journal-host.c"
static struct ufs_journal *backing_journal;
static unsigned backing_snapshot,backing_second;
static int backing_result;
static unsigned backing_add,backing_read_failure;

static void backing_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==32 || first==224 || first==160 || first==352 || first==8 || first==UFS_SBLOCK_OFFSET/512) {
  REQUIRE(backing_journal->pending_ready && backing_journal->image_valid);
  REQUIRE(drv_ufs_get32(backing_journal->image,16,0)==4 || (backing_add && drv_ufs_get32(backing_journal->image,16,0)==1));
  if(backing_snapshot)REQUIRE(snapshot_calls==4);
 }
}
static void perform_backing(void *argument)
{
 struct inode *inode=argument;int handled;
 struct componentname name={.cn_nameptr="child",.cn_namelen=5};
 if(backing_add){backing_result=dir_add(inode,&name,3,8);return;}
 mutex_lock(&inode->i_lock);backing_result=directory_backing_group(inode,&handled);REQUIRE(handled);mutex_unlock(&inode->i_lock);
}
static void backing_scenario(unsigned second,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record record;struct quota_charge charge;
 uint8_t *cg,*raw;uint64_t fragment=second?352:160;unsigned n,committed,retained;
 storage_fixture(&fs,&node,&mountp,&disk,0,1);
 disk.d_block_size=512;disk.d_block_count=512;
 node.direct[0]=0;node.blocks=8;node.extattr[0]=176;node.extattr_size=16;
 node.inode.i_type=INODE_DIR;node.inode.i_mode=S_IFDIR|0700;node.inode.i_size=0;node.inode.i_linkcount=0;
 cg=storage+32*512;
 if(second) {
  fs.super.ncg=2;fs.super.fpg=192;fs.super.size=384;drv_ufs_put32(cg,UFS_CG_NDBLK,192,0);
  memcpy(storage+224*512,cg,4096);drv_ufs_put32(storage+224*512,UFS_CG_CGX,1,0);
  for(n=160;n<168;n++)bit_clear(cg+264,n);
  drv_ufs_put32(cg,UFS_CG_NBFREE,0,0);cg=storage+224*512;
 }
 memset(storage+fragment*512,0xd7,4096);
 memset(storage+176*512,0xa6,4096);
 memset(storage+8*512+3*UFS_DINODE_SIZE,0xb5,UFS_DINODE_SIZE);
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 REQUIRE(quota_reserve(&fs.quota,0,0,1,1,0,&charge)==0);quota_commit(&charge);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=backing_snapshot;
 backing_journal=&fs.journal;backing_second=second;group_write_check=backing_write_check;
 storage_reads=0;failure_read=backing_read_failure;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;backing_result=EIO;
 (void)host_crash_run(perform_backing,&node.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(backing_result==0 || backing_result==EIO || (media_override_error && backing_result==media_override_error));
 if(!crashed) {
  committed=fs.journal.committed_sequence!=0;retained=committed || fs.journal.poisoned || fs.journal.pending_sequence!=0;
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(node.extattr_size==16 && node.extattr[0]==176);
  REQUIRE(node.direct[0]==(committed?fragment:0));
  REQUIRE((node.inode.i_size==0 || (backing_add && node.inode.i_size==512)) && node.inode.i_linkcount==0);
  REQUIRE(node.blocks==(committed?16:8) && fs.super.cstotal_nbfree==1-committed);
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.blocks==1+retained && record.inodes==1);
 }
 failure_read=0;failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(drv_ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);backing_journal=&recovered;
 REQUIRE(drv_ufs_journal_replay(&recovered)==0);
 raw=storage+8*512+2*UFS_DINODE_SIZE;committed=drv_ufs_get64(raw,UFS_DI_DB,0)==fragment;
 REQUIRE(drv_ufs_get32(raw,UFS_DI_EXTSIZE,0)==16);
 REQUIRE(drv_ufs_get64(raw,UFS_DI_EXTB,0)==176);
 REQUIRE(drv_ufs_get64(raw,UFS_DI_EXTB+8,0)==0);
 REQUIRE(drv_ufs_get64(raw,UFS_DI_BLOCKS,0)==(committed?16:8));
 REQUIRE(drv_ufs_get64(raw,UFS_DI_DB,0)==(committed?fragment:0));
 REQUIRE(drv_ufs_get64(raw,UFS_DI_SIZE,0)==0 || (backing_add && committed && drv_ufs_get64(raw,UFS_DI_SIZE,0)==512));
 if(drv_ufs_get64(raw,UFS_DI_SIZE,0)==512){REQUIRE(drv_ufs_get32(storage+fragment*512,0,0)==3);REQUIRE(memcmp(storage+fragment*512+8,"child",5)==0);}REQUIRE(drv_ufs_get16(raw,UFS_DI_NLINK,0)==0);
 for(n=160;n<168;n++)REQUIRE((unsigned)bit_test(cg+264,n)==1-committed);
 REQUIRE(drv_ufs_get32(cg,UFS_CG_NBFREE,0)==1-committed);
 REQUIRE(drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0)==1-committed);
 if(committed){for(n=backing_add?512:0;n<4096;n++)REQUIRE(storage[fragment*512+n]==0);}
 else REQUIRE(storage[fragment*512]==0xd7);
 for(n=0;n<4096;n++)REQUIRE(storage[176*512+n]==0xa6);
 for(n=0;n<UFS_DINODE_SIZE;n++)REQUIRE(storage[8*512+3*UFS_DINODE_SIZE+n]==0xb5);
 if(backing_result==0)REQUIRE(committed);
 group_write_check=NULL;free(fs.cg);
}
static void backing_refusals(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;
 struct ufs_journal_io io;struct quota_record record;int handled;
 storage_fixture(&fs,&node,&mountp,&disk,0,1);
 disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_type=INODE_DIR;node.inode.i_mode=S_IFDIR|0700;node.inode.i_size=0;
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,42,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 storage_writes=storage_syncs=0;
 mutex_lock(&node.inode.i_lock);
 node.inode.i_type=INODE_REG;
 REQUIRE(directory_backing_group(&node.inode,&handled)==EIO && handled);node.inode.i_type=INODE_DIR;
 node.inode.i_size=512;
 REQUIRE(directory_backing_group(&node.inode,&handled)==EIO && handled);node.inode.i_size=0;
 node.direct[1]=176;
 REQUIRE(directory_backing_group(&node.inode,&handled)==EIO && handled);node.direct[1]=0;
 node.indirect[2]=176;
 REQUIRE(directory_backing_group(&node.inode,&handled)==EIO && handled);node.indirect[2]=0;
 node.blocks=UINT64_MAX;
 REQUIRE(directory_backing_group(&node.inode,&handled)==EIO && handled);node.blocks=0;
 allocation_failure_size=3*4096+UFS_SBLOCK_SIZE;
 REQUIRE(directory_backing_group(&node.inode,&handled)==ENOMEM && handled);
 REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.blocks==0);
 fs.journal.sector_count=41;
 REQUIRE(directory_backing_group(&node.inode,&handled)==0 && !handled);
 REQUIRE(storage_writes==0 && storage_syncs==0 && node.direct[0]==0 && node.blocks==0 && fs.writable);
 mutex_unlock(&node.inode.i_lock);free(fs.cg);
}
int main(void)
{
 unsigned second,n,mode;
 backing_refusals();
 for(backing_add=0;backing_add<2;backing_add++)for(second=0;second<2;second++) {
  backing_scenario(second,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=48;n++)backing_scenario(second,n,0,mode,0);
  for(n=1;n<=28;n++)backing_scenario(second,0,n,0,0);
  for(n=1;n<=48;n++){second_failure=n+1;backing_scenario(second,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=72;n++)backing_scenario(second,0,0,0,n);}crash_mode=0;
  for(n=1;n<=24;n++){backing_read_failure=n;backing_scenario(second,0,0,0,0);}backing_read_failure=0;
  if(backing_add)continue;
  backing_snapshot=1;backing_scenario(second,0,0,0,0);
  for(n=1;n<=4;n++){snapshot_fail=n;backing_scenario(second,0,0,0,0);}backing_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;backing_scenario(second,0,1,0,0);media_override_error=0;
 }
 printf("UFS grouped first directory backing/quota/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
