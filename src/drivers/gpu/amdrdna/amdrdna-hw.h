/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the amdrdna driver learns from the tables an RDNA GPU and its video
 * BIOS carry, and the plain functions that read those tables.
 *
 * Nothing here touches the hardware: every function takes bytes the
 * caller has already read and fills a description, so the same code runs
 * in the kernel and in the host tests.
 */

#ifndef DRIVERS_GPU_AMDRDNA_AMDRDNA_HW_H
#define DRIVERS_GPU_AMDRDNA_AMDRDNA_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The most windows one IP block lists, and the most display pipes. */
#define AMDRDNA_IP_WINDOWS		6U
#define AMDRDNA_DISPLAY_PIPES		6U

/* The most reserved ranges a VRAM layout holds. */
#define AMDRDNA_LAYOUT_RANGES		8U

/* The most free spans one address pool tracks. */
#define AMDRDNA_POOL_SPANS		512U

/* The page-table levels of a context's address space, root first. */
#define AMDRDNA_VM_LEVELS		4U

/* The bytes of one entry of the interrupt ring. */
#define AMDRDNA_INTERRUPT_ENTRY_BYTES	32U

/*
 * The bits of a page entry the memory hubs read: present, in system memory,
 * coherent with the CPU's caches, executable, readable and writable; the
 * fragment (bits 11:7: the page lies in an aligned run of 2^(12 + n)
 * bytes mapped alike and contiguously, which one translation covers); the
 * memory type in bits 50:48 (3 is uncached); the bit that makes a
 * directory entry a leaf; and the address bits of a page and of a table.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_vm.h (AMDGPU_PTE_VALID,
 * AMDGPU_PTE_SYSTEM, AMDGPU_PTE_SNOOPED, AMDGPU_PTE_EXECUTABLE,
 * AMDGPU_PTE_READABLE, AMDGPU_PTE_WRITEABLE, AMDGPU_PTE_FRAG,
 * AMDGPU_PTE_MTYPE_NV10,
 * AMDGPU_PDE_PTE), drivers/gpu/drm/amd/include/navi10_enum.h (MTYPE_UC),
 * drivers/gpu/drm/amd/amdgpu/amdgpu_gmc.c (amdgpu_gmc_set_pte_pde: the
 * page address mask) and gmc_v10_0.c (gmc_v10_0_get_vm_pde: a table
 * address is 64-byte aligned and below 2^48).
 */
#define AMDRDNA_PAGE_PRESENT		0x0000000000000001ULL
#define AMDRDNA_PAGE_SYSTEM		0x0000000000000002ULL
#define AMDRDNA_PAGE_COHERENT		0x0000000000000004ULL
#define AMDRDNA_PAGE_EXECUTE		0x0000000000000010ULL
#define AMDRDNA_PAGE_READ		0x0000000000000020ULL
#define AMDRDNA_PAGE_WRITE		0x0000000000000040ULL
#define AMDRDNA_PAGE_FRAGMENT_SHIFT	7U
#define AMDRDNA_PAGE_FRAGMENT_MASK	0x0000000000000f80ULL
#define AMDRDNA_PAGE_TYPE_SHIFT		48U
#define AMDRDNA_PAGE_TYPE_MASK		0x0007000000000000ULL
#define AMDRDNA_PAGE_TYPE_UNCACHED	3ULL
#define AMDRDNA_PAGE_LEAF		0x0040000000000000ULL
#define AMDRDNA_PAGE_ADDRESS_MASK	0x0000fffffffff000ULL
#define AMDRDNA_TABLE_ADDRESS_MASK	0x0000ffffffffffc0ULL

/*
 * The memory type of a context's VRAM pages: non-coherent (0), the type
 * the hubs give an ordinary mapping of local memory.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/amd/include/navi10_enum.h
 * (MTYPE_NC) and drivers/gpu/drm/amd/amdgpu/gmc_v10_0.c
 * (gmc_v10_0_get_vm_pte: the default memory type of a mapping).
 */
#define AMDRDNA_PAGE_TYPE_NONCOHERENT	0ULL

/*
 * The interrupt clients and the source the driver handles in this stage:
 * the page faults the media hub and the graphics hub report (source 0 of
 * each).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/soc15_ih_clientid.h (SOC15_IH_CLIENTID_VMC,
 * SOC15_IH_CLIENTID_UTCL2) and
 * drivers/gpu/drm/amd/include/ivsrcid/vmc/irqsrcs_vmc_1_0.h
 * (VMC_1_0__SRCID__VM_FAULT, UTCL2_1_0__SRCID__FAULT).
 */
#define AMDRDNA_CLIENT_MEDIA_HUB	0x12U
#define AMDRDNA_CLIENT_GRAPHICS_HUB	0x1bU
#define AMDRDNA_SOURCE_PAGE_FAULT	0x00U

/*
 * The video engine's interrupt client and its sources: the decode ring's
 * interrupt command (the firmware's system message), and the general and
 * low-latency encode rings' interrupt commands.
 *
 * Values referenced: Linux 7.1
 * drivers/gpu/drm/amd/include/soc15_ih_clientid.h (SOC15_IH_CLIENTID_VCN)
 * and drivers/gpu/drm/amd/include/ivsrcid/vcn/irqsrcs_vcn_2_0.h
 * (VCN_2_0__SRCID__UVD_SYSTEM_MESSAGE_INTERRUPT,
 * VCN_2_0__SRCID__UVD_ENC_GENERAL_PURPOSE,
 * VCN_2_0__SRCID__UVD_ENC_LOW_LATENCY).
 */
#define AMDRDNA_CLIENT_VIDEO		0x10U
#define AMDRDNA_SOURCE_VIDEO_DECODE	0x7cU
#define AMDRDNA_SOURCE_VIDEO_ENCODE	0x77U
#define AMDRDNA_SOURCE_VIDEO_ENCODE_LOW	0x78U

/*
 * The interrupt clients and sources of the thermal stage: the thermal
 * block's high alert (the temperature rose above the high threshold) and
 * low alert (it fell below the low one), the critical-temperature trip the
 * SMU's I/O block reports on its GPIO 19, and the software interrupt the
 * system-management unit raises for the driver.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/soc15_ih_clientid.h
 * (SOC15_IH_CLIENTID_THM, SOC15_IH_CLIENTID_ROM_SMUIO,
 * SOC15_IH_CLIENTID_MP1), drivers/gpu/drm/amd/pm/swsmu/smu11/smu_v11_0.c
 * (THM_11_0__SRCID__THM_DIG_THERM_L2H, THM_11_0__SRCID__THM_DIG_THERM_H2L,
 * SMUIO_11_0__SRCID__SMUIO_GPIO19; smu_v11_0_set_irq_state: the software
 * interrupt's ID 0xFE).
 */
#define AMDRDNA_CLIENT_THERMAL		0x0fU
#define AMDRDNA_CLIENT_SMU_IO		0x16U
#define AMDRDNA_CLIENT_SMU		0x1fU
#define AMDRDNA_SOURCE_THERMAL_HIGH	0x00U
#define AMDRDNA_SOURCE_THERMAL_LOW	0x01U
#define AMDRDNA_SOURCE_CRITICAL		0x53U
#define AMDRDNA_SOURCE_SMU_SOFT		0xfeU

/*
 * The three features of the system-management unit's firmware the driver
 * asks for, as the low and high words of the 64-bit feature mask: the
 * firmware's own critical-temperature stop (bit 31), the fan control (bit
 * 32) and the thermal control (bit 33).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (FEATURE_FW_CTF_BIT, FEATURE_FAN_CONTROL_BIT, FEATURE_THERMAL_BIT).
 */
#define AMDRDNA_SMU_FEATURES_LOW	0x80000000U
#define AMDRDNA_SMU_FEATURES_HIGH	0x00000003U

/*
 * The features the driver also permits when the boot line asks the
 * firmware to move the graphics clock (amdrdna.smu.gfxclk=1), in the low
 * word of the feature mask: the clock's prefetcher (bit 0) and its levels
 * (bit 1), and the limits that keep a raised clock within the board's
 * power (bit 23), current (bit 24), current steps (bit 25) and regulator
 * temperature (bit 29), which Linux permits whatever it asks for; and the
 * controls of the graphics voltage that Linux also always permits, so a
 * raised clock under bursts of work keeps a safe voltage: the advanced
 * power-clock control (bit 26), the spread spectrum (bit 37, bit 5 of the
 * high word), the temperature-dependent minimum voltage (bit 39, bit 7)
 * and the droop control (bit 42, bit 10).  And the memory clock's levels
 * (bit 3) with the memory's two voltages that Linux always permits with
 * them (bits 8 and 9), and the system clock's levels (bit 4): the power
 * side holds the memory clock at its highest level once (smu.c), which the
 * firmware does not do while the system clock stands still.  Not the system
 * clock's deep sleep (bit 11, below).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (FEATURE_DPM_PREFETCHER_BIT, FEATURE_DPM_GFXCLK_BIT, FEATURE_PPT_BIT,
 * FEATURE_TDC_BIT, FEATURE_GFX_EDC_BIT, FEATURE_VR0HOT_BIT,
 * FEATURE_APCC_PLUS_BIT, FEATURE_GFX_SS_BIT,
 * FEATURE_TEMP_DEPENDENT_VMIN_BIT, FEATURE_APCC_DFLL_BIT,
 * FEATURE_DPM_UCLK_BIT, FEATURE_MEM_VDDCI_SCALING_BIT,
 * FEATURE_MEM_MVDD_SCALING_BIT, FEATURE_DPM_SOCCLK_BIT) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_get_allowed_feature_mask: the features always permitted).
 */
#define AMDRDNA_SMU_CLOCK_FEATURES_LOW	0x2780031bU
#define AMDRDNA_SMU_CLOCK_FEATURES_HIGH	0x000004a0U

/*
 * The memory side's part of the low word above: the memory clock's levels
 * (3), the SoC clock's levels (4, which the memory clock's highest level
 * needs: without it the switch to that level never ends) and the memory
 * voltages' scaling (8, 9), permitted only with amdrdna.smu.memory=1.  The
 * SoC clock's deep sleep (11) stays off: it only saves power at idle, and
 * without the display's clock levels and the deep sleep companions Linux
 * permits with it, the firmware stopped at idle once with it.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (FEATURE_DPM_UCLK_BIT, FEATURE_DPM_SOCCLK_BIT,
 * FEATURE_MEM_VDDCI_SCALING_BIT, FEATURE_MEM_MVDD_SCALING_BIT,
 * FEATURE_DS_SOCCLK_BIT) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_get_allowed_feature_mask: DS_SOCCLK with DS_LCLK, DS_DCEFCLK and
 * DPM_DCEFCLK).
 */
#define AMDRDNA_SMU_MEMORY_FEATURES_LOW	0x00000318U

/*
 * The display engine's clock's levels (bit 7), permitted with the memory
 * side when the firmware moves the memory clock with the load
 * (amdrdna.smu.memory=2): the display's step then gives that clock a
 * floor the display's plan was worked out with, as Linux does.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (FEATURE_DPM_DCEFCLK_BIT) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_get_allowed_feature_mask).
 */
#define AMDRDNA_SMU_DISPLAY_FEATURES_LOW	0x00000080U

/*
 * The graphics clock's levels, in the low word of the feature mask.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (FEATURE_DPM_GFXCLK_BIT).
 */
#define AMDRDNA_SMU_FEATURE_GFX_LEVELS	0x00000002U

/*
 * The memory clock's levels, in the low word of the feature mask.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (FEATURE_DPM_UCLK_BIT).
 */
#define AMDRDNA_SMU_FEATURE_MEMORY_LEVELS	0x00000008U

/*
 * The sizes of the two tables the driver exchanges with the unit's
 * firmware: the board's power table it hands over, and the table of
 * readings it gets back (the larger of the two layouts the firmware
 * versions use; the fields the driver reads lie at the same place in
 * both).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (sizeof(PPTable_t), sizeof(SmuMetrics_t), sizeof(SmuMetrics_legacy_t)).
 */
#define AMDRDNA_SMU_POWER_TABLE_BYTES	1192U
#define AMDRDNA_SMU_READINGS_BYTES	104U

