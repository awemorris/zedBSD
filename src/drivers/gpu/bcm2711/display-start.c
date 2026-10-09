/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Captures the boot output before relinquishing firmware ownership, then
 * executes the independently prepared initial display hardware program.
 * It deliberately refuses unsupported modes before the first notification.
 * Device-source IRQ service confirms the first adopted frame at boot.
 */

#include <stdbool.h>
#include <stdint.h>

#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/display-program.h"

/* Device-tree identities of the encoder blocks and their shared clock gate. */
static const char *const start_encoders[BCM2711_HDMI_COUNT] = {
    "brcm,bcm2711-hdmi0", "brcm,bcm2711-hdmi1"};

/* Role order is private; each role is located by its binding's reg-name. */
static const char *const start_regions[BCM2711_ENCODER_REGIONS] = {
    "hdmi", "dvp", "phy", "rm", "packet", "csc", "hd", "cec"};

/* Required span includes the highest register touched in each role. */
static const uint32_t start_spans[BCM2711_ENCODER_REGIONS] = {
    0x1c8, 0xf4, 0x60, 0x20, 0x6c, 0x30, 0x4c, 0x04};

/* Boot-owned mappings remain valid for the lifetime of the driver. */
static struct bcm2711_window start_windows[BCM2711_REGION_COUNT];

/* One exclusive boot program; it is never regenerated while execution runs. */
static struct bcm2711_display_program start_program;

/* A successful or partly executed restart cannot be safely repeated at attach. */
static bool start_attempted;

static int map_regions(const struct drv_fdt *fdt, const struct bcm2711_display *display);
static int named_window(const struct drv_fdt *fdt, uint32_t node, const char *name, struct bcm2711_window *window);
static int capture_mode(const struct bcm2711_display *display, struct bcm2711_display_mode *mode, uint32_t *order, uint32_t *max_core_hz);
static uint32_t encoder_read(uint32_t base, uint32_t role, uint32_t offset);
static int capture_avi(uint32_t base, struct bcm2711_display_mode *mode);

/*
 * Starts the initial scanout on the unique firmware framebuffer output.
 *
 * All windows, inputs and commands are prepared before firmware ownership
 * ends. Returns zero only after a fresh frame and current-list adoption.
 * A failure after notification leaves the pipeline incomplete; the caller
 * must not register a usable display or retry this destructive boot attempt.
 */
int
bcm2711_display_start(
	const struct drv_fdt *fdt,
	struct bcm2711_display *display)
{
	struct bcm2711_display_mode mode;
	uint32_t order;
	uint32_t max_core_hz;
	uint64_t pixels_per_frame;
	bool allowed;
	int error;

	/* Requires the unique framebuffer output proved by the read-only stage. */
	if (start_attempted)
		return EBUSY;
	display->scanout_started = false;
	if (!display->readout_done || !display->screen_matches)
		return ENODEV;

	/* Keeps both the new restart stop and the former takeover stop effective. */
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "R0");
	if (!allowed)
		return ECANCELED;
	allowed = bcm2711_stage_allowed(BCM2711_FAMILY_DISPLAY, "N1");
	if (!allowed)
		return ECANCELED;

	/* Maps all required resources without touching registers. */
	error = map_regions(fdt, display);
	if (error != 0)
		return error;

	/* Installs source service while the previously registered lines are masked. */
	error = bcm2711_display_irq_prepare(display);
	if (error != 0)
		return error;

	/* Captures the mode and sink metadata before any firmware notification. */
	error = capture_mode(display, &mode, &order, &max_core_hz);
	if (error != 0)
		return error;

	/* Ensures no invalid field can fail preparation after the screen is stopped. */
	error = bcm2711_display_program_prepare(&start_program, &mode, &display->screen, display->port, order, max_core_hz);
	if (error != 0)
		return error;

	/* Publishes the selected port and gives time to record the last visible mark. */
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "R0 begin hdmi%u %ux%u tmds %u",
			   display->port,
			   mode.width,
			   mode.height,
			   mode.tmds_hz);
	bcm2711_stage_pause(BCM2711_FAMILY_DISPLAY, "R0");

	/* Runs the prepared hardware operations once, stopping on the first failure. */
	start_attempted = true;
	display->channel = 0;
	error = bcm2711_display_program_execute(&start_program, start_windows, &display->screen, display);
	if (error != 0) {
		bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "R0 failed op %u error %d", start_program.failure, error);
		return error;
	}

	/* Succeeded: scanout adoption was observed, not inferred from enable bits. */
	display->channel = 0;
	display->scanout_started = true;
	display->max_core_hz = max_core_hz;
	display->console_core_hz = start_program.commands[start_program.count - 1U].value;
	pixels_per_frame = (uint64_t)(mode.width + mode.hfront + mode.hsync + mode.hback) * (mode.height + mode.vfront + mode.vsync + mode.vback);
	display->refresh_millihz = (uint32_t)(((uint64_t)mode.tmds_hz * 1000U + pixels_per_frame / 2U) / pixels_per_frame);
	bcm2711_stage_mark(BCM2711_FAMILY_DISPLAY, "R0 ok hdmi%u ch0 list43 frame observed", display->port);
	return 0;
}

