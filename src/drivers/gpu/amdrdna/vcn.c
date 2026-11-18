/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video engine (VCN 2.0): its power, its start and stop, its rings,
 * fences and interrupts, its self-test, and its restart.
 *
 * The engine is a processor of its own that runs a firmware program the
 * security processor placed in the trusted region (stage A3).  The driver
 * powers the engine's tiles itself, without the system-management unit
 * unless the boot line asks for it (amdrdna.vcn.smu=1), lets the
 * processor boot from the program, and gives it three rings: decoding,
 * and general and low-latency encoding.  Every submission ends with a
 * fence the engine writes into the driver's write-back page and an
 * interrupt that wakes the waiters.  The engine starts at the end of
 * stage A5 and tests its rings and its message path; a failure leaves it
 * off without failing the stage.  A wait that runs out hands the engine
 * to a thread of its own, which stops and starts it again outside every
 * other lock.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"
#include "drivers/gpu/amdrdna/amdrdna-vcn.h"

/* The page, and the self-test's three pages: the indirect buffers, the creation and the destruction messages. */
#define VCN_PAGE_BYTES			4096U
#define VCN_TEST_BYTES			(3U * VCN_PAGE_BYTES)
#define VCN_TEST_CREATE_IB_AT		0U
#define VCN_TEST_DESTROY_IB_AT		256U
#define VCN_TEST_CREATE_AT		VCN_PAGE_BYTES
#define VCN_TEST_DESTROY_AT		(2U * VCN_PAGE_BYTES)

/* The fence words of the three rings in the engine's write-back page, 64 bytes apart. */
#define VCN_SLOT_DECODE			0U
#define VCN_SLOT_ENCODE			64U
#define VCN_SLOT_ENCODE_LOW		128U

/* The longest submission the driver writes on a ring at once. */
#define VCN_SUBMIT_WORDS		64U

/*
 * The waits on the engine's registers: polled every 10 microseconds for
 * 100 ms at most.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu.h
 * (AMDGPU_MAX_USEC_TIMEOUT 100000) and soc15_common.h
 * (SOC15_WAIT_ON_RREG: polled for usec_timeout).
 */
#define VCN_REGISTER_POLLS		10000U
#define VCN_REGISTER_STEP_US		10U

/*
 * The processor's boot: its report polled 100 times 10 ms apart, and up
 * to 10 tries, each after a soft reset of the processor held 10 ms.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start: the loops of 10 and 100 and the mdelay(10)).
 */
#define VCN_BOOT_TRIES			10U
#define VCN_BOOT_POLLS			100U
#define VCN_BOOT_STEP_US		10000U

/*
 * The rings' tests: the value the decode ring writes into the ninth
 * scratch register over the CPU's mark, each looked for 100 ms at most.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_dec_ring_test_ring: 0xCAFEDEAD, 0xDEADBEEF).
 */
#define VCN_TEST_MARK			0xcafedeadU
#define VCN_TEST_VALUE			0xdeadbeefU

/*
 * The self-test's session: handle 1, an H.264 stream of 1920 by 1088
 * samples, whose creation and destruction the firmware answers within a
 * second.
 *
 * Values referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c
 * (amdgpu_vcn_dec_get_create_msg: the handle, 0x780 and 0x440).
 */
#define VCN_TEST_HANDLE			1U
#define VCN_TEST_WIDTH			1920U
#define VCN_TEST_HEIGHT			1088U
#define VCN_TEST_FENCE_MS		1000U

/* How often a waiter looks at its fence without an interrupt. */
#define VCN_FENCE_POLL_MS		1U

/* The waits in a row that ran out before the engine stays down until the GPU's reset. */
#define VCN_TIMEOUT_LIMIT		3U

/*
 * The media hub's context a session's run uses.  Only the video engine
 * reaches memory through the media hub, and it runs one session's work
 * at a time, so one context serves every session: each run points it at
 * its session's tables first.
 */
#define VCN_SPACE_CONTEXT		1U

/*
 * The decode sessions the device keeps open at once, and the memory of
 * the engine they may take together (of the 6 GiB of VRAM, the rest
 * stays the desktop's and the other clients').
 */
#define VCN_SESSIONS_MAX		8U
#define VCN_SESSION_BYTES_MAX		(2ULL * 1024U * 1024U * 1024U)

/* The first handle a decode session is created with (the self-test's is 1). */
#define VCN_FIRST_HANDLE		2U

/* How long the stop waits for the restart thread to end, in steps of 10 ms. */
#define VCN_STOP_WAIT_US		5000000U
#define VCN_STOP_STEP_US		10000U

static int vcn_buffer(struct amdrdna_device *device, struct amdrdna_system_buffer *buffer, unsigned bytes, const char *what);
static int vcn_power_up(struct amdrdna_device *device);
static void vcn_power_down(struct amdrdna_device *device);
static int vcn_boot(struct amdrdna_device *device);
static int vcn_boot_gating(struct amdrdna_device *device);
static int vcn_pause(struct amdrdna_device *device, bool pause);
static bool vcn_halt_gating(struct amdrdna_device *device);
static void vcn_clocks_ungated(struct amdrdna_device *device);
static void vcn_clocks_gated(struct amdrdna_device *device);
static void vcn_windows(struct amdrdna_device *device);
static int vcn_processor_wait(struct amdrdna_device *device);
static void vcn_rings(struct amdrdna_device *device);
static void vcn_ring_place(struct amdrdna_device *device, struct amdrdna_vcn_ring *ring, unsigned queue, uint32_t base_low, uint32_t base_high, uint32_t size, uint32_t read, uint32_t write);
static bool vcn_halt(struct amdrdna_device *device);
static int vcn_test_rings(struct amdrdna_device *device);
static int vcn_test_messages(struct amdrdna_device *device);
static int vcn_submit(struct amdrdna_device *device, struct amdrdna_vcn_ring *ring, uint32_t *words, unsigned count, bool fenced, uint32_t *sequence);
static int vcn_wait(struct amdrdna_device *device, struct amdrdna_vcn_ring *ring, uint32_t sequence, unsigned milliseconds);
static int vcn_decode_run(struct amdrdna_device *device, unsigned context, uint64_t address, uint32_t length, unsigned milliseconds, bool starting);
static void vcn_timed_out(struct amdrdna_device *device);
static void vcn_hold(struct amdrdna_device *device);
static int vcn_thread_start(struct amdrdna_device *device);
static void vcn_restart_thread(void *argument);
static bool vcn_restart(struct amdrdna_device *device);
static uint32_t vcn_read_pointer(struct amdrdna_device *device, const struct amdrdna_vcn_ring *ring);
static void vcn_write_pointer(struct amdrdna_device *device, const struct amdrdna_vcn_ring *ring);
static void vcn_doorbells(struct amdrdna_device *device, bool on);
static uint64_t vcn_fence_address(const struct amdrdna_device *device, const struct amdrdna_vcn_ring *ring);
static uint32_t vcn_fence(const struct amdrdna_device *device, const struct amdrdna_vcn_ring *ring);
static int vcn_wait_register(struct amdrdna_device *device, uint32_t offset, uint32_t mask, uint32_t value);
static uint32_t vcn_read(struct amdrdna_device *device, uint32_t offset);
static void vcn_report(struct amdrdna_device *device);
static void vcn_write(struct amdrdna_device *device, uint32_t offset, uint32_t value);
static void vcn_set(struct amdrdna_device *device, uint32_t offset, uint32_t bits);
static void vcn_clear(struct amdrdna_device *device, uint32_t offset, uint32_t bits);

/*
 * Prepares the engine's record at the attach: its locks, its wait queues,
 * and its rings' names, places and doorbells.
 */
void
amdrdna_vcn_init(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;

	/* The locks and the wait queues. */
	vcn = &device->vcn;
	(void)mutex_init(&vcn->run_lock, LOCK_RANK_DEVICE, "amdrdna vcn run");
	(void)mutex_init(&vcn->decode_lock, LOCK_RANK_DEVICE, "amdrdna vcn decode");
	(void)mutex_init(&vcn->encode_lock, LOCK_RANK_DEVICE, "amdrdna vcn encode");
	spin_init(&vcn->lock, LOCK_RANK_DEVICE, "amdrdna vcn");
	waitq_init(&vcn->fences, "amdrdna vcn fence");
	waitq_init(&vcn->restart_waitq, "amdrdna vcn restart");

	/* The decode ring. */
	vcn->decode.name = "vcn-decode";
	vcn->decode.slot = VCN_SLOT_DECODE;
	vcn->decode.doorbell = AMDRDNA_DOORBELL_VIDEO_DECODE;
	vcn->decode.encode = false;

	/* The general encode ring. */
	vcn->encode.name = "vcn-encode";
	vcn->encode.slot = VCN_SLOT_ENCODE;
	vcn->encode.doorbell = AMDRDNA_DOORBELL_VIDEO_ENCODE;
	vcn->encode.encode = true;

	/* The low-latency encode ring, set up but not given work. */
	vcn->encode_low.name = "vcn-encode-low";
	vcn->encode_low.slot = VCN_SLOT_ENCODE_LOW;
	vcn->encode_low.doorbell = AMDRDNA_DOORBELL_VIDEO_ENCODE_LOW;
	vcn->encode_low.encode = true;
}

/*
 * Reads the boot words of the engine: amdrdna.vcn=1 uses it (the default
 * 0 leaves it alone); amdrdna.vcn.static=1 starts it without its dynamic
 * power gating (the default starts it under the gating, the one start the
 * security processor allows when it loaded the firmware);
 * amdrdna.vcn.smu asks the system-management unit to power it (1, the
 * default under the gating) or not (0, the default of the static start);
 * and amdrdna.vcn.doorbell=1 moves the rings' write pointers through
 * doorbells rather than the engine's registers.
 */
void
amdrdna_vcn_parameters(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned word;

	/* Whether the engine is used at all. */
	vcn = &device->vcn;
	word = amdrdna_parameter_number("amdrdna.vcn", 0U);
	vcn->wanted = false;
	if (word == 1U)
		vcn->wanted = true;

	/* Whether it starts under its dynamic power gating. */
	word = amdrdna_parameter_number("amdrdna.vcn.static", 0U);
	vcn->gating = true;
	if (word == 1U)
		vcn->gating = false;

	/* Whether the system-management unit powers it. */
	word = amdrdna_parameter_number("amdrdna.vcn.smu", vcn->gating ? 1U : 0U);
	vcn->power_unit = false;
	if (word == 1U)
		vcn->power_unit = true;

	/* Whether the write pointers go through doorbells. */
	word = amdrdna_parameter_number("amdrdna.vcn.doorbell", 0U);
	vcn->doorbells = false;
	if (word == 1U)
		vcn->doorbells = true;
}

