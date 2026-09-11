/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define main baseline_existing_audit_main
#include "../../ws018/tests/ufs-metadata-audit.c"
#undef main

#define CONTENT_READ IO_UFS_CONTENT_READ
#define CONTENT_WRITE IO_UFS_CONTENT_WRITE
#define ALL_READ IO_UFS_READ
#define ALL_WRITE IO_UFS_WRITE

int main(void)
{
	AUDIT_STATE fs;
	AUDIT_INODE node;
	struct mount mountp;
	struct disk disk;
	struct io_stats before, after;
	unsigned char block[4096];

	storage_fixture(&fs, &node, &mountp, &disk, 0, 0);
	disk.d_block_size = 512;
	memset(block, 0x37, sizeof(block));
	io_stats_snapshot(&before);
	REQUIRE(pwrite_inode(&node.inode, block, sizeof(block), 0) == sizeof(block));
	io_stats_snapshot(&after);
	REQUIRE(after.events[CONTENT_WRITE].calls - before.events[CONTENT_WRITE].calls == 1);
	REQUIRE(after.events[CONTENT_WRITE].bytes - before.events[CONTENT_WRITE].bytes == 4096);
	REQUIRE(after.events[ALL_WRITE].calls - before.events[ALL_WRITE].calls == 2);
	REQUIRE(after.events[ALL_WRITE].bytes - before.events[ALL_WRITE].bytes == 8192);
	REQUIRE(after.events[CONTENT_READ].calls - before.events[CONTENT_READ].calls ==
	    0U);

	before = after;
	REQUIRE(pwrite_inode(&node.inode, block + 101, 777, 101) == 777);
	io_stats_snapshot(&after);
	REQUIRE(after.events[CONTENT_READ].calls - before.events[CONTENT_READ].calls == 1);
	REQUIRE(after.events[ALL_READ].calls - before.events[ALL_READ].calls == 2);
	REQUIRE(after.events[ALL_READ].bytes - before.events[ALL_READ].bytes == 8192);

	/* An attempted failed write counts once without counting inode publication. */
	before = after;
	failure_write = storage_writes + 1;
	REQUIRE(pwrite_inode(&node.inode, block, sizeof(block), 0) == -EIO);
	io_stats_snapshot(&after);
	REQUIRE(after.events[CONTENT_WRITE].calls - before.events[CONTENT_WRITE].calls == 1);
	REQUIRE(after.events[ALL_WRITE].calls - before.events[ALL_WRITE].calls == 1);
	free(fs.cg);
	printf("IO-UFS%u PASS: data/metadata, full/partial overwrite, failed attempt\n",
	    UFS_AUDIT_VERSION);
	return 0;
}
