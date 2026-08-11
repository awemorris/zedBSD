/*
 * NEC PC-98 internal IDE driver (PIO, polled)
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The PC-98 built-in IDE multiplexes two ATA channels (each with a
 * master/slave pair) onto one register block: port 0x432 bit 0 selects
 * the active bank, the command block lives at 0x640-0x64e with a 2-byte
 * stride (16-bit data at 0x640), and the control block at 0x74c/0x74e.
 * Reference model: qemu-pc98 hw/ide/pc98-ide.c; reference driver:
 * linux-pc98 drivers/ata/pata_pc9800.c.
 *
 * Interrupts stay disabled (nIEN); everything is polled.  IRQ 9
 * completion arrives with the Phase D scheduler.
 */

#include "drivers/pc98-ide.h"
#include "kern/boot.h"
#include <errno.h>

#define IDE_BANK_SELECT   0x432U
#define IDE_DATA          0x640U
#define IDE_ERROR         0x642U
#define IDE_NSECT         0x644U
#define IDE_LBA_LOW       0x646U
#define IDE_LBA_MID       0x648U
#define IDE_LBA_HIGH      0x64aU
#define IDE_DRIVE_HEAD    0x64cU
#define IDE_STATUS        0x64eU   /* read: status, write: command */
#define IDE_ALT_STATUS    0x74cU   /* read: alt status, write: devctl */

#define IDE_STATUS_BSY    0x80U
#define IDE_STATUS_DRDY   0x40U
#define IDE_STATUS_DRQ    0x08U
#define IDE_STATUS_ERR    0x01U

#define IDE_CMD_READ      0x20U
#define IDE_CMD_WRITE     0x30U
#define IDE_CMD_IDENTIFY  0xecU

#define IDE_DEVCTL_NIEN   0x02U

#define IDE_UNIT_MAX      4U
/* Polls before a stuck controller is declared dead.  PIO on real
 * hardware answers within microseconds; QEMU immediately. */
#define IDE_TIMEOUT_SPINS 5000000U

struct ide_unit {
	struct disk *disk;
	uint8_t present;
	uint8_t bank;
	uint8_t drive;
	uint8_t use_lba;
	/* Native geometry from IDENTIFY, for the CHS command fallback. */
	uint16_t native_cylinders;
	uint16_t native_heads;
	uint16_t native_sectors;
	uint16_t firmware_heads;
	uint16_t firmware_sectors;
};

static struct ide_unit units[IDE_UNIT_MAX];
static struct ide_unit *unit_order[IDE_UNIT_MAX];
static unsigned unit_count;

