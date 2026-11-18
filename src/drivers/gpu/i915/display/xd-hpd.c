/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hotplug of the display 11 to 13 code (see xd-hpd.h).
 */

#include "display-internal.h"
#include "xd-hpd.h"
#include "xd-edid.h"
#include "xd-gmbus.h"
#include "xd-io.h"
#include "xd-platform.h"

#include "../intel/display-regs-xd.h"
#include "../intel/display-regs-xd-south.h"
#include "../intel/display-regs-xd9.h"

#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The DDI pins of the south display that raise interrupts (A to C), and its Type-C pins. */
#define I915_XD_HPD_SOUTH_DDI_PINS	3U
#define I915_XD_HPD_TC_PINS		6U

/* The pins of every DDI (A to D) in the south control, and of every Type-C port. */
#define I915_XD_HPD_DDI_CTL_PINS	4U

/* The GMBUS pins of the DDI pins (A is 1) and of the Type-C pins (TC1 is 9). */
#define I915_XD_HPD_GMBUS_DDI		1U
#define I915_XD_HPD_GMBUS_TC		9U

/*
 * The DDC of an HDMI monitor: the EEPROM that holds the EDID and the
 * segment pointer, as 7-bit I2C addresses.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (DDC_ADDR, DDC_SEGMENT_ADDR).
 */
#define I915_XD_HPD_DDC_EDID		0x50U
#define I915_XD_HPD_DDC_SEGMENT		0x30U

/* The EDID blocks a connector keeps. */
#define I915_XD_HPD_EDID_BLOCKS		(I915_XD_HPD_EDID_BYTES / I915_XD_EDID_BLOCK_BYTES)

/*
 * How many times a detection that found nothing new is repeated: once,
 * and five times on a Type-C port, whose AUX channel takes a while to
 * work after its partner asserted the pin again.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_hotplug: the retry limits 1 and 5).
 */
#define I915_XD_HPD_RETRIES		1U
#define I915_XD_HPD_RETRIES_TC		5U

/* What a storm counts for a long pulse and for a short one. */
#define I915_XD_HPD_STORM_LONG		10
#define I915_XD_HPD_STORM_SHORT		1

/* The context of an EDID read over GMBUS: the hotplug, the window and the pin. */
struct i915_xd_hpd_ddc {
	struct i915_xd_hpd *hpd;
	struct i915_xd_io *io;
	unsigned pin;
};

static void i915_xd_hpd_names(struct i915_xd_hpd *hpd, struct i915_xd_hpd_connector *connector);
static void i915_xd_hpd_bits(struct i915_xd_hpd *hpd, uint32_t *north_used, uint32_t *north_enabled, uint32_t *south_used, uint32_t *south_enabled);
static uint32_t i915_xd_hpd_update(struct i915_xd_io *io, uint32_t offset, uint32_t clear, uint32_t set);
static uint32_t i915_xd_hpd_enables(const struct i915_xd_hpd *hpd, unsigned first, unsigned count);
static void i915_xd_hpd_pins(const struct i915_xd_hpd *hpd, uint32_t trigger, uint32_t control, int south, unsigned first, unsigned count, uint32_t *pins, uint32_t *longs);
static int i915_xd_hpd_storm(struct i915_xd_hpd *hpd, unsigned pin, int long_pulse, uint64_t now_ms);
static int i915_xd_hpd_takes_pulses(const struct i915_xd_hpd_connector *connector);
static int i915_xd_hpd_probe_hdmi(struct i915_xd_hpd *hpd, struct i915_xd_io *io, const struct i915_xd_hpd_env *env, struct i915_xd_hpd_connector *connector);
static int i915_xd_hpd_probe_dp(struct i915_xd_hpd *hpd, const struct i915_xd_hpd_env *env, struct i915_xd_hpd_connector *connector);
static int i915_xd_hpd_live(struct i915_xd_hpd *hpd, struct i915_xd_io *io, const struct i915_xd_hpd_env *env, const struct i915_xd_hpd_connector *connector);
static void i915_xd_hpd_keep_edid(struct i915_xd_hpd_connector *connector, const uint8_t *edid, unsigned size);
static int i915_xd_hpd_edid_block(void *context, unsigned segment, unsigned offset, uint8_t *buffer, unsigned size);

/*
 * Prepares the hotplug of a display: the platform's pins with their
 * interrupt bits, each enabled and unused, and no connector.
 *
 * south is the kind of the south display (enum i915_pch).  The Type-C
 * pins raise their Type-C and Thunderbolt bits in the north display; the
 * DDI pins A to C and the Type-C pins raise one bit each in a south
 * display of the ICP kind or later.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug_irq.c
 * (hpd_gen11, hpd_icp).
 */
void
drv_i915_xd_hpd_init(
	struct i915_xd_hpd *hpd,
	const struct i915_xd_platform *platform,
	int south)
{
	unsigned index;
	unsigned tc;

	/* Starts with nothing: no connector, no pulse, no event. */
	kern_memset(hpd, 0, sizeof(*hpd));
	hpd->platform = platform;
	hpd->south = south;
	drv_i915_xd_gmbus_init(&hpd->gmbus);

	/* Every pin takes its interrupts until a storm. */
	for (index = 0U; index < I915_XD_HPD_PINS; index++)
		hpd->pin[index].state = I915_XD_HPD_PIN_ENABLED;

	/* The Type-C pins' bits in the north display. */
	for (tc = 0U; tc < I915_XD_HPD_TC_PINS; tc++)
		hpd->pin[I915_XD_HPD_PIN_TC(tc)].north = I915_XD_HPD_INT_TC(tc) | I915_XD_HPD_INT_TBT(tc);

	/* A south display before the ICP kind has no hotplug of this kind. */
	if (south < I915_PCH_ICP)
		return;

	/* The DDI pins' and the Type-C pins' bits in the south display. */
	for (index = 0U; index < I915_XD_HPD_SOUTH_DDI_PINS; index++)
		hpd->pin[I915_XD_HPD_PIN_DDI(index)].south = I915_XD_SDE_DDI_HPD(index);

	/* The south bits of the Type-C pins. */
	for (tc = 0U; tc < I915_XD_HPD_TC_PINS; tc++)
		hpd->pin[I915_XD_HPD_PIN_TC(tc)].south = I915_XD_SDE_TC_HPD(tc);
}

