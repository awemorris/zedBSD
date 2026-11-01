/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The BCM2712's HVS6 display: discovery (P0), the readout of what the
 * firmware shows (P1), the takeover of the firmware's display list (R0),
 * the pixel valve's vertical blank (R1) and a moving overlay plane flipped
 * through the driver's own lists (R2).  R2 is a test picture; the
 * attachment runs it only for the self-test (rpi4gpu.selftest=1).
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
 * list memory at 0x4000); Raspberry Pi rpi-6.12.y
 * drivers/iommu/bcm2712-iommu.c lines 24-80 (the IOMMU registers, only
 * read).
 */

#include <stdbool.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/display/hvs6.h"
#include "drivers/gpu/bcm2711/v3d71-private.h"

/* The device tree names of the parts (binding facts). */
#define HVS_COMPATIBLE			"brcm,bcm2712-hvs"
#define VALVE0_COMPATIBLE		"brcm,bcm2712-pixelvalve0"
#define VALVE1_COMPATIBLE		"brcm,bcm2712-pixelvalve1"

/* The compositor's common registers. */
#define HVS_VERSION			0x000U
#define HVS_LIST_SIZE			0x004U
#define HVS_LINE_BUFFER_SIZE		0x008U
#define HVS_PIXEL_MEMORY_SIZE		0x00cU
#define HVS_CONTROL			0x020U
#define HVS_VERSION_C0			0x53U
#define HVS_VERSION_D0			0x54U

/* The channel blocks' places: C0 from 0x030 every 0x20, D0 from 0x100 every 0x40. */
#define HVS_C0_CHANNEL_BASE		0x030U
#define HVS_C0_CHANNEL_STRIDE		0x020U
#define HVS_D0_CHANNEL_BASE		0x100U
#define HVS_D0_CHANNEL_STRIDE		0x040U

/* The channel registers' offsets inside a block, which differ by layout. */
#define CHANNEL_CTRL0			0x00U
#define CHANNEL_CTRL1			0x04U
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
#define PLANE_ALPHA_FIXED		(3U << 18)
#define PLANE_UNITY			0x8000U
#define PLANE_ORDER_SHIFT		13U
#define PLANE_ORDER_ARGB		2U
#define PLANE_FORMAT_RGBA8888		7U
#define PLANE_OPAQUE_ALPHA		(0xfffU << 4)
#define PLANE_C0_ALPHA_FIXED		(1U << 30)
#define PLANE_CONTEXT			0xc0c0c0c0U
#define PLANE_WORDS			8U

/* The unified pixel memory: 32 handles, 256-byte words, two-line buffers. */
#define UPM_HANDLES			32U
#define UPM_WORD_BYTES			256U
#define UPM_DEFAULT_WORDS		1024U

/* The pixel valve's interrupt registers and the start of its vertical front porch. */
#define VALVE_INTEN			0x24U
#define VALVE_INTSTAT			0x28U
#define VALVE_VFP_START			0x80U

/* The IOMMU's registers the readout shows. */
#define IOMMU_CTRL			0x00U
#define IOMMU_ADDR_CAP			0x14U
#define IOMMU_BYPASS_START		0x1cU
#define IOMMU_BYPASS_END		0x20U
#define IOMMU_DEBUG_INFO		0x38U

/* How long the hardware may take to adopt a list: 200 ms, in 1 ms polls. */
#define ADOPT_POLL_US			1000U
#define ADOPT_POLLS			200U

/*
 * How long R1 waits for its BCM2712_VBLANK_TIMED timed vertical blanks
 * (six frames, 100 ms at 60 Hz) before it counts over the whole wait.
 */
#define VBLANK_LIMIT_MS			1000U

/* The overlay of the R2 test: a 64x64 opaque square moved 30 times. */
#define OVERLAY_SIZE			64U
#define OVERLAY_STEPS			30U

