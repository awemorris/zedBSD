/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private MMIO implementation
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

#include "intel-ax211-mmio.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AX211_CSR_HW_IF_CONFIG_REG 0x000U
#define AX211_CSR_RESET 0x020U
#define AX211_CSR_GP_CNTRL 0x024U
#define AX211_CSR_GIO_REG 0x03cU
#define AX211_CSR_MBOX_SET_REG 0x088U
#define AX211_CSR_LTR_LONG_VAL_AD 0x0d4U
#define AX211_CSR_GIO_CHICKEN_BITS 0x100U
#define AX211_CSR_CTXT_INFO_ADDR 0x118U
#define AX211_CSR_IML_DATA_ADDR 0x120U
#define AX211_CSR_IML_SIZE_ADDR 0x128U
#define AX211_CSR_DBG_HPET_MEM_REG 0x240U
#define AX211_CSR_DBG_LINK_PWR_MGMT_REG 0x250U
#define AX211_CSR_MAC_ADDRESS_BASE 0x380U
#define AX211_CSR_MAC_ADDRESS0_OTP (AX211_CSR_MAC_ADDRESS_BASE + 0x00U)
#define AX211_CSR_MAC_ADDRESS1_OTP (AX211_CSR_MAC_ADDRESS_BASE + 0x04U)
#define AX211_CSR_MAC_ADDRESS0_STRAP (AX211_CSR_MAC_ADDRESS_BASE + 0x08U)
#define AX211_CSR_MAC_ADDRESS1_STRAP (AX211_CSR_MAC_ADDRESS_BASE + 0x0cU)

#define AX211_HW_IF_HAP_WAKE_L1A 0x00080000U
#define AX211_HW_IF_NIC_READY 0x00400000U
#define AX211_HW_IF_PREPARE 0x08000000U
#define AX211_HW_IF_ENABLE_PME 0x10000000U
#define AX211_MBOX_OS_ALIVE 0x00000020U
#define AX211_RESET_SW 0x00000080U
#define AX211_RESET_MASTER_DISABLED 0x00000100U
#define AX211_RESET_STOP_MASTER 0x00000200U
#define AX211_LINK_POWER_MANAGEMENT_DISABLED 0x80000000U
#define AX211_GP_MAC_CLOCK_READY 0x00000001U
#define AX211_GP_INIT_DONE 0x00000004U
#define AX211_GP_MAC_ACCESS_REQ 0x00000008U
#define AX211_GP_GOING_TO_SLEEP 0x00000010U
#define AX211_GIO_L0S_DISABLED 0x00000002U
#define AX211_GIO_L1A_NO_L0S_RX 0x00800000U
#define AX211_HPET_WAIT_THRESHOLD 0xffff0000U
#define AX211_AUTO_FUNC_BOOT 0x00000002U

#define AX211_LTR_NO_SNOOP_BOOTSTRAP 0x80000000U
#define AX211_LTR_NO_SNOOP_SCALE_US 0x02000000U
#define AX211_LTR_NO_SNOOP_250_US 0x00fa0000U
#define AX211_LTR_SNOOP_BOOTSTRAP 0x00008000U
#define AX211_LTR_SNOOP_SCALE_US 0x00000800U
#define AX211_LTR_SNOOP_250_US 0x000000faU
#define AX211_LTR_BOOTSTRAP                                                    \
	(AX211_LTR_NO_SNOOP_BOOTSTRAP | AX211_LTR_NO_SNOOP_SCALE_US |          \
	 AX211_LTR_NO_SNOOP_250_US | AX211_LTR_SNOOP_BOOTSTRAP |               \
	 AX211_LTR_SNOOP_SCALE_US | AX211_LTR_SNOOP_250_US)

#define AX211_UREG_CPU_INIT_RUN 0xa05c44U
#define AX211_PRPH_MAX_ADDRESS 0x00ffffffU

#define AX211_HW_READY_TIMEOUT_US 50U
#define AX211_HW_READY_POLL_US 10U
#define AX211_PREPARE_SETTLE_US 1000U
#define AX211_PREPARE_RETRY_POLL_US 200U
#define AX211_PREPARE_TOTAL_POLL_US 150000U
#define AX211_PREPARE_RETRY_DELAY_US 25000U
#define AX211_PREPARE_RETRY_COUNT 10U
#define AX211_SW_RESET_DELAY_US 5000U
#define AX211_APM_CLOCK_TIMEOUT_US 25000U
#define AX211_NIC_REQUEST_SETTLE_US 2U
#define AX211_NIC_OWNERSHIP_TIMEOUT_US 150000U
#define AX211_STOP_PREPARE_DELAY_US 1000U
#define AX211_STOP_LINK_DELAY_US 5000U
#define AX211_STOP_MASTER_TIMEOUT_US 100U
#define AX211_GENERAL_POLL_US 10U
#define AX211_MAC_ADDRESS_SIZE 6U

