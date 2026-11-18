/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The south display registers the xd display code (display/xd-*.c) uses
 * beside a display 12 or 13 GPU: those of the PCH generations ICP, TGP
 * and ADP, which share one layout.  The south display holds the DDC
 * controller (GMBUS), the panel power sequencer, the backlight PWM, the
 * hotplug detection of the DDIs and Type-C ports, the raw clock and the
 * south interrupt.  The north display registers are in
 * display-regs-xd.h, which also explains the instance arguments.
 */

#ifndef DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD_SOUTH_H
#define DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD_SOUTH_H

/*
 * The south display interrupt words (ISR, IMR, IIR, IER), and their bits
 * for the DDI hotplug pins (A, B, C at 16..18), the Type-C hotplug pins
 * (TC1..TC6 at 24..29) and the GMBUS.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (SDEISR, SDEIMR, SDEIIR, SDEIER, SDE_DDI_HOTPLUG_ICP,
 * SDE_TC_HOTPLUG_ICP, SDE_GMBUS_ICP).
 */
#define I915_XD_SDE_ISR				0xc4000U
#define I915_XD_SDE_IMR				0xc4004U
#define I915_XD_SDE_IIR				0xc4008U
#define I915_XD_SDE_IER				0xc400cU
#define I915_XD_SDE_DDI_HPD(ddi)		(1U << (16U + (ddi)))
#define I915_XD_SDE_TC_HPD(tc)			(1U << (24U + (tc)))
#define I915_XD_SDE_GMBUS			0x00800000U

/*
 * The south hotplug controls of the DDI pins and the Type-C pins: a
 * nibble a pin with the enable, and the long and short detect status
 * (written back to clear); and the hotplug filter count.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (SHOTPLUG_CTL_DDI, SHOTPLUG_CTL_DDI_HPD_ENABLE,
 * SHOTPLUG_CTL_DDI_HPD_STATUS_MASK, SHOTPLUG_CTL_DDI_HPD_SHORT_DETECT,
 * SHOTPLUG_CTL_DDI_HPD_LONG_DETECT, SHOTPLUG_CTL_TC, ICP_TC_HPD_ENABLE,
 * ICP_TC_HPD_LONG_DETECT, ICP_TC_HPD_SHORT_DETECT, SHPD_FILTER_CNT,
 * SHPD_FILTER_CNT_500_ADJ, SHPD_FILTER_CNT_250).
 */
#define I915_XD_SHOTPLUG_CTL_DDI		0xc4030U
#define I915_XD_SHOTPLUG_CTL_TC			0xc4034U
#define I915_XD_SHOTPLUG_ENABLE(pin)		(0x8U << ((pin) * 4U))
#define I915_XD_SHOTPLUG_STATUS(pin)		(0x3U << ((pin) * 4U))
#define I915_XD_SHOTPLUG_LONG(pin)		(0x2U << ((pin) * 4U))
#define I915_XD_SHOTPLUG_SHORT(pin)		(0x1U << ((pin) * 4U))
#define I915_XD_SHPD_FILTER_CNT			0xc4038U
#define I915_XD_SHPD_FILTER_500US_ADJUSTED	0x000001d9U
#define I915_XD_SHPD_FILTER_250US		0x000000f8U

/*
 * The south chicken word (the second panel power sequencer's IO select)
 * and the south clock gating word (the DPMG and DPLS units' clock gates).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (SOUTH_CHICKEN1, ICP_SECOND_PPS_IO_SELECT, SOUTH_DSPCLK_GATE_D,
 * PCH_DPMGUNIT_CLOCK_GATE_DISABLE, PCH_DPLSUNIT_CLOCK_GATE_DISABLE).
 */
#define I915_XD_SOUTH_CHICKEN1			0xc2000U
#define I915_XD_SOUTH_CHICKEN1_SECOND_PPS_IO	0x00000004U
#define I915_XD_SOUTH_DSPCLK_GATE_D		0xc2020U
#define I915_XD_SOUTH_DSPCLK_DPMG_UNGATE	0x00008000U
#define I915_XD_SOUTH_DSPCLK_DPLS_UNGATE	0x20000000U

/*
 * The south chicken bit that lets the PCH stop the display's reference
 * clock while the display is in DC9.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (SBCLK_RUN_REFCLK_DIS).
 */
#define I915_XD_SOUTH_CHICKEN1_REFCLK_STOP	0x00000080U

/*
 * The fuse strap of the south display (bit 8 set: the raw clock is
 * 24 MHz, clear: 19.2 MHz), and the raw clock frequency word: the
 * integer MHz, the fraction's denominator less one and, from ICP on, its
 * numerator (from bit 11).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (SFUSE_STRAP, SFUSE_STRAP_RAW_FREQUENCY, PCH_RAWCLK_FREQ,
 * CNP_RAWCLK_DIV_MASK, CNP_RAWCLK_FRAC_MASK, ICP_RAWCLK_NUM).
 */
