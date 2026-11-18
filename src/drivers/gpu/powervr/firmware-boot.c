/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR driver's later stage: loading and starting the firmware.
 *
 * The stage runs on a thread of its own once the early stage has kept the
 * core.  It waits for the root file system, reads the firmware file and
 * keeps it, lays the firmware out in memory the core's MIPS processor
 * maps, builds the records and command buffers the firmware expects,
 * takes the core out of reset, waits for the firmware to say it started,
 * and sends it one health check.  The outcome is one line of the boot
 * log.
 *
 * On any failure the stage stops the firmware, puts the core back in
 * reset, returns every block of memory, and powers the core down.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/sysctl.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/powervr-private.h"
#include "drivers/gpu/powervr/firmware-file.h"
#include "drivers/gpu/powervr/firmware-image.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/mips-table.h"
#include "drivers/gpu/powervr/render-command.h"

/* How long a poll of the firmware sleeps before it looks again, in milliseconds (a tick at least). */
#define BOOT_POLL_MS 1U

/*
 * Where the firmware file is, and the most bytes the driver accepts.
 *
 * Values referenced:
 * Imagination linux-firmware branch powervr 8a58f818
 * powervr/rogue_36.50.54.182_v1.fw (the file's name; it is 128 KiB).
 */
#define FIRMWARE_PATH		"/lib/firmware/powervr/rogue_36.50.54.182_v1.fw"
#define FIRMWARE_LIMIT		0x400000U

/*
 * How long the firmware has to say it started, and how long the stop
 * waits on each unit going idle.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c
 * (FW_BOOT_TIMEOUT_USEC 5 s); pvr_fw_startstop.c (POLL_TIMEOUT_USEC 1 s).
 */
#define BOOT_WAIT_MS		5000U
#define IDLE_WAIT_MS		1000U

/*
 * The pause around taking the firmware processor out of reset, which
 * needs at least 16 core cycles on each side.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_startstop.c
 * (pvr_fw_start: 3 us on each side).
 */
#define RELEASE_PAUSE_US	3U

/*
 * How far the later stage got, so a failure undoes exactly what was done.
 *
 * Each stage includes those before it.
 */
enum late_stage {
	LATE_NOTHING = 0,
	LATE_FILE,
	LATE_MEMORY,
	LATE_INTERRUPT,
	LATE_STARTED
};

static void measure_core_clock(struct powervr_core *core);
static void late_thread(void *argument);
static enum powervr_stage_reason firmware_reason(const char *step, int error);
static int bring_up(struct powervr_core *core, enum late_stage *reached, const char **step);
static int check_processor(const struct powervr_fw_image *image, unsigned *bus_bits);
static int lay_out(struct powervr_core *core, unsigned bus_bits);
static int make_program(struct powervr_core *core);
static int load_program(struct powervr_core *core);
static int make_command_buffers(struct powervr_core *core);
static int make_records(struct powervr_core *core);
static void fill_records(struct powervr_core *core);
static void fill_init_records(struct powervr_core *core);
static int install_interrupt(struct powervr_core *core);
static void remove_interrupt(struct powervr_core *core);
static void interrupt_handler(int irq, kern_irq_ack_t acknowledge, void *argument);
static int start_processor(struct powervr_core *core);
static void fence_reset(struct powervr_core *core, int second_reset);
static void init_cache(struct powervr_core *core);
static void init_wrapper(struct powervr_core *core);
static int wait_started(struct powervr_core *core);
static int health_check(struct powervr_core *core);
static void stop_processor(struct powervr_core *core);
static int wait_register(struct powervr_core *core, uint32_t offset, uint64_t mask, uint64_t value, int wide);
static void tear_down(struct powervr_core *core, enum late_stage reached);
static void free_blocks(struct powervr_core *core);
static void free_file(struct powervr_core *core);
static uint64_t ms_deadline(unsigned milliseconds);
static const char *probe_owner(const struct powervr_firmware *firmware, uint32_t heap_offset);
static void dump_boot_trace(struct powervr_core *core);
static void dump_trace(const struct powervr_firmware *firmware, uint32_t first, uint32_t count);
static void register_check(struct powervr_core *core);
static void register_map(struct powervr_core *core);
static void step_start(const char *name);
static void step_done(const char *name, int error);
static void watch_thread(void *argument);

/* How often a step that has not ended is reported again. */
#define WATCH_REPORT_MS		5000U

/*
 * The trace the firmware keeps, for the dump of a command it did not
 * execute: its main group on, the last words printed.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
 * (ROGUE_FWIF_LOG_TYPE_TRACE 0x1, ROGUE_FWIF_LOG_TYPE_GROUP_MAIN 0x2,
 * and every group from GROUP_MAIN 0x2 to GROUP_MISC 0x4000 (MTS,
 * CLEANUP, CSW, BIF, PM, RTD, SPM, POW, HWR, HWP, RPM, DMA), the debug
 * group 0x80000000 on only for the full trace; struct rogue_fwif_tracebuf_space:
 * trace_pointer 0, the assert buffer at 16 with path 200 bytes, info 200
 * bytes and the line; struct rogue_fwif_osdata: interrupt_count 560,
 * kccb_cmds_executed 568; struct rogue_fwif_sysdata: pow_state 8;
 * struct rogue_fwif_sysinit: firmware_started_timestamp 360;
 * struct rogue_fwif_osinit: the hardware and firmware identities at 40
 * and 56, each its layout and BVNC) and pvr_rogue_fwif_check.h.
 */
/*
 * The trace's groups the firmware logs in normal running: the
 * trace on (bit 0) with the recovery group (HWR, 0x400) alone.  Every group
 * (0x7fff) made about ninety entries a render, and their writing doubled
 * the firmware's time around each job on the board (the TA's
 * share 639 to 250 us, the 3D's 608 to 276 us without them).  The groups a
 * render logs on every job (MAIN, MTS, PM, RTD, BIF, POW) are for a
 * diagnosis build: set 0x7fff here, or a full trace
 * (POWERVR_RENDER_CHECK_FULL_TRACE, POWERVR_FIRMWARE_BOOT_TRACE).  A
 * release image (POWERVR_DEBUG 0) keeps no trace at all.
 */
#ifndef POWERVR_FIRMWARE_TRACE_GROUPS
#define POWERVR_FIRMWARE_TRACE_GROUPS	(POWERVR_DEBUG ? 0x0401U : 0U)
#endif
#define DUMP_TRACE_LOG_TYPE		(POWERVR_RENDER_CHECK_FULL_TRACE || POWERVR_FIRMWARE_BOOT_TRACE ? 0x80007fffU : POWERVR_FIRMWARE_TRACE_GROUPS)
#define BOOT_TRACE_WORDS		2048U
#define DUMP_TRACE_POINTER		8U
#define DUMP_ASSERT_PATH		24U
#define DUMP_ASSERT_INFO		224U
#define DUMP_ASSERT_LINE		424U
#define DUMP_ASSERT_TEXT		200U
/* How much of the trace a dump prints: most for the full trace, more with the diagnostics. */
#if POWERVR_RENDER_CHECK_FULL_TRACE
#define DUMP_TRACE_LAST_WORDS		512U
#define DUMP_TRACE_ALL_WORDS		2048U
#define DUMP_TRACE_HEAD_WORDS		1536U
#elif POWERVR_DIAGNOSTICS
#define DUMP_TRACE_LAST_WORDS		128U
#define DUMP_TRACE_ALL_WORDS		1536U
#define DUMP_TRACE_HEAD_WORDS		256U
#else
#define DUMP_TRACE_LAST_WORDS		64U
#define DUMP_TRACE_ALL_WORDS		128U
#define DUMP_TRACE_HEAD_WORDS		32U
#endif
/* The most words the trace since a mark prints (powervr_firmware_trace_since_mark). */
#define TRACE_SINCE_MARK_WORDS		4096U
#define DUMP_OS_DATA_INTERRUPTS		560U
#define DUMP_SYSTEM_DATA_POWER		8U
#define DUMP_SYSTEM_INIT_STARTED_TIME	360U

/*
 * The processor's page table entry, read back for the probe: the frame
 * field over a wide bus and its shift, the valid bit, and how many pages
 * the probe names at most.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_mips.h
 * (ROGUE_MIPSFW_ENTRYLO_PFN_SHIFT 6, ROGUE_MIPSFW_ENTRYLO_PFN_MASK_ABOVE_32BIT
 * 0x3FFFFFC0, ROGUE_MIPSFW_ENTRYLO_VALID_EN 0x2, the 4 KiB page).
 */
#define PROBE_FRAME_MASK		0x3fffffc0U
#define PROBE_FRAME_SHIFT		6U
#define PROBE_PAGE_SHIFT		12U
#define PROBE_ENTRY_VALID		0x2U
#define PROBE_FOUND_MAX			8U
#define PROBE_OBJECTS			20U

/* The register the host's write and read back is checked on, and the pattern (a page address). */
#define REGISTER_CHECK_CATALOGUE	0x1238U
#define REGISTER_CHECK_PATTERN		0x0000000012345000ULL
#define REGISTER_CHECK_PAGE		0x1000U

/*
 * The core's clock control and status, and every clock field of the
 * control set to on (01 in each two-bit field of the full mask).
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/rgx_cr_defs_km.h
 * (RGX_CR_CLK_CTRL 0x0000 with RGX_CR_CLK_CTRL__PBE2_XE__MASKFULL
 * 0xFFFFFF003F3FFFFF, each field OFF 0, ON 1, AUTO 2; RGX_CR_CLK_STATUS
 * 0x0008).
 */
/* The dust power event register and the event bits its completion sets (sources: see start_processor). */
#define DUST_POWER_EVENT		0x0038U
#define DUST_POWER_EVENT_UP_ALL		0xff000703U
#define DUST_EVENT_STATUS		0x0130U
#define DUST_EVENT_CLEAR		0x0138U
#define DUST_EVENT_POWER_COMPLETE	0x00000400U
#define DUST_EVENT_POWER_ABORT		0x00000800U
/*
 * 1 asks the host to power the dusts up itself at the start and leaves the
 * firmware's management of their power off (no POW_RASCALDUST), so they
 * stay on between jobs (the firmware turns them off after every job and
 * on again for the next); 0 leaves their power to the firmware.  The
 * host's power up completes only while the PMU's hardware turn-on event
 * of the GPU (bit 7 of its mask at 0x4) is unmasked.
 */
/*
 * Whether the firmware processor caches its own code and data sections.
 * Both were uncached, as mainline Linux maps them, and the
 * firmware's work around each job (about 1 ms on the board) is far more
 * than the core's; the host writes them only while the processor is held
 * in reset (the load and the restart), whose reset empties its caches.
 * The code cached (read only) halved that work on the board (the TA's
 * share 639 to 296 us, the 3D's 608 to 357 us, with every trace group on);
 * the data cached is a trial (the firmware may have the core read some of
 * it, which a write-back cache would leave stale).
 */
#ifndef POWERVR_FIRMWARE_CODE_CACHED
#define POWERVR_FIRMWARE_CODE_CACHED	1
#endif
#ifndef POWERVR_FIRMWARE_DATA_CACHED
#define POWERVR_FIRMWARE_DATA_CACHED	0
#endif

/*
 * The trials of the vendor driver's settings the board's dump showed: how
 * long the firmware waits, idle, before it may power the core down (the
 * active power management's latency; the vendor's 100 ms, ours 0 as
 * mainline), and the system level cache's address decoding (the vendor's
 * register read 64-byte interleaving, 0x3 in SLC_CTRL_MISC, where
 * mainline writes the PVR hash).
 */
#ifndef POWERVR_APM_LATENCY_MS
#define POWERVR_APM_LATENCY_MS		0U
#endif
#ifndef POWERVR_SLC_DECODE_INTERLEAVED
#define POWERVR_SLC_DECODE_INTERLEAVED	0
#endif

#ifndef DUST_POWER_BY_HOST
#define DUST_POWER_BY_HOST		0
#endif

