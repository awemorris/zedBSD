/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's power management unit.
 *
 * Software switches a domain in two steps: it names the domain in the
 * turn-on or turn-off mode register, then writes a fixed three-byte
 * command to the encourage register.  The current power mode register
 * shows the outcome.  Hardware events can switch domains too: the GPU's
 * power controller asks for its shader cluster's power through the GPU's
 * event, which therefore stays unmasked while the core runs (the GPU's
 * driver masks the turn-off events while the core is off).  The
 * display's domain has no hardware event, so only software switches it.
 */

#include "drivers/platform/visionfive2/vf2-pmu.h"
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/*
 * The unit's registers and the software command bytes.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, section 3.6 PMU (Tables 3-12 to
 * 3-14, 3-19 and 3-26: the hardware event turn-on mask at 0x4 with the
 * GPU event in bit 7, the software turn-on and turn-off modes at 0xc and
 * 0x10, the software encourage at 0x44 and the current power mode at
 * 0x80; the command sequences 0xff, 0x05, 0x50 to turn on and 0xff, 0x0a,
 * 0xa0 to turn off); StarFive Linux JH7110_VisionFive2_6.12.y_devel
 * 4cecf169f38e drivers/pmdomain/starfive/jh71xx-pmu.c
 * (JH71XX_PMU_HW_EVENT_TURN_OFF 0x08, the turn-off mask the TRM does not
 * list).
 */
#define PMU_EVENT_ON_MASK	0x04U
#define PMU_EVENT_OFF_MASK	0x08U
#define PMU_SOFT_ON		0x0cU
#define PMU_SOFT_OFF		0x10U
#define PMU_ENCOURAGE		0x44U
#define PMU_POWER_STATE		0x80U
#define PMU_EVENT_GPU		0x80U
#define PMU_COMMAND_RESET	0xffU
#define PMU_COMMAND_ON_LOW	0x05U
#define PMU_COMMAND_ON_HIGH	0x50U
#define PMU_COMMAND_OFF_LOW	0x0aU
#define PMU_COMMAND_OFF_HIGH	0xa0U

/* The bytes of the unit's register block that are mapped. */
#define PMU_BLOCK_SIZE		0x1000U

/* How many state reads a switch waits before giving up. */
#define PMU_POLLS		100000U

/*
 * The unit's registers in the kernel's address space.
 *
 * Written only by drv_vf2_pmu_init(), which the board runs once on the
 * boot CPU before any driver attaches; NULL while the unit is absent.
 */
static volatile uint8_t *pmu_registers;

/*
 * Serializes the two-step command, whose second step must follow its own
 * first step.  A leaf lock, taken with interrupts held off.
 */
static struct spinlock pmu_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "vf2 pmu", 0, 0
};

static volatile uint32_t *pmu_register(uint32_t offset);

/*
 * Finds the power management unit in the device tree and maps it.
 */
