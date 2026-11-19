/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's ELF, checked and described for the handoff.
 *
 * The kernel is a big-endian 32-bit m68k executable whose loadable
 * segments run at their physical addresses.  The fields are read byte by
 * byte, so that the check also runs on a little-endian host.
 */

#include "bootloader/mac68k/stage2.h"
#include "kern/elf.h"

/* The offsets of the ELF header's and the program header's fields. */
#define EHDR_TYPE		16U
#define EHDR_MACHINE		18U
#define EHDR_ENTRY		24U
#define EHDR_PHOFF		28U
#define EHDR_PHENTSIZE		42U
#define EHDR_PHNUM		44U
#define EHDR_SIZE		52U

#define PHDR_TYPE		0U
#define PHDR_OFFSET		4U
#define PHDR_PADDR		12U
#define PHDR_FILESZ		16U
#define PHDR_MEMSZ		20U
#define PHDR_SIZE		32U

static const char *check_segment(const struct mac68k_segment *segment, uint32_t length);

/*
 * Checks a kernel file and describes its loadable segments.  Reports
 * NULL, or why the file is not a kernel the loader can place.
 */
const char *
mac68k_kernel_parse(
	const uint8_t *file,
	uint32_t length,
	struct mac68k_kernel_image *image)
{
	struct mac68k_segment *segment;
	const uint8_t *header;
	const char *problem;
	uint32_t program_offset;
	uint32_t segment_type;
	uint16_t file_type;
	uint16_t machine;
	uint32_t entry_size;
	uint32_t entry_count;
	uint32_t index;
	uint32_t end;

	/* Starts with no segment. */
	mac68k_fill_bytes(image, 0, sizeof(*image));

	/* Refuses a file shorter than the ELF header. */
	if (length < EHDR_SIZE)
		return "the kernel is shorter than an ELF header";

	/* Refuses anything but an ELF file. */
	if (file[EI_MAG0] != ELFMAG0 ||
	    file[EI_MAG1] != ELFMAG1 ||
	    file[EI_MAG2] != ELFMAG2 ||
	    file[EI_MAG3] != ELFMAG3)
		return "the kernel is not an ELF file";

	/* Refuses another class or byte order. */
	if (file[EI_CLASS] != ELFCLASS32)
		return "the kernel is not a 32-bit ELF file";
	if (file[EI_DATA] != ELFDATA2MSB)
		return "the kernel is not big-endian";

	/* Refuses another type or processor. */
	file_type = mac68k_read_be16(file + EHDR_TYPE);
	machine = mac68k_read_be16(file + EHDR_MACHINE);
	if (file_type != ET_EXEC)
		return "the kernel is not an executable";
	if (machine != EM_68K)
		return "the kernel is not for the m68k";

	/* Finds the program headers, which must lie in the file. */
	program_offset = mac68k_read_be32(file + EHDR_PHOFF);
	entry_size = mac68k_read_be16(file + EHDR_PHENTSIZE);
	entry_count = mac68k_read_be16(file + EHDR_PHNUM);
	if (entry_size < PHDR_SIZE)
		return "the kernel's program headers are too short";
	if (program_offset > length)
		return "the kernel's program headers lie past its end";
	if (entry_count > (length - program_offset) / entry_size)
		return "the kernel's program headers lie past its end";

	/* Describes each loadable segment. */
	image->lowest = 0xffffffffU;
	for (index = 0; index < entry_count; index++) {
		header = file + program_offset + index * entry_size;
		segment_type = mac68k_read_be32(header + PHDR_TYPE);
		if (segment_type != PT_LOAD)
			continue;

		/* Refuses more segments than the loader keeps. */
		if (image->segment_count >= MAC68K_SEGMENTS_MAX)
			return "the kernel has too many segments";

		/* Records the segment and checks it. */
		segment = &image->segments[image->segment_count];
		segment->file_offset = mac68k_read_be32(header + PHDR_OFFSET);
		segment->physical = mac68k_read_be32(header + PHDR_PADDR);
		segment->file_size = mac68k_read_be32(header + PHDR_FILESZ);
		segment->memory_size = mac68k_read_be32(header + PHDR_MEMSZ);
		problem = check_segment(segment, length);
		if (problem != NULL)
			return problem;
		image->segment_count++;

		/* Follows the lowest start and the highest end of the image. */
		end = segment->physical + segment->memory_size;
		if (segment->physical < image->lowest)
			image->lowest = segment->physical;
		if (end > image->end)
			image->end = end;
	}

	/* Refuses a kernel with nothing to load. */
	if (image->segment_count == 0U)
		return "the kernel has no loadable segment";

	/* Succeeded: the entry and the segments. */
	image->entry = mac68k_read_be32(file + EHDR_ENTRY);
	return NULL;
}

/* Checks that a segment's bytes lie in the file and its addresses do not wrap. */
static const char *
check_segment(
	const struct mac68k_segment *segment,
	uint32_t length)
{
	/* The file bytes lie inside the file. */
	if (segment->file_offset > length)
		return "a kernel segment lies past the file's end";
	if (segment->file_size > length - segment->file_offset)
		return "a kernel segment lies past the file's end";

	/* The memory holds at least the file bytes. */
	if (segment->memory_size < segment->file_size)
		return "a kernel segment is smaller in memory than in the file";

	/* The segment does not wrap past the top of the address space. */
	if (segment->physical + segment->memory_size < segment->physical)
		return "a kernel segment wraps the address space";

	/* Succeeded: the segment can be placed. */
	return NULL;
}
