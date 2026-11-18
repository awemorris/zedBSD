/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerPC Mac's ATA channels in the I/O controller (KeyLargo's
 * "keylargo-ata", which QEMU's mac99 models as macio-ide), driven by
 * programmed I/O through the shared ATA core.
 *
 * The task file registers are 16 bytes apart from the channel's base,
 * the device control and alternate status register at 0x160.  The data
 * port is read and written 16 bits at a time without reversing the
 * bytes, which keeps them in the order they have on the disk.  The
 * firmware set the channel's timing; the driver keeps it.  Each drive
 * that answers IDENTIFY DEVICE is published as an sd disk; the hardware
 * layer hands the channels over as "mac99.ata", one record a channel:
 * its task file's and its DBDMA channel's physical addresses, its
 * interrupt, and the drive the firmware booted from on it (0 or 1, or
 * MAC99_ATA_NOT_BOOT).
 *
 * Values referenced: the Linux kernel's drivers/ata/pata_macio.c (the
 * register spacing of 0x10 and the control register at 0x160, the data
 * port's accesses without byte reversal), and QEMU's hw/ide/macio.c,
 * which decodes the same.
 */

#include "drivers/platform/mac99/mac99-ata.h"

#include <hal/hal.h>
#include <drivers/generic/ata-pio.h>
#include <kern/disk.h>
#include <kern/kmem.h>
#include <kern/kcrt.h>
#include <kern/pmem.h>
#include <uapi/sysctl.h>

#include <uapi/errno.h>
#include "kern/klog.h"
#include "kern/device-io.h"

/* The spacing of the task file registers, the control register, and the size mapped. */
#define MACIO_ATA_STRIDE	0x10U
#define MACIO_ATA_CONTROL	0x160U
#define MACIO_ATA_SIZE		0x1000U

/* The words of a channel's record in the handoff, and the most channels kept. */
#define MACIO_ATA_RECORD_WORDS	4U
#define MACIO_ATA_CHANNELS_MAX	4U

/* The drives of a channel, and the longest model kept. */
#define MACIO_ATA_DRIVES	2U
#define MACIO_ATA_MODEL_SIZE	41U

/*
 * One channel: its registers and the ATA core's view of them.  Made by
 * drv_mac99_ata_init() and kept for the life of the system.
 */
struct macio_channel {
	volatile uint8_t *base;
	struct ata_pio_channel ata;
};

/*
 * One published drive: its channel and its number on it.  Kept for the
 * life of the system; the disk layer issues one request at a time per
 * disk, and the channel's two drives are served under the channel's
 * lock.
 */
struct macio_drive {
	struct macio_channel *channel;
	unsigned number;
	struct disk *disk;
};

/*
 * The channels and the drives published, and the drive the firmware
 * booted from (NULL when none of them).  Filled once by
 * drv_mac99_ata_init().
 */
static struct macio_channel channels[MACIO_ATA_CHANNELS_MAX];
static struct macio_drive drives[MACIO_ATA_CHANNELS_MAX * MACIO_ATA_DRIVES];
static unsigned drive_count;
static struct macio_drive *boot_drive;

/*
 * Serves one request at a time on all channels: the two drives of a
 * channel share its registers.
 */
static struct mutex channel_lock;

static uint8_t channel_read8(void *context, unsigned reg);
static void channel_write8(void *context, unsigned reg, uint8_t value);
static void channel_read_data(void *context, uint8_t *data, size_t length);
static void channel_write_data(void *context, const uint8_t *data, size_t length);
static int publish_drive(struct macio_channel *channel, unsigned number, int boot);
static int submit(struct disk *disk, struct bio *request);
static int ioctl(struct disk *disk, unsigned long request, void *argument);

/*
 * The disk layer's entry points for a drive.
 */
static const struct disk_ops ops = {
	.submit = submit,
	.ioctl = ioctl
};

/*
 * Resets each ATA channel the firmware described and publishes each
 * drive that answers.  Reports ENODEV when there is no channel.
 */