/*
 * Takes the engine's memory while only the CPU changes the system-memory
 * table (before the copy engine runs in stage A5): the processor's stack
 * and context in VRAM only the GPU reaches, and in system memory the
 * shared page, the write-back page, the three rings and the self-test's
 * pages.  Everything is taken once for the record's life and zeroed at
 * each run of the stage.  A failure leaves the engine unprepared, and
 * stage A5 goes on without it.
 */
void
amdrdna_vcn_prepare(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	uint64_t bytes;
	int error;

	/* Nothing to take unless the security processor loaded the firmware. */
	vcn = &device->vcn;
	vcn->prepared = false;
	if (!vcn->loaded)
		return;

	/* The processor's stack and context, next to each other, once. */
	if (vcn->cache_vram == 0U) {
		bytes = (uint64_t)AMDRDNA_VCN_STACK_BYTES + AMDRDNA_VCN_CONTEXT_BYTES;
		error = amdrdna_vram_take(device, false, bytes, VCN_PAGE_BYTES, &vcn->cache_vram);
		if (error != 0) {
			amdrdna_mark("A5 vcn: no VRAM for the processor's stack and context (%d); the video engine stays off", error);
			return;
		}
	}

	/* The pages in system memory. */
	error = vcn_buffer(device, &vcn->shared, VCN_PAGE_BYTES, "shared page");
	if (error == 0)
		error = vcn_buffer(device, &vcn->writeback, VCN_PAGE_BYTES, "write-back page");
	if (error == 0)
		error = vcn_buffer(device, &vcn->decode.buffer, AMDRDNA_VCN_RING_BYTES, "decode ring");
	if (error == 0)
		error = vcn_buffer(device, &vcn->encode.buffer, AMDRDNA_VCN_RING_BYTES, "encode ring");
	if (error == 0)
		error = vcn_buffer(device, &vcn->encode_low.buffer, AMDRDNA_VCN_RING_BYTES, "low-latency encode ring");
	if (error == 0)
		error = vcn_buffer(device, &vcn->test, VCN_TEST_BYTES, "self-test pages");
	if (error == 0)
		error = vcn_buffer(device, &vcn->table, VCN_PAGE_BYTES, "gating's table");
	if (error != 0)
		return;

	/* The rings start empty, their fences from 0 (the write-back page is zero). */
	vcn->decode.write = 0U;
	vcn->decode.sequence = 0U;
	vcn->encode.write = 0U;
	vcn->encode.sequence = 0U;
	vcn->encode_low.write = 0U;
	vcn->encode_low.sequence = 0U;

	/* The start may now run. */
	vcn->prepared = true;
}

/*
 * Starts the engine at the end of stage A5 and tests it: powers it,
 * boots its processor from the placed program, sets its rings up, tests
 * the decode ring, the general encode ring and a session's creation and
 * destruction, and starts the restart thread.  A failure stops the engine
 * again and leaves it off; the stage goes on.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_hw_init, vcn_v2_0_start) and amdgpu_vcn.c
 * (amdgpu_vcn_dec_ring_test_ib, amdgpu_vcn_enc_ring_test_ring).
 */
void
amdrdna_vcn_start(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint32_t index;
	int error;

	/* Nothing to start without the firmware and the memory. */
	vcn = &device->vcn;
	if (!vcn->wanted)
		return;
	if (!vcn->loaded || !vcn->prepared) {
		amdrdna_mark("A5 vcn: not started (firmware %s, memory %s)",
		    vcn->loaded ? "loaded" : "not loaded",
		    vcn->prepared ? "taken" : "not taken");
		return;
	}

	/* The engine's registers must be in the block table. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_VCN, AMDRDNA_VCN_WINDOW_MAIN, AMDRDNA_VCN_REG_STATUS, &index);
	if (error != 0) {
		amdrdna_mark("A5 vcn: no register window in the block table (%d)", error);
		return;
	}

	/* Takes new work again: the GPU's own reset or the first start leaves nothing held. */
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->stopping = false;
	vcn->ready = false;
	vcn->restart_wanted = false;
	vcn->timeouts = 0U;
	vcn->held = false;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Powers the engine and boots its processor. */
	error = vcn_power_up(device);
	if (error == 0)
		error = vcn_boot(device);

	/* Tests the rings, then the message path. */
	if (error == 0)
		error = vcn_test_rings(device);
	if (error == 0)
		error = vcn_test_messages(device);

	/* Starts the thread that restarts the engine after a wait ran out. */
	if (error == 0)
		error = vcn_thread_start(device);

	/* A failure stops the engine again and leaves it off. */
	if (error != 0) {
		amdrdna_mark("A5 vcn fail (%d) status=0x%08x power=0x%08x tiles=0x%08x; the video engine stays off",
		    error,
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_STATUS),
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_POWER_STATUS),
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_TILE_STATE));
		vcn_report(device);
		amdrdna_vcn_stop(device);
		return;
	}

	/* The engine takes work; work begun before this start is known as lost. */
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->ready = true;
	vcn->generation++;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Marks the result. */
	amdrdna_mark("A5 vcn ok fw=0x%08x enc=%u.%u dec=%u start=%s power=%s wptr=%s tmr=0x%llx irq=dec:%llu enc:%llu",
	    (unsigned)vcn->firmware.version_word,
	    vcn->firmware.version.encode_major,
	    vcn->firmware.version.encode_minor,
	    vcn->firmware.version.decode,
	    vcn->gating ? "gating" : "static",
	    vcn->power_unit ? "smu" : "tiles",
	    vcn->doorbells ? "doorbell" : "register",
	    (unsigned long long)vcn->place,
	    (unsigned long long)vcn->decode_interrupts,
	    (unsigned long long)vcn->encode_interrupts);
}

/*
 * Stops the engine: turns new work away, wakes every waiter (which then
 * gives up), ends the restart thread, halts the engine, and powers it
 * down.  When the halt cannot confirm that the engine stopped, the engine
 * is held: the memory its last work pointed at stays the engine's until
 * the GPU is reset.  Running it twice does nothing the second time.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_hw_fini, vcn_v2_0_stop).
 */
void
amdrdna_vcn_stop(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	unsigned waited;
	bool thread_running;
	bool stopped;

	/* Turns new work away and wakes every waiter and the restart thread. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->stopping = true;
	vcn->ready = false;
	vcn->restart_wanted = false;
	waitq_wake_all(&vcn->fences);
	waitq_wake_all(&vcn->restart_waitq);

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Waits for the restart thread to end, for a bounded time. */
	thread_running = false;
	for (waited = 0U; waited < VCN_STOP_WAIT_US; waited += VCN_STOP_STEP_US) {
		/* Reads whether the thread still runs. */
		irq = spin_lock_irqsave(&vcn->lock);

		thread_running = vcn->thread_running;

		spin_unlock_irqrestore(&vcn->lock, irq);

		/* Stops waiting once it has ended. */
		if (!thread_running)
			break;

		/* Gives the thread a step to finish its restart. */
		kern_usleep_range(VCN_STOP_STEP_US, VCN_STOP_STEP_US * 2U);
	}

	/* A thread still restarting the engine owns its registers; the GPU's reset follows anyway. */
	if (thread_running) {
		amdrdna_mark("vcn stop: the restart thread still runs; the engine is held until the GPU is reset");
		vcn_hold(device);
		return;
	}

	/* Halts an engine that runs, with no submission in progress. */
	stopped = true;
	if (vcn->running) {
		mutex_lock(&vcn->decode_lock);
		mutex_lock(&vcn->encode_lock);

		stopped = vcn_halt(device);
		vcn->running = false;

		mutex_unlock(&vcn->encode_lock);
		mutex_unlock(&vcn->decode_lock);

		/* Marks the stop, for the log. */
		amdrdna_mark("vcn stopped (%s), restarts %u", stopped ? "halted" : "not confirmed, held", vcn->restarts);
	}

	/* An engine not seen to stop keeps the memory its work pointed at. */
	if (!stopped)
		vcn_hold(device);

	/* The doorbells and the power, as far as the start set them. */
	vcn_doorbells(device, false);
	vcn_power_down(device);
}

/*
 * Handles an interrupt of the engine: counts it by ring and wakes the
 * waiters of every ring's fence, which then read their fence words.
 */
void
amdrdna_vcn_interrupt(
	struct amdrdna_device *device,
	const struct amdrdna_interrupt *interrupt)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;

	/* Counts the interrupt by its source and wakes the fences' waiters. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	if (interrupt->source == AMDRDNA_SOURCE_VIDEO_DECODE) {
		vcn->decode_interrupts++;
	} else if (interrupt->source == AMDRDNA_SOURCE_VIDEO_ENCODE) {
		vcn->encode_interrupts++;
	} else if (interrupt->source == AMDRDNA_SOURCE_VIDEO_ENCODE_LOW) {
		vcn->encode_interrupts++;
	} else {
		vcn->other_interrupts++;
	}
	waitq_wake_all(&vcn->fences);

	spin_unlock_irqrestore(&vcn->lock, irq);
}

/* Reports whether the engine takes work: it was asked for, and it is up (not stopped, down or restarting). */
bool
amdrdna_vcn_available(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	bool ready;

	/* Read under the engine's lock. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	ready = vcn->wanted && vcn->ready;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Succeeded: the answer. */
	return ready;
}

/*
 * Reports the engine's generation, which grows at each start: a session
 * created in an older one lost its firmware state.
 */
uint32_t
amdrdna_vcn_generation(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint32_t generation;

	/* Read under the engine's lock. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	generation = vcn->generation;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Succeeded: the generation. */
	return generation;
}

/*
 * Runs an indirect buffer of the decode ring in an address space and
 * waits for it, for at most milliseconds: the media hub's video context
 * is pointed at the space's tables and its translations dropped (which
 * also makes what the CPU wrote through its window reach memory), then
 * the buffer runs in that context.  elapsed_us gets the run's time from
 * the submission to the fence.  A run that did not end keeps the space
 * (the engine may still touch it) until the engine is seen to halt, and
 * hands the engine to the restart thread.
 *
 * Returns 0, EIO when the engine takes no work or the context could not
 * be pointed at the space, EFAULT when the media hub reported a page
 * fault during the run, ETIMEDOUT when the run did not end, or
 * ECANCELED when the engine was stopped meanwhile.
 */
