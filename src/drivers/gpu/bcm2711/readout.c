/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display path of the BCM2711: reading the firmware's display (stage N0).
 *
 * Before the driver may take the screen over, it has to know what the
 * firmware set up: which HDMI port the firmware's screen goes out of, which
 * compositor channel feeds that port, and whether the channel's display list
 * shows the firmware's framebuffer one to one.  N0 learns this by reading
 * only: get tags of the firmware mailbox and register reads.  Nothing is
 * written, so the picture is untouched.
 *
 * Two guards come before the first register read.  The VideoCore firmware
 * stamps its revision with its build time, while an emulator answers a small
 * constant; an emulator (QEMU's raspi4b has no compositor and faults on its
 * registers) is left alone.  And when the HDMI state machine's clock is off,
 * the board shows nothing and the display hardware is not read at all.
 *
 * The register offsets and bit positions are hardware facts of the BCM2711.
 */

#include <stdbool.h>
#include <stdint.h>

#include <kern/device-io.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"

/* Asks the firmware's revision, which a real firmware stamps with its build time. */
#define READOUT_TAG_REVISION		0x00000001U

/* Asks the width and height of the firmware's framebuffer. */
#define READOUT_TAG_SCREEN_SIZE		0x00040003U

/* Asks the bits per pixel of the firmware's framebuffer. */
#define READOUT_TAG_SCREEN_DEPTH	0x00040005U

/* Asks the order of the colour components of the firmware's framebuffer. */
#define READOUT_TAG_SCREEN_ORDER	0x00040006U

/* Asks the bytes per line of the firmware's framebuffer. */
#define READOUT_TAG_SCREEN_PITCH	0x00040008U

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

/* The channel control word: enable, and the output width and height. */
#define READOUT_CHANNEL_ENABLE		0x80000000U
#define READOUT_CHANNEL_WIDTH_SHIFT	16U
#define READOUT_CHANNEL_SIZE_MASK	0x1fffU

/* The channel status word: its mode, bits 31:30 (0 off, 1 starting, 2 running, 3 frame end). */
#define READOUT_CHANNEL_MODE_SHIFT	30U
#define READOUT_CHANNEL_MODE_MASK	0x3U

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

/* A timing generator: the active width (in clocks) and the active height. */
#define READOUT_TIMING_HORIZONTAL	0x0010U
#define READOUT_TIMING_VERTICAL		0x0018U
#define READOUT_TIMING_ACTIVE_MASK	0xffffU

/* The timing generators of both HDMI ports send two pixels per clock. */
#define READOUT_PIXELS_PER_CLOCK	2U

/* The timing generator window the readout needs. */
#define READOUT_TIMING_SPAN		0x001cU

/* The VideoCore sees ARM memory below 1 GiB at this alias. */
#define READOUT_BUS_ALIAS_MASK		0x3fffffffU

static uint32_t read_compositor(const struct bcm2711_display *display, uint32_t offset);
static bool emulator_firmware(void);
static void read_screen(const struct drv_bcm2711_boot_screen *screen);
static void read_channels(struct bcm2711_display *display);
static void read_list(struct bcm2711_display *display);
static void read_timing(const struct bcm2711_display *display, unsigned port);
static int choose_port(struct bcm2711_display *display);

/*
 * Runs stage N0: reads what the firmware set up on the display path.
 *
 * Returns 0 when the firmware's display was read; ENODEV when there is
 * nothing to read (an emulator, a board that shows nothing, a window too
 * small); ECANCELED when the boot parameters stop the display before N0.
 */
