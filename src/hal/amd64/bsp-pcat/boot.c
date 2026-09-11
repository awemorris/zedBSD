/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 PC/AT ZBL6 handoff and boot-memory discovery path.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "bootloader/include/amd64-handoff.h"
#include "../../x86/boot-parameters.h"
#include "../bsp.h"
#include "../defs.h"
#include "handoff-validation.h"

#define VGA_FONT_HANDOFF       0x00007000U
#define VGA_FONT_MAGIC         0x3854465aU
#define MAX_BOOT_MEMORY_RANGES 256U
#define PCAT_BOOT_FONT_GLYPHS  128U
#define PCAT_BOOT_FONT_HEIGHT  16U

struct vga_font_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t height;
	uint16_t glyphs;
	uint16_t reserved[3];
	uint8_t data[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
} __attribute__((packed));

typedef char zbl6_kernel_mbr_partition_scheme_must_match[
	ZBL6_PARTITION_SCHEME_MBR == ZEDBSD_PARTITION_SCHEME_MBR ? 1 : -1];
typedef char zbl6_kernel_gpt_partition_scheme_must_match[
	ZBL6_PARTITION_SCHEME_GPT == ZEDBSD_PARTITION_SCHEME_GPT ? 1 : -1];
typedef char zbl6_kernel_unknown_partition_index_must_match[
	ZBL6_PARTITION_INDEX_UNKNOWN == ZEDBSD_PARTITION_INDEX_UNKNOWN ? 1 : -1];

