/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's clock and reset generators (SYS, STG, AON and VOUT).
 *
 * Every clock of a generator has one 32-bit register at four times its
 * number.  The register holds, depending on the clock, a gate in bit 31,
 * an output inverter in bit 30, a source selector in bits 24 to 29 and a
 * divider in bits 0 to 23.  The reset lines of a generator are bits of a
 * few consecutive assert registers, with a status register of the same
 * shape behind them.
 *
 * Only the clocks in the tables below can be reached.  The tables hold
 * what the board's drivers use and the bus clocks above them; a bus clock
 * that several devices share can be started but not stopped.  Everything
 * else -- the CPUs', the memory's and the console's -- is outside the
 * tables and therefore never touched, so the running system stays as it
 * is.  The display's clocks are in the tables for the display's driver,
 * but nothing else starts or stops them; the bus clocks of the display's
 * domain are shared, so the picture the firmware left on the screen
 * keeps its bus.
 *
 * Rates are computed lock-free from the registers.  Gate, divider and
 * reset changes are read-modify-writes under crg_lock.
 */

#include "drivers/platform/visionfive2/vf2-crg.h"
#include "drivers/platform/visionfive2/vf2-syscon.h"
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * The fields of a clock register.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.1 SYS CRG
 * (clk_icg, clk_polarity, clk_mux_sel, clk_divcfg), Tables 2-50 to 2-208.
 */
#define CRG_GATE		0x80000000U
#define CRG_MUX_SHIFT		24U
#define CRG_MUX_MASK		0x3fU
#define CRG_DIVIDER_MASK	0x00ffffffU

/* The bytes of one generator's register block. */
#define CRG_BLOCK_SIZE		0x10000U

/* How deep the walk from a clock to its PLL or input may go. */
#define CRG_MAX_DEPTH		16U

/* How many status reads a reset change waits before giving up. */
#define CRG_RESET_POLLS		100000U

/* The most cells a clocks or resets property is read with. */
#define CRG_PROPERTY_CELLS	64U

/*
 * The PLL fields shared by the three PLLs' second register.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.4 SYS SYSCON,
 * Tables 2-303, 2-305 and 2-307 (pll_frac, pll_pd, pll_postdiv1).
 */
#define PLL_FRACTION_MASK	0x00ffffffU
#define PLL_POWER_DOWN		0x08000000U
#define PLL_POSTDIV_SHIFT	28U
#define PLL_POSTDIV_MASK	0x3U
#define PLL_PREDIV_MASK		0x3fU

/* The fraction is a 24-bit binary fraction of the feedback divider. */
#define PLL_FRACTION_BITS	24U

/* The fastest reference clock the PLL arithmetic accepts, in hertz. */
#define PLL_MAX_REFERENCE	100000000U

/*
 * What a clock register controls, as flags of struct crg_clock.
 *
 * GATE: bit 31 starts and stops the clock.  DIVIDER: bits 0 to 23 divide
 * the source.  MUX: bits 24 to 29 select one of the sources.  SHARED:
 * several devices hang off the clock, so it is never stopped.  SETTABLE:
 * the divider belongs to one device's driver, which may set its rate.
 * PASS: the register has no gate of its own (an inverter or a delay
 * line); starting and stopping it act on its first source.
 */
#define CLOCK_GATE		0x01U
#define CLOCK_DIVIDER		0x02U
#define CLOCK_MUX		0x04U
#define CLOCK_SHARED		0x08U
#define CLOCK_SETTABLE		0x10U
#define CLOCK_PASS		0x20U

/* The PLLs of the SYS system control block. */
#define PLL_COUNT		3U

/*
 * Where a clock takes its signal from.
 *
 * NONE: the table does not know, so its rate cannot be computed; its gate
 * still works.  CLOCK: another clock in a table.  PLL: one of the three
 * PLLs.  INPUT: a clock the generator's node lists in its clocks property
 * under the given name, which must be a fixed-rate clock.
 */
enum crg_source_kind {
	SOURCE_NONE = 0,
	SOURCE_CLOCK,
	SOURCE_PLL,
	SOURCE_INPUT
};

/*
 * One source of a clock.
 *
 * Crg and number name a clock for SOURCE_CLOCK; number names a PLL for
 * SOURCE_PLL; input names a clocks entry of the generator for
 * SOURCE_INPUT.
 */
struct crg_source {
	enum crg_source_kind kind;
	enum drv_vf2_crg crg;
	uint32_t number;
	const char *input;
};

/*
 * One clock a generator's table knows.
 *
 * A multiplexed clock lists its sources in selector order; any other clock
 * has one source.  Divider limits bound what drv_vf2_clock_set_rate()
 * writes.
 */
struct crg_clock {
	uint32_t index;
	unsigned flags;
	uint32_t divider_min;
	uint32_t divider_max;
	unsigned source_count;
	struct crg_source sources[2];
};

/*
 * The fixed description of one generator.
 *
 * Reset lines are bits of consecutive registers from the assert offset;
 * the status registers start at the status offset.
 */
struct crg_generator {
	const char *compatible;
	const struct crg_clock *clocks;
	unsigned clock_count;
	uint32_t reset_assert;
	uint32_t reset_status;
	uint32_t reset_count;
};

/*
 * The registers of one PLL in the SYS system control block.
 *
 * The mode register holds the two power-down bits that select integer or
 * fractional mode, the feedback register the feedback divider, the
 * fraction register the fraction, the power-down and the post-divider,
 * and the pre-divider register the pre-divider in its low bits.
 */
struct pll_layout {
	uint32_t mode_offset;
	unsigned dac_shift;
	unsigned dsm_shift;
	uint32_t feedback_offset;
	unsigned feedback_shift;
	uint32_t fraction_offset;
	uint32_t prediv_offset;
};

/*
 * One generator's place in the device tree and in the kernel's address
 * space.
 *
 * Filled by drv_vf2_crg_init() at boot and never changed afterwards.
 * Registers is NULL while the generator is absent.
 */
struct crg_instance {
	uint32_t node;
	volatile uint8_t *registers;
};

/*
 * The SYS generator's clocks.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.1 SYS CRG
 * (register fields and divider limits of offsets 0x14 to 0x278);
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_SYSCLK_* numbers); OpenBSD sys/dev/fdt/stfclock.c
 * (stfclock_get_frequency_jh7110_sys: the source of each clock).
 */
