/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 private DMA ownership.
 */

#include "intel-ax211-dma.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>

/*
 * The layout of the scratch area, and the boot context's fixed fields.
 *
 * ax211_scratch_build() writes, from byte 0: the silicon revision as read from
 * the device, a zero layout version, the size of the area in 32-bit words and,
 * at byte 8, the control flags.  The flags used are 0x000f0000: receive
 * buffers of 4 KiB (bit 16), completions in the descriptor form (bit 17), and
 * transfer descriptors of 256 bytes (bits 19:18 set to 3), the forms the
 * transport implements.  Byte 16 holds the address of the PNVM address table
 * and byte 24 the total length of the copied PNVM sections, both written only
 * once the firmware has named its board.  Byte 48 holds the address of the
 * receive transfer ring.  From byte 124 the area holds three lists of 64
 * section addresses (64-bit, 512 bytes each): the UMAC sections, then the LMAC
 * sections, then the paged sections.  The firmware copies its code from those
 * addresses; a boot with an empty group is refused.
 *
 * Values referenced: Linux v6.17
 * drivers/net/wireless/intel/iwlwifi/pcie/iwl-context-info-v2.h (struct
 * iwl_prph_scratch and enum iwl_prph_scratch_flags) and OpenBSD 0f464d413c50
 * sys/dev/pci/if_iwxreg.h (struct iwx_prph_scratch).
 */
#define AX211_SCRATCH_CONTROL_FLAGS 0x000f0000U
#define AX211_SCRATCH_PNVM_BASE_OFFSET 16U
#define AX211_SCRATCH_PNVM_SIZE_OFFSET 24U
#define AX211_SCRATCH_RBD_BASE_OFFSET 48U
#define AX211_SCRATCH_DRAM_OFFSET 124U
#define AX211_SCRATCH_UMAC_OFFSET AX211_SCRATCH_DRAM_OFFSET
#define AX211_SCRATCH_LMAC_OFFSET (AX211_SCRATCH_UMAC_OFFSET + 512U)
#define AX211_SCRATCH_PAGING_OFFSET (AX211_SCRATCH_LMAC_OFFSET + 512U)
#define AX211_BOOT_CONTEXT_VERSION 0U
#define AX211_BOOT_CONTEXT_CONFIG 0U

/*
 * How a DMA buffer is cleared before it is freed: the host test counts the
 * clearings, the kernel clears with the driver's scrub.
 */
#ifdef INTEL_AX211_DMA_HOST_TEST
#define AX211_DMA_SCRUB intel_ax211_dma_host_scrub
#else
#define AX211_DMA_SCRUB drv_intel_ax211_scrub
#endif

static int ax211_boot_manifest_validate(const uint8_t *firmware_bytes, size_t firmware_length, const struct intel_ax211_firmware_manifest *manifest);
static int ax211_range_valid(size_t offset, size_t length, size_t capacity);
static int ax211_fixed_buffers_allocate(struct intel_ax211_dma_resources *resources);
static int ax211_buffer_allocate(struct intel_ax211_dma_resources *resources, struct drv_dma_buffer *buffer, size_t size, size_t alignment);
static int ax211_firmware_buffers_allocate(struct intel_ax211_dma_resources *resources, const uint8_t *bytes, size_t length, const struct intel_ax211_firmware_manifest *manifest);
static int ax211_rx_buffers_allocate(struct intel_ax211_dma_resources *resources);
static int ax211_scratch_build(struct intel_ax211_dma_resources *resources, uint16_t hardware_revision);
static void ax211_put_le16(uint8_t *output, uint16_t value);
static void ax211_put_le32(uint8_t *output, uint32_t value);
static void ax211_put_le64(uint8_t *output, uint64_t value);
static int ax211_context_build(struct intel_ax211_dma_resources *resources);
static void ax211_buffer_release(struct intel_ax211_dma_resources *resources, struct drv_dma_buffer *buffer);

