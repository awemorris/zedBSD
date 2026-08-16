/* zedBSD PC/AT MBR partition parser host tests.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/disk.h"
#include "kern/buf.h"
#include "kern/partition.h"
#include "kern/mbr-partition.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define SECTORS 4096U
static uint8_t medium[SECTORS * 512U];
static int failures;

#define CHECK(expr) do { if (!(expr)) { \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; \
} } while (0)

static void put32(uint8_t *p, uint32_t value)
{
	p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8);
	p[2]=(uint8_t)(value>>16); p[3]=(uint8_t)(value>>24);
}

static void entry(unsigned slot, uint8_t active, uint8_t type,
    uint32_t start, uint32_t blocks)
{
	uint8_t *p=medium+0x1beU+slot*16U;
	p[0]=active; p[4]=type; put32(p+8,start); put32(p+12,blocks);
}

static int submit(struct disk *disk, struct bio *bio)
{
	size_t bytes=(size_t)bio->b_block_count*512U;
	(void)disk;
	if (bio->b_op == BIO_READ)
		memcpy(bio->b_data,medium+(size_t)bio->b_mapped_block*512U,bytes);
	else if (bio->b_op == BIO_WRITE)
		memcpy(medium+(size_t)bio->b_mapped_block*512U,bio->b_data,bytes);
	else if (bio->b_op != BIO_FLUSH)
		return EOPNOTSUPP;
	bio_complete(bio,0,bio->b_op == BIO_FLUSH ? 0 : bytes);
	return 0;
}

static const struct disk_ops ops={ .submit=submit };

static struct disk *setup(void)
{
	struct disk *disk;
	memset(medium,0,sizeof(medium));
	buf_reset();
	disk_registry_reset(); partition_reset();
	disk=disk_alloc(); CHECK(disk != NULL);
	if (disk == NULL) return NULL;
	strcpy(disk->d_name,"ata0"); disk->d_block_size=512;
	disk->d_block_count=SECTORS; disk->d_max_transfer_blocks=255;
	disk->d_ops=&ops; CHECK(disk_create(disk) == 0);
	partition_set_scheme(&partition_scheme_mbr);
	medium[510]=0x55; medium[511]=0xaa;
	return disk;
}

static void test_valid(void)
{
	struct partition part[4]; struct disk *disk=setup(); int count;
	entry(0,0x80,0x0e,63,1024); entry(1,0,0x83,2048,512);
	entry(2,0,0x0f,3000,500); entry(3,0,0xee,3500,100);
	memset(part,0xa5,sizeof(part)); count=partition_scan(disk,part,4);
	CHECK(count == 4); CHECK(part[0].p_index == 0);
	CHECK(part[0].p_start_block == 63); CHECK(part[0].p_block_count == 1024);
	CHECK((part[0].p_flags & PARTITION_BOOTABLE) != 0);
	CHECK(part[1].p_start_block == 2048); CHECK(part[1].p_block_count == 512);
	CHECK(part[2].p_block_count == 0); CHECK(part[3].p_block_count == 0);
	CHECK(strcmp(part[0].p_label,"mbr1") == 0);
	CHECK(partition_create_disk(&part[0]) == 0);
	CHECK(strcmp(part[0].p_disk->d_name,"ata0p1") == 0);
	CHECK(part[0].p_disk->d_parent_offset == 63);
}

static void test_rejections(void)
{
	struct partition part[4]; struct disk *disk=setup();
	medium[511]=0; CHECK(partition_scan(disk,part,4) == -1);
	medium[511]=0xaa;
	entry(0,0x7f,0x0e,10,10); entry(1,0,0x0e,10,0);
	entry(2,0,0x0e,SECTORS-2U,4); entry(3,0,0,10,10);
	CHECK(buf_invalidate_disk(disk, BUF_INVALIDATE_DISCARD) == 0);
	CHECK(partition_scan(disk,part,4) == 4);
	for (unsigned i=0;i<4;i++) CHECK(part[i].p_block_count == 0);
	disk->d_block_size=1024; CHECK(partition_scan(disk,part,4) == -1);
}

int main(void)
{
	CHECK(buf_init() == 0);
	test_valid(); test_rejections();
	if (failures) { printf("PC/AT MBR tests: %d failure(s)\n",failures); return 1; }
	puts("zedBSD PC/AT MBR host tests: OK"); return 0;
}
