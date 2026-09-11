/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ELF executable and interpreter loader.
 *
 * A 32-bit or 64-bit image is read through a file content lease, its
 * headers are normalized into one representation, and every PT_LOAD
 * segment is validated before anything is mapped.  Immutable full text
 * pages retain a pinned cache snapshot; writable data and mutable files
 * are copied while the content lease is held.  A position
 * independent main program and an interpreter are placed at a load bias
 * found in the free address space.
 */

#include "kern/io-pool.h"
#include "kern/elf.h"
#include "kern/exec.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vmspace.h"

#include <zedbsd/tls.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ELF_PHNUM_MAX 32U
#define PAGE_SIZE KERN_PAGE_SIZE
#ifdef KERN_USER_ABI_LP64
#define ELF_OFF_MAX 0x7fffffffffffffffULL
#else
#define ELF_OFF_MAX 0x7fffffffULL
#endif

#if defined(HAL_ARCH_SPARCV9)
#define ELF_EXPECTED_DATA ELFDATA2MSB
#define ELF32_EXPECTED_MACHINE EM_386
#define ELF64_EXPECTED_MACHINE EM_SPARCV9
#elif defined(HAL_ARCH_AMD64)
#define ELF_EXPECTED_DATA ELFDATA2LSB
#define ELF32_EXPECTED_MACHINE EM_386
#define ELF64_EXPECTED_MACHINE EM_X86_64
#elif defined(HAL_ARCH_ARM64)
#define ELF_EXPECTED_DATA ELFDATA2LSB
#define ELF32_EXPECTED_MACHINE EM_386
#define ELF64_EXPECTED_MACHINE EM_AARCH64
#elif defined(HAL_ARCH_I386)
#define ELF_EXPECTED_DATA ELFDATA2LSB
#define ELF32_EXPECTED_MACHINE EM_386
#define ELF64_EXPECTED_MACHINE EM_X86_64
#elif defined(HAL_ARCH_M68K)
#define ELF_EXPECTED_DATA ELFDATA2MSB
#define ELF32_EXPECTED_MACHINE EM_68K
#define ELF64_EXPECTED_MACHINE 0xffffU
#else
#error ELF machine is not defined for this architecture
#endif

enum elf_load_role {
	ELF_LOAD_MAIN,
	ELF_LOAD_INTERPRETER,
};

struct normalized_header {
	unsigned elf_class;
	uint16_t type;
	uint16_t machine;
	uint64_t entry;
	uint64_t phoff;
	uint16_t phentsize;
	uint16_t phnum;
};

struct normalized_program {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
};

struct normalized_image {
	uintptr_t thread_pointer;
	uintptr_t entry;
	uintptr_t brk_start;
	size_t static_data_size;
	uintptr_t program_headers;
	uintptr_t load_bias;
	size_t stack_size;
	uint16_t program_header_size;
	uint16_t program_header_count;
	unsigned has_interpreter;
	char interpreter[EXEC_INTERP_MAX];
};

static int temporary_writable_plt(const struct normalized_program *program);
static uint32_t initial_segment_prot(const struct normalized_program *program);
static uint16_t elf_u16(const void *field, unsigned data);
static uint32_t elf_u32(const void *field, unsigned data);
static uint64_t elf_u64(const void *field, unsigned data);
static void decode_elf32_header(struct elf32_ehdr *header, unsigned data);
static void decode_elf32_program(struct elf32_phdr *program, unsigned data);
static void decode_elf64_header(struct elf64_ehdr *header, unsigned data);
static void decode_elf64_program(struct elf64_phdr *program, unsigned data);
static int read_exact(struct file_content_lease *lease, off_t offset, void *buffer, size_t size);
static int power_of_two64(uint64_t value);
static uint32_t segment_prot(uint32_t flags);
static int read_headers(struct file_content_lease *lease, unsigned elf_class, struct normalized_header *header, struct normalized_program **programs_out, uint64_t *file_size_out);
static int copy_segment_snapshot(struct file_content_lease *lease, struct vmspace *vm, uintptr_t destination, off_t source, size_t size);
static int load_segment_snapshot(struct file_content_lease *lease, struct vmspace *vm, uintptr_t destination, off_t source, size_t size, uint32_t prot);
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_I386)
static int load_static_tls(struct file_content_lease *, struct vmspace *, const struct normalized_program *, uintptr_t *);
#endif
static int validate_and_load(struct file_content_lease *lease, struct vmspace *vm, unsigned elf_class, enum elf_load_role role, struct normalized_image *image);
static void copy_image32(struct elf32_image_info *destination, const struct normalized_image *source);
static void copy_image64(struct elf64_image_info *destination, const struct normalized_image *source);

/*
 * Loads a 32-bit main program from a held content lease.
 */
int
elf32_load_content(
	struct file_content_lease *lease,
	struct vmspace *vm,
	struct elf32_image_info *image)
{
	struct normalized_image normalized;
	int error;

	/* Rejects a missing result. */
	if (image == NULL)
		return EINVAL;

	/* Loads the image and describes it in the 32-bit record. */
	error = validate_and_load(lease, vm, ELFCLASS32, ELF_LOAD_MAIN,
	    &normalized);
	if (error == 0)
		copy_image32(image, &normalized);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Loads a 32-bit main program from a file.
 */
int
elf32_load(
	struct file *file,
	struct vmspace *vm,
	struct elf32_image_info *image)
{
	struct file_content_lease lease;
	int error;

	/* Holds the file content while it is read. */
	error = file_exec_snapshot_begin(file, &lease);
	if (error != 0)
		return error;
	error = elf32_load_content(&lease, vm, image);
	file_content_lease_end(&lease);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Loads a 32-bit interpreter from a file.
 */
int
elf32_load_interpreter(
	struct file *file,
	struct vmspace *vm,
	struct elf32_image_info *image)
{
	struct file_content_lease lease;
	struct normalized_image normalized;
	int error;

