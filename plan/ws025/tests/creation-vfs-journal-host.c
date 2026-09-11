/* Actual creation callbacks, including zero-link xattr preparation and rollback. */
#define UFS_AUDIT_GROUPED_FREE
#define UFS_AUDIT_CUSTOM_CREATION
#define UFS_UNLINK_JOURNAL_LIBRARY
#include "unlink-journal-host.c"

int mutex_init(struct mutex *mutex,enum lock_rank rank,const char *name)
{ (void)rank;(void)name;memset(mutex,0,sizeof(*mutex));return 0; }
static struct inode_creation_request create_request;
static struct componentname create_name={"new",3,0};
static struct inode *created;
static int create_result;
static unsigned create_read_failure,create_prepare_failure,create_snapshot;
static unsigned create_deferred;

/* Models ACL preparation with the real xattr publisher, before final publication. */
int inode_creation_prepare(struct inode *parent,struct inode *child,
    const struct inode_creation_request *request)
{
 uint8_t area[16]={0};int error;
 (void)parent;
 REQUIRE(child->i_linkcount==0);
 child->i_mode=inode_type_mode(request->type)|request->mode;
 child->i_uid=request->uid;child->i_gid=request->gid;child->i_rdev=request->rdev;
 child->i_special=request->special;
 if(request->type==INODE_REG || request->type==INODE_DIR) {
  drv_ufs_put32(area,0,16,0);area[4]=UFS_EXTATTR_NAMESPACE_USER;area[6]=1;area[7]='x';
  memcpy(area+8,"prepared",8);
  mutex_lock(&child->i_lock);error=extattr_publish(child,area,sizeof(area));mutex_unlock(&child->i_lock);
  if(error)return error;
 }
 REQUIRE(child->i_linkcount==0);
 return create_prepare_failure?EIO:0;
}

static void perform_create(void *argument)
{
 struct inode *parent=argument;
 switch(create_request.type) {
 case INODE_REG: create_result=ufs_create(parent,&create_name,&create_request,&created);break;
 case INODE_DIR: create_result=ufs_mkdir(parent,&create_name,&create_request,&created);break;
 case INODE_SYMLINK: create_result=ufs_symlink(parent,&create_name,"destination",&create_request,&created);break;
 default: create_result=ufs_mknod(parent,&create_name,&create_request,&created);break;
 }
 if(create_result==0){REQUIRE(created!=NULL);inode_dir_changed(parent);}
 else REQUIRE(created==NULL);
}

static unsigned create_names(uint8_t *block,unsigned size)
{
 unsigned at=0,found=0,length;
 while(at<size) {
  length=drv_ufs_get16(block,at+4,0);
  REQUIRE(length>=8 && !(length&3) && length<=size-at && at%512+length<=512);
  if(drv_ufs_get32(block,at,0)==3 && block[at+7]==3 && !memcmp(block+at+8,"new",3))found++;
  at+=length;
 }
 return found;
}