/*
 * Reports the hotplug pin of a DDI on a platform: its south DDI or
 * Type-C pin, or the Type-C pin of its north pin; I915_XD_HPD_PIN_NONE
 * for a DDI the platform does not have or without a pin.
 */
unsigned
drv_i915_xd_hpd_pin_of(
	const struct i915_xd_platform *platform,
	unsigned ddi)
{
	const struct i915_xd_port_desc *port;

	/* A DDI the platform does not drive has no pin. */
	port = drv_i915_xd_platform_port(platform, ddi);
	if (port == NULL)
		return I915_XD_HPD_PIN_NONE;

	/* The south pin, of a DDI or of a Type-C port. */
	if (port->south_pin_kind == I915_XD_SOUTH_PIN_DDI)
		return I915_XD_HPD_PIN_DDI(port->south_pin);
	if (port->south_pin_kind == I915_XD_SOUTH_PIN_TC)
		return I915_XD_HPD_PIN_TC(port->south_pin);

	/* A Type-C port with only a north pin. */
	if (port->north_pin != 0U)
		return I915_XD_HPD_PIN_TC(port->north_pin - 1U);

	/* Succeeded: the DDI has no hotplug pin. */
	return I915_XD_HPD_PIN_NONE;
}

/*
 * Adds a connector of a kind on a DDI and marks its pin used; its name
 * is its kind and its number among the connectors of that kind ("eDP-1",
 * "HDMI-A-1", "DP-2").  Returns its index, or -1 when there is no room.
 */
int
drv_i915_xd_hpd_add(
	struct i915_xd_hpd *hpd,
	unsigned kind,
	unsigned ddi)
{
	struct i915_xd_hpd_connector *connector;
	const struct i915_xd_port_desc *port;
	unsigned index;

	/* Refuses a connector past the last place. */
	if (hpd->count >= I915_XD_HPD_CONNECTORS)
		return -1;

	/* The connector, unknown, with no EDID. */
	index = hpd->count;
	connector = &hpd->connector[index];
	kern_memset(connector, 0, sizeof(*connector));
	connector->kind = (uint8_t)kind;
	connector->ddi = (uint8_t)ddi;
	connector->status = I915_XD_HPD_UNKNOWN;

	/* Its Type-C port (0 for none) and its pin. */
	port = drv_i915_xd_platform_port(hpd->platform, ddi);
	if (port != NULL)
		connector->tc = port->tc;
	connector->pin = (uint8_t)drv_i915_xd_hpd_pin_of(hpd->platform, ddi);

	/* The GMBUS pin the DDC of an HDMI monitor is on: the DDI's, or the Type-C port's. */
	connector->ddc_pin = 0U;
	if (port != NULL && port->south_pin_kind == I915_XD_SOUTH_PIN_DDI)
		connector->ddc_pin = (uint8_t)(I915_XD_HPD_GMBUS_DDI + port->south_pin);
	if (port != NULL && port->south_pin_kind == I915_XD_SOUTH_PIN_TC)
		connector->ddc_pin = (uint8_t)(I915_XD_HPD_GMBUS_TC + port->south_pin);

	/* Its name, and its pin is armed from now on. */
	i915_xd_hpd_names(hpd, connector);
	if (connector->pin != I915_XD_HPD_PIN_NONE)
		hpd->pin[connector->pin].used = 1U;

	/* Succeeded: the connector is the next one. */
	hpd->count++;
	return (int)index;
}

/*
 * Arms the pins the connectors use: unmasks the enabled ones' interrupts
 * in the north and the south display, enables their detection in the
 * Type-C, Thunderbolt and south hotplug controls (every other pin's is
 * cleared), and sets the south display's pulse filter.  The south mask
 * is written only while the device takes interrupts (interrupts nonzero);
 * the north mask and the controls only when they change.  The caller
 * holds the pins' lock.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug_irq.c
 * (icp_hpd_irq_setup: SHPD_FILTER_CNT_500_ADJ up to PCH_TGP, else
 * SHPD_FILTER_CNT_250).
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug_irq.c
 * (gen11_hpd_irq_setup, gen11_tc_hpd_detection_setup,
 * gen11_tbt_hpd_detection_setup, icp_hpd_irq_setup,
 * icp_ddi_hpd_detection_setup, icp_tc_hpd_detection_setup).
 */