static const struct crg_clock sys_clocks[] = {
	/*
	 * The GPU root, from PLL2 or PLL1, which only the GPU's core clock
	 * divides.
	 *
	 * Values referenced:
	 * StarFive JH7110 TRM Preliminary V2, Table 2-53 Clock GPU Root
	 * (offset 0xc, selector 0 clk_pll2, 1 clk_pll1).
	 */
	{ 3, CLOCK_MUX, 0, 0, 2,
	  { { SOURCE_PLL, DRV_VF2_CRG_SYS, 2, NULL },
	    { SOURCE_PLL, DRV_VF2_CRG_SYS, 1, NULL } } },

	/* The bus root, from the oscillator or PLL2. */
	{ 5, CLOCK_MUX | CLOCK_SHARED, 0, 0, 2,
	  { { SOURCE_INPUT, DRV_VF2_CRG_SYS, 0, "osc" },
	    { SOURCE_PLL, DRV_VF2_CRG_SYS, 2, NULL } } },

	/* The AXI configuration bus. */
	{ 7, CLOCK_DIVIDER | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 5, NULL } } },

	/* The STG AXI and AHB bus. */
	{ 8, CLOCK_DIVIDER | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 7, NULL } } },

	/* The first AHB bus. */
	{ 9, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 8, NULL } } },

	/*
	 * The second AHB bus, which the display's register bus runs on.
	 *
	 * Values referenced:
	 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
	 * drivers/clk/starfive/clk-starfive-jh7110-sys.c (ahb1 below
	 * stg_axiahb).
	 */
	{ 10, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 8, NULL } } },

	/* The APB bus. */
	{ 11, CLOCK_DIVIDER | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 8, NULL } } },

	/* The first APB bus segment. */
	{ 12, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 11, NULL } } },

	/*
	 * The audio root, PLL2 divided by 2 to 8.
	 *
	 * Values referenced:
	 * StarFive JH7110 TRM Preliminary V2, Figure 2-4 Clock Structure
	 * (clk_audio_root below PLL2) and Table 2-66 Clock Audio Root
	 * (offset 0x40, divider 2 to 8); Linux v6.12
	 * drivers/clk/starfive/clk-starfive-jh7110-sys.c (PLL2 as its
	 * source).
	 */
	{ 16, CLOCK_DIVIDER | CLOCK_SHARED, 2, 8, 1,
	  { { SOURCE_PLL, DRV_VF2_CRG_SYS, 2, NULL } } },

	/*
	 * The ISP's double-rate clock and its AXI bus, which the GPU's system
	 * clock also runs on; shared, so only read.
	 *
	 * Values referenced:
	 * StarFive JH7110 TRM Preliminary V2, Table 2-70 Clock ISP 2x
	 * (offset 0x50, selector 0 clk_pll2, 1 clk_pll1, divider 2 to 8) and
	 * Table 2-71 Clock ISP AXI (offset 0x54, divider 2 to 4); Linux v6.12
	 * drivers/clk/starfive/clk-starfive-jh7110-sys.c (the ISP AXI clock
	 * below the double-rate clock).
	 */
	{ 20, CLOCK_MUX | CLOCK_DIVIDER | CLOCK_SHARED, 2, 8, 2,
	  { { SOURCE_PLL, DRV_VF2_CRG_SYS, 2, NULL },
	    { SOURCE_PLL, DRV_VF2_CRG_SYS, 1, NULL } } },
	{ 21, CLOCK_DIVIDER | CLOCK_SHARED, 2, 4, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 20, NULL } } },

	/*
	 * The GPU's clocks: the core clock's divider, whose rate the GPU's
	 * driver sets, and the gates of the core, system, register bus, timer
	 * and bus master clocks.
	 *
	 * Values referenced:
	 * StarFive JH7110 TRM Preliminary V2, Tables 2-95 to 2-100
	 * (clk_gpu_core at 0xb4, divider 3 to 7 as listed, taken down to 2
	 * since the vendor kernel runs the core at PLL2 / 2, 594 MHz:
	 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
	 * drivers/gpu/drm/img/img-rogue/services/system/rogue/sf_7110/sysconfig.h
	 * RGX_STARFIVE_7100_CORE_CLOCK_SPEED, and the firmware reports that
	 * rate on the board; the core, system and APB
	 * gates at 0xb8 to 0xc0; the timer at 0xc4, divider 12; the bus
	 * master gate at 0xc8); Linux v6.12
	 * include/dt-bindings/clock/starfive,jh7110-crg.h (JH7110_SYSCLK_*
	 * numbers 45 to 50) and drivers/clk/starfive/clk-starfive-jh7110-sys.c
	 * (the source of each clock).
	 */
	{ 45, CLOCK_DIVIDER | CLOCK_SETTABLE, 2, 7, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 3, NULL } } },
	{ 46, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 45, NULL } } },
	{ 47, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 21, NULL } } },
	{ 48, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 11, NULL } } },
	{ 49, CLOCK_GATE | CLOCK_DIVIDER, 12, 12, 1,
	  { { SOURCE_INPUT, DRV_VF2_CRG_SYS, 0, "osc" } } },
	{ 50, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 45, NULL } } },

	/*
	 * The display domain's clocks: the pixel source, the AXI divider and
	 * the bus gates of the display's network-on-chip port and of the VOUT
	 * generator's AHB and AXI side, which every clock of the VOUT
	 * generator runs on (all shared, so never stopped from here), and the
	 * HDMI transmitter's audio master clock.
	 *
	 * Values referenced:
	 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
	 * (JH7110_SYSCLK_VOUT_SRC 58 to JH7110_SYSCLK_VOUT_TOP_HDMITX0_MCLK
	 * 63); StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
	 * drivers/clk/starfive/clk-starfive-jh7110-sys.c (vout_src gate on
	 * PLL2, vout_axi divider of at most 7 on PLL2, noc_bus_disp_axi and
	 * vout_top_axi gates on vout_axi, vout_top_ahb gate on ahb1,
	 * vout_top_hdmitx0_mclk gate on mclk).
	 */
	{ 58, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_PLL, DRV_VF2_CRG_SYS, 2, NULL } } },
	{ 59, CLOCK_DIVIDER | CLOCK_SHARED, 1, 7, 1,
	  { { SOURCE_PLL, DRV_VF2_CRG_SYS, 2, NULL } } },
	{ 60, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 59, NULL } } },
	{ 61, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 10, NULL } } },
	{ 62, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 59, NULL } } },
	{ 63, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_SYS, 0, NULL } } },

	/* The SD card controller's bus interface. */
	{ 92, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 9, NULL } } },

	/* The SD card controller's card clock, whose rate its driver sets. */
	{ 94, CLOCK_GATE | CLOCK_DIVIDER | CLOCK_SETTABLE, 2, 15, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 7, NULL } } },

	/* The bus to the STG domain, which both PCIe hosts use. */
	{ 96, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_SYS, 0, NULL } } },

	/*
	 * The second Ethernet controller's clocks: the register and bus
	 * master interfaces, the transmit clock source (whose rate its driver
	 * sets on a board that takes the transmit clock from it), the reduced
	 * interface's transmit and receive clock, the time stamps, the
	 * transmit clock with its inverter, and the gated transmit clock the
	 * controller drives out.
	 *
	 * Values referenced:
	 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
	 * (JH7110_SYSCLK_GMAC1_AHB 97 to JH7110_SYSCLK_GMAC1_GTXC 107);
	 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
	 * drivers/clk/starfive/clk-starfive-jh7110-sys.c (gmac1_ahb gate on
	 * ahb0, gmac1_axi gate on stg_axiahb, gmac1_gtxclk divider of at most
	 * 15 on pll0_out, gmac1_rmii_rtx divider of at most 30 on the
	 * gmac1_rmii_refin input, gmac1_ptp gated divider of at most 31,
	 * gmac1_tx gated selector of gmac1_gtxclk and gmac1_rmii_rtx,
	 * gmac1_tx_inv inverter, gmac1_gtxc gate on gmac1_gtxclk).
	 */
	{ 97, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 9, NULL } } },
	{ 98, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 8, NULL } } },
	{ 100, CLOCK_DIVIDER | CLOCK_SETTABLE, 1, 15, 1,
	  { { SOURCE_PLL, DRV_VF2_CRG_SYS, 0, NULL } } },
	{ 101, CLOCK_DIVIDER, 1, 30, 1,
	  { { SOURCE_INPUT, DRV_VF2_CRG_SYS, 0, "gmac1_rmii_refin" } } },
	{ 102, CLOCK_GATE | CLOCK_DIVIDER, 1, 31, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_SYS, 0, NULL } } },
	{ 105, CLOCK_GATE | CLOCK_MUX, 0, 0, 2,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 100, NULL },
	    { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 101, NULL } } },
	{ 106, CLOCK_PASS, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 105, NULL } } },
	{ 107, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 100, NULL } } },

	/*
	 * The first Ethernet controller's transmit clock source, whose rate
	 * its driver sets on a board that takes the transmit clock from it.
	 */
	{ 108, CLOCK_GATE | CLOCK_DIVIDER | CLOCK_SETTABLE, 1, 15, 1,
	  { { SOURCE_PLL, DRV_VF2_CRG_SYS, 0, NULL } } },

	/* The first Ethernet controller's time-stamp clock. */
	{ 109, CLOCK_GATE | CLOCK_DIVIDER, 1, 31, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_SYS, 0, NULL } } },

	/* The first Ethernet controller's transmit delay line. */
	{ 111, CLOCK_PASS, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 108, NULL } } },

	/* The pin controller's bus interface. */
	{ 112, CLOCK_GATE | CLOCK_SHARED, 0, 0, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_SYS, 0, NULL } } },

	/* The audio PWM converter's bus interface. */
	{ 157, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_SYS, 0, NULL } } },

	/*
	 * The audio PWM converter's sample clock, whose rate its driver sets.
	 *
	 * Values referenced:
	 * StarFive JH7110 TRM Preliminary V2, Table 2-208 Clock PWMDAC Core
	 * (divider 12 to 256); Linux v6.12
	 * drivers/clk/starfive/clk-starfive-jh7110-sys.c (the audio root as
	 * its source).
	 */
	{ 158, CLOCK_GATE | CLOCK_DIVIDER | CLOCK_SETTABLE, 12, 256, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 16, NULL } } }
};

