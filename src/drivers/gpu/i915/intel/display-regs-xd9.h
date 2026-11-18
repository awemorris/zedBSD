/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display 9 registers (Skylake, Kaby Lake, Coffee Lake and Comet
 * Lake) the xd display code reads and writes, with their fields.
 *
 * Display 9 shares many of the later registers' places; what is here is
 * either only on display 9 or laid out differently there.
 */

#ifndef DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD9_H
#define DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD9_H

/*
 * The CD clock's control and DPLL 0's control (CDCLK_CTL, LCPLL1_CTL):
 * the frequency select in CDCLK_CTL 27:26, DPLL 0's enable and lock.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (CDCLK_CTL, LCPLL1_CTL).
 */
#define I915_XD9_CDCLK_CTL			0x46000U
#define I915_XD9_CDCLK_SELECT_SHIFT		26U
#define I915_XD9_CDCLK_SELECT_MASK		0x3U
#define I915_XD9_LCPLL1_CTL			0x46010U
#define I915_XD9_LCPLL_ENABLE			(1U << 31)
#define I915_XD9_LCPLL_LOCK			(1U << 30)

/*
 * The DPLL controls (DPLL_CTRL1, DPLL_CTRL2).
 *
 * DPLL_CTRL1 has six bits for each of the DPLLs 0 to 3: the override that
 * lets the field rule the DPLL (bit 0), the link rate (bits 3:1) and the
 * HDMI mode (bit 5).  DPLL_CTRL2 has three bits for each DDI: the
 * override (bit 0) and the DPLL that clocks the DDI (bits 2:1), and a
 * clock off bit for each DDI from bit 15.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DPLL_CTRL1, DPLL_CTRL2).
 */
#define I915_XD9_DPLL_CTRL1			0x6c058U
#define I915_XD9_DPLL_CTRL1_BITS		6U
#define I915_XD9_DPLL_CTRL1_OVERRIDE		(1U << 0)
#define I915_XD9_DPLL_CTRL1_RATE_SHIFT		1U
#define I915_XD9_DPLL_CTRL1_RATE_MASK		0x7U
#define I915_XD9_DPLL_CTRL1_HDMI		(1U << 5)
#define I915_XD9_DPLL_CTRL2			0x6c05cU
#define I915_XD9_DPLL_CTRL2_BITS		3U
#define I915_XD9_DPLL_CTRL2_OVERRIDE		(1U << 0)
#define I915_XD9_DPLL_CTRL2_SELECT_SHIFT	1U
#define I915_XD9_DPLL_CTRL2_SELECT_MASK		0x3U
#define I915_XD9_DPLL_CTRL2_CLOCK_OFF(ddi)	(1U << ((ddi) + 15U))

/*
 * The DPLLs' enables (DPLL 0 is LCPLL1, DPLL 1 LCPLL2, DPLLs 2 and 3 the
 * WRPLLs), their configuration words in HDMI mode, and their locks.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (LCPLL1_CTL, LCPLL2_CTL, WRPLL_CTL, DPLL_CFGCR1, DPLL_CFGCR2,
 * DPLL_STATUS).
 */
#define I915_XD9_DPLL0_CTL			0x46010U
#define I915_XD9_DPLL1_CTL			0x46014U
#define I915_XD9_DPLL2_CTL			0x46040U
#define I915_XD9_DPLL3_CTL			0x46060U
#define I915_XD9_DPLL_ENABLE			(1U << 31)
#define I915_XD9_DPLL1_CFGCR1			0x6c040U
#define I915_XD9_DPLL2_CFGCR1			0x6c048U
#define I915_XD9_DPLL3_CFGCR1			0x6c050U
#define I915_XD9_DPLL_STATUS			0x6c060U
#define I915_XD9_DPLL_LOCK(dpll)		(1U << ((dpll) * 8U))

/*
 * The link rates of DPLL_CTRL1's rate field, named by the link clock in
 * MHz (half the port's bit rate in 10 Mbit/s): 2700, 1350, 810, 1620,
 * 1080 and 2160.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DPLL_CTRL1, Link Rate).
 */
#define I915_XD9_LINK_RATE_2700			0U
#define I915_XD9_LINK_RATE_1350			1U
#define I915_XD9_LINK_RATE_810			2U
#define I915_XD9_LINK_RATE_1620			3U
#define I915_XD9_LINK_RATE_1080			4U
#define I915_XD9_LINK_RATE_2160			5U

