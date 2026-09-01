/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private first-boot coordinator
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_BOOT_H
#define ZEDBSD_DRIVERS_INTEL_AX211_BOOT_H

#include "intel-ax211-command.h"
#include "intel-ax211-dma.h"
#include "intel-ax211-firmware.h"
#include "intel-ax211-init.h"
#include "intel-ax211-mmio.h"
#include "intel-ax211-protocol.h"
#include "intel-ax211-transport.h"

#include <stddef.h>
#include <stdint.h>

#define INTEL_AX211_BOOT_EVENT_CAPACITY                 4096U
#define INTEL_AX211_BOOT_EVENT_LIMIT                      64U
#define INTEL_AX211_BOOT_ALIVE_TIMEOUT_US            1000000U
#define INTEL_AX211_BOOT_COMMAND_TIMEOUT_US          1000000U
#define INTEL_AX211_BOOT_PNVM_TIMEOUT_US             2000000U
#define INTEL_AX211_BOOT_INIT_TIMEOUT_US             2000000U

enum intel_ax211_boot_result {
	INTEL_AX211_BOOT_OK = 0,
	INTEL_AX211_BOOT_INVALID = 1,
	INTEL_AX211_BOOT_FIRMWARE = 2,
	INTEL_AX211_BOOT_DMA = 3,
	INTEL_AX211_BOOT_MMIO = 4,
	INTEL_AX211_BOOT_TRANSPORT = 5,
	INTEL_AX211_BOOT_PROTOCOL = 6,
	INTEL_AX211_BOOT_COMMAND = 7,
	INTEL_AX211_BOOT_TIMEOUT = 8,
	INTEL_AX211_BOOT_DUPLICATE = 9,
	INTEL_AX211_BOOT_IO = 10,
	INTEL_AX211_BOOT_STOP_REQUIRED = 11
};

enum intel_ax211_boot_receive_result {
	INTEL_AX211_BOOT_RECEIVE_OK = 0,
	INTEL_AX211_BOOT_RECEIVE_TIMEOUT = 1,
	INTEL_AX211_BOOT_RECEIVE_IO = 2
};

enum intel_ax211_boot_state {
	INTEL_AX211_BOOT_STATE_IDLE = 0,
	INTEL_AX211_BOOT_STATE_RUNNING = 1,
	INTEL_AX211_BOOT_STATE_STOP_REQUIRED = 2,
	INTEL_AX211_BOOT_STATE_COMPLETE = 3,
	INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA = 4
};

struct intel_ax211_boot_received_event {
	size_t length;
	uint32_t generation;
	uint8_t notification_version;
};

/*
 * These operations are the controller-owned seams which existing private
 * subsystems cannot provide on their own.  receive_epoch_begin drains every
 * queued event and atomically stamps all later events with generation; failure
 * leaves delivery disabled.  transport_bind binds that same generation and
 * is transactional: failure leaves no IRQ or transport ownership.
 * receive_event must return no later than deadline_us.  publish_pnvm
 * synchronizes the PNVM table and sections before ringing the PNVM doorbell.
 * interrupt_drain masks, disestablishes, and drains the sole MSI-X handler
 * before returning success.
 */
struct intel_ax211_boot_ops {
	int (*receive_epoch_begin)(void *argument, uint32_t generation);
	int (*transport_bind)(void *argument,
		struct intel_ax211_dma_resources *dma,
		struct intel_ax211_mmio *mmio,
		struct intel_ax211_transport *transport,
		uint32_t generation);
	int (*receive_event)(void *argument, uint64_t deadline_us,
		uint8_t *bytes, size_t capacity,
		struct intel_ax211_boot_received_event *event);
	int (*publish_pnvm)(void *argument,
		struct intel_ax211_dma_resources *dma);
	int (*post_alive)(void *argument,
		const struct intel_ax211_protocol_alive *alive);
	int (*interrupt_drain)(void *argument);
	int (*clock_us)(void *argument, uint64_t *time_us);
};

/*
 * The PCI controller owns one instance for its entire lifetime.  generation
 * begins as the caller's last retired epoch seed and advances for every run,
 * skipping zero after wrap.  The command table and NVM result remain valid
 * after a successful first pass; DMA and firmware-file storage do not.  If
 * state is STOP_REQUIRED, DMA deliberately remains owned until cleanup
 * confirms IRQ drain and reset.  STOP_REQUIRED_NO_DMA means never-exposed DMA
 * was safely released but a dirty controller still requires checked reset.
 */
struct intel_ax211_boot {
	const struct intel_ax211_boot_ops *ops;
	void *argument;
	struct drv_dma_device *dma_device;
	struct intel_ax211_mmio *mmio;
	struct intel_ax211_transport *transport;
	uint16_t hardware_revision;
	uint16_t rf_type;
	uint32_t generation;
	struct intel_ax211_firmware_files files;
	struct intel_ax211_dma_resources dma;
	struct intel_ax211_command_transaction commands;
	struct intel_ax211_protocol_alive alive;
	struct intel_ax211_protocol_nvm nvm;
	uint8_t command_version_bytes[
		INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t event_bytes[INTEL_AX211_BOOT_EVENT_CAPACITY];
	uint8_t files_loaded;
	uint8_t dma_prepared;
	uint8_t dma_exposed;
	uint8_t hardware_touched;
	uint8_t transport_bound;
	uint8_t alive_accepted;
	uint8_t pnvm_accepted;
	uint8_t init_accepted;
	uint8_t command_table_valid;
	uint8_t nvm_valid;
	uint8_t state;
	uint8_t last_error;
};

int intel_ax211_boot_init(struct intel_ax211_boot *boot,
	const struct intel_ax211_boot_ops *ops, void *argument,
	struct drv_dma_device *dma_device, struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport, uint16_t hardware_revision,
	uint16_t rf_type, uint32_t generation_seed);
int intel_ax211_boot_run(struct intel_ax211_boot *boot,
	struct intel_ax211_protocol_nvm *nvm);
int intel_ax211_boot_cleanup(struct intel_ax211_boot *boot);
int intel_ax211_boot_command_table(const struct intel_ax211_boot *boot,
	struct intel_ax211_protocol_command_table *table);

#endif