/*
 * The table of the firmware's averaging settings the driver hands over
 * once the graphics clock's levels run: its size, and the time constant
 * the driver gives each of its six averages (the graphics, system and
 * memory clocks, the graphics and memory activity, and the board's
 * power), in milliseconds.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (sizeof(DriverSmuConfig_t)) and
 * drivers/gpu/drm/amd/pm/swsmu/smu11/navi10_ppt.c
 * (navi10_get_default_config_table_settings).
 */
#define AMDRDNA_SMU_SETTINGS_BYTES	44U

/*
 * The firmware's watermark table (its table 1): the display's clock
 * ranges, a row each, and the watermark set the display hub uses within
 * a range.  Two groups of four rows of 44 bytes (the writers' by the
 * system clock, then the readers' by the display engine's clock), each
 * row the clock's range and the memory clock's range in MHz (16 bits
 * each) and the set (a byte), then the firmware's own words; the table
 * ends in more of them.
 *
 * Values referenced: Linux v6.12
 * drivers/gpu/drm/amd/pm/swsmu/inc/pmfw_if/smu11_driver_if_navi10.h
 * (WatermarkRowGeneric_t, NUM_WM_RANGES, WM_SOCCLK, WM_DCEFCLK,
 * Watermarks_t, TABLE_WATERMARKS).
 */
#define AMDRDNA_SMU_WATERMARKS_TABLE	1U
#define AMDRDNA_SMU_WATERMARKS_BYTES	384U
#define AMDRDNA_SMU_WATERMARK_ROW_BYTES	44U
#define AMDRDNA_SMU_WATERMARK_ROWS	4U
#define AMDRDNA_SMU_WATERMARK_WRITERS	0U
#define AMDRDNA_SMU_WATERMARK_READERS	1U
#define AMDRDNA_SMU_AVERAGE_MS		10U

/*
 * The interrupt clients and sources of the engines: the command processor
 * (its end-of-pipe event, a bad opcode, a privileged register and a
 * privileged instruction) and the first copy engine (its trap); and the
 * ring field of a command processor's interrupt (engine 3:2, pipe 1:0,
 * queue 6:4).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/soc15_ih_clientid.h
 * (SOC15_IH_CLIENTID_GRBM_CP, SOC15_IH_CLIENTID_SDMA0),
 * drivers/gpu/drm/amd/include/ivsrcid/gfx/irqsrcs_gfx_10_1.h
 * (GFX_10_1__SRCID__CP_EOP_INTERRUPT, GFX_10_1__SRCID__CP_BAD_OPCODE_ERROR,
 * GFX_10_1__SRCID__CP_PRIV_REG_FAULT, GFX_10_1__SRCID__CP_PRIV_INSTR_FAULT),
 * ivsrcid/sdma0/irqsrcs_sdma0_5_0.h (SDMA0_5_0__SRCID__SDMA_TRAP) and
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_eop_irq: the ring
 * field).
 */
#define AMDRDNA_CLIENT_COMMAND		0x14U
#define AMDRDNA_CLIENT_COPY0		0x08U
#define AMDRDNA_SOURCE_END_OF_PIPE	0xb5U
#define AMDRDNA_SOURCE_BAD_OPCODE	0xb7U
#define AMDRDNA_SOURCE_PRIVILEGED_WORD	0xb8U
#define AMDRDNA_SOURCE_PRIVILEGED_CODE	0xb9U
#define AMDRDNA_SOURCE_COPY_TRAP	0xe0U
#define AMDRDNA_RING_FIELD_ENGINE_MASK	0x0cU
#define AMDRDNA_RING_FIELD_ENGINE_SHIFT	2U
#define AMDRDNA_RING_FIELD_PIPE_MASK	0x03U
#define AMDRDNA_RING_FIELD_QUEUE_MASK	0x70U
#define AMDRDNA_RING_FIELD_QUEUE_SHIFT	4U

/*
 * The interrupt client and sources of the display engine: the start of a
 * frame's fetch of each of the six timing generators, a finished flip of
 * each of the six planes, and the hot-plug lines, which share one source
 * and name the line in the first data word (0 to 5 a plug or an unplug, 6
 * to 11 a short pulse).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/soc15_ih_clientid.h (SOC15_IH_CLIENTID_DCE),
 * drivers/gpu/drm/amd/include/ivsrcid/dcn/irqsrcs_dcn_1_0.h
 * (DCN_1_0__SRCID__DC_D1_OTG_VSTARTUP, DCN_1_0__SRCID__HUBP0_FLIP_INTERRUPT,
 * DCN_1_0__SRCID__DC_HPD1_INT, DCN_1_0__CTXID__DC_HPD1_INT,
 * DCN_1_0__CTXID__DC_HPD1_RX_INT) and
 * drivers/gpu/drm/amd/display/dc/irq/dcn20/irq_service_dcn20.c
 * (to_dal_irq_source_dcn20: the line in the extra word).
 */
#define AMDRDNA_CLIENT_DISPLAY		0x04U
#define AMDRDNA_SOURCE_FRAME_START	0x3cU
#define AMDRDNA_SOURCE_FLIP_DONE	0x4fU
#define AMDRDNA_SOURCE_HOTPLUG		0x09U
#define AMDRDNA_HOTPLUG_SHORT_FIRST	6U

/*
 * The IP blocks the driver knows by name.  The GPU's block table lists
 * many more; the others are counted but not kept.
 */
enum amdrdna_block {
	AMDRDNA_BLOCK_GC,
	AMDRDNA_BLOCK_NBIO,
	AMDRDNA_BLOCK_PSP,
	AMDRDNA_BLOCK_SMU,
	AMDRDNA_BLOCK_SDMA0,
	AMDRDNA_BLOCK_SDMA1,
	AMDRDNA_BLOCK_IH,
	AMDRDNA_BLOCK_HDP,
	AMDRDNA_BLOCK_MMHUB,
	AMDRDNA_BLOCK_ATHUB,
	AMDRDNA_BLOCK_DCN,
	AMDRDNA_BLOCK_THM,
	AMDRDNA_BLOCK_SMUIO,
	AMDRDNA_BLOCK_VCN,
	AMDRDNA_BLOCK_DF,
	AMDRDNA_BLOCK_UMC,
	AMDRDNA_BLOCK_COUNT
};

/*
 * The generations of AMD GPU the driver recognizes by device ID.  Only
 * the first is driven; the others are named in the log and left alone.
 */
enum amdrdna_generation {
	AMDRDNA_GENERATION_UNKNOWN,
	AMDRDNA_GENERATION_RDNA1,
	AMDRDNA_GENERATION_RDNA2,
	AMDRDNA_GENERATION_RDNA3
};

