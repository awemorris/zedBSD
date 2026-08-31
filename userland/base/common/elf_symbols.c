/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements shared userland elf symbols support.
 */

#include "userland/base/common/elf_symbols.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_DYNSYM 11
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_OBJECT 1

struct section_record {
	uint32_t type;
	uint64_t flags;
	uint64_t offset;
	uint64_t size;
	uint32_t link;
	uint64_t entsize;
};

static uint64_t get64(const unsigned char *p, int little);
static uint32_t get32(const unsigned char *p, int little);
static uint16_t get16(const unsigned char *p, int little);
static int range_ok(uint64_t offset, uint64_t length, size_t size);
static int add_symbol(struct elf_symbol_table *table, const unsigned char *entry, int elf64, int little, const unsigned char *strings, size_t string_size, const struct section_record *sections, size_t section_count);
static char symbol_letter(const struct section_record *sections, size_t section_count, unsigned short section, unsigned binding, unsigned type);

/*
 * Implements the elf symbols read operation.
 */
int
elf_symbols_read(
	const void *buffer,
	size_t size,
	int dynamic,
	struct elf_symbol_table *table)
{
	const unsigned char *s;
	const unsigned char *entry;
	const struct section_record *symbols;
	const struct section_record *strings;
	size_t minimum;
	size_t i_index_for;
	size_t i_index_for1;
	uint64_t n_index_for;
	const unsigned char *data;
	struct section_record *sections;
	uint64_t section_offset;
	uint16_t section_size, section_count;
	int elf64, little;
	int found;

	data = buffer;
	sections = NULL;
	found = 0;

	memset(table, 0, sizeof(*table));

