/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The command streams the driver runs: one stream of one group slot
 * each, its ring of instructions, the doorbell that tells the firmware
 * the ring grew, and the wait for a sync object the stream adds to.
 *
 * The driver's own stream is stream 0 of group slot 0, in an address
 * space of its own (slot 1), where its ring buffer lives; the ring's two
 * small blocks and the group's suspend buffers live in the
 * microcontroller's space.  Starting it on an empty ring shows the whole
 * path from the host to the firmware's scheduler works, and a first run
 * of a NOP and a sync add shows the front end reads the ring.  The
 * render node's stream (sched.c) is another of the same kind.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/cs/cs-build.h"
#include "drivers/gpu/mali/mali-private.h"
#include "drivers/gpu/mali/mali-regs.h"

/* How long a poll sleeps before the next read, in milliseconds (a tick at least). */
#define MALI_QUEUE_POLL_MS 1U

/* The driver's ring buffer's size (64 KiB), and the two blocks after it: the input page, then the output page. */
#define RING_BYTES		0x10000ULL
#define RING_BLOCKS_BYTES	0x2000ULL
#define RING_OUTPUT_OFFSET	0x1000U

/* The driver's stream's doorbell: the first after the global one. */
#define QUEUE_DOORBELL		1U

/* A stream's priority among its group's streams. */
#define QUEUE_PRIORITY		1U

/* How long the firmware has to acknowledge a start or a stop (100 ms). */
#define START_WAIT_MS		100U

/* How long the self-test's instructions may take (1 s). */
#define SELF_TEST_WAIT_MS	1000U

/*
 * The registers the ring's own instructions use: the last four of the
 * stream's file, which the work a ring calls does not keep.
 */
#define RING_SCRATCH_REGISTERS	4U

/*
 * A stream's fault and fatal requests, and the group's terminate state.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (CS_FATAL, CS_FAULT,
 * CSG_STATE_TERMINATE).
 */
#define STREAM_FATAL		0x80000000U
#define STREAM_FAULT		0x40000000U
#define GROUP_STATE_TERMINATE	0x00000000U

/*
 * The output words of a stream that say where it stopped and why.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.h (struct
 * panthor_fw_cs_output_iface: status_wait 0x48, status_req_resource 0x4c,
 * status_blocked_reason 0x60, fault_info 0x88, fatal_info 0x90).
 */
#define STREAM_OUTPUT_WAIT		0x48U
#define STREAM_OUTPUT_RESOURCE		0x4cU
#define STREAM_OUTPUT_BLOCKED		0x60U
#define STREAM_OUTPUT_FAULT_INFO	0x88U
#define STREAM_OUTPUT_FATAL_INFO	0x90U

static int make_buffers(struct mali_device *device, struct mali_queue *queue, const struct mali_group_interface *group);
static void program_stream(struct mali_queue *queue, struct mali_stream_interface *stream);
static void program_group(struct mali_device *device, struct mali_queue *queue, struct mali_group_interface *group);
static int wait_stream_started(struct mali_stream_interface *stream, unsigned milliseconds);
static int take_faults(struct mali_device *device, struct mali_queue *queue, struct mali_queue_fault *fault);
static int self_test(struct mali_device *device);

/*
 * Starts the driver's own stream, stream 0 of group slot 0, on an empty
 * ring, and runs its self-test.
 */
int
mali_queue_start(
	struct mali_device *device)
{
	struct mali_queue *queue;
	int error;

	/* Only a running firmware schedules groups. */
	if (!device->firmware_running)
		return ENXIO;

	/* The first slot and its first stream, in the driver's group space. */
	queue = &device->queue;
	queue->group_slot = 0U;
	queue->stream = 0U;
	queue->doorbell = QUEUE_DOORBELL;
	queue->space = &device->group;

	/* The group's space, empty, translating from now on. */
	error = mali_as_init(device, &device->group, MALI_GROUP_ADDRESS_SPACE);
	if (error == 0) {
		device->group_next = MALI_GROUP_SPACE_START;
		error = mali_as_enable(device, &device->group);
	}

	if (error != 0) {
		kern_logf("mali: queue: group space not made (%d)\n", error);
		return error;
	}