/*
 * The configuration words of a DPLL in HDMI mode (DPLL_CFGCR1, DPLL_CFGCR2):
 * the frequency enable, the DCO fraction (24:9, in 1/0x8000 of the
 * reference) and integer (8:0, in reference clocks); the Q divider ratio
 * (15:8) and mode (7), the K divider (6:5), the P divider (4:2) and the
 * central frequency (1:0).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DPLL_CFGCR1, DPLL_CFGCR2).
 */
#define I915_XD9_CFGCR1_FREQ_ENABLE		(1U << 31)
#define I915_XD9_CFGCR1_FRACTION_SHIFT		9U
#define I915_XD9_CFGCR2_QDIV_RATIO_SHIFT	8U
#define I915_XD9_CFGCR2_QDIV_MODE		(1U << 7)
#define I915_XD9_CFGCR2_KDIV_SHIFT		5U
#define I915_XD9_CFGCR2_PDIV_SHIFT		2U
#define I915_XD9_CFGCR2_CENTRAL_9600		0U
#define I915_XD9_CFGCR2_CENTRAL_9000		1U
#define I915_XD9_CFGCR2_CENTRAL_8400		3U

/*
 * The GMBUS pins of the DDIs B, C and D on the Sunrise Point and Cannon
 * Point families, and the PCH's clock gating bit of the GMBUS unit (in
 * SOUTH_DSPCLK_GATE_D), which display 9 holds off while the GMBUS runs.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (GMBUS0 Pin Pair Select, SOUTH_DSPCLK_GATE_D); Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.c (Display WA #0868).
 */
#define I915_XD9_GMBUS_PIN_DDI_C		4U
#define I915_XD9_GMBUS_PIN_DDI_B		5U
#define I915_XD9_GMBUS_PIN_DDI_D		6U
#define I915_XD9_GMBUS_UNIT_UNGATE		(1U << 31)

/*
 * The driver's power well request register (HSW_PWR_WELL_CTL2) and the
 * state bit of power well 1, and the eDP transcoder's DDI function
 * control with its enable.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (PWR_WELL_CTL2, TRANS_DDI_FUNC_CTL_EDP).
 */
#define I915_XD9_PWR_WELL_CTL2			0x45404U
#define I915_XD9_PW1_STATE			(1U << 28)

/* The indexes of display 9's power wells in PWR_WELL_CTL2: DDI A to D's IO wells, power well 2. */
#define I915_XD9_WELL_DDI_IO(ddi)		(1U + (ddi))
#define I915_XD9_WELL_PW_2			15U
#define I915_XD9_TRANS_DDI_FUNC_CTL_EDP		0x6f400U
#define I915_XD9_TRANS_DDI_FUNC_ENABLE		(1U << 31)

/*
 * A display 9 DDI's buffer (DDI_BUF_CTL): the enable, the translation row
 * it drives (27:24), the lane reversal, the idle state and the width
 * (lanes less one in 3:1); its translation rows (DDI_BUF_TRANS, two words
 * a row); and its DisplayPort transport (DP_TP_CTL, DP_TP_STATUS): the
 * enable, enhanced framing, the training pattern (10:8) and the idle
 * patterns' end.  Each DDI's registers are 0x100 after the last one's,
 * its translation rows 0x60.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DDI_BUF_CTL, DDI_BUF_TRANS, DP_TP_CTL, DP_TP_STATUS).
 */
#define I915_XD9_DDI_BUF_CTL(ddi)		(0x64000U + 0x100U * (ddi))
#define I915_XD9_DDI_BUF_ENABLE			(1U << 31)
#define I915_XD9_DDI_BUF_ROW_SHIFT		24U
#define I915_XD9_DDI_BUF_ROW_MASK		(0xfU << 24)
#define I915_XD9_DDI_BUF_REVERSAL		(1U << 16)
#define I915_XD9_DDI_BUF_IDLE			(1U << 7)
#define I915_XD9_DDI_BUF_WIDTH_SHIFT		1U
#define I915_XD9_DDI_BUF_TRANS(ddi, row)	(0x64e00U + 0x60U * (ddi) + 8U * (row))
#define I915_XD9_DP_TP_CTL(ddi)			(0x64040U + 0x100U * (ddi))
#define I915_XD9_DP_TP_STATUS(ddi)		(0x64044U + 0x100U * (ddi))
#define I915_XD9_DP_TP_ENABLE			(1U << 31)
#define I915_XD9_DP_TP_ENHANCED			(1U << 18)
#define I915_XD9_DP_TP_PATTERN_MASK		(7U << 8)
#define I915_XD9_DP_TP_PATTERN_1		(0U << 8)
#define I915_XD9_DP_TP_PATTERN_2		(1U << 8)
#define I915_XD9_DP_TP_PATTERN_IDLE		(2U << 8)
#define I915_XD9_DP_TP_PATTERN_NORMAL		(3U << 8)
#define I915_XD9_DP_TP_PATTERN_3		(4U << 8)
#define I915_XD9_DP_TP_STATUS_IDLE_DONE		(1U << 25)

