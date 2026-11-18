/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The VBT as the older display code reads it, served by the xd VBT parse.
 *
 * The older code reads the VBT through struct i915_vbt_state and struct
 * i915_vbt (internal.h) and the functions of vbt-parse.h.  Each of them is
 * kept here with its name and its types, and its values come from the xd
 * parse (xd-vbt.c): the outputs, the general features and the panels.
 * The VBT bytes are still chosen here (the OpRegion's copy, else the PCI
 * expansion ROM), and the OpRegion reader the firmware display check uses
 * is kept until that check is replaced.  This file goes when the older
 * code that reads these types goes.
 */

#include "internal.h"
#include "vbt-parse.h"
#include "xd-vbt.h"

#include "../pci.h"
#include "../trace.h"

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* Where a VBT header keeps the VBT's size, and how long the header is. */
#define I915_VBT_OFF_VBT_SIZE		24U
#define I915_VBT_HEADER_SIZE		48U

/*
 * The PCI expansion ROM: its base address register, the address bits and the
 * decode enable, and the most of it that is mapped to look for a VBT.
 */
#define I915_VBT_PCI_ROM_BASE		0x30U
#define I915_VBT_ROM_ADDRESS_MASK	0xfffff800U
#define I915_VBT_ROM_ENABLE		1U
#define I915_VBT_ROM_MAP_MAX		(256U * 1024U)

/* The stage the VBT's trace records are filed under. */
#define I915_VBT_TRACE_STAGE		4U

/* How many child devices struct i915_vbt_state flattens. */
#define I915_VBT_STATE_MAX_CHILDREN	8U

/*
 * The VBT state of a display, kept for the display's lifetime: what the
 * older code's world was.  It holds nothing the xd parse does not.
 */
struct i915_vbt_world {
	struct i915_display *display;
};

static void i915_bios_choose_source(struct i915_display *display, struct i915_vbt_state *vbt, struct i915_pci *pci, const void **out, size_t *out_size, int *origin);
static int i915_bios_oprom_get_vbt(struct i915_display *display, struct i915_pci *pci, const void **out, size_t *out_size);
static int i915_bios_oprom_lift(struct i915_display *display, const volatile uint8_t *rom, size_t rom_size, size_t found, const void **out, size_t *out_size);
static void i915_bios_fill(struct i915_vbt *parsed, const struct i915_xd_vbt *xd_vbt, const void *bytes, size_t size, int origin);
static void i915_bios_output_set(struct i915_vbt_encoder *encoder, const struct i915_xd_vbt_child *child);
static void i915_bios_panel_set(struct i915_vbt_panel *out, const struct i915_xd_vbt *vbt, const struct i915_xd_vbt_panel *panel);
static void i915_bios_report(struct i915_vbt_state *vbt, struct i915_trace *trace);
static uint32_t i915_opregion_le32(const uint8_t *bytes);
static uint64_t i915_opregion_le64(const uint8_t *bytes);

/*
 * Creates the VBT state of a display.
 */
int
drv_i915_vbt_world_create(
	struct i915_display *display)
{
	struct i915_vbt_world *world;

	/* Allocates the state zeroed. */
	world = kern_calloc(1, sizeof(*world));
	if (world == NULL)
		return ENOMEM;

	/* Hands it to the display. */
	world->display = display;
	display->vbt_world = world;

	/* Succeeded: the display has its VBT state. */
	return 0;
}

/*
 * Frees the VBT state of a display.
 */
void
drv_i915_vbt_world_destroy(
	struct i915_display *display)
{
	/* A display without a state has nothing to free. */
	if (display->vbt_world == NULL)
		return;

	/* Frees the state. */
	kern_free(display->vbt_world);
	display->vbt_world = NULL;
}

/*
 * Remembers the OpRegion's VBT copy for the VBT stage.
 */
void
drv_i915_bios_set_opregion_vbt(
	struct i915_display *display,
	const void *buf,
	size_t size)
{
	/* The copy the VBT stage tries first. */
	display->opregion_vbt_buf = buf;
	display->opregion_vbt_size = size;
}

