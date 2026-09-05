/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/*
 * WS011 p009: production FAT validation/write traversal cost and safety gate.
 * Disk storage and VFS locks are the maintained single-threaded WS018 mocks.
 * Count backend sector reads, not host elapsed time or target lock progress.
 */
#define main ws018_fixture_main
#define disk_read ws018_fixture_disk_read
#include "../../ws018-kernel-architecture/tests/fat-native-vfs-host-test.c"
#undef main
#undef disk_read

#define COST_FIRST_CLUSTER 16795U
#define COST_FILE_CLUSTERS 16384U
#define COST_SECTORS_PER_CLUSTER 4U
#define COST_FILE_BYTES (COST_FILE_CLUSTERS * COST_SECTORS_PER_CLUSTER * SECTOR_SIZE)

static int injected_read_error;
static unsigned injected_read_after;

int
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	if (injected_read_error != 0) {
		if (injected_read_after == 0U) {
			int error = injected_read_error;
			injected_read_error = 0;
			return error;
		}
		injected_read_after--;
	}
	return ws018_fixture_disk_read(disk, block, count, data);
}

static void
prepare_cost_image(struct memory_image *image)
{
	static const uint8_t name[11] = {
		'D', 'A', 'T', 'A', ' ', ' ', ' ', ' ', 'I', 'M', 'G'
	};
	uint8_t boot[SECTOR_SIZE], *root;
	uint32_t index;

	/* Match the preserved guest: FAT32, two FAT copies, 2KiB clusters,
	 * 32MiB DATA.IMG at first cluster 16795.  Keep enough data clusters for
	 * FAT32 classification while allocating only disposable host memory. */
	format_image(image, ZEDBSD_FAT32, 1U, 2U);
	memcpy(boot, image->bytes, sizeof(boot));
	free(image->bytes);
	image->sectors = 270000U;
	image->fat_sectors = 528U;
	image->sectors_per_cluster = COST_SECTORS_PER_CLUSTER;
	image->root_start = image->reserved + image->fat_sectors * image->fat_copies;
	image->data_start = image->root_start;
	image->cluster_count = (image->sectors - image->data_start) /
		image->sectors_per_cluster;
	image->bytes = calloc(image->sectors, SECTOR_SIZE);
	CHECK(image->bytes != NULL);
	boot[13] = COST_SECTORS_PER_CLUSTER;
	put32(boot + 32U, image->sectors);
	put32(boot + 36U, image->fat_sectors);
	memcpy(image->bytes, boot, sizeof(boot));
	image->disk.d_block_count = image->sectors;
	set_fat_entry(image, 0U, 0x0ffffff8U);
	set_fat_entry(image, 1U, fat_end_of_chain(ZEDBSD_FAT32));
	set_fat_entry(image, image->root_cluster, fat_end_of_chain(ZEDBSD_FAT32));
	CHECK(COST_FIRST_CLUSTER + COST_FILE_CLUSTERS <= image->cluster_count + 2U);
	for (index = 0; index < COST_FILE_CLUSTERS; index++)
		set_fat_entry(image, COST_FIRST_CLUSTER + index,
			index + 1U == COST_FILE_CLUSTERS ?
			fat_end_of_chain(ZEDBSD_FAT32) : COST_FIRST_CLUSTER + index + 1U);
	root = image_sector(image, cluster_lba(image, image->root_cluster));
	set_dirent(root, name, 0x20U, COST_FIRST_CLUSTER, COST_FILE_BYTES,
		ZEDBSD_FAT32);
	injected_read_error = 0;
	injected_read_after = 0U;
}

static void
open_cost_file(struct memory_image *image, struct mount *mountp,
	struct inode **inode, struct file *file)
{
	CHECK_ERROR(host_mount(image, 0U, mountp), 0);
	CHECK_ERROR(lookup_child(mountp->m_root, "DATA.IMG", inode), 0);
	CHECK((*inode)->i_size == COST_FILE_BYTES);
	CHECK_ERROR(host_file_open(*inode, O_RDWR, file), 0);
}

