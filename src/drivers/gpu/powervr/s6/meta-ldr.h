/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware image of a Series6 core and the memory its META firmware
 * processor runs in.
 *
 * The image is a META loader stream: a header names the first of a chain
 * of command blocks, and each command loads bytes into the firmware's
 * memory, zeroes a range of it, or asks for a register write before the
 * firmware starts.  The firmware's memory is two allocations in the
 * core's firmware heap: code (the bootloader page and the text) and data
 * (the shared and the private data).  The META processor reaches them
 * through segments of its own MMU, which the bootloader sets up from a
 * table of register writes the host leaves in the bootloader page.
 *
 * The image's last 4 KiB is a trailer the loader stream does not use: a
 * signature header in the images this driver runs, a block naming the
 * core in images of a later interface.
 *
 * This part reads only the loader's headers and commands and copies the
 * loaded bytes without looking at them.  It holds no lock and no memory,
 * and it builds on the host too.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_S6_META_LDR_H
#define KERN_DRIVERS_GPU_POWERVR_S6_META_LDR_H

#include <stddef.h>
#include <stdint.h>

/*
 * The shape of the loader stream.
 *
 * The header is 16 bytes: the device word, the offset of the loader's
 * code, the offset of the first command block, a control half-word and
 * a check half-word.  A command block starts with its command half-word,
 * its own length and the offset of the next block (all ones at the end),
 * followed by its argument words.  The argument words of a load name the
 * META address and the offset of a data block; those of a zeroing the
 * address and the byte count; that of a configuration the offset of a
 * data block.  A data block is a tag, a length that counts the tag, the
 * length and a trailing check half-word, and the bytes between.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_meta.h (RGX_META_LDR_BLOCK_HDR,
 * RGX_META_LDR_L1_DATA_BLK, RGX_META_LDR_L2_DATA_BLK, the command values
 * 0 to 5 under RGX_META_LDR_CMD_MASK 0xF, RGX_META_LDR_COMMENT_TYPE_MASK
 * 0x10, the configuration values 0 to 4) and rgxfwimageutils.c
 * (ProcessLDRCommandStream: the data length less 6, the configuration
 * entries of 8, 12 and 20 bytes, the end marker 0xFFFFFFFF).
 */
#define POWERVR_S6_LDR_HEADER_BYTES	16U
#define POWERVR_S6_LDR_BLOCK_BYTES	8U
#define POWERVR_S6_LDR_DATA_HEAD	4U
#define POWERVR_S6_LDR_DATA_OVERHEAD	6U
#define POWERVR_S6_LDR_END		0xffffffffU
#define POWERVR_S6_LDR_COMMAND_MASK	0x000fU
#define POWERVR_S6_LDR_COMMENT		0x0010U
#define POWERVR_S6_LDR_LOAD		0U
#define POWERVR_S6_LDR_LOAD_CORE	1U
#define POWERVR_S6_LDR_LOAD_REGISTERS	2U
#define POWERVR_S6_LDR_START_THREADS	3U
#define POWERVR_S6_LDR_ZERO		4U
#define POWERVR_S6_LDR_CONFIG		5U
#define POWERVR_S6_LDR_CONFIG_PAUSE	0U
#define POWERVR_S6_LDR_CONFIG_READ	1U
#define POWERVR_S6_LDR_CONFIG_WRITE	2U
#define POWERVR_S6_LDR_CONFIG_MEMSET	3U
#define POWERVR_S6_LDR_CONFIG_MEMCHECK	4U
#define POWERVR_S6_LDR_CONFIG_WRITE_BYTES 12U

/*
 * The trailer at the end of an image.
 *
 * Every image is padded to whole 4 KiB blocks and the last block is the
 * trailer.  A signature trailer starts with five one-byte fields
 * (algorithm, digest, key identifier type, signer length, key identifier
 * length), three pad bytes and the signature length as a big-endian
 * word.  A core trailer of a later interface holds, big-endian, the
 * record layout, the longest version string, the packed branch, unit
 * count and configuration (16 bits each from bit 32 down), and an
 * eight-byte version string.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgxfwload.c (VerifyFirmware: the header
 * at the start of the last FW_BLOCK_SIZE bytes, its algorithm, digest,
 * identifier type, signer and key lengths and the network-order
 * signature length); Linux v6.12 include/linux/module_signature.h
 * (struct module_signature: the same five bytes, three pad bytes and a
 * big-endian length at byte 8; PKEY_ID_X509 1); Android kernel/x86_64
 * android-x86_64-fugu-3.10-oreo-r6 8ff24327ec
 * drivers/staging/imgtec/rogue/rgx_fwif_shared.h (FW_BLOCK_SIZE 4096,
 * FW_BVNC_BACKWARDS_OFFSET, RGXFWIF_COMPCHECKS_BVNC with
 * RGXFWIF_COMPCHECKS_LAYOUT_VERSION 2 and V_LEN_MAX 7), rgx_compat_bvnc.h
 * (RGX_BVNC_PACK_SHIFT_B 32, _N 16, _C 0) and rgxsrvinit.c
 * (GetFirmwareBVNC: the trailer's words are big-endian).
 */
