/* Pure SCSI-2 CDB, response, and transfer-boundary helpers. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "x68k-mb89352.h"

static void
zero_bytes(uint8_t *bytes, size_t length)
{
	while (length-- != 0)
		*bytes++ = 0;
}

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		(uint32_t)p[2] << 8 | p[3];
}

int
x68k_scsi_cdb10(uint8_t cdb[10], uint8_t opcode, uint32_t lba,
		uint32_t blocks)
{
	if (cdb == NULL ||
	    (opcode != X68K_SCSI_READ_10 && opcode != X68K_SCSI_WRITE_10) ||
	    blocks == 0 || blocks > X68K_SCSI_CDB10_MAX_BLOCKS)
		return -1;
	zero_bytes(cdb, 10);
	cdb[0] = opcode;
	cdb[2] = (uint8_t)(lba >> 24);
	cdb[3] = (uint8_t)(lba >> 16);
	cdb[4] = (uint8_t)(lba >> 8);
	cdb[5] = (uint8_t)lba;
	cdb[7] = (uint8_t)(blocks >> 8);
	cdb[8] = (uint8_t)blocks;
	return 0;
}

void
x68k_scsi_inquiry_cdb(uint8_t cdb[6], uint8_t allocation_length)
{
	zero_bytes(cdb, 6);
	cdb[0] = X68K_SCSI_INQUIRY;
	cdb[4] = allocation_length;
}

void
x68k_scsi_request_sense_cdb(uint8_t cdb[6], uint8_t allocation_length)
{
	zero_bytes(cdb, 6);
	cdb[0] = X68K_SCSI_REQUEST_SENSE;
	cdb[4] = allocation_length;
}

void
x68k_scsi_simple_cdb10(uint8_t cdb[10], uint8_t opcode)
{
	zero_bytes(cdb, 10);
	cdb[0] = opcode;
}

int
x68k_scsi_parse_capacity10(const uint8_t response[8], uint64_t *blocks,
		uint32_t *block_size)
{
	uint32_t last, size;
	if (response == NULL || blocks == NULL || block_size == NULL)
		return -1;
	last = be32(response);
	size = be32(response + 4);
	/* 0xffffffff requests READ CAPACITY(16), deliberately outside v1. */
	if (last == UINT32_MAX || size != X68K_SCSI_BLOCK_SIZE)
		return -1;
	*blocks = (uint64_t)last + 1U;
	*block_size = size;
	return 0;
}

int
x68k_scsi_parse_sense(const uint8_t *response, size_t length,
		struct x68k_scsi_sense *sense)
{
	size_t declared;
	if (response == NULL || sense == NULL || length < 8U ||
	    (response[0] & 0x7eU) != 0x70U)
		return -1;
	declared = 8U + response[7];
	if (declared > length || declared < 14U)
		return -1;
	sense->key = response[2] & 0x0fU;
	sense->asc = response[12];
	sense->ascq = response[13];
	return 0;
}

uint32_t
x68k_scsi_transfer_chunk(uint64_t lba, uint32_t blocks,
		uint64_t capacity, uint32_t driver_limit)
{
	uint64_t available;
	uint32_t chunk;
	if (blocks == 0 || driver_limit == 0 || lba >= capacity)
		return 0;
	available = capacity - lba;
	chunk = blocks;
	if (chunk > X68K_SCSI_CDB10_MAX_BLOCKS)
		chunk = X68K_SCSI_CDB10_MAX_BLOCKS;
	if (chunk > driver_limit)
		chunk = driver_limit;
	if ((uint64_t)chunk > available)
		chunk = (uint32_t)available;
	return chunk;
}

/*
 * The first X68k driver deliberately uses neither HD63450 DMA nor SPC IRQs.
 * SCMD_PROGRAM_TRANSFER keeps the transfer on the SPC data-register path.
 */
#define SPC_DEFAULT_POLL_LIMIT 1000000U
#define SPC_MAX_TRANSFER       0x00ffffffU
#define SPC_IDENTIFY(lun)      (0x80U | (lun)) /* disconnect not permitted */

static uint32_t
poll_limit(const struct x68k_spc_bus *bus)
{
	return bus->poll_limit != 0 ? bus->poll_limit : SPC_DEFAULT_POLL_LIMIT;
}