/*
 * Whether the start clears every event before the processor leaves
 * reset (1): EVENT_STATUS read ISP_END_MACROTILE (bit 4) from the
 * firmware's first interrupt on, where the vendor firmware on Linux
 * reads 0 when idle.  The vendor driver's start writes no event
 * register; this is an experiment.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/rgx_cr_defs_km.h
 * (RGX_CR_EVENT_CLEAR 0x0138, RGX_CR_EVENT_STATUS_ISP_END_MACROTILE_EN
 * 0x10).
 */
#ifndef POWERVR_CLEAR_EVENTS_AT_START
#define POWERVR_CLEAR_EVENTS_AT_START	0
#endif
#define EVENT_STATUS_REGISTER		0x0130U

/*
 * Whether the start measures the core clock with the core's timer (1)
 * and logs it; a diagnostic that delays the first jobs by a second.
 */
#ifndef POWERVR_MEASURE_CORE_CLOCK
#define POWERVR_MEASURE_CORE_CLOCK	0
#endif
#define CORE_TIMER_REGISTER		0x0160U
#define CORE_TIMER_MASK			0x0000FFFFFFFFFFFFULL
#define CORE_TIMER_CYCLES		256U
#define CORE_TIMER_MS			1000U
#define EVENT_CLEAR_ALL			0xffffffffU

#define REGISTER_CHECK_CLOCK_CONTROL	0x0000U
#define REGISTER_CHECK_CLOCK_STATUS	0x0008U
#define REGISTER_CHECK_ALL_ON		(0x5555555555555555ULL & 0xFFFFFF003F3FFFFFULL)

/*
 * The step the later stage is in, for its log lines and for the thread
 * that reports a step taking long: the name (NULL between steps), the
 * tick it began, and whether the stage has ended.
 */
static const char *step_name;

/* Where the trace was when a kick was about to go, for the dump of what followed. */
static uint32_t trace_mark;
static int trace_marked;
static uint64_t step_began;
static int stage_ended;

/*
 * Starts the later stage on a thread of its own: it waits for the root
 * file system, loads the firmware and starts it.
 */
int
powervr_late_start(
	struct powervr_core *core)
{
	struct thread *thread;
	int error;

	/* Creates the thread; nobody joins it, and its end releases it. */
	error = kthread_create(late_thread, core, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;

	/* Makes it runnable: kthread_create leaves a new thread stopped. */
	thread_start(thread);

	/* Succeeded: the stage runs on its own from here. */
	return 0;
}

/* Runs the later stage and reports its outcome on one line. */
static void
late_thread(
	void *argument)
{
	struct powervr_core *core;
	struct powervr_fw_image *image;
	struct thread *watcher;
	enum late_stage reached;
	const char *step;
	unsigned replies;
	int error;

	/* The core the early stage kept. */
	core = argument;
	reached = LATE_NOTHING;
	step = "file";

	/* The thread that reports a step taking long; the stage goes on without it. */
	__atomic_store_n(&stage_ended, 0, __ATOMIC_RELEASE);
	error = kthread_create(watch_thread, NULL, SCHED_PRIORITY_DEFAULT, &watcher);
	if (error != 0) {
		kern_logf("powervr: firmware: no thread to report long steps\n");
	} else {
		thread_start(watcher);
	}

	/* Reads the file once the root is live, and keeps it for later resets. */
	step_start("file");
	error = powervr_firmware_read(FIRMWARE_PATH, FIRMWARE_LIMIT, &core->firmware.file, &core->firmware.file_size);
	step_done("file", error);
	if (error == 0) {
		reached = LATE_FILE;

		/* Brings the firmware up from the kept file. */
		error = bring_up(core, &reached, &step);
	}

	/* On failure undoes what was done and powers the core down. */
	if (error != 0) {
		tear_down(core, reached);
		kern_logf("powervr: firmware %s failed (%d); powered down\n", step, error);
		powervr_stage_fail(POWERVR_STAGE_FIRMWARE, firmware_reason(step, error));
		__atomic_store_n(&stage_ended, 1, __ATOMIC_RELEASE);
		kern_gpu_attach_end();
		return;
	}

	/* Takes any replies the firmware queued meanwhile. */
	replies = powervr_notices_take(core, NULL, NULL);

	/* Reports the running firmware on one line. */
	image = &core->firmware.image;
	kern_logf("powervr: firmware %u.%u build %u running, health check ok, %u interrupts, %u replies\n",
		  (unsigned)image->version_major,
		  (unsigned)image->version_minor,
		  (unsigned)image->version_build,
		  (unsigned)core->firmware.interrupts,
		  replies);
	if (POWERVR_MEASURE_CORE_CLOCK)
		measure_core_clock(core);
	powervr_stage_ok(POWERVR_STAGE_FIRMWARE);

	/* Publishes the render node over the running firmware; on failure the core goes down the same way. */
	step_start("render node");
	error = powervr_render_start(core);
	step_done("render node", error);
	__atomic_store_n(&stage_ended, 1, __ATOMIC_RELEASE);
	if (error != 0) {
		tear_down(core, LATE_STARTED);
		powervr_render_abandon(core);
		kern_logf("powervr: render node failed (%d); powered down\n", error);
		powervr_stage_fail(POWERVR_STAGE_RENDER_NODE, powervr_stage_reason_of(error));
		kern_gpu_attach_end();
		return;
	}

	/* The render node is published: the GPU no longer counts as attaching. */
	powervr_stage_ok(POWERVR_STAGE_RENDER_NODE);
	kern_gpu_attach_end();
}

/*
 * The stage line's reason for a firmware that did not come up at step:
 * an image that does not parse is "bad image", otherwise the error's own.
 */
static enum powervr_stage_reason
firmware_reason(
	const char *step,
	int error)
{
	int image_step;

	/* The image step fails only on what the file holds. */
	image_step = kern_strcmp(step, "image") == 0;
	if (image_step && error != ENOENT && error != ETIMEDOUT)
		return POWERVR_REASON_BAD_IMAGE;

	/* Otherwise the error says why. */
	return powervr_stage_reason_of(error);
}

/*
 * Starts the firmware again from the kept file after a reset: stops the
 * processor, returns the firmware's memory, powers the core off and on,
 * and brings the firmware up as at boot.  On failure the core is left
 * powered down with nothing kept.
 */
int
powervr_firmware_restart(
	struct powervr_core *core)
{
	enum late_stage reached;
	const char *step;
	int error;

	/* Stops the processor and the interrupt, and returns the firmware's memory. */
	stop_processor(core);
	remove_interrupt(core);
	free_blocks(core);

	/* Powers the core off and on, its domain, clocks and resets with it. */
	powervr_power_down(core);
	error = powervr_power_up(core);
	if (error != 0) {
		free_file(core);
		return error;
	}

	/* Brings the firmware up from the kept file, without the root file system. */
	reached = LATE_FILE;
	error = bring_up(core, &reached, &step);
	if (error != 0) {
		tear_down(core, reached);
		return error;
	}

	/* Takes any replies the restarted firmware queued. */
	(void)powervr_notices_take(core, NULL, NULL);

	/* Succeeded: the firmware runs and answers again. */
	return 0;
}

/*
 * Brings the firmware up from the kept file: checks it, lays it out,
 * starts it and checks it answers.
 *
 * Reached and step report how far it got and which step failed.
 */
static int
bring_up(
	struct powervr_core *core,
	enum late_stage *reached,
	const char **step)
{
	struct powervr_firmware *firmware;
	uint64_t bvnc;
	unsigned bus_bits;
	int error;

	/* Checks the file is for this core and describes it. */
	firmware = &core->firmware;
	*step = "image";
	bvnc = powervr_bvnc_pack(core->branch, core->version, core->units, core->config);
	step_start("image");
	error = powervr_fw_image_parse(firmware->file, firmware->file_size, bvnc, &firmware->image);
	step_done("image", error);
	if (error != 0)
		return error;

	/* Checks the firmware runs on a MIPS processor on a bus wider than 32 bits. */
	step_start("processor");
	error = check_processor(&firmware->image, &bus_bits);
	step_done("processor", error);
	if (error != 0)
		return error;

	/* A core with multicore support runs each render on one core. */
	powervr_render_command_single_core(powervr_fw_image_has_feature(&firmware->image, POWERVR_FEATURE_GPU_MULTICORE));

	/* Lays out the program, the command buffers and the records. */
	*step = "memory";
	*reached = LATE_MEMORY;
	step_start("memory");
	error = lay_out(core, bus_bits);
	step_done("memory", error);
	if (error != 0)
		return error;

	/* Installs the handler of the firmware's interrupt. */
	*step = "interrupt";
	step_start("interrupt");
	error = install_interrupt(core);
	step_done("interrupt", error);
	if (error != 0)
		return error;
	*reached = LATE_INTERRUPT;

	/* Takes the core out of reset; the processor runs from here. */
	*step = "start";
	step_start("start");
	error = start_processor(core);
	step_done("start", error);
	*reached = LATE_STARTED;
	if (error != 0)
		return error;

	/* Waits for the firmware to say it started. */
	*step = "boot";
	step_start("boot");
	error = wait_started(core);
	step_done("boot", error);
	if (error != 0)
		return error;

	/* Sends one health check and waits for the firmware to execute it. */
	*step = "health check";
	step_start("health check");
	error = health_check(core);
	step_done("health check", error);
	if (error != 0)
		return error;

	/* The firmware's trace from its start, when asked. */
	if (POWERVR_FIRMWARE_BOOT_TRACE)
		dump_boot_trace(core);

	/* Succeeded: the firmware runs and answers. */
	return 0;
}

/* Checks the firmware is for a MIPS processor on a wide bus, and reports the bus width. */
static int
check_processor(
	const struct powervr_fw_image *image,
	unsigned *bus_bits)
{
	uint64_t width;
	int mips;
	int error;

	/* Refuses firmware for another processor. */
	mips = powervr_fw_image_has_feature(image, POWERVR_FEATURE_MIPS);
	if (!mips)
		return ENOEXEC;

	/*
	 * Refuses a bus of 32 bits or less, on which the MIPS processor's
	 * table takes another form the driver does not build.
	 *
	 * Procedure referenced:
	 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_mips.c
	 * (pvr_mips_wrapper_init: MIPS firmware only on a bus wider than 32
	 * bits).
	 */
	error = powervr_fw_image_feature_value(image, POWERVR_FEATURE_PHYS_BUS_WIDTH, &width);
	if (error != 0)
		return ENOEXEC;
	if (width <= 32U || width > 40U)
		return ENOEXEC;

	/* Succeeded: the bus width the table entries use. */
	*bus_bits = (unsigned)width;
	return 0;
}

/* Lays out the program, the command buffers and the records in the firmware's heap. */
static int
lay_out(
	struct powervr_core *core,
	unsigned bus_bits)
{
	int error;

	/* Makes the heap's empty page table. */
	error = powervr_heap_init(&core->firmware.heap, bus_bits, core->description.dma_limit);
	if (error != 0)
		return error;

	/* Places the code and the data where the firmware expects them, and loads the program. */
	error = make_program(core);
	if (error != 0)
		return error;

	/* Makes the kernel's and the firmware's command buffers. */
	error = make_command_buffers(core);
	if (error != 0)
		return error;

	/* Makes the records the firmware reads at boot and keeps up to date. */
	error = make_records(core);
	if (error != 0)
		return error;

	/* Fills the records, which name each other and the command buffers. */
	fill_records(core);

	/* Succeeded: the firmware's memory is ready. */
	return 0;
}

/*
 * Places the code at the bottom of the heap and the data where the
 * private data section names, then loads the program into them.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c (pvr_fw_process: the
 * code at offset 0 of the heap, the data at the private data section's
 * base for a processor with a fixed data address, both uncached to the
 * processor).
 */
static int
make_program(
	struct powervr_core *core)
{
	enum powervr_mips_caching caching;
	struct powervr_firmware *firmware;
	const struct powervr_fw_section *private_data;
	const struct powervr_fw_section *boot_code;
	const struct powervr_fw_section *exceptions;
	uint32_t data_offset;
	int error;

