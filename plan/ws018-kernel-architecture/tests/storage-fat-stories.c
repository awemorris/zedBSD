/* Production FAT + loop submit + buffer cache, memory medium and VFS adapters.
 * SPDX-License-Identifier: Zlib */
#define main fat_regression_main
#define disk_read media_read
#define disk_write_filesystem media_write
#define disk_write_filesystem_context media_write_context
#include "fat-native-vfs-host-test.c"
#undef main
#undef disk_read
#undef disk_write_filesystem
#undef disk_write_filesystem_context
#include "../../../src/kern/buf.c"
static int extent_fault;
static unsigned live_claims;
static int loop_test_file_extents(struct file *, fat_extent_cb, void *);
#define fat_file_extents loop_test_file_extents
#include "../../../src/drivers/loop.c"
#undef fat_file_extents

static int loop_test_file_extents(struct file *f, fat_extent_cb cb, void *context)
{
	if (extent_fault) return cb(1, 100, 16, context);
	return fat_file_extents(f, cb, context);
}
void *kern_calloc(size_t n, size_t size) { return calloc(n, size); }
struct backing_claim { unsigned token; };
int backing_claim_prepare_inode(struct inode *inode, enum backing_claim_owner owner, struct backing_claim **out)
{ (void)inode; (void)owner; *out = calloc(1, sizeof(**out)); if (!*out) return ENOMEM; live_claims++; return 0; }
int backing_claim_finalize(struct backing_claim *claim, const struct backing_claim_extent *extents, unsigned count)
{ (void)claim; (void)extents; (void)count; return 0; }
void backing_claim_release(struct backing_claim *claim)
{ if (claim) { CHECK(live_claims); live_claims--; free(claim); } }
void file_ref(struct file *f) { refcount_get(&f->f_refs); }
int file_close(struct file *f) { CHECK(!refcount_put(&f->f_refs)); return 0; }
struct disk *disk_alloc(void) { return calloc(1, sizeof(struct disk)); }
int disk_create(struct disk *disk) { (void)disk; return 0; }
int disk_gone_if_idle(struct disk *disk) { (void)disk; return 0; }
int disk_destroy(struct disk *disk) { free(disk); return 0; }
int block_identity_get(struct disk *disk, struct block_identity *identity)
{ (void)disk; (void)identity; return ENODEV; }

void spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{ (void)rank; (void)name; memset(lock, 0, sizeof(*lock)); }
int hal_printf(const char *format, ...) { (void)format; return 0; }
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
int disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{ return buf_read(disk, block, count, data); }
int disk_write_filesystem(struct disk *disk, uint64_t block, uint32_t count, const void *data)
{ return buf_write(disk, block, count, data); }
int disk_write_direct(struct disk *disk, uint64_t block, uint32_t count, const void *data)
{ return memory_transfer(disk, block, count, (void *)data, 1); }
int disk_transfer_progress(struct disk *disk, enum bio_op op, uint64_t block,
    uint32_t count, void *data, uint32_t *completed)
{
	int error = memory_transfer(disk, block, count, data, op == BIO_WRITE);
	*completed = error == 0 ? count : 0;
	return error;
}
int disk_resolve_range(struct disk *disk, uint64_t block, uint32_t count,
    struct disk **leaf, uint64_t *mapped)
{ if (block > disk->d_block_count || count > disk->d_block_count - block) return EIO; *leaf = disk; *mapped = block; return 0; }
void disk_ref(struct disk *disk) { (void)disk; }
void disk_release(struct disk *disk) { (void)disk; }
int disk_buffer_acquire(struct disk *disk)
{ if(disk->d_media_revoked)return ENXIO;disk->d_buffer_refs++;return 0; }
void disk_buffer_release(struct disk *disk)
{ CHECK(disk->d_buffer_refs);disk->d_buffer_refs--; }
struct thread *thread_current(void) { return NULL; }
int hal_pmem_alloc(const struct hal_pmem_request *r, struct hal_pmem *m)
{ memset(m, 0, sizeof(*m)); m->size = r->size; m->vaddr = aligned_alloc(4096, r->size); return m->vaddr ? HAL_OK : HAL_ERR_NOMEM; }
int hal_pmem_free(struct hal_pmem *m) { free(m->vaddr); return HAL_OK; }
size_t hal_pmem_get_total_size(void) { return 64U * 1024U * 1024U; }
void waitq_init(struct wait_queue *q, const char *name) { (void)name; memset(q, 0, sizeof(*q)); }
uint64_t waitq_sequence(const struct wait_queue *q) { (void)q; return 0; }
void waitq_wake_all(struct wait_queue *q) { (void)q; }
int waitq_sleep(struct wait_queue *q, struct spinlock *lock, uint64_t seq, uint64_t ticks, unsigned flags)
{ (void)q; (void)lock; (void)seq; (void)flags; (void)ticks; abort(); }
ssize_t file_pread(struct file *f, void *b, size_t n, off_t o) { return f->f_ops->pread(f,b,n,o); }
ssize_t file_pwrite_internal(struct file *f, const void *b, size_t n, off_t o, unsigned flags)
{ CHECK(flags == (FILE_IO_LOOP_BACKING | FILE_IO_DRAIN)); return f->f_ops->pwrite(f,b,n,o); }
int file_fsync(struct file *f) { return f->f_ops->fsync(f); }
static int completion_error;
static size_t completion_bytes;
void bio_complete(struct bio *b, int error, size_t bytes)
{ (void)b; completion_error = error; completion_bytes = bytes; }

