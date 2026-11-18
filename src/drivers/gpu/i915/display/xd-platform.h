/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The platform descriptions of the xd display code (xd-platform.c):
 * display 11 to 13, and display 9 in a table of its own.
 *
 * A description (struct i915_xd_platform; its tables are in
 * intel/display-tables-xd.h) says, as data, everything about one
 * platform that the xd code needs: its ports and PHYs, its power
 * wells, PLLs, CD clock steps, display buffer rules, swing tables,
 * firmware and workarounds.  The code reads the fields; it never
 * branches on the platform's name.
 *
 * The choice is made once at the start from the GPU's PCI device id and
 * revision and from the PCH the display found.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_PLATFORM_H
#define DRIVERS_GPU_I915_DISPLAY_XD_PLATFORM_H

#include <stdint.h>

#include "display-internal.h"

/*
 * The kinds of PHY behind a port: a combo PHY, a Dekel Type-C PHY (display
 * 12 and 13, reached through a window), an MG Type-C PHY (display 11,
 * reached directly), the DDI's own buffer with no PHY of its own (display
 * 9), or none (the port is not driven).
 */
#define I915_XD_PHY_NONE		0U
#define I915_XD_PHY_COMBO		1U
#define I915_XD_PHY_DEKEL		2U
#define I915_XD_PHY_MG			3U
#define I915_XD_PHY_DDI			4U

/*
 * Where a port's hotplug pin is in the south display: none, a DDI pin
 * (A..C) or a Type-C pin (TC1..TC6).
 */
#define I915_XD_SOUTH_PIN_NONE		0U
#define I915_XD_SOUTH_PIN_DDI		1U
#define I915_XD_SOUTH_PIN_TC		2U

/*
 * How a platform's Type-C ports are connected and kept out of TC cold:
 * no Type-C flow (every port is a combo port), the FIA flow of display
 * 11 and 12 (the live state, ready and ownership in the FIA registers),
 * or the display 13 flow (TCSS status, the ownership bit in the DDI
 * buffer control); and the block of TC cold through a mailbox well
 * (Tiger Lake), through the AUX well of the port (display 13), or, for a
 * legacy connector only, through the AUX well of the port with the
 * firmware asked to leave TC cold while the well comes up (Ice Lake).
 */
#define I915_XD_TC_FLOW_NONE		0U
#define I915_XD_TC_FLOW_FIA		1U
#define I915_XD_TC_FLOW_TCSS		2U
#define I915_XD_TCCOLD_NONE		0U
#define I915_XD_TCCOLD_MAILBOX_WELL	1U
#define I915_XD_TCCOLD_AUX_WELL		2U
#define I915_XD_TCCOLD_AUX_WELL_EXIT	3U

/*
 * Where the FIA flow finds a Type-C port's FIA: FIA 1 says whether the
 * FIA is modular (Tiger Lake), or FIA 1 serves every port and is never
 * asked (Ice Lake).
 */
#define I915_XD_FIA_READ_MODULAR	0U
#define I915_XD_FIA_SINGLE		1U

/*
 * How a platform maps a DDI to a combo DPLL: in DPCLKA_CFGCR0 with the
 * two-bit select at twice the PHY (Tiger Lake and display 13), with the
 * Rocket Lake places of PHY D and of the clock-off bits, in the two
 * words of Alder Lake-S with three PHYs each, or in DPLL_CTRL2 with a
 * clock select and an override bit for each DDI (display 9).
 */
#define I915_XD_PLL_MAP_CFGCR0		0U
#define I915_XD_PLL_MAP_CFGCR0_RKL	1U
#define I915_XD_PLL_MAP_CFGCR_ADLS	2U
#define I915_XD_PLL_MAP_DPLL_CTRL2	3U

/*
 * The kinds of PLL a platform has: a combo DPLL, the Thunderbolt PLL,
 * and the PLL of one Type-C port.
 */
#define I915_XD_PLL_COMBO		1U
#define I915_XD_PLL_TBT			2U
#define I915_XD_PLL_TC			3U

