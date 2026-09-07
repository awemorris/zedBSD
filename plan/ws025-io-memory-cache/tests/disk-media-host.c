/* Actual disk admission and late completion after irreversible media loss. */
#define main retained_foundation_main
#include "../../ws019-installation/tests/storage-foundation-test.c"
#undef main
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
void hal_fatal(const char *file,int line,const char *message)
{ fprintf(stderr,"%s:%d %s\n",file,line,message);abort(); }
static struct bio *held;
static unsigned submissions;
static unsigned discards, discard_busy, discard_pin;
int backing_mutation_begin_retired_disk(struct disk *d,struct backing_mutation_guard *g)
{ CHECK(d->d_media_revoked);memset(g,0,sizeof(*g));return claim_error; }
int buf_discard_media(struct disk *d)
{ CHECK(d->d_media_revoked);discards++;if(discard_pin){discard_pin=0;disk_ref(d);}return discard_busy?EBUSY:0; }
static int hold_submit(struct disk *disk,struct bio *bio)
{ (void)disk;CHECK(held==NULL);held=bio;submissions++;return 0; }

int main(void)
{
 struct disk *disk,*part,*token,*resolved,*loop,*failed;struct bio bio,next;
 struct disk_ops ops={.submit=hold_submit};unsigned char bytes[512];
 struct partition entry;
 uint64_t epoch,mapped,proof;unsigned refs;
 disk_registry_reset();disk=disk_alloc();CHECK(disk);
 strcpy(disk->d_name,"sda");disk->d_block_size=512;disk->d_block_count=128;disk->d_ops=&ops;
 CHECK(disk_create(disk)==0);partition_reset();memset(&entry,0,sizeof(entry));
 entry.p_parent=disk;entry.p_data_block=1;entry.p_block_count=64;
 CHECK(partition_create_disk(&entry)==0);part=entry.p_disk;
 CHECK(partition_count()==1 && partition_at(0)->p_disk==part);
 loop=disk_alloc();CHECK(loop);
 strcpy(loop->d_name,"loop0");loop->d_block_size=512;loop->d_block_count=32;
 loop->d_ops=&ops;loop->d_media_backing=part;
 refs=refcount_load(&part->d_refs);
 CHECK(disk_create(loop)==0 && refcount_load(&part->d_refs)==refs+1);
 CHECK(disk_media_status(loop)==0);
 CHECK(disk_resolve_range(loop,0,1,&resolved,&mapped)==0 && resolved==loop && mapped==0);
 /* Publication failure must not release a dependency that it never acquired. */
 failed=disk_alloc();CHECK(failed);strcpy(failed->d_name,"loop0");
 failed->d_block_size=512;failed->d_block_count=1;failed->d_ops=&ops;failed->d_media_backing=part;
 CHECK(disk_create(failed)==EEXIST);CHECK(disk_destroy(failed)==0);
 CHECK(refcount_load(&part->d_refs)==refs+1);
 refs=refcount_load(&disk->d_refs);
 CHECK(disk_buffer_acquire(disk)==0 && disk->d_buffer_refs==1 && refcount_load(&disk->d_refs)==refs+1);
 CHECK(disk_open(part)==0);CHECK(disk_cache_acquire(part,&token)==0 && token==disk);
 memset(&bio,0,sizeof(bio));bio.b_op=BIO_READ;bio.b_block_count=1;bio.b_data=bytes;
 CHECK(bio_submit(part,&bio)==0 && held==&bio && disk->d_inflight==1);
 epoch=disk->d_media_epoch;proof=disk->d_persist_epoch;disk->d_stable_valid=1;
 disk_persistence_forget(part);
 CHECK(disk->d_media_epoch==epoch && disk->d_persist_epoch==proof+1 && !disk->d_stable_valid);
 CHECK(disk_media_status(part)==0 && disk->d_inflight==1);
 epoch=disk->d_media_epoch;refs=refcount_load(&disk->d_refs);
 disk_media_revoke(part);CHECK(disk->d_media_revoked && disk->d_media_epoch==epoch+1);
 CHECK(disk_media_retire(disk)==EBUSY && discards==0);
 CHECK(refcount_load(&disk->d_refs)==refs && disk->d_inflight==1);
 disk_media_revoke(disk);CHECK(disk->d_media_epoch==epoch+1);
 CHECK(disk_buffer_acquire(disk)==ENXIO && disk->d_buffer_refs==1);
 disk_buffer_release(disk);CHECK(disk->d_buffer_refs==0);
 CHECK(disk_media_status(part)==ENXIO && disk_media_status(disk)==ENXIO);
 CHECK(disk_media_status(loop)==ENXIO);
 CHECK(disk_cache_acquire(loop,&resolved)==ENXIO && resolved==NULL);
 CHECK(disk_resolve_range(loop,0,1,&resolved,&mapped)==ENXIO);
 CHECK(disk_open(loop)==ENXIO);
 CHECK(disk_resolve_range(part,0,1,&resolved,&mapped)==ENXIO);
 CHECK(disk_cache_acquire(part,&resolved)==ENXIO && resolved==NULL);
 CHECK(disk_open(part)==ENXIO);
 next=bio;next.b_initialized=0;next.b_state=BIO_NEW;
 CHECK(bio_submit(part,&next)==ENXIO && submissions==1);
 held=NULL;bio_complete(&bio,0,512);
 CHECK(bio.b_state==BIO_COMPLETED && bio.b_error==ESTALE && bio.b_transferred==0);
 CHECK(disk->d_inflight==0);
 disk_cache_release(token);disk_close(part);
 CHECK(disk->d_cache_users==0 && part->d_open_count==0);
 CHECK(disk_media_retire(disk)==EBUSY);
 disk_media_revoke(loop);CHECK(disk_media_retire(loop)==0);
 CHECK(disk_destroy(loop)==0 && refcount_load(&part->d_refs)==1);
 discards=0;
 claim_error=EBUSY;CHECK(disk_media_retire(disk)==EBUSY && discards==0);claim_error=0;
 discard_busy=1;CHECK(disk_media_retire(disk)==EBUSY && disk->d_state==DISK_LIVE && part->d_state==DISK_LIVE);discard_busy=0;
 discard_pin=1;CHECK(disk_media_retire(disk)==EBUSY && disk->d_state==DISK_LIVE);disk_release(disk);
 atomic_store_release(&partition_reloading,1);
 CHECK(partition_retire_media(disk)==EBUSY && partition_count()==1);
 CHECK(partition_create_disk(&entry)==EBUSY);
 atomic_store_release(&partition_reloading,0);
 discard_busy=1;CHECK(partition_retire_media(disk)==EBUSY && partition_count()==1);discard_busy=0;
 CHECK(partition_retire_media(disk)==0 && disk->d_state==DISK_GONE);
 CHECK(partition_count()==0 && partition_at(0)==NULL);
 CHECK(disk_find("sda")==NULL && disk_find("sda1")==NULL);
 CHECK(disk_destroy(disk)==0);
 disk_registry_reset();printf("disk media: irreversible ancestry admission, idempotence, late completion, retained release PASS (%u checks)\n",checks);
 return 0;
}