	/* The ring the stream reads its instructions from, never run as code. */
	error = mali_group_alloc(device, &queue->ring, RING_BYTES, MALI_PT_WRITE);
	if (error != 0) {
		kern_logf("mali: queue: ring not made (%d)\n", error);
		return error;
	}

	/* Programs and starts it. */
	error = mali_queue_open(device, queue);
	if (error != 0)
		return error;

	/* A NOP and a sync add through the ring. */
	error = self_test(device);
	if (error != 0)
		return error;

	/* Succeeded: the stream runs and has run work. */
	return 0;
}

/*
 * Starts a stream whose slot, stream, doorbell, space and mapped ring
 * the caller set: its ring blocks and suspend buffers are made once, the
 * stream and its group programmed and started, and the firmware's
 * acknowledgement waited for.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_sched.c
 * (cs_slot_prog_locked, csg_slot_prog_locked, the start request of
 * tick_ctx_apply, csgs_upd_ctx_apply_locked).
 */
int
mali_queue_open(
	struct mali_device *device,
	struct mali_queue *queue)
{
	struct mali_group_interface *group;
	struct mali_stream_interface *stream;
	uint32_t request;
	uint32_t acknowledge;
	uint32_t state;
	int error;

	/* A slot and a stream the firmware offers. */
	if (queue->group_slot >= device->interfaces.group_count)
		return ENODEV;
	group = &device->interfaces.groups[queue->group_slot];
	if (queue->stream >= group->stream_count)
		return ENODEV;
	stream = &group->streams[queue->stream];

	/* The firmware's blocks of the ring and the group's suspend buffers. */
	error = make_buffers(device, queue, group);
	if (error != 0) {
		kern_logf("mali: queue: group %u buffers not made (%d)\n", queue->group_slot, error);
		return error;
	}

	/* The work registers of the stream; the ring's own are the last of them. */
	queue->registers = (stream->features & MALI_CS_FEATURES_REGISTERS_MASK) + 1U;
	queue->inserted = 0U;

	/* The stream first, then its group, which names the stream in its doorbell request. */
	program_stream(queue, stream);
	program_group(device, queue, group);

	/* Asks for the group's start and its endpoint configuration, and rings its doorbell. */
	request = mali_iface_read32(group->input, MALI_CSG_INPUT_REQUEST);
	acknowledge = mali_iface_read32(group->output, MALI_CSG_OUTPUT_ACK);
	request = mali_iface_update(request, MALI_CSG_STATE_START, MALI_CSG_STATE_MASK);
	request = mali_iface_toggle(request, acknowledge, MALI_CSG_ENDPOINT_CONFIG);
	mali_iface_write32(group->input, MALI_CSG_INPUT_REQUEST, request);
	mali_csf_ring_groups(device, 1U << queue->group_slot);

	/* Waits for the firmware to take both. */
	error = mali_csf_wait_group(device, queue->group_slot, MALI_CSG_STATE_MASK | MALI_CSG_ENDPOINT_CONFIG, START_WAIT_MS);
	if (error != 0) {
		kern_logf("mali: queue: group %u did not start (request %08x, acknowledge %08x, state %08x)\n",
			  queue->group_slot,
			  (unsigned)mali_iface_read32(group->input, MALI_CSG_INPUT_REQUEST),
			  (unsigned)mali_iface_read32(group->output, MALI_CSG_OUTPUT_ACK),
			  (unsigned)mali_iface_read32(group->output, MALI_CSG_OUTPUT_STATE));
		return error;
	}

	/* Then for the stream's own start. */
	error = wait_stream_started(stream, START_WAIT_MS);
	if (error != 0) {
		kern_logf("mali: queue: group %u stream %u did not start (request %08x, acknowledge %08x, fault %08x, fatal %08x)\n",
			  queue->group_slot,
			  queue->stream,
			  (unsigned)mali_iface_read32(stream->input, MALI_CS_INPUT_REQUEST),
			  (unsigned)mali_iface_read32(stream->output, MALI_CS_OUTPUT_ACK),
			  (unsigned)mali_iface_read32(stream->output, MALI_CS_OUTPUT_FAULT),
			  (unsigned)mali_iface_read32(stream->output, MALI_CS_OUTPUT_FATAL));
		return error;
	}