static void
poll_relax(const struct x68k_spc_bus *bus)
{
	if (bus->relax != NULL)
		bus->relax(bus->cookie);
}

static uint8_t
spc_read(const struct x68k_spc_bus *bus, unsigned reg)
{
	return bus->read(bus->cookie, reg);
}

static void
spc_write(const struct x68k_spc_bus *bus, unsigned reg, uint8_t value)
{
	bus->write(bus->cookie, reg, value);
}

static int
valid_bus(const struct x68k_spc_bus *bus)
{
	return bus != NULL && bus->read != NULL && bus->write != NULL;
}

static int
controller_error(const struct x68k_spc_bus *bus, uint8_t interrupts)
{
	if ((interrupts & X68K_SPC_INTS_HARD_ERROR) != 0 ||
	    spc_read(bus, X68K_SPC_SERR) != 0)
		return X68K_SPC_ERR_CONTROLLER;
	return X68K_SPC_OK;
}

static int
wait_bus_free(const struct x68k_spc_bus *bus)
{
	uint32_t count = poll_limit(bus);
	while (count-- != 0) {
		uint8_t ints = spc_read(bus, X68K_SPC_INTS);
		int error = controller_error(bus, ints);
		if (error != X68K_SPC_OK)
			return error;
		/* COMMAND DONE is latched after the command-complete message.  The
		 * X68030 SPC does not expose the following BUS FREE state until this
		 * completion is acknowledged.  Polling mode must perform that
		 * acknowledgement itself because no IRQ handler will do it. */
		if (ints != 0)
			spc_write(bus, X68K_SPC_INTS, ints);
		if ((spc_read(bus, X68K_SPC_PSNS) & X68K_SPC_PSNS_BUSY) == 0)
			return X68K_SPC_OK;
		poll_relax(bus);
	}
	return X68K_SPC_ERR_TIMEOUT;
}

static int
wait_phase(const struct x68k_spc_bus *bus, uint8_t *phase,
	uint8_t *interrupts)
{
	uint32_t count = poll_limit(bus);
	while (count-- != 0) {
		uint8_t ints = spc_read(bus, X68K_SPC_INTS);
		int error = controller_error(bus, ints);
		uint8_t sense;
		if (error != X68K_SPC_OK)
			return error;
		if ((ints & X68K_SPC_INTS_TIMEOUT) != 0)
			return X68K_SPC_ERR_SELECTION;
		if ((ints & X68K_SPC_INTS_DISCONNECT) != 0) {
			*interrupts = ints;
			return X68K_SPC_ERR_DISCONNECT;
		}
		sense = spc_read(bus, X68K_SPC_PSNS);
		if ((sense & X68K_SPC_PSNS_REQUEST) != 0) {
			*phase = sense & X68K_SPC_PHASE_MASK;
			*interrupts = ints;
			return X68K_SPC_OK;
		}
		if (ints != 0)
			spc_write(bus, X68K_SPC_INTS, ints);
		poll_relax(bus);
	}
	return X68K_SPC_ERR_TIMEOUT;
}

static void
set_transfer_count(const struct x68k_spc_bus *bus, size_t length)
{
	spc_write(bus, X68K_SPC_TCH, (uint8_t)(length >> 16));
	spc_write(bus, X68K_SPC_TCM, (uint8_t)(length >> 8));
	spc_write(bus, X68K_SPC_TCL, (uint8_t)length);
}

static int
pio_out(const struct x68k_spc_bus *bus, uint8_t phase,
	const uint8_t *data, size_t length, size_t *transferred)
{
	size_t done = 0;
	if (length == 0 || length > SPC_MAX_TRANSFER)
		return X68K_SPC_ERR_ARGUMENT;
	set_transfer_count(bus, length);
	spc_write(bus, X68K_SPC_PCTL,
	    X68K_SPC_PCTL_BUS_FREE_INT | phase);
	spc_write(bus, X68K_SPC_SCMD,
	    X68K_SPC_SCMD_TRANSFER | X68K_SPC_SCMD_PROGRAM_TRANSFER);
	while (done != length) {
		uint32_t count = poll_limit(bus);
		while (count-- != 0) {
			uint8_t ints = spc_read(bus, X68K_SPC_INTS);
			int error = controller_error(bus, ints);
			if (error != X68K_SPC_OK)
				return error;
			if ((ints & (X68K_SPC_INTS_TIMEOUT |
			    X68K_SPC_INTS_DISCONNECT)) != 0)
				return X68K_SPC_ERR_DISCONNECT;
			if ((spc_read(bus, X68K_SPC_SSTS) &
			    X68K_SPC_SSTS_DREG_FULL) == 0)
				break;
			poll_relax(bus);
		}
		if (count == UINT32_MAX)
			return X68K_SPC_ERR_TIMEOUT;
		spc_write(bus, X68K_SPC_DREG, data[done++]);
	}
	*transferred += done;
	return X68K_SPC_OK;
}