static int ax211_profile_valid(const struct intel_ax211_mmio_profile *profile);
static int ax211_mmio_valid(const struct intel_ax211_mmio *mmio);
static int ax211_set_hw_ready(struct intel_ax211_mmio *mmio);
static int ax211_csr_set_bits(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t bits);
static int ax211_csr_read(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t *value);
static int ax211_csr_write(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t value);
static int ax211_poll_csr(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t expected, uint32_t mask, uint32_t timeout_us, uint32_t step_us, enum intel_ax211_mmio_wait wait);
static int ax211_clock(struct intel_ax211_mmio *mmio, uint64_t *time_us);
static int ax211_delay(struct intel_ax211_mmio *mmio, uint32_t duration_us);
static int ax211_prepare_fallback(struct intel_ax211_mmio *mmio);
static int ax211_csr_clear_bits(struct intel_ax211_mmio *mmio, uint32_t offset, uint32_t bits);
static void ax211_remember_error(int *first_error, int result);
static void ax211_scrub(void *memory, size_t length);
static int ax211_read_mac_words(struct intel_ax211_mmio *mmio, uint32_t first_offset, uint32_t second_offset, uint8_t address[AX211_MAC_ADDRESS_SIZE]);
static void ax211_decode_mac_words(uint32_t first, uint32_t second, uint8_t address[AX211_MAC_ADDRESS_SIZE]);
static int ax211_mac_valid(const uint8_t address[AX211_MAC_ADDRESS_SIZE]);
static int ax211_publish_address(struct intel_ax211_mmio *mmio, uint32_t offset, uint64_t address);

/*
 * Initializes one exact-device MMIO state without touching hardware.
 */
int
drv_intel_ax211_mmio_init(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_ops *ops,
	void *argument,
	const struct intel_ax211_mmio_profile *profile)
{
	/* Handles the mmio availability. */
	if (mmio == NULL || ops == NULL || profile == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the csr read32 availability. */
	if (ops->csr_read32 == NULL || ops->csr_write32 == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the prph read32 availability. */
	if (ops->prph_read32 == NULL || ops->prph_write32 == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the delay us availability. */
	if (ops->delay_us == NULL || ops->clock_us == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Checks the ax211 profile valid result. */
	if (!ax211_profile_valid(profile))
		return INTEL_AX211_MMIO_INVALID;

	/* Initializes a private, unowned device state. */
	memset(mmio, 0, sizeof(*mmio));
	mmio->ops = ops;
	mmio->argument = argument;
	mmio->profile = *profile;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Acquires PCI ownership through the bounded AX210-family prepare sequence.
 */
int
drv_intel_ax211_mmio_prepare_card_hw(
	struct intel_ax211_mmio *mmio)
{
	int result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio))
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (mmio->nic_lock_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Starts each attempt from an unpublished state. */
	mmio->prepared = 0;
	mmio->reset_done = 0;
	mmio->apm_ready = 0;
	mmio->master_disable_timed_out = 0;

	/* Tries the ordinary ownership handshake first. */
	result = ax211_set_hw_ready(mmio);

	/* Checks the operation result. */
	if (result == INTEL_AX211_MMIO_OK) {
		mmio->prepared = 1;

		/* Returns the computed result. */
		return INTEL_AX211_MMIO_OK;
	}

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_TIMEOUT)
		return result;

	/* Runs the finite wake-and-retry fallback. */
	result = ax211_prepare_fallback(mmio);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	mmio->prepared = 1;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Applies the AX210-family software reset and its required five-ms delay.
 */
int
drv_intel_ax211_mmio_sw_reset(
	struct intel_ax211_mmio *mmio)
{
	int result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio))
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (!mmio->prepared || mmio->nic_lock_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Invalidates every post-reset readiness assertion. */
	mmio->reset_done = 0;
	mmio->apm_ready = 0;

	/* Requests the AX210-family CSR reset. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_RESET, AX211_RESET_SW);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Observes the documented reset settling time. */
	result = ax211_delay(mmio, AX211_SW_RESET_DELAY_US);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	mmio->reset_done = 1;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Powers the MAC clock through the bounded AX210-family APM sequence.
 */
int
drv_intel_ax211_mmio_apm_init(
	struct intel_ax211_mmio *mmio)
{
	int result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio))
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (!mmio->prepared || !mmio->reset_done)
		return INTEL_AX211_MMIO_ORDER;

	/* Handles the mmio condition. */
	if (mmio->nic_lock_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Keeps L0s from racing L1A receive wakeup. */
	mmio->apm_ready = 0;
	result = ax211_csr_set_bits(mmio, AX211_CSR_GIO_CHICKEN_BITS,
				    AX211_GIO_L1A_NO_L0S_RX);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Raises the host-processor-event wait threshold. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_DBG_HPET_MEM_REG,
				    AX211_HPET_WAIT_THRESHOLD);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Permits management traffic to wake the PCIe link. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_HW_IF_CONFIG_REG,
				    AX211_HW_IF_HAP_WAKE_L1A);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Disables unsupported L0s without disabling L1. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_GIO_REG,
				    AX211_GIO_L0S_DISABLED);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Moves the adapter from D0U into its active power state. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_GP_CNTRL,
				    AX211_GP_INIT_DONE);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Waits only for the clock indication required by PRPH access. */
	result = ax211_poll_csr(
		mmio, AX211_CSR_GP_CNTRL, AX211_GP_MAC_CLOCK_READY,
		AX211_GP_MAC_CLOCK_READY, AX211_APM_CLOCK_TIMEOUT_US,
		AX211_GENERAL_POLL_US, INTEL_AX211_MMIO_WAIT_APM_CLOCK);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	mmio->apm_ready = 1;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Quiesces AX210-family bus-master activity and finishes with a software reset.
 */
