/* Block-device discovery, partition selection, and image dispatch. SPDX-License-Identifier: Zlib */
#include "kern/internal.h"
#include "kern/disk.h"
#include "kern/fat16.h"
#include "kern/partition.h"
#include "kern/platform.h"

#ifdef ZEDBSD_M9_WRITE_TEST
static uint8_t m9_original[512], m9_pattern[512], m9_observed[512];
#endif

/*
 * Native block device lookup.  The firmware enumerates IDE disks in the
 * same bank-major order the driver probes them, so BIOS IDs 80h..83h map
 * directly to native IDE slots.  Non-IDE classes have no native driver yet
 * (SCSI later; floppies need a future FDC driver) and read as absent.
 */
static struct disk *blk_for_dev(const struct boot_device *d)
{
	return kern_platform_block_device(d);
}
/* Nonzero on failure, matching the old gateway convention. */
static int readsec(const struct boot_device *d, uint32_t lba, void *buf)
{
	struct disk *blk = blk_for_dev(d);

	return blk == 0 || disk_read(blk, lba, 1, buf) != 0;
}
static int writesec(const struct boot_device *d, uint32_t lba,
		    const void *buf)
{
	struct disk *blk = blk_for_dev(d);

	return blk == 0 || disk_write(blk, lba, 1, buf) != 0;
}
/* PC-98 partition-table discovery using per-device BIOS logical geometry. */
void devname(int i)
{
	switch (devs[i].device_class) {
	case ZEDBSD_DEV_FDD:
		puts("fd");
		break;
	case ZEDBSD_DEV_IDE:
		puts("ide");
		break;
	default:
		puts("scsi");
	}
	dec(devs[i].display_index);
}
int scanparts(int di)
{
	struct partition entries[MAX_PARTS];
	struct disk *blk;
	int count;

	memzero(parts, sizeof(parts));
	if (di < 0 || !(devs[di].flags & ZEDBSD_DEV_HAS_GEOMETRY))
		return 0;
	blk = blk_for_dev(&devs[di]);
	if (blk == 0)
		return 0;
	count = partition_scan(blk, entries, MAX_PARTS);
	if (count < 0)
		return 0;
	for (int i = 0; i < count; i++) {
		const struct partition *e = &entries[i];

		if (e->p_block_count == 0)
			continue;
		parts[i].valid = 1;
		parts[i].index = i;
		parts[i].bootable = (e->p_flags & PARTITION_BOOTABLE) != 0;
		parts[i].start = (uint32_t)e->p_start_block;
		parts[i].data = (uint32_t)e->p_data_block;
		parts[i].count = (uint32_t)e->p_block_count;
		for (int j = 0; j < 16; j++) {
			parts[i].name[j] = e->p_label[j];
			if (!parts[i].name[j])
				break;
		}
		parts[i].name[16] = 0;
	}
	return 1;
}
static int disk_volume_read(const void *context, uint32_t lba, void *buffer)
{
	return !readsec(context, lba, buffer);
}
static int disk_volume_write(void *context, uint32_t lba,
			     const void *buffer)
{
	return !writesec(context, lba, buffer);
}

static int mountpart_into(struct bootfs *filesystem,
			  int device_index, int partition_index)
{
	const struct bootfs_driver *const drivers[] = {
		&bootfat16_driver,
		&bootfat12_driver,
	};
	struct boot_volume volume;

	if (!parts[partition_index].valid)
		return 0;
	/* The shared volume ABI uses mutable context for write callbacks.  The
	 * device descriptor itself remains logically read-only. */
	volume.context = (void *)&devs[device_index];
	volume.start_lba = parts[partition_index].data;
	volume.sector_size = 512;
	volume.read = disk_volume_read;
	volume.write = disk_volume_write;
	return bootfs_mount(filesystem, &volume, drivers,
			       sizeof(drivers) / sizeof(drivers[0]));
}

