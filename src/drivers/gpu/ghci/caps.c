/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a GHCI 1.0 GPU can do: the identification and BOOT_CAPS, the
 * extended capability list and its blocks, decoded from a copy of the
 * register window, and the capability block the command processor
 * writes to memory; and the sizes the driver derives from them (the
 * page table heap, the watchdog and the reset wait).
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "ghci-regs.h"

/* The pairs of a DEVICE_CAPS bit and a copy of it in another word. */
#define MIRROR_PAIRS			5U

/* The VA_BITS of the 2-level walk; a wider VA has the 3-level walk. */
#define VA_BITS_32			32U

/* The address range of the GPU the driver uses: the low 4 GiB. */
#define DRIVER_VA_BYTES			0x100000000ULL

/* The milliseconds a reset may take when the clock is not known. */
#define RESET_WAIT_UNKNOWN_MS		10000U

/* The least milliseconds a reset is waited for, and the margin added to quiesce. */
#define RESET_WAIT_MIN_MS		1000U
#define RESET_WAIT_MARGIN_MS		500U

/* How many watchdog periods quiesce may take before AXI_STUCK. */
#define QUIESCE_PERIODS			4U

/* A DEVICE_CAPS bit, the block word that mirrors it and the mirror bit. */
struct caps_mirror {
	uint32_t device_bit;
	uint32_t word;
	uint32_t mirror_bit;
};

/*
 * The DEVICE_CAPS bits and their mirrors.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.2,
 * EXT_CAP_FEATURE CAP_DUAL_SOURCE [8] and CAP_FP16 [28]; EXT_CAP_FMT0
 * CAP_FFILT_FP16 [10], which is CAP_TEX_FFILT; section 3.2.4,
 * SHADER_STAGES [0] and [1], which mirror DEVICE_CAPS[27] and [28]).
 */
static const struct caps_mirror caps_mirrors[MIRROR_PAIRS] = {
	{ GHCI_CAP_DUAL_SOURCE, GHCI_CAPS_WORD_FEATURE, GHCI_FEATURE_DUAL_SOURCE },
	{ GHCI_CAP_FP16, GHCI_CAPS_WORD_FEATURE, GHCI_FEATURE_FP16 },
	{ GHCI_CAP_TEX_FFILT, GHCI_CAPS_WORD_FMT0, GHCI_FMT0_FFILT_FP16 },
	{ GHCI_CAP_TASK_SHADER, GHCI_CAPS_WORD_SHADER_STAGES, GHCI_STAGE_TASK_SHADER },
	{ GHCI_CAP_MESH_SHADER, GHCI_CAPS_WORD_SHADER_STAGES, GHCI_STAGE_MESH_SHADER }
};

static uint32_t window_word(const uint32_t *window, uint32_t offset);
static void decode_memory(const uint32_t *window, struct ghci_caps *caps);
static int block_layout(const uint32_t *words, uint32_t dwords, uint32_t *first, uint32_t *count);
static void decode_words(const uint32_t *words, struct ghci_caps *caps);
static void check_copies(struct ghci_caps *caps);
static void check_stages(struct ghci_caps *caps);
static uint32_t mirror_word(const struct ghci_caps *caps, uint32_t word);
static int bit_disagrees(uint32_t authority, uint32_t copy);

/*
 * Reports whether a register window is a GHCI 1.0 GPU.
 */
int
ghci_caps_identify(
	const uint32_t *window)
{
	uint32_t magic;
	uint32_t major;

	/* Refuses a window whose magic is not GHCI. */
	magic = window_word(window, GHCI_REG_MAGIC);
	if (magic != GHCI_MAGIC_VALUE)
		return ENODEV;

	/* Refuses a major version other than 1; any minor is the same GHCI 1.0. */
	major = window_word(window, GHCI_REG_VERSION) >> GHCI_VERSION_MAJOR_SHIFT;
	if (major != GHCI_VERSION_MAJOR)
		return ENODEV;

	/* Succeeded: the window is a GHCI 1.0 GPU. */
	return 0;
}