/*
 * Runs the VBT stage of the display start: chooses the VBT bytes, parses
 * them with the xd parser and puts what it found in the older code's types.
 *
 * A device the xd display does not describe gets no outputs.  pci reaches
 * the expansion ROM when the OpRegion has no VBT.  Returns 0.
 */
int
drv_i915_bios_init_ex(
	struct i915_display *display,
	struct i915_vbt_state *vbt,
	struct i915_pci *pci,
	int opregion_has_vbt,
	int explicit_blob,
	struct i915_trace *trace)
{
	const struct i915_vbt_encoder *encoder;
	struct i915_vbt_child *child;
	const void *vbt_buf;
	size_t vbt_size;
	unsigned i;
	int origin;
	int error;

	UNUSED_PARAMETER(opregion_has_vbt);
	UNUSED_PARAMETER(explicit_blob);

	/* Starts the record empty. */
	kern_memset(vbt, 0, sizeof(*vbt));
	vbt->has_display = 1;
	vbt->source = I915_VBT_SRC_NONE;

	/* Chooses the VBT bytes. */
	i915_bios_choose_source(display, vbt, pci, &vbt_buf, &vbt_size, &origin);
	vbt->vbt_found = 0;
	if (vbt_buf != NULL)
		vbt->vbt_found = 1;

	/* A device without an xd description has no outputs from the VBT. */
	if (!display->xd.chosen) {
		kern_logf("i915: VBT: the device has no xd description: no outputs are read from the VBT\n");
		vbt->parsed_live = 1;
		i915_bios_report(vbt, trace);
		return 0;
	}

	/* Parses the bytes (none: the platform's defaults). */
	error = drv_i915_xd_vbt_parse(display->xd.choice.platform, vbt_buf, vbt_size, &display->xd.vbt);
	if (error != 0) {
		kern_logf("i915: VBT: the parse failed (%d): the platform's defaults are used\n", error);
		(void)drv_i915_xd_vbt_parse(display->xd.choice.platform, NULL, 0U, &display->xd.vbt);
		vbt->vbt_found = 0;
	}

	/* The xd VBT holds a parse, the VBT's or the defaults, from here on. */
	display->xd.vbt_parsed = 1;

	/* Puts the parse in the older code's types. */
	i915_bios_fill(&vbt->parsed, &display->xd.vbt, vbt_buf, vbt_size, origin);
	vbt->parsed_live = 1;
	vbt->version = vbt->parsed.bdb_version;
	vbt->missing_defaults_used = vbt->parsed.missing_defaults_used;

	/* Flattens the child devices. */
	for (i = 0U; i < vbt->parsed.n_encoders && vbt->num_display_devices < I915_VBT_STATE_MAX_CHILDREN; i++) {
		encoder = &vbt->parsed.enc[i];
		child = &vbt->display_devices[vbt->num_display_devices++];
		child->port = 0U;
		if (encoder->port >= 0)
			child->port = (unsigned)encoder->port;
		child->dvo_port = encoder->dvo_port;
		child->device_type = encoder->device_type;
	}

	/* Reports the source, the parse and the children. */
	i915_bios_report(vbt, trace);

	/* Succeeded: vbt holds the parse. */
	return 0;
}

/*
 * Forgets the VBT stage's result.
 */
void
drv_i915_bios_driver_remove(
	struct i915_vbt_state *vbt)
{
	/* Nothing was parsed, or it was already forgotten. */
	if (vbt == NULL)
		return;

	/* Forgets the parse. */
	vbt->parsed_live = 0;
	vbt->parsed.inited = 0;
	vbt->parsed.xd = NULL;
}

/*
 * Hashes data with SHA-256.
 */
void
drv_i915_sha256(
	const void *data,
	size_t len,
	uint8_t out[32])
{
	/* The xd parser's digest. */
	drv_i915_xd_sha256(data, len, out);
}

/*
 * Reports whether bytes hold a VBT that can be parsed: 1, or 0.
 */
int
drv_i915_vbt_validate(
	const void *bytes,
	size_t size)
{
	int error;

	/* The xd parser's check. */
	error = drv_i915_xd_vbt_check(bytes, size);
	if (error != 0)
		return 0;

	/* Succeeded: the VBT can be parsed. */
	return 1;
}