int mountpart(int device_index, int partition_index)
{
	return mountpart_into(&mounted_fs, device_index, partition_index);
}

static int disk_mount_name(int device_index, char name[8])
{
	unsigned ordinal;

	if (device_index < 0 || (unsigned)device_index >= device_count)
		return 0;
	ordinal = (unsigned)device_index + 1U;
	name[0] = 'd';
	name[1] = 'i';
	name[2] = 's';
	name[3] = 'k';
	if (ordinal < 10U) {
		name[4] = (char)('0' + ordinal);
		name[5] = 0;
	} else {
		name[4] = (char)('0' + ordinal / 10U);
		name[5] = (char)('0' + ordinal % 10U);
		name[6] = 0;
	}
	return 1;
}

void select_disk_home(int device_index)
{
	char name[8];
	char home[24];
	char dictionary[48];
	unsigned name_length;

	if (!disk_mount_name(device_index, name) ||
	    !bootfs_namespace_set_default(&mounted_namespace, name))
		return;
	home[0] = '/';
	name_length = slen(name);
	memcopy(home + 1, name, name_length);
	memcopy(home + 1 + name_length, "/home", 6);
	(void)env_set(&boot_environment, "HOME", home);
	memcopy(dictionary, home, slen(home));
	memcopy(dictionary + slen(home), "/skkjisyo.dic", 14);
	(void)env_set(&boot_environment, "REMACS_SKK_DICT", dictionary);
}

/* Mount one user-visible FAT volume per physical disk.  BOOT is preferred;
 * otherwise the first readable FAT16 partition becomes /diskN.  The
 * namespace is intentionally above the filesystem drivers so future ext4
 * and UFS drivers can use the same UNIX path contract. */
void register_scanned_disk(int device_index)
{
	struct bootfs filesystem;
	char name[8];
	int preferred = -1;
	int fallback = -1;
	int partition;

	if (!disk_mount_name(device_index, name))
		return;
	for (partition = 0; partition < MAX_PARTS; partition++) {
		if (!parts[partition].valid)
			continue;
		if (streq(parts[partition].name, "BOOT"))
			preferred = partition;
		else if (fallback < 0)
			fallback = partition;
	}
	if (preferred >= 0 &&
	    mountpart_into(&filesystem, device_index, preferred)) {
		(void)bootfs_namespace_mount(&mounted_namespace, name, &filesystem);
		return;
	}
	for (partition = fallback; partition >= 0 && partition < MAX_PARTS;
	     partition++)
		if (parts[partition].valid &&
		    mountpart_into(&filesystem, device_index, partition)) {
			(void)bootfs_namespace_mount(&mounted_namespace, name,
						&filesystem);
			return;
		}
}

/* Keyboard input, parser, and stateful shell selection helpers. */
/*
 * 0:055Dh contains the dense BIOS IDE-drive map in its low nibble.  Unlike
 * physical-slot bitmap 0:05BAh, bit N corresponds directly to BIOS unit
 * 80h+N.  Stock ROMs may not return from SENSE for an absent unit, so this
 * map is authoritative for enumeration.  Preserve a boot unit already
 * handed to Stage 2 even if unusual firmware failed to publish the bit.
 */
uint8_t ide_reported_drives(void)
{
	uint8_t bitmap = 0;

	for (unsigned i = 0; i < device_count; i++)
		if (devs[i].device_class == ZEDBSD_DEV_IDE &&
		    devs[i].bios_id >= 0x80 &&
		    devs[i].bios_id < 0x80 + MAX_IDE_DEVICES)
			bitmap |= 1U << (devs[i].bios_id - 0x80);
	return bitmap;
}

/*
 * A PC-9801-55/92 host adapter normally owns SCSI ID 7.  Some firmware sets
 * bit 7 in 0:0482h for the adapter itself; treating it as an eighth disk and
 * issuing SENSE to A7h can enter firmware that never returns.  Enumerate only
 * the seven target IDs which the registered SCSI BIOS reports as disks.
 */
