/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A5: the command processor and the copy engine run commands.
 *
 * The stage takes every buffer the engines use first, while only the CPU
 * changes the system-memory table: once the copy engine runs, Navi10 must
 * not have the table's translations dropped from the CPU.  Then it gives
 * the graphics engine its settings, hands the run-list controller its
 * clear state, brings the queues up (cp.c) and the copy engine (sdma.c),
 * and checks what the engines do: a fence with its interrupt on every
 * ring, a copy through VRAM by the copy engine and back by the command
 * processor, a dispatch of a small compute program whose every output
 * word is known, and a write to an address nothing is mapped at, which
 * the graphics hub must report as a page fault.
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
#include <kern/waitq.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-private.h"
#include "drivers/gpu/amdrdna/amdrdna-regs.h"

/* The page, and the bytes of a queue image and of an end-of-pipe buffer. */
#define ENGINES_PAGE_BYTES		4096U
#define ENGINES_IMAGE_BYTES		(AMDRDNA_QUEUE_IMAGE_WORDS * 4U)
#define ENGINES_END_BYTES		2048U

/* The rings' sizes: the graphics ring holds the preamble, the others small submissions. */
#define ENGINES_GRAPHICS_BYTES		65536U
#define ENGINES_RING_BYTES		16384U

/* The rings' slots in the write-back page, 64 bytes each. */
#define ENGINES_SLOT_BYTES		64U
#define ENGINES_SLOT_SCHEDULER		0U
#define ENGINES_SLOT_GRAPHICS		1U
#define ENGINES_SLOT_COMPUTE		2U
#define ENGINES_SLOT_COPY		6U

/*
 * The places of the scheduler's own queue (the second compute engine's
 * pipe 1, queue 0) and of the compute queues (queue 0 of each pipe of the
 * first compute engine).
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/amdgpu_gfx.c
 * (amdgpu_gfx_kiq_acquire, amdgpu_gfx_compute_queue_acquire: the queues
 * the driver takes on Navi10, which its log names as "kiq ring mec 2 pipe
 * 1 q 0" and "comp_1.0.0" to "comp_1.3.0").
 */
#define ENGINES_SCHEDULER_PIPE		1U
#define ENGINES_SCHEDULER_QUEUE		0U
#define ENGINES_COMPUTE_QUEUE		0U

/* The bytes of the copy check, and the words of the dispatch's output buffer. */
#define ENGINES_COPY_BYTES		4096U
#define ENGINES_OUTPUT_BYTES		8192U

/* The places in the program's VRAM: the code at 0, the table at 2048, the workgroup counts at 2304. */
#define ENGINES_TABLE_AT		2048U
#define ENGINES_GROUPS_AT		2304U

/* The workgroups the check dispatches in each direction. */
#define ENGINES_GROUPS			2U

/*
 * The mark the checks fill memory with before an engine overwrites it
 * (the value Linux's ring tests write), the value the fault check writes
 * (the mark, inverted), and the step of the copy check's pattern (an odd
 * number, so the pattern's words all differ).
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_ring_test_ring:
 * 0xDEADBEEF).
 */
#define ENGINES_UNWRITTEN		0xdeadbeefU
#define ENGINES_FAULT_VALUE		(~ENGINES_UNWRITTEN)
#define ENGINES_PATTERN_STEP		2654435769U

/*
 * A GPU address in the system-memory table's reach that nothing is
 * mapped at: the table's guard at its start is never handed out.
 */
#define ENGINES_FAULT_OFFSET		8192U

/* How long a check's fence and its interrupts are waited for. */
#define ENGINES_FENCE_MS		2000U
#define ENGINES_INTERRUPT_MS		200U

/*
 * A clear's fill packets: at most 4 MiB each, at most 16 a submission.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c
 * (sdma_v5_0_buffer_funcs: fill_max_bytes 0x400000).
 */
#define ENGINES_FILL_BYTES		0x400000U
#define ENGINES_FILL_PACKETS		16U

/* How many command errors the log names before it stays quiet. */
#define ENGINES_NAMED_ERRORS		8U

static int engines_prepare(struct amdrdna_device *device);
static int engines_ring(struct amdrdna_device *device, struct amdrdna_ring *ring, unsigned bytes, bool queue_buffers);
static int engines_buffer(struct amdrdna_device *device, struct amdrdna_system_buffer *buffer, unsigned bytes, const char *what);
static int engines_run(struct amdrdna_device *device);
static int engines_fences(struct amdrdna_device *device);
static int engines_fence_one(struct amdrdna_device *device, struct amdrdna_ring *ring);
static int engines_copy(struct amdrdna_device *device);
static int engines_dispatch(struct amdrdna_device *device, struct amdrdna_ring *ring, bool indirect);
static void engines_fault(struct amdrdna_device *device);
static uint64_t engines_ring_interrupts(struct amdrdna_ring *ring);
static uint32_t engines_shader_index(struct amdrdna_device *device, uint32_t offset);

