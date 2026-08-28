/*
 * NEC PC-98 partition-table scheme
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The table lives in the sector after the IPL (LBA 1 on the 512-byte
 * disks this loader supports) and holds sixteen 32-byte entries.  All
 * addresses are CHS in the geometry the firmware sensed at boot, which
 * is why the disk geometry ioctl carries that geometry rather than the
 * drive's native IDENTIFY values.  The inclusive end CHS determines the
 * partition disk's block count.
 */

#include <drivers/disklabel.h>

#define PC98_TABLE_LBA 1U
#define PC98_ENTRY_SIZE 32U

static uint16_t
get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* sect, head, cyl(16bit) -> LBA in the firmware geometry. */
static int
chs_to_lba(const struct disk_geometry *geometry, const uint8_t *p,
	   uint64_t *result)
{
	uint16_t cylinder = get16(p + 2);
	if (p[1] >= geometry->heads || p[0] >= geometry->sectors_per_track)
		return 0;
	*result = ((uint64_t)cylinder * geometry->heads + p[1]) *
		geometry->sectors_per_track + p[0];
	return 1;
}

static int
pc98_scan(const struct partition_scheme *scheme,
	  struct disk *dev, struct partition *entries,
	  unsigned max_entries)
{
	struct disk_geometry geometry;
	uint8_t sector[512];
	unsigned count;
	unsigned i;

	(void)scheme;
	if (dev->d_block_size != 512 ||
	    disk_ioctl(dev, DISK_IOCTL_GET_GEOMETRY, &geometry) != 0 ||
	    geometry.heads == 0 || geometry.sectors_per_track == 0)
		return -1;
	if (disk_read(dev, PC98_TABLE_LBA, 1, sector) != 0)
		return -1;
	count = 512U / PC98_ENTRY_SIZE;
	if (count > max_entries)
		count = max_entries;
	for (i = 0; i < count; i++) {
		const uint8_t *p = sector + i * PC98_ENTRY_SIZE;
		struct partition *entry = &entries[i];
		uint64_t end_lba;
		unsigned j;

		entry->p_parent = dev;
		entry->p_disk = NULL;
		entry->p_index = i;
		entry->p_start_block = 0;
		entry->p_data_block = 0;
		entry->p_block_count = 0;
		entry->p_flags = 0;
		entry->p_label[0] = '\0';
		entry->p_uuid[0] = '\0';
		if (p[0] == 0)
			continue;
		if (!chs_to_lba(&geometry, p + 4, &entry->p_start_block) ||
		    !chs_to_lba(&geometry, p + 8, &entry->p_data_block) ||
		    !chs_to_lba(&geometry, p + 12, &end_lba) ||
		    entry->p_data_block < entry->p_start_block ||
		    end_lba < entry->p_data_block || end_lba >= dev->d_block_count)
			continue;
		entry->p_block_count = end_lba - entry->p_data_block + 1U;
		if ((p[0] & 0x80U) && (p[1] & 0x80U))
			entry->p_flags |= PARTITION_BOOTABLE;
		for (j = 0; j < 16; j++) {
			char c = (char)p[16 + j];

			if (c == '\0' || c == ' ')
				break;
			entry->p_label[j] = c;
		}
		entry->p_label[j] = '\0';
		if (j != 0) entry->p_flags |= PARTITION_HAS_LABEL;
	}
	return (int)count;
}

const struct partition_scheme partition_scheme_pc98 = {
	.name = "pc98",
	.scan = pc98_scan,
};
