/* PC/AT ZBL6 handoff and memory discovery for amd64. */
#include <hal/hal.h>
#include <kern/boot.h>
#include "bootloader/include/amd64-handoff.h"
#include "../bsp.h"

#define VGA_FONT_HANDOFF 0x00007000U
#define VGA_FONT_MAGIC 0x3854465aU
#define MAX_BOOT_MEMORY_RANGES 256U
#define PCAT_BOOT_FONT_GLYPHS 128U
#define PCAT_BOOT_FONT_HEIGHT 16U

struct vga_font_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t height;
	uint16_t glyphs;
	uint16_t reserved[3];
	uint8_t data[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
} __attribute__((packed));

static struct zbl6_handoff boot_info;
static struct zbl6_handoff_v2 boot_info_v2;
static struct zbl6_framebuffer boot_framebuffer;
static struct zbl6_memory_range boot_memory_range[MAX_BOOT_MEMORY_RANGES];
static uint32_t boot_memory_range_count;
static struct boot_handoff kernel_handoff;
static uint64_t total_memory;
static uint8_t boot_font[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
static int boot_font_valid;
static char boot_selector[15];

static int
handoff_name_is(const char *name, const char *expected)
{
	if (name == NULL)
		return 0;
	while (*name != '\0' && *name == *expected) {
		name++;
		expected++;
	}
	return *name == *expected;
}

static char
hex_digit(unsigned value)
{
	return (char)(value < 10U ? '0' + value : 'a' + value - 10U);
}

static void
set_boot_uuid(uint32_t serial)
{
	unsigned index, shift;

	boot_selector[0] = 'U';
	boot_selector[1] = 'U';
	boot_selector[2] = 'I';
	boot_selector[3] = 'D';
	boot_selector[4] = '=';
	for (index = 0, shift = 28; index < 8U; index++, shift -= 4U)
		boot_selector[5U + index + (index >= 4U ? 1U : 0U)] =
		    hex_digit((serial >> shift) & 15U);
	boot_selector[9] = '-';
	boot_selector[14] = '\0';
}

static void
accept_framebuffer(uint64_t base, uint64_t size, uint32_t width,
		   uint32_t height, uint32_t stride, uint32_t format)
{
	uint64_t offset = base & 0x1fffffULL;
	uint64_t required;

	if (size == 0 || base > UINT64_MAX - size || width == 0 ||
	    height == 0 || stride < width ||
	    (format != ZBL6_FRAMEBUFFER_RGBX8888 &&
	     format != ZBL6_FRAMEBUFFER_BGRX8888) ||
	    stride > UINT64_MAX / 4U / height)
		HAL_FATAL("invalid amd64 framebuffer handoff");
	required = (uint64_t)stride * height * 4U;
	if (required > size || size > 0x3e000000ULL - offset)
		HAL_FATAL("invalid amd64 framebuffer extent");
	boot_framebuffer.physical_base = base;
	boot_framebuffer.size = size;
	boot_framebuffer.width = width;
	boot_framebuffer.height = height;
	boot_framebuffer.stride = stride;
	boot_framebuffer.format = format;
}

void
bsp_boot_init(const void *raw_boot_info)
{
	const struct zbl6_handoff *raw = raw_boot_info;
	const struct zbl6_handoff_v2 *raw_v2 = raw_boot_info;
	const struct zbl6_handoff_v4 *raw_v4 = raw_boot_info;
	const struct vga_font_handoff *font =
	    (const struct vga_font_handoff *)(uintptr_t)VGA_FONT_HANDOFF;
	uint64_t highest = 0;
	uint32_t index;

	if (raw == NULL || raw->magic != ZBL6_HANDOFF_MAGIC)
		HAL_FATAL("invalid amd64 ZBL6 handoff");
	hal_memset(&boot_framebuffer, 0, sizeof(boot_framebuffer));
	if (raw->version == ZBL6_HANDOFF_VERSION) {
		const struct zbl6_handoff_framebuffer *raw_framebuffer =
		    raw_boot_info;

		if (raw->size < sizeof(*raw) || raw->boot_drive < 0x80U ||
		    raw->partition_index < 1U || raw->partition_index > 4U)
			HAL_FATAL("invalid amd64 ZBL6 v1 handoff");
		boot_info = *raw;
		if ((raw->flags & ZBL6_HANDOFF_FLAG_BOOT_UUID) != 0)
			set_boot_uuid(raw->reserved);
		if ((raw->flags & ZBL6_HANDOFF_FLAG_FRAMEBUFFER) != 0) {
			if (raw->size < sizeof(*raw_framebuffer))
				HAL_FATAL(
				    "truncated amd64 BIOS framebuffer handoff");
			accept_framebuffer(raw_framebuffer->framebuffer_base,
					   raw_framebuffer->framebuffer_size,
					   raw_framebuffer->framebuffer_width,
					   raw_framebuffer->framebuffer_height,
					   raw_framebuffer->framebuffer_stride,
					   raw_framebuffer->framebuffer_format);
		}
		total_memory =
		    0x100000ULL + (uint64_t)boot_info.mem_upper_kib * 1024ULL;
		if (total_memory > 0x40000000ULL)
			total_memory = 0x40000000ULL;
		boot_memory_range_count = 1;
		boot_memory_range[0].base = 0;
		boot_memory_range[0].size = total_memory;
		boot_memory_range[0].type = ZBL6_MEMORY_USABLE;
		boot_memory_range[0].flags = 0;
	} else if (raw->version == ZBL6_HANDOFF_V2_VERSION ||
		   raw->version == ZBL6_HANDOFF_V3_VERSION ||
		   raw->version == ZBL6_HANDOFF_V4_VERSION) {
		const struct zbl6_memory_range *source;
		const struct zbl6_handoff_v3 *raw_v3 = raw_boot_info;
		uint64_t previous_end = 0;

		size_t required =
		    raw->version == ZBL6_HANDOFF_V4_VERSION   ? sizeof(*raw_v4)
		    : raw->version == ZBL6_HANDOFF_V3_VERSION ? sizeof(*raw_v3)
							      : sizeof(*raw_v2);

		if (raw_v2->size < required ||
		    (raw_v2->flags &
		     (ZBL6_HANDOFF_FLAG_UEFI | ZBL6_HANDOFF_FLAG_MEMORY_MAP |
		      ZBL6_HANDOFF_FLAG_ACPI_RSDP)) !=
			(ZBL6_HANDOFF_FLAG_UEFI | ZBL6_HANDOFF_FLAG_MEMORY_MAP |
			 ZBL6_HANDOFF_FLAG_ACPI_RSDP) ||
		    raw_v2->boot_drive < 0x80U ||
		    raw_v2->root_partition_scheme !=
			ZEDBSD_PARTITION_SCHEME_MBR ||
		    raw_v2->root_partition_index < 1U ||
		    raw_v2->root_partition_index > 4U ||
		    raw_v2->memory_range_count == 0 ||
		    raw_v2->memory_range_count > MAX_BOOT_MEMORY_RANGES ||
		    raw_v2->memory_range_entry_size !=
			sizeof(struct zbl6_memory_range) ||
		    (raw_v2->memory_ranges & 7U) != 0 ||
		    raw_v2->memory_ranges >= 0x40000000ULL ||
		    raw_v2->memory_range_count >
			(0x40000000ULL - raw_v2->memory_ranges) /
			    sizeof(struct zbl6_memory_range) ||
		    raw_v2->loader_partition_index != 2U ||
		    raw_v2->kernel_phys_start != 0x00200000ULL ||
		    raw_v2->kernel_phys_end <= raw_v2->kernel_phys_start ||
		    raw_v2->kernel_phys_end > 0x01200000ULL ||
		    (raw_v2->bootstrap_cr3 & 0xfffU) != 0 ||
		    raw_v2->bootstrap_cr3 >= 0x40000000ULL ||
		    raw_v2->rsdp == 0)
			HAL_FATAL("invalid amd64 ZBL6 v2 handoff");
		boot_info_v2 = *raw_v2;
		if (raw->version == ZBL6_HANDOFF_V3_VERSION ||
		    raw->version == ZBL6_HANDOFF_V4_VERSION) {
			if ((raw_v3->flags & ZBL6_HANDOFF_FLAG_FRAMEBUFFER) ==
			    0)
				HAL_FATAL("invalid amd64 framebuffer handoff");
			accept_framebuffer(raw_v3->framebuffer_base,
					   raw_v3->framebuffer_size,
					   raw_v3->framebuffer_width,
					   raw_v3->framebuffer_height,
					   raw_v3->framebuffer_stride,
					   raw_v3->framebuffer_format);
		}
		if (raw->version == ZBL6_HANDOFF_V4_VERSION) {
			if ((raw_v4->common.flags &
			     ZBL6_HANDOFF_FLAG_BOOT_UUID) == 0)
				HAL_FATAL("missing amd64 boot UUID handoff");
			set_boot_uuid(raw_v4->boot_volume_serial);
		}
		source = (const void *)(uintptr_t)raw_v2->memory_ranges;
		boot_memory_range_count = raw_v2->memory_range_count;
		for (index = 0; index < boot_memory_range_count; index++) {
			uint64_t end;
			boot_memory_range[index] = source[index];
			if (boot_memory_range[index].size == 0 ||
			    (boot_memory_range[index].base & 0xfffU) != 0 ||
			    (boot_memory_range[index].size & 0xfffU) != 0 ||
			    boot_memory_range[index].base >
				UINT64_MAX - boot_memory_range[index].size ||
			    boot_memory_range[index].type <
				ZBL6_MEMORY_USABLE ||
			    boot_memory_range[index].type > ZBL6_MEMORY_MMIO)
				HAL_FATAL("invalid amd64 memory range");
			end = boot_memory_range[index].base +
			      boot_memory_range[index].size;
			if (index != 0 &&
			    boot_memory_range[index].base < previous_end)
				HAL_FATAL("overlapping amd64 memory ranges");
			previous_end = end;
			if (end > highest &&
			    boot_memory_range[index].type == ZBL6_MEMORY_USABLE)
				highest = end;
		}
		total_memory =
		    highest > 0x40000000ULL ? 0x40000000ULL : highest;
	} else {
		HAL_FATAL("unsupported amd64 ZBL6 handoff");
	}
	if (total_memory < 0x00400000ULL)
		HAL_FATAL("too little amd64 memory");
	boot_font_valid = font->magic == VGA_FONT_MAGIC && font->version == 1 &&
			  font->height == PCAT_BOOT_FONT_HEIGHT &&
			  font->glyphs == PCAT_BOOT_FONT_GLYPHS;
	if (boot_font_valid)
		hal_memcpy(boot_font, font->data, sizeof(boot_font));

	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = ZEDBSD_HANDOFF_MAGIC;
	kernel_handoff.version = ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	if (raw->version == ZBL6_HANDOFF_VERSION) {
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

void *
hal_get_arch_handoff(const char *name)
{
	if (boot_selector[0] != '\0' && handoff_name_is(name, "boot.selector"))
		return boot_selector;
	if (boot_font_valid && handoff_name_is(name, "pcat.boot-font"))
		return boot_font;
	if (boot_framebuffer.size != 0 &&
	    handoff_name_is(name, "pcat.framebuffer"))
		return &boot_framebuffer;
	return NULL;
}

uint64_t
bsp_mem_probe(void)
{
	return total_memory;
}

uint32_t
bsp_mem_range_count(void)
{
	return boot_memory_range_count;
}

int
bsp_mem_range(uint32_t index, uint64_t *base, uint64_t *size, uint32_t *type)
{
	if (index >= boot_memory_range_count || base == NULL || size == NULL ||
	    type == NULL)
		return 0;
	*base = boot_memory_range[index].base;
	*size = boot_memory_range[index].size;
	*type = boot_memory_range[index].type;
	return 1;
}

int
bsp_physical_range_mappable(uint64_t physical, size_t size)
{
	uint64_t end;
	uint32_t index;
	int legacy = boot_info_v2.magic != ZBL6_HANDOFF_MAGIC;

	if (size == 0 || physical > UINT64_MAX - size)
		return 0;
	end = physical + size;
	/* The v1 BIOS handoff has only a usable-memory total.  SeaBIOS places
	 * checksum-protected ACPI tables in the reserved gap just above that
	 * total, so retain the old sub-1-GiB readable extent for this path. */
	if (legacy)
		return end <= 0x40000000ULL;
	for (index = 0; index < boot_memory_range_count; index++) {
		uint64_t range_end = boot_memory_range[index].base +
				     boot_memory_range[index].size;
		uint32_t type = boot_memory_range[index].type;

		if (boot_memory_range[index].base <= physical && end <= range_end &&
		    (type == ZBL6_MEMORY_RESERVED ||
		     type == ZBL6_MEMORY_ACPI_RECLAIM ||
		     type == ZBL6_MEMORY_ACPI_NVS))
			return 1;
	}
	return 0;
}

uint64_t
bsp_acpi_rsdp(void)
{
	return boot_info_v2.magic == ZBL6_HANDOFF_MAGIC ? boot_info_v2.rsdp : 0;
}

const void *
bsp_kernel_handoff(const void *raw_boot_info)
{
	(void)raw_boot_info;
	return &kernel_handoff;
}