/*
 * Runs stage A5: takes the engines' buffers, brings the command processor
 * and the copy engine up, and checks fences, interrupts, a copy, a
 * dispatch and a page fault.
 *
 * Returns 0 when every check passed, or the error of the step that
 * failed; a failure after the engines were touched stops them again.
 */
int
amdrdna_a5_engines(
	struct amdrdna_device *device)
{
	struct amdrdna_engines *engines;
	const char *arrangement;
	uint64_t graphics;
	uint64_t compute;
	uint64_t copy;
	unsigned index;
	unsigned kiq;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A5 begin");
	engines = &device->engines;

	/* The scheduler's queue runs the queues unless the boot says amdrdna.kiq=0. */
	kiq = amdrdna_parameter_number("amdrdna.kiq", 1U);
	engines->direct = false;
	if (kiq == 0U)
		engines->direct = true;

	/* Takes every buffer before any engine runs. */
	error = engines_prepare(device);
	if (error != 0)
		return error;

	/* Brings the engines up and checks them; a failure stops them. */
	error = engines_run(device);
	if (error != 0) {
		amdrdna_engines_stop(device);
		return error;
	}

	/* The interrupts of the graphics ring, of every compute queue and of the copy engine. */
	graphics = engines_ring_interrupts(&engines->graphics);
	compute = 0U;
	for (index = 0U; index < engines->compute_count; index++)
		compute += engines_ring_interrupts(&engines->compute[index]);
	copy = engines_ring_interrupts(&engines->copy);

	/* Names how the queues run. */
	arrangement = "kiq";
	if (engines->direct)
		arrangement = "direct";

	/* Marks the result. */
	amdrdna_mark("A5 ok %s gfx compute=%u sdma0 fences=%u irq=gfx:%llu compute:%llu copy:%llu copied=%uB dispatch=%u vmfault=%u cu=%u rb=0x%x tcc_off=0x%x",
	    arrangement,
	    engines->compute_count,
	    (unsigned)engines->fences,
	    (unsigned long long)graphics,
	    (unsigned long long)compute,
	    (unsigned long long)copy,
	    (unsigned)engines->copied_bytes,
	    (unsigned)engines->dispatch_words,
	    (unsigned)engines->faults_seen,
	    engines->units,
	    (unsigned)engines->backends,
	    (unsigned)engines->channels_off);

	/* Starts and tests the video engine last; its failure leaves it off, not the stage. */
	amdrdna_vcn_start(device);

	/* Succeeded: the engines run commands. */
	return 0;
}

/*
 * Stops the engines stage A5 started: the video engine, the command
 * processor's queues and engines, then the copy engine.  Running it twice
 * does nothing the second time.
 */
void
amdrdna_engines_stop(
	struct amdrdna_device *device)
{
	/* The video engine first: its work may wait on nothing else. */
	amdrdna_vcn_stop(device);

	/* Nothing else was started without the stage's buffers. */
	if (!device->engines.buffers_ready)
		return;

	/* The command processor first, then the copy engine. */
	amdrdna_cp_stop(device);
	amdrdna_sdma_stop(device);

	/* Marks the stop, for the log of the reboot. */
	amdrdna_mark("shutdown: A5 undone, queues detached, engines halted");
}

/*
 * Zeroes a run of VRAM with the copy engine (VRAM the CPU's window does
 * not reach), and waits until it is done.  vram and bytes are whole words.
 *
 * Returns 0, EBUSY when the copy engine does not run, or the ring's error.
 */
int
amdrdna_engines_clear(
	struct amdrdna_device *device,
	uint64_t vram,
	uint64_t bytes)
{
	uint32_t words[ENGINES_FILL_PACKETS * AMDRDNA_COPY_FILL_WORDS];
	struct amdrdna_engines *engines;
	uint64_t address;
	uint64_t left;
	uint32_t piece;
	uint32_t sequence;
	unsigned count;
	unsigned packets;
	int error;

	/* The copy engine must run. */
	engines = &device->engines;
	if (!engines->copy_running || !engines->copy.up)
		return EBUSY;

	/* The run in fills of at most 4 MiB, a submission of at most 16 fills at a time. */
	address = device->frame_buffer_base + vram;
	left = bytes;
	while (left != 0U) {
		count = 0U;
		for (packets = 0U; packets < ENGINES_FILL_PACKETS && left != 0U; packets++) {
			piece = ENGINES_FILL_BYTES;
			if (left < piece)
				piece = (uint32_t)left;
			count += amdrdna_copy_fill(words + count, address, 0U, piece);
			address += piece;
			left -= piece;
		}

		/* The submission, and its fence. */
		error = amdrdna_ring_submit(device, &engines->copy, words, count, false, &sequence);
		if (error == 0)
			error = amdrdna_ring_wait(device, &engines->copy, sequence, ENGINES_FENCE_MS);
		if (error != 0)
			return error;
	}

	/* Succeeded: the run reads as zeroes. */
	return 0;
}

/*
 * Handles an interrupt of the engines: counts a ring's end-of-pipe event
 * or the copy engine's trap, and names the first command errors.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c (gfx_v10_0_eop_irq,
 * gfx_v10_0_priv_reg_irq, gfx_v10_0_bad_op_irq, gfx_v10_0_priv_inst_irq)
 * and sdma_v5_0.c (sdma_v5_0_process_trap_irq).
 */