int
drv_intel_ax211_mmio_stop(
	struct intel_ax211_mmio *mmio)
{
	int first_error;
	int result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio))
		return INTEL_AX211_MMIO_INVALID;

	/* Makes every failure fail closed to later MMIO/PRPH users. */
	mmio->nic_lock_depth = 0U;
	mmio->prepared = 0;
	mmio->reset_done = 0;
	mmio->apm_ready = 0;
	first_error = INTEL_AX211_MMIO_OK;

	/* Releases a redundant or leaked request to keep the MAC awake. */
	result = ax211_csr_clear_bits(mmio, AX211_CSR_GP_CNTRL,
				      AX211_GP_MAC_ACCESS_REQ);
	ax211_remember_error(&first_error, result);

	/* Uses the exact pre-BZ OpenBSD link-power stop preamble. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_DBG_LINK_PWR_MGMT_REG,
				    AX211_LINK_POWER_MANAGEMENT_DISABLED);
	ax211_remember_error(&first_error, result);
	result = ax211_csr_set_bits(mmio, AX211_CSR_HW_IF_CONFIG_REG,
				    AX211_HW_IF_PREPARE |
					    AX211_HW_IF_ENABLE_PME);
	ax211_remember_error(&first_error, result);
	result = ax211_delay(mmio, AX211_STOP_PREPARE_DELAY_US);
	ax211_remember_error(&first_error, result);
	result = ax211_csr_clear_bits(mmio, AX211_CSR_DBG_LINK_PWR_MGMT_REG,
				      AX211_LINK_POWER_MANAGEMENT_DISABLED);
	ax211_remember_error(&first_error, result);
	result = ax211_delay(mmio, AX211_STOP_LINK_DELAY_US);
	ax211_remember_error(&first_error, result);

	/* Stops bus-master DMA and exposes the finite 100-us deadline. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_RESET,
				    AX211_RESET_STOP_MASTER);
	ax211_remember_error(&first_error, result);
	result = ax211_poll_csr(
		mmio, AX211_CSR_RESET, AX211_RESET_MASTER_DISABLED,
		AX211_RESET_MASTER_DISABLED, AX211_STOP_MASTER_TIMEOUT_US,
		AX211_GENERAL_POLL_US, INTEL_AX211_MMIO_WAIT_MASTER_DISABLED);

	/*
 * Linux and OpenBSD both treat this 100-us indication deadline as a
	 * warning and continue the mandatory software reset.  The PCI owner has
	 * already disabled bus mastering before a DMA-owning stop reaches here;
	 * keep all actual MMIO, clock, and reset failures fatal. */
	if (result == INTEL_AX211_MMIO_TIMEOUT)
		mmio->master_disable_timed_out = 1;
	else
		ax211_remember_error(&first_error, result);

	/* Returns the adapter to D0U even when an earlier stop stage failed. */
	result = ax211_csr_clear_bits(mmio, AX211_CSR_GP_CNTRL,
				      AX211_GP_INIT_DONE);
	ax211_remember_error(&first_error, result);

	/*
 * Invalidates the stopped firmware generation with the required reset.
	 */
	result = ax211_csr_set_bits(mmio, AX211_CSR_RESET, AX211_RESET_SW);
	ax211_remember_error(&first_error, result);
	result = ax211_delay(mmio, AX211_SW_RESET_DELAY_US);
	ax211_remember_error(&first_error, result);

	/* Returns the computed result. */
	return first_error;
}

