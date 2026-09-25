/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_bios.c),
 * which carries the following notice.
 *
 * Copyright © 2006 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 */

/*
 * The VBT: where it comes from, the Linux parser, and what the display reads
 * of it (see vbt-parse.h).
 *
 * intel_bios_init() in the Linux order: the device and block lists, the
 * defaults, then the VBT bytes -- in the test build only, the explicit blob
 * when it is pinned to this machine; then the OpRegion's validated copy, else
 * the PCI expansion ROM read through configuration offset 0x30 and a device
 * mapping and scanned for "$VBT".  A found and validated VBT is parsed by
 * the Linux v6.8.12 intel_bios.c text below (the BDB header, the block walk,
 * the general features and definitions, the panel blocks); a genuine absence
 * takes init_vbt_missing_defaults().  The DGFX SPI-flash path is not run on
 * ADL-P.
 * Nothing here fabricates a VBT: "absent" is a real read result.
 *
 * The parser's functions keep the Linux text's decisions, values and
 * messages; the Linux copyright and permission notice of intel_bios.c, from
 * which they are rewritten, is in intel/vbt-defs.h.  The Linux text's
 * messages are counted and
 * shown as their format text only (vbt.h).
 */

#include "vbt.h"
#include "vbt-parse.h"
#include <kern/kcrt.h>

#include "../pci.h"
#include "../trace.h"

#include <uapi/errno.h>
#include <hal/hal.h>
#include <kern/klog.h>
#include <kern/kmem.h>

/*
 * The VBT block layouts of the Linux text and the Linux parser's tables (the
 * BDB block sizes, the DDC pin, port and AUX channel maps), which are private
 * to the parser.  The tables never change.
 */
#define _INTEL_BIOS_PRIVATE
#include "../intel/vbt-defs.h"

/* The size of the arena every parser allocation comes from. */
#define I915_VBT_ARENA_BYTES (48u * 1024u)

/* The vbt_header fields the byte-level check reads: signature[20], version, header_size, vbt_size, ... */
#define I915_VBT_OFF_VBT_SIZE    24u	/* u16 */
#define I915_VBT_OFF_BDB_OFFSET  28u	/* u32 */
#define I915_VBT_HEADER_SIZE     48u

/* The bdb_header fields the byte-level check reads: signature[16], version, header_size, bdb_size. */
#define I915_BDB_OFF_VERSION     16u	/* u16 */
#define I915_BDB_OFF_HEADER_SIZE 18u	/* u16 */
#define I915_BDB_OFF_BDB_SIZE    20u	/* u16 */
#define I915_BDB_HEADER_SIZE     22u

/* The PCI configuration offsets the VBT acquisition reads. */
#define I915_VBT_PCI_ROM_BASE        0x30u	/* the expansion ROM base address register */
#define I915_VBT_PCI_SUBSYS_VENDOR   0x2cu
#define I915_VBT_PCI_SUBSYS_DEVICE   0x2eu

/* The address bits and the decode enable of the ROM base address register. */
#define I915_VBT_ROM_ADDRESS_MASK 0xFFFFF800u
#define I915_VBT_ROM_ENABLE       1u

/* The largest ROM window the scan maps. */
#define I915_VBT_ROM_MAP_MAX (256u * 1024u)

/* The probe stage the display start stamps its trace records with (intel_display_driver_probe_noirq). */
#define I915_VBT_TRACE_STAGE_P3 4u

/* The largest child device table the flattened VBT state keeps. */
#define I915_VBT_STATE_MAX_CHILDREN 8u

/*
 * The panel type sources, in the order get_panel_type() consults them.
 */
enum panel_type {
	PANEL_TYPE_OPREGION,
	PANEL_TYPE_VBT,
	PANEL_TYPE_PNPID,
	PANEL_TYPE_FALLBACK,
};

/*
 * The device the Linux parser works on.
 *
 * It is the parser's own layout, without the DP members (dp-internal.h has
 * the DP layout under the same Linux name); one instance lives in the
 * parser's world and is cleared by every drv_i915_vbt_init().
 */
struct drm_i915_private {
	/* The DRM device the Linux messages name. */
	struct drm_device drm;

	/* The display's VBT data and the one module parameter the parser reads. */
	struct {
		/* What the parser found in the VBT. */
		struct intel_vbt_data vbt;

		/* The i915.edp_vswing module parameter: 0 lets the VBT decide. */
		struct {
			int edp_vswing;
		} params;
	} display;
};

/*
 * The VBT parser's world state.
 *
 * It holds what the old parser kept in file-scope variables.  The display
 * owns it through display->vbt_world from drv_i915_vbt_world_create() to
 * drv_i915_vbt_world_destroy(); the probe thread is its only user.
 */
struct i915_vbt_world {
	/* The device the parser works on; cleared by every parse. */
	struct drm_i915_private vbt_i915;

	/* The bump arena every parser allocation comes from, released as a whole. */
	uint8_t vbt_arena[I915_VBT_ARENA_BYTES];

	/* How much of the arena is handed out, the most ever handed out, and the refusals. */
	unsigned vbt_arena_used;
	unsigned vbt_arena_peak;
	unsigned vbt_alloc_failures;

	/* The validated VBT bytes the parser reads, or NULL for none. */
	const void *vbt_provider_bytes;

	/* The parse result the world is working for; NULL when no VBT state is live. */
	struct i915_vbt *vbt_live;

	/* The most verbose message level shown (I915_VBT_LOG_*), and the error messages counted. */
	int vbt_log_level;
	unsigned vbt_log_errors;

	/* The panel drv_i915_vbt_init_panel() fills; the mode it may allocate lives in the arena. */
	struct intel_panel i915_vbt_init_panel_panel;
};

/*
 * One VBT child device as the parser keeps it (the Linux wrapper of the
 * child device config).
 *
 * It lives in the arena on the device's display_devices list until the
 * parser releases the VBT.
 */
struct intel_bios_encoder_data {
	/* The device whose VBT named the child. */
	struct drm_i915_private *i915;

	/* As much of the child device config as the VBT carried. */
	struct child_device_config child;

	/* The DSC parameters of the child; never filled here. */
	struct dsc_compression_parameters_entry *dsc;

	/* The link on the device's display_devices list. */
	struct list_head node;
};

/*
 * One BDB block the parser copied out of the VBT.
 *
 * The data carries the block's 3-byte header followed by the block, padded
 * to the block's minimum size; it lives in the arena on the device's
 * bdb_blocks list until the parser releases the VBT.
 */
struct bdb_block_entry {
	/* The link on the device's bdb_blocks list. */
	struct list_head node;

	/* Which block this is. */
	enum bdb_block_id section_id;

	/* The block header and the block. */
	u8 data[];
};

#ifdef I915_TEST_VBT
/*
 * One pinned VBT blob: the machine it belongs to and its SHA-256.
 *
 * A row is used only when its SHA-256 is the pinned one, the bytes hold a
 * valid VBT, and the PCI subsystem id is the machine's.
 *
 * XXX: a test crutch.  The QEMU passthrough test runs the driver in a guest
 * whose firmware presents ASLS=0, so the guest has no OpRegion and no VBT;
 * the test build (I915_TEST_VBT) supplies the captured VBT of the one test
 * machine instead.  Delete it once the GPU tests run on bare metal.  A
 * production kernel does not contain it.
 */
struct i915_vbt_pin {
	/* The name the blob is logged by. */
	const char *name;

	/* The captured bytes and their length. */
	const uint8_t *data;
	unsigned size;

	/* The PCI subsystem id of the machine the blob was read from. */
	uint16_t subsys_vendor;
	uint16_t subsys_device;

	/* The SHA-256 of the captured blob. */
	uint8_t sha256[32];
};

/*
 * The VBT captured from the OpRegion of the Dell Latitude 5330 the QEMU
 * passthrough test runs on (vendor/intel-vbt/README.md).
 *
 * It is platform data of the machine's vendor, and never changes.
 *
 * XXX: a test crutch.  The QEMU passthrough test runs the driver in a guest
 * whose firmware presents ASLS=0, so the guest has no OpRegion and no VBT;
 * the test build (I915_TEST_VBT) supplies the captured VBT of the one test
 * machine instead.  Delete it once the GPU tests run on bare metal.  A
 * production kernel does not contain it.
 */
static const uint8_t i915_vbt_test_dell_latitude_5330[] = {
#include "vendor/intel-vbt/dell-latitude-5330-1028-0b02.inc"
};

/*
 * The explicit VBT blobs, one row per machine the test build carries.
 *
 * The table never changes.
 *
 * XXX: a test crutch.  The QEMU passthrough test runs the driver in a guest
 * whose firmware presents ASLS=0, so the guest has no OpRegion and no VBT;
 * the test build (I915_TEST_VBT) supplies the captured VBT of the one test
 * machine instead.  Delete it once the GPU tests run on bare metal.  A
 * production kernel does not contain it.
 */
static const struct i915_vbt_pin i915_vbt_explicit_pins[] = {
	{ "vendor/intel-vbt/dell-latitude-5330-1028-0b02.inc",
		i915_vbt_test_dell_latitude_5330, sizeof(i915_vbt_test_dell_latitude_5330),
		0x1028u, 0x0b02u,
		{ 0x3b, 0xff, 0x4a, 0x09, 0x20, 0xd5, 0x5c, 0x9a, 0xee, 0x0e, 0xa3, 0xc6, 0x78, 0x90, 0x4f, 0x98,
		  0x2f, 0x86, 0x71, 0xc0, 0xe7, 0xb5, 0xbc, 0x97, 0xa3, 0x35, 0xe4, 0x29, 0xb2, 0x96, 0x24, 0xcd } },
};
#endif

/*
 * The parser's world of the display that is up.
 *
 * vbt.h declares the arena allocator and the message counters without a
 * world, because the Linux text reaches them through kzalloc() and the
 * message macros, and drv_i915_vbt_init(), _init_panel() and _fini() keep
 * their old arguments; they all reach the world through this binding.
 * drv_i915_vbt_world_create() sets it and drv_i915_vbt_world_destroy()
 * clears it, both on the probe thread; the driver drives one display.  NULL
 * means no world exists: the allocator refuses and nothing is counted.
 *
 * XXX: this is the one file-scope state left; it goes when the environment
 * hooks of vbt.h take the world as an argument.
 */
static struct i915_vbt_world *i915_vbt_bound_world;

