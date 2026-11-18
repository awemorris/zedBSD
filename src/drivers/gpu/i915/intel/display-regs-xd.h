/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The north display registers of display 12 and 13 (Tiger Lake, Rocket
 * Lake, Alder Lake-S and Raptor Lake-S on display 12; Alder Lake-P/N and
 * Raptor Lake-P/U on display 13), as the xd display code (display/xd-*.c)
 * reads and writes them.
 *
 * Each register name has the I915_XD_ prefix.  A register that repeats per pipe, transcoder, port,
 * PHY or Type-C port takes the instance as an argument; the argument is
 * a plain index:
 *
 *   pipe, transcoder   0..3 for A..D (on display 12 and 13 every
 *                      transcoder belongs to the pipe of the same index)
 *   port (DDI)         0..8: 0..2 for A..C, 3..8 for the DDIs that follow
 *                      (the Type-C ports TC1..TC6 on Tiger Lake and
 *                      display 13, combo ports on Rocket Lake and
 *                      Alder Lake-S)
 *   phy                0..4 for combo PHY A..E
 *   tc                 0..5 for Type-C port TC1..TC6
 *   fia                0..2 for FIA 1..3
 *   aux                the AUX channel, numbered like the DDIs
 *
 * A field is given by its mask; the code places a value into a field
 * with the mask's lowest set bit as the shift.  The south display (PCH)
 * registers are in display-regs-xd-south.h, and the tables of values
 * (PLL words, clock steps, buffer translations, the platforms) in
 * display-tables-xd.h.
 */

#ifndef DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD_H
#define DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD_H

/*
 * The distance between the registers of two neighbouring instances: a
 * pipe's and a transcoder's blocks repeat every 4 KiB, a DDI's every
 * 256 bytes.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_display_device.c (PIPE_A_OFFSET to
 * PIPE_D_OFFSET, TRANSCODER_A_OFFSET to TRANSCODER_D_OFFSET), and
 * drivers/gpu/drm/i915/i915_reg.h (_DDI_BUF_CTL_A, _DDI_BUF_CTL_B).
 */
#define I915_XD_PIPE_STRIDE			0x1000U
#define I915_XD_TRANS_STRIDE			0x1000U
#define I915_XD_DDI_STRIDE			0x100U

/*
 * The GT driver mailbox: the request word (bit 31 set by the driver and
 * cleared by the firmware when it has answered; the low byte the
 * command, then the status), and the two data words.  Status 0 is
 * success; the other values name the firmware's refusal.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN6_PCODE_MAILBOX, GEN6_PCODE_READY, GEN6_PCODE_ERROR_MASK,
 * GEN6_PCODE_SUCCESS, GEN6_PCODE_ILLEGAL_CMD, GEN7_PCODE_TIMEOUT,
 * GEN7_PCODE_ILLEGAL_DATA, GEN11_PCODE_ILLEGAL_SUBCOMMAND,
 * GEN11_PCODE_LOCKED, GEN11_PCODE_REJECTED,
 * GEN7_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE, GEN6_PCODE_DATA,
 * GEN6_PCODE_DATA1).
 */
#define I915_XD_GTDRIVER_MAILBOX		0x138124U
#define I915_XD_GTDRIVER_MAILBOX_BUSY		0x80000000U
#define I915_XD_GTDRIVER_MAILBOX_STATUS		0x000000ffU
#define I915_XD_GTDRIVER_STATUS_OK		0x00U
#define I915_XD_GTDRIVER_STATUS_BAD_COMMAND	0x01U
#define I915_XD_GTDRIVER_STATUS_TIMEOUT		0x02U
#define I915_XD_GTDRIVER_STATUS_BAD_DATA	0x03U
#define I915_XD_GTDRIVER_STATUS_BAD_SUBCOMMAND	0x04U
#define I915_XD_GTDRIVER_STATUS_LOCKED		0x06U
#define I915_XD_GTDRIVER_STATUS_OUT_OF_RANGE	0x10U
#define I915_XD_GTDRIVER_STATUS_REJECTED	0x11U
#define I915_XD_GTDRIVER_DATA0			0x138128U
#define I915_XD_GTDRIVER_DATA1			0x13812cU

/*
 * The mailbox commands the display uses: the memory latencies (data 0
 * picks levels 0..3 or 4..7, one byte a level), the CD clock change
 * handshake, the memory subsystem and QGV point information, the SAGV
 * and QGV point restriction, the Ice Lake TC cold exit, the Tiger Lake
 * TC cold block and unblock, and the SAGV block time.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN9_PCODE_READ_MEM_LATENCY, GEN9_MEM_LATENCY_LEVEL_0_4_MASK to
 * GEN9_MEM_LATENCY_LEVEL_3_7_MASK, SKL_PCODE_CDCLK_CONTROL,
 * SKL_CDCLK_PREPARE_FOR_CHANGE, SKL_CDCLK_READY_FOR_CHANGE,
 * ICL_PCODE_MEM_SUBSYSYSTEM_INFO, ICL_PCODE_MEM_SS_READ_GLOBAL_INFO,
 * ICL_PCODE_MEM_SS_READ_QGV_POINT_INFO, ADL_PCODE_MEM_SS_READ_PSF_GV_INFO,
 * ICL_PCODE_SAGV_DE_MEM_SS_CONFIG,
 * ICL_PCODE_REP_QGV_MASK, ICL_PCODE_REQ_QGV_PT_MASK,
 * ADLS_PCODE_REP_PSF_MASK, ADLS_PCODE_REQ_PSF_PT_MASK,
 * ICL_PCODE_EXIT_TCCOLD, TGL_PCODE_TCCOLD,
 * TGL_PCODE_EXIT_TCCOLD_DATA_L_EXIT_FAILED,
 * TGL_PCODE_EXIT_TCCOLD_DATA_L_UNBLOCK_REQ,
 * GEN12_PCODE_READ_SAGV_BLOCK_TIME_US).
 */
#define I915_XD_MAILBOX_READ_LATENCY		0x06U
#define I915_XD_MAILBOX_LATENCY_LEVEL_BITS	8U
#define I915_XD_MAILBOX_LATENCY_LEVEL_MASK	0xffU
#define I915_XD_MAILBOX_CDCLK_CONTROL		0x07U
#define I915_XD_MAILBOX_CDCLK_PREPARE		0x03U
#define I915_XD_MAILBOX_CDCLK_READY		0x01U
#define I915_XD_MAILBOX_MEMORY_INFO		0x0dU
#define I915_XD_MAILBOX_MEMORY_INFO_GLOBAL	0x00000000U
#define I915_XD_MAILBOX_MEMORY_INFO_QGV(point)	(((point) << 16) | 0x100U)
#define I915_XD_MAILBOX_MEMORY_INFO_PSF		0x00000200U
#define I915_XD_MAILBOX_SAGV_CONFIG		0x0eU
#define I915_XD_MAILBOX_SAGV_REPLY_QGV		0x00000003U
#define I915_XD_MAILBOX_SAGV_REQUEST_QGV	0x000000ffU
#define I915_XD_MAILBOX_SAGV_REPLY_PSF		0x0000000cU
#define I915_XD_MAILBOX_SAGV_REQUEST_PSF	0x00000700U
#define I915_XD_MAILBOX_EXIT_TCCOLD		0x12U
#define I915_XD_MAILBOX_TCCOLD			0x26U
#define I915_XD_MAILBOX_TCCOLD_BLOCK		0x0U
#define I915_XD_MAILBOX_TCCOLD_UNBLOCK		0x1U
#define I915_XD_MAILBOX_TCCOLD_EXIT_FAILED	0x1U
#define I915_XD_MAILBOX_SAGV_BLOCK_TIME		0x23U

/*
 * The power well controls.  Each well has a request bit and a state bit
 * at twice its index; the driver's copy of each control is the second of
 * the four (BIOS, driver, KVMR, debug).  The main wells, the AUX wells
 * and the DDI IO wells have controls of their own.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (HSW_PWR_WELL_CTL1, HSW_PWR_WELL_CTL2, HSW_PWR_WELL_CTL_REQ,
 * HSW_PWR_WELL_CTL_STATE, ICL_PWR_WELL_CTL_AUX1, ICL_PWR_WELL_CTL_AUX2,
 * ICL_PWR_WELL_CTL_DDI1, ICL_PWR_WELL_CTL_DDI2).
 */
#define I915_XD_PWR_WELL_CTL_BIOS		0x45400U
#define I915_XD_PWR_WELL_CTL			0x45404U
#define I915_XD_PWR_WELL_CTL_AUX_BIOS		0x45440U
#define I915_XD_PWR_WELL_CTL_AUX		0x45444U
#define I915_XD_PWR_WELL_CTL_DDI_BIOS		0x45450U
#define I915_XD_PWR_WELL_CTL_DDI		0x45454U
#define I915_XD_PWR_WELL_REQUEST(index)		(0x2U << ((index) * 2U))
#define I915_XD_PWR_WELL_STATE(index)		(0x1U << ((index) * 2U))

/*
 * The indexes of the main wells in the main control: power wells 1 to 5
 * (5 only on display 12), and the pipe wells A to D of display 13.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ICL_PW_CTL_IDX_PW_1 to ICL_PW_CTL_IDX_PW_4, TGL_PW_CTL_IDX_PW_5,
 * XELPD_PW_CTL_IDX_PW_A to XELPD_PW_CTL_IDX_PW_D).
 */
#define I915_XD_WELL_PW_1			0U
#define I915_XD_WELL_PW_2			1U
#define I915_XD_WELL_PW_3			2U
#define I915_XD_WELL_PW_4			3U
#define I915_XD_WELL_PW_5			4U
#define I915_XD_WELL_PW_A			5U
#define I915_XD_WELL_PW_B			6U
#define I915_XD_WELL_PW_C			7U
#define I915_XD_WELL_PW_D			8U

/*
 * The indexes of the AUX wells: one per AUX channel (A, B, C, then the
 * Type-C channels, or D and E on display 13), and the Thunderbolt AUX
 * wells of the Type-C ports after them.  Tiger Lake and display 13 place
 * the Thunderbolt wells at 9..14; display 13 calls 7 and 8 AUX D and E.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ICL_PW_CTL_IDX_AUX_A to ICL_PW_CTL_IDX_AUX_C, TGL_PW_CTL_IDX_AUX_TC1
 * to TGL_PW_CTL_IDX_AUX_TC6, TGL_PW_CTL_IDX_AUX_TBT1 to
 * TGL_PW_CTL_IDX_AUX_TBT6, XELPD_PW_CTL_IDX_AUX_D, XELPD_PW_CTL_IDX_AUX_E).
 */
#define I915_XD_WELL_AUX(aux)			(aux)
#define I915_XD_WELL_AUX_TBT(tc)		(9U + (tc))

/*
 * The indexes of Ice Lake's Thunderbolt AUX wells, right after its six
 * AUX wells A..F.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ICL_PW_CTL_IDX_AUX_TBT1 to ICL_PW_CTL_IDX_AUX_TBT4).
 */
#define I915_XD_WELL_AUX_TBT_ICL(tc)		(8U + (tc))

/*
 * The indexes of the DDI IO wells: one per DDI, numbered like the DDIs
 * (display 13 calls 7 and 8 DDI D and E).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ICL_PW_CTL_IDX_DDI_A to ICL_PW_CTL_IDX_DDI_C, TGL_PW_CTL_IDX_DDI_TC1
 * to TGL_PW_CTL_IDX_DDI_TC6, XELPD_PW_CTL_IDX_DDI_D,
 * XELPD_PW_CTL_IDX_DDI_E).
 */
#define I915_XD_WELL_DDI(port)			(port)

/*
 * The fuse status: the download of the fuses is done, and power gate n
 * (0 for the always-on part, 1..4 for power wells 1..4) has its fuses
 * distributed.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (SKL_FUSE_STATUS, SKL_FUSE_DOWNLOAD_STATUS, SKL_FUSE_PG_DIST_STATUS).
 */
#define I915_XD_FUSE_STATUS			0x42000U
#define I915_XD_FUSE_STATUS_DOWNLOADED		0x80000000U
#define I915_XD_FUSE_STATUS_PG(gate)		(1U << (27U - (gate)))

/*
 * The DC state the DMC may take the display into (DC5, DC6, or DC9 for
 * a suspend), and the debug word whose two masks the DMC loading sets.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (DC_STATE_EN, DC_STATE_DISABLE, DC_STATE_EN_UPTO_DC5,
 * DC_STATE_EN_UPTO_DC6, DC_STATE_EN_UPTO_DC5_DC6_MASK, DC_STATE_EN_DC9,
 * DC_STATE_EN_DC3CO, DC_STATE_DEBUG, DC_STATE_DEBUG_MASK_CORES,
 * DC_STATE_DEBUG_MASK_MEMORY_UP).
 */
#define I915_XD_DC_STATE_EN			0x45504U
#define I915_XD_DC_STATE_OFF			0x00000000U
#define I915_XD_DC_STATE_DC5			0x00000001U
#define I915_XD_DC_STATE_DC6			0x00000002U
#define I915_XD_DC_STATE_DC5_DC6_MASK		0x00000003U
#define I915_XD_DC_STATE_DC9			0x00000008U
#define I915_XD_DC_STATE_DC3CO			0x40000000U
#define I915_XD_DC_STATE_DEBUG			0x45520U
#define I915_XD_DC_STATE_DEBUG_MASK_CORES	0x00000001U
#define I915_XD_DC_STATE_DEBUG_MASK_MEMORY_UP	0x00000002U

/*
 * The utility pin: its enable and its mode field, whose PWM mode keeps
 * the display out of DC6.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight_regs.h (UTIL_PIN_CTL,
 * UTIL_PIN_ENABLE, UTIL_PIN_MODE_MASK, UTIL_PIN_MODE_PWM).
 */
#define I915_XD_UTIL_PIN_CTL			0x48400U
#define I915_XD_UTIL_PIN_ENABLE			0x80000000U
#define I915_XD_UTIL_PIN_MODE_MASK		0x0f000000U
#define I915_XD_UTIL_PIN_MODE_PWM		0x01000000U