/*
 * The STG generator's clocks: the PCIe hosts' and the DMA controller's
 * gates.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.2 STG CRG
 * (Tables 2-256 to 2-261, 2-275 and 2-276);
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_STGCLK_* numbers).
 */
static const struct crg_clock stg_clocks[] = {
	/* The first PCIe host's master, register and link clocks. */
	{ 8, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } },
	{ 9, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } },
	{ 10, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } },

	/* The second PCIe host's master, register and link clocks. */
	{ 11, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } },
	{ 12, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } },
	{ 13, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } },

	/* The DMA controller's bus master and register clocks. */
	{ 27, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } },
	{ 28, CLOCK_GATE, 0, 0, 1, { { SOURCE_NONE, DRV_VF2_CRG_STG, 0, NULL } } }
};

/*
 * The AON generator's clocks: the first Ethernet controller's.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.3 AON CRG
 * (Tables 2-281 to 2-285);
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_AONCLK_* numbers); OpenBSD sys/dev/fdt/stfclock.c
 * (stfclock_get_frequency_jh7110_aon: the source of each clock).
 */
static const struct crg_clock aon_clocks[] = {
	/* The controller's register interface. */
	{ 2, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_AON, 0, NULL } } },

	/* The controller's bus master interface. */
	{ 3, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 8, NULL } } },

	/* The reduced-interface transmit and receive clock. */
	{ 4, CLOCK_DIVIDER, 1, 30, 1,
	  { { SOURCE_INPUT, DRV_VF2_CRG_AON, 0, "gmac0_rmii_refin" } } },

	/* The transmit clock, from the SYS source or the reduced interface. */
	{ 5, CLOCK_GATE | CLOCK_MUX, 0, 0, 2,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 108, NULL },
	    { SOURCE_CLOCK, DRV_VF2_CRG_AON, 4, NULL } } },

	/* The transmit clock's inverter. */
	{ 6, CLOCK_PASS, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_AON, 5, NULL } } }
};

/*
 * The VOUT generator's clocks: the display controller's and the HDMI
 * transmitter's.
 *
 * The display controller's two pixel clocks choose between the
 * generator's own pixel divider and the HDMI transmitter's PHY output
 * (the hdmitx0_pixelclk input, whose rate in the device tree is a nominal
 * one); the LCD domain clock chooses between the two pixel clocks.  The
 * register bus divider feeds the transmitter's and the DSI's register
 * clocks, so it is shared.
 *
 * Values referenced:
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_VOUTCLK_* numbers 0 to 17); StarFive Linux
 * JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/clk/starfive/clk-starfive-jh7110-vout.c (jh7110_voutclk_data:
 * apb divider of at most 8 on vout_top_ahb, dc8200_pix divider of at most
 * 63 on vout_src, the dc8200 axi and core gates on vout_top_axi and ahb
 * gate on vout_top_ahb, the pix0 and pix1 gated selectors of dc8200_pix
 * and hdmitx0_pixelclk, the dom_vout_top_lcd gated selector of pix0 and
 * pix1, the hdmi_tx mclk, bclk and sys gates);
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (voutcrg's clock-names).
 */
static const struct crg_clock vout_clocks[] = {
	/* The register bus divider. */
	{ 0, CLOCK_DIVIDER | CLOCK_SHARED, 1, 8, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 61, NULL } } },

	/* The pixel divider, whose rate the display's driver sets. */
	{ 1, CLOCK_DIVIDER | CLOCK_SETTABLE, 1, 63, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 58, NULL } } },

	/* The display controller's AXI, core and AHB clocks. */
	{ 4, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 62, NULL } } },
	{ 5, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 62, NULL } } },
	{ 6, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 61, NULL } } },

	/* The display controller's two pixel clocks. */
	{ 7, CLOCK_GATE | CLOCK_MUX, 0, 0, 2,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_VOUT, 1, NULL },
	    { SOURCE_INPUT, DRV_VF2_CRG_VOUT, 0, "hdmitx0_pixelclk" } } },
	{ 8, CLOCK_GATE | CLOCK_MUX, 0, 0, 2,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_VOUT, 1, NULL },
	    { SOURCE_INPUT, DRV_VF2_CRG_VOUT, 0, "hdmitx0_pixelclk" } } },

	/* The LCD domain clock. */
	{ 9, CLOCK_GATE | CLOCK_MUX, 0, 0, 2,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_VOUT, 7, NULL },
	    { SOURCE_CLOCK, DRV_VF2_CRG_VOUT, 8, NULL } } },

	/* The HDMI transmitter's audio master, audio bit and register clocks. */
	{ 15, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_SYS, 63, NULL } } },
	{ 16, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_NONE, DRV_VF2_CRG_VOUT, 0, NULL } } },
	{ 17, CLOCK_GATE, 0, 0, 1,
	  { { SOURCE_CLOCK, DRV_VF2_CRG_VOUT, 0, NULL } } }
};

/*
 * The four generators, in drv_vf2_crg order.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, Tables 2-240 and 2-244 (SYS reset
 * assert and status at 0x2f8 and 0x308), 2-277 and 2-278 (STG at 0x74 and
 * 0x78), 2-293 and 2-294 (AON at 0x38 and 0x3c), 5-24 (VOUT status
 * at 0x4c; Table 5-23 prints the VOUT assert register at 0x38, which is
 * the generator's clock 14, so its offset 0x48 comes from the StarFive
 * Linux below);
 * Linux v6.12 include/dt-bindings/reset/starfive,jh7110-crg.h
 * (JH7110_SYSRST_END, JH7110_STGRST_END, JH7110_AONRST_END,
 * JH7110_VOUTRST_END 12); StarFive Linux JH7110_VisionFive2_6.12.y_devel
 * 4cecf169f38e drivers/reset/starfive/reset-starfive-jh7110.c
 * (jh7110_vout_info).
 */
static const struct crg_generator crg_generators[DRV_VF2_CRG_COUNT] = {
	{ "starfive,jh7110-syscrg", sys_clocks, sizeof(sys_clocks) / sizeof(sys_clocks[0]),
	  0x2f8U, 0x308U, 126U },
	{ "starfive,jh7110-stgcrg", stg_clocks, sizeof(stg_clocks) / sizeof(stg_clocks[0]),
	  0x74U, 0x78U, 23U },
	{ "starfive,jh7110-aoncrg", aon_clocks, sizeof(aon_clocks) / sizeof(aon_clocks[0]),
	  0x38U, 0x3cU, 8U },
	{ "starfive,jh7110-voutcrg", vout_clocks, sizeof(vout_clocks) / sizeof(vout_clocks[0]),
	  0x48U, 0x4cU, 12U }
};

/*
 * The three PLLs' registers in the SYS system control block.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.8.4 SYS SYSCON,
 * Tables 2-301 to 2-308 (pll0 to pll2 dacpd, dsmpd, fbdiv, frac, prediv).
 */
static const struct pll_layout pll_layouts[PLL_COUNT] = {
	{ 0x18U, 24U, 25U, 0x1cU, 0U, 0x20U, 0x24U },
	{ 0x24U, 15U, 16U, 0x24U, 17U, 0x28U, 0x2cU },
	{ 0x2cU, 15U, 16U, 0x2cU, 17U, 0x30U, 0x34U }
};

/*
 * The mapped generators, in drv_vf2_crg order.
 *
 * Written only by drv_vf2_crg_init(), which the board runs once on the
 * boot CPU before any driver attaches; read without a lock afterwards.
 */
static struct crg_instance crg_instances[DRV_VF2_CRG_COUNT];