/*
 * Acquires nested NIC ownership with a 150-ms outer bound.
 */
int
drv_intel_ax211_mmio_nic_lock(
	struct intel_ax211_mmio *mmio)
{
	int clear_result;
	int result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio))
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (!mmio->apm_ready)
		return INTEL_AX211_MMIO_ORDER;

	/* Reuses an ownership scope already held by this state. */
	if (mmio->nic_lock_depth != 0U) {
		/* Handles the mmio condition. */
		if (mmio->nic_lock_depth == (unsigned int)-1)
			return INTEL_AX211_MMIO_INVALID;
		mmio->nic_lock_depth++;

		/* Returns the computed result. */
		return INTEL_AX211_MMIO_OK;
	}

	/* Requests access and gives the power controller time to react. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_GP_CNTRL,
				    AX211_GP_MAC_ACCESS_REQ);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	result = ax211_delay(mmio, AX211_NIC_REQUEST_SETTLE_US);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK) {
		clear_result = ax211_csr_clear_bits(mmio, AX211_CSR_GP_CNTRL,
						    AX211_GP_MAC_ACCESS_REQ);

		/* Handles the clear result condition. */
		if (clear_result != INTEL_AX211_MMIO_OK) {
			mmio->apm_ready = 0;

			/* Returns the computed result. */
			return clear_result;
		}

		/* Returns the computed result. */
		return result;
	}

	/* Requires a running clock and a device which is not going to sleep. */
	result = ax211_poll_csr(
		mmio, AX211_CSR_GP_CNTRL, AX211_GP_MAC_CLOCK_READY,
		AX211_GP_MAC_CLOCK_READY | AX211_GP_GOING_TO_SLEEP,
		AX211_NIC_OWNERSHIP_TIMEOUT_US, AX211_GENERAL_POLL_US,
		INTEL_AX211_MMIO_WAIT_NIC_OWNERSHIP);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK) {
		clear_result = ax211_csr_clear_bits(mmio, AX211_CSR_GP_CNTRL,
						    AX211_GP_MAC_ACCESS_REQ);

		/* Handles the clear result condition. */
		if (clear_result != INTEL_AX211_MMIO_OK) {
			mmio->apm_ready = 0;

			/* Returns the computed result. */
			return clear_result;
		}

		/* Returns the computed result. */
		return result;
	}

	mmio->nic_lock_depth = 1U;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Releases one nested NIC ownership reference.
 */
int
drv_intel_ax211_mmio_nic_unlock(
	struct intel_ax211_mmio *mmio)
{
	int result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio))
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (mmio->nic_lock_depth == 0U)
		return INTEL_AX211_MMIO_NOT_OWNER;

	/* Retains hardware ownership for an outer caller. */
	mmio->nic_lock_depth--;

	/* Handles the mmio condition. */
	if (mmio->nic_lock_depth != 0U)
		return INTEL_AX211_MMIO_OK;

	/* Drops the hardware request at the outermost boundary. */
	result = ax211_csr_clear_bits(mmio, AX211_CSR_GP_CNTRL,
				      AX211_GP_MAC_ACCESS_REQ);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK) {
		mmio->apm_ready = 0;

		/* Returns the computed result. */
		return result;
	}

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Reads the fused station address transactionally under NIC ownership.
 */
