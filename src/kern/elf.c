/*
 * Copyright (C) 2026 Awe Morris
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
#define ELF_OFF_MAX 0x7fffffffULL

#ifdef ZEDBSD_USER_ABI_SPARCV9
#define ELF64_EXPECTED_DATA ELFDATA2MSB
#define ELF64_EXPECTED_MACHINE EM_SPARCV9
#else
#define ELF64_EXPECTED_DATA ELFDATA2LSB
#define ELF64_EXPECTED_MACHINE EM_AARCH64
#endif

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

int
elf32_load(struct file *file, struct vmspace *vm,
	   struct elf32_image_info *image)
{
	struct elf32_ehdr header;
	struct elf32_phdr *programs = NULL;
	off_t file_size;
	unsigned i, j;
	int error = ENOEXEC;
	int loads = 0, entry_ok = 0;
	unsigned stack_headers = 0;
	size_t stack_size = EXEC_STACK_DEFAULT_SIZE;
	uint32_t brk_start = 0;

	if (file == NULL || vm == NULL || image == NULL)
		return EINVAL;
	memset(image, 0, sizeof(*image));
	file_size = file->f_inode->i_size;
	if (file_size < (off_t)sizeof(header) ||
	    read_exact(file, 0, &header, sizeof(header)) != 0)
		return ENOEXEC;
	if (header.e_ident[EI_MAG0] != ELFMAG0 ||
	    header.e_ident[EI_MAG1] != ELFMAG1 ||
	    header.e_ident[EI_MAG2] != ELFMAG2 ||
	    header.e_ident[EI_MAG3] != ELFMAG3 ||
	    header.e_ident[EI_CLASS] != ELFCLASS32 ||
	    header.e_ident[EI_DATA] != ELF64_EXPECTED_DATA ||
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
		if (program->p_type == PT_GNU_STACK) {
			uint32_t requested = program->p_memsz;
			if (++stack_headers != 1 || program->p_filesz != 0 ||
			    (program->p_flags & ~(PF_R | PF_W | PF_X)) != 0 ||
			    (program->p_flags & (PF_R | PF_W)) != (PF_R | PF_W) ||
			    (program->p_flags & PF_X) != 0)
				goto out;
			if (requested == 0) {
				stack_size = EXEC_STACK_DEFAULT_SIZE;
				continue;
			}
			if (requested > EXEC_STACK_HARD_MAX)
				goto out;
			stack_size = (requested + PAGE_SIZE - 1U) &
				~(size_t)(PAGE_SIZE - 1U);
			if (stack_size == 0 || stack_size > EXEC_STACK_HARD_MAX)
				goto out;
			continue;
		}
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
		if (end > brk_start)
			brk_start = end;
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
		uint32_t start, end;
		if (program->p_type != PT_LOAD)
			continue;
		start = program->p_vaddr & ~(PAGE_SIZE - 1U);
		end = (program->p_vaddr + program->p_memsz + PAGE_SIZE - 1U) &
			~(PAGE_SIZE - 1U);
		error = vmspace_map_file(vm, start, end - start,
			segment_prot(program->p_flags), file,
			(off_t)program->p_offset, program->p_vaddr,
			program->p_filesz, NULL);
		if (error != 0)
			goto out;
	}
	vm->entry = header.e_entry;
	image->entry = header.e_entry;
	image->brk_start = brk_start;
	image->stack_size = stack_size;
	error = 0;
out:
	kern_free(programs);
	return error;
}

int
elf64_load(struct file *file, struct vmspace *vm,
	   struct elf64_image_info *image)
{
	struct elf64_ehdr header;
	struct elf64_phdr *programs = NULL;
	uint64_t file_size;
	unsigned i, j;
	int error = ENOEXEC;
	int loads = 0, entry_ok = 0;
	unsigned stack_headers = 0;
	size_t stack_size = EXEC_STACK_DEFAULT_SIZE;
	uintptr_t brk_start = 0;

	if (file == NULL || vm == NULL || image == NULL)
		return EINVAL;
	memset(image, 0, sizeof(*image));
	file_size = (uint64_t)file->f_inode->i_size;
	if (file_size < sizeof(header) || file_size > ELF_OFF_MAX ||
	    read_exact(file, 0, &header, sizeof(header)) != 0)
		return ENOEXEC;
	if (header.e_ident[EI_MAG0] != ELFMAG0 ||
	    header.e_ident[EI_MAG1] != ELFMAG1 ||
	    header.e_ident[EI_MAG2] != ELFMAG2 ||
	    header.e_ident[EI_MAG3] != ELFMAG3 ||
	    header.e_ident[EI_CLASS] != ELFCLASS64 ||
	    header.e_ident[EI_DATA] != ELF64_EXPECTED_DATA ||
	    header.e_ident[EI_VERSION] != EV_CURRENT ||
	    header.e_type != ET_EXEC ||
	    header.e_machine != ELF64_EXPECTED_MACHINE ||
	    header.e_version != EV_CURRENT || header.e_ehsize != sizeof(header) ||
	    header.e_phentsize != sizeof(struct elf64_phdr) ||
	    header.e_phnum == 0 || header.e_phnum > ELF_PHNUM_MAX)
		return ENOEXEC;
	if (header.e_phoff > file_size || header.e_phnum >
	    (file_size - header.e_phoff) / sizeof(*programs))
		return ENOEXEC;
	programs = kern_malloc((size_t)header.e_phnum * sizeof(*programs));
	if (programs == NULL)
		return ENOMEM;
	if (header.e_phoff > ELF_OFF_MAX ||
	    read_exact(file, (off_t)header.e_phoff, programs,
		       (size_t)header.e_phnum * sizeof(*programs)) != 0)
		goto out;

	for (i = 0; i < header.e_phnum; i++) {
		struct elf64_phdr *program = &programs[i];
		uintptr_t start, end;

		if (program->p_type == PT_INTERP || program->p_type == PT_DYNAMIC)
			goto out;
		if (program->p_type == PT_GNU_STACK) {
			uint64_t requested = program->p_memsz;
			if (++stack_headers != 1 || program->p_filesz != 0 ||
			    (program->p_flags & ~(PF_R | PF_W | PF_X)) != 0 ||
			    (program->p_flags & (PF_R | PF_W)) != (PF_R | PF_W) ||
			    (program->p_flags & PF_X) != 0 ||
			    requested > EXEC_STACK_HARD_MAX)
				goto out;
			if (requested != 0) {
				stack_size = (size_t)((requested + PAGE_SIZE - 1U) &
					~(uint64_t)(PAGE_SIZE - 1U));
				if (stack_size == 0 || stack_size > EXEC_STACK_HARD_MAX)
					goto out;
			}
			continue;
		}
		if (program->p_type != PT_LOAD)
			continue;
		loads++;
		if (program->p_memsz == 0 || program->p_filesz > program->p_memsz ||
		    segment_prot(program->p_flags) == 0 ||
		    program->p_offset > file_size ||
		    program->p_filesz > file_size - program->p_offset ||
		    program->p_vaddr < VM_USER_MIN || program->p_vaddr >= VM_USER_TOP ||
		    program->p_memsz > (uint64_t)VM_USER_TOP - program->p_vaddr ||
		    ((program->p_offset ^ program->p_vaddr) & (PAGE_SIZE - 1U)) != 0 ||
		    (program->p_align > 1U &&
		     (!power_of_two64(program->p_align) ||
		      ((program->p_offset ^ program->p_vaddr) &
		       (program->p_align - 1U)) != 0)) ||
		    program->p_offset > ELF_OFF_MAX)
			goto out;
		start = (uintptr_t)program->p_vaddr & ~(uintptr_t)(PAGE_SIZE - 1U);
		end = ((uintptr_t)program->p_vaddr + (uintptr_t)program->p_memsz +
		       PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
		if (end <= start || end > VM_USER_TOP)
			goto out;
		if (end > brk_start)
			brk_start = end;
		for (j = 0; j < i; j++) {
			struct elf64_phdr *other = &programs[j];
			uintptr_t other_start, other_end;
			if (other->p_type != PT_LOAD)
				continue;
			other_start = (uintptr_t)other->p_vaddr &
				~(uintptr_t)(PAGE_SIZE - 1U);
			other_end = ((uintptr_t)other->p_vaddr +
				(uintptr_t)other->p_memsz + PAGE_SIZE - 1U) &
				~(uintptr_t)(PAGE_SIZE - 1U);
			if (start < other_end && other_start < end)
				goto out;
		}
		if ((program->p_flags & PF_X) &&
		    header.e_entry >= program->p_vaddr &&
		    header.e_entry < program->p_vaddr + program->p_memsz)
			entry_ok = 1;
	}
	if (loads == 0 || !entry_ok || header.e_entry < VM_USER_MIN ||
	    header.e_entry >= VM_USER_TOP)
		goto out;

	for (i = 0; i < header.e_phnum; i++) {
		struct elf64_phdr *program = &programs[i];
		uintptr_t start, end;
		if (program->p_type != PT_LOAD)
			continue;
		start = (uintptr_t)program->p_vaddr & ~(uintptr_t)(PAGE_SIZE - 1U);
		end = ((uintptr_t)program->p_vaddr + (uintptr_t)program->p_memsz +
		       PAGE_SIZE - 1U) & ~(uintptr_t)(PAGE_SIZE - 1U);
		error = vmspace_map_file(vm, start, end - start,
			segment_prot(program->p_flags), file,
			(off_t)program->p_offset, (uintptr_t)program->p_vaddr,
			(size_t)program->p_filesz, NULL);
		if (error != 0)
			goto out;
	}
	vm->entry = (uintptr_t)header.e_entry;
	image->entry = (uintptr_t)header.e_entry;
	image->brk_start = brk_start;
	image->stack_size = stack_size;
	error = 0;
out:
	kern_free(programs);
	return error;
}
