/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/*
 * WS011 p009: operation-local FAT write cursor boundary/rollback regression.
 * Reuse the maintained WS018 disk and single-threaded VFS lock mocks.  This
 * tests production FAT code, not overlay atomicity or target lock scheduling.
 */
#define main ws018_fixture_main
#include "../../ws018-kernel-architecture/tests/fat-native-vfs-host-test.c"
#undef main

static const uint8_t cursor_sfn[11] = {
	'C', 'U', 'R', 'S', 'O', 'R', ' ', ' ', 'B', 'I', 'N'
};

static uint8_t *
cursor_dirent(struct memory_image *image)
{
	return image_sector(image, image->type == ZEDBSD_FAT32 ?
		cluster_lba(image, image->root_cluster) : image->root_start);
}

static void
prepare_cursor_file(struct memory_image *image, enum bootfat_type type,
	unsigned scale, const uint32_t *chain, unsigned count)
{
	uint32_t cluster_bytes;
	unsigned index;

	format_image(image, type, scale, 2U);
	cluster_bytes = (uint32_t)image->sectors_per_cluster * SECTOR_SIZE;
	for (index = 0; index < count; index++) {
		set_fat_entry(image, chain[index], index + 1U < count ?
			chain[index + 1U] : fat_end_of_chain(type));
		memset(image_sector(image, cluster_lba(image, chain[index])),
			0x35, cluster_bytes);
	}
	/* Replace only the fixture's HELLO directory entry.  Its old allocated
	 * clusters remain fixture-owned, outside the cursor file under test. */
	set_dirent(cursor_dirent(image), cursor_sfn, 0x20U, chain[0],
		count * cluster_bytes, type);
}

static void
open_cursor_file(struct memory_image *image, struct mount *mountp,
	struct inode **inode, struct file *file)
{
	CHECK_ERROR(host_mount(image, 0U, mountp), 0);
	CHECK_ERROR(lookup_child(mountp->m_root, "CURSOR.BIN", inode), 0);
	CHECK_ERROR(host_file_open(*inode, O_RDWR, file), 0);
}

static void
close_cursor_file(struct mount *mountp, struct inode *inode, struct file *file)
{
	CHECK_ERROR(host_file_close(file), 0);
	inode_release(inode);
	host_check_idle_mount(mountp);
	host_unmount(mountp);
}

static void
check_cursor_contents(struct inode *inode, struct file *file,
	const uint8_t *expected, size_t length)
{
	uint8_t *actual = malloc(length);

	CHECK(actual != NULL);
	CHECK(inode->i_size == (off_t)length);
	CHECK(file->f_ops->pread(file, actual, length, 0) == (ssize_t)length);
	CHECK(memcmp(actual, expected, length) == 0);
	CHECK(file->f_ops->pread(file, actual, 1U, (off_t)length) == 0);
	free(actual);
}

static void
check_remounted_contents(struct memory_image *image,
	const uint8_t *expected, size_t length, uint64_t free_blocks)
{
	struct mount mountp;
	struct inode *inode;
	struct file file;

	open_cursor_file(image, &mountp, &inode, &file);
	check_cursor_contents(inode, &file, expected, length);
	CHECK(mount_free_blocks(&mountp) == free_blocks);
	close_cursor_file(&mountp, inode, &file);
	check_fat_copies(image);
}

