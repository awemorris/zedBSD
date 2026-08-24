/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static uint16_t
get16(const unsigned char *p, int little)
{
	if (little)
		return (uint16_t)p[0] | (uint16_t)p[1] << 8;
	return (uint16_t)p[0] << 8 | p[1];
}

static uint32_t
get32(const unsigned char *p, int little)
{
	if (little)
		return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | p[3];
}

static uint64_t
get64(const unsigned char *p, int little)
{
	if (little)
		return (uint64_t)get32(p, 1) | (uint64_t)get32(p + 4, 1) << 32;
	return (uint64_t)get32(p, 0) << 32 | get32(p + 4, 0);
}

static int
range_ok(uint64_t offset, uint64_t length, size_t size)
{
	return offset <= size && length <= (uint64_t)size - offset;
}

static char
symbol_letter(const struct section_record *sections, size_t section_count,
	      unsigned short section, unsigned binding, unsigned type)
{
	char letter;
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
	if (binding == STB_WEAK) {
		if (section == SHN_UNDEF)
			return type == STT_OBJECT ? 'v' : 'w';
		return type == STT_OBJECT ? 'V' : 'W';
	}
	if (binding == STB_LOCAL && letter >= 'A' && letter <= 'Z')
		letter = (char)(letter - 'A' + 'a');
	return letter;
}

static int
add_symbol(struct elf_symbol_table *table, const unsigned char *entry,
	   int elf64, int little, const unsigned char *strings,
	   size_t string_size, const struct section_record *sections,
	   size_t section_count)
{
	struct elf_symbol_record symbol;
	struct elf_symbol_record *symbols;
	uint32_t name_offset = get32(entry, little);
	unsigned char info = entry[elf64 ? 4 : 12];

	if (name_offset >= string_size ||
	    !memchr(strings + name_offset, '\0', string_size - name_offset)) {
		errno = EINVAL;
		return -1;
	}
	memset(&symbol, 0, sizeof(symbol));
	symbol.name = strdup((const char *)strings + name_offset);
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
	if (table->count == SIZE_MAX / sizeof(*symbols)) {
		free(symbol.name);
		errno = EOVERFLOW;
		return -1;
	}
	symbols =
	    realloc(table->symbols, (table->count + 1) * sizeof(*symbols));
	if (!symbols) {
		free(symbol.name);
		return -1;
	}
	table->symbols = symbols;
	table->symbols[table->count++] = symbol;
	return 0;
}

int
elf_symbols_read(const void *buffer, size_t size, int dynamic,
		 struct elf_symbol_table *table)
{
	const unsigned char *data = buffer;
	struct section_record *sections = NULL;
	uint64_t section_offset;
	uint16_t section_size, section_count;
	int elf64, little;
	int found = 0;

	memset(table, 0, sizeof(*table));
	if (size < 16 || memcmp(data, "\177ELF", 4) ||
	    (data[4] != 1 && data[4] != 2) || (data[5] != 1 && data[5] != 2)) {
		errno = EINVAL;
		return -1;
	}
	elf64 = data[4] == 2;
	little = data[5] == 1;
	if (size < (size_t)(elf64 ? 64 : 52)) {
		errno = EINVAL;
		return -1;
	}
	section_offset =
	    elf64 ? get64(data + 40, little) : get32(data + 32, little);
	section_size = get16(data + (elf64 ? 58 : 46), little);
	section_count = get16(data + (elf64 ? 60 : 48), little);
	if (!section_count || section_size < (elf64 ? 64 : 40) ||
	    !range_ok(section_offset, (uint64_t)section_size * section_count,
		      size)) {
		errno = EINVAL;
		return -1;
	}
	sections = calloc(section_count, sizeof(*sections));
	if (!sections)
		return -1;
	for (size_t i = 0; i < section_count; i++) {
		const unsigned char *s =
		    data + section_offset + i * section_size;
		sections[i].type = get32(s + 4, little);
		sections[i].flags =
		    elf64 ? get64(s + 8, little) : get32(s + 8, little);
		sections[i].offset =
		    elf64 ? get64(s + 24, little) : get32(s + 16, little);
		sections[i].size =
		    elf64 ? get64(s + 32, little) : get32(s + 20, little);
		sections[i].link = get32(s + (elf64 ? 40 : 24), little);
		sections[i].entsize =
		    elf64 ? get64(s + 56, little) : get32(s + 36, little);
		if (sections[i].type != SHT_NOBITS &&
		    !range_ok(sections[i].offset, sections[i].size, size)) {
			errno = EINVAL;
			goto fail;
		}
	}
	for (size_t i = 0; i < section_count; i++) {
		const struct section_record *symbols = &sections[i];
		const struct section_record *strings;
		size_t minimum = elf64 ? 24 : 16;
		if (symbols->type != (dynamic ? SHT_DYNSYM : SHT_SYMTAB))
			continue;
		found = 1;
		if (symbols->link >= section_count ||
		    sections[symbols->link].type != SHT_STRTAB ||
		    symbols->entsize < minimum ||
		    symbols->size % symbols->entsize) {
			errno = EINVAL;
			goto fail;
		}
		strings = &sections[symbols->link];
		for (uint64_t n = 0; n < symbols->size / symbols->entsize;
		     n++) {
			const unsigned char *entry =
			    data + symbols->offset + n * symbols->entsize;
			if (add_symbol(table, entry, elf64, little,
				       data + strings->offset,
				       (size_t)strings->size, sections,
				       section_count))
				goto fail;
		}
	}
	free(sections);
	table->bits = elf64 ? 64 : 32;
	if (!found) {
		errno = ENOENT;
		return -1;
	}
	return 0;
fail:
	free(sections);
	elf_symbols_free(table);
	return -1;
}

void
elf_symbols_free(struct elf_symbol_table *table)
{
	for (size_t i = 0; i < table->count; i++)
		free(table->symbols[i].name);
	free(table->symbols);
	memset(table, 0, sizeof(*table));
}