/*
 * Finds the output of the VBT on a port, or NULL.
 */
const struct i915_vbt_encoder *
drv_i915_vbt_encoder_for_port(
	const struct i915_vbt *v,
	int port)
{
	unsigned i;

	/* No parse has no outputs. */
	if (v == NULL)
		return NULL;
	if (!v->inited)
		return NULL;

	/* Looks for the port among the outputs. */
	for (i = 0U; i < v->n_encoders; i++) {
		if (v->enc[i].port == port)
			return &v->enc[i];
	}

	/* No output is on the port. */
	return NULL;
}

/*
 * Reads what the VBT says about the panel on a port.
 *
 * edid128, when not NULL, is the panel's EDID base block, which picks the
 * panel's row when the VBT leaves that to the EDID.  Returns 0, EINVAL
 * without a parse, ENODEV when no output is on the port, or ENODATA when
 * the VBT has no panel for it.
 */
int
drv_i915_vbt_init_panel(
	struct i915_vbt *v,
	int port,
	const uint8_t *edid128,
	struct i915_vbt_panel *out)
{
	const struct i915_xd_vbt_child *child;
	struct i915_xd_vbt_panel panel;
	size_t edid_size;
	int error;

	/* Refuses anything but a live parse. */
	if (v == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (!v->inited)
		return EINVAL;
	if (v->xd == NULL)
		return EINVAL;

	/* Finds the output on the port. */
	kern_memset(out, 0, sizeof(*out));
	if (port < 0)
		return ENODEV;
	child = drv_i915_xd_vbt_child(v->xd, (unsigned)port);
	if (child == NULL)
		return ENODEV;

	/* Reads its panel. */
	edid_size = 0U;
	if (edid128 != NULL)
		edid_size = 128U;
	error = drv_i915_xd_vbt_panel(v->xd, child, edid128, edid_size, &panel);
	if (error != 0)
		return ENODATA;

	/* Puts it in the older code's type. */
	i915_bios_panel_set(out, v->xd, &panel);
	out->initialized = 1;

	/* Succeeded: out holds the panel data. */
	return 0;
}

/*
 * Reads where the OpRegion keeps its VBT, for the firmware display check.
 *
 * op is a copy of the 8 KiB OpRegion found at asls.  The raw VBT (RVDA)
 * is the candidate from version 2 on, else mailbox #4; the runtime
 * mailboxes are recorded as the firmware left them.  Returns 0, or EINVAL
 * when the copy is missing, too short or has the wrong signature.
 */
int
drv_i915_opregion_locate_vbt(
	const uint8_t *op,
	size_t len,
	uint64_t asls,
	struct i915_opregion_info *out)
{
	static const char signature[16] = { 'I', 'n', 't', 'e', 'l', 'G', 'r', 'a', 'p', 'h', 'i', 'c', 's', 'M', 'e', 'm' };
	const uint8_t *asle;
	unsigned i;