/*
 * Allocates and fills every DMA buffer a boot of the pinned image needs.
 *
 * On failure every buffer allocated so far is released again.
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
	int supported;

	/* Refuses missing arguments and resources that are already in use. */
	if (device == NULL ||
	    resources == NULL ||
	    firmware_length == 0U ||
	    resources->device != NULL ||
	    resources->boot_prepared) {
		return EINVAL;
	}

	/* Refuses an image whose sections are not laid out as expected. */
	error = ax211_boot_manifest_validate(firmware_bytes, firmware_length,
					     manifest);
	if (error != 0)
		return error;

	/* Refuses a MAC type the image is not built for. */
	supported = drv_intel_ax211_mac_type_supported(
		(uint16_t)((hardware_revision & 0xfff0U) >> 4));
	if (!supported)
		return EINVAL;

	/*
	 * Allocates the rings, the image sections and the receive buffers in
	 * turn.
	 */
	resources->device = device;
	error = ax211_fixed_buffers_allocate(resources);
	if (error == 0) {
		error = ax211_firmware_buffers_allocate(
			resources, firmware_bytes, firmware_length, manifest);
	}

	/* Fills the receive ring, the scratch area and the boot context. */
	if (error == 0)
		error = ax211_rx_buffers_allocate(resources);
	if (error == 0)
		error = ax211_scratch_build(resources, hardware_revision);
	if (error == 0)
		error = ax211_context_build(resources);

	/* Releases everything when a step failed. */
	if (error != 0) {
		drv_intel_ax211_dma_release(resources);
		return error;
	}

	/* Succeeded: the DMA memory is ready for the boot. */
	resources->boot_prepared = 1U;
	return 0;
}

/*
 * Copies the PNVM sections of the board into DMA memory and publishes their
 * address table in the scratch area.
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
	int valid;
	int error = 0;

	/*
	 * Refuses missing arguments, a boot not ready for PNVM, and an
	 * impossible manifest.
	 */
	if (pnvm_bytes == NULL ||
	    manifest == NULL ||
	    resources == NULL ||
	    !resources->boot_prepared ||
	    !resources->boot_images_released ||
	    resources->pnvm_prepared ||
	    pnvm_length != INTEL_AX211_PNVM_FILE_SIZE ||
	    manifest->section_count == 0U ||
	    manifest->section_count > INTEL_AX211_PNVM_SECTION_LIMIT ||
	    manifest->total_length == 0U ||
	    manifest->total_length > UINT32_MAX) {
		return EINVAL;
	}

	/*
	 * Allocates the address table, then copies each section; the first
	 * failure stops.
	 */
	error = ax211_buffer_allocate(resources, &resources->pnvm_table,
				      INTEL_AX211_PNVM_ADDRESS_TABLE_SIZE, 1U);
	for (index = 0U; index < manifest->section_count && error == 0;
	     index++) {
		/* Refuses an empty section. */
		section = &manifest->section[index];
		if (section->length == 0U) {
			error = EINVAL;
			continue;
		}

		/* Refuses a section outside the file. */
		valid = ax211_range_valid(section->file_offset, section->length,
					  pnvm_length);
		if (!valid) {
			error = EINVAL;
			continue;
		}

		/* Refuses a total length that overflows. */
		if (resources->pnvm_total_length > SIZE_MAX - section->length) {
			error = EINVAL;
			continue;
		}

		/* Copies the section into its own buffer. */
		error = ax211_buffer_allocate(resources,
					      &resources->pnvm[index],
					      section->length, 1U);
		if (error == 0) {
			kern_memcpy(resources->pnvm[index].address,
			       pnvm_bytes + section->file_offset,
			       section->length);
			resources->pnvm_total_length += section->length;
			resources->pnvm_count++;
		}
	}

	/*
	 * Refuses sections whose lengths do not add up to the manifest's total.
	 */
	if (error == 0 &&
	    resources->pnvm_total_length != manifest->total_length)
		error = EINVAL;

	/* Releases the sections and the table when anything failed. */
	if (error != 0) {
		/* Releases the sections in reverse order. */
		while (resources->pnvm_count != 0U) {
			resources->pnvm_count--;
			ax211_buffer_release(
				resources,
				&resources->pnvm[resources->pnvm_count]);
		}

		/* Releases the table. */
		resources->pnvm_total_length = 0U;
		ax211_buffer_release(resources, &resources->pnvm_table);
		return error;
	}

	/* Writes the address of every section into the table. */
	table = resources->pnvm_table.address;
	for (index = 0U; index < resources->pnvm_count; index++) {
		ax211_put_le64(table + index * 8U,
			       resources->pnvm[index].device_address);
	}

	/* Publishes the table and the total length in the scratch area. */
	ax211_put_le64((uint8_t *)resources->scratch.address +
			       AX211_SCRATCH_PNVM_BASE_OFFSET,
		       resources->pnvm_table.device_address);
	ax211_put_le32((uint8_t *)resources->scratch.address +
			       AX211_SCRATCH_PNVM_SIZE_OFFSET,
		       (uint32_t)resources->pnvm_total_length);
	resources->pnvm_prepared = 1U;

	/* Succeeded: the PNVM sections are ready for the doorbell. */
	return 0;
}

