#ifndef ZEDBSD_HAL_I386_MULTIBOOT_H
#define ZEDBSD_HAL_I386_MULTIBOOT_H

#include <hal/types.h>

#define MULTIBOOT_HEADER_MAGIC 0x1badb002U
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2badb002U
#define MULTIBOOT_HEADER_FLAGS 0x00000003U

#define MBINFO_FLAG_MEMORY       (1U << 0)
#define MBINFO_FLAG_BOOT_DEVICE  (1U << 1)
#define MBINFO_FLAG_CMDLINE      (1U << 2)
#define MBINFO_FLAG_MODULES      (1U << 3)
#define MBINFO_FLAG_ELF          (1U << 5)
#define MBINFO_FLAG_MMAP         (1U << 6)
#define MBINFO_FLAG_LOADER_NAME  (1U << 9)

struct multiboot_info {
	uint32 flags;
	uint32 mem_lower;
	uint32 mem_upper;
	uint32 boot_device;
	uint32 cmdline;
	uint32 mods_count;
	uint32 mods_addr;
	union {
		struct {
			uint32 tabsize, strsize, addr, reserved;
		} aout;
		struct {
			uint32 num, size, addr, shndx;
		} elf;
	} symbols;
	uint32 mmap_length;
	uint32 mmap_addr;
	uint32 drives_length;
	uint32 drives_addr;
	uint32 config_table;
	uint32 boot_loader_name;
	uint32 apm_table;
	uint32 vbe_control_info;
	uint32 vbe_mode_info;
	uint16 vbe_mode;
	uint16 vbe_interface_seg;
	uint16 vbe_interface_off;
	uint16 vbe_interface_len;
};

struct multiboot_mmap_entry {
	uint32 size;
	uint32 base_low;
	uint32 base_high;
	uint32 length_low;
	uint32 length_high;
	uint32 type;
} __attribute__((packed));

#endif