int
drv_intel_ax211_mmio_read_mac(
	struct intel_ax211_mmio *mmio,
	uint8_t mac_address[AX211_MAC_ADDRESS_SIZE])
{
	uint8_t candidate[AX211_MAC_ADDRESS_SIZE];
	int result;
	int unlock_result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio) || mac_address == NULL)
		return INTEL_AX211_MMIO_INVALID;

	memset(candidate, 0, sizeof(candidate));
	result = drv_intel_ax211_mmio_nic_lock(mmio);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK) {
		ax211_scrub(candidate, sizeof(candidate));

		/* Returns the computed result. */
		return result;
	}

	/* Prefers the OEM strap value when it is a usable unicast address. */
	result = ax211_read_mac_words(mmio, AX211_CSR_MAC_ADDRESS0_STRAP,
				      AX211_CSR_MAC_ADDRESS1_STRAP, candidate);

	/* Checks the ax211 mac valid result. */
	if (result == INTEL_AX211_MMIO_OK && !ax211_mac_valid(candidate)) {
		ax211_scrub(candidate, sizeof(candidate));
		result = ax211_read_mac_words(mmio, AX211_CSR_MAC_ADDRESS0_OTP,
					      AX211_CSR_MAC_ADDRESS1_OTP,
					      candidate);
	}

	/* Checks the ax211 mac valid result. */
	if (result == INTEL_AX211_MMIO_OK && !ax211_mac_valid(candidate))
		result = INTEL_AX211_MMIO_INVALID;

	/*
 * Never publishes an address until its ownership reference is released.
	 */
	unlock_result = drv_intel_ax211_mmio_nic_unlock(mmio);

	/* Handles the unlock result condition. */
	if (unlock_result != INTEL_AX211_MMIO_OK)
		result = unlock_result;

	/* Checks the operation result. */
	if (result == INTEL_AX211_MMIO_OK)
		memcpy(mac_address, candidate, sizeof(candidate));
	ax211_scrub(candidate, sizeof(candidate));

	/* Returns the computed result. */
	return result;
}

/*
 * Reads one AX210-family PRPH register while NIC ownership is held.
 */
int
drv_intel_ax211_mmio_prph_read32(
	struct intel_ax211_mmio *mmio,
	uint32_t address,
	uint32_t *value)
{
	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio) || value == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (mmio->nic_lock_depth == 0U)
		return INTEL_AX211_MMIO_NOT_OWNER;

	/* Handles the address condition. */
	if (address > AX211_PRPH_MAX_ADDRESS)
		return INTEL_AX211_MMIO_INVALID;

	/* Checks the prph read32 result. */
	if (mmio->ops->prph_read32(mmio->argument, address, value) != 0)
		return INTEL_AX211_MMIO_IO;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Writes one AX210-family PRPH register while NIC ownership is held.
 */
int
drv_intel_ax211_mmio_prph_write32(
	struct intel_ax211_mmio *mmio,
	uint32_t address,
	uint32_t value)
{
	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio))
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (mmio->nic_lock_depth == 0U)
		return INTEL_AX211_MMIO_NOT_OWNER;

	/* Handles the address condition. */
	if (address > AX211_PRPH_MAX_ADDRESS)
		return INTEL_AX211_MMIO_INVALID;

	/* Checks the prph write32 result. */
	if (mmio->ops->prph_write32(mmio->argument, address, value) != 0)
		return INTEL_AX211_MMIO_IO;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/*
 * Publishes preallocated Gen3 context and IML addresses, then starts UMAC.
 */
int
drv_intel_ax211_mmio_publish_gen3(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_boot *boot)
{
	uint32_t boot_control;
	int result;
	int unlock_result;

	/* Checks the ax211 mmio valid result. */
	if (!ax211_mmio_valid(mmio) || boot == NULL)
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the mmio condition. */
	if (!mmio->apm_ready || mmio->nic_lock_depth != 0U)
		return INTEL_AX211_MMIO_ORDER;

	/* Handles the boot condition. */
	if (boot->context_address == 0U || boot->iml_address == 0U)
		return INTEL_AX211_MMIO_INVALID;

	/* Handles the boot condition. */
	if (boot->iml_size != INTEL_AX211_MMIO_IML_SIZE)
		return INTEL_AX211_MMIO_INVALID;

	/*
 * Publishes the context physical address low word before its high word.
	 */
	result = ax211_publish_address(mmio, AX211_CSR_CTXT_INFO_ADDR,
				       boot->context_address);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Publishes the IML physical address before its byte length. */
	result = ax211_publish_address(mmio, AX211_CSR_IML_DATA_ADDR,
				       boot->iml_address);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	result = ax211_csr_write(mmio, AX211_CSR_IML_SIZE_ADDR, boot->iml_size);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/*
 * Enables automatic function boot only after every boot input is
	 * visible. */
	result =
		ax211_csr_read(mmio, AX211_CSR_HW_IF_CONFIG_REG, &boot_control);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	boot_control |= AX211_AUTO_FUNC_BOOT;
	result =
		ax211_csr_write(mmio, AX211_CSR_HW_IF_CONFIG_REG, boot_control);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Acquires ownership before the LTR bootstrap and UMAC PRPH write. */
	result = drv_intel_ax211_mmio_nic_lock(mmio);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Applies the non-integrated SO/GF bootstrap latency contract. */
	result = ax211_csr_write(mmio, AX211_CSR_LTR_LONG_VAL_AD,
				 AX211_LTR_BOOTSTRAP);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK) {
		(void)drv_intel_ax211_mmio_nic_unlock(mmio);

		/* Returns the computed result. */
		return result;
	}

	/* Starts UMAC through the supplied, validated PRPH aperture offset. */
	result = drv_intel_ax211_mmio_prph_write32(
		mmio, mmio->profile.umac_prph_offset + AX211_UREG_CPU_INIT_RUN,
		1U);
	unlock_result = drv_intel_ax211_mmio_nic_unlock(mmio);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Handles the unlock result condition. */
	if (unlock_result != INTEL_AX211_MMIO_OK)
		return unlock_result;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/* Validates the exact P038 SO-or-SOF plus GF transport assumptions. */
