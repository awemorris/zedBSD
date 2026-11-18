/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout of the Mali command stream firmware file.
 *
 * The file is a header and a list of entries; the entries that matter
 * name sections of the microcontroller's address space, where they go and
 * which bytes of the file fill them.  The parser only reads and checks;
 * it allocates nothing, so it runs unchanged in a host test.
 */

#ifndef KERN_DRIVERS_GPU_MALI_FIRMWARE_IMAGE_H
#define KERN_DRIVERS_GPU_MALI_FIRMWARE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* The most sections one image may describe, and the longest name kept. */
#define MALI_FW_SECTIONS_MAX		32U
#define MALI_FW_NAME_BYTES		32U

/*
 * A section's flags: readable, writable, executable, how it is cached,
 * protected-mode only, the shared interface, and zero-filled past its
 * data.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (CSF_FW_BINARY_IFACE_ENTRY_RD, _WR, _EX, _CACHE_MODE_NONE, _CACHED,
 * _UNCACHED_COHERENT, _CACHED_COHERENT, _CACHE_MODE_MASK, _PROT,
 * _SHARED, _ZERO).
 */
#define MALI_FW_SECTION_READ		0x00000001U
#define MALI_FW_SECTION_WRITE		0x00000002U
#define MALI_FW_SECTION_EXECUTE		0x00000004U
#define MALI_FW_SECTION_CACHE_MASK	0x00000018U
#define MALI_FW_SECTION_CACHE_NONE	0x00000000U
#define MALI_FW_SECTION_CACHED		0x00000008U
#define MALI_FW_SECTION_UNCACHED_COHERENT	0x00000010U
#define MALI_FW_SECTION_CACHED_COHERENT	0x00000018U
#define MALI_FW_SECTION_PROTECTED	0x00000020U
#define MALI_FW_SECTION_SHARED		0x40000000U
#define MALI_FW_SECTION_ZERO		0x80000000U

/*
 * Where the shared interface between the host and the firmware lives in
 * the microcontroller's address space, and the size of that region.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (CSF_MCU_SHARED_REGION_START, CSF_MCU_SHARED_REGION_SIZE).
 */
#define MALI_FW_SHARED_START		0x04000000U
#define MALI_FW_SHARED_SIZE		0x04000000U

/*
 * One section of the microcontroller's address space.
 *
 * The section covers [va_start, va_end) and is filled from the file's
 * bytes [data_start, data_end), the rest zero when the ZERO flag says so.
 */
struct mali_fw_section {
	uint32_t flags;
	uint32_t va_start;
	uint32_t va_end;
	uint32_t data_start;
	uint32_t data_end;
	char name[MALI_FW_NAME_BYTES];
};

/*
 * What the parser found in one image.
 *
 * Shared is the index of the section of the shared interface.  Build
 * information is the offset and length of the firmware's build text, both
 * zero when the image has none.  Skipped counts the protected-mode
 * sections the driver does not load.
 */
struct mali_fw_image {
	uint8_t minor;
	uint8_t major;
	uint32_t version_hash;
	uint32_t size;
	unsigned section_count;
	unsigned shared;
	unsigned skipped;
	uint32_t build_info_start;
	uint32_t build_info_size;
	struct mali_fw_section sections[MALI_FW_SECTIONS_MAX];
};

/*
 * Reads and checks a whole firmware file; the sections must lie on page
 * boundaries of page_bytes.
 */
int mali_fw_image_parse(const uint8_t *bytes, size_t size, uint32_t page_bytes, struct mali_fw_image *image);

#endif