static uint16_t i915_bios_rd16(const uint8_t *bytes);
static uint32_t i915_bios_rd32(const uint8_t *bytes);
static int i915_bios_oprom_get_vbt(struct i915_display *display, struct i915_pci *pci, const void **out, size_t *out_size);
static int i915_bios_oprom_lift(struct i915_display *display, const volatile uint8_t *rom, size_t rom_size, size_t found, const void **out, size_t *out_size);
static uint32_t i915_sha_rotr(uint32_t x, unsigned n);
#ifdef I915_TEST_VBT
static int i915_bios_explicit_blob_get(struct i915_display *display, struct i915_vbt_state *vbt, struct i915_pci *pci, const void **out, size_t *out_size);
static const uint8_t *i915_bios_explicit_pin_for(uint16_t vendor, uint16_t device);
#endif
static void i915_bios_choose_source(struct i915_display *display, struct i915_vbt_state *vbt, struct i915_pci *pci, int explicit_blob, const void **out, size_t *out_size, int *origin);
static void i915_bios_report(struct i915_vbt_state *vbt, int opregion_has_vbt, struct i915_trace *trace);
static uint32_t i915_opregion_le32(const uint8_t *bytes);
static uint64_t i915_opregion_le64(const uint8_t *bytes);
static void i915_vbt_append_decimal(struct drm_display_mode *mode, unsigned *length, unsigned value);
static void i915_vbt_flatten_encoder(struct i915_vbt_encoder *encoder, const struct intel_bios_encoder_data *devdata);
static void i915_vbt_flatten_panel(struct i915_vbt_world *world, const struct intel_panel *panel, struct i915_vbt_panel *out);
static u32 i915_get_blocksize_raw(const u8 *block_base);
static u32 i915_get_blocksize(const void *block_data);
static const void *i915_find_raw_section(const void *bdb_bytes, enum bdb_block_id section_id);
static u32 i915_raw_block_offset(const void *bdb, enum bdb_block_id section_id);
static const void *i915_bdb_find_section(struct drm_i915_private *i915, enum bdb_block_id section_id);
static size_t i915_lfp_data_min_size(struct drm_i915_private *i915);
static bool i915_validate_lfp_data_ptrs(const void *bdb, const struct bdb_lvds_lfp_data_ptrs *ptrs);
static bool i915_fixup_lfp_data_ptrs(const void *bdb, void *ptrs_block);
static int i915_make_lfp_data_ptr(struct lvds_lfp_data_ptr_table *table, int table_size, int total_size);
static void i915_next_lfp_data_ptr(struct lvds_lfp_data_ptr_table *next, const struct lvds_lfp_data_ptr_table *prev, int size);
static void *i915_generate_lfp_data_ptrs(struct drm_i915_private *i915, const void *bdb);
static void i915_init_bdb_block(struct drm_i915_private *i915, const void *bdb, enum bdb_block_id section_id, size_t min_size);
static void i915_init_bdb_blocks(struct drm_i915_private *i915, const void *bdb);
static void i915_fill_detail_timing_data(struct drm_i915_private *i915, struct drm_display_mode *panel_fixed_mode, const struct lvds_dvo_timing *dvo_timing);
static const struct lvds_dvo_timing *i915_get_lvds_dvo_timing(const struct bdb_lvds_lfp_data *data, const struct bdb_lvds_lfp_data_ptrs *ptrs, int index);
static const struct lvds_fp_timing *i915_get_lvds_fp_timing(const struct bdb_lvds_lfp_data *data, const struct bdb_lvds_lfp_data_ptrs *ptrs, int index);
static const struct lvds_pnp_id *i915_get_lvds_pnp_id(const struct bdb_lvds_lfp_data *data, const struct bdb_lvds_lfp_data_ptrs *ptrs, int index);
static const struct bdb_lvds_lfp_data_tail *i915_get_lfp_data_tail(const struct bdb_lvds_lfp_data *data, const struct bdb_lvds_lfp_data_ptrs *ptrs);
static void i915_dump_pnp_id(struct drm_i915_private *i915, const struct lvds_pnp_id *pnp_id, const char *name);
static int i915_opregion_get_panel_type(struct drm_i915_private *i915, const struct intel_bios_encoder_data *devdata, const struct drm_edid *drm_edid, bool use_fallback);
static int i915_vbt_get_panel_type(struct drm_i915_private *i915, const struct intel_bios_encoder_data *devdata, const struct drm_edid *drm_edid, bool use_fallback);
static int i915_pnpid_get_panel_type(struct drm_i915_private *i915, const struct intel_bios_encoder_data *devdata, const struct drm_edid *drm_edid, bool use_fallback);
static int i915_fallback_get_panel_type(struct drm_i915_private *i915, const struct intel_bios_encoder_data *devdata, const struct drm_edid *drm_edid, bool use_fallback);
static int i915_get_panel_type(struct drm_i915_private *i915, const struct intel_bios_encoder_data *devdata, const struct drm_edid *drm_edid, bool use_fallback);
static unsigned int i915_panel_bits(unsigned int value, int panel_type, int num_bits);
static bool i915_panel_bool(unsigned int value, int panel_type);
static void i915_parse_panel_options(struct drm_i915_private *i915, struct intel_panel *panel);
static void i915_parse_lfp_panel_dtd(struct drm_i915_private *i915, struct intel_panel *panel, const struct bdb_lvds_lfp_data *lvds_lfp_data, const struct bdb_lvds_lfp_data_ptrs *lvds_lfp_data_ptrs);
static void i915_parse_lfp_data(struct drm_i915_private *i915, struct intel_panel *panel);
static void i915_parse_generic_dtd(struct drm_i915_private *i915, struct intel_panel *panel);
static void i915_parse_lfp_backlight(struct drm_i915_private *i915, struct intel_panel *panel);
static int i915_bios_ssc_frequency(struct drm_i915_private *i915, bool alternate);
static void i915_parse_general_features(struct drm_i915_private *i915);
static const struct child_device_config *i915_child_device_ptr(const struct bdb_general_definitions *defs, int i);
static void i915_parse_driver_features(struct drm_i915_private *i915);
static void i915_parse_panel_driver_features(struct drm_i915_private *i915, struct intel_panel *panel);
static void i915_parse_power_conservation_features(struct drm_i915_private *i915, struct intel_panel *panel);
static void i915_parse_edp(struct drm_i915_private *i915, struct intel_panel *panel);
static void i915_parse_edp_link_params(struct drm_i915_private *i915, struct intel_panel *panel, const struct edp_fast_link_params *edp_link_params);
static u8 i915_translate_iboost(u8 val);
static u8 i915_map_ddc_pin(struct drm_i915_private *i915, u8 vbt_pin);
static u8 i915_dvo_port_type(u8 dvo_port);
static enum port i915_dvo_port_to_port_table(int n_ports, int n_dvo, const int port_mapping_table[][3], u8 dvo_port);
static enum port i915_dvo_port_to_port(struct drm_i915_private *i915, u8 dvo_port);
static enum port i915_dsi_dvo_port_to_port(struct drm_i915_private *i915, u8 dvo_port);
static enum port i915_bios_encoder_port(const struct intel_bios_encoder_data *devdata);
static int i915_parse_bdb_230_dp_max_link_rate(const int vbt_max_link_rate);
static int i915_parse_bdb_216_dp_max_link_rate(const int vbt_max_link_rate);
static int i915_bios_dp_max_link_rate(const struct intel_bios_encoder_data *devdata);
static int i915_bios_dp_max_lane_count(const struct intel_bios_encoder_data *devdata);
static void i915_sanitize_device_type(struct intel_bios_encoder_data *devdata, enum port port);
static void i915_sanitize_hdmi_level_shift(struct intel_bios_encoder_data *devdata, enum port port);
static bool i915_bios_encoder_supports_crt(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_supports_dvi(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_supports_hdmi(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_supports_dp(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_supports_edp(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_supports_dsi(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_is_lspcon(const struct intel_bios_encoder_data *devdata);
static int i915_bios_hdmi_max_tmds_clock(const struct intel_bios_encoder_data *devdata);
static bool i915_is_port_valid(struct drm_i915_private *i915, enum port port);
static void i915_print_ddi_port(const struct intel_bios_encoder_data *devdata);
static void i915_parse_ddi_port(struct intel_bios_encoder_data *devdata);
static bool i915_has_ddi_port_info(struct drm_i915_private *i915);
static void i915_parse_ddi_ports(struct drm_i915_private *i915);
static void i915_parse_general_definitions(struct drm_i915_private *i915);
static u8 i915_child_device_expected_size(struct drm_i915_private *i915);
static void i915_init_vbt_defaults(struct drm_i915_private *i915);
static void i915_init_vbt_panel_defaults(struct intel_panel *panel);
static void i915_init_vbt_missing_defaults(struct drm_i915_private *i915);
static const struct bdb_header *i915_get_bdb_header(const struct vbt_header *vbt);
static void i915_bios_init(struct drm_i915_private *i915);
static void i915_bios_parse_vbt(struct drm_i915_private *i915, const struct vbt_header *vbt);
static void i915_bios_init_panel(struct drm_i915_private *i915, struct intel_panel *panel, const struct intel_bios_encoder_data *devdata, const struct drm_edid *drm_edid, bool use_fallback);
static void i915_bios_init_panel_early(struct drm_i915_private *i915, struct intel_panel *panel, const struct intel_bios_encoder_data *devdata);
static void i915_bios_init_panel_late(struct drm_i915_private *i915, struct intel_panel *panel, const struct intel_bios_encoder_data *devdata, const struct drm_edid *drm_edid);
static void i915_bios_driver_remove(struct drm_i915_private *i915);
static void i915_bios_fini_panel(struct intel_panel *panel) __attribute__((unused));
static bool i915_bios_is_port_present(struct drm_i915_private *i915, enum port port) __attribute__((unused));
static bool i915_bios_encoder_supports_dp_dual_mode(const struct intel_bios_encoder_data *devdata);
static enum aux_ch i915_map_aux_ch(struct drm_i915_private *i915, u8 aux_channel);
static enum aux_ch i915_bios_dp_aux_ch(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_dp_has_shared_aux_ch(const struct intel_bios_encoder_data *devdata) __attribute__((unused));
static int i915_bios_dp_boost_level(const struct intel_bios_encoder_data *devdata);
static int i915_bios_hdmi_boost_level(const struct intel_bios_encoder_data *devdata);
static int i915_bios_hdmi_ddc_pin(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_supports_typec_usb(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_supports_tbt(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_lane_reversal(const struct intel_bios_encoder_data *devdata);
static bool i915_bios_encoder_hpd_invert(const struct intel_bios_encoder_data *devdata);
static const struct intel_bios_encoder_data *i915_bios_encoder_data_lookup(struct drm_i915_private *i915, enum port port);
static void i915_bios_for_each_encoder(struct drm_i915_private *i915, void (*func)(struct drm_i915_private *i915, const struct intel_bios_encoder_data *devdata)) __attribute__((unused));

/*
 * Creates the VBT parser's world of a display.
 *
 * The world starts zeroed, which is what the old file-scope state was at
 * boot.  Returns 0, EBUSY when a world already exists, or ENOMEM.
 */
int
drv_i915_vbt_world_create(
	struct i915_display *display)
{
	struct i915_vbt_world *world;

	/* The parser works for one display; a second world would steal the binding. */
	if (i915_vbt_bound_world != NULL)
		return EBUSY;

	/* Allocates the world zeroed. */
	world = kern_calloc(1, sizeof(*world));
	if (world == NULL)
		return ENOMEM;

	/* Hands the world to the display and to the hooks that are given no world. */
	display->vbt_world = world;
	i915_vbt_bound_world = world;

	/* Succeeded: the parser has its world. */
	return 0;
}

/*
 * Destroys the VBT parser's world of a display.
 *
 * The parse state must have been released (drv_i915_vbt_fini()); a display
 * without a world is left alone.
 */
void
drv_i915_vbt_world_destroy(
	struct i915_display *display)
{
	struct i915_vbt_world *world;

	/* Nothing to destroy without a world. */
	world = display->vbt_world;
	if (world == NULL)
		return;

	/* Withdraws the binding the hooks use. */
	if (i915_vbt_bound_world == world)
		i915_vbt_bound_world = NULL;

	/* Frees the world and forgets it. */
	display->vbt_world = NULL;
	kern_free(world);
}

/*
 * Tells whether a buffer holds a VBT whose header, BDB offset and BDB size fit.
 *
 * This is the byte-level check of the VBT acquisition; the Linux parser's
 * own check is drv_i915_bios_is_valid_vbt().  Reports 1 or 0.
 */
int
drv_i915_bios_is_valid_vbt_header(
	const void *buf,
	size_t size)
{
	const uint8_t *bytes;
	uint16_t vbt_size;
	uint32_t bdb_offset;
	uint16_t bdb_size;

	/* No buffer holds no VBT. */
	bytes = buf;
	if (bytes == NULL)
		return 0;

	/* A buffer shorter than the VBT header cannot hold one. */
	if (size < I915_VBT_HEADER_SIZE)
		return 0;

	/* The VBT header starts with the "$VBT" signature. */
	if (bytes[0] != '$')
		return 0;
	if (bytes[1] != 'V')
		return 0;
	if (bytes[2] != 'B')
		return 0;
	if (bytes[3] != 'T')
		return 0;

	/* The VBT must fit in the buffer. */
	vbt_size = i915_bios_rd16(bytes + I915_VBT_OFF_VBT_SIZE);
	if ((size_t)vbt_size > size)
		return 0;

	/* From here the VBT's own size bounds everything. */
	size = vbt_size;

	/* The BDB header must fit in the VBT. */
	bdb_offset = i915_bios_rd32(bytes + I915_VBT_OFF_BDB_OFFSET);
	if ((size_t)bdb_offset + I915_BDB_HEADER_SIZE > size)
		return 0;

	/* The whole BDB must fit in the VBT. */
	bdb_size = i915_bios_rd16(bytes + bdb_offset + I915_BDB_OFF_BDB_SIZE);
	if ((size_t)bdb_offset + bdb_size > size)
		return 0;

	/* Succeeded: the buffer holds a VBT. */
	return 1;
}

/*
 * Records the BDB version and the block count of a VBT buffer.
 *
 * The child devices are not extracted here; only the presence of the
 * blocks is counted.  Returns 0, or EINVAL when the buffer holds no valid
 * VBT.
 */
int
drv_i915_bios_process_vbt(
	struct i915_vbt_state *vbt,
	const void *buf,
	size_t size)
{
	const uint8_t *bytes;
	const uint8_t *bdb;
	uint32_t bdb_offset;
	uint16_t bdb_hdr_size;
	uint16_t bdb_size;
	uint16_t block_length;
	size_t offset;
	int valid;

	/*
	 * Refuses a buffer that holds no VBT.
	 *
	 * XXX: the old function returned -1 here, which is no errno; EINVAL
	 * is its positive errno now.
	 */
	valid = drv_i915_bios_is_valid_vbt_header(buf, size);
	if (valid == 0)
		return EINVAL;

	/* Reads the BDB header the VBT header points at. */
	bytes = buf;
	bdb_offset = i915_bios_rd32(bytes + I915_VBT_OFF_BDB_OFFSET);
	bdb = bytes + bdb_offset;
	vbt->version = i915_bios_rd16(bdb + I915_BDB_OFF_VERSION);
	bdb_hdr_size = i915_bios_rd16(bdb + I915_BDB_OFF_HEADER_SIZE);
	bdb_size = i915_bios_rd16(bdb + I915_BDB_OFF_BDB_SIZE);

	/* Walks the blocks (each an id byte, a 16-bit size and the data), counting them. */
	vbt->num_bdb_blocks = 0;
	offset = bdb_hdr_size;
	while (offset + 3u <= (size_t)bdb_size) {
		block_length = i915_bios_rd16(bdb + offset + 1u);
		vbt->num_bdb_blocks++;

		/* A block of size zero ends the walk. */
		if (block_length == 0u)
			break;

		offset += 3u + (size_t)block_length;
	}

	/* Records that a real VBT was found. */
	vbt->vbt_found = 1;

	/* Succeeded: the version and the block count are recorded. */
	return 0;
}

/*
 * Generates the default child devices a missing VBT stands for.
 *
 * init_vbt_missing_defaults(): ports A to F, skipping the ports whose ADL-P
 * PHY is Type-C (PHY F to I); ports A, B and C map to the combo PHYs A, B
 * and C, so three defaults result.
 */
void
drv_i915_bios_init_vbt_missing_defaults(
	struct i915_vbt_state *vbt)
{
	struct i915_vbt_child *child;
	unsigned port;
	unsigned phy;

	/* Generates one default child per non-Type-C port. */
	vbt->num_display_devices = 0;
	for (port = 0u; port <= 5u; port++) {
		/* Ports A to C are combo PHYs A to C; ports D to F are Type-C PHYs F to H. */
		if (port < 3u) {
			phy = (unsigned)PHY_A + port;
		} else {
			phy = (unsigned)PHY_F + (port - 3u);
		}

		/* A Type-C port is not generated here (intel_phy_is_tc()). */
		if (phy >= (unsigned)PHY_F && phy <= (unsigned)PHY_I)
			continue;

		/* The flattened table holds eight children. */
		if (vbt->num_display_devices >= I915_VBT_STATE_MAX_CHILDREN)
			break;

		/* Names the DVO port of the child. */
		child = &vbt->display_devices[vbt->num_display_devices++];
		child->port = port;
		if (port == 5u) {
			child->dvo_port = DVO_PORT_HDMIF;
		} else if (port == 4u) {
			child->dvo_port = DVO_PORT_HDMIE;
		} else {
			child->dvo_port = (uint8_t)(DVO_PORT_HDMIA + port);
		}

		/* Gives the child the output types the Linux defaults give it. */
		child->device_type = 0u;
		if (port != 0u && port != 4u)
			child->device_type |= DEVICE_TYPE_TMDS_DVI_SIGNALING;
		if (port != 4u)
			child->device_type |= DEVICE_TYPE_DISPLAYPORT_OUTPUT;
		if (port == 0u)
			child->device_type |= DEVICE_TYPE_INTERNAL_CONNECTOR;
	}

	/* Bypasses some minimum baseline VBT version checks, as Linux does. */
	vbt->version = 155u;
	vbt->missing_defaults_used = 1;
}

/*
 * Computes the SHA-256 (FIPS 180-4) of a byte range.
 *
 * The firmware display check hashes the OpRegion's VBT with it, and the
 * test build pins its explicit blob with it.
 */
void
drv_i915_sha256(
	const void *data,
	size_t len,
	uint8_t out[32])
{
	static const uint32_t round_constants[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};
	uint32_t hash[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
	const uint8_t *message;
	uint64_t bits;
	size_t total;
	size_t offset;
	size_t position;
	uint32_t schedule[64];
	uint32_t word;
	uint32_t sigma0;
	uint32_t sigma1;
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t choose;
	uint32_t majority;
	uint32_t temp1;
	uint32_t temp2;
	uint8_t byte;
	unsigned i;
	unsigned k;

	/* The padded message: the data, 0x80, zeros, and the 64-bit bit length, in 64-byte blocks. */
	message = data;
	bits = (uint64_t)len * 8u;
	total = ((len + 9u + 63u) / 64u) * 64u;

	/* Compresses every 64-byte block of the padded message. */
	for (offset = 0u; offset < total; offset += 64u) {
		/* Builds the first sixteen schedule words from the padded bytes. */
		for (i = 0u; i < 16u; i++) {
			word = 0u;
			for (k = 0u; k < 4u; k++) {
				position = offset + i * 4u + k;

				/* Picks the data byte, the terminator, the length or a zero pad. */
				if (position < len) {
					byte = message[position];
				} else if (position == len) {
					byte = 0x80u;
				} else if (position >= total - 8u) {
					byte = (uint8_t)(bits >> (8u * (total - 1u - position)));
				} else {
					byte = 0u;
				}

				word = (word << 8) | byte;
			}

			schedule[i] = word;
		}

		/* Extends the schedule to sixty-four words. */
		for (i = 16u; i < 64u; i++) {
			sigma0 = i915_sha_rotr(schedule[i - 15u], 7u) ^ i915_sha_rotr(schedule[i - 15u], 18u) ^ (schedule[i - 15u] >> 3);
			sigma1 = i915_sha_rotr(schedule[i - 2u], 17u) ^ i915_sha_rotr(schedule[i - 2u], 19u) ^ (schedule[i - 2u] >> 10);
			schedule[i] = schedule[i - 16u] + sigma0 + schedule[i - 7u] + sigma1;
		}

		/* Starts the rounds from the current hash. */
		a = hash[0];
		b = hash[1];
		c = hash[2];
		d = hash[3];
		e = hash[4];
		f = hash[5];
		g = hash[6];
		h = hash[7];

		/* Runs the sixty-four rounds. */
		for (i = 0u; i < 64u; i++) {
			sigma1 = i915_sha_rotr(e, 6u) ^ i915_sha_rotr(e, 11u) ^ i915_sha_rotr(e, 25u);
			choose = (e & f) ^ (~e & g);
			temp1 = h + sigma1 + choose + round_constants[i] + schedule[i];
			sigma0 = i915_sha_rotr(a, 2u) ^ i915_sha_rotr(a, 13u) ^ i915_sha_rotr(a, 22u);
			majority = (a & b) ^ (a & c) ^ (b & c);
			temp2 = sigma0 + majority;

			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}

		/* Adds the block's result into the hash. */
		hash[0] += a;
		hash[1] += b;
		hash[2] += c;
		hash[3] += d;
		hash[4] += e;
		hash[5] += f;
		hash[6] += g;
		hash[7] += h;
	}

	/* Writes the hash out big-endian. */
	for (i = 0u; i < 8u; i++) {
		out[i * 4u] = (uint8_t)(hash[i] >> 24);
		out[i * 4u + 1u] = (uint8_t)(hash[i] >> 16);
		out[i * 4u + 2u] = (uint8_t)(hash[i] >> 8);
		out[i * 4u + 3u] = (uint8_t)hash[i];
	}
}

/*
 * Writes one parser message to the kernel log.
 *
 * The message is the Linux text's format text; its arguments are not
 * rendered in the kernel (vbt.h).
 */
void
drv_i915_vbt_emit(
	const char *text)
{
	/* Logs the format text under the VBT's own tag. */
	kern_logf("i915: vbt: %s", text);
}

/*
 * Accepts a message and its arguments so the compiler checks them against
 * the format; it is never called.
 */
int
drv_i915_vbt_fmtcheck(
	const char *fmt,
	...)
{
	UNUSED_PARAMETER(fmt);

	/* Succeeded: there is nothing to do at run time. */
	return 0;
}

/*
 * Offers the OpRegion's validated VBT copy to the next VBT acquisition.
 *
 * The copy is not owned; NULL withdraws it.
 */
void
drv_i915_bios_set_opregion_vbt(
	struct i915_display *display,
	const void *buf,
	size_t size)
{
	/* Remembers the copy for i915_bios_choose_source(). */
	display->opregion_vbt_buf = buf;
	display->opregion_vbt_size = size;
}

/*
 * Finds the VBT and runs the Linux parser on it (intel_bios_init()).
 *
 * opregion_has_vbt reports whether the same start's OpRegion step holds a
 * usable VBT; explicit_blob asks for the pinned blob of this machine, which
 * only the test build (I915_TEST_VBT) carries and a production kernel
 * ignores.  The byte source is, in order, the explicit blob, the OpRegion's
 * copy, and the PCI ROM; with none the parser takes the missing-VBT
 * defaults.  vbt is filled with what was actually done.  Returns 0, or the
 * parser's refusal (a VBT state already live, or no parser world).
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
	int origin;
	unsigned i;
	int error;

	/* Starts the record empty. */
	vbt->version = 0u;
	vbt->vbt_found = 0;
	vbt->source = I915_VBT_SRC_NONE;
	vbt->missing_defaults_used = 0;
	vbt->num_bdb_blocks = 0u;
	vbt->num_display_devices = 0u;
	vbt->parsed_live = 0;
	vbt->blob_name = NULL;
	vbt->blob_size = 0u;
	vbt->blob_requested = 0;
	vbt->blob_found = 0;
	vbt->blob_hash_ok = 0;
	vbt->blob_subsys_ok = 0;
	vbt->blob_valid = 0;
	vbt->subsys_vendor = 0u;
	vbt->subsys_device = 0u;

	/* HAS_DISPLAY() holds on ADL-P; a device without a display skips the VBT. */
	vbt->has_display = 1;
	if (vbt->has_display == 0) {
		drv_i915_trace_record(trace, I915_VBT_TRACE_STAGE_P3, I915_TRACE_NOTE, "intel_bios_init:skip_no_display", 0u, 0u);
		return 0;
	}

	/* Chooses the VBT bytes. */
	i915_bios_choose_source(display, vbt, pci, explicit_blob, &vbt_buf, &vbt_size, &origin);

	/* Runs the Linux intel_bios_init() on the chosen bytes, or on none. */
	error = drv_i915_vbt_init(&vbt->parsed, vbt_buf, vbt_size, origin);
	if (error != 0) {
		kern_logf("i915: intel_bios_init: parser state busy/invalid rc=%d\n", error);
		return error;
	}

	/* Records what the parser found. */
	vbt->parsed_live = 1;
	vbt->vbt_found = 0;
	if (vbt_buf != NULL)
		vbt->vbt_found = 1;
	vbt->version = vbt->parsed.bdb_version;
	vbt->num_bdb_blocks = vbt->parsed.num_bdb_blocks;
	vbt->missing_defaults_used = vbt->parsed.missing_defaults_used;

	/*
	 * Flattens the child devices.  They come from one place: the real VBT
	 * when there is one, the Linux init_vbt_missing_defaults() otherwise,
	 * never a mix.
	 */
	for (i = 0u; i < vbt->parsed.n_encoders && vbt->num_display_devices < I915_VBT_STATE_MAX_CHILDREN; i++) {
		encoder = &vbt->parsed.enc[i];
		child = &vbt->display_devices[vbt->num_display_devices++];
		child->port = encoder->port >= 0 ? (unsigned)encoder->port : 0u;
		child->dvo_port = encoder->dvo_port;
		child->device_type = encoder->device_type;
	}

	/* Reports the source, the parse and the children. */
	i915_bios_report(vbt, opregion_has_vbt, trace);

	/* Succeeded: vbt holds the parse. */
	return 0;
}

/*
 * Finds the VBT and runs the Linux parser on it, without the explicit blob.
 */
int
drv_i915_bios_init(
	struct i915_display *display,
	struct i915_vbt_state *vbt,
	struct i915_pci *pci,
	int opregion_has_vbt,
	struct i915_trace *trace)
{
	int error;

	/* Runs the acquisition with the explicit blob not requested. */
	error = drv_i915_bios_init_ex(display, vbt, pci, opregion_has_vbt, 0, trace);
	if (error != 0)
		return error;

	/* Succeeded: vbt holds the parse. */
	return 0;
}

/*
 * Releases what the Linux parser allocated (intel_bios_driver_remove()).
 *
 * A record without a live parse is left alone.
 */
void
drv_i915_bios_driver_remove(
	struct i915_vbt_state *vbt)
{
	/* Nothing was parsed, or it was already released. */
	if (vbt == NULL)
		return;
	if (vbt->parsed_live == 0)
		return;

	/* Releases the parser's lists and arena. */
	drv_i915_vbt_fini(&vbt->parsed);
	vbt->parsed_live = 0;
}

#ifdef I915_TEST_VBT
/*
 * Returns the pinned SHA-256 of this machine's explicit blob, or NULL when
 * the build carries no row for the machine.
 *
 * The machine is the one the last VBT acquisition read the PCI subsystem id
 * of; the firmware display check compares the OpRegion's VBT against it.
 *
 * XXX: a test crutch.  The QEMU passthrough test runs the driver in a guest
 * whose firmware presents ASLS=0, so the guest has no OpRegion and no VBT;
 * the test build (I915_TEST_VBT) supplies the captured VBT of the one test
 * machine instead.  Delete it once the GPU tests run on bare metal.  A
 * production kernel does not contain it.
 */
const uint8_t *
drv_i915_vbt_explicit_pin(
	struct i915_display *display)
{
	const uint8_t *pin;

	/* Looks the remembered machine up in the pin table. */
	pin = i915_bios_explicit_pin_for(display->pinned_vendor, display->pinned_device);

	/* Succeeded: reports the pin, or NULL for a machine without one. */
	return pin;
}
#endif

/*
 * Reads where a copy of the OpRegion keeps its VBT (intel_opregion_setup()).
 *
 * op is a copy of the 8 KiB OpRegion found at asls.  The candidate Linux
 * tries first is the RVDA (a physical address, or an offset from the
 * OpRegion from version 2.1), else mailbox #4; the caller maps and validates
 * it.  The runtime mailboxes are recorded as the firmware left them.
 * Returns 0, or EINVAL when the copy is missing or too short or has the
 * wrong signature (nothing is filled then).
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

	/*
	 * Refuses a missing or short copy.
	 *
	 * XXX: the old function returned -1 for every refusal, which is no
	 * errno; EINVAL is its positive errno now.
	 */
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

		/* An offset inside the OpRegion is what Linux warns about. */
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

/*
 * Returns zeroed memory from the parser's arena, or NULL.
 *
 * Allocations are rounded up to 16 bytes.  An empty request or an
 * exhausted arena is refused and counted as an allocation failure.
 */
void *
drv_i915_vbt_zalloc(
	size_t bytes)
{
	struct i915_vbt_world *world;
	unsigned need;
	void *memory;

	/* Without a world there is no arena. */
	world = i915_vbt_bound_world;
	if (world == NULL)
		return NULL;

	/* Refuses an empty request, or one the rest of the arena cannot hold. */
	need = (unsigned)((bytes + 15u) & ~(size_t)15u);
	if (bytes == 0u || need > I915_VBT_ARENA_BYTES - world->vbt_arena_used) {
		world->vbt_alloc_failures++;
		return NULL;
	}

	/* Hands out the next piece of the arena and moves the high-water mark. */
	memory = &world->vbt_arena[world->vbt_arena_used];
	world->vbt_arena_used += need;
	if (world->vbt_arena_used > world->vbt_arena_peak)
		world->vbt_arena_peak = world->vbt_arena_used;

	/* Clears the piece. */
	kern_memset(memory, 0, need);

	/* Succeeded: the memory lives until the arena is released. */
	return memory;
}

/*
 * Releases parser memory.
 *
 * The arena is a bump arena, so a single release does nothing;
 * drv_i915_vbt_fini() releases everything.
 */
void
drv_i915_vbt_free(
	void *p)
{
	UNUSED_PARAMETER(p);
}

/*
 * Returns the validated VBT bytes the parser is to read, or NULL for none.
 */
const void *
drv_i915_vbt_provider_get(
	struct drm_i915_private *i915)
{
	struct i915_vbt_world *world;

	/* The device is the world's own parser device. */
	world = container_of(i915, struct i915_vbt_world, vbt_i915);

	/* Succeeded: reports the bytes drv_i915_vbt_init() chose. */
	return world->vbt_provider_bytes;
}

/*
 * Sets how verbose the parser's messages are: 0 errors only, 1 with
 * information, 2 with debug.
 */
void
drv_i915_vbt_set_log_level(
	int level)
{
	struct i915_vbt_world *world;

	/* Without a world there is no level to set. */
	world = i915_vbt_bound_world;
	if (world == NULL)
		return;

	/* Records the level. */
	world->vbt_log_level = level;
}

/*
 * Counts one parser message and shows its format text when its level is
 * shown.
 */
void
drv_i915_vbt_note(
	int level,
	const char *fmt)
{
	struct i915_vbt_world *world;

	/* Without a world nothing is counted or shown. */
	world = i915_vbt_bound_world;
	if (world == NULL)
		return;

	/* Every error-level message is counted. */
	if (level == I915_VBT_LOG_ERR)
		world->vbt_log_errors++;

	/* Shows the message when its level is shown. */
	if (level <= world->vbt_log_level)
		drv_i915_vbt_emit(fmt);
}

/*
 * Counts an error-level message and tells whether a level is shown.
 */
int
drv_i915_vbt_log_enabled(
	int level)
{
	struct i915_vbt_world *world;

	/* Without a world nothing is counted or shown. */
	world = i915_vbt_bound_world;
	if (world == NULL)
		return 0;

	/* Every error-level message is counted. */
	if (level == I915_VBT_LOG_ERR)
		world->vbt_log_errors++;

	/* A level above the shown one is not shown. */
	if (level > world->vbt_log_level)
		return 0;

	/* Succeeded: the level is shown. */
	return 1;
}

/*
 * Names a mode "<hdisplay>x<vdisplay>" (the parser's drm_mode_set_name()).
 *
 * The name is written without a formatted print and is cut to the name
 * buffer.
 */
void
drv_i915_vbt_drm_mode_set_name(
	struct drm_display_mode *mode)
{
	unsigned length;

	/* Writes the width, the separator and the height. */
	length = 0u;
	i915_vbt_append_decimal(mode, &length, mode->hdisplay);
	if (length + 1u < sizeof(mode->name))
		mode->name[length++] = 'x';
	i915_vbt_append_decimal(mode, &length, mode->vdisplay);

	/* Terminates the name. */
	mode->name[length] = '\0';
}

/*
 * Tells whether a buffer holds a VBT the parser accepts
 * (intel_bios_is_valid_vbt()); reports 1 or 0.
 */
int
drv_i915_vbt_validate(
	const void *bytes,
	size_t size)
{
	bool valid;

	/* Asks the Linux check. */
	valid = drv_i915_bios_is_valid_vbt(bytes, size);
	if (!valid)
		return 0;

	/* Succeeded: the buffer holds a valid VBT. */
	return 1;
}

/*
 * Runs the Linux intel_bios_init() on validated bytes and flattens the result.
 *
 * NULL bytes or a zero size mean no VBT: the parser takes the missing-VBT
 * defaults.  Returns 0, EINVAL for no result record or no parser world, or
 * EBUSY when a parse is already live (one display).
 *
 * XXX: the old function returned the Linux numbers -22 and -16, which the
 * acquisition logged; the positive zedBSD EINVAL and EBUSY are logged now.
 */
int
drv_i915_vbt_init(
	struct i915_vbt *v,
	const void *bytes,
	size_t size,
	int origin)
{
	struct i915_vbt_world *world;
	struct intel_bios_encoder_data *devdata;
	struct bdb_block_entry *entry;
	const struct vbt_header *header;
	unsigned i;

	/* Refuses a missing result record. */
	if (v == NULL)
		return EINVAL;

	/* Refuses to parse without a world. */
	world = i915_vbt_bound_world;
	if (world == NULL)
		return EINVAL;

	/* One parse is live at a time. */
	if (world->vbt_live != NULL)
		return EBUSY;

	/* Starts the result, the parser device and the counters empty. */
	kern_memset(v, 0, sizeof(*v));
	kern_memset(&world->vbt_i915, 0, sizeof(world->vbt_i915));
	world->vbt_arena_used = 0u;
	world->vbt_arena_peak = 0u;
	world->vbt_alloc_failures = 0u;
	world->vbt_log_errors = 0u;

	/* Records the bytes and their signature, or that there are none. */
	if (bytes != NULL && size != 0u) {
		header = bytes;
		v->bytes = bytes;
		v->size = size;
		v->origin = origin;
		for (i = 0u; i < sizeof(header->signature) && i + 1u < sizeof(v->signature); i++)
			v->signature[i] = (char)header->signature[i];
	} else {
		bytes = NULL;
		v->origin = I915_VBT_ORIGIN_NONE;
	}

	/* Hands the bytes to the parser and marks the parse live. */
	world->vbt_provider_bytes = bytes;
	world->vbt_live = v;

	/* Runs the Linux intel_bios_init(). */
	i915_bios_init(&world->vbt_i915);

	/* Copies the general features the display consumes. */
	v->bdb_version = world->vbt_i915.display.vbt.version;
	v->missing_defaults_used = 0;
	if (bytes == NULL)
		v->missing_defaults_used = 1;
	v->int_lvds_support = (int)world->vbt_i915.display.vbt.int_lvds_support;
	v->display_clock_mode = (int)world->vbt_i915.display.vbt.display_clock_mode;
	v->lvds_use_ssc = (int)world->vbt_i915.display.vbt.lvds_use_ssc;
	v->lvds_ssc_freq = world->vbt_i915.display.vbt.lvds_ssc_freq;
	v->crt_ddc_pin = world->vbt_i915.display.vbt.crt_ddc_pin;

	/* Counts the BDB blocks the parser kept. */
	list_for_each_entry(entry, &world->vbt_i915.display.vbt.bdb_blocks, node) {
		v->num_bdb_blocks++;
	}

	/* Flattens each child device, up to the table size. */
	list_for_each_entry(devdata, &world->vbt_i915.display.vbt.display_devices, node) {
		if (v->n_encoders >= I915_VBT_MAX_ENCODERS)
			break;

		i915_vbt_flatten_encoder(&v->enc[v->n_encoders++], devdata);
	}

	/* Copies the arena and message counters. */
	v->arena_used = world->vbt_arena_used;
	v->arena_peak = world->vbt_arena_peak;
	v->alloc_failures = world->vbt_alloc_failures;
	v->log_errors = world->vbt_log_errors;

	/* Marks the result usable. */
	v->inited = 1;

	/* Succeeded: v holds the parse. */
	return 0;
}

/*
 * Returns the flattened child device of a port, or NULL when the VBT names
 * none (or no parse is usable).
 */
const struct i915_vbt_encoder *
drv_i915_vbt_encoder_for_port(
	const struct i915_vbt *v,
	int port)
{
	unsigned i;

	/* Without a usable parse there is no child. */
	if (v == NULL)
		return NULL;
	if (v->inited == 0)
		return NULL;

	/* Looks for the child on the port. */
	for (i = 0u; i < v->n_encoders; i++) {
		if (v->enc[i].port == port)
			return &v->enc[i];
	}

	/* The VBT names no child on the port. */
	return NULL;
}

/*
 * Initializes the panel data of the child on a port (intel_bios_init_panel_late()).
 *
 * The early pass runs without an EDID; when it finds no panel type the late
 * pass runs with edid128 (128 bytes, or NULL for the Linux fallback panel
 * type), as intel_edp_init_connector() does.  Returns 0, EINVAL when v is not
 * the live parse, ENODEV when the VBT names no child on the port, or ENODATA
 * when no panel type was found.
 *
 * XXX: the old function returned the Linux numbers -22, -19 and -61; the
 * callers only compare with 0.
 */
int
drv_i915_vbt_init_panel(
	struct i915_vbt *v,
	int port,
	const uint8_t *edid128,
	struct i915_vbt_panel *out)
{
	struct i915_vbt_world *world;
	const struct intel_bios_encoder_data *devdata;
	struct intel_panel *panel;
	struct drm_edid drm_edid;
	const struct drm_edid *late_edid;

	/* Refuses anything but the live, usable parse. */
	world = i915_vbt_bound_world;
	if (v == NULL)
		return EINVAL;
	if (out == NULL)
		return EINVAL;
	if (world == NULL)
		return EINVAL;
	if (v != world->vbt_live)
		return EINVAL;
	if (v->inited == 0)
		return EINVAL;

	/* Finds the child on the port. */
	kern_memset(out, 0, sizeof(*out));
	devdata = i915_bios_encoder_data_lookup(&world->vbt_i915, (enum port)port);
	if (devdata == NULL)
		return ENODEV;

	/* Starts the panel with its type not known yet (intel_panel_init_alloc()). */
	panel = &world->i915_vbt_init_panel_panel;
	kern_memset(panel, 0, sizeof(*panel));
	panel->vbt.panel_type = -1;
	drm_edid.edid = (const struct edid *)edid128;

	/* The early pass: without an EDID. */
	i915_bios_init_panel_early(&world->vbt_i915, panel, devdata);

	/* The late pass: with the EDID read over AUX, when the early pass found no panel type. */
	if (panel->vbt.panel_type < 0) {
		if (edid128 != NULL) {
			late_edid = &drm_edid;
		} else {
			late_edid = NULL;
		}

		i915_bios_init_panel_late(&world->vbt_i915, panel, devdata, late_edid);
	}

	/* No pass found a panel type. */
	if (panel->vbt.panel_type < 0)
		return ENODATA;

	/* Copies what the panel data says. */
	i915_vbt_flatten_panel(world, panel, out);

	/* Copies the arena and message counters. */
	v->arena_used = world->vbt_arena_used;
	v->arena_peak = world->vbt_arena_peak;
	v->alloc_failures = world->vbt_alloc_failures;
	v->log_errors = world->vbt_log_errors;

	/* Succeeded: out holds the panel data. */
	return 0;
}

/*
 * Releases the live parse (intel_bios_driver_remove()) and the arena.
 *
 * A record that is not the live parse is left alone.
 */
void
drv_i915_vbt_fini(
	struct i915_vbt *v)
{
	struct i915_vbt_world *world;

	/* Only the live parse is released. */
	world = i915_vbt_bound_world;
	if (v == NULL)
		return;
	if (world == NULL)
		return;
	if (v != world->vbt_live)
		return;

	/* Releases the parser's lists. */
	i915_bios_driver_remove(&world->vbt_i915);

	/* Releases the bytes and the arena, and ends the live parse. */
	world->vbt_provider_bytes = NULL;
	world->vbt_arena_used = 0u;
	world->vbt_live = NULL;

	/* Leaves the record empty. */
	v->inited = 0;
	v->n_encoders = 0u;
	v->bytes = NULL;
}

/*
 * Tells whether a buffer holds a valid VBT (intel_bios_is_valid_vbt()).
 *
 * The VBT header must be complete and start with "$VBT", the VBT must fit
 * in the buffer, and the BDB header and the whole BDB must fit in the VBT.
 */
bool
drv_i915_bios_is_valid_vbt(
	const void *buf,
	size_t size)
{
	const struct vbt_header *vbt;
	const struct bdb_header *bdb;
	bool overflows;
	int compared;

	/* No buffer holds no VBT. */
	vbt = buf;
	if (!vbt)
		return false;

	/* The VBT header must be complete. */
	if (sizeof(struct vbt_header) > size) {
		I915_VBT_DRM_DEBUG_DRIVER("VBT header incomplete\n");
		return false;
	}

	/* The VBT header starts with "$VBT". */
	compared = kern_memcmp(vbt->signature, "$VBT", 4);
	if (compared != 0) {
		I915_VBT_DRM_DEBUG_DRIVER("VBT invalid signature\n");
		return false;
	}

	/* The VBT must fit in the buffer. */
	if (vbt->vbt_size > size) {
		I915_VBT_DRM_DEBUG_DRIVER("VBT incomplete (vbt_size overflows)\n");
		return false;
	}

	/* From here the VBT's own size bounds everything. */
	size = vbt->vbt_size;

	/* The BDB header must fit in the VBT. */
	overflows = range_overflows_t(size_t, vbt->bdb_offset, sizeof(struct bdb_header), size);
	if (overflows) {
		I915_VBT_DRM_DEBUG_DRIVER("BDB header incomplete\n");
		return false;
	}

	/* The whole BDB must fit in the VBT. */
	bdb = i915_get_bdb_header(vbt);
	overflows = range_overflows_t(size_t, vbt->bdb_offset, bdb->bdb_size, size);
	if (overflows) {
		I915_VBT_DRM_DEBUG_DRIVER("BDB incomplete\n");
		return false;
	}

	/* Succeeded: the buffer holds a valid VBT. */
	return true;
}

/*
 * Returns the HDMI/DVI buffer translation index the VBT names for a child,
 * or -1 (intel_bios_hdmi_level_shift()).
 */
int
drv_i915_bios_hdmi_level_shift(
	const struct intel_bios_encoder_data *devdata)
{
	int display_ver;

	/* A missing child, a VBT older than 158 or display version 14 name none. */
	if (!devdata)
		return -1;
	if (devdata->i915->display.vbt.version < 158)
		return -1;
	display_ver = i915_vbt_display_ver(devdata->i915);
	if (display_ver >= 14)
		return -1;

	/* Succeeded: reports the child's level shifter value. */
	return devdata->child.hdmi_level_shifter_value;
}

/* Reads a little-endian 16-bit value. */
static uint16_t
i915_bios_rd16(
	const uint8_t *bytes)
{
	/* Succeeded: reports the two bytes, low first. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Reads a little-endian 32-bit value. */
static uint32_t
i915_bios_rd32(
	const uint8_t *bytes)
{
	/* Succeeded: reports the four bytes, low first. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*
 * Lifts a validated VBT out of the PCI expansion ROM.
 *
 * Returns 1 with the VBT copied into display->i915_vbt_buf, or 0 on a
 * genuine absence (no ROM BAR, unmappable, or no valid "$VBT").  The ROM
 * decode is left as it was found.
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
	if (base == 0u)
		return 0;

	/* Sizes the ROM window: writes ones to the address bits, keeps the enable, restores. */
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, I915_VBT_ROM_ADDRESS_MASK | (original & I915_VBT_ROM_ENABLE));
	probed = drv_i915_pci_read32(pci, I915_VBT_PCI_ROM_BASE) & I915_VBT_ROM_ADDRESS_MASK;
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);
	if (probed == 0u)
		return 0;

	/* Bounds the mapping. */
	size32 = (~probed) + 1u;
	rom_size = (size_t)size32;
	if (rom_size > I915_VBT_ROM_MAP_MAX)
		rom_size = I915_VBT_ROM_MAP_MAX;

	/* Enables the ROM decode and maps the ROM. */
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, base | I915_VBT_ROM_ENABLE);
	rom = NULL;
	map_error = hal_space_map_device((hal_physaddr_t)base, rom_size, HAL_SPACE_READ, &rom);
	if (map_error != HAL_OK) {
		drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);
		return 0;
	}
	if (rom == NULL) {
		drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);
		return 0;
	}

	/* Scans the ROM for the "$VBT" signature on 4-byte boundaries. */
	bytes = (const volatile uint8_t *)rom;
	found = -1;
	for (i = 0u; i + 4u < rom_size; i += 4u) {
		if (bytes[i] != '$')
			continue;
		if (bytes[i + 1u] != 'V')
			continue;
		if (bytes[i + 2u] != 'B')
			continue;
		if (bytes[i + 3u] != 'T')
			continue;

		found = (int)i;
		break;
	}

	/* Copies and validates the VBT the signature starts. */
	lifted = 0;
	if (found >= 0)
		lifted = i915_bios_oprom_lift(display, bytes, rom_size, (size_t)found, out, out_size);

	/* Unmaps the ROM and leaves its decode as it was found. */
	(void)hal_space_unmap_device(rom, rom_size);
	drv_i915_pci_write32(pci, I915_VBT_PCI_ROM_BASE, original);

	/* A signature without a valid VBT behind it is an absence. */
	if (lifted == 0)
		return 0;

	/* Succeeded: *out is the VBT copy. */
	return 1;
}

/*
 * Copies the VBT a ROM signature starts into display->i915_vbt_buf and
 * validates it; reports 1 with *out and *out_size set, or 0.
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
	high = rom[found + I915_VBT_OFF_VBT_SIZE + 1u];
	vbt_size = (uint16_t)(low | (high << 8));

	/* The VBT must be nonempty, fit in the ROM and fit in the copy buffer. */
	if (vbt_size == 0u)
		return 0;
	if ((size_t)vbt_size > available)
		return 0;
	if ((size_t)vbt_size > I915_VBT_MAX)
		return 0;

	/* Copies the VBT out of the ROM. */
	for (k = 0u; k < (size_t)vbt_size; k++)
		display->i915_vbt_buf[k] = rom[found + k];

	/* Validates the copy. */
	valid = drv_i915_bios_is_valid_vbt_header(display->i915_vbt_buf, vbt_size);
	if (valid == 0)
		return 0;

	/* Hands the copy out. */
	*out = display->i915_vbt_buf;
	*out_size = vbt_size;

	/* Succeeded: the copy is a valid VBT. */
	return 1;
}

/* Rotates a 32-bit value right by n bits. */
static uint32_t
i915_sha_rotr(
	uint32_t x,
	unsigned n)
{
	/* Succeeded: reports the rotated value. */
	return (x >> n) | (x << (32u - n));
}

#ifdef I915_TEST_VBT
/*
 * Supplies this machine's explicit blob when the test build carries one for
 * it, it matches its pin, and it holds a valid VBT.
 *
 * Records every step in vbt; reports 1 with *out and *out_size set, or 0
 * (nothing is used, and the caller reports the absence).
 *
 * XXX: a test crutch.  The QEMU passthrough test runs the driver in a guest
 * whose firmware presents ASLS=0, so the guest has no OpRegion and no VBT;
 * the test build (I915_TEST_VBT) supplies the captured VBT of the one test
 * machine instead.  Delete it once the GPU tests run on bare metal.  A
 * production kernel does not contain it.
 */
static int
i915_bios_explicit_blob_get(
	struct i915_display *display,
	struct i915_vbt_state *vbt,
	struct i915_pci *pci,
	const void **out,
	size_t *out_size)
{
	const struct i915_vbt_pin *pin;
	unsigned row;
	unsigned i;

	/* Reads which machine this is, and remembers it for the pin lookup. */
	vbt->blob_requested = 1;
	vbt->subsys_vendor = drv_i915_pci_read16(pci, I915_VBT_PCI_SUBSYS_VENDOR);
	vbt->subsys_device = drv_i915_pci_read16(pci, I915_VBT_PCI_SUBSYS_DEVICE);
	display->pinned_vendor = vbt->subsys_vendor;
	display->pinned_device = vbt->subsys_device;
	vbt->blob_name = i915_vbt_explicit_pins[0].name;

	/* Looks for the row of this machine. */
	for (row = 0u; row < ARRAY_SIZE(i915_vbt_explicit_pins); row++) {
		pin = &i915_vbt_explicit_pins[row];
		if (vbt->subsys_vendor != pin->subsys_vendor)
			continue;
		if (vbt->subsys_device != pin->subsys_device)
			continue;

		/* Takes the machine's blob, which the test build carries. */
		vbt->blob_subsys_ok = 1;
		vbt->blob_name = pin->name;
		vbt->blob_found = 1;
		vbt->blob_size = pin->size;

		/* Hashes the blob and compares it with the pin. */
		drv_i915_sha256(pin->data, pin->size, vbt->blob_sha256);
		vbt->blob_hash_ok = 1;
		for (i = 0u; i < 32u; i++) {
			if (vbt->blob_sha256[i] != pin->sha256[i])
				vbt->blob_hash_ok = 0;
		}

		/* A blob that differs from the pin, or holds no valid VBT, is not used. */
		vbt->blob_valid = drv_i915_vbt_validate(pin->data, pin->size);
		if (vbt->blob_hash_ok == 0 || vbt->blob_valid == 0)
			return 0;

		/* The blob is static read-only data and stays valid for the kernel lifetime. */
		*out = pin->data;
		*out_size = pin->size;

		/* Succeeded: the pinned blob is used. */
		return 1;
	}

	/* No row for this machine: nothing is used. */
	return 0;
}
#endif

/*
 * Chooses the VBT bytes in the Linux intel_opregion_get_vbt() order.
 *
 * The explicit blob of the test build stands where Linux has its VBT
 * firmware file, and is taken only when requested and pinned to this
 * machine.  It is not an OpRegion: ASLS and the OpRegion state stay as the
 * firmware reported them.  A production kernel has no explicit blob.
 * Then the OpRegion's validated copy (RVDA or mailbox #4), then the PCI ROM
 * (ADL-P is not DGFX, so no SPI flash).  With none, *out is NULL and
 * *origin is I915_VBT_ORIGIN_NONE.
 */
static void
i915_bios_choose_source(
	struct i915_display *display,
	struct i915_vbt_state *vbt,
	struct i915_pci *pci,
	int explicit_blob,
	const void **out,
	size_t *out_size,
	int *origin)
{
	int lifted;
	int valid;

#ifndef I915_TEST_VBT
	UNUSED_PARAMETER(explicit_blob);

#endif
	/* Starts with no bytes. */
	*out = NULL;
	*out_size = 0u;
	*origin = I915_VBT_ORIGIN_NONE;

#ifdef I915_TEST_VBT
	/*
	 * The explicit blob, when the build asks for it.
	 *
	 * XXX: a test crutch.  The QEMU passthrough test runs the driver in a guest
	 * whose firmware presents ASLS=0, so the guest has no OpRegion and no VBT;
	 * the test build (I915_TEST_VBT) supplies the captured VBT of the one test
	 * machine instead.  Delete it once the GPU tests run on bare metal.  A
	 * production kernel does not contain it.
	 */
	if (explicit_blob != 0) {
		lifted = i915_bios_explicit_blob_get(display, vbt, pci, out, out_size);
		if (lifted != 0) {
			vbt->source = I915_VBT_SRC_EXPLICIT_BLOB;
			*origin = I915_VBT_ORIGIN_EXPLICIT_BLOB;
		}
	}

#endif

	/* The OpRegion's copy, when it holds a valid VBT. */
	if (*out == NULL && display->opregion_vbt_buf != NULL) {
		valid = drv_i915_vbt_validate(display->opregion_vbt_buf, display->opregion_vbt_size);
		if (valid != 0) {
			*out = display->opregion_vbt_buf;
			*out_size = display->opregion_vbt_size;
			vbt->source = I915_VBT_SRC_OPREGION;
			*origin = I915_VBT_ORIGIN_OPREGION;
		}
	}

	/* The PCI ROM, when the parser accepts what it holds. */
	if (*out == NULL) {
		lifted = i915_bios_oprom_get_vbt(display, pci, out, out_size);
		if (lifted != 0) {
			valid = drv_i915_vbt_validate(*out, *out_size);
			if (valid != 0) {
				vbt->source = I915_VBT_SRC_PCI_ROM;
				*origin = I915_VBT_ORIGIN_PCI_ROM;
			} else {
				*out = NULL;
				*out_size = 0u;
			}
		}
	}
}

/*
 * Records the acquisition in the trace and logs the source, the parse, the
 * explicit blob and every child device.
 */
static void
i915_bios_report(
	struct i915_vbt_state *vbt,
	int opregion_has_vbt,
	struct i915_trace *trace)
{
	const struct i915_vbt_encoder *encoder;
	char port_letter;
	unsigned i;

	/* Records whether a VBT was parsed or is absent. */
	if (!vbt->vbt_found) {
		drv_i915_trace_record(trace, I915_VBT_TRACE_STAGE_P3, I915_TRACE_NOTE,
			"intel_bios_init:vbt_absent", (uint64_t)(unsigned)vbt->source, 0u);
	} else {
		drv_i915_trace_record(trace, I915_VBT_TRACE_STAGE_P3, I915_TRACE_ACQUIRE,
			"intel_bios_init:vbt_parsed", (uint64_t)vbt->version,
			(uint64_t)vbt->num_bdb_blocks);
	}

	/* Logs the source and the parse. */
	kern_logf("i915: intel_bios_init: source=%d vbt_found=%d version=%u "
		"bdb_blocks=%u child_devices=%u missing_defaults=%d parser_errors=%u arena_peak=%u\n",
		vbt->source, vbt->vbt_found, (unsigned)vbt->version,
		vbt->num_bdb_blocks, vbt->num_display_devices, vbt->missing_defaults_used,
		vbt->parsed.log_errors, vbt->parsed.arena_peak);

	/* Logs what became of the explicit blob, when it was requested. */
	if (vbt->blob_requested) {
		kern_logf("i915: VBT explicit blob: name=%s requested=1 found=%d size=%u "
			"sha256=%02x%02x%02x%02x%02x%02x%02x%02x.. hash_ok=%d valid=%d subsys=%04x:%04x "
			"subsys_ok=%d used=%d (explicit supply; OpRegion present=%d is unchanged)\n",
			vbt->blob_name, vbt->blob_found, vbt->blob_size,
			vbt->blob_sha256[0], vbt->blob_sha256[1], vbt->blob_sha256[2], vbt->blob_sha256[3],
			vbt->blob_sha256[4], vbt->blob_sha256[5], vbt->blob_sha256[6], vbt->blob_sha256[7],
			vbt->blob_hash_ok, vbt->blob_valid, vbt->subsys_vendor, vbt->subsys_device,
			vbt->blob_subsys_ok, vbt->source == I915_VBT_SRC_EXPLICIT_BLOB, opregion_has_vbt);
	}

	/* Logs every child device the parser flattened. */
	for (i = 0u; i < vbt->parsed.n_encoders; i++) {
		encoder = &vbt->parsed.enc[i];
		port_letter = encoder->port >= 0 ? (char)('A' + encoder->port) : '-';
		kern_logf("i915: VBT child[%u]: port=%c dvo_port=%u type=0x%04x aux_ch=%d ddc_pin=%d "
			"dp=%d edp=%d hdmi=%d typec=%d tbt=%d max_lanes=%d max_rate=%d hpd_invert=%d lane_reversal=%d\n",
			i, port_letter, encoder->dvo_port, encoder->device_type, encoder->aux_ch,
			encoder->ddc_pin, encoder->supports_dp, encoder->supports_edp, encoder->supports_hdmi,
			encoder->supports_typec_usb, encoder->supports_tbt, encoder->dp_max_lane_count,
			encoder->dp_max_link_rate, encoder->hpd_invert, encoder->lane_reversal);
	}
}

#ifdef I915_TEST_VBT
/* Returns the pinned SHA-256 of a machine's row, or NULL for a machine without one. */
static const uint8_t *
i915_bios_explicit_pin_for(
	uint16_t vendor,
	uint16_t device)
{
	unsigned row;

	/* Looks for the machine's row. */
	for (row = 0u; row < ARRAY_SIZE(i915_vbt_explicit_pins); row++) {
		if (i915_vbt_explicit_pins[row].subsys_vendor != vendor)
			continue;
		if (i915_vbt_explicit_pins[row].subsys_device != device)
			continue;

		/* Succeeded: reports the row's pin. */
		return i915_vbt_explicit_pins[row].sha256;
	}

	/* The build carries no row for the machine. */
	return NULL;
}
#endif

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

/* Appends a decimal number to a mode name, as far as the name buffer holds it. */
static void
i915_vbt_append_decimal(
	struct drm_display_mode *mode,
	unsigned *length,
	unsigned value)
{
	char digits[8];
	unsigned count;

	/* Writes the digits lowest first, up to the digit buffer. */
	count = 0u;
	do {
		digits[count++] = (char)('0' + value % 10u);
		value /= 10u;
	} while (value != 0u && count < sizeof(digits));

	/* Copies them highest first, leaving room for the terminator. */
	while (count > 0u && *length + 1u < sizeof(mode->name))
		mode->name[(*length)++] = digits[--count];
}

/* Copies what the parser says about one child device into its flat record. */
static void
i915_vbt_flatten_encoder(
	struct i915_vbt_encoder *encoder,
	const struct intel_bios_encoder_data *devdata)
{
	/* Copies where the child is and how it is wired. */
	encoder->port = (int)i915_bios_encoder_port(devdata);
	encoder->aux_ch = (int)i915_bios_dp_aux_ch(devdata);
	encoder->device_type = devdata->child.device_type;
	encoder->dvo_port = devdata->child.dvo_port;
	encoder->ddc_pin_raw = devdata->child.ddc_pin;
	encoder->ddc_pin = i915_bios_hdmi_ddc_pin(devdata);

	/* Copies which outputs the child supports. */
	encoder->supports_dp = i915_bios_encoder_supports_dp(devdata);
	encoder->supports_edp = i915_bios_encoder_supports_edp(devdata);
	encoder->supports_hdmi = i915_bios_encoder_supports_hdmi(devdata);
	encoder->supports_dvi = i915_bios_encoder_supports_dvi(devdata);
	encoder->supports_typec_usb = i915_bios_encoder_supports_typec_usb(devdata);
	encoder->supports_tbt = i915_bios_encoder_supports_tbt(devdata);
	encoder->lane_reversal = i915_bios_encoder_lane_reversal(devdata);
	encoder->hpd_invert = i915_bios_encoder_hpd_invert(devdata);

	/* Copies the link limits and the drive levels. */
	encoder->dp_max_lane_count = i915_bios_dp_max_lane_count(devdata);
	encoder->dp_max_link_rate = i915_bios_dp_max_link_rate(devdata);
	encoder->dp_boost_level = i915_bios_dp_boost_level(devdata);
	encoder->hdmi_boost_level = i915_bios_hdmi_boost_level(devdata);
	encoder->hdmi_level_shift = drv_i915_bios_hdmi_level_shift(devdata);
}

/* Copies what the panel data says into the panel's flat record. */
static void
i915_vbt_flatten_panel(
	struct i915_vbt_world *world,
	const struct intel_panel *panel,
	struct i915_vbt_panel *out)
{
	const struct drm_display_mode *mode;

	/* Copies the panel type and the eDP link data. */
	out->initialized = 1;
	out->panel_type = panel->vbt.panel_type;
	out->bpp = panel->vbt.edp.bpp;
	out->edp_rate = panel->vbt.edp.rate;
	out->edp_lanes = panel->vbt.edp.lanes;
	out->edp_preemphasis = panel->vbt.edp.preemphasis;
	out->edp_vswing = panel->vbt.edp.vswing;
	out->edp_low_vswing = panel->vbt.edp.low_vswing;
	out->edp_hobl = panel->vbt.edp.hobl;
	out->override_afc_startup = world->vbt_i915.display.vbt.override_afc_startup;
	out->edp_max_link_rate = panel->vbt.edp.max_link_rate;

	/* Copies the eDP power sequence, in 100 us units. */
	out->t1_t3 = panel->vbt.edp.pps.t1_t3;
	out->t8 = panel->vbt.edp.pps.t8;
	out->t9 = panel->vbt.edp.pps.t9;
	out->t10 = panel->vbt.edp.pps.t10;
	out->t11_t12 = panel->vbt.edp.pps.t11_t12;

	/* Copies the backlight data. */
	out->bl_present = panel->vbt.backlight.present;
	out->bl_type = (int)panel->vbt.backlight.type;
	out->bl_controller = panel->vbt.backlight.controller;
	out->bl_active_low_pwm = panel->vbt.backlight.active_low_pwm;
	out->bl_pwm_freq_hz = panel->vbt.backlight.pwm_freq_hz;
	out->bl_min_brightness = panel->vbt.backlight.min_brightness;
	out->drrs_type = (int)panel->vbt.drrs_type;
	out->vrr = panel->vbt.vrr;

	/* Copies the VBT's own panel mode, when it carries one. */
	mode = panel->vbt.lfp_lvds_vbt_mode;
	if (mode != NULL) {
		out->has_lfp_mode = 1;
		out->mode_clock_khz = mode->clock;
		out->hdisplay = mode->hdisplay;
		out->hsync_start = mode->hsync_start;
		out->hsync_end = mode->hsync_end;
		out->htotal = mode->htotal;
		out->vdisplay = mode->vdisplay;
		out->vsync_start = mode->vsync_start;
		out->vsync_end = mode->vsync_end;
		out->vtotal = mode->vtotal;
	}
}

/* Returns the size of a BDB block, given a pointer to its block id. */
static u32
i915_get_blocksize_raw(
	const u8 *block_base)
{
	/* The MIPI Sequence Block v3+ has a separate size field. */
	if (*block_base == BDB_MIPI_SEQUENCE && *(block_base + 3) >= 3)
		return *((const u32 *)(block_base + 4));

	/* Succeeded: every other block has the 16-bit size after its id. */
	return *((const u16 *)(block_base + 1));
}

/* Returns the size of a BDB block, given a pointer to the data after its header. */
static u32
i915_get_blocksize(
	const void *block_data)
{
	u32 size;

	/* The block header is the three bytes before the data. */
	size = i915_get_blocksize_raw((const u8 *)block_data - 3);

	/* Succeeded: reports the block size. */
	return size;
}

/* Returns the data of a BDB block in the raw VBT, or NULL when it is missing or cut short. */
static const void *
i915_find_raw_section(
	const void *bdb_bytes,
	enum bdb_block_id section_id)
{
	const struct bdb_header *bdb;
	const u8 *base;
	int index;
	u32 total;
	u32 current_size;
	enum bdb_block_id current_id;

	/* Skips to the first section. */
	bdb = bdb_bytes;
	base = bdb_bytes;
	index = 0;
	index += bdb->header_size;
	total = bdb->bdb_size;

	/* Walks the sections looking for section_id. */
	while ((u32)(index + 3) < total) {
		current_id = *(base + index);
		current_size = i915_get_blocksize_raw(base + index);
		index += 3;

		/* A block that runs past the BDB ends the walk. */
		if (index + current_size > total)
			return NULL;

		/* Succeeded: this is the block. */
		if (current_id == section_id)
			return base + index;

		index += current_size;
	}

	/* The BDB holds no such block. */
	return NULL;
}

/* Returns the offset from the BDB to a block's data, or 0 when the block is missing. */
static u32
i915_raw_block_offset(
	const void *bdb,
	enum bdb_block_id section_id)
{
	const void *block;

	/* Finds the block. */
	block = i915_find_raw_section(bdb, section_id);
	if (!block)
		return 0;

	/* Succeeded: reports the distance from the BDB. */
	return (u32)((const u8 *)block - (const u8 *)bdb);
}

/* Returns the data of a block the parser copied, or NULL. */
static const void *
i915_bdb_find_section(
	struct drm_i915_private *i915,
	enum bdb_block_id section_id)
{
	struct bdb_block_entry *entry;

	/* Looks for the block among the copied ones. */
	list_for_each_entry(entry, &i915->display.vbt.bdb_blocks, node) {
		if (entry->section_id == section_id)
			return entry->data + 3;
	}

	/* The parser copied no such block. */
	return NULL;
}

/* Returns the smallest size the LFP data block may have, or 0 without LFP data pointers. */
static size_t
i915_lfp_data_min_size(
	struct drm_i915_private *i915)
{
	const struct bdb_lvds_lfp_data_ptrs *ptrs;
	size_t size;

	/* Without the pointers the size is not known. */
	ptrs = i915_bdb_find_section(i915, BDB_LVDS_LFP_DATA_PTRS);
	if (!ptrs)
		return 0;

	/* The block must reach the panel name table when there is one. */
	size = sizeof(struct bdb_lvds_lfp_data);
	if (ptrs->panel_name.table_size)
		size = max(size, ptrs->panel_name.offset + sizeof(struct bdb_lvds_lfp_data_tail));

	/* Succeeded: reports the minimum size. */
	return size;
}

/* Tells whether the LFP data table pointers describe the LFP data block correctly. */
static bool
i915_validate_lfp_data_ptrs(
	const void *bdb,
	const struct bdb_lvds_lfp_data_ptrs *ptrs)
{
	int fp_timing_size;
	int dvo_timing_size;
	int panel_pnp_id_size;
	int panel_name_size;
	int data_block_size;
	int lfp_data_size;
	const void *data_block;
	const u16 *terminator;
	int i;

	/* The LFP data block must exist and have a size. */
	data_block = i915_find_raw_section(bdb, BDB_LVDS_LFP_DATA);
	if (!data_block)
		return false;
	data_block_size = i915_get_blocksize(data_block);
	if (data_block_size == 0)
		return false;

	/* always 3 indicating the presence of fp_timing+dvo_timing+panel_pnp_id */
	if (ptrs->lvds_entries != 3)
		return false;

	/* Reads the table sizes of the first entry. */
	fp_timing_size = ptrs->ptr[0].fp_timing.table_size;
	dvo_timing_size = ptrs->ptr[0].dvo_timing.table_size;
	panel_pnp_id_size = ptrs->ptr[0].panel_pnp_id.table_size;
	panel_name_size = ptrs->panel_name.table_size;

	/* fp_timing has variable size */
	if (fp_timing_size < 32 ||
	    dvo_timing_size != sizeof(struct lvds_dvo_timing) ||
	    panel_pnp_id_size != sizeof(struct lvds_pnp_id))
		return false;

	/* panel_name is not present in old VBTs */
	if (panel_name_size != 0 &&
	    panel_name_size != sizeof(struct lvds_lfp_panel_name))
		return false;

	/* Sixteen entries of the entry size must fit in the block. */
	lfp_data_size = ptrs->ptr[1].fp_timing.offset - ptrs->ptr[0].fp_timing.offset;
	if (16 * lfp_data_size > data_block_size)
		return false;

	/* make sure the table entries have uniform size */
	for (i = 1; i < 16; i++) {
		if (ptrs->ptr[i].fp_timing.table_size != fp_timing_size ||
		    ptrs->ptr[i].dvo_timing.table_size != dvo_timing_size ||
		    ptrs->ptr[i].panel_pnp_id.table_size != panel_pnp_id_size)
			return false;

		if (ptrs->ptr[i].fp_timing.offset - ptrs->ptr[i - 1].fp_timing.offset != lfp_data_size ||
		    ptrs->ptr[i].dvo_timing.offset - ptrs->ptr[i - 1].dvo_timing.offset != lfp_data_size ||
		    ptrs->ptr[i].panel_pnp_id.offset - ptrs->ptr[i - 1].panel_pnp_id.offset != lfp_data_size)
			return false;
	}

	/*
	 * Except for vlv/chv machines all real VBTs seem to have 6
	 * unaccounted bytes in the fp_timing table. And it doesn't
	 * appear to be a really intentional hole as the fp_timing
	 * 0xffff terminator is always within those 6 missing bytes.
	 */
	if (fp_timing_size + 6 + dvo_timing_size + panel_pnp_id_size == lfp_data_size)
		fp_timing_size += 6;

	/* The three tables must fill one entry exactly. */
	if (fp_timing_size + dvo_timing_size + panel_pnp_id_size != lfp_data_size)
		return false;

	/* The three tables of the first entry must follow each other. */
	if (ptrs->ptr[0].fp_timing.offset + fp_timing_size != ptrs->ptr[0].dvo_timing.offset ||
	    ptrs->ptr[0].dvo_timing.offset + dvo_timing_size != ptrs->ptr[0].panel_pnp_id.offset ||
	    ptrs->ptr[0].panel_pnp_id.offset + panel_pnp_id_size != lfp_data_size)
		return false;

	/* make sure the tables fit inside the data block */
	for (i = 0; i < 16; i++) {
		if (ptrs->ptr[i].fp_timing.offset + fp_timing_size > data_block_size ||
		    ptrs->ptr[i].dvo_timing.offset + dvo_timing_size > data_block_size ||
		    ptrs->ptr[i].panel_pnp_id.offset + panel_pnp_id_size > data_block_size)
			return false;
	}

	/* The panel name table must fit inside the data block. */
	if (ptrs->panel_name.offset + 16 * panel_name_size > data_block_size)
		return false;

	/* make sure fp_timing terminators are present at expected locations */
	for (i = 0; i < 16; i++) {
		terminator = (const u16 *)((const u8 *)data_block + ptrs->ptr[i].fp_timing.offset + fp_timing_size - 2);
		if (*terminator != 0xffff)
			return false;
	}

	/* Succeeded: the pointers describe the block. */
	return true;
}

/* Makes the data table offsets relative to the data block, and validates them. */
static bool
i915_fixup_lfp_data_ptrs(
	const void *bdb,
	void *ptrs_block)
{
	struct bdb_lvds_lfp_data_ptrs *ptrs;
	u32 offset;
	bool valid;
	int i;

	/* Finds where the data block starts. */
	ptrs = ptrs_block;
	offset = i915_raw_block_offset(bdb, BDB_LVDS_LFP_DATA);

	/* Moves every table offset of every entry. */
	for (i = 0; i < 16; i++) {
		if (ptrs->ptr[i].fp_timing.offset < offset ||
		    ptrs->ptr[i].dvo_timing.offset < offset ||
		    ptrs->ptr[i].panel_pnp_id.offset < offset)
			return false;

		ptrs->ptr[i].fp_timing.offset -= offset;
		ptrs->ptr[i].dvo_timing.offset -= offset;
		ptrs->ptr[i].panel_pnp_id.offset -= offset;
	}

	/* Moves the panel name offset, when there is a panel name table. */
	if (ptrs->panel_name.table_size) {
		if (ptrs->panel_name.offset < offset)
			return false;

		ptrs->panel_name.offset -= offset;
	}

	/* Validates the moved pointers. */
	valid = i915_validate_lfp_data_ptrs(bdb, ptrs);
	if (!valid)
		return false;

	/* Succeeded: the pointers are relative and valid. */
	return true;
}

/* Places one table at the end of the space left, and reports the space left before it. */
static int
i915_make_lfp_data_ptr(
	struct lvds_lfp_data_ptr_table *table,
	int table_size,
	int total_size)
{
	/* A table that does not fit is not placed. */
	if (total_size < table_size)
		return total_size;

	/* Places the table at the end. */
	table->table_size = table_size;
	table->offset = total_size - table_size;

	/* Succeeded: reports the space before the table. */
	return total_size - table_size;
}

/* Places one table of the next entry one entry after the same table of the previous one. */
static void
i915_next_lfp_data_ptr(
	struct lvds_lfp_data_ptr_table *next,
	const struct lvds_lfp_data_ptr_table *prev,
	int size)
{
	/* Copies the size and moves the offset by one entry. */
	next->table_size = prev->table_size;
	next->offset = prev->offset + size;
}

/*
 * Makes up the LFP data table pointers block a modern VBT lacks.
 *
 * Returns the block with its 3-byte header, or NULL.
 */
static void *
i915_generate_lfp_data_ptrs(
	struct drm_i915_private *i915,
	const void *bdb)
{
	int i;
	int size;
	int table_size;
	int block_size;
	int offset;
	int fp_timing_size;
	struct bdb_lvds_lfp_data_ptrs *ptrs;
	const void *block;
	void *ptrs_block;

	/*
	 * The hardcoded fp_timing_size is only valid for
	 * modernish VBTs. All older VBTs definitely should
	 * include block 41 and thus we don't need to
	 * generate one.
	 */
	if (i915->display.vbt.version < 155)
		return NULL;
	fp_timing_size = 38;

	/* The LFP data block the pointers describe must exist. */
	block = i915_find_raw_section(bdb, BDB_LVDS_LFP_DATA);
	if (!block)
		return NULL;
	I915_VBT_DRM_DBG_KMS(&i915->drm, "Generating LFP data table pointers\n");

	/* Sixteen entries of the modern entry size must fit in the block. */
	block_size = i915_get_blocksize(block);
	size = fp_timing_size + sizeof(struct lvds_dvo_timing) +
		sizeof(struct lvds_pnp_id);
	if (size * 16 > block_size)
		return NULL;

	/* Allocates the block with its header. */
	ptrs_block = kzalloc(sizeof(*ptrs) + 3, GFP_KERNEL);
	if (!ptrs_block)
		return NULL;

	/* Writes the block header and points at the block. */
	*(u8 *)((u8 *)ptrs_block + 0) = BDB_LVDS_LFP_DATA_PTRS;
	*(u16 *)((u8 *)ptrs_block + 1) = sizeof(*ptrs);
	ptrs = (struct bdb_lvds_lfp_data_ptrs *)((u8 *)ptrs_block + 3);

	/* Places the three tables of the first entry from its end. */
	table_size = sizeof(struct lvds_pnp_id);
	size = i915_make_lfp_data_ptr(&ptrs->ptr[0].panel_pnp_id, table_size, size);
	table_size = sizeof(struct lvds_dvo_timing);
	size = i915_make_lfp_data_ptr(&ptrs->ptr[0].dvo_timing, table_size, size);
	table_size = fp_timing_size;
	size = i915_make_lfp_data_ptr(&ptrs->ptr[0].fp_timing, table_size, size);

	/* Counts the tables that were placed. */
	if (ptrs->ptr[0].fp_timing.table_size)
		ptrs->lvds_entries++;
	if (ptrs->ptr[0].dvo_timing.table_size)
		ptrs->lvds_entries++;
	if (ptrs->ptr[0].panel_pnp_id.table_size)
		ptrs->lvds_entries++;

	/* The three tables must fill the entry exactly. */
	if (size != 0 || ptrs->lvds_entries != 3) {
		i915_vbt_kfree(ptrs_block);
		return NULL;
	}

	/* Places the tables of the other fifteen entries one entry apart. */
	size = fp_timing_size + sizeof(struct lvds_dvo_timing) +
		sizeof(struct lvds_pnp_id);
	for (i = 1; i < 16; i++) {
		i915_next_lfp_data_ptr(&ptrs->ptr[i].fp_timing, &ptrs->ptr[i - 1].fp_timing, size);
		i915_next_lfp_data_ptr(&ptrs->ptr[i].dvo_timing, &ptrs->ptr[i - 1].dvo_timing, size);
		i915_next_lfp_data_ptr(&ptrs->ptr[i].panel_pnp_id, &ptrs->ptr[i - 1].panel_pnp_id, size);
	}

	/* Places the panel name table after the entries, when it fits. */
	table_size = sizeof(struct lvds_lfp_panel_name);
	if (16 * (size + table_size) <= block_size) {
		ptrs->panel_name.table_size = table_size;
		ptrs->panel_name.offset = size * 16;
	}

	/* Makes every offset relative to the BDB, as a VBT's own pointers are. */
	offset = (int)((const u8 *)block - (const u8 *)bdb);
	for (i = 0; i < 16; i++) {
		ptrs->ptr[i].fp_timing.offset += offset;
		ptrs->ptr[i].dvo_timing.offset += offset;
		ptrs->ptr[i].panel_pnp_id.offset += offset;
	}
	if (ptrs->panel_name.table_size)
		ptrs->panel_name.offset += offset;

	/* Succeeded: reports the made-up block. */
	return ptrs_block;
}

/* Copies one BDB block out of the VBT onto the device's block list. */
static void
i915_init_bdb_block(
	struct drm_i915_private *i915,
	const void *bdb,
	enum bdb_block_id section_id,
	size_t min_size)
{
	struct bdb_block_entry *entry;
	void *temp_block;
	const void *block;
	size_t block_size;
	bool fixed;

	/* Finds the block. */
	temp_block = NULL;
	block = i915_find_raw_section(bdb, section_id);

	/* Modern VBTs lack the LFP data table pointers block, make one up */
	if (!block && section_id == BDB_LVDS_LFP_DATA_PTRS) {
		temp_block = i915_generate_lfp_data_ptrs(i915, bdb);
		if (temp_block)
			block = (const u8 *)temp_block + 3;
	}
	if (!block)
		return;

	/* Every block the parser copies has a minimum size. */
	(void)I915_VBT_DRM_WARN(&i915->drm, min_size == 0,
		"Block %d min_size is zero\n", section_id);

	/*
	 * Version number and new block size are considered
	 * part of the header for MIPI sequenece block v3+.
	 */
	block_size = i915_get_blocksize(block);
	if (section_id == BDB_MIPI_SEQUENCE && *(const u8 *)block >= 3)
		block_size += 5;

	/* Allocates the copy, padded to the minimum size. */
	entry = kzalloc(struct_size(entry, data, max(min_size, block_size) + 3),
			GFP_KERNEL);
	if (!entry) {
		i915_vbt_kfree(temp_block);
		return;
	}

	/* Copies the block with its header. */
	entry->section_id = section_id;
	kern_memcpy(entry->data, (const u8 *)block - 3, block_size + 3);
	i915_vbt_kfree(temp_block);
	I915_VBT_DRM_DBG_KMS(&i915->drm, "Found BDB block %d (size %zu, min size %zu)\n",
		section_id, block_size, min_size);

	/* Makes the LFP data table pointers relative, and drops malformed ones. */
	if (section_id == BDB_LVDS_LFP_DATA_PTRS) {
		fixed = i915_fixup_lfp_data_ptrs(bdb, entry->data + 3);
		if (!fixed) {
			I915_VBT_DRM_ERR(&i915->drm, "VBT has malformed LFP data table pointers\n");
			i915_vbt_kfree(entry);
			return;
		}
	}

	/* Puts the copy on the block list. */
	i915_list_add_tail(&entry->node, &i915->display.vbt.bdb_blocks);
}

/* Copies every BDB block the parser uses out of the VBT. */
static void
i915_init_bdb_blocks(
	struct drm_i915_private *i915,
	const void *bdb)
{
	enum bdb_block_id section_id;
	size_t min_size;
	size_t i;

	/* Copies each block in the table order. */
	for (i = 0; i < ARRAY_SIZE(bdb_blocks); i++) {
		section_id = bdb_blocks[i].section_id;
		min_size = bdb_blocks[i].min_size;

		/* The LFP data block's minimum size depends on its pointers. */
		if (section_id == BDB_LVDS_LFP_DATA)
			min_size = i915_lfp_data_min_size(i915);

		i915_init_bdb_block(i915, bdb, section_id, min_size);
	}
}

/* Fills a mode from a VBT DVO timing. */
static void
i915_fill_detail_timing_data(
	struct drm_i915_private *i915,
	struct drm_display_mode *panel_fixed_mode,
	const struct lvds_dvo_timing *dvo_timing)
{
	UNUSED_PARAMETER(i915);

	/* The horizontal timing. */
	panel_fixed_mode->hdisplay = (dvo_timing->hactive_hi << 8) |
		dvo_timing->hactive_lo;
	panel_fixed_mode->hsync_start = panel_fixed_mode->hdisplay +
		((dvo_timing->hsync_off_hi << 8) | dvo_timing->hsync_off_lo);
	panel_fixed_mode->hsync_end = panel_fixed_mode->hsync_start +
		((dvo_timing->hsync_pulse_width_hi << 8) |
			dvo_timing->hsync_pulse_width_lo);
	panel_fixed_mode->htotal = panel_fixed_mode->hdisplay +
		((dvo_timing->hblank_hi << 8) | dvo_timing->hblank_lo);

	/* The vertical timing. */
	panel_fixed_mode->vdisplay = (dvo_timing->vactive_hi << 8) |
		dvo_timing->vactive_lo;
	panel_fixed_mode->vsync_start = panel_fixed_mode->vdisplay +
		((dvo_timing->vsync_off_hi << 4) | dvo_timing->vsync_off_lo);
	panel_fixed_mode->vsync_end = panel_fixed_mode->vsync_start +
		((dvo_timing->vsync_pulse_width_hi << 4) |
			dvo_timing->vsync_pulse_width_lo);
	panel_fixed_mode->vtotal = panel_fixed_mode->vdisplay +
		((dvo_timing->vblank_hi << 8) | dvo_timing->vblank_lo);

	/* The clock, in kHz, and the mode's type. */
	panel_fixed_mode->clock = dvo_timing->clock * 10;
	panel_fixed_mode->type = DRM_MODE_TYPE_PREFERRED;

	/* The horizontal sync polarity. */
	if (dvo_timing->hsync_positive) {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_PHSYNC;
	} else {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_NHSYNC;
	}

	/* The vertical sync polarity. */
	if (dvo_timing->vsync_positive) {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_PVSYNC;
	} else {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_NVSYNC;
	}

	/* The physical size. */
	panel_fixed_mode->width_mm = (dvo_timing->himage_hi << 8) |
		dvo_timing->himage_lo;
	panel_fixed_mode->height_mm = (dvo_timing->vimage_hi << 8) |
		dvo_timing->vimage_lo;

	/* Some VBTs have bogus h/vsync_end values */
	if (panel_fixed_mode->hsync_end > panel_fixed_mode->htotal) {
		I915_VBT_DRM_DBG_KMS(&i915->drm, "reducing hsync_end %d->%d\n",
			panel_fixed_mode->hsync_end, panel_fixed_mode->htotal);
		panel_fixed_mode->hsync_end = panel_fixed_mode->htotal;
	}
	if (panel_fixed_mode->vsync_end > panel_fixed_mode->vtotal) {
		I915_VBT_DRM_DBG_KMS(&i915->drm, "reducing vsync_end %d->%d\n",
			panel_fixed_mode->vsync_end, panel_fixed_mode->vtotal);
		panel_fixed_mode->vsync_end = panel_fixed_mode->vtotal;
	}

	/* Names the mode. */
	drv_i915_vbt_drm_mode_set_name(panel_fixed_mode);
}

/* Returns the DVO timing of a panel entry. */
static const struct lvds_dvo_timing *
i915_get_lvds_dvo_timing(
	const struct bdb_lvds_lfp_data *data,
	const struct bdb_lvds_lfp_data_ptrs *ptrs,
	int index)
{
	/* Succeeded: the pointers give the timing's offset in the data block. */
	return (const struct lvds_dvo_timing *)((const u8 *)data + ptrs->ptr[index].dvo_timing.offset);
}

/* Returns the FP timing of a panel entry. */
static const struct lvds_fp_timing *
i915_get_lvds_fp_timing(
	const struct bdb_lvds_lfp_data *data,
	const struct bdb_lvds_lfp_data_ptrs *ptrs,
	int index)
{
	/* Succeeded: the pointers give the timing's offset in the data block. */
	return (const struct lvds_fp_timing *)((const u8 *)data + ptrs->ptr[index].fp_timing.offset);
}

/* Returns the PnP id of a panel entry. */
static const struct lvds_pnp_id *
i915_get_lvds_pnp_id(
	const struct bdb_lvds_lfp_data *data,
	const struct bdb_lvds_lfp_data_ptrs *ptrs,
	int index)
{
	/* Succeeded: the pointers give the id's offset in the data block. */
	return (const struct lvds_pnp_id *)((const u8 *)data + ptrs->ptr[index].panel_pnp_id.offset);
}

/* Returns the tail of the LFP data block (the panel names), or NULL when there is none. */
static const struct bdb_lvds_lfp_data_tail *
i915_get_lfp_data_tail(
	const struct bdb_lvds_lfp_data *data,
	const struct bdb_lvds_lfp_data_ptrs *ptrs)
{
	/* An old VBT has no panel name table. */
	if (!ptrs->panel_name.table_size)
		return NULL;

	/* Succeeded: the pointers give the tail's offset in the data block. */
	return (const struct bdb_lvds_lfp_data_tail *)((const u8 *)data + ptrs->panel_name.offset);
}

/* Logs a PnP id. */
static void
i915_dump_pnp_id(
	struct drm_i915_private *i915,
	const struct lvds_pnp_id *pnp_id,
	const char *name)
{
	u16 mfg_name;
	char vend[4];

	UNUSED_PARAMETER(i915);

	/* The manufacturer is stored big-endian. */
	mfg_name = be16_to_cpu((__force __be16)pnp_id->mfg_name);

	/* Logs the id. */
	I915_VBT_DRM_DBG_KMS(&i915->drm, "%s PNPID mfg: %s (0x%x), prod: %u, serial: %u, week: %d, year: %d\n",
		name, i915_drm_edid_decode_mfg_id(mfg_name, vend),
		pnp_id->mfg_name, pnp_id->product_code, pnp_id->serial,
		pnp_id->mfg_week, pnp_id->mfg_year + 1990);
}

/* Returns the panel type the OpRegion names. */
static int
i915_opregion_get_panel_type(
	struct drm_i915_private *i915,
	const struct intel_bios_encoder_data *devdata,
	const struct drm_edid *drm_edid,
	bool use_fallback)
{
	int panel_type;

	UNUSED_PARAMETER(devdata);
	UNUSED_PARAMETER(drm_edid);
	UNUSED_PARAMETER(use_fallback);

	/*
	 * Asks the OpRegion.
	 *
	 * XXX: this answers -ENODEV even when the start found an OpRegion,
	 * as the old parser did (vbt.h).
	 */
	panel_type = i915_vbt_intel_opregion_get_panel_type(i915);

	/* Succeeded: reports the OpRegion's answer. */
	return panel_type;
}

/* Returns the panel type the LVDS options block names, or -1. */
static int
i915_vbt_get_panel_type(
	struct drm_i915_private *i915,
	const struct intel_bios_encoder_data *devdata,
	const struct drm_edid *drm_edid,
	bool use_fallback)
{
	const struct bdb_lvds_options *lvds_options;

	UNUSED_PARAMETER(drm_edid);
	UNUSED_PARAMETER(use_fallback);

	/* Without the LVDS options block the VBT names no panel type. */
	lvds_options = i915_bdb_find_section(i915, BDB_LVDS_OPTIONS);
	if (!lvds_options)
		return -1;

	/* Refuses a panel type out of range. */
	if (lvds_options->panel_type > 0xf &&
	    lvds_options->panel_type != 0xff) {
		I915_VBT_DRM_DBG_KMS(&i915->drm, "Invalid VBT panel type 0x%x\n",
			lvds_options->panel_type);
		return -1;
	}

	/* The second LFP has a panel type of its own. */
	if (devdata && devdata->child.handle == DEVICE_HANDLE_LFP2)
		return lvds_options->panel_type2;

	/* Any other child must be the first LFP. */
	(void)I915_VBT_DRM_WARN_ON(&i915->drm, devdata && devdata->child.handle != DEVICE_HANDLE_LFP1);

	/* Succeeded: reports the first LFP's panel type. */
	return lvds_options->panel_type;
}

/* Returns the panel entry whose PnP id matches the EDID, or -1. */
static int
i915_pnpid_get_panel_type(
	struct drm_i915_private *i915,
	const struct intel_bios_encoder_data *devdata,
	const struct drm_edid *drm_edid,
	bool use_fallback)
{
	const struct bdb_lvds_lfp_data *data;
	const struct bdb_lvds_lfp_data_ptrs *ptrs;
	const struct lvds_pnp_id *edid_id;
	const struct lvds_pnp_id *vbt_id;
	struct lvds_pnp_id edid_id_nodate;
	const struct edid *edid;
	int compared;
	int i;
	int best;

	UNUSED_PARAMETER(devdata);
	UNUSED_PARAMETER(use_fallback);

	/* Without an EDID there is nothing to match. */
	edid = i915_drm_edid_raw(drm_edid); /* FIXME */
	if (!edid)
		return -1;

	/* The EDID's id, and the same id without its date. */
	edid_id = (const struct lvds_pnp_id *)&edid->mfg_id[0];
	edid_id_nodate = *edid_id;
	edid_id_nodate.mfg_week = 0;
	edid_id_nodate.mfg_year = 0;
	i915_dump_pnp_id(i915, edid_id, "EDID");

	/* The panel table needs both LFP blocks. */
	ptrs = i915_bdb_find_section(i915, BDB_LVDS_LFP_DATA_PTRS);
	if (!ptrs)
		return -1;
	data = i915_bdb_find_section(i915, BDB_LVDS_LFP_DATA);
	if (!data)
		return -1;

	/* Looks for a full match, remembering the first match without a date. */
	best = -1;
	for (i = 0; i < 16; i++) {
		vbt_id = i915_get_lvds_pnp_id(data, ptrs, i);

		/* full match? */
		compared = kern_memcmp(vbt_id, edid_id, sizeof(*vbt_id));
		if (compared == 0)
			return i;

		/*
		 * Accept a match w/o date if no full match is found,
		 * and the VBT entry does not specify a date.
		 */
		if (best < 0) {
			compared = kern_memcmp(vbt_id, &edid_id_nodate, sizeof(*vbt_id));
			if (compared == 0)
				best = i;
		}
	}

	/* Succeeded: reports the match without a date, or -1. */
	return best;
}

/* Returns panel type 0 when a fallback is allowed, else -1. */
static int
i915_fallback_get_panel_type(
	struct drm_i915_private *i915,
	const struct intel_bios_encoder_data *devdata,
	const struct drm_edid *drm_edid,
	bool use_fallback)
{
	UNUSED_PARAMETER(i915);
	UNUSED_PARAMETER(devdata);
	UNUSED_PARAMETER(drm_edid);

	/* Without a fallback there is no panel type. */
	if (!use_fallback)
		return -1;

	/* Succeeded: the fallback is panel type 0. */
	return 0;
}

/*
 * Returns the panel type of a child, from the OpRegion, the VBT, the PnP
 * id or the fallback, in that preference, or -1.
 */
static int
i915_get_panel_type(
	struct drm_i915_private *i915,
	const struct intel_bios_encoder_data *devdata,
	const struct drm_edid *drm_edid,
	bool use_fallback)
{
	/*
	 * One panel type source: its name, how it is asked, and its answer.
	 */
	struct {
		const char *name;
		int (*get_panel_type)(struct drm_i915_private *i915,
				      const struct intel_bios_encoder_data *devdata,
				      const struct drm_edid *drm_edid, bool use_fallback);
		int panel_type;
	} panel_types[] = {
		[PANEL_TYPE_OPREGION] = {
			.name = "OpRegion",
			.get_panel_type = i915_opregion_get_panel_type,
		},
		[PANEL_TYPE_VBT] = {
			.name = "VBT",
			.get_panel_type = i915_vbt_get_panel_type,
		},
		[PANEL_TYPE_PNPID] = {
			.name = "PNPID",
			.get_panel_type = i915_pnpid_get_panel_type,
		},
		[PANEL_TYPE_FALLBACK] = {
			.name = "fallback",
			.get_panel_type = i915_fallback_get_panel_type,
		},
	};
	size_t i;
	int selected;

	/* Asks every source. */
	for (i = 0; i < ARRAY_SIZE(panel_types); i++) {
		panel_types[i].panel_type = panel_types[i].get_panel_type(i915, devdata,
									  drm_edid, use_fallback);

		/* A source must answer a panel type in range, or 0xff. */
		(void)I915_VBT_DRM_WARN_ON(&i915->drm, panel_types[i].panel_type > 0xf &&
			panel_types[i].panel_type != 0xff);

		/* Logs every source that answered. */
		if (panel_types[i].panel_type >= 0) {
			I915_VBT_DRM_DBG_KMS(&i915->drm, "Panel type (%s): %d\n",
				panel_types[i].name, panel_types[i].panel_type);
		}
	}

	/* Prefers the OpRegion, then the PnP id when the VBT defers to it, then the VBT, then the fallback. */
	if (panel_types[PANEL_TYPE_OPREGION].panel_type >= 0) {
		selected = PANEL_TYPE_OPREGION;
	} else if (panel_types[PANEL_TYPE_VBT].panel_type == 0xff &&
		   panel_types[PANEL_TYPE_PNPID].panel_type >= 0) {
		selected = PANEL_TYPE_PNPID;
	} else if (panel_types[PANEL_TYPE_VBT].panel_type != 0xff &&
		   panel_types[PANEL_TYPE_VBT].panel_type >= 0) {
		selected = PANEL_TYPE_VBT;
	} else {
		selected = PANEL_TYPE_FALLBACK;
	}

	/* Logs the choice. */
	I915_VBT_DRM_DBG_KMS(&i915->drm, "Selected panel type (%s): %d\n",
		panel_types[selected].name, panel_types[selected].panel_type);

	/* Succeeded: reports the chosen source's answer. */
	return panel_types[selected].panel_type;
}

/* Returns a panel's field of num_bits bits from a value packing one field per panel type. */
static unsigned int
i915_panel_bits(
	unsigned int value,
	int panel_type,
	int num_bits)
{
	/* Succeeded: reports the panel's field. */
	return (value >> (panel_type * num_bits)) & (BIT(num_bits) - 1);
}

/* Returns a panel's bit from a value packing one bit per panel type. */
static bool
i915_panel_bool(
	unsigned int value,
	int panel_type)
{
	unsigned int bit;

	/* Takes the panel's one-bit field. */
	bit = i915_panel_bits(value, panel_type, 1);
	if (bit == 0)
		return false;

	/* Succeeded: the panel's bit is set. */
	return true;
}

/* Parses the general panel options: dithering and the DRRS mode. */
static void
i915_parse_panel_options(
	struct drm_i915_private *i915,
	struct intel_panel *panel)
{
	const struct bdb_lvds_options *lvds_options;
	int panel_type;
	int drrs_mode;
	u32 block_size;

	/* Without the LVDS options block there is nothing to parse. */
	panel_type = panel->vbt.panel_type;
	lvds_options = i915_bdb_find_section(i915, BDB_LVDS_OPTIONS);
	if (!lvds_options)
		return;

	/* The dithering. */
	panel->vbt.lvds_dither = lvds_options->pixel_dither;

	/*
	 * Empirical evidence indicates the block size can be
	 * either 4,14,16,24+ bytes. For older VBTs no clear
	 * relationship between the block size vs. BDB version.
	 */
	block_size = i915_get_blocksize(lvds_options);
	if (block_size < 16)
		return;

	/*
	 * VBT has static DRRS = 0 and seamless DRRS = 2.
	 * The below piece of code is required to adjust vbt.drrs_type
	 * to match the enum drrs_support_type.
	 */
	drrs_mode = i915_panel_bits(lvds_options->dps_panel_type_bits,
				    panel_type, 2);
	switch (drrs_mode) {
	case 0:
		panel->vbt.drrs_type = DRRS_TYPE_STATIC;
		I915_VBT_DRM_DBG_KMS(&i915->drm, "DRRS supported mode is static\n");
		break;
	case 2:
		panel->vbt.drrs_type = DRRS_TYPE_SEAMLESS;
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"DRRS supported mode is seamless\n");
		break;
	default:
		panel->vbt.drrs_type = DRRS_TYPE_NONE;
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"DRRS not supported (VBT input)\n");
		break;
	}
}

/* Parses the panel's mode out of the legacy LFP panel table. */
static void
i915_parse_lfp_panel_dtd(
	struct drm_i915_private *i915,
	struct intel_panel *panel,
	const struct bdb_lvds_lfp_data *lvds_lfp_data,
	const struct bdb_lvds_lfp_data_ptrs *lvds_lfp_data_ptrs)
{
	const struct lvds_dvo_timing *panel_dvo_timing;
	const struct lvds_fp_timing *fp_timing;
	struct drm_display_mode *panel_fixed_mode;
	int panel_type;

	/* Finds the panel's timing. */
	panel_type = panel->vbt.panel_type;
	panel_dvo_timing = i915_get_lvds_dvo_timing(lvds_lfp_data,
						    lvds_lfp_data_ptrs,
						    panel_type);

	/* Allocates the mode. */
	panel_fixed_mode = kzalloc(sizeof(*panel_fixed_mode), GFP_KERNEL);
	if (!panel_fixed_mode)
		return;

	/* Fills the mode and hands it to the panel. */
	i915_fill_detail_timing_data(i915, panel_fixed_mode, panel_dvo_timing);
	panel->vbt.lfp_lvds_vbt_mode = panel_fixed_mode;
	I915_VBT_DRM_DBG_KMS(&i915->drm,
		"Found panel mode in BIOS VBT legacy lfp table: " DRM_MODE_FMT "\n",
		DRM_MODE_ARG(panel_fixed_mode));

	/* check the resolution, just to be sure */
	fp_timing = i915_get_lvds_fp_timing(lvds_lfp_data,
					    lvds_lfp_data_ptrs,
					    panel_type);
	if (fp_timing->x_res == panel_fixed_mode->hdisplay &&
	    fp_timing->y_res == panel_fixed_mode->vdisplay) {
		panel->vbt.bios_lvds_val = fp_timing->lvds_reg_val;
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"VBT initial LVDS value %x\n",
			panel->vbt.bios_lvds_val);
	}
}

/* Parses the LFP data: the mode when none is known yet, the PnP id, the name and the DRRS rate. */
static void
i915_parse_lfp_data(
	struct drm_i915_private *i915,
	struct intel_panel *panel)
{
	const struct bdb_lvds_lfp_data *data;
	const struct bdb_lvds_lfp_data_tail *tail;
	const struct bdb_lvds_lfp_data_ptrs *ptrs;
	const struct lvds_pnp_id *pnp_id;
	int panel_type;

	/* The LFP data needs both LFP blocks. */
	panel_type = panel->vbt.panel_type;
	ptrs = i915_bdb_find_section(i915, BDB_LVDS_LFP_DATA_PTRS);
	if (!ptrs)
		return;
	data = i915_bdb_find_section(i915, BDB_LVDS_LFP_DATA);
	if (!data)
		return;

	/* Takes the mode from the legacy table when the generic DTD gave none. */
	if (!panel->vbt.lfp_lvds_vbt_mode)
		i915_parse_lfp_panel_dtd(i915, panel, data, ptrs);

	/* Logs the panel's PnP id. */
	pnp_id = i915_get_lvds_pnp_id(data, ptrs, panel_type);
	i915_dump_pnp_id(i915, pnp_id, "Panel");

	/* An old VBT ends here, without the panel names. */
	tail = i915_get_lfp_data_tail(data, ptrs);
	if (!tail)
		return;
	I915_VBT_DRM_DBG_KMS(&i915->drm, "Panel name: %.*s\n",
		(int)sizeof(tail->panel_name[0].name),
		tail->panel_name[panel_type].name);

	/* The seamless DRRS minimum refresh rate, from BDB 188. */
	if (i915->display.vbt.version >= 188) {
		panel->vbt.seamless_drrs_min_refresh_rate =
			tail->seamless_drrs_min_refresh_rate[panel_type];
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"Seamless DRRS min refresh rate: %d Hz\n",
			panel->vbt.seamless_drrs_min_refresh_rate);
	}
}

/* Parses the panel's mode out of the generic DTD block. */
static void
i915_parse_generic_dtd(
	struct drm_i915_private *i915,
	struct intel_panel *panel)
{
	const struct bdb_generic_dtd *generic_dtd;
	const struct generic_dtd_entry *dtd;
	struct drm_display_mode *panel_fixed_mode;
	int num_dtd;

	/*
	 * Older VBTs provided DTD information for internal displays through
	 * the "LFP panel tables" block (42).  As of VBT revision 229 the
	 * DTD information should be provided via a newer "generic DTD"
	 * block (58).  Just to be safe, we'll try the new generic DTD block
	 * first on VBT >= 229, but still fall back to trying the old LFP
	 * block if that fails.
	 */
	if (i915->display.vbt.version < 229)
		return;

	/* Without the generic DTD block there is nothing to parse. */
	generic_dtd = i915_bdb_find_section(i915, BDB_GENERIC_DTD);
	if (!generic_dtd)
		return;

	/* Refuses entries too small to hold a DTD; larger ones are read anyway. */
	if (generic_dtd->gdtd_size < sizeof(struct generic_dtd_entry)) {
		I915_VBT_DRM_ERR(&i915->drm, "GDTD size %u is too small.\n",
			generic_dtd->gdtd_size);
		return;
	} else if (generic_dtd->gdtd_size !=
		   sizeof(struct generic_dtd_entry)) {
		I915_VBT_DRM_ERR(&i915->drm, "Unexpected GDTD size %u\n",
			generic_dtd->gdtd_size);
		/* DTD has unknown fields, but keep going */
	}

	/* The panel type must have an entry. */
	num_dtd = (i915_get_blocksize(generic_dtd) -
		   sizeof(struct bdb_generic_dtd)) / generic_dtd->gdtd_size;
	if (panel->vbt.panel_type >= num_dtd) {
		I915_VBT_DRM_ERR(&i915->drm,
			"Panel type %d not found in table of %d DTD's\n",
			panel->vbt.panel_type, num_dtd);
		return;
	}
	dtd = &generic_dtd->dtd[panel->vbt.panel_type];

	/* Allocates the mode. */
	panel_fixed_mode = kzalloc(sizeof(*panel_fixed_mode), GFP_KERNEL);
	if (!panel_fixed_mode)
		return;

	/* The horizontal timing. */
	panel_fixed_mode->hdisplay = dtd->hactive;
	panel_fixed_mode->hsync_start =
		panel_fixed_mode->hdisplay + dtd->hfront_porch;
	panel_fixed_mode->hsync_end =
		panel_fixed_mode->hsync_start + dtd->hsync;
	panel_fixed_mode->htotal =
		panel_fixed_mode->hdisplay + dtd->hblank;

	/* The vertical timing. */
	panel_fixed_mode->vdisplay = dtd->vactive;
	panel_fixed_mode->vsync_start =
		panel_fixed_mode->vdisplay + dtd->vfront_porch;
	panel_fixed_mode->vsync_end =
		panel_fixed_mode->vsync_start + dtd->vsync;
	panel_fixed_mode->vtotal =
		panel_fixed_mode->vdisplay + dtd->vblank;

	/* The clock, the physical size, the type and the name. */
	panel_fixed_mode->clock = dtd->pixel_clock;
	panel_fixed_mode->width_mm = dtd->width_mm;
	panel_fixed_mode->height_mm = dtd->height_mm;
	panel_fixed_mode->type = DRM_MODE_TYPE_PREFERRED;
	drv_i915_vbt_drm_mode_set_name(panel_fixed_mode);

	/* The horizontal sync polarity. */
	if (dtd->hsync_positive_polarity) {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_PHSYNC;
	} else {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_NHSYNC;
	}

	/* The vertical sync polarity. */
	if (dtd->vsync_positive_polarity) {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_PVSYNC;
	} else {
		panel_fixed_mode->flags |= DRM_MODE_FLAG_NVSYNC;
	}

	/* Hands the mode to the panel. */
	I915_VBT_DRM_DBG_KMS(&i915->drm,
		"Found panel mode in BIOS VBT generic dtd table: " DRM_MODE_FMT "\n",
		DRM_MODE_ARG(panel_fixed_mode));
	panel->vbt.lfp_lvds_vbt_mode = panel_fixed_mode;
}

/* Parses the panel's PWM backlight data. */
static void
i915_parse_lfp_backlight(
	struct drm_i915_private *i915,
	struct intel_panel *panel)
{
	const struct bdb_lfp_backlight_data *backlight_data;
	const struct lfp_backlight_data_entry *entry;
	const struct lfp_backlight_control_method *method;
	int panel_type;
	u16 level;
	u16 min_level;
	bool scale;

	/* Without the backlight block there is nothing to parse. */
	panel_type = panel->vbt.panel_type;
	backlight_data = i915_bdb_find_section(i915, BDB_LVDS_BACKLIGHT);
	if (!backlight_data)
		return;

	/* Refuses entries of another size. */
	if (backlight_data->entry_size != sizeof(backlight_data->data[0])) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"Unsupported backlight data entry size %u\n",
			backlight_data->entry_size);
		return;
	}

	/* Only a PWM backlight is described here. */
	entry = &backlight_data->data[panel_type];
	panel->vbt.backlight.present = false;
	if (entry->type == BDB_BACKLIGHT_TYPE_PWM)
		panel->vbt.backlight.present = true;
	if (!panel->vbt.backlight.present) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"PWM backlight not present in VBT (type %u)\n",
			entry->type);
		return;
	}

	/* The backlight type and controller: DDI and 0, or what BDB 191 names. */
	panel->vbt.backlight.type = INTEL_BACKLIGHT_DISPLAY_DDI;
	panel->vbt.backlight.controller = 0;
	if (i915->display.vbt.version >= 191) {
		method = &backlight_data->backlight_control[panel_type];
		panel->vbt.backlight.type = method->type;
		panel->vbt.backlight.controller = method->controller;
	}

	/* The PWM frequency and polarity. */
	panel->vbt.backlight.pwm_freq_hz = entry->pwm_freq_hz;
	panel->vbt.backlight.active_low_pwm = entry->active_low_pwm;

	/* The level and the minimum brightness: the 16-bit fields from BDB 234, else the 8-bit ones. */
	if (i915->display.vbt.version >= 234) {
		level = backlight_data->brightness_level[panel_type].level;
		min_level = backlight_data->brightness_min_level[panel_type].level;

		/* From BDB 236 the precision says whether the values are 16-bit. */
		scale = false;
		if (i915->display.vbt.version >= 236) {
			if (backlight_data->brightness_precision_bits[panel_type] == 16)
				scale = true;
		} else {
			if (level > 255)
				scale = true;
		}

		/* Scales a 16-bit minimum down to 8 bits. */
		if (scale)
			min_level = min_level / 255;

		/* A minimum out of range is warned about; the level is clamped, as Linux does. */
		if (min_level > 255) {
			I915_VBT_DRM_WARN_MSG(&i915->drm, "Brightness min level > 255\n");
			level = 255;
		}

		panel->vbt.backlight.min_brightness = min_level;
		panel->vbt.backlight.brightness_precision_bits =
			backlight_data->brightness_precision_bits[panel_type];
	} else {
		level = backlight_data->level[panel_type];
		panel->vbt.backlight.min_brightness = entry->min_brightness;
	}

	/* The HDR DPCD refresh timeout, from BDB 239. */
	if (i915->display.vbt.version >= 239) {
		panel->vbt.backlight.hdr_dpcd_refresh_timeout =
			DIV_ROUND_UP(backlight_data->hdr_dpcd_refresh_timeout[panel_type], 100);
	} else {
		panel->vbt.backlight.hdr_dpcd_refresh_timeout = 30;
	}

	/* Logs the backlight. */
	I915_VBT_DRM_DBG_KMS(&i915->drm,
		"VBT backlight PWM modulation frequency %u Hz, "
		"active %s, min brightness %u, level %u, controller %u\n",
		panel->vbt.backlight.pwm_freq_hz,
		panel->vbt.backlight.active_low_pwm ? "low" : "high",
		panel->vbt.backlight.min_brightness,
		level,
		panel->vbt.backlight.controller);
}

