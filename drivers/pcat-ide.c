/*
 * PC/AT ATA PIO driver
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/pcat-ide.h"
#include <errno.h>
#include <hal/hal.h>
#include <kern/lock.h>

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

static uint8_t inb(uint16_t port)
{
	uint8_t value; __asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}
static uint16_t inw(uint16_t port)
{
	uint16_t value; __asm__ volatile("inw %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}
static void outb(uint16_t port, uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}
static void outw(uint16_t port, uint16_t value)
{
	__asm__ volatile("outw %0,%w1" : : "a"(value), "Nd"(port));
}

static void delay400(const struct ata_unit *unit)
{
	(void)inb(unit->control); (void)inb(unit->control);
	(void)inb(unit->control); (void)inb(unit->control);
}

static int wait_not_busy(const struct ata_unit *unit)
{
	for (uint32_t spin = 0; spin < ATA_TIMEOUT; spin++) {
		uint8_t status = inb(unit->control);
		if (status == 0xffU) return 0;
		if (!(status & ATA_BSY)) return 1;
	}
	return 0;
}

static int wait_drq(const struct ata_unit *unit)
{
	for (uint32_t spin = 0; spin < ATA_TIMEOUT; spin++) {
		uint8_t status = inb(unit->control);
		if (status == 0xffU || (status & (ATA_DF | ATA_ERR))) return 0;
		if (!(status & ATA_BSY) && (status & ATA_DRQ)) return 1;
	}
	return 0;
}

static int select_unit(const struct ata_unit *unit, uint32_t lba)
{
	if (!wait_not_busy(unit)) return 0;
	outb(unit->io + ATA_DEVICE, (uint8_t)(0xe0U | (unit->drive << 4) |
	    ((lba >> 24) & 0x0fU)));
	delay400(unit);
	return wait_not_busy(unit);
}

static int setup(const struct ata_unit *unit, uint32_t lba, uint8_t count)
{
	if (!select_unit(unit, lba)) return 0;
	outb(unit->io + ATA_NSECT, count);
	outb(unit->io + ATA_LBA0, (uint8_t)lba);
	outb(unit->io + ATA_LBA1, (uint8_t)(lba >> 8));
	outb(unit->io + ATA_LBA2, (uint8_t)(lba >> 16));
	return 1;
}

static int transfer(struct ata_unit *unit, int write, uint64_t block,
    uint32_t count, void *buffer)
{
	uint16_t *words = buffer;
	if (block >= 0x10000000ULL || count > 0x10000000ULL - block)
		return EINVAL;
	while (count != 0) {
		uint32_t chunk = count > 255U ? 255U : count;
		if (!setup(unit, (uint32_t)block, (uint8_t)chunk)) return EIO;
		outb(unit->io + ATA_COMMAND, write ? ATA_WRITE : ATA_READ);
		for (uint32_t sector = 0; sector < chunk; sector++) {
			if (!wait_drq(unit)) return EIO;
			for (unsigned word = 0; word < 256U; word++) {
				if (write) outw(unit->io + ATA_DATA, *words++);
				else *words++ = inw(unit->io + ATA_DATA);
			}
		}
		if (write && !wait_not_busy(unit)) return EIO;
		block += chunk; count -= chunk;
	}
	return 0;
}

static int flush(struct ata_unit *unit)
{
	if (!select_unit(unit, 0)) return EIO;
	outb(unit->io + ATA_COMMAND, ATA_FLUSH);
	if (!wait_not_busy(unit)) return EIO;
	return (inb(unit->io + ATA_STATUS) & (ATA_DF | ATA_ERR)) ? EIO : 0;
}

static int ata_submit(struct disk *disk, struct bio *bio)
{
	struct ata_unit *unit = disk->d_data;
	struct mutex *channel_lock = &channel_locks[unit->slot / 2U];
	int error;
	mutex_lock(channel_lock);
	if (bio->b_op == BIO_READ)
		error = transfer(unit, 0, bio->b_mapped_block,
		    bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_WRITE)
		error = transfer(unit, 1, bio->b_mapped_block,
		    bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_FLUSH)
		error = flush(unit);
	else error = EOPNOTSUPP;
	if (error != 0)
		hal_printf("ata: %s op=%u lba=%u count=%u error=%d status=%02X\n",
		    disk->d_name, (unsigned)bio->b_op,
		    (uint32_t)bio->b_mapped_block, bio->b_block_count, error,
		    inb(unit->io + ATA_STATUS));
	mutex_unlock(channel_lock);
	bio_complete(bio, error, error == 0 ?
	    (size_t)bio->b_block_count * disk->d_block_size : 0);
	return 0;
}

static int ata_ioctl(struct disk *disk, unsigned long request, void *argument)
{
	struct ata_unit *unit = disk->d_data;
	struct disk_geometry *geometry = argument;
	if (request != DISK_IOCTL_GET_GEOMETRY) return EOPNOTSUPP;
	if (geometry == 0) return EINVAL;
	geometry->cylinders = unit->cylinders;
	geometry->heads = unit->heads;
	geometry->sectors_per_track = unit->sectors;
	return 0;
}

static const struct disk_ops ata_ops = { .submit = ata_submit, .ioctl = ata_ioctl };

static int identify(struct ata_unit *unit, uint16_t data[256])
{
	uint8_t status;
	outb(unit->control, 0x02U);
	outb(unit->io + ATA_DEVICE, (uint8_t)(0xa0U | (unit->drive << 4)));
	delay400(unit);
	outb(unit->io + ATA_NSECT, 0); outb(unit->io + ATA_LBA0, 0);
	outb(unit->io + ATA_LBA1, 0); outb(unit->io + ATA_LBA2, 0);
	outb(unit->io + ATA_COMMAND, ATA_IDENTIFY);
	status = inb(unit->io + ATA_STATUS);
	if (status == 0 || status == 0xffU || !wait_not_busy(unit)) return 0;
	if (inb(unit->io + ATA_LBA1) != 0 || inb(unit->io + ATA_LBA2) != 0)
		return 0;
	if (!wait_drq(unit)) return 0;
	for (unsigned word = 0; word < 256U; word++) data[word] = inw(unit->io);
	return !(data[0] & 0x8000U) && (data[49] & 0x0200U);
}

unsigned zedbsd_ide_pcat_init(void)
{
	static uint16_t data[256];
	(void)mutex_init(&channel_locks[0], LOCK_RANK_DISK, "ata-primary");
	(void)mutex_init(&channel_locks[1], LOCK_RANK_DISK, "ata-secondary");
	present_count = 0;
	for (unsigned slot = 0; slot < ATA_UNIT_MAX; slot++) {
		struct ata_unit *unit = &units[slot];
		uint32_t sectors;
		unit->slot = (uint8_t)slot; unit->drive = (uint8_t)(slot & 1U);
		unit->io = slot < 2 ? 0x1f0U : 0x170U;
		unit->control = slot < 2 ? 0x3f6U : 0x376U;
		unit->present = 0; unit->disk = 0; order[slot] = 0;
		if (!identify(unit, data)) continue;
		sectors = (uint32_t)data[60] | ((uint32_t)data[61] << 16);
		if (sectors == 0) continue;
		unit->disk = disk_alloc(); if (unit->disk == 0) continue;
		unit->cylinders = data[1]; unit->heads = data[3]; unit->sectors = data[6];
		unit->disk->d_name[0]='i'; unit->disk->d_name[1]='d';
		unit->disk->d_name[2]='e'; unit->disk->d_name[3]=(char)('0'+slot);
		unit->disk->d_name[4]='\0'; unit->disk->d_block_size=512;
		unit->disk->d_block_count=sectors; unit->disk->d_max_transfer_blocks=255;
		unit->disk->d_ops=&ata_ops; unit->disk->d_data=unit;
		if (disk_create(unit->disk) != 0) continue;
		unit->present=1; order[present_count++]=unit;
		hal_printf("ata: %s blocks=%u CHS=%u/%u/%u\n", unit->disk->d_name,
		    sectors, unit->cylinders, unit->heads, unit->sectors);
	}
	return present_count;
}

struct disk *zedbsd_ide_pcat_unit(unsigned ordinal)
{
	return ordinal < present_count ? order[ordinal]->disk : 0;
}

struct disk *zedbsd_ide_pcat_bios_unit(uint8_t bios_id)
{
	unsigned slot;
	if (bios_id < 0x80U || bios_id >= 0x84U) return 0;
	slot = bios_id - 0x80U;
	return units[slot].present ? units[slot].disk : 0;
}
