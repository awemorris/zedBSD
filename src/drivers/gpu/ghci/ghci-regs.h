/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register window of a GHCI 1.0 GPU: the offsets and bits of the
 * registers the driver reads and writes, the extended capability list,
 * the blocks it points to, and the record the driver keeps of what one
 * device can do.
 *
 * Nothing declared here touches the hardware.  The functions take a copy
 * of the register window and answer from it, so a host test calls them
 * exactly as the kernel does.
 */

#ifndef DRIVERS_GPU_GHCI_REGS_H
#define DRIVERS_GPU_GHCI_REGS_H

#include <stdint.h>

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 3.1, Canonical Register Map).
 */
#define GHCI_REG_MAGIC			0x000U
#define GHCI_REG_VERSION		0x004U
#define GHCI_REG_DEVICE_CAPS		0x008U
#define GHCI_REG_EXTCAP_PTR		0x00CU
#define GHCI_REG_STATUS			0x010U
#define GHCI_REG_CTRL			0x014U
#define GHCI_REG_FAULT_INFO		0x018U
#define GHCI_REG_FAULT_ADDR_LO		0x01CU
#define GHCI_REG_FAULT_ADDR_HI		0x020U
#define GHCI_REG_RING_BASE_LO		0x024U
#define GHCI_REG_RING_BASE_HI		0x028U
#define GHCI_REG_RING_SIZE		0x02CU
#define GHCI_REG_RING_WPTR		0x030U
#define GHCI_REG_RING_RPTR		0x034U
#define GHCI_REG_IRQ_STATUS		0x038U
#define GHCI_REG_IRQ_ENABLE		0x03CU
#define GHCI_REG_IRQ_SET		0x040U
#define GHCI_REG_SCRATCH		0x044U
#define GHCI_REG_WDT_CYCLES		0x048U
#define GHCI_REG_RESIDENT_WARPS		0x04CU
#define GHCI_REG_CP_HEAP_BASE		0x050U
#define GHCI_REG_CP_HEAP_LIMIT		0x054U
#define GHCI_REG_SESSION_ACTIVE		0x058U
#define GHCI_REG_TIMESTAMP_LO		0x05CU
#define GHCI_REG_TIMESTAMP_HI		0x060U
#define GHCI_REG_GCLK_HZ		0x064U
#define GHCI_REG_NATIVE_ISA_ID		0x068U
#define GHCI_REG_DIAG_CP_PC		0x074U
#define GHCI_REG_DIAG_CP_STATUS		0x078U
#define GHCI_REG_EXT_CAP_FMT0		0x080U
#define GHCI_REG_EXT_CAP_FEATURE	0x084U

/* The bytes of the window the driver uses: the registers and the extended capabilities. */
#define GHCI_WINDOW_BYTES		0x400U

/* The words of that window, as a copy of it is kept. */
#define GHCI_WINDOW_WORDS		(GHCI_WINDOW_BYTES / 4U)

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 3.1, GHCI_MAGIC, GHCI_VERSION and NATIVE_ISA_ID).
 */
#define GHCI_MAGIC_VALUE		0x47484349U
#define GHCI_VERSION_MAJOR_SHIFT	16U
#define GHCI_VERSION_MAJOR		1U
#define GHCI_NATIVE_ISA_GSI1		0x47534931U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 3.2.1, DEVICE_CAPS).
 */
#define GHCI_CAP_UMA			(1U << 0)
#define GHCI_CAP_IOMMU			(1U << 1)
#define GHCI_CAP_HW_WALKER		(1U << 2)
#define GHCI_CAP_INDIRECT_DRAW		(1U << 3)
#define GHCI_CAP_SECONDARY_RING		(1U << 4)
#define GHCI_CAP_DUAL_SOURCE		(1U << 5)
#define GHCI_CAP_FP16			(1U << 6)
#define GHCI_CAP_GSI1			(1U << 7)
#define GHCI_CAP_PHYS_LANES_SHIFT	8U
#define GHCI_CAP_NUM_SU_SHIFT		16U
#define GHCI_CAP_PROT			(1U << 24)
#define GHCI_CAP_TEX_FFILT		(1U << 25)
#define GHCI_CAP_ROP_PACK		(1U << 26)
#define GHCI_CAP_TASK_SHADER		(1U << 27)
#define GHCI_CAP_MESH_SHADER		(1U << 28)
#define GHCI_CAP_BLT			(1U << 29)
#define GHCI_CAP_DISP			(1U << 30)
#define GHCI_CAP_MSAA			(1U << 31)

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 3.2.2, EXT_CAP_FEATURE and EXT_CAP_FMT0).
 */
