/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's description of the display for the display 12 and 13
 * code (see xd-vbt.h): the place of the VBT in the OpRegion, the check
 * of a VBT, the outputs it lists, the built-in panel it describes, and
 * the SHA-256 digest the start log reports the VBT by.
 */

#include "display-internal.h"
#include "xd-edid.h"
#include "xd-platform.h"
#include "xd-vbt.h"

#include "../intel/xd-opregion-layout.h"
#include "../intel/xd-vbt-layout.h"

#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* The DDIs a VBT-less board gets a default child for: A..F. */
#define I915_XD_VBT_DEFAULT_DDIS	6U

/* The DDI a VBT-less board has its panel on, and the one it has no outputs on. */
#define I915_XD_VBT_DEFAULT_PANEL_DDI	0U
#define I915_XD_VBT_DEFAULT_EMPTY_DDI	4U

/* The BDB version a VBT-less board is read as. */
#define I915_XD_VBT_DEFAULT_VERSION	155U

/* The backlight type a VBT without a control method names: the display's own PWM. */
#define I915_XD_VBT_BACKLIGHT_DISPLAY	2U

/* The precision a brightness word of 16 bits has. */
#define I915_XD_VBT_PRECISION_16	16U

/* The largest brightness value of the old 8-bit scale. */
#define I915_XD_VBT_BRIGHTNESS_8BIT	255U

/* The size of one SHA-256 message block and of its length field. */
#define I915_XD_SHA256_BLOCK		64U
#define I915_XD_SHA256_LENGTH_BYTES	8U

/*
 * One block of the BDB: its data (after the three header bytes), its
 * size, and where its data starts counted from the start of the BDB.
 * found is zero when the BDB has no such block.
 */
struct i915_xd_vbt_block {
	const uint8_t *data;
	size_t size;
	uint32_t offset;
	int found;
};

/*
 * The panel data tables of the sixteen panels, where the VBT's pointers
 * (or the pointers made up for a VBT without them) say they are inside
 * the panel data block: the timing table, the DVO timing and the PnP id
 * of each panel.  valid is zero when the pointers do not describe the
 * block.
 */
struct i915_xd_vbt_tables {
	int valid;
	struct i915_xd_vbt_block data;
	uint32_t fp[I915_XD_VBT_PANEL_TYPES];
	uint32_t dvo[I915_XD_VBT_PANEL_TYPES];
	uint32_t pnp[I915_XD_VBT_PANEL_TYPES];
	uint32_t fp_size;
	uint32_t name_offset;
	uint32_t name_size;
};

/*
 * The running state of one SHA-256 digest: the eight hash words, the
 * message bytes not yet hashed, and the length so far.
 */
struct i915_xd_sha256 {
	uint32_t hash[8];
	uint8_t block[I915_XD_SHA256_BLOCK];
	unsigned used;
	uint64_t length;
};

/*
 * The SHA-256 round constants and the initial hash words.
 *
 * Values referenced: NIST FIPS 180-4 (Secure Hash Standard), section
 * 4.2.2 (SHA-256 constants) and section 5.3.3 (SHA-256 initial hash
 * value).
 */