	/* An empty ring: the stream's doorbell must find nothing to run. */
	mali_csf_ring_doorbell(device, queue->doorbell);
	queue->running = 1;

	/* Says the stream runs, with the group's state word. */
	state = mali_iface_read32(group->output, MALI_CSG_OUTPUT_STATE);
	kern_logf("mali: queue: group %u stream %u started on an empty ring (group state %08x, ring at %llx, doorbell %u, %u registers)\n",
		  queue->group_slot,
		  queue->stream,
		  (unsigned)state,
		  (unsigned long long)queue->ring.gpu_address,
		  queue->doorbell,
		  queue->registers);

	/* Succeeded: the stream waits for instructions. */
	return 0;
}

/*
 * Stops a stream's group with a terminate request, so its ring and its
 * address space may change; a stream that stopped on a fatal error is
 * started again by mali_queue_open() after this.
 */
int
mali_queue_stop(
	struct mali_device *device,
	struct mali_queue *queue)
{
	struct mali_group_interface *group;
	uint32_t request;
	int error;

	/* A stream that never ran has nothing to stop. */
	if (!queue->running)
		return 0;
	group = &device->interfaces.groups[queue->group_slot];

	/* Asks the firmware to terminate the group, and waits for the answer. */
	request = mali_iface_read32(group->input, MALI_CSG_INPUT_REQUEST);
	request = mali_iface_update(request, GROUP_STATE_TERMINATE, MALI_CSG_STATE_MASK);
	mali_iface_write32(group->input, MALI_CSG_INPUT_REQUEST, request);
	mali_csf_ring_groups(device, 1U << queue->group_slot);
	error = mali_csf_wait_group(device, queue->group_slot, MALI_CSG_STATE_MASK, START_WAIT_MS);
	queue->running = 0;
	if (error != 0) {
		kern_logf("mali: queue: group %u did not stop (request %08x, acknowledge %08x)\n",
			  queue->group_slot,
			  (unsigned)mali_iface_read32(group->input, MALI_CSG_INPUT_REQUEST),
			  (unsigned)mali_iface_read32(group->output, MALI_CSG_OUTPUT_ACK));
		return error;
	}

	/* Succeeded: the group no longer runs. */
	return 0;
}

/*
 * Appends instructions to a running stream's ring and rings its
 * doorbell.  EBUSY when the ring has no room for them: the front end
 * has not read far enough yet.
 */
int
mali_queue_push(
	struct mali_device *device,
	struct mali_queue *queue,
	const uint64_t *words,
	uint32_t count)
{
	volatile uint64_t *ring;
	uint64_t extract;
	uint64_t bytes;
	uint64_t slots;
	uint64_t at;
	uint32_t index;

	/* A running stream, and instructions that fit the ring at all. */
	if (!queue->running)
		return ENXIO;
	bytes = (uint64_t)count * MALI_CS_INSTRUCTION_BYTES;
	if (count == 0U || bytes > queue->ring.bytes)
		return EINVAL;

	/* Room behind what the front end has read. */
	kern_io_read_barrier();
	extract = mali_iface_read64(queue->ring_blocks.view + RING_OUTPUT_OFFSET, MALI_RING_OUTPUT_EXTRACT);
	if (queue->inserted - extract + bytes > queue->ring.bytes)
		return EBUSY;

	/* The instructions after the last ones, around the ring's end. */
	ring = (volatile uint64_t *)(volatile void *)queue->ring.view;
	slots = queue->ring.bytes / MALI_CS_INSTRUCTION_BYTES;
	at = queue->inserted / MALI_CS_INSTRUCTION_BYTES;
	for (index = 0; index < count; index++)
		ring[(at + index) % slots] = words[index];

	/* The instructions are in memory before the insert point says so, and it before the doorbell. */
	kern_io_write_barrier();
	queue->inserted += bytes;
	mali_iface_write64(queue->ring_blocks.view, MALI_RING_INPUT_INSERT, queue->inserted);
	kern_io_write_barrier();
	mali_csf_ring_doorbell(device, queue->doorbell);

	/* Succeeded: the front end reads them. */
	return 0;
}

