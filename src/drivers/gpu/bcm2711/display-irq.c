/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Device-source IRQ service for scanout and flips, independent of DRM objects. */
#include <stdbool.h>
#include <stdint.h>

#include <kern/device-io.h>
#include <kern/irq.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/display-program.h"

static bool service_timing(void *owner);
static bool service_compositor(void *owner);

/*
 * Gives each registered, masked line a persistent device-source service.
 * The display owns the callback arguments for the entire kernel lifetime.
 */
int
bcm2711_display_irq_prepare(
	struct bcm2711_display *display)
{
	uint32_t port;

	/* Requires all source handlers before allowing the first hardware reset. */
	if (!display->compositor_irq.registered)
		return ENODEV;
	for (port = 0; port < BCM2711_TIMING_COUNT; port++) {
		/* A missing PV handler cannot be substituted by an enabled level IRQ. */
		if (!display->timing_irq[port].registered)
			return ENODEV;
	}

	/* Initializes IRQ-owned completion state before publishing callback owners. */
	bcm2711_display_flip_init(display);
	display->adoption_armed = false;
	display->first_frame = false;
	display->underruns = 0;
	display->compositor_source.display = display;
	display->compositor_source.port = BCM2711_HDMI_COUNT;
	display->compositor_irq.owner = &display->compositor_source;
	display->compositor_irq.service = service_compositor;
	for (port = 0; port < BCM2711_TIMING_COUNT; port++) {
		/* Each PV callback borrows only its own mapped source registers. */
		display->timing_source[port].display = display;
		display->timing_source[port].port = port;
		display->timing_irq[port].owner = &display->timing_source[port];
		display->timing_irq[port].service = service_timing;
	}

	/* Succeeded: callbacks can service sources before the common handler sends EOI. */
	return 0;
}

/*
 * Opens the serviced HVS or PV line at its prepared initialization point.
 */
int
bcm2711_display_irq_open(
	struct bcm2711_display *display,
	uint32_t region)
{
	struct bcm2711_irq_line *line;

	/* Only the compositor and the two timing roles own display IRQs. */
	if (region == BCM2711_REGION_HVS) {
		line = &display->compositor_irq;
	} else if (region >= BCM2711_REGION_PV0 && region <= BCM2711_REGION_PV1) {
		line = &display->timing_irq[region - BCM2711_REGION_PV0];
	} else {
		return EINVAL;
	}

	/* A generic discovery handler cannot retire a live device source. */
	if (!line->registered || line->service == NULL)
		return ENODEV;
	kern_irq_unmask(line->irq);

	/* Succeeded: the persistent source callback owns delivery. */
	return 0;
}

/*
 * Discards stale PV status before opening the new mode's pixel stream.
 */
void
bcm2711_display_frame_arm(
	struct bcm2711_display *display)
{
	unsigned long enabled;

	/* Serializes source acknowledgement and arming against every CPU's handler. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->adoption_armed = false;
	kern_mmio_write32(display->timing[display->port].mapped + 0x28, 0x80);
	display->first_frame = false;
	display->adoption_armed = true;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the selected frame source is armed under the flip guard. */
	return;
}

/*
 * Stops IRQ delivery after an incomplete boot attempt without freeing owners.
 */
void
bcm2711_display_irq_mask(
	struct bcm2711_display *display)
{
	uint32_t port;
	unsigned long enabled;

	/* Stops initial completion before masking lines on every delivery CPU. */
	enabled = spin_lock_irqsave(&display->flip.guard);

	display->adoption_armed = false;

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Keeps a failed display from issuing unowned events during later boot. */
	kern_irq_mask(display->compositor_irq.irq);
	for (port = 0; port < BCM2711_TIMING_COUNT; port++) {
		/* The register mappings and callback owners remain alive. */
		kern_irq_mask(display->timing_irq[port].irq);
	}

	/* Succeeded: the display sources no longer admit new service. */
	return;
}

/* Acknowledges owned vblank and confirms the new list before unmasking underrun. */
static bool
service_timing(
	void *owner)
{
	struct bcm2711_display_irq_source *source;
	struct bcm2711_display *display;
	volatile uint8_t *timing;
	uint32_t status;
	uint32_t current;
	uint32_t control;
	unsigned long enabled;

	/* Ignores another device's source on a shared PV interrupt line. */
	source = owner;
	display = source->display;
	timing = display->timing[source->port].mapped;
	enabled = spin_lock_irqsave(&display->flip.guard);

	status = kern_mmio_read32(timing + 0x28);
	if ((status & 0x80U) == 0) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return false;
	}

	/* Quiets the owned source before sampling the current display list. */
	kern_mmio_write32(timing + 0x28, 0x80);

	/* Only the selected new-mode frame can complete this boot commit. */
	if (source->port == display->port) {
		/* The HVS current pointer proves adoption rather than just submission. */
		current = kern_mmio_read32(display->compositor.mapped + 0x30);
		if (display->adoption_armed &&
		    current == 43U &&
		    !display->first_frame) {
			/* Clears stale underrun before enabling its channel-zero source. */
			kern_mmio_write32(display->compositor.mapped + 0x04, 0x200);
			control = kern_mmio_read32(display->compositor.mapped);
			kern_mmio_write32(display->compositor.mapped, control | 0x200U);
			display->first_frame = true;
		}

		/* Runtime completion uses the same sampled list and selected-source guard. */
		bcm2711_display_flip_vblank_locked(display, current);
	}

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: an owned source was quieted before EOI. */
	return true;
}

/* Masks reported underrun sources and acknowledges all HVS channel status. */
static bool
service_compositor(
	void *owner)
{
	struct bcm2711_display_irq_source *source;
	struct bcm2711_display *display;
	uint32_t status;
	uint32_t control;
	uint32_t channel;
	uint32_t bit;
	unsigned long enabled;

	/* Records only actual HVS channel IRQs on the owned mapping. */
	source = owner;
	display = source->display;
	enabled = spin_lock_irqsave(&display->flip.guard);

	status = kern_mmio_read32(display->compositor.mapped + 0x04);
	if ((status & 0x3f3f3f00U) == 0) {
		spin_unlock_irqrestore(&display->flip.guard, enabled);
		return false;
	}

	/* Samples the source enables under the same guard as runtime publication. */
	control = kern_mmio_read32(display->compositor.mapped);
	for (channel = 0; channel < BCM2711_CHANNEL_COUNT; channel++) {
		/* Masks an enabled underrun before another level interrupt can recur. */
		if ((status & (1U << (9U + channel * 8U))) == 0)
			continue;
		bit = 1U << (9U + channel * 4U);
		if ((control & bit) == 0)
			continue;
		control &= ~bit;
		kern_mmio_write32(display->compositor.mapped, control);
		display->underruns++;
	}

	/* Retires device status before the common handler acknowledges the GIC. */
	kern_mmio_write32(display->compositor.mapped + 0x04, 0x3f3f3f00U);

	spin_unlock_irqrestore(&display->flip.guard, enabled);

	/* Succeeded: the HVS source cannot keep the level IRQ asserted. */
	return true;
}
