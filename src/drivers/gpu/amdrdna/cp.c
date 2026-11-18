/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A5's command processor: the graphics engine's settings, the
 * run-list controller's clear state, the scheduler's own queue, the
 * compute queues and the graphics queue, and the rings the driver writes
 * commands to.
 *
 * The security processor loaded the command processor's programs in
 * stage A3, and the run-list controller started them.  This file gives
 * the graphics engine its recommended settings and reads its shape, waits
 * for the run-list controller to finish loading, hands it the clear state,
 * and brings the queues up: the scheduler's own queue (on the second
 * compute engine) by its registers, then the compute queues and the
 * graphics queue through the scheduler, then the graphics engine with its
 * preamble.  With amdrdna.kiq=0 the graphics ring is set up by its
 * registers instead and no compute queue is attached.
 *
 * A ring is system memory the engine reads from the write pointer the
 * driver publishes (in a copy in memory and through the ring's doorbell)
 * up to where it has read (the read pointer it reports).  Each submission
 * ends with a fence: a value the engine writes once everything before it
 * is done.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/*
 * The contexts of the graphics hub, and the most shader arrays and
 * workgroup processors per array the driver handles (the processors'
 * fuse field is 16 bits wide).
 */
#define CP_CONTEXTS			16U
#define CP_ARRAYS_MOST			8U
#define CP_GROUPS_MOST			16U

/*
 * The hardware contexts of the graphics engine (the context limit is one
 * less), and the device ID word the command processor is given.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_gpu_early_init:
 * max_hw_contexts 8; gfx_v10_0_cp_gfx_start: CP_DEVICE_ID 1).
 */
#define CP_HARDWARE_CONTEXTS		8U
#define CP_DEVICE_WORD			1U

/*
 * The waits: a register poll every 10 to 20 microseconds for about
 * 100 ms (Linux waits up to 100000 microseconds), the 50 microseconds
 * after the compute engines are unhalted, and the 1 ms between the two
 * writes of the direct graphics ring's control.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (usec_timeout 100000) and
 * gfx_v10_0.c (gfx_v10_0_cp_compute_enable: udelay(50);
 * gfx_v10_0_cp_gfx_resume: mdelay(1)).
 */
#define CP_POLLS			6000U
#define CP_POLL_MIN_US			10U
#define CP_POLL_MAX_US			20U
#define CP_UNHALT_US			50U
#define CP_RING_SETTLE_US		1000U

/*
 * The ring test: the scratch word written by the CPU, then by the ring.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_test_ring:
 * 0xCAFEDEAD and 0xDEADBEEF).
 */
#define CP_TEST_BEFORE			0xcafedeadU
#define CP_TEST_AFTER			0xdeadbeefU

/*
 * The padding of a submission: the command rings take whole blocks of
 * 256 words, the copy engine's ring blocks of 16.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (the ring functions' align_mask
 * 0xff) and sdma_v5_0.c (sdma_v5_0_ring_funcs: align_mask 0xf).
 */
#define CP_BLOCK_WORDS			256U
#define CP_COPY_BLOCK_WORDS		16U

/*
 * The words of a ring's write-back slot: the read pointer, the write
 * pointer's copy, the fence, a test word, and the GPU's timestamp counter
 * at the start and at the end of a timed run (two words each).
 */
#define CP_WORD_READ			0U
#define CP_WORD_WRITE			2U
#define CP_WORD_FENCE			4U
#define CP_WORD_TEST			6U
#define CP_WORD_STAMP_START		8U
#define CP_WORD_STAMP_END		12U

/* The largest command the scheduler is handed at once: the resources and an attach per compute queue. */
#define CP_SCHEDULER_WORDS		(AMDRDNA_PM4_RESOURCES_WORDS + AMDRDNA_COMPUTE_QUEUES * AMDRDNA_PM4_ATTACH_WORDS)

/* How long a fence of the queue setup is waited for. */
#define CP_FENCE_MS			1000U

/* The longest sleep between two looks at a fence when no event comes, in milliseconds. */
#define CP_FENCE_POLL_MS		1U

/* The timed steps of a queue's reset, with the start. */
#define CP_RESET_STEPS			5U

static uint32_t gc_read(struct amdrdna_device *device, unsigned window, uint32_t offset);
static void gc_write(struct amdrdna_device *device, unsigned window, uint32_t offset, uint32_t value);
static void gc_change(struct amdrdna_device *device, unsigned window, uint32_t offset, uint32_t clear, uint32_t set);
static int gc_wait(struct amdrdna_device *device, unsigned window, uint32_t offset, uint32_t mask, uint32_t value);
static void select_queue(struct amdrdna_device *device, const struct amdrdna_queue_place *place, unsigned context);
static void select_nothing(struct amdrdna_device *device);
static void select_array(struct amdrdna_device *device, unsigned engine, unsigned array);
static void select_all_arrays(struct amdrdna_device *device);
static void settings_shape(struct amdrdna_device *device);
static void settings_contexts(struct amdrdna_device *device);
static void settings_harvest(struct amdrdna_device *device);
static void interrupts_set(struct amdrdna_device *device, bool on);
static void scheduler_registers(struct amdrdna_device *device);
static int scheduler_run(struct amdrdna_device *device, const uint32_t *words, unsigned count, const char *what);
static int scheduler_fenced(struct amdrdna_device *device, const uint32_t *words, unsigned count, const char *what);
static int ring_fenced(struct amdrdna_device *device, struct amdrdna_ring *ring);
static void compute_image(struct amdrdna_device *device, struct amdrdna_ring *ring, bool active);
static int compute_attach(struct amdrdna_device *device);
static int graphics_attach(struct amdrdna_device *device);
static void graphics_direct(struct amdrdna_device *device);
static int graphics_begin(struct amdrdna_device *device);
static int graphics_preamble(struct amdrdna_device *device);
static unsigned ring_room(struct amdrdna_device *device, const struct amdrdna_ring *ring);
static void ring_commit(struct amdrdna_device *device, struct amdrdna_ring *ring);
static uint64_t ring_gpu_word(const struct amdrdna_device *device, const struct amdrdna_ring *ring, unsigned which);

/*
 * Gives the graphics engine its recommended settings and reads its shape:
 * the settings, the bus timeout, the address configuration, the render
 * back ends, compute units and cache channels the fuses leave, the tile
 * steering, and every context's shader memory mode and apertures.
 *
 * Returns 0, or EINVAL when the block table gave no shape or the
 * graphics block's windows are not where the commands address them.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_hw_init,
 * gfx_v10_0_init_golden_registers, gfx_v10_0_constants_init).
 */
int
amdrdna_cp_settings(
	struct amdrdna_device *device)
{
	const struct amdrdna_register_setting *settings;
	const struct amdrdna_shader_shape *shape;
	const struct amdrdna_ip *graphics;
	uint32_t word;
	unsigned count;
	unsigned index;
	bool reads;

	/* The commands address registers by their absolute index; the windows must be Navi10's. */
	graphics = &device->blocks.blocks[AMDRDNA_BLOCK_GC];
	if (graphics->window_count < 2U ||
	    graphics->window[AMDRDNA_GC_WINDOW_SECOND] != AMDRDNA_GC_SECOND_WINDOW_START) {
		amdrdna_mark("A5 fail: graphics block windows not where the commands expect them");
		return EINVAL;
	}

	/* The shape says how many arrays and back ends to read. */
	if (!device->blocks.has_shape) {
		amdrdna_mark("A5 fail: no graphics shape in the block table");
		return EINVAL;
	}

	/* A shape the driver cannot select its arrays by is refused. */
	shape = &device->blocks.shape;
	if (shape->engines == 0U ||
	    shape->arrays_per_engine == 0U ||
	    shape->engines * shape->arrays_per_engine > CP_ARRAYS_MOST ||
	    shape->pairs_per_array == 0U ||
	    shape->pairs_per_array > CP_GROUPS_MOST ||
	    shape->backends_per_engine < shape->arrays_per_engine) {
		amdrdna_mark("A5 fail: graphics shape se=%u sa=%u wgp=%u rb=%u not handled",
		    (unsigned)shape->engines,
		    (unsigned)shape->arrays_per_engine,
		    (unsigned)shape->pairs_per_array,
		    (unsigned)shape->backends_per_engine);
		return EINVAL;
	}

	/* The recommended settings, each merged into the register's old value where it changes only some bits. */
	settings = amdrdna_graphics_settings(&count);
	for (index = 0U; index < count; index++) {
		/* Reads the old value only where it is kept. */
		word = 0U;
		reads = amdrdna_setting_reads(&settings[index]);
		if (reads)
			word = gc_read(device, settings[index].window, settings[index].offset);

		/* Writes the setting. */
		word = amdrdna_setting_word(&settings[index], word);
		gc_write(device, settings[index].window, settings[index].offset, word);
	}

	/* The longest read timeout of the bus. */
	gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_BUS_CONTROL, 0U, AMDRDNA_GC_BUS_TIMEOUT_MASK);

	/* The address configuration the firmware set. */
	device->engines.address_config = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_ADDRESS_CONFIG);

	/* The back ends, compute units and channels, and the steering they give. */
	settings_shape(device);

	/* Every context's shader memory mode, apertures and data-share words. */
	settings_contexts(device);

	/* Marks what the stage read. */
	amdrdna_mark("A5 settings %u gb=0x%08x rb=0x%x cu=%u tcc_off=0x%x steering=0x%08x",
	    count,
	    (unsigned)device->engines.address_config,
	    (unsigned)device->engines.backends,
	    device->engines.units,
	    (unsigned)device->engines.channels_off,
	    (unsigned)device->engines.steering);

	/* Succeeded: the engine has its settings. */
	return 0;
}