/*
 * Decodes what the register window says before the processor runs:
 * BOOT_CAPS, the extended capability list and the MEM block.
 *
 * The record is cleared first, so that the words of the capability block
 * read as absent until ghci_caps_block() fills them.  A list that is
 * malformed leaves the device without blocks rather than refusing it.
 */
int
ghci_caps_boot(
	const uint32_t *window,
	struct ghci_caps *caps)
{
	uint32_t found;
	int error;

	/* Refuses a window that is not a GHCI 1.0 GPU. */
	error = ghci_caps_identify(window);
	if (error != 0)
		return error;

	/* Clears the record so that a block the list does not name reads as absent. */
	*caps = (struct ghci_caps){ 0 };

	/*
	 * The one capability register and the widths it encodes.
	 *
	 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.0,
	 * BOOT_CAPS [11:8] VA_BITS - 30, [15:12] PA_BITS - 40).
	 */
	caps->boot_caps = window_word(window, GHCI_REG_BOOT_CAPS);
	caps->va_bits = GHCI_BOOT_VA_BITS_BASE + ((caps->boot_caps >> GHCI_BOOT_VA_BITS_SHIFT) & GHCI_BOOT_BITS_MASK);
	caps->pa_bits = GHCI_BOOT_PA_BITS_BASE + ((caps->boot_caps >> GHCI_BOOT_PA_BITS_SHIFT) & GHCI_BOOT_BITS_MASK);

	/* Walks the extended capability list; a malformed list leaves no blocks. */
	error = ghci_extcap_walk(window, caps->block_offset, caps->block_version, &found);
	if (error != 0) {
		caps->blocks = 0U;
		return 0;
	}

	/* Reads the memory configuration of the MEM block. */
	caps->blocks = found;
	decode_memory(window, caps);

	/* Succeeded: the record holds what the device says before it runs. */
	return 0;
}

/*
 * Decodes the capability block CMD_QUERY_CAPS wrote: words holds the
 * dwords of the answer buffer, of which the device wrote at most dwords.
 *
 * Returns 0 with the block's words in the record, or EINVAL for a block
 * whose identity, version or size the driver cannot read; the record's
 * block words are then left as ghci_caps_boot() cleared them.
 */
int
ghci_caps_block(
	const uint32_t *words,
	uint32_t dwords,
	struct ghci_caps *caps)
{
	uint32_t first;
	uint32_t count;
	uint32_t index;
	int error;

	/* Refuses a block the driver cannot read, before anything is taken from it. */
	error = block_layout(words, dwords, &first, &count);
	if (error != 0)
		return error;

	/* The capability words. */
	decode_words(words, caps);

	/* The format table: one word per VkFormat, as far as the driver keeps them. */
	caps->format_count = count;
	if (caps->format_count > GHCI_CAPS_FORMATS)
		caps->format_count = GHCI_CAPS_FORMATS;
	for (index = 0U; index < caps->format_count; index++)
		caps->formats[index] = words[first + index];

	/* The copies of one fact in two words, for the log. */
	check_copies(caps);

	/* The optional stages the shared memory cannot hold, taken as absent. */
	check_stages(caps);

	/* The words past BOOT_CAPS now answer ghci_caps_has(); before, they read as absent. */
	caps->block_read = 1;

	/* Succeeded: the record holds the whole block. */
	return 0;
}

/*
 * Answers one capability question from a decoded record.
 */
int
ghci_caps_has(
	const struct ghci_caps *caps,
	enum ghci_caps_flag flag)
{
	uint32_t bit;