static struct zbl6_handoff boot_info;
static struct zbl6_handoff_v2 boot_info_v2;
static struct zbl6_framebuffer boot_framebuffer;
static struct zbl6_memory_range_v6 boot_memory_range[MAX_BOOT_MEMORY_RANGES];
static uint32_t boot_memory_range_count;
static struct zbl6_memory_handoff boot_memory;
static struct zbl6_boot_allocation boot_allocations[ZBL6_MAX_BOOT_ALLOCATIONS];
static struct boot_handoff kernel_handoff;
static uint64_t total_memory;
static uint8_t boot_font[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
static int boot_font_valid;
static char boot_selector[15];
static char boot_parameters[ZEDBSD_BOOT_PARAMETERS_STORAGE_SIZE];

static void accept_memory(const struct zbl6_memory_handoff *memory, uint32_t source);
static int handoff_name_is(const char *name, const char *expected);
static char hex_digit(unsigned value);
static void set_boot_uuid(uint32_t serial);
static void accept_framebuffer(uint64_t base, uint64_t size, uint32_t width, uint32_t height, uint32_t stride, uint32_t format);

/*
 * Validates and preserves the amd64 bootloader handoff.
 */
void
prekern_bsp_boot_init(
	const void *raw_boot_info)
{
	const struct zbl6_handoff *raw;
	const struct zbl6_handoff_v2 *raw_v2;
	const struct zbl6_handoff_v3 *raw_v3;
	const struct zbl6_handoff_v4 *raw_v4;
	const struct zbl6_handoff_v5_bios *raw_v5_bios;
	const struct zbl6_handoff_v5_uefi *raw_v5_uefi;
	const struct zbl6_handoff_framebuffer *raw_framebuffer;
	const struct vga_font_handoff *font;
	const struct zbl6_memory_range *source;
	const struct zbl6_memory_handoff *memory;
	uint64_t previous_end;
	uint64_t highest;
	uint64_t end;
	uint32_t index;
	int bios_form;
	int v5;
	int v6;
	int framebuffer_version;
	int uuid_version;
	int partition_valid;
	enum zbl6_handoff_form form;
	enum x86_boot_parameters_result parameter_result;

	/* Interprets the fixed handoff address through each supported version. */
	raw = raw_boot_info;
	raw_v2 = raw_boot_info;
	raw_v3 = raw_boot_info;
	raw_v4 = raw_boot_info;
	raw_v5_bios = raw_boot_info;
	raw_v5_uefi = raw_boot_info;
	font = (const struct vga_font_handoff *)(uintptr_t)VGA_FONT_HANDOFF;
	highest = 0;
	bios_form = 0;

	/* Requires the common ZBL6 magic before any version-specific access. */
	if (raw == NULL || raw->magic != ZBL6_HANDOFF_MAGIC)
		HAL_FATAL("invalid amd64 ZBL6 handoff");

	/* Clears persistent copies before classifying the incoming handoff. */
	hal_memset(&boot_info, 0, sizeof(boot_info));
	hal_memset(&boot_info_v2, 0, sizeof(boot_info_v2));
	hal_memset(&boot_framebuffer, 0, sizeof(boot_framebuffer));
	hal_memset(boot_selector, 0, sizeof(boot_selector));
	(void)kern_boot_provenance_set(NULL);
	form = zbl6_handoff_classify_raw(raw_boot_info);

	/* Rejects unsupported version, size, or flag combinations. */
	if (form == ZBL6_HANDOFF_FORM_INVALID)
		HAL_FATAL("unsupported amd64 ZBL6 handoff");

	v6 = form == ZBL6_HANDOFF_FORM_V6_BIOS ||
	    form == ZBL6_HANDOFF_FORM_V6_UEFI || form == ZBL6_HANDOFF_FORM_V7_UEFI;
	hal_memset(&boot_memory, 0, sizeof(boot_memory));
	if (form == ZBL6_HANDOFF_FORM_V7_UEFI) {
		struct boot_provenance provenance;

		/* The raw packed tail may be unaligned; retain an aligned copy. */
		hal_memcpy(&provenance,
		    (const uint8_t *)raw + ZBL6_HANDOFF_V6_UEFI_SIZE,
		    sizeof(provenance));
		if (kern_boot_provenance_set(&provenance) != 0)
			HAL_FATAL("invalid amd64 boot provenance");
	}


	/* Preserves BIOS and UEFI forms through their distinct contracts. */
	if (form == ZBL6_HANDOFF_FORM_LEGACY_BIOS ||
	    form == ZBL6_HANDOFF_FORM_V5_BIOS || form == ZBL6_HANDOFF_FORM_V6_BIOS) {
		raw_framebuffer = raw_boot_info;
		v5 = form == ZBL6_HANDOFF_FORM_V5_BIOS || v6;

		/* Validates the legacy BIOS drive and partition ordinals. */
		if (raw->boot_drive < 0x80U ||
		    raw->partition_index < 1U ||
		    raw->partition_index > 4U)
			HAL_FATAL("invalid amd64 ZBL6 v1 handoff");

		/* Preserves the common BIOS handoff and selector identity. */
		boot_info = *raw;
		bios_form = 1;
		if ((raw->flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) != 0)
			set_boot_uuid(raw->reserved);

		/* Validates and preserves an optional BIOS framebuffer. */
		if ((raw->flags & ZBL6_HANDOFF_FLAG_FRAMEBUFFER) != 0) {
			/* Rejects a legacy envelope too short for framebuffer fields. */
			if (!v5 && raw->size < sizeof(*raw_framebuffer)) {
				HAL_FATAL(
					"truncated amd64 BIOS framebuffer handoff");
			}
			accept_framebuffer(
				raw_framebuffer->framebuffer_base,
				raw_framebuffer->framebuffer_size,
				raw_framebuffer->framebuffer_width,
				raw_framebuffer->framebuffer_height,
				raw_framebuffer->framebuffer_stride,
				raw_framebuffer->framebuffer_format);
		}

		/* Copies V5 parameters or establishes an empty legacy record. */
		if (v5) {
			parameter_result = x86_boot_parameter_record_copy(
				boot_parameters,
				&raw_v5_bios->parameters,
				sizeof(raw_v5_bios->parameters));
		} else {
			parameter_result = x86_boot_parameters_copy(
				boot_parameters,
				NULL,
				0U);
		}

		/* Requires an accepted BIOS boot-parameter record. */
		if (parameter_result != X86_BOOT_PARAMETERS_OK)
			HAL_FATAL("invalid amd64 BIOS boot parameters");

		if (v6) {
			accept_memory(&((const struct zbl6_handoff_v6_bios *)raw)->memory,
			    ZBL6_MEMORY_SOURCE_BIOS_E820);
		} else {
			/* Builds the single legacy usable-memory range below one GiB. */
			total_memory = 0x100000ULL +
			    (uint64_t)boot_info.mem_upper_kib * 1024ULL;
			if (total_memory > 0x40000000ULL)
				total_memory = 0x40000000ULL;
			boot_memory_range_count = 1;
			boot_memory_range[0].base = 0;
			boot_memory_range[0].size = total_memory;
			boot_memory_range[0].type = ZBL6_MEMORY_USABLE;
			boot_memory_range[0].flags = 0;
			boot_memory_range[0].attributes = 0;
		}
	} else {
		previous_end = 0;
		framebuffer_version =
		    raw->version == ZBL6_HANDOFF_V3_VERSION ||
		    raw->version == ZBL6_HANDOFF_V4_VERSION ||
		    raw->version == ZBL6_HANDOFF_V5_VERSION || v6;
		uuid_version = raw->version == ZBL6_HANDOFF_V4_VERSION ||
		    raw->version == ZBL6_HANDOFF_V5_VERSION || v6;

		/* Rejects an invalid UEFI boot drive before partition validation. */
		if (raw_v2->boot_drive < 0x80U)
			HAL_FATAL("invalid amd64 ZBL6 v2 handoff");
		partition_valid = zbl6_uefi_partition_handoff_valid(
			raw->version,
			raw_v2->root_partition_scheme,
			raw_v2->root_partition_index,
			raw_v2->loader_partition_index,
			raw_v2->flags);

		/* Validates UEFI partition, memory, kernel, and ACPI geometry. */
		if (!partition_valid ||
		    raw_v2->memory_range_count == 0 ||
		    raw_v2->memory_range_count > MAX_BOOT_MEMORY_RANGES ||
		    raw_v2->memory_range_entry_size !=
		    (v6 ? sizeof(struct zbl6_memory_range_v6) : sizeof(struct zbl6_memory_range)) ||
		    (raw_v2->memory_ranges & 7U) != 0 ||
		    raw_v2->memory_ranges >= 0x40000000ULL ||
		    raw_v2->memory_range_count >
		    (0x40000000ULL - raw_v2->memory_ranges) /
		    (v6 ? sizeof(struct zbl6_memory_range_v6) : sizeof(struct zbl6_memory_range)) ||
		    raw_v2->kernel_phys_start != 0x00200000ULL ||
		    raw_v2->kernel_phys_end <= raw_v2->kernel_phys_start ||
		    raw_v2->kernel_phys_end > 0x01200000ULL ||
		    (raw_v2->bootstrap_cr3 & 0xfffU) != 0 ||
		    raw_v2->bootstrap_cr3 >= 0x40000000ULL ||
		    raw_v2->rsdp == 0)
			HAL_FATAL("invalid amd64 ZBL6 v2 handoff");

		/* Requires the framebuffer flag for versions that carry it. */
		if (framebuffer_version &&
		    (raw_v3->flags & ZBL6_HANDOFF_FLAG_FRAMEBUFFER) == 0U)
			HAL_FATAL("invalid amd64 framebuffer handoff");

		/* Requires the UUID flag for versions that carry its serial. */
		if (uuid_version &&
		    (raw_v4->common.flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) == 0U)
			HAL_FATAL("missing amd64 boot UUID handoff");

		/* Preserves the common UEFI handoff and optional extensions. */
		boot_info_v2 = *raw_v2;
		if (framebuffer_version) {
			accept_framebuffer(
				raw_v3->framebuffer_base,
				raw_v3->framebuffer_size,
				raw_v3->framebuffer_width,
				raw_v3->framebuffer_height,
				raw_v3->framebuffer_stride,
				raw_v3->framebuffer_format);
		}

		/* Preserves the optional boot-volume serial when advertised. */
		if (uuid_version)
			set_boot_uuid(raw_v4->boot_volume_serial);

		/* Copies V5 parameters or establishes an empty legacy record. */
		if (raw->version == ZBL6_HANDOFF_V5_VERSION || v6) {
			parameter_result = x86_boot_parameter_record_copy(
				boot_parameters,
				&raw_v5_uefi->parameters,
				sizeof(raw_v5_uefi->parameters));
		} else {
			parameter_result = x86_boot_parameters_copy(
				boot_parameters,
				NULL,
				0U);
		}

		/* Requires an accepted UEFI boot-parameter record. */
		if (parameter_result != X86_BOOT_PARAMETERS_OK)
			HAL_FATAL("invalid amd64 UEFI boot parameters");

		if (v6) {
			memory = &((const struct zbl6_handoff_v6_uefi *)raw)->memory;
			if (memory->range_count != raw_v2->memory_range_count ||
			    memory->range_entry_size != raw_v2->memory_range_entry_size ||
			    memory->ranges != raw_v2->memory_ranges ||
			    memory->bootstrap_cr3 != raw_v2->bootstrap_cr3 ||
			    memory->kernel_phys_start != raw_v2->kernel_phys_start ||
			    memory->kernel_phys_end != raw_v2->kernel_phys_end)
				HAL_FATAL("inconsistent amd64 memory handoff");
			accept_memory(memory, ZBL6_MEMORY_SOURCE_UEFI);
		} else {
			/* Copies and validates the ordered UEFI memory map. */
			source = (const void *)(uintptr_t)raw_v2->memory_ranges;
			boot_memory_range_count = raw_v2->memory_range_count;
			for (index = 0; index < boot_memory_range_count; index++) {
				boot_memory_range[index].base = source[index].base;
				boot_memory_range[index].size = source[index].size;
				boot_memory_range[index].type = source[index].type;
				boot_memory_range[index].flags = source[index].flags;
				boot_memory_range[index].attributes = 0;

				/* Validates this aligned, bounded, and recognized range. */
				if (boot_memory_range[index].size == 0 ||
				    (boot_memory_range[index].base & 0xfffU) != 0 ||
				    (boot_memory_range[index].size & 0xfffU) != 0 ||
				    boot_memory_range[index].base >
				    UINT64_MAX - boot_memory_range[index].size ||
				    boot_memory_range[index].type < ZBL6_MEMORY_USABLE ||
				    boot_memory_range[index].type > ZBL6_MEMORY_MMIO)
					HAL_FATAL("invalid amd64 memory range");

				/* Requires ascending, nonoverlapping ranges. */
				end = boot_memory_range[index].base +
				    boot_memory_range[index].size;
				if (index != 0 &&
				    boot_memory_range[index].base < previous_end)
					HAL_FATAL("overlapping amd64 memory ranges");
				previous_end = end;

				/* Tracks the highest end of usable memory. */
				if (end > highest &&
				    boot_memory_range[index].type == ZBL6_MEMORY_USABLE)
					highest = end;
			}

			/* Reports full firmware geometry; the allocator applies its own limit. */
			total_memory = highest;
		}
	}

	/* Requires enough memory for the loaded kernel and initial services. */
	if (total_memory < 0x00400000ULL)
		HAL_FATAL("too little amd64 memory");

	/* Validates and preserves the optional low-memory VGA font. */
	boot_font_valid = font->magic == VGA_FONT_MAGIC &&
	    font->version == 1 &&
	    font->height == PCAT_BOOT_FONT_HEIGHT &&
	    font->glyphs == PCAT_BOOT_FONT_GLYPHS;
	if (boot_font_valid)
		hal_memcpy(boot_font, font->data, sizeof(boot_font));

	/* Builds the architecture-independent kernel handoff. */
	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = ZEDBSD_HANDOFF_MAGIC;
	kernel_handoff.version = ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	if (bios_form) {
		kernel_handoff.boot_bios_id = boot_info.boot_drive;
		kernel_handoff.boot_partition_scheme =
		    ZEDBSD_PARTITION_SCHEME_MBR;
		kernel_handoff.boot_partition_index = boot_info.partition_index;
	} else {
		kernel_handoff.boot_bios_id = boot_info_v2.boot_drive;
		kernel_handoff.boot_partition_scheme =
		    boot_info_v2.root_partition_scheme;
		kernel_handoff.boot_partition_index =
		    boot_info_v2.root_partition_index;
	}
}

/*
 * Returns one named architecture handoff object.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	int match;

	/* Exposes the canonical boot command-line buffer. */
	match = handoff_name_is(name, "boot.command-line");
	if (match)
		return boot_parameters;

	/* Exposes a boot selector only when one was supplied. */
	if (boot_selector[0] != '\0') {
		/* Returns the selector only for its stable handoff name. */
		match = handoff_name_is(name, "boot.selector");
		if (match)
			return boot_selector;
	}

	/* Exposes the boot font only after fixed-layout validation. */
	if (boot_font_valid) {
		/* Returns the font only for its stable handoff name. */
		match = handoff_name_is(name, "pcat.boot-font");
		if (match)
			return boot_font;
	}

	/* Exposes the framebuffer only when a nonempty extent was accepted. */
	if (boot_framebuffer.size != 0) {
		/* Returns the framebuffer only for its stable handoff name. */
		match = handoff_name_is(name, "pcat.framebuffer");
		if (match)
			return &boot_framebuffer;
	}

	/* Reports an unavailable or unknown architecture handoff. */
	return NULL;
}