static void
check_fragmented_unaligned(enum bootfat_type type, unsigned scale)
{
	static const uint32_t chain[] = {10U, 20U, 12U, 24U};
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct file file;

	current_type = type;
	current_stage = "fragmented unaligned multi-cluster write";
	prepare_cursor_file(&image, type, scale, chain, ARRAY_COUNT(chain));
	uint32_t cluster_bytes = (uint32_t)image.sectors_per_cluster * SECTOR_SIZE;
	size_t file_bytes = ARRAY_COUNT(chain) * cluster_bytes;
	size_t length = 2U * cluster_bytes + 3U;
	uint8_t *expected = malloc(file_bytes);
	uint8_t *payload = malloc(length);
	CHECK(expected != NULL && payload != NULL);
	memset(expected, 0x35, file_bytes);
	for (size_t index = 0; index < length; index++)
		payload[index] = (uint8_t)(index * 37U + 11U);
	memcpy(expected + 511U, payload, length);
	open_cursor_file(&image, &mountp, &inode, &file);
	uint64_t free_before = mount_free_blocks(&mountp);
	CHECK(file.f_ops->pwrite(&file, payload, length, 511U) == (ssize_t)length);
	check_cursor_contents(inode, &file, expected, file_bytes);
	for (unsigned index = 0; index < ARRAY_COUNT(chain); index++) {
		CHECK(get_fat_entry(&image, 0U, chain[index]) ==
			(index + 1U < ARRAY_COUNT(chain) ? chain[index + 1U] :
			fat_end_of_chain(type)));
		CHECK(memcmp(image_sector(&image, cluster_lba(&image, chain[index])),
			expected + index * cluster_bytes, cluster_bytes) == 0);
	}
	CHECK(mount_free_blocks(&mountp) == free_before);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	close_cursor_file(&mountp, inode, &file);
	check_remounted_contents(&image, expected, file_bytes, free_before);
	printf("NCOM FAT%u cursor: fragmented offset511, logical-sector=%u PASS\n",
		(unsigned)type, image.logical_sector_size);
	free(payload);
	free(expected);
	destroy_image(&image);
}

static void
check_sparse_handoff(enum bootfat_type type, unsigned scale, unsigned extra)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct file file;
	uint8_t initial[513];

	current_type = type;
	current_stage = "sparse zero-fill to payload cursor handoff";
	format_image(&image, type, scale, 2U);
	uint32_t cluster_bytes = (uint32_t)image.sectors_per_cluster * SECTOR_SIZE;
	size_t offset = 4U * cluster_bytes + extra;
	size_t length = 2U * cluster_bytes + 7U;
	size_t end = offset + length;
	uint8_t *expected = calloc(end, 1U);
	uint8_t *payload = malloc(length);
	CHECK(expected != NULL && payload != NULL);
	memset(initial, 0x35, sizeof(initial));
	memset(payload, 0x6d, length);
	memcpy(expected, initial, sizeof(initial));
	memcpy(expected + offset, payload, length);
	CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
	inode = create_payload(mountp.m_root, "CURSOR.BIN", initial, sizeof(initial));
	CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
	uint64_t free_before = mount_free_blocks(&mountp);
	uint64_t added = (end + cluster_bytes - 1U) / cluster_bytes -
		(sizeof(initial) + cluster_bytes - 1U) / cluster_bytes;
	CHECK(file.f_ops->pwrite(&file, payload, length, (off_t)offset) ==
		(ssize_t)length);
	check_cursor_contents(inode, &file, expected, end);
	CHECK(mount_free_blocks(&mountp) == free_before - added);
	CHECK_ERROR(file.f_ops->fsync(&file), 0);
	close_cursor_file(&mountp, inode, &file);
	check_remounted_contents(&image, expected, end, free_before - added);
	printf("NCOM FAT%u cursor: sparse handoff +%u, logical-sector=%u PASS\n",
		(unsigned)type, extra, image.logical_sector_size);
	free(payload);
	free(expected);
	destroy_image(&image);
}