	/* Rejects a missing result. */
	if (image == NULL)
		return EINVAL;

	/* Holds the file content while the interpreter is loaded. */
	error = file_exec_snapshot_begin(file, &lease);
	if (error != 0)
		return error;
	error = validate_and_load(&lease, vm, ELFCLASS32,
	    ELF_LOAD_INTERPRETER, &normalized);
	if (error == 0)
		copy_image32(image, &normalized);
	file_content_lease_end(&lease);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Loads a 64-bit main program from a held content lease.
 */
int
elf64_load_content(
	struct file_content_lease *lease,
	struct vmspace *vm,
	struct elf64_image_info *image)
{
	struct normalized_image normalized;
	int error;

	/* Rejects a missing result. */
	if (image == NULL)
		return EINVAL;

	/* Loads the image and describes it in the 64-bit record. */
	error = validate_and_load(lease, vm, ELFCLASS64, ELF_LOAD_MAIN,
	    &normalized);
	if (error == 0)
		copy_image64(image, &normalized);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Loads a 64-bit main program from a file.
 */
int
elf64_load(
	struct file *file,
	struct vmspace *vm,
	struct elf64_image_info *image)
{
	struct file_content_lease lease;
	int error;

	/* Holds the file content while it is read. */
	error = file_exec_snapshot_begin(file, &lease);
	if (error != 0)
		return error;
	error = elf64_load_content(&lease, vm, image);
	file_content_lease_end(&lease);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Loads a 64-bit interpreter from a file.
 */
int
elf64_load_interpreter(
	struct file *file,
	struct vmspace *vm,
	struct elf64_image_info *image)
{
	struct file_content_lease lease;
	struct normalized_image normalized;
	int error;

	/* Rejects a missing result. */
	if (image == NULL)
		return EINVAL;

