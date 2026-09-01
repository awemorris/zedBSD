/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private Gen3 transport implementation
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c at
 * commit 0f464d413c50396e4e6cd70948f15613d6a73081.
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "intel-ax211-transport.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AX211_COMMAND_TFD_SIZE                         65536U
#define AX211_COMMAND_BYTE_COUNT_SIZE                   2048U
#define AX211_COMMAND_SLOTS_SIZE                       82944U
#define AX211_COMMAND_EXTERNAL_SIZE                     4096U
#define AX211_RX_TRANSFER_SIZE                          8192U
#define AX211_RX_COMPLETION_SIZE                       16384U
#define AX211_RX_STATUS_SIZE                               2U
#define AX211_COMMAND_QUEUE                                0U
#define AX211_COMMAND_FIRST_TRANSFER_SIZE                  20U
#define AX211_COMMAND_GROUP_LEGACY                          0U
#define AX211_COMMAND_GROUP_LONG                            1U

#define AX211_CSR_INT_COALESCING                         0x004U
#define AX211_CSR_UCODE_DRV_GP1_CLR                     0x05cU
#define AX211_CSR_MAC_SHADOW_REG_CTRL                   0x0a8U
#define AX211_HBUS_TARG_WRPTR                           0x460U
#define AX211_RFH_Q0_FRBDCB_WIDX_TRG                   0x1c80U
#define AX211_CSR_MSIX_FH_INT_CAUSES_AD                0x2800U
#define AX211_CSR_MSIX_FH_INT_MASK_AD                  0x2804U
#define AX211_CSR_MSIX_HW_INT_CAUSES_AD                0x2808U
#define AX211_CSR_MSIX_HW_INT_MASK_AD                  0x280cU
#define AX211_CSR_MSIX_AUTOMASK_ST_AD                  0x2810U
#define AX211_CSR_MSIX_RX_IVAR_AD_REG                  0x2880U
#define AX211_CSR_MSIX_IVAR_AD_REG                     0x2890U

#define AX211_HOST_INT_TIMEOUT_DEFAULT                    0x40U
#define AX211_MAC_SHADOW_ENABLE                       0x800fffffU
#define AX211_UCODE_RFKILL_CLEAR                      0x00000002U
#define AX211_UCODE_COMMAND_BLOCKED_CLEAR             0x00000004U
#define AX211_MSIX_NON_AUTO_CLEAR                             0x80U
#define AX211_MSIX_VECTOR                                      0U
#define AX211_MSIX_AUTOMASK_VECTOR0                            1U

#define AX211_FH_CAUSE_Q0                              0x00000001U
#define AX211_FH_CAUSE_Q1                              0x00000002U
#define AX211_FH_CAUSE_D2S_CH0                         0x00010000U
#define AX211_FH_CAUSE_D2S_CH1                         0x00020000U
#define AX211_FH_CAUSE_S2D                             0x00080000U
#define AX211_FH_CAUSE_ERROR                           0x00200000U
#define AX211_FH_SUPPORTED (AX211_FH_CAUSE_Q0 | AX211_FH_CAUSE_Q1 | \
	AX211_FH_CAUSE_D2S_CH0 | AX211_FH_CAUSE_D2S_CH1 | \
	AX211_FH_CAUSE_S2D | AX211_FH_CAUSE_ERROR)

#define AX211_HW_CAUSE_ALIVE                           0x00000001U
#define AX211_HW_CAUSE_WAKEUP                          0x00000002U
#define AX211_HW_CAUSE_RESET_DONE                      0x00000004U
#define AX211_HW_CAUSE_SW_ERROR_V2                     0x00000020U
#define AX211_HW_CAUSE_CT_KILL                         0x00000040U
#define AX211_HW_CAUSE_RF_KILL                         0x00000080U
#define AX211_HW_CAUSE_PERIODIC                        0x00000100U
#define AX211_HW_CAUSE_SW_ERROR                        0x02000000U
#define AX211_HW_CAUSE_SCD                             0x04000000U
#define AX211_HW_CAUSE_FH_TX                           0x08000000U
#define AX211_HW_CAUSE_HW_ERROR                        0x20000000U
#define AX211_HW_CAUSE_HAP                             0x40000000U
#define AX211_HW_SUPPORTED (AX211_HW_CAUSE_ALIVE | AX211_HW_CAUSE_WAKEUP | \
	AX211_HW_CAUSE_RESET_DONE | AX211_HW_CAUSE_SW_ERROR_V2 | \
	AX211_HW_CAUSE_CT_KILL | AX211_HW_CAUSE_RF_KILL | \
	AX211_HW_CAUSE_PERIODIC | AX211_HW_CAUSE_SW_ERROR | \
	AX211_HW_CAUSE_SCD | AX211_HW_CAUSE_FH_TX | \
	AX211_HW_CAUSE_HW_ERROR | AX211_HW_CAUSE_HAP)

#define AX211_UREG_CHICK                                0xa05c00U
#define AX211_UREG_CHICK_MSIX_ENABLE                   0x02000000U
#define AX211_RFH_GEN_STATUS_GEN3                       0xa07824U
#define AX211_RFH_RXF_DMA_CFG_GEN3                     0xa07880U
#define AX211_RXF_DMA_IDLE                              0x80000000U
#define AX211_RX_IDLE_TIMEOUT_US                             10000U
#define AX211_RX_IDLE_POLL_US                                   10U

static const uint8_t ax211_fh_ivar_cause[] = {
	0x00U, 0x01U, 0x03U, 0x05U
};

static const uint8_t ax211_hw_ivar_cause[] = {
	0x10U, 0x11U, 0x12U, 0x16U, 0x17U, 0x18U,
	0x29U, 0x15U, 0x2aU, 0x2bU, 0x2dU, 0x2eU
};

static int ax211_transport_valid(const struct intel_ax211_transport *transport);
static int ax211_profile_valid(const struct intel_ax211_mmio_profile *profile);
static int ax211_memory_valid(const struct intel_ax211_transport_ring_memory *memory);
static int ax211_ops_valid(const struct intel_ax211_transport_ops *ops);
static int ax211_csr_read(struct intel_ax211_transport *transport, uint32_t offset, uint32_t *value);
static int ax211_csr_write(struct intel_ax211_transport *transport, uint32_t offset, uint32_t value);
static int ax211_csr_write8(struct intel_ax211_transport *transport, uint32_t offset, uint8_t value);
static int ax211_dma_sync(struct intel_ax211_transport *transport, enum intel_ax211_transport_dma_region region, size_t offset, size_t length, enum intel_ax211_transport_dma_direction direction);
static int ax211_mask_all(struct intel_ax211_transport *transport);
static void ax211_mask_all_best_effort(struct intel_ax211_transport *transport);
static int ax211_ack_raw(struct intel_ax211_transport *transport, uint32_t *flow_handler, uint32_t *hardware);
static int ax211_configure_msix_routes(struct intel_ax211_transport *transport);
static int ax211_rx_descriptor_valid(uint16_t index, uint64_t device_address);
static int ax211_publish_rx_descriptor(struct intel_ax211_transport *transport, uint16_t index, uint64_t device_address);
static void ax211_set_published(struct intel_ax211_transport *transport, uint16_t index, int published);
static int ax211_is_published(const struct intel_ax211_transport *transport, uint16_t index);
static int ax211_all_rx_published(const struct intel_ax211_transport *transport);
static uint16_t ax211_get_le16(const uint8_t *bytes);
static void ax211_command_rollback(struct intel_ax211_transport *transport);
static int ax211_command_external_matches(
	const struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token);
