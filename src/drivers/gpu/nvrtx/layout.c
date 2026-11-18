/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The placement of the GSP firmware in the VRAM, and the start records.
 *
 * The booter puts the GSP firmware into a write-protected region at the top
 * of the VRAM, below the VGA workspace and the region the VBIOS firmware
 * (FWSEC) fills.  The driver computes that placement and writes it into the
 * placement record the booter reads; it also writes the memory regions the
 * GSP's operating system starts with and the resource manager's start
 * arguments.  Nothing here touches the hardware.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/* Units of the placement. */
#define LAYOUT_KIB			0x400ULL
#define LAYOUT_MIB			0x100000ULL
#define LAYOUT_GIB			0x40000000ULL

/*
 * The VRAM-size register: its magnitude and scale fields, and the
 * fifteen-sixteenths flag.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/fb/gp102.c
 * (gp102_fb_vidmem_size).
 */
#define LAYOUT_VRAM_MAGNITUDE_SHIFT	4U
#define LAYOUT_VRAM_MAGNITUDE_MASK	0x3fU
#define LAYOUT_VRAM_SCALE_MASK		0xfU
#define LAYOUT_VRAM_FIFTEEN_SIXTEENTHS	0x40000000U

/*
 * The display's VGA-workspace register: its valid bit and its address field (in
 * 256-byte units).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_vga_workspace_addr).
 */
#define LAYOUT_VGA_VALID		0x8U
#define LAYOUT_VGA_ADDRESS_MASK		0xffffff00U
#define LAYOUT_VGA_ADDRESS_SHIFT	8U

/*
 * The region the VBIOS firmware fills, and the alignment of the region's top.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_oneinit).
 */
#define LAYOUT_FRTS_BYTES		(1ULL * LAYOUT_MIB)
#define LAYOUT_TOP_ALIGN		(128ULL * LAYOUT_KIB)

/*
 * The heap the GSP's resource manager gets, Turing to Ada: a base of 8 MiB,
 * 96 KiB for each GiB of VRAM and room for the clients' allocations, on top
 * of its operating system's part, and at least a least size; both of those
 * are the generation's (its profile).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h
 * (GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X, GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB,
 * GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE) and nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_wpr_heap_size).
 */
#define LAYOUT_HEAP_BASE		(8ULL * LAYOUT_MIB)
#define LAYOUT_HEAP_PER_GIB		(96ULL * LAYOUT_KIB)
#define LAYOUT_HEAP_CLIENTS		(48ULL * LAYOUT_KIB * 2048ULL)

/*
 * The end of the VRAM the firmware has scrubbed by the time the driver
 * starts, Turing to Ada: the GSP's reserved memory, from the heap outside
 * the protected region to the end of the VRAM, must lie inside it, or the
 * resource manager would find memory it did not clear.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c (the 256 MiB
 * the firmware scrubs at the end of the frame buffer) and
 * src/nvidia/generated/g_kernel_gsp_nvoc.c (the same size for Turing,
 * Ampere and Ada).
 */
#define LAYOUT_SCRUBBED_BYTES		(256ULL * LAYOUT_MIB)

/*
 * The heap outside the protected region.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_oneinit).
 */
#define LAYOUT_OUTER_HEAP_BYTES		(1ULL * LAYOUT_MIB)

/*
 * The longest region name the GSP's operating system takes.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_libos_id8).
 */
#define LAYOUT_LIBOS_NAME_MAX		8U

static uint64_t layout_align_down(uint64_t value, uint64_t alignment);
static uint64_t layout_align_up(uint64_t value, uint64_t alignment);

/*
 * Decodes the VRAM size from the VRAM-size register.
 *
 * The size is the magnitude shifted by the scale in MiB, and fifteen
 * sixteenths of that when the flag says so.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/fb/gp102.c (gp102_fb_vidmem_size).
 */
uint64_t
nvrtx_vram_size(
	uint32_t value)
{
	uint64_t magnitude;
	uint32_t scale;
	uint64_t size;

	/* Reads the two fields and multiplies them out. */
	magnitude = (value >> LAYOUT_VRAM_MAGNITUDE_SHIFT) & LAYOUT_VRAM_MAGNITUDE_MASK;
	scale = value & LAYOUT_VRAM_SCALE_MASK;
	size = magnitude << (scale + 20U);

	/* A part with the flag has fifteen sixteenths of the decoded size. */
	if ((value & LAYOUT_VRAM_FIFTEEN_SIXTEENTHS) != 0U)
		return size / 16U * 15U;

	/* Succeeded: the full decoded size. */
	return size;
}