	/* Refuses a missing or short copy with EINVAL. */
	if (op == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (len < I915_OPREGION_SIZE)
		return EINVAL;

	/* Starts the record empty. */
	kern_memset(out, 0, sizeof(*out));

	/* Refuses a copy without the OpRegion signature. */
	for (i = 0u; i < 16u; i++) {
		if (op[i] != (uint8_t)signature[i])
			return EINVAL;
	}

	/* Records the header: size, version (struct { u8 rsvd, revision, minor, major } at 0x14) and mailboxes. */
	out->signature_ok = 1;
	out->size_kib = i915_opregion_le32(op + 16);
	out->revision = op[21];
	out->minor = op[22];
	out->major = op[23];
	out->mboxes = i915_opregion_le32(op + 0x58);

	/* Records struct opregion_acpi (0x100): drdy 0x00, csts 0x04, cevt 0x08, chpd 0xa8, clid 0xac. */
	out->acpi_drdy = i915_opregion_le32(op + 0x100);
	out->acpi_csts = i915_opregion_le32(op + 0x104);
	out->acpi_cevt = i915_opregion_le32(op + 0x108);
	out->acpi_chpd = i915_opregion_le32(op + 0x1a8);
	out->acpi_clid = i915_opregion_le32(op + 0x1ac);

	/* Records struct opregion_asle (0x300): ardy 0x00, aslc 0x04, tche 0x08. */
	out->asle_ardy = i915_opregion_le32(op + 0x300);
	out->asle_aslc = i915_opregion_le32(op + 0x304);
	out->asle_tche = i915_opregion_le32(op + 0x308);

	/* Records the RVDA and its size, after fdss (u64), fdsp and stat in struct opregion_asle. */
	asle = op + I915_OPREGION_ASLE_OFFSET;
	out->rvda = i915_opregion_le64(asle + 186);
	out->rvds = i915_opregion_le32(asle + 194);

	/* The RVDA is the candidate from version 2 on, when the ASLE mailbox (bit 2) exists and names one. */
	if (out->major >= 2u &&
	    (out->mboxes & 0x4u) != 0u &&
	    out->rvda != 0u &&
	    out->rvds != 0u) {
		/* From version 2.1 the RVDA is an offset from the OpRegion base. */
		out->rvda_relative = 0;
		if (out->major > 2u || out->minor >= 1u)
			out->rvda_relative = 1;

		/* An offset inside the OpRegion is suspect. */
		out->rvda_inside = 0;
		if (out->rvda_relative != 0 && out->rvda < I915_OPREGION_SIZE)
			out->rvda_inside = 1;

		/* Resolves the physical address of the VBT. */
		out->src = I915_OPVBT_RVDA;
		if (out->rvda_relative != 0) {
			out->vbt_phys = asls + out->rvda;
		} else {
			out->vbt_phys = out->rvda;
		}

		/* Succeeded: the VBT is at the RVDA. */
		return 0;
	}

	/* Mailbox #4 reaches the ASLE extension mailbox when that one is in use, else the end of the OpRegion. */
	out->src = I915_OPVBT_MAILBOX4;
	out->vbt_offset = I915_OPREGION_VBT_OFFSET;
	if ((out->mboxes & 0x10u) != 0u) {
		out->vbt_max = I915_OPREGION_ASLE_EXT - I915_OPREGION_VBT_OFFSET;
	} else {
		out->vbt_max = I915_OPREGION_SIZE - I915_OPREGION_VBT_OFFSET;
	}

	/* Succeeded: the VBT is in mailbox #4. */
	return 0;
}

/* Chooses the VBT bytes: the OpRegion's copy when it checks, else the PCI expansion ROM's. */
static void
i915_bios_choose_source(
	struct i915_display *display,
	struct i915_vbt_state *vbt,
	struct i915_pci *pci,
	const void **out,
	size_t *out_size,
	int *origin)
{
	int lifted;
	int valid;

	/* Starts with no bytes. */
	*out = NULL;
	*out_size = 0U;
	*origin = I915_VBT_ORIGIN_NONE;

	/* The OpRegion's copy, when it holds a VBT that checks. */
	if (display->opregion_vbt_buf != NULL) {
		valid = drv_i915_vbt_validate(display->opregion_vbt_buf, display->opregion_vbt_size);
		if (valid != 0) {
			*out = display->opregion_vbt_buf;
			*out_size = display->opregion_vbt_size;
			vbt->source = I915_VBT_SRC_OPREGION;
			*origin = I915_VBT_ORIGIN_OPREGION;
			return;
		}
	}

	/* The PCI expansion ROM, when there is a device to read it from. */
	if (pci == NULL)
		return;
	lifted = i915_bios_oprom_get_vbt(display, pci, out, out_size);
	if (lifted != 0) {
		vbt->source = I915_VBT_SRC_PCI_ROM;
		*origin = I915_VBT_ORIGIN_PCI_ROM;
	}
}

/*
 * Lifts a VBT that checks out of the PCI expansion ROM.
 *
 * Returns 1 with the VBT copied into display->i915_vbt_buf, or 0 on a
 * genuine absence (no ROM BAR, unmappable, or no VBT that checks).  The
 * ROM decode is left as it was found.
 */
static int
i915_bios_oprom_get_vbt(
	struct i915_display *display,
	struct i915_pci *pci,
	const void **out,
	size_t *out_size)
{
	uint32_t original;
	uint32_t base;
	uint32_t probed;
	uint32_t size32;
	size_t rom_size;
	size_t i;
	void *rom;
	const volatile uint8_t *bytes;
	int map_error;
	int found;
	int lifted;

	/* A ROM base address register without an address is a real absence. */
	original = drv_i915_pci_read32(pci, I915_VBT_PCI_ROM_BASE);
	base = original & I915_VBT_ROM_ADDRESS_MASK;
	if (base == 0U)
		return 0;

	/* Sizes the ROM window: writes ones to the address bits, keeps the enable, restores. */
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, I915_VBT_ROM_ADDRESS_MASK | (original & I915_VBT_ROM_ENABLE));
	probed = drv_i915_pci_read32(pci, I915_VBT_PCI_ROM_BASE) & I915_VBT_ROM_ADDRESS_MASK;
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);
	if (probed == 0U)
		return 0;