/* The display registers the readout and the host tests name. */
enum amdrdna_dcn_register {
	AMDRDNA_DCN_HEAD_LEGACY_MODE,
	AMDRDNA_DCN_DISPLAY_CLOCK_DIVIDER,
	AMDRDNA_DCN_PIXEL_RATE_CONTROL,
	AMDRDNA_DCN_DP_CLOCK_PHASE,
	AMDRDNA_DCN_DP_CLOCK_MODULO,
	AMDRDNA_DCN_PLANE_FORMAT,
	AMDRDNA_DCN_PLANE_TILING,
	AMDRDNA_DCN_PLANE_VIEWPORT_START,
	AMDRDNA_DCN_PLANE_VIEWPORT_SIZE,
	AMDRDNA_DCN_PLANE_CONTROL,
	AMDRDNA_DCN_PLANE_CLOCK,
	AMDRDNA_DCN_PLANE_PITCH,
	AMDRDNA_DCN_PLANE_ADDRESS_LOW,
	AMDRDNA_DCN_PLANE_ADDRESS_HIGH,
	AMDRDNA_DCN_PLANE_SURFACE_CONTROL,
	AMDRDNA_DCN_PLANE_FLIP_CONTROL,
	AMDRDNA_DCN_PLANE_COLOR_ROUTE,
	AMDRDNA_DCN_PIXEL_PROCESSOR_CONTROL,
	AMDRDNA_DCN_BLEND_TOP,
	AMDRDNA_DCN_BLEND_BOTTOM,
	AMDRDNA_DCN_BLEND_OUTPUT,
	AMDRDNA_DCN_BLEND_CONTROL,
	AMDRDNA_DCN_BLEND_TREE_ROOT,
	AMDRDNA_DCN_COMBINER_SOURCE,
	AMDRDNA_DCN_COMBINER_INPUT,
	AMDRDNA_DCN_TIMING_H_TOTAL,
	AMDRDNA_DCN_TIMING_H_BLANK,
	AMDRDNA_DCN_TIMING_H_SYNC,
	AMDRDNA_DCN_TIMING_H_SYNC_CONTROL,
	AMDRDNA_DCN_TIMING_V_TOTAL,
	AMDRDNA_DCN_TIMING_V_BLANK,
	AMDRDNA_DCN_TIMING_V_SYNC,
	AMDRDNA_DCN_TIMING_V_SYNC_CONTROL,
	AMDRDNA_DCN_TIMING_CONTROL,
	AMDRDNA_DCN_TIMING_UPDATE_LOCK,
	AMDRDNA_DCN_ENCODER_FRONT_CONTROL,
	AMDRDNA_DCN_ENCODER_BACK_CONTROL,
	AMDRDNA_DCN_ENCODER_BACK_ENABLE,
	AMDRDNA_DCN_DP_STREAM_CONTROL,
	AMDRDNA_DCN_DP_LINK_CONTROL,
	AMDRDNA_DCN_SCALER_MODE,
	AMDRDNA_DCN_SCALER_H_RATIO,
	AMDRDNA_DCN_SCALER_V_RATIO,
	AMDRDNA_DCN_SCALER_OUTPUT_START,
	AMDRDNA_DCN_SCALER_OUTPUT_SIZE,
	AMDRDNA_DCN_SCALER_AUTOMATIC,
	AMDRDNA_DCN_PLANE_SPACE_SELECT,
	AMDRDNA_DCN_PLANE_FLIP_INTERRUPT,
	AMDRDNA_DCN_PLANE_SHOWN_LOW,
	AMDRDNA_DCN_PLANE_SHOWN_HIGH,
	AMDRDNA_DCN_PLANE_CURSOR_TIMING,
	AMDRDNA_DCN_CURSOR_CONTROL,
	AMDRDNA_DCN_CURSOR_ADDRESS_LOW,
	AMDRDNA_DCN_CURSOR_ADDRESS_HIGH,
	AMDRDNA_DCN_CURSOR_SIZE,
	AMDRDNA_DCN_CURSOR_POSITION,
	AMDRDNA_DCN_CURSOR_HOT_SPOT,
	AMDRDNA_DCN_CURSOR_DESTINATION,
	AMDRDNA_DCN_PIXEL_CURSOR_CONTROL,
	AMDRDNA_DCN_PIXEL_CURSOR_SCALE,
	AMDRDNA_DCN_TIMING_POSITION,
	AMDRDNA_DCN_TIMING_FRAME_COUNT,
	AMDRDNA_DCN_TIMING_SYNC_STATUS,
	AMDRDNA_DCN_HOTPLUG_STATUS,
	AMDRDNA_DCN_HOTPLUG_CONTROL,
	AMDRDNA_DCN_I2C_CONTROL,
	AMDRDNA_DCN_I2C_ARBITRATION,
	AMDRDNA_DCN_I2C_STATUS,
	AMDRDNA_DCN_I2C_LINE_STATUS,
	AMDRDNA_DCN_I2C_LINE_SPEED,
	AMDRDNA_DCN_I2C_LINE_SETUP,
	AMDRDNA_DCN_I2C_TRANSACTION,
	AMDRDNA_DCN_I2C_DATA,
	AMDRDNA_DCN_TIME_BASE,
	AMDRDNA_DCN_DDC_PAD_MASK,
	AMDRDNA_DCN_AUX_CONTROL,
	AMDRDNA_DCN_AUX_SEND_CONTROL,
	AMDRDNA_DCN_AUX_ARBITRATION,
	AMDRDNA_DCN_AUX_INTERRUPT_CONTROL,
	AMDRDNA_DCN_AUX_SEND_STATUS,
	AMDRDNA_DCN_AUX_SEND_DATA,
	AMDRDNA_DCN_AUX_RECEIVE_CONTROL,
	AMDRDNA_DCN_TIMING_BLANK,
	AMDRDNA_DCN_TIMING_CRC_CONTROL,
	AMDRDNA_DCN_TIMING_CRC_RED_GREEN,
	AMDRDNA_DCN_TIMING_CRC_BLUE,
	AMDRDNA_DCN_PLANE_SECOND_VIEWPORT_START,
	AMDRDNA_DCN_PLANE_SECOND_VIEWPORT_SIZE,
	AMDRDNA_DCN_PLANE_REQUEST_SIZE,
	AMDRDNA_DCN_PLANE_REQUEST_SIZE_CHROMA,
	AMDRDNA_DCN_PLANE_REQUEST_DEBUG,
	AMDRDNA_DCN_PLANE_EXPANSION,
	AMDRDNA_DCN_PLANE_LEVEL_MARKS,
	AMDRDNA_DCN_PLANE_GLOBAL_TTU,
	AMDRDNA_DCN_PLANE_LUMA_TTU,
	AMDRDNA_DCN_PLANE_LUMA_TTU_PREFETCH,
	AMDRDNA_DCN_PLANE_CHROMA_TTU,
	AMDRDNA_DCN_PLANE_CHROMA_TTU_PREFETCH,
	AMDRDNA_DCN_PLANE_CURSOR_TTU,
	AMDRDNA_DCN_PLANE_CURSOR_TTU_PREFETCH,
	AMDRDNA_DCN_PLANE_SECOND_CURSOR_TTU_PREFETCH,
	AMDRDNA_DCN_PLANE_BLANK_OFFSET,
	AMDRDNA_DCN_PLANE_NEXT_START,
	AMDRDNA_DCN_PLANE_LINE_CYCLES,
	AMDRDNA_DCN_PLANE_AFTER_SCALER,
	AMDRDNA_DCN_PLANE_PREFETCH,
	AMDRDNA_DCN_PLANE_PREFETCH_CHROMA,
	AMDRDNA_DCN_PLANE_VBLANK_LINES,
	AMDRDNA_DCN_PLANE_VBLANK_PTE,
	AMDRDNA_DCN_PLANE_VBLANK_PTE_CHROMA,
	AMDRDNA_DCN_PLANE_VBLANK_META,
	AMDRDNA_DCN_PLANE_VBLANK_META_CHROMA,
	AMDRDNA_DCN_PLANE_FLIP_LINES,
	AMDRDNA_DCN_PLANE_FLIP_PTE,
	AMDRDNA_DCN_PLANE_FLIP_META,
	AMDRDNA_DCN_PLANE_PTE_ROW_LINES,
	AMDRDNA_DCN_PLANE_PTE_GROUP,
	AMDRDNA_DCN_PLANE_PTE_ROW_LINES_CHROMA,
	AMDRDNA_DCN_PLANE_PTE_GROUP_CHROMA,
	AMDRDNA_DCN_PLANE_META_ROW_LINES,
	AMDRDNA_DCN_PLANE_META_CHUNK,
	AMDRDNA_DCN_PLANE_META_ROW_LINES_CHROMA,
	AMDRDNA_DCN_PLANE_META_CHUNK_CHROMA,
	AMDRDNA_DCN_PLANE_LINE_DELIVERY_PREFETCH,
	AMDRDNA_DCN_PLANE_LINE_DELIVERY,
	AMDRDNA_DCN_PLANE_REFERENCE_RATIO,
	AMDRDNA_DCN_PLANE_REQUEST_LIMIT,
	AMDRDNA_DCN_HUB_URGENT_MARK,
	AMDRDNA_DCN_HUB_TRIP_MARK,
	AMDRDNA_DCN_HUB_ENTER_MARK,
	AMDRDNA_DCN_HUB_EXIT_MARK,
	AMDRDNA_DCN_HUB_SWITCH_MARK,
	AMDRDNA_DCN_HUB_MARK_SELECT,
	AMDRDNA_DCN_HUB_SATURATION,
	AMDRDNA_DCN_HUB_OUTSTANDING,
	AMDRDNA_DCN_HUB_TIMER,
	AMDRDNA_DCN_TIMING_START_LINE,
	AMDRDNA_DCN_TIMING_UPDATE,
	AMDRDNA_DCN_TIMING_READY,
	AMDRDNA_DCN_TIMING_V_TOTAL_MIN,
	AMDRDNA_DCN_TIMING_V_TOTAL_MAX,
	AMDRDNA_DCN_TIMING_INTERLACE,
	AMDRDNA_DCN_TIMING_CLOCK_CONTROL,
	AMDRDNA_DCN_VERTICAL_GENERATOR,
	AMDRDNA_DCN_SCALER_H_BLANK,
	AMDRDNA_DCN_SCALER_V_BLANK,
	AMDRDNA_DCN_SCALER_BLEND_SIZE,
	AMDRDNA_DCN_PIXEL_RATE_SOURCE,
	AMDRDNA_DCN_TIMING_CRC_WINDOW_A_X,
	AMDRDNA_DCN_TIMING_CRC_WINDOW_A_Y,
	AMDRDNA_DCN_TIMING_CRC_WINDOW_B_X,
	AMDRDNA_DCN_TIMING_CRC_WINDOW_B_Y,
	AMDRDNA_DCN_HUB_DRAM_STATE,
	AMDRDNA_DCN_HUB_DEBUG_INDEX,
	AMDRDNA_DCN_HUB_DEBUG_DATA,
	AMDRDNA_DCN_REGISTER_COUNT
};

/*
 * Where the hardware cursor lies against the plane it is drawn over: the
 * position and hot-spot words the cursor takes, and whether any of its
 * image falls inside the plane.
 */
struct amdrdna_cursor_place {
	uint32_t position;
	uint32_t hot_spot;
	bool shown;
};

/*
 * How the firmware's surface reaches the screen: it fills the timing's
 * active area pixel for pixel, it sits unscaled inside a larger active
 * area, or the pixel processor's scaler stretches it.  The order is the
 * one of the AMDRDNA_GOP_SHAPE_ values of the control interface.
 */
enum amdrdna_picture_shape {
	AMDRDNA_PICTURE_WHOLE,
	AMDRDNA_PICTURE_PLACED,
	AMDRDNA_PICTURE_SCALED
};

/*
 * One IP block as the block table lists it: its version and where its
 * register windows start (dword indices).  found is zero for a block the
 * table did not list.
 */
struct amdrdna_ip {
	uint8_t found;
	uint8_t major;
	uint8_t minor;
	uint8_t step;
	uint8_t window_count;
	uint32_t window[AMDRDNA_IP_WINDOWS];
};

/*
 * The shape of the graphics engine as the block table states it: the
 * shader engines, the arrays in each, the paired compute units in each
 * array, the render back ends and the level-2 cache channels, the wave
 * width and waves per SIMD, and the scan converters of an engine and the
 * packers of a converter.
 */
struct amdrdna_shader_shape {
	uint32_t engines;
	uint32_t arrays_per_engine;
	uint32_t pairs_per_array;
	uint32_t backends_per_engine;
	uint32_t cache_channels;
	uint32_t wave_lanes;
	uint32_t waves_per_simd;
	uint32_t converters_per_engine;
	uint32_t packers_per_converter;
};

/*
 * What the GPU's own block table says: its layout version, the blocks the
 * driver knows, how many entries it listed in all, and the shader shape
 * when the table carries one.
 */
struct amdrdna_block_table {
	uint16_t layout_major;
	uint16_t layout_minor;
	uint16_t list_layout;
	unsigned listed;
	struct amdrdna_ip blocks[AMDRDNA_BLOCK_COUNT];
	bool has_shape;
	struct amdrdna_shader_shape shape;
};

/*
 * The boot database the security processor may leave at the top of VRAM:
 * whether it is there, its version, how many entries it lists, and the
 * boot configuration bits when one entry carries them.  config_offset is
 * where that entry lies from the database's start, for a caller that has
 * to read it separately.
 */
struct amdrdna_boot_database {
	bool present;
	uint16_t version;
	uint16_t entries;
	bool has_config;
	uint32_t config_offset;
	bool config_read;
	uint32_t config_bits;
};

/*
 * One command routine of the video BIOS: where it starts in the image,
 * and the workspace and parameter bytes it takes.
 */
struct amdrdna_vbios_routine {
	uint16_t offset;
	uint16_t work_bytes;
	uint16_t parameter_bytes;
};

/* The command routines the driver names in its log and later runs. */
enum amdrdna_vbios_command {
	AMDRDNA_VBIOS_COMMAND_POST,
	AMDRDNA_VBIOS_COMMAND_ENCODER,
	AMDRDNA_VBIOS_COMMAND_PIXEL_CLOCK,
	AMDRDNA_VBIOS_COMMAND_PIPE_POWER,
	AMDRDNA_VBIOS_COMMAND_TIMING_ENABLE,
	AMDRDNA_VBIOS_COMMAND_DISPLAY_CLOCK,
	AMDRDNA_VBIOS_COMMAND_TRANSMITTER,
	AMDRDNA_VBIOS_COMMAND_AUX,
	AMDRDNA_VBIOS_COMMAND_COUNT
};

/*
 * What the video BIOS image says that the driver uses: where its lists of
 * data tables and command routines are, the firmware description, the VRAM
 * the firmware keeps for itself, the power table's header and the board's
 * power-delivery table version, the display table's boot display clock and
 * reference clock (in 10 kHz), and the command routines named above.
 *
 * A field whose table the image lacks is zero.
 */
struct amdrdna_vbios_info {
	uint32_t length;
	uint16_t header_offset;
	uint16_t data_list;
	uint16_t command_list;
	uint16_t firmware_table;
	uint8_t firmware_format;
	uint8_t firmware_content;
	uint32_t capability_bits;
	uint32_t boot_engine_clock_10khz;
	uint32_t boot_memory_clock_10khz;
	uint32_t scratch_first;
	uint32_t power_table_id;
	uint32_t reserved_top_kib;
	uint16_t reserve_table;
	uint8_t reserve_format;
	uint8_t reserve_content;
	uint32_t firmware_start_kib;
	uint32_t firmware_kib;
	uint32_t driver_start_kib;
	uint32_t driver_kib;
	uint16_t power_table;
	uint8_t power_format;
	uint8_t power_content;
	uint8_t power_revision;
	uint16_t power_bytes;
	uint16_t shutdown_temperature;
	uint16_t board_table;
	uint8_t board_format;
	uint8_t board_content;
	uint32_t boot_display_clock_10khz;
	uint32_t display_reference_10khz;
	struct amdrdna_vbios_routine commands[AMDRDNA_VBIOS_COMMAND_COUNT];
};

/* The kinds of display connector the driver tells apart. */
enum amdrdna_connector_kind {
	AMDRDNA_CONNECTOR_OTHER,
	AMDRDNA_CONNECTOR_DP,
	AMDRDNA_CONNECTOR_HDMI,
	AMDRDNA_CONNECTOR_DVI
};

/* The display transmitters A to G, and none. */
#define AMDRDNA_TRANSMITTERS		7U
#define AMDRDNA_TRANSMITTER_NONE	255U

/*
 * One display path of the board as the video BIOS describes it: the
 * connector's object word and kind, the encoder's object word and the
 * transmitter it is, the I2C line's identifier (0 for none) and the
 * register index of its pad's output word (from the pin table, 0 when the
 * table lacks it), and the hot-plug pin (0 for none).
 */
struct amdrdna_vbios_connector {
	uint16_t connector_object;
	enum amdrdna_connector_kind kind;
	uint16_t encoder_object;
	unsigned transmitter;
	uint8_t ddc_identifier;
	uint32_t ddc_pad_register;
	uint8_t hotplug_pin;
};

/* The words the largest display routine's parameters take (the transmitter's 16 bytes and 16 reserved). */
#define AMDRDNA_COMMAND_WORDS		8U