uint8_t scsi_reported_targets(void)
{
	uint8_t bitmap = 0;
	for (unsigned i = 0; i < device_count; i++)
		if (devs[i].device_class == ZEDBSD_DEV_SCSI &&
		    devs[i].bios_id >= 0xa0 &&
		    devs[i].bios_id < 0xa0 + MAX_SCSI_TARGETS)
			bitmap |= 1U << (devs[i].bios_id - 0xa0);
	return bitmap;
}

unsigned bit_count(uint8_t value)
{
	unsigned count = 0;

	while (value) {
		count += value & 1U;
		value >>= 1;
	}
	return count;
}

/* Probe exactly one BIOS unit.  A nonnegative result is the new list index. */
int probe_fixed_device(uint8_t device_class, uint8_t bios_id)
{
	for (unsigned i = 0; i < device_count; i++)
		if (devs[i].device_class == device_class &&
		    devs[i].bios_id == bios_id)
			return (int)i;
	return -1;
}

/* Shell probes remain exhaustive across every supported BIOS unit. */
void probe_fixed_class(uint8_t device_class)
{
	uint8_t first = device_class == ZEDBSD_DEV_IDE ? 0x80 : 0xa0;
	unsigned count = device_class == ZEDBSD_DEV_IDE ? MAX_IDE_DEVICES :
							MAX_SCSI_TARGETS;

	/* IDE disks back mounted partition objects.  Replacing their registry
	 * objects in place would invalidate live mounts; the initial native probe
	 * is authoritative until hot-unplug lifecycle support is added. */
	for (unsigned index = 0; index < count; index++)
		probe_fixed_device(device_class, first + index);
}


#ifdef ZEDBSD_M9_WRITE_TEST
static void m9_debug_puts(const char *text)
{
	kern_platform_debug_write(text);
}

static void m9_report(const char *text)
{
	puts(text);
	m9_debug_puts(text);
}

static int m9_same_sector(const uint8_t *left, const uint8_t *right)
{
	for (unsigned i = 0; i < 512; i++)
		if (left[i] != right[i])
			return 0;
	return 1;
}

/* Destructive raw-sector test, compiled only into vmunix-m9.  The caller
 * must select an expendable sector in a temporary image.  Once the first
 * write succeeds, every exit path attempts to restore the original sector. */
int m9_write_test(uint32_t lba)
{
	int result = 1;

	if (curdev < 0) {
		m9_report("M9 disk write test: no selected disk\n");
		return 0;
	}
	if (readsec(&devs[curdev], lba, m9_original)) {
		m9_report("M9 disk write test: initial read failed\n");
		return 0;
	}
	for (unsigned i = 0; i < sizeof(m9_pattern); i++)
		m9_pattern[i] = (uint8_t)(0xa5U ^ i ^ lba ^ (lba >> 8));
	if (writesec(&devs[curdev], lba, m9_pattern)) {
		m9_report("M9 disk write test: pattern write failed\n");
		return 0;
	}
	if (readsec(&devs[curdev], lba, m9_observed) ||
	    !m9_same_sector(m9_observed, m9_pattern)) {
		m9_report("M9 disk write test: pattern read-back failed\n");
		result = 0;
	}
	if (writesec(&devs[curdev], lba, m9_original)) {
		m9_report("M9 disk write test: RESTORE FAILED\n");
		return 0;
	}
	if (readsec(&devs[curdev], lba, m9_observed) ||
	    !m9_same_sector(m9_observed, m9_original)) {
		m9_report("M9 disk write test: restore read-back failed\n");
		return 0;
	}
	if (result)
		m9_report("M9 disk write/read/restore: PASS\n");
	return result;
}
#endif

/* Resolve PREFIX + NAME.EXT with the name upper-cased. */