/* Resolves every register role and validates its span before register reads. */
static int
map_regions(
	const struct drv_fdt *fdt,
	const struct bcm2711_display *display)
{
	struct bcm2711_window *window;
	uint32_t port;
	uint32_t role;
	uint32_t base;
	uint32_t node;
	int error;

	/* Reuses the already mapped HVS and PV windows. */
	start_windows[BCM2711_REGION_HVS] = display->compositor;
	if (display->compositor.mapped == NULL || display->compositor.size < 0x8000U)
		return ENODEV;
	for (port = 0; port < BCM2711_TIMING_COUNT; port++) {
		/* Both PV blocks are needed for the initial interrupt-source preparation. */
		start_windows[BCM2711_REGION_PV0 + port] = display->timing[port];
		if (display->timing[port].mapped == NULL || display->timing[port].size < 0x38U)
			return ENODEV;
	}

	/* Maps encoder windows by names, allowing different firmware reg ordering. */
	for (port = 0; port < BCM2711_HDMI_COUNT; port++) {
		/* Finds the exact encoder described by the board's firmware tree. */
		error = bcm2711_fdt_find(fdt, start_encoders[port], &node);
		if (error != 0)
			return error;
		base = BCM2711_REGION_HDMI0 + port * BCM2711_ENCODER_REGIONS;
		for (role = 0; role < BCM2711_ENCODER_REGIONS; role++) {
			/* Resolves and bounds one register role before mapping it. */
			window = &start_windows[base + role];
			error = named_window(fdt, node, start_regions[role], window);
			if (error != 0)
				return error;
			if (window->size < start_spans[role])
				return EINVAL;
			error = bcm2711_map_window(window);
			if (error != 0)
				return error;
		}
	}

	/* Maps the shared audio-clock gate used even without HDMI audio. */
	error = bcm2711_fdt_find(fdt, "brcm,brcm2711-dvp", &node);
	if (error != 0)
		return error;
	window = &start_windows[BCM2711_REGION_GLUE];
	error = bcm2711_fdt_window(fdt, node, 0, window);
	if (error != 0)
		return error;
	if (window->size < 0x0cU)
		return EINVAL;
	error = bcm2711_map_window(window);
	if (error != 0)
		return error;

	/* Succeeded: every future register access has a bounded mapped window. */
	return 0;
}

/* Finds a reg index from the bounded, NUL-terminated reg-names string list. */
static int
named_window(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	struct bcm2711_window *window)
{
	const uint8_t *names;
	uint32_t length;
	uint32_t first;
	uint32_t end;
	uint32_t index;
	int comparison;
	int error;

	/* Requires the binding's role names instead of assuming an address order. */
	error = drv_fdt_property(fdt, node, "reg-names", &names, &length);
	if (error != 0)
		return error;
	first = 0;
	index = 0;
	while (first < length) {
		/* Proves termination inside the property before comparing the string. */
		end = first;
		while (end < length && names[end] != 0)
			end++;
		if (end == length)
			return EINVAL;
		comparison = kern_strcmp((const char *)names + first, name);
		if (comparison == 0) {
			/* Resolves the matching reg entry through the existing translation helper. */
			error = bcm2711_fdt_window(fdt, node, index, window);
			if (error != 0)
				return error;

			/* Succeeded: the window corresponds to the requested hardware role. */
			return 0;
		}

		/* Advances only past a string proved to be completely inside the property. */
		first = end + 1U;
		index++;
	}

	/* A missing role cannot be replaced by an assumed register base. */
	return ENOENT;
}