void
drv_i915_xd_hpd_arm(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	int interrupts)
{
	struct i915_xd_hpd_words *words;
	uint32_t controls;
	uint32_t enables;
	uint32_t sde_imr;

	/* The bits of the used pins, and of the enabled ones among them. */
	words = &hpd->words;
	hpd->interrupts = interrupts;
	hpd->arms++;
	i915_xd_hpd_bits(hpd, &words->north_used, &words->north_enabled, &words->south_used, &words->south_enabled);

	/* The north mask: a used pin is masked unless it is enabled. */
	words->de_imr = i915_xd_hpd_update(io, I915_XD_DE_HPD_ISR + I915_XD_DE_IMR_OFFSET, words->north_used, words->north_used & ~words->north_enabled);
	drv_i915_xd_io_posting_read(io, I915_XD_DE_HPD_ISR + I915_XD_DE_IMR_OFFSET);

	/* The Type-C and Thunderbolt controls: every Type-C pin cleared, the used ones enabled. */
	controls = i915_xd_hpd_enables(NULL, I915_XD_HPD_PIN_TC(0U), I915_XD_HPD_TC_PINS);
	enables = i915_xd_hpd_enables(hpd, I915_XD_HPD_PIN_TC(0U), I915_XD_HPD_TC_PINS);
	words->tc_ctl = i915_xd_hpd_update(io, I915_XD_TC_HOTPLUG_CTL, controls, enables);
	words->tbt_ctl = i915_xd_hpd_update(io, I915_XD_TBT_HOTPLUG_CTL, controls, enables);

	/* A south display before the ICP kind has no hotplug of this kind. */
	if (hpd->south < I915_PCH_ICP)
		return;

	/* The pulse filter of the south display. */
	words->filter = I915_XD_SHPD_FILTER_250US;
	if (hpd->south <= I915_PCH_TGP)
		words->filter = I915_XD_SHPD_FILTER_500US_ADJUSTED;

	/* Writes the filter. */
	drv_i915_xd_io_write(io, I915_XD_SHPD_FILTER_CNT, words->filter);

	/* The south mask, written only while the device takes interrupts. */
	sde_imr = drv_i915_xd_io_read(io, I915_XD_SDE_IMR);
	sde_imr &= ~words->south_used;
	sde_imr |= words->south_used & ~words->south_enabled;
	words->sde_imr_written = 0;
	if (interrupts) {
		drv_i915_xd_io_write(io, I915_XD_SDE_IMR, sde_imr);
		drv_i915_xd_io_posting_read(io, I915_XD_SDE_IMR);
		words->sde_imr_written = 1;
	}

	/* Reads the south mask back. */
	words->sde_imr = drv_i915_xd_io_read(io, I915_XD_SDE_IMR);

	/* The south controls of the DDI pins and of the Type-C pins. */
	controls = i915_xd_hpd_enables(NULL, I915_XD_HPD_PIN_DDI(0U), I915_XD_HPD_DDI_CTL_PINS);
	enables = i915_xd_hpd_enables(hpd, I915_XD_HPD_PIN_DDI(0U), I915_XD_HPD_DDI_CTL_PINS);
	words->ddi_ctl = i915_xd_hpd_update(io, I915_XD_SHOTPLUG_CTL_DDI, controls, enables);
	controls = i915_xd_hpd_enables(NULL, I915_XD_HPD_PIN_TC(0U), I915_XD_HPD_TC_PINS);
	enables = i915_xd_hpd_enables(hpd, I915_XD_HPD_PIN_TC(0U), I915_XD_HPD_TC_PINS);
	words->tc_south_ctl = i915_xd_hpd_update(io, I915_XD_SHOTPLUG_CTL_TC, controls, enables);
}

/*
 * Acknowledges a south display interrupt's hotplug pulses and reports
 * the pins they came on and which were long.  The DDI and the Type-C
 * controls each latch their pins' pulses, which writing back what was
 * read clears.  A GMBUS interrupt is counted.  Runs in the interrupt.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug_irq.c
 * (icp_irq_handler, intel_get_hpd_pins, icp_ddi_port_hotplug_long_detect,
 * icp_tc_port_hotplug_long_detect).
 */
void
drv_i915_xd_hpd_south_irq(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	uint32_t iir,
	uint32_t *pins,
	uint32_t *longs)
{
	uint32_t ddi_bits;
	uint32_t tc_bits;
	uint32_t control;
	unsigned index;

	/* The bits of the DDI pins and of the Type-C pins. */
	*pins = 0U;
	*longs = 0U;
	ddi_bits = 0U;
	for (index = 0U; index < I915_XD_HPD_SOUTH_DDI_PINS; index++)
		ddi_bits |= hpd->pin[I915_XD_HPD_PIN_DDI(index)].south;

	/* The south bits of the Type-C pins. */
	tc_bits = 0U;
	for (index = 0U; index < I915_XD_HPD_TC_PINS; index++)
		tc_bits |= hpd->pin[I915_XD_HPD_PIN_TC(index)].south;

	/* The DDI pulses: acknowledged, and their lengths read from the control. */
	if ((iir & ddi_bits) != 0U) {
		control = drv_i915_xd_io_read(io, I915_XD_SHOTPLUG_CTL_DDI);
		drv_i915_xd_io_write(io, I915_XD_SHOTPLUG_CTL_DDI, control);
		i915_xd_hpd_pins(hpd, iir, control, 1, I915_XD_HPD_PIN_DDI(0U), I915_XD_HPD_SOUTH_DDI_PINS, pins, longs);
	}

	/* The Type-C pulses, the same way. */
	if ((iir & tc_bits) != 0U) {
		control = drv_i915_xd_io_read(io, I915_XD_SHOTPLUG_CTL_TC);
		drv_i915_xd_io_write(io, I915_XD_SHOTPLUG_CTL_TC, control);
		i915_xd_hpd_pins(hpd, iir, control, 1, I915_XD_HPD_PIN_TC(0U), I915_XD_HPD_TC_PINS, pins, longs);
	}

	/* The GMBUS controller's interrupt, which nobody waits for: counted. */
	if ((iir & I915_XD_SDE_GMBUS) != 0U)
		hpd->gmbus_irqs++;
}

/*
 * Acknowledges a north display hotplug interrupt (the Type-C and the
 * Thunderbolt pins) and reports the pins and which pulses were long, the
 * same way as the south display's.  An interrupt with no pin is counted.
 * Runs in the interrupt.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug_irq.c
 * (gen11_hpd_irq_handler, gen11_port_hotplug_long_detect).
 */
