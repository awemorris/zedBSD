/* X68k polled-PIO SCSI block-adapter integration test. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <kern/disk.h>
#include "drivers/x68k-spc-disk.h"

#define CHECK(expression) do { if (!(expression)) { \
	fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #expression); return 1; \
} } while (0)

enum fake_phase {
	FAKE_FREE, FAKE_MSGOUT, FAKE_COMMAND, FAKE_DATAIN, FAKE_DATAOUT,
	FAKE_STATUS, FAKE_MSGIN
};

struct fake_spc {
	enum fake_phase phase;
	uint8_t regs[16];
	uint8_t cdb[12];
	uint8_t response[1024];
	unsigned target;
	unsigned cdb_count;
	unsigned cdb_length;
	unsigned response_length;
	unsigned data_count;
	unsigned select_done;
	unsigned selection_timeout;
	unsigned absent_selections;
	unsigned irq_enabled;
	unsigned non_program_transfer;
	unsigned reset_ack_commands;
	unsigned read_commands;
	unsigned write_commands;
	unsigned flush_commands;
	unsigned unit_attention_once;
	unsigned unit_attention_sent;
	uint8_t status;
	uint8_t write_first;
	uint8_t write_last;
};

void
hal_printf(const char *format, ...)
{
	(void)format;
}

static void
prepare_response(struct fake_spc *fake)
{
	uint8_t opcode = fake->cdb[0];
	fake->data_count = 0;
	fake->response_length = 0;
	fake->status = X68K_SCSI_STATUS_GOOD;
	memset(fake->response, 0, sizeof(fake->response));
	if (opcode == X68K_SCSI_INQUIRY) {
		static const char identity[] = "ZEDBSD  PIO SCSI DISK   0001";
		fake->response_length = 36U;
		fake->response[2] = 2U;
		fake->response[4] = 31U;
		memcpy(fake->response + 8U, identity, sizeof(identity) - 1U);
		fake->phase = FAKE_DATAIN;
	} else if (opcode == X68K_SCSI_READ_CAPACITY_10) {
		fake->response_length = 8U;
		fake->response[2] = 3U;
		fake->response[3] = 0xffU; /* last LBA 1023 */
		fake->response[6] = 2U;    /* 512-byte blocks */
		fake->phase = FAKE_DATAIN;
	} else if (opcode == X68K_SCSI_READ_10) {
		unsigned blocks = (unsigned)fake->cdb[7] << 8 | fake->cdb[8];
		fake->read_commands++;
		if (fake->unit_attention_once && !fake->unit_attention_sent) {
			fake->unit_attention_sent = 1;
			fake->status = 0x02U;
			fake->phase = FAKE_STATUS;
			return;
		}
		fake->response_length = blocks * X68K_SCSI_BLOCK_SIZE;
		for (unsigned index = 0; index < fake->response_length; index++)
			fake->response[index] = (uint8_t)(index ^ 0xa5U);
		fake->phase = FAKE_DATAIN;
	} else if (opcode == X68K_SCSI_REQUEST_SENSE) {
		fake->response_length = 18U;
		fake->response[0] = 0x70U;
		fake->response[2] = 0x06U;
		fake->response[7] = 10U;
		fake->response[12] = 0x29U;
		fake->phase = FAKE_DATAIN;
	} else if (opcode == X68K_SCSI_WRITE_10) {
		fake->response_length = ((unsigned)fake->cdb[7] << 8 |
		    fake->cdb[8]) * X68K_SCSI_BLOCK_SIZE;
		fake->write_commands++;
		fake->phase = FAKE_DATAOUT;
	} else {
		if (opcode == X68K_SCSI_SYNCHRONIZE_10)
			fake->flush_commands++;
		fake->phase = FAKE_STATUS;
	}
}