	/* Holds the file content while the interpreter is loaded. */
	error = file_exec_snapshot_begin(file, &lease);
	if (error != 0)
		return error;
	error = validate_and_load(&lease, vm, ELFCLASS64, ELF_LOAD_INTERPRETER,
	    &normalized);
	if (error == 0)
		copy_image64(image, &normalized);
	file_content_lease_end(&lease);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Recognizes the one-page RWX PLT segment that SPARC V9 binaries carry. */
static int
temporary_writable_plt(
	const struct normalized_program *program)
{
#if defined(HAL_ARCH_SPARCV9)
	/* The segment is a single page-aligned page fully backed by the file. */
	if (program->flags != (PF_R | PF_W | PF_X))
		return 0;
	if (program->filesz != program->memsz)
		return 0;
	if (program->memsz == 0)
		return 0;
	if (program->memsz > PAGE_SIZE)
		return 0;
	if ((program->vaddr & (PAGE_SIZE - 1U)) != 0)
		return 0;
	if ((program->offset & (PAGE_SIZE - 1U)) != 0)
		return 0;

	/* Reports the PLT segment. */
	return 1;
#else
	(void)program;

	/* No other architecture has such a segment. */
	return 0;
#endif
}

/* Computes the protection a segment is published with. */
static uint32_t
initial_segment_prot(
	const struct normalized_program *program)
{
	uint32_t prot;

	/* A temporary writable PLT starts without execute permission. */
	prot = segment_prot(program->flags);
	if (temporary_writable_plt(program))
		prot &= ~HAL_SPACE_EXEC;

	/* Reports the protection. */
	return prot;
}

/* Reads a 16-bit field in the image's byte order. */
static uint16_t
elf_u16(
	const void *field,
	unsigned data)
{
	const uint8_t *p;

	/* Assembles the bytes big-endian or little-endian. */
	p = field;
	if (data == ELFDATA2MSB)
		return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
	return (uint16_t)((uint16_t)p[1] << 8 | p[0]);
}

/* Reads a 32-bit field in the image's byte order. */
static uint32_t
elf_u32(
	const void *field,
	unsigned data)
{
	const uint8_t *p;

	/* Assembles the bytes big-endian or little-endian. */
	p = field;
	if (data == ELFDATA2MSB)
		return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		    (uint32_t)p[2] << 8 | p[3];
	return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[1] << 8 | p[0];
}

/* Reads a 64-bit field in the image's byte order. */
static uint64_t
elf_u64(
	const void *field,
	unsigned data)
{
	const uint8_t *p;
	uint64_t high;
	uint64_t low;

	/* Reads the two halves in the order the byte order puts them. */
	p = field;
	if (data == ELFDATA2MSB) {
		high = elf_u32(p, data);
		low = elf_u32(p + 4, data);
	} else {
		low = elf_u32(p, data);
		high = elf_u32(p + 4, data);
	}

	/* Reports the assembled value. */
	return high << 32 | low;
}

/* Converts a 32-bit ELF header to host byte order in place. */
static void
decode_elf32_header(
	struct elf32_ehdr *header,
	unsigned data)
{
	header->e_type = elf_u16(&header->e_type, data);
	header->e_machine = elf_u16(&header->e_machine, data);
	header->e_version = elf_u32(&header->e_version, data);
	header->e_entry = elf_u32(&header->e_entry, data);
	header->e_phoff = elf_u32(&header->e_phoff, data);
	header->e_shoff = elf_u32(&header->e_shoff, data);
	header->e_flags = elf_u32(&header->e_flags, data);
	header->e_ehsize = elf_u16(&header->e_ehsize, data);
	header->e_phentsize = elf_u16(&header->e_phentsize, data);
	header->e_phnum = elf_u16(&header->e_phnum, data);
	header->e_shentsize = elf_u16(&header->e_shentsize, data);
	header->e_shnum = elf_u16(&header->e_shnum, data);
	header->e_shstrndx = elf_u16(&header->e_shstrndx, data);
}

/* Converts a 32-bit program header to host byte order in place. */
static void
decode_elf32_program(
	struct elf32_phdr *program,
	unsigned data)
{
	program->p_type = elf_u32(&program->p_type, data);
	program->p_offset = elf_u32(&program->p_offset, data);
	program->p_vaddr = elf_u32(&program->p_vaddr, data);
	program->p_paddr = elf_u32(&program->p_paddr, data);
	program->p_filesz = elf_u32(&program->p_filesz, data);
	program->p_memsz = elf_u32(&program->p_memsz, data);
	program->p_flags = elf_u32(&program->p_flags, data);
	program->p_align = elf_u32(&program->p_align, data);
}

/* Converts a 64-bit ELF header to host byte order in place. */
static void
decode_elf64_header(
	struct elf64_ehdr *header,
	unsigned data)
{
	header->e_type = elf_u16(&header->e_type, data);
	header->e_machine = elf_u16(&header->e_machine, data);
	header->e_version = elf_u32(&header->e_version, data);
	header->e_entry = elf_u64(&header->e_entry, data);
	header->e_phoff = elf_u64(&header->e_phoff, data);
	header->e_shoff = elf_u64(&header->e_shoff, data);
	header->e_flags = elf_u32(&header->e_flags, data);
	header->e_ehsize = elf_u16(&header->e_ehsize, data);
	header->e_phentsize = elf_u16(&header->e_phentsize, data);
	header->e_phnum = elf_u16(&header->e_phnum, data);
	header->e_shentsize = elf_u16(&header->e_shentsize, data);
	header->e_shnum = elf_u16(&header->e_shnum, data);
	header->e_shstrndx = elf_u16(&header->e_shstrndx, data);
}

/* Converts a 64-bit program header to host byte order in place. */
static void
decode_elf64_program(
	struct elf64_phdr *program,
	unsigned data)
{
	program->p_type = elf_u32(&program->p_type, data);
	program->p_flags = elf_u32(&program->p_flags, data);
	program->p_offset = elf_u64(&program->p_offset, data);
	program->p_vaddr = elf_u64(&program->p_vaddr, data);
	program->p_paddr = elf_u64(&program->p_paddr, data);
	program->p_filesz = elf_u64(&program->p_filesz, data);
	program->p_memsz = elf_u64(&program->p_memsz, data);
	program->p_align = elf_u64(&program->p_align, data);
}

/* Reads exactly a number of bytes at a file offset through the lease. */
static int
read_exact(
	struct file_content_lease *lease,
	off_t offset,
	void *buffer,
	size_t size)
{
	ssize_t count;

	/* Rejects an inactive lease or a negative offset. */
	if (lease == NULL || !lease->active || offset < 0)
		return EINVAL;

	/* A short read counts as an I/O error. */
	count = file_content_lease_pread(lease, buffer, size, offset);
	if (count != (ssize_t)size)
		return EIO;

	/* Reports the complete read. */
	return 0;
}

/* Tests whether a value is a power of two. */
static int
power_of_two64(
	uint64_t value)
{
	/* Zero is not a power of two; a power of two has one bit set. */
	if (value == 0)
		return 0;
	if ((value & (value - 1U)) != 0)
		return 0;

	/* Reports a power of two. */
	return 1;
}

/* Converts segment flags to address space protection bits. */
static uint32_t
segment_prot(
	uint32_t flags)
{
	uint32_t prot;

	/* Maps each flag onto its protection bit. */
	prot = 0;
	if (flags & PF_R)
		prot |= HAL_SPACE_READ;
	if (flags & PF_W)
		prot |= HAL_SPACE_WRITE;
	if (flags & PF_X)
		prot |= HAL_SPACE_EXEC;

	/* Reports the protection. */
	return prot;
}

/* Reads and normalizes the ELF header and program headers of one class. */
static int
read_headers(
	struct file_content_lease *lease,
	unsigned elf_class,
	struct normalized_header *header,
	struct normalized_program **programs_out,
	uint64_t *file_size_out)
{
	struct file *file;
	struct normalized_program *programs;
	struct elf32_ehdr raw32;
	struct elf32_phdr *raw_programs32;
	struct elf64_ehdr raw64;
	struct elf64_phdr *raw_programs64;
	uint64_t file_size;
	unsigned i;

	/* Rejects an inactive lease, a file without an inode, or a missing result. */
	if (lease == NULL || !lease->active)
		return EINVAL;
	file = lease->file;
	if (file == NULL ||
	    file->f_inode == NULL ||
	    header == NULL ||
	    programs_out == NULL ||
	    file_size_out == NULL ||
	    lease->size < 0)
		return EINVAL;

	/* A file beyond the offset range cannot be addressed. */
	file_size = (uint64_t)lease->size;
	if (file_size > ELF_OFF_MAX)
		return ENOEXEC;
	memset(header, 0, sizeof(*header));

	if (elf_class == ELFCLASS32) {
		/* Reads and checks the 32-bit identification and header. */
		if (file_size < sizeof(raw32) ||
		    read_exact(lease, 0, &raw32, sizeof(raw32)) != 0)
			return ENOEXEC;
		if (raw32.e_ident[EI_MAG0] != ELFMAG0 ||
		    raw32.e_ident[EI_MAG1] != ELFMAG1 ||
		    raw32.e_ident[EI_MAG2] != ELFMAG2 ||
		    raw32.e_ident[EI_MAG3] != ELFMAG3 ||
		    raw32.e_ident[EI_CLASS] != ELFCLASS32 ||
		    raw32.e_ident[EI_DATA] != ELF_EXPECTED_DATA ||
		    raw32.e_ident[EI_VERSION] != EV_CURRENT)
			return ENOEXEC;
		decode_elf32_header(&raw32, ELF_EXPECTED_DATA);
		if (raw32.e_machine != ELF32_EXPECTED_MACHINE ||
		    raw32.e_version != EV_CURRENT ||
		    raw32.e_ehsize != sizeof(raw32) ||
		    raw32.e_phentsize != sizeof(struct elf32_phdr) ||
		    raw32.e_phnum == 0 ||
		    raw32.e_phnum > ELF_PHNUM_MAX ||
		    raw32.e_phoff > file_size ||
		    raw32.e_phnum > (file_size - raw32.e_phoff) / sizeof(struct elf32_phdr))
			return ENOEXEC;

		/* Reads the program headers. */
		raw_programs32 = kern_malloc((size_t)raw32.e_phnum * sizeof(*raw_programs32));
		if (raw_programs32 == NULL)
			return ENOMEM;
		if (read_exact(lease, (off_t)raw32.e_phoff, raw_programs32,
		    (size_t)raw32.e_phnum * sizeof(*raw_programs32)) != 0) {
			kern_free(raw_programs32);
			return ENOEXEC;
		}

		for (i = 0; i < raw32.e_phnum; i++)
			decode_elf32_program(&raw_programs32[i], ELF_EXPECTED_DATA);

		/* Normalizes the program headers. */
		programs = kern_calloc(raw32.e_phnum, sizeof(*programs));
		if (programs == NULL) {
			kern_free(raw_programs32);
			return ENOMEM;
		}

		for (i = 0; i < raw32.e_phnum; i++) {
			programs[i].type = raw_programs32[i].p_type;
			programs[i].flags = raw_programs32[i].p_flags;
			programs[i].offset = raw_programs32[i].p_offset;
			programs[i].vaddr = raw_programs32[i].p_vaddr;
			programs[i].filesz = raw_programs32[i].p_filesz;
			programs[i].memsz = raw_programs32[i].p_memsz;
			programs[i].align = raw_programs32[i].p_align;
		}

		kern_free(raw_programs32);

		/* Normalizes the header. */
		header->type = raw32.e_type;
		header->machine = raw32.e_machine;
		header->entry = raw32.e_entry;
		header->phoff = raw32.e_phoff;
		header->phentsize = raw32.e_phentsize;
		header->phnum = raw32.e_phnum;
	} else {
		/* Reads and checks the 64-bit identification and header. */
		if (file_size < sizeof(raw64) ||
		    read_exact(lease, 0, &raw64, sizeof(raw64)) != 0)
			return ENOEXEC;
		if (raw64.e_ident[EI_MAG0] != ELFMAG0 ||
		    raw64.e_ident[EI_MAG1] != ELFMAG1 ||
		    raw64.e_ident[EI_MAG2] != ELFMAG2 ||
		    raw64.e_ident[EI_MAG3] != ELFMAG3 ||
		    raw64.e_ident[EI_CLASS] != ELFCLASS64 ||
		    raw64.e_ident[EI_DATA] != ELF_EXPECTED_DATA ||
		    raw64.e_ident[EI_VERSION] != EV_CURRENT)
			return ENOEXEC;
		decode_elf64_header(&raw64, ELF_EXPECTED_DATA);
		if (raw64.e_machine != ELF64_EXPECTED_MACHINE ||
		    raw64.e_version != EV_CURRENT ||
		    raw64.e_ehsize != sizeof(raw64) ||
		    raw64.e_phentsize != sizeof(struct elf64_phdr) ||
		    raw64.e_phnum == 0 ||
		    raw64.e_phnum > ELF_PHNUM_MAX ||
		    raw64.e_phoff > file_size ||
		    raw64.e_phnum > (file_size - raw64.e_phoff) / sizeof(struct elf64_phdr))
			return ENOEXEC;

		/* Reads the program headers. */
		raw_programs64 = kern_malloc((size_t)raw64.e_phnum * sizeof(*raw_programs64));
		if (raw_programs64 == NULL)
			return ENOMEM;
		if (read_exact(lease, (off_t)raw64.e_phoff, raw_programs64,
		    (size_t)raw64.e_phnum * sizeof(*raw_programs64)) != 0) {
			kern_free(raw_programs64);
			return ENOEXEC;
		}

		for (i = 0; i < raw64.e_phnum; i++)
			decode_elf64_program(&raw_programs64[i], ELF_EXPECTED_DATA);

		/* Normalizes the program headers. */
		programs = kern_calloc(raw64.e_phnum, sizeof(*programs));
		if (programs == NULL) {
			kern_free(raw_programs64);
			return ENOMEM;
		}

		for (i = 0; i < raw64.e_phnum; i++) {
			programs[i].type = raw_programs64[i].p_type;
			programs[i].flags = raw_programs64[i].p_flags;
			programs[i].offset = raw_programs64[i].p_offset;
			programs[i].vaddr = raw_programs64[i].p_vaddr;
			programs[i].filesz = raw_programs64[i].p_filesz;
			programs[i].memsz = raw_programs64[i].p_memsz;
			programs[i].align = raw_programs64[i].p_align;
		}

		kern_free(raw_programs64);

		/* Normalizes the header. */
		header->type = raw64.e_type;
		header->machine = raw64.e_machine;
		header->entry = raw64.e_entry;
		header->phoff = raw64.e_phoff;
		header->phentsize = raw64.e_phentsize;
		header->phnum = raw64.e_phnum;
	}

	header->elf_class = elf_class;
	*programs_out = programs;
	*file_size_out = file_size;

	/* Reports the normalized headers. */
	return 0;
}

/* Copies file bytes through a bounded pool run or a small stack fallback. */
static int
copy_segment_snapshot(
	struct file_content_lease *lease,
	struct vmspace *vm,
	uintptr_t destination,
	off_t source,
	size_t size)
{
	uint8_t fallback[512];
	uint8_t *buffer;
	size_t capacity;
	size_t done;
	size_t chunk;
	ssize_t count;
	int error;

	done = 0;
	error = 0;

	/* An empty segment needs no copy. */
	if (size == 0)
		return 0;

	/* Borrows without waiting while the executable content lease is held. */
	capacity = sizeof(fallback);
	buffer = io_pool_borrow(size, &capacity);
	if (buffer == NULL)
		buffer = fallback;
	while (done < size) {
		if (size - done > capacity)
			chunk = capacity;
		else
			chunk = size - done;
		if ((uint64_t)source + done > ELF_OFF_MAX) {
			error = EOVERFLOW;
			break;
		}

		count = file_content_lease_pread(lease, buffer, chunk,
		    source + (off_t)done);
		if (count != (ssize_t)chunk) {
			if (count < 0)
				error = (int)-count;
			else
				error = EIO;
			break;
		}

		error = vmspace_copy_to(vm, destination + done, buffer, chunk);
		if (error != 0)
			break;
		done += chunk;
	}

	if (buffer != fallback)
		io_pool_release(buffer);

	/* Reports why the copy failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Shares immutable full pages, retaining private edge and BSS pages. */
static int
load_segment_snapshot(
	struct file_content_lease *lease,
	struct vmspace *vm,
	uintptr_t destination,
	off_t source,
	size_t size,
	uint32_t prot)
{
	struct file_exec_snapshot *snapshot;
	uintptr_t first, end;
	size_t leading, shared;
	int error;

	/* A writable or unshareable segment is copied rather than mapped. */
	if (!lease->shared_read || lease->read_object == NULL ||
	    (prot & HAL_SPACE_WRITE) != 0 || size < PAGE_SIZE)
		return copy_segment_snapshot(lease, vm, destination, source, size);

	/* Finds the whole pages inside the segment, if there are any. */
	first = (destination + PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
	end = (destination + size) & ~(uintptr_t)(PAGE_SIZE - 1U);
	if (first >= end)
		return copy_segment_snapshot(lease, vm, destination, source, size);

	/* Only a file offset that stays page aligned can be mapped. */
	leading = first - destination;
	shared = end - first;
	if (((uint64_t)source + leading) % PAGE_SIZE != 0)
		return copy_segment_snapshot(lease, vm, destination, source, size);

	/* Optional cache admission fails back before altering the original mapping. */
	error = file_exec_snapshot_create(lease, source + (off_t)leading,
	    shared, &snapshot);
	if (error == ENOMEM || error == EAGAIN || error == EOPNOTSUPP)
		return copy_segment_snapshot(lease, vm, destination, source, size);
	if (error != 0)
		return error;
	error = vmspace_unmap(vm, first, shared);
	if (error == 0)
		error = vmspace_map_exec_snapshot(vm, first, prot, snapshot);
	file_exec_snapshot_put(snapshot);
	if (error != 0)
		return error;

	/* Never copy over shared text; only partial file pages need private bytes. */
	error = copy_segment_snapshot(lease, vm, destination, source, leading);
	if (error == 0)
		error = copy_segment_snapshot(lease, vm, end,
		    source + (off_t)(leading + shared), size - leading - shared);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Validates an image of one class and role and maps its segments. */
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_I386)
/* Builds a private initial block and a VM-owned immutable thread template. */
static int
load_static_tls(
	struct file_content_lease *lease,
	struct vmspace *vm,
	const struct normalized_program *tls,
	uintptr_t *thread_pointer)
{
	struct kern_tls_prefix prefix;
	uintptr_t template_address;
	uintptr_t mapping;
	uintptr_t tp;
	size_t template_size;
	size_t payload_size;
	size_t mapping_size;
	size_t alignment;
	size_t distance;
	int error;

	alignment = (size_t)tls->align;
	if (alignment == 0)
		alignment = 1;
	distance = (size_t)tls->memsz +
	    (size_t)((0U - tls->vaddr - tls->memsz) & (alignment - 1U));
	payload_size = (distance + PAGE_SIZE - 1U) & ~(size_t)(PAGE_SIZE - 1U);
	mapping_size = payload_size + KERN_TLS_TCB_RESERVE;
	template_size = ((size_t)tls->filesz + PAGE_SIZE - 1U) &
	    ~(size_t)(PAGE_SIZE - 1U);
	template_address = 0;

	/* Empty/zero-fill TLS requires no file template mapping. */
	if (template_size != 0) {
		error = vmspace_map_find(vm, vm_layout.mmap_base, template_size,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, &template_address);
		if (error != 0)
			return error;
		error = copy_segment_snapshot(lease, vm, template_address,
		    (off_t)tls->offset, (size_t)tls->filesz);
		if (error != 0)
			goto free_template;
		error = vmspace_protect(vm, template_address, template_size, HAL_SPACE_READ);
		if (error != 0)
			goto free_template;
	}

	error = vmspace_map_find(vm, vm_layout.mmap_base, mapping_size,
	    HAL_SPACE_READ | HAL_SPACE_WRITE, &mapping);
	if (error != 0)
		goto free_template;
	tp = mapping + payload_size;
	error = copy_segment_snapshot(lease, vm, tp - distance,
	    (off_t)tls->offset, (size_t)tls->filesz);
	if (error != 0)
		goto free_mapping;

	/* Only the shared prefix is known to the loader; the tail remains zero. */
	memset(&prefix, 0, sizeof(prefix));
	prefix.self = tp;
	prefix.mapping_base = mapping;
	prefix.mapping_size = mapping_size;
	prefix.template_address = template_address;
	prefix.template_size = (size_t)tls->filesz;
	prefix.memory_size = (size_t)tls->memsz;
	prefix.alignment = alignment;
	prefix.distance = distance;
	error = vmspace_copy_to(vm, tp, &prefix, sizeof(prefix));
	if (error != 0)
		goto free_mapping;
	*thread_pointer = tp;
	return 0;

free_mapping:
	(void)vmspace_unmap(vm, mapping, mapping_size);
free_template:
	if (template_size != 0)
		(void)vmspace_unmap(vm, template_address, template_size);
	return error;
}

#endif

static int
validate_and_load(
	struct file_content_lease *lease,
	struct vmspace *vm,
	unsigned elf_class,
	enum elf_load_role role,
	struct normalized_image *image)
{
	struct file *file;
	struct normalized_header header;
	struct normalized_program *programs;
	struct normalized_program *program;
	struct normalized_program *other;
	struct normalized_program *segment;
	struct normalized_program *tls;
	int tls_contained;
	uintptr_t mapped_start[ELF_PHNUM_MAX];
	size_t mapped_size[ELF_PHNUM_MAX];
	uint64_t file_size;
	uint64_t minimum;
	uint64_t maximum;
	uint64_t maximum_align;
	uint64_t data_minimum;
	uint64_t data_maximum;
	uint64_t phdr_file_end;
	uint64_t phdr_vaddr;
	uint64_t start;
	uint64_t end;
	uint64_t expected_size;
	uint64_t requested;
	uint64_t other_start;
	uint64_t other_end;
	uint64_t span;
	uintptr_t load_bias;
	uintptr_t base;
	uintptr_t map_start;
	uintptr_t map_end;
	uintptr_t data_start;
	unsigned i;
	unsigned j;
	unsigned mapped_count;
	unsigned loads;
	unsigned dynamic_count;
	unsigned interp_count;
	unsigned stack_count;
	unsigned phdr_count;
	int position_independent;
	int entry_ok;
	int phdr_ok;
	int error;

	/* Starts with an empty image and an inverted address range. */
	programs = NULL;
	tls = NULL;
	tls_contained = 0;
	minimum = UINT64_MAX;
	maximum = 0;
	maximum_align = PAGE_SIZE;
	data_minimum = UINT64_MAX;
	data_maximum = 0;
	phdr_vaddr = 0;
	load_bias = 0;
	mapped_count = 0;
	loads = 0;
	dynamic_count = 0;
	interp_count = 0;
	stack_count = 0;
	phdr_count = 0;
	entry_ok = 0;
	phdr_ok = 0;

	/* Rejects an inactive lease, a missing file, vmspace, or result. */
	if (lease == NULL || !lease->active)
		return EINVAL;
	file = lease->file;
	if (file == NULL || vm == NULL || image == NULL)
		return EINVAL;

	/* Starts from the default stack size and the normalized headers. */
	vmspace_layout_init();
	memset(image, 0, sizeof(*image));
	image->stack_size = EXEC_STACK_DEFAULT_SIZE;
	error = read_headers(lease, elf_class, &header, &programs, &file_size);
	if (error != 0)
		return error;

	/* A main program is an executable or shared object; an interpreter a shared object. */
	if ((role == ELF_LOAD_MAIN &&
	     header.type != ET_EXEC &&
	     header.type != ET_DYN) ||
	    (role == ELF_LOAD_INTERPRETER && header.type != ET_DYN))
		goto invalid;
	position_independent = 0;
	if (role == ELF_LOAD_MAIN && header.type == ET_DYN)
		position_independent = 1;

	/* The program header table must lie within the file. */
	if (header.phnum > UINT64_MAX / header.phentsize)
		goto invalid;
	phdr_file_end = header.phoff +
	    (uint64_t)header.phnum * header.phentsize;
	if (phdr_file_end < header.phoff || phdr_file_end > file_size)
		goto invalid;

	/* Validates every program header and gathers the layout. */
	for (i = 0; i < header.phnum; i++) {
		/* PT_PHDR must describe the table itself, read-only, once. */
		program = &programs[i];
		if (program->type == PT_PHDR) {
			expected_size =
			    (uint64_t)header.phnum * header.phentsize;
			phdr_count++;
			if (phdr_count != 1 ||
			    program->offset != header.phoff ||
			    program->filesz != expected_size ||
			    program->memsz < program->filesz ||
			    (program->flags & PF_R) == 0 ||
			    (program->flags & (PF_W | PF_X)) != 0 ||
			    program->vaddr > UINT64_MAX - program->memsz)
				goto invalid;
			phdr_vaddr = program->vaddr;
			continue;
		}

		/* PT_INTERP must name the one supported interpreter, once. */
		if (program->type == PT_INTERP) {
			interp_count++;
			if (role != ELF_LOAD_MAIN ||
			    interp_count != 1 ||
			    program->filesz < 2 ||
			    program->filesz > EXEC_INTERP_MAX ||
			    program->offset > file_size ||
			    program->filesz > file_size - program->offset ||
			    read_exact(lease, (off_t)program->offset, image->interpreter,
			    (size_t)program->filesz) != 0 ||
			    image->interpreter[program->filesz - 1U] != '\0' ||
			    strlen(image->interpreter) + 1U != program->filesz ||
			    strcmp(image->interpreter, EXEC_INTERP_PATH) != 0)
				goto invalid;
			continue;
		}

		/* PT_DYNAMIC appears at most once. */
		if (program->type == PT_DYNAMIC) {
			dynamic_count++;
			if (dynamic_count != 1)
				goto invalid;
			continue;
		}

		/* PT_GNU_STACK must ask for a non-executable stack, sized in pages. */
		if (program->type == PT_GNU_STACK) {
			requested = program->memsz;
			stack_count++;
			if (stack_count != 1 ||
			    program->filesz != 0 ||
			    (program->flags & ~(PF_R | PF_W | PF_X)) != 0 ||
			    (program->flags & (PF_R | PF_W)) != (PF_R | PF_W) ||
			    (program->flags & PF_X) != 0 ||
			    requested > EXEC_STACK_HARD_MAX)
				goto invalid;
			if (requested != 0) {
				requested = (requested + PAGE_SIZE - 1U) &
				    ~(uint64_t)(PAGE_SIZE - 1U);
				if (requested == 0 || requested > EXEC_STACK_HARD_MAX)
					goto invalid;
				image->stack_size = (size_t)requested;
			}

			continue;
		}

		/* Validate TLS before allocating any part of the candidate image. */
		if (program->type == PT_TLS) {
			if (tls != NULL)
				goto invalid;
			tls = program;
			if (tls->filesz > tls->memsz)
				goto invalid;
			if (tls->memsz > KERN_TLS_MEMORY_MAX)
				goto invalid;
			if (tls->offset > file_size)
				goto invalid;
			if (tls->filesz > file_size - tls->offset)
				goto invalid;
			if (tls->vaddr > UINT64_MAX - tls->memsz)
				goto invalid;
			if (tls->align > KERN_TLS_ALIGN_MAX)
				goto invalid;
			if (tls->align > 1U) {
				if (!power_of_two64(tls->align))
					goto invalid;
				if (((tls->offset ^ tls->vaddr) & (tls->align - 1U)) != 0)
					goto invalid;
			}
			continue;
		}

		if (program->type != PT_LOAD)
			continue;

		/* A PT_LOAD must be sane, in the file, page-congruent, and aligned. */
		loads++;
		if (program->memsz == 0 ||
		    program->filesz > program->memsz ||
		    (program->flags & ~(PF_R | PF_W | PF_X)) != 0 ||
		    ((program->flags & (PF_W | PF_X)) == (PF_W | PF_X) &&
		     !temporary_writable_plt(program)) ||
		    segment_prot(program->flags) == 0 ||
		    program->offset > file_size ||
		    program->filesz > file_size - program->offset ||
		    ((program->offset ^ program->vaddr) & (PAGE_SIZE - 1U)) != 0 ||
		    (program->align > 1U &&
		     (!power_of_two64(program->align) ||
		      ((program->offset ^ program->vaddr) &
		       (program->align - 1U)) != 0)) ||
		    program->vaddr > UINT64_MAX - program->memsz ||
		    program->vaddr > UINT64_MAX - (PAGE_SIZE - 1U) ||
		    program->vaddr + program->memsz >
		    UINT64_MAX - (PAGE_SIZE - 1U))
			goto invalid;

		/* Extends the image bounds by the page-rounded segment. */
		start = program->vaddr & ~(uint64_t)(PAGE_SIZE - 1U);
		end = (program->vaddr + program->memsz + PAGE_SIZE - 1U) &
		    ~(uint64_t)(PAGE_SIZE - 1U);
		if (end <= start)
			goto invalid;
		if (start < minimum)
			minimum = start;
		if (end > maximum)
			maximum = end;
		if (program->align > maximum_align)
			maximum_align = program->align;

		/* Segments must not overlap page-wise. */
		for (j = 0; j < i; j++) {
			other = &programs[j];
			if (other->type != PT_LOAD)
				continue;
			other_start = other->vaddr & ~(uint64_t)(PAGE_SIZE - 1U);
			other_end = (other->vaddr + other->memsz + PAGE_SIZE - 1U) &
			    ~(uint64_t)(PAGE_SIZE - 1U);
			if (start < other_end && other_start < end)
				goto invalid;
		}

		/* The entry point must lie in an executable segment. */
		if ((program->flags & PF_X) &&
		    header.entry >= program->vaddr &&
		    header.entry < program->vaddr + program->memsz)
			entry_ok = 1;

		/* The writable segments of a main program bound its static data. */
		if (role == ELF_LOAD_MAIN && (program->flags & PF_W) != 0) {
			if (program->vaddr < data_minimum)
				data_minimum = program->vaddr;
			if (program->vaddr + program->memsz > data_maximum)
				data_maximum = program->vaddr + program->memsz;
		}

		/* The segment that holds the program header table locates it in memory. */
		if (header.phoff >= program->offset &&
		    phdr_file_end >= header.phoff &&
		    phdr_file_end <= program->offset + program->filesz) {
			image->program_headers = (uintptr_t)(program->vaddr +
			    (header.phoff - program->offset));
			phdr_ok = 1;
		}
	}

	/* Checks the image as a whole against its role. */
	if (loads == 0 ||
	    !entry_ok ||
	    (role == ELF_LOAD_INTERPRETER && !phdr_ok) ||
	    (role == ELF_LOAD_MAIN && interp_count != 0 && !phdr_ok) ||
	    (role == ELF_LOAD_MAIN &&
	     ((interp_count != 0 && dynamic_count != 1) ||
	      (interp_count == 0 && dynamic_count != 0))) ||
	    (role == ELF_LOAD_INTERPRETER &&
	     (interp_count != 0 || dynamic_count != 1)))
		goto invalid;
	if (position_independent &&
	    (interp_count != 1 ||
	     phdr_count != 1 ||
	     !phdr_ok ||
	     phdr_vaddr != image->program_headers))
		goto invalid;

	/* A fixed main program must fit the user range as it is. */
	if (role == ELF_LOAD_MAIN && !position_independent) {
		if (minimum < vm_layout.user_minimum ||
		    maximum > vm_layout.user_limit ||
		    header.entry < vm_layout.user_minimum ||
		    header.entry >= vm_layout.user_limit)
			goto invalid;
	} else {
		/* Anything else is placed at a load bias in free address space. */
		span = maximum - minimum;
		if (span == 0 ||
		    span > SIZE_MAX ||
		    minimum > UINTPTR_MAX ||
		    maximum_align > SIZE_MAX ||
		    maximum_align > (uint64_t)UINTPTR_MAX)
			goto invalid;
		if (maximum_align < PAGE_SIZE)
			maximum_align = PAGE_SIZE;
		if (position_independent)
			error = vmspace_find_free_range_bounded(vm,
			    vm_layout.user_minimum, vm_layout.brk_limit,
			    (size_t)span, (size_t)maximum_align, &base);
		else
			error = vmspace_find_free_range(vm, vm_layout.mmap_base,
			    (size_t)span, (size_t)maximum_align, &base);
		if (error != 0)
			goto out;
		if (base < (uintptr_t)minimum)
			goto invalid;
		load_bias = base - (uintptr_t)minimum;
		if (maximum > UINTPTR_MAX - load_bias ||
		    maximum + load_bias > vm_layout.user_limit ||
		    header.entry > UINTPTR_MAX - load_bias)
			goto invalid;
	}

	/* Linkers may emit an empty PT_TLS for a program without TLS symbols. */
	if (tls != NULL && tls->memsz == 0)
		tls = NULL;

	/* The template must describe the same bytes as a readable load segment. */
	if (tls != NULL) {
		if (tls->vaddr > UINTPTR_MAX - load_bias)
			goto invalid;
		start = tls->vaddr + load_bias;
		if (start < vm_layout.user_minimum)
			goto invalid;
		if (start >= vm_layout.user_limit)
			goto invalid;
		if (tls->memsz > vm_layout.user_limit - start)
			goto invalid;
		for (i = 0; i < header.phnum; i++) {
			segment = &programs[i];
			if (segment->type != PT_LOAD)
				continue;
			if ((segment->flags & PF_R) == 0)
				continue;
			if (tls->vaddr < segment->vaddr)
				continue;
			start = tls->vaddr - segment->vaddr;
			if (start > segment->filesz)
				continue;
			if (tls->filesz > segment->filesz - start)
				continue;
			if (tls->offset != segment->offset + start)
				continue;
			tls_contained = 1;
			break;
		}
		if (!tls_contained)
			goto invalid;
	}

	/* Maps and fills every PT_LOAD segment. */
	for (i = 0; i < header.phnum; i++) {
		segment = &programs[i];
		if (segment->type != PT_LOAD)
			continue;
		map_start = load_bias +
		    ((uintptr_t)segment->vaddr & ~(uintptr_t)(PAGE_SIZE - 1U));
		data_start = load_bias + (uintptr_t)segment->vaddr;
		map_end = load_bias + (uintptr_t)((segment->vaddr + segment->memsz +
		    PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U));

		/*
		 * The executable file may be modified as soon as its content
		 * lease is released.  Build anonymous private pages while the
		 * lease is held, rather than leaving PT_LOAD as a later file
		 * fault which could splice bytes from a newer image.  The
		 * temporary mapping is writable but never executable.  Immutable
		 * full text pages replace its interior with pinned cache mappings;
		 * final protection is published after private edges are copied.
		 */
		error = vmspace_map_anon_fixed_noreplace(vm, map_start, map_end - map_start,
		    HAL_SPACE_READ | HAL_SPACE_WRITE, NULL);
		if (error != 0)
			goto rollback;
		mapped_start[mapped_count] = map_start;
		mapped_size[mapped_count++] = map_end - map_start;
		error = load_segment_snapshot(lease, vm, data_start,
		    (off_t)segment->offset, (size_t)segment->filesz,
		    initial_segment_prot(segment));
		if (error != 0)
			goto rollback;
		error = vmspace_protect(vm, map_start, map_end - map_start,
		    initial_segment_prot(segment));
		if (error != 0)
			goto rollback;

		/* The heap of a main program starts after its highest segment. */
		if (role == ELF_LOAD_MAIN && map_end > image->brk_start)
			image->brk_start = map_end;
	}

	/* Describes the loaded image. */
	image->load_bias = load_bias;
	if (data_minimum != UINT64_MAX) {
		if (data_maximum < data_minimum ||
		    data_maximum - data_minimum > SIZE_MAX)
			goto rollback;
		image->static_data_size = (size_t)(data_maximum - data_minimum);
	}

	/* The runtime linker owns dynamic TLS; static images start with a ready TP. */
	if (tls != NULL && role == ELF_LOAD_MAIN && interp_count == 0) {
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_I386)
		error = load_static_tls(lease, vm, tls, &image->thread_pointer);
#else
		error = ENOEXEC;
#endif
		if (error != 0)
			goto rollback;
	}

	image->entry = load_bias + (uintptr_t)header.entry;
	image->program_headers += load_bias;
	image->program_header_size = header.phentsize;
	image->program_header_count = header.phnum;
	image->has_interpreter = interp_count != 0;
	vm->entry = image->entry;
	error = 0;
	goto out;

rollback:
	/* Unmaps whatever was mapped before the failure. */
	while (mapped_count != 0) {
		mapped_count--;
		(void)vmspace_unmap(vm, mapped_start[mapped_count],
		    mapped_size[mapped_count]);
	}

	goto out;
invalid:
	error = ENOEXEC;
out:
	kern_free(programs);

	/* Reports why the load failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Copies a normalized image description into the 32-bit record. */
static void
copy_image32(
	struct elf32_image_info *destination,
	const struct normalized_image *source)
{
	destination->thread_pointer = source->thread_pointer;
	destination->entry = source->entry;
	destination->brk_start = source->brk_start;
	destination->static_data_size = source->static_data_size;
	destination->program_headers = source->program_headers;
	destination->load_bias = source->load_bias;
	destination->stack_size = source->stack_size;
	destination->program_header_size = source->program_header_size;
	destination->program_header_count = source->program_header_count;
	destination->has_interpreter = source->has_interpreter;
	memcpy(destination->interpreter, source->interpreter,
	    sizeof(destination->interpreter));
}

/* Copies a normalized image description into the 64-bit record. */
static void
copy_image64(
	struct elf64_image_info *destination,
	const struct normalized_image *source)
{
	destination->thread_pointer = source->thread_pointer;
	destination->entry = source->entry;
	destination->brk_start = source->brk_start;
	destination->static_data_size = source->static_data_size;
	destination->program_headers = source->program_headers;
	destination->load_bias = source->load_bias;
	destination->stack_size = source->stack_size;
	destination->program_header_size = source->program_header_size;
	destination->program_header_count = source->program_header_count;
	destination->has_interpreter = source->has_interpreter;
	memcpy(destination->interpreter, source->interpreter,
	    sizeof(destination->interpreter));
}