#define POWERVR_S6_IMAGE_BLOCK		4096U
#define POWERVR_S6_TRAILER_UNKNOWN	0
#define POWERVR_S6_TRAILER_SIGNATURE	1
#define POWERVR_S6_TRAILER_CORE		2
#define POWERVR_S6_SIGNATURE_X509	1U
#define POWERVR_S6_CORE_TRAILER_LAYOUT	2U
#define POWERVR_S6_CORE_TRAILER_VERSION	8U

/*
 * The firmware's memory as the META processor sees it.
 *
 * The code allocation holds the 4 KiB bootloader page, which the
 * processor reaches at 0x40000000, and the 0x30000-byte text after it.
 * The data allocation holds 0x5000 bytes of shared data and 0x12000
 * bytes of private data.  Bit 31 of an address picks the global view of
 * the same memory.  These cores have no core memory; the range a core
 * memory's data would take is skipped by a zeroing.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgxfwimageutils.c (asRGXFWCodeSegments and
 * asRGXFWDataSegments: segment, META address, size and allocation offset)
 * and rgx_meta.h (RGXFW_SEGMMU_TEXT_ID 0, _SHARED_ID 1, _BOOTLDR_ID 2,
 * _DATA_ID 3, META_MEM_GLOBAL_RANGE_BIT 0x80000000,
 * RGX_META_COREMEM_DATA_ADDR 0x82000000, RGX_META_STACK_SIZE 0x1000,
 * RGX_META_COREMEM_BSS_SIZE 0xA00).
 */
#define POWERVR_S6_SEGMENT_TEXT		0U
#define POWERVR_S6_SEGMENT_SHARED	1U
#define POWERVR_S6_SEGMENT_BOOTLOADER	2U
#define POWERVR_S6_SEGMENT_DATA		3U
#define POWERVR_S6_SEGMENT_COUNT	4U
#define POWERVR_S6_CODE_BYTES		0x31000U
#define POWERVR_S6_DATA_BYTES		0x17000U
#define POWERVR_S6_META_GLOBAL		0x80000000U
#define POWERVR_S6_CORE_DATA_START	0x82000000U
#define POWERVR_S6_CORE_DATA_BYTES	0x1a00U

/*
 * The firmware heap of the core's own MMU and the views META has of it.
 *
 * The heap is three 8 MiB windows from 0xE1_C000_0000; the code
 * allocation sits at its start, because the bootloader page has to be
 * at the heap's base.  An offset in the heap is reached by META at
 * 0x40000000 plus the offset through its cache, or at 0x06000000 plus
 * the offset around it.  A segment's output address carries the page
 * catalogue (always 0, the firmware's own) at bit 44 and the data master
 * of the META processor (7) at bit 40.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgxheapconfig.h (RGX_FIRMWARE_HEAP_BASE
 * 0xE1C0000000, RGX_FIRMWARE_HEAP_SIZE three RGXFW_SEGMMU_DMAP_SIZE),
 * rgx_meta.h (RGXFW_SEGMMU_DMAP_SIZE 8 MiB, RGXFW_SEGMMU_DMAP_ADDR_START
 * 0x06000000, RGXFW_BOOTLDR_META_ADDR 0x40000000, RGXFW_BOOTLDR_DEVV_ADDR,
 * RGXFW_SEGMMU_OUTADDR_TOP without HW_ERN_45914, RGXFW_SEGMMU_META_DM_ID
 * 7) and rgxfwutils.c (RGXSetFirmwareAddress: the heap offset with the
 * cached or the direct-map base).
 */