/*
 * Frees the image loader and the LMAC and UMAC sections once the firmware has
 * copied them; the paged sections stay for the firmware's paging.
 */
void
drv_intel_ax211_dma_release_boot_images(
	struct intel_ax211_dma_resources *resources)
{
	size_t count;

	/* Nothing to release before a prepared boot, or twice. */
	if (resources == NULL ||
	    resources->device == NULL ||
	    !resources->boot_prepared ||
	    resources->boot_images_released) {
		return;
	}

	/* Releases every section that is not paged, in reverse order. */
	count = resources->firmware_count;
	while (count != 0U) {
		count--;

		/* The paged sections stay. */
		if (resources->firmware[count].image_class !=
		    INTEL_AX211_DMA_IMAGE_PAGING) {
			ax211_buffer_release(
				resources, &resources->firmware[count].buffer);
		}
	}

	/* Releases the image loader. */
	ax211_buffer_release(resources, &resources->iml);
	resources->boot_images_released = 1U;
}

/*
 * Frees every DMA buffer of the device and returns the resources to zero.
 */
void
drv_intel_ax211_dma_release(
	struct intel_ax211_dma_resources *resources)
{
	size_t count;

	/* Nothing to release without resources. */
	if (resources == NULL)
		return;

	/* Resources without a device hold no buffer. */
	if (resources->device == NULL) {
		kern_memset(resources, 0, sizeof(*resources));
		return;
	}

	/* Releases the PNVM sections and their table. */
	count = resources->pnvm_count;
	while (count != 0U) {
		count--;
		ax211_buffer_release(resources, &resources->pnvm[count]);
	}

	/* Releases the receive buffers. */
	ax211_buffer_release(resources, &resources->pnvm_table);
	count = resources->rx_buffer_count;
	while (count != 0U) {
		count--;
		ax211_buffer_release(resources, &resources->rx_buffer[count]);
	}

	/* Releases the image sections. */
	count = resources->firmware_count;
	while (count != 0U) {
		count--;
		ax211_buffer_release(resources,
				     &resources->firmware[count].buffer);
	}