	/* Picks the bit that answers the question. */
	switch (flag) {
	case GHCI_HAS_UMA:
		/* Unified memory is a GPU without local memory in MEM. */
		bit = 1U;
		if (caps->local_bytes != 0U)
			bit = 0U;
		break;
	case GHCI_HAS_HW_WALKER:
		bit = caps->boot_caps & GHCI_BOOT_HW_WALKER;
		break;
	case GHCI_HAS_PROT:
		bit = caps->boot_caps & GHCI_BOOT_PROT;
		break;
	case GHCI_HAS_INDIRECT_DRAW:
		bit = caps->device_caps & GHCI_CAP_INDIRECT_DRAW;
		break;
	case GHCI_HAS_DUAL_SOURCE:
		bit = caps->device_caps & GHCI_CAP_DUAL_SOURCE;
		break;
	case GHCI_HAS_GSI1:
		bit = caps->device_caps & GHCI_CAP_GSI1;
		break;
	case GHCI_HAS_SHADER_UNITS:
		bit = caps->num_su;
		break;
	case GHCI_HAS_BLT:
		bit = caps->device_caps & GHCI_CAP_BLT;
		break;
	case GHCI_HAS_DISPLAY:
		/* The display is the DISP2 block of the list, which the driver reads before the processor runs. */
		bit = caps->blocks & (1U << GHCI_EXTCAP_ID_DISP2);
		break;
	case GHCI_HAS_MSAA:
		bit = caps->device_caps & GHCI_CAP_MSAA;
		break;
	case GHCI_HAS_TEX_FFILT:
		bit = caps->device_caps & GHCI_CAP_TEX_FFILT;
		break;
	case GHCI_HAS_ROP_PACK:
		bit = caps->device_caps & GHCI_CAP_ROP_PACK;
		break;
	case GHCI_HAS_LOGIC_OP:
		bit = caps->feature & GHCI_FEATURE_LOGIC_OP;
		break;
	case GHCI_HAS_MRT_BLEND:
		bit = caps->feature & GHCI_FEATURE_MRT_BLEND;
		break;
	case GHCI_HAS_HW_CURSOR:
		bit = caps->feature & GHCI_FEATURE_HW_CURSOR;
		break;
	case GHCI_HAS_ETC2:
		bit = caps->fmt0 & GHCI_FMT0_ETC2;
		break;
	case GHCI_HAS_SRGB:
		bit = caps->fmt0 & GHCI_FMT0_SRGB;
		break;
	case GHCI_HAS_DISCRETE:
		/* Discrete is a GPU with local memory in MEM; a GPU without MEM is unified. */
		bit = 0U;
		if (caps->local_bytes != 0U)
			bit = 1U;
		break;
	case GHCI_HAS_FFILT_FP32:
		/* The filter of 32-bit floats needs the float filter of DEVICE_CAPS and its own bit. */
		bit = caps->fmt0 & GHCI_FMT0_FFILT_FP32;
		if ((caps->device_caps & GHCI_CAP_TEX_FFILT) == 0U)
			bit = 0U;
		break;
	case GHCI_HAS_CLIP_POLY:
		bit = caps->feature & GHCI_FEATURE_CLIP_POLY;
		break;
	case GHCI_HAS_TU_BILINEAR:
		bit = caps->fmt0 & GHCI_FMT0_TU_BILINEAR;
		break;
	case GHCI_HAS_BLT_SCALE:
		/* The blitter's own abilities need the blitter. */
		bit = caps->feature & GHCI_FEATURE_BLT_SCALE;
		if ((caps->device_caps & GHCI_CAP_BLT) == 0U)
			bit = 0U;
		break;
	case GHCI_HAS_BLT_BLEND:
		bit = caps->fmt0 & GHCI_FMT0_BLT_BLEND;
		if ((caps->device_caps & GHCI_CAP_BLT) == 0U)
			bit = 0U;
		break;
	case GHCI_HAS_BLT_TILE:
		bit = caps->fmt0 & GHCI_FMT0_BLT_TILE;
		if ((caps->device_caps & GHCI_CAP_BLT) == 0U)
			bit = 0U;
		break;
	case GHCI_HAS_CP:
		/* The ring, the doorbell, the interrupt and the capability block. */
		bit = caps->boot_caps & GHCI_BOOT_CP;
		break;
	case GHCI_HAS_TEX_ARRAY:
		/* The layers of a 1D or 2D array, which the texture unit addresses. */
		bit = caps->fmt0 & GHCI_FMT0_TEX_ARRAY;
		break;
	case GHCI_HAS_TEX_3D:
		bit = caps->fmt0 & GHCI_FMT0_TEX_3D;
		break;
	case GHCI_HAS_TEX_CUBE:
		bit = caps->fmt0 & GHCI_FMT0_TEX_CUBE;
		break;
	case GHCI_HAS_TEX_TRILINEAR:
		/* The linear filter between two mip levels needs the filter within one. */
		bit = caps->fmt0 & GHCI_FMT0_TEX_TRILINEAR;
		if ((caps->fmt0 & GHCI_FMT0_TU_BILINEAR) == 0U)
			bit = 0U;
		break;
	case GHCI_HAS_GEOMETRY_SHADER:
		/* A stage the shared memory cannot hold is in stages_dropped, not here. */
		bit = caps->shader_stages & ~caps->stages_dropped & GHCI_STAGE_GEOMETRY_SHADER;
		break;
	case GHCI_HAS_TESSELLATION:
		bit = caps->shader_stages & ~caps->stages_dropped & GHCI_STAGE_TESSELLATION;
		break;
	case GHCI_HAS_STREAM_OUT:
		/* The stream output of a geometry shader needs the geometry shader. */
		bit = caps->shader_stages & GHCI_STAGE_STREAM_OUT;
		if ((caps->shader_stages & ~caps->stages_dropped & GHCI_STAGE_GEOMETRY_SHADER) == 0U)
			bit = 0U;
		break;
	default:
		bit = 0U;
		break;
	}

