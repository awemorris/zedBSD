/* PC/AT ZBL6 handoff and memory discovery for amd64. */
#include <hal/hal.h>
#include <kern/boot.h>
#include "bootloader/include/amd64-handoff.h"
#include "../bsp.h"

#define VGA_FONT_HANDOFF 0x00007000U
#define VGA_FONT_MAGIC   0x3854465aU
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
static uint32 boot_memory_range_count;
static struct boot_handoff kernel_handoff;
static uint64 total_memory;
static uint8_t boot_font[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
static int boot_font_valid;

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

void
bsp_boot_init(const void *raw_boot_info)
{
	const struct zbl6_handoff *raw = raw_boot_info;
	const struct zbl6_handoff_v2 *raw_v2 = raw_boot_info;
	const struct vga_font_handoff *font =
	    (const struct vga_font_handoff *)(uintptr_t)VGA_FONT_HANDOFF;
	uint64 highest = 0;
	uint32 index;

	if (raw == NULL || raw->magic != ZBL6_HANDOFF_MAGIC)
		HAL_FATAL("invalid amd64 ZBL6 handoff");
	if (raw->version == ZBL6_HANDOFF_VERSION) {
		if (raw->size < sizeof(*raw) || raw->boot_drive < 0x80U ||
		    raw->partition_index < 1U || raw->partition_index > 4U)
			HAL_FATAL("invalid amd64 ZBL6 v1 handoff");
		boot_info = *raw;
		total_memory = 0x100000ULL +
		    (uint64)boot_info.mem_upper_kib * 1024ULL;
		if (total_memory > 0x40000000ULL)
			total_memory = 0x40000000ULL;
		boot_memory_range_count = 1;
		boot_memory_range[0].base = 0;
		boot_memory_range[0].size = total_memory;
		boot_memory_range[0].type = ZBL6_MEMORY_USABLE;
		boot_memory_range[0].flags = 0;
	} else if (raw->version == ZBL6_HANDOFF_V2_VERSION ||
	    raw->version == ZBL6_HANDOFF_V3_VERSION) {
		const struct zbl6_memory_range *source;
		const struct zbl6_handoff_v3 *raw_v3 = raw_boot_info;
		uint64 previous_end = 0;

		if (raw_v2->size < (raw->version == ZBL6_HANDOFF_V3_VERSION ?
		    sizeof(*raw_v3) : sizeof(*raw_v2)) ||
		    (raw_v2->flags & (ZBL6_HANDOFF_FLAG_UEFI |
		    ZBL6_HANDOFF_FLAG_MEMORY_MAP |
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
		    raw_v2->rsdp == 0 || raw_v2->rsdp >= 0x40000000ULL)
			HAL_FATAL("invalid amd64 ZBL6 v2 handoff");
		boot_info_v2 = *raw_v2;
		if (raw->version == ZBL6_HANDOFF_V3_VERSION) {
			uint64 offset = raw_v3->framebuffer_base & 0x1fffffULL;
			if ((raw_v3->flags & ZBL6_HANDOFF_FLAG_FRAMEBUFFER) == 0 ||
			    raw_v3->framebuffer_size == 0 ||
			    raw_v3->framebuffer_base > UINT64_MAX -
			    raw_v3->framebuffer_size ||
			    raw_v3->framebuffer_width == 0 ||
			    raw_v3->framebuffer_height == 0 ||
			    raw_v3->framebuffer_stride <
			    raw_v3->framebuffer_width ||
			    (raw_v3->framebuffer_format !=
			    ZBL6_FRAMEBUFFER_RGBX8888 &&
			    raw_v3->framebuffer_format !=
			    ZBL6_FRAMEBUFFER_BGRX8888) ||
			    raw_v3->framebuffer_size > 0x3e000000ULL - offset)
				HAL_FATAL("invalid amd64 framebuffer handoff");
			boot_framebuffer.physical_base =
			    raw_v3->framebuffer_base;
			boot_framebuffer.size = raw_v3->framebuffer_size;
			boot_framebuffer.width = raw_v3->framebuffer_width;
			boot_framebuffer.height = raw_v3->framebuffer_height;
			boot_framebuffer.stride = raw_v3->framebuffer_stride;
			boot_framebuffer.format = raw_v3->framebuffer_format;
		}
		source = (const void *)(uintptr_t)raw_v2->memory_ranges;
		boot_memory_range_count = raw_v2->memory_range_count;
		for (index = 0; index < boot_memory_range_count; index++) {
			uint64 end;
			boot_memory_range[index] = source[index];
			if (boot_memory_range[index].size == 0 ||
			    (boot_memory_range[index].base & 0xfffU) != 0 ||
			    (boot_memory_range[index].size & 0xfffU) != 0 ||
			    boot_memory_range[index].base > UINT64_MAX -
			    boot_memory_range[index].size ||
			    boot_memory_range[index].type < ZBL6_MEMORY_USABLE ||
			    boot_memory_range[index].type > ZBL6_MEMORY_MMIO)
				HAL_FATAL("invalid amd64 memory range");
			end = boot_memory_range[index].base +
			    boot_memory_range[index].size;
			if (index != 0 && boot_memory_range[index].base <
			    previous_end)
				HAL_FATAL("overlapping amd64 memory ranges");
			previous_end = end;
			if (end > highest &&
			    boot_memory_range[index].type == ZBL6_MEMORY_USABLE)
				highest = end;
		}
		total_memory = highest > 0x40000000ULL ?
		    0x40000000ULL : highest;
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
		kernel_handoff.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_MBR;
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
	if (boot_font_valid && handoff_name_is(name, "pcat.boot-font"))
		return boot_font;
	if (boot_framebuffer.size != 0 &&
	    handoff_name_is(name, "pcat.framebuffer"))
		return &boot_framebuffer;
	return NULL;
}

uint64 bsp_mem_probe(void) { return total_memory; }

uint32 bsp_mem_range_count(void) { return boot_memory_range_count; }

int
bsp_mem_range(uint32 index, uint64 *base, uint64 *size, uint32 *type)
{
	if (index >= boot_memory_range_count || base == NULL || size == NULL ||
	    type == NULL)
		return 0;
	*base = boot_memory_range[index].base;
	*size = boot_memory_range[index].size;
	*type = boot_memory_range[index].type;
	return 1;
}

uint64
bsp_acpi_rsdp(void)
{
	return boot_info_v2.magic == ZBL6_HANDOFF_MAGIC ?
	    boot_info_v2.rsdp : 0;
}

const void *
bsp_kernel_handoff(const void *raw_boot_info)
{
	(void)raw_boot_info;
	return &kernel_handoff;
}
