/* Real bounded UFS allocation plus real journal under failed writes/flushes. */
#define UFS_AUDIT_CUSTOM_ALLOC
#define UFS_AUDIT_CUSTOM_IO
#define main retained_metadata_main
#include "../../ws024-unified-ufs/tests/ufs-metadata-host.c"
#undef main
int host_crash_run(void (*action)(void *),void *argument);
void host_crash_now(void);
static int media_override_error;
static unsigned second_failure,space_limit,memory_refusal,quota_limit;
static unsigned snapshot_active,snapshot_fail,snapshot_calls,snapshot_mask,snapshot_expected;
static unsigned crash_cut,crash_mode,crash_ops,crashed;
static void *owned[32];
void *kern_malloc(size_t bytes)
{
 unsigned n;void *p;
 if(bytes==allocation_failure_size){allocation_failure_size=0;return NULL;}
 p=malloc(bytes);if(!p)return NULL;
 for(n=0;n<32;n++)if(!owned[n]){owned[n]=p;return p;}
 abort();
}
void kern_free(void *p)
{
 unsigned n;if(!p)return;
 for(n=0;n<32;n++)if(owned[n]==p){owned[n]=NULL;free(p);return;}
 abort();
}
void *kern_calloc(size_t n,size_t bytes)
{ void *p=kern_malloc(n*bytes);if(p)memset(p,0,n*bytes);return p; }
static void cut_power(void)
{ crash_ops++;if(crash_cut && crash_ops==crash_cut){crashed=1;host_crash_now();} }
static unsigned char redo[UFS_JOURNAL_IMAGE_BYTES], input[4096];
int ufs_snapshot_preserve(struct ufs_snapshot *snapshot,uint64_t first,uint32_t count)
{
 (void)snapshot;(void)count;snapshot_calls++;
 if(snapshot_calls==snapshot_fail)return EIO;
 if(first==32)snapshot_mask|=1;
 if(first==UFS_SBLOCK_OFFSET/512)snapshot_mask|=2;
 if(first==8)snapshot_mask|=4;
 if(first==176)snapshot_mask|=8;
 if(first==168)snapshot_mask|=16;
 if(first==184)snapshot_mask|=32;
 if(first==304)snapshot_mask|=64;
 if(first==312)snapshot_mask|=128;
 if(first==16)snapshot_mask|=256;
 return 0;
}
static int media_read(void *context,uint64_t first,uint32_t count,void *buffer)
{ return disk_read(context,first,count,buffer); }
static void (*group_write_check)(uint64_t, uint32_t, const void *);
static int media_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
 int error;
 if(group_write_check)group_write_check(first,count,buffer);
 if(snapshot_active && first==32)REQUIRE(snapshot_mask==snapshot_expected);
 error=disk_write(context,first,count,buffer);
 if(crash_mode)memcpy(durable+first*512,storage+first*512,count*512);
 cut_power();return error;
}
static int media_flush(void *context)
{ int error=disk_sync(context);cut_power();return error && media_override_error?media_override_error:error; }
static struct mutex *expected_allocation_lock;
static int owned_metadata_read(struct disk *disk,uint64_t first,uint32_t count,void *buffer)
{
 int error;
 REQUIRE(mutex_owned(expected_allocation_lock));
 disk_read_hook=NULL;error=disk_read(disk,first,count,buffer);
 disk_read_hook=owned_metadata_read;return error;
}
struct request { struct inode *inode; uint64_t logical; ssize_t result; };
static void perform_allocation(void *argument)
{
 struct request *request=argument;
 request->result=allocation_write_run(request->inode,input,sizeof(input),request->logical,NULL);
}
static void scenario(unsigned fail_write,unsigned fail_flush,unsigned landed,unsigned indirect)
{
 static AUDIT_STATE fs;static AUDIT_INODE node;static struct mount mountp;static struct disk disk;
 struct ufs_journal_io io;struct ufs_journal recovered;struct quota_record quota={0};
 uint64_t pointer,free_total,logical,raw_pointer,remaining,divisor;ssize_t result;unsigned allocated,n,needed,level,depth,existing;struct request request;
 storage_fixture(&fs,&node,&mountp,&disk,0,1);
 disk.d_block_size=512;disk.d_block_count=512;node.inode.i_size=0;
 needed=1;existing=0;level=0;logical=0;
 if(indirect==1){node.indirect[0]=176;node.blocks=8;logical=12;existing=1;}
 if(indirect==2){needed=2;logical=15;}
 if(indirect==3 || indirect==5){level=1;logical=12+512+2*512+7;needed=indirect==3?3:2;}
 if(indirect>=4 && indirect!=5){level=2;logical=12+512+512*512+512*512+2*512+7;needed=indirect==4?4:(indirect==6?3:2);}
 if(indirect>=5) {
  node.indirect[level]=304;existing=1;
  ufs_put64(storage+304*512,(level==1?3:2)*8,320,0);
  if(indirect==7){ufs_put64(storage+304*512,8,312,0);existing=2;}
  node.blocks=existing*8;
 }
 if(space_limit)needed=space_limit;
 for(n=160;n<160+needed*8;n++)bit_set(storage+32*512+264,n);
 ufs_put32(storage+32*512,UFS_CG_NBFREE,needed,0);fs.super.cstotal_nbfree=needed;
 if(quota_limit) {
  node.inode.i_uid=1234;quota.id=1234;quota.block_hard=quota_limit;
  REQUIRE(quota_set(&fs.quota,QUOTA_USER,&quota)==0);
  REQUIRE(quota_enable(&fs.quota,QUOTA_USER,1)==0);
 }
 REQUIRE(persist_inode(&node.inode)==0);
 REQUIRE(write_super_summaries(&mountp)==0);
 REQUIRE(disk_sync(&disk)==0);
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&fs.journal,redo,sizeof(redo))==0);
 fs.journal_enabled=1;fs.snapshot_available=snapshot_active;
 snapshot_calls=snapshot_mask=0;
 { const unsigned masks[]={7,15,23,31,63,87,95,151};snapshot_expected=masks[indirect]; }
 storage_writes=storage_syncs=0;
 failure_write=fail_write;failure_write_again=second_failure;failure_sync=fail_flush;commit_error=landed;
 memset(input,0x6b,sizeof(input));
 allocation_failure_size=memory_refusal?3*4096:0;
 crash_ops=crashed=0;
 request.inode=&node.inode;request.logical=logical;request.result=-EIO;
 expected_allocation_lock=&fs.lock;disk_read_hook=owned_metadata_read;
 (void)host_crash_run(perform_allocation,&request);result=request.result;
 disk_read_hook=NULL;
 /* Power loss ends all volatile owners; reclaim their simulated process memory. */
 if(crashed)for(n=0;n<32;n++)if(owned[n])kern_free(owned[n]);
 if(memory_refusal)REQUIRE(result==-ENOMEM);
 else if(quota_limit)REQUIRE(result==-EDQUOT);
 else if(space_limit)REQUIRE(result==0);
 else REQUIRE(result==sizeof(input) || result==-EIO);
 if(memory_refusal || quota_limit || space_limit) {
  REQUIRE(storage_writes==0 && storage_syncs==0);
  if(quota_limit){REQUIRE(quota_get(&fs.quota,QUOTA_USER,1234,&quota)==0);REQUIRE(quota.blocks==0);}
 }
 if(result==sizeof(input))REQUIRE(fs.super.cstotal_nbfree==0);
 if(!crashed && fs.journal.poisoned) {
  unsigned char block[4096];
  REQUIRE(!fs.writable);
  REQUIRE(read_block(&mountp,indirect?176:8,block)==EIO);
 }
 if(!crashed)REQUIRE(fs.journal_io.context==NULL && fs.snapshot_io.context==NULL);
 /* Power loss discards volatile homes, then mount replay reconstructs one group. */
 failure_write=failure_write_again=failure_sync=commit_error=0;crash_cut=0;memcpy(storage,durable,sizeof(storage));
 REQUIRE(ufs_journal_init(&recovered,&io,380,130,379)==0);
 REQUIRE(ufs_journal_bind_image(&recovered,redo,sizeof(redo))==0);
 REQUIRE(ufs_journal_replay(&recovered)==0);
 pointer=ufs_get64(storage+8*512+2*UFS_DINODE_SIZE,UFS_DI_DB,0);
 if(indirect) {
  raw_pointer=ufs_get64(storage+8*512+2*UFS_DINODE_SIZE,UFS_DI_IB+level*8,0);
  remaining=logical-12;
  if(level>=1)remaining-=512;
  if(level>=2)remaining-=512*512;
  for(depth=level+1;depth && raw_pointer;depth--) {
   REQUIRE(raw_pointer<379);
   divisor=1;for(n=1;n<depth;n++)divisor*=512;
   raw_pointer=ufs_get64(storage+raw_pointer*512,(remaining/divisor)*8,0);
   remaining%=divisor;
  }
  pointer=raw_pointer;
 }
 if(indirect>=5)REQUIRE(ufs_get64(storage+304*512,(level==1?3:2)*8,0)==320);
 allocated=!bit_test(storage+32*512+264,160);
 free_total=ufs_get64(storage+UFS_SBLOCK_OFFSET,UFS_FS_CSTOTAL_NBFREE,0);
 REQUIRE(pointer==0 || pointer==160);
 REQUIRE(allocated==(pointer!=0));
 REQUIRE(free_total==(allocated?0U:needed));
 REQUIRE(ufs_get32(storage+32*512,UFS_CG_NBFREE,0)==free_total);
 for(n=0;n<needed;n++)REQUIRE((!bit_test(storage+32*512+264,160+n*8))==allocated);
 REQUIRE(ufs_get64(storage+8*512+2*UFS_DINODE_SIZE,UFS_DI_BLOCKS,0)==(existing+(allocated?needed:0))*8);
 REQUIRE(ufs_get64(storage+8*512+2*UFS_DINODE_SIZE,UFS_DI_SIZE,0)==(pointer?(logical+1)*4096:0));
 if(pointer)REQUIRE(memcmp(storage+160*512,input,sizeof(input))==0);
 if(result==sizeof(input))REQUIRE(pointer==160);
 free(fs.cg);
}
#ifdef UFS_ALLOCATION_JOURNAL_LIBRARY
int retained_allocation_main(void)
#else
int main(void)
#endif
{
 unsigned n,landed,indirect;
 for(indirect=0;indirect<8;indirect++) {
  scenario(0,0,0,indirect);
  for(landed=0;landed<2;landed++)
   for(n=1;n<=32;n++)scenario(n,0,landed,indirect);
  for(n=1;n<=20;n++)scenario(0,n,0,indirect);
  for(n=1;n<=32;n++){second_failure=n+1;scenario(n,0,0,indirect);}
  second_failure=0;
  for(landed=0;landed<2;landed++) {
   crash_mode=landed;
   for(n=1;n<=40;n++){crash_cut=n;scenario(0,0,0,indirect);}
  }
  crash_mode=0;
  if(indirect>=2) {
   space_limit=1;scenario(0,0,0,indirect);space_limit=0;
   memory_refusal=1;scenario(0,0,0,indirect);memory_refusal=0;
   quota_limit=1;scenario(0,0,0,indirect);quota_limit=0;
  }
  {
   const unsigned boundaries[]={4,5,5,6,7,6,7,6};
   snapshot_active=1;snapshot_fail=0;scenario(0,0,0,indirect);
   for(n=1;n<=boundaries[indirect];n++){snapshot_fail=n;scenario(0,0,0,indirect);}
   snapshot_active=snapshot_fail=0;
  }
 }
 printf("UFS allocation grouped journal: PASS (%u checks)\n",functional_checks);return 0;
}