static int
pio_in(const struct x68k_spc_bus *bus, uint8_t phase, uint8_t *data,
	size_t length, size_t *transferred)
{
	size_t done = 0;
	if (length == 0 || length > SPC_MAX_TRANSFER)
		return X68K_SPC_ERR_ARGUMENT;
	set_transfer_count(bus, length);
	spc_write(bus, X68K_SPC_PCTL,
	    X68K_SPC_PCTL_BUS_FREE_INT | phase);
	spc_write(bus, X68K_SPC_SCMD,
	    X68K_SPC_SCMD_TRANSFER | X68K_SPC_SCMD_PROGRAM_TRANSFER);
	while (done != length) {
		uint32_t count = poll_limit(bus);
		while (count-- != 0) {
			uint8_t ints = spc_read(bus, X68K_SPC_INTS);
			int error = controller_error(bus, ints);
			if (error != X68K_SPC_OK)
				return error;
			if ((spc_read(bus, X68K_SPC_SSTS) &
			    X68K_SPC_SSTS_DREG_EMPTY) == 0)
				break;
			if ((ints & (X68K_SPC_INTS_TIMEOUT |
			    X68K_SPC_INTS_DISCONNECT)) != 0)
				return X68K_SPC_ERR_DISCONNECT;
			poll_relax(bus);
		}
		if (count == UINT32_MAX)
			return X68K_SPC_ERR_TIMEOUT;
		data[done++] = spc_read(bus, X68K_SPC_DREG);
	}
	*transferred += done;
	return X68K_SPC_OK;
}

int
x68k_spc_pio_init(const struct x68k_spc_bus *bus, unsigned initiator_id)
{
	uint32_t delay;
	if (!valid_bus(bus) || initiator_id > 7U)
		return X68K_SPC_ERR_ARGUMENT;
	spc_write(bus, X68K_SPC_SCTL,
	    X68K_SPC_SCTL_DISABLE | X68K_SPC_SCTL_CONTROLLER_RESET);
	spc_write(bus, X68K_SPC_SCMD, 0);
	spc_write(bus, X68K_SPC_TMOD, 0);
	spc_write(bus, X68K_SPC_PCTL, 0);
	spc_write(bus, X68K_SPC_TEMP, 0);
	set_transfer_count(bus, 0);
	spc_write(bus, X68K_SPC_INTS, 0xffU);
	spc_write(bus, X68K_SPC_SCTL,
	    X68K_SPC_SCTL_DISABLE | X68K_SPC_SCTL_ABORT_ENABLE |
	    X68K_SPC_SCTL_PARITY_ENABLE);
	spc_write(bus, X68K_SPC_BDID, (uint8_t)initiator_id);
	for (delay = 0; delay < 400U; delay++)
		poll_relax(bus);
	/* IRQ and reselection stay disabled for the whole v1 transaction. */
	spc_write(bus, X68K_SPC_SCTL,
	    X68K_SPC_SCTL_ABORT_ENABLE | X68K_SPC_SCTL_PARITY_ENABLE);
	return wait_bus_free(bus);
}