/*
 * The pixel clock routine's parameters: the clock in 100 Hz, the PLL, the
 * encoder object's identifier, the encoder mode, the flags, the timing
 * generator and the deep colour ratio.
 */
struct amdrdna_pixel_clock_command {
	uint32_t pixel_100hz;
	uint8_t pll;
	uint8_t encoder_object;
	uint8_t mode;
	uint8_t flags;
	uint8_t generator;
	uint8_t depth;
};

/*
 * The encoder routine's stream setup: the DIG, the action, the mode, the
 * lanes, the pixel clock in 10 kHz, the bits a colour and the DisplayPort
 * rate in 270 MHz units.
 */
struct amdrdna_encoder_command {
	uint8_t dig;
	uint8_t action;
	uint8_t mode;
	uint8_t lanes;
	uint32_t pixel_10khz;
	uint8_t bits_per_colour;
	uint8_t dp_rate;
};

/*
 * The transmitter routine's parameters: the PHY, the action, the mode (or
 * a DisplayPort lane setting), the lanes, the symbol clock in 10 kHz, the
 * hot-plug line (1 to 6, 0 for none), the DIG front ends and the connector
 * object's identifier.
 */
struct amdrdna_transmitter_command {
	uint8_t phy;
	uint8_t action;
	uint8_t mode;
	uint8_t lanes;
	uint32_t symbol_10khz;
	uint8_t hotplug;
	uint8_t front_ends;
	uint8_t connector_id;
};

/*
 * What the display plan (mode-plan.c) is given: a mode's timing (the
 * totals, the active area and the front porches, the pixel clock in kHz),
 * the surface's pitch in pixels (linear, 32 bits a pixel, shown unscaled
 * on one pipe with a cursor), and the clocks the board runs in kHz (the hub's reference, the data fabric's return clock, the
 * fabric, the display clock, the pipe clock) and the memory's transfer
 * rate in MT/s, and the memory clock change's latency in tenths of a
 * microsecond (0: the latency of a change between the memory clock's
 * levels; the power side gives a shorter one when it holds the clock and
 * the firmware only pauses the memory now and then).
 */
struct amdrdna_mode_plan_input {
	uint32_t h_total;
	uint32_t width;
	uint32_t h_front;
	uint32_t v_total;
	uint32_t height;
	uint32_t v_front;
	uint32_t pixel_khz;
	uint32_t pitch;
	uint32_t reference_khz;
	uint32_t return_khz;
	uint32_t fabric_khz;
	uint32_t display_khz;
	uint32_t pipe_khz;
	uint32_t memory_mts;
	uint32_t switch_tenths;
};

/*
 * The display plan of a mode: what the memory may do in the vertical
 * blank (0 change its clock and self-refresh, 1 only self-refresh, 2
 * neither), whether its clock may change (0 never, 1 in the vertical
 * blank, 2 in the active area too), the timing generator's global sync in
 * lines and pixels, the hub's watermarks in ns, and the plane's request,
 * deadline and arbitration fields as the registers take them.
 */
struct amdrdna_mode_plan {
	unsigned vblank_use;
	unsigned memory_switch;
	uint32_t start_lines;
	uint32_t update_offset;
	uint32_t update_width;
	uint32_t ready_offset;
	uint32_t urgent_ns;
	uint32_t switch_ns;
	uint32_t refresh_exit_ns;
	uint32_t refresh_enter_exit_ns;
	uint32_t trip_ns;
	uint32_t chunk_log;
	uint32_t least_chunk_log;
	uint32_t meta_chunk_log;
	uint32_t least_meta_chunk_log;
	uint32_t pte_group_log;
	uint32_t meta_pte_group_log;
	uint32_t pte_rows_log;
	uint32_t swath_log;
	uint32_t data_expansion;
	uint32_t pte_expansion;
	uint32_t meta_expansion;
	uint32_t cursor_expansion;
	uint32_t blank_end_cycles;
	uint32_t blank_end_line;
	uint32_t next_start_quarters;
	uint32_t line_cycles;
	uint32_t after_scaler_cycles;
	uint32_t after_scaler_lines;
	uint32_t prefetch_quarters;
	uint32_t vm_vblank_quarters;
	uint32_t row_vblank_quarters;
	uint32_t vm_flip_quarters;
	uint32_t row_flip_quarters;
	uint32_t reference_to_pixel;
	uint32_t prefetch_ratio;
	uint32_t prefetch_ratio_chroma;
	uint32_t pte_group_vblank_cycles;
	uint32_t meta_chunk_vblank_cycles;
	uint32_t pte_group_flip_cycles;
	uint32_t meta_chunk_flip_cycles;
	uint32_t pte_row_quarters;
	uint32_t meta_row_quarters;
	uint32_t pte_group_cycles;
	uint32_t meta_chunk_cycles;
	uint32_t line_delivery_prefetch_cycles;
	uint32_t line_delivery_cycles;
	uint32_t cursor_chunk_adjust;
	uint32_t ready_after_zero;
	uint32_t request_limit;
	uint32_t low_level_mark;
	uint32_t high_level_mark;
	uint32_t vblank_ttu_cycles;
	uint32_t flip_level;
	uint32_t request_delivery_prefetch_cycles;
	uint32_t request_delivery_cycles;
	uint32_t cursor_delivery_prefetch_cycles;
	uint32_t cursor_delivery_cycles;
	uint32_t fixed_level;
};

/* The PCI place and identity a video BIOS image in the ACPI table names. */
struct amdrdna_vbios_owner {
	uint32_t bus;
	uint32_t device;
	uint32_t function;
	uint16_t vendor;
	uint16_t product;
};

/*
 * What the board's power table says about temperatures, read while the
 * table is put together for the unit's firmware: the limits of the edge,
 * the hotspot and the memory and the shutdown temperature, all in degrees
 * Celsius, the kind of thermal controller the board has (0 for none), and
 * the features the table asks the firmware to run (low and high words).
 */
struct amdrdna_power_limits {
	unsigned edge;
	unsigned hotspot;
	unsigned memory;
	unsigned shutdown;
	unsigned controller;
	uint32_t features_low;
	uint32_t features_high;
};

/*
 * One set of readings the unit's firmware handed back: the edge, hotspot
 * and memory temperatures in degrees Celsius and the fan's speed in turns
 * per minute; the graphics, system and memory clocks, the display's data
 * return clock and the display clock as they run now and the graphics
 * clock's average, in megahertz; and how busy the graphics
 * engine was on average, in percent.
 */
struct amdrdna_power_readings {
	unsigned edge;
	unsigned hotspot;
	unsigned memory;
	unsigned fan_speed;
	unsigned gfx_clock;
	unsigned system_clock;
	unsigned memory_clock;
	unsigned return_clock;
	unsigned display_engine_clock;
	unsigned gfx_average;
	unsigned gfx_activity;
};

/*
 * What the display chain and the loader say about the firmware's picture.
 *
 * The timing generator gives the active area the screen shows; the plane
 * gives the viewport, the part of the surface it fetches; the pixel
 * processor's scaler gives the rectangle of the active area the viewport
 * is drawn into, its mode (0 passes the pixels through), its two ratios,
 * the viewport's size over the rectangle's in units of 1/2^24, and its
 * automatic mode (0 off; 1 the scaler works the ratios out itself; 2 and 3
 * it centres the viewport unscaled, 3 also repeating pixels); the loader
 * gives the size of the framebuffer it handed the kernel.
 */
struct amdrdna_picture_input {
	uint32_t active_width;
	uint32_t active_height;
	uint32_t viewport_x;
	uint32_t viewport_y;
	uint32_t viewport_width;
	uint32_t viewport_height;
	uint32_t scaler_mode;
	uint32_t scaler_x;
	uint32_t scaler_y;
	uint32_t scaler_width;
	uint32_t scaler_height;
	uint32_t horizontal_ratio;
	uint32_t vertical_ratio;
	uint32_t scaler_automatic;
	bool loader_present;
	uint32_t loader_width;
	uint32_t loader_height;
};

/*
 * What the check found: how the surface reaches the screen, whether the
 * scaler's rectangle lies inside the active area, and whether its mode and
 * ratios agree with the viewport and the rectangle.  The last two are only
 * noted; they do not decide whether the picture is the console's.
 */
struct amdrdna_picture {
	enum amdrdna_picture_shape shape;
	bool rectangle_inside;
	bool ratios_agree;
};

/*
 * What the VRAM layout is planned from: the VRAM's size, the part of it the
 * CPU window shows, the firmware's picture, and the ranges the video BIOS
 * keeps for the firmware and the driver.
 */
struct amdrdna_layout_input {
	uint64_t vram_bytes;
	uint64_t window_bytes;
	bool gop_present;
	uint64_t gop_offset;
	uint64_t gop_bytes;
	uint64_t firmware_start;
	uint64_t firmware_bytes;
	uint64_t driver_start;
	uint64_t driver_bytes;
};

/* Why a VRAM range is kept out of the allocator. */
enum amdrdna_range_reason {
	AMDRDNA_RANGE_TOP,
	AMDRDNA_RANGE_GOP,
	AMDRDNA_RANGE_FIRMWARE,
	AMDRDNA_RANGE_DRIVER,
	AMDRDNA_RANGE_TRAINING
};

/* One VRAM range kept out of the allocator, [start, end). */
struct amdrdna_range {
	uint64_t start;
	uint64_t end;
	enum amdrdna_range_reason reason;
};

/*
 * The planned VRAM layout: the ranges no allocation may take, the pool
 * the CPU can reach through its window, and the pool only the GPU reaches.
 * The training range is held only until memory training has run or has
 * been skipped.
 */
struct amdrdna_layout {
	struct amdrdna_range reserved[AMDRDNA_LAYOUT_RANGES];
	unsigned reserved_count;
	uint64_t cpu_pool_start;
	uint64_t cpu_pool_end;
	uint64_t gpu_pool_start;
	uint64_t gpu_pool_end;
};

/* One known AMD GPU device ID and what it is. */
struct amdrdna_product {
	uint16_t device;
	enum amdrdna_generation generation;
	const char *chip;
};

/*
 * The two memory hubs that translate the GPU's addresses: the graphics hub,
 * which the graphics, compute and copy engines go through, and the media
 * hub, which the display, the interrupt controller and the host data path
 * go through.
 */
enum amdrdna_hub {
	AMDRDNA_HUB_GRAPHICS,
	AMDRDNA_HUB_MEDIA,
	AMDRDNA_HUB_COUNT
};

/*
 * The registers of a memory hub the driver names.  The two hubs have the
 * same registers in the same order, in their own blocks.  A register kept
 * once per context (16) or once per invalidation engine (18) takes the
 * context's or the engine's number as its instance.
 */
