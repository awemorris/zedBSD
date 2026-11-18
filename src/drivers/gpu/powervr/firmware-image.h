/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR firmware file: its trailing description and its program.
 *
 * The file is an ELF program for the GPU's own processor followed by a
 * block that says which core it was built for, how its sections are laid
 * out in the processor's address space, and which errata, enhancements
 * and features the core has.  The driver takes the core's properties
 * from here rather than from a table of its own.
 *
 * Nothing here touches hardware, so the parser runs on the build host as
 * well as in the kernel.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_FIRMWARE_IMAGE_H
#define KERN_DRIVERS_GPU_POWERVR_FIRMWARE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* The most sections the layout table may describe. */
#define POWERVR_FW_SECTION_MAX		8U

/*
 * What a section of the firmware's processor holds.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_info.h
 * (enum pvr_fw_section_id: the MIPS sections are 4 to 9).
 */
#define POWERVR_SECTION_MIPS_CODE	4U
#define POWERVR_SECTION_MIPS_EXCEPTIONS	5U
#define POWERVR_SECTION_MIPS_BOOT_CODE	6U
#define POWERVR_SECTION_MIPS_PRIVATE	7U
#define POWERVR_SECTION_MIPS_BOOT_DATA	8U
#define POWERVR_SECTION_MIPS_STACK	9U

/*
 * Which of the firmware's memory allocations a section lives in.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_info.h
 * (enum pvr_fw_section_type: none 0, code 1, data 2, core code 3, core
 * data 4).
 */
#define POWERVR_MEMORY_NONE		0U
#define POWERVR_MEMORY_CODE		1U
#define POWERVR_MEMORY_DATA		2U
#define POWERVR_MEMORY_CORE_CODE	3U
#define POWERVR_MEMORY_CORE_DATA	4U

/*
 * The positions of the core's features in the firmware's feature mask.
 *
 * Only the features the driver consults are named; the parser keeps all
 * of them.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_dev_info.h
 * (the third enum: 71 features, in this order).
 */
#define POWERVR_FEATURE_GPU_MULTICORE	16U
#define POWERVR_FEATURE_LAYOUT_MARS	23U
#define POWERVR_FEATURE_META		25U
#define POWERVR_FEATURE_MIPS		27U
#define POWERVR_FEATURE_NUM_CLUSTERS	28U
#define POWERVR_FEATURE_PBE2_IN_XE	32U
#define POWERVR_FEATURE_PHYS_BUS_WIDTH	36U
#define POWERVR_FEATURE_RISCV		37U
#define POWERVR_FEATURE_SLC_LINE_BITS	44U
#define POWERVR_FEATURE_SLC_KILOBYTES	46U
#define POWERVR_FEATURE_SECURE_RESET	48U
#define POWERVR_FEATURE_XE_TPU2		65U
#define POWERVR_FEATURE_XT_TOP		69U
#define POWERVR_FEATURE_COUNT		71U

/*
 * The positions of the core's errata in the firmware's errata mask.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif_dev_info.h
 * (the first enum: BRN 63553 is 9, BRN 71242 is 11, 12 in all).
 */
#define POWERVR_QUIRK_63553		9U
#define POWERVR_QUIRK_71242		11U
#define POWERVR_QUIRK_COUNT		12U

/*
 * The errata the firmware's mask cannot name, which the driver keeps
 * itself.
 *
 * Bit 0: the GPU's MMU faults on its 1 MiB and 2 MiB pages, so only 4 KiB
 * pages may be used.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/cores/rgxcore_km_36.50.54.182.h
 * (FIX_HW_BRN_71317) and services/server/devices/rogue/rgxinit.c (what
 * the erratum limits).
 */
#define POWERVR_HOST_QUIRK_SMALL_PAGES	0x1U

/*
 * One section of the firmware as the layout table describes it.
 *
 * Base is its address in the firmware processor's space; the section
 * occupies alloc_size bytes at alloc_offset within the allocation its
 * memory kind names.
 */
struct powervr_fw_section {
	uint32_t id;
	uint32_t memory;
	uint32_t base;
	uint32_t max_size;
	uint32_t alloc_size;
	uint32_t alloc_offset;
};

/*
 * A firmware file checked and described.
 *
 * Bytes points into the caller's copy of the file, which must outlive the
 * description: the program is loaded from it again whenever the core is
 * reset.  The sizes are those of the four allocations the sections are
 * placed in.
 */
struct powervr_fw_image {
	const uint8_t *bytes;
	size_t size;
	uint64_t bvnc;
	uint32_t page_size;
	uint32_t version_major;
	uint32_t version_minor;
	uint32_t version_build;
	uint32_t section_count;
	struct powervr_fw_section sections[POWERVR_FW_SECTION_MAX];
	uint32_t code_size;
	uint32_t data_size;
	uint32_t core_code_size;
	uint32_t core_data_size;
	uint64_t quirks;
	uint64_t enhancements;
	uint64_t features[2];
	uint64_t feature_values[POWERVR_FEATURE_COUNT];
	uint32_t host_quirks;
};

/*
 * Packs a core's branch, version, unit count and configuration the way
 * the firmware names the core it was built for.
 */
uint64_t powervr_bvnc_pack(uint32_t branch, uint32_t version, uint32_t units, uint32_t config);

/*
 * Checks a firmware file and describes it.
 *
 * EINVAL means the file is malformed; ENOEXEC that it is well formed but
 * not usable here: another format version, a closed build, an unknown
 * major version, or a core other than core_bvnc.
 */
int powervr_fw_image_parse(const uint8_t *bytes, size_t size, uint64_t core_bvnc, struct powervr_fw_image *image);

/* Finds a section by its identifier; NULL when the layout has none. */
const struct powervr_fw_section *powervr_fw_image_section(const struct powervr_fw_image *image, uint32_t id);

/* Reports whether the core has a feature. */
int powervr_fw_image_has_feature(const struct powervr_fw_image *image, unsigned feature);

/* Reads the value of a valued feature; ENOENT when the core lacks it. */
int powervr_fw_image_feature_value(const struct powervr_fw_image *image, unsigned feature, uint64_t *value);

/* Reports whether the core has an erratum the firmware names. */
int powervr_fw_image_has_quirk(const struct powervr_fw_image *image, unsigned quirk);

/*
 * Places the program's loadable segments in the code and data
 * allocations.
 *
 * Code and data must be code_size and data_size bytes, already cleared;
 * the core allocations are not supported.  EINVAL means a segment lies
 * outside the file or outside every section.
 */
int powervr_fw_image_load(const struct powervr_fw_image *image, uint8_t *code, uint8_t *data);

#endif