int
amdrdna_vcn_space_run(
	struct amdrdna_device *device,
	struct amdrdna_space *space,
	uint64_t address,
	uint32_t words,
	unsigned milliseconds,
	uint64_t *elapsed_us)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint64_t faults;
	uint64_t started_us;
	int error;

	/* Nothing ran yet. */
	vcn = &device->vcn;
	*elapsed_us = 0U;

	/* One session's run at a time, from the context's pointing to the run's end. */
	mutex_lock(&vcn->run_lock);

	/* The video context points at the space's tables, its old translations dropped. */
	amdrdna_hub_context_set(device, AMDRDNA_HUB_MEDIA, VCN_SPACE_CONTEXT, space->root_word);
	error = amdrdna_tlb_flush(device, AMDRDNA_HUB_MEDIA, VCN_SPACE_CONTEXT);
	if (error != 0) {
		mutex_unlock(&vcn->run_lock);
		return EIO;
	}

	/* The media hub's faults so far, so a fault of this run is told apart. */
	irq = spin_lock_irqsave(&vcn->lock);

	faults = vcn->media_faults;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* The run and its wait, timed. */
	started_us = amdrdna_display_now_us();
	error = vcn_decode_run(device, VCN_SPACE_CONTEXT, address, words, milliseconds, false);
	*elapsed_us = amdrdna_display_now_us() - started_us;

	/* A run that did not end keeps its space; one that faulted failed. */
	if (error == ETIMEDOUT)
		amdrdna_space_video_hold(space);
	irq = spin_lock_irqsave(&vcn->lock);

	if (error == 0 && vcn->media_faults != faults)
		error = EFAULT;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* The CPU reads what the engine wrote to VRAM, not what the host path held. */
	amdrdna_host_path_drop(device);
	mutex_unlock(&vcn->run_lock);

	/* Reports a run that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the engine ran the buffer in the space. */
	return 0;
}

/* Counts a page fault of the media hub, which only the video engine's runs reach memory through. */
void
amdrdna_vcn_media_fault(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;

	/* Counted under the engine's lock, where a run reads it. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->media_faults++;

	spin_unlock_irqrestore(&vcn->lock, irq);
}

/*
 * Counts a new decode session of bytes of the engine's memory against
 * the device's limits and gives its handle.
 *
 * Returns 0, EMFILE when the device has its most sessions open, or
 * ENOMEM when the sessions would take more memory than the limit.
 */
int
amdrdna_vcn_session_take(
	struct amdrdna_device *device,
	uint64_t bytes,
	uint32_t *handle)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	int error;

	/* Counted under the engine's lock. */
	vcn = &device->vcn;
	error = 0;
	irq = spin_lock_irqsave(&vcn->lock);

	if (vcn->sessions >= VCN_SESSIONS_MAX) {
		error = EMFILE;
	} else if (bytes > VCN_SESSION_BYTES_MAX - vcn->session_bytes) {
		error = ENOMEM;
	} else {
		vcn->sessions++;
		vcn->session_bytes += bytes;
		if (vcn->next_handle < VCN_FIRST_HANDLE)
			vcn->next_handle = VCN_FIRST_HANDLE;
		*handle = vcn->next_handle;
		vcn->next_handle++;
	}

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Reports a session over the limits. */
	if (error != 0)
		return error;

	/* Succeeded: the session is counted. */
	return 0;
}

/* Gives back a decode session's count and the bytes of the engine's memory it took. */
void
amdrdna_vcn_session_give(
	struct amdrdna_device *device,
	uint64_t bytes)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;

	/* Counted under the engine's lock. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	if (vcn->sessions > 0U)
		vcn->sessions--;
	if (bytes > vcn->session_bytes)
		bytes = vcn->session_bytes;
	vcn->session_bytes -= bytes;

	spin_unlock_irqrestore(&vcn->lock, irq);
}

/*
 * Allocates a buffer of system memory once for the record's life, maps it
 * for the GPU at a page boundary, and zeroes it.
 */
static int
vcn_buffer(
	struct amdrdna_device *device,
	struct amdrdna_system_buffer *buffer,
	unsigned bytes,
	const char *what)
{
	int error;

	/* A buffer kept from an earlier run is only zeroed again. */
	if (!buffer->allocated) {
		error = drv_dma_alloc_coherent(device->dma, bytes, VCN_PAGE_BYTES, &buffer->dma);
		if (error != 0) {
			amdrdna_mark("A5 vcn: %s not allocated (%d); the video engine stays off", what, error);
			return error;
		}

		/* The buffer is the GPU's for the record's life. */
		buffer->allocated = true;

		/* Maps it for the GPU. */
		error = amdrdna_gart_map(device, buffer, VCN_PAGE_BYTES);
		if (error != 0) {
			amdrdna_mark("A5 vcn: %s not mapped (%d); the video engine stays off", what, error);
			return error;
		}
	}

	/* Zeroes it. */
	kern_memset(buffer->dma.address, 0, buffer->dma.size);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Succeeded: the GPU reaches the buffer. */
	return 0;
}

/*
 * Asks the system-management unit to power the engine, when the boot line
 * asks for that way (amdrdna.vcn.smu=1).
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start: amdgpu_dpm_enable_vcn before the start).
 */
static int
vcn_power_up(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	int error;

	/* The tiles alone power the engine unless the unit is asked to. */
	vcn = &device->vcn;
	if (!vcn->power_unit)
		return 0;

	/* Asks the unit. */
	error = amdrdna_power_video(device, true);
	if (error != 0) {
		amdrdna_mark("A5 vcn: the power unit did not power the engine (%d)", error);
		return error;
	}

	/* The stop powers it down again. */
	vcn->powered = true;

	/* Succeeded: the unit powered the engine. */
	return 0;
}

/* Asks the system-management unit to power the engine down, when it powered it. */
static void
vcn_power_down(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	int error;

	/* Nothing to undo when the unit was not asked. */
	vcn = &device->vcn;
	if (!vcn->powered)
		return;

	/* Asks the unit; a refusal is only logged. */
	error = amdrdna_power_video(device, false);
	if (error != 0)
		amdrdna_mark("vcn stop: the power unit did not power the engine down (%d)", error);
	vcn->powered = false;
}

/*
 * Boots the engine without its dynamic power gating: powers every tile,
 * ungates the clocks, sets the processor's clock, the memory interface
 * and the picture cache up, points the processor's windows at the placed
 * program, its stack, its context and the shared page, lets the
 * processor out of its reset, waits for its report, and sets the rings
 * up.
 *
 * Returns 0, ETIMEDOUT when the tiles or the processor do not come up,
 * or ECANCELED when a stop came during the wait.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start, the path without dynamic power gating, and
 * vcn_v2_0_disable_static_power_gating, the path without
 * AMD_PG_SUPPORT_VCN: every tile on).
 */
static int
vcn_boot(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	uint32_t word;
	int error;

	/* The engine's state; the start under the gating is another way. */
	vcn = &device->vcn;
	if (vcn->gating)
		return vcn_boot_gating(device);

	/* Powers every tile and waits until every one reads on. */
	vcn_write(device, AMDRDNA_VCN_REG_TILE_POWER, AMDRDNA_VCN_TILES_ON);
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_TILE_STATE, AMDRDNA_VCN_TILES_MASK, AMDRDNA_VCN_TILES_STATE_ON);
	if (error != 0) {
		amdrdna_mark("A5 vcn: the tiles did not power on (state 0x%08x)", (unsigned)vcn_read(device, AMDRDNA_VCN_REG_TILE_STATE));
		return error;
	}

	/* Clears the power state and the hardware's own gating from the status word. */
	vcn_clear(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_START_CLEAR);

	/*
	 * The busy mark tells the engine the driver is starting it; it is
	 * cleared once the processor runs.
	 */
	vcn_set(device, AMDRDNA_VCN_REG_STATUS, AMDRDNA_VCN_STATUS_BUSY);

	/* The clocks of every unit run. */
	vcn_clocks_ungated(device);

	/* The processor's clock on, its interrupt off until it runs. */
	vcn_set(device, AMDRDNA_VCN_REG_PROCESSOR_CONTROL, AMDRDNA_VCN_PROCESSOR_CLOCK);
	vcn_clear(device, AMDRDNA_VCN_REG_INTERRUPT_ENABLE, AMDRDNA_VCN_INTERRUPT_PROCESSOR);

	/* The memory interface's settings. */
	vcn_set(device, AMDRDNA_VCN_REG_MEMORY_CONTROL, AMDRDNA_VCN_MEMORY_SETTINGS);

	/* The picture cache's replacement mode and its multiplexers. */
	word = vcn_read(device, AMDRDNA_VCN_REG_PICTURE_CONTROL);
	word &= ~AMDRDNA_VCN_PICTURE_REPLACE_MASK;
	word |= AMDRDNA_VCN_PICTURE_REPLACE;
	vcn_write(device, AMDRDNA_VCN_REG_PICTURE_CONTROL, word);
	vcn_write(device, AMDRDNA_VCN_REG_PICTURE_MUX_A, AMDRDNA_VCN_PICTURE_MUX_A);
	vcn_write(device, AMDRDNA_VCN_REG_PICTURE_MUX_B, AMDRDNA_VCN_PICTURE_MUX_B);
	vcn_write(device, AMDRDNA_VCN_REG_PICTURE_MUX, AMDRDNA_VCN_PICTURE_MUX);

	/* The processor's windows on its program, stack, context and shared page. */
	vcn_windows(device);

	/* Lets the processor out of its reset and the memory interface run. */
	vcn_clear(device, AMDRDNA_VCN_REG_SOFT_RESET, AMDRDNA_VCN_RESET_PROCESSOR);
	vcn_clear(device, AMDRDNA_VCN_REG_MEMORY_CONTROL2, AMDRDNA_VCN_MEMORY_STALL);
	vcn_clear(device, AMDRDNA_VCN_REG_SOFT_RESET, AMDRDNA_VCN_RESET_MEMORY | AMDRDNA_VCN_RESET_MEMORY_CONTROLLER);

	/* No byte swapping: the CPU and the engine are both little-endian. */
	vcn_write(device, AMDRDNA_VCN_REG_BYTE_SWAP, 0U);

	/* The stop must undo everything from here on. */
	vcn->running = true;

	/* Waits for the processor to report that it runs. */
	error = vcn_processor_wait(device);
	if (error != 0)
		return error;

	/* The processor's interrupt on, the busy mark off, the decode ring in the kernel's context. */
	vcn_set(device, AMDRDNA_VCN_REG_INTERRUPT_ENABLE, AMDRDNA_VCN_INTERRUPT_PROCESSOR);
	vcn_clear(device, AMDRDNA_VCN_REG_STATUS, AMDRDNA_VCN_STATUS_BUSY);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_CONTEXT, 0U);

	/* Sets the three rings up, and their doorbells when asked. */
	vcn_rings(device);
	if (vcn->doorbells)
		vcn_doorbells(device, true);

	/* Reads the status once, so every write above has reached the engine. */
	(void)vcn_read(device, AMDRDNA_VCN_REG_STATUS);

	/* Succeeded: the processor runs and the rings are set up. */
	return 0;
}