static const uint32_t i915_xd_sha256_k[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
	0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
	0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
	0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
	0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
	0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
	0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static const uint32_t i915_xd_sha256_initial[8] = {
	0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

static int i915_xd_opregion_copy(const struct i915_xd_opregion_access *access, uint64_t address, size_t size, uint8_t *copy);
static uint32_t i915_xd_vbt_u8(const uint8_t *bytes, size_t size, size_t offset);
static uint32_t i915_xd_vbt_u16(const uint8_t *bytes, size_t size, size_t offset);
static uint32_t i915_xd_vbt_u32(const uint8_t *bytes, size_t size, size_t offset);
static uint64_t i915_xd_vbt_u64(const uint8_t *bytes, size_t size, size_t offset);
static uint32_t i915_xd_block_u8(const struct i915_xd_vbt_block *block, size_t offset);
static uint32_t i915_xd_block_u16(const struct i915_xd_vbt_block *block, size_t offset);
static uint32_t i915_xd_block_u32(const struct i915_xd_vbt_block *block, size_t offset);
static uint64_t i915_xd_block_u64(const struct i915_xd_vbt_block *block, size_t offset);
static void i915_xd_vbt_find_block(const struct i915_xd_vbt *vbt, unsigned id, struct i915_xd_vbt_block *block);
static void i915_xd_vbt_start(const struct i915_xd_platform *platform, struct i915_xd_vbt *vbt);
static void i915_xd_vbt_features(struct i915_xd_vbt *vbt);
static void i915_xd_vbt_driver(struct i915_xd_vbt *vbt);
static void i915_xd_vbt_children(struct i915_xd_vbt *vbt);
static void i915_xd_vbt_defaults(struct i915_xd_vbt *vbt);
static void i915_xd_vbt_fill_child(const struct i915_xd_vbt *vbt, const struct i915_xd_vbt_block *bytes, struct i915_xd_vbt_child *child);
static uint8_t i915_xd_vbt_dvo_ddi(const struct i915_xd_platform *platform, unsigned code);
static uint8_t i915_xd_vbt_ddc_pin(const struct i915_xd_platform *platform, unsigned code);
static uint8_t i915_xd_vbt_aux_ch(const struct i915_xd_platform *platform, unsigned code);
static uint32_t i915_xd_vbt_dp_rate(unsigned version, unsigned code);
static uint32_t i915_xd_vbt_tmds_rate(unsigned code);
static uint8_t i915_xd_vbt_boost(unsigned code);
static void i915_xd_vbt_tables(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_tables *tables);
static int i915_xd_vbt_pointers(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_tables *tables);
static int i915_xd_vbt_made_pointers(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_tables *tables);
static int i915_xd_vbt_tables_fit(struct i915_xd_vbt_tables *tables);
static int i915_xd_vbt_panel_type(const struct i915_xd_vbt *vbt, const struct i915_xd_vbt_child *child, const struct i915_xd_vbt_tables *tables, const uint8_t *edid, size_t edid_size, struct i915_xd_vbt_panel *panel);
static int i915_xd_vbt_own_panel_type(const struct i915_xd_vbt *vbt, const struct i915_xd_vbt_child *child);
static int i915_xd_vbt_pnp_panel_type(const struct i915_xd_vbt_tables *tables, const uint8_t *edid, size_t edid_size);
static void i915_xd_vbt_panel_options(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_panel *panel);
static void i915_xd_vbt_generic_dtd(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_panel *panel);
static void i915_xd_vbt_lfp_dtd(const struct i915_xd_vbt_tables *tables, struct i915_xd_vbt_panel *panel);
static void i915_xd_vbt_backlight(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_panel *panel);
static void i915_xd_vbt_driver_drrs(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_panel *panel);
static void i915_xd_vbt_power(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_panel *panel);
static void i915_xd_vbt_edp(const struct i915_xd_vbt *vbt, struct i915_xd_vbt_panel *panel);
static unsigned i915_xd_vbt_panel_bits(uint64_t word, unsigned panel_type, unsigned width);
static void i915_xd_sha256_start(struct i915_xd_sha256 *sha);
static void i915_xd_sha256_add(struct i915_xd_sha256 *sha, const uint8_t *data, size_t size);
static void i915_xd_sha256_finish(struct i915_xd_sha256 *sha, uint8_t digest[32]);
static void i915_xd_sha256_block(struct i915_xd_sha256 *sha);
static uint32_t i915_xd_sha256_rotr(uint32_t word, unsigned count);

/*
 * Finds where an OpRegion keeps its VBT.
 *
 * opregion is a copy (or mapping) of size bytes of the OpRegion found at
 * the physical address address.  Returns 0 with where filled, or EINVAL
 * for a copy that is too short or is no OpRegion.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_opregion.c
 * (intel_opregion_setup: the raw VBT and mailbox 4 choice).
 */
int
drv_i915_xd_opregion_find_vbt(
	const uint8_t *opregion,
	size_t size,
	uint64_t address,
	struct i915_xd_opregion_vbt *where)
{
	static const char signature[I915_XD_OPREGION_SIGNATURE_BYTES] = {
		'I', 'n', 't', 'e', 'l', 'G', 'r', 'a', 'p', 'h', 'i', 'c', 's', 'M', 'e', 'm'
	};
	uint64_t raw_address;
	uint32_t raw_size;
	uint32_t end;
	int compared;

	/* Refuses a copy that cannot hold the OpRegion. */
	if (opregion == NULL)
		return EINVAL;
	if (size < I915_XD_OPREGION_BYTES)
		return EINVAL;

	/* Refuses a copy without the OpRegion signature. */
	compared = kern_memcmp(opregion + I915_XD_OPREGION_SIGNATURE, signature, sizeof(signature));
	if (compared != 0)
		return EINVAL;

	/* Reads the version and the mailboxes the OpRegion has. */
	kern_memset(where, 0, sizeof(*where));
	where->major = opregion[I915_XD_OPREGION_MAJOR];
	where->minor = opregion[I915_XD_OPREGION_MINOR];
	where->mailboxes = i915_xd_vbt_u32(opregion, size, I915_XD_OPREGION_MAILBOXES);

	/*
	 * Mailbox 4 holds the VBT up to the ASLE extension when the OpRegion
	 * uses that mailbox, and up to the OpRegion's end otherwise.
	 */
	end = I915_XD_OPREGION_BYTES;
	if ((where->mailboxes & I915_XD_OPREGION_MBOX_ASLE_EXT) != 0U)
		end = I915_XD_OPREGION_ASLE_EXT;
	where->mailbox_offset = I915_XD_OPREGION_VBT;
	where->mailbox_size = end - I915_XD_OPREGION_VBT;

	/* An OpRegion before version 2.0, or without the ASLE mailbox, has no raw VBT. */
	if (where->major < I915_XD_OPREGION_RVDA_MAJOR)
		return 0;
	if ((where->mailboxes & I915_XD_OPREGION_MBOX_ASLE) == 0U)
		return 0;

	/* Reads the raw VBT's address and size; either of them zero means there is none. */
	raw_address = i915_xd_vbt_u64(opregion, size, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDA);
	raw_size = i915_xd_vbt_u32(opregion, size, I915_XD_OPREGION_ASLE + I915_XD_OPREGION_ASLE_RVDS);
	if (raw_address == 0U)
		return 0;
	if (raw_size == 0U)
		return 0;

	/* From version 2.1 the address counts from the OpRegion's own address. */
	if (where->major > I915_XD_OPREGION_RVDA_MAJOR) {
		raw_address += address;
	} else if (where->minor >= I915_XD_OPREGION_RVDA_RELATIVE_MINOR) {
		raw_address += address;
	}

	/* Records the raw VBT as the first place to try. */
	where->raw_address = raw_address;
	where->raw_size = raw_size;

	/* Succeeded: the raw VBT and mailbox 4 are found. */
	return 0;
}

/*
 * Reads the OpRegion at a physical address and copies the VBT it keeps.
 *
 * The 8 KiB OpRegion is copied to opregion_copy.  The raw VBT is tried
 * first, as the OpRegion names it; one that lies inside the OpRegion
 * itself, is larger than vbt_room, cannot be mapped or does not check is
 * passed over, and the VBT in mailbox 4 is tried next.  The VBT taken is
 * copied to vbt_copy and hashed.  Returns 0 (also when ASLS is 0: there
 * is no OpRegion), EIO when the OpRegion cannot be mapped, EINVAL when it
 * has no OpRegion signature, or ENOENT when neither place holds a VBT
 * that checks.
 *
 * Procedure referenced: Linux v6.8.12
 * drivers/gpu/drm/i915/display/intel_opregion.c (intel_opregion_setup).
 */
int
drv_i915_xd_opregion_read(
	const struct i915_xd_opregion_access *access,
	uint32_t asls,
	uint8_t *opregion_copy,
	uint8_t *vbt_copy,
	size_t vbt_room,
	struct i915_xd_opregion_read *read)
{
	uint64_t opregion_end;
	uint64_t raw_end;
	int checked;
	int error;

	/* Starts with nothing found. */
	kern_memset(read, 0, sizeof(*read));
	read->source = I915_XD_VBT_FROM_NONE;
	read->raw_status = I915_XD_OPREGION_RAW_ABSENT;

	/* ASLS 0 means the firmware has no OpRegion. */
	if (asls == 0U)
		return 0;

	/* There is an OpRegion; the rest says whether it could be read. */
	read->present = 1;

	/* Copies the OpRegion. */
	error = i915_xd_opregion_copy(access, asls, I915_XD_OPREGION_BYTES, opregion_copy);
	if (error != 0)
		return EIO;
	read->mapped = 1;

	/* Finds where it keeps its VBT. */
	error = drv_i915_xd_opregion_find_vbt(opregion_copy, I915_XD_OPREGION_BYTES, asls, &read->where);
	if (error != 0)
		return EINVAL;
	read->found = 1;

	/* Decides whether the raw VBT can be taken. */
	opregion_end = (uint64_t)asls + I915_XD_OPREGION_BYTES;
	raw_end = read->where.raw_address + read->where.raw_size;
	if (read->where.raw_size == 0U) {
		/* The OpRegion names no raw VBT. */
		read->raw_status = I915_XD_OPREGION_RAW_ABSENT;
	} else if (read->where.raw_address < opregion_end && raw_end > asls) {
		/* A raw VBT overlapping the OpRegion is a firmware error. */
		read->raw_status = I915_XD_OPREGION_RAW_INSIDE;
	} else if (read->where.raw_size > vbt_room) {
		/* A raw VBT larger than the copy is not taken. */
		read->raw_status = I915_XD_OPREGION_RAW_TOO_LARGE;
	} else {
		/* Copies the raw VBT and checks it. */
		read->raw_status = I915_XD_OPREGION_RAW_UNMAPPED;
		error = i915_xd_opregion_copy(access, read->where.raw_address, read->where.raw_size, vbt_copy);
		if (error == 0) {
			read->raw_status = I915_XD_OPREGION_RAW_UNCHECKED;
			checked = drv_i915_xd_vbt_check(vbt_copy, read->where.raw_size);
			if (checked == 0)
				read->raw_status = I915_XD_OPREGION_RAW_USED;
		}
	}

	/* Takes the raw VBT when it checked. */
	if (read->raw_status == I915_XD_OPREGION_RAW_USED) {
		read->source = I915_XD_VBT_FROM_RAW;
		read->vbt_size = read->where.raw_size;
	} else {
		/* Copies mailbox 4 out of the OpRegion copy and checks it. */
		if (read->where.mailbox_size > vbt_room)
			return ENOENT;
		kern_memcpy(vbt_copy, opregion_copy + read->where.mailbox_offset, read->where.mailbox_size);
		checked = drv_i915_xd_vbt_check(vbt_copy, read->where.mailbox_size);
		if (checked != 0)
			return ENOENT;
		read->source = I915_XD_VBT_FROM_MAILBOX;
		read->vbt_size = read->where.mailbox_size;
	}

	/* Hashes the VBT taken. */
	read->vbt_valid = 1;
	drv_i915_xd_sha256(vbt_copy, read->vbt_size, read->vbt_sha256);

	/* Succeeded: the VBT is in vbt_copy. */
	return 0;
}

/*
 * Checks that a buffer holds a VBT the parse can read: the header is
 * whole and signed "$VBT", the VBT fits the buffer, and the BDB header
 * and the whole BDB fit the VBT.  Returns 0 or EINVAL.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (intel_bios_is_valid_vbt).
 */
int
drv_i915_xd_vbt_check(
	const uint8_t *bytes,
	size_t size)
{
	static const char signature[I915_XD_VBT_SIGNATURE_CHECKED] = { '$', 'V', 'B', 'T' };
	uint32_t vbt_size;
	uint32_t bdb_offset;
	uint32_t bdb_size;
	int compared;

	/* Refuses a buffer without a whole VBT header. */
	if (bytes == NULL)
		return EINVAL;
	if (size < I915_XD_VBT_HEADER_BYTES)
		return EINVAL;

	/* Refuses a header that is not a VBT's. */
	compared = kern_memcmp(bytes + I915_XD_VBT_SIGNATURE, signature, sizeof(signature));
	if (compared != 0)
		return EINVAL;

	/* Refuses a VBT longer than the buffer; the VBT's own size bounds everything after. */
	vbt_size = i915_xd_vbt_u16(bytes, size, I915_XD_VBT_VBT_SIZE);
	if (vbt_size > size)
		return EINVAL;

	/* Refuses a BDB header that starts or ends outside the VBT. */
	bdb_offset = i915_xd_vbt_u32(bytes, size, I915_XD_VBT_BDB_OFFSET);
	if (bdb_offset > vbt_size)
		return EINVAL;
	if (I915_XD_BDB_HEADER_BYTES > vbt_size - bdb_offset)
		return EINVAL;

	/* Refuses a BDB that runs past the VBT. */
	bdb_size = i915_xd_vbt_u16(bytes, size, bdb_offset + I915_XD_BDB_SIZE);
	if (bdb_size > vbt_size - bdb_offset)
		return EINVAL;

	/* Succeeded: the VBT can be parsed. */
	return 0;
}

/*
 * Reads what a VBT says about the board.
 *
 * bytes may be NULL when no VBT was found; the board then gets the
 * defaults.  A VBT that does not check gets the defaults too, and the
 * parse reports EINVAL so the caller can say why.  The bytes must stay
 * mapped while the panel is read (drv_i915_xd_vbt_panel()).  Returns 0
 * or EINVAL.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (intel_bios_init, init_vbt_defaults).
 */
int
drv_i915_xd_vbt_parse(
	const struct i915_xd_platform *platform,
	const uint8_t *bytes,
	size_t size,
	struct i915_xd_vbt *vbt)
{
	uint32_t bdb_offset;
	int error;

	/* Starts from what every board has before its VBT is read. */
	i915_xd_vbt_start(platform, vbt);

	/* A board without a VBT gets the default outputs. */
	if (bytes == NULL) {
		i915_xd_vbt_defaults(vbt);
		return 0;
	}

	/* A VBT that does not check is not read at all; the board gets the defaults. */
	error = drv_i915_xd_vbt_check(bytes, size);
	if (error != 0) {
		i915_xd_vbt_defaults(vbt);
		return error;
	}

	/* Records the VBT, its BDB and the BDB version the field rules go by. */
	bdb_offset = i915_xd_vbt_u32(bytes, size, I915_XD_VBT_BDB_OFFSET);
	vbt->found = 1;
	vbt->bytes = bytes;
	vbt->size = size;
	vbt->bdb_offset = bdb_offset;
	vbt->bdb_size = i915_xd_vbt_u16(bytes, size, bdb_offset + I915_XD_BDB_SIZE);
	vbt->version = (uint16_t)i915_xd_vbt_u16(bytes, size, bdb_offset + I915_XD_BDB_VERSION);

	/* Reads the general features, the outputs and the driver features. */
	i915_xd_vbt_features(vbt);
	i915_xd_vbt_children(vbt);
	i915_xd_vbt_driver(vbt);

	/* Succeeded: the VBT is read. */
	return 0;
}

/*
 * Reports the output the VBT lists first on a DDI, or NULL when it lists
 * none there.
 */
const struct i915_xd_vbt_child *
drv_i915_xd_vbt_child(
	const struct i915_xd_vbt *vbt,
	unsigned ddi)
{
	unsigned index;

	/* Looks at each output in the VBT's order. */
	for (index = 0U; index < vbt->nchildren; index++) {
		if (vbt->children[index].ddi == ddi)
			return &vbt->children[index];
	}

	/* No output of the VBT is on the DDI. */
	return NULL;
}

/*
 * Reads what the VBT says about the built-in panel of an output.
 *
 * The panel's row in the VBT tables is the VBT's own choice; when the
 * VBT leaves it to the driver, the row whose PnP id matches the panel's
 * EDID (edid may be NULL when none was read); otherwise the first row.
 * The VBT the board's parse read must still be mapped.  Returns 0 with
 * panel filled, or EINVAL for a missing output.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (intel_bios_init_panel, init_vbt_panel_defaults, get_panel_type).
 */
int
drv_i915_xd_vbt_panel(
	const struct i915_xd_vbt *vbt,
	const struct i915_xd_vbt_child *child,
	const uint8_t *edid,
	size_t edid_size,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_tables tables;
	int error;

	/* Refuses a panel without its output. */
	if (child == NULL)
		return EINVAL;

	/* Starts the panel with a backlight and dithering, as a panel without VBT data has. */
	kern_memset(panel, 0, sizeof(*panel));
	panel->backlight = 1;
	panel->dither = 1;

	/* Finds the panel tables, and the panel's row in them. */
	i915_xd_vbt_tables(vbt, &tables);
	error = i915_xd_vbt_panel_type(vbt, child, &tables, edid, edid_size, panel);
	if (error != 0)
		return error;

	/* Reads the panel options, its mode, its backlight and its features. */
	i915_xd_vbt_panel_options(vbt, panel);
	i915_xd_vbt_generic_dtd(vbt, panel);
	i915_xd_vbt_lfp_dtd(&tables, panel);
	i915_xd_vbt_backlight(vbt, panel);
	i915_xd_vbt_driver_drrs(vbt, panel);
	i915_xd_vbt_power(vbt, panel);

	/* Reads the eDP link and power sequence. */
	i915_xd_vbt_edp(vbt, panel);

	/* Succeeded: the panel is read. */
	return 0;
}

/*
 * Computes the SHA-256 digest of a byte string, which the start log
 * names the firmware tables by.
 *
 * Procedure referenced: NIST FIPS 180-4 (Secure Hash Standard), sections
 * 5.1.1 (padding) and 6.2.2 (SHA-256 hash computation).
 */
void
drv_i915_xd_sha256(
	const uint8_t *data,
	size_t size,
	uint8_t digest[32])
{
	struct i915_xd_sha256 sha;

	/* Hashes the string and writes out the digest. */
	i915_xd_sha256_start(&sha);
	i915_xd_sha256_add(&sha, data, size);
	i915_xd_sha256_finish(&sha, digest);
}

/* Reads one byte, 0 past the end of the bytes. */
static uint32_t
i915_xd_vbt_u8(
	const uint8_t *bytes,
	size_t size,
	size_t offset)
{
	/* A byte past the end reads as zero, as the firmware's padding does. */
	if (offset >= size)
		return 0U;

	/* Succeeded: the byte. */
	return bytes[offset];
}

/* Reads a little-endian 16-bit word, its bytes past the end as 0. */
static uint32_t
i915_xd_vbt_u16(
	const uint8_t *bytes,
	size_t size,
	size_t offset)
{
	uint32_t low;
	uint32_t high;

	/* Reads the two bytes, low first. */
	low = i915_xd_vbt_u8(bytes, size, offset);
	high = i915_xd_vbt_u8(bytes, size, offset + 1U);

	/* Succeeded: the word. */
	return low | (high << 8);
}

/* Reads a little-endian 32-bit word, its bytes past the end as 0. */
static uint32_t
i915_xd_vbt_u32(
	const uint8_t *bytes,
	size_t size,
	size_t offset)
{
	uint32_t low;
	uint32_t high;

	/* Reads the two halves, low first. */
	low = i915_xd_vbt_u16(bytes, size, offset);
	high = i915_xd_vbt_u16(bytes, size, offset + 2U);

	/* Succeeded: the word. */
	return low | (high << 16);
}

/* Reads a little-endian 64-bit word, its bytes past the end as 0. */
static uint64_t
i915_xd_vbt_u64(
	const uint8_t *bytes,
	size_t size,
	size_t offset)
{
	uint64_t low;
	uint64_t high;

	/* Reads the two halves, low first. */
	low = i915_xd_vbt_u32(bytes, size, offset);
	high = i915_xd_vbt_u32(bytes, size, offset + 4U);

	/* Succeeded: the word. */
	return low | (high << 32);
}

/* Reads one byte of a block, 0 past its end (or for a block the BDB lacks). */
static uint32_t
i915_xd_block_u8(
	const struct i915_xd_vbt_block *block,
	size_t offset)
{
	uint32_t value;

	/* Reads within the block only. */
	value = i915_xd_vbt_u8(block->data, block->size, offset);

	/* Succeeded: the byte. */
	return value;
}

/* Reads a 16-bit word of a block. */
static uint32_t
i915_xd_block_u16(
	const struct i915_xd_vbt_block *block,
	size_t offset)
{
	uint32_t value;

	/* Reads within the block only. */
	value = i915_xd_vbt_u16(block->data, block->size, offset);

	/* Succeeded: the word. */
	return value;
}

/* Reads a 32-bit word of a block. */
static uint32_t
i915_xd_block_u32(
	const struct i915_xd_vbt_block *block,
	size_t offset)
{
	uint32_t value;

	/* Reads within the block only. */
	value = i915_xd_vbt_u32(block->data, block->size, offset);

	/* Succeeded: the word. */
	return value;
}

/* Reads a 64-bit word of a block. */
static uint64_t
i915_xd_block_u64(
	const struct i915_xd_vbt_block *block,
	size_t offset)
{
	uint64_t value;

	/* Reads within the block only. */
	value = i915_xd_vbt_u64(block->data, block->size, offset);

	/* Succeeded: the word. */
	return value;
}

/*
 * Finds a block of the BDB by its id.  The walk stops at a block that
 * runs past the BDB, which hides that block and every later one.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (find_raw_section, _get_blocksize).
 */
static void
i915_xd_vbt_find_block(
	const struct i915_xd_vbt *vbt,
	unsigned id,
	struct i915_xd_vbt_block *block)
{
	const uint8_t *bdb;
	uint32_t total;
	uint32_t index;
	uint32_t block_id;
	uint32_t block_size;
	uint32_t mipi_version;

	/* Starts from no block. */
	block->data = NULL;
	block->size = 0U;
	block->offset = 0U;
	block->found = 0;

	/* A board without a VBT has no blocks. */
	if (vbt->found == 0)
		return;

	/* Walks the blocks, which start after the BDB header. */
	bdb = vbt->bytes + vbt->bdb_offset;
	total = vbt->bdb_size;
	index = i915_xd_vbt_u16(bdb, total, I915_XD_BDB_HEADER_SIZE);
	while (index + I915_XD_BLOCK_HEADER_BYTES < total) {
		/* Reads the block's id and size; the MIPI sequence block from version 3 keeps its size apart. */
		block_id = i915_xd_vbt_u8(bdb, total, index + I915_XD_BLOCK_ID);
		block_size = i915_xd_vbt_u16(bdb, total, index + I915_XD_BLOCK_SIZE);
		mipi_version = i915_xd_vbt_u8(bdb, total, index + I915_XD_BLOCK_MIPI_VERSION);
		if (block_id == I915_XD_BLOCK_MIPI_SEQUENCE && mipi_version >= I915_XD_BLOCK_MIPI_LONG_VERSION)
			block_size = i915_xd_vbt_u32(bdb, total, index + I915_XD_BLOCK_MIPI_SIZE);
		index += I915_XD_BLOCK_HEADER_BYTES;

		/* A block that runs past the BDB ends the walk. */
		if (block_size > total - index)
			return;

		/* Takes the block that has the id. */
		if (block_id == id) {
			block->data = bdb + index;
			block->size = block_size;
			block->offset = index;
			block->found = 1;
			return;
		}

		/* Steps over the block. */
		index += block_size;
	}
}

/*
 * Starts the reading of a board: no VBT yet, the panel clock spread at
 * 120 MHz, and an internal panel assumed.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (init_vbt_defaults).
 */
static void
i915_xd_vbt_start(
	const struct i915_xd_platform *platform,
	struct i915_xd_vbt *vbt)
{
	/* Clears every field, then sets what a board has before its VBT says otherwise. */
	kern_memset(vbt, 0, sizeof(*vbt));
	vbt->platform = platform;
	vbt->ssc = 1;
	vbt->ssc_khz = I915_XD_VBT_SSC_KHZ;
	vbt->internal_panel = 1;
}

/*
 * Reads the general features: the panel clock spread spectrum and its
 * frequency, the display clock mode, and the AFC startup override.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_general_features).
 */
static void
i915_xd_vbt_features(
	struct i915_xd_vbt *vbt)
{
	struct i915_xd_vbt_block block;
	uint32_t bits;
	uint32_t afc;

	/* Without the block the defaults stand. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_GENERAL_FEATURES, &block);
	if (block.found == 0)
		return;

	/* The spread spectrum switch and frequency, and the display clock mode. */
	bits = i915_xd_block_u8(&block, I915_XD_VBT_FEATURES_BITS2);
	vbt->ssc = 0;
	if ((bits & I915_XD_VBT_FEATURES_SSC) != 0U)
		vbt->ssc = 1;
	vbt->ssc_khz = I915_XD_VBT_SSC_KHZ;
	if ((bits & I915_XD_VBT_FEATURES_SSC_ALTERNATE) != 0U)
		vbt->ssc_khz = I915_XD_VBT_SSC_ALTERNATE_KHZ;
	vbt->display_clock_mode = 0;
	if ((bits & I915_XD_VBT_FEATURES_CLOCK_MODE) != 0U)
		vbt->display_clock_mode = 1;

	/* An older VBT has no AFC startup override. */
	if (vbt->version < I915_XD_BDB_AFC_STARTUP)
		return;

	/* A nonzero code overrides the AFC startup: code 1 with 0, the others with 7. */
	afc = i915_xd_block_u8(&block, I915_XD_VBT_FEATURES_BITS7) & I915_XD_VBT_FEATURES_AFC_STARTUP;
	if (afc == 0U)
		return;
	vbt->afc_override = 1;
	vbt->afc_value = I915_XD_VBT_AFC_SEVEN;
	if (afc == I915_XD_VBT_AFC_CODE_ZERO)
		vbt->afc_value = I915_XD_VBT_AFC_ZERO;
}

/*
 * Reads the driver features: a panel configuration other than the
 * internal panel says the board has none.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_driver_features).
 */
static void
i915_xd_vbt_driver(
	struct i915_xd_vbt *vbt)
{
	struct i915_xd_vbt_block block;
	uint32_t bits;
	uint32_t config;

	/* Without the block the board keeps its internal panel. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_DRIVER_FEATURES, &block);
	if (block.found == 0)
		return;

	/* Reads the panel configuration out of the extended bits. */
	bits = i915_xd_block_u16(&block, I915_XD_VBT_DRIVER_EXT_BITS);
	config = (bits & I915_XD_VBT_DRIVER_LFP_CONFIG) >> __builtin_ctz(I915_XD_VBT_DRIVER_LFP_CONFIG);

	/* Any configuration but the internal panel means the board has none. */
	if (config != I915_XD_VBT_DRIVER_LFP_INTERNAL)
		vbt->internal_panel = 0;
}

/*
 * Reads the outputs the general definitions list.  A child of a size
 * the VBT version does not expect is still read for the fields it has;
 * one smaller than the oldest layout ends the list, and a child with no
 * device type is an empty slot.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_general_definitions, child_device_ptr).
 */
static void
i915_xd_vbt_children(
	struct i915_xd_vbt *vbt)
{
	struct i915_xd_vbt_block block;
	struct i915_xd_vbt_block bytes;
	uint32_t child_size;
	uint32_t count;
	uint32_t index;
	uint32_t type;

	/* A VBT without the block lists no outputs. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_GENERAL_DEFINITIONS, &block);
	if (block.found == 0)
		return;
	if (block.size < I915_XD_VBT_DEFS_CHILDREN)
		return;

	/* Children smaller than the oldest layout cannot be read. */
	child_size = i915_xd_block_u8(&block, I915_XD_VBT_DEFS_CHILD_SIZE);
	if (child_size < I915_XD_VBT_CHILD_MIN_BYTES)
		return;

	/* Reads each child that has a device type, up to the size the parse keeps. */
	count = (uint32_t)((block.size - I915_XD_VBT_DEFS_CHILDREN) / child_size);
	for (index = 0U; index < count; index++) {
		/* The child's bytes: as many as the VBT gives, at most the fields this code knows. */
		bytes.data = block.data + I915_XD_VBT_DEFS_CHILDREN + index * child_size;
		bytes.size = child_size;
		if (bytes.size > I915_XD_VBT_CHILD_KNOWN_BYTES)
			bytes.size = I915_XD_VBT_CHILD_KNOWN_BYTES;
		bytes.offset = 0U;
		bytes.found = 1;

		/* An empty slot is no output. */
		type = i915_xd_block_u16(&bytes, I915_XD_VBT_CHILD_DEVICE_TYPE);
		if (type == 0U)
			continue;

		/* A VBT with more outputs than the parse keeps loses the rest. */
		if (vbt->nchildren >= I915_XD_VBT_MAX_CHILDREN)
			return;

		/* Reads the output. */
		i915_xd_vbt_fill_child(vbt, &bytes, &vbt->children[vbt->nchildren]);
		vbt->nchildren++;
	}
}

/*
 * Makes the outputs of a board without a VBT: one on each of DDI A..F
 * that is not on a Type-C PHY, the panel on A, DisplayPort and DVI on
 * the others, and none on E; and the BDB version a minimal VBT has.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (init_vbt_missing_defaults).
 */
static void
i915_xd_vbt_defaults(
	struct i915_xd_vbt *vbt)
{
	const struct i915_xd_port_desc *port;
	struct i915_xd_vbt_block bytes;
	uint8_t child[I915_XD_VBT_CHILD_KNOWN_BYTES];
	uint32_t type;
	unsigned ddi;

	/* Reads the outputs as a minimal VBT's. */
	vbt->version = I915_XD_VBT_DEFAULT_VERSION;

	/* Makes one output on each DDI A..F that the VBT would leave to its Type-C mode. */
	for (ddi = 0U; ddi < I915_XD_VBT_DEFAULT_DDIS; ddi++) {
		/* A Type-C port's mode comes from the VBT, so without one it gets no output. */
		port = drv_i915_xd_platform_port(vbt->platform, ddi);
		if (port != NULL && port->tc != 0U)
			continue;

		/* The output's DVO port: HDMI A..D for DDI A..D, then HDMI E and F. */
		kern_memset(child, 0, sizeof(child));
		child[I915_XD_VBT_CHILD_DVO_PORT] = (uint8_t)(I915_XD_DVO_HDMI_A + ddi);
		if (ddi == I915_XD_VBT_DEFAULT_EMPTY_DDI)
			child[I915_XD_VBT_CHILD_DVO_PORT] = I915_XD_DVO_HDMI_E;
		if (ddi == I915_XD_VBT_DEFAULT_EMPTY_DDI + 1U)
			child[I915_XD_VBT_CHILD_DVO_PORT] = I915_XD_DVO_HDMI_F;

		/* What it carries: DVI except on A and E, DisplayPort except on E, the panel on A. */
		type = 0U;
		if (ddi != I915_XD_VBT_DEFAULT_PANEL_DDI && ddi != I915_XD_VBT_DEFAULT_EMPTY_DDI)
			type |= I915_XD_VBT_TYPE_TMDS;
		if (ddi != I915_XD_VBT_DEFAULT_EMPTY_DDI)
			type |= I915_XD_VBT_TYPE_DISPLAYPORT;
		if (ddi == I915_XD_VBT_DEFAULT_PANEL_DDI)
			type |= I915_XD_VBT_TYPE_INTERNAL;
		child[I915_XD_VBT_CHILD_DEVICE_TYPE] = (uint8_t)(type & 0xffU);
		child[I915_XD_VBT_CHILD_DEVICE_TYPE + 1U] = (uint8_t)(type >> 8);

		/* Reads the made-up child as any other. */
		bytes.data = child;
		bytes.size = sizeof(child);
		bytes.offset = 0U;
		bytes.found = 1;
		i915_xd_vbt_fill_child(vbt, &bytes, &vbt->children[vbt->nchildren]);
		vbt->nchildren++;
	}
}

/*
 * Reads one child device into an output: where it is, what it carries,
 * its AUX channel and DDC pin in the platform's numbering, its flags and
 * its limits, each field only from the BDB version that has it.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (intel_bios_encoder_port, intel_bios_encoder_supports_dp,
 * intel_bios_encoder_supports_edp, intel_bios_encoder_supports_dvi,
 * intel_bios_encoder_supports_hdmi, intel_bios_encoder_supports_typec_usb,
 * intel_bios_encoder_supports_tbt, intel_bios_dp_aux_ch,
 * intel_bios_hdmi_ddc_pin, intel_bios_dp_max_link_rate,
 * intel_bios_dp_max_lane_count, intel_bios_hdmi_max_tmds_clock,
 * intel_bios_dp_boost_level, intel_bios_hdmi_boost_level,
 * intel_bios_hdmi_level_shift).
 */
static void
i915_xd_vbt_fill_child(
	const struct i915_xd_vbt *vbt,
	const struct i915_xd_vbt_block *bytes,
	struct i915_xd_vbt_child *child)
{
	uint32_t flags;
	uint32_t flags2;
	uint32_t hdmi_bits;
	uint32_t levels;
	uint32_t code;

	/* Where the output is. */
	kern_memset(child, 0, sizeof(*child));
	child->handle = (uint16_t)i915_xd_block_u16(bytes, I915_XD_VBT_CHILD_HANDLE);
	child->dvo_port = (uint8_t)i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_DVO_PORT);
	child->ddi = i915_xd_vbt_dvo_ddi(vbt->platform, child->dvo_port);

	/* What it carries: an eDP output is DisplayPort on the internal connector, HDMI is DVI that is not marked otherwise. */
	child->device_type = (uint16_t)i915_xd_block_u16(bytes, I915_XD_VBT_CHILD_DEVICE_TYPE);
	if ((child->device_type & I915_XD_VBT_TYPE_DISPLAYPORT) != 0U)
		child->dp = 1;
	if (child->dp != 0 && (child->device_type & I915_XD_VBT_TYPE_INTERNAL) != 0U)
		child->edp = 1;
	if ((child->device_type & I915_XD_VBT_TYPE_TMDS) != 0U)
		child->dvi = 1;
	if (child->dvi != 0 && (child->device_type & I915_XD_VBT_TYPE_NOT_HDMI) == 0U)
		child->hdmi = 1;

	/* The Type-C and Thunderbolt flags exist from BDB 195 and 209. */
	flags2 = i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_FLAGS2);
	if (vbt->version >= I915_XD_BDB_TYPE_C && (flags2 & I915_XD_VBT_CHILD_TYPE_C) != 0U)
		child->type_c = 1;
	if (vbt->version >= I915_XD_BDB_TBT && (flags2 & I915_XD_VBT_CHILD_TBT) != 0U)
		child->tbt = 1;

	/* The AUX channel and the DDC pin, in the platform's numbering. */
	code = i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_AUX_CHANNEL);
	child->aux_ch = i915_xd_vbt_aux_ch(vbt->platform, code);
	child->ddc_code = (uint8_t)i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_DDC_PIN);
	child->gmbus_pin = i915_xd_vbt_ddc_pin(vbt->platform, child->ddc_code);

	/* The wiring flags. */
	flags = i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_FLAGS);
	if ((flags & I915_XD_VBT_CHILD_LANE_REVERSAL) != 0U)
		child->lane_reversal = 1;
	if ((flags & I915_XD_VBT_CHILD_HPD_INVERT) != 0U)
		child->hpd_invert = 1;

	/* The DisplayPort rate limit, whose codes changed at BDB 230. */
	code = i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_DP_RATE_BITS) & I915_XD_VBT_CHILD_DP_MAX_RATE;
	child->dp_max_rate_khz = i915_xd_vbt_dp_rate(vbt->version, code);

	/* The lane limit, from BDB 244: the code is the count less one. */
	if (vbt->version >= I915_XD_BDB_DP_MAX_LANES) {
		code = (flags & I915_XD_VBT_CHILD_DP_MAX_LANES) >> __builtin_ctz(I915_XD_VBT_CHILD_DP_MAX_LANES);
		child->dp_max_lanes = (uint8_t)(code + 1U);
	}

	/* The TMDS clock limit, from BDB 204. */
	hdmi_bits = i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_HDMI_BITS);
	if (vbt->version >= I915_XD_BDB_HDMI_MAX_RATE) {
		code = (hdmi_bits & I915_XD_VBT_CHILD_HDMI_MAX_RATE) >> __builtin_ctz(I915_XD_VBT_CHILD_HDMI_MAX_RATE);
		child->hdmi_max_tmds_khz = i915_xd_vbt_tmds_rate(code);
	}

	/* The current boosts, from BDB 196 and only when the child asks for boost. */
	levels = i915_xd_block_u8(bytes, I915_XD_VBT_CHILD_IBOOST_LEVELS);
	if (vbt->version >= I915_XD_BDB_IBOOST && (flags & I915_XD_VBT_CHILD_IBOOST) != 0U) {
		child->dp_boost = i915_xd_vbt_boost(levels & I915_XD_VBT_CHILD_DP_IBOOST);
		child->hdmi_boost = i915_xd_vbt_boost((levels & I915_XD_VBT_CHILD_HDMI_IBOOST) >> __builtin_ctz(I915_XD_VBT_CHILD_HDMI_IBOOST));
	}

	/* The HDMI swing row, from BDB 158. */
	child->hdmi_level_shift = -1;
	if (vbt->version >= I915_XD_BDB_HDMI_LEVEL_SHIFT)
		child->hdmi_level_shift = (int)(hdmi_bits & I915_XD_VBT_CHILD_HDMI_LEVEL_SHIFT);
}

