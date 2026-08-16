/*
 * ELF executable and interpreter loader
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/elf.h"
#include "kern/exec.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ELF_PHNUM_MAX 32U
#define PAGE_SIZE ZEDBSD_PAGE_SIZE
#ifdef ZEDBSD_USER_ABI_LP64
#define ELF_OFF_MAX 0x7fffffffffffffffULL
#else
#define ELF_OFF_MAX 0x7fffffffULL
#endif

#if defined(HAL_ARCH_SPARCV9)
#define ELF_EXPECTED_DATA ELFDATA2MSB
#define ELF64_EXPECTED_MACHINE EM_SPARCV9
#elif defined(HAL_ARCH_AMD64)
#define ELF_EXPECTED_DATA ELFDATA2LSB
#define ELF64_EXPECTED_MACHINE EM_X86_64
#elif defined(HAL_ARCH_ARM64)
#define ELF_EXPECTED_DATA ELFDATA2LSB
#define ELF64_EXPECTED_MACHINE EM_AARCH64
#elif defined(HAL_ARCH_I386)
#define ELF_EXPECTED_DATA ELFDATA2LSB
#define ELF64_EXPECTED_MACHINE EM_X86_64
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
	uintptr_t entry;
	uintptr_t brk_start;
	uintptr_t program_headers;
	uintptr_t load_bias;
	size_t stack_size;
	uint16_t program_header_size;
	uint16_t program_header_count;
	unsigned has_interpreter;
	char interpreter[EXEC_INTERP_MAX];
};

static int
read_exact(struct file *file, off_t offset, void *buffer, size_t size)
{
	ssize_t count;
	if (offset < 0 || file_seek(file, offset, 0) != offset)
		return EIO;
	count = file_read(file, buffer, size);
	return count == (ssize_t)size ? 0 : EIO;
}

static int
power_of_two64(uint64_t value)
{
	return value != 0 && (value & (value - 1U)) == 0;
}

static uint32_t
segment_prot(uint32_t flags)
{
	uint32_t prot = 0;
	if (flags & PF_R) prot |= HAL_SPACE_READ;
	if (flags & PF_W) prot |= HAL_SPACE_WRITE;
	if (flags & PF_X) prot |= HAL_SPACE_EXEC;
	return prot;
}

static int
read_headers(struct file *file, unsigned elf_class,
	struct normalized_header *header, struct normalized_program **programs_out,
	uint64_t *file_size_out)
{
	struct normalized_program *programs;
	uint64_t file_size;
	unsigned i;

	if (file == NULL || file->f_inode == NULL || header == NULL ||
	    programs_out == NULL || file_size_out == NULL ||
	    file->f_inode->i_size < 0)
		return EINVAL;
	file_size = (uint64_t)file->f_inode->i_size;
	if (file_size > ELF_OFF_MAX)
		return ENOEXEC;
	memset(header, 0, sizeof(*header));
	if (elf_class == ELFCLASS32) {
		struct elf32_ehdr raw;
		struct elf32_phdr *raw_programs;
		if (file_size < sizeof(raw) ||
		    read_exact(file, 0, &raw, sizeof(raw)) != 0)
			return ENOEXEC;
		if (raw.e_ident[EI_MAG0] != ELFMAG0 ||
		    raw.e_ident[EI_MAG1] != ELFMAG1 ||
		    raw.e_ident[EI_MAG2] != ELFMAG2 ||
		    raw.e_ident[EI_MAG3] != ELFMAG3 ||
		    raw.e_ident[EI_CLASS] != ELFCLASS32 ||
		    raw.e_ident[EI_DATA] != ELF_EXPECTED_DATA ||
		    raw.e_ident[EI_VERSION] != EV_CURRENT ||
		    raw.e_machine != EM_386 || raw.e_version != EV_CURRENT ||
		    raw.e_ehsize != sizeof(raw) ||
		    raw.e_phentsize != sizeof(struct elf32_phdr) ||
		    raw.e_phnum == 0 || raw.e_phnum > ELF_PHNUM_MAX ||
		    raw.e_phoff > file_size || raw.e_phnum >
		    (file_size - raw.e_phoff) / sizeof(struct elf32_phdr))
			return ENOEXEC;
		raw_programs = kern_malloc((size_t)raw.e_phnum * sizeof(*raw_programs));
		if (raw_programs == NULL)
			return ENOMEM;
		if (read_exact(file, (off_t)raw.e_phoff, raw_programs,
		    (size_t)raw.e_phnum * sizeof(*raw_programs)) != 0) {
			kern_free(raw_programs);
			return ENOEXEC;
		}
		programs = kern_calloc(raw.e_phnum, sizeof(*programs));
		if (programs == NULL) {
			kern_free(raw_programs);
			return ENOMEM;
		}
		for (i = 0; i < raw.e_phnum; i++) {
			programs[i].type = raw_programs[i].p_type;
			programs[i].flags = raw_programs[i].p_flags;
			programs[i].offset = raw_programs[i].p_offset;
			programs[i].vaddr = raw_programs[i].p_vaddr;
			programs[i].filesz = raw_programs[i].p_filesz;
			programs[i].memsz = raw_programs[i].p_memsz;
			programs[i].align = raw_programs[i].p_align;
		}
		kern_free(raw_programs);
		header->type = raw.e_type;
		header->machine = raw.e_machine;
		header->entry = raw.e_entry;
		header->phoff = raw.e_phoff;
		header->phentsize = raw.e_phentsize;
		header->phnum = raw.e_phnum;
	} else {
		struct elf64_ehdr raw;
		struct elf64_phdr *raw_programs;
		if (file_size < sizeof(raw) ||
		    read_exact(file, 0, &raw, sizeof(raw)) != 0)
			return ENOEXEC;
		if (raw.e_ident[EI_MAG0] != ELFMAG0 ||
		    raw.e_ident[EI_MAG1] != ELFMAG1 ||
		    raw.e_ident[EI_MAG2] != ELFMAG2 ||
		    raw.e_ident[EI_MAG3] != ELFMAG3 ||
		    raw.e_ident[EI_CLASS] != ELFCLASS64 ||
		    raw.e_ident[EI_DATA] != ELF_EXPECTED_DATA ||
		    raw.e_ident[EI_VERSION] != EV_CURRENT ||
		    raw.e_machine != ELF64_EXPECTED_MACHINE ||
		    raw.e_version != EV_CURRENT || raw.e_ehsize != sizeof(raw) ||
		    raw.e_phentsize != sizeof(struct elf64_phdr) ||
		    raw.e_phnum == 0 || raw.e_phnum > ELF_PHNUM_MAX ||
		    raw.e_phoff > file_size || raw.e_phnum >
		    (file_size - raw.e_phoff) / sizeof(struct elf64_phdr))
			return ENOEXEC;
		raw_programs = kern_malloc((size_t)raw.e_phnum * sizeof(*raw_programs));
		if (raw_programs == NULL)
			return ENOMEM;
		if (read_exact(file, (off_t)raw.e_phoff, raw_programs,
		    (size_t)raw.e_phnum * sizeof(*raw_programs)) != 0) {
			kern_free(raw_programs);
			return ENOEXEC;
		}
		programs = kern_calloc(raw.e_phnum, sizeof(*programs));
		if (programs == NULL) {
			kern_free(raw_programs);
			return ENOMEM;
		}
		for (i = 0; i < raw.e_phnum; i++) {
			programs[i].type = raw_programs[i].p_type;
			programs[i].flags = raw_programs[i].p_flags;
			programs[i].offset = raw_programs[i].p_offset;
			programs[i].vaddr = raw_programs[i].p_vaddr;
			programs[i].filesz = raw_programs[i].p_filesz;
			programs[i].memsz = raw_programs[i].p_memsz;
			programs[i].align = raw_programs[i].p_align;
		}
		kern_free(raw_programs);
		header->type = raw.e_type;
		header->machine = raw.e_machine;
		header->entry = raw.e_entry;
		header->phoff = raw.e_phoff;
		header->phentsize = raw.e_phentsize;
		header->phnum = raw.e_phnum;
	}
	header->elf_class = elf_class;
	*programs_out = programs;
	*file_size_out = file_size;
	return 0;
}

static int
validate_and_load(struct file *file, struct vmspace *vm, unsigned elf_class,
	 enum elf_load_role role, struct normalized_image *image)
{
	struct normalized_header header;
	struct normalized_program *programs = NULL;
	uintptr_t mapped_start[ELF_PHNUM_MAX];
	size_t mapped_size[ELF_PHNUM_MAX];
	uint64_t file_size, minimum = UINT64_MAX, maximum = 0, maximum_align = PAGE_SIZE;
	uint64_t phdr_file_end;
	uintptr_t load_bias = 0;
	unsigned i, j, mapped_count = 0, loads = 0, dynamic_count = 0;
	unsigned interp_count = 0, stack_count = 0;
	int entry_ok = 0, phdr_ok = 0;
	int error;

	if (file == NULL || vm == NULL || image == NULL)
		return EINVAL;
	vmspace_layout_init();
	memset(image, 0, sizeof(*image));
	image->stack_size = EXEC_STACK_DEFAULT_SIZE;
	error = read_headers(file, elf_class, &header, &programs, &file_size);
	if (error != 0)
		return error;
	if ((role == ELF_LOAD_MAIN && header.type != ET_EXEC) ||
	    (role == ELF_LOAD_INTERPRETER && header.type != ET_DYN))
		goto invalid;
	if (header.phnum > UINT64_MAX / header.phentsize)
		goto invalid;
	phdr_file_end = header.phoff +
	    (uint64_t)header.phnum * header.phentsize;
	if (phdr_file_end < header.phoff || phdr_file_end > file_size)
		goto invalid;

	for (i = 0; i < header.phnum; i++) {
		struct normalized_program *program = &programs[i];
		uint64_t start, end;

		if (program->type == PT_INTERP) {
			if (role != ELF_LOAD_MAIN || ++interp_count != 1 ||
			    program->filesz < 2 || program->filesz > EXEC_INTERP_MAX ||
			    program->offset > file_size ||
			    program->filesz > file_size - program->offset ||
			    read_exact(file, (off_t)program->offset, image->interpreter,
			    (size_t)program->filesz) != 0 ||
			    image->interpreter[program->filesz - 1U] != '\0' ||
			    strlen(image->interpreter) + 1U != program->filesz ||
			    strcmp(image->interpreter, EXEC_INTERP_PATH) != 0)
				goto invalid;
			continue;
		}
		if (program->type == PT_DYNAMIC) {
			if (++dynamic_count != 1)
				goto invalid;
			continue;
		}
		if (program->type == PT_GNU_STACK) {
			uint64_t requested = program->memsz;
			if (++stack_count != 1 || program->filesz != 0 ||
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
		if (program->type != PT_LOAD)
			continue;
		loads++;
		if (program->memsz == 0 || program->filesz > program->memsz ||
		    (program->flags & ~(PF_R | PF_W | PF_X)) != 0 ||
		    (program->flags & (PF_W | PF_X)) == (PF_W | PF_X) ||
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
		for (j = 0; j < i; j++) {
			uint64_t other_start, other_end;
			struct normalized_program *other = &programs[j];
			if (other->type != PT_LOAD)
				continue;
			other_start = other->vaddr & ~(uint64_t)(PAGE_SIZE - 1U);
			other_end = (other->vaddr + other->memsz + PAGE_SIZE - 1U) &
			    ~(uint64_t)(PAGE_SIZE - 1U);
			if (start < other_end && other_start < end)
				goto invalid;
		}
		if ((program->flags & PF_X) && header.entry >= program->vaddr &&
		    header.entry < program->vaddr + program->memsz)
			entry_ok = 1;
		if (header.phoff >= program->offset && phdr_file_end >= header.phoff &&
		    phdr_file_end <= program->offset + program->filesz) {
			image->program_headers = (uintptr_t)(program->vaddr +
			    (header.phoff - program->offset));
			phdr_ok = 1;
		}
	}
	if (loads == 0 || !entry_ok ||
	    (role == ELF_LOAD_INTERPRETER && !phdr_ok) ||
	    (role == ELF_LOAD_MAIN && interp_count != 0 && !phdr_ok) ||
	    (role == ELF_LOAD_MAIN &&
	    ((interp_count != 0 && dynamic_count != 1) ||
	    (interp_count == 0 && dynamic_count != 0))) ||
	    (role == ELF_LOAD_INTERPRETER &&
	    (interp_count != 0 || dynamic_count != 1)))
		goto invalid;
	if (role == ELF_LOAD_MAIN) {
		if (minimum < vm_layout.user_minimum || maximum > vm_layout.user_limit ||
		    header.entry < vm_layout.user_minimum ||
		    header.entry >= vm_layout.user_limit)
			goto invalid;
	} else {
		uintptr_t base;
		uint64_t span = maximum - minimum;
		if (span == 0 || span > SIZE_MAX || minimum > UINTPTR_MAX ||
		    maximum_align > SIZE_MAX || maximum_align > (uint64_t)UINTPTR_MAX)
			goto invalid;
		if (maximum_align < PAGE_SIZE)
			maximum_align = PAGE_SIZE;
		error = vmspace_find_free_range(vm, vm_layout.mmap_base, (size_t)span,
		    (size_t)maximum_align, &base);
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

	for (i = 0; i < header.phnum; i++) {
		struct normalized_program *program = &programs[i];
		struct vm_region *region;
		uintptr_t start, data_start, end;
		if (program->type != PT_LOAD)
			continue;
		start = load_bias +
		    ((uintptr_t)program->vaddr & ~(uintptr_t)(PAGE_SIZE - 1U));
		data_start = load_bias + (uintptr_t)program->vaddr;
		end = load_bias + (uintptr_t)((program->vaddr + program->memsz +
		    PAGE_SIZE - 1U) & ~(uint64_t)(PAGE_SIZE - 1U));
		error = vmspace_map_file(vm, start, end - start,
		    segment_prot(program->flags), file, (off_t)program->offset,
		    data_start, (size_t)program->filesz, &region);
		if (error != 0)
			goto rollback;
		region->flags |= VM_REGION_ELF_ZERO_TAIL;
		mapped_start[mapped_count] = start;
		mapped_size[mapped_count++] = end - start;
		if (role == ELF_LOAD_MAIN && end > image->brk_start)
			image->brk_start = end;
	}
	image->load_bias = load_bias;
	image->entry = load_bias + (uintptr_t)header.entry;
	image->program_headers += load_bias;
	image->program_header_size = header.phentsize;
	image->program_header_count = header.phnum;
	image->has_interpreter = interp_count != 0;
	vm->entry = image->entry;
	error = 0;
	goto out;

rollback:
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
	return error;
}

static void
copy_image32(struct elf32_image_info *destination,
	const struct normalized_image *source)
{
	destination->entry = source->entry;
	destination->brk_start = source->brk_start;
	destination->program_headers = source->program_headers;
	destination->load_bias = source->load_bias;
	destination->stack_size = source->stack_size;
	destination->program_header_size = source->program_header_size;
	destination->program_header_count = source->program_header_count;
	destination->has_interpreter = source->has_interpreter;
	memcpy(destination->interpreter, source->interpreter,
	    sizeof(destination->interpreter));
}

static void
copy_image64(struct elf64_image_info *destination,
	const struct normalized_image *source)
{
	destination->entry = source->entry;
	destination->brk_start = source->brk_start;
	destination->program_headers = source->program_headers;
	destination->load_bias = source->load_bias;
	destination->stack_size = source->stack_size;
	destination->program_header_size = source->program_header_size;
	destination->program_header_count = source->program_header_count;
	destination->has_interpreter = source->has_interpreter;
	memcpy(destination->interpreter, source->interpreter,
	    sizeof(destination->interpreter));
}

int
elf32_load(struct file *file, struct vmspace *vm,
	struct elf32_image_info *image)
{
	struct normalized_image normalized;
	int error;
	if (image == NULL)
		return EINVAL;
	error = validate_and_load(file, vm, ELFCLASS32, ELF_LOAD_MAIN,
	    &normalized);
	if (error == 0)
		copy_image32(image, &normalized);
	return error;
}

int
elf32_load_interpreter(struct file *file, struct vmspace *vm,
	struct elf32_image_info *image)
{
	struct normalized_image normalized;
	int error;
	if (image == NULL)
		return EINVAL;
	error = validate_and_load(file, vm, ELFCLASS32, ELF_LOAD_INTERPRETER,
	    &normalized);
	if (error == 0)
		copy_image32(image, &normalized);
	return error;
}

int
elf64_load(struct file *file, struct vmspace *vm,
	struct elf64_image_info *image)
{
	struct normalized_image normalized;
	int error;
	if (image == NULL)
		return EINVAL;
	error = validate_and_load(file, vm, ELFCLASS64, ELF_LOAD_MAIN,
	    &normalized);
	if (error == 0)
		copy_image64(image, &normalized);
	return error;
}

int
elf64_load_interpreter(struct file *file, struct vmspace *vm,
	struct elf64_image_info *image)
{
	struct normalized_image normalized;
	int error;
	if (image == NULL)
		return EINVAL;
	error = validate_and_load(file, vm, ELFCLASS64, ELF_LOAD_INTERPRETER,
	    &normalized);
	if (error == 0)
		copy_image64(image, &normalized);
	return error;
}