/* Captures and cross-checks a progressive RGB8 mode on the selected output. */
static int
capture_mode(
	const struct bcm2711_display *display,
	struct bcm2711_display_mode *mode,
	uint32_t *order,
	uint32_t *max_core_hz)
{
	const struct bcm2711_list_plane *plane;
	const struct bcm2711_window *timing;
	uint64_t frequency;
	uint32_t base;
	uint32_t horizontal;
	uint32_t vertical;
	uint32_t value;
	uint32_t divider;
	uint32_t offset;
	uint32_t index;
	uint32_t count;
	uint32_t values[2];
	uint32_t hz;
	int error;

	/* Checks the shared HSM clock immediately before any encoder register read. */
	error = bcm2711_clock_hz(13, &hz);
	if (error != 0)
		return error;
	if (hz == 0)
		return ENODEV;
	base = BCM2711_REGION_HDMI0 + display->port * BCM2711_ENCODER_REGIONS;
	timing = &display->timing[display->port];

	/* Refuses disabled video, interlace, repetition, deep colour and scrambling. */
	value = encoder_read(base, BCM2711_ENCODER_SHARED, 0x44U + display->port * 4U);
	if ((value & 0x80000000U) == 0)
		return ENODEV;
	value = kern_mmio_read32(timing->mapped + 0x04);
	if ((value & 0x11U) != 1U)
		return ENOTSUP;
	value = encoder_read(base, BCM2711_ENCODER_CORE, 0x100);
	if ((value & 0x0fU) != 0)
		return ENOTSUP;
	value = encoder_read(base, BCM2711_ENCODER_CORE, 0x170);
	if ((value & 0x0fU) != 0)
		return ENOTSUP;
	value = encoder_read(base, BCM2711_ENCODER_CORE, 0x1c4);
	if ((value & 1U) != 0)
		return ENOTSUP;

	/* Extracts full-pixel HDMI timings rather than mistaking PV clocks for pixels. */
	horizontal = encoder_read(base, BCM2711_ENCODER_CORE, 0xe4);
	mode->width = horizontal & 0x3fffU;
	mode->hfront = (horizontal >> 16) & 0x1fffU;
	mode->hpositive = false;
	if ((horizontal & 0x4000U) != 0)
		mode->hpositive = true;
	mode->vpositive = false;
	if ((horizontal & 0x8000U) != 0)
		mode->vpositive = true;
	horizontal = encoder_read(base, BCM2711_ENCODER_CORE, 0xe8);
	mode->hsync = horizontal & 0x7ffU;
	mode->hback = (horizontal >> 16) & 0x7ffU;
	vertical = encoder_read(base, BCM2711_ENCODER_CORE, 0xec);
	mode->height = vertical & 0x1fffU;
	mode->vfront = (vertical >> 16) & 0x7fU;
	mode->vsync = (vertical >> 24) & 0x1fU;
	vertical = encoder_read(base, BCM2711_ENCODER_CORE, 0xf0);
	mode->vback = vertical & 0x1ffU;

	/* Cross-checks PV active size and horizontal/vertical porches against HDMI. */
	value = kern_mmio_read32(timing->mapped + 0x10);
	if ((value & 0xffffU) * 2U != mode->width || (value >> 16) * 2U != mode->hfront)
		return EINVAL;
	value = kern_mmio_read32(timing->mapped + 0x0c);
	if ((value & 0xffffU) * 2U != mode->hsync || (value >> 16) * 2U != mode->hback)
		return EINVAL;
	value = kern_mmio_read32(timing->mapped + 0x18);
	if ((value & 0xffffU) != mode->height || (value >> 16) != mode->vfront)
		return EINVAL;
	value = kern_mmio_read32(timing->mapped + 0x14);
	if ((value & 0xffffU) != mode->vsync || (value >> 16) != mode->vback)
		return EINVAL;

	/* Requires the oscillator representation used by the fixed-point recovery. */
	value = encoder_read(base, BCM2711_ENCODER_RATE, 0x1c);
	if ((value & 0x03000000U) != 0x02000000U)
		return ENOTSUP;
	value = encoder_read(base, BCM2711_ENCODER_PHY, 0x34);
	if ((value & 0x0fU) != 1U)
		return ENOTSUP;
	value = encoder_read(base, BCM2711_ENCODER_PHY, 0x20);
	if ((value & 0x2000U) == 0)
		return ENOTSUP;

	/* Recovers the programmed TMDS rate from the oscillator's fixed-point ratio. */
	divider = encoder_read(base, BCM2711_ENCODER_PHY, 0x28);
	divider = (divider >> 8) & 0xffU;
	offset = encoder_read(base, BCM2711_ENCODER_RATE, 0x18);
	offset &= 0x7fffffffU;
	if (divider == 0 || offset == 0)
		return ENOTSUP;
	frequency = (uint64_t)offset * 54000000U;
	frequency /= (uint64_t)divider * 20U * 1048576U;
	frequency = (frequency + 500U) / 1000U * 1000U;
	if (frequency > 0xffffffffULL)
		return EINVAL;
	mode->tmds_hz = (uint32_t)frequency;

	/* Requires a simple packed RGB32 plane, with no flips or tiling. */
	count = display->list.plane_count;
	if (count > BCM2711_LIST_PLANES)
		count = BCM2711_LIST_PLANES;
	*order = 0;
	for (index = 0; index < count; index++) {
		/* Finds the console plane even when another firmware plane precedes it. */
		plane = &display->list.planes[index];
		if ((uint64_t)(plane->pointer & 0x3fffffffU) != display->screen.physical)
			continue;
		if (plane->format != 7U || plane->scaled || plane->flipped || plane->words != 8U)
			return ENOTSUP;
		if ((plane->control & 0x00300000U) != 0)
			return ENOTSUP;
		*order = plane->order;
		break;
	}

	/* No retained plane supplied a usable RGB32 order. */
	if (*order == 0)
		return ENOTSUP;

	/* Captures sink metadata only after all timing and framebuffer checks. */
	value = encoder_read(base, BCM2711_ENCODER_CORE, 0xe0);
	mode->hdmi = false;
	if ((value & 1U) != 0)
		mode->hdmi = true;
	mode->limited = false;
	for (index = 0; index < 17U; index++) {
		/* DVI carries no AVI packet. */
		mode->avi[index] = 0;
	}

	/* HDMI sink metadata is required before any destructive hardware operation. */
	if (mode->hdmi) {
		/* Validates the firmware's AVI packet without changing sink configuration. */
		error = capture_avi(base, mode);
		if (error != 0)
			return error;
	}

	/* Requires the core maximum used by both setup and sustained-rate requests. */
	values[0] = 4;
	values[1] = 0;
	error = bcm2711_firmware_get(0x00030004U, values, 1, 2);
	if (error != 0)
		return error;
	if (values[0] != 4 || values[1] == 0)
		return EIO;
	*max_core_hz = values[1];

	/* Succeeded: the captured mode is ready for pure program validation. */
	return 0;
}