static int ax211_command_external_scrub(
	struct intel_ax211_transport *transport);
static int ax211_poll_rx_idle(struct intel_ax211_transport *transport);

/*
 * Initializes one exact AX211 Gen3 transport without touching hardware.
 */
int
intel_ax211_transport_init(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_transport_ops *ops,
	void *argument,
	const struct intel_ax211_mmio_profile *profile,
	const struct intel_ax211_transport_ring_memory *memory)
{
	int result;

	if (transport == NULL || !ax211_ops_valid(ops))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!ax211_profile_valid(profile) || !ax211_memory_valid(memory))
		return INTEL_AX211_TRANSPORT_INVALID;

	/* Initializes an unpublished and fail-closed transport state. */
	memset(transport, 0, sizeof(*transport));
	transport->ops = ops;
	transport->argument = argument;
	transport->profile = *profile;
	transport->memory = *memory;
	result = intel_ax211_ring_init(&transport->command_ring,
	    AX211_COMMAND_QUEUE, INTEL_AX211_COMMAND_RING_SIZE);
	if (result != INTEL_AX211_OK) {
		memset(transport, 0, sizeof(*transport));
		return INTEL_AX211_TRANSPORT_INVALID;
	}
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Routes every admitted AX211 cause to one non-auto-clearing MSI-X vector.
 */
int
intel_ax211_transport_configure_msix(
	struct intel_ax211_transport *transport)
{
	int result;
	int unlock_result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (transport->msix_configured)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Selects MSI-X inside UMAC while NIC ownership is held. */
	result = transport->ops->nic_lock(transport->argument);
	if (result != 0)
		return INTEL_AX211_TRANSPORT_IO;
	result = transport->ops->prph_write32(transport->argument,
	    transport->profile.umac_prph_offset + AX211_UREG_CHICK,
	    AX211_UREG_CHICK_MSIX_ENABLE);
	unlock_result = transport->ops->nic_unlock(transport->argument);
	if (result != 0 || unlock_result != 0) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return INTEL_AX211_TRANSPORT_IO;
	}

	/* Keeps every cause masked while the IVAR table is changed. */
	result = ax211_mask_all(transport);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	/* Programs the single-vector routing table. */
	result = ax211_configure_msix_routes(transport);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		ax211_mask_all_best_effort(transport);
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	transport->msix_configured = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Initializes the preallocated command and RX Gen3 ring memory.
 */
int
intel_ax211_transport_initialize_rings(
	struct intel_ax211_transport *transport)
{
	uint32_t shadow;
	int core_result;
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (transport->quiesced || transport->failed ||
	    transport->rings_initialized)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Clears every host-owned command and completion object. */
	memset(transport->memory.command_tfd, 0,
	    transport->memory.command_tfd_size);
	memset(transport->memory.command_byte_count, 0,
	    transport->memory.command_byte_count_size);
	memset(transport->memory.command_slots, 0,
	    transport->memory.command_slots_size);
	memset(transport->memory.command_external, 0,
	    transport->memory.command_external_size);
	memset(transport->memory.rx_completion, 0,
	    transport->memory.rx_completion_size);
	memset(transport->memory.rx_status, 0,
	    transport->memory.rx_status_size);
	memset(transport->rx_published, 0,
	    sizeof(transport->rx_published));

	/* Publishes the cleared command objects before any doorbell can ring. */
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, 0U,
	    transport->memory.command_tfd_size,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT, 0U,
		    transport->memory.command_byte_count_size,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS, 0U,
		    transport->memory.command_slots_size,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL, 0U,
		    transport->memory.command_external_size,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	/* Arms device-write completion and status memory for DMA. */
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION, 0U,
	    transport->memory.rx_completion_size,
	    INTEL_AX211_TRANSPORT_DMA_PREREAD);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 0U,
		    transport->memory.rx_status_size,
		    INTEL_AX211_TRANSPORT_DMA_PREREAD);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	/* Resets host bookkeeping to command queue zero and RX index zero. */
	core_result = intel_ax211_ring_init(&transport->command_ring,
	    AX211_COMMAND_QUEUE, INTEL_AX211_COMMAND_RING_SIZE);
	if (core_result != INTEL_AX211_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return INTEL_AX211_TRANSPORT_FAILED;
	}
	transport->rx_head = 0U;
	transport->rx_tail = 0U;
	transport->rx_pending = 0U;
	transport->rx_active = 0U;
	transport->rx_last_credit = 0xffffU;

	/* Applies the proven interrupt-coalescing and shadow-register setup. */
	result = ax211_csr_write8(transport, AX211_CSR_INT_COALESCING,
	    AX211_HOST_INT_TIMEOUT_DEFAULT);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	result = ax211_csr_read(transport, AX211_CSR_MAC_SHADOW_REG_CTRL,
	    &shadow);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	shadow |= AX211_MAC_SHADOW_ENABLE;
	result = ax211_csr_write(transport, AX211_CSR_MAC_SHADOW_REG_CTRL,
	    shadow);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	transport->rings_initialized = 1U;
	transport->rx_dma_idle = 0U;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Clears stale startup state and enables only firmware-load causes.
 */
