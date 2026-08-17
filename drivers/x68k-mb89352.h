/* X68000 MB89352/SPC and SCSI-2 command contract. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_DRIVERS_X68K_MB89352_H
#define ZEDBSD_DRIVERS_X68K_MB89352_H

#include <stddef.h>
#include <stdint.h>

enum x68k_spc_register {
	X68K_SPC_BDID = 0, X68K_SPC_SCTL, X68K_SPC_SCMD, X68K_SPC_TMOD,
	X68K_SPC_INTS, X68K_SPC_PSNS, X68K_SPC_SSTS, X68K_SPC_SERR,
	X68K_SPC_PCTL, X68K_SPC_MBC, X68K_SPC_DREG, X68K_SPC_TEMP,
	X68K_SPC_TCH, X68K_SPC_TCM, X68K_SPC_TCL, X68K_SPC_EXBF
};

#define X68K_SPC_SCTL_DISABLE       0x80U
#define X68K_SPC_SCTL_CONTROLLER_RESET 0x40U
#define X68K_SPC_SCTL_ABORT_ENABLE  0x10U
#define X68K_SPC_SCTL_PARITY_ENABLE 0x08U
#define X68K_SPC_SCTL_SELECT_ENABLE 0x04U
#define X68K_SPC_SCTL_RESELECT_ENABLE 0x02U
#define X68K_SPC_SCTL_INTERRUPT_ENABLE 0x01U
#define X68K_SPC_SCMD_SELECT        0x20U
#define X68K_SPC_SCMD_TRANSFER      0x80U
#define X68K_SPC_SCMD_PROGRAM_TRANSFER 0x04U
#define X68K_SPC_SCMD_RESET_ACK     0xc0U
#define X68K_SPC_SCMD_SET_ACK       0xe0U
#define X68K_SPC_INTS_SELECTED      0x80U
#define X68K_SPC_INTS_RESELECTED    0x40U
#define X68K_SPC_INTS_DISCONNECT    0x20U
#define X68K_SPC_INTS_COMMAND_DONE  0x10U
#define X68K_SPC_INTS_SERVICE       0x08U
#define X68K_SPC_INTS_TIMEOUT       0x04U
#define X68K_SPC_INTS_HARD_ERROR    0x02U
#define X68K_SPC_PSNS_REQUEST       0x80U
#define X68K_SPC_PSNS_BUSY          0x08U
#define X68K_SPC_SSTS_BUSY          0x20U
#define X68K_SPC_SSTS_TRANSFER      0x10U
#define X68K_SPC_SSTS_COUNT_ZERO    0x04U
#define X68K_SPC_SSTS_DREG_FULL     0x02U
#define X68K_SPC_SSTS_DREG_EMPTY    0x01U
#define X68K_SPC_PCTL_BUS_FREE_INT  0x80U
#define X68K_SPC_PHASE_MASK         0x07U
#define X68K_SPC_PHASE_DATA_OUT     0x00U
#define X68K_SPC_PHASE_DATA_IN      0x01U
#define X68K_SPC_PHASE_COMMAND      0x02U
#define X68K_SPC_PHASE_STATUS       0x03U
#define X68K_SPC_PHASE_MESSAGE_OUT  0x06U
#define X68K_SPC_PHASE_MESSAGE_IN   0x07U

#define X68K_SCSI_TEST_UNIT_READY   0x00U
#define X68K_SCSI_REQUEST_SENSE     0x03U
#define X68K_SCSI_INQUIRY           0x12U
#define X68K_SCSI_READ_CAPACITY_10  0x25U
#define X68K_SCSI_READ_10           0x28U
#define X68K_SCSI_WRITE_10          0x2aU
#define X68K_SCSI_SYNCHRONIZE_10    0x35U

#define X68K_SCSI_BLOCK_SIZE        512U
#define X68K_SCSI_CDB10_MAX_BLOCKS  65535U
#define X68K_SCSI_MESSAGE_COMMAND_COMPLETE 0x00U
#define X68K_SCSI_STATUS_GOOD       0x00U

enum x68k_spc_direction {
	X68K_SPC_DATA_NONE = 0,
	X68K_SPC_DATA_IN,
	X68K_SPC_DATA_OUT
};

enum x68k_spc_error {
	X68K_SPC_OK = 0,
	X68K_SPC_ERR_ARGUMENT = -1,
	X68K_SPC_ERR_TIMEOUT = -2,
	X68K_SPC_ERR_SELECTION = -3,
	X68K_SPC_ERR_CONTROLLER = -4,
	X68K_SPC_ERR_PHASE = -5,
	X68K_SPC_ERR_DISCONNECT = -6,
	X68K_SPC_ERR_RESIDUAL = -7,
	X68K_SPC_ERR_STATUS = -8,
	X68K_SPC_ERR_MESSAGE = -9
};

struct x68k_spc_bus {
	void *cookie;
	uint8_t (*read)(void *cookie, unsigned reg);
	void (*write)(void *cookie, unsigned reg, uint8_t value);
	void (*relax)(void *cookie);
	uint32_t poll_limit;
};

struct x68k_spc_result {
	uint8_t status;
	uint8_t message;
	size_t transferred;
};

struct x68k_scsi_sense {
	uint8_t key;
	uint8_t asc;
	uint8_t ascq;
};

int x68k_scsi_cdb10(uint8_t cdb[10], uint8_t opcode, uint32_t lba,
	uint32_t blocks);
void x68k_scsi_inquiry_cdb(uint8_t cdb[6], uint8_t allocation_length);
void x68k_scsi_request_sense_cdb(uint8_t cdb[6],
	uint8_t allocation_length);
void x68k_scsi_simple_cdb10(uint8_t cdb[10], uint8_t opcode);
int x68k_scsi_parse_capacity10(const uint8_t response[8],
	uint64_t *blocks, uint32_t *block_size);
int x68k_scsi_parse_sense(const uint8_t *response, size_t length,
	struct x68k_scsi_sense *sense);
uint32_t x68k_scsi_transfer_chunk(uint64_t lba, uint32_t blocks,
	uint64_t capacity, uint32_t driver_limit);
int x68k_spc_pio_init(const struct x68k_spc_bus *bus,
	unsigned initiator_id);
int x68k_spc_pio_command(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	const uint8_t *cdb, size_t cdb_length, void *data, size_t data_length,
	enum x68k_spc_direction direction, struct x68k_spc_result *result);
int x68k_spc_pio_read10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun, uint32_t lba,
	uint32_t blocks, void *buffer, struct x68k_spc_result *result);
int x68k_spc_pio_write10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun, uint32_t lba,
	uint32_t blocks, const void *buffer, struct x68k_spc_result *result);
int x68k_spc_pio_request_sense(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	uint8_t response[18], struct x68k_spc_result *result);
int x68k_spc_pio_inquiry(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	uint8_t response[36], struct x68k_spc_result *result);
int x68k_spc_pio_read_capacity10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	uint8_t response[8], struct x68k_spc_result *result);
int x68k_spc_pio_test_unit_ready(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	struct x68k_spc_result *result);
int x68k_spc_pio_synchronize10(const struct x68k_spc_bus *bus,
	unsigned initiator_id, unsigned target_id, unsigned lun,
	struct x68k_spc_result *result);

#endif