void
amdrdna_engines_interrupt(
	struct amdrdna_device *device,
	const struct amdrdna_interrupt *interrupt)
{
	struct amdrdna_engines *engines;
	struct amdrdna_ring *ring;
	unsigned long irq;
	unsigned engine;
	unsigned pipe;
	unsigned queue;

	/* Another event of the copy engine is counted as an error. */
	engines = &device->engines;
	if (interrupt->client == AMDRDNA_CLIENT_COPY0 && interrupt->source != AMDRDNA_SOURCE_COPY_TRAP) {
		engines->command_errors++;
		return;
	}

	/* The copy engine's trap. */
	if (interrupt->client == AMDRDNA_CLIENT_COPY0) {
		irq = spin_lock_irqsave(&engines->copy.lock);

		engines->copy.interrupts++;
		engines->copy_traps++;

		spin_unlock_irqrestore(&engines->copy.lock, irq);
		return;
	}

	/* A command error: the context's space is lost; counted, and the first ones named. */
	if (interrupt->source != AMDRDNA_SOURCE_END_OF_PIPE) {
		engines->command_errors++;
		amdrdna_space_command_error(device, interrupt->context, interrupt->source);
		if (engines->command_errors > ENGINES_NAMED_ERRORS)
			return;
		amdrdna_mark("cp error source=%u ring=0x%x ctx=%u data=0x%08x",
		    (unsigned)interrupt->source,
		    (unsigned)interrupt->ring,
		    (unsigned)interrupt->context,
		    (unsigned)interrupt->data[0]);
		return;
	}

	/* The ring the end-of-pipe event names. */
	engine = (interrupt->ring & AMDRDNA_RING_FIELD_ENGINE_MASK) >> AMDRDNA_RING_FIELD_ENGINE_SHIFT;
	pipe = interrupt->ring & AMDRDNA_RING_FIELD_PIPE_MASK;
	queue = (interrupt->ring & AMDRDNA_RING_FIELD_QUEUE_MASK) >> AMDRDNA_RING_FIELD_QUEUE_SHIFT;
	ring = NULL;
	if (engine == AMDRDNA_ENGINE_GRAPHICS) {
		ring = &engines->graphics;
	} else if (engine == AMDRDNA_ENGINE_COMPUTE_FIRST &&
	    queue == ENGINES_COMPUTE_QUEUE &&
	    pipe < engines->compute_count) {
		ring = &engines->compute[pipe];
	}

	/* An event of a ring the driver does not run is only counted as an error. */
	if (ring == NULL) {
		engines->command_errors++;
		return;
	}

	/* Counts the ring's event and wakes the waiters of its fence, which the event follows. */
	irq = spin_lock_irqsave(&ring->lock);

	ring->interrupts++;
	waitq_wake_all(&ring->fences);

	spin_unlock_irqrestore(&ring->lock, irq);
}

/*
 * Loads the check program into a page of VRAM the CPU writes at vram and
 * the GPU reads at code_address: the code and its padding at the start,
 * the table of the output's and the counts' descriptors, and the workgroup
 * counts.  The output is output_words words at output_address.
 */
void
amdrdna_check_load(
	volatile uint8_t *vram,
	uint64_t code_address,
	uint64_t output_address,
	uint32_t output_words,
	const uint32_t *groups)
{
	struct amdrdna_check_description program;
	uint32_t descriptor[4];
	uint64_t groups_address;
	unsigned index;

	/* The program's code and padding at the start of the page. */
	amdrdna_check_program(&program);
	for (index = 0U; index < program.padded_words; index++) {
		if (index < program.code_words) {
			kern_mmio_write32(vram + (size_t)index * 4U, program.code[index]);
		} else {
			kern_mmio_write32(vram + (size_t)index * 4U, program.end_word);
		}
	}

	/* The workgroup counts. */
	for (index = 0U; index < 3U; index++)
		kern_mmio_write32(vram + ENGINES_GROUPS_AT + index * 4U, groups[index]);

	/* The table: the output's descriptor and the counts' descriptor. */
	groups_address = code_address + ENGINES_GROUPS_AT;
	amdrdna_raw_buffer_descriptor(output_address, output_words * 4U, descriptor);
	for (index = 0U; index < 4U; index++)
		kern_mmio_write32(vram + ENGINES_TABLE_AT + program.output_at + index * 4U, descriptor[index]);
	amdrdna_raw_buffer_descriptor(groups_address, 12U, descriptor);
	for (index = 0U; index < 4U; index++)
		kern_mmio_write32(vram + ENGINES_TABLE_AT + program.groups_at + index * 4U, descriptor[index]);
}

/*
 * Writes the commands that dispatch the check program loaded at
 * code_address: the caches synced, so the program, the table and the
 * counts are read from memory; the program's address, counts, inputs,
 * limits, start and workgroup size; the table's address in the first two
 * user data words; and the workgroups.  compute marks a compute queue's
 * cache sync.  Reports the words written (at most
 * AMDRDNA_CHECK_COMMAND_WORDS).
 */
