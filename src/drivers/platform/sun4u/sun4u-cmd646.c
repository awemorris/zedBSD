/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CMD646 IDE controller's primary channel, driven by programmed I/O.
 *
 * The first drive of the channel is the disk: one 512-byte sector per
 * command, polled to completion, addressed by 28-bit LBA.
 */

#include "drivers/platform/sun4u/sun4u-cmd646.h"

#include <kern/disk.h>
#include <uapi/sysctl.h>

#include <uapi/errno.h>
#include "kern/klog.h"
#include "kern/device-io.h"

/*
 * The ATA command block registers, by their offset, and the control
 * block's device control register.
 *
 * Values referenced: ATA/ATAPI-6 (INCITS 361-2002), the command and
 * control block registers, the status bits, the device control bits and
 * the commands IDENTIFY DEVICE, READ SECTORS, WRITE SECTORS and FLUSH
 * CACHE.
 */
#define ATA_DATA		0U
#define ATA_ERROR		1U
#define ATA_COUNT		2U
#define ATA_LBA0		3U
#define ATA_LBA1		4U
#define ATA_LBA2		5U
#define ATA_DRIVE		6U
#define ATA_STATUS		7U
#define ATA_COMMAND		7U
#define ATA_DEVICE_CONTROL	2U

/* The status bits. */
#define ATA_BSY			0x80U
#define ATA_DRDY		0x40U
#define ATA_DRQ			0x08U
#define ATA_ERR			0x01U

/* The device control bits: software reset, and interrupts off. */
#define ATA_CONTROL_RESET	0x04U
#define ATA_CONTROL_NIEN	0x02U

/* The drive register: the first drive, and the first drive by LBA. */
#define ATA_DRIVE_MASTER	0xa0U
#define ATA_DRIVE_MASTER_LBA	0xe0U

/* The commands. */
#define ATA_CMD_IDENTIFY	0xecU
#define ATA_CMD_READ_SECTORS	0x20U
#define ATA_CMD_WRITE_SECTORS	0x30U
#define ATA_CMD_FLUSH_CACHE	0xe7U

/* The identify data's words 60 and 61: the sectors addressable by 28-bit LBA. */
#define ATA_IDENTIFY_LBA_LOW	60U
#define ATA_IDENTIFY_LBA_HIGH	61U

/* A sector, and the highest LBA a 28-bit command reaches. */
#define ATA_SECTOR_SIZE		512U
#define ATA_LBA28_LIMIT		0x10000000ULL

/* How long a status wait polls, and how long the reset pulse lasts. */
#define ATA_STATUS_SPINS	10000000UL
#define ATA_RESET_SPINS		100000U

/*
 * The channel's command and control block ports.
 *
 * Set once by drv_sun4u_cmd646_init() and constant afterwards; the disk
 * layer issues one request at a time, so the ports are never shared.
 */
static uint16_t command_block;
static uint16_t control_block;

/*
 * The published disk, or NULL until the drive was found; and the number
 * of sectors its identify data reported.
 */
static struct disk *ata_disk;
static uint64_t ata_sectors;

static int wait_status(uint8_t set, uint8_t clear);
static void log_failure(const char *step, int error);
static int identify(void);
static void select_lba(uint32_t lba);
static int transfer_sector(int write, uint32_t lba, uint8_t *data);
static int flush_cache(void);
static int submit(struct disk *disk, struct bio *request);
static int ioctl(struct disk *disk, unsigned long request, void *argument);

/*
 * The disk layer's entry points for the drive.
 */
static const struct disk_ops ops = {
	.submit = submit,
	.ioctl = ioctl
};

/*
 * Resets the channel, identifies its first drive and publishes it as a
 * disk.
 */