/*
 * Reports the DDI a DVO port code names on a platform, or
 * I915_XD_VBT_NONE for a code the platform's ports do not have.
 */
static uint8_t
i915_xd_vbt_dvo_ddi(
	const struct i915_xd_platform *platform,
	unsigned code)
{
	const struct i915_xd_vbt_maps *maps;
	unsigned row;
	unsigned slot;

	/* Looks for the code among each DDI's codes. */
	maps = platform->vbt_maps;
	for (row = 0U; row < maps->ndvo; row++) {
		for (slot = 0U; slot < sizeof(maps->dvo[row].codes); slot++) {
			if (maps->dvo[row].codes[slot] == code)
				return maps->dvo[row].ddi;
		}
	}

	/* Succeeded: the code names no DDI of the platform. */
	return I915_XD_VBT_NONE;
}

/*
 * Reports the GMBUS pin a VBT DDC pin code names on a platform, or 0
 * for no pin (the code is 0 or one the platform does not have).
 */
static uint8_t
i915_xd_vbt_ddc_pin(
	const struct i915_xd_platform *platform,
	unsigned code)
{
	const struct i915_xd_vbt_maps *maps;
	unsigned pin;

	/* The code 0 names no pin. */
	if (code == 0U)
		return 0U;

	/* Looks for the GMBUS pin the code stands for. */
	maps = platform->vbt_maps;
	for (pin = 0U; pin < maps->nddc; pin++) {
		if (maps->ddc[pin] == code)
			return (uint8_t)pin;
	}

	/* Succeeded: the code is not one of the platform's pins. */
	return 0U;
}