/*
 * The device tree the generators were found in.
 *
 * Kept by drv_vf2_crg_init() for the input clocks' rates, which the
 * generators' nodes describe; the blob stays mapped for the life of the
 * system.
 */
static struct drv_fdt crg_tree;

/*
 * Serializes every read-modify-write of a generator register.
 *
 * A leaf lock: nothing else is taken while it is held.  Taken with
 * interrupts held off, so a change may also run from an interrupt.
 */
static struct spinlock crg_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "vf2 crg", 0, 0
};

static int map_generator(const struct drv_fdt *fdt, enum drv_vf2_crg crg);
static const struct crg_clock *find_clock(enum drv_vf2_crg crg, uint32_t index);
static volatile uint32_t *clock_register(enum drv_vf2_crg crg, uint32_t index);
static int current_source(const struct crg_clock *entry, uint32_t contents, const struct crg_source **source);
static int clock_rate(enum drv_vf2_crg crg, uint32_t index, unsigned depth, uint64_t *rate);
static int source_rate(const struct crg_source *source, unsigned depth, uint64_t *rate);
static int pll_rate(uint32_t pll, uint64_t *rate);
static int input_rate(enum drv_vf2_crg crg, const char *input, uint64_t *rate);
static int generator_of(uint32_t node, enum drv_vf2_crg *crg);
static int lookup_specifier(const struct drv_fdt *fdt, uint32_t node, const char *list, const char *cells_name, unsigned position, enum drv_vf2_crg *crg, uint32_t *index);
static int lookup_named(const struct drv_fdt *fdt, uint32_t node, const char *list, const char *names, const char *cells_name, const char *name, enum drv_vf2_crg *crg, uint32_t *index);
static int reset_change(const struct drv_vf2_reset *reset, int assert);

/*
 * Finds the four generators in the device tree and maps them.
 */
int
drv_vf2_crg_init(
	const struct drv_fdt *fdt)
{
	unsigned crg;
	int error;

	/* Refuses a missing tree. */
	if (fdt == NULL)
		return EINVAL;

	/* Keeps the tree for the input clocks. */
	crg_tree = *fdt;

	/* Maps every generator the tree describes; a missing one stays absent. */
	for (crg = 0; crg < DRV_VF2_CRG_COUNT; crg++) {
		error = map_generator(fdt, (enum drv_vf2_crg)crg);
		if (error != 0 && error != ENOENT)
			return error;
	}

	/* Refuses a tree without the SYS generator, which feeds the others. */
	if (crg_instances[DRV_VF2_CRG_SYS].registers == NULL)
		return ENODEV;

	/* Succeeded: the generators the tree describes are mapped. */
	return 0;
}

/*
 * Finds a clock of a device by its clock-names entry.
 */
int
drv_vf2_clock_lookup(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	struct drv_vf2_clock *clock)
{
	enum drv_vf2_crg crg;
	uint32_t index;
	int error;

	/* Refuses a missing blob or result. */
	if (fdt == NULL || clock == NULL)
		return EINVAL;

	/* Decodes the clocks entry the name selects. */
	error = lookup_named(fdt, node, "clocks", "clock-names", "#clock-cells", name, &crg, &index);
	if (error != 0)
		return error;

	/* Succeeded: the generator and its number for the clock. */
	clock->crg = crg;
	clock->index = index;
	return 0;
}

/*
 * Starts a clock and every gate above it on its current path.
 */
int
drv_vf2_clock_enable(
	const struct drv_vf2_clock *clock)
{
	const struct crg_clock *entry;
	const struct crg_source *source;
	volatile uint32_t *address;
	enum drv_vf2_crg crg;
	unsigned long irq;
	uint32_t contents;
	uint32_t index;
	unsigned depth;
	int error;

	/* Refuses a missing clock or one outside the tables. */
	if (clock == NULL)
		return EINVAL;
	entry = find_clock(clock->crg, clock->index);
	if (entry == NULL)
		return ENOENT;

	/* Refuses a clock whose generator is absent. */
	address = clock_register(clock->crg, clock->index);
	if (address == NULL)
		return ENODEV;

	/*
	 * Opens the gates from the clock up to its PLL or input.  Each step
	 * follows the source the register currently selects, so the path is
	 * the one the clock really runs on.
	 */
	error = 0;
	crg = clock->crg;
	index = clock->index;
	irq = spin_lock_irqsave(&crg_lock);

	for (depth = 0; depth < CRG_MAX_DEPTH; depth++) {
		/* Stops at a clock whose generator is absent. */
		entry = find_clock(crg, index);
		address = clock_register(crg, index);
		if (entry == NULL || address == NULL) {
			error = ENODEV;
			break;
		}

		/* Opens this clock's own gate when it is closed. */
		contents = kern_mmio_read32(address);
		if ((entry->flags & CLOCK_GATE) != 0 && (contents & CRG_GATE) == 0)
			kern_mmio_write32(address, contents | CRG_GATE);

		/* Finds the source the clock runs on now. */
		error = current_source(entry, contents, &source);
		if (error != 0)
			break;

		/* Ends at a PLL, an input or an unknown source: no gate is above. */
		if (source->kind != SOURCE_CLOCK)
			break;

		/* Goes one step up. */
		crg = source->crg;
		index = source->number;
	}

	spin_unlock_irqrestore(&crg_lock, irq);

	/* Reports a path that could not be followed to its end. */
	if (error != 0)
		return error;

	/* Succeeded: the clock and the gates above it are open. */
	return 0;
}

/*
 * Stops a clock's own gate.
 */
int
drv_vf2_clock_disable(
	const struct drv_vf2_clock *clock)
{
	const struct crg_clock *entry;
	volatile uint32_t *address;
	enum drv_vf2_crg crg;
	unsigned long irq;
	uint32_t contents;

	/* Refuses a missing clock or one outside the tables. */
	if (clock == NULL)
		return EINVAL;
	crg = clock->crg;
	entry = find_clock(crg, clock->index);
	if (entry == NULL)
		return ENOENT;

	/* Refuses to stop a bus clock that other devices run on. */
	if ((entry->flags & CLOCK_SHARED) != 0)
		return EBUSY;

	/* A register without a gate stops its first source instead. */
	if ((entry->flags & CLOCK_PASS) != 0) {
		/* Only a clock source can be stopped this way. */
		if (entry->sources[0].kind != SOURCE_CLOCK)
			return ENOTSUP;

		/* Follows the pass-through to the clock that has the gate. */
		crg = entry->sources[0].crg;
		entry = find_clock(crg, entry->sources[0].number);
		if (entry == NULL)
			return ENOENT;

		/* Refuses to stop that clock too when other devices run on it. */
		if ((entry->flags & CLOCK_SHARED) != 0)
			return EBUSY;
	}

	/* A clock without a gate cannot be stopped. */
	if ((entry->flags & CLOCK_GATE) == 0)
		return ENOTSUP;

	/* Refuses a clock whose generator is absent. */
	address = clock_register(crg, entry->index);
	if (address == NULL)
		return ENODEV;

	/* Closes the gate when it is open. */
	irq = spin_lock_irqsave(&crg_lock);

	contents = kern_mmio_read32(address);
	if ((contents & CRG_GATE) != 0)
		kern_mmio_write32(address, contents & ~CRG_GATE);

	spin_unlock_irqrestore(&crg_lock, irq);

	/* Succeeded: the clock is stopped. */
	return 0;
}

/*
 * Reports whether a clock's own gate is open.
 */
int
drv_vf2_clock_running(
	const struct drv_vf2_clock *clock,
	int *running)
{
	const struct crg_clock *entry;
	volatile uint32_t *address;
	uint32_t contents;

	/* Refuses a missing clock or result, or a clock outside the tables. */
	if (clock == NULL || running == NULL)
		return EINVAL;
	entry = find_clock(clock->crg, clock->index);
	if (entry == NULL)
		return ENOENT;

	/* Refuses a clock whose generator is absent. */
	address = clock_register(clock->crg, clock->index);
	if (address == NULL)
		return ENODEV;

	/* A clock without a gate of its own runs whenever its source does. */
	if ((entry->flags & CLOCK_GATE) == 0) {
		*running = 1;
		return 0;
	}

	/* Reads the gate bit. */
	contents = kern_mmio_read32(address);
	*running = 0;
	if ((contents & CRG_GATE) != 0)
		*running = 1;

	/* Succeeded: the gate's state. */
	return 0;
}