/*
 * Reports allocator-visible physical memory.
 */
uint64_t
bsp_mem_probe(
	void)
{
	/* Returns the highest usable firmware address, including holes. */
	return total_memory;
}

/*
 * Reports the number of validated boot memory ranges.
 */
uint32_t
bsp_mem_range_count(
	void)
{
	/* Returns the persistent memory-map entry count. */
	return boot_memory_range_count;
}

/*
 * Copies one validated boot memory range.
 */
int
bsp_mem_range(
	uint32_t index,
	uint64_t *base,
	uint64_t *size,
	uint32_t *type)
{
	/* Validates the range index and every result destination. */
	if (index >= boot_memory_range_count ||
	    base == NULL ||
	    size == NULL ||
	    type == NULL)
		return 0;

	/* Copies the range fields required by the page allocator. */
	*base = boot_memory_range[index].base;
	*size = boot_memory_range[index].size;
	*type = boot_memory_range[index].type;

	/* Reports a valid range index. */
	return 1;
}

/*
 * Reports whether firmware memory may be mapped for ACPI discovery.
 */
int
bsp_physical_range_mappable(
	uint64_t physical,
	size_t size)
{
	uint64_t end;
	uint64_t range_end;
	uint32_t type;
	uint32_t index;
	int legacy;

	/* Classifies the handoff before validating the requested extent. */
	legacy = boot_info_v2.magic != ZBL6_HANDOFF_MAGIC && boot_memory.source == 0;
	if (size == 0 || physical > UINT64_MAX - size)
		return 0;
	end = physical + size;
	/* BIOS ACPI discovery reads the BDA, EBDA and ROM search window.
	 * The entire low MiB stays reserved independently of E820 types. */
	if (boot_memory.source == ZBL6_MEMORY_SOURCE_BIOS_E820 && end <= 0x100000U)
		return 1;

	/*
	 * The v1 BIOS handoff has only a usable-memory total. SeaBIOS places
	 * checksum-protected ACPI tables in the reserved gap just above that
	 * total, so retain the historical sub-one-GiB readable extent.
	 */
	if (legacy) {
		/* Accepts a legacy request within the readable extent. */
		if (end <= 0x40000000ULL)
			return 1;

		/* Rejects a legacy request beyond the readable extent. */
		return 0;
	}

	/* Searches reserved and ACPI-classified firmware ranges. */
	for (index = 0; index < boot_memory_range_count; index++) {
		range_end = boot_memory_range[index].base +
		    boot_memory_range[index].size;
		type = boot_memory_range[index].type;

		/* Accepts a request contained in an allowed firmware range. */
		if (boot_memory_range[index].base <= physical &&
		    end <= range_end &&
		    (type == ZBL6_MEMORY_RESERVED ||
		    type == ZBL6_MEMORY_BOOT_RECLAIM ||
		    type == ZBL6_MEMORY_ACPI_RECLAIM ||
		    type == ZBL6_MEMORY_ACPI_NVS))
			return 1;
	}

	/* Rejects ranges outside permitted firmware memory. */
	return 0;
}

