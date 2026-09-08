/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* X68000 MB89352 synchronous polled-PIO block driver. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "drivers/platform/x68k/x68k-spc-disk.h"

#include <errno.h>
#include <hal/hal.h>
#include <kern/disk.h>

#define SPC_TARGET_COUNT 7U
#define SPC_LUN 0U
#define SPC_MAX_BLOCKS 127U
#define SCSI_SENSE_NOT_READY 0x02U
#define SCSI_SENSE_UNIT_ATTENTION 0x06U
#define SCSI_ASC_BECOMING_READY 0x04U

struct spc_disk_unit {
	struct disk *disk;
	unsigned target_id;
	unsigned ordinal;
	struct x68k_scsi_sense sense;
	int sense_valid;
};

static struct x68k_spc_bus controller_bus;
static struct spc_disk_unit units[SPC_TARGET_COUNT];
static unsigned controller_initiator;
static unsigned present_count;

static void zero_bytes(void *pointer, size_t length);
static int probe_target(unsigned target_id);
static int unit_ready(struct spc_disk_unit *unit);
static int request_sense(struct spc_disk_unit *unit);
static void sanitize(char *output, const uint8_t *input, unsigned length);
static int spc_errno(int error);
static int record_command_error(struct spc_disk_unit *unit, uint8_t opcode, int error);
static int read_write(struct spc_disk_unit *unit, int write, uint64_t lba, uint32_t blocks, void *buffer);
static int spc_submit(struct disk *disk, struct bio *bio);
static int spc_ioctl(struct disk *disk, unsigned long request, void *argument);

static const struct disk_ops spc_ops = {
	.submit = spc_submit,
	.ioctl = spc_ioctl,
};












/*
 * Implements the drv x68k spc disk init operation.
 */
unsigned
drv_x68k_spc_disk_init(
	const struct x68k_spc_bus *bus,
	unsigned initiator_id,
	unsigned boot_target_id)
{
	unsigned target;

	/* Handles the bus availability. */
	if (bus == NULL || bus->read == NULL || bus->write == NULL ||
	    initiator_id > 7U || boot_target_id >= SPC_TARGET_COUNT ||
	    initiator_id == boot_target_id)

		/* Reports successful completion. */
		return 0;
	controller_bus = *bus;
	controller_initiator = initiator_id;
	present_count = 0;
	zero_bytes(units, sizeof(units));

	/* Checks the drv x68k spc pio init result. */
	if (drv_x68k_spc_pio_init(&controller_bus, initiator_id) != X68K_SPC_OK)
		return 0;
	(void)probe_target(boot_target_id);
	/* Process each remaining element. */
	for (target = 0; target < SPC_TARGET_COUNT; target++) {
		/* Handles the target condition. */
		if (target != boot_target_id && target != initiator_id)
			(void)probe_target(target);
	}

	/* Returns the computed result. */
	return present_count;
}

/*
 * Implements the drv x68k spc disk target operation.
 */
struct disk *
drv_x68k_spc_disk_target(
	unsigned target_id)
{
	/* Returns the computed result. */
	return target_id < SPC_TARGET_COUNT ? units[target_id].disk : NULL;
}

/* Supports the zero bytes operation. */
static void
zero_bytes(
	void *pointer,
	size_t length)
{
	uint8_t *bytes = pointer;

	/* Process each remaining element. */
	while (length-- != 0)
		*bytes++ = 0;
}

