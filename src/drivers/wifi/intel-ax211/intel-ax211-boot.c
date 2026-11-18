/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private first-boot coordinator.
 */

#include "intel-ax211-boot.h"
#include <kern/kcrt.h>

/*
 * The notifications of the first boot, in the order they arrive; 0 is none of
 * them.
 */
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
static int ax211_boot_notification_kind(const struct intel_ax211_protocol_message *message);
static int ax211_boot_notification_duplicate(const struct intel_ax211_boot *boot, int kind);
static int ax211_boot_notification_accept(struct intel_ax211_boot *boot, int kind, const struct intel_ax211_protocol_message *message);
static int ax211_boot_select_and_publish_pnvm(struct intel_ax211_boot *boot);
static int ax211_boot_run_nvm_commands(struct intel_ax211_boot *boot);
static int ax211_boot_send_stage_select(struct intel_ax211_boot *boot);
static int ax211_boot_command_result(int result);
static int ax211_boot_wait_command(struct intel_ax211_boot *boot, uint64_t deadline, uint8_t *response, size_t response_capacity, size_t *response_length);
static uint32_t ax211_boot_get_le32(const uint8_t bytes[4]);
static int ax211_boot_send_nvm_handoff(struct intel_ax211_boot *boot);
static int ax211_boot_send_nvm_query(struct intel_ax211_boot *boot);

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
	int valid;

	/* Refuses a missing coordinator. */
	if (boot == NULL)
		return INTEL_AX211_BOOT_INVALID;

	/* Refuses incomplete controller operations. */
	valid = ax211_boot_ops_valid(ops);
	if (!valid)
		return INTEL_AX211_BOOT_INVALID;

	/* Refuses missing controller objects and an unset generation seed. */
	if (dma_device == NULL ||
	    mmio == NULL ||
	    transport == NULL ||
	    generation_seed == 0U) {
		return INTEL_AX211_BOOT_INVALID;
	}

	/* Refuses a MAC type the image is not built for. */
	mac_type = (uint16_t)((hardware_revision & 0xfff0U) >> 4);
	valid = drv_intel_ax211_mac_type_supported(mac_type);
	if (!valid)
		return INTEL_AX211_BOOT_INVALID;

	/* Refuses an MMIO state bound to another MAC or radio. */
	if (mmio->profile.mac_type != mac_type ||
	    mmio->profile.rf_type != rf_type) {
		return INTEL_AX211_BOOT_INVALID;
	}

	/* Publishes a fully initialized, otherwise empty coordinator. */
	kern_memset(boot, 0, sizeof(*boot));
	boot->ops = ops;
	boot->argument = argument;
	boot->dma_device = dma_device;
	boot->mmio = mmio;
	boot->transport = transport;
	boot->hardware_revision = hardware_revision;
	boot->rf_type = rf_type;
	boot->generation = generation_seed;
	boot->state = INTEL_AX211_BOOT_STATE_IDLE;

	/* Succeeded: the coordinator can run a first boot. */
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
	int result;

	/*
	 * Admits only a clean coordinator and keeps the output transactional.
	 */
	if (boot == NULL ||
	    nvm == NULL ||
	    (boot->state != INTEL_AX211_BOOT_STATE_IDLE &&
	     boot->state != INTEL_AX211_BOOT_STATE_COMPLETE) ||
	    boot->dma.device != NULL ||
	    boot->files_loaded) {
		return INTEL_AX211_BOOT_INVALID;
	}

	/* Starts a new run in a new hardware epoch. */
	ax211_boot_run_state_clear(boot);
	boot->state = INTEL_AX211_BOOT_STATE_RUNNING;
	ax211_boot_generation_advance(boot);

	/* Loads exact files and pins the borrowed command-version table. */
	result = ax211_boot_load_and_pin(boot);
	if (result != INTEL_AX211_BOOT_OK) {
		result = ax211_boot_finish(boot, result, nvm);
		return result;
	}

	/*
	 * Copies all first-pass DMA objects before hardware can observe them.
	 */
	result = ax211_boot_prepare_dma(boot);
	if (result != INTEL_AX211_BOOT_OK) {
		result = ax211_boot_finish(boot, result, nvm);
		return result;
	}

	/* Opens the controller's receive path for this epoch. */
	result = boot->ops->receive_epoch_begin(boot->argument,
						boot->generation);
	if (result != 0) {
		result = ax211_boot_finish(boot, INTEL_AX211_BOOT_IO, nvm);
		return result;
	}

	/* Starts the exact Gen3 transport. */
	result = ax211_boot_start_device(boot);
	if (result != INTEL_AX211_BOOT_OK) {
		result = ax211_boot_finish(boot, result, nvm);
		return result;
	}

	/* Accepts one ALIVE of this generation. */
	result = ax211_boot_wait_notification(
		boot, AX211_BOOT_NOTIFICATION_ALIVE,
		INTEL_AX211_BOOT_ALIVE_TIMEOUT_US);
	if (result != INTEL_AX211_BOOT_OK) {
		result = ax211_boot_finish(boot, result, nvm);
		return result;
	}

	/*
	 * Retires one-shot firmware images only after exact ALIVE acceptance.
	 */
	drv_intel_ax211_dma_release_boot_images(&boot->dma);

	/* Publishes the PNVM sections of the board ALIVE named. */
	result = ax211_boot_select_and_publish_pnvm(boot);
	if (result != INTEL_AX211_BOOT_OK) {
		result = ax211_boot_finish(boot, result, nvm);
		return result;
	}

	/*
	 * Runs the exact API89 read-NVM command sequence and stops the device.
	 */
	result = ax211_boot_run_nvm_commands(boot);
	result = ax211_boot_finish(boot, result, nvm);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Succeeded: *nvm holds the NVM answer of a stopped device. */
	return INTEL_AX211_BOOT_OK;
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
	    (!boot->dma_prepared && !boot->hardware_touched)) {
		return INTEL_AX211_BOOT_INVALID;
	}

	/* Repeats the complete checked stop boundary before releasing DMA. */
	result = ax211_boot_stop_and_release(boot);
	if (result != INTEL_AX211_BOOT_OK) {
		/* Records what the next retry still has to do. */
		if (boot->dma_prepared) {
			boot->state = INTEL_AX211_BOOT_STATE_STOP_REQUIRED;
		} else if (boot->hardware_touched) {
			boot->state =
				INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA;
		} else {
			boot->state = INTEL_AX211_BOOT_STATE_IDLE;
		}

		/* Reports the failed stop. */
		return result;
	}

	/* Succeeded: the device is stopped and the DMA released. */
	boot->state = INTEL_AX211_BOOT_STATE_IDLE;
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
	if (boot == NULL ||
	    table == NULL ||
	    boot->state != INTEL_AX211_BOOT_STATE_COMPLETE ||
	    !boot->command_table_valid) {
		return INTEL_AX211_BOOT_INVALID;
	}

	/* Describes the pinned bytes as a table. */
	result = drv_intel_ax211_protocol_command_table_parse(
		boot->command_version_bytes,
		sizeof(boot->command_version_bytes), table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Succeeded. */
	return INTEL_AX211_BOOT_OK;
}

