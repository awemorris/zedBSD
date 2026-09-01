/*
 * zedBSD Intel AX211 private DMA ownership
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c.
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
 *
 * SPDX-License-Identifier: ISC AND BSD-3-Clause
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_DMA_H
#define ZEDBSD_DRIVERS_INTEL_AX211_DMA_H

#include <drivers/dma.h>

#include "intel-ax211-internal.h"

#define INTEL_AX211_PRPH_SCRATCH_SIZE		1660U
#define INTEL_AX211_PRPH_INFO_SIZE		4096U
#define INTEL_AX211_ICT_SIZE			4096U
#define INTEL_AX211_COMMAND_TFD_RING_SIZE	65536U
#define INTEL_AX211_COMMAND_BC_TABLE_SIZE	2048U
#define INTEL_AX211_COMMAND_SLOTS_SIZE		82944U
#define INTEL_AX211_COMMAND_EXTERNAL_SIZE	4096U
#define INTEL_AX211_RX_TRANSFER_RING_SIZE	8192U
#define INTEL_AX211_RX_COMPLETION_RING_SIZE	16384U
#define INTEL_AX211_RX_STATUS_SIZE		2U
#define INTEL_AX211_RX_BUFFER_SIZE		4096U
#define INTEL_AX211_PNVM_ADDRESS_TABLE_SIZE	512U
#define INTEL_AX211_IML_SIZE			13944U
#define INTEL_AX211_FIRMWARE_SECTION_SIZE_MAX	32768U

enum intel_ax211_dma_image_class {
	INTEL_AX211_DMA_IMAGE_LMAC = 1,
	INTEL_AX211_DMA_IMAGE_UMAC = 2,
	INTEL_AX211_DMA_IMAGE_PAGING = 3
};

struct intel_ax211_dma_image {
	struct drv_dma_buffer buffer;
	uint32_t destination;
	uint8_t image_class;
	uint8_t reserved[3];
};

/*
 * The PCI transport owns one zero-initialized instance.  No member may be
 * copied after preparation because private_data belongs to the DMA mapper.
 */
struct intel_ax211_dma_resources {
	struct drv_dma_device *device;
	struct drv_dma_buffer context;
	struct drv_dma_buffer scratch;
	struct drv_dma_buffer prph_info;
	struct drv_dma_buffer ict;
	struct drv_dma_buffer command_tfd;
	struct drv_dma_buffer command_byte_count;
	struct drv_dma_buffer command_slots;
	struct drv_dma_buffer command_external;
	struct drv_dma_buffer rx_transfer;
	struct drv_dma_buffer rx_completion;
	struct drv_dma_buffer rx_status;
	struct drv_dma_buffer iml;
	struct intel_ax211_dma_image firmware[INTEL_AX211_MAX_FW_SECTIONS];
	size_t firmware_count;
	struct drv_dma_buffer rx_buffer[INTEL_AX211_RX_RING_SIZE];
	size_t rx_buffer_count;
	struct drv_dma_buffer pnvm_table;
	struct drv_dma_buffer pnvm[INTEL_AX211_MAX_PNVM_SECTIONS];
	size_t pnvm_count;
	size_t pnvm_total_length;
	uint8_t boot_prepared;
	uint8_t pnvm_prepared;
	uint8_t boot_images_released;
	uint8_t reserved;
};

int intel_ax211_dma_prepare_boot(struct drv_dma_device *device,
	const uint8_t *firmware_bytes, size_t firmware_length,
	const struct intel_ax211_firmware_manifest *manifest,
	uint16_t hardware_revision, struct intel_ax211_dma_resources *resources);
/* PNVM preparation is admitted only after release_boot_images records an
 * accepted ALIVE generation. */
int intel_ax211_dma_prepare_pnvm(const uint8_t *pnvm_bytes,
	size_t pnvm_length, const struct intel_ax211_pnvm_manifest *manifest,
	struct intel_ax211_dma_resources *resources);
/* Call only after the exact ALIVE notification has been accepted.  This
 * retires IML and LMAC/UMAC images; paging and runtime rings remain owned. */
void intel_ax211_dma_release_boot_images(
	struct intel_ax211_dma_resources *resources);
void intel_ax211_dma_release(struct intel_ax211_dma_resources *resources);

#ifdef INTEL_AX211_DMA_HOST_TEST
void intel_ax211_dma_host_scrub(void *memory, size_t length);
#endif

#endif
