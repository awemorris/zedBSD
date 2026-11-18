/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private DMA ownership.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_DMA_H
#define KERN_DRIVERS_INTEL_AX211_DMA_H

#include <drivers/generic/dma.h>

#include "intel-ax211-internal.h"

/*
 * The sizes of the DMA areas prepared for one boot.
 *
 * - The scratch area is 1660 bytes; the firmware is told its size in words
 *   at its byte 4 (415).
 * - The status page is one 4 KiB page owned by the firmware; the transmit
 *   and completion tail arrays live inside it (intel-ax211-internal.h).
 * - The interrupt cause table (4 KiB, 4 KiB aligned) is allocated but never
 *   given to the device: the table is the legacy interrupt path, and this
 *   driver runs in MSI-X mode only.
 * - The command ring, its byte-count table, its slots and the external
 *   command buffer, and the two receive rings, have the sizes given in
 *   intel-ax211-transport.c.  The device's completion index is one 16-bit
 *   word, and each receive buffer is 4 KiB, which the scratch area's
 *   control flags announce.
 * - The PNVM address table holds the 64-bit address of each PNVM section
 *   copied for the running board, at most 64 entries in 512 bytes.
 * - The image loader is the 13944-byte loader record of the pinned image,
 *   and no runtime section of that image exceeds 32 KiB, which the loader
 *   checks before it copies one into DMA memory.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/pcie/iwl-context-info-v2.h (the scratch
 * area, struct iwl_prph_scratch, and the status page),
 * drivers/net/wireless/intel/iwlwifi/iwl-fh.h (rings and the 4 KiB receive
 * buffers) and drivers/net/wireless/intel/iwlwifi/fw/file.h (the image loader
 * record).
 */
#define INTEL_AX211_SCRATCH_AREA_SIZE 1660U
#define INTEL_AX211_STATUS_PAGE_SIZE 4096U
#define INTEL_AX211_INTERRUPT_TABLE_SIZE 4096U
#define INTEL_AX211_COMMAND_TFD_RING_SIZE 65536U
#define INTEL_AX211_COMMAND_LENGTH_TABLE_SIZE 2048U
#define INTEL_AX211_COMMAND_SLOTS_SIZE 82944U
#define INTEL_AX211_COMMAND_EXTERNAL_SIZE 4096U
#define INTEL_AX211_RX_TRANSFER_RING_SIZE 8192U
#define INTEL_AX211_RX_COMPLETION_RING_SIZE 16384U
#define INTEL_AX211_RX_STATUS_SIZE 2U
#define INTEL_AX211_RX_BUFFER_SIZE 4096U
#define INTEL_AX211_PNVM_ADDRESS_TABLE_SIZE 512U
#define INTEL_AX211_LOADER_IMAGE_SIZE 13944U
#define INTEL_AX211_FIRMWARE_SECTION_SIZE_MAX 32768U

/*
 * The three groups of runtime sections, in the order the firmware file
 * lists them between its two marks; each group has its own address list in
 * the scratch area.
 */
enum intel_ax211_dma_image_class {
	INTEL_AX211_DMA_IMAGE_LMAC = 1,
	INTEL_AX211_DMA_IMAGE_UMAC = 2,
	INTEL_AX211_DMA_IMAGE_PAGING = 3
};

/* One runtime section of the image, copied into its own DMA buffer. */
struct intel_ax211_dma_image {
	struct drv_dma_buffer buffer;
	uint32_t destination;
	uint8_t image_class;
	uint8_t reserved[3];
};

/*
 * Every DMA buffer one boot of the device uses.
 *
 * The PCI transport owns one zero-initialized instance.  No member may be
 * copied after preparation because private_data belongs to the DMA mapper.
 */
struct intel_ax211_dma_resources {
	struct drv_dma_device *device;
	struct drv_dma_buffer context;
	struct drv_dma_buffer scratch;
	struct drv_dma_buffer status_page;
	struct drv_dma_buffer ict;
	struct drv_dma_buffer command_tfd;
	struct drv_dma_buffer command_byte_count;
	struct drv_dma_buffer command_slots;
	struct drv_dma_buffer command_external;
	struct drv_dma_buffer rx_transfer;
	struct drv_dma_buffer rx_completion;
	struct drv_dma_buffer rx_status;
	struct drv_dma_buffer iml;
	struct intel_ax211_dma_image firmware[INTEL_AX211_FIRMWARE_SECTION_LIMIT];
	size_t firmware_count;
	struct drv_dma_buffer rx_buffer[INTEL_AX211_RX_RING_SIZE];
	size_t rx_buffer_count;
	struct drv_dma_buffer pnvm_table;
	struct drv_dma_buffer pnvm[INTEL_AX211_PNVM_SECTION_LIMIT];
	size_t pnvm_count;
	size_t pnvm_total_length;
	uint8_t boot_prepared;
	uint8_t pnvm_prepared;
	uint8_t boot_images_released;
	uint8_t reserved;
};

int
drv_intel_ax211_dma_prepare_boot(
	struct drv_dma_device *device,
	const uint8_t *firmware_bytes,
	size_t firmware_length,
	const struct intel_ax211_firmware_manifest *manifest,
	uint16_t hardware_revision,
	struct intel_ax211_dma_resources *resources);

/*
 * PNVM preparation is admitted only after release_boot_images records
 * an accepted ALIVE generation.
 */
int
drv_intel_ax211_dma_prepare_pnvm(
	const uint8_t *pnvm_bytes,
	size_t pnvm_length,
	const struct intel_ax211_pnvm_manifest *manifest,
	struct intel_ax211_dma_resources *resources);

/*
 * Call only after the exact ALIVE notification has been accepted.  This
 * retires IML and LMAC/UMAC images; paging and runtime rings remain owned.
 */
void
drv_intel_ax211_dma_release_boot_images(
	struct intel_ax211_dma_resources *resources);

void
drv_intel_ax211_dma_release(
	struct intel_ax211_dma_resources *resources);

#ifdef INTEL_AX211_DMA_HOST_TEST
void
intel_ax211_dma_host_scrub(
	void *memory,
	size_t length);
#endif

#endif
