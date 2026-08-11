/* PC-98 Linux boot adapter. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "linux-boot.h"
#include <hal/hal.h>
#include <kern/image.h>
#include <kern/messages.h>
#include <kern/process.h>
#include <stdint.h>
#include <string.h>

#define BP_ADDR 0x80000U
#define CMD_ADDR 0x81000U
#define PC98_ADDR 0x82000U
#define PC98_SETUP_NODE_SIZE 32U

struct elf32_header {
	uint8_t id[16];
	uint16_t type, machine;
	uint32_t version, entry, phoff, shoff, flags;
	uint16_t ehsize, phsize, phnum;
};
struct elf32_program_header {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

static const struct boots_device *linux_devices;
static unsigned linux_device_count;
static int linux_boot_device;
static uint32_t text_done, text_total, data_done, data_total;
static int progress_class = -1;

void boots_pc98_jump_linux(uint32_t entry, uint32_t boot_params)
	__attribute__((noreturn));

static uint32_t read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_unsigned(unsigned value)
{
	char buffer[11];
	unsigned length = 0;

	if (!value) {
		hal_cons_putc('0');
		return;
	}
	while (value) {
		buffer[length++] = (char)('0' + value % 10);
		value /= 10;
	}
	while (length)
		hal_cons_putc(buffer[--length]);
}

static unsigned kibibytes(uint32_t bytes)
{
	return (bytes >> 10) + !!(bytes & 1023U);
}

static void show_progress(int load_class)
{
	uint32_t done = load_class ? data_done : text_done;
	uint32_t total = load_class ? data_total : text_total;

	if (progress_class >= 0 && progress_class != load_class)
		hal_cons_putc('\n');
	progress_class = load_class;
	hal_cons_putc('\r');
	hal_cons_clear_to_eol();
	hal_cons_putc('\r');
	hal_cons_write((const char *)(load_class ? boots_msg_data : boots_msg_code));
	write_unsigned(kibibytes(done));
	hal_cons_write(" / ");
	write_unsigned(kibibytes(total));
	hal_cons_write(" KB");
	if (done >= total) {
		hal_cons_putc('\n');
		progress_class = -1;
	}
}

static void begin_progress(uint32_t kernel_size)
{
	hal_cons_write((const char *)boots_msg_kernel_size);
	write_unsigned(kibibytes(kernel_size));
	hal_cons_write(" KB\n");
	progress_class = -1;
}

static uint8_t low_u8(uint32_t address)
{
	uint8_t value;

	asm volatile("movb (%1),%0" : "=q"(value) : "r"(address));
	return value;
}

static uint16_t low_u16(uint32_t address)
{
	uint16_t value;

	asm volatile("movw (%1),%0" : "=r"(value) : "r"(address));
	return value;
}

static void build_disk_setup(uint8_t *bp)
{
	uint8_t *previous = NULL;
	unsigned count = 0;

	*(uint32_t *)(bp + 0x250) = 0;
	*(uint32_t *)(bp + 0x254) = 0;
	for (unsigned i = 0; i < linux_device_count; i++) {
		const struct boots_device *d = &linux_devices[i];
		uint8_t *node;

		if ((d->device_class != BOOTS_DEV_IDE &&
		     d->device_class != BOOTS_DEV_SCSI) ||
		    !(d->flags & BOOTS_DEV_HAS_GEOMETRY) || !d->heads ||
		    !d->sectors || count >= 12)
			continue;
		node = (uint8_t *)(PC98_ADDR + count * PC98_SETUP_NODE_SIZE);
		memset(node, 0, PC98_SETUP_NODE_SIZE);
		if (!count)
			*(uint32_t *)(bp + 0x250) = (uint32_t)node;
		if (previous)
			*(uint32_t *)previous = (uint32_t)node;
		*(uint32_t *)(node + 8) = 11;
		*(uint32_t *)(node + 12) = 12;
		*(uint32_t *)(node + 16) = 0x44383950;
		*(uint16_t *)(node + 20) = 1;
		*(uint16_t *)(node + 22) = 12;
		node[24] = d->bios_id;
		node[25] = d->heads;
		node[26] = d->sectors;
		if ((int)i == linux_boot_device)
			node[27] = BOOTS_LINUX_DISK_F_BOOT;
		previous = node;
		count++;
	}
}

static int vmlinux_probe(struct boots_file *file)
{
	struct elf32_header header;

	return file->size <= UINT32_MAX &&
	       boots_file_read(file, 0, &header, sizeof(header)) &&
	       read_le32(header.id) == 0x464c457f && header.id[4] == 1 &&
	       header.id[5] == 1 && header.machine == 3;
}

static void load_progress(void *context, uint32_t bytes)
{
	int load_class = *(const int *)context;

	if (load_class)
		data_done += bytes;
	else
		text_done += bytes;
	show_progress(load_class);
}

static int vmlinux_load(struct boots_file *file, const char *arguments)
{
	struct elf32_header header;
	struct elf32_program_header programs[16];
	uint32_t staging_offsets[16];
	struct hal_pmem staging;
	unsigned segments = 0;
	uint8_t *bp, *e820;
	uint16_t high_mib;
	int entry_valid = 0;
	size_t argument_length, staging_size = 0;
	uint32_t destination_end = 0;

	if (!boots_file_read(file, 0, &header, sizeof(header))) {
		hal_cons_write("Linux: header read failed.\n");
		return 0;
	}
	argument_length = arguments != NULL ? strlen(arguments) : 0;
	if (read_le32(header.id) != 0x464c457f || header.id[4] != 1 ||
	    header.id[5] != 1 || header.machine != 3 ||
	    header.phsize != sizeof(programs[0]) || header.phnum > 16 ||
	    argument_length >= 4096U ||
	    header.phoff > file->size ||
	    (uint32_t)header.phnum >
		(file->size - header.phoff) / sizeof(programs[0]))
	{
		hal_cons_write("Linux: invalid ELF header.\n");
		return 0;
	}
	text_done = text_total = data_done = data_total = 0;
	for (unsigned i = 0; i < header.phnum; i++) {
		struct elf32_program_header *program = &programs[i];
		staging_offsets[i] = 0;
		if (!boots_file_read(file, header.phoff + i * sizeof(programs[0]),
				     program, sizeof(*program))) {
			hal_cons_write("Linux: program header read failed.\n");
			return 0;
		}
		if (program->type != 1)
			continue;
		if (program->filesz > program->memsz ||
		    program->paddr < 0x100000 ||
		    program->paddr + program->memsz < program->paddr ||
		    program->offset + program->filesz < program->offset ||
		    program->offset + program->filesz > file->size)
		{
			hal_cons_write("Linux: invalid load segment.\n");
			return 0;
		}
		if (header.entry >= program->paddr &&
		    header.entry < program->paddr + program->memsz)
			entry_valid = 1;
		if (program->filesz > SIZE_MAX - staging_size)
			return 0;
		staging_offsets[i] = (uint32_t)staging_size;
		staging_size += program->filesz;
		if (program->paddr + program->memsz > destination_end)
			destination_end = program->paddr + program->memsz;
		if (program->flags & 2) {
			if (data_total + program->filesz < data_total)
				return 0;
			data_total += program->filesz;
		} else {
			if (text_total + program->filesz < text_total)
				return 0;
			text_total += program->filesz;
		}
		segments++;
	}
	if (!segments || !entry_valid) {
		hal_cons_write("Linux: no loadable entry segment.\n");
		return 0;
	}
	if (destination_end > UINT32_MAX - 4095U) {
		hal_cons_write("Linux: load destination is out of range.\n");
		return 0;
	}
	if (process_quiesce_users() != 0) {
		hal_cons_write("Linux: a user process is still active.\n");
		return 0;
	}
	if (hal_pmem_alloc_limited(staging_size,
		(destination_end + 4095U) & ~4095U,
		hal_pmem_get_total_size(), &staging) != HAL_PMEM_SUCCESS) {
		hal_cons_write("Linux: no staging memory above destination.\n");
		return 0;
	}
	begin_progress((uint32_t)file->size);
	hal_pc98_enable_high_memory();
	for (unsigned i = 0; i < header.phnum; i++) {
		struct elf32_program_header *program = &programs[i];
		int load_class;

		if (program->type != 1)
			continue;
		load_class = !!(program->flags & 2);
		if (!boots_file_read_progress(file, program->offset,
			(void *)(staging.vaddr + staging_offsets[i]),
			program->filesz, load_progress, &load_class)) {
			(void)hal_pmem_free(&staging);
			hal_cons_write("Linux: staging read failed.\n");
			return 0;
		}
	}
	memset((void *)BP_ADDR, 0, 4096);
	memcpy((void *)CMD_ADDR, arguments != NULL ? arguments : "",
	       argument_length + 1U);
	bp = (uint8_t *)BP_ADDR;
	*(uint32_t *)(bp + 0x228) = CMD_ADDR;
	bp[0x210] = 0xff;
	build_disk_setup(bp);
	e820 = bp + 0x2d0;
	*(uint64_t *)(e820 + 0) = 0;
	*(uint64_t *)(e820 + 8) = ((uint32_t)(low_u8(0x501) & 7) + 1) << 17;
	*(uint32_t *)(e820 + 16) = 1;
	*(uint64_t *)(e820 + 20) = 0x100000;
	*(uint64_t *)(e820 + 28) = (uint32_t)low_u8(0x401) << 17;
	*(uint32_t *)(e820 + 36) = 1;
	high_mib = low_u16(0x594);
	if (high_mib) {
		*(uint64_t *)(e820 + 40) = 0x1000000;
		*(uint64_t *)(e820 + 48) = (uint64_t)high_mib << 20;
		*(uint32_t *)(e820 + 56) = 1;
		bp[0x1e8] = 3;
	} else {
		bp[0x1e8] = 2;
	}
	/* Point of no return.  All fallible file I/O is complete, every user task
	 * is gone, and the staging allocation is disjoint from every destination.
	 * The low closure now commits bytes with interrupts masked and cannot
	 * return through the overwritten high kernel image or heap. */
	(void)hal_irq_disable();
	for (unsigned i = 0; i < header.phnum; i++) {
		struct elf32_program_header *program = &programs[i];
		if (program->type != 1)
			continue;
		memcpy((void *)program->paddr,
		       (const void *)(staging.vaddr + staging_offsets[i]),
		       program->filesz);
		memset((void *)(program->paddr + program->filesz), 0,
		       program->memsz - program->filesz);
	}
	boots_pc98_jump_linux(header.entry, BP_ADDR);
}

static const struct boots_image_loader loader = {
	"vmlinux", vmlinux_probe, vmlinux_load
};

int pc98_linux_boot(struct boots_filesystem *filesystem, const char *path,
		    const char *arguments, const struct boots_device *devices,
		    unsigned count, int boot_device)
{
	linux_devices = devices;
	linux_device_count = count;
	linux_boot_device = boot_device;
	return boots_image_boot(&loader, filesystem, path, arguments);
}