	/* Reports whether the device has it. */
	if (bit != 0U)
		return 1;

	/* Succeeded: the device does not have it. */
	return 0;
}

/*
 * Walks the extended capability list and records where each block is.
 *
 * A list that loops, leaves the list area, has more than eight entries,
 * names a block outside the window or names one ID twice is refused.
 */
int
ghci_extcap_walk(
	const uint32_t *window,
	uint32_t *offsets,
	uint32_t *versions,
	uint32_t *found)
{
	uint32_t entry;
	uint32_t count;
	uint32_t word0;
	uint32_t block;
	uint32_t id;
	uint32_t next;

	/* Starts with no blocks found. */
	*found = 0U;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 4, the entry: [7:0] ID, [15:8] version, [31:16] the
	 * distance to the next entry in dwords; word 1 the block offset;
	 * section 4.1, the list at 0x100..0x13F, eight entries at most).
	 */
	entry = window_word(window, GHCI_REG_EXTCAP_PTR);
	if (entry == 0U)
		return 0;

	/* Follows the entries until the last one. */
	for (count = 0U; ; count++) {
		/* Refuses a list longer than the list area holds. */
		if (count >= GHCI_EXTCAP_ENTRIES_MAX)
			return EINVAL;

		/* Refuses an entry outside the list area or not on a dword. */
		if (entry < GHCI_EXTCAP_LIST_FIRST || entry + 8U > GHCI_EXTCAP_LIST_END)
			return EINVAL;
		if ((entry & 3U) != 0U)
			return EINVAL;

		/* Reads the entry's ID, version and block. */
		word0 = window_word(window, entry);
		block = window_word(window, entry + 4U);
		id = word0 & 0xFFU;

		/* Refuses a block outside the window or not on a dword. */
		if (block < GHCI_EXTCAP_BLOCK_FIRST || block >= GHCI_WINDOW_BYTES)
			return EINVAL;
		if ((block & 3U) != 0U)
			return EINVAL;

		/* Records an ID the driver knows; a second entry of one ID is malformed. */
		if (id != 0U && id < GHCI_EXTCAP_IDS) {
			if ((*found & (1U << id)) != 0U)
				return EINVAL;

			*found |= 1U << id;
			offsets[id] = block;
			versions[id] = (word0 >> 8) & 0xFFU;
		}

		/* Stops at the last entry; a distance of zero ends the list. */
		next = word0 >> 16;
		if (next == 0U)
			break;

		/* Moves on; the distance counts dwords from this entry. */
		entry += next * 4U;
	}

	/* Succeeded: every block the list names is recorded. */
	return 0;
}

/*
 * Reports the CP_HEAP bytes one session's root and upper tables may take.
 */