/*
 * Waits for the run-list controller to finish loading the command
 * processor's programs, hands it the clear state in VRAM, turns its
 * save/restore machine on, and keeps invalidations away from the
 * workgroup processors the fuses took.
 *
 * Returns 0, ETIMEDOUT when the load never finished, or ENOMEM.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_rlc_resume, the
 * path of the security processor's load with autoload:
 * gfx_v10_0_wait_for_rlc_autoload_complete, gfx_v10_0_init_csb,
 * gfx_v10_0_rlc_enable_srm; then gfx_v10_0_tcp_harvest).
 */
int
amdrdna_cp_run_list(
	struct amdrdna_device *device)
{
	struct amdrdna_engines *engines;
	volatile uint8_t *clear;
	uint32_t *stream;
	uint64_t address;
	unsigned index;
	unsigned poll;
	uint32_t busy;
	uint32_t boot;
	bool done;
	int error;

	/* Waits until the command processor is idle and the run-list controller says its load is done. */
	engines = &device->engines;
	done = false;
	busy = 0U;
	boot = 0U;
	for (poll = 0U; poll < CP_POLLS; poll++) {
		/* Reads both words. */
		busy = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMMAND_BUSY);
		boot = gc_read(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_RUN_LIST_BOOT);

		/* Stops once both say so. */
		if (busy == 0U && (boot & AMDRDNA_GC_RUN_LIST_BOOT_DONE) != 0U) {
			done = true;
			break;
		}

		/* Sleeps one step. */
		kern_usleep_range(CP_POLL_MIN_US, CP_POLL_MAX_US);
	}

	/* Reports a load that never finished. */
	if (!done) {
		amdrdna_mark("A5 fail: run-list load not done (busy=0x%08x boot=0x%08x)", (unsigned)busy, (unsigned)boot);
		return ETIMEDOUT;
	}

	/* The clear state's length, and VRAM for it, once for the record's life. */
	engines->clear_state_words = amdrdna_clear_state_words();
	if (engines->clear_state_vram == 0U) {
		error = amdrdna_vram_take(device, true, (uint64_t)engines->clear_state_words * 4U, 4096U, &engines->clear_state_vram);
		if (error != 0) {
			amdrdna_mark("A5 fail: no VRAM for the clear state (%d)", error);
			return error;
		}
	}

	/* Builds the stream in kernel memory. */
	stream = kern_malloc((size_t)engines->clear_state_words * 4U);
	if (stream == NULL)
		return ENOMEM;
	(void)amdrdna_clear_state(stream, engines->steering);

	/* Copies it to VRAM and makes it visible to the GPU. */
	clear = amdrdna_vram_cpu(device, engines->clear_state_vram);
	for (index = 0U; index < engines->clear_state_words; index++)
		kern_mmio_write32(clear + (size_t)index * 4U, stream[index]);
	kern_free(stream);
	amdrdna_host_path_flush(device);

	/* Tells the run-list controller where the clear state is and how long it is. */
	address = device->frame_buffer_base + engines->clear_state_vram;
	gc_write(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_CLEAR_STATE_HIGH, (uint32_t)(address >> 32));
	gc_write(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_CLEAR_STATE_LOW, (uint32_t)address & 0xfffffffcU);
	gc_write(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_CLEAR_STATE_LENGTH, engines->clear_state_words);

	/* Turns the save/restore machine on. */
	gc_change(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_SAVE_RESTORE, 0U, AMDRDNA_GC_SAVE_RESTORE_ON);

	/* Keeps invalidations away from the processors the fuses took. */
	settings_harvest(device);

	/* Marks the result. */
	amdrdna_mark("A5 run list ready, clear state %u words at 0x%llx", engines->clear_state_words, (unsigned long long)address);

	/* Succeeded: the run-list controller has its clear state. */
	return 0;
}

/*
 * Brings the queues up: the interrupts of the rings on, then either the
 * scheduler's own queue, the compute queues and the graphics queue
 * through it, or (amdrdna.kiq=0) the graphics ring by its registers; then
 * the graphics engine with its preamble, and a ring test of every queue.
 *
 * Returns 0, or the error of the step that failed (ETIMEDOUT for a queue
 * that never answered).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_cp_resume,
 * gfx_v10_0_kiq_resume, gfx_v10_0_kcq_resume,
 * gfx_v10_0_cp_async_gfx_ring_resume, gfx_v10_0_cp_gfx_resume).
 */
int
amdrdna_cp_start(
	struct amdrdna_device *device)
{
	struct amdrdna_engines *engines;
	unsigned index;
	int error;

	/* The rings' interrupts, and the idle events off. */
	engines = &device->engines;
	interrupts_set(device, true);

	/* The graphics ring by its registers, without the scheduler. */
	if (engines->direct) {
		graphics_direct(device);

		/* Unhalts the compute engines too, as the scheduler's path does. */
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMPUTE_HALT, 0U);
		kern_usleep_range(CP_UNHALT_US, CP_UNHALT_US * 2U);
		engines->compute_running = true;
	} else {
		/* The scheduler's own queue, by its registers. */
		scheduler_registers(device);

		/* Unhalts both compute engines, which run the scheduler and the compute queues. */
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMPUTE_HALT, 0U);
		kern_usleep_range(CP_UNHALT_US, CP_UNHALT_US * 2U);
		engines->compute_running = true;

		/* The scheduler's queue must run commands: its ring test is the first command the GPU runs. */
		error = amdrdna_ring_test(device, &engines->scheduler);
		if (error != 0)
			return error;
		engines->scheduler_up = true;

		/* The compute queues, through the scheduler. */
		error = compute_attach(device);
		if (error != 0)
			return error;

		/* The graphics queue, through the scheduler. */
		error = graphics_attach(device);
		if (error != 0)
			return error;
	}

	/* The graphics engine, and its preamble. */
	error = graphics_begin(device);
	if (error != 0)
		return error;

	/* A ring test of the graphics queue. */
	error = amdrdna_ring_test(device, &engines->graphics);
	if (error != 0)
		return error;
	engines->graphics.up = true;

	/* A ring test of every compute queue. */
	for (index = 0U; index < engines->compute_count; index++) {
		/* Tests one queue. */
		error = amdrdna_ring_test(device, &engines->compute[index]);
		if (error != 0)
			return error;
		engines->compute[index].up = true;
	}

	/* Marks the result. */
	if (engines->direct) {
		amdrdna_mark("A5 queues up: graphics ring direct, no compute queue");
	} else {
		amdrdna_mark("A5 queues up: scheduler, graphics, %u compute", engines->compute_count);
	}

	/* Succeeded: every queue runs commands. */
	return 0;
}

/*
 * Stops the command processor as far as stage A5 started it: detaches
 * the queues through the scheduler (the graphics queue preempted, the
 * compute queues reset), halts the graphics engine and the compute
 * engines, and turns the rings' interrupts off.  The run-list controller
 * keeps running.  Running it twice does nothing the second time.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_hw_fini,
 * gfx_v10_0_cp_enable) and amdgpu_gfx.c (amdgpu_gfx_disable_kgq,
 * amdgpu_gfx_disable_kcq).
 */
void
amdrdna_cp_stop(
	struct amdrdna_device *device)
{
	uint32_t words[(AMDRDNA_COMPUTE_QUEUES + 1U) * AMDRDNA_PM4_DETACH_WORDS];
	struct amdrdna_engines *engines;
	struct amdrdna_ring *ring;
	unsigned count;
	unsigned index;
	int error;

	/* Detaches the queues the scheduler attached. */
	engines = &device->engines;
	if (engines->scheduler_up) {
		/* The graphics queue is preempted. */
		count = 0U;
		if (engines->graphics.up) {
			count += amdrdna_pm4_queue_detach(words + count, AMDRDNA_QUEUE_ENGINE_GRAPHICS,
			    AMDRDNA_QUEUE_DETACH_PREEMPT, engines->graphics.doorbell);
		}

		/* The compute queues are reset: a stuck one does not finish a preemption. */
		for (index = 0U; index < engines->compute_count; index++) {
			ring = &engines->compute[index];
			if (ring->up) {
				count += amdrdna_pm4_queue_detach(words + count, AMDRDNA_QUEUE_ENGINE_COMPUTE,
				    AMDRDNA_QUEUE_DETACH_RESET, ring->doorbell);
			}
		}

		/* Hands the detaches to the scheduler and waits for them. */
		error = 0;
		if (count != 0U)
			error = scheduler_run(device, words, count, "detach");
		if (error != 0)
			amdrdna_mark("shutdown: the scheduler did not detach the queues (%d)", error);
		engines->scheduler_up = false;
	}

	/* No ring runs any more. */
	engines->graphics.up = false;
	for (index = 0U; index < AMDRDNA_COMPUTE_QUEUES; index++)
		engines->compute[index].up = false;

