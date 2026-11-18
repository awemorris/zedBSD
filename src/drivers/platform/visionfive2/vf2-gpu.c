/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for its PowerVR GPU core.
 *
 * The core sits in its own outer power domain, runs on five clocks of the
 * SYS clock generator and is held by two of its reset lines.  Bringing it
 * up turns the domain on, starts the bus master clock, then the register
 * bus and timer clocks, sets the core clock's source and rate, starts the
 * core and system clocks, and releases the register bus reset before the
 * core's own.  Bringing it down undoes those steps in reverse.  The bus
 * master's reset line is never touched.
 */

#include "drivers/platform/visionfive2/vf2-gpu.h"
#include "drivers/platform/visionfive2/vf2-crg.h"
#include "drivers/platform/visionfive2/vf2-pmu.h"
#include "drivers/gpu/powervr/powervr.h"
#include <kern/klog.h>
#include <uapi/errno.h>

/*
 * The GPU's clocks and resets in the SYS clock generator.
 *
 * Values referenced:
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_SYSCLK_GPU_CORE 45 to JH7110_SYSCLK_NOC_BUS_GPU_AXI 50) and
 * include/dt-bindings/reset/starfive,jh7110-crg.h (JH7110_SYSRST_GPU_APB
 * 21, JH7110_SYSRST_GPU_DOMA 22).
 */
#define VF2_GPU_CORE_DIVIDER	45U
#define VF2_GPU_CORE_CLOCK	46U
#define VF2_GPU_SYSTEM_CLOCK	47U
#define VF2_GPU_BUS_CLOCK	48U
#define VF2_GPU_TIMER_CLOCK	49U
#define VF2_GPU_MASTER_CLOCK	50U
#define VF2_GPU_BUS_RESET	21U
#define VF2_GPU_CORE_RESET	22U

/*
 * The core clock's source and rate: PLL2, divided by two (594 MHz) or by
 * three (396 MHz, POWERVR_VF2_CORE_594 0).
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/services/system/rogue/sf_7110/sysconfig.h
 * (RGX_STARFIVE_7100_CORE_CLOCK_SPEED 594 MHz, the vendor kernel's rate
 * unless the device tree has the 1.25 GHz operating point, whose binned
 * parts take RGX_STARFIVE_7100_CORE_CLOCK_SPEED_BIN2 396 MHz); StarFive
 * JH7110 TRM Preliminary V2, Table 2-53 Clock GPU Root (PLL2 as selector 0).
 */
#define VF2_GPU_ROOT_CLOCK	3U
#define VF2_GPU_ROOT_PLL	2U
/*
 * POWERVR_VF2_CORE_594 runs the core at the vendor driver's default 594
 * MHz (PLL2 divided by two; RGX_STARFIVE_7100_CORE_CLOCK_SPEED, used
 * unless the device tree has /opp-table-0/opp-1250000000) instead of
 * the 396 MHz of the binned parts.  The firmware is told the rate the
 * clock reads back.  594 MHz is the default: the vendor's Linux on the
 * same board runs it, and the board ran vkmatrix and the desktop at it
 * without a lockup; 0 keeps 396 MHz.
 */
#ifndef POWERVR_VF2_CORE_594
#define POWERVR_VF2_CORE_594	1
#endif
#define VF2_GPU_CORE_RATE	(POWERVR_VF2_CORE_594 ? 594000000ULL : 396000000ULL)

/* The highest address the core is let name until wider ones are proven. */
#define VF2_GPU_DMA_LIMIT	0xffffffffULL

/* Every domain's hardware turn-off event, masked (the vendor kernel's (uint32_t)-1). */
#define VF2_GPU_EVENTS_ALL	0xffffffffU

/*
 * How far the bring-up got, so a failure undoes exactly the steps done.
 *
 * Each stage includes those before it.
 */
enum vf2_gpu_stage {
	VF2_GPU_OFF = 0,
	VF2_GPU_POWERED,
	VF2_GPU_MASTER_RUNNING,
	VF2_GPU_BUS_RUNNING,
	VF2_GPU_TIMER_RUNNING,
	VF2_GPU_CORE_RUNNING,
	VF2_GPU_SYSTEM_RUNNING,
	VF2_GPU_BUS_RELEASED,
	VF2_GPU_RELEASED
};

/*
 * The board's one GPU core's glue state.
 *
 * Stage is changed only by the power steps, which the PowerVR driver
 * calls one at a time.
 */