#define POWERVR_S6_FW_HEAP_BASE		0xe1c0000000ULL
#define POWERVR_S6_FW_HEAP_BYTES	0x1800000ULL
#define POWERVR_S6_FW_CACHED_BASE	0x40000000U
#define POWERVR_S6_FW_UNCACHED_BASE	0x06000000U
#define POWERVR_S6_SEGMENT_CATALOGUE_SHIFT 44U
#define POWERVR_S6_SEGMENT_MASTER_SHIFT	40U
#define POWERVR_S6_META_DATA_MASTER	7U

/*
 * The bootloader's table of register writes.
 *
 * The table starts 0x80 words into the bootloader page and is a run of
 * (register, value) pairs ended by a zero pair, followed by the
 * firmware's arguments: the firmware address of the init record.  The
 * writes the host puts first make the slave port and the debug port
 * privileged and set the text, shared and data segments; the loader's
 * own configuration writes follow; the cache set-up for one running
 * thread comes last.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_meta.h (RGXFW_BOOTLDR_CONF_OFFSET
 * 0x80, META_CR_SYSC_JTAG_THREAD 0x04830030 and _PRIV_EN 4,
 * META_CR_MMCU_SEGMENTn_BASE 0x04850000, _LIMIT +4, _OUTA0 +8, _OUTA1 +0xC
 * with a stride of 0x10, RGXFW_SEGMMU_ALLTHRS_WRITEABLE 0xF02,
 * RGXFW_SEGMMU_ALIGN 0x1000) and rgxfwimageutils.c
 * (RGXFWConfigureMetaCaches: META_CR_MMCU_LOCAL_EBCTRL 0x04830600 with
 * the window-3 bits 0x3 << 14 and 0x3 << 6, META_CR_SYSC_DCPART(n)
 * 0x04830200 + 8n, META_CR_SYSC_ICPART(n) 0x04830220 + 8n, the cached
 * write bit 31, the full-cache value 0xF, META_CR_MMCU_DCACHE_CTRL
 * 0x04830018, META_CR_MMCU_ICACHE_CTRL 0x04830020, the hit enable 1, and
 * the closing write of 0 to 0x040000C0).
 */
#define POWERVR_S6_BOOT_TABLE_OFFSET	0x200U
#define POWERVR_S6_BOOT_TABLE_WORDS	((0x1000U - POWERVR_S6_BOOT_TABLE_OFFSET) / 4U)
#define POWERVR_S6_META_JTAG_THREAD	0x04830030U
#define POWERVR_S6_META_JTAG_PRIVILEGED	0x00000004U
#define POWERVR_S6_META_SEGMENT_BASE	0x04850000U
#define POWERVR_S6_META_SEGMENT_STRIDE	0x10U
#define POWERVR_S6_META_SEGMENT_LIMIT	0x4U
#define POWERVR_S6_META_SEGMENT_OUT_LOW	0x8U
#define POWERVR_S6_META_SEGMENT_OUT_HIGH 0xcU
#define POWERVR_S6_META_SEGMENT_SHARED	0x00000f02U
#define POWERVR_S6_META_SEGMENT_MINIMUM	0x1000U
#define POWERVR_S6_META_BYPASS_CONTROL	0x04830600U
#define POWERVR_S6_META_BYPASS_WINDOWS	0x0000c0c0U
#define POWERVR_S6_META_DATA_PARTITION	0x04830200U
#define POWERVR_S6_META_CODE_PARTITION	0x04830220U
#define POWERVR_S6_META_PARTITION_STRIDE 0x8U
#define POWERVR_S6_META_PARTITION_WRITE	0x80000000U
#define POWERVR_S6_META_PARTITION_WHOLE	0x0000000fU
#define POWERVR_S6_META_THREADS		4U
#define POWERVR_S6_META_DATA_CACHE	0x04830018U
#define POWERVR_S6_META_CODE_CACHE	0x04830020U
#define POWERVR_S6_META_CACHE_HITS	0x00000001U
#define POWERVR_S6_META_LAST_SETUP	0x040000c0U

/* The loader stream's header. */
struct powervr_s6_ldr_header {
	uint32_t device;
	uint32_t loader_code;
	uint32_t first_block;
	uint16_t control;
	uint16_t check;
};

/*
 * What an image's trailer says.
 *
 * Kind tells which of the two forms the trailer has, or that it has
 * neither; only the fields of that form are filled.
 */
struct powervr_s6_trailer {
	int kind;
	uint32_t algorithm;
	uint32_t digest;
	uint32_t key_type;
	uint32_t signer_bytes;
	uint32_t key_bytes;
	uint32_t signature_bytes;
	uint32_t layout;
	uint32_t version_max;
	uint32_t branch;
	uint32_t units;
	uint32_t config;
	char version[POWERVR_S6_CORE_TRAILER_VERSION + 1];
};

