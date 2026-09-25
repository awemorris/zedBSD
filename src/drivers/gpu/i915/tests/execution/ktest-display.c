/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The in-kernel tests of the display's data sources, power and firmware.
 *
 * The part covers the DRM device management and the per-crtc vblank state,
 * the VBT checks and parse, the VGA decode, the power-domain map and the
 * power-well operations with the asynchronous put, the combo PHYs, the
 * CDCLK bring-up and change, the display-core bring-up, the DMC firmware
 * (provider, parse, load, asynchronous loader) and the display software
 * state.
 *
 * The device is running while the suite runs, so nothing here touches the
 * live display.  Every display object the code under test works on lives in
 * a display of the test's own, allocated for the length of the part: the
 * display code finds the display a power-well context, a display core, a
 * VGA client or a DMC parse belongs to by its position in struct
 * i915_display, so the objects have to sit inside one.  The registers the
 * code reads and writes are a register model; the legacy VGA ports are a
 * recording stand-in.
 */

#include "firmware-override.h"
#include "ktest.h"
#include <kern/kcrt.h>

#include "../../display/internal.h"
#include "../../display/clock.h"
#include "../../display/display.h"
#include "../../display/dmc.h"
#include "../../display/phy.h"
#include "../../display/power.h"
#include "../../display/state.h"
#include "../../display/takeover.h"
#include "../../display/vbt-parse.h"
#include "../../display/watermark.h"
#include "../../firmware.h"
#include "../../mmio.h"
#include "../../trace.h"
#include "../../workqueue.h"

#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stdint.h>

/* The fuse status the power-well enable waits on (SKL_FUSE_STATUS). */
#define I915_KTEST_FUSE_STATUS		0x42000U

/* The PCODE mailbox, its two data registers and the READY bit of the mailbox. */
#define I915_KTEST_PCODE_MAILBOX	0x138124U
#define I915_KTEST_PCODE_DATA		0x138128U
#define I915_KTEST_PCODE_DATA1		0x13812cU
#define I915_KTEST_PCODE_READY		0x80000000U

/* The power-well control registers: CTL1 is the BIOS request, CTL2 the driver's. */
#define I915_KTEST_PW_CTL1		0x45400U
#define I915_KTEST_PW_CTL2		0x45404U
#define I915_KTEST_AUX_CTL1		0x45440U
#define I915_KTEST_AUX_CTL2		0x45444U
#define I915_KTEST_DDI_CTL1		0x45450U
#define I915_KTEST_DDI_CTL2		0x45454U

/* The request bits of a power-well control register; each well's state bit is the bit below. */
#define I915_KTEST_PW_REQUEST_BITS	0xAAAAAAAAU

/* The request and state bits of well PW_A (control index 5). */
#define I915_KTEST_PW_A_REQUEST		(0x2U << 10)
#define I915_KTEST_PW_A_STATE		(0x1U << 10)

/* The four DBUF slice control registers (DBUF_CTL_S1 to S4). */
#define I915_KTEST_DBUF_CTL_S1		0x45008U
#define I915_KTEST_DBUF_CTL_S2		0x44FE8U
#define I915_KTEST_DBUF_CTL_S3		0x44300U
#define I915_KTEST_DBUF_CTL_S4		0x44304U

/* The request and state bits of a DBUF slice. */
#define I915_KTEST_DBUF_POWER_REQUEST	31U
#define I915_KTEST_DBUF_POWER_STATE	30U

/* The display engine PLL (BXT_DE_PLL_ENABLE) and its enable/lock and frequency request/ack bits. */
#define I915_KTEST_DE_PLL_ENABLE	0x46070U
#define I915_KTEST_DE_PLL_PLL_ENABLE	31U
#define I915_KTEST_DE_PLL_LOCK		30U
#define I915_KTEST_DE_PLL_FREQ_REQ	23U
#define I915_KTEST_DE_PLL_FREQ_ACK	22U

/* The CDCLK control register, and the DSSM strap that names a 38.4 MHz reference. */
#define I915_KTEST_CDCLK_CTL		0x46000U
#define I915_KTEST_SKL_DSSM		0x51004U
#define I915_KTEST_DSSM_REF_38400	(2U << 29)

/* The BW_BUDDY page mask register the display-core bring-up programs. */
#define I915_KTEST_BW_BUDDY_PAGE_MASK	0x45134U

/* Combo PHY A: COMP_DW0 (COMP_INIT), COMP_DW8 (IREFGEN), CL_DW5 (CL_POWER_DOWN) and COMP_DW9 (procmon). */
#define I915_KTEST_PHY_A_COMP_DW0	0x162100U
#define I915_KTEST_PHY_A_COMP_DW8	0x162120U
#define I915_KTEST_PHY_A_CL_DW5		0x162014U
#define I915_KTEST_PHY_A_COMP_DW9	0x162124U

/* The combo PHY group registers and the lane-0 registers a group write reaches. */
#define I915_KTEST_PHY_A_GRP_TX_DW8	0x1626A0U
#define I915_KTEST_PHY_A_LN0_TX_DW8	0x1628A0U
#define I915_KTEST_PHY_A_GRP_PCS_DW1	0x162604U
#define I915_KTEST_PHY_A_LN0_PCS_DW1	0x162804U
#define I915_KTEST_PHY_B_GRP_TX_DW8	0x6C6A0U
#define I915_KTEST_PHY_B_LN0_TX_DW8	0x6C8A0U
#define I915_KTEST_PHY_B_GRP_PCS_DW1	0x6C604U
#define I915_KTEST_PHY_B_LN0_PCS_DW1	0x6C804U

/* How many registers the model stores, and how many writes it records in order. */
#define I915_KTEST_REG_SLOTS		96U

/* A scripted PCODE field that accepts any value. */
#define I915_KTEST_PCODE_ANY		0xffffffffU

/*
 * The size of the DMC reference blob, and the name it is requested by.  The
 * i915-firmware package installs it as /lib/firmware/i915/adlp_dmc.bin.
 */
#define I915_KTEST_DMC_SIZE		79088U
#define I915_KTEST_DMC_NAME		"i915/adlp_dmc.bin"

/* The size of the image the test's firmware replacement serves. */
#define I915_KTEST_FIRMWARE_TEST_IMAGE	16U

/*
 * Where the MAIN DMC header's version byte sits in the reference blob: the
 * CSS and package headers (528 bytes), then the MAIN entry at dword 6301,
 * then byte 5 of its header.
 */
#define I915_KTEST_DMC_MAIN_VERSION_BYTE	(528U + 6301U * 4U + 5U)

/* The size of the synthetic VBT buffer. */
#define I915_KTEST_VBT_BUFFER		128U

/*
 * One scripted PCODE transaction.
 *
 * The register model checks the command and the data the driver wrote
 * before the mailbox, and answers with the data and status listed here.
 */
struct i915_ktest_pcode_txn {
	/* The expected command (the mailbox without READY), or I915_KTEST_PCODE_ANY. */
	uint32_t expected_mailbox;

	/* The expected DATA written before the mailbox, or I915_KTEST_PCODE_ANY. */
	uint32_t expected_data;

	/* The DATA and DATA1 the reader gets back. */
	uint32_t reply_data;
	uint32_t reply_data1;

	/* The status byte the mailbox shows once READY clears. */
	uint8_t status;

	/* How many mailbox reads keep READY set first (0 answers at once). */
	uint8_t ready_delay;
};

/*
 * The register model the code under test reads and writes.
 *
 * It answers the power-well control, fuse, PCODE, DBUF, DE PLL and combo
 * PHY registers the way the hardware does, stores every other register, and
 * records the writes in order.  One instance lives in the fixture; a test
 * clears it before each case.
 */
struct i915_ktest_fake_mmio {
	/* The PCODE mailbox and its two data registers. */
	uint32_t mailbox;
	uint32_t data;
	uint32_t data1;

	/* When nonzero, every PCODE transaction ends with this status byte. */
	int pcode_sticky_status;

	/* How many unscripted PCODE transactions were started. */
	unsigned pcode_txn_count;

	/* The scripted PCODE transactions (NULL: none), and how far the driver got. */
	const struct i915_ktest_pcode_txn *ptxn;
	unsigned ptxn_len;
	unsigned ptxn_i;

	/* Set once a transaction was unexpected or carried the wrong command or data. */
	int ptxn_bad;

	/* The answer of the running scripted transaction, and the reads left before it shows. */
	uint32_t ptxn_pd;
	uint32_t ptxn_pd1;
	uint8_t ptxn_pstatus;
	unsigned ptxn_rc;

	/* The driver's (CTL2) and the BIOS's (CTL1) request bits of the three well registers. */
	uint32_t pw_hsw_req;
	uint32_t pw_aux_req;
	uint32_t pw_ddi_req;
	uint32_t pw_hsw_bios;
	uint32_t pw_aux_bios;
	uint32_t pw_ddi_bios;

	/* When set, a requested well never reports its state (no acknowledge). */
	int pw_no_ack;

	/* The stored registers: offsets and values. */
	uint32_t gen_off[I915_KTEST_REG_SLOTS];
	uint32_t gen_val[I915_KTEST_REG_SLOTS];
	unsigned gen_n;

	/* The first writes in order, and how many writes there were in all. */
	uint32_t wt_off[I915_KTEST_REG_SLOTS];
	uint32_t wt_val[I915_KTEST_REG_SLOTS];
	unsigned wt_n;
	unsigned wt_total;

	/* The fuse status: which power-gate fuses are distributed. */
	uint32_t fuse_status;
};

/*
 * The recording stand-in for the legacy VGA ports.
 *
 * It records the order of the arbiter get, the port read, the port write
 * and the put that the VGA reset makes, so no real VGA port is touched.
 */
struct i915_ktest_vga_recorder {
	/* The calls in order: 1 get, 2 read, 3 write, 4 put. */
	int sequence[8];
	unsigned count;

	/* The last value read and written. */
	unsigned char last_read;
	unsigned char last_written;

	/* Whether the resource was taken and given back. */
	int got;
	int put;
};

/*
 * The recording stand-in for the delayed work behind the asynchronous
 * power put: it remembers whether the work is queued and with what delay.
 */
struct i915_ktest_async_recorder {
	/* Whether the work is queued, and the delay it was last queued with. */
	int queued;
	int last_delay;

	/* How many queue, cancel and synchronous cancel requests came in. */
	unsigned queue_calls;
	unsigned cancel_calls;
	unsigned cancel_sync_calls;
};

/*
 * Everything the part works on, allocated for its length.
 *
 * The display is the test's own and is never bound to a device; the other
 * members are the register model, the stand-ins and the scratch storage
 * that is too large for the kernel stack.
 */
struct i915_ktest_display_fixture {
	/* The test's display: its power domains, clocks, DMC and state are the objects under test. */
	struct i915_display display;

	/* The register model and the register access that reaches it. */
	struct i915_ktest_fake_mmio fake;
	struct i915_mmio mmio;

	/* The legacy VGA port stand-in and the accessor that reaches it. */
	struct i915_ktest_vga_recorder vga;
	struct i915_vga_io_ops vga_ops;

	/* The delayed-work stand-in of the asynchronous power put. */
	struct i915_ktest_async_recorder async;

	/* How many managed DRM actions ran. */
	unsigned drm_actions_ran;

	/* The PCODE sideband lock the CDCLK and bandwidth code take. */
	struct mutex sb_lock;

	/* The memory bandwidth table of the SAGV cases. */
	struct i915_bw_state bw;

	/* A second display state for the cases that compare branches. */
	struct i915_display_state scratch_state;

	/* The VBT record and the synthetic VBT. */
	struct i915_vbt_state vbt;
	uint8_t vbt_buffer[I915_KTEST_VBT_BUFFER];

	/* The digest of the SHA-256 case. */
	uint8_t sha[32];

	/* A corrupted copy of the DMC blob. */
	uint8_t dmc_badcopy[I915_KTEST_DMC_SIZE];
};

/*
 * The SHA-256 of the DMC reference blob (linux-firmware dc85cced,
 * i915/adlp_dmc.bin v2.20), which the i915-firmware package pins too.
 *
 * The table never changes.
 */
static const uint8_t i915_ktest_dmc_sha256[32] = {
	0x35, 0x16, 0xde, 0x2e, 0x13, 0x4d, 0xdc, 0xf3, 0xb3, 0x19, 0xc7, 0x5d, 0x2e, 0x43, 0x77, 0x79,
	0xfe, 0xcb, 0xd5, 0x8c, 0xbb, 0x77, 0x23, 0x4b, 0xd6, 0xf2, 0x97, 0xc5, 0x44, 0xe9, 0x2c, 0xcb
};

#ifdef I915_TEST_VBT
/*
 * The test build's explicit VBT (vendor/intel-vbt/README.md), which the
 * validation check reads whole and cut short.
 *
 * XXX: a test crutch for the QEMU passthrough guest without an OpRegion;
 * delete it with the explicit VBT once the GPU tests run on bare metal.
 */
static const uint8_t i915_ktest_vbt_dell_latitude_5330[] = {
#include "vendor/intel-vbt/dell-latitude-5330-1028-0b02.inc"
};
#endif