/*
 * Reports the AUX channel a VBT AUX code names on a platform, or
 * I915_XD_VBT_NONE (the code is 0 or one the platform does not have).
 */
static uint8_t
i915_xd_vbt_aux_ch(
	const struct i915_xd_platform *platform,
	unsigned code)
{
	const struct i915_xd_vbt_maps *maps;
	unsigned channel;

	/* The code 0 names no channel. */
	if (code == 0U)
		return I915_XD_VBT_NONE;

	/* Looks for the channel the code stands for. */
	maps = platform->vbt_maps;
	for (channel = 0U; channel < maps->naux; channel++) {
		if (maps->aux[channel] == code)
			return (uint8_t)channel;
	}

	/* Succeeded: the code is not one of the platform's channels. */
	return I915_XD_VBT_NONE;
}

/*
 * Reports the DisplayPort rate limit (kHz) of a child's rate code, 0
 * for none: no limit before BDB 216, the old codes up to BDB 229, the
 * new ones after.
 */
static uint32_t
i915_xd_vbt_dp_rate(
	unsigned version,
	unsigned code)
{
	/* Before BDB 216 the VBT sets no limit. */
	if (version < I915_XD_BDB_DP_MAX_RATE)
		return 0U;

	/* The old codes: HBR3, HBR2, HBR, RBR (an unknown code is HBR3). */
	if (version < I915_XD_BDB_DP_MAX_RATE_V2) {
		/* Names the rate of the old code. */
		switch (code) {
		case 1U:
			return I915_XD_VBT_RATE_HBR2_KHZ;
		case 2U:
			return I915_XD_VBT_RATE_HBR_KHZ;
		case 3U:
			return I915_XD_VBT_RATE_RBR_KHZ;
		default:
			return I915_XD_VBT_RATE_HBR3_KHZ;
		}
	}

	/* Names the rate of the new code; 0 and unknown codes set no limit. */
	switch (code) {
	case 1U:
		return I915_XD_VBT_RATE_RBR_KHZ;
	case 2U:
		return I915_XD_VBT_RATE_HBR_KHZ;
	case 3U:
		return I915_XD_VBT_RATE_HBR2_KHZ;
	case 4U:
		return I915_XD_VBT_RATE_HBR3_KHZ;
	case 5U:
		return I915_XD_VBT_RATE_UHBR10_KHZ;
	case 6U:
		return I915_XD_VBT_RATE_UHBR13P5_KHZ;
	case 7U:
		return I915_XD_VBT_RATE_UHBR20_KHZ;
	default:
		break;
	}

	/* Succeeded: the code sets no limit. */
	return 0U;
}

