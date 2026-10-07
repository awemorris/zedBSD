/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host side of the Vulkan executor fixtures (res, resdispatch, pipe,
 * cmdbuf, sync).
 *
 * run-vk-host-tests.sh links each fixture with the executor's own objects:
 * every file of src/drivers/gpu/i915/render/ that decodes, records or
 * emits, and the shader compiler, each built as its own translation unit
 * exactly as the kernel builds it.  This file supplies what those objects
 * take from the rest of the kernel, and the fixture session they run in:
 *
 *  - the kernel allocator, which keeps every live block on a list so a
 *    fixture can count what production code still holds;
 *  - the kernel log, which keeps the last line;
 *  - the direct map, in which a session blob's physical address is its
 *    host pointer;
 *  - stand-ins for the parts that run work on the GPU (draw.c and blit.c),
 *    which record what they were asked to run;
 *  - a little-endian wire builder that writes commands the way libvulkan
 *    does, and one node session with a reply blob in slot 3.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>

#include "../../../src/drivers/gpu/i915/i915.h"
#include "../../../src/drivers/gpu/i915/memory.h"
#include "../../../src/drivers/gpu/i915/session.h"
#include "../../../src/drivers/gpu/i915/render/codec.h"
#include "../../../src/drivers/gpu/i915/render/draw.h"
#include "../../../src/drivers/gpu/i915/render/gfx.h"
#include "../../../src/drivers/gpu/i915/render/internal.h"
#include "../../../src/drivers/gpu/i915/render/object.h"
#include "../../../src/drivers/gpu/i915/render/render.h"
#include "../../../src/drivers/gpu/i915/render/transport.h"

/* The largest stream one fixture command is built into. */
#define STUB_WIRE_BYTES		65536U

/* The size of the reply blob, and the slot the fixture session gives it. */
#define STUB_REPLY_BYTES	8192U
#define STUB_REPLY_SLOT		3U

/* The opcode that selects the reply region, and the bytes it occupies. */
#define STUB_SET_REPLY_STREAM	178U
#define STUB_SELECTOR_BYTES	36U

/* How many of the rectangles asked for are kept in order. */
#define STUB_RECT_LOG		8U

/*
 * The header in front of every block the fixture's kernel allocator hands
 * out.
 *
 * Every live block is on one list from its allocation to its free, so a
 * block production code lost is still reachable and does not trip the leak
 * checker; the fixtures count live blocks instead and say which step left
 * one behind.  The union keeps the block behind the header aligned for any
 * type.
 */
union stub_block {
	struct {
		/* The neighbours on the list of live blocks. */
		union stub_block *next;
		union stub_block *previous;

		/* The allocation's number: blocks allocated later have larger ones. */
		unsigned long serial;
	} header;
	max_align_t alignment;
};

/*
 * One recorded request to run a draw on the GPU.
 *
 * The stand-in of drv_i915_gfx_draw keeps a copy of what it was asked to
 * draw and answers with stub_draw_result.
 */
struct stub_draw_call {
	/* What was bound when the draw was reached. */
	struct i915_gfx_draw_state state;

	/* The draw's own parameters: counts, firsts and whether it is indexed. */
	struct i915_gfx_draw_args args;
};

/*
 * One recorded request to run a rectangle on the GPU.
 *
 * The stand-in of drv_i915_gfx_rect keeps the target and whether it was a
 * copy or a fill.
 */
struct stub_rect_call {
	/* The surface and the rectangle written. */
	struct i915_gfx_surface dst;
	struct i915_gfx_rect dst_rect;

	/* Nonzero for a copy (a source was given), zero for a fill. */
	int copy;

	/* The surface and the rectangle a copy reads; zero for a fill. */
	struct i915_gfx_surface src;
	struct i915_gfx_rect src_rect;

	/* The four words a fill writes. */
	uint32_t clear[4];
};

/*
 * A command stream under construction.
 *
 * A fixture fills it with little-endian words exactly as libvulkan's wire
 * writer lays them down.
 */
struct stub_wire {
	/* The stream and how many bytes of it are written. */
	uint8_t bytes[STUB_WIRE_BYTES];
	size_t size;
};

/*
 * The live blocks, newest first, and the serial the next block gets.
 *
 * Only the fixture's single thread allocates, so no lock protects them.
 */
static union stub_block *stub_blocks;
static unsigned long stub_next_serial = 1UL;

/* How many blocks are live now. */
static unsigned stub_live;

