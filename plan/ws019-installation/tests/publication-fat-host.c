/* Real FAT directory fsync: sector write failure, barrier failure and retry. */
#include "src/drivers/fs/fat.c"
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAT sync line %d: %s\n",__LINE__,#x); abort(); } } while (0)
static unsigned writes, barriers;
static int write_failure, barrier_failure;
void mutex_lock(struct mutex *lock) { (void)lock; }
void mutex_unlock(struct mutex *lock) { (void)lock; }
unsigned long spin_lock_irqsave(struct spinlock *lock) { (void)lock; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long flags) { (void)lock; (void)flags; }
int disk_write_filesystem(struct disk *disk, uint64_t first, uint32_t count, const void *data)
{ (void)disk; (void)first; (void)count; (void)data; writes++; return write_failure; }
int disk_sync(struct disk *disk) { (void)disk; barriers++; return barrier_failure; }
int main(void)
{
	struct fat_mount_state state = {0};
	struct mount mountp = {0};
	struct disk disk = {0};
	struct inode inode = {0};
	struct file file = {0};
	mountp.m_data = &state; mountp.m_disk = &disk;
	state.disk = &disk; state.total_sectors = 16;
	inode.i_type = INODE_DIR; inode.i_mount = &mountp; file.f_inode = &inode;
	REQUIRE(fat_fsync_directory(NULL) == EINVAL);
	REQUIRE(fat_fsync_directory(&file) == 0 && barriers == 1);
	state.sector_cache_valid = 1; state.sector_cache_dirty = 1; state.sector_cache_lba = 1;
	write_failure = EIO;
	REQUIRE(fat_fsync_directory(&file) == EIO && writes == 1 && barriers == 1);
	REQUIRE(state.sector_cache_dirty);
	write_failure = 0; barrier_failure = EIO;
	REQUIRE(fat_fsync_directory(&file) == EIO && writes == 2 && barriers == 2);
	barrier_failure = 0;
	REQUIRE(fat_fsync_directory(&file) == 0 && writes == 2 && barriers == 3);
	puts("FAT directory fsync PASS write/barrier errors and retry");
	return 0;
}
void *kern_malloc(size_t size) { return malloc(size); }
void kern_free(void *pointer) { free(pointer); }
void io_stats_record(enum io_stat_event event, uint64_t bytes) { (void)event; (void)bytes; }
int disk_write_filesystem_context(struct disk *disk, uint64_t first, uint32_t count,
    const void *data, const struct io_context *context)
{ (void)context; return disk_write_filesystem(disk, first, count, data); }