/* Reports the TMDS clock limit (kHz) of a child's HDMI rate code, 0 for the platform's own. */
static uint32_t
i915_xd_vbt_tmds_rate(
	unsigned code)
{
	/* Names the clock of the code. */
	switch (code) {
	case I915_XD_VBT_HDMI_RATE_297:
		return 297000U;
	case I915_XD_VBT_HDMI_RATE_165:
		return 165000U;
	case I915_XD_VBT_HDMI_RATE_594:
		return 594000U;
	case I915_XD_VBT_HDMI_RATE_340:
		return 340000U;
	case I915_XD_VBT_HDMI_RATE_300:
		return 300000U;
	default:
		break;
	}

	/* Succeeded: the platform's own limit applies. */
	return 0U;
}

/* Reports the current boost a boost level code stands for, 0 for an unknown code. */
static uint8_t
i915_xd_vbt_boost(
	unsigned code)
{
	static const uint8_t boosts[I915_XD_VBT_IBOOST_CODES] = {
		I915_XD_VBT_IBOOST_0,
		I915_XD_VBT_IBOOST_1,
		I915_XD_VBT_IBOOST_2
	};

	/* A code past the table has no boost. */
	if (code >= I915_XD_VBT_IBOOST_CODES)
		return 0U;

	/* Succeeded: the boost of the code. */
	return boosts[code];
}

/*
 * Finds the panel data tables of the sixteen panels: through the VBT's
 * pointers block when it has one, else through pointers made up for the
 * usual layout; and checks that they describe the panel data block.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (init_bdb_block, fixup_lfp_data_ptrs, generate_lfp_data_ptrs,
 * validate_lfp_data_ptrs).
 */
static void
i915_xd_vbt_tables(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_tables *tables)
{
	struct i915_xd_vbt_block pointers;
	int error;

	/* Starts with no tables. */
	kern_memset(tables, 0, sizeof(*tables));

	/* Without the panel data block there are no tables to find. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_LFP_DATA, &tables->data);
	if (tables->data.found == 0)
		return;

	/* Takes the VBT's pointers, or makes them up when the VBT has none. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_LFP_DATA_POINTERS, &pointers);
	if (pointers.found != 0) {
		error = i915_xd_vbt_pointers(vbt, tables);
	} else {
		error = i915_xd_vbt_made_pointers(vbt, tables);
	}

	/* A VBT without the tables' pointers has no panel tables. */
	if (error != 0)
		return;

	/* Keeps the tables only when the pointers describe the block. */
	error = i915_xd_vbt_tables_fit(tables);
	if (error != 0)
		return;

	/* Succeeded: the tables are usable. */
	tables->valid = 1;
}

/*
 * Reads the VBT's pointers to the panel data tables, made relative to
 * the panel data block.  A pointer before the block is refused.
 * Returns 0 or EINVAL.
 */
static int
i915_xd_vbt_pointers(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_tables *tables)
{
	struct i915_xd_vbt_block pointers;
	uint32_t base;
	uint32_t entries;
	uint32_t fp;
	uint32_t dvo;
	uint32_t pnp;
	uint32_t fp_size;
	uint32_t dvo_size;
	uint32_t pnp_size;
	size_t at;
	unsigned panel;

	/* The pointers count from the start of the BDB; the tables are inside the data block. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_LFP_DATA_POINTERS, &pointers);
	base = tables->data.offset;

	/* Every panel has its three tables. */
	entries = i915_xd_block_u8(&pointers, I915_XD_VBT_PTRS_ENTRIES);
	if (entries != I915_XD_VBT_PTRS_EXPECTED_ENTRIES)
		return EINVAL;

	/* Reads the tables of each panel; the sizes must agree, which the fit checks. */
	tables->fp_size = i915_xd_block_u8(&pointers, I915_XD_VBT_PTRS_TABLES + I915_XD_VBT_PTRS_FP + I915_XD_VBT_PTRS_TABLE_SIZE);
	for (panel = 0U; panel < I915_XD_VBT_PANEL_TYPES; panel++) {
		/* The panel's three pointers. */
		at = I915_XD_VBT_PTRS_TABLES + panel * I915_XD_VBT_PTRS_STRIDE;
		fp = i915_xd_block_u16(&pointers, at + I915_XD_VBT_PTRS_FP + I915_XD_VBT_PTRS_OFFSET);
		dvo = i915_xd_block_u16(&pointers, at + I915_XD_VBT_PTRS_DVO + I915_XD_VBT_PTRS_OFFSET);
		pnp = i915_xd_block_u16(&pointers, at + I915_XD_VBT_PTRS_PNP + I915_XD_VBT_PTRS_OFFSET);

		/* A table that starts before the data block is not in it. */
		if (fp < base ||
		    dvo < base ||
		    pnp < base)
			return EINVAL;

		/* The sizes of every panel's tables must be the first panel's, the DVO timing's and the PnP id's. */
		fp_size = i915_xd_block_u8(&pointers, at + I915_XD_VBT_PTRS_FP + I915_XD_VBT_PTRS_TABLE_SIZE);
		dvo_size = i915_xd_block_u8(&pointers, at + I915_XD_VBT_PTRS_DVO + I915_XD_VBT_PTRS_TABLE_SIZE);
		pnp_size = i915_xd_block_u8(&pointers, at + I915_XD_VBT_PTRS_PNP + I915_XD_VBT_PTRS_TABLE_SIZE);
		if (fp_size != tables->fp_size)
			return EINVAL;
		if (dvo_size != I915_XD_VBT_DVO_TIMING_BYTES)
			return EINVAL;
		if (pnp_size != I915_XD_VBT_PNP_BYTES)
			return EINVAL;

		/* Records the tables relative to the data block. */
		tables->fp[panel] = fp - base;
		tables->dvo[panel] = dvo - base;
		tables->pnp[panel] = pnp - base;
	}

	/* The panel name table, which old VBTs do not have. */
	tables->name_size = i915_xd_block_u8(&pointers, I915_XD_VBT_PTRS_PANEL_NAME + I915_XD_VBT_PTRS_TABLE_SIZE);
	if (tables->name_size == 0U)
		return 0;

	/* A name table that starts before the data block is not in it. */
	tables->name_offset = i915_xd_block_u16(&pointers, I915_XD_VBT_PTRS_PANEL_NAME + I915_XD_VBT_PTRS_OFFSET);
	if (tables->name_offset < base)
		return EINVAL;
	tables->name_offset -= base;

	/* Succeeded: the pointers are read. */
	return 0;
}

/*
 * Makes up the pointers of a VBT that has no pointers block: the
 * sixteen entries one after another from the start of the data block,
 * each a timing table of the usual size, a DVO timing and a PnP id, and
 * the panel name table after them when the block has room for it.  An
 * old VBT (before BDB 155) or a block too small for sixteen entries gets
 * none.  Returns 0 or EINVAL.
 */
static int
i915_xd_vbt_made_pointers(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_tables *tables)
{
	uint32_t entry;
	unsigned panel;

	/* An old VBT always has the block, so a missing one is not made up. */
	if (vbt->version < I915_XD_BDB_GENERATED_POINTERS)
		return EINVAL;

	/* The block must hold sixteen entries of the usual size. */
	entry = I915_XD_VBT_FP_TIMING_GENERATED + I915_XD_VBT_DVO_TIMING_BYTES + I915_XD_VBT_PNP_BYTES;
	if (entry * I915_XD_VBT_PANEL_TYPES > tables->data.size)
		return EINVAL;

	/* Lays the entries out one after another. */
	tables->fp_size = I915_XD_VBT_FP_TIMING_GENERATED;
	for (panel = 0U; panel < I915_XD_VBT_PANEL_TYPES; panel++) {
		tables->fp[panel] = panel * entry;
		tables->dvo[panel] = panel * entry + I915_XD_VBT_FP_TIMING_GENERATED;
		tables->pnp[panel] = panel * entry + I915_XD_VBT_FP_TIMING_GENERATED + I915_XD_VBT_DVO_TIMING_BYTES;
	}

	/* The name table follows the entries when the block has room for it. */
	if ((entry + I915_XD_VBT_PANEL_NAME_BYTES) * I915_XD_VBT_PANEL_TYPES <= tables->data.size) {
		tables->name_size = I915_XD_VBT_PANEL_NAME_BYTES;
		tables->name_offset = entry * I915_XD_VBT_PANEL_TYPES;
	}

	/* Succeeded: the pointers are made up. */
	return 0;
}

/*
 * Checks that the pointers describe the panel data block: sixteen
 * equal entries one after another from the block's start, each the
 * timing table (possibly with six unaccounted bytes, which are then
 * counted in it), the DVO timing and the PnP id; every table inside the
 * block; and each timing table ending in its terminator.  Returns 0 or
 * EINVAL.
 */
static int
i915_xd_vbt_tables_fit(
	struct i915_xd_vbt_tables *tables)
{
	int64_t stride;
	uint32_t fp_size;
	uint32_t terminator;
	size_t block_size;
	unsigned panel;

	/* An empty block has no tables. */
	block_size = tables->data.size;
	if (block_size == 0U)
		return EINVAL;

	/* The timing table has a least size, and the name table, when present, its own size. */
	fp_size = tables->fp_size;
	if (fp_size < I915_XD_VBT_FP_TIMING_MIN)
		return EINVAL;
	if (tables->name_size != 0U && tables->name_size != I915_XD_VBT_PANEL_NAME_BYTES)
		return EINVAL;

	/* One entry's length, which sixteen of must fit the block. */
	stride = (int64_t)tables->fp[1] - (int64_t)tables->fp[0];
	if (16 * stride > (int64_t)block_size)
		return EINVAL;

	/* Every entry follows the one before by the same length. */
	for (panel = 1U; panel < I915_XD_VBT_PANEL_TYPES; panel++) {
		if ((int64_t)tables->fp[panel] - (int64_t)tables->fp[panel - 1U] != stride)
			return EINVAL;
		if ((int64_t)tables->dvo[panel] - (int64_t)tables->dvo[panel - 1U] != stride)
			return EINVAL;
		if ((int64_t)tables->pnp[panel] - (int64_t)tables->pnp[panel - 1U] != stride)
			return EINVAL;
	}

	/* Real VBTs leave six bytes after the timing table out of its size. */
	if ((int64_t)fp_size + I915_XD_VBT_FP_TIMING_HOLE + I915_XD_VBT_DVO_TIMING_BYTES + I915_XD_VBT_PNP_BYTES == stride)
		fp_size += I915_XD_VBT_FP_TIMING_HOLE;

	/* The three tables make up the entry, in order, from the block's start. */
	if ((int64_t)fp_size + I915_XD_VBT_DVO_TIMING_BYTES + I915_XD_VBT_PNP_BYTES != stride)
		return EINVAL;
	if (tables->fp[0] + fp_size != tables->dvo[0])
		return EINVAL;
	if (tables->dvo[0] + I915_XD_VBT_DVO_TIMING_BYTES != tables->pnp[0])
		return EINVAL;
	if ((int64_t)tables->pnp[0] + I915_XD_VBT_PNP_BYTES != stride)
		return EINVAL;

