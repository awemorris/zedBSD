/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The in-kernel tests of the display probe: the PCH id table, the display
 * interrupt reset, postinstall, handler, power-well hooks and vblank
 * references, and the front of the display probe (watermark latencies,
 * shared DPLLs, crtcs, the maximum CDCLK, the display workarounds, the
 * output setup, the readout, the sanitize and the VGA plane).
 *
 * Nothing here touches the started device.  Every register access goes to
 * a register model in this file, and every display object the code under
 * test reaches through its containing display (the vblank, the hotplug and
 * the VGA paths find the display with container_of) lives in a private
 * display allocated for the run and freed at its end.  The live display's
 * objects and the driver-global pointers are never used.
 */

#include "ktest.h"
#include <kern/kcrt.h>

#include "../../display/internal.h"
#include "../../display/display.h"
#include "../../display/interrupts.h"
#include "../../display/power.h"
#include "../../display/takeover.h"
#include "../../display/vbt-parse.h"

#include "../../irq.h"
#include "../../mmio.h"
#include "../../trace.h"

#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* How many register writes the model records, in order. */
#define I915_FAKE_WRITE_RECORDS		96U

/* How many distinct plain registers the model keeps. */
#define I915_FAKE_REGISTERS		96U

/* The fuse status register the power-well enables wait on. */
#define I915_FAKE_FUSE_STATUS		0x42000U

/* The PCODE mailbox and its two data words. */
#define I915_FAKE_PCODE_MAILBOX		0x138124U
#define I915_FAKE_PCODE_DATA		0x138128U
#define I915_FAKE_PCODE_DATA1		0x13812cU
#define I915_FAKE_PCODE_READY		0x80000000U

/* The firmware and driver request registers of the three power-well banks. */
#define I915_FAKE_PW_HSW_BIOS		0x45400U
#define I915_FAKE_PW_HSW_DRIVER		0x45404U
#define I915_FAKE_PW_AUX_BIOS		0x45440U
#define I915_FAKE_PW_AUX_DRIVER		0x45444U
#define I915_FAKE_PW_DDI_BIOS		0x45450U
#define I915_FAKE_PW_DDI_DRIVER		0x45454U

/* The request bits of a power-well register; each state bit sits one below its request bit. */
#define I915_FAKE_PW_REQUEST_BITS	0xAAAAAAAAU

/* The ISA bridge PCH ids the id table check uses. */
#define I915_PROBE_PCI_VENDOR_QUMRANET	0x1af4U
#define I915_PROBE_PCI_SUBDEVICE_QEMU	0x1100U

/*
 * One scripted PCODE transaction of the register model.
 *
 * A transaction is consumed by each write of the mailbox; the command and
 * the data word written before it are checked against the script, and the
 * scripted answer is loaded.
 */
struct i915_fake_pcode_txn {
	/* The command expected in the mailbox (without READY); 0xffffffff accepts any. */
	uint32_t expected_mbox;

	/* The data word expected before the mailbox write; 0xffffffff accepts any. */
	uint32_t expected_data;

	/* The data words handed back to the reader. */
	uint32_t response_data;
	uint32_t response_data1;

	/* The mailbox status byte once READY clears. */
	uint8_t status;
};

/*
 * The register model the tests run the code under test against.
 *
 * One instance lives for the whole run; each test opens it again, which
 * clears every register and the write record.  Plain registers read back
 * what was last written (0 before any write); the fuse status, the PCODE
 * mailbox and the power-well request registers behave like the hardware.
 */
struct i915_fake_mmio {
	/* The writes in the order they were made, up to the record's capacity. */
	uint32_t write_offset[I915_FAKE_WRITE_RECORDS];
	uint32_t write_value[I915_FAKE_WRITE_RECORDS];
	unsigned write_count;

	/* Every write, recorded or not. */
	unsigned write_total;

	/* The plain registers written so far. */
	uint32_t register_offset[I915_FAKE_REGISTERS];
	uint32_t register_value[I915_FAKE_REGISTERS];
	unsigned register_count;

	/* What the fuse status register reports. */
	uint32_t fuse_status;

	/* The PCODE mailbox and its data words. */
	uint32_t mailbox;
	uint32_t data;
	uint32_t data1;

	/* The PCODE script, the next transaction, and whether a transaction broke it. */
	const struct i915_fake_pcode_txn *pcode_script;
	unsigned pcode_script_length;
	unsigned pcode_script_next;
	int pcode_script_broken;

	/* The driver and firmware request bits of the three power-well banks. */
	uint32_t pw_hsw_driver;
	uint32_t pw_hsw_bios;
	uint32_t pw_aux_driver;
	uint32_t pw_aux_bios;
	uint32_t pw_ddi_driver;
	uint32_t pw_ddi_bios;
};

/*
 * The legacy VGA I/O recorder.
 *
 * It stands in for the port accesses of the VGA plane disable and records
 * their order: 1 get, 2 in, 3 out, 4 put.
 */
struct i915_vga_record {
	int sequence[8];
	unsigned count;
	unsigned char last_read;
	unsigned char last_written;
	int got;
	int put;
};

/*
 * The vblank fixture.
 *
 * The frame-counter read of a vblank wait also plays the hardware: it can
 * raise one pipe's vblank interrupt and run the display handler, and it
 * can make the frame counter move between reads.
 */
struct i915_vblank_fixture {
	/* The pipe whose vblank the next frame read raises; -1 for none. */
	int raise_pipe;

	/* Nonzero makes the frame counter advance on every read. */
	int frame_moves;

	/* The frame counter the reads report. */
	uint32_t frame;
};

/*
 * The register model and the register access that reaches it.
 *
 * Both are opened by each test that needs them and live for the whole run;
 * they are file-scope because the model is far too large for the stack and
 * the vblank fixture reaches them from a callback.
 */
static struct i915_fake_mmio i915_probe_fake;
static struct i915_mmio i915_probe_mmio;

/*
 * The private display of the run.
 *
 * Allocated at the start of the run and freed at its end.  Its power
 * domains, well context, PCH, interrupt half, vblank delivery and VGA
 * accessor are what the code under test reaches; the live device's display
 * is never used.  NULL outside the run.
 */
static struct i915_display *i915_probe_display;

/*
 * The interrupt device of the run.
 *
 * Its register access is the model, and its display table routes to the
 * private display's interrupt half.  It is never installed.
 */
static struct i915_irq_dev i915_probe_irq;

/* The second interrupt device, which runs the GuC arm of the GT postinstall only. */
static struct i915_irq_dev i915_probe_guc_irq;

/* The vblank fixture state, reset by each vblank test. */
static struct i915_vblank_fixture i915_probe_vblank;

/* The VGA I/O recorder, reset by each VGA test. */
static struct i915_vga_record i915_probe_vga;

/* The sideband lock the PCODE reads of the watermark test take. */
static struct mutex i915_probe_sb_lock;

/*
 * The output records the display-probe tests build.
 *
 * Each is cleared by the test that uses it; they are file-scope because
 * they are too large for the stack.
 */
static struct i915_display_nogem i915_probe_nogem;
static struct i915_display_nogem i915_probe_nogem_scratch;
static struct i915_display_nogem i915_probe_nogem_readout;

/* The VBT states the output setup reads; cleared by each test that uses them. */
static struct i915_vbt_state i915_probe_vbt;
static struct i915_vbt_state i915_probe_vbt_scratch;

static uint32_t i915_fake_read32(void *context, uint32_t offset);
static void i915_fake_write32(void *context, uint32_t offset, uint32_t value);
static void i915_fake_forcewake_request(void *context, int domain, int wake);
static int i915_fake_forcewake_ack(void *context, int domain);
static void i915_fake_record(struct i915_fake_mmio *fake, uint32_t offset, uint32_t value);
static uint32_t i915_fake_get(struct i915_fake_mmio *fake, uint32_t offset);
static void i915_fake_set(struct i915_fake_mmio *fake, uint32_t offset, uint32_t value);
static void i915_fake_pcode_post(struct i915_fake_mmio *fake, uint32_t value);
static uint32_t i915_fake_well_read(uint32_t driver, uint32_t bios);
static void i915_fake_open(void);
static int i915_fake_find(uint32_t offset, uint32_t value, uint32_t mask);
static int i915_fake_count(uint32_t offset, uint32_t value);
static void i915_probe_irq_set_enabled(void *context, int enabled);
static void i915_probe_irq_uninstall_check(void *context);
static void i915_probe_irq_reset(void *context);
static void i915_probe_irq_postinstall(void *context);
static void i915_probe_irq_handle(void *context, uint32_t master_ctl);
static void i915_probe_irq_gse(void *context);
static int i915_vga_record_get(void *context, int resource);
static unsigned char i915_vga_record_in8(void *context, unsigned short port);
static void i915_vga_record_out8(void *context, unsigned short port, unsigned char value);
static void i915_vga_record_put(void *context, int resource);
static void i915_vga_record_reset(void);
static uint32_t i915_vblank_read_frame(void *context);
static void i915_probe_wells_set(int all_on);
static struct i915_power_well *i915_probe_well_of_pipes(unsigned pipe_mask);
static int i915_probe_count_enabled_wells(void);
static void i915_probe_irq_setup(void);
static void i915_probe_irq_masks(struct i915_ktest *ktest);
static void i915_probe_pch(struct i915_ktest *ktest);
static void i915_probe_irq_reset_checks(struct i915_ktest *ktest);
static void i915_probe_irq_postinstall_checks(struct i915_ktest *ktest);
static void i915_probe_irq_guc(struct i915_ktest *ktest);
static void i915_probe_irq_poweroff(struct i915_ktest *ktest);
static void i915_probe_irq_ack(struct i915_ktest *ktest);
static void i915_probe_irq_ack_lied(struct i915_ktest *ktest);
static void i915_probe_irq_hook_post(struct i915_ktest *ktest);
static int i915_probe_vblank_enable(void);
static void i915_probe_vblank_deliver(struct i915_ktest *ktest);
static void i915_probe_vblank_wait(struct i915_ktest *ktest);
static void i915_probe_vblank_put(struct i915_ktest *ktest);
static void i915_probe_irq_hook_pre(struct i915_ktest *ktest);
static int i915_probe_hook_well(struct i915_power_well *pwa);
static void i915_probe_irq_drain_well(struct i915_ktest *ktest, struct i915_power_well *pwa);
static int i915_probe_drain_latch(struct i915_power_well *other);
static void i915_probe_irq_hook(struct i915_ktest *ktest);
static void i915_probe_irq_nodisplay(struct i915_ktest *ktest);
static void i915_probe_wm(struct i915_ktest *ktest);
static void i915_probe_wm_adjust(struct i915_ktest *ktest);
static void i915_probe_dpll(struct i915_ktest *ktest);
static void i915_probe_dpll_native(struct i915_ktest *ktest);
static void i915_probe_dpll_unused(struct i915_ktest *ktest);
static void i915_probe_dpll_tc(struct i915_ktest *ktest);
static void i915_probe_crtc(struct i915_ktest *ktest);
static void i915_probe_max_cdclk(struct i915_ktest *ktest);
static void i915_probe_wa(struct i915_ktest *ktest);
static void i915_probe_portmap(struct i915_ktest *ktest);
static void i915_probe_outputs(struct i915_ktest *ktest);
static void i915_probe_outputs_early(struct i915_ktest *ktest, unsigned port_mask);
static void i915_probe_ddi_clock(struct i915_ktest *ktest);
static void i915_probe_readout(struct i915_ktest *ktest);
static void i915_probe_readout_active(struct i915_ktest *ktest);
static void i915_probe_sanitize_quiet(struct i915_ktest *ktest);
static void i915_probe_sanitize_dpll(struct i915_ktest *ktest);
static void i915_probe_sanitize_cmtg(struct i915_ktest *ktest);
static void i915_probe_sanitize_fbc(struct i915_ktest *ktest);
static void i915_probe_sanitize_encoder_clock(struct i915_ktest *ktest);
static void i915_probe_sanitize_active(struct i915_ktest *ktest);
static void i915_probe_sanitize_well(struct i915_ktest *ktest);
static void i915_probe_vga_disable(struct i915_ktest *ktest);

/*
 * The bus end of the register model.
 *
 * Forcewake is always granted: the model has no sleeping domain.
 */
static const struct i915_mmio_ops i915_fake_mmio_ops = {
	i915_fake_read32,
	i915_fake_write32,
	i915_fake_forcewake_request,
	i915_fake_forcewake_ack
};

/*
 * The display table of the run's interrupt device.
 *
 * It routes the top-level interrupt flow to the private display's
 * interrupt half through the public display interrupt entries.  The table
 * never changes.
 */
static const struct i915_irq_display_ops i915_probe_irq_display_ops = {
	i915_probe_irq_set_enabled,
	i915_probe_irq_uninstall_check,
	i915_probe_irq_reset,
	i915_probe_irq_postinstall,
	i915_probe_irq_handle,
	i915_probe_irq_gse
};

/* The legacy VGA I/O recorder as an accessor table. */
static const struct i915_vga_io_ops i915_vga_record_ops = {
	i915_vga_record_get,
	i915_vga_record_in8,
	i915_vga_record_out8,
	i915_vga_record_put,
	NULL
};

/*
 * Runs the display-probe tests.
 *
 * Allocates the private display the tests work in, runs the PCH and
 * display interrupt tests and then the display-probe front, readout and
 * sanitize tests, and frees the display.  Nothing is written to the
 * hardware.
 */
void
drv_i915_ktest_display_probe(
	struct i915_ktest *ktest)
{
	struct i915_display *display;

	/* Allocates the private display; without it no test can run. */
	display = kern_calloc(1U, sizeof(*display));
	if (display == NULL) {
		drv_i915_ktest_skip(ktest, "display probe tests", "the private display could not be allocated");
		return;
	}

	/*
	 * Every legacy VGA access of the private display goes to the recorder,
	 * never to the I/O ports.
	 */
	i915_probe_display = display;
	drv_i915_vga_io_test_set(display, &i915_vga_record_ops);
	display->pwc.vga = &display->vga_client;

	/* Creates the sideband lock the PCODE reads take. */
	(void)mutex_init(&i915_probe_sb_lock, LOCK_RANK_DEVICE, "i915-ktest-sb");

	/* The PCH and the display interrupt half. */
	i915_probe_irq_masks(ktest);
	i915_probe_pch(ktest);
	i915_probe_irq_setup();
	i915_probe_irq_reset_checks(ktest);
	i915_probe_irq_postinstall_checks(ktest);
	i915_probe_irq_guc(ktest);
	i915_probe_irq_poweroff(ktest);
	i915_probe_irq_ack(ktest);
	i915_probe_irq_ack_lied(ktest);
	i915_probe_irq_hook(ktest);
	i915_probe_irq_nodisplay(ktest);