struct vf2_gpu {
	enum vf2_gpu_stage stage;
};

/* The board's GPU core's glue state; off until the driver powers it. */
static struct vf2_gpu board_gpu;

static int power_up(void *context, uint64_t *core_rate);
static void power_down(void *context);

/*
 * The power steps the PowerVR driver calls.
 *
 * A constant table whose initializer names the two steps, which are
 * declared above it.
 */
static const struct drv_powervr_power vf2_gpu_power = {
	power_up,
	power_down
};

static int find_node(const struct drv_fdt *fdt, uint32_t *node);
static void make_clock(uint32_t index, struct drv_vf2_clock *clock);
static void make_reset(uint32_t index, struct drv_vf2_reset *reset);
static int start_clock(uint32_t index, struct vf2_gpu *gpu, enum vf2_gpu_stage stage);
static void log_clock_rates(void);

/*
 * Powers the GPU core and hands it to the PowerVR driver.
 */
int
drv_vf2_gpu_attach(
	const struct drv_fdt *fdt,
	const struct drv_vf2_gpu_place *builtin)
{
	struct drv_powervr_description description;
	const uint8_t *cells;
	uint32_t length;
	uint32_t node;
	int error;

	/* Refuses a missing tree or description. */
	if (fdt == NULL || builtin == NULL)
		return EINVAL;

	/*
	 * Publishes the screen as the display node first (gpu0); the board
	 * keeps its text console when there is no screen, and the core is
	 * brought up either way.
	 */
	error = drv_powervr_display_attach();
	if (error != 0 && error != ENOENT)
		kern_logf("vf2-gpu: display node unavailable (%d)\n", error);

	/* Maps the power management unit, which turns the core's domain on. */
	error = drv_vf2_pmu_init(fdt);
	if (error != 0) {
		kern_logf("vf2-gpu: no power management unit (%d)\n", error);
		return error;
	}

	/* Starts from the board's own description of the core. */
	description.registers = builtin->registers;
	description.register_size = builtin->register_size;
	description.interrupt = builtin->interrupt;
	description.coherent = 0;
	description.dma_limit = VF2_GPU_DMA_LIMIT;
	description.power = &vf2_gpu_power;
	description.context = &board_gpu;

	/*
	 * Takes the place from a node the tree enables, when there is one.
	 * The coherency follows the node's markings with non-coherent as the
	 * default, not the board's coherent default.
	 */
	error = find_node(fdt, &node);
	if (error == 0) {
		error = drv_fdt_reg(fdt, node, 0, &description.registers, &description.register_size);
		if (error != 0)
			return error;
		error = drv_fdt_property(fdt, node, "interrupts", &cells, &length);
		if (error == 0 && length >= 4U)
			description.interrupt = (uint32_t)drv_fdt_cells_value(cells, 0, 1U);
		description.coherent = drv_fdt_dma_coherent(fdt, node, 0);
	}

	/* Hands the core to the driver, which powers it through the steps below. */
	error = drv_powervr_attach(&description);
	if (error != 0)
		return error;

	/* Succeeded: the core is up. */
	return 0;
}

/*
 * Powers the core's domain, starts its clocks and releases its resets.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/services/system/rogue/sf_7110/sysconfig.c
 * (sys_gpu_runtime_resume, u0_img_gpu_enable: the bus master clock, then
 * the register bus and timer clocks, the core clock's rate, the core and
 * system clocks, and the register bus reset before the core's).
 */
