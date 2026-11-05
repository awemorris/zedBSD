/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The V3D 7.1 render engine of the BCM2712: discovery (stage V0) and power
 * (stage V1).
 *
 * The engine sits in a power domain of the power manager (PM), which the
 * driver releases from reset by one bit, and runs from firmware clock 5.
 * 7.1 adds a power sequencer (SMS) in front of the engine, which has to be
 * brought out of its power-off state and reset before the engine's
 * registers answer.  Touching the engine's registers before then can stop
 * the processor, so discovery only reads the device tree, maps the windows,
 * installs the (masked) interrupt handlers and reads the power manager,
 * which is always on.
 *
 * Values referenced: Linux v6.19 drivers/pmdomain/bcm/bcm2835-power.c
 * lines 80-101 (the 2712 graphics register 0x304, the V3D reset bit 6 and
 * the password), 288-391 (a 2712 domain has no AXI bridge and no clock of
 * its own); drivers/gpu/drm/v3d/v3d_drv.c lines 290-318 and v3d_gem.c lines
 * 108-122, v3d_regs.h lines 518-542 (the sequencer's registers, states and
 * the order of power-up and reset).
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/v3d71-private.h"
#include "drivers/platform/rpi5/rpi5-serror.h"

/* The device tree names of the engine and of the power manager (binding facts). */
#define ENGINE_COMPATIBLE		"brcm,2712-v3d"
#define POWER_COMPATIBLE		"brcm,bcm2712-pm"

/* The interrupts in the order the node lists them: the hub's, then the core's. */
#define ENGINE_IRQ_HUB			0U
#define ENGINE_IRQ_CORE			1U

/* The power manager's domain and reset numbers of the engine (binding facts). */
#define POWER_DOMAIN_ENGINE		1U
#define POWER_RESET_ENGINE		0U

/*
 * The power manager's registers: the 2711-style graphics register, which
 * is only read here, and the 2712's graphics register with the engine's
 * reset-release bit.  Every write carries the password.
 */
#define POWER_GRAPHICS_LEGACY		0x10cU
#define POWER_GRAPHICS			0x304U
#define POWER_ENGINE_RELEASE		0x40U
#define POWER_WRITE_KEY			0x5a000000U

/* The firmware's clock number of the engine and its clock tags. */
#define ENGINE_CLOCK			5U
#define CLOCK_TAG_STATE			0x00030001U
#define CLOCK_TAG_RATE			0x00030002U
#define CLOCK_TAG_MAX_RATE		0x00030004U
#define CLOCK_TAG_SET_STATE		0x00038001U
#define CLOCK_TAG_SET_RATE		0x00038002U
#define CLOCK_RUNNING			0x1U
#define CLOCK_ABSENT			0x2U

/*
 * The sequencer's registers: the REE control and status at 0x000 and the
 * TEE control and status at 0x400.  The low four bits of either are the
 * state; writing bit 29 of the TEE word clears a power-off request, and
 * writing state 4 to the REE word asks for a reset.
 */
#define SEQUENCER_REE			0x000U
#define SEQUENCER_TEE			0x400U
#define SEQUENCER_STATE_MASK		0x0fU
#define SEQUENCER_CLEAR_POWER_OFF	0x20000000U
#define SEQUENCER_RESET_REQUEST		0x4U
#define SEQUENCER_IDLE			0x0U
#define SEQUENCER_ISOLATING		0xaU
#define SEQUENCER_RESETTING		0xbU

/* How long a sequencer step may take: 100 ms, in 100 us polls. */
#define SEQUENCER_POLL_US		100U
#define SEQUENCER_POLLS			1000U

/* The smallest windows the driver reads into: the hub to the MMU's last register, the core to compute. */
#define ENGINE_HUB_MINIMUM		0x123cU
#define ENGINE_CORE_MINIMUM		0x980U
#define ENGINE_SEQUENCER_MINIMUM	0x404U
#define POWER_MINIMUM			0x308U

/* The cells of one clocks entry: the provider's phandle and its clock number. */
#define ENGINE_CLOCK_CELLS		2U

static int provider(const struct drv_fdt *fdt, uint32_t node, const char *property, uint32_t *owner, uint32_t *number);
static uint32_t read_clock_id(const struct drv_fdt *fdt, uint32_t node);
static int map_engine_window(const struct drv_fdt *fdt, uint32_t node, const char *name, uint64_t minimum, struct bcm2711_window *window);
static int clock_start(struct bcm2712_v3d *v3d);
static int sequencer_wake(struct bcm2712_v3d *v3d);
static int sequencer_wait(struct bcm2712_v3d *v3d, uint32_t offset, bool idle, uint32_t *state, uint32_t *polls);
static void clock_report(void);

