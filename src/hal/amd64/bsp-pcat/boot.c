/* PC/AT ZBL6 handoff and memory discovery for amd64. */
#include <hal/hal.h>
#include <hal/pcat/boot-font.h>
#include <kern/boot.h>
#include "bootloader/include/amd64-handoff.h"
#include "../bsp.h"

#define VGA_FONT_HANDOFF 0x00007000U
#define VGA_FONT_MAGIC   0x3854465aU

struct vga_font_handoff {
	uint32_t magic;
	uint16_t version;
	uint16_t height;
	uint16_t glyphs;
	uint16_t reserved[3];
	uint8_t data[BSP_PCAT_ASCII_GLYPHS][BSP_PCAT_GLYPH_HEIGHT];
} __attribute__((packed));

static struct zbl6_handoff boot_info;
static struct zedbsd_handoff kernel_handoff;
static uint64 total_memory;
static uint8_t boot_font[BSP_PCAT_ASCII_GLYPHS][BSP_PCAT_GLYPH_HEIGHT];
static int boot_font_valid;

void
bsp_boot_init(const void *raw_boot_info)
{
	const struct zbl6_handoff *raw = raw_boot_info;
	const struct vga_font_handoff *font =
	    (const struct vga_font_handoff *)(uintptr_t)VGA_FONT_HANDOFF;

	if (raw == NULL || raw->magic != ZBL6_HANDOFF_MAGIC ||
	    raw->version != ZBL6_HANDOFF_VERSION ||
	    raw->size < sizeof(*raw) || raw->boot_drive < 0x80U ||
	    raw->partition_index < 1U || raw->partition_index > 4U)
		HAL_FATAL("invalid amd64 ZBL6 handoff");
	boot_info = *raw;
	total_memory = 0x100000ULL + (uint64)boot_info.mem_upper_kib * 1024ULL;
	if (total_memory > 0x40000000ULL)
		total_memory = 0x40000000ULL;
	if (total_memory < 0x00400000ULL)
		HAL_FATAL("too little amd64 memory");
	boot_font_valid = font->magic == VGA_FONT_MAGIC && font->version == 1 &&
	    font->height == BSP_PCAT_GLYPH_HEIGHT &&
	    font->glyphs == BSP_PCAT_ASCII_GLYPHS;
	if (boot_font_valid)
		hal_memcpy(boot_font, font->data, sizeof(boot_font));

	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = ZEDBSD_HANDOFF_MAGIC;
	kernel_handoff.version = ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	kernel_handoff.boot_bios_id = boot_info.boot_drive;
	kernel_handoff.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_MBR;
	kernel_handoff.boot_partition_index = boot_info.partition_index;
}

int
bsp_pcat_get_boot_font(
	uint8_t font[BSP_PCAT_ASCII_GLYPHS][BSP_PCAT_GLYPH_HEIGHT])
{
	if (!boot_font_valid || font == NULL)
		return 0;
	hal_memcpy(font, boot_font, sizeof(boot_font));
	return 1;
}

uint64 bsp_mem_probe(void) { return total_memory; }

const void *
bsp_kernel_handoff(const void *raw_boot_info)
{
	(void)raw_boot_info;
	return &kernel_handoff;
}