	/* Checks the current data size. */
	if (size < 16 || memcmp(data, "\177ELF", 4) ||
	    (data[4] != 1 && data[4] != 2) || (data[5] != 1 && data[5] != 2)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	elf64 = data[4] == 2;
	little = data[5] == 1;

	/* Checks the current data size. */
	if (size < (size_t)(elf64 ? 64 : 52)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	section_offset =
	    elf64 ? get64(data + 40, little) : get32(data + 32, little);
	section_size = get16(data + (elf64 ? 58 : 46), little);
	section_count = get16(data + (elf64 ? 60 : 48), little);

	/* Handles a failed range ok operation. */
	if (!section_count || section_size < (elf64 ? 64 : 40) ||
	    !range_ok(section_offset, (uint64_t)section_size * section_count,
		      size)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	sections = calloc(section_count, sizeof(*sections));

	/* Handles the sections condition. */
	if (!sections)
		return -1;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < section_count; i_index_for++) {

		s = data + section_offset + i_index_for * section_size;
		sections[i_index_for].type = get32(s + 4, little);
		sections[i_index_for].flags =
		    elf64 ? get64(s + 8, little) : get32(s + 8, little);
		sections[i_index_for].offset =
		    elf64 ? get64(s + 24, little) : get32(s + 16, little);
		sections[i_index_for].size =
		    elf64 ? get64(s + 32, little) : get32(s + 20, little);
		sections[i_index_for].link = get32(s + (elf64 ? 40 : 24), little);
		sections[i_index_for].entsize =
		    elf64 ? get64(s + 56, little) : get32(s + 36, little);

		/* Handles a failed range ok operation. */
		if (sections[i_index_for].type != SHT_NOBITS &&
		    !range_ok(sections[i_index_for].offset, sections[i_index_for].size, size)) {
			errno = EINVAL;
			goto fail;
		}
	}

	/* Process each remaining element. */
	for (i_index_for1 = 0; i_index_for1 < section_count; i_index_for1++) {
				symbols = &sections[i_index_for1];

				minimum = elf64 ? 24 : 16;

		/* Handles the symbols condition. */
		if (symbols->type != (dynamic ? SHT_DYNSYM : SHT_SYMTAB))
			continue;
		found = 1;

		/* Handles the symbols condition. */
		if (symbols->link >= section_count ||
		    sections[symbols->link].type != SHT_STRTAB ||
		    symbols->entsize < minimum ||
		    symbols->size % symbols->entsize) {
			errno = EINVAL;
			goto fail;
		}

		/* Process each remaining element. */
		strings = &sections[symbols->link];
		for (n_index_for = 0; n_index_for < symbols->size / symbols->entsize;
		     n_index_for++) {

			entry = data + symbols->offset + n_index_for * symbols->entsize;

			/* Handles a failed add symbol operation. */
			if (add_symbol(table, entry, elf64, little,
				       data + strings->offset,
				       (size_t)strings->size, sections,
				       section_count))
				goto fail;
		}
	}
	free(sections);
	table->bits = elf64 ? 64 : 32;

	/* Handles the found condition. */
	if (!found) {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
fail:
	free(sections);
	elf_symbols_free(table);

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the elf symbols free operation.
 */
void
elf_symbols_free(
	struct elf_symbol_table *table)
{
	size_t i_index_for;

	/* Process each remaining element. */
	for (i_index_for = 0; i_index_for < table->count; i_index_for++)
		free(table->symbols[i_index_for].name);
	free(table->symbols);
	memset(table, 0, sizeof(*table));
}

/* Supports the get64 operation. */
static uint64_t
get64(
	const unsigned char *p,
	int little)
{
	uint64_t function_result;

	/* Handles the little condition. */
	if (little) {
		/* Computes the function result. */
		function_result = (uint64_t)get32(p, 1) | (uint64_t)get32(p + 4, 1) << 32;

		/* Returns the computed result. */
		return function_result;
	}

	/* Computes the function result. */
	function_result = (uint64_t)get32(p, 0) << 32 | get32(p + 4, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the get32 operation. */
static uint32_t
get32(
	const unsigned char *p,
	int little)
{
	/* Handles the little condition. */
	if (little)
		return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;

	/* Returns the computed result. */
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

/* Supports the get16 operation. */
static uint16_t
get16(
	const unsigned char *p,
	int little)
{
	/* Handles the little condition. */
	if (little)
		return (uint16_t)p[0] | (uint16_t)p[1] << 8;

	/* Returns the computed result. */
	return (uint16_t)p[0] << 8 | p[1];
}

/* Supports the range ok operation. */
static int
range_ok(
	uint64_t offset,
	uint64_t length,
	size_t size)
{
	/* Returns the computed result. */
	return offset <= size && length <= (uint64_t)size - offset;
}

/* Supports the add symbol operation. */
static int
add_symbol(
	struct elf_symbol_table *table,
	const unsigned char *entry,
	int elf64,
	int little,
	const unsigned char *strings,
	size_t string_size,
	const struct section_record *sections,
	size_t section_count)
{
	struct elf_symbol_record symbol;
	struct elf_symbol_record *symbols;
	uint32_t name_offset = get32(entry, little);
	unsigned char info = entry[elf64 ? 4 : 12];

	/* Handles a failed memchr operation. */
	if (name_offset >= string_size ||
	    !memchr(strings + name_offset, '\0', string_size - name_offset)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(&symbol, 0, sizeof(symbol));
	symbol.name = strdup((const char *)strings + name_offset);

	/* Handles the symbol condition. */
	if (!symbol.name)
		return -1;
	symbol.binding = info >> 4;
	symbol.type = info & 15;
	symbol.section = get16(entry + (elf64 ? 6 : 14), little);
	symbol.value =
	    elf64 ? get64(entry + 8, little) : get32(entry + 4, little);
	symbol.size =
	    elf64 ? get64(entry + 16, little) : get32(entry + 8, little);
	symbol.letter = symbol_letter(sections, section_count, symbol.section,
				      symbol.binding, symbol.type);

	/* Handles the table condition. */
	if (table->count == SIZE_MAX / sizeof(*symbols)) {
		free(symbol.name);
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	symbols =
	    realloc(table->symbols, (table->count + 1) * sizeof(*symbols));

	/* Handles the symbols condition. */
	if (!symbols) {
		free(symbol.name);

		/* Reports operation failure. */
		return -1;
	}
	table->symbols = symbols;
	table->symbols[table->count++] = symbol;

	/* Reports successful completion. */
	return 0;
}

/* Supports the symbol letter operation. */
static char
symbol_letter(
	const struct section_record *sections,
	size_t section_count,
	unsigned short section,
	unsigned binding,
	unsigned type)
{
	char letter;

	/* Handles the section condition. */
	if (section == SHN_UNDEF)
		letter = 'U';
	else if (section == SHN_ABS)
		letter = 'A';
	else if (section == SHN_COMMON)
		letter = 'C';
	else if (section >= section_count)
		letter = '?';
	else if (sections[section].type == SHT_NOBITS &&
		 (sections[section].flags & SHF_ALLOC))
		letter = 'B';
	else if (sections[section].flags & SHF_EXECINSTR)
		letter = 'T';
	else if ((sections[section].flags & (SHF_ALLOC | SHF_WRITE)) ==
		 (SHF_ALLOC | SHF_WRITE))
		letter = 'D';
	else if (sections[section].flags & SHF_ALLOC)
		letter = 'R';
	else
		letter = 'N';

	/* Handles the binding condition. */
	if (binding == STB_WEAK) {
		/* Handles the section condition. */
		if (section == SHN_UNDEF)
			return type == STT_OBJECT ? 'v' : 'w';

		/* Returns the computed result. */
		return type == STT_OBJECT ? 'V' : 'W';
	}

	/* Handles the binding condition. */
	if (binding == STB_LOCAL && letter >= 'A' && letter <= 'Z')
		letter = (char)(letter - 'A' + 'a');

	/* Returns the computed result. */
	return letter;
}