/*
 * The lines production code logged since the current stream began (a
 * refused command logs the refusal after its reason, ws075-p024), and how
 * many it logged in all.
 */
static char stub_log[4096];
static unsigned stub_log_lines;

/* How many draws and rectangles were asked for, and the most recent of each. */
static unsigned stub_draw_calls;
static struct stub_draw_call stub_last_draw;
static unsigned stub_rect_calls;
static struct stub_rect_call stub_last_rect;

/*
 * The first rectangles asked for since the fixture last cleared the count,
 * in order: stub_rect_calls says how many were asked for, and the first
 * STUB_RECT_LOG of them are kept.
 */
static struct stub_rect_call stub_rects[STUB_RECT_LOG];

/* What the next draw and rectangle report: 0, or the errno the GPU run failed with. */
static int stub_draw_result;
static int stub_rect_result;

/* How many times the session's draw state was released. */
static unsigned stub_session_closes;

/*
 * The fixture's hardware device, node session, executor and executor
 * session, opened by stub_session_open and closed by stub_session_close.
 */
static struct i915_device stub_device;
static struct i915_session stub_gpu;
static struct i915_render_device *stub_vk;
static struct i915_render_session *stub_session;

/*
 * The reply blob in slot 3 and the node object that stands for it.
 *
 * Every fixture stream selects it first, so a reply starts at stub_reply[0].
 */
static uint8_t stub_reply[STUB_REPLY_BYTES];
static struct i915_gem_object stub_reply_object;

static void *stub_allocate(size_t size, int zero);

/*
 * Allocates a kernel block of `size` bytes.
 */
void *
kern_malloc(
	size_t size)
{
	void *pointer;

	/* Allocates an uninitialized block. */
	pointer = stub_allocate(size, 0);

	/* Reports the block, or NULL when the host is out of memory. */
	return pointer;
}

/*
 * Allocates a zeroed kernel block of `count` elements of `size` bytes.
 */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	void *pointer;

	/* Refuses a request whose size overflows. */
	if (size != 0U && count > SIZE_MAX / size)
		return NULL;

	/* Allocates a zeroed block. */
	pointer = stub_allocate(count * size, 1);

	/* Reports the block, or NULL when the host is out of memory. */
	return pointer;
}

/*
 * Frees a kernel block; NULL is nothing to free.
 */
void
kern_free(
	void *pointer)
{
	union stub_block *block;

	/* Freeing nothing is allowed. */
	if (pointer == NULL)
		return;

	/* Finds the header in front of the block. */
	block = (union stub_block *)pointer - 1;

	/* Takes the block off the list of live blocks. */
	if (block->header.previous != NULL) {
		block->header.previous->header.next = block->header.next;
	} else {
		stub_blocks = block->header.next;
	}

	if (block->header.next != NULL)
		block->header.next->header.previous = block->header.previous;

	/* One block fewer is live. */
	stub_live--;
	free(block);
}

/*
 * The mutexes (the device's, which vkAllocateMemory takes to find an
 * imported resource, and the executor's object table and allocation list,
 * ws075-p025): the fixture runs one thread, so taking one waits for nothing,
 * but the held ones are remembered, so a mutex taken again by its holder (a
 * deadlock in the kernel) or given back when not held fails the fixture.
 */
static struct mutex *stub_mutexes_held[8];

int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Nothing to set up. */
	(void)mutex;
	(void)rank;
	(void)name;
	return 0;
}

void
mutex_lock(
	struct mutex *mutex)
{
	unsigned index;

	/* Not held already, and one more held. */
	for (index = 0U; index < 8U; index++)
		assert(stub_mutexes_held[index] != mutex);
	for (index = 0U; index < 8U && stub_mutexes_held[index] != NULL; index++)
		;
	assert(index < 8U);
	stub_mutexes_held[index] = mutex;
}

/*
 * Gives a mutex back.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	unsigned index;

	/* It was held. */
	for (index = 0U; index < 8U && stub_mutexes_held[index] != mutex; index++)
		;
	assert(index < 8U);
	stub_mutexes_held[index] = NULL;
}

/*
 * Keeps the last kernel log line for the fixture to inspect.
 */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;
	char line[512];
	size_t used;
	size_t length;

	/* Formats the line. */
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* Appends it; one that does not fit starts the text again. */
	used = strlen(stub_log);
	length = strlen(line);
	if (used + length >= sizeof(stub_log))
		used = 0U;
	memcpy(stub_log + used, line, length + 1U);

	/* Counts the line. */
	stub_log_lines++;
}

/*
 * Orders writes; the fixture has nothing to order.
 */