int
bcm2711_display_readout(
	struct bcm2711_display *display,
	const struct drv_bcm2711_boot_screen *screen)
{
	uint32_t hdmi_hz;
	uint32_t core_hz;
	bool allowed;
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

	/* Honors rpi4gpu.stop=N0. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "N0");
	if (!allowed) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 stopped by boot parameter");
		return ECANCELED;
	}

	/* Marks the start of the stage on the screen. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 begin");

	/* Leaves an emulator's missing display hardware unread. */
	emulator = emulator_firmware();
	if (emulator) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 emulator: no display registers read");
		return ENODEV;
	}

	/* Shows the firmware's screen as the mailbox and the boot handoff describe it. */
	read_screen(screen);

	/* Leaves the hardware unread when the HDMI state machine has no clock. */
	error = bcm2711_clock_hz(READOUT_CLOCK_HDMI_MACHINE, &hdmi_hz);
	if (error != 0 || hdmi_hz == 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 hdmi clock off (%d): no display", error);
		return ENODEV;
	}

	/* Requires the HVS core clock before its first register access as well. */
	error = bcm2711_clock_hz(4U, &core_hz);
	if (error != 0 || core_hz == 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 core clock off (%d): no display", error);
		return ENODEV;
	}

	/* Refuses a compositor window too small for the registers and the list memory. */
	if (display->compositor.size < READOUT_COMPOSITOR_SPAN) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 hvs window too small: no display");
		return ENODEV;
	}

	/* Reads which channel feeds which port and what each channel is doing. */
	read_channels(display);

	/* Picks the port the firmware's screen goes out of and reads its list. */
	error = choose_port(display);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 boot output unknown (%d)", error);
		return error;
	}

	/* Reads only the list of the uniquely proven console output. */
	display->channel = display->port_channel[display->port];
	read_list(display);

	/* Reads both timing generators. */
	read_timing(display, 0);
	read_timing(display, 1);

	/* Publishes the readout to the takeover stages. */
	display->readout_done = true;
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 ok hdmi%u ch %u %s",
			   (unsigned)display->port,
			   (unsigned)display->channel,
			   display->screen_matches ? "shows fb 1:1" : "does not show fb 1:1");

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
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 firmware revision unknown (%d)", error);
		return true;
	}

	/* Shows the revision; a real firmware's is its build time. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 firmware revision %08x", (unsigned)values[0]);
	if (values[0] < READOUT_REAL_REVISION_MIN)
		return true;

	/* A build time: the real VideoCore firmware. */
	return false;
}

/* Shows the firmware's framebuffer as the mailbox and the boot handoff describe it. */
static void
read_screen(
	const struct drv_bcm2711_boot_screen *screen)
{
	uint32_t size[2];
	uint32_t depth[1];
	uint32_t order[1];
	uint32_t pitch[1];
	int error;

	/* Asks the framebuffer's width and height; zero stands for no answer. */
	size[0] = 0;
	size[1] = 0;
	error = bcm2711_firmware_get(READOUT_TAG_SCREEN_SIZE, size, 0U, 2U);
	if (error != 0) {
		size[0] = 0;
		size[1] = 0;
	}

	/* Asks the bits per pixel; zero stands for no answer. */
	depth[0] = 0;
	error = bcm2711_firmware_get(READOUT_TAG_SCREEN_DEPTH, depth, 0U, 1U);
	if (error != 0)
		depth[0] = 0;

	/* Asks the order of the colour components; zero stands for no answer. */
	order[0] = 0;
	error = bcm2711_firmware_get(READOUT_TAG_SCREEN_ORDER, order, 0U, 1U);
	if (error != 0)
		order[0] = 0;

	/* Asks the bytes per line; zero stands for no answer. */
	pitch[0] = 0;
	error = bcm2711_firmware_get(READOUT_TAG_SCREEN_PITCH, pitch, 0U, 1U);
	if (error != 0)
		pitch[0] = 0;

	/* Shows the mailbox's answers and then where the handoff says the pixels are. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 fb %ux%u %ubpp order %u pitch %u",
			   (unsigned)size[0],
			   (unsigned)size[1],
			   (unsigned)depth[0],
			   (unsigned)order[0],
			   (unsigned)pitch[0]);
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 fb at %llx+%llx (handoff %ux%u pitch %u)",
			   (unsigned long long)screen->physical,
			   (unsigned long long)screen->size,
			   (unsigned)screen->width,
			   (unsigned)screen->height,
			   (unsigned)screen->pitch);
}

/* Reads which channel feeds each HDMI port and shows every channel's state. */
static void
read_channels(
	struct bcm2711_display *display)
{
	uint32_t out_control;
	uint32_t control;
	uint32_t status;
	uint32_t next;
	uint32_t now;
	uint32_t channel;

	/* Reads the whole block's enable and the two output choices. */
	out_control = read_compositor(display, READOUT_OUT_CONTROL);
	display->port_channel[0] = (read_compositor(display, READOUT_MUX_HDMI0) >> READOUT_MUX_SHIFT) & READOUT_MUX_MASK;
	display->port_channel[1] = (read_compositor(display, READOUT_MUX_HDMI1) >> READOUT_MUX_SHIFT) & READOUT_MUX_MASK;
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 hvs %s hdmi0<-ch %u hdmi1<-ch %u (3 none)",
			   (out_control & READOUT_OUT_ENABLE) != 0 ? "on" : "off",
			   (unsigned)display->port_channel[0],
			   (unsigned)display->port_channel[1]);

	/* Shows each channel: enabled or not, mode, size and its two lists. */
	for (channel = 0; channel < BCM2711_CHANNEL_COUNT; channel++) {
		/* Reads the channel's control, status and list positions. */
		control = read_compositor(display, READOUT_CHANNEL_CONTROL(channel));
		status = read_compositor(display, READOUT_CHANNEL_STATUS(channel));
		next = read_compositor(display, READOUT_LIST_NEXT(channel));
		now = read_compositor(display, READOUT_LIST_NOW(channel));

		/* Shows the channel on one line. */
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 ch%u %s mode %u %ux%u list next %u now %u",
				   (unsigned)channel,
				   (control & READOUT_CHANNEL_ENABLE) != 0 ? "on" : "off",
				   (unsigned)((status >> READOUT_CHANNEL_MODE_SHIFT) & READOUT_CHANNEL_MODE_MASK),
				   (unsigned)((control >> READOUT_CHANNEL_WIDTH_SHIFT) & READOUT_CHANNEL_SIZE_MASK),
				   (unsigned)(control & READOUT_CHANNEL_SIZE_MASK),
				   (unsigned)next,
				   (unsigned)now);
	}
}

