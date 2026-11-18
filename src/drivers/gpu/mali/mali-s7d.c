/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Amlogic S7D's steps around its Mali GPU: the bus clock's gate, the
 * GPU's two core clocks and its two resets.
 *
 * Each core clock has two identical paths behind a glitch-free mux; the
 * driver sets the first path to its rate and selects it, and leaves the
 * second alone.  A path whose source, divider and gate already say what
 * the driver wants is not touched, so a clock the bootloader set is kept
 * running.  The resets are level resets held while their bit is zero;
 * the driver only releases them (the vendor's kernel does not pulse them
 * on this SoC), and the GPU's own soft reset follows.  The vendor's
 * kernel also writes one power override value once the GPU is out of
 * reset, which the driver repeats before its soft reset.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-private.h"
#include "drivers/gpu/mali/mali-regs.h"

/*
 * The clock controller's registers the GPU needs: the bus gates, and the
 * two core clocks' control words.  The control word holds the first
 * path's source [11:9], divider minus one [6:0] and gate bit 8, the
 * second path's in [27:25], [22:16] and bit 24, and the final mux in bit
 * 31 (zero selects the first path).  Source 6 is fclk_div5, 400 MHz.
 *
 * Values referenced:
 * hardkernel/kernel_common_drivers odroids7d-6.12.y 34daba9
 * drivers/clk/meson/s7d.h (CLKCTRL_SYS_CLK_EN0_REG0,
 * CLKCTRL_MALI_CLK_CTRL, CLKCTRL_MALI_STACK_CLK_CTRL);
 * drivers/clk/meson/s7d.c (sys_mali bit 6, mali_0_mux, mali_0_div,
 * mali_0, mali, mali_stack_0_mux, mali_stack_0_div, mali_stack_0,
 * mali_stack and the order of mali_01_parent_data);
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (the GPU node's assigned
 * clock parents and the 400 MHz rate).
 */
#define CLOCK_GATE0			0x044U
#define CLOCK_GATE0_MALI		0x00000040U
#define CLOCK_MALI			0x15cU
#define CLOCK_MALI_STACK		0x160U
#define CLOCK_SOURCE_SHIFT		9U
#define CLOCK_SOURCE_MASK		0x7U
#define CLOCK_SOURCE_FCLK_DIV5		6U
#define CLOCK_DIVIDER_MASK		0x7fU
#define CLOCK_GATE			0x00000100U
#define CLOCK_SECOND_PATH		0x80000000U
#define CLOCK_WINDOW_BYTES		0x1000U
#define CLOCK_RATE			400000000ULL

/*
 * The reset controller's level registers: four bytes for each 32 resets,
 * a reset held while its bit is zero.  The GPU's core reset is 34, its
 * bus bridge's 33.
 *
 * Values referenced:
 * Linux v7.3 drivers/reset/amlogic/reset-meson.c (meson_s4_param,
 * level_offset 0x40, level_low_reset); hardkernel/kernel_common_drivers
 * odroids7d-6.12.y 34daba9 include/dt-bindings/reset/
 * amlogic,meson-s7d-reset.h (RESET_MALI_CAPB3, RESET_MALI).
 */
#define RESET_LEVEL_BASE		0x040U
#define RESET_GPU			34U
#define RESET_GPU_BUS			33U
#define RESET_WINDOW_BYTES		0x1000U

/*
 * The power override the vendor's kernel writes once the GPU is out of
 * reset.
 *
 * Value referenced:
 * hardkernel/kernel_common_drivers odroids7d-6.12.y 34daba9
 * drivers/gpu/arm/midgard/platform/devicetree/mali_kbase_runtime_pm.c
 * (mali_hw_init).
 */
#define POWER_OVERRIDE1_VALUE		(0xfffU | (0x20U << 16))

/* How long a released reset settles before the GPU is touched (10 to 20 µs). */
#define RESET_SETTLE_MIN_US		10U
#define RESET_SETTLE_MAX_US		20U

/*
 * The SoC's windows the steps write.
 *
 * Mapped once by mali_s7d_glue() and kept for the life of the system.
 */
struct s7d_glue {
	volatile uint8_t *clocks;
	volatile uint8_t *resets;
};

