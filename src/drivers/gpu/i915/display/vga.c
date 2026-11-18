/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display device as a client of the VGA arbiter, and the legacy VGA
 * plane the firmware may have left on.
 *
 * The client registers the display device (a PCI VGA-class device) with
 * the arbiter and switches the host bridge's VGA decode for it.  The
 * legacy VGA I/O (the sequencer's screen off and the Misc Output register)
 * is the client's; a host test installs an accessor in place of the
 * ports.  The VGA plane is switched off through the xd parts.
 */

#include "internal.h"
#include "vga.h"
#include <kern/kcrt.h>

#include "../trace.h"

#include <drivers/pci/pci.h>
#include <kern/device-io.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The trace stage of the VGA client's registration (the stage number the
 * display start's trace records it under).
 */
#define I915_VGA_TRACE_STAGE		4U

/*
 * The legacy VGA ports: the sequencer's index and data, the screen off
 * bit of SR01, and the Misc Output register's read and write ports.
 *
 * Values referenced: Linux v6.8.12 include/video/vga.h (VGA_SEQ_I,
 * VGA_SEQ_D, VGA_MIS_R, VGA_MIS_W); drivers/gpu/drm/i915/display/intel_vga.c
 * (intel_vga_disable: bit 5 of SR01).
 */
#define I915_VGA_SEQ_I			0x3C4U
#define I915_VGA_SEQ_D			0x3C5U
#define I915_VGA_SR01_SCREEN_OFF	0x20U
#define I915_VGA_MIS_R			0x3CCU
#define I915_VGA_MIS_W			0x3C2U

/* The PCI VGA display class (base 0x03, subclass 0x00) and the bits that name it. */
#define I915_PCI_CLASS_VGA_MASK		0xffff00U
#define I915_PCI_CLASS_VGA		0x030000U

static int i915_vga_client_register(struct i915_vga_client *c);
static int i915_vga_get_legacy_io(struct i915_display *display);
static unsigned char i915_vga_in8(struct i915_display *display, unsigned short port);
static int i915_vga_screen_off(void *context);
static void i915_vga_out8(struct i915_display *display, unsigned short port, unsigned char value);
static void i915_vga_put_legacy_io(struct i915_display *display);

/*
 * Registers the display device with the VGA arbiter.
 *
 * The client finds the host bridge, whose VGA decode it would switch; a
 * device that is not of the PCI VGA display class (a secondary
 * controller) is not a client, which is not an error.  Returns 0, or the
 * arbiter's error.
 */
int
drv_i915_vga_register(
	struct i915_vga_client *c,
	struct drv_pci_device *gpu,
	unsigned display_ver,
	struct i915_trace *trace)
{
	struct drv_pci_address bridge;
	const char *what;
	const char *client_state;
	const char *bridge_state;
	uint16_t op;
	int error;
	int reported_error;

	/* Binds the client to the display device. */
	c->gpu = gpu;
	c->display_ver = display_ver;
	c->registered = 0;

	/* The host bridge at 0:0.0, whose control word holds the VGA decode. */
	kern_memset(&bridge, 0, sizeof(bridge));
	c->gmch = drv_pci_find_device(&bridge);
	if (c->gmch == NULL)
		drv_i915_trace_record(trace, I915_VGA_TRACE_STAGE, I915_TRACE_NOTE, "vga register: no host bridge", 0U, 0U);

	/* Registers the decode callback; a real arbiter error ends the registration. */
	error = i915_vga_client_register(c);
	if (error != 0 && error != ENODEV)
		return error;

	/* The trace and the log keep the error as a negative code. */
	reported_error = -error;

	/* Records the registration, or the tolerated secondary controller. */
	op = I915_TRACE_NOTE;
	what = "vga register: not a VGA-class device";
	if (error == 0) {
		op = I915_TRACE_ACQUIRE;
		what = "vga register";
	}

	/* Records the registration's outcome. */
	drv_i915_trace_record(trace, I915_VGA_TRACE_STAGE, op, what, (uint64_t)(unsigned)reported_error, 0U);

	/* Reports the client and the host bridge. */
	client_state = (c->registered != 0) ? "registered" : "not-vga(-ENODEV)";
	bridge_state = (c->gmch != NULL) ? "found" : "absent";
	kern_logf("i915: vga register: client=%s gmch_bridge=%s ret=%d\n", client_state, bridge_state, reported_error);

	/* Succeeded: the device takes part in arbitration, or is a tolerated secondary. */
	return 0;
}

/*
 * Unregisters the display device from the VGA arbiter.
 */
void
drv_i915_vga_unregister(
	struct i915_vga_client *c)
{
	/* Forgets the callback, the device and the host bridge. */
	c->registered = 0;
	c->gpu = NULL;
	c->gmch = NULL;
}

/*
 * Resets the legacy VGA I/O state after a power well that holds it came
 * back.
 *
 * Owns the legacy I/O resource, writes the Misc Output register back to
 * itself so a text console keeps working, then releases the resource.
 * I/O ownership is independent of the decode-client registration; if the
 * resource cannot be acquired there is no I/O and no put.  c must be the
 * vga_client of a struct i915_display (the power-well code holds only the
 * client); the legacy I/O accessor is that display's.
 */
void
drv_i915_vga_reset_io_mem(
	struct i915_vga_client *c)
{
	struct i915_display *display;
	unsigned char misc;
	int owned;

	/* The client is embedded in the display whose accessor is used. */
	display = container_of(c, struct i915_display, vga_client);

	/* Does nothing when the legacy I/O resource cannot be owned. */
	owned = i915_vga_get_legacy_io(display);
	if (owned == 0)
		return;

	/* Writes the Misc Output register back to itself. */
	misc = i915_vga_in8(display, I915_VGA_MIS_R);
	i915_vga_out8(display, I915_VGA_MIS_W, misc);

	/* Gives the resource back. */
	i915_vga_put_legacy_io(display);
}

/*
 * Disables the VGA plane the driver never uses, through the xd parts
 * (drv_i915_xd_vga_off): the legacy sequencer's screen off is the
 * client's, since the legacy VGA I/O belongs to it.  Returns 0
 * after disabling the plane, 1 when it was already disabled (not an
 * errno), or EIO when the legacy I/O could not be owned.
 */
int
drv_i915_vga_disable(
	struct i915_display *display)
{
	int result;

	/* The screen off through the sequencer, then the plane. */
	result = drv_i915_xd_vga_off(&display->xd.io, i915_vga_screen_off, display);
	if (result == EIO)
		return EIO;

	/* Succeeded: the plane is off (1 when it already was). */
	return result;
}

/* Registers the client with the arbiter; only a PCI VGA-class device is a client. */
static int
i915_vga_client_register(
	struct i915_vga_client *c)
{
	uint32_t class_code;

	/* A device that is not of the PCI VGA display class is not an arbiter client. */
	class_code = drv_pci_device_class(c->gpu);
	if ((class_code & I915_PCI_CLASS_VGA_MASK) != I915_PCI_CLASS_VGA)
		return ENODEV;

	/* The callback is installed. */
	c->registered = 1;

	/* Succeeded: the device is an arbiter client. */
	return 0;
}

/* Takes the legacy VGA I/O resource: 1 when owned, 0 when refused. */
static int
i915_vga_get_legacy_io(
	struct i915_display *display)
{
	const struct i915_vga_io_ops *ops;
	int owned;

	/* Without an accessor of its own the driver owns the legacy VGA I/O of this device, so the get succeeds. */
	ops = display->g_vga_io;
	if (ops == NULL)
		return 1;

	/* Asks the injected accessor. */
	owned = ops->get(ops->ctx, (int)I915_VGA_RSRC_LEGACY_IO);

	/* Succeeded: reports whether the resource was granted. */
	return owned;
}

/* Reads a legacy VGA port. */
static unsigned char
i915_vga_in8(
	struct i915_display *display,
	unsigned short port)
{
	const struct i915_vga_io_ops *ops;
	unsigned char value;

	/* Reads through the injected accessor, or the port itself. */
	ops = display->g_vga_io;
	if (ops != NULL)
		value = ops->in8(ops->ctx, port);
	else
		value = kern_io_in8(port);

	/* Succeeded: reports the port value. */
	return value;
}

/* Turns the screen off through sequencer register SR01 of the legacy VGA I/O; 0, or EIO when the I/O could not be owned. */
static int
i915_vga_screen_off(
	void *context)
{
	struct i915_display *display;
	unsigned char sr1;
	int owned;

	/* Owns the legacy I/O first. */
	display = context;
	owned = i915_vga_get_legacy_io(display);
	if (owned == 0)
		return EIO;

	/* SR01's screen-off bit. */
	i915_vga_out8(display, I915_VGA_SEQ_I, 0x01U);
	sr1 = i915_vga_in8(display, I915_VGA_SEQ_D);
	i915_vga_out8(display, I915_VGA_SEQ_D, (unsigned char)(sr1 | I915_VGA_SR01_SCREEN_OFF));
	i915_vga_put_legacy_io(display);

	/* Succeeded: the screen is off. */
	return 0;
}

/* Writes a legacy VGA port. */
static void
i915_vga_out8(
	struct i915_display *display,
	unsigned short port,
	unsigned char value)
{
	const struct i915_vga_io_ops *ops;

	/* Writes through the injected accessor, or the port itself. */
	ops = display->g_vga_io;
	if (ops != NULL)
		ops->out8(ops->ctx, port, value);
	else
		kern_io_out8(port, value);
}

/* Gives the legacy VGA I/O resource back; without an accessor of its own there is nothing to give back. */
static void
i915_vga_put_legacy_io(
	struct i915_display *display)
{
	const struct i915_vga_io_ops *ops;

	/* Only an injected accessor has a resource to release. */
	ops = display->g_vga_io;
	if (ops != NULL)
		ops->put(ops->ctx, (int)I915_VGA_RSRC_LEGACY_IO);
}