/* Returns the SSC reference frequency, in kHz, of the display version. */
static int
i915_bios_ssc_frequency(
	struct drm_i915_private *i915,
	bool alternate)
{
	int display_ver;

	/* Picks the frequency pair of the display version. */
	display_ver = i915_vbt_display_ver(i915);
	switch (display_ver) {
	case 2:
		return alternate ? 66667 : 48000;
	case 3:
	case 4:
		return alternate ? 100000 : 96000;
	default:
		return alternate ? 100000 : 120000;
	}
}

/* Parses the general features block. */
static void
i915_parse_general_features(
	struct drm_i915_private *i915)
{
	const struct bdb_general_features *general;
	int has_ddi;

	/* Without the general features block the defaults stay. */
	general = i915_bdb_find_section(i915, BDB_GENERAL_FEATURES);
	if (!general)
		return;

	/* int_crt_support can't be trusted on earlier platforms */
	i915->display.vbt.int_tv_support = general->int_tv_support;
	has_ddi = i915_vbt_has_ddi(i915);
	if (i915->display.vbt.version >= 155 &&
	    (has_ddi || IS_VALLEYVIEW(i915)))
		i915->display.vbt.int_crt_support = general->int_crt_support;

	/* The SSC, the clock mode and the FDI polarity. */
	i915->display.vbt.lvds_use_ssc = general->enable_ssc;
	i915->display.vbt.lvds_ssc_freq =
		i915_bios_ssc_frequency(i915, general->ssc_freq);
	i915->display.vbt.display_clock_mode = general->display_clock_mode;
	i915->display.vbt.fdi_rx_polarity_inverted = general->fdi_rx_polarity_inverted;

