/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The JH7110's glue for its Wave511 video decoder.
 *
 * The decoder sits in its own power domain, runs on six clocks of the SYS
 * clock generator and is held by five of its reset lines: the core's bus
 * master, bit-stream processor, codec engine and register bus, and the
 * on-chip line memory.  Bringing it up turns the domain on, starts the
 * clocks and releases the resets in the order the device tree lists them;
 * bringing it down holds the resets, stops the clocks and turns the
 * domain off.  The firmware is read from the root file system.  Two more
 * reset lines of the decoder's subsystem, its network-on-chip port and
 * its main block, are outside the tree's list; their state is reported,
 * and they are released only when the firmware does not start without.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/platform/visionfive2/vf2-vpu.h"
#include "drivers/platform/visionfive2/vf2-crg.h"
#include "drivers/platform/visionfive2/vf2-pmu.h"
#include "drivers/gpu/powervr/firmware-file.h"
#include "drivers/vpu/wave5-video.h"
#include "drivers/vpu/wave5.h"
#if CONFIG_DRIVER_VF2_VPU_PROBE
#include "drivers/platform/visionfive2/vf2-vpu-probe.h"
#endif
#include <kern/cdev.h>
#include <kern/file.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/*
 * The decoder's clocks in the SYS clock generator, in the order the
 * device tree lists them, and its reset lines likewise; then the two
 * reset lines the tree leaves out.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (vpu_dec@130a0000: clocks
 * axi_clk, bpu_clk, vce_clk, apb_clk, noc_bus, main_clk; resets rst_axi,
 * rst_bpu, rst_vce, rst_apb, rst_sram),
 * include/dt-bindings/clock/starfive,jh7110-crg.h (JH7110_SYSCLK_WAVE511_AXI
 * 70 to JH7110_SYSCLK_WAVE511_APB 73, JH7110_SYSCLK_VDEC_MAIN 75,
 * JH7110_SYSCLK_NOC_BUS_VDEC_AXI 76) and
 * include/dt-bindings/reset/starfive,jh7110-crg.h
 * (JH7110_SYSRST_NOC_BUS_VDEC_AXI 31, JH7110_SYSRST_WAVE511_AXI 47 to
 * JH7110_SYSRST_WAVE511_APB 50, JH7110_SYSRST_VDEC_MAIN 52,
 * JH7110_SYSRST_AXIMEM0_AXI 53).
 */
static const uint32_t vpu_clocks[] = { 70U, 71U, 72U, 73U, 76U, 75U };
static const uint32_t vpu_resets[] = { 47U, 48U, 49U, 50U, 53U };
static const uint32_t vpu_outer_resets[] = { 31U, 52U };

/* The clocks whose rates are reported: the AXI divider and the clocks above. */
static const uint32_t vpu_rate_clocks[] = { 69U, 70U, 71U, 72U, 73U, 75U, 76U };

/*
 * The decoder's on-chip line memory as the core sees it, and its size.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpu.h
 * (VDI_SRAM_BASE_ADDR 0, VDI_WAVE511_SRAM_SIZE 0x2d000).
 */
#define VPU_SRAM_ADDRESS	0x0U
#define VPU_SRAM_SIZE		0x2d000U

/* The highest address the core's 32-bit bus master names. */
#define VPU_DMA_LIMIT		0xffffffffULL

/* Where the firmware is (the wave511-firmware package's file) and how large it may be. */
#define VPU_FIRMWARE_PATH	"/lib/firmware/wave511/chagall.bin"
#define VPU_FIRMWARE_LIMIT	0x100000U

/* The probe device's number (a major of its own), its name and its text's room. */
#define PROBE_DEVICE_NUMBER	0x001a0000U
#define PROBE_DEVICE_NAME	"vf2-vpu-probe"
#define PROBE_TEXT		(64U * 1024U)

/*
 * How far the bring-up got, so the bring-down undoes exactly the steps
 * done.  Each stage includes those before it.
 */
enum vpu_stage {
	VPU_OFF = 0,
	VPU_POWERED,
	VPU_CLOCKED,
	VPU_RELEASED
};