	/* Refuses a layout without code or data. */
	firmware = &core->firmware;
	if (firmware->image.code_size == 0U || firmware->image.data_size == 0U)
		return EINVAL;

	/* Refuses a layout that lacks a section the boot needs. */
	private_data = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_PRIVATE);
	boot_code = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_BOOT_CODE);
	exceptions = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_EXCEPTIONS);
	if (private_data == NULL || boot_code == NULL || exceptions == NULL)
		return EINVAL;

	/* Refuses boot code or exception vectors outside the code. */
	if (boot_code->memory != POWERVR_MEMORY_CODE || exceptions->memory != POWERVR_MEMORY_CODE)
		return EINVAL;

	/* The data's place is its private section's base within the heap. */
	data_offset = private_data->base & (POWERVR_HEAP_BYTES - 1U);

	/* The code goes first, at the bottom of the heap, cached by the processor when chosen. */
	caching = POWERVR_MIPS_UNCACHED;
	if (POWERVR_FIRMWARE_CODE_CACHED)
		caching = POWERVR_MIPS_CACHED;
	error = powervr_object_create(&firmware->heap, firmware->image.code_size, 0, caching, &firmware->code);
	if (error != 0)
		return error;

	/* The data goes at its fixed place, likewise. */
	caching = POWERVR_MIPS_UNCACHED;
	if (POWERVR_FIRMWARE_DATA_CACHED)
		caching = POWERVR_MIPS_CACHED;
	error = powervr_object_create(&firmware->heap, firmware->image.data_size, data_offset, caching, &firmware->data);
	if (error != 0)
		return error;

	/* Loads the program and the boot loader's parameters. */
	error = load_program(core);
	if (error != 0)
		return error;

	/* Succeeded: the program is in place. */
	return 0;
}

/*
 * Loads the program from the kept file into the code and data blocks,
 * then gives the boot loader its parameters.
 *
 * The program is placed in kernel memory first, because the sections are
 * not in file order, and copied to the blocks through their views.
 */
static int
load_program(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;
	const struct powervr_fw_section *boot_data;
	const struct powervr_fw_section *stack;
	uint64_t table_pages[POWERVR_MIPS_TABLE_PAGES];
	uint8_t *code;
	uint8_t *data;
	uint32_t page;
	int error;

	/* Finds the boot data page and the stack, both of which live in the data. */
	firmware = &core->firmware;
	boot_data = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_BOOT_DATA);
	stack = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_STACK);
	if (boot_data == NULL || stack == NULL)
		return EINVAL;
	if (boot_data->memory != POWERVR_MEMORY_DATA || stack->memory != POWERVR_MEMORY_DATA)
		return EINVAL;

	/* Takes a cleared buffer for the code. */
	code = kern_malloc(firmware->image.code_size);
	if (code == NULL)
		return ENOMEM;
	kern_memset(code, 0, firmware->image.code_size);

	/* Takes a cleared buffer for the data. */
	data = kern_malloc(firmware->image.data_size);
	if (data == NULL) {
		kern_free(code);
		return ENOMEM;
	}
	kern_memset(data, 0, firmware->image.data_size);

	/* Places the program's segments, then copies both buffers to the blocks. */
	error = powervr_fw_image_load(&firmware->image, code, data);
	if (error == 0) {
		powervr_object_fill(&firmware->code, 0, code, firmware->image.code_size);
		powervr_object_fill(&firmware->data, 0, data, firmware->image.data_size);
	}
	kern_free(data);
	kern_free(code);
	if (error != 0)
		return error;

	/* Names the stack, the core's registers and the page table to the boot loader. */
	for (page = 0; page < POWERVR_MIPS_TABLE_PAGES; page++)
		table_pages[page] = powervr_object_physical(&firmware->heap.table, page * POWERVR_MIPS_PAGE_BYTES);
	powervr_mips_boot_parameters(firmware->data.view + boot_data->alloc_offset,
				     powervr_object_physical(&firmware->data, stack->alloc_offset),
				     core->description.registers,
				     table_pages);

	/* Succeeded: the program and its parameters are in place. */
	return 0;
}

/*
 * Makes the kernel's command buffer with its return slots, and the
 * firmware's command buffer, each with its control block.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_ccb.c (pvr_ccb_init: both
 * offsets zero, the wrap mask one less than the power-of-two slot count,
 * the command size; all of it uncached).
 */
static int
make_command_buffers(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;
	struct powervr_heap *heap;
	uint32_t slot;
	int error;

	/* The kernel's buffer's control block. */
	firmware = &core->firmware;
	heap = &firmware->heap;
	error = powervr_object_create(heap, POWERVR_CCB_CONTROL_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->kccb_control);
	if (error != 0)
		return error;

	/* The kernel's buffer. */
	error = powervr_object_create(heap, POWERVR_KERNEL_CCB_SLOTS * POWERVR_COMMAND_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->kccb);
	if (error != 0)
		return error;

	/* One return slot per command of the kernel's buffer. */
	error = powervr_object_create(heap, POWERVR_KERNEL_CCB_SLOTS * 4U, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->kccb_returns);
	if (error != 0)
		return error;

	/* The firmware's buffer's control block. */
	error = powervr_object_create(heap, POWERVR_CCB_CONTROL_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->fwccb_control);
	if (error != 0)
		return error;

	/* The firmware's buffer. */
	error = powervr_object_create(heap, POWERVR_FIRMWARE_CCB_SLOTS * POWERVR_COMMAND_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->fwccb);
	if (error != 0)
		return error;

	/* Fills the kernel's control block: empty, its slot count and command size. */
	powervr_object_put32(&firmware->kccb_control, POWERVR_CCB_WRITE, 0);
	powervr_object_put32(&firmware->kccb_control, POWERVR_CCB_READ, 0);
	powervr_object_put32(&firmware->kccb_control, POWERVR_CCB_WRAP_MASK, POWERVR_KERNEL_CCB_SLOTS - 1U);
	powervr_object_put32(&firmware->kccb_control, POWERVR_CCB_COMMAND_BYTES, POWERVR_COMMAND_BYTES);

	/* Fills the firmware's control block the same way. */
	powervr_object_put32(&firmware->fwccb_control, POWERVR_CCB_WRITE, 0);
	powervr_object_put32(&firmware->fwccb_control, POWERVR_CCB_READ, 0);
	powervr_object_put32(&firmware->fwccb_control, POWERVR_CCB_WRAP_MASK, POWERVR_FIRMWARE_CCB_SLOTS - 1U);
	powervr_object_put32(&firmware->fwccb_control, POWERVR_CCB_COMMAND_BYTES, POWERVR_COMMAND_BYTES);
	firmware->fwccb_read = 0;
	firmware->kccb_sent = 0;
	for (slot = 0; slot < POWERVR_KERNEL_CCB_SLOTS; slot++)
		powervr_object_put32(&firmware->kccb_returns, slot * 4U, POWERVR_RETURN_MARKER(slot));
	firmware->return_fallback_noted = false;

	/* Succeeded: both buffers are empty. */
	return 0;
}

/*
 * Makes the records the firmware reads at boot and keeps up to date.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c
 * (pvr_fw_create_structures and pvr_fw_create_fwif_connection_ctl: which
 * records exist, every one uncached; the connection control, the per-OS
 * and the system initialisation records at their fixed places) and
 * pvr_fw_trace.c (pvr_fw_trace_init: one trace buffer for the one
 * thread).
 */
static int
make_records(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;
	struct powervr_heap *heap;
	int error;

	/* The connection control, at its fixed place. */
	firmware = &core->firmware;
	heap = &firmware->heap;
	error = powervr_object_create(heap, POWERVR_CONNECTION_BYTES, POWERVR_HEAP_CONNECTION_OFFSET, POWERVR_MIPS_UNCACHED, &firmware->connection);
	if (error != 0)
		return error;

	/* The power synchronisation word the firmware reports power changes in. */
	error = powervr_object_create(heap, 4U, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->power_sync);
	if (error != 0)
		return error;

	/* The recovery information buffer. */
	error = powervr_object_create(heap, POWERVR_HWR_INFO_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->hwr_info);
	if (error != 0)
		return error;

	/* The word an MMU cache flush is synchronised on. */
	error = powervr_object_create(heap, 4U, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->mmu_sync);
	if (error != 0)
		return error;

	/* The system data the firmware keeps. */
	error = powervr_object_create(heap, POWERVR_SYSTEM_DATA_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->system_data);
	if (error != 0)
		return error;

	/* The page the core reads in place of a faulting one. */
	error = powervr_object_create(heap, POWERVR_MIPS_PAGE_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->fault_page);
	if (error != 0)
		return error;

	/* The utilisation buffer. */
	error = powervr_object_create(heap, POWERVR_UTILISATION_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->utilisation);
	if (error != 0)
		return error;

	/* The runtime configuration. */
	error = powervr_object_create(heap, POWERVR_RUNTIME_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->runtime);
	if (error != 0)
		return error;

	/* The one thread's trace buffer. */
	error = powervr_object_create(heap, POWERVR_TRACE_WORDS * 4U, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->trace_buffer);
	if (error != 0)
		return error;

	/* The trace control block. */
	error = powervr_object_create(heap, POWERVR_TRACE_CONTROL_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->trace_control);
	if (error != 0)
		return error;

	/* The per-OS data the firmware keeps. */
	error = powervr_object_create(heap, POWERVR_OS_DATA_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &firmware->os_data);
	if (error != 0)
		return error;

	/* The per-OS initialisation record, at its fixed place. */
	error = powervr_object_create(heap, POWERVR_OS_INIT_BYTES, POWERVR_HEAP_OS_INIT_OFFSET, POWERVR_MIPS_UNCACHED, &firmware->os_init);
	if (error != 0)
		return error;

	/* The system initialisation record, at its fixed place. */
	error = powervr_object_create(heap, POWERVR_SYSTEM_INIT_BYTES, POWERVR_HEAP_SYSTEM_INIT_OFFSET, POWERVR_MIPS_UNCACHED, &firmware->system_init);
	if (error != 0)
		return error;

	/* Succeeded: every record exists, cleared. */
	return 0;
}

/*
 * Fills the records, which name each other and the command buffers.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c (fw_osdata_init,
 * fw_fault_page_init, fw_sysdata_init, fw_runtime_cfg_init and
 * fw_gpu_util_fwcb_init: the values each record starts with) and
 * pvr_fw_trace.c (tracebuf_ctrl_init: tracing off, the buffer's address
 * and size).
 */
static void
fill_records(
	struct powervr_core *core)
{
	uint32_t os_flags;
	struct powervr_firmware *firmware;
	uint64_t clusters;
	uint64_t slc_kilobytes;
	uint32_t flags;
	uint32_t word;
	int error;

	/* The per-OS data names the power synchronisation word. */
	firmware = &core->firmware;
	powervr_object_put32(&firmware->os_data, POWERVR_OS_DATA_POWER_SYNC, powervr_object_address(&firmware->power_sync, 0));

	/* The fault page reads as a recognisable pattern. */
	for (word = 0; word < POWERVR_MIPS_PAGE_BYTES / 4U; word++)
		powervr_object_put32(&firmware->fault_page, word * 4U, POWERVR_FAULT_PAGE_FILL);

	/* Overlapping data masters needs a big enough system level cache. */
	flags = 0;
	error = powervr_fw_image_feature_value(&firmware->image, POWERVR_FEATURE_SLC_KILOBYTES, &slc_kilobytes);
	if (error != 0 || slc_kilobytes < POWERVR_DM_OVERLAP_MIN_KB)
		flags |= POWERVR_FLAG_NO_DM_OVERLAP;

	/*
	 * The data masters' overlap (one context's geometry while another's
	 * 3D runs) can be turned off with POWERVR_DM_OVERLAP 0.  The stale
	 * catalogue the board showed with several sessions (2026-10-21) was
	 * the render target halves, not the overlap.
	 */
	if (!POWERVR_DM_OVERLAP)
		flags |= POWERVR_FLAG_NO_DM_OVERLAP;

