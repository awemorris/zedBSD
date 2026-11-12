/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a GHCI 1.0 GPU can do, decoded from a copy of its register
 * window: the identification, the capability registers, the extended
 * capability list and its blocks, and the sizes the driver derives from
 * them (the page table heap, the watchdog and the reset wait).
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "ghci-regs.h"

/* The bits of DEVICE_CAPS that EXT_CAP_FEATURE and EXT_CAP_FMT0 mirror. */
#define MIRROR_PAIRS			3U

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

/* A DEVICE_CAPS bit, the register that mirrors it and the mirror bit. */
struct caps_mirror {
	uint32_t device_bit;
	uint32_t reg;
	uint32_t mirror_bit;
};

/*
 * The DEVICE_CAPS bits and their mirrors.
 *
 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section 3.2.2,
 * EXT_CAP_FEATURE CAP_DUAL_SOURCE [8] and CAP_FP16 [28]; EXT_CAP_FMT0
 * CAP_FFILT_FP16 [10], which is CAP_TEX_FFILT).
 */
static const struct caps_mirror caps_mirrors[MIRROR_PAIRS] = {
	{ GHCI_CAP_DUAL_SOURCE, GHCI_REG_EXT_CAP_FEATURE, GHCI_FEATURE_DUAL_SOURCE },
	{ GHCI_CAP_FP16, GHCI_REG_EXT_CAP_FEATURE, GHCI_FEATURE_FP16 },
	{ GHCI_CAP_TEX_FFILT, GHCI_REG_EXT_CAP_FMT0, GHCI_FMT0_FFILT_FP16 }
};

static uint32_t window_word(const uint32_t *window, uint32_t offset);
static void decode_blocks(const uint32_t *window, struct ghci_caps *caps);

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
 * Decodes what a GPU can do from a copy of its register window.
 *
 * The extended capability list is walked first; a list that is malformed
 * leaves the device without blocks rather than refusing it.
 */
int
ghci_caps_decode(
	const uint32_t *window,
	struct ghci_caps *caps)
{
	uint32_t resident;
	uint32_t in_caps;
	uint32_t in_mirror;
	uint32_t mirror;
	uint32_t found;
	uint32_t pair;
	int error;

	/* Refuses a window that is not a GHCI 1.0 GPU. */
	error = ghci_caps_identify(window);
	if (error != 0)
		return error;

	/* Clears the record so that a block the list does not name reads as absent. */
	*caps = (struct ghci_caps){ 0 };

	/* Copies the capability registers. */
	caps->device_caps = window_word(window, GHCI_REG_DEVICE_CAPS);
	caps->feature = window_word(window, GHCI_REG_EXT_CAP_FEATURE);
	caps->fmt0 = window_word(window, GHCI_REG_EXT_CAP_FMT0);
	caps->native_isa = window_word(window, GHCI_REG_NATIVE_ISA_ID);
	caps->gclk_hz = window_word(window, GHCI_REG_GCLK_HZ);

	/* Splits RESIDENT_WARPS into the warp capacity and the registers per lane. */
	resident = window_word(window, GHCI_REG_RESIDENT_WARPS);
	caps->resident_warps = resident & 0xFFFFU;
	caps->lane_registers = (resident >> 16) & 0xFFU;

	/* Reads the shader unit count from DEVICE_CAPS. */
	caps->num_su = (caps->device_caps >> GHCI_CAP_NUM_SU_SHIFT) & 0xFFU;

	/*
	 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md
	 * (section 3.2.2, VA_BITS = 30 + [3:0], PA_BITS = 40 + [7:4]).
	 */
	caps->va_bits = GHCI_FEATURE_VA_BITS_BASE + (caps->feature & GHCI_FEATURE_VA_BITS_MASK);
	caps->pa_bits = 40U + ((caps->feature >> GHCI_FEATURE_PA_BITS_SHIFT) & 0xFU);

	/* A MAX_MRT of 0 reads as one colour attachment. */
	caps->max_mrt = (caps->feature >> GHCI_FEATURE_MAX_MRT_SHIFT) & 0xFU;
	if (caps->max_mrt == 0U)
		caps->max_mrt = 1U;