/*
 * Computes a clock's rate in hertz from the registers.
 */
int
drv_vf2_clock_get_rate(
	const struct drv_vf2_clock *clock,
	uint64_t *rate)
{
	uint64_t computed;
	int error;

	/* Refuses a missing clock or result. */
	if (clock == NULL || rate == NULL)
		return EINVAL;

	/* Walks from the clock to its PLL or input. */
	error = clock_rate(clock->crg, clock->index, 0, &computed);
	if (error != 0)
		return error;

	/* Succeeded: the clock's rate. */
	*rate = computed;
	return 0;
}

/*
 * Computes the rate in hertz of one of the three PLLs (0 to 2).
 */
int
drv_vf2_pll_get_rate(
	unsigned pll,
	uint64_t *rate)
{
	uint64_t computed;
	int error;

	/* Refuses a missing result or a PLL outside the three. */
	if (rate == NULL || pll >= PLL_COUNT)
		return EINVAL;

	/* Decodes the PLL's settings. */
	error = pll_rate(pll, &computed);
	if (error != 0)
		return error;

	/* Succeeded: the PLL's rate. */
	*rate = computed;
	return 0;
}

/*
 * Sets a clock's divider so that its rate is as close as possible to the
 * requested one without exceeding it.
 */
int
drv_vf2_clock_set_rate(
	const struct drv_vf2_clock *clock,
	uint64_t rate)
{
	const struct crg_clock *entry;
	const struct crg_source *source;
	volatile uint32_t *address;
	unsigned long irq;
	uint64_t parent;
	uint64_t divider;
	uint32_t contents;
	int error;

	/* Refuses a missing clock, a zero rate or a clock outside the tables. */
	if (clock == NULL || rate == 0)
		return EINVAL;
	entry = find_clock(clock->crg, clock->index);
	if (entry == NULL)
		return ENOENT;

	/* Refuses a clock whose divider no single driver owns. */
	if ((entry->flags & CLOCK_SETTABLE) == 0)
		return ENOTSUP;

	/* Refuses a clock whose generator is absent. */
	address = clock_register(clock->crg, clock->index);
	if (address == NULL)
		return ENODEV;

	/* Finds the source the clock runs on now. */
	contents = kern_mmio_read32(address);
	error = current_source(entry, contents, &source);
	if (error != 0)
		return error;

	/* Computes the source's rate the divider works from. */
	error = source_rate(source, 1, &parent);
	if (error != 0)
		return error;

	/*
	 * Picks the smallest divider that does not exceed the rate.  The
	 * smallest divider the hardware allows may give a lower rate than
	 * asked; the largest one must not give a higher one.
	 */
	divider = (parent + rate - 1U) / rate;
	if (divider < entry->divider_min)
		divider = entry->divider_min;
	if (divider > entry->divider_max)
		return ERANGE;

	/* Writes the divider, keeping the gate and the other fields. */
	irq = spin_lock_irqsave(&crg_lock);

	contents = kern_mmio_read32(address);
	contents &= ~CRG_DIVIDER_MASK;
	contents |= (uint32_t)divider;
	kern_mmio_write32(address, contents);

	spin_unlock_irqrestore(&crg_lock, irq);

	/* Succeeded: the clock runs at or just below the rate. */
	return 0;
}

/*
 * Applies a device's assigned-clocks and assigned-clock-rates.
 */
int
drv_vf2_clock_apply_assigned(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	struct drv_vf2_clock clock;
	const uint8_t *rates;
	uint32_t rates_length;
	uint64_t rate;
	unsigned position;
	int error;

	/* Refuses a missing blob. */
	if (fdt == NULL)
		return EINVAL;

	/* Reads the rates; a device without them has nothing to apply. */
	error = drv_fdt_property(fdt, node, "assigned-clock-rates", &rates, &rates_length);
	if (error != 0)
		return error;

	/* Sets every clock whose rate is not zero. */
	for (position = 0; (position + 1U) * 4U <= rates_length; position++) {
		/* A zero rate leaves its clock as it is. */
		rate = drv_fdt_cells_value(rates, position, 1U);
		if (rate == 0)
			continue;

		/* Finds the clock the same position of assigned-clocks names. */
		error = lookup_specifier(fdt,
					 node,
					 "assigned-clocks",
					 "#clock-cells",
					 position,
					 &clock.crg,
					 &clock.index);
		if (error != 0)
			return error;

		/* Sets the clock's rate. */
		error = drv_vf2_clock_set_rate(&clock, rate);
		if (error != 0)
			return error;
	}

	/* Succeeded: every assigned rate is set. */
	return 0;
}

/*
 * Finds a multiplexed clock that one driver may steer.
 */
static int
find_mux(
	const struct drv_vf2_clock *clock,
	const struct crg_clock **entry,
	volatile uint32_t **address)
{
	/* Refuses a clock outside the tables. */
	*entry = find_clock(clock->crg, clock->index);
	if (*entry == NULL)
		return ENOENT;

	/* Refuses a clock without a selector, or one several devices run on. */
	if (((*entry)->flags & CLOCK_MUX) == 0)
		return ENOTSUP;
	if (((*entry)->flags & CLOCK_SHARED) != 0)
		return ENOTSUP;

	/* Refuses a clock whose generator is absent. */
	*address = clock_register(clock->crg, clock->index);
	if (*address == NULL)
		return ENODEV;

	/* Succeeded: the clock may be steered. */
	return 0;
}

/*
 * Writes a multiplexed clock's selector, keeping the gate and the other
 * fields.
 */
static void
write_mux(
	volatile uint32_t *address,
	unsigned selector)
{
	unsigned long irq;
	uint32_t contents;

	/* Rewrites only the selector field under the lock. */
	irq = spin_lock_irqsave(&crg_lock);

	contents = kern_mmio_read32(address);
	contents &= ~(CRG_MUX_MASK << CRG_MUX_SHIFT);
	contents |= (uint32_t)selector << CRG_MUX_SHIFT;
	kern_mmio_write32(address, contents);

	spin_unlock_irqrestore(&crg_lock, irq);
}

/*
 * Makes a multiplexed clock run from one of its sources.
 */
int
drv_vf2_clock_set_parent(
	const struct drv_vf2_clock *clock,
	const struct drv_vf2_clock *parent)
{
	const struct crg_clock *entry;
	const struct crg_source *source;
	volatile uint32_t *address;
	unsigned selector;
	int error;

	/* Refuses a missing clock or parent. */
	if (clock == NULL || parent == NULL)
		return EINVAL;
	error = find_mux(clock, &entry, &address);
	if (error != 0)
		return error;

	/* Finds the parent among the clock's sources. */
	for (selector = 0; selector < entry->source_count; selector++) {
		/* Stops at the source that is the parent clock. */
		source = &entry->sources[selector];
		if (source->kind == SOURCE_CLOCK &&
		    source->crg == parent->crg &&
		    source->number == parent->index)
			break;
	}

	/* Refuses a parent the clock cannot run from. */
	if (selector == entry->source_count)
		return ENOENT;

	/* Succeeded: the clock runs from the parent. */
	write_mux(address, selector);
	return 0;
}

/*
 * Makes a multiplexed clock run from one of its generator's inputs.
 */
int
drv_vf2_clock_set_input_parent(
	const struct drv_vf2_clock *clock,
	const char *input)
{
	const struct crg_clock *entry;
	const struct crg_source *source;
	volatile uint32_t *address;
	unsigned selector;
	int error;

	/* Refuses a missing clock or input name. */
	if (clock == NULL || input == NULL)
		return EINVAL;
	error = find_mux(clock, &entry, &address);
	if (error != 0)
		return error;

	/* Finds the input among the clock's sources. */
	for (selector = 0; selector < entry->source_count; selector++) {
		/* Stops at the source that is the named input. */
		source = &entry->sources[selector];
		if (source->kind == SOURCE_INPUT &&
		    source->input != NULL &&
		    kern_strcmp(source->input, input) == 0)
			break;
	}

	/* Refuses an input the clock cannot run from. */
	if (selector == entry->source_count)
		return ENOENT;

	/* Succeeded: the clock runs from the input. */
	write_mux(address, selector);
	return 0;
}