enum amdrdna_hub_register {
	AMDRDNA_HUB_CACHE_CONTROL,
	AMDRDNA_HUB_CACHE_CONTROL_2,
	AMDRDNA_HUB_CACHE_CONTROL_3,
	AMDRDNA_HUB_CACHE_CONTROL_4,
	AMDRDNA_HUB_CACHE_CONTROL_5,
	AMDRDNA_HUB_FAULT_CONTROL,
	AMDRDNA_HUB_FAULT_CONTROL_2,
	AMDRDNA_HUB_FAULT_STATUS,
	AMDRDNA_HUB_FAULT_ADDRESS_LOW,
	AMDRDNA_HUB_FAULT_ADDRESS_HIGH,
	AMDRDNA_HUB_FAULT_PAGE_LOW,
	AMDRDNA_HUB_FAULT_PAGE_HIGH,
	AMDRDNA_HUB_IDENTITY_FIRST_LOW,
	AMDRDNA_HUB_IDENTITY_FIRST_HIGH,
	AMDRDNA_HUB_IDENTITY_LAST_LOW,
	AMDRDNA_HUB_IDENTITY_LAST_HIGH,
	AMDRDNA_HUB_IDENTITY_SHIFT_LOW,
	AMDRDNA_HUB_IDENTITY_SHIFT_HIGH,
	AMDRDNA_HUB_CONTEXT_CONTROL,
	AMDRDNA_HUB_INVALIDATE_LOCK,
	AMDRDNA_HUB_INVALIDATE_REQUEST,
	AMDRDNA_HUB_INVALIDATE_DONE,
	AMDRDNA_HUB_INVALIDATE_RANGE_LOW,
	AMDRDNA_HUB_INVALIDATE_RANGE_HIGH,
	AMDRDNA_HUB_TABLE_BASE_LOW,
	AMDRDNA_HUB_TABLE_BASE_HIGH,
	AMDRDNA_HUB_TABLE_FIRST_LOW,
	AMDRDNA_HUB_TABLE_FIRST_HIGH,
	AMDRDNA_HUB_TABLE_LAST_LOW,
	AMDRDNA_HUB_TABLE_LAST_HIGH,
	AMDRDNA_HUB_FRAME_BUFFER_PHYSICAL,
	AMDRDNA_HUB_DEFAULT_PAGE_LOW,
	AMDRDNA_HUB_DEFAULT_PAGE_HIGH,
	AMDRDNA_HUB_FRAME_BUFFER_BOTTOM,
	AMDRDNA_HUB_FRAME_BUFFER_TOP,
	AMDRDNA_HUB_BYPASS_TOP,
	AMDRDNA_HUB_BYPASS_BOTTOM,
	AMDRDNA_HUB_BYPASS_BASE,
	AMDRDNA_HUB_APERTURE_LOW,
	AMDRDNA_HUB_APERTURE_HIGH,
	AMDRDNA_HUB_TRANSLATION_CONTROL,
	AMDRDNA_HUB_REGISTER_COUNT
};

/* One run of free addresses in a pool, [start, end). */
struct amdrdna_span {
	uint64_t start;
	uint64_t end;
};

/*
 * A pool of addresses handed out in aligned runs: the VRAM the CPU reaches
 * through its window, the VRAM only the GPU reaches, or the GPU addresses
 * of the system-memory table.
 *
 * The pool covers [start, end); spans lists the runs that are free, sorted
 * and never touching (a run given back is merged with its neighbours), and
 * free_bytes is their sum.  A pool starts empty; its owner gives it the
 * runs it may hand out.  The owner serializes every call.
 */
struct amdrdna_pool {
	uint64_t start;
	uint64_t end;
	struct amdrdna_span spans[AMDRDNA_POOL_SPANS];
	unsigned span_count;
	uint64_t free_bytes;
};

/*
 * What one entry of the interrupt ring says: the client block and its
 * source, the ring and the context the event belongs to, whether the media
 * hub stamped the context, the timestamp, the process address-space ID the
 * hub attached, and the four words of source data.
 */
struct amdrdna_interrupt {
	uint8_t client;
	uint8_t source;
	uint8_t ring;
	uint8_t context;
	bool from_media_hub;
	uint64_t timestamp;
	uint16_t space_id;
	uint32_t data[4];
};

/*
 * What a page fault of a memory hub says: the hub, the faulting address,
 * what the access was (from the interrupt, decoded and as its raw bits),
 * and the hub's latched status
 * word decoded: more faults behind this one, the walker's error, the
 * permission bits that failed, a mapping error, the unit that asked, a
 * write, and the context.
 */
struct amdrdna_vm_fault {
	enum amdrdna_hub hub;
	uint64_t fault_address;
	bool retried;
	bool reading;
	bool writing;
	bool executing;
	uint32_t access_bits;
	uint16_t space_id;
	uint32_t latched;
	bool more_behind;
	unsigned walker_error;
	unsigned denied;
	bool unmapped;
	unsigned unit;
	bool unit_wrote;
	unsigned context;
};

/* The most pieces the driver cuts one firmware file into. */
#define AMDRDNA_FIRMWARE_PIECES		4U

/*
 * The kinds of firmware file, by how the driver cuts them: the security
 * processor's own file (its boot pieces), a file whose program is the one
 * piece, a compute processor's file (the program and its jump table) and
 * the run-list controller's file (its program and three lists).
 */
enum amdrdna_firmware_kind {
	AMDRDNA_FIRMWARE_SECURE,
	AMDRDNA_FIRMWARE_WHOLE,
	AMDRDNA_FIRMWARE_COMPUTE,
	AMDRDNA_FIRMWARE_RUN_LIST
};

/*
 * The pieces' indexes: the security processor's system driver, secure
 * OS, table of contents and key database; a compute processor's program
 * and jump table; the run-list controller's program, control list, GPM
 * list and SRM list.
 */
#define AMDRDNA_SECURE_SYSTEM_DRIVER	0U
#define AMDRDNA_SECURE_OS		1U
#define AMDRDNA_SECURE_CONTENTS		2U
#define AMDRDNA_SECURE_KEYS		3U
#define AMDRDNA_COMPUTE_PROGRAM		0U
#define AMDRDNA_COMPUTE_JUMPS		1U
#define AMDRDNA_RUN_LIST_PROGRAM	0U
#define AMDRDNA_RUN_LIST_CONTROL	1U
#define AMDRDNA_RUN_LIST_GPM		2U
#define AMDRDNA_RUN_LIST_SRM		3U

/* One piece of a firmware file: its offset in the file and its size. */
struct amdrdna_firmware_piece {
	uint32_t offset;
	uint32_t bytes;
};

/*
 * What a firmware file's header says: its layout version, the version of
 * the program it carries, and the pieces the driver hands on.
 */
struct amdrdna_firmware_layout {
	uint16_t header_major;
	uint16_t header_minor;
	uint32_t version;
	unsigned piece_count;
	struct amdrdna_firmware_piece pieces[AMDRDNA_FIRMWARE_PIECES];
};

/*
 * The security processor's ring and command buffers: a frame of 64
 * bytes, a ring of 4 KiB, a command of 1024 bytes; and the reserve at the
 * top of VRAM memory training assumes without one from the video BIOS.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (struct psp_gfx_rb_frame,
 * struct psp_gfx_cmd_resp), amdgpu_psp.c (psp_ring_init: 0x1000) and
 * drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.h (DISCOVERY_TMR_OFFSET).
 */
#define AMDRDNA_PSP_FRAME_BYTES		64U
#define AMDRDNA_PSP_RING_BYTES		4096U
#define AMDRDNA_PSP_COMMAND_BYTES	1024U
#define AMDRDNA_TRAINING_DEFAULT_RESERVE 0x10000U

/*
 * The program types the security processor loads: the graphics pipe's ME,
 * PFP and CE, the compute processor, the run-list controller's program,
 * the two copy engines, the system-management unit, and the run-list
 * controller's GPM, SRM and control lists.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h (enum psp_gfx_fw_type:
 * GFX_FW_TYPE_CP_ME, GFX_FW_TYPE_CP_PFP, GFX_FW_TYPE_CP_CE,
 * GFX_FW_TYPE_CP_MEC, GFX_FW_TYPE_RLC_G, GFX_FW_TYPE_SDMA0,
 * GFX_FW_TYPE_SDMA1, GFX_FW_TYPE_SMU,
 * GFX_FW_TYPE_RLC_RESTORE_LIST_GPM_MEM,
 * GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_MEM,
 * GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_CNTL).
 */
#define AMDRDNA_PROGRAM_ME		1U
#define AMDRDNA_PROGRAM_PFP		2U
#define AMDRDNA_PROGRAM_CE		3U
#define AMDRDNA_PROGRAM_COMPUTE		4U
#define AMDRDNA_PROGRAM_RUN_LIST	8U
#define AMDRDNA_PROGRAM_COPY0		9U
#define AMDRDNA_PROGRAM_COPY1		10U
#define AMDRDNA_PROGRAM_SMU		18U
#define AMDRDNA_PROGRAM_LIST_GPM	20U
#define AMDRDNA_PROGRAM_LIST_SRM	21U
#define AMDRDNA_PROGRAM_LIST_CONTROL	22U

/*
 * The program types of the video engine: its firmware, which the security
 * processor loads after the run-list controller's, and the table of
 * register writes the security processor applies when the engine starts
 * under its dynamic power gating.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h
 * (enum psp_gfx_fw_type: GFX_FW_TYPE_VCN, GFX_FW_TYPE_VCN0_RAM).
 */
#define AMDRDNA_PROGRAM_VIDEO		13U
#define AMDRDNA_PROGRAM_VIDEO_TABLE	49U

/*
 * The reply the secure OS writes into a command buffer: the status (0 is
 * success), the session of a loaded application, where a loaded program
 * went in the trusted region, and the region size the table of contents
 * asks for.
 */
struct amdrdna_psp_reply {
	uint32_t status;
	uint32_t session;
	uint64_t place;
	uint32_t region_bytes;
};

/*
 * The engines a queue lives on, as the queue select and the scheduler
 * name them: the graphics engine, and the first and second compute
 * engines.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_compute_ring_init:
 * the compute engine of a ring is its compute processor plus one; the
 * graphics ring is engine 0).
 */
#define AMDRDNA_ENGINE_GRAPHICS		0U
#define AMDRDNA_ENGINE_COMPUTE_FIRST	1U
#define AMDRDNA_ENGINE_COMPUTE_SECOND	2U

/*
 * The engine kinds of the scheduler's queue attach and detach packets
 * (compute 0, graphics 4), and the detach actions the driver uses
 * (preempt 0, reset 1).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx10_kiq_map_queues: the engine
 * selection) and amdgpu_gfx.h (enum amdgpu_unmap_queues_action).
 */
#define AMDRDNA_QUEUE_ENGINE_COMPUTE	0U
#define AMDRDNA_QUEUE_ENGINE_GRAPHICS	4U
#define AMDRDNA_QUEUE_DETACH_PREEMPT	0U
#define AMDRDNA_QUEUE_DETACH_RESET	1U

/*
 * The command packets of the graphics and compute queues, shared by the
 * driver's ring commands (command-format.c) and the Vulkan executor's
 * indirect buffers (vk/): the type-3 header (the type in bits 31:30, the
 * body's words less one in 29:16, the opcode in 15:8, the compute
 * pipeline's mark in bit 1), the count of the one-word filler that has no
 * body, and every opcode either of them writes.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET_TYPE3, PACKET3, PACKET3_COMPUTE, PACKET3_NOP,
 * PACKET3_SET_BASE, PACKET3_CLEAR_STATE, PACKET3_DISPATCH_DIRECT,
 * PACKET3_CONTEXT_CONTROL, PACKET3_DRAW_INDEX_AUTO, PACKET3_NUM_INSTANCES,
 * PACKET3_WRITE_DATA, PACKET3_WAIT_REG_MEM, PACKET3_INDIRECT_BUFFER,
 * PACKET3_PFP_SYNC_ME, PACKET3_EVENT_WRITE, PACKET3_RELEASE_MEM,
 * PACKET3_PREAMBLE_CNTL, PACKET3_DMA_DATA, PACKET3_ACQUIRE_MEM,
 * PACKET3_SET_CONFIG_REG, PACKET3_SET_CONTEXT_REG, PACKET3_SET_SH_REG,
 * PACKET3_SET_UCONFIG_REG, PACKET3_SET_UCONFIG_REG_INDEX,
 * PACKET3_SET_RESOURCES, PACKET3_MAP_QUEUES, PACKET3_UNMAP_QUEUES),
 * Mesa 25.0.7 src/amd/common/sid.h (PKT3_SET_SH_REG_INDEX 0x9B) and
 * gfx_v10_0.c (the ring's nop word, PACKET3(PACKET3_NOP, 0x3FFF)).
 */