	/* The panel orientation, from BDB 181. */
	if (i915->display.vbt.version >= 181) {
		if (general->rotate_180) {
			i915->display.vbt.orientation = DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP;
		} else {
			i915->display.vbt.orientation = DRM_MODE_PANEL_ORIENTATION_NORMAL;
		}
	} else {
		i915->display.vbt.orientation = DRM_MODE_PANEL_ORIENTATION_UNKNOWN;
	}

	/* The AFC startup override, from BDB 249. */
	if (i915->display.vbt.version >= 249 && general->afc_startup_config) {
		i915->display.vbt.override_afc_startup = true;
		i915->display.vbt.override_afc_startup_val = general->afc_startup_config == 0x1 ? 0x0 : 0x7;
	}

	/* Logs the features. */
	I915_VBT_DRM_DBG_KMS(&i915->drm,
		"BDB_GENERAL_FEATURES int_tv_support %d int_crt_support %d lvds_use_ssc %d lvds_ssc_freq %d display_clock_mode %d fdi_rx_polarity_inverted %d\n",
		i915->display.vbt.int_tv_support,
		i915->display.vbt.int_crt_support,
		i915->display.vbt.lvds_use_ssc,
		i915->display.vbt.lvds_ssc_freq,
		i915->display.vbt.display_clock_mode,
		i915->display.vbt.fdi_rx_polarity_inverted);
}