static void discover_valve(const struct drv_fdt *fdt, struct bcm2712_display *display, unsigned index, const char *compatible);
static uint32_t channel_register(const struct bcm2712_display *display, uint32_t channel, uint32_t c0_offset, uint32_t d0_offset);
static uint32_t list_read(const struct bcm2712_display *display, uint32_t word);
static void list_write(struct bcm2712_display *display, uint32_t word, uint32_t value);
static int decode_list(struct bcm2712_display *display, uint32_t head);
static void show_list(const struct bcm2712_display *display, uint32_t head, uint32_t end);
static int switch_list(struct bcm2712_display *display, uint32_t head, uint32_t *waited_us);
static bool valve_service(void *owner);
static uint64_t median_frame(const uint64_t *counters);
static bool device_vblank_service(void *owner);
static uint32_t overlay_pixel(uint32_t x, uint32_t y, uint32_t step);

/*
 * Runs stage P0: finds the compositor, the pixel valves and the IOMMU and
 * maps them.
 *
 * Reads nothing from the hardware.  Returns ENODEV without a compositor.
 */
int
bcm2712_display_discover(
	const struct drv_fdt *fdt,
	struct bcm2712_display *display,
	const struct kern_boot_framebuffer *screen)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t node;
	uint32_t iommu_node;
	uint32_t phandle;
	bool allowed;
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

	/* Honors hvs6.stop=P0. */
	allowed = bcm2711_stage_allowed(BCM2712_FAMILY_DISPLAY, "P0");
	if (!allowed) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P0 stopped by boot parameter");
		return ECANCELED;
	}

	/* Finds and maps the compositor. */
	error = bcm2711_fdt_find(fdt, HVS_COMPATIBLE, &node);
	if (error != 0) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P0 no hvs node: no display");
		return ENODEV;
	}
	error = bcm2711_fdt_window(fdt, node, 0, &display->base.compositor);
	if (error != 0 || display->base.compositor.size < LIST_MEMORY_OFFSET + 4U * LIST_BOOT_WORDS) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P0 hvs reg unusable (%d)", error);
		return ENODEV;
	}
	error = bcm2711_map_window(&display->base.compositor);
	if (error != 0) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P0 hvs map failed (%d)", error);
		return ENODEV;
	}

	/* Maps the IOMMU the compositor names, only to read it. */
	error = drv_fdt_property(fdt, node, "iommus", &value, &length);
	if (error == 0 && length >= 4U) {
		/* Finds the IOMMU's node and maps its registers. */
		phandle = (uint32_t)drv_fdt_cells_value(value, 0, 1);
		error = drv_fdt_find_phandle(fdt, phandle, &iommu_node);
		if (error == 0)
			error = bcm2711_fdt_window(fdt, iommu_node, 0, &display->iommu);
		if (error == 0 && display->iommu.size >= 0x3cU)
			error = bcm2711_map_window(&display->iommu);
		if (error != 0)
			display->iommu.mapped = NULL;
	}

	/* Finds the two pixel valves. */
	discover_valve(fdt, display, 0, VALVE0_COMPATIBLE);
	discover_valve(fdt, display, 1, VALVE1_COMPATIBLE);

	/* Shows what was found. */
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P0 hvs %llx+%llx iommu %llx pv0 %llx irq %d pv1 %llx irq %d",
			   (unsigned long long)display->base.compositor.physical,
			   (unsigned long long)display->base.compositor.size,
			   (unsigned long long)display->iommu.physical,
			   (unsigned long long)display->base.timing[0].physical,
			   display->base.timing_irq[0].irq,
			   (unsigned long long)display->base.timing[1].physical,
			   display->base.timing_irq[1].irq);
	display->present = true;

	/* Succeeded: the compositor is mapped. */
	return 0;
}