#define AMDRDNA_PM4_TYPE3			0xc0000000U
#define AMDRDNA_PM4_COUNT_SHIFT			16U
#define AMDRDNA_PM4_COUNT_MASK			0x3fffU
#define AMDRDNA_PM4_OPCODE_SHIFT		8U
#define AMDRDNA_PM4_COMPUTE			0x00000002U
#define AMDRDNA_PM4_FILLER_COUNT		0x3fffU
#define AMDRDNA_PM4_OP_NOTHING			0x10U
#define AMDRDNA_PM4_OP_PARTITION		0x11U
#define AMDRDNA_PM4_OP_STATE_RESET		0x12U
#define AMDRDNA_PM4_OP_DISPATCH			0x15U
#define AMDRDNA_PM4_OP_CONTEXT_LOADS		0x28U
#define AMDRDNA_PM4_OP_DRAW_GENERATED		0x2dU
#define AMDRDNA_PM4_OP_INSTANCES		0x2fU
#define AMDRDNA_PM4_OP_WRITE_VALUE		0x37U
#define AMDRDNA_PM4_OP_WAIT_VALUE		0x3cU
#define AMDRDNA_PM4_OP_INDIRECT			0x3fU
#define AMDRDNA_PM4_OP_FETCHER_SYNC		0x42U
#define AMDRDNA_PM4_OP_EVENT			0x46U
#define AMDRDNA_PM4_OP_EVENT_FENCE		0x49U
#define AMDRDNA_PM4_OP_STATE_BRACKET		0x4aU
#define AMDRDNA_PM4_OP_MOVE			0x50U
#define AMDRDNA_PM4_OP_CACHE_SYNC		0x58U
#define AMDRDNA_PM4_OP_CONFIG_REGISTERS		0x68U
#define AMDRDNA_PM4_OP_CONTEXT_REGISTERS	0x69U
#define AMDRDNA_PM4_OP_SHADER_REGISTERS		0x76U
#define AMDRDNA_PM4_OP_USER_REGISTERS		0x79U
#define AMDRDNA_PM4_OP_USER_REGISTERS_INDEXED	0x7aU
#define AMDRDNA_PM4_OP_SHADER_REGISTERS_INDEXED	0x9bU
#define AMDRDNA_PM4_OP_QUEUE_RESOURCES		0xa0U
#define AMDRDNA_PM4_OP_QUEUE_ATTACH		0xa2U
#define AMDRDNA_PM4_OP_QUEUE_DETACH		0xa3U

/*
 * The first register of each space the register writes address relative
 * to (dword indices): the configuration, context, shader and user
 * configuration registers; and where the indexed user configuration write
 * puts its index (bits 31:28 of the register's word).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET3_SET_CONFIG_REG_START, PACKET3_SET_CONTEXT_REG_START,
 * PACKET3_SET_SH_REG_START, PACKET3_SET_UCONFIG_REG_START) and Mesa 25.0.7
 * src/amd/vulkan/radv_cs.h (radeon_set_uconfig_reg_idx: the index from
 * bit 28).
 */
#define AMDRDNA_PM4_CONFIG_FIRST		0x2000U
#define AMDRDNA_PM4_CONTEXT_FIRST		0xa000U
#define AMDRDNA_PM4_SHADER_FIRST		0x2c00U
#define AMDRDNA_PM4_USER_FIRST			0xc000U
#define AMDRDNA_PM4_REGISTER_INDEX_SHIFT	28U

/*
 * The value wait's control word: the comparison (bits 2:0, 3 for equal)
 * and the space it reads (bit 4: 1 memory); and the poll interval of a
 * memory wait (4).  The memory acquire's fixed words: the whole address
 * space (two words) and the poll interval.  The end-of-pipe release's
 * data field: the low 32 bits of data (1 at bit 29).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (WAIT_REG_MEM_FUNCTION, WAIT_REG_MEM_MEM_SPACE,
 * PACKET3_RELEASE_MEM_DATA_SEL) and gfx_v10_0.c
 * (gfx_v10_0_ring_emit_pipeline_sync: the poll interval;
 * gfx_v10_0_emit_mem_sync: the acquire's size and poll interval).
 */
#define AMDRDNA_PM4_WAIT_EQUAL			0x00000003U
#define AMDRDNA_PM4_WAIT_IN_MEMORY		0x00000010U
#define AMDRDNA_PM4_WAIT_MEMORY_POLL		0x00000004U
#define AMDRDNA_PM4_SYNC_SIZE_LOW		0xffffffffU
#define AMDRDNA_PM4_SYNC_SIZE_HIGH		0x00ffffffU
#define AMDRDNA_PM4_SYNC_POLL			0x0000000aU
#define AMDRDNA_PM4_FENCE_DATA_LOW		0x20000000U

/*
 * The GPU's free-running timestamp counter written to memory: the word
 * copy's opcode, its control word with the counter as the source (9),
 * memory as the destination (5 at bit 8), both words of the counter (bit
 * 16) and the write confirmed (bit 20); and an event fence's data
 * selector for the counter at the end of the pipe (3 at bit 29).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/nvd.h
 * (PACKET3_COPY_DATA and its field positions) and Mesa 25.0.7
 * src/amd/common/sid.h (COPY_DATA_TIMESTAMP, COPY_DATA_DST_MEM,
 * COPY_DATA_COUNT_SEL, COPY_DATA_WR_CONFIRM, EOP_DATA_SEL_TIMESTAMP).
 * derived: 9 | 5 << 8 | 1 << 16 | 1 << 20 = 0x00110509; 3 << 29 =
 * 0x60000000.
 */
#define AMDRDNA_PM4_OP_COPY_WORDS		0x40U
#define AMDRDNA_PM4_COPY_COUNTER_TO_MEMORY	0x00110509U
#define AMDRDNA_PM4_FENCE_DATA_COUNTER		0x60000000U

/* The words of the packets the driver builds. */
#define AMDRDNA_PM4_WRITE_WORDS		5U
#define AMDRDNA_PM4_FENCE_WORDS		8U
#define AMDRDNA_PM4_STAMP_WORDS		6U
#define AMDRDNA_PM4_SYNC_WORDS		8U
#define AMDRDNA_PM4_MOVE_WORDS		7U
#define AMDRDNA_PM4_DISPATCH_WORDS	5U
#define AMDRDNA_PM4_RESOURCES_WORDS	8U
#define AMDRDNA_PM4_ATTACH_WORDS	7U
#define AMDRDNA_PM4_DETACH_WORDS	6U
#define AMDRDNA_PM4_INDIRECT_WORDS	4U
#define AMDRDNA_PM4_WAIT_WORDS		7U
#define AMDRDNA_PM4_FETCH_SYNC_WORDS	2U
#define AMDRDNA_COPY_WRITE_WORDS	5U
#define AMDRDNA_COPY_LINEAR_WORDS	7U
#define AMDRDNA_COPY_FILL_WORDS		5U
#define AMDRDNA_COPY_FENCE_WORDS	6U

/*
 * The register spaces the register-write packets address: the context
 * registers, the shader registers and the user configuration registers.
 */
enum amdrdna_register_space {
	AMDRDNA_SPACE_CONTEXT,
	AMDRDNA_SPACE_SHADER,
	AMDRDNA_SPACE_USER
};

/* One hardware queue of the command processor: its engine (AMDRDNA_ENGINE_*), pipe and queue. */
struct amdrdna_queue_place {
	uint32_t engine;
	uint32_t pipe;
	uint32_t queue;
};

/*
 * The contexts of the graphics hub: context 0 is the kernel's flat
 * space, contexts 1 to 15 are handed to the address spaces of the GPU's
 * sessions while they run work.
 */
#define AMDRDNA_CONTEXTS		16U
#define AMDRDNA_FIRST_USER_CONTEXT	1U

/*
 * The rings a context's work is tracked on: the graphics ring, the four
 * compute queues and the first copy engine.
 */
#define AMDRDNA_TRACKED_RINGS		6U
#define AMDRDNA_TRACKED_GRAPHICS	0U
#define AMDRDNA_TRACKED_COMPUTE		1U
#define AMDRDNA_TRACKED_COPY		5U

/*
 * The address-space identifiers the interrupt controller attaches to a
 * page fault: 1 to 65535 (0 names no space), one bit each.
 */
#define AMDRDNA_SPACE_IDS		65536U
#define AMDRDNA_SPACE_ID_WORDS		(AMDRDNA_SPACE_IDS / 32U)

/*
 * What one hub context is doing for the address spaces.
 *
 * owner is the identifier of the address space the context is handed to
 * (0 when it is nobody's) and table its root table's word.  flushed is
 * the owner's table generation the context's cached translations were
 * last dropped for; flush_ring and flush_sequence name the fence after
 * that drop, which a run on another ring must see passed before it may
 * skip its own drop (flush_known says there was one).  last holds, for
 * each tracked ring that ran work in the context (used), the fence after
 * the latest such work: the context may be handed to another space only
 * once all of them passed.  stamp orders the contexts from the least to
 * the most recently used.  held keeps a context that may still run work
 * nobody can stop out of the pool for good.
 */
struct amdrdna_context_slot {
	uint32_t owner;
	uint64_t table;
	uint64_t flushed;
	bool flush_known;
	unsigned flush_ring;
	uint32_t flush_sequence;
	bool used[AMDRDNA_TRACKED_RINGS];
	uint32_t last[AMDRDNA_TRACKED_RINGS];
	uint64_t stamp;
	bool held;
};

/*
 * The pool of hub contexts the address spaces share (contexts 1 to 15):
 * the slots, the clock their stamps come from, and the counts of drops
 * and of contexts taken from another space.  The owner serializes every
 * call.
 */
struct amdrdna_context_pool {
	struct amdrdna_context_slot slots[AMDRDNA_CONTEXTS];
	uint64_t clock;
	uint64_t flushes;
	uint64_t takeovers;
};

/*
 * What a run asks of the context pool: the address space's identifier,
 * the context it held last (0 for none), its root table's word, its table
 * generation, and the tracked ring the run goes to.
 */
struct amdrdna_context_request {
	uint32_t owner;
	unsigned held;
	uint64_t table;
	uint64_t generation;
	unsigned ring;
};

/*
 * What the context pool answers a run: the context, whether its cached
 * translations must be dropped before the run (flush) and whether the run
 * must first wait on its own ring for that ring's earlier work (sync); or
 * that no context is free yet and the caller waits for the fence
 * wait_sequence of tracked ring wait_ring and asks again (wait).
 */
struct amdrdna_context_grant {
	unsigned context;
	bool flush;
	bool sync;
	bool wait;
	unsigned wait_ring;
	uint32_t wait_sequence;
};

/*
 * The address-space identifiers in use, one bit each, and where the
 * search for a free one starts (identifiers are not reused at once).  The
 * owner serializes every call.
 */
struct amdrdna_space_ids {
	uint32_t used[AMDRDNA_SPACE_ID_WORDS];
	uint32_t next;
	uint32_t count;
};

/* The words of a queue image. */
#define AMDRDNA_QUEUE_IMAGE_WORDS	512U

/*
 * The words of a compute queue image the driver fills: the header, the
 * pipeline statistics, the compute units of the four shader engines, the
 * reserved word; then, mirroring the queue's registers, the image's own
 * address (low, high), active, context, persistent state, the pipe and
 * queue priorities, the ring (low, high), the read pointer and its report
 * address (low, high), the write pointer's poll address (low, high), the
 * doorbell, the ring control, the indirect-buffer control, the dequeue
 * request, the image control, the end-of-pipe buffer (low, high) and its
 * control, and the write pointer (low, high).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/v10_structs.h (struct v10_compute_mqd).
 */