/* Returns the i-th child device config of the general definitions block. */
static const struct child_device_config *
i915_child_device_ptr(
	const struct bdb_general_definitions *defs,
	int i)
{
	/* Succeeded: the children are child_dev_size bytes apart. */
	return (const struct child_device_config *)&defs->devices[i * defs->child_dev_size];
}

/* Parses the driver features block: whether the internal LVDS/eDP is supported. */
static void
i915_parse_driver_features(
	struct drm_i915_private *i915)
{
	const struct bdb_driver_features *driver;
	int display_ver;

	/* Without the driver features block the defaults stay. */
	driver = i915_bdb_find_section(i915, BDB_DRIVER_FEATURES);
	if (!driver)
		return;

	/* Decides from the LVDS configuration. */
	display_ver = i915_vbt_display_ver(i915);
	if (display_ver >= 5) {
		/*
		 * Note that we consider BDB_DRIVER_FEATURE_INT_SDVO_LVDS
		 * to mean "eDP". The VBT spec doesn't agree with that
		 * interpretation, but real world VBTs seem to.
		 */
		if (driver->lvds_config != BDB_DRIVER_FEATURE_INT_LVDS)
			i915->display.vbt.int_lvds_support = 0;
	} else {
		/*
		 * FIXME it's not clear which BDB version has the LVDS config
		 * bits defined. Revision history in the VBT spec says:
		 * "0.92 | Add two definitions for VBT value of LVDS Active
		 *  Config (00b and 11b values defined) | 06/13/2005"
		 * but does not the specify the BDB version.
		 *
		 * So far version 134 (on i945gm) is the oldest VBT observed
		 * in the wild with the bits correctly populated. Version
		 * 108 (on i85x) does not have the bits correctly populated.
		 */
		if (i915->display.vbt.version >= 134 &&
		    driver->lvds_config != BDB_DRIVER_FEATURE_INT_LVDS &&
		    driver->lvds_config != BDB_DRIVER_FEATURE_INT_SDVO_LVDS)
			i915->display.vbt.int_lvds_support = 0;
	}
}

