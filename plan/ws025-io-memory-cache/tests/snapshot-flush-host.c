/* Exercise the actual UFS snapshot BIO adapter's physical flush identity. */
#include <stdio.h>
#include <stdlib.h>
#include "../temp/p031-driver-fragments/src/drivers/fs/ufs/ufs-vfs.c"
static struct disk *expected_disk;
static int injected_error,completion_error,completions;
int disk_sync(struct disk *disk)
{
 if(disk!=expected_disk) { fputs("snapshot flush used an I/O context as a disk\n",stderr);exit(1); }
 return injected_error;
}
void bio_complete(struct bio *bio,int error,size_t transferred)
{ (void)bio;if(transferred!=0)abort();completion_error=error;completions++; }
void mutex_lock(struct mutex *mutex) { (void)mutex; }
void mutex_unlock(struct mutex *mutex) { (void)mutex; }
int drv_ufs_snapshot_read(struct ufs_snapshot *snapshot,uint64_t first,uint32_t count,void *data)
{ (void)snapshot;(void)first;(void)count;(void)data;abort(); }
int main(void)
{
 struct ufs_mount_state state={0};struct disk physical={0},snapshot={0};struct bio bio={0};
 expected_disk=&physical;state.snapshot_io.disk=&physical;
 state.snapshot.io.context=&state.snapshot_io;snapshot.d_data=&state;bio.b_op=BIO_FLUSH;
 if(snapshot_disk_submit(&snapshot,&bio)!=0 || completions!=1 || completion_error!=0)abort();
 injected_error=EIO;
 if(snapshot_disk_submit(&snapshot,&bio)!=0 || completions!=2 || completion_error!=EIO)abort();
 puts("UFS snapshot physical flush/error: PASS");return 0;
}