/*
 * The current boost of the DDIs' buffers (DISPIO_CR_TX_BMU_CR0): each
 * DDI's balance leg (three bits from 8 + 3 x DDI) and its disable bit
 * (23 + DDI).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DISPIO_CR_TX_BMU_CR0).
 */
#define I915_XD9_BMU_CR0			0x6c00cU
#define I915_XD9_BALANCE_LEG_SHIFT(ddi)		(8U + 3U * (ddi))
#define I915_XD9_BALANCE_LEG_MASK(ddi)		(7U << (8U + 3U * (ddi)))
#define I915_XD9_BALANCE_LEG_DISABLE(ddi)	(1U << (23U + (ddi)))

/*
 * A display 9 transcoder and pipe (transcoders A to C have the number of
 * their pipe, each 0x1000 after the last; the eDP transcoder, 3, is
 * 0xf000 after A's): the pixel multiplier, the
 * configuration with its enable and state, the DDI function (the enable,
 * the DDI in 30:28, the mode in 26:24, 8 bits a colour, the sync
 * polarities, the DisplayPort lanes less one in 3:1), the pipe's colour
 * depth word, and the frame counter.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (TRANS_MULT, TRANSCONF, TRANS_DDI_FUNC_CTL, PIPE_MISC,
 * PIPE_FRMCNT).
 */
#define I915_XD9_TRANS_EDP			3U
#define I915_XD9_TRANS_OFFSET(trans)		((trans) == I915_XD9_TRANS_EDP ? 0xf000U : 0x1000U * (trans))
#define I915_XD9_TRANS_MULT(trans)		(0x6002cU + I915_XD9_TRANS_OFFSET(trans))
#define I915_XD9_TRANSCONF(trans)		(0x70008U + I915_XD9_TRANS_OFFSET(trans))
#define I915_XD9_TRANSCONF_ENABLE		(1U << 31)
#define I915_XD9_TRANSCONF_STATE		(1U << 30)
#define I915_XD9_TRANS_DDI_FUNC(trans)		(0x60400U + I915_XD9_TRANS_OFFSET(trans))
#define I915_XD9_DDI_FUNC_ENABLE		(1U << 31)
#define I915_XD9_DDI_FUNC_DDI_SHIFT		28U
#define I915_XD9_DDI_FUNC_DDI_MASK		(7U << 28)
#define I915_XD9_DDI_FUNC_MODE_SHIFT		24U
#define I915_XD9_DDI_FUNC_MODE_HDMI		0U
#define I915_XD9_DDI_FUNC_MODE_DP_SST		2U
#define I915_XD9_DDI_FUNC_BPC_8			(0U << 20)
#define I915_XD9_DDI_FUNC_PVSYNC		(1U << 17)
#define I915_XD9_DDI_FUNC_PHSYNC		(1U << 16)
#define I915_XD9_DDI_FUNC_WIDTH_SHIFT		1U
#define I915_XD9_PIPE_MISC(pipe)		(0x70030U + 0x1000U * (pipe))
#define I915_XD9_PIPE_MISC_BPC_8		(0U << 5)
#define I915_XD9_PIPE_FRMCOUNT(pipe)		(0x70040U + 0x1000U * (pipe))

/*
 * A display 9 pipe's first plane (each pipe's 0x1000 after the last):
 * the control (the enable, the format in 27:24 with XRGB8888 4, the R G B
 * X order, the plane gamma off), the stride in 64-byte units, the
 * position, the size (height less one in 31:16), the offset and the
 * surface, whose write arms the others.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (PLANE_CTL, PLANE_STRIDE, PLANE_POS, PLANE_SIZE, PLANE_OFFSET,
 * PLANE_SURF).
 */
#define I915_XD9_PLANE_CTL(pipe)		(0x70180U + 0x1000U * (pipe))
#define I915_XD9_PLANE_STRIDE(pipe)		(0x70188U + 0x1000U * (pipe))
#define I915_XD9_PLANE_POS(pipe)		(0x7018cU + 0x1000U * (pipe))
#define I915_XD9_PLANE_SIZE(pipe)		(0x70190U + 0x1000U * (pipe))
#define I915_XD9_PLANE_SURF(pipe)		(0x7019cU + 0x1000U * (pipe))
#define I915_XD9_PLANE_OFFSET(pipe)		(0x701a4U + 0x1000U * (pipe))
#define I915_XD9_PLANE_CTL_ENABLE		(1U << 31)
#define I915_XD9_PLANE_CTL_XRGB_8888		(4U << 24)
#define I915_XD9_PLANE_CTL_RGBX			(1U << 20)
#define I915_XD9_PLANE_CTL_GAMMA_OFF		(1U << 13)
#define I915_XD9_PLANE_SIZE_HEIGHT_SHIFT	16U