void
drv_i915_xd_hpd_north_irq(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	uint32_t iir,
	uint32_t *pins,
	uint32_t *longs)
{
	uint32_t tc_bits;
	uint32_t tbt_bits;
	uint32_t control;
	unsigned tc;

	/* The Type-C bits and the Thunderbolt bits of the pins. */
	*pins = 0U;
	*longs = 0U;
	tc_bits = 0U;
	tbt_bits = 0U;
	for (tc = 0U; tc < I915_XD_HPD_TC_PINS; tc++) {
		tc_bits |= I915_XD_HPD_INT_TC(tc);
		tbt_bits |= I915_XD_HPD_INT_TBT(tc);
	}

	/* The Type-C pulses: acknowledged, and their lengths read from the control. */
	if ((iir & tc_bits) != 0U) {
		control = drv_i915_xd_io_read(io, I915_XD_TC_HOTPLUG_CTL);
		drv_i915_xd_io_write(io, I915_XD_TC_HOTPLUG_CTL, control);
		i915_xd_hpd_pins(hpd, iir & tc_bits, control, 0, I915_XD_HPD_PIN_TC(0U), I915_XD_HPD_TC_PINS, pins, longs);
	}

	/* The Thunderbolt pulses, the same way. */
	if ((iir & tbt_bits) != 0U) {
		control = drv_i915_xd_io_read(io, I915_XD_TBT_HOTPLUG_CTL);
		drv_i915_xd_io_write(io, I915_XD_TBT_HOTPLUG_CTL, control);
		i915_xd_hpd_pins(hpd, iir & tbt_bits, control, 0, I915_XD_HPD_PIN_TC(0U), I915_XD_HPD_TC_PINS, pins, longs);
	}

	/* An interrupt that named no pin. */
	if (*pins == 0U)
		hpd->unexpected++;
}

/*
 * Sorts the pulses of an interrupt: a connector that takes its own
 * pulses gets a long or a short one for the pulse work, any other pin's
 * pulse is a detection event; a pin raising too many is marked to be
 * masked, its event dropped, and the pins are armed again at once.
 * Returns what to queue (I915_XD_HPD_QUEUE_*) and whether a storm began.
 * Runs in the interrupt, under the pins' lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug.c
 * (intel_hpd_irq_handler, intel_hpd_irq_storm_detect).
 */
unsigned
drv_i915_xd_hpd_sort(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	uint32_t pins,
	uint32_t longs,
	uint64_t now_ms)
{
	struct i915_xd_hpd_connector *connector;
	uint32_t pulse_pins;
	uint32_t long_pins;
	unsigned queue;
	unsigned index;
	unsigned pin;
	int long_pulse;
	int storm;
	int takes;

	/* Nothing raised, nothing to do. */
	queue = 0U;
	if (pins == 0U)
		return queue;

	/* The connectors that take their own pulses get them. */
	pulse_pins = 0U;
	long_pins = 0U;
	for (index = 0U; index < hpd->count; index++) {
		connector = &hpd->connector[index];

		/* A connector whose pin did not raise, or that leaves its pulses to the detection. */
		if (connector->pin == I915_XD_HPD_PIN_NONE)
			continue;
		if ((pins & (1U << connector->pin)) == 0U)
			continue;

		/* A connector that takes no pulses is passed over. */
		takes = i915_xd_hpd_takes_pulses(connector);
		if (!takes)
			continue;

		/* A long pulse or a short one for the connector's port. */
		pulse_pins |= 1U << connector->pin;
		queue |= I915_XD_HPD_QUEUE_PULSES;
		if ((longs & (1U << connector->pin)) != 0U) {
			long_pins |= 1U << connector->pin;
			hpd->long_ports |= 1U << connector->ddi;
		} else {
			hpd->short_ports |= 1U << connector->ddi;
		}
	}

	/* Each pin once: its event, and its storm count. */
	for (pin = 0U; pin < I915_XD_HPD_PINS; pin++) {
		if ((pins & (1U << pin)) == 0U)
			continue;

		/* A masked pin should not have raised; one marked for masking takes no more. */
		if (hpd->pin[pin].state == I915_XD_HPD_PIN_DISABLED) {
			hpd->disabled_irqs++;
			continue;
		}

		/* A pin that is not enabled is passed over. */
		if (hpd->pin[pin].state != I915_XD_HPD_PIN_ENABLED)
			continue;

		/* The length of a pulse a connector takes, or an event of the detection (taken as long). */
		long_pulse = 1;
		if ((pulse_pins & (1U << pin)) != 0U) {
			long_pulse = 0;
			if ((long_pins & (1U << pin)) != 0U)
				long_pulse = 1;
		} else {
			hpd->events |= 1U << pin;
			queue |= I915_XD_HPD_QUEUE_DETECT;
		}

		/* A storm takes the pin's event back and has the detection mask the pin. */
		storm = i915_xd_hpd_storm(hpd, pin, long_pulse, now_ms);
		if (storm) {
			hpd->events &= ~(1U << pin);
			queue |= I915_XD_HPD_QUEUE_DETECT | I915_XD_HPD_STORM;
		}
	}

	/* The storming pins are masked now. */
	if ((queue & I915_XD_HPD_STORM) != 0U) {
		hpd->storms++;
		drv_i915_xd_hpd_arm(hpd, io, hpd->interrupts);
	}

	/* Succeeded: the pulses and events are sorted. */
	return queue;
}

/*
 * Masks the pins a storm marked; nonzero when one was, and the caller
 * arms them again after I915_XD_HPD_STORM_REARM_MS.  The caller holds
 * the pins' lock.
 */
int
drv_i915_xd_hpd_storms(
	struct i915_xd_hpd *hpd)
{
	unsigned pin;
	int masked;

	/* Every marked pin is masked from now on. */
	masked = 0;
	for (pin = 0U; pin < I915_XD_HPD_PINS; pin++) {
		if (hpd->pin[pin].state != I915_XD_HPD_PIN_MARKED)
			continue;

		/* Disables the marked pin. */
		hpd->pin[pin].state = I915_XD_HPD_PIN_DISABLED;
		masked = 1;
	}