static uint32_t i915_ktest_fake_read32(void *context, uint32_t offset);
static void i915_ktest_fake_write32(void *context, uint32_t offset, uint32_t value);
static void i915_ktest_fake_forcewake_request(void *context, int domain, int wake);
static int i915_ktest_fake_forcewake_ack(void *context, int domain);
static uint32_t i915_ktest_fake_well_control(const struct i915_ktest_fake_mmio *fake, uint32_t request, uint32_t bios);
static void i915_ktest_fake_pcode_countdown(struct i915_ktest_fake_mmio *fake);
static void i915_ktest_fake_pcode_start(struct i915_ktest_fake_mmio *fake, uint32_t value);
static void i915_ktest_fake_pcode_scripted(struct i915_ktest_fake_mmio *fake, uint32_t value);
static uint32_t i915_ktest_follow_bit(uint32_t value, unsigned request_bit, unsigned status_bit);
static void i915_ktest_fake_record(struct i915_ktest_fake_mmio *fake, uint32_t offset, uint32_t value);
static int i915_ktest_fake_find(const struct i915_ktest_fake_mmio *fake, uint32_t offset, uint32_t value, uint32_t mask);
static uint32_t i915_ktest_fake_get(const struct i915_ktest_fake_mmio *fake, uint32_t offset);
static void i915_ktest_fake_set(struct i915_ktest_fake_mmio *fake, uint32_t offset, uint32_t value);
static void i915_ktest_mmio_open(struct i915_ktest_display_fixture *fx);
static int i915_ktest_vga_get(void *context, int resource);
static unsigned char i915_ktest_vga_in8(void *context, unsigned short port);
static void i915_ktest_vga_out8(void *context, unsigned short port, unsigned char value);
static void i915_ktest_vga_put(void *context, int resource);
static void i915_ktest_vga_record(struct i915_ktest_vga_recorder *recorder, int call);
static int i915_ktest_async_queue(void *context, int delay_ms);
static int i915_ktest_async_cancel(void *context, int sync);
static void i915_ktest_drm_action(void *arg);
static int i915_ktest_dmc_bad_request(void *context, struct i915_firmware *firmware, const char *name);
static int i915_ktest_firmware_image_request(void *context, struct i915_firmware *firmware, const char *name);
static void i915_ktest_pwc_reset(struct i915_ktest_display_fixture *fx);
static void i915_ktest_cdclk_reset(struct i915_ktest_display_fixture *fx);
static unsigned i915_ktest_well_refcounts(const struct i915_power_domains *pd);
static unsigned i915_ktest_make_vbt(struct i915_ktest_display_fixture *fx, int good_signature);
static void i915_ktest_drm(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_drm_bounds(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_bios(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
#ifdef I915_TEST_VBT
static void i915_ktest_bios_blob(struct i915_ktest *ktest);
#endif
static void i915_ktest_vga(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_power_map(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_power_wells(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_power_well_state(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_power_domains(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_power_async(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_power_async_second(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_power_well_edges(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_combo_phy(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_cdclk_init(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_cdclk_readout(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_cdclk_full_setup(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_cdclk_crawl(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_cdclk_notify_fail(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_core_setup(struct i915_ktest_display_fixture *fx);
static void i915_ktest_core_normal(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_core_normal_parts(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_core_remove(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_core_preserve(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_dmc_provider(struct i915_ktest *ktest);
static void i915_ktest_dmc_parse(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_dmc_parse_log(const struct i915_dmc *dmc, int parse_result, int main_present);
static void i915_ktest_dmc_load(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static uint64_t i915_ktest_dmc_expected_sum(const struct i915_dmc *dmc, const uint8_t *blob);
static void i915_ktest_dmc_loader_setup(struct i915_ktest_display_fixture *fx);
static void i915_ktest_dmc_async(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_dmc_async_cases(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_dmc_bad_fw(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_dmc_bad_fw_cases(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_state_bw_fixture(struct i915_bw_state *bw);
static void i915_ktest_state_mode(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static unsigned i915_ktest_state_mode_ladders(struct i915_display_state *t);
static void i915_ktest_state_index(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_state_objects(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_state_sagv_skip(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_state_sagv_fail(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_state_color(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_state_quirks(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static unsigned i915_ktest_state_fbc_cases(struct i915_display_state *t);
static unsigned i915_ktest_state_fbc_ladder(struct i915_display_state *t);
static void i915_ktest_state_fbc(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);
static void i915_ktest_state_fini(struct i915_ktest *ktest, struct i915_ktest_display_fixture *fx);

/*
 * The register access of the register model.
 *
 * It is constant and shared by every case.
 */
static const struct i915_mmio_ops i915_ktest_fake_mmio_ops = {
	i915_ktest_fake_read32,
	i915_ktest_fake_write32,
	i915_ktest_fake_forcewake_request,
	i915_ktest_fake_forcewake_ack
};

/*
 * The delayed-work hooks of the asynchronous power put, bound to the
 * recording stand-in for the length of the asynchronous-put cases.
 */
static const struct i915_pw_async_ops i915_ktest_async_ops = {
	i915_ktest_async_queue,
	i915_ktest_async_cancel
};

/*
 * Runs the tests of the display's data sources, power wells, clocks and
 * firmware.
 *
 * The part builds a display of its own for the objects under test and
 * frees it at the end; the live display is not touched.
 */
void
drv_i915_ktest_display(
	struct i915_ktest *ktest)
{
	struct i915_ktest_display_fixture *fx;

	/* Allocates the fixture: the test's display alone is far larger than a kernel stack. */
	fx = kern_calloc(1, sizeof(*fx));
	if (fx == NULL) {
		drv_i915_ktest_skip(ktest, "display: every display check", "no memory for the test's display");
		return;
	}

	/* Prepares the PCODE lock and routes the test display's legacy VGA ports to the recorder. */
	(void)mutex_init(&fx->sb_lock, LOCK_RANK_DEVICE, "ktest-display-sb");
	fx->vga_ops.get = i915_ktest_vga_get;
	fx->vga_ops.in8 = i915_ktest_vga_in8;
	fx->vga_ops.out8 = i915_ktest_vga_out8;
	fx->vga_ops.put = i915_ktest_vga_put;
	fx->vga_ops.ctx = &fx->vga;
	drv_i915_vga_io_test_set(&fx->display, &fx->vga_ops);

	/* The DRM device management and the per-crtc vblank state. */
	i915_ktest_drm(ktest, fx);
	i915_ktest_drm_bounds(ktest, fx);

	/* The VBT checks, the parse and the missing-VBT defaults. */
	i915_ktest_bios(ktest, fx);
#ifdef I915_TEST_VBT
	i915_ktest_bios_blob(ktest);
#endif

	/*
	 * The eDP, eDP-with-threads and one-screen modeset tests are the
	 * display tests' own (tests/display) and do not run here.
	 */

	/* The VGA decode, the power-domain map and the PM demand state. */
	i915_ktest_vga(ktest, fx);
	i915_ktest_power_map(ktest, fx);

	/* The power-well operation bodies and the asynchronous put. */
	i915_ktest_power_wells(ktest, fx);

	/* The combo PHYs and the CDCLK. */
	i915_ktest_combo_phy(ktest, fx);
	i915_ktest_cdclk_init(ktest, fx);
	i915_ktest_cdclk_readout(ktest, fx);
	i915_ktest_cdclk_full_setup(ktest, fx);
	i915_ktest_cdclk_crawl(ktest, fx);
	i915_ktest_cdclk_notify_fail(ktest, fx);

	/* The display-core bring-up end to end. */
	i915_ktest_core_normal(ktest, fx);
	i915_ktest_core_remove(ktest, fx);
	i915_ktest_core_preserve(ktest, fx);

	/* The DMC firmware: provider, parse, load and the asynchronous loader. */
	i915_ktest_dmc_provider(ktest);
	i915_ktest_dmc_parse(ktest, fx);
	i915_ktest_dmc_load(ktest, fx);
	i915_ktest_dmc_async(ktest, fx);
	i915_ktest_dmc_bad_fw(ktest, fx);

	/* The display software state. */
	i915_ktest_state_bw_fixture(&fx->bw);
	i915_ktest_state_mode(ktest, fx);
	i915_ktest_state_index(ktest, fx);
	i915_ktest_state_objects(ktest, fx);
	i915_ktest_state_sagv_skip(ktest, fx);
	i915_ktest_state_sagv_fail(ktest, fx);
	i915_ktest_state_color(ktest, fx);
	i915_ktest_state_quirks(ktest, fx);
	i915_ktest_state_fbc(ktest, fx);
	i915_ktest_state_fini(ktest, fx);

	/* Frees the fixture; nothing of it is bound anywhere. */
	drv_i915_vga_io_test_set(&fx->display, NULL);
	kern_free(fx);
}

/* Reads one register of the model. */
static uint32_t
i915_ktest_fake_read32(
	void *context,
	uint32_t offset)
{
	struct i915_ktest_fake_mmio *fake;
	uint32_t value;

	/* The context is the fixture's register model. */
	fake = context;

	/* The fuse status as the case set it. */
	if (offset == I915_KTEST_FUSE_STATUS)
		return fake->fuse_status;

	/* A mailbox read counts down a delayed PCODE answer. */
	if (offset == I915_KTEST_PCODE_MAILBOX) {
		i915_ktest_fake_pcode_countdown(fake);
		return fake->mailbox;
	}

	/* The PCODE data registers. */
	if (offset == I915_KTEST_PCODE_DATA)
		return fake->data;
	if (offset == I915_KTEST_PCODE_DATA1)
		return fake->data1;

	/* The driver's well control shows its requests and the state of every requested well. */
	if (offset == I915_KTEST_PW_CTL2) {
		value = i915_ktest_fake_well_control(fake, fake->pw_hsw_req, fake->pw_hsw_bios);
		return value;
	}
	if (offset == I915_KTEST_AUX_CTL2) {
		value = i915_ktest_fake_well_control(fake, fake->pw_aux_req, fake->pw_aux_bios);
		return value;
	}
	if (offset == I915_KTEST_DDI_CTL2) {
		value = i915_ktest_fake_well_control(fake, fake->pw_ddi_req, fake->pw_ddi_bios);
		return value;
	}

	/* The BIOS's well control shows the BIOS requests. */
	if (offset == I915_KTEST_PW_CTL1)
		return fake->pw_hsw_bios;
	if (offset == I915_KTEST_AUX_CTL1)
		return fake->pw_aux_bios;
	if (offset == I915_KTEST_DDI_CTL1)
		return fake->pw_ddi_bios;

	/* Every other register reads what was stored, or 0. */
	value = i915_ktest_fake_get(fake, offset);

	/* Succeeded: reports the stored value. */
	return value;
}

/* Writes one register of the model. */
static void
i915_ktest_fake_write32(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_ktest_fake_mmio *fake;
	uint32_t stored;

	/* The context is the fixture's register model. */
	fake = context;

	/* Records the write in order. */
	fake->wt_total++;
	i915_ktest_fake_record(fake, offset, value);

	/* The PCODE data registers, and the mailbox write that starts a transaction. */
	if (offset == I915_KTEST_PCODE_DATA) {
		fake->data = value;
		return;
	}
	if (offset == I915_KTEST_PCODE_DATA1) {
		fake->data1 = value;
		return;
	}
	if (offset == I915_KTEST_PCODE_MAILBOX) {
		i915_ktest_fake_pcode_start(fake, value);
		return;
	}

	/* The well controls keep the request bits only. */
	if (offset == I915_KTEST_PW_CTL2) {
		fake->pw_hsw_req = value & I915_KTEST_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_KTEST_AUX_CTL2) {
		fake->pw_aux_req = value & I915_KTEST_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_KTEST_DDI_CTL2) {
		fake->pw_ddi_req = value & I915_KTEST_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_KTEST_PW_CTL1) {
		fake->pw_hsw_bios = value & I915_KTEST_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_KTEST_AUX_CTL1) {
		fake->pw_aux_bios = value & I915_KTEST_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_KTEST_DDI_CTL1) {
		fake->pw_ddi_bios = value & I915_KTEST_PW_REQUEST_BITS;
		return;
	}

	/* A DBUF slice's power state follows its power request. */
	if (offset == I915_KTEST_DBUF_CTL_S1 ||
	    offset == I915_KTEST_DBUF_CTL_S2 ||
	    offset == I915_KTEST_DBUF_CTL_S3 ||
	    offset == I915_KTEST_DBUF_CTL_S4) {
		stored = i915_ktest_follow_bit(value, I915_KTEST_DBUF_POWER_REQUEST, I915_KTEST_DBUF_POWER_STATE);
		i915_ktest_fake_set(fake, offset, stored);
		return;
	}

	/* The DE PLL locks when enabled and acknowledges a frequency request. */
	if (offset == I915_KTEST_DE_PLL_ENABLE) {
		stored = i915_ktest_follow_bit(value, I915_KTEST_DE_PLL_PLL_ENABLE, I915_KTEST_DE_PLL_LOCK);
		stored = i915_ktest_follow_bit(stored, I915_KTEST_DE_PLL_FREQ_REQ, I915_KTEST_DE_PLL_FREQ_ACK);
		i915_ktest_fake_set(fake, offset, stored);
		return;
	}

	/* A combo PHY group write also reaches lane 0 (kept as a distinct register). */
	if (offset == I915_KTEST_PHY_A_GRP_TX_DW8) {
		i915_ktest_fake_set(fake, I915_KTEST_PHY_A_LN0_TX_DW8, value);
	} else if (offset == I915_KTEST_PHY_A_GRP_PCS_DW1) {
		i915_ktest_fake_set(fake, I915_KTEST_PHY_A_LN0_PCS_DW1, value);
	} else if (offset == I915_KTEST_PHY_B_GRP_TX_DW8) {
		i915_ktest_fake_set(fake, I915_KTEST_PHY_B_LN0_TX_DW8, value);
	} else if (offset == I915_KTEST_PHY_B_GRP_PCS_DW1) {
		i915_ktest_fake_set(fake, I915_KTEST_PHY_B_LN0_PCS_DW1, value);
	}

	/* Stores the value. */
	i915_ktest_fake_set(fake, offset, value);
}

/* Accepts a forcewake request; the model has no sleeping domain. */
static void
i915_ktest_fake_forcewake_request(
	void *context,
	int domain,
	int wake)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);
	UNUSED_PARAMETER(wake);
}

/* Acknowledges every forcewake request at once. */
static int
i915_ktest_fake_forcewake_ack(
	void *context,
	int domain)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);

	/* Succeeded: the domain is awake. */
	return 1;
}

/* Composes a well control register: the requests plus the state of every well either side requests. */
static uint32_t
i915_ktest_fake_well_control(
	const struct i915_ktest_fake_mmio *fake,
	uint32_t request,
	uint32_t bios)
{
	uint32_t state;

	/* A requested well is on unless the case withholds the acknowledge. */
	state = 0U;
	if (!fake->pw_no_ack)
		state = ((request | bios) & I915_KTEST_PW_REQUEST_BITS) >> 1;

	/* Succeeded: reports the requests and the states. */
	return request | state;
}

/* Counts down a delayed scripted PCODE answer and shows it when the count ends. */
static void
i915_ktest_fake_pcode_countdown(
	struct i915_ktest_fake_mmio *fake)
{
	/* Only a scripted answer that is still held back counts down. */
	if (fake->ptxn == NULL)
		return;
	if (fake->ptxn_rc == 0U)
		return;

	/* Shows the answer once the last held-back read passed. */
	fake->ptxn_rc--;
	if (fake->ptxn_rc == 0U) {
		fake->data = fake->ptxn_pd;
		fake->data1 = fake->ptxn_pd1;
		fake->mailbox = (uint32_t)fake->ptxn_pstatus;
	}
}

/* Starts a PCODE transaction on a mailbox write. */
static void
i915_ktest_fake_pcode_start(
	struct i915_ktest_fake_mmio *fake,
	uint32_t value)
{
	/* A scripted transaction checks its command and data. */
	if (fake->ptxn != NULL) {
		i915_ktest_fake_pcode_scripted(fake, value);
		return;
	}

	/* A sticky status ends every transaction with that status. */
	if (fake->pcode_sticky_status != 0) {
		fake->mailbox = (uint32_t)fake->pcode_sticky_status & 0xffU;
		return;
	}

	/* An unscripted transaction completes at once with status 0. */
	fake->pcode_txn_count++;
	fake->mailbox = 0U;
}

/* Runs the next scripted PCODE transaction. */
static void
i915_ktest_fake_pcode_scripted(
	struct i915_ktest_fake_mmio *fake,
	uint32_t value)
{
	const struct i915_ktest_pcode_txn *txn;
	uint32_t command;

	/* A transaction past the script is unexpected. */
	if (fake->ptxn_i >= fake->ptxn_len) {
		fake->ptxn_bad = 1;
		fake->mailbox = 0U;
		return;
	}

	/* The command and the data must be the ones scripted. */
	txn = &fake->ptxn[fake->ptxn_i];
	command = value & ~I915_KTEST_PCODE_READY;
	if (txn->expected_mailbox != I915_KTEST_PCODE_ANY && command != txn->expected_mailbox)
		fake->ptxn_bad = 1;
	if (txn->expected_data != I915_KTEST_PCODE_ANY && fake->data != txn->expected_data)
		fake->ptxn_bad = 1;

	/* Takes the scripted answer and moves to the next transaction. */
	fake->ptxn_pd = txn->reply_data;
	fake->ptxn_pd1 = txn->reply_data1;
	fake->ptxn_pstatus = txn->status;
	fake->ptxn_rc = txn->ready_delay;
	fake->ptxn_i++;

	/* Answers now, or keeps READY set until the countdown ends. */
	if (fake->ptxn_rc == 0U) {
		fake->data = fake->ptxn_pd;
		fake->data1 = fake->ptxn_pd1;
		fake->mailbox = (uint32_t)fake->ptxn_pstatus;
	} else {
		fake->mailbox = I915_KTEST_PCODE_READY;
	}
}

/* Makes a status bit follow its request bit. */
static uint32_t
i915_ktest_follow_bit(
	uint32_t value,
	unsigned request_bit,
	unsigned status_bit)
{
	/* The status is set exactly when the request is. */
	if ((value & (1U << request_bit)) != 0U) {
		value |= 1U << status_bit;
	} else {
		value &= ~(1U << status_bit);
	}

	/* Succeeded: reports the value with the status in step. */
	return value;
}

/* Records one write in the ordered list while there is room. */
static void
i915_ktest_fake_record(
	struct i915_ktest_fake_mmio *fake,
	uint32_t offset,
	uint32_t value)
{
	/* The list keeps the first writes only. */
	if (fake->wt_n >= I915_KTEST_REG_SLOTS)
		return;

	fake->wt_off[fake->wt_n] = offset;
	fake->wt_val[fake->wt_n] = value;
	fake->wt_n++;
}

/* Finds the position of the first recorded write to a register whose masked value matches; -1 for none. */
static int
i915_ktest_fake_find(
	const struct i915_ktest_fake_mmio *fake,
	uint32_t offset,
	uint32_t value,
	uint32_t mask)
{
	unsigned i;

	/* Looks through the writes in order. */
	for (i = 0U; i < fake->wt_n; i++) {
		if (fake->wt_off[i] != offset)
			continue;
		if ((fake->wt_val[i] & mask) != (value & mask))
			continue;

		return (int)i;
	}

	/* No such write was made. */
	return -1;
}

/* Reads a stored register; a register never written reads 0. */
static uint32_t
i915_ktest_fake_get(
	const struct i915_ktest_fake_mmio *fake,
	uint32_t offset)
{
	unsigned i;

	/* Looks the register up. */
	for (i = 0U; i < fake->gen_n; i++) {
		if (fake->gen_off[i] == offset)
			return fake->gen_val[i];
	}

	/* A register never written reads 0. */
	return 0U;
}

/* Stores a register, while there is room for a new one. */
static void
i915_ktest_fake_set(
	struct i915_ktest_fake_mmio *fake,
	uint32_t offset,
	uint32_t value)
{
	unsigned i;

	/* Replaces the value of a register already stored. */
	for (i = 0U; i < fake->gen_n; i++) {
		if (fake->gen_off[i] == offset) {
			fake->gen_val[i] = value;
			return;
		}
	}

	/* A full store drops the register. */
	if (fake->gen_n >= I915_KTEST_REG_SLOTS)
		return;

	/* Adds the register. */
	fake->gen_off[fake->gen_n] = offset;
	fake->gen_val[fake->gen_n] = value;
	fake->gen_n++;
}

/* Clears the register model and opens the register access onto it. */
static void
i915_ktest_mmio_open(
	struct i915_ktest_display_fixture *fx)
{
	/* Every register starts at 0, with nothing scripted. */
	kern_memset(&fx->fake, 0, sizeof(fx->fake));

	/* No forcewake ranges: every register of the model is always on. */
	drv_i915_mmio_init(&fx->mmio, &i915_ktest_fake_mmio_ops, &fx->fake, NULL, 0U, NULL);
}

/* Takes the legacy VGA resource for the recorder. */
static int
i915_ktest_vga_get(
	void *context,
	int resource)
{
	struct i915_ktest_vga_recorder *recorder;

	UNUSED_PARAMETER(resource);

	/* Records the get; the resource is always granted. */
	recorder = context;
	i915_ktest_vga_record(recorder, 1);
	recorder->got = 1;

	/* Succeeded: the resource is owned. */
	return 1;
}

/* Reads a legacy VGA port for the recorder: MIS_R reads 0xAB, every other port 0. */
static unsigned char
i915_ktest_vga_in8(
	void *context,
	unsigned short port)
{
	struct i915_ktest_vga_recorder *recorder;

	/* Records the read and the value it returns. */
	recorder = context;
	i915_ktest_vga_record(recorder, 2);
	recorder->last_read = 0U;
	if (port == 0x3CCU)
		recorder->last_read = 0xABU;

	/* Succeeded: reports the port value. */
	return recorder->last_read;
}

/* Writes a legacy VGA port for the recorder. */
static void
i915_ktest_vga_out8(
	void *context,
	unsigned short port,
	unsigned char value)
{
	struct i915_ktest_vga_recorder *recorder;

	UNUSED_PARAMETER(port);

	/* Records the write and its value. */
	recorder = context;
	i915_ktest_vga_record(recorder, 3);
	recorder->last_written = value;
}

/* Gives the legacy VGA resource back for the recorder. */
static void
i915_ktest_vga_put(
	void *context,
	int resource)
{
	struct i915_ktest_vga_recorder *recorder;

	UNUSED_PARAMETER(resource);

	/* Records the put. */
	recorder = context;
	i915_ktest_vga_record(recorder, 4);
	recorder->put = 1;
}

/* Appends one call to the recorder's sequence while there is room. */
static void
i915_ktest_vga_record(
	struct i915_ktest_vga_recorder *recorder,
	int call)
{
	/* The sequence keeps the first eight calls. */
	if (recorder->count >= 8U)
		return;

	recorder->sequence[recorder->count] = call;
	recorder->count++;
}

/* Queues the delayed work in the stand-in: 1 when newly queued, 0 when already queued. */
static int
i915_ktest_async_queue(
	void *context,
	int delay_ms)
{
	struct i915_ktest_async_recorder *recorder;

	/* Counts the request. */
	recorder = context;
	recorder->queue_calls++;

	/* An already queued work keeps its delay. */
	if (recorder->queued)
		return 0;

	/* Queues the work with the delay. */
	recorder->queued = 1;
	recorder->last_delay = delay_ms;

	/* Succeeded: the work is newly queued. */
	return 1;
}

/* Cancels the delayed work in the stand-in; reports whether it was queued. */
static int
i915_ktest_async_cancel(
	void *context,
	int sync)
{
	struct i915_ktest_async_recorder *recorder;
	int was_queued;

	/* Counts the request and whether it waits for a running work. */
	recorder = context;
	recorder->cancel_calls++;
	if (sync)
		recorder->cancel_sync_calls++;

	/* Takes the work off the queue. */
	was_queued = recorder->queued;
	recorder->queued = 0;

	/* Succeeded: reports whether a queued work was cancelled. */
	return was_queued;
}

/* Counts one run of a managed DRM action. */
static void
i915_ktest_drm_action(
	void *arg)
{
	unsigned *ran;

	/* The argument is the fixture's counter. */
	ran = arg;
	(*ran)++;
}

/* Serves the corrupted DMC copy under the DMC name; every other name is absent. */
static int
i915_ktest_dmc_bad_request(
	void *context,
	struct i915_firmware *firmware,
	const char *name)
{
	struct i915_ktest_display_fixture *fx;
	int compared;

	/* The context is the fixture that holds the corrupted copy. */
	fx = context;

	/* Any other image is absent. */
	compared = kern_strcmp(name, I915_KTEST_DMC_NAME);
	if (compared != 0) {
		firmware->data = NULL;
		firmware->size = 0U;
		return ENOENT;
	}

	/* Hands back the corrupted copy. */
	firmware->data = fx->dmc_badcopy;
	firmware->size = I915_KTEST_DMC_SIZE;

	/* Succeeded: the caller holds the corrupted copy. */
	return 0;
}

/* Serves the test's own image under every name. */
static int
i915_ktest_firmware_image_request(
	void *context,
	struct i915_firmware *firmware,
	const char *name)
{
	UNUSED_PARAMETER(name);

	/* Hands back the image the context points at; the test keeps owning it. */
	firmware->data = context;
	firmware->size = I915_KTEST_FIRMWARE_TEST_IMAGE;

	/* Succeeded: the caller holds the test's image. */
	return 0;
}

/* Clears the test display's power-well context and points it at the register model and the VGA client. */
static void
i915_ktest_pwc_reset(
	struct i915_ktest_display_fixture *fx)
{
	struct i915_pw_ctx *pwc;

	/*
	 * The VGA client is the test display's, so a well with VGA on it
	 * resets the legacy VGA ports through the recorder.
	 */
	pwc = &fx->display.pwc;
	kern_memset(pwc, 0, sizeof(*pwc));
	pwc->mmio = &fx->mmio;
	pwc->vga = &fx->display.vga_client;
}

/* Clears the test display's CDCLK, selects the ADL-P D0 hooks and points it at the model. */
static void
i915_ktest_cdclk_reset(
	struct i915_ktest_display_fixture *fx)
{
	struct i915_cdclk_dev *cd;

	/* ADL-P, display stepping D0. */
	cd = &fx->display.cdclk;
	kern_memset(cd, 0, sizeof(*cd));
	drv_i915_init_cdclk_hooks(cd, 13, I915_STEP_D0, 1);

	/* The register model and the PCODE lock. */
	cd->m = &fx->mmio;
	cd->sb_lock = &fx->sb_lock;
}

/* Sums the reference counts of every power well. */
static unsigned
i915_ktest_well_refcounts(
	const struct i915_power_domains *pd)
{
	unsigned sum;
	unsigned i;

	/* Adds each well's count. */
	sum = 0U;
	for (i = 0U; i < pd->num_power_wells; i++)
		sum += pd->power_wells[i].refcount;

	/* Succeeded: reports the sum. */
	return sum;
}

/*
 * Builds a minimal VBT in the fixture: the 48-byte VBT header, the BDB
 * header and two blocks, 81 bytes in all; reports its size.
 */
static unsigned
i915_ktest_make_vbt(
	struct i915_ktest_display_fixture *fx,
	int good_signature)
{
	uint8_t *vbt;
	uint8_t *bdb;

	/* The signature: "$VBT", or a broken one. */
	vbt = fx->vbt_buffer;
	kern_memset(vbt, 0, I915_KTEST_VBT_BUFFER);
	vbt[0] = (uint8_t)'X';
	if (good_signature)
		vbt[0] = (uint8_t)'$';
	vbt[1] = (uint8_t)'V';
	vbt[2] = (uint8_t)'B';
	vbt[3] = (uint8_t)'T';

	/* The version, the header size, the VBT size and the BDB offset. */
	vbt[20] = 0x00U;
	vbt[21] = 0x01U;
	vbt[22] = 48U;
	vbt[24] = 81U;
	vbt[28] = 48U;

	/* The BDB header: signature, version 200, header size 22, BDB size 33. */
	bdb = vbt + 48U;
	bdb[0] = (uint8_t)'B';
	bdb[1] = (uint8_t)'D';
	bdb[2] = (uint8_t)'B';
	bdb[3] = (uint8_t)' ';
	bdb[16] = 200U;
	bdb[18] = 22U;
	bdb[20] = 33U;

	/* Block 1 of two bytes. */
	bdb[22] = 1U;
	bdb[23] = 2U;
	bdb[25] = 0xAAU;
	bdb[26] = 0xBBU;

	/* Block 2 of three bytes. */
	bdb[27] = 2U;
	bdb[28] = 3U;

	/* Succeeded: reports the VBT size. */
	return 81U;
}

/* Tests the DRM device init, the vblank init with its per-pipe workers, and the teardown. */
static void
i915_ktest_drm(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_drm_device *ddev;
	int error;

	/* Brings the test display's DRM device up. */
	ddev = &fx->display.drm_dev;
	kern_memset(ddev, 0, sizeof(*ddev));
	error = drv_i915_drm_dev_init(ddev, NULL, 0x3U);
	drv_i915_ktest_check(ktest, error == 0, "drm: dev_init");

	/* Four crtcs, each with its own vblank worker. */
	error = drv_i915_drm_vblank_init(ddev, 4U);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    ddev->num_crtcs == 4U &&
		    ddev->vblank_inited == 1,
		"drm: vblank_init 4 CRTCs");

	/* Every crtc has its state and its worker. */
	drv_i915_ktest_check(
		ktest,
		ddev->vblank[0].inited == 1 &&
		    ddev->vblank[0].worker_created == 1 &&
		    ddev->vblank[3].pipe == 3U &&
		    ddev->vblank[3].worker_created == 1,
		"drm: per-CRTC state + per-pipe worker");

	/* The teardown runs the managed cleanups: every worker goes. */
	drv_i915_drm_dev_fini(ddev);
	drv_i915_ktest_check(
		ktest,
		ddev->vblank_inited == 0 &&
		    ddev->inited == 0 &&
		    ddev->vblank[0].worker_created == 0 &&
		    ddev->vblank[3].worker_created == 0,
		"drm: dev_fini tears down every per-pipe worker");
}

/* Tests the refusals of the vblank init and the full managed-action list. */
static void
i915_ktest_drm_bounds(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_drm_device *ddev;
	unsigned k;
	int error;

	/* A crtc count of 0 is refused. */
	ddev = &fx->display.drm_dev;
	kern_memset(ddev, 0, sizeof(*ddev));
	(void)drv_i915_drm_dev_init(ddev, NULL, 0U);
	error = drv_i915_drm_vblank_init(ddev, 0U);
	drv_i915_ktest_check(ktest, error == EINVAL, "drm: reject 0 CRTCs");

	/* A count over the pipe array is refused without partial state. */
	error = drv_i915_drm_vblank_init(ddev, I915_DRM_MAX_PIPES + 1U);
	drv_i915_ktest_check(
		ktest,
		error == EINVAL && ddev->vblank_inited == 0,
		"drm: reject a CRTC count over the pipe array");
	drv_i915_drm_dev_fini(ddev);

	/* Fills the managed-action list. */
	kern_memset(ddev, 0, sizeof(*ddev));
	(void)drv_i915_drm_dev_init(ddev, NULL, 0U);
	for (k = 0U; k < I915_DRMM_MAX; k++)
		(void)drv_i915_drmm_add_action_or_reset(ddev, i915_ktest_drm_action, &fx->drm_actions_ran);

	/* One more action on the full list runs at once (drm_add_action_or_reset). */
	fx->drm_actions_ran = 0U;
	error = drv_i915_drmm_add_action_or_reset(ddev, i915_ktest_drm_action, &fx->drm_actions_ran);
	drv_i915_ktest_check(
		ktest,
		error == ENOMEM && fx->drm_actions_ran == 1U,
		"drm: add_action_or_reset runs the action on a full list");
	drv_i915_drm_dev_fini(ddev);

	/* A crtc cleanup that cannot be registered unwinds the vblank init. */
	kern_memset(ddev, 0, sizeof(*ddev));
	(void)drv_i915_drm_dev_init(ddev, NULL, 0U);
	for (k = 0U; k < I915_DRMM_MAX; k++)
		(void)drv_i915_drmm_add_action_or_reset(ddev, i915_ktest_drm_action, &fx->drm_actions_ran);
	error = drv_i915_drm_vblank_init(ddev, 4U);
	drv_i915_ktest_check(
		ktest,
		error == ENOMEM && ddev->vblank_inited == 0,
		"drm: vblank_init unwinds when a CRTC cleanup cannot be registered");
	drv_i915_drm_dev_fini(ddev);

	/*
	 * The per-pipe worker-create failure and its teardown are not tested:
	 * the vblank init has no way left to make a worker creation fail.
	 */
}

/* Tests the VBT header check, the block walk, the missing-VBT defaults and the SHA-256. */
static void
i915_ktest_bios(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_vbt_state *vbt;
	const uint8_t *sha;
	unsigned length;
	int valid;
	int error;

	/* A well-formed VBT is accepted. */
	length = i915_ktest_make_vbt(fx, 1);
	valid = drv_i915_bios_is_valid_vbt_header(fx->vbt_buffer, length);
	drv_i915_ktest_check(ktest, valid == 1, "bios: is_valid_vbt accepts a good VBT");

	/* No buffer holds no VBT. */
	valid = drv_i915_bios_is_valid_vbt_header(NULL, length);
	drv_i915_ktest_check(ktest, valid == 0, "bios: is_valid_vbt rejects NULL");

	/* A buffer shorter than the header holds none either. */
	valid = drv_i915_bios_is_valid_vbt_header(fx->vbt_buffer, 10U);
	drv_i915_ktest_check(ktest, valid == 0, "bios: is_valid_vbt rejects a short buffer");

	/* A broken signature is refused. */
	(void)i915_ktest_make_vbt(fx, 0);
	valid = drv_i915_bios_is_valid_vbt_header(fx->vbt_buffer, length);
	drv_i915_ktest_check(ktest, valid == 0, "bios: is_valid_vbt rejects a bad signature");

	/* The block walk reads the BDB version and counts both blocks. */
	length = i915_ktest_make_vbt(fx, 1);
	vbt = &fx->vbt;
	kern_memset(vbt, 0, sizeof(*vbt));
	error = drv_i915_bios_process_vbt(vbt, fx->vbt_buffer, length);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    vbt->version == 200U &&
		    vbt->num_bdb_blocks == 2U &&
		    vbt->vbt_found == 1,
		"bios: process_vbt parses BDB header + walks blocks");

	/* A missing VBT stands for the three non-Type-C DDI ports. */
	kern_memset(vbt, 0, sizeof(*vbt));
	drv_i915_bios_init_vbt_missing_defaults(vbt);
	drv_i915_ktest_check(
		ktest,
		vbt->num_display_devices == 3U && vbt->version == 155U,
		"bios: missing defaults generate 3 non-TC DDI children");

	/* Port A is the internal connector and carries no TMDS. */
	drv_i915_ktest_check(
		ktest,
		vbt->display_devices[0].port == 0U &&
		    (vbt->display_devices[0].device_type & (1U << 12)) != 0U &&
		    (vbt->display_devices[0].device_type & (1U << 4)) == 0U,
		"bios: PORT_A child is internal-connector and not TMDS");

	/*
	 * The acquisition itself (intel_bios_init()) runs the parser in its
	 * world, and the parser's world is bound driver-wide to the live display
	 * (one parse at a time); a second acquisition would be refused or would
	 * share the live display's arena.
	 */
	drv_i915_ktest_skip(ktest, "bios: intel_bios_init falls back to defaults when VBT is absent",
	    "the VBT parser world is a driver-wide binding held by the live display");

	/* The SHA-256 the blob is pinned with matches the FIPS 180-4 "abc" vector. */
	sha = fx->sha;
	drv_i915_sha256("abc", 3U, fx->sha);
	drv_i915_ktest_check(
		ktest,
		sha[0] == 0xbaU &&
		    sha[1] == 0x78U &&
		    sha[2] == 0x16U &&
		    sha[3] == 0xbfU &&
		    sha[28] == 0xf2U &&
		    sha[29] == 0x00U &&
		    sha[30] == 0x15U &&
		    sha[31] == 0xadU,
		"bios: VBT-SHA the SHA-256 used to pin the blob matches the FIPS 180-4 'abc' vector");
}

#ifdef I915_TEST_VBT
/*
 * Records the explicit-blob parse checks that cannot run, and tests the
 * blob's validation.
 *
 * XXX: the explicit VBT is a test crutch for the QEMU passthrough guest
 * without an OpRegion; delete this with it once the GPU tests run on bare
 * metal.
 */
static void
i915_ktest_bios_blob(
	struct i915_ktest *ktest)
{
	static const char *const acquisitions[] = {
		"bios: VBT-EXPLICIT not requested -> the blob is not touched; missing defaults as before",
		"bios: VBT-EXPLICIT another machine (subsystem 1028:0b03) -> no row, nothing read; defaults",
		"bios: VBT-EXPLICIT on the target: 8704 bytes, SHA-256 pinned, BDB 249, source = explicit blob (not OpRegion), no parser error",
		"bios: VBT-CHILDREN from the real VBT only: A=eDP/AUX A, B=HDMI (DDC pin 2), TC1+TC2=DP Type-C/TBT, no port C (not the A/B/C defaults)",
		"bios: VBT-PANEL type 2, 18 bpp, PPS T3 200ms/T7 80ms/T9 200ms/T10 110ms/T12 500ms, PWM backlight 200 Hz active-high controller 0, min 15 (BDB>=234 field)",
		"bios: VBT-DEFAULTS the reference init_vbt_missing_defaults() yields the same A/B/C children as before (version 155)"
	};
	unsigned i;
	int full;
	int truncated;
	int header_only;

	/* Each of these runs the acquisition or the panel parse in the parser's driver-wide world. */
	for (i = 0U; i < sizeof(acquisitions) / sizeof(acquisitions[0]); i++) {
		drv_i915_ktest_skip(ktest, acquisitions[i],
		    "the VBT parser world is a driver-wide binding held by the live display");
	}

	/* The full blob validates; truncated copies do not. */
	full = drv_i915_vbt_validate(i915_ktest_vbt_dell_latitude_5330, sizeof(i915_ktest_vbt_dell_latitude_5330));
	truncated = drv_i915_vbt_validate(i915_ktest_vbt_dell_latitude_5330, 4000U);
	header_only = drv_i915_vbt_validate(i915_ktest_vbt_dell_latitude_5330, 40U);
	drv_i915_ktest_check(
		ktest,
		full == 1 &&
		    truncated == 0 &&
		    header_only == 0,
		"bios: VBT-VALIDATE the full blob validates; truncated copies do not");
}
#endif

/* Tests the VGA decode callback and the decode state without a host bridge. */
static void
i915_ktest_vga(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_vga_client *vga;
	unsigned decode;
	int error;

	/* A client with no display device and no host bridge. */
	vga = &fx->display.vga_client;
	vga->gpu = NULL;
	vga->gmch = NULL;
	vga->display_ver = 13U;
	vga->registered = 0;

	/* A decoding device claims the legacy and the normal resources. */
	decode = drv_i915_gmch_vga_set_decode(vga, 1);
	drv_i915_ktest_check(
		ktest,
		decode == (I915_VGA_RSRC_LEGACY_IO |
		    I915_VGA_RSRC_LEGACY_MEM |
		    I915_VGA_RSRC_NORMAL_IO |
		    I915_VGA_RSRC_NORMAL_MEM),
		"vga: decode enable returns legacy+normal IO/MEM");

	/* A device that does not decode claims the normal resources only. */
	decode = drv_i915_gmch_vga_set_decode(vga, 0);
	drv_i915_ktest_check(
		ktest,
		decode == (I915_VGA_RSRC_NORMAL_IO | I915_VGA_RSRC_NORMAL_MEM),
		"vga: decode disable returns normal IO/MEM only");

	/* Without a host bridge the control word cannot be reached. */
	error = drv_i915_gmch_vga_set_state(vga, 1);
	drv_i915_ktest_check(ktest, error == ENODEV, "vga: set_state without a GMCH bridge is -ENODEV");
}

/* Tests the ADL-P power-domain map, its well descriptors and the PM demand early init. */
static void
i915_ktest_power_map(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_power_domains *pd;
	struct i915_pmdemand *pm;
	uint64_t wells;
	int pw1;
	int pw2;
	int error;

	/* Builds the xelpd map: 30 wells and the DC masks. */
	pd = &fx->display.power_domains;
	drv_i915_trace_init(&fx->display.dc_trace);
	error = drv_i915_power_domains_init(pd, 13U, -1, -1, &fx->display.dc_trace);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    pd->num_power_wells == 30U &&
		    pd->allowed_dc_mask == 0x4000000aU &&
		    pd->target_dc_state == 0x00000002U,
		"power: domains_init builds xelpd map (30 wells) + DC mask 0x4000000a/0x2");

	/* PIPE_A needs the always-on well and PW_A. */
	wells = drv_i915_power_domain_wells(pd, I915_PW_DOMAIN_PIPE_A);
	drv_i915_ktest_check(
		ktest,
		(wells & ((uint64_t)1U << 0)) != 0U && (wells & ((uint64_t)1U << 4)) != 0U,
		"power: PIPE_A -> always_on + PW_A");

	/* DC_OFF needs the always-on well and the DC_off well. */
	wells = drv_i915_power_domain_wells(pd, I915_PW_DOMAIN_DC_OFF);
	drv_i915_ktest_check(
		ktest,
		(wells & ((uint64_t)1U << 0)) != 0U && (wells & ((uint64_t)1U << 2)) != 0U,
		"power: DC_OFF -> always_on + DC_off well");

	/* PW_2 carries VGA and is not always on; well 0 is. */
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[3].has_vga == 1 &&
		    pd->power_wells[3].always_on == 0 &&
		    pd->power_wells[0].always_on == 1,
		"power: PW_2 has_vga (not always_on); always_on well flagged");

	/* The live state starts apart from the descriptor: no reference, state unknown. */
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[4].refcount == 0U && pd->power_wells[4].hw_enabled == -1,
		"power: live state (refcount/hw_enabled) separate from descriptor");

	/* The Thunderbolt AUX wells are present with their control index. */
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[29].is_tc_tbt == 1 &&
		    pd->power_wells[29].hsw_idx == 12U &&
		    pd->power_wells[26].is_tc_tbt == 1,
		"power: AUX_TBT4/TBT1 present with is_tc_tbt + hsw.idx=12");

	/* AUX_USBC1 has the fixed enable delay and the 500 ms workaround timeout. */
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[22].fixed_enable_delay == 1 &&
		    pd->power_wells[22].enable_timeout == 500U &&
		    pd->power_wells[22].hsw_idx == 3U,
		"power: AUX_USBC1 fixed_enable_delay + enable_timeout=500 + idx=3");

	/* The wells are found by their reference ids, not by array position. */
	pw2 = drv_i915_power_well_by_id(pd, I915_SKL_DISP_PW_2);
	pw1 = drv_i915_power_well_by_id(pd, I915_SKL_DISP_PW_1);
	drv_i915_ktest_check(
		ktest,
		pw2 == 3 &&
		    pw1 == 1 &&
		    pd->power_wells[3].hsw_idx == 1U &&
		    pd->power_wells[4].hsw_idx == 5U,
		"power: PW_1/PW_2 found by id; PW_2 idx=1, PW_A idx=5");

	/* An empty domain list means every domain; PW_1's missing list means none. */
	wells = drv_i915_power_domain_wells(pd, I915_PW_DOMAIN_PORT_DDI_LANES_TC4);
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[0].domains_all == 1 &&
		    pd->power_wells[1].domains_all == 0 &&
		    wells != 0U,
		"power: always_on=all-domains, PW_1=NULL-list(none)");

	/* The cleanup releases the map. */
	drv_i915_power_domains_cleanup(pd);
	drv_i915_ktest_check(ktest, pd->initialized == 0, "power: cleanup releases the map");

	/* The PM demand early init prepares its lock and wait queue. */
	pm = &fx->display.pmdemand;
	pm->early_initialized = 0;
	drv_i915_pmdemand_init_early(pm);
	drv_i915_ktest_check(ktest, pm->early_initialized == 1, "pmdemand: init_early sets up lock + wait queue");
}

/* Tests the power-well get, put and enable bodies on well PW_A. */
static void
i915_ktest_power_wells(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_power_domains *pd;
	struct i915_power_well *well;
	struct i915_pw_ctx *pwc;
	int error;

	/* A fresh map on the register model. */
	pd = &fx->display.power_domains;
	pwc = &fx->display.pwc;
	drv_i915_trace_init(&fx->display.dc_trace);
	(void)drv_i915_power_domains_init(pd, 13U, -1, -1, &fx->display.dc_trace);
	i915_ktest_mmio_open(fx);
	i915_ktest_pwc_reset(fx);

	/* The first get of PW_A (well 4) requests it and waits for its state. */
	well = &pd->power_wells[4];
	error = drv_i915_power_well_get(well, pwc);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    well->refcount == 1U &&
		    well->hw_enabled == 1 &&
		    (fx->fake.pw_hsw_req & I915_KTEST_PW_A_REQUEST) != 0U,
		"pw: get enables the well (REQ set, ACK); refcount=1");

	/* A second get only counts the reference. */
	error = drv_i915_power_well_get(well, pwc);
	drv_i915_ktest_check(
		ktest,
		error == 0 && well->refcount == 2U,
		"pw: nested get bumps refcount without re-enabling");

	/* The first put keeps the well on. */
	drv_i915_power_well_put(well, pwc);
	drv_i915_ktest_check(
		ktest,
		well->refcount == 1U && (fx->fake.pw_hsw_req & I915_KTEST_PW_A_REQUEST) != 0U,
		"pw: first put keeps the well enabled (refcount 2->1)");

	/* The last put withdraws the request. */
	drv_i915_power_well_put(well, pwc);
	drv_i915_ktest_check(
		ktest,
		well->refcount == 0U &&
		    well->hw_enabled == 0 &&
		    (fx->fake.pw_hsw_req & I915_KTEST_PW_A_REQUEST) == 0U,
		"pw: last put disables the well (REQ cleared)");

	/* An explicit enable drives the hardware and leaves the count alone. */
	well->refcount = 0U;
	error = drv_i915_power_well_enable(well, pwc);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    well->refcount == 0U &&
		    well->hw_enabled == 1,
		"pw: explicit enable() drives HW without touching the refcount");

	/* The state readout, the BIOS takeover, the shared domains, the asynchronous put and the edge cases. */
	i915_ktest_power_well_state(ktest, fx);
	i915_ktest_power_domains(ktest, fx);
	i915_ktest_power_async(ktest, fx);
	i915_ktest_power_async_second(ktest, fx);
	i915_ktest_power_well_edges(ktest, fx);
}

/* Tests the well state readout and the takeover of a BIOS request. */
static void
i915_ktest_power_well_state(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_power_well *well;
	struct i915_pw_ctx *pwc;
	int enabled;

	/* The case works on PW_A (well 4) of the test display. */
	well = &fx->display.power_domains.power_wells[4];
	pwc = &fx->display.pwc;

	/* Neither request: off. */
	i915_ktest_mmio_open(fx);
	fx->fake.pw_hsw_req = 0U;
	fx->fake.pw_hsw_bios = 0U;
	enabled = drv_i915_power_well_is_enabled(well, pwc);
	drv_i915_ktest_check(ktest, enabled == 0, "pw: is_enabled 00 -> off");

	/* The BIOS request alone powers the well but the driver does not own it. */
	fx->fake.pw_hsw_bios = I915_KTEST_PW_A_REQUEST;
	enabled = drv_i915_power_well_is_enabled(well, pwc);
	drv_i915_ktest_check(ktest, enabled == 0, "pw: is_enabled STATE-only (BIOS) -> driver-off");

	/* The driver's request with the state set: on. */
	fx->fake.pw_hsw_bios = 0U;
	fx->fake.pw_hsw_req = I915_KTEST_PW_A_REQUEST;
	enabled = drv_i915_power_well_is_enabled(well, pwc);
	drv_i915_ktest_check(ktest, enabled == 1, "pw: is_enabled REQ+STATE -> on");

	/* The sync takes a BIOS-held request over: the driver requests first, then clears the BIOS bit. */
	i915_ktest_mmio_open(fx);
	fx->fake.pw_hsw_bios = I915_KTEST_PW_A_REQUEST;
	well->hw_enabled = -1;
	well->refcount = 0U;
	drv_i915_power_well_sync_hw(well, pwc);
	drv_i915_ktest_check(
		ktest,
		(fx->fake.pw_hsw_req & I915_KTEST_PW_A_REQUEST) != 0U &&
		    (fx->fake.pw_hsw_bios & I915_KTEST_PW_A_REQUEST) == 0U &&
		    well->hw_enabled == 1 &&
		    well->refcount == 0U,
		"pw: sync_hw takes over the BIOS request (driver REQ set, BIOS cleared, refcount unchanged)");

	/* Without a BIOS request the sync only records the state. */
	i915_ktest_mmio_open(fx);
	fx->fake.pw_hsw_req = I915_KTEST_PW_A_REQUEST;
	well->hw_enabled = -1;
	drv_i915_power_well_sync_hw(well, pwc);
	drv_i915_ktest_check(ktest, well->hw_enabled == 1, "pw: sync_hw records is_enabled when no BIOS handoff is needed");
}

/* Tests two domains that share PW_A. */
static void
i915_ktest_power_domains(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;
	unsigned k;
	int error;

	/* The objects under test are the test display's. */
	pd = &fx->display.power_domains;
	pwc = &fx->display.pwc;

	/* Every well starts unreferenced and unknown. */
	i915_ktest_mmio_open(fx);
	for (k = 0U; k < pd->num_power_wells; k++) {
		pd->power_wells[k].refcount = 0U;
		pd->power_wells[k].hw_enabled = -1;
	}

	/* PIPE_A takes the always-on well and PW_A in order. */
	error = drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	drv_i915_ktest_check(
		ktest,
		error == 0 && pd->power_wells[4].refcount == 1U,
		"pw: display_power_get(PIPE_A) takes always_on + PW_A in order");

	/* PANEL_FITTER_A needs PW_A too: its count reaches 2. */
	error = drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_PANEL_FITTER_A, pwc);
	drv_i915_ktest_check(
		ktest,
		error == 0 && pd->power_wells[4].refcount == 2U,
		"pw: a second domain sharing PW_A bumps its refcount to 2");

	/* Putting one sharer keeps PW_A. */
	drv_i915_display_power_put(pd, I915_PW_DOMAIN_PIPE_PANEL_FITTER_A, pwc);
	drv_i915_ktest_check(ktest, pd->power_wells[4].refcount == 1U, "pw: putting one sharer keeps PW_A enabled");

	/* Putting the last sharer lets PW_A go. */
	drv_i915_display_power_put(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	drv_i915_ktest_check(ktest, pd->power_wells[4].refcount == 0U, "pw: putting the last sharer disables PW_A");
}

/* Tests the asynchronous put: park, grab back and the delayed release. */
static void
i915_ktest_power_async(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;
	struct i915_ktest_async_recorder *recorder;
	int error;

	/* Binds the recording stand-in as the delayed work. */
	pd = &fx->display.power_domains;
	pwc = &fx->display.pwc;
	recorder = &fx->async;
	kern_memset(recorder, 0, sizeof(*recorder));
	drv_i915_display_power_async_bind(pd, &i915_ktest_async_ops, recorder, pwc);

	/* The last reference is parked, not dropped: the well stays on and the work is queued for 100 ms. */
	(void)drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	drv_i915_display_power_put_async(pd, I915_PW_DOMAIN_PIPE_A, pwc, -1);
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[4].refcount == 1U &&
		    (fx->fake.pw_hsw_req & I915_KTEST_PW_A_REQUEST) != 0U &&
		    pd->domain_use_count[I915_PW_DOMAIN_PIPE_A] == 1U &&
		    pd->async_put_wakeref == 1 &&
		    recorder->queued == 1 &&
		    recorder->last_delay == 100 &&
		    pd->async_parked == 1U,
		"pw-async: ASYNC-PARK the last reference is parked, not dropped: well still on, work queued for the default 100 ms");

	/* The next get takes the parked reference back without touching the hardware. */
	error = drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    pd->power_wells[4].refcount == 1U &&
		    pd->async_grabs == 1U &&
		    pd->async_put_wakeref == 0 &&
		    recorder->queued == 0 &&
		    pd->domain_use_count[I915_PW_DOMAIN_PIPE_A] == 1U,
		"pw-async: ASYNC-GRAB the next get takes the parked reference back: no hardware change, work cancelled");

	/* The timer fires: the work drops the parked reference and the well goes off. */
	drv_i915_display_power_put_async(pd, I915_PW_DOMAIN_PIPE_A, pwc, -1);
	recorder->queued = 0;
	drv_i915_display_power_async_work(pd);
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[4].refcount == 0U &&
		    (fx->fake.pw_hsw_req & I915_KTEST_PW_A_REQUEST) == 0U &&
		    pd->domain_use_count[I915_PW_DOMAIN_PIPE_A] == 0U &&
		    pd->async_put_wakeref == 0 &&
		    pd->async_released == 1U,
		"pw-async: ASYNC-RELEASE the delayed work drops the parked reference: well off");
}

/* Tests the second parked mask, the flush, a late work body and a put that is not the last. */
static void
i915_ktest_power_async_second(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;
	struct i915_ktest_async_recorder *recorder;

	/* The objects under test are the test display's. */
	pd = &fx->display.power_domains;
	pwc = &fx->display.pwc;
	recorder = &fx->async;

	/* A put while the work is outstanding waits in the second mask; the work is not queued again. */
	(void)drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	(void)drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_PANEL_FITTER_A, pwc);
	drv_i915_display_power_put_async(pd, I915_PW_DOMAIN_PIPE_A, pwc, -1);
	drv_i915_display_power_put_async(pd, I915_PW_DOMAIN_PIPE_PANEL_FITTER_A, pwc, 250);
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[4].refcount == 2U &&
		    pd->async_put_next_delay == 250 &&
		    recorder->queue_calls == 3U,
		"pw-async: ASYNC-SECOND a put while the work is outstanding waits in the second mask (no second queue)");

	/* The work releases the first mask and queues itself again for the second, with its delay. */
	recorder->queued = 0;
	drv_i915_display_power_async_work(pd);
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[4].refcount == 1U &&
		    pd->async_requeues == 1U &&
		    recorder->queued == 1 &&
		    recorder->last_delay == 250 &&
		    pd->async_put_wakeref == 1,
		"pw-async: ASYNC-REQUEUE the work releases the first mask and re-queues itself for the second, with its delay");

	/* The flush releases everything parked and waits for the work. */
	drv_i915_display_power_flush_work_sync(pd);
	drv_i915_ktest_check(
		ktest,
		pd->power_wells[4].refcount == 0U &&
		    (fx->fake.pw_hsw_req & I915_KTEST_PW_A_REQUEST) == 0U &&
		    pd->async_put_wakeref == 0 &&
		    recorder->cancel_sync_calls == 1U,
		"pw-async: ASYNC-FLUSH flush releases everything parked now and syncs the work");

	/* A work body that finds nothing does nothing; the whole sequence made no state error. */
	drv_i915_display_power_async_work(pd);
	drv_i915_ktest_check(
		ktest,
		pd->async_work_empty == 1U &&
		    pd->power_wells[4].refcount == 0U &&
		    pd->async_state_errors == 0U &&
		    pd->use_count_errors == 0U,
		"pw-async: ASYNC-EMPTY a late work body finds nothing; no state or use-count error in the whole sequence");

	/* With another holder the asynchronous put is an ordinary put. */
	(void)drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	(void)drv_i915_display_power_get(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	drv_i915_display_power_put_async(pd, I915_PW_DOMAIN_PIPE_A, pwc, -1);
	drv_i915_ktest_check(
		ktest,
		pd->domain_use_count[I915_PW_DOMAIN_PIPE_A] == 1U &&
		    pd->async_put_wakeref == 0 &&
		    pd->power_wells[4].refcount == 1U,
		"pw-async: ASYNC-NOT-LAST with another holder the async put is an ordinary put");

	/* Drops the last holder and unbinds the stand-in. */
	drv_i915_display_power_put(pd, I915_PW_DOMAIN_PIPE_A, pwc);
	drv_i915_display_power_async_bind(pd, NULL, NULL, NULL);
}

/* Tests a BIOS-held well at disable, the post-enable hooks and a missing acknowledge. */
static void
i915_ktest_power_well_edges(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_power_domains *pd;
	struct i915_pw_ctx *pwc;
	uint32_t control;
	int error;

	/* The objects under test are the test display's. */
	pd = &fx->display.power_domains;
	pwc = &fx->display.pwc;

	/* The BIOS holds PW_A: the driver's disable leaves its state on and does not hang. */
	i915_ktest_mmio_open(fx);
	fx->fake.pw_hsw_bios = I915_KTEST_PW_A_REQUEST;
	pd->power_wells[4].refcount = 0U;
	pd->power_wells[4].hw_enabled = -1;
	(void)drv_i915_power_well_get(&pd->power_wells[4], pwc);
	drv_i915_power_well_put(&pd->power_wells[4], pwc);
	control = drv_i915_raw_read32(&fx->mmio, I915_KTEST_PW_CTL2);
	drv_i915_ktest_check(
		ktest,
		(control & I915_KTEST_PW_A_STATE) != 0U && pd->power_wells[4].hw_enabled == 0,
		"pw: disable tolerates a BIOS-held well (STATE stays; driver ownership off)");

	/* PW_2 carries VGA: its enable resets the legacy VGA ports. */
	i915_ktest_mmio_open(fx);
	pwc->vga_reset_calls = 0U;
	pwc->irq_post_enable_calls = 0U;
	pwc->irqs_enabled = 0;
	pd->power_wells[3].refcount = 0U;
	pd->power_wells[3].hw_enabled = -1;
	(void)drv_i915_power_well_enable(&pd->power_wells[3], pwc);
	drv_i915_ktest_check(ktest, pwc->vga_reset_calls == 1U, "pw: PW_2 post-enable calls intel_vga_reset_io_mem (has_vga)");

	/* PW_A has no VGA, and its pipe-interrupt hook is gated off while interrupts are off. */
	pwc->vga_reset_calls = 0U;
	pd->power_wells[4].refcount = 0U;
	pd->power_wells[4].hw_enabled = -1;
	(void)drv_i915_power_well_enable(&pd->power_wells[4], pwc);
	drv_i915_ktest_check(
		ktest,
		pwc->vga_reset_calls == 0U && pwc->irq_post_enable_calls == 0U,
		"pw: PW_A post-enable: no VGA; pipe-IRQ post-enable gated off before P4");

	/* A missing acknowledge is warned and the enable goes on; the caller does not fail. */
	i915_ktest_mmio_open(fx);
	fx->fake.pw_no_ack = 1;
	pwc->ack_timeouts = 0U;
	pd->power_wells[4].refcount = 0U;
	pd->power_wells[4].hw_enabled = -1;
	error = drv_i915_power_well_enable(&pd->power_wells[4], pwc);
	drv_i915_ktest_check(
		ktest,
		error == 0 && pwc->ack_timeouts == 1U,
		"pw: a missing HW ACK is warned and the enable continues (not a caller failure)");

	/*
	 * The time-base anomaly during the acknowledge wait is not tested: the
	 * scripted time source it needs is no longer part of the driver.
	 */
}

/* Tests the combo PHY verify, the per-PHY init and the top-level init. */
static void
i915_ktest_combo_phy(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_trace *trace;
	uint32_t comp_dw0;
	uint32_t comp_dw8;
	uint32_t cl_dw5;
	uint32_t comp_dw9;
	unsigned count;
	int verified;
	int error;

	/* The test display's trace ring records the top-level init. */
	trace = &fx->display.dc_trace;
	drv_i915_trace_init(trace);

	/* A fresh PHY is not COMP_INIT-enabled, so it does not verify. */
	i915_ktest_mmio_open(fx);
	verified = drv_i915_combo_phy_verify_state(&fx->mmio, I915_COMBO_PHY_A);
	drv_i915_ktest_check(ktest, verified == 0, "combo: verify fails when the PHY is not COMP_INIT-enabled");

	/* The per-PHY init programs COMP_INIT, IREFGEN, CL_POWER_DOWN and the process monitor. */
	drv_i915_combo_phy_init_one(&fx->mmio, I915_COMBO_PHY_A);
	comp_dw0 = drv_i915_raw_read32(&fx->mmio, I915_KTEST_PHY_A_COMP_DW0);
	comp_dw8 = drv_i915_raw_read32(&fx->mmio, I915_KTEST_PHY_A_COMP_DW8);
	cl_dw5 = drv_i915_raw_read32(&fx->mmio, I915_KTEST_PHY_A_CL_DW5);
	comp_dw9 = drv_i915_raw_read32(&fx->mmio, I915_KTEST_PHY_A_COMP_DW9);
	drv_i915_ktest_check(
		ktest,
		(comp_dw0 & (1U << 31)) != 0U &&
		    (comp_dw8 & (1U << 24)) != 0U &&
		    (cl_dw5 & (1U << 4)) != 0U &&
		    comp_dw9 == 0x62AB67BBU,
		"combo: init_one programs COMP_INIT/IREFGEN/CL_POWER_DOWN/procmon");

	/* After the init the PHY verifies, the group writes having reached lane 0. */
	verified = drv_i915_combo_phy_verify_state(&fx->mmio, I915_COMBO_PHY_A);
	drv_i915_ktest_check(ktest, verified == 1, "combo: after init_one the PHY verifies (GRP->LN broadcast reflected)");

	/* COMP_INIT alone, with the rest not matching, is not an initialised PHY. */
	i915_ktest_mmio_open(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_PHY_A_COMP_DW0, 1U << 31);
	verified = drv_i915_combo_phy_verify_state(&fx->mmio, I915_COMBO_PHY_A);
	drv_i915_ktest_check(ktest, verified == 0, "combo: COMP_INIT alone (procmon/other mismatch) is not verified");

	/* Both PHYs already correct: the top-level init programs none. */
	i915_ktest_mmio_open(fx);
	drv_i915_combo_phy_init_one(&fx->mmio, I915_COMBO_PHY_A);
	drv_i915_combo_phy_init_one(&fx->mmio, I915_COMBO_PHY_B);
	count = 99U;
	error = drv_i915_combo_phy_init(&fx->mmio, trace, &count);
	drv_i915_ktest_check(
		ktest,
		error == 0 && count == 0U,
		"combo: init returns 0 (success) and re-initialises none when all verify");

	/* Unconfigured PHYs: the top-level init programs both and still reports success, not the count. */
	i915_ktest_mmio_open(fx);
	count = 99U;
	error = drv_i915_combo_phy_init(&fx->mmio, trace, &count);
	drv_i915_ktest_check(
		ktest,
		error == 0 && count == 2U,
		"combo: init returns 0 (NOT the count) so the D3 parent proceeds; 2 programmed");
}

/* Tests the CDCLK hook selection for ADL-P. */
static void
i915_ktest_cdclk_init(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	const struct i915_cdclk_vals *table;
	struct i915_cdclk_dev *cd;
	int step;

	/* Revision 0x0c is display stepping D0, not the raw revision. */
	step = drv_i915_adlp_display_step(0x0CU);
	drv_i915_ktest_check(ktest, step == I915_STEP_D0, "cdclk: adlp revid 0x0c maps to display STEP_D0 (not raw revision)");

	/* The hooks select the ADL-P table and the Tiger Lake functions, with crawl and without squash. */
	cd = &fx->display.cdclk;
	kern_memset(cd, 0, sizeof(*cd));
	drv_i915_init_cdclk_hooks(cd, 13, step, 1);
	table = drv_i915_adlp_cdclk_table();
	drv_i915_ktest_check(
		ktest,
		cd->table == table &&
		    cd->funcs == I915_CDCLK_FUNCS_TGL &&
		    cd->has_cdclk_crawl == 1 &&
		    cd->has_cdclk_squash == 0,
		"cdclk: hooks select adlp_cdclk_table + tgl funcs, crawl=1 squash=0");
}

/* Tests the CDCLK readout, the unchanged init, the sanitize and a refused PREPARE. */
static void
i915_ktest_cdclk_readout(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_cdclk_config cfg;
	struct i915_cdclk_dev *cd;
	uint32_t cdclk_ctl;

	/* A 38.4 MHz reference, the PLL locked at ratio 34 and the CD2X divider /2. */
	cd = &fx->display.cdclk;
	i915_ktest_mmio_open(fx);
	cd->m = &fx->mmio;
	cd->sb_lock = &fx->sb_lock;
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DE_PLL_ENABLE, (1U << 31) | 34U);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_CDCLK_CTL, 0x518U);

	/* The readout decodes the reference, the VCO, the bypass, the CDCLK and the voltage level. */
	kern_memset(&cfg, 0, sizeof(cfg));
	drv_i915_bxt_get_cdclk(cd, &cfg);
	drv_i915_ktest_check(
		ktest,
		cfg.ref == 38400U &&
		    cfg.vco == 1305600U &&
		    cfg.bypass == 19200U &&
		    cfg.cdclk == 652800U &&
		    cfg.voltage_level == 3U,
		"cdclk: bxt_get_cdclk decodes ref/vco/bypass/cdclk/voltage (652800 @ 38.4MHz)");

	/* A legal pre-OS state: the init reprograms nothing. */
	cd->hw = cfg;
	drv_i915_cdclk_init_hw(cd);
	drv_i915_ktest_check(
		ktest,
		cd->diag_no_change == 1 &&
		    cd->hw.cdclk == 652800U &&
		    cd->hw.vco == 1305600U &&
		    cd->diag_hw_sequence_reached == 0,
		"cdclk: CD-1 legal state -> no reprogram (cdclk/vco kept, no HW writes)");

	/* The PLL off before the OS: the sanitize forces a full setup (cdclk 0, VCO unknown). */
	i915_ktest_mmio_open(fx);
	i915_ktest_cdclk_reset(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DE_PLL_ENABLE, 0U);
	drv_i915_bxt_sanitize_cdclk(cd);
	drv_i915_ktest_check(
		ktest,
		cd->hw.cdclk == 0U && cd->hw.vco == ~0U,
		"cdclk: CD-2 pre-OS PLL off -> sanitize forces cdclk=0, vco=~0 (not a real freq)");

	/* The PCU refuses PREPARE: no PLL and no CDCLK_CTL write follows. */
	i915_ktest_mmio_open(fx);
	fx->fake.pcode_sticky_status = 0x2;
	i915_ktest_cdclk_reset(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	cd->hw.ref = 38400U;
	cd->hw.bypass = 19200U;
	cd->hw.vco = ~0U;
	cd->hw.cdclk = 0U;
	kern_memset(&cfg, 0, sizeof(cfg));
	cfg.cdclk = 652800U;
	cfg.vco = 1305600U;
	cfg.voltage_level = 3U;
	drv_i915_bxt_set_cdclk(cd, &cfg);
	cdclk_ctl = drv_i915_raw_read32(&fx->mmio, I915_KTEST_CDCLK_CTL);
	drv_i915_ktest_check(
		ktest,
		cd->diag_prepare_status != 0 &&
		    cd->diag_hw_sequence_reached == 0 &&
		    cdclk_ctl == 0U,
		"cdclk: CD-3 PCU prepare failure -> no PLL/CDCLK_CTL writes");
}

/* Tests the full CDCLK setup the init drives from an unusable state. */
static void
i915_ktest_cdclk_full_setup(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	/* PREPARE is approved (reply READY), then voltage level 0 is notified. */
	static const struct i915_ktest_pcode_txn ok_179k[] = {
		{ 0x7U, 0x3U, 0x1U, 0U, 0x0U, 0U },
		{ 0x7U, 0x0U, 0U, 0U, 0x0U, 0U }
	};
	struct i915_cdclk_dev *cd;
	int pll_disable;
	int pll_enable;
	int ctl;

	/* The CDCLK under test is the test display's. */
	cd = &fx->display.cdclk;

	/* The PLL off: the init runs PREPARE, the PLL disable and enable, CDCLK_CTL and the notify. */
	i915_ktest_mmio_open(fx);
	fx->fake.ptxn = ok_179k;
	fx->fake.ptxn_len = 2U;
	i915_ktest_cdclk_reset(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DE_PLL_ENABLE, 0U);
	fx->fake.wt_n = 0U;
	drv_i915_cdclk_init_hw(cd);

	/* Finds the PLL disable, the enable at ratio 14 and CDCLK_CTL at 179.2 MHz with the /1.5 divider. */
	pll_disable = i915_ktest_fake_find(&fx->fake, I915_KTEST_DE_PLL_ENABLE, 0U, 1U << 31);
	pll_enable = i915_ktest_fake_find(&fx->fake, I915_KTEST_DE_PLL_ENABLE, (1U << 31) | 14U, (1U << 31) | 0xffU);
	ctl = i915_ktest_fake_find(&fx->fake, I915_KTEST_CDCLK_CTL, 0x780164U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		fx->fake.ptxn_i == 2U &&
		    fx->fake.ptxn_bad == 0 &&
		    pll_disable >= 0 &&
		    pll_enable >= 0 &&
		    ctl >= 0 &&
		    pll_enable < ctl &&
		    cd->diag_hw_sequence_reached == 1 &&
		    cd->diag_prepare_status == 0 &&
		    cd->diag_notify_status == 0 &&
		    cd->hw.cdclk == 179200U &&
		    cd->hw.vco == 537600U &&
		    cd->hw.voltage_level == 0U,
		"cdclk: CD-2a init_hw full re-setup (PREPARE->PLL disable+enable->CDCLK_CTL->notify->state)");

	/* The PLL locked but CDCLK_CTL wrong: the state is still unknown and fully set up again. */
	i915_ktest_mmio_open(fx);
	fx->fake.ptxn = ok_179k;
	fx->fake.ptxn_len = 2U;
	i915_ktest_cdclk_reset(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DE_PLL_ENABLE, (1U << 31) | 34U);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_CDCLK_CTL, 0U);
	fx->fake.wt_n = 0U;
	drv_i915_cdclk_init_hw(cd);
	drv_i915_ktest_check(
		ktest,
		fx->fake.ptxn_i == 2U &&
		    fx->fake.ptxn_bad == 0 &&
		    cd->diag_no_change == 0 &&
		    cd->hw.cdclk == 179200U &&
		    cd->hw.vco == 537600U,
		"cdclk: CD-2a variant (PLL locked, CDCLK_CTL bad) still forces full re-setup via unknown");
}

/* Tests the CDCLK crawl from a known-good state and the same-VCO guard. */
static void
i915_ktest_cdclk_crawl(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	/* PREPARE is approved, then voltage level 2 is notified. */
	static const struct i915_ktest_pcode_txn ok_557k[] = {
		{ 0x7U, 0x3U, 0x1U, 0U, 0x0U, 0U },
		{ 0x7U, 0x2U, 0U, 0U, 0x0U, 0U }
	};
	struct i915_cdclk_config cfg;
	struct i915_cdclk_dev *cd;
	int pll_disable;
	int freq_req;
	int ctl;
	int pll_write;

	/* The CDCLK under test is the test display's. */
	cd = &fx->display.cdclk;

	/* From 307.2 MHz to 556.8 MHz: the ratio changes with FREQ_REQ, without a PLL disable. */
	i915_ktest_mmio_open(fx);
	fx->fake.ptxn = ok_557k;
	fx->fake.ptxn_len = 2U;
	i915_ktest_cdclk_reset(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	cd->hw.ref = 38400U;
	cd->hw.bypass = 19200U;
	cd->hw.cdclk = 307200U;
	cd->hw.vco = 614400U;
	kern_memset(&cfg, 0, sizeof(cfg));
	cfg.cdclk = 556800U;
	cfg.vco = 1113600U;
	cfg.voltage_level = 2U;
	fx->fake.wt_n = 0U;
	drv_i915_bxt_set_cdclk(cd, &cfg);

	/* Finds no PLL disable, the crawl's FREQ_REQ and CDCLK_CTL at 556.8 MHz with the /1 divider. */
	pll_disable = i915_ktest_fake_find(&fx->fake, I915_KTEST_DE_PLL_ENABLE, 0U, 1U << 31);
	freq_req = i915_ktest_fake_find(&fx->fake, I915_KTEST_DE_PLL_ENABLE, 1U << 23, 1U << 23);
	ctl = i915_ktest_fake_find(&fx->fake, I915_KTEST_CDCLK_CTL, 0x380458U, 0xffffffffU);
	drv_i915_ktest_check(
		ktest,
		fx->fake.ptxn_i == 2U &&
		    fx->fake.ptxn_bad == 0 &&
		    pll_disable < 0 &&
		    freq_req >= 0 &&
		    ctl >= 0 &&
		    cd->hw.cdclk == 556800U &&
		    cd->hw.vco == 1113600U &&
		    cd->hw.voltage_level == 2U,
		"cdclk: CD-2b crawl (ratio+FREQ_REQ, LOCK+ACK, no PLL disable) from known-good state");

	/* The same VCO needs no crawl: no PLL write at all. */
	i915_ktest_mmio_open(fx);
	fx->fake.ptxn = ok_557k;
	fx->fake.ptxn_len = 2U;
	i915_ktest_cdclk_reset(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	cd->hw.ref = 38400U;
	cd->hw.bypass = 19200U;
	cd->hw.cdclk = 556800U;
	cd->hw.vco = 1113600U;
	kern_memset(&cfg, 0, sizeof(cfg));
	cfg.cdclk = 556800U;
	cfg.vco = 1113600U;
	cfg.voltage_level = 2U;
	fx->fake.wt_n = 0U;
	drv_i915_bxt_set_cdclk(cd, &cfg);
	pll_write = i915_ktest_fake_find(&fx->fake, I915_KTEST_DE_PLL_ENABLE, 0U, 0U);
	drv_i915_ktest_check(ktest, pll_write < 0, "cdclk: CD-2b guard: same VCO emits no PLL/crawl write");
}

/* Tests a crawl whose final voltage notify is refused. */
static void
i915_ktest_cdclk_notify_fail(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	/* PREPARE is approved; the notify of voltage level 2 is refused (status 0x11). */
	static const struct i915_ktest_pcode_txn notifyfail_557k[] = {
		{ 0x7U, 0x3U, 0x1U, 0U, 0x00U, 0U },
		{ 0x7U, 0x2U, 0U, 0U, 0x11U, 0U }
	};
	struct i915_cdclk_config cfg;
	struct i915_cdclk_dev *cd;
	int pll_disable;
	int freq_req;
	int ctl;

	/* From 307.2 MHz at voltage level 0 towards 556.8 MHz at level 2. */
	cd = &fx->display.cdclk;
	i915_ktest_mmio_open(fx);
	fx->fake.ptxn = notifyfail_557k;
	fx->fake.ptxn_len = 2U;
	i915_ktest_cdclk_reset(fx);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);
	cd->hw.ref = 38400U;
	cd->hw.bypass = 19200U;
	cd->hw.cdclk = 307200U;
	cd->hw.vco = 614400U;
	cd->hw.voltage_level = 0U;
	kern_memset(&cfg, 0, sizeof(cfg));
	cfg.cdclk = 556800U;
	cfg.vco = 1113600U;
	cfg.voltage_level = 2U;
	fx->fake.wt_n = 0U;
	drv_i915_bxt_set_cdclk(cd, &cfg);

	/* Finds the crawl, no PLL disable, and CDCLK_CTL written. */
	freq_req = i915_ktest_fake_find(&fx->fake, I915_KTEST_DE_PLL_ENABLE, 1U << 23, 1U << 23);
	pll_disable = i915_ktest_fake_find(&fx->fake, I915_KTEST_DE_PLL_ENABLE, 0U, 1U << 31);
	ctl = i915_ktest_fake_find(&fx->fake, I915_KTEST_CDCLK_CTL, 0x380458U, 0xffffffffU);

	/*
	 * A partial update: the crawl advanced the VCO and CDCLK_CTL was
	 * written, but the final update of the software state did not run, so
	 * the CDCLK keeps its old 307.2 MHz and the voltage level stays 0
	 * rather than the requested 2.  The refused status 0x11 is EACCES.
	 */
	drv_i915_ktest_check(
		ktest,
		cd->diag_prepare_status == 0 &&
		    cd->diag_hw_sequence_reached == 1 &&
		    cd->diag_notify_status == EACCES &&
		    freq_req >= 0 &&
		    pll_disable < 0 &&
		    ctl >= 0 &&
		    cd->hw.vco == 1113600U &&
		    cd->hw.cdclk == 307200U &&
		    cd->hw.voltage_level == 0U,
		"cdclk: CD-4 notify-fail leaves a partial state (VCO advanced, cdclk/voltage kept, not requested)");
}

/* Builds the display-core fixture: the map, the CDCLK, the well context and the core on one register model. */
static void
i915_ktest_core_setup(
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display *display;
	struct i915_display_core *dc;
	struct i915_pw_ctx *pwc;

	/* The register model, the map, the CDCLK and the well context. */
	display = &fx->display;
	drv_i915_trace_init(&display->dc_trace);
	i915_ktest_mmio_open(fx);
	(void)drv_i915_power_domains_init(&display->power_domains, 13U, -1, 1, &display->dc_trace);
	i915_ktest_cdclk_reset(fx);
	i915_ktest_pwc_reset(fx);

	/* The core shares the one map, CDCLK, well context, register access and PCODE lock. */
	dc = &display->dcore;
	kern_memset(dc, 0, sizeof(*dc));
	dc->pd = &display->power_domains;
	dc->cd = &display->cdclk;
	dc->pwc = &display->pwc;
	dc->m = &fx->mmio;
	dc->sb_lock = &fx->sb_lock;

	/* The device the fixture stands for: ADL-P with four DBUF slices, two ABOX and LPDDR5 on two channels. */
	dc->display_ver = 13;
	dc->is_alderlake_p = 1;
	dc->dbuf_slice_mask = 0x0fU;
	dc->abox_mask = 0x03U;
	dc->dram_type = I915_DRAM_LPDDR5;
	dc->dram_channels = 2U;

	/* A 38.4 MHz reference. */
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_SKL_DSSM, I915_KTEST_DSSM_REF_38400);

	/* The DC_off enable reads the same CDCLK and DBUF mask the core keeps. */
	pwc = &display->pwc;
	pwc->cd = &display->cdclk;
	pwc->dbuf_slices = &dc->dbuf_enabled_slices;
	pwc->target_dc_state = display->power_domains.target_dc_state;
	pwc->allowed_dc_mask = display->power_domains.allowed_dc_mask;

	/* The VGA recorder starts empty. */
	kern_memset(&fx->vga, 0, sizeof(fx->vga));
}

/* Tests the display-core bring-up when the PHYs and the CDCLK need an init. */
static void
i915_ktest_core_normal(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	/* PREPARE is approved, then voltage level 0 is notified. */
	static const struct i915_ktest_pcode_txn ok_cdclk[] = {
		{ 0x7U, 0x3U, 0x1U, 0U, 0x0U, 0U },
		{ 0x7U, 0x0U, 0U, 0U, 0x0U, 0U }
	};
	struct i915_display_core *dc;

	/* Every fuse distributed, the DE PLL off: the CDCLK needs its full setup. */
	i915_ktest_core_setup(fx);
	fx->fake.fuse_status = 0xFFFFFFFFU;
	fx->fake.ptxn = ok_cdclk;
	fx->fake.ptxn_len = 2U;
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DE_PLL_ENABLE, 0U);
	dc = &fx->display.dcore;
	drv_i915_power_domains_init_hw(dc, 0);

	/* The bring-up completes: the INIT reference is held and every well synced. */
	drv_i915_ktest_check(
		ktest,
		dc->fault_stop == 0 &&
		    dc->reached_init_ref == 1 &&
		    dc->reached_sync_hw == 1 &&
		    dc->initializing == 0 &&
		    dc->init_wakeref_held == 1,
		"d3: D-NORMAL parent completes -> INIT ref held + all-well sync reached");

	/* The parts of the bring-up, each on the shared device. */
	i915_ktest_core_normal_parts(ktest, fx);
}

/* Tests what each part of the normal display-core bring-up left behind. */
static void
i915_ktest_core_normal_parts(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display *display;
	struct i915_ktest_vga_recorder *vga;
	uint32_t page_mask;
	uint32_t comp_dw0;
	int pw1;
	int pw1_enabled;

	/* The objects under test are the test display's. */
	display = &fx->display;
	vga = &fx->vga;

	/* The CDCLK ran on the shared device and used the whole PCODE script. */
	drv_i915_ktest_check(
		ktest,
		fx->fake.ptxn_bad == 0 && display->cdclk.hw.cdclk == 179200U,
		"d3: D-NORMAL CDCLK ran on the shared device (cdclk 179200, PCODE consumed)");

	/* DBUF slice S1 is on and the BW_BUDDY page mask is programmed. */
	page_mask = drv_i915_raw_read32(&fx->mmio, I915_KTEST_BW_BUDDY_PAGE_MASK);
	drv_i915_ktest_check(
		ktest,
		display->dcore.dbuf_enabled_slices == 0x1U && page_mask == 0x1CU,
		"d3: D-NORMAL DBUF slice S1 enabled + BW_BUDDY page mask programmed");

	/* Combo PHY A is initialised and PW1 is on, on the same register access. */
	comp_dw0 = drv_i915_raw_read32(&fx->mmio, I915_KTEST_PHY_A_COMP_DW0);
	pw1 = drv_i915_power_well_by_id(&display->power_domains, I915_SKL_DISP_PW_1);
	pw1_enabled = 0;
	if (pw1 >= 0)
		pw1_enabled = display->power_domains.power_wells[pw1].hw_enabled;
	drv_i915_ktest_check(
		ktest,
		(comp_dw0 & (1U << 31)) != 0U &&
		    pw1 >= 0 &&
		    pw1_enabled == 1,
		"d3: D-NORMAL combo PHY_A initialised + PW1 enabled (same MMIO)");

	/* The INIT get entered the DC_off body: DC disable, the CDCLK and DBUF compares and the PHY. */
	drv_i915_ktest_check(
		ktest,
		display->pwc.dc_off_enable_calls > 0U &&
		    display->pwc.dc_off_cdclk_readouts > 0U &&
		    display->pwc.dc_off_dbuf_asserts > 0U &&
		    display->pwc.dc_off_combo_inits > 0U,
		"d3: D-NORMAL INIT get enters DC_off body (DC disable + CDCLK/DBUF compare + PHY)");

	/* The VGA reset owned the legacy I/O: get, MIS_R read, MIS_W write of the same value, put. */
	drv_i915_ktest_check(
		ktest,
		vga->got == 1 &&
		    vga->put == 1 &&
		    vga->count >= 4U &&
		    vga->sequence[0] == 1 &&
		    vga->sequence[1] == 2 &&
		    vga->sequence[2] == 3 &&
		    vga->sequence[3] == 4 &&
		    vga->last_written == vga->last_read &&
		    vga->last_read == 0xABU,
		"d3: D-NORMAL VGA reset owns LEGACY_IO: get -> MIS_R read -> MIS_W write -> put");
}

/* Tests the driver remove after the normal bring-up. */
static void
i915_ktest_core_remove(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display_core *dc;
	unsigned sum_before;
	unsigned sum_after;

	/* The remove drops the runtime-PM side only: no domain put, the wells stay referenced. */
	dc = &fx->display.dcore;
	sum_before = i915_ktest_well_refcounts(&fx->display.power_domains);
	drv_i915_power_domains_driver_remove(dc);
	sum_after = i915_ktest_well_refcounts(&fx->display.power_domains);
	drv_i915_ktest_check(
		ktest,
		dc->pm_wakeref == 0 &&
		    dc->init_wakeref_held == 0 &&
		    sum_before > 0U &&
		    sum_after == sum_before,
		"d3: D-REMOVE cancels the rpm wakeref only; well refcounts unchanged (no domain put)");
}

/* Tests the display-core bring-up when the PHYs, the CDCLK and two DBUF slices are already right. */
static void
i915_ktest_core_preserve(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	/* An empty script: any PCODE transaction fails the case. */
	static const struct i915_ktest_pcode_txn no_pcode[] = {
		{ 0x7U, 0x3U, 0x1U, 0U, 0x0U, 0U }
	};
	struct i915_display_core *dc;

	/* Every fuse distributed, a legal CDCLK (ratio 34 locked, /2) and both PHYs initialised. */
	i915_ktest_core_setup(fx);
	fx->fake.fuse_status = 0xFFFFFFFFU;
	fx->fake.ptxn = no_pcode;
	fx->fake.ptxn_len = 0U;
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DE_PLL_ENABLE, (1U << 31) | 34U);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_CDCLK_CTL, 0x518U);
	drv_i915_combo_phy_init_one(&fx->mmio, I915_COMBO_PHY_A);
	drv_i915_combo_phy_init_one(&fx->mmio, I915_COMBO_PHY_B);

	/* DBUF slices S1 and S2 already powered, to tell "kept" from "always S1". */
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DBUF_CTL_S1, 1U << 31);
	drv_i915_raw_write32(&fx->mmio, I915_KTEST_DBUF_CTL_S2, 1U << 31);

	/* The bring-up keeps everything: no PCODE, the CDCLK as it is, both slices. */
	dc = &fx->display.dcore;
	drv_i915_power_domains_init_hw(dc, 0);
	drv_i915_ktest_check(
		ktest,
		dc->fault_stop == 0 &&
		    dc->reached_init_ref == 1 &&
		    dc->reached_sync_hw == 1 &&
		    fx->fake.ptxn_bad == 0 &&
		    fx->fake.ptxn_i == 0U &&
		    fx->display.cdclk.diag_no_change == 1 &&
		    dc->dbuf_enabled_slices == 0x3U,
		"d3: D-PRESERVE keeps existing DBUF slices (S1+S2) + appropriate PHY/CDCLK (no PCODE)");

	/*
	 * The fault injection mid-child (D-FAULT) and the driver remove after
	 * it are not tested: the faulting time source they need is no longer
	 * part of the driver, and the time-base fault is driver-wide.
	 */
}

/*
 * Tests the firmware provider: the DMC file the package installs, an absent
 * name, and the ownership of the image the provider and a test hand back.
 */
static void
i915_ktest_dmc_provider(
	struct i915_ktest *ktest)
{
	struct i915_test_firmware_override override;
	uint8_t test_image[I915_KTEST_FIRMWARE_TEST_IMAGE];
	struct i915_firmware fw;
	uint8_t digest[32];
	int hash_ok;
	int owned;
	unsigned i;
	int error;

	/* Reads /lib/firmware/i915/adlp_dmc.bin, which the i915-firmware package installs. */
	error = drv_i915_firmware_request(&fw, I915_KTEST_DMC_NAME);

	/* Compares the bytes with the pinned SHA-256 when the file has the pinned size. */
	hash_ok = 0;
	if (error == 0 && fw.size == I915_KTEST_DMC_SIZE) {
		drv_i915_sha256(fw.data, fw.size, digest);
		hash_ok = 1;
		for (i = 0U; i < 32U; i++) {
			if (digest[i] != i915_ktest_dmc_sha256[i])
				hash_ok = 0;
		}
	}

	/* The bytes are the provider's own buffer, which the release frees. */
	owned = 0;
	if (fw.allocation != NULL && fw.allocation == (const void *)fw.data)
		owned = 1;

	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    hash_ok == 1 &&
		    owned == 1,
		"dmc-fw: request i915/adlp_dmc.bin reads the 79088-byte package file (sha256 3516de2e..) into a buffer of its own");

	/* The release frees the provider's buffer and drops the handle. */
	drv_i915_firmware_release(&fw);
	drv_i915_ktest_check(
		ktest,
		fw.data == NULL &&
		    fw.size == 0U &&
		    fw.allocation == NULL,
		"dmc-fw: release frees the buffer and drops the handle");

	/* An absent file reports ENOENT and hands back no bytes. */
	error = drv_i915_firmware_request(&fw, "i915/nonexistent.bin");
	drv_i915_ktest_check(
		ktest,
		error == ENOENT &&
		    fw.data == NULL &&
		    fw.allocation == NULL,
		"dmc-fw: a genuinely absent firmware returns ENOENT with no bytes");

	/*
	 * Serves an image of the test's own.  The replacement is driver-wide;
	 * it is removed before anything else requests firmware.
	 */
	kern_memset(test_image, 0, sizeof(test_image));
	test_image[0] = 0x5aU;
	override.request = i915_ktest_firmware_image_request;
	override.context = test_image;
	drv_i915_test_firmware_set_override(&override);
	error = drv_i915_firmware_request(&fw, I915_KTEST_DMC_NAME);

	/* The request leaves the test's image without a provider allocation. */
	owned = 1;
	if (fw.allocation == NULL)
		owned = 0;

	/* The release drops the handle and leaves the test's bytes alone. */
	drv_i915_firmware_release(&fw);
	drv_i915_test_firmware_set_override(NULL);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    owned == 0 &&
		    fw.data == NULL &&
		    test_image[0] == 0x5aU,
		"dmc-fw: a test's image is not freed by the release");
}

/* Tests the DMC parse of the reference blob into the display's own payloads. */
static void
i915_ktest_dmc_parse(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_firmware fw;
	struct i915_dmc *dmc;
	int request_result;
	int parse_result;
	int main_present;
	int all_present;

	/* Parses the blob for ADL-P stepping D0 (revision 0x0c). */
	dmc = &fx->display.dmc_dev.dmc;
	request_result = drv_i915_firmware_request(&fw, I915_KTEST_DMC_NAME);
	drv_i915_dmc_prepare(dmc, 13, 'D', '0');
	parse_result = request_result;
	if (request_result == 0)
		parse_result = drv_i915_parse_dmc_fw(dmc, fw.data, fw.size);
	main_present = drv_i915_dmc_has_payload(dmc);
	i915_ktest_dmc_parse_log(dmc, parse_result, main_present);

	/* The payloads stay valid after the firmware handle is released. */
	drv_i915_firmware_release(&fw);

	/* MAIN and the four pipe DMCs are chosen. */
	all_present = 0;
	if (dmc->dmc_info[0].present &&
	    dmc->dmc_info[1].present &&
	    dmc->dmc_info[2].present &&
	    dmc->dmc_info[3].present &&
	    dmc->dmc_info[4].present)
		all_present = 1;

	/* Version 2.20, six entries, a v3 MAIN header and its 25096-byte payload saved. */
	drv_i915_ktest_check(
		ktest,
		parse_result == 0 &&
		    main_present == 1 &&
		    dmc->version == ((2U << 16) | 20U) &&
		    dmc->num_entries == 6U &&
		    dmc->package_header_ver == 2U &&
		    all_present == 1 &&
		    dmc->dmc_info[I915_DMC_FW_MAIN].header_ver == 3 &&
		    dmc->dmc_info[I915_DMC_FW_MAIN].payload_size == 25096U &&
		    dmc->dmc_info[I915_DMC_FW_MAIN].payload != NULL,
		"dmc: parse (v2.20) selects MAIN+4 pipes, MAIN payload 25096B saved (valid after fw release)");

	/* Forgets the payloads. */
	drv_i915_dmc_parse_reset(dmc);
}

/* Logs what the DMC parse found, the header and each chosen DMC. */
static void
i915_ktest_dmc_parse_log(
	const struct i915_dmc *dmc,
	int parse_result,
	int main_present)
{
	const struct i915_dmc_info *info;
	int id;

	/* The header. */
	kern_logf("i915: DMC-PARSE rc=%d ver=%u.%u pkg_ver=%u entries=%u css_len=%u main=%d\n",
	    parse_result,
	    (unsigned)(dmc->version >> 16),
	    (unsigned)(dmc->version & 0xffffU),
	    dmc->package_header_ver,
	    dmc->num_entries,
	    dmc->css_header_len_bytes,
	    main_present);

	/* Each chosen DMC. */
	for (id = 0; id < I915_DMC_FW_MAX; id++) {
		info = &dmc->dmc_info[id];
		if (!info->present)
			continue;

		kern_logf("i915: DMC-PARSE id=%d hv=%d off_dw=%u start=0x%x mmio=%u fwsz_dw=%u payload=%u present=%d\n",
		    id,
		    info->header_ver,
		    info->dmc_offset,
		    info->start_mmioaddr,
		    info->mmio_count,
		    info->dmc_fw_size,
		    info->payload_size,
		    info->present);
	}
}

/* Tests the DMC program load against the raw blob, and that a changed payload is detected. */
static void
i915_ktest_dmc_load(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_firmware fw;
	struct i915_dmc *dmc;
	uint8_t *main_payload;
	uint64_t expected_sum;
	uint64_t changed_sum;
	uint32_t dc_state;
	int request_result;

	/* Parses the blob. */
	dmc = &fx->display.dmc_dev.dmc;
	request_result = drv_i915_firmware_request(&fw, I915_KTEST_DMC_NAME);
	drv_i915_dmc_prepare(dmc, 13, 'D', '0');
	if (request_result == 0)
		(void)drv_i915_parse_dmc_fw(dmc, fw.data, fw.size);

	/* Loads the program into the register model. */
	i915_ktest_mmio_open(fx);
	fx->fake.wt_total = 0U;
	dc_state = 0xffffffffU;
	drv_i915_dmc_load_program(dmc, &fx->mmio, &dc_state);

	/*
	 * The expected payload sum comes from the raw blob at the parsed
	 * offsets, independent of the parser's saved copy, so a copy error is
	 * caught: 13019 payload, 35 trailing and 80 event-disable writes.
	 */
	expected_sum = i915_ktest_dmc_expected_sum(dmc, fw.data);
	drv_i915_ktest_check(
		ktest,
		dc_state == 0U &&
		    dmc->load_seq_completed == 1 &&
		    dmc->payload_writes == 13019U &&
		    dmc->aux_writes == 35U &&
		    dmc->evt_disable_writes == 80U &&
		    dmc->psum == expected_sum &&
		    fx->fake.wt_total == 13141U,
		"dmc: load_program writes 13019 payload + 35 aux + 80 evt-disable (verified vs raw blob)");

	/* Flips the MAIN payload's first byte and loads again: the sum must differ. */
	changed_sum = expected_sum;
	main_payload = (uint8_t *)(uintptr_t)dmc->dmc_info[I915_DMC_FW_MAIN].payload;
	if (main_payload != NULL) {
		main_payload[0] ^= 0xffU;
		i915_ktest_mmio_open(fx);
		drv_i915_dmc_load_program(dmc, &fx->mmio, &dc_state);
		changed_sum = dmc->psum;
		main_payload[0] ^= 0xffU;
	}
	drv_i915_ktest_check(
		ktest,
		changed_sum != expected_sum,
		"dmc: a one-DWORD payload change is detected against the fixed reference");

	/* Releases the image and the payloads. */
	drv_i915_firmware_release(&fw);
	drv_i915_dmc_parse_reset(dmc);
}

/*
 * Computes the payload sum the load must produce, from the raw blob: the
 * CSS and package headers are 528 bytes and a v3 DMC header 256, and the
 * sum runs over MAIN to PIPED in program order.
 */
static uint64_t
i915_ktest_dmc_expected_sum(
	const struct i915_dmc *dmc,
	const uint8_t *blob)
{
	const struct i915_dmc_info *info;
	const uint8_t *word;
	uint64_t sum;
	uint32_t address;
	uint32_t value;
	unsigned blob_offset;
	unsigned i;
	int id;

	/* Without the blob there is nothing to sum. */
	sum = 0U;
	if (blob == NULL)
		return sum;

	/* Sums each saved DMC's program dwords with their addresses. */
	for (id = 0; id < I915_DMC_FW_MAX; id++) {
		info = &dmc->dmc_info[id];
		if (info->payload == NULL)
			continue;

		/* The payload starts after the headers and the entry's DMC header. */
		blob_offset = 528U + info->dmc_offset * 4U + 256U;
		for (i = 0U; i < info->dmc_fw_size; i++) {
			word = blob + blob_offset + 4U * i;
			address = info->start_mmioaddr + i * 4U;
			value = (uint32_t)word[0] |
			    ((uint32_t)word[1] << 8) |
			    ((uint32_t)word[2] << 16) |
			    ((uint32_t)word[3] << 24);
			sum = sum * 1000003U + address + (uint64_t)value * 7U;
		}
	}

	/* Succeeded: reports the expected sum. */
	return sum;
}

/* Prepares the asynchronous DMC loader's map and well context on a register model with every fuse set. */
static void
i915_ktest_dmc_loader_setup(
	struct i915_ktest_display_fixture *fx)
{
	/* Every fuse distributed, so the wells come on during the DMC's INIT get. */
	drv_i915_trace_init(&fx->display.dc_trace);
	i915_ktest_mmio_open(fx);
	fx->fake.fuse_status = 0xFFFFFFFFU;
	(void)drv_i915_power_domains_init(&fx->display.power_domains, 13U, -1, 1, &fx->display.dc_trace);
	i915_ktest_pwc_reset(fx);
}

/* Tests the asynchronous DMC loader with the blob present and absent. */
static void
i915_ktest_dmc_async(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	int error;

	/* The loader's own workqueue. */
	i915_ktest_dmc_loader_setup(fx);
	error = drv_i915_workqueue_create(&fx->display.dmc_wq, "dmc");
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "dmc: could not create the DMC workqueue");
		drv_i915_power_domains_cleanup(&fx->display.power_domains);
		return;
	}

	/* Runs the cases and takes the workqueue and the map down. */
	i915_ktest_dmc_async_cases(ktest, fx);
	drv_i915_workqueue_destroy(&fx->display.dmc_wq);
	drv_i915_power_domains_cleanup(&fx->display.power_domains);
}

/* Runs the normal and the absent-firmware loader cases. */
static void
i915_ktest_dmc_async_cases(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_dmc_dev *dev;
	struct i915_power_domains *pd;
	unsigned refcounts_before;
	unsigned refcounts_after;

	/* The loader and the map under test are the test display's. */
	dev = &fx->display.dmc_dev;
	pd = &fx->display.power_domains;

	/* The worker loads the default image and gives the DMC's reference back: the counts net to 0. */
	refcounts_before = i915_ktest_well_refcounts(pd);
	kern_memset(dev, 0, sizeof(*dev));
	drv_i915_dmc_init(dev, &fx->display.dmc_wq, &fx->mmio, pd, &fx->display.pwc, 13, 1, 'D', '0', NULL);
	(void)drv_i915_flush_work(&fx->display.dmc_wq, &dev->work, drv_i915_ktest_deadline_ms(2000U));
	refcounts_after = i915_ktest_well_refcounts(pd);
	drv_i915_ktest_check(
		ktest,
		dev->work_submitted == 1 &&
		    dev->worker_started == 1 &&
		    dev->firmware_acquired == 1 &&
		    dev->main_payload_present == 1 &&
		    dev->load_seq_completed_flag == 1 &&
		    dev->dmc_wakeref_held == 0 &&
		    dev->dmc.payload_writes == 13019U &&
		    refcounts_after == refcounts_before,
		"dmc: DMC-NORMAL worker loads the payload; DMC ref released (nets to 0), no leak");
	drv_i915_dmc_fini(dev, drv_i915_ktest_deadline_ms(2000U));

	/* An absent image asks for the fallback, loads nothing and keeps the DMC's reference. */
	kern_memset(dev, 0, sizeof(*dev));
	drv_i915_dmc_init(dev, &fx->display.dmc_wq, &fx->mmio, pd, &fx->display.pwc, 13, 1, 'D', '0', "i915/absent.bin");
	(void)drv_i915_flush_work(&fx->display.dmc_wq, &dev->work, drv_i915_ktest_deadline_ms(2000U));
	drv_i915_ktest_check(
		ktest,
		dev->worker_started == 1 &&
		    dev->fallback_requested == 1 &&
		    dev->main_payload_present == 0 &&
		    dev->dmc.payload_writes == 0U &&
		    dev->dmc_wakeref_held == 1,
		"dmc: DMC-NO-FW requests default+fallback, no load, DMC ref held (blocks rpm)");

	/* The stop gives the kept reference back. */
	drv_i915_dmc_fini(dev, drv_i915_ktest_deadline_ms(2000U));
	drv_i915_ktest_check(ktest, dev->dmc_wakeref_held == 0, "dmc: fini releases the still-held DMC ref (failure path)");
}

/* Tests the loader on a blob whose MAIN header is corrupt. */
static void
i915_ktest_dmc_bad_fw(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	int error;

	/* The loader's own workqueue. */
	i915_ktest_dmc_loader_setup(fx);
	error = drv_i915_workqueue_create(&fx->display.dmc_wq, "dmc2");
	if (error != 0) {
		drv_i915_ktest_check(ktest, 0, "dmc: could not create the DMC workqueue (2)");
		drv_i915_power_domains_cleanup(&fx->display.power_domains);
		return;
	}

	/* Runs the case and takes the workqueue and the map down. */
	i915_ktest_dmc_bad_fw_cases(ktest, fx);
	drv_i915_workqueue_destroy(&fx->display.dmc_wq);
	drv_i915_power_domains_cleanup(&fx->display.power_domains);

	/*
	 * The stop of a loader that is still running and the stop after a
	 * register fault mid-load are not tested: the pause and fault-at hooks
	 * of the loader are no longer part of the driver.
	 */
}

/* Runs the corrupt-header case: no MAIN payload, no program write, the reference kept and then recovered. */
static void
i915_ktest_dmc_bad_fw_cases(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_test_firmware_override override;
	struct i915_firmware fw;
	struct i915_dmc_dev *dev;
	int error;

	/* Copies the blob and gives the MAIN header the unknown version 7. */
	error = drv_i915_firmware_request(&fw, I915_KTEST_DMC_NAME);
	if (error == 0 && fw.size == I915_KTEST_DMC_SIZE)
		kern_memcpy(fx->dmc_badcopy, fw.data, I915_KTEST_DMC_SIZE);
	drv_i915_firmware_release(&fw);
	fx->dmc_badcopy[I915_KTEST_DMC_MAIN_VERSION_BYTE] = 7U;

	/*
	 * Serves the corrupt copy under the DMC name.  The replacement is
	 * driver-wide; it is installed only while this loader runs and removed
	 * after its work is done.
	 */
	override.request = i915_ktest_dmc_bad_request;
	override.context = fx;
	drv_i915_test_firmware_set_override(&override);

	/* The header is refused: nothing is programmed and the DMC's reference stays held. */
	dev = &fx->display.dmc_dev;
	kern_memset(dev, 0, sizeof(*dev));
	drv_i915_dmc_init(dev, &fx->display.dmc_wq, &fx->mmio, &fx->display.power_domains, &fx->display.pwc, 13, 1, 'D', '0', NULL);
	(void)drv_i915_flush_work(&fx->display.dmc_wq, &dev->work, drv_i915_ktest_deadline_ms(2000U));
	drv_i915_ktest_check(
		ktest,
		dev->firmware_acquired == 1 &&
		    dev->main_payload_present == 0 &&
		    dev->dmc.payload_writes == 0U &&
		    dev->dmc.load_seq_completed == 0 &&
		    dev->dmc_wakeref_held == 1,
		"dmc: DMC-BAD-FW corrupt MAIN header -> no MAIN payload, no program write, ref held");

	/* The stop recovers the reference and the partly used payload storage; then the replacement goes. */
	drv_i915_dmc_fini(dev, drv_i915_ktest_deadline_ms(2000U));
	drv_i915_test_firmware_set_override(NULL);
	drv_i915_ktest_check(
		ktest,
		dev->dmc_wakeref_held == 0 && dev->dmc.dmc_info[I915_DMC_FW_PIPEA].payload == NULL,
		"dmc: DMC-BAD-FW fini recovers the held ref and the partially used arena");
}

/*
 * Fills the SAGV bandwidth table.
 *
 * The plane counts make the Tiger Lake index (a reverse scan for
 * num_planes <= the group's, falling back to 0) and the Ice Lake index (a
 * forward scan for num_planes >= the group's, falling back to UINT_MAX)
 * land on different groups: group 0 carries the Ice Lake answer and group
 * 5 the Tiger Lake one, which shows that both were kept.
 */
static void
i915_ktest_state_bw_fixture(
	struct i915_bw_state *bw)
{
	unsigned k;

	/* Three QGV and two PSF points per group; group 0 has one plane, the others four. */
	kern_memset(bw, 0, sizeof(*bw));
	for (k = 0U; k < (unsigned)I915_BW_GROUPS; k++) {
		bw->max[k].num_qgv_points = 3U;
		bw->max[k].num_psf_gv_points = 2U;
		bw->max[k].num_planes = 4U;
		if (k == 0U)
			bw->max[k].num_planes = 1U;
	}

	/* Group 0 peaks at QGV point 0, group 5 at point 1. */
	bw->max[0].deratedbw[0] = 500U;
	bw->max[0].deratedbw[1] = 100U;
	bw->max[0].deratedbw[2] = 100U;
	bw->max[5].deratedbw[0] = 100U;
	bw->max[5].deratedbw[1] = 300U;
	bw->max[5].deratedbw[2] = 200U;

	/* A PSF tie, which gives a mask of two bits. */
	bw->max[0].psf_bw[0] = 50U;
	bw->max[0].psf_bw[1] = 50U;
	bw->valid = 1;
}

/* Tests the ADL-P mode configuration and the version and cursor ladders. */
static void
i915_ktest_state_mode(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display_state *ds;
	unsigned ok;

	/* ADL-P takes the modern arms: 16K limits, 256 cursors and an empty global list. */
	ds = &fx->display.dstate;
	kern_memset(ds, 0, sizeof(*ds));
	drv_i915_mode_config_init(ds, 13, I915_PLAT_NONE);
	drv_i915_ktest_check(
		ktest,
		ds->mode_config.max_width == 16384U &&
		    ds->mode_config.max_height == 16384U &&
		    ds->mode_config.cursor_width == 256U &&
		    ds->mode_config.cursor_height == 256U &&
		    ds->mode_config.min_width == 0U &&
		    ds->mode_config.min_height == 0U &&
		    ds->mode_config.preferred_depth == 24U &&
		    ds->mode_config.prefer_shadow == 1 &&
		    ds->mode_config.async_page_flip == 1 &&
		    ds->mode_config.funcs_set == 1 &&
		    ds->mode_config.helper_private_set == 1 &&
		    ds->obj_list_inited == 1 &&
		    ds->obj_count == 0U &&
		    ds->obj_list_head == NULL,
		"ds: DS-MODE mode_config_init(ADL-P) limits + empty global obj_list");

	/* The ladders are real branches, not constants. */
	ok = i915_ktest_state_mode_ladders(&fx->scratch_state);
	drv_i915_ktest_check(ktest, ok == 1U, "ds: DS-MODE version/cursor ladders take every reference arm");
}

/* Walks the version and cursor ladders of the mode configuration; reports 1 when every arm answered right. */
static unsigned
i915_ktest_state_mode_ladders(
	struct i915_display_state *t)
{
	unsigned ok;

	/* Every arm answers right until one does not. */
	ok = 1U;

	/* Version 4: 8K and no asynchronous flips. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_mode_config_init(t, 4, I915_PLAT_NONE);
	if (t->mode_config.max_width != 8192U || t->mode_config.async_page_flip != 0)
		ok = 0U;

	/* Version 3: 4K. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_mode_config_init(t, 3, I915_PLAT_NONE);
	if (t->mode_config.max_width != 4096U)
		ok = 0U;

	/* Version 2: 2K. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_mode_config_init(t, 2, I915_PLAT_NONE);
	if (t->mode_config.max_width != 2048U)
		ok = 0U;

	/* The 845G cursor: 64 by 1023. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_mode_config_init(t, 4, I915_PLAT_I845G);
	if (t->mode_config.cursor_width != 64U || t->mode_config.cursor_height != 1023U)
		ok = 0U;

	/* The 865G cursor: 512 by 1023. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_mode_config_init(t, 4, I915_PLAT_I865G);
	if (t->mode_config.cursor_width != 512U || t->mode_config.cursor_height != 1023U)
		ok = 0U;

	/* The 915GM cursor: 64 by 64. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_mode_config_init(t, 3, I915_PLAT_I915GM);
	if (t->mode_config.cursor_width != 64U || t->mode_config.cursor_height != 64U)
		ok = 0U;

	/* Succeeded: reports whether every arm answered right. */
	return ok;
}

/* Tests that version 12 and later use the Tiger Lake QGV index and earlier the Ice Lake one. */
static void
i915_ktest_state_index(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	uint16_t mask13;
	uint16_t mask11;
	unsigned psf_mask;
	uint16_t qgv_mask;

	/* Reads the masks of both indexes, the PSF tie and the whole QGV set. */
	mask13 = (uint16_t)drv_i915_icl_max_bw_qgv_point_mask(&fx->bw, 13, 0);
	mask11 = (uint16_t)drv_i915_icl_max_bw_qgv_point_mask(&fx->bw, 11, 0);
	psf_mask = drv_i915_icl_max_bw_psf_gv_point_mask(&fx->bw);
	qgv_mask = drv_i915_icl_qgv_points_mask(&fx->bw);
	drv_i915_ktest_check(
		ktest,
		mask13 == 0x2U &&
		    mask11 == 0x1U &&
		    psf_mask == 0x3U &&
		    qgv_mask == 0x307U,
		"ds: DS-INDEX ver>=12 uses tgl index (BIT1), ver<12 uses icl index (BIT0); psf tie ORs");
}

/* Tests the four global objects, their order and the forced SAGV disable. */
static void
i915_ktest_state_objects(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	/* The SAGV configuration request with points mask 0x5, answered safe. */
	static const struct i915_ktest_pcode_txn sagv_ok[] = {
		{ 0xeU, 0x5U, 0x0U, 0U, 0x0U, 0U }
	};
	struct i915_display_state *ds;
	int error;

	/* Registers the four objects, with SAGV enabled so the bandwidth init forces it off. */
	ds = &fx->display.dstate;
	kern_memset(ds, 0, sizeof(*ds));
	drv_i915_mode_config_init(ds, 13, I915_PLAT_NONE);
	error = drv_i915_cdclk_init(ds);
	error |= drv_i915_color_init(ds, 13);
	error |= drv_i915_dbuf_init(ds);
	i915_ktest_mmio_open(fx);
	fx->fake.ptxn = sagv_ok;
	fx->fake.ptxn_len = 1U;
	fx->bw.sagv_status = (int)I915_SAGV_ENABLED;
	error |= drv_i915_bw_init(ds, 13, &fx->bw, &fx->sb_lock, &fx->mmio);
	error |= drv_i915_pmdemand_init(ds, 13, &fx->mmio, -1);

	/* The list is cdclk, dbuf, bw, pmdemand, each appended at the tail. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    ds->obj_count == 4U &&
		    ds->color_done == 1 &&
		    ds->obj_list_head == &ds->cdclk_obj &&
		    ds->cdclk_obj.next == &ds->dbuf_obj &&
		    ds->dbuf_obj.next == &ds->bw_obj &&
		    ds->bw_obj.next == &ds->pmdemand_obj &&
		    ds->pmdemand_obj.next == NULL &&
		    ds->obj_list_tail == &ds->pmdemand_obj,
		"ds: DS-OBJ global obj_list order is cdclk,dbuf,bw,pmdemand (tail-appended)");

	/* Each state points back at its object with one reference; the hooks differ; no version-14 WA. */
	drv_i915_ktest_check(
		ktest,
		ds->cdclk_obj.state == &ds->cdclk_state.base &&
		    ds->cdclk_state.base.obj == &ds->cdclk_obj &&
		    ds->cdclk_state.base.ref == 1U &&
		    ds->bw_obj_state.base.ref == 1U &&
		    ds->pmdemand_state.base.obj == &ds->pmdemand_obj &&
		    ds->cdclk_obj.funcs != ds->dbuf_obj.funcs &&
		    ds->bw_obj.funcs != ds->pmdemand_obj.funcs &&
		    ds->pmdemand_wa_14016740474 == 0,
		"ds: DS-OBJ each state back-links its obj, kref=1, funcs distinct, no ver14 WA");

	/*
	 * The forced disable is one real PCODE transaction: the QGV mask keeps
	 * point 1 and both PSF points out, ~(0x2 | 0x300) & 0x307 = 0x5, and the
	 * status becomes DISABLED.
	 */
	drv_i915_ktest_check(
		ktest,
		fx->fake.ptxn_i == 1U &&
		    fx->fake.ptxn_bad == 0 &&
		    ds->sagv_force_disable_attempted == 1 &&
		    ds->sagv_qgv_points == 0x2U &&
		    ds->sagv_psf_points == 0x3U &&
		    ds->bw_obj_state.qgv_points_mask == 0x5U &&
		    ds->sagv_pcode_ret == 0 &&
		    fx->bw.sagv_status == (int)I915_SAGV_DISABLED,
		"ds: DS-SAGV bw_init forces SAGV off via PCODE 0xe data 0x5, status -> DISABLED");
}

/* Tests that SAGV the display does not control is left alone. */
static void
i915_ktest_state_sagv_skip(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display_state *ds;
	int not_controlled;
	int enabled;
	int old_version;
	int error;

	/* No PCODE script: any transaction would be counted. */
	ds = &fx->display.dstate;
	kern_memset(ds, 0, sizeof(*ds));
	drv_i915_mode_config_init(ds, 13, I915_PLAT_NONE);
	i915_ktest_mmio_open(fx);
	fx->bw.sagv_status = (int)I915_SAGV_NOT_CONTROLLED;
	error = drv_i915_bw_init(ds, 13, &fx->bw, &fx->sb_lock, &fx->mmio);

	/* Asks whether SAGV exists: not controlled, enabled, and before version 9. */
	not_controlled = drv_i915_has_sagv(13, I915_PLAT_NONE, (int)I915_SAGV_NOT_CONTROLLED);
	enabled = drv_i915_has_sagv(13, I915_PLAT_NONE, (int)I915_SAGV_ENABLED);
	old_version = drv_i915_has_sagv(8, I915_PLAT_NONE, (int)I915_SAGV_ENABLED);
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    ds->obj_count == 1U &&
		    fx->fake.pcode_txn_count == 0U &&
		    ds->sagv_force_disable_attempted == 0 &&
		    fx->bw.sagv_status == (int)I915_SAGV_NOT_CONTROLLED &&
		    not_controlled == 0 &&
		    enabled == 1 &&
		    old_version == 0,
		"ds: DS-SAGV-SKIP NOT_CONTROLLED (or ver<9) skips the forced disable entirely");
}

/* Tests that a PCODE error of the forced disable is only logged. */
static void
i915_ktest_state_sagv_fail(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display_state *ds;
	int error;

	/* Every PCODE transaction ends with an error status. */
	ds = &fx->display.dstate;
	kern_memset(ds, 0, sizeof(*ds));
	drv_i915_mode_config_init(ds, 13, I915_PLAT_NONE);
	i915_ktest_mmio_open(fx);
	fx->fake.pcode_sticky_status = 0x2;
	fx->bw.sagv_status = (int)I915_SAGV_ENABLED;
	error = drv_i915_bw_init(ds, 13, &fx->bw, &fx->sb_lock, &fx->mmio);

	/* The init still succeeds and the status is not updated. */
	drv_i915_ktest_check(
		ktest,
		error == 0 &&
		    ds->sagv_force_disable_attempted == 1 &&
		    ds->sagv_pcode_ret != 0 &&
		    fx->bw.sagv_status == (int)I915_SAGV_ENABLED &&
		    ds->obj_count == 1U,
		"ds: DS-SAGV-FAIL PCODE error leaves sagv.status untouched; bw_init still succeeds");

	/*
	 * The allocation failure of a global state (DS-ENOMEM) is not tested:
	 * the state storage is the device's, its allocation cannot fail, and
	 * the failure hook is no longer part of the driver.
	 */
}

/* Tests that the colour init of version 10 reports itself unimplemented. */
static void
i915_ktest_state_color(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display_state *t;
	int error13;
	int done13;
	int error10;

	/* Version 13 has nothing to do; version 10 is not a silent success. */
	t = &fx->scratch_state;
	kern_memset(t, 0, sizeof(*t));
	error13 = drv_i915_color_init(t, 13);
	done13 = t->color_done;
	error10 = drv_i915_color_init(t, 10);
	drv_i915_ktest_check(
		ktest,
		error13 == 0 &&
		    done13 == 1 &&
		    error10 == ENOSYS,
		"ds: DS-COLOR ver!=10 returns 0; ver==10 reports -ENOSYS (no silent success)");
}

/* Tests the PCI quirk table on ADL-P, an exact match and a wrong subsystem. */
static void
i915_ktest_state_quirks(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display_state *t;
	unsigned ok;

	/* Every case answers right until one does not. */
	ok = 1U;
	t = &fx->scratch_state;

	/* ADL-P matches nothing; the DMI list is walked without a backend. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_init_quirks(t, 0x46a8U, 0x8086U, 0x2212U);
	if (t->quirk_mask != 0U ||
	    t->quirk_hooks_fired != 0U ||
	    t->dmi_scanned != 1 ||
	    t->dmi_available != 0)
		ok = 0U;

	/* The Acer C720 entry fires on its exact device and subsystem. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_init_quirks(t, 0x0a06U, 0x1025U, 0x0a11U);
	if (t->quirk_mask != (1U << I915_QUIRK_BACKLIGHT_PRESENT) || t->quirk_hooks_fired != 1U)
		ok = 0U;

	/* The right device with the wrong subsystem matches nothing. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_init_quirks(t, 0x0a06U, 0x1025U, 0x9999U);
	if (t->quirk_mask != 0U)
		ok = 0U;

	/*
	 * The DMI entry case (a forced Lillipup match) is not tested: the DMI
	 * match hook is no longer part of the driver, and the kernel has no DMI
	 * backend to match against.
	 */
	drv_i915_ktest_check(ktest, ok == 1U, "ds: DS-QUIRKS ADL-P matches nothing; PCI + DMI entries fire on an exact match");
}

/* Tests the FBC creation, the VT-d workaround, the option sanitize and the functions ladder. */
static void
i915_ktest_state_fbc(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	unsigned ok;
	unsigned ladder_ok;
	int bdw;
	int older;
	int forced;

	/* The creation cases and the functions ladder. */
	ok = i915_ktest_state_fbc_cases(&fx->scratch_state);
	ladder_ok = i915_ktest_state_fbc_ladder(&fx->scratch_state);
	if (ladder_ok != 1U)
		ok = 0U;

	/* enable_fbc is a tri-state: Broadwell is the only default yes before version 9. */
	bdw = drv_i915_sanitize_fbc_option(8, I915_PLAT_BROADWELL, 0x1U, -1);
	older = drv_i915_sanitize_fbc_option(8, I915_PLAT_NONE, 0x1U, -1);
	forced = drv_i915_sanitize_fbc_option(13, I915_PLAT_NONE, 0x0U, 1);
	if (bdw != 1 || older != 0 || forced != 1)
		ok = 0U;

	drv_i915_ktest_check(ktest, ok == 1U, "ds: DS-FBC vtd WA / sanitize tri-state / per-id create / funcs ladder");
}

/* Walks the FBC creation cases; reports 1 when every case answered right. */
static unsigned
i915_ktest_state_fbc_cases(
	struct i915_display_state *t)
{
	unsigned ok;

	/* Every case answers right until one does not. */
	ok = 1U;

	/* ADL-P: FBC A with the IVB functions and its lock, the option on, no VT-d workaround. */
	kern_memset(t, 0, sizeof(*t));
	t->enable_fbc_param = -1;
	drv_i915_fbc_init(t, 13, 0x1U, 0, I915_PLAT_NONE);
	if (t->fbc_created != 1U ||
	    t->fbc[0] == NULL ||
	    t->fbc[1] != NULL ||
	    t->enable_fbc_sanitized != 1 ||
	    t->fbc_vtd_wa != 0) {
		ok = 0U;
	} else if (t->fbc[0]->id != I915_FBC_A ||
	    t->fbc[0]->funcs_kind != I915_FBC_FUNCS_IVB ||
	    t->fbc[0]->lock_inited != 1) {
		ok = 0U;
	}

	/* enable_fbc=0 sanitizes to 0 but the instances are still created. */
	kern_memset(t, 0, sizeof(*t));
	t->enable_fbc_param = 0;
	drv_i915_fbc_init(t, 13, 0x3U, 0, I915_PLAT_NONE);
	if (t->enable_fbc_sanitized != 0 || t->fbc_created != 2U || t->fbc[1] == NULL) {
		ok = 0U;
	} else if (t->fbc[1]->id != I915_FBC_B) {
		ok = 0U;
	}

	/* No FBC in the runtime mask: the option sanitizes to 0 and nothing is created. */
	kern_memset(t, 0, sizeof(*t));
	t->enable_fbc_param = -1;
	drv_i915_fbc_init(t, 13, 0x0U, 0, I915_PLAT_NONE);
	if (t->enable_fbc_sanitized != 0 || t->fbc_created != 0U)
		ok = 0U;

	/* WaFbcTurnOffFbcWhenHyperVisorIsUsed: Skylake with VT-d loses FBC. */
	kern_memset(t, 0, sizeof(*t));
	t->enable_fbc_param = -1;
	drv_i915_fbc_init(t, 9, 0x1U, 1, I915_PLAT_SKYLAKE);
	if (t->fbc_vtd_wa != 1 || t->fbc_mask != 0U || t->fbc_created != 0U)
		ok = 0U;

	/* VT-d on anything but Skylake or Broxton keeps FBC. */
	kern_memset(t, 0, sizeof(*t));
	t->enable_fbc_param = -1;
	drv_i915_fbc_init(t, 13, 0x1U, 1, I915_PLAT_NONE);
	if (t->fbc_vtd_wa != 0 || t->fbc_created != 1U)
		ok = 0U;

	/* Succeeded: reports whether every case answered right. */
	return ok;
}

/* Walks the FBC functions ladder by display version; reports 1 when every arm answered right. */
static unsigned
i915_ktest_state_fbc_ladder(
	struct i915_display_state *t)
{
	static const int versions[] = { 6, 5, 4, 3 };
	static const int kinds[] = {
		I915_FBC_FUNCS_SNB,
		I915_FBC_FUNCS_ILK,
		I915_FBC_FUNCS_I965,
		I915_FBC_FUNCS_I8XX
	};
	unsigned ok;
	unsigned i;

	/* Every arm answers right until one does not. */
	ok = 1U;

	/* Each version picks its own functions. */
	for (i = 0U; i < sizeof(versions) / sizeof(versions[0]); i++) {
		kern_memset(t, 0, sizeof(*t));
		drv_i915_fbc_init(t, versions[i], 0x1U, 0, I915_PLAT_NONE);
		if (t->fbc[0] == NULL) {
			ok = 0U;
		} else if (t->fbc[0]->funcs_kind != kinds[i]) {
			ok = 0U;
		}
	}

	/* Succeeded: reports whether every arm answered right. */
	return ok;
}

/* Tests that the teardown unlinks the global objects, drops their references and releases FBC. */
static void
i915_ktest_state_fini(
	struct i915_ktest *ktest,
	struct i915_ktest_display_fixture *fx)
{
	struct i915_display_state *ds;

	/* Builds the mode configuration, the CDCLK and DBUF objects and FBC A. */
	ds = &fx->display.dstate;
	kern_memset(ds, 0, sizeof(*ds));
	drv_i915_mode_config_init(ds, 13, I915_PLAT_NONE);
	(void)drv_i915_cdclk_init(ds);
	(void)drv_i915_dbuf_init(ds);
	ds->enable_fbc_param = -1;
	drv_i915_fbc_init(ds, 13, 0x1U, 0, I915_PLAT_NONE);

	/* Takes it apart. */
	drv_i915_display_state_fini(ds);
	drv_i915_ktest_check(
		ktest,
		ds->obj_count == 0U &&
		    ds->obj_list_head == NULL &&
		    ds->obj_list_tail == NULL &&
		    ds->cdclk_obj.next == NULL &&
		    ds->cdclk_obj.state == NULL &&
		    ds->cdclk_state.base.ref == 0U &&
		    ds->dbuf_state.base.ref == 0U &&
		    ds->fbc[0] == NULL &&
		    ds->fbc_created == 0U &&
		    ds->inited == 0,
		"ds: DS-FINI global objs unlinked + refs dropped + fbc released");
}