/*
 * The display buffer slices: request and state of a slice's power, and
 * the tracker state service fields (the minimum one only on display 13).
 * Slice 0..3 is S1..S4.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_watermark_regs.h (DBUF_CTL_S,
 * DBUF_POWER_REQUEST, DBUF_POWER_STATE, DBUF_TRACKER_STATE_SERVICE_MASK,
 * DBUF_MIN_TRACKER_STATE_SERVICE_MASK).
 */
#define I915_XD_DBUF_CTL_S1			0x45008U
#define I915_XD_DBUF_CTL_S2			0x44fe8U
#define I915_XD_DBUF_CTL_S3			0x44300U
#define I915_XD_DBUF_CTL_S4			0x44304U
#define I915_XD_DBUF_POWER_REQUEST		0x80000000U
#define I915_XD_DBUF_POWER_STATE		0x40000000U
#define I915_XD_DBUF_TRACKER_STATE_SERVICE	0x00f80000U
#define I915_XD_DBUF_MIN_TRACKER_STATE_SERVICE	0x00070000U

/*
 * The memory bus: the join of the slices to one pipe and the hashing
 * mode (display 13), the credit of the arbitration boxes 0..2, and a
 * pipe's credits towards the data box.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_watermark_regs.h (MBUS_CTL, MBUS_JOIN,
 * MBUS_HASHING_MODE_MASK, MBUS_JOIN_PIPE_SELECT_MASK,
 * MBUS_JOIN_PIPE_SELECT_NONE, PIPE_MBUS_DBOX_CTL,
 * MBUS_DBOX_B2B_TRANSACTIONS_MAX_MASK,
 * MBUS_DBOX_B2B_TRANSACTIONS_DELAY_MASK,
 * MBUS_DBOX_REGULATE_B2B_TRANSACTIONS_EN, MBUS_DBOX_BW_CREDIT_MASK,
 * MBUS_DBOX_B_CREDIT_MASK, MBUS_DBOX_I_CREDIT_MASK,
 * MBUS_DBOX_A_CREDIT_MASK), and drivers/gpu/drm/i915/i915_reg.h
 * (MBUS_ABOX_CTL, _MBUS_ABOX0_CTL to _MBUS_ABOX2_CTL,
 * MBUS_ABOX_BW_CREDIT_MASK, MBUS_ABOX_B_CREDIT_MASK,
 * MBUS_ABOX_BT_CREDIT_POOL2_MASK, MBUS_ABOX_BT_CREDIT_POOL1_MASK).
 */
#define I915_XD_MBUS_CTL			0x4438cU
#define I915_XD_MBUS_CTL_JOIN			0x80000000U
#define I915_XD_MBUS_CTL_HASH_1X4		0x40000000U
#define I915_XD_MBUS_CTL_JOIN_PIPE		0x1c000000U
#define I915_XD_MBUS_CTL_JOIN_PIPE_NONE		7U
#define I915_XD_MBUS_ABOX0_CTL			0x45038U
#define I915_XD_MBUS_ABOX1_CTL			0x45048U
#define I915_XD_MBUS_ABOX2_CTL			0x4504cU
#define I915_XD_MBUS_ABOX_BW_CREDIT		0x00300000U
#define I915_XD_MBUS_ABOX_B_CREDIT		0x000f0000U
#define I915_XD_MBUS_ABOX_BT_CREDIT_POOL2	0x00001f00U
#define I915_XD_MBUS_ABOX_BT_CREDIT_POOL1	0x0000001fU
#define I915_XD_PIPE_MBUS_DBOX_CTL(pipe)	(0x7003cU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_DBOX_B2B_MAX			0x01f00000U
#define I915_XD_DBOX_B2B_DELAY			0x000e0000U
#define I915_XD_DBOX_B2B_REGULATE		0x00010000U
#define I915_XD_DBOX_BW_CREDIT			0x0000c000U
#define I915_XD_DBOX_B_CREDIT			0x00001f00U
#define I915_XD_DBOX_I_CREDIT			0x000000e0U
#define I915_XD_DBOX_A_CREDIT			0x0000000fU

/*
 * The bandwidth buddy: two instances, each with a disable bit, the TLB
 * request timer and the page mask chosen by the memory type.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (BW_BUDDY_CTL, BW_BUDDY_DISABLE, BW_BUDDY_TLB_REQ_TIMER_MASK,
 * BW_BUDDY_PAGE_MASK).
 */
#define I915_XD_BW_BUDDY_CTL(index)		(0x45130U + 0x10U * (index))
#define I915_XD_BW_BUDDY_DISABLE		0x80000000U
#define I915_XD_BW_BUDDY_TLB_REQ_TIMER		0x003f0000U
#define I915_XD_BW_BUDDY_PAGE_MASK(index)	(0x45134U + 0x10U * (index))

/*
 * The display arbiter control: the IPC enable and the data partition.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (DISP_ARB_CTL2, DISP_IPC_ENABLE, DISP_DATA_PARTITION_5_6).
 */
#define I915_XD_DISP_ARB_CTL2			0x45004U
#define I915_XD_DISP_ARB_CTL2_IPC		0x00000008U
#define I915_XD_DISP_ARB_CTL2_PARTITION_5_6	0x00000040U

/*
 * The chicken bits and clock gates the display start writes: the PCH
 * reset handshake, the DC power request chickens (the FLR source, the
 * DDI clock register access, the response delay of Ice Lake, and the
 * four bits of the second word), the error mask of display 13, the DPCE
 * clock gate, and a pipe's pipe-DMC clock gate.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (HSW_NDE_RSTWRN_OPT, RESET_PCH_HANDSHAKE_ENABLE, GEN8_CHICKEN_DCPR_1,
 * DISABLE_FLR_SRC, DDI_CLOCK_REG_ACCESS, ICL_DELAY_PMRSP,
 * GEN11_CHICKEN_DCPR_2, DCPR_MASK_MAXLATENCY_MEMUP_CLR, DCPR_MASK_LPMODE,
 * DCPR_SEND_RESP_IMM, DCPR_CLEAR_MEMSTAT_DIS,
 * XELPD_DISPLAY_ERR_FATAL_MASK, GEN9_CLKGATE_DIS_5, DPCE_GATING_DIS,
 * CLKGATE_DIS_PSL_EXT, PIPEDMC_GATING_DIS).
 */
#define I915_XD_NDE_RSTWRN_OPT			0x46408U
#define I915_XD_NDE_RSTWRN_PCH_HANDSHAKE	0x00000010U
#define I915_XD_CHICKEN_DCPR_1			0x46430U
#define I915_XD_DCPR_1_DELAY_PMRSP		0x00400000U
#define I915_XD_DCPR_1_DISABLE_FLR_SRC		0x00008000U
#define I915_XD_DCPR_1_DDI_CLOCK_REG_ACCESS	0x00000080U
#define I915_XD_CHICKEN_DCPR_2			0x46434U
#define I915_XD_DCPR_2_MASK_MAXLATENCY_MEMUP	0x08000000U
#define I915_XD_DCPR_2_MASK_LPMODE		0x04000000U
#define I915_XD_DCPR_2_SEND_RESP_IMM		0x02000000U
#define I915_XD_DCPR_2_CLEAR_MEMSTAT_DIS	0x01000000U
#define I915_XD_DISPLAY_ERR_FATAL_MASK		0x4421cU
#define I915_XD_CLKGATE_DIS_5			0x46540U
#define I915_XD_CLKGATE_DIS_5_DPCE		0x00020000U
#define I915_XD_CLKGATE_DIS_PSL_EXT(pipe)	(0x4654cU + 4U * (pipe))
#define I915_XD_CLKGATE_PSL_EXT_PIPEDMC		0x00001000U

/*
 * The start's workarounds of display 11 and 12: the frame buffer
 * compressor's dummy pixel chicken (compressor A) and the memory-up
 * override of the clock request policy.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ILK_DPFC_CHICKEN, DPFC_CHICKEN_COMP_DUMMY_PIXEL, CLKREQ_POLICY,
 * CLKREQ_POLICY_MEM_UP_OVRD).
 */
#define I915_XD_DPFC_CHICKEN_A			0x43224U
#define I915_XD_DPFC_CHICKEN_DUMMY_PIXEL	0x00004000U
#define I915_XD_CLKREQ_POLICY			0x101038U
#define I915_XD_CLKREQ_POLICY_MEM_UP		0x00000002U

/*
 * The firmware's hold on ports (Rocket Lake and Alder Lake-S): which
 * DPLLs and which PHYs the firmware keeps for itself.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_hti_regs.h (HDPORT_STATE,
 * HDPORT_DPLL_USED_MASK, HDPORT_DDI_USED, HDPORT_ENABLED).
 */
#define I915_XD_HDPORT_STATE			0x45050U
#define I915_XD_HDPORT_DPLL_USED		0x0000f000U
#define I915_XD_HDPORT_DDI_USED(phy)		(1U << (2U * (phy) + 1U))
#define I915_XD_HDPORT_ENABLED			0x00000001U

/*
 * The VGA plane control: setting the top bit turns the legacy VGA plane
 * off.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (CPU_VGACNTRL, VGA_DISP_DISABLE).
 */
#define I915_XD_VGA_CONTROL			0x41000U
#define I915_XD_VGA_CONTROL_DISABLE		0x80000000U

/*
 * The display interrupt control and its four kinds of source: per pipe,
 * the ports (AUX done, GMBUS), the miscellaneous, and the hotplug of the
 * Type-C and Thunderbolt pins.  Each kind has an ISR, IMR, IIR and IER
 * word, 4 bytes apart.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_DISPLAY_INT_CTL, GEN11_DISPLAY_IRQ_ENABLE, GEN11_DE_PCH_IRQ,
 * GEN11_DE_MISC_IRQ, GEN11_DE_HPD_IRQ, GEN11_DE_PORT_IRQ,
 * GEN11_DE_PIPE_A, GEN8_DE_PIPE_ISR, GEN8_DE_PORT_ISR, GEN8_DE_MISC_ISR,
 * GEN11_DE_HPD_ISR).
 */
#define I915_XD_DISPLAY_INT_CTL			0x44200U
#define I915_XD_DISPLAY_INT_ENABLE		0x80000000U
#define I915_XD_DISPLAY_INT_PCH			0x00800000U
#define I915_XD_DISPLAY_INT_MISC		0x00400000U
#define I915_XD_DISPLAY_INT_HPD			0x00200000U
#define I915_XD_DISPLAY_INT_PORT		0x00100000U
#define I915_XD_DISPLAY_INT_PIPE(pipe)		(0x00010000U << (pipe))
#define I915_XD_DE_PIPE_ISR(pipe)		(0x44400U + 0x10U * (pipe))
#define I915_XD_DE_PORT_ISR			0x44440U
#define I915_XD_DE_MISC_ISR			0x44460U
#define I915_XD_DE_HPD_ISR			0x44470U
#define I915_XD_DE_IMR_OFFSET			0x4U
#define I915_XD_DE_IIR_OFFSET			0x8U
#define I915_XD_DE_IER_OFFSET			0xcU

/*
 * The pipe interrupt bits the display uses: the FIFO underrun, the
 * first plane's flip done and fault, and the vblank.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN8_PIPE_FIFO_UNDERRUN, GEN9_PIPE_PLANE1_FLIP_DONE,
 * GEN9_PIPE_PLANE1_FAULT, GEN8_PIPE_VBLANK).
 */
#define I915_XD_PIPE_INT_UNDERRUN		0x80000000U
#define I915_XD_PIPE_INT_PLANE1_FAULT		0x00000080U
#define I915_XD_PIPE_INT_PLANE1_FLIP_DONE	0x00000008U
#define I915_XD_PIPE_INT_VBLANK			0x00000001U

/*
 * The port interrupt bits of the AUX channels: A, B and C, then the
 * Type-C channels USBC1..USBC6 (AUX D and E on display 13 at the place
 * of USBC5 and USBC6).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TGL_DE_PORT_AUX_DDIA to TGL_DE_PORT_AUX_DDIC, TGL_DE_PORT_AUX_USBC1
 * to TGL_DE_PORT_AUX_USBC6).
 */
#define I915_XD_PORT_INT_AUX_DDI(aux)		(1U << (aux))
#define I915_XD_PORT_INT_AUX_TC(tc)		(1U << (8U + (tc)))

/*
 * The hotplug of the Type-C pins: in the HPD interrupt word, bit 16 + tc
 * for a Type-C connection and bit tc for a Thunderbolt one; in the
 * Type-C and Thunderbolt hotplug controls, a nibble a pin with the
 * enable and the long and short detect bits.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_TC_HOTPLUG, GEN11_TBT_HOTPLUG, GEN11_TBT_HOTPLUG_CTL,
 * GEN11_TC_HOTPLUG_CTL, GEN11_HOTPLUG_CTL_ENABLE,
 * GEN11_HOTPLUG_CTL_LONG_DETECT, GEN11_HOTPLUG_CTL_SHORT_DETECT).
 */
#define I915_XD_HPD_INT_TC(tc)			(1U << (16U + (tc)))
#define I915_XD_HPD_INT_TBT(tc)			(1U << (tc))
#define I915_XD_TBT_HOTPLUG_CTL			0x44030U
#define I915_XD_TC_HOTPLUG_CTL			0x44038U
#define I915_XD_HOTPLUG_CTL_ENABLE(tc)		(0x8U << ((tc) * 4U))
#define I915_XD_HOTPLUG_CTL_LONG(tc)		(0x2U << ((tc) * 4U))
#define I915_XD_HOTPLUG_CTL_SHORT(tc)		(0x1U << ((tc) * 4U))