	/*
	 * The firmware manages the power of the Rascal and the dusts.  On the
	 * JH7110 their island is off after reset and no one else turns it on,
	 * so without this every data master kick locks up without a fault.
	 *
	 * Values referenced:
	 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_fwif.h
	 * (ROGUE_FWIF_INICFG_POW_RASCALDUST, bit 4); Dominique Belhachemi,
	 * "drm/imagination: re-program PDS/USC exec bases after POW_RASCALDUST
	 * wipeout" (github.com/domibel/linux commit b53c4500f133, the flag
	 * set for the VisionFive 2).
	 */
	if (!DUST_POWER_BY_HOST)
		flags |= POWERVR_FLAG_POW_RASCALDUST;

	/*
	 * The context switch: off by default, as the mainline driver leaves it
	 * for this (mainline) firmware, so the firmware never preempts one
	 * context's job for another's; with POWERVR_CONTEXT_SWITCH the vendor
	 * driver's medium profile and every data master's switch.  On the
	 * board two sessions rendering at once locked the 3D up with the
	 * switch on (2026-10-21).
	 *
	 * Procedure referenced:
	 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c (the system
	 * configuration flags: no context switch profile, the OS flags not
	 * set).
	 */
	os_flags = 0U;
	if (POWERVR_CONTEXT_SWITCH) {
		flags |= POWERVR_FLAG_CSW_PROFILE_MEDIUM;
		os_flags = POWERVR_OS_FLAGS_CSW_ALL;
	}
	powervr_object_put32(&firmware->system_data, POWERVR_SYSTEM_DATA_FLAGS, flags);
	powervr_object_put32(&firmware->os_data, POWERVR_OS_DATA_CONFIG_FLAGS, os_flags);
	if (POWERVR_DEBUG)
		kern_logf("powervr: firmware: system flags 0x%08x, os flags 0x%08x, core clock %u Hz\n", (unsigned)flags, (unsigned)os_flags, (unsigned)core->core_rate);

	/*
	 * The runtime configuration: the core clock's real rate, every
	 * cluster on, no active power management and no watchdog.
	 */
	clusters = 1;
	(void)powervr_fw_image_feature_value(&firmware->image, POWERVR_FEATURE_NUM_CLUSTERS, &clusters);
	powervr_object_put32(&firmware->runtime, POWERVR_RUNTIME_APM_LATENCY, POWERVR_APM_LATENCY_MS);
	powervr_object_put32(&firmware->runtime, POWERVR_RUNTIME_PERSISTENT, 1);
	powervr_object_put32(&firmware->runtime, POWERVR_RUNTIME_CORE_CLOCK, (uint32_t)core->core_rate);
	powervr_object_put32(&firmware->runtime, POWERVR_RUNTIME_CLUSTERS, (uint32_t)clusters);
	powervr_object_put32(&firmware->runtime, POWERVR_RUNTIME_WATCHDOG, 0);

	/* The trace: every group but the debug one on, the one thread's buffer named and sized. */
	powervr_object_put32(&firmware->trace_control, POWERVR_TRACE_LOG_TYPE, DUMP_TRACE_LOG_TYPE);
	powervr_object_put32(&firmware->trace_control, POWERVR_TRACE_BUFFER_ADDRESS, powervr_object_address(&firmware->trace_buffer, 0));
	powervr_object_put32(&firmware->trace_control, POWERVR_TRACE_WORDS_FIELD, POWERVR_TRACE_WORDS);

	/* The two initialisation records name everything else. */
	fill_init_records(core);
}

/*
 * Fills the per-OS and the system initialisation records.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw.c (fw_osinit_init and
 * fw_sysinit_init: no work estimation buffer, no counter dump, no
 * alignment checks, no performance tracing, the started flag clear and
 * the marker 1).
 */
static void
fill_init_records(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;
	struct powervr_object *os_init;
	struct powervr_object *system_init;

	/* The per-OS record names the command buffers, the recovery buffer and the per-OS data. */
	firmware = &core->firmware;
	os_init = &firmware->os_init;
	powervr_object_put32(os_init, POWERVR_OS_INIT_KCCB_CONTROL, powervr_object_address(&firmware->kccb_control, 0));
	powervr_object_put32(os_init, POWERVR_OS_INIT_KCCB, powervr_object_address(&firmware->kccb, 0));
	powervr_object_put32(os_init, POWERVR_OS_INIT_KCCB_RETURNS, powervr_object_address(&firmware->kccb_returns, 0));
	powervr_object_put32(os_init, POWERVR_OS_INIT_FWCCB_CONTROL, powervr_object_address(&firmware->fwccb_control, 0));
	powervr_object_put32(os_init, POWERVR_OS_INIT_FWCCB, powervr_object_address(&firmware->fwccb, 0));
	powervr_object_put32(os_init, POWERVR_OS_INIT_HWR_INFO, powervr_object_address(&firmware->hwr_info, 0));
	powervr_object_put32(os_init, POWERVR_OS_INIT_OS_DATA, powervr_object_address(&firmware->os_data, 0));

	/* The identity checks carry their layout version; the firmware fills the identities. */
	powervr_object_put32(os_init, POWERVR_OS_INIT_HW_IDENTITY, POWERVR_IDENTITY_LAYOUT);
	powervr_object_put32(os_init, POWERVR_OS_INIT_FW_IDENTITY, POWERVR_IDENTITY_LAYOUT);

	/* The system record names the fault page by its physical address and the execution bases. */
	system_init = &firmware->system_init;
	powervr_object_put64(system_init, POWERVR_SYSTEM_INIT_FAULT_PAGE, powervr_object_physical(&firmware->fault_page, 0));
	powervr_object_put64(system_init, POWERVR_SYSTEM_INIT_PDS_BASE, POWERVR_PDS_HEAP_BASE);
	powervr_object_put64(system_init, POWERVR_SYSTEM_INIT_USC_BASE, POWERVR_USC_HEAP_BASE);

	/* It names the other records by their firmware addresses. */
	powervr_object_put32(system_init, POWERVR_SYSTEM_INIT_RUNTIME, powervr_object_address(&firmware->runtime, 0));
	powervr_object_put32(system_init, POWERVR_SYSTEM_INIT_TRACE, powervr_object_address(&firmware->trace_control, 0));
	powervr_object_put32(system_init, POWERVR_SYSTEM_INIT_SYSTEM_DATA, powervr_object_address(&firmware->system_data, 0));
	powervr_object_put32(system_init, POWERVR_SYSTEM_INIT_UTILISATION, powervr_object_address(&firmware->utilisation, 0));

	/* The core clock's real rate, the started flag clear and the marker set. */
	powervr_object_put32(system_init, POWERVR_SYSTEM_INIT_CORE_CLOCK, (uint32_t)core->core_rate);
	powervr_object_put32(system_init, POWERVR_SYSTEM_INIT_STARTED, 0);
	powervr_object_put32(system_init, POWERVR_SYSTEM_INIT_MARKER, 1);
}

/* Installs the handler of the firmware's interrupt, with nothing pending. */
static int
install_interrupt(
	struct powervr_core *core)
{
	int error;

	/* Drops an event left from before, so the first one counted is the firmware's. */
	powervr_write32(core, POWERVR_MIPS_IRQ_CLEAR, POWERVR_MIPS_IRQ_EVENT);
	core->firmware.interrupts = 0;

	/* Installs the handler and lets the line through. */
	error = kern_irq_register((int)core->description.interrupt, interrupt_handler, core);
	if (error != 0)
		return error;
	core->firmware.interrupt_installed = 1;
	kern_irq_unmask((int)core->description.interrupt);

	/* Succeeded: the firmware's events are counted. */
	return 0;
}

/* Removes the handler of the firmware's interrupt. */
static void
remove_interrupt(
	struct powervr_core *core)
{
	/* A handler that was never installed has nothing to remove. */
	if (!core->firmware.interrupt_installed)
		return;

	/* Stops the line, then removes the handler. */
	kern_irq_mask((int)core->description.interrupt);
	(void)kern_irq_unregister((int)core->description.interrupt, interrupt_handler, core);
	core->firmware.interrupt_installed = 0;
}

/*
 * Takes the firmware's event and counts it.
 *
 * It reads and clears one register, moves a counter only it writes, and
 * wakes the render node's service; it takes no lock of the firmware's.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_mips.c
 * (pvr_mips_irq_pending and pvr_mips_irq_clear).
 */
static void
interrupt_handler(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct powervr_core *core;
	uint32_t status;

	/* The core the handler was installed for. */
	(void)irq;
	core = argument;

	/*
	 * Clears the event when the firmware raised one.  The count tells the
	 * later stage the interrupt reached the host.
	 */
	status = powervr_read32(core, POWERVR_MIPS_IRQ_STATUS);
	if ((status & POWERVR_MIPS_IRQ_EVENT) != 0U) {
		powervr_write32(core, POWERVR_MIPS_IRQ_CLEAR, POWERVR_MIPS_IRQ_EVENT);
		core->firmware.interrupts++;

		/* Wakes the render node's service, which reads what the firmware wrote. */
		powervr_render_interrupt(core);
	}

	/* Retires the acknowledgement before returning, as every handler must. */
	kern_irq_send_eoi(acknowledge);
}

/*
 * Takes the core out of reset in steps and lets its processor run.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_startstop.c
 * (pvr_fw_start: the secure bus protection off, everything into soft
 * reset, then out of reset in three steps around the cache, wrapper and
 * bus setup, the second reset register with them; a MIPS processor needs
 * no BIF set up).
 */
static int
start_processor(
	struct powervr_core *core)
{
	const struct powervr_fw_image *image;
	uint64_t everything;
	int second_reset;
	int pbe2;
	int secure;
	int error;

	/* Chooses the reset masks the core's features call for. */
	image = &core->firmware.image;
	pbe2 = powervr_fw_image_has_feature(image, POWERVR_FEATURE_PBE2_IN_XE);
	second_reset = powervr_fw_image_has_feature(image, POWERVR_FEATURE_XE_TPU2);
	secure = powervr_fw_image_has_feature(image, POWERVR_FEATURE_SECURE_RESET);
	everything = POWERVR_SOFT_RESET_ALL;
	if (pbe2)
		everything = POWERVR_SOFT_RESET_ALL_PBE2;

	/* Turns off the secure bus protection that blocks the setup; the read fences the write. */
	if (secure) {
		powervr_write32(core, POWERVR_SYSTEM_BUS_SECURE, 0);
		(void)powervr_read32(core, POWERVR_SYSTEM_BUS_SECURE);
	}

	/* Puts everything in soft reset. */
	powervr_write64(core, POWERVR_SOFT_RESET, everything);
	if (second_reset)
		powervr_write64(core, POWERVR_SOFT_RESET2, POWERVR_SOFT_RESET2_ALL);
	fence_reset(core, second_reset);

	/*
	 * Powers the Rascal and the dusts up by hand before they leave reset.
	 * On the JH7110 their power island stays off after reset and nothing
	 * else turns it on (the firmware is not asked to manage dust power),
	 * so every data master kick locks up without a fault.
	 *
	 * Values referenced:
	 * Icenowy Zheng, "hack to power up rascal+dust" (2024-07-28,
	 * github.com/Icenowy/linux commit 09db317c9c22, pvr_fw_start):
	 * register 0x0038 written 0xff000703, then EVENT_STATUS polled for
	 * POWER_COMPLETE, then 0x0038 written 0 and the event cleared.
	 * StarFive linux (vf2-6.12.y) rgx_cr_defs_km.h: RGX_CR_EVENT_STATUS
	 * 0x0130, RGX_CR_EVENT_CLEAR 0x0138, POWER_COMPLETE_EN 0x00000400.
	 */
	if (DUST_POWER_BY_HOST) {
		kern_logf("powervr: firmware: start: dust power up\n");
		powervr_write32(core, DUST_POWER_EVENT, DUST_POWER_EVENT_UP_ALL);
		error = wait_register(core, DUST_EVENT_STATUS, DUST_EVENT_POWER_COMPLETE, DUST_EVENT_POWER_COMPLETE, 0);
		kern_logf("powervr: firmware: start: dust power up %s (event status 0x%x)\n",
			  error == 0 ? "complete" : "timed out", (unsigned)powervr_read32(core, DUST_EVENT_STATUS));
		powervr_write32(core, DUST_POWER_EVENT, 0);
		powervr_write32(core, DUST_EVENT_CLEAR, DUST_EVENT_POWER_COMPLETE | DUST_EVENT_POWER_ABORT);
	}