/*
 * Runs stage V0: finds and maps the V3D 7.1 engine and its power manager.
 *
 * Returns 0 when every window was found and mapped; ENODEV when the board
 * has no usable engine; ECANCELED when the boot parameters stop V3D before
 * V0.  No register of the engine is read.
 */
int
bcm2712_v3d_discover(
	const struct drv_fdt *fdt,
	struct bcm2712_v3d *v3d)
{
	struct bcm2711_v3d *engine;
	uint32_t node;
	uint32_t owner;
	uint32_t number;
	uint32_t reset_owner;
	uint32_t index;
	bool allowed;
	int core_error;
	int hub_error;
	int error;

	/* Starts from an empty description with no interrupt lines. */
	engine = &v3d->engine;
	engine->present = false;
	engine->irq.irq = BCM2711_NO_IRQ;
	engine->irq.registered = false;
	engine->irq.count = 0;
	engine->irq.service = NULL;
	engine->irq.owner = NULL;
	v3d->hub_irq.irq = BCM2711_NO_IRQ;
	v3d->hub_irq.registered = false;
	v3d->hub_irq.count = 0;
	v3d->hub_irq.service = NULL;
	v3d->hub_irq.owner = NULL;

	/* Honors v3d.stop=V0. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V0");
	if (!allowed) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 stopped by boot parameter");
		return ECANCELED;
	}
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 begin (7.1)");

	/* Finds the engine; without it there is nothing to render with. */
	error = bcm2711_fdt_find(fdt, ENGINE_COMPATIBLE, &node);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 no %s node: no engine", ENGINE_COMPATIBLE);
		return ENODEV;
	}

	/* Requires the power manager's engine domain and reset, as the binding names them. */
	error = provider(fdt, node, "power-domains", &owner, &number);
	if (error != 0 || number != POWER_DOMAIN_ENGINE) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 power domain unusable (%d)", error);
		return ENODEV;
	}
	error = bcm2711_fdt_string_index(fdt, owner, "compatible", POWER_COMPATIBLE, &index);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 power domain is not the BCM2712 PM");
		return ENODEV;
	}
	error = provider(fdt, node, "resets", &reset_owner, &number);
	if (error != 0 || reset_owner != owner || number != POWER_RESET_ENGINE) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 reset unusable (%d)", error);
		return ENODEV;
	}
	engine->has_power_domain = true;
	engine->has_reset = true;

	/* Requires the firmware's engine clock; the tree misspells its name, so it is taken by position. */
	engine->clock_id = read_clock_id(fdt, node);
	if (engine->clock_id != ENGINE_CLOCK) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 clock %u is not the engine's", (unsigned)engine->clock_id);
		return ENODEV;
	}

	/* Maps the hub, the core and the sequencer by name; nothing is read from them yet. */
	error = map_engine_window(fdt, node, "hub", ENGINE_HUB_MINIMUM, &engine->hub);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 hub window unusable (%d)", error);
		return ENODEV;
	}
	error = map_engine_window(fdt, node, "core0", ENGINE_CORE_MINIMUM, &engine->core);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 core window unusable (%d)", error);
		return ENODEV;
	}
	error = map_engine_window(fdt, node, "sms", ENGINE_SEQUENCER_MINIMUM, &v3d->sequencer);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 sequencer window unusable (%d)", error);
		return ENODEV;
	}

	/* Maps the power manager, which is always powered. */
	error = map_engine_window(fdt, owner, "pm", POWER_MINIMUM, &engine->power.control);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 PM window unusable (%d)", error);
		return ENODEV;
	}
	engine->power.prepared = true;

	/* Reads both interrupts; a missing one leaves its line absent. */
	error = bcm2711_fdt_gic_irq(fdt, node, ENGINE_IRQ_HUB, &v3d->hub_irq.irq);
	if (error != 0)
		v3d->hub_irq.irq = BCM2711_NO_IRQ;
	error = bcm2711_fdt_gic_irq(fdt, node, ENGINE_IRQ_CORE, &engine->irq.irq);
	if (error != 0)
		engine->irq.irq = BCM2711_NO_IRQ;

	/* Installs both handlers, leaving the lines masked. */
	hub_error = bcm2711_irq_install(&v3d->hub_irq);
	core_error = bcm2711_irq_install(&engine->irq);

	/* Reads the power manager's graphics registers as the firmware left them. */
	v3d->pm_graphics = kern_mmio_read32(engine->power.control.mapped + POWER_GRAPHICS_LEGACY);
	v3d->pm_v3d = kern_mmio_read32(engine->power.control.mapped + POWER_GRAPHICS);

	/* Shows the windows, the interrupts and the power manager. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 hub %llx core %llx sms %llx pm %llx",
			   (unsigned long long)engine->hub.physical,
			   (unsigned long long)engine->core.physical,
			   (unsigned long long)v3d->sequencer.physical,
			   (unsigned long long)engine->power.control.physical);
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 irq hub %d%s core %d%s pm 10c=%08x 304=%08x",
			   v3d->hub_irq.irq,
			   hub_error == 0 ? "" : " (no handler)",
			   engine->irq.irq,
			   core_error == 0 ? "" : " (no handler)",
			   v3d->pm_graphics,
			   v3d->pm_v3d);

	/* Shows the firmware's engine clock; only asks. */
	clock_report();

	/* Publishes the engine to the later stages. */
	engine->present = true;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 ok");

	/* Succeeded: every window is mapped and the providers are known. */
	return 0;
}