/*
 * The CD clock: the control word (the CD2X divider, the pipe the change
 * waits for, the decimal frequency in the low bits), the reference
 * clock in DSSM, and the CD clock PLL's enable, lock, ratio and the
 * frequency change request of the crawl.  On display 12 and 13 the pipe
 * field takes the pipe at bit 20 and "no pipe" as all three bits 21..19
 * set.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (CDCLK_CTL, BXT_CDCLK_CD2X_DIV_SEL_MASK, BXT_CDCLK_CD2X_DIV_SEL_1,
 * BXT_CDCLK_CD2X_DIV_SEL_1_5, BXT_CDCLK_CD2X_DIV_SEL_2,
 * BXT_CDCLK_CD2X_DIV_SEL_4, TGL_CDCLK_CD2X_PIPE, TGL_CDCLK_CD2X_PIPE_NONE,
 * ICL_CDCLK_CD2X_PIPE_NONE,
 * CDCLK_FREQ_DECIMAL_MASK, SKL_DSSM, ICL_DSSM_CDCLK_PLL_REFCLK_MASK,
 * ICL_DSSM_CDCLK_PLL_REFCLK_24MHz, ICL_DSSM_CDCLK_PLL_REFCLK_19_2MHz,
 * ICL_DSSM_CDCLK_PLL_REFCLK_38_4MHz, BXT_DE_PLL_ENABLE,
 * BXT_DE_PLL_PLL_ENABLE, BXT_DE_PLL_LOCK, BXT_DE_PLL_FREQ_REQ,
 * BXT_DE_PLL_FREQ_REQ_ACK, ICL_CDCLK_PLL_RATIO_MASK).
 */
#define I915_XD_CDCLK_CTL			0x46000U
#define I915_XD_CDCLK_CD2X_DIV			0x00c00000U
#define I915_XD_CDCLK_CD2X_DIV_1		0U
#define I915_XD_CDCLK_CD2X_DIV_1_5		1U
#define I915_XD_CDCLK_CD2X_DIV_2		2U
#define I915_XD_CDCLK_CD2X_DIV_4		3U
#define I915_XD_CDCLK_CD2X_PIPE_FIELD		0x00380000U
#define I915_XD_CDCLK_CD2X_PIPE(pipe)		((pipe) << 20U)
#define I915_XD_CDCLK_CD2X_PIPE_NONE		0x00380000U
#define I915_XD_CDCLK_DECIMAL			0x000007ffU
#define I915_XD_DSSM				0x51004U
#define I915_XD_DSSM_REFCLK			0xe0000000U
#define I915_XD_DSSM_REFCLK_24			0U
#define I915_XD_DSSM_REFCLK_19P2		1U
#define I915_XD_DSSM_REFCLK_38P4		2U
#define I915_XD_CDCLK_PLL_ENABLE		0x46070U
#define I915_XD_CDCLK_PLL_ON			0x80000000U
#define I915_XD_CDCLK_PLL_LOCK			0x40000000U
#define I915_XD_CDCLK_PLL_FREQ_REQUEST		0x00800000U
#define I915_XD_CDCLK_PLL_FREQ_ACK		0x00400000U
#define I915_XD_CDCLK_PLL_RATIO			0x000000ffU

/*
 * The combo PHY DPLLs: the enable word of DPLL0, DPLL1 and the
 * Alder Lake-S DPLL2 and DPLL3 (the TBT PLL and the Type-C PLLs follow
 * below), with the request and state of the PLL and of its power.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_DPLL0_ENABLE, _DPLL1_ENABLE, _ADLS_DPLL2_ENABLE, _ADLS_DPLL3_ENABLE,
 * PLL_ENABLE, PLL_LOCK, PLL_POWER_ENABLE, PLL_POWER_STATE).
 */
#define I915_XD_DPLL0_ENABLE			0x46010U
#define I915_XD_DPLL1_ENABLE			0x46014U
#define I915_XD_DPLL2_ENABLE_ADLS		0x46018U
#define I915_XD_DPLL3_ENABLE_ADLS		0x46030U
#define I915_XD_PLL_ENABLE			0x80000000U
#define I915_XD_PLL_LOCK			0x40000000U
#define I915_XD_PLL_POWER_ENABLE		0x08000000U
#define I915_XD_PLL_POWER_STATE			0x04000000U

/*
 * The TBT PLL and the Type-C PLLs of the ports: Tiger Lake's are at the
 * Ice Lake place (MG_PLL_ENABLE), display 13 moved them.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TBT_PLL_ENABLE, MG_PLL_ENABLE, _MG_PLL1_ENABLE, _MG_PLL2_ENABLE,
 * ADLP_PORTTC_PLL_ENABLE, PORTTC1_PLL_ENABLE, PORTTC2_PLL_ENABLE).
 */
#define I915_XD_TBT_PLL_ENABLE			0x46020U
#define I915_XD_TC_PLL_ENABLE(tc)		(0x46030U + 4U * (tc))
#define I915_XD_TC_PLL_ENABLE_ADLP(tc)		(0x46038U + 8U * (tc))

/*
 * The configuration words of the combo DPLLs on display 12 and 13: the
 * DCO integer and fraction (CFGCR0) and the dividers (CFGCR1).  DPLL0
 * and DPLL1 sit at the same place on every platform; the TBT PLL comes
 * third on Tiger Lake and display 13; Alder Lake-S's DPLL2 and DPLL3 use
 * other words.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_TGL_DPLL0_CFGCR0, _TGL_DPLL1_CFGCR0, _TGL_TBTPLL_CFGCR0,
 * _TGL_DPLL0_CFGCR1, _TGL_DPLL1_CFGCR1, _TGL_TBTPLL_CFGCR1,
 * _ADLS_DPLL4_CFGCR0, _ADLS_DPLL3_CFGCR0, _ADLS_DPLL4_CFGCR1,
 * _ADLS_DPLL3_CFGCR1, _TGL_DPLL0_DIV0, _TGL_DPLL1_DIV0).
 */
#define I915_XD_DPLL_CFGCR0(pll)		(0x164284U + 8U * (pll))
#define I915_XD_DPLL_CFGCR1(pll)		(0x164288U + 8U * (pll))
#define I915_XD_TBT_PLL_CFGCR0			0x16429cU
#define I915_XD_TBT_PLL_CFGCR1			0x1642a0U
#define I915_XD_DPLL2_CFGCR0_ADLS		0x164294U
#define I915_XD_DPLL2_CFGCR1_ADLS		0x164298U
#define I915_XD_DPLL3_CFGCR0_ADLS		0x1642c0U
#define I915_XD_DPLL3_CFGCR1_ADLS		0x1642c4U
#define I915_XD_DPLL_DIV0(pll)			(0x164b00U + 0x100U * (pll))
#define I915_XD_DPLL_DIV0_AFC_STARTUP		0x0e000000U

/*
 * The configuration words of Ice Lake's combo DPLLs and Thunderbolt PLL,
 * by their PLL number (0 and 1, the Thunderbolt PLL 2), 0x80 apart; Ice
 * Lake has no DIV0 word.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_ICL_DPLL0_CFGCR0, _ICL_DPLL1_CFGCR0, _ICL_DPLL0_CFGCR1,
 * _ICL_DPLL1_CFGCR1), and Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dpll_mgr.c (icl_dpll_write).
 */
#define I915_XD_DPLL_CFGCR0_ICL(pll)		(0x164000U + 0x80U * (pll))
#define I915_XD_DPLL_CFGCR1_ICL(pll)		(0x164004U + 0x80U * (pll))

/*
 * The fields of the DPLL configuration words: the DCO fraction and
 * integer; the Q divider ratio and mode, the K and P dividers, and the
 * central frequency select.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (DPLL_CFGCR0_DCO_FRACTION_MASK, DPLL_CFGCR0_DCO_INTEGER_MASK,
 * DPLL_CFGCR1_QDIV_RATIO_MASK, DPLL_CFGCR1_QDIV_MODE_SHIFT,
 * DPLL_CFGCR1_KDIV_MASK, DPLL_CFGCR1_PDIV_MASK,
 * DPLL_CFGCR1_CENTRAL_FREQ_8400).
 */
#define I915_XD_CFGCR0_DCO_FRACTION		0x01fffc00U
#define I915_XD_CFGCR0_DCO_INTEGER		0x000003ffU
#define I915_XD_CFGCR1_QDIV_RATIO		0x0003fc00U
#define I915_XD_CFGCR1_QDIV_MODE		0x00000200U
#define I915_XD_CFGCR1_KDIV			0x000001c0U
#define I915_XD_CFGCR1_PDIV			0x0000003cU
#define I915_XD_CFGCR1_CENTRAL_FREQ_8400	0x00000003U

/*
 * The map of the DDIs to the combo DPLLs.  On Tiger Lake and display 13
 * one word holds, per combo PHY, a two-bit DPLL select at twice the PHY
 * and a clock-off bit (PHY A, B, C at bits 10, 11, 24; PHY D and E,
 * which only Alder Lake-S has, at bits 4 and 5 of their own word), and a
 * clock-off bit per Type-C port.  Rocket Lake moves the selects of PHY D
 * and the clock-off bits; Alder Lake-S uses two words of three PHYs each.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ICL_DPCLKA_CFGCR0, ICL_DPCLKA_CFGCR0_DDI_CLK_OFF,
 * ICL_DPCLKA_CFGCR0_TC_CLK_OFF, ICL_DPCLKA_CFGCR0_DDI_CLK_SEL_SHIFT,
 * RKL_DPCLKA_CFGCR0_DDI_CLK_OFF, RKL_DPCLKA_CFGCR0_DDI_CLK_SEL_SHIFT,
 * _ADLS_DPCLKA_CFGCR0, _ADLS_DPCLKA_CFGCR1, ADLS_DPCLKA_CFGCR_DDI_SHIFT,
 * ADLS_DPCLKA_DDIA_SEL_MASK, ADLS_DPCLKA_DDIB_SEL_MASK,
 * ADLS_DPCLKA_DDII_SEL_MASK, ADLS_DPCLKA_DDIJ_SEL_MASK,
 * ADLS_DPCLKA_DDIK_SEL_MASK).
 */
#define I915_XD_DPCLKA_CFGCR0			0x164280U
#define I915_XD_DPCLKA_SEL_SHIFT(phy)		(2U * (phy))
#define I915_XD_DPCLKA_SEL_BITS			0x3U
#define I915_XD_DPCLKA_DDI_OFF_A		0x00000400U
#define I915_XD_DPCLKA_DDI_OFF_B		0x00000800U
#define I915_XD_DPCLKA_DDI_OFF_C		0x01000000U
#define I915_XD_DPCLKA_DDI_OFF_D		0x00000010U
#define I915_XD_DPCLKA_DDI_OFF_E		0x00000020U
#define I915_XD_DPCLKA_TC_OFF(tc)		(1U << ((tc) < 3U ? (tc) + 12U : (tc) - 3U + 21U))
#define I915_XD_DPCLKA_RKL_SEL_SHIFT_D		27U
#define I915_XD_DPCLKA_RKL_DDI_OFF(phy)		(1U << ((phy) + 10U))
#define I915_XD_DPCLKA_CFGCR1_ADLS		0x1642bcU
#define I915_XD_DPCLKA_ADLS_SEL_SHIFT(phy)	(((phy) % 3U) * 2U)

/*
 * A DDI's clock select on display 12 and 13 for the Type-C ports: no
 * clock, the Type-C PLL, or one of the TBT PLL's four link rates.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (PORT_CLK_SEL, _PORT_CLK_SEL_A, _PORT_CLK_SEL_B, DDI_CLK_SEL_MASK,
 * DDI_CLK_SEL_NONE, DDI_CLK_SEL_MG, DDI_CLK_SEL_TBT_162,
 * DDI_CLK_SEL_TBT_270, DDI_CLK_SEL_TBT_540, DDI_CLK_SEL_TBT_810).
 */
#define I915_XD_DDI_CLK_SEL(port)		(0x46100U + 4U * (port))
#define I915_XD_DDI_CLK_SEL_FIELD		0xf0000000U
#define I915_XD_DDI_CLK_SEL_NONE		0x0U
#define I915_XD_DDI_CLK_SEL_TC_PLL		0x8U
#define I915_XD_DDI_CLK_SEL_TBT_162		0xcU
#define I915_XD_DDI_CLK_SEL_TBT_270		0xdU
#define I915_XD_DDI_CLK_SEL_TBT_540		0xeU
#define I915_XD_DDI_CLK_SEL_TBT_810		0xfU

/*
 * A transcoder's clock select: which DDI's clock the transcoder runs on
 * (display 12 and 13: the index plus one in bits 31..28, 0 for none).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TRANS_CLK_SEL, _TRANS_CLK_SEL_A, _TRANS_CLK_SEL_B,
 * TGL_TRANS_CLK_SEL_DISABLED, TGL_TRANS_CLK_SEL_PORT).
 */
#define I915_XD_TRANS_CLK_SEL(trans)		(0x46140U + 4U * (trans))
#define I915_XD_TRANS_CLK_SEL_FIELD		0xf0000000U
#define I915_XD_TRANS_CLK_SEL_SHIFT		28U
#define I915_XD_TRANS_CLK_SEL_NONE		0x0U

/*
 * The transcoder timing: totals, blanks and syncs (the end in the upper
 * half, the start or the active size in the lower), the vertical sync
 * shift and the pipe's source size (width in the upper half, height in
 * the lower, both less one).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_TRANS_HTOTAL_A, _TRANS_HBLANK_A, _TRANS_HSYNC_A, _TRANS_VTOTAL_A,
 * _TRANS_VBLANK_A, _TRANS_VSYNC_A, _TRANS_VSYNCSHIFT_A, _PIPEASRC,
 * HTOTAL_MASK, HACTIVE_MASK, PIPESRC_WIDTH_MASK, PIPESRC_HEIGHT_MASK).
 */
#define I915_XD_TRANS_HTOTAL(trans)		(0x60000U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_HBLANK(trans)		(0x60004U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_HSYNC(trans)		(0x60008U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_VTOTAL(trans)		(0x6000cU + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_VBLANK(trans)		(0x60010U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_VSYNC(trans)		(0x60014U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_PIPE_SRCSZ(pipe)		(0x6001cU + I915_XD_TRANS_STRIDE * (pipe))
#define I915_XD_TRANS_VSYNCSHIFT(trans)		(0x60028U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TIMING_UPPER			0xffff0000U
#define I915_XD_TIMING_LOWER			0x0000ffffU