/*
 * Reports the first of the four work registers the ring's own
 * instructions use: two for an address pair, two for a value pair.
 */
unsigned
mali_queue_scratch(
	const struct mali_queue *queue)
{
	return queue->registers - RING_SCRATCH_REGISTERS;
}

/*
 * Waits until a 64-bit sync object the stream adds to reaches a value,
 * the stream reports a fatal error, or the time is spent.  A fault is
 * acknowledged and recorded; EIO for a fatal error, which stops the
 * stream, ETIMEDOUT for the time.
 */
int
mali_queue_wait(
	struct mali_device *device,
	struct mali_queue *queue,
	const volatile uint64_t *sync,
	uint64_t target,
	unsigned milliseconds,
	struct mali_queue_fault *fault)
{
	uint64_t deadline;
	uint64_t now;
	uint64_t value;
	int error;

	/* Reads, then sleeps a tick, until the value, a fatal error or the end of the time. */
	kern_memset(fault, 0, sizeof(*fault));
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (;;) {
		/* The sync object, through the uncached view. */
		kern_io_read_barrier();
		value = *sync;
		if (value >= target)
			return 0;

		/* A fault or a fatal error the stream raised. */
		error = take_faults(device, queue, fault);
		if (error != 0)
			return error;

		/* The time is spent. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* A millisecond before the next read. */
		sched_sleep(now + kern_ms_to_ticks(MALI_QUEUE_POLL_MS));
	}

	/* The value was not reached in time. */
	return ETIMEDOUT;
}

/*
 * Writes one line of a stream's state: where the host inserted and the
 * front end extracted to, the instruction it is at, what it waits for,
 * and its fault and fatal words (the exception type in the low byte).
 */
void
mali_queue_report(
	struct mali_device *device,
	struct mali_queue *queue,
	const char *what)
{
	struct mali_group_interface *group;
	struct mali_stream_interface *stream;
	volatile uint8_t *output_block;

	/* The stream's interface and its ring's output block. */
	group = &device->interfaces.groups[queue->group_slot];
	stream = &group->streams[queue->stream];
	output_block = queue->ring_blocks.view + RING_OUTPUT_OFFSET;
	kern_io_read_barrier();

	/* One line. */
	kern_logf("mali: %s: group %u stream %u insert %llx extract %llx active %u at %llx wait %08x blocked %08x resource %08x fault %08x (%llx) fatal %08x (%llx) group state %08x\n",
		  what,
		  queue->group_slot,
		  queue->stream,
		  (unsigned long long)queue->inserted,
		  (unsigned long long)mali_iface_read64(output_block, MALI_RING_OUTPUT_EXTRACT),
		  (unsigned)mali_iface_read32(output_block, MALI_RING_OUTPUT_ACTIVE),
		  (unsigned long long)mali_iface_read64(stream->output, MALI_CS_OUTPUT_COMMAND_POINTER),
		  (unsigned)mali_iface_read32(stream->output, STREAM_OUTPUT_WAIT),
		  (unsigned)mali_iface_read32(stream->output, STREAM_OUTPUT_BLOCKED),
		  (unsigned)mali_iface_read32(stream->output, STREAM_OUTPUT_RESOURCE),
		  (unsigned)mali_iface_read32(stream->output, MALI_CS_OUTPUT_FAULT),
		  (unsigned long long)mali_iface_read64(stream->output, STREAM_OUTPUT_FAULT_INFO),
		  (unsigned)mali_iface_read32(stream->output, MALI_CS_OUTPUT_FATAL),
		  (unsigned long long)mali_iface_read64(stream->output, STREAM_OUTPUT_FATAL_INFO),
		  (unsigned)mali_iface_read32(group->output, MALI_CSG_OUTPUT_STATE));
}

