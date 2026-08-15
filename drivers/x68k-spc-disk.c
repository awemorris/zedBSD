/* X68000 MB89352 synchronous polled-PIO block driver. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "drivers/x68k-spc-disk.h"

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

static void
zero_bytes(void *pointer, size_t length)
{
	uint8_t *bytes = pointer;
	while (length-- != 0)
		*bytes++ = 0;
}

static int
spc_errno(int error)
{
	switch (error) {
	case X68K_SPC_OK: return 0;
	case X68K_SPC_ERR_ARGUMENT: return EINVAL;
	case X68K_SPC_ERR_TIMEOUT: return ETIMEDOUT;
	case X68K_SPC_ERR_SELECTION: return ENODEV;
	default: return EIO;
	}
}

static int
request_sense(struct spc_disk_unit *unit)
{
	uint8_t response[18];
	struct x68k_spc_result result;
	int error;
	unit->sense_valid = 0;
	error = x68k_spc_pio_request_sense(&controller_bus,
	    controller_initiator, unit->target_id, SPC_LUN, response, &result);
	if (error != X68K_SPC_OK ||
	    x68k_scsi_parse_sense(response, sizeof(response), &unit->sense) != 0)
		return EIO;
	unit->sense_valid = 1;
	return 0;
}

static int
record_command_error(struct spc_disk_unit *unit, uint8_t opcode, int error)
{
	if (error == X68K_SPC_ERR_STATUS &&
	    (unit->sense_valid || request_sense(unit) == 0)) {
		hal_printf("spc: target=%u op=%02X sense=%02X/%02X/%02X\n",
		    unit->target_id, opcode, unit->sense.key, unit->sense.asc,
		    unit->sense.ascq);
	} else {
		unit->sense_valid = 0;
		hal_printf("spc: target=%u op=%02X transport=%d\n",
		    unit->target_id, opcode, error);
	}
	return spc_errno(error);
}

static int
read_write(struct spc_disk_unit *unit, int write, uint64_t lba,
	uint32_t blocks, void *buffer)
{
	uint8_t *bytes = buffer;
	while (blocks != 0) {
		struct x68k_spc_result result;
		uint32_t chunk;
		int error, retry = 0;
		if (lba > UINT32_MAX)
			return EOVERFLOW;
		chunk = x68k_scsi_transfer_chunk(lba, blocks,
		    unit->disk->d_block_count, SPC_MAX_BLOCKS);
		if (chunk == 0)
			return EOVERFLOW;
		do {
			unit->sense_valid = 0;
			if (write)
				error = x68k_spc_pio_write10(&controller_bus,
				    controller_initiator, unit->target_id, SPC_LUN,
				    (uint32_t)lba, chunk, bytes, &result);
			else
				error = x68k_spc_pio_read10(&controller_bus,
				    controller_initiator, unit->target_id, SPC_LUN,
				    (uint32_t)lba, chunk, bytes, &result);
			if (error != X68K_SPC_ERR_STATUS || request_sense(unit) != 0 ||
			    unit->sense.key != SCSI_SENSE_UNIT_ATTENTION || retry != 0)
				break;
			/* CHECK CONDITION with UNIT ATTENTION means the command was not
			 * executed.  A transport timeout is never retried for writes. */
			retry = 1;
		} while (1);
		if (error != X68K_SPC_OK)
			return record_command_error(unit,
			    write ? X68K_SCSI_WRITE_10 : X68K_SCSI_READ_10, error);
		lba += chunk;
		blocks -= chunk;
		bytes += (size_t)chunk * X68K_SCSI_BLOCK_SIZE;
	}
	return 0;
}

static int
spc_submit(struct disk *disk, struct bio *bio)
{
	struct spc_disk_unit *unit = disk->d_data;
	int error;
	if (bio->b_op == BIO_READ)
		error = read_write(unit, 0, bio->b_mapped_block,
		    bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_WRITE)
		error = read_write(unit, 1, bio->b_mapped_block,
		    bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_FLUSH) {
		struct x68k_spc_result result;
		int transport = x68k_spc_pio_synchronize10(&controller_bus,
		    controller_initiator, unit->target_id, SPC_LUN, &result);
		error = transport == X68K_SPC_OK ? 0 :
		    record_command_error(unit, X68K_SCSI_SYNCHRONIZE_10,
		    transport);
	} else {
		error = EOPNOTSUPP;
	}
	bio_complete(bio, error, error == 0 ?
	    (size_t)bio->b_block_count * disk->d_block_size : 0);
	return 0;
}