/*
 * The control a power well is switched through: the main, AUX or DDI
 * power well control, the DC state (the well that keeps the display out
 * of DC5 and DC6), or a GT driver mailbox request (Tiger Lake's TC cold
 * block).
 */
#define I915_XD_WELL_CONTROL_MAIN	1U
#define I915_XD_WELL_CONTROL_AUX	2U
#define I915_XD_WELL_CONTROL_DDI	3U
#define I915_XD_WELL_CONTROL_DC_OFF	4U
#define I915_XD_WELL_CONTROL_MAILBOX	5U

/*
 * The facts a power well carries besides its control: the DMC (not the
 * driver) turns it off in DC states, it powers the VGA plane, it is the
 * Thunderbolt AUX well of a Type-C port, its AUX well needs a fixed
 * delay after enabling; and a well without fuses to wait for.
 */
#define I915_XD_WELL_DMC_OWNED		0x01U
#define I915_XD_WELL_VGA		0x02U
#define I915_XD_WELL_TBT_AUX		0x04U
#define I915_XD_WELL_FIXED_DELAY	0x08U
#define I915_XD_WELL_NO_FUSE		0xffU

/*
 * What a power well powers, as bits of a 64-bit set: the pipes A..D,
 * the transcoders A..D, the main link lanes and the IO of the DDIs 0..8,
 * the AUX channels 0..8 and their IO, the Thunderbolt AUX of Type-C
 * ports 0..5, the VGA plane, the audio, the exit from DC states and the
 * exit from TC cold.  A function needs every well whose set holds it.
 */
#define I915_XD_POWERS_PIPE(pipe)	(1ULL << (pipe))
#define I915_XD_POWERS_TRANS(trans)	(1ULL << (4U + (trans)))
#define I915_XD_POWERS_LANES(ddi)	(1ULL << (8U + (ddi)))
#define I915_XD_POWERS_DDI_IO(ddi)	(1ULL << (17U + (ddi)))
#define I915_XD_POWERS_AUX(aux)		(1ULL << (26U + (aux)))
#define I915_XD_POWERS_AUX_IO(aux)	(1ULL << (35U + (aux)))
#define I915_XD_POWERS_TBT_AUX(tc)	(1ULL << (44U + (tc)))
#define I915_XD_POWERS_VGA		(1ULL << 50U)
#define I915_XD_POWERS_AUDIO		(1ULL << 51U)
#define I915_XD_POWERS_DC_OFF		(1ULL << 52U)
#define I915_XD_POWERS_TC_COLD_OFF	(1ULL << 53U)

/*
 * The power class of a display 9 part, which chooses its DDI buffer
 * tables: the H and S parts, the U parts and the Y parts.  Display 11 to
 * 13 rows leave it at 0.
 */
#define I915_XD_POWER_CLASS_H		0U
#define I915_XD_POWER_CLASS_U		1U
#define I915_XD_POWER_CLASS_Y		2U

/*
 * The display steppings: the letter in the upper nibble (A = 0) and the
 * number in the lower; a revision the table does not know past its last
 * row is a future stepping.
 */
#define I915_XD_STEP_A0			0x00U
#define I915_XD_STEP_A2			0x02U
#define I915_XD_STEP_B0			0x10U
#define I915_XD_STEP_C0			0x20U
#define I915_XD_STEP_D0			0x30U
#define I915_XD_STEP_E0			0x40U
#define I915_XD_STEP_FUTURE		0xffU

/*
 * The memory types the bandwidth buddy masks are chosen by, and the two
 * older ones the firmware may still report (0 is a type not known).
 */
#define I915_XD_DRAM_DDR4		1U
#define I915_XD_DRAM_DDR5		2U
#define I915_XD_DRAM_LPDDR4		3U
#define I915_XD_DRAM_LPDDR5		4U
#define I915_XD_DRAM_DDR3		5U
#define I915_XD_DRAM_LPDDR3		6U