/*
 * Runs stage V1: powers the engine and wakes its sequencer.
 *
 * Releases the power manager's reset, runs the firmware clock at its
 * highest rate and brings the sequencer to idle through its reset.  Every
 * wait is bounded; a failure leaves the engine not ready, which keeps every
 * later stage from reading its registers.
 */
int
bcm2712_v3d_power_start(
	struct bcm2712_v3d *v3d)
{
	struct bcm2711_v3d *engine;
	uint32_t value;
	bool allowed;
	int error;

	/* A partly executed startup is not retried. */
	engine = &v3d->engine;
	if (!engine->power.prepared)
		return ENODEV;
	if (engine->power.attempted)
		return EBUSY;
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_V3D, "V1");
	if (!allowed)
		return ECANCELED;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 begin PM release, clock, SMS");
	engine->power.attempted = true;

	/* Releases the engine from the power manager's reset. */
	value = kern_mmio_read32(engine->power.control.mapped + POWER_GRAPHICS);
	kern_mmio_write32(engine->power.control.mapped + POWER_GRAPHICS, POWER_WRITE_KEY | value | POWER_ENGINE_RELEASE);
	value = kern_mmio_read32(engine->power.control.mapped + POWER_GRAPHICS);
	kern_usleep_range(1, 1);
	if ((value & POWER_ENGINE_RELEASE) == 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 PM 304 did not take the release (%08x)", value);
		return EIO;
	}

	/* Runs the engine's clock at its highest rate. */
	error = clock_start(v3d);
	if (error != 0)
		return error;

	/* Wakes the sequencer and resets it, the first access behind the engine's domain. */
	error = sequencer_wake(v3d);
	(void)drv_rpi5_serror_check("v3d: waking the power sequencer");
	if (error != 0)
		return error;

	/* Grants register access to the later stages. */
	engine->power.ready = true;
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 ok PM 304=%08x clock %u Hz", value, v3d->clock_hz);

	/* Succeeded: the engine's registers may now be read. */
	return 0;
}

/*
 * Resets the engine through its sequencer and the power manager.
 *
 * The caller has masked the engine's interrupts and stopped submission.
 * The sequencer isolates the engine and resets, then the power manager's
 * reset bit is dropped and released again; the caller restores the
 * invariant state, the page table and the interrupts.
 */
int
bcm2712_v3d_power_reset(
	struct bcm2712_v3d *v3d)
{
	struct bcm2711_v3d *engine;
	uint32_t value;
	int error;

	/* Refuses an engine that was never powered. */
	engine = &v3d->engine;
	if (!engine->power.prepared || !engine->power.attempted)
		return ENODEV;
	engine->power.ready = false;

	/* Isolates and resets the engine through the sequencer. */
	error = bcm2712_v3d_sequencer_reset(v3d);
	if (error != 0)
		return error;

	/* Pulses the power manager's reset of the engine. */
	value = kern_mmio_read32(engine->power.control.mapped + POWER_GRAPHICS);
	kern_mmio_write32(engine->power.control.mapped + POWER_GRAPHICS, POWER_WRITE_KEY | (value & ~POWER_ENGINE_RELEASE));
	kern_usleep_range(1, 1);
	kern_mmio_write32(engine->power.control.mapped + POWER_GRAPHICS, POWER_WRITE_KEY | value | POWER_ENGINE_RELEASE);
	kern_usleep_range(1, 1);