static void
close_cost_file(struct memory_image *image, struct mount *mountp,
	struct inode *inode, struct file *file)
{
	CHECK_ERROR(host_file_close(file), 0);
	inode_release(inode);
	host_check_idle_mount(mountp);
	host_unmount(mountp);
	destroy_image(image);
}

static void
check_write_cost(uint32_t offset, size_t length, unsigned maximum_reads)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct file file;
	uint8_t payload[8193], *data;
	unsigned before, reads;
	uint32_t index;

	current_type = ZEDBSD_FAT32;
	current_stage = "32MiB fixed-size backing write cost";
	prepare_cost_image(&image);
	open_cost_file(&image, &mountp, &inode, &file);
	CHECK(length <= sizeof(payload));
	CHECK(offset > 0U && offset + length <= COST_FILE_BYTES);
	memset(payload, 0x6d, sizeof(payload));
	before = image.reads;
	CHECK(file.f_ops->pwrite(&file, payload, length, offset) ==
		(ssize_t)length);
	reads = image.reads - before;
	printf("NCOM FAT32 write cost: size=%u offset=%u bytes=%zu "
		"sector_reads=%u limit=%u\n", COST_FILE_BYTES, offset,
		length, reads, maximum_reads);
	fflush(stdout);
	if (getenv("NCOM_FAT_COST_REPORT_ONLY") == NULL)
		CHECK(reads <= maximum_reads);
	CHECK(inode->i_size == COST_FILE_BYTES);
	data = image_sector(&image, cluster_lba(&image, COST_FIRST_CLUSTER));
	CHECK(memcmp(data + offset, payload, length) == 0);
	CHECK(data[offset - 1U] == 0U);
	if (offset + length < COST_FILE_BYTES)
		CHECK(data[offset + length] == 0U);
	for (index = 0; index < COST_FILE_CLUSTERS; index++)
		CHECK(get_fat_entry(&image, 0U, COST_FIRST_CLUSTER + index) ==
			(index + 1U == COST_FILE_CLUSTERS ?
			fat_end_of_chain(ZEDBSD_FAT32) : COST_FIRST_CLUSTER + index + 1U));
	check_fat_copies(&image);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK(image.syncs != 0U);
	close_cost_file(&image, &mountp, inode, &file);
}

static void
check_growth_cost(uint32_t gap, size_t length, unsigned maximum_reads)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct file file;
	uint8_t payload[8193], result[16384], *data;
	unsigned before, reads;
	size_t index, result_length = 16U + gap + length;

	current_type = ZEDBSD_FAT32;
	current_stage = "32MiB backing growth cost";
	prepare_cost_image(&image);
	CHECK(length <= sizeof(payload));
	CHECK(result_length <= sizeof(result));
	data = image_sector(&image, cluster_lba(&image, COST_FIRST_CLUSTER));
	memset(data + COST_FILE_BYTES - 16U, 0x3c, 16U);
	open_cost_file(&image, &mountp, &inode, &file);
	memset(payload, 0x6d, sizeof(payload));
	before = image.reads;
	CHECK(file.f_ops->pwrite(&file, payload, length,
		COST_FILE_BYTES + gap) == (ssize_t)length);
	reads = image.reads - before;
	printf("NCOM FAT32 growth cost: size=%u gap=%u bytes=%zu "
		"sector_reads=%u limit=%u\n", COST_FILE_BYTES, gap, length,
		reads, maximum_reads);
	fflush(stdout);
	if (getenv("NCOM_FAT_COST_REPORT_ONLY") == NULL)
		CHECK(reads <= maximum_reads);
	CHECK(inode->i_size == (off_t)(COST_FILE_BYTES + gap + length));
	/* Growth may allocate noncontiguous clusters: verify through production
	 * pread, outside the measured write interval, instead of assuming layout. */
	CHECK(file.f_ops->pread(&file, result, result_length,
		COST_FILE_BYTES - 16U) == (ssize_t)result_length);
	for (index = 0; index < 16U; index++)
		CHECK(result[index] == 0x3c);
	for (; index < 16U + gap; index++)
		CHECK(result[index] == 0U);
	CHECK(memcmp(result + 16U + gap, payload, length) == 0);
	for (index = 0; index + 1U < COST_FILE_CLUSTERS; index++)
		CHECK(get_fat_entry(&image, 0U, COST_FIRST_CLUSTER + index) ==
			COST_FIRST_CLUSTER + index + 1U);
	check_fat_copies(&image);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	CHECK(image.syncs != 0U);
	close_cost_file(&image, &mountp, inode, &file);
}

