/* Actual serialized xattr clear with same/different CG release and replay. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#define UFS_AUDIT_GROUPED_FREE
#include "allocation-journal-host.c"
static struct ufs_journal *xattr_journal;
static unsigned xattr_snapshot,xattr_groups,xattr_keep,xattr_extents;
static uint8_t xattr_update_area[16];
static int xattr_result;
static void xattr_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==160 || first==32 || first==224 || first==UFS_SBLOCK_OFFSET/512 || first==8) {
  REQUIRE(xattr_journal->pending_ready && xattr_journal->image_valid);
  REQUIRE(ufs_get32(xattr_journal->image,16,0)==xattr_extents);
  if(xattr_snapshot)REQUIRE(snapshot_calls==xattr_extents);
 }
}
static void perform_xattr_clear(void *argument)
{
 struct inode *inode=argument;
 mutex_lock(&inode->i_lock);xattr_result=extattr_publish(inode,xattr_keep?xattr_update_area:NULL,xattr_keep?sizeof(xattr_update_area):0);mutex_unlock(&inode->i_lock);
}
static void xattr_scenario(unsigned layout,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record record;struct quota_charge charge;
 uint8_t *raw,*cg,*second,*area;unsigned count=layout?2:1,n,committed;uint64_t second_fragment=layout==2?352:168;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 node.direct[0]=176;node.blocks=8+count*8;node.extattr_size=count*4096;node.extattr[0]=160;node.extattr[1]=count==2?second_fragment:0;
 cg=storage+32*512;second=storage+224*512;
 if(layout==2) {
  fs.super.ncg=2;fs.super.fpg=192;fs.super.size=384;
  ufs_put32(cg,UFS_CG_NDBLK,192,0);memcpy(second,cg,4096);ufs_put32(second,UFS_CG_CGX,1,0);
 }
 area=storage+160*512;memset(area,0,4096);ufs_put32(area,0,count*4096,0);area[4]=UFS_EXTATTR_NAMESPACE_USER;area[6]=1;area[7]='x';
 if(count==2)memset(storage+second_fragment*512,0x6b,4096);
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 REQUIRE(quota_reserve(&fs.quota,0,0,count+1,1,0,&charge)==0);quota_commit(&charge);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=xattr_snapshot;
 xattr_groups=layout==2?2:1;xattr_extents=xattr_keep?(count==1?2:4):xattr_groups+2;xattr_journal=&fs.journal;group_write_check=xattr_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;xattr_result=EIO;
 (void)host_crash_run(perform_xattr_clear,&node.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(xattr_result==0 || xattr_result==EIO || (media_override_error && xattr_result==media_override_error));
 if(!crashed) {
  committed=fs.journal.committed_sequence!=0;
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(node.extattr_size==(committed?(xattr_keep?16U:0U):count*4096));
  REQUIRE(node.blocks==(committed?8+xattr_keep*8:8+count*8));
  REQUIRE(fs.super.cstotal_nbfree==(committed?count-xattr_keep:0));
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.blocks==(committed?1+xattr_keep:count+1) && record.inodes==1);
 }
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);xattr_journal=&recovered;
 REQUIRE(ufs_journal_replay(&recovered)==0);
 raw=storage+8*512+2*UFS_DINODE_SIZE;committed=ufs_get32(raw,UFS_DI_EXTSIZE,0)==(xattr_keep?16U:0U);
 REQUIRE(ufs_get32(raw,UFS_DI_EXTSIZE,0)==(committed?(xattr_keep?16U:0U):count*4096));
 REQUIRE(ufs_get64(raw,UFS_DI_EXTB,0)==(committed && !xattr_keep?0:160));
 REQUIRE(ufs_get64(raw,UFS_DI_EXTB+8,0)==(committed || count==1?0:second_fragment));
 REQUIRE(ufs_get64(raw,UFS_DI_BLOCKS,0)==(committed?8+xattr_keep*8:8+count*8));
 REQUIRE(ufs_get64(raw,UFS_DI_DB,0)==176 && !bit_test(cg+264,176));
 for(n=0;n<8;n++) {
  REQUIRE((unsigned)bit_test(cg+264,160+n)==(committed && !xattr_keep));
  if(count==2)REQUIRE((unsigned)bit_test((layout==2?second:cg)+264,(layout==2?160:168)+n)==committed);
 }
 REQUIRE(ufs_get32(cg,UFS_CG_NBFREE,0)==(committed?(layout==2?1-xattr_keep:count-xattr_keep):0));
 if(layout==2)REQUIRE(ufs_get32(second,UFS_CG_NBFREE,0)==committed);
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0)==(committed?count-xattr_keep:0));
 REQUIRE(ufs_get32(storage+160*512,0,0)==(committed && xattr_keep?16U:count*4096));
 if(committed && xattr_keep){REQUIRE(memcmp(storage+160*512,xattr_update_area,16)==0);REQUIRE(storage[160*512+4095]==0);}
 if(count==2)REQUIRE(storage[second_fragment*512+4095]==0x6b);
 if(xattr_result==0)REQUIRE(committed);
 group_write_check=NULL;free(fs.cg);
}
static void xattr_refusals(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;struct ufs_journal_io io;
 int handled;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);disk.d_block_size=512;disk.d_block_count=512;
 node.extattr[0]=160;node.extattr[1]=168;node.extattr_size=8192;node.direct[0]=176;node.blocks=24;
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,34,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 storage_writes=storage_syncs=0;
 mutex_lock(&node.inode.i_lock);
 node.extattr[1]=160;
 REQUIRE(xattr_release_group(&node.inode,&handled)==EIO && handled);node.extattr[1]=168;
 node.extattr_size=8193;
 REQUIRE(xattr_release_group(&node.inode,&handled)==EIO && handled);node.extattr_size=8192;
 node.extattr[1]=0;
 REQUIRE(xattr_release_group(&node.inode,&handled)==EIO && handled);node.extattr[1]=168;
 node.extattr[1]=169;
 REQUIRE(xattr_release_group(&node.inode,&handled)==EIO && handled);node.extattr[1]=168;
 node.blocks=8;
 REQUIRE(xattr_release_group(&node.inode,&handled)==EIO && handled);node.blocks=24;
 bit_set(storage+32*512+264,160);
 REQUIRE(xattr_release_group(&node.inode,&handled)==EIO && handled);bit_clear(storage+32*512+264,160);
 allocation_failure_size=2*4096+UFS_SBLOCK_SIZE;
 REQUIRE(xattr_release_group(&node.inode,&handled)==ENOMEM && handled);
 fs.journal.sector_count=33;
 REQUIRE(xattr_release_group(&node.inode,&handled)==0 && !handled);fs.journal.sector_count=34;
 REQUIRE(storage_writes==0 && storage_syncs==0 && node.blocks==24 && fs.writable);
 /* Two blocks sharing one CG fit exactly three extents in this smaller slot. */
 REQUIRE(xattr_release_group(&node.inode,&handled)==0 && handled);
 REQUIRE(node.extattr_size==0 && node.blocks==8 && fs.super.cstotal_nbfree==2);
 mutex_unlock(&node.inode.i_lock);free(fs.cg);
}
#ifdef UFS_XATTR_RELEASE_LIBRARY
int retained_xattr_clear_main(void)
#else
int main(void)
#endif
{
 unsigned layout,n,mode;
 xattr_refusals();
 for(layout=0;layout<3;layout++) {
  xattr_scenario(layout,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=24;n++)xattr_scenario(layout,n,0,mode,0);
  for(n=1;n<=14;n++)xattr_scenario(layout,0,n,0,0);
  for(n=1;n<=24;n++){second_failure=n+1;xattr_scenario(layout,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=36;n++)xattr_scenario(layout,0,0,0,n);}crash_mode=0;
  xattr_snapshot=1;xattr_scenario(layout,0,0,0,0);
  for(n=1;n<=xattr_groups+2;n++){snapshot_fail=n;xattr_scenario(layout,0,0,0,0);}xattr_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;xattr_scenario(layout,0,1,0,0);media_override_error=0;
 }
 printf("UFS grouped xattr release/quota/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
