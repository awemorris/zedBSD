/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 raw ELF stage 2.
 *
 * It reads the kernel from the boot SCSI disk by programmed I/O, checks and
 * loads it, builds the handoff and enters the kernel.  IOCS is forbidden
 * after the transfer to the kernel.
 */

#include "bootloader/x68k/boot-layout.h"
#include "bootloader/x68k/handoff.h"
#include "drivers/platform/x68k/x68k-mb89352.h"
#include "kern/boot.h"
#include "kern/elf.h"

#include <stdint.h>

/* The most program headers a kernel may have. */
#define PT_LOAD_LIMIT			8U

/* The program header flag of an executable segment. */
#define PF_X				1U

/* The physical address of the MB89352 SCSI protocol controller's registers. */
#define X68K_SPC_PHYSICAL_BASE		0x00e96020U

/* The address of one SPC register; the registers sit on odd bytes. */
#define X68K_SPC_REG_ADDRESS(reg)	(X68K_SPC_PHYSICAL_BASE + 1U + (uintptr_t)(reg) * 2U)

/* The SRAM byte that holds the machine's own SCSI ID. */
#define X68K_SRAM_SCSI_ID		0x00ed0070U

/* How many times a wait on the SPC polls before it gives up. */
#define X68K_SPC_POLL_LIMIT		10000000U

/* Marks a parameter that this function does not use. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The assembly helpers.
 *
 * The IOCS calls are valid only until the kernel is entered, and the
 * transfer to the kernel does not return.
 */
extern void x68k_iocs_print(const char *);
extern void x68k_iocs_fatal(const char *) __attribute__((noreturn));
extern void x68k_stage2_enter_kernel(uint32_t, const void *, uint32_t) __attribute__((noreturn));

static uint8_t stage2_spc_read(void *cookie, unsigned reg);
static void stage2_spc_write(void *cookie, unsigned reg, uint8_t value);
static void stage2_spc_relax(void *cookie);

/*
 * The fixed bounce buffer the kernel's CRC is computed through.
 *
 * It lies in RAM the loader reserves above its stack and lives until the
 * kernel is entered.
 */
static uint8_t *const bounce = (uint8_t *)X68K_STAGE2_BOUNCE;

/*
 * The buffer a partial-sector read goes through.
 *
 * Partial-sector reads must not use bounce as their scratch buffer: CRC
 * verification intentionally passes bounce as the destination.
 */
static uint8_t sector_scratch[X68K_SECTOR_SIZE];

/*
 * The SCSI ID the machine itself uses on the bus.
 *
 * It is read from SRAM once before the SPC is set up and is used for every
 * command after that.
 */
static unsigned spc_initiator_id;

/* The SPC driver's access to the controller, by direct register access. */
static const struct x68k_spc_bus stage2_spc_bus = {
	0,
	stage2_spc_read,
	stage2_spc_write,
	stage2_spc_relax,
	X68K_SPC_POLL_LIMIT,
};

static void copy_bytes(void *destination, const void *source, uint32_t size);
static void zero_bytes(void *destination, uint32_t size);
static uint16_t be16(const void *field);
static uint32_t be32(const void *field);
static int manifest_valid(const struct x68k_boot_manifest *manifest, uint32_t scsi_id);
static int read_sectors(uint32_t lba, uint32_t blocks, uint32_t scsi_id, void *buffer);
static int read_bytes(uint32_t base_lba, uint32_t offset, uint32_t size, uint32_t scsi_id, void *destination);
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t size);
static int check_kernel_crc(const struct x68k_boot_manifest *manifest, uint32_t scsi_id);
static int allowed_segment(uint32_t physical, uint32_t memory_size, uint32_t ram_bytes);
static int segment_acceptable(const struct elf32_phdr *program, uint32_t kernel_bytes, uint32_t ram_bytes);
static int segment_overlaps(const struct elf32_phdr *programs, unsigned count, uint32_t physical, uint32_t memory_size);
static uint32_t load_kernel(const struct x68k_boot_manifest *manifest, uint32_t scsi_id, uint32_t *physical_end);
static void make_handoff(const struct x68k_boot_manifest *manifest, uint32_t scsi_id, uint32_t kernel_end);

/*
 * Loads the kernel the manifest names and enters it.
 *
 * Every failure stops at an IOCS message; the function never returns.
 */