static void
check_rejected_chain(const char *name, uint32_t bad_index,
	uint32_t bad_value, int read_error, unsigned read_after)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct file file;
	unsigned writes, reads;

	current_stage = name;
	prepare_cost_image(&image);
	if (bad_index < COST_FILE_CLUSTERS)
		set_fat_entry(&image, COST_FIRST_CLUSTER + bad_index, bad_value);
	open_cost_file(&image, &mountp, &inode, &file);
	writes = image.write_attempts;
	reads = image.reads;
	injected_read_error = read_error;
	injected_read_after = read_after;
	CHECK(file.f_ops->pwrite(&file, "X", 1U, 0) ==
		-(read_error != 0 ? read_error : EIO));
	CHECK(image.write_attempts == writes);
	CHECK(inode->i_size == COST_FILE_BYTES);
	CHECK(*image_sector(&image, cluster_lba(&image, COST_FIRST_CLUSTER)) == 0U);
	printf("NCOM FAT32 validation: %s PASS (sector_reads=%u)\n",
		name, image.reads - reads);
	close_cost_file(&image, &mountp, inode, &file);
}

int
main(void)
{
	/* 129 FAT sectors for full validation, also capturing the initial seek,
	 * plus bounded data/allocation overhead. Floyd's distant cursors, a
	 * separate initial seek, and per-sector restarts exceed these budgets. */
	check_write_cost(8192U, 4096U, 160U);
	check_write_cost(COST_FILE_BYTES - 4096U, 4096U, 160U);
	check_write_cost(511U, 8193U, 170U);
	check_growth_cost(0U, 4096U, 180U);
	check_growth_cost(4099U, 8193U, 240U);
	check_rejected_chain("self cycle", 0U, COST_FIRST_CLUSTER, 0, 0U);
	check_rejected_chain("prefix then cycle", 511U, COST_FIRST_CLUSTER + 128U,
		0, 0U);
	check_rejected_chain("full-chain cycle", COST_FILE_CLUSTERS - 1U,
		COST_FIRST_CLUSTER, 0, 0U);
	check_rejected_chain("free link", 511U, 0U, 0, 0U);
	check_rejected_chain("reserved link", 511U, 1U, 0, 0U);
	check_rejected_chain("bad cluster", 511U, 0x0ffffff7U, 0, 0U);
	check_rejected_chain("out-of-volume link", 511U, 100000U, 0, 0U);
	check_rejected_chain("corrupt tail beyond write target",
		COST_FILE_CLUSTERS - 1U, 0U, 0, 0U);
	check_rejected_chain("first read error", COST_FILE_CLUSTERS, 0U,
		ETIMEDOUT, 0U);
	check_rejected_chain("later read error", COST_FILE_CLUSTERS, 0U,
		ENXIO, 3U);
	check_rejected_chain("tail read error beyond write target",
		COST_FILE_CLUSTERS, 0U, ENXIO, 128U);
	CHECK(inode_allocations == inode_destructions);
	CHECK(inode_free_attempts == inode_destructions);
	printf("WS011 FAT write traversal: PASS (%u checks; %s)\n", checks,
		getenv("NCOM_FAT_COST_REPORT_ONLY") == NULL ?
		"cost bounds enforced" : "report-only baseline, cost bounds not enforced");
	return 0;
}
