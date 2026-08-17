/* X68000 raw ELF stage 2.  IOCS is forbidden after enter_kernel(). */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "boot/x68k/boot-layout.h"
#include "drivers/x68k-mb89352.h"
#include "kern/boot.h"
#include "kern/elf.h"

#include <stdint.h>

#define PT_LOAD_LIMIT 8U
#define PF_X 1U

extern void x68k_iocs_print(const char *);
extern void x68k_iocs_fatal(const char *) __attribute__((noreturn));
extern void x68k_stage2_enter_kernel(uint32_t, const void *, uint32_t)
	__attribute__((noreturn));

static uint8_t *const bounce = (uint8_t *)X68K_STAGE2_BOUNCE;
/* Partial-sector reads must not use bounce as their scratch buffer: CRC
 * verification intentionally passes bounce as the destination. */
static uint8_t sector_scratch[X68K_SECTOR_SIZE];

#define X68K_SPC_PHYSICAL_BASE 0x00e96020U
#define X68K_SPC_REG_ADDRESS(reg) (X68K_SPC_PHYSICAL_BASE + 1U + \
	(uintptr_t)(reg) * 2U)
#define X68K_SRAM_SCSI_ID 0x00ed0070U
#define X68K_SPC_POLL_LIMIT 10000000U

static unsigned spc_initiator_id;

static uint8_t
stage2_spc_read(void *cookie, unsigned reg)
{
	(void)cookie;
	return *(const volatile uint8_t *)X68K_SPC_REG_ADDRESS(reg);
}

static void
stage2_spc_write(void *cookie, unsigned reg, uint8_t value)
{
	(void)cookie;
	*(volatile uint8_t *)X68K_SPC_REG_ADDRESS(reg) = value;
}

static void
stage2_spc_relax(void *cookie)
{
	(void)cookie;
	__asm__ volatile ("nop");
}

static const struct x68k_spc_bus stage2_spc_bus = {
	.cookie = 0,
	.read = stage2_spc_read,
	.write = stage2_spc_write,
	.relax = stage2_spc_relax,
	.poll_limit = X68K_SPC_POLL_LIMIT,
};

static void
copy_bytes(void *destination, const void *source, uint32_t size)
{
	uint8_t *d = destination;
	const uint8_t *s = source;
	while (size-- != 0)
		*d++ = *s++;
}

static void
zero_bytes(void *destination, uint32_t size)
{
	uint8_t *d = destination;
	while (size-- != 0)
		*d++ = 0;
}