/* Makes the ring's input and output blocks and the group's suspend buffers, in the microcontroller's space. */
static int
make_buffers(
	struct mali_device *device,
	struct mali_queue *queue,
	const struct mali_group_interface *group)
{
	int error;

	/* Made once: a restarted stream keeps them. */
	if (queue->ring_blocks.view != NULL)
		return 0;

	/* The ring's input and output blocks, where the firmware reads and writes them. */
	error = mali_mcu_alloc(device, &queue->ring_blocks, RING_BLOCKS_BYTES, MALI_PT_WRITE);
	if (error != 0)
		return error;

	/* The group's suspend buffer, when the firmware asks for one. */
	if (group->suspend_bytes != 0U) {
		error = mali_mcu_alloc(device, &queue->suspend, group->suspend_bytes, MALI_PT_WRITE);
		if (error != 0)
			return error;
	}

	/* And its protected-mode one, likewise. */
	if (group->protected_suspend_bytes != 0U) {
		error = mali_mcu_alloc(device, &queue->protected_suspend, group->protected_suspend_bytes, MALI_PT_WRITE);
		if (error != 0)
			return error;
	}

	/* Succeeded: every buffer is mapped. */
	return 0;
}

/*
 * Writes the stream's ring, its blocks, its doorbell and priority, and
 * asks for its start, with idle reports when it waits or runs dry.
 */
static void
program_stream(
	struct mali_queue *queue,
	struct mali_stream_interface *stream)
{
	volatile uint8_t *input_block;
	uint32_t configuration;
	uint32_t request;
	uint32_t mask;

	/* An empty ring: nothing written, nothing read. */
	input_block = queue->ring_blocks.view;
	mali_iface_write64(input_block, MALI_RING_INPUT_INSERT, 0U);
	mali_iface_write64(input_block, MALI_RING_INPUT_EXTRACT, 0U);
	mali_iface_write64(input_block + RING_OUTPUT_OFFSET, MALI_RING_OUTPUT_EXTRACT, 0U);

	/* The ring and its two blocks. */
	mali_iface_write64(stream->input, MALI_CS_INPUT_RING_BASE, queue->ring.gpu_address);
	mali_iface_write32(stream->input, MALI_CS_INPUT_RING_SIZE, (uint32_t)queue->ring.bytes);
	mali_iface_write64(stream->input, MALI_CS_INPUT_RING_INPUT, queue->ring_blocks.gpu_address);
	mali_iface_write64(stream->input, MALI_CS_INPUT_RING_OUTPUT, queue->ring_blocks.gpu_address + RING_OUTPUT_OFFSET);

	/* The priority and the doorbell the host rings for this stream. */
	configuration = QUEUE_PRIORITY;
	configuration |= queue->doorbell << MALI_CS_CONFIG_DOORBELL_SHIFT;
	mali_iface_write32(stream->input, MALI_CS_INPUT_CONFIG, configuration);

	/* Every answer raises the interrupt. */
	mali_iface_write32(stream->input, MALI_CS_INPUT_ACK_IRQ_MASK, 0xffffffffU);

	/* The start, with idle reports when it waits on a sync object or runs dry. */
	mask = MALI_CS_IDLE_SYNC_WAIT | MALI_CS_IDLE_EMPTY | MALI_CS_STATE_MASK;
	request = mali_iface_read32(stream->input, MALI_CS_INPUT_REQUEST);
	request = mali_iface_update(request, MALI_CS_IDLE_SYNC_WAIT | MALI_CS_IDLE_EMPTY | MALI_CS_STATE_START, mask);
	mali_iface_write32(stream->input, MALI_CS_INPUT_REQUEST, request);
}

/*
 * Writes the group slot's cores, endpoint, address space and suspend
 * buffers, and flips the stream's bit of its doorbell request so the
 * firmware reads the stream when the group starts.
 */
static void
program_group(
	struct mali_device *device,
	struct mali_queue *queue,
	struct mali_group_interface *group)
{
	uint64_t endpoint;
	unsigned shaders;
	unsigned tilers;
	uint32_t request;
	uint32_t acknowledge;

	/* Every core may run the group's compute, fragment and tiler work. */
	mali_iface_write64(group->input, MALI_CSG_INPUT_ALLOW_COMPUTE, device->shader_present);
	mali_iface_write64(group->input, MALI_CSG_INPUT_ALLOW_FRAGMENT, device->shader_present);
	mali_iface_write32(group->input, MALI_CSG_INPUT_ALLOW_OTHER, (uint32_t)device->tiler_present);