	/* Drops the power-well map of the interrupt tests. */
	drv_i915_power_domains_cleanup(&display->power_domains);

	/* The front of the display probe. */
	i915_probe_wm(ktest);
	i915_probe_wm_adjust(ktest);
	i915_probe_dpll(ktest);
	i915_probe_dpll_native(ktest);
	i915_probe_dpll_unused(ktest);
	i915_probe_dpll_tc(ktest);
	i915_probe_crtc(ktest);
	i915_probe_max_cdclk(ktest);
	i915_probe_wa(ktest);
	i915_probe_portmap(ktest);
	i915_probe_outputs(ktest);
	i915_probe_ddi_clock(ktest);

	/* The readout and the sanitize. */
	i915_probe_readout(ktest);

	/* The VGA plane. */
	i915_probe_vga_disable(ktest);

	/* Frees the private display. */
	i915_probe_display = NULL;
	kern_free(display);
}

/* Reads one register of the model. */
static uint32_t
i915_fake_read32(
	void *context,
	uint32_t offset)
{
	struct i915_fake_mmio *fake;
	uint32_t value;

	fake = context;

	/* Every power gate's fuses report as loaded when the test says so. */
	if (offset == I915_FAKE_FUSE_STATUS)
		return fake->fuse_status;

	/* The PCODE mailbox and its data words. */
	if (offset == I915_FAKE_PCODE_MAILBOX)
		return fake->mailbox;
	if (offset == I915_FAKE_PCODE_DATA)
		return fake->data;
	if (offset == I915_FAKE_PCODE_DATA1)
		return fake->data1;

	/* A driver request register reports the requests and the state they give. */
	if (offset == I915_FAKE_PW_HSW_DRIVER) {
		value = i915_fake_well_read(fake->pw_hsw_driver, fake->pw_hsw_bios);
		return value;
	}
	if (offset == I915_FAKE_PW_AUX_DRIVER) {
		value = i915_fake_well_read(fake->pw_aux_driver, fake->pw_aux_bios);
		return value;
	}
	if (offset == I915_FAKE_PW_DDI_DRIVER) {
		value = i915_fake_well_read(fake->pw_ddi_driver, fake->pw_ddi_bios);
		return value;
	}

	/* A firmware request register reports the firmware's requests. */
	if (offset == I915_FAKE_PW_HSW_BIOS)
		return fake->pw_hsw_bios;
	if (offset == I915_FAKE_PW_AUX_BIOS)
		return fake->pw_aux_bios;
	if (offset == I915_FAKE_PW_DDI_BIOS)
		return fake->pw_ddi_bios;

	/* Any other register reads back what was last written. */
	value = i915_fake_get(fake, offset);

	/* Succeeded: reports the plain register. */
	return value;
}

/* Writes one register of the model and records the write. */
static void
i915_fake_write32(
	void *context,
	uint32_t offset,
	uint32_t value)
{
	struct i915_fake_mmio *fake;

	fake = context;

	/* Counts and records the write in order. */
	fake->write_total++;
	i915_fake_record(fake, offset, value);

	/* The PCODE data words and the mailbox, which runs a transaction. */
	if (offset == I915_FAKE_PCODE_DATA) {
		fake->data = value;
		return;
	}
	if (offset == I915_FAKE_PCODE_DATA1) {
		fake->data1 = value;
		return;
	}
	if (offset == I915_FAKE_PCODE_MAILBOX) {
		i915_fake_pcode_post(fake, value);
		return;
	}

	/* A power-well request register keeps only its request bits. */
	if (offset == I915_FAKE_PW_HSW_DRIVER) {
		fake->pw_hsw_driver = value & I915_FAKE_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_FAKE_PW_AUX_DRIVER) {
		fake->pw_aux_driver = value & I915_FAKE_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_FAKE_PW_DDI_DRIVER) {
		fake->pw_ddi_driver = value & I915_FAKE_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_FAKE_PW_HSW_BIOS) {
		fake->pw_hsw_bios = value & I915_FAKE_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_FAKE_PW_AUX_BIOS) {
		fake->pw_aux_bios = value & I915_FAKE_PW_REQUEST_BITS;
		return;
	}
	if (offset == I915_FAKE_PW_DDI_BIOS) {
		fake->pw_ddi_bios = value & I915_FAKE_PW_REQUEST_BITS;
		return;
	}

	/* Any other register keeps the value. */
	i915_fake_set(fake, offset, value);
}

/* Accepts a forcewake request; the model has no sleeping domain. */
static void
i915_fake_forcewake_request(
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
i915_fake_forcewake_ack(
	void *context,
	int domain)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(domain);

	/* Succeeded: the domain is awake. */
	return 1;
}

/* Appends one write to the ordered record while it has room. */
static void
i915_fake_record(
	struct i915_fake_mmio *fake,
	uint32_t offset,
	uint32_t value)
{
	/* A full record keeps its first writes. */
	if (fake->write_count >= I915_FAKE_WRITE_RECORDS)
		return;

	fake->write_offset[fake->write_count] = offset;
	fake->write_value[fake->write_count] = value;
	fake->write_count++;
}

/* Reads a plain register of the model; one never written reads 0. */
static uint32_t
i915_fake_get(
	struct i915_fake_mmio *fake,
	uint32_t offset)
{
	unsigned index;

	/* Looks the register up among the ones written. */
	for (index = 0U; index < fake->register_count; index++) {
		if (fake->register_offset[index] == offset)
			return fake->register_value[index];
	}

	/* A register never written reads 0. */
	return 0U;
}

/* Stores a plain register of the model, adding it while there is room. */
static void
i915_fake_set(
	struct i915_fake_mmio *fake,
	uint32_t offset,
	uint32_t value)
{
	unsigned index;

	/* Replaces the value of a register already written. */
	for (index = 0U; index < fake->register_count; index++) {
		if (fake->register_offset[index] == offset) {
			fake->register_value[index] = value;
			return;
		}
	}

	/* A full register file drops a new register. */
	if (fake->register_count >= I915_FAKE_REGISTERS)
		return;

	fake->register_offset[fake->register_count] = offset;
	fake->register_value[fake->register_count] = value;
	fake->register_count++;
}

/* Runs one PCODE transaction posted by a mailbox write. */
static void
i915_fake_pcode_post(
	struct i915_fake_mmio *fake,
	uint32_t value)
{
	const struct i915_fake_pcode_txn *txn;
	uint32_t command;

	/* Without a script a transaction completes at once with status 0. */
	if (fake->pcode_script == NULL) {
		fake->mailbox = 0U;
		return;
	}

	/* A transaction beyond the script breaks it and completes empty. */
	if (fake->pcode_script_next >= fake->pcode_script_length) {
		fake->pcode_script_broken = 1;
		fake->mailbox = 0U;
		return;
	}

	/* Checks the command and the data word against the script. */
	txn = &fake->pcode_script[fake->pcode_script_next];
	command = value & ~I915_FAKE_PCODE_READY;
	if (txn->expected_mbox != 0xffffffffU && command != txn->expected_mbox)
		fake->pcode_script_broken = 1;
	if (txn->expected_data != 0xffffffffU && fake->data != txn->expected_data)
		fake->pcode_script_broken = 1;

	/* Loads the scripted answer and clears READY, leaving the status byte. */
	fake->pcode_script_next++;
	fake->data = txn->response_data;
	fake->data1 = txn->response_data1;
	fake->mailbox = (uint32_t)txn->status;
}

/* Reports a driver request register: its requests, and a state bit for every request the driver or the firmware holds. */
static uint32_t
i915_fake_well_read(
	uint32_t driver,
	uint32_t bios)
{
	uint32_t state;

	/* A well is on while either side requests it. */
	state = ((driver | bios) & I915_FAKE_PW_REQUEST_BITS) >> 1;

	/* Succeeded: reports the requests and the state. */
	return driver | state;
}

/* Opens the register model: every register cleared, the write record empty. */
static void
i915_fake_open(void)
{
	/* Clears the model. */
	kern_memset(&i915_probe_fake, 0, sizeof(i915_probe_fake));

	/* Points the register access at the model with no forcewake ranges. */
	drv_i915_mmio_init(&i915_probe_mmio, &i915_fake_mmio_ops, &i915_probe_fake, NULL, 0U, NULL);
}

/* Reports the order index of the first recorded write to a register whose value matches under a mask; -1 if none. */
static int
i915_fake_find(
	uint32_t offset,
	uint32_t value,
	uint32_t mask)
{
	unsigned index;

	/* Walks the record in the order of the writes. */
	for (index = 0U; index < i915_probe_fake.write_count; index++) {
		if (i915_probe_fake.write_offset[index] != offset)
			continue;
		if ((i915_probe_fake.write_value[index] & mask) != (value & mask))
			continue;

		/* Succeeded: reports the write's position. */
		return (int)index;
	}

	/* No such write was made. */
	return -1;
}

/* Counts the recorded writes of one value to one register. */
static int
i915_fake_count(
	uint32_t offset,
	uint32_t value)
{
	unsigned index;
	int count;

	/* Walks the whole record. */
	count = 0;
	for (index = 0U; index < i915_probe_fake.write_count; index++) {
		if (i915_probe_fake.write_offset[index] == offset && i915_probe_fake.write_value[index] == value)
			count++;
	}

	/* Succeeded: reports how many writes matched. */
	return count;
}

/* Tells the private display's power wells whether interrupts are enabled. */
static void
i915_probe_irq_set_enabled(
	void *context,
	int enabled)
{
	struct i915_display_irq *d;

	d = context;

	/* The power-well post-enable path is gated on this flag. */
	if (d->pwc != NULL)
		d->pwc->irqs_enabled = enabled;
}

/* Has nothing to check at uninstall: the run's interrupt device is never installed. */
static void
i915_probe_irq_uninstall_check(
	void *context)
{
	UNUSED_PARAMETER(context);
}

/* Resets the private display's interrupt sources for the interrupt device. */
static void
i915_probe_irq_reset(
	void *context)
{
	/* The context is the private display's interrupt half. */
	drv_i915_gen11_display_irq_reset(context);
}

/* Enables the private display's interrupt sources for the interrupt device. */
static void
i915_probe_irq_postinstall(
	void *context)
{
	/* The context is the private display's interrupt half. */
	drv_i915_gen11_de_irq_postinstall(context);
}

/* Serves the private display's interrupt sources for the interrupt device. */
static void
i915_probe_irq_handle(
	void *context,
	uint32_t master_ctl)
{
	UNUSED_PARAMETER(master_ctl);

	/* The display summary names the pending blocks. */
	drv_i915_gen11_display_irq_handler(context);
}

/* Drops a graphics system event: the private display has no OpRegion. */
static void
i915_probe_irq_gse(
	void *context)
{
	UNUSED_PARAMETER(context);
}

/* Records a legacy I/O resource get and grants it. */
static int
i915_vga_record_get(
	void *context,
	int resource)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(resource);

	/* Records the step while the record has room. */
	if (i915_probe_vga.count < 8U)
		i915_probe_vga.sequence[i915_probe_vga.count++] = 1;
	i915_probe_vga.got = 1;

	/* Succeeded: the resource is owned. */
	return 1;
}

/* Records a legacy port read; the Misc Output read port reports 0xAB. */
static unsigned char
i915_vga_record_in8(
	void *context,
	unsigned short port)
{
	UNUSED_PARAMETER(context);

	/* Records the step while the record has room. */
	if (i915_probe_vga.count < 8U)
		i915_probe_vga.sequence[i915_probe_vga.count++] = 2;

	/* Only the Misc Output read port has a recognizable value. */
	i915_probe_vga.last_read = 0U;
	if (port == 0x3CCU)
		i915_probe_vga.last_read = 0xABU;

	/* Succeeded: reports the port value. */
	return i915_probe_vga.last_read;
}

/* Records a legacy port write and its value. */
static void
i915_vga_record_out8(
	void *context,
	unsigned short port,
	unsigned char value)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(port);

	/* Records the step while the record has room. */
	if (i915_probe_vga.count < 8U)
		i915_probe_vga.sequence[i915_probe_vga.count++] = 3;
	i915_probe_vga.last_written = value;
}

/* Records a legacy I/O resource put. */
static void
i915_vga_record_put(
	void *context,
	int resource)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(resource);

	/* Records the step while the record has room. */
	if (i915_probe_vga.count < 8U)
		i915_probe_vga.sequence[i915_probe_vga.count++] = 4;
	i915_probe_vga.put = 1;
}

/* Empties the VGA I/O record. */
static void
i915_vga_record_reset(void)
{
	/* Starts the record over. */
	kern_memset(&i915_probe_vga, 0, sizeof(i915_probe_vga));
}

/* Reads the fixture's frame counter, first raising the requested pipe's vblank through the display handler. */
static uint32_t
i915_vblank_read_frame(
	void *context)
{
	unsigned pipe;

	UNUSED_PARAMETER(context);

	/* Plays the hardware: the pipe's summary bit and its VBLANK IIR bit, then the handler. */
	if (i915_probe_vblank.raise_pipe >= 0) {
		pipe = (unsigned)i915_probe_vblank.raise_pipe;
		drv_i915_raw_write32(&i915_probe_mmio, 0x44200U, 1U << (16U + pipe));
		drv_i915_raw_write32(&i915_probe_mmio, 0x44408U + 0x10U * pipe, 1U);
		drv_i915_gen11_display_irq_handler(&i915_probe_display->irq);
		i915_probe_vblank.raise_pipe = -1;
	}

	/* Advances the frame counter when the test asks for a moving one. */
	if (i915_probe_vblank.frame_moves)
		i915_probe_vblank.frame++;

	/* Succeeded: reports the frame counter. */
	return i915_probe_vblank.frame;
}

/* Marks every well of the private display on, or only the always-on ones. */
static void
i915_probe_wells_set(
	int all_on)
{
	struct i915_power_domains *pd;
	unsigned index;

	pd = &i915_probe_display->power_domains;

	/* Sets the cached hardware state the pipe and transcoder gates read. */
	for (index = 0U; index < pd->num_power_wells; index++) {
		if (all_on || pd->power_wells[index].always_on)
			pd->power_wells[index].hw_enabled = 1;
		else
			pd->power_wells[index].hw_enabled = 0;
	}
}

