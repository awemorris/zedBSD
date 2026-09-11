/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC/AT ATA PIO driver
 */

#include "drivers/pcat-ide.h"
#include <errno.h>
#include <kern/lock.h>
#include "kern/klog.h"

#define ATA_DATA 0U
#define ATA_ERROR 1U
#define ATA_NSECT 2U
#define ATA_LBA0 3U
#define ATA_LBA1 4U
#define ATA_LBA2 5U
#define ATA_DEVICE 6U
#define ATA_STATUS 7U
#define ATA_COMMAND 7U
#define ATA_BSY 0x80U
#define ATA_DF 0x20U
#define ATA_DRQ 0x08U
#define ATA_ERR 0x01U
#define ATA_IDENTIFY 0xecU
#define ATA_READ 0x20U
#define ATA_WRITE 0x30U
#define ATA_FLUSH 0xe7U
#define ATA_TIMEOUT 5000000U
#define ATA_UNIT_MAX 4U

struct ata_unit {
	struct disk *disk;
	uint16_t io, control;
	uint8_t drive, slot, present;
	uint16_t cylinders, heads, sectors;
};

static struct ata_unit units[ATA_UNIT_MAX];
static struct ata_unit *order[ATA_UNIT_MAX];
/* ATA task-file registers are shared by master and slave on each channel. */
static struct mutex channel_locks[2];
static unsigned present_count;

static int identify(struct ata_unit *unit, uint16_t data[256]);
static void outb(uint16_t port, uint8_t value);
static void delay400(const struct ata_unit *unit);
static uint8_t inb(uint16_t port);
static int wait_not_busy(const struct ata_unit *unit);
static int wait_drq(const struct ata_unit *unit);
static uint16_t inw(uint16_t port);
static void outw(uint16_t port, uint16_t value);
static int select_unit(const struct ata_unit *unit, uint32_t lba);
static int setup(const struct ata_unit *unit, uint32_t lba, uint8_t count);
static int transfer(struct ata_unit *unit, int write, uint64_t block, uint32_t count, void *buffer);
static int flush(struct ata_unit *unit);
static int ata_submit(struct disk *disk, struct bio *bio);
static int ata_ioctl(struct disk *disk, unsigned long request, void *argument);

static const struct disk_ops ata_ops = {.submit = ata_submit,
					.ioctl = ata_ioctl};

/*
 * Implements the drv pcat ide init operation.
 */
unsigned
drv_pcat_ide_init(
	void)
{
	struct ata_unit *unit;
	uint32_t sectors;
	unsigned slot_for;
	static uint16_t data[256];

	(void)mutex_init(&channel_locks[0], LOCK_RANK_DISK, "ata-primary");
	(void)mutex_init(&channel_locks[1], LOCK_RANK_DISK, "ata-secondary");
	present_count = 0;
	/* Process each element required by the operation. */
	for (slot_for = 0; slot_for < ATA_UNIT_MAX; slot_for++) {
		unit = &units[slot_for];

		/* Describes where this slot's registers live. */
		unit->slot = (uint8_t)slot_for;
		unit->drive = (uint8_t)(slot_for & 1U);
		unit->io = slot_for < 2 ? 0x1f0U : 0x170U;
		unit->control = slot_for < 2 ? 0x3f6U : 0x376U;
		unit->present = 0;
		unit->disk = 0;
		order[slot_for] = 0;

		/* Checks the identify result. */
		if (!identify(unit, data))
			continue;

		/* Handles the sectors condition. */
		sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
		if (sectors == 0)
			continue;
		unit->disk = disk_alloc();

		/* Handles the unit condition. */
		if (unit->disk == 0)
			continue;

		/* Checks the disk alloc sd name result. */
		if (disk_alloc_sd_name(unit->disk) != 0) {
			(void)disk_destroy(unit->disk);
			unit->disk = 0;
			continue;
		}

		unit->cylinders = data[1];
		unit->heads = data[3];
		unit->sectors = data[6];
		unit->disk->d_block_size = 512;
		unit->disk->d_block_count = sectors;
		unit->disk->d_max_transfer_blocks = 255;
		unit->disk->d_ops = &ata_ops;
		unit->disk->d_data = unit;

		/* Checks the disk create result. */
		if (disk_create(unit->disk) != 0)
			continue;
		unit->present = 1;
		order[present_count++] = unit;
		kern_logf("ata: %s blocks=%u CHS=%u/%u/%u\n",
			   unit->disk->d_name, sectors, unit->cylinders,
			   unit->heads, unit->sectors);
	}

	/* Returns the computed result. */
	return present_count;
}

