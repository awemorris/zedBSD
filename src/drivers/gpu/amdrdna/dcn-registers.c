/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The addresses of the display registers of the DCN 2.0 display engine.
 *
 * Each pipe's blocks repeat at a fixed distance, so a register is the
 * first instance's offset plus the instance times that distance, in the
 * display block's window the register belongs to.  Two families do not
 * repeat evenly (the legacy VGA controls of the six heads and the pixel
 * processors of pipes 4 and 5): those keep one base per instance, and a
 * register of the family lies at a fixed distance from its instance's base.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

/*
 * Where each named display register lies: the display block's window, the
 * first instance's offset in it, and the distance between instances; or,
 * for the uneven families, one base per instance and the register's
 * distance from that base in the offset.
 */
struct dcn_place {
	uint8_t window;
	uint16_t offset;
	uint16_t stride;
	const uint16_t *instances;
};

/*
 * The legacy VGA controls of the six heads (window 1); bit 0 says the head
 * runs in VGA mode.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmD1VGA_CONTROL to mmD6VGA_CONTROL).
 */
static const uint16_t head_legacy_offsets[AMDRDNA_DISPLAY_PIPES] = {
	0x000cU,
	0x000eU,
	0x0038U,
	0x0039U,
	0x003aU,
	0x003bU
};

/*
 * The control words of the six pixel processors (window 2): pipes 0 to 3
 * repeat, pipes 4 and 5 lie elsewhere.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDPP_TOP0_DPP_CONTROL to mmDPP_TOP5_DPP_CONTROL).
 */
static const uint16_t pixel_processor_offsets[AMDRDNA_DISPLAY_PIPES] = {
	0x0cc5U,
	0x0e30U,
	0x0f9bU,
	0x1106U,
	0x35d0U,
	0x373bU
};

/*
 * The I2C engine's four transaction words (window 2); instances past the
 * fourth name the last.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDC_I2C_TRANSACTION0 to mmDC_I2C_TRANSACTION3).
 */
static const uint16_t i2c_transaction_offsets[AMDRDNA_DISPLAY_PIPES] = {
	0x1eaeU,
	0x1eafU,
	0x1eb0U,
	0x1eb1U,
	0x1eb1U,
	0x1eb1U
};

/*
 * The hub arbiter's four watermark sets A to D (window 2), each the five
 * words urgent, page table urgent, self-refresh entry, self-refresh exit
 * and memory clock change in a row; instances past the fourth name the
 * last.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDCHUBBUB_ARB_DATA_URGENCY_WATERMARK_A to _D, and the four other words
 * of each set after it).
 */
static const uint16_t mark_set_offsets[AMDRDNA_DISPLAY_PIPES] = {
	0x0509U,
	0x050eU,
	0x0513U,
	0x0518U,
	0x0518U,
	0x0518U
};