	/*
	 * Releases the loader, the rings, the status page, the scratch area and
	 * the context.
	 */
	ax211_buffer_release(resources, &resources->iml);
	ax211_buffer_release(resources, &resources->rx_status);
	ax211_buffer_release(resources, &resources->rx_completion);
	ax211_buffer_release(resources, &resources->rx_transfer);
	ax211_buffer_release(resources, &resources->command_external);
	ax211_buffer_release(resources, &resources->command_slots);
	ax211_buffer_release(resources, &resources->command_byte_count);
	ax211_buffer_release(resources, &resources->command_tfd);
	ax211_buffer_release(resources, &resources->ict);
	ax211_buffer_release(resources, &resources->status_page);
	ax211_buffer_release(resources, &resources->scratch);
	ax211_buffer_release(resources, &resources->context);
	kern_memset(resources, 0, sizeof(*resources));
}

/* Checks that the sections of the image are laid out as the boot expects. */
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
	int valid;

	/*
	 * Refuses a missing or wrong-sized image, group counts out of range or
	 * not adding up with the two marks, and a loader of another size.
	 */
	if (firmware_bytes == NULL ||
	    manifest == NULL ||
	    firmware_length != INTEL_AX211_FIRMWARE_SIZE ||
	    manifest->runtime_count > INTEL_AX211_FIRMWARE_SECTION_LIMIT ||
	    manifest->lmac_count == 0U ||
	    manifest->lmac_count > 64U ||
	    manifest->umac_count == 0U ||
	    manifest->umac_count > 64U ||
	    manifest->paging_count == 0U ||
	    manifest->paging_count > 64U ||
	    manifest->lmac_count + manifest->umac_count +
			    manifest->paging_count + 2U !=
		    manifest->runtime_count ||
	    manifest->iml_length != INTEL_AX211_LOADER_IMAGE_SIZE) {
		return EINVAL;
	}

	/* Refuses a loader outside the image. */
	valid = ax211_range_valid(manifest->iml_offset, manifest->iml_length,
				  firmware_length);
	if (!valid)
		return EINVAL;

	/* The two marks follow the LMAC sections and the UMAC sections. */
	cpu_separator = manifest->lmac_count;
	paging_separator = cpu_separator + manifest->umac_count + 1U;

	/* Checks every section and both marks. */
	for (index = 0U; index < manifest->runtime_count; index++) {
		/* A mark must stand where it is expected and be empty. */
		section = &manifest->runtime[index];
		if (index == cpu_separator) {
			/* Refuses anything but the empty upper-CPU mark. */
			if (section->destination !=
				    INTEL_AX211_UPPER_CPU_IMAGE_MARK ||
			    section->length != 0U) {
				return EINVAL;
			}
		} else if (index == paging_separator) {
			/* Refuses anything but the empty paging mark. */
			if (section->destination !=
				    INTEL_AX211_PAGED_IMAGE_MARK ||
			    section->length != 0U) {
				return EINVAL;
			}
		} else {
			/*
			 * Refuses a stray mark, an empty or oversized section.
			 */
			if (section->destination ==
				    INTEL_AX211_UPPER_CPU_IMAGE_MARK ||
			    section->destination ==
				    INTEL_AX211_PAGED_IMAGE_MARK ||
			    section->length == 0U ||
			    section->length >
				    INTEL_AX211_FIRMWARE_SECTION_SIZE_MAX) {
				return EINVAL;
			}

			/* Refuses a section outside the image. */
			valid = ax211_range_valid(section->file_offset,
						  section->length,
						  firmware_length);
			if (!valid)
				return EINVAL;
		}
	}

	/* Succeeded: the layout is the expected one. */
	return 0;
}

/* Reports whether a range lies inside a buffer: 1 when it does, 0 when not. */
static int
ax211_range_valid(
	size_t offset,
	size_t length,
	size_t capacity)
{
	/* Rejects an offset past the end. */
	if (offset > capacity)
		return 0;

	/* Rejects a length that runs past the end. */
	if (length > capacity - offset)
		return 0;

	/* The range lies inside the buffer. */
	return 1;
}

/*
 * Allocates the boot context, the scratch area, the status page, the
 * interrupt table and the rings; the first failure stops.
 */