/*
 * Runs stage P1: reads the compositor, the IOMMU and the firmware's list.
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
	uint32_t index;
	bool found;
	bool allowed;
	int error;

	/* Honors hvs6.stop=P1. */
	if (!display->present)
		return ENODEV;
	allowed = bcm2711_stage_allowed(BCM2712_FAMILY_DISPLAY, "P1");
	if (!allowed)
		return ECANCELED;
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
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 unknown HVS version %02x", display->version);
		return ENOTSUP;
	}

	/* Reads the memory sizes; the list memory is addressed by a 12-bit head. */
	display->list_words = kern_mmio_read32(hvs + HVS_LIST_SIZE);
	display->ubm_words = kern_mmio_read32(hvs + HVS_PIXEL_MEMORY_SIZE);
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 version %02x (%s) control %08x list %u lbm %u ubm %u",
			   display->version,
			   display->version == HVS_VERSION_D0 ? "D0" : "C0",
			   kern_mmio_read32(hvs + HVS_CONTROL),
			   display->list_words,
			   kern_mmio_read32(hvs + HVS_LINE_BUFFER_SIZE),
			   display->ubm_words);

	/* Shows every channel and picks the first enabled, running one. */
	found = false;
	for (channel = 0; channel < BCM2712_HVS_CHANNELS; channel++) {
		/* Reads the channel's control and status. */
		control0 = kern_mmio_read32(hvs + channel_register(display, channel, CHANNEL_CTRL0, CHANNEL_CTRL0));
		status = kern_mmio_read32(hvs + channel_register(display, channel, CHANNEL_C0_STATUS, CHANNEL_D0_STATUS));
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 ch%u ctrl0 %08x ctrl1 %08x lptrs %08x status %08x dl %08x",
				   channel,
				   control0,
				   kern_mmio_read32(hvs + channel_register(display, channel, CHANNEL_CTRL1, CHANNEL_CTRL1)),
				   kern_mmio_read32(hvs + channel_register(display, channel, CHANNEL_C0_LPTRS, CHANNEL_D0_LPTRS)),
				   status,
				   kern_mmio_read32(hvs + channel_register(display, channel, CHANNEL_C0_DL, CHANNEL_D0_DL)));

		/* Takes the first channel that is enabled and running. */
		mode = (status >> CHANNEL_MODE_SHIFT) & CHANNEL_MODE_MASK;
		if (!found && (control0 & CHANNEL_ENABLED) != 0 && mode == CHANNEL_MODE_RUN) {
			display->channel = channel;
			found = true;
		}
	}

	/* Shows the IOMMU as the firmware left it. */
	if (display->iommu.mapped != NULL) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 iommu ctrl %08x cap %08x bypass %08x-%08x debug %08x",
				   kern_mmio_read32(display->iommu.mapped + IOMMU_CTRL),
				   kern_mmio_read32(display->iommu.mapped + IOMMU_ADDR_CAP),
				   kern_mmio_read32(display->iommu.mapped + IOMMU_BYPASS_START),
				   kern_mmio_read32(display->iommu.mapped + IOMMU_BYPASS_END),
				   kern_mmio_read32(display->iommu.mapped + IOMMU_DEBUG_INFO));
	}

	/* Refuses a display the firmware does not show on any channel. */
	if (!found) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 no running channel");
		return ENODEV;
	}

	/* Decodes the channel's list. */
	display->firmware_head = kern_mmio_read32(hvs + channel_register(display, display->channel, CHANNEL_C0_LPTRS, CHANNEL_D0_LPTRS)) & CHANNEL_HEAD_MASK;
	error = decode_list(display, display->firmware_head);
	show_list(display, display->firmware_head, display->firmware_end);
	if (error != 0) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 ch%u list at %u unreadable (%d)", display->channel, display->firmware_head, error);
		return error;
	}

	/* Shows each plane and whether it is the boot framebuffer. */
	for (index = 0; index < display->plane_count && index < BCM2712_HVS_LIST_PLANES; index++) {
		/* Shows one plane. */
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 plane%u fmt %u ord %u mode %u %s %u,%u %ux%u",
				   index,
				   display->planes[index].format,
				   display->planes[index].order,
				   display->planes[index].address_mode,
				   display->planes[index].scaled ? "scaled" : "unity",
				   display->planes[index].x,
				   display->planes[index].y,
				   display->planes[index].width,
				   display->planes[index].height);
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 plane%u addr %llx pitch %u upm %u@%u%s",
				   index,
				   (unsigned long long)display->planes[index].address,
				   display->planes[index].pitch,
				   display->planes[index].upm_handle,
				   display->planes[index].upm_base,
				   (display->screen_known && display->planes[index].address == display->screen.physical_base) ? " = boot framebuffer" : "");
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
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 ok ch%u list %u-%u, %u planes %u unused, fb %llx %ux%u pitch %u",
			   display->channel,
			   display->firmware_head,
			   display->firmware_end,
			   display->plane_count,
			   display->skipped_elements,
			   (unsigned long long)display->screen.physical_base,
			   display->screen.width,
			   display->screen.height,
			   display->screen.pitch);

	/* Succeeded: the firmware's channel and list are known. */
	return 0;
}

