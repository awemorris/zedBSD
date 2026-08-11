/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/elf.h"
#include "kern/exec.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ELF_PHNUM_MAX 32U
#define PAGE_SIZE 4096U

static int
read_exact(struct file *file, off_t offset, void *buffer, size_t size)
{
	ssize_t count;
	if (file_seek(file, offset, 0) != offset)
		return EIO;
	count = file_read(file, buffer, size);
	return count == (ssize_t)size ? 0 : EIO;
}

static int
power_of_two(uint32_t value)
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

int
elf32_load(struct file *file, struct vmspace *vm, uintptr_t *entry)
{
	struct elf32_ehdr header;
	struct elf32_phdr *programs = NULL;
	off_t file_size;
	unsigned i, j;
	int error = ENOEXEC;
	int loads = 0, entry_ok = 0;

	if (file == NULL || vm == NULL || entry == NULL)
		return EINVAL;
	file_size = file->f_inode->i_size;
	if (file_size < (off_t)sizeof(header) ||
	    read_exact(file, 0, &header, sizeof(header)) != 0)
		return ENOEXEC;
	if (header.e_ident[EI_MAG0] != ELFMAG0 ||
	    header.e_ident[EI_MAG1] != ELFMAG1 ||
	    header.e_ident[EI_MAG2] != ELFMAG2 ||
	    header.e_ident[EI_MAG3] != ELFMAG3 ||
	    header.e_ident[EI_CLASS] != ELFCLASS32 ||
	    header.e_ident[EI_DATA] != ELFDATA2LSB ||
	    header.e_ident[EI_VERSION] != EV_CURRENT ||
	    header.e_type != ET_EXEC || header.e_machine != EM_386 ||
	    header.e_version != EV_CURRENT || header.e_ehsize != sizeof(header) ||
	    header.e_phentsize != sizeof(struct elf32_phdr) ||
	    header.e_phnum == 0 || header.e_phnum > ELF_PHNUM_MAX)
		return ENOEXEC;
	if (header.e_phoff > (uint32_t)file_size ||
	    (uint32_t)header.e_phnum >
	    ((uint32_t)file_size - header.e_phoff) / sizeof(*programs))
		return ENOEXEC;
	programs = kern_malloc((size_t)header.e_phnum * sizeof(*programs));
	if (programs == NULL)
		return ENOMEM;
	if (read_exact(file, (off_t)header.e_phoff, programs,
		       (size_t)header.e_phnum * sizeof(*programs)) != 0)
		goto out;

	for (i = 0; i < header.e_phnum; i++) {
		struct elf32_phdr *program = &programs[i];
		uint32_t start, end;
		if (program->p_type == PT_INTERP || program->p_type == PT_DYNAMIC)
			goto out;
		if (program->p_type != PT_LOAD)
			continue;
		loads++;
		if (program->p_memsz == 0 || program->p_filesz > program->p_memsz ||
		    segment_prot(program->p_flags) == 0 ||
		    program->p_offset > (uint32_t)file_size ||
		    program->p_filesz > (uint32_t)file_size - program->p_offset ||
		    program->p_vaddr < VM_USER_MIN ||
		    program->p_memsz > VM_USER_TOP - program->p_vaddr ||
		    ((program->p_offset ^ program->p_vaddr) & (PAGE_SIZE - 1U)) != 0 ||
		    (program->p_align > 1U &&
		     (!power_of_two(program->p_align) ||
		      ((program->p_offset ^ program->p_vaddr) &
		       (program->p_align - 1U)) != 0)))
			goto out;
		start = program->p_vaddr & ~(PAGE_SIZE - 1U);
		end = (program->p_vaddr + program->p_memsz + PAGE_SIZE - 1U) &
			~(PAGE_SIZE - 1U);
		if (end <= start || end > VM_USER_TOP)
			goto out;
		for (j = 0; j < i; j++) {
			struct elf32_phdr *other = &programs[j];
			uint32_t other_start, other_end;
			if (other->p_type != PT_LOAD)
				continue;
			other_start = other->p_vaddr & ~(PAGE_SIZE - 1U);
			other_end = (other->p_vaddr + other->p_memsz +
				     PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);
			if (start < other_end && other_start < end)
				goto out;
		}
		if ((program->p_flags & PF_X) && header.e_entry >= program->p_vaddr &&
		    header.e_entry < program->p_vaddr + program->p_memsz)
			entry_ok = 1;
	}
	if (loads == 0 || !entry_ok || header.e_entry < VM_USER_MIN ||
	    header.e_entry >= VM_USER_TOP)
		goto out;

	for (i = 0; i < header.e_phnum; i++) {
		struct elf32_phdr *program = &programs[i];
		struct vm_region *region;
		uint32_t start, end;
		uint8_t *destination;
		if (program->p_type != PT_LOAD)
			continue;
		start = program->p_vaddr & ~(PAGE_SIZE - 1U);
		end = (program->p_vaddr + program->p_memsz + PAGE_SIZE - 1U) &
			~(PAGE_SIZE - 1U);
		error = vmspace_map_anon(vm, start, end - start,
					 segment_prot(program->p_flags), &region);
		if (error != 0)
			goto out;
		destination = (uint8_t *)region->pmem.vaddr +
			(program->p_vaddr - start);
		if (program->p_filesz != 0 &&
		    read_exact(file, (off_t)program->p_offset, destination,
			       program->p_filesz) != 0) {
			error = EIO;
			goto out;
		}
	}
	vm->entry = header.e_entry;
	*entry = header.e_entry;
	error = 0;
out:
	kern_free(programs);
	return error;
}