static int
ax211_fixed_buffers_allocate(
	struct intel_ax211_dma_resources *resources)
{
	int error;

	/* Allocates the boot context and the scratch area. */
	error = ax211_buffer_allocate(resources, &resources->context,
				      INTEL_AX211_BOOT_CONTEXT_SIZE, 1U);
	if (error == 0) {
		error = ax211_buffer_allocate(resources, &resources->scratch,
					      INTEL_AX211_SCRATCH_AREA_SIZE,
					      1U);
	}

	/* Allocates the status page. */
	if (error == 0) {
		error = ax211_buffer_allocate(resources, &resources->status_page,
					      INTEL_AX211_STATUS_PAGE_SIZE, 1U);
	}

	/* Allocates the interrupt table, aligned to its size. */
	if (error == 0) {
		error = ax211_buffer_allocate(resources, &resources->ict,
					      INTEL_AX211_INTERRUPT_TABLE_SIZE,
					      INTEL_AX211_INTERRUPT_TABLE_SIZE);
	}

	/* Allocates the command TFD ring. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_tfd,
			INTEL_AX211_COMMAND_TFD_RING_SIZE, 256U);
	}

	/* Allocates the command byte-count table. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_byte_count,
			INTEL_AX211_COMMAND_LENGTH_TABLE_SIZE, 128U);
	}

	/* Allocates the command slots. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_slots,
			INTEL_AX211_COMMAND_SLOTS_SIZE, 64U);
	}

	/* Allocates the external command buffer. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->command_external,
			INTEL_AX211_COMMAND_EXTERNAL_SIZE, 64U);
	}

	/* Allocates the receive transfer ring. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->rx_transfer,
			INTEL_AX211_RX_TRANSFER_RING_SIZE, 256U);
	}

	/* Allocates the receive completion ring. */
	if (error == 0) {
		error = ax211_buffer_allocate(
			resources, &resources->rx_completion,
			INTEL_AX211_RX_COMPLETION_RING_SIZE, 256U);
	}

	/* Allocates the receive status word. */
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

/* Allocates one zeroed coherent DMA buffer of an exact size and alignment. */
static int
ax211_buffer_allocate(
	struct intel_ax211_dma_resources *resources,
	struct drv_dma_buffer *buffer,
	size_t size,
	size_t alignment)
{
	int error;

	/* Allocates the buffer. */
	error = drv_dma_alloc_coherent(resources->device, size, alignment,
				       buffer);
	if (error != 0)
		return error;

	/*
	 * Frees a buffer of another size, at the top of the address space or
	 * misaligned.
	 */
	if (buffer->address == NULL ||
	    buffer->size != size ||
	    buffer->device_address > UINT64_MAX - (uint64_t)(size - 1U) ||
	    (alignment > 1U &&
	     (buffer->device_address & (uint64_t)(alignment - 1U)) != 0U)) {
		/* Clears what was allocated before it is freed. */
		if (buffer->address != NULL && buffer->size != 0U)
			AX211_DMA_SCRUB(buffer->address, buffer->size);
		drv_dma_free_coherent(resources->device, buffer);
		return EIO;
	}

	/* Zeroes the buffer. */
	kern_memset(buffer->address, 0, buffer->size);

	/* Succeeded. */
	return 0;
}

/*
 * Copies the image loader and every section of the image into DMA buffers,
 * noting the group each section belongs to.
 */
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
	int valid;
	int error = 0;

	/* Refuses a loader outside the image. */
	valid = ax211_range_valid(manifest->iml_offset, manifest->iml_length,
				  length);
	if (!valid)
		return EINVAL;

	/* Refuses a loader of another size. */
	if (manifest->iml_length != INTEL_AX211_LOADER_IMAGE_SIZE)
		return EINVAL;

	/* Copies the loader. */
	error = ax211_buffer_allocate(resources, &resources->iml,
				      manifest->iml_length, 1U);
	if (error != 0)
		return error;
	kern_memcpy(resources->iml.address, bytes + manifest->iml_offset,
	       manifest->iml_length);

