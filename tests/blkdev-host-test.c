/*
 * Boots block device and PC-98 partition scheme host tests
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "core/blkdev.h"
#include "core/partition.h"
#include "platform/pc98/partition-pc98.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expression)						\
	do {								\
		if (!(expression)) {					\
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__,	\
			       #expression);				\
			failures++;					\
		}							\
	} while (0)

/* --------------------------------------------------------------- */
/* In-memory fake device.                                          */

#define FAKE_SECTORS 64U

struct fake_disk {
	uint8_t data[FAKE_SECTORS * 512U];
	unsigned read_calls;
	unsigned write_calls;
};

static enum boots_blkdev_result
fake_read(struct boots_blkdev *dev, uint64_t lba, uint32_t count, void *buffer)
{
	struct fake_disk *disk = dev->private_data;

	disk->read_calls++;
	memcpy(buffer, disk->data + (size_t)lba * 512U, (size_t)count * 512U);
	return BOOTS_BLKDEV_OK;
}

static enum boots_blkdev_result
fake_write(struct boots_blkdev *dev, uint64_t lba, uint32_t count,
	   const void *buffer)
{
	struct fake_disk *disk = dev->private_data;

	disk->write_calls++;
	memcpy(disk->data + (size_t)lba * 512U, buffer, (size_t)count * 512U);
	return BOOTS_BLKDEV_OK;
}

static struct fake_disk fake;
static struct boots_blkdev fake_dev;

static void
setup_fake(void)
{
	memset(&fake, 0, sizeof(fake));
	memset(&fake_dev, 0, sizeof(fake_dev));
	strcpy(fake_dev.name, "fake0");
	fake_dev.sector_size = 512;
	fake_dev.sector_count = FAKE_SECTORS;
	fake_dev.heads = 8;
	fake_dev.sectors_per_track = 17;
	fake_dev.read = fake_read;
	fake_dev.write = fake_write;
	fake_dev.private_data = &fake;
}

/* --------------------------------------------------------------- */
/* Registry.                                                       */

static void
test_registry(void)
{
	uint8_t buffer[512];

	boots_blkdev_reset();
	setup_fake();
	CHECK(boots_blkdev_count() == 0);
	CHECK(boots_blkdev_register(&fake_dev));
	CHECK(boots_blkdev_count() == 1);
	CHECK(boots_blkdev_get(0) == &fake_dev);
	CHECK(boots_blkdev_get(1) == NULL);
	CHECK(boots_blkdev_find("fake0") == &fake_dev);
	CHECK(boots_blkdev_find("nope") == NULL);

	/* Range checking happens above the driver. */
	CHECK(boots_blkdev_read(&fake_dev, 0, 1, buffer) == BOOTS_BLKDEV_OK);
	CHECK(boots_blkdev_read(&fake_dev, FAKE_SECTORS, 1, buffer) ==
	      BOOTS_BLKDEV_OUT_OF_RANGE);
	CHECK(boots_blkdev_read(&fake_dev, FAKE_SECTORS - 1U, 2, buffer) ==
	      BOOTS_BLKDEV_OUT_OF_RANGE);
	CHECK(boots_blkdev_read(&fake_dev, 0, 0, buffer) ==
	      BOOTS_BLKDEV_INVALID);

	/* A device without a write callback is read-only. */
	fake_dev.write = NULL;
	CHECK(boots_blkdev_write(&fake_dev, 0, 1, buffer) ==
	      BOOTS_BLKDEV_READ_ONLY);
	fake_dev.write = fake_write;
	CHECK(boots_blkdev_write(&fake_dev, 3, 1, buffer) == BOOTS_BLKDEV_OK);

	/* Malformed registrations are rejected. */
	{
		struct boots_blkdev bad;

		memset(&bad, 0, sizeof(bad));
		CHECK(!boots_blkdev_register(&bad));
		CHECK(!boots_blkdev_register(NULL));
	}
}

/* --------------------------------------------------------------- */
/* PC-98 partition scheme.                                         */

static void
put_entry(uint8_t *table, unsigned slot, unsigned boot_flags,
	  unsigned start_sect, unsigned start_head, unsigned start_cyl,
	  unsigned data_sect, unsigned data_head, unsigned data_cyl,
	  const char *name)
{
	uint8_t *p = table + slot * 32U;
	unsigned i;

	p[0] = (uint8_t)boot_flags;
	p[1] = (uint8_t)(boot_flags & 0x80U);
	p[4] = (uint8_t)start_sect;
	p[5] = (uint8_t)start_head;
	p[6] = (uint8_t)start_cyl;
	p[7] = (uint8_t)(start_cyl >> 8);
	p[8] = (uint8_t)data_sect;
	p[9] = (uint8_t)data_head;
	p[10] = (uint8_t)data_cyl;
	p[11] = (uint8_t)(data_cyl >> 8);
	for (i = 0; name[i] != '\0' && i < 16; i++)
		p[16 + i] = (uint8_t)name[i];
	for (; i < 16; i++)
		p[16 + i] = ' ';
}

static void
test_pc98_partitions(void)
{
	struct boots_partition entries[16];
	uint8_t *table = fake.data + 512;   /* LBA 1 */
	int count;

	boots_blkdev_reset();
	setup_fake();
	CHECK(boots_blkdev_register(&fake_dev));
	boots_partition_set_scheme(&boots_partition_scheme_pc98);

	memset(table, 0, 512);
	/* geometry 8 heads x 17 spt:
	 * cyl 1 head 0 sect 0 -> lba (1*8+0)*17+0 = 136
	 * cyl 2 head 1 sect 3 -> lba (2*8+1)*17+3 = 292 */
	put_entry(table, 0, 0x80, 0, 0, 1, 3, 1, 2, "BOOT");
	/* 0x11: a live, non-bootable entry (byte 0 is zero only when the
	 * slot is unused). */
	put_entry(table, 2, 0x11, 0, 0, 3, 0, 0, 4, "DATA VOL");

	count = boots_partition_scan(&fake_dev, entries, 16);
	CHECK(count == 16);
	CHECK(entries[0].bootable == 1);
	CHECK(entries[0].start_lba == 136);
	CHECK(entries[0].data_lba == 292);
	CHECK(entries[0].sector_count == 0);
	CHECK(strcmp(entries[0].name, "BOOT") == 0);
	/* Slot 1 is empty. */
	CHECK(entries[1].bootable == 0 && entries[1].start_lba == 0 &&
	      entries[1].data_lba == 0 && entries[1].name[0] == '\0');
	/* Slot 2: non-bootable, name stops at the first space. */
	CHECK(entries[2].bootable == 0);
	CHECK(entries[2].start_lba == (uint64_t)(3 * 8) * 17);
	CHECK(strcmp(entries[2].name, "DATA") == 0);

	/* Unregistered scheme and bad arguments fail cleanly. */
	boots_partition_set_scheme(NULL);
	CHECK(boots_partition_scan(&fake_dev, entries, 16) == -1);
	boots_partition_set_scheme(&boots_partition_scheme_pc98);
	CHECK(boots_partition_scan(NULL, entries, 16) == -1);

	/* A geometry-less device cannot be interpreted. */
	fake_dev.heads = 0;
	CHECK(boots_partition_scan(&fake_dev, entries, 16) == -1);
	fake_dev.heads = 8;
}

int
main(void)
{
	test_registry();
	test_pc98_partitions();
	if (failures != 0) {
		printf("blkdev tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("Boots blkdev/partition host tests: OK\n");
	return 0;
}
