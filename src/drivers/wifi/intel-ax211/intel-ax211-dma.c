/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

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

#include "intel-ax211-dma.h"

#include <errno.h>
#include <string.h>

#define AX211_SCRATCH_CONTROL_FLAGS 0x000f0000U
#define AX211_SCRATCH_PNVM_BASE_OFFSET 16U
#define AX211_SCRATCH_PNVM_SIZE_OFFSET 24U
#define AX211_SCRATCH_RBD_BASE_OFFSET 48U
#define AX211_SCRATCH_DRAM_OFFSET 124U
#define AX211_SCRATCH_UMAC_OFFSET AX211_SCRATCH_DRAM_OFFSET
#define AX211_SCRATCH_LMAC_OFFSET (AX211_SCRATCH_UMAC_OFFSET + 512U)
#define AX211_SCRATCH_PAGING_OFFSET (AX211_SCRATCH_LMAC_OFFSET + 512U)
#define AX211_CONTEXT_VERSION 0U
#define AX211_CONTEXT_CONFIG 0U

#ifdef INTEL_AX211_DMA_HOST_TEST
#define AX211_DMA_SCRUB intel_ax211_dma_host_scrub
#else
#define AX211_DMA_SCRUB drv_intel_ax211_scrub
#endif

static int ax211_boot_manifest_validate( const uint8_t *firmware_bytes, size_t firmware_length, const struct intel_ax211_firmware_manifest *manifest);
static int ax211_range_valid(size_t offset, size_t length, size_t capacity);
static int ax211_fixed_buffers_allocate(struct intel_ax211_dma_resources *resources);
static int ax211_buffer_allocate(struct intel_ax211_dma_resources *resources, struct drv_dma_buffer *buffer, size_t size, size_t alignment);
static int ax211_firmware_buffers_allocate( struct intel_ax211_dma_resources *resources, const uint8_t *bytes, size_t length, const struct intel_ax211_firmware_manifest *manifest);
static int ax211_rx_buffers_allocate(struct intel_ax211_dma_resources *resources);
static int ax211_scratch_build(struct intel_ax211_dma_resources *resources, uint16_t hardware_revision);
static void ax211_put_le16(uint8_t *output, uint16_t value);
static void ax211_put_le32(uint8_t *output, uint32_t value);
static void ax211_put_le64(uint8_t *output, uint64_t value);
static int ax211_context_build(struct intel_ax211_dma_resources *resources);
static void ax211_buffer_release(struct intel_ax211_dma_resources *resources, struct drv_dma_buffer *buffer);

/*
 * Implements the drv intel ax211 dma prepare boot operation.
 */
int
drv_intel_ax211_dma_prepare_boot(
	struct drv_dma_device *device,
	const uint8_t *firmware_bytes,
	size_t firmware_length,
	const struct intel_ax211_firmware_manifest *manifest,
	uint16_t hardware_revision,
	struct intel_ax211_dma_resources *resources)
{
	int error;

