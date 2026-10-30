/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver stages of the xd display (see xd.h): the start, which
 * chooses the device's description; the south display's kind once the PCH
 * is known; and the read of the firmware's OpRegion and its VBT.
 */

#include "display-internal.h"
#include "xd.h"

#include "../intel/display-dpcd-xd.h"
#include "../intel/display-regs-xd.h"
#include "../intel/xd-vbt-layout.h"

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/platform.h>
#include <kern/pmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

static int i915_xd_map_device(void *context, uint64_t address, size_t size, const volatile uint8_t **bytes);
static void i915_xd_unmap_device(void *context, const volatile uint8_t *bytes, size_t size);
static const char *i915_xd_south_name(int pch_kind);
static const char *i915_xd_vbt_source_name(unsigned source);
static int i915_xd_hypervisor(void);
static int i915_xd_vtd_read(void *context, uint64_t base, uint32_t *version, uint32_t *status, uint32_t *protect);
static int i915_xd_panel_ddi(const struct i915_xd *xd);
static unsigned i915_xd_panel_controller(struct i915_xd *xd);
static void i915_xd_takeover_backlight_off(void *context, unsigned pipe);
static void i915_xd_takeover_sink_after(void *context, unsigned pipe);
static void i915_xd_takeover_panel_off(void *context, unsigned pipe);
static void i915_xd_takeover_port_release(void *context, unsigned pipe);
static void i915_xd_takeover_link_release(void *context, unsigned pipe);

/*
 * What the hooks of a takeover's stop work with: the display, the
 * stopped pipe's DDI, the panel's power sequencer and its waits, the
 * caller's power, and the functions held for the stopped pipe's port
 * (its DDI IO) and link (its AUX channel), 0 for none held.
 */
struct i915_xd_takeover_ctx {
	struct i915_xd *xd;
	unsigned ddi;
	unsigned pps;
	struct i915_xd_pps_waits waits;
	const struct i915_xd_takeover_power *power;
	uint64_t port_held;
	uint64_t link_held;
};

static void i915_xd_takeover_hold(struct i915_xd_takeover_ctx *ctx, unsigned pipe);

/*
 * The panel power sequencers of the south displays of display 12 and 13
 * (ICP to ADP), and whether the second one shares its pins.
 *
 * Values referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_num_pps,
 * intel_pps_is_valid).
 */
#define I915_XD_PANEL_SEQUENCERS	2U
#define I915_XD_PANEL_SEQUENCER_MUXED	1

/*
 * Prepares the xd display of a device and chooses its description.
 *
 * The description is chosen by the PCI product and revision; the PCH is
 * not known yet, so the platform's own south display is assumed until
 * drv_i915_xd_south() reads it.  A device without a description keeps
 * chosen at zero and only logs it: its display does not use the xd parts.
 */
void
drv_i915_xd_start(
	struct i915_xd *xd,
	struct i915_mmio *mmio,
	struct mutex *mailbox_lock,
	uint16_t product,
	uint8_t revid)
{
	int error;

	/* Starts from nothing chosen and nothing read. */
	kern_memset(xd, 0, sizeof(*xd));

	/* The register window writes the device's registers; the mailbox is serialized with the GT's own users. */
	drv_i915_xd_io_init(&xd->io, mmio, mailbox_lock);

	/* The Dekel PHY windows of the Type-C ports, reached through that window. */
	drv_i915_xd_dkl_init(&xd->dkl, &xd->io);

	/* No screen is prepared yet, and none is described until a description is chosen. */
	drv_i915_xd_pictures_init(&xd->pictures, NULL);

	/* Chooses the description by the product and revision. */
	error = drv_i915_xd_platform_choose(product, revid, 0, 0U, &xd->choice);
	if (error != 0) {
		kern_logf("i915: xd: no description of the device 0x%04x: the xd display is not used\n", (unsigned)product);
		return;
	}

	/* Records and reports the choice; the screens are described by it. */
	xd->chosen = 1;
	drv_i915_xd_pictures_platform(&xd->pictures, xd->choice.platform);
	kern_logf("i915: xd: platform %s, display %u, stepping %u (south display %s assumed until the PCH is read)\n",
	    xd->choice.platform->name,
	    (unsigned)xd->choice.platform->display_ver,
	    (unsigned)xd->choice.step,
	    i915_xd_south_name(xd->choice.pch_kind));
}