static void operation(struct loop_device *loop, unsigned op, uint64_t block,
    uint32_t count, void *data, int expected)
{
	struct bio bio = {0}; bio.b_op = op; bio.b_mapped_block = block;
	bio.b_block_count = count; bio.b_data = data;
	int error = loop_submit(loop->disk, &bio);
	CHECK((error != 0 ? error : completion_error) == expected);
	if (expected == 0 && op != BIO_FLUSH) CHECK(completion_bytes == count * 512U);
}

int main(void)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode, *neighbor;
	struct file file;
	struct extent_capture ext = {0};
	struct fat_loop_extent map[8], bad[8];
	unsigned char data[8192], actual[8192], adjacent[512];
	struct loop_device loop = {0};
	struct disk disk = {0};
	CHECK(buf_init() == 0);
	format_image(&image, ZEDBSD_FAT32, 1, 2);
	CHECK(host_mount(&image, 0, &mountp) == 0);
	memset(data, 0x31, sizeof(data)); memset(adjacent, 0x62, sizeof(adjacent));
	inode = create_payload(mountp.m_root, "image", data, 512);
	neighbor = create_payload(mountp.m_root, "neighbor", adjacent, 512);
	CHECK(host_file_open(inode, O_RDWR, &file) == 0);
	CHECK(file.f_ops->pwrite(&file, data + 512, sizeof(data) - 512, 512) == sizeof(data) - 512);
	CHECK(fat_file_extents(&file, capture_extent, &ext) == 0 && ext.used >= 2);
	for (unsigned i = 0; i < ext.used; i++)
		map[i] = (struct fat_loop_extent){ext.file_block[i], ext.disk_block[i], ext.count[i]};
	unsigned baseline_writes = image.writes;
	CHECK(file.f_ops->pwrite(&file, data, sizeof(data), 0) == sizeof(data));
	baseline_writes = image.writes - baseline_writes;
	/* The real claim authorization is covered by backing-claim fixtures and
	 * native boot. This fixture isolates physical cache/slot/map behavior. */
	file.f_backing_claim = (struct backing_claim *)&loop;
	CHECK(fat_file_set_loop_map(&file, map, ext.used) == 0);
	loop.attached = true; loop.flags = LOOP_READ_WRITE; loop.backing = &file;
	loop.disk = &disk; loop.size_bytes = sizeof(data); disk.d_data = &loop;
	memset(data, 0x84, sizeof(data));
	unsigned mapped_writes = image.writes;
	operation(&loop, BIO_WRITE, 0, 16, data, 0);
	mapped_writes = image.writes - mapped_writes;
	CHECK(mapped_writes < baseline_writes);
	printf("METRIC FAT fragmented 8192-byte overwrite: old=%u mapped=%u physical writes\n",
	    baseline_writes, mapped_writes);
	read_inode(inode, actual, sizeof(actual));
	CHECK(memcmp(data, actual, sizeof(data)) == 0);
	puts("S19 PASS mapped write followed by ordinary FAT alias read");
	operation(&loop, BIO_READ, 0, 16, actual, 0);
	CHECK(memcmp(data, actual, sizeof(data)) == 0);
	puts("S20 PASS fragmented extent boundary read/write");
	operation(&loop, BIO_READ, 15, 1, actual, 0);
	operation(&loop, BIO_READ, 16, 1, actual, EOVERFLOW);
	operation(&loop, BIO_READ, UINT64_MAX, 1, actual, EOVERFLOW);
	puts("S21 PASS exact end, overrun and multiplication overflow");
	memcpy(bad, map, sizeof(map)); bad[0].file_block = 1;
	CHECK(fat_file_set_loop_map(&file, bad, ext.used) == EIO);
	operation(&loop, BIO_READ, 0, 1, actual, 0);
	puts("S22 PASS attach map rejects logical hole and preserves previous map");
	memcpy(bad, map, sizeof(map)); bad[0].disk_block = image.disk.d_block_count;
	CHECK(fat_file_set_loop_map(&file, bad, ext.used) == EIO);
	puts("S23 PASS attach map rejects parent disk overrun");
	read_inode(neighbor, actual, 512); CHECK(memcmp(actual, adjacent, 512) == 0);
	/* Explicitly share one physical 4KiB cache line. */
	uint64_t line = map[1].disk_block & ~(uint64_t)7;
	unsigned char before[4096], after[4096], sector[512];
	CHECK(disk_read(&image.disk, line, 8, before) == 0);
	memset(sector, 0xc3, 512);
	CHECK(disk_write_filesystem(&image.disk, line + 1, 1, sector) == 0);
	CHECK(disk_read(&image.disk, line, 8, after) == 0);
	CHECK(memcmp(before, after, 512) == 0 && memcmp(before + 1024, after + 1024, 3072) == 0);
	puts("S24 PASS production buffer-cache shared-line RMW preserves neighbors");
	read_inode(inode, actual, 512);
	operation(&loop, BIO_WRITE, 0, 1, sector, 0);
	read_inode(inode, actual, 512); CHECK(memcmp(actual, sector, 512) == 0);
	puts("S25 PASS warmed FAT sector slot invalidated across mapped write");
	CHECK(fat_file_set_loop_map(&file, NULL, 0) == 0); file.f_backing_claim = NULL;
	CHECK(file.f_ops->pwrite(&file, data, 512, 0) == 512);
	file.f_backing_claim = (struct backing_claim *)&loop;
	CHECK(fat_file_set_loop_map(&file, map, ext.used) == 0);
	operation(&loop, BIO_READ, 0, 1, actual, 0); CHECK(memcmp(actual, data, 512) == 0);
	puts("S27 PASS map unbind modify bind and fresh content");
	image.fail_syncs = 1; operation(&loop, BIO_FLUSH, 0, 0, NULL, EIO);
	operation(&loop, BIO_READ, 0, 1, actual, 0); operation(&loop, BIO_FLUSH, 0, 0, NULL, 0);
	puts("S28 PASS loop flush error followed by readable and syncable backing");
	CHECK(fat_file_set_loop_map(&file, NULL, 0) == 0); file.f_backing_claim = NULL;
	CHECK(loop_init() == 0);
	struct disk *attached = NULL;
	off_t saved_size = inode->i_size;
	inode->i_size = (off_t)((UINT64_C(1) << 32) + 512U);
	CHECK(loop_attach_file(&file, LOOP_READ_WRITE, &attached) == EFBIG);
	CHECK(attached == NULL && live_claims == 0);
	inode->i_size = saved_size;
	extent_fault = 1;
	CHECK(loop_attach_file(&file, LOOP_READ_WRITE, &attached) == EIO);
	CHECK(attached == NULL && live_claims == 0);
	extent_fault = 0;
	for (unsigned i = 0; i < LOOP_MAX_DEVICES; i++) loops[i].reserved = true;
	CHECK(loop_attach_file(&file, LOOP_READ_WRITE, &attached) == ENOSPC);
	CHECK(attached == NULL && live_claims == 0);
	for (unsigned i = 0; i < LOOP_MAX_DEVICES; i++) loops[i].reserved = false;
	CHECK(loop_attach_file(&file, LOOP_READ_WRITE, &attached) == 0);
	CHECK(live_claims == 1 && attached != NULL);
	CHECK(loop_detach(attached) == 0 && live_claims == 0);
	CHECK(file.f_backing_claim == NULL && !(inode->i_flags & INODE_LOOPFILE));
	puts("S22 PASS actual attach rejects hole and full registry without leaked map/claim");
	puts("S27 PASS actual attach/detach clears borrowed map before release");
	CHECK(host_file_close(&file) == 0);
	inode_release(inode); inode_release(neighbor); host_unmount(&mountp);
	buf_reset();
	uint32_t last = (uint32_t)((map[ext.used - 1].disk_block +
	    map[ext.used - 1].count - 1 - image.data_start) /
	    image.sectors_per_cluster + 2);
	for (unsigned copy = 0; copy < image.fat_copies; copy++)
		set_one_fat_entry(&image, copy, last, 0);
	CHECK(host_mount(&image, 0, &mountp) == 0);
	CHECK(lookup_child(mountp.m_root, "image", &inode) == 0);
	CHECK(host_file_open(inode, O_RDWR, &file) == 0);
	unsigned writes = image.writes;
	CHECK(file.f_ops->pwrite(&file, data, 1, 0) == -EIO);
	CHECK(image.writes == writes);
	CHECK(host_file_close(&file) == 0); inode_release(inode); host_unmount(&mountp);
	buf_reset();
	for (unsigned copy = 0; copy < image.fat_copies; copy++)
		set_one_fat_entry(&image, copy, last, fat_end_of_chain(image.type));
	CHECK(host_mount(&image, 0, &mountp) == 0);
	CHECK(lookup_child(mountp.m_root, "image", &inode) == 0);
	CHECK(host_file_open(inode, O_RDWR, &file) == 0);
	CHECK(file.f_ops->pwrite(&file, data, 1, 0) == 1);
	CHECK(host_file_close(&file) == 0); inode_release(inode); host_unmount(&mountp);
	buf_reset(); destroy_image(&image);
	puts("S32 PASS full FAT write-chain validation rejects distant corruption before mutation");
	return 0;
}