/*
 * Reports whether every controller-owned operation the coordinator calls is
 * present: 1 when it is, 0 when not.
 */
static int
ax211_boot_ops_valid(
	const struct intel_ax211_boot_ops *ops)
{
	/* Rejects missing operations. */
	if (ops == NULL)
		return 0;

	/* Rejects operations without the receive path or the transport bind. */
	if (ops->receive_epoch_begin == NULL ||
	    ops->transport_bind == NULL ||
	    ops->receive_event == NULL) {
		return 0;
	}

	/*
	 * Rejects operations without the PNVM doorbell or the post-ALIVE step.
	 */
	if (ops->publish_pnvm == NULL || ops->after_alive == NULL)
		return 0;

	/* Rejects operations without the interrupt drain or the clock. */
	if (ops->interrupt_drain == NULL || ops->clock_us == NULL)
		return 0;

	/* Refuses ops which cannot end the DMA ownership after a reset. */
	if (ops->bus_master_disable == NULL)
		return 0;

	/* The operations are complete. */
	return 1;
}

/* Clears transient state without invalidating controller dependencies. */
static void
ax211_boot_run_state_clear(
	struct intel_ax211_boot *boot)
{
	kern_memset(&boot->files, 0, sizeof(boot->files));
	kern_memset(&boot->dma, 0, sizeof(boot->dma));
	kern_memset(&boot->commands, 0, sizeof(boot->commands));
	kern_memset(&boot->alive, 0, sizeof(boot->alive));
	kern_memset(&boot->nvm, 0, sizeof(boot->nvm));
	kern_memset(boot->command_version_bytes, 0,
	       sizeof(boot->command_version_bytes));
	kern_memset(boot->event_bytes, 0, sizeof(boot->event_bytes));
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
	/* Skips zero when the counter wraps. */
	boot->generation++;
	if (boot->generation == 0U)
		boot->generation = 1U;
}

/* Loads exact artifacts and copies every borrowed version byte. */
static int
ax211_boot_load_and_pin(
	struct intel_ax211_boot *boot)
{
	struct intel_ax211_protocol_command_table table;
	size_t offset;
	size_t length;
	int result;

	/* Loads and marks ownership before inspecting any borrowed member. */
	result = drv_intel_ax211_firmware_files_load(&boot->files);
	if (result != 0)
		return INTEL_AX211_BOOT_FIRMWARE;
	boot->files_loaded = 1U;

	/* Requires both exact file buffers and the full API89 version table. */
	if (boot->files.ucode_bytes == NULL ||
	    boot->files.ucode_size == 0U ||
	    boot->files.pnvm_bytes == NULL ||
	    boot->files.pnvm_size == 0U) {
		return INTEL_AX211_BOOT_FIRMWARE;
	}