	/* Halts the graphics engine and waits for the command processor to go idle. */
	if (engines->graphics_running) {
		gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_HALT, 0U,
		    AMDRDNA_GC_GRAPHICS_HALT_CONSTANT | AMDRDNA_GC_GRAPHICS_HALT_PREFETCH | AMDRDNA_GC_GRAPHICS_HALT_MICRO);
		(void)gc_wait(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMMAND_BUSY, 0xffffffffU, 0U);
		engines->graphics_running = false;
	}

	/* Halts both compute engines. */
	if (engines->compute_running) {
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMPUTE_HALT,
		    AMDRDNA_GC_COMPUTE_HALT_FIRST | AMDRDNA_GC_COMPUTE_HALT_SECOND);
		kern_usleep_range(CP_UNHALT_US, CP_UNHALT_US * 2U);
		engines->compute_running = false;
	}

	/* The rings' interrupts off. */
	if (engines->interrupts_on)
		interrupts_set(device, false);
}

/*
 * Writes count words to a ring, a fence after them when sequence is not
 * NULL (the fence's value goes to *sequence; it raises the ring's
 * interrupt when interrupt is true), pads the ring to its block and
 * publishes the new write pointer.
 *
 * Returns 0, or EBUSY when the ring has no room for the words.
 */
int
amdrdna_ring_submit(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring,
	const uint32_t *words,
	unsigned count,
	bool interrupt,
	uint32_t *sequence)
{
	uint32_t fence[AMDRDNA_PM4_FENCE_WORDS];
	volatile uint32_t *slots;
	uint64_t fence_address;
	unsigned fence_words;
	unsigned block;
	unsigned room;
	unsigned mask;
	unsigned index;
	unsigned long irq;
	uint32_t value;

	/* The block the ring pads to. */
	block = CP_BLOCK_WORDS;
	if (ring->kind == AMDRDNA_RING_COPY)
		block = CP_COPY_BLOCK_WORDS;
	mask = ring->bytes / 4U - 1U;
	slots = ring->buffer.dma.address;

	/* Writes the words, the fence and the padding as one submission. */
	irq = spin_lock_irqsave(&ring->lock);

	/* The fence the engine writes once the words are done. */
	fence_words = 0U;
	value = 0U;
	if (sequence != NULL) {
		value = ring->sequence + 1U;
		fence_address = ring_gpu_word(device, ring, CP_WORD_FENCE);
		if (ring->kind == AMDRDNA_RING_COPY) {
			fence_words = amdrdna_copy_fence(fence, fence_address, value, interrupt);
		} else if (ring->kind == AMDRDNA_RING_SCHEDULER) {
			fence_words = amdrdna_pm4_write_memory(fence, fence_address, value, false);
		} else {
			fence_words = amdrdna_pm4_fence(fence, fence_address, value, interrupt, false);
		}
	}

	/* The ring must hold the words, the fence and a whole block of padding. */
	room = ring_room(device, ring);
	if (count + fence_words + block > room) {
		spin_unlock_irqrestore(&ring->lock, irq);
		amdrdna_mark("%s ring full: %u words, room %u (read %u, write %llu, fence %u of %u)", ring->name, count + fence_words, room,
		    (unsigned)*amdrdna_ring_word(device, ring, CP_WORD_READ), (unsigned long long)ring->write_pointer,
		    (unsigned)*amdrdna_ring_word(device, ring, CP_WORD_FENCE), (unsigned)ring->sequence);
		return EBUSY;
	}

	/* Copies the words and the fence. */
	for (index = 0U; index < count; index++) {
		slots[ring->write_pointer & mask] = words[index];
		ring->write_pointer++;
	}

	/* The fence after them. */
	for (index = 0U; index < fence_words; index++) {
		slots[ring->write_pointer & mask] = fence[index];
		ring->write_pointer++;
	}

	/* Pads to the ring's block. */
	while ((ring->write_pointer & (block - 1U)) != 0U) {
		if (ring->kind == AMDRDNA_RING_COPY) {
			slots[ring->write_pointer & mask] = amdrdna_copy_filler();
		} else {
			slots[ring->write_pointer & mask] = amdrdna_pm4_filler();
		}

		/* One more word written. */
		ring->write_pointer++;
	}

	/* The fence's value is the ring's newest; the engine has read up to here once it passes. */
	if (sequence != NULL) {
		ring->sequence = value;
		ring->done_value[value % AMDRDNA_RING_HISTORY] = value;
		ring->done_pointer[value % AMDRDNA_RING_HISTORY] = ring->write_pointer;
		*sequence = value;
	}

	/* Publishes the write pointer. */
	ring_commit(device, ring);

	spin_unlock_irqrestore(&ring->lock, irq);

	/* Succeeded: the engine reads the submission. */
	return 0;
}

/*
 * Waits until a ring's fence reaches sequence, for at most milliseconds.
 *
 * The waiter sleeps on the ring's fence queue, which the interrupt thread
 * wakes at each end-of-pipe event of the ring, so it wakes as soon as the
 * fence is written.  Every sleep also ends at the next tick, so a fence
 * whose event does not come (the interrupt ring off, a ring that writes
 * its fence without one) is still seen within a tick.
 *
 * Returns 0, or ETIMEDOUT when the fence did not come.
 */
int
amdrdna_ring_wait(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring,
	uint32_t sequence,
	unsigned milliseconds)
{
	volatile uint32_t *fence;
	unsigned long irq;
	uint64_t started;
	uint64_t now;
	uint64_t observed;
	uint64_t wake;
	uint32_t value;
	bool reached;

	/* The fence word. */
	fence = amdrdna_ring_word(device, ring, CP_WORD_FENCE);

	/* Looks, then sleeps until the next event or a millisecond, until the fence passes the sequence or the time is up. */
	reached = false;
	started = clock_milliseconds(NULL);
	irq = spin_lock_irqsave(&ring->lock);

	for (;;) {
		/* The queue's sequence first, so an event after the look ends the sleep at once. */
		observed = waitq_sequence(&ring->fences);

		/* Stops once the fence reached the sequence (the difference handles the wrap). */
		value = *fence;
		if ((int32_t)(value - sequence) >= 0) {
			reached = true;
			break;
		}

		/* Stops when the time is up. */
		now = clock_milliseconds(NULL);
		if (now - started >= milliseconds)
			break;

		/* Sleeps until the ring's next event, or a millisecond at the latest. */
		wake = sched_ticks() + kern_ms_to_ticks(CP_FENCE_POLL_MS);
		(void)waitq_sleep(&ring->fences, &ring->lock, observed, wake, 0U);
	}

	spin_unlock_irqrestore(&ring->lock, irq);

	/* Reports a fence that never came. */
	if (!reached) {
		amdrdna_mark("%s fence %u not reached (at %u)", ring->name, (unsigned)sequence, (unsigned)*fence);
		return ETIMEDOUT;
	}

	/* Succeeded: the fence passed the sequence. */
	return 0;
}

/*
 * Tests that a ring runs commands: a command queue writes the scratch
 * register, the copy engine writes the ring's test word, and the CPU
 * waits to read the new value.
 *
 * Returns 0, or ETIMEDOUT.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_test_ring) and
 * sdma_v5_0.c (sdma_v5_0_ring_test_ring).
 */
int
amdrdna_ring_test(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring)
{
	uint32_t words[AMDRDNA_COPY_WRITE_WORDS];
	volatile uint32_t *test;
	uint32_t value;
	uint32_t scratch;
	uint32_t after;
	unsigned count;
	unsigned poll;
	int error;

	/* Sets the word the ring overwrites, and builds the write. */
	test = NULL;
	if (ring->kind == AMDRDNA_RING_COPY) {
		test = amdrdna_ring_word(device, ring, CP_WORD_TEST);
		*test = CP_TEST_BEFORE;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		count = amdrdna_copy_write(words, ring_gpu_word(device, ring, CP_WORD_TEST), CP_TEST_AFTER);
	} else {
		gc_write(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_SCRATCH, CP_TEST_BEFORE);
		scratch = AMDRDNA_GC_SECOND_WINDOW_START + AMDRDNA_GC_REG_SCRATCH;
		after = CP_TEST_AFTER;
		count = amdrdna_pm4_registers(words, AMDRDNA_SPACE_USER, scratch, &after, 1U, false);
	}

	/* Submits it, without a fence. */
	error = amdrdna_ring_submit(device, ring, words, count, false, NULL);
	if (error != 0)
		return error;

	/* Waits for the new value. */
	value = 0U;
	for (poll = 0U; poll < CP_POLLS; poll++) {
		/* Reads the word the ring writes. */
		if (test != NULL) {
			value = *test;
		} else {
			value = gc_read(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_SCRATCH);
		}

		/* Stops once it changed. */
		if (value == CP_TEST_AFTER)
			break;

		/* Sleeps one step. */
		kern_usleep_range(CP_POLL_MIN_US, CP_POLL_MAX_US);
	}

	/* Reports a ring that did not run the write. */
	if (value != CP_TEST_AFTER) {
		amdrdna_mark("A5 fail: %s ring test read 0x%08x", ring->name, (unsigned)value);
		return ETIMEDOUT;
	}

	/* Marks the ring as running. */
	amdrdna_mark("A5 %s ring test ok", ring->name);

	/* Succeeded: the ring runs commands. */
	return 0;
}

/*
 * Reports the CPU's view of a word of a ring's write-back slot: the read
 * pointer (0), the write pointer's copy (2, two words), the fence (4),
 * the test word (6) or a timed run's timestamps (8 and 12, two words
 * each).
 */