static uint8_t
fake_read(void *cookie, unsigned reg)
{
	struct fake_spc *fake = cookie;
	if (reg == X68K_SPC_INTS) {
		if (fake->selection_timeout)
			return X68K_SPC_INTS_TIMEOUT;
		return fake->select_done ? X68K_SPC_INTS_COMMAND_DONE : 0;
	}
	if (reg == X68K_SPC_PSNS)
		return fake->phase == FAKE_FREE ? 0 :
		    (uint8_t)(X68K_SPC_PSNS_REQUEST | X68K_SPC_PSNS_BUSY |
		    (fake->phase == FAKE_MSGOUT ? X68K_SPC_PHASE_MESSAGE_OUT :
		     fake->phase == FAKE_COMMAND ? X68K_SPC_PHASE_COMMAND :
		     fake->phase == FAKE_DATAIN ? X68K_SPC_PHASE_DATA_IN :
		     fake->phase == FAKE_DATAOUT ? X68K_SPC_PHASE_DATA_OUT :
		     fake->phase == FAKE_STATUS ? X68K_SPC_PHASE_STATUS :
		     X68K_SPC_PHASE_MESSAGE_IN));
	if (reg == X68K_SPC_SERR || reg == X68K_SPC_SSTS)
		return 0;
	if (reg == X68K_SPC_DREG) {
		if (fake->phase == FAKE_DATAIN) {
			uint8_t value = fake->response[fake->data_count++];
			if (fake->data_count == fake->response_length)
				fake->phase = FAKE_STATUS;
			return value;
		}
		if (fake->phase == FAKE_STATUS) {
			fake->phase = FAKE_MSGIN;
			return fake->status;
		}
		if (fake->phase == FAKE_MSGIN) {
			return X68K_SCSI_MESSAGE_COMMAND_COMPLETE;
		}
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
	if (reg == X68K_SPC_INTS) {
		fake->select_done = 0;
		return;
	}
	if (reg == X68K_SPC_SCMD && value == X68K_SPC_SCMD_SELECT) {
		uint8_t mask = fake->regs[X68K_SPC_TEMP] & 0x7fU;
		fake->target = 0;
		while (fake->target < 7U && (mask & (1U << fake->target)) == 0)
			fake->target++;
		fake->selection_timeout = fake->target != 0;
		if (fake->selection_timeout)
			fake->absent_selections++;
		else {
			fake->phase = FAKE_MSGOUT;
			fake->select_done = 1;
			fake->cdb_count = 0;
			fake->cdb_length = 0;
		}
		return;
	}
	if (reg == X68K_SPC_SCMD && value == X68K_SPC_SCMD_RESET_ACK) {
		fake->reset_ack_commands++;
		fake->phase = FAKE_FREE;
		return;
	}
	if (reg == X68K_SPC_SCMD &&
	    (value & X68K_SPC_SCMD_TRANSFER) != 0) {
		if ((value & X68K_SPC_SCMD_PROGRAM_TRANSFER) == 0)
			fake->non_program_transfer = 1;
		return;
	}
	if (reg != X68K_SPC_DREG)
		return;
	if (fake->phase == FAKE_MSGOUT) {
		fake->phase = FAKE_COMMAND;
	} else if (fake->phase == FAKE_COMMAND) {
		if (fake->cdb_count == 0)
			fake->cdb_length = (value >> 5) == 0 ? 6U : 10U;
		fake->cdb[fake->cdb_count++] = value;
		if (fake->cdb_count == fake->cdb_length)
			prepare_response(fake);
	} else if (fake->phase == FAKE_DATAOUT) {
		if (fake->data_count == 0)
			fake->write_first = value;
		fake->write_last = value;
		fake->data_count++;
		if (fake->data_count == fake->response_length)
			fake->phase = FAKE_STATUS;
	}
}

static void fake_relax(void *cookie) { (void)cookie; }

int
main(void)
{
	struct fake_spc fake;
	struct x68k_spc_bus bus;
	struct disk *disk;
	uint8_t data[2U * X68K_SCSI_BLOCK_SIZE];
	memset(&fake, 0, sizeof(fake));
	memset(&bus, 0, sizeof(bus));
	bus.cookie = &fake;
	bus.read = fake_read;
	bus.write = fake_write;
	bus.relax = fake_relax;
	bus.poll_limit = 100U;
	disk_registry_reset();
	CHECK(x68k_spc_disk_init(&bus, 7, 0) == 1U);
	disk = x68k_spc_disk_target(0);
	CHECK(disk != NULL && disk->d_block_size == 512U &&
	    disk->d_block_count == 1024U && disk->d_max_transfer_blocks == 127U);
	CHECK(x68k_spc_disk_target(1) == NULL && fake.absent_selections == 6U);
	fake.unit_attention_once = 1;
	CHECK(disk_read(disk, 20, 2, data) == 0);
	CHECK(fake.read_commands == 2U && fake.unit_attention_sent &&
	    data[0] == 0xa5U &&
	    data[1] == 0xa4U && data[1023] == (uint8_t)(1023U ^ 0xa5U));
	for (unsigned index = 0; index < sizeof(data); index++)
		data[index] = (uint8_t)(index ^ 0x3cU);
	CHECK(disk_write(disk, 30, 2, data) == 0);
	CHECK(fake.write_commands == 1U && fake.write_first == 0x3cU &&
	    fake.write_last == (uint8_t)(1023U ^ 0x3cU));
	CHECK(bio_flush(disk) == 0 && fake.flush_commands == 1U);
	CHECK(!fake.irq_enabled && !fake.non_program_transfer &&
	    fake.reset_ack_commands != 0);
	puts("X68k SCSI disk adapter tests passed");
	return 0;
}