/* Supports the probe target operation. */
static int
probe_target(
	unsigned target_id)
{
	uint8_t inquiry[36], capacity[8];
	char vendor[9], product[17], revision[5];
	struct x68k_spc_result result;
	struct spc_disk_unit *unit = &units[target_id];
	uint64_t blocks;
	uint32_t block_size;
	int error;

	unit->target_id = target_id;

	/* Checks the unit ready result. */
	if (!unit_ready(unit))
		return 0;
	error = drv_x68k_spc_pio_inquiry(&controller_bus, controller_initiator,
					 target_id, SPC_LUN, inquiry, &result);

	/* Checks the operation status. */
	if (error != X68K_SPC_OK || (inquiry[0] & 0xe0U) != 0 ||
	    (inquiry[0] & 0x1fU) != 0)

		/* Reports successful completion. */
		return 0;
	error = drv_x68k_spc_pio_read_capacity10(
		&controller_bus, controller_initiator, target_id, SPC_LUN,
		capacity, &result);

	/* Checks the operation status. */
	if (error != X68K_SPC_OK ||
	    drv_x68k_scsi_parse_capacity10(capacity, &blocks, &block_size) != 0)

		/* Reports successful completion. */
		return 0;
	unit->disk = disk_alloc();

	/* Handles the disk availability. */
	if (unit->disk == NULL)
		return 0;

	/* Checks the disk alloc sd name result. */
	if (disk_alloc_sd_name(unit->disk) != 0) {
		(void)disk_destroy(unit->disk);
		unit->disk = NULL;

		/* Reports successful completion. */
		return 0;
	}
	unit->ordinal = present_count;
	unit->disk->d_flags = (inquiry[1] & 0x80U) != 0 ? DISK_REMOVABLE : 0;
	unit->disk->d_block_size = block_size;
	unit->disk->d_block_count = blocks;
	unit->disk->d_max_transfer_blocks = SPC_MAX_BLOCKS;
	unit->disk->d_ops = &spc_ops;
	unit->disk->d_data = unit;

	/* Checks the disk create result. */
	if (disk_create(unit->disk) != 0) {
		unit->disk = NULL;

		/* Reports successful completion. */
		return 0;
	}
	sanitize(vendor, inquiry + 8U, 8U);
	sanitize(product, inquiry + 16U, 16U);
	sanitize(revision, inquiry + 32U, 4U);
	hal_printf("spc: %s target=%u blocks=%llu %s %s %s\n",
		   unit->disk->d_name, target_id, blocks, vendor, product,
		   revision);
	present_count++;

	/* Reports operation failure. */
	return 1;
}