volatile uint32_t *
amdrdna_ring_word(
	struct amdrdna_device *device,
	const struct amdrdna_ring *ring,
	unsigned which)
{
	volatile uint32_t *words;

	/* The slot in the write-back page. */
	words = (volatile uint32_t *)((uint8_t *)device->engines.writeback.dma.address + ring->slot);

	/* Succeeded: the word. */
	return words + which;
}

/* Reports the fence a ring last wrote: the sequence of its newest finished submission. */
uint32_t
amdrdna_ring_fence(
	struct amdrdna_device *device,
	const struct amdrdna_ring *ring)
{
	volatile uint32_t *fence;
	uint32_t value;

	/* The fence word of the ring's write-back slot. */
	fence = amdrdna_ring_word(device, ring, CP_WORD_FENCE);
	value = *fence;

	/* Succeeded: the fence. */
	return value;
}

/* Reports the sequence of the newest submission put on a ring. */
uint32_t
amdrdna_ring_sequence(
	struct amdrdna_ring *ring)
{
	unsigned long irq;
	uint32_t sequence;

	/* The sequence, read under the ring's lock. */
	irq = spin_lock_irqsave(&ring->lock);

	sequence = ring->sequence;

	spin_unlock_irqrestore(&ring->lock, irq);

	/* Succeeded: the sequence. */
	return sequence;
}

/* Reports the GPU's address of a ring's fence word. */
uint64_t
amdrdna_ring_fence_address(
	const struct amdrdna_device *device,
	const struct amdrdna_ring *ring)
{
	uint64_t address;

	/* The fence word of the ring's write-back slot. */
	address = ring_gpu_word(device, ring, CP_WORD_FENCE);

	/* Succeeded: the address. */
	return address;
}

/* Reports the GPU's address of where a timed run writes the timestamp counter, at its start or at its end. */
uint64_t
amdrdna_ring_stamp_address(
	const struct amdrdna_device *device,
	const struct amdrdna_ring *ring,
	bool end)
{
	uint64_t address;

	/* The pair of words of the slot for the start or the end. */
	address = ring_gpu_word(device, ring, CP_WORD_STAMP_START);
	if (end)
		address = ring_gpu_word(device, ring, CP_WORD_STAMP_END);

	/* Succeeded: the address. */
	return address;
}

/*
 * Reads the timestamp counter a timed run wrote at its start and at its
 * end; the run's fence has passed, so both are written.
 */
void
amdrdna_ring_stamps(
	struct amdrdna_device *device,
	const struct amdrdna_ring *ring,
	uint64_t *started,
	uint64_t *ended)
{
	volatile uint32_t *start;
	volatile uint32_t *end;

	/* The two pairs of words of the slot. */
	start = amdrdna_ring_word(device, ring, CP_WORD_STAMP_START);
	end = amdrdna_ring_word(device, ring, CP_WORD_STAMP_END);

	/* Each counter, its low word first. */
	*started = (uint64_t)start[0] | ((uint64_t)start[1] << 32);
	*ended = (uint64_t)end[0] | ((uint64_t)end[1] << 32);
}

/* Reads a register of the graphics block. */
static uint32_t
gc_read(
	struct amdrdna_device *device,
	unsigned window,
	uint32_t offset)
{
	uint32_t value;

	/* The register of the window. */
	value = amdrdna_block_read(device, AMDRDNA_BLOCK_GC, window, offset);

	/* Succeeded: the register's value. */
	return value;
}

/* Writes a register of the graphics block. */
static void
gc_write(
	struct amdrdna_device *device,
	unsigned window,
	uint32_t offset,
	uint32_t value)
{
	/* The register of the window. */
	amdrdna_block_write(device, AMDRDNA_BLOCK_GC, window, offset, value);
}

/* Clears and then sets bits of a register of the graphics block. */
static void
gc_change(
	struct amdrdna_device *device,
	unsigned window,
	uint32_t offset,
	uint32_t clear,
	uint32_t set)
{
	uint32_t word;

	/* Reads, changes, writes. */
	word = gc_read(device, window, offset);
	word &= ~clear;
	word |= set;
	gc_write(device, window, offset, word);
}

/* Waits until bits of a register read a value; ETIMEDOUT when they never did. */
static int
gc_wait(
	struct amdrdna_device *device,
	unsigned window,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	uint32_t word;
	unsigned poll;

	/* Looks, then sleeps. */
	for (poll = 0U; poll < CP_POLLS; poll++) {
		/* Stops once the bits read the value. */
		word = gc_read(device, window, offset);
		if ((word & mask) == value)
			return 0;

		/* Sleeps one step. */
		kern_usleep_range(CP_POLL_MIN_US, CP_POLL_MAX_US);
	}

	/* The bits never read the value. */
	return ETIMEDOUT;
}

/*
 * Points the queue registers at one queue and the shader memory registers
 * at one context.  The caller holds the select mutex.
 */
static void
select_queue(
	struct amdrdna_device *device,
	const struct amdrdna_queue_place *place,
	unsigned context)
{
	uint32_t word;

	/* The queue select word. */
	word = amdrdna_queue_select_word(place, context);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_SELECT, word);
}

/*
 * Points the queue select back at the graphics engine's first queue and
 * context 0, where nothing selected is written by accident.  The caller
 * holds the select mutex.
 */
static void
select_nothing(
	struct amdrdna_device *device)
{
	/* Engine 0, pipe 0, queue 0, context 0. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_SELECT, 0U);
}

/*
 * Points the registers of the shader arrays at one array of one shader
 * engine, every instance of it.  The caller holds the select mutex.
 */
static void
select_array(
	struct amdrdna_device *device,
	unsigned engine,
	unsigned array)
{
	uint32_t word;

	/* The engine, the array and every instance. */
	word = AMDRDNA_GC_SELECT_ALL_INSTANCES;
	word |= (uint32_t)array << AMDRDNA_GC_SELECT_ARRAY_SHIFT;
	word |= (uint32_t)engine << AMDRDNA_GC_SELECT_ENGINE_INDEX_SHIFT;
	gc_write(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_ARRAY_SELECT, word);
}

/* Points the registers of the shader arrays back at all of them. The caller holds the select mutex. */
static void
select_all_arrays(
	struct amdrdna_device *device)
{
	/* Every engine, array and instance. */
	gc_write(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_ARRAY_SELECT,
	    AMDRDNA_GC_SELECT_ALL_INSTANCES | AMDRDNA_GC_SELECT_ALL_ARRAYS | AMDRDNA_GC_SELECT_ALL_ENGINES);
}

/*
 * Reads the render back ends and compute units every shader array has on
 * and the cache channels that are off, and derives the tile steering.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_setup_rb,
 * gfx_v10_0_get_cu_info, gfx_v10_0_get_tcc_info,
 * gfx_v10_0_init_pa_sc_tile_steering_override).
 */
static void
settings_shape(
	struct amdrdna_device *device)
{
	const struct amdrdna_shader_shape *shape;
	struct amdrdna_engines *engines;
	unsigned backends_per_array;
	unsigned converters_per_array;
	unsigned first_backends;
	unsigned engine;
	unsigned array;
	unsigned place;
	uint32_t fused;
	uint32_t off;
	uint32_t on;

	/* The shape the block table gave. */
	shape = &device->blocks.shape;
	engines = &device->engines;
	backends_per_array = shape->backends_per_engine / shape->arrays_per_engine;

	/* Reads every array's back ends and compute units. */
	engines->backends = 0U;
	engines->units = 0U;
	first_backends = 0U;
	mutex_lock(&engines->select);

	for (engine = 0U; engine < shape->engines; engine++) {
		for (array = 0U; array < shape->arrays_per_engine; array++) {
			/* Selects the array. */
			select_array(device, engine, array);
			place = engine * shape->arrays_per_engine + array;

			/* Its back ends, in its own bits of the mask. */
			fused = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_BACKENDS_FUSED);
			off = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_BACKENDS_OFF);
			on = amdrdna_backends_on(fused, off, backends_per_array);
			engines->backends |= on << (place * backends_per_array);
			if (place == 0U)
				first_backends = on;

			/* Its compute units. */
			fused = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GROUPS_FUSED);
			off = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GROUPS_OFF);
			on = amdrdna_groups_on(fused, off, shape->pairs_per_array);
			engines->units += amdrdna_units_on(on);
		}
	}

	/* Points the array registers back at every array. */
	select_all_arrays(device);

	mutex_unlock(&engines->select);

	/* The cache channels that are off. */
	fused = gc_read(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_CHANNELS_FUSED);
	off = gc_read(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_CHANNELS_OFF);
	engines->channels_off = amdrdna_channels_off(fused, off);

	/* The steering: the converters of the whole engine, the back ends of one array per converter. */
	converters_per_array = shape->converters_per_engine / shape->arrays_per_engine;
	if (converters_per_array == 0U)
		converters_per_array = 1U;
	on = 0U;
	while (first_backends != 0U) {
		first_backends &= first_backends - 1U;
		on++;
	}

	/* The steering word from the three counts. */
	engines->steering = amdrdna_tile_steering(shape->engines * shape->converters_per_engine,
	    on / converters_per_array,
	    shape->packers_per_converter);
}

/*
 * Gives every context of the graphics hub its shader memory mode, the
 * contexts above 0 their apertures, and takes the global data share,
 * the wave sync and the ordered append away from them.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_constants_init,
 * gfx_v10_0_init_gds_vmid).
 */
static void
settings_contexts(
	struct amdrdna_device *device)
{
	struct amdrdna_queue_place place;
	unsigned context;