/*
 * Makes a multiplexed clock run from one of the PLLs.
 */
int
drv_vf2_clock_set_pll(
	const struct drv_vf2_clock *clock,
	unsigned pll)
{
	const struct crg_clock *entry;
	const struct crg_source *source;
	volatile uint32_t *address;
	unsigned long irq;
	uint32_t contents;
	unsigned selector;

	/* Refuses a missing clock or one outside the tables. */
	if (clock == NULL)
		return EINVAL;
	entry = find_clock(clock->crg, clock->index);
	if (entry == NULL)
		return ENOENT;

	/* Refuses a clock without a selector, or one several devices run on. */
	if ((entry->flags & CLOCK_MUX) == 0)
		return ENOTSUP;
	if ((entry->flags & CLOCK_SHARED) != 0)
		return ENOTSUP;

	/* Refuses a clock whose generator is absent. */
	address = clock_register(clock->crg, clock->index);
	if (address == NULL)
		return ENODEV;

	/* Finds the PLL among the clock's sources. */
	for (selector = 0; selector < entry->source_count; selector++) {
		/* Stops at the source that is the PLL. */
		source = &entry->sources[selector];
		if (source->kind == SOURCE_PLL && source->number == pll)
			break;
	}

	/* Refuses a PLL the clock cannot run from. */
	if (selector == entry->source_count)
		return ENOENT;

	/* Writes the selector, keeping the other fields. */
	irq = spin_lock_irqsave(&crg_lock);

	contents = kern_mmio_read32(address);
	contents &= ~(CRG_MUX_MASK << CRG_MUX_SHIFT);
	contents |= (uint32_t)selector << CRG_MUX_SHIFT;
	kern_mmio_write32(address, contents);

	spin_unlock_irqrestore(&crg_lock, irq);

	/* Succeeded: the clock runs from the PLL. */
	return 0;
}

/*
 * Applies a device's assigned-clocks and assigned-clock-parents.
 */
int
drv_vf2_clock_apply_assigned_parents(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	struct drv_vf2_clock clock;
	struct drv_vf2_clock parent;
	const uint8_t *parents;
	uint32_t parents_length;
	unsigned position;
	int error;

	/* Refuses a missing blob. */
	if (fdt == NULL)
		return EINVAL;

	/* Reads the parents; a device without them has nothing to apply. */
	error = drv_fdt_property(fdt, node, "assigned-clock-parents", &parents, &parents_length);
	if (error != 0)
		return error;

	/* Moves every clock the list names a parent for. */
	position = 0;
	for (;;) {
		/* Finds the parent at this position; the list ends where its entries do. */
		error = lookup_specifier(fdt,
					 node,
					 "assigned-clock-parents",
					 "#clock-cells",
					 position,
					 &parent.crg,
					 &parent.index);
		if (error == ENOENT)
			break;

		/* Refuses a parent that is not a generator's clock. */
		if (error != 0)
			return error;

		/* Finds the clock the same position of assigned-clocks names. */
		error = lookup_specifier(fdt,
					 node,
					 "assigned-clocks",
					 "#clock-cells",
					 position,
					 &clock.crg,
					 &clock.index);
		if (error != 0)
			return error;

		/* Makes the clock run from the parent. */
		error = drv_vf2_clock_set_parent(&clock, &parent);
		if (error != 0)
			return error;

		/* Goes on to the next entry. */
		position++;
	}

	/* Succeeded: every assigned parent is set. */
	return 0;
}

/*
 * Finds a reset line of a device by its reset-names entry.
 */
int
drv_vf2_reset_lookup(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	struct drv_vf2_reset *reset)
{
	enum drv_vf2_crg crg;
	uint32_t index;
	int error;

	/* Refuses a missing blob or result. */
	if (fdt == NULL || reset == NULL)
		return EINVAL;

	/* Decodes the resets entry the name selects. */
	error = lookup_named(fdt, node, "resets", "reset-names", "#reset-cells", name, &crg, &index);
	if (error != 0)
		return error;

	/* Refuses a line the generator does not have. */
	if (index >= crg_generators[crg].reset_count)
		return ERANGE;

	/* Succeeded: the generator and its number for the line. */
	reset->crg = crg;
	reset->index = index;
	return 0;
}

/*
 * Finds a reset line of a device by its position in the resets property.
 */
int
drv_vf2_reset_lookup_index(
	const struct drv_fdt *fdt,
	uint32_t node,
	unsigned position,
	struct drv_vf2_reset *reset)
{
	enum drv_vf2_crg crg;
	uint32_t index;
	int error;

	/* Refuses a missing blob or result. */
	if (fdt == NULL || reset == NULL)
		return EINVAL;

	/* Decodes the resets entry at that position. */
	error = lookup_specifier(fdt, node, "resets", "#reset-cells", position, &crg, &index);
	if (error != 0)
		return error;

	/* Refuses a line the generator does not have. */
	if (index >= crg_generators[crg].reset_count)
		return ERANGE;

	/* Succeeded: the generator and its number for the line. */
	reset->crg = crg;
	reset->index = index;
	return 0;
}

/*
 * Holds a device in reset.
 */
int
drv_vf2_reset_assert(
	const struct drv_vf2_reset *reset)
{
	int error;

	/* Sets the line's assert bit and waits for the status to follow. */
	error = reset_change(reset, 1);
	if (error != 0)
		return error;

	/* Succeeded: the device is held in reset. */
	return 0;
}

/*
 * Releases a device from reset and waits until the generator reports it
 * released.
 */
int
drv_vf2_reset_deassert(
	const struct drv_vf2_reset *reset)
{
	int error;

	/* Clears the line's assert bit and waits for the status to follow. */
	error = reset_change(reset, 0);
	if (error != 0)
		return error;

	/* Succeeded: the device is out of reset. */
	return 0;
}

/*
 * Reports whether the generator's status shows a line released.
 */
int
drv_vf2_reset_released(
	const struct drv_vf2_reset *reset,
	int *released)
{
	const struct crg_generator *generator;
	volatile uint8_t *registers;
	uint32_t offset;
	uint32_t status;

	/* Refuses a missing line or result, or a line out of range. */
	if (reset == NULL || released == NULL)
		return EINVAL;
	if ((unsigned)reset->crg >= DRV_VF2_CRG_COUNT)
		return EINVAL;
	generator = &crg_generators[reset->crg];
	if (reset->index >= generator->reset_count)
		return ERANGE;

	/* Refuses a line whose generator is absent. */
	registers = crg_instances[reset->crg].registers;
	if (registers == NULL)
		return ENODEV;

	/*
	 * Reads the line's status bit; a set bit means released.
	 *
	 * Value referenced:
	 * StarFive JH7110 TRM Preliminary V2, Tables 2-240 and 2-244 (the
	 * JTAG bridge's line resets to assert 0 and status 1).
	 */
	offset = generator->reset_status + (reset->index / 32U) * 4U;
	status = kern_mmio_read32((volatile uint32_t *)(registers + offset));
	*released = 0;
	if ((status & (1U << (reset->index % 32U))) != 0)
		*released = 1;

	/* Succeeded: the line's state. */
	return 0;
}

/* Finds and maps one generator, recording where it is. */
static int
map_generator(
	const struct drv_fdt *fdt,
	enum drv_vf2_crg crg)
{
	struct crg_instance *instance;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	void *mapped;
	int error;

