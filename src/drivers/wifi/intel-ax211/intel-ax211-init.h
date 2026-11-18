/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private initialization protocol contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_INIT_H
#define KERN_DRIVERS_INTEL_AX211_INIT_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

/*
 * The stage-select command, which tells a freshly started firmware whether
 * to stop after reading its NVM or to go straight to runtime.
 *
 * The command is opcode 3 of the platform family 2, and its payload is one
 * 32-bit word of flags, bit 1 asking for the NVM to be read.  The pinned image
 * lists no version for this command; the driver sends the fixed 4-byte
 * version 1 layout.  The first boot pass (intel-ax211-boot.c) sends it with
 * STORE_READ, then the NVM handoff, waits for INIT_FINISHED, and only then
 * queries the NVM; each command waits for its answer before the next is sent.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/fw/api/commands.h
 * (INIT_EXTENDED_CFG_CMD) and
 * drivers/net/wireless/intel/iwlwifi/fw/api/system.h (struct
 * iwl_init_extended_cfg_cmd).
 *
 * Procedure referenced: OpenBSD 0f464d413c50 sys/dev/pci/if_iwx.c
 * (iwx_run_init_mvm_ucode, the version 1 layout).
 */
#define INTEL_AX211_INIT_FAMILY_PLATFORM 2U
#define INTEL_AX211_INIT_STAGE_SELECT_OPCODE 0x03U
#define INTEL_AX211_INIT_STAGE_SELECT_VERSION 1U
#define INTEL_AX211_INIT_STAGE_SELECT_SIZE 4U
#define INTEL_AX211_INIT_STAGE_SELECT_STORE_READ 0x02U

/*
 * The versions of the two NVM commands, as the pinned image lists them.
 *
 * The NVM handoff (family 12, opcode 0) is command version 1 with no
 * notification version; the NVM query (family 12, opcode 2) is command version
 * 1.
 *
 * Values referenced: linux-firmware 20260410
 * intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode (the command version record).
 */
#define INTEL_AX211_INIT_NVM_HANDOFF_COMMAND_VERSION 1U
#define INTEL_AX211_INIT_NVM_HANDOFF_NOTIFICATION_VERSION 0U
#define INTEL_AX211_INIT_NVM_QUERY_COMMAND_VERSION 1U

/* What the stage-select command asks the firmware to do. */
enum intel_ax211_init_profile {
	INTEL_AX211_INIT_PROFILE_RUNTIME = 0,
	INTEL_AX211_INIT_PROFILE_STORE_READ = 1
};

int
drv_intel_ax211_init_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

int
drv_intel_ax211_init_stage_select_encode(
	enum intel_ax211_init_profile profile,
	uint8_t output[INTEL_AX211_INIT_STAGE_SELECT_SIZE]);

int
drv_intel_ax211_init_stage_select_decode(
	const uint8_t *bytes,
	size_t length,
	enum intel_ax211_init_profile *profile);

int
drv_intel_ax211_init_finished_validate(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation);

#endif