/*
 * The training override of a DDI's transcoder chicken register
 * (CHICKEN_TRANS of transcoders A to C, 4 apart), toggled before an HDMI
 * buffer starts.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (CHICKEN_TRANS); Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_ddi.c (Display WA #1143).
 */
#define I915_XD9_CHICKEN_TRANS(trans)		(0x420c0U + 4U * (trans))
#define I915_XD9_TRAINING_OVERRIDE		((1U << 19) | (1U << 18))

/*
 * A display 9 pipe's planes 2 and 3 (0x100 after the first, each 0x100
 * apart: the control, the surface and the buffer range), the first
 * plane's NV12 buffer range, and the cursor's base, whose writes arm the
 * planes' and the cursor's watermarks and ranges, and the cursor's
 * transition watermark.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (PLANE_CTL, PLANE_SURF, PLANE_BUF_CFG, PLANE_NV12_BUF_CFG,
 * CURBASE, CUR_WM_TRANS).
 */
#define I915_XD9_PLANE_N_CTL(pipe, plane)	(0x70180U + 0x1000U * (pipe) + 0x100U * (plane))
#define I915_XD9_PLANE_N_SURF(pipe, plane)	(0x7019cU + 0x1000U * (pipe) + 0x100U * (plane))
#define I915_XD9_PLANE_N_BUF_CFG(pipe, plane)	(0x7027cU + 0x1000U * (pipe) + 0x100U * (plane))
#define I915_XD9_PLANE_NV12_BUF_CFG(pipe)	(0x70278U + 0x1000U * (pipe))
#define I915_XD9_CURBASE(pipe)			(0x70084U + 0x1000U * (pipe))
#define I915_XD9_CUR_WM_TRANS(pipe)		(0x70168U + 0x1000U * (pipe))
#define I915_XD9_WM_LINETIME_MASK		0x1ffU

/*
 * The south display's hotplug on the SPT, KBP, CNP and CMP PCHs: the live
 * state of DDIs B to D in SDEISR, their sense enables in PCH_PORT_HOTPLUG,
 * and the DDIs the board has in SFUSE_STRAP.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (SDEISR, PCH_PORT_HOTPLUG, SFUSE_STRAP).
 */
#define I915_XD9_SDE_HPD(ddi)			(1U << (20U + (ddi)))
#define I915_XD9_PORT_HOTPLUG			0xc4030U
#define I915_XD9_PORT_HOTPLUG_ENABLE(ddi)	(1U << (4U + 8U * ((ddi) - 1U)))
#define I915_XD9_SFUSE_DDI_DETECTED(ddi)	(1U << (3U - (ddi)))

/*
 * What the firmware's display is read from on display 9: power well 2's
 * state beside power well 1's, the DC states the DMC may enter, the frame
 * buffer compressor's control, the eDP transcoder's PSR control, a
 * transcoder's totals (the total less one in 31:16) and DisplayPort link
 * M and N (24 bits), the DDI function's mode (DisplayPort single or multi
 * stream) and the eDP transcoder's input pipe in 14:12, the pipe's source
 * size, the first plane's live surface, the plane control's format,
 * tiling, rotation and asynchronous flip, the stride's 64-byte units, a
 * linear surface's 256 KiB alignment, and each pipe's two scalers.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (PWR_WELL_CTL2, DC_STATE_EN, FBC_CTL, SRD_CTL, TRANS_HTOTAL,
 * TRANS_VTOTAL, TRANS_LINKM1, TRANS_LINKN1, TRANS_DDI_FUNC_CTL, PIPESRC,
 * PLANE_SURFLIVE, PLANE_CTL, PLANE_STRIDE, PLANE_SURF, PS_CTRL).
 */