void
kern_io_write_barrier(void)
{
}

/*
 * Finds the kernel view of a physical address: in the fixture a session
 * blob's physical address is its host pointer.
 */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	/* Reports the host pointer the address was made from. */
	return (void *)(uintptr_t)address;
}

/*
 * Stands in for running one draw on the GPU: records it and reports
 * stub_draw_result.
 */
int
drv_i915_gfx_draw(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_draw_args *args)
{
	UNUSED_PARAMETER(session);

	/* Records what was bound and what was drawn. */
	stub_draw_calls++;
	stub_last_draw.state = *state;
	stub_last_draw.args = *args;

	/* Reports a GPU failure the fixture asked for. */
	if (stub_draw_result != 0)
		return stub_draw_result;

	/* Succeeded: the draw counts as run. */
	return 0;
}

/*
 * Stands in for running one rectangle on the GPU: records it and reports
 * stub_rect_result.
 */
int
drv_i915_gfx_rect(
	struct i915_render_session *session,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *src,
	const struct i915_gfx_rect *src_rect,
	const uint32_t clear[4],
	int linear)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(linear);

	/* Records the target and whether the rectangle copies or fills. */
	stub_rect_calls++;
	memset(&stub_last_rect, 0, sizeof(stub_last_rect));
	stub_last_rect.dst = *dst;
	stub_last_rect.dst_rect = *dst_rect;
	stub_last_rect.copy = 0;
	if (src != NULL) {
		stub_last_rect.copy = 1;
		stub_last_rect.src = *src;
		stub_last_rect.src_rect = *src_rect;
	}

	/* Keeps the words of a fill. */
	if (clear != NULL)
		memcpy(stub_last_rect.clear, clear, sizeof(stub_last_rect.clear));

	/* Keeps the first rectangles in order. */
	if (stub_rect_calls <= STUB_RECT_LOG)
		stub_rects[stub_rect_calls - 1U] = stub_last_rect;

	/* Reports a GPU failure the fixture asked for. */
	if (stub_rect_result != 0)
		return stub_rect_result;

	/* Succeeded: the rectangle counts as run. */
	return 0;
}

/*
 * Stands in for resolving a multisampled rectangle on the GPU: records it
 * as a copy from sample 0 and reports stub_rect_result.
 */
int
drv_i915_gfx_resolve(
	struct i915_render_session *session,
	const struct i915_gfx_surface *dst,
	const struct i915_gfx_rect *dst_rect,
	const struct i915_gfx_surface *samples,
	const struct i915_gfx_rect *src_rect)
{
	int error;

	/* Records it as the rectangle stand-in records a copy. */
	error = drv_i915_gfx_rect(session, dst, dst_rect, &samples[0], src_rect, NULL, 0);
	if (error != 0)
		return error;

	/* Succeeded: the resolve counts as run. */
	return 0;
}

/*
 * Stands in for opening a submission's batch: the stand-in draw and
 * rectangle count as run when they are called, so there is no batch.
 */
int
drv_i915_gfx_submit_begin(
	struct i915_render_session *session)
{
	UNUSED_PARAMETER(session);

	/* Succeeded: nothing to open. */
	return 0;
}

/*
 * Stands in for running a submission's batch: nothing waits in one.
 */
int
drv_i915_gfx_submit_end(
	struct i915_render_session *session)
{
	UNUSED_PARAMETER(session);

	/* Succeeded: nothing to run. */
	return 0;
}

/*
 * The video decode engine (ws083-p003b): none unless a test sets the state,
 * and an attach that succeeds and is counted.
 */
int stub_video_state = ENODEV;
unsigned stub_video_attaches;

int
drv_i915_worker_video_state(struct i915_device *device)
{
	(void)device;
	return stub_video_state;
}

int
drv_i915_worker_context_attach(struct i915_device *device, struct i915_context *context)
{
	(void)device;
	(void)context;
	stub_video_attaches++;
	return 0;
}

/*
 * The release of a session object a quarantined video session's batch goes
 * through (ws083-p007): the host keeps no registry, so a retained object is
 * counted and, like any other, freed at once.
 */
unsigned stub_gem_retained;

void
drv_i915_gem_destroy(struct i915_gem_registry *registry, struct i915_gem_object *object)
{
	(void)registry;
	if (object->quarantined != 0U)
		stub_gem_retained++;
	kern_free(object->address);
	kern_free(object);
}

/* The IRQ lock a video hang quarantines a session under: one host thread needs no lock. */
unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	return 0UL;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long flags)
{
	(void)lock;
	(void)flags;
}

