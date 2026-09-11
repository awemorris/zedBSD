/* Real truncate groups, including interruption between independently durable frees. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#define UFS_AUDIT_GROUPED_FREE
#include "allocation-journal-host.c"
static struct ufs_journal *release_journal;
static unsigned release_snapshot;
static int truncate_result;
static void release_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==32) {
  REQUIRE(release_journal->pending_ready);
  REQUIRE(release_journal->committed_sequence==release_journal->pending_sequence);
  if(release_snapshot)REQUIRE((snapshot_mask&7)==7);
 }
}
static void perform_truncate(void *argument)
{ truncate_result=ufs_truncate(argument,0); }
static unsigned reachable(uint64_t fragment,unsigned depth,unsigned *mask)
{
 unsigned n,count=0;
 if(!fragment)return 0;
 REQUIRE(fragment>=160 && fragment<=184 && (fragment-160)%8==0);
 REQUIRE((*mask&(1U<<((fragment-160)/8)))==0);
 *mask|=1U<<((fragment-160)/8);
 REQUIRE(!bit_test(storage+32*512+264,fragment));
 count++;
 if(depth)for(n=0;n<512;n++)count+=reachable(drv_ufs_get64(storage+fragment*512,n*8,0),depth-1,mask);
 return count;
}
static void release_scenario(unsigned depth,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;
 unsigned n,mask=0,live=0,free_count=0;uint8_t *raw;uint64_t size;
 storage_fixture(&fs,&node,&mountp,&disk,depth,0);
 disk.d_block_size=512;disk.d_block_count=512;
 REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=release_snapshot;
 release_journal=&fs.journal;group_write_check=release_write_check;
 storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=0;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;truncate_result=EIO;
 (void)host_crash_run(perform_truncate,&node.inode);
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(truncate_result==0 || truncate_result==EIO || truncate_result==media_override_error);
 if(!crashed)REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(drv_ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 release_journal=&recovered;
 REQUIRE(drv_ufs_journal_replay(&recovered)==0);
 raw=storage+8*512+2*UFS_DINODE_SIZE;
 for(n=0;n<12;n++)live+=reachable(drv_ufs_get64(raw,UFS_DI_DB+n*8,0),0,&mask);
 for(n=0;n<3;n++)live+=reachable(drv_ufs_get64(raw,UFS_DI_IB+n*8,0),n+1,&mask);
 for(n=0;n<=depth;n++)if(bit_test(storage+32*512+264,160+n*8))free_count++;
 REQUIRE(live+free_count==depth+1);
 REQUIRE(drv_ufs_get64(raw,UFS_DI_BLOCKS,0)==live*8);
 REQUIRE(drv_ufs_get32(storage+32*512,UFS_CG_NBFREE,0)==free_count);
 REQUIRE(drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0)==free_count);
 size=drv_ufs_get64(raw,UFS_DI_SIZE,0);REQUIRE(size==0 || size==13*4096);
 if(truncate_result==0)REQUIRE(live==0 && size==0);
 if(media_override_error && flush_fail==1) {
  REQUIRE(truncate_result==media_override_error);
  REQUIRE(live==depth+1 && free_count==0 && size==13*4096);
 }
 group_write_check=NULL;free(fs.cg);
}
int main(void)
{
 unsigned depth,n,mode;
 for(depth=0;depth<4;depth++) {
  release_scenario(depth,0,0,0,0);
  media_override_error=EOPNOTSUPP;release_scenario(depth,0,1,0,0);media_override_error=0;
  for(mode=0;mode<2;mode++)for(n=1;n<=48;n++)release_scenario(depth,n,0,mode,0);
  for(n=1;n<=32;n++)release_scenario(depth,0,n,0,0);
  for(n=1;n<=48;n++){second_failure=n+1;release_scenario(depth,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++) {
   crash_mode=mode;
   for(n=1;n<=80;n++)release_scenario(depth,0,0,0,n);
  }
  crash_mode=0;release_snapshot=1;snapshot_fail=0;release_scenario(depth,0,0,0,0);
  for(n=1;n<=4*(depth+1);n++){snapshot_fail=n;release_scenario(depth,0,0,0,0);}
  release_snapshot=snapshot_fail=0;
 }
 printf("UFS grouped truncate/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
