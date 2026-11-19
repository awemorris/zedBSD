/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register window of a GHCI 1.0 GPU: the offsets and bits of the
 * registers the driver reads and writes, the extended capability list,
 * the blocks it points to, the capability block the command processor
 * writes to memory, and the record the driver keeps of what one device
 * can do.
 *
 * Nothing declared here touches the hardware.  The functions take a copy
 * of the register window or of the capability block and answer from it,
 * so a host test calls them exactly as the kernel does.
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
#define GHCI_REG_BOOT_CAPS		0x008U
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
#define GHCI_REG_CP_HEAP_BASE		0x050U
#define GHCI_REG_CP_HEAP_LIMIT		0x054U
#define GHCI_REG_SESSION_ACTIVE		0x058U
#define GHCI_REG_TIMESTAMP_LO		0x05CU
#define GHCI_REG_TIMESTAMP_HI		0x060U
#define GHCI_REG_DIAG_CP_PC		0x074U
#define GHCI_REG_DIAG_CP_STATUS		0x078U

/* The bytes of the window the driver uses: the registers and the extended capabilities. */
#define GHCI_WINDOW_BYTES		0x400U

/* The words of that window, as a copy of it is kept. */
#define GHCI_WINDOW_WORDS		(GHCI_WINDOW_BYTES / 4U)

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 3.1, GHCI_MAGIC, GHCI_VERSION and NATIVE_ISA_ID's GSI1).
 */
#define GHCI_MAGIC_VALUE		0x47484349U
#define GHCI_VERSION_MAJOR_SHIFT	16U
#define GHCI_VERSION_MAJOR		1U
#define GHCI_NATIVE_ISA_GSI1		0x47534931U

/*
 * BOOT_CAPS, the one capability register: whether the GPU has a command
 * processor (and so the ring, the doorbell, the interrupt and the
 * capability block), whether it walks page tables and protects sessions,
 * and the widths of its GPU and bus addresses, which size CP_HEAP and
 * place the ring before the processor runs.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.0,
 * BOOT_CAPS: [0] CAP_BOOT_CP, [1] CAP_BOOT_HW_WALKER, [2] CAP_BOOT_PROT,
 * [11:8] VA_BITS - 30, [15:12] PA_BITS - 40).
 */
#define GHCI_BOOT_CP			(1U << 0)
#define GHCI_BOOT_HW_WALKER		(1U << 1)
#define GHCI_BOOT_PROT			(1U << 2)
#define GHCI_BOOT_VA_BITS_SHIFT		8U
#define GHCI_BOOT_PA_BITS_SHIFT		12U
#define GHCI_BOOT_BITS_MASK		0xFU
#define GHCI_BOOT_VA_BITS_BASE		30U
#define GHCI_BOOT_PA_BITS_BASE		40U

/*
 * The capability block CMD_QUERY_CAPS writes to memory: its identity,
 * its version and size, the words it carries and where the format table
 * starts, and the bytes of the block of version 1, the most the command
 * may be asked to write.  A later version adds words behind these and
 * keeps them.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.5,
 * the capability block: word 0 BLOCK_ID "GCAP", word 1 [15:0] version
 * and [31:16] the dwords, words 2 to 20, the format table from word 32,
 * the format word's [8:0] OPTIMAL, [17:9] LINEAR and [21:18] the buffer
 * features 3 to 6; section 7.3, CMD_QUERY_CAPS's dst_bytes at most 768).
 */
#define GHCI_CAPS_ID_VALUE		0x50414347U
#define GHCI_CAPS_VERSION_MASK		0xFFFFU
#define GHCI_CAPS_DWORDS_SHIFT		16U
#define GHCI_CAPS_DWORDS_ALIGN		16U
#define GHCI_CAPS_WORD_ID		0U
#define GHCI_CAPS_WORD_INFO		1U
#define GHCI_CAPS_WORD_DEVICE_CAPS	2U
#define GHCI_CAPS_WORD_FEATURE		3U
#define GHCI_CAPS_WORD_FMT0		4U
#define GHCI_CAPS_WORD_RESIDENT_WARPS	5U
#define GHCI_CAPS_WORD_NATIVE_ISA_ID	6U
#define GHCI_CAPS_WORD_GCLK_HZ		7U
#define GHCI_CAPS_WORD_VENDOR_ID	8U
#define GHCI_CAPS_WORD_DEVICE_ID	9U
#define GHCI_CAPS_WORD_SUBSYSTEM	10U
#define GHCI_CAPS_WORD_DEVICE_VERSION	11U
#define GHCI_CAPS_WORD_MEM_CACHE_CFG	12U
#define GHCI_CAPS_WORD_PROT_CAPS	13U
#define GHCI_CAPS_WORD_PERF_SRC		14U
#define GHCI_CAPS_WORD_FMT_LIMITS	15U
#define GHCI_CAPS_WORD_FMT_FIRST	16U
#define GHCI_CAPS_WORD_FMT_COUNT	17U
#define GHCI_CAPS_WORD_EXTCAP_PRESENT	18U
#define GHCI_CAPS_WORD_SHADER_STAGES	19U
#define GHCI_CAPS_WORD_STAGE_LIMITS	20U
#define GHCI_CAPS_WORDS_KNOWN		21U
#define GHCI_CAPS_BLOCK_BYTES		768U
#define GHCI_CAPS_HEADER_DWORDS		32U
#define GHCI_CAPS_FMT_OPTIMAL_MASK	0x1FFU
#define GHCI_CAPS_FMT_LINEAR_SHIFT	9U
#define GHCI_CAPS_FMT_BUFFER_SHIFT	18U
#define GHCI_CAPS_FMT_BUFFER_MASK	0xFU
#define GHCI_CAPS_FMT_BUFFER_FIRST	3U
#define GHCI_CAPS_FMT_LIMITS_MASK	0xFU

