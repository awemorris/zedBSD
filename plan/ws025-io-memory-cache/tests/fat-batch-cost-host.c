/* Measure real FAT create/append/unlink before and after metadata batching. */
#define main retained_fat_main
#include "../../ws018-kernel-architecture/tests/fat-native-vfs-host-test.c"
#undef main

int main(void)
{
	struct memory_image image;
	struct mount mountp;
	struct inode *inode;
	struct file file;
	unsigned char payload[16384], readback[16384];
	unsigned writes_before, syncs_before;
	enum bootfat_type types[] = { ZEDBSD_FAT12, ZEDBSD_FAT16, ZEDBSD_FAT32 };
	unsigned i;

	memset(payload, 0x59, sizeof(payload));
	for (i = 0; i < ARRAY_COUNT(types); i++) {
		current_type = types[i];
		format_image(&image, types[i], 1U, 2U);
		CHECK_ERROR(host_mount(&image, 0U, &mountp), 0);
		writes_before = image.writes;
		syncs_before = image.syncs;
		inode = create_payload(mountp.m_root, "BATCH.DAT", payload, sizeof(payload));
		printf("FAT%u create writes=%u sync=%u\n", types[i], image.writes - writes_before, image.syncs - syncs_before);
		CHECK_ERROR(host_file_open(inode, O_RDWR, &file), 0);
		writes_before = image.writes;
		syncs_before = image.syncs;
		CHECK(file.f_ops->pwrite(&file, payload, sizeof(payload), sizeof(payload)) == sizeof(payload));
		CHECK_ERROR(file.f_ops->fsync(&file), 0);
		printf("FAT%u append writes=%u sync=%u\n", types[i], image.writes - writes_before, image.syncs - syncs_before);
		CHECK(file.f_ops->pread(&file, readback, sizeof(readback), sizeof(payload)) == sizeof(readback));
		CHECK(memcmp(payload, readback, sizeof(payload)) == 0);
		CHECK_ERROR(host_file_close(&file), 0);
		inode_release(inode);
		writes_before = image.writes;
		syncs_before = image.syncs;
		CHECK_ERROR(unlink_child(mountp.m_root, "BATCH.DAT"), 0);
		printf("FAT%u unlink writes=%u sync=%u\n", types[i], image.writes - writes_before, image.syncs - syncs_before);
		check_fat_copies(&image);
		host_unmount(&mountp);
		destroy_image(&image);
	}
	printf("FAT batch cost PASS %u checks\n", checks);
	return 0;
}
