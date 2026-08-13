/*
 * zedBSD disk/bio and PC-98 partition scheme host tests
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/disk.h"
#include "kern/partition.h"
#include "kern/mbr-partition.h"
#include "kern/pc98/partition.h"
#include "kern/pc98/partition-auto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expression)                                                \
	do {                                                               \
		if (!(expression)) {                                          \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
			       #expression);                                   \
			failures++;                                             \
		}                                                          \
	} while (0)

/* --------------------------------------------------------------- */
/* In-memory leaf disk.                                            */

#define FAKE_SECTORS 1024U

struct fake_disk {
	uint8_t data[FAKE_SECTORS * 512U];
	unsigned read_calls;
	unsigned write_calls;
};

static struct fake_disk fake;

static int
fake_submit(struct disk *dev, struct bio *bio)
{
	struct fake_disk *store = dev->d_data;
	size_t bytes = (size_t)bio->b_block_count * dev->d_block_size;
	uint8_t *where = store->data + (size_t)bio->b_mapped_block * 512U;

	CHECK(dev == bio->b_leaf_disk);
	if (bio->b_op == BIO_READ) {
		store->read_calls++;
		memcpy(bio->b_data, where, bytes);
	} else if (bio->b_op == BIO_WRITE) {
		store->write_calls++;
		memcpy(where, bio->b_data, bytes);
	} else if (bio->b_op != BIO_FLUSH) {
		return EOPNOTSUPP;
	}
	bio_complete(bio, 0, bytes);
	return 0;
}

static int
fake_ioctl(struct disk *dev, unsigned long request, void *argument)
{
	struct disk_geometry *geometry = argument;

	(void)dev;
	if (request != DISK_IOCTL_GET_GEOMETRY)
		return EOPNOTSUPP;
	if (geometry == NULL)
		return EINVAL;
	geometry->cylinders = 8;
	geometry->heads = 8;
	geometry->sectors_per_track = 17;
	return 0;
}

static const struct disk_ops fake_ops = {
	.submit = fake_submit,
	.ioctl = fake_ioctl,
};

static struct disk *
setup_fake(void)
{
	struct disk *dev;

	memset(&fake, 0, sizeof(fake));
	disk_registry_reset();
	partition_reset();
	dev = disk_alloc();
	CHECK(dev != NULL);
	if (dev == NULL)
		return NULL;
	strcpy(dev->d_name, "fake0");
	dev->d_block_size = 512;
	dev->d_block_count = FAKE_SECTORS;
	dev->d_max_transfer_blocks = 255;
	dev->d_ops = &fake_ops;
	dev->d_data = &fake;
	CHECK(disk_create(dev) == 0);
	return dev;
}

/* --------------------------------------------------------------- */
/* Registry, synchronous bio, and slice mapping.                   */

static void
test_registry(void)
{
	uint8_t buffer[512];
	struct disk *dev = setup_fake();
	struct disk *bad;

	CHECK(disk_count() == 1);
	CHECK(disk_at(0) == dev);
	CHECK(disk_at(1) == NULL);
	CHECK(disk_find("fake0") == dev);
	CHECK(disk_find_by_dev(dev->d_dev) == dev);
	CHECK(disk_find("nope") == NULL);

	CHECK(disk_read(dev, 0, 1, buffer) == 0);
	CHECK(fake.read_calls == 1);
	CHECK(disk_read(dev, FAKE_SECTORS, 1, buffer) == EOVERFLOW);
	CHECK(disk_read(dev, FAKE_SECTORS - 1U, 2, buffer) == EOVERFLOW);
	CHECK(disk_read(dev, 0, 0, buffer) == EINVAL);

	dev->d_flags |= DISK_READ_ONLY;
	CHECK(disk_write(dev, 0, 1, buffer) == EROFS);
	dev->d_flags &= ~DISK_READ_ONLY;
	memset(buffer, 0x5a, sizeof(buffer));
	CHECK(disk_write(dev, 3, 1, buffer) == 0);
	CHECK(fake.write_calls == 1);
	CHECK(fake.data[3U * 512U] == 0x5a);
	CHECK(bio_flush(dev) == 0);

	CHECK(disk_open(dev) == 0);
	CHECK(dev->d_open_count == 1);
	disk_close(dev);
	CHECK(dev->d_open_count == 0);

	bad = disk_alloc();
	CHECK(bad != NULL);
	CHECK(disk_create(bad) == EINVAL);
	CHECK(disk_create(NULL) == EINVAL);
}

/* --------------------------------------------------------------- */
/* PC-98 partition scheme.                                         */

static void
put_chs(uint8_t *p, unsigned sect, unsigned head, unsigned cyl)
{
	p[0] = (uint8_t)sect;
	p[1] = (uint8_t)head;
	p[2] = (uint8_t)cyl;
	p[3] = (uint8_t)(cyl >> 8);
}