uint32_t
ghci_heap_session_bytes(
	uint32_t va_bits)
{
	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 11.2, the 16 KiB root of the 2-level walk; the 8 KiB
	 * root and the 4 KiB level 1 tables of the 3-level walk).
	 */
	if (va_bits <= VA_BITS_32)
		return GHCI_ROOT_BYTES_VA32;

	/* The 3-level walk: the root and the level 1 tables over the low 4 GiB the driver uses. */
	return GHCI_ROOT_BYTES_VA40 + (uint32_t)(DRIVER_VA_BYTES / GHCI_L1_SPAN_BYTES) * GHCI_TABLE_BYTES;
}

/*
 * Reports the CP_HEAP bytes a device needs for every session and chunk.
 *
 * Every session's root and upper tables, a level 0 table for every 2 MiB
 * chunk the driver may use (the low 4 GiB, or all of a narrower VA), and
 * the spare the specification adds.  A VA narrower than 32 bits is
 * counted with the root of the 2-level walk, the largest it may have.
 */
uint64_t
ghci_heap_device_bytes(
	uint32_t va_bits)
{
	uint64_t bytes;

	/* Adds the roots and upper tables of all sixteen sessions. */
	bytes = (uint64_t)GHCI_SESSIONS * ghci_heap_session_bytes(va_bits);

	/* Adds a level 0 table for each chunk the VA holds; a chunk belongs to one session. */
	bytes += (uint64_t)ghci_va_chunks(va_bits) * GHCI_TABLE_BYTES;

	/* Succeeded: adds the spare the specification's sizing rule keeps. */
	return bytes + GHCI_HEAP_SPARE_BYTES;
}

/*
 * Reports the 2 MiB chunks of GPU addresses the driver may use: those of
 * the low 4 GiB, or of the whole VA when it is narrower.
 */
uint32_t
ghci_va_chunks(
	uint32_t va_bits)
{
	/* A VA of 32 bits or more: the low 4 GiB. */
	if (va_bits >= VA_BITS_32)
		return GHCI_CHUNKS;

	/* Succeeded: the chunks of the narrower VA. */
	return (uint32_t)((1ULL << va_bits) / GHCI_L0_SPAN_BYTES);
}

/*
 * Converts a watchdog time to WDT_CYCLES at the core clock.
 *
 * A clock of zero is unknown and gives zero, which leaves the reset
 * default in place; a time too long for the register is clamped.
 */
uint32_t
ghci_watchdog_cycles(
	uint32_t gclk_hz,
	uint32_t milliseconds)
{
	uint64_t cycles;

	/* Leaves the reset default when the clock is unknown. */
	if (gclk_hz == 0U)
		return 0U;

	/* Computes in 64 bits so that a fast clock and a long time do not wrap. */
	cycles = (uint64_t)gclk_hz * milliseconds / 1000U;
	if (cycles > 0xFFFFFFFFULL)
		return 0xFFFFFFFFU;

	/* Refuses a zero limit, which would fire on every packet. */
	if (cycles == 0U)
		return 1U;

	/* Succeeded: the limit fits the register. */
	return (uint32_t)cycles;
}

/*
 * Reports how long a halt may take, from the watchdog and the clock.
 *
 * HALTING lasts until the packet in flight ends and the memory accesses
 * quiesce; the watchdog ends a packet that runs longer than one period,
 * so the wait covers one period and a margin, and never less than a
 * second.
 */
uint32_t
ghci_halt_wait_ms(
	uint32_t gclk_hz,
	uint32_t wdt_cycles)
{
	uint64_t milliseconds;

	/* Waits the long default when the clock is not known. */
	if (gclk_hz == 0U)
		return RESET_WAIT_UNKNOWN_MS;

	/*
	 * Values referenced: amaterrace-gpu 44b7fab docs/ghci.md (section
	 * 5.2, HALTING ends when the current command finishes and memory
	 * accesses quiesce; section 10, the watchdog counts per packet).
	 */
	milliseconds = (uint64_t)wdt_cycles * 1000U / gclk_hz;
	milliseconds += RESET_WAIT_MARGIN_MS;
	if (milliseconds < RESET_WAIT_MIN_MS)
		return RESET_WAIT_MIN_MS;
	if (milliseconds > 0xFFFFFFFFULL)
		return 0xFFFFFFFFU;

	/* Succeeded: the wait covers the packet in flight. */
	return (uint32_t)milliseconds;
}

