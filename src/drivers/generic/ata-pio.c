/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ATA command protocol by programmed I/O.
 *
 * Every command is polled to completion with the channel's interrupt
 * off.  A drive is addressed by 28-bit LBA; a transfer moves up to 256
 * sectors with one READ SECTORS or WRITE SECTORS, a sector at a time
 * through the data port as the drive asks for it, and a write is
 * followed by FLUSH CACHE.  The identify data is little-endian words.
 *
 * Values referenced: ATA/ATAPI-6 (INCITS 361-2002), the command and
 * control block registers, the status bits (BSY, DRDY, DRQ, ERR), the
 * device control bits (SRST, nIEN), the device register (LBA bit 6, DEV
 * bit 4), the commands IDENTIFY DEVICE, READ SECTORS, WRITE SECTORS and
 * FLUSH CACHE, a sector count of zero meaning 256, and IDENTIFY DEVICE's
 * words 27 to 46 (the model, two characters a word, the first in the
 * high byte) and 60 and 61 (the sectors a 28-bit address reaches).
 */

#include <drivers/generic/ata-pio.h>

#include <uapi/errno.h>

/* The status bits. */
#define ATA_BSY			0x80U
#define ATA_DRDY		0x40U
#define ATA_DRQ			0x08U
#define ATA_ERR			0x01U

/* The device control bits: software reset, and interrupts off. */
#define ATA_CONTROL_RESET	0x04U
#define ATA_CONTROL_NIEN	0x02U

/* The device register: addressed by LBA, and the second drive. */
#define ATA_DRIVE_BASE		0xa0U
#define ATA_DRIVE_LBA		0x40U
#define ATA_DRIVE_SLAVE		0x10U

/* The commands. */
#define ATA_CMD_IDENTIFY	0xecU
#define ATA_CMD_READ_SECTORS	0x20U
#define ATA_CMD_WRITE_SECTORS	0x30U
#define ATA_CMD_FLUSH_CACHE	0xe7U

/* The identify data's words: the model, and the sectors addressable by 28-bit LBA. */
#define ATA_IDENTIFY_MODEL	27U
#define ATA_IDENTIFY_MODEL_WORDS 20U
#define ATA_IDENTIFY_LBA_LOW	60U
#define ATA_IDENTIFY_LBA_HIGH	61U

/* How long a status wait polls, and how long the reset pulse lasts. */
#define ATA_STATUS_SPINS	10000000UL
#define ATA_RESET_SPINS		100000U

static int wait_status(const struct ata_pio_channel *channel, uint8_t set, uint8_t clear);
static uint8_t drive_bits(unsigned drive);
static void select_lba(const struct ata_pio_channel *channel, unsigned drive, uint32_t lba, unsigned count);
static unsigned identify_word(const uint8_t *data, unsigned index);

/*
 * Pulses the channel's software reset and leaves its interrupt off, for
 * polling.  Reports 0, or ETIMEDOUT when no drive becomes ready.
 */
int
ata_pio_reset(
	const struct ata_pio_channel *channel)
{
	volatile unsigned spin;
	int error;

	/* The reset pulse, then interrupts off. */
	channel->write8(channel->context, ATA_PIO_CONTROL, ATA_CONTROL_RESET | ATA_CONTROL_NIEN);
	for (spin = 0; spin < ATA_RESET_SPINS; spin++)
		continue;
	channel->write8(channel->context, ATA_PIO_CONTROL, ATA_CONTROL_NIEN);

	/* Waits for the channel to leave the reset. */
	error = wait_status(channel, 0, ATA_BSY);
	if (error != 0)
		return error;

	/* Succeeded: the channel is idle. */
	return 0;
}

/*
 * Identifies one drive (0 the first, 1 the second) and reports its size
 * in sectors and its model, NUL-terminated and trimmed.  Reports ENODEV
 * when no drive answers.
 */
int
ata_pio_identify(
	const struct ata_pio_channel *channel,
	unsigned drive,
	uint64_t *sectors,
	char *model,
	size_t model_size)
{
	uint8_t data[ATA_PIO_SECTOR_SIZE];
	uint8_t status;
	unsigned word;
	unsigned at;
	unsigned index;
	int error;

	/* Selects the drive; a status of zero or all ones means none is there. */
	channel->write8(channel->context, ATA_PIO_DRIVE, drive_bits(drive));
	status = channel->read8(channel->context, ATA_PIO_STATUS);
	if (status == 0 || status == 0xffU)
		return ENODEV;
	error = wait_status(channel, 0, ATA_BSY);
	if (error != 0)
		return error;

	/* Issues IDENTIFY DEVICE. */
	channel->write8(channel->context, ATA_PIO_COUNT, 0);
	channel->write8(channel->context, ATA_PIO_LBA0, 0);
	channel->write8(channel->context, ATA_PIO_LBA1, 0);
	channel->write8(channel->context, ATA_PIO_LBA2, 0);
	channel->write8(channel->context, ATA_PIO_COMMAND, ATA_CMD_IDENTIFY);
	status = channel->read8(channel->context, ATA_PIO_STATUS);
	if (status == 0)
		return ENODEV;

	/* Takes the 256 words. */
	error = wait_status(channel, ATA_DRQ, ATA_BSY);
	if (error != 0)
		return error;
	channel->read_data(channel->context, data, sizeof(data));

	/* The size, from the two words of the 28-bit count. */
	*sectors = (uint64_t)identify_word(data, ATA_IDENTIFY_LBA_LOW) |
		   (uint64_t)identify_word(data, ATA_IDENTIFY_LBA_HIGH) << 16;
	if (*sectors == 0)
		return EIO;

	/* The model: two characters a word, the first in the high byte. */
	if (model == NULL || model_size == 0)
		return 0;
	at = 0;
	for (index = 0; index < ATA_IDENTIFY_MODEL_WORDS && at + 2U < model_size; index++) {
		word = identify_word(data, ATA_IDENTIFY_MODEL + index);
		model[at++] = (char)(word >> 8);
		model[at++] = (char)(word & 0xffU);
	}

	/* Trims the trailing spaces. */
	while (at > 0 && (model[at - 1U] == ' ' || model[at - 1U] == '\0'))
		at--;
	model[at] = '\0';

	/* Succeeded: the drive is known. */
	return 0;
}