/*
 * The DisplayPort M/N values of a transcoder: the data M (with the TU
 * size less one in bits 30..25) and N, the link M and N.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_PIPEA_DATA_M1, _PIPEA_DATA_N1, _PIPEA_LINK_M1, _PIPEA_LINK_N1,
 * TU_SIZE_MASK, DATA_LINK_M_N_MASK).
 */
#define I915_XD_TRANS_DATA_M1(trans)		(0x60030U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_DATA_N1(trans)		(0x60034U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_LINK_M1(trans)		(0x60040U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_LINK_N1(trans)		(0x60044U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_MN_TU_SIZE			0x7e000000U
#define I915_XD_MN_VALUE			0x00ffffffU

/*
 * The transcoder's DDI function control: enable, the DDI select (the
 * DDI index plus one, display 12 and 13), the mode (HDMI, DVI, DP SST),
 * the bits per colour, the sync polarities, the eDP input select of the
 * Ice Lake eDP transcoder, and the port width (lanes less one).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_TRANS_DDI_FUNC_CTL_A, TRANS_DDI_FUNC_ENABLE, TGL_TRANS_DDI_PORT_MASK,
 * TGL_TRANS_DDI_SELECT_PORT, TRANS_DDI_MODE_SELECT_MASK,
 * TRANS_DDI_MODE_SELECT_HDMI, TRANS_DDI_MODE_SELECT_DVI,
 * TRANS_DDI_MODE_SELECT_DP_SST, TRANS_DDI_BPC_MASK, TRANS_DDI_BPC_8,
 * TRANS_DDI_BPC_10, TRANS_DDI_BPC_6, TRANS_DDI_BPC_12, TRANS_DDI_PVSYNC,
 * TRANS_DDI_PHSYNC, TRANS_DDI_EDP_INPUT_MASK, TRANS_DDI_PORT_WIDTH_MASK,
 * _TRANS_DDI_FUNC_CTL2_A).
 */
#define I915_XD_TRANS_DDI_FUNC_CTL(trans)	(0x60400U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_DDI_FUNC_ENABLE			0x80000000U
#define I915_XD_DDI_FUNC_SELECT			0x78000000U
#define I915_XD_DDI_FUNC_MODE			0x07000000U
#define I915_XD_DDI_FUNC_MODE_HDMI		0U
#define I915_XD_DDI_FUNC_MODE_DVI		1U
#define I915_XD_DDI_FUNC_MODE_DP_SST		2U
#define I915_XD_DDI_FUNC_BPC			0x00700000U
#define I915_XD_DDI_FUNC_BPC_8			0U
#define I915_XD_DDI_FUNC_BPC_10			1U
#define I915_XD_DDI_FUNC_BPC_6			2U
#define I915_XD_DDI_FUNC_BPC_12			3U
#define I915_XD_DDI_FUNC_VSYNC_HIGH		0x00020000U
#define I915_XD_DDI_FUNC_HSYNC_HIGH		0x00010000U
#define I915_XD_DDI_FUNC_EDP_INPUT		0x00007000U
#define I915_XD_DDI_FUNC_WIDTH			0x0000000eU
#define I915_XD_TRANS_DDI_FUNC_CTL2(trans)	(0x60404U + I915_XD_TRANS_STRIDE * (trans))

/*
 * The transcoder's main stream attribute word (MSA MISC: the
 * synchronous clock bit and the colour depth), and on display 13 the
 * set context latency.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_TRANSA_MSA_MISC, _TRANS_A_SET_CONTEXT_LATENCY,
 * TRANS_SET_CONTEXT_LATENCY_MASK), and Linux v6.8.12
 * include/drm/display/drm_dp.h (DP_MSA_MISC_SYNC_CLOCK,
 * DP_MSA_MISC_6_BPC, DP_MSA_MISC_8_BPC).
 */
#define I915_XD_TRANS_MSA_MISC(trans)		(0x60410U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_MSA_SYNC_CLOCK			0x00000001U
#define I915_XD_MSA_BPC_6			0x00000000U
#define I915_XD_MSA_BPC_8			0x00000020U
#define I915_XD_TRANS_SET_CONTEXT_LATENCY(trans) (0x6007cU + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_CONTEXT_LATENCY			0x0000ffffU

/*
 * The DisplayPort transport control and status of a transcoder (display
 * 12 and 13): enable, FEC, the multi-stream mode, the enhanced framing,
 * the training pattern (1, 2,
 * 3, 4, idle, normal); the idle-done status.
 *
 * Display 11 has the transport in its port instead: a transport is
 * named by its transcoder on display 12 and 13 and by
 * I915_XD_DP_TP_PORT(ddi) on display 11, and the two addresses follow
 * from the name.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_TGL_DP_TP_CTL_A, _TGL_DP_TP_STATUS_A, _DP_TP_CTL_A, _DP_TP_CTL_B,
 * _DP_TP_STATUS_A, _DP_TP_STATUS_B, DP_TP_CTL_ENABLE,
 * DP_TP_CTL_MODE_SST, DP_TP_CTL_FEC_ENABLE, DP_TP_CTL_MODE_MST,
 * DP_TP_CTL_ENHANCED_FRAME_ENABLE,
 * DP_TP_CTL_LINK_TRAIN_MASK, DP_TP_CTL_LINK_TRAIN_PAT1,
 * DP_TP_CTL_LINK_TRAIN_PAT2, DP_TP_CTL_LINK_TRAIN_PAT3,
 * DP_TP_CTL_LINK_TRAIN_PAT4, DP_TP_CTL_LINK_TRAIN_IDLE,
 * DP_TP_CTL_LINK_TRAIN_NORMAL, DP_TP_STATUS_IDLE_DONE).
 */
#define I915_XD_DP_TP_PORT(ddi)			(0x100U + (ddi))
#define I915_XD_DP_TP_CTL(tp)			((tp) >= I915_XD_DP_TP_PORT(0U) ? 0x64040U + 0x100U * ((tp) - I915_XD_DP_TP_PORT(0U)) : \
						 0x60540U + I915_XD_TRANS_STRIDE * (tp))
#define I915_XD_DP_TP_STATUS(tp)		(I915_XD_DP_TP_CTL(tp) + 4U)
#define I915_XD_DP_TP_ENABLE			0x80000000U
#define I915_XD_DP_TP_FEC			0x40000000U
#define I915_XD_DP_TP_MST			0x08000000U
#define I915_XD_DP_TP_ENHANCED_FRAMING		0x00040000U
#define I915_XD_DP_TP_PATTERN			0x00000700U
#define I915_XD_DP_TP_PATTERN_1			0U
#define I915_XD_DP_TP_PATTERN_2			1U
#define I915_XD_DP_TP_PATTERN_IDLE		2U
#define I915_XD_DP_TP_PATTERN_NORMAL		3U
#define I915_XD_DP_TP_PATTERN_3			4U
#define I915_XD_DP_TP_PATTERN_4			5U
#define I915_XD_DP_TP_STATUS_IDLE_DONE		0x02000000U

/*
 * The DDI buffer control: enable, the translation select, the PHY link
 * rate (display 13 Type-C), the lane reversal,
 * the idle status, the Type-C PHY ownership (display 13), the four-lane
 * strap of DDI A, and the port width (lanes less one).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_DDI_BUF_CTL_A, DDI_BUF_CTL_ENABLE, DDI_BUF_TRANS_SELECT,
 * DDI_BUF_PHY_LINK_RATE, DDI_BUF_PORT_REVERSAL,
 * DDI_BUF_IS_IDLE, DDI_BUF_CTL_TC_PHY_OWNERSHIP, DDI_A_4_LANES,
 * DDI_PORT_WIDTH_MASK).
 */
#define I915_XD_DDI_BUF_CTL(port)		(0x64000U + I915_XD_DDI_STRIDE * (port))
#define I915_XD_DDI_BUF_ENABLE			0x80000000U
#define I915_XD_DDI_BUF_TRANS_SELECT		0x0f000000U
#define I915_XD_DDI_BUF_PHY_LINK_RATE		0x00700000U
#define I915_XD_DDI_BUF_REVERSAL		0x00010000U
#define I915_XD_DDI_BUF_IDLE			0x00000080U
#define I915_XD_DDI_BUF_TC_OWNED		0x00000040U
#define I915_XD_DDI_BUF_A_4_LANES		0x00000010U
#define I915_XD_DDI_BUF_WIDTH			0x0000000eU

/*
 * The AUX channel of a DDI: the control word (busy, done, the done
 * interrupt, the timeout and receive errors, the timeout length, the
 * message size, the HDCP key select, the Thunderbolt IO select and the
 * two sync pulse counts less one) and the five data words.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp_aux_regs.h (_DPA_AUX_CH_CTL,
 * _DPB_AUX_CH_CTL, _DPA_AUX_CH_DATA1, DP_AUX_CH_CTL_SEND_BUSY,
 * DP_AUX_CH_CTL_DONE, DP_AUX_CH_CTL_INTERRUPT,
 * DP_AUX_CH_CTL_TIME_OUT_ERROR, DP_AUX_CH_CTL_TIME_OUT_MASK,
 * DP_AUX_CH_CTL_TIME_OUT_MAX, DP_AUX_CH_CTL_RECEIVE_ERROR,
 * DP_AUX_CH_CTL_MESSAGE_SIZE_MASK, DP_AUX_CH_CTL_AUX_AKSV_SELECT,
 * DP_AUX_CH_CTL_TBT_IO, DP_AUX_CH_CTL_FW_SYNC_PULSE_SKL_MASK,
 * DP_AUX_CH_CTL_SYNC_PULSE_SKL_MASK).
 */
#define I915_XD_DDI_AUX_CTL(aux)		(0x64010U + I915_XD_DDI_STRIDE * (aux))
#define I915_XD_DDI_AUX_DATA(aux, word)		(0x64014U + I915_XD_DDI_STRIDE * (aux) + 4U * (word))
#define I915_XD_DDI_AUX_DATA_WORDS		5U
#define I915_XD_AUX_CTL_BUSY			0x80000000U
#define I915_XD_AUX_CTL_DONE			0x40000000U
#define I915_XD_AUX_CTL_INTERRUPT		0x20000000U
#define I915_XD_AUX_CTL_TIMEOUT_ERROR		0x10000000U
#define I915_XD_AUX_CTL_TIMEOUT			0x0c000000U
#define I915_XD_AUX_CTL_TIMEOUT_MAX		3U
#define I915_XD_AUX_CTL_RECEIVE_ERROR		0x02000000U
#define I915_XD_AUX_CTL_SIZE			0x01f00000U
#define I915_XD_AUX_CTL_HDCP_KEY		0x00008000U
#define I915_XD_AUX_CTL_TBT_IO			0x00000800U
#define I915_XD_AUX_CTL_FW_SYNC_PULSE		0x000003e0U
#define I915_XD_AUX_CTL_SYNC_PULSE		0x0000001fU

/*
 * The pipe and plane registers of display 12 and 13: the scan line, the
 * frame counter, the arbiter's programmable slots, the pipe's
 * miscellaneous word (the bits per colour and the dither), the bottom
 * colour, the pipe chicken word (underrun recovery), and the pipe's
 * status (underruns).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_PIPEADSL, PIPEDSL_LINE_MASK, _PIPEA_FRMCOUNT_G4X,
 * _PIPEA_FLIPCOUNT_G4X, _PIPE_ARB_CTL_A, PIPE_ARB_USE_PROG_SLOTS,
 * _PIPE_MISC_A, _PIPE_MISC_B, PIPE_MISC_BPC_MASK, PIPE_MISC_BPC_8,
 * PIPE_MISC_BPC_10, PIPE_MISC_BPC_6, PIPE_MISC_BPC_12_ADLP,
 * PIPE_MISC_DITHER_ENABLE, PIPE_MISC_DITHER_TYPE_MASK,
 * PIPE_MISC_PIXEL_ROUNDING_TRUNC, _PIPEA_CHICKEN,
 * UNDERRUN_RECOVERY_DISABLE_ADLP, _ICL_PIPE_A_STATUS,
 * PIPE_STATUS_UNDERRUN, PIPE_STATUS_SOFT_UNDERRUN_XELPD,
 * PIPE_STATUS_HARD_UNDERRUN_XELPD, PIPE_STATUS_PORT_UNDERRUN_XELPD), and
 * Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_color_regs.h (_SKL_BOTTOM_COLOR_A,
 * _SKL_BOTTOM_COLOR_B, SKL_BOTTOM_COLOR_GAMMA_ENABLE,
 * SKL_BOTTOM_COLOR_CSC_ENABLE).
 */
#define I915_XD_PIPE_SCANLINE(pipe)		(0x70000U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PIPE_SCANLINE_LINE		0x000fffffU
#define I915_XD_TRANS_CONF(trans)		(0x70008U + I915_XD_PIPE_STRIDE * (trans))
#define I915_XD_TRANS_CONF_ENABLE		0x80000000U
#define I915_XD_TRANS_CONF_STATE		0x40000000U
#define I915_XD_PIPE_ARB_CTL(pipe)		(0x70028U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PIPE_ARB_PROG_SLOTS		0x00002000U
#define I915_XD_PIPE_MISC(pipe)			(0x70030U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PIPE_MISC_ROUNDING_TRUNC	0x00000100U
#define I915_XD_PIPE_MISC_BPC			0x000000e0U
#define I915_XD_PIPE_MISC_BPC_8			0U
#define I915_XD_PIPE_MISC_BPC_10		1U
#define I915_XD_PIPE_MISC_BPC_6			2U
#define I915_XD_PIPE_MISC_BPC_12		4U
#define I915_XD_PIPE_MISC_DITHER		0x00000010U
#define I915_XD_PIPE_MISC_DITHER_TYPE		0x0000000cU
#define I915_XD_PIPE_BOTTOM_COLOR(pipe)		(0x70034U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_BOTTOM_COLOR_GAMMA		0x80000000U
#define I915_XD_BOTTOM_COLOR_CSC		0x40000000U
#define I915_XD_PIPE_CHICKEN(pipe)		(0x70038U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PIPE_CHICKEN_NO_UNDERRUN_RECOVERY 0x40000000U
#define I915_XD_PIPE_FRMCNT(pipe)		(0x70040U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PIPE_FLIPCNT(pipe)		(0x70044U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PIPE_STATUS(pipe)		(0x70058U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PIPE_STATUS_UNDERRUN		0x80000000U
#define I915_XD_PIPE_STATUS_UNDERRUN_SOFT	0x10000000U
#define I915_XD_PIPE_STATUS_UNDERRUN_HARD	0x08000000U
#define I915_XD_PIPE_STATUS_UNDERRUN_PORT	0x04000000U