/*
 * Reports how long a reset may take, from the watchdog and the clock.
 *
 * Quiesce may take four watchdog periods before AXI_STUCK, so the wait
 * covers them and a margin, and never less than a second.
 */
uint32_t
ghci_reset_wait_ms(
	uint32_t gclk_hz,
	uint32_t wdt_cycles)
{
	uint64_t milliseconds;

	/* Waits the long default when the clock is unknown. */
	if (gclk_hz == 0U)
		return RESET_WAIT_UNKNOWN_MS;

	/*
	 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
	 * (section 10, quiesce ends within 4 x WDT_CYCLES).
	 */
	milliseconds = (uint64_t)QUIESCE_PERIODS * wdt_cycles * 1000U / gclk_hz;
	milliseconds += RESET_WAIT_MARGIN_MS;
	if (milliseconds < RESET_WAIT_MIN_MS)
		return RESET_WAIT_MIN_MS;
	if (milliseconds > 0xFFFFFFFFULL)
		return 0xFFFFFFFFU;

	/* Succeeded: the wait covers quiesce. */
	return (uint32_t)milliseconds;
}

/* Reads one word of a register window copy by its byte offset. */
static uint32_t
window_word(
	const uint32_t *window,
	uint32_t offset)
{
	/* A word outside the window reads as zero, as a reserved register does. */
	if (offset >= GHCI_WINDOW_BYTES)
		return 0U;

	/* Succeeded: the word at the offset. */
	return window[offset / 4U];
}

/* Reads the registers of the MEM block, when the list names it. */
static void
decode_memory(
	const uint32_t *window,
	struct ghci_caps *caps)
{
	uint32_t base;

	/* A GPU without MEM is unified, reaches host memory below 2 GiB and has no local window. */
	if ((caps->blocks & (1U << GHCI_EXTCAP_ID_MEM)) == 0U)
		return;

	/* The local memory, the window onto it and the host addresses the GPU reaches. */
	base = caps->block_offset[GHCI_EXTCAP_ID_MEM];
	caps->local_base = window_word(window, base + GHCI_MEM_LOCAL_BASE);
	caps->local_bytes = window_word(window, base + GHCI_MEM_LOCAL_SIZE);
	caps->window_bytes = window_word(window, base + GHCI_MEM_WINDOW_SIZE);
	caps->host_limit = window_word(window, base + GHCI_MEM_HOST_LIMIT);
}

/*
 * Checks the identity, version and size of a capability block and finds
 * its format table: the table's first dword and its formats.  Returns 0,
 * or EINVAL.
 */
static int
block_layout(
	const uint32_t *words,
	uint32_t dwords,
	uint32_t *first,
	uint32_t *count)
{
	uint32_t version;
	uint32_t size;

	/*
	 * A buffer too short for the header, or one the device did not
	 * write (it reads as the zero the driver cleared it to).
	 */
	if (dwords < GHCI_CAPS_HEADER_DWORDS)
		return EINVAL;
	if (words[GHCI_CAPS_WORD_ID] != GHCI_CAPS_ID_VALUE)
		return EINVAL;

	/*
	 * Version 1 or later; a later version keeps every word of version 1
	 * where it is.  The size counts dwords in whole 64-byte lines.
	 *
	 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.5,
	 * BLOCK_INFO: [15:0] version 1, [31:16] the dwords, 192).
	 */
	version = words[GHCI_CAPS_WORD_INFO] & GHCI_CAPS_VERSION_MASK;
	size = words[GHCI_CAPS_WORD_INFO] >> GHCI_CAPS_DWORDS_SHIFT;
	if (version == 0U)
		return EINVAL;
	if (size < GHCI_CAPS_HEADER_DWORDS || (size % GHCI_CAPS_DWORDS_ALIGN) != 0U)
		return EINVAL;

	/* The device wrote no more than the buffer holds; the rest of a larger block is not read. */
	if (size > dwords)
		size = dwords;

	/* The format table lies behind the words the driver knows and within the block. */
	*first = words[GHCI_CAPS_WORD_FMT_FIRST];
	*count = words[GHCI_CAPS_WORD_FMT_COUNT];
	if (*first < GHCI_CAPS_WORDS_KNOWN || *first > size)
		return EINVAL;
	if (*count > size - *first)
		return EINVAL;

	/* Succeeded: the block can be read. */
	return 0;
}