/*
 * Boots the engine under its dynamic power gating, the way the security
 * processor allows once it loaded the firmware (it guards the processor's
 * reset and the first two cache windows from the host, which read as all
 * ones): powers the tiles off and turns the gating on, has the security
 * processor apply the table of register writes (amdrdna_vcn_dpg_table:
 * the clock gates, the processor, the memory interface, the windows, the
 * resets released), then sets the decode ring up while the gating's power
 * up is stalled.  The processor runs when the gating powers the engine up
 * for work; the encode rings are set up by the gating's pause
 * (vcn_pause).
 *
 * Returns 0, ETIMEDOUT when the tiles do not power off, or the error of
 * the security processor's table.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start_dpg_mode with the table, and
 * vcn_v2_0_enable_static_power_gating, the path with AMD_PG_SUPPORT_VCN).
 */
static int
vcn_boot_gating(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn_dpg_setup setup;
	struct amdrdna_vcn *vcn;
	uint64_t stack;
	uint64_t base;
	uint32_t window;
	uint32_t word;
	uint32_t read;
	unsigned count;
	int error;

	/* The engine's state. */
	vcn = &device->vcn;
	vcn->paused = false;

	/* The tiles off, the power status saying so first; the gating then powers them. */
	word = vcn_read(device, AMDRDNA_VCN_REG_POWER_STATUS);
	word &= ~AMDRDNA_VCN_POWER_STATE_MASK;
	word |= AMDRDNA_VCN_POWER_STATE_OFF;
	vcn_write(device, AMDRDNA_VCN_REG_POWER_STATUS, word);
	vcn_write(device, AMDRDNA_VCN_REG_TILE_POWER, AMDRDNA_VCN_TILES_OFF);
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_TILE_STATE, AMDRDNA_VCN_TILES_MASK, AMDRDNA_VCN_TILES_OFF);
	if (error != 0) {
		amdrdna_mark("A5 vcn: the tiles did not power off (state 0x%08x)", (unsigned)vcn_read(device, AMDRDNA_VCN_REG_TILE_STATE));
		return error;
	}

	/* The gating's mode on. */
	vcn_set(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_GATING_MODE | AMDRDNA_VCN_POWER_GATING_ON);

	/* The table: the engine's window of registers, the context after the stack, the shared page. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_VCN, AMDRDNA_VCN_WINDOW_MAIN, 0U, &window);
	if (error != 0)
		return error;
	stack = device->frame_buffer_base + vcn->cache_vram;
	setup.window = window;
	setup.context = stack + AMDRDNA_VCN_STACK_BYTES;
	setup.shared = vcn->shared.gpu_address;
	setup.address_config = device->engines.address_config;
	amdrdna_vcn_shared_init(vcn->shared.dma.address);
	count = amdrdna_vcn_dpg_table(vcn->table.dma.address, AMDRDNA_VCN_DPG_WORDS, &setup);
	if (count == 0U)
		return EINVAL;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The security processor applies it. */
	error = amdrdna_psp_video_table(device, vcn->table.gpu_address, count * 4U);
	if (error != 0) {
		amdrdna_mark("A5 vcn: the security processor did not apply the gating's table (%d)", error);
		return error;
	}

	/* The stop must undo everything from here on. */
	vcn->running = true;

	/* The decode ring held off its fetch, while the gating's power up is stalled. */
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_CONTROL, amdrdna_vcn_decode_ring_control(AMDRDNA_VCN_RING_BYTES));
	vcn_set(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STALL);
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, AMDRDNA_VCN_QUEUE_DECODE, true);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Its write-pointer delay, its read pointer's write-back, its base and pointers. */
	base = vcn->decode.buffer.gpu_address;
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_WRITE_DELAY, 0U);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_READ_ADDRESS, (uint32_t)(base >> 32) >> 2);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_BASE_LOW, (uint32_t)base);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_BASE_HIGH, (uint32_t)(base >> 32));
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_READ, 0U);
	vcn_write(device, AMDRDNA_VCN_REG_SCRATCH2, 0U);
	read = vcn_read(device, AMDRDNA_VCN_REG_DECODE_READ);
	vcn->decode.write = read & (AMDRDNA_VCN_RING_WORDS - 1U);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_WRITE, vcn->decode.write);
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, AMDRDNA_VCN_QUEUE_DECODE, false);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The gating's power up may go on. */
	vcn_clear(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STALL);
	if (vcn->doorbells)
		vcn_doorbells(device, true);

	/* Reads the status once, so every write above has reached the engine. */
	(void)vcn_read(device, AMDRDNA_VCN_REG_STATUS);

	/* Succeeded: the engine runs under its gating. */
	return 0;
}

/*
 * Pauses the dynamic power gating (pause) for the encode rings, or lets
 * it go on (!pause), when the engine runs under it; nothing otherwise.
 * The pause waits for the engine to be powered down, asks for the pause
 * and waits for its acknowledgement, then, with the gating's power up
 * stalled, sets both encode rings up afresh (empty) and gives the decode
 * ring its write pointer again, and waits for the main tiles to power.
 *
 * Returns 0, or ETIMEDOUT when a wait ran out.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_pause_dpg_mode) and amdgpu_vcn.c
 * (amdgpu_vcn_ring_begin_use: the encode rings' work runs paused).
 */
static int
vcn_pause(
	struct amdrdna_device *device,
	bool pause)
{
	struct amdrdna_vcn *vcn;
	uint32_t word;
	int error;

	/* Nothing to change without the gating or when it is in the state asked. */
	vcn = &device->vcn;
	if (!vcn->gating || vcn->paused == pause)
		return 0;
	word = vcn_read(device, AMDRDNA_VCN_REG_PAUSE) & ~AMDRDNA_VCN_PAUSE_ACKNOWLEDGED;

	/* Lets the gating go on: no wait. */
	if (!pause) {
		vcn_write(device, AMDRDNA_VCN_REG_PAUSE, word & ~AMDRDNA_VCN_PAUSE_REQUEST);
		vcn->paused = false;
		return 0;
	}

	/* Waits for the engine to be powered down, then asks for the pause and waits for it. */
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STATE_MASK, AMDRDNA_VCN_POWER_STATE_OFF);
	if (error == 0) {
		vcn_write(device, AMDRDNA_VCN_REG_PAUSE, word | AMDRDNA_VCN_PAUSE_REQUEST);
		error = vcn_wait_register(device, AMDRDNA_VCN_REG_PAUSE, AMDRDNA_VCN_PAUSE_ACKNOWLEDGED, AMDRDNA_VCN_PAUSE_ACKNOWLEDGED);
	}
	if (error != 0) {
		amdrdna_mark("vcn pause: not acknowledged (power 0x%08x, pause 0x%08x)",
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_POWER_STATUS),
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_PAUSE));
		return error;
	}

	/* With the power up stalled: both encode rings afresh, the decode ring's pointer again. */
	vcn_set(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STALL);
	vcn->encode.write = 0U;
	vcn->encode_low.write = 0U;
	vcn_ring_place(device,
	    &vcn->encode,
	    AMDRDNA_VCN_QUEUE_ENCODE,
	    AMDRDNA_VCN_REG_ENCODE_BASE_LOW,
	    AMDRDNA_VCN_REG_ENCODE_BASE_HIGH,
	    AMDRDNA_VCN_REG_ENCODE_SIZE,
	    AMDRDNA_VCN_REG_ENCODE_READ,
	    AMDRDNA_VCN_REG_ENCODE_WRITE);
	vcn_ring_place(device,
	    &vcn->encode_low,
	    AMDRDNA_VCN_QUEUE_ENCODE_LOW,
	    AMDRDNA_VCN_REG_ENCODE_LOW_BASE_LOW,
	    AMDRDNA_VCN_REG_ENCODE_LOW_BASE_HIGH,
	    AMDRDNA_VCN_REG_ENCODE_LOW_SIZE,
	    AMDRDNA_VCN_REG_ENCODE_LOW_READ,
	    AMDRDNA_VCN_REG_ENCODE_LOW_WRITE);
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, AMDRDNA_VCN_QUEUE_DECODE, true);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_WRITE, vcn_read(device, AMDRDNA_VCN_REG_SCRATCH2) & AMDRDNA_VCN_SCRATCH2_WRITE_MASK);
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, AMDRDNA_VCN_QUEUE_DECODE, false);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	vcn_clear(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STALL);

	/* Waits for the main tiles to power. */
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STATE_MASK, AMDRDNA_VCN_POWER_STATE_ON);
	if (error != 0)
		amdrdna_mark("vcn pause: the tiles did not power (0x%08x)", (unsigned)vcn_read(device, AMDRDNA_VCN_REG_POWER_STATUS));
	vcn->paused = true;

	/* Succeeded, or the tiles' wait's error. */
	return error;
}

/*
 * Halts the engine under its dynamic power gating: lets the gating go on,
 * waits for the engine to be powered down, for each ring's read pointer
 * to reach its write pointer and for the engine to be powered down again,
 * and turns the gating's mode off.
 *
 * Returns true when every wait was met, false otherwise.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_stop_dpg_mode).
 */
static bool
vcn_halt_gating(
	struct amdrdna_device *device)
{
	uint32_t write;
	bool confirmed;
	int error;

	/* The gating goes on, then the engine powers down. */
	confirmed = true;
	(void)vcn_pause(device, false);
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STATE_MASK, AMDRDNA_VCN_POWER_STATE_OFF);

	/* Each ring's read pointer reaches its write pointer. */
	if (error == 0) {
		write = vcn_read(device, AMDRDNA_VCN_REG_ENCODE_WRITE);
		error = vcn_wait_register(device, AMDRDNA_VCN_REG_ENCODE_READ, 0xffffffffU, write);
	}
	if (error == 0) {
		write = vcn_read(device, AMDRDNA_VCN_REG_ENCODE_LOW_WRITE);
		error = vcn_wait_register(device, AMDRDNA_VCN_REG_ENCODE_LOW_READ, 0xffffffffU, write);
	}
	if (error == 0) {
		write = vcn_read(device, AMDRDNA_VCN_REG_DECODE_WRITE) & AMDRDNA_VCN_SCRATCH2_WRITE_MASK;
		error = vcn_wait_register(device, AMDRDNA_VCN_REG_DECODE_READ, 0xffffffffU, write);
	}
	if (error == 0)
		error = vcn_wait_register(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_STATE_MASK, AMDRDNA_VCN_POWER_STATE_OFF);
	if (error != 0) {
		amdrdna_mark("vcn halt: not idle under the gating (power 0x%08x, decode read 0x%x write 0x%x)",
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_POWER_STATUS),
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_DECODE_READ),
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_DECODE_WRITE));
		confirmed = false;
	}

	/* The gating's mode off. */
	vcn_clear(device, AMDRDNA_VCN_REG_POWER_STATUS, AMDRDNA_VCN_POWER_GATING_MODE);
	(void)vcn_read(device, AMDRDNA_VCN_REG_STATUS);

	/* Succeeded: reports whether the engine was seen to stop. */
	return confirmed;
}

/*
 * Ungates the clocks of every unit of the engine.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_disable_clock_gating, with Navi10's medium-grain gating).
 */