/*
 * The pipe's colour controls the display sets to pass pixels through:
 * the gamma mode (8-bit, with the pre- and post-CSC gamma enables) and
 * the CSC mode.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_color_regs.h (_GAMMA_MODE_A,
 * _GAMMA_MODE_B, GAMMA_MODE_MODE_MASK, GAMMA_MODE_MODE_8BIT,
 * PRE_CSC_GAMMA_ENABLE, POST_CSC_GAMMA_ENABLE, _PIPE_A_CSC_MODE,
 * _PIPE_B_CSC_MODE, ICL_CSC_ENABLE, ICL_OUTPUT_CSC_ENABLE).
 */
#define I915_XD_GAMMA_MODE(pipe)		(0x4a480U + 0x800U * (pipe))
#define I915_XD_GAMMA_MODE_PRE_CSC		0x80000000U
#define I915_XD_GAMMA_MODE_POST_CSC		0x40000000U
#define I915_XD_GAMMA_MODE_MODE			0x00000003U
#define I915_XD_CSC_MODE(pipe)			(0x49028U + 0x100U * (pipe))
#define I915_XD_CSC_MODE_CSC			0x80000000U
#define I915_XD_CSC_MODE_OUTPUT_CSC		0x40000000U

/*
 * The first pipe scaler's control (the display turns scalers off).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_PS_1A_CTRL, _PS_1B_CTRL, PS_SCALER_EN).
 */
#define I915_XD_PS_CTRL_1(pipe)			(0x68180U + 0x800U * (pipe))
#define I915_XD_PS_CTRL_ENABLE			0x80000000U

/*
 * A pipe's two scalers, each with its control, window position and
 * window size (all three are cleared when the pipe stops).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_PS_1A_CTRL, _PS_2A_CTRL, _PS_1B_CTRL, _PS_WIN_POS_1A, _PS_WIN_SZ_1A).
 */
#define I915_XD_SCALERS				2U
#define I915_XD_PS_CTRL(pipe, scaler)		(0x68180U + 0x800U * (pipe) + 0x100U * (scaler))
#define I915_XD_PS_WIN_POS(pipe, scaler)	(0x68170U + 0x800U * (pipe) + 0x100U * (scaler))
#define I915_XD_PS_WIN_SZ(pipe, scaler)		(0x68174U + 0x800U * (pipe) + 0x100U * (scaler))

/*
 * A transcoder's data island packets: the control word with the enables
 * of the infoframes (AVI, GCP, VS, GMP, SPD and the HDR metadata's), of
 * the DisplayPort VSC packet and of the picture parameter packet.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_HSW_VIDEO_DIP_CTL_A, VIDEO_DIP_ENABLE_AVI_HSW,
 * VIDEO_DIP_ENABLE_GCP_HSW, VIDEO_DIP_ENABLE_VS_HSW,
 * VIDEO_DIP_ENABLE_GMP_HSW, VIDEO_DIP_ENABLE_SPD_HSW,
 * VIDEO_DIP_ENABLE_DRM_GLK, VIDEO_DIP_ENABLE_VSC_HSW, VDIP_ENABLE_PPS).
 */
#define I915_XD_DIP_CTL(trans)			(0x60200U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_DIP_INFOFRAMES			0x10011111U
#define I915_XD_DIP_VSC				0x00100000U
#define I915_XD_DIP_PPS				0x01000000U

/*
 * A pipe's splitter for an eDP panel driven as several streams: its
 * enable, the number of segments and the overlapping pixels (the three
 * fields are cleared when no splitter is used).  The address is that of
 * pipe B's and C's control with pipe A one step below them.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_vdsc_regs.h (_ICL_PIPE_DSS_CTL1_PB,
 * _ICL_PIPE_DSS_CTL1_PC, ICL_PIPE_DSS_CTL1, SPLITTER_ENABLE,
 * SPLITTER_CONFIGURATION_MASK, OVERLAP_PIXELS_MASK).
 */
#define I915_XD_PIPE_DSS_CTL1(pipe)		(0x78000U + 0x200U * (pipe))
#define I915_XD_PIPE_DSS_SPLITTER		0x860f0000U

/*
 * The first universal plane of a pipe: control (enable, the arbiter
 * slots of display 13, the pixel format, the tiling), stride, position,
 * size, surface, offset, live surface, colour control and the buffer
 * allocation.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_PLANE_CTL_1_A, _PLANE_CTL_1_B, PLANE_CTL_ENABLE,
 * PLANE_CTL_ARB_SLOTS_MASK, PLANE_CTL_FORMAT_MASK_ICL,
 * PLANE_CTL_FORMAT_XRGB_8888, PLANE_CTL_TILED_MASK,
 * PLANE_CTL_TILED_LINEAR, _PLANE_STRIDE_1_A, PLANE_STRIDE__MASK,
 * _PLANE_POS_1_A, _PLANE_SIZE_1_A, _PLANE_SURF_1_A, PLANE_SURF_ADDR_MASK,
 * _PLANE_OFFSET_1_A, _PLANE_SURFLIVE_1_A, _PLANE_COLOR_CTL_1_A,
 * _PLANE_BUF_CFG_1_A, _PLANE_BUF_CFG_1_B, PLANE_BUF_END_MASK,
 * PLANE_BUF_START_MASK).
 */
#define I915_XD_PLANE_CTL(pipe)			(0x70180U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_CTL_ENABLE		0x80000000U
#define I915_XD_PLANE_CTL_ARB_SLOTS		0x70000000U
#define I915_XD_PLANE_CTL_FORMAT		0x0f800000U
#define I915_XD_PLANE_CTL_FORMAT_XRGB8888	0x08U
#define I915_XD_PLANE_CTL_TILING		0x00001c00U
#define I915_XD_PLANE_STRIDE(pipe)		(0x70188U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_STRIDE_UNITS		0x00000fffU
#define I915_XD_PLANE_POS(pipe)			(0x7018cU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_SIZE(pipe)		(0x70190U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_SURF(pipe)		(0x7019cU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_SURF_ADDRESS		0xfffff000U
#define I915_XD_PLANE_OFFSET(pipe)		(0x701a4U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_SURFLIVE(pipe)		(0x701acU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_COLOR_CTL(pipe)		(0x701ccU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_BUF_CFG(pipe)		(0x7027cU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_BUF_CFG_END			0x0fff0000U
#define I915_XD_BUF_CFG_START			0x00000fffU

/*
 * The first plane's watermarks: one word per level (eight on display
 * 12, six on display 13), the transition watermark, and display 13's
 * hardware SAGV watermark and its transition; the cursor's buffer
 * allocation; and a pipe's line time.  A watermark word holds the
 * enable, the lines and the blocks.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/skl_watermark_regs.h (_PLANE_WM_1_A_0,
 * _PLANE_WM_1_B_0, _PLANE_WM_TRANS_1_A, _PLANE_WM_SAGV_1_A,
 * _PLANE_WM_SAGV_TRANS_1_A, PLANE_WM_EN, PLANE_WM_IGNORE_LINES,
 * PLANE_WM_LINES_MASK, PLANE_WM_BLOCKS_MASK, _CUR_BUF_CFG_A,
 * _CUR_BUF_CFG_B, _CUR_WM_A_0), and Linux v6.8.12
 * drivers/gpu/drm/i915/i915_reg.h (_WM_LINETIME_A, _WM_LINETIME_B,
 * HSW_LINETIME_MASK).
 */
#define I915_XD_PLANE_WM(pipe, level)		(0x70240U + I915_XD_PIPE_STRIDE * (pipe) + 4U * (level))
#define I915_XD_PLANE_WM_TRANS(pipe)		(0x70268U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_WM_SAGV(pipe)		(0x70258U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_WM_SAGV_TRANS(pipe)	(0x7025cU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_CUR_WM(pipe, level)		(0x70140U + I915_XD_PIPE_STRIDE * (pipe) + 4U * (level))
#define I915_XD_CUR_BUF_CFG(pipe)		(0x7017cU + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_WM_ENABLE			0x80000000U
#define I915_XD_WM_IGNORE_LINES			0x40000000U
#define I915_XD_WM_LINES			0x07ffc000U
#define I915_XD_WM_BLOCKS			0x00000fffU
#define I915_XD_WM_LINETIME(pipe)		(0x45270U + 4U * (pipe))
#define I915_XD_LINETIME			0x000001ffU

/*
 * The pipe DMC control of a pipe (display 12 and 13), and the DMC
 * program storage and event handlers.  The main DMC's registers are at
 * 0x8f000; a pipe DMC's copy of the same registers is at its own base
 * (Tiger Lake 0x92000 + 0x400 a pipe; display 13 0x5f000 + 0x400 a pipe).
 * The MMIO ranges a package may write are checked against the main and
 * pipe ranges.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dmc_regs.h (_PIPEDMC_CONTROL_A,
 * _PIPEDMC_CONTROL_B, PIPEDMC_ENABLE, _ADLP_PIPEDMC_REG_MMIO_BASE_A,
 * _TGL_PIPEDMC_REG_MMIO_BASE_A, __DMC_REG_MMIO_BASE,
 * DMC_EVENT_HANDLER_COUNT_GEN12, _DMC_EVT_HTP_0, _DMC_EVT_CTL_0,
 * DMC_EVT_CTL_ENABLE, DMC_EVT_CTL_RECURRING, DMC_EVT_CTL_TYPE_MASK,
 * DMC_EVT_CTL_TYPE_EDGE_0_1, DMC_EVT_CTL_EVENT_ID_MASK,
 * DMC_EVT_CTL_EVENT_ID_FALSE, DMC_EVT_CTL_EVENT_ID_VBLANK_A,
 * DMC_EVT_CTL_EVENT_ID_CLK_MSEC, DMC_V1_MMIO_START_RANGE,
 * TGL_MAIN_MMIO_START, TGL_MAIN_MMIO_END, _TGL_PIPEA_MMIO_START,
 * _TGL_PIPEA_MMIO_END, _TGL_PIPEB_MMIO_START, _TGL_PIPEB_MMIO_END,
 * ADLP_PIPE_MMIO_START, ADLP_PIPE_MMIO_END).
 */
#define I915_XD_PIPEDMC_CONTROL(pipe)		(0x45250U + 4U * (pipe))
#define I915_XD_PIPEDMC_ENABLE			0x00000001U
#define I915_XD_DMC_MAIN_BASE			0x8f000U
#define I915_XD_DMC_PIPE_BASE_TGL		0x92000U
#define I915_XD_DMC_PIPE_BASE_ADLP		0x5f000U
#define I915_XD_DMC_PIPE_STRIDE			0x400U
#define I915_XD_DMC_HANDLERS			8U
#define I915_XD_DMC_EVT_HTP_OFFSET		0x004U
#define I915_XD_DMC_EVT_CTL_OFFSET		0x034U
#define I915_XD_DMC_EVT_ENABLE			0x80000000U
#define I915_XD_DMC_EVT_RECURRING		0x40000000U
#define I915_XD_DMC_EVT_TYPE			0x00030000U
#define I915_XD_DMC_EVT_TYPE_EDGE_0_1		3U
#define I915_XD_DMC_EVT_ID			0x0000ff00U
#define I915_XD_DMC_EVT_ID_FALSE		0x01U
#define I915_XD_DMC_EVT_ID_VBLANK_A		0x32U
#define I915_XD_DMC_EVT_ID_CLK_MSEC		0xbfU
#define I915_XD_DMC_V1_PROGRAM_BASE		0x80000U
#define I915_XD_DMC_MAIN_MMIO_START		0x8f000U
#define I915_XD_DMC_MAIN_MMIO_END		0x8ffffU
#define I915_XD_DMC_PIPE_A_MMIO_START_TGL	0x92000U
#define I915_XD_DMC_PIPE_A_MMIO_END_TGL		0x93fffU
#define I915_XD_DMC_PIPE_B_MMIO_START_TGL	0x96000U
#define I915_XD_DMC_PIPE_B_MMIO_END_TGL		0x97fffU
#define I915_XD_DMC_PIPE_MMIO_START_ADLP	0x5f000U
#define I915_XD_DMC_PIPE_MMIO_END_ADLP		0x5ffffU