static int
ax211_profile_valid(
	const struct intel_ax211_mmio_profile *profile)
{
	/* Handles the profile availability. */
	if (profile == NULL)
		return 0;

	/* Handles the profile condition. */
	if (profile->mac_type != INTEL_AX211_MMIO_MAC_SO &&
	    profile->mac_type != INTEL_AX211_MMIO_MAC_SOF)

		/* Reports successful completion. */
		return 0;

	/* Handles the profile condition. */
	if (profile->rf_type != INTEL_AX211_MMIO_RF_GF)
		return 0;

	/* Handles the profile condition. */
	if (profile->cdb != 0U || profile->integrated != 0U)
		return 0;

	/* Handles the profile condition. */
	if (profile->umac_prph_offset != INTEL_AX211_MMIO_UMAC_PRPH_OFFSET)
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Checks that a state was initialized through the exact profile gate. */
static int
ax211_mmio_valid(
	const struct intel_ax211_mmio *mmio)
{
	/* Handles the mmio availability. */
	if (mmio == NULL || mmio->ops == NULL)
		return 0;

	/* Handles the csr read32 availability. */
	if (mmio->ops->csr_read32 == NULL || mmio->ops->csr_write32 == NULL)
		return 0;

	/* Handles the prph read32 availability. */
	if (mmio->ops->prph_read32 == NULL || mmio->ops->prph_write32 == NULL)
		return 0;

	/* Handles the delay us availability. */
	if (mmio->ops->delay_us == NULL || mmio->ops->clock_us == NULL)
		return 0;

	/* Checks the ax211 profile valid result. */
	if (!ax211_profile_valid(&mmio->profile))
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Requests NIC readiness and announces OS_ALIVE only after acceptance. */
static int
ax211_set_hw_ready(
	struct intel_ax211_mmio *mmio)
{
	int function_result;
	int result;

	result = ax211_csr_set_bits(mmio, AX211_CSR_HW_IF_CONFIG_REG,
				    AX211_HW_IF_NIC_READY);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	result = ax211_poll_csr(
		mmio, AX211_CSR_HW_IF_CONFIG_REG, AX211_HW_IF_NIC_READY,
		AX211_HW_IF_NIC_READY, AX211_HW_READY_TIMEOUT_US,
		AX211_HW_READY_POLL_US, INTEL_AX211_MMIO_WAIT_HW_READY);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Obtains the ax211 csr write result. */
	function_result = ax211_csr_write(mmio, AX211_CSR_MBOX_SET_REG,
					  AX211_MBOX_OS_ALIVE);

	/* Returns the computed result. */
	return function_result;
}

/* Sets CSR bits through a checked read-modify-write operation. */
static int
ax211_csr_set_bits(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t bits)
{
	int function_result;
	uint32_t value;
	int result;

	result = ax211_csr_read(mmio, offset, &value);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	value |= bits;

	/* Obtains the ax211 csr write result. */
	function_result = ax211_csr_write(mmio, offset, value);

	/* Returns the computed result. */
	return function_result;
}

/* Reads one CSR and converts a backend failure into a private result. */
static int
ax211_csr_read(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t *value)
{
	/* Checks the csr read32 result. */
	if (mmio->ops->csr_read32(mmio->argument, offset, value) != 0)
		return INTEL_AX211_MMIO_IO;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/* Writes one CSR and converts a backend failure into a private result. */
static int
ax211_csr_write(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t value)
{
	/* Checks the csr write32 result. */
	if (mmio->ops->csr_write32(mmio->argument, offset, value) != 0)
		return INTEL_AX211_MMIO_IO;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/* Polls one CSR against an observable finite deadline. */
static int
ax211_poll_csr(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t expected,
	uint32_t mask,
	uint32_t timeout_us,
	uint32_t step_us,
	enum intel_ax211_mmio_wait wait)
{
	uint64_t start;
	uint64_t deadline;
	uint64_t previous;
	uint64_t current;
	uint32_t value;
	uint32_t iteration;
	uint32_t maximum_iterations;
	int result;

	/* Handles the step us condition. */
	if (step_us == 0U || timeout_us == 0U)
		return INTEL_AX211_MMIO_INVALID;

	/* Creates a checked absolute deadline. */
	result = ax211_clock(mmio, &start);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Handles the uint64 t condition. */
	if ((uint64_t)timeout_us > UINT64_MAX - start)
		return INTEL_AX211_MMIO_CLOCK;
	deadline = start + (uint64_t)timeout_us;

	/* Handles the trace deadline availability. */
	if (mmio->ops->trace_deadline != NULL) {
		mmio->ops->trace_deadline(mmio->argument, wait, start,
					  deadline);
	}

	/* Polls with both absolute-time and iteration bounds. */
	previous = start;
	maximum_iterations = timeout_us / step_us + 2U;
	/* Process each element required by the operation. */
	for (iteration = 0U; iteration < maximum_iterations; iteration++) {
		result = ax211_csr_read(mmio, offset, &value);

		/* Checks the operation result. */
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Validates the current value. */
		if ((value & mask) == (expected & mask))
			return INTEL_AX211_MMIO_OK;

		result = ax211_clock(mmio, &current);

		/* Checks the operation result. */
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Handles the current condition. */
		if (current < previous)
			return INTEL_AX211_MMIO_CLOCK;

		/* Handles the current condition. */
		if (current >= deadline)
			return INTEL_AX211_MMIO_TIMEOUT;

		result = ax211_delay(mmio, step_us);

		/* Checks the operation result. */
		if (result != INTEL_AX211_MMIO_OK)
			return result;
		result = ax211_clock(mmio, &current);

		/* Checks the operation result. */
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Handles the current condition. */
		if (current <= previous)
			return INTEL_AX211_MMIO_CLOCK;
		previous = current;
	}

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_TIMEOUT;
}

/* Reads the monotonic backend clock. */
static int
ax211_clock(
	struct intel_ax211_mmio *mmio,
	uint64_t *time_us)
{
	/* Checks the clock us result. */
	if (mmio->ops->clock_us(mmio->argument, time_us) != 0)
		return INTEL_AX211_MMIO_CLOCK;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/* Delays through the checked backend clock source. */
static int
ax211_delay(
	struct intel_ax211_mmio *mmio,
	uint32_t duration_us)
{
	/* Checks the delay us result. */
	if (mmio->ops->delay_us(mmio->argument, duration_us) != 0)
		return INTEL_AX211_MMIO_IO;

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_OK;
}

/* Runs the finite OpenBSD-derived link-power fallback. */
static int
ax211_prepare_fallback(
	struct intel_ax211_mmio *mmio)
{
	uint32_t retry;
	uint32_t polled_us;
	int result;

	/* Keeps the PCIe link awake while requesting ownership. */
	result = ax211_csr_set_bits(mmio, AX211_CSR_DBG_LINK_PWR_MGMT_REG,
				    AX211_LINK_POWER_MANAGEMENT_DISABLED);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	result = ax211_delay(mmio, AX211_PREPARE_SETTLE_US);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Retries within one cumulative 150-ms polling budget. */
	polled_us = 0U;
	/* Process each remaining element. */
	for (retry = 0U; retry < AX211_PREPARE_RETRY_COUNT; retry++) {
		result = ax211_csr_set_bits(mmio, AX211_CSR_HW_IF_CONFIG_REG,
					    AX211_HW_IF_PREPARE);

		/* Checks the operation result. */
		if (result != INTEL_AX211_MMIO_OK)
			return result;

		/* Rechecks readiness at 200-us fallback intervals. */
		do {
			result = ax211_set_hw_ready(mmio);

			/* Checks the operation result. */
			if (result == INTEL_AX211_MMIO_OK)
				return INTEL_AX211_MMIO_OK;

			/* Checks the operation result. */
			if (result != INTEL_AX211_MMIO_TIMEOUT)
				return result;
			result = ax211_delay(mmio, AX211_PREPARE_RETRY_POLL_US);

			/* Checks the operation result. */
			if (result != INTEL_AX211_MMIO_OK)
				return result;
			polled_us += AX211_PREPARE_RETRY_POLL_US;
		} while (polled_us < AX211_PREPARE_TOTAL_POLL_US);

		/* Separates hardware-prepare attempts by 25 ms. */
		result = ax211_delay(mmio, AX211_PREPARE_RETRY_DELAY_US);

		/* Checks the operation result. */
		if (result != INTEL_AX211_MMIO_OK)
			return result;
	}

	/* Returns the computed result. */
	return INTEL_AX211_MMIO_TIMEOUT;
}

/* Clears CSR bits through a checked read-modify-write operation. */
static int
ax211_csr_clear_bits(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint32_t bits)
{
	int function_result;
	uint32_t value;
	int result;

	result = ax211_csr_read(mmio, offset, &value);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;
	value &= ~bits;

	/* Obtains the ax211 csr write result. */
	function_result = ax211_csr_write(mmio, offset, value);

	/* Returns the computed result. */
	return function_result;
}

/* Retains the first stop failure while later cleanup stages still run. */
static void
ax211_remember_error(
	int *first_error,
	int result)
{
	/* Checks the operation status. */
	if (*first_error == INTEL_AX211_MMIO_OK &&
	    result != INTEL_AX211_MMIO_OK)
		*first_error = result;
}

/* Erases private identity temporaries through an observable byte loop. */
static void
ax211_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *byte;

	byte = memory;
	/* Process each remaining element. */
	while (length != 0U) {
		*byte++ = 0U;
		length--;
	}
}

/* Reads and decodes one two-word AX210-family MAC-address source. */
static int
ax211_read_mac_words(
	struct intel_ax211_mmio *mmio,
	uint32_t first_offset,
	uint32_t second_offset,
	uint8_t address[AX211_MAC_ADDRESS_SIZE])
{
	uint32_t first;
	uint32_t second;
	int result;

	first = 0U;
	second = 0U;
	result = ax211_csr_read(mmio, first_offset, &first);

	/* Checks the operation result. */
	if (result == INTEL_AX211_MMIO_OK)
		result = ax211_csr_read(mmio, second_offset, &second);

	/* Checks the operation result. */
	if (result == INTEL_AX211_MMIO_OK)
		ax211_decode_mac_words(first, second, address);
	ax211_scrub(&first, sizeof(first));
	ax211_scrub(&second, sizeof(second));

	/* Returns the computed result. */
	return result;
}

/* Applies the OpenBSD byte order without relying on host endianness. */
static void
ax211_decode_mac_words(
	uint32_t first,
	uint32_t second,
	uint8_t address[AX211_MAC_ADDRESS_SIZE])
{
	address[0] = (uint8_t)(first >> 24);
	address[1] = (uint8_t)(first >> 16);
	address[2] = (uint8_t)(first >> 8);
	address[3] = (uint8_t)first;
	address[4] = (uint8_t)(second >> 8);
	address[5] = (uint8_t)second;
}

/* Rejects the OpenBSD sentinel plus zero, broadcast, and multicast values. */
static int
ax211_mac_valid(
	const uint8_t address[AX211_MAC_ADDRESS_SIZE])
{
	static const uint8_t reserved[AX211_MAC_ADDRESS_SIZE] = {
		0x02U, 0xccU, 0xaaU, 0xffU, 0xeeU, 0x00U};
	static const uint8_t zero[AX211_MAC_ADDRESS_SIZE] = {0U, 0U, 0U,
							     0U, 0U, 0U};
	static const uint8_t broadcast[AX211_MAC_ADDRESS_SIZE] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU};

	/* Handles the address condition. */
	if ((address[0] & 1U) != 0U)
		return 0;

	/* Handles the memcmp condition. */
	if (memcmp(address, reserved, sizeof(reserved)) == 0)
		return 0;

	/* Handles the memcmp condition. */
	if (memcmp(address, zero, sizeof(zero)) == 0)
		return 0;

	/* Handles the memcmp condition. */
	if (memcmp(address, broadcast, sizeof(broadcast)) == 0)
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Writes a 64-bit DMA address as ordered low and high CSR words. */
static int
ax211_publish_address(
	struct intel_ax211_mmio *mmio,
	uint32_t offset,
	uint64_t address)
{
	int function_result;
	int result;

	result = ax211_csr_write(mmio, offset, (uint32_t)address);

	/* Checks the operation result. */
	if (result != INTEL_AX211_MMIO_OK)
		return result;

	/* Obtains the ax211 csr write result. */
	function_result =
		ax211_csr_write(mmio, offset + 4U, (uint32_t)(address >> 32));

	/* Returns the computed result. */
	return function_result;
}
