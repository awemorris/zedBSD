/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 68040 Macintosh second stage: what its parts share.
 *
 * The parts that only compute (the memory banks, the kernel's ELF, the
 * boot information records, the configuration and the handoff plan) take
 * addresses as 32-bit numbers and do not touch the machine, so that they
 * build and run on a development host too.
 */

#ifndef KERN_BOOT_MAC68K_STAGE2_H
#define KERN_BOOT_MAC68K_STAGE2_H

#include <stddef.h>
#include <stdint.h>

/* The unit the loader translates and copies memory in. */
#define MAC68K_PAGE_SIZE		4096U

/* The most physical runs of RAM the loader keeps while it walks memory. */
#define MAC68K_RUNS_MAX			32U

/* The most memory banks the kernel reads from the boot information. */
#define MAC68K_KERNEL_BANKS_MAX		8U

/* The most loadable segments a kernel may have, and the most ranges its handoff writes. */
#define MAC68K_SEGMENTS_MAX		8U
#define MAC68K_RANGES_MAX		(2U * MAC68K_SEGMENTS_MAX + 1U)

/* The longest kernel file name (an HFS name) and command line. */
#define MAC68K_NAME_MAX			31U
#define MAC68K_COMMAND_LINE_MAX		512U

/* A piece of the handoff that fills its target with zeroes instead of copying. */
#define MAC68K_PIECE_ZERO		0x00000001U

/* The handoff resets the floating-point unit, which the processor has. */
#define MAC68K_PLAN_FPU			0x00000001U

/*
 * One run of physical RAM.
 */
struct mac68k_bank {
	uint32_t base;
	uint32_t size;
};

/*
 * The machine's physical RAM as the loader found it, by walking the
 * memory the ROM maps.  The runs are kept sorted and merged once the walk
 * is finished.
 */
struct mac68k_memory {
	struct mac68k_bank banks[MAC68K_RUNS_MAX];
	unsigned count;
	unsigned dropped_pages;
};

/*
 * One loadable segment of the kernel: where its bytes are in the file
 * and where the kernel runs them.
 */
struct mac68k_segment {
	uint32_t file_offset;
	uint32_t physical;
	uint32_t file_size;
	uint32_t memory_size;
};

/*
 * The kernel's ELF as the loader needs it: the entry, the segments and
 * the end of the image, after which the boot information goes.
 */
struct mac68k_kernel_image {
	uint32_t entry;
	uint32_t lowest;
	uint32_t end;
	unsigned segment_count;
	struct mac68k_segment segments[MAC68K_SEGMENTS_MAX];
};

/*
 * The boot information being written: a buffer filled with tagged
 * records, big-endian, in the form the kernel reads.
 */
struct mac68k_bootinfo {
	uint8_t *buffer;
	uint32_t capacity;
	uint32_t used;
	int overflow;
};

/*
 * The loader's configuration file: the kernel's file name and the
 * kernel's command line.
 */
struct mac68k_config {
	char kernel[MAC68K_NAME_MAX + 1U];
	char command_line[MAC68K_COMMAND_LINE_MAX];
};

/*
 * One range the handoff writes: bytes from the loader's memory (at a
 * logical address) to the kernel's physical address, or zeroes.
 */
struct mac68k_load_range {
	uint32_t source;
	uint32_t target;
	uint32_t length;
	uint32_t flags;
};

/*
 * One step of the handoff's copy, in physical addresses: the trampoline
 * runs them in order with translation off.
 */
struct mac68k_piece {
	uint32_t source;
	uint32_t target;
	uint32_t length;
	uint32_t flags;
};

/*
 * The handoff plan the trampoline follows: the kernel's entry, a zero
 * word that resets the floating-point unit, the flags, and the steps.
 * The steps first move every source that lies where the kernel goes to
 * spare pages, then place the kernel and its boot information.
 */
struct mac68k_plan {
	uint32_t entry;
	uint32_t null_fpu_frame;
	uint32_t flags;
	uint32_t count;
	uint32_t pieces;
};

/*
 * Translates a logical address of the loader to a physical one; reports
 * zero when the address is mapped.
 */
typedef int (*mac68k_translate_fn)(void *cookie, uint32_t logical, uint32_t *physical);

/* lib.c */
void mac68k_copy_bytes(void *target, const void *source, uint32_t length);
void mac68k_fill_bytes(void *target, int value, uint32_t length);
uint32_t mac68k_read_be32(const uint8_t *bytes);
uint16_t mac68k_read_be16(const uint8_t *bytes);
void mac68k_write_be32(uint8_t *bytes, uint32_t value);
void mac68k_write_be16(uint8_t *bytes, uint16_t value);
uint32_t mac68k_crc32(const uint8_t *bytes, uint32_t length);
uint32_t mac68k_string_length(const char *text);

/* memory.c */
void mac68k_memory_init(struct mac68k_memory *memory);
void mac68k_memory_add_page(struct mac68k_memory *memory, uint32_t physical);
void mac68k_memory_finish(struct mac68k_memory *memory);
int mac68k_memory_holds(const struct mac68k_memory *memory, uint32_t base, uint32_t size);

/* elf.c */
const char *mac68k_kernel_parse(const uint8_t *file, uint32_t length, struct mac68k_kernel_image *image);

/* bootinfo.c */
void mac68k_bootinfo_begin(struct mac68k_bootinfo *info, uint8_t *buffer, uint32_t capacity);
void mac68k_bootinfo_add_long(struct mac68k_bootinfo *info, uint16_t tag, uint32_t value);
void mac68k_bootinfo_add_pair(struct mac68k_bootinfo *info, uint16_t tag, uint32_t first, uint32_t second);
void mac68k_bootinfo_add_text(struct mac68k_bootinfo *info, uint16_t tag, const char *text);
void mac68k_bootinfo_end(struct mac68k_bootinfo *info);

/* config.c */
void mac68k_config_parse(struct mac68k_config *config, const char *text, uint32_t length);

/* handoff.c */
unsigned mac68k_handoff_ranges(struct mac68k_load_range *ranges, const struct mac68k_kernel_image *image, uint32_t kernel, uint32_t bootinfo, uint32_t bootinfo_size);
uint32_t mac68k_handoff_pieces_max(const struct mac68k_load_range *ranges, unsigned count);
const char *mac68k_handoff_plan(struct mac68k_plan *plan, struct mac68k_piece *pieces, uint32_t capacity, const struct mac68k_load_range *ranges, unsigned range_count, mac68k_translate_fn translate, void *cookie, const struct mac68k_memory *memory, uint32_t area_base, uint32_t area_size);

#endif