/*
 * Chooses the description again now that the PCH is known.
 *
 * pch_id is the PCH's PCI device id, or 0 when no PCH was found (a
 * virtual machine): the platform's own south display stays assumed.
 */
void
drv_i915_xd_south(
	struct i915_xd *xd,
	uint16_t product,
	uint8_t revid,
	uint16_t pch_id)
{
	struct i915_xd_choice choice;
	int pch_present;
	int error;

	/* A device without a description has no south display to read. */
	if (!xd->chosen)
		return;

	/* Chooses again with the PCH. */
	pch_present = 0;
	if (pch_id != 0U)
		pch_present = 1;
	error = drv_i915_xd_platform_choose(product, revid, pch_present, pch_id, &choice);
	if (error != 0) {
		kern_logf("i915: xd: the PCH 0x%04x does not go with platform %s (error %d): the description is kept\n",
		    (unsigned)pch_id,
		    xd->choice.platform->name,
		    error);
		return;
	}

	/* Reports a description that changed with the PCH. */
	if (choice.platform != xd->choice.platform)
		kern_logf("i915: xd: the PCH changed the description from %s to %s\n", xd->choice.platform->name, choice.platform->name);

	/* Keeps the choice, which describes the screens, and reports the south display. */
	xd->choice = choice;
	drv_i915_xd_pictures_platform(&xd->pictures, choice.platform);
	kern_logf("i915: xd: platform %s, south display %s (%s, PCH 0x%04x)\n",
	    choice.platform->name,
	    i915_xd_south_name(choice.pch_kind),
	    choice.pch_assumed ? "assumed" : "read",
	    (unsigned)pch_id);
}

/*
 * Reads the firmware's OpRegion and the VBT it keeps into the copies.
 *
 * The OpRegion and the raw VBT are mapped read-only for the copy and
 * unmapped again.  Returns what drv_i915_xd_opregion_read() returns; the
 * result is kept in xd->opregion and logged.
 */
int
drv_i915_xd_read_opregion(
	struct i915_xd *xd,
	uint32_t asls,
	uint8_t *opregion_copy,
	uint8_t *vbt_copy,
	size_t vbt_room)
{
	struct i915_xd_opregion_access access;
	const struct i915_xd_opregion_read *read;
	const uint8_t *digest;
	int error;

	/* Reads through read-only device mappings. */
	access.map = i915_xd_map_device;
	access.unmap = i915_xd_unmap_device;
	access.context = NULL;
	error = drv_i915_xd_opregion_read(&access, asls, opregion_copy, vbt_copy, vbt_room, &xd->opregion);

	/* Reports what was found. */
	read = &xd->opregion;
	digest = read->vbt_sha256;
	if (read->present == 0) {
		kern_logf("i915: xd-vbt: no OpRegion (ASLS 0)\n");
	} else if (error != 0) {
		kern_logf("i915: xd-vbt: OpRegion at 0x%08x: no VBT (error %d, mapped %d, version %u.%u, raw VBT status %u)\n",
		    asls,
		    error,
		    read->mapped,
		    (unsigned)read->where.major,
		    (unsigned)read->where.minor,
		    read->raw_status);
	} else {
		kern_logf("i915: xd-vbt: OpRegion at 0x%08x version %u.%u mailboxes 0x%x: VBT from %s, %u bytes, sha256 %02x%02x%02x%02x%02x%02x%02x%02x.. (raw VBT status %u)\n",
		    asls,
		    (unsigned)read->where.major,
		    (unsigned)read->where.minor,
		    read->where.mailboxes,
		    i915_xd_vbt_source_name(read->source),
		    read->vbt_size,
		    digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
		    read->raw_status);
	}

	/* Reports a failed read. */
	if (error != 0)
		return error;

	/* Succeeded: the VBT is in vbt_copy. */
	return 0;
}

/*
 * Reads the display the firmware left, before the driver's first display
 * write, into xd->firmware: decides whether the display may be used and
 * names the output the firmware lit (see xd-takeover.h).  The aperture,
 * the GGTT size and the first GGTT page the driver writes, and the
 * OpRegion's address (ASLS) are the device's.  Returns what
 * drv_i915_xd_firmware_read() returns.
 */