	/* Takes the Rascal and the dusts out of reset. */
	powervr_write64(core, POWERVR_SOFT_RESET, everything ^ POWERVR_SOFT_RESET_RASCAL_DUSTS);
	if (second_reset)
		powervr_write64(core, POWERVR_SOFT_RESET2, 0);
	fence_reset(core, second_reset);

	/* Takes everything out of reset but the Garten, which holds the processor. */
	powervr_write64(core, POWERVR_SOFT_RESET, POWERVR_SOFT_RESET_GARTEN);
	if (second_reset)
		powervr_write64(core, POWERVR_SOFT_RESET2, 0);
	fence_reset(core, second_reset);

	/* Sets up the system level cache, the MIPS wrapper and the bus attributes. */
	if (POWERVR_DEBUG)
		kern_logf("powervr: firmware: start: out of reset but the Garten; cache\n");
	init_cache(core);

	/* The diagnostics: the registers' values after the cache is set up. */
	if (POWERVR_DIAGNOSTICS)
		register_check(core);

	if (POWERVR_DEBUG)
		kern_logf("powervr: firmware: start: MIPS wrapper\n");
	init_wrapper(core);
	powervr_write64(core, POWERVR_AXI_ACE_LITE, POWERVR_AXI_ACE_LITE_VALUE);

	/* The events cleared before the processor runs, when asked; the status said once before and after. */
	if (POWERVR_CLEAR_EVENTS_AT_START) {
		kern_logf("powervr: firmware: start: event status 0x%x before the clear\n", (unsigned)powervr_read32(core, EVENT_STATUS_REGISTER));
		powervr_write32(core, DUST_EVENT_CLEAR, EVENT_CLEAR_ALL);
		kern_logf("powervr: firmware: start: event status 0x%x after the clear\n", (unsigned)powervr_read32(core, EVENT_STATUS_REGISTER));
	}

	/* Lets the processor out of reset with a pause on each side. */
	if (POWERVR_DEBUG)
		kern_logf("powervr: firmware: start: processor out of reset (started flag before 0x%x)\n",
			  (unsigned)powervr_object_get32(&core->firmware.system_init, POWERVR_SYSTEM_INIT_STARTED));
	kern_usleep_range(RELEASE_PAUSE_US, RELEASE_PAUSE_US * 2U);
	powervr_write64(core, POWERVR_SOFT_RESET, 0);
	(void)powervr_read64(core, POWERVR_SOFT_RESET);
	kern_usleep_range(RELEASE_PAUSE_US, RELEASE_PAUSE_US * 2U);

	/* Succeeded: the processor runs the boot loader. */
	return 0;
}

/*
 * Reads the reset registers back, so the writes leave the register
 * interface before the next step.
 */
static void
fence_reset(
	struct powervr_core *core,
	int second_reset)
{
	/* Reads the first register, and the second when the core has one. */
	(void)powervr_read64(core, POWERVR_SOFT_RESET);
	if (second_reset)
		(void)powervr_read64(core, POWERVR_SOFT_RESET2);
}

/*
 * Sets the system level cache's address decoding, keeping its hazard
 * check, and bypasses the burst combiner for short cache lines.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_startstop.c
 * (rogue_slc_init: only the low 32 bits are written; the lazy write-back
 * override of BRN 71242 applies only without multicore support).
 */
static void
init_cache(
	struct powervr_core *core)
{
	const struct powervr_fw_image *image;
	uint64_t line_bits;
	uint32_t control;
	int error;

	/* Keeps the hazard check and selects the hashed address decoding (or, as the vendor driver's register reads on the board, 64-byte interleaving). */
	image = &core->firmware.image;
	control = powervr_read32(core, POWERVR_SLC_CONTROL) & POWERVR_SLC_HAZARD_CHECK;
	if (!POWERVR_SLC_DECODE_INTERLEAVED)
		control |= POWERVR_SLC_HASH_DECODE;

	/* Bypasses the burst combiner when the cache line is shorter than 1024 bits. */
	error = powervr_fw_image_feature_value(image, POWERVR_FEATURE_SLC_LINE_BITS, &line_bits);
	if (error == 0 && line_bits < POWERVR_SLC_COMBINER_MIN_BITS)
		control |= POWERVR_SLC_BYPASS_COMBINER;

	/* Writes the low half; the high half keeps its reset value. */
	powervr_write32(core, POWERVR_SLC_CONTROL, control);
}

/*
 * Sets up the MIPS wrapper: where the processor sees the core's
 * registers, that it boots in microMIPS, and the remap windows that put
 * its boot code, boot data and exception vectors on their pages.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_mips.c
 * (pvr_mips_wrapper_init: the four remap windows, the fifth only for BRN
 * 63553, the Garten idle bit left to the processor, the debug probe on).
 */
static void
init_wrapper(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;
	const struct powervr_fw_section *boot_code;
	const struct powervr_fw_section *boot_data;
	const struct powervr_fw_section *exceptions;
	uint64_t boot_code_page;
	uint64_t boot_data_page;
	uint64_t exceptions_page;
	int quirk_63553;

	/* Finds the physical pages of the boot code, the boot data and the exception vectors. */
	firmware = &core->firmware;
	boot_code = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_BOOT_CODE);
	boot_data = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_BOOT_DATA);
	exceptions = powervr_fw_image_section(&firmware->image, POWERVR_SECTION_MIPS_EXCEPTIONS);
	boot_code_page = powervr_object_physical(&firmware->code, boot_code->alloc_offset);
	boot_data_page = powervr_object_physical(&firmware->data, boot_data->alloc_offset);
	exceptions_page = powervr_object_physical(&firmware->code, exceptions->alloc_offset);

	/* Puts the register window at its virtual base and boots in microMIPS. */
	powervr_write32(core, POWERVR_MIPS_WRAPPER, (POWERVR_MIPS_REGISTER_WINDOW >> POWERVR_MIPS_REGISTER_ALIGN) | POWERVR_MIPS_MICROMIPS_BOOT);

	/* Remaps the boot code. */
	powervr_write64(core, POWERVR_MIPS_REMAP1_INPUT, POWERVR_MIPS_BOOT_INPUT | POWERVR_MIPS_REMAP_ENABLE);
	powervr_write64(core, POWERVR_MIPS_REMAP1_OUTPUT, powervr_mips_remap_target(boot_code_page));

	/*
	 * Remaps address 0 to the boot code too, so the erratum's stray
	 * accesses there do not run off into unmapped memory.
	 */
	quirk_63553 = powervr_fw_image_has_quirk(&firmware->image, POWERVR_QUIRK_63553);
	if (quirk_63553) {
		powervr_write64(core, POWERVR_MIPS_REMAP5_INPUT, POWERVR_MIPS_REMAP_ENABLE);
		powervr_write64(core, POWERVR_MIPS_REMAP5_OUTPUT, powervr_mips_remap_target(boot_code_page));
	}

	/* Remaps the boot data and the exception vectors. */
	powervr_write64(core, POWERVR_MIPS_REMAP2_INPUT, POWERVR_MIPS_DATA_INPUT | POWERVR_MIPS_REMAP_ENABLE);
	powervr_write64(core, POWERVR_MIPS_REMAP2_OUTPUT, powervr_mips_remap_target(boot_data_page));
	powervr_write64(core, POWERVR_MIPS_REMAP3_INPUT, POWERVR_MIPS_CODE_INPUT | POWERVR_MIPS_REMAP_ENABLE);
	powervr_write64(core, POWERVR_MIPS_REMAP3_OUTPUT, powervr_mips_remap_target(exceptions_page));

	/* Leaves the Garten's idle bit to the processor and turns on its debug probe. */
	powervr_write64(core, POWERVR_GARTEN_WRAPPER, POWERVR_GARTEN_IDLE_BY_FIRMWARE);
	powervr_write32(core, POWERVR_MIPS_DEBUG, 0);
}

/* Waits for the firmware to set its started flag. */
static int
wait_started(
	struct powervr_core *core)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t started;

	/* Polls the flag in short sleeps until it is set or the bound is spent. */
	deadline = ms_deadline(BOOT_WAIT_MS);
	for (;;) {
		started = powervr_object_get32(&core->firmware.system_init, POWERVR_SYSTEM_INIT_STARTED);
		if (started != 0U)
			break;

		/* The bound is spent without the flag. */
		now = sched_ticks();
		if (now >= deadline) {
			kern_logf("powervr: firmware: boot timed out waiting for the started flag (0x%x)\n", (unsigned)started);
			return ETIMEDOUT;
		}

		/* Sleeps a millisecond (a tick at least). */
		sched_sleep(now + kern_ms_to_ticks(BOOT_POLL_MS));
	}

	/* What the firmware wrote at its start: the flag, its time, and the identities it filled. */
	if (POWERVR_DEBUG)
		kern_logf("powervr: firmware: boot: started 0x%x at %u, marker %u, hardware identity %u/0x%x, firmware identity %u/0x%x\n",
			  (unsigned)started,
			  (unsigned)powervr_object_get32(&core->firmware.system_init, DUMP_SYSTEM_INIT_STARTED_TIME),
			  (unsigned)powervr_object_get32(&core->firmware.system_init, POWERVR_SYSTEM_INIT_MARKER),
			  (unsigned)powervr_object_get32(&core->firmware.os_init, POWERVR_OS_INIT_HW_IDENTITY),
			  (unsigned)powervr_object_get32(&core->firmware.os_init, POWERVR_OS_INIT_HW_IDENTITY + 8U),
			  (unsigned)powervr_object_get32(&core->firmware.os_init, POWERVR_OS_INIT_FW_IDENTITY),
			  (unsigned)powervr_object_get32(&core->firmware.os_init, POWERVR_OS_INIT_FW_IDENTITY + 8U));

	/* Succeeded: the firmware runs. */
	return 0;
}

/*
 * Sends one health check through the kernel's command buffer and waits
 * for the firmware's executed count to reach it: on the VF2 its return
 * slot stayed empty though the firmware ran it, and mainline sends it
 * without waiting on the slot.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_ccb.c
 * (pvr_kccb_send_cmd_reserved_powered: the command into the slot at the
 * write offset, its return slot cleared, a barrier, the write offset
 * moved, then a kick of the general purpose data master) and pvr_fw.c
 * (pvr_fw_mts_schedule: the kick ordered after the memory writes).
 */
static int
health_check(
	struct powervr_core *core)
{
	uint32_t command[POWERVR_COMMAND_WORDS];
	uint32_t answer;
	uint32_t index;
	uint32_t slot;
	int error;

	/* The command: its type alone. */
	answer = 0;
	for (index = 0; index < POWERVR_COMMAND_WORDS; index++)
		command[index] = 0;
	command[POWERVR_COMMAND_TYPE / 4U] = POWERVR_COMMAND_HEALTH_CHECK;

	/* Sends it and waits for the return slot to say it was executed. */
	error = powervr_command_send(core, command, &slot);
	if (error != 0)
		return error;
	error = powervr_command_wait_executed(core, slot, POWERVR_COMMAND_WAIT_MS);
	if (error != 0) {
		answer = powervr_object_get32(&core->firmware.kccb_returns, slot * 4U);
		kern_logf("powervr: firmware: health check timed out waiting for the executed count (%d, return 0x%x)\n", error, (unsigned)answer);
		powervr_firmware_dump(core, slot);
		return error;
	}

	/* Succeeded: the firmware executed the command. */
	return 0;
}

/*
 * Stops the firmware's processor: waits, each wait bounded, for the bus
 * and the cache to go idle, then holds the whole core in soft reset.
 *
 * A wait that runs out does not stop the stop; the reset follows anyway,
 * and the power domain goes off after it.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_fw_startstop.c (pvr_fw_stop:
 * the data masters unassociated from the thread, the BIF and SLC status
 * waits, and for a core with the MARS layout the processor's idle bits
 * in place of the sidekick's).
 */