/*
 * Decodes the VRAM size from the register Ampere and later report it in: whole
 * MiB.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/fb/ga102.c (ga102_fb_vidmem_size).
 */
uint64_t
nvrtx_vram_size_mib(
	uint32_t value)
{
	uint64_t size;

	/* The register counts MiB. */
	size = (uint64_t)value << 20;

	/* Succeeded: the size in bytes. */
	return size;
}

/*
 * Finds the VGA workspace from the display's workspace register.
 *
 * Without the valid bit the workspace is the last MiB.  A valid address below
 * the last MiB is not trusted and the last 128 KiB are used instead.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c
 * (tu102_gsp_vga_workspace_addr).
 */
uint64_t
nvrtx_vga_workspace(
	uint64_t vram_size,
	uint32_t value)
{
	uint64_t last_mib;
	uint64_t address;

	/* An invalid register leaves the last MiB to the workspace. */
	last_mib = vram_size - LAYOUT_MIB;
	if ((value & LAYOUT_VGA_VALID) == 0U)
		return last_mib;

	/* A valid register names the workspace's address in 256-byte units. */
	address = (uint64_t)(value & LAYOUT_VGA_ADDRESS_MASK) << LAYOUT_VGA_ADDRESS_SHIFT;

	/* An address below the last MiB is not trusted. */
	if (address < last_mib)
		return vram_size - LAYOUT_TOP_ALIGN;

	/* Succeeded: the register's address. */
	return address;
}

/*
 * Checks that the GSP's reserved memory of a placement lies in the end of the
 * VRAM the firmware scrubbed: from the heap outside the protected region to
 * the end of the VRAM, at most 256 MiB.
 *
 * Returns 0, or ENOSPC when it reaches below the scrubbed end (the boot then
 * stops before it changes anything on the GPU).
 *
 * Procedure referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c (the reserved size measured as
 * the frame buffer's size less the start of the non-WPR heap).
 */
int
nvrtx_wpr_scrubbed(
	const struct nvrtx_wpr_layout *layout)
{
	uint64_t reserved;

	/* From the heap outside the protected region to the end of the VRAM. */
	reserved = layout->vram_size - layout->outer_heap;
	if (reserved > LAYOUT_SCRUBBED_BYTES)
		return ENOSPC;

	/* Succeeded: the firmware cleared all of it. */
	return 0;
}

/*
 * Computes the placement of the GSP firmware below the VGA workspace.
 *
 * heap_os is the part of the heap the GSP's operating system takes and
 * heap_min the least the heap may be (the generation's).  Returns EINVAL
 * when the VRAM is too small to hold the pieces.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_oneinit,
 * tu102_gsp_wpr_heap_size).
 */
int
nvrtx_wpr_layout(
	uint64_t vram_size,
	uint64_t vga,
	uint64_t boot_size,
	uint64_t image_size,
	uint64_t heap_os,
	uint64_t heap_min,
	struct nvrtx_wpr_layout *layout)
{
	uint64_t gib;
	uint64_t heap_size;
	uint64_t needed;

	/* Refuses a VRAM that cannot hold the firmware, the heap and the regions around them. */
	needed = LAYOUT_TOP_ALIGN + LAYOUT_FRTS_BYTES + boot_size + image_size + 256ULL * LAYOUT_MIB;
	if (vga > vram_size || vga < needed)
		return EINVAL;

	/* The workspace runs from its address to the end of the VRAM. */
	layout->vram_size = vram_size;
	layout->vga = vga;
	layout->vga_size = vram_size - vga;

	/* The VBIOS firmware's region ends at the workspace rounded down to 128 KiB. */
	layout->frts_size = LAYOUT_FRTS_BYTES;
	layout->frts = layout_align_down(vga, LAYOUT_TOP_ALIGN) - layout->frts_size;

	/* The bootloader below it on a page, the firmware image below that on 64 KiB. */
	layout->boot_size = boot_size;
	layout->boot = layout_align_down(layout->frts - boot_size, 4ULL * LAYOUT_KIB);
	layout->image_size = image_size;
	layout->image = layout_align_down(layout->boot - image_size, 64ULL * LAYOUT_KIB);

	/* The heap grows with the VRAM and holds the clients' allocations. */
	gib = (vram_size + LAYOUT_GIB - 1U) / LAYOUT_GIB;
	heap_size = heap_os;
	heap_size += LAYOUT_HEAP_BASE;
	heap_size += layout_align_up(LAYOUT_HEAP_PER_GIB * gib, LAYOUT_MIB);
	heap_size += layout_align_up(LAYOUT_HEAP_CLIENTS, LAYOUT_MIB);