static uint8_t
inb(uint16_t port)
{
	uint8_t value;

	__asm__ volatile ("inb %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
outb(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0, %w1" : : "a"(value), "Nd"(port));
}

static uint16_t
inw(uint16_t port)
{
	uint16_t value;

	__asm__ volatile ("inw %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
outw(uint16_t port, uint16_t value)
{
	__asm__ volatile ("outw %0, %w1" : : "a"(value), "Nd"(port));
}

static void
select_bank(uint8_t bank)
{
	/* Bit 3 would enable 32-bit data transfers; stay on 16-bit PIO. */
	outb(IDE_BANK_SELECT, bank & 1U);
}

/* Reading the alternate status four times gives the 400ns settle time
 * the ATA specification requires after a drive-select. */
static void
select_delay(void)
{
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
}

static int
wait_clear(uint8_t mask)
{
	uint32_t spins;

	for (spins = 0; spins < IDE_TIMEOUT_SPINS; spins++)
		if (!(inb(IDE_ALT_STATUS) & mask))
			return 1;
	return 0;
}

/* Wait for BSY to drop and DRQ to rise; fails on ERR or timeout. */
static int
wait_drq(void)
{
	uint32_t spins;

	for (spins = 0; spins < IDE_TIMEOUT_SPINS; spins++) {
		uint8_t status = inb(IDE_ALT_STATUS);

		if (status & IDE_STATUS_BSY)
			continue;
		if (status & IDE_STATUS_ERR)
			return 0;
		if (status & IDE_STATUS_DRQ)
			return 1;
	}
	return 0;
}

static int
select_unit(const struct ide_unit *unit, uint8_t head_bits, int lba)
{
	select_bank(unit->bank);
	if (!wait_clear(IDE_STATUS_BSY | IDE_STATUS_DRQ))
		return 0;
	outb(IDE_DRIVE_HEAD, (uint8_t)(0xa0U | (lba ? 0x40U : 0U) |
				       ((unit->drive & 1U) << 4) |
				       (head_bits & 0x0fU)));
	select_delay();
	return wait_clear(IDE_STATUS_BSY);
}

/*
 * Program the address registers for one chunk.  LBA28 when the drive
 * supports it, CHS in the drive's native geometry otherwise.
 */
static int
setup_transfer(const struct ide_unit *unit, uint64_t lba, uint32_t count)
{
	if (unit->use_lba) {
		if (!select_unit(unit, (uint8_t)((lba >> 24) & 0x0fU), 1))
			return 0;
		outb(IDE_NSECT, (uint8_t)count);
		outb(IDE_LBA_LOW, (uint8_t)lba);
		outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
		outb(IDE_LBA_HIGH, (uint8_t)(lba >> 16));
	} else {
		uint32_t spt = unit->native_sectors;
		uint32_t heads = unit->native_heads;
		uint32_t sect = (uint32_t)(lba % spt) + 1U;
		uint32_t head = (uint32_t)((lba / spt) % heads);
		uint32_t cyl = (uint32_t)(lba / ((uint64_t)spt * heads));

		if (!select_unit(unit, (uint8_t)head, 0))
			return 0;
		outb(IDE_NSECT, (uint8_t)count);
		outb(IDE_LBA_LOW, (uint8_t)sect);
		outb(IDE_LBA_MID, (uint8_t)cyl);
		outb(IDE_LBA_HIGH, (uint8_t)(cyl >> 8));
	}
	return 1;
}

static int
pio_read(struct disk *dev, uint64_t lba, uint32_t count, void *buffer)
{
	struct ide_unit *unit = dev->d_data;
	uint16_t *out = buffer;

	while (count > 0) {
		/* nsect is 8-bit; 0 would mean 256, keep chunks explicit. */
		uint32_t chunk = count > 255U ? 255U : count;
		uint32_t sector;

		if (!setup_transfer(unit, lba, chunk))
			return EIO;
		outb(IDE_STATUS, IDE_CMD_READ);
		for (sector = 0; sector < chunk; sector++) {
			unsigned word;

			if (!wait_drq())
				return EIO;
			for (word = 0; word < 256; word++)
				*out++ = inw(IDE_DATA);
		}
		lba += chunk;
		count -= chunk;
	}
	return 0;
}

static int
pio_write(struct disk *dev, uint64_t lba, uint32_t count,
	  const void *buffer)
{
	struct ide_unit *unit = dev->d_data;
	const uint16_t *in = buffer;

	while (count > 0) {
		uint32_t chunk = count > 255U ? 255U : count;
		uint32_t sector;

		if (!setup_transfer(unit, lba, chunk))
			return EIO;
		outb(IDE_STATUS, IDE_CMD_WRITE);
		for (sector = 0; sector < chunk; sector++) {
			unsigned word;

			if (!wait_drq())
				return EIO;
			for (word = 0; word < 256; word++)
				outw(IDE_DATA, *in++);
		}
		if (!wait_clear(IDE_STATUS_BSY) ||
		    (inb(IDE_ALT_STATUS) & IDE_STATUS_ERR))
			return EIO;
		lba += chunk;
		count -= chunk;
	}
	return 0;
}

static int
pc98_ide_submit(struct disk *dev, struct bio *bio)
{
	int error;
	size_t transferred = 0;

	if (bio->b_op == BIO_READ)
		error = pio_read(dev, bio->b_mapped_block,
				 bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_WRITE)
		error = pio_write(dev, bio->b_mapped_block,
				  bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_FLUSH)
		error = 0; /* PIO completion is synchronous on supported drives. */
	else
		return EOPNOTSUPP;
	if (error == 0)
		transferred = (size_t)bio->b_block_count * dev->d_block_size;
	bio_complete(bio, error, transferred);
	return 0;
}

static int
pc98_ide_ioctl(struct disk *dev, unsigned long request, void *argument)
{
	struct ide_unit *unit = dev->d_data;
	struct disk_geometry *geometry = argument;

	if (request != DISK_IOCTL_GET_GEOMETRY)
		return EOPNOTSUPP;
	if (geometry == NULL)
		return EINVAL;
	geometry->cylinders = unit->native_cylinders;
	geometry->heads = unit->native_heads;
	geometry->sectors_per_track = unit->native_sectors;
	/* Firmware geometry, when present, is stored separately below. */
	if (unit->firmware_heads != 0) {
		geometry->heads = unit->firmware_heads;
		geometry->sectors_per_track = unit->firmware_sectors;
	}
	return 0;
}

static const struct disk_ops pc98_ide_disk_ops = {
	.submit = pc98_ide_submit,
	.ioctl = pc98_ide_ioctl,
};

static int
identify(uint8_t bank, uint8_t drive, uint16_t data[256])
{
	struct ide_unit probe;
	unsigned word;
	uint8_t status;

	probe.bank = bank;
	probe.drive = drive;
	select_bank(bank);
	status = inb(IDE_ALT_STATUS);
	/* A floating bus reads 0xff on both units: nothing on this bank. */
	if (status == 0xffU)
		return 0;
	if (!select_unit(&probe, 0, 0))
		return 0;
	/* Selecting an absent device makes its sibling answer (or the bus
	 * float); the signature check below rejects both cases. */
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN);
	outb(IDE_STATUS, IDE_CMD_IDENTIFY);
	status = inb(IDE_ALT_STATUS);
	if (status == 0 || status == 0xffU)
		return 0;
	if (!wait_drq())
		return 0;
	for (word = 0; word < 256; word++)
		data[word] = inw(IDE_DATA);
	/* Word 0 bit 15 clear identifies an ATA (not ATAPI) device. */
	if (data[0] & 0x8000U)
		return 0;
	return 1;
}

static void
set_name(struct ide_unit *unit, unsigned ordinal)
{
	unit->disk->d_name[0] = 'i';
	unit->disk->d_name[1] = 'd';
	unit->disk->d_name[2] = 'e';
	unit->disk->d_name[3] = (char)('0' + ordinal);
	unit->disk->d_name[4] = '\0';
	unit->disk->d_name[DISK_NAME_MAX - 1U] = '\0';
}

unsigned
boots_ide_pc98_init(const struct boots_device *bios_devices,
		     unsigned bios_device_count)
{
	static uint16_t data[256];
	unsigned slot;
	uint8_t bank;
	uint8_t drive;

	unit_count = 0;
	for (slot = 0; slot < IDE_UNIT_MAX; slot++) {
		units[slot].present = 0;
		units[slot].disk = NULL;
		unit_order[slot] = NULL;
	}
	for (bank = 0; bank < 2; bank++) {
		for (drive = 0; drive < 2; drive++) {
			struct ide_unit *unit;
			const struct boots_device *bios_dev = NULL;
			uint64_t sector_count;
			unsigned i;

			slot = (unsigned)bank * 2U + drive;
			unit = &units[slot];
			if (!identify(bank, drive, data))
				continue;
			unit->bank = bank;
			unit->drive = drive;
			unit->native_cylinders = data[1];
			unit->native_heads = data[3];
			unit->native_sectors = data[6];
			unit->use_lba = (data[49] & 0x0200U) != 0;
			if (unit->use_lba) {
				sector_count =
					(uint32_t)data[60] |
					((uint32_t)data[61] << 16);
			} else {
				sector_count =
					(uint64_t)data[1] * data[3] * data[6];
			}
			if (sector_count == 0 ||
			    (!unit->use_lba &&
			     (unit->native_heads == 0 ||
			      unit->native_sectors == 0)))
				continue;
			unit->disk = disk_alloc();
			if (unit->disk == NULL)
				continue;
			unit->disk->d_block_count = sector_count;
			unit->disk->d_block_size = 512;
			unit->disk->d_max_transfer_blocks = 255;
			unit->disk->d_ops = &pc98_ide_disk_ops;
			unit->disk->d_data = unit;
			set_name(unit, unit_count);
			/*
			 * Partition tables are written in the firmware-sensed
			 * geometry, so prefer it over IDENTIFY.  The firmware
			 * enumerates IDE disks in the same bank-major order
			 * this probe uses, so BIOS ID 80h+slot is an exact pairing.
			 */
			for (i = 0; bios_devices != NULL && i < bios_device_count;
			     i++) {
				if (bios_devices[i].device_class == BOOTS_DEV_IDE &&
				    bios_devices[i].bios_id == 0x80U + slot) {
					bios_dev = &bios_devices[i];
					break;
				}
			}
			if (bios_dev != NULL && bios_dev->heads != 0 &&
			    bios_dev->sectors != 0) {
				unit->firmware_heads = bios_dev->heads;
				unit->firmware_sectors = bios_dev->sectors;
			} else {
				unit->firmware_heads = unit->native_heads;
				unit->firmware_sectors = unit->native_sectors;
			}
			if (disk_create(unit->disk) == 0) {
				unit->present = 1;
				unit_order[unit_count] = unit;
				unit_count++;
			}
		}
	}
	return unit_count;
}

struct disk *
boots_ide_pc98_unit(unsigned ordinal)
{
	return ordinal < unit_count ? unit_order[ordinal]->disk : NULL;
}

struct disk *
boots_ide_pc98_bios_unit(uint8_t bios_id)
{
	unsigned slot;

	if (bios_id < 0x80U || bios_id >= 0x80U + IDE_UNIT_MAX)
		return NULL;
	slot = bios_id - 0x80U;
	return units[slot].present ? units[slot].disk : NULL;
}