int
drv_sun4u_cmd646_init(
	uint16_t command_port,
	uint16_t control_port)
{
	volatile unsigned spin;
	int error;

	/* Keeps the channel's ports; no disk is published yet. */
	command_block = command_port;
	control_block = control_port;
	ata_disk = NULL;

	/* Pulses the software reset, then leaves interrupts off for polling. */
	kern_io_out8(control_block + ATA_DEVICE_CONTROL, ATA_CONTROL_RESET);
	for (spin = 0; spin < ATA_RESET_SPINS; spin++)
		continue;
	kern_io_out8(control_block + ATA_DEVICE_CONTROL, ATA_CONTROL_NIEN);

	/* Selects the first drive and waits for it to be ready. */
	kern_io_out8(command_block + ATA_DRIVE, ATA_DRIVE_MASTER);
	error = wait_status(ATA_DRDY, ATA_BSY);
	if (error != 0) {
		log_failure("reset", error);
		return error;
	}

	/* Reads the drive's size. */
	error = identify();
	if (error != 0) {
		log_failure("identify", error);
		return error;
	}

	/* Allocates the disk's record. */
	ata_disk = disk_alloc();
	if (ata_disk == NULL)
		return ENOMEM;

	/* Names the disk as an sd disk. */
	error = disk_alloc_sd_name(ata_disk);
	if (error != 0) {
		(void)disk_destroy(ata_disk);
		ata_disk = NULL;
		return error;
	}

	/* Describes the disk: 512-byte sectors, one per request, listed as IDE. */
	ata_disk->d_block_size = ATA_SECTOR_SIZE;
	ata_disk->d_block_count = ata_sectors;
	ata_disk->d_max_transfer_blocks = 1;
	ata_disk->d_ops = &ops;
	disk_set_stats_kind(ata_disk, DISK_STATS_KIND_IDE);

	/* Publishes the disk. */
	error = disk_create(ata_disk);
	if (error != 0) {
		ata_disk = NULL;
		return error;
	}
	kern_logf("cmd646: sectors=%lu\n", (unsigned long)ata_sectors);

	/* Succeeded: the disk is published. */
	return 0;
}

/*
 * Reports the published disk, or NULL when none was found.
 */
struct disk *
drv_sun4u_cmd646_disk(
	void)
{
	/* Succeeded: the disk drv_sun4u_cmd646_init() published. */
	return ata_disk;
}

/* Waits until the status shows every bit of set and none of clear. */
static int
wait_status(
	uint8_t set,
	uint8_t clear)
{
	unsigned long spins;
	uint8_t status;

	/* Polls the status until it matches, the drive reports an error, or the time runs out. */
	for (spins = 0; spins < ATA_STATUS_SPINS; spins++) {
		/* A drive that reports an error has failed the command. */
		status = kern_io_in8(command_block + ATA_STATUS);
		if ((status & ATA_ERR) != 0)
			return EIO;

		/* The wait ends when the wanted bits are set and the unwanted ones clear. */
		if ((status & set) != set)
			continue;
		if ((status & clear) != 0)
			continue;
		return 0;
	}

	/* Failed: the drive never reached the state. */
	return ETIMEDOUT;
}

/* Logs a failed step with the drive's status and error registers. */
static void
log_failure(
	const char *step,
	int error)
{
	uint8_t status;
	uint8_t ata_error;

	/* Reads the registers that say why. */
	status = kern_io_in8(command_block + ATA_STATUS);
	ata_error = kern_io_in8(command_block + ATA_ERROR);

	/* Logs the step and the registers. */
	kern_logf("cmd646: %s error=%d status=%x ata=%x\n", step, error, status, ata_error);
}

/* Reads the first drive's identify data and keeps its size. */
static int
identify(
	void)
{
	uint16_t words[256];
	uint8_t status;
	unsigned index;
	int error;

	/* Selects the drive and issues the identify command. */
	kern_io_out8(command_block + ATA_DRIVE, ATA_DRIVE_MASTER);
	kern_io_out8(command_block + ATA_COUNT, 0);
	kern_io_out8(command_block + ATA_LBA0, 0);
	kern_io_out8(command_block + ATA_LBA1, 0);
	kern_io_out8(command_block + ATA_LBA2, 0);
	kern_io_out8(command_block + ATA_COMMAND, ATA_CMD_IDENTIFY);

	/* A status of zero means no drive is attached. */
	status = kern_io_in8(command_block + ATA_STATUS);
	if (status == 0)
		return ENODEV;

	/* Waits for the data. */
	error = wait_status(ATA_DRQ, ATA_BSY);
	if (error != 0)
		return error;

	/* Reads the 256 words of identify data. */
	for (index = 0; index < 256U; index++)
		words[index] = kern_io_in16(command_block + ATA_DATA);

	/* The drive's size in sectors, from the two words of the 28-bit count. */
	ata_sectors = (uint64_t)words[ATA_IDENTIFY_LBA_LOW] | (uint64_t)words[ATA_IDENTIFY_LBA_HIGH] << 16;
	if (ata_sectors == 0)
		return EIO;

	/* Succeeded: the size is known. */
	return 0;
}

