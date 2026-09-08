/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* CMD646 primary-channel ATA PIO driver for QEMU sun4u. */
#include "drivers/platform/sun4u/sun4u-cmd646.h"
#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>

#define ATA_DATA 0U
#define ATA_ERROR 1U
#define ATA_COUNT 2U
#define ATA_LBA0 3U
#define ATA_LBA1 4U
#define ATA_LBA2 5U
#define ATA_DRIVE 6U
#define ATA_STATUS 7U
#define ATA_COMMAND 7U
#define ATA_BSY 0x80U
#define ATA_DRDY 0x40U
#define ATA_DRQ 0x08U
#define ATA_ERR 0x01U
static uint16_t cmd, ctl;
static struct disk *ata_disk;
static uint64_t sectors;

static int wait_status(uint8_t set, uint8_t clear);
static int identify(void);
static void select_lba(uint32_t lba);
static int block(int write, uint32_t lba, uint8_t *data);
static int submit(struct disk *d, struct bio *b);
static int ioctl(struct disk *d, unsigned long r, void *a);

static const struct disk_ops ops = {.submit = submit, .ioctl = ioctl};

/*
 * Implements the drv sun4u cmd646 init operation.
 */
int
drv_sun4u_cmd646_init(
	uint16_t command_port,
	uint16_t control_port)
{
	volatile unsigned i_index_for;
	int error;

	cmd = command_port;
	ctl = control_port;
	ata_disk = NULL;
	hal_io_outp8(ctl + 2U, 0x04);
	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < 100000U; i_index_for++)
		;
	hal_io_outp8(ctl + 2U, 0x02);
	hal_io_outp8(cmd + ATA_DRIVE, 0xa0);

	/* Checks the operation status. */
	error = wait_status(ATA_DRDY, ATA_BSY);
	if (error) {
		hal_printf("cmd646: reset error=%d status=%x ata=%x\n", error,
			   hal_io_inp8(cmd + ATA_STATUS),
			   hal_io_inp8(cmd + ATA_ERROR));

		/* Returns the computed result. */
		return error;
	}

	/* Checks the operation status. */
	error = identify();
	if (error) {
		hal_printf("cmd646: identify error=%d status=%x ata=%x\n",
			   error, hal_io_inp8(cmd + ATA_STATUS),
			   hal_io_inp8(cmd + ATA_ERROR));

		/* Returns the computed result. */
		return error;
	}

	/* Handles the ata disk condition. */
	ata_disk = disk_alloc();
	if (!ata_disk)
		return ENOMEM;

	/* Checks the operation status. */
	error = disk_alloc_sd_name(ata_disk);
	if (error) {
		(void)disk_destroy(ata_disk);
		ata_disk = NULL;

		/* Returns the computed result. */
		return error;
	}

	ata_disk->d_block_size = 512;
	ata_disk->d_block_count = sectors;
	ata_disk->d_max_transfer_blocks = 1;
	ata_disk->d_ops = &ops;

	/* Checks the operation status. */
	error = disk_create(ata_disk);
	if (error) {
		ata_disk = NULL;

		/* Returns the computed result. */
		return error;
	}

	hal_printf("SPARCV9 IDE PASS sectors=%llu\n", sectors);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv sun4u cmd646 disk operation.
 */
struct disk *
drv_sun4u_cmd646_disk(
	void)
{
	/* Returns the computed result. */
	return ata_disk;
}

/* Supports the wait status operation. */
static int
wait_status(
	uint8_t set,
	uint8_t clear)
{
	uint8_t s;
	unsigned long n = 10000000UL;

	/* Continue while the operation condition remains true. */
	while (n--) {
		/* Checks the current string state. */
		s = hal_io_inp8(cmd + ATA_STATUS);
		if (s & ATA_ERR)
			return EIO;

		/* Checks the current string state. */
		if ((s & set) == set && !(s & clear))
			return 0;
	}

	/* Returns the computed result. */
	return ETIMEDOUT;
}