	/* Requires the release to have taken effect before register access returns. */
	value = kern_mmio_read32(engine->power.control.mapped + POWER_GRAPHICS);
	if ((value & POWER_ENGINE_RELEASE) == 0)
		return EIO;
	engine->power.ready = true;

	/* Succeeded: the engine left reset. */
	return 0;
}

/*
 * Asks the sequencer for a reset and waits until it has finished.
 *
 * The wait ends when the state is neither "isolating for reset" nor
 * "resetting", at most 100 ms after the request.
 */
int
bcm2712_v3d_sequencer_reset(
	struct bcm2712_v3d *v3d)
{
	uint32_t state;
	uint32_t polls;
	int error;

	/* Asks for the reset. */
	kern_mmio_write32(v3d->sequencer.mapped + SEQUENCER_REE, SEQUENCER_RESET_REQUEST);

	/* Waits for the sequencer to leave isolation and reset. */
	error = sequencer_wait(v3d, SEQUENCER_REE, false, &state, &polls);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "SMS reset timed out, REE %08x", state);
		return error;
	}

	/* Succeeded: the sequencer finished its reset. */
	return 0;
}

/* Resolves a single-cell provider specifier to its node and number. */
static int
provider(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *property,
	uint32_t *owner,
	uint32_t *number)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	int error;

	/* Power and reset contain one complete specifier: a phandle and a number. */
	error = drv_fdt_property(fdt, node, property, &value, &length);
	if (error != 0)
		return error;
	if (length != 8U)
		return EINVAL;

	/* Finds the provider the phandle names. */
	phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1);
	*number = (uint32_t)drv_fdt_cells_value(value, 1, 1);
	error = drv_fdt_find_phandle(fdt, phandle, owner);
	if (error != 0)
		return error;

	/* Succeeded: owner and number describe the specifier. */
	return 0;
}

/* Reads the firmware clock number from the node's first clocks entry, zero when none. */
static uint32_t
read_clock_id(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t clock_id;
	int error;

	/* Finds the clocks property. */
	error = drv_fdt_property(fdt, node, "clocks", &value, &length);
	if (error != 0)
		return 0;

	/* Refuses an entry too short to hold a provider and a number. */
	if (length < ENGINE_CLOCK_CELLS * 4U)
		return 0;

	/* Reads the number that follows the provider's phandle. */
	clock_id = (uint32_t)drv_fdt_cells_value(value, 1U, 1U);

	/* Reports the clock number. */
	return clock_id;
}

/* Finds one named window of a node, checks its size and maps it uncached. */
static int
map_engine_window(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint64_t minimum,
	struct bcm2711_window *window)
{
	int error;

	/* Finds the window by its reg-names entry. */
	error = bcm2711_fdt_named_window(fdt, node, name, window);
	if (error != 0)
		return error;

	/* Refuses a window smaller than the registers the driver reads. */
	if (window->size < minimum)
		return EINVAL;

	/* Maps the window; mapping reads nothing from the device. */
	error = bcm2711_map_window(window);
	if (error != 0)
		return error;

	/* Succeeded: the window is mapped. */
	return 0;
}

/* Sets the engine's clock to its highest rate and turns it on, checking the result. */
static int
clock_start(
	struct bcm2712_v3d *v3d)
{
	uint32_t state;
	uint32_t maximum;
	uint32_t rate;
	int error;

	/* Refuses a firmware without the engine's clock. */
	error = bcm2712_clock_get(CLOCK_TAG_STATE, ENGINE_CLOCK, &state);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 clock state unanswered (%d)", error);
		return error;
	}
	if ((state & CLOCK_ABSENT) != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 firmware has no engine clock");
		return ENODEV;
	}

	/* Reads the highest rate the firmware allows. */
	error = bcm2712_clock_get(CLOCK_TAG_MAX_RATE, ENGINE_CLOCK, &maximum);
	if (error != 0 || maximum == 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 clock maximum unanswered (%d)", error);
		return EIO;
	}

	/* Asks for the highest rate, then turns the clock on. */
	error = bcm2712_clock_set(CLOCK_TAG_SET_RATE, ENGINE_CLOCK, maximum);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 clock rate refused (%d)", error);
		return error;
	}
	error = bcm2712_clock_set(CLOCK_TAG_SET_STATE, ENGINE_CLOCK, CLOCK_RUNNING);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 clock enable refused (%d)", error);
		return error;
	}

	/* Requires a running clock at a nonzero rate before the engine is touched. */
	error = bcm2712_clock_get(CLOCK_TAG_STATE, ENGINE_CLOCK, &state);
	if (error != 0)
		return error;
	error = bcm2712_clock_get(CLOCK_TAG_RATE, ENGINE_CLOCK, &rate);
	if (error != 0)
		return error;
	if ((state & CLOCK_RUNNING) == 0 || rate == 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 clock not running (state %x rate %u)", state, rate);
		return EIO;
	}
	v3d->clock_hz = rate;
	v3d->engine.clock_asked_hz = maximum;

	/* Succeeded: the engine's clock runs. */
	return 0;
}