/* Finds the last well of the private display whose interrupt pipe mask is exactly the one given; NULL if none. */
static struct i915_power_well *
i915_probe_well_of_pipes(
	unsigned pipe_mask)
{
	struct i915_power_domains *pd;
	struct i915_power_well *found;
	unsigned index;

	pd = &i915_probe_display->power_domains;

	/* Walks every well, keeping the last match. */
	found = NULL;
	for (index = 0U; index < pd->num_power_wells; index++) {
		if (pd->power_wells[index].irq_pipe_mask == pipe_mask)
			found = &pd->power_wells[index];
	}

	/* Succeeded: reports the well, or NULL. */
	return found;
}

/* Counts the wells of the private display that read back enabled now. */
static int
i915_probe_count_enabled_wells(void)
{
	struct i915_power_domains *pd;
	unsigned index;
	int enabled;
	int count;

	pd = &i915_probe_display->power_domains;

	/* Asks each well with a fresh read. */
	count = 0;
	for (index = 0U; index < pd->num_power_wells; index++) {
		enabled = drv_i915_power_well_is_enabled(&pd->power_wells[index], &i915_probe_display->pwc);
		if (enabled)
			count++;
	}

	/* Succeeded: reports how many are on. */
	return count;
}

/*
 * Prepares the private display's power wells and interrupt half, and the
 * run's interrupt device, as the ADL-P display interrupt tests expect them.
 */
static void
i915_probe_irq_setup(void)
{
	struct i915_display *display;
	struct i915_display_irq *d;

	display = i915_probe_display;
	d = &display->irq;

	/* Opens the register model with every power gate's fuses loaded. */
	i915_fake_open();
	i915_probe_fake.fuse_status = 0xFFFFFFFFU;

	/* Builds the ADL-P power-well map of the private display. */
	drv_i915_trace_init(&display->dc_trace);
	(void)drv_i915_power_domains_init(&display->power_domains, 13U, -1, 1, &display->dc_trace);

	/* The well context reaches the model; the interrupt hooks are not bound yet. */
	display->pwc.mmio = &i915_probe_mmio;

	/*
	 * Marks every well as enabled so the pipe and transcoder gates let the
	 * reset and the postinstall through (they use the cached state).
	 */
	i915_probe_wells_set(1);

	/* An Alder Lake PCH. */
	kern_memset(&display->pch, 0, sizeof(display->pch));
	display->pch.type = I915_PCH_ADP;

	/* The interrupt device: the model, execlists, and the private display's interrupt half. */
	kern_memset(&i915_probe_irq, 0, sizeof(i915_probe_irq));
	i915_probe_irq.m = &i915_probe_mmio;
	i915_probe_irq.submission = I915_SUBMISSION_EXECLISTS;
	i915_probe_irq.display_ops = &i915_probe_irq_display_ops;
	i915_probe_irq.display_context = d;

	/* The display interrupt half: ADL-P with pipes and transcoders A to D and a display. */
	kern_memset(d, 0, sizeof(*d));
	d->irq = &i915_probe_irq;
	d->m = &i915_probe_mmio;
	d->pd = &display->power_domains;
	d->pwc = &display->pwc;
	d->pch = &display->pch;
	d->display_ver = 13;
	d->pipe_mask = 0xfU;
	d->cpu_transcoder_mask = 0xfU;
	d->has_display = 1;
}

/* IRQ-MASKS: the ADL-P mask helpers resolve to the reference values. */
static void
i915_probe_irq_masks(
	struct i915_ktest *ktest)
{
	uint32_t fault13;
	uint32_t fault11;
	uint32_t fault9;
	uint32_t fault8;
	uint32_t aux13;
	uint32_t underrun13;
	uint32_t underrun12;
	uint32_t flip13;

	/* Asks every helper for its neighbours of ADL-P too. */
	fault13 = drv_i915_gen8_de_pipe_fault_mask(13);
	fault11 = drv_i915_gen8_de_pipe_fault_mask(11);
	fault9 = drv_i915_gen8_de_pipe_fault_mask(9);
	fault8 = drv_i915_gen8_de_pipe_fault_mask(8);
	aux13 = drv_i915_gen8_de_port_aux_mask(13);
	underrun13 = drv_i915_gen8_de_pipe_underrun_mask(13);
	underrun12 = drv_i915_gen8_de_pipe_underrun_mask(12);
	flip13 = drv_i915_gen8_de_pipe_flip_done_mask(13);

	/* The RKL fault set on 13, the GEN11 one on 11. */
	drv_i915_ktest_check(ktest,
	    fault13 == 0x00100F80U &&
	    fault11 == 0x00700F80U &&
	    fault9 == 0x00000F80U &&
	    fault8 == 0x00000700U &&
	    aux13 == 0x00003F07U &&
	    underrun13 == 0x80600000U &&
	    underrun12 == 0x80000000U &&
	    flip13 == 0x00000008U,
	    "p4: IRQ-MASKS gen8_de_* helpers match the reference for ADL-P (and neighbours)");
}

/*
 * PCH: the id table and the virtual-bridge predicate.
 *
 * The scripted ISA-bridge walk the bridge-scan paths (virtual, real,
 * absent and NOP) were tested with no longer exists: the detection always
 * scans the real PCI bus, so those paths are not tested here.
 */
static void
i915_probe_pch(
	struct i915_ktest *ktest)
{
	int icp;
	int jsp;
	int tgp;
	int mcc;
	int ppt;
	int cmp_v;
	int none;
	int virt_p3x;
	int virt_qemu;
	int virt_wrong;
	int ok;

	/* Looks up every id family the table folds together. */
	icp = drv_i915_pch_type(0x3480U);
	jsp = drv_i915_pch_type(0x4D80U);
	tgp = drv_i915_pch_type(0xA080U);
	mcc = drv_i915_pch_type(0x4B00U);
	ppt = drv_i915_pch_type(0x1e00U);
	cmp_v = drv_i915_pch_type(0xA380U);
	none = drv_i915_pch_type(0x0000U);

	/* Asks the virtual-bridge predicate about a P3X, QEMU's bridge, and QEMU's id with the wrong subsystem. */
	virt_p3x = drv_i915_is_virt_pch(0x7100U, 0U, 0U);
	virt_qemu = drv_i915_is_virt_pch(0x2900U, I915_PROBE_PCI_VENDOR_QUMRANET, I915_PROBE_PCI_SUBDEVICE_QEMU);
	virt_wrong = drv_i915_is_virt_pch(0x2900U, 0x8086U, I915_PROBE_PCI_SUBDEVICE_QEMU);

	/* JSP is ICP, MCC is TGP, PPT is CPT, CMP-V is SPT. */
	ok = 1;
	if (icp != I915_PCH_ICP || jsp != I915_PCH_ICP)
		ok = 0;
	if (tgp != I915_PCH_TGP || mcc != I915_PCH_TGP)
		ok = 0;
	if (ppt != I915_PCH_CPT || cmp_v != I915_PCH_SPT)
		ok = 0;
	if (none != I915_PCH_NONE)
		ok = 0;
	if (virt_p3x != 1 ||
	    virt_qemu != 1 ||
	    virt_wrong != 0)
		ok = 0;

	drv_i915_ktest_check(ktest, ok == 1, "p4: PCH the id table and virt predicate");
}

/* IRQ-RESET: gen11_irq_reset masks and clears every block. */
static void
i915_probe_irq_reset_checks(
	struct i915_ktest *ktest)
{
	int master;
	int render_copy;
	int rcs0;
	int vecs;
	int display_ctl;
	int psr_imr_a;
	int psr_iir_d;
	int pipe_imr_a;
	int pipe_imr_d;
	int sde_imr;
	int gu_misc;
	int pcu;
	int iir_writes;

	/* Runs the reset through the interrupt device, which reaches the display half. */
	i915_probe_fake.write_count = 0U;
	drv_i915_irq_reset(&i915_probe_irq);

	/* Finds the writes the reset must have made. */
	master = i915_fake_find(0x190010U, 0U, 0xffffffffU);
	render_copy = i915_fake_find(0x190030U, 0U, 0xffffffffU);
	rcs0 = i915_fake_find(0x190090U, 0xffffffffU, 0xffffffffU);
	vecs = i915_fake_find(0x1900d0U, 0xffffffffU, 0xffffffffU);
	display_ctl = i915_fake_find(0x44200U, 0U, 0xffffffffU);
	psr_imr_a = i915_fake_find(0x60814U, 0xffffffffU, 0xffffffffU);
	psr_iir_d = i915_fake_find(0x63818U, 0xffffffffU, 0xffffffffU);
	pipe_imr_a = i915_fake_find(0x44404U, 0xffffffffU, 0xffffffffU);
	pipe_imr_d = i915_fake_find(0x44434U, 0xffffffffU, 0xffffffffU);
	sde_imr = i915_fake_find(0xc4004U, 0xffffffffU, 0xffffffffU);
	gu_misc = i915_fake_find(0x444f4U, 0xffffffffU, 0xffffffffU);
	pcu = i915_fake_find(0x444e4U, 0xffffffffU, 0xffffffffU);

	/* The master control first, then RENDER_COPY, RCS0, VECS0_VECS1, the display, SDE (>= ICP), GU_MISC and PCU. */
	drv_i915_ktest_check(ktest,
	    master == 0 &&
	    render_copy > 0 &&
	    rcs0 > 0 &&
	    vecs > 0 &&
	    display_ctl > 0 &&
	    psr_imr_a > 0 &&
	    psr_iir_d > 0 &&
	    pipe_imr_a > 0 &&
	    pipe_imr_d > 0 &&
	    sde_imr > 0 &&
	    gu_misc > 0 &&
	    pcu > 0,
	    "p4: IRQ-RESET master disabled first, then GT/display/GU_MISC/PCU all masked");

	/* gen3_irq_reset writes IIR twice (the reference is deliberately paranoid). */
	iir_writes = i915_fake_count(0x44408U, 0xffffffffU);
	drv_i915_ktest_check(ktest, iir_writes == 2, "p4: IRQ-RESET gen3_irq_reset clears each IIR twice");
}

/* IRQ-POST: gen11_irq_postinstall enables the right sources. */
static void
i915_probe_irq_postinstall_checks(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	int render_copy;
	int rcs0;
	int master;
	int pipe_ier_a;
	int sde_ier;
	int hpd_ier;
	int display_ctl;

	d = &i915_probe_display->irq;

	/* Runs the postinstall through the interrupt device. */
	i915_probe_fake.write_count = 0U;
	drv_i915_irq_postinstall(&i915_probe_irq);

	/* The RENDER_COPY enable, the RCS0 mask and the master enable. */
	render_copy = i915_fake_find(0x190030U, 0x09090909U, 0xffffffffU);
	rcs0 = i915_fake_find(0x190090U, ~0x09090000U, 0xffffffffU);
	master = i915_fake_find(0x190010U, 0x80000000U, 0xffffffffU);

	/* The execlists arm, with the master enabled last. */
	drv_i915_ktest_check(ktest,
	    i915_probe_irq.gt_irqs == 0x909U &&
	    i915_probe_irq.gt_dmask == 0x09090909U &&
	    i915_probe_irq.gt_smask == 0x09090000U &&
	    render_copy >= 0 &&
	    rcs0 >= 0 &&
	    master >= 0 &&
	    master > render_copy &&
	    master > rcs0 &&
	    i915_probe_irq.reached_master_enable == 1,
	    "p4: IRQ-POST execlists irqs=0x909, dmask/smask written, master enabled LAST");

	/* The pipe A enables, the SDE enables (ICP), the Type-C hotplug enables and the display summary. */
	pipe_ier_a = i915_fake_find(0x4440cU, 0x90700F89U, 0xffffffffU);
	sde_ier = i915_fake_find(0xc400cU, 0xffffffffU, 0xffffffffU);
	hpd_ier = i915_fake_find(0x4447cU, 0x003F003FU, 0xffffffffU);
	display_ctl = i915_fake_find(0x44200U, 0x80000000U, 0xffffffffU);

	/* The misc mask is EDP_PSR only on display 11 and later. */
	drv_i915_ktest_check(ktest,
	    d->de_pipe_masked == 0x10100F80U &&
	    d->de_pipe_enables == 0x90700F89U &&
	    d->de_port_masked == 0x00003F07U &&
	    d->de_misc_masked == 0x00080000U &&
	    d->de_irq_mask[0] == ~0x10100F80U &&
	    pipe_ier_a >= 0 &&
	    sde_ier >= 0 &&
	    hpd_ier >= 0 &&
	    display_ctl >= 0,
	    "p4: IRQ-POST ADL-P DE masks + per-pipe IER + icp SDE + TC/TBT hotplug");
}

/* IRQ-GUC: the GuC-submission arm drops the three command streamer interrupts. */
static void
i915_probe_irq_guc(
	struct i915_ktest *ktest)
{
	/* A GT-only interrupt device on the model, with GuC submission. */
	kern_memset(&i915_probe_guc_irq, 0, sizeof(i915_probe_guc_irq));
	i915_probe_guc_irq.m = &i915_probe_mmio;
	i915_probe_guc_irq.submission = I915_SUBMISSION_GUC;

	/* Runs the GT postinstall only. */
	drv_i915_gen11_gt_irq_postinstall(&i915_probe_guc_irq);

	/* Only the user interrupt remains. */
	drv_i915_ktest_check(ktest,
	    i915_probe_guc_irq.gt_irqs == 0x1U && i915_probe_guc_irq.gt_dmask == 0x00010001U,
	    "p4: IRQ-GUC guc-submission leaves out CS_MASTER_ERROR/CTX_SWITCH/SEMAPHORE");
}

/*
 * IRQ-POWEROFF: pipes and transcoders whose power is off are skipped.
 *
 * PIPE_A belongs to PW_A, which is off, so pipe A is skipped.
 * TRANSCODER_A is claimed by no dedicated well; only the always-on well
 * covers it, and the reference skips always-on wells when deciding, so the
 * domain still counts as enabled and the PSR registers are reset.  Blocks
 * with no power gate at all (DE_PORT) are always reset.
 */
static void
i915_probe_irq_poweroff(
	struct i915_ktest *ktest)
{
	int display_ctl;
	int pipe_a;
	int pipe_d;
	int transcoder_a;
	int transcoder_b;
	int port;

	/* Leaves only the always-on wells on and resets the display half. */
	i915_probe_wells_set(0);
	i915_probe_fake.write_count = 0U;
	drv_i915_gen11_display_irq_reset(&i915_probe_display->irq);

