#ifndef _SYS_ARCH_X86_MULTIBOOT_H_
#define _SYS_ARCH_X86_MULTIBOOT_H_

#include <sys/types.h>

#define MBINFO_FLAG_MEMORY	(1)

/*
 * Multiboot Infomation
 */
struct multiboot_info {
	uint32	flags;
	uint32	mem_lower;
	uint32	mem_upper;
	uint32	boot_device;
	uint32	cmdline;
	uint32	mods_count;
	uint32	mods_addr;

	struct elf_section_header_table {
		uint32 num;
		uint32 size;
		uint32 addr;
		uint32 shndx;
	} shtab;

	uint32	mmap_length;
	uint32	mmap_addr;
};

#endif