	/* Succeeded: reports whether a pin was masked. */
	return masked;
}

/*
 * Enables the pins a storm masked and arms them.  The caller holds the
 * pins' lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug.c
 * (intel_hpd_irq_storm_reenable_work).
 */
void
drv_i915_xd_hpd_rearm(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io)
{
	unsigned pin;

	/* Every masked pin takes its interrupts again. */
	for (pin = 0U; pin < I915_XD_HPD_PINS; pin++) {
		if (hpd->pin[pin].state == I915_XD_HPD_PIN_DISABLED)
			hpd->pin[pin].state = I915_XD_HPD_PIN_ENABLED;
	}

	/* Unmasks them. */
	drv_i915_xd_hpd_arm(hpd, io, hpd->interrupts);
}

/*
 * Hands a pulse to a connector that takes its own: the built-in panel's
 * is ignored (its sink's power going off raises one); a long one gives
 * the sink its own link limits back and is left to the detection; a
 * short one of a Type-C DisplayPort port has its sink checked, and a sink
 * that changed is left to the detection.  Returns 1 when the pulse was
 * taken, 0 when the detection is to look at the connector.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_hpd_pulse).
 */
int
drv_i915_xd_hpd_pulse(
	struct i915_xd_hpd *hpd,
	const struct i915_xd_hpd_env *env,
	unsigned index,
	int long_pulse)
{
	struct i915_xd_hpd_connector *connector;
	int same;

	/* Refuses a connector that does not exist. */
	if (index >= hpd->count)
		return 1;

	/* The built-in panel's pulses are ignored. */
	connector = &hpd->connector[index];
	if (connector->kind == I915_XD_HPD_EDP)
		return 1;

	/* A long pulse: the sink's limits back, and the detection looks. */
	if (long_pulse) {
		if (env->dp_reset != NULL)
			env->dp_reset(env->ctx, connector->ddi);

		/* Succeeded: the pulse is handled. */
		return 0;
	}

	/* A short pulse of a port whose sink is not checked is taken as it is. */
	if (connector->tc == 0U || env->dp_pulse == NULL)
		return 1;

	/* The sink is checked; one that is not as it was is detected again. */
	same = env->dp_pulse(env->ctx, connector->ddi);
	if (!same)
		return 0;

	/* Succeeded: the sink is as it was. */
	return 1;
}

/*
 * Reads a connector's state: an HDMI connector's live state and its EDID
 * over GMBUS, a Type-C DisplayPort connector's live state and its sink's
 * probe; the built-in panel and the other DisplayPort connectors stay
 * unknown.  The epoch moves when the state or the EDID changes.
 * Returns the state (I915_XD_HPD_CONNECTED and so on).  The caller holds
 * the detection's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_probe_helper.c
 * (drm_helper_probe_detect).
 */
int
drv_i915_xd_hpd_probe(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	const struct i915_xd_hpd_env *env,
	unsigned index)
{
	struct i915_xd_hpd_connector *connector;
	int status;

	/* Refuses a connector that does not exist. */
	if (index >= hpd->count)
		return I915_XD_HPD_UNKNOWN;

	/* The state of the connector's kind. */
	connector = &hpd->connector[index];
	status = I915_XD_HPD_UNKNOWN;
	if (connector->kind == I915_XD_HPD_HDMI) {
		status = i915_xd_hpd_probe_hdmi(hpd, io, env, connector);
	} else if (connector->kind == I915_XD_HPD_DP) {
		status = i915_xd_hpd_probe_dp(hpd, env, connector);
	}

	/* A changed state is a new epoch. */
	if (status != connector->status)
		connector->epoch++;

	/* Succeeded: the connector's state. */
	connector->status = status;
	return status;
}

/*
 * Detects a connector for an event (event nonzero) or a retry: reads its
 * state, has its port's link looked at, and asks for another look when
 * nothing changed while retries remain (a Type-C sink's AUX channel and a
 * monitor's DDC may answer only after a while).  Returns
 * I915_XD_HPD_UNCHANGED, _CHANGED or _RETRY.  The caller holds the
 * detection's lock.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_ddi.c
 * (intel_ddi_hotplug), drivers/gpu/drm/i915/display/intel_hotplug.c
 * (intel_hotplug_detect_connector).
 */
int
drv_i915_xd_hpd_detect(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	const struct i915_xd_hpd_env *env,
	unsigned index,
	int event)
{
	struct i915_xd_hpd_connector *connector;
	uint64_t epoch;
	unsigned limit;
	int hdmi;

	/* Refuses a connector that does not exist. */
	if (index >= hpd->count)
		return I915_XD_HPD_UNCHANGED;

	/* An event starts the retries over; a retry counts one more. */
	connector = &hpd->connector[index];
	if (event) {
		connector->retries = 0U;
	} else {
		connector->retries++;
	}

	/* Reads the connector's state; the epoch says whether it changed. */
	epoch = connector->epoch;
	(void)drv_i915_xd_hpd_probe(hpd, io, env, index);

	/* The port's link is looked at (a lost Type-C partner, a lit link's lock). */
	hdmi = 0;
	if (connector->kind == I915_XD_HPD_HDMI)
		hdmi = 1;

	/* Hands a sink other than the panel its link step. */
	if (connector->kind != I915_XD_HPD_EDP && env->link != NULL)
		env->link(env->ctx, connector->ddi, hdmi);

	/* A connector whose epoch moved changed. */
	if (connector->epoch != epoch)
		return I915_XD_HPD_CHANGED;

	/* Nothing changed: another look while retries remain. */
	limit = I915_XD_HPD_RETRIES;
	if (connector->tc != 0U)
		limit = I915_XD_HPD_RETRIES_TC;

	/* A connector with tries left is tried again. */
	if (connector->retries < limit)
		return I915_XD_HPD_RETRY;

	/* Succeeded: the connector is as it was. */
	return I915_XD_HPD_UNCHANGED;
}