/*
 * Implements the drv pcat ide unit operation.
 */
struct disk *
drv_pcat_ide_unit(
	unsigned ordinal)
{
	/* Returns the computed result. */
	return ordinal < present_count ? order[ordinal]->disk : 0;
}

/*
 * Implements the drv pcat ide bios unit operation.
 */
struct disk *
drv_pcat_ide_bios_unit(
	uint8_t bios_id)
{
	unsigned slot;

	/* Handles the bios id condition. */
	if (bios_id < 0x80U || bios_id >= 0x84U)
		return 0;
	slot = bios_id - 0x80U;

	/* Returns the computed result. */
	return units[slot].present ? units[slot].disk : 0;
}

/* Supports the identify operation. */
static int
identify(
	struct ata_unit *unit,
	uint16_t data[256])
{
	unsigned word_for;
	uint8_t status;

	/* Selects the drive and issues the identify command. */
	outb(unit->control, 0x02U);
	outb(unit->io + ATA_DEVICE, (uint8_t)(0xa0U | (unit->drive << 4)));
	delay400(unit);
	outb(unit->io + ATA_NSECT, 0);
	outb(unit->io + ATA_LBA0, 0);
	outb(unit->io + ATA_LBA1, 0);
	outb(unit->io + ATA_LBA2, 0);
	outb(unit->io + ATA_COMMAND, ATA_IDENTIFY);

	/* Checks the wait not busy result. */
	status = inb(unit->io + ATA_STATUS);
	if (status == 0 || status == 0xffU || !wait_not_busy(unit))
		return 0;

	/* Checks the inb result. */
	if (inb(unit->io + ATA_LBA1) != 0 || inb(unit->io + ATA_LBA2) != 0)
		return 0;

	/* Checks the wait drq result. */
	if (!wait_drq(unit))
		return 0;
	/* Process each element required by the operation. */
	for (word_for = 0; word_for < 256U; word_for++)
		data[word_for] = inw(unit->io);

	/* Returns the computed result. */
	return !(data[0] & 0x8000U) && (data[49] & 0x0200U);
}