#define GHCI_FEATURE_VA_BITS_MASK	0x0000000FU
#define GHCI_FEATURE_PA_BITS_SHIFT	4U
#define GHCI_FEATURE_DUAL_SOURCE	(1U << 8)
#define GHCI_FEATURE_COARSE_SCAN	(1U << 9)
#define GHCI_FEATURE_FAST_CLEAR		(1U << 10)
#define GHCI_FEATURE_LOGIC_OP		(1U << 11)
#define GHCI_FEATURE_EARLY_Z		(1U << 12)
#define GHCI_FEATURE_HW_CURSOR		(1U << 13)
#define GHCI_FEATURE_COLOR_LUT		(1U << 14)
#define GHCI_FEATURE_MRT_BLEND		(1U << 15)
#define GHCI_FEATURE_MAX_MRT_SHIFT	16U
#define GHCI_FEATURE_LDS_KB_SHIFT	20U
#define GHCI_FEATURE_FP16		(1U << 28)
#define GHCI_FEATURE_SUBGROUP		(1U << 29)
#define GHCI_FMT0_BC_MASK		0x0000007FU
#define GHCI_FMT0_ETC2			(1U << 7)
#define GHCI_FMT0_SRGB			(1U << 9)

/*
 * The VA width is VA_BITS_ENC + 30, the float filters of EXT_CAP_FMT0,
 * its copy of the colour attachment count, and the blocks a
 * configuration may leave out: the polygon clipper, the blitter's
 * scaling, blending and tiling, and the texture unit's bilinear filter.
 *
 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section 3.2.2,
 * EXT_CAP_FEATURE [3:0] VA_BITS_ENC (VA - 30), [30] CAP_CLIP_POLY and
 * [31] CAP_BLT_SCALE; EXT_CAP_FMT0 [10] CAP_FFILT_FP16, [11]
 * CAP_FFILT_FP32, [15:12] MAX_COLOR_ATTACHMENTS, [16] CAP_TU_BILINEAR,
 * [17] CAP_BLT_BLEND and [18] CAP_BLT_TILE).
 */
#define GHCI_FEATURE_VA_BITS_BASE	30U
#define GHCI_FEATURE_CLIP_POLY		(1U << 30)
#define GHCI_FEATURE_BLT_SCALE		(1U << 31)
#define GHCI_FMT0_FFILT_FP16		(1U << 10)
#define GHCI_FMT0_FFILT_FP32		(1U << 11)
#define GHCI_FMT0_MAX_COLOR_SHIFT	12U
#define GHCI_FMT0_MAX_COLOR_MASK	0xFU
#define GHCI_FMT0_TU_BILINEAR		(1U << 16)
#define GHCI_FMT0_BLT_BLEND		(1U << 17)
#define GHCI_FMT0_BLT_TILE		(1U << 18)

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 3.3, CTRL and STATUS).
 */
#define GHCI_CTRL_RESET			(1U << 0)
#define GHCI_CTRL_RUN			(1U << 1)
#define GHCI_CTRL_HALT			(1U << 2)
#define GHCI_CTRL_STEP			(1U << 3)
#define GHCI_CTRL_WDT_EN		(1U << 5)
#define GHCI_CTRL_IRQ_GLOBAL		(1U << 6)
#define GHCI_STATUS_RUNNING		(1U << 0)
#define GHCI_STATUS_HALTED		(1U << 1)
#define GHCI_STATUS_FAULTED		(1U << 2)
#define GHCI_STATUS_MC_IDLE		(1U << 8)
#define GHCI_STATUS_RING_EMPTY		(1U << 11)
#define GHCI_STATUS_MEM_READY		(1U << 12)
#define GHCI_STATUS_AXI_STUCK		(1U << 13)
#define GHCI_STATUS_QUIESCE		(1U << 14)
#define GHCI_STATUS_CP_STATE_SHIFT	16U
#define GHCI_STATUS_CP_STATE_MASK	0xFU

/*
 * Values referenced: amaterrace-gpu 20bf888 docs/ghci.md
 * (section 3.4, IRQ_STATUS and IRQ_ENABLE).
 */
#define GHCI_IRQ_FENCE			(1U << 0)
#define GHCI_IRQ_FAULT			(1U << 1)
#define GHCI_IRQ_WATCHDOG		(1U << 2)
#define GHCI_IRQ_DISPLAY		(1U << 3)
#define GHCI_IRQ_SOFT			(1U << 5)
#define GHCI_IRQ_HALTED			(1U << 6)