#define I915_XD_SFUSE_STRAP			0xc2014U
#define I915_XD_SFUSE_STRAP_RAW_24MHZ		0x00000100U
#define I915_XD_RAWCLK_FREQ			0xc6204U
#define I915_XD_RAWCLK_DIVIDER			0x03ff0000U
#define I915_XD_RAWCLK_DENOMINATOR		0x3c000000U
#define I915_XD_RAWCLK_NUMERATOR_SHIFT		11U

/*
 * The GMBUS controller: the pin (the low bits) and the rate (GMBUS0),
 * the command (GMBUS1: software clear, software ready, timeout enable,
 * the cycle, the byte count, the index and the target address with the
 * read bit),
 * the status (GMBUS2: in use, hardware wait phase, the stall timeout,
 * hardware ready, the NAK), the data (GMBUS3), the interrupt enables
 * (GMBUS4: the bus going idle, the wait phase, hardware ready) and the
 * two-byte index (GMBUS5).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/i915_reg.h
 * (PCH_DISPLAY_BASE), and Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus_regs.h (GMBUS0 to GMBUS5,
 * GMBUS_RATE_100KHZ, GMBUS_HOLD_EXT, GMBUS_SW_CLR_INT, GMBUS_SW_RDY,
 * GMBUS_ENT, GMBUS_CYCLE_NONE, GMBUS_CYCLE_WAIT, GMBUS_CYCLE_INDEX,
 * GMBUS_CYCLE_STOP, GMBUS_BYTE_COUNT_SHIFT, GEN9_GMBUS_BYTE_COUNT_MAX,
 * GMBUS_SLAVE_INDEX_SHIFT, GMBUS_SLAVE_ADDR_SHIFT, GMBUS_SLAVE_READ,
 * GMBUS_INUSE, GMBUS_HW_WAIT_PHASE, GMBUS_STALL_TIMEOUT, GMBUS_HW_RDY,
 * GMBUS_SATOER, GMBUS_ACTIVE, GMBUS_IDLE_EN, GMBUS_HW_WAIT_EN,
 * GMBUS_HW_RDY_EN, GMBUS_2BYTE_INDEX_EN).
 */
#define I915_XD_GMBUS0				0xc5100U
#define I915_XD_GMBUS0_RATE_100KHZ		0x00000000U
#define I915_XD_GMBUS0_HOLD_EXTENDED		0x00000080U
#define I915_XD_GMBUS1				0xc5104U
#define I915_XD_GMBUS1_SW_CLEAR			0x80000000U
#define I915_XD_GMBUS1_SW_READY			0x40000000U
#define I915_XD_GMBUS1_TIMEOUT_ENABLE		0x20000000U
#define I915_XD_GMBUS1_CYCLE			0x0e000000U
#define I915_XD_GMBUS1_CYCLE_NONE		0U
#define I915_XD_GMBUS1_CYCLE_WAIT		1U
#define I915_XD_GMBUS1_CYCLE_INDEX		2U
#define I915_XD_GMBUS1_CYCLE_STOP		4U
#define I915_XD_GMBUS1_BYTE_COUNT		0x01ff0000U
#define I915_XD_GMBUS1_BYTE_COUNT_MAX		511U
#define I915_XD_GMBUS1_INDEX			0x0000ff00U
#define I915_XD_GMBUS1_ADDRESS			0x000000feU
#define I915_XD_GMBUS1_READ			0x00000001U
#define I915_XD_GMBUS2				0xc5108U
#define I915_XD_GMBUS2_IN_USE			0x00008000U
#define I915_XD_GMBUS2_WAIT_PHASE		0x00004000U
#define I915_XD_GMBUS2_STALL_TIMEOUT		0x00002000U
#define I915_XD_GMBUS2_HW_READY			0x00000800U
#define I915_XD_GMBUS2_NAK			0x00000400U
#define I915_XD_GMBUS2_ACTIVE			0x00000200U
#define I915_XD_GMBUS3				0xc510cU
#define I915_XD_GMBUS4				0xc5110U
#define I915_XD_GMBUS4_IDLE			0x00000004U
#define I915_XD_GMBUS4_WAIT_PHASE		0x00000002U
#define I915_XD_GMBUS4_HW_READY			0x00000001U
#define I915_XD_GMBUS5				0xc5120U
#define I915_XD_GMBUS5_TWO_BYTE_INDEX		0x80000000U

/*
 * The GMBUS pins of the ICP, TGP and ADP south displays: the DDC pins of
 * DDI A, B and C, and of the Type-C ports TC1..TC6 (TC5 and TC6 from TGP
 * on).
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_gmbus.h (GMBUS_PIN_1_BXT,
 * GMBUS_PIN_2_BXT, GMBUS_PIN_3_BXT, GMBUS_PIN_9_TC1_ICP to
 * GMBUS_PIN_14_TC6_TGP), and drivers/gpu/drm/i915/display/intel_gmbus.c
 * (gmbus_pins_icp).
 */