static void
vcn_clocks_ungated(
	struct amdrdna_device *device)
{
	uint32_t word;

	/* The dynamic mode and the delays of the gate control. */
	word = vcn_read(device, AMDRDNA_VCN_REG_GATE_CONTROL);
	word |= AMDRDNA_VCN_GATE_DYNAMIC;
	word |= AMDRDNA_VCN_GATE_DELAYS;
	vcn_write(device, AMDRDNA_VCN_REG_GATE_CONTROL, word);

	/* No unit's clock gated, and no unit in its gating mode. */
	vcn_clear(device, AMDRDNA_VCN_REG_GATE, AMDRDNA_VCN_GATE_UNITS);
	vcn_clear(device, AMDRDNA_VCN_REG_GATE_CONTROL, AMDRDNA_VCN_GATE_MODES);

	/* The fine gates, and none of their units in its gating mode. */
	vcn_set(device, AMDRDNA_VCN_REG_FINE_GATE, AMDRDNA_VCN_FINE_GATES);
	vcn_clear(device, AMDRDNA_VCN_REG_FINE_GATE_CONTROL, AMDRDNA_VCN_FINE_GATE_MODES);
}

/*
 * Gates the clocks of every unit again, as the engine is left after its
 * stop.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_enable_clock_gating, with Navi10's medium-grain gating).
 */
static void
vcn_clocks_gated(
	struct amdrdna_device *device)
{
	uint32_t word;

	/* The dynamic mode and the delays of the gate control. */
	word = vcn_read(device, AMDRDNA_VCN_REG_GATE_CONTROL);
	word |= AMDRDNA_VCN_GATE_DYNAMIC;
	word |= AMDRDNA_VCN_GATE_DELAYS;
	vcn_write(device, AMDRDNA_VCN_REG_GATE_CONTROL, word);

	/* Every unit in its gating mode, the fine units too. */
	vcn_set(device, AMDRDNA_VCN_REG_GATE_CONTROL, AMDRDNA_VCN_GATE_MODES);
	vcn_set(device, AMDRDNA_VCN_REG_FINE_GATE_CONTROL, AMDRDNA_VCN_FINE_GATE_MODES);
}

/*
 * Points the processor's windows: window 0 at the program the security
 * processor placed in the trusted region (the file and a word more),
 * window 1 at the stack, window 2 at the context, the uncached window at
 * the shared page; and gives the engine the graphics block's address
 * layout.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_mc_resume, the path of a program the security processor
 * loaded).
 */
static void
vcn_windows(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	uint64_t stack;
	uint64_t context;
	uint64_t shared;

	/* The GPU addresses of the stack, the context and the shared page. */
	vcn = &device->vcn;
	stack = device->frame_buffer_base + vcn->cache_vram;
	context = stack + AMDRDNA_VCN_STACK_BYTES;
	shared = vcn->shared.gpu_address;

	/* Window 0: the placed program. */
	vcn_write(device, AMDRDNA_VCN_REG_CACHE0_LOW, (uint32_t)vcn->place);
	vcn_write(device, AMDRDNA_VCN_REG_CACHE0_HIGH, (uint32_t)(vcn->place >> 32));
	vcn_write(device, AMDRDNA_VCN_REG_CACHE0_OFFSET, 0U);
	vcn_write(device, AMDRDNA_VCN_REG_CACHE0_SIZE, amdrdna_vcn_cache_bytes(vcn->firmware.file_bytes));

	/* Window 1: the stack. */
	vcn_write(device, AMDRDNA_VCN_REG_CACHE1_LOW, (uint32_t)stack);
	vcn_write(device, AMDRDNA_VCN_REG_CACHE1_HIGH, (uint32_t)(stack >> 32));
	vcn_write(device, AMDRDNA_VCN_REG_CACHE1_OFFSET, 0U);
	vcn_write(device, AMDRDNA_VCN_REG_CACHE1_SIZE, AMDRDNA_VCN_STACK_BYTES);

	/* Window 2: the context. */
	vcn_write(device, AMDRDNA_VCN_REG_CACHE2_LOW, (uint32_t)context);
	vcn_write(device, AMDRDNA_VCN_REG_CACHE2_HIGH, (uint32_t)(context >> 32));
	vcn_write(device, AMDRDNA_VCN_REG_CACHE2_OFFSET, 0U);
	vcn_write(device, AMDRDNA_VCN_REG_CACHE2_SIZE, AMDRDNA_VCN_CONTEXT_BYTES);

	/* The uncached window: the shared page, filled before the processor reads it. */
	amdrdna_vcn_shared_init(vcn->shared.dma.address);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	vcn_write(device, AMDRDNA_VCN_REG_UNCACHED_LOW, (uint32_t)shared);
	vcn_write(device, AMDRDNA_VCN_REG_UNCACHED_HIGH, (uint32_t)(shared >> 32));
	vcn_write(device, AMDRDNA_VCN_REG_UNCACHED_OFFSET, 0U);
	vcn_write(device, AMDRDNA_VCN_REG_UNCACHED_SIZE, VCN_PAGE_BYTES);

	/* The graphics block's address layout, which stage A5 read. */
	vcn_write(device, AMDRDNA_VCN_REG_ADDRESS_CONFIG, device->engines.address_config);
}

/*
 * Waits for the processor to report that it runs, resetting it between
 * tries.
 *
 * Returns 0, ETIMEDOUT after the last try, or ECANCELED when a stop came.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start: the loop on the status word's bit 1).
 */
static int
vcn_processor_wait(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint32_t status;
	unsigned tries;
	unsigned poll;
	bool stopping;

	/* The engine's state. */
	vcn = &device->vcn;
	status = 0U;

	/* Each try polls the report, then resets the processor. */
	for (tries = 0U; tries < VCN_BOOT_TRIES; tries++) {
		/* Polls the report. */
		for (poll = 0U; poll < VCN_BOOT_POLLS; poll++) {
			/* Stops once the processor reports. */
			status = vcn_read(device, AMDRDNA_VCN_REG_STATUS);
			if ((status & AMDRDNA_VCN_STATUS_RUNNING) != 0U)
				break;

			/* Sleeps one step. */
			kern_usleep_range(VCN_BOOT_STEP_US, VCN_BOOT_STEP_US + VCN_BOOT_STEP_US / 4U);
		}

		/* The processor runs. */
		if ((status & AMDRDNA_VCN_STATUS_RUNNING) != 0U) {
			amdrdna_mark("A5 vcn processor runs after %u resets (status 0x%08x)", tries, (unsigned)status);
			return 0;
		}

		/* A stop that came meanwhile ends the tries. */
		irq = spin_lock_irqsave(&vcn->lock);

		stopping = vcn->stopping;

		spin_unlock_irqrestore(&vcn->lock, irq);

		/* Gives up for the stop. */
		if (stopping)
			return ECANCELED;

		/* Resets the processor for the next try. */
		amdrdna_mark("A5 vcn processor silent (status 0x%08x), resetting it", (unsigned)status);
		vcn_set(device, AMDRDNA_VCN_REG_SOFT_RESET, AMDRDNA_VCN_RESET_PROCESSOR);
		kern_usleep_range(VCN_BOOT_STEP_US, VCN_BOOT_STEP_US + VCN_BOOT_STEP_US / 4U);
		vcn_clear(device, AMDRDNA_VCN_REG_SOFT_RESET, AMDRDNA_VCN_RESET_PROCESSOR);
		kern_usleep_range(VCN_BOOT_STEP_US, VCN_BOOT_STEP_US + VCN_BOOT_STEP_US / 4U);
	}

	/* The processor never reported. */
	amdrdna_mark("A5 vcn processor did not start (status 0x%08x)", (unsigned)status);
	return ETIMEDOUT;
}

/*
 * Sets the three rings up: the decode ring held off its fetch with its
 * size, base and pointers, then the two encode rings with their bases,
 * sizes and pointers, each while its queue's "ring reset" mark is raised
 * in the shared page.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_start: the rings' part).
 */
static void
vcn_rings(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	uint64_t base;
	uint32_t read;

	/* The engine's state. */
	vcn = &device->vcn;

	/* The decode ring: its control, then its base and pointers under its reset mark. */
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_CONTROL, amdrdna_vcn_decode_ring_control(AMDRDNA_VCN_RING_BYTES));
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, AMDRDNA_VCN_QUEUE_DECODE, true);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	base = vcn->decode.buffer.gpu_address;
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_BASE_LOW, (uint32_t)base);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_BASE_HIGH, (uint32_t)(base >> 32));
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_READ, 0U);
	read = vcn_read(device, AMDRDNA_VCN_REG_DECODE_READ);
	vcn->decode.write = read & (AMDRDNA_VCN_RING_WORDS - 1U);
	vcn_write(device, AMDRDNA_VCN_REG_DECODE_WRITE, vcn->decode.write);
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, AMDRDNA_VCN_QUEUE_DECODE, false);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The general encode ring. */
	vcn_ring_place(device,
	    &vcn->encode,
	    AMDRDNA_VCN_QUEUE_ENCODE,
	    AMDRDNA_VCN_REG_ENCODE_BASE_LOW,
	    AMDRDNA_VCN_REG_ENCODE_BASE_HIGH,
	    AMDRDNA_VCN_REG_ENCODE_SIZE,
	    AMDRDNA_VCN_REG_ENCODE_READ,
	    AMDRDNA_VCN_REG_ENCODE_WRITE);

	/* The low-latency encode ring. */
	vcn_ring_place(device,
	    &vcn->encode_low,
	    AMDRDNA_VCN_QUEUE_ENCODE_LOW,
	    AMDRDNA_VCN_REG_ENCODE_LOW_BASE_LOW,
	    AMDRDNA_VCN_REG_ENCODE_LOW_BASE_HIGH,
	    AMDRDNA_VCN_REG_ENCODE_LOW_SIZE,
	    AMDRDNA_VCN_REG_ENCODE_LOW_READ,
	    AMDRDNA_VCN_REG_ENCODE_LOW_WRITE);
}

/*
 * Sets one encode ring up under its queue's reset mark: both pointers at
 * the ring's write count, its base, and its size in words.
 */