/* Supports the outb operation. */
static void
outb(
	uint16_t port,
	uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the delay400 operation. */
static void
delay400(
	const struct ata_unit *unit)
{
	(void)inb(unit->control);
	(void)inb(unit->control);
	(void)inb(unit->control);
	(void)inb(unit->control);
}

/* Supports the inb operation. */
static uint8_t
inb(
	uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the wait not busy operation. */
static int
wait_not_busy(
	const struct ata_unit *unit)
{
	uint8_t status;
	uint32_t spin_for;

	/* Process each element required by the operation. */
	for (spin_for = 0; spin_for < ATA_TIMEOUT; spin_for++) {
		/* Checks the operation status. */
		status = inb(unit->control);
		if (status == 0xffU)
			return 0;
		if (!(status & ATA_BSY))
			return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the wait drq operation. */
static int
wait_drq(
	const struct ata_unit *unit)
{
	uint8_t status;
	uint32_t spin_for;

	/* Process each element required by the operation. */
	for (spin_for = 0; spin_for < ATA_TIMEOUT; spin_for++) {
		/* Checks the operation status. */
		status = inb(unit->control);
		if (status == 0xffU || (status & (ATA_DF | ATA_ERR)))
			return 0;
		if (!(status & ATA_BSY) && (status & ATA_DRQ))
			return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the inw operation. */
static uint16_t
inw(
	uint16_t port)
{
	uint16_t value;

	__asm__ volatile("inw %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the outw operation. */
static void
outw(
	uint16_t port,
	uint16_t value)
{
	__asm__ volatile("outw %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the select unit operation. */
static int
select_unit(
	const struct ata_unit *unit,
	uint32_t lba)
{
	int error;

	/* Checks the wait not busy result. */
	if (!wait_not_busy(unit))
		return 0;
	outb(unit->io + ATA_DEVICE,
	     (uint8_t)(0xe0U | (unit->drive << 4) | ((lba >> 24) & 0x0fU)));
	delay400(unit);

	/* Obtains the wait not busy result. */
	error = wait_not_busy(unit);

	/* Returns the computed result. */
	return error;
}

/* Supports the setup operation. */
static int
setup(
	const struct ata_unit *unit,
	uint32_t lba,
	uint8_t count)
{
	/* Checks the select unit result. */
	if (!select_unit(unit, lba))
		return 0;
	outb(unit->io + ATA_NSECT, count);
	outb(unit->io + ATA_LBA0, (uint8_t)lba);
	outb(unit->io + ATA_LBA1, (uint8_t)(lba >> 8));
	outb(unit->io + ATA_LBA2, (uint8_t)(lba >> 16));

	/* Reports operation failure. */
	return 1;
}

/* Supports the transfer operation. */
static int
transfer(
	struct ata_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	void *buffer)
{
	uint32_t chunk;
	uint32_t sector_for;
	unsigned word_for;
	uint16_t *words = buffer;

	/* Handles the block condition. */
	if (block >= 0x10000000ULL || count > 0x10000000ULL - block)
		return EINVAL;
	/* Process each remaining element. */
	while (count != 0) {
		/* Checks the setup result. */
		chunk = count > 255U ? 255U : count;
		if (!setup(unit, (uint32_t)block, (uint8_t)chunk))
			return EIO;
		outb(unit->io + ATA_COMMAND, write ? ATA_WRITE : ATA_READ);
		/* Process each element required by the operation. */
		for (sector_for = 0; sector_for < chunk; sector_for++) {
			/* Checks the wait drq result. */
			if (!wait_drq(unit))
				return EIO;
			/* Process each element required by the operation. */
			for (word_for = 0; word_for < 256U; word_for++) {
				/* Handles the write condition. */
				if (write)
					outw(unit->io + ATA_DATA, *words++);
				else
					*words++ = inw(unit->io + ATA_DATA);
			}
		}

		/* Checks the wait not busy result. */
		if (write && !wait_not_busy(unit))
			return EIO;
		block += chunk;
		count -= chunk;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the flush operation. */
static int
flush(
	struct ata_unit *unit)
{
	int error;

	/* Checks the select unit result. */
	if (!select_unit(unit, 0))
		return EIO;
	outb(unit->io + ATA_COMMAND, ATA_FLUSH);

	/* Checks the wait not busy result. */
	if (!wait_not_busy(unit))
		return EIO;

	/* Computes the function result. */
	error =
		(inb(unit->io + ATA_STATUS) & (ATA_DF | ATA_ERR)) ? EIO : 0;

	/* Returns the computed result. */
	return error;
}

/* Supports the ata submit operation. */
static int
ata_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct ata_unit *unit = disk->d_data;
	struct mutex *channel_lock = &channel_locks[unit->slot / 2U];
	int error;

	mutex_lock(channel_lock);

	/* Handles the bio condition. */
	if (bio->b_op == BIO_READ) {
		error = transfer(unit, 0, bio->b_mapped_block,
				 bio->b_block_count, bio->b_data);
	} else if (bio->b_op == BIO_WRITE) {
		error = transfer(unit, 1, bio->b_mapped_block,
				 bio->b_block_count, bio->b_data);
	} else if (bio->b_op == BIO_FLUSH)
		error = flush(unit);
	else
		error = EOPNOTSUPP;
	if (error != 0) {
		kern_logf(
			"ata: %s op=%u lba=%u count=%u error=%d status=%02X\n",
			disk->d_name, (unsigned)bio->b_op,
			(uint32_t)bio->b_mapped_block, bio->b_block_count,
			error, inb(unit->io + ATA_STATUS));
	}

	mutex_unlock(channel_lock);

	bio_complete(bio, error,
		     error == 0
			     ? (size_t)bio->b_block_count * disk->d_block_size
			     : 0);

	/* Succeeded. */
	return 0;
}

/* Supports the ata ioctl operation. */
static int
ata_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	struct ata_unit *unit = disk->d_data;
	struct disk_geometry *geometry = argument;

	/* Handles the request condition. */
	if (request != DISK_IOCTL_GET_GEOMETRY)
		return EOPNOTSUPP;

	/* Handles the geometry condition. */
	if (geometry == 0)
		return EINVAL;
	geometry->cylinders = unit->cylinders;
	geometry->heads = unit->heads;
	geometry->sectors_per_track = unit->sectors;

	/* Succeeded. */
	return 0;
}