/*
 * Values referenced: amaterrace-gpu 20bf888 docs/ghci.md
 * (section 3.5, FAULT_INFO kinds and units; bit 20 tells a write
 * page fault from a read one).
 */
#define GHCI_FAULT_KIND_MASK		0xFU
#define GHCI_FAULT_UNIT_SHIFT		4U
#define GHCI_FAULT_UNIT_MASK		0xFU
#define GHCI_FAULT_SESSION_SHIFT	8U
#define GHCI_FAULT_SESSION_MASK		0xFFU
#define GHCI_FAULT_STAGE_SHIFT		16U
#define GHCI_FAULT_STAGE_MASK		0xFU
#define GHCI_FAULT_SYNDROME_SHIFT	20U
#define GHCI_FAULT_SYNDROME_MASK	0xFFFU
#define GHCI_FAULT_WRITE		(1U << 20)
#define GHCI_FAULT_KIND_NONE		0U
#define GHCI_FAULT_KIND_INVALID_OP	1U
#define GHCI_FAULT_KIND_KICK_ABSENT	2U
#define GHCI_FAULT_KIND_BAD_REG		3U
#define GHCI_FAULT_KIND_WATCHDOG	4U
#define GHCI_FAULT_KIND_AXI_ERROR	5U
#define GHCI_FAULT_KIND_PROT_FAULT	6U
#define GHCI_FAULT_KIND_LOCKED_REG	7U
#define GHCI_FAULT_KIND_MMU_OOM		8U
#define GHCI_FAULT_KIND_PAGE_FAULT	9U
#define GHCI_FAULT_KIND_PERM_FAULT	10U
#define GHCI_UNIT_CP			0U
#define GHCI_UNIT_VF			1U
#define GHCI_UNIT_SU			2U
#define GHCI_UNIT_RX			3U
#define GHCI_UNIT_TU			4U
#define GHCI_UNIT_ROP			5U
#define GHCI_UNIT_BLT			6U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 4 and 4.1, the extended capability list).
 */
#define GHCI_EXTCAP_ID_DISP		1U
#define GHCI_EXTCAP_ID_DBG		2U
#define GHCI_EXTCAP_ID_PERF		3U
#define GHCI_EXTCAP_ID_VEND		4U
#define GHCI_EXTCAP_ID_MMU		5U
#define GHCI_EXTCAP_ID_CPFW		6U
#define GHCI_EXTCAP_ID_SA		7U
#define GHCI_EXTCAP_ID_MEM		8U
#define GHCI_EXTCAP_ID_PROT		9U
#define GHCI_EXTCAP_ID_FMT		10U
#define GHCI_EXTCAP_ID_DISP2		11U
#define GHCI_EXTCAP_LIST_FIRST		0x100U
#define GHCI_EXTCAP_LIST_END		0x140U
#define GHCI_EXTCAP_ENTRIES_MAX		8U
#define GHCI_EXTCAP_BLOCK_FIRST		0x100U

/* The capability IDs the driver records, 1 to 11. */
#define GHCI_EXTCAP_IDS			12U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 4.1, VEND; section 11.3, MEM; section 4.1, PROT).
 */
#define GHCI_VEND_VENDOR_ID		0x00U
#define GHCI_VEND_DEVICE_ID		0x04U
#define GHCI_VEND_SUBSYSTEM_VENDOR	0x08U
#define GHCI_VEND_SUBSYSTEM_DEVICE	0x0CU
#define GHCI_MEM_LOCAL_BASE		0x00U
#define GHCI_MEM_LOCAL_SIZE		0x04U
#define GHCI_MEM_WINDOW_SIZE		0x08U
#define GHCI_MEM_HOST_LIMIT		0x0CU
#define GHCI_MEM_WINDOW_CTRL		0x10U
#define GHCI_MEM_CACHE_CFG		0x14U
#define GHCI_PROT_CAPS			0x00U

/*
 * The DISP2 block: the registers of a display controller that is a unit
 * of the GPU, at offsets from the block (a window of 256 bytes), with
 * their fields.  The same offsets name the registers in CMD_DISP_SET.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md
 * (section 13.2, the DISP2 registers, DISP_IRQ_STATUS and DC_CAPS).
 */