/* Brings the sequencer out of power-off to idle and resets it, logging what it saw. */
static int
sequencer_wake(
	struct bcm2712_v3d *v3d)
{
	uint32_t before;
	uint32_t state;
	uint32_t polls;
	uint32_t reset_polls;
	int error;

	/* Clears a power-off request and waits for the sequencer to become idle. */
	before = kern_mmio_read32(v3d->sequencer.mapped + SEQUENCER_TEE);
	kern_mmio_write32(v3d->sequencer.mapped + SEQUENCER_TEE, SEQUENCER_CLEAR_POWER_OFF);
	error = sequencer_wait(v3d, SEQUENCER_TEE, true, &state, &polls);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 SMS power-up timed out, TEE %08x then %08x", before, state);
		return error;
	}

	/* Resets the sequencer, as the engine's first start requires. */
	kern_mmio_write32(v3d->sequencer.mapped + SEQUENCER_REE, SEQUENCER_RESET_REQUEST);
	error = sequencer_wait(v3d, SEQUENCER_REE, false, &state, &reset_polls);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V1 SMS reset timed out, REE %08x", state);
		return error;
	}

	/* Shows the sequencer's states and how long each step took. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"V1 SMS TEE %08x idle after %u us, REE %08x after %u us",
		before,
		(unsigned)(polls * SEQUENCER_POLL_US),
		state,
		(unsigned)(reset_polls * SEQUENCER_POLL_US));

	/* Succeeded: the sequencer is idle and reset. */
	return 0;
}

/*
 * Waits for one sequencer state: idle when idle is true, otherwise any
 * state but isolating and resetting.
 *
 * state reports the last word read and polls how many polls passed.
 */
static int
sequencer_wait(
	struct bcm2712_v3d *v3d,
	uint32_t offset,
	bool idle,
	uint32_t *state,
	uint32_t *polls)
{
	uint32_t word;
	uint32_t current;
	uint32_t poll;

	/* Polls the state until it is reached or 100 ms pass. */
	word = 0;
	for (poll = 0; poll <= SEQUENCER_POLLS; poll++) {
		/* Reads the state. */
		word = kern_mmio_read32(v3d->sequencer.mapped + offset);
		current = word & SEQUENCER_STATE_MASK;

		/* Stops at idle when idle was asked for. */
		if (idle && current == SEQUENCER_IDLE)
			break;

		/* Stops at any state outside the reset when that was asked for. */
		if (!idle && current != SEQUENCER_ISOLATING && current != SEQUENCER_RESETTING)
			break;

		/* Waits one poll before looking again. */
		kern_usleep_range(SEQUENCER_POLL_US, SEQUENCER_POLL_US);
	}

	/* Reports what the wait saw. */
	*state = word;
	*polls = poll;

	/* Reports a sequencer that never reached the state. */
	if (poll > SEQUENCER_POLLS)
		return ETIMEDOUT;

	/* Succeeded: the sequencer reached the state. */
	return 0;
}

/* Writes one stage-mark line with what the firmware says about the engine's clock. */
static void
clock_report(void)
{
	uint32_t state;
	uint32_t rate;
	uint32_t maximum;
	int error;

	/* Asks whether the clock exists and runs. */
	error = bcm2712_clock_get(CLOCK_TAG_STATE, ENGINE_CLOCK, &state);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 clock v3d (5) no answer (%d)", error);
		return;
	}

	/* Asks the present and the highest rate; zero stands for an unanswered question. */
	error = bcm2712_clock_get(CLOCK_TAG_RATE, ENGINE_CLOCK, &rate);
	if (error != 0)
		rate = 0;
	error = bcm2712_clock_get(CLOCK_TAG_MAX_RATE, ENGINE_CLOCK, &maximum);
	if (error != 0)
		maximum = 0;

	/* Shows the clock. */
	bcm2711_stage_mark(BCM2711_FAMILY_V3D, "V0 clock v3d (5) state %x rate %u max %u", state, rate, maximum);
}