/*
 * Reads or writes count sectors (1 to 256) from lba of one drive into or
 * out of data.  A write is flushed to the medium before it is reported.
 */
int
ata_pio_transfer(
	const struct ata_pio_channel *channel,
	unsigned drive,
	int write,
	uint64_t lba,
	unsigned count,
	uint8_t *data)
{
	unsigned index;
	int error;

	/* Refuses a range a 28-bit address does not reach, and an empty or long one. */
	if (count == 0 || count > ATA_PIO_SECTORS_MAX)
		return EINVAL;
	if (lba >= ATA_PIO_LBA28_LIMIT || count > ATA_PIO_LBA28_LIMIT - lba)
		return EINVAL;

	/* Issues the command once the drive is ready. */
	channel->write8(channel->context, ATA_PIO_DRIVE, drive_bits(drive));
	error = wait_status(channel, ATA_DRDY, ATA_BSY);
	if (error != 0)
		return error;
	select_lba(channel, drive, (uint32_t)lba, count);
	if (write) {
		channel->write8(channel->context, ATA_PIO_COMMAND, ATA_CMD_WRITE_SECTORS);
	} else {
		channel->write8(channel->context, ATA_PIO_COMMAND, ATA_CMD_READ_SECTORS);
	}

	/* Moves each sector when the drive asks for it. */
	for (index = 0; index < count; index++) {
		error = wait_status(channel, ATA_DRQ, ATA_BSY);
		if (error != 0)
			return error;
		if (write) {
			channel->write_data(channel->context, data + (size_t)index * ATA_PIO_SECTOR_SIZE, ATA_PIO_SECTOR_SIZE);
		} else {
			channel->read_data(channel->context, data + (size_t)index * ATA_PIO_SECTOR_SIZE, ATA_PIO_SECTOR_SIZE);
		}
	}

	/* A read is done once its data is taken. */
	if (!write)
		return 0;

	/* A write is done once the drive's cache holds nothing of it. */
	error = ata_pio_flush(channel, drive);
	if (error != 0)
		return error;

	/* Succeeded: the sectors are on the medium. */
	return 0;
}

/*
 * Writes one drive's cache to the medium.
 */
int
ata_pio_flush(
	const struct ata_pio_channel *channel,
	unsigned drive)
{
	int error;

	/* Issues the flush and waits for the drive to be ready again. */
	channel->write8(channel->context, ATA_PIO_DRIVE, drive_bits(drive));
	channel->write8(channel->context, ATA_PIO_COMMAND, ATA_CMD_FLUSH_CACHE);
	error = wait_status(channel, ATA_DRDY, ATA_BSY);
	if (error != 0)
		return error;

	/* Succeeded: the cache is written. */
	return 0;
}

/* Waits until the status shows every bit of set and none of clear. */
static int
wait_status(
	const struct ata_pio_channel *channel,
	uint8_t set,
	uint8_t clear)
{
	unsigned long spins;
	uint8_t status;

	/* Polls the status until it matches, the drive reports an error, or the time runs out. */
	for (spins = 0; spins < ATA_STATUS_SPINS; spins++) {
		/* A busy drive's other bits mean nothing. */
		status = channel->read8(channel->context, ATA_PIO_STATUS);
		if ((status & ATA_BSY) != 0 && (clear & ATA_BSY) != 0)
			continue;

		/* A drive that reports an error has failed the command. */
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

/* Reports the device register's fixed bits for a drive. */
static uint8_t
drive_bits(
	unsigned drive)
{
	/* The second drive has its own bit. */
	if (drive != 0)
		return (uint8_t)(ATA_DRIVE_BASE | ATA_DRIVE_SLAVE);
	return (uint8_t)ATA_DRIVE_BASE;
}

/* Selects a run of sectors by its 28-bit LBA; a count of 256 is written as zero. */
static void
select_lba(
	const struct ata_pio_channel *channel,
	unsigned drive,
	uint32_t lba,
	unsigned count)
{
	/* The count, the LBA in three bytes, and the top four bits with the drive. */
	channel->write8(channel->context, ATA_PIO_COUNT, (uint8_t)(count & 0xffU));
	channel->write8(channel->context, ATA_PIO_LBA0, (uint8_t)lba);
	channel->write8(channel->context, ATA_PIO_LBA1, (uint8_t)(lba >> 8));
	channel->write8(channel->context, ATA_PIO_LBA2, (uint8_t)(lba >> 16));
	channel->write8(channel->context, ATA_PIO_DRIVE, (uint8_t)(drive_bits(drive) | ATA_DRIVE_LBA | (lba >> 24 & 15U)));
}

/* Reads one little-endian word of the identify data. */
static unsigned
identify_word(
	const uint8_t *data,
	unsigned index)
{
	/* The low byte first. */
	return (unsigned)data[index * 2U] | (unsigned)data[index * 2U + 1U] << 8;
}