unsigned
amdrdna_check_commands(
	struct amdrdna_device *device,
	uint32_t *words,
	uint64_t code_address,
	const uint32_t *groups,
	bool compute)
{
	struct amdrdna_check_description program;
	uint32_t values[8];
	uint64_t table_address;
	unsigned count;

	/* The program and its table's address. */
	amdrdna_check_program(&program);
	table_address = code_address + ENGINES_TABLE_AT;

	/* The caches synced, so the program, the table and the counts are read from memory. */
	count = amdrdna_pm4_cache_sync(words, compute);

	/* The program's address. */
	values[0] = (uint32_t)(code_address >> 8);
	values[1] = (uint32_t)(code_address >> 40);
	count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_SHADER, engines_shader_index(device, AMDRDNA_GC_REG_DISPATCH_PROGRAM), values, 2U, false);

	/* The counts and the inputs the compiler gave. */
	values[0] = program.counts;
	values[1] = program.inputs;
	count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_SHADER, engines_shader_index(device, AMDRDNA_GC_REG_DISPATCH_COUNTS), values, 2U, false);

	/* No third control word. */
	values[0] = 0U;
	count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_SHADER, engines_shader_index(device, AMDRDNA_GC_REG_DISPATCH_MORE), values, 1U, false);

	/* No limits, every compute unit of the four shader engines, no scratch. */
	values[0] = 0U;
	values[1] = 0xffffffffU;
	values[2] = 0xffffffffU;
	values[3] = 0U;
	values[4] = 0xffffffffU;
	values[5] = 0xffffffffU;
	count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_SHADER, engines_shader_index(device, AMDRDNA_GC_REG_DISPATCH_LIMITS), values, 6U, false);

	/* The dispatch starts at workgroup 0, with the program's workgroup size. */
	values[0] = 0U;
	values[1] = 0U;
	values[2] = 0U;
	values[3] = program.local_size[0];
	values[4] = program.local_size[1];
	values[5] = program.local_size[2];
	count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_SHADER, engines_shader_index(device, AMDRDNA_GC_REG_DISPATCH_START), values, 6U, false);

	/* The table's address in the first two user data words. */
	values[0] = (uint32_t)table_address;
	values[1] = (uint32_t)(table_address >> 32);
	count += amdrdna_pm4_registers(words + count, AMDRDNA_SPACE_SHADER, engines_shader_index(device, AMDRDNA_GC_REG_DISPATCH_USER), values, 2U, false);

	/* The workgroups. */
	count += amdrdna_pm4_dispatch(words + count, groups[0], groups[1], groups[2], program.initiator);

	/* Succeeded: the words. */
	return count;
}

/*
 * Describes the rings and takes every buffer the stage uses: the
 * write-back page, each ring with its queue image and end-of-pipe buffer,
 * and the checks' buffers in system memory and VRAM.  Each is taken once
 * for the record's life.
 *
 * Returns 0, or the error of the buffer that could not be had.
 */
static int
engines_prepare(
	struct amdrdna_device *device)
{
	struct amdrdna_engines *engines;
	struct amdrdna_ring *ring;
	unsigned index;
	int error;

	/* The write-back page, zeroed. */
	engines = &device->engines;
	error = engines_buffer(device, &engines->writeback, ENGINES_PAGE_BYTES, "write-back page");
	if (error != 0)
		return error;

	/* The scheduler's own queue: on the second compute engine. */
	ring = &engines->scheduler;
	ring->name = "kiq";
	ring->kind = AMDRDNA_RING_SCHEDULER;
	ring->place.engine = AMDRDNA_ENGINE_COMPUTE_SECOND;
	ring->place.pipe = ENGINES_SCHEDULER_PIPE;
	ring->place.queue = ENGINES_SCHEDULER_QUEUE;
	ring->doorbell = AMDRDNA_DOORBELL_SCHEDULER;
	ring->slot = ENGINES_SLOT_SCHEDULER * ENGINES_SLOT_BYTES;

	/* The graphics queue. */
	ring = &engines->graphics;
	ring->name = "gfx";
	ring->kind = AMDRDNA_RING_GRAPHICS;
	ring->place.engine = AMDRDNA_ENGINE_GRAPHICS;
	ring->place.pipe = 0U;
	ring->place.queue = 0U;
	ring->doorbell = AMDRDNA_DOORBELL_GRAPHICS;
	ring->slot = ENGINES_SLOT_GRAPHICS * ENGINES_SLOT_BYTES;

	/* The compute queues: queue 0 of each pipe of the first compute engine, without the scheduler none. */
	engines->compute_count = AMDRDNA_COMPUTE_QUEUES;
	if (engines->direct)
		engines->compute_count = 0U;
	for (index = 0U; index < AMDRDNA_COMPUTE_QUEUES; index++) {
		ring = &engines->compute[index];
		ring->name = "compute";
		ring->kind = AMDRDNA_RING_COMPUTE;
		ring->place.engine = AMDRDNA_ENGINE_COMPUTE_FIRST;
		ring->place.pipe = index;
		ring->place.queue = ENGINES_COMPUTE_QUEUE;
		ring->doorbell = AMDRDNA_DOORBELL_COMPUTE_FIRST + 2U * index;
		ring->slot = (ENGINES_SLOT_COMPUTE + index) * ENGINES_SLOT_BYTES;
	}