	/* A small VRAM still gives the heap its minimum. */
	if (heap_size < heap_min)
		heap_size = heap_min;

	/* Refuses a VRAM whose room below the image cannot hold the heap, the record and the outer heap. */
	if (layout->image < heap_size + 4ULL * LAYOUT_MIB)
		return EINVAL;

	/* The heap sits below the image on a MiB and fills the gap up to it. */
	layout->heap = layout_align_down(layout->image - heap_size, LAYOUT_MIB);
	layout->heap_size = layout_align_down(layout->image - layout->heap, LAYOUT_MIB);

	/* The protected region starts below the heap, leaving room for the placement record. */
	layout->region = layout_align_down(layout->heap - NVRTX_WPRM_BYTES, LAYOUT_MIB);
	layout->region_size = layout->frts + layout->frts_size - layout->region;

	/* The heap outside the protected region is the MiB below it. */
	layout->outer_heap_size = LAYOUT_OUTER_HEAP_BYTES;
	layout->outer_heap = layout->region - layout->outer_heap_size;

	/* Succeeded: every piece has its place. */
	return 0;
}

/*
 * Writes the placement record the booter reads.
 *
 * The record is NVRTX_WPRM_BYTES long; fields not written here are zero.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/tu102.c (tu102_gsp_wpr_meta_init).
 */
void
nvrtx_wpr_record(
	uint8_t *record,
	const struct nvrtx_wpr_layout *layout,
	const struct nvrtx_wpr_sources *sources)
{
	size_t at;

	/* Clears the record, so every field this driver does not set is zero. */
	for (at = 0U; at < NVRTX_WPRM_BYTES; at++)
		record[at] = 0U;

	/* Identifies the record. */
	nvrtx_put64(record, NVRTX_WPRM_MAGIC_AT, NVRTX_WPRM_MAGIC);
	nvrtx_put64(record, NVRTX_WPRM_REVISION_AT, NVRTX_WPRM_REVISION);

	/* Points at the firmware image's three-level table and gives the image's size. */
	nvrtx_put64(record, NVRTX_WPRM_IMAGE_TABLE_AT, sources->image_table);
	nvrtx_put64(record, NVRTX_WPRM_IMAGE_SIZE_AT, layout->image_size);

	/* Points at the bootloader and names its code, data and manifest. */
	nvrtx_put64(record, NVRTX_WPRM_BOOT_ADDRESS_AT, sources->boot_address);
	nvrtx_put64(record, NVRTX_WPRM_BOOT_SIZE_AT, sources->boot_size);
	nvrtx_put64(record, NVRTX_WPRM_BOOT_CODE_AT, sources->bootloader->monitor_code_at);
	nvrtx_put64(record, NVRTX_WPRM_BOOT_DATA_AT, sources->bootloader->monitor_data_at);
	nvrtx_put64(record, NVRTX_WPRM_BOOT_MANIFEST_AT, sources->bootloader->manifest_at);

	/* Points at the image's signature. */
	nvrtx_put64(record, NVRTX_WPRM_SIGNATURE_AT, sources->signature);
	nvrtx_put64(record, NVRTX_WPRM_SIGNATURE_SIZE_AT, sources->signature_size);

	/* Gives the outer heap, which is also where the firmware's reservation starts. */
	nvrtx_put64(record, NVRTX_WPRM_RESERVED_START_AT, layout->outer_heap);
	nvrtx_put64(record, NVRTX_WPRM_OUTER_HEAP_AT, layout->outer_heap);
	nvrtx_put64(record, NVRTX_WPRM_OUTER_HEAP_SIZE_AT, layout->outer_heap_size);

	/* Gives the protected region's pieces. */
	nvrtx_put64(record, NVRTX_WPRM_REGION_START_AT, layout->region);
	nvrtx_put64(record, NVRTX_WPRM_HEAP_AT, layout->heap);
	nvrtx_put64(record, NVRTX_WPRM_HEAP_SIZE_AT, layout->heap_size);
	nvrtx_put64(record, NVRTX_WPRM_IMAGE_AT, layout->image);
	nvrtx_put64(record, NVRTX_WPRM_BOOT_AT, layout->boot);
	nvrtx_put64(record, NVRTX_WPRM_FRTS_AT, layout->frts);
	nvrtx_put64(record, NVRTX_WPRM_FRTS_SIZE_AT, layout->frts_size);
	nvrtx_put64(record, NVRTX_WPRM_REGION_END_AT, layout_align_down(layout->vga, LAYOUT_TOP_ALIGN));

	/* Gives the VRAM size and the VGA workspace; the boot count starts at zero. */
	nvrtx_put64(record, NVRTX_WPRM_VRAM_SIZE_AT, layout->vram_size);
	nvrtx_put64(record, NVRTX_WPRM_VGA_AT, layout->vga);
	nvrtx_put64(record, NVRTX_WPRM_VGA_SIZE_AT, layout->vga_size);
	nvrtx_put64(record, NVRTX_WPRM_BOOT_COUNT_AT, 0U);
}