static void
stop_processor(
	struct powervr_core *core)
{
	const struct powervr_fw_image *image;
	uint64_t layout_mars;
	uint64_t everything;
	int xt_top;
	int pbe2;
	int error;

	/* Reads which waits apply to this core. */
	image = &core->firmware.image;
	layout_mars = 0;
	(void)powervr_fw_image_feature_value(image, POWERVR_FEATURE_LAYOUT_MARS, &layout_mars);
	xt_top = powervr_fw_image_has_feature(image, POWERVR_FEATURE_XT_TOP);
	pbe2 = powervr_fw_image_has_feature(image, POWERVR_FEATURE_PBE2_IN_XE);

	/* Unassociates the data masters from the processor's thread. */
	powervr_write32(core, POWERVR_MTS_INTERRUPT_ASSOC, 0);
	powervr_write32(core, POWERVR_MTS_BACKGROUND_ASSOC, 0);

	/* Waits for the MMUs and the outstanding reads to drain. */
	error = wait_register(core, POWERVR_BIF_MMU_STATUS, POWERVR_BIF_MMU_STATUS_MASK, 0, 0);
	if (error == 0)
		error = wait_register(core, POWERVR_BIFPM_MMU_STATUS, POWERVR_BIFPM_MMU_STATUS_MASK, 0, 0);
	if (error == 0 && !xt_top)
		error = wait_register(core, POWERVR_BIF_READS_STATUS, POWERVR_BIF_READS_STATUS_MASK, 0, 0);
	if (error == 0)
		error = wait_register(core, POWERVR_BIFPM_READS_STATUS, POWERVR_BIFPM_READS_STATUS_MASK, 0, 0);
	if (error == 0)
		error = wait_register(core, POWERVR_SLC_STATUS, POWERVR_SLC_STATUS_MASK, 0, 1);

	/* With the MARS layout the processor and its bus arbiter report idle on their own. */
	if (error == 0 && layout_mars != 0U)
		(void)wait_register(core, POWERVR_MARS_IDLE, POWERVR_MARS_IDLE_FIRMWARE, POWERVR_MARS_IDLE_FIRMWARE, 0);

	/* Holds the whole core in soft reset. */
	everything = POWERVR_SOFT_RESET_ALL;
	if (pbe2)
		everything = POWERVR_SOFT_RESET_ALL_PBE2;
	powervr_write64(core, POWERVR_SOFT_RESET, everything);
	(void)powervr_read64(core, POWERVR_SOFT_RESET);
}

/* Waits, bounded, until a register's masked bits read as the value. */
static int
wait_register(
	struct powervr_core *core,
	uint32_t offset,
	uint64_t mask,
	uint64_t value,
	int wide)
{
	uint64_t deadline;
	uint64_t now;
	uint64_t current;

	/* Polls in short sleeps until the bits match or the bound is spent. */
	deadline = ms_deadline(IDLE_WAIT_MS);
	for (;;) {
		/* Reads the register at its width. */
		if (wide) {
			current = powervr_read64(core, offset);
		} else {
			current = powervr_read32(core, offset);
		}

		/* Stops once the masked bits read as asked. */
		if ((current & mask) == value)
			break;

		/* The bound is spent without the bits. */
		now = sched_ticks();
		if (now >= deadline) {
			kern_logf("powervr: firmware: stop timed out waiting for register 0x%x (0x%llx, mask 0x%llx)\n",
				  (unsigned)offset, (unsigned long long)current, (unsigned long long)mask);
			return ETIMEDOUT;
		}

		/* Sleeps a millisecond (a tick at least). */
		sched_sleep(now + kern_ms_to_ticks(BOOT_POLL_MS));
	}

	/* Succeeded: the register reads as asked. */
	return 0;
}

/* Undoes the later stage as far as it got, and powers the core down. */
static void
tear_down(
	struct powervr_core *core,
	enum late_stage reached)
{
	/* Stops a processor that was let out of reset. */
	if (reached >= LATE_STARTED)
		stop_processor(core);

	/* Removes the interrupt handler before the memory it reports on goes. */
	if (reached >= LATE_INTERRUPT)
		remove_interrupt(core);

	/* Returns the firmware's memory and the kept file. */
	free_blocks(core);
	free_file(core);

	/* Puts the resets back, stops the clocks in reverse and turns the domain off. */
	powervr_power_down(core);
}

/* Returns every block and the page table; the kept file stays. */
static void
free_blocks(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;
	struct powervr_heap *heap;

	/* Returns the blocks in the reverse of the order they were made. */
	firmware = &core->firmware;
	heap = &firmware->heap;
	powervr_object_destroy(heap, &firmware->system_init);
	powervr_object_destroy(heap, &firmware->os_init);
	powervr_object_destroy(heap, &firmware->os_data);
	powervr_object_destroy(heap, &firmware->trace_control);
	powervr_object_destroy(heap, &firmware->trace_buffer);
	powervr_object_destroy(heap, &firmware->runtime);
	powervr_object_destroy(heap, &firmware->utilisation);
	powervr_object_destroy(heap, &firmware->fault_page);
	powervr_object_destroy(heap, &firmware->system_data);
	powervr_object_destroy(heap, &firmware->mmu_sync);
	powervr_object_destroy(heap, &firmware->hwr_info);
	powervr_object_destroy(heap, &firmware->power_sync);
	powervr_object_destroy(heap, &firmware->connection);
	powervr_object_destroy(heap, &firmware->fwccb);
	powervr_object_destroy(heap, &firmware->fwccb_control);
	powervr_object_destroy(heap, &firmware->kccb_returns);
	powervr_object_destroy(heap, &firmware->kccb);
	powervr_object_destroy(heap, &firmware->kccb_control);
	powervr_object_destroy(heap, &firmware->data);
	powervr_object_destroy(heap, &firmware->code);
	powervr_heap_fini(heap);
}

/* Returns the kept file. */
static void
free_file(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;

	/* A file that was never read holds nothing. */
	firmware = &core->firmware;
	if (firmware->file != NULL)
		kern_free(firmware->file);
	firmware->file = NULL;
	firmware->file_size = 0;
}

/* Reports the scheduler tick a number of milliseconds from now, at least one tick away. */
static uint64_t
ms_deadline(
	unsigned milliseconds)
{
	uint64_t ticks;

	/* Rounds up, so a short bound still waits one tick. */
	ticks = kern_ms_to_ticks(milliseconds);
	if (ticks == 0U)
		ticks = 1;

	/* Succeeded: the deadline. */
	return sched_ticks() + ticks;
}

/*
 * Logs the rate the core really runs at: the core's timer counts one for
 * every 256 core clocks, read before and after a second of the
 * scheduler's clock.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/rgx_cr_defs_km.h
 * (RGX_CR_TIMER 0x0160, RGX_CR_TIMER_VALUE_CLRMSK 0xFFFF000000000000)
 * and rgxdefs_km.h (RGX_CRTIME_TICK_IN_CYCLES 256).
 */
static void
measure_core_clock(
	struct powervr_core *core)
{
	uint64_t first;
	uint64_t last;
	uint64_t began;
	uint64_t ended;
	uint64_t ticks;
	uint64_t microseconds;
	uint64_t rate;

	/* The timer and the scheduler's clock, a second apart. */
	began = sched_ticks();
	first = powervr_read64(core, CORE_TIMER_REGISTER) & CORE_TIMER_MASK;
	sched_sleep(began + kern_ms_to_ticks(CORE_TIMER_MS));
	last = powervr_read64(core, CORE_TIMER_REGISTER) & CORE_TIMER_MASK;
	ended = sched_ticks();

	/* The core clocks over the time that passed. */
	ticks = ended - began;
	microseconds = kern_ticks_to_us(ticks);
	rate = 0;
	if (microseconds != 0U)
		rate = (last - first) * CORE_TIMER_CYCLES * 1000000U / microseconds;
	kern_logf("powervr: core clock measured %llu Hz (timer %llu over %llu us)\n",
		  (unsigned long long)rate, (unsigned long long)(last - first),
		  (unsigned long long)microseconds);
}

/* Notes that a step of the later stage begins, on one line. */
static void
step_start(
	const char *name)
{
	/* The tick first, so the thread that reports never sees a name with an old tick. */
	__atomic_store_n(&step_began, sched_ticks(), __ATOMIC_RELEASE);
	__atomic_store_n(&step_name, name, __ATOMIC_RELEASE);
	if (POWERVR_DEBUG)
		kern_logf("powervr: firmware: %s start\n", name);
}

/* Notes that a step ended, with how long it took and how it went, on one line. */
static void
step_done(
	const char *name,
	int error)
{
	uint64_t ticks;
	unsigned milliseconds;

	/* The time the step took. */
	ticks = sched_ticks() - __atomic_load_n(&step_began, __ATOMIC_ACQUIRE);
	milliseconds = (unsigned)kern_ticks_to_ms(ticks);
	__atomic_store_n(&step_name, (const char *)NULL, __ATOMIC_RELEASE);

	/* Done, or failed with its error. */
	if (error == 0) {
		if (POWERVR_DEBUG)
			kern_logf("powervr: firmware: %s done (%u ms)\n", name, milliseconds);
	} else {
		kern_logf("powervr: firmware: %s failed (%d) after %u ms\n", name, error, milliseconds);
	}
}

/*
 * Reports, every few seconds, a step of the later stage that has not
 * ended, until the stage ends.  It runs beside the stage, so a step that
 * never returns is still named.
 */
static void
watch_thread(
	void *argument)
{
	const char *name;
	uint64_t began;
	uint64_t now;
	uint64_t reported;
	uint64_t interval;
	unsigned seconds;

	(void)argument;
	interval = kern_ms_to_ticks(WATCH_REPORT_MS);
	reported = 0;
	while (!__atomic_load_n(&stage_ended, __ATOMIC_ACQUIRE)) {
		/* Sleeps a second. */
		sched_sleep(sched_ticks() + kern_ms_to_ticks(1000U));

		/* A step that has run past the next report time is named. */
		name = __atomic_load_n(&step_name, __ATOMIC_ACQUIRE);
		began = __atomic_load_n(&step_began, __ATOMIC_ACQUIRE);
		now = sched_ticks();
		if (name == NULL || now - began < interval) {
			reported = 0;
			continue;
		}
		if (reported != 0 && now - reported < interval)
			continue;
		reported = now;

		/* Names the step and how long it has run. */
		seconds = (unsigned)(kern_ticks_to_ms(now - began) / 1000U);
		kern_logf("powervr: firmware: still in %s after %u s\n", name, seconds);
	}
}

/*
 * Prints what the host and the firmware share about a command the
 * firmware did not execute: the two command buffers' controls, the
 * command and its return slot with their firmware addresses, the
 * scheduler and the processor's registers, the interrupts, the
 * firmware's own counts and power state, an assertion it left, and the
 * last words of its trace.
 */