	/* The graphics engine's first queue; only the context matters here. */
	place.engine = AMDRDNA_ENGINE_GRAPHICS;
	place.pipe = 0U;
	place.queue = 0U;

	/* Each context's shader memory registers. */
	mutex_lock(&device->engines.select);

	for (context = 0U; context < CP_CONTEXTS; context++) {
		/* Selects the context. */
		select_queue(device, &place, context);

		/* Its mode, and the apertures of the contexts above 0. */
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_SHADER_MEMORY_MODE, amdrdna_shader_memory_mode());
		if (context != 0U)
			gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_SHADER_APERTURES, amdrdna_shader_apertures());
	}

	/* Points the select back at context 0. */
	select_nothing(device);

	mutex_unlock(&device->engines.select);

	/* No data share, wave sync or ordered append for the contexts above 0. */
	for (context = 1U; context < CP_CONTEXTS; context++) {
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_SHARE_BASE + 2U * context, 0U);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_SHARE_SIZE + 2U * context, 0U);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_WAVE_SYNC + context, 0U);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_ORDERED_APPEND + context, 0U);
	}
}

/*
 * Keeps invalidations away from the workgroup processors the fuses took,
 * array by array.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_tcp_harvest).
 */
static void
settings_harvest(
	struct amdrdna_device *device)
{
	const struct amdrdna_shader_shape *shape;
	unsigned engine;
	unsigned array;
	uint32_t fused;
	uint32_t off;
	uint32_t groups;
	uint32_t translation;
	uint32_t targets;

	/* The shape the block table gave. */
	shape = &device->blocks.shape;

	/* Each array's two words. */
	mutex_lock(&device->engines.select);

	for (engine = 0U; engine < shape->engines; engine++) {
		for (array = 0U; array < shape->arrays_per_engine; array++) {
			/* Selects the array and reads its processors. */
			select_array(device, engine, array);
			fused = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GROUPS_FUSED);
			off = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GROUPS_OFF);
			groups = amdrdna_groups_on(fused, off, shape->pairs_per_array);

			/* Rewrites only the processors' bits of both words. */
			translation = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_TRANSLATION_SKIP);
			targets = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_CACHE_TARGETS_SKIP);
			amdrdna_harvest_words(groups, shape->pairs_per_array, translation, targets, &translation, &targets);
			gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_TRANSLATION_SKIP, translation);
			gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_CACHE_TARGETS_SKIP, targets);
		}
	}

	/* Points the array registers back at every array. */
	select_all_arrays(device);

	mutex_unlock(&device->engines.select);
}

/*
 * Turns the rings' interrupts on (the end-of-pipe event, the bad opcode
 * and the privileged accesses, with the idle events off) or off.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c
 * (gfx_v10_0_enable_gui_idle_interrupt,
 * gfx_v10_0_set_gfx_eop_interrupt_state,
 * gfx_v10_0_set_compute_eop_interrupt_state,
 * gfx_v10_0_set_priv_reg_fault_state, gfx_v10_0_set_bad_op_fault_state,
 * gfx_v10_0_set_priv_inst_fault_state).
 */
static void
interrupts_set(
	struct amdrdna_device *device,
	bool on)
{
	uint32_t graphics;
	uint32_t compute;
	unsigned pipe;

	/* The bits of the graphics ring and of a compute pipe. */
	graphics = AMDRDNA_GC_INTERRUPT_END_OF_PIPE | AMDRDNA_GC_INTERRUPT_BAD_OPCODE |
	    AMDRDNA_GC_INTERRUPT_PRIVILEGED_WORD | AMDRDNA_GC_INTERRUPT_PRIVILEGED_CODE;
	compute = AMDRDNA_GC_INTERRUPT_END_OF_PIPE | AMDRDNA_GC_INTERRUPT_BAD_OPCODE |
	    AMDRDNA_GC_INTERRUPT_PRIVILEGED_WORD;

	/* Sets or clears them; the idle events stay off either way. */
	if (on) {
		gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_INTERRUPTS,
		    AMDRDNA_GC_INTERRUPT_IDLE_EVENTS, graphics);
		for (pipe = 0U; pipe < AMDRDNA_COMPUTE_QUEUES; pipe++)
			gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMPUTE_INTERRUPTS + pipe, 0U, compute);
	} else {
		gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_INTERRUPTS,
		    AMDRDNA_GC_INTERRUPT_IDLE_EVENTS | graphics, 0U);
		for (pipe = 0U; pipe < AMDRDNA_COMPUTE_QUEUES; pipe++)
			gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMPUTE_INTERRUPTS + pipe, compute, 0U);
	}

	/* Records the state, so the stop turns them off only once. */
	device->engines.interrupts_on = on;
}

/*
 * Brings the scheduler's own queue up by its registers: names it to the
 * run-list controller, fills its image (active), and writes the queue's
 * registers from the image with the queue selected, its doorbell and the
 * compute doorbell range last.  The compute engines are still halted.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_kiq_init_queue,
 * gfx_v10_0_kiq_setting, gfx_v10_0_kiq_init_register).
 */
static void
scheduler_registers(
	struct amdrdna_device *device)
{
	struct amdrdna_ring *ring;
	const uint32_t *image;
	uint32_t word;

	/* The scheduler's queue and its image. */
	ring = &device->engines.scheduler;
	image = ring->image_copy;

	/* Names the queue to the run-list controller. */
	word = gc_read(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_SCHEDULER_QUEUE_SELECT);
	word = amdrdna_scheduler_queue_word(word, &ring->place);
	gc_write(device, AMDRDNA_GC_WINDOW_SECOND, AMDRDNA_GC_REG_SCHEDULER_QUEUE_SELECT, word);

	/* Writes the queue's registers with it selected. */
	mutex_lock(&device->engines.select);

	/* Fills the image from the registers the queue holds now. */
	compute_image(device, ring, true);

	/* The write pointer is not polled; the doorbell carries it. */
	gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_POINTER_POLL, AMDRDNA_GC_POINTER_POLL_ON, 0U);

	/* A queue left active is dequeued first, and its pointers put back to the image's. */
	word = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_ACTIVE);
	if ((word & 1U) != 0U) {
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_DEQUEUE, 1U);
		(void)gc_wait(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_ACTIVE, 1U, 0U);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_DEQUEUE, image[AMDRDNA_COMPUTE_IMAGE_DEQUEUE]);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_READ, image[AMDRDNA_COMPUTE_IMAGE_READ]);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_WRITE_LOW, image[AMDRDNA_COMPUTE_IMAGE_WRITE_LOW]);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_WRITE_HIGH, image[AMDRDNA_COMPUTE_IMAGE_WRITE_HIGH]);
	}

	/* The doorbell off while the queue is set up. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_DOORBELL, 0U);

	/* The end-of-pipe buffer and its size. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_END_LOW, image[AMDRDNA_COMPUTE_IMAGE_END_LOW]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_END_HIGH, image[AMDRDNA_COMPUTE_IMAGE_END_HIGH]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_END_CONTROL, image[AMDRDNA_COMPUTE_IMAGE_END_CONTROL]);

	/* The image and its context. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_IMAGE_LOW, image[AMDRDNA_COMPUTE_IMAGE_SELF_LOW]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_IMAGE_HIGH, image[AMDRDNA_COMPUTE_IMAGE_SELF_HIGH]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_IMAGE_CONTROL, image[AMDRDNA_COMPUTE_IMAGE_CONTROL]);

	/* The ring and its control. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_RING_LOW, image[AMDRDNA_COMPUTE_IMAGE_RING_LOW]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_RING_HIGH, image[AMDRDNA_COMPUTE_IMAGE_RING_HIGH]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_RING_CONTROL, image[AMDRDNA_COMPUTE_IMAGE_RING_CONTROL]);

	/* Where the read pointer is reported and the write pointer polled. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_READ_REPORT_LOW, image[AMDRDNA_COMPUTE_IMAGE_READ_REPORT_LOW]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_READ_REPORT_HIGH, image[AMDRDNA_COMPUTE_IMAGE_READ_REPORT_HIGH]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_WRITE_POLL_LOW, image[AMDRDNA_COMPUTE_IMAGE_WRITE_POLL_LOW]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_WRITE_POLL_HIGH, image[AMDRDNA_COMPUTE_IMAGE_WRITE_POLL_HIGH]);

	/* The doorbells the compute engines take: from the scheduler's to the last compute queue's. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMPUTE_DOORBELLS_LOW, AMDRDNA_DOORBELL_SCHEDULER << 2);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMPUTE_DOORBELLS_HIGH, AMDRDNA_DOORBELL_COMPUTE_LAST << 2);

	/* The queue's doorbell, an empty write pointer, context 0, its persistent state. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_DOORBELL, image[AMDRDNA_COMPUTE_IMAGE_DOORBELL]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_WRITE_LOW, image[AMDRDNA_COMPUTE_IMAGE_WRITE_LOW]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_WRITE_HIGH, image[AMDRDNA_COMPUTE_IMAGE_WRITE_HIGH]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_CONTEXT, image[AMDRDNA_COMPUTE_IMAGE_CONTEXT]);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_PERSISTENT, image[AMDRDNA_COMPUTE_IMAGE_PERSISTENT]);

	/* The queue active, and the doorbells of the compute engines on. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_ACTIVE, image[AMDRDNA_COMPUTE_IMAGE_ACTIVE]);
	gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_STATUS, 0U, AMDRDNA_GC_QUEUE_DOORBELLS_ON);

	/* Points the select back at nothing in particular. */
	select_nothing(device);

	mutex_unlock(&device->engines.select);
}