/*
 * A combo PHY's register block: the base of each PHY (A..E), and in it
 * the common lane words (DW5 with the power-down enable and the clock
 * configuration, DW10 with the lane power-down field, DW12 with the AUX
 * lane enable), the compensation words (DW0 init, DW1, DW3 with the
 * process and voltage, DW8 with the reference generator, DW9, DW10),
 * the PCS words (DW1 with the keeper) and the transmit words (DW2 with
 * the swing select and the scalar, DW4 with the cursor coefficients and
 * the load generator, DW5 with the training enable and the scaling
 * mode, DW7 with the N scalar) for the group, the AUX lane and each of
 * the four lanes.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_combo_phy_regs.h (_ICL_COMBOPHY_A,
 * _ICL_COMBOPHY_B, _EHL_COMBOPHY_C, _RKL_COMBOPHY_D, _ADL_COMBOPHY_E,
 * ICL_PORT_CL_DW5, CL_POWER_DOWN_ENABLE, SUS_CLOCK_CONFIG,
 * ICL_PORT_CL_DW10, PWR_DOWN_LN_MASK, PWR_UP_ALL_LANES,
 * EDP4K2K_MODE_OVRD_EN, EDP4K2K_MODE_OVRD_OPTIMIZED,
 * PWR_DOWN_LN_3_2_1, PWR_DOWN_LN_3_2, PWR_DOWN_LN_3, PWR_DOWN_LN_2_1_0,
 * PWR_DOWN_LN_1_0, PWR_DOWN_LN_3_1, PWR_DOWN_LN_3_1_0,
 * ICL_PORT_CL_DW12, ICL_LANE_ENABLE_AUX, _ICL_PORT_COMP, COMP_INIT,
 * PROCESS_INFO_MASK, VOLTAGE_INFO_MASK, IREFGEN, _ICL_PORT_PCS_AUX,
 * _ICL_PORT_PCS_GRP, _ICL_PORT_PCS_LN, COMMON_KEEPER_EN,
 * _ICL_PORT_TX_AUX, _ICL_PORT_TX_GRP, _ICL_PORT_TX_LN,
 * SWING_SEL_UPPER_MASK, SWING_SEL_LOWER_MASK, RCOMP_SCALAR_MASK,
 * LOADGEN_SELECT, POST_CURSOR_1_MASK, POST_CURSOR_2_MASK,
 * CURSOR_COEFF_MASK, TX_TRAINING_EN, TAP2_DISABLE, TAP3_DISABLE,
 * SCALING_MODE_SEL_MASK, RTERM_SELECT_MASK, N_SCALAR_MASK).
 */
#define I915_XD_COMBO_PHY_A			0x162000U
#define I915_XD_COMBO_PHY_B			0x06c000U
#define I915_XD_COMBO_PHY_C			0x160000U
#define I915_XD_COMBO_PHY_D			0x161000U
#define I915_XD_COMBO_PHY_E			0x16b000U
#define I915_XD_PORT_CL_DW(base, dw)		((base) + 4U * (dw))
#define I915_XD_CL_DW5_POWER_DOWN_ENABLE	0x00000010U
#define I915_XD_CL_DW5_SUS_CLOCK_CONFIG		0x00000003U
#define I915_XD_CL_DW10_LANE_POWER_DOWN		0x000000f0U
#define I915_XD_CL_DW10_EDP4K2K_OVERRIDE	0x00000008U
#define I915_XD_CL_DW10_EDP4K2K_OPTIMIZED	0x00000004U
#define I915_XD_CL_DW12_AUX_LANE_ENABLE		0x00000001U
#define I915_XD_PORT_COMP_DW(base, dw)		((base) + 0x100U + 4U * (dw))
#define I915_XD_COMP_DW0_INIT			0x80000000U
#define I915_XD_COMP_DW3_PROCESS		0x1c000000U
#define I915_XD_COMP_DW3_VOLTAGE		0x03000000U
#define I915_XD_COMP_DW8_IREFGEN		0x01000000U
#define I915_XD_PORT_PCS_DW_AUX(base, dw)	((base) + 0x300U + 4U * (dw))
#define I915_XD_PORT_PCS_DW_GRP(base, dw)	((base) + 0x600U + 4U * (dw))
#define I915_XD_PORT_PCS_DW_LN(base, ln, dw)	((base) + 0x800U + 0x100U * (ln) + 4U * (dw))
#define I915_XD_PCS_DW1_COMMON_KEEPER		0x04000000U
#define I915_XD_PORT_TX_DW_AUX(base, dw)	((base) + 0x380U + 4U * (dw))
#define I915_XD_PORT_TX_DW_GRP(base, dw)	((base) + 0x680U + 4U * (dw))
#define I915_XD_PORT_TX_DW_LN(base, ln, dw)	((base) + 0x880U + 0x100U * (ln) + 4U * (dw))
#define I915_XD_TX_DW2_SWING_UPPER		0x00008000U
#define I915_XD_TX_DW2_SWING_LOWER		0x00003800U
#define I915_XD_TX_DW2_RCOMP_SCALAR		0x000000ffU
#define I915_XD_TX_DW4_LOADGEN			0x80000000U
#define I915_XD_TX_DW4_POST_CURSOR_1		0x0003f000U
#define I915_XD_TX_DW4_POST_CURSOR_2		0x00000fc0U
#define I915_XD_TX_DW4_CURSOR_COEFF		0x0000003fU
#define I915_XD_TX_DW5_TRAINING			0x80000000U
#define I915_XD_TX_DW5_TAP2_DISABLE		0x40000000U
#define I915_XD_TX_DW5_TAP3_DISABLE		0x20000000U
#define I915_XD_TX_DW5_SCALING_MODE		0x001c0000U
#define I915_XD_TX_DW5_RTERM			0x00000038U
#define I915_XD_TX_DW7_N_SCALAR			0x7f000000U

/*
 * The AUX analog override word of Ice Lake's combo PHYs A and B (0x398
 * into the PHY's block), with its LDO bypass and enable bits (display WA
 * #1178).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_ICL_AUX_ANAOVRD1_A, _ICL_AUX_ANAOVRD1_B, ICL_AUX_ANAOVRD1_LDO_BYPASS,
 * ICL_AUX_ANAOVRD1_ENABLE).
 */
#define I915_XD_AUX_ANAOVRD1(base)		((base) + 0x398U)
#define I915_XD_AUX_ANAOVRD1_LDO_BYPASS		0x00000080U
#define I915_XD_AUX_ANAOVRD1_ENABLE		0x00000001U

/*
 * The duty cycle corrector of a combo PHY on display 12 and 13: the
 * ODCC clock select and its divider (DW8 of the transmit lanes) and the
 * DCC mode (DW1 of the PCS lanes), which the PHY init sets to run once.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_combo_phy_regs.h
 * (ICL_PORT_TX_DW8_ODCC_CLK_SEL, ICL_PORT_TX_DW8_ODCC_CLK_DIV_SEL_MASK,
 * ICL_PORT_TX_DW8_ODCC_CLK_DIV_SEL_DIV2, DCC_MODE_SELECT_MASK,
 * RUN_DCC_ONCE).
 */
#define I915_XD_TX_DW8_ODCC_CLOCK_SELECT	0x80000000U
#define I915_XD_TX_DW8_ODCC_DIVIDER		0x60000000U
#define I915_XD_TX_DW8_ODCC_DIVIDE_BY_2		1U
#define I915_XD_PCS_DW1_DCC_MODE		0x00300000U
#define I915_XD_PCS_DW1_DCC_RUN_ONCE		0U

/*
 * The frame buffer compressor of pipe A (display 12 and 13 have that one
 * only): its control register and enable.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ILK_DPFC_CONTROL, DPFC_CTL_EN); intel_display_device.c (the fbc_mask
 * of display 12 and 13: FBC A).
 */
#define I915_XD_DPFC_CONTROL_A			0x43208U
#define I915_XD_DPFC_CONTROL_ENABLE		0x80000000U

/*
 * The display 12 chickens of the start: the frame buffer compression
 * chicken of pipe A (the compressor's dummy pixel) and the clock request
 * policy (the memory-up override).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (ILK_DPFC_CHICKEN, DPFC_CHICKEN_COMP_DUMMY_PIXEL, CLKREQ_POLICY,
 * CLKREQ_POLICY_MEM_UP_OVRD).
 */
#define I915_XD_DPFC_CHICKEN_A			0x43224U
#define I915_XD_DPFC_CHICKEN_DUMMY_PIXEL	0x00004000U
#define I915_XD_CLKREQ_POLICY			0x101038U
#define I915_XD_CLKREQ_POLICY_MEM_UP_OVERRIDE	0x00000002U

/*
 * The PHY miscellaneous word of combo PHY A and B: the IO compensation
 * power-down.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_ICL_PHY_MISC_A, _ICL_PHY_MISC_B, ICL_PHY_MISC_DE_IO_COMP_PWR_DOWN).
 */
#define I915_XD_PHY_MISC(phy)			(0x64c00U + 4U * (phy))
#define I915_XD_PHY_MISC_IO_COMP_POWER_DOWN	0x00800000U

/*
 * The Type-C flexible IO adapters (FIA 1..3): the live state and the
 * modular flag (DFLEXDPSP), the PHY ready (DFLEXDPPMS), the
 * driver's ownership (DFLEXDPCSSS), the pin assignment (DFLEXPA1) and
 * the main link lanes (DFLEXDPMLE1); one byte or nibble a port of the
 * FIA.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_mg_phy_regs.h (FIA1_BASE, FIA2_BASE,
 * FIA3_BASE, PORT_TX_DFLEXDPMLE1, DFLEXDPMLE1_DPMLETC_MASK), and
 * drivers/gpu/drm/i915/i915_reg.h (PORT_TX_DFLEXDPSP, MODULAR_FIA_MASK,
 * TC_LIVE_STATE_TBT, TC_LIVE_STATE_TC, PORT_TX_DFLEXDPPMS,
 * DP_PHY_MODE_STATUS_COMPLETED, PORT_TX_DFLEXDPCSSS,
 * DP_PHY_MODE_STATUS_NOT_SAFE, PORT_TX_DFLEXPA1, DP_PIN_ASSIGNMENT_MASK).
 */
#define I915_XD_FIA1_BASE			0x163000U
#define I915_XD_FIA2_BASE			0x16e000U
#define I915_XD_FIA3_BASE			0x16f000U
#define I915_XD_FIA_DFLEXPA1			0x880U
#define I915_XD_FIA_DFLEXDPPMS			0x890U
#define I915_XD_FIA_DFLEXDPCSSS			0x894U
#define I915_XD_FIA_DFLEXDPSP			0x8a0U
#define I915_XD_FIA_DFLEXDPMLE1			0x8c0U
#define I915_XD_DFLEXDPSP_MODULAR		0x00000010U
#define I915_XD_DFLEXDPSP_LIVE_TBT(idx)		(1U << ((idx) * 8U + 6U))
#define I915_XD_DFLEXDPSP_LIVE_TC(idx)		(1U << ((idx) * 8U + 5U))
#define I915_XD_DFLEXDPPMS_READY(idx)		(1U << (idx))
#define I915_XD_DFLEXDPCSSS_OWNED(idx)		(1U << (idx))
#define I915_XD_DFLEXPA1_PIN(idx)		(0xfU << ((idx) * 4U))
#define I915_XD_DFLEXDPMLE1_LANES(idx)		(0xfU << ((idx) * 4U))

/*
 * The lanes the sink of a Type-C port assigned to DisplayPort (in
 * DFLEXDPSP, a nibble at the port's byte), and the main link lanes the
 * display uses (in DFLEXDPMLE1, the port's nibble): lane 0, lanes 1..0,
 * lane 3, lanes 3..2, lanes 3..0.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (DP_LANE_ASSIGNMENT_SHIFT, DP_LANE_ASSIGNMENT_MASK), and
 * drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (DFLEXDPMLE1_DPMLETC_ML0, DFLEXDPMLE1_DPMLETC_ML1_0,
 * DFLEXDPMLE1_DPMLETC_ML3, DFLEXDPMLE1_DPMLETC_ML3_2,
 * DFLEXDPMLE1_DPMLETC_ML3_0).
 */
#define I915_XD_DFLEXDPSP_LANES(idx)		(0xfU << ((idx) * 8U))
#define I915_XD_DFLEXDPMLE1_ML0			0x1U
#define I915_XD_DFLEXDPMLE1_ML1_0		0x3U
#define I915_XD_DFLEXDPMLE1_ML3			0x8U
#define I915_XD_DFLEXDPMLE1_ML3_2		0xcU
#define I915_XD_DFLEXDPMLE1_ML3_0		0xfU

/*
 * The Type-C subsystem status of display 13: the pin assignment, the
 * PHY ready and the two live states.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_TCSS_DDI_STATUS_1, _TCSS_DDI_STATUS_2,
 * TCSS_DDI_STATUS_PIN_ASSIGNMENT_MASK, TCSS_DDI_STATUS_READY,
 * TCSS_DDI_STATUS_HPD_LIVE_STATUS_TBT,
 * TCSS_DDI_STATUS_HPD_LIVE_STATUS_ALT).
 */
#define I915_XD_TCSS_DDI_STATUS(tc)		(0x161500U + 4U * (tc))
#define I915_XD_TCSS_PIN_ASSIGNMENT		0x1e000000U
#define I915_XD_TCSS_READY			0x00000004U
#define I915_XD_TCSS_LIVE_TBT			0x00000002U
#define I915_XD_TCSS_LIVE_ALT			0x00000001U

/*
 * The Dekel PHY of a Type-C port (Tiger Lake and display 13): a 4 KiB
 * window per port, in which bits 15..12 of a PHY offset select the bank
 * through the index register (port n's byte of HIP_INDEX_REG0 for ports
 * 0..3, of HIP_INDEX_REG1 for 4 and 5).  The offsets below are PHY
 * offsets: lane 1's copy of a lane register is 0x1000 above lane 0's.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h (_DKL_PHY1_BASE,
 * _DKL_PHY2_BASE, _DKL_BANK_SHIFT, _HIP_INDEX_REG0, _HIP_INDEX_REG1,
 * _HIP_INDEX_SHIFT, _DKL_PCS_DW5_LN0, DKL_PCS_DW5_CORE_SOFTRESET,
 * _DKL_PLL_DIV0, DKL_PLL_DIV0_AFC_STARTUP_MASK, _DKL_PLL_DIV1,
 * _DKL_PLL_SSC, _DKL_PLL_BIAS, _DKL_PLL_TDC_COLDST_BIAS,
 * _DKL_REFCLKIN_CTL, _DKL_CLKTOP2_HSCLKCTL, _DKL_CLKTOP2_CORECLKCTL1,
 * _DKL_TX_DPCNTL0_LN0, _DKL_TX_DPCNTL1_LN0, _DKL_TX_DPCNTL2_LN0,
 * DKL_TX_DP20BITMODE, DKL_TX_DPCNTL2_CFG_LOADGENSELECT_TX1_MASK,
 * DKL_TX_DPCNTL2_CFG_LOADGENSELECT_TX2_MASK, _DKL_TX_FW_CALIB_LN0,
 * DKL_TX_CFG_DISABLE_WAIT_INIT, _DKL_TX_PMD_LANE_SUS_LN0,
 * _DKL_TX_DW17_LN0, _DKL_TX_DW18_LN0, _DKL_DP_MODE_LN0,
 * _DKL_CMN_UC_DW27, DKL_CMN_UC_DW27_UC_HEALTH, DKL_TX_PRESHOOT_COEFF_MASK,
 * DKL_TX_DE_EMPAHSIS_COEFF_MASK, DKL_TX_VSWING_CONTROL_MASK), and
 * drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (MG_DP_MODE_CFG_DP_X1_MODE, MG_DP_MODE_CFG_DP_X2_MODE).
 */