	/* Every table is inside the block. */
	for (panel = 0U; panel < I915_XD_VBT_PANEL_TYPES; panel++) {
		if (tables->fp[panel] + fp_size > block_size)
			return EINVAL;
		if (tables->dvo[panel] + I915_XD_VBT_DVO_TIMING_BYTES > block_size)
			return EINVAL;
		if (tables->pnp[panel] + I915_XD_VBT_PNP_BYTES > block_size)
			return EINVAL;
	}

	/* Refuses a name table past the block. */
	if (tables->name_offset + I915_XD_VBT_PANEL_TYPES * tables->name_size > block_size)
		return EINVAL;

	/* Every timing table ends in its terminator. */
	for (panel = 0U; panel < I915_XD_VBT_PANEL_TYPES; panel++) {
		terminator = i915_xd_block_u16(&tables->data, tables->fp[panel] + fp_size - 2U);
		if (terminator != I915_XD_VBT_FP_TERMINATOR)
			return EINVAL;
	}

	/* Succeeded: the tables describe the block. */
	tables->fp_size = fp_size;
	return 0;
}

/*
 * Chooses the panel's row in the VBT tables: the VBT's own row when it
 * names one; the row whose PnP id is the EDID's when the VBT leaves it
 * to the driver; otherwise the first row.  The OpRegion's choice, which
 * needs the firmware's runtime interface, is not asked for.  Returns 0.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (get_panel_type, fallback_get_panel_type).
 */
static int
i915_xd_vbt_panel_type(
	const struct i915_xd_vbt *vbt,
	const struct i915_xd_vbt_child *child,
	const struct i915_xd_vbt_tables *tables,
	const uint8_t *edid,
	size_t edid_size,
	struct i915_xd_vbt_panel *panel)
{
	int own;
	int pnp;

	/* The VBT's own row. */
	own = i915_xd_vbt_own_panel_type(vbt, child);
	if (own >= 0 && (unsigned)own != I915_XD_VBT_PANEL_TYPE_MATCH_EDID) {
		panel->panel_type = (unsigned)own;
		panel->type_source = I915_XD_PANEL_TYPE_VBT;
		return 0;
	}

	/* The row of the EDID's PnP id, when the VBT asks for it. */
	pnp = i915_xd_vbt_pnp_panel_type(tables, edid, edid_size);
	if ((unsigned)own == I915_XD_VBT_PANEL_TYPE_MATCH_EDID && pnp >= 0) {
		panel->panel_type = (unsigned)pnp;
		panel->type_source = I915_XD_PANEL_TYPE_PNP;
		return 0;
	}

	/* Succeeded: no rule chose a row, so the first one is the panel's. */
	panel->panel_type = 0U;
	panel->type_source = I915_XD_PANEL_TYPE_FIRST;
	return 0;
}

/*
 * Reports the row the VBT names for a panel (the second panel has its
 * own field), I915_XD_VBT_PANEL_TYPE_MATCH_EDID when the VBT leaves it to
 * the driver, or -1 when the VBT names no usable row.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (vbt_get_panel_type).
 */
static int
i915_xd_vbt_own_panel_type(
	const struct i915_xd_vbt *vbt,
	const struct i915_xd_vbt_child *child)
{
	struct i915_xd_vbt_block block;
	uint32_t type;

	/* Without the panel options the VBT names no row. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_LFP_OPTIONS, &block);
	if (block.found == 0)
		return -1;

	/* The first panel's row must be a row or the request to match the EDID. */
	type = i915_xd_block_u8(&block, I915_XD_VBT_LFP_PANEL_TYPE);
	if (type >= I915_XD_VBT_PANEL_TYPES && type != I915_XD_VBT_PANEL_TYPE_MATCH_EDID)
		return -1;

	/* The second panel takes its own field, held to the same rule. */
	if (child->handle == I915_XD_VBT_HANDLE_LFP2) {
		type = i915_xd_block_u8(&block, I915_XD_VBT_LFP_PANEL_TYPE2);
		if (type >= I915_XD_VBT_PANEL_TYPES && type != I915_XD_VBT_PANEL_TYPE_MATCH_EDID)
			return -1;
	}

	/* Succeeded: the row the VBT names. */
	return (int)type;
}

/*
 * Reports the row whose PnP id is the EDID's (manufacturer, product,
 * serial, week and year), else the first row whose id matches with the
 * date cleared, else -1.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (pnpid_get_panel_type).
 */
static int
i915_xd_vbt_pnp_panel_type(
	const struct i915_xd_vbt_tables *tables,
	const uint8_t *edid,
	size_t edid_size)
{
	uint8_t undated[I915_XD_VBT_PNP_BYTES];
	const uint8_t *id;
	unsigned panel;
	int best;
	int compared;

	/* Without the EDID or the tables nothing can match. */
	if (edid == NULL || edid_size < I915_XD_EDID_BLOCK_BYTES)
		return -1;
	if (tables->valid == 0)
		return -1;

	/* The EDID's id is its bytes 8..17, laid out as the VBT's; the undated copy has no week and year. */
	id = edid + I915_XD_EDID_VENDOR;
	kern_memcpy(undated, id, sizeof(undated));
	undated[I915_XD_VBT_PNP_BYTES - 2U] = 0U;
	undated[I915_XD_VBT_PNP_BYTES - 1U] = 0U;

	/* Takes the first full match, remembering the first undated one. */
	best = -1;
	for (panel = 0U; panel < I915_XD_VBT_PANEL_TYPES; panel++) {
		/* A full match is the panel's row. */
		compared = kern_memcmp(tables->data.data + tables->pnp[panel], id, I915_XD_VBT_PNP_BYTES);
		if (compared == 0)
			return (int)panel;

		/* An entry without a date matches the undated id. */
		compared = kern_memcmp(tables->data.data + tables->pnp[panel], undated, I915_XD_VBT_PNP_BYTES);
		if (best < 0 && compared == 0)
			best = (int)panel;
	}

	/* Succeeded: the undated match, or -1. */
	return best;
}

/*
 * Reads the panel options: dithering, and the refresh rate switching
 * the panel allows (only in a block of at least 16 bytes).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_panel_options).
 */
static void
i915_xd_vbt_panel_options(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_block block;
	uint32_t caps;
	uint32_t bits;
	unsigned mode;

	/* Without the block the panel keeps its defaults. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_LFP_OPTIONS, &block);
	if (block.found == 0)
		return;

	/* The dither flag. */
	caps = i915_xd_block_u8(&block, I915_XD_VBT_LFP_CAPS);
	panel->dither = 0;
	if ((caps & I915_XD_VBT_LFP_DITHER) != 0U)
		panel->dither = 1;

	/* A short block has no switching bits. */
	if (block.size < I915_XD_VBT_LFP_DRRS_MIN_BLOCK)
		return;

	/* The panel's two bits: 0 switches at a mode set, 2 seamlessly, the others not at all. */
	bits = i915_xd_block_u32(&block, I915_XD_VBT_LFP_DRRS_BITS);
	mode = i915_xd_vbt_panel_bits(bits, panel->panel_type, 2U);

	/* Names the switching the bits allow. */
	switch (mode) {
	case 0U:
		panel->drrs = I915_XD_DRRS_STATIC;
		break;
	case 2U:
		panel->drrs = I915_XD_DRRS_SEAMLESS;
		break;
	default:
		panel->drrs = I915_XD_DRRS_NONE;
		break;
	}
}

/*
 * Reads the panel's mode from the generic DTD block (BDB 229 on).
 *
 * The entries are read at their own size whatever the block says it is;
 * a block whose entries are too small, or that has no entry for the
 * panel, gives no mode.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_generic_dtd).
 */
static void
i915_xd_vbt_generic_dtd(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_block block;
	struct i915_display_mode *mode;
	uint32_t entry_size;
	uint32_t entries;
	uint32_t flags;
	size_t at;

	/* An older VBT has no generic DTD. */
	if (vbt->version < I915_XD_BDB_GENERIC_DTD)
		return;

	/* Without the block there is no mode here. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_GENERIC_DTD, &block);
	if (block.found == 0)
		return;

	/* Entries smaller than the known layout cannot be read. */
	entry_size = i915_xd_block_u16(&block, I915_XD_VBT_GDTD_SIZE);
	if (entry_size < I915_XD_VBT_GDTD_ENTRY_BYTES)
		return;

	/* The block must have an entry for the panel. */
	entries = 0U;
	if (block.size > I915_XD_VBT_GDTD_ENTRIES)
		entries = (uint32_t)((block.size - I915_XD_VBT_GDTD_ENTRIES) / entry_size);
	if (panel->panel_type >= entries)
		return;

	/* The horizontal timing: active, sync start and end after the front porch and the sync, total. */
	at = I915_XD_VBT_GDTD_ENTRIES + panel->panel_type * I915_XD_VBT_GDTD_ENTRY_BYTES;
	mode = &panel->mode;
	kern_memset(mode, 0, sizeof(*mode));
	mode->hdisplay = (uint16_t)i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_HACTIVE);
	mode->hsync_start = (uint16_t)(mode->hdisplay + i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_HFRONT));
	mode->hsync_end = (uint16_t)(mode->hsync_start + i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_HSYNC));
	mode->htotal = (uint16_t)(mode->hdisplay + i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_HBLANK));

	/* The vertical timing, in the same way. */
	mode->vdisplay = (uint16_t)i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_VACTIVE);
	mode->vsync_start = (uint16_t)(mode->vdisplay + i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_VFRONT));
	mode->vsync_end = (uint16_t)(mode->vsync_start + i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_VSYNC));
	mode->vtotal = (uint16_t)(mode->vdisplay + i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_VBLANK));

	/* The pixel clock in kHz, the image size, and the sync polarities. */
	mode->clock_khz = (int)i915_xd_block_u32(&block, at + I915_XD_VBT_GDTD_CLOCK);
	mode->width_mm = (uint16_t)i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_WIDTH_MM);
	mode->height_mm = (uint16_t)i915_xd_block_u16(&block, at + I915_XD_VBT_GDTD_HEIGHT_MM);
	flags = i915_xd_block_u8(&block, at + I915_XD_VBT_GDTD_FLAGS);
	if ((flags & I915_XD_VBT_GDTD_HSYNC_POSITIVE) != 0U)
		mode->hsync_positive = 1;
	if ((flags & I915_XD_VBT_GDTD_VSYNC_POSITIVE) != 0U)
		mode->vsync_positive = 1;

	/* The panel has the VBT's mode. */
	panel->has_mode = 1;
}

/*
 * Reads the panel's mode from the panel data block's DVO timing, when
 * the generic DTD gave none: the timing is laid out as an EDID detailed
 * timing, and sync ends past the totals are cut back to them.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_lfp_data, parse_lfp_panel_dtd, fill_detail_timing_data).
 */