static int
spc_ioctl(struct disk *disk, unsigned long request, void *argument)
{
	(void)disk;
	(void)request;
	(void)argument;
	return EOPNOTSUPP;
}

static const struct disk_ops spc_ops = {
	.submit = spc_submit,
	.ioctl = spc_ioctl,
};

static int
unit_ready(struct spc_disk_unit *unit)
{
	unsigned attempt;
	for (attempt = 0; attempt < 3U; attempt++) {
		struct x68k_spc_result result;
		int error = x68k_spc_pio_test_unit_ready(&controller_bus,
		    controller_initiator, unit->target_id, SPC_LUN, &result);
		if (error == X68K_SPC_OK)
			return 1;
		if (error != X68K_SPC_ERR_STATUS || request_sense(unit) != 0)
			return 0;
		if (unit->sense.key != SCSI_SENSE_UNIT_ATTENTION &&
		    !(unit->sense.key == SCSI_SENSE_NOT_READY &&
		      unit->sense.asc == SCSI_ASC_BECOMING_READY))
			return 0;
	}
	return 0;
}

static void
sanitize(char *output, const uint8_t *input, unsigned length)
{
	unsigned index;
	for (index = 0; index < length; index++) {
		uint8_t value = input[index];
		output[index] = value >= 0x20U && value <= 0x7eU ?
		    (char)value : '?';
	}
	while (length != 0 && output[length - 1U] == ' ')
		length--;
	output[length] = '\0';
}

static int
probe_target(unsigned target_id)
{
	uint8_t inquiry[36], capacity[8];
	char vendor[9], product[17], revision[5];
	struct x68k_spc_result result;
	struct spc_disk_unit *unit = &units[target_id];
	uint64_t blocks;
	uint32_t block_size;
	int error;
	unit->target_id = target_id;
	if (!unit_ready(unit))
		return 0;
	error = x68k_spc_pio_inquiry(&controller_bus, controller_initiator,
	    target_id, SPC_LUN, inquiry, &result);
	if (error != X68K_SPC_OK || (inquiry[0] & 0xe0U) != 0 ||
	    (inquiry[0] & 0x1fU) != 0)
		return 0;
	error = x68k_spc_pio_read_capacity10(&controller_bus,
	    controller_initiator, target_id, SPC_LUN, capacity, &result);
	if (error != X68K_SPC_OK ||
	    x68k_scsi_parse_capacity10(capacity, &blocks, &block_size) != 0)
		return 0;
	unit->disk = disk_alloc();
	if (unit->disk == NULL)
		return 0;
	unit->ordinal = present_count;
	unit->disk->d_name[0] = 's';
	unit->disk->d_name[1] = 'd';
	unit->disk->d_name[2] = (char)('0' + present_count);
	unit->disk->d_name[3] = '\0';
	unit->disk->d_flags = (inquiry[1] & 0x80U) != 0 ? DISK_REMOVABLE : 0;
	unit->disk->d_block_size = block_size;
	unit->disk->d_block_count = blocks;
	unit->disk->d_max_transfer_blocks = SPC_MAX_BLOCKS;
	unit->disk->d_ops = &spc_ops;
	unit->disk->d_data = unit;
	if (disk_create(unit->disk) != 0) {
		unit->disk = NULL;
		return 0;
	}
	sanitize(vendor, inquiry + 8U, 8U);
	sanitize(product, inquiry + 16U, 16U);
	sanitize(revision, inquiry + 32U, 4U);
	hal_printf("spc: %s target=%u blocks=%llu %s %s %s\n",
	    unit->disk->d_name, target_id, blocks, vendor, product, revision);
	present_count++;
	return 1;
}

unsigned
x68k_spc_disk_init(const struct x68k_spc_bus *bus, unsigned initiator_id,
	unsigned boot_target_id)
{
	unsigned target;
	if (bus == NULL || bus->read == NULL || bus->write == NULL ||
	    initiator_id > 7U || boot_target_id >= SPC_TARGET_COUNT ||
	    initiator_id == boot_target_id)
		return 0;
	controller_bus = *bus;
	controller_initiator = initiator_id;
	present_count = 0;
	zero_bytes(units, sizeof(units));
	if (x68k_spc_pio_init(&controller_bus, initiator_id) != X68K_SPC_OK)
		return 0;
	(void)probe_target(boot_target_id);
	for (target = 0; target < SPC_TARGET_COUNT; target++)
		if (target != boot_target_id && target != initiator_id)
			(void)probe_target(target);
	return present_count;
}

struct disk *
x68k_spc_disk_target(unsigned target_id)
{
	return target_id < SPC_TARGET_COUNT ? units[target_id].disk : NULL;
}