/*
 * GPU objects and the batches run on an engine (ws083-p004).  Off by
 * default: no object is made (below) and a run runs nothing.  A test that
 * sets stub_objects_on gets objects of host memory at made-up addresses,
 * and each run's dwords appended to stub_batch_words (at most
 * STUB_BATCH_WORDS of them; the test empties it by zeroing
 * stub_batch_count) with the engine the last one ran on.
 */
#define STUB_BATCH_WORDS	8192U
int stub_objects_on;
uint32_t stub_batch_words[STUB_BATCH_WORDS];
unsigned stub_batch_count;
unsigned stub_batch_runs;
unsigned stub_batch_engine;
static uint64_t stub_object_next_va = 0x7000000000ULL;

/* Nonzero makes the next run end with that error, as a hang would (ws083-p007); it applies once. */
int stub_batch_run_error;

/* A batch run on an engine (draw.c): the host runs nothing, and keeps the dwords when objects are on. */
int
drv_i915_gfx_batch_run(struct i915_render_session *session, struct i915_gfx_batch *batch, uint64_t batch_va, unsigned engine)
{
	unsigned index;
	int error;

	(void)session;
	(void)batch_va;
	if (stub_batch_run_error != 0) {
		error = stub_batch_run_error;
		stub_batch_run_error = 0;
		return error;
	}
	if (!stub_objects_on)
		return 0;
	for (index = 0U; index < batch->count && stub_batch_count < STUB_BATCH_WORDS; index++) {
		stub_batch_words[stub_batch_count] = batch->cmds[index];
		stub_batch_count++;
	}
	stub_batch_engine = engine;
	stub_batch_runs++;
	return 0;
}

/*
 * Stand in for the query pools' GPU objects and batch (ws075-p006): the
 * host has no GPU, so creating a pool's counters fails and a recorded query
 * command cannot take a batch.
 */
int
drv_i915_gfx_object_create(
	struct i915_render_session *session,
	uint64_t bytes,
	struct i915_gem_object **result)
{
	struct i915_gem_object *object;

	UNUSED_PARAMETER(session);

	/* No object on the host unless a test turned objects on. */
	*result = NULL;
	if (!stub_objects_on)
		return ENOMEM;

	/* An object of host memory at the next made-up address. */
	object = kern_calloc(1U, sizeof(*object));
	if (object == NULL)
		return ENOMEM;
	object->address = kern_calloc(1U, (size_t)bytes);
	if (object->address == NULL) {
		kern_free(object);
		return ENOMEM;
	}
	object->bytes = bytes;
	object->va = stub_object_next_va;
	stub_object_next_va += (bytes + 0xffffULL) & ~0xffffULL;
	*result = object;
	return 0;
}

/* Stands in for destroying a GPU object: frees one the stand-in made. */
void
drv_i915_gfx_object_destroy(
	struct i915_render_session *session,
	struct i915_gem_object *object)
{
	UNUSED_PARAMETER(session);

	/* Only an object made above. */
	if (object == NULL)
		return;
	kern_free(object->address);
	kern_free(object);
}

/* Stands in for the session's draw objects: none on the host. */
struct i915_gfx_session *
drv_i915_gfx_session_get(
	struct i915_render_session *session)
{
	UNUSED_PARAMETER(session);

	/* No objects. */
	return NULL;
}

/* Stands in for taking the batch: never reached without the session's objects. */
int
drv_i915_gfx_op_begin(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	struct i915_gfx_op_space *space)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(work);
	UNUSED_PARAMETER(space);

	/* No batch. */
	return ENOMEM;
}

/* Stands in for ending an operation: never reached. */
int
drv_i915_gfx_op_end(
	struct i915_render_session *session,
	struct i915_gfx_session *work,
	int error)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(work);

	/* Reports the operation's own outcome. */
	return error;
}

/* Stands in for flushing CPU cache lines: host memory needs none. */
void
drv_i915_gt_clflush(
	const volatile void *address,
	size_t bytes)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(bytes);
}

/*
 * Stands in for releasing the session's draw state: the stand-in draw
 * keeps none, so it only counts the call.
 */
void
drv_i915_gfx_session_close(
	struct i915_render_session *session)
{
	UNUSED_PARAMETER(session);

	/* Counts the release. */
	stub_session_closes++;
}

/*
 * Reports the serial the next allocated block will carry.
 *
 * A fixture takes it before a step and asks stub_live_since afterwards what
 * the step left allocated.
 */