void
x68k_stage2_main(
	uint32_t scsi_id,
	const struct x68k_boot_manifest *manifest)
{
	uint32_t entry;
	uint32_t kernel_end;
	int valid;
	int status;

	/* Stops on a manifest stage 1 should not have handed over. */
	valid = manifest_valid(manifest, scsi_id);
	if (!valid)
		x68k_iocs_fatal("zedBSD S2 MANIFEST\r\n");

	/* Reports that stage 2 runs. */
	x68k_iocs_print("Z68:BOOT2\r\n");

	/* Reads the machine's own SCSI ID, which must differ from the disk's. */
	spc_initiator_id = *(const volatile uint8_t *)X68K_SRAM_SCSI_ID & 7U;
	if (spc_initiator_id == scsi_id)
		x68k_iocs_fatal("zedBSD S2 SPC PIO\r\n");

	/* Sets up the SPC for programmed I/O. */
	status = drv_x68k_spc_pio_init(&stage2_spc_bus, spc_initiator_id);
	if (status != X68K_SPC_OK)
		x68k_iocs_fatal("zedBSD S2 SPC PIO\r\n");

	/* Stops on a kernel whose bytes are not the ones the manifest names. */
	status = check_kernel_crc(manifest, scsi_id);
	if (status != 0)
		x68k_iocs_fatal("zedBSD S2 KERNEL CRC\r\n");

	/* Loads the kernel's segments and finds its entry point. */
	entry = load_kernel(manifest, scsi_id, &kernel_end);
	if (entry == 0)
		x68k_iocs_fatal("zedBSD S2 ELF\r\n");

	/* Describes the machine and the boot disk to the kernel. */
	make_handoff(manifest, scsi_id, kernel_end);
	x68k_iocs_print("Z68:LOW\r\n");

	/* One-way boundary: no IOCS/ROM call is permitted after this transfer. */
	x68k_stage2_enter_kernel(entry, (const void *)X68K_HANDOFF_ADDRESS, scsi_id);
}

/* Reads one SPC register. */
static uint8_t
stage2_spc_read(
	void *cookie,
	unsigned reg)
{
	uint8_t value;

	UNUSED_PARAMETER(cookie);

	/* Reads the register through its uncached address. */
	value = *(const volatile uint8_t *)X68K_SPC_REG_ADDRESS(reg);

	/* Reports the register's value. */
	return value;
}

/* Writes one SPC register. */
static void
stage2_spc_write(
	void *cookie,
	unsigned reg,
	uint8_t value)
{
	UNUSED_PARAMETER(cookie);

	/* Writes the register through its uncached address. */
	*(volatile uint8_t *)X68K_SPC_REG_ADDRESS(reg) = value;
}

/* Pauses briefly inside a wait on the SPC. */
static void
stage2_spc_relax(
	void *cookie)
{
	UNUSED_PARAMETER(cookie);

	/* One instruction is enough of a pause between two polls. */
	__asm__ volatile ("nop");
}

/* Copies bytes without the C library. */
static void
copy_bytes(
	void *destination,
	const void *source,
	uint32_t size)
{
	uint8_t *d;
	const uint8_t *s;

	/* Copies the bytes one at a time, front to back. */
	d = destination;
	s = source;
	while (size != 0) {
		*d = *s;
		d++;
		s++;
		size--;
	}
}

/* Clears bytes without the C library. */
static void
zero_bytes(
	void *destination,
	uint32_t size)
{
	uint8_t *d;

	/* Clears the bytes one at a time. */
	d = destination;
	while (size != 0) {
		*d = 0;
		d++;
		size--;
	}
}

