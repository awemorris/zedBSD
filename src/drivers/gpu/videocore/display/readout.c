/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display path of the BCM2711: reading the firmware's display.
 *
 * Before the driver may take the screen over, it has to know what the
 * firmware set up: which HDMI port the firmware's screen goes out of, which
 * compositor channel feeds that port, and whether the channel's display list
 * shows the firmware's framebuffer one to one.  The readout prepares the clocks
 * needed for register access, then reads the display configuration.  It
 * does not write pipeline registers or release the firmware framebuffer.
 *
 * Two guards come before the first register read.  The VideoCore firmware
 * stamps its revision with its build time, while an emulator answers a small
 * constant; an emulator (QEMU's raspi4b has no compositor and faults on its
 * registers) is left alone.  The HSM and core clocks must then be enabled
 * at usable rates; a provider failure leaves the display registers unread.
 *
 * The register offsets and bit positions are hardware facts of the BCM2711.
 */

#include <stdbool.h>
#include <stdint.h>

#include <kern/device-io.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/bcm2711-private.h"

/* Asks the firmware's revision, which a real firmware stamps with its build time. */
#define READOUT_TAG_REVISION		0x00000001U

/*
 * The lowest revision taken for a real firmware: a build time after 2004.
 * Every VideoCore VI firmware was built long after that; QEMU answers a small
 * constant.
 */
#define READOUT_REAL_REVISION_MIN	0x40000000U

/* The firmware's clock of the HDMI state machine (firmware wiki: M2MC). */
#define READOUT_CLOCK_HDMI_MACHINE	13U

/* The compositor: its output control word and the enable bit of the whole block. */
#define READOUT_OUT_CONTROL		0x0000U
#define READOUT_OUT_ENABLE		0x80000000U

/* The compositor: the words that choose the channel of the HDMI0 and HDMI1 outputs. */
#define READOUT_MUX_HDMI0		0x0018U
#define READOUT_MUX_HDMI1		0x0014U
#define READOUT_MUX_SHIFT		30U
#define READOUT_MUX_MASK		0x3U

/* The compositor: per channel, the next and the current display list. */
#define READOUT_LIST_NEXT(channel)	(0x0020U + 4U * (channel))
#define READOUT_LIST_NOW(channel)	(0x0030U + 4U * (channel))

/* The compositor: per channel, its control and status words. */
#define READOUT_CHANNEL_CONTROL(channel) (0x0040U + 0x10U * (channel))
#define READOUT_CHANNEL_STATUS(channel)	(0x0048U + 0x10U * (channel))

/* The channel control word's enable. */
#define READOUT_CHANNEL_ENABLE		0x80000000U

/* Where the display list memory starts in the compositor's window. */
#define READOUT_LIST_MEMORY		0x4000U

/* The compositor window the readout needs: registers and the whole list memory. */
#define READOUT_COMPOSITOR_SPAN		(READOUT_LIST_MEMORY + 4U * BCM2711_LIST_WORDS)

/* A timing generator: its control word with the enable bit. */
#define READOUT_TIMING_CONTROL		0x0000U
#define READOUT_TIMING_ENABLE		0x00000001U

/* A timing generator: its vertical control word with the video enable bit. */
#define READOUT_TIMING_VCONTROL		0x0004U
#define READOUT_TIMING_VIDEO_ENABLE	0x00000001U

/* The timing generator window the readout needs. */
#define READOUT_TIMING_SPAN		0x001cU

static uint32_t read_compositor(const struct bcm2711_display *display, uint32_t offset);
static bool emulator_firmware(void);
static void read_channels(struct bcm2711_display *display);
static void read_list(struct bcm2711_display *display);
static int choose_port(struct bcm2711_display *display);

/*
 * Reads what the firmware set up on the display path.
 *
 * Returns 0 when the firmware's display was read; ENODEV when there is
 * nothing to read (an emulator, a board that shows nothing, a window too
 * small).
 */
int
bcm2711_display_readout(
	struct bcm2711_display *display,
	const struct drv_bcm2711_boot_screen *screen)
{
	uint32_t hdmi_hz;
	uint32_t core_hz;
	bool emulator;
	int error;

	/* Starts from nothing read. */
	display->readout_done = false;
	display->screen = *screen;
	display->port_channel[0] = BCM2711_NO_CHANNEL;
	display->port_channel[1] = BCM2711_NO_CHANNEL;
	display->port = 0;
	display->channel = BCM2711_NO_CHANNEL;
	display->list.valid = false;
	display->list.plane_count = 0;
	display->screen_matches = false;

	/* Leaves an emulator's missing display hardware unread. */
	emulator = emulator_firmware();
	if (emulator) {
		kern_logf("rpi4gpu: emulator firmware: no display registers read\n");
		return ENODEV;
	}

	/* Prepares the HSM minimum before touching the firmware display registers. */
	error = bcm2711_clock_prepare(
		READOUT_CLOCK_HDMI_MACHINE,
		120000000U,
		&hdmi_hz);
	if (error != 0) {
		kern_logf("rpi4gpu: hdmi clock preparation failed (%d): no display\n", error);
		return ENODEV;
	}

	/* Requires the HVS core clock before its first register access as well. */
	error = bcm2711_clock_prepare(4U, 0U, &core_hz);
	if (error != 0) {
		kern_logf("rpi4gpu: core clock preparation failed (%d): no display\n", error);
		return ENODEV;
	}

	/* Refuses a compositor window too small for the registers and the list memory. */
	if (display->compositor.size < READOUT_COMPOSITOR_SPAN) {
		kern_logf("rpi4gpu: compositor window too small: no display\n");
		return ENODEV;
	}

	/* Reads which channel feeds which port. */
	read_channels(display);

	/* Picks the port the firmware's screen goes out of and reads its list. */
	error = choose_port(display);
	if (error != 0) {
		kern_logf("rpi4gpu: boot output unknown (%d)\n", error);
		return error;
	}

	/* Reads only the list of the uniquely proven console output. */
	display->channel = display->port_channel[display->port];
	read_list(display);

	/* Publishes the readout to the takeover steps. */
	display->readout_done = true;

	/* Succeeded: the firmware's display is known. */
	return 0;
}