static void
vcn_ring_place(
	struct amdrdna_device *device,
	struct amdrdna_vcn_ring *ring,
	unsigned queue,
	uint32_t base_low,
	uint32_t base_high,
	uint32_t size,
	uint32_t read,
	uint32_t write)
{
	struct amdrdna_vcn *vcn;
	uint64_t base;

	/* Raises the queue's reset mark. */
	vcn = &device->vcn;
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, queue, true);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The pointers, the base and the size. */
	base = ring->buffer.gpu_address;
	vcn_write(device, read, ring->write);
	vcn_write(device, write, ring->write);
	vcn_write(device, base_low, (uint32_t)base);
	vcn_write(device, base_high, (uint32_t)(base >> 32));
	vcn_write(device, size, AMDRDNA_VCN_RING_WORDS);

	/* Clears the mark: the ring is set up. */
	amdrdna_vcn_shared_queue_reset(vcn->shared.dma.address, queue, false);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/*
 * Halts the engine without its dynamic power gating: waits for it to go
 * idle and for the memory interface to be clean, stalls the memory
 * controller's side and waits for it to be clean, stops the processor's
 * clock, puts the memory interface and the processor into their resets,
 * clears the status, gates the clocks and powers the tiles off.  The
 * resets are applied even when a wait ran out, so the engine stops
 * fetching either way.
 *
 * Returns true when the engine was seen idle and clean and its processor
 * is held in reset, false when that could not be confirmed.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_stop, the path without dynamic power gating, and
 * vcn_v2_0_enable_static_power_gating).
 */
static bool
vcn_halt(
	struct amdrdna_device *device)
{
	uint32_t word;
	bool confirmed;
	int error;

	/* The engine under its gating stops another way. */
	if (device->vcn.gating)
		return vcn_halt_gating(device);

	/* Waits for the engine to go idle and for the memory interface to be clean. */
	confirmed = true;
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_STATUS, AMDRDNA_VCN_STATUS_IDLE_MASK, AMDRDNA_VCN_STATUS_IDLE);
	if (error == 0)
		error = vcn_wait_register(device, AMDRDNA_VCN_REG_MEMORY_STATUS, AMDRDNA_VCN_MEMORY_CLEAN, AMDRDNA_VCN_MEMORY_CLEAN);
	if (error != 0) {
		amdrdna_mark("vcn halt: not idle (status 0x%08x, memory 0x%08x)",
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_STATUS),
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_MEMORY_STATUS));
		confirmed = false;
	}

	/* Stalls the memory controller's side and waits for it to be clean. */
	vcn_set(device, AMDRDNA_VCN_REG_MEMORY_CONTROL2, AMDRDNA_VCN_MEMORY_STALL);
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_MEMORY_STATUS, AMDRDNA_VCN_MEMORY_CONTROLLER_CLEAN, AMDRDNA_VCN_MEMORY_CONTROLLER_CLEAN);
	if (error != 0) {
		amdrdna_mark("vcn halt: the memory controller's side not clean (0x%08x)", (unsigned)vcn_read(device, AMDRDNA_VCN_REG_MEMORY_STATUS));
		confirmed = false;
	}

	/* Stops the processor's clock and holds the memory interface and the processor in reset. */
	vcn_clear(device, AMDRDNA_VCN_REG_PROCESSOR_CONTROL, AMDRDNA_VCN_PROCESSOR_CLOCK);
	vcn_set(device, AMDRDNA_VCN_REG_SOFT_RESET, AMDRDNA_VCN_RESET_MEMORY_CONTROLLER);
	vcn_set(device, AMDRDNA_VCN_REG_SOFT_RESET, AMDRDNA_VCN_RESET_MEMORY);
	vcn_set(device, AMDRDNA_VCN_REG_SOFT_RESET, AMDRDNA_VCN_RESET_PROCESSOR);
	vcn_write(device, AMDRDNA_VCN_REG_STATUS, 0U);

	/* Reads back that the processor is held in reset. */
	word = vcn_read(device, AMDRDNA_VCN_REG_SOFT_RESET);
	if ((word & AMDRDNA_VCN_RESET_PROCESSOR) == 0U) {
		amdrdna_mark("vcn halt: the processor's reset did not hold (0x%08x)", (unsigned)word);
		confirmed = false;
	}

	/* Gates the clocks again. */
	vcn_clocks_gated(device);

	/* Powers the tiles off, the power status saying so first. */
	word = vcn_read(device, AMDRDNA_VCN_REG_POWER_STATUS);
	word &= ~AMDRDNA_VCN_POWER_STATE_MASK;
	word |= AMDRDNA_VCN_POWER_STATE_OFF;
	vcn_write(device, AMDRDNA_VCN_REG_POWER_STATUS, word);
	vcn_write(device, AMDRDNA_VCN_REG_TILE_POWER, AMDRDNA_VCN_TILES_OFF);
	error = vcn_wait_register(device, AMDRDNA_VCN_REG_TILE_STATE, AMDRDNA_VCN_TILES_MASK, AMDRDNA_VCN_TILES_OFF);
	if (error != 0)
		amdrdna_mark("vcn halt: the tiles did not power off (state 0x%08x)", (unsigned)vcn_read(device, AMDRDNA_VCN_REG_TILE_STATE));

	/* Reads the status once, so every write above has reached the engine. */
	(void)vcn_read(device, AMDRDNA_VCN_REG_STATUS);

	/* Succeeded: reports whether the engine was seen to stop. */
	return confirmed;
}

/*
 * Tests the rings: the decode ring writes a value into the ninth scratch
 * register over the CPU's mark, and the general encode ring moves its
 * read pointer past an end command.
 *
 * Returns 0, or ETIMEDOUT when a ring did not run its commands.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_dec_ring_test_ring) and amdgpu_vcn.c
 * (amdgpu_vcn_enc_ring_test_ring).
 */
static int
vcn_test_rings(
	struct amdrdna_device *device)
{
	uint32_t words[VCN_SUBMIT_WORDS];
	struct amdrdna_vcn *vcn;
	uint32_t value;
	uint32_t before;
	uint32_t after;
	unsigned count;
	unsigned poll;
	int error;

	/* The decode ring: the CPU's mark, then the ring's scratch write. */
	vcn = &device->vcn;
	vcn_write(device, AMDRDNA_VCN_REG_SCRATCH9, VCN_TEST_MARK);
	count = amdrdna_vcn_decode_scratch(words, VCN_TEST_VALUE);
	error = vcn_submit(device, &vcn->decode, words, count, false, NULL);
	if (error != 0)
		return error;

	/* Polls the scratch register until the ring's value is there. */
	value = VCN_TEST_MARK;
	for (poll = 0U; poll < VCN_REGISTER_POLLS; poll++) {
		/* Stops once the value is written. */
		value = vcn_read(device, AMDRDNA_VCN_REG_SCRATCH9);
		if (value == VCN_TEST_VALUE)
			break;

		/* Sleeps one step. */
		kern_usleep_range(VCN_REGISTER_STEP_US, VCN_REGISTER_STEP_US * 2U);
	}

	/* Reports a decode ring that did not run. */
	if (value != VCN_TEST_VALUE) {
		amdrdna_mark("A5 vcn decode ring test: scratch 0x%08x, read 0x%x write 0x%x",
		    (unsigned)value,
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_DECODE_READ),
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_DECODE_WRITE));
		return ETIMEDOUT;
	}

	/* The general encode ring, under the gating set up by its pause: an end command, after which its read pointer moves. */
	error = vcn_pause(device, true);
	if (error != 0)
		return error;
	before = vcn_read_pointer(device, &vcn->encode);
	count = amdrdna_vcn_encode_end(words);
	error = vcn_submit(device, &vcn->encode, words, count, false, NULL);
	if (error != 0) {
		(void)vcn_pause(device, false);
		return error;
	}

	/* Polls the read pointer until it moves. */
	after = before;
	for (poll = 0U; poll < VCN_REGISTER_POLLS; poll++) {
		/* Stops once the engine read past the command. */
		after = vcn_read_pointer(device, &vcn->encode);
		if (after != before)
			break;

		/* Sleeps one step. */
		kern_usleep_range(VCN_REGISTER_STEP_US, VCN_REGISTER_STEP_US * 2U);
	}

	/* The decode work runs with the gating not paused. */
	error = vcn_pause(device, false);
	if (error != 0)
		return error;

	/* Reports an encode ring that did not run. */
	if (after == before) {
		amdrdna_mark("A5 vcn encode ring test: read 0x%x write 0x%x",
		    (unsigned)after,
		    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_ENCODE_WRITE));
		return ETIMEDOUT;
	}

	/* Marks the tests. */
	amdrdna_mark("A5 vcn ring tests ok: decode scratch 0x%08x, encode read 0x%x to 0x%x",
	    (unsigned)value,
	    (unsigned)before,
	    (unsigned)after);

	/* Succeeded: both rings run their commands. */
	return 0;
}

/*
 * Tests the message path: creates a decode session (an H.264 stream of
 * 1920 by 1088) and destroys it again, each message handed over by an
 * indirect buffer in the kernel's address context, and waits for the
 * destruction's fence.
 *
 * Returns 0, or the error of the submission or of the wait.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/amdgpu_vcn.c
 * (amdgpu_vcn_dec_ring_test_ib, amdgpu_vcn_dec_send_msg).
 */
static int
vcn_test_messages(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	uint8_t *pages;
	uint64_t address;
	unsigned count;
	int error;

	/* The self-test's pages and their GPU address. */
	vcn = &device->vcn;
	pages = vcn->test.dma.address;
	address = vcn->test.gpu_address;

	/* The creation's message and its indirect buffer. */
	error = amdrdna_vcn_message_create(pages + VCN_TEST_CREATE_AT, VCN_PAGE_BYTES, VCN_TEST_HANDLE, AMDRDNA_VCN_STREAM_H264, VCN_TEST_WIDTH, VCN_TEST_HEIGHT);
	if (error != 0)
		return error;
	count = amdrdna_vcn_message_ib((uint32_t *)(void *)(pages + VCN_TEST_CREATE_IB_AT), address + VCN_TEST_CREATE_AT);

	/* The destruction's message and its indirect buffer. */
	error = amdrdna_vcn_message_destroy(pages + VCN_TEST_DESTROY_AT, VCN_PAGE_BYTES, VCN_TEST_HANDLE);
	if (error != 0)
		return error;
	(void)amdrdna_vcn_message_ib((uint32_t *)(void *)(pages + VCN_TEST_DESTROY_IB_AT), address + VCN_TEST_DESTROY_AT);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Creates the session. */
	error = vcn_decode_run(device, 0U, address + VCN_TEST_CREATE_IB_AT, count, VCN_TEST_FENCE_MS, true);
	if (error != 0) {
		amdrdna_mark("A5 vcn create message: no fence (%d)", error);
		return error;
	}

	/* Destroys it. */
	error = vcn_decode_run(device, 0U, address + VCN_TEST_DESTROY_IB_AT, count, VCN_TEST_FENCE_MS, true);
	if (error != 0) {
		amdrdna_mark("A5 vcn destroy message: no fence (%d)", error);
		return error;
	}

	/* Marks the test. */
	amdrdna_mark("A5 vcn message test ok: session %u created and destroyed, fence %u",
	    VCN_TEST_HANDLE,
	    (unsigned)vcn->decode.sequence);

	/* Succeeded: the firmware answers messages. */
	return 0;
}