static int
select_target(const struct x68k_spc_bus *bus, unsigned initiator_id,
	unsigned target_id)
{
	uint32_t count;
	if (wait_bus_free(bus) != X68K_SPC_OK)
		return X68K_SPC_ERR_TIMEOUT;
	spc_write(bus, X68K_SPC_INTS, 0xffU);
	spc_write(bus, X68K_SPC_PCTL, 0);
	spc_write(bus, X68K_SPC_TEMP,
	    (uint8_t)((1U << initiator_id) | (1U << target_id)));
	spc_write(bus, X68K_SPC_TCH, 2U);
	spc_write(bus, X68K_SPC_TCM, 113U);
	spc_write(bus, X68K_SPC_TCL, 3U);
	spc_write(bus, X68K_SPC_SCMD, X68K_SPC_SCMD_SELECT);
	count = poll_limit(bus);
	while (count-- != 0) {
		uint8_t ints = spc_read(bus, X68K_SPC_INTS);
		int error = controller_error(bus, ints);
		if (error != X68K_SPC_OK)
			return error;
		if ((ints & X68K_SPC_INTS_TIMEOUT) != 0)
			return X68K_SPC_ERR_SELECTION;
		if ((ints & X68K_SPC_INTS_COMMAND_DONE) != 0) {
			spc_write(bus, X68K_SPC_INTS, ints);
			return X68K_SPC_OK;
		}
		poll_relax(bus);
	}
	return X68K_SPC_ERR_TIMEOUT;
}

int
x68k_spc_pio_command(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	const uint8_t *cdb, size_t cdb_length, void *data, size_t data_length,
	enum x68k_spc_direction direction, struct x68k_spc_result *result)
{
	uint8_t identify, status = 0xffU, message = 0xffU;
	uint8_t *bytes = data;
	size_t data_done = 0, ignored = 0;
	unsigned steps;
	int command_done = 0, error;
	if (!valid_bus(bus) || initiator_id > 7U || target_id > 7U ||
	    initiator_id == target_id || lun > 7U || cdb == NULL ||
	    (cdb_length != 6U && cdb_length != 10U && cdb_length != 12U) ||
	    direction > X68K_SPC_DATA_OUT ||
	    ((direction == X68K_SPC_DATA_NONE) != (data_length == 0)) ||
	    (data_length != 0 && data == NULL) || data_length > SPC_MAX_TRANSFER)
		return X68K_SPC_ERR_ARGUMENT;
	if (result != NULL) {
		result->status = 0xffU;
		result->message = 0xffU;
		result->transferred = 0;
	}
	error = select_target(bus, initiator_id, target_id);
	if (error != X68K_SPC_OK)
		return error;
	identify = (uint8_t)SPC_IDENTIFY(lun);
	for (steps = 0; steps < 32U; steps++) {
		uint8_t phase, ints = 0;
		error = wait_phase(bus, &phase, &ints);
		if (error == X68K_SPC_ERR_DISCONNECT)
			return command_done ? X68K_SPC_OK : error;
		if (error != X68K_SPC_OK)
			return error;
		if (ints != 0)
			spc_write(bus, X68K_SPC_INTS, ints);
		switch (phase) {
		case X68K_SPC_PHASE_MESSAGE_OUT:
			error = pio_out(bus, phase, &identify, 1U, &ignored);
			break;
		case X68K_SPC_PHASE_COMMAND:
			error = pio_out(bus, phase, cdb, cdb_length, &ignored);
			break;
		case X68K_SPC_PHASE_DATA_IN:
			if (direction != X68K_SPC_DATA_IN || data_done >= data_length)
				return X68K_SPC_ERR_PHASE;
			error = pio_in(bus, phase, bytes + data_done,
			    data_length - data_done, &data_done);
			break;
		case X68K_SPC_PHASE_DATA_OUT:
			if (direction != X68K_SPC_DATA_OUT || data_done >= data_length)
				return X68K_SPC_ERR_PHASE;
			error = pio_out(bus, phase, bytes + data_done,
			    data_length - data_done, &data_done);
			break;
		case X68K_SPC_PHASE_STATUS:
			error = pio_in(bus, phase, &status, 1U, &ignored);
			break;
		case X68K_SPC_PHASE_MESSAGE_IN:
			error = pio_in(bus, phase, &message, 1U, &ignored);
			if (error == X68K_SPC_OK) {
				/* Programmed transfer deliberately leaves ACK asserted
				 * after the last MESSAGE IN byte.  Release it so the
				 * target can deassert REQ/BSY and enter BUS FREE. */
				spc_write(bus, X68K_SPC_SCMD,
				    X68K_SPC_SCMD_RESET_ACK);
				if (message == X68K_SCSI_MESSAGE_COMMAND_COMPLETE)
					command_done = 1;
			}
			break;
		default:
			return X68K_SPC_ERR_PHASE;
		}
		if (error != X68K_SPC_OK)
			return error;
		if (command_done)
			break;
	}
	if (result != NULL) {
		result->status = status;
		result->message = message;
		result->transferred = data_done;
	}
	if (!command_done || message != X68K_SCSI_MESSAGE_COMMAND_COMPLETE)
		return X68K_SPC_ERR_MESSAGE;
	if (status != X68K_SCSI_STATUS_GOOD)
		return X68K_SPC_ERR_STATUS;
	if (data_done != data_length)
		return X68K_SPC_ERR_RESIDUAL;
	/* Do not let the next command inherit the preceding target's BUSY
	 * state or completion interrupt. */
	return wait_bus_free(bus);
}