/* Copies the capability words of a block whose layout block_layout() accepted. */
static void
decode_words(
	const uint32_t *words,
	struct ghci_caps *caps)
{
	uint32_t resident;
	uint32_t subsystem;

	/* The capability words as they are. */
	caps->block_version_read = words[GHCI_CAPS_WORD_INFO] & GHCI_CAPS_VERSION_MASK;
	caps->device_caps = words[GHCI_CAPS_WORD_DEVICE_CAPS];
	caps->feature = words[GHCI_CAPS_WORD_FEATURE];
	caps->fmt0 = words[GHCI_CAPS_WORD_FMT0];
	caps->native_isa = words[GHCI_CAPS_WORD_NATIVE_ISA_ID];
	caps->gclk_hz = words[GHCI_CAPS_WORD_GCLK_HZ];
	caps->device_version = words[GHCI_CAPS_WORD_DEVICE_VERSION];
	caps->cache_config = words[GHCI_CAPS_WORD_MEM_CACHE_CFG];
	caps->prot_caps = words[GHCI_CAPS_WORD_PROT_CAPS];
	caps->perf_src = words[GHCI_CAPS_WORD_PERF_SRC];
	caps->extcap_present = words[GHCI_CAPS_WORD_EXTCAP_PRESENT];
	caps->shader_stages = words[GHCI_CAPS_WORD_SHADER_STAGES];
	caps->stage_limits = words[GHCI_CAPS_WORD_STAGE_LIMITS];
	caps->fmt_limits = words[GHCI_CAPS_WORD_FMT_LIMITS] & GHCI_CAPS_FMT_LIMITS_MASK;

	/* The identification for the log: the PCI-style IDs and the subsystem's two halves. */
	subsystem = words[GHCI_CAPS_WORD_SUBSYSTEM];
	caps->vendor[0] = words[GHCI_CAPS_WORD_VENDOR_ID];
	caps->vendor[1] = words[GHCI_CAPS_WORD_DEVICE_ID];
	caps->vendor[2] = subsystem & 0xFFFFU;
	caps->vendor[3] = subsystem >> 16;

	/* Splits RESIDENT_WARPS into the warp capacity and the registers per lane. */
	resident = words[GHCI_CAPS_WORD_RESIDENT_WARPS];
	caps->resident_warps = resident & 0xFFFFU;
	caps->lane_registers = (resident >> 16) & 0xFFU;

	/* Reads the shader unit count from DEVICE_CAPS. */
	caps->num_su = (caps->device_caps >> GHCI_CAP_NUM_SU_SHIFT) & 0xFFU;

	/* A MAX_MRT of 0 reads as one colour attachment. */
	caps->max_mrt = (caps->feature >> GHCI_FEATURE_MAX_MRT_SHIFT) & 0xFU;
	if (caps->max_mrt == 0U)
		caps->max_mrt = 1U;

	/* EXT_CAP_FMT0's copy of the colour attachments, for the log; MAX_MRT stays the authority. */
	caps->max_color = (caps->fmt0 >> GHCI_FMT0_MAX_COLOR_SHIFT) & GHCI_FMT0_MAX_COLOR_MASK;

	/* The shared memory of a shader unit, in KiB in the word. */
	caps->lds_bytes = ((caps->feature >> GHCI_FEATURE_LDS_KB_SHIFT) & 0xFFU) * 1024U;
}

/*
 * Records each copy of a fact that disagrees with the word the driver
 * keeps as its authority (struct ghci_caps).
 */
static void
check_copies(
	struct ghci_caps *caps)
{
	uint32_t unified;
	uint32_t va_bits;
	uint32_t pa_bits;
	uint32_t mirror;
	uint32_t pair;
	int differs;