/* Models the explicit lower backend drain without a VM layer in this fixture. */
int file_fsync_backend(struct file *file) { return file_fsync(file); }

ssize_t file_pwrite_context(struct file *file, const void *buffer, size_t length,
    off_t offset, unsigned flags, const struct ucred *credential,
    const struct io_context *context)
{
	(void)credential;
	CHECK(io_context_validate(context) == 0);
	CHECK((context->flags & IO_CONTEXT_DRAIN) != 0);
	CHECK(flags == (FILE_IO_LOOP_BACKING | FILE_IO_DRAIN));
	return file->f_ops->pwrite_internal(file, buffer, length, offset, flags,
	    credential, context);
}

int disk_write_filesystem_context(struct disk *disk, uint64_t block,
    uint32_t count, const void *data, const struct io_context *context)
{ return buf_write_context(disk, block, count, data, context); }
int disk_write_direct_context(struct disk *disk, uint64_t block,
    uint32_t count, const void *data, const struct io_context *context)
{
	CHECK(io_context_validate(context) == 0);
	return disk_write_direct(disk, block, count, data);
}
int disk_transfer_progress_context(struct disk *disk, enum bio_op op,
    uint64_t block, uint32_t count, void *data, uint32_t *completed,
    const struct io_context *context)
{
	CHECK(io_context_validate(context) == 0);
	return disk_transfer_progress(disk, op, block, count, data, completed);
}
