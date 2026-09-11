/* Production UFS private operations; deterministic physical error injection.
 * SPDX-License-Identifier: Zlib */
#define UFS_AUDIT_VERSION 2
#define UFS_AUDIT_CUSTOM_IO
#define main ufs_regression_main
#include "ufs-metadata-audit.c"
#undef main

/* Host IRQ state and both disk APIs share the existing fault-injected medium. */
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) { }
int disk_write_filesystem_context(struct disk *disk, uint64_t block,
    uint32_t count, const void *data, const struct io_context *context)
{ return disk_write_context(disk, block, count, data, context); }

int main(void)
{
	AUDIT_STATE fs; AUDIT_INODE node;
	struct mount mountp; struct disk disk;
	unsigned char block[4096];
	storage_fixture(&fs, &node, &mountp, &disk, 0, 0);
	storage_data_reads = 0; memset(block, 0x37, sizeof(block));
	REQUIRE(pwrite_inode(&node.inode, block, sizeof(block), 0) == sizeof(block));
	REQUIRE(storage_data_reads == 0);
	REQUIRE(memcmp(storage + 160 * 512, block, sizeof(block)) == 0);
	puts("S29 PASS existing full-block overwrite has no data read");
	memset(block + 101, 0x65, 777);
	REQUIRE(pwrite_inode(&node.inode, block + 101, 777, 101) == 777);
	REQUIRE(storage_data_reads == 1);
	REQUIRE(memcmp(storage + 160 * 512, block, sizeof(block)) == 0);
	puts("S30 PASS partial overwrite reads and preserves surrounding bytes");
	free(fs.cg);
	storage_fixture(&fs, &node, &mountp, &disk, 0, 1);
	memset(storage + 160 * 512, 0xda, 4096);
	REQUIRE(pwrite_inode(&node.inode, block + 101, 777, 101) == 777);
	for (unsigned i = 0; i < 4096; i++)
		REQUIRE(storage[160 * 512 + i] == (i >= 101 && i < 878 ? 0x65 : 0));
	puts("S31 PASS newly allocated partial block initializes all reachable bytes");
	free(fs.cg);
	storage_fixture(&fs, &node, &mountp, &disk, 0, 1);
	failure_write = 4; failure_write_again = 5; commit_error = 1;
	uint64_t fragment = 0;
	REQUIRE(bmap_ensure(&node.inode, 0, &fragment) == EIO);
	REQUIRE(!fs.writable);
	failure_write = failure_write_again = failure_sync = 0;
	REQUIRE(disk_sync(&disk) == 0);
	REQUIRE(pwrite_inode(&node.inode, block, 1, 0) == -EROFS && !fs.writable);
	REQUIRE(!node.inode.i_lock.locked && !fs.lock.locked);
	puts("S33 PASS failed metadata rollback stays readonly after successful flush");
	free(fs.cg);
	return 0;
}