	/* The first copy engine's queue. */
	ring = &engines->copy;
	ring->name = "sdma0";
	ring->kind = AMDRDNA_RING_COPY;
	ring->doorbell = AMDRDNA_DOORBELL_COPY;
	ring->slot = ENGINES_SLOT_COPY * ENGINES_SLOT_BYTES;

	/* The scheduler's ring, image and end-of-pipe buffer, when it is used. */
	if (!engines->direct) {
		error = engines_ring(device, &engines->scheduler, ENGINES_RING_BYTES, true);
		if (error != 0)
			return error;
	}

	/* The graphics ring and its image. */
	error = engines_ring(device, &engines->graphics, ENGINES_GRAPHICS_BYTES, true);
	if (error != 0)
		return error;

	/* Each compute queue's ring, image and end-of-pipe buffer. */
	for (index = 0U; index < engines->compute_count; index++) {
		/* One queue's buffers. */
		error = engines_ring(device, &engines->compute[index], ENGINES_RING_BYTES, true);
		if (error != 0)
			return error;
	}

	/* The copy engine's ring. */
	error = engines_ring(device, &engines->copy, ENGINES_RING_BYTES, false);
	if (error != 0)
		return error;

	/* The checks' buffers in system memory. */
	error = engines_buffer(device, &engines->source, ENGINES_COPY_BYTES, "copy source");
	if (error == 0)
		error = engines_buffer(device, &engines->destination, ENGINES_COPY_BYTES, "copy destination");
	if (error == 0)
		error = engines_buffer(device, &engines->output, ENGINES_OUTPUT_BYTES, "dispatch output");
	if (error == 0)
		error = engines_buffer(device, &engines->indirect, ENGINES_PAGE_BYTES, "indirect buffer");
	if (error != 0)
		return error;

	/* The checks' VRAM: the program's page and the copy's middle stop. */
	if (engines->program_vram == 0U) {
		error = amdrdna_vram_take(device, true, ENGINES_PAGE_BYTES, ENGINES_PAGE_BYTES, &engines->program_vram);
		if (error == 0)
			error = amdrdna_vram_take(device, true, ENGINES_COPY_BYTES, ENGINES_PAGE_BYTES, &engines->middle_vram);
		if (error != 0) {
			amdrdna_mark("A5 fail: no VRAM for the checks (%d)", error);
			return error;
		}
	}

	/* The video engine's memory, while only the CPU changes the system-memory table; without it the engine stays off. */
	amdrdna_vcn_prepare(device);

	/* The stop may now undo what the stage starts. */
	engines->buffers_ready = true;

	/* Succeeded: every buffer is mapped. */
	return 0;
}

/*
 * Takes one ring's buffers: the ring itself and, for a command queue, its
 * queue image (and the image's copy in kernel memory) and its end-of-pipe
 * buffer.
 */
static int
engines_ring(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring,
	unsigned bytes,
	bool queue_buffers)
{
	int error;

	/* The ring. */
	ring->bytes = bytes;
	error = engines_buffer(device, &ring->buffer, bytes, ring->name);
	if (error != 0)
		return error;

	/*
	 * The ring starts empty, as the engine's own pointers do after a
	 * reset: the write pointer and the sequence from 0 again (the
	 * write-back page that holds the fence is zeroed too), not running.
	 */
	ring->write_pointer = 0U;
	ring->sequence = 0U;
	kern_memset(ring->done_value, 0, sizeof(ring->done_value));
	kern_memset(ring->done_pointer, 0, sizeof(ring->done_pointer));
	ring->up = false;

	/* A ring without a queue image is done. */
	if (!queue_buffers)
		return 0;

	/* The queue image the firmware reads, and the driver's copy of it. */
	error = engines_buffer(device, &ring->image, ENGINES_PAGE_BYTES, "queue image");
	if (error != 0)
		return error;
	if (ring->image_copy == NULL) {
		ring->image_copy = kern_calloc(1U, ENGINES_IMAGE_BYTES);
		if (ring->image_copy == NULL)
			return ENOMEM;
	}

	/* The end-of-pipe buffer of a compute engine's queue. */
	if (ring->kind == AMDRDNA_RING_GRAPHICS)
		return 0;
	error = engines_buffer(device, &ring->end_of_pipe, ENGINES_PAGE_BYTES, "end-of-pipe buffer");
	if (error != 0)
		return error;

	/* Succeeded: the ring's buffers are mapped. */
	return 0;
}

/*
 * Allocates a buffer of system memory once for the record's life, maps it
 * for the GPU at a page boundary, and zeroes it.
 */