	/*
	 * Copies the sections; a mark switches the group, the first failure
	 * stops.
	 */
	for (index = 0U; index < manifest->runtime_count && error == 0;
	     index++) {
		/* The upper-CPU mark starts the UMAC sections. */
		section = &manifest->runtime[index];
		if (section->destination == INTEL_AX211_UPPER_CPU_IMAGE_MARK) {
			image_class = INTEL_AX211_DMA_IMAGE_UMAC;
			continue;
		}

		/* The paging mark starts the paged sections. */
		if (section->destination == INTEL_AX211_PAGED_IMAGE_MARK) {
			image_class = INTEL_AX211_DMA_IMAGE_PAGING;
			continue;
		}

		/* Refuses an empty or oversized section. */
		if (section->length == 0U ||
		    section->length > INTEL_AX211_FIRMWARE_SECTION_SIZE_MAX) {
			error = EINVAL;
			continue;
		}

		/* Refuses a section outside the image. */
		valid = ax211_range_valid(section->file_offset, section->length,
					  length);
		if (!valid) {
			error = EINVAL;
			continue;
		}

		/* Refuses more sections than the resources hold. */
		if (resources->firmware_count >= INTEL_AX211_FIRMWARE_SECTION_LIMIT) {
			error = EINVAL;
			continue;
		}

		/* Copies the section into its own buffer. */
		image = &resources->firmware[resources->firmware_count];
		error = ax211_buffer_allocate(resources, &image->buffer,
					      section->length, 1U);
		if (error == 0) {
			kern_memcpy(image->buffer.address,
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

/*
 * Allocates the receive buffers and writes a transfer descriptor for each;
 * the first failure stops.
 */
static int
ax211_rx_buffers_allocate(
	struct intel_ax211_dma_resources *resources)
{
	uint8_t *descriptor;
	int result;
	size_t index;
	int error = 0;

	/* Fills every slot of the receive transfer ring. */
	for (index = 0U; index < INTEL_AX211_RX_RING_SIZE && error == 0;
	     index++) {
		/* Allocates the buffer, aligned to its size. */
		error = ax211_buffer_allocate(
			resources, &resources->rx_buffer[index],
			INTEL_AX211_RX_BUFFER_SIZE, INTEL_AX211_RX_BUFFER_SIZE);
		if (error == 0) {
			/*
			 * Writes the descriptor that hands the buffer to the
			 * device.
			 */
			resources->rx_buffer_count++;
			descriptor =
				(uint8_t *)resources->rx_transfer.address +
				index * INTEL_AX211_RX_TRANSFER_DESCRIPTOR_SIZE;
			result = drv_intel_ax211_rx_transfer_descriptor_encode(
				descriptor, (uint16_t)index,
				resources->rx_buffer[index].device_address);
			if (result != INTEL_AX211_SUCCESS)
				error = EINVAL;
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes the scratch area: the header, the receive ring and the section lists.
 */
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

	/* Refuses a missing scratch area. */
	if (scratch == NULL)
		return EINVAL;

	/* Writes the header and the address of the receive transfer ring. */
	kern_memset(scratch, 0, resources->scratch.size);
	ax211_put_le16(scratch, hardware_revision);
	ax211_put_le16(scratch + 2U, 0U);
	ax211_put_le16(scratch + 4U, INTEL_AX211_SCRATCH_AREA_SIZE / 4U);
	ax211_put_le32(scratch + 8U, AX211_SCRATCH_CONTROL_FLAGS);
	ax211_put_le64(scratch + AX211_SCRATCH_RBD_BASE_OFFSET,
		       resources->rx_transfer.device_address);

	/* Lists the address of every section in the list of its group. */
	for (index = 0U; index < resources->firmware_count; index++) {
		/*
		 * Takes the next slot of the section's group, at most 64 per
		 * group.
		 */
		image = &resources->firmware[index];
		if (image->image_class == INTEL_AX211_DMA_IMAGE_LMAC) {
			/* Refuses a 65th LMAC section. */
			if (lmac >= 64U)
				return EOVERFLOW;
			offset = AX211_SCRATCH_LMAC_OFFSET + lmac++ * 8U;
		} else if (image->image_class == INTEL_AX211_DMA_IMAGE_UMAC) {
			/* Refuses a 65th UMAC section. */
			if (umac >= 64U)
				return EOVERFLOW;
			offset = AX211_SCRATCH_UMAC_OFFSET + umac++ * 8U;
		} else if (image->image_class == INTEL_AX211_DMA_IMAGE_PAGING) {
			/* Refuses a 65th paged section. */
			if (paging >= 64U)
				return EOVERFLOW;
			offset = AX211_SCRATCH_PAGING_OFFSET + paging++ * 8U;
		} else {
			return EINVAL;
		}

		/* Writes the section's address. */
		ax211_put_le64(scratch + offset, image->buffer.device_address);
	}

	/* Refuses a boot with an empty group. */
	if (lmac == 0U || umac == 0U || paging == 0U)
		return EINVAL;

	/* Succeeded: the scratch area is complete. */
	return 0;
}

/* Stores a 16-bit value in little-endian byte order. */
static void
ax211_put_le16(
	uint8_t *output,
	uint16_t value)
{
	output[0] = (uint8_t)value;
	output[1] = (uint8_t)(value >> 8);
}

/* Stores a 32-bit value in little-endian byte order. */
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

/* Stores a 64-bit value in little-endian byte order. */
static void
ax211_put_le64(
	uint8_t *output,
	uint64_t value)
{
	ax211_put_le32(output, (uint32_t)value);
	ax211_put_le32(output + 4U, (uint32_t)(value >> 32));
}

/* Writes the boot context that tells the device where every ring is. */
static int
ax211_context_build(
	struct intel_ax211_dma_resources *resources)
{
	struct intel_ax211_boot_context context;
	int result;

	/* Describes every ring and scratch area to the firmware. */
	kern_memset(&context, 0, sizeof(context));
	context.version = AX211_BOOT_CONTEXT_VERSION;
	context.config = AX211_BOOT_CONTEXT_CONFIG;
	context.status_page_address = resources->status_page.device_address;
	context.completion_head_array_address = resources->rx_status.device_address;
	context.transfer_tail_array_address =
		resources->status_page.device_address + 2048U;
	context.completion_tail_array_address =
		resources->status_page.device_address + 3072U;
	context.command_ring_address =
		resources->command_tfd.device_address;
	context.response_ring_address =
		resources->rx_completion.device_address;
	context.command_ring_size_code = INTEL_AX211_COMMAND_RING_CB_SIZE;
	context.response_ring_size_code = INTEL_AX211_RX_RING_CB_SIZE;
	context.scratch_area_address = resources->scratch.device_address;
	context.scratch_area_size = INTEL_AX211_SCRATCH_AREA_SIZE;

	/* Encodes the context into its DMA page. */
	result = drv_intel_ax211_boot_context_encode(
		resources->context.address, &context);
	if (result != INTEL_AX211_SUCCESS)
		return EINVAL;

	/* Succeeded. */
	return 0;
}

/* Clears and frees one DMA buffer, and forgets it. */
static void
ax211_buffer_release(
	struct intel_ax211_dma_resources *resources,
	struct drv_dma_buffer *buffer)
{
	/* Only an allocated buffer is cleared and freed. */
	if (buffer->address != NULL) {
		AX211_DMA_SCRUB(buffer->address, buffer->size);
		drv_dma_free_coherent(resources->device, buffer);
	}

	/* Forgets the buffer. */
	kern_memset(buffer, 0, sizeof(*buffer));
}