#define AMDRDNA_COMPUTE_IMAGE_HEADER		0U
#define AMDRDNA_COMPUTE_IMAGE_STATISTICS	11U
#define AMDRDNA_COMPUTE_IMAGE_UNITS_0		23U
#define AMDRDNA_COMPUTE_IMAGE_UNITS_1		24U
#define AMDRDNA_COMPUTE_IMAGE_UNITS_2		26U
#define AMDRDNA_COMPUTE_IMAGE_UNITS_3		27U
#define AMDRDNA_COMPUTE_IMAGE_RESERVED		32U
#define AMDRDNA_COMPUTE_IMAGE_SELF_LOW		128U
#define AMDRDNA_COMPUTE_IMAGE_SELF_HIGH		129U
#define AMDRDNA_COMPUTE_IMAGE_ACTIVE		130U
#define AMDRDNA_COMPUTE_IMAGE_CONTEXT		131U
#define AMDRDNA_COMPUTE_IMAGE_PERSISTENT	132U
#define AMDRDNA_COMPUTE_IMAGE_PIPE_PRIORITY	133U
#define AMDRDNA_COMPUTE_IMAGE_QUEUE_PRIORITY	134U
#define AMDRDNA_COMPUTE_IMAGE_RING_LOW		136U
#define AMDRDNA_COMPUTE_IMAGE_RING_HIGH		137U
#define AMDRDNA_COMPUTE_IMAGE_READ		138U
#define AMDRDNA_COMPUTE_IMAGE_READ_REPORT_LOW	139U
#define AMDRDNA_COMPUTE_IMAGE_READ_REPORT_HIGH	140U
#define AMDRDNA_COMPUTE_IMAGE_WRITE_POLL_LOW	141U
#define AMDRDNA_COMPUTE_IMAGE_WRITE_POLL_HIGH	142U
#define AMDRDNA_COMPUTE_IMAGE_DOORBELL		143U
#define AMDRDNA_COMPUTE_IMAGE_RING_CONTROL	145U
#define AMDRDNA_COMPUTE_IMAGE_BUFFER_CONTROL	149U
#define AMDRDNA_COMPUTE_IMAGE_DEQUEUE		152U
#define AMDRDNA_COMPUTE_IMAGE_CONTROL		162U
#define AMDRDNA_COMPUTE_IMAGE_END_LOW		165U
#define AMDRDNA_COMPUTE_IMAGE_END_HIGH		166U
#define AMDRDNA_COMPUTE_IMAGE_END_CONTROL	167U
#define AMDRDNA_COMPUTE_IMAGE_WRITE_LOW		182U
#define AMDRDNA_COMPUTE_IMAGE_WRITE_HIGH	183U

/*
 * The words of the graphics queue image the driver fills: the image's own
 * address (low, high), active, context, priority, time quantum, the ring
 * (low, high), the read pointer and its report address (low, high), the
 * write pointer's poll address (low, high), the doorbell, the ring
 * control, the write pointer (low, high) and the image control.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/v10_structs.h (struct v10_gfx_mqd).
 */
#define AMDRDNA_GRAPHICS_IMAGE_SELF_LOW		128U
#define AMDRDNA_GRAPHICS_IMAGE_SELF_HIGH	129U
#define AMDRDNA_GRAPHICS_IMAGE_ACTIVE		130U
#define AMDRDNA_GRAPHICS_IMAGE_CONTEXT		131U
#define AMDRDNA_GRAPHICS_IMAGE_PRIORITY		134U
#define AMDRDNA_GRAPHICS_IMAGE_QUANTUM		135U
#define AMDRDNA_GRAPHICS_IMAGE_RING_LOW		136U
#define AMDRDNA_GRAPHICS_IMAGE_RING_HIGH	137U
#define AMDRDNA_GRAPHICS_IMAGE_READ		138U
#define AMDRDNA_GRAPHICS_IMAGE_READ_REPORT_LOW	139U
#define AMDRDNA_GRAPHICS_IMAGE_READ_REPORT_HIGH	140U
#define AMDRDNA_GRAPHICS_IMAGE_WRITE_POLL_LOW	141U
#define AMDRDNA_GRAPHICS_IMAGE_WRITE_POLL_HIGH	142U
#define AMDRDNA_GRAPHICS_IMAGE_DOORBELL		143U
#define AMDRDNA_GRAPHICS_IMAGE_RING_CONTROL	145U
#define AMDRDNA_GRAPHICS_IMAGE_WRITE_LOW	149U
#define AMDRDNA_GRAPHICS_IMAGE_WRITE_HIGH	150U
#define AMDRDNA_GRAPHICS_IMAGE_CONTROL		162U

/*
 * What a compute queue image is filled from: the GPU addresses of the
 * image, the ring (its size in bytes), the read pointer's report word, the
 * write pointer's copy and the end-of-pipe buffer; the doorbell (a dword
 * index); whether the driver activates the queue itself; and the values
 * the queue's end-of-pipe control, doorbell, image control, ring control,
 * read pointer, persistent state and buffer control registers held.
 */
struct amdrdna_compute_image_input {
	uint64_t image;
	uint64_t ring;
	uint32_t ring_bytes;
	uint64_t read_report;
	uint64_t write_poll;
	uint64_t end_of_pipe;
	uint32_t doorbell;
	bool active;
	uint32_t end_control;
	uint32_t doorbell_control;
	uint32_t image_control;
	uint32_t ring_control;
	uint32_t read_pointer;
	uint32_t persistent;
	uint32_t buffer_control;
};

/*
 * What the graphics queue image is filled from: the GPU addresses of the
 * image, the ring (its size in bytes), the read pointer's report word and
 * the write pointer's copy; the doorbell; and the values the queue's image
 * control, priority, quantum, ring control, doorbell control and read
 * pointer registers held.
 */
struct amdrdna_graphics_image_input {
	uint64_t image;
	uint64_t ring;
	uint32_t ring_bytes;
	uint64_t read_report;
	uint64_t write_poll;
	uint32_t doorbell;
	uint32_t image_control;
	uint32_t priority;
	uint32_t quantum;
	uint32_t ring_control;
	uint32_t doorbell_control;
	uint32_t read_pointer;
};

/*
 * One recommended register setting: the GC window and the register's
 * offset in it, the bits to change (all of them when every bit is set)
 * and their value.
 */
struct amdrdna_register_setting {
	uint8_t window;
	uint16_t offset;
	uint32_t mask;
	uint32_t value;
};

/* One run of context registers of the clear state: the first (an absolute index) and how many. */
struct amdrdna_register_run {
	uint32_t first;
	uint32_t count;
};

/* One register of the clear state and its value. */
struct amdrdna_register_value {
	uint32_t index;
	uint32_t value;
};

/* The longest run of the clear state. */
#define AMDRDNA_CLEAR_RUN_LONGEST	272U

/*
 * The compute program the first dispatch runs: its code (code_words
 * words, to be followed by end_word up to padded_words), the register
 * values the compiler gave (counts and inputs), the workgroup size, where
 * its two buffer descriptors go in its table and the table's size, and
 * the dispatch's launch word.
 */
struct amdrdna_check_description {
	const uint32_t *code;
	uint32_t code_words;
	uint32_t padded_words;
	uint32_t end_word;
	uint32_t counts;
	uint32_t inputs;
	uint32_t local_size[3];
	uint32_t output_at;
	uint32_t groups_at;
	uint32_t table_bytes;
	uint32_t initiator;
};

uint16_t
amdrdna_le16(
	const uint8_t *bytes,
	size_t at);

uint32_t
amdrdna_le32(
	const uint8_t *bytes,
	size_t at);

int
amdrdna_block_table_parse(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_block_table *table);

int
amdrdna_boot_database_parse(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_boot_database *database);

void
amdrdna_boot_database_set_config(
	struct amdrdna_boot_database *database,
	const uint8_t *entry);

int
amdrdna_vbios_find_in_acpi(
	const uint8_t *table,
	size_t length,
	const struct amdrdna_vbios_owner *owner,
	size_t *image_offset,
	size_t *image_length);

int
amdrdna_vbios_check(
	const uint8_t *image,
	size_t length);

uint32_t
amdrdna_vbios_declared_length(
	const uint8_t *image);

int
amdrdna_vbios_parse(
	const uint8_t *image,
	size_t length,
	struct amdrdna_vbios_info *info);

void
amdrdna_command_pixel_clock(
	uint32_t *words,
	const struct amdrdna_pixel_clock_command *command);

void
amdrdna_command_encoder(
	uint32_t *words,
	const struct amdrdna_encoder_command *command);

void
amdrdna_command_transmitter(
	uint32_t *words,
	const struct amdrdna_transmitter_command *command);

void
amdrdna_command_pipe(
	uint32_t *words,
	uint32_t generator,
	uint32_t on);

int
amdrdna_mode_plan(
	const struct amdrdna_mode_plan_input *input,
	struct amdrdna_mode_plan *plan);

int
amdrdna_vbios_connector(
	const uint8_t *image,
	size_t length,
	const struct amdrdna_vbios_info *info,
	unsigned transmitter,
	struct amdrdna_vbios_connector *connector);

int
amdrdna_picture_check(
	const struct amdrdna_picture_input *input,
	struct amdrdna_picture *picture);

const char *
amdrdna_picture_shape_name(
	enum amdrdna_picture_shape shape);

int
amdrdna_layout_plan(
	const struct amdrdna_layout_input *input,
	struct amdrdna_layout *layout);

const char *
amdrdna_range_reason_name(
	enum amdrdna_range_reason reason);

const struct amdrdna_product *
amdrdna_product_find(
	uint16_t device);

const char *
amdrdna_generation_name(
	enum amdrdna_generation generation);

const char *
amdrdna_graphics_chip_name(
	const struct amdrdna_ip *graphics);

uint32_t
amdrdna_dcn_index(
	const struct amdrdna_ip *display,
	enum amdrdna_dcn_register which,
	unsigned instance);

void
amdrdna_cursor_locate(
	int32_t x,
	int32_t y,
	uint32_t side,
	uint32_t plane_width,
	uint32_t plane_height,
	struct amdrdna_cursor_place *place);

uint32_t
amdrdna_cursor_control_word(
	uint32_t current,
	bool shown);

uint32_t
amdrdna_cursor_size_word(
	uint32_t side);

uint32_t
amdrdna_cursor_timing_word(void);

uint32_t
amdrdna_pixel_cursor_word(
	uint32_t current,
	bool shown);

uint32_t
amdrdna_frame_advance(
	uint32_t before,
	uint32_t after);

uint32_t
amdrdna_refresh_millihertz(
	uint32_t frames,
	uint64_t microseconds);

uint32_t
amdrdna_sync_status_word(
	uint32_t current,
	bool frame_interrupt,
	bool acknowledge);

uint32_t
amdrdna_flip_interrupt_word(
	uint32_t current,
	bool enabled,
	bool acknowledge);

uint32_t
amdrdna_hotplug_control_word(
	uint32_t current,
	bool enabled,
	bool connected,
	bool acknowledge,
	bool acknowledge_short);

bool
amdrdna_hotplug_connected(
	uint32_t status);

uint32_t
amdrdna_flip_control_word(
	uint32_t current);

bool
amdrdna_flip_pending(
	uint32_t flip_control);

const char *
amdrdna_block_name(
	enum amdrdna_block block);

void
amdrdna_pool_init(
	struct amdrdna_pool *pool,
	uint64_t start,
	uint64_t end);

int
amdrdna_pool_give(
	struct amdrdna_pool *pool,
	uint64_t start,
	uint64_t bytes);

void
amdrdna_pool_hold(
	struct amdrdna_pool *pool,
	uint64_t start,
	uint64_t bytes);

int
amdrdna_pool_take(
	struct amdrdna_pool *pool,
	uint64_t bytes,
	uint64_t alignment,
	uint64_t *start);

uint32_t
amdrdna_hub_index(
	const struct amdrdna_ip *block,
	enum amdrdna_hub hub,
	enum amdrdna_hub_register which,
	unsigned instance);

unsigned
amdrdna_hub_instances(
	enum amdrdna_hub_register which);

int
amdrdna_gart_place(
	uint64_t frame_buffer_start,
	uint64_t frame_buffer_end,
	uint64_t bytes,
	uint64_t *start);

uint64_t
amdrdna_gart_system_entry(
	uint64_t bus_address);

uint64_t
amdrdna_gart_unused_entry(
	uint64_t dummy_address);

uint64_t
amdrdna_vm_leaf_entry(
	uint64_t address,
	uint64_t access);

uint64_t
amdrdna_vm_table_entry(
	uint64_t table_address,
	bool in_system);

void
amdrdna_vm_split(
	uint64_t address,
	unsigned index[AMDRDNA_VM_LEVELS]);

uint32_t
amdrdna_invalidate_word(
	unsigned context);

uint32_t
amdrdna_hub_cache_word(
	uint32_t old);

uint32_t
amdrdna_hub_cache_flush_word(
	uint32_t old);

uint32_t
amdrdna_hub_cache_word_3(void);

uint32_t
amdrdna_hub_cache_word_4(void);

uint32_t
amdrdna_hub_cache_word_5(void);

uint32_t
amdrdna_hub_translation_word(
	uint32_t old);

uint32_t
amdrdna_hub_system_context_word(
	uint32_t old);