/*
 * Names a connector's state for the log.
 */
const char *
drv_i915_xd_hpd_status_name(
	int status)
{
	/* The two definite states. */
	if (status == I915_XD_HPD_CONNECTED)
		return "connected";
	if (status == I915_XD_HPD_DISCONNECTED)
		return "disconnected";

	/* Succeeded: the state is not known. */
	return "unknown";
}

/*
 * Turns on the display 9 south display's hotplug sense of DDIs B to D.
 */
void
drv_i915_xd9_hpd_enable(
	struct i915_xd_io *io)
{
	uint32_t enables;
	unsigned ddi;

	/* Succeeded: each DDI's enable. */
	enables = 0U;
	for (ddi = I915_XD9_HPD_FIRST_DDI; ddi <= I915_XD9_HPD_LAST_DDI; ddi++)
		enables |= I915_XD9_PORT_HOTPLUG_ENABLE(ddi);
	(void)drv_i915_xd_io_rmw(io, I915_XD9_PORT_HOTPLUG, 0U, enables);
}

/* Reports the display 9 DDIs B to D the board has (bit per DDI, B is bit 1). */
unsigned
drv_i915_xd9_hpd_present(
	struct i915_xd_io *io)
{
	uint32_t strap;
	unsigned ddis;
	unsigned ddi;

	/* Each DDI the strap found. */
	strap = drv_i915_xd_io_read(io, I915_XD_SFUSE_STRAP);
	ddis = 0U;
	for (ddi = I915_XD9_HPD_FIRST_DDI; ddi <= I915_XD9_HPD_LAST_DDI; ddi++) {
		if ((strap & I915_XD9_SFUSE_DDI_DETECTED(ddi)) != 0U)
			ddis |= 1U << ddi;
	}

	/* Succeeded: the DDIs. */
	return ddis;
}

/* Reports the display 9 DDIs B to D a sink is attached to now (bit per DDI). */
unsigned
drv_i915_xd9_hpd_connected(
	struct i915_xd_io *io)
{
	uint32_t live;
	unsigned ddis;
	unsigned ddi;

	/* Each DDI's live state. */
	live = drv_i915_xd_io_read(io, I915_XD_SDE_ISR);
	ddis = 0U;
	for (ddi = I915_XD9_HPD_FIRST_DDI; ddi <= I915_XD9_HPD_LAST_DDI; ddi++) {
		if ((live & I915_XD9_SDE_HPD(ddi)) != 0U)
			ddis |= 1U << ddi;
	}

	/* Succeeded: the attached DDIs. */
	return ddis;
}

/* Names a connector after its kind and its number among the connectors of that kind. */
static void
i915_xd_hpd_names(
	struct i915_xd_hpd *hpd,
	struct i915_xd_hpd_connector *connector)
{
	const char *prefix;
	unsigned number;
	unsigned length;

	/* The kind's prefix and the connector's number among its kind. */
	if (connector->kind == I915_XD_HPD_EDP) {
		prefix = "eDP-";
		hpd->edp++;
		number = hpd->edp;
	} else if (connector->kind == I915_XD_HPD_HDMI) {
		prefix = "HDMI-A-";
		hpd->hdmi++;
		number = hpd->hdmi;
	} else {
		prefix = "DP-";
		hpd->dp++;
		number = hpd->dp;
	}

	/* The prefix, then the number as one digit. */
	length = (unsigned)kern_strlen(prefix);
	kern_memcpy(connector->name, prefix, length);
	connector->name[length] = (char)('0' + number % 10U);
	connector->name[length + 1U] = '\0';
}

/* Collects the north and south bits of the used pins, and of the enabled ones among them. */
static void
i915_xd_hpd_bits(
	struct i915_xd_hpd *hpd,
	uint32_t *north_used,
	uint32_t *north_enabled,
	uint32_t *south_used,
	uint32_t *south_enabled)
{
	unsigned pin;

	/* Starts with no bits. */
	*north_used = 0U;
	*north_enabled = 0U;
	*south_used = 0U;
	*south_enabled = 0U;

	/* Every used pin's bits; only an enabled pin's are unmasked. */
	for (pin = 0U; pin < I915_XD_HPD_PINS; pin++) {
		if (!hpd->pin[pin].used)
			continue;

		/* Gathers the pin's bits. */
		*north_used |= hpd->pin[pin].north;
		*south_used |= hpd->pin[pin].south;
		if (hpd->pin[pin].state == I915_XD_HPD_PIN_ENABLED) {
			*north_enabled |= hpd->pin[pin].north;
			*south_enabled |= hpd->pin[pin].south;
		}
	}
}

/* Clears and sets bits of a register, writing it only when it changes; reports what it holds. */
static uint32_t
i915_xd_hpd_update(
	struct i915_xd_io *io,
	uint32_t offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t old_value;
	uint32_t new_value;

	/* The register now and what it is to hold. */
	old_value = drv_i915_xd_io_read(io, offset);
	new_value = (old_value & ~clear) | set;

	/* Only a change is written. */
	if (new_value != old_value)
		drv_i915_xd_io_write(io, offset, new_value);

	/* Succeeded: what the register holds. */
	return new_value;
}

/*
 * Collects the detection enable bits of count pins from first in their
 * hotplug control (a nibble a pin): every pin's with no hotplug given, or
 * only the used ones'.
 */
static uint32_t
i915_xd_hpd_enables(
	const struct i915_xd_hpd *hpd,
	unsigned first,
	unsigned count)
{
	uint32_t bits;
	unsigned index;

	/* The enable bit of each pin asked for. */
	bits = 0U;
	for (index = 0U; index < count; index++) {
		if (hpd != NULL && !hpd->pin[first + index].used)
			continue;

		/* Enables the pin's detection. */
		bits |= I915_XD_SHOTPLUG_ENABLE(index);
	}

	/* Succeeded: the enable bits. */
	return bits;
}