/*
 * The board's one glue, filled by mali_s7d_glue() on the boot CPU.
 */
static struct s7d_glue board_glue;

static int power_up(void *context, uint64_t *core_rate);
static void registers_ready(void *context, struct mali_device *device);
static void power_down(void *context);
static int map_window(const struct drv_fdt *fdt, uint32_t node, unsigned index, size_t bytes, volatile uint8_t **view);
static void set_clock(volatile uint8_t *clocks, uint32_t offset);
static int release_reset(volatile uint8_t *resets, unsigned reset);
static void hold_reset(volatile uint8_t *resets, unsigned reset);

/*
 * Finds the S7D's clock and reset controllers and fills the GPU's glue.
 *
 * The reset controller is the GPU node's second register window, as the
 * vendor's tree gives it, or the SoC's reset controller node.  ENODEV
 * when the tree names neither controller.
 */
int
mali_s7d_glue(
	const struct drv_fdt *fdt,
	uint32_t gpu_node,
	struct mali_glue *glue)
{
	struct s7d_glue *s7d;
	uint32_t node;
	int error;

	/* The clock controller. */
	s7d = &board_glue;
	error = drv_fdt_find_compatible(fdt, "amlogic,s7d-clkc", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;

	error = map_window(fdt, node, 0U, CLOCK_WINDOW_BYTES, &s7d->clocks);
	if (error != 0)
		return error;

	/* The reset window: the GPU node's second, else the reset controller's own. */
	error = map_window(fdt, gpu_node, 1U, RESET_WINDOW_BYTES, &s7d->resets);
	if (error != 0) {
		error = drv_fdt_find_compatible(fdt, "amlogic,meson-sc2-reset", DRV_FDT_NO_NODE, &node);
		if (error != 0)
			return ENODEV;

		error = map_window(fdt, node, 0U, RESET_WINDOW_BYTES, &s7d->resets);
		if (error != 0)
			return error;
	}

	/* The steps. */
	glue->power_up = power_up;
	glue->registers_ready = registers_ready;
	glue->power_down = power_down;
	glue->context = s7d;

	/* Succeeded: the GPU can be powered. */
	return 0;
}

/*
 * Opens the bus gate, sets both core clocks to 400 MHz from fclk_div5 and
 * releases the two resets.
 */
static int
power_up(
	void *context,
	uint64_t *core_rate)
{
	struct s7d_glue *s7d;
	uint32_t gates;
	int released;

	/* The GPU's registers need the bus clock before anything. */
	s7d = context;
	gates = kern_mmio_read32((const volatile void *)(s7d->clocks + CLOCK_GATE0));
	if ((gates & CLOCK_GATE0_MALI) == 0U)
		kern_mmio_write32((volatile void *)(s7d->clocks + CLOCK_GATE0), gates | CLOCK_GATE0_MALI);

	/* The core clock and the shader stack's clock. */
	set_clock(s7d->clocks, CLOCK_MALI);
	set_clock(s7d->clocks, CLOCK_MALI_STACK);

	/* The bus bridge out of reset first, then the core. */
	released = release_reset(s7d->resets, RESET_GPU_BUS);
	released |= release_reset(s7d->resets, RESET_GPU);
	if (released)
		kern_usleep_range(RESET_SETTLE_MIN_US, RESET_SETTLE_MAX_US);

	/* Says what the steps found, for the first look at the board. */
	kern_logf("mali: s7d clocks %08x %08x gate %08x%s\n",
		  (unsigned)kern_mmio_read32((const volatile void *)(s7d->clocks + CLOCK_MALI)),
		  (unsigned)kern_mmio_read32((const volatile void *)(s7d->clocks + CLOCK_MALI_STACK)),
		  (unsigned)kern_mmio_read32((const volatile void *)(s7d->clocks + CLOCK_GATE0)),
		  released ? ", resets released" : "");

	/* Succeeded: the GPU is clocked and out of reset. */
	*core_rate = CLOCK_RATE;
	return 0;
}

/* Writes the vendor's power override, behind the power manager's key. */
static void
registers_ready(
	void *context,
	struct mali_device *device)
{
	(void)context;