/*
 * The board's one decoder's glue state.
 *
 * The place is set once by drv_vf2_vpu_attach() before anything else
 * runs.  The lock serializes bring-ups, bring-downs and probe reads; the
 * stage, the outer resets released and the core change only under it.
 */
struct vf2_vpu {
	struct drv_vf2_vpu_place place;
	struct mutex lock;
	enum vpu_stage stage;
	int outer_released;
	struct drv_wave5 *core;
};

/* The board's decoder's glue state; off until a bring-up. */
static struct vf2_vpu board_vpu;

#if CONFIG_DRIVER_VF2_VPU_PROBE
/*
 * The last probe's text, written only by the reader holding the glue's
 * lock, with its length.
 */
static char *probe_text;
static size_t probe_length;
#endif

static int power_up(struct vf2_vpu *vpu);
static void power_down(struct vf2_vpu *vpu);
static void hold_lines(void *glue, unsigned what);
static int release_outer(struct vf2_vpu *vpu);
static void make_clock(uint32_t index, struct drv_vf2_clock *clock);
static void make_reset(uint32_t index, struct drv_vf2_reset *reset);
static void put_text(char *text, size_t capacity, size_t *length, const char *line);
static void describe(char *text, size_t capacity, size_t *length);
static struct drv_wave5 *boot(struct vf2_vpu *vpu, char *text, size_t capacity, size_t *length);
#if CONFIG_DRIVER_VF2_VPU
static void attach_thread(void *argument);
#endif
#if CONFIG_DRIVER_VF2_VPU_PROBE
static ssize_t probe_read(struct file *file, void *buffer, size_t size);

/* The probe device: a read from the start runs the probe. */
static const struct cdev_ops probe_ops = {
	.read = probe_read,
};
#endif

/*
 * Finds the video decoder and starts its bring-up.
 */
