/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private operational firmware coordinator
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "intel-ax211-runtime-start.h"

#include <string.h>

enum ax211_runtime_start_notification {
	AX211_RUNTIME_START_NOTIFICATION_ALIVE = 1,
	AX211_RUNTIME_START_NOTIFICATION_PNVM = 2,
	AX211_RUNTIME_START_NOTIFICATION_INIT = 3
};

static int ax211_runtime_start_ops_valid(
	const struct intel_ax211_runtime_start_ops *ops);
static void ax211_runtime_start_run_state_clear(
	struct intel_ax211_runtime_start *session);
static uint32_t ax211_runtime_start_next_generation(uint32_t generation);
static int ax211_runtime_start_load_and_profile(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_prepare_dma(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_device(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_select_and_publish_pnvm(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_init_firmware(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_send_extended_cfg(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_send_nvm_access_complete(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_commands(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_commands_locked(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_send_step(
	struct intel_ax211_runtime_start *session,
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_command *command);
static int ax211_runtime_start_submit(
	struct intel_ax211_runtime_start *session,
	const struct intel_ax211_command_request *request,
	struct intel_ax211_command_handle *handle, uint64_t *deadline);
static int ax211_runtime_start_wait_command(
	struct intel_ax211_runtime_start *session, uint64_t deadline,
	uint8_t *response, size_t response_capacity,
	size_t *response_length);
static int ax211_runtime_start_wait_notification(
	struct intel_ax211_runtime_start *session,
	enum ax211_runtime_start_notification expected, uint64_t timeout);
static int ax211_runtime_start_receive(
	struct intel_ax211_runtime_start *session, uint64_t deadline,
	struct intel_ax211_event *event,
	struct intel_ax211_protocol_message *message);
static int ax211_runtime_start_deadline(
	struct intel_ax211_runtime_start *session, uint64_t timeout,
	uint64_t *now, uint64_t *deadline);
static int ax211_runtime_start_notification_kind(
	const struct intel_ax211_protocol_message *message);
static int ax211_runtime_start_notification_duplicate(
	const struct intel_ax211_runtime_start *session, int kind);
static int ax211_runtime_start_notification_accept(
	struct intel_ax211_runtime_start *session, int kind,
	const struct intel_ax211_protocol_message *message);
static int ax211_runtime_start_stop_and_release(
	struct intel_ax211_runtime_start *session);
static void ax211_runtime_start_release_files(
	struct intel_ax211_runtime_start *session);
static int ax211_runtime_start_fail(
	struct intel_ax211_runtime_start *session, int result);
static int ax211_runtime_start_protocol_result(int result);
static int ax211_runtime_start_command_result(int result);
static int ax211_runtime_start_runtime_result(int result);
static uint32_t ax211_runtime_start_get_le32(const uint8_t *bytes);

/* Copies the read-NVM outputs into one idle operational session. */
int
intel_ax211_runtime_start_init(
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
	int result;

	if (session == NULL || !ax211_runtime_start_ops_valid(ops) ||
	    dma_device == NULL || mmio == NULL || transport == NULL ||
	    command_table == NULL || command_table->bytes == NULL || nvm == NULL ||
	    command_table->count != INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT ||
	    (ltr_enabled != 0 && ltr_enabled != 1) || generation_seed == 0U)
		return INTEL_AX211_RUNTIME_START_INVALID;
	mac_type = (uint16_t)((hardware_revision & 0xfff0U) >> 4);
	if (!intel_ax211_mac_type_supported(mac_type) ||
	    mmio->profile.mac_type != mac_type ||
	    mmio->profile.rf_type != rf_type)
		return INTEL_AX211_RUNTIME_START_INVALID;

	/* Owns the exact table instead of retaining the boot coordinator's view. */
	memset(session, 0, sizeof(*session));
	memcpy(session->command_version_bytes, command_table->bytes,
	    sizeof(session->command_version_bytes));
	result = intel_ax211_protocol_command_table_parse(
	    session->command_version_bytes,
	    sizeof(session->command_version_bytes), &copied_table);
	if (result != INTEL_AX211_PROTOCOL_OK ||
	    intel_ax211_init_api89_validate(&copied_table) !=
	    INTEL_AX211_PROTOCOL_OK) {
		memset(session, 0, sizeof(*session));
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}

	session->ops = ops;
	session->argument = argument;
	session->dma_device = dma_device;
	session->mmio = mmio;
	session->transport = transport;
	session->hardware_revision = hardware_revision;
	session->rf_type = rf_type;
	session->generation = generation_seed;
	session->nvm = *nvm;
	session->ltr_enabled = ltr_enabled ? 1U : 0U;
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Starts a fresh operational image and retains its live session ownership. */
int
intel_ax211_runtime_start_run(
	struct intel_ax211_runtime_start *session)
{
	int result;

	if (session == NULL ||
	    session->state != INTEL_AX211_RUNTIME_START_STATE_IDLE ||
	    session->dma.device != NULL || session->files_loaded)
		return INTEL_AX211_RUNTIME_START_INVALID;
	ax211_runtime_start_run_state_clear(session);
	session->state = INTEL_AX211_RUNTIME_START_STATE_STARTING;
	session->generation = ax211_runtime_start_next_generation(
	    session->generation);

	result = ax211_runtime_start_load_and_profile(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return ax211_runtime_start_fail(session, result);
	result = ax211_runtime_start_prepare_dma(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return ax211_runtime_start_fail(session, result);
	result = session->ops->boot.receive_epoch_begin(session->argument,
	    session->generation);
	if (result != 0)
		return ax211_runtime_start_fail(session,
		    INTEL_AX211_RUNTIME_START_IO);

	result = ax211_runtime_start_device(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return ax211_runtime_start_fail(session, result);
	result = ax211_runtime_start_wait_notification(session,
	    AX211_RUNTIME_START_NOTIFICATION_ALIVE,
	    INTEL_AX211_RUNTIME_START_ALIVE_TIMEOUT_US);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return ax211_runtime_start_fail(session, result);

	/* Firmware images are retired only after exact ALIVE acceptance. */
	intel_ax211_dma_release_boot_images(&session->dma);
	result = ax211_runtime_start_select_and_publish_pnvm(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return ax211_runtime_start_fail(session, result);
	result = ax211_runtime_start_init_firmware(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return ax211_runtime_start_fail(session, result);
	result = ax211_runtime_start_commands(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return ax211_runtime_start_fail(session, result);

	result = intel_ax211_transport_enable_runtime_interrupts(
	    session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return ax211_runtime_start_fail(session,
		    INTEL_AX211_RUNTIME_START_TRANSPORT);
	ax211_runtime_start_release_files(session);
	session->state = INTEL_AX211_RUNTIME_START_STATE_RUNNING;
	session->last_error = INTEL_AX211_RUNTIME_START_OK;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Stops one live session across the checked IRQ/reset/DMA boundary. */
int
intel_ax211_runtime_start_stop(
	struct intel_ax211_runtime_start *session)
{
	int result;

	if (session == NULL ||
	    session->state != INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    !session->dma_prepared)
		return INTEL_AX211_RUNTIME_START_INVALID;
	result = ax211_runtime_start_stop_and_release(session);
	if (result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED)
		return result;
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	session->last_error = (uint8_t)result;
	return result;
}

/* Retries a stop which retained DMA because drain/reset was not proven. */
int
intel_ax211_runtime_start_cleanup(
	struct intel_ax211_runtime_start *session)
{
	int result;

	if (session == NULL ||
	    (session->state != INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED &&
	     session->state !=
	     INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA) ||
	    (!session->dma_prepared && !session->hardware_touched))
		return INTEL_AX211_RUNTIME_START_INVALID;
	result = ax211_runtime_start_stop_and_release(session);
	if (result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED)
		return result;
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	session->last_error = (uint8_t)result;
	return result;
}

/* Copies the retained MCC response only from a live completed session. */
int
intel_ax211_runtime_start_mcc(
	const struct intel_ax211_runtime_start *session,
	struct intel_ax211_runtime_mcc *mcc)
{
	if (session == NULL || mcc == NULL ||
	    session->state != INTEL_AX211_RUNTIME_START_STATE_RUNNING ||
	    !session->mcc_valid)
		return INTEL_AX211_RUNTIME_START_INVALID;
	*mcc = session->mcc;
	return INTEL_AX211_RUNTIME_START_OK;
}

static int
ax211_runtime_start_ops_valid(
	const struct intel_ax211_runtime_start_ops *ops)
{
	if (ops == NULL || ops->boot.receive_epoch_begin == NULL ||
	    ops->boot.transport_bind == NULL ||
	    ops->boot.receive_event == NULL ||
	    ops->boot.publish_pnvm == NULL || ops->boot.post_alive == NULL ||
	    ops->boot.interrupt_drain == NULL ||
	    ops->boot.clock_us == NULL || ops->nic_lock == NULL ||
	    ops->nic_unlock == NULL)
		return 0;
	return 1;
}

static uint32_t
ax211_runtime_start_next_generation(
	uint32_t generation)
{
	generation++;
	if (generation == 0U)
		generation = 1U;
	return generation;
}

/* Clears one stopped run without discarding copied read-NVM inputs. */
static void
ax211_runtime_start_run_state_clear(
	struct intel_ax211_runtime_start *session)
{
	memset(&session->files, 0, sizeof(session->files));
	memset(&session->dma, 0, sizeof(session->dma));
	memset(&session->commands, 0, sizeof(session->commands));
	memset(&session->alive, 0, sizeof(session->alive));
	memset(&session->profile, 0, sizeof(session->profile));
	memset(&session->runtime, 0, sizeof(session->runtime));
	memset(&session->mcc, 0, sizeof(session->mcc));
	memset(session->event_bytes, 0, sizeof(session->event_bytes));
	memset(session->response_bytes, 0, sizeof(session->response_bytes));
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
	session->nic_locked = 0U;
	session->last_error = INTEL_AX211_RUNTIME_START_OK;
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

	result = intel_ax211_firmware_files_load(&session->files);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_FIRMWARE;
	session->files_loaded = 1U;
	if (session->files.ucode_bytes == NULL ||
	    session->files.ucode_size == 0U ||
	    session->files.pnvm_bytes == NULL ||
	    session->files.pnvm_size == 0U)
		return INTEL_AX211_RUNTIME_START_FIRMWARE;

	offset = session->files.ucode_manifest.command_versions_offset;
	length = session->files.ucode_manifest.command_versions_length;
	if (length != sizeof(session->command_version_bytes) ||
	    offset > session->files.ucode_size ||
	    length > session->files.ucode_size - offset ||
	    memcmp(session->files.ucode_bytes + offset,
	    session->command_version_bytes, length) != 0)
		return INTEL_AX211_RUNTIME_START_FIRMWARE;
	result = intel_ax211_protocol_command_table_parse(
	    session->command_version_bytes,
	    sizeof(session->command_version_bytes), &table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	result = intel_ax211_runtime_profile_from_manifest(
	    &session->files.ucode_manifest, &session->nvm,
	    session->ltr_enabled, &session->profile);
	if (result != INTEL_AX211_RUNTIME_OK)
		return ax211_runtime_start_runtime_result(result);
	session->profile_valid = 1U;
	return INTEL_AX211_RUNTIME_START_OK;
}

static int
ax211_runtime_start_prepare_dma(
	struct intel_ax211_runtime_start *session)
{
	int result;

	result = intel_ax211_dma_prepare_boot(session->dma_device,
	    session->files.ucode_bytes, session->files.ucode_size,
	    &session->files.ucode_manifest, session->hardware_revision,
	    &session->dma);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_DMA;
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

	session->hardware_touched = 1U;
	result = intel_ax211_mmio_prepare_card_hw(session->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;
	result = intel_ax211_mmio_sw_reset(session->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;
	result = intel_ax211_mmio_apm_init(session->mmio);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;
	result = session->ops->boot.transport_bind(session->argument,
	    &session->dma,
	    session->mmio, session->transport, session->generation);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;
	session->transport_bound = 1U;
	result = intel_ax211_transport_configure_msix(session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;

	session->dma_exposed = 1U;
	result = intel_ax211_transport_initialize_rings(session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;
	if (session->dma.rx_buffer_count != INTEL_AX211_RX_RING_SIZE)
		return INTEL_AX211_RUNTIME_START_DMA;
	for (index = 0U; index < session->dma.rx_buffer_count; index++) {
		if (session->dma.rx_buffer[index].address == NULL ||
		    session->dma.rx_buffer[index].device_address == 0U)
			return INTEL_AX211_RUNTIME_START_DMA;
		result = intel_ax211_transport_publish_rx_descriptor(
		    session->transport, (uint16_t)index,
		    session->dma.rx_buffer[index].device_address);
		if (result != INTEL_AX211_TRANSPORT_OK)
			return INTEL_AX211_RUNTIME_START_TRANSPORT;
	}
	/* The first RX credit is deferred until the hardware-ALIVE cause. */
	result = intel_ax211_transport_enable_firmware_interrupts(
	    session->transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;

	if (session->dma.context.device_address == 0U ||
	    session->dma.iml.device_address == 0U ||
	    session->dma.iml.size != INTEL_AX211_MMIO_IML_SIZE)
		return INTEL_AX211_RUNTIME_START_DMA;
	memset(&mmio_boot, 0, sizeof(mmio_boot));
	mmio_boot.context_address = session->dma.context.device_address;
	mmio_boot.iml_address = session->dma.iml.device_address;
	mmio_boot.iml_size = (uint32_t)session->dma.iml.size;
	result = intel_ax211_mmio_publish_gen3(session->mmio, &mmio_boot);
	if (result != INTEL_AX211_MMIO_OK)
		return INTEL_AX211_RUNTIME_START_MMIO;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Selects the accepted-ALIVE SKU and publishes copied PNVM segments. */
static int
ax211_runtime_start_select_and_publish_pnvm(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_pnvm_manifest manifest;
	struct intel_ax211_sku_id sku;
	int result;

	memset(&sku, 0, sizeof(sku));
	sku.data[0] = session->alive.sku[0];
	sku.data[1] = session->alive.sku[1];
	sku.data[2] = session->alive.sku[2];
	result = intel_ax211_pnvm_parse(session->files.pnvm_bytes,
	    session->files.pnvm_size, &sku, session->mmio->profile.mac_type,
	    session->rf_type, &manifest);
	if (result != INTEL_AX211_OK)
		return INTEL_AX211_RUNTIME_START_FIRMWARE;
	result = intel_ax211_dma_prepare_pnvm(session->files.pnvm_bytes,
	    session->files.pnvm_size, &manifest, &session->dma);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_DMA;
	ax211_runtime_start_release_files(session);

	result = session->ops->boot.publish_pnvm(session->argument,
	    &session->dma);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	result = ax211_runtime_start_wait_notification(session,
	    AX211_RUNTIME_START_NOTIFICATION_PNVM,
	    INTEL_AX211_RUNTIME_START_PNVM_TIMEOUT_US);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	result = session->ops->boot.post_alive(session->argument,
	    &session->alive);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Performs the operational unified-firmware init/NVM handshake. */
static int
ax211_runtime_start_init_firmware(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_protocol_command_table table;
	int result;

	result = intel_ax211_command_transaction_init(&session->commands,
	    session->transport, 1U, session->generation);
	if (result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_RUNTIME_START_COMMAND;
	session->commands_initialized = 1U;
	result = ax211_runtime_start_send_extended_cfg(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	result = ax211_runtime_start_send_nvm_access_complete(session);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	result = ax211_runtime_start_wait_notification(session,
	    AX211_RUNTIME_START_NOTIFICATION_INIT,
	    INTEL_AX211_RUNTIME_START_INIT_TIMEOUT_US);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;

	/* DQA absence and every runtime layout are revalidated after init. */
	result = intel_ax211_protocol_command_table_parse(
	    session->command_version_bytes,
	    sizeof(session->command_version_bytes), &table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	result = intel_ax211_runtime_api89_validate(&table, &session->profile);
	if (result != INTEL_AX211_RUNTIME_OK)
		return ax211_runtime_start_runtime_result(result);
	return INTEL_AX211_RUNTIME_START_OK;
}

static int
ax211_runtime_start_send_extended_cfg(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	uint8_t payload[INTEL_AX211_INIT_EXTENDED_CFG_SIZE];
	uint8_t response[INTEL_AX211_RUNTIME_START_GENERIC_RESPONSE_SIZE];
	uint64_t deadline;
	size_t response_length;
	int result;

	result = intel_ax211_init_extended_cfg_encode(
	    INTEL_AX211_INIT_PROFILE_READ_NVM, payload);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return ax211_runtime_start_protocol_result(result);
	memset(&request, 0, sizeof(request));
	request.command.opcode = INTEL_AX211_INIT_EXTENDED_CFG_OPCODE;
	request.command.group = INTEL_AX211_INIT_SYSTEM_GROUP;
	request.command.version = 0U;
	request.payload = payload;
	request.payload_length = sizeof(payload);
	request.response_version = 0U;
	request.minimum_response_length = sizeof(response);
	request.maximum_response_length = sizeof(response);
	result = ax211_runtime_start_submit(session, &request, &handle,
	    &deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	response_length = 0U;
	memset(response, 0, sizeof(response));
	result = ax211_runtime_start_wait_command(session, deadline, response,
	    sizeof(response),
	    &response_length);
	if (result == INTEL_AX211_RUNTIME_START_OK &&
	    response_length != sizeof(response))
		result = INTEL_AX211_RUNTIME_START_PROTOCOL;
	if (result == INTEL_AX211_RUNTIME_START_OK &&
	    ax211_runtime_start_get_le32(response) != 0U)
		result = INTEL_AX211_RUNTIME_START_COMMAND;
	memset(response, 0, sizeof(response));
	return result;
}

static int
ax211_runtime_start_send_nvm_access_complete(
	struct intel_ax211_runtime_start *session)
{
	struct intel_ax211_command_handle handle;
	uint64_t deadline;
	uint64_t now;
	size_t response_length;
	int result;

	result = ax211_runtime_start_deadline(session,
	    INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, &now, &deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	result = intel_ax211_command_submit_nvm_access_complete(
	    &session->commands, now,
	    INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, &handle);
	if (result != INTEL_AX211_COMMAND_OK)
		return ax211_runtime_start_command_result(result);
	response_length = 0U;
	result = ax211_runtime_start_wait_command(session, deadline, NULL, 0U,
	    &response_length);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	if (response_length != 0U)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	return INTEL_AX211_RUNTIME_START_OK;
}

/* Owns the NIC across the complete ordered runtime command transaction. */
static int
ax211_runtime_start_commands(
	struct intel_ax211_runtime_start *session)
{
	int result;
	int unlock_result;

	result = session->ops->nic_lock(session->argument);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	session->nic_locked = 1U;
	result = ax211_runtime_start_commands_locked(session);
	unlock_result = session->ops->nic_unlock(session->argument);
	if (unlock_result == 0)
		session->nic_locked = 0U;
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	if (unlock_result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
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

	result = intel_ax211_protocol_command_table_parse(
	    session->command_version_bytes,
	    sizeof(session->command_version_bytes), &table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	result = session->ops->boot.clock_us(session->argument, &now);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	result = intel_ax211_runtime_begin(&session->runtime, &table,
	    &session->profile, session->generation, now);
	if (result != INTEL_AX211_RUNTIME_OK)
		return ax211_runtime_start_runtime_result(result);

	while (session->runtime.active && !session->runtime.terminal) {
		result = session->ops->boot.clock_us(session->argument, &now);
		if (result != 0)
			return INTEL_AX211_RUNTIME_START_IO;
		result = intel_ax211_runtime_current(&session->runtime, now,
		    &command);
		if (result != INTEL_AX211_RUNTIME_OK)
			return ax211_runtime_start_runtime_result(result);
		result = ax211_runtime_start_send_step(session,
		    session->runtime.step, &command);
		if (result != INTEL_AX211_RUNTIME_START_OK)
			return result;
		result = session->ops->boot.clock_us(session->argument, &now);
		if (result != 0)
			return INTEL_AX211_RUNTIME_START_IO;
		result = intel_ax211_runtime_ack(&session->runtime,
		    session->generation, session->runtime.step, now);
		if (result == INTEL_AX211_RUNTIME_COMPLETE)
			break;
		if (result != INTEL_AX211_RUNTIME_OK)
			return ax211_runtime_start_runtime_result(result);
	}
	if (!session->runtime.terminal || session->runtime.active ||
	    session->runtime.step != INTEL_AX211_RUNTIME_STEP_DONE)
		return INTEL_AX211_RUNTIME_START_RUNTIME;
	if (session->profile.lar_enabled && !session->mcc_valid)
		return INTEL_AX211_RUNTIME_START_RUNTIME;
	return INTEL_AX211_RUNTIME_START_OK;
}

static int
ax211_runtime_start_send_step(
	struct intel_ax211_runtime_start *session,
	enum intel_ax211_runtime_step step,
	const struct intel_ax211_runtime_command *command)
{
	struct intel_ax211_protocol_message message;
	struct intel_ax211_command_request request;
	struct intel_ax211_command_handle handle;
	uint64_t deadline;
	size_t response_capacity;
	size_t response_length;
	int result;

	if (command == NULL || command->wire_version != 0U ||
	    command->payload_length > sizeof(command->payload))
		return INTEL_AX211_RUNTIME_START_RUNTIME;
	memset(&request, 0, sizeof(request));
	request.command.group = command->group;
	request.command.opcode = command->opcode;
	request.command.version = command->wire_version;
	request.payload = command->payload;
	request.payload_length = command->payload_length;
	request.response_version = command->response_version;
	if (step == INTEL_AX211_RUNTIME_STEP_MCC_UPDATE) {
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
	result = ax211_runtime_start_submit(session, &request, &handle,
	    &deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	response_length = 0U;
	result = ax211_runtime_start_wait_command(session, deadline,
	    response_capacity == 0U ? NULL : session->response_bytes,
	    response_capacity, &response_length);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	if (step != INTEL_AX211_RUNTIME_STEP_MCC_UPDATE) {
		if (response_length != 0U)
			return INTEL_AX211_RUNTIME_START_PROTOCOL;
		return INTEL_AX211_RUNTIME_START_OK;
	}

	memset(&message, 0, sizeof(message));
	message.group = command->group;
	message.opcode = command->opcode;
	message.version = command->response_version;
	message.queue = handle.token.queue;
	message.index = handle.token.index;
	message.generation = session->generation;
	message.payload = session->response_bytes;
	message.payload_length = response_length;
	result = intel_ax211_runtime_mcc_decode(&message,
	    session->generation, &session->mcc);
	memset(session->response_bytes, 0, sizeof(session->response_bytes));
	if (result != INTEL_AX211_RUNTIME_OK)
		return ax211_runtime_start_runtime_result(result);
	session->mcc_valid = 1U;
	return INTEL_AX211_RUNTIME_START_OK;
}

static uint32_t
ax211_runtime_start_get_le32(
	const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int
ax211_runtime_start_submit(
	struct intel_ax211_runtime_start *session,
	const struct intel_ax211_command_request *request,
	struct intel_ax211_command_handle *handle,
	uint64_t *deadline)
{
	uint64_t now;
	int result;

	result = ax211_runtime_start_deadline(session,
	    INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, &now, deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	result = intel_ax211_command_submit(&session->commands, request, now,
	    INTEL_AX211_RUNTIME_START_COMMAND_TIMEOUT_US, handle);
	if (result != INTEL_AX211_COMMAND_OK)
		return ax211_runtime_start_command_result(result);
	return INTEL_AX211_RUNTIME_START_OK;
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
	int result;

	for (index = 0U;
	     index < INTEL_AX211_RUNTIME_START_EVENT_LIMIT; index++) {
		result = ax211_runtime_start_receive(session, deadline, &event,
		    &message);
		if (result != INTEL_AX211_RUNTIME_START_OK)
			return result;
		if (message.generation != session->generation)
			continue;
		if ((event.queue & 0x80U) != 0U) {
			kind = ax211_runtime_start_notification_kind(&message);
			if (ax211_runtime_start_notification_duplicate(session,
			    kind))
				return INTEL_AX211_RUNTIME_START_DUPLICATE;
			if (kind != 0)
				return INTEL_AX211_RUNTIME_START_PROTOCOL;
			continue;
		}
		result = intel_ax211_command_complete(&session->commands,
		    session->event_bytes,
		    message.payload_length + INTEL_AX211_EVENT_HEADER_SIZE,
		    session->generation, response, response_capacity,
		    response_length);
		return ax211_runtime_start_command_result(result);
	}

	result = session->ops->boot.clock_us(session->argument, &now);
	if (result == 0 && now < deadline)
		now = deadline;
	result = intel_ax211_command_timeout_oldest(&session->commands, now,
	    &timed_out);
	if (result != INTEL_AX211_COMMAND_TIMEOUT &&
	    result != INTEL_AX211_COMMAND_POISONED)
		return INTEL_AX211_RUNTIME_START_COMMAND;
	return INTEL_AX211_RUNTIME_START_TIMEOUT;
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
	int result;

	result = ax211_runtime_start_deadline(session, timeout, &now,
	    &deadline);
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	for (index = 0U;
	     index < INTEL_AX211_RUNTIME_START_EVENT_LIMIT; index++) {
		result = ax211_runtime_start_receive(session, deadline, &event,
		    &message);
		if (result != INTEL_AX211_RUNTIME_START_OK)
			return result;
		if (message.generation != session->generation)
			continue;
		if ((event.queue & 0x80U) == 0U)
			return INTEL_AX211_RUNTIME_START_PROTOCOL;
		kind = ax211_runtime_start_notification_kind(&message);
		if (ax211_runtime_start_notification_duplicate(session, kind))
			return INTEL_AX211_RUNTIME_START_DUPLICATE;
		if (kind == 0)
			continue;
		if (kind != (int)expected)
			return INTEL_AX211_RUNTIME_START_PROTOCOL;
		return ax211_runtime_start_notification_accept(session, kind,
		    &message);
	}
	return INTEL_AX211_RUNTIME_START_TIMEOUT;
}

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

	memset(&received, 0, sizeof(received));
	result = session->ops->boot.receive_event(session->argument, deadline,
	    session->event_bytes, sizeof(session->event_bytes), &received);
	if (result == INTEL_AX211_BOOT_RECEIVE_TIMEOUT)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;
	if (result != INTEL_AX211_BOOT_RECEIVE_OK)
		return INTEL_AX211_RUNTIME_START_IO;
	if (received.length == 0U ||
	    received.length > sizeof(session->event_bytes) ||
	    received.generation == 0U)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	result = session->ops->boot.clock_us(session->argument, &now);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	if (now > deadline)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;
	result = intel_ax211_event_decode(session->event_bytes,
	    received.length, event);
	if (result != INTEL_AX211_OK ||
	    event->payload_offset > received.length ||
	    event->payload_length != received.length - event->payload_offset)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	memset(message, 0, sizeof(*message));
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
	return INTEL_AX211_RUNTIME_START_OK;
}

static int
ax211_runtime_start_deadline(
	struct intel_ax211_runtime_start *session,
	uint64_t timeout,
	uint64_t *now,
	uint64_t *deadline)
{
	int result;

	if (timeout == 0U || now == NULL || deadline == NULL)
		return INTEL_AX211_RUNTIME_START_INVALID;
	result = session->ops->boot.clock_us(session->argument, now);
	if (result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	if (*now > UINT64_MAX - timeout)
		return INTEL_AX211_RUNTIME_START_IO;
	*deadline = *now + timeout;
	return INTEL_AX211_RUNTIME_START_OK;
}

static int
ax211_runtime_start_notification_kind(
	const struct intel_ax211_protocol_message *message)
{
	if (message->group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    message->opcode == INTEL_AX211_PROTOCOL_ALIVE_OPCODE)
		return AX211_RUNTIME_START_NOTIFICATION_ALIVE;
	if (message->group == INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM &&
	    message->opcode == INTEL_AX211_PROTOCOL_PNVM_INIT_COMPLETE_OPCODE)
		return AX211_RUNTIME_START_NOTIFICATION_PNVM;
	if (message->group == INTEL_AX211_PROTOCOL_GROUP_LEGACY &&
	    message->opcode == INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE)
		return AX211_RUNTIME_START_NOTIFICATION_INIT;
	return 0;
}

static int
ax211_runtime_start_notification_duplicate(
	const struct intel_ax211_runtime_start *session,
	int kind)
{
	if (kind == AX211_RUNTIME_START_NOTIFICATION_ALIVE &&
	    session->alive_accepted)
		return 1;
	if (kind == AX211_RUNTIME_START_NOTIFICATION_PNVM &&
	    session->pnvm_accepted)
		return 1;
	if (kind == AX211_RUNTIME_START_NOTIFICATION_INIT &&
	    session->init_accepted)
		return 1;
	return 0;
}

static int
ax211_runtime_start_notification_accept(
	struct intel_ax211_runtime_start *session,
	int kind,
	const struct intel_ax211_protocol_message *message)
{
	int result;

	if (kind == AX211_RUNTIME_START_NOTIFICATION_ALIVE) {
		result = intel_ax211_protocol_alive_decode(message,
		    session->generation, &session->alive);
		if (result == INTEL_AX211_PROTOCOL_OK)
			session->alive_accepted = 1U;
	} else if (kind == AX211_RUNTIME_START_NOTIFICATION_PNVM) {
		result = intel_ax211_protocol_pnvm_init_complete(message,
		    session->generation);
		if (result == INTEL_AX211_PROTOCOL_OK)
			session->pnvm_accepted = 1U;
	} else if (kind == AX211_RUNTIME_START_NOTIFICATION_INIT) {
		result = intel_ax211_init_complete_validate(message,
		    session->generation);
		if (result == INTEL_AX211_PROTOCOL_OK)
			session->init_accepted = 1U;
	} else {
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	}
	return ax211_runtime_start_protocol_result(result);
}

/* Proves reset before retiring command metadata or releasing any DMA. */
static int
ax211_runtime_start_stop_and_release(
	struct intel_ax211_runtime_start *session)
{
	uint32_t retired_generation;
	int command_result;
	int after_reset_result;
	int drain_result;
	int nic_unlock_result;
	int quiesce_result;
	int stop_result;

	nic_unlock_result = 0;
	if (session->nic_locked) {
		nic_unlock_result = session->ops->nic_unlock(session->argument);
		if (nic_unlock_result == 0)
			session->nic_locked = 0U;
	}

	/* A prior no-DMA failure still requires a checked controller reset. */
	if (!session->dma_prepared) {
		if (!session->hardware_touched)
			return nic_unlock_result == 0 ?
			    INTEL_AX211_RUNTIME_START_OK :
			    INTEL_AX211_RUNTIME_START_IO;
		stop_result = intel_ax211_mmio_stop(session->mmio);
		if (stop_result != INTEL_AX211_MMIO_OK) {
			session->state =
			    INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA;
			return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
		}
		session->hardware_touched = 0U;
		session->nic_locked = 0U;
		return nic_unlock_result == 0 ?
		    INTEL_AX211_RUNTIME_START_OK :
		    INTEL_AX211_RUNTIME_START_IO;
	}

	/* Never-exposed DMA is safe to free, but reset failure remains sticky. */
	if (!session->dma_exposed && !session->transport_bound) {
		stop_result = INTEL_AX211_MMIO_OK;
		if (session->hardware_touched)
			stop_result = intel_ax211_mmio_stop(session->mmio);
		intel_ax211_dma_release(&session->dma);
		session->dma_prepared = 0U;
		if (stop_result != INTEL_AX211_MMIO_OK) {
			session->state =
			    INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED_NO_DMA;
			return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
		}
		session->hardware_touched = 0U;
		session->nic_locked = 0U;
		return nic_unlock_result == 0 ?
		    INTEL_AX211_RUNTIME_START_OK :
		    INTEL_AX211_RUNTIME_START_IO;
	}

	drain_result = session->ops->boot.interrupt_drain(session->argument);
	quiesce_result = intel_ax211_transport_quiesce(session->transport);
	stop_result = intel_ax211_mmio_stop(session->mmio);
	if (drain_result != 0 || stop_result != INTEL_AX211_MMIO_OK) {
		session->state = INTEL_AX211_RUNTIME_START_STATE_STOP_REQUIRED;
		return INTEL_AX211_RUNTIME_START_STOP_REQUIRED;
	}
	after_reset_result = intel_ax211_transport_command_after_device_reset(
	    session->transport);
	command_result = INTEL_AX211_COMMAND_OK;
	if (after_reset_result == INTEL_AX211_TRANSPORT_OK &&
	    session->commands_initialized) {
		retired_generation = ax211_runtime_start_next_generation(
		    session->generation);
		command_result = intel_ax211_command_after_device_reset(
		    &session->commands, retired_generation);
	}
	intel_ax211_dma_release(&session->dma);
	session->dma_prepared = 0U;
	session->dma_exposed = 0U;
	session->hardware_touched = 0U;
	session->transport_bound = 0U;
	session->commands_initialized = 0U;
	session->nic_locked = 0U;
	if (quiesce_result != INTEL_AX211_TRANSPORT_OK ||
	    after_reset_result != INTEL_AX211_TRANSPORT_OK)
		return INTEL_AX211_RUNTIME_START_TRANSPORT;
	if (command_result != INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_RUNTIME_START_COMMAND;
	if (nic_unlock_result != 0)
		return INTEL_AX211_RUNTIME_START_IO;
	return INTEL_AX211_RUNTIME_START_OK;
}

static void
ax211_runtime_start_release_files(
	struct intel_ax211_runtime_start *session)
{
	if (!session->files_loaded)
		return;
	intel_ax211_firmware_files_release(&session->files);
	session->files_loaded = 0U;
}

static int
ax211_runtime_start_fail(
	struct intel_ax211_runtime_start *session,
	int result)
{
	int cleanup_result;

	ax211_runtime_start_release_files(session);
	session->last_error = (uint8_t)result;
	cleanup_result = ax211_runtime_start_stop_and_release(session);
	if (cleanup_result == INTEL_AX211_RUNTIME_START_STOP_REQUIRED)
		return cleanup_result;
	session->state = INTEL_AX211_RUNTIME_START_STATE_IDLE;
	if (result != INTEL_AX211_RUNTIME_START_OK)
		return result;
	return cleanup_result;
}

static int
ax211_runtime_start_protocol_result(
	int result)
{
	if (result == INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_RUNTIME_START_OK;
	if (result == INTEL_AX211_PROTOCOL_DUPLICATE)
		return INTEL_AX211_RUNTIME_START_DUPLICATE;
	return INTEL_AX211_RUNTIME_START_PROTOCOL;
}

static int
ax211_runtime_start_command_result(
	int result)
{
	if (result == INTEL_AX211_COMMAND_OK)
		return INTEL_AX211_RUNTIME_START_OK;
	if (result == INTEL_AX211_COMMAND_TIMEOUT)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;
	if (result == INTEL_AX211_COMMAND_DUPLICATE)
		return INTEL_AX211_RUNTIME_START_DUPLICATE;
	if (result == INTEL_AX211_COMMAND_MALFORMED ||
	    result == INTEL_AX211_COMMAND_FIRMWARE_FAILED ||
	    result == INTEL_AX211_COMMAND_VERSION_MISMATCH ||
	    result == INTEL_AX211_COMMAND_OUT_OF_ORDER ||
	    result == INTEL_AX211_COMMAND_BUFFER_TOO_SMALL)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	return INTEL_AX211_RUNTIME_START_COMMAND;
}

static int
ax211_runtime_start_runtime_result(
	int result)
{
	if (result == INTEL_AX211_RUNTIME_OK ||
	    result == INTEL_AX211_RUNTIME_COMPLETE)
		return INTEL_AX211_RUNTIME_START_OK;
	if (result == INTEL_AX211_RUNTIME_TIMEOUT)
		return INTEL_AX211_RUNTIME_START_TIMEOUT;
	if (result == INTEL_AX211_RUNTIME_DUPLICATE)
		return INTEL_AX211_RUNTIME_START_DUPLICATE;
	if (result == INTEL_AX211_RUNTIME_TRUNCATED ||
	    result == INTEL_AX211_RUNTIME_OVERSIZED ||
	    result == INTEL_AX211_RUNTIME_UNSUPPORTED)
		return INTEL_AX211_RUNTIME_START_PROTOCOL;
	return INTEL_AX211_RUNTIME_START_RUNTIME;
}