	/* Finds the writes that must, and must not, have been made. */
	display_ctl = i915_fake_find(0x44200U, 0U, 0xffffffffU);
	pipe_a = i915_fake_find(0x44404U, 0xffffffffU, 0xffffffffU);
	pipe_d = i915_fake_find(0x44434U, 0xffffffffU, 0xffffffffU);
	transcoder_a = i915_fake_find(0x60814U, 0xffffffffU, 0xffffffffU);
	transcoder_b = i915_fake_find(0x61814U, 0xffffffffU, 0xffffffffU);
	port = i915_fake_find(0x44444U, 0xffffffffU, 0xffffffffU);

	/* The summary still goes off; pipes A and D and transcoder B are skipped; transcoder A and DE_PORT are reset. */
	drv_i915_ktest_check(ktest,
	    display_ctl >= 0 &&
	    pipe_a < 0 &&
	    pipe_d < 0 &&
	    transcoder_a >= 0 &&
	    transcoder_b < 0 &&
	    port >= 0,
	    "p4: IRQ-POWEROFF unpowered pipes skipped; always-on-only domains still processed");

	/* Every well is on again for the tests that follow. */
	i915_probe_wells_set(1);
}

/*
 * IRQ-ACK: every asserted display source is read and acknowledged.
 *
 * MISC, HPD, PORT, PIPE_A, PIPE_B and PCH are asserted in the display
 * summary and each block gets a non-zero IIR.  Pipe A carries vblank, flip
 * done, underrun and a fault bit so each decode arm runs at once.
 */
static void
i915_probe_irq_ack(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	uint32_t display_ctl;
	int misc;
	int hpd;
	int port;
	int pipe_a;
	int pipe_b;
	int pch;
	int gated_off;
	int enabled;

	d = &i915_probe_display->irq;

	/* Asserts the six blocks and their IIRs. */
	display_ctl = (1U << 22) | (1U << 21) | (1U << 20) | (1U << 16) | (1U << 17) | (1U << 23);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44200U, display_ctl);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44468U, 0x00080000U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44478U, 0x00000001U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44448U, 0x00000001U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44408U, 0x80000009U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44418U, 0x00000080U);
	drv_i915_raw_write32(&i915_probe_mmio, 0xc4008U, 0x00800000U);

	/* Runs the display handler. */
	i915_probe_fake.write_count = 0U;
	drv_i915_gen11_display_irq_handler(d);

	/* Each source counted once, decoded into vblank, flip done, underrun and fault. */
	drv_i915_ktest_check(ktest,
	    d->de_misc_acks == 1U &&
	    d->de_hpd_acks == 1U &&
	    d->de_port_acks == 1U &&
	    d->de_pch_acks == 1U &&
	    d->de_pipe_iir_acks[0] == 1U &&
	    d->de_pipe_iir_acks[1] == 1U &&
	    d->de_vblank_count[0] == 1U &&
	    d->de_flip_done_count == 1U &&
	    d->de_underrun_count == 1U &&
	    d->de_fault_count == 1U &&
	    d->de_lied_count == 0U &&
	    d->last_disp_ctl == display_ctl,
	    "p4: IRQ-ACK each asserted display source is read, acked and decoded");

	/* Each acknowledge wrote back the IIR value it read. */
	misc = i915_fake_find(0x44468U, 0x00080000U, 0xffffffffU);
	hpd = i915_fake_find(0x44478U, 0x00000001U, 0xffffffffU);
	port = i915_fake_find(0x44448U, 0x00000001U, 0xffffffffU);
	pipe_a = i915_fake_find(0x44408U, 0x80000009U, 0xffffffffU);
	pipe_b = i915_fake_find(0x44418U, 0x00000080U, 0xffffffffU);
	pch = i915_fake_find(0xc4008U, 0x00800000U, 0xffffffffU);
	drv_i915_ktest_check(ktest,
	    misc >= 0 &&
	    hpd >= 0 &&
	    port >= 0 &&
	    pipe_a >= 0 &&
	    pipe_b >= 0 &&
	    pch >= 0,
	    "p4: IRQ-ACK the ack is a write-back of the IIR value just read");

	/* The display block is gated off across the read and acknowledge and re-enabled. */
	gated_off = i915_fake_find(0x44200U, 0U, 0xffffffffU);
	enabled = i915_fake_find(0x44200U, 0x80000000U, 0xffffffffU);
	drv_i915_ktest_check(ktest,
	    gated_off == 0 && enabled > 0,
	    "p4: IRQ-ACK DISPLAY_INT_CTL is gated off first and re-enabled last");
}

/* IRQ-ACK: a summary bit with an empty IIR, and a source not named in the summary. */
static void
i915_probe_irq_ack_lied(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	int port;

	d = &i915_probe_display->irq;

	/* A master bit set with a zero IIR is the reference's "lied" case. */
	d->de_lied_count = 0U;
	d->de_misc_acks = 0U;
	drv_i915_raw_write32(&i915_probe_mmio, 0x44200U, 1U << 22);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44468U, 0U);
	drv_i915_gen11_display_irq_handler(d);
	drv_i915_ktest_check(ktest,
	    d->de_lied_count == 1U && d->de_misc_acks == 0U,
	    "p4: IRQ-ACK a master bit with a zero IIR is counted as lied, not acked");

	/* A source not asserted in the summary is never touched; only the handler is recorded. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x44200U, 0U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44448U, 0x00000001U);
	i915_probe_fake.write_count = 0U;
	drv_i915_gen11_display_irq_handler(d);
	port = i915_fake_find(0x44448U, 0x00000001U, 0xffffffffU);
	drv_i915_ktest_check(ktest, port < 0, "p4: IRQ-ACK an unasserted source is not read or acked");
}

/*
 * IRQ-HOOK-POST and IRQ-HOOK-OFF: the power-well post-enable hook restores
 * a pipe's interrupts only while interrupts are enabled.
 */
static void
i915_probe_irq_hook_post(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	struct i915_irq_vblank *v;
	uint32_t extra;
	uint32_t ier;
	int iir_clear;
	int ier_write;
	int imr_write;
	int pipe_b;
	int get_refused;

	d = &i915_probe_display->irq;
	v = &i915_probe_display->irq_vblank;

	/* VBLANK, the XE_LPD underrun mask and flip done. */
	extra = 0x00000001U | 0x80600000U | 0x00000008U;

	/* Binds the vblank delivery and enables interrupts with the postinstall's pipe mask. */
	drv_i915_irq_vblank_init(d, v);
	i915_probe_irq.irqs_enabled = 1;
	d->de_irq_mask[0] = ~d->de_pipe_masked;

	/* A stale IIR, then the post-enable of pipe A. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x44408U, 0x00000001U);
	i915_probe_fake.write_count = 0U;
	drv_i915_gen8_irq_power_well_post_enable(d, 1U << 0);

	/* The stale IIR cleared, then IER, then IMR; pipe B untouched. */
	ier = ~d->de_irq_mask[0] | extra;
	iir_clear = i915_fake_find(0x44408U, 0xffffffffU, 0xffffffffU);
	ier_write = i915_fake_find(0x4440cU, ier, 0xffffffffU);
	imr_write = i915_fake_find(0x44404U, d->de_irq_mask[0], 0xffffffffU);
	pipe_b = i915_fake_find(0x44414U, 0U, 0U);
	drv_i915_ktest_check(ktest,
	    v->post_enable_calls == 1U &&
	    v->post_imr[0] == d->de_irq_mask[0] &&
	    v->post_ier[0] == ier &&
	    iir_clear >= 0 &&
	    ier_write > iir_clear &&
	    imr_write > ier_write &&
	    pipe_b < 0,
	    "irq: IRQ-HOOK-POST pipe A only: stale IIR cleared, IER = ~de_irq_mask | vblank | underrun | flip done, then IMR = de_irq_mask");
	drv_i915_ktest_check(ktest,
	    (v->post_imr[0] & 1U) == 1U,
	    "irq: IRQ-HOOK-POST the restored mask keeps vblank MASKED (IER enabling it is not delivery)");

	/* With interrupts not enabled the hook writes nothing (intel_irqs_enabled gate). */
	i915_probe_irq.irqs_enabled = 0;
	i915_probe_fake.write_count = 0U;
	drv_i915_gen8_irq_power_well_post_enable(d, 1U << 0);
	drv_i915_ktest_check(ktest,
	    v->post_enable_calls == 1U &&
	    v->skipped_irqs_disabled == 1U &&
	    i915_probe_fake.write_count == 0U,
	    "irq: IRQ-HOOK-OFF with interrupts not enabled the hook writes nothing (intel_irqs_enabled gate)");

	/* A vblank reference is refused while interrupts are not enabled. */
	get_refused = drv_i915_drm_vblank_get(d, 0U);
	drv_i915_ktest_check(ktest,
	    get_refused == EINVAL && v->refs[0] == 0U,
	    "irq: VBL-GET refused while interrupts are not enabled");
	i915_probe_irq.irqs_enabled = 1;
}

/*
 * VBL-ENABLE: the first reference unmasks vblank in IMR, the second only
 * counts.  Reports whether every step held, stopping at the first that
 * did not.
 */
static int
i915_probe_vblank_enable(void)
{
	struct i915_display_irq *d;
	struct i915_irq_vblank *v;
	int got;
	int imr_write;

	d = &i915_probe_display->irq;
	v = &i915_probe_display->irq_vblank;

	/* The first reference enables the delivery and unmasks the vblank bit. */
	i915_probe_fake.write_count = 0U;
	got = drv_i915_drm_vblank_get(d, 0U);
	if (got != 0)
		return 0;
	if (v->refs[0] != 1U || v->enabled[0] != 1)
		return 0;
	if ((d->de_irq_mask[0] & 1U) != 0U)
		return 0;

	/* The IMR was written with the new mask. */
	imr_write = i915_fake_find(0x44404U, d->de_irq_mask[0], 0xffffffffU);
	if (imr_write < 0)
		return 0;

	/* The second reference only counts. */
	got = drv_i915_drm_vblank_get(d, 0U);
	if (got != 0)
		return 0;
	if (v->enable_calls[0] != 1U)
		return 0;

	/* Succeeded: both references behaved. */
	return 1;
}

/* VBL-DELIVER: a vblank of an enabled pipe reaches its count; another pipe's does not. */
static void
i915_probe_vblank_deliver(
	struct i915_ktest *ktest)
{
	struct i915_irq_vblank *v;

	v = &i915_probe_display->irq_vblank;

	/* A pipe A vblank through the real display handler. */
	i915_probe_vblank.frame = 100U;
	i915_probe_vblank.raise_pipe = 0;
	i915_probe_vblank.frame_moves = 0;
	(void)i915_vblank_read_frame(NULL);
	drv_i915_ktest_check(ktest, v->count[0] == 1U, "irq: VBL-DELIVER a pipe A vblank interrupt reaches the pipe's vblank count");

	/* A pipe B vblank, whose delivery is not enabled. */
	i915_probe_vblank.raise_pipe = 1;
	(void)i915_vblank_read_frame(NULL);
	drv_i915_ktest_check(ktest,
	    v->count[0] == 1U && v->count[1] == 0U,
	    "irq: VBL-DELIVER pipe B's vblank (not enabled there) counts nowhere");
}

/* VBL-WAIT: a wait needs a new vblank of its pipe and a moving frame counter. */
static void
i915_probe_vblank_wait(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	uint32_t seen;
	int waited;

	d = &i915_probe_display->irq;
	seen = 0U;

	/* A new pipe A vblank and a moving frame counter. */
	i915_probe_vblank.raise_pipe = 0;
	i915_probe_vblank.frame_moves = 1;
	waited = drv_i915_wait_vblank(d, 0U, 1U, 50U, i915_vblank_read_frame, NULL, &seen);
	drv_i915_ktest_check(ktest,
	    waited == 0 && seen == 1U,
	    "irq: VBL-WAIT a new pipe A vblank + a moving frame counter completes the wait");

	/* Another pipe's vblank. */
	i915_probe_vblank.raise_pipe = 1;
	i915_probe_vblank.frame_moves = 1;
	waited = drv_i915_wait_vblank(d, 0U, 1U, 50U, i915_vblank_read_frame, NULL, &seen);
	drv_i915_ktest_check(ktest,
	    waited == ETIMEDOUT && seen == 0U,
	    "irq: VBL-WAIT another pipe's vblank does not complete it (timeout)");

	/* An interrupt without the frame counter moving (a stale pending bit). */
	i915_probe_vblank.raise_pipe = 0;
	i915_probe_vblank.frame_moves = 0;
	waited = drv_i915_wait_vblank(d, 0U, 1U, 50U, i915_vblank_read_frame, NULL, &seen);
	drv_i915_ktest_check(ktest,
	    waited == ETIMEDOUT && seen == 1U,
	    "irq: VBL-WAIT an interrupt without the frame counter moving (stale pending bit) does not");

	/* A moving counter and elapsed time alone. */
	i915_probe_vblank.raise_pipe = -1;
	i915_probe_vblank.frame_moves = 1;
	waited = drv_i915_wait_vblank(d, 0U, 1U, 50U, i915_vblank_read_frame, NULL, &seen);
	drv_i915_ktest_check(ktest,
	    waited == ETIMEDOUT && seen == 0U,
	    "irq: VBL-WAIT a moving counter / elapsed time alone does not");
}

/* VBL-PUT, VBL-OFF and VBL-WAIT without a reference. */
static void
i915_probe_vblank_put(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	struct i915_irq_vblank *v;
	uint32_t count_before;
	uint32_t seen;
	unsigned acks_before;
	int imr_write;
	int waited;

	d = &i915_probe_display->irq;
	v = &i915_probe_display->irq_vblank;
	seen = 0U;

	/* The non-last put keeps vblank enabled. */
	drv_i915_drm_vblank_put(d, 0U);
	drv_i915_ktest_check(ktest,
	    v->refs[0] == 1U && (d->de_irq_mask[0] & 1U) == 0U,
	    "irq: VBL-PUT the non-last put keeps vblank enabled");

	/* The last put masks vblank at once. */
	i915_probe_fake.write_count = 0U;
	drv_i915_drm_vblank_put(d, 0U);
	imr_write = i915_fake_find(0x44404U, d->de_irq_mask[0], 0xffffffffU);
	drv_i915_ktest_check(ktest,
	    v->refs[0] == 0U &&
	    v->enabled[0] == 0 &&
	    (d->de_irq_mask[0] & 1U) == 1U &&
	    imr_write >= 0 &&
	    v->disable_calls[0] == 1U,
	    "irq: VBL-PUT the last put masks vblank at once (vblank_disable_immediate)");

	/* A vblank arriving after the last put is acknowledged but not counted. */
	count_before = v->count[0];
	acks_before = d->de_pipe_iir_acks[0];
	i915_probe_vblank.raise_pipe = 0;
	(void)i915_vblank_read_frame(NULL);
	drv_i915_ktest_check(ktest,
	    v->count[0] == count_before && d->de_pipe_iir_acks[0] == acks_before + 1U,
	    "irq: VBL-OFF a vblank arriving after the last put is acked but not counted");

	/* A wait without a reference is refused, not waited out. */
	waited = drv_i915_wait_vblank(d, 0U, 1U, 10U, i915_vblank_read_frame, NULL, &seen);
	drv_i915_ktest_check(ktest, waited == EINVAL, "irq: VBL-WAIT without a reference is refused, not waited out");
}