static void
i915_xd_vbt_lfp_dtd(
	const struct i915_xd_vbt_tables *tables,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_block timing;
	struct i915_display_mode *mode;
	uint32_t h_hi;
	uint32_t v_hi;
	uint32_t vsync_lo;
	uint32_t sync_hi;
	uint32_t image_hi;
	uint32_t flags;
	uint32_t value;

	/* A mode from the generic DTD stands, and without the tables there is none here. */
	if (panel->has_mode != 0)
		return;
	if (tables->valid == 0)
		return;

	/* The panel's DVO timing inside the data block. */
	timing.data = tables->data.data + tables->dvo[panel->panel_type];
	timing.size = I915_XD_VBT_DVO_TIMING_BYTES;
	timing.offset = 0U;
	timing.found = 1;
	h_hi = i915_xd_block_u8(&timing, I915_XD_DVO_H_HI);
	v_hi = i915_xd_block_u8(&timing, I915_XD_DVO_V_HI);
	vsync_lo = i915_xd_block_u8(&timing, I915_XD_DVO_VSYNC_LO);
	sync_hi = i915_xd_block_u8(&timing, I915_XD_DVO_SYNC_HI);
	image_hi = i915_xd_block_u8(&timing, I915_XD_DVO_IMAGE_HI);
	mode = &panel->mode;
	kern_memset(mode, 0, sizeof(*mode));

	/* The horizontal active pixels, then the sync start after the sync offset. */
	value = i915_xd_block_u8(&timing, I915_XD_DVO_HACTIVE_LO) | (((h_hi & I915_XD_DVO_HACTIVE_HI) >> 4) << 8);
	mode->hdisplay = (uint16_t)value;
	value = i915_xd_block_u8(&timing, I915_XD_DVO_HSYNC_OFF_LO) | (((sync_hi & I915_XD_DVO_HSYNC_OFF_HI) >> 6) << 8);
	mode->hsync_start = (uint16_t)(mode->hdisplay + value);

	/* The horizontal sync end after the sync width, and the total after the blank. */
	value = i915_xd_block_u8(&timing, I915_XD_DVO_HSYNC_WIDTH_LO) | (((sync_hi & I915_XD_DVO_HSYNC_WIDTH_HI) >> 4) << 8);
	mode->hsync_end = (uint16_t)(mode->hsync_start + value);
	value = i915_xd_block_u8(&timing, I915_XD_DVO_HBLANK_LO) | ((h_hi & I915_XD_DVO_HBLANK_HI) << 8);
	mode->htotal = (uint16_t)(mode->hdisplay + value);

	/* The vertical active lines, then the sync start after the sync offset (its upper bits at 4). */
	value = i915_xd_block_u8(&timing, I915_XD_DVO_VACTIVE_LO) | (((v_hi & I915_XD_DVO_VACTIVE_HI) >> 4) << 8);
	mode->vdisplay = (uint16_t)value;
	value = ((vsync_lo & I915_XD_DVO_VSYNC_OFF_LO) >> 4) | (((sync_hi & I915_XD_DVO_VSYNC_OFF_HI) >> 2) << 4);
	mode->vsync_start = (uint16_t)(mode->vdisplay + value);

	/* The vertical sync end after the sync width, and the total after the blank. */
	value = (vsync_lo & I915_XD_DVO_VSYNC_WIDTH_LO) | ((sync_hi & I915_XD_DVO_VSYNC_WIDTH_HI) << 4);
	mode->vsync_end = (uint16_t)(mode->vsync_start + value);
	value = i915_xd_block_u8(&timing, I915_XD_DVO_VBLANK_LO) | ((v_hi & I915_XD_DVO_VBLANK_HI) << 8);
	mode->vtotal = (uint16_t)(mode->vdisplay + value);

	/* The clock (10 kHz units) and the image size. */
	mode->clock_khz = (int)(i915_xd_block_u16(&timing, I915_XD_DVO_CLOCK) * 10U);
	value = i915_xd_block_u8(&timing, I915_XD_DVO_HIMAGE_LO) | (((image_hi & I915_XD_DVO_HIMAGE_HI) >> 4) << 8);
	mode->width_mm = (uint16_t)value;
	value = i915_xd_block_u8(&timing, I915_XD_DVO_VIMAGE_LO) | ((image_hi & I915_XD_DVO_VIMAGE_HI) << 8);
	mode->height_mm = (uint16_t)value;

	/* The sync polarities. */
	flags = i915_xd_block_u8(&timing, I915_XD_DVO_FLAGS);
	if ((flags & I915_XD_DVO_HSYNC_POSITIVE) != 0U)
		mode->hsync_positive = 1;
	if ((flags & I915_XD_DVO_VSYNC_POSITIVE) != 0U)
		mode->vsync_positive = 1;

	/* Some VBTs end the syncs past the totals; the ends are cut back. */
	if (mode->hsync_end > mode->htotal)
		mode->hsync_end = mode->htotal;
	if (mode->vsync_end > mode->vtotal)
		mode->vsync_end = mode->vtotal;

	/* The panel has the VBT's mode. */
	panel->has_mode = 1;
}

/*
 * Reads the panel's backlight: present only when it is a PWM backlight;
 * its type and controller (BDB 191 on), frequency and polarity; and its
 * least brightness, which BDB 234 on keeps as a word, scaled to 8 bits
 * when the word has 16 bits of precision.  A block with entries of
 * another size is not read.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_lfp_backlight).
 */
static void
i915_xd_vbt_backlight(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_block block;
	uint32_t entry_size;
	uint32_t entry;
	uint32_t flags;
	uint32_t control;
	uint32_t level;
	uint32_t minimum;
	uint32_t precision;
	int scale;

	/* Without the block the default stands. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_LFP_BACKLIGHT, &block);
	if (block.found == 0)
		return;

	/* A block with entries of another size is not read either. */
	entry_size = i915_xd_block_u8(&block, I915_XD_VBT_BL_ENTRY_SIZE);
	if (entry_size != I915_XD_VBT_BL_ENTRY_BYTES)
		return;

	/* Only a PWM backlight is driven. */
	entry = I915_XD_VBT_BL_ENTRIES + panel->panel_type * I915_XD_VBT_BL_ENTRY_BYTES;
	flags = i915_xd_block_u8(&block, entry + I915_XD_VBT_BL_FLAGS);
	panel->backlight = 0;
	if ((flags & I915_XD_VBT_BL_TYPE) != I915_XD_VBT_BL_TYPE_PWM)
		return;
	panel->backlight = 1;

	/* The display's own PWM on controller 0, unless the control method says otherwise. */
	panel->backlight_type = I915_XD_VBT_BACKLIGHT_DISPLAY;
	panel->backlight_controller = 0U;
	if (vbt->version >= I915_XD_BDB_BACKLIGHT_CONTROL) {
		control = i915_xd_block_u8(&block, I915_XD_VBT_BL_CONTROL + panel->panel_type);
		panel->backlight_type = (uint8_t)(control & I915_XD_VBT_BL_CONTROL_TYPE);
		panel->backlight_controller = (uint8_t)((control & I915_XD_VBT_BL_CONTROL_CONTROLLER) >> 4);
	}

	/* The PWM frequency and polarity. */
	panel->backlight_hz = (uint16_t)i915_xd_block_u16(&block, entry + I915_XD_VBT_BL_PWM_HZ);
	if ((flags & I915_XD_VBT_BL_ACTIVE_LOW) != 0U)
		panel->backlight_active_low = 1;

	/* An older VBT keeps the least brightness as one byte. */
	if (vbt->version < I915_XD_BDB_BRIGHTNESS_WORDS) {
		panel->backlight_min = (uint8_t)i915_xd_block_u8(&block, entry + I915_XD_VBT_BL_OLD_MIN);
		return;
	}

	/* The words, and whether they are on a 16-bit scale (said from BDB 236, guessed from the level before). */
	level = i915_xd_block_u16(&block, I915_XD_VBT_BL_LEVELS + panel->panel_type * I915_XD_VBT_BL_LEVEL_STRIDE);
	minimum = i915_xd_block_u16(&block, I915_XD_VBT_BL_MIN_LEVELS + panel->panel_type * I915_XD_VBT_BL_LEVEL_STRIDE);
	precision = i915_xd_block_u8(&block, I915_XD_VBT_BL_PRECISION + panel->panel_type);
	scale = 0;
	if (vbt->version >= I915_XD_BDB_BRIGHTNESS_PRECISION) {
		if (precision == I915_XD_VBT_PRECISION_16)
			scale = 1;
	} else if (level > I915_XD_VBT_BRIGHTNESS_8BIT) {
		scale = 1;
	}

	/* Brings a 16-bit least brightness to the 8-bit scale the driver keeps it on. */
	if (scale != 0)
		minimum = minimum / I915_XD_VBT_BRIGHTNESS_8BIT;
	panel->backlight_min = (uint8_t)minimum;
	panel->backlight_precision = (uint8_t)precision;
}

/*
 * Reads, before BDB 228, whether the driver features turn the panel's
 * refresh rate switching off (or down to a switch at a mode set).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_panel_driver_features).
 */
static void
i915_xd_vbt_driver_drrs(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_block block;
	uint32_t flags;

	/* From BDB 228 the power conservation block says it instead. */
	if (vbt->version >= I915_XD_BDB_LFP_POWER_FLAGS)
		return;

	/* Without the block the panel options stand. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_DRIVER_FEATURES, &block);
	if (block.found == 0)
		return;

	/* Switching that is turned off stays only at a mode set when DMRRS is on. */
	flags = i915_xd_block_u16(&block, I915_XD_VBT_DRIVER_FLAGS);
	if ((flags & I915_XD_VBT_DRIVER_DRRS) == 0U && panel->drrs != I915_XD_DRRS_NONE) {
		panel->drrs = I915_XD_DRRS_NONE;
		if ((flags & I915_XD_VBT_DRIVER_DMRRS) != 0U)
			panel->drrs = I915_XD_DRRS_STATIC;
	}
}

/*
 * Reads the power conservation features (BDB 228 on): refresh rate
 * switching turned off or down, HOBL (BDB 232 on) and VRR (BDB 233 on;
 * allowed before, as other systems do).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_power_conservation_features).
 */
static void
i915_xd_vbt_power(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_block block;
	uint32_t word;
	unsigned drrs;
	unsigned dmrrs;

	/* VRR is allowed unless the VBT says otherwise. */
	panel->vrr = 1;

	/* An older VBT has no power conservation flags. */
	if (vbt->version < I915_XD_BDB_LFP_POWER_FLAGS)
		return;

	/* Without the block the defaults stand. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_LFP_POWER, &block);
	if (block.found == 0)
		return;

	/* Switching that is turned off stays only at a mode set when DMRRS is on. */
	word = i915_xd_block_u16(&block, I915_XD_VBT_POWER_DRRS);
	drrs = i915_xd_vbt_panel_bits(word, panel->panel_type, 1U);
	word = i915_xd_block_u16(&block, I915_XD_VBT_POWER_DMRRS);
	dmrrs = i915_xd_vbt_panel_bits(word, panel->panel_type, 1U);
	if (drrs == 0U && panel->drrs != I915_XD_DRRS_NONE) {
		panel->drrs = I915_XD_DRRS_NONE;
		if (dmrrs != 0U)
			panel->drrs = I915_XD_DRRS_STATIC;
	}

	/* The high-efficiency swing setting. */
	if (vbt->version >= I915_XD_BDB_HOBL) {
		word = i915_xd_block_u16(&block, I915_XD_VBT_POWER_HOBL);
		panel->hobl = (int)i915_xd_vbt_panel_bits(word, panel->panel_type, 1U);
	}

	/* The VBT's own VRR switch. */
	if (vbt->version >= I915_XD_BDB_VRR) {
		word = i915_xd_block_u16(&block, I915_XD_VBT_POWER_VRR);
		panel->vrr = (int)i915_xd_vbt_panel_bits(word, panel->panel_type, 1U);
	}
}