/* Supports the identify operation. */
static int
identify(
	void)
{
	unsigned i_index_for;
	uint16_t words[256];
	int error;

	/* Selects the drive and issues the identify command. */
	hal_io_outp8(cmd + ATA_DRIVE, 0xa0);
	hal_io_outp8(cmd + ATA_COUNT, 0);
	hal_io_outp8(cmd + ATA_LBA0, 0);
	hal_io_outp8(cmd + ATA_LBA1, 0);
	hal_io_outp8(cmd + ATA_LBA2, 0);
	hal_io_outp8(cmd + ATA_COMMAND, 0xec);

	/* Checks the hal io inp8 result. */
	if (hal_io_inp8(cmd + ATA_STATUS) == 0)
		return ENODEV;

	/* Checks the operation status. */
	error = wait_status(ATA_DRQ, ATA_BSY);
	if (error)
		return error;
	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < 256U; i_index_for++)
		words[i_index_for] = hal_io_inp16(cmd + ATA_DATA);
	sectors = (uint32_t)words[60] | (uint64_t)words[61] << 16;

	/* Returns the computed result. */
	return sectors ? 0 : EIO;
}

/* Supports the select lba operation. */
static void
select_lba(
	uint32_t lba)
{
	hal_io_outp8(cmd + ATA_COUNT, 1);
	hal_io_outp8(cmd + ATA_LBA0, (uint8_t)lba);
	hal_io_outp8(cmd + ATA_LBA1, (uint8_t)(lba >> 8));
	hal_io_outp8(cmd + ATA_LBA2, (uint8_t)(lba >> 16));
	hal_io_outp8(cmd + ATA_DRIVE, (uint8_t)(0xe0U | (lba >> 24 & 15U)));
}

/* Supports the block operation. */
static int
block(
	int write,
	uint32_t lba,
	uint8_t *data)
{
	int function_result;
	uint16_t word_local;
	uint16_t word_local1;
	unsigned i_index_for;
	int error;

	select_lba(lba);
	hal_io_outp8(cmd + ATA_COMMAND, write ? 0x30U : 0x20U);

	/* Checks the operation status. */
	error = wait_status(ATA_DRQ, ATA_BSY);
	if (error)
		return error;
	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < 256U; i_index_for++) {
		/* Handles the write condition. */
		if (write) {
			word_local = (uint16_t)data[i_index_for * 2U] |
				     (uint16_t)data[i_index_for * 2U + 1U] << 8;
			hal_io_outp16(cmd + ATA_DATA, word_local);
		} else {
			word_local1 = hal_io_inp16(cmd + ATA_DATA);
			data[i_index_for * 2U] = (uint8_t)word_local1;
			data[i_index_for * 2U + 1U] =
				(uint8_t)(word_local1 >> 8);
		}
	}

	/* Handles the write condition. */
	if (write) {
		hal_io_outp8(cmd + ATA_COMMAND, 0xe7);

		/* Obtains the wait status result. */
		function_result = wait_status(ATA_DRDY, ATA_BSY);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the submit operation. */
static int
submit(
	struct disk *d,
	struct bio *b)
{
	uint32_t i_index_for;
	int error = 0;
	uint8_t *p = b->b_data;

	(void)d;

	/* Handles the b condition. */
	if (b->b_op == BIO_FLUSH) {
		hal_io_outp8(cmd + ATA_COMMAND, 0xe7);
		error = wait_status(ATA_DRDY, ATA_BSY);
	} else if (b->b_op != BIO_READ && b->b_op != BIO_WRITE)
		error = EOPNOTSUPP;
	else if (b->b_mapped_block > 0x0fffffffULL ||
		 b->b_block_count > 0x10000000ULL - b->b_mapped_block)
		error = EINVAL;
	else
		/* Process each remaining element. */
		for (i_index_for = 0; i_index_for < b->b_block_count && !error;
		     i_index_for++) {
			error = block(b->b_op == BIO_WRITE,
				      (uint32_t)b->b_mapped_block + i_index_for,
				      p + (size_t)i_index_for * 512U);
		}

	/* Checks the operation status. */
	if (error) {
		hal_printf("cmd646: op=%u lba=%llu count=%u error=%d status=%x "
			   "ata=%x\n",
			   (unsigned)b->b_op, b->b_mapped_block,
			   b->b_block_count, error,
			   hal_io_inp8(cmd + ATA_STATUS),
			   hal_io_inp8(cmd + ATA_ERROR));
	}

	bio_complete(b, error, error ? 0 : (size_t)b->b_block_count * 512U);

	/* Reports successful completion. */
	return 0;
}

/* Supports the ioctl operation. */
static int
ioctl(
	struct disk *d,
	unsigned long r,
	void *a)
{
	(void)d;
	(void)r;
	(void)a;

	/* Returns the computed result. */
	return EOPNOTSUPP;
}