/* Parses the panel part of the driver features block (before BDB 228): DRRS and PSR. */
static void
i915_parse_panel_driver_features(
	struct drm_i915_private *i915,
	struct intel_panel *panel)
{
	const struct bdb_driver_features *driver;

	/* Without the driver features block there is nothing to parse. */
	driver = i915_bdb_find_section(i915, BDB_DRIVER_FEATURES);
	if (!driver)
		return;

	/* From BDB 228 the LFP power block says this instead. */
	if (i915->display.vbt.version < 228) {
		I915_VBT_DRM_DBG_KMS(&i915->drm, "DRRS State Enabled:%d\n",
			driver->drrs_enabled);
		/*
		 * If DRRS is not supported, drrs_type has to be set to 0.
		 * This is because, VBT is configured in such a way that
		 * static DRRS is 0 and DRRS not supported is represented by
		 * driver->drrs_enabled=false
		 */
		if (!driver->drrs_enabled && panel->vbt.drrs_type != DRRS_TYPE_NONE) {
			/*
			 * FIXME Should DMRRS perhaps be treated as seamless
			 * but without the automatic downclocking?
			 */
			if (driver->dmrrs_enabled) {
				panel->vbt.drrs_type = DRRS_TYPE_STATIC;
			} else {
				panel->vbt.drrs_type = DRRS_TYPE_NONE;
			}
		}

		panel->vbt.psr.enable = driver->psr_enabled;
	}
}

/* Parses the LFP power block (from BDB 228): PSR, DRRS, HOBL and VRR. */
static void
i915_parse_power_conservation_features(
	struct drm_i915_private *i915,
	struct intel_panel *panel)
{
	const struct bdb_lfp_power *power;
	u8 panel_type;
	bool drrs;
	bool dmrrs;

	/* VRR is on unless the VBT says otherwise; this matches Windows behaviour. */
	panel_type = panel->vbt.panel_type;
	panel->vbt.vrr = true;

	/* The block exists from BDB 228. */
	if (i915->display.vbt.version < 228)
		return;
	power = i915_bdb_find_section(i915, BDB_LFP_POWER);
	if (!power)
		return;

	/* PSR. */
	panel->vbt.psr.enable = i915_panel_bool(power->psr, panel_type);

	/*
	 * If DRRS is not supported, drrs_type has to be set to 0.
	 * This is because, VBT is configured in such a way that
	 * static DRRS is 0 and DRRS not supported is represented by
	 * power->drrs & BIT(panel_type)=false
	 */
	drrs = i915_panel_bool(power->drrs, panel_type);
	if (!drrs && panel->vbt.drrs_type != DRRS_TYPE_NONE) {
		/*
		 * FIXME Should DMRRS perhaps be treated as seamless
		 * but without the automatic downclocking?
		 */
		dmrrs = i915_panel_bool(power->dmrrs, panel_type);
		if (dmrrs) {
			panel->vbt.drrs_type = DRRS_TYPE_STATIC;
		} else {
			panel->vbt.drrs_type = DRRS_TYPE_NONE;
		}
	}

	/* HOBL, from BDB 232. */
	if (i915->display.vbt.version >= 232)
		panel->vbt.edp.hobl = i915_panel_bool(power->hobl, panel_type);

	/* VRR, from BDB 233. */
	if (i915->display.vbt.version >= 233) {
		panel->vbt.vrr = i915_panel_bool(power->vrr_feature_enabled,
						 panel_type);
	}
}

/* Parses the eDP block: colour depth, power sequence, link parameters, low vswing, DRRS delay, rate limit. */
static void
i915_parse_edp(
	struct drm_i915_private *i915,
	struct intel_panel *panel)
{
	const struct bdb_edp *edp;
	const struct edp_power_seq *edp_pps;
	const struct edp_fast_link_params *edp_link_params;
	int panel_type;
	unsigned int color_depth;
	u8 vswing;

	/* Without the eDP block there is nothing to parse. */
	panel_type = panel->vbt.panel_type;
	edp = i915_bdb_find_section(i915, BDB_EDP);
	if (!edp)
		return;

	/* The colour depth. */
	color_depth = i915_panel_bits(edp->color_depth, panel_type, 2);
	switch (color_depth) {
	case EDP_18BPP:
		panel->vbt.edp.bpp = 18;
		break;
	case EDP_24BPP:
		panel->vbt.edp.bpp = 24;
		break;
	case EDP_30BPP:
		panel->vbt.edp.bpp = 30;
		break;
	}

	/* Get the eDP sequencing and link info */
	edp_pps = &edp->power_seqs[panel_type];
	edp_link_params = &edp->fast_link_params[panel_type];
	panel->vbt.edp.pps = *edp_pps;

	/* The fast link training rate, in kHz from BDB 224, else from the link parameters. */
	if (i915->display.vbt.version >= 224) {
		panel->vbt.edp.rate =
			edp->edp_fast_link_training_rate[panel_type] * 20;
	} else {
		switch (edp_link_params->rate) {
		case EDP_RATE_1_62:
			panel->vbt.edp.rate = 162000;
			break;
		case EDP_RATE_2_7:
			panel->vbt.edp.rate = 270000;
			break;
		case EDP_RATE_5_4:
			panel->vbt.edp.rate = 540000;
			break;
		default:
			I915_VBT_DRM_DBG_KMS(&i915->drm,
				"VBT has unknown eDP link rate value %u\n",
				edp_link_params->rate);
			break;
		}
	}

	/* The lane count, pre-emphasis and voltage swing. */
	i915_parse_edp_link_params(i915, panel, edp_link_params);

	/* The low vswing selection, from BDB 173. */
	if (i915->display.vbt.version >= 173) {
		/* Don't read from VBT if module parameter has valid value*/
		panel->vbt.edp.low_vswing = false;
		if (i915->display.params.edp_vswing) {
			if (i915->display.params.edp_vswing == 1)
				panel->vbt.edp.low_vswing = true;
		} else {
			vswing = (edp->edp_vswing_preemph >> (panel_type * 4)) & 0xF;
			if (vswing == 0)
				panel->vbt.edp.low_vswing = true;
		}
	}

	/* The DRRS MSA timing delay. */
	panel->vbt.edp.drrs_msa_timing_delay =
		i915_panel_bits(edp->sdrrs_msa_timing_delay, panel_type, 2);

	/* The port's maximum link rate, from BDB 244. */
	if (i915->display.vbt.version >= 244) {
		panel->vbt.edp.max_link_rate =
			edp->edp_max_port_link_rate[panel_type] * 20;
	}
}

/* Parses the lane count, pre-emphasis and voltage swing of the eDP fast link parameters. */
static void
i915_parse_edp_link_params(
	struct drm_i915_private *i915,
	struct intel_panel *panel,
	const struct edp_fast_link_params *edp_link_params)
{
	UNUSED_PARAMETER(i915);

	/* The lane count. */
	switch (edp_link_params->lanes) {
	case EDP_LANE_1:
		panel->vbt.edp.lanes = 1;
		break;
	case EDP_LANE_2:
		panel->vbt.edp.lanes = 2;
		break;
	case EDP_LANE_4:
		panel->vbt.edp.lanes = 4;
		break;
	default:
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"VBT has unknown eDP lane count value %u\n",
			edp_link_params->lanes);
		break;
	}

	/* The pre-emphasis. */
	switch (edp_link_params->preemphasis) {
	case EDP_PREEMPHASIS_NONE:
		panel->vbt.edp.preemphasis = DP_TRAIN_PRE_EMPH_LEVEL_0;
		break;
	case EDP_PREEMPHASIS_3_5dB:
		panel->vbt.edp.preemphasis = DP_TRAIN_PRE_EMPH_LEVEL_1;
		break;
	case EDP_PREEMPHASIS_6dB:
		panel->vbt.edp.preemphasis = DP_TRAIN_PRE_EMPH_LEVEL_2;
		break;
	case EDP_PREEMPHASIS_9_5dB:
		panel->vbt.edp.preemphasis = DP_TRAIN_PRE_EMPH_LEVEL_3;
		break;
	default:
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"VBT has unknown eDP pre-emphasis value %u\n",
			edp_link_params->preemphasis);
		break;
	}

	/* The voltage swing. */
	switch (edp_link_params->vswing) {
	case EDP_VSWING_0_4V:
		panel->vbt.edp.vswing = DP_TRAIN_VOLTAGE_SWING_LEVEL_0;
		break;
	case EDP_VSWING_0_6V:
		panel->vbt.edp.vswing = DP_TRAIN_VOLTAGE_SWING_LEVEL_1;
		break;
	case EDP_VSWING_0_8V:
		panel->vbt.edp.vswing = DP_TRAIN_VOLTAGE_SWING_LEVEL_2;
		break;
	case EDP_VSWING_1_2V:
		panel->vbt.edp.vswing = DP_TRAIN_VOLTAGE_SWING_LEVEL_3;
		break;
	default:
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"VBT has unknown eDP voltage swing value %u\n",
			edp_link_params->vswing);
		break;
	}
}

/* Returns the I_boost value a VBT boost level stands for, or 0. */
static u8
i915_translate_iboost(
	u8 val)
{
	static const u8 mapping[] = { 1, 3, 7 }; /* See VBT spec */

	/* A level beyond the table is not supported. */
	if (val >= ARRAY_SIZE(mapping)) {
		I915_VBT_DRM_DEBUG_KMS("Unsupported I_boost value found in VBT (%d), display may not work properly\n", val);
		return 0;
	}

	/* Succeeded: reports the level's value. */
	return mapping[val];
}

/* Returns the GMBUS pin of a VBT DDC pin on this platform, or 0. */
static u8
i915_map_ddc_pin(
	struct drm_i915_private *i915,
	u8 vbt_pin)
{
	const u8 *ddc_pin_map;
	int n_entries;
	int is_alderlake_p;
	int display_ver;
	int i;

	/* A discrete GPU names the GMBUS pin directly. */
	if (IS_DGFX(i915))
		return vbt_pin;

	/* Picks the platform's pin map. */
	is_alderlake_p = i915_vbt_is_alderlake_p(i915);
	display_ver = i915_vbt_display_ver(i915);
	if (INTEL_PCH_TYPE(i915) >= PCH_MTL || is_alderlake_p) {
		ddc_pin_map = adlp_ddc_pin_map;
		n_entries = ARRAY_SIZE(adlp_ddc_pin_map);
	} else if (IS_ALDERLAKE_S(i915)) {
		ddc_pin_map = adls_ddc_pin_map;
		n_entries = ARRAY_SIZE(adls_ddc_pin_map);
	} else if (IS_ROCKETLAKE(i915) && INTEL_PCH_TYPE(i915) == PCH_TGP) {
		ddc_pin_map = rkl_pch_tgp_ddc_pin_map;
		n_entries = ARRAY_SIZE(rkl_pch_tgp_ddc_pin_map);
	} else if (HAS_PCH_TGP(i915) && display_ver == 9) {
		ddc_pin_map = gen9bc_tgp_ddc_pin_map;
		n_entries = ARRAY_SIZE(gen9bc_tgp_ddc_pin_map);
	} else if (INTEL_PCH_TYPE(i915) >= PCH_ICP) {
		ddc_pin_map = icp_ddc_pin_map;
		n_entries = ARRAY_SIZE(icp_ddc_pin_map);
	} else if (HAS_PCH_CNP(i915)) {
		ddc_pin_map = cnp_ddc_pin_map;
		n_entries = ARRAY_SIZE(cnp_ddc_pin_map);
	} else {
		/* Assuming direct map */
		return vbt_pin;
	}

	/* Looks the VBT pin up in the map. */
	for (i = 0; i < n_entries; i++) {
		if (ddc_pin_map[i] == vbt_pin)
			return i;
	}

	/* The VBT names a pin this platform does not have. */
	I915_VBT_DRM_DBG_KMS(&i915->drm,
		"Ignoring alternate pin: VBT claims DDC pin %d, which is not valid for this platform\n",
		vbt_pin);
	return 0;
}