static void create_scenario(enum inode_type type,unsigned empty,unsigned write_fail,unsigned flush_fail,unsigned landed,unsigned stop)
{
 static AUDIT_STATE fs;static AUDIT_INODE root;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_charge charge;struct quota_record quota;
 uint8_t *cg,*parent,*child;uint64_t backing,attribute,parent_backing,blocks;
 unsigned n,k,found,links,allocated,free_blocks,owned_blocks;
 storage_fixture(&fs,&root,&mountp,&disk,0,1);
 allocations=0;memset(&cached_node,0,sizeof(cached_node));
 disk.d_block_size=512;disk.d_block_count=512;fs.super.maxsymlinklen=120;
 root.inode.i_type=INODE_DIR;root.inode.i_mode=S_IFDIR|0755;root.inode.i_linkcount=2;
 root.inode.i_size=empty?0:512;root.direct[0]=empty?0:176;root.blocks=empty?0:8;
 atomic_u64_store_release(&root.inode.i_dirseq,1);
 cg=storage+32*512;
 for(n=0;n<3;n++)bit_set(cg+256,n);
 for(n=160;n<192;n++)bit_set(cg+264,n);
 if(!empty)for(n=176;n<184;n++)bit_clear(cg+264,n);
 drv_ufs_put32(cg,UFS_CG_NIFREE,29,0);drv_ufs_put32(cg,UFS_CG_NDIR,1,0);
 drv_ufs_put32(cg,UFS_CG_NBFREE,empty?4:3,0);
 fs.super.cstotal_nifree=29;fs.super.cstotal_ndir=1;fs.super.cstotal_nbfree=empty?4:3;
 memset(storage+160*512,0,32*512);
 if(!empty)record(0,512,2,"old");
 REQUIRE(persist_inode(&root.inode)==0);REQUIRE(write_super_summaries(&mountp)==0);REQUIRE(disk_sync(&disk)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 REQUIRE(quota_reserve(&fs.quota,0,0,empty?0:1,1,0,&charge)==0);quota_commit(&charge);
 memset(&create_request,0,sizeof(create_request));create_request.type=type;create_request.mode=0700;
 if(type==INODE_SOCKET)create_request.special=&disk;
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=create_snapshot;
 storage_reads=storage_writes=storage_syncs=0;snapshot_calls=snapshot_mask=dir_changes=0;
 failure_read=create_read_failure;failure_write=write_fail;failure_write_again=second_failure;
 failure_sync=flush_fail;commit_error=landed;crash_cut=stop;crash_ops=crashed=0;create_result=EIO;created=NULL;
 audit_writeback_active=create_deferred;
 (void)host_crash_run(perform_create,&root.inode);
 audit_writeback_active=0; /* Remount does not inherit volatile opt-in policy. */
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 REQUIRE(create_result==0 || create_result==EIO || (media_override_error && create_result==media_override_error));
 if(!crashed) {
  REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
  if(create_result!=0 && type==INODE_SOCKET)REQUIRE(cached_node.inode.i_special==NULL);
  REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&quota)==0);
  owned_blocks=(unsigned)((root.blocks+cached_node.blocks)/8);
  REQUIRE(quota.blocks>=owned_blocks && quota.blocks<=owned_blocks+!fs.writable);
  REQUIRE(quota.inodes>=1U+(cached_node.inode.i_ino!=0));
  REQUIRE(quota.inodes<=1U+(cached_node.inode.i_ino!=0)+!fs.writable);
 }
 namecache_purge_mount(&mountp);
 failure_read=failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;
 memcpy(storage,durable,sizeof(storage));
 REQUIRE(drv_ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(drv_ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);REQUIRE(drv_ufs_journal_replay(&recovered)==0);
 parent=storage+8*512+2*UFS_DINODE_SIZE;child=parent+UFS_DINODE_SIZE;
 parent_backing=drv_ufs_get64(parent,UFS_DI_DB,0);backing=drv_ufs_get64(child,UFS_DI_DB,0);
 attribute=drv_ufs_get64(child,UFS_DI_EXTB,0);blocks=drv_ufs_get64(child,UFS_DI_BLOCKS,0);
 allocated=bit_test(cg+256,3);links=drv_ufs_get16(child,UFS_DI_NLINK,0);
 REQUIRE(allocated==(drv_ufs_get16(child,UFS_DI_MODE,0)!=0));
 REQUIRE(drv_ufs_get32(cg,UFS_CG_NIFREE,0)==29-allocated);
 REQUIRE(drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NIFREE,0)==29-allocated);
 REQUIRE(drv_ufs_get32(cg,UFS_CG_NDIR,0)==1U+(allocated && type==INODE_DIR));
 REQUIRE(drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NDIR,0)==1U+(allocated && type==INODE_DIR));
 REQUIRE(drv_ufs_get64(parent,UFS_DI_SIZE,0)==0 || drv_ufs_get64(parent,UFS_DI_SIZE,0)==512);
 found=parent_backing?create_names(storage+parent_backing*512,(unsigned)drv_ufs_get64(parent,UFS_DI_SIZE,0)):0;
 REQUIRE(found<=1 && links==found*(type==INODE_DIR?2U:1U));
 REQUIRE(drv_ufs_get16(parent,UFS_DI_NLINK,0)==2+(found && type==INODE_DIR));
 REQUIRE(!found || allocated);
 if(create_result==0)REQUIRE(found==1);
 if(found && type==INODE_DIR) {
  REQUIRE(drv_ufs_get64(child,UFS_DI_SIZE,0)==512);
  REQUIRE(drv_ufs_get32(storage+backing*512,0,0)==3 && storage[backing*512+7]==1 && storage[backing*512+8]=='.');
  REQUIRE(drv_ufs_get32(storage+backing*512,12,0)==2 && storage[backing*512+19]==2 && !memcmp(storage+backing*512+20,"..",2));
 }
 if(found && (type==INODE_REG || type==INODE_DIR)) {
  REQUIRE(drv_ufs_get32(child,UFS_DI_EXTSIZE,0)==16 && attribute!=0);
  REQUIRE(memcmp(storage+attribute*512+8,"prepared",8)==0);
 }
 if(type==INODE_SYMLINK && found)REQUIRE(memcmp(child+UFS_DI_DB,"destination",11)==0);
 if(type!=INODE_REG && type!=INODE_DIR)backing=0;
 owned_blocks=(parent_backing!=0)+(backing!=0)+(attribute!=0);
 REQUIRE(blocks==8U*((backing!=0)+(attribute!=0)));
 free_blocks=0;
 for(n=160;n<192;n+=8) {
  unsigned used=n==parent_backing || n==backing || n==attribute;
  for(k=0;k<8;k++)REQUIRE((unsigned)bit_test(cg+264,n+k)==!used);
  free_blocks+=!used;
 }
 REQUIRE(free_blocks+owned_blocks==4);
 REQUIRE(drv_ufs_get32(cg,UFS_CG_NBFREE,0)==free_blocks);
 REQUIRE(drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0)==free_blocks);
 /* Reboots into the actual private orphan owner after validating the crash state. */
 fs.super.cstotal_nbfree=drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0);
 fs.super.cstotal_nifree=drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NIFREE,0);
 fs.super.cstotal_ndir=drv_ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NDIR,0);
 fs.cg_valid=0;fs.writable=1;snapshot_fail=0;
 mutex_init(&fs.namespace_lock,LOCK_RANK_NAMESPACE,"reboot namespace");
 mutex_init(&fs.lock,LOCK_RANK_INODE,"reboot mount");
 mutex_init(&fs.journal_lock,LOCK_RANK_DEVICE,"reboot journal");
 mutex_init(&fs.snapshot_lock,LOCK_RANK_DEVICE,"reboot snapshot");
 fs.journal_io.context=fs.snapshot_io.context=NULL;
 memset(&cached_node,0,sizeof(cached_node));allocations=0;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);REQUIRE(drv_ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 quota_state_init(&fs.quota);REQUIRE(ufs_quota_rebuild(&mountp)==0);
 REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);REQUIRE(orphan_recover(&mountp)==0);
 REQUIRE((unsigned)bit_test(cg+256,3)==found);
 REQUIRE(drv_ufs_get16(child,UFS_DI_NLINK,0)==links);
 owned_blocks=(parent_backing!=0)+(found?((backing!=0)+(attribute!=0)):0);
 REQUIRE(fs.super.cstotal_nbfree==4-owned_blocks && fs.super.cstotal_nifree==29-found);
 REQUIRE(quota_get(&fs.quota,QUOTA_USER,0,&quota)==0 && quota.blocks==owned_blocks && quota.inodes==1+found);
 storage_writes=storage_syncs=0;REQUIRE(orphan_recover(&mountp)==0);REQUIRE(storage_writes==0 && storage_syncs==0);
 free(fs.cg);
}