/*
 * Hands words to the scheduler's queue with a fence, waits for the fence,
 * and tests the queue again.  Returns 0, or the error of the step that
 * failed.
 */
static int
scheduler_run(
	struct amdrdna_device *device,
	const uint32_t *words,
	unsigned count,
	const char *what)
{
	struct amdrdna_ring *ring;
	uint32_t sequence;
	int error;

	/* One command for the scheduler at a time. */
	ring = &device->engines.scheduler;
	mutex_lock(&device->engines.scheduler_lock);

	/* Submits the words with a fence and waits for it. */
	error = amdrdna_ring_submit(device, ring, words, count, false, &sequence);
	if (error == 0)
		error = amdrdna_ring_wait(device, ring, sequence, CP_FENCE_MS);

	/* Tests that the queue still runs commands. */
	if (error == 0)
		error = amdrdna_ring_test(device, ring);

	mutex_unlock(&device->engines.scheduler_lock);

	/* Reports a scheduler that did not run the words. */
	if (error != 0) {
		amdrdna_mark("A5 fail: the scheduler did not run the %s (%d)", what, error);
		return error;
	}

	/* Succeeded: the scheduler ran the words. */
	return 0;
}

/*
 * Hands words to the scheduler's queue with a fence and waits for the
 * fence (the fence shows the queue ran them; no register test after).
 * Returns 0, or the error of the submission or the fence.
 */
static int
scheduler_fenced(
	struct amdrdna_device *device,
	const uint32_t *words,
	unsigned count,
	const char *what)
{
	struct amdrdna_ring *ring;
	uint32_t sequence;
	int error;

	/* One command for the scheduler at a time; the words, their fence, the wait. */
	ring = &device->engines.scheduler;
	mutex_lock(&device->engines.scheduler_lock);

	error = amdrdna_ring_submit(device, ring, words, count, false, &sequence);
	if (error == 0)
		error = amdrdna_ring_wait(device, ring, sequence, CP_FENCE_MS);

	mutex_unlock(&device->engines.scheduler_lock);

	/* Reports a scheduler that did not run the words. */
	if (error != 0) {
		amdrdna_mark("the scheduler did not run the %s (%d)", what, error);
		return error;
	}

	/* Succeeded: the scheduler ran the words. */
	return 0;
}

/* Tests that a ring runs: a fence alone on it, waited for.  Returns 0, or the error of the submission or the fence. */
static int
ring_fenced(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring)
{
	uint32_t sequence;
	int error;

	/* The fence and the wait. */
	error = amdrdna_ring_submit(device, ring, NULL, 0U, false, &sequence);
	if (error == 0)
		error = amdrdna_ring_wait(device, ring, sequence, CP_FENCE_MS);
	return error;
}

/*
 * Fills a compute queue's image from the registers it holds now (the
 * queue is selected by this function; the caller holds the select mutex),
 * writes it where the firmware reads it, and keeps a copy.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_compute_mqd_init).
 */
static void
compute_image(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring,
	bool active)
{
	struct amdrdna_compute_image_input input;

	/* Selects the queue. */
	select_queue(device, &ring->place, 0U);

	/* The buffers. */
	kern_memset(&input, 0, sizeof(input));
	input.image = ring->image.gpu_address;
	input.ring = ring->buffer.gpu_address;
	input.ring_bytes = ring->bytes;
	input.read_report = ring_gpu_word(device, ring, CP_WORD_READ);
	input.write_poll = ring_gpu_word(device, ring, CP_WORD_WRITE);
	input.end_of_pipe = ring->end_of_pipe.gpu_address;
	input.doorbell = ring->doorbell;
	input.active = active;

	/* The registers' values now. */
	input.end_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_END_CONTROL);
	input.doorbell_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_DOORBELL);
	input.image_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_IMAGE_CONTROL);
	input.ring_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_RING_CONTROL);
	input.read_pointer = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_READ);
	input.persistent = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_PERSISTENT);
	input.buffer_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_BUFFER_CONTROL);

	/* Fills the copy, then the image the firmware reads. */
	amdrdna_compute_image(&input, ring->image_copy);
	kern_memcpy(ring->image.dma.address, ring->image_copy, AMDRDNA_QUEUE_IMAGE_WORDS * 4U);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/*
 * Fills the compute queues' images and has the scheduler attach them,
 * after telling it which queues it runs.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_kcq_resume,
 * gfx_v10_0_kcq_init_queue) and amdgpu_gfx.c (amdgpu_gfx_enable_kcq).
 */
static int
compute_attach(
	struct amdrdna_device *device)
{
	struct amdrdna_queue_place places[AMDRDNA_COMPUTE_QUEUES];
	uint32_t words[CP_SCHEDULER_WORDS];
	struct amdrdna_engines *engines;
	struct amdrdna_ring *ring;
	unsigned count;
	unsigned index;
	int error;

	/* Fills every queue's image with the queue selected. */
	engines = &device->engines;
	mutex_lock(&engines->select);

	for (index = 0U; index < engines->compute_count; index++) {
		compute_image(device, &engines->compute[index], false);
		places[index] = engines->compute[index].place;
	}

	/* Points the select back at nothing in particular. */
	select_nothing(device);

	mutex_unlock(&engines->select);

	/* The scheduler's resources: these queues. */
	count = amdrdna_pm4_queue_resources(words, amdrdna_queue_resource_mask(places, engines->compute_count));

	/* One attach per queue. */
	for (index = 0U; index < engines->compute_count; index++) {
		ring = &engines->compute[index];
		count += amdrdna_pm4_queue_attach(words + count, &ring->place, AMDRDNA_QUEUE_ENGINE_COMPUTE,
		    ring->doorbell, ring->image.gpu_address, ring_gpu_word(device, ring, CP_WORD_WRITE));
	}

	/* Hands them to the scheduler. */
	error = scheduler_run(device, words, count, "compute attach");
	if (error != 0)
		return error;

	/* Succeeded: the compute queues are attached. */
	return 0;
}

/*
 * Fills the graphics queue's image, sets the graphics doorbell range, and
 * has the scheduler attach the queue.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_kgq_init_queue,
 * gfx_v10_0_gfx_mqd_init, gfx_v10_0_cp_gfx_set_doorbell) and
 * amdgpu_gfx.c (amdgpu_gfx_enable_kgq).
 */
static int
graphics_attach(
	struct amdrdna_device *device)
{
	struct amdrdna_graphics_image_input input;
	uint32_t words[AMDRDNA_PM4_ATTACH_WORDS];
	struct amdrdna_ring *ring;
	unsigned count;
	int error;

	/* The graphics queue. */
	ring = &device->engines.graphics;

	/* Fills the image from the queue's registers, with it selected. */
	mutex_lock(&device->engines.select);

	select_queue(device, &ring->place, 0U);
	kern_memset(&input, 0, sizeof(input));
	input.image = ring->image.gpu_address;
	input.ring = ring->buffer.gpu_address;
	input.ring_bytes = ring->bytes;
	input.read_report = ring_gpu_word(device, ring, CP_WORD_READ);
	input.write_poll = ring_gpu_word(device, ring, CP_WORD_WRITE);
	input.doorbell = ring->doorbell;
	input.image_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_IMAGE_CONTROL);
	input.priority = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_PRIORITY);
	input.quantum = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_QUANTUM);
	input.ring_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_RING_CONTROL);
	input.doorbell_control = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_DOORBELL);
	input.read_pointer = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_READ);
	amdrdna_graphics_image(&input, ring->image_copy);

	/* The graphics doorbells: from the queue's own up. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_DOORBELLS_LOW, (ring->doorbell << 2) & AMDRDNA_GC_DOORBELL_RANGE_MASK);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_DOORBELLS_HIGH, AMDRDNA_GC_DOORBELL_RANGE_MASK);

	/* Points the select back at nothing in particular. */
	select_nothing(device);

	mutex_unlock(&device->engines.select);

	/* Writes the image where the firmware reads it. */
	kern_memcpy(ring->image.dma.address, ring->image_copy, AMDRDNA_QUEUE_IMAGE_WORDS * 4U);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Has the scheduler attach the queue. */
	count = amdrdna_pm4_queue_attach(words, &ring->place, AMDRDNA_QUEUE_ENGINE_GRAPHICS,
	    ring->doorbell, ring->image.gpu_address, ring_gpu_word(device, ring, CP_WORD_WRITE));
	error = scheduler_run(device, words, count, "graphics attach");
	if (error != 0)
		return error;

	/* Succeeded: the graphics queue is attached. */
	return 0;
}

/*
 * Sets the graphics ring up by its registers, without the scheduler
 * (amdrdna.kiq=0).
 *
 * The ring takes its write pointer in its registers, without a doorbell
 * (the path of a ring without a doorbell).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_cp_gfx_resume,
 * gfx_v10_0_cp_gfx_set_doorbell, gfx_v10_0_ring_set_wptr_gfx).
 */
static void
graphics_direct(
	struct amdrdna_device *device)
{
	struct amdrdna_ring *ring;
	uint64_t address;
	uint32_t control;

	/* The graphics ring and its control word. */
	ring = &device->engines.graphics;
	control = amdrdna_graphics_ring_word(ring->bytes);