/*
 * The platform kinds, for the log only: the steps of the code read the
 * fields of the description, never the kind.
 */
#define I915_XD_KIND_TGL		1U
#define I915_XD_KIND_RKL		2U
#define I915_XD_KIND_ADLS		3U
#define I915_XD_KIND_ADLP		4U
#define I915_XD_KIND_ICL		5U
#define I915_XD_KIND_SKL		6U
#define I915_XD_KIND_KBL		7U
#define I915_XD_KIND_CFL		8U
#define I915_XD_KIND_CML		9U

/*
 * The workarounds a platform takes, as bits of its description (the
 * steps that need one read the bit): the Wa_1409767108 bandwidth buddy
 * masks (Alder Lake-S), the PW_1 FLR source chicken before enabling PW_1
 * (Wa_16013190616, display 13), the DPCE clock gating and the DDI clock
 * register access chicken of display 13, the pipe DMC clock gating
 * (Wa_16015201720, display 13), the main DMC CLK_MSEC and VBLANK_A event
 * handlers disabled (Tiger Lake) or the VBLANK_A one only (Alder Lake-S),
 * the programmable arbiter slots (Wa_22012358565, display 13), the
 * display 12 compression dummy pixel and clock request chickens
 * (Wa_1409120013, Wa_14013723622), the Wa_1409767108 buddy masks on
 * the first stepping only (Rocket Lake A0), the Dekel PHY load
 * generator select of display 13, the CD clock of at least the
 * pixel rate (Tiger Lake, which can underrun at half of it), and the
 * Dekel PHY's core reset released before a Type-C port's DDI buffer
 * starts in DisplayPort alternate or legacy mode (Alder Lake-P, whose
 * port may come from Thunderbolt mode), and the Ice Lake combo PHY AUX
 * wells: the AUX lane enable of the PHY's common lane with the well, and
 * the AUX analog override of a port that is not eDP (display WA #1178).
 */
#define I915_XD_WA_BUDDY_1409767108	0x0001U
#define I915_XD_WA_PW1_FLR_SOURCE	0x0002U
#define I915_XD_WA_DPCE_UNGATE		0x0004U
#define I915_XD_WA_PIPEDMC_UNGATE	0x0008U
#define I915_XD_WA_DMC_CLK_MSEC_VBLANK	0x0010U
#define I915_XD_WA_DMC_VBLANK_A		0x0020U
#define I915_XD_WA_ARB_PROG_SLOTS	0x0040U
#define I915_XD_WA_XE_D_CHICKENS	0x0080U
#define I915_XD_WA_BUDDY_1409767108_A0	0x0100U
#define I915_XD_WA_DKL_LOADGEN		0x0200U
#define I915_XD_WA_CDCLK_PIXEL_RATE	0x0400U
#define I915_XD_WA_TC_RESET_AFTER_TBT	0x0800U
#define I915_XD_WA_COMBO_AUX_LANE	0x1000U
#define I915_XD_WA_AUX_ANAOVRD_1178	0x2000U

/*
 * One row of a display 9 DDI buffer translation table: the two words of
 * the row's DDI_BUF_TRANS pair, and the current boost the row asks for
 * (0: the buffer's default).
 */
struct i915_xd9_buf_trans {
	uint32_t trans1;
	uint32_t trans2;
	uint8_t iboost;
};

/* A display 9 DDI buffer translation table: its rows. */
struct i915_xd9_buf_table {
	const struct i915_xd9_buf_trans *rows;
	unsigned count;
};

/*
 * The DDI buffer tables of a display 9 part: DisplayPort, eDP (the low
 * swing table) and HDMI, and the HDMI row a port uses when the VBT names
 * none.
 */
struct i915_xd9_ddi_buffers {
	struct i915_xd9_buf_table dp;
	struct i915_xd9_buf_table edp;
	struct i915_xd9_buf_table hdmi;
	unsigned hdmi_default;
};