#define I915_XD_DKL_WINDOW(tc)			(0x168000U + 0x1000U * (tc))
#define I915_XD_DKL_BANK_SHIFT			12U
#define I915_XD_DKL_OFFSET_IN_BANK		0x00000fffU
#define I915_XD_HIP_INDEX_REG(tc)		((tc) < 4U ? 0x1010a0U : 0x1010a4U)
#define I915_XD_HIP_INDEX_SHIFT(tc)		(8U * ((tc) % 4U))
#define I915_XD_DKL_LANE_STRIDE			0x1000U
#define I915_XD_DKL_PCS_DW5			0x0014U
#define I915_XD_DKL_PCS_DW5_CORE_SOFTRESET	0x00000800U
#define I915_XD_DKL_DP_MODE			0x00a0U
#define I915_XD_DKL_DP_MODE_X1			0x00000040U
#define I915_XD_DKL_DP_MODE_X2			0x00000080U
#define I915_XD_DKL_TX_DPCNTL0			0x02c0U
#define I915_XD_DKL_TX_DPCNTL1			0x02c4U
#define I915_XD_DKL_TX_DPCNTL2			0x02c8U
#define I915_XD_DKL_TX_FW_CALIB			0x02f8U
#define I915_XD_DKL_TX_PMD_LANE_SUS		0x0d00U
#define I915_XD_DKL_TX_DW17			0x0dc4U
#define I915_XD_DKL_TX_DW18			0x0dc8U
#define I915_XD_DKL_CLKTOP2_HSCLKCTL		0x20d4U
#define I915_XD_DKL_CLKTOP2_CORECLKCTL1		0x20d8U
#define I915_XD_DKL_REFCLKIN_CTL		0x212cU
#define I915_XD_DKL_PLL_DIV0			0x2200U
#define I915_XD_DKL_PLL_DIV1			0x2204U
#define I915_XD_DKL_PLL_SSC			0x2210U
#define I915_XD_DKL_PLL_BIAS			0x2214U
#define I915_XD_DKL_PLL_TDC_COLDST_BIAS		0x2218U
#define I915_XD_DKL_CMN_UC_DW27			0x236cU
#define I915_XD_DKL_UC_HEALTH			0x00008000U
#define I915_XD_DKL_PLL_DIV0_AFC_STARTUP	0x0e000000U
#define I915_XD_DKL_TX_PRESHOOT			0x0003e000U
#define I915_XD_DKL_TX_DE_EMPHASIS		0x00001f00U
#define I915_XD_DKL_TX_VSWING			0x00000007U
#define I915_XD_DKL_TX_DP20BITMODE		0x00000004U
#define I915_XD_DKL_TX_LOADGEN_TX1		0x00000018U
#define I915_XD_DKL_TX_LOADGEN_TX2		0x00000060U
#define I915_XD_DKL_TX_DISABLE_WAIT_INIT	0x00000080U

/*
 * The fields of the Type-C PLL words in a Dekel PHY: the reference clock
 * input mux, the core clock A divider, the high-speed clock control (the
 * core input select, the line driver clock select, the HS divider and the
 * DS divider), the PLL dividers (integral and proportional coefficients,
 * feedback pre-divider, integer feedback divider), the current reference
 * trim and the TDC target count, the SSC words (reference divider ratio,
 * step length and count, enable), the fractional feedback divider and its
 * enable, and the TDC SSC step size and feed-forward gain; and the codes
 * of the HS divider for the divisors 2, 3, 5 and 7.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (MG_REFCLKIN_CTL_OD_2_MUX_MASK, MG_CLKTOP2_CORECLKCTL1_A_DIVRATIO_MASK,
 * MG_CLKTOP2_HSCLKCTL_CORE_INPUTSEL_MASK,
 * MG_CLKTOP2_HSCLKCTL_TLINEDRV_CLKSEL_MASK,
 * MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_MASK, MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_2,
 * MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_3, MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_5,
 * MG_CLKTOP2_HSCLKCTL_HSDIV_RATIO_7, MG_CLKTOP2_HSCLKCTL_DSDIV_RATIO_MASK)
 * and Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dkl_phy_regs.h
 * (DKL_PLL_DIV0_INTEG_COEFF_MASK, DKL_PLL_DIV0_PROP_COEFF_MASK,
 * DKL_PLL_DIV0_FBPREDIV_MASK, DKL_PLL_DIV0_FBDIV_INT_MASK,
 * DKL_PLL_DIV1_IREF_TRIM_MASK, DKL_PLL_DIV1_TDC_TARGET_CNT_MASK,
 * DKL_PLL_SSC_IREF_NDIV_RATIO_MASK, DKL_PLL_SSC_STEP_LEN_MASK,
 * DKL_PLL_SSC_STEP_NUM_MASK, DKL_PLL_SSC_EN, DKL_PLL_BIAS_FRAC_EN_H,
 * DKL_PLL_BIAS_FBDIV_FRAC_MASK, DKL_PLL_TDC_SSC_STEP_SIZE_MASK,
 * DKL_PLL_TDC_FEED_FWD_GAIN_MASK).
 */
#define I915_XD_DKL_REFCLKIN_OD_2_MUX		0x00000700U
#define I915_XD_DKL_CORECLK_A_DIVRATIO		0x0000ff00U
#define I915_XD_DKL_HSCLK_CORE_INPUTSEL		0x00010000U
#define I915_XD_DKL_HSCLK_TLINEDRV		0x0000c000U
#define I915_XD_DKL_HSCLK_HSDIV			0x00003000U
#define I915_XD_DKL_HSCLK_HSDIV_2		0U
#define I915_XD_DKL_HSCLK_HSDIV_3		1U
#define I915_XD_DKL_HSCLK_HSDIV_5		2U
#define I915_XD_DKL_HSCLK_HSDIV_7		3U
#define I915_XD_DKL_HSCLK_DSDIV			0x00000f00U
#define I915_XD_DKL_DIV0_INTEG_COEFF		0x001f0000U
#define I915_XD_DKL_DIV0_PROP_COEFF		0x0000f000U
#define I915_XD_DKL_DIV0_FBPREDIV		0x00000f00U
#define I915_XD_DKL_DIV0_FBDIV_INT		0x000000ffU
#define I915_XD_DKL_DIV1_IREF_TRIM		0x001f0000U
#define I915_XD_DKL_DIV1_TDC_TARGET		0x000000ffU
#define I915_XD_DKL_SSC_IREF_NDIV		0xe0000000U
#define I915_XD_DKL_SSC_STEP_LEN		0x00ff0000U
#define I915_XD_DKL_SSC_STEP_NUM		0x00003800U
#define I915_XD_DKL_SSC_ENABLE			0x00000200U
#define I915_XD_DKL_BIAS_FRAC_ENABLE		0x40000000U
#define I915_XD_DKL_BIAS_FBDIV_FRAC		0x3fffff00U
#define I915_XD_DKL_TDC_SSC_STEP_SIZE		0x0000ff00U
#define I915_XD_DKL_TDC_FEED_FWD_GAIN		0x000000ffU

/*
 * The registers of an MG PHY, the Type-C PHY of display 11, reached
 * directly rather than through a window: Type-C port n's block is 0x1000
 * above port 0's, and lane 1's copy of a lane register is 0x400 above
 * lane 0's.  The lane registers of the two transmitters (TX1, TX2): the
 * link parameters (the FS32 use), the swing control (de-emphasis bits
 * 17..12), the drive control (de-emphasis bits 11..6 and 5..0 and their
 * override), the duty-cycle correction (the AMI clock divider override),
 * the PISO read load (the calculation init); the lane's clock hub (the
 * low-rate lock enable) and its DisplayPort mode word.  The PLL words:
 * the reference input, the clock tops, the dividers, the loop filter,
 * the fractional lock, the SSC, the bias and the TDC cold start bias.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_mg_phy_regs.h
 * (MG_PHY_PORT_LN, MG_TX_LINK_PARAMS_TX1LN0_PORT1,
 * MG_TX_LINK_PARAMS_TX2LN0_PORT1, CRI_USE_FS32,
 * MG_TX_PISO_READLOAD_TX1LN0_PORT1, MG_TX_PISO_READLOAD_TX2LN0_PORT1,
 * CRI_CALCINIT, MG_TX_SWINGCTRL_TX1LN0_PORT1,
 * MG_TX_SWINGCTRL_TX2LN0_PORT1, CRI_TXDEEMPH_OVERRIDE_17_12_MASK,
 * MG_TX_DRVCTRL_TX1LN0_TXPORT1, MG_TX_DRVCTRL_TX2LN0_PORT1,
 * CRI_TXDEEMPH_OVERRIDE_11_6_MASK, CRI_TXDEEMPH_OVERRIDE_EN,
 * CRI_TXDEEMPH_OVERRIDE_5_0_MASK, MG_CLKHUB_LN0_PORT1,
 * CFG_LOW_RATE_LKREN_EN, MG_TX_DCC_TX1LN0_PORT1, MG_TX_DCC_TX2LN0_PORT1,
 * CFG_AMI_CK_DIV_OVERRIDE_VAL_MASK, CFG_AMI_CK_DIV_OVERRIDE_EN,
 * MG_DP_MODE_LN0_ACU_PORT1, _MG_REFCLKIN_CTL_PORT1,
 * _MG_CLKTOP2_CORECLKCTL1_PORT1, _MG_CLKTOP2_HSCLKCTL_PORT1,
 * _MG_PLL_DIV0_PORT1, _MG_PLL_DIV1_PORT1, _MG_PLL_LF_PORT1,
 * _MG_PLL_FRAC_LOCK_PORT1, _MG_PLL_SSC_PORT1, _MG_PLL_BIAS_PORT1,
 * _MG_PLL_TDC_COLDST_BIAS_PORT1).
 */
#define I915_XD_MG_PHY(tc)			(0x168000U + 0x1000U * (tc))
#define I915_XD_MG_LANE_STRIDE			0x400U
#define I915_XD_MG_TX1_LINK_PARAMS		0x012cU
#define I915_XD_MG_TX2_LINK_PARAMS		0x00acU
#define I915_XD_MG_TX_USE_FS32			0x00000020U
#define I915_XD_MG_TX1_PISO_READLOAD		0x014cU
#define I915_XD_MG_TX2_PISO_READLOAD		0x00ccU
#define I915_XD_MG_TX_CALCINIT			0x00000002U
#define I915_XD_MG_TX1_SWINGCTRL		0x0148U
#define I915_XD_MG_TX2_SWINGCTRL		0x00c8U
#define I915_XD_MG_TX_DEEMPH_17_12		0x0000003fU
#define I915_XD_MG_TX1_DRVCTRL			0x0144U
#define I915_XD_MG_TX2_DRVCTRL			0x00c4U
#define I915_XD_MG_TX_DEEMPH_11_6		0x3f000000U
#define I915_XD_MG_TX_DEEMPH_OVERRIDE		0x00400000U
#define I915_XD_MG_TX_DEEMPH_5_0		0x003f0000U
#define I915_XD_MG_CLKHUB			0x039cU
#define I915_XD_MG_CLKHUB_LOW_RATE_LKREN	0x00000800U
#define I915_XD_MG_TX1_DCC			0x0110U
#define I915_XD_MG_TX2_DCC			0x0090U
#define I915_XD_MG_DCC_AMI_DIV			0x06000000U
#define I915_XD_MG_DCC_AMI_DIV_OVERRIDE		0x01000000U
#define I915_XD_MG_DP_MODE			0x03a0U
#define I915_XD_MG_REFCLKIN_CTL			0x092cU
#define I915_XD_MG_CLKTOP2_HSCLKCTL		0x08d4U
#define I915_XD_MG_CLKTOP2_CORECLKCTL1		0x08d8U
#define I915_XD_MG_PLL_DIV0			0x0a00U
#define I915_XD_MG_PLL_DIV1			0x0a04U
#define I915_XD_MG_PLL_LF			0x0a08U
#define I915_XD_MG_PLL_FRAC_LOCK		0x0a0cU
#define I915_XD_MG_PLL_SSC			0x0a10U
#define I915_XD_MG_PLL_BIAS			0x0a14U
#define I915_XD_MG_PLL_TDC_COLDST_BIAS		0x0a18U

