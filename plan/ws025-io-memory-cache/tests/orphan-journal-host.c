/* Private mount orphan scan with interrupted recovery, quota rebuild and retry. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#define UFS_AUDIT_GROUPED_FREE
#include "allocation-journal-host.c"
int mutex_init(struct mutex *mutex,enum lock_rank rank,const char *name)
{ (void)rank;(void)name;memset(mutex,0,sizeof(*mutex));return 0; }
static struct ufs_journal *orphan_journal;
static unsigned orphan_directory,orphan_writes,orphan_syncs,orphan_ops,orphan_reads,orphan_snapshot,orphan_snapshots,orphan_read_fail;
static int orphan_result;
static void orphan_write_check(uint64_t first,uint32_t count,const void *buffer)
{
 (void)count;(void)buffer;
 if(first==32 || first==8 || first==UFS_SBLOCK_OFFSET/512)
  REQUIRE(orphan_journal->pending_ready && orphan_journal->image_valid);
}
static void perform_orphan(void *argument)
{ orphan_result=orphan_recover(argument); }
static void orphan_scenario(unsigned directory,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record record;struct quota_charge charge;
 uint8_t *cg,*raw,*attr;uint8_t neighbors[2*UFS_DINODE_SIZE];unsigned n,allocated,data_owned,attr_owned,links;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);disk.d_block_size=512;disk.d_block_count=512;
 node.inode.i_type=INODE_DIR;node.inode.i_mode=S_IFDIR|0755;node.inode.i_size=512;node.inode.i_linkcount=2;
 node.direct[0]=176;node.blocks=8;REQUIRE(persist_inode(&node.inode)==0);
 node.inode.i_type=INODE_REG;node.inode.i_mode=S_IFREG|0600;node.inode.i_size=4096;node.inode.i_linkcount=1;
 node.inode.i_ino=4;node.direct[0]=184;REQUIRE(persist_inode(&node.inode)==0);
 memcpy(neighbors,storage+8*512+2*UFS_DINODE_SIZE,UFS_DINODE_SIZE);
 memcpy(neighbors+UFS_DINODE_SIZE,storage+8*512+4*UFS_DINODE_SIZE,UFS_DINODE_SIZE);
 node.direct[0]=160;node.inode.i_ino=3;node.inode.i_type=directory?INODE_DIR:INODE_REG;node.inode.i_mode=(directory?S_IFDIR:S_IFREG)|0700;
 node.inode.i_linkcount=0;node.inode.i_size=4096;
 node.extattr_size=16;node.extattr[0]=168;node.blocks=16;
 cg=storage+32*512;for(n=0;n<5;n++)bit_set(cg+256,n);
 ufs_put32(cg,UFS_CG_NIFREE,27,0);ufs_put32(cg,UFS_CG_NDIR,1+directory,0);
 fs.super.cstotal_nifree=27;fs.super.cstotal_ndir=1+directory;
 attr=storage+168*512;ufs_put32(attr,0,16,0);attr[4]=UFS_EXTATTR_NAMESPACE_USER;attr[6]=1;attr[7]='x';memcpy(attr+8,"retained",8);
 REQUIRE(persist_inode(&node.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);REQUIRE(quota_reserve(&fs.quota,0,0,4,3,0,&charge)==0);quota_commit(&charge);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=orphan_snapshot;orphan_journal=&fs.journal;orphan_directory=directory;group_write_check=orphan_write_check;
 storage_writes=storage_syncs=storage_reads=0;snapshot_calls=0;failure_read=orphan_read_fail;
 failure_write=write_fail;failure_write_again=second_failure;failure_sync=flush_fail;commit_error=landed;
 crash_cut=stop;crash_ops=crashed=0;orphan_result=EIO;
 (void)host_crash_run(perform_orphan,&mountp);
 orphan_writes=storage_writes;orphan_syncs=storage_syncs;orphan_ops=crash_ops;orphan_reads=storage_reads;orphan_snapshots=snapshot_calls;
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(orphan_result==0 || orphan_result==EIO || (media_override_error && orphan_result==media_override_error));
 if(!crashed) {
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0);
  REQUIRE(record.blocks==4-fs.super.cstotal_nbfree && record.inodes==30-fs.super.cstotal_nifree);
  if(orphan_result==0)REQUIRE(fs.super.cstotal_nifree==28 && fs.super.cstotal_nbfree==2);
 }
 failure_write=failure_write_again=failure_sync=failure_read=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 orphan_journal=&recovered;REQUIRE(ufs_journal_replay(&recovered)==0);
 raw=storage+8*512+3*UFS_DINODE_SIZE;
 allocated=bit_test(cg+256,3)!=0;data_owned=ufs_get64(raw,UFS_DI_DB,0)!=0;attr_owned=ufs_get64(raw,UFS_DI_EXTB,0)!=0;
 links=ufs_get16(raw,UFS_DI_NLINK,0);
 REQUIRE(links==0);
 if(links!=0)REQUIRE(allocated && data_owned && attr_owned);
 REQUIRE((ufs_get16(raw,UFS_DI_MODE,0)!=0)==allocated);
 REQUIRE(ufs_get64(raw,UFS_DI_DB,0)==(data_owned?160:0));
 REQUIRE(ufs_get64(raw,UFS_DI_EXTB,0)==(attr_owned?168:0));
 REQUIRE(ufs_get32(raw,UFS_DI_EXTSIZE,0)==(attr_owned?16:0));
 REQUIRE(ufs_get64(raw,UFS_DI_BLOCKS,0)==8*(data_owned+attr_owned));
 for(n=0;n<8;n++){REQUIRE((unsigned)bit_test(cg+264,160+n)==!data_owned);REQUIRE((unsigned)bit_test(cg+264,168+n)==!attr_owned);}
 REQUIRE(ufs_get32(cg,UFS_CG_NBFREE,0)==2-data_owned-attr_owned);
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0)==2-data_owned-attr_owned);
 REQUIRE(ufs_get32(cg,UFS_CG_NIFREE,0)==28-allocated);
 REQUIRE(ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NIFREE,0)==28-allocated);
 REQUIRE(ufs_get32(cg,UFS_CG_NDIR,0)==1+directory*allocated);
 if(!allocated)REQUIRE(!data_owned && !attr_owned && links==0);
 if(orphan_result==0)REQUIRE(!allocated);
 REQUIRE(memcmp(neighbors,storage+8*512+2*UFS_DINODE_SIZE,UFS_DINODE_SIZE)==0);
 REQUIRE(memcmp(neighbors+UFS_DINODE_SIZE,storage+8*512+4*UFS_DINODE_SIZE,UFS_DINODE_SIZE)==0);
 /* Reconstructs mount-owned accounting, retries, and proves a second scan writes nothing. */
 fs.super.cstotal_nbfree=ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0);
 fs.super.cstotal_nifree=ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NIFREE,0);
 fs.super.cstotal_ndir=ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NDIR,0);
 fs.cg_valid=0;fs.writable=1;snapshot_fail=0;
 mutex_init(&fs.namespace_lock,LOCK_RANK_NAMESPACE,"reboot namespace");
 mutex_init(&fs.lock,LOCK_RANK_INODE,"reboot mount");
 mutex_init(&fs.journal_lock,LOCK_RANK_DEVICE,"reboot journal");
 mutex_init(&fs.snapshot_lock,LOCK_RANK_DEVICE,"reboot snapshot");
 fs.journal_io.context=fs.snapshot_io.context=NULL;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 orphan_journal=&fs.journal;quota_state_init(&fs.quota);REQUIRE(ufs_quota_rebuild(&mountp)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 REQUIRE(orphan_recover(&mountp)==0);
 REQUIRE(!bit_test(cg+256,3) && fs.super.cstotal_nifree==28 && fs.super.cstotal_nbfree==2);
 REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&record)==0 && record.blocks==2 && record.inodes==2);
 storage_writes=storage_syncs=0;REQUIRE(orphan_recover(&mountp)==0);REQUIRE(storage_writes==0 && storage_syncs==0);
 REQUIRE(memcmp(neighbors,storage+8*512+2*UFS_DINODE_SIZE,UFS_DINODE_SIZE)==0);
 REQUIRE(memcmp(neighbors+UFS_DINODE_SIZE,storage+8*512+4*UFS_DINODE_SIZE,UFS_DINODE_SIZE)==0);
 group_write_check=NULL;free(fs.cg);
}
static void orphan_boundaries(void)
{
 AUDIT_STATE fs;AUDIT_INODE node,decoded;struct mount mountp;struct disk disk;
 struct ufs_journal_io io;struct quota_record quota;uint8_t *cg,*raw;unsigned n;
 storage_fixture(&fs,&node,&mountp,&disk,0,0);disk.d_block_size=512;disk.d_block_count=512;
 fs.super.ncg=2;fs.super.fpg=192;fs.super.size=384;
 cg=storage+32*512;ufs_put32(cg,UFS_CG_NDBLK,192,0);
 memset(cg+256,0,8);for(n=0;n<4;n++)bit_set(cg+256,n);
 ufs_put32(cg,UFS_CG_NIFREE,28,0);ufs_put32(cg,UFS_CG_NDIR,1,0);
 memcpy(storage+224*512,cg,4096);cg=storage+224*512;
 ufs_put32(cg,UFS_CG_CGX,1,0);memset(cg+256,0,8);bit_set(cg+256,0);bit_set(cg+256,1);
 ufs_put32(cg,UFS_CG_NIFREE,30,0);
 fs.super.cstotal_nifree=58;fs.super.cstotal_ndir=2;fs.super.cstotal_nbfree=0;
 node.inode.i_type=INODE_DIR;node.inode.i_mode=S_IFDIR|0700;node.inode.i_linkcount=2;node.inode.i_size=512;
 node.direct[0]=176;REQUIRE(persist_inode(&node.inode)==0);
 node.inode.i_ino=3;node.inode.i_type=INODE_REG;node.inode.i_mode=S_IFREG|0600;
 node.inode.i_size=4096;node.inode.i_linkcount=0;node.direct[0]=160;REQUIRE(persist_inode(&node.inode)==0);
 node.inode.i_ino=32;node.inode.i_type=INODE_DIR;node.inode.i_mode=S_IFDIR|0700;
 node.inode.i_size=0;node.direct[0]=352;REQUIRE(persist_inode(&node.inode)==0);
 node.inode.i_ino=33;node.inode.i_type=INODE_REG;node.inode.i_mode=S_IFREG|0600;
 node.inode.i_size=4096;node.inode.i_linkcount=1;node.direct[0]=360;REQUIRE(persist_inode(&node.inode)==0);
 REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;quota_state_init(&fs.quota);REQUIRE(ufs_quota_rebuild(&mountp)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 storage_reads=storage_writes=storage_syncs=0;
 fs.writable=0;REQUIRE(orphan_recover(&mountp)==0);fs.writable=1;
 fs.journal_enabled=0;REQUIRE(orphan_recover(&mountp)==0);fs.journal_enabled=1;
 mountp.m_root=&node.inode;REQUIRE(orphan_recover(&mountp)==EBUSY);mountp.m_root=NULL;
 REQUIRE(storage_reads==0 && storage_writes==0 && storage_syncs==0);
 allocation_failure_size=sizeof(struct ufs_orphan_scan)+8192;
 REQUIRE(orphan_recover(&mountp)==ENOMEM);
 fs.journal.sector_count=41;
 REQUIRE(orphan_recover(&mountp)==EOPNOTSUPP && !fs.writable);fs.writable=1;fs.journal.sector_count=130;
 raw=storage+8*512+3*UFS_DINODE_SIZE;ufs_put64(raw,UFS_DI_DB,379,0);
 REQUIRE(orphan_recover(&mountp)==EIO && !fs.writable);fs.writable=1;ufs_put64(raw,UFS_DI_DB,160,0);
 ufs_put16(raw,UFS_DI_MODE,0,0);
 REQUIRE(orphan_recover(&mountp)==EOPNOTSUPP && !fs.writable);fs.writable=1;ufs_put16(raw,UFS_DI_MODE,S_IFREG|0600,0);
 REQUIRE(storage_writes==0 && storage_syncs==0);
 memset(&decoded,0,sizeof(decoded));decoded.inode.i_mount=&mountp;
 raw=storage+200*512;
 REQUIRE(decode_inode_raw(&decoded.inode,raw,32,0)==EIO);
 memset(&decoded,0,sizeof(decoded));decoded.inode.i_mount=&mountp;
 REQUIRE(decode_inode_raw(&decoded.inode,raw,32,1)==0 && decoded.inode.i_size==0);
 REQUIRE(orphan_recover(&mountp)==0);
 REQUIRE(!bit_test(storage+32*512+256,3) && !bit_test(cg+256,0) && bit_test(cg+256,1));
 REQUIRE(fs.super.cstotal_nifree==60 && fs.super.cstotal_ndir==1 && fs.super.cstotal_nbfree==2);
 REQUIRE(ufs_get64(storage+200*512+UFS_DINODE_SIZE,UFS_DI_DB,0)==360);
 REQUIRE(ufs_get16(storage+200*512+UFS_DINODE_SIZE,UFS_DI_NLINK,0)==1);
 REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&quota)==0 && quota.blocks==2 && quota.inodes==2);
 free(fs.cg);
}
int main(void)
{
 unsigned directory,n,mode,writes,syncs,ops,reads,snapshots;
 orphan_boundaries();
 for(directory=0;directory<2;directory++) {
  orphan_scenario(directory,0,0,0,0);writes=orphan_writes;syncs=orphan_syncs;ops=orphan_ops;reads=orphan_reads;
  for(mode=0;mode<2;mode++)for(n=1;n<=writes;n++)orphan_scenario(directory,n,0,mode,0);
  for(n=1;n<=syncs;n++)orphan_scenario(directory,0,n,0,0);
  for(n=1;n<=writes;n++){second_failure=n+1;orphan_scenario(directory,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=ops;n++)orphan_scenario(directory,0,0,0,n);}crash_mode=0;
  for(n=1;n<=reads;n++){orphan_read_fail=n;orphan_scenario(directory,0,0,0,0);}orphan_read_fail=0;
  orphan_snapshot=1;orphan_scenario(directory,0,0,0,0);snapshots=orphan_snapshots;
  for(n=1;n<=snapshots;n++){snapshot_fail=n;orphan_scenario(directory,0,0,0,0);}orphan_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;orphan_scenario(directory,0,1,0,0);media_override_error=0;
 }
 printf("UFS private mount orphan recovery/retry: PASS (%u checks)\n",functional_checks);return 0;
}