/*
 * Writes one submission on a ring under the ring's lock: the words, the
 * fence and its interrupt when asked (sequence gets its value), the
 * padding up to the ring's step, then the write pointer.
 *
 * Returns 0, EIO when the engine takes no new work, or EBUSY when the
 * ring has no room for the submission.
 */
static int
vcn_submit(
	struct amdrdna_device *device,
	struct amdrdna_vcn_ring *ring,
	uint32_t *words,
	unsigned count,
	bool fenced,
	uint32_t *sequence)
{
	struct amdrdna_vcn *vcn;
	struct mutex *ring_lock;
	volatile uint32_t *buffer;
	unsigned long irq;
	uint32_t room;
	uint32_t read;
	unsigned index;
	bool stopping;

	/* The ring's lock: the decode ring's, or the encode rings'. */
	vcn = &device->vcn;
	ring_lock = &vcn->decode_lock;
	if (ring->encode)
		ring_lock = &vcn->encode_lock;

	/* Writes the submission with the ring to itself. */
	mutex_lock(ring_lock);

	/* Refuses new work while the engine stops. */
	irq = spin_lock_irqsave(&vcn->lock);

	stopping = vcn->stopping;

	spin_unlock_irqrestore(&vcn->lock, irq);

	if (stopping) {
		mutex_unlock(ring_lock);
		return EIO;
	}

	/* The fence and its interrupt after the words, then the padding to the ring's step. */
	if (fenced) {
		ring->sequence++;
		if (ring->encode) {
			count += amdrdna_vcn_encode_fence(words + count, vcn_fence_address(device, ring), ring->sequence);
		} else {
			count += amdrdna_vcn_decode_fence(words + count, vcn_fence_address(device, ring), ring->sequence);
		}
	}
	if (ring->encode && fenced)
		count += amdrdna_vcn_encode_end(words + count);
	if (!ring->encode && fenced)
		count += amdrdna_vcn_decode_end(words + count);
	if (ring->encode) {
		count = amdrdna_vcn_encode_pad(words, count);
	} else {
		count = amdrdna_vcn_decode_pad(words, count);
	}

	/* The ring must have room for every word. */
	read = vcn_read_pointer(device, ring);
	room = amdrdna_vcn_ring_room(read, ring->write, AMDRDNA_VCN_RING_WORDS);
	if (room < count) {
		if (fenced)
			ring->sequence--;
		mutex_unlock(ring_lock);
		amdrdna_mark("%s: no room for %u words (read 0x%x, write 0x%x)", ring->name, count, (unsigned)read, (unsigned)ring->write);
		return EBUSY;
	}

	/* Copies the words at the write count, wrapping at the ring's end. */
	buffer = ring->buffer.dma.address;
	for (index = 0U; index < count; index++) {
		buffer[ring->write] = words[index];
		ring->write = (ring->write + 1U) & (AMDRDNA_VCN_RING_WORDS - 1U);
	}

	/* Orders the words before the engine is told about them, then moves the write pointer. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	vcn_write_pointer(device, ring);

	/* The submission's fence value. */
	if (sequence != NULL)
		*sequence = ring->sequence;

	mutex_unlock(ring_lock);

	/* Succeeded: the engine has the submission. */
	return 0;
}

/*
 * Waits until a ring's fence reaches sequence, for at most milliseconds,
 * without holding the ring's lock.  The interrupt thread wakes the
 * waiter at each of the engine's interrupts; every sleep also ends after
 * a millisecond, so a fence whose interrupt does not come is still seen.
 *
 * Returns 0, ECANCELED when the engine is being stopped, or ETIMEDOUT
 * when the fence did not come.
 */
static int
vcn_wait(
	struct amdrdna_device *device,
	struct amdrdna_vcn_ring *ring,
	uint32_t sequence,
	unsigned milliseconds)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint64_t started;
	uint64_t now;
	uint64_t observed;
	uint64_t wake;
	uint32_t value;
	bool reached;
	bool stopping;
	bool passed;

	/* Looks, then sleeps until the next interrupt or a millisecond, until the fence passes or the time is up. */
	vcn = &device->vcn;
	reached = false;
	stopping = false;
	started = clock_milliseconds(NULL);
	irq = spin_lock_irqsave(&vcn->lock);

	for (;;) {
		/* The queue's sequence first, so an interrupt after the look ends the sleep at once. */
		observed = waitq_sequence(&vcn->fences);

		/* Stops once the fence reached the sequence. */
		value = vcn_fence(device, ring);
		passed = amdrdna_fence_passed(value, sequence);
		if (passed) {
			reached = true;
			break;
		}

		/* Gives up when the engine is being stopped. */
		if (vcn->stopping) {
			stopping = true;
			break;
		}

		/* Stops when the time is up. */
		now = clock_milliseconds(NULL);
		if (now - started >= milliseconds)
			break;

		/* Sleeps until the engine's next interrupt, or a millisecond at the latest. */
		wake = sched_ticks() + kern_ms_to_ticks(VCN_FENCE_POLL_MS);
		(void)waitq_sleep(&vcn->fences, &vcn->lock, observed, wake, 0U);
	}

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Reports a wait the stop ended. */
	if (stopping)
		return ECANCELED;

	/* Reports a fence that never came. */
	if (!reached) {
		amdrdna_mark("%s fence %u not reached (at %u)", ring->name, (unsigned)sequence, (unsigned)value);
		return ETIMEDOUT;
	}

	/* Succeeded: the fence passed the sequence. */
	return 0;
}

/*
 * Runs one indirect buffer on the decode ring in an address context and
 * waits for its fence.  starting says the start's self-test runs it,
 * before the engine takes other work; otherwise a wait that runs out
 * hands the engine to the restart thread.
 *
 * Returns 0, EIO when the engine takes no work, or the error of the
 * submission or of the wait.
 */
static int
vcn_decode_run(
	struct amdrdna_device *device,
	unsigned context,
	uint64_t address,
	uint32_t length,
	unsigned milliseconds,
	bool starting)
{
	uint32_t words[VCN_SUBMIT_WORDS];
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint32_t sequence;
	unsigned count;
	bool ready;
	int error;

	/* Only a ready engine takes work, apart from the start's own tests. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	ready = vcn->ready;

	spin_unlock_irqrestore(&vcn->lock, irq);

	if (!ready && !starting)
		return EIO;

	/* The start of the packet and the indirect buffer; the submission adds the fence and the end. */
	count = amdrdna_vcn_decode_start(words);
	count += amdrdna_vcn_decode_ib(words + count, context, address, length);
	error = vcn_submit(device, &vcn->decode, words, count, true, &sequence);
	if (error != 0)
		return error;

	/* Waits for the fence. */
	error = vcn_wait(device, &vcn->decode, sequence, milliseconds);
	if (error == ETIMEDOUT && !starting)
		vcn_timed_out(device);
	if (error != 0)
		return error;

	/* A fence that came ends a run of waits that ran out. */
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->timeouts = 0U;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Succeeded: the engine ran the buffer. */
	return 0;
}

/*
 * Notes a wait that ran out: the engine takes no work until it has been
 * restarted, and the restart thread is asked to restart it; after
 * VCN_TIMEOUT_LIMIT waits in a row the engine stays down until the GPU
 * is reset.
 */
static void
vcn_timed_out(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	unsigned timeouts;

	/* Counts the wait and hands the engine to the restart thread, unless the limit is reached. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->ready = false;
	vcn->timeouts++;
	vcn->timed_out_ms = clock_milliseconds(NULL);
	timeouts = vcn->timeouts;
	if (timeouts < VCN_TIMEOUT_LIMIT && !vcn->stopping) {
		vcn->restart_wanted = true;
		waitq_wake_all(&vcn->restart_waitq);
	}

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Says what happens next. */
	if (timeouts < VCN_TIMEOUT_LIMIT) {
		amdrdna_mark("vcn: a wait ran out (%u in a row); restarting the engine", timeouts);
	} else {
		amdrdna_mark("vcn: %u waits ran out in a row; the engine stays down until the GPU is reset", timeouts);
	}
}

/*
 * Holds the engine: the memory its last work pointed at stays the
 * engine's until the GPU is reset, since the engine may still write it.
 */