int
drv_vf2_vpu_attach(
	const struct drv_fdt *fdt,
	const struct drv_vf2_vpu_place *builtin)
{
	const uint8_t *cells;
	uint32_t length;
	uint32_t node;
	bool enabled;
	int error;
#if CONFIG_DRIVER_VF2_VPU
	struct thread *thread;
#endif

	/* Refuses a missing tree or description. */
	if (fdt == NULL || builtin == NULL)
		return EINVAL;

	/* Maps the power management unit, which turns the decoder's domain on. */
	error = drv_vf2_pmu_init(fdt);
	if (error != 0)
		return error;

	/* Starts from the board's own description of the decoder. */
	board_vpu.place = *builtin;
	(void)mutex_init(&board_vpu.lock, LOCK_RANK_DEVICE, "vf2 vpu");

	/*
	 * Takes the place from a node the tree enables, when there is one.
	 *
	 * Value referenced:
	 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
	 * arch/riscv/boot/dts/starfive/jh7110.dtsi (vpu_dec@130a0000,
	 * compatible "starfive,vdec").
	 */
	enabled = false;
	error = drv_fdt_find_compatible(fdt, "starfive,vdec", DRV_FDT_NO_NODE, &node);
	if (error == 0)
		enabled = drv_fdt_node_enabled(fdt, node);
	if (enabled) {
		error = drv_fdt_reg(fdt, node, 0, &board_vpu.place.registers, &board_vpu.place.register_size);
		if (error != 0)
			return error;
		error = drv_fdt_property(fdt, node, "interrupts", &cells, &length);
		if (error == 0 && length >= 4U)
			board_vpu.place.interrupt = (uint32_t)drv_fdt_cells_value(cells, 0, 1U);
	}

#if CONFIG_DRIVER_VF2_VPU_PROBE
	/* Publishes the probe device; the decoder stays off until it is read. */
	error = cdev_register(PROBE_DEVICE_NAME, (dev_t)PROBE_DEVICE_NUMBER, &probe_ops, NULL);
	if (error != 0)
		kern_logf("vf2-vpu: /dev/%s not published (%d)\n", PROBE_DEVICE_NAME, error);
#endif
#if CONFIG_DRIVER_VF2_VPU

	/* Brings the decoder up on a thread of its own, which waits for the root file system. */
	error = kthread_create(attach_thread, &board_vpu, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;
	thread_start(thread);
#endif

	/* Succeeded: the bring-up is under way or waits for the probe. */
	return 0;
}

/*
 * Powers the decoder, boots its firmware and appends what that showed.
 */
struct drv_wave5 *
drv_vf2_vpu_bring_up(
	char *text,
	size_t capacity,
	size_t *length)
{
	struct drv_wave5 *core;

	/* Brings the decoder up once; a decoder already up is handed back as it is. */
	mutex_lock(&board_vpu.lock);

	core = board_vpu.core;
	if (core == NULL)
		core = boot(&board_vpu, text, capacity, length);

	mutex_unlock(&board_vpu.lock);

	/* Reports the core, or NULL with the failure in the text. */
	return core;
}

/*
 * Stops the decoder, holds it in reset, powers it off and frees the core.
 */
void
drv_vf2_vpu_bring_down(
	struct drv_wave5 *core)
{
	/* Stops the core, then holds it in reset before its memory is freed. */
	mutex_lock(&board_vpu.lock);

	drv_wave5_stop(core);
	power_down(&board_vpu);
	drv_wave5_detach(core);
	if (board_vpu.core == core)
		board_vpu.core = NULL;

	mutex_unlock(&board_vpu.lock);
}

/*
 * Powers the decoder, reads its firmware and boots it, the glue's lock
 * held.  Retries once with the outer resets released when the firmware
 * did not start.
 */
static struct drv_wave5 *
boot(
	struct vf2_vpu *vpu,
	char *text,
	size_t capacity,
	size_t *length)
{
	struct drv_wave5_identity identity;
	struct drv_wave5_place place;
	struct drv_wave5 *core;
	uint8_t *firmware;
	size_t size;
	char line[160];
	int error;

	/* Reads the firmware once the root is live. */
	error = powervr_firmware_read(VPU_FIRMWARE_PATH, VPU_FIRMWARE_LIMIT, &firmware, &size);
	if (error != 0) {
		kern_snprintf(line, sizeof(line), "vpu firmware %s unavailable (%d)\n", VPU_FIRMWARE_PATH, error);
		put_text(text, capacity, length, line);
		return NULL;
	}
	kern_snprintf(line, sizeof(line), "vpu firmware %s bytes=%u\n", VPU_FIRMWARE_PATH, (unsigned)size);
	put_text(text, capacity, length, line);

	/* Powers the decoder and reports its clocks and resets. */
	error = power_up(vpu);
	describe(text, capacity, length);
	if (error != 0) {
		kern_snprintf(line, sizeof(line), "vpu power up failed (%d)\n", error);
		put_text(text, capacity, length, line);
		power_down(vpu);
		kern_free(firmware);
		return NULL;
	}

	/* Describes the core for the decoder's driver. */
	place.registers = vpu->place.registers;
	place.register_size = vpu->place.register_size;
	place.interrupt = vpu->place.interrupt;
	place.dma_limit = VPU_DMA_LIMIT;
	place.sram_address = VPU_SRAM_ADDRESS;
	place.sram_size = VPU_SRAM_SIZE;
	place.lines = hold_lines;
	place.glue = vpu;

	/* Boots the firmware; a start that fails is tried again with the outer resets released. */
	core = NULL;
	error = drv_wave5_attach(&place, firmware, size, &core);
	kern_snprintf(line, sizeof(line), "vpu boot (outer resets as left) %d\n", error);
	put_text(text, capacity, length, line);
	if (error == ETIMEDOUT || error == EIO) {
		error = release_outer(vpu);
		if (error == 0)
			error = drv_wave5_attach(&place, firmware, size, &core);
		kern_snprintf(line, sizeof(line), "vpu boot (outer resets released) %d\n", error);
		put_text(text, capacity, length, line);
	}
	kern_free(firmware);

	/* Powers the decoder off again when it did not boot. */
	if (error != 0) {
		power_down(vpu);
		return NULL;
	}

	/* Reports what runs. */
	drv_wave5_identity(core, &identity);
	kern_snprintf(line,
		      sizeof(line),
		      "vpu product %04x name %s version %08x firmware %08x customer %08x\n",
		      identity.product_code,
		      identity.product_name,
		      identity.product_version,
		      identity.firmware_version,
		      identity.customer);
	put_text(text, capacity, length, line);
	kern_snprintf(line,
		      sizeof(line),
		      "vpu standards %08x %08x features %08x config %08x %08x\n",
		      identity.standards[0],
		      identity.standards[1],
		      identity.features,
		      identity.configuration[0],
		      identity.configuration[1]);
	put_text(text, capacity, length, line);
	kern_logf("vf2-vpu: %s %04x firmware %08x\n", identity.product_name, identity.product_code, identity.firmware_version);

	/* Succeeded: the core runs. */
	vpu->core = core;
	return core;
}

/*
 * Turns the decoder's domain on, starts its clocks and releases its
 * resets.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/media/platform/chips-media/wave5/wave5-vpu.c (wave5_vpu_probe:
 * every clock started, then every reset released) and
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (the power domain and the
 * order of the clocks and resets).
 */
static int
power_up(
	struct vf2_vpu *vpu)
{
	struct drv_vf2_clock clock;
	struct drv_vf2_reset reset;
	unsigned position;
	int error;

	/* Refuses a decoder already up. */
	if (vpu->stage != VPU_OFF)
		return EBUSY;

	/* Turns the decoder's domain on. */
	error = drv_vf2_pmu_set_power(DRV_VF2_PD_VDEC, 1);
	if (error != 0)
		return error;
	vpu->stage = VPU_POWERED;

	/* Starts the clocks. */
	for (position = 0; position < sizeof(vpu_clocks) / sizeof(vpu_clocks[0]); position++) {
		make_clock(vpu_clocks[position], &clock);
		error = drv_vf2_clock_enable(&clock);
		if (error != 0)
			return error;
	}
	vpu->stage = VPU_CLOCKED;

	/* Releases the resets. */
	for (position = 0; position < sizeof(vpu_resets) / sizeof(vpu_resets[0]); position++) {
		make_reset(vpu_resets[position], &reset);
		error = drv_vf2_reset_deassert(&reset);
		if (error != 0)
			return error;
	}
	vpu->stage = VPU_RELEASED;

	/* Succeeded: the decoder is powered, clocked and out of reset. */
	return 0;
}

/*
 * Holds the decoder's resets, stops its clocks and turns its domain off,
 * undoing exactly the steps power_up() and release_outer() got through.
 */
static void
power_down(
	struct vf2_vpu *vpu)
{
	struct drv_vf2_clock clock;
	struct drv_vf2_reset reset;
	unsigned position;

	/* Holds the resets, the outer ones too when they were released here. */
	if (vpu->stage >= VPU_RELEASED) {
		for (position = sizeof(vpu_resets) / sizeof(vpu_resets[0]); position > 0; position--) {
			make_reset(vpu_resets[position - 1U], &reset);
			(void)drv_vf2_reset_assert(&reset);
		}
	}

	if (vpu->outer_released) {
		for (position = 0; position < sizeof(vpu_outer_resets) / sizeof(vpu_outer_resets[0]); position++) {
			make_reset(vpu_outer_resets[position], &reset);
			(void)drv_vf2_reset_assert(&reset);
		}
		vpu->outer_released = 0;
	}

	/* Stops the clocks in reverse. */
	if (vpu->stage >= VPU_POWERED) {
		for (position = sizeof(vpu_clocks) / sizeof(vpu_clocks[0]); position > 0; position--) {
			make_clock(vpu_clocks[position - 1U], &clock);
			(void)drv_vf2_clock_disable(&clock);
		}
	}

	/* Turns the domain off. */
	if (vpu->stage >= VPU_POWERED)
		(void)drv_vf2_pmu_set_power(DRV_VF2_PD_VDEC, 0);

	/* The decoder is off. */
	vpu->stage = VPU_OFF;
}

/*
 * Holds or releases the decoder's resets, or holds them and powers it
 * off, for the core's recovery (wave5.h).  The core calls it with its
 * command lock held, in a decode; the glue's lock is not taken, since a
 * bring-down takes the core's lock under it, and nothing else changes the
 * stage of a decoder the render node's engine holds.
 */
static void
hold_lines(
	void *glue,
	unsigned what)
{
	struct drv_vf2_reset reset;
	struct vf2_vpu *vpu;
	unsigned position;

	/* The decoder whose core asks. */
	vpu = glue;

	/* Off: the resets held, the clocks stopped, the domain off. */
	if (what == DRV_WAVE5_LINES_OFF) {
		power_down(vpu);
		return;
	}

	/* Holds the resets in reverse, or releases them in order. */
	if (what == DRV_WAVE5_LINES_HOLD) {
		for (position = sizeof(vpu_resets) / sizeof(vpu_resets[0]); position > 0; position--) {
			make_reset(vpu_resets[position - 1U], &reset);
			(void)drv_vf2_reset_assert(&reset);
		}
		return;
	}
	for (position = 0; position < sizeof(vpu_resets) / sizeof(vpu_resets[0]); position++) {
		make_reset(vpu_resets[position], &reset);
		(void)drv_vf2_reset_deassert(&reset);
	}
}

/* Releases the two reset lines of the decoder's subsystem the tree leaves out. */
static int
release_outer(
	struct vf2_vpu *vpu)
{
	struct drv_vf2_reset reset;
	unsigned position;
	int error;

	/* Releases each line; power_down() holds them again. */
	vpu->outer_released = 1;
	for (position = 0; position < sizeof(vpu_outer_resets) / sizeof(vpu_outer_resets[0]); position++) {
		make_reset(vpu_outer_resets[position], &reset);
		error = drv_vf2_reset_deassert(&reset);
		if (error != 0)
			return error;
	}

	/* Succeeded: the subsystem's port and main block are out of reset. */
	return 0;
}

/* Appends the domain's state, the clocks' rates and gates and the resets' states. */
static void
describe(
	char *text,
	size_t capacity,
	size_t *length)
{
	struct drv_vf2_clock clock;
	struct drv_vf2_reset reset;
	unsigned position;
	uint64_t rate;
	char line[96];
	int running;
	int released;
	int on;
	int error;

	/* The domain. */
	on = 0;
	(void)drv_vf2_pmu_powered(DRV_VF2_PD_VDEC, &on);
	kern_snprintf(line, sizeof(line), "vpu domain %u on=%d\n", DRV_VF2_PD_VDEC, on);
	put_text(text, capacity, length, line);

	/* Each clock's rate and gate. */
	for (position = 0; position < sizeof(vpu_rate_clocks) / sizeof(vpu_rate_clocks[0]); position++) {
		make_clock(vpu_rate_clocks[position], &clock);
		rate = 0;
		running = 0;
		error = drv_vf2_clock_get_rate(&clock, &rate);
		(void)drv_vf2_clock_running(&clock, &running);
		kern_snprintf(line,
			      sizeof(line),
			      "vpu clock %u rate=%llu running=%d error=%d\n",
			      (unsigned)vpu_rate_clocks[position],
			      (unsigned long long)rate,
			      running,
			      error);
		put_text(text, capacity, length, line);
	}

	/* Each reset line the tree lists and the two it leaves out. */
	for (position = 0; position < sizeof(vpu_resets) / sizeof(vpu_resets[0]); position++) {
		make_reset(vpu_resets[position], &reset);
		released = -1;
		(void)drv_vf2_reset_released(&reset, &released);
		kern_snprintf(line, sizeof(line), "vpu reset %u released=%d\n", (unsigned)vpu_resets[position], released);
		put_text(text, capacity, length, line);
	}

	for (position = 0; position < sizeof(vpu_outer_resets) / sizeof(vpu_outer_resets[0]); position++) {
		make_reset(vpu_outer_resets[position], &reset);
		released = -1;
		(void)drv_vf2_reset_released(&reset, &released);
		kern_snprintf(line, sizeof(line), "vpu outer reset %u released=%d\n", (unsigned)vpu_outer_resets[position], released);
		put_text(text, capacity, length, line);
	}
}

/* Appends one line to a text, dropping what does not fit; a NULL text takes nothing. */
static void
put_text(
	char *text,
	size_t capacity,
	size_t *length,
	const char *line)
{
	size_t size;

	/* A bring-up without a text writes nothing. */
	if (text == NULL || length == NULL)
		return;

	/* Copies the line when it fits with the terminator. */
	size = kern_strlen(line);
	if (*length + size + 1U > capacity)
		return;
	kern_memcpy(text + *length, line, size);
	*length += size;
	text[*length] = '\0';
}

/* Names one of the decoder's clocks in the SYS generator. */
static void
make_clock(
	uint32_t index,
	struct drv_vf2_clock *clock)
{
	/* The SYS generator's clock of that number. */
	clock->crg = DRV_VF2_CRG_SYS;
	clock->index = index;
}

/* Names one of the decoder's reset lines in the SYS generator. */
static void
make_reset(
	uint32_t index,
	struct drv_vf2_reset *reset)
{
	/* The SYS generator's line of that number. */
	reset->crg = DRV_VF2_CRG_SYS;
	reset->index = index;
}

#if CONFIG_DRIVER_VF2_VPU
/* Brings the decoder up at boot and keeps it up. */
static void
attach_thread(
	void *argument)
{
	struct drv_wave5 *core;
	int error;

	/* Boots the decoder; the board goes on without video when it fails. */
	(void)argument;
	core = drv_vf2_vpu_bring_up(NULL, 0, NULL);
	if (core == NULL) {
		kern_logf("vf2-vpu: video decoder unavailable\n");
		return;
	}

	/* The render node's sessions opened from now on decode on it. */
	error = drv_wave5_video_register(core);
	if (error != 0) {
		kern_logf("vf2-vpu: the video decoder is not the render node's engine (%d)\n", error);
		return;
	}

	/* Succeeded: the decoder is the render node's video engine. */
	kern_logf("vf2-vpu: video decoder is the render node's engine\n");
}
#endif

#if CONFIG_DRIVER_VF2_VPU_PROBE
/*
 * Runs the probe on a read from the start, then reads its text from the
 * file's offset on.  Zero at the end.
 */
static ssize_t
probe_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct drv_wave5 *core;
	uint64_t offset;
	size_t chunk;
	int brought;

	/* Refuses a negative offset. */
	if (file->f_offset < 0)
		return -EINVAL;

	/* One reader at a time: the text is the glue's. */
	mutex_lock(&board_vpu.lock);

	if (probe_text == NULL)
		probe_text = kern_malloc(PROBE_TEXT);

	mutex_unlock(&board_vpu.lock);

	/* A read from the start brings the decoder up, probes it and brings it down. */
	if (file->f_offset == 0 && probe_text != NULL) {
		probe_length = 0;
		probe_text[0] = '\0';
		brought = board_vpu.core == NULL;
		core = drv_vf2_vpu_bring_up(probe_text, PROBE_TEXT, &probe_length);
		if (core != NULL) {
			probe_length = drv_vf2_vpu_probe_run(core, probe_text, PROBE_TEXT, probe_length);
			if (brought)
				drv_vf2_vpu_bring_down(core);
		}
	}

	/* The part of the text at the offset. */
	offset = (uint64_t)file->f_offset;
	chunk = 0;
	if (probe_text != NULL && offset < probe_length) {
		chunk = probe_length - (size_t)offset;
		if (chunk > size)
			chunk = size;
		kern_memcpy(buffer, probe_text + offset, chunk);
	}

	/* Moves the file's offset past the bytes read. */
	file->f_offset = (off_t)(offset + chunk);

	/* Succeeded: the bytes read. */
	return (ssize_t)chunk;
}
#endif