/* Reads one word of the compositor's window. */
static uint32_t
read_compositor(
	const struct bcm2711_display *display,
	uint32_t offset)
{
	uint32_t value;

	/* Reads the register through the uncached mapping. */
	value = kern_mmio_read32(display->compositor.mapped + offset);

	/* Reports the register's value. */
	return value;
}

/* Reports whether the firmware's revision is an emulator's constant. */
static bool
emulator_firmware(
	void)
{
	uint32_t values[1];
	int error;

	/* Asks the firmware's revision. */
	values[0] = 0;
	error = bcm2711_firmware_get(READOUT_TAG_REVISION, values, 0U, 1U);
	if (error != 0) {
		kern_logf("rpi4gpu: firmware revision unknown (%d)\n", error);
		return true;
	}

	/* A real firmware's revision is its build time. */
	if (values[0] < READOUT_REAL_REVISION_MIN)
		return true;

	/* A build time: the real VideoCore firmware. */
	return false;
}

/* Reads which channel feeds each HDMI port. */
static void
read_channels(
	struct bcm2711_display *display)
{
	/* Reads the two output choices. */
	display->port_channel[0] = (read_compositor(display, READOUT_MUX_HDMI0) >> READOUT_MUX_SHIFT) & READOUT_MUX_MASK;
	display->port_channel[1] = (read_compositor(display, READOUT_MUX_HDMI1) >> READOUT_MUX_SHIFT) & READOUT_MUX_MASK;
}

/*
 * Decodes the display list the chosen channel shows now and compares its
 * retained planes with the firmware's framebuffer.
 */
static void
read_list(
	struct bcm2711_display *display)
{
	uint32_t now;

	/* A port without a channel has no list. */
	if (display->channel == BCM2711_NO_CHANNEL) {
		kern_logf("rpi4gpu: no channel feeds hdmi%u\n", (unsigned)display->port);
		return;
	}

	/* Refuses a current list position outside the list memory. */
	now = read_compositor(display, READOUT_LIST_NOW(display->channel));
	if (now >= BCM2711_LIST_WORDS) {
		kern_logf("rpi4gpu: list position %u out of range\n", (unsigned)now);
		return;
	}

	/* Decodes the list in place, through the uncached mapping. */
	bcm2711_list_decode((const volatile uint32_t *)(display->compositor.mapped + READOUT_LIST_MEMORY), now, &display->list);

	/* Checks every decoded plane against the actual boot framebuffer. */
	display->screen_matches = bcm2711_list_screen_matches(&display->list, &display->screen);

	/* Succeeded: the list and its comparison with the framebuffer are recorded. */
	return;
}

/* Selects only a running output whose list displays the boot framebuffer. */
static int
choose_port(
	struct bcm2711_display *display)
{
	struct bcm2711_list candidate;
	const struct bcm2711_window *timing;
	uint32_t control;
	uint32_t video;
	uint32_t channel;
	uint32_t current;
	uint32_t port;
	uint32_t selected;
	bool matches;

	/* Keeps the selection invalid until exactly one proven output is found. */
	selected = BCM2711_HDMI_COUNT;
	control = read_compositor(display, READOUT_OUT_CONTROL);
	if ((control & READOUT_OUT_ENABLE) == 0)
		return ENODEV;

	/* Checks both ports rather than treating any HDMI0 mux as the boot output. */
	for (port = 0; port < BCM2711_HDMI_COUNT; port++) {
		/* Ignores disconnected muxes and missing timing generators. */
		channel = display->port_channel[port];
		if (channel >= BCM2711_CHANNEL_COUNT)
			continue;
		timing = &display->timing[port];
		if (timing->mapped == NULL || timing->size < READOUT_TIMING_SPAN)
			continue;

		/* Requires both the compositor channel and its pixel stream to run. */
		control = read_compositor(display, READOUT_CHANNEL_CONTROL(channel));
		if ((control & READOUT_CHANNEL_ENABLE) == 0)
			continue;
		control = kern_mmio_read32(timing->mapped + READOUT_TIMING_CONTROL);
		video = kern_mmio_read32(timing->mapped + READOUT_TIMING_VCONTROL);
		if ((control & READOUT_TIMING_ENABLE) == 0)
			continue;
		if ((video & READOUT_TIMING_VIDEO_ENABLE) == 0)
			continue;

		/* Proves that this output displays the framebuffer from the handoff. */
		current = read_compositor(display, READOUT_LIST_NOW(channel));
		bcm2711_list_decode(
			(const volatile uint32_t *)(display->compositor.mapped + READOUT_LIST_MEMORY),
			current,
			&candidate);
		matches = bcm2711_list_screen_matches(&candidate, &display->screen);
		if (!matches)
			continue;

		/* Refuses ambiguous mirrors instead of silently changing the chosen port. */
		if (selected != BCM2711_HDMI_COUNT)
			return EBUSY;
		selected = port;
	}

	/* A mux alone cannot identify the firmware's actual console output. */
	if (selected == BCM2711_HDMI_COUNT)
		return ENODEV;

	/* Succeeded: records the unique proven boot output. */
	display->port = selected;
	return 0;
}
