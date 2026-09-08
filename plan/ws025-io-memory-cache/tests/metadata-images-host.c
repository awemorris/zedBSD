/* Exercises shared-block edits in every four-inode order and failed admission. */
#define UFS_ALLOCATION_JOURNAL_LIBRARY
#include "allocation-journal-host.c"

static unsigned image_reads;
static unsigned image_fail_read;
static struct mutex *image_lock;

static int image_read(struct disk *disk, uint64_t first, uint32_t count, void *buffer)
{
 int error;
 REQUIRE(mutex_owned(image_lock));
 image_reads++;
 if(image_fail_read)return EIO;
 disk_read_hook=NULL;
 error=disk_read(disk,first,count,buffer);
 disk_read_hook=image_read;
 return error;
}

int main(void)
{
 AUDIT_STATE fs;
 AUDIT_INODE node,prepared[4];
 struct mount mountp,foreign;
 struct disk disk;
 struct ufs_journal_io io;
 struct ufs_metadata_images images;
 uint8_t memory[3*4096],expected[2*4096],*block,*again;
 unsigned a,b,c,d,n,order[4],indices[4]={2,3,18,19};
 uint64_t fragment;

 storage_fixture(&fs,&node,&mountp,&disk,0,0);
 disk.d_block_size=512;disk.d_block_count=512;
 io.context=&disk;io.read=media_read;io.write=media_write;io.flush=media_flush;
 REQUIRE(drv_ufs_journal_init(&fs.journal,&io,380,130,379)==0);
 fs.journal_enabled=1;
 memset(storage+8*512,0xa5,8192);
 memcpy(expected,storage+8*512,sizeof(expected));
 for(n=0;n<4;n++) {
  prepared[n]=node;
  prepared[n].inode.i_ino=indices[n];
  prepared[n].inode.i_linkcount=7+n;
  prepared[n].inode.i_size=4096+n;
  prepared[n].direct[0]=160+n*8;
  encode_inode_locked(&prepared[n].inode,expected+(n/2)*4096);
 }
 storage_writes=storage_syncs=0;
 image_lock=&fs.lock;
 mutex_lock(image_lock);
 disk_read_hook=image_read;
 /* Distinct inode edits sharing either block must survive every ordering. */
 for(a=0;a<4;a++)for(b=0;b<4;b++)for(c=0;c<4;c++)for(d=0;d<4;d++) {
  if(a==b || a==c || a==d || b==c || b==d || c==d)continue;
  order[0]=a;order[1]=b;order[2]=c;order[3]=d;
  metadata_images_init(&images,&mountp,memory,8192);
  image_reads=0;
  for(n=0;n<4;n++)REQUIRE(metadata_image_inode(&images,&prepared[order[n]].inode)==0);
  REQUIRE(images.count==2 && image_reads==2);
  for(n=0;n<2;n++) {
   fragment=8+n*8;
   REQUIRE(metadata_image_get(&images,fragment,&block)==0);
   REQUIRE(memcmp(block,expected+n*4096,4096)==0);
  }
  REQUIRE(image_reads==2);
  REQUIRE(metadata_image_get(&images,8,&block)==0);
  block[4000]=0x39;
  REQUIRE(metadata_image_get(&images,8,&again)==0 && block==again && again[4000]==0x39);
  REQUIRE(metadata_image_get(&images,24,&again)==ENOSPC && again==NULL && images.count==2);
  REQUIRE(image_reads==2);
 }
 /* A failed read admits nothing; retry must reload before sharing the image. */
 metadata_images_init(&images,&mountp,memory,sizeof(memory));
 image_reads=0;image_fail_read=1;
 REQUIRE(metadata_image_get(&images,8,&block)==EIO && block==NULL && images.count==0);
 image_fail_read=0;
 REQUIRE(metadata_image_get(&images,8,&block)==0 && images.count==1 && image_reads==2);
 block[0]=0x73;
 REQUIRE(metadata_image_get(&images,9,&again)==EIO && again==NULL && images.count==1);
 REQUIRE(metadata_image_get(&images,7,&again)==EIO && again==NULL && images.count==1);
 REQUIRE(metadata_image_get(&images,0,&again)==EIO && again==NULL);
 REQUIRE(metadata_image_get(&images,UINT64_MAX,&again)==EIO && again==NULL);
 REQUIRE(metadata_image_get(&images,8,&again)==0 && again==block && again[0]==0x73);
 REQUIRE(image_reads==2);
 foreign=mountp;prepared[0].inode.i_mount=&foreign;
 REQUIRE(metadata_image_inode(&images,&prepared[0].inode)==EXDEV && images.count==1);
 prepared[0].inode.i_mount=&mountp;
 /* Slot and byte limits are rounded down to whole images before admission. */
 fs.journal.sector_count=18;
 metadata_images_init(&images,&mountp,memory,sizeof(memory));
 REQUIRE(images.capacity==2);
 REQUIRE(metadata_image_get(&images,8,&block)==0);
 REQUIRE(metadata_image_get(&images,16,&block)==0);
 REQUIRE(metadata_image_get(&images,24,&block)==ENOSPC && images.count==2);
 metadata_images_init(&images,&mountp,memory,8191);
 REQUIRE(images.capacity==1);
 REQUIRE(metadata_image_get(&images,8,&block)==0);
 REQUIRE(metadata_image_get(&images,16,&block)==ENOSPC && images.count==1);
 fs.journal.sector_count=2;
 metadata_images_init(&images,&mountp,memory,sizeof(memory));
 REQUIRE(images.capacity==0 && metadata_image_get(&images,8,&block)==ENOSPC);
 REQUIRE(storage_writes==0 && storage_syncs==0);
 REQUIRE(storage[8*512]==0xa5 && storage[8*512+4000]==0xa5);
 disk_read_hook=NULL;
 mutex_unlock(image_lock);
 free(fs.cg);
 printf("UFS private metadata image ownership: PASS (%u checks)\n",functional_checks);
 return 0;
}
