/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private MMIO contract.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_MMIO_H
#define KERN_DRIVERS_INTEL_AX211_MMIO_H

#include <stdint.h>

/*
 * The device profile the MMIO layer accepts.
 *
 * drv_intel_ax211_mmio_init() refuses any other profile, a radio that runs two
 * bands at once, an integrated part, or another periphery base, because the
 * register sequences in intel-ax211-mmio.c were established for this
 * combination only.  The image loader size is that of the loader record of the
 * pinned firmware image (13944 bytes), and publish_gen3 refuses any other
 * length.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/iwl-config.h (MAC type codes 0x37 So and
 * 0x43 SoF, radio kind 0x10d Garfield Peak) and
 * drivers/net/wireless/intel/iwlwifi/cfg/ax210.c (the upper-CPU periphery base
 * 0x300000 of the AX210 family).
 */
#define INTEL_AX211_MMIO_MAC_SO 0x37U
#define INTEL_AX211_MMIO_MAC_SOF 0x43U
#define INTEL_AX211_MMIO_RF_GF 0x10dU
#define INTEL_AX211_MMIO_UPPER_CPU_PRPH_BASE 0x300000U
#define INTEL_AX211_MMIO_LOADER_IMAGE_SIZE 13944U

/* The results of the MMIO layer. */
enum intel_ax211_mmio_result {
	INTEL_AX211_MMIO_OK = 0,
	INTEL_AX211_MMIO_INVALID = 1,
	INTEL_AX211_MMIO_IO = 2,
	INTEL_AX211_MMIO_TIMEOUT = 3,
	INTEL_AX211_MMIO_CLOCK = 4,
	INTEL_AX211_MMIO_ORDER = 5,
	INTEL_AX211_MMIO_NOT_OWNER = 6
};

/* The bounded waits of the MMIO layer, named for the deadline trace. */
enum intel_ax211_mmio_wait {
	INTEL_AX211_MMIO_WAIT_HW_READY = 1,
	INTEL_AX211_MMIO_WAIT_APM_CLOCK = 2,
	INTEL_AX211_MMIO_WAIT_NIC_OWNERSHIP = 3,
	INTEL_AX211_MMIO_WAIT_MASTER_DISABLED = 4
};

/* The identity of the device the MMIO layer runs on. */
struct intel_ax211_mmio_profile {
	uint16_t mac_type;
	uint16_t rf_type;
	uint8_t concurrent_bands;
	uint8_t integrated;
	uint32_t upper_cpu_prph_base;
};

/*
 * The register access, the delay and the clock the MMIO layer needs from its
 * backend; the deadline trace is optional.
 */
struct intel_ax211_mmio_ops {
	int (*csr_read32)(void *argument, uint32_t offset, uint32_t *value);
	int (*csr_write32)(void *argument, uint32_t offset, uint32_t value);
	int (*prph_read32)(void *argument, uint32_t address, uint32_t *value);
	int (*prph_write32)(void *argument, uint32_t address, uint32_t value);
	int (*delay_us)(void *argument, uint32_t duration_us);
	int (*clock_us)(void *argument, uint64_t *time_us);
	void (*trace_deadline)(void *argument, enum intel_ax211_mmio_wait wait,
			       uint64_t start_us, uint64_t deadline_us);
};

/*
 * What the device needs to start its firmware: the boot context and the loader.
 */
struct intel_ax211_mmio_boot {
	uint64_t context_address;
	uint64_t iml_address;
	uint32_t iml_size;
};

/*
 * The power and ownership state of one device.
 *
 * prepared, reset_done and apm_ready record how far the power-up has come, in
 * that order; a stop clears them.  wake_hold_depth counts the nested holds
 * that keep the device awake for register access.
 */
struct intel_ax211_mmio {
	const struct intel_ax211_mmio_ops *ops;
	void *argument;
	struct intel_ax211_mmio_profile profile;
	unsigned int wake_hold_depth;
	int prepared;
	int reset_done;
	int apm_ready;
	int master_disable_timed_out;
};

int
drv_intel_ax211_mmio_init(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_ops *ops,
	void *argument,
	const struct intel_ax211_mmio_profile *profile);

int
drv_intel_ax211_mmio_take_ownership(
	struct intel_ax211_mmio *mmio);

int
drv_intel_ax211_mmio_sw_reset(
	struct intel_ax211_mmio *mmio);

int
drv_intel_ax211_mmio_apm_init(
	struct intel_ax211_mmio *mmio);

/* Success guarantees a STOP_MASTER request, APM stop, and settled reset. */
int
drv_intel_ax211_mmio_stop(
	struct intel_ax211_mmio *mmio);

int
drv_intel_ax211_mmio_wake_hold(
	struct intel_ax211_mmio *mmio);

int
drv_intel_ax211_mmio_wake_release(
	struct intel_ax211_mmio *mmio);

int
drv_intel_ax211_mmio_read_mac(
	struct intel_ax211_mmio *mmio,
	uint8_t mac_address[6]);

int
drv_intel_ax211_mmio_prph_read32(
	struct intel_ax211_mmio *mmio,
	uint32_t address,
	uint32_t *value);

int
drv_intel_ax211_mmio_prph_write32(
	struct intel_ax211_mmio *mmio,
	uint32_t address,
	uint32_t value);

int
drv_intel_ax211_mmio_publish_gen3(
	struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_boot *boot);

#endif
