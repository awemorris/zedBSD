/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ATA command protocol by programmed I/O, shared by the controllers
 * whose task file registers differ only in where they are: a channel
 * names how to reach its registers and its data port, and the core
 * resets it, identifies a drive and moves sectors by polling.
 */

#ifndef KERN_DRIVERS_GENERIC_ATA_PIO_H
#define KERN_DRIVERS_GENERIC_ATA_PIO_H

#include <stddef.h>
#include <stdint.h>

/* The task file registers, by their number, and the control block's device control. */
#define ATA_PIO_DATA		0U
#define ATA_PIO_ERROR		1U
#define ATA_PIO_COUNT		2U
#define ATA_PIO_LBA0		3U
#define ATA_PIO_LBA1		4U
#define ATA_PIO_LBA2		5U
#define ATA_PIO_DRIVE		6U
#define ATA_PIO_STATUS		7U
#define ATA_PIO_COMMAND		7U
#define ATA_PIO_CONTROL		8U

/* A sector, and the most sectors one command moves. */
#define ATA_PIO_SECTOR_SIZE	512U
#define ATA_PIO_SECTORS_MAX	256U

/* The highest sector a 28-bit address reaches, plus one. */
#define ATA_PIO_LBA28_LIMIT	0x10000000ULL

/*
 * How one channel's registers are reached: read8 and write8 take a
 * register number above (ATA_PIO_CONTROL is the control block's device
 * control and alternate status), read_data and write_data move whole
 * sectors through the data port keeping the bytes in the order they
 * have on the disk, and context is the controller's own.
 */
struct ata_pio_channel {
	uint8_t (*read8)(void *context, unsigned reg);
	void (*write8)(void *context, unsigned reg, uint8_t value);
	void (*read_data)(void *context, uint8_t *data, size_t length);
	void (*write_data)(void *context, const uint8_t *data, size_t length);
	void *context;
};

int
ata_pio_reset(
	const struct ata_pio_channel *channel);

int
ata_pio_identify(
	const struct ata_pio_channel *channel,
	unsigned drive,
	uint64_t *sectors,
	char *model,
	size_t model_size);

int
ata_pio_transfer(
	const struct ata_pio_channel *channel,
	unsigned drive,
	int write,
	uint64_t lba,
	unsigned count,
	uint8_t *data);

int
ata_pio_flush(
	const struct ata_pio_channel *channel,
	unsigned drive);

#endif