static int
power_up(
	void *context,
	uint64_t *core_rate)
{
	struct drv_vf2_clock clock;
	struct drv_vf2_reset reset;
	struct vf2_gpu *gpu;
	int error;

	/* Refuses a core already up. */
	gpu = context;
	if (gpu->stage != VF2_GPU_OFF)
		return EBUSY;

	/* Turns the core's outer domain on. */
	error = drv_vf2_pmu_set_power(DRV_VF2_PD_GPU, 1);
	if (error != 0)
		goto fail;
	gpu->stage = VF2_GPU_POWERED;

	/* Lets the hardware events turn domains off again, as the vendor kernel does before the GPU's clocks start. */
	error = drv_vf2_pmu_event_off_mask(0);
	if (error != 0)
		goto fail;

	/* Starts the bus master clock, then the register bus and timer clocks. */
	error = start_clock(VF2_GPU_MASTER_CLOCK, gpu, VF2_GPU_MASTER_RUNNING);
	if (error != 0)
		goto fail;
	error = start_clock(VF2_GPU_BUS_CLOCK, gpu, VF2_GPU_BUS_RUNNING);
	if (error != 0)
		goto fail;
	error = start_clock(VF2_GPU_TIMER_CLOCK, gpu, VF2_GPU_TIMER_RUNNING);
	if (error != 0)
		goto fail;

	/* Runs the core clock from PLL2 at its rate. */
	make_clock(VF2_GPU_ROOT_CLOCK, &clock);
	error = drv_vf2_clock_set_pll(&clock, VF2_GPU_ROOT_PLL);
	if (error != 0)
		goto fail;
	make_clock(VF2_GPU_CORE_DIVIDER, &clock);
	error = drv_vf2_clock_set_rate(&clock, VF2_GPU_CORE_RATE);
	if (error != 0)
		goto fail;

	/* Starts the core and system clocks. */
	error = start_clock(VF2_GPU_CORE_CLOCK, gpu, VF2_GPU_CORE_RUNNING);
	if (error != 0)
		goto fail;
	error = start_clock(VF2_GPU_SYSTEM_CLOCK, gpu, VF2_GPU_SYSTEM_RUNNING);
	if (error != 0)
		goto fail;

	/* Releases the register bus reset, then the core's. */
	make_reset(VF2_GPU_BUS_RESET, &reset);
	error = drv_vf2_reset_deassert(&reset);
	if (error != 0)
		goto fail;
	gpu->stage = VF2_GPU_BUS_RELEASED;
	make_reset(VF2_GPU_CORE_RESET, &reset);
	error = drv_vf2_reset_deassert(&reset);
	if (error != 0)
		goto fail;
	gpu->stage = VF2_GPU_RELEASED;

	/* Reports the rate the core clock really runs at. */
	make_clock(VF2_GPU_CORE_DIVIDER, &clock);
	error = drv_vf2_clock_get_rate(&clock, core_rate);
	if (error != 0)
		goto fail;

	/* Logs the rate of every clock the core runs on, to see which is stopped; a release image does not. */
	if (POWERVR_DEBUG)
		log_clock_rates();

	/* Succeeded: the core is powered, clocked and out of reset. */
	return 0;

fail:
	/* Undoes the steps done and reports the failure. */
	power_down(gpu);
	return error;
}

/*
 * Logs the rate of each clock the core runs on and of its PLL, one line
 * each; a divider of zero or an unknown source shows as an error.  A
 * diagnostic for the board: the GPU's system clock comes from the ISP's
 * AXI clock, which nothing else on this board starts.
 */
static void
log_clock_rates(void)
{
	static const struct { uint32_t index; const char *name; } clocks[] = {
		{ 20U, "isp_2x" }, { 21U, "isp_axi" }, { 45U, "gpu_core" }, { 46U, "gpu_core_clk" },
		{ 47U, "gpu_sys_clk" }, { 48U, "gpu_apb" }, { 49U, "gpu_rtc_toggle" }, { 50U, "noc_bus_gpu_axi" },
	};
	struct drv_vf2_clock clock;
	uint64_t rate;
	unsigned i;
	int error;

	/* Each clock, with its rate or the error. */
	for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		make_clock(clocks[i].index, &clock);
		rate = 0;
		error = drv_vf2_clock_get_rate(&clock, &rate);
		kern_logf("vf2-gpu: clock %u %s: %llu Hz (error %d)\n", (unsigned)clocks[i].index, clocks[i].name, (unsigned long long)rate, error);
	}

	/* The PLL the core runs from. */
	error = drv_vf2_pll_get_rate(VF2_GPU_ROOT_PLL, &rate);
	kern_logf("vf2-gpu: pll %u: %llu Hz (error %d)\n", (unsigned)VF2_GPU_ROOT_PLL, (unsigned long long)rate, error);
}

/*
 * Holds the core's resets, stops its clocks in reverse and turns its
 * domain off, undoing exactly the steps power_up() got through.
 */
