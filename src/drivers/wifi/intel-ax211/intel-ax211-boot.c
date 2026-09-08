/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private first-boot coordinator
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "intel-ax211-boot.h"

#include <string.h>

enum ax211_boot_notification {
	AX211_BOOT_NOTIFICATION_ALIVE = 1,
	AX211_BOOT_NOTIFICATION_PNVM = 2,
	AX211_BOOT_NOTIFICATION_INIT = 3
};

static int ax211_boot_ops_valid(const struct intel_ax211_boot_ops *ops);
static void ax211_boot_run_state_clear(struct intel_ax211_boot *boot);
static void ax211_boot_generation_advance(struct intel_ax211_boot *boot);
static int ax211_boot_load_and_pin(struct intel_ax211_boot *boot);
static int ax211_boot_protocol_result(int result);
static int ax211_boot_finish(struct intel_ax211_boot *boot, int result, struct intel_ax211_protocol_nvm *nvm);
static void ax211_boot_release_files(struct intel_ax211_boot *boot);
static int ax211_boot_stop_and_release(struct intel_ax211_boot *boot);
static int ax211_boot_prepare_dma(struct intel_ax211_boot *boot);
static int ax211_boot_start_device(struct intel_ax211_boot *boot);
static int ax211_boot_wait_notification(struct intel_ax211_boot *boot, enum ax211_boot_notification expected, uint64_t timeout);
static int ax211_boot_deadline(struct intel_ax211_boot *boot, uint64_t timeout, uint64_t *now, uint64_t *deadline);
static int ax211_boot_receive(struct intel_ax211_boot *boot, uint64_t deadline, struct intel_ax211_event *event, struct intel_ax211_protocol_message *message);
static int ax211_boot_notification_kind( const struct intel_ax211_protocol_message *message);
static int ax211_boot_notification_duplicate(const struct intel_ax211_boot *boot, int kind);
static int ax211_boot_notification_accept( struct intel_ax211_boot *boot, int kind, const struct intel_ax211_protocol_message *message);
static int ax211_boot_select_and_publish_pnvm(struct intel_ax211_boot *boot);
static int ax211_boot_run_nvm_commands(struct intel_ax211_boot *boot);
static int ax211_boot_send_extended_cfg(struct intel_ax211_boot *boot);
static int ax211_boot_command_result(int result);
static int ax211_boot_wait_command(struct intel_ax211_boot *boot, uint64_t deadline, uint8_t *response, size_t response_capacity, size_t *response_length);
static uint32_t ax211_boot_get_le32(const uint8_t bytes[4]);
static int ax211_boot_send_nvm_access_complete(struct intel_ax211_boot *boot);
static int ax211_boot_send_nvm_get_info(struct intel_ax211_boot *boot);

/*
 * Initializes one controller-owned first-boot coordinator.
 */