unsigned long
stub_allocation_mark(void)
{
	/* Reports the next serial. */
	return stub_next_serial;
}

/*
 * Counts the live blocks allocated at or after `mark`.
 */
unsigned
stub_live_since(
	unsigned long mark)
{
	union stub_block *block;
	unsigned count;

	/* Counts every live block with a serial from the mark on. */
	count = 0U;
	for (block = stub_blocks;
	     block != NULL;
	     block = block->header.next) {
		if (block->header.serial >= mark)
			count++;
	}

	/* Reports the count. */
	return count;
}

/*
 * Frees every live block allocated at or after `mark` and reports how many.
 *
 * A fixture uses it only after it has asserted that production code lost
 * those blocks, so that the rest of its run starts clean.
 */
unsigned
stub_release_since(
	unsigned long mark)
{
	union stub_block *block;
	union stub_block *next;
	unsigned count;

	/* Frees every live block with a serial from the mark on. */
	count = 0U;
	for (block = stub_blocks;
	     block != NULL;
	     block = next) {
		next = block->header.next;
		if (block->header.serial >= mark) {
			kern_free(block + 1);
			count++;
		}
	}

	/* Reports how many blocks were reclaimed. */
	return count;
}

/*
 * Empties a wire.
 */
void
stub_wire_reset(
	struct stub_wire *wire)
{
	/* Nothing is written yet. */
	wire->size = 0U;
}

/*
 * Appends one little-endian 32-bit word to a wire.
 */
void
stub_put32(
	struct stub_wire *wire,
	uint32_t value)
{
	/* A fixture never builds a stream past the wire. */
	assert(wire->size + 4U <= sizeof(wire->bytes));

	/* Writes the word low byte first. */
	wire->bytes[wire->size] = (uint8_t)value;
	wire->bytes[wire->size + 1U] = (uint8_t)(value >> 8);
	wire->bytes[wire->size + 2U] = (uint8_t)(value >> 16);
	wire->bytes[wire->size + 3U] = (uint8_t)(value >> 24);
	wire->size += 4U;
}

/*
 * Appends one little-endian 64-bit word to a wire.
 */
void
stub_put64(
	struct stub_wire *wire,
	uint64_t value)
{
	/* Writes the low half, then the high half. */
	stub_put32(wire, (uint32_t)value);
	stub_put32(wire, (uint32_t)(value >> 32));
}

/*
 * Reads one little-endian 32-bit word of a reply.
 */
uint32_t
stub_get32(
	const uint8_t *bytes,
	size_t offset)
{
	uint32_t value;

	/* Assembles the word low byte first. */
	value = (uint32_t)bytes[offset];
	value |= (uint32_t)bytes[offset + 1U] << 8;
	value |= (uint32_t)bytes[offset + 2U] << 16;
	value |= (uint32_t)bytes[offset + 3U] << 24;

	/* Reports the word. */
	return value;
}

/*
 * Reads one little-endian 64-bit word of a reply.
 */
uint64_t
stub_get64(
	const uint8_t *bytes,
	size_t offset)
{
	uint64_t low;
	uint64_t high;

	/* Reads the low half, then the high half. */
	low = stub_get32(bytes, offset);
	high = stub_get32(bytes, offset + 4U);

	/* Reports the word. */
	return low | (high << 32);
}

/*
 * Starts a stream with the selector that points replies at the start of
 * the reply blob, as libvulkan opens every submission.
 */
void
stub_wire_begin(
	struct stub_wire *wire)
{
	/* Empties the wire. */
	stub_wire_reset(wire);

	/* [178][no reply][present][slot 3][offset 0][the whole blob]. */
	stub_put32(wire, STUB_SET_REPLY_STREAM);
	stub_put32(wire, 0U);
	stub_put64(wire, 1U);
	stub_put32(wire, STUB_REPLY_SLOT);
	stub_put64(wire, 0U);
	stub_put64(wire, STUB_REPLY_BYTES);
}

/*
 * Opens the fixture session: a device, a node session with the reply blob
 * in slot 3, an executor and an executor session.
 *
 * `blobs` lists further session objects to place behind the reply blob;
 * the list may be NULL.
 */
void
stub_session_open(
	struct i915_gem_object *blobs)
{
	int error;

	/* The device an Alder Lake-P executor serves. */
	memset(&stub_device, 0, sizeof(stub_device));
	stub_device.product = 0x46a6U;