/*
 * Writes one memory region of the GSP operating system's start page.
 *
 * The name is packed into a word with its first character in the highest
 * used byte; the region is one contiguous range in system memory.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_libos_init,
 * r535_gsp_libos_id8).
 */
void
nvrtx_libos_region(
	uint8_t *page,
	unsigned index,
	const char *name,
	uint64_t address,
	uint64_t size)
{
	uint64_t packed;
	size_t at;
	unsigned position;

	/* Packs up to eight characters, each one shifting the earlier ones up. */
	packed = 0U;
	for (position = 0U; position < LAYOUT_LIBOS_NAME_MAX; position++) {
		/* A shorter name ends at its terminating zero. */
		if (name[position] == '\0')
			break;

		/* Shifts the earlier characters up and adds this one. */
		packed = (packed << 8) | (uint8_t)name[position];
	}

	/* Writes the region's entry. */
	at = (size_t)index * NVRTX_LIBOS_REGION_BYTES;
	nvrtx_put64(page, at + NVRTX_LIBOS_NAME_AT, packed);
	nvrtx_put64(page, at + NVRTX_LIBOS_ADDRESS_AT, address);
	nvrtx_put64(page, at + NVRTX_LIBOS_SIZE_AT, size);
	nvrtx_put8(page, at + NVRTX_LIBOS_KIND_AT, NVRTX_LIBOS_KIND_CONTIGUOUS);
	nvrtx_put8(page, at + NVRTX_LIBOS_LOCATION_AT, NVRTX_LIBOS_LOCATION_SYSTEM);
}

/*
 * Prepares one log buffer the GSP writes.
 *
 * The first word is the GSP's put pointer, zero at start; the words after it
 * list the bus address of each page of the buffer, which is contiguous.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_libos_init,
 * create_pte_array).
 */
void
nvrtx_log_buffer(
	uint8_t *buffer,
	uint64_t bus,
	size_t size)
{
	size_t at;
	size_t page;

	/* Clears the buffer, so the put pointer and the log start at zero. */
	for (at = 0U; at < size; at++)
		buffer[at] = 0U;

	/* Lists the pages after the put pointer. */
	for (page = 0U; page < size / NVRTX_GSP_PAGE_SIZE; page++)
		nvrtx_put64(buffer, 8U + page * 8U, bus + (uint64_t)page * NVRTX_GSP_PAGE_SIZE);
}

/*
 * Writes the resource manager's start arguments for a cold start.
 *
 * The queues' page list and its length, the two queues' offsets in the
 * shared memory, no suspend-resume state, GPU instance 0, and the stack kept
 * in the GSP's data memory, which this firmware version expects.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/gsp.c (r570_gsp_set_rmargs).
 */
void
nvrtx_rmargs(
	uint8_t *args,
	uint64_t queue_pages,
	uint32_t pte_count)
{
	size_t at;

	/* Clears the arguments, so the suspend-resume state and the profiler are zero. */
	for (at = 0U; at < NVRTX_RMARGS_BYTES; at++)
		args[at] = 0U;

	/* Describes the queues. */
	nvrtx_put64(args, NVRTX_RMARGS_QUEUE_PAGES_AT, queue_pages);
	nvrtx_put32(args, NVRTX_RMARGS_QUEUE_PTE_COUNT_AT, pte_count);
	nvrtx_put64(args, NVRTX_RMARGS_COMMAND_AT, NVRTX_QUEUE_COMMAND_AT);
	nvrtx_put64(args, NVRTX_RMARGS_STATUS_AT, NVRTX_QUEUE_STATUS_AT);

	/* Keeps the stack in the GSP's data memory. */
	nvrtx_put8(args, NVRTX_RMARGS_DMEM_STACK_AT, 1U);
}

/* Rounds a value down to a power-of-two alignment. */
static uint64_t
layout_align_down(
	uint64_t value,
	uint64_t alignment)
{
	/* Succeeded: the value with its low bits cleared. */
	return value & ~(alignment - 1U);
}

/* Rounds a value up to a power-of-two alignment. */
static uint64_t
layout_align_up(
	uint64_t value,
	uint64_t alignment)
{
	/* Succeeded: the value raised to the next boundary. */
	return (value + alignment - 1U) & ~(alignment - 1U);
}