static void
power_down(
	void *context)
{
	struct drv_vf2_clock clock;
	struct drv_vf2_reset reset;
	struct vf2_gpu *gpu;

	/* Holds the core's reset, then the register bus's. */
	gpu = context;
	if (gpu->stage >= VF2_GPU_RELEASED) {
		make_reset(VF2_GPU_CORE_RESET, &reset);
		(void)drv_vf2_reset_assert(&reset);
	}

	if (gpu->stage >= VF2_GPU_BUS_RELEASED) {
		make_reset(VF2_GPU_BUS_RESET, &reset);
		(void)drv_vf2_reset_assert(&reset);
	}

	/* Stops the system, core, timer, register bus and bus master clocks. */
	if (gpu->stage >= VF2_GPU_SYSTEM_RUNNING) {
		make_clock(VF2_GPU_SYSTEM_CLOCK, &clock);
		(void)drv_vf2_clock_disable(&clock);
	}

	if (gpu->stage >= VF2_GPU_CORE_RUNNING) {
		make_clock(VF2_GPU_CORE_CLOCK, &clock);
		(void)drv_vf2_clock_disable(&clock);
	}

	if (gpu->stage >= VF2_GPU_TIMER_RUNNING) {
		make_clock(VF2_GPU_TIMER_CLOCK, &clock);
		(void)drv_vf2_clock_disable(&clock);
	}

	if (gpu->stage >= VF2_GPU_BUS_RUNNING) {
		make_clock(VF2_GPU_BUS_CLOCK, &clock);
		(void)drv_vf2_clock_disable(&clock);
	}

	if (gpu->stage >= VF2_GPU_MASTER_RUNNING) {
		make_clock(VF2_GPU_MASTER_CLOCK, &clock);
		(void)drv_vf2_clock_disable(&clock);
	}

	/* Masks every hardware turn-off event again, as the vendor kernel does once the clocks stop. */
	if (gpu->stage >= VF2_GPU_POWERED)
		(void)drv_vf2_pmu_event_off_mask(VF2_GPU_EVENTS_ALL);

	/* Turns the core's outer domain off. */
	if (gpu->stage >= VF2_GPU_POWERED)
		(void)drv_vf2_pmu_set_power(DRV_VF2_PD_GPU, 0);

	/* The core is off. */
	gpu->stage = VF2_GPU_OFF;
}

/*
 * Finds a GPU node the device tree enables.
 *
 * Mainline-style compatibles come first, then the vendor kernel's, whose
 * tree ships the node disabled, so only an enabled node counts.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_drv.c (the "img,img-rogue"
 * compatible); StarFive Linux JH7110_VisionFive2_6.12.y_devel
 * 4cecf169f38e arch/riscv/boot/dts/starfive/jh7110.dtsi (gpu@18000000,
 * compatible "img-gpu").
 */
static int
find_node(
	const struct drv_fdt *fdt,
	uint32_t *node)
{
	static const char *const compatibles[] = {
		"img,img-bxe-4-32",
		"img,img-rogue",
		"img-gpu"
	};
	unsigned position;
	uint32_t found;
	bool enabled;
	int error;

	/* Tries each compatible in order of preference. */
	for (position = 0; position < sizeof(compatibles) / sizeof(compatibles[0]); position++) {
		/* Takes the first node of the compatible, if the tree enables it. */
		error = drv_fdt_find_compatible(fdt, compatibles[position], DRV_FDT_NO_NODE, &found);
		if (error != 0)
			continue;
		enabled = drv_fdt_node_enabled(fdt, found);
		if (!enabled)
			continue;

		/* Succeeded: an enabled node describes the core. */
		*node = found;
		return 0;
	}

	/* The tree describes no enabled core. */
	return ENOENT;
}

/* Names one of the GPU's clocks in the SYS generator. */
static void
make_clock(
	uint32_t index,
	struct drv_vf2_clock *clock)
{
	/* The SYS generator's clock of that number. */
	clock->crg = DRV_VF2_CRG_SYS;
	clock->index = index;
}

/* Names one of the GPU's reset lines in the SYS generator. */
static void
make_reset(
	uint32_t index,
	struct drv_vf2_reset *reset)
{
	/* The SYS generator's line of that number. */
	reset->crg = DRV_VF2_CRG_SYS;
	reset->index = index;
}

/* Starts one of the GPU's clocks and records the stage it reaches. */
static int
start_clock(
	uint32_t index,
	struct vf2_gpu *gpu,
	enum vf2_gpu_stage stage)
{
	struct drv_vf2_clock clock;
	int error;

	/* Opens the clock's gate and those above it. */
	make_clock(index, &clock);
	error = drv_vf2_clock_enable(&clock);
	if (error != 0)
		return error;

	/* Succeeded: the bring-up has reached the stage. */
	gpu->stage = stage;
	return 0;
}