int
drv_i915_xd_check_firmware(
	struct i915_xd *xd,
	uint64_t aperture_base,
	uint64_t aperture_size,
	uint32_t ggtt_pages,
	uint32_t driver_first_page,
	uint32_t asls)
{
	struct kern_boot_framebuffer framebuffer;
	const struct i915_xd_platform *platform;
	struct i915_xd_fw_env env;
	int panel_ddi;
	int error;

	/* The machine, the GGTT the driver writes, and a reader of the VT-d unit. */
	kern_memset(&env, 0, sizeof(env));
	env.hypervisor = i915_xd_hypervisor();
	env.aperture_base = aperture_base;
	env.aperture_size = aperture_size;
	env.ggtt_pages = ggtt_pages;
	env.driver_first_page = driver_first_page;
	env.vtd_read = i915_xd_vtd_read;
	env.context = NULL;

	/* The firmware's framebuffer from the boot handoff. */
	error = kern_boot_framebuffer("pcat.framebuffer", &framebuffer);
	if (error == 0) {
		env.fb_present = 1;
		env.fb_base = framebuffer.physical_base;
		env.fb_size = framebuffer.size;
	}

	/* Reads and decides; a device without a description is recorded as one. */
	platform = NULL;
	if (xd->chosen)
		platform = xd->choice.platform;
	error = drv_i915_xd_firmware_read(&xd->io, platform, &env, asls, &xd->firmware);

	/* Names the output by the panel's DDI in the VBT. */
	panel_ddi = i915_xd_panel_ddi(xd);
	drv_i915_xd_firmware_classify(platform, panel_ddi, &xd->firmware);
	drv_i915_xd_firmware_log(&xd->firmware);

	/* Reports a device without a description. */
	if (error != 0)
		return error;

	/* Succeeded: the record and its decision. */
	return 0;
}

/*
 * Makes the table of the display's ports from the VBT's outputs.
 *
 * Each output the VBT describes becomes a port when its DDI is one of
 * ddi_mask and of the platform, no earlier output took the DDI, it is not
 * a MIPI DSI output, and it carries DisplayPort, HDMI or DVI.  A port
 * keeps its DDI, its Type-C port (0 for none), what it carries, its AUX
 * channel (the platform's when the VBT names none) and the VBT's device
 * type.  The outputs that did not become a port are counted.
 */
void
drv_i915_xd_ports(
	const struct i915_xd_platform *platform,
	const struct i915_xd_vbt *vbt,
	unsigned ddi_mask,
	struct i915_xd_ports *ports)
{
	const struct i915_xd_port_desc *desc;
	const struct i915_xd_vbt_child *child;
	struct i915_xd_port *port;
	unsigned index;
	unsigned taken;

	/* Starts with no port. */
	kern_memset(ports, 0, sizeof(*ports));

	/* A device without a description has no ports. */
	if (platform == NULL)
		return;

	/* Each output of the VBT, in its order. */
	taken = 0U;
	for (index = 0U; index < vbt->nchildren; index++) {
		child = &vbt->children[index];

		/* An output on no DDI the platform has. */
		if (child->ddi == I915_XD_VBT_NONE || child->ddi >= 32U) {
			ports->skipped++;
			continue;
		}

		/* An output on a DDI the platform or the caller does not drive, or that an earlier output took. */
		desc = drv_i915_xd_platform_port(platform, child->ddi);
		if (desc == NULL ||
		    (ddi_mask & (1U << child->ddi)) == 0U ||
		    (taken & (1U << child->ddi)) != 0U) {
			ports->skipped++;
			continue;
		}

		/* A MIPI DSI output, or one that carries neither DisplayPort nor HDMI nor DVI. */
		if ((child->device_type & I915_XD_VBT_TYPE_MIPI) != 0U ||
		    (!child->dp && !child->hdmi && !child->dvi)) {
			ports->skipped++;
			continue;
		}

		/* No room for another port. */
		if (ports->n >= I915_XD_PORTS_MAX) {
			ports->skipped++;
			continue;
		}

		/* The port: its DDI, its Type-C port, what it carries, its AUX channel and the VBT's device type. */
		port = &ports->port[ports->n];
		port->ddi = child->ddi;
		port->tc = desc->tc;
		port->dp = child->dp;
		port->edp = child->edp;
		port->device_type = child->device_type;

		/* A DVI output is driven as HDMI. */
		port->hdmi = 0;
		if (child->hdmi || child->dvi)
			port->hdmi = 1;

		/* The VBT's AUX channel, or the platform's when it names none. */
		port->aux = child->aux_ch;
		if (port->aux == I915_XD_VBT_NONE)
			port->aux = desc->aux;

		/* The port is in the table, and its DDI is taken. */
		ports->n++;
		taken |= 1U << child->ddi;
	}
}