uint32_t
amdrdna_hub_user_context_word(
	uint32_t old);

uint32_t
amdrdna_hub_fault_default_word(
	uint32_t old);

uint32_t
amdrdna_hub_fault_retry_word(
	uint32_t old);

uint32_t
amdrdna_hub_fault_rearm_word(
	uint32_t old);

uint32_t
amdrdna_hub_fault_interrupt_word(
	uint32_t old,
	bool on);

void
amdrdna_vm_fault_decode(
	const struct amdrdna_interrupt *interrupt,
	uint32_t latched,
	struct amdrdna_vm_fault *fault);

void
amdrdna_interrupt_decode(
	const uint32_t words[AMDRDNA_INTERRUPT_ENTRY_BYTES / 4U],
	struct amdrdna_interrupt *interrupt);

int
amdrdna_interrupt_ring_word(
	uint32_t old,
	uint32_t ring_bytes,
	bool message,
	uint32_t *word);

uint32_t
amdrdna_interrupt_switch_word(
	uint32_t old,
	bool on);

uint32_t
amdrdna_interrupt_overflow_word(
	uint32_t old,
	bool clear);

uint32_t
amdrdna_interrupt_doorbell_word(
	uint32_t doorbell);

uint32_t
amdrdna_interrupt_doorbell_range_word(
	uint32_t old,
	uint32_t doorbell);

uint32_t
amdrdna_interrupt_control_word(
	uint32_t old);

uint32_t
amdrdna_interrupt_offset(
	uint32_t pointer,
	uint32_t ring_bytes,
	bool *overflowed);


int
amdrdna_firmware_layout(
	enum amdrdna_firmware_kind kind,
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_firmware_layout *layout);

void
amdrdna_psp_frame(
	uint8_t *frame,
	uint64_t command,
	uint64_t fence,
	uint32_t fence_sequence);

int
amdrdna_psp_ring_step(
	uint32_t write_pointer,
	uint32_t *frame,
	uint32_t *next);

void
amdrdna_psp_command(
	uint8_t *command,
	uint32_t id);

void
amdrdna_psp_command_program(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes,
	uint32_t type);

void
amdrdna_psp_command_contents(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes);

void
amdrdna_psp_command_region(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes,
	uint64_t physical);

void
amdrdna_psp_command_application(
	uint8_t *command,
	uint64_t address,
	uint32_t bytes);

void
amdrdna_psp_command_unload(
	uint8_t *command,
	uint32_t session);

void
amdrdna_psp_command_plain(
	uint8_t *command,
	bool tear_down);

void
amdrdna_psp_reply_read(
	const uint8_t *command,
	struct amdrdna_psp_reply *reply);

void
amdrdna_training_place(
	uint64_t vram_bytes,
	uint64_t reserve_bytes,
	uint64_t *driver_copy,
	uint64_t *processor_copy);

void
amdrdna_temperature_read(
	uint32_t word,
	unsigned *sensor,
	unsigned *highest);

int
amdrdna_fan_duty_word(
	uint32_t old,
	uint32_t limits,
	unsigned percent,
	uint32_t *word);

uint32_t
amdrdna_fan_mode_word(
	uint32_t old);

uint32_t
amdrdna_fan_speed(
	uint32_t tachometer);

unsigned
amdrdna_fan_driven(
	uint32_t status,
	uint32_t limits);

uint32_t
amdrdna_alert_word(
	uint32_t old,
	unsigned high,
	unsigned low);

uint32_t
amdrdna_alert_off_word(
	uint32_t old);

uint32_t
amdrdna_alert_clear_word(void);

int
amdrdna_power_table_in_firmware(
	const uint8_t *file,
	size_t length,
	uint32_t identity,
	size_t *offset,
	size_t *bytes);

int
amdrdna_power_table_build(
	const uint8_t *power,
	size_t power_bytes,
	const uint8_t *board,
	size_t board_bytes,
	unsigned board_format,
	unsigned board_content,
	uint8_t *table,
	struct amdrdna_power_limits *limits);

void
amdrdna_power_readings_read(
	const uint8_t *readings,
	struct amdrdna_power_readings *result);

bool
amdrdna_power_features_on(
	uint32_t low,
	uint32_t high);

void
amdrdna_power_settings_build(
	uint8_t *table);

void
amdrdna_power_watermarks_build(
	const uint32_t *memory_mhz,
	unsigned count,
	uint8_t *table);

/* command-format.c: the ring commands. */
uint32_t
amdrdna_pm4_filler(void);

uint32_t
amdrdna_pm4_header(
	uint32_t opcode,
	unsigned body_words,
	bool compute);

unsigned
amdrdna_pm4_registers(
	uint32_t *words,
	enum amdrdna_register_space space,
	uint32_t first,
	const uint32_t *values,
	unsigned count,
	bool compute);

unsigned
amdrdna_pm4_write_memory(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool compute);

unsigned
amdrdna_pm4_write_register(
	uint32_t *words,
	uint32_t index,
	uint32_t value,
	bool compute);

unsigned
amdrdna_pm4_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool interrupt,
	bool compute);

unsigned
amdrdna_pm4_stamp(
	uint32_t *words,
	uint64_t address,
	bool compute);

unsigned
amdrdna_pm4_stamp_done(
	uint32_t *words,
	uint64_t address,
	bool compute);

unsigned
amdrdna_pm4_cache_sync(
	uint32_t *words,
	bool compute);

unsigned
amdrdna_pm4_move(
	uint32_t *words,
	uint64_t source,
	uint64_t destination,
	uint32_t bytes);

unsigned
amdrdna_pm4_dispatch(
	uint32_t *words,
	uint32_t x,
	uint32_t y,
	uint32_t z,
	uint32_t initiator);

unsigned
amdrdna_pm4_queue_resources(
	uint32_t *words,
	uint64_t queue_mask);

unsigned
amdrdna_pm4_queue_attach(
	uint32_t *words,
	const struct amdrdna_queue_place *place,
	uint32_t engine,
	uint32_t doorbell,
	uint64_t image,
	uint64_t write_pointer);

unsigned
amdrdna_pm4_queue_detach(
	uint32_t *words,
	uint32_t engine,
	uint32_t action,
	uint32_t doorbell);

unsigned
amdrdna_pm4_indirect(
	uint32_t *words,
	uint64_t address,
	uint32_t count,
	unsigned context,
	bool compute);

unsigned
amdrdna_pm4_bracket(
	uint32_t *words,
	bool begin);

unsigned
amdrdna_pm4_context_loads(
	uint32_t *words);

unsigned
amdrdna_pm4_state_reset(
	uint32_t *words);

unsigned
amdrdna_pm4_partition(
	uint32_t *words);

unsigned
amdrdna_pm4_move_cached(
	uint32_t *words,
	uint64_t source,
	uint64_t destination,
	uint32_t bytes);

unsigned
amdrdna_pm4_fill_cached(
	uint32_t *words,
	uint64_t destination,
	uint32_t value,
	uint32_t bytes);

unsigned
amdrdna_pm4_fetcher_write_register(
	uint32_t *words,
	uint32_t index,
	uint32_t value);

unsigned
amdrdna_pm4_write_and_wait(
	uint32_t *words,
	uint32_t request,
	uint32_t done,
	uint32_t value,
	uint32_t mask,
	bool fetcher);

unsigned
amdrdna_pm4_wait_memory(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool fetcher);

unsigned
amdrdna_pm4_wait_register(
	uint32_t *words,
	uint32_t index,
	uint32_t value,
	uint32_t mask);

unsigned
amdrdna_pm4_fetcher_sync(
	uint32_t *words);

uint32_t
amdrdna_copy_filler(void);

unsigned
amdrdna_copy_write(
	uint32_t *words,
	uint64_t address,
	uint32_t value);

unsigned
amdrdna_copy_linear(
	uint32_t *words,
	uint64_t source,
	uint64_t destination,
	uint32_t bytes);

unsigned
amdrdna_copy_fill(
	uint32_t *words,
	uint64_t destination,
	uint32_t value,
	uint32_t bytes);

unsigned
amdrdna_copy_fence(
	uint32_t *words,
	uint64_t address,
	uint32_t value,
	bool interrupt);

/* queue-format.c: the queue images and queue words. */
void
amdrdna_compute_image(
	const struct amdrdna_compute_image_input *input,
	uint32_t *image);

void
amdrdna_graphics_image(
	const struct amdrdna_graphics_image_input *input,
	uint32_t *image);

uint32_t
amdrdna_graphics_ring_word(
	uint32_t ring_bytes);

uint32_t
amdrdna_queue_select_word(
	const struct amdrdna_queue_place *place,
	unsigned context);

uint32_t
amdrdna_scheduler_queue_word(
	uint32_t old,
	const struct amdrdna_queue_place *place);

uint64_t
amdrdna_queue_resource_mask(
	const struct amdrdna_queue_place *places,
	unsigned count);

/* settings.c: the engine settings and the shape words. */
const struct amdrdna_register_setting *
amdrdna_graphics_settings(
	unsigned *count);

const struct amdrdna_register_setting *
amdrdna_copy_settings(
	unsigned *count);

bool
amdrdna_setting_reads(
	const struct amdrdna_register_setting *setting);

uint32_t
amdrdna_setting_word(
	const struct amdrdna_register_setting *setting,
	uint32_t old);

unsigned
amdrdna_clear_state_words(void);

unsigned
amdrdna_clear_state(
	uint32_t *words,
	uint32_t steering);

uint32_t
amdrdna_backends_on(
	uint32_t fused,
	uint32_t off,
	unsigned per_array);

uint32_t
amdrdna_groups_on(
	uint32_t fused,
	uint32_t off,
	unsigned per_array);

unsigned
amdrdna_units_on(
	uint32_t groups);

uint32_t
amdrdna_channels_off(
	uint32_t fused,
	uint32_t off);

uint32_t
amdrdna_tile_steering(
	unsigned converters,
	unsigned backends_per_converter,
	unsigned packers);

void
amdrdna_harvest_words(
	uint32_t groups,
	unsigned per_array,
	uint32_t old_translation,
	uint32_t old_targets,
	uint32_t *translation,
	uint32_t *targets);

uint32_t
amdrdna_shader_memory_mode(void);

uint32_t
amdrdna_shader_apertures(void);

/* check-program.c: the first dispatch's program. */
void
amdrdna_check_program(
	struct amdrdna_check_description *description);

void
amdrdna_raw_buffer_descriptor(
	uint64_t address,
	uint32_t bytes,
	uint32_t *words);

uint32_t
amdrdna_check_output_words(
	const uint32_t *groups);

bool
amdrdna_check_output(
	const uint32_t *words,
	const uint32_t *groups,
	uint32_t *bad_word,
	uint32_t *expected,
	uint32_t *found);

/* context-pool.c: the hub contexts and the address-space identifiers. */
void
amdrdna_context_pool_init(
	struct amdrdna_context_pool *pool);

int
amdrdna_context_grant(
	struct amdrdna_context_pool *pool,
	const struct amdrdna_context_request *request,
	const uint32_t *passed,
	struct amdrdna_context_grant *grant);

void
amdrdna_context_submitted(
	struct amdrdna_context_pool *pool,
	unsigned context,
	const struct amdrdna_context_request *request,
	uint32_t sequence,
	bool flushed);

bool
amdrdna_context_idle(
	const struct amdrdna_context_pool *pool,
	unsigned context,
	const uint32_t *passed);

void
amdrdna_context_release(
	struct amdrdna_context_pool *pool,
	unsigned context,
	uint32_t owner);

void
amdrdna_context_hold(
	struct amdrdna_context_pool *pool,
	unsigned context);

unsigned
amdrdna_context_free_count(
	const struct amdrdna_context_pool *pool);

bool
amdrdna_fence_passed(
	uint32_t fence,
	uint32_t sequence);

void
amdrdna_space_ids_init(
	struct amdrdna_space_ids *ids);

int
amdrdna_space_id_take(
	struct amdrdna_space_ids *ids,
	uint32_t *id);

void
amdrdna_space_id_give(
	struct amdrdna_space_ids *ids,
	uint32_t id);

#endif
