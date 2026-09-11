/* Actual payload/dinode replacement, including release of a second xattr block. */
#define UFS_XATTR_RELEASE_LIBRARY
#include "xattr-release-journal-host.c"
static void replacement_boundaries(void)
{
 AUDIT_STATE fs;AUDIT_INODE node;struct mount mountp;struct disk disk;struct ufs_journal_io io;
 uint8_t replacement[4096],observed[4096];int handled;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);disk.d_block_size=512;disk.d_block_count=512;
 node.direct[0]=176;node.extattr[0]=160;node.extattr_size=8;node.blocks=16;
 memset(storage+160*512,0,4096);drv_ufs_put32(storage+160*512,0,8,0);
 storage[160*512+4]=UFS_EXTATTR_NAMESPACE_USER;storage[160*512+6]=1;storage[160*512+7]='x';
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,18,379)==0);REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);fs.journal_enabled=1;
 memset(replacement,0x62,sizeof(replacement));drv_ufs_put32(replacement,0,sizeof(replacement),0);
 replacement[4]=UFS_EXTATTR_NAMESPACE_USER;replacement[5]=0;replacement[6]=1;replacement[7]='x';
 storage_writes=storage_syncs=0;
 mutex_lock(&node.inode.i_lock);
 REQUIRE(xattr_existing_group(&node.inode,NULL,16,&handled)==EINVAL && handled);
 REQUIRE(xattr_existing_group(&node.inode,replacement,4097,&handled)==EINVAL && handled);
 allocation_failure_size=8192;
 REQUIRE(xattr_existing_group(&node.inode,replacement,4096,&handled)==ENOMEM && handled);
 bit_set(storage+32*512+264,160);
 REQUIRE(xattr_existing_group(&node.inode,replacement,4096,&handled)==EIO && handled);bit_clear(storage+32*512+264,160);
 fs.journal.sector_count=17;
 REQUIRE(xattr_existing_group(&node.inode,replacement,4096,&handled)==0 && !handled);fs.journal.sector_count=18;
 REQUIRE(storage_writes==0 && storage_syncs==0);
 REQUIRE(extattr_publish(&node.inode,replacement,sizeof(replacement))==0);
 REQUIRE(node.extattr_size==4096 && node.blocks==16 && node.extattr[0]==160);
 REQUIRE(read_block(&mountp,160,observed)==0 && memcmp(observed,replacement,4096)==0);
 REQUIRE(ufs_sync(&mountp)==0);
 REQUIRE(memcmp(storage+160*512,replacement,4096)==0 && fs.super.cstotal_nbfree==0);
 mutex_unlock(&node.inode.i_lock);free(fs.cg);
}
int main(void)
{
 unsigned layout,n,mode;
 replacement_boundaries();
 xattr_keep=1;
 memset(xattr_update_area,0,sizeof(xattr_update_area));
 drv_ufs_put32(xattr_update_area,0,sizeof(xattr_update_area),0);
 xattr_update_area[4]=UFS_EXTATTR_NAMESPACE_USER;xattr_update_area[6]=1;xattr_update_area[7]='x';
 memcpy(xattr_update_area+8,"updated!",8);
 for(layout=0;layout<3;layout++) {
  xattr_scenario(layout,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=24;n++)xattr_scenario(layout,n,0,mode,0);
  for(n=1;n<=14;n++)xattr_scenario(layout,0,n,0,0);
  for(n=1;n<=24;n++){second_failure=n+1;xattr_scenario(layout,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=36;n++)xattr_scenario(layout,0,0,0,n);}crash_mode=0;
  xattr_snapshot=1;xattr_scenario(layout,0,0,0,0);
  for(n=1;n<=xattr_extents;n++){snapshot_fail=n;xattr_scenario(layout,0,0,0,0);}xattr_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;xattr_scenario(layout,0,1,0,0);media_override_error=0;
 }
 printf("UFS grouped xattr replacement/quota/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