	/* EXT_CAP_FMT0's copy of the colour attachments, for the log; MAX_MRT stays the authority. */
	caps->max_color = (caps->fmt0 >> GHCI_FMT0_MAX_COLOR_SHIFT) & GHCI_FMT0_MAX_COLOR_MASK;

	/* The shared memory of a shader unit, in KiB in the register. */
	caps->lds_bytes = ((caps->feature >> GHCI_FEATURE_LDS_KB_SHIFT) & 0xFFU) * 1024U;

	/* Records each mirror that disagrees with DEVICE_CAPS, which stays the authority. */
	for (pair = 0U; pair < MIRROR_PAIRS; pair++) {
		mirror = caps->feature;
		if (caps_mirrors[pair].reg == GHCI_REG_EXT_CAP_FMT0)
			mirror = caps->fmt0;
		in_caps = caps->device_caps & caps_mirrors[pair].device_bit;
		in_mirror = mirror & caps_mirrors[pair].mirror_bit;
		if ((in_caps != 0U) != (in_mirror != 0U))
			caps->mismatches |= caps_mirrors[pair].device_bit;
	}

	/* Walks the extended capability list; a malformed list leaves no blocks. */
	error = ghci_extcap_walk(window, caps->block_offset, caps->block_version, &found);
	if (error != 0) {
		caps->blocks = 0U;
		return 0;
	}

	/* Reads the blocks the list names. */
	caps->blocks = found;
	decode_blocks(window, caps);

	/* Succeeded: the record holds what the device can do. */
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
		bit = caps->device_caps & GHCI_CAP_UMA;
		break;
	case GHCI_HAS_HW_WALKER:
		bit = caps->device_caps & GHCI_CAP_HW_WALKER;
		break;
	case GHCI_HAS_PROT:
		bit = caps->device_caps & GHCI_CAP_PROT;
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
		/* The display needs both the flag and the DISP2 block. */
		bit = caps->device_caps & GHCI_CAP_DISP;
		if ((caps->blocks & (1U << GHCI_EXTCAP_ID_DISP2)) == 0U)
			bit = 0U;
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
		/*
		 * Discrete needs UMA clear and local memory in MEM; a GPU
		 * without MEM is unified whatever DEVICE_CAPS says.
		 */
		bit = 0U;
		if ((caps->device_caps & GHCI_CAP_UMA) == 0U && caps->local_bytes != 0U)
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

/* Reads the registers of the MEM, VEND and PROT blocks the list names. */
static void
decode_blocks(
	const uint32_t *window,
	struct ghci_caps *caps)
{
	uint32_t base;

	/* Reads the memory configuration; a GPU without MEM is unified. */
	if ((caps->blocks & (1U << GHCI_EXTCAP_ID_MEM)) != 0U) {
		base = caps->block_offset[GHCI_EXTCAP_ID_MEM];
		caps->local_base = window_word(window, base + GHCI_MEM_LOCAL_BASE);
		caps->local_bytes = window_word(window, base + GHCI_MEM_LOCAL_SIZE);
		caps->window_bytes = window_word(window, base + GHCI_MEM_WINDOW_SIZE);
		caps->host_limit = window_word(window, base + GHCI_MEM_HOST_LIMIT);
		caps->cache_config = window_word(window, base + GHCI_MEM_CACHE_CFG);
	}

	/* Reads the vendor identification for the log. */
	if ((caps->blocks & (1U << GHCI_EXTCAP_ID_VEND)) != 0U) {
		base = caps->block_offset[GHCI_EXTCAP_ID_VEND];
		caps->vendor[0] = window_word(window, base + GHCI_VEND_VENDOR_ID);
		caps->vendor[1] = window_word(window, base + GHCI_VEND_DEVICE_ID);
		caps->vendor[2] = window_word(window, base + GHCI_VEND_SUBSYSTEM_VENDOR);
		caps->vendor[3] = window_word(window, base + GHCI_VEND_SUBSYSTEM_DEVICE);
	}

	/* Reads the session protection capabilities for the log. */
	if ((caps->blocks & (1U << GHCI_EXTCAP_ID_PROT)) != 0U) {
		base = caps->block_offset[GHCI_EXTCAP_ID_PROT];
		caps->prot_caps = window_word(window, base + GHCI_PROT_CAPS);
	}
}