int
intel_ax211_transport_enable_firmware_interrupts(
	struct intel_ax211_transport *transport)
{
	uint32_t flow_handler;
	uint32_t hardware;
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->msix_configured || !transport->rings_initialized ||
	    transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Masks and acknowledges every stale cause before clearing handshakes. */
	result = ax211_mask_all(transport);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_ack_raw(transport, &flow_handler, &hardware);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		ax211_mask_all_best_effort(transport);
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	/* Clears firmware RF-kill and blocked-command handshake bits. */
	result = ax211_csr_write(transport, AX211_CSR_UCODE_DRV_GP1_CLR,
	    AX211_UCODE_RFKILL_CLEAR);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_csr_write(transport,
		    AX211_CSR_UCODE_DRV_GP1_CLR,
		    AX211_UCODE_COMMAND_BLOCKED_CLEAR);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		ax211_mask_all_best_effort(transport);
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	/* Re-acknowledges before exposing ALIVE and flow-handler delivery. */
	result = ax211_ack_raw(transport, &flow_handler, &hardware);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		ax211_mask_all_best_effort(transport);
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	result = ax211_csr_write(transport, AX211_CSR_MSIX_HW_INT_MASK_AD,
	    ~AX211_HW_CAUSE_ALIVE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_csr_write(transport,
		    AX211_CSR_MSIX_FH_INT_MASK_AD, ~AX211_FH_SUPPORTED);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		ax211_mask_all_best_effort(transport);
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	transport->enabled_hw_causes = AX211_HW_CAUSE_ALIVE;
	transport->enabled_fh_causes = AX211_FH_SUPPORTED;
	transport->interrupts_enabled = 1U;
	transport->firmware_load_mode = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Enables the complete proven AX211 runtime cause set.
 */
int
intel_ax211_transport_enable_runtime_interrupts(
	struct intel_ax211_transport *transport)
{
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->msix_configured || !transport->rings_initialized ||
	    transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Publishes both unmasked-cause sets before recording runtime mode. */
	result = ax211_csr_write(transport, AX211_CSR_MSIX_FH_INT_MASK_AD,
	    ~AX211_FH_SUPPORTED);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_csr_write(transport,
		    AX211_CSR_MSIX_HW_INT_MASK_AD, ~AX211_HW_SUPPORTED);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		ax211_mask_all_best_effort(transport);
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	transport->enabled_fh_causes = AX211_FH_SUPPORTED;
	transport->enabled_hw_causes = AX211_HW_SUPPORTED;
	transport->interrupts_enabled = 1U;
	transport->firmware_load_mode = 0U;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Masks every MSI-X cause before later device or DMA shutdown.
 */
int
intel_ax211_transport_disable_interrupts(
	struct intel_ax211_transport *transport)
{
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->msix_configured)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Masks flow-handler causes before hardware causes. */
	result = ax211_mask_all(transport);
	transport->enabled_fh_causes = 0U;
	transport->enabled_hw_causes = 0U;
	transport->interrupts_enabled = 0U;
	transport->firmware_load_mode = 0U;
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		return result;
	}
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Acknowledges one MSI-X snapshot and returns only currently admitted causes.
 */
int
intel_ax211_transport_interrupt_claim(
	struct intel_ax211_transport *transport,
	struct intel_ax211_transport_causes *causes)
{
	uint32_t flow_handler;
	uint32_t hardware;
	int result;

	if (!ax211_transport_valid(transport) || causes == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->msix_configured || !transport->interrupts_enabled ||
	    transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Captures and W1C-acknowledges both cause banks before dispatch. */
	result = ax211_ack_raw(transport, &flow_handler, &hardware);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		return result;
	}
	causes->flow_handler = flow_handler & transport->enabled_fh_causes;
	causes->hardware = hardware & transport->enabled_hw_causes;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Rearms the single non-auto-clearing MSI-X vector after dispatch.
 */
int
intel_ax211_transport_interrupt_rearm(
	struct intel_ax211_transport *transport)
{
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->msix_configured || !transport->interrupts_enabled ||
	    transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;
	result = ax211_csr_write(transport, AX211_CSR_MSIX_AUTOMASK_ST_AD,
	    AX211_MSIX_AUTOMASK_VECTOR0);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
	}
	return result;
}

/*
 * Encodes and DMA-publishes one exact Gen3 RX transfer descriptor.
 */
int
intel_ax211_transport_publish_rx_descriptor(
	struct intel_ax211_transport *transport,
	uint16_t index,
	uint64_t device_address)
{
	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->rings_initialized || transport->rx_active ||
	    transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (!ax211_rx_descriptor_valid(index, device_address))
		return INTEL_AX211_TRANSPORT_INVALID;
	return ax211_publish_rx_descriptor(transport, index, device_address);
}

/*
 * Activates RX only after all 512 transfer descriptors are DMA-visible.
 */
int
intel_ax211_transport_activate_rx(
	struct intel_ax211_transport *transport)
{
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->rings_initialized || transport->rx_active ||
	    transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (!ax211_all_rx_published(transport))
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Gives firmware the first aligned descriptor-credit boundary. */
	result = ax211_csr_write(transport, AX211_RFH_Q0_FRBDCB_WIDX_TRG, 8U);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	transport->rx_last_credit = 8U;
	transport->rx_active = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Refreshes the bounded Gen3 RX completion head from DMA status memory.
 */
int
intel_ax211_transport_rx_refresh(
	struct intel_ax211_transport *transport)
{
	uint16_t hardware_head;
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->rx_active || transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Acquires the device-written status word before decoding its head. */
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 0U,
	    AX211_RX_STATUS_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	hardware_head = ax211_get_le16(transport->memory.rx_status);
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_RX_STATUS, 0U,
	    AX211_RX_STATUS_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_PREREAD);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	hardware_head &= 0x0fffU;
	hardware_head &= INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT - 1U;
	transport->rx_head = hardware_head;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Acquires one device-written RX completion without advancing host credit.
 */
int
intel_ax211_transport_rx_next(
	struct intel_ax211_transport *transport,
	struct intel_ax211_transport_rx_completion *completion)
{
	const uint8_t *descriptor;
	size_t offset;
	uint16_t buffer_id;
	uint8_t flags;
	int core_result;
	int result;

	if (!ax211_transport_valid(transport) || completion == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (transport->rx_pending)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Refreshes the device head before testing for one available entry. */
	result = intel_ax211_transport_rx_refresh(transport);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return result;
	if (transport->rx_tail == transport->rx_head)
		return INTEL_AX211_TRANSPORT_STALE;

	/* Acquires and decodes exactly one completion descriptor. */
	offset = (size_t)transport->rx_tail *
	    INTEL_AX211_TRANSPORT_RX_COMPLETION_SIZE;
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION, offset,
	    INTEL_AX211_TRANSPORT_RX_COMPLETION_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_POSTREAD);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return result;
	descriptor = transport->memory.rx_completion + offset;
	core_result = intel_ax211_rx_completion_descriptor_decode(descriptor,
	    &buffer_id, &flags);
	if (core_result != INTEL_AX211_OK ||
	    buffer_id >= INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!ax211_is_published(transport, buffer_id))
		return INTEL_AX211_TRANSPORT_STALE;

	/* Transfers that buffer slot to the caller until it is replenished. */
	ax211_set_published(transport, buffer_id, 0);
	transport->rx_pending = 1U;
	transport->rx_pending_index = transport->rx_tail;
	transport->rx_pending_buffer = buffer_id;
	completion->completion_index = transport->rx_tail;
	completion->buffer_id = buffer_id;
	completion->flags = flags;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Replenishes the pending RX buffer and advances aligned firmware credit.
 */
int
intel_ax211_transport_rx_replenish(
	struct intel_ax211_transport *transport,
	uint64_t device_address)
{
	size_t completion_offset;
	uint16_t credit;
	uint16_t previous;
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->rx_pending || transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (transport->rx_pending_index != transport->rx_tail)
		return INTEL_AX211_TRANSPORT_STALE;
	if (!ax211_rx_descriptor_valid(transport->rx_pending_buffer,
	    device_address))
		return INTEL_AX211_TRANSPORT_INVALID;

	/* Re-encodes only the buffer owned by the pending completion. */
	result = ax211_publish_rx_descriptor(transport,
	    transport->rx_pending_buffer, device_address);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	completion_offset = (size_t)transport->rx_pending_index *
	    INTEL_AX211_TRANSPORT_RX_COMPLETION_SIZE;
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_RX_COMPLETION, completion_offset,
	    INTEL_AX211_TRANSPORT_RX_COMPLETION_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_PREREAD);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	/* Advances the host tail only after both DMA slots are device-owned. */
	previous = transport->rx_tail;
	transport->rx_tail = (uint16_t)((transport->rx_tail + 1U) &
	    (INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT - 1U));
	transport->rx_pending = 0U;
	credit = transport->rx_tail == 0U ?
	    INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT - 1U :
	    transport->rx_tail - 1U;
	credit &= (uint16_t)~7U;
	if (credit == transport->rx_last_credit)
		return INTEL_AX211_TRANSPORT_OK;

	/* Reports only a complete group of eight replenished descriptors. */
	result = ax211_csr_write(transport, AX211_RFH_Q0_FRBDCB_WIDX_TRG,
	    credit);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->rx_tail = previous;
		transport->rx_pending = 1U;
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	transport->rx_last_credit = credit;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Reserves and DMA-publishes one command without ringing its doorbell.
 */
int
intel_ax211_transport_command_prepare_inline(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command,
	const void *payload,
	size_t payload_length,
	struct intel_ax211_ring_token *token)
{
	struct intel_ax211_ring_token reserved;
	struct intel_ax211_tfd_buffer buffer[2];
	uint8_t *slot;
	uint8_t *tfd;
	uint64_t slot_address;
	size_t header_size;
	size_t slot_offset;
	size_t tfd_offset;
	size_t total_length;
	size_t buffer_count;
	int core_result;
	int result;

	if (!ax211_transport_valid(transport) || command == NULL || token == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->rings_initialized || !transport->interrupts_enabled ||
	    transport->quiesced || transport->failed ||
	    transport->command_prepared || transport->command_reset_required)
		return INTEL_AX211_TRANSPORT_ORDER;
	if ((payload == NULL && payload_length != 0U) ||
	    payload_length > INTEL_AX211_TRANSPORT_COMMAND_INLINE_PAYLOAD_MAX)
		return INTEL_AX211_TRANSPORT_INVALID;

	/* Reserves a stable queue token before selecting its DMA slots. */
	core_result = intel_ax211_ring_reserve(&transport->command_ring,
	    &reserved);
	if (core_result == INTEL_AX211_FULL)
		return INTEL_AX211_TRANSPORT_FULL;
	if (core_result != INTEL_AX211_OK)
		return INTEL_AX211_TRANSPORT_INVALID;
	slot_offset = (size_t)reserved.index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE;
	tfd_offset = (size_t)reserved.index * INTEL_AX211_TFD_SIZE;
	slot = transport->memory.command_slots + slot_offset;
	tfd = transport->memory.command_tfd + tfd_offset;

	/* API 89 rejects the original four-byte group-zero command form.  Legacy
	 * logical commands are therefore carried by LONG_GROUP on the wire, just
	 * as the pinned OpenBSD implementation does for firmware API >= 50. */
	memset(slot, 0, INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE);
	header_size = INTEL_AX211_WIDE_COMMAND_HEADER_SIZE;
	if (command->group == AX211_COMMAND_GROUP_LEGACY) {
		struct intel_ax211_command_id wire_command;

		wire_command = *command;
		wire_command.group = AX211_COMMAND_GROUP_LONG;
		wire_command.version = 0U;
		core_result = intel_ax211_wide_command_encode(slot,
		    &wire_command, (uint16_t)payload_length, &reserved);
	} else
		core_result = intel_ax211_wide_command_encode(slot, command,
		    (uint16_t)payload_length, &reserved);
	if (core_result != INTEL_AX211_OK) {
		ax211_command_rollback(transport);
		return INTEL_AX211_TRANSPORT_INVALID;
	}
	if (payload_length != 0U)
		memcpy(slot + header_size, payload, payload_length);

	/* Encodes one TFD pointing at the stable command slot. */
	if (slot_offset > UINT64_MAX -
	    transport->memory.command_slots_device_address) {
		memset(slot, 0, INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE);
		ax211_command_rollback(transport);
		return INTEL_AX211_TRANSPORT_INVALID;
	}
	slot_address = transport->memory.command_slots_device_address +
	    (uint64_t)slot_offset;
	total_length = header_size + payload_length;
	buffer[0].address = slot_address;
	buffer[0].length = (uint16_t)total_length;
	buffer_count = 1U;
	if (total_length > AX211_COMMAND_FIRST_TRANSFER_SIZE) {
		buffer[0].length = AX211_COMMAND_FIRST_TRANSFER_SIZE;
		buffer[1].address = slot_address +
		    AX211_COMMAND_FIRST_TRANSFER_SIZE;
		buffer[1].length = (uint16_t)(total_length -
		    AX211_COMMAND_FIRST_TRANSFER_SIZE);
		buffer_count = 2U;
	}
	core_result = intel_ax211_tfd_encode(tfd, buffer, buffer_count);
	if (core_result != INTEL_AX211_OK) {
		memset(slot, 0, INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE);
		ax211_command_rollback(transport);
		return INTEL_AX211_TRANSPORT_INVALID;
	}

	/* Publishes command bytes before the TFD which references them. */
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS, slot_offset,
	    header_size + payload_length,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, tfd_offset,
		    INTEL_AX211_TFD_SIZE,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		memset(slot, 0, INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE);
		memset(tfd, 0, INTEL_AX211_TFD_SIZE);
		ax211_command_rollback(transport);
		return result;
	}

	/* Exposes the token only after both DMA objects are fully visible. */
	transport->command_prepared_token = reserved;
	transport->command_prepared = 1U;
	*token = reserved;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Reserves one command and publishes a complete wide command from the
 * controller-owned 4 KiB external DMA buffer.  The single buffer remains
 * owned by this token until completion, an un-rung abort, or proven reset.
 */
int
intel_ax211_transport_command_prepare_external(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command,
	const void *payload,
	size_t payload_length,
	struct intel_ax211_ring_token *token)
{
	struct intel_ax211_ring_token reserved;
	struct intel_ax211_tfd_buffer buffer[2];
	uint8_t *external;
	uint8_t *tfd;
	size_t header_size;
	size_t tfd_offset;
	size_t total_length;
	int core_result;
	int result;
	int scrub_result;

	if (!ax211_transport_valid(transport) || command == NULL ||
	    token == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->rings_initialized || !transport->interrupts_enabled ||
	    transport->quiesced || transport->failed ||
	    transport->command_prepared || transport->command_reset_required)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (transport->command_external_active)
		return INTEL_AX211_TRANSPORT_FULL;
	if (payload == NULL || payload_length <=
	    INTEL_AX211_TRANSPORT_COMMAND_INLINE_PAYLOAD_MAX ||
	    payload_length >
	    INTEL_AX211_TRANSPORT_COMMAND_EXTERNAL_PAYLOAD_MAX)
		return INTEL_AX211_TRANSPORT_INVALID;

	core_result = intel_ax211_ring_reserve(&transport->command_ring,
	    &reserved);
	if (core_result == INTEL_AX211_FULL)
		return INTEL_AX211_TRANSPORT_FULL;
	if (core_result != INTEL_AX211_OK)
		return INTEL_AX211_TRANSPORT_INVALID;
	tfd_offset = (size_t)reserved.index * INTEL_AX211_TFD_SIZE;
	external = transport->memory.command_external;
	tfd = transport->memory.command_tfd + tfd_offset;

	memset(external, 0, transport->memory.command_external_size);
	header_size = INTEL_AX211_WIDE_COMMAND_HEADER_SIZE;
	if (command->group == AX211_COMMAND_GROUP_LEGACY) {
		struct intel_ax211_command_id wire_command;

		wire_command = *command;
		wire_command.group = AX211_COMMAND_GROUP_LONG;
		wire_command.version = 0U;
		core_result = intel_ax211_wide_command_encode(external,
		    &wire_command, (uint16_t)payload_length, &reserved);
	} else
		core_result = intel_ax211_wide_command_encode(external, command,
		    (uint16_t)payload_length, &reserved);
	if (core_result != INTEL_AX211_OK) {
		ax211_command_rollback(transport);
		return INTEL_AX211_TRANSPORT_INVALID;
	}
	memcpy(external + header_size, payload, payload_length);
	total_length = header_size + payload_length;
	buffer[0].address =
	    transport->memory.command_external_device_address;
	buffer[0].length = AX211_COMMAND_FIRST_TRANSFER_SIZE;
	buffer[1].address =
	    transport->memory.command_external_device_address +
	    AX211_COMMAND_FIRST_TRANSFER_SIZE;
	buffer[1].length = (uint16_t)(total_length -
	    AX211_COMMAND_FIRST_TRANSFER_SIZE);
	core_result = intel_ax211_tfd_encode(tfd, buffer, 2U);
	if (core_result != INTEL_AX211_OK) {
		memset(external, 0, transport->memory.command_external_size);
		ax211_command_rollback(transport);
		return INTEL_AX211_TRANSPORT_INVALID;
	}

	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL, 0U, total_length,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, tfd_offset,
		    INTEL_AX211_TFD_SIZE,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		memset(external, 0, transport->memory.command_external_size);
		memset(tfd, 0, INTEL_AX211_TFD_SIZE);
		scrub_result = ax211_command_external_scrub(transport);
		if (scrub_result == INTEL_AX211_TRANSPORT_OK)
			scrub_result = ax211_dma_sync(transport,
			    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, tfd_offset,
			    INTEL_AX211_TFD_SIZE,
			    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
		ax211_command_rollback(transport);
		if (scrub_result != INTEL_AX211_TRANSPORT_OK) {
			transport->failed = 1U;
			transport->quiesced = 1U;
			return scrub_result;
		}
		return result;
	}

	transport->command_external_token = reserved;
	transport->command_external_active = 1U;
	transport->command_prepared_token = reserved;
	transport->command_prepared = 1U;
	*token = reserved;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Rings one previously prepared command after logical metadata is installed.
 */
int
intel_ax211_transport_command_publish(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	int result;

	if (!ax211_transport_valid(transport) || token == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->rings_initialized || !transport->interrupts_enabled ||
	    transport->quiesced || transport->failed ||
	    !transport->command_prepared)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (token->queue != transport->command_prepared_token.queue ||
	    token->index != transport->command_prepared_token.index)
		return INTEL_AX211_TRANSPORT_STALE;

	/* A write error is ambiguous: hardware may have consumed the doorbell. */
	result = ax211_csr_write(transport, AX211_HBUS_TARG_WRPTR,
	    ((uint32_t)token->queue << 16) | transport->command_ring.head);
	transport->command_prepared = 0U;
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->command_reset_required = 1U;
		return INTEL_AX211_TRANSPORT_AMBIGUOUS;
	}
	return INTEL_AX211_TRANSPORT_OK;
}

/* Aborts only a DMA-visible command whose doorbell was never attempted. */
int
intel_ax211_transport_command_abort_prepared(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	size_t slot_offset;
	size_t tfd_offset;
	uint16_t newest;
	int external;
	int result;

	if (!ax211_transport_valid(transport) || token == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->command_prepared || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (token->queue != transport->command_prepared_token.queue ||
	    token->index != transport->command_prepared_token.index)
		return INTEL_AX211_TRANSPORT_STALE;
	newest = (uint16_t)((transport->command_ring.head - 1U) &
	    (transport->command_ring.capacity - 1U));
	if (newest != token->index)
		return INTEL_AX211_TRANSPORT_STALE;
	external = ax211_command_external_matches(transport, token);

	/* Scrubs and republishes the un-rung slot before making it reusable. */
	slot_offset = (size_t)token->index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE;
	tfd_offset = (size_t)token->index * INTEL_AX211_TFD_SIZE;
	memset(transport->memory.command_slots + slot_offset, 0,
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE);
	memset(transport->memory.command_tfd + tfd_offset, 0,
	    INTEL_AX211_TFD_SIZE);
	result = INTEL_AX211_TRANSPORT_OK;
	if (external)
		result = ax211_command_external_scrub(transport);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS, slot_offset,
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, tfd_offset,
		    INTEL_AX211_TFD_SIZE,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}
	ax211_command_rollback(transport);
	if (external) {
		memset(&transport->command_external_token, 0,
		    sizeof(transport->command_external_token));
		transport->command_external_active = 0U;
	}
	memset(&transport->command_prepared_token, 0,
	    sizeof(transport->command_prepared_token));
	transport->command_prepared = 0U;
	transport->command_reset_required = 0U;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Convenience wrapper for callers which need no pre-doorbell metadata step.
 */
int
intel_ax211_transport_command_submit_inline(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_command_id *command,
	const void *payload,
	size_t payload_length,
	struct intel_ax211_ring_token *token)
{
	int result;

	result = intel_ax211_transport_command_prepare_inline(transport,
	    command, payload, payload_length, token);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return result;
	return intel_ax211_transport_command_publish(transport, token);
}

/*
 * Retires one in-order command token and scrubs its reusable DMA slots.
 */
int
intel_ax211_transport_command_complete(
	struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	size_t slot_offset;
	size_t tfd_offset;
	int external;
	int core_result;
	int result;

	if (!ax211_transport_valid(transport) || token == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (transport->quiesced || transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;
	if (transport->command_prepared &&
	    token->queue == transport->command_prepared_token.queue &&
	    token->index == transport->command_prepared_token.index)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* Validates the oldest token before touching its reusable DMA slot. */
	if (transport->command_ring.used == 0U ||
	    token->queue != transport->command_ring.queue ||
	    token->index != (uint8_t)transport->command_ring.tail)
		return INTEL_AX211_TRANSPORT_STALE;
	slot_offset = (size_t)token->index *
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE;
	tfd_offset = (size_t)token->index * INTEL_AX211_TFD_SIZE;
	external = ax211_command_external_matches(transport, token);

	/* Scrubs retired host-command bytes before the slot can be reused. */
	memset(transport->memory.command_slots + slot_offset, 0,
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE);
	memset(transport->memory.command_tfd + tfd_offset, 0,
	    INTEL_AX211_TFD_SIZE);
	result = INTEL_AX211_TRANSPORT_OK;
	if (external)
		result = ax211_command_external_scrub(transport);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS, slot_offset,
	    INTEL_AX211_TRANSPORT_COMMAND_SLOT_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, tfd_offset,
		    INTEL_AX211_TFD_SIZE,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return result;
	}

	/* Retires the slot only after its scrub is DMA-visible. */
	core_result = intel_ax211_ring_complete(&transport->command_ring, token);
	if (core_result != INTEL_AX211_OK) {
		transport->failed = 1U;
		transport->quiesced = 1U;
		return INTEL_AX211_TRANSPORT_FAILED;
	}
	if (external) {
		memset(&transport->command_external_token, 0,
		    sizeof(transport->command_external_token));
		transport->command_external_active = 0U;
	}
	return INTEL_AX211_TRANSPORT_OK;
}

/* Returns the exact number of reserved, not-yet-retired hardware slots. */
size_t
intel_ax211_transport_command_pending_count(
	const struct intel_ax211_transport *transport)
{
	if (!ax211_transport_valid(transport))
		return 0U;
	return transport->command_ring.used;
}

/* Returns the oldest hardware-owned command token without retiring it. */
int
intel_ax211_transport_command_oldest(
	const struct intel_ax211_transport *transport,
	struct intel_ax211_ring_token *token)
{
	if (!ax211_transport_valid(transport) || token == NULL)
		return INTEL_AX211_TRANSPORT_INVALID;
	if (transport->command_ring.used == 0U)
		return INTEL_AX211_TRANSPORT_STALE;
	token->queue = transport->command_ring.queue;
	token->index = transport->command_ring.tail;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Scrubs and resets command DMA only after the controller reset boundary.
 */
int
intel_ax211_transport_command_after_device_reset(
	struct intel_ax211_transport *transport)
{
	int core_result;
	int result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (!transport->quiesced && !transport->failed)
		return INTEL_AX211_TRANSPORT_ORDER;

	/* The caller's reset guarantee makes every prior doorbell unobservable. */
	memset(transport->memory.command_slots, 0,
	    transport->memory.command_slots_size);
	memset(transport->memory.command_tfd, 0,
	    transport->memory.command_tfd_size);
	memset(transport->memory.command_byte_count, 0,
	    transport->memory.command_byte_count_size);
	memset(transport->memory.command_external, 0,
	    transport->memory.command_external_size);
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_SLOTS, 0U,
	    transport->memory.command_slots_size,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_TFD, 0U,
		    transport->memory.command_tfd_size,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_dma_sync(transport,
		    INTEL_AX211_TRANSPORT_DMA_COMMAND_BYTE_COUNT, 0U,
		    transport->memory.command_byte_count_size,
		    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_command_external_scrub(transport);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		transport->failed = 1U;
		return result;
	}

	core_result = intel_ax211_ring_init(&transport->command_ring,
	    AX211_COMMAND_QUEUE, INTEL_AX211_COMMAND_RING_SIZE);
	if (core_result != INTEL_AX211_OK) {
		transport->failed = 1U;
		return INTEL_AX211_TRANSPORT_FAILED;
	}
	memset(&transport->command_prepared_token, 0,
	    sizeof(transport->command_prepared_token));
	memset(&transport->command_external_token, 0,
	    sizeof(transport->command_external_token));
	transport->command_prepared = 0U;
	transport->command_external_active = 0U;
	transport->command_reset_required = 0U;
	transport->command_reset_completed = 1U;
	return INTEL_AX211_TRANSPORT_OK;
}

/*
 * Masks and acknowledges interrupts, then waits finitely for RX DMA idle.
 */
int
intel_ax211_transport_quiesce(
	struct intel_ax211_transport *transport)
{
	uint32_t flow_handler;
	uint32_t hardware;
	int commands_outstanding;
	int result;
	int unlock_result;

	if (!ax211_transport_valid(transport))
		return INTEL_AX211_TRANSPORT_INVALID;
	if (transport->quiesced && transport->rx_dma_idle)
		return INTEL_AX211_TRANSPORT_OK;

	/* Stops all new host submissions before changing device ownership. */
	transport->quiesced = 1U;
	commands_outstanding = transport->command_ring.used != 0U;
	if (transport->msix_configured) {
		result = ax211_mask_all(transport);
		transport->interrupts_enabled = 0U;
		transport->enabled_fh_causes = 0U;
		transport->enabled_hw_causes = 0U;
		if (result != INTEL_AX211_TRANSPORT_OK) {
			transport->failed = 1U;
			return result;
		}
		result = ax211_ack_raw(transport, &flow_handler, &hardware);
		if (result != INTEL_AX211_TRANSPORT_OK) {
			transport->failed = 1U;
			return result;
		}
	}

	/* A state which never published rings has no device DMA to stop. */
	if (!transport->rings_initialized) {
		transport->rx_dma_idle = 1U;
		return INTEL_AX211_TRANSPORT_OK;
	}

	/* Disables Gen3 RX DMA only inside one checked NIC ownership scope. */
	result = transport->ops->nic_lock(transport->argument);
	if (result != 0) {
		transport->failed = 1U;
		return INTEL_AX211_TRANSPORT_IO;
	}
	result = transport->ops->prph_write32(transport->argument,
	    transport->profile.umac_prph_offset +
	    AX211_RFH_RXF_DMA_CFG_GEN3, 0U);
	if (result == 0)
		result = ax211_poll_rx_idle(transport);
	else
		result = INTEL_AX211_TRANSPORT_IO;
	unlock_result = transport->ops->nic_unlock(transport->argument);
	if (result != INTEL_AX211_TRANSPORT_OK || unlock_result != 0) {
		transport->failed = 1U;
		transport->rx_dma_idle = 0U;
		if (result != INTEL_AX211_TRANSPORT_OK)
			return result;
		return INTEL_AX211_TRANSPORT_IO;
	}

	transport->rx_dma_idle = 1U;
	if (commands_outstanding) {
		transport->failed = 1U;
		return INTEL_AX211_TRANSPORT_FAILED;
	}
	return INTEL_AX211_TRANSPORT_OK;
}

/* Validates one initialized private transport state. */
static int
ax211_transport_valid(
	const struct intel_ax211_transport *transport)
{
	if (transport == NULL || !ax211_ops_valid(transport->ops))
		return 0;
	if (!ax211_profile_valid(&transport->profile))
		return 0;
	if (!ax211_memory_valid(&transport->memory))
		return 0;
	return 1;
}

/* Validates the exact P038 SO-or-SOF plus GF MMIO profile. */
static int
ax211_profile_valid(
	const struct intel_ax211_mmio_profile *profile)
{
	if (profile == NULL)
		return 0;
	if (profile->mac_type != INTEL_AX211_MMIO_MAC_SO &&
	    profile->mac_type != INTEL_AX211_MMIO_MAC_SOF)
		return 0;
	if (profile->rf_type != INTEL_AX211_MMIO_RF_GF || profile->cdb != 0U)
		return 0;
	if (profile->integrated != 0U ||
	    profile->umac_prph_offset != INTEL_AX211_MMIO_UMAC_PRPH_OFFSET)
		return 0;
	return 1;
}

/* Validates every fixed Gen3 ring allocation and command-slot address. */
static int
ax211_memory_valid(
	const struct intel_ax211_transport_ring_memory *memory)
{
	if (memory == NULL)
		return 0;
	if (memory->command_tfd == NULL ||
	    memory->command_tfd_size != AX211_COMMAND_TFD_SIZE)
		return 0;
	if (memory->command_byte_count == NULL ||
	    memory->command_byte_count_size != AX211_COMMAND_BYTE_COUNT_SIZE)
		return 0;
	if (memory->command_slots == NULL ||
	    memory->command_slots_size != AX211_COMMAND_SLOTS_SIZE ||
	    memory->command_slots_device_address == 0U ||
	    (memory->command_slots_device_address & 63U) != 0U ||
	    memory->command_slots_device_address >
	    UINT64_MAX - AX211_COMMAND_SLOTS_SIZE)
		return 0;
	if (memory->command_external == NULL ||
	    memory->command_external_size != AX211_COMMAND_EXTERNAL_SIZE ||
	    memory->command_external_device_address == 0U ||
	    (memory->command_external_device_address & 63U) != 0U ||
	    memory->command_external_device_address >
	    UINT64_MAX - AX211_COMMAND_EXTERNAL_SIZE)
		return 0;
	if (memory->rx_transfer == NULL ||
	    memory->rx_transfer_size != AX211_RX_TRANSFER_SIZE)
		return 0;
	if (memory->rx_completion == NULL ||
	    memory->rx_completion_size != AX211_RX_COMPLETION_SIZE)
		return 0;
	if (memory->rx_status == NULL ||
	    memory->rx_status_size != AX211_RX_STATUS_SIZE)
		return 0;
	return 1;
}

/* Validates every checked operation required by this transport. */
static int
ax211_ops_valid(
	const struct intel_ax211_transport_ops *ops)
{
	if (ops == NULL)
		return 0;
	if (ops->csr_read32 == NULL || ops->csr_write32 == NULL ||
	    ops->csr_write8 == NULL)
		return 0;
	if (ops->nic_lock == NULL || ops->nic_unlock == NULL ||
	    ops->prph_read32 == NULL || ops->prph_write32 == NULL)
		return 0;
	if (ops->dma_sync == NULL || ops->delay_us == NULL ||
	    ops->clock_us == NULL)
		return 0;
	return 1;
}

/* Reads one checked transport CSR. */
static int
ax211_csr_read(
	struct intel_ax211_transport *transport,
	uint32_t offset,
	uint32_t *value)
{
	if (transport->ops->csr_read32(transport->argument, offset, value) != 0)
		return INTEL_AX211_TRANSPORT_IO;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Writes one checked 32-bit transport CSR. */
static int
ax211_csr_write(
	struct intel_ax211_transport *transport,
	uint32_t offset,
	uint32_t value)
{
	if (transport->ops->csr_write32(transport->argument, offset, value) != 0)
		return INTEL_AX211_TRANSPORT_IO;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Writes one checked IVAR or interrupt-coalescing byte. */
static int
ax211_csr_write8(
	struct intel_ax211_transport *transport,
	uint32_t offset,
	uint8_t value)
{
	if (transport->ops->csr_write8(transport->argument, offset, value) != 0)
		return INTEL_AX211_TRANSPORT_IO;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Synchronizes one checked subrange of a fixed DMA object. */
static int
ax211_dma_sync(
	struct intel_ax211_transport *transport,
	enum intel_ax211_transport_dma_region region,
	size_t offset,
	size_t length,
	enum intel_ax211_transport_dma_direction direction)
{
	if (transport->ops->dma_sync(transport->argument, region, offset,
	    length, direction) != 0)
		return INTEL_AX211_TRANSPORT_IO;
	return INTEL_AX211_TRANSPORT_OK;
}

/* Masks both MSI-X cause banks in safe shutdown order. */
static int
ax211_mask_all(
	struct intel_ax211_transport *transport)
{
	int first_result;
	int second_result;

	first_result = ax211_csr_write(transport,
	    AX211_CSR_MSIX_FH_INT_MASK_AD, UINT32_MAX);
	second_result = ax211_csr_write(transport,
	    AX211_CSR_MSIX_HW_INT_MASK_AD, UINT32_MAX);
	if (first_result != INTEL_AX211_TRANSPORT_OK)
		return first_result;
	return second_result;
}

/* Makes a best-effort mask after partial IVAR programming. */
static void
ax211_mask_all_best_effort(
	struct intel_ax211_transport *transport)
{
	(void)ax211_csr_write(transport, AX211_CSR_MSIX_FH_INT_MASK_AD,
	    UINT32_MAX);
	(void)ax211_csr_write(transport, AX211_CSR_MSIX_HW_INT_MASK_AD,
	    UINT32_MAX);
}

/* Captures and W1C-acknowledges both raw MSI-X cause registers. */
static int
ax211_ack_raw(
	struct intel_ax211_transport *transport,
	uint32_t *flow_handler,
	uint32_t *hardware)
{
	int result;

	result = ax211_csr_read(transport, AX211_CSR_MSIX_FH_INT_CAUSES_AD,
	    flow_handler);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return result;
	result = ax211_csr_read(transport, AX211_CSR_MSIX_HW_INT_CAUSES_AD,
	    hardware);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return result;
	result = ax211_csr_write(transport, AX211_CSR_MSIX_FH_INT_CAUSES_AD,
	    *flow_handler);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return result;
	return ax211_csr_write(transport, AX211_CSR_MSIX_HW_INT_CAUSES_AD,
	    *hardware);
}

/* Programs the exact single-vector RX and non-RX IVAR entries. */
static int
ax211_configure_msix_routes(
	struct intel_ax211_transport *transport)
{
	size_t index;
	int result;

	/* Maps command and data RX causes first. */
	result = ax211_csr_write8(transport, AX211_CSR_MSIX_RX_IVAR_AD_REG,
	    AX211_MSIX_VECTOR | AX211_MSIX_NON_AUTO_CLEAR);
	if (result == INTEL_AX211_TRANSPORT_OK)
		result = ax211_csr_write8(transport,
		    AX211_CSR_MSIX_RX_IVAR_AD_REG + 1U,
		    AX211_MSIX_VECTOR | AX211_MSIX_NON_AUTO_CLEAR);
	if (result != INTEL_AX211_TRANSPORT_OK)
		return result;

	/* Maps every admitted flow-handler cause to vector zero. */
	for (index = 0U;
	     index < sizeof(ax211_fh_ivar_cause);
	     index++) {
		result = ax211_csr_write8(transport,
		    AX211_CSR_MSIX_IVAR_AD_REG + ax211_fh_ivar_cause[index],
		    AX211_MSIX_VECTOR | AX211_MSIX_NON_AUTO_CLEAR);
		if (result != INTEL_AX211_TRANSPORT_OK)
			return result;
	}

	/* Maps every admitted hardware cause to vector zero. */
	for (index = 0U;
	     index < sizeof(ax211_hw_ivar_cause);
	     index++) {
		result = ax211_csr_write8(transport,
		    AX211_CSR_MSIX_IVAR_AD_REG + ax211_hw_ivar_cause[index],
		    AX211_MSIX_VECTOR | AX211_MSIX_NON_AUTO_CLEAR);
		if (result != INTEL_AX211_TRANSPORT_OK)
			return result;
	}
	return INTEL_AX211_TRANSPORT_OK;
}

/* Validates one fixed-size RX buffer mapping before DMA publication. */
static int
ax211_rx_descriptor_valid(
	uint16_t index,
	uint64_t device_address)
{
	if (index >= INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_COUNT ||
	    device_address == 0U || (device_address & 0xfffU) != 0U ||
	    device_address > UINT64_MAX - 4095U)
		return 0;
	return 1;
}

/* Encodes and synchronizes one caller-validated RX transfer descriptor. */
static int
ax211_publish_rx_descriptor(
	struct intel_ax211_transport *transport,
	uint16_t index,
	uint64_t device_address)
{
	uint8_t *descriptor;
	size_t offset;
	int core_result;
	int result;

	/* Keeps the private boundary safe if an internal caller is added later. */
	if (!ax211_rx_descriptor_valid(index, device_address))
		return INTEL_AX211_TRANSPORT_INVALID;

	/* Encodes the device address into its stable ring slot. */
	offset = (size_t)index * INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_SIZE;
	descriptor = transport->memory.rx_transfer + offset;
	core_result = intel_ax211_rx_transfer_descriptor_encode(descriptor,
	    index, device_address);
	if (core_result != INTEL_AX211_OK)
		return INTEL_AX211_TRANSPORT_INVALID;

	/* Makes the complete descriptor visible before marking it published. */
	result = ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_RX_TRANSFER, offset,
	    INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_SIZE,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
	if (result != INTEL_AX211_TRANSPORT_OK) {
		memset(descriptor, 0, INTEL_AX211_TRANSPORT_RX_DESCRIPTOR_SIZE);
		ax211_set_published(transport, index, 0);
		return result;
	}
	ax211_set_published(transport, index, 1);
	return INTEL_AX211_TRANSPORT_OK;
}

/* Updates one host-owned RX descriptor publication bit. */
static void
ax211_set_published(
	struct intel_ax211_transport *transport,
	uint16_t index,
	int published)
{
	uint8_t bit;
	uint8_t *byte;

	bit = (uint8_t)(1U << (index & 7U));
	byte = &transport->rx_published[index >> 3];
	if (published)
		*byte |= bit;
	else
		*byte &= (uint8_t)~bit;
}

/* Tests one host-owned RX descriptor publication bit. */
static int
ax211_is_published(
	const struct intel_ax211_transport *transport,
	uint16_t index)
{
	uint8_t bit;

	bit = (uint8_t)(1U << (index & 7U));
	return (transport->rx_published[index >> 3] & bit) != 0U;
}

/* Requires every exact Gen3 RX descriptor before activation. */
static int
ax211_all_rx_published(
	const struct intel_ax211_transport *transport)
{
	size_t index;

	/* Verifies every full publication byte. */
	for (index = 0U;
	     index < sizeof(transport->rx_published);
	     index++) {
		if (transport->rx_published[index] != 0xffU)
			return 0;
	}
	return 1;
}

/* Decodes one little-endian status word without alignment assumptions. */
static uint16_t
ax211_get_le16(
	const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] |
	    ((uint16_t)bytes[1] << 8));
}

/* Rolls back only the newest command reservation before its doorbell. */
static void
ax211_command_rollback(
	struct intel_ax211_transport *transport)
{
	if (transport->command_ring.used == 0U)
		return;
	transport->command_ring.head = (uint16_t)(
	    (transport->command_ring.head - 1U) &
	    (transport->command_ring.capacity - 1U));
	transport->command_ring.used--;
}

/* Tests whether the one external DMA buffer still belongs to this token. */
static int
ax211_command_external_matches(
	const struct intel_ax211_transport *transport,
	const struct intel_ax211_ring_token *token)
{
	if (!transport->command_external_active)
		return 0;
	return token->queue == transport->command_external_token.queue &&
	    token->index == transport->command_external_token.index;
}

/* Scrubs the full external command object and publishes the scrub to DMA. */
static int
ax211_command_external_scrub(
	struct intel_ax211_transport *transport)
{
	memset(transport->memory.command_external, 0,
	    transport->memory.command_external_size);
	return ax211_dma_sync(transport,
	    INTEL_AX211_TRANSPORT_DMA_COMMAND_EXTERNAL, 0U,
	    transport->memory.command_external_size,
	    INTEL_AX211_TRANSPORT_DMA_PREWRITE);
}

/* Polls Gen3 RX-DMA idle through one observable 10-ms deadline. */
static int
ax211_poll_rx_idle(
	struct intel_ax211_transport *transport)
{
	uint64_t start;
	uint64_t deadline;
	uint64_t previous;
	uint64_t current;
	uint32_t status;
	uint32_t iteration;
	int result;

	/* Creates a checked absolute RX-idle deadline. */
	if (transport->ops->clock_us(transport->argument, &start) != 0)
		return INTEL_AX211_TRANSPORT_CLOCK;
	if (start > UINT64_MAX - AX211_RX_IDLE_TIMEOUT_US)
		return INTEL_AX211_TRANSPORT_CLOCK;
	deadline = start + AX211_RX_IDLE_TIMEOUT_US;
	if (transport->ops->trace_deadline != NULL)
		transport->ops->trace_deadline(transport->argument,
		    INTEL_AX211_TRANSPORT_WAIT_RX_IDLE, start, deadline);

	/* Polls with independent elapsed-time and iteration bounds. */
	previous = start;
	for (iteration = 0U;
	     iteration < AX211_RX_IDLE_TIMEOUT_US / AX211_RX_IDLE_POLL_US + 2U;
	     iteration++) {
		result = transport->ops->prph_read32(transport->argument,
		    transport->profile.umac_prph_offset +
		    AX211_RFH_GEN_STATUS_GEN3, &status);
		if (result != 0)
			return INTEL_AX211_TRANSPORT_IO;
		if ((status & AX211_RXF_DMA_IDLE) != 0U)
			return INTEL_AX211_TRANSPORT_OK;
		if (transport->ops->clock_us(transport->argument, &current) != 0)
			return INTEL_AX211_TRANSPORT_CLOCK;
		if (current < previous)
			return INTEL_AX211_TRANSPORT_CLOCK;
		if (current >= deadline)
			return INTEL_AX211_TRANSPORT_TIMEOUT;
		if (transport->ops->delay_us(transport->argument,
		    AX211_RX_IDLE_POLL_US) != 0)
			return INTEL_AX211_TRANSPORT_IO;
		if (transport->ops->clock_us(transport->argument, &current) != 0)
			return INTEL_AX211_TRANSPORT_CLOCK;
		if (current <= previous)
			return INTEL_AX211_TRANSPORT_CLOCK;
		previous = current;
	}
	return INTEL_AX211_TRANSPORT_TIMEOUT;
}