/*
 * The place of every named display register, in the order of
 * enum amdrdna_dcn_register: the clock controls in window 1, the pipe
 * blocks in window 2, with the plane (0xdc apart), the blend units (0x1b),
 * the blend tree roots (4), the combiners (0x10), the timing generators
 * (0x80), the encoders and their DisplayPort parts (0x100) and the clock
 * controls of the pixel rate and the DisplayPort clocks (4).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_2_0_0_offset.h
 * (mmDENTIST_DISPCLK_CNTL, mmOTG0_PIXEL_RATE_CNTL, mmDP_DTO0_PHASE,
 * mmDP_DTO0_MODULO, mmHUBP0_DCSURF_SURFACE_CONFIG,
 * mmHUBP0_DCSURF_TILING_CONFIG, mmHUBP0_DCSURF_PRI_VIEWPORT_START,
 * mmHUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, mmHUBP0_DCHUBP_CNTL,
 * mmHUBP0_HUBP_CLK_CNTL, mmHUBPREQ0_DCSURF_SURFACE_PITCH,
 * mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,
 * mmHUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,
 * mmHUBPREQ0_DCSURF_SURFACE_CONTROL, mmHUBPREQ0_DCSURF_FLIP_CONTROL,
 * mmHUBPRET0_HUBPRET_CONTROL, mmMPCC0_MPCC_TOP_SEL, mmMPCC0_MPCC_BOT_SEL,
 * mmMPCC0_MPCC_OPP_ID, mmMPCC0_MPCC_CONTROL, mmMPC_OUT0_MUX,
 * mmODM0_OPTC_DATA_SOURCE_SELECT, mmODM0_OPTC_INPUT_GLOBAL_CONTROL,
 * mmOTG0_OTG_H_TOTAL, mmOTG0_OTG_H_BLANK_START_END, mmOTG0_OTG_H_SYNC_A,
 * mmOTG0_OTG_H_SYNC_A_CNTL, mmOTG0_OTG_V_TOTAL,
 * mmOTG0_OTG_V_BLANK_START_END, mmOTG0_OTG_V_SYNC_A,
 * mmOTG0_OTG_V_SYNC_A_CNTL, mmOTG0_OTG_CONTROL,
 * mmOTG0_OTG_MASTER_UPDATE_LOCK, mmDIG0_DIG_FE_CNTL, mmDIG0_DIG_BE_CNTL,
 * mmDIG0_DIG_BE_EN_CNTL, mmDP0_DP_VID_STREAM_CNTL, mmDP0_DP_LINK_CNTL, and
 * the _BASE_IDX of each); the scaler's registers from the same header
 * (mmDSCL0_SCL_MODE, mmDSCL0_SCL_HORZ_FILTER_SCALE_RATIO,
 * mmDSCL0_SCL_VERT_FILTER_SCALE_RATIO, mmDSCL0_RECOUT_START,
 * mmDSCL0_RECOUT_SIZE, mmDSCL0_DSCL_AUTOCAL, and the same registers of
 * DSCL1 to DSCL5).
 * The registers the display's takeover adds, from the same header: the
 * plane's context select, flip interrupt and the two words of the address
 * it reads (mmHUBPREQ0_VMID_SETTINGS_0,
 * mmHUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT,
 * mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE,
 * mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH), the cursor's request
 * timing (mmHUBPREQ0_CURSOR_SETTINGS), the plane's cursor
 * (mmCURSOR0_0_CURSOR_CONTROL, mmCURSOR0_0_CURSOR_SURFACE_ADDRESS,
 * mmCURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH, mmCURSOR0_0_CURSOR_SIZE,
 * mmCURSOR0_0_CURSOR_POSITION, mmCURSOR0_0_CURSOR_HOT_SPOT,
 * mmCURSOR0_0_CURSOR_DST_OFFSET), the pixel processor's cursor
 * (mmCNVC_CUR0_CURSOR0_CONTROL, mmCNVC_CUR0_CURSOR0_FP_SCALE_BIAS and the
 * same registers of CNVC_CUR1 to CNVC_CUR5), the timing generator's
 * position, frame count and sync status (mmOTG0_OTG_STATUS_POSITION,
 * mmOTG0_OTG_STATUS_FRAME_COUNT, mmOTG0_OTG_GLOBAL_SYNC_STATUS) and the
 * hot-plug lines (mmHPD0_DC_HPD_INT_STATUS, mmHPD0_DC_HPD_INT_CONTROL).
 * The registers the reading of the sinks adds, from the same header: the
 * I2C engine's control, arbitration, status, each line's status, speed
 * and setup, the data port (mmDC_I2C_CONTROL, mmDC_I2C_ARBITRATION,
 * mmDC_I2C_SW_STATUS, mmDC_I2C_DDC1_HW_STATUS, mmDC_I2C_DDC1_SPEED,
 * mmDC_I2C_DDC1_SETUP and the same of DDC2 to DDC6, mmDC_I2C_DATA), the
 * microsecond time base (mmMICROSECOND_TIME_BASE_DIV), each line's pad
 * mask (mmDC_GPIO_DDC1_MASK to mmDC_GPIO_DDC6_MASK), and each AUX
 * channel's control, request control, arbitration, interrupt control,
 * request status, data port and receiver control
 * (mmDP_AUX0_AUX_CONTROL, mmDP_AUX0_AUX_SW_CONTROL,
 * mmDP_AUX0_AUX_ARB_CONTROL, mmDP_AUX0_AUX_INTERRUPT_CONTROL,
 * mmDP_AUX0_AUX_SW_STATUS, mmDP_AUX0_AUX_SW_DATA,
 * mmDP_AUX0_AUX_DPHY_RX_CONTROL0 and the same of DP_AUX1 to DP_AUX5).
 * The registers the mode set adds, from the same header: the timing
 * generator's blank control, CRC control and the first CRC's words
 * (mmOTG0_OTG_BLANK_CONTROL, mmOTG0_OTG_CRC_CNTL, mmOTG0_OTG_CRC0_DATA_RG,
 * mmOTG0_OTG_CRC0_DATA_B).
 * The registers the mode set's own pipe programming adds, from the same
 * header: the plane's second viewport, request sizes, debug word,
 * expansion modes, arbitration levels and times, deadline words
 * (mmHUBP0_DCSURF_SEC_VIEWPORT_START, mmHUBP0_DCSURF_SEC_VIEWPORT_DIMENSION,
 * mmHUBP0_DCHUBP_REQ_SIZE_CONFIG, mmHUBP0_DCHUBP_REQ_SIZE_CONFIG_C,
 * mmHUBP0_HUBPREQ_DEBUG_DB, mmHUBPREQ0_DCN_EXPANSION_MODE,
 * mmHUBPREQ0_DCN_TTU_QOS_WM, mmHUBPREQ0_DCN_GLOBAL_TTU_CNTL,
 * mmHUBPREQ0_DCN_SURF0_TTU_CNTL0 and CNTL1, mmHUBPREQ0_DCN_SURF1_TTU_CNTL0
 * and CNTL1, mmHUBPREQ0_DCN_CUR0_TTU_CNTL0 and CNTL1,
 * mmHUBPREQ0_DCN_CUR1_TTU_CNTL1, mmHUBPREQ0_BLANK_OFFSET_0 and _1,
 * mmHUBPREQ0_DST_DIMENSIONS, mmHUBPREQ0_DST_AFTER_SCALER,
 * mmHUBPREQ0_PREFETCH_SETTINGS and _C, mmHUBPREQ0_VBLANK_PARAMETERS_0 to
 * _4, mmHUBPREQ0_FLIP_PARAMETERS_0 to _2, mmHUBPREQ0_NOM_PARAMETERS_0 to
 * _7, mmHUBPREQ0_PER_LINE_DELIVERY_PRE, mmHUBPREQ0_PER_LINE_DELIVERY,
 * mmHUBPREQ0_REF_FREQ_TO_PIX_FREQ, mmHUBPREQ0_DST_Y_DELTA_DRQ_LIMIT), the
 * hub arbiter's watermarks (the five words of each set, as above), its
 * watermark select, saturation level, outstanding requests and timer
 * (mmDCHUBBUB_ARB_WATERMARK_CHANGE_CNTL, mmDCHUBBUB_ARB_SAT_LEVEL,
 * mmDCHUBBUB_ARB_DF_REQ_OUTSTAND, mmDCHUBBUB_GLOBAL_TIMER_CNTL), the
 * timing generator's global sync, total limits, interlace and clock
 * control (mmOTG0_OTG_VSTARTUP_PARAM, mmOTG0_OTG_VUPDATE_PARAM,
 * mmOTG0_OTG_VREADY_PARAM, mmOTG0_OTG_V_TOTAL_MIN, mmOTG0_OTG_V_TOTAL_MAX,
 * mmOTG0_OTG_INTERLACE_CONTROL, mmOTG0_OTG_CLOCK_CONTROL), the vertical
 * timing generators (mmVTG0_CONTROL), and the scaler's copy of the blank
 * and its blend size (mmDSCL0_OTG_H_BLANK, mmDSCL0_OTG_V_BLANK,
 * mmDSCL0_MPC_SIZE and the same of DSCL1 to DSCL5), and the PHY PLL that
 * clocks each timing generator (mmOTG0_PHYPLL_PIXEL_RATE_CNTL), and the
 * first CRC's two windows (mmOTG0_OTG_CRC0_WINDOWA_X_CONTROL,
 * mmOTG0_OTG_CRC0_WINDOWA_Y_CONTROL, mmOTG0_OTG_CRC0_WINDOWB_X_CONTROL,
 * mmOTG0_OTG_CRC0_WINDOWB_Y_CONTROL), the hub arbiter's DRAM state
 * control (mmDCHUBBUB_ARB_DRAM_STATE_CNTL), and the hub's test debug
 * index and data (mmDCHUBBUB_TEST_DEBUG_INDEX, mmDCHUBBUB_TEST_DEBUG_DATA).
 * derived: the distances are the differences between the offsets of
 * instance 1 and instance 0 in the same header; a scaler register's
 * distance from its pixel processor's control word is the same in all six
 * pipes (0x27, 0x2c, 0x30, 0x3d, 0x3e and 0x38), and so is a cursor
 * register's (0x1b and 0x1e), and so are the scaler's blank and blend
 * size words (0x3b, 0x3c and 0x3f).
 */