#define GHCI_DC_WINDOW			0x100U
#define GHCI_DC_BLOCK_VERSION		1U
#define GHCI_DC_ID			0x00U
#define GHCI_DC_VERSION			0x04U
#define GHCI_DC_CAPS			0x08U
#define GHCI_DC_CTRL			0x10U
#define GHCI_DC_STATUS			0x14U
#define GHCI_DC_IRQ_STATUS		0x18U
#define GHCI_DC_IRQ_ENABLE		0x1CU
#define GHCI_DC_FB_BASE			0x20U
#define GHCI_DC_FB_NEXT			0x24U
#define GHCI_DC_FB_STRIDE		0x28U
#define GHCI_DC_FB_SIZE			0x2CU
#define GHCI_DC_H_ACTIVE		0x30U
#define GHCI_DC_H_FP			0x34U
#define GHCI_DC_H_SYNC			0x38U
#define GHCI_DC_H_BP			0x3CU
#define GHCI_DC_V_ACTIVE		0x40U
#define GHCI_DC_V_FP			0x44U
#define GHCI_DC_V_SYNC			0x48U
#define GHCI_DC_V_BP			0x4CU
#define GHCI_DC_SYNC_POL		0x50U
#define GHCI_DC_FRAME_COUNT		0x54U
#define GHCI_DC_PCLK_SEL		0x58U
#define GHCI_DC_I2C_CTRL		0x60U
#define GHCI_DC_I2C_DATA		0x64U
#define GHCI_DC_I2C_STATUS		0x68U
#define GHCI_DC_CONNECTED		0x6CU
#define GHCI_DC_MODE_INDEX		0x80U
#define GHCI_DC_MODE_H0			0x84U
#define GHCI_DC_MODE_H1			0x88U
#define GHCI_DC_MODE_V0			0x8CU
#define GHCI_DC_MODE_V1			0x90U
#define GHCI_DC_MODE_CLOCK		0x94U
#define GHCI_DC_MODE_FLAGS		0x98U
#define GHCI_DC_CURSOR_BASE		0xA0U
#define GHCI_DC_CURSOR_POS		0xA4U
#define GHCI_DC_CURSOR_CTRL		0xA8U
#define GHCI_DC_TAKEN0			0xB0U
#define GHCI_DC_TAKEN1			0xB4U
#define GHCI_DC_CAPS_TMDS		(1U << 1)
#define GHCI_DC_CAPS_TRANSMITTER	(1U << 2)
#define GHCI_DC_CAPS_BGRA8		(1U << 4)
#define GHCI_DC_CAPS_RGBA8		(1U << 5)
#define GHCI_DC_CAPS_CURSOR		(1U << 8)
#define GHCI_DC_CAPS_MODES_SHIFT	9U
#define GHCI_DC_CAPS_MODES_MASK		0x7U
#define GHCI_DC_CAPS_SENSE		(1U << 12)
#define GHCI_DC_CTRL_ENABLE		(1U << 0)
#define GHCI_DC_CTRL_RESET		(1U << 1)
#define GHCI_DC_CTRL_FORMAT_SHIFT	4U
#define GHCI_DC_CTRL_FORMAT_MASK	0x3U
#define GHCI_DC_CTRL_OUTPUT_HDMI	(1U << 6)
#define GHCI_DC_FMT_BGRA8		0U
#define GHCI_DC_FMT_RGBA8		1U
#define GHCI_DC_ST_RUNNING		(1U << 0)
#define GHCI_DC_ST_UNDERFLOW		(1U << 1)
#define GHCI_DC_ST_IDLE			(1U << 2)
#define GHCI_DC_ST_READ_ERROR		(1U << 3)
#define GHCI_DC_ST_RECORD_ERROR		(1U << 4)
#define GHCI_DC_ST_BAD_WRITE		(1U << 5)
#define GHCI_DC_ST_CONNECTED		(1U << 28)
#define GHCI_DC_ST_PENDING0		(1U << 29)
#define GHCI_DC_ST_PENDING1		(1U << 30)
#define GHCI_DC_ST_RECORD		(1U << 31)
#define GHCI_DC_IRQ_VSYNC		(1U << 0)
#define GHCI_DC_IRQ_FLIP_DONE		(1U << 1)
#define GHCI_DC_IRQ_CURSOR_DONE		(1U << 2)
#define GHCI_DC_IRQ_HOTPLUG		(1U << 3)
#define GHCI_DC_IRQ_UNDERFLOW		(1U << 4)
#define GHCI_DC_IRQ_ERROR		(1U << 5)
#define GHCI_DC_IRQ_ALL			0x3FU
#define GHCI_DC_SIZE_HEIGHT_SHIFT	16U
#define GHCI_DC_MODE_PCLK_SHIFT		4U
#define GHCI_DC_MODE_PCLK_MASK		0xFU
#define GHCI_DC_MODE_SYNC_MASK		0x3U
#define GHCI_DC_TAKEN_CANCELLED		(1U << 31)
#define GHCI_DC_CURSOR_SIZE		64U
#define GHCI_DC_RECORD_BYTES		16U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 6.1, RING_SIZE).
 */