	/* No write-pointer delay, context 0. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_WRITE_DELAY, 0U);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_CONTEXT, 0U);

	/* Sets pipe 0's ring up with the pipe selected. */
	mutex_lock(&device->engines.select);

	select_queue(device, &ring->place, 0U);

	/* The size, an empty write pointer. */
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_CONTROL, control);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_WRITE_LOW, 0U);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_WRITE_HIGH, 0U);

	/* Where the read pointer is reported and the write pointer polled. */
	address = ring_gpu_word(device, ring, CP_WORD_READ);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_READ_REPORT_LOW, (uint32_t)address);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_READ_REPORT_HIGH, (uint32_t)(address >> 32) & 0xffffU);
	address = ring_gpu_word(device, ring, CP_WORD_WRITE);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_WRITE_POLL_LOW, (uint32_t)address);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_WRITE_POLL_HIGH, (uint32_t)(address >> 32));

	/* The control again after a millisecond, then the base, and the ring active. */
	kern_usleep_range(CP_RING_SETTLE_US, CP_RING_SETTLE_US * 2U);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_CONTROL, control);
	address = ring->buffer.gpu_address >> 8;
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_BASE_LOW, (uint32_t)address);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_BASE_HIGH, (uint32_t)(address >> 32));
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_ACTIVE, 1U);

	/* No doorbell: the driver writes the write pointer's registers itself. */
	gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_DOORBELL,
	    AMDRDNA_GC_DOORBELL_RANGE_MASK | AMDRDNA_GC_DOORBELL_ON, 0U);

	mutex_unlock(&device->engines.select);
}

/*
 * Starts the graphics engine: its context limit and device word, the
 * micro, prefetch and constant engines unhalted, and the preamble on its
 * ring (the clear state and the constant engine's partition).
 *
 * Returns 0, or the error of the preamble's submission or fence.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_cp_gfx_start,
 * gfx_v10_0_cp_gfx_enable).
 */
static int
graphics_begin(
	struct amdrdna_device *device)
{
	struct amdrdna_engines *engines;
	uint32_t busy;
	int error;

	/* The context limit and the device word. */
	engines = &device->engines;
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_CONTEXT_LIMIT, CP_HARDWARE_CONTEXTS - 1U);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMMAND_DEVICE, CP_DEVICE_WORD);

	/* Unhalts the three engines and waits for the command processor to go idle. */
	gc_change(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_HALT,
	    AMDRDNA_GC_GRAPHICS_HALT_CONSTANT | AMDRDNA_GC_GRAPHICS_HALT_PREFETCH | AMDRDNA_GC_GRAPHICS_HALT_MICRO, 0U);
	engines->graphics_running = true;
	error = gc_wait(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMMAND_BUSY, 0xffffffffU, 0U);
	if (error != 0) {
		busy = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_COMMAND_BUSY);
		amdrdna_mark("A5 graphics engine unhalted but busy (0x%08x)", (unsigned)busy);
	}

	/* The preamble. */
	error = graphics_preamble(device);
	if (error != 0)
		return error;

	/* Succeeded: the graphics engine runs with its clear state. */
	return 0;
}

/*
 * Runs the graphics ring's preamble (the clear state and the constant
 * engine's partition) and waits for its fence.  Returns 0, or the error of
 * the submission or the fence.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_cp_gfx_start).
 */
static int
graphics_preamble(
	struct amdrdna_device *device)
{
	struct amdrdna_engines *engines;
	uint32_t *words;
	uint32_t sequence;
	unsigned count;
	int error;

	/* The clear state and the partition. */
	engines = &device->engines;
	words = kern_malloc(((size_t)engines->clear_state_words + 4U) * 4U);
	if (words == NULL)
		return ENOMEM;
	count = amdrdna_clear_state(words, engines->steering);
	count += amdrdna_pm4_partition(words + count);

	/* Runs it on the graphics ring and waits for its fence. */
	error = amdrdna_ring_submit(device, &engines->graphics, words, count, false, &sequence);
	kern_free(words);
	if (error == 0)
		error = amdrdna_ring_wait(device, &engines->graphics, sequence, CP_FENCE_MS);
	if (error != 0) {
		amdrdna_mark("graphics preamble not done (%d)", error);
		return error;
	}

	/* Succeeded: the preamble ran. */
	return 0;
}

/*
 * Resets the graphics queue after work of a context did not end: the
 * scheduler resets the context's work on the queue and waits until the
 * queue went inactive; the queue's image comes back from its copy and its
 * ring starts empty; the scheduler attaches the queue again; the ring is
 * tested and the preamble runs again (the reset may have dropped the
 * state).  Each step is checked by a fence (the scratch register's test
 * takes a quarter of a second each).  The work that did not end is gone;
 * nothing else was on the ring (every run is waited for before the next
 * one is written).
 *
 * Returns 0, ENOTSUP for a graphics ring run without the scheduler, or the
 * error of the step that failed (the device then has no graphics queue).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_reset_kgq: the
 * register write, the wait for the image's active word and for the
 * register to read 0, then gfx_v10_0_kgq_init_queue with the copy and an
 * empty ring, the attach and the ring tests) and amdgpu_ring.c
 * (amdgpu_ring_reset_helper_end: the ring test after the reset).
 */
int
amdrdna_cp_graphics_reset(
	struct amdrdna_device *device,
	unsigned context)
{
	uint32_t words[AMDRDNA_PM4_WRITE_WORDS + 2U * AMDRDNA_PM4_WAIT_WORDS + AMDRDNA_PM4_ATTACH_WORDS];
	struct amdrdna_engines *engines;
	struct amdrdna_ring *ring;
	volatile uint32_t *slots;
	unsigned long irq;
	uint64_t step_ms[CP_RESET_STEPS];
	uint32_t reset_index;
	uint32_t request;
	uint32_t index;
	unsigned count;
	int error;

	/* Only a queue the scheduler attached is reset through it. */
	engines = &device->engines;
	ring = &engines->graphics;
	if (engines->direct || !engines->scheduler_up)
		return ENOTSUP;
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_CONTEXT_RESET, &reset_index);
	if (error != 0)
		return error;

	/* The queue takes nothing new until it is attached again. */
	ring->up = false;
	step_ms[0] = clock_milliseconds(NULL);

	/* The context's work on queue 0 of the pipe reset; then the image's active word and the register read 0. */
	request = (1U << context) | (1U << (AMDRDNA_GC_CONTEXT_RESET_QUEUES_SHIFT + ring->place.queue));
	count = amdrdna_pm4_write_register(words, reset_index, request, false);
	count += amdrdna_pm4_wait_memory(words + count, ring->image.gpu_address + (uint64_t)AMDRDNA_GRAPHICS_IMAGE_ACTIVE * 4U, 0U, false);
	count += amdrdna_pm4_wait_register(words + count, reset_index, 0U, 0xffffffffU);
	error = scheduler_fenced(device, words, count, "graphics reset");
	if (error != 0)
		return error;
	step_ms[1] = clock_milliseconds(NULL);

	/* The image from its copy, the ring empty: filler words, both pointers 0. */
	irq = spin_lock_irqsave(&ring->lock);

	kern_memcpy(ring->image.dma.address, ring->image_copy, AMDRDNA_QUEUE_IMAGE_WORDS * 4U);
	slots = ring->buffer.dma.address;
	for (index = 0U; index < ring->bytes / 4U; index++)
		slots[index] = amdrdna_pm4_filler();
	ring->write_pointer = 0U;
	kern_memset(ring->done_value, 0, sizeof(ring->done_value));
	kern_memset(ring->done_pointer, 0, sizeof(ring->done_pointer));
	*amdrdna_ring_word(device, ring, CP_WORD_READ) = 0U;
	amdrdna_ring_word(device, ring, CP_WORD_WRITE)[0] = 0U;
	amdrdna_ring_word(device, ring, CP_WORD_WRITE)[1] = 0U;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	spin_unlock_irqrestore(&ring->lock, irq);

	/* The graphics doorbells again, with the queue selected, as at the first attach. */
	mutex_lock(&engines->select);

	select_queue(device, &ring->place, 0U);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_DOORBELLS_LOW, (ring->doorbell << 2) & AMDRDNA_GC_DOORBELL_RANGE_MASK);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_GRAPHICS_DOORBELLS_HIGH, AMDRDNA_GC_DOORBELL_RANGE_MASK);
	select_nothing(device);

	mutex_unlock(&engines->select);

	/* The scheduler attaches the queue again. */
	count = amdrdna_pm4_queue_attach(words, &ring->place, AMDRDNA_QUEUE_ENGINE_GRAPHICS,
	    ring->doorbell, ring->image.gpu_address, ring_gpu_word(device, ring, CP_WORD_WRITE));
	error = scheduler_fenced(device, words, count, "graphics attach after the reset");
	if (error != 0)
		return error;
	step_ms[2] = clock_milliseconds(NULL);

	/* The ring runs commands (a fence alone on it), and its preamble again. */
	error = ring_fenced(device, ring);
	step_ms[3] = clock_milliseconds(NULL);
	if (error == 0)
		error = graphics_preamble(device);
	if (error != 0)
		return error;
	step_ms[4] = clock_milliseconds(NULL);

	/* Succeeded: the queue takes work again; the steps' times are said. */
	ring->up = true;
	amdrdna_mark("graphics queue reset steps: reset %llums, attach %llums, test %llums, preamble %llums",
	    (unsigned long long)(step_ms[1] - step_ms[0]),
	    (unsigned long long)(step_ms[2] - step_ms[1]),
	    (unsigned long long)(step_ms[3] - step_ms[2]),
	    (unsigned long long)(step_ms[4] - step_ms[3]));
	return 0;
}