void
powervr_firmware_dump(
	struct powervr_core *core,
	uint32_t slot)
{
	struct powervr_firmware *firmware;
	struct powervr_object *trace;
	char text[DUMP_ASSERT_TEXT + 1U];
	uint32_t pointer;
	uint32_t first;
	uint32_t count;
	uint32_t word;
	uint32_t index;

	/* The command buffers' controls. */
	firmware = &core->firmware;
	kern_logf("powervr: dump: kccb control at 0x%x: write %u read %u wrap 0x%x size %u; fwccb control at 0x%x: write %u read %u\n",
		  (unsigned)powervr_object_address(&firmware->kccb_control, 0),
		  (unsigned)powervr_object_get32(&firmware->kccb_control, POWERVR_CCB_WRITE),
		  (unsigned)powervr_object_get32(&firmware->kccb_control, POWERVR_CCB_READ),
		  (unsigned)powervr_object_get32(&firmware->kccb_control, POWERVR_CCB_WRAP_MASK),
		  (unsigned)powervr_object_get32(&firmware->kccb_control, POWERVR_CCB_COMMAND_BYTES),
		  (unsigned)powervr_object_address(&firmware->fwccb_control, 0),
		  (unsigned)powervr_object_get32(&firmware->fwccb_control, POWERVR_CCB_WRITE),
		  (unsigned)powervr_object_get32(&firmware->fwccb_control, POWERVR_CCB_READ));

	/* The command in its slot and its return slot, when one is named. */
	if (slot < POWERVR_KERNEL_CCB_SLOTS) {
		kern_logf("powervr: dump: slot %u at 0x%x: 0x%x 0x%x 0x%x 0x%x; return at 0x%x: 0x%x\n",
		  (unsigned)slot,
		  (unsigned)powervr_object_address(&firmware->kccb, slot * POWERVR_COMMAND_BYTES),
		  (unsigned)powervr_object_get32(&firmware->kccb, slot * POWERVR_COMMAND_BYTES),
		  (unsigned)powervr_object_get32(&firmware->kccb, slot * POWERVR_COMMAND_BYTES + 4U),
		  (unsigned)powervr_object_get32(&firmware->kccb, slot * POWERVR_COMMAND_BYTES + 8U),
		  (unsigned)powervr_object_get32(&firmware->kccb, slot * POWERVR_COMMAND_BYTES + 12U),
		  (unsigned)powervr_object_address(&firmware->kccb_returns, slot * 4U),
		  (unsigned)powervr_object_get32(&firmware->kccb_returns, slot * 4U));
	}

	/* The first four return slots, in case the firmware wrote another than the one waited on. */
	kern_logf("powervr: dump: returns 0..3: 0x%x 0x%x 0x%x 0x%x; returns physical 0x%llx\n",
		  (unsigned)powervr_object_get32(&firmware->kccb_returns, 0U),
		  (unsigned)powervr_object_get32(&firmware->kccb_returns, 4U),
		  (unsigned)powervr_object_get32(&firmware->kccb_returns, 8U),
		  (unsigned)powervr_object_get32(&firmware->kccb_returns, 12U),
		  (unsigned long long)powervr_object_physical(&firmware->kccb_returns, 0));

	/* The records' firmware addresses as the per-OS record names them. */
	kern_logf("powervr: dump: os init at 0x%x names kccb control 0x%x kccb 0x%x returns 0x%x fwccb control 0x%x os data 0x%x\n",
		  (unsigned)powervr_object_address(&firmware->os_init, 0),
		  (unsigned)powervr_object_get32(&firmware->os_init, POWERVR_OS_INIT_KCCB_CONTROL),
		  (unsigned)powervr_object_get32(&firmware->os_init, POWERVR_OS_INIT_KCCB),
		  (unsigned)powervr_object_get32(&firmware->os_init, POWERVR_OS_INIT_KCCB_RETURNS),
		  (unsigned)powervr_object_get32(&firmware->os_init, POWERVR_OS_INIT_FWCCB_CONTROL),
		  (unsigned)powervr_object_get32(&firmware->os_init, POWERVR_OS_INIT_OS_DATA));

	/* The scheduler's and the processor's registers. */
	kern_logf("powervr: dump: mts schedule 0x%x interrupt assoc 0x%x background assoc 0x%x; soft reset 0x%llx; mips wrapper 0x%x garten 0x%llx; mips irq status 0x%x\n",
		  (unsigned)powervr_read32(core, POWERVR_MTS_SCHEDULE),
		  (unsigned)powervr_read32(core, POWERVR_MTS_INTERRUPT_ASSOC),
		  (unsigned)powervr_read32(core, POWERVR_MTS_BACKGROUND_ASSOC),
		  (unsigned long long)powervr_read64(core, POWERVR_SOFT_RESET),
		  (unsigned)powervr_read32(core, POWERVR_MIPS_WRAPPER),
		  (unsigned long long)powervr_read64(core, POWERVR_GARTEN_WRAPPER),
		  (unsigned)powervr_read32(core, POWERVR_MIPS_IRQ_STATUS));

	/* The interrupts the host took and the firmware's own counts and power state. */
	kern_logf("powervr: dump: host interrupts %u; firmware interrupts %u, kccb executed %u, power state %u; started 0x%x at %u\n",
		  (unsigned)firmware->interrupts,
		  (unsigned)powervr_object_get32(&firmware->os_data, DUMP_OS_DATA_INTERRUPTS),
		  (unsigned)powervr_object_get32(&firmware->os_data, POWERVR_OS_DATA_KCCB_EXECUTED),
		  (unsigned)powervr_object_get32(&firmware->system_data, DUMP_SYSTEM_DATA_POWER),
		  (unsigned)powervr_object_get32(&firmware->system_init, POWERVR_SYSTEM_INIT_STARTED),
		  (unsigned)powervr_object_get32(&firmware->system_init, DUMP_SYSTEM_INIT_STARTED_TIME));

	/* An assertion the firmware left: its file, its text and its line. */
	trace = &firmware->trace_control;
	for (index = 0; index < DUMP_ASSERT_TEXT; index++) {
		word = powervr_object_get32(trace, (DUMP_ASSERT_PATH + index) & ~3U);
		text[index] = (char)(word >> (8U * ((DUMP_ASSERT_PATH + index) & 3U)));
	}
	text[DUMP_ASSERT_TEXT] = '\0';
	kern_logf("powervr: dump: assert path \"%s\" line %u\n", text, (unsigned)powervr_object_get32(trace, DUMP_ASSERT_LINE));
	for (index = 0; index < DUMP_ASSERT_TEXT; index++) {
		word = powervr_object_get32(trace, (DUMP_ASSERT_INFO + index) & ~3U);
		text[index] = (char)(word >> (8U * ((DUMP_ASSERT_INFO + index) & 3U)));
	}
	text[DUMP_ASSERT_TEXT] = '\0';
	kern_logf("powervr: dump: assert info \"%s\"\n", text);

	/* The trace: where the firmware writes next, then the words since the mark (or the last ones), eight a line; a release image keeps none. */
	if (!POWERVR_DEBUG)
		return;
	pointer = powervr_object_get32(trace, DUMP_TRACE_POINTER);
	kern_logf("powervr: dump: trace log type 0x%x, pointer %u of %u, mark %u\n",
		  (unsigned)powervr_object_get32(trace, POWERVR_TRACE_LOG_TYPE), (unsigned)pointer, (unsigned)POWERVR_TRACE_WORDS,
		  (unsigned)trace_mark);
	if (pointer >= POWERVR_TRACE_WORDS)
		pointer = 0;
	if (trace_marked) {
		/* The words since the mark: all of them, or the first and the last when there are many. */
		count = (pointer + POWERVR_TRACE_WORDS - trace_mark) % POWERVR_TRACE_WORDS;
		if (count <= DUMP_TRACE_ALL_WORDS) {
			dump_trace(firmware, trace_mark, count);
		} else {
			dump_trace(firmware, trace_mark, DUMP_TRACE_HEAD_WORDS);
			kern_logf("powervr: dump: trace ... %u words left out\n", (unsigned)(count - DUMP_TRACE_HEAD_WORDS - DUMP_TRACE_LAST_WORDS));
			dump_trace(firmware, (pointer + POWERVR_TRACE_WORDS - DUMP_TRACE_LAST_WORDS) % POWERVR_TRACE_WORDS, DUMP_TRACE_LAST_WORDS);
		}
	} else {
		first = pointer >= DUMP_TRACE_LAST_WORDS ? pointer - DUMP_TRACE_LAST_WORDS : 0;
		dump_trace(firmware, first, pointer - first);
	}
}

/*
 * Looks for where the firmware's answer to a command went, when its
 * return slot reads empty: the return slots' firmware address, the
 * processor's page table entry for it and the physical page that entry
 * names, the page the host maps, the first slots read through the alias
 * and through the cached view, and every mapped page of the heap whose
 * word at the slot's offset reads as executed.
 */
/* Names the firmware record a heap offset lies in, for the probe's lines. */
static const char *
probe_owner(
	const struct powervr_firmware *firmware,
	uint32_t heap_offset)
{
	const struct powervr_object *objects[PROBE_OBJECTS];
	static const char *const names[PROBE_OBJECTS] = {
		"code", "data", "connection", "os init", "system init", "kccb control", "kccb", "kccb returns",
		"fwccb control", "fwccb", "os data", "system data", "power sync", "hwr info", "mmu sync",
		"fault page", "utilisation", "runtime", "trace control", "trace buffer",
	};
	unsigned index;

	/* The records in the order of the names. */
	objects[0] = &firmware->code;
	objects[1] = &firmware->data;
	objects[2] = &firmware->connection;
	objects[3] = &firmware->os_init;
	objects[4] = &firmware->system_init;
	objects[5] = &firmware->kccb_control;
	objects[6] = &firmware->kccb;
	objects[7] = &firmware->kccb_returns;
	objects[8] = &firmware->fwccb_control;
	objects[9] = &firmware->fwccb;
	objects[10] = &firmware->os_data;
	objects[11] = &firmware->system_data;
	objects[12] = &firmware->power_sync;
	objects[13] = &firmware->hwr_info;
	objects[14] = &firmware->mmu_sync;
	objects[15] = &firmware->fault_page;
	objects[16] = &firmware->utilisation;
	objects[17] = &firmware->runtime;
	objects[18] = &firmware->trace_control;
	objects[19] = &firmware->trace_buffer;

	/* The record whose pages hold the offset. */
	for (index = 0; index < PROBE_OBJECTS; index++) {
		if (objects[index]->view != NULL && heap_offset >= objects[index]->heap_offset &&
		    heap_offset - objects[index]->heap_offset < ((objects[index]->size + POWERVR_MIPS_PAGE_BYTES - 1U) & ~(POWERVR_MIPS_PAGE_BYTES - 1U)))
			return names[index];
	}

	/* None of the named records. */
	return "unnamed";
}

void
powervr_firmware_return_probe(
	struct powervr_core *core,
	uint32_t slot)
{
	struct powervr_firmware *firmware;
	const volatile uint32_t *table;
	const volatile uint32_t *cached;
	uint64_t physical;
	uint64_t entry_physical;
	uint32_t entry;
	uint32_t page;
	uint32_t pages;
	uint32_t found;
	uint32_t index;

	/* The slots' firmware address, the table's entry for it and the page it names. */
	firmware = &core->firmware;
	table = (const volatile uint32_t *)firmware->heap.table.view;
	page = firmware->kccb_returns.heap_offset / POWERVR_MIPS_PAGE_BYTES;
	entry = table[page];
	entry_physical = ((uint64_t)(entry & PROBE_FRAME_MASK) >> PROBE_FRAME_SHIFT) << PROBE_PAGE_SHIFT;
	physical = powervr_object_physical(&firmware->kccb_returns, 0);
	kern_logf("powervr: probe: returns at 0x%x (heap offset 0x%x): entry 0x%x names 0x%llx; host page 0x%llx, alias %p\n",
		  (unsigned)powervr_object_address(&firmware->kccb_returns, 0),
		  (unsigned)firmware->kccb_returns.heap_offset,
		  (unsigned)entry,
		  (unsigned long long)entry_physical,
		  (unsigned long long)physical,
		  (const void *)firmware->kccb_returns.view);

	/* The first slots through the alias and through the cached view, its lines dropped first. */
	cached = kern_pmem_to_kernel((kern_physaddr_t)physical);
	if (cached != NULL)
		kern_dcache_invalidate_range((const void *)cached, POWERVR_MIPS_PAGE_BYTES);
	for (index = 0; index < 8U; index += 4U) {
		kern_logf("powervr: probe: slots %u..%u alias %x %x %x %x cached %x %x %x %x\n",
			  (unsigned)index, (unsigned)(index + 3U),
			  (unsigned)powervr_object_get32(&firmware->kccb_returns, index * 4U),
			  (unsigned)powervr_object_get32(&firmware->kccb_returns, (index + 1U) * 4U),
			  (unsigned)powervr_object_get32(&firmware->kccb_returns, (index + 2U) * 4U),
			  (unsigned)powervr_object_get32(&firmware->kccb_returns, (index + 3U) * 4U),
			  cached != NULL ? (unsigned)cached[index] : 0U,
			  cached != NULL ? (unsigned)cached[index + 1U] : 0U,
			  cached != NULL ? (unsigned)cached[index + 2U] : 0U,
			  cached != NULL ? (unsigned)cached[index + 3U] : 0U);
	}

	/* Every mapped page of the heap whose word at the slot's offset reads as executed. */
	pages = POWERVR_HEAP_BYTES / POWERVR_MIPS_PAGE_BYTES;
	found = 0;
	for (page = 0; page < pages && found < PROBE_FOUND_MAX; page++) {
		entry = table[page];
		if ((entry & PROBE_ENTRY_VALID) == 0U)
			continue;
		entry_physical = ((uint64_t)(entry & PROBE_FRAME_MASK) >> PROBE_FRAME_SHIFT) << PROBE_PAGE_SHIFT;
		cached = kern_pmem_to_kernel((kern_physaddr_t)entry_physical);
		if (cached == NULL)
			continue;
		kern_dcache_invalidate_range((const void *)cached, POWERVR_MIPS_PAGE_BYTES);
		if (cached[slot % (POWERVR_MIPS_PAGE_BYTES / 4U)] != POWERVR_RETURN_EXECUTED)
			continue;
		kern_logf("powervr: probe: heap page 0x%x (firmware 0x%x, physical 0x%llx, %s) holds 1 at slot %u\n",
			  (unsigned)(page * POWERVR_MIPS_PAGE_BYTES),
			  (unsigned)powervr_mips_address(page * POWERVR_MIPS_PAGE_BYTES),
			  (unsigned long long)entry_physical,
			  probe_owner(firmware, page * POWERVR_MIPS_PAGE_BYTES),
			  (unsigned)slot);
		found++;
	}
	kern_logf("powervr: probe: %u heap pages hold 1 at slot %u\n", (unsigned)found, (unsigned)slot);
}