int
drv_vf2_pmu_init(
	const struct drv_fdt *fdt)
{
	uint64_t address;
	uint64_t size;
	uint32_t node;
	void *mapped;
	int error;

	/* Refuses a missing tree. */
	if (fdt == NULL)
		return EINVAL;

	/*
	 * Finds the unit's node.
	 *
	 * Value referenced:
	 * Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi (pwrc,
	 * compatible "starfive,jh7110-pmu").
	 */
	error = drv_fdt_find_compatible(fdt, "starfive,jh7110-pmu", DRV_FDT_NO_NODE, &node);
	if (error != 0)
		return ENODEV;

	/* Reads where the registers are; the block must hold every register used. */
	error = drv_fdt_reg(fdt, node, 0, &address, &size);
	if (error != 0)
		return error;
	if (size < PMU_BLOCK_SIZE)
		return EINVAL;

	/* Maps the registers as device memory. */
	error = kern_device_map(address, PMU_BLOCK_SIZE, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Succeeded: the unit can be driven. */
	pmu_registers = mapped;
	return 0;
}

/*
 * Turns a power domain on or off and waits until the unit reports it so.
 */
int
drv_vf2_pmu_set_power(
	unsigned domain,
	int on)
{
	unsigned long irq;
	uint32_t mask;
	uint32_t state;
	uint32_t before;
	uint32_t events;
	unsigned poll;

	/*
	 * Refuses every domain but the GPU's, the display's and the video
	 * codecs', which their own drivers own.
	 */
	if (domain != DRV_VF2_PD_GPU &&
	    domain != DRV_VF2_PD_VOUT &&
	    domain != DRV_VF2_PD_VDEC &&
	    domain != DRV_VF2_PD_VENC)
		return EPERM;

	/* Refuses the call before the unit is mapped. */
	if (pmu_registers == NULL)
		return ENODEV;

	/*
	 * Names the domain and writes the command (the GPU's hardware events are
	 * handled below): the reset byte
	 * restarts the unit's command parser, the two bytes after it select
	 * turning on or off.  The display's and the video codecs' domains are
	 * switched by software alone, so no event is unmasked for them.
	 *
	 * Procedure referenced:
	 * StarFive JH7110 TRM Preliminary V2, section 3.6.4 (SW encourage
	 * turn-on and turn-off sequences) and Tables 3-12 and 3-13 (the VOUT
	 * domain in bit 4 of the software modes); StarFive Linux
	 * JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
	 * drivers/pmdomain/starfive/jh71xx-pmu.c (jh7110_power_domains: VDEC
	 * in bit 3, VENC in bit 6).
	 */
	mask = 1U << domain;
	irq = spin_lock_irqsave(&pmu_lock);
	before = kern_mmio_read32(pmu_register(PMU_POWER_STATE));

	/*
	 * The GPU's hardware event must stay unmasked while the core runs:
	 * the core's power controller asks the unit through it for the power
	 * of its shader cluster (the Rascal and the dusts), and a masked
	 * event makes every such request abort, so no job ever runs.  The
	 * vendor kernel never masks it; the turn-off mask is set by the GPU's
	 * driver around the clocks (drv_vf2_pmu_event_off_mask).
	 *
	 * Procedure referenced:
	 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
	 * drivers/gpu/drm/img/img-rogue/services/system/rogue/sf_7110/
	 * sysconfig.c (sys_gpu_runtime_resume: only the turn-off mask is
	 * written); the firmware's trace on the board, 2026-10-20 ("HW
	 * Request On(1) Units 0x01000703", "Aborted", "Power controller
	 * returned ABORT for last request so retrying" while the event was
	 * masked).
	 */
	if (domain == DRV_VF2_PD_GPU) {
		events = kern_mmio_read32(pmu_register(PMU_EVENT_ON_MASK));
		kern_mmio_write32(pmu_register(PMU_EVENT_ON_MASK), events & ~PMU_EVENT_GPU);
	}
	if (on) {
		kern_mmio_write32(pmu_register(PMU_SOFT_ON), mask);
		kern_mmio_write32(pmu_register(PMU_ENCOURAGE), PMU_COMMAND_RESET);
		kern_mmio_write32(pmu_register(PMU_ENCOURAGE), PMU_COMMAND_ON_LOW);
		kern_mmio_write32(pmu_register(PMU_ENCOURAGE), PMU_COMMAND_ON_HIGH);
	} else {
		kern_mmio_write32(pmu_register(PMU_SOFT_OFF), mask);
		kern_mmio_write32(pmu_register(PMU_ENCOURAGE), PMU_COMMAND_RESET);
		kern_mmio_write32(pmu_register(PMU_ENCOURAGE), PMU_COMMAND_OFF_LOW);
		kern_mmio_write32(pmu_register(PMU_ENCOURAGE), PMU_COMMAND_OFF_HIGH);
	}

	spin_unlock_irqrestore(&pmu_lock, irq);

	/* Waits for the domain's state bit to follow. */
	for (poll = 0; poll < PMU_POLLS; poll++) {
		/* Stops once the state matches the request. */
		state = kern_mmio_read32(pmu_register(PMU_POWER_STATE));
		if ((on && (state & mask) != 0) || (!on && (state & mask) == 0)) {
			/*
			 * Records every domain's state around the switch, so a
			 * switch that dropped another domain shows on the screen.
			 */
			kern_logf("vf2-pmu: domain %u %s, state %x -> %x\n", domain, on ? "on" : "off", before, state);
			return 0;
		}
	}

	/* Failed: the unit did not switch the domain. */
	kern_logf("vf2-pmu: domain %u %s timed out, state %x -> %x\n", domain, on ? "on" : "off", before, state);
	return ETIMEDOUT;
}

/*
 * Reports whether a power domain is on.
 */
int
drv_vf2_pmu_powered(
	unsigned domain,
	int *on)
{
	uint32_t state;

	/* Refuses a missing result or a domain the register does not hold. */
	if (on == NULL || domain >= 32U)
		return EINVAL;

	/* Refuses the call before the unit is mapped. */
	if (pmu_registers == NULL)
		return ENODEV;

	/* Reads the domain's bit of the current power mode. */
	state = kern_mmio_read32(pmu_register(PMU_POWER_STATE));
	*on = 0;
	if ((state & (1U << domain)) != 0)
		*on = 1;

	/* Succeeded: the domain's state. */
	return 0;
}

/* Finds the kernel address of one of the unit's registers. */
static volatile uint32_t *
pmu_register(
	uint32_t offset)
{
	/* The registers are 32 bits wide at their byte offsets. */
	return (volatile uint32_t *)(pmu_registers + offset);
}

/*
 * Sets the hardware event turn-off mask: a set bit keeps that domain's
 * hardware event from turning its domain off.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/pmdomain/starfive/jh71xx-pmu.c
 * (starfive_pmu_hw_event_turn_off_mask: the mask written whole) and
 * drivers/gpu/drm/img/img-rogue/services/system/rogue/sf_7110/sysconfig.c
 * (sys_gpu_runtime_resume: 0 before the GPU's clocks start;
 * sys_gpu_runtime_suspend: all ones after they stop).
 */
int
drv_vf2_pmu_event_off_mask(
	uint32_t mask)
{
	unsigned long irq;

	/* Refuses the call before the unit is mapped. */
	if (pmu_registers == NULL)
		return ENODEV;

	/* Writes the mask whole, as the vendor kernel does. */
	irq = spin_lock_irqsave(&pmu_lock);
	kern_mmio_write32(pmu_register(PMU_EVENT_OFF_MASK), mask);
	spin_unlock_irqrestore(&pmu_lock, irq);

	/* Succeeded: the mask is set. */
	return 0;
}