/*
 * Resets compute queue index after work of a context on it did not end:
 * the context's waves are killed (a wave that never ends keeps the queue
 * from being dequeued), the scheduler
 * detaches the queue with a reset (its work is dropped), the queue is
 * waited for until it is no longer active, its image is put back from the
 * copy kept at the first attach, its ring emptied, and the scheduler
 * attaches it again; a fence alone on it shows it runs.  As for the
 * graphics queue, nothing else was on the ring.
 *
 * Returns 0, ENOTSUP for a queue the scheduler did not attach, or the
 * error of the step that failed (the queue then takes no work).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_reset_kcq: the
 * unmap with the reset action, the wait for the queue's active bit with
 * the queue selected, then gfx_v10_0_kcq_init_queue with the copy and an
 * empty ring, and the map; the safe mode around the wait is for clock
 * gating, which the driver does not turn on) and
 * amdgpu_amdkfd_gfx_v10.c (kgd_wave_control_execute: the command written
 * with every shader array selected, then the selection put back).
 */
int
amdrdna_cp_compute_reset(
	struct amdrdna_device *device,
	unsigned index,
	unsigned context)
{
	uint32_t words[AMDRDNA_PM4_DETACH_WORDS + AMDRDNA_PM4_ATTACH_WORDS];
	struct amdrdna_engines *engines;
	struct amdrdna_ring *ring;
	volatile uint32_t *slots;
	unsigned long irq;
	uint64_t step_ms[CP_RESET_STEPS];
	uint32_t active;
	uint32_t dequeue;
	uint32_t slot;
	unsigned count;
	int error;

	/* Only a queue the scheduler attached is reset through it. */
	engines = &device->engines;
	if (!engines->scheduler_up || !engines->compute_running || index >= engines->compute_count)
		return ENOTSUP;
	ring = &engines->compute[index];

	/* The queue takes nothing new until it is attached again. */
	ring->up = false;
	step_ms[0] = clock_milliseconds(NULL);

	/* The context's waves killed in every shader array. */
	mutex_lock(&engines->select);

	select_all_arrays(device);
	gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_WAVE_COMMAND,
	    AMDRDNA_GC_WAVE_KILL | AMDRDNA_GC_WAVE_EVERY | AMDRDNA_GC_WAVE_OF_CONTEXT | ((uint32_t)context << AMDRDNA_GC_WAVE_CONTEXT_SHIFT));
	select_all_arrays(device);

	mutex_unlock(&engines->select);

	/* The scheduler detaches the queue and drops its work; a detach that does not end shows the queue's state. */
	count = amdrdna_pm4_queue_detach(words, AMDRDNA_QUEUE_ENGINE_COMPUTE, AMDRDNA_QUEUE_DETACH_RESET, ring->doorbell);
	error = scheduler_fenced(device, words, count, "compute reset");
	if (error != 0) {
		mutex_lock(&engines->select);

		select_queue(device, &ring->place, 0U);
		active = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_ACTIVE);
		dequeue = gc_read(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_DEQUEUE);
		select_nothing(device);

		mutex_unlock(&engines->select);

		amdrdna_mark("compute queue %u not detached: active=0x%x dequeue=0x%x", index, active, dequeue);
		return error;
	}

	/* The queue no longer active, read with it selected. */
	mutex_lock(&engines->select);

	select_queue(device, &ring->place, 0U);
	error = gc_wait(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_QUEUE_ACTIVE, 1U, 0U);
	select_nothing(device);

	mutex_unlock(&engines->select);

	if (error != 0) {
		amdrdna_mark("compute queue %u stayed active after its reset (%d)", index, error);
		return error;
	}
	step_ms[1] = clock_milliseconds(NULL);

	/* The image from its copy, the ring empty: filler words, both pointers 0. */
	irq = spin_lock_irqsave(&ring->lock);

	kern_memcpy(ring->image.dma.address, ring->image_copy, AMDRDNA_QUEUE_IMAGE_WORDS * 4U);
	slots = ring->buffer.dma.address;
	for (slot = 0U; slot < ring->bytes / 4U; slot++)
		slots[slot] = amdrdna_pm4_filler();
	ring->write_pointer = 0U;
	kern_memset(ring->done_value, 0, sizeof(ring->done_value));
	kern_memset(ring->done_pointer, 0, sizeof(ring->done_pointer));
	*amdrdna_ring_word(device, ring, CP_WORD_READ) = 0U;
	amdrdna_ring_word(device, ring, CP_WORD_WRITE)[0] = 0U;
	amdrdna_ring_word(device, ring, CP_WORD_WRITE)[1] = 0U;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	spin_unlock_irqrestore(&ring->lock, irq);

	/* The scheduler attaches the queue again. */
	count = amdrdna_pm4_queue_attach(words, &ring->place, AMDRDNA_QUEUE_ENGINE_COMPUTE,
	    ring->doorbell, ring->image.gpu_address, ring_gpu_word(device, ring, CP_WORD_WRITE));
	error = scheduler_fenced(device, words, count, "compute attach after the reset");
	if (error != 0)
		return error;
	step_ms[2] = clock_milliseconds(NULL);

	/* The ring runs commands: a fence alone on it. */
	error = ring_fenced(device, ring);
	if (error != 0)
		return error;
	step_ms[3] = clock_milliseconds(NULL);

	/* Succeeded: the queue takes work again; the steps' times are said. */
	ring->up = true;
	amdrdna_mark("compute queue %u reset steps: reset %llums, attach %llums, test %llums", index,
	    (unsigned long long)(step_ms[1] - step_ms[0]),
	    (unsigned long long)(step_ms[2] - step_ms[1]),
	    (unsigned long long)(step_ms[3] - step_ms[2]));
	return 0;
}

/*
 * Reports the words a ring can take before it would overwrite what the
 * engine has not read: what lies after the read pointer it reported, or
 * after the end of the newest submission whose fence has passed when that
 * is less (the engine reports its read pointer only now and then, so the
 * report may lag the work done).
 */
static unsigned
ring_room(
	struct amdrdna_device *device,
	const struct amdrdna_ring *ring)
{
	uint32_t read_pointer;
	uint32_t fence;
	uint32_t slot;
	uint32_t used;
	uint32_t pending;
	uint32_t words;

	/* The engine's read pointer: dwords, or bytes for the copy engine. */
	read_pointer = *amdrdna_ring_word(device, ring, CP_WORD_READ);
	if (ring->kind == AMDRDNA_RING_COPY)
		read_pointer >>= 2;

	/* What the engine has not read yet, by its report. */
	words = ring->bytes / 4U;
	used = ((uint32_t)ring->write_pointer - read_pointer) & (words - 1U);

	/* Less when the newest passed fence is remembered: the engine read everything up to its submission's end. */
	fence = *amdrdna_ring_word(device, ring, CP_WORD_FENCE);
	slot = fence % AMDRDNA_RING_HISTORY;
	if (fence != 0U &&
	    ring->done_value[slot] == fence &&
	    ring->write_pointer - ring->done_pointer[slot] < words) {
		pending = (uint32_t)(ring->write_pointer - ring->done_pointer[slot]);
		if (pending < used)
			used = pending;
	}

	/* Succeeded: the rest, less one word that keeps a full ring from looking empty. */
	return words - used - 1U;
}

/*
 * Publishes a ring's write pointer: in its copy in memory and through
 * its doorbell (dwords for a command ring, bytes for the copy engine), or
 * for the graphics ring run without the scheduler in its registers.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_set_wptr_gfx,
 * gfx_v10_0_ring_set_wptr_compute) and sdma_v5_0.c
 * (sdma_v5_0_ring_set_wptr).
 */
static void
ring_commit(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring)
{
	volatile uint8_t *doorbells;
	uint64_t *copy;
	uint64_t value;

	/* The value: dwords, or bytes for the copy engine. */
	value = ring->write_pointer;
	if (ring->kind == AMDRDNA_RING_COPY)
		value <<= 2;

	/* The ring's words are written before the engine is told. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The copy the engine polls. */
	copy = (uint64_t *)(uintptr_t)amdrdna_ring_word(device, ring, CP_WORD_WRITE);
	__atomic_store_n(copy, value, __ATOMIC_RELEASE);

	/* The graphics ring run without the scheduler takes its write pointer in its registers. */
	if (ring->kind == AMDRDNA_RING_GRAPHICS && device->engines.direct) {
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_WRITE_LOW, (uint32_t)value);
		gc_write(device, AMDRDNA_GC_WINDOW_MAIN, AMDRDNA_GC_REG_RING_WRITE_HIGH, (uint32_t)(value >> 32));
		return;
	}

	/* The doorbell. */
	doorbells = device->doorbells.address;
	kern_mmio_write64(doorbells + (size_t)ring->doorbell * 4U, value);
}

/* Reports the GPU's address of a word of a ring's write-back slot. */
static uint64_t
ring_gpu_word(
	const struct amdrdna_device *device,
	const struct amdrdna_ring *ring,
	unsigned which)
{
	/* The write-back page's address, the slot and the word. */
	return device->engines.writeback.gpu_address + ring->slot + (uint64_t)which * 4U;
}