/* Notes where the firmware's trace is, so a dump shows what followed. */
void
powervr_firmware_trace_mark(
	struct powervr_core *core)
{
	/* The word the firmware writes next. */
	trace_mark = powervr_object_get32(&core->firmware.trace_control, DUMP_TRACE_POINTER) % POWERVR_TRACE_WORDS;
	trace_marked = 1;
}

/*
 * Prints every word of the firmware's trace from the mark to where the
 * firmware writes next, at most TRACE_SINCE_MARK_WORDS, for the job
 * trace (a dump of only the ring's head and tail leaves out the middle
 * of one job).
 */
void
powervr_firmware_trace_since_mark(
	struct powervr_core *core)
{
	uint32_t pointer;
	uint32_t count;

	/* Nothing marked, nothing to print. */
	if (!trace_marked)
		return;

	/* The words since the mark, the first ones when there are too many. */
	pointer = powervr_object_get32(&core->firmware.trace_control, DUMP_TRACE_POINTER) % POWERVR_TRACE_WORDS;
	count = (pointer + POWERVR_TRACE_WORDS - trace_mark) % POWERVR_TRACE_WORDS;
	if (count > TRACE_SINCE_MARK_WORDS)
		count = TRACE_SINCE_MARK_WORDS;
	kern_logf("powervr: dump: trace since the mark, %u words from %u\n", (unsigned)count, (unsigned)trace_mark);
	dump_trace(&core->firmware, trace_mark, count);
}

/* Prints the trace from its first word to where the firmware writes next, at most BOOT_TRACE_WORDS. */
static void
dump_boot_trace(
	struct powervr_core *core)
{
	uint32_t pointer;
	uint32_t count;

	pointer = powervr_object_get32(&core->firmware.trace_control, DUMP_TRACE_POINTER) % POWERVR_TRACE_WORDS;
	count = pointer < BOOT_TRACE_WORDS ? pointer : BOOT_TRACE_WORDS;
	kern_logf("powervr: boot trace: log type 0x%x, pointer %u, %u words from 0\n",
		  (unsigned)powervr_object_get32(&core->firmware.trace_control, POWERVR_TRACE_LOG_TYPE), (unsigned)pointer, (unsigned)count);
	dump_trace(&core->firmware, 0, count);
}

/* Prints count words of the trace from first, wrapping at its end, eight a line (the last line may be short). */
static void
dump_trace(
	const struct powervr_firmware *firmware,
	uint32_t first,
	uint32_t count)
{
	uint32_t words[8];
	uint32_t done;
	uint32_t index;

	/* Eight words a line, each read at its place in the ring. */
	for (done = 0; done < count; done += 8U) {
		for (index = 0; index < 8U; index++) {
			words[index] = 0;
			if (done + index < count)
				words[index] = powervr_object_get32(&firmware->trace_buffer, ((first + done + index) % POWERVR_TRACE_WORDS) * 4U);
		}
		kern_logf("powervr: dump: trace %5u: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  (unsigned)((first + done) % POWERVR_TRACE_WORDS),
			  (unsigned)words[0], (unsigned)words[1], (unsigned)words[2], (unsigned)words[3],
			  (unsigned)words[4], (unsigned)words[5], (unsigned)words[6], (unsigned)words[7]);
	}
}

/*
 * Checks the host's writes reach the core's registers and read back: the
 * system level cache's control just written, and a page catalogue base
 * no data master uses yet (set 7) written with a pattern, read and
 * cleared again; then the same with every clock of the core forced on,
 * the clocks' control put back after.  A register that reads 0 after a
 * write is not one the host's reads of it can be trusted on; one that
 * reads back only with the clocks on sits behind the core's clock gating.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/rgx_cr_defs_km.h
 * (RGX_CR_BIF_CAT_BASE7 0x1238, address field 12..39).
 */
static void
register_check(
	struct powervr_core *core)
{
	uint64_t clock_control;
	uint64_t catalogue;
	uint64_t forced;
	uint64_t low_first;
	uint64_t high_first;
	uint32_t control;

	/* The clocks' control and status and the soft reset as they stand. */
	clock_control = powervr_read64(core, REGISTER_CHECK_CLOCK_CONTROL);
	kern_logf("powervr: firmware: start: register check: window 0x%llx bytes, clock control 0x%llx, clock status 0x%llx, soft reset 0x%llx\n",
		  (unsigned long long)core->description.register_size,
		  (unsigned long long)clock_control,
		  (unsigned long long)powervr_read64(core, REGISTER_CHECK_CLOCK_STATUS),
		  (unsigned long long)powervr_read64(core, POWERVR_SOFT_RESET));

	/* The cache control as written just before, and the catalogue base written and read back. */
	control = powervr_read32(core, POWERVR_SLC_CONTROL);
	powervr_write64(core, REGISTER_CHECK_CATALOGUE, REGISTER_CHECK_PATTERN);
	catalogue = powervr_read64(core, REGISTER_CHECK_CATALOGUE);
	powervr_write64(core, REGISTER_CHECK_CATALOGUE, 0);
	kern_logf("powervr: firmware: start: register check: slc control 0x%x, catalogue base 7 wrote 0x%llx read 0x%llx\n",
		  (unsigned)control, (unsigned long long)REGISTER_CHECK_PATTERN, (unsigned long long)catalogue);

	/* The same with every clock forced on, then the clocks' control as it was. */
	powervr_write64(core, REGISTER_CHECK_CLOCK_CONTROL, REGISTER_CHECK_ALL_ON);
	(void)powervr_read64(core, REGISTER_CHECK_CLOCK_CONTROL);
	powervr_write64(core, REGISTER_CHECK_CATALOGUE, REGISTER_CHECK_PATTERN);
	catalogue = powervr_read64(core, REGISTER_CHECK_CATALOGUE);
	powervr_write64(core, REGISTER_CHECK_CATALOGUE, 0);
	forced = powervr_read64(core, REGISTER_CHECK_CLOCK_CONTROL);
	kern_logf("powervr: firmware: start: register check: clocks forced on (control reads 0x%llx, status 0x%llx): slc control 0x%x, catalogue base 7 read 0x%llx\n",
		  (unsigned long long)forced,
		  (unsigned long long)powervr_read64(core, REGISTER_CHECK_CLOCK_STATUS),
		  (unsigned)powervr_read32(core, POWERVR_SLC_CONTROL), (unsigned long long)catalogue);
	powervr_write64(core, REGISTER_CHECK_CLOCK_CONTROL, clock_control);

	/* The bus's secure protection, and the catalogue base and the clocks' control written as two 32-bit halves, low then high and high then low. */
	powervr_write32(core, REGISTER_CHECK_CATALOGUE, (uint32_t)REGISTER_CHECK_PATTERN);
	powervr_write32(core, REGISTER_CHECK_CATALOGUE + 4U, (uint32_t)(REGISTER_CHECK_PATTERN >> 32));
	low_first = powervr_read64(core, REGISTER_CHECK_CATALOGUE);
	powervr_write32(core, REGISTER_CHECK_CATALOGUE + 4U, (uint32_t)(REGISTER_CHECK_PATTERN >> 32));
	powervr_write32(core, REGISTER_CHECK_CATALOGUE, (uint32_t)REGISTER_CHECK_PATTERN + REGISTER_CHECK_PAGE);
	high_first = powervr_read64(core, REGISTER_CHECK_CATALOGUE);
	powervr_write64(core, REGISTER_CHECK_CATALOGUE, 0);
	powervr_write32(core, REGISTER_CHECK_CLOCK_CONTROL, (uint32_t)REGISTER_CHECK_ALL_ON);
	powervr_write32(core, REGISTER_CHECK_CLOCK_CONTROL + 4U, (uint32_t)(REGISTER_CHECK_ALL_ON >> 32));
	forced = powervr_read64(core, REGISTER_CHECK_CLOCK_CONTROL);
	powervr_write64(core, REGISTER_CHECK_CLOCK_CONTROL, clock_control);
	kern_logf("powervr: firmware: start: register check: bus secure 0x%x; 32-bit halves: catalogue low first 0x%llx, high first 0x%llx, clock control 0x%llx\n",
		  (unsigned)powervr_read32(core, POWERVR_SYSTEM_BUS_SECURE),
		  (unsigned long long)low_first, (unsigned long long)high_first, (unsigned long long)forced);

	/* A map of the registers that read other than 0, band by band, four to a line. */
	register_map(core);
}

/* Prints the registers of each band that read other than 0, and how many read 0. */
static void
register_map(
	struct powervr_core *core)
{
	static const uint32_t bands[][2] = {
		{ 0x0000U, 0x0200U }, { 0x0800U, 0x0900U }, { 0x0a00U, 0x0c00U }, { 0x1200U, 0x1400U },
		{ 0x3800U, 0x3900U }, { 0xa100U, 0xa110U }, { 0xe100U, 0xe200U },
	};
	uint32_t offsets[4];
	uint64_t values[4];
	uint32_t offset;
	uint64_t value;
	unsigned band;
	unsigned count;
	unsigned zeros;
	unsigned index;

	/* Each band, 64 bits at a time. */
	for (band = 0; band < sizeof(bands) / sizeof(bands[0]); band++) {
		count = 0;
		zeros = 0;
		for (offset = bands[band][0]; offset < bands[band][1]; offset += 8U) {
			value = powervr_read64(core, offset);
			if (value == 0U) {
				zeros++;
				continue;
			}
			offsets[count] = offset;
			values[count] = value;
			count++;
			if (count == 4U) {
				kern_logf("powervr: map: %04x=%llx %04x=%llx %04x=%llx %04x=%llx\n",
					  (unsigned)offsets[0], (unsigned long long)values[0], (unsigned)offsets[1], (unsigned long long)values[1],
					  (unsigned)offsets[2], (unsigned long long)values[2], (unsigned)offsets[3], (unsigned long long)values[3]);
				count = 0;
			}
		}
		for (index = 0; index < count; index++)
			kern_logf("powervr: map: %04x=%llx\n", (unsigned)offsets[index], (unsigned long long)values[index]);
		kern_logf("powervr: map: band %04x-%04x: %u read 0\n", (unsigned)bands[band][0], (unsigned)bands[band][1], zeros);
	}
}