/* Returns the first DVO port of a DVO port's kind (HDMI, DP or MIPI), or the port itself. */
static u8
i915_dvo_port_type(
	u8 dvo_port)
{
	/* Folds each kind onto its first port. */
	switch (dvo_port) {
	case DVO_PORT_HDMIA:
	case DVO_PORT_HDMIB:
	case DVO_PORT_HDMIC:
	case DVO_PORT_HDMID:
	case DVO_PORT_HDMIE:
	case DVO_PORT_HDMIF:
	case DVO_PORT_HDMIG:
	case DVO_PORT_HDMIH:
	case DVO_PORT_HDMII:
		return DVO_PORT_HDMIA;
	case DVO_PORT_DPA:
	case DVO_PORT_DPB:
	case DVO_PORT_DPC:
	case DVO_PORT_DPD:
	case DVO_PORT_DPE:
	case DVO_PORT_DPF:
	case DVO_PORT_DPG:
	case DVO_PORT_DPH:
	case DVO_PORT_DPI:
		return DVO_PORT_DPA;
	case DVO_PORT_MIPIA:
	case DVO_PORT_MIPIB:
	case DVO_PORT_MIPIC:
	case DVO_PORT_MIPID:
		return DVO_PORT_MIPIA;
	default:
		return dvo_port;
	}
}

/* Returns the DDI port a table maps a DVO port to, or PORT_NONE. */
static enum port
i915_dvo_port_to_port_table(
	int n_ports,
	int n_dvo,
	const int port_mapping_table[][3],
	u8 dvo_port)
{
	enum port port;
	int i;

	/* Looks at every value of every port, up to the port's -1 terminator. */
	for (port = PORT_A; port < n_ports; port++) {
		for (i = 0; i < n_dvo; i++) {
			if (port_mapping_table[port][i] == -1)
				break;

			/* Succeeded: the DVO port is one of this port's values. */
			if (dvo_port == port_mapping_table[port][i])
				return port;
		}
	}

	/* No port has the value. */
	return PORT_NONE;
}

/* Returns the DDI port of a DVO port on this platform, or PORT_NONE. */
static enum port
i915_dvo_port_to_port(
	struct drm_i915_private *i915,
	u8 dvo_port)
{
	enum port port;
	int display_ver;

	/* Picks the platform's table and looks the DVO port up in it. */
	display_ver = i915_vbt_display_ver(i915);
	if (display_ver >= 13) {
		port = i915_dvo_port_to_port_table(ARRAY_SIZE(xelpd_port_mapping),
						   ARRAY_SIZE(xelpd_port_mapping[0]),
						   xelpd_port_mapping,
						   dvo_port);
	} else if (IS_ALDERLAKE_S(i915)) {
		port = i915_dvo_port_to_port_table(ARRAY_SIZE(adls_port_mapping),
						   ARRAY_SIZE(adls_port_mapping[0]),
						   adls_port_mapping,
						   dvo_port);
	} else if (IS_DG1(i915) || IS_ROCKETLAKE(i915)) {
		port = i915_dvo_port_to_port_table(ARRAY_SIZE(rkl_port_mapping),
						   ARRAY_SIZE(rkl_port_mapping[0]),
						   rkl_port_mapping,
						   dvo_port);
	} else {
		port = i915_dvo_port_to_port_table(ARRAY_SIZE(port_mapping),
						   ARRAY_SIZE(port_mapping[0]),
						   port_mapping,
						   dvo_port);
	}

	/* Succeeded: reports the port, or PORT_NONE. */
	return port;
}

/* Returns the DDI port of a DSI DVO port, or PORT_NONE. */
static enum port
i915_dsi_dvo_port_to_port(
	struct drm_i915_private *i915,
	u8 dvo_port)
{
	int display_ver;

	/* MIPI A is port A; MIPI C is port B from display version 11, else port C. */
	switch (dvo_port) {
	case DVO_PORT_MIPIA:
		return PORT_A;
	case DVO_PORT_MIPIC:
		display_ver = i915_vbt_display_ver(i915);
		if (display_ver >= 11)
			return PORT_B;
		return PORT_C;
	default:
		return PORT_NONE;
	}
}

/* Returns the DDI port of a child device, or PORT_NONE. */
static enum port
i915_bios_encoder_port(
	const struct intel_bios_encoder_data *devdata)
{
	struct drm_i915_private *i915;
	const struct child_device_config *child;
	enum port port;
	int display_ver;

	/* Maps the child's DVO port. */
	i915 = devdata->i915;
	child = &devdata->child;
	port = i915_dvo_port_to_port(i915, child->dvo_port);

	/* A DSI port is mapped on its own from display version 11. */
	display_ver = i915_vbt_display_ver(i915);
	if (port == PORT_NONE && display_ver >= 11)
		port = i915_dsi_dvo_port_to_port(i915, child->dvo_port);

	/* Succeeded: reports the port, or PORT_NONE. */
	return port;
}

/* Returns the DP rate limit, in kHz, a BDB 230 encoding stands for; 0 is no limit. */
static int
i915_parse_bdb_230_dp_max_link_rate(
	const int vbt_max_link_rate)
{
	/* Decodes the rate. */
	switch (vbt_max_link_rate) {
	default:
	case BDB_230_VBT_DP_MAX_LINK_RATE_DEF:
		return 0;
	case BDB_230_VBT_DP_MAX_LINK_RATE_UHBR20:
		return 2000000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_UHBR13P5:
		return 1350000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_UHBR10:
		return 1000000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_HBR3:
		return 810000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_HBR2:
		return 540000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_HBR:
		return 270000;
	case BDB_230_VBT_DP_MAX_LINK_RATE_LBR:
		return 162000;
	}
}

/* Returns the DP rate limit, in kHz, a BDB 216 encoding stands for. */
static int
i915_parse_bdb_216_dp_max_link_rate(
	const int vbt_max_link_rate)
{
	/* Decodes the rate. */
	switch (vbt_max_link_rate) {
	default:
	case BDB_216_VBT_DP_MAX_LINK_RATE_HBR3:
		return 810000;
	case BDB_216_VBT_DP_MAX_LINK_RATE_HBR2:
		return 540000;
	case BDB_216_VBT_DP_MAX_LINK_RATE_HBR:
		return 270000;
	case BDB_216_VBT_DP_MAX_LINK_RATE_LBR:
		return 162000;
	}
}

/* Returns a child's DP rate limit in kHz, or 0 for the platform's maximum. */
static int
i915_bios_dp_max_link_rate(
	const struct intel_bios_encoder_data *devdata)
{
	int rate;

	/* A missing child or a VBT older than 216 sets no limit. */
	if (!devdata)
		return 0;
	if (devdata->i915->display.vbt.version < 216)
		return 0;

	/* Decodes the limit in the encoding of the VBT's version. */
	if (devdata->i915->display.vbt.version >= 230) {
		rate = i915_parse_bdb_230_dp_max_link_rate(devdata->child.dp_max_link_rate);
	} else {
		rate = i915_parse_bdb_216_dp_max_link_rate(devdata->child.dp_max_link_rate);
	}

	/* Succeeded: reports the limit. */
	return rate;
}

/* Returns a child's DP lane count limit, or 0 for none. */
static int
i915_bios_dp_max_lane_count(
	const struct intel_bios_encoder_data *devdata)
{
	/* A missing child or a VBT older than 244 sets no limit. */
	if (!devdata)
		return 0;
	if (devdata->i915->display.vbt.version < 244)
		return 0;

	/* Succeeded: the VBT stores the count minus one. */
	return devdata->child.dp_max_lane_count + 1;
}

/* Ignores DVI on port A before display version 12, as Linux does. */
static void
i915_sanitize_device_type(
	struct intel_bios_encoder_data *devdata,
	enum port port)
{
	struct drm_i915_private *i915;
	bool is_hdmi;
	bool is_dvi;
	int display_ver;

	/* Only port A before display version 12 is sanitized. */
	i915 = devdata->i915;
	display_ver = i915_vbt_display_ver(i915);
	if (port != PORT_A || display_ver >= 12)
		return;

	/* A port A without DVI needs nothing. */
	is_dvi = i915_bios_encoder_supports_dvi(devdata);
	if (!is_dvi)
		return;

	/* Drops DVI and HDMI from the child. */
	is_hdmi = i915_bios_encoder_supports_hdmi(devdata);
	I915_VBT_DRM_DBG_KMS(&i915->drm, "VBT claims port A supports DVI%s, ignoring\n",
		is_hdmi ? "/HDMI" : "");
	devdata->child.device_type &= ~DEVICE_TYPE_TMDS_DVI_SIGNALING;
	devdata->child.device_type |= DEVICE_TYPE_NOT_HDMI_OUTPUT;
}

/* Clamps a Broadwell HDMI level shift the HSW VBT sets too high. */
static void
i915_sanitize_hdmi_level_shift(
	struct intel_bios_encoder_data *devdata,
	enum port port)
{
	bool is_dvi;

	/* Only a DVI child has a level shift. */
	is_dvi = i915_bios_encoder_supports_dvi(devdata);
	if (!is_dvi)
		return;

	/*
	 * Some BDW machines (eg. HP Pavilion 15-ab) shipped
	 * with a HSW VBT where the level shifter value goes
	 * up to 11, whereas the BDW max is 9.
	 */
	if (IS_BROADWELL(devdata->i915) && devdata->child.hdmi_level_shifter_value > 9) {
		I915_VBT_DRM_DBG_KMS(&devdata->i915->drm, "Bogus port %c VBT HDMI level shift %d, adjusting to %d\n",
			port_name(port), devdata->child.hdmi_level_shifter_value, 9);
		devdata->child.hdmi_level_shifter_value = 9;
	}
}

/* Tells whether a child has an analog output. */
static bool
i915_bios_encoder_supports_crt(
	const struct intel_bios_encoder_data *devdata)
{
	/* The analog output bit. */
	if ((devdata->child.device_type & DEVICE_TYPE_ANALOG_OUTPUT) == 0)
		return false;

	/* Succeeded: the child has an analog output. */
	return true;
}

/* Tells whether a child signals TMDS (DVI). */
static bool
i915_bios_encoder_supports_dvi(
	const struct intel_bios_encoder_data *devdata)
{
	/* The TMDS signalling bit. */
	if ((devdata->child.device_type & DEVICE_TYPE_TMDS_DVI_SIGNALING) == 0)
		return false;

	/* Succeeded: the child signals TMDS. */
	return true;
}

/* Tells whether a child is an HDMI output: TMDS and not marked as not HDMI. */
static bool
i915_bios_encoder_supports_hdmi(
	const struct intel_bios_encoder_data *devdata)
{
	bool is_dvi;

	/* HDMI needs TMDS. */
	is_dvi = i915_bios_encoder_supports_dvi(devdata);
	if (!is_dvi)
		return false;

	/* A TMDS output marked as not HDMI is DVI only. */
	if ((devdata->child.device_type & DEVICE_TYPE_NOT_HDMI_OUTPUT) != 0)
		return false;

	/* Succeeded: the child is an HDMI output. */
	return true;
}

/* Tells whether a child is a DisplayPort output. */
static bool
i915_bios_encoder_supports_dp(
	const struct intel_bios_encoder_data *devdata)
{
	/* The DisplayPort output bit. */
	if ((devdata->child.device_type & DEVICE_TYPE_DISPLAYPORT_OUTPUT) == 0)
		return false;

	/* Succeeded: the child is a DisplayPort output. */
	return true;
}

/* Tells whether a child is an eDP output: DisplayPort on an internal connector. */
static bool
i915_bios_encoder_supports_edp(
	const struct intel_bios_encoder_data *devdata)
{
	bool is_dp;

	/* eDP is DisplayPort. */
	is_dp = i915_bios_encoder_supports_dp(devdata);
	if (!is_dp)
		return false;

	/* eDP is on an internal connector. */
	if ((devdata->child.device_type & DEVICE_TYPE_INTERNAL_CONNECTOR) == 0)
		return false;

	/* Succeeded: the child is an eDP output. */
	return true;
}

/* Tells whether a child is a MIPI DSI output. */
static bool
i915_bios_encoder_supports_dsi(
	const struct intel_bios_encoder_data *devdata)
{
	/* The MIPI output bit. */
	if ((devdata->child.device_type & DEVICE_TYPE_MIPI_OUTPUT) == 0)
		return false;

	/* Succeeded: the child is a DSI output. */
	return true;
}

/* Tells whether a child is an LSPCON on a platform that has them. */
static bool
i915_bios_encoder_is_lspcon(
	const struct intel_bios_encoder_data *devdata)
{
	/* A missing child is none. */
	if (!devdata)
		return false;

	/* The platform must have LSPCONs, and the child must be one. */
	if (!HAS_LSPCON(devdata->i915))
		return false;
	if (!devdata->child.lspcon)
		return false;

	/* Succeeded: the child is an LSPCON. */
	return true;
}

/* Returns a child's HDMI TMDS clock limit in kHz, or 0 for the platform's. */
static int
i915_bios_hdmi_max_tmds_clock(
	const struct intel_bios_encoder_data *devdata)
{
	/* A missing child or a VBT older than 204 sets no limit. */
	if (!devdata)
		return 0;
	if (devdata->i915->display.vbt.version < 204)
		return 0;

	/* Decodes the limit. */
	switch (devdata->child.hdmi_max_data_rate) {
	default:
		I915_VBT_MISSING_CASE(devdata->child.hdmi_max_data_rate);
		fallthrough;
	case HDMI_MAX_DATA_RATE_PLATFORM:
		return 0;
	case HDMI_MAX_DATA_RATE_594:
		return 594000;
	case HDMI_MAX_DATA_RATE_340:
		return 340000;
	case HDMI_MAX_DATA_RATE_300:
		return 300000;
	case HDMI_MAX_DATA_RATE_297:
		return 297000;
	case HDMI_MAX_DATA_RATE_165:
		return 165000;
	}
}

/* Tells whether a port can exist on the platform (ICL port F only on the SKUs that have it). */
static bool
i915_is_port_valid(
	struct drm_i915_private *i915,
	enum port port)
{
	UNUSED_PARAMETER(i915);

	/*
	 * On some ICL SKUs port F is not present, but broken VBTs mark
	 * the port as present. Only try to initialize port F for the
	 * SKUs that may actually have it.
	 */
	if (port == PORT_F && IS_ICELAKE(i915))
		return IS_ICL_WITH_PORT_F(i915);

	/* Succeeded: the port can exist. */
	return true;
}

/* Logs what the VBT says about a child's port. */
static void
i915_print_ddi_port(
	const struct intel_bios_encoder_data *devdata)
{
	const struct child_device_config *child;
	bool is_dvi, is_hdmi, is_dp, is_edp, is_dsi, is_crt, supports_typec_usb, supports_tbt;
	int dp_boost_level, dp_max_link_rate, hdmi_boost_level, hdmi_level_shift, max_tmds_clock;
	enum port port;

	/* A child without a port has nothing to log. */
	child = &devdata->child;
	port = i915_bios_encoder_port(devdata);
	if (port == PORT_NONE)
		return;

	/* The outputs the child supports. */
	is_dvi = i915_bios_encoder_supports_dvi(devdata);
	is_dp = i915_bios_encoder_supports_dp(devdata);
	is_crt = i915_bios_encoder_supports_crt(devdata);
	is_hdmi = i915_bios_encoder_supports_hdmi(devdata);
	is_edp = i915_bios_encoder_supports_edp(devdata);
	is_dsi = i915_bios_encoder_supports_dsi(devdata);
	supports_typec_usb = i915_bios_encoder_supports_typec_usb(devdata);
	supports_tbt = i915_bios_encoder_supports_tbt(devdata);
	I915_VBT_DRM_DBG_KMS(&devdata->i915->drm,
		"Port %c VBT info: CRT:%d DVI:%d HDMI:%d DP:%d eDP:%d DSI:%d DP++:%d LSPCON:%d USB-Type-C:%d TBT:%d DSC:%d\n",
		port_name(port), is_crt, is_dvi, is_hdmi, is_dp, is_edp, is_dsi,
		i915_bios_encoder_supports_dp_dual_mode(devdata),
		i915_bios_encoder_is_lspcon(devdata),
		supports_typec_usb, supports_tbt,
		devdata->dsc != NULL);

	/* The HDMI level shift. */
	hdmi_level_shift = drv_i915_bios_hdmi_level_shift(devdata);
	if (hdmi_level_shift >= 0) {
		I915_VBT_DRM_DBG_KMS(&devdata->i915->drm,
			"Port %c VBT HDMI level shift: %d\n",
			port_name(port), hdmi_level_shift);
	}

	/* The HDMI TMDS clock limit. */
	max_tmds_clock = i915_bios_hdmi_max_tmds_clock(devdata);
	if (max_tmds_clock) {
		I915_VBT_DRM_DBG_KMS(&devdata->i915->drm,
			"Port %c VBT HDMI max TMDS clock: %d kHz\n",
			port_name(port), max_tmds_clock);
	}

	/* I_boost config for SKL and above */
	dp_boost_level = i915_bios_dp_boost_level(devdata);
	if (dp_boost_level) {
		I915_VBT_DRM_DBG_KMS(&devdata->i915->drm,
			"Port %c VBT (e)DP boost level: %d\n",
			port_name(port), dp_boost_level);
	}
	hdmi_boost_level = i915_bios_hdmi_boost_level(devdata);
	if (hdmi_boost_level) {
		I915_VBT_DRM_DBG_KMS(&devdata->i915->drm,
			"Port %c VBT HDMI boost level: %d\n",
			port_name(port), hdmi_boost_level);
	}

	/* The DP rate limit. */
	dp_max_link_rate = i915_bios_dp_max_link_rate(devdata);
	if (dp_max_link_rate) {
		I915_VBT_DRM_DBG_KMS(&devdata->i915->drm,
			"Port %c VBT DP max link rate: %d\n",
			port_name(port), dp_max_link_rate);
	}

	/*
	 * FIXME need to implement support for VBT
	 * vswing/preemph tables should this ever trigger.
	 */
	(void)I915_VBT_DRM_WARN(&devdata->i915->drm, child->use_vbt_vswing,
		"Port %c asks to use VBT vswing/preemph tables\n",
		port_name(port));
}

/* Sanitizes a child whose port is valid on the platform. */
static void
i915_parse_ddi_port(
	struct intel_bios_encoder_data *devdata)
{
	struct drm_i915_private *i915;
	enum port port;
	bool valid;

	/* A child without a port is left alone. */
	i915 = devdata->i915;
	port = i915_bios_encoder_port(devdata);
	if (port == PORT_NONE)
		return;

	/* A port the platform cannot have is skipped. */
	valid = i915_is_port_valid(i915, port);
	if (!valid) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"VBT reports port %c as supported, but that can't be true: skipping\n",
			port_name(port));
		return;
	}

	/* Sanitizes the device type and the HDMI level shift. */
	i915_sanitize_device_type(devdata, port);
	i915_sanitize_hdmi_level_shift(devdata, port);
}

/* Tells whether the VBT's children carry DDI port information (display version 5 on, or G4X). */
static bool
i915_has_ddi_port_info(
	struct drm_i915_private *i915)
{
	int display_ver;

	/* Display version 5 and later carry it. */
	display_ver = i915_vbt_display_ver(i915);
	if (display_ver >= 5)
		return true;

	/* G4X carries it too. */
	if (IS_G4X(i915))
		return true;

	/* Older platforms do not. */
	return false;
}

/* Sanitizes and then logs every child's port. */
static void
i915_parse_ddi_ports(
	struct drm_i915_private *i915)
{
	struct intel_bios_encoder_data *devdata;
	bool has_info;

	/* Without port information there is nothing to parse. */
	has_info = i915_has_ddi_port_info(i915);
	if (!has_info)
		return;

	/* Sanitizes every child first. */
	list_for_each_entry(devdata, &i915->display.vbt.display_devices, node) {
		i915_parse_ddi_port(devdata);
	}

	/* Then logs every child. */
	list_for_each_entry(devdata, &i915->display.vbt.display_devices, node) {
		i915_print_ddi_port(devdata);
	}
}

/* Parses the general definitions block: the CRT DDC pin and the child devices. */
static void
i915_parse_general_definitions(
	struct drm_i915_private *i915)
{
	const struct bdb_general_definitions *defs;
	struct intel_bios_encoder_data *devdata;
	const struct child_device_config *child;
	int i;
	int child_device_num;
	u8 expected_size;
	u16 block_size;
	int bus_pin;
	bool valid_pin;
	bool empty;