	/* The key lets the override take effect. */
	mali_write32(device, MALI_REG_POWER_KEY, MALI_POWER_KEY);
	mali_write32(device, MALI_REG_POWER_OVERRIDE1, POWER_OVERRIDE1_VALUE);
}

/* Holds the GPU in reset again, the core first. */
static void
power_down(
	void *context)
{
	struct s7d_glue *s7d;

	/* The core, then its bus bridge. */
	s7d = context;
	hold_reset(s7d->resets, RESET_GPU);
	hold_reset(s7d->resets, RESET_GPU_BUS);
}

/* Maps one register window of a node, of at least bytes. */
static int
map_window(
	const struct drv_fdt *fdt,
	uint32_t node,
	unsigned index,
	size_t bytes,
	volatile uint8_t **view)
{
	uint64_t address;
	uint64_t size;
	void *mapped;
	int error;

	/* The window's place in the processor's space. */
	error = drv_fdt_reg(fdt, node, index, &address, &size);
	if (error != 0)
		return error;

	/* A page-aligned window; the tree's size may be smaller than the page mapped. */
	if ((address & (MALI_PAGE_BYTES - 1U)) != 0U)
		return EINVAL;

	error = kern_device_map(address, bytes, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the window is mapped for the life of the system. */
	*view = mapped;
	return 0;
}

/*
 * Sets the first path of a core clock to fclk_div5 undivided and selects
 * it.  The source and divider change only while the path's gate is
 * closed, as the controller requires.
 */
static void
set_clock(
	volatile uint8_t *clocks,
	uint32_t offset)
{
	uint32_t control;
	uint32_t wanted;
	uint32_t source;

	/* The path is already what the driver wants: only the final mux may need to move. */
	control = kern_mmio_read32((const volatile void *)(clocks + offset));
	source = (control >> CLOCK_SOURCE_SHIFT) & CLOCK_SOURCE_MASK;
	if (source == CLOCK_SOURCE_FCLK_DIV5 &&
	    (control & CLOCK_DIVIDER_MASK) == 0U &&
	    (control & CLOCK_GATE) != 0U) {
		if ((control & CLOCK_SECOND_PATH) != 0U)
			kern_mmio_write32((volatile void *)(clocks + offset), control & ~CLOCK_SECOND_PATH);
		return;
	}

	/* Closes the first path's gate, then sets its source and divider. */
	wanted = control & ~CLOCK_GATE;
	kern_mmio_write32((volatile void *)(clocks + offset), wanted);
	wanted &= ~((CLOCK_SOURCE_MASK << CLOCK_SOURCE_SHIFT) | CLOCK_DIVIDER_MASK);
	wanted |= CLOCK_SOURCE_FCLK_DIV5 << CLOCK_SOURCE_SHIFT;
	kern_mmio_write32((volatile void *)(clocks + offset), wanted);

	/* Opens the gate, then selects the first path. */
	wanted |= CLOCK_GATE;
	kern_mmio_write32((volatile void *)(clocks + offset), wanted);
	wanted &= ~CLOCK_SECOND_PATH;
	kern_mmio_write32((volatile void *)(clocks + offset), wanted);
}

/* Releases one level reset; nonzero when it was held. */
static int
release_reset(
	volatile uint8_t *resets,
	unsigned reset)
{
	volatile uint8_t *level;
	uint32_t bit;
	uint32_t value;

	/* The reset's word and bit. */
	level = resets + RESET_LEVEL_BASE + 4U * (reset / 32U);
	bit = 1U << (reset % 32U);

	/* A set bit is a released reset. */
	value = kern_mmio_read32((const volatile void *)level);
	if ((value & bit) != 0U)
		return 0;

	kern_mmio_write32((volatile void *)level, value | bit);

	/* Succeeded: the reset was held and is now released. */
	return 1;
}

/* Holds one level reset. */
static void
hold_reset(
	volatile uint8_t *resets,
	unsigned reset)
{
	volatile uint8_t *level;
	uint32_t bit;
	uint32_t value;

	/* The reset's word and bit, cleared. */
	level = resets + RESET_LEVEL_BASE + 4U * (reset / 32U);
	bit = 1U << (reset % 32U);
	value = kern_mmio_read32((const volatile void *)level);
	kern_mmio_write32((volatile void *)level, value & ~bit);
}