int
drv_mac99_ata_init(
	void)
{
	const uint32_t *table;
	const uint32_t *record;
	struct macio_channel *channel;
	unsigned count;
	unsigned index;
	unsigned number;
	void *window;
	int error;

	/* The channels the firmware described. */
	table = hal_get_arch_handoff("mac99.ata");
	if (table == NULL || table[0] == 0)
		return ENODEV;
	count = table[0];
	if (count > MACIO_ATA_CHANNELS_MAX)
		count = MACIO_ATA_CHANNELS_MAX;
	mutex_init(&channel_lock, LOCK_RANK_DEVICE, "mac99 ATA");

	/* Each channel: its registers, a reset, and its drives. */
	for (index = 0; index < count; index++) {
		record = &table[1U + index * MACIO_ATA_RECORD_WORDS];
		channel = &channels[index];
		error = kern_device_map(record[0], MACIO_ATA_SIZE, KERN_DEVICE_UNCACHED, &window);
		if (error != 0) {
			kern_logf("mac99-ata: channel %u at %x unmapped (%d)\n", index, record[0], error);
			continue;
		}
		channel->base = window;
		channel->ata.read8 = channel_read8;
		channel->ata.write8 = channel_write8;
		channel->ata.read_data = channel_read_data;
		channel->ata.write_data = channel_write_data;
		channel->ata.context = channel;

		/* A channel that stays busy has no drive to serve. */
		error = ata_pio_reset(&channel->ata);
		if (error != 0) {
			kern_logf("mac99-ata: channel %u at %x: no drive (%d)\n", index, record[0], error);
			continue;
		}

		/* Each drive that answers. */
		for (number = 0; number < MACIO_ATA_DRIVES; number++)
			(void)publish_drive(channel, number, record[3] == number);
	}

	/* Succeeded: the drives found are published. */
	return 0;
}

/*
 * Reports the disk of the drive the firmware booted from, or NULL when
 * the firmware booted from none of these channels.
 */
struct disk *
drv_mac99_ata_boot_disk(
	void)
{
	/* The drive marked at publication. */
	if (boot_drive == NULL)
		return NULL;

	/* Succeeded: its disk. */
	return boot_drive->disk;
}