	/* Bounds the mapping. */
	size32 = (~probed) + 1U;
	rom_size = (size_t)size32;
	if (rom_size > I915_VBT_ROM_MAP_MAX)
		rom_size = I915_VBT_ROM_MAP_MAX;

	/* Enables the ROM decode and maps the ROM. */
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, base | I915_VBT_ROM_ENABLE);
	rom = NULL;
	map_error = kern_device_map(base, rom_size, KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY, &rom);
	if (map_error != 0) {
		drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);
		return 0;
	}

	/* A mapping without an address cannot be read. */
	if (rom == NULL) {
		drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);
		return 0;
	}

	/* Scans the ROM for the "$VBT" signature on 4-byte boundaries. */
	bytes = (const volatile uint8_t *)rom;
	found = -1;
	for (i = 0U; i + 4U < rom_size; i += 4U) {
		if (bytes[i] != '$')
			continue;
		if (bytes[i + 1U] != 'V')
			continue;
		if (bytes[i + 2U] != 'B')
			continue;
		if (bytes[i + 3U] != 'T')
			continue;

		/* The signature's start. */
		found = (int)i;
		break;
	}

	/* Copies and checks the VBT the signature starts. */
	lifted = 0;
	if (found >= 0)
		lifted = i915_bios_oprom_lift(display, bytes, rom_size, (size_t)found, out, out_size);

	/* Unmaps the ROM and leaves its decode as it was found. */
	(void)kern_device_unmap(rom, rom_size);
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);

	/* A signature without a VBT that checks behind it is an absence. */
	if (lifted == 0)
		return 0;

	/* Succeeded: *out is the VBT copy. */
	return 1;
}

/*
 * Copies the VBT a ROM signature starts into display->i915_vbt_buf and
 * checks it; reports 1 with *out and *out_size set, or 0.
 */
static int
i915_bios_oprom_lift(
	struct i915_display *display,
	const volatile uint8_t *rom,
	size_t rom_size,
	size_t found,
	const void **out,
	size_t *out_size)
{
	size_t available;
	uint8_t low;
	uint8_t high;
	uint16_t vbt_size;
	size_t k;
	int valid;

	/* The VBT header must fit in the rest of the ROM. */
	available = rom_size - found;
	if (available < I915_VBT_HEADER_SIZE)
		return 0;

	/* Reads the VBT size from the header. */
	low = rom[found + I915_VBT_OFF_VBT_SIZE];
	high = rom[found + I915_VBT_OFF_VBT_SIZE + 1U];
	vbt_size = (uint16_t)(low | (high << 8));

	/* The VBT must be nonempty, fit in the ROM and fit in the copy buffer. */
	if (vbt_size == 0U)
		return 0;
	if ((size_t)vbt_size > available)
		return 0;
	if ((size_t)vbt_size > I915_VBT_MAX)
		return 0;

	/* Copies the VBT out of the ROM. */
	for (k = 0U; k < (size_t)vbt_size; k++)
		display->i915_vbt_buf[k] = rom[found + k];

	/* Refuses a copy whose header or checksum does not hold. */
	valid = drv_i915_vbt_validate(display->i915_vbt_buf, vbt_size);
	if (valid == 0)
		return 0;

	/* Hands the copy out. */
	*out = display->i915_vbt_buf;
	*out_size = vbt_size;

	/* Succeeded: the copy is a VBT that checks. */
	return 1;
}