#define I915_XD9_PW2_STATE			(1U << 30)
#define I915_XD9_DC_STATE_EN			0x45504U
#define I915_XD9_DC_STATE_MASK			0x3U
#define I915_XD9_FBC_CTL			0x43208U
#define I915_XD9_FBC_ENABLE			(1U << 31)
#define I915_XD9_PSR_CTL_EDP			0x6f800U
#define I915_XD9_PSR_ENABLE			(1U << 31)
#define I915_XD9_TRANS_HTOTAL(trans)		(0x60000U + I915_XD9_TRANS_OFFSET(trans))
#define I915_XD9_TRANS_VTOTAL(trans)		(0x6000cU + I915_XD9_TRANS_OFFSET(trans))
#define I915_XD9_TRANS_LINK_M1(trans)		(0x60040U + I915_XD9_TRANS_OFFSET(trans))
#define I915_XD9_TRANS_LINK_N1(trans)		(0x60044U + I915_XD9_TRANS_OFFSET(trans))
#define I915_XD9_TIMING_TOTAL_SHIFT		16U
#define I915_XD9_TIMING_FIELD_MASK		0xffffU
#define I915_XD9_LINK_MN_MASK			0x00ffffffU
#define I915_XD9_DDI_FUNC_MODE_MASK		0x7U
#define I915_XD9_DDI_FUNC_MODE_DP_MST		3U
#define I915_XD9_DDI_FUNC_EDP_INPUT_SHIFT	12U
#define I915_XD9_DDI_FUNC_EDP_INPUT_MASK	0x7U
#define I915_XD9_DDI_FUNC_EDP_INPUT_A_ON	0U
#define I915_XD9_DDI_FUNC_EDP_INPUT_A_ONOFF	4U
#define I915_XD9_DDI_FUNC_EDP_INPUT_B_ONOFF	5U
#define I915_XD9_DDI_FUNC_EDP_INPUT_C_ONOFF	6U
#define I915_XD9_PIPESRC(pipe)			(0x6001cU + 0x1000U * (pipe))
#define I915_XD9_PLANE_SURFLIVE(pipe)		(0x701acU + 0x1000U * (pipe))
#define I915_XD9_PLANE_SURF_ADDRESS		0xfffff000U
#define I915_XD9_PLANE_CTL_FORMAT_SHIFT		24U
#define I915_XD9_PLANE_CTL_FORMAT_MASK		0xfU
#define I915_XD9_PLANE_FORMAT_XRGB_8888		4U
#define I915_XD9_PLANE_CTL_TILED_SHIFT		10U
#define I915_XD9_PLANE_CTL_TILED_MASK		0x7U
#define I915_XD9_PLANE_TILED_LINEAR		0U
#define I915_XD9_PLANE_CTL_ROTATE_MASK		0x3U
#define I915_XD9_PLANE_CTL_ASYNC_FLIP		(1U << 9)
#define I915_XD9_PLANE_STRIDE_MASK		0xfffU
#define I915_XD9_PLANE_STRIDE_UNIT		64U
#define I915_XD9_PLANE_SIZE_FIELD_MASK		0xffffU
#define I915_XD9_LINEAR_ALIGNMENT		(256U * 1024U)
#define I915_XD9_PS_CTRL(pipe, scaler)		(0x68180U + 0x800U * (pipe) + 0x100U * (scaler))
#define I915_XD9_PS_ENABLE			(1U << 31)

/*
 * A display 9 pipe's interrupts (each pipe's 0x10 after the last): the
 * mask, the identity and the enable, with the first plane's flip done and
 * fault; and the pipe's bit in the master interrupt control (A in 16).
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DE_PIPE_IMR, DE_PIPE_IIR, DE_PIPE_IER, MASTER_INT_CTL).
 */
#define I915_XD9_DE_PIPE_IMR(pipe)		(0x44404U + 0x10U * (pipe))
#define I915_XD9_DE_PIPE_IIR(pipe)		(0x44408U + 0x10U * (pipe))
#define I915_XD9_DE_PIPE_IER(pipe)		(0x4440cU + 0x10U * (pipe))
#define I915_XD9_DE_PIPE_FLIP_DONE		(1U << 3)
#define I915_XD9_DE_PIPE_FAULT			(1U << 7)
#define I915_XD9_MASTER_DE_PIPE(pipe)		(1U << (16U + (pipe)))
#define I915_XD9_FLIP_IRQS			(I915_XD9_DE_PIPE_FLIP_DONE | I915_XD9_DE_PIPE_FAULT)

/*
 * The display arbiter's control 2, with IPC's enable (Kaby Lake and
 * later), which lengthens the watermark latencies.
 *
 * Values referenced: Intel Kaby Lake PRM Volume 2c, Command Reference:
 * Registers (DISP_ARB_CTL2).
 */
#define I915_XD9_DISP_ARB_CTL2			0x45004U
#define I915_XD9_IPC_ENABLE			(1U << 3)

#endif /* DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD9_H */