#define GHCI_RING_BYTES_MIN		0x1000U
#define GHCI_RING_BYTES_MAX		0x1000000U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 11.2, the multi-level page table and CP_HEAP).
 */
#define GHCI_PAGE_BYTES			4096U
#define GHCI_ROOT_BYTES_VA32		(16U * 1024U)
#define GHCI_ROOT_BYTES_VA40		(8U * 1024U)
#define GHCI_TABLE_BYTES		4096U
#define GHCI_L0_SPAN_BYTES		(2U * 1024U * 1024U)
#define GHCI_L1_SPAN_BYTES		(1024U * 1024U * 1024U)
#define GHCI_HEAP_SPARE_BYTES		(64U * 1024U)

/* The sessions of a GPU: 0 is the driver's, 1 to 15 are clients'. */
#define GHCI_SESSIONS			16U

/* The 2 MiB chunks of the 32-bit GPU address range, the most the driver uses. */
#define GHCI_CHUNKS			2048U

/*
 * What one device can do, read once at attach and not changed after.
 *
 * DEVICE_CAPS is the authority; the mirrors in EXT_CAP_FEATURE and
 * EXT_CAP_FMT0 only fill in what DEVICE_CAPS does not hold, and a mirror
 * that disagrees is kept in mismatches for the log.  MAX_MRT is the
 * authority on the colour attachments; max_color is EXT_CAP_FMT0's copy,
 * for the log.  A block the list does not name has offset 0.
 */
struct ghci_caps {
	uint32_t device_caps;
	uint32_t feature;
	uint32_t fmt0;
	uint32_t native_isa;
	uint32_t gclk_hz;
	uint32_t resident_warps;
	uint32_t lane_registers;
	uint32_t num_su;
	uint32_t va_bits;
	uint32_t pa_bits;
	uint32_t max_mrt;
	uint32_t max_color;
	uint32_t lds_bytes;
	uint32_t blocks;
	uint32_t block_offset[GHCI_EXTCAP_IDS];
	uint32_t block_version[GHCI_EXTCAP_IDS];
	uint64_t local_base;
	uint64_t local_bytes;
	uint64_t window_bytes;
	uint32_t host_limit;
	uint32_t cache_config;
	uint32_t prot_caps;
	uint32_t vendor[4];
	uint32_t mismatches;
};

/*
 * The capability flags whose meaning the driver decides on: one bit per
 * question, so that no other file looks at a register bit.
 */
enum ghci_caps_flag {
	GHCI_HAS_UMA = 0,
	GHCI_HAS_HW_WALKER,
	GHCI_HAS_PROT,
	GHCI_HAS_INDIRECT_DRAW,
	GHCI_HAS_DUAL_SOURCE,
	GHCI_HAS_GSI1,
	GHCI_HAS_SHADER_UNITS,
	GHCI_HAS_BLT,
	GHCI_HAS_DISPLAY,
	GHCI_HAS_MSAA,
	GHCI_HAS_TEX_FFILT,
	GHCI_HAS_ROP_PACK,
	GHCI_HAS_LOGIC_OP,
	GHCI_HAS_MRT_BLEND,
	GHCI_HAS_HW_CURSOR,
	GHCI_HAS_ETC2,
	GHCI_HAS_SRGB,
	GHCI_HAS_DISCRETE,
	GHCI_HAS_FFILT_FP32,
	GHCI_HAS_CLIP_POLY,
	GHCI_HAS_TU_BILINEAR,
	GHCI_HAS_BLT_SCALE,
	GHCI_HAS_BLT_BLEND,
	GHCI_HAS_BLT_TILE
};

int ghci_caps_identify(const uint32_t *window);
int ghci_caps_decode(const uint32_t *window, struct ghci_caps *caps);
int ghci_caps_has(const struct ghci_caps *caps, enum ghci_caps_flag flag);
int ghci_extcap_walk(const uint32_t *window, uint32_t *offsets, uint32_t *versions, uint32_t *found);
uint32_t ghci_heap_session_bytes(uint32_t va_bits);
uint64_t ghci_heap_device_bytes(uint32_t va_bits);
uint32_t ghci_va_chunks(uint32_t va_bits);
uint32_t ghci_watchdog_cycles(uint32_t gclk_hz, uint32_t milliseconds);
uint32_t ghci_reset_wait_ms(uint32_t gclk_hz, uint32_t wdt_cycles);

#endif