/*
 * One row of a combo PHY swing table: the values of one voltage swing
 * and pre-emphasis level, as the transmit words DW2, DW7 and DW4 take
 * them.
 */
struct i915_xd_combo_swing {
	uint8_t swing_select;
	uint8_t n_scalar;
	uint8_t cursor_coeff;
	uint8_t post_cursor_1;
	uint8_t post_cursor_2;
};

/*
 * One row of a Dekel PHY swing table: the voltage swing control, the
 * de-emphasis and the preshoot coefficients of one level.
 */
struct i915_xd_dkl_swing {
	uint8_t vswing;
	uint8_t de_emphasis;
	uint8_t preshoot;
};

/*
 * A combo PHY swing table: its rows and the row an HDMI port uses when
 * the VBT names none (only an HDMI table has one; 0 otherwise).
 */
struct i915_xd_combo_swing_table {
	const struct i915_xd_combo_swing *rows;
	unsigned count;
	unsigned hdmi_default;
};

/* A Dekel PHY swing table, in the same form. */
struct i915_xd_dkl_swing_table {
	const struct i915_xd_dkl_swing *rows;
	unsigned count;
	unsigned hdmi_default;
};

/*
 * One row of an MG PHY swing table: the three parts of the transmit
 * de-emphasis override of one level (bits 17..12, 11..6 and 5..0).
 */
struct i915_xd_mg_swing {
	uint8_t deemph_17_12;
	uint8_t deemph_11_6;
	uint8_t deemph_5_0;
};

/* An MG PHY swing table, in the same form. */
struct i915_xd_mg_swing_table {
	const struct i915_xd_mg_swing *rows;
	unsigned count;
	unsigned hdmi_default;
};

/*
 * The swing tables a platform chooses from: for a combo port, DisplayPort
 * up to 2.7 GHz and above it, eDP with the VBT's low swing, eDP with the
 * VBT's high-efficiency (HOBL) setting (NULL where the platform has no
 * HOBL), eDP above 5.4 GHz, and HDMI; for a Dekel port, DisplayPort up to
 * 2.7 GHz and above it, and HDMI (NULL on a platform without Dekel
 * ports); for an MG port the same three (NULL without MG ports).
 */
struct i915_xd_swing_set {
	const struct i915_xd_combo_swing_table *dp_low;
	const struct i915_xd_combo_swing_table *dp_high;
	const struct i915_xd_combo_swing_table *edp_low_swing;
	const struct i915_xd_combo_swing_table *edp_hobl;
	const struct i915_xd_combo_swing_table *edp_hbr3;
	const struct i915_xd_combo_swing_table *hdmi;
	const struct i915_xd_dkl_swing_table *dkl_dp_low;
	const struct i915_xd_dkl_swing_table *dkl_dp_high;
	const struct i915_xd_dkl_swing_table *dkl_hdmi;
	const struct i915_xd_mg_swing_table *mg_dp_low;
	const struct i915_xd_mg_swing_table *mg_dp_high;
	const struct i915_xd_mg_swing_table *mg_hdmi;
};

/*
 * The combo DPLL words of one DisplayPort link rate: the DCO integer and
 * fraction (CFGCR0) and the dividers in their register encoding (CFGCR1).
 */
struct i915_xd_dpll_dp_row {
	uint32_t link_khz;
	uint16_t dco_integer;
	uint16_t dco_fraction;
	uint8_t qdiv_ratio;
	uint8_t qdiv_mode;
	uint8_t kdiv;
	uint8_t pdiv;
};

/* The DCO words of a PLL that runs at one fixed rate (the TBT PLL). */
struct i915_xd_dpll_dco {
	uint16_t dco_integer;
	uint16_t dco_fraction;
};

/*
 * One CD clock step: at a reference clock, the CD clock reached with a
 * PLL ratio and a CD2X divider (given doubled, so 3 means 1.5).
 */
struct i915_xd_cdclk_row {
	uint32_t ref_khz;
	uint32_t cdclk_khz;
	uint8_t ratio;
	uint8_t divider_x2;
};