/*
 * Decodes the display list the chosen channel shows now and compares its
 * retained planes with the firmware's framebuffer.
 */
static void
read_list(
	struct bcm2711_display *display)
{
	const struct bcm2711_list_plane *plane;
	uint32_t now;
	uint32_t index;
	uint32_t shown;

	/* A port without a channel has no list. */
	if (display->channel == BCM2711_NO_CHANNEL) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 no channel feeds hdmi%u", (unsigned)display->port);
		return;
	}

	/* Refuses a current list position outside the list memory. */
	now = read_compositor(display, READOUT_LIST_NOW(display->channel));
	if (now >= BCM2711_LIST_WORDS) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 list position %u out of range", (unsigned)now);
		return;
	}

	/* Decodes the list in place, through the uncached mapping. */
	bcm2711_list_decode((const volatile uint32_t *)(display->compositor.mapped + READOUT_LIST_MEMORY), now, &display->list);
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 list words %u..%u planes %u%s",
			   (unsigned)display->list.start,
			   (unsigned)display->list.end,
			   (unsigned)display->list.plane_count,
			   display->list.valid ? "" : " (malformed)");

	/* Shows the planes the readout kept. */
	shown = display->list.plane_count;
	if (shown > BCM2711_LIST_PLANES)
		shown = BCM2711_LIST_PLANES;
	for (index = 0; index < shown; index++) {
		/* Shows one plane on one line. */
		plane = &display->list.planes[index];
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 plane%u fmt %u ord %u%s %ux%u at %u,%u ptr %08x p %u",
				   (unsigned)index,
				   (unsigned)plane->format,
				   (unsigned)plane->order,
				   plane->scaled ? " scaled" : "",
				   (unsigned)plane->width,
				   (unsigned)plane->height,
				   (unsigned)plane->x,
				   (unsigned)plane->y,
				   (unsigned)plane->pointer,
				   (unsigned)plane->pitch);
	}

	/* Checks every decoded plane against the actual boot framebuffer. */
	display->screen_matches = bcm2711_list_screen_matches(&display->list, &display->screen);
}

/* Shows one timing generator's enables and active size. */
static void
read_timing(
	const struct bcm2711_display *display,
	unsigned port)
{
	const struct bcm2711_window *window;
	uint32_t control;
	uint32_t vcontrol;
	uint32_t horizontal;
	uint32_t vertical;

	/* Skips a generator that P0 could not map, or whose window is too small. */
	window = &display->timing[port];
	if (window->mapped == NULL || window->size < READOUT_TIMING_SPAN) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 pv of hdmi%u not mapped", port);
		return;
	}

	/* Reads the enables and the active width and height. */
	control = kern_mmio_read32(window->mapped + READOUT_TIMING_CONTROL);
	vcontrol = kern_mmio_read32(window->mapped + READOUT_TIMING_VCONTROL);
	horizontal = kern_mmio_read32(window->mapped + READOUT_TIMING_HORIZONTAL);
	vertical = kern_mmio_read32(window->mapped + READOUT_TIMING_VERTICAL);

	/* Shows them; the width is in clocks of two pixels each. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "N0 pv of hdmi%u %s video %s %ux%u",
			   port,
			   (control & READOUT_TIMING_ENABLE) != 0 ? "on" : "off",
			   (vcontrol & READOUT_TIMING_VIDEO_ENABLE) != 0 ? "on" : "off",
			   (unsigned)((horizontal & READOUT_TIMING_ACTIVE_MASK) * READOUT_PIXELS_PER_CLOCK),
			   (unsigned)(vertical & READOUT_TIMING_ACTIVE_MASK));
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