/*
 * Runs stage R0: shows the firmware's list from the driver's own memory.
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
	bool allowed;
	int error;

	/* Honors hvs6.stop=R0; needs a decoded firmware list. */
	if (!display->present || display->plane_count == 0)
		return ENODEV;
	allowed = bcm2711_stage_allowed(BCM2712_FAMILY_DISPLAY, "R0");
	if (!allowed)
		return ECANCELED;

	/* Places the regions at the top of the memory a list head can name. */
	limit = display->list_words;
	if (limit > LIST_ADDRESSABLE_WORDS)
		limit = LIST_ADDRESSABLE_WORDS;
	if (limit < LIST_BOOT_WORDS + OWN_REGIONS * OWN_REGION_WORDS) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R0 list memory too small (%u)", display->list_words);
		return ENOSPC;
	}
	display->own_head = limit - OWN_REGIONS * OWN_REGION_WORDS;

	/* Refuses regions that overlap the firmware's list, or planes that do not fit one region. */
	words = 1U;
	for (plane = 0; plane < display->plane_count; plane++)
		words += display->planes[plane].words;
	if (display->firmware_end >= display->own_head || words + PLANE_WORDS > OWN_REGION_WORDS) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R0 firmware list %u-%u (%u words) does not fit below %u", display->firmware_head, display->firmware_end, words, display->own_head);
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
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R0 switch ch%u to own list at %u (%u words)", display->channel, display->own_head, words);
	error = switch_list(display, display->own_head, &waited_us);
	if (error != 0) {
		(void)switch_list(display, display->firmware_head, &waited_us);
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R0 own list not taken; firmware list restored");
		return error;
	}
	display->adopted = true;
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R0 ok own list taken after %u us", waited_us);

	/* Succeeded: the channel shows the driver's list. */
	return 0;
}

/*
 * Runs stage R1: times the pixel valve's vertical blanks.
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
	uint32_t status;
	uint64_t counters[BCM2712_VBLANK_TIMED];
	uint64_t count;
	uint64_t timed;
	uint64_t frequency;
	uint64_t started;
	uint64_t elapsed;
	uint64_t frame;
	uint64_t frame_us;
	uint64_t millihz;
	unsigned step;
	unsigned index;
	bool allowed;

	/* Honors hvs6.stop=R1; channel 2 has no valve. */
	if (!display->present || display->channel >= BCM2712_HVS_VALVES)
		return ENODEV;
	allowed = bcm2711_stage_allowed(BCM2712_FAMILY_DISPLAY, "R1");
	if (!allowed)
		return ECANCELED;
	valve = display->base.timing[display->channel].mapped;
	line = &display->base.timing_irq[display->channel];
	if (valve == NULL || !line->registered)
		return ENODEV;

	/* Shows the valve's interrupt state as the firmware left it. */
	enable = kern_mmio_read32(valve + VALVE_INTEN);
	status = kern_mmio_read32(valve + VALVE_INTSTAT);
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R1 pv%u inten %08x intstat %08x", display->channel, enable, status);

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
	frame_us = 0;
	if (frame != 0)
		frame_us = frame * 1000000U / frequency;
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R1 %llu vblanks in %llu ms, median frame %llu us",
			   (unsigned long long)count,
			   (unsigned long long)elapsed,
			   (unsigned long long)frame_us);

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
 * Runs stage R2: moves an opaque 64x64 square over the console by flipping
 * between two of the driver's lists, then shows the console alone again.
 *
 * The square has its own unified-pixel-memory handle and area, away from
 * the firmware's.  Each step waits for the hardware to take its list, so the
 * one it replaced is free for the step after.
 */
int
bcm2712_display_overlay_test(
	struct bcm2712_display *display)
{
	struct bcm2711_buffer *image;
	uint32_t *pixels;
	uint32_t handle;
	uint32_t upm_base;
	uint32_t upm_words;
	uint32_t ubm_words;
	uint32_t pitch;
	uint32_t head;
	uint32_t word;
	uint32_t x;
	uint32_t y;
	uint32_t step;
	uint32_t taken;
	uint32_t waited_us;
	uint32_t slowest_us;
	uint32_t index;
	uint32_t plane;
	uint32_t alpha_control;
	uint32_t alpha_mode;
	bool used;
	bool allowed;
	int error;

