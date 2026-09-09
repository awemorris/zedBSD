/* Production UFS mappings over an independently laid out memory medium. */
#define UFS_AUDIT_CURRENT_DRIVER
#define UFS_AUDIT_CUSTOM_IO
#define main prior_metadata_main
#include "../../ws024-unified-ufs/tests/ufs-metadata-host.c"
#undef main

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }

/* Both disk entry points target the same memory medium in this provider test. */
int
disk_write_filesystem_context(struct disk *disk, uint64_t block, uint32_t count,
    const void *data, const struct io_context *context)
{
	return disk_write_context(disk, block, count, data, context);
}

struct extent_capture {
	uint64_t logical[32];
	uint64_t physical[32];
	uint32_t count[32];
	unsigned used;
	int error;
};

static int
capture(uint64_t logical, uint64_t physical, uint32_t count, void *argument)
{
	struct extent_capture *result;
	unsigned at;

	result = argument;
	if (result->error != 0)
		return result->error;
	at = result->used++;
	REQUIRE(at < 32);
	result->logical[at] = logical;
	result->physical[at] = physical;
	result->count[at] = count;
	return 0;
}

static int
capture_metadata(uint64_t physical, uint32_t count, void *argument)
{
	return capture(0, physical, count, argument);
}

int
main(void)
{
	AUDIT_STATE fs;
	AUDIT_INODE node;
	struct mount mountp;
	struct disk disk;
	struct disk *identity_disk;
	uint64_t identity_object;
	struct file file;
	struct filesystem_type type;
	struct extent_capture result;
	unsigned n;
	uint64_t mapped;

	storage_fixture(&fs, &node, &mountp, &disk, 0, 0);
	fs.super.fsize = 512;
	disk.d_block_size = 512;
	disk.d_block_count = 512;
	memset(&type, 0, sizeof(type));
	type.file_extents = ufs_file_extents;
	type.file_backing_identity = ufs_backing_identity;
	mountp.m_type = &type;
	REQUIRE(ufs_backing_identity(&node.inode, &identity_disk, &identity_object) == 0);
	REQUIRE(identity_disk == &disk && identity_object == node.inode.i_ino);
	fs.snapshot.active = 1;
	REQUIRE(ufs_backing_identity(&node.inode, &identity_disk, &identity_object) == 0);
	fs.snapshot.active = 0;
	disk.d_flags = DISK_FILE_BACKED;
	REQUIRE(ufs_backing_identity(&node.inode, &identity_disk, &identity_object) == EOPNOTSUPP);
	disk.d_flags = 0;
	memset(&file, 0, sizeof(file));
	file.f_inode = &node.inode;
	for (n = 0; n < 12; n++)
		node.direct[n] = 160 + n * 8;
	node.indirect[0] = 320;
	AUDIT_PUTPTR(storage + 320 * 512, 0, 256, 0);
	memset(&result, 0, sizeof(result));
	REQUIRE(ufs_file_extents(&file, capture, &result) == 0);
	REQUIRE(result.used == 1 && result.logical[0] == 0 &&
	    result.physical[0] == 160 && result.count[0] == 104);
	REQUIRE(storage_writes == 0 && storage_syncs == 0);

	/* A fragmented final block reports only the file's final complete sector. */
	AUDIT_PUTPTR(storage + 320 * 512, 0, 280, 0);
	node.inode.i_size = 12 * 4096 + 512;
	memset(&result, 0, sizeof(result));
	REQUIRE(ufs_file_extents(&file, capture, &result) == 0);
	REQUIRE(result.used == 2 && result.count[0] == 96 &&
	    result.logical[1] == 96 && result.physical[1] == 280 && result.count[1] == 1);
	result.error = ENOSPC;
	REQUIRE(ufs_file_extents(&file, capture, &result) == ENOSPC);
	result.error = 0;
	for (n = 0; n < 4; n++) {
		node.direct[0] = n == 0 ? 0 : n == 1 ? 32 : n == 2 ? 513 : 161;
		REQUIRE(ufs_file_extents(&file, capture, &result) == EIO);
	}
	node.direct[0] = 160;
	fs.super.csaddr = 160;
	fs.super.cssize = 512;
	REQUIRE(ufs_file_extents(&file, capture, &result) == EIO);
	fs.super.cssize = 0;
	bit_set(storage + 32 * 512 + 264, 163);
	fs.cg_valid = 0;
	REQUIRE(ufs_file_extents(&file, capture, &result) == EIO);
	bit_clear(storage + 32 * 512 + 264, 163);
	fs.cg_valid = 0;
	node.indirect[0] = 32;
	REQUIRE(ufs_file_extents(&file, capture, &result) == EIO);
	node.indirect[0] = 320;
	fs.snapshot.active = 1;
	REQUIRE(ufs_file_extents(&file, capture, &result) == EBUSY);
	fs.snapshot.active = 0;
	node.inode.i_size++;
	REQUIRE(ufs_file_extents(&file, capture, &result) == EINVAL);
	node.inode.i_size--;

	/* Follow independently placed double and triple indirect blocks. */
	node.indirect[1] = 328;
	node.indirect[2] = 336;
	AUDIT_PUTPTR(storage + 328 * 512, 0, 320, 0);
	AUDIT_PUTPTR(storage + 336 * 512, 0, 328, 0);
	REQUIRE(ufs_backing_map(&node.inode, 12 + 512, &mapped) == 0 && mapped == 280);
	REQUIRE(ufs_backing_map(&node.inode, 12 + 512 + 512 * 512, &mapped) == 0 && mapped == 280);
	REQUIRE(!fs.lock.locked && storage_writes == 0);
	failure_read = storage_reads + 1;
	fs.cg_valid = 0;
	REQUIRE(ufs_file_extents(&file, capture, &result) == EIO);
	REQUIRE(!fs.lock.locked);
	failure_read = 0;

	/* Native formatter geometry uses 1-KiB fragments and 8-KiB blocks. */
	free(fs.cg);
	fs.cg = calloc(1, 8192);
	REQUIRE(fs.cg != NULL);
	fs.cg_valid = 0;
	fs.super.fsize = 1024;
	fs.super.bsize = 8192;
	fs.super.frag = 8;
	fs.super.fsbtodb = 1;
	fs.super.size = fs.super.fpg = 256;
	fs.super.nindir = 1024;
	fs.super.cgsize = 8192;
	fs.super.cblkno = 16;
	fs.super.dblkno = 80;
	AUDIT_PUT32(storage + 32 * 512, AUDIT_CG_NDBLK, 256, 0);
	for (n = 0; n < 12; n++)
		node.direct[n] = 80 + n * 8;
	node.indirect[0] = 200;
	AUDIT_PUTPTR(storage + 400 * 512, 0, 176, 0);
	node.inode.i_size = 13 * 8192;
	memset(&result, 0, sizeof(result));
	REQUIRE(ufs_file_extents(&file, capture, &result) == 0);
	REQUIRE(result.used == 1 && result.physical[0] == 160 && result.count[0] == 208);

	memset(&result, 0, sizeof(result));
	REQUIRE(ufs_file_metadata_extents(&file, capture_metadata, &result) == 0);
	REQUIRE(result.used == 1 && result.physical[0] == 400 && result.count[0] == 16);
	node.inode.i_size = 12 * 8192;
	node.indirect[0] = 0;
	memset(&result, 0, sizeof(result));
	REQUIRE(ufs_file_metadata_extents(&file, capture_metadata, &result) == 0 && result.used == 0);
	node.inode.i_size++;
	REQUIRE(ufs_file_metadata_extents(&file, capture_metadata, &result) == EINVAL);
	node.inode.i_size += 511;
	REQUIRE(ufs_file_metadata_extents(&file, capture_metadata, &result) == EIO);
	REQUIRE(!fs.lock.locked);
	node.indirect[0] = 200;
	result.error = ENOSPC;
	REQUIRE(ufs_file_metadata_extents(&file, capture_metadata, &result) == ENOSPC);
	REQUIRE(!fs.lock.locked);
	memset(&result, 0, sizeof(result));
	AUDIT_PUTPTR(storage + 416 * 512, 0, 200, 0);
	AUDIT_PUTPTR(storage + 432 * 512, 0, 208, 0);
	/* Invalid second children lie beyond the one-block EOF and stay unread. */
	AUDIT_PUTPTR(storage + 416 * 512, 8, 32, 0);
	AUDIT_PUTPTR(storage + 432 * 512, 8, 32, 0);
	mutex_lock(&fs.lock);
	REQUIRE(ufs_metadata_tree(&mountp, 208, 2, 1, capture_metadata, &result) == 0);
	REQUIRE(result.used == 2 && result.physical[0] == 416 && result.physical[1] == 400);
	memset(&result, 0, sizeof(result));
	REQUIRE(ufs_metadata_tree(&mountp, 216, 3, 1, capture_metadata, &result) == 0);
	REQUIRE(result.used == 3 && result.physical[0] == 432 &&
	    result.physical[1] == 416 && result.physical[2] == 400);
	memset(&result, 0, sizeof(result));
	REQUIRE(ufs_metadata_tree(&mountp, 208, 2, 1025, capture_metadata, &result) == EIO);
	mutex_unlock(&fs.lock);
	fs.snapshot.active = 1;
	REQUIRE(ufs_file_metadata_extents(&file, capture_metadata, &result) == EBUSY);
	REQUIRE(!fs.lock.locked && storage_writes == 0);
	fs.snapshot.active = 0;
	fs.cg_valid = 0;
	failure_read = storage_reads + 1;
	REQUIRE(ufs_file_metadata_extents(&file, capture_metadata, &result) == EIO);
	REQUIRE(!fs.lock.locked);
	free(fs.cg);
	printf("UFS file extents PASS (%u checks)\n", functional_checks);
	return 0;
}