	/* The most cores of each kind at once, at the highest priority. */
	shaders = (unsigned)__builtin_popcountll(device->shader_present);
	tilers = (unsigned)__builtin_popcountll(device->tiler_present);
	endpoint = shaders & MALI_CSG_ENDPOINT_CORES_MASK;
	endpoint |= (uint64_t)(shaders & MALI_CSG_ENDPOINT_CORES_MASK) << MALI_CSG_ENDPOINT_FRAGMENT_SHIFT;
	endpoint |= (uint64_t)(tilers & MALI_CSG_ENDPOINT_TILERS_MASK) << MALI_CSG_ENDPOINT_TILER_SHIFT;
	endpoint |= (uint64_t)MALI_CSG_PRIORITY_HIGHEST << MALI_CSG_ENDPOINT_PRIORITY_SHIFT;

	/* A firmware of interface 4.0 on takes the endpoint as a 64-bit word at its own place. */
	if (device->interfaces.version >= MALI_IFACE_ENDPOINT64_VERSION) {
		mali_iface_write64(group->input, MALI_CSG_INPUT_ENDPOINT64, endpoint);
	} else {
		mali_iface_write32(group->input, MALI_CSG_INPUT_ENDPOINT, (uint32_t)endpoint);
	}

	/* The address space the group's streams translate through. */
	mali_iface_write32(group->input, MALI_CSG_INPUT_CONFIG, queue->space->slot);

	/* The suspend buffers, zero when the firmware asked for none. */
	mali_iface_write64(group->input, MALI_CSG_INPUT_SUSPEND, queue->suspend.gpu_address);
	mali_iface_write64(group->input, MALI_CSG_INPUT_PROTM_SUSPEND, queue->protected_suspend.gpu_address);

	/* Every answer raises the interrupt. */
	mali_iface_write32(group->input, MALI_CSG_INPUT_ACK_IRQ_MASK, 0xffffffffU);

	/* The stream's doorbell bit, flipped away from the last answer. */
	request = mali_iface_read32(group->input, MALI_CSG_INPUT_DOORBELL);
	acknowledge = mali_iface_read32(group->output, MALI_CSG_OUTPUT_DOORBELL_ACK);
	mali_iface_write32(group->input, MALI_CSG_INPUT_DOORBELL, mali_iface_toggle(request, acknowledge, 1U << queue->stream));
}

/* Polls the stream's acknowledge word until its state says started; ETIMEDOUT when it does not. */
static int
wait_stream_started(
	struct mali_stream_interface *stream,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t acknowledge;

	/* Reads, then sleeps a tick, until started or the time is spent. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (;;) {
		/* The state field of the acknowledge word mirrors the request once taken. */
		kern_io_read_barrier();
		acknowledge = mali_iface_read32(stream->output, MALI_CS_OUTPUT_ACK);
		if ((acknowledge & MALI_CS_STATE_MASK) == MALI_CS_STATE_START)
			return 0;

		/* The time is spent. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* A millisecond before the next read. */
		sched_sleep(now + kern_ms_to_ticks(MALI_QUEUE_POLL_MS));
	}

	/* Succeeded in waiting, but the stream did not start. */
	return ETIMEDOUT;
}

/*
 * Takes the stream's raised fault and fatal error: each is a bit of the
 * acknowledge word that differs from the request word, answered by
 * setting the request's bit to match.  EIO for a fatal error.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_sched.c
 * (cs_slot_process_fatal_event_locked, cs_slot_process_fault_event_locked:
 * the event is the request and acknowledge bits differing).
 */
static int
take_faults(
	struct mali_device *device,
	struct mali_queue *queue,
	struct mali_queue_fault *fault)
{
	struct mali_stream_interface *stream;
	uint32_t request;
	uint32_t acknowledge;
	uint32_t raised;