int main(void)
{
 const enum inode_type types[]={INODE_REG,INODE_DIR,INODE_SYMLINK,INODE_FIFO,INODE_SOCKET,INODE_CHAR,INODE_BLOCK};
 unsigned t,empty,n,mode;
 for(create_deferred=0;create_deferred<2;create_deferred++)
 for(t=0;t<sizeof(types)/sizeof(types[0]);t++)for(empty=0;empty<2;empty++) {
  create_scenario(types[t],empty,0,0,0,0);
  for(mode=0;mode<2;mode++)for(n=1;n<=100;n++)create_scenario(types[t],empty,n,0,mode,0);
  for(n=1;n<=70;n++)create_scenario(types[t],empty,0,n,0,0);
  for(n=1;n<=100;n++){second_failure=n+1;create_scenario(types[t],empty,n,0,0,0);}second_failure=0;
  for(mode=0;mode<2;mode++){crash_mode=mode;for(n=1;n<=180;n++)create_scenario(types[t],empty,0,0,0,n);}crash_mode=0;
  for(n=1;n<=70;n++){create_read_failure=n;create_scenario(types[t],empty,0,0,0,0);}create_read_failure=0;
  create_prepare_failure=1;create_scenario(types[t],empty,0,0,0,0);create_prepare_failure=0;
  create_snapshot=1;
  for(n=1;n<=40;n++){snapshot_fail=n;create_scenario(types[t],empty,0,0,0,0);}create_snapshot=snapshot_fail=0;
  media_override_error=EOPNOTSUPP;create_scenario(types[t],empty,0,1,0,0);media_override_error=0;
 }
 printf("UFS creation callbacks/zero-link preparation/recovery: PASS (%u checks)\n",functional_checks);return 0;
}