/* Reads a big-endian 16-bit field. */
static uint16_t
be16(
	const void *field)
{
	const uint8_t *p;

	/* Combines the high byte and the low byte. */
	p = field;
	return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

/* Reads a big-endian 32-bit field. */
static uint32_t
be32(
	const void *field)
{
	const uint8_t *p;

	/* Combines the four bytes, the most significant first. */
	p = field;
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Reports whether stage 1 handed over a usable manifest and disk. */
static int
manifest_valid(
	const struct x68k_boot_manifest *manifest,
	uint32_t scsi_id)
{
	uint32_t magic;
	uint32_t root_lba;
	uint16_t version;
	uint16_t header_size;

	/* There is no manifest. */
	if (manifest == 0)
		return 0;

	/* SCSI ID 7 is the machine itself. */
	if (scsi_id > 6U)
		return 0;

	/* The manifest carries another identity. */
	magic = be32(&manifest->magic);
	if (magic != X68K_MANIFEST_MAGIC)
		return 0;

	/* The manifest is of another version. */
	version = be16(&manifest->version);
	if (version != X68K_MANIFEST_VERSION)
		return 0;

	/* The manifest is of another size. */
	header_size = be16(&manifest->header_size);
	if (header_size != X68K_MANIFEST_SIZE)
		return 0;

	/* The root file system is not where the disk layout puts it. */
	root_lba = be32(&manifest->root_lba);
	if (root_lba != X68K_ROOT_LBA)
		return 0;

	/* Succeeded: the manifest can be used. */
	return 1;
}

/* Reads whole sectors from the boot disk. */
static int
read_sectors(
	uint32_t lba,
	uint32_t blocks,
	uint32_t scsi_id,
	void *buffer)
{
	struct x68k_spc_result result;
	int status;

	/* Refuses a transfer READ(10) cannot carry in one command. */
	if (blocks == 0 ||
	    blocks > 127U ||
	    lba > 0x7fffffffU)
		return -1;

	/* Reads the sectors by programmed I/O. */
	status = drv_x68k_spc_pio_read10(&stage2_spc_bus,
					 spc_initiator_id,
					 scsi_id,
					 0U,
					 lba,
					 blocks,
					 buffer,
					 &result);
	if (status != X68K_SPC_OK)
		return status;

	/* Succeeded: the sectors are in the buffer. */
	return X68K_SPC_OK;
}

/* Reads a byte range from the boot disk, starting at a sector. */
static int
read_bytes(
	uint32_t base_lba,
	uint32_t offset,
	uint32_t size,
	uint32_t scsi_id,
	void *destination)
{
	uint8_t *out;
	uint32_t lba;
	uint32_t amount;
	uint32_t blocks;
	int status;

	/* Refuses a range whose sector or rounded offset does not fit. */
	if (base_lba > 0x7fffffffU || offset > 0xffffffffU - 511U)
		return -1;

	/* Finds the first sector and the offset within it. */
	out = destination;
	lba = base_lba + offset / X68K_SECTOR_SIZE;
	offset %= X68K_SECTOR_SIZE;

	/* Reads partial sectors through the scratch buffer and whole runs directly. */
	while (size != 0) {
		/* A partial sector goes through the scratch buffer; whole sectors go straight to the destination. */
		if (offset != 0 || size < X68K_SECTOR_SIZE) {
			/* Reads the sector into the scratch buffer. */
			status = read_sectors(lba, 1, scsi_id, sector_scratch);
			if (status < 0)
				return -1;

			/* Copies the part of the sector the range covers. */
			amount = X68K_SECTOR_SIZE - offset;
			if (amount > size)
				amount = size;

			copy_bytes(out, sector_scratch + offset, amount);
			offset = 0;
			lba++;
		} else {
			/* Reads as many whole sectors as one command carries. */
			blocks = size / X68K_SECTOR_SIZE;
			if (blocks > 127U)
				blocks = 127U;

			amount = blocks * X68K_SECTOR_SIZE;
			status = read_sectors(lba, blocks, scsi_id, out);
			if (status < 0)
				return -1;

			lba += blocks;
		}

		/* Moves past what was read. */
		out += amount;
		size -= amount;
	}

	/* Succeeded: the range is in the destination. */
	return 0;
}

/* Adds bytes to a running CRC-32 (the reflected IEEE polynomial). */
static uint32_t
crc32_update(
	uint32_t crc,
	const uint8_t *data,
	uint32_t size)
{
	unsigned bit;

	/* Folds each byte into the CRC one bit at a time. */
	while (size != 0) {
		crc ^= *data;
		data++;
		size--;

		/* Shifts each bit out, applying the polynomial when it was set. */
		for (bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xedb88320U &
			      (uint32_t)-(int32_t)(crc & 1U));
		}
	}

	/* Reports the updated CRC. */
	return crc;
}

/* Reports whether the kernel's bytes on the disk have the CRC the manifest names. */
static int
check_kernel_crc(
	const struct x68k_boot_manifest *manifest,
	uint32_t scsi_id)
{
	uint32_t offset;
	uint32_t remaining;
	uint32_t expected;
	uint32_t crc;
	uint32_t amount;
	uint32_t kernel_lba;
	int status;

	/* Reads the kernel's place, size and CRC from the manifest. */
	offset = 0;
	remaining = be32(&manifest->kernel_bytes);
	expected = be32(&manifest->kernel_crc32);
	crc = 0xffffffffU;

	/* Reads the kernel through the bounce buffer and folds it into the CRC. */
	while (remaining != 0) {
		/* Reads at most one bounce buffer of the kernel. */
		amount = remaining > X68K_STAGE2_BOUNCE_SIZE ? X68K_STAGE2_BOUNCE_SIZE : remaining;
		kernel_lba = be32(&manifest->kernel_lba);
		status = read_bytes(kernel_lba, offset, amount, scsi_id, bounce);
		if (status != 0)
			return -1;

		/* Folds the bytes into the CRC and moves past them. */
		crc = crc32_update(crc, bounce, amount);
		offset += amount;
		remaining -= amount;
	}

	/* Refuses a kernel whose CRC is not the manifest's. */
	if ((crc ^ 0xffffffffU) != expected)
		return -1;

	/* Succeeded: the kernel's bytes are the ones the manifest names. */
	return 0;
}

/* Reports whether a segment lies in RAM the kernel may be loaded into. */
static int
allowed_segment(
	uint32_t physical,
	uint32_t memory_size,
	uint32_t ram_bytes)
{
	uint32_t end;

	/* Refuses an empty segment and one whose end does not fit. */
	if (memory_size == 0 || physical > 0xffffffffU - memory_size)
		return 0;

	/* The low window below the loader takes the segment. */
	end = physical + memory_size;
	if (physical >= X68K_KERNEL_LOW_MIN && end <= X68K_KERNEL_LOW_END)
		return 1;

	/* The high window above 1 MiB takes the segment when it ends within RAM. */
	if (physical >= X68K_KERNEL_HIGH_MIN && end <= ram_bytes)
		return 1;

	/* Refuses a segment outside both windows. */
	return 0;
}

/* Reports whether a PT_LOAD segment's file range, place and alignment can be loaded. */
static int
segment_acceptable(
	const struct elf32_phdr *program,
	uint32_t kernel_bytes,
	uint32_t ram_bytes)
{
	uint32_t offset;
	uint32_t virtual;
	uint32_t physical;
	uint32_t filesz;
	uint32_t memsz;
	uint32_t alignment;
	int allowed;

	/* Reads the segment's fields. */
	offset = be32(&program->p_offset);
	virtual = be32(&program->p_vaddr);
	physical = be32(&program->p_paddr);
	filesz = be32(&program->p_filesz);
	memsz = be32(&program->p_memsz);
	alignment = be32(&program->p_align);

	/* The file part is larger than the segment, or lies outside the kernel file. */
	if (filesz > memsz ||
	    offset > kernel_bytes ||
	    filesz > kernel_bytes - offset)
		return 0;

	/* The segment lies outside the RAM the kernel may use. */
	allowed = allowed_segment(physical, memsz, ram_bytes);
	if (!allowed)
		return 0;

	/* The alignment is below a page or not a power of two. */
	if (alignment < 4096U || (alignment & (alignment - 1U)) != 0)
		return 0;

	/* The file offset and the address disagree within the alignment. */
	if (((offset ^ virtual) & (alignment - 1U)) != 0)
		return 0;

	/* A low address is identity mapped; a high one is 2 GiB above its physical address. */
	if (virtual < 0x80000000U) {
		if (virtual != physical)
			return 0;
	} else if (virtual - physical != 0x80000000U) {
		return 0;
	}

	/* Succeeded: the segment can be loaded. */
	return 1;
}

/* Reports whether a segment overlaps a PT_LOAD segment before it, or one of those wraps. */
static int
segment_overlaps(
	const struct elf32_phdr *programs,
	unsigned count,
	uint32_t physical,
	uint32_t memory_size)
{
	const struct elf32_phdr *other;
	uint32_t other_type;
	uint32_t other_start;
	uint32_t other_size;
	uint32_t other_end;
	unsigned index;

	/* Compares the segment with each segment loaded before it. */
	for (index = 0; index < count; index++) {
		/* Only PT_LOAD segments take memory. */
		other = &programs[index];
		other_type = be32(&other->p_type);
		if (other_type != PT_LOAD)
			continue;

		/* An earlier segment whose end does not fit is refused as an overlap. */
		other_start = be32(&other->p_paddr);
		other_size = be32(&other->p_memsz);
		if (other_start > 0xffffffffU - other_size)
			return 1;

		/* The two physical ranges intersect. */
		other_end = other_start + other_size;
		if (physical < other_end && other_start < physical + memory_size)
			return 1;
	}

	/* Succeeded: the segment overlaps nothing before it. */
	return 0;
}

/*
 * Loads the kernel's PT_LOAD segments and reports its entry point.
 *
 * Returns zero when the kernel is not one this loader can run.  The highest
 * physical address the segments reach is stored at physical_end.
 */
static uint32_t
load_kernel(
	const struct x68k_boot_manifest *manifest,
	uint32_t scsi_id,
	uint32_t *physical_end)
{
	struct elf32_ehdr *header;
	struct elf32_phdr programs[PT_LOAD_LIMIT];
	struct elf32_phdr *program;
	uint32_t kernel_lba;
	uint32_t kernel_bytes;
	uint32_t ram_bytes;
	uint32_t phoff;
	uint32_t entry;
	uint32_t type;
	uint32_t offset;
	uint32_t physical;
	uint32_t filesz;
	uint32_t memsz;
	uint32_t flags;
	uint16_t e_type;
	uint16_t e_machine;
	uint16_t phnum;
	uint16_t phentsize;
	unsigned i;
	unsigned loads;
	int entry_ok;
	int acceptable;
	int overlaps;
	int status;

	/* Reads the kernel's place and size, and the machine's RAM, from the manifest. */
	header = (struct elf32_ehdr *)bounce;
	kernel_lba = be32(&manifest->kernel_lba);
	kernel_bytes = be32(&manifest->kernel_bytes);
	ram_bytes = be32(&manifest->ram_bytes);
	loads = 0;
	entry_ok = 0;

	/* Refuses a kernel too small to hold an ELF header. */
	if (kernel_bytes < sizeof(*header))
		return 0;

	/* Reads the kernel's first sector, which holds its headers. */
	status = read_bytes(kernel_lba, 0, X68K_SECTOR_SIZE, scsi_id, bounce);
	if (status != 0)
		return 0;

	/* Refuses a file that is not a big-endian ELF32 file. */
	if (header->e_ident[EI_MAG0] != ELFMAG0 ||
	    header->e_ident[EI_MAG1] != ELFMAG1 ||
	    header->e_ident[EI_MAG2] != ELFMAG2 ||
	    header->e_ident[EI_MAG3] != ELFMAG3 ||
	    header->e_ident[EI_CLASS] != ELFCLASS32 ||
	    header->e_ident[EI_DATA] != ELFDATA2MSB)
		return 0;

	/* Refuses a file that is not an executable. */
	e_type = be16(&header->e_type);
	if (e_type != ET_EXEC)
		return 0;

	/* Refuses an executable for another machine. */
	e_machine = be16(&header->e_machine);
	if (e_machine != EM_68K)
		return 0;

	/* Reads where the program headers are and where the kernel starts. */
	phoff = be32(&header->e_phoff);
	phnum = be16(&header->e_phnum);
	phentsize = be16(&header->e_phentsize);
	entry = be32(&header->e_entry);

	/* Refuses program headers that are not all within the first sector. */
	if (phnum == 0 ||
	    phnum > PT_LOAD_LIMIT ||
	    phentsize != sizeof(struct elf32_phdr) ||
	    phoff > X68K_SECTOR_SIZE ||
	    (uint32_t)phnum * phentsize > X68K_SECTOR_SIZE - phoff)
		return 0;

	/* Keeps the program headers, since the bounce buffer is reused below. */
	copy_bytes(programs, bounce + phoff, (uint32_t)phnum * phentsize);

	/* Loads each PT_LOAD segment and tracks how far they reach. */
	*physical_end = 0;
	for (i = 0; i < phnum; i++) {
		/* Only PT_LOAD segments are loaded. */
		program = &programs[i];
		type = be32(&program->p_type);
		if (type != PT_LOAD)
			continue;

		/* Reads the fields the load uses. */
		offset = be32(&program->p_offset);
		physical = be32(&program->p_paddr);
		filesz = be32(&program->p_filesz);
		memsz = be32(&program->p_memsz);
		flags = be32(&program->p_flags);

		/* Refuses a segment that cannot be loaded where it asks. */
		acceptable = segment_acceptable(program, kernel_bytes, ram_bytes);
		if (!acceptable)
			return 0;

		/* Refuses a segment that overlaps one loaded before it. */
		overlaps = segment_overlaps(programs, i, physical, memsz);
		if (overlaps)
			return 0;

		/* The entry point must lie in an executable segment. */
		if ((flags & PF_X) != 0 && entry >= physical && entry < physical + memsz)
			entry_ok = 1;

		/* Reads the segment's file part into place. */
		status = read_bytes(kernel_lba, offset, filesz, scsi_id, (void *)(uintptr_t)physical);
		if (status != 0)
			return 0;

		/* Clears the rest of the segment. */
		zero_bytes((void *)(uintptr_t)(physical + filesz), memsz - filesz);

		/* The kernel reaches at least to the end of this segment. */
		if (physical + memsz > *physical_end)
			*physical_end = physical + memsz;

		loads++;
	}

	/* Refuses a kernel with nothing loaded. */
	if (loads == 0)
		return 0;

	/* Refuses a kernel whose entry point is in no executable segment. */
	if (!entry_ok)
		return 0;

	/* Succeeded: the kernel's entry point. */
	return entry;
}

/* Builds the handoff and the boot device table the kernel reads. */
static void
make_handoff(
	const struct x68k_boot_manifest *manifest,
	uint32_t scsi_id,
	uint32_t kernel_end)
{
	struct x68k_boot_handoff *handoff;
	struct kern_boot_device *device;
	uint32_t ram_bytes;

	/* Finds the handoff and the device table at their fixed addresses. */
	handoff = (struct x68k_boot_handoff *)X68K_HANDOFF_ADDRESS;
	device = (struct kern_boot_device *)X68K_DEVICE_TABLE_ADDRESS;
	ram_bytes = be32(&manifest->ram_bytes);

	/* Clears both before they are filled. */
	zero_bytes(handoff, sizeof(*handoff));
	zero_bytes(device, sizeof(*device));

	/* Fills the common handoff: one SCSI boot device and its root partition. */
	handoff->common.magic = KERN_HANDOFF_MAGIC;
	handoff->common.version = KERN_HANDOFF_VERSION_X68K;
	handoff->common.size = sizeof(*handoff);
	handoff->common.device_count = 1;
	handoff->common.boot_bios_id = (uint8_t)scsi_id;
	handoff->common.boot_partition_scheme = KERN_PARTITION_SCHEME_X68K;
	handoff->common.boot_partition_index = 1;
	handoff->common.device_table = X68K_DEVICE_TABLE_ADDRESS;
	handoff->common.boot_partition_lba = be32(&manifest->root_lba);

	/* Fills the X68000 extension: RAM, the kernel's and the loader's ranges, and the memory map. */
	handoff->extension_magic = X68K_HANDOFF_EXTENSION_MAGIC;
	handoff->extension_version = X68K_HANDOFF_EXTENSION_VERSION;
	handoff->extension_size = sizeof(*handoff) - sizeof(handoff->common);
	handoff->ram_bytes = ram_bytes;
	handoff->kernel_phys_start = X68K_KERNEL_LOW_MIN;
	handoff->kernel_phys_end = kernel_end;
	handoff->loader_phys_start = X68K_STAGE1_ADDRESS;
	handoff->loader_phys_end = X68K_STAGE2_LIMIT;
	handoff->memory_region_count = 2;
	handoff->memory_regions[0].base = 0;
	handoff->memory_regions[0].size = ram_bytes;
	handoff->memory_regions[0].type = KERN_MEMORY_AVAILABLE;
	handoff->memory_regions[1].base = X68K_STAGE1_ADDRESS;
	handoff->memory_regions[1].size = X68K_STAGE2_LIMIT - X68K_STAGE1_ADDRESS;
	handoff->memory_regions[1].type = KERN_MEMORY_RESERVED;

	/* Describes the boot disk. */
	device->device_class = KERN_DEV_SCSI;
	device->display_index = 0;
	device->bios_id = (uint8_t)scsi_id;
	device->flags = KERN_DEV_PRESENT | KERN_DEV_BOOT_ORIGIN;
	device->sector_size = X68K_SECTOR_SIZE;
}
