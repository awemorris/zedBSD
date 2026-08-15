/* X68k SCSI CDB, capacity, sense, and chunking tests. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdio.h>
#include <string.h>
#include "drivers/x68k-mb89352.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); return 1; \
} } while (0)

enum fake_phase {
	FAKE_FREE, FAKE_MSGOUT, FAKE_COMMAND, FAKE_DATAIN, FAKE_DATAOUT,
	FAKE_STATUS, FAKE_MSGIN
};

struct fake_spc {
	enum fake_phase phase;
	uint8_t regs[16];
	uint8_t cdb[10];
	unsigned cdb_bytes;
	unsigned cdb_expected;
	unsigned data_bytes;
	unsigned data_expected;
	uint8_t first_data;
	uint8_t last_data;
	unsigned transfer_commands;
	unsigned reset_ack_commands;
	int irq_enabled;
	int non_program_transfer;
	int protocol_error;
	int selection_timeout;
	int stall_data_in;
};

static uint8_t
fake_read(void *cookie, unsigned reg)
{
	struct fake_spc *fake = cookie;
	if (reg == X68K_SPC_INTS) {
		if (fake->selection_timeout)
			return X68K_SPC_INTS_TIMEOUT;
		return fake->phase == FAKE_MSGOUT ?
		    X68K_SPC_INTS_COMMAND_DONE : 0;
	}
	if (reg == X68K_SPC_PSNS) {
		static const uint8_t phases[] = {
			0, X68K_SPC_PHASE_MESSAGE_OUT, X68K_SPC_PHASE_COMMAND,
			X68K_SPC_PHASE_DATA_IN, X68K_SPC_PHASE_DATA_OUT,
			X68K_SPC_PHASE_STATUS, X68K_SPC_PHASE_MESSAGE_IN
		};
		return fake->phase == FAKE_FREE ? 0 :
		    (uint8_t)(X68K_SPC_PSNS_BUSY |
		    X68K_SPC_PSNS_REQUEST | phases[fake->phase]);
	}
	if (reg == X68K_SPC_SERR)
		return 0;
	if (reg == X68K_SPC_SSTS) {
		if (fake->stall_data_in && fake->phase == FAKE_DATAIN)
			return X68K_SPC_SSTS_DREG_EMPTY;
		return fake->phase == FAKE_DATAIN ||
		    fake->phase == FAKE_STATUS || fake->phase == FAKE_MSGIN ? 0 :
		    X68K_SPC_SSTS_DREG_EMPTY;
	}
	if (reg == X68K_SPC_DREG) {
		if (fake->phase == FAKE_DATAIN) {
			uint8_t value = (uint8_t)fake->data_bytes++;
			if (fake->data_bytes == fake->data_expected)
				fake->phase = FAKE_STATUS;
			return value;
		}
		if (fake->phase == FAKE_STATUS) {
			fake->phase = FAKE_MSGIN;
			return X68K_SCSI_STATUS_GOOD;
		}
		if (fake->phase == FAKE_MSGIN)
			return X68K_SCSI_MESSAGE_COMMAND_COMPLETE;
	}
	return fake->regs[reg];
}

static void
fake_write(void *cookie, unsigned reg, uint8_t value)
{
	struct fake_spc *fake = cookie;
	fake->regs[reg] = value;
	if (reg == X68K_SPC_SCTL &&
	    (value & X68K_SPC_SCTL_INTERRUPT_ENABLE) != 0)
		fake->irq_enabled = 1;
	if (reg == X68K_SPC_SCMD && value == X68K_SPC_SCMD_SELECT) {
		fake->cdb_bytes = 0;
		fake->cdb_expected = 0;
		fake->data_bytes = 0;
		fake->data_expected = 0;
		fake->phase = FAKE_MSGOUT;
		return;
	}
	if (reg == X68K_SPC_SCMD && value == X68K_SPC_SCMD_RESET_ACK) {
		fake->reset_ack_commands++;
		if (fake->phase != FAKE_MSGIN)
			fake->protocol_error = 1;
		else
			fake->phase = FAKE_FREE;
		return;
	}
	if (reg == X68K_SPC_SCMD &&
	    (value & X68K_SPC_SCMD_TRANSFER) != 0) {
		fake->transfer_commands++;
		if ((value & X68K_SPC_SCMD_PROGRAM_TRANSFER) == 0)
			fake->non_program_transfer = 1;
		return;
	}
	if (reg != X68K_SPC_DREG)
		return;
	if (fake->phase == FAKE_MSGOUT) {
		if (value != 0x80U)
			fake->protocol_error = 1;
		fake->phase = FAKE_COMMAND;
	} else if (fake->phase == FAKE_COMMAND) {
		if (fake->cdb_bytes == 0)
			fake->cdb_expected = (value >> 5) == 0 ? 6U : 10U;
		if (fake->cdb_bytes < sizeof(fake->cdb))
			fake->cdb[fake->cdb_bytes++] = value;
		if (fake->cdb_bytes == fake->cdb_expected) {
			if (fake->cdb[0] == X68K_SCSI_READ_10) {
				fake->data_expected = X68K_SCSI_BLOCK_SIZE;
				fake->phase = FAKE_DATAIN;
			} else if (fake->cdb[0] == X68K_SCSI_WRITE_10) {
				fake->data_expected = X68K_SCSI_BLOCK_SIZE;
				fake->phase = FAKE_DATAOUT;
			} else if (fake->cdb[0] == X68K_SCSI_REQUEST_SENSE) {
				fake->data_expected = 18U;
				fake->phase = FAKE_DATAIN;
			} else if (fake->cdb[0] == X68K_SCSI_INQUIRY) {
				fake->data_expected = 36U;
				fake->phase = FAKE_DATAIN;
			} else if (fake->cdb[0] == X68K_SCSI_READ_CAPACITY_10) {
				fake->data_expected = 8U;
				fake->phase = FAKE_DATAIN;
			} else {
				fake->phase = FAKE_STATUS;
			}
		}
	} else if (fake->phase == FAKE_DATAOUT) {
		if (fake->data_bytes == 0)
			fake->first_data = value;
		fake->last_data = value;
		fake->data_bytes++;
		if (fake->data_bytes == fake->data_expected)
			fake->phase = FAKE_STATUS;
	}
}

static void
fake_relax(void *cookie)
{
	(void)cookie;
}

int
main(void)
{
	uint8_t cdb[10], capacity[8] = { 0, 0, 0x0f, 0xff, 0, 0, 2, 0 };
	uint8_t raw_sense[18] = { 0x70, 0, 6, 0, 0, 0, 0, 10,
		0, 0, 0, 0, 0x29, 0 };
	struct x68k_scsi_sense sense;
	struct fake_spc fake;
	struct x68k_spc_bus bus;
	struct x68k_spc_result result;
	uint8_t block[X68K_SCSI_BLOCK_SIZE];
	uint64_t blocks;
	uint32_t size;

	memset(cdb, 0xaa, sizeof(cdb));
	CHECK(x68k_scsi_cdb10(cdb, X68K_SCSI_READ_10, 0x12345678U,
	    0x3456U) == 0);
	CHECK(cdb[0] == 0x28 && cdb[2] == 0x12 && cdb[3] == 0x34 &&
	    cdb[4] == 0x56 && cdb[5] == 0x78 && cdb[7] == 0x34 &&
	    cdb[8] == 0x56 && cdb[9] == 0);
	CHECK(x68k_scsi_cdb10(cdb, 0xff, 0, 1) != 0);
	CHECK(x68k_scsi_cdb10(cdb, X68K_SCSI_WRITE_10, 0, 65536U) != 0);
	CHECK(x68k_scsi_parse_capacity10(capacity, &blocks, &size) == 0);
	CHECK(blocks == 4096U && size == 512U);
	capacity[0] = capacity[1] = capacity[2] = capacity[3] = 0xff;
	CHECK(x68k_scsi_parse_capacity10(capacity, &blocks, &size) != 0);
	CHECK(x68k_scsi_parse_sense(raw_sense, sizeof(raw_sense), &sense) == 0);
	CHECK(sense.key == 6 && sense.asc == 0x29 && sense.ascq == 0);
	CHECK(x68k_scsi_parse_sense(raw_sense, 13, &sense) != 0);
	CHECK(x68k_scsi_transfer_chunk(100, 70000, 100000, 4096) == 4096);
	CHECK(x68k_scsi_transfer_chunk(99999, 5, 100000, 4096) == 1);
	CHECK(x68k_scsi_transfer_chunk(100000, 1, 100000, 4096) == 0);
	memset(&fake, 0, sizeof(fake));
	memset(&bus, 0, sizeof(bus));
	bus.cookie = &fake;
	bus.read = fake_read;
	bus.write = fake_write;
	bus.relax = fake_relax;
	bus.poll_limit = 1000;
	CHECK(x68k_spc_pio_init(&bus, 7) == X68K_SPC_OK);
	CHECK(!fake.irq_enabled);
	CHECK((fake.regs[X68K_SPC_SCTL] &
	    (X68K_SPC_SCTL_INTERRUPT_ENABLE |
	    X68K_SPC_SCTL_RESELECT_ENABLE)) == 0);
	CHECK(x68k_spc_pio_read10(&bus, 7, 0, 0, 9, 1, block,
	    &result) == X68K_SPC_OK);
	CHECK(fake.cdb_bytes == 10 && fake.cdb[0] == X68K_SCSI_READ_10 &&
	    fake.cdb[5] == 9 && fake.cdb[8] == 1);
	CHECK(fake.data_bytes == sizeof(block));
	CHECK(result.status == X68K_SCSI_STATUS_GOOD &&
	    result.message == X68K_SCSI_MESSAGE_COMMAND_COMPLETE &&
	    result.transferred == sizeof(block));
	CHECK(fake.transfer_commands == 5 && !fake.non_program_transfer &&
	    !fake.protocol_error);
	CHECK(fake.reset_ack_commands == 1);
	CHECK(block[0] == 0 && block[1] == 1 && block[511] == 0xff);
	for (unsigned index = 0; index < sizeof(block); index++)
		block[index] = (uint8_t)(index ^ 0x5aU);
	CHECK(x68k_spc_pio_write10(&bus, 7, 0, 0, 10, 1, block,
	    &result) == X68K_SPC_OK);
	CHECK(fake.cdb_bytes == 10 && fake.cdb[0] == X68K_SCSI_WRITE_10 &&
	    fake.cdb[5] == 10 && fake.cdb[8] == 1);
	CHECK(fake.data_bytes == sizeof(block) &&
	    fake.first_data == (uint8_t)(0 ^ 0x5aU) &&
	    fake.last_data == (uint8_t)(511U ^ 0x5aU));
	CHECK(result.transferred == sizeof(block));
	CHECK(fake.reset_ack_commands == 2);
	CHECK(x68k_spc_pio_synchronize10(&bus, 7, 0, 0, &result) ==
	    X68K_SPC_OK);
	CHECK(fake.cdb_bytes == 10 &&
	    fake.cdb[0] == X68K_SCSI_SYNCHRONIZE_10 &&
	    result.transferred == 0);
	CHECK(fake.reset_ack_commands == 3);
	CHECK(!fake.irq_enabled && !fake.non_program_transfer &&
	    !fake.protocol_error);
	memset(&fake, 0, sizeof(fake));
	bus.cookie = &fake;
	bus.poll_limit = 8U;
	CHECK(x68k_spc_pio_init(&bus, 7) == X68K_SPC_OK);
	fake.selection_timeout = 1;
	CHECK(x68k_spc_pio_read10(&bus, 7, 0, 0, 0, 1, block,
	    &result) == X68K_SPC_ERR_SELECTION);
	memset(&fake, 0, sizeof(fake));
	bus.cookie = &fake;
	CHECK(x68k_spc_pio_init(&bus, 7) == X68K_SPC_OK);
	fake.stall_data_in = 1;
	CHECK(x68k_spc_pio_read10(&bus, 7, 0, 0, 0, 1, block,
	    &result) == X68K_SPC_ERR_TIMEOUT);
	puts("X68k SCSI host tests passed");
	return 0;
}