/* Maps device memory read-only for the OpRegion reader; 0, or EIO. */
static int
i915_xd_map_device(
	void *context,
	uint64_t address,
	size_t size,
	const volatile uint8_t **bytes)
{
	void *mapping;
	int status;

	UNUSED_PARAMETER(context);

	/* Asks the kernel for a read-only mapping. */
	mapping = NULL;
	status = kern_device_map(address, size, KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY, &mapping);
	if (status != 0)
		return EIO;
	if (mapping == NULL)
		return EIO;

	/* Succeeded: the memory can be read. */
	*bytes = mapping;
	return 0;
}

/* Gives back a mapping the OpRegion reader made. */
static void
i915_xd_unmap_device(
	void *context,
	const volatile uint8_t *bytes,
	size_t size)
{
	(void)context;

	/* Removes the mapping; nothing is left to do if that fails. */
	(void)kern_device_unmap((void *)(uintptr_t)bytes, size);
}

/* Names a south display kind for the log. */
static const char *
i915_xd_south_name(
	int pch_kind)
{
	/* The south displays the descriptions name. */
	if (pch_kind == I915_PCH_ICP)
		return "ICP";
	if (pch_kind == I915_PCH_TGP)
		return "TGP";
	if (pch_kind == I915_PCH_ADP)
		return "ADP";

	/* Any other kind. */
	return "none";
}

/* Names where a VBT was taken from, for the log. */
static const char *
i915_xd_vbt_source_name(
	unsigned source)
{
	/* The raw VBT or mailbox 4. */
	if (source == I915_XD_VBT_FROM_RAW)
		return "the raw VBT";
	if (source == I915_XD_VBT_FROM_MAILBOX)
		return "mailbox 4";

	/* No VBT. */
	return "nowhere";
}

/* Reports CPUID.1:ECX[31], the bit a hypervisor sets for its guests. */
static int
i915_xd_hypervisor(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	/* Asks CPUID leaf 1. */
	eax = 1U;
	__asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	(void)ebx;
	(void)edx;

	/* Succeeded: the hypervisor bit. */
	return (int)((ecx >> 31) & 1U);
}

/*
 * Reads the version, the global status and the protected memory enable
 * of a VT-d unit through a read-only mapping; 0, or EIO when the unit
 * cannot be mapped.
 *
 * Values referenced: Intel Virtualization Technology for Directed I/O
 * Architecture Specification (VER_REG at 0x0, GSTS_REG at 0x1c, PMEN_REG
 * at 0x64).
 */
static int
i915_xd_vtd_read(
	void *context,
	uint64_t base,
	uint32_t *version,
	uint32_t *status,
	uint32_t *protect)
{
	const volatile uint8_t *unit;
	int error;

	/* Maps the unit's page read-only. */
	error = i915_xd_map_device(context, base, 0x1000U, &unit);
	if (error != 0)
		return error;

	/* The three words. */
	*version = *(const volatile uint32_t *)(unit + 0x00U);
	*status = *(const volatile uint32_t *)(unit + 0x1cU);
	*protect = *(const volatile uint32_t *)(unit + 0x64U);

	/* Gives the mapping back. */
	i915_xd_unmap_device(context, unit, 0x1000U);

	/* Succeeded: the words are read. */
	return 0;
}

/* Returns the DDI of the built-in panel the VBT names, or DDI A when the VBT names none (or was not parsed). */
static int
i915_xd_panel_ddi(
	const struct i915_xd *xd)
{
	unsigned index;

	/* Without a parsed VBT, the panel's DDI is A. */
	if (!xd->vbt_parsed)
		return 0;

	/* The first output of the VBT that is an eDP. */
	for (index = 0U; index < xd->vbt.nchildren; index++) {
		if (xd->vbt.children[index].edp)
			return (int)xd->vbt.children[index].ddi;
	}

	/* Succeeded: no eDP child, so DDI A. */
	return 0;
}