/* Reads one encoder register through its already bounded mapping. */
static uint32_t
encoder_read(
	uint32_t base,
	uint32_t role,
	uint32_t offset)
{
	uint32_t value;

	/* Ordered reads preserve hardware observation semantics. */
	value = kern_mmio_read32(start_windows[base + role].mapped + offset);

	/* Returns the observed register value. */
	return value;
}

/* Decodes the packed AVI slot and derives its RGB quantization policy. */
static int
capture_avi(
	uint32_t base,
	struct bcm2711_display_mode *mode)
{
	uint32_t index;
	uint32_t first;
	uint32_t second;
	uint32_t checksum;
	uint32_t quantization;
	uint32_t value;

	/* Requires AVI transmission to be enabled and the packet RAM to exist. */
	value = encoder_read(base, BCM2711_ENCODER_CORE, 0xbc);
	if ((value & 0x10004U) != 0x10004U)
		return ENOTSUP;
	checksum = 0;
	for (index = 0; index < 17U; index++) {
		/* Reads the two hardware words that encode each seven-byte group. */
		first = encoder_read(base, BCM2711_ENCODER_PACKET, 0x48U + (index / 7U) * 8U);
		second = encoder_read(base, BCM2711_ENCODER_PACKET, 0x4cU + (index / 7U) * 8U);
		if (index % 7U < 3U) {
			/* The first word stores three payload bytes. */
			mode->avi[index] = (uint8_t)(first >> ((index % 7U) * 8U));
		} else {
			/* The second word stores the remaining four payload bytes. */
			mode->avi[index] = (uint8_t)(second >> ((index % 7U - 3U) * 8U));
		}

		/* Counts every header and payload byte in the checksum. */
		checksum += mode->avi[index];
	}

	/* A torn or invalid packet cannot identify a reusable sink configuration. */
	if ((checksum & 0xffU) != 0)
		return EIO;

	/* Explicit AVI quantization takes precedence over the CEA default. */
	quantization = (mode->avi[6] >> 2) & 3U;
	if (quantization == 3U)
		return ENOTSUP;
	if (quantization == 1U)
		mode->limited = true;
	if (quantization == 0U && (mode->avi[7] & 0x7fU) > 1U)
		mode->limited = true;

	/* Succeeded: metadata can be republished after the same-mode restart. */
	return 0;
}