/*
 * Reports the bootloader-provided ACPI RSDP address.
 */
uint64_t
prekern_bsp_acpi_rsdp(
	void)
{
	/* Returns a UEFI RSDP only for a preserved extended handoff. */
	if (boot_info_v2.magic == ZBL6_HANDOFF_MAGIC)
		return boot_info_v2.rsdp;

	/* Requests firmware scanning for a legacy BIOS handoff. */
	return 0;
}

/*
 * Returns the architecture-independent kernel handoff.
 */
const void *
prekern_bsp_kernel_handoff(
	const void *raw_boot_info)
{
	UNUSED_PARAMETER(raw_boot_info);

	/* Returns the persistent normalized handoff. */
	return &kernel_handoff;
}

/* Compares a requested architecture handoff name. */
static int
handoff_name_is(
	const char *name,
	const char *expected)
{
	/* Rejects an absent requested name. */
	if (name == NULL)
		return 0;

	/* Advances through the common terminated prefix. */
	while (*name != '\0' && *name == *expected) {
		name++;
		expected++;
	}

	/* Requires both strings to terminate at the same position. */
	if (*name == *expected)
		return 1;

	/* Rejects names which differ before their terminators. */
	return 0;
}

/* Encodes one lowercase hexadecimal digit. */
static char
hex_digit(
	unsigned value)
{
	/* Selects the decimal half of the alphabet. */
	if (value < 10U)
		return (char)('0' + value);

	/* Returns the lowercase alphabetic digit. */
	return (char)('a' + value - 10U);
}