/* IRQ-HOOK-PRE and IRQ-SYNC: the pre-disable hook and the bounded handler synchronisation. */
static void
i915_probe_irq_hook_pre(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	struct i915_irq_vblank *v;
	int imr_mask;
	int ier_off;
	int iir_clear;
	int synced;

	d = &i915_probe_display->irq;
	v = &i915_probe_display->irq_vblank;

	/* Stops pipe A's interrupts: sources reset, then the handler synchronisation. */
	i915_probe_fake.write_count = 0U;
	(void)drv_i915_gen8_irq_power_well_pre_disable(d, 1U << 0);
	imr_mask = i915_fake_find(0x44404U, 0xffffffffU, 0xffffffffU);
	ier_off = i915_fake_find(0x4440cU, 0U, 0xffffffffU);
	iir_clear = i915_fake_find(0x44408U, 0xffffffffU, 0xffffffffU);
	drv_i915_ktest_check(ktest,
	    v->pre_disable_calls == 1U &&
	    v->sync_calls == 1U &&
	    v->sync_timeouts == 0U &&
	    imr_mask == 0 &&
	    ier_off > 0 &&
	    iir_clear > 0,
	    "irq: IRQ-HOOK-PRE pipe A: IMR all masked, IER 0, IIR cleared, then the handler synchronisation");

	/* One invocation still inside the handler: the synchronisation is bounded and reports it. */
	i915_probe_irq.handler_entries = 5U;
	i915_probe_irq.handler_exits = 4U;
	synced = drv_i915_synchronize_irq(&i915_probe_irq);
	drv_i915_ktest_check(ktest,
	    synced == ETIMEDOUT && i915_probe_irq.sync_timeouts == 1U,
	    "irq: IRQ-SYNC a handler that never finishes is reported (bounded), not assumed done");

	/* The exits reach the entries it saw. */
	i915_probe_irq.handler_exits = 5U;
	synced = drv_i915_synchronize_irq(&i915_probe_irq);
	drv_i915_ktest_check(ktest, synced == 0, "irq: IRQ-SYNC returns once the exits reach the entries it saw");
	i915_probe_irq.handler_entries = 0U;
	i915_probe_irq.handler_exits = 0U;
}

/*
 * IRQ-HOOK-WELL: PW_A's enable restores and its disable stops pipe A's
 * interrupts through the bound hooks.  Reports whether every step held,
 * stopping at the first that did not.
 */
static int
i915_probe_hook_well(
	struct i915_power_well *pwa)
{
	struct i915_irq_vblank *v;
	struct i915_pw_ctx *pwc;
	unsigned before_post;
	unsigned before_pre;
	int result;

	v = &i915_probe_display->irq_vblank;
	pwc = &i915_probe_display->pwc;
	before_post = v->post_enable_calls;
	before_pre = v->pre_disable_calls;

	/* The ADL-P map has a well for pipe A. */
	if (pwa == NULL)
		return 0;

	/* The enable runs the post-enable hook. */
	result = drv_i915_power_well_enable(pwa, pwc);
	if (result != 0)
		return 0;
	if (v->post_enable_calls != before_post + 1U)
		return 0;

	/* The disable runs the pre-disable hook. */
	result = drv_i915_power_well_disable(pwa, pwc);
	if (result != 0)
		return 0;
	if (v->pre_disable_calls != before_pre + 1U)
		return 0;

	/* The well context counted both. */
	if (pwc->irq_post_enable_calls < 1U || pwc->irq_pre_disable_calls != 1U)
		return 0;

	/* Succeeded: both hooks ran through the well. */
	return 1;
}

/*
 * IRQ-DRAIN-WELL, IRQ-DRAIN-LATCH and IRQ-GATE: a handler still inside
 * pipe A keeps PW_A on, latches every later disable, and a closed pipe is
 * not entered.
 */
static void
i915_probe_irq_drain_well(
	struct i915_ktest *ktest,
	struct i915_power_well *pwa)
{
	struct i915_display_irq *d;
	struct i915_irq_vblank *v;
	struct i915_pw_ctx *pwc;
	struct i915_power_well *other;
	unsigned acks;
	int taken;
	int enabled;
	int latched;

	d = &i915_probe_display->irq;
	v = &i915_probe_display->irq_vblank;
	pwc = &i915_probe_display->pwc;

	/* The well of pipe B. */
	other = i915_probe_well_of_pipes(2U);

	/* PW_A taken; the post-enable left pipe A open. */
	taken = EINVAL;
	if (pwa != NULL)
		taken = drv_i915_power_well_get(pwa, pwc);
	drv_i915_ktest_check(ktest,
	    taken == 0 &&
	    pwa->refcount == 1U &&
	    d->pipe_closed[0] == 0,
	    "irq: IRQ-DRAIN-WELL PW_A taken; post-enable left pipe A open");

	/* An invocation that never leaves pipe A, then the last put of PW_A. */
	d->pipe_inflight[0] = 1U;
	enabled = 0;
	if (pwa != NULL) {
		drv_i915_power_well_put(pwa, pwc);
		enabled = drv_i915_power_well_is_enabled(pwa, pwc);
	}
	drv_i915_ktest_check(ktest,
	    pwc->irq_sync_failed == 1 &&
	    pwa != NULL &&
	    pwa->refcount == 1U &&
	    pwc->kept_wells == 1U &&
	    enabled == 1 &&
	    v->drain_timeouts == 1U &&
	    d->pipe_closed[0] == 1,
	    "irq: IRQ-DRAIN-WELL drain timed out -> POWER_REQUEST NOT cleared (the well still reads enabled), kept and owned");

	/* From then on every later well disable is refused, another pipe's too. */
	latched = i915_probe_drain_latch(other);
	drv_i915_ktest_check(ktest,
	    latched == 1,
	    "irq: IRQ-DRAIN-LATCH after that, every later well disable is refused (another pipe's well stays on too)");

	/* The handler refuses the closed pipe: its IIR is neither read nor acknowledged. */
	acks = d->de_pipe_iir_acks[0];
	d->pipe_inflight[0] = 0U;
	drv_i915_raw_write32(&i915_probe_mmio, 0x44200U, 1U << 16);
	drv_i915_raw_write32(&i915_probe_mmio, 0x44408U, 1U);
	drv_i915_gen11_display_irq_handler(d);
	drv_i915_ktest_check(ktest,
	    d->de_pipe_iir_acks[0] == acks &&
	    d->pipe_refused[0] >= 1U &&
	    d->pipe_inflight[0] == 0U,
	    "irq: IRQ-GATE the handler does not enter a closed pipe (admission refused, in-flight back to 0)");
}

/*
 * IRQ-DRAIN-LATCH: with the drain latch set, pipe B's well is taken and its
 * disable refused, and it stays on.  Reports whether every step held,
 * stopping at the first that did not.
 */
static int
i915_probe_drain_latch(
	struct i915_power_well *other)
{
	struct i915_pw_ctx *pwc;
	int result;

	pwc = &i915_probe_display->pwc;

	/* The ADL-P map has a well for pipe B. */
	if (other == NULL)
		return 0;

	/* Takes the well. */
	result = drv_i915_power_well_get(other, pwc);
	if (result != 0)
		return 0;

	/* Its disable is refused. */
	result = drv_i915_power_well_disable(other, pwc);
	if (result != EBUSY)
		return 0;
	if (pwc->disable_refusals < 2U)
		return 0;

	/* And it still reads enabled. */
	result = drv_i915_power_well_is_enabled(other, pwc);
	if (result != 1)
		return 0;

	/* Succeeded: the latch held. */
	return 1;
}

/*
 * IRQ-HOOK: the power-well hooks, the vblank references and waits, the
 * handler synchronisation and the drain, on the private display.
 */
static void
i915_probe_irq_hook(
	struct i915_ktest *ktest)
{
	struct i915_display_irq *d;
	struct i915_irq_vblank *v;
	struct i915_pw_ctx *pwc;
	struct i915_power_well *pwa;
	uint32_t seen;
	int enabled;
	int hooked;
	int waited;

	d = &i915_probe_display->irq;
	v = &i915_probe_display->irq_vblank;
	pwc = &i915_probe_display->pwc;
	seen = 0U;

	/* The post-enable hook and its interrupts-enabled gate. */
	i915_probe_irq_hook_post(ktest);

	/* Vblank enable and disable change only the IMR vblank bit. */
	enabled = i915_probe_vblank_enable();
	drv_i915_ktest_check(ktest,
	    enabled == 1,
	    "irq: VBL-ENABLE the first reference unmasks vblank in IMR (bdw_enable_vblank); the second only counts");

	/* Delivery, waits and puts. */
	i915_probe_vblank_deliver(ktest);
	i915_probe_vblank_wait(ktest);
	i915_probe_vblank_put(ktest);

	/* The pre-disable hook and the bounded synchronisation. */
	i915_probe_irq_hook_pre(ktest);

	/* Binds the hooks to the well context, as the display does. */
	pwa = i915_probe_well_of_pipes(1U);
	pwc->irqs_enabled = 1;
	pwc->irq_ops = &drv_i915_pw_irq_ops;
	pwc->irq_ctx = d;

	/* Through the power-well bodies: PW_A's enable and disable call the bound hooks. */
	hooked = i915_probe_hook_well(pwa);
	drv_i915_ktest_check(ktest,
	    hooked == 1,
	    "irq: IRQ-HOOK-WELL PW_A's enable restores and its disable stops pipe A's interrupts through the bound hooks");

	/* A handler still inside pipe A: the real disable entry must keep the well on. */
	i915_probe_irq_drain_well(ktest, pwa);

	/* One waiter per pipe: a second waiter on pipe C is refused. */
	i915_probe_irq.irqs_enabled = 1;
	(void)drv_i915_drm_vblank_get(d, 2U);
	v->waiting[2] = 1;
	waited = drv_i915_wait_vblank(d, 2U, 1U, 10U, i915_vblank_read_frame, NULL, &seen);
	drv_i915_ktest_check(ktest,
	    waited == EBUSY && v->second_waiter_refusals == 1U,
	    "irq: VBL-ONE a second waiter on the same pipe is refused (it would re-arm the first one's wake-up)");
	v->waiting[2] = 0;
	drv_i915_drm_vblank_put(d, 2U);

	/* The private display's latch goes with this test. */
	pwc->irq_sync_failed = 0;
	d->pipe_closed[0] = 0;
	pwc->irq_ops = NULL;
	pwc->irqs_enabled = 0;
	d->vbl = NULL;
	i915_probe_irq.irqs_enabled = 0;
}

/* IRQ-NODISPLAY: without a display the display reset and postinstall touch no register. */
static void
i915_probe_irq_nodisplay(
	struct i915_ktest *ktest)
{
	struct i915_display_irq nodisplay;

	/* A display interrupt half that reports no display. */
	kern_memset(&nodisplay, 0, sizeof(nodisplay));
	nodisplay.irq = &i915_probe_irq;
	nodisplay.m = &i915_probe_mmio;
	nodisplay.pd = &i915_probe_display->power_domains;
	nodisplay.pwc = &i915_probe_display->pwc;
	nodisplay.pch = &i915_probe_display->pch;
	nodisplay.display_ver = 13;
	nodisplay.pipe_mask = 0xfU;
	nodisplay.cpu_transcoder_mask = 0xfU;
	nodisplay.has_display = 0;

	/* Runs the display reset and postinstall. */
	i915_probe_fake.write_count = 0U;
	drv_i915_gen11_display_irq_reset(&nodisplay);
	drv_i915_gen11_de_irq_postinstall(&nodisplay);
	drv_i915_ktest_check(ktest,
	    i915_probe_fake.write_count == 0U,
	    "p4: IRQ-NODISPLAY display reset/postinstall are no-ops without a display");
}

/*
 * P5A-WM: the two PCODE latency reads.
 *
 * data0=0 gives levels 0..3, data0=1 levels 4..7.  Level 0 is non-zero, so
 * WaWmMemoryReadLatency must not add the read latency; level 6 is 0, so
 * levels 6..7 are zeroed.
 */
static void
i915_probe_wm(
	struct i915_ktest *ktest)
{
	static const struct i915_fake_pcode_txn latency[] = {
		{ 0x6U, 0x0U, 0x0e0a0602U, 0U, 0U },
		{ 0x6U, 0x1U, 0x00001a16U, 0U, 0U }
	};
	struct i915_display_nogem *ng;

	ng = &i915_probe_nogem;

	/* Scripts the two reads: 2,6,10,14 and 22,26,0,0. */
	i915_fake_open();
	i915_probe_fake.pcode_script = latency;
	i915_probe_fake.pcode_script_length = 2U;

	/* Reads the latencies into empty records. */
	kern_memset(ng, 0, sizeof(*ng));
	drv_i915_skl_setup_wm_latency(ng, 13, &i915_probe_sb_lock, &i915_probe_mmio, 0);

	/* HAS_HW_SAGV_WM gives 6 levels, not 8. */
	drv_i915_ktest_check(ktest,
	    i915_probe_fake.pcode_script_next == 2U &&
	    i915_probe_fake.pcode_script_broken == 0 &&
	    ng->wm_num_levels == 6U &&
	    ng->wm_skl_latency[0] == 2U &&
	    ng->wm_skl_latency[1] == 6U &&
	    ng->wm_skl_latency[2] == 10U &&
	    ng->wm_skl_latency[3] == 14U &&
	    ng->wm_skl_latency[4] == 22U &&
	    ng->wm_skl_latency[5] == 26U &&
	    ng->wm_latency_valid == 1,
	    "p5a: P5A-WM two PCODE reads (data0=0/1) decode 8 latencies, 6 levels");
}

/* P5A-WM: adjust_wm_latency zero truncation, read-latency add and the DIMM workaround. */
static void
i915_probe_wm_adjust(
	struct i915_ktest *ktest)
{
	uint16_t wm[8];
	int ok;