static void
put_entry(uint8_t *table, unsigned slot, unsigned boot_flags,
	  unsigned start_sect, unsigned start_head, unsigned start_cyl,
	  unsigned data_sect, unsigned data_head, unsigned data_cyl,
	  unsigned end_sect, unsigned end_head, unsigned end_cyl,
	  const char *name)
{
	uint8_t *p = table + slot * 32U;
	unsigned i;

	p[0] = (uint8_t)boot_flags;
	p[1] = (uint8_t)(boot_flags & 0x80U);
	put_chs(p + 4, start_sect, start_head, start_cyl);
	put_chs(p + 8, data_sect, data_head, data_cyl);
	put_chs(p + 12, end_sect, end_head, end_cyl);
	for (i = 0; name[i] != '\0' && i < 16; i++)
		p[16 + i] = (uint8_t)name[i];
	for (; i < 16; i++)
		p[16 + i] = ' ';
}

static void
test_pc98_partitions(void)
{
	struct partition entries[PARTITION_MAX];
	struct disk *dev = setup_fake();
	struct disk *slice;
	uint8_t buffer[512];
	uint8_t *table = fake.data + 512;   /* LBA 1 */
	int count;

	partition_set_scheme(&partition_scheme_pc98);
	memset(table, 0, 512);
	/* 8 heads x 17 sectors.  Entry 0 data starts at LBA 292 and
	 * ends at LBA 407 (cyl 2, head 7, sector 16). */
	put_entry(table, 0, 0x80, 0, 0, 1, 3, 1, 2,
		  16, 7, 2, "BOOT");
	put_entry(table, 2, 0x11, 0, 0, 3, 0, 0, 4,
		  16, 7, 4, "DATA VOL");

	count = partition_scan(dev, entries, PARTITION_MAX);
	CHECK(count == 16);
	CHECK((entries[0].p_flags & PARTITION_BOOTABLE) != 0);
	CHECK(entries[0].p_start_block == 136);
	CHECK(entries[0].p_data_block == 292);
	CHECK(entries[0].p_block_count == 116);
	CHECK(strcmp(entries[0].p_label, "BOOT") == 0);
	CHECK(entries[1].p_block_count == 0);
	CHECK((entries[2].p_flags & PARTITION_BOOTABLE) == 0);
	CHECK(entries[2].p_start_block == 408);
	CHECK(entries[2].p_data_block == 544);
	CHECK(entries[2].p_block_count == 136);
	CHECK(strcmp(entries[2].p_label, "DATA") == 0);

	CHECK(partition_create_disk(&entries[0]) == 0);
	slice = entries[0].p_disk;
	CHECK(slice != NULL);
	CHECK(strcmp(slice->d_name, "fake0p1") == 0);
	CHECK(slice->d_parent == dev);
	CHECK(slice->d_parent_offset == 292);
	CHECK(slice->d_block_count == 116);
	fake.data[292U * 512U] = 0xa5;
	memset(buffer, 0, sizeof(buffer));
	CHECK(disk_read(slice, 0, 1, buffer) == 0);
	CHECK(buffer[0] == 0xa5);
	CHECK(disk_read(slice, 116, 1, buffer) == EOVERFLOW);

	partition_set_scheme(NULL);
	CHECK(partition_scan(dev, entries, PARTITION_MAX) == -EINVAL);
	partition_set_scheme(&partition_scheme_pc98);
	CHECK(partition_scan(NULL, entries, PARTITION_MAX) == -EINVAL);
}

static void
put_le32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static void
test_pc98_partition_auto(void)
{
	struct partition entries[PARTITION_MAX];
	struct disk *dev = setup_fake();
	uint8_t *mbr = fake.data;
	uint8_t *raw = mbr + 0x1be;
	uint8_t *native = fake.data + 512;
	int count;

	partition_set_scheme(&partition_scheme_pc98_auto);
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	raw[0] = 0x80;
	raw[4] = 0x0e;
	put_le32(raw + 8, 128);
	put_le32(raw + 12, 256);
	count = partition_scan(dev, entries, PARTITION_MAX);
	CHECK(count == 4);
	CHECK(entries[0].p_start_block == 128);
	CHECK(entries[0].p_block_count == 256);
	CHECK(strcmp(entries[0].p_label, "mbr1") == 0);

	/* Removing only the marker selects the LBA-1 native table. */
	mbr[510] = 0;
	mbr[511] = 0;
	memset(native, 0, 512);
	put_entry(native, 0, 0x80, 0, 0, 1, 0, 0, 1,
	    16, 7, 1, "NATIVE");
	count = partition_scan(dev, entries, PARTITION_MAX);
	CHECK(count == 16);
	CHECK(entries[0].p_start_block == 136);
	CHECK(strcmp(entries[0].p_label, "NATIVE") == 0);

	/* A signed but empty MBR remains MBR; there is no native fallback. */
	memset(mbr, 0, 512);
	mbr[510] = 0x55;
	mbr[511] = 0xaa;
	count = partition_scan(dev, entries, PARTITION_MAX);
	CHECK(count == 4);
	CHECK(entries[0].p_block_count == 0);
}

int
main(void)
{
	test_registry();
	test_pc98_partitions();
	test_pc98_partition_auto();
	if (failures != 0) {
		printf("disk/bio tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("zedBSD disk/bio/partition host tests: OK\n");
	return 0;
}