/*
 * How the display buffer is shared by a set of active pipes: the slices
 * each pipe gets, and whether the slices are joined to the one pipe.
 */
struct i915_xd_dbuf_rule {
	uint8_t pipes;
	uint8_t joined;
	uint8_t slices[4];
};

/*
 * A platform's credits towards the memory bus data box: A credits when
 * the bus is joined and when not, the BW and B credits, and the
 * back-to-back transaction limit and delay.
 */
struct i915_xd_dbox_credit {
	uint8_t a_joined;
	uint8_t a_split;
	uint8_t bw;
	uint8_t b;
	uint8_t b2b_max;
	uint8_t b2b_delay;
};

/* A bandwidth buddy page mask for one memory type and channel count. */
struct i915_xd_buddy_mask {
	uint8_t dram_type;
	uint8_t channels;
	uint32_t page_mask;
};

/*
 * The compensation reference words of a combo PHY for one voltage and
 * process (as COMP_DW3 reports them): COMP_DW1 (two bytes of it), DW9 and
 * DW10.
 */
struct i915_xd_procmon {
	uint32_t voltage_process;
	uint32_t dw1;
	uint32_t dw9;
	uint32_t dw10;
};

/* One PCI revision and the display stepping it stands for. */
struct i915_xd_step_row {
	uint8_t revid;
	uint8_t display_step;
};

/* One PCH device id (masked) and the kind of south display it has. */
struct i915_xd_pch_id {
	uint16_t id;
	uint8_t kind;
};

/*
 * One power well: its name for the log, the control it is switched
 * through and its index there, the power gate whose fuses it waits for,
 * its facts, the pipes whose interrupts must be restored after it comes
 * up, how long its enable may take, and what it powers.
 */
struct i915_xd_well_desc {
	const char *name;
	uint8_t control;
	uint8_t index;
	uint8_t fuse_gate;
	uint8_t flags;
	uint8_t irq_pipes;
	uint16_t enable_timeout_ms;
	uint64_t powers;
};

/*
 * One port of a platform: the DDI, the kind and index of its PHY, its
 * Type-C port (0 for none, 1..6), its AUX channel when the VBT names
 * none, its north Type-C hotplug pin (0 for none, 1..6), and its south
 * hotplug pin.
 */
struct i915_xd_port_desc {
	uint8_t ddi;
	uint8_t phy_kind;
	uint8_t phy;
	uint8_t tc;
	uint8_t aux;
	uint8_t north_pin;
	uint8_t south_pin_kind;
	uint8_t south_pin;
};

/*
 * One PLL of a platform: its kind, the value that selects it for a DDI,
 * its Type-C port (TC PLLs only, 1..6), and its enable and configuration
 * registers (0 where the PLL has none: a TC PLL's words are in its Dekel
 * PHY).
 */
struct i915_xd_pll_desc {
	uint8_t kind;
	uint8_t select;
	uint8_t tc;
	uint32_t enable;
	uint32_t cfgcr0;
	uint32_t cfgcr1;
};

/*
 * One DDI and the DVO port codes a VBT may name it with (up to three;
 * I915_XD_DVO_NONE fills the rest).
 */
struct i915_xd_vbt_dvo_row {
	uint8_t ddi;
	uint8_t codes[3];
};

/*
 * How a platform's VBT numbers its ports and pins: the DVO port codes of
 * each DDI; the VBT DDC pin code of each GMBUS pin (indexed by the GMBUS
 * pin, 0 where the pin has none); and the VBT AUX channel code of each
 * AUX channel (indexed by the AUX channel, 0 where it has none).  The
 * platforms differ because some VBTs number by PHY and some by DDI.
 */
struct i915_xd_vbt_maps {
	const struct i915_xd_vbt_dvo_row *dvo;
	unsigned ndvo;
	const uint8_t *ddc;
	unsigned nddc;
	const uint8_t *aux;
	unsigned naux;
};