	/* Without the block no devices are defined. */
	defs = i915_bdb_find_section(i915, BDB_GENERAL_DEFINITIONS);
	if (!defs) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"No general definition block is found, no devices defined.\n");
		return;
	}

	/* A block shorter than its header defines nothing. */
	block_size = i915_get_blocksize(defs);
	if (block_size < sizeof(*defs)) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"General definitions block too small (%u)\n",
			block_size);
		return;
	}

	/* The CRT DDC pin, when the platform has it. */
	bus_pin = defs->crt_ddc_gmbus_pin;
	I915_VBT_DRM_DBG_KMS(&i915->drm, "crt_ddc_bus_pin: %d\n", bus_pin);
	valid_pin = i915_vbt_intel_gmbus_is_valid_pin(i915, bus_pin);
	if (valid_pin)
		i915->display.vbt.crt_ddc_pin = bus_pin;

	/* Flag an error for unexpected size, but continue anyway. */
	expected_size = i915_child_device_expected_size(i915);
	if (defs->child_dev_size != expected_size) {
		I915_VBT_DRM_ERR(&i915->drm,
			"Unexpected child device config size %u (expected %u for VBT version %u)\n",
			defs->child_dev_size, expected_size, i915->display.vbt.version);
	}

	/* The legacy sized child device config is the minimum we need. */
	if (defs->child_dev_size < LEGACY_CHILD_DEVICE_CONFIG_SIZE) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"Child device config size %u is too small.\n",
			defs->child_dev_size);
		return;
	}

	/* get the number of child device */
	child_device_num = (block_size - sizeof(*defs)) / defs->child_dev_size;

	/* Copies every child that has a device type onto the device list. */
	for (i = 0; i < child_device_num; i++) {
		child = i915_child_device_ptr(defs, i);
		if (!child->device_type)
			continue;
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"Found VBT child device with type 0x%x\n",
			child->device_type);

		/* Allocates the child's record; an exhausted arena ends the walk. */
		devdata = kzalloc(sizeof(*devdata), GFP_KERNEL);
		if (!devdata)
			break;

		/*
		 * Copy as much as we know (sizeof) and is available
		 * (child_dev_size) of the child device config. Accessing the
		 * data must depend on VBT version.
		 */
		devdata->i915 = i915;
		kern_memcpy(&devdata->child, child,
		       min_t(size_t, defs->child_dev_size, sizeof(*child)));
		i915_list_add_tail(&devdata->node, &i915->display.vbt.display_devices);
	}

	/* Notes a VBT that defined no child. */
	empty = i915_list_empty(&i915->display.vbt.display_devices);
	if (empty) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"no child dev is parsed from VBT\n");
	}
}

/* Returns the child device config size the VBT's version should have. */
static u8
i915_child_device_expected_size(
	struct drm_i915_private *i915)
{
	u8 expected_size;

	BUILD_BUG_ON(sizeof(struct child_device_config) < 39);

	/* Picks the size of the VBT's version. */
	if (i915->display.vbt.version < 106) {
		expected_size = 22;
	} else if (i915->display.vbt.version < 111) {
		expected_size = 27;
	} else if (i915->display.vbt.version < 195) {
		expected_size = LEGACY_CHILD_DEVICE_CONFIG_SIZE;
	} else if (i915->display.vbt.version == 195) {
		expected_size = 37;
	} else if (i915->display.vbt.version <= 215) {
		expected_size = 38;
	} else if (i915->display.vbt.version <= 250) {
		expected_size = 39;
	} else {
		expected_size = sizeof(struct child_device_config);
		I915_VBT_DRM_DBG(&i915->drm,
			"Expected child device config size for VBT version %u not known; assuming %u\n",
			i915->display.vbt.version, expected_size);
	}

	/* Succeeded: reports the expected size. */
	return expected_size;
}

/* Sets the common defaults the VBT may override. */
static void
i915_init_vbt_defaults(
	struct drm_i915_private *i915)
{
	/* The CRT DDC pin. */
	i915->display.vbt.crt_ddc_pin = GMBUS_PIN_VGADDC;

	/* general features */
	i915->display.vbt.int_tv_support = 1;
	i915->display.vbt.int_crt_support = 1;

	/* driver features */
	i915->display.vbt.int_lvds_support = 1;

	/*
	 * Default to using SSC.  Core/SandyBridge/IvyBridge use alternative
	 * (120MHz) reference clock for LVDS.
	 */
	i915->display.vbt.lvds_use_ssc = 1;
	i915->display.vbt.lvds_ssc_freq = i915_bios_ssc_frequency(i915,
								  !HAS_PCH_SPLIT(i915));
	I915_VBT_DRM_DBG_KMS(&i915->drm, "Set default to SSC at %d kHz\n",
		i915->display.vbt.lvds_ssc_freq);
}

/* Sets the panel defaults the VBT may override. */
static void
i915_init_vbt_panel_defaults(
	struct intel_panel *panel)
{
	/* Default to having backlight */
	panel->vbt.backlight.present = true;

	/* LFP panel data */
	panel->vbt.lvds_dither = true;
}

/* Generates the child devices of the non-Type-C DDI ports when there is no VBT. */
static void
i915_init_vbt_missing_defaults(
	struct drm_i915_private *i915)
{
	struct intel_bios_encoder_data *devdata;
	struct child_device_config *child;
	enum phy phy;
	enum port port;
	int ports;
	int has_ddi;
	bool is_tc;

	/* Only a DDI platform or CHV gets defaults. */
	ports = BIT(PORT_A) | BIT(PORT_B) | BIT(PORT_C) |
		BIT(PORT_D) | BIT(PORT_E) | BIT(PORT_F);
	has_ddi = i915_vbt_has_ddi(i915);
	if (!has_ddi && !IS_CHERRYVIEW(i915))
		return;

	/* Generates one child per port of the mask. */
	for (port = PORT_A; port < I915_MAX_PORTS; port++) {
		if ((ports & BIT(port)) == 0)
			continue;

		/*
		 * VBT has the TypeC mode (native,TBT/USB) and we don't want
		 * to detect it.
		 */
		phy = i915_vbt_intel_port_to_phy(i915, port);
		is_tc = i915_vbt_intel_phy_is_tc(i915, phy);
		if (is_tc)
			continue;

		/* Create fake child device config */
		devdata = kzalloc(sizeof(*devdata), GFP_KERNEL);
		if (!devdata)
			break;
		devdata->i915 = i915;
		child = &devdata->child;

		/* The DVO port. */
		if (port == PORT_F) {
			child->dvo_port = DVO_PORT_HDMIF;
		} else if (port == PORT_E) {
			child->dvo_port = DVO_PORT_HDMIE;
		} else {
			child->dvo_port = DVO_PORT_HDMIA + port;
		}

		/* The output types. */
		if (port != PORT_A && port != PORT_E)
			child->device_type |= DEVICE_TYPE_TMDS_DVI_SIGNALING;
		if (port != PORT_E)
			child->device_type |= DEVICE_TYPE_DISPLAYPORT_OUTPUT;
		if (port == PORT_A)
			child->device_type |= DEVICE_TYPE_INTERNAL_CONNECTOR;

		/* Puts the child on the device list. */
		i915_list_add_tail(&devdata->node, &i915->display.vbt.display_devices);
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"Generating default VBT child device with type 0x04%x on port %c\n",
			child->device_type, port_name(port));
	}

	/* Bypass some minimum baseline VBT version checks */
	i915->display.vbt.version = 155;
}

/* Returns the BDB header a VBT header points at. */
static const struct bdb_header *
i915_get_bdb_header(
	const struct vbt_header *vbt)
{
	/* Succeeded: the BDB is bdb_offset bytes into the VBT. */
	return (const struct bdb_header *)((const u8 *)vbt + vbt->bdb_offset);
}

/*
 * Parses the VBT, or takes the defaults without one (intel_bios_init()).
 *
 * The VBT pointer comes from drv_i915_vbt_provider_get() instead of the
 * OpRegion and the SPI / PCI ROM lookups of Linux.
 */
static void
i915_bios_init(
	struct drm_i915_private *i915)
{
	const struct vbt_header *vbt;

	/* The VBT the acquisition chose, or NULL. */
	vbt = drv_i915_vbt_provider_get(i915);

	/* Starts both lists empty. */
	i915_init_list_head(&i915->display.vbt.display_devices);
	i915_init_list_head(&i915->display.vbt.bdb_blocks);

	/* A device without a display has no VBT to parse. */
	if (!HAS_DISPLAY(i915)) {
		I915_VBT_DRM_DBG_KMS(&i915->drm,
			"Skipping VBT init due to disabled display.\n");
		return;
	}

	/* The defaults the VBT may override. */
	i915_init_vbt_defaults(i915);

	/* Parses the VBT, or generates the children a missing VBT stands for. */
	if (vbt == NULL) {
		I915_VBT_DRM_INFO(&i915->drm,
			"Failed to find VBIOS tables (VBT)\n");
		i915_init_vbt_missing_defaults(i915);
	} else {
		i915_bios_parse_vbt(i915, vbt);
	}

	/* Further processing on pre-parsed or generated child device data */
	i915_parse_ddi_ports(i915);
}

/*
 * Copies the VBT's blocks and parses the general blocks.
 *
 * The SDVO device mapping and the compression parameters are not parsed
 * (not reachable for the ADL-P eDP).
 */
static void
i915_bios_parse_vbt(
	struct drm_i915_private *i915,
	const struct vbt_header *vbt)
{
	const struct bdb_header *bdb;

	/* Records the BDB version. */
	bdb = i915_get_bdb_header(vbt);
	i915->display.vbt.version = bdb->version;
	I915_VBT_DRM_DBG_KMS(&i915->drm,
		"VBT signature \"%.*s\", BDB version %d\n",
		(int)sizeof(vbt->signature), vbt->signature, i915->display.vbt.version);

	/* Copies the blocks. */
	i915_init_bdb_blocks(i915, bdb);

	/* Grab useful general definitions */
	i915_parse_general_features(i915);
	i915_parse_general_definitions(i915);
	i915_parse_driver_features(i915);
}

/*
 * Parses the panel data of a child (intel_bios_init_panel()).
 *
 * The SDVO panel data, PSR and MIPI blocks are not parsed (not reachable for
 * the ADL-P eDP).
 */
static void
i915_bios_init_panel(
	struct drm_i915_private *i915,
	struct intel_panel *panel,
	const struct intel_bios_encoder_data *devdata,
	const struct drm_edid *drm_edid,
	bool use_fallback)
{
	/* already have it? */
	if (panel->vbt.panel_type >= 0) {
		(void)I915_VBT_DRM_WARN_ON(&i915->drm, !use_fallback);
		return;
	}

	/* Finds the panel type; an early pass may find none. */
	panel->vbt.panel_type = i915_get_panel_type(i915, devdata,
						    drm_edid, use_fallback);
	if (panel->vbt.panel_type < 0) {
		(void)I915_VBT_DRM_WARN_ON(&i915->drm, use_fallback);
		return;
	}

	/* Parses the panel blocks over the defaults. */
	i915_init_vbt_panel_defaults(panel);
	i915_parse_panel_options(i915, panel);
	i915_parse_generic_dtd(i915, panel);
	i915_parse_lfp_data(i915, panel);
	i915_parse_lfp_backlight(i915, panel);
	i915_parse_panel_driver_features(i915, panel);
	i915_parse_power_conservation_features(i915, panel);
	i915_parse_edp(i915, panel);
}

/* Parses the panel data of a child without an EDID (intel_bios_init_panel_early()). */
static void
i915_bios_init_panel_early(
	struct drm_i915_private *i915,
	struct intel_panel *panel,
	const struct intel_bios_encoder_data *devdata)
{
	/* The early pass has no EDID and no fallback. */
	i915_bios_init_panel(i915, panel, devdata, NULL, false);
}

/* Parses the panel data of a child with its EDID (intel_bios_init_panel_late()). */
static void
i915_bios_init_panel_late(
	struct drm_i915_private *i915,
	struct intel_panel *panel,
	const struct intel_bios_encoder_data *devdata,
	const struct drm_edid *drm_edid)
{
	/* The late pass has the EDID and the fallback. */
	i915_bios_init_panel(i915, panel, devdata, drm_edid, true);
}

/* Takes the parser's children and blocks off their lists (intel_bios_driver_remove()). */
static void
i915_bios_driver_remove(
	struct drm_i915_private *i915)
{
	struct intel_bios_encoder_data *devdata, *next_devdata;
	struct bdb_block_entry *entry, *next_entry;

	/* Takes every child off the list. */
	list_for_each_entry_safe(devdata, next_devdata, &i915->display.vbt.display_devices, node) {
		i915_list_del(&devdata->node);
		i915_vbt_kfree(devdata->dsc);
		i915_vbt_kfree(devdata);
	}

	/* Takes every block off the list. */
	list_for_each_entry_safe(entry, next_entry, &i915->display.vbt.bdb_blocks, node) {
		i915_list_del(&entry->node);
		i915_vbt_kfree(entry);
	}
}

/*
 * Releases the modes and DSI data of a panel (intel_bios_fini_panel()).
 *
 * No caller in this driver: the arena releases the panel memory as a whole.
 */
static void
i915_bios_fini_panel(
	struct intel_panel *panel)
{
	/* Releases each piece and forgets it. */
	i915_vbt_kfree(panel->vbt.sdvo_lvds_vbt_mode);
	panel->vbt.sdvo_lvds_vbt_mode = NULL;
	i915_vbt_kfree(panel->vbt.lfp_lvds_vbt_mode);
	panel->vbt.lfp_lvds_vbt_mode = NULL;
	i915_vbt_kfree(panel->vbt.dsi.data);
	panel->vbt.dsi.data = NULL;
	i915_vbt_kfree(panel->vbt.dsi.pps);
	panel->vbt.dsi.pps = NULL;
	i915_vbt_kfree(panel->vbt.dsi.config);
	panel->vbt.dsi.config = NULL;
	i915_vbt_kfree(panel->vbt.dsi.deassert_seq);
	panel->vbt.dsi.deassert_seq = NULL;
}

/*
 * Tells whether the VBT names a child on a port (intel_bios_is_port_present()).
 *
 * No caller in this driver.
 */
static bool
i915_bios_is_port_present(
	struct drm_i915_private *i915,
	enum port port)
{
	const struct intel_bios_encoder_data *devdata;
	const struct child_device_config *child;
	bool has_info;
	bool warned;
	bool valid;
	enum port child_port;

	/* Without port information every port is taken as present. */
	has_info = i915_has_ddi_port_info(i915);
	warned = I915_VBT_WARN_ON(!has_info);
	if (warned)
		return true;

	/* A port the platform cannot have is not present. */
	valid = i915_is_port_valid(i915, port);
	if (!valid)
		return false;

	/* Looks for a child on the port. */
	list_for_each_entry(devdata, &i915->display.vbt.display_devices, node) {
		child = &devdata->child;
		child_port = i915_dvo_port_to_port(i915, child->dvo_port);
		if (child_port == port)
			return true;
	}

	/* No child is on the port. */
	return false;
}

/* Tells whether a child is a DP++ (dual mode) port. */
static bool
i915_bios_encoder_supports_dp_dual_mode(
	const struct intel_bios_encoder_data *devdata)
{
	const struct child_device_config *child;
	bool is_dp;
	bool is_hdmi;
	u8 dvo_type;

	/* A missing child is none. */
	child = &devdata->child;
	if (!devdata)
		return false;

	/* DP++ is both DisplayPort and HDMI. */
	is_dp = i915_bios_encoder_supports_dp(devdata);
	if (!is_dp)
		return false;
	is_hdmi = i915_bios_encoder_supports_hdmi(devdata);
	if (!is_hdmi)
		return false;

	/* A DP DVO port is DP++. */
	dvo_type = i915_dvo_port_type(child->dvo_port);
	if (dvo_type == DVO_PORT_DPA)
		return true;

	/* Only accept a HDMI dvo_port as DP++ if it has an AUX channel */
	if (dvo_type == DVO_PORT_HDMIA &&
	    child->aux_channel != 0)
		return true;

	/* Any other port is not DP++. */
	return false;
}

/* Returns the AUX channel a VBT AUX value names on this platform, or AUX_CH_NONE. */
static enum aux_ch
i915_map_aux_ch(
	struct drm_i915_private *i915,
	u8 aux_channel)
{
	const u8 *aux_ch_map;
	int n_entries;
	int display_ver;
	int i;

	/* Picks the platform's map. */
	display_ver = i915_vbt_display_ver(i915);
	if (display_ver >= 13) {
		aux_ch_map = adlp_aux_ch_map;
		n_entries = ARRAY_SIZE(adlp_aux_ch_map);
	} else if (IS_ALDERLAKE_S(i915)) {
		aux_ch_map = adls_aux_ch_map;
		n_entries = ARRAY_SIZE(adls_aux_ch_map);
	} else if (IS_DG1(i915) || IS_ROCKETLAKE(i915)) {
		aux_ch_map = rkl_aux_ch_map;
		n_entries = ARRAY_SIZE(rkl_aux_ch_map);
	} else {
		aux_ch_map = direct_aux_ch_map;
		n_entries = ARRAY_SIZE(direct_aux_ch_map);
	}

	/* Looks the VBT value up in the map. */
	for (i = 0; i < n_entries; i++) {
		if (aux_ch_map[i] == aux_channel)
			return i;
	}

	/* The VBT names a channel this platform does not have. */
	I915_VBT_DRM_DBG_KMS(&i915->drm,
		"Ignoring alternate AUX CH: VBT claims AUX 0x%x, which is not valid for this platform\n",
		aux_channel);
	return AUX_CH_NONE;
}

/* Returns a child's AUX channel, or AUX_CH_NONE. */
static enum aux_ch
i915_bios_dp_aux_ch(
	const struct intel_bios_encoder_data *devdata)
{
	enum aux_ch aux_ch;

	/* A missing child, or one without an AUX value, has none. */
	if (!devdata)
		return AUX_CH_NONE;
	if (!devdata->child.aux_channel)
		return AUX_CH_NONE;

	/* Maps the value. */
	aux_ch = i915_map_aux_ch(devdata->i915, devdata->child.aux_channel);

	/* Succeeded: reports the channel, or AUX_CH_NONE. */
	return aux_ch;
}

/*
 * Tells whether another DP child shares a child's AUX channel
 * (intel_bios_dp_has_shared_aux_ch()).
 *
 * No caller in this driver.
 */
static bool
i915_bios_dp_has_shared_aux_ch(
	const struct intel_bios_encoder_data *devdata)
{
	struct drm_i915_private *i915;
	struct intel_bios_encoder_data *other;
	u8 aux_channel;
	int count;
	bool is_dp;

	/* A missing child, or one without an AUX value, shares nothing. */
	if (!devdata)
		return false;
	if (!devdata->child.aux_channel)
		return false;

	/* Counts the DP children on the same AUX value, the child included. */
	i915 = devdata->i915;
	aux_channel = devdata->child.aux_channel;
	count = 0;
	list_for_each_entry(other, &i915->display.vbt.display_devices, node) {
		is_dp = i915_bios_encoder_supports_dp(other);
		if (is_dp && aux_channel == other->child.aux_channel)
			count++;
	}

	/* The channel is shared when more than one child uses it. */
	if (count <= 1)
		return false;

	/* Succeeded: the channel is shared. */
	return true;
}

/* Returns a child's DP I_boost, or 0. */
static int
i915_bios_dp_boost_level(
	const struct intel_bios_encoder_data *devdata)
{
	u8 boost;

	/* A missing child, a VBT older than 196 or a child without I_boost has none. */
	if (!devdata)
		return 0;
	if (devdata->i915->display.vbt.version < 196)
		return 0;
	if (!devdata->child.iboost)
		return 0;

	/* Translates the level. */
	boost = i915_translate_iboost(devdata->child.dp_iboost_level);

	/* Succeeded: reports the I_boost. */
	return boost;
}

/* Returns a child's HDMI I_boost, or 0. */
static int
i915_bios_hdmi_boost_level(
	const struct intel_bios_encoder_data *devdata)
{
	u8 boost;

	/* A missing child, a VBT older than 196 or a child without I_boost has none. */
	if (!devdata)
		return 0;
	if (devdata->i915->display.vbt.version < 196)
		return 0;
	if (!devdata->child.iboost)
		return 0;

	/* Translates the level. */
	boost = i915_translate_iboost(devdata->child.hdmi_iboost_level);

	/* Succeeded: reports the I_boost. */
	return boost;
}

/* Returns a child's GMBUS pin, or 0. */
static int
i915_bios_hdmi_ddc_pin(
	const struct intel_bios_encoder_data *devdata)
{
	u8 pin;

	/* A missing child, or one without a DDC pin, has none. */
	if (!devdata)
		return 0;
	if (!devdata->child.ddc_pin)
		return 0;

	/* Maps the pin. */
	pin = i915_map_ddc_pin(devdata->i915, devdata->child.ddc_pin);

	/* Succeeded: reports the pin, or 0. */
	return pin;
}

/* Tells whether a child is a USB Type-C port (from BDB 195). */
static bool
i915_bios_encoder_supports_typec_usb(
	const struct intel_bios_encoder_data *devdata)
{
	/* Older VBTs do not say. */
	if (devdata->i915->display.vbt.version < 195)
		return false;
	if (!devdata->child.dp_usb_type_c)
		return false;

	/* Succeeded: the child is a USB Type-C port. */
	return true;
}

/* Tells whether a child is a Thunderbolt port (from BDB 209). */
static bool
i915_bios_encoder_supports_tbt(
	const struct intel_bios_encoder_data *devdata)
{
	/* Older VBTs do not say. */
	if (devdata->i915->display.vbt.version < 209)
		return false;
	if (!devdata->child.tbt)
		return false;

	/* Succeeded: the child is a Thunderbolt port. */
	return true;
}

/* Tells whether a child's lanes are reversed. */
static bool
i915_bios_encoder_lane_reversal(
	const struct intel_bios_encoder_data *devdata)
{
	/* A missing child is not reversed. */
	if (!devdata)
		return false;
	if (!devdata->child.lane_reversal)
		return false;

	/* Succeeded: the lanes are reversed. */
	return true;
}

/* Tells whether a child's HPD is inverted. */
static bool
i915_bios_encoder_hpd_invert(
	const struct intel_bios_encoder_data *devdata)
{
	/* A missing child is not inverted. */
	if (!devdata)
		return false;
	if (!devdata->child.hpd_invert)
		return false;

	/* Succeeded: the HPD is inverted. */
	return true;
}

/* Returns the child on a port, or NULL. */
static const struct intel_bios_encoder_data *
i915_bios_encoder_data_lookup(
	struct drm_i915_private *i915,
	enum port port)
{
	struct intel_bios_encoder_data *devdata;
	enum port child_port;

	/* Looks for the child on the port. */
	list_for_each_entry(devdata, &i915->display.vbt.display_devices, node) {
		child_port = i915_bios_encoder_port(devdata);
		if (child_port == port)
			return devdata;
	}

	/* No child is on the port. */
	return NULL;
}

/*
 * Calls a function for every child (intel_bios_for_each_encoder()).
 *
 * No caller in this driver.
 */
static void
i915_bios_for_each_encoder(
	struct drm_i915_private *i915,
	void (*func)(struct drm_i915_private *i915, const struct intel_bios_encoder_data *devdata))
{
	struct intel_bios_encoder_data *devdata;

	/* Hands every child to the function. */
	list_for_each_entry(devdata, &i915->display.vbt.display_devices, node) {
		func(i915, devdata);
	}
}
