/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private operational firmware coordinator.
 */

#include "intel-ax211-runtime-start.h"
#include <kern/kcrt.h>

/*
 * The notifications of the operational start, in the order they arrive; 0 is
 * none of them.
 */
enum ax211_runtime_start_notification {
	AX211_RUNTIME_START_NOTIFICATION_ALIVE = 1,
	AX211_RUNTIME_START_NOTIFICATION_PNVM = 2,
	AX211_RUNTIME_START_NOTIFICATION_INIT = 3
};

static int ax211_runtime_start_ops_valid(const struct intel_ax211_runtime_start_ops *ops);
static void ax211_runtime_start_run_state_clear(struct intel_ax211_runtime_start *session);
static uint32_t ax211_runtime_start_next_generation(uint32_t generation);
static int ax211_runtime_start_load_and_profile(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_runtime_result(int result);
static int ax211_runtime_start_fail(struct intel_ax211_runtime_start *session, int result);
static void ax211_runtime_start_release_files(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_stop_and_release(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_prepare_dma(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_device(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_wait_notification(struct intel_ax211_runtime_start *session, enum ax211_runtime_start_notification expected, uint64_t timeout);
static int ax211_runtime_start_deadline(struct intel_ax211_runtime_start *session, uint64_t timeout, uint64_t *now, uint64_t *deadline);
static int ax211_runtime_start_receive(struct intel_ax211_runtime_start *session, uint64_t deadline, struct intel_ax211_event *event, struct intel_ax211_protocol_message *message);
static int ax211_runtime_start_notification_kind(const struct intel_ax211_protocol_message *message);
static int ax211_runtime_start_notification_duplicate(const struct intel_ax211_runtime_start *session, int kind);
static int ax211_runtime_start_notification_accept(struct intel_ax211_runtime_start *session, int kind, const struct intel_ax211_protocol_message *message);
static int ax211_runtime_start_protocol_result(int result);
static int ax211_runtime_start_select_and_publish_pnvm(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_init_firmware(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_send_stage_select(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_submit(struct intel_ax211_runtime_start *session, const struct intel_ax211_command_request *request, struct intel_ax211_command_handle *handle, uint64_t *deadline);
static int ax211_runtime_start_command_result(int result);
static int ax211_runtime_start_wait_command(struct intel_ax211_runtime_start *session, uint64_t deadline, uint8_t *response, size_t response_capacity, size_t *response_length);
static uint32_t ax211_runtime_start_get_le32(const uint8_t *bytes);
static int ax211_runtime_start_send_nvm_handoff(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_commands(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_commands_locked(struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_send_step(struct intel_ax211_runtime_start *session, enum intel_ax211_runtime_step step, const struct intel_ax211_runtime_command *command);

/*
 * Copies the read-NVM outputs into one idle operational session.
 */
int
drv_intel_ax211_runtime_start_init(
	struct intel_ax211_runtime_start *session,
	const struct intel_ax211_runtime_start_ops *ops,
	void *argument,
	struct drv_dma_device *dma_device,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_transport *transport,
	uint16_t hardware_revision,
	uint16_t rf_type,
	const struct intel_ax211_protocol_command_table *command_table,
	const struct intel_ax211_protocol_nvm *nvm,
	int ltr_enabled,
	uint32_t generation_seed)
{
	struct intel_ax211_protocol_command_table copied_table;
	uint16_t mac_type;
	int valid;
	int result;

	/* Refuses a missing session. */
	if (session == NULL)
		return INTEL_AX211_RUNTIME_START_INVALID;

	/* Refuses incomplete controller operations. */
	valid = ax211_runtime_start_ops_valid(ops);
	if (!valid)
		return INTEL_AX211_RUNTIME_START_INVALID;

	/* Refuses missing controller objects and missing read-NVM outputs. */
	if (dma_device == NULL ||
	    mmio == NULL ||
	    transport == NULL ||
	    command_table == NULL ||
	    command_table->bytes == NULL ||
	    nvm == NULL) {
		return INTEL_AX211_RUNTIME_START_INVALID;
	}

	/*
	 * Refuses a table of another size, an LTR choice other than 0 or 1, and
	 * an unset generation seed.
	 */
	if (command_table->count != INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT ||
	    (ltr_enabled != 0 && ltr_enabled != 1) ||
	    generation_seed == 0U) {
		return INTEL_AX211_RUNTIME_START_INVALID;
	}

	/* Refuses a MAC type the image is not built for. */
	mac_type = (uint16_t)((hardware_revision & 0xfff0U) >> 4);
	valid = drv_intel_ax211_mac_type_supported(mac_type);
	if (!valid)
		return INTEL_AX211_RUNTIME_START_INVALID;

	/* Refuses an MMIO state bound to another MAC or radio. */
	if (mmio->profile.mac_type != mac_type ||
	    mmio->profile.rf_type != rf_type) {
		return INTEL_AX211_RUNTIME_START_INVALID;
	}

	/*
	 * Owns the exact table instead of retaining the boot coordinator's
	 * view.
	 */
	kern_memset(session, 0, sizeof(*session));
	kern_memcpy(session->command_version_bytes, command_table->bytes,
	       sizeof(session->command_version_bytes));

	/* Describes the copied bytes as a table. */
	result = drv_intel_ax211_protocol_command_table_parse(
		session->command_version_bytes,
		sizeof(session->command_version_bytes), &copied_table);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		kern_memset(session, 0, sizeof(*session));
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}

	/* Requires the versions of the initialization commands. */
	result = drv_intel_ax211_init_api89_validate(&copied_table);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		kern_memset(session, 0, sizeof(*session));
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}

	/* Records everything the session runs against. */
	session->ops = ops;
	session->argument = argument;
	session->dma_device = dma_device;
	session->mmio = mmio;
	session->transport = transport;
	session->hardware_revision = hardware_revision;
	session->rf_type = rf_type;
	session->generation = generation_seed;
	session->nvm = *nvm;

	/* Stores the LTR choice as a flag. */
	if (ltr_enabled)
		session->ltr_enabled = 1U;
	else
		session->ltr_enabled = 0U;

	/* Succeeded: the session can run. */
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	return INTEL_AX211_RUNTIME_START_OK;
}

/*
 * Starts a fresh operational image and retains its live session ownership.
 */
int
drv_intel_ax211_runtime_start_run(
	struct intel_ax211_runtime_start *session)
{
	int result;

	/* Admits only an idle session that holds no DMA and no files. */
	if (session == NULL ||
	    session->state != INTEL_AX211_RUNTIME_START_STATE_IDLE ||
	    session->dma.device != NULL ||
	    session->files_loaded) {
		return INTEL_AX211_RUNTIME_START_INVALID;
	}

	/* Starts a new run in a new hardware epoch. */
	ax211_runtime_start_run_state_clear(session);
	session->state = INTEL_AX211_RUNTIME_START_STATE_STARTING;
	session->generation =
		ax211_runtime_start_next_generation(session->generation);

	/* Loads the files and builds the operational profile. */
	result = ax211_runtime_start_load_and_profile(session);
	if (result != INTEL_AX211_RUNTIME_START_OK) {
		result = ax211_runtime_start_fail(session, result);
		return result;
	}

	/* Copies the firmware and the rings into DMA memory. */
	result = ax211_runtime_start_prepare_dma(session);
	if (result != INTEL_AX211_RUNTIME_START_OK) {
		result = ax211_runtime_start_fail(session, result);
		return result;
	}

	/* Opens the controller's receive path for this epoch. */
	result = session->ops->boot.receive_epoch_begin(session->argument,
							session->generation);
	if (result != 0) {
		result = ax211_runtime_start_fail(
			session, INTEL_AX211_RUNTIME_START_IO);
		return result;
	}

	/* Starts the exact Gen3 transport. */
	result = ax211_runtime_start_device(session);
	if (result != INTEL_AX211_RUNTIME_START_OK) {
		result = ax211_runtime_start_fail(session, result);
		return result;
	}

	/* Accepts one ALIVE of this generation. */
	result = ax211_runtime_start_wait_notification(
		session, AX211_RUNTIME_START_NOTIFICATION_ALIVE,
		INTEL_AX211_RUNTIME_START_ALIVE_TIMEOUT_US);
	if (result != INTEL_AX211_RUNTIME_START_OK) {
		result = ax211_runtime_start_fail(session, result);
		return result;
	}

	/* Firmware images are retired only after exact ALIVE acceptance. */
	drv_intel_ax211_dma_release_boot_images(&session->dma);

	/* Publishes the PNVM sections of the board ALIVE named. */
	result = ax211_runtime_start_select_and_publish_pnvm(session);
	if (result != INTEL_AX211_RUNTIME_START_OK) {
		result = ax211_runtime_start_fail(session, result);
		return result;
	}

	/* Runs the operational initialization handshake. */
	result = ax211_runtime_start_init_firmware(session);
	if (result != INTEL_AX211_RUNTIME_START_OK) {
		result = ax211_runtime_start_fail(session, result);
		return result;
	}

	/* Sends the runtime commands. */
	result = ax211_runtime_start_commands(session);
	if (result != INTEL_AX211_RUNTIME_START_OK) {
		result = ax211_runtime_start_fail(session, result);
		return result;
	}

	/* Enables the interrupts of the running firmware. */
	result = drv_intel_ax211_transport_enable_runtime_interrupts(
		session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		result = ax211_runtime_start_fail(
			session, INTEL_AX211_RUNTIME_START_TRANSPORT);
		return result;
	}

	/* Succeeded: the firmware runs and the session keeps its ownership. */
	ax211_runtime_start_release_files(session);
	session->state = INTEL_AX211_RUNTIME_START_STATE_RUNNING;
	session->last_error = INTEL_AX211_RUNTIME_START_OK;
	return INTEL_AX211_RUNTIME_START_OK;
}

/*
 * Stops one live session across the checked IRQ/reset/DMA boundary.
 */
int
drv_intel_ax211_runtime_start_stop(
	struct intel_ax211_runtime_start *session)
{
	int result;

	/* Admits only a running session that holds its DMA. */
	if (session == NULL ||
	    session->state != INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    !session->dma_prepared) {
		return INTEL_AX211_RUNTIME_START_INVALID;
	}

	/* A stop that must be retried leaves the session as it is. */
	result = ax211_runtime_start_stop_and_release(session);
	if (result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED)
		return result;

	/* Reports a stop that ended with an error. */
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	session->last_error = (uint8_t)result;
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Succeeded: the device is stopped and the DMA released. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/*
 * Retries a stop which retained DMA because drain/reset was not proven.
 */
int
drv_intel_ax211_runtime_start_cleanup(
	struct intel_ax211_runtime_start *session)
{
	int result;

	/* Admits only a session waiting for a stop retry. */
	if (session == NULL ||
	    (session->state != INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED &&
	     session->state !=
		     INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA) ||
	    (!session->dma_prepared && !session->hardware_touched)) {
		return INTEL_AX211_RUNTIME_START_INVALID;
	}

	/* A stop that must be retried again leaves the session as it is. */
	result = ax211_runtime_start_stop_and_release(session);
	if (result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED)
		return result;

	/* Reports a stop that ended with an error. */
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	session->last_error = (uint8_t)result;
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Succeeded: the device is stopped and the DMA released. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/*
 * Copies the retained MCC response only from a live completed session.
 */
int
drv_intel_ax211_runtime_start_mcc(
	const struct intel_ax211_runtime_start *session,
	struct intel_ax211_runtime_mcc *mcc)
{
	/* Refuses a session that is not running or holds no MCC response. */
	if (session == NULL ||
	    mcc == NULL ||
	    session->state != INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    !session->mcc_valid) {
		return INTEL_AX211_RUNTIME_START_INVALID;
	}

	/* Succeeded: copies the response. */
	*mcc = session->mcc;
	return INTEL_AX211_RUNTIME_START_OK;
}

/*
 * Reports whether every controller-owned operation the session calls is
 * present: 1 when it is, 0 when not.
 */
static int
ax211_runtime_start_ops_valid(
	const struct intel_ax211_runtime_start_ops *ops)
{
	/* Rejects missing operations. */
	if (ops == NULL)
		return 0;

	/* Rejects operations without the receive path or the transport bind. */
	if (ops->boot.receive_epoch_begin == NULL ||
	    ops->boot.transport_bind == NULL ||
	    ops->boot.receive_event == NULL) {
		return 0;
	}

	/*
	 * Rejects operations without the PNVM doorbell or the post-ALIVE step.
	 */
	if (ops->boot.publish_pnvm == NULL || ops->boot.after_alive == NULL)
		return 0;

	/* Rejects operations without the interrupt drain or the clock. */
	if (ops->boot.interrupt_drain == NULL || ops->boot.clock_us == NULL)
		return 0;

	/* Rejects operations that cannot hold or release the NIC. */
	if (ops->wake_hold == NULL || ops->wake_release == NULL)
		return 0;

	/* Refuses ops which cannot end the DMA ownership after a reset. */
	if (ops->boot.bus_master_disable == NULL)
		return 0;

	/* The operations are complete. */
	return 1;
}

/* Clears one stopped run without discarding copied read-NVM inputs. */
static void
ax211_runtime_start_run_state_clear(
	struct intel_ax211_runtime_start *session)
{
	kern_memset(&session->files, 0, sizeof(session->files));
	kern_memset(&session->dma, 0, sizeof(session->dma));
	kern_memset(&session->commands, 0, sizeof(session->commands));
	kern_memset(&session->alive, 0, sizeof(session->alive));
	kern_memset(&session->profile, 0, sizeof(session->profile));
	kern_memset(&session->runtime, 0, sizeof(session->runtime));
	kern_memset(&session->mcc, 0, sizeof(session->mcc));
	kern_memset(session->event_bytes, 0, sizeof(session->event_bytes));
	kern_memset(session->response_bytes, 0, sizeof(session->response_bytes));
	session->files_loaded = 0U;
	session->dma_prepared = 0U;
	session->dma_exposed = 0U;
	session->hardware_touched = 0U;
	session->transport_bound = 0U;
	session->commands_initialized = 0U;
	session->alive_accepted = 0U;
	session->pnvm_accepted = 0U;
	session->init_accepted = 0U;
	session->profile_valid = 0U;
	session->mcc_valid = 0U;
	session->wake_held = 0U;
	session->last_error = INTEL_AX211_RUNTIME_START_OK;
}

/* Returns the hardware epoch after generation, skipping zero as invalid. */
static uint32_t
ax211_runtime_start_next_generation(
	uint32_t generation)
{
	/* Skips zero when the counter wraps. */
	generation++;
	if (generation == 0U)
		generation = 1U;

	/* Reports the next epoch. */
	return generation;
}

/* Reloads the exact artifact and builds a non-DQA operational profile. */
static int
ax211_runtime_start_load_and_profile(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_protocol_command_table table;
	size_t length;
	size_t offset;
	int result;

	/* Loads and marks ownership before inspecting any borrowed member. */
	result = drv_intel_ax211_firmware_files_load(&session->files);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_FIRMWARE;
	session->files_loaded = 1U;

	/* Requires both exact file buffers. */
	if (session->files.ucode_bytes == NULL ||
	    session->files.ucode_size == 0U ||
	    session->files.pnvm_bytes == NULL ||
	    session->files.pnvm_size == 0U) {
		return INTEL_AX211_RUNTIME_START_FIRMWARE;
	}

	/* Refuses a version record of another length. */
	offset = session->files.ucode_manifest.command_versions_offset;
	length = session->files.ucode_manifest.command_versions_length;
	if (length != sizeof(session->command_version_bytes))
		return INTEL_AX211_RUNTIME_START_FIRMWARE;

	/* Refuses a version record outside the image. */
	if (offset > session->files.ucode_size ||
	    length > session->files.ucode_size - offset) {
		return INTEL_AX211_RUNTIME_START_FIRMWARE;
	}

	/* Refuses an image whose versions differ from the first boot's. */
	result = kern_memcmp(session->files.ucode_bytes + offset,
			     session->command_version_bytes, length);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_FIRMWARE;

	/* Describes the copied bytes as a table. */
	result = drv_intel_ax211_protocol_command_table_parse(
		session->command_version_bytes,
		sizeof(session->command_version_bytes), &table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;

	/* Builds the profile from the image and the NVM. */
	result = drv_intel_ax211_runtime_profile_from_manifest(
		&session->files.ucode_manifest, &session->nvm,
		session->ltr_enabled, &session->profile);
	if (result != INTEL_AX211_RUNTIME_OK) {
		result = ax211_runtime_start_runtime_result(result);
		return result;
	}

	/* Succeeded: the profile is valid. */
	session->profile_valid = 1U;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Maps the runtime layer's outcomes to the session's results. */
static int
ax211_runtime_start_runtime_result(
	int result)
{
	/* A runtime success or completion is a session success. */
	if (result == INTEL_AX211_RUNTIME_OK ||
	    result == INTEL_AX211_RUNTIME_COMPLETE) {
		return INTEL_AX211_RUNTIME_START_OK;
	}

	/* A runtime timeout is a session timeout. */
	if (result == INTEL_AX211_RUNTIME_TIMEOUT)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;

	/* A repeat stays a repeat. */
	if (result == INTEL_AX211_RUNTIME_DUPLICATE)
		return INTEL_AX211_RUNTIME_START_DUPLICATE;

	/*
	 * A truncated, oversized or unsupported answer is a protocol failure.
	 */
	if (result == INTEL_AX211_RUNTIME_TRUNCATED ||
	    result == INTEL_AX211_RUNTIME_OVERSIZED ||
	    result == INTEL_AX211_RUNTIME_UNSUPPORTED) {
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}

	/* Any other outcome is a runtime failure. */
	return INTEL_AX211_RUNTIME_START_RUNTIME;
}

/* Preserves the primary error while enforcing the checked stop boundary. */
static int
ax211_runtime_start_fail(
	struct intel_ax211_runtime_start *session,
	int result)
{
	int cleanup_result;

	/*
	 * Releases immutable source files independently of device stop outcome.
	 */
	ax211_runtime_start_release_files(session);
	session->last_error = (uint8_t)result;

	/* A stop that must be retried is reported before anything else. */
	cleanup_result = ax211_runtime_start_stop_and_release(session);
	if (cleanup_result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED)
		return cleanup_result;
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;

	/* Reports the run's own failure. */
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Reports a failed cleanup. */
	if (cleanup_result != INTEL_AX211_RUNTIME_START_OK)
		return cleanup_result;

	/* Succeeded: the device is stopped and the DMA released. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Releases source-file ownership exactly once. */
static void
ax211_runtime_start_release_files(
	struct intel_ax211_runtime_start *session)
{
	/* Nothing to release when no files are held. */
	if (!session->files_loaded)
		return;

	/* Releases the files. */
	drv_intel_ax211_firmware_files_release(&session->files);
	session->files_loaded = 0U;
}

/* Proves reset before retiring command metadata or releasing any DMA. */
static int
ax211_runtime_start_stop_and_release(
	struct intel_ax211_runtime_start *session)
{
	uint32_t retired_generation;
	int command_result;
	int after_reset_result;
	int bus_master_result;
	int drain_result;
	int wake_release_result;
	int quiesce_result;
	int stop_result;

	/* Releases the NIC if the session still holds it. */
	wake_release_result = 0;
	if (session->wake_held) {
		/* Marks the NIC released only when the release succeeded. */
		wake_release_result = session->ops->wake_release(session->argument);
		if (wake_release_result == 0)
			session->wake_held = 0U;
	}

	/* A prior no-DMA failure still requires a checked controller reset. */
	if (!session->dma_prepared) {
		/* Nothing to stop when the hardware was not touched. */
		if (!session->hardware_touched) {
			/* Reports a failed NIC release. */
			if (wake_release_result != 0)
				return INTEL_AX211_RUNTIME_START_IO;

			/* Succeeded: there was nothing to stop. */
			return INTEL_AX211_RUNTIME_START_OK;
		}

		/* Stops the device; a failure must be retried. */
		stop_result = drv_intel_ax211_mmio_stop(session->mmio);
		if (stop_result != INTEL_AX211_MMIO_OK) {
			session->state =
				INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA;
			return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
		}

		/* The device is stopped; reports a failed NIC release. */
		session->hardware_touched = 0U;
		session->wake_held = 0U;
		if (wake_release_result != 0)
			return INTEL_AX211_RUNTIME_START_IO;

		/* Succeeded: the device is stopped. */
		return INTEL_AX211_RUNTIME_START_OK;
	}

	/*
	 * Never-exposed DMA is safe to free, but reset failure remains sticky.
	 */
	if (!session->dma_exposed && !session->transport_bound) {
		/* Stops a touched device and frees the DMA. */
		stop_result = INTEL_AX211_MMIO_OK;
		if (session->hardware_touched)
			stop_result = drv_intel_ax211_mmio_stop(session->mmio);
		drv_intel_ax211_dma_release(&session->dma);
		session->dma_prepared = 0U;

		/* A failed stop must be retried. */
		if (stop_result != INTEL_AX211_MMIO_OK) {
			session->state =
				INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA;
			return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
		}

		/* The device is stopped; reports a failed NIC release. */
		session->hardware_touched = 0U;
		session->wake_held = 0U;
		if (wake_release_result != 0)
			return INTEL_AX211_RUNTIME_START_IO;

		/* Succeeded: the device is stopped and the DMA released. */
		return INTEL_AX211_RUNTIME_START_OK;
	}

	/*
	 * Silences the interrupt handler and stops RX DMA on the device.  Bus
	 * mastering stays on here: the firmware may still be writing scan
	 * frames, and turning it off under that DMA hangs the integrated CNVi
	 * platform (BUG-158).
	 */
	drain_result = session->ops->boot.interrupt_drain(session->argument);
	quiesce_result = drv_intel_ax211_transport_quiesce(session->transport);

	/*
	 * Resets the device; this stop's master-disable indication is observed
	 * afresh.
	 */
	session->mmio->master_disable_timed_out = 0;
	stop_result = drv_intel_ax211_mmio_stop(session->mmio);
	if (drain_result != 0 || stop_result != INTEL_AX211_MMIO_OK) {
		session->state = INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED;

		/*
		 * Failed: the DMA and bus mastering are retained for the retry.
		 */
		return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
	}

	/*
	 * Without the master-disable indication, a DMA write already issued may
	 * still land after the reset.  The first such stop keeps every DMA page
	 * and bus mastering and asks for a retry; by the retry, after another
	 * reset, nothing can be in flight (BUG-158).
	 */
	if (session->mmio->master_disable_timed_out &&
	    !session->dma_release_deferred) {
		session->dma_release_deferred = 1U;
		session->state = INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED;

		/* Failed: the DMA is retained for the retry. */
		return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
	}

	/*
	 * Ends the device's DMA ownership now that the reset has stopped it.
	 * This is the barrier the DMA release below relies on.
	 */
	bus_master_result = session->ops->boot.bus_master_disable(
		session->argument);
	if (bus_master_result != 0) {
		session->state = INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED;

		/* Failed: the DMA is retained until bus mastering is off. */
		return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
	}

	/* This stop frees the DMA, deferred before or not. */
	session->dma_release_deferred = 0U;

	/*
	 * A command still outstanding when the stop began is not a failure of
	 * the stop: RX DMA went idle and the reset made every doorbell
	 * unobservable, so the command reset below retires it.  Treating it as
	 * a failure left the recovery stop reporting EIO (BUG-158).
	 */
	if (quiesce_result == INTEL_AX211_TRANSPORT_FAILED)
		quiesce_result = INTEL_AX211_TRANSPORT_OK;

	/* Retires the transport's command ownership. */
	after_reset_result =
		drv_intel_ax211_transport_command_after_device_reset(
			session->transport);
	command_result = INTEL_AX211_COMMAND_OK;

	/* Retires the command tokens once the transport is clean. */
	if (after_reset_result == INTEL_AX211_TRANSPORT_OK &&
	    session->commands_initialized) {
		retired_generation = ax211_runtime_start_next_generation(
			session->generation);
		command_result = drv_intel_ax211_command_after_device_reset(
			&session->commands, retired_generation);
	}

	/* Frees the DMA and forgets every ownership of the run. */
	drv_intel_ax211_dma_release(&session->dma);
	session->dma_prepared = 0U;
	session->dma_exposed = 0U;
	session->hardware_touched = 0U;
	session->transport_bound = 0U;
	session->commands_initialized = 0U;
	session->wake_held = 0U;

	/* Reports a transport that failed to quiesce or to clean up. */
	if (quiesce_result != INTEL_AX211_TRANSPORT_OK ||
	    after_reset_result != INTEL_AX211_TRANSPORT_OK) {
		return INTEL_AX211_RUNTIME_START_TRANSPORT;
	}

	/* Reports command tokens that failed to retire. */
	if (command_result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_RUNTIME_START_COMMAND;

	/* Reports a failed NIC release. */
	if (wake_release_result != 0)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Succeeded: the device is stopped and the DMA released. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Copies firmware and all runtime rings into controller-owned DMA. */
static int
ax211_runtime_start_prepare_dma(
	struct intel_ax211_runtime_start *session)
{
	int result;

	/* Allocates and fills the DMA memory of the run. */
	result = drv_intel_ax211_dma_prepare_boot(
		session->dma_device, session->files.ucode_bytes,
		session->files.ucode_size, &session->files.ucode_manifest,
		session->hardware_revision, &session->dma);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_DMA;

	/* Succeeded: the DMA memory is prepared. */
	session->dma_prepared = 1U;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Starts the exact Gen3 transport for this operational epoch. */
static int
ax211_runtime_start_device(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_mmio_boot mmio_boot;
	size_t index;
	int result;

	/* Executes the checked card-ready, reset, and APM sequence. */
	session->hardware_touched = 1U;
	result = drv_intel_ax211_mmio_take_ownership(session->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;

	/* Resets the device. */
	result = drv_intel_ax211_mmio_sw_reset(session->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;

	/* Powers the MAC clock. */
	result = drv_intel_ax211_mmio_apm_init(session->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;

	/* Binds the sole controller transport before exposing ring storage. */
	result = session->ops->boot.transport_bind(
		session->argument, &session->dma, session->mmio,
		session->transport, session->generation);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;
	session->transport_bound = 1U;

	/* Routes the interrupts. */
	result = drv_intel_ax211_transport_configure_msix(session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;

	/* Treats partial ring publication as device DMA ownership. */
	session->dma_exposed = 1U;
	result = drv_intel_ax211_transport_initialize_rings(session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;

	/* Refuses DMA memory without every receive buffer. */
	if (session->dma.rx_buffer_count != INTEL_AX211_RX_RING_SIZE)
		return INTEL_AX211_RUNTIME_START_DMA;

	/* Publishes every exact RX buffer before firmware can configure RFH. */
	for (index = 0U; index < session->dma.rx_buffer_count; index++) {
		/* Refuses a receive buffer without an address. */
		if (session->dma.rx_buffer[index].address == NULL ||
		    session->dma.rx_buffer[index].device_address == 0U) {
			return INTEL_AX211_RUNTIME_START_DMA;
		}

		/* Publishes the buffer's descriptor. */
		result = drv_intel_ax211_transport_publish_rx_descriptor(
			session->transport, (uint16_t)index,
			session->dma.rx_buffer[index].device_address);
		if (result != INTEL_AX211_TRANSPORT_OK)
			return INTEL_AX211_RUNTIME_START_TRANSPORT;
	}

	/*
	 * Enables the firmware's interrupts; the first RX credit waits for the
	 * hardware-ALIVE cause.
	 */
	result = drv_intel_ax211_transport_enable_firmware_interrupts(
		session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;

	/*
	 * Publishes context and IML only after rings and interrupts are ready.
	 */
	if (session->dma.context.device_address == 0U ||
	    session->dma.iml.device_address == 0U ||
	    session->dma.iml.size != INTEL_AX211_MMIO_LOADER_IMAGE_SIZE) {
		return INTEL_AX211_RUNTIME_START_DMA;
	}

	/* Describes the boot context and the loader to the MMIO layer. */
	kern_memset(&mmio_boot, 0, sizeof(mmio_boot));
	mmio_boot.context_address = session->dma.context.device_address;
	mmio_boot.iml_address = session->dma.iml.device_address;
	mmio_boot.iml_size = (uint32_t)session->dma.iml.size;

	/* Starts the firmware. */
	result = drv_intel_ax211_mmio_publish_gen3(session->mmio, &mmio_boot);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;

	/* Succeeded: the firmware boots. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Waits finitely for one expected notification in this hardware epoch. */
static int
ax211_runtime_start_wait_notification(
	struct intel_ax211_runtime_start *session,
	enum ax211_runtime_start_notification expected,
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
	result =
		ax211_runtime_start_deadline(session, timeout, &now, &deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/*
	 * Discards stale generations but never extends the original deadline.
	 */
	for (index = 0U; index < INTEL_AX211_RUNTIME_START_EVENT_LIMIT;
	     index++) {
		/* Receives the next event. */
		result = ax211_runtime_start_receive(session, deadline, &event,
						     &message);
		if (result != INTEL_AX211_RUNTIME_START_OK)
			return result;

		/* Skips an event of an earlier generation. */
		if (message.generation != session->generation)
			continue;

		/* Refuses a command answer while a notification is awaited. */
		if ((event.queue & 0x80U) == 0U)
			return INTEL_AX211_RUNTIME_START_PROTOCOL;

		/* Refuses a repeat of a notification already accepted. */
		kind = ax211_runtime_start_notification_kind(&message);
		duplicate =
			ax211_runtime_start_notification_duplicate(session, kind);
		if (duplicate)
			return INTEL_AX211_RUNTIME_START_DUPLICATE;

		/* Skips a notification that is not part of the start order. */
		if (kind == 0)
			continue;

		/* Refuses a start notification out of order. */
		if (kind != (int)expected)
			return INTEL_AX211_RUNTIME_START_PROTOCOL;

		/* Validates and records the awaited notification. */
		result = ax211_runtime_start_notification_accept(
			session, kind, &message);
		return result;
	}

	/* The event budget ran out. */
	return INTEL_AX211_RUNTIME_START_TIMEOUT;
}

/* Reads one monotonic start time and computes a checked absolute deadline. */
static int
ax211_runtime_start_deadline(
	struct intel_ax211_runtime_start *session,
	uint64_t timeout,
	uint64_t *now,
	uint64_t *deadline)
{
	int result;

	/* Refuses a zero timeout and missing outputs. */
	if (timeout == 0U || now == NULL || deadline == NULL)
		return INTEL_AX211_RUNTIME_START_INVALID;

	/* Reads the clock. */
	result = session->ops->boot.clock_us(session->argument, now);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Refuses a deadline that overflows the clock. */
	if (*now > UINT64_MAX - timeout)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Succeeded: reports the deadline. */
	*deadline = *now + timeout;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Receives and decodes one bounded event without assigning ownership. */
static int
ax211_runtime_start_receive(
	struct intel_ax211_runtime_start *session,
	uint64_t deadline,
	struct intel_ax211_event *event,
	struct intel_ax211_protocol_message *message)
{
	struct intel_ax211_boot_received_event received;
	uint64_t now;
	int result;

	/* Lets the controller fill only the session-owned fixed buffer. */
	kern_memset(&received, 0, sizeof(received));
	result = session->ops->boot.receive_event(
		session->argument, deadline, session->event_bytes,
		sizeof(session->event_bytes), &received);
	if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;
	if (result != INTEL_AX211_BOOT_RECEIVE_OK)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Refuses an empty or oversized event and an unset generation. */
	if (received.length == 0U ||
	    received.length > sizeof(session->event_bytes) ||
	    received.generation == 0U) {
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}

	/*
	 * Rejects a receiver which returned after the caller's fixed deadline.
	 */
	result = session->ops->boot.clock_us(session->argument, &now);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	if (now > deadline)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;

	/* Decodes the common RX envelope before attaching private metadata. */
	result = drv_intel_ax211_event_decode(session->event_bytes,
					      received.length, event);
	if (result != INTEL_AX211_SUCCESS)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;

	/* Refuses a payload that does not end where the event ends. */
	if (event->payload_offset > received.length ||
	    event->payload_length != received.length - event->payload_offset) {
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
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
	message->payload = session->event_bytes + event->payload_offset;
	message->payload_length = event->payload_length;

	/* Succeeded: the event is decoded. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Classifies only notifications which participate in the start order. */
static int
ax211_runtime_start_notification_kind(
	const struct intel_ax211_protocol_message *message)
{
	/* The ALIVE notification. */
	if (message->group == INTEL_AX211_PROTOCOL_FAMILY_CLASSIC &&
	    message->opcode == INTEL_AX211_PROTOCOL_ALIVE_OPCODE) {
		return AX211_RUNTIME_START_NOTIFICATION_ALIVE;
	}

	/* The PNVM-accepted notification. */
	if (message->group == INTEL_AX211_PROTOCOL_FAMILY_REGULATORY_STORE &&
	    message->opcode == INTEL_AX211_PROTOCOL_PNVM_ACCEPTED_OPCODE) {
		return AX211_RUNTIME_START_NOTIFICATION_PNVM;
	}

	/* The init-finished notification. */
	if (message->group == INTEL_AX211_PROTOCOL_FAMILY_CLASSIC &&
	    message->opcode == INTEL_AX211_PROTOCOL_INIT_FINISHED_OPCODE) {
		return AX211_RUNTIME_START_NOTIFICATION_INIT;
	}

	/* Any other notification is not part of the start order. */
	return 0;
}

/*
 * Reports whether a notification was already accepted in the current
 * generation: 1 when it was, 0 when not.
 */
static int
ax211_runtime_start_notification_duplicate(
	const struct intel_ax211_runtime_start *session,
	int kind)
{
	/* A second ALIVE. */
	if (kind == AX211_RUNTIME_START_NOTIFICATION_ALIVE &&
	    session->alive_accepted) {
		return 1;
	}

	/* A second PNVM-accepted notification. */
	if (kind == AX211_RUNTIME_START_NOTIFICATION_PNVM &&
	    session->pnvm_accepted) {
		return 1;
	}

	/* A second init-finished notification. */
	if (kind == AX211_RUNTIME_START_NOTIFICATION_INIT &&
	    session->init_accepted) {
		return 1;
	}

	/* The notification is new. */
	return 0;
}

/* Validates and records one expected notification transactionally. */
static int
ax211_runtime_start_notification_accept(
	struct intel_ax211_runtime_start *session,
	int kind,
	const struct intel_ax211_protocol_message *message)
{
	int result;

	/* Validates the notification by its kind and records a valid one. */
	if (kind == AX211_RUNTIME_START_NOTIFICATION_ALIVE) {
		/* Decodes ALIVE into the session. */
		result = drv_intel_ax211_protocol_alive_decode(
			message, session->generation, &session->alive);
		if (result == INTEL_AX211_PROTOCOL_OK)
			session->alive_accepted = 1U;
	} else if (kind == AX211_RUNTIME_START_NOTIFICATION_PNVM) {
		/* Validates the PNVM-accepted notification. */
		result = drv_intel_ax211_protocol_pnvm_accepted(
			message, session->generation);
		if (result == INTEL_AX211_PROTOCOL_OK)
			session->pnvm_accepted = 1U;
	} else if (kind == AX211_RUNTIME_START_NOTIFICATION_INIT) {
		/* Validates the init-finished notification. */
		result = drv_intel_ax211_init_finished_validate(
			message, session->generation);
		if (result == INTEL_AX211_PROTOCOL_OK)
			session->init_accepted = 1U;
	} else {
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}

	/* Reports a rejected notification. */
	result = ax211_runtime_start_protocol_result(result);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Succeeded: the notification is recorded. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Maps all exact protocol rejections to the session's results. */
static int
ax211_runtime_start_protocol_result(
	int result)
{
	/* A protocol success is a session success. */
	if (result == INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_OK;

	/* A repeat stays a repeat. */
	if (result == INTEL_AX211_PROTOCOL_DUPLICATE)
		return INTEL_AX211_RUNTIME_START_DUPLICATE;

	/* Any other rejection is a protocol failure. */
	return INTEL_AX211_RUNTIME_START_PROTOCOL;
}

/* Selects the accepted-ALIVE SKU and publishes copied PNVM segments. */
static int
ax211_runtime_start_select_and_publish_pnvm(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_pnvm_manifest manifest;
	struct intel_ax211_board_signature sku;
	int result;

	/* Selects the platform image the reported SKU calls for. */
	kern_memset(&sku, 0, sizeof(sku));
	sku.data[0] = session->alive.sku[0];
	sku.data[1] = session->alive.sku[1];
	sku.data[2] = session->alive.sku[2];
	result = drv_intel_ax211_pnvm_select(
		session->files.pnvm_bytes, session->files.pnvm_size, &sku,
		session->mmio->profile.mac_type, session->rf_type, &manifest);
	if (result != INTEL_AX211_SUCCESS)
		return INTEL_AX211_RUNTIME_START_FIRMWARE;

	/* Copies every selected segment before releasing both source files. */
	result = drv_intel_ax211_dma_prepare_pnvm(session->files.pnvm_bytes,
						  session->files.pnvm_size,
						  &manifest, &session->dma);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_DMA;
	ax211_runtime_start_release_files(session);

	/* Synchronizes and rings the controller-owned PNVM doorbell. */
	result = session->ops->boot.publish_pnvm(session->argument,
						 &session->dma);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Waits for the firmware to accept the sections. */
	result = ax211_runtime_start_wait_notification(
		session, AX211_RUNTIME_START_NOTIFICATION_PNVM,
		INTEL_AX211_RUNTIME_START_PNVM_TIMEOUT_US);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Applies the transport's exact post-ALIVE hardware transition. */
	result = session->ops->boot.after_alive(session->argument,
					       &session->alive);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Succeeded: the firmware has its radio tuning data. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Performs the operational unified-firmware init/NVM handshake. */
static int
ax211_runtime_start_init_firmware(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_protocol_command_table table;
	int result;

	/* Binds one-command-at-a-time transactions to this hardware epoch. */
	result = drv_intel_ax211_command_transaction_init(
		&session->commands, session->transport, 1U,
		session->generation);
	if (result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_RUNTIME_START_COMMAND;
	session->commands_initialized = 1U;

	/* Selects the initialization stage. */
	result = ax211_runtime_start_send_stage_select(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Hands the NVM over. */
	result = ax211_runtime_start_send_nvm_handoff(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Waits for the end of the initialization stage. */
	result = ax211_runtime_start_wait_notification(
		session, AX211_RUNTIME_START_NOTIFICATION_INIT,
		INTEL_AX211_RUNTIME_START_INIT_TIMEOUT_US);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/*
	 * DQA absence and every runtime layout are revalidated after init.
	 * Describes the copied bytes as a table first.
	 */
	result = drv_intel_ax211_protocol_command_table_parse(
		session->command_version_bytes,
		sizeof(session->command_version_bytes), &table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;

	/* Requires the versions of the runtime commands. */
	result = drv_intel_ax211_runtime_api89_validate(&table,
							&session->profile);
	if (result != INTEL_AX211_RUNTIME_OK) {
		result = ax211_runtime_start_runtime_result(result);
		return result;
	}

	/* Succeeded: the firmware is initialized. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Sends the stage-select command and checks its one-word status. */
static int
ax211_runtime_start_send_stage_select(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	uint8_t payload[INTEL_AX211_INIT_STAGE_SELECT_SIZE];
	uint8_t response[INTEL_AX211_RUNTIME_START_GENERIC_RESPONSE_SIZE];
	uint64_t deadline;
	size_t response_length;
	uint32_t status;
	int result;

	/* Encodes the exact read-NVM profile. */
	result = drv_intel_ax211_init_stage_select_encode(
		INTEL_AX211_INIT_PROFILE_STORE_READ, payload);
	if (result != INTEL_AX211_PROTOCOL_OK) {
		result = ax211_runtime_start_protocol_result(result);
		return result;
	}

	/* Describes the command and its one-word answer. */
	kern_memset(&request, 0, sizeof(request));
	request.command.opcode = INTEL_AX211_INIT_STAGE_SELECT_OPCODE;
	request.command.group = INTEL_AX211_INIT_FAMILY_PLATFORM;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = sizeof(response);
	request.maximum_response_length = sizeof(response);

	/* Submits the command. */
	result = ax211_runtime_start_submit(session, &request, &handle,
					    &deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Waits for the one-word answer. */
	response_length = 0U;
	kern_memset(response, 0, sizeof(response));
	result = ax211_runtime_start_wait_command(session, deadline, response,
						  sizeof(response),
						  &response_length);
	if (result == INTEL_AX211_RUNTIME_START_OK &&
	    response_length != sizeof(response))
		result = INTEL_AX211_RUNTIME_START_PROTOCOL;

	/* A nonzero status word means the firmware refused the profile. */
	if (result == INTEL_AX211_RUNTIME_START_OK) {
		/* Reads the status word. */
		status = ax211_runtime_start_get_le32(response);
		if (status != 0U)
			result = INTEL_AX211_RUNTIME_START_COMMAND;
	}

	/* Clears the answer and reports the outcome. */
	kern_memset(response, 0, sizeof(response));
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Succeeded: the firmware runs the selected stage. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Submits one command under a fresh one-command deadline. */
static int
ax211_runtime_start_submit(
	struct intel_ax211_runtime_start *session,
	const struct intel_ax211_command_request *request,
	struct intel_ax211_command_handle *handle,
	uint64_t *deadline)
{
	uint64_t now;
	int result;

	/* Fixes one finite answer deadline. */
	result = ax211_runtime_start_deadline(
		session, INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, &now,
		deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Submits the command. */
	result = drv_intel_ax211_command_submit(
		&session->commands, request, now,
		INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, handle);
	if (result != INTEL_AX211_COMMAND_OK) {
		result = ax211_runtime_start_command_result(result);
		return result;
	}

	/* Succeeded: the command is outstanding. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Maps command transaction outcomes without weakening token validation. */
static int
ax211_runtime_start_command_result(
	int result)
{
	/* A command success is a session success. */
	if (result == INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_RUNTIME_START_OK;

	/* A command timeout is a session timeout. */
	if (result == INTEL_AX211_COMMAND_TIMEOUT)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;

	/* A repeat stays a repeat. */
	if (result == INTEL_AX211_COMMAND_DUPLICATE)
		return INTEL_AX211_RUNTIME_START_DUPLICATE;

	/*
	 * A malformed, failed, mismatched, misplaced or oversized answer is a
	 * protocol failure.
	 */
	if (result == INTEL_AX211_COMMAND_MALFORMED ||
	    result == INTEL_AX211_COMMAND_FIRMWARE_FAILED ||
	    result == INTEL_AX211_COMMAND_VERSION_MISMATCH ||
	    result == INTEL_AX211_COMMAND_OUT_OF_ORDER ||
	    result == INTEL_AX211_COMMAND_BUFFER_TOO_SMALL) {
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}

	/* Any other outcome is a command failure. */
	return INTEL_AX211_RUNTIME_START_COMMAND;
}

/* Waits for one exact oldest command without extending its deadline. */
static int
ax211_runtime_start_wait_command(
	struct intel_ax211_runtime_start *session,
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
	for (index = 0U; index < INTEL_AX211_RUNTIME_START_EVENT_LIMIT;
	     index++) {
		/* Receives the next event. */
		result = ax211_runtime_start_receive(session, deadline, &event,
						     &message);
		if (result != INTEL_AX211_RUNTIME_START_OK)
			return result;

		/* Skips an event of an earlier generation. */
		if (message.generation != session->generation)
			continue;

		/*
		 * Rejects repeated accepted notifications in this generation.
		 */
		if ((event.queue & 0x80U) != 0U) {
			/*
			 * Refuses a repeat of a notification already accepted.
			 */
			kind = ax211_runtime_start_notification_kind(&message);
			duplicate = ax211_runtime_start_notification_duplicate(
				session, kind);
			if (duplicate)
				return INTEL_AX211_RUNTIME_START_DUPLICATE;

			/*
			 * Refuses a start notification while an answer is
			 * awaited.
			 */
			if (kind != 0)
				return INTEL_AX211_RUNTIME_START_PROTOCOL;

			/* Skips any other notification. */
			continue;
		}

		/*
		 * Lets the command layer validate token, version, size, and
		 * flags.
		 */
		result = drv_intel_ax211_command_complete(
			&session->commands, session->event_bytes,
			message.payload_length + INTEL_AX211_EVENT_HEADER_SIZE,
			session->generation, response, response_capacity,
			response_length);

		/* Reports the answer's outcome. */
		result = ax211_runtime_start_command_result(result);
		return result;
	}

	/* Poisons the outstanding token once the local event budget expires. */
	result = session->ops->boot.clock_us(session->argument, &now);
	if (result == 0 && now < deadline)
		now = deadline;

	/* Times out the command; any other outcome is a command failure. */
	result = drv_intel_ax211_command_timeout_oldest(&session->commands, now,
							&timed_out);
	if (result != INTEL_AX211_COMMAND_TIMEOUT &&
	    result != INTEL_AX211_COMMAND_POISONED) {
		return INTEL_AX211_RUNTIME_START_COMMAND;
	}

	/* Reports the timeout. */
	return INTEL_AX211_RUNTIME_START_TIMEOUT;
}

/* Decodes one exact little-endian generic firmware status. */
static uint32_t
ax211_runtime_start_get_le32(
	const uint8_t *bytes)
{
	/* Assembles the word from its four bytes, least significant first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Sends the NVM handoff and waits for its empty acknowledgement. */
static int
ax211_runtime_start_send_nvm_handoff(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_command_handle handle;
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	int result;

	/* Fixes one finite acknowledgement deadline. */
	result = ax211_runtime_start_deadline(
		session, INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, &now,
		&deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Submits the request for a zero-payload acknowledgement. */
	result = drv_intel_ax211_command_submit_nvm_handoff(
		&session->commands, now,
		INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, &handle);
	if (result != INTEL_AX211_COMMAND_OK) {
		result = ax211_runtime_start_command_result(result);
		return result;
	}

	/* Waits for the acknowledgement. */
	response_length = 0U;
	result = ax211_runtime_start_wait_command(session, deadline, NULL, 0U,
						  &response_length);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Refuses an acknowledgement with a payload. */
	if (response_length != 0U)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;

	/* Succeeded. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Owns the NIC across the complete ordered runtime command transaction. */
static int
ax211_runtime_start_commands(
	struct intel_ax211_runtime_start *session)
{
	int result;
	int unlock_result;

	/* Holds the NIC awake. */
	result = session->ops->wake_hold(session->argument);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	session->wake_held = 1U;

	/* Sends the commands. */
	result = ax211_runtime_start_commands_locked(session);

	/* Releases the NIC, which a later stop retries if this fails. */
	unlock_result = session->ops->wake_release(session->argument);
	if (unlock_result == 0)
		session->wake_held = 0U;

	/* Reports a failed command sequence first. */
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Reports a failed NIC release. */
	if (unlock_result != 0)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Succeeded: every runtime command was answered. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Sends every runtime.c step serially under its own one-second deadline. */
static int
ax211_runtime_start_commands_locked(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_runtime_command command;
	uint64_t now;
	int result;

	/* Describes the copied bytes as a table. */
	result = drv_intel_ax211_protocol_command_table_parse(
		session->command_version_bytes,
		sizeof(session->command_version_bytes), &table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;

	/* Reads the clock. */
	result = session->ops->boot.clock_us(session->argument, &now);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;

	/* Begins the runtime command sequence. */
	result = drv_intel_ax211_runtime_begin(&session->runtime, &table,
					       &session->profile,
					       session->generation, now);
	if (result != INTEL_AX211_RUNTIME_OK) {
		result = ax211_runtime_start_runtime_result(result);
		return result;
	}

	/* Sends one step at a time until the sequence ends. */
	while (session->runtime.active && !session->runtime.terminal) {
		/* Reads the clock. */
		result = session->ops->boot.clock_us(session->argument, &now);
		if (result != 0)
			return INTEL_AX211_RUNTIME_START_IO;

		/* Builds the current step's command. */
		result = drv_intel_ax211_runtime_current(&session->runtime, now,
							 &command);
		if (result != INTEL_AX211_RUNTIME_OK) {
			result = ax211_runtime_start_runtime_result(result);
			return result;
		}

		/* Sends the command and waits for its answer. */
		result = ax211_runtime_start_send_step(
			session, session->runtime.step, &command);
		if (result != INTEL_AX211_RUNTIME_START_OK)
			return result;

		/* Reads the clock. */
		result = session->ops->boot.clock_us(session->argument, &now);
		if (result != 0)
			return INTEL_AX211_RUNTIME_START_IO;

		/* Records the answer; the last one ends the sequence. */
		result = drv_intel_ax211_runtime_ack(
			&session->runtime, session->generation,
			session->runtime.step, now);
		if (result == INTEL_AX211_RUNTIME_COMPLETE)
			break;
		if (result != INTEL_AX211_RUNTIME_OK) {
			result = ax211_runtime_start_runtime_result(result);
			return result;
		}
	}

	/* Refuses a sequence that did not reach its end. */
	if (!session->runtime.terminal ||
	    session->runtime.active ||
	    session->runtime.step != INTEL_AX211_RUNTIME_STEP_DONE) {
		return INTEL_AX211_RUNTIME_START_RUNTIME;
	}

	/* Refuses firmware regulation without a country answer. */
	if (session->profile.firmware_regulatory && !session->mcc_valid)
		return INTEL_AX211_RUNTIME_START_RUNTIME;

	/* Succeeded: every step was answered. */
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Sends one runtime step and decodes the country answer it may carry. */
static int
ax211_runtime_start_send_step(
	struct intel_ax211_runtime_start *session,
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_command *command)
{
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	uint8_t *response;
	uint64_t deadline;
	size_t response_capacity;
	size_t response_length;
	int result;

	/*
	 * Refuses a missing command, a wire version, and an oversized payload.
	 */
	if (command == NULL ||
	    command->wire_version != 0U ||
	    command->payload_length > sizeof(command->payload)) {
		return INTEL_AX211_RUNTIME_START_RUNTIME;
	}

	/* Describes the command. */
	kern_memset(&request, 0, sizeof(request));
	request.command.group = command->group;
	request.command.opcode = command->opcode;
	request.command.version = command->wire_version;
	request.payload = command->payload;
	request.payload_length = command->payload_length;
	request.response_version = command->response_version;

	/*
	 * Only the country update has an answer; the others are acknowledged.
	 */
	if (step == INTEL_AX211_RUNTIME_STEP_COUNTRY_UPDATE) {
		request.minimum_response_length =
			INTEL_AX211_RUNTIME_START_MCC_RESPONSE_MIN;
		request.maximum_response_length =
			INTEL_AX211_RUNTIME_START_MCC_RESPONSE_MAX;
		response_capacity = sizeof(session->response_bytes);
	} else {
		request.minimum_response_length = 0U;
		request.maximum_response_length = 0U;
		response_capacity = 0U;
	}

	/* Submits the command. */
	result = ax211_runtime_start_submit(session, &request, &handle,
					    &deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* Gives the answer a buffer only when one is expected. */
	if (response_capacity == 0U)
		response = NULL;
	else
		response = session->response_bytes;

	/* Waits for the answer. */
	response_length = 0U;
	result = ax211_runtime_start_wait_command(session, deadline, response,
						  response_capacity,
						  &response_length);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* An acknowledged step ends here. */
	if (step != INTEL_AX211_RUNTIME_STEP_COUNTRY_UPDATE) {
		/* Refuses an acknowledgement with a payload. */
		if (response_length != 0U)
			return INTEL_AX211_RUNTIME_START_PROTOCOL;

		/* Succeeded: the step was acknowledged. */
		return INTEL_AX211_RUNTIME_START_OK;
	}

	/* Rebuilds the response as the message the decoder expects. */
	kern_memset(&message, 0, sizeof(message));
	message.group = command->group;
	message.opcode = command->opcode;
	message.version = command->response_version;
	message.queue = handle.token.queue;
	message.index = handle.token.index;
	message.generation = session->generation;
	message.payload = session->response_bytes;
	message.payload_length = response_length;

	/* Decodes the country answer and clears its buffer. */
	result = drv_intel_ax211_runtime_mcc_decode(
		&message, session->generation, &session->mcc);
	kern_memset(session->response_bytes, 0, sizeof(session->response_bytes));
	if (result != INTEL_AX211_RUNTIME_OK) {
		result = ax211_runtime_start_runtime_result(result);
		return result;
	}

	/* Succeeded: the country answer is held. */
	session->mcc_valid = 1U;
	return INTEL_AX211_RUNTIME_START_OK;
}