	/* The reply blob, whose physical address is its host pointer. */
	memset(&stub_reply_object, 0, sizeof(stub_reply_object));
	stub_reply_object.slot = STUB_REPLY_SLOT;
	stub_reply_object.bytes = sizeof(stub_reply);
	stub_reply_object.run.paddr = (hal_physaddr_t)(uintptr_t)stub_reply;
	stub_reply_object.session_next = blobs;

	/* The node session owns the reply blob and the fixture's other objects. */
	memset(&stub_gpu, 0, sizeof(stub_gpu));
	stub_gpu.device = &stub_device;
	stub_gpu.objects = &stub_reply_object;

	/* Attaches the executor to the device. */
	error = drv_i915_render_attach(&stub_device, &stub_vk);
	assert(error == 0);

	/* Opens the executor's side of the node session. */
	error = drv_i915_render_open(stub_vk, &stub_gpu, &stub_session);
	assert(error == 0);
}

/*
 * Closes the fixture session and detaches the executor.
 */
void
stub_session_close(void)
{
	/* Closes the executor session, then detaches the executor. */
	drv_i915_render_close(stub_session);
	drv_i915_render_detach(stub_vk);
	stub_session = NULL;
	stub_vk = NULL;
}

/*
 * Runs one stream the way the node's command operation does and reports
 * the executor's errno.
 *
 * The reply lands at the start of stub_reply; *reply_bytes receives its
 * length, which the executor publishes only on success.
 */
int
stub_execute(
	const struct stub_wire *wire,
	size_t *reply_bytes)
{
	size_t capacity;
	void *region;
	int error;

	/* The log holds this stream's lines. */
	stub_log[0] = '\0';

	/* Resolves the reply region the stream's selector names. */
	region = drv_i915_render_transport_reply(&stub_gpu, wire->bytes, (uint32_t)wire->size, &capacity);
	assert(region == stub_reply);

	/* Executes the stream into the region. */
	error = drv_i915_render_execute(stub_session, wire->bytes, wire->size, region, &capacity);
	*reply_bytes = capacity;

	/* Reports why the executor refused the stream. */
	if (error != 0)
		return error;

	/* Succeeded: the reply holds capacity bytes. */
	return 0;
}

/*
 * Runs one stream that the executor must accept, and reports the length of
 * its reply.
 */
size_t
stub_execute_ok(
	const struct stub_wire *wire)
{
	size_t reply_bytes;
	int error;

	/* Executes the stream; a fixture stream that fails is a fixture failure. */
	error = stub_execute(wire, &reply_bytes);
	assert(error == 0);

	/* Succeeded: reports the reply length. */
	return reply_bytes;
}

/* Allocates one block behind its header and puts it on the list of live blocks. */
static void *
stub_allocate(
	size_t size,
	int zero)
{
	union stub_block *block;

	/* Refuses a request whose header does not fit a size_t. */
	if (size > SIZE_MAX - sizeof(*block))
		return NULL;

	/* Allocates the header and the block together. */
	block = malloc(sizeof(*block) + size);
	if (block == NULL)
		return NULL;

	/* A zeroed block starts with every byte zero. */
	if (zero != 0)
		memset(block + 1, 0, size);

	/* Puts the block at the head of the live list with the next serial. */
	block->header.serial = stub_next_serial;
	stub_next_serial++;
	block->header.previous = NULL;
	block->header.next = stub_blocks;
	if (stub_blocks != NULL)
		stub_blocks->header.previous = block;
	stub_blocks = block;

	/* One block more is live. */
	stub_live++;

	/* Succeeded: the caller owns the block behind the header. */
	return block + 1;
}

/*
 * ws101-p004: the dispatch of a compute pipeline (render/compute.c), which
 * command.c calls, stands in here as the draw does: it records the call and
 * the groups and reports success.
 */
#include "../../../src/drivers/gpu/i915/render/compute.h"

/* How many dispatches ran, and the groups of the last one (its indirect counts' address, ws101-p007: 0 for none). */
static unsigned stub_dispatch_calls;
static uint32_t stub_last_dispatch[3];
static uint64_t stub_last_indirect_va;

/*
 * Stands in for recording one dispatch: records it and reports success.
 */
int
drv_i915_gfx_dispatch(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_grid *grid)
{
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(state);

	/* Records the grid of the dispatch. */
	stub_dispatch_calls++;
	memcpy(stub_last_dispatch, grid->groups, sizeof(stub_last_dispatch));
	stub_last_indirect_va = grid->indirect_va;

	/* Succeeded: the dispatch counts as recorded. */
	return 0;
}