	/* Finds the generator's node; a tree without it leaves it absent. */
	instance = &crg_instances[crg];
	error = drv_fdt_find_compatible(fdt, crg_generators[crg].compatible, DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENOENT;

	/* Reads where the registers are; the block must hold every register. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < CRG_BLOCK_SIZE)
		return EINVAL;

	/* Maps the registers as device memory. */
	error = kern_device_map(address, CRG_BLOCK_SIZE, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: records the generator for the other calls. */
	instance->node = node;
	instance->registers = mapped;
	return 0;
}

/* Finds a clock in its generator's table. */
static const struct crg_clock *
find_clock(
	enum drv_vf2_crg crg,
	uint32_t index)
{
	const struct crg_generator *generator;
	unsigned position;

	/* Refuses a generator number outside the four. */
	if ((unsigned)crg >= DRV_VF2_CRG_COUNT)
		return NULL;

	/* Searches the generator's table. */
	generator = &crg_generators[crg];
	for (position = 0; position < generator->clock_count; position++) {
		/* Stops at the clock's entry. */
		if (generator->clocks[position].index == index)
			return &generator->clocks[position];
	}

	/* Reports a clock the table does not know. */
	return NULL;
}

/* Finds the kernel address of a clock's register, or NULL when absent. */
static volatile uint32_t *
clock_register(
	enum drv_vf2_crg crg,
	uint32_t index)
{
	volatile uint8_t *registers;

	/* Refuses a generator number outside the four. */
	if ((unsigned)crg >= DRV_VF2_CRG_COUNT)
		return NULL;

	/* Refuses a generator the tree did not describe. */
	registers = crg_instances[crg].registers;
	if (registers == NULL)
		return NULL;

	/* Reports the register at four times the clock's number. */
	return (volatile uint32_t *)(registers + index * 4U);
}

/* Finds the source a clock's register currently selects. */
static int
current_source(
	const struct crg_clock *entry,
	uint32_t contents,
	const struct crg_source **source)
{
	uint32_t selector;

	/* A clock without a selector has its one source. */
	if ((entry->flags & CLOCK_MUX) == 0) {
		*source = &entry->sources[0];
		return 0;
	}

	/* Refuses a selector beyond the sources the table lists. */
	selector = (contents >> CRG_MUX_SHIFT) & CRG_MUX_MASK;
	if (selector >= entry->source_count)
		return EIO;

	/* Succeeded: the selected source. */
	*source = &entry->sources[selector];
	return 0;
}

/* Computes a clock's rate, walking at most CRG_MAX_DEPTH steps up. */
static int
clock_rate(
	enum drv_vf2_crg crg,
	uint32_t index,
	unsigned depth,
	uint64_t *rate)
{
	const struct crg_clock *entry;
	const struct crg_source *source;
	volatile uint32_t *address;
	uint64_t parent;
	uint32_t contents;
	uint32_t divider;
	int error;

	/* Refuses a path longer than any real clock tree has. */
	if (depth >= CRG_MAX_DEPTH)
		return ELOOP;

	/* Finds the clock in its table. */
	entry = find_clock(crg, index);
	if (entry == NULL)
		return ENOENT;

	/* Finds the clock's register, refusing one whose generator is absent. */
	address = clock_register(crg, index);
	if (address == NULL)
		return ENODEV;

	/* Finds the source the register selects. */
	contents = kern_mmio_read32(address);
	error = current_source(entry, contents, &source);
	if (error != 0)
		return error;

	/* Computes the source's rate. */
	error = source_rate(source, depth + 1U, &parent);
	if (error != 0)
		return error;

	/* A clock without a divider runs at its source's rate. */
	if ((entry->flags & CLOCK_DIVIDER) == 0) {
		*rate = parent;
		return 0;
	}

	/* Refuses a divider of zero, which the hardware does not define. */
	divider = contents & CRG_DIVIDER_MASK;
	if (divider == 0)
		return EIO;

	/* Succeeded: the source's rate divided down. */
	*rate = parent / divider;
	return 0;
}

/* Computes the rate of one source of a clock. */
static int
source_rate(
	const struct crg_source *source,
	unsigned depth,
	uint64_t *rate)
{
	int error;

	/* Computes the rate the way the source's kind says. */
	switch (source->kind) {
	case SOURCE_CLOCK:
		/* Another clock: walks one step further up. */
		error = clock_rate(source->crg, source->number, depth, rate);
		break;
	case SOURCE_PLL:
		/* A PLL: reads its settings. */
		error = pll_rate(source->number, rate);
		break;
	case SOURCE_INPUT:
		/* A fixed clock the generator's node lists. */
		error = input_rate(source->crg, source->input, rate);
		break;
	default:
		/* A source the table does not know. */
		error = ENOTSUP;
		break;
	}

	/* Reports why the source's rate is unknown. */
	if (error != 0)
		return error;

	/* Succeeded: the source's rate. */
	return 0;
}

/*
 * Computes a PLL's output rate from its settings.
 *
 * In integer mode the output is the reference times the feedback divider,
 * divided by the pre-divider and the post-divider; in fractional mode the
 * 24-bit fraction is added to the feedback divider.  A powered-down PLL
 * runs at zero.
 *
 * Procedure referenced:
 * StarFive JH7110 TRM Preliminary V2, section 2.5.6 (Fvco = Fref x
 * (NI + NF) / M, Fclko1 = Fvco / Q1); OpenBSD sys/dev/fdt/stfclock.c
 * (stfclock_get_frequency_jh7110_pll).
 */
static int
pll_rate(
	uint32_t pll,
	uint64_t *rate)
{
	const struct pll_layout *layout;
	uint64_t reference;
	uint64_t multiplier;
	uint64_t divisor;
	uint32_t mode;
	uint32_t feedback;
	uint32_t fraction;
	uint32_t prediv;
	uint32_t dac_off;
	uint32_t dsm_off;
	uint32_t feedback_divider;
	uint32_t pre_divider;
	uint32_t post_divider;
	int error;

	/* Refuses a PLL number outside the three. */
	if (pll >= PLL_COUNT)
		return EINVAL;
	layout = &pll_layouts[pll];

	/* Reads the register with the integer and fractional mode bits. */
	error = drv_vf2_syscon_read(DRV_VF2_SYSCON_SYS, layout->mode_offset, &mode);
	if (error != 0)
		return error;

	/* Reads the register with the feedback divider. */
	error = drv_vf2_syscon_read(DRV_VF2_SYSCON_SYS, layout->feedback_offset, &feedback);
	if (error != 0)
		return error;

	/* Reads the register with the fraction, the power-down and the post-divider. */
	error = drv_vf2_syscon_read(DRV_VF2_SYSCON_SYS, layout->fraction_offset, &fraction);
	if (error != 0)
		return error;

	/* Reads the register with the pre-divider. */
	error = drv_vf2_syscon_read(DRV_VF2_SYSCON_SYS, layout->prediv_offset, &prediv);
	if (error != 0)
		return error;

	/* A powered-down PLL has no output. */
	if ((fraction & PLL_POWER_DOWN) != 0) {
		*rate = 0;
		return 0;
	}

	/* Decodes the dividers; the post-divider is a power of two. */
	dac_off = (mode >> layout->dac_shift) & 1U;
	dsm_off = (mode >> layout->dsm_shift) & 1U;
	feedback_divider = (feedback >> layout->feedback_shift) & 0xfffU;
	pre_divider = prediv & PLL_PREDIV_MASK;
	post_divider = 1U << ((fraction >> PLL_POSTDIV_SHIFT) & PLL_POSTDIV_MASK);

	/* Refuses dividers the hardware does not define. */
	if (feedback_divider == 0 || pre_divider == 0)
		return EIO;

	/* Refuses a mode that is neither integer nor fractional. */
	if (dac_off != dsm_off)
		return EIO;

	/* Reads the reference the PLLs multiply, the board's oscillator. */
	error = input_rate(DRV_VF2_CRG_SYS, "osc", &reference);
	if (error != 0)
		return error;

	/* Refuses a reference the arithmetic below cannot take. */
	if (reference == 0 || reference > PLL_MAX_REFERENCE)
		return ERANGE;

	/*
	 * Forms the feedback divider in units of 2^-24, adding the fraction
	 * in fractional mode.  The product with the reference stays below
	 * 2^63: the divider is below 2^36 and the reference below 2^27.
	 */
	multiplier = (uint64_t)feedback_divider << PLL_FRACTION_BITS;
	if (dac_off == 0)
		multiplier += fraction & PLL_FRACTION_MASK;
	divisor = ((uint64_t)pre_divider * post_divider) << PLL_FRACTION_BITS;

	/* Succeeded: the PLL's output rate. */
	*rate = reference * multiplier / divisor;
	return 0;
}

/*
 * Reads the rate of a fixed clock the generator's node lists under a name.
 *
 * The board's device tree gives such clocks a clock-frequency property and
 * no cells.
 */
static int
input_rate(
	enum drv_vf2_crg crg,
	const char *input,
	uint64_t *rate)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t provider;
	uint32_t generator_node;
	uint32_t phandle;
	uint32_t cells;
	uint32_t offset;
	unsigned position;
	unsigned entry;
	int error;

	/* Refuses an input of a generator that is absent. */
	if (crg_instances[crg].registers == NULL)
		return ENODEV;
	generator_node = crg_instances[crg].node;

	/* Finds the input's position among the generator's clock names. */
	error = drv_fdt_string_index(&crg_tree, generator_node, "clock-names", input, &position);
	if (error != 0)
		return error;

	/* Reads the generator's clocks, the providers in the same order. */
	error = drv_fdt_property(&crg_tree, generator_node, "clocks", &value, &length);
	if (error != 0)
		return error;

	/* Walks the entries, whose widths their providers give, to the input's. */
	offset = 0;
	entry = 0;
	provider = DRV_FDT_NO_NODE;
	cells = 0;
	for (;;) {
		/* Refuses a list that ends before the input's entry. */
		if (offset + 4U > length)
			return ENOENT;

		/* Finds the entry's provider and how many cells follow its phandle. */
		phandle = (uint32_t)drv_fdt_cells_value(value, offset / 4U, 1U);
		error = drv_fdt_find_phandle(&crg_tree, phandle, &provider);
		if (error != 0)
			return error;
		cells = drv_fdt_node_cells(&crg_tree, provider, "#clock-cells", 0U);

		/* Stops at the input's entry. */
		if (entry == position)
			break;

		/* Moves past the phandle and its cells. */
		offset += (1U + cells) * 4U;
		entry++;
	}

	/* Refuses an input that is not a fixed clock. */
	if (cells != 0)
		return ENOTSUP;

	/* Reads the fixed clock's rate, refusing a provider without one. */
	error = drv_fdt_property(&crg_tree, provider, "clock-frequency", &value, &length);
	if (error != 0)
		return ENOTSUP;
	if (length != 4U)
		return EINVAL;

	/* Succeeded: the fixed clock's rate. */
	*rate = drv_fdt_cells_value(value, 0, 1U);
	return 0;
}

/* Finds which mapped generator a node is. */
static int
generator_of(
	uint32_t node,
	enum drv_vf2_crg *crg)
{
	unsigned candidate;