static uint16_t
be16(const void *field)
{
	const uint8_t *p = field;
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t
be32(const void *field)
{
	const uint8_t *p = field;
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static int
read_sectors(uint32_t lba, uint32_t blocks, uint32_t scsi_id, void *buffer)
{
	struct x68k_spc_result result;
	if (blocks == 0 || blocks > 127U || lba > 0x7fffffffU)
		return -1;
	return x68k_spc_pio_read10(&stage2_spc_bus, spc_initiator_id,
	    scsi_id, 0U, lba, blocks, buffer, &result);
}

static int
read_bytes(uint32_t base_lba, uint32_t offset, uint32_t size,
	uint32_t scsi_id, void *destination)
{
	uint8_t *out = destination;
	uint32_t lba;
	if (base_lba > 0x7fffffffU || offset > 0xffffffffU - 511U)
		return -1;
	lba = base_lba + offset / X68K_SECTOR_SIZE;
	offset %= X68K_SECTOR_SIZE;
	while (size != 0) {
		uint32_t amount;
		if (offset != 0 || size < X68K_SECTOR_SIZE) {
			if (read_sectors(lba, 1, scsi_id, sector_scratch) < 0)
				return -1;
			amount = X68K_SECTOR_SIZE - offset;
			if (amount > size)
				amount = size;
			copy_bytes(out, sector_scratch + offset, amount);
			offset = 0;
			lba++;
		} else {
			uint32_t blocks = size / X68K_SECTOR_SIZE;
			if (blocks > 127U)
				blocks = 127U;
			amount = blocks * X68K_SECTOR_SIZE;
			if (read_sectors(lba, blocks, scsi_id, out) < 0)
				return -1;
			lba += blocks;
		}
		out += amount;
		size -= amount;
	}
	return 0;
}

static uint32_t
crc32_update(uint32_t crc, const uint8_t *data, uint32_t size)
{
	while (size-- != 0) {
		unsigned bit;
		crc ^= *data++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U &
			       (uint32_t)-(int32_t)(crc & 1U));
	}
	return crc;
}

static int
check_kernel_crc(const struct x68k_boot_manifest *manifest, uint32_t scsi_id)
{
	uint32_t offset = 0;
	uint32_t remaining = be32(&manifest->kernel_bytes);
	uint32_t expected = be32(&manifest->kernel_crc32);
	uint32_t crc = 0xffffffffU;
	while (remaining != 0) {
		uint32_t amount = remaining > X68K_STAGE2_BOUNCE_SIZE ?
			X68K_STAGE2_BOUNCE_SIZE : remaining;
		if (read_bytes(be32(&manifest->kernel_lba), offset, amount,
		    scsi_id, bounce) != 0)
			return -1;
		crc = crc32_update(crc, bounce, amount);
		offset += amount;
		remaining -= amount;
	}
	return (crc ^ 0xffffffffU) == expected ? 0 : -1;
}

static int
allowed_segment(uint32_t physical, uint32_t memory_size, uint32_t ram_bytes)
{
	uint32_t end;
	if (memory_size == 0 || physical > 0xffffffffU - memory_size)
		return 0;
	end = physical + memory_size;
	if (physical >= X68K_KERNEL_LOW_MIN && end <= X68K_KERNEL_LOW_END)
		return 1;
	return physical >= X68K_KERNEL_HIGH_MIN && end <= ram_bytes;
}

static uint32_t
load_kernel(const struct x68k_boot_manifest *manifest, uint32_t scsi_id,
	uint32_t *physical_end)
{
	struct elf32_ehdr *header = (struct elf32_ehdr *)bounce;
	struct elf32_phdr programs[PT_LOAD_LIMIT];
	uint32_t kernel_lba = be32(&manifest->kernel_lba);
	uint32_t kernel_bytes = be32(&manifest->kernel_bytes);
	uint32_t ram_bytes = be32(&manifest->ram_bytes);
	uint32_t phoff, entry;
	uint16_t phnum, phentsize;
	unsigned i, loads = 0;
	int entry_ok = 0;

	if (kernel_bytes < sizeof(*header) ||
	    read_bytes(kernel_lba, 0, X68K_SECTOR_SIZE, scsi_id, bounce) != 0)
		return 0;
	if (header->e_ident[EI_MAG0] != ELFMAG0 ||
	    header->e_ident[EI_MAG1] != ELFMAG1 ||
	    header->e_ident[EI_MAG2] != ELFMAG2 ||
	    header->e_ident[EI_MAG3] != ELFMAG3 ||
	    header->e_ident[EI_CLASS] != ELFCLASS32 ||
	    header->e_ident[EI_DATA] != ELFDATA2MSB ||
	    be16(&header->e_type) != ET_EXEC || be16(&header->e_machine) != EM_68K)
		return 0;
	phoff = be32(&header->e_phoff);
	phnum = be16(&header->e_phnum);
	phentsize = be16(&header->e_phentsize);
	entry = be32(&header->e_entry);
	if (phnum == 0 || phnum > PT_LOAD_LIMIT ||
	    phentsize != sizeof(struct elf32_phdr) ||
	    phoff > X68K_SECTOR_SIZE ||
	    (uint32_t)phnum * phentsize > X68K_SECTOR_SIZE - phoff)
		return 0;
	copy_bytes(programs, bounce + phoff, (uint32_t)phnum * phentsize);

	*physical_end = 0;
	for (i = 0; i < phnum; i++) {
		struct elf32_phdr *program = &programs[i];
		uint32_t type = be32(&program->p_type);
		uint32_t offset, virtual, physical, filesz, memsz, flags, alignment;
		unsigned j;
		if (type != PT_LOAD)
			continue;
		offset = be32(&program->p_offset);
		virtual = be32(&program->p_vaddr);
		physical = be32(&program->p_paddr);
		filesz = be32(&program->p_filesz);
		memsz = be32(&program->p_memsz);
		flags = be32(&program->p_flags);
		alignment = be32(&program->p_align);
		if (filesz > memsz || offset > kernel_bytes ||
		    filesz > kernel_bytes - offset ||
		    !allowed_segment(physical, memsz, ram_bytes) ||
		    alignment < 4096U || (alignment & (alignment - 1U)) != 0 ||
		    ((offset ^ virtual) & (alignment - 1U)) != 0 ||
		    (virtual < 0x80000000U ? virtual != physical :
		     virtual - physical != 0x80000000U))
			return 0;
		for (j = 0; j < i; j++) {
			struct elf32_phdr *other = &programs[j];
			uint32_t other_start, other_size, other_end;
			if (be32(&other->p_type) != PT_LOAD)
				continue;
			other_start = be32(&other->p_paddr);
			other_size = be32(&other->p_memsz);
			if (other_start > 0xffffffffU - other_size)
				return 0;
			other_end = other_start + other_size;
			if (physical < other_end && other_start < physical + memsz)
				return 0;
		}
		if ((flags & PF_X) != 0 && entry >= physical && entry < physical + memsz)
			entry_ok = 1;
		if (read_bytes(kernel_lba, offset, filesz, scsi_id,
		    (void *)(uintptr_t)physical) != 0)
			return 0;
		zero_bytes((void *)(uintptr_t)(physical + filesz), memsz - filesz);
		if (physical + memsz > *physical_end)
			*physical_end = physical + memsz;
		loads++;
	}
	return loads != 0 && entry_ok ? entry : 0;
}

static void
make_handoff(const struct x68k_boot_manifest *manifest, uint32_t scsi_id,
	uint32_t kernel_end)
{
	struct zedbsd_x68k_handoff *handoff =
		(struct zedbsd_x68k_handoff *)X68K_HANDOFF_ADDRESS;
	struct zedbsd_device *device =
		(struct zedbsd_device *)X68K_DEVICE_TABLE_ADDRESS;
	uint32_t ram_bytes = be32(&manifest->ram_bytes);

	zero_bytes(handoff, sizeof(*handoff));
	zero_bytes(device, sizeof(*device));
	handoff->common.magic = ZEDBSD_HANDOFF_MAGIC;
	handoff->common.version = ZEDBSD_HANDOFF_VERSION_X68K;
	handoff->common.size = sizeof(*handoff);
	handoff->common.device_count = 1;
	handoff->common.boot_bios_id = (uint8_t)scsi_id;
	handoff->common.boot_partition_scheme = ZEDBSD_PARTITION_SCHEME_X68K;
	handoff->common.boot_partition_index = 1;
	handoff->common.device_table = X68K_DEVICE_TABLE_ADDRESS;
	handoff->common.boot_partition_lba = be32(&manifest->root_lba);
	handoff->extension_magic = ZEDBSD_X68K_HANDOFF_MAGIC;
	handoff->extension_version = ZEDBSD_X68K_HANDOFF_VERSION;
	handoff->extension_size = sizeof(*handoff) - sizeof(handoff->common);
	handoff->ram_bytes = ram_bytes;
	handoff->kernel_phys_start = X68K_KERNEL_LOW_MIN;
	handoff->kernel_phys_end = kernel_end;
	handoff->loader_phys_start = X68K_STAGE1_ADDRESS;
	handoff->loader_phys_end = X68K_STAGE2_LIMIT;
	handoff->memory_region_count = 2;
	handoff->memory_regions[0].base = 0;
	handoff->memory_regions[0].size = ram_bytes;
	handoff->memory_regions[0].type = ZEDBSD_MEMORY_AVAILABLE;
	handoff->memory_regions[1].base = X68K_STAGE1_ADDRESS;
	handoff->memory_regions[1].size = X68K_STAGE2_LIMIT - X68K_STAGE1_ADDRESS;
	handoff->memory_regions[1].type = ZEDBSD_MEMORY_RESERVED;

	device->device_class = ZEDBSD_DEV_SCSI;
	device->display_index = 0;
	device->bios_id = (uint8_t)scsi_id;
	device->flags = ZEDBSD_DEV_PRESENT | ZEDBSD_DEV_BOOT_ORIGIN;
	device->sector_size = X68K_SECTOR_SIZE;
}

void
x68k_stage2_main(uint32_t scsi_id, const struct x68k_boot_manifest *manifest)
{
	uint32_t entry, kernel_end;
	if (manifest == 0 || scsi_id > 6U ||
	    be32(&manifest->magic) != X68K_MANIFEST_MAGIC ||
	    be16(&manifest->version) != X68K_MANIFEST_VERSION ||
	    be16(&manifest->header_size) != X68K_MANIFEST_SIZE ||
	    be32(&manifest->root_lba) != X68K_ROOT_LBA)
		x68k_iocs_fatal("zedBSD S2 MANIFEST\r\n");
	x68k_iocs_print("Z68:BOOT2\r\n");
	spc_initiator_id = *(const volatile uint8_t *)X68K_SRAM_SCSI_ID & 7U;
	if (spc_initiator_id == scsi_id ||
	    x68k_spc_pio_init(&stage2_spc_bus, spc_initiator_id) != X68K_SPC_OK)
		x68k_iocs_fatal("zedBSD S2 SPC PIO\r\n");
	if (check_kernel_crc(manifest, scsi_id) != 0)
		x68k_iocs_fatal("zedBSD S2 KERNEL CRC\r\n");
	entry = load_kernel(manifest, scsi_id, &kernel_end);
	if (entry == 0)
		x68k_iocs_fatal("zedBSD S2 ELF\r\n");
	make_handoff(manifest, scsi_id, kernel_end);
	x68k_iocs_print("Z68:LOW\r\n");
	/* One-way boundary: no IOCS/ROM call is permitted after this transfer. */
	x68k_stage2_enter_kernel(entry, (const void *)X68K_HANDOFF_ADDRESS,
	    scsi_id);
}