static int
engines_buffer(
	struct amdrdna_device *device,
	struct amdrdna_system_buffer *buffer,
	unsigned bytes,
	const char *what)
{
	int error;

	/* A buffer kept from an earlier run is only zeroed again. */
	if (!buffer->allocated) {
		error = drv_dma_alloc_coherent(device->dma, bytes, ENGINES_PAGE_BYTES, &buffer->dma);
		if (error != 0) {
			amdrdna_mark("A5 fail: %s not allocated (%d)", what, error);
			return error;
		}

		/* The buffer is the GPU's for the record's life. */
		buffer->allocated = true;

		/* Maps it for the GPU. */
		error = amdrdna_gart_map(device, buffer, ENGINES_PAGE_BYTES);
		if (error != 0) {
			amdrdna_mark("A5 fail: %s not mapped (%d)", what, error);
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
 * Brings the engines up and runs the checks, stopping at the first step
 * that fails.
 */
static int
engines_run(
	struct amdrdna_device *device)
{
	int error;

	/* The graphics engine's settings and shape. */
	error = amdrdna_cp_settings(device);
	if (error != 0)
		return error;

	/* The run-list controller's load and clear state. */
	error = amdrdna_cp_run_list(device);
	if (error != 0)
		return error;

	/* The command processor's queues. */
	error = amdrdna_cp_start(device);
	if (error != 0)
		return error;

	/* The copy engine. */
	error = amdrdna_sdma_start(device);
	if (error != 0)
		return error;

	/* A fence with its interrupt on every ring. */
	error = engines_fences(device);
	if (error != 0)
		return error;

	/* A copy through VRAM and back. */
	error = engines_copy(device);
	if (error != 0)
		return error;

	/* A dispatch of the check program on the first compute queue. */
	if (device->engines.compute_count != 0U) {
		error = engines_dispatch(device, &device->engines.compute[0], false);
		if (error != 0)
			return error;
	}

	/* The same dispatch on the graphics ring, from an indirect buffer as the executor will run it. */
	error = engines_dispatch(device, &device->engines.graphics, true);
	if (error != 0)
		return error;

	/* A page fault, reported or not. */
	engines_fault(device);

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Runs an empty submission with a fence and its interrupt on every ring
 * that runs, waits for each fence, then gives the interrupts a moment to
 * arrive and marks how many each ring got.  A missing interrupt is only
 * marked; a missing fence fails the stage.
 */
static int
engines_fences(
	struct amdrdna_device *device)
{
	struct amdrdna_engines *engines;
	unsigned index;
	int error;

	/* The graphics ring. */
	engines = &device->engines;
	error = engines_fence_one(device, &engines->graphics);
	if (error != 0)
		return error;

	/* Every compute queue. */
	for (index = 0U; index < engines->compute_count; index++) {
		/* One queue's fence. */
		error = engines_fence_one(device, &engines->compute[index]);
		if (error != 0)
			return error;
	}

	/* The copy engine. */
	error = engines_fence_one(device, &engines->copy);
	if (error != 0)
		return error;

	/* Gives the interrupts a moment to arrive. */
	kern_usleep_range(ENGINES_INTERRUPT_MS * 1000U, ENGINES_INTERRUPT_MS * 1000U + 1000U);

	/* Marks what arrived. */
	amdrdna_mark("A5 fences %u, interrupts gfx=%llu compute=%llu/%llu/%llu/%llu sdma0=%llu errors=%llu",
	    (unsigned)engines->fences,
	    (unsigned long long)engines_ring_interrupts(&engines->graphics),
	    (unsigned long long)engines_ring_interrupts(&engines->compute[0]),
	    (unsigned long long)engines_ring_interrupts(&engines->compute[1]),
	    (unsigned long long)engines_ring_interrupts(&engines->compute[2]),
	    (unsigned long long)engines_ring_interrupts(&engines->compute[3]),
	    (unsigned long long)engines_ring_interrupts(&engines->copy),
	    (unsigned long long)engines->command_errors);

	/* Succeeded: every fence came. */
	return 0;
}

/* Runs one empty submission with a fence and its interrupt, and waits for the fence. */
static int
engines_fence_one(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring)
{
	uint32_t sequence;
	int error;

	/* Submits the fence. */
	error = amdrdna_ring_submit(device, ring, NULL, 0U, true, &sequence);
	if (error != 0)
		return error;

	/* Waits for it. */
	error = amdrdna_ring_wait(device, ring, sequence, ENGINES_FENCE_MS);
	if (error != 0) {
		amdrdna_mark("A5 fail: %s fence did not come (%d)", ring->name, error);
		return error;
	}

	/* Counts it. */
	device->engines.fences++;

	/* Succeeded: the fence came. */
	return 0;
}

/*
 * Copies a page with a known pattern from system memory to VRAM with the
 * copy engine, reads the VRAM back through the CPU's window, then copies
 * it from VRAM to another page of system memory with the command
 * processor, and compares both with the pattern.
 */
static int
engines_copy(
	struct amdrdna_device *device)
{
	uint32_t words[AMDRDNA_PM4_MOVE_WORDS];
	struct amdrdna_engines *engines;
	volatile uint8_t *middle;
	uint32_t *source;
	uint32_t *destination;
	uint64_t middle_address;
	uint32_t sequence;
	uint32_t word;
	unsigned count;
	unsigned index;
	int error;

	/* The pattern in the source, the destination and the VRAM page marked unwritten. */
	engines = &device->engines;
	source = engines->source.dma.address;
	destination = engines->destination.dma.address;
	middle = amdrdna_vram_cpu(device, engines->middle_vram);
	for (index = 0U; index < ENGINES_COPY_BYTES / 4U; index++) {
		source[index] = (index * ENGINES_PATTERN_STEP) ^ ENGINES_UNWRITTEN;
		destination[index] = ENGINES_UNWRITTEN;
		kern_mmio_write32(middle + (size_t)index * 4U, ENGINES_UNWRITTEN);
	}

	/* The GPU must see what the CPU wrote, in memory and in VRAM. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	amdrdna_host_path_flush(device);

	/* The copy engine copies the source to VRAM. */
	middle_address = device->frame_buffer_base + engines->middle_vram;
	count = amdrdna_copy_linear(words, engines->source.gpu_address, middle_address, ENGINES_COPY_BYTES);
	error = amdrdna_ring_submit(device, &engines->copy, words, count, true, &sequence);
	if (error == 0)
		error = amdrdna_ring_wait(device, &engines->copy, sequence, ENGINES_FENCE_MS);
	if (error != 0) {
		amdrdna_mark("A5 fail: sdma0 copy did not finish (%d)", error);
		return error;
	}

	/* Reads the VRAM page back through the window. */
	amdrdna_host_path_drop(device);
	for (index = 0U; index < ENGINES_COPY_BYTES / 4U; index++) {
		/* Each word must be the pattern's. */
		word = kern_mmio_read32(middle + (size_t)index * 4U);
		if (word != source[index]) {
			amdrdna_mark("A5 fail: sdma0 copy word %u is 0x%08x, not 0x%08x", index, (unsigned)word, (unsigned)source[index]);
			return EIO;
		}
	}

	/* The command processor copies the VRAM page to the destination. */
	count = amdrdna_pm4_move(words, middle_address, engines->destination.gpu_address, ENGINES_COPY_BYTES);
	error = amdrdna_ring_submit(device, &engines->graphics, words, count, true, &sequence);
	if (error == 0)
		error = amdrdna_ring_wait(device, &engines->graphics, sequence, ENGINES_FENCE_MS);
	if (error != 0) {
		amdrdna_mark("A5 fail: gfx copy did not finish (%d)", error);
		return error;
	}

	/* Compares the destination with the pattern. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (index = 0U; index < ENGINES_COPY_BYTES / 4U; index++) {
		/* Each word must be the pattern's. */
		if (destination[index] != source[index]) {
			amdrdna_mark("A5 fail: gfx copy word %u is 0x%08x, not 0x%08x", index, (unsigned)destination[index], (unsigned)source[index]);
			return EIO;
		}
	}

	/* Records and marks the result. */
	engines->copied_bytes = 2U * ENGINES_COPY_BYTES;
	amdrdna_mark("A5 copy ok: sdma0 system->vram %uB, gfx vram->system %uB", ENGINES_COPY_BYTES, ENGINES_COPY_BYTES);

	/* Succeeded: both copies match. */
	return 0;
}

/*
 * Dispatches the check program on a ring, directly or from an indirect
 * buffer of context 0 the ring points at: its code, its table and the
 * workgroup counts in VRAM, its output in system memory marked unwritten;
 * the caches synced, the program's registers set, the workgroups
 * launched, and every output word compared with what the program writes.
 */
static int
engines_dispatch(
	struct amdrdna_device *device,
	struct amdrdna_ring *ring,
	bool indirect)
{
	uint32_t words[AMDRDNA_CHECK_COMMAND_WORDS];
	uint32_t groups[3];
	uint32_t pointer[AMDRDNA_PM4_INDIRECT_WORDS];
	struct amdrdna_check_description program;
	struct amdrdna_engines *engines;
	volatile uint8_t *vram;
	const char *path;
	uint32_t *buffer;
	uint32_t *output;
	uint64_t code_address;
	uint32_t output_words;
	uint32_t bad_word;
	uint32_t expected;
	uint32_t found;
	uint32_t sequence;
	unsigned count;
	unsigned index;
	bool compute;
	bool matched;
	int error;

	/* The program, whether the ring is a compute queue, and the workgroups. */
	engines = &device->engines;
	amdrdna_check_program(&program);
	compute = false;
	if (ring->kind == AMDRDNA_RING_COMPUTE)
		compute = true;
	groups[0] = ENGINES_GROUPS;
	groups[1] = ENGINES_GROUPS;
	groups[2] = ENGINES_GROUPS;

	/* The output marked unwritten. */
	output = engines->output.dma.address;
	output_words = amdrdna_check_output_words(groups);
	for (index = 0U; index < output_words; index++)
		output[index] = ENGINES_UNWRITTEN;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The program's code, its table and the workgroup counts in its VRAM page. */
	vram = amdrdna_vram_cpu(device, engines->program_vram);
	code_address = device->frame_buffer_base + engines->program_vram;
	amdrdna_check_load(vram, code_address, engines->output.gpu_address, output_words, groups);

	/* The GPU must read what the CPU wrote to VRAM. */
	amdrdna_host_path_flush(device);

	/* The caches synced, the program's registers and the workgroups. */
	count = amdrdna_check_commands(device, words, code_address, groups, compute);

	/* Runs it on the ring, or from the indirect buffer the ring points at. */
	if (indirect) {
		buffer = engines->indirect.dma.address;
		for (index = 0U; index < count; index++)
			buffer[index] = words[index];
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		count = amdrdna_pm4_indirect(pointer, engines->indirect.gpu_address, count, 0U, compute);
		error = amdrdna_ring_submit(device, ring, pointer, count, true, &sequence);
	} else {
		error = amdrdna_ring_submit(device, ring, words, count, true, &sequence);
	}

	/* Waits for the fence. */
	if (error == 0)
		error = amdrdna_ring_wait(device, ring, sequence, ENGINES_FENCE_MS);
	if (error != 0) {
		amdrdna_mark("A5 fail: dispatch on %s did not finish (%d)", ring->name, error);
		return error;
	}

	/* Compares every output word. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	matched = amdrdna_check_output(output, groups, &bad_word, &expected, &found);
	if (!matched) {
		amdrdna_mark("A5 fail: dispatch word %u is 0x%08x, not 0x%08x", (unsigned)bad_word, (unsigned)found, (unsigned)expected);
		return EIO;
	}

	/* Names how the dispatch reached the ring. */
	path = "the ring";
	if (indirect)
		path = "an indirect buffer";

	/* Records and marks the result. */
	engines->dispatch_words += output_words;
	amdrdna_mark("A5 dispatch ok on %s from %s: %ux%ux%u groups of %ux%ux%u, %u words", ring->name, path,
	    groups[0], groups[1], groups[2],
	    program.local_size[0], program.local_size[1], program.local_size[2],
	    (unsigned)output_words);

	/* Succeeded: every output word is what the program writes. */
	return 0;
}

/*
 * Writes one word to an address of the system-memory table's reach that
 * nothing is mapped at, from the last compute queue (the graphics ring
 * without the scheduler), and waits for the graphics hub to report the
 * page fault.  The hub sends the write to the default page and the queue
 * goes on.  The result is only marked: the count of faults it saw.
 */
static void
engines_fault(
	struct amdrdna_device *device)
{
	uint32_t words[AMDRDNA_PM4_WRITE_WORDS];
	struct amdrdna_engines *engines;
	struct amdrdna_ring *ring;
	uint64_t before;
	uint64_t after;
	uint64_t address;
	uint32_t sequence;
	unsigned count;
	unsigned waited;
	int error;

	/* The ring, and the faults counted so far. */
	engines = &device->engines;
	ring = &engines->graphics;
	if (engines->compute_count != 0U)
		ring = &engines->compute[engines->compute_count - 1U];
	before = device->interrupts.faults;

	/* The write to the unmapped address, with a fence after it. */
	address = device->gart_start + ENGINES_FAULT_OFFSET;
	count = amdrdna_pm4_write_memory(words, address, ENGINES_FAULT_VALUE, false);
	error = amdrdna_ring_submit(device, ring, words, count, false, &sequence);
	if (error == 0)
		error = amdrdna_ring_wait(device, ring, sequence, ENGINES_FENCE_MS);
	if (error != 0)
		amdrdna_mark("A5 vm fault check: %s did not get past the write (%d)", ring->name, error);

	/* Waits for the fault's interrupt. */
	after = before;
	for (waited = 0U; waited < ENGINES_INTERRUPT_MS; waited++) {
		/* Stops once a fault was counted. */
		after = device->interrupts.faults;
		if (after != before)
			break;

		/* Sleeps a millisecond. */
		kern_usleep_range(1000U, 1500U);
	}

	/* Records and marks the result. */
	engines->faults_seen = (uint32_t)(after - before);
	amdrdna_mark("A5 vm fault check: write to 0x%llx from %s, faults reported %u",
	    (unsigned long long)address,
	    ring->name,
	    (unsigned)engines->faults_seen);
}

/* Reports the interrupts a ring counted so far. */
static uint64_t
engines_ring_interrupts(
	struct amdrdna_ring *ring)
{
	unsigned long irq;
	uint64_t count;

	/* The count, read under the ring's lock. */
	irq = spin_lock_irqsave(&ring->lock);

	count = ring->interrupts;

	spin_unlock_irqrestore(&ring->lock, irq);

	/* Succeeded: the count. */
	return count;
}

/* Reports the absolute index of a register of the graphics block's first window. */
static uint32_t
engines_shader_index(
	struct amdrdna_device *device,
	uint32_t offset)
{
	uint32_t index;
	int error;

	/* The window's start plus the offset; the stage checked the window exists. */
	error = amdrdna_block_index(device, AMDRDNA_BLOCK_GC, AMDRDNA_GC_WINDOW_MAIN, offset, &index);
	if (error != 0)
		return 0U;

	/* Succeeded: the index. */
	return index;
}