/* The VkFormats whose table words the driver keeps (0 to 184); a later format has no feature. */
#define GHCI_CAPS_FORMATS		185U

/*
 * Values referenced: amaterrace-gpu cb937e6 docs/ghci.md
 * (section 3.2.1, DEVICE_CAPS, the capability block's word 2).
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
 * (section 3.2.2, EXT_CAP_FEATURE and EXT_CAP_FMT0, the capability
 * block's words 3 and 4).
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
 * The texture unit's optional image types and filter of EXT_CAP_FMT0:
 * the layers of 1D and 2D arrays, 3D images, cube maps, and the linear
 * filter between two mip levels.  A texture of a type whose bit is 0
 * reads undefined texels, and without the filter between levels the
 * nearest level is sampled.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.2,
 * EXT_CAP_FMT0 [19] CAP_TEX_ARRAY, [20] CAP_TEX_3D, [21] CAP_TEX_CUBE,
 * [22] CAP_TEX_TRILINEAR); amaterrace-gpu ea5d39c1
 * rtl/gpu/tu/atg_tu_addr.v (the layer, 3D and cube addressing and the
 * level chosen without ENABLE_TRILINEAR).
 */
#define GHCI_FMT0_TEX_ARRAY		(1U << 19)
#define GHCI_FMT0_TEX_3D		(1U << 20)
#define GHCI_FMT0_TEX_CUBE		(1U << 21)
#define GHCI_FMT0_TEX_TRILINEAR		(1U << 22)

/*
 * The pipeline's optional shader stages and their limits, words 19 and
 * 20 of the capability block: task and mesh shaders (copies of
 * DEVICE_CAPS[27] and [28]), geometry shaders, tessellation and the
 * geometry stream output; the most vertices a geometry shader emits and
 * its invocations, and the control points of a patch and the largest
 * tessellation level.  Geometry shaders and tessellation need 16 KiB of
 * shared memory in a shader unit, and a GPU with less reports neither.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.4,
 * word 19 SHADER_STAGES [0] CAP_TASK_SHADER, [1] CAP_MESH_SHADER, [2]
 * CAP_GEOMETRY_SHADER, [3] CAP_TESSELLATION, [4] CAP_STREAM_OUT; word 20
 * STAGE_LIMITS [8:0], [14:9], [20:15], [27:21]; CAP_LDS_SIZE_KB >= 16).
 */
#define GHCI_STAGE_TASK_SHADER		(1U << 0)
#define GHCI_STAGE_MESH_SHADER		(1U << 1)
#define GHCI_STAGE_GEOMETRY_SHADER	(1U << 2)
#define GHCI_STAGE_TESSELLATION		(1U << 3)
#define GHCI_STAGE_STREAM_OUT		(1U << 4)
#define GHCI_LIMIT_GS_VERTICES_MASK	0x1FFU
#define GHCI_LIMIT_GS_INVOCATIONS_SHIFT	9U
#define GHCI_LIMIT_GS_INVOCATIONS_MASK	0x3FU
#define GHCI_LIMIT_PATCH_SIZE_SHIFT	15U
#define GHCI_LIMIT_PATCH_SIZE_MASK	0x3FU
#define GHCI_LIMIT_TESS_LEVEL_SHIFT	21U
#define GHCI_LIMIT_TESS_LEVEL_MASK	0x7FU
#define GHCI_STAGES_LDS_BYTES_MIN	(16U * 1024U)

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
 * FAULT_INFO's fields, its kinds and units, and the shader stages of a
 * shader unit's fault.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.5,
 * FAULT_INFO kinds and units, [19:16] SHADER_STAGE 0 VS to 7 GS; bit 20
 * tells a write page fault from a read one).
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
#define GHCI_FAULT_STAGE_VS		0U
#define GHCI_FAULT_STAGE_FS		1U
#define GHCI_FAULT_STAGE_CS		2U
#define GHCI_FAULT_STAGE_MS		3U
#define GHCI_FAULT_STAGE_TS		4U
#define GHCI_FAULT_STAGE_TCS		5U
#define GHCI_FAULT_STAGE_TES		6U
#define GHCI_FAULT_STAGE_GS		7U