	ok = 1;

	/* Level 0 of 0 adds the read latency to every valid level. */
	wm[0] = 0U;
	wm[1] = 4U;
	wm[2] = 8U;
	wm[3] = 12U;
	wm[4] = 16U;
	wm[5] = 20U;
	wm[6] = 24U;
	wm[7] = 28U;
	drv_i915_adjust_wm_latency(wm, 6, 3, 0);
	if (wm[0] != 3U ||
	    wm[1] != 7U ||
	    wm[5] != 23U)
		ok = 0;

	/* A zero at level n>=1 disables n and above and stops the read-latency add. */
	wm[0] = 2U;
	wm[1] = 4U;
	wm[2] = 0U;
	wm[3] = 12U;
	wm[4] = 16U;
	wm[5] = 20U;
	wm[6] = 24U;
	wm[7] = 28U;
	drv_i915_adjust_wm_latency(wm, 6, 3, 0);
	if (wm[2] != 0U ||
	    wm[3] != 0U ||
	    wm[4] != 0U ||
	    wm[5] != 0U)
		ok = 0;
	if (wm[0] != 2U || wm[1] != 4U)
		ok = 0;

	/* The 16GB-DIMM workaround adds 1 to level 0 only. */
	wm[0] = 5U;
	wm[1] = 9U;
	wm[2] = 13U;
	wm[3] = 17U;
	wm[4] = 21U;
	wm[5] = 25U;
	wm[6] = 0U;
	wm[7] = 0U;
	drv_i915_adjust_wm_latency(wm, 6, 3, 1);
	if (wm[0] != 6U || wm[1] != 9U)
		ok = 0;

	drv_i915_ktest_check(ktest, ok == 1, "p5a: P5A-WM adjust_wm_latency zero-truncation, read-latency add, DIMM WA");
}