/* Builds the boot selector from one FAT volume serial. */
static void
set_boot_uuid(
	uint32_t serial)
{
	unsigned index;
	unsigned shift;
	unsigned destination;
	char digit;

	/* Writes the stable selector prefix. */
	boot_selector[0] = 'U';
	boot_selector[1] = 'U';
	boot_selector[2] = 'I';
	boot_selector[3] = 'D';
	boot_selector[4] = '=';

	/* Encodes eight serial nibbles around the fixed separator. */
	for (index = 0, shift = 28;
	     index < 8U;
	     index++, shift -= 4U) {
		digit = hex_digit((serial >> shift) & 15U);

		/* Skips the fixed separator while selecting the destination. */
		if (index >= 4U)
			destination = 5U + index + 1U;
		else
			destination = 5U + index;
		boot_selector[destination] = digit;
	}

	/* Inserts the separator and terminates the selector. */
	boot_selector[9] = '-';
	boot_selector[14] = '\0';
}

/* Validates and preserves one framebuffer handoff. */
static void
accept_framebuffer(
	uint64_t base,
	uint64_t size,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	uint32_t format)
{
	uint64_t offset;
	uint64_t required;

	/* Derives the large-page offset before validating the extent. */
	offset = base & 0x1fffffULL;

	/* Validates geometry, format, multiplication, and address overflow. */
	if (size == 0 ||
	    base > UINT64_MAX - size ||
	    width == 0 ||
	    height == 0 ||
	    stride < width ||
	    (format != ZBL6_FRAMEBUFFER_RGBX8888 &&
	    format != ZBL6_FRAMEBUFFER_BGRX8888) ||
	    stride > UINT64_MAX / 4U / height)
		HAL_FATAL("invalid amd64 framebuffer handoff");

	/* Requires the pixel payload inside the supplied mapping window. */
	required = (uint64_t)stride * height * 4U;
	if (required > size || size > 0x3e000000ULL - offset)
		HAL_FATAL("invalid amd64 framebuffer extent");

	/* Preserves the validated framebuffer contract. */
	boot_framebuffer.physical_base = base;
	boot_framebuffer.size = size;
	boot_framebuffer.width = width;
	boot_framebuffer.height = height;
	boot_framebuffer.stride = stride;
	boot_framebuffer.format = format;
}

