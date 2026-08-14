/* Restricted ELF64 validation shared by the UEFI loader. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "elf64.h"

#define PT_LOAD 1U
#define PF_X    1U
#define EM_X86_64 62U
#define ET_EXEC 2U
#define AMD64_DIRECT_BASE 0xffffffff80000000ULL
#define KERNEL_PHYS_START 0x00200000ULL
#define KERNEL_PHYS_LIMIT 0x01200000ULL

struct elf64_header {
	uint8_t ident[16];
	uint16_t type;
	uint16_t machine;
	uint32_t version;
	uint64_t entry;
	uint64_t phoff;
	uint64_t shoff;
	uint32_t flags;
	uint16_t ehsize;
	uint16_t phentsize;
	uint16_t phnum;
	uint16_t shentsize;
	uint16_t shnum;
	uint16_t shstrndx;
} __attribute__((packed));

struct elf64_program_header {
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t paddr;
	uint64_t filesz;
	uint64_t memsz;
	uint64_t align;
} __attribute__((packed));

int
zbl_elf64_plan(const void *buffer, uint64_t file_size_limit,
	struct zbl_elf64_plan *plan)
{
	const struct elf64_header *header = buffer;
	const uint8_t *bytes = buffer;
	uint64_t previous_end = 0;
	uint16_t index;
	int entry_is_executable = 0;

	if (buffer == 0 || plan == 0 || header->ident[0] != 0x7f ||
	    header->ident[1] != 'E' || header->ident[2] != 'L' ||
	    header->ident[3] != 'F' || header->ident[4] != 2 ||
	    header->ident[5] != 1 || header->ident[6] != 1 ||
	    header->type != ET_EXEC || header->machine != EM_X86_64 ||
	    header->version != 1 || header->ehsize != sizeof(*header) ||
	    header->phentsize != sizeof(struct elf64_program_header) ||
	    header->phnum == 0 || header->phnum > ZBL_ELF_MAX_SEGMENTS ||
	    header->phoff > ZBL_ELF_HEADER_BYTES ||
	    (uint64_t)header->phnum * header->phentsize >
	    ZBL_ELF_HEADER_BYTES - header->phoff)
		return 0;
	plan->entry = header->entry;
	plan->physical_start = UINT64_MAX;
	plan->physical_end = 0;
	plan->segment_count = 0;
	for (index = 0; index < header->phnum; index++) {
		const struct elf64_program_header *ph =
		    (const void *)(bytes + header->phoff +
		    (uint64_t)index * header->phentsize);
		struct zbl_elf64_segment *segment;
		uint64_t end;

		if (ph->type != PT_LOAD)
			continue;
		if (plan->segment_count == ZBL_ELF_MAX_SEGMENTS ||
		    ph->filesz > ph->memsz || ph->memsz == 0 ||
		    ph->paddr < KERNEL_PHYS_START ||
		    ph->paddr >= KERNEL_PHYS_LIMIT ||
		    ph->memsz > KERNEL_PHYS_LIMIT - ph->paddr ||
		    ph->vaddr != AMD64_DIRECT_BASE + ph->paddr ||
		    (ph->paddr & 0xfffU) != 0 || ph->align < 0x1000U ||
		    (ph->align & (ph->align - 1U)) != 0 ||
		    ph->offset > file_size_limit ||
		    ph->filesz > file_size_limit - ph->offset)
			return 0;
		end = ph->paddr + ph->memsz;
		if (previous_end != 0 && ph->paddr < previous_end)
			return 0;
		previous_end = end;
		segment = &plan->segment[plan->segment_count++];
		segment->offset = ph->offset;
		segment->physical = ph->paddr;
		segment->virtual_address = ph->vaddr;
		segment->file_size = ph->filesz;
		segment->memory_size = ph->memsz;
		segment->flags = ph->flags;
		if (ph->paddr < plan->physical_start)
			plan->physical_start = ph->paddr;
		if (end > plan->physical_end)
			plan->physical_end = end;
		if ((ph->flags & PF_X) != 0 && header->entry >= ph->vaddr &&
		    header->entry < ph->vaddr + ph->memsz)
			entry_is_executable = 1;
	}
	return plan->segment_count != 0 && entry_is_executable &&
	    plan->physical_start == KERNEL_PHYS_START;
}