/* Supports the unit ready operation. */
static int
unit_ready(
	struct spc_disk_unit *unit)
{
	struct x68k_spc_result result;
	int error;
	unsigned attempt;

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 3U; attempt++) {
		error = drv_x68k_spc_pio_test_unit_ready(
			&controller_bus, controller_initiator, unit->target_id,
			SPC_LUN, &result);

		/* Checks the operation status. */
		if (error == X68K_SPC_OK)
			return 1;

		/* Checks the operation status. */
		if (error != X68K_SPC_ERR_STATUS || request_sense(unit) != 0)
			return 0;

		/* Handles the unit condition. */
		if (unit->sense.key != SCSI_SENSE_UNIT_ATTENTION &&
		    !(unit->sense.key == SCSI_SENSE_NOT_READY &&
		      unit->sense.asc == SCSI_ASC_BECOMING_READY))

			/* Reports successful completion. */
			return 0;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the request sense operation. */
static int
request_sense(
	struct spc_disk_unit *unit)
{
	uint8_t response[18];
	struct x68k_spc_result result;
	int error;

	unit->sense_valid = 0;
	error = drv_x68k_spc_pio_request_sense(
		&controller_bus, controller_initiator, unit->target_id, SPC_LUN,
		response, &result);

	/* Checks the operation status. */
	if (error != X68K_SPC_OK ||
	    drv_x68k_scsi_parse_sense(response, sizeof(response),
				      &unit->sense) != 0)

		/* Returns the computed result. */
		return EIO;
	unit->sense_valid = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the sanitize operation. */
static void
sanitize(
	char *output,
	const uint8_t *input,
	unsigned length)
{
	uint8_t value;
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < length; index++) {
		value = input[index];
		output[index] =
			value >= 0x20U && value <= 0x7eU ? (char)value : '?';
	}
	while (length != 0 && output[length - 1U] == ' ')
		length--;
	output[length] = '\0';
}

/* Supports the spc errno operation. */
static int
spc_errno(
	int error)
{
	/* Dispatch the selected operation case. */
	switch (error) {
	case X68K_SPC_OK:
		/* Reports successful completion. */
		return 0;
	case X68K_SPC_ERR_ARGUMENT:
		/* Returns the computed result. */
		return EINVAL;
	case X68K_SPC_ERR_TIMEOUT:
		/* Returns the computed result. */
		return ETIMEDOUT;
	case X68K_SPC_ERR_SELECTION:
		/* Returns the computed result. */
		return ENODEV;
	default:
		/* Returns the computed result. */
		return EIO;
	}
}

/* Supports the record command error operation. */
static int
record_command_error(
	struct spc_disk_unit *unit,
	uint8_t opcode,
	int error)
{
	int function_result;

	/* Checks the operation status. */
	if (error == X68K_SPC_ERR_STATUS &&
	    (unit->sense_valid || request_sense(unit) == 0)) {
		hal_printf("spc: target=%u op=%02X sense=%02X/%02X/%02X\n",
			   unit->target_id, opcode, unit->sense.key,
			   unit->sense.asc, unit->sense.ascq);
	} else {
		unit->sense_valid = 0;
		hal_printf("spc: target=%u op=%02X transport=%d\n",
			   unit->target_id, opcode, error);
	}

	/* Obtains the spc errno result. */
	function_result = spc_errno(error);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the read write operation. */
static int
read_write(
	struct spc_disk_unit *unit,
	int write,
	uint64_t lba,
	uint32_t blocks,
	void *buffer)
{
	int function_result;
	struct x68k_spc_result result;
	uint32_t chunk;
	int error, retry;
	uint8_t *bytes = buffer;

	/* Continue while the operation condition remains true. */
	while (blocks != 0) {
		retry = 0;

		/* Handles the lba condition. */
		if (lba > UINT32_MAX)
			return EOVERFLOW;
		chunk = drv_x68k_scsi_transfer_chunk(
			lba, blocks, unit->disk->d_block_count, SPC_MAX_BLOCKS);

		/* Handles the chunk condition. */
		if (chunk == 0)
			return EOVERFLOW;
		do {
			unit->sense_valid = 0;

			/* Handles the write condition. */
			if (write) {
				error = drv_x68k_spc_pio_write10(
					&controller_bus, controller_initiator,
					unit->target_id, SPC_LUN, (uint32_t)lba,
					chunk, bytes, &result);
			} else {
				error = drv_x68k_spc_pio_read10(
					&controller_bus, controller_initiator,
					unit->target_id, SPC_LUN, (uint32_t)lba,
					chunk, bytes, &result);
			}

			/* Checks the operation status. */
			if (error != X68K_SPC_ERR_STATUS ||
			    request_sense(unit) != 0 ||
			    unit->sense.key != SCSI_SENSE_UNIT_ATTENTION ||
			    retry != 0)
				break;

			/*
 * CHECK CONDITION with UNIT ATTENTION means the command
			 * was not executed.  A transport timeout is never
			 * retried for writes. */
			retry = 1;
		} while (1);

		/* Checks the operation status. */
		if (error != X68K_SPC_OK) {
			/* Obtains the record command error result. */
			function_result = record_command_error(
				unit,
				write ? X68K_SCSI_WRITE_10 : X68K_SCSI_READ_10,
				error);

			/* Returns the computed result. */
			return function_result;
		}
		lba += chunk;
		blocks -= chunk;
		bytes += (size_t)chunk * X68K_SCSI_BLOCK_SIZE;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the spc submit operation. */
static int
spc_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct x68k_spc_result result;
	int transport;
	struct spc_disk_unit *unit = disk->d_data;
	int error;

	/* Handles the bio condition. */
	if (bio->b_op == BIO_READ) {
		error = read_write(unit, 0, bio->b_mapped_block,
				   bio->b_block_count, bio->b_data);
	} else if (bio->b_op == BIO_WRITE) {
		error = read_write(unit, 1, bio->b_mapped_block,
				   bio->b_block_count, bio->b_data);
	} else if (bio->b_op == BIO_FLUSH) {
		transport = drv_x68k_spc_pio_synchronize10(
			&controller_bus, controller_initiator, unit->target_id,
			SPC_LUN, &result);
		error = transport == X68K_SPC_OK
				? 0
				: record_command_error(unit,
						       X68K_SCSI_SYNCHRONIZE_10,
						       transport);
	} else {
		error = EOPNOTSUPP;
	}
	bio_complete(bio, error,
		     error == 0
			     ? (size_t)bio->b_block_count * disk->d_block_size
			     : 0);

	/* Reports successful completion. */
	return 0;
}

/* Supports the spc ioctl operation. */
static int
spc_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	(void)disk;
	(void)request;
	(void)argument;

	/* Returns the computed result. */
	return EOPNOTSUPP;
}