/* Reads one register: a task file register, or the control block's. */
static uint8_t
channel_read8(
	void *context,
	unsigned reg)
{
	struct macio_channel *channel;
	uint8_t value;

	/* The register's place in the channel. */
	channel = context;
	if (reg == ATA_PIO_CONTROL) {
		value = kern_mmio_read8(channel->base + MACIO_ATA_CONTROL);
	} else {
		value = kern_mmio_read8(channel->base + reg * MACIO_ATA_STRIDE);
	}

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one register: a task file register, or the control block's. */
static void
channel_write8(
	void *context,
	unsigned reg,
	uint8_t value)
{
	struct macio_channel *channel;

	/* The register's place in the channel. */
	channel = context;
	if (reg == ATA_PIO_CONTROL) {
		kern_mmio_write8(channel->base + MACIO_ATA_CONTROL, value);
	} else {
		kern_mmio_write8(channel->base + reg * MACIO_ATA_STRIDE, value);
	}
}

/* Reads bytes from the data port, 16 bits at a time as they lie on the disk. */
static void
channel_read_data(
	void *context,
	uint8_t *data,
	size_t length)
{
	struct macio_channel *channel;
	uint16_t word;
	size_t at;

	/* Each word stored as loaded keeps the bytes' order. */
	channel = context;
	for (at = 0; at + 1U < length; at += 2U) {
		word = kern_mmio_read16(channel->base);
		kern_memcpy(data + at, &word, sizeof(word));
	}
}

/* Writes bytes to the data port, 16 bits at a time as they lie on the disk. */
static void
channel_write_data(
	void *context,
	const uint8_t *data,
	size_t length)
{
	struct macio_channel *channel;
	uint16_t word;
	size_t at;

	/* Each word stored as loaded keeps the bytes' order. */
	channel = context;
	for (at = 0; at + 1U < length; at += 2U) {
		kern_memcpy(&word, data + at, sizeof(word));
		kern_mmio_write16(channel->base, word);
	}
}

/* Identifies one drive of a channel and publishes it as an sd disk. */
static int
publish_drive(
	struct macio_channel *channel,
	unsigned number,
	int boot)
{
	char model[MACIO_ATA_MODEL_SIZE];
	struct macio_drive *drive;
	struct disk *disk;
	uint64_t sectors;
	int error;

	/* A drive that does not answer is not there. */
	error = ata_pio_identify(&channel->ata, number, &sectors, model, sizeof(model));
	if (error != 0)
		return error;
	if (drive_count == MACIO_ATA_CHANNELS_MAX * MACIO_ATA_DRIVES)
		return ENOSPC;

	/* The disk's record, named as an sd disk. */
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;
	error = disk_alloc_sd_name(disk);
	if (error != 0) {
		(void)disk_destroy(disk);
		return error;
	}

	/* Describes the disk: 512-byte sectors, up to 256 a request, listed as IDE. */
	drive = &drives[drive_count];
	drive->channel = channel;
	drive->number = number;
	drive->disk = disk;
	disk->d_block_size = ATA_PIO_SECTOR_SIZE;
	disk->d_block_count = sectors;
	disk->d_max_transfer_blocks = ATA_PIO_SECTORS_MAX;
	disk->d_ops = &ops;
	disk->d_data = drive;
	disk_set_stats_kind(disk, DISK_STATS_KIND_IDE);

	/* Publishes the disk. */
	error = disk_create(disk);
	if (error != 0)
		return error;
	drive_count++;
	if (boot)
		boot_drive = drive;
	kern_logf("mac99-ata: %s drive %u \"%s\" sectors=%llu%s\n", disk->d_name, number, model,
		  (unsigned long long)sectors, boot ? " (boot)" : "");

	/* Succeeded: the drive is a disk. */
	return 0;
}

/* Carries out one request of the disk layer, up to 256 sectors a command. */
static int
submit(
	struct disk *disk,
	struct bio *request)
{
	struct macio_drive *drive;
	uint8_t *data;
	uint64_t done;
	unsigned count;
	size_t transferred;
	int write;
	int error;

	/* Carries out the request under the channels' lock. */
	drive = disk->d_data;
	error = 0;
	data = request->b_data;
	mutex_lock(&channel_lock);

	if (request->b_op == BIO_FLUSH) {
		/* A flush writes the drive's cache. */
		error = ata_pio_flush(&drive->channel->ata, drive->number);
	} else if (request->b_op != BIO_READ && request->b_op != BIO_WRITE) {
		/* No other operation is supported. */
		error = EOPNOTSUPP;
	} else {
		/* Moves the sectors in runs of up to 256, stopping at the first failure. */
		write = 0;
		if (request->b_op == BIO_WRITE)
			write = 1;
		for (done = 0; done < request->b_block_count; done += count) {
			count = ATA_PIO_SECTORS_MAX;
			if (request->b_block_count - done < count)
				count = (unsigned)(request->b_block_count - done);
			error = ata_pio_transfer(&drive->channel->ata, drive->number, write,
						 request->b_mapped_block + done, count,
						 data + (size_t)done * ATA_PIO_SECTOR_SIZE);
			if (error != 0)
				break;
		}
	}

	mutex_unlock(&channel_lock);

	/* Logs a failed request. */
	if (error != 0) {
		kern_logf("mac99-ata: %s op=%u lba=%lu count=%u error=%d\n", disk->d_name,
			  (unsigned)request->b_op,
			  (unsigned long)request->b_mapped_block,
			  (unsigned)request->b_block_count,
			  error);
	}

	/* Completes the request with what was moved. */
	transferred = 0;
	if (error == 0)
		transferred = (size_t)request->b_block_count * ATA_PIO_SECTOR_SIZE;
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