static void
vcn_hold(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;

	/* held is read by whoever would give that memory back. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->held = true;

	spin_unlock_irqrestore(&vcn->lock, irq);
}

/* Starts the restart thread, unless one already runs. */
static int
vcn_thread_start(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	struct thread *thread;
	unsigned long irq;
	bool running;
	int error;

	/* A thread kept from an earlier start serves this one too. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	running = vcn->thread_running;

	spin_unlock_irqrestore(&vcn->lock, irq);

	if (running)
		return 0;

	/* Creates the thread. */
	error = kthread_create(vcn_restart_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		amdrdna_mark("A5 vcn: the restart thread not created (%d)", error);
		return error;
	}

	/*
	 * thread_running tells the stop that the thread may still touch the
	 * engine; the thread clears it last.  Nobody joins the thread.
	 */
	vcn->thread_running = true;
	thread->detached = 1;
	thread_start(thread);

	/* Succeeded: the thread waits for work. */
	return 0;
}

/*
 * Waits until a restart is asked for or the stop comes, and restarts the
 * engine: stops it, starts it and tests its rings, without the lock of
 * the sessions' device (nothing else waits on it).
 */
static void
vcn_restart_thread(
	void *argument)
{
	struct amdrdna_device *device;
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint64_t sequence;
	bool stopping;
	bool wanted;
	bool restarted;

	/* The GPU the thread serves. */
	device = argument;
	vcn = &device->vcn;

	/* One restart per turn of the loop. */
	for (;;) {
		/* Sleeps until a restart or the stop is asked for. */
		irq = spin_lock_irqsave(&vcn->lock);

		if (!vcn->stopping && !vcn->restart_wanted) {
			sequence = waitq_sequence(&vcn->restart_waitq);
			(void)waitq_sleep(&vcn->restart_waitq, &vcn->lock, sequence, 0U, 0U);
		}

		/* Takes the request; restarting says the thread has the engine. */
		stopping = vcn->stopping;
		wanted = vcn->restart_wanted;
		vcn->restart_wanted = false;
		if (wanted && !stopping)
			vcn->restarting = true;

		spin_unlock_irqrestore(&vcn->lock, irq);

		/* Leaves the loop when told to stop. */
		if (stopping)
			break;

		/* Nothing to do on a wake without a request. */
		if (!wanted)
			continue;

		/* Restarts the engine. */
		restarted = vcn_restart(device);

		/* Lets the engine take work again, or leaves it down. */
		irq = spin_lock_irqsave(&vcn->lock);

		vcn->restarting = false;
		if (restarted && !vcn->stopping) {
			vcn->ready = true;
			vcn->generation++;
			vcn->restarts++;
		}
		waitq_wake_all(&vcn->fences);

		spin_unlock_irqrestore(&vcn->lock, irq);
	}

	/* Tells the stop that the thread no longer touches the engine. */
	irq = spin_lock_irqsave(&vcn->lock);

	vcn->thread_running = false;

	spin_unlock_irqrestore(&vcn->lock, irq);
}

/*
 * Restarts the engine with both rings' locks held: halts it, and when the
 * halt was confirmed, lets go of the spaces its runs that did not end
 * kept, boots it again and tests its rings.  An engine whose halt was not
 * confirmed stays down and held until the GPU is reset.
 *
 * Returns true when the engine runs again.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_reset: the stop, then the start).
 */
static bool
vcn_restart(
	struct amdrdna_device *device)
{
	struct amdrdna_vcn *vcn;
	unsigned long irq;
	uint64_t asked_ms;
	uint64_t begun_ms;
	uint64_t halted_ms;
	unsigned released;
	bool stopped;
	int error;

	/* When the wait ran out, for the log. */
	vcn = &device->vcn;
	irq = spin_lock_irqsave(&vcn->lock);

	asked_ms = vcn->timed_out_ms;

	spin_unlock_irqrestore(&vcn->lock, irq);

	/* Halts the engine with no submission in progress. */
	mutex_lock(&vcn->decode_lock);
	mutex_lock(&vcn->encode_lock);

	begun_ms = clock_milliseconds(NULL);
	stopped = vcn_halt(device);
	halted_ms = clock_milliseconds(NULL);
	vcn->running = false;

	mutex_unlock(&vcn->encode_lock);
	mutex_unlock(&vcn->decode_lock);

	/* An engine not seen to stop keeps the memory its work pointed at, and stays down. */
	if (!stopped) {
		vcn_hold(device);
		amdrdna_mark("vcn restart: the halt was not confirmed; the engine stays down until the GPU is reset");
		return false;
	}

	/* The halted engine runs nothing more in the spaces its runs that did not end kept. */
	released = amdrdna_space_video_release(device);
	if (released != 0U)
		amdrdna_mark("vcn restart: %u spaces kept by runs that did not end let go", released);

	/* The rings start empty again. */
	vcn->decode.write = 0U;
	vcn->encode.write = 0U;
	vcn->encode_low.write = 0U;

	/* Boots the engine and tests its rings; the submissions take the rings' locks themselves. */
	error = vcn_boot(device);
	if (error == 0)
		error = vcn_test_rings(device);
	if (error != 0) {
		amdrdna_mark("vcn restart fail (%d); the engine stays down until the GPU is reset", error);
		return false;
	}

	/* Marks the restart and its times: from the wait that ran out, the halt, the start. */
	amdrdna_mark("vcn restarted: %llu ms after the wait ran out (halt begun after %llu ms, took %llu ms; start %llu ms)",
	    (unsigned long long)(clock_milliseconds(NULL) - asked_ms),
	    (unsigned long long)(begun_ms - asked_ms),
	    (unsigned long long)(halted_ms - begun_ms),
	    (unsigned long long)(clock_milliseconds(NULL) - halted_ms));

	/* Succeeded: the engine runs again. */
	return true;
}

/* Reads a ring's read pointer in words from the engine. */
static uint32_t
vcn_read_pointer(
	struct amdrdna_device *device,
	const struct amdrdna_vcn_ring *ring)
{
	struct amdrdna_vcn *vcn;
	uint32_t offset;
	uint32_t value;

	/* The ring's read pointer register. */
	vcn = &device->vcn;
	offset = AMDRDNA_VCN_REG_DECODE_READ;
	if (ring == &vcn->encode)
		offset = AMDRDNA_VCN_REG_ENCODE_READ;
	if (ring == &vcn->encode_low)
		offset = AMDRDNA_VCN_REG_ENCODE_LOW_READ;

	/* The pointer, inside the ring. */
	value = vcn_read(device, offset) & (AMDRDNA_VCN_RING_WORDS - 1U);

	/* Succeeded: the word the engine reads next. */
	return value;
}

/* Tells the engine a ring's write count, by its register or by its doorbell. */
static void
vcn_write_pointer(
	struct amdrdna_device *device,
	const struct amdrdna_vcn_ring *ring)
{
	struct amdrdna_vcn *vcn;
	volatile uint8_t *doorbells;
	uint32_t offset;

	/* Under the gating, the decode ring's pointer goes first to the gating, marked new. */
	vcn = &device->vcn;
	if (vcn->gating && ring == &vcn->decode)
		vcn_write(device, AMDRDNA_VCN_REG_SCRATCH2, ring->write | AMDRDNA_VCN_SCRATCH2_NEW_WRITE);

	/* The doorbell, when the boot line asks for it. */
	if (vcn->doorbells) {
		doorbells = device->doorbells.address;
		kern_mmio_write32(doorbells + (size_t)ring->doorbell * 4U, ring->write);
		return;
	}

	/* The ring's write pointer register. */
	offset = AMDRDNA_VCN_REG_DECODE_WRITE;
	if (ring == &vcn->encode)
		offset = AMDRDNA_VCN_REG_ENCODE_WRITE;
	if (ring == &vcn->encode_low)
		offset = AMDRDNA_VCN_REG_ENCODE_LOW_WRITE;
	vcn_write(device, offset, ring->write);
}

/*
 * Opens or closes the host bridge's doorbell range of the engine.
 *
 * Procedure referenced: Linux 7.1 drivers/gpu/drm/amd/amdgpu/vcn_v2_0.c
 * (vcn_v2_0_hw_init) and nbio_v2_3.c (nbio_v2_3_vcn_doorbell_range).
 */
static void
vcn_doorbells(
	struct amdrdna_device *device,
	bool on)
{
	uint32_t word;
	uint32_t count;

	/* Nothing to open or close unless the boot asks for the engine and its doorbells. */
	if (!device->vcn.wanted)
		return;
	if (!device->vcn.doorbells)
		return;

	/* The range: eight doorbells from the decode ring's, or none. */
	count = 0U;
	if (on)
		count = AMDRDNA_DOORBELL_VIDEO_COUNT;
	word = amdrdna_block_read(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_VIDEO_DOORBELLS);
	word = amdrdna_vcn_doorbell_range_word(word, AMDRDNA_DOORBELL_VIDEO_DECODE, count);
	amdrdna_block_write(device, AMDRDNA_BLOCK_NBIO, AMDRDNA_NBIO_WINDOW_HOST, AMDRDNA_NBIO_REG_VIDEO_DOORBELLS, word);
}

/* Reports the GPU's address of a ring's fence word. */
static uint64_t
vcn_fence_address(
	const struct amdrdna_device *device,
	const struct amdrdna_vcn_ring *ring)
{
	/* The write-back page's address and the ring's slot. */
	return device->vcn.writeback.gpu_address + ring->slot;
}

/* Reads a ring's fence word as the engine last wrote it. */
static uint32_t
vcn_fence(
	const struct amdrdna_device *device,
	const struct amdrdna_vcn_ring *ring)
{
	const volatile uint8_t *page;
	uint32_t value;

	/* The word at the ring's slot of the write-back page. */
	page = device->vcn.writeback.dma.address;
	value = __atomic_load_n((const volatile uint32_t *)(const volatile void *)(page + ring->slot), __ATOMIC_ACQUIRE);

	/* Succeeded: the fence's value. */
	return value;
}

/*
 * Waits until one of the engine's registers, masked, reads a value: polls
 * every 10 microseconds for 100 ms at most.  Returns 0, or ETIMEDOUT.
 */
static int
vcn_wait_register(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	uint32_t word;
	unsigned poll;

	/* Looks, then sleeps, until the register reads the value. */
	for (poll = 0U; poll < VCN_REGISTER_POLLS; poll++) {
		/* Stops once the register reads the value. */
		word = vcn_read(device, offset);
		if ((word & mask) == value)
			return 0;

		/* Sleeps one step. */
		kern_usleep_range(VCN_REGISTER_STEP_US, VCN_REGISTER_STEP_US * 2U);
	}

	/* The register never read the value. */
	return ETIMEDOUT;
}

/*
 * Marks the registers that tell why a start failed: the processor's
 * control and reset, the memory interface's settings, the clock gates and
 * their state, and what the processor's windows read back.
 */
static void
vcn_report(
	struct amdrdna_device *device)
{
	/* The processor and the memory interface. */
	amdrdna_mark("A5 vcn regs: cpu=0x%08x reset=0x%08x lmi=0x%08x lmi2=0x%08x vm=0x%08x vmid=0x%08x",
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_PROCESSOR_CONTROL),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_SOFT_RESET),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_MEMORY_CONTROL),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_MEMORY_CONTROL2),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_MEMORY_CONTEXT),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE_CONTEXT));

	/* The clock gates and their state. */
	amdrdna_mark("A5 vcn gates: cgc=0x%08x/0x%08x/0x%08x suvd=0x%08x/0x%08x/0x%08x tiles=0x%08x mastint=0x%08x",
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_GATE),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_GATE_CONTROL),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_GATE_STATUS),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_FINE_GATE),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_FINE_GATE_CONTROL),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_FINE_GATE_STATUS),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_TILE_POWER),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_INTERRUPT_ENABLE));

	/* The windows as the engine holds them. */
	amdrdna_mark("A5 vcn windows: 0=0x%08x%08x+0x%x/0x%x 1=0x%08x%08x/0x%x 2=0x%08x%08x/0x%x nc=0x%08x%08x/0x%x",
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE0_HIGH),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE0_LOW),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE0_OFFSET),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE0_SIZE),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE1_HIGH),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE1_LOW),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE1_SIZE),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE2_HIGH),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE2_LOW),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_CACHE2_SIZE),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_UNCACHED_HIGH),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_UNCACHED_LOW),
	    (unsigned)vcn_read(device, AMDRDNA_VCN_REG_UNCACHED_SIZE));
}

/* Reads one of the engine's registers. */
static uint32_t
vcn_read(
	struct amdrdna_device *device,
	uint32_t offset)
{
	uint32_t value;

	/* The registers are in the engine's second window. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_VCN, AMDRDNA_VCN_WINDOW_MAIN, offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes one of the engine's registers. */
static void
vcn_write(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t value)
{
	/* The registers are in the engine's second window. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_VCN, AMDRDNA_VCN_WINDOW_MAIN, offset, value);
}

/* Sets bits of one of the engine's registers, keeping the others. */
static void
vcn_set(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t bits)
{
	uint32_t word;

	/* Reads, sets, writes back. */
	word = vcn_read(device, offset);
	word |= bits;
	vcn_write(device, offset, word);
}

/* Clears bits of one of the engine's registers, keeping the others. */
static void
vcn_clear(
	struct amdrdna_device *device,
	uint32_t offset,
	uint32_t bits)
{
	uint32_t word;

	/* Reads, clears, writes back. */
	word = vcn_read(device, offset);
	word &= ~bits;
	vcn_write(device, offset, word);
}