/* Copies the complete map and ownership records before reclaim can begin. */
static void
accept_memory(const struct zbl6_memory_handoff *memory, uint32_t source)
{
	uint32_t i;
	uint64_t end;

	if (!zbl6_memory_envelope_valid(memory, source))
		HAL_FATAL("invalid amd64 v6 memory envelope");
	boot_memory = *memory;
	hal_memcpy(boot_memory_range, (const void *)(uintptr_t)memory->ranges,
	    (size_t)memory->range_count * sizeof(boot_memory_range[0]));
	hal_memcpy(boot_allocations, (const void *)(uintptr_t)memory->allocations,
	    (size_t)memory->allocation_count * sizeof(boot_allocations[0]));
	if (!zbl6_memory_contents_valid(&boot_memory, boot_memory_range, boot_allocations))
		HAL_FATAL("invalid amd64 v6 memory ownership");
	boot_memory_range_count = memory->range_count;
	total_memory = 0;
	for (i = 0; i < boot_memory_range_count; i++) {
		end = boot_memory_range[i].base + boot_memory_range[i].size;
		if (boot_memory_range[i].type == ZBL6_MEMORY_USABLE && end > total_memory)
			total_memory = end;
	}
}

/* Reports the memory-map source, with zero denoting legacy geometry. */
uint32_t
bsp_memory_source(void)
{
	return boot_memory.source;
}

/* Returns the firmware-specific attributes without truncating UEFI bits. */
int
bsp_mem_attributes(uint32_t index, uint64_t *attributes)
{
	if (index >= boot_memory_range_count || attributes == NULL)
		return 0;
	*attributes = boot_memory_range[index].attributes;
	return 1;
}

/* Returns boot reservations retained independently of the firmware map. */
int
bsp_boot_allocation(uint32_t index, uint64_t *base, uint64_t *size)
{
	if (index >= boot_memory.allocation_count || base == NULL || size == NULL)
		return 0;
	*base = boot_allocations[index].base;
	*size = boot_allocations[index].size;
	return 1;
}

/*
 * Reports the validated lifetime of a retained boot allocation.
 */
int
bsp_boot_allocation_lifetime(
	uint32_t index,
	uint32_t *lifetime)
{
	if (index >= boot_memory.allocation_count || lifetime == NULL)
		return 0;
	*lifetime = boot_allocations[index].lifetime;
	return 1;
}
