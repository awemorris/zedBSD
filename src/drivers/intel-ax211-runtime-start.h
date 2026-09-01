/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private operational firmware coordinator
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_RUNTIME_START_H
#define ZEDBSD_DRIVERS_INTEL_AX211_RUNTIME_START_H

#include "intel-ax211-boot.h"
#include "intel-ax211-runtime.h"

#include <stddef.h>
#include <stdint.h>

#define INTEL_AX211_RUNTIME_START_EVENT_CAPACITY         4096U
#define INTEL_AX211_RUNTIME_START_EVENT_LIMIT              64U
#define INTEL_AX211_RUNTIME_START_ALIVE_TIMEOUT_US      1000000U
#define INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US    1000000U
#define INTEL_AX211_RUNTIME_START_PNVM_TIMEOUT_US       2000000U
#define INTEL_AX211_RUNTIME_START_INIT_TIMEOUT_US       2000000U
#define INTEL_AX211_RUNTIME_START_MCC_RESPONSE_MIN           20U
#define INTEL_AX211_RUNTIME_START_MCC_RESPONSE_MAX          460U
#define INTEL_AX211_RUNTIME_START_GENERIC_RESPONSE_SIZE       4U

enum intel_ax211_runtime_start_result {
	INTEL_AX211_RUNTIME_START_OK = 0,
	INTEL_AX211_RUNTIME_START_INVALID = 1,
	INTEL_AX211_RUNTIME_START_FIRMWARE = 2,
	INTEL_AX211_RUNTIME_START_DMA = 3,
	INTEL_AX211_RUNTIME_START_MMIO = 4,
	INTEL_AX211_RUNTIME_START_TRANSPORT = 5,
	INTEL_AX211_RUNTIME_START_PROTOCOL = 6,
	INTEL_AX211_RUNTIME_START_COMMAND = 7,
	INTEL_AX211_RUNTIME_START_TIMEOUT = 8,
	INTEL_AX211_RUNTIME_START_DUPLICATE = 9,
	INTEL_AX211_RUNTIME_START_IO = 10,
	INTEL_AX211_RUNTIME_START_STOP_REQUIRED = 11,
	INTEL_AX211_RUNTIME_START_RUNTIME = 12
};

enum intel_ax211_runtime_start_state {
	INTEL_AX211_RUNTIME_START_STATE_IDLE = 0,
	INTEL_AX211_RUNTIME_START_STATE_STARTING = 1,
	INTEL_AX211_RUNTIME_START_STATE_RUNNING = 2,
	INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED = 3,
	INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA = 4
};

/*
 * The operational pass additionally serializes all runtime commands under
 * firmware NIC ownership.  boot supplies the checked hardware epoch and
 * transport seams; nic_lock/unlock must be finite and must not recurse.
 */
struct intel_ax211_runtime_start_ops {
	struct intel_ax211_boot_ops boot;
	int (*nic_lock)(void *argument);
	int (*nic_unlock)(void *argument);
};

/*
 * One PCI controller owns this session for its lifetime.  command_table and
 * NVM are copied at init.  A successful run intentionally retains DMA, IRQ,
 * transport, command, and firmware runtime ownership until stop succeeds.
 * STOP_REQUIRED retains DMA; STOP_REQUIRED_NO_DMA has released never-exposed
 * DMA but blocks every new run until a checked controller reset succeeds.
 */
struct intel_ax211_runtime_start {
	const struct intel_ax211_runtime_start_ops *ops;
	void *argument;
	struct drv_dma_device *dma_device;
	struct intel_ax211_mmio *mmio;
	struct intel_ax211_transport *transport;
	uint16_t hardware_revision;
	uint16_t rf_type;
	uint32_t generation;
	struct intel_ax211_protocol_nvm nvm;
	struct intel_ax211_firmware_files files;
	struct intel_ax211_dma_resources dma;
	struct intel_ax211_command_transaction commands;
	struct intel_ax211_protocol_alive alive;
	struct intel_ax211_runtime_profile profile;
	struct intel_ax211_runtime_state runtime;
	struct intel_ax211_runtime_mcc mcc;
	uint8_t command_version_bytes[
		INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t event_bytes[INTEL_AX211_RUNTIME_START_EVENT_CAPACITY];
	uint8_t response_bytes[INTEL_AX211_RUNTIME_START_MCC_RESPONSE_MAX];
	uint8_t files_loaded;
	uint8_t dma_prepared;
	uint8_t dma_exposed;
	uint8_t hardware_touched;
	uint8_t transport_bound;
	uint8_t commands_initialized;
	uint8_t alive_accepted;
	uint8_t pnvm_accepted;
	uint8_t init_accepted;
	uint8_t profile_valid;
	uint8_t mcc_valid;
	uint8_t ltr_enabled;
	uint8_t nic_locked;
	uint8_t state;
	uint8_t last_error;
};

int intel_ax211_runtime_start_init(
	struct intel_ax211_runtime_start *session,
	const struct intel_ax211_runtime_start_ops *ops, void *argument,
	struct drv_dma_device *dma_device, struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport, uint16_t hardware_revision,
	uint16_t rf_type,
	const struct intel_ax211_protocol_command_table *command_table,
	const struct intel_ax211_protocol_nvm *nvm, int ltr_enabled,
	uint32_t generation_seed);
int intel_ax211_runtime_start_run(
	struct intel_ax211_runtime_start *session);
int intel_ax211_runtime_start_stop(
	struct intel_ax211_runtime_start *session);
int intel_ax211_runtime_start_cleanup(
	struct intel_ax211_runtime_start *session);
int intel_ax211_runtime_start_mcc(
	const struct intel_ax211_runtime_start *session,
	struct intel_ax211_runtime_mcc *mcc);

#endif