int
x68k_spc_pio_read10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun, uint32_t lba,
	uint32_t blocks, void *buffer, struct x68k_spc_result *result)
{
	uint8_t cdb[10];
	size_t length;
	if (blocks == 0 || blocks > SPC_MAX_TRANSFER / X68K_SCSI_BLOCK_SIZE ||
	    x68k_scsi_cdb10(cdb, X68K_SCSI_READ_10, lba, blocks) != 0)
		return X68K_SPC_ERR_ARGUMENT;
	length = (size_t)blocks * X68K_SCSI_BLOCK_SIZE;
	return x68k_spc_pio_command(bus, initiator_id, target_id, lun,
	    cdb, sizeof(cdb), buffer, length, X68K_SPC_DATA_IN, result);
}

int
x68k_spc_pio_write10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun, uint32_t lba,
	uint32_t blocks, const void *buffer, struct x68k_spc_result *result)
{
	uint8_t cdb[10];
	size_t length;
	if (blocks == 0 || blocks > SPC_MAX_TRANSFER / X68K_SCSI_BLOCK_SIZE ||
	    x68k_scsi_cdb10(cdb, X68K_SCSI_WRITE_10, lba, blocks) != 0)
		return X68K_SPC_ERR_ARGUMENT;
	length = (size_t)blocks * X68K_SCSI_BLOCK_SIZE;
	return x68k_spc_pio_command(bus, initiator_id, target_id, lun,
	    cdb, sizeof(cdb), (void *)buffer, length, X68K_SPC_DATA_OUT,
	    result);
}

int
x68k_spc_pio_request_sense(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	uint8_t response[18], struct x68k_spc_result *result)
{
	uint8_t cdb[6];
	x68k_scsi_request_sense_cdb(cdb, 18U);
	return x68k_spc_pio_command(bus, initiator_id, target_id, lun,
	    cdb, sizeof(cdb), response, 18U, X68K_SPC_DATA_IN, result);
}

int
x68k_spc_pio_inquiry(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	uint8_t response[36], struct x68k_spc_result *result)
{
	uint8_t cdb[6];
	x68k_scsi_inquiry_cdb(cdb, 36U);
	return x68k_spc_pio_command(bus, initiator_id, target_id, lun,
	    cdb, sizeof(cdb), response, 36U, X68K_SPC_DATA_IN, result);
}

int
x68k_spc_pio_read_capacity10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	uint8_t response[8], struct x68k_spc_result *result)
{
	uint8_t cdb[10];
	x68k_scsi_simple_cdb10(cdb, X68K_SCSI_READ_CAPACITY_10);
	return x68k_spc_pio_command(bus, initiator_id, target_id, lun,
	    cdb, sizeof(cdb), response, 8U, X68K_SPC_DATA_IN, result);
}

int
x68k_spc_pio_test_unit_ready(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	struct x68k_spc_result *result)
{
	uint8_t cdb[6];
	zero_bytes(cdb, sizeof(cdb));
	cdb[0] = X68K_SCSI_TEST_UNIT_READY;
	return x68k_spc_pio_command(bus, initiator_id, target_id, lun,
	    cdb, sizeof(cdb), NULL, 0, X68K_SPC_DATA_NONE, result);
}

int
x68k_spc_pio_synchronize10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	struct x68k_spc_result *result)
{
	uint8_t cdb[10];
	x68k_scsi_simple_cdb10(cdb, X68K_SCSI_SYNCHRONIZE_10);
	return x68k_spc_pio_command(bus, initiator_id, target_id, lun,
	    cdb, sizeof(cdb), NULL, 0, X68K_SPC_DATA_NONE, result);
}