/* Puts the xd parse in struct i915_vbt: the outputs, the general features and the version. */
static void
i915_bios_fill(
	struct i915_vbt *parsed,
	const struct i915_xd_vbt *xd_vbt,
	const void *bytes,
	size_t size,
	int origin)
{
	const struct i915_xd_vbt_child *child;
	struct i915_vbt_encoder *encoder;
	unsigned c;

	/* Starts empty, with the bytes and where they came from. */
	kern_memset(parsed, 0, sizeof(*parsed));
	parsed->inited = 1;
	parsed->origin = origin;
	parsed->bytes = bytes;
	parsed->size = size;
	parsed->xd = xd_vbt;

	/* The version, the defaults and the general features. */
	parsed->bdb_version = xd_vbt->version;
	parsed->missing_defaults_used = 0;
	if (!xd_vbt->found)
		parsed->missing_defaults_used = 1;
	parsed->int_lvds_support = xd_vbt->internal_panel;
	parsed->display_clock_mode = xd_vbt->display_clock_mode;
	parsed->lvds_use_ssc = xd_vbt->ssc;
	parsed->lvds_ssc_freq = (int)xd_vbt->ssc_khz;

	/* The outputs, in the VBT's order; one the platform has no DDI for has no port. */
	for (c = 0U; c < xd_vbt->nchildren && parsed->n_encoders < I915_VBT_MAX_ENCODERS; c++) {
		child = &xd_vbt->children[c];
		encoder = &parsed->enc[parsed->n_encoders++];
		encoder->port = -1;
		if (child->ddi != I915_XD_VBT_NONE)
			encoder->port = (int)child->ddi;
		i915_bios_output_set(encoder, child);
	}
}

/* Puts one xd output's values in an older output. */
static void
i915_bios_output_set(
	struct i915_vbt_encoder *encoder,
	const struct i915_xd_vbt_child *child)
{
	/* The AUX channel: none is -1 to the older code. */
	encoder->aux_ch = -1;
	if (child->aux_ch != I915_XD_VBT_NONE)
		encoder->aux_ch = (int)child->aux_ch;

	/* What the output is and how it is wired. */
	encoder->device_type = child->device_type;
	encoder->dvo_port = child->dvo_port;
	encoder->ddc_pin_raw = child->ddc_code;
	encoder->ddc_pin = child->gmbus_pin;
	encoder->supports_dp = child->dp;
	encoder->supports_edp = child->edp;
	encoder->supports_hdmi = child->hdmi;
	encoder->supports_dvi = child->dvi;
	encoder->supports_typec_usb = child->type_c;
	encoder->supports_tbt = child->tbt;
	encoder->lane_reversal = child->lane_reversal;
	encoder->hpd_invert = child->hpd_invert;

	/* Its link limits and boosts. */
	encoder->dp_max_lane_count = child->dp_max_lanes;
	encoder->dp_max_link_rate = (int)child->dp_max_rate_khz;
	encoder->dp_boost_level = child->dp_boost;
	encoder->hdmi_boost_level = child->hdmi_boost;
	encoder->hdmi_level_shift = child->hdmi_level_shift;
}