/*
 * One command of the loader stream as the walk met it.
 *
 * Block is the command block's offset in the image.  For a load,
 * address and bytes are the META range and source the offset of the
 * bytes in the image; for a zeroing, the range; for a configuration,
 * bytes is the length of its entries.  Segment and target say where the
 * range lands: the segment index and the byte offset in the code or the
 * data allocation, or a segment of -1 when nothing lands.
 */
struct powervr_s6_ldr_step {
	uint32_t block;
	uint32_t command;
	uint32_t address;
	uint32_t bytes;
	uint32_t source;
	int segment;
	uint32_t target;
};

/* Counts of what one walk of a loader stream met. */
struct powervr_s6_ldr_summary {
	uint32_t blocks;
	uint32_t comments;
	uint32_t loads;
	uint32_t zeroings;
	uint32_t configs;
	uint32_t config_writes;
	uint32_t start_threads;
	uint32_t skipped_zeroings;
	uint32_t loaded_bytes[POWERVR_S6_SEGMENT_COUNT];
	uint32_t zeroed_bytes[POWERVR_S6_SEGMENT_COUNT];
	uint32_t end;
};

/*
 * Where a walk puts the firmware.
 *
 * Code and data are the host's views of the two allocations, of
 * POWERVR_S6_CODE_BYTES and POWERVR_S6_DATA_BYTES.  Table is where the
 * bootloader's register writes go, words its capacity and used how many
 * it holds; the walk appends the loader's configuration writes to it.
 * Step, when set, is told of every command, with its context.
 */
struct powervr_s6_ldr_target {
	uint8_t *code;
	uint8_t *data;
	uint32_t *table;
	size_t words;
	size_t used;
	void (*step)(void *context, const struct powervr_s6_ldr_step *step);
	void *context;
};

/* Reads the loader stream's header; EINVAL when the image is too short. */
int powervr_s6_ldr_header(const uint8_t *image, size_t size, struct powervr_s6_ldr_header *header);

/*
 * Reads the trailer at the end of an image.
 *
 * Returns zero with the trailer's form filled, POWERVR_S6_TRAILER_UNKNOWN
 * when it is neither form; EINVAL when the image is not whole 4 KiB
 * blocks with room for a trailer.
 */
int powervr_s6_trailer_read(const uint8_t *image, size_t size, struct powervr_s6_trailer *trailer);

/*
 * Walks the loader stream of an image.
 *
 * Every block, data block and range is checked against the image and the
 * firmware's segments before it is used.  With a target, loads and
 * zeroings go into the target's code and data and configuration writes
 * into its table; without one the walk only checks and counts.  The
 * stream must end before the trailer.  Returns zero, EINVAL for a
 * malformed stream, ENOEXEC for a command the host cannot carry out,
 * ERANGE for a range outside the firmware's memory, or ENOSPC when the
 * table is full.
 */
int powervr_s6_ldr_walk(const uint8_t *image, size_t size, struct powervr_s6_ldr_target *target, struct powervr_s6_ldr_summary *summary);

/*
 * Finds where a META address lands in the firmware's memory.
 *
 * Returns the segment index, or -1 when the range is in no segment;
 * offset receives the byte offset in the code allocation (text and
 * bootloader) or the data allocation (shared and data).
 */
int powervr_s6_segment_find(uint32_t address, uint32_t bytes, uint32_t *offset);

/* Tells whether a segment lives in the code allocation. */
int powervr_s6_segment_is_code(int segment);

/*
 * Starts the bootloader's table.
 *
 * Writes the privilege of the debug port and the text, shared and data
 * segments for code and data allocations at these device addresses.
 * ENOSPC when the table is too small.
 */
int powervr_s6_boot_table_begin(struct powervr_s6_ldr_target *target, uint64_t code_device, uint64_t data_device);

/*
 * Ends the bootloader's table.
 *
 * Writes the cache set-up for one running thread, the zero pair and the
 * firmware address of the init record.  ENOSPC when the table is full.
 */
int powervr_s6_boot_table_end(struct powervr_s6_ldr_target *target, uint32_t init_record);

/*
 * Converts a device address in the firmware heap to a META address.
 *
 * Cached picks the view through META's cache.  ERANGE when the address
 * is outside the heap.
 */
int powervr_s6_fw_address(uint64_t device, int cached, uint32_t *address);

#endif
