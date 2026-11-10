/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2712's HVS6 display: discovery, the readout of what the firmware
 * shows, the takeover of the firmware's display list, the measurement of the
 * pixel valve's vertical blank, and the publication of the display device.
 *
 * The firmware keeps the mode: the driver writes neither the compositor's
 * control, output buffers and colour conversion nor the pixel valves'
 * timing nor the HDMI encoders.  It writes only display-list memory past
 * the firmware's own lists, the list pointer of the channel the firmware
 * shows, and the pixel valve's vertical-blank interrupt enable.  Every wait
 * is bounded, and a list the hardware does not take is replaced by the
 * firmware's again.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/vc4/vc4_regs.h lines
 * 543-741 (the HVS6 registers of C0 and D0), 1350-1407 (the HVS6 plane
 * words), 145-215 (the pixel valve's interrupt registers), 1070 and
 * 1090-1096 (the RGBA8888 format and the ARGB order); vc4_plane.c lines
 * 816-831 and 978-1055 (the unified pixel memory), 1187-1201 and
 * 1736-1960 (an unscaled raster plane), vc4_hvs.c lines 1717-1720 (the
 * list memory at 0x4000).
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/display/hvs6.h"
#include "drivers/gpu/videocore/v3d71-private.h"

/* The device tree names of the parts (binding facts). */
#define HVS_COMPATIBLE			"brcm,bcm2712-hvs"
#define VALVE0_COMPATIBLE		"brcm,bcm2712-pixelvalve0"
#define VALVE1_COMPATIBLE		"brcm,bcm2712-pixelvalve1"

/* The compositor's common registers. */
#define HVS_VERSION			0x000U
#define HVS_LIST_SIZE			0x004U
#define HVS_VERSION_C0			0x53U
#define HVS_VERSION_D0			0x54U

/* The channel blocks' places: C0 from 0x030 every 0x20, D0 from 0x100 every 0x40. */
#define HVS_C0_CHANNEL_BASE		0x030U
#define HVS_C0_CHANNEL_STRIDE		0x020U
#define HVS_D0_CHANNEL_BASE		0x100U
#define HVS_D0_CHANNEL_STRIDE		0x040U

/* The channel registers' offsets inside a block, which differ by layout. */
#define CHANNEL_CTRL0			0x00U
#define CHANNEL_C0_LPTRS		0x0cU
#define CHANNEL_C0_STATUS		0x14U
#define CHANNEL_C0_DL			0x18U
#define CHANNEL_D0_LPTRS		0x10U
#define CHANNEL_D0_STATUS		0x18U
#define CHANNEL_D0_DL			0x1cU

/* The channel bits: enabled, the status's mode field and "running", and the list-head field. */
#define CHANNEL_ENABLED			0x80000000U
#define CHANNEL_MODE_SHIFT		13U
#define CHANNEL_MODE_MASK		3U
#define CHANNEL_MODE_RUN		2U
#define CHANNEL_HEAD_MASK		0xfffU

/* The display list memory: from 0x4000 in the window, the first 32 words the boot loader's. */
#define LIST_MEMORY_OFFSET		0x4000U
#define LIST_BOOT_WORDS			32U
#define LIST_ADDRESSABLE_WORDS		4096U

/* The driver's own lists: three regions of 128 words near the top of the addressable memory. */
#define OWN_REGION_WORDS		256U
#define OWN_REGIONS			3U

/* The words of a plane. */
#define PLANE_END			0x80000000U
#define PLANE_VALID			0x40000000U
#define PLANE_NEXT_SHIFT		24U
#define PLANE_NEXT_MASK			0x3fU
#define PLANE_ADDRESS_MODE_SHIFT	20U
#define PLANE_UNITY			0x8000U
#define PLANE_ORDER_SHIFT		13U
#define PLANE_WORDS			8U

/* The pixel valve's interrupt registers and the start of its vertical front porch. */
#define VALVE_INTEN			0x24U
#define VALVE_INTSTAT			0x28U
#define VALVE_VFP_START			0x80U

/* How long the hardware may take to adopt a list: 200 ms, in 1 ms polls. */
#define ADOPT_POLL_US			1000U
#define ADOPT_POLLS			200U

/*
 * How long the refresh measurement waits for its BCM2712_VBLANK_TIMED timed
 * vertical blanks (six frames, 100 ms at 60 Hz) before it counts over the
 * whole wait.
 */
#define VBLANK_LIMIT_MS			1000U

static void discover_valve(const struct drv_fdt *fdt, struct bcm2712_display *display, unsigned index, const char *compatible);
static uint32_t channel_register(const struct bcm2712_display *display, uint32_t channel, uint32_t c0_offset, uint32_t d0_offset);
static uint32_t list_read(const struct bcm2712_display *display, uint32_t word);
static void list_write(struct bcm2712_display *display, uint32_t word, uint32_t value);
static int decode_list(struct bcm2712_display *display, uint32_t head);
static int switch_list(struct bcm2712_display *display, uint32_t head, uint32_t *waited_us);
static bool valve_service(void *owner);
static uint64_t median_frame(const uint64_t *counters);
static bool device_vblank_service(void *owner);

/*
 * Finds the compositor and the pixel valves and maps them.
 *
 * Reads nothing from the hardware.  Returns ENODEV without a compositor.
 */
int
bcm2712_display_discover(
	const struct drv_fdt *fdt,
	struct bcm2712_display *display,
	const struct kern_boot_framebuffer *screen)
{
	uint32_t node;
	int error;

	/* Starts from an empty description. */
	kern_memset(display, 0, sizeof(*display));
	display->base.timing_irq[0].irq = BCM2711_NO_IRQ;
	display->base.timing_irq[1].irq = BCM2711_NO_IRQ;
	spin_init(&display->guard, LOCK_RANK_DEVICE, "bcm2712-hvs");
	if (screen != NULL) {
		display->screen = *screen;
		display->screen_known = true;
	}

	/* Finds and maps the compositor; a board without one has no display. */
	error = bcm2711_fdt_find(fdt, HVS_COMPATIBLE, &node);
	if (error != 0)
		return ENODEV;
	error = bcm2711_fdt_window(fdt, node, 0, &display->base.compositor);
	if (error != 0 || display->base.compositor.size < LIST_MEMORY_OFFSET + 4U * LIST_BOOT_WORDS) {
		kern_logf("hvs6: compositor registers unusable (%d)\n", error);
		return ENODEV;
	}
	error = bcm2711_map_window(&display->base.compositor);
	if (error != 0) {
		kern_logf("hvs6: compositor map failed (%d)\n", error);
		return ENODEV;
	}

	/* Finds the two pixel valves. */
	discover_valve(fdt, display, 0, VALVE0_COMPATIBLE);
	discover_valve(fdt, display, 1, VALVE1_COMPATIBLE);
	display->present = true;

	/* Succeeded: the compositor is mapped. */
	return 0;
}

/*
 * Reads the compositor and the firmware's list.
 *
 * Only reads.  Picks the channel the firmware shows (enabled and running)
 * and decodes its list.  Returns ENOTSUP for an unknown layout and ENODEV
 * when no channel runs.
 */
int
bcm2712_display_readout(
	struct bcm2712_display *display)
{
	volatile uint8_t *hvs;
	uint32_t channel;
	uint32_t control0;
	uint32_t status;
	uint32_t mode;
	bool found;
	int error;

	/* Needs the compositor discovery found. */
	if (!display->present)
		return ENODEV;
	hvs = display->base.compositor.mapped;

	/* Reads the layout: C0 and D0 place the channel blocks differently. */
	display->version = kern_mmio_read32(hvs + HVS_VERSION) & 0xffU;
	if (display->version == HVS_VERSION_D0) {
		display->channel_base = HVS_D0_CHANNEL_BASE;
		display->channel_stride = HVS_D0_CHANNEL_STRIDE;
	} else if (display->version == HVS_VERSION_C0) {
		display->channel_base = HVS_C0_CHANNEL_BASE;
		display->channel_stride = HVS_C0_CHANNEL_STRIDE;
	} else {
		kern_logf("hvs6: unknown HVS version %02x\n", display->version);
		return ENOTSUP;
	}

	/* Reads the list memory's size; the list memory is addressed by a 12-bit head. */
	display->list_words = kern_mmio_read32(hvs + HVS_LIST_SIZE);

	/* Picks the first enabled, running channel. */
	found = false;
	for (channel = 0; channel < BCM2712_HVS_CHANNELS; channel++) {
		/* Reads the channel's control and status. */
		control0 = kern_mmio_read32(hvs + channel_register(display, channel, CHANNEL_CTRL0, CHANNEL_CTRL0));
		status = kern_mmio_read32(hvs + channel_register(display, channel, CHANNEL_C0_STATUS, CHANNEL_D0_STATUS));

		/* Takes the first channel that is enabled and running. */
		mode = (status >> CHANNEL_MODE_SHIFT) & CHANNEL_MODE_MASK;
		if (!found && (control0 & CHANNEL_ENABLED) != 0 && mode == CHANNEL_MODE_RUN) {
			display->channel = channel;
			found = true;
		}
	}

	/* Refuses a display the firmware does not show on any channel. */
	if (!found) {
		kern_logf("hvs6: no running channel\n");
		return ENODEV;
	}

	/* Decodes the channel's list. */
	display->firmware_head = kern_mmio_read32(hvs + channel_register(display, display->channel, CHANNEL_C0_LPTRS, CHANNEL_D0_LPTRS)) & CHANNEL_HEAD_MASK;
	error = decode_list(display, display->firmware_head);
	if (error != 0) {
		kern_logf("hvs6: firmware list of channel %u unreadable (%d)\n", display->channel, error);
		return error;
	}

	/* Describes the console to the shared record from the firmware's first plane. */
	display->base.channel = display->channel;
	display->base.port = display->channel;
	display->base.screen.physical = display->planes[0].address;
	display->base.screen.size = display->screen.size;
	display->base.screen.width = display->planes[0].width;
	display->base.screen.height = display->planes[0].height;
	display->base.screen.pitch = display->planes[0].pitch;
	display->base.screen.format = 0;
	if (display->planes[0].order == 3U)
		display->base.screen.format = 1;
	display->base.screen.uncached = false;
	if (display->base.screen.size < (uint64_t)display->base.screen.pitch * display->base.screen.height)
		display->base.screen.size = (uint64_t)display->base.screen.pitch * display->base.screen.height;

	/* Succeeded: the firmware's channel and list are known. */
	return 0;
}

/*
 * Shows the firmware's list from the driver's own memory.
 *
 * Copies the firmware's list word for word into the driver's first region
 * and points the channel at it; the screen does not change.  Returns
 * ETIMEDOUT, after pointing the channel back, when the hardware does not
 * take the list.
 */
int
bcm2712_display_adopt(
	struct bcm2712_display *display)
{
	uint32_t limit;
	uint32_t words;
	uint32_t index;
	uint32_t plane;
	uint32_t at;
	uint32_t waited_us;
	int error;

	/* Needs a decoded firmware list. */
	if (!display->present || display->plane_count == 0)
		return ENODEV;

	/* Places the regions at the top of the memory a list head can name. */
	limit = display->list_words;
	if (limit > LIST_ADDRESSABLE_WORDS)
		limit = LIST_ADDRESSABLE_WORDS;
	if (limit < LIST_BOOT_WORDS + OWN_REGIONS * OWN_REGION_WORDS) {
		kern_logf("hvs6: list memory too small (%u)\n", display->list_words);
		return ENOSPC;
	}
	display->own_head = limit - OWN_REGIONS * OWN_REGION_WORDS;

	/* Refuses regions that overlap the firmware's list, or planes that do not fit one region. */
	words = 1U;
	for (plane = 0; plane < display->plane_count; plane++)
		words += display->planes[plane].words;
	if (display->firmware_end >= display->own_head || words + PLANE_WORDS > OWN_REGION_WORDS) {
		kern_logf("hvs6: firmware list %u-%u (%u words) does not fit below %u\n", display->firmware_head, display->firmware_end, words, display->own_head);
		return ENOSPC;
	}

	/* Copies the firmware's valid planes one after another, then the END word. */
	at = display->own_head;
	for (plane = 0; plane < display->plane_count; plane++) {
		/* Copies one plane's element word for word; its length field is relative. */
		for (index = 0; index < display->planes[plane].words; index++)
			list_write(display, at + index, list_read(display, display->planes[plane].start + index));
		at += display->planes[plane].words;
	}
	list_write(display, at, PLANE_END);
	display->own_words = words;
	kern_io_write_barrier();

	/* Points the channel at the copy and waits for the hardware to take it. */
	error = switch_list(display, display->own_head, &waited_us);
	if (error != 0) {
		(void)switch_list(display, display->firmware_head, &waited_us);
		kern_logf("hvs6: own list not taken (%d); firmware list restored\n", error);
		return error;
	}
	display->adopted = true;

	/* Succeeded: the channel shows the driver's list. */
	return 0;
}

/*
 * Measures the refresh from the pixel valve's vertical blanks.
 *
 * Enables the start-of-front-porch interrupt of the valve behind the
 * channel, waits for BCM2712_VBLANK_TIMED of them (at most
 * VBLANK_LIMIT_MS), puts the enable back as it was, and takes the refresh
 * from the median of the frames the monotonic counter timed between them,
 * so one blank served late does not move it.  Without the counter the
 * refresh is the count over the whole limit.
 */
int
bcm2712_display_vblank_test(
	struct bcm2712_display *display)
{
	struct bcm2711_irq_line *line;
	volatile uint8_t *valve;
	unsigned long enabled;
	uint32_t enable;
	uint64_t counters[BCM2712_VBLANK_TIMED];
	uint64_t count;
	uint64_t timed;
	uint64_t frequency;
	uint64_t started;
	uint64_t elapsed;
	uint64_t frame;
	uint64_t millihz;
	unsigned step;
	unsigned index;

	/* Needs a pixel valve: channel 2 has none. */
	if (!display->present || display->channel >= BCM2712_HVS_VALVES)
		return ENODEV;
	valve = display->base.timing[display->channel].mapped;
	line = &display->base.timing_irq[display->channel];
	if (valve == NULL || !line->registered)
		return ENODEV;

	/* Keeps the valve's interrupt enable as the firmware left it. */
	enable = kern_mmio_read32(valve + VALVE_INTEN);

	/* Installs the service, clears a stale event and enables the interrupt. */
	line->owner = display;
	line->service = valve_service;
	enabled = spin_lock_irqsave(&display->guard);

	display->vblank_count = 0;
	display->vblank_timed = 0;
	display->vblank_counter_hz = 0;

	spin_unlock_irqrestore(&display->guard, enabled);
	kern_mmio_write32(valve + VALVE_INTSTAT, VALVE_VFP_START);
	kern_mmio_write32(valve + VALVE_INTEN, enable | VALVE_VFP_START);
	kern_irq_unmask(line->irq);

	/* Looks each millisecond until enough vertical blanks came or the limit passed. */
	started = clock_ticks();
	count = 0;
	timed = 0;
	frequency = 0;
	for (step = 0; step < VBLANK_LIMIT_MS; step++) {
		kern_usleep_range(1000, 1000);
		enabled = spin_lock_irqsave(&display->guard);

		count = display->vblank_count;
		timed = display->vblank_timed;
		frequency = display->vblank_counter_hz;

		spin_unlock_irqrestore(&display->guard, enabled);
		if (timed >= BCM2712_VBLANK_TIMED)
			break;
	}

	/* Copies the timed blanks; the handler writes no more of them. */
	enabled = spin_lock_irqsave(&display->guard);

	for (index = 0; index < BCM2712_VBLANK_TIMED; index++)
		counters[index] = display->vblank_counters[index];

	spin_unlock_irqrestore(&display->guard, enabled);

	/* Puts the valve back as it was and masks the line. */
	kern_irq_mask(line->irq);
	kern_mmio_write32(valve + VALVE_INTEN, enable);
	kern_mmio_write32(valve + VALVE_INTSTAT, VALVE_VFP_START);
	elapsed = kern_ticks_to_ms(clock_ticks() - started);
	frame = 0;
	if (timed >= BCM2712_VBLANK_TIMED && frequency != 0)
		frame = median_frame(counters);

	/* Takes the refresh from the median frame, or else from the count over the time waited. */
	millihz = 0;
	if (frame != 0) {
		/* One frame of the counter's time. */
		millihz = (frequency * 1000U + frame / 2U) / frame;
	} else if (count != 0 && elapsed != 0) {
		/* The blanks counted over the whole wait. */
		millihz = (count * 1000000U + elapsed / 2U) / elapsed;
	}

	/* Refuses a valve that gave no interrupt. */
	if (millihz == 0)
		return EIO;

	/* Records the measured refresh, rounded to the whole hertz (the flip code budgets with it). */
	display->base.refresh_millihz = (uint32_t)((millihz + 500U) / 1000U) * 1000U;

	/* Succeeded: the valve's vertical blank reaches the driver. */
	return 0;
}

/*
 * Reports the window offset of one register of the shown channel.
 */
uint32_t
bcm2712_display_register_offset(
	const struct bcm2712_display *display,
	enum bcm2712_hvs_register which)
{
	uint32_t offset;

	/* Picks the register's place in each layout. */
	switch (which) {
	case BCM2712_HVS_CTRL0:
		offset = channel_register(display, display->channel, CHANNEL_CTRL0, CHANNEL_CTRL0);
		break;
	case BCM2712_HVS_LPTRS:
		offset = channel_register(display, display->channel, CHANNEL_C0_LPTRS, CHANNEL_D0_LPTRS);
		break;
	default:
		offset = channel_register(display, display->channel, CHANNEL_C0_DL, CHANNEL_D0_DL);
		break;
	}

	/* Reports the offset. */
	return offset;
}

/*
 * Publishes the display device on the driver's console list.
 *
 * Fills the shared record's mode, clocks and screen, starts the persistent
 * vertical-blank service of the shown channel's pixel valve, and has the
 * shared display device check its flips and register its node.  Needs the
 * takeover of the firmware's list and a measured refresh.
 */
int
bcm2712_display_attach_device(
	struct bcm2712_display *display)
{
	struct bcm2711_irq_line *line;
	volatile uint8_t *valve;
	uint32_t rate;
	uint32_t plane;
	uint32_t enable;
	int error;

	/* Needs the driver's console list and a measured refresh. */
	if (!display->adopted || display->base.refresh_millihz == 0)
		return ENODEV;
	if (display->channel >= BCM2712_HVS_VALVES)
		return ENODEV;
	valve = display->base.timing[display->channel].mapped;
	line = &display->base.timing_irq[display->channel];
	if (valve == NULL || !line->registered)
		return ENODEV;

	/* Refuses firmware planes whose pixel memory the driver's planes would share. */
	for (plane = 0; plane < display->plane_count && plane < BCM2712_HVS_LIST_PLANES; plane++) {
		/* A handle or a base inside the driver's ranges collides. */
		if (display->planes[plane].upm_handle >= BCM2712_UPM_LOWEST_HANDLE ||
		    display->planes[plane].upm_base + 129U > BCM2712_UPM_LOWEST_BASE) {
			kern_logf("hvs6: firmware plane %u pixel memory %u@%u collides with the driver's\n", plane, display->planes[plane].upm_handle, display->planes[plane].upm_base);
			return ENOSPC;
		}
	}

	/* The compositor's clock stays the firmware's: its rate is both the floor and the ceiling. */
	rate = 0;
	error = bcm2712_clock_get(0x00030002U, 4U, &rate);
	if (error != 0 || rate == 0)
		rate = 500000000U;
	display->base.console_core_hz = rate;
	display->base.max_core_hz = rate;
	display->attach_control0 = kern_mmio_read32(display->base.compositor.mapped + bcm2712_display_register_offset(display, BCM2712_HVS_CTRL0));

	/* Starts the flip state, then the persistent vertical-blank service that feeds it. */
	bcm2711_display_flip_init(&display->base);
	if (!display->base.flip.initialized)
		return ENOMEM;
	line->owner = display;
	line->service = device_vblank_service;
	kern_mmio_write32(valve + BCM2712_VALVE_INTSTAT, BCM2712_VALVE_VFP_START);
	enable = kern_mmio_read32(valve + BCM2712_VALVE_INTEN);
	kern_mmio_write32(valve + BCM2712_VALVE_INTEN, enable | BCM2712_VALVE_VFP_START);
	kern_irq_unmask(line->irq);
	display->base.scanout_started = true;

	/* Has the shared display device exercise the flips and publish its node. */
	error = bcm2711_display_register(&display->base);
	if (error != 0) {
		kern_logf("hvs6: display device not registered (%d)\n", error);
		return error;
	}

	/* Succeeded: the display node is published. */
	return 0;
}

/* Finds one pixel valve, maps it and installs its (masked) interrupt handler. */
static void
discover_valve(
	const struct drv_fdt *fdt,
	struct bcm2712_display *display,
	unsigned index,
	const char *compatible)
{
	uint32_t node;
	int error;

	/* Finds the valve; a missing one is only logged. */
	error = bcm2711_fdt_find(fdt, compatible, &node);
	if (error != 0)
		return;
	error = bcm2711_fdt_window(fdt, node, 0, &display->base.timing[index]);
	if (error != 0)
		return;
	error = bcm2711_map_window(&display->base.timing[index]);
	if (error != 0) {
		display->base.timing[index].mapped = NULL;
		return;
	}

	/* Installs the handler on the valve's line, masked. */
	error = bcm2711_fdt_gic_irq(fdt, node, 0, &display->base.timing_irq[index].irq);
	if (error != 0) {
		display->base.timing_irq[index].irq = BCM2711_NO_IRQ;
		return;
	}
	(void)bcm2711_irq_install(&display->base.timing_irq[index]);
}

/* Reports the window offset of one channel register in the layout the readout found. */
static uint32_t
channel_register(
	const struct bcm2712_display *display,
	uint32_t channel,
	uint32_t c0_offset,
	uint32_t d0_offset)
{
	uint32_t offset;

	/* The register's place inside the channel's block depends on the layout. */
	offset = c0_offset;
	if (display->version == HVS_VERSION_D0)
		offset = d0_offset;

	/* Reports the block's start plus the place. */
	return display->channel_base + channel * display->channel_stride + offset;
}

/* Reads one word of the display list memory. */
static uint32_t
list_read(
	const struct bcm2712_display *display,
	uint32_t word)
{
	uint32_t value;

	/* The memory starts at 0x4000 in the compositor's window. */
	value = kern_mmio_read32(display->base.compositor.mapped + LIST_MEMORY_OFFSET + word * 4U);

	/* Reports the word. */
	return value;
}

/* Writes one word of the display list memory. */
static void
list_write(
	struct bcm2712_display *display,
	uint32_t word,
	uint32_t value)
{
	/* The memory starts at 0x4000 in the compositor's window. */
	kern_mmio_write32(display->base.compositor.mapped + LIST_MEMORY_OFFSET + word * 4U, value);
}

/* Walks a list from its head, decoding up to BCM2712_HVS_LIST_PLANES unscaled-layout planes. */
static int
decode_list(
	struct bcm2712_display *display,
	uint32_t head)
{
	struct bcm2712_hvs_plane *plane;
	uint32_t word;
	uint32_t control;
	uint32_t next;
	uint32_t at;
	uint32_t limit;
	uint32_t pos2;
	uint32_t count;

	/* Walks element by element until the END word or the memory's end. */
	limit = display->list_words;
	if (limit > LIST_ADDRESSABLE_WORDS)
		limit = LIST_ADDRESSABLE_WORDS;
	word = head;
	count = 0;
	display->firmware_end = head;
	while (word < limit) {
		/* Stops at the END word. */
		control = list_read(display, word);
		if ((control & PLANE_END) != 0) {
			display->firmware_end = word;
			display->plane_count = count;
			return 0;
		}

		/* Refuses an element that does not say how long it is. */
		next = (control >> PLANE_NEXT_SHIFT) & PLANE_NEXT_MASK;
		if (next == 0) {
			display->firmware_end = word;
			display->plane_count = count;
			return EINVAL;
		}

		/* Skips an element the firmware keeps but marks not valid. */
		if ((control & PLANE_VALID) == 0) {
			word += next;
			continue;
		}

		/* Refuses a valid element too short for a plane. */
		if (next < PLANE_WORDS) {
			display->firmware_end = word;
			display->plane_count = count;
			return EINVAL;
		}

		/* Refuses more valid planes than the readout keeps. */
		if (count >= BCM2712_HVS_LIST_PLANES) {
			display->firmware_end = word;
			display->plane_count = count;
			return E2BIG;
		}

		/* Decodes the plane's words; a scaled plane has one more before the source size. */
		plane = &display->planes[count];
		plane->start = word;
		plane->words = next;
		plane->control = control;
		plane->format = control & 0x1fU;
		plane->order = (control >> PLANE_ORDER_SHIFT) & 3U;
		plane->address_mode = (control >> PLANE_ADDRESS_MODE_SHIFT) & 7U;
		plane->scaled = (control & PLANE_UNITY) == 0;
		plane->x = list_read(display, word + 1U) & 0x1fffU;
		plane->y = (list_read(display, word + 1U) >> 16) & 0x1fffU;
		at = word + 3U;
		if (plane->scaled)
			at++;
		pos2 = list_read(display, at);
		plane->width = (pos2 & 0x1fffU) + 1U;
		plane->height = ((pos2 >> 16) & 0x1fffU) + 1U;
		plane->pointer0 = list_read(display, at + 2U);
		plane->address = ((uint64_t)(plane->pointer0 & 0xffU) << 32) | list_read(display, at + 3U);
		plane->pitch = list_read(display, at + 4U) & 0x1ffffU;
		plane->upm_handle = ((plane->pointer0 >> 10) & 0x1fU) + 1U;
		plane->upm_base = (plane->pointer0 >> 16) & 0x1fffU;
		count++;
		word += next;
	}

	/* Refuses a list that ran off the memory. */
	display->plane_count = count;
	return EINVAL;
}

/* Points the channel at a list and waits until the hardware reports it as current. */
static int
switch_list(
	struct bcm2712_display *display,
	uint32_t head,
	uint32_t *waited_us)
{
	volatile uint8_t *hvs;
	uint32_t pointers;
	uint32_t current;
	uint32_t poll;

	/* Writes the new head into the channel's list pointer, keeping the other bits. */
	hvs = display->base.compositor.mapped;
	pointers = kern_mmio_read32(hvs + channel_register(display, display->channel, CHANNEL_C0_LPTRS, CHANNEL_D0_LPTRS));
	pointers = (pointers & ~CHANNEL_HEAD_MASK) | head;
	kern_mmio_write32(hvs + channel_register(display, display->channel, CHANNEL_C0_LPTRS, CHANNEL_D0_LPTRS), pointers);

	/* Polls the list the hardware uses until it is the new one, at most 200 ms. */
	for (poll = 0; poll <= ADOPT_POLLS; poll++) {
		/* Stops when the hardware took the list. */
		current = kern_mmio_read32(hvs + channel_register(display, display->channel, CHANNEL_C0_DL, CHANNEL_D0_DL)) & CHANNEL_HEAD_MASK;
		if (current == head) {
			*waited_us = poll * ADOPT_POLL_US;
			return 0;
		}

		/* Waits one poll. */
		kern_usleep_range(ADOPT_POLL_US, ADOPT_POLL_US);
	}

	/* The hardware never took the list. */
	*waited_us = poll * ADOPT_POLL_US;
	return ETIMEDOUT;
}

/* Quiets the pixel valve's vertical-blank interrupt and counts it. */
static bool
valve_service(
	void *owner)
{
	struct bcm2712_display *display;
	volatile uint8_t *valve;
	uint32_t status;
	uint64_t now;
	uint64_t frequency;
	unsigned long enabled;
	bool counted;

	/* Ignores an interrupt that is not the vertical blank. */
	display = owner;
	valve = display->base.timing[display->channel].mapped;
	status = kern_mmio_read32(valve + VALVE_INTSTAT);
	if ((status & VALVE_VFP_START) == 0)
		return false;

	/* Acknowledges, counts and times it. */
	kern_mmio_write32(valve + VALVE_INTSTAT, VALVE_VFP_START);
	counted = kern_rtc_read_counter(&now, &frequency);
	enabled = spin_lock_irqsave(&display->guard);

	display->vblank_count++;
	if (counted && display->vblank_timed < BCM2712_VBLANK_TIMED) {
		/* Times it on the monotonic counter. */
		display->vblank_counters[display->vblank_timed] = now;
		display->vblank_timed++;
		display->vblank_counter_hz = frequency;
	}

	spin_unlock_irqrestore(&display->guard, enabled);

	/* Succeeded: the vertical blank was quieted. */
	return true;
}

/*
 * Reports the median of the frames between consecutive timed blanks, in
 * counts of the monotonic counter: the mean of the two middle ones, since
 * there is an even number of them.
 */
static uint64_t
median_frame(
	const uint64_t *counters)
{
	uint64_t frames[BCM2712_VBLANK_TIMED - 1U];
	uint64_t frame;
	unsigned index;
	unsigned place;

	/* Inserts each frame in order among those before it. */
	for (index = 0; index + 1U < BCM2712_VBLANK_TIMED; index++) {
		frame = counters[index + 1U] - counters[index];
		place = index;
		while (place > 0 && frames[place - 1U] > frame) {
			/* Moves a longer frame up one place. */
			frames[place] = frames[place - 1U];
			place--;
		}
		frames[place] = frame;
	}

	/* Reports the middle. */
	return (frames[BCM2712_VBLANK_TIMED / 2U - 1U] + frames[BCM2712_VBLANK_TIMED / 2U]) / 2U;
}

/*
 * Quiets the shown channel's vertical blank and hands the list the hardware
 * now uses to the flip state.
 */
static bool
device_vblank_service(
	void *owner)
{
	struct bcm2712_display *display;
	volatile uint8_t *valve;
	uint32_t status;
	uint32_t current;
	unsigned long enabled;

	/* Ignores an interrupt that is not the vertical blank. */
	display = owner;
	valve = display->base.timing[display->channel].mapped;
	enabled = spin_lock_irqsave(&display->base.flip.guard);

	status = kern_mmio_read32(valve + BCM2712_VALVE_INTSTAT);
	if ((status & BCM2712_VALVE_VFP_START) == 0) {
		spin_unlock_irqrestore(&display->base.flip.guard, enabled);
		return false;
	}

	/* Acknowledges it and tells the flip state which list the hardware shows. */
	kern_mmio_write32(valve + BCM2712_VALVE_INTSTAT, BCM2712_VALVE_VFP_START);
	current = kern_mmio_read32(display->base.compositor.mapped + bcm2712_display_register_offset(display, BCM2712_HVS_DL)) & BCM2712_HVS_HEAD_MASK;
	bcm2711_display_flip_vblank_locked(&display->base, current);

	spin_unlock_irqrestore(&display->base.flip.guard, enabled);

	/* Succeeded: the vertical blank was quieted. */
	return true;
}
