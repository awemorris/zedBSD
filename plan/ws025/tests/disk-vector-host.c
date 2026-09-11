/* Real disk/BIO prefix semantics and bounded vector assembly. */
#define main retained_foundation_main
#include "../../ws019/tests/storage-foundation-test.c"
#undef main
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
static unsigned char vector_media[65536],pool_buffer[65536];
static unsigned pool_off,pool_busy,submissions,fail_submission;
void *io_pool_borrow(size_t wanted,size_t *capacity)
{ CHECK(wanted<=65536);if(pool_off || pool_busy)return NULL;pool_busy=1;*capacity=65536;return pool_buffer; }
void io_pool_release(void *buffer) { CHECK(buffer==pool_buffer && pool_busy);pool_busy=0; }
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
static int vector_submit(struct disk *disk,struct bio *bio)
{
 size_t offset=(size_t)bio->b_mapped_block*disk->d_block_size;
 size_t size=(size_t)bio->b_block_count*disk->d_block_size;
 submissions++;CHECK(offset+size<=sizeof(vector_media));
 if(submissions==fail_submission){bio_complete(bio,EIO,0);return 0;}
 if(bio->b_op==BIO_READ)memcpy(bio->b_data,vector_media+offset,size);
 else {CHECK(bio->b_op==BIO_WRITE);memcpy(vector_media+offset,bio->b_data,size);}
 bio_complete(bio,0,size);return 0;
}
int main(void)
{
 static unsigned char spans[16][8192],contiguous[65536];
 struct disk_vector vectors[16];struct disk *disk;uint32_t completed;
 const struct disk_ops vector_ops={.submit=vector_submit};unsigned i;
 disk_registry_reset();disk=disk_alloc();CHECK(disk);
 strcpy(disk->d_name,"sda");disk->d_block_size=512;disk->d_block_count=128;disk->d_max_transfer_blocks=128;disk->d_ops=&vector_ops;
 CHECK(disk_create(disk)==0);
 for(i=0;i<16;i++){memset(spans[i],i+1,sizeof(spans[i]));vectors[i].data=spans[i];vectors[i].length=4096;}
 CHECK(disk_transfer_vector_context(disk,BIO_WRITE,0,vectors,16,&completed,NULL)==0);
 CHECK(completed==128 && submissions==1 && !pool_busy);
 for(i=0;i<16;i++)CHECK(vector_media[i*4096]==i+1);
 memset(spans,0,sizeof(spans));submissions=0;
 CHECK(disk_transfer_vector_context(disk,BIO_READ,0,vectors,16,&completed,NULL)==0);
 CHECK(completed==128 && submissions==1);
 for(i=0;i<16;i++)CHECK(spans[i][0]==i+1 && spans[i][4096]==0);
 vectors[15].length=1;submissions=0;
 CHECK(disk_transfer_vector_context(disk,BIO_WRITE,0,vectors,16,&completed,NULL)==EINVAL && completed==0 && !submissions);
 vectors[15].length=4096;
 CHECK(disk_transfer_vector_context(disk,BIO_WRITE,1,vectors,16,&completed,NULL)==EINVAL && !submissions);
 claim_error=EBUSY;CHECK(disk_transfer_vector_context(disk,BIO_WRITE,0,vectors,16,&completed,NULL)==EBUSY && !submissions && !pool_busy);claim_error=0;
 /* A partial confirmed BIO prefix changes only its covered destinations. */
 memset(spans,0xee,sizeof(spans));disk->d_max_transfer_blocks=64;submissions=0;fail_submission=2;
 CHECK(disk_transfer_vector_context(disk,BIO_READ,0,vectors,16,&completed,NULL)==EIO);
 CHECK(completed==64 && !pool_busy);
 for(i=0;i<16;i++)CHECK(spans[i][0]==(i<8?i+1:0xee));
 pool_off=1;submissions=0;fail_submission=3;memset(spans,0xee,sizeof(spans));
 CHECK(disk_transfer_vector_context(disk,BIO_READ,0,vectors,16,&completed,NULL)==EIO && completed==16);
 for(i=0;i<16;i++)CHECK(spans[i][0]==(i<2?i+1:0xee));
 fail_submission=0;submissions=0;disk->d_max_transfer_blocks=128;
 for(i=0;i<16;i++)vectors[i].data=contiguous+i*4096;
 CHECK(disk_transfer_vector_context(disk,BIO_READ,0,vectors,16,&completed,NULL)==0);
 CHECK(completed==128 && submissions==1 && memcmp(contiguous,vector_media,65536)==0);
 disk_registry_reset();printf("disk vectors: batch/pool fallback/confirmed prefix/admission PASS (%u checks)\n",checks);return 0;
}