/* P5A-DPLL: adlp_plls is 7 entries with the reference ids; display 14 has none. */
static void
i915_probe_dpll(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *ng;
	struct i915_display_nogem *t;

	ng = &i915_probe_nogem;
	t = &i915_probe_nogem_scratch;

	/* Builds the ADL-P DPLL records. */
	kern_memset(ng, 0, sizeof(*ng));
	drv_i915_shared_dpll_init(ng, 13, 1);
	drv_i915_ktest_check(ktest,
	    ng->dpll_mgr_present == 1 &&
	    ng->num_dplls == 7U &&
	    ng->dplls[0].id == 0 &&
	    ng->dplls[0].enable_reg == 0x46010U &&
	    ng->dplls[1].id == 1 &&
	    ng->dplls[1].enable_reg == 0x46014U &&
	    ng->dplls[2].id == 2 &&
	    ng->dplls[2].enable_reg == 0x46020U &&
	    ng->dplls[2].funcs == I915_DPLL_FUNCS_TBT &&
	    ng->dplls[3].enable_reg == 0x46030U &&
	    ng->dplls[6].id == 6 &&
	    ng->dplls[6].enable_reg == 0x4603cU &&
	    ng->dplls[6].funcs == I915_DPLL_FUNCS_DKL &&
	    ng->dplls[0].funcs == I915_DPLL_FUNCS_COMBO,
	    "p5a: P5A-DPLL adlp_plls = DPLL0/1 + TBT + TC1..4 with the reference ids");

	/* Display 14 gets no table invented. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_shared_dpll_init(t, 14, 0);
	drv_i915_ktest_check(ktest,
	    t->dpll_mgr_present == 0 && t->num_dplls == 0U,
	    "p5a: P5A-DPLL ver>=14 has no shared DPLLs (no table is invented)");
}

/* P5C-DPLL: the native start -- pipe A on DDI A fed by DPLL1 keeps DPLL1. */
static void
i915_probe_dpll_native(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *t;
	uint32_t enable;
	int kept;

	t = &i915_probe_nogem_scratch;

	/* ADL-P DPLL records on an empty register model. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_shared_dpll_init(t, 13, 1);
	i915_fake_open();

	/* PHY A's clock selects DPLL1, which the firmware left enabled and locked. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 0x1U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x46014U, 0xc0000000U);

	/* Pipe A active on DDI A. */
	t->crtcs[0].state.active = 1;
	t->num_encoders = 1U;
	t->encoders[0].port = I915_PORT_A;
	t->encoders[0].phy = I915_PHY_A;
	t->encoders[0].clk_funcs = I915_DDI_CLK_ICL_COMBO;
	t->encoders[0].crtc_linked = 1;
	t->encoders[0].pipe_mask = 1U;

	/* Reads the DPLLs and sanitizes them. */
	drv_i915_dpll_readout(t, &i915_probe_mmio);
	i915_probe_fake.write_count = 0U;
	drv_i915_nogem_dpll_sanitize_state(t, &i915_probe_mmio, 13, 0);

	/* DPLL1 is still enabled. */
	enable = drv_i915_raw_read32(&i915_probe_mmio, 0x46014U);
	kept = 0;
	if ((enable & 0x80000000U) != 0U)
		kept = 1;

	drv_i915_ktest_check(ktest,
	    t->encoders[0].shared_dpll_id == 1 &&
	    t->dplls[1].on &&
	    t->dplls[1].pipe_mask == 1U &&
	    t->dplls[1].active_mask == 1U &&
	    t->dplls[0].pipe_mask == 0U &&
	    kept &&
	    t->dplls_disabled == 0U,
	    "p5c: P5C-DPLL native start: DDI A's clock select names DPLL1 -> pipe A credited -> sanitize keeps DPLL1");
}

/* P5C-DPLL-UNUSED: a PLL on with no active pipe on it is disabled (the reference's own case). */
static void
i915_probe_dpll_unused(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *t;
	uint32_t dpll0;
	uint32_t dpll1;

	t = &i915_probe_nogem_scratch;

	/* ADL-P DPLL records on an empty register model. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_shared_dpll_init(t, 13, 1);
	i915_fake_open();

	/* PHY A on DPLL0; DPLL0 and DPLL1 both on, nobody uses DPLL1. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 0x0U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x46010U, 0xc0000000U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x46014U, 0xc0000000U);

	/* Pipe A active on DDI A. */
	t->crtcs[0].state.active = 1;
	t->num_encoders = 1U;
	t->encoders[0].port = I915_PORT_A;
	t->encoders[0].phy = I915_PHY_A;
	t->encoders[0].clk_funcs = I915_DDI_CLK_ICL_COMBO;
	t->encoders[0].crtc_linked = 1;
	t->encoders[0].pipe_mask = 1U;

	/* Reads the DPLLs and sanitizes them. */
	drv_i915_dpll_readout(t, &i915_probe_mmio);
	drv_i915_nogem_dpll_sanitize_state(t, &i915_probe_mmio, 13, 0);

	/* DPLL0 stays; DPLL1 goes. */
	dpll0 = drv_i915_raw_read32(&i915_probe_mmio, 0x46010U);
	dpll1 = drv_i915_raw_read32(&i915_probe_mmio, 0x46014U);
	drv_i915_ktest_check(ktest,
	    t->dplls[0].active_mask == 1U &&
	    (dpll0 & 0x80000000U) != 0U &&
	    (dpll1 & 0x80000000U) == 0U &&
	    t->dplls_disabled == 1U,
	    "p5c: P5C-DPLL-UNUSED DPLL0 feeds pipe A and stays; DPLL1 on but unused is disabled (reference behaviour)");
}

/* P5C-DPLL-TC: an active Type-C link whose PLL cannot be read disables nothing. */
static void
i915_probe_dpll_tc(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *t;
	uint32_t dpll1;

	t = &i915_probe_nogem_scratch;

	/* ADL-P DPLL records on an empty register model with DPLL1 on. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_shared_dpll_init(t, 13, 1);
	i915_fake_open();
	drv_i915_raw_write32(&i915_probe_mmio, 0x46014U, 0xc0000000U);

	/* Pipe B active on TC1. */
	t->crtcs[1].state.active = 1;
	t->num_encoders = 1U;
	t->encoders[0].port = I915_PORT_TC1;
	t->encoders[0].phy = I915_PHY_F;
	t->encoders[0].clk_funcs = I915_DDI_CLK_ICL_TC;
	t->encoders[0].crtc_linked = 1;
	t->encoders[0].pipe_mask = 2U;

	/* Reads the DPLLs and sanitizes them. */
	drv_i915_dpll_readout(t, &i915_probe_mmio);
	drv_i915_nogem_dpll_sanitize_state(t, &i915_probe_mmio, 13, 0);

	/* The readout is incomplete and DPLL1 stays on. */
	dpll1 = drv_i915_raw_read32(&i915_probe_mmio, 0x46014U);
	drv_i915_ktest_check(ktest,
	    t->dplls[1].readout_incomplete &&
	    (dpll1 & 0x80000000U) != 0U &&
	    t->dplls_disabled == 0U,
	    "p5c: P5C-DPLL-TC an active TC link's PLL is unknown -> readout incomplete -> no PLL is disabled");
}

/* P5A-CRTC: 6 planes per pipe (1 primary + 4 sprites + cursor). */
static void
i915_probe_crtc(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *ng;
	int created;

	ng = &i915_probe_nogem;

	/* Creates pipe A's crtc in empty records. */
	kern_memset(ng, 0, sizeof(*ng));
	created = drv_i915_crtc_init(ng, 13, 0U);

	/* Plane ids 0,1,2,3,4 and 7. */
	drv_i915_ktest_check(ktest,
	    created == 0 &&
	    ng->crtcs[0].num_planes == 6U &&
	    ng->crtcs[0].num_scalers == 2U &&
	    ng->crtcs[0].planes[0].type == I915_PLANE_PRIMARY &&
	    ng->crtcs[0].planes[1].type == I915_PLANE_SPRITE &&
	    ng->crtcs[0].planes[4].type == I915_PLANE_SPRITE &&
	    ng->crtcs[0].planes[5].type == I915_PLANE_CURSOR &&
	    ng->crtcs[0].planes[5].id == 7 &&
	    ng->crtcs[0].plane_ids_mask == 0x9fU &&
	    ng->crtcs[0].state.cpu_transcoder == -1 &&
	    ng->crtcs[0].fifo_underrun_reporting == 0,
	    "p5a: P5A-CRTC ADL-P pipe = primary + 4 sprites + cursor, 2 scalers");
}

/* P5A-MAXCDCLK: the display 11 and later reference-clock split. */
static void
i915_probe_max_cdclk(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *ng;

	ng = &i915_probe_nogem;
	kern_memset(ng, 0, sizeof(*ng));

	/* A 38.4 MHz reference. */
	drv_i915_update_max_cdclk(ng, 13, 38400U);
	drv_i915_ktest_check(ktest, ng->max_cdclk_freq == 652800U, "p5a: P5A-MAXCDCLK ref 38.4MHz -> 652800");

	/* A 24 MHz reference. */
	drv_i915_update_max_cdclk(ng, 13, 24000U);
	drv_i915_ktest_check(ktest, ng->max_cdclk_freq == 648000U, "p5a: P5A-MAXCDCLK ref 24MHz -> 648000");
}

/* P5A-WA: the ADL-P and xe_d display workaround register operations. */
static void
i915_probe_wa(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *ng;
	struct i915_display_nogem *t;
	int dpce;
	int ddi_clock;
	int dpfc;
	int clkreq;

	ng = &i915_probe_nogem;
	t = &i915_probe_nogem_scratch;

	/* The ADL-P arm on registers with the bits in the opposite state. */
	i915_fake_open();
	kern_memset(ng, 0, sizeof(*ng));
	drv_i915_raw_write32(&i915_probe_mmio, 0x46540U, 0U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x46430U, 0xffffffffU);
	i915_probe_fake.write_count = 0U;
	drv_i915_display_wa_apply(ng, &i915_probe_mmio, 13, 1);
	dpce = i915_fake_find(0x46540U, 1U << 17, 1U << 17);
	ddi_clock = i915_fake_find(0x46430U, 0U, 1U << 7);
	drv_i915_ktest_check(ktest,
	    ng->adlp_wa_applied == 1 &&
	    dpce >= 0 &&
	    ddi_clock >= 0,
	    "p5a: P5A-WA Wa_22011091694 sets DPCE_GATING_DIS, Bspec49189 clears DDI_CLOCK_REG_ACCESS");

	/* Display 13 that is not ADL-P has no arm. */
	kern_memset(t, 0, sizeof(*t));
	i915_probe_fake.write_count = 0U;
	drv_i915_display_wa_apply(t, &i915_probe_mmio, 13, 0);
	drv_i915_ktest_check(ktest,
	    t->adlp_wa_applied == 0 &&
	    t->xe_d_wa_applied == 0 &&
	    i915_probe_fake.write_count == 0U,
	    "p5a: P5A-WA display 13 that is not ADL-P has no arm and writes nothing");

	/* The xe_d arm: Tiger Lake and friends. */
	kern_memset(t, 0, sizeof(*t));
	drv_i915_raw_write32(&i915_probe_mmio, 0x43224U, 0xffffffffU);
	drv_i915_raw_write32(&i915_probe_mmio, 0x101038U, 0xffffffffU);
	i915_probe_fake.write_count = 0U;
	drv_i915_display_wa_apply(t, &i915_probe_mmio, 12, 0);
	dpfc = i915_fake_find(0x43224U, 1U << 14, 0xffffffffU);
	clkreq = i915_fake_find(0x101038U, 0U, 1U << 1);
	drv_i915_ktest_check(ktest,
	    t->xe_d_wa_applied == 1 &&
	    t->adlp_wa_applied == 0 &&
	    dpfc >= 0 &&
	    clkreq >= 0,
	    "p5a: P5A-WA-XED Wa_1409120013 writes the DPFC chicken, Wa_14013723622 clears CLKREQ_POLICY_MEM_UP_OVRD");
}

/* P5B-PORTMAP: the xelpd DVO-to-port mapping, and the port, PHY and Type-C predicates. */
static void
i915_probe_portmap(
	struct i915_ktest *ktest)
{
	int hdmia;
	int hdmib;
	int hdmic;
	int hdmif;
	int hdmii;
	int dpa;
	int hdmid;
	int legacy_hdmid;
	int phy_a;
	int phy_b;
	int phy_tc1;
	int phy_b_tc;
	int phy_f_tc;
	int ddi_b_tc;
	int ddi_tc1_tc;
	int crt;

	/* Maps the DVO port codes on display 13, and HDMID on the legacy map. */
	hdmia = drv_i915_dvo_port_to_port(13, 0U);
	hdmib = drv_i915_dvo_port_to_port(13, 1U);
	hdmic = drv_i915_dvo_port_to_port(13, 2U);
	hdmif = drv_i915_dvo_port_to_port(13, 14U);
	hdmii = drv_i915_dvo_port_to_port(13, 17U);
	dpa = drv_i915_dvo_port_to_port(13, 10U);
	hdmid = drv_i915_dvo_port_to_port(13, 3U);
	legacy_hdmid = drv_i915_dvo_port_to_port(12, 3U);

	/* The Type-C ports take HDMIF..HDMII; HDMID is not xelpd. */
	drv_i915_ktest_check(ktest,
	    hdmia == I915_PORT_A &&
	    hdmib == I915_PORT_B &&
	    hdmic == I915_PORT_C &&
	    hdmif == I915_PORT_TC1 &&
	    hdmii == I915_PORT_TC4 &&
	    dpa == I915_PORT_A &&
	    hdmid == I915_PORT_NONE &&
	    legacy_hdmid == I915_PORT_D,
	    "p5b: P5B-PORTMAP ver>=13 uses the xelpd map (TC ports take HDMIF..HDMII)");

	/* Asks the port-to-PHY map and the Type-C predicates. */
	phy_a = drv_i915_port_to_phy(13, I915_PORT_A);
	phy_b = drv_i915_port_to_phy(13, I915_PORT_B);
	phy_tc1 = drv_i915_port_to_phy(13, I915_PORT_TC1);
	phy_b_tc = drv_i915_phy_is_tc(13, I915_PHY_B);
	phy_f_tc = drv_i915_phy_is_tc(13, I915_PHY_F);
	ddi_b_tc = drv_i915_ddi_is_tc(13, I915_PORT_B);
	ddi_tc1_tc = drv_i915_ddi_is_tc(13, I915_PORT_TC1);
	crt = drv_i915_ddi_crt_present(13);

	/* No DDI CRT on display 9 and later. */
	drv_i915_ktest_check(ktest,
	    phy_a == I915_PHY_A &&
	    phy_b == I915_PHY_B &&
	    phy_tc1 == I915_PHY_F &&
	    phy_b_tc == 0 &&
	    phy_f_tc == 1 &&
	    ddi_b_tc == 0 &&
	    ddi_tc1_tc == 1 &&
	    crt == 0,
	    "p5b: P5B-PORTMAP port->phy, phy_is_tc, ddi_is_tc, no DDI CRT on ver>=9");
}

/*
 * P5B-OUTPUTS: the missing-defaults VBT on ADL-P.
 *
 * init_vbt_missing_defaults() generates children for PORT_A/B/C (the
 * non-TC PHYs).  On ADL-P the port mask has no PORT_C, so the third child
 * legitimately fails assert_port_valid and is skipped: two encoders, not
 * three.
 */
static void
i915_probe_outputs(
	struct i915_ktest *ktest)
{
	struct i915_display_nogem *t;
	unsigned port_mask;

	t = &i915_probe_nogem_scratch;

	/* Ports A, B and TC1..TC4. */
	port_mask = (1U << 0) | (1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 6);

	/* The default children, the output setup on an empty register model. */
	kern_memset(&i915_probe_vbt, 0, sizeof(i915_probe_vbt));
	drv_i915_bios_init_vbt_missing_defaults(&i915_probe_vbt);
	kern_memset(t, 0, sizeof(*t));
	i915_fake_open();
	drv_i915_setup_outputs(t, 13, port_mask, &i915_probe_vbt, &i915_probe_mmio);

	drv_i915_ktest_check(ktest,
	    i915_probe_vbt.num_display_devices == 3U &&
	    t->ddi_init_calls == 3U &&
	    t->num_encoders == 2U &&
	    t->ddi_skipped == 1U &&
	    t->ddi_skip_reason[0] == I915_DDI_SKIP_PORT_INVALID &&
	    t->ddi_skip_port[0] == I915_PORT_C &&
	    t->encoders[0].port == I915_PORT_A &&
	    t->encoders[1].port == I915_PORT_B &&
	    t->outputs_done == 1 &&
	    t->crt_present == 0,
	    "p5b: P5B-OUTPUTS 3 VBT children -> 2 encoders; PORT_C fails assert_port_valid");

	/* PORT_A is eDP only (DDI_LANES_A, no DVI bit); PORT_B is DP and HDMI (DDI_LANES_B). */
	drv_i915_ktest_check(ktest,
	    t->encoders[0].phy == I915_PHY_A &&
	    t->encoders[0].is_tc == 0 &&
	    t->encoders[0].clk_funcs == I915_DDI_CLK_ICL_COMBO &&
	    t->encoders[0].power_domain == I915_PW_DOMAIN_PORT_DDI_LANES_A &&
	    t->encoders[0].init_dp == 1 &&
	    t->encoders[0].init_hdmi == 0 &&
	    t->encoders[1].power_domain == I915_PW_DOMAIN_PORT_DDI_LANES_B &&
	    t->encoders[1].init_dp == 1 &&
	    t->encoders[1].init_hdmi == 1,
	    "p5b: P5B-OUTPUTS PORT_A is eDP-only (no DVI bit), PORT_B is DP+HDMI");

	/* Each early return of intel_ddi_init. */
	i915_probe_outputs_early(ktest, port_mask);
}

/* P5B-OUTPUTS: each early return of intel_ddi_init is reachable. */
static void
i915_probe_outputs_early(
	struct i915_ktest *ktest,
	unsigned port_mask)
{
	struct i915_vbt_state *v2;
	struct i915_display_nogem *t2;
	int ok;

	v2 = &i915_probe_vbt_scratch;
	t2 = &i915_probe_nogem_readout;
	ok = 1;

	/* One child. */
	kern_memset(v2, 0, sizeof(*v2));
	v2->num_display_devices = 1U;

	/* A DVO port that maps to no port at all. */
	v2->display_devices[0].dvo_port = 99U;
	v2->display_devices[0].device_type = 0x4U;
	kern_memset(t2, 0, sizeof(*t2));
	drv_i915_setup_outputs(t2, 13, port_mask, v2, &i915_probe_mmio);
	if (t2->num_encoders != 0U || t2->ddi_skip_reason[0] != I915_DDI_SKIP_PORT_NONE)
		ok = 0;

	/* A DSI child takes the icl_dsi_init path, not intel_ddi_init (HDMIA, MIPI_OUTPUT). */
	v2->display_devices[0].dvo_port = 0U;
	v2->display_devices[0].device_type = 1U << 10;
	kern_memset(t2, 0, sizeof(*t2));
	drv_i915_setup_outputs(t2, 13, port_mask, v2, &i915_probe_mmio);
	if (t2->num_encoders != 0U || t2->ddi_skip_reason[0] != I915_DDI_SKIP_DSI)
		ok = 0;

	/* Neither DVI/HDMI nor DP: the reference respects it. */
	v2->display_devices[0].device_type = 0U;
	kern_memset(t2, 0, sizeof(*t2));
	drv_i915_setup_outputs(t2, 13, port_mask, v2, &i915_probe_mmio);
	if (t2->num_encoders != 0U || t2->ddi_skip_reason[0] != I915_DDI_SKIP_NOT_DVI_HDMI_DP)
		ok = 0;

	/* The same port twice (HDMIA and DPA): the second is already claimed. */
	v2->num_display_devices = 2U;
	v2->display_devices[0].dvo_port = 0U;
	v2->display_devices[0].device_type = 0x4U;
	v2->display_devices[1].dvo_port = 10U;
	v2->display_devices[1].device_type = 0x4U;
	kern_memset(t2, 0, sizeof(*t2));
	drv_i915_setup_outputs(t2, 13, port_mask, v2, &i915_probe_mmio);
	if (t2->num_encoders != 1U || t2->ddi_skip_reason[0] != I915_DDI_SKIP_PORT_IN_USE)
		ok = 0;

	drv_i915_ktest_check(ktest, ok == 1, "p5b: P5B-OUTPUTS every intel_ddi_init early return is reachable");
}

/* P5B-DDICLK: combo versus Type-C clock state, and the combo disable. */
static void
i915_probe_ddi_clock(
	struct i915_ktest *ktest)
{
	struct i915_encoder combo;
	struct i915_encoder tc;
	int enabled;
	int gated;
	int ok;

	ok = 1;

	/* A combo encoder on PHY B, whose DDI_CLK_OFF is bit 11. */
	kern_memset(&combo, 0, sizeof(combo));
	combo.port = I915_PORT_B;
	combo.phy = I915_PHY_B;
	combo.clk_funcs = I915_DDI_CLK_ICL_COMBO;

	/* The clock runs with the bit clear and is gated with it set. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 0U);
	enabled = drv_i915_ddi_is_clock_enabled(&combo, &i915_probe_mmio);
	if (enabled != 1)
		ok = 0;
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 1U << 11);
	enabled = drv_i915_ddi_is_clock_enabled(&combo, &i915_probe_mmio);
	if (enabled != 0)
		ok = 0;

	/* The disable sets the bit. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 0U);
	i915_probe_fake.write_count = 0U;
	drv_i915_ddi_disable_clock(&i915_probe_nogem_scratch, &combo, &i915_probe_mmio);
	gated = i915_fake_find(0x164280U, 1U << 11, 1U << 11);
	if (gated < 0)
		ok = 0;

	/* A Type-C encoder on TC1 needs both a DDI_CLK_SEL other than NONE and its TC clock not off. */
	kern_memset(&tc, 0, sizeof(tc));
	tc.port = I915_PORT_TC1;
	tc.phy = I915_PHY_F;
	tc.clk_funcs = I915_DDI_CLK_ICL_TC;

	/* SEL_NONE: no clock. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x46100U + 3U * 4U, 0U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 0U);
	enabled = drv_i915_ddi_is_clock_enabled(&tc, &i915_probe_mmio);
	if (enabled != 0)
		ok = 0;

	/* A selected clock runs. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x46100U + 3U * 4U, 0x80000000U);
	enabled = drv_i915_ddi_is_clock_enabled(&tc, &i915_probe_mmio);
	if (enabled != 1)
		ok = 0;

	/* TC1's clock off gates it. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 1U << 12);
	enabled = drv_i915_ddi_is_clock_enabled(&tc, &i915_probe_mmio);
	if (enabled != 0)
		ok = 0;

	drv_i915_ktest_check(ktest, ok == 1, "p5b: P5B-DDICLK combo uses DDI_CLK_OFF(phy); TC needs CLK_SEL + TC_CLK_OFF");
}

/*
 * P5C-READOUT and P5D: the readout of a quiescent device and of one active
 * pipe, then each arm of the sanitize, on the private display's power
 * wells.
 */
static void
i915_probe_readout(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *rd;
	unsigned port_mask;
	unsigned pipe;

	display = i915_probe_display;
	rd = &i915_probe_nogem_readout;
	port_mask = (1U << 0) | (1U << 1) | (1U << 3) | (1U << 4) | (1U << 5) | (1U << 6);

	/* An empty register model with every power gate's fuses loaded. */
	i915_fake_open();
	i915_probe_fake.fuse_status = 0xFFFFFFFFU;

	/* A fresh ADL-P power-well map and a well context on the model, every well on. */
	drv_i915_trace_init(&display->dc_trace);
	(void)drv_i915_power_domains_init(&display->power_domains, 13U, -1, 1, &display->dc_trace);
	kern_memset(&display->pwc, 0, sizeof(display->pwc));
	display->pwc.mmio = &i915_probe_mmio;
	display->pwc.vga = &display->vga_client;
	i915_probe_wells_set(1);

	/* Four crtcs, the DPLLs and the default outputs. */
	kern_memset(rd, 0, sizeof(*rd));
	for (pipe = 0U; pipe < 4U; pipe++)
		(void)drv_i915_crtc_init(rd, 13, pipe);
	drv_i915_shared_dpll_init(rd, 13, 1);
	kern_memset(&i915_probe_vbt, 0, sizeof(i915_probe_vbt));
	drv_i915_bios_init_vbt_missing_defaults(&i915_probe_vbt);
	drv_i915_setup_outputs(rd, 13, port_mask, &i915_probe_vbt, &i915_probe_mmio);

	/* Everything quiescent: the real device's state. */
	drv_i915_modeset_readout_hw_state(rd, 13, &i915_probe_mmio, &display->power_domains, &display->pwc);
	drv_i915_ktest_check(ktest,
	    rd->readout_done == 1 &&
	    rd->readout_crtcs == 4U &&
	    rd->active_pipes == 0U &&
	    rd->readout_planes_visible == 0U &&
	    rd->readout_encoders_linked == 0U &&
	    rd->readout_dplls_on == 0U &&
	    rd->crtcs[0].state.cpu_transcoder == -1 &&
	    rd->crtcs[0].active == 0 &&
	    rd->crtcs[0].enabled == 0,
	    "p5c: P5C-READOUT a quiescent device reads back nothing active");

	/* Pipe B active, then an unpowered pipe. */
	i915_probe_readout_active(ktest);

	/* The sanitize arms. */
	i915_probe_sanitize_quiet(ktest);
	i915_probe_sanitize_dpll(ktest);
	i915_probe_sanitize_cmtg(ktest);
	i915_probe_sanitize_fbc(ktest);
	i915_probe_sanitize_encoder_clock(ktest);
	i915_probe_sanitize_active(ktest);
	i915_probe_sanitize_well(ktest);

	/* Drops the power-well map. */
	drv_i915_power_domains_cleanup(&display->power_domains);
}

/*
 * P5C-READOUT: pipe B active, then the same state with the pipe's power
 * off.
 *
 * Pipe B: TRANS_DDI_FUNC_CTL(B) enabled and selecting DDI B, TRANSCONF(B)
 * enabled, timings and PIPESRC set, the primary plane of B enabled,
 * DDI_BUF_CTL(B) enabled and DPLL0 on.
 */
static void
i915_probe_readout_active(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *rd;

	display = i915_probe_display;
	rd = &i915_probe_nogem_readout;

	/* Programs the model with pipe B lit. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x61400U, (1U << 31) | ((1U + 1U) << 27));
	drv_i915_raw_write32(&i915_probe_mmio, 0x71008U, 1U << 31);
	drv_i915_raw_write32(&i915_probe_mmio, 0x61000U, (2559U << 16) | 1919U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x6100cU, (1124U << 16) | 1079U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x6101cU, (1919U << 16) | 1079U);
	drv_i915_raw_write32(&i915_probe_mmio, 0x71180U, 1U << 31);
	drv_i915_raw_write32(&i915_probe_mmio, 0x64100U, 1U << 31);
	drv_i915_raw_write32(&i915_probe_mmio, 0x46010U, 1U << 31);

	/* Reads it back. */
	drv_i915_modeset_readout_hw_state(rd, 13, &i915_probe_mmio, &display->power_domains, &display->pwc);
	drv_i915_ktest_check(ktest,
	    rd->active_pipes == (1U << 1) &&
	    rd->crtcs[1].active == 1 &&
	    rd->crtcs[1].enabled == 1 &&
	    rd->crtcs[1].state.cpu_transcoder == 1 &&
	    rd->crtcs[1].state.enabled_transcoders == (1U << 1) &&
	    rd->crtcs[0].active == 0 &&
	    rd->crtcs[2].active == 0,
	    "p5c: P5C-READOUT pipe B reads back active via TRANS_DDI_FUNC + TRANSCONF");
	drv_i915_ktest_check(ktest,
	    rd->crtcs[1].state.hdisplay == 1920U &&
	    rd->crtcs[1].state.htotal == 2560U &&
	    rd->crtcs[1].state.vdisplay == 1080U &&
	    rd->crtcs[1].state.vtotal == 1125U &&
	    rd->crtcs[1].state.pipe_src_w == 1920U &&
	    rd->crtcs[1].state.pipe_src_h == 1080U,
	    "p5c: P5C-READOUT transcoder timings and PIPESRC decode (+1 on each field)");
	drv_i915_ktest_check(ktest,
	    rd->readout_planes_visible == 1U &&
	    rd->crtcs[1].planes[0].visible == 1 &&
	    rd->crtcs[1].state.active_planes == 0x1U &&
	    rd->crtcs[0].planes[0].visible == 0,
	    "p5c: P5C-READOUT only the enabled plane reads back visible");
	drv_i915_ktest_check(ktest,
	    rd->readout_encoders_linked == 1U &&
	    rd->encoders[1].crtc_linked == 1 &&
	    rd->encoders[1].pipe_mask == (1U << 1) &&
	    rd->encoders[1].is_mst == 0 &&
	    rd->encoders[0].crtc_linked == 0 &&
	    rd->readout_dplls_on == 1U &&
	    rd->dplls[0].on == 1 &&
	    rd->dplls[1].on == 0,
	    "p5c: P5C-READOUT DDI B links to pipe B; DPLL0 reads back on");

	/* The pipe power being off must not be read as inactive. */
	i915_probe_wells_set(0);
	drv_i915_modeset_readout_hw_state(rd, 13, &i915_probe_mmio, &display->power_domains, &display->pwc);
	drv_i915_ktest_check(ktest,
	    rd->active_pipes == 0U &&
	    rd->crtcs[1].state.power_gated == 1 &&
	    rd->crtcs[1].state.transconf == 0U,
	    "p5c: P5C-READOUT an unpowered pipe is recorded as power-gated, not read");
	i915_probe_wells_set(1);
}

/* P5D-SANITIZE: a quiescent device needs no hardware change. */
static void
i915_probe_sanitize_quiet(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *q;
	unsigned pipe;

	display = i915_probe_display;
	q = &i915_probe_nogem_scratch;

	/* Reads the device back once more. */
	drv_i915_modeset_readout_hw_state(&i915_probe_nogem_readout, 13, &i915_probe_mmio, &display->power_domains, &display->pwc);

	/* Only the shape the sanitize needs: four crtcs and the DPLLs. */
	kern_memset(q, 0, sizeof(*q));
	for (pipe = 0U; pipe < 4U; pipe++)
		(void)drv_i915_crtc_init(q, 13, pipe);
	drv_i915_shared_dpll_init(q, 13, 1);

	/* Sanitizes a D0 part with FBC A. */
	i915_probe_fake.write_count = 0U;
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_D0, 0x1U, &i915_probe_mmio, &display->power_domains, &display->pwc);

	/* D0 is not A0; display 13 is not 10..12; display 4 and later return from the plane mapping. */
	drv_i915_ktest_check(ktest,
	    q->sanitize_done == 1 &&
	    q->vblank_resets == 4U &&
	    q->dmc_pipes_enabled == 0U &&
	    q->vblank_on_count == 0U &&
	    q->dplls_disabled == 0U &&
	    q->encoder_clocks_gated == 0U &&
	    q->crtc_disable_noatomic_unimplemented == 0 &&
	    q->cmtg_wa_applied == 0 &&
	    q->early_display_was_applied == 0 &&
	    q->plane_mapping_sanitized == 0,
	    "p5d: P5D-SANITIZE a quiescent device needs no hardware change");
}

/* P5D-DPLL: an enabled but unused PLL is disabled; an enabled one in use is not. */
static void
i915_probe_sanitize_dpll(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *q;
	int dpll0;
	int dpll1;

	display = i915_probe_display;
	q = &i915_probe_nogem_scratch;

	/* DPLL0 on and unused, DPLL1 on and used by pipe B, the TBT PLL off. */
	kern_memset(q, 0, sizeof(*q));
	drv_i915_shared_dpll_init(q, 13, 1);
	q->dplls[0].on = 1;
	q->dplls[0].active_mask = 0U;
	q->dplls[1].on = 1;
	q->dplls[1].active_mask = 0x2U;
	q->dplls[2].on = 0;
	drv_i915_raw_write32(&i915_probe_mmio, 0x46010U, 1U << 31);
	drv_i915_raw_write32(&i915_probe_mmio, 0x46014U, 1U << 31);

	/* Sanitizes. */
	i915_probe_fake.write_count = 0U;
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_D0, 0U, &i915_probe_mmio, &display->power_domains, &display->pwc);
	dpll0 = i915_fake_find(0x46010U, 0U, 1U << 31);
	dpll1 = i915_fake_find(0x46014U, 0U, 1U << 31);
	drv_i915_ktest_check(ktest,
	    q->dplls_disabled == 1U &&
	    q->dplls[0].on == 0 &&
	    q->dplls[1].on == 1 &&
	    dpll0 >= 0 &&
	    dpll1 < 0,
	    "p5d: P5D-DPLL only an enabled-but-unused shared DPLL is turned off");
}