	/* Honors hvs6.stop=R2; needs the driver's lists. */
	if (!display->adopted)
		return ENODEV;
	allowed = bcm2711_stage_allowed(BCM2712_FAMILY_DISPLAY, "R2");
	if (!allowed)
		return ECANCELED;

	/* Picks the highest handle no firmware plane uses. */
	handle = 0;
	for (index = UPM_HANDLES; index >= 1U && handle == 0; index--) {
		/* Checks the firmware's planes for this handle. */
		used = false;
		for (plane = 0; plane < display->plane_count && plane < BCM2712_HVS_LIST_PLANES; plane++) {
			/* A plane with this handle makes it taken. */
			if (display->planes[plane].upm_handle == index)
				used = true;
		}
		if (!used)
			handle = index;
	}

	/* Places the square's pixel memory at the top, past every firmware plane's base. */
	pitch = OVERLAY_SIZE * 4U;
	upm_words = ((pitch + 62U) / 32U * 32U * 2U + UPM_WORD_BYTES - 1U) / UPM_WORD_BYTES;
	ubm_words = display->ubm_words;
	if (ubm_words == 0 || ubm_words > 0x1fffU)
		ubm_words = UPM_DEFAULT_WORDS;
	upm_base = ubm_words - upm_words;
	for (plane = 0; plane < display->plane_count && plane < BCM2712_HVS_LIST_PLANES; plane++) {
		/* Refuses an area a firmware plane may already use. */
		if (display->planes[plane].upm_base >= upm_base) {
			bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R2 no free pixel memory above %u", display->planes[plane].upm_base);
			return ENOSPC;
		}
	}
	if (handle == 0)
		return ENOSPC;

	/* An opaque plane's fixed alpha is a control-word field on D0 and a mode of the alpha word on C0. */
	alpha_control = PLANE_ALPHA_FIXED;
	alpha_mode = 0;
	if (display->version == HVS_VERSION_C0) {
		alpha_control = 0;
		alpha_mode = PLANE_C0_ALPHA_FIXED;
	}