	/* Compares the node with each mapped generator's. */
	for (candidate = 0; candidate < DRV_VF2_CRG_COUNT; candidate++) {
		/* A generator counts only once it is mapped. */
		if (crg_instances[candidate].registers == NULL)
			continue;

		/* Stops at the generator the node is. */
		if (crg_instances[candidate].node == node)
			break;
	}

	/* Refuses a node that is no generator. */
	if (candidate == DRV_VF2_CRG_COUNT)
		return ENODEV;

	/* Succeeded: the generator's number. */
	*crg = (enum drv_vf2_crg)candidate;
	return 0;
}

/*
 * Decodes one entry of a phandle-and-cells list that must name a
 * generator with one cell.
 *
 * Position counts entries, whose widths their providers give with the
 * cells property named.
 */
static int
lookup_specifier(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *list,
	const char *cells_name,
	unsigned position,
	enum drv_vf2_crg *crg,
	uint32_t *index)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t provider;
	uint32_t phandle;
	uint32_t cells;
	uint32_t cell;
	unsigned entry;
	int error;

	/* Reads the list. */
	error = drv_fdt_property(fdt, node, list, &value, &length);
	if (error != 0)
		return error;
	if ((length & 3U) != 0 || length / 4U > CRG_PROPERTY_CELLS)
		return EINVAL;

	/* Walks the entries to the one asked for. */
	cell = 0;
	entry = 0;
	provider = DRV_FDT_NO_NODE;
	cells = 0;
	for (;;) {
		/* Refuses a list that ends before the entry. */
		if ((cell + 1U) * 4U > length)
			return ENOENT;

		/* Finds the entry's provider and how many cells follow its phandle. */
		phandle = (uint32_t)drv_fdt_cells_value(value, cell, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, &provider);
		if (error != 0)
			return error;
		cells = drv_fdt_node_cells(fdt, provider, cells_name, 0U);

		/* Stops at the entry asked for. */
		if (entry == position)
			break;

		/* Moves past the phandle and its cells. */
		cell += 1U + cells;
		entry++;
	}

	/* Refuses a provider that is not a generator. */
	error = generator_of(provider, crg);
	if (error != 0)
		return error;

	/* Refuses an entry of other than one cell, or one that runs off the list. */
	if (cells != 1U)
		return EINVAL;
	if ((cell + 2U) * 4U > length)
		return EINVAL;

	/* Succeeded: the generator's number of the clock or line. */
	*index = (uint32_t)drv_fdt_cells_value(value, cell + 1U, 1U);
	return 0;
}

/* Decodes the entry of a list that a names property selects. */
static int
lookup_named(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *list,
	const char *names,
	const char *cells_name,
	const char *name,
	enum drv_vf2_crg *crg,
	uint32_t *index)
{
	unsigned position;
	int error;

	/* Finds the name's position; no name means the first entry. */
	position = 0;
	if (name != NULL) {
		error = drv_fdt_string_index(fdt, node, names, name, &position);
		if (error != 0)
			return error;
	}

	/* Decodes the entry at that position. */
	error = lookup_specifier(fdt, node, list, cells_name, position, crg, index);
	if (error != 0)
		return error;

	/* Succeeded: the generator and its number. */
	return 0;
}

/*
 * Moves a reset line and waits until its status follows.
 *
 * Procedure referenced:
 * OpenBSD sys/dev/fdt/stfclock.c (stfclock_reset_jh7110_sys: the assert
 * register and bit of a line).
 */
static int
reset_change(
	const struct drv_vf2_reset *reset,
	int assert)
{
	const struct crg_generator *generator;
	volatile uint32_t *address;
	volatile uint8_t *registers;
	unsigned long irq;
	uint32_t contents;
	uint32_t bit;
	unsigned poll;
	int released;
	int error;

	/* Refuses a missing line or one the generator does not have. */
	if (reset == NULL)
		return EINVAL;
	if ((unsigned)reset->crg >= DRV_VF2_CRG_COUNT)
		return EINVAL;
	generator = &crg_generators[reset->crg];
	if (reset->index >= generator->reset_count)
		return ERANGE;

	/* Refuses a line whose generator is absent. */
	registers = crg_instances[reset->crg].registers;
	if (registers == NULL)
		return ENODEV;

	/* Sets or clears the line's assert bit. */
	address = (volatile uint32_t *)(registers + generator->reset_assert + (reset->index / 32U) * 4U);
	bit = 1U << (reset->index % 32U);
	irq = spin_lock_irqsave(&crg_lock);

	contents = kern_mmio_read32(address);
	if (assert != 0) {
		contents |= bit;
	} else {
		contents &= ~bit;
	}

	/* Writes the register back with the reset line changed. */
	kern_mmio_write32(address, contents);

	spin_unlock_irqrestore(&crg_lock, irq);

	/* Waits for the status to show the new state. */
	for (poll = 0; poll < CRG_RESET_POLLS; poll++) {
		/* Reads the line's status. */
		error = drv_vf2_reset_released(reset, &released);
		if (error != 0)
			return error;

		/* Stops once the status agrees with the request. */
		if (assert != 0 && released == 0)
			break;
		if (assert == 0 && released != 0)
			break;
	}

	/* Reports a status that never followed. */
	if (poll == CRG_RESET_POLLS)
		return ETIMEDOUT;

	/* Succeeded: the line is in the requested state. */
	return 0;
}