static void
check_in_loop_growth_faults(enum bootfat_type type, unsigned scale)
{
	static const uint32_t chain[] = {10U};
	unsigned successful_writes = 0U;

	/* Ordinal zero measures this geometry's successful write sequence.  All
	 * later ordinals inject each of those writes in a fresh identical image:
	 * old-tail payload, zeroing, both EOC/link copies, and later payloads.
	 * Three new clusters force allocations inside one write_bytes loop. */
	for (unsigned ordinal = 0U; ordinal <= successful_writes; ordinal++) {
		struct memory_image image;
		struct mount mountp;
		struct inode *inode;
		struct file file;
		uint8_t directory_before[32];

		current_type = type;
		current_stage = "in-loop multi-cluster growth write failure/retry";
		current_fault_ordinal = ordinal;
		prepare_cursor_file(&image, type, scale, chain, ARRAY_COUNT(chain));
		uint32_t cluster_bytes = (uint32_t)image.sectors_per_cluster * SECTOR_SIZE;
		size_t offset = cluster_bytes - 17U;
		size_t length = 2U * cluster_bytes + 19U;
		size_t end = offset + length;
		size_t fat_bytes = (size_t)image.fat_sectors * image.fat_copies * SECTOR_SIZE;
		uint8_t *fat_before = malloc(fat_bytes);
		uint8_t *expected = malloc(end);
		uint8_t *payload = malloc(length);
		CHECK(fat_before != NULL && expected != NULL && payload != NULL);
		memset(payload, 0x6d, length);
		memset(expected, 0x35, offset);
		memcpy(expected + offset, payload, length);
		memcpy(fat_before, image_sector(&image, image.reserved), fat_bytes);
		memcpy(directory_before, cursor_dirent(&image), sizeof(directory_before));
		open_cursor_file(&image, &mountp, &inode, &file);
		uint64_t free_before = mount_free_blocks(&mountp);
		unsigned before = image.write_attempts;
		if (ordinal != 0U) {
			schedule_write_failure(&image, ordinal);
			CHECK(file.f_ops->pwrite(&file, payload, length, (off_t)offset) == -EIO);
			CHECK(image.fail_write_attempt == 0U);
			CHECK(inode->i_size == (off_t)cluster_bytes);
			CHECK(memcmp(image_sector(&image, image.reserved), fat_before,
				fat_bytes) == 0);
			CHECK(memcmp(cursor_dirent(&image), directory_before,
				sizeof(directory_before)) == 0);
			CHECK(mount_free_blocks(&mountp) == free_before);
			/* Existing bytes already written before the failure need not be
			 * restored: this API rolls back growth, not old payload changes. */
			for (size_t index = 0; index < offset; index++)
				CHECK(image_sector(&image, cluster_lba(&image, chain[0]))[index] == 0x35U);
			check_fat_copies(&image);
		}
		CHECK(file.f_ops->pwrite(&file, payload, length, (off_t)offset) ==
			(ssize_t)length);
		if (ordinal == 0U) {
			successful_writes = image.write_attempts - before;
			CHECK(successful_writes > 6U && successful_writes < 128U);
		}
		check_cursor_contents(inode, &file, expected, end);
		CHECK(mount_free_blocks(&mountp) == free_before - 3U);
		CHECK_ERROR(file.f_ops->fsync(&file), 0);
		close_cursor_file(&mountp, inode, &file);
		check_remounted_contents(&image, expected, end, free_before - 3U);
		free(payload);
		free(expected);
		free(fat_before);
		destroy_image(&image);
	}
	printf("NCOM FAT%u cursor: %u in-loop growth write faults + retries, "
		"logical-sector=%u PASS\n", (unsigned)type, successful_writes,
		scale * SECTOR_SIZE);
	current_fault_ordinal = 0U;
}

int
main(void)
{
	static const struct {
		enum bootfat_type type;
		unsigned scale;
	} variants[] = {
		{ZEDBSD_FAT12, 1U}, {ZEDBSD_FAT12, 2U},
		{ZEDBSD_FAT16, 1U}, {ZEDBSD_FAT32, 1U}
	};

	for (unsigned index = 0; index < ARRAY_COUNT(variants); index++) {
		check_fragmented_unaligned(variants[index].type, variants[index].scale);
		check_sparse_handoff(variants[index].type, variants[index].scale, 0U);
		check_sparse_handoff(variants[index].type, variants[index].scale, 3U);
	}
	check_in_loop_growth_faults(ZEDBSD_FAT16, 1U);
	check_in_loop_growth_faults(ZEDBSD_FAT12, 2U);
	CHECK(inode_allocations == inode_destructions);
	CHECK(inode_free_attempts == inode_destructions);
	for (unsigned index = 0; index < HOST_INODE_MAX; index++)
		CHECK(host_inode_cache[index] == NULL);
	printf("WS011 FAT write cursor: PASS (%u checks; fragmented boundaries, "
		"sparse handoffs, exhaustive in-loop write faults)\n", checks);
	return 0;
}