/* Puts one xd panel's values in the older panel type. */
static void
i915_bios_panel_set(
	struct i915_vbt_panel *out,
	const struct i915_xd_vbt *vbt,
	const struct i915_xd_vbt_panel *panel)
{
	/* The panel row and the eDP link; the older code keeps the pre-emphasis three bits up. */
	out->panel_type = (int)panel->panel_type;
	out->bpp = panel->bpp;
	out->edp_rate = (int)panel->edp_rate_khz;
	out->edp_lanes = panel->edp_lanes;
	out->edp_preemphasis = (int)panel->edp_preemphasis << 3;
	out->edp_vswing = panel->edp_vswing;
	out->edp_low_vswing = panel->low_vswing;
	out->edp_hobl = panel->hobl;
	out->edp_max_link_rate = (int)panel->edp_max_rate_khz;
	out->override_afc_startup = vbt->afc_override;

	/* The power sequence. */
	out->t1_t3 = panel->t1_t3;
	out->t8 = panel->t8;
	out->t9 = panel->t9;
	out->t10 = panel->t10;
	out->t11_t12 = panel->t11_t12;

	/* The backlight, the refresh rate switching and VRR. */
	out->bl_present = panel->backlight;
	out->bl_type = panel->backlight_type;
	out->bl_controller = panel->backlight_controller;
	out->bl_active_low_pwm = panel->backlight_active_low;
	out->bl_pwm_freq_hz = panel->backlight_hz;
	out->bl_min_brightness = panel->backlight_min;
	out->drrs_type = (int)panel->drrs;
	out->vrr = panel->vrr;

	/* The VBT's own mode. */
	out->has_lfp_mode = panel->has_mode;
	out->mode_clock_khz = (int)panel->mode.clock_khz;
	out->hdisplay = (uint16_t)panel->mode.hdisplay;
	out->hsync_start = (uint16_t)panel->mode.hsync_start;
	out->hsync_end = (uint16_t)panel->mode.hsync_end;
	out->htotal = (uint16_t)panel->mode.htotal;
	out->vdisplay = (uint16_t)panel->mode.vdisplay;
	out->vsync_start = (uint16_t)panel->mode.vsync_start;
	out->vsync_end = (uint16_t)panel->mode.vsync_end;
	out->vtotal = (uint16_t)panel->mode.vtotal;
}

/* Records the VBT stage in the trace and logs the source, the parse and every output. */
static void
i915_bios_report(
	struct i915_vbt_state *vbt,
	struct i915_trace *trace)
{
	const struct i915_vbt_encoder *encoder;
	char port_letter;
	unsigned i;

	/* Records whether a VBT was parsed or is absent. */
	if (!vbt->vbt_found) {
		drv_i915_trace_record(trace, I915_VBT_TRACE_STAGE, I915_TRACE_NOTE, "vbt:vbt_absent", (uint64_t)(unsigned)vbt->source, 0U);
	} else {
		drv_i915_trace_record(trace, I915_VBT_TRACE_STAGE, I915_TRACE_ACQUIRE, "vbt:vbt_parsed", (uint64_t)vbt->version, 0U);
	}

	/* Logs the source and the parse. */
	kern_logf("i915: VBT: source=%d vbt_found=%d version=%u child_devices=%u missing_defaults=%d\n",
	    vbt->source,
	    vbt->vbt_found,
	    (unsigned)vbt->version,
	    vbt->num_display_devices,
	    vbt->missing_defaults_used);

	/* Logs every output. */
	for (i = 0U; i < vbt->parsed.n_encoders; i++) {
		encoder = &vbt->parsed.enc[i];
		port_letter = '-';
		if (encoder->port >= 0)
			port_letter = (char)('A' + encoder->port);
		kern_logf("i915: VBT child[%u]: port=%c dvo_port=%u type=0x%04x aux_ch=%d ddc_pin=%d dp=%d edp=%d hdmi=%d typec=%d tbt=%d max_lanes=%d max_rate=%d hpd_invert=%d lane_reversal=%d\n",
		    i,
		    port_letter,
		    encoder->dvo_port,
		    encoder->device_type,
		    encoder->aux_ch,
		    encoder->ddc_pin,
		    encoder->supports_dp,
		    encoder->supports_edp,
		    encoder->supports_hdmi,
		    encoder->supports_typec_usb,
		    encoder->supports_tbt,
		    encoder->dp_max_lane_count,
		    encoder->dp_max_link_rate,
		    encoder->hpd_invert,
		    encoder->lane_reversal);
	}
}

/* Reads a little-endian 32-bit value of the OpRegion. */
static uint32_t
i915_opregion_le32(
	const uint8_t *bytes)
{
	/* Succeeded: reports the four bytes, low first. */
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/* Reads a little-endian 64-bit value of the OpRegion. */
static uint64_t
i915_opregion_le64(
	const uint8_t *bytes)
{
	uint64_t low;
	uint64_t high;

	/* Reads the two halves. */
	low = i915_opregion_le32(bytes);
	high = i915_opregion_le32(bytes + 4);

	/* Succeeded: reports the low half joined with the high one. */
	return low | high << 32;
}