/*
 * One platform, or one subplatform where its tables differ: everything
 * the xd code needs to know about the hardware, as data.
 */
struct i915_xd_platform {
	const char *name;
	uint8_t kind;
	uint8_t display_ver;
	uint8_t pch_kind;
	uint8_t pipes;
	uint8_t verified;
	const uint16_t *pci_ids;
	unsigned npci_ids;
	const struct i915_xd_step_row *steps;
	unsigned nsteps;
	const struct i915_xd_port_desc *ports;
	unsigned nports;
	uint8_t combo_phys;
	uint8_t comp_masters;
	uint8_t phy_misc;
	uint8_t hti;
	uint8_t tc_flow;
	uint8_t tccold;
	const struct i915_xd_well_desc *wells;
	unsigned nwells;
	const struct i915_xd_pll_desc *plls;
	unsigned nplls;
	uint8_t pll_map;
	int frac_wa_38p4;
	const struct i915_xd_cdclk_row *cdclk;
	unsigned ncdclk;
	const uint32_t *voltage_steps;
	unsigned nvoltage;
	int cdclk_crawl;
	uint8_t dbuf_slices;
	uint16_t dbuf_blocks;
	const struct i915_xd_dbuf_rule *dbuf;
	unsigned ndbuf;
	const struct i915_xd_dbox_credit *dbox;
	uint8_t abox_credit_mask;
	uint8_t abox_buddy_mask;
	int tracker_state_service;
	uint8_t wm_levels;
	uint8_t wm_read_latency_us;
	int hw_sagv_wm;
	const char *dmc_path;
	uint32_t pipedmc_base;
	const struct i915_xd_swing_set *swing;
	unsigned wa;
	const struct i915_xd_vbt_maps *vbt_maps;
	uint8_t fia;
	const struct i915_xd_dpll_dp_row *tbt_pll_19p2;
	const struct i915_xd_dpll_dp_row *tbt_pll_24;
	uint8_t power_class;
	const struct i915_xd9_ddi_buffers *ddi_buffers;
};


/*
 * The description chosen for a device, with what the choice found out:
 * the display stepping, the kind of the south display, and whether that
 * kind was read from the PCH or assumed from the platform (no PCH bridge
 * was found, or one the tables do not know, as in a virtual machine).
 */
struct i915_xd_choice {
	const struct i915_xd_platform *platform;
	uint8_t step;
	int pch_kind;
	int pch_assumed;
};

int drv_i915_xd_platform_choose(uint16_t device, uint8_t revid, int pch_present, uint16_t pch_device, struct i915_xd_choice *choice);
uint8_t drv_i915_xd_platform_step(const struct i915_xd_platform *platform, uint8_t revid);
int drv_i915_xd_pch_kind(uint16_t pch_device);
unsigned drv_i915_xd_platform_count(void);
const struct i915_xd_platform *drv_i915_xd_platform_row(unsigned index);
int drv_i915_xd_platform_choose_display9(uint16_t device, uint8_t revid, int pch_present, uint16_t pch_device, struct i915_xd_choice *choice);
unsigned drv_i915_xd_platform_display9_count(void);
const struct i915_xd_platform *drv_i915_xd_platform_display9_class(unsigned kind, unsigned power_class);
const struct i915_xd_platform *drv_i915_xd_platform_display9_row(unsigned index);
const struct i915_xd_port_desc *drv_i915_xd_platform_port(const struct i915_xd_platform *platform, unsigned ddi);
const struct i915_xd_procmon *drv_i915_xd_procmon(uint32_t comp_dw3);
const struct i915_xd_dpll_dp_row *drv_i915_xd_dpll_dp(unsigned ref_khz, unsigned link_khz);
const struct i915_xd_dpll_dco *drv_i915_xd_tbt_pll(unsigned ref_khz);
const struct i915_xd_buddy_mask *drv_i915_xd_buddy_mask(const struct i915_xd_platform *platform, uint8_t step, unsigned dram_type, unsigned channels);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_PLATFORM_H */