/*
 * Takes the firmware's display over: every pipe the firmware left lit
 * is stopped in the order an output is disabled
 * (drv_i915_xd_takeover_stop).  A panel pipe's backlight goes off first
 * and its power last; a DisplayPort sink is put to sleep before its port
 * stops.  An HDMI sink that was scrambling ends it by itself once the
 * scrambled clock is gone, so it is not told.  The caller holds the
 * display's start reference (every pipe's wells and the display out of
 * its deep power states); the wells of the stopped pipe's port (its DDI
 * IO) and, on DisplayPort, of its AUX channel are taken through power
 * for the stop and given back where an output's disable gives them back:
 * the port's after the DDI is off, the AUX channel's after the PLL.
 * power may be NULL (nothing taken).  Returns 0 when every lit pipe was
 * stopped, or the error of the first that was not (outcome says which
 * pipes are still lit).
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_ddi.c (intel_ddi_get_power_domains,
 * intel_ddi_post_disable_dp, intel_ddi_post_disable).
 */
int
drv_i915_xd_takeover(
	struct i915_xd *xd,
	const struct i915_xd_takeover_power *power,
	struct i915_xd_takeover_outcome *outcome)
{
	struct i915_xd_takeover_hooks hooks;
	struct i915_xd_takeover_report report;
	struct i915_xd_takeover_ctx ctx;
	struct i915_xd_pps_delays firmware;
	struct i915_xd_pps_delays vbt;
	struct i915_xd_pps_delays program;
	uint32_t function;
	unsigned pipe;
	int error;

	/* Nothing stopped yet. */
	kern_memset(outcome, 0, sizeof(*outcome));
	outcome->lit = xd->firmware.lit_pipes;

	/* A device without a description has no parts to stop its pipes with. */
	if (!xd->chosen) {
		outcome->still_lit = outcome->lit;
		if (outcome->lit != 0U)
			outcome->error = ENODEV;
		return outcome->error;
	}

	/*
	 * The panel's sequencer, and how long its backlight takes to go
	 * dark: the delays the firmware programmed (the VBT's do not apply
	 * to the firmware's sequence).
	 */
	kern_memset(&ctx, 0, sizeof(ctx));
	ctx.xd = xd;
	ctx.power = power;
	ctx.pps = i915_xd_panel_controller(xd);
	drv_i915_xd_pps_read_delays(&xd->io, ctx.pps, &firmware);
	kern_memset(&vbt, 0, sizeof(vbt));
	drv_i915_xd_pps_choose(&firmware, &vbt, &program, &ctx.waits);

	/* The hooks of the parts the stop does not own. */
	kern_memset(&hooks, 0, sizeof(hooks));
	hooks.backlight_off = i915_xd_takeover_backlight_off;
	hooks.sink_after = i915_xd_takeover_sink_after;
	hooks.panel_off = i915_xd_takeover_panel_off;
	hooks.port_release = i915_xd_takeover_port_release;
	hooks.link_release = i915_xd_takeover_link_release;
	hooks.context = &ctx;

	/* Stops each lit pipe, the lowest first. */
	for (pipe = 0U; pipe < I915_XD_FW_PIPES; pipe++) {
		if ((xd->firmware.lit_pipes & (1U << pipe)) == 0U)
			continue;

		/* The DDI the pipe drives, as the firmware check read it. */
		function = xd->firmware.pipe[pipe].ddi_function;
		ctx.ddi = i915_xd_field_get(I915_XD_DDI_FUNC_SELECT, function);
		if (ctx.ddi != 0U)
			ctx.ddi--;

		/* The port's and the link's wells, held for the stop. */
		i915_xd_takeover_hold(&ctx, pipe);

		/* Stops the pipe and logs how it went; the hooks gave the wells back. */
		error = drv_i915_xd_takeover_stop(&xd->io, xd->choice.platform, &xd->dmc, &xd->firmware, pipe, &hooks, &report);
		i915_xd_takeover_port_release(&ctx, pipe);
		i915_xd_takeover_link_release(&ctx, pipe);
		kern_logf("i915: takeover: pipe %c stopped: DDI %d mode %u, PLL %d, late 0x%x (rc %d)\n",
		    'A' + (int)pipe,
		    report.ddi,
		    report.mode,
		    report.pll,
		    report.late,
		    error);
		if (error != 0) {
			if (outcome->error == 0)
				outcome->error = error;
			continue;
		}

		/* The pipe was stopped. */
		outcome->stopped |= 1U << pipe;
	}

	/* What stays lit. */
	outcome->still_lit = xd->firmware.lit_pipes;
	if (outcome->still_lit != 0U && outcome->error == 0)
		outcome->error = EBUSY;

	/* Succeeded when nothing stays lit. */
	return outcome->error;
}

