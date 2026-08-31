/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland elf interface.
 */

#ifndef ZEDBSD_RTLD_ELF_H
#define ZEDBSD_RTLD_ELF_H

#include <stddef.h>
#include <stdint.h>

#define EI_NIDENT 16
#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2
#define EV_CURRENT 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_386 3
#define EM_SPARCV9 43
#define EM_X86_64 62
#define EM_AARCH64 183

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551U
#define PT_GNU_RELRO 0x6474e552U
#define PF_X 1U
#define PF_W 2U
#define PF_R 4U

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_GNU_HASH 0x6ffffef5
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_RPATH 15
#define DT_SYMBOLIC 16
#define DT_REL 17
#define DT_RELSZ 18
#define DT_RELENT 19
#define DT_PLTREL 20
#define DT_DEBUG 21
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_BIND_NOW 24
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH 29
#define DT_FLAGS 30
#define DT_PREINIT_ARRAY 32
#define DT_PREINIT_ARRAYSZ 33
#define DT_VERSYM 0x6ffffff0
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEED 0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

#define VER_DEF_CURRENT 1
#define VER_NEED_CURRENT 1
#define VER_FLG_BASE 0x1U
#define VER_NDX_LOCAL 0
#define VER_NDX_GLOBAL 1
#define VER_NDX_HIDDEN 0x8000U
#define VER_NDX_MASK 0x7fffU

#define SHN_UNDEF 0
#define STN_UNDEF 0
#define SHN_ABS 0xfff1U
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_TLS 6
#define STV_DEFAULT 0
#define STV_HIDDEN 2
#define ELF_ST_BIND(info) ((unsigned)(info) >> 4)
#define ELF_ST_TYPE(info) ((unsigned)(info) & 15U)
#define ELF_ST_VISIBILITY(other) ((unsigned)(other) & 3U)

struct elf32_ehdr {
	uint8_t e_ident[EI_NIDENT];
	uint16_t e_type, e_machine;
	uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum;
	uint16_t e_shentsize, e_shnum, e_shstrndx;
};
struct elf32_phdr {
	uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz;
	uint32_t p_flags, p_align;
};
struct elf32_dyn {
	int32_t d_tag;
	union {
		uint32_t d_val, d_ptr;
	} d_un;
};
struct elf32_sym {
	uint32_t st_name, st_value, st_size;
	uint8_t st_info, st_other;
	uint16_t st_shndx;
};
struct elf32_rel {
	uint32_t r_offset, r_info;
};
struct elf32_rela {
	uint32_t r_offset, r_info;
	int32_t r_addend;
};

struct elf64_ehdr {
	uint8_t e_ident[EI_NIDENT];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum;
	uint16_t e_shentsize, e_shnum, e_shstrndx;
};
struct elf64_phdr {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct elf64_dyn {
	int64_t d_tag;
	union {
		uint64_t d_val, d_ptr;
	} d_un;
};
struct elf64_sym {
	uint32_t st_name;
	uint8_t st_info, st_other;
	uint16_t st_shndx;
	uint64_t st_value, st_size;
};
struct elf64_rel {
	uint64_t r_offset, r_info;
};
struct elf64_rela {
	uint64_t r_offset, r_info;
	int64_t r_addend;
};

/* The GNU/Solaris symbol-version records have the same layout in ELF32/64. */
struct elf_verdef {
	uint16_t vd_version, vd_flags, vd_ndx, vd_cnt;
	uint32_t vd_hash, vd_aux, vd_next;
};
struct elf_verdaux {
	uint32_t vda_name, vda_next;
};
struct elf_verneed {
	uint16_t vn_version, vn_cnt;
	uint32_t vn_file, vn_aux, vn_next;
};
struct elf_vernaux {
	uint32_t vna_hash;
	uint16_t vna_flags, vna_other;
	uint32_t vna_name, vna_next;
};
typedef struct elf_verdef Elf_Verdef;
typedef struct elf_verdaux Elf_Verdaux;
typedef struct elf_verneed Elf_Verneed;
typedef struct elf_vernaux Elf_Vernaux;
typedef uint16_t Elf_Versym;

#if defined(ZEDBSD_USER_ABI_LP64)
typedef struct elf64_ehdr Elf_Ehdr;
typedef struct elf64_phdr Elf_Phdr;
typedef struct elf64_dyn Elf_Dyn;
typedef struct elf64_sym Elf_Sym;
typedef struct elf64_rel Elf_Rel;
typedef struct elf64_rela Elf_Rela;
typedef uint64_t Elf_Addr;
typedef uint64_t Elf_Off;
#define ELF_CLASS ELFCLASS64
#define ELF_R_SYM(value) ((uint32_t)((value) >> 32))
#define ELF_R_TYPE(value) ((uint32_t)(value))
#else
typedef struct elf32_ehdr Elf_Ehdr;
typedef struct elf32_phdr Elf_Phdr;
typedef struct elf32_dyn Elf_Dyn;
typedef struct elf32_sym Elf_Sym;
typedef struct elf32_rel Elf_Rel;
typedef struct elf32_rela Elf_Rela;
typedef uint32_t Elf_Addr;
typedef uint32_t Elf_Off;
#define ELF_CLASS ELFCLASS32
#define ELF_R_SYM(value) ((uint32_t)(value) >> 8)
#define ELF_R_TYPE(value) ((uint8_t)(value))
#endif

/* i386 */
#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define R_386_TLS_DTPMOD32 35
#define R_386_TLS_DTPOFF32 36
#define R_386_TLS_DESC 41

/* AMD64 */
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TLSDESC 36

/* AArch64 */
#define R_AARCH64_NONE 0
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLSDESC 1031

/* SPARC V9 */
#define R_SPARC_NONE 0
#define R_SPARC_64 32
#define R_SPARC_GLOB_DAT 20
#define R_SPARC_JMP_SLOT 21
#define R_SPARC_RELATIVE 22
#define R_SPARC_TLS_DTPMOD64 75
#define R_SPARC_TLS_DTPOFF64 77

#endif