	/* The bits of the stream's request and acknowledge words that differ. */
	stream = &device->interfaces.groups[queue->group_slot].streams[queue->stream];
	request = mali_iface_read32(stream->input, MALI_CS_INPUT_REQUEST);
	acknowledge = mali_iface_read32(stream->output, MALI_CS_OUTPUT_ACK);
	raised = (request ^ acknowledge) & (STREAM_FATAL | STREAM_FAULT);
	if (raised == 0U)
		return 0;

	/* The fault's type and data, answered so the stream goes on past it. */
	if ((raised & STREAM_FAULT) != 0U) {
		fault->fault = mali_iface_read32(stream->output, MALI_CS_OUTPUT_FAULT);
		fault->fault_info = mali_iface_read64(stream->output, STREAM_OUTPUT_FAULT_INFO);
		fault->faults++;
	}

	/* The fatal error's type and data; the stream has stopped. */
	if ((raised & STREAM_FATAL) != 0U) {
		fault->fatal = mali_iface_read32(stream->output, MALI_CS_OUTPUT_FATAL);
		fault->fatal_info = mali_iface_read64(stream->output, STREAM_OUTPUT_FATAL_INFO);
	}

	/* Answers both by matching the request to the acknowledgement. */
	request = mali_iface_update(request, acknowledge, raised);
	mali_iface_write32(stream->input, MALI_CS_INPUT_REQUEST, request);
	mali_csf_ring_groups(device, 1U << queue->group_slot);

	/* A fatal error ends the wait; a fault is only recorded. */
	if ((raised & STREAM_FATAL) != 0U)
		return EIO;

	/* Succeeded: the fault is recorded and the stream goes on. */
	return 0;
}

/*
 * Runs a NOP and a 64-bit sync add of one through the driver's ring and
 * waits for the sync object to reach one; one line says what happened.
 */
static int
self_test(
	struct mali_device *device)
{
	struct mali_queue_fault fault;
	struct mali_buffer *sync;
	struct mali_queue *queue;
	struct mali_cs cs;
	uint64_t words[8];
	volatile uint64_t *value;
	unsigned address;
	unsigned data;
	int error;

	/* A page of the group's space for the sync object, zeroed. */
	queue = &device->queue;
	sync = &device->queue_sync;
	error = mali_group_alloc(device, sync, MALI_PAGE_BYTES, MALI_PT_WRITE);
	if (error != 0) {
		kern_logf("mali: queue self-test: no sync page (%d)\n", error);
		return error;
	}

	value = (volatile uint64_t *)(volatile void *)sync->view;

	/* NOP, the address and the value one, the add, an error barrier. */
	address = mali_queue_scratch(queue);
	data = address + 2U;
	mali_cs_init(&cs, words, 8);
	mali_cs_emit(&cs, mali_cs_nop());
	mali_cs_emit(&cs, mali_cs_move48(address, sync->gpu_address));
	mali_cs_emit(&cs, mali_cs_move48(data, 1U));
	mali_cs_emit(&cs, mali_cs_sync_add64(true, MALI_CS_SCOPE_SYSTEM, 0, data, address, 0));
	mali_cs_emit(&cs, mali_cs_error_barrier());

	/* Into the ring. */
	error = mali_queue_push(device, queue, words, cs.count);
	if (error != 0) {
		kern_logf("mali: queue self-test: not pushed (%d)\n", error);
		return error;
	}

	/* The front end adds one to the sync object. */
	error = mali_queue_wait(device, queue, value, 1U, SELF_TEST_WAIT_MS, &fault);
	if (error != 0) {
		kern_logf("mali: queue self-test failed (%d): sync %llx, fault %08x (%llx), fatal %08x (%llx)\n",
			  error,
			  (unsigned long long)*value,
			  (unsigned)fault.fault,
			  (unsigned long long)fault.fault_info,
			  (unsigned)fault.fatal,
			  (unsigned long long)fault.fatal_info);
		mali_queue_report(device, queue, "queue self-test");
		return error;
	}

	/* Says the ring ran. */
	kern_logf("mali: queue self-test: NOP and SYNC_ADD64 ran (sync 0 -> %llx, %u bytes through the ring)\n",
		  (unsigned long long)*value,
		  (unsigned)mali_cs_bytes(&cs));

	/* Succeeded: the front end runs the ring's instructions. */
	return 0;
}