/*
 * Reads the panel's eDP data: the colour depth, the power sequence, the
 * link rate (a word in 20 kHz units from BDB 224, a code before), the
 * lane count, pre-emphasis and swing levels, the swing table choice
 * (BDB 173 on) and the largest link rate (BDB 244 on).  Codes the VBT
 * spec does not define leave their field 0.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/i915/display/intel_bios.c
 * (parse_edp).
 */
static void
i915_xd_vbt_edp(
	const struct i915_xd_vbt *vbt,
	struct i915_xd_vbt_panel *panel)
{
	struct i915_xd_vbt_block block;
	size_t pps;
	size_t link;
	uint32_t link_bits;
	uint32_t level_bits;
	uint32_t depth;
	uint64_t choices;
	unsigned code;

	/* Without the block the panel has no eDP data. */
	i915_xd_vbt_find_block(vbt, I915_XD_BLOCK_EDP, &block);
	if (block.found == 0)
		return;

	/* The colour depth's two bits. */
	depth = i915_xd_block_u32(&block, I915_XD_VBT_EDP_COLOR_DEPTH);
	code = i915_xd_vbt_panel_bits(depth, panel->panel_type, 2U);
	if (code == I915_XD_VBT_EDP_BPP_18)
		panel->bpp = 18U;
	if (code == I915_XD_VBT_EDP_BPP_24)
		panel->bpp = 24U;
	if (code == I915_XD_VBT_EDP_BPP_30)
		panel->bpp = 30U;

	/* The power sequence delays. */
	pps = I915_XD_VBT_EDP_PPS + panel->panel_type * I915_XD_VBT_EDP_PPS_STRIDE;
	panel->t1_t3 = (uint16_t)i915_xd_block_u16(&block, pps + I915_XD_VBT_EDP_PPS_T1_T3);
	panel->t8 = (uint16_t)i915_xd_block_u16(&block, pps + I915_XD_VBT_EDP_PPS_T8);
	panel->t9 = (uint16_t)i915_xd_block_u16(&block, pps + I915_XD_VBT_EDP_PPS_T9);
	panel->t10 = (uint16_t)i915_xd_block_u16(&block, pps + I915_XD_VBT_EDP_PPS_T10);
	panel->t11_t12 = (uint16_t)i915_xd_block_u16(&block, pps + I915_XD_VBT_EDP_PPS_T11_T12);

	/* The fast link parameters: rate and lane codes, then pre-emphasis and swing. */
	link = I915_XD_VBT_EDP_LINK + panel->panel_type * I915_XD_VBT_EDP_LINK_STRIDE;
	link_bits = i915_xd_block_u8(&block, link);
	level_bits = i915_xd_block_u8(&block, link + 1U);

	/* The rate: a word from BDB 224, a code before. */
	if (vbt->version >= I915_XD_BDB_EDP_RATE_WORD) {
		panel->edp_rate_khz = i915_xd_block_u16(&block, I915_XD_VBT_EDP_LINK_RATE_WORDS + panel->panel_type * 2U) * I915_XD_VBT_EDP_RATE_UNIT_KHZ;
	} else {
		/* Names the rate of the code. */
		code = link_bits & I915_XD_VBT_EDP_LINK_RATE;
		switch (code) {
		case I915_XD_VBT_EDP_RATE_1P62:
			panel->edp_rate_khz = I915_XD_VBT_RATE_RBR_KHZ;
			break;
		case I915_XD_VBT_EDP_RATE_2P7:
			panel->edp_rate_khz = I915_XD_VBT_RATE_HBR_KHZ;
			break;
		case I915_XD_VBT_EDP_RATE_5P4:
			panel->edp_rate_khz = I915_XD_VBT_RATE_HBR2_KHZ;
			break;
		default:
			break;
		}
	}

	/* Names the lane count of the code. */
	code = (link_bits & I915_XD_VBT_EDP_LINK_LANES) >> 4;
	switch (code) {
	case I915_XD_VBT_EDP_LANES_1:
		panel->edp_lanes = 1U;
		break;
	case I915_XD_VBT_EDP_LANES_2:
		panel->edp_lanes = 2U;
		break;
	case I915_XD_VBT_EDP_LANES_4:
		panel->edp_lanes = 4U;
		break;
	default:
		break;
	}

	/* The pre-emphasis and swing codes are the levels 0..3. */
	panel->edp_preemphasis = (uint8_t)(level_bits & I915_XD_VBT_EDP_LINK_PREEMPHASIS);
	panel->edp_vswing = (uint8_t)((level_bits & I915_XD_VBT_EDP_LINK_VSWING) >> 4);

	/* The low swing table is chosen by a zero nibble (BDB 173 on). */
	if (vbt->version >= I915_XD_BDB_EDP_LOW_VSWING) {
		choices = i915_xd_block_u64(&block, I915_XD_VBT_EDP_VSWING_PREEMPH);
		code = i915_xd_vbt_panel_bits(choices, panel->panel_type, 4U);
		panel->low_vswing = 0;
		if (code == 0U)
			panel->low_vswing = 1;
	}

	/* The largest link rate (BDB 244 on). */
	if (vbt->version >= I915_XD_BDB_EDP_MAX_RATE)
		panel->edp_max_rate_khz = i915_xd_block_u16(&block, I915_XD_VBT_EDP_MAX_RATE_WORDS + panel->panel_type * 2U) * I915_XD_VBT_EDP_RATE_UNIT_KHZ;
}

/* Takes a panel's field of the given width out of a word that holds one field a panel. */
static unsigned
i915_xd_vbt_panel_bits(
	uint64_t word,
	unsigned panel_type,
	unsigned width)
{
	uint64_t mask;

	/* The field's mask at the bottom of the word. */
	mask = (1ULL << width) - 1U;

	/* Succeeded: the panel's field. */
	return (unsigned)((word >> (panel_type * width)) & mask);
}

/* Starts a digest from the initial hash words. */
static void
i915_xd_sha256_start(
	struct i915_xd_sha256 *sha)
{
	/* Clears the state and loads the initial hash. */
	kern_memset(sha, 0, sizeof(*sha));
	kern_memcpy(sha->hash, i915_xd_sha256_initial, sizeof(sha->hash));
}

/* Adds bytes to a digest, hashing each block as it fills. */
static void
i915_xd_sha256_add(
	struct i915_xd_sha256 *sha,
	const uint8_t *data,
	size_t size)
{
	size_t index;

	/* Moves the bytes into the block, hashing every full block. */
	for (index = 0U; index < size; index++) {
		sha->block[sha->used] = data[index];
		sha->used++;
		sha->length++;
		if (sha->used == I915_XD_SHA256_BLOCK) {
			i915_xd_sha256_block(sha);
			sha->used = 0U;
		}
	}
}

/* Pads the message with its bit length, hashes the rest and writes the digest out big endian. */
static void
i915_xd_sha256_finish(
	struct i915_xd_sha256 *sha,
	uint8_t digest[32])
{
	uint64_t bits;
	unsigned index;

	/* The one bit after the message, then zeros up to the room for the length. */
	bits = sha->length * 8U;
	sha->block[sha->used] = 0x80U;
	sha->used++;
	if (sha->used > I915_XD_SHA256_BLOCK - I915_XD_SHA256_LENGTH_BYTES) {
		kern_memset(sha->block + sha->used, 0, I915_XD_SHA256_BLOCK - sha->used);
		i915_xd_sha256_block(sha);
		sha->used = 0U;
	}

	/* Pads the final block with zeros. */
	kern_memset(sha->block + sha->used, 0, I915_XD_SHA256_BLOCK - sha->used);

	/* The message length in bits, big endian, ends the last block. */
	for (index = 0U; index < I915_XD_SHA256_LENGTH_BYTES; index++)
		sha->block[I915_XD_SHA256_BLOCK - 1U - index] = (uint8_t)(bits >> (8U * index));
	i915_xd_sha256_block(sha);

	/* Writes the hash words out, big endian. */
	for (index = 0U; index < 32U; index++)
		digest[index] = (uint8_t)(sha->hash[index / 4U] >> (24U - 8U * (index % 4U)));
}

/* Hashes one full block into the hash words. */
static void
i915_xd_sha256_block(
	struct i915_xd_sha256 *sha)
{
	uint32_t schedule[64];
	uint32_t work[8];
	uint32_t sigma0;
	uint32_t sigma1;
	uint32_t choose;
	uint32_t majority;
	uint32_t first;
	uint32_t second;
	unsigned round;

	/* The message schedule: the block's sixteen words, then the expanded ones. */
	for (round = 0U; round < 16U; round++) {
		schedule[round] = ((uint32_t)sha->block[round * 4U] << 24) |
		    ((uint32_t)sha->block[round * 4U + 1U] << 16) |
		    ((uint32_t)sha->block[round * 4U + 2U] << 8) |
		    (uint32_t)sha->block[round * 4U + 3U];
	}

	/* Expands the schedule to 64 words. */
	for (round = 16U; round < 64U; round++) {
		sigma0 = i915_xd_sha256_rotr(schedule[round - 15U], 7U) ^ i915_xd_sha256_rotr(schedule[round - 15U], 18U) ^ (schedule[round - 15U] >> 3);
		sigma1 = i915_xd_sha256_rotr(schedule[round - 2U], 17U) ^ i915_xd_sha256_rotr(schedule[round - 2U], 19U) ^ (schedule[round - 2U] >> 10);
		schedule[round] = schedule[round - 16U] + sigma0 + schedule[round - 7U] + sigma1;
	}

	/* The sixty-four rounds over a copy of the hash words. */
	kern_memcpy(work, sha->hash, sizeof(work));
	for (round = 0U; round < 64U; round++) {
		sigma1 = i915_xd_sha256_rotr(work[4], 6U) ^ i915_xd_sha256_rotr(work[4], 11U) ^ i915_xd_sha256_rotr(work[4], 25U);
		choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
		first = work[7] + sigma1 + choose + i915_xd_sha256_k[round] + schedule[round];
		sigma0 = i915_xd_sha256_rotr(work[0], 2U) ^ i915_xd_sha256_rotr(work[0], 13U) ^ i915_xd_sha256_rotr(work[0], 22U);
		majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
		second = sigma0 + majority;
		work[7] = work[6];
		work[6] = work[5];
		work[5] = work[4];
		work[4] = work[3] + first;
		work[3] = work[2];
		work[2] = work[1];
		work[1] = work[0];
		work[0] = first + second;
	}

	/* Adds the result into the hash words. */
	for (round = 0U; round < 8U; round++)
		sha->hash[round] += work[round];
}

/* Rotates a word right. */
static uint32_t
i915_xd_sha256_rotr(
	uint32_t word,
	unsigned count)
{
	/* Succeeded: the rotated word. */
	return (word >> count) | (word << (32U - count));
}

/* Copies size bytes of physical memory through the reader's mapping; 0, or the mapping's errno. */
static int
i915_xd_opregion_copy(
	const struct i915_xd_opregion_access *access,
	uint64_t address,
	size_t size,
	uint8_t *copy)
{
	const volatile uint8_t *bytes;
	size_t index;
	int error;

	/* Maps the memory readable. */
	bytes = NULL;
	error = access->map(access->context, address, size, &bytes);
	if (error != 0)
		return error;
	if (bytes == NULL)
		return EIO;

	/* Copies it a byte at a time (device memory) and gives the mapping back. */
	for (index = 0U; index < size; index++)
		copy[index] = bytes[index];
	access->unmap(access->context, bytes, size);

	/* Succeeded: the bytes are in the copy. */
	return 0;
}