/*
 * Adds the pins of count from first whose interrupt bit is in the
 * trigger (their south bit, or their north bits), and the long ones by
 * the long detect bit of their nibble in the control, to the masks.
 */
static void
i915_xd_hpd_pins(
	const struct i915_xd_hpd *hpd,
	uint32_t trigger,
	uint32_t control,
	int south,
	unsigned first,
	unsigned count,
	uint32_t *pins,
	uint32_t *longs)
{
	uint32_t bits;
	unsigned index;

	/* Every pin whose bit raised, and its length. */
	for (index = 0U; index < count; index++) {
		bits = hpd->pin[first + index].north;
		if (south)
			bits = hpd->pin[first + index].south;

		/* A pin that did not trigger is passed over. */
		if ((trigger & bits) == 0U)
			continue;

		/* Records the pin, and whether its pulse was long. */
		*pins |= 1U << (first + index);
		if ((control & I915_XD_SHOTPLUG_LONG(index)) != 0U)
			*longs |= 1U << (first + index);
	}
}

/*
 * Counts a pulse on a pin within its one-second period (a long pulse
 * ten, a short one one; a new period starts once the old one ended) and
 * marks the pin to be masked past the limit; nonzero then.  Short pulses
 * count for nothing: they are how a DisplayPort sink talks to its source.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hotplug.c
 * (HPD_STORM_DEFAULT_THRESHOLD, HPD_STORM_DETECT_PERIOD, the long pulse's
 * weight of 10).
 */
static int
i915_xd_hpd_storm(
	struct i915_xd_hpd *hpd,
	unsigned pin,
	int long_pulse,
	uint64_t now_ms)
{
	struct i915_xd_hpd_pin *entry;

	/* A short pulse is not counted. */
	if (!long_pulse)
		return 0;

	/* A pulse after the period starts a new one. */
	entry = &hpd->pin[pin];
	if (now_ms < entry->period_ms || now_ms > entry->period_ms + I915_XD_HPD_STORM_PERIOD_MS) {
		entry->period_ms = now_ms;
		entry->count = 0;
	}

	/* Counts the pulse; past the limit the pin is to be masked. */
	entry->count += I915_XD_HPD_STORM_LONG;
	if (entry->count > I915_XD_HPD_STORM_LIMIT) {
		entry->state = I915_XD_HPD_PIN_MARKED;
		return 1;
	}

	/* Succeeded: no storm on the pin. */
	return 0;
}

/* Tells whether a connector takes its own pulses: the DisplayPort ones and the built-in panel's. */
static int
i915_xd_hpd_takes_pulses(
	const struct i915_xd_hpd_connector *connector)
{
	/* An HDMI connector leaves its pulses to the detection. */
	if (connector->kind == I915_XD_HPD_HDMI)
		return 0;

	/* Succeeded: the connector takes its pulses. */
	return 1;
}

/*
 * Reads an HDMI connector's state: a port with nothing plugged in is
 * disconnected and forgets its EDID; a live one is connected when its
 * monitor answers with a digital EDID over GMBUS.  The GMBUS controller's
 * power is held across the reads.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_hdmi.c
 * (intel_hdmi_detect, intel_hdmi_set_edid).
 */
static int
i915_xd_hpd_probe_hdmi(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	const struct i915_xd_hpd_env *env,
	struct i915_xd_hpd_connector *connector)
{
	struct i915_xd_hpd_ddc context;
	struct i915_xd_edid_ddc ddc;
	uint8_t edid[I915_XD_HPD_EDID_BYTES];
	unsigned blocks;
	unsigned extensions;
	int refused;
	int status;
	int error;

	/* Holds the GMBUS controller's power across the detection. */
	refused = env->power(env->ctx, I915_XD_HPD_POWER_DDC, 1);
	if (refused)
		return I915_XD_HPD_DISCONNECTED;

	/* Nothing plugged in: disconnected, without an EDID. */
	status = I915_XD_HPD_DISCONNECTED;
	connector->live = i915_xd_hpd_live(hpd, io, env, connector);
	if (!connector->live) {
		i915_xd_hpd_keep_edid(connector, NULL, 0U);
		(void)env->power(env->ctx, I915_XD_HPD_POWER_DDC, 0);
		return status;
	}

	/* The EDID over GMBUS. */
	context.hpd = hpd;
	context.io = io;
	context.pin = connector->ddc_pin;
	ddc.read_block = i915_xd_hpd_edid_block;
	ddc.context = &context;
	error = drv_i915_xd_edid_read(&ddc, edid, I915_XD_HPD_EDID_BLOCKS, &blocks, &extensions);
	if (error != 0)
		blocks = 0U;

	/* The connector keeps what was read; a digital monitor is connected. */
	i915_xd_hpd_keep_edid(connector, edid, blocks * I915_XD_EDID_BLOCK_BYTES);
	if (blocks != 0U && (edid[I915_XD_EDID_INPUT] & I915_XD_EDID_INPUT_DIGITAL) != 0U)
		status = I915_XD_HPD_CONNECTED;

	/* Gives the power back. */
	(void)env->power(env->ctx, I915_XD_HPD_POWER_DDC, 0);

	/* Succeeded: the connector's state. */
	return status;
}

/*
 * Reads a DisplayPort connector's state: only a Type-C port's sink is
 * probed (its port's live state first, through the hooks); any other
 * stays unknown.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_dp_detect).
 */
static int
i915_xd_hpd_probe_dp(
	struct i915_xd_hpd *hpd,
	const struct i915_xd_hpd_env *env,
	struct i915_xd_hpd_connector *connector)
{
	uint8_t edid[I915_XD_HPD_EDID_BYTES];
	unsigned bytes;
	int status;