static const struct dcn_place dcn_places[AMDRDNA_DCN_REGISTER_COUNT] = {
	{ 1U, 0x0000U, 0x0000U, head_legacy_offsets },
	{ 1U, 0x0064U, 0x0000U, NULL },
	{ 1U, 0x0080U, 0x0004U, NULL },
	{ 1U, 0x0081U, 0x0004U, NULL },
	{ 1U, 0x0082U, 0x0004U, NULL },
	{ 2U, 0x05e5U, 0x00dcU, NULL },
	{ 2U, 0x05e7U, 0x00dcU, NULL },
	{ 2U, 0x05e9U, 0x00dcU, NULL },
	{ 2U, 0x05eaU, 0x00dcU, NULL },
	{ 2U, 0x05f3U, 0x00dcU, NULL },
	{ 2U, 0x05f4U, 0x00dcU, NULL },
	{ 2U, 0x0607U, 0x00dcU, NULL },
	{ 2U, 0x060aU, 0x00dcU, NULL },
	{ 2U, 0x060bU, 0x00dcU, NULL },
	{ 2U, 0x061aU, 0x00dcU, NULL },
	{ 2U, 0x061bU, 0x00dcU, NULL },
	{ 2U, 0x066cU, 0x00dcU, NULL },
	{ 2U, 0x0000U, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x1271U, 0x001bU, NULL },
	{ 2U, 0x1272U, 0x001bU, NULL },
	{ 2U, 0x1273U, 0x001bU, NULL },
	{ 2U, 0x1274U, 0x001bU, NULL },
	{ 2U, 0x1385U, 0x0004U, NULL },
	{ 2U, 0x1acbU, 0x0010U, NULL },
	{ 2U, 0x1acaU, 0x0010U, NULL },
	{ 2U, 0x1b2aU, 0x0080U, NULL },
	{ 2U, 0x1b2bU, 0x0080U, NULL },
	{ 2U, 0x1b2cU, 0x0080U, NULL },
	{ 2U, 0x1b2dU, 0x0080U, NULL },
	{ 2U, 0x1b2fU, 0x0080U, NULL },
	{ 2U, 0x1b36U, 0x0080U, NULL },
	{ 2U, 0x1b37U, 0x0080U, NULL },
	{ 2U, 0x1b38U, 0x0080U, NULL },
	{ 2U, 0x1b41U, 0x0080U, NULL },
	{ 2U, 0x1b8bU, 0x0080U, NULL },
	{ 2U, 0x2068U, 0x0100U, NULL },
	{ 2U, 0x20afU, 0x0100U, NULL },
	{ 2U, 0x20b0U, 0x0100U, NULL },
	{ 2U, 0x210cU, 0x0100U, NULL },
	{ 2U, 0x2108U, 0x0100U, NULL },
	{ 2U, 0x0027U, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x002cU, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x0030U, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x003dU, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x003eU, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x0038U, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x0609U, 0x00dcU, NULL },
	{ 2U, 0x0620U, 0x00dcU, NULL },
	{ 2U, 0x0625U, 0x00dcU, NULL },
	{ 2U, 0x0626U, 0x00dcU, NULL },
	{ 2U, 0x065eU, 0x00dcU, NULL },
	{ 2U, 0x0678U, 0x00dcU, NULL },
	{ 2U, 0x0679U, 0x00dcU, NULL },
	{ 2U, 0x067aU, 0x00dcU, NULL },
	{ 2U, 0x067bU, 0x00dcU, NULL },
	{ 2U, 0x067cU, 0x00dcU, NULL },
	{ 2U, 0x067dU, 0x00dcU, NULL },
	{ 2U, 0x067fU, 0x00dcU, NULL },
	{ 2U, 0x001bU, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x001eU, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x1b4aU, 0x0080U, NULL },
	{ 2U, 0x1b4cU, 0x0080U, NULL },
	{ 2U, 0x1b8aU, 0x0080U, NULL },
	{ 2U, 0x1f14U, 0x0008U, NULL },
	{ 2U, 0x1f15U, 0x0008U, NULL },
	{ 2U, 0x1e98U, 0x0000U, NULL },
	{ 2U, 0x1e99U, 0x0000U, NULL },
	{ 2U, 0x1e9bU, 0x0000U, NULL },
	{ 2U, 0x1e9cU, 0x0001U, NULL },
	{ 2U, 0x1ea2U, 0x0002U, NULL },
	{ 2U, 0x1ea3U, 0x0002U, NULL },
	{ 2U, 0x0000U, 0x0000U, i2c_transaction_offsets },
	{ 2U, 0x1eb2U, 0x0000U, NULL },
	{ 1U, 0x007bU, 0x0000U, NULL },
	{ 2U, 0x28d0U, 0x0004U, NULL },
	{ 2U, 0x1f50U, 0x001cU, NULL },
	{ 2U, 0x1f51U, 0x001cU, NULL },
	{ 2U, 0x1f52U, 0x001cU, NULL },
	{ 2U, 0x1f53U, 0x001cU, NULL },
	{ 2U, 0x1f54U, 0x001cU, NULL },
	{ 2U, 0x1f56U, 0x001cU, NULL },
	{ 2U, 0x1f5aU, 0x001cU, NULL },
	{ 2U, 0x1b42U, 0x0080U, NULL },
	{ 2U, 0x1b68U, 0x0080U, NULL },
	{ 2U, 0x1b6eU, 0x0080U, NULL },
	{ 2U, 0x1b6fU, 0x0080U, NULL },
	{ 2U, 0x05edU, 0x00dcU, NULL },
	{ 2U, 0x05eeU, 0x00dcU, NULL },
	{ 2U, 0x05f1U, 0x00dcU, NULL },
	{ 2U, 0x05f2U, 0x00dcU, NULL },
	{ 2U, 0x05f6U, 0x00dcU, NULL },
	{ 2U, 0x062cU, 0x00dcU, NULL },
	{ 2U, 0x062dU, 0x00dcU, NULL },
	{ 2U, 0x062eU, 0x00dcU, NULL },
	{ 2U, 0x062fU, 0x00dcU, NULL },
	{ 2U, 0x0630U, 0x00dcU, NULL },
	{ 2U, 0x0631U, 0x00dcU, NULL },
	{ 2U, 0x0632U, 0x00dcU, NULL },
	{ 2U, 0x0633U, 0x00dcU, NULL },
	{ 2U, 0x0634U, 0x00dcU, NULL },
	{ 2U, 0x0636U, 0x00dcU, NULL },
	{ 2U, 0x0646U, 0x00dcU, NULL },
	{ 2U, 0x0647U, 0x00dcU, NULL },
	{ 2U, 0x0648U, 0x00dcU, NULL },
	{ 2U, 0x0649U, 0x00dcU, NULL },
	{ 2U, 0x064aU, 0x00dcU, NULL },
	{ 2U, 0x064bU, 0x00dcU, NULL },
	{ 2U, 0x064cU, 0x00dcU, NULL },
	{ 2U, 0x064dU, 0x00dcU, NULL },
	{ 2U, 0x064eU, 0x00dcU, NULL },
	{ 2U, 0x064fU, 0x00dcU, NULL },
	{ 2U, 0x0650U, 0x00dcU, NULL },
	{ 2U, 0x0651U, 0x00dcU, NULL },
	{ 2U, 0x0652U, 0x00dcU, NULL },
	{ 2U, 0x0653U, 0x00dcU, NULL },
	{ 2U, 0x0654U, 0x00dcU, NULL },
	{ 2U, 0x0655U, 0x00dcU, NULL },
	{ 2U, 0x0656U, 0x00dcU, NULL },
	{ 2U, 0x0657U, 0x00dcU, NULL },
	{ 2U, 0x0658U, 0x00dcU, NULL },
	{ 2U, 0x0659U, 0x00dcU, NULL },
	{ 2U, 0x065aU, 0x00dcU, NULL },
	{ 2U, 0x065bU, 0x00dcU, NULL },
	{ 2U, 0x065cU, 0x00dcU, NULL },
	{ 2U, 0x065dU, 0x00dcU, NULL },
	{ 2U, 0x065fU, 0x00dcU, NULL },
	{ 2U, 0x0660U, 0x00dcU, NULL },
	{ 2U, 0x0000U, 0x0000U, mark_set_offsets },
	{ 2U, 0x0001U, 0x0000U, mark_set_offsets },
	{ 2U, 0x0002U, 0x0000U, mark_set_offsets },
	{ 2U, 0x0003U, 0x0000U, mark_set_offsets },
	{ 2U, 0x0004U, 0x0000U, mark_set_offsets },
	{ 2U, 0x051dU, 0x0000U, NULL },
	{ 2U, 0x0506U, 0x0000U, NULL },
	{ 2U, 0x0505U, 0x0000U, NULL },
	{ 2U, 0x051fU, 0x0000U, NULL },
	{ 2U, 0x1b87U, 0x0080U, NULL },
	{ 2U, 0x1b88U, 0x0080U, NULL },
	{ 2U, 0x1b89U, 0x0080U, NULL },
	{ 2U, 0x1b30U, 0x0080U, NULL },
	{ 2U, 0x1b31U, 0x0080U, NULL },
	{ 2U, 0x1b44U, 0x0080U, NULL },
	{ 2U, 0x1b86U, 0x0080U, NULL },
	{ 2U, 0x0528U, 0x0001U, NULL },
	{ 2U, 0x003bU, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x003cU, 0x0000U, pixel_processor_offsets },
	{ 2U, 0x003fU, 0x0000U, pixel_processor_offsets },
	{ 1U, 0x0083U, 0x0004U, NULL },
	{ 2U, 0x1b6aU, 0x0080U, NULL },
	{ 2U, 0x1b6bU, 0x0080U, NULL },
	{ 2U, 0x1b6cU, 0x0080U, NULL },
	{ 2U, 0x1b6dU, 0x0080U, NULL },
	{ 2U, 0x0508U, 0x0000U, NULL },
	{ 2U, 0x053dU, 0x0000U, NULL },
	{ 2U, 0x053eU, 0x0000U, NULL }
};

/*
 * Reports the absolute dword index of one instance of a display register.
 *
 * Returns 0 for a register the table does not name, an instance past the
 * sixth pipe, or a display block without the register's window; a real
 * display register never lies at index 0.
 */
uint32_t
amdrdna_dcn_index(
	const struct amdrdna_ip *display,
	enum amdrdna_dcn_register which,
	unsigned instance)
{
	const struct dcn_place *place;
	uint32_t offset;

	/* A register or an instance the table does not hold has no address. */
	if ((unsigned)which >= AMDRDNA_DCN_REGISTER_COUNT)
		return 0U;
	if (instance >= AMDRDNA_DISPLAY_PIPES)
		return 0U;

	/* The display block must list the register's window. */
	place = &dcn_places[which];
	if (display->found == 0U)
		return 0U;
	if (place->window >= display->window_count)
		return 0U;

	/* An uneven family adds the register's distance to the instance's own base; the others repeat at their distance. */
	if (place->instances != NULL) {
		offset = place->instances[instance] + place->offset;
	} else {
		offset = place->offset + (uint32_t)place->stride * instance;
	}

	/* Succeeded: the window's start plus the offset. */
	return display->window[place->window] + offset;
}