/*
 * The panel's power sequencer and backlight controller: the VBT's
 * backlight controller names both, as the panel's power and its
 * backlight are one block of the south display; without a VBT, the
 * sequencer that has the panel powered, then the first one.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (bxt_power_sequencer_idx,
 * bxt_initial_pps_idx).
 */
static unsigned
i915_xd_panel_controller(
	struct i915_xd *xd)
{
	const struct i915_xd_vbt_child *child;
	struct i915_xd_vbt_panel panel;
	int found;
	int error;
	int usable;

	/* The VBT's controller of the panel's output, when it is one the display has. */
	if (xd->vbt_parsed) {
		child = drv_i915_xd_vbt_child(&xd->vbt, (unsigned)i915_xd_panel_ddi(xd));
		error = EINVAL;
		if (child != NULL)
			error = drv_i915_xd_vbt_panel(&xd->vbt, child, NULL, 0U, &panel);

		/* The VBT's controller, when it names one of the sequencers and it is usable. */
		if (error == 0 && panel.backlight_controller < I915_XD_PANEL_SEQUENCERS) {
			usable = drv_i915_xd_pps_usable(&xd->io, panel.backlight_controller, I915_XD_PANEL_SEQUENCERS, I915_XD_PANEL_SEQUENCER_MUXED);
			if (usable)
				return panel.backlight_controller;
		}
	}

	/* The sequencer that has the panel powered. */
	found = drv_i915_xd_pps_find(&xd->io, I915_XD_PANEL_SEQUENCERS, I915_XD_PPS_FIND_POWERED);
	if (found >= 0)
		return (unsigned)found;

	/* Succeeded: the first one. */
	return 0U;
}

/*
 * A takeover's panel backlight off: the sequencer's backlight enable,
 * the backlight's off delay, then the PWM at duty 0.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_backlight_off),
 * intel_backlight.c (intel_backlight_disable, cnp_disable_backlight).
 */
static void
i915_xd_takeover_backlight_off(
	void *context,
	unsigned pipe)
{
	struct i915_xd_takeover_ctx *ctx;
	struct i915_xd_backlight backlight;

	UNUSED_PARAMETER(pipe);

	/* The sequencer's enable, and the time the panel takes to go dark. */
	ctx = context;
	drv_i915_xd_pps_backlight(&ctx->xd->io, ctx->pps, 0);
	(void)drv_i915_xd_io_delay(&ctx->xd->io, ctx->waits.backlight_off_ms * 1000U);

	/* The PWM, at duty 0. */
	drv_i915_xd_backlight_read(&backlight, &ctx->xd->io, ctx->pps);
	drv_i915_xd_backlight_disable(&backlight, &ctx->xd->io, 0U);
}

/*
 * A takeover's DisplayPort sink to sleep (D3) over its port's AUX
 * channel, whose well the takeover holds; the panel's VDD is turned on
 * for the channel and left on for the panel's power off.  A sink that
 * does not answer is only logged.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_dp.c (intel_dp_set_power),
 * intel_dp_aux.c (intel_dp_aux_xfer), intel_pps.c
 * (intel_pps_vdd_on_unlocked).
 */
static void
i915_xd_takeover_sink_after(
	void *context,
	unsigned pipe)
{
	const struct i915_xd_port_desc *port;
	struct i915_xd_takeover_ctx *ctx;
	struct i915_xd_aux aux;
	uint8_t power;
	int forced;
	int powered;
	int error;

	/* The port's AUX channel. */
	ctx = context;
	port = drv_i915_xd_platform_port(ctx->xd->choice.platform, ctx->ddi);
	if (port == NULL)
		return;
	error = drv_i915_xd_aux_init(&aux, port->aux);
	if (error != 0)
		return;

	/*
	 * The panel's AUX channel answers with VDD on: held from here until
	 * the panel's power goes off (a panel without power gets its
	 * power-up delay first).
	 */
	if ((int)ctx->ddi == ctx->xd->firmware.panel_ddi) {
		forced = drv_i915_xd_pps_vdd_forced(&ctx->xd->io, ctx->pps);
		if (!forced) {
			powered = drv_i915_xd_pps_vdd_on(&ctx->xd->io, ctx->pps);
			if (!powered)
				(void)drv_i915_xd_io_delay(&ctx->xd->io, ctx->waits.power_up_ms * 1000U);
		}
	}

	/* The sink to D3. */
	power = I915_XD_DPCD_POWER_D3;
	error = drv_i915_xd_dpcd_write(&aux, &ctx->xd->io, I915_XD_DPCD_SET_POWER, &power, 1U);
	if (error != 0)
		kern_logf("i915: takeover: pipe %c: the sink did not go to sleep (rc %d)\n", 'A' + (int)pipe, error);
}