#define I915_XD_GMBUS_PIN_NONE			0U
#define I915_XD_GMBUS_PIN_DDI(ddi)		(1U + (ddi))
#define I915_XD_GMBUS_PIN_TC(tc)		(9U + (tc))

/*
 * The panel power sequencer of the PCH (the first of two; the second is
 * 0x100 above): status (on, the sequence in progress, the power cycle
 * delay), control (the unlock key, forced VDD, the backlight enable, the
 * reset and the power), the power-on and power-off delays (in 100 us
 * units, two 13-bit fields) and the power cycle delay (in 100 ms units
 * plus one; from the CNP south display on it is a field of the control
 * word, before that a register of its own).
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps_regs.h (PCH_PPS_BASE,
 * _PP_STATUS, PP_ON, PP_SEQUENCE_MASK, PP_SEQUENCE_NONE,
 * PP_SEQUENCE_POWER_UP, PP_SEQUENCE_POWER_DOWN, PP_CYCLE_DELAY_ACTIVE,
 * PP_SEQUENCE_STATE_MASK, PP_SEQUENCE_STATE_OFF_IDLE,
 * PP_SEQUENCE_STATE_ON_IDLE, _PP_CONTROL, PANEL_UNLOCK_MASK,
 * PANEL_UNLOCK_REGS, BXT_POWER_CYCLE_DELAY_MASK, EDP_FORCE_VDD,
 * EDP_BLC_ENABLE, PANEL_POWER_RESET, PANEL_POWER_ON, _PP_ON_DELAYS,
 * PANEL_POWER_UP_DELAY_MASK, PANEL_LIGHT_ON_DELAY_MASK, _PP_OFF_DELAYS,
 * PANEL_POWER_DOWN_DELAY_MASK, PANEL_LIGHT_OFF_DELAY_MASK, _PP_DIVISOR,
 * PANEL_POWER_CYCLE_DELAY_MASK).
 */
#define I915_XD_PP_STATUS(pps)			(0xc7200U + 0x100U * (pps))
#define I915_XD_PP_STATUS_ON			0x80000000U
#define I915_XD_PP_STATUS_SEQUENCE		0x30000000U
#define I915_XD_PP_STATUS_SEQUENCE_NONE		0U
#define I915_XD_PP_STATUS_SEQUENCE_UP		1U
#define I915_XD_PP_STATUS_SEQUENCE_DOWN		2U
#define I915_XD_PP_STATUS_CYCLE_DELAY		0x08000000U
#define I915_XD_PP_STATUS_STATE			0x0000000fU
#define I915_XD_PP_STATUS_STATE_OFF_IDLE	0x0U
#define I915_XD_PP_STATUS_STATE_ON_IDLE		0x8U
#define I915_XD_PP_CONTROL(pps)			(0xc7204U + 0x100U * (pps))
#define I915_XD_PP_CONTROL_UNLOCK		0xabcd0000U
#define I915_XD_PP_CONTROL_UNLOCK_FIELD		0xffff0000U
#define I915_XD_PP_CONTROL_CYCLE		0x000001f0U
#define I915_XD_PP_CONTROL_FORCE_VDD		0x00000008U
#define I915_XD_PP_CONTROL_BACKLIGHT		0x00000004U
#define I915_XD_PP_CONTROL_RESET		0x00000002U
#define I915_XD_PP_CONTROL_POWER_ON		0x00000001U
#define I915_XD_PP_ON_DELAYS(pps)		(0xc7208U + 0x100U * (pps))
#define I915_XD_PP_OFF_DELAYS(pps)		(0xc720cU + 0x100U * (pps))
#define I915_XD_PP_DELAY_UPPER			0x1fff0000U
#define I915_XD_PP_DELAY_LOWER			0x00001fffU
#define I915_XD_PP_DIVISOR(pps)			(0xc7210U + 0x100U * (pps))
#define I915_XD_PP_DIVISOR_CYCLE		0x0000001fU

/*
 * The backlight PWM of the PCH (two controllers, 0x100 apart): control
 * (enable, polarity), the period and the duty cycle, both in raw clock
 * cycles.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_backlight_regs.h (_BXT_BLC_PWM_CTL1,
 * _BXT_BLC_PWM_FREQ1, _BXT_BLC_PWM_DUTY1, _BXT_BLC_PWM_CTL2,
 * BXT_BLC_PWM_ENABLE, BXT_BLC_PWM_POLARITY).
 */
#define I915_XD_SBLC_PWM_CTL(controller)	(0xc8250U + 0x100U * (controller))
#define I915_XD_SBLC_PWM_ENABLE			0x80000000U
#define I915_XD_SBLC_PWM_POLARITY		0x20000000U
#define I915_XD_SBLC_PWM_FREQ(controller)	(0xc8254U + 0x100U * (controller))
#define I915_XD_SBLC_PWM_DUTY(controller)	(0xc8258U + 0x100U * (controller))

#endif /* DRIVERS_GPU_I915_INTEL_DISPLAY_REGS_XD_SOUTH_H */