	/* Handles the device availability. */
	if (device == NULL || resources == NULL || firmware_length == 0U ||
	    resources->device != NULL || resources->boot_prepared) {
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = ax211_boot_manifest_validate(firmware_bytes, firmware_length,
					     manifest);
	if (error != 0 ||
	    !drv_intel_ax211_mac_type_supported(
		    (uint16_t)((hardware_revision & 0xfff0U) >> 4))) {
		/* Returns the computed result. */
		return error != 0 ? error : EINVAL;
	}
	resources->device = device;

	/* Checks the operation status. */
	error = ax211_fixed_buffers_allocate(resources);
	if (error == 0) {
		error = ax211_firmware_buffers_allocate(
			resources, firmware_bytes, firmware_length, manifest);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = ax211_rx_buffers_allocate(resources);

	/* Checks the operation status. */
	if (error == 0)
		error = ax211_scratch_build(resources, hardware_revision);

	/* Checks the operation status. */
	if (error == 0)
		error = ax211_context_build(resources);

	/* Checks the operation status. */
	if (error != 0) {
		drv_intel_ax211_dma_release(resources);

		/* Returns the computed result. */
		return error;
	}

	resources->boot_prepared = 1U;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv intel ax211 dma prepare pnvm operation.
 */
int
drv_intel_ax211_dma_prepare_pnvm(
	const uint8_t *pnvm_bytes,
	size_t pnvm_length,
	const struct intel_ax211_pnvm_manifest *manifest,
	struct intel_ax211_dma_resources *resources)
{
	const struct intel_ax211_section *section;
	uint8_t *table;
	size_t index;
	int error = 0;

	/* Handles the pnvm bytes availability. */
	if (pnvm_bytes == NULL || manifest == NULL || resources == NULL ||
	    !resources->boot_prepared || !resources->boot_images_released ||
	    resources->pnvm_prepared || pnvm_length != INTEL_AX211_PNVM_SIZE ||
	    manifest->section_count == 0U ||
	    manifest->section_count > INTEL_AX211_MAX_PNVM_SECTIONS ||
	    manifest->total_length == 0U || manifest->total_length > UINT32_MAX) {
		/* Returns the computed result. */
		return EINVAL;
	}
	error = ax211_buffer_allocate(resources, &resources->pnvm_table,
				      INTEL_AX211_PNVM_ADDRESS_TABLE_SIZE, 1U);
	/* Process each remaining element. */
	for (index = 0U; index < manifest->section_count && error == 0;
	     index++) {
		/* Checks the ax211 range valid result. */
		section = &manifest->section[index];
		if (section->length == 0U ||
		    !ax211_range_valid(section->file_offset, section->length,
				       pnvm_length) ||
		    resources->pnvm_total_length > SIZE_MAX - section->length) {
			error = EINVAL;
			continue;
		}

		/* Checks the operation status. */
		error = ax211_buffer_allocate(resources,
					      &resources->pnvm[index],
					      section->length, 1U);
		if (error == 0) {
			memcpy(resources->pnvm[index].address,
			       pnvm_bytes + section->file_offset,
			       section->length);
			resources->pnvm_total_length += section->length;
			resources->pnvm_count++;
		}
	}

	/* Checks the operation status. */
	if (error == 0 &&
	    resources->pnvm_total_length != manifest->total_length)
		error = EINVAL;

	/* Checks the operation status. */
	if (error != 0) {
		/* Process each remaining element. */
		while (resources->pnvm_count != 0U) {
			resources->pnvm_count--;
			ax211_buffer_release(
				resources,
				&resources->pnvm[resources->pnvm_count]);
		}

		resources->pnvm_total_length = 0U;
		ax211_buffer_release(resources, &resources->pnvm_table);

		/* Returns the computed result. */
		return error;
	}

	table = resources->pnvm_table.address;
	/* Process each remaining element. */
	for (index = 0U; index < resources->pnvm_count; index++) {
		ax211_put_le64(table + index * 8U,
			       resources->pnvm[index].device_address);
	}

	ax211_put_le64((uint8_t *)resources->scratch.address +
			       AX211_SCRATCH_PNVM_BASE_OFFSET,
		       resources->pnvm_table.device_address);
	ax211_put_le32((uint8_t *)resources->scratch.address +
			       AX211_SCRATCH_PNVM_SIZE_OFFSET,
		       (uint32_t)resources->pnvm_total_length);
	resources->pnvm_prepared = 1U;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv intel ax211 dma release boot images operation.
 */
void
drv_intel_ax211_dma_release_boot_images(
	struct intel_ax211_dma_resources *resources)
{
	size_t count;

	/* Handles the resources availability. */
	if (resources == NULL || resources->device == NULL ||
	    !resources->boot_prepared || resources->boot_images_released) {
		/* Returns the computed result. */
		return;
	}
	count = resources->firmware_count;
	/* Process each remaining element. */
	while (count != 0U) {
		count--;

		/* Handles the resources condition. */
		if (resources->firmware[count].image_class !=
		    INTEL_AX211_DMA_IMAGE_PAGING) {
			ax211_buffer_release(
				resources, &resources->firmware[count].buffer);
		}
	}

	ax211_buffer_release(resources, &resources->iml);
	resources->boot_images_released = 1U;
}

/*
 * Implements the drv intel ax211 dma release operation.
 */
void
drv_intel_ax211_dma_release(
	struct intel_ax211_dma_resources *resources)
{
	size_t count;

	/* Handles the resources availability. */
	if (resources == NULL)
		return;

	/* Handles the device availability. */
	if (resources->device == NULL) {
		memset(resources, 0, sizeof(*resources));

		/* Returns the computed result. */
		return;
	}

	count = resources->pnvm_count;
	/* Process each remaining element. */
	while (count != 0U) {
		count--;
		ax211_buffer_release(resources, &resources->pnvm[count]);
	}

	ax211_buffer_release(resources, &resources->pnvm_table);
	count = resources->rx_buffer_count;
	/* Process each remaining element. */
	while (count != 0U) {
		count--;
		ax211_buffer_release(resources, &resources->rx_buffer[count]);
	}

	count = resources->firmware_count;
	/* Process each remaining element. */
	while (count != 0U) {
		count--;
		ax211_buffer_release(resources,
				     &resources->firmware[count].buffer);
	}

	ax211_buffer_release(resources, &resources->iml);
	ax211_buffer_release(resources, &resources->rx_status);
	ax211_buffer_release(resources, &resources->rx_completion);
	ax211_buffer_release(resources, &resources->rx_transfer);
	ax211_buffer_release(resources, &resources->command_external);
	ax211_buffer_release(resources, &resources->command_slots);
	ax211_buffer_release(resources, &resources->command_byte_count);
	ax211_buffer_release(resources, &resources->command_tfd);
	ax211_buffer_release(resources, &resources->ict);
	ax211_buffer_release(resources, &resources->prph_info);
	ax211_buffer_release(resources, &resources->scratch);
	ax211_buffer_release(resources, &resources->context);
	memset(resources, 0, sizeof(*resources));
}

/* Supports the ax211 boot manifest validate operation. */
static int
ax211_boot_manifest_validate(
	const uint8_t *firmware_bytes,
	size_t firmware_length,
	const struct intel_ax211_firmware_manifest *manifest)
{
	const struct intel_ax211_section *section;
	size_t cpu_separator;
	size_t paging_separator;
	size_t index;

	/* Checks the ax211 range valid result. */
	if (firmware_bytes == NULL || manifest == NULL ||
	    firmware_length != INTEL_AX211_FIRMWARE_SIZE ||
	    manifest->runtime_count > INTEL_AX211_MAX_FW_SECTIONS ||
	    manifest->lmac_count == 0U || manifest->lmac_count > 64U ||
	    manifest->umac_count == 0U || manifest->umac_count > 64U ||
	    manifest->paging_count == 0U || manifest->paging_count > 64U ||
	    manifest->lmac_count + manifest->umac_count +
			    manifest->paging_count + 2U !=
		    manifest->runtime_count ||
	    manifest->iml_length != INTEL_AX211_IML_SIZE ||
	    !ax211_range_valid(manifest->iml_offset, manifest->iml_length,
			       firmware_length)) {
		/* Returns the computed result. */
		return EINVAL;
	}
	cpu_separator = manifest->lmac_count;
	paging_separator = cpu_separator + manifest->umac_count + 1U;
	/* Process each remaining element. */
	for (index = 0U; index < manifest->runtime_count; index++) {
		/* Checks the current index. */
		section = &manifest->runtime[index];
		if (index == cpu_separator) {
			/* Handles the section condition. */
			if (section->destination !=
				    INTEL_AX211_CPU1_CPU2_SEPARATOR ||
			    section->length != 0U) {
				/* Returns the computed result. */
				return EINVAL;
			}
		} else if (index == paging_separator) {
			/* Handles the section condition. */
			if (section->destination !=
				    INTEL_AX211_PAGING_SEPARATOR ||
			    section->length != 0U) {
				/* Returns the computed result. */
				return EINVAL;
			}
		} else if (section->destination ==
				   INTEL_AX211_CPU1_CPU2_SEPARATOR ||
			   section->destination ==
				   INTEL_AX211_PAGING_SEPARATOR ||
			   section->length == 0U ||
			   section->length >
				   INTEL_AX211_FIRMWARE_SECTION_SIZE_MAX ||
			   !ax211_range_valid(section->file_offset,
					      section->length,
					      firmware_length)) {
			/* Returns the computed result. */
			return EINVAL;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 range valid operation. */
static int
ax211_range_valid(
	size_t offset,
	size_t length,
	size_t capacity)
{
	/* Returns the computed result. */
	return offset <= capacity && length <= capacity - offset;
}

/* Supports the ax211 fixed buffers allocate operation. */
static int
ax211_fixed_buffers_allocate(
	struct intel_ax211_dma_resources *resources)
{
	int error;

	/* Checks the operation status. */
	error = ax211_buffer_allocate(resources, &resources->context,
				      INTEL_AX211_CONTEXT_INFO_GEN3_SIZE, 1U);
	if (error == 0) {
		error = ax211_buffer_allocate(resources, &resources->scratch,
					      INTEL_AX211_PRPH_SCRATCH_SIZE,
					      1U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(resources, &resources->prph_info,
					      INTEL_AX211_PRPH_INFO_SIZE, 1U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(resources, &resources->ict,
					      INTEL_AX211_ICT_SIZE,
					      INTEL_AX211_ICT_SIZE);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_tfd,
			INTEL_AX211_COMMAND_TFD_RING_SIZE, 256U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_byte_count,
			INTEL_AX211_COMMAND_BC_TABLE_SIZE, 128U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_slots,
			INTEL_AX211_COMMAND_SLOTS_SIZE, 64U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_external,
			INTEL_AX211_COMMAND_EXTERNAL_SIZE, 64U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->rx_transfer,
			INTEL_AX211_RX_TRANSFER_RING_SIZE, 256U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->rx_completion,
			INTEL_AX211_RX_COMPLETION_RING_SIZE, 256U);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = ax211_buffer_allocate(resources, &resources->rx_status,
					      INTEL_AX211_RX_STATUS_SIZE, 16U);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 buffer allocate operation. */
static int
ax211_buffer_allocate(
	struct intel_ax211_dma_resources *resources,
	struct drv_dma_buffer *buffer,
	size_t size,
	size_t alignment)
{
	int error;

	/* Checks the operation status. */
	error = drv_dma_alloc_coherent(resources->device, size, alignment,
				       buffer);
	if (error != 0)
		return error;

	/* Handles the address availability. */
	if (buffer->address == NULL || buffer->size != size ||
	    buffer->device_address > UINT64_MAX - (uint64_t)(size - 1U) ||
	    (alignment > 1U &&
	     (buffer->device_address & (uint64_t)(alignment - 1U)) != 0U)) {
		/* Handles the address availability. */
		if (buffer->address != NULL && buffer->size != 0U)
			AX211_DMA_SCRUB(buffer->address, buffer->size);
		drv_dma_free_coherent(resources->device, buffer);

		/* Returns the computed result. */
		return EIO;
	}

	memset(buffer->address, 0, buffer->size);

	/* Reports successful completion. */
	return 0;
}

/* Supports the ax211 firmware buffers allocate operation. */
static int
ax211_firmware_buffers_allocate(
	struct intel_ax211_dma_resources *resources,
	const uint8_t *bytes,
	size_t length,
	const struct intel_ax211_firmware_manifest *manifest)
{
	const struct intel_ax211_section *section;
	struct intel_ax211_dma_image *image;
	size_t index;
	unsigned image_class = INTEL_AX211_DMA_IMAGE_LMAC;
	int error = 0;

	/* Checks the ax211 range valid result. */
	if (!ax211_range_valid(manifest->iml_offset, manifest->iml_length,
			       length) ||
	    manifest->iml_length != INTEL_AX211_IML_SIZE) {
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = ax211_buffer_allocate(resources, &resources->iml,
				      manifest->iml_length, 1U);
	if (error != 0)
		return error;
	memcpy(resources->iml.address, bytes + manifest->iml_offset,
	       manifest->iml_length);
	/* Process each remaining element. */
	for (index = 0U; index < manifest->runtime_count && error == 0;
	     index++) {
		/* Handles the section condition. */
		section = &manifest->runtime[index];
		if (section->destination == INTEL_AX211_CPU1_CPU2_SEPARATOR) {
			image_class = INTEL_AX211_DMA_IMAGE_UMAC;
			continue;
		}

		/* Handles the section condition. */
		if (section->destination == INTEL_AX211_PAGING_SEPARATOR) {
			image_class = INTEL_AX211_DMA_IMAGE_PAGING;
			continue;
		}

		/* Checks the ax211 range valid result. */
		if (section->length == 0U ||
		    section->length > INTEL_AX211_FIRMWARE_SECTION_SIZE_MAX ||
		    !ax211_range_valid(section->file_offset, section->length,
				       length) ||
		    resources->firmware_count >= INTEL_AX211_MAX_FW_SECTIONS) {
			error = EINVAL;
			continue;
		}

		image = &resources->firmware[resources->firmware_count];

		/* Checks the operation status. */
		error = ax211_buffer_allocate(resources, &image->buffer,
					      section->length, 1U);
		if (error == 0) {
			memcpy(image->buffer.address,
			       bytes + section->file_offset, section->length);
			image->destination = section->destination;
			image->image_class = (uint8_t)image_class;
			resources->firmware_count++;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 rx buffers allocate operation. */
static int
ax211_rx_buffers_allocate(
	struct intel_ax211_dma_resources *resources)
{
	uint8_t *descriptor;
	int result;
	size_t index;
	int error = 0;

	/* Process each remaining element. */
	for (index = 0U; index < INTEL_AX211_RX_RING_SIZE && error == 0;
	     index++) {
		/* Checks the operation status. */
		error = ax211_buffer_allocate(
			resources, &resources->rx_buffer[index],
			INTEL_AX211_RX_BUFFER_SIZE, INTEL_AX211_RX_BUFFER_SIZE);
		if (error == 0) {
			resources->rx_buffer_count++;
			descriptor =
				(uint8_t *)resources->rx_transfer.address +
				index * INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE;

			/* Checks the operation result. */
			result = drv_intel_ax211_rx_transfer_descriptor_encode(
				descriptor, (uint16_t)index,
				resources->rx_buffer[index].device_address);
			if (result != INTEL_AX211_OK)
				error = EINVAL;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the ax211 scratch build operation. */
static int
ax211_scratch_build(
	struct intel_ax211_dma_resources *resources,
	uint16_t hardware_revision)
{
	const struct intel_ax211_dma_image *image;
	size_t offset;
	uint8_t *scratch = resources->scratch.address;
	size_t index;
	size_t lmac = 0U;
	size_t umac = 0U;
	size_t paging = 0U;

	/* Handles the scratch availability. */
	if (scratch == NULL)
		return EINVAL;
	memset(scratch, 0, resources->scratch.size);
	ax211_put_le16(scratch, hardware_revision);
	ax211_put_le16(scratch + 2U, 0U);
	ax211_put_le16(scratch + 4U, INTEL_AX211_PRPH_SCRATCH_SIZE / 4U);
	ax211_put_le32(scratch + 8U, AX211_SCRATCH_CONTROL_FLAGS);
	ax211_put_le64(scratch + AX211_SCRATCH_RBD_BASE_OFFSET,
		       resources->rx_transfer.device_address);
	/* Process each remaining element. */
	for (index = 0U; index < resources->firmware_count; index++) {
		/* Handles the image condition. */
		image = &resources->firmware[index];
		if (image->image_class == INTEL_AX211_DMA_IMAGE_LMAC) {
			/* Handles the lmac condition. */
			if (lmac >= 64U)
				return EOVERFLOW;
			offset = AX211_SCRATCH_LMAC_OFFSET + lmac++ * 8U;
		} else if (image->image_class == INTEL_AX211_DMA_IMAGE_UMAC) {
			/* Handles the umac condition. */
			if (umac >= 64U)
				return EOVERFLOW;
			offset = AX211_SCRATCH_UMAC_OFFSET + umac++ * 8U;
		} else if (image->image_class == INTEL_AX211_DMA_IMAGE_PAGING) {
			/* Handles the paging condition. */
			if (paging >= 64U)
				return EOVERFLOW;
			offset = AX211_SCRATCH_PAGING_OFFSET + paging++ * 8U;
		} else {
			/* Returns the computed result. */
			return EINVAL;
		}

		ax211_put_le64(scratch + offset, image->buffer.device_address);
	}

	/* Returns the computed result. */
	return lmac == 0U || umac == 0U || paging == 0U ? EINVAL : 0;
}

/* Supports the ax211 put le16 operation. */
static void
ax211_put_le16(
	uint8_t *output,
	uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
}

/* Supports the ax211 put le32 operation. */
static void
ax211_put_le32(
	uint8_t *output,
	uint32_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
	output[2] = (uint8_t)(value >> 16);
	output[3] = (uint8_t)(value >> 24);
}

/* Supports the ax211 put le64 operation. */
static void
ax211_put_le64(
	uint8_t *output,
	uint64_t value)
{
	ax211_put_le32(output, (uint32_t)value);
	ax211_put_le32(output + 4U, (uint32_t)(value >> 32));
}

/* Supports the ax211 context build operation. */
static int
ax211_context_build(
	struct intel_ax211_dma_resources *resources)
{
	struct intel_ax211_context_info_gen3 context;
	int result;

	/* Describes every ring and scratch area to the firmware. */
	memset(&context, 0, sizeof(context));
	context.version = AX211_CONTEXT_VERSION;
	context.config = AX211_CONTEXT_CONFIG;
	context.prph_info_base = resources->prph_info.device_address;
	context.cr_head_index_base = resources->rx_status.device_address;
	context.tr_tail_index_base =
		resources->prph_info.device_address + 2048U;
	context.cr_tail_index_base =
		resources->prph_info.device_address + 3072U;
	context.command_transfer_ring_base =
		resources->command_tfd.device_address;
	context.command_completion_ring_base =
		resources->rx_completion.device_address;
	context.command_transfer_ring_size = INTEL_AX211_COMMAND_RING_CB_SIZE;
	context.command_completion_ring_size = INTEL_AX211_RX_RING_CB_SIZE;
	context.prph_scratch_base = resources->scratch.device_address;
	context.prph_scratch_size = INTEL_AX211_PRPH_SCRATCH_SIZE;
	result = drv_intel_ax211_context_info_gen3_encode(
		resources->context.address, &context);

	/* Returns the computed result. */
	return result == INTEL_AX211_OK ? 0 : EINVAL;
}

/* Supports the ax211 buffer release operation. */
static void
ax211_buffer_release(
	struct intel_ax211_dma_resources *resources,
	struct drv_dma_buffer *buffer)
{
	/* Handles the address availability. */
	if (buffer->address != NULL) {
		AX211_DMA_SCRUB(buffer->address, buffer->size);
		drv_dma_free_coherent(resources->device, buffer);
	}

	memset(buffer, 0, sizeof(*buffer));
}