/*
 * A takeover's panel power off: VDD held first (the sequencer needs it
 * to turn the panel off; the sink's D3 has normally turned it on
 * already), then the power, the reset, VDD and the backlight enable
 * together, and the wait for the panel to be off.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_pps.c (intel_pps_off_unlocked).
 */
static void
i915_xd_takeover_panel_off(
	void *context,
	unsigned pipe)
{
	struct i915_xd_takeover_ctx *ctx;
	int forced;
	int error;

	/* VDD on when it is not already. */
	ctx = context;
	forced = drv_i915_xd_pps_vdd_forced(&ctx->xd->io, ctx->pps);
	if (!forced)
		(void)drv_i915_xd_pps_vdd_on(&ctx->xd->io, ctx->pps);

	/* Everything off. */
	error = drv_i915_xd_pps_power_off(&ctx->xd->io, ctx->pps);
	if (error != 0)
		kern_logf("i915: takeover: pipe %c: the panel did not go off (rc %d)\n", 'A' + (int)pipe, error);
}

/*
 * Takes the wells of a lit pipe's port for its stop: the DDI's IO, then,
 * on DisplayPort, its AUX channel.  A well that does not come up is
 * logged and not held; the stop goes on.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_ddi.c (intel_ddi_get_power_domains).
 */
static void
i915_xd_takeover_hold(
	struct i915_xd_takeover_ctx *ctx,
	unsigned pipe)
{
	const struct i915_xd_port_desc *port;
	uint64_t functions;
	uint32_t function;
	unsigned mode;
	int error;

	/* Nothing held yet; without the caller's power nothing is taken. */
	ctx->port_held = 0U;
	ctx->link_held = 0U;
	if (ctx->power == NULL)
		return;

	/* The port the pipe drives; one the description does not have is not stopped either. */
	port = drv_i915_xd_platform_port(ctx->xd->choice.platform, ctx->ddi);
	if (port == NULL)
		return;

	/* The DDI's IO. */
	functions = I915_XD_POWERS_DDI_IO(ctx->ddi);
	error = ctx->power->get(ctx->power->context, functions);
	if (error != 0) {
		kern_logf("i915: takeover: pipe %c: the DDI IO well did not come up (rc %d)\n", 'A' + (int)pipe, error);
	} else {
		ctx->port_held = functions;
	}

	/* A DisplayPort link's AUX channel, which the sink's D3 goes over. */
	function = ctx->xd->firmware.pipe[pipe].ddi_function;
	mode = i915_xd_field_get(I915_XD_DDI_FUNC_MODE, function);
	if (mode != I915_XD_DDI_FUNC_MODE_DP_SST)
		return;
	functions = I915_XD_POWERS_AUX(port->aux);
	error = ctx->power->get(ctx->power->context, functions);
	if (error != 0) {
		kern_logf("i915: takeover: pipe %c: the AUX well did not come up (rc %d)\n", 'A' + (int)pipe, error);
		return;
	}

	/* Succeeded: the link's well is held. */
	ctx->link_held = functions;
}

/*
 * A takeover's port wells back once its DDI is off (and after the stop
 * for a pipe that stopped early).
 */
static void
i915_xd_takeover_port_release(
	void *context,
	unsigned pipe)
{
	struct i915_xd_takeover_ctx *ctx;

	UNUSED_PARAMETER(pipe);

	/* Nothing held. */
	ctx = context;
	if (ctx->port_held == 0U)
		return;

	/* Gives the DDI IO back. */
	ctx->power->put(ctx->power->context, ctx->port_held);
	ctx->port_held = 0U;
}

/*
 * A takeover's link wells back once the PLL is off (and after the stop
 * for a pipe that stopped early).
 */
static void
i915_xd_takeover_link_release(
	void *context,
	unsigned pipe)
{
	struct i915_xd_takeover_ctx *ctx;

	UNUSED_PARAMETER(pipe);

	/* Nothing held. */
	ctx = context;
	if (ctx->link_held == 0U)
		return;

	/* Gives the AUX channel back. */
	ctx->power->put(ctx->power->context, ctx->link_held);
	ctx->link_held = 0U;
}