/* P5D-CMTG: the ADL-P A0 CMTG workaround gate. */
static void
i915_probe_sanitize_cmtg(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *q;

	display = i915_probe_display;
	q = &i915_probe_nogem_scratch;

	/* DPLL0 and DPLL1 on and in use. */
	kern_memset(q, 0, sizeof(*q));
	drv_i915_shared_dpll_init(q, 13, 1);
	q->dplls[0].on = 1;
	q->dplls[0].active_mask = 0x1U;
	q->dplls[1].on = 1;
	q->dplls[1].active_mask = 0x1U;

	/* Sanitizes an A0 part. */
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_A0, 0U, &i915_probe_mmio, &display->power_domains, &display->pwc);
	drv_i915_ktest_check(ktest,
	    q->cmtg_wa_applied == 1 && q->dplls_disabled == 0U,
	    "p5d: P5D-CMTG Wa_16011069516 fires on ADL-P A0 for DPLL0 only");
}

/* P5D-FBC: an FBC left active by the pre-OS is deactivated; an inactive one is left alone. */
static void
i915_probe_sanitize_fbc(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *q;
	int cleared;
	int touched;

	display = i915_probe_display;
	q = &i915_probe_nogem_scratch;

	/* DPFC_CTL with the enable bit set. */
	kern_memset(q, 0, sizeof(*q));
	drv_i915_raw_write32(&i915_probe_mmio, 0x43208U, (1U << 31) | 0x5U);
	i915_probe_fake.write_count = 0U;
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_D0, 0x1U, &i915_probe_mmio, &display->power_domains, &display->pwc);
	cleared = i915_fake_find(0x43208U, 0x5U, 0xffffffffU);
	drv_i915_ktest_check(ktest,
	    q->fbc_deactivated == 1U && cleared >= 0,
	    "p5d: P5D-FBC an active FBC is deactivated by clearing DPFC_CTL_EN only");

	/* DPFC_CTL clear. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x43208U, 0U);
	kern_memset(q, 0, sizeof(*q));
	i915_probe_fake.write_count = 0U;
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_D0, 0x1U, &i915_probe_mmio, &display->power_domains, &display->pwc);
	touched = i915_fake_find(0x43208U, 0U, 0xffffffffU);
	drv_i915_ktest_check(ktest,
	    q->fbc_deactivated == 0U && touched < 0,
	    "p5d: P5D-FBC an inactive FBC is left alone");
}

/* P5D-ENCCLK: only a disabled encoder's ungated DDI clock is gated. */
static void
i915_probe_sanitize_encoder_clock(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *q;
	int gated_a;
	int gated_b;

	display = i915_probe_display;
	q = &i915_probe_nogem_scratch;

	/* A disabled encoder on PHY A and an encoder in use on PHY B. */
	kern_memset(q, 0, sizeof(*q));
	q->num_encoders = 2U;
	q->encoders[0].port = I915_PORT_A;
	q->encoders[0].phy = I915_PHY_A;
	q->encoders[0].clk_funcs = I915_DDI_CLK_ICL_COMBO;
	q->encoders[0].crtc_linked = 0;
	q->encoders[1].port = I915_PORT_B;
	q->encoders[1].phy = I915_PHY_B;
	q->encoders[1].clk_funcs = I915_DDI_CLK_ICL_COMBO;
	q->encoders[1].crtc_linked = 1;

	/* Both clocks ungated. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x164280U, 0U);
	i915_probe_fake.write_count = 0U;
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_D0, 0U, &i915_probe_mmio, &display->power_domains, &display->pwc);
	gated_a = i915_fake_find(0x164280U, 1U << 10, 1U << 10);
	gated_b = i915_fake_find(0x164280U, 1U << 11, 1U << 11);
	drv_i915_ktest_check(ktest,
	    q->encoder_clocks_gated == 1U &&
	    gated_a >= 0 &&
	    gated_b < 0,
	    "p5d: P5D-ENCCLK only the DISABLED encoder's DDI clock is gated");
}

/* P5D-ACTIVE: an active pipe with no encoders is reported, not faked. */
static void
i915_probe_sanitize_active(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *q;
	int pipe_dmc;

	display = i915_probe_display;
	q = &i915_probe_nogem_scratch;

	/* Pipe B active with no encoder. */
	kern_memset(q, 0, sizeof(*q));
	(void)drv_i915_crtc_init(q, 13, 1U);
	q->crtcs[1].state.active = 1;

	/* Sanitizes; PIPEDMC_CONTROL(B) gets its enable. */
	i915_probe_fake.write_count = 0U;
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_D0, 0U, &i915_probe_mmio, &display->power_domains, &display->pwc);
	pipe_dmc = i915_fake_find(0x45250U + 4U, 1U << 0, 1U << 0);
	drv_i915_ktest_check(ktest,
	    q->crtc_disable_noatomic_unimplemented == 1 &&
	    q->dmc_pipes_enabled == 1U &&
	    q->vblank_on_count == 1U &&
	    q->crtcs[1].fifo_underrun_reporting == 0 &&
	    pipe_dmc >= 0,
	    "p5d: P5D-ACTIVE an active pipe enables its DMC + vblank and flags the missing crtc_disable_noatomic instead of pretending");
}

/*
 * P5D-WELL: a firmware-left unused well is disabled, in reverse.
 *
 * The DC_off well's disable enables the target DC state only with a DMC
 * payload; the sanitize gets one so that well can turn off too.  The
 * counts use the same fresh is_enabled() read the sanitize uses.
 */
static void
i915_probe_sanitize_well(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	struct i915_display_nogem *q;
	int before;
	int after;

	display = i915_probe_display;
	q = &i915_probe_nogem_scratch;

	/* A DMC payload and the DC states of the power-well map. */
	display->pwc.display_ver = 13;
	display->pwc.dmc_has_payload = 1;
	display->pwc.allowed_dc_mask = display->power_domains.allowed_dc_mask;
	display->pwc.target_dc_state = display->power_domains.target_dc_state;

	/* Counts the wells on, sanitizes, and counts again. */
	before = i915_probe_count_enabled_wells();
	kern_memset(q, 0, sizeof(*q));
	drv_i915_modeset_sanitize_hw_state(q, 13, I915_STEP_D0, 0U, &i915_probe_mmio, &display->power_domains, &display->pwc);
	after = i915_probe_count_enabled_wells();
	drv_i915_ktest_check(ktest,
	    before > 0 && (int)q->wells_disabled + after == before,
	    "p5d: P5D-WELL every unreferenced non-always-on well that reads back enabled is disabled");
}

/* P5A-VGA: the disable sequence is I/O first, then the register. */
static void
i915_probe_vga_disable(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	int disabled;
	int plane_off;

	display = i915_probe_display;

	/* Every legacy VGA access goes to the recorder. */
	i915_fake_open();
	drv_i915_vga_io_test_set(display, &i915_vga_record_ops);

	/* Already disabled: the reference returns without touching I/O. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x41000U, 1U << 31);
	i915_vga_record_reset();
	disabled = drv_i915_vga_disable(display, &display->vga_client, &i915_probe_mmio);
	drv_i915_ktest_check(ktest,
	    disabled == 1 && i915_probe_vga.count == 0U,
	    "p5a: P5A-VGA VGA_DISP_DISABLE already set -> no IO, no write");

	/* Not disabled: get, SR01 read-modify-write, put, then the register. */
	drv_i915_raw_write32(&i915_probe_mmio, 0x41000U, 0U);
	i915_vga_record_reset();
	i915_probe_fake.write_count = 0U;
	disabled = drv_i915_vga_disable(display, &display->vga_client, &i915_probe_mmio);
	plane_off = i915_fake_find(0x41000U, 1U << 31, 1U << 31);

	/* get, out, in, out, put; SR01 screen-off set; then VGA_DISP_DISABLE. */
	drv_i915_ktest_check(ktest,
	    disabled == 0 &&
	    i915_probe_vga.got == 1 &&
	    i915_probe_vga.put == 1 &&
	    i915_probe_vga.count == 5U &&
	    i915_probe_vga.sequence[0] == 1 &&
	    i915_probe_vga.sequence[1] == 3 &&
	    i915_probe_vga.sequence[2] == 2 &&
	    i915_probe_vga.sequence[3] == 3 &&
	    i915_probe_vga.sequence[4] == 4 &&
	    (i915_probe_vga.last_written & 0x20U) != 0U &&
	    plane_off >= 0,
	    "p5a: P5A-VGA legacy IO screen-off first, then VGA_DISP_DISABLE");
}