int
drv_intel_ax211_boot_init(
	struct intel_ax211_boot *boot,
	const struct intel_ax211_boot_ops *ops,
	void *argument,
	struct drv_dma_device *dma_device,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport,
	uint16_t hardware_revision,
	uint16_t rf_type,
	uint32_t generation_seed)
{
	uint16_t mac_type;

	/*
 * Rejects an incomplete or internally inconsistent controller binding.
	 */
	if (boot == NULL || !ax211_boot_ops_valid(ops) || dma_device == NULL ||
	    mmio == NULL || transport == NULL || generation_seed == 0U)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_INVALID;
	mac_type = (uint16_t)((hardware_revision & 0xfff0U) >> 4);

	/* Checks the drv intel ax211 mac type supported result. */
	if (!drv_intel_ax211_mac_type_supported(mac_type))
		return INTEL_AX211_BOOT_INVALID;

	/* Handles the mmio condition. */
	if (mmio->profile.mac_type != mac_type ||
	    mmio->profile.rf_type != rf_type)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_INVALID;

	/* Publishes a fully initialized, otherwise empty coordinator. */
	memset(boot, 0, sizeof(*boot));
	boot->ops = ops;
	boot->argument = argument;
	boot->dma_device = dma_device;
	boot->mmio = mmio;
	boot->transport = transport;
	boot->hardware_revision = hardware_revision;
	boot->rf_type = rf_type;
	boot->generation = generation_seed;
	boot->state = INTEL_AX211_BOOT_STATE_IDLE;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/*
 * Runs one bounded read-NVM firmware pass and then stops the device.
 */
int
drv_intel_ax211_boot_run(
	struct intel_ax211_boot *boot,
	struct intel_ax211_protocol_nvm *nvm)
{
	int function_result;
	int result;

	/*
 * Admits only a clean coordinator and keeps the output transactional.
	 */
	if (boot == NULL || nvm == NULL ||
	    (boot->state != INTEL_AX211_BOOT_STATE_IDLE &&
	     boot->state != INTEL_AX211_BOOT_STATE_COMPLETE) ||
	    boot->dma.device != NULL || boot->files_loaded)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_INVALID;
	ax211_boot_run_state_clear(boot);
	boot->state = INTEL_AX211_BOOT_STATE_RUNNING;
	ax211_boot_generation_advance(boot);

	/* Loads exact files and pins the borrowed command-version table. */
	result = ax211_boot_load_and_pin(boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK) {
		/* Obtains the ax211 boot finish result. */
		function_result = ax211_boot_finish(boot, result, nvm);

		/* Returns the computed result. */
		return function_result;
	}

	/*
 * Copies all first-pass DMA objects before hardware can observe them.
	 */
	result = ax211_boot_prepare_dma(boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK) {
		/* Obtains the ax211 boot finish result. */
		function_result = ax211_boot_finish(boot, result, nvm);

		/* Returns the computed result. */
		return function_result;
	}
	result = boot->ops->receive_epoch_begin(boot->argument,
						boot->generation);

	/* Checks the operation result. */
	if (result != 0) {
		/* Obtains the ax211 boot finish result. */
		function_result =
			ax211_boot_finish(boot, INTEL_AX211_BOOT_IO, nvm);

		/* Returns the computed result. */
		return function_result;
	}

	/* Starts the exact Gen3 transport and accepts one ALIVE generation. */
	result = ax211_boot_start_device(boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK) {
		/* Obtains the ax211 boot finish result. */
		function_result = ax211_boot_finish(boot, result, nvm);

		/* Returns the computed result. */
		return function_result;
	}
	result = ax211_boot_wait_notification(
		boot, AX211_BOOT_NOTIFICATION_ALIVE,
		INTEL_AX211_BOOT_ALIVE_TIMEOUT_US);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK) {
		/* Obtains the ax211 boot finish result. */
		function_result = ax211_boot_finish(boot, result, nvm);

		/* Returns the computed result. */
		return function_result;
	}

	/*
 * Retires one-shot firmware images only after exact ALIVE acceptance.
	 */
	drv_intel_ax211_dma_release_boot_images(&boot->dma);
	result = ax211_boot_select_and_publish_pnvm(boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK) {
		/* Obtains the ax211 boot finish result. */
		function_result = ax211_boot_finish(boot, result, nvm);

		/* Returns the computed result. */
		return function_result;
	}

	/* Runs the exact API89 read-NVM command sequence. */
	result = ax211_boot_run_nvm_commands(boot);

	/* Obtains the ax211 boot finish result. */
	function_result = ax211_boot_finish(boot, result, nvm);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Retries a previously failed IRQ-drain and reset boundary.
 */
int
drv_intel_ax211_boot_cleanup(
	struct intel_ax211_boot *boot)
{
	int result;

	/* Restricts retry cleanup to retained-DMA failure state. */
	if (boot == NULL ||
	    (boot->state != INTEL_AX211_BOOT_STATE_STOP_REQUIRED &&
	     boot->state != INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA) ||
	    (!boot->dma_prepared && !boot->hardware_touched))

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_INVALID;

	/* Repeats the complete checked stop boundary before releasing DMA. */
	result = ax211_boot_stop_and_release(boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK) {
		/* Handles the boot condition. */
		if (boot->dma_prepared)
			boot->state = INTEL_AX211_BOOT_STATE_STOP_REQUIRED;
		else if (boot->hardware_touched) {
			boot->state =
				INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA;
		} else {
			boot->state = INTEL_AX211_BOOT_STATE_IDLE;
		}

		/* Returns the computed result. */
		return result;
	}
	boot->state = INTEL_AX211_BOOT_STATE_IDLE;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/*
 * Borrows the coordinator-owned exact API89 command-version table.
 */
int
drv_intel_ax211_boot_command_table(
	const struct intel_ax211_boot *boot,
	struct intel_ax211_protocol_command_table *table)
{
	int result;

	/* Exposes the pinned table only after a complete stopped first pass. */
	if (boot == NULL || table == NULL ||
	    boot->state != INTEL_AX211_BOOT_STATE_COMPLETE ||
	    !boot->command_table_valid)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_INVALID;
	result = drv_intel_ax211_protocol_command_table_parse(
		boot->command_version_bytes,
		sizeof(boot->command_version_bytes), table);

	/* Checks the operation result. */
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Verifies every controller-owned operation required by the coordinator. */
static int
ax211_boot_ops_valid(
	const struct intel_ax211_boot_ops *ops)
{
	/* Handles the ops availability. */
	if (ops == NULL)
		return 0;

	/* Handles the receive epoch begin availability. */
	if (ops->receive_epoch_begin == NULL || ops->transport_bind == NULL ||
	    ops->receive_event == NULL)

		/* Reports successful completion. */
		return 0;

	/* Handles the publish pnvm availability. */
	if (ops->publish_pnvm == NULL || ops->post_alive == NULL)
		return 0;

	/* Handles the interrupt drain availability. */
	if (ops->interrupt_drain == NULL || ops->clock_us == NULL)
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Clears transient state without invalidating controller dependencies. */
static void
ax211_boot_run_state_clear(
	struct intel_ax211_boot *boot)
{
	memset(&boot->files, 0, sizeof(boot->files));
	memset(&boot->dma, 0, sizeof(boot->dma));
	memset(&boot->commands, 0, sizeof(boot->commands));
	memset(&boot->alive, 0, sizeof(boot->alive));
	memset(&boot->nvm, 0, sizeof(boot->nvm));
	memset(boot->command_version_bytes, 0,
	       sizeof(boot->command_version_bytes));
	memset(boot->event_bytes, 0, sizeof(boot->event_bytes));
	boot->files_loaded = 0U;
	boot->dma_prepared = 0U;
	boot->dma_exposed = 0U;
	boot->hardware_touched = 0U;
	boot->transport_bound = 0U;
	boot->alive_accepted = 0U;
	boot->pnvm_accepted = 0U;
	boot->init_accepted = 0U;
	boot->command_table_valid = 0U;
	boot->nvm_valid = 0U;
	boot->last_error = INTEL_AX211_BOOT_OK;
}

/* Advances the hardware epoch for every run and reserves zero as invalid. */
static void
ax211_boot_generation_advance(
	struct intel_ax211_boot *boot)
{
	boot->generation++;

	/* Handles the boot condition. */
	if (boot->generation == 0U)
		boot->generation = 1U;
}

/* Loads exact artifacts and copies every borrowed version byte. */
static int
ax211_boot_load_and_pin(
	struct intel_ax211_boot *boot)
{
	int function_result;
	struct intel_ax211_protocol_command_table table;
	size_t offset;
	size_t length;
	int result;

	/* Loads and marks ownership before inspecting any borrowed member. */
	result = drv_intel_ax211_firmware_files_load(&boot->files);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_FIRMWARE;
	boot->files_loaded = 1U;

	/* Requires both exact file buffers and the full API89 version table. */
	if (boot->files.ucode_bytes == NULL || boot->files.ucode_size == 0U ||
	    boot->files.pnvm_bytes == NULL || boot->files.pnvm_size == 0U)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_FIRMWARE;
	offset = boot->files.ucode_manifest.command_versions_offset;
	length = boot->files.ucode_manifest.command_versions_length;

	/* Checks the current data length. */
	if (length != sizeof(boot->command_version_bytes))
		return INTEL_AX211_BOOT_FIRMWARE;

	/* Checks the current offset. */
	if (offset > boot->files.ucode_size ||
	    length > boot->files.ucode_size - offset)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_FIRMWARE;

	/* Pins the table before any path may release firmware-file storage. */
	memcpy(boot->command_version_bytes, boot->files.ucode_bytes + offset,
	       length);
	result = drv_intel_ax211_protocol_command_table_parse(
		boot->command_version_bytes, length, &table);

	/* Checks the operation result. */
	if (result != INTEL_AX211_PROTOCOL_OK) {
		/* Obtains the ax211 boot protocol result result. */
		function_result = ax211_boot_protocol_result(result);

		/* Returns the computed result. */
		return function_result;
	}
	result = drv_intel_ax211_init_api89_validate(&table);

	/* Checks the operation result. */
	if (result != INTEL_AX211_PROTOCOL_OK) {
		/* Obtains the ax211 boot protocol result result. */
		function_result = ax211_boot_protocol_result(result);

		/* Returns the computed result. */
		return function_result;
	}
	boot->command_table_valid = 1U;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Maps all exact protocol rejections to the coordinator boundary. */
static int
ax211_boot_protocol_result(
	int result)
{
	/* Checks the operation result. */
	if (result == INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_BOOT_OK;

	/* Checks the operation result. */
	if (result == INTEL_AX211_PROTOCOL_DUPLICATE)
		return INTEL_AX211_BOOT_DUPLICATE;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_PROTOCOL;
}

/* Preserves the primary error while enforcing the checked stop boundary. */
static int
ax211_boot_finish(
	struct intel_ax211_boot *boot,
	int result,
	struct intel_ax211_protocol_nvm *nvm)
{
	int cleanup_result;

	/*
 * Releases immutable source files independently of device stop outcome.
	 */
	ax211_boot_release_files(boot);
	boot->last_error = (uint8_t)result;
	cleanup_result = ax211_boot_stop_and_release(boot);

	/* Handles the cleanup result condition. */
	if (cleanup_result == INTEL_AX211_BOOT_STOP_REQUIRED)
		return cleanup_result;

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK) {
		boot->state = INTEL_AX211_BOOT_STATE_IDLE;

		/* Returns the computed result. */
		return result;
	}

	/* Handles the cleanup result condition. */
	if (cleanup_result != INTEL_AX211_BOOT_OK) {
		boot->state = INTEL_AX211_BOOT_STATE_IDLE;

		/* Returns the computed result. */
		return cleanup_result;
	}

	/* Handles the boot condition. */
	if (!boot->nvm_valid) {
		boot->state = INTEL_AX211_BOOT_STATE_IDLE;

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_PROTOCOL;
	}

	/*
 * Publishes the copied NVM only after a complete checked device stop.
	 */
	*nvm = boot->nvm;
	boot->state = INTEL_AX211_BOOT_STATE_COMPLETE;
	boot->last_error = INTEL_AX211_BOOT_OK;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Releases source-file ownership exactly once. */
static void
ax211_boot_release_files(
	struct intel_ax211_boot *boot)
{
	/* Handles the boot condition. */
	if (!boot->files_loaded)
		return;
	drv_intel_ax211_firmware_files_release(&boot->files);
	boot->files_loaded = 0U;
}

/* Stops device ownership and releases DMA only across a proven safe boundary. */
static int
ax211_boot_stop_and_release(
	struct intel_ax211_boot *boot)
{
	int after_reset_result;
	int drain_result;
	int quiesce_result;
	int stop_result;

	/* A prior no-DMA failure still requires a checked controller reset. */
	if (!boot->dma_prepared) {
		/* Handles the boot condition. */
		if (!boot->hardware_touched)
			return INTEL_AX211_BOOT_OK;
		stop_result = drv_intel_ax211_mmio_stop(boot->mmio);

		/* Handles the stop result condition. */
		if (stop_result != INTEL_AX211_MMIO_OK) {
			boot->state =
				INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA;

			/* Returns the computed result. */
			return INTEL_AX211_BOOT_STOP_REQUIRED;
		}
		boot->hardware_touched = 0U;

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_OK;
	}

	/*
 * Never-exposed DMA is safe to free, but reset failure remains sticky.
	 */
	if (!boot->dma_exposed && !boot->transport_bound) {
		stop_result = INTEL_AX211_MMIO_OK;

		/* Handles the boot condition. */
		if (boot->hardware_touched)
			stop_result = drv_intel_ax211_mmio_stop(boot->mmio);
		drv_intel_ax211_dma_release(&boot->dma);
		boot->dma_prepared = 0U;

		/* Handles the stop result condition. */
		if (stop_result != INTEL_AX211_MMIO_OK) {
			boot->state =
				INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA;

			/* Returns the computed result. */
			return INTEL_AX211_BOOT_STOP_REQUIRED;
		}
		boot->hardware_touched = 0U;

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_OK;
	}

	/*
 * Stops submissions and RX before draining the sole interrupt handler.
	 */
	quiesce_result = drv_intel_ax211_transport_quiesce(boot->transport);
	drain_result = boot->ops->interrupt_drain(boot->argument);
	stop_result = drv_intel_ax211_mmio_stop(boot->mmio);

	/* Handles the drain result condition. */
	if (drain_result != 0 || stop_result != INTEL_AX211_MMIO_OK) {
		boot->state = INTEL_AX211_BOOT_STATE_STOP_REQUIRED;

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_STOP_REQUIRED;
	}

	/*
 * Retires all command ownership only after bus-master stop completed.
	 */
	after_reset_result =
		drv_intel_ax211_transport_command_after_device_reset(
			boot->transport);
	drv_intel_ax211_dma_release(&boot->dma);
	boot->dma_prepared = 0U;
	boot->dma_exposed = 0U;
	boot->hardware_touched = 0U;
	boot->transport_bound = 0U;

	/* Handles the quiesce result condition. */
	if (quiesce_result != INTEL_AX211_TRANSPORT_OK ||
	    after_reset_result != INTEL_AX211_TRANSPORT_OK)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_TRANSPORT;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Copies firmware and all runtime rings into controller-owned DMA. */
static int
ax211_boot_prepare_dma(
	struct intel_ax211_boot *boot)
{
	int result;

	result = drv_intel_ax211_dma_prepare_boot(
		boot->dma_device, boot->files.ucode_bytes,
		boot->files.ucode_size, &boot->files.ucode_manifest,
		boot->hardware_revision, &boot->dma);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_DMA;
	boot->dma_prepared = 1U;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Starts one exact Gen3 firmware-loading transport. */
static int
ax211_boot_start_device(
	struct intel_ax211_boot *boot)
{
	struct intel_ax211_mmio_boot mmio_boot;
	size_t index;
	int result;

	/* Executes the checked card-ready, reset, and APM sequence. */
	boot->hardware_touched = 1U;
	result = drv_intel_ax211_mmio_prepare_card_hw(boot->mmio);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;
	result = drv_intel_ax211_mmio_sw_reset(boot->mmio);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;
	result = drv_intel_ax211_mmio_apm_init(boot->mmio);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;

	/* Binds the sole controller transport before exposing ring storage. */
	result = boot->ops->transport_bind(boot->argument, &boot->dma,
					   boot->mmio, boot->transport,
					   boot->generation);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_TRANSPORT;
	boot->transport_bound = 1U;
	result = drv_intel_ax211_transport_configure_msix(boot->transport);

	/* Checks the operation result. */
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_BOOT_TRANSPORT;

	/* Treats partial ring publication as device DMA ownership. */
	boot->dma_exposed = 1U;
	result = drv_intel_ax211_transport_initialize_rings(boot->transport);

	/* Checks the operation result. */
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_BOOT_TRANSPORT;

	/* Handles the boot condition. */
	if (boot->dma.rx_buffer_count != INTEL_AX211_RX_RING_SIZE)
		return INTEL_AX211_BOOT_DMA;

	/* Publishes every exact RX buffer before firmware can configure RFH. */
	for (index = 0U; index < boot->dma.rx_buffer_count; index++) {
		/* Handles the address availability. */
		if (boot->dma.rx_buffer[index].address == NULL ||
		    boot->dma.rx_buffer[index].device_address == 0U)

			/* Returns the computed result. */
			return INTEL_AX211_BOOT_DMA;
		result = drv_intel_ax211_transport_publish_rx_descriptor(
			boot->transport, (uint16_t)index,
			boot->dma.rx_buffer[index].device_address);

		/* Checks the operation result. */
		if (result != INTEL_AX211_TRANSPORT_OK)
			return INTEL_AX211_BOOT_TRANSPORT;
	}

	/* The first RX credit is deferred until the hardware-ALIVE cause. */
	result = drv_intel_ax211_transport_enable_firmware_interrupts(
		boot->transport);

	/* Checks the operation result. */
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_BOOT_TRANSPORT;

	/*
 * Publishes context and IML only after rings and interrupts are ready.
	 */
	if (boot->dma.context.device_address == 0U ||
	    boot->dma.iml.device_address == 0U ||
	    boot->dma.iml.size != INTEL_AX211_MMIO_IML_SIZE)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_DMA;
	memset(&mmio_boot, 0, sizeof(mmio_boot));
	mmio_boot.context_address = boot->dma.context.device_address;
	mmio_boot.iml_address = boot->dma.iml.device_address;
	mmio_boot.iml_size = (uint32_t)boot->dma.iml.size;
	result = drv_intel_ax211_mmio_publish_gen3(boot->mmio, &mmio_boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Waits finitely for one exact generation-tagged notification. */
static int
ax211_boot_wait_notification(
	struct intel_ax211_boot *boot,
	enum ax211_boot_notification expected,
	uint64_t timeout)
{
	int function_result;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_event event;
	uint64_t deadline;
	uint64_t now;
	size_t index;
	int kind;
	int result;

	/* Fixes one monotonic absolute deadline for the entire wait. */
	result = ax211_boot_deadline(boot, timeout, &now, &deadline);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/*
 * Discards stale generations but never extends the original deadline.
	 */
	/* Process each remaining element. */
	for (index = 0U; index < INTEL_AX211_BOOT_EVENT_LIMIT; index++) {
		result = ax211_boot_receive(boot, deadline, &event, &message);

		/* Checks the operation result. */
		if (result != INTEL_AX211_BOOT_OK)
			return result;

		/* Handles the message condition. */
		if (message.generation != boot->generation)
			continue;

		/* Handles the event condition. */
		if ((event.queue & 0x80U) == 0U)
			return INTEL_AX211_BOOT_PROTOCOL;
		kind = ax211_boot_notification_kind(&message);

		/* Handles the ax211 boot notification duplicate condition. */
		if (ax211_boot_notification_duplicate(boot, kind))
			return INTEL_AX211_BOOT_DUPLICATE;

		/* Handles the kind condition. */
		if (kind == 0)
			continue;

		/* Handles the kind condition. */
		if (kind != (int)expected)
			return INTEL_AX211_BOOT_PROTOCOL;

		/* Obtains the ax211 boot notification accept result. */
		function_result =
			ax211_boot_notification_accept(boot, kind, &message);

		/* Returns the computed result. */
		return function_result;
	}

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_TIMEOUT;
}

/* Reads one monotonic start time and computes a checked absolute deadline. */
static int
ax211_boot_deadline(
	struct intel_ax211_boot *boot,
	uint64_t timeout,
	uint64_t *now,
	uint64_t *deadline)
{
	int result;

	/* Handles the now availability. */
	if (timeout == 0U || now == NULL || deadline == NULL)
		return INTEL_AX211_BOOT_INVALID;
	result = boot->ops->clock_us(boot->argument, now);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_IO;

	/* Handles the now condition. */
	if (*now > UINT64_MAX - timeout)
		return INTEL_AX211_BOOT_IO;
	*deadline = *now + timeout;
	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Receives and decodes one bounded event without assigning ownership. */
static int
ax211_boot_receive(
	struct intel_ax211_boot *boot,
	uint64_t deadline,
	struct intel_ax211_event *event,
	struct intel_ax211_protocol_message *message)
{
	struct intel_ax211_boot_received_event received;
	uint64_t now;
	int result;

	/* Lets the controller fill only the coordinator-owned fixed buffer. */
	memset(&received, 0, sizeof(received));
	result = boot->ops->receive_event(boot->argument, deadline,
					  boot->event_bytes,
					  sizeof(boot->event_bytes), &received);

	/* Checks the operation result. */
	if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
		return INTEL_AX211_BOOT_TIMEOUT;

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_RECEIVE_OK)
		return INTEL_AX211_BOOT_IO;

	/* Handles the received condition. */
	if (received.length == 0U ||
	    received.length > sizeof(boot->event_bytes) ||
	    received.generation == 0U)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_PROTOCOL;

	/*
 * Rejects a receiver which returned after the caller's fixed deadline.
	 */
	result = boot->ops->clock_us(boot->argument, &now);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_IO;

	/* Handles the now condition. */
	if (now > deadline)
		return INTEL_AX211_BOOT_TIMEOUT;

	/* Decodes the common RX envelope before attaching private metadata. */
	result = drv_intel_ax211_event_decode(boot->event_bytes,
					      received.length, event);

	/* Checks the operation result. */
	if (result != INTEL_AX211_OK)
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Handles the event condition. */
	if (event->payload_offset > received.length ||
	    event->payload_length != received.length - event->payload_offset)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_PROTOCOL;
	memset(message, 0, sizeof(*message));
	message->opcode = event->command.opcode;
	message->group = event->flags &
			 (uint8_t)~INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	message->version = received.notification_version;
	message->flags = event->flags;
	message->queue = event->queue;
	message->index = event->index;
	message->generation = received.generation;
	message->payload = boot->event_bytes + event->payload_offset;
	message->payload_length = event->payload_length;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Classifies only notifications which participate in the first-pass order. */
static int
ax211_boot_notification_kind(
	const struct intel_ax211_protocol_message *message)
{
	/* Handles the message condition. */
	if (message->group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    message->opcode == INTEL_AX211_PROTOCOL_ALIVE_OPCODE)

		/* Returns the computed result. */
		return AX211_BOOT_NOTIFICATION_ALIVE;

	/* Handles the message condition. */
	if (message->group == INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM &&
	    message->opcode == INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE)

		/* Returns the computed result. */
		return AX211_BOOT_NOTIFICATION_PNVM;

	/* Handles the message condition. */
	if (message->group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    message->opcode == INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE)

		/* Returns the computed result. */
		return AX211_BOOT_NOTIFICATION_INIT;

	/* Reports successful completion. */
	return 0;
}

/* Detects a repeated accepted notification in the current generation. */
static int
ax211_boot_notification_duplicate(
	const struct intel_ax211_boot *boot,
	int kind)
{
	/* Handles the kind condition. */
	if (kind == AX211_BOOT_NOTIFICATION_ALIVE && boot->alive_accepted)
		return 1;

	/* Handles the kind condition. */
	if (kind == AX211_BOOT_NOTIFICATION_PNVM && boot->pnvm_accepted)
		return 1;

	/* Handles the kind condition. */
	if (kind == AX211_BOOT_NOTIFICATION_INIT && boot->init_accepted)
		return 1;

	/* Reports successful completion. */
	return 0;
}

/* Validates and records one expected notification transactionally. */
static int
ax211_boot_notification_accept(
	struct intel_ax211_boot *boot,
	int kind,
	const struct intel_ax211_protocol_message *message)
{
	int function_result;
	int result;

	/* Handles the kind condition. */
	if (kind == AX211_BOOT_NOTIFICATION_ALIVE) {
		result = drv_intel_ax211_protocol_alive_decode(
			message, boot->generation, &boot->alive);

		/* Checks the operation result. */
		if (result == INTEL_AX211_PROTOCOL_OK)
			boot->alive_accepted = 1U;
	} else if (kind == AX211_BOOT_NOTIFICATION_PNVM) {
		result = drv_intel_ax211_protocol_pnvm_init_complete(
			message, boot->generation);

		/* Checks the operation result. */
		if (result == INTEL_AX211_PROTOCOL_OK)
			boot->pnvm_accepted = 1U;
	} else if (kind == AX211_BOOT_NOTIFICATION_INIT) {
		result = drv_intel_ax211_init_complete_validate(
			message, boot->generation);

		/* Checks the operation result. */
		if (result == INTEL_AX211_PROTOCOL_OK)
			boot->init_accepted = 1U;
	} else {
		/* Returns the computed result. */
		return INTEL_AX211_BOOT_PROTOCOL;
	}

	/* Obtains the ax211 boot protocol result result. */
	function_result = ax211_boot_protocol_result(result);

	/* Returns the computed result. */
	return function_result;
}

/* Selects the ALIVE SKU and publishes an exact copied PNVM image. */
static int
ax211_boot_select_and_publish_pnvm(
	struct intel_ax211_boot *boot)
{
	struct intel_ax211_pnvm_manifest manifest;
	struct intel_ax211_sku_id sku;
	int result;

	/* Selects only the exact three-word SKU from accepted ALIVE. */
	memset(&sku, 0, sizeof(sku));
	sku.data[0] = boot->alive.sku[0];
	sku.data[1] = boot->alive.sku[1];
	sku.data[2] = boot->alive.sku[2];
	result = drv_intel_ax211_pnvm_parse(
		boot->files.pnvm_bytes, boot->files.pnvm_size, &sku,
		boot->mmio->profile.mac_type, boot->rf_type, &manifest);

	/* Checks the operation result. */
	if (result != INTEL_AX211_OK)
		return INTEL_AX211_BOOT_FIRMWARE;

	/* Copies every selected segment before releasing both source files. */
	result = drv_intel_ax211_dma_prepare_pnvm(boot->files.pnvm_bytes,
						  boot->files.pnvm_size,
						  &manifest, &boot->dma);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_DMA;
	ax211_boot_release_files(boot);

	/* Synchronizes and rings the controller-owned PNVM doorbell. */
	result = boot->ops->publish_pnvm(boot->argument, &boot->dma);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_IO;
	result =
		ax211_boot_wait_notification(boot, AX211_BOOT_NOTIFICATION_PNVM,
					     INTEL_AX211_BOOT_PNVM_TIMEOUT_US);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Applies the transport's exact post-ALIVE hardware transition. */
	result = boot->ops->post_alive(boot->argument, &boot->alive);

	/* Checks the operation result. */
	if (result != 0)
		return INTEL_AX211_BOOT_IO;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Runs the four first-pass command/notification transitions. */
static int
ax211_boot_run_nvm_commands(
	struct intel_ax211_boot *boot)
{
	int result;

	/* Binds one-command-at-a-time transactions to this hardware epoch. */
	result = drv_intel_ax211_command_transaction_init(
		&boot->commands, boot->transport, 1U, boot->generation);

	/* Checks the operation result. */
	if (result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_BOOT_COMMAND;

	/* Marks the firmware session as an exact read-NVM pass. */
	result = ax211_boot_send_extended_cfg(boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;
	result = ax211_boot_send_nvm_access_complete(boot);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;
	result =
		ax211_boot_wait_notification(boot, AX211_BOOT_NOTIFICATION_INIT,
					     INTEL_AX211_BOOT_INIT_TIMEOUT_US);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Retrieves and decodes one exact v4 NVM response. */
	result = ax211_boot_send_nvm_get_info(boot);

	/* Returns the computed result. */
	return result;
}

/* Sends fixed-v1 INIT_EXTENDED_CFG and waits for its empty acknowledgement. */
static int
ax211_boot_send_extended_cfg(
	struct intel_ax211_boot *boot)
{
	int function_result;
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	uint8_t payload[INTEL_AX211_INIT_EXTENDED_CFG_SIZE];
	uint8_t response[4];
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	int result;

	/* Encodes the exact read-NVM profile. */
	result = drv_intel_ax211_init_extended_cfg_encode(
		INTEL_AX211_INIT_PROFILE_READ_NVM, payload);

	/* Checks the operation result. */
	if (result != INTEL_AX211_PROTOCOL_OK) {
		/* Obtains the ax211 boot protocol result result. */
		function_result = ax211_boot_protocol_result(result);

		/* Returns the computed result. */
		return function_result;
	}
	memset(&request, 0, sizeof(request));
	request.command.opcode = INTEL_AX211_INIT_EXTENDED_CFG_OPCODE;
	request.command.group = INTEL_AX211_INIT_SYSTEM_GROUP;

	/* OpenBSD API89 uses zero in the wide-header wire-version byte. */
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = sizeof(response);
	request.maximum_response_length = sizeof(response);

	/* Submits the command with one finite acknowledgement deadline. */
	result = ax211_boot_deadline(boot, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
				     &now, &deadline);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;
	result = drv_intel_ax211_command_submit(
		&boot->commands, &request, now,
		INTEL_AX211_BOOT_COMMAND_TIMEOUT_US, &handle);

	/* Checks the operation result. */
	if (result != INTEL_AX211_COMMAND_OK) {
		/* Obtains the ax211 boot command result result. */
		function_result = ax211_boot_command_result(result);

		/* Returns the computed result. */
		return function_result;
	}
	response_length = 0U;
	memset(response, 0, sizeof(response));
	result = ax211_boot_wait_command(boot, deadline, response,
					 sizeof(response), &response_length);

	/* Checks the operation result. */
	if (result == INTEL_AX211_BOOT_OK &&
	    response_length != sizeof(response))
		result = INTEL_AX211_BOOT_PROTOCOL;

	/* Checks the ax211 boot get le32 result. */
	if (result == INTEL_AX211_BOOT_OK &&
	    ax211_boot_get_le32(response) != 0U)
		result = INTEL_AX211_BOOT_COMMAND;
	memset(response, 0, sizeof(response));

	/* Returns the computed result. */
	return result;
}

/* Maps command transaction outcomes without weakening token validation. */
static int
ax211_boot_command_result(
	int result)
{
	/* Checks the operation result. */
	if (result == INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_BOOT_OK;

	/* Checks the operation result. */
	if (result == INTEL_AX211_COMMAND_TIMEOUT)
		return INTEL_AX211_BOOT_TIMEOUT;

	/* Checks the operation result. */
	if (result == INTEL_AX211_COMMAND_DUPLICATE)
		return INTEL_AX211_BOOT_DUPLICATE;

	/* Checks the operation status. */
	if (result == INTEL_AX211_COMMAND_MALFORMED ||
	    result == INTEL_AX211_COMMAND_FIRMWARE_FAILED ||
	    result == INTEL_AX211_COMMAND_VERSION_MISMATCH ||
	    result == INTEL_AX211_COMMAND_OUT_OF_ORDER ||
	    result == INTEL_AX211_COMMAND_BUFFER_TOO_SMALL)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_COMMAND;
}

/* Waits finitely for the oldest exact command response. */
static int
ax211_boot_wait_command(
	struct intel_ax211_boot *boot,
	uint64_t deadline,
	uint8_t *response,
	size_t response_capacity,
	size_t *response_length)
{
	int function_result;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_handle timed_out;
	struct intel_ax211_event event;
	uint64_t now;
	size_t index;
	int kind;
	int result;

	/*
 * Consumes at most one bounded event budget before declaring timeout.
	 */
	/* Process each remaining element. */
	for (index = 0U; index < INTEL_AX211_BOOT_EVENT_LIMIT; index++) {
		result = ax211_boot_receive(boot, deadline, &event, &message);

		/* Checks the operation result. */
		if (result != INTEL_AX211_BOOT_OK)
			return result;

		/* Handles the message condition. */
		if (message.generation != boot->generation)
			continue;

		/*
 * Rejects repeated accepted notifications in this generation.
		 */
		if ((event.queue & 0x80U) != 0U) {
			kind = ax211_boot_notification_kind(&message);

			/* Handles the ax211 boot notification duplicate condition. */
			if (ax211_boot_notification_duplicate(boot, kind))
				return INTEL_AX211_BOOT_DUPLICATE;

			/* Handles the kind condition. */
			if (kind != 0)
				return INTEL_AX211_BOOT_PROTOCOL;
			continue;
		}

		/*
 * Lets the command layer validate token, version, size, and
		 * flags. */
		result = drv_intel_ax211_command_complete(
			&boot->commands, boot->event_bytes,
			message.payload_length + INTEL_AX211_EVENT_HEADER_SIZE,
			boot->generation, response, response_capacity,
			response_length);

		/* Obtains the ax211 boot command result result. */
		function_result = ax211_boot_command_result(result);

		/* Returns the computed result. */
		return function_result;
	}

	/* Poisons the outstanding token once the local event budget expires. */
	result = boot->ops->clock_us(boot->argument, &now);

	/* Checks the operation result. */
	if (result == 0 && now < deadline)
		now = deadline;
	result = drv_intel_ax211_command_timeout_oldest(&boot->commands, now,
							&timed_out);

	/* Checks the operation result. */
	if (result != INTEL_AX211_COMMAND_TIMEOUT &&
	    result != INTEL_AX211_COMMAND_POISONED)

		/* Returns the computed result. */
		return INTEL_AX211_BOOT_COMMAND;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_TIMEOUT;
}

/* Decodes one exact little-endian generic firmware status. */
static uint32_t
ax211_boot_get_le32(
	const uint8_t bytes[4])
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Sends fixed-v1 NVM_ACCESS_COMPLETE and waits for its acknowledgement. */
static int
ax211_boot_send_nvm_access_complete(
	struct intel_ax211_boot *boot)
{
	int function_result;
	struct intel_ax211_command_handle handle;
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	int result;

	/*
 * Submits the 4-byte reserved request for a zero-payload
	 * acknowledgement. */
	result = ax211_boot_deadline(boot, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
				     &now, &deadline);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;
	result = drv_intel_ax211_command_submit_nvm_access_complete(
		&boot->commands, now, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
		&handle);

	/* Checks the operation result. */
	if (result != INTEL_AX211_COMMAND_OK) {
		/* Obtains the ax211 boot command result result. */
		function_result = ax211_boot_command_result(result);

		/* Returns the computed result. */
		return function_result;
	}
	response_length = 0U;
	result = ax211_boot_wait_command(boot, deadline, NULL, 0U,
					 &response_length);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Handles the response length condition. */
	if (response_length != 0U)
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}

/* Sends fixed-v1 NVM_GET_INFO and decodes its exact v4 response. */
static int
ax211_boot_send_nvm_get_info(
	struct intel_ax211_boot *boot)
{
	int function_result;
	struct intel_ax211_protocol_pending_command pending;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_handle handle;
	uint8_t response[INTEL_AX211_PROTOCOL_NVM_GET_INFO_SIZE];
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	int result;

	/*
 * Submits the exact four-byte request with a finite response deadline.
	 */
	result = ax211_boot_deadline(boot, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
				     &now, &deadline);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;
	result = drv_intel_ax211_command_submit_nvm_get_info(
		&boot->commands, now, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
		&handle);

	/* Checks the operation result. */
	if (result != INTEL_AX211_COMMAND_OK) {
		/* Obtains the ax211 boot command result result. */
		function_result = ax211_boot_command_result(result);

		/* Returns the computed result. */
		return function_result;
	}
	response_length = 0U;
	result = ax211_boot_wait_command(boot, deadline, response,
					 sizeof(response), &response_length);

	/* Checks the operation result. */
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Handles the response length condition. */
	if (response_length != sizeof(response))
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Reconstructs the validated response envelope for the NVM codec. */
	memset(&pending, 0, sizeof(pending));
	pending.opcode = INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE;
	pending.group = INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM;
	pending.response_version = INTEL_AX211_PROTOCOL_NVM_GET_INFO_VERSION;
	pending.queue = handle.token.queue;
	pending.index = handle.token.index;
	pending.generation = boot->generation;
	pending.minimum_response_length = sizeof(response);
	pending.maximum_response_length = sizeof(response);
	memset(&message, 0, sizeof(message));
	message.opcode = pending.opcode;
	message.group = pending.group;
	message.version = pending.response_version;
	message.queue = pending.queue;
	message.index = pending.index;
	message.generation = pending.generation;
	message.payload = response;
	message.payload_length = response_length;
	result = drv_intel_ax211_protocol_nvm_get_info_decode(
		&message, &pending, &boot->nvm);
	memset(response, 0, sizeof(response));

	/* Checks the operation result. */
	if (result != INTEL_AX211_PROTOCOL_OK) {
		/* Obtains the ax211 boot protocol result result. */
		function_result = ax211_boot_protocol_result(result);

		/* Returns the computed result. */
		return function_result;
	}
	boot->nvm_valid = 1U;

	/* Returns the computed result. */
	return INTEL_AX211_BOOT_OK;
}