/* Selects one sector by its 28-bit LBA on the first drive. */
static void
select_lba(
	uint32_t lba)
{
	/* One sector, its LBA in three bytes, and the top four bits with the drive. */
	kern_io_out8(command_block + ATA_COUNT, 1);
	kern_io_out8(command_block + ATA_LBA0, (uint8_t)lba);
	kern_io_out8(command_block + ATA_LBA1, (uint8_t)(lba >> 8));
	kern_io_out8(command_block + ATA_LBA2, (uint8_t)(lba >> 16));
	kern_io_out8(command_block + ATA_DRIVE, (uint8_t)(ATA_DRIVE_MASTER_LBA | (lba >> 24 & 15U)));
}

/* Reads or writes one sector. */
static int
transfer_sector(
	int write,
	uint32_t lba,
	uint8_t *data)
{
	uint16_t word;
	unsigned index;
	int error;

	/* Issues the read or the write of the sector. */
	select_lba(lba);
	if (write) {
		kern_io_out8(command_block + ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
	} else {
		kern_io_out8(command_block + ATA_COMMAND, ATA_CMD_READ_SECTORS);
	}

	/* Waits for the drive to take or give the data. */
	error = wait_status(ATA_DRQ, ATA_BSY);
	if (error != 0)
		return error;

	/* Moves the sector a little-endian word at a time. */
	for (index = 0; index < ATA_SECTOR_SIZE / 2U; index++) {
		if (write) {
			word = (uint16_t)data[index * 2U] | (uint16_t)data[index * 2U + 1U] << 8;
			kern_io_out16(command_block + ATA_DATA, word);
		} else {
			word = kern_io_in16(command_block + ATA_DATA);
			data[index * 2U] = (uint8_t)word;
			data[index * 2U + 1U] = (uint8_t)(word >> 8);
		}
	}

	/* A read is done once the data is taken. */
	if (!write)
		return 0;

	/* A write is done once the drive's cache has written it. */
	error = flush_cache();
	if (error != 0)
		return error;

	/* Succeeded: the sector is on the disk. */
	return 0;
}

/* Writes the drive's cache to the medium. */
static int
flush_cache(
	void)
{
	int error;

	/* Issues the flush and waits for the drive to be ready again. */
	kern_io_out8(command_block + ATA_COMMAND, ATA_CMD_FLUSH_CACHE);
	error = wait_status(ATA_DRDY, ATA_BSY);
	if (error != 0)
		return error;

	/* Succeeded: the cache is written. */
	return 0;
}

/* Carries out one request of the disk layer, sector by sector. */
static int
submit(
	struct disk *disk,
	struct bio *request)
{
	uint8_t *data;
	uint32_t index;
	size_t transferred;
	int write;
	int error;

	(void)disk;

	/* Carries out the request, or names why it cannot be. */
	error = 0;
	data = request->b_data;
	if (request->b_op == BIO_FLUSH) {
		/* A flush writes the drive's cache. */
		error = flush_cache();
	} else if (request->b_op != BIO_READ && request->b_op != BIO_WRITE) {
		/* No other operation is supported. */
		error = EOPNOTSUPP;
	} else if (request->b_mapped_block >= ATA_LBA28_LIMIT ||
		   request->b_block_count > ATA_LBA28_LIMIT - request->b_mapped_block) {
		/* A range past what a 28-bit LBA reaches. */
		error = EINVAL;
	} else {
		/* Moves each sector in order, stopping at the first failure. */
		write = 0;
		if (request->b_op == BIO_WRITE)
			write = 1;
		for (index = 0; index < request->b_block_count; index++) {
			error = transfer_sector(write,
						(uint32_t)request->b_mapped_block + index,
						data + (size_t)index * ATA_SECTOR_SIZE);
			if (error != 0)
				break;
		}
	}

	/* Logs a failed request with the drive's registers. */
	if (error != 0) {
		kern_logf("cmd646: op=%u lba=%lu count=%u\n",
			  (unsigned)request->b_op,
			  (unsigned long)request->b_mapped_block,
			  (unsigned)request->b_block_count);
		log_failure("request", error);
	}

	/* Completes the request with what was moved. */
	transferred = 0;
	if (error == 0)
		transferred = (size_t)request->b_block_count * ATA_SECTOR_SIZE;
	bio_complete(request, error, transferred);

	/* Succeeded: the request is completed, with its own status. */
	return 0;
}

/* Refuses every control request: the drive has none. */
static int
ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	(void)disk;
	(void)request;
	(void)argument;

	/* Failed: no request is supported. */
	return EOPNOTSUPP;
}
