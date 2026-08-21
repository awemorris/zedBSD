/* PC/AT Multiboot handoff and memory discovery.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include <kern/boot.h>
#include "../bsp.h"
#include "../multiboot.h"
#include "../i386.h"

#define VGA_FONT_HANDOFF 0x00007000U
#define VGA_FONT_MAGIC 0x3854465aU
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

static const struct multiboot_info *mbi;
static struct boot_handoff handoff;
static struct boot_device boot_device;
static uint32_t total_memory;
static uint8_t root_bios_id;
static uint8_t root_partition;
static int boot_info_valid;
static uint8_t boot_font[PCAT_BOOT_FONT_GLYPHS][PCAT_BOOT_FONT_HEIGHT];
static int boot_font_valid;
static const char *boot_command_line;

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

static int
hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int
parse_number(const char **text, uint32_t *value)
{
	const char *p = *text;
	uint32_t result = 0;
	int base = 10, digit, count = 0;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	}
	while ((digit = hex_digit(*p)) >= 0 && digit < base) {
		if (result > (0xffffffffU - (uint32_t)digit) / (uint32_t)base)
			return 0;
		result = result * (uint32_t)base + (uint32_t)digit;
		p++;
		count++;
	}
	if (count == 0) return 0;
	*text = p;
	*value = result;
	return 1;
}

static void
parse_command_line(const char *line)
{
	static const char option[] = "zedbsd.root=";

	if (line == 0) return;
	while (*line != '\0') {
		const char *p = line;
		unsigned i;
		uint32_t drive, partition;

		while (*p == ' ') p++;
		for (i = 0; option[i] != '\0' && p[i] == option[i]; i++) ;
		if (option[i] == '\0') {
			p += i;
			if (parse_number(&p, &drive) && *p++ == ',' &&
			    parse_number(&p, &partition) &&
			    (*p == '\0' || *p == ' ') && drive >= 0x80U &&
			    drive <= 0x83U && partition >= 1U && partition <= 4U) {
				root_bios_id = (uint8_t)drive;
				root_partition = (uint8_t)partition;
			}
			return;
		}
		while (*p != '\0' && *p != ' ') p++;
		line = p;
	}
}

void
bsp_boot_init(const void *raw_boot_info)
{
	const struct vga_font_handoff *font =
		(const struct vga_font_handoff *)(uintptr_t)VGA_FONT_HANDOFF;
	uint32_t upper;

	boot_font_valid = font->magic == VGA_FONT_MAGIC && font->version == 1 &&
		font->height == PCAT_BOOT_FONT_HEIGHT &&
		font->glyphs == PCAT_BOOT_FONT_GLYPHS;
	if (boot_font_valid)
		hal_memcpy(boot_font, font->data, sizeof(boot_font));
	mbi = raw_boot_info;
	boot_info_valid = mbi != 0 && (mbi->flags & MBINFO_FLAG_MEMORY) != 0;
	if (!boot_info_valid) return;
	upper = mbi->mem_upper;
	if (upper > (127U * 1024U)) upper = 127U * 1024U;
	total_memory = 0x100000U + upper * 1024U;
	root_bios_id = 0x80U;
	root_partition = 1U;
	if (mbi->flags & MBINFO_FLAG_BOOT_DEVICE) {
		uint8_t drive = (uint8_t)(mbi->boot_device >> 24);
		uint8_t partition = (uint8_t)(mbi->boot_device >> 16);
		if (drive >= 0x80U && drive <= 0x83U)
			root_bios_id = drive;
		if (partition < 4U)
			root_partition = (uint8_t)(partition + 1U);
	}
	if ((mbi->flags & MBINFO_FLAG_CMDLINE) && mbi->cmdline != 0)
		boot_command_line = (const char *)(uintptr_t)mbi->cmdline;
	if (boot_command_line != NULL)
		parse_command_line(boot_command_line);
}

void *
hal_get_arch_handoff(const char *name)
{
	if (handoff_name_is(name, "boot.command-line"))
		return (void *)boot_command_line;
	if (!boot_font_valid || !handoff_name_is(name, "pcat.boot-font"))
		return NULL;
	return boot_font;
}

uint32_t
bsp_mem_probe(void)
{
	return total_memory;
}

const void *
bsp_kernel_handoff(const void *raw_boot_info)
{
	(void)raw_boot_info;
	if (!boot_info_valid || total_memory < 0x00400000U)
		HAL_FATAL("invalid PC/AT Multiboot information");
	boot_device.device_class = ZEDBSD_DEV_IDE;
	boot_device.display_index = (uint8_t)(root_bios_id - 0x80U);
	boot_device.bios_id = root_bios_id;
	boot_device.flags = ZEDBSD_DEV_PRESENT | ZEDBSD_DEV_BOOT_ORIGIN;
	boot_device.sector_size = 512U;
	boot_device.cylinders = 0;
	boot_device.heads = 0;
	boot_device.sectors = 0;
	boot_device.controller_location = (uint8_t)(root_bios_id - 0x80U);
	for (unsigned i = 0; i < sizeof(boot_device.reserved); i++)
		boot_device.reserved[i] = 0;
	handoff.magic = ZEDBSD_HANDOFF_MAGIC;
	handoff.version = ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	handoff.size = sizeof(handoff);
	handoff.device_count = 1;
	handoff.boot_bios_id = root_bios_id;
	handoff.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_MBR;
	handoff.boot_partition_index = root_partition;
	handoff.device_table = (uint32_t)(uintptr_t)&boot_device;
	handoff.bios_gateway = 0;
	handoff.boot_partition_lba = ZEDBSD_BOOT_PARTITION_LBA_UNKNOWN;
	hal_printf("boot: Multiboot root BIOS=%02X MBR partition=%u\n",
	    root_bios_id, root_partition);
	if ((mbi->flags & MBINFO_FLAG_LOADER_NAME) && mbi->boot_loader_name != 0)
		hal_printf("boot: Multiboot loader %s\n",
		    (const char *)(uintptr_t)mbi->boot_loader_name);
	return &handoff;
}
