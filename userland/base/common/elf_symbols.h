/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_ELF_SYMBOLS_H
#define ZEDBSD_USERLAND_ELF_SYMBOLS_H

#include <stddef.h>
#include <stdint.h>

struct elf_symbol_record {
	char *name;
	uint64_t value;
	uint64_t size;
	unsigned char binding;
	unsigned char type;
	unsigned short section;
	char letter;
};

struct elf_symbol_table {
	struct elf_symbol_record *symbols;
	size_t count;
	unsigned bits;
};

int elf_symbols_read(const void *data, size_t size, int dynamic,
		     struct elf_symbol_table *table);
void elf_symbols_free(struct elf_symbol_table *table);

#endif