	/* Allocates the square below 4 GiB and fills it. */
	error = bcm2711_buffer_create(OVERLAY_SIZE * pitch, 0xffffffffULL, 4096, &image);
	if (error != 0)
		return error;
	pixels = image->address;
	for (y = 0; y < OVERLAY_SIZE; y++) {
		/* Fills one row. */
		for (x = 0; x < OVERLAY_SIZE; x++)
			pixels[y * OVERLAY_SIZE + x] = overlay_pixel(x, y, 0);
	}
	kern_dcache_clean_range(pixels, OVERLAY_SIZE * pitch);
	kern_io_write_barrier();
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R2 overlay %llx upm %u@%u (%u words), %u steps",
			   (unsigned long long)image->memory.paddr,
			   handle,
			   upm_base,
			   upm_words,
			   OVERLAY_STEPS);

	/* Moves the square: each step writes the region the hardware is not using and flips to it. */
	taken = 0;
	slowest_us = 0;
	error = 0;
	for (step = 0; step < OVERLAY_STEPS && error == 0; step++) {
		/* Alternates between the second and the third region. */
		head = display->own_head + OWN_REGION_WORDS * (1U + step % 2U);

		/* Copies the console's planes without their END word. */
		for (index = 0; index + 1U < display->own_words; index++)
			list_write(display, head + index, list_read(display, display->own_head + index));
		word = head + display->own_words - 1U;

		/* Appends the square: unity RGBA8888 in ARGB order, fixed alpha, at its step's place. */
		x = 64U + step * 24U;
		y = 64U + step * 8U;
		list_write(display, word + 0U, PLANE_VALID | (PLANE_WORDS << PLANE_NEXT_SHIFT) | alpha_control | PLANE_UNITY | (PLANE_ORDER_ARGB << PLANE_ORDER_SHIFT) | PLANE_FORMAT_RGBA8888);
		list_write(display, word + 1U, (y << 16) | x);
		list_write(display, word + 2U, PLANE_OPAQUE_ALPHA | alpha_mode);
		list_write(display, word + 3U, ((OVERLAY_SIZE - 1U) << 16) | (OVERLAY_SIZE - 1U));
		list_write(display, word + 4U, PLANE_CONTEXT);
		list_write(display, word + 5U, (upm_base << 16) | ((handle - 1U) << 10) | (uint32_t)((image->memory.paddr >> 32) & 0xffU));
		list_write(display, word + 6U, (uint32_t)image->memory.paddr);
		list_write(display, word + 7U, pitch);
		list_write(display, word + 8U, PLANE_END);
		kern_io_write_barrier();

		/* Flips to the region and waits until the hardware took it. */
		error = switch_list(display, head, &waited_us);
		if (error == 0) {
			taken++;
			if (waited_us > slowest_us)
				slowest_us = waited_us;
		}
	}

	/* Shows the console alone again, then the square's memory may go. */
	(void)switch_list(display, display->own_head, &waited_us);
	bcm2711_buffer_release(image);
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R2 overlay moved %u/%u steps, slowest flip %u us; console list back", taken, OVERLAY_STEPS, slowest_us);

	/* Reports a step the hardware did not take. */
	if (error != 0)
		return error;

	/* Succeeded: every flip was taken at a frame start. */
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
 * shared display device check its flips and register its node.  Needs R0
 * and R1.
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
	bool allowed;
	int error;

	/* Honors hvs6.stop=R3; needs the driver's console list and a measured refresh. */
	if (!display->adopted || display->base.refresh_millihz == 0)
		return ENODEV;
	if (display->channel >= BCM2712_HVS_VALVES)
		return ENODEV;
	allowed = bcm2711_stage_allowed(BCM2712_FAMILY_DISPLAY, "R3");
	if (!allowed)
		return ECANCELED;
	valve = display->base.timing[display->channel].mapped;
	line = &display->base.timing_irq[display->channel];
	if (valve == NULL || !line->registered)
		return ENODEV;

	/* Refuses firmware planes whose pixel memory the driver's planes would share. */
	for (plane = 0; plane < display->plane_count && plane < BCM2712_HVS_LIST_PLANES; plane++) {
		/* A handle or a base inside the driver's ranges collides. */
		if (display->planes[plane].upm_handle >= BCM2712_UPM_LOWEST_HANDLE ||
		    display->planes[plane].upm_base + 129U > BCM2712_UPM_LOWEST_BASE) {
			bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R3 firmware plane%u upm %u@%u collides", plane, display->planes[plane].upm_handle, display->planes[plane].upm_base);
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
	bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R3 device on console list %u, %u mHz, core %u Hz",
			   display->own_head,
			   display->base.refresh_millihz,
			   rate);

	/* Has the shared display device exercise the flips and publish its node. */
	error = bcm2711_display_register(&display->base);
	if (error != 0) {
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "R3 display device not registered (%d)", error);
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

/* Reports the window offset of one channel register in the layout read at P1. */
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
	display->skipped_elements = 0;
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
			display->skipped_elements++;
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

/* Shows the words of a list, four to a line, through its END word and at most 48 words. */
static void
show_list(
	const struct bcm2712_display *display,
	uint32_t head,
	uint32_t end)
{
	uint32_t word;
	uint32_t words[4];
	uint32_t index;

	/* Shows each group of four words. */
	for (word = head; word <= end + 3U && word < head + 48U; word += 4U) {
		/* Reads the group. */
		for (index = 0; index < 4U; index++)
			words[index] = list_read(display, word + index);
		bcm2711_stage_mark(BCM2712_FAMILY_DISPLAY, "P1 @%u %08x %08x %08x %08x",
				   word, words[0], words[1], words[2], words[3]);
	}
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

/* Packs the square's opaque pattern: a magenta and green checker of 8-pixel cells. */
static uint32_t
overlay_pixel(
	uint32_t x,
	uint32_t y,
	uint32_t step)
{
	/* Alternates the colour by cell. */
	if ((((x >> 3) + (y >> 3) + step) & 1U) == 0)
		return 0xffff00ffU;

	/* The other cells are green. */
	return 0xff00ff00U;
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