/*
 * The fields of the MG PLL words.  DIV0: the fraction enable, the 22-bit
 * fractional and the 8-bit integer feedback divider.  DIV1: the current
 * reference divider ratio, the dither divider of 2, the N divider ratio
 * and the feedback pre-divider.  The loop filter: the TDC target count,
 * the AFC counter select of 512, the gain control, the integral and the
 * proportional coefficient.  The fractional lock: the true and the early
 * lock criteria of 32, the lock threshold, the DCO dither enable, the
 * feed-forward calibration enable and gain.  The SSC: the enable, the
 * type, the step length and count, the FLL enable and the step size.
 * The bias: the bias GB select, the initial DCO amplitude, the bias
 * bonus, the bias calibration enable, the C trim, the reference RDAC and
 * the current reference trim.  The TDC cold start bias: the current
 * reference integer enable, the reference bias start pulse width, the
 * cold start, the TDC overcurrent correction enable and the TDC select.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_mg_phy_regs.h (MG_PLL_DIV0_FRACNEN_H,
 * MG_PLL_DIV0_FBDIV_FRAC_MASK, MG_PLL_DIV0_FBDIV_INT_MASK,
 * MG_PLL_DIV1_IREF_NDIVRATIO, MG_PLL_DIV1_DITHER_DIV_2,
 * MG_PLL_DIV1_NDIVRATIO, MG_PLL_DIV1_FBPREDIV_MASK,
 * MG_PLL_LF_TDCTARGETCNT, MG_PLL_LF_AFCCNTSEL_512, MG_PLL_LF_GAINCTRL,
 * MG_PLL_LF_INT_COEFF, MG_PLL_LF_PROP_COEFF,
 * MG_PLL_FRAC_LOCK_TRUELOCK_CRIT_32, MG_PLL_FRAC_LOCK_EARLYLOCK_CRIT_32,
 * MG_PLL_FRAC_LOCK_LOCKTHRESH, MG_PLL_FRAC_LOCK_DCODITHEREN,
 * MG_PLL_FRAC_LOCK_FEEDFWRDCAL_EN, MG_PLL_FRAC_LOCK_FEEDFWRDGAIN,
 * MG_PLL_SSC_EN, MG_PLL_SSC_TYPE, MG_PLL_SSC_STEPLENGTH,
 * MG_PLL_SSC_STEPNUM, MG_PLL_SSC_FLLEN, MG_PLL_SSC_STEPSIZE,
 * MG_PLL_BIAS_BIAS_GB_SEL, MG_PLL_BIAS_INIT_DCOAMP,
 * MG_PLL_BIAS_BIAS_BONUS, MG_PLL_BIAS_BIASCAL_EN, MG_PLL_BIAS_CTRIM,
 * MG_PLL_BIAS_VREF_RDAC, MG_PLL_BIAS_IREFTRIM,
 * MG_PLL_TDC_COLDST_IREFINT_EN, MG_PLL_TDC_COLDST_REFBIAS_START_PULSE_W,
 * MG_PLL_TDC_COLDST_COLDSTART, MG_PLL_TDC_TDCOVCCORR_EN,
 * MG_PLL_TDC_TDCSEL).
 */
#define I915_XD_MG_DIV0_FRAC_ENABLE		0x40000000U
#define I915_XD_MG_DIV0_FBDIV_FRAC		0x3fffff00U
#define I915_XD_MG_DIV0_FBDIV_INT		0x000000ffU
#define I915_XD_MG_DIV1_IREF_NDIV(x)		((x) << 16)
#define I915_XD_MG_DIV1_DITHER_DIV_2		0x00001000U
#define I915_XD_MG_DIV1_NDIV(x)			((x) << 4)
#define I915_XD_MG_DIV1_FBPREDIV		0x0000000fU
#define I915_XD_MG_LF_TDC_TARGET(x)		((x) << 24)
#define I915_XD_MG_LF_AFC_COUNT_512		0x00100000U
#define I915_XD_MG_LF_GAIN(x)			((x) << 16)
#define I915_XD_MG_LF_INT_COEFF(x)		((x) << 8)
#define I915_XD_MG_LF_PROP_COEFF(x)		((x) << 0)
#define I915_XD_MG_FRAC_LOCK_TRUELOCK_32	0x00040000U
#define I915_XD_MG_FRAC_LOCK_EARLYLOCK_32	0x00010000U
#define I915_XD_MG_FRAC_LOCK_THRESHOLD(x)	((x) << 11)
#define I915_XD_MG_FRAC_LOCK_DCO_DITHER		0x00000400U
#define I915_XD_MG_FRAC_LOCK_FEED_FWD_CAL	0x00000100U
#define I915_XD_MG_FRAC_LOCK_FEED_FWD_GAIN(x)	((x) << 0)
#define I915_XD_MG_SSC_ENABLE			0x10000000U
#define I915_XD_MG_SSC_TYPE(x)			((x) << 26)
#define I915_XD_MG_SSC_STEP_LENGTH(x)		((x) << 16)
#define I915_XD_MG_SSC_STEP_NUM(x)		((x) << 10)
#define I915_XD_MG_SSC_FLL_ENABLE		0x00000200U
#define I915_XD_MG_SSC_STEP_SIZE(x)		((x) << 0)
#define I915_XD_MG_BIAS_GB_SEL(x)		((x) << 30)
#define I915_XD_MG_BIAS_INIT_DCOAMP(x)		((x) << 24)
#define I915_XD_MG_BIAS_BONUS(x)		((x) << 16)
#define I915_XD_MG_BIAS_CAL_ENABLE		0x00008000U
#define I915_XD_MG_BIAS_CTRIM(x)		((x) << 8)
#define I915_XD_MG_BIAS_VREF_RDAC(x)		((x) << 5)
#define I915_XD_MG_BIAS_IREF_TRIM(x)		((x) << 0)
#define I915_XD_MG_TDC_IREFINT_ENABLE		0x08000000U
#define I915_XD_MG_TDC_REFBIAS_PULSE(x)		((x) << 17)
#define I915_XD_MG_TDC_COLDSTART		0x00010000U
#define I915_XD_MG_TDC_OVERCURRENT_CORR		0x00000004U
#define I915_XD_MG_TDC_SELECT(x)		((x) << 0)

/*
 * Display 11's transcoder differences: the eDP transcoder, whose
 * registers lie 0xF000 above transcoder A's (so the transcoder register
 * macros reach them with the index 15) and whose chicken word lies
 * apart; the DDI select of a transcoder's DDI function (the DDI index
 * itself, in bits 30..28); the eDP input of the eDP transcoder's DDI
 * function (pipe A always on, pipe B or C switched with the panel); and
 * the transcoder clock select (the DDI index plus one, in bits 31..29).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_display_device.c
 * (TRANSCODER_A_OFFSET, TRANSCODER_EDP_OFFSET, ICL_DISPLAY), and
 * Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h (_CHICKEN_TRANS_EDP,
 * TRANS_DDI_PORT_MASK, TRANS_DDI_SELECT_PORT, TRANS_DDI_EDP_INPUT_A_ON,
 * TRANS_DDI_EDP_INPUT_B_ONOFF, TRANS_DDI_EDP_INPUT_C_ONOFF,
 * TRANS_CLK_SEL_PORT).
 */
#define I915_XD_TRANS_EDP			15U
#define I915_XD_CHICKEN_TRANS_EDP		0x420ccU
#define I915_XD_DDI_FUNC_SELECT_11		0x70000000U
#define I915_XD_DDI_FUNC_EDP_INPUT_A_ON		0U
#define I915_XD_DDI_FUNC_EDP_INPUT_B_ONOFF	5U
#define I915_XD_DDI_FUNC_EDP_INPUT_C_ONOFF	6U
#define I915_XD_TRANS_CLK_SEL_FIELD_11		0xe0000000U

/*
 * The transcoder's chicken word: the vblank with the delay of the set
 * context latency (display 12 and 13), the frame start delay (lines less
 * one), and the FEC stall release of a stopping transcoder.  Transcoder D
 * lies apart from A to C.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_CHICKEN_TRANS_A, _CHICKEN_TRANS_B, _CHICKEN_TRANS_C,
 * _CHICKEN_TRANS_D, PIPE_VBLANK_WITH_DELAY, HSW_FRAME_START_DELAY_MASK,
 * FECSTALL_DIS_DPTSTREAM_DPTTG).
 */
#define I915_XD_CHICKEN_TRANS(trans)		((trans) == 3U ? 0x420d8U : 0x420c0U + 4U * (trans))
#define I915_XD_CHICKEN_TRANS_VBLANK_DELAY	0x80000000U
#define I915_XD_CHICKEN_TRANS_FRAME_START	0x18000000U
#define I915_XD_CHICKEN_TRANS_FEC_STALL		0x00800000U

/*
 * The transcoder's pixel multiplier (less one), its variable refresh
 * words (control: the ignored maximum shift, the flip line enable, the
 * pipeline full lines and their override on display 12, the guardband on
 * display 13; the vertical maximum, minimum and the flip line, each less
 * one), and the interlace field of the transcoder configuration
 * (progressive, or interlaced with the field indication).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_TRANS_MULT_A, _TRANS_VRR_CTL_A, VRR_CTL_IGN_MAX_SHIFT,
 * VRR_CTL_FLIP_LINE_EN, VRR_CTL_PIPELINE_FULL_MASK,
 * VRR_CTL_PIPELINE_FULL_OVERRIDE, XELPD_VRR_CTL_VRR_GUARDBAND_MASK,
 * _TRANS_VRR_VMAX_A, _TRANS_VRR_VMIN_A, _TRANS_VRR_FLIPLINE_A,
 * TRANSCONF_INTERLACE_MASK_ILK, TRANSCONF_INTERLACE_IF_ID_ILK).
 */
#define I915_XD_TRANS_MULT(trans)		(0x6002cU + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_VRR_CTL(trans)		(0x60420U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_VRR_IGNORE_MAX_SHIFT		0x40000000U
#define I915_XD_VRR_FLIP_LINE			0x20000000U
#define I915_XD_VRR_PIPELINE_FULL		0x000007f8U
#define I915_XD_VRR_PIPELINE_FULL_OVERRIDE	0x00000001U
#define I915_XD_VRR_GUARDBAND			0x0000ffffU
#define I915_XD_TRANS_VRR_VMAX(trans)		(0x60424U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_VRR_VMIN(trans)		(0x60434U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_VRR_FLIPLINE(trans)	(0x60438U + I915_XD_TRANS_STRIDE * (trans))
#define I915_XD_TRANS_CONF_INTERLACE		0x00e00000U
#define I915_XD_TRANS_CONF_INTERLACED		3U

/*
 * The rest of the pipe words the transcoder's setup writes: the pipe
 * chicken word's per-pixel alpha bypass and frame buffer pass-through
 * rounding; the miscellaneous word's YUV 4:2:0 (and its full blend), the
 * HDR precision and the YUV output.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (PER_PIXEL_ALPHA_BYPASS_EN, PIXEL_ROUNDING_TRUNC_FB_PASSTHRU,
 * PIPE_MISC_YUV420_ENABLE, PIPE_MISC_YUV420_MODE_FULL_BLEND,
 * PIPE_MISC_HDR_MODE_PRECISION, PIPE_MISC_OUTPUT_COLORSPACE_YUV).
 */
#define I915_XD_PIPE_CHICKEN_ALPHA_BYPASS	0x00000080U
#define I915_XD_PIPE_CHICKEN_FB_PASSTHRU	0x00008000U
#define I915_XD_PIPE_MISC_YUV420		0x08000000U
#define I915_XD_PIPE_MISC_YUV420_FULL_BLEND	0x04000000U
#define I915_XD_PIPE_MISC_HDR_PRECISION		0x00800000U
#define I915_XD_PIPE_MISC_OUTPUT_YUV		0x00000800U

/*
 * The rest of the transcoder's DDI function words: the multi-stream
 * mode, the multi-stream transport's master transcoder, HDCP signalling,
 * the HDMI high TMDS character rate and scrambling; the second word's
 * port sync enable and master select.  The MSA word's colour depth codes
 * of 10 and 12 bits, and its colour formats (CEA RGB, YCbCr 4:4:4 BT.709,
 * and the one that points at the VSC SDP).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (TRANS_DDI_MODE_SELECT_DP_MST, TRANS_DDI_MST_TRANSPORT_SELECT_MASK,
 * TRANS_DDI_HDCP_SIGNALLING, TRANS_DDI_HIGH_TMDS_CHAR_RATE,
 * TRANS_DDI_HDMI_SCRAMBLING, PORT_SYNC_MODE_ENABLE,
 * PORT_SYNC_MODE_MASTER_SELECT_MASK), and Linux v6.8.12
 * include/drm/display/drm_dp.h (DP_MSA_MISC_10_BPC, DP_MSA_MISC_12_BPC,
 * DP_MSA_MISC_COLOR_CEA_RGB, DP_MSA_MISC_COLOR_YCBCR_444_BT709,
 * DP_MSA_MISC_COLOR_VSC_SDP).
 */
#define I915_XD_DDI_FUNC_MODE_DP_MST		3U
#define I915_XD_DDI_FUNC_MST_MASTER		0x00000c00U
#define I915_XD_DDI_FUNC_HDCP			0x00000200U
#define I915_XD_DDI_FUNC_HIGH_TMDS		0x00000010U
#define I915_XD_DDI_FUNC_SCRAMBLING		0x00000001U
#define I915_XD_DDI_FUNC2_PORT_SYNC		0x00000010U
#define I915_XD_DDI_FUNC2_PORT_SYNC_MASTER	0x00000007U
#define I915_XD_MSA_BPC_10			0x00000040U
#define I915_XD_MSA_BPC_12			0x00000060U
#define I915_XD_MSA_CEA_RGB			0x00000008U
#define I915_XD_MSA_YCBCR_444_BT709		0x0000001cU
#define I915_XD_MSA_VSC_SDP			0x00004000U

/*
 * The rest of the first universal plane's words: the colour key's value,
 * mask (with the constant alpha enable) and maximum (with the plane
 * alpha in the top byte), the distance to an auxiliary surface, the
 * chroma upsampler control, and the colour control's plane gamma off.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (_PLANE_KEYVAL_1_A, _PLANE_KEYMSK_1_A, PLANE_KEYMSK_ALPHA_ENABLE,
 * _PLANE_KEYMAX_1_A, PLANE_KEYMAX_ALPHA_MASK, _PLANE_AUX_DIST_1_A,
 * _PLANE_CUS_CTL_1_A, PLANE_COLOR_PLANE_GAMMA_DISABLE).
 */
#define I915_XD_PLANE_KEYVAL(pipe)		(0x70194U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_KEYMSK(pipe)		(0x70198U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_KEYMSK_ALPHA		0x80000000U
#define I915_XD_PLANE_KEYMAX(pipe)		(0x701a0U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_KEYMAX_ALPHA		0xff000000U
#define I915_XD_PLANE_AUX_DIST(pipe)		(0x701c0U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_CUS_CTL(pipe)		(0x701c8U + I915_XD_PIPE_STRIDE * (pipe))
#define I915_XD_PLANE_COLOR_GAMMA_OFF		0x00002000U

#endif /* DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD_H */