	/* Refuses a version record of another length. */
	offset = boot->files.ucode_manifest.command_versions_offset;
	length = boot->files.ucode_manifest.command_versions_length;
	if (length != sizeof(boot->command_version_bytes))
		return INTEL_AX211_BOOT_FIRMWARE;

	/* Refuses a version record outside the image. */
	if (offset > boot->files.ucode_size ||
	    length > boot->files.ucode_size - offset) {
		return INTEL_AX211_BOOT_FIRMWARE;
	}

	/* Pins the table before any path may release firmware-file storage. */
	kern_memcpy(boot->command_version_bytes, boot->files.ucode_bytes + offset,
	       length);

	/* Describes the pinned bytes as a table. */
	result = drv_intel_ax211_protocol_command_table_parse(
		boot->command_version_bytes, length, &table);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		result = ax211_boot_protocol_result(result);
		return result;
	}

	/* Requires the versions of the initialization commands. */
	result = drv_intel_ax211_init_api89_validate(&table);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		result = ax211_boot_protocol_result(result);
		return result;
	}

	/* Succeeded: the table is pinned. */
	boot->command_table_valid = 1U;
	return INTEL_AX211_BOOT_OK;
}

/* Maps all exact protocol rejections to the coordinator boundary. */
static int
ax211_boot_protocol_result(
	int result)
{
	/* A protocol success is a boot success. */
	if (result == INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_BOOT_OK;

	/* A repeat stays a repeat. */
	if (result == INTEL_AX211_PROTOCOL_DUPLICATE)
		return INTEL_AX211_BOOT_DUPLICATE;

	/* Any other rejection is a protocol failure. */
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

	/* A stop that must be retried is reported before anything else. */
	cleanup_result = ax211_boot_stop_and_release(boot);
	if (cleanup_result == INTEL_AX211_BOOT_STOP_REQUIRED)
		return cleanup_result;

	/* Reports the run's own failure. */
	if (result != INTEL_AX211_BOOT_OK) {
		boot->state = INTEL_AX211_BOOT_STATE_IDLE;
		return result;
	}

	/* Reports a failed cleanup of a successful run. */
	if (cleanup_result != INTEL_AX211_BOOT_OK) {
		boot->state = INTEL_AX211_BOOT_STATE_IDLE;
		return cleanup_result;
	}

	/* Refuses a run that ended without an NVM answer. */
	if (!boot->nvm_valid) {
		boot->state = INTEL_AX211_BOOT_STATE_IDLE;
		return INTEL_AX211_BOOT_PROTOCOL;
	}

	/*
	 * Publishes the copied NVM only after a complete checked device stop.
	 */
	*nvm = boot->nvm;
	boot->state = INTEL_AX211_BOOT_STATE_COMPLETE;
	boot->last_error = INTEL_AX211_BOOT_OK;

	/* Succeeded: the first boot is complete. */
	return INTEL_AX211_BOOT_OK;
}

/* Releases source-file ownership exactly once. */
static void
ax211_boot_release_files(
	struct intel_ax211_boot *boot)
{
	/* Nothing to release when no files are held. */
	if (!boot->files_loaded)
		return;

	/* Releases the files. */
	drv_intel_ax211_firmware_files_release(&boot->files);
	boot->files_loaded = 0U;
}

/*
 * Stops device ownership and releases DMA only across a proven safe boundary.
 */
static int
ax211_boot_stop_and_release(
	struct intel_ax211_boot *boot)
{
	int after_reset_result;
	int bus_master_result;
	int drain_result;
	int quiesce_result;
	int stop_result;

	/* A prior no-DMA failure still requires a checked controller reset. */
	if (!boot->dma_prepared) {
		/* Nothing to stop when the hardware was not touched. */
		if (!boot->hardware_touched)
			return INTEL_AX211_BOOT_OK;

		/* Stops the device; a failure must be retried. */
		stop_result = drv_intel_ax211_mmio_stop(boot->mmio);
		if (stop_result != INTEL_AX211_MMIO_OK) {
			boot->state =
				INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA;
			return INTEL_AX211_BOOT_STOP_REQUIRED;
		}

		/* The device is stopped. */
		boot->hardware_touched = 0U;
		return INTEL_AX211_BOOT_OK;
	}

	/*
	 * Never-exposed DMA is safe to free, but reset failure remains sticky.
	 */
	if (!boot->dma_exposed && !boot->transport_bound) {
		/* Stops a touched device and frees the DMA. */
		stop_result = INTEL_AX211_MMIO_OK;
		if (boot->hardware_touched)
			stop_result = drv_intel_ax211_mmio_stop(boot->mmio);
		drv_intel_ax211_dma_release(&boot->dma);
		boot->dma_prepared = 0U;

		/* A failed stop must be retried. */
		if (stop_result != INTEL_AX211_MMIO_OK) {
			boot->state =
				INTEL_AX211_BOOT_STATE_STOP_REQUIRED_NO_DMA;
			return INTEL_AX211_BOOT_STOP_REQUIRED;
		}

		/* The device is stopped. */
		boot->hardware_touched = 0U;
		return INTEL_AX211_BOOT_OK;
	}

	/*
	 * Stops submissions and RX before draining the sole interrupt handler.
	 */
	quiesce_result = drv_intel_ax211_transport_quiesce(boot->transport);
	drain_result = boot->ops->interrupt_drain(boot->argument);

	/* Resets the device, which stops its DMA; bus mastering is still on. */
	stop_result = drv_intel_ax211_mmio_stop(boot->mmio);
	if (drain_result != 0 || stop_result != INTEL_AX211_MMIO_OK) {
		boot->state = INTEL_AX211_BOOT_STATE_STOP_REQUIRED;

		/*
		 * Failed: the DMA and bus mastering are retained for the retry.
		 */
		return INTEL_AX211_BOOT_STOP_REQUIRED;
	}

	/*
	 * Ends the device's DMA ownership only after the reset, never while the
	 * firmware may still write DMA (BUG-158).
	 */
	bus_master_result = boot->ops->bus_master_disable(boot->argument);
	if (bus_master_result != 0) {
		boot->state = INTEL_AX211_BOOT_STATE_STOP_REQUIRED;

		/* Failed: the DMA is retained until bus mastering is off. */
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

	/* Reports a transport that failed to quiesce or to clean up. */
	if (quiesce_result != INTEL_AX211_TRANSPORT_OK ||
	    after_reset_result != INTEL_AX211_TRANSPORT_OK) {
		return INTEL_AX211_BOOT_TRANSPORT;
	}

	/* Succeeded: the device is stopped and the DMA released. */
	return INTEL_AX211_BOOT_OK;
}

/* Copies firmware and all runtime rings into controller-owned DMA. */
static int
ax211_boot_prepare_dma(
	struct intel_ax211_boot *boot)
{
	int result;

	/* Allocates and fills the DMA memory of the boot. */
	result = drv_intel_ax211_dma_prepare_boot(
		boot->dma_device, boot->files.ucode_bytes,
		boot->files.ucode_size, &boot->files.ucode_manifest,
		boot->hardware_revision, &boot->dma);
	if (result != 0)
		return INTEL_AX211_BOOT_DMA;

	/* Succeeded: the DMA memory is prepared. */
	boot->dma_prepared = 1U;
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
	result = drv_intel_ax211_mmio_take_ownership(boot->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;

	/* Resets the device. */
	result = drv_intel_ax211_mmio_sw_reset(boot->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;

	/* Powers the MAC clock. */
	result = drv_intel_ax211_mmio_apm_init(boot->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;

	/* Binds the sole controller transport before exposing ring storage. */
	result = boot->ops->transport_bind(boot->argument, &boot->dma,
					   boot->mmio, boot->transport,
					   boot->generation);
	if (result != 0)
		return INTEL_AX211_BOOT_TRANSPORT;
	boot->transport_bound = 1U;

	/* Routes the interrupts. */
	result = drv_intel_ax211_transport_configure_msix(boot->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_BOOT_TRANSPORT;

	/* Treats partial ring publication as device DMA ownership. */
	boot->dma_exposed = 1U;
	result = drv_intel_ax211_transport_initialize_rings(boot->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_BOOT_TRANSPORT;

	/* Refuses DMA memory without every receive buffer. */
	if (boot->dma.rx_buffer_count != INTEL_AX211_RX_RING_SIZE)
		return INTEL_AX211_BOOT_DMA;

	/* Publishes every exact RX buffer before firmware can configure RFH. */
	for (index = 0U; index < boot->dma.rx_buffer_count; index++) {
		/* Refuses a receive buffer without an address. */
		if (boot->dma.rx_buffer[index].address == NULL ||
		    boot->dma.rx_buffer[index].device_address == 0U) {
			return INTEL_AX211_BOOT_DMA;
		}

		/* Publishes the buffer's descriptor. */
		result = drv_intel_ax211_transport_publish_rx_descriptor(
			boot->transport, (uint16_t)index,
			boot->dma.rx_buffer[index].device_address);
		if (result != INTEL_AX211_TRANSPORT_OK)
			return INTEL_AX211_BOOT_TRANSPORT;
	}

	/*
	 * Enables the firmware's interrupts; the first RX credit waits for the
	 * hardware-ALIVE cause.
	 */
	result = drv_intel_ax211_transport_enable_firmware_interrupts(
		boot->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_BOOT_TRANSPORT;

	/*
	 * Publishes context and IML only after rings and interrupts are ready.
	 */
	if (boot->dma.context.device_address == 0U ||
	    boot->dma.iml.device_address == 0U ||
	    boot->dma.iml.size != INTEL_AX211_MMIO_LOADER_IMAGE_SIZE) {
		return INTEL_AX211_BOOT_DMA;
	}

	/* Describes the boot context and the loader to the MMIO layer. */
	kern_memset(&mmio_boot, 0, sizeof(mmio_boot));
	mmio_boot.context_address = boot->dma.context.device_address;
	mmio_boot.iml_address = boot->dma.iml.device_address;
	mmio_boot.iml_size = (uint32_t)boot->dma.iml.size;

	/* Starts the firmware. */
	result = drv_intel_ax211_mmio_publish_gen3(boot->mmio, &mmio_boot);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_BOOT_MMIO;

	/* Succeeded: the firmware boots. */
	return INTEL_AX211_BOOT_OK;
}

/* Waits finitely for one exact generation-tagged notification. */
static int
ax211_boot_wait_notification(
	struct intel_ax211_boot *boot,
	enum ax211_boot_notification expected,
	uint64_t timeout)
{
	struct intel_ax211_protocol_message message;
	struct intel_ax211_event event;
	uint64_t deadline;
	uint64_t now;
	size_t index;
	int kind;
	int duplicate;
	int result;

	/* Fixes one monotonic absolute deadline for the entire wait. */
	result = ax211_boot_deadline(boot, timeout, &now, &deadline);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/*
	 * Discards stale generations but never extends the original deadline.
	 */
	for (index = 0U; index < INTEL_AX211_BOOT_EVENT_LIMIT; index++) {
		/* Receives the next event. */
		result = ax211_boot_receive(boot, deadline, &event, &message);
		if (result != INTEL_AX211_BOOT_OK)
			return result;

		/* Skips an event of an earlier generation. */
		if (message.generation != boot->generation)
			continue;

		/* Refuses a command answer while a notification is awaited. */
		if ((event.queue & 0x80U) == 0U)
			return INTEL_AX211_BOOT_PROTOCOL;

		/* Refuses a repeat of a notification already accepted. */
		kind = ax211_boot_notification_kind(&message);
		duplicate = ax211_boot_notification_duplicate(boot, kind);
		if (duplicate)
			return INTEL_AX211_BOOT_DUPLICATE;

		/* Skips a notification that is not part of the boot order. */
		if (kind == 0)
			continue;

		/* Refuses a boot notification out of order. */
		if (kind != (int)expected)
			return INTEL_AX211_BOOT_PROTOCOL;

		/* Validates and records the awaited notification. */
		result = ax211_boot_notification_accept(boot, kind, &message);
		return result;
	}

	/* The event budget ran out. */
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

	/* Refuses a zero timeout and missing outputs. */
	if (timeout == 0U || now == NULL || deadline == NULL)
		return INTEL_AX211_BOOT_INVALID;

	/* Reads the clock. */
	result = boot->ops->clock_us(boot->argument, now);
	if (result != 0)
		return INTEL_AX211_BOOT_IO;

	/* Refuses a deadline that overflows the clock. */
	if (*now > UINT64_MAX - timeout)
		return INTEL_AX211_BOOT_IO;

	/* Succeeded: reports the deadline. */
	*deadline = *now + timeout;
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
	kern_memset(&received, 0, sizeof(received));
	result = boot->ops->receive_event(boot->argument, deadline,
					  boot->event_bytes,
					  sizeof(boot->event_bytes), &received);
	if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
		return INTEL_AX211_BOOT_TIMEOUT;
	if (result != INTEL_AX211_BOOT_RECEIVE_OK)
		return INTEL_AX211_BOOT_IO;

	/* Refuses an empty or oversized event and an unset generation. */
	if (received.length == 0U ||
	    received.length > sizeof(boot->event_bytes) ||
	    received.generation == 0U) {
		return INTEL_AX211_BOOT_PROTOCOL;
	}

	/*
	 * Rejects a receiver which returned after the caller's fixed deadline.
	 */
	result = boot->ops->clock_us(boot->argument, &now);
	if (result != 0)
		return INTEL_AX211_BOOT_IO;
	if (now > deadline)
		return INTEL_AX211_BOOT_TIMEOUT;

	/* Decodes the common RX envelope before attaching private metadata. */
	result = drv_intel_ax211_event_decode(boot->event_bytes,
					      received.length, event);
	if (result != INTEL_AX211_SUCCESS)
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Refuses a payload that does not end where the event ends. */
	if (event->payload_offset > received.length ||
	    event->payload_length != received.length - event->payload_offset) {
		return INTEL_AX211_BOOT_PROTOCOL;
	}

	/* Builds the message the protocol layer validates. */
	kern_memset(message, 0, sizeof(*message));
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

	/* Succeeded: the event is decoded. */
	return INTEL_AX211_BOOT_OK;
}

/* Classifies only notifications which participate in the first-pass order. */
static int
ax211_boot_notification_kind(
	const struct intel_ax211_protocol_message *message)
{
	/* The ALIVE notification. */
	if (message->group == INTEL_AX211_PROTOCOL_FAMILY_CLASSIC &&
	    message->opcode == INTEL_AX211_PROTOCOL_ALIVE_OPCODE) {
		return AX211_BOOT_NOTIFICATION_ALIVE;
	}

	/* The PNVM-accepted notification. */
	if (message->group == INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE &&
	    message->opcode == INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_OPCODE) {
		return AX211_BOOT_NOTIFICATION_PNVM;
	}

	/* The init-finished notification. */
	if (message->group == INTEL_AX211_PROTOCOL_FAMILY_CLASSIC &&
	    message->opcode == INTEL_AX211_PROTOCOL_INIT_FINISHED_OPCODE) {
		return AX211_BOOT_NOTIFICATION_INIT;
	}

	/* Any other notification is not part of the boot order. */
	return 0;
}

/*
 * Reports whether a notification was already accepted in the current
 * generation: 1 when it was, 0 when not.
 */
static int
ax211_boot_notification_duplicate(
	const struct intel_ax211_boot *boot,
	int kind)
{
	/* A second ALIVE. */
	if (kind == AX211_BOOT_NOTIFICATION_ALIVE && boot->alive_accepted)
		return 1;

	/* A second PNVM-accepted notification. */
	if (kind == AX211_BOOT_NOTIFICATION_PNVM && boot->pnvm_accepted)
		return 1;

	/* A second init-finished notification. */
	if (kind == AX211_BOOT_NOTIFICATION_INIT && boot->init_accepted)
		return 1;

	/* The notification is new. */
	return 0;
}

/* Validates and records one expected notification transactionally. */
static int
ax211_boot_notification_accept(
	struct intel_ax211_boot *boot,
	int kind,
	const struct intel_ax211_protocol_message *message)
{
	int result;

	/* Validates the notification by its kind and records a valid one. */
	if (kind == AX211_BOOT_NOTIFICATION_ALIVE) {
		/* Decodes ALIVE into the coordinator. */
		result = drv_intel_ax211_protocol_alive_decode(
			message, boot->generation, &boot->alive);
		if (result == INTEL_AX211_PROTOCOL_OK)
			boot->alive_accepted = 1U;
	} else if (kind == AX211_BOOT_NOTIFICATION_PNVM) {
		/* Validates the PNVM-accepted notification. */
		result = drv_intel_ax211_protocol_pnvm_accepted(
			message, boot->generation);
		if (result == INTEL_AX211_PROTOCOL_OK)
			boot->pnvm_accepted = 1U;
	} else if (kind == AX211_BOOT_NOTIFICATION_INIT) {
		/* Validates the init-finished notification. */
		result = drv_intel_ax211_init_finished_validate(
			message, boot->generation);
		if (result == INTEL_AX211_PROTOCOL_OK)
			boot->init_accepted = 1U;
	} else {
		return INTEL_AX211_BOOT_PROTOCOL;
	}

	/* Reports a rejected notification. */
	result = ax211_boot_protocol_result(result);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Succeeded: the notification is recorded. */
	return INTEL_AX211_BOOT_OK;
}

/* Selects the ALIVE SKU and publishes an exact copied PNVM image. */
static int
ax211_boot_select_and_publish_pnvm(
	struct intel_ax211_boot *boot)
{
	struct intel_ax211_pnvm_manifest manifest;
	struct intel_ax211_board_signature sku;
	int result;

	/* Selects only the exact three-word SKU from accepted ALIVE. */
	kern_memset(&sku, 0, sizeof(sku));
	sku.data[0] = boot->alive.sku[0];
	sku.data[1] = boot->alive.sku[1];
	sku.data[2] = boot->alive.sku[2];
	result = drv_intel_ax211_pnvm_select(
		boot->files.pnvm_bytes, boot->files.pnvm_size, &sku,
		boot->mmio->profile.mac_type, boot->rf_type, &manifest);
	if (result != INTEL_AX211_SUCCESS)
		return INTEL_AX211_BOOT_FIRMWARE;

	/* Copies every selected segment before releasing both source files. */
	result = drv_intel_ax211_dma_prepare_pnvm(boot->files.pnvm_bytes,
						  boot->files.pnvm_size,
						  &manifest, &boot->dma);
	if (result != 0)
		return INTEL_AX211_BOOT_DMA;
	ax211_boot_release_files(boot);

	/* Synchronizes and rings the controller-owned PNVM doorbell. */
	result = boot->ops->publish_pnvm(boot->argument, &boot->dma);
	if (result != 0)
		return INTEL_AX211_BOOT_IO;

	/* Waits for the firmware to accept the sections. */
	result =
		ax211_boot_wait_notification(boot, AX211_BOOT_NOTIFICATION_PNVM,
					     INTEL_AX211_BOOT_PNVM_TIMEOUT_US);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Applies the transport's exact post-ALIVE hardware transition. */
	result = boot->ops->after_alive(boot->argument, &boot->alive);
	if (result != 0)
		return INTEL_AX211_BOOT_IO;

	/* Succeeded: the firmware has its radio tuning data. */
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
	if (result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_BOOT_COMMAND;

	/* Marks the firmware session as an exact read-NVM pass. */
	result = ax211_boot_send_stage_select(boot);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Hands the NVM over. */
	result = ax211_boot_send_nvm_handoff(boot);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Waits for the end of the initialization stage. */
	result =
		ax211_boot_wait_notification(boot, AX211_BOOT_NOTIFICATION_INIT,
					     INTEL_AX211_BOOT_INIT_TIMEOUT_US);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Retrieves and decodes one exact v4 NVM response. */
	result = ax211_boot_send_nvm_query(boot);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Succeeded: the NVM answer is decoded. */
	return INTEL_AX211_BOOT_OK;
}

/* Sends fixed-v1 INIT_EXTENDED_CFG and waits for its empty acknowledgement. */
static int
ax211_boot_send_stage_select(
	struct intel_ax211_boot *boot)
{
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	uint8_t payload[INTEL_AX211_INIT_STAGE_SELECT_SIZE];
	uint8_t response[4];
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	uint32_t status;
	int result;

	/* Encodes the exact read-NVM profile. */
	result = drv_intel_ax211_init_stage_select_encode(
		INTEL_AX211_INIT_PROFILE_STORE_READ, payload);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		result = ax211_boot_protocol_result(result);
		return result;
	}

	/*
	 * Describes the command.  It sends zero in the wide-header wire-version
	 * byte, as API 89 accepts.
	 *
	 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
	 * (iwx_send_cmd).
	 */
	kern_memset(&request, 0, sizeof(request));
	request.command.opcode = INTEL_AX211_INIT_STAGE_SELECT_OPCODE;
	request.command.group = INTEL_AX211_INIT_FAMILY_PLATFORM;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = sizeof(response);
	request.maximum_response_length = sizeof(response);

	/* Fixes one finite acknowledgement deadline. */
	result = ax211_boot_deadline(boot, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
				     &now, &deadline);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Submits the command. */
	result = drv_intel_ax211_command_submit(
		&boot->commands, &request, now,
		INTEL_AX211_BOOT_COMMAND_TIMEOUT_US, &handle);
	if (result != INTEL_AX211_COMMAND_OK) {
		result = ax211_boot_command_result(result);
		return result;
	}

	/* Waits for the one-word answer. */
	response_length = 0U;
	kern_memset(response, 0, sizeof(response));
	result = ax211_boot_wait_command(boot, deadline, response,
					 sizeof(response), &response_length);
	if (result == INTEL_AX211_BOOT_OK &&
	    response_length != sizeof(response))
		result = INTEL_AX211_BOOT_PROTOCOL;

	/* A nonzero status word means the firmware refused the profile. */
	if (result == INTEL_AX211_BOOT_OK) {
		/* Reads the status word. */
		status = ax211_boot_get_le32(response);
		if (status != 0U)
			result = INTEL_AX211_BOOT_COMMAND;
	}

	/* Clears the answer and reports the outcome. */
	kern_memset(response, 0, sizeof(response));
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Succeeded: the firmware will stop after reading its NVM. */
	return INTEL_AX211_BOOT_OK;
}

/* Maps command transaction outcomes without weakening token validation. */
static int
ax211_boot_command_result(
	int result)
{
	/* A command success is a boot success. */
	if (result == INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_BOOT_OK;

	/* A command timeout is a boot timeout. */
	if (result == INTEL_AX211_COMMAND_TIMEOUT)
		return INTEL_AX211_BOOT_TIMEOUT;

	/* A repeat stays a repeat. */
	if (result == INTEL_AX211_COMMAND_DUPLICATE)
		return INTEL_AX211_BOOT_DUPLICATE;

	/*
	 * A malformed, failed, mismatched, misplaced or oversized answer is a
	 * protocol failure.
	 */
	if (result == INTEL_AX211_COMMAND_MALFORMED ||
	    result == INTEL_AX211_COMMAND_FIRMWARE_FAILED ||
	    result == INTEL_AX211_COMMAND_VERSION_MISMATCH ||
	    result == INTEL_AX211_COMMAND_OUT_OF_ORDER ||
	    result == INTEL_AX211_COMMAND_BUFFER_TOO_SMALL) {
		return INTEL_AX211_BOOT_PROTOCOL;
	}

	/* Any other outcome is a command failure. */
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
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_handle timed_out;
	struct intel_ax211_event event;
	uint64_t now;
	size_t index;
	int kind;
	int duplicate;
	int result;

	/*
	 * Consumes at most one bounded event budget before declaring timeout.
	 */
	for (index = 0U; index < INTEL_AX211_BOOT_EVENT_LIMIT; index++) {
		/* Receives the next event. */
		result = ax211_boot_receive(boot, deadline, &event, &message);
		if (result != INTEL_AX211_BOOT_OK)
			return result;

		/* Skips an event of an earlier generation. */
		if (message.generation != boot->generation)
			continue;

		/*
		 * Rejects repeated accepted notifications in this generation.
		 */
		if ((event.queue & 0x80U) != 0U) {
			/*
			 * Refuses a repeat of a notification already accepted.
			 */
			kind = ax211_boot_notification_kind(&message);
			duplicate = ax211_boot_notification_duplicate(boot, kind);
			if (duplicate)
				return INTEL_AX211_BOOT_DUPLICATE;

			/*
			 * Refuses a boot notification while an answer is
			 * awaited.
			 */
			if (kind != 0)
				return INTEL_AX211_BOOT_PROTOCOL;

			/* Skips any other notification. */
			continue;
		}

		/*
		 * Lets the command layer validate token, version, size, and
		 * flags.
		 */
		result = drv_intel_ax211_command_complete(
			&boot->commands, boot->event_bytes,
			message.payload_length + INTEL_AX211_EVENT_HEADER_SIZE,
			boot->generation, response, response_capacity,
			response_length);

		/* Reports the answer's outcome. */
		result = ax211_boot_command_result(result);
		return result;
	}

	/* Poisons the outstanding token once the local event budget expires. */
	result = boot->ops->clock_us(boot->argument, &now);
	if (result == 0 && now < deadline)
		now = deadline;

	/* Times out the command; any other outcome is a command failure. */
	result = drv_intel_ax211_command_timeout_oldest(&boot->commands, now,
							&timed_out);
	if (result != INTEL_AX211_COMMAND_TIMEOUT &&
	    result != INTEL_AX211_COMMAND_POISONED) {
		return INTEL_AX211_BOOT_COMMAND;
	}

	/* Reports the timeout. */
	return INTEL_AX211_BOOT_TIMEOUT;
}

/* Decodes one exact little-endian generic firmware status. */
static uint32_t
ax211_boot_get_le32(
	const uint8_t bytes[4])
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Sends fixed-v1 NVM_ACCESS_COMPLETE and waits for its acknowledgement. */
static int
ax211_boot_send_nvm_handoff(
	struct intel_ax211_boot *boot)
{
	struct intel_ax211_command_handle handle;
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	int result;

	/* Fixes one finite acknowledgement deadline. */
	result = ax211_boot_deadline(boot, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
				     &now, &deadline);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/*
	 * Submits the 4-byte reserved request for a zero-payload
	 * acknowledgement.
	 */
	result = drv_intel_ax211_command_submit_nvm_handoff(
		&boot->commands, now, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
		&handle);
	if (result != INTEL_AX211_COMMAND_OK) {
		result = ax211_boot_command_result(result);
		return result;
	}

	/* Waits for the acknowledgement. */
	response_length = 0U;
	result = ax211_boot_wait_command(boot, deadline, NULL, 0U,
					 &response_length);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Refuses an acknowledgement with a payload. */
	if (response_length != 0U)
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Succeeded. */
	return INTEL_AX211_BOOT_OK;
}

/* Sends fixed-v1 NVM_GET_INFO and decodes its exact v4 response. */
static int
ax211_boot_send_nvm_query(
	struct intel_ax211_boot *boot)
{
	struct intel_ax211_protocol_pending_command pending;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_handle handle;
	uint8_t response[INTEL_AX211_PROTOCOL_NVM_QUERY_SIZE];
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	int result;

	/* Fixes one finite response deadline. */
	result = ax211_boot_deadline(boot, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
				     &now, &deadline);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Submits the exact four-byte request. */
	result = drv_intel_ax211_command_submit_nvm_query(
		&boot->commands, now, INTEL_AX211_BOOT_COMMAND_TIMEOUT_US,
		&handle);
	if (result != INTEL_AX211_COMMAND_OK) {
		result = ax211_boot_command_result(result);
		return result;
	}

	/* Waits for the answer. */
	response_length = 0U;
	result = ax211_boot_wait_command(boot, deadline, response,
					 sizeof(response), &response_length);
	if (result != INTEL_AX211_BOOT_OK)
		return result;

	/* Refuses an answer of another length. */
	if (response_length != sizeof(response))
		return INTEL_AX211_BOOT_PROTOCOL;

	/* Reconstructs the validated response envelope for the NVM codec. */
	kern_memset(&pending, 0, sizeof(pending));
	pending.opcode = INTEL_AX211_PROTOCOL_NVM_QUERY_OPCODE;
	pending.group = INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE;
	pending.response_version = INTEL_AX211_PROTOCOL_NVM_QUERY_VERSION;
	pending.queue = handle.token.queue;
	pending.index = handle.token.index;
	pending.generation = boot->generation;
	pending.minimum_response_length = sizeof(response);
	pending.maximum_response_length = sizeof(response);
	kern_memset(&message, 0, sizeof(message));
	message.opcode = pending.opcode;
	message.group = pending.group;
	message.version = pending.response_version;
	message.queue = pending.queue;
	message.index = pending.index;
	message.generation = pending.generation;
	message.payload = response;
	message.payload_length = response_length;

	/* Decodes the answer and clears the copy. */
	result = drv_intel_ax211_protocol_nvm_query_decode(
		&message, &pending, &boot->nvm);
	kern_memset(response, 0, sizeof(response));
	if (result != INTEL_AX211_PROTOCOL_OK) {
		result = ax211_boot_protocol_result(result);
		return result;
	}

	/* Succeeded: the NVM answer is decoded. */
	boot->nvm_valid = 1U;
	return INTEL_AX211_BOOT_OK;
}