/*
 * The extended capability list's IDs and where the list lies.  ID 1
 * (DISP) is not defined in GHCI 1.0 and ID 4 (VEND) is reserved: the
 * driver records them as any ID but uses neither.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 4, the
 * IDs; section 4.1, the list at 0x100..0x13F, eight entries at most;
 * section 13.1, DISP not defined).
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
 * (section 11.3, the MEM block).
 */
#define GHCI_MEM_LOCAL_BASE		0x00U
#define GHCI_MEM_LOCAL_SIZE		0x04U
#define GHCI_MEM_WINDOW_SIZE		0x08U
#define GHCI_MEM_HOST_LIMIT		0x0CU
#define GHCI_MEM_WINDOW_CTRL		0x10U

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
 * What one device can do, read once at the start and not changed after.
 *
 * Two reads fill it.  BOOT_CAPS, the extended capability list and the
 * MEM block come from the register window before the processor runs
 * (boot_caps, va_bits, pa_bits, blocks, the local memory); everything
 * else comes from the capability block CMD_QUERY_CAPS writes once the
 * ring runs (block_read is then 1).  formats holds the block's format
 * table word of each VkFormat below format_count; a later one has none.
 *
 * Where two words carry one fact, the one the driver had first stays the
 * authority: BOOT_CAPS for the walker, PROT and the address widths, the
 * list for the display, MEM for unified memory, and DEVICE_CAPS above
 * its mirrors in EXT_CAP_FEATURE, EXT_CAP_FMT0 and SHADER_STAGES.  A
 * copy that disagrees is kept for the log: mismatches holds the
 * DEVICE_CAPS bits concerned, width_mismatch the address widths.
 * MAX_MRT is the authority on the colour attachments; max_color is
 * EXT_CAP_FMT0's copy and fmt_limits FMT_LIMITS's, for the log.  A block
 * the list does not name has offset 0.  shader_stages and stage_limits
 * are words 19 and 20 as the device wrote them; stages_dropped holds the
 * stages of shader_stages the driver takes as absent because the shared
 * memory is below what they need.
 */
struct ghci_caps {
	uint32_t boot_caps;
	uint32_t va_bits;
	uint32_t pa_bits;
	uint32_t blocks;
	uint32_t block_offset[GHCI_EXTCAP_IDS];
	uint32_t block_version[GHCI_EXTCAP_IDS];
	uint64_t local_base;
	uint64_t local_bytes;
	uint64_t window_bytes;
	uint32_t host_limit;
	int block_read;
	uint32_t block_version_read;
	uint32_t device_caps;
	uint32_t feature;
	uint32_t fmt0;
	uint32_t native_isa;
	uint32_t gclk_hz;
	uint32_t resident_warps;
	uint32_t lane_registers;
	uint32_t num_su;
	uint32_t max_mrt;
	uint32_t max_color;
	uint32_t fmt_limits;
	uint32_t lds_bytes;
	uint32_t vendor[4];
	uint32_t device_version;
	uint32_t cache_config;
	uint32_t prot_caps;
	uint32_t perf_src;
	uint32_t extcap_present;
	uint32_t shader_stages;
	uint32_t stage_limits;
	uint32_t stages_dropped;
	uint32_t format_count;
	uint32_t formats[GHCI_CAPS_FORMATS];
	uint32_t mismatches;
	int width_mismatch;
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
	GHCI_HAS_BLT_TILE,
	GHCI_HAS_CP,
	GHCI_HAS_TEX_ARRAY,
	GHCI_HAS_TEX_3D,
	GHCI_HAS_TEX_CUBE,
	GHCI_HAS_TEX_TRILINEAR,
	GHCI_HAS_GEOMETRY_SHADER,
	GHCI_HAS_TESSELLATION,
	GHCI_HAS_STREAM_OUT
};

int ghci_caps_identify(const uint32_t *window);
int ghci_caps_boot(const uint32_t *window, struct ghci_caps *caps);
int ghci_caps_block(const uint32_t *words, uint32_t dwords, struct ghci_caps *caps);
int ghci_caps_has(const struct ghci_caps *caps, enum ghci_caps_flag flag);
int ghci_extcap_walk(const uint32_t *window, uint32_t *offsets, uint32_t *versions, uint32_t *found);
uint32_t ghci_heap_session_bytes(uint32_t va_bits);
uint64_t ghci_heap_device_bytes(uint32_t va_bits);
uint32_t ghci_va_chunks(uint32_t va_bits);
uint32_t ghci_watchdog_cycles(uint32_t gclk_hz, uint32_t milliseconds);
uint32_t ghci_halt_wait_ms(uint32_t gclk_hz, uint32_t wdt_cycles);
uint32_t ghci_reset_wait_ms(uint32_t gclk_hz, uint32_t wdt_cycles);

#endif