	/* BOOT_CAPS's walker and PROT against DEVICE_CAPS. */
	differs = bit_disagrees(caps->boot_caps & GHCI_BOOT_HW_WALKER, caps->device_caps & GHCI_CAP_HW_WALKER);
	if (differs)
		caps->mismatches |= GHCI_CAP_HW_WALKER;
	differs = bit_disagrees(caps->boot_caps & GHCI_BOOT_PROT, caps->device_caps & GHCI_CAP_PROT);
	if (differs)
		caps->mismatches |= GHCI_CAP_PROT;

	/* The list's DISP2 against DEVICE_CAPS's display, MEM's local memory against its UMA. */
	differs = bit_disagrees(caps->blocks & (1U << GHCI_EXTCAP_ID_DISP2), caps->device_caps & GHCI_CAP_DISP);
	if (differs)
		caps->mismatches |= GHCI_CAP_DISP;
	unified = 0U;
	if (caps->local_bytes == 0U)
		unified = 1U;
	differs = bit_disagrees(unified, caps->device_caps & GHCI_CAP_UMA);
	if (differs)
		caps->mismatches |= GHCI_CAP_UMA;

	/* The mirrors of DEVICE_CAPS bits in EXT_CAP_FEATURE, EXT_CAP_FMT0 and SHADER_STAGES. */
	for (pair = 0U; pair < MIRROR_PAIRS; pair++) {
		mirror = mirror_word(caps, caps_mirrors[pair].word);
		differs = bit_disagrees(caps->device_caps & caps_mirrors[pair].device_bit, mirror & caps_mirrors[pair].mirror_bit);
		if (differs)
			caps->mismatches |= caps_mirrors[pair].device_bit;
	}

	/*
	 * EXT_CAP_FEATURE's address widths against BOOT_CAPS's.
	 *
	 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md
	 * (section 3.2.2, VA_BITS = 30 + [3:0], PA_BITS = 40 + [7:4]).
	 */
	va_bits = GHCI_FEATURE_VA_BITS_BASE + (caps->feature & GHCI_FEATURE_VA_BITS_MASK);
	pa_bits = 40U + ((caps->feature >> GHCI_FEATURE_PA_BITS_SHIFT) & 0xFU);
	if (va_bits != caps->va_bits)
		caps->width_mismatch = 1;
	if (pa_bits != caps->pa_bits)
		caps->width_mismatch = 1;
}

/*
 * Takes as absent the geometry shader and tessellation of a device whose
 * shader unit has less shared memory than they need, which the device
 * should not report; the dropped bits stay in stages_dropped for the log.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.4,
 * CAP_GEOMETRY_SHADER and CAP_TESSELLATION MUST report 0 where
 * CAP_LDS_SIZE_KB < 16).
 */
static void
check_stages(
	struct ghci_caps *caps)
{
	/* Enough shared memory keeps every stage the device reports. */
	caps->stages_dropped = 0U;
	if (caps->lds_bytes >= GHCI_STAGES_LDS_BYTES_MIN)
		return;

	/* Less drops the stages that need it. */
	caps->stages_dropped = caps->shader_stages & (GHCI_STAGE_GEOMETRY_SHADER | GHCI_STAGE_TESSELLATION);
}

/* Reads the decoded copy of a capability word that mirrors DEVICE_CAPS bits. */
static uint32_t
mirror_word(
	const struct ghci_caps *caps,
	uint32_t word)
{
	/* Picks the word by its position in the block. */
	switch (word) {
	case GHCI_CAPS_WORD_FMT0:
		return caps->fmt0;
	case GHCI_CAPS_WORD_SHADER_STAGES:
		return caps->shader_stages;
	default:
		break;
	}

	/* Succeeded: EXT_CAP_FEATURE, the word of the other mirrors. */
	return caps->feature;
}

/* Reports whether a copy of a bit says otherwise than its authority. */
static int
bit_disagrees(
	uint32_t authority,
	uint32_t copy)
{
	/* Both set or both clear agree. */
	if (authority != 0U && copy != 0U)
		return 0;
	if (authority == 0U && copy == 0U)
		return 0;

	/* Succeeded: the copy disagrees. */
	return 1;
}