	UNUSED_PARAMETER(hpd);

	/* Only a Type-C port's sink is probed. */
	if (connector->tc == 0U ||
	    env->tc_live == NULL ||
	    env->dp_probe == NULL)
		return I915_XD_HPD_UNKNOWN;

	/* Nothing plugged in: disconnected, without an EDID. */
	connector->live = env->tc_live(env->ctx, connector->ddi);
	if (!connector->live) {
		i915_xd_hpd_keep_edid(connector, NULL, 0U);
		return I915_XD_HPD_DISCONNECTED;
	}

	/* The sink's probe, and the EDID it read. */
	bytes = 0U;
	status = env->dp_probe(env->ctx, connector->ddi, edid, sizeof(edid), &bytes);
	if (bytes > sizeof(edid))
		bytes = 0U;

	/* Keeps the EDID read. */
	i915_xd_hpd_keep_edid(connector, edid, bytes);

	/* Succeeded: the sink's state. */
	return status;
}

/*
 * Reads whether something is plugged into a connector's port: a Type-C
 * port asks the hook, any other reads its south pin's live bit while the
 * display core is powered.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_dp.c
 * (intel_digital_port_connected), drivers/gpu/drm/i915/display/intel_ddi.c
 * (lpt_digital_port_connected).
 */
static int
i915_xd_hpd_live(
	struct i915_xd_hpd *hpd,
	struct i915_xd_io *io,
	const struct i915_xd_hpd_env *env,
	const struct i915_xd_hpd_connector *connector)
{
	uint32_t isr;
	int refused;
	int live;

	/* A Type-C port answers through the hook, or not at all. */
	if (connector->tc != 0U) {
		live = 0;
		if (env->tc_live != NULL)
			live = env->tc_live(env->ctx, connector->ddi);

		/* Succeeded: the Type-C port's live state. */
		return live;
	}

	/* A port without a pin has nothing plugged in. */
	if (connector->pin == I915_XD_HPD_PIN_NONE)
		return 0;

	/* The live bit of the pin, read while the display core is powered. */
	refused = env->power(env->ctx, I915_XD_HPD_POWER_CORE, 1);
	if (refused)
		return 0;

	/* Reads the south live state, then gives the power back. */
	isr = drv_i915_xd_io_read(io, I915_XD_SDE_ISR);
	(void)env->power(env->ctx, I915_XD_HPD_POWER_CORE, 0);

	/* Nothing plugged in. */
	if ((isr & hpd->pin[connector->pin].south) == 0U)
		return 0;

	/* Succeeded: something is plugged in. */
	return 1;
}

/*
 * Keeps an EDID as a connector's (none: size 0); one that differs from
 * the EDID the connector had moves its epoch.
 */
static void
i915_xd_hpd_keep_edid(
	struct i915_xd_hpd_connector *connector,
	const uint8_t *edid,
	unsigned size)
{
	int differs;

	/* The EDID differs in its size or in its bytes. */
	differs = 0;
	if (size != connector->edid_size) {
		differs = 1;
	} else if (size != 0U) {
		differs = kern_memcmp(edid, connector->edid, size);
	}

	/* A different EDID is a new epoch. */
	if (differs != 0)
		connector->epoch++;

	/* The connector keeps it. */
	connector->edid_size = size;
	if (size != 0U)
		kern_memcpy(connector->edid, edid, size);
}

/*
 * Reads size bytes of an EDID block over GMBUS: the segment pointer for
 * a segment past the first, the offset within the segment, the read
 * (the DDC of struct i915_xd_edid_ddc).  Returns 0, ENXIO when no
 * monitor answers, or EIO.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (drm_do_probe_ddc_edid).
 */
static int
i915_xd_hpd_edid_block(
	void *context,
	unsigned segment,
	unsigned offset,
	uint8_t *buffer,
	unsigned size)
{
	struct i915_xd_gmbus_message messages[3];
	struct i915_xd_gmbus_request request;
	struct i915_xd_gmbus_result result;
	struct i915_xd_hpd_ddc *ddc;
	uint8_t segment_byte;
	uint8_t offset_byte;
	unsigned first;

	/* The DDC, and the bytes of the segment and the offset. */
	ddc = context;
	segment_byte = (uint8_t)segment;
	offset_byte = (uint8_t)offset;

	/* The segment pointer, which only a segment past the first needs. */
	messages[0].address = I915_XD_HPD_DDC_SEGMENT;
	messages[0].read = 0;
	messages[0].data = &segment_byte;
	messages[0].length = 1U;
	first = 1U;
	if (segment != 0U)
		first = 0U;

	/* The offset, then the bytes. */
	messages[1].address = I915_XD_HPD_DDC_EDID;
	messages[1].read = 0;
	messages[1].data = &offset_byte;
	messages[1].length = 1U;
	messages[2].address = I915_XD_HPD_DDC_EDID;
	messages[2].read = 1;
	messages[2].data = buffer;
	messages[2].length = size;

	/* The transfer on the connector's pin, without the controller's interrupts. */
	request.pin = ddc->pin;
	request.messages = &messages[first];
	request.count = 3U - first;
	request.interrupts = 0;
	request.display9 = 0;
	request.noted = NULL;
	request.context = NULL;
	drv_i915_xd_gmbus_transfer(&ddc->hpd->gmbus, ddc->io, &request, &result);

	/* No monitor answered. */
	if (result.outcome == I915_XD_GMBUS_NO_ACK || result.outcome == I915_XD_GMBUS_BAD_PIN)
		return ENXIO;

	/* The controller failed. */
	if (result.outcome != I915_XD_GMBUS_DONE)
		return EIO;

	/* Succeeded: the bytes are read. */
	return 0;
}
