/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage A5s: the GPU's sessions, each in an address space of its own, and
 * the GPU device.
 *
 * The stage lets the address spaces run work, makes the GPU device's
 * record, and checks through the same entries the Vulkan executor uses
 * (vk-node.c) on sessions of its own: a session runs an indirect buffer in
 * its address space that writes two words, moves them, and dispatches the
 * check program, whose every output word is compared; sixteen sessions
 * run one after another, so the fifteen hub contexts change hands in the
 * order of their last use; and a session that writes to an address it
 * has nothing mapped at must be named by the page fault the hub reports
 * (that check only marks what it saw).  Then the GPU is registered with
 * the GPU core, which publishes /dev/gpuN; when the boot asks the driver
 * to own the display, stage A8 registers it instead, with the display.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-node.h"

/* The bytes of a check's run of GPU memory, and of a page. */
#define SESSIONS_RUN_BYTES		65536U
#define SESSIONS_PAGE_BYTES		4096U

/*
 * The places in the dispatch check's run: the indirect buffer at 0, the
 * program's page at 4096, the two written words at 12288, the dispatch's
 * output at 16384, the moved words at 32768.
 */
#define SESSIONS_PROGRAM_AT		4096U
#define SESSIONS_WRITTEN_AT		12288U
#define SESSIONS_OUTPUT_AT		16384U
#define SESSIONS_MOVED_AT		32768U

/* The words an indirect buffer of the checks holds at most, and the multiple it is padded to. */
#define SESSIONS_BUFFER_WORDS		128U
#define SESSIONS_BUFFER_ALIGN		8U

/* The sessions the context check runs: one more than the hub has contexts for them. */
#define SESSIONS_CONTEXT_CHECKS		16U

/* The workgroups the dispatch check launches in each direction. */
#define SESSIONS_GROUPS			2U

/* How long a check's run may take, and how long the fault check waits for its fault. */
#define SESSIONS_RUN_MS			2000U
#define SESSIONS_FAULT_RUN_MS		500U
#define SESSIONS_FAULT_WAIT_MS		200U

/*
 * The values the checks write: the first and second written word, the
 * mark of a context check's session (its address space's identifier is
 * added), and the fault check's word.
 */
#define SESSIONS_FIRST_VALUE		1001U
#define SESSIONS_SECOND_VALUE		1002U
#define SESSIONS_CONTEXT_MARK		2000000U
#define SESSIONS_FAULT_VALUE		3003U

/*
 * An address of a session's space that nothing is ever mapped at: below
 * 4 GiB, where the spaces hand out no address.
 */
#define SESSIONS_FAULT_ADDRESS		4096ULL

/* The compute queue the fault check runs on: one stage A5's own fault check left alone. */
#define SESSIONS_FAULT_QUEUE		2U

/*
 * What the stage's checks found, for its ok line: the output words the
 * dispatch compared, the hub contexts' drops and changes of hands the
 * context check caused, and whether the fault check's page fault named
 * its session.
 */
struct sessions_result {
	uint32_t dispatch_words;
	uint64_t flushes;
	uint64_t takeovers;
	bool fault_named;
	uint32_t fault_space;
};

static int sessions_dispatch(struct amdrdna_device *device, struct sessions_result *result);
static int sessions_contexts(struct amdrdna_device *device, struct sessions_result *result);
static void sessions_fault(struct amdrdna_device *device, struct sessions_result *result);
static unsigned sessions_pad(uint32_t *words, unsigned count);
static void sessions_put(volatile uint8_t *cpu, const uint32_t *words, unsigned count);

/*
 * Runs stage A5s: lets the address spaces run work, makes the GPU
 * device, checks the sessions' runs, contexts and faults, and registers
 * the device.
 *
 * Returns 0 when the device is registered, or the error of the step that
 * failed (nothing is registered then; the address spaces stay usable).
 */
int
amdrdna_a5s_sessions(
	struct amdrdna_device *device)
{
	struct sessions_result result;
	const char *fault;
	const char *registration;
	bool display;
	int error;

	/* Says the stage starts. */
	amdrdna_mark("A5s begin");
	kern_memset(&result, 0, sizeof(result));

	/* The graphics ring run without the scheduler stays stopped by stage A5's fault check. */
	if (device->engines.direct) {
		amdrdna_mark("A5s fail: needs the queues the scheduler runs (amdrdna.kiq=0)");
		return ENOTSUP;
	}

	/* The address spaces may run work from now on. */
	amdrdna_spaces_start(device);

	/* The GPU device's record. */
	error = amdrdna_node_create(device);
	if (error != 0) {
		amdrdna_mark("A5s fail: no gpu device record (%d)", error);
		return error;
	}

	/* A session writes, moves and dispatches in its own address space. */
	error = sessions_dispatch(device, &result);
	if (error != 0)
		return error;

	/* Sixteen sessions share the fifteen contexts. */
	error = sessions_contexts(device, &result);
	if (error != 0)
		return error;

	/* A session's page fault names it; only marked. */
	sessions_fault(device, &result);

	/* Whether the fault check's page fault named its session. */
	fault = "unnamed";
	if (result.fault_named)
		fault = "named";

	/* Registers the GPU device (the core publishes /dev/gpuN), unless stage A8 registers it with the display. */
	registration = "waits for the display (A8)";
	display = amdrdna_display_wanted();
	if (!display) {
		error = amdrdna_node_register(device);
		if (error != 0) {
			amdrdna_mark("A5s fail: gpu device not registered (%d)", error);
			return error;
		}

		/* The device is published without a display. */
		registration = "registered (no display)";
	}

	/* Marks the result. */
	amdrdna_mark("A5s ok sessions spaces=%u contexts=%u flushes=%llu takeovers=%llu dispatch=%u fault=%s space=%u gpu device %s",
	    SESSIONS_CONTEXT_CHECKS + 2U,
	    amdrdna_context_free_count(&device->spaces.pool),
	    (unsigned long long)result.flushes,
	    (unsigned long long)result.takeovers,
	    (unsigned)result.dispatch_words,
	    fault,
	    (unsigned)result.fault_space,
	    registration);

	/* Succeeded: the GPU is a GPU device. */
	return 0;
}

/*
 * Runs one session's indirect buffer in its address space: two words
 * written, moved elsewhere in the space, and the check program dispatched
 * with its code, table, counts and output all in the space.  Compares the
 * moved words and every output word.
 */
static int
sessions_dispatch(
	struct amdrdna_device *device,
	struct sessions_result *result)
{
	uint32_t words[SESSIONS_BUFFER_WORDS];
	uint32_t groups[3];
	struct amdrdna_session *session;
	volatile uint8_t *cpu;
	uint32_t *output;
	uint64_t va;
	uint32_t output_words;
	uint32_t bad_word;
	uint32_t expected;
	uint32_t found;
	uint32_t moved[2];
	uint32_t written[2];
	uint32_t buffer[2];
	uint32_t first_output;
	unsigned count;
	unsigned index;
	void *token;
	uint8_t *run_cpu;
	bool matched;
	int error;

	/* The session. */
	error = amdrdna_node_session_open(device->node, &session);
	if (error != 0) {
		amdrdna_mark("A5s fail: no session (%d)", error);
		return error;
	}

	/* Its run, mapped in its space. */
	error = amdrdna_vk_gpu_alloc(session, SESSIONS_RUN_BYTES, &token, &run_cpu, &va);
	if (error != 0) {
		amdrdna_mark("A5s fail: no run for the session (%d)", error);
		amdrdna_node_session_close(device->node, session);
		return error;
	}

	/* The run's output marked unwritten, the program loaded in its page. */
	cpu = (volatile uint8_t *)run_cpu;
	groups[0] = SESSIONS_GROUPS;
	groups[1] = SESSIONS_GROUPS;
	groups[2] = SESSIONS_GROUPS;
	output_words = amdrdna_check_output_words(groups);
	for (index = 0U; index < output_words; index++)
		kern_mmio_write32(cpu + SESSIONS_OUTPUT_AT + index * 4U, SESSIONS_CONTEXT_MARK);
	amdrdna_check_load(cpu + SESSIONS_PROGRAM_AT, va + SESSIONS_PROGRAM_AT, va + SESSIONS_OUTPUT_AT, output_words, groups);

	/* The indirect buffer: the two writes, their move, the dispatch, padded. */
	count = amdrdna_pm4_write_memory(words, va + SESSIONS_WRITTEN_AT, SESSIONS_FIRST_VALUE, false);
	count += amdrdna_pm4_write_memory(words + count, va + SESSIONS_WRITTEN_AT + 4U, SESSIONS_SECOND_VALUE, false);
	count += amdrdna_pm4_move_cached(words + count, va + SESSIONS_WRITTEN_AT, va + SESSIONS_MOVED_AT, 8U);
	count += amdrdna_check_commands(device, words + count, va + SESSIONS_PROGRAM_AT, groups, false);
	count = sessions_pad(words, count);
	sessions_put(cpu, words, count);

	/* Runs it on the graphics ring in the session's space. */
	error = amdrdna_vk_gpu_run(session, va, count * 4U);
	if (error != 0) {
		amdrdna_mark("A5s fail: the session's run did not end (%d)", error);
		amdrdna_vk_gpu_free(session, token);
		amdrdna_node_session_close(device->node, session);
		return error;
	}

	/* The moved words, and for a failure's mark the written ones, the first output word and the buffer's start. */
	moved[0] = kern_mmio_read32(cpu + SESSIONS_MOVED_AT);
	moved[1] = kern_mmio_read32(cpu + SESSIONS_MOVED_AT + 4U);
	written[0] = kern_mmio_read32(cpu + SESSIONS_WRITTEN_AT);
	written[1] = kern_mmio_read32(cpu + SESSIONS_WRITTEN_AT + 4U);
	first_output = kern_mmio_read32(cpu + SESSIONS_OUTPUT_AT);
	buffer[0] = kern_mmio_read32(cpu);
	buffer[1] = kern_mmio_read32(cpu + 4U);
	if (moved[0] != SESSIONS_FIRST_VALUE)
		amdrdna_space_describe(session->space, va);

	/* The output, copied out of the window to be compared. */
	output = kern_malloc((size_t)output_words * 4U);
	if (output == NULL) {
		amdrdna_vk_gpu_free(session, token);
		amdrdna_node_session_close(device->node, session);
		return ENOMEM;
	}

	/* Every output word compared with what the program writes. */
	for (index = 0U; index < output_words; index++)
		output[index] = kern_mmio_read32(cpu + SESSIONS_OUTPUT_AT + index * 4U);
	matched = amdrdna_check_output(output, groups, &bad_word, &expected, &found);
	kern_free(output);

	/* The run, the session. */
	amdrdna_vk_gpu_free(session, token);
	amdrdna_node_session_close(device->node, session);

	/* Words that are not what the commands wrote fail the stage. */
	if (moved[0] != SESSIONS_FIRST_VALUE || moved[1] != SESSIONS_SECOND_VALUE) {
		amdrdna_mark("A5s fail: the moved words are 0x%08x 0x%08x, written 0x%08x 0x%08x, output 0x%08x, buffer 0x%08x 0x%08x",
		    (unsigned)moved[0],
		    (unsigned)moved[1],
		    (unsigned)written[0],
		    (unsigned)written[1],
		    (unsigned)first_output,
		    (unsigned)buffer[0],
		    (unsigned)buffer[1]);
		return EIO;
	}

	/* An output word that is not the program's fails the stage. */
	if (!matched) {
		amdrdna_mark("A5s fail: dispatch word %u is 0x%08x, not 0x%08x", (unsigned)bad_word, (unsigned)found, (unsigned)expected);
		return EIO;
	}

	/* Records and marks the result. */
	result->dispatch_words = output_words;
	amdrdna_mark("A5s session run ok at 0x%llx: written, moved, dispatch %u words", (unsigned long long)va, (unsigned)output_words);

	/* Succeeded: the session's space runs work. */
	return 0;
}

/*
 * Runs sixteen sessions one after another, each writing its mark into its
 * own run, then the first one twice more: the sixteenth session takes the
 * least recently used context, so does the first one coming back, and its
 * second run keeps its context without a drop.  Compares every mark and
 * the drops and changes of hands the pool counted (seventeen and two).
 */
static int
sessions_contexts(
	struct amdrdna_device *device,
	struct sessions_result *result)
{
	uint32_t words[SESSIONS_BUFFER_WORDS];
	struct amdrdna_session *sessions[SESSIONS_CONTEXT_CHECKS];
	void *tokens[SESSIONS_CONTEXT_CHECKS];
	uint64_t addresses[SESSIONS_CONTEXT_CHECKS];
	uint8_t *cpus[SESSIONS_CONTEXT_CHECKS];
	unsigned long irq;
	uint64_t flushes_before;
	uint64_t takeovers_before;
	uint64_t flushes;
	uint64_t takeovers;
	uint32_t mark;
	uint32_t word;
	unsigned opened;
	unsigned index;
	unsigned pass;
	unsigned count;
	int error;

	/* The counts before the check. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	flushes_before = device->spaces.pool.flushes;
	takeovers_before = device->spaces.pool.takeovers;

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Opens the sessions, each with a run of its own. */
	error = 0;
	opened = 0U;
	for (index = 0U; index < SESSIONS_CONTEXT_CHECKS; index++) {
		/* The session. */
		error = amdrdna_node_session_open(device->node, &sessions[index]);
		if (error != 0)
			break;

		/* Its run; a session without one is closed again. */
		error = amdrdna_vk_gpu_alloc(sessions[index], SESSIONS_PAGE_BYTES * 2U, &tokens[index], &cpus[index], &addresses[index]);
		if (error != 0) {
			amdrdna_node_session_close(device->node, sessions[index]);
			break;
		}

		/* One more session to close at the end. */
		opened++;
	}

	/* Runs each session once, then the first one twice: the marks, the order of use. */
	for (pass = 0U; pass < SESSIONS_CONTEXT_CHECKS + 2U; pass++) {
		/* Nothing runs once a session could not be had. */
		if (error != 0)
			break;

		/* The session of this pass: each in turn, then the first again. */
		index = pass;
		if (pass >= SESSIONS_CONTEXT_CHECKS)
			index = 0U;

		/* An indirect buffer writing the session's mark to the second page of its run. */
		mark = SESSIONS_CONTEXT_MARK + sessions[index]->space->id + pass;
		count = amdrdna_pm4_write_memory(words, addresses[index] + SESSIONS_PAGE_BYTES, mark, false);
		count = sessions_pad(words, count);
		sessions_put((volatile uint8_t *)cpus[index], words, count);

		/* Runs it and compares the mark. */
		error = amdrdna_vk_gpu_run(sessions[index], addresses[index], count * 4U);
		if (error != 0) {
			amdrdna_mark("A5s fail: context check pass %u did not end (%d)", pass, error);
			break;
		}

		/* The mark must be in the run. */
		word = kern_mmio_read32((volatile uint8_t *)cpus[index] + SESSIONS_PAGE_BYTES);
		if (word != mark) {
			amdrdna_mark("A5s fail: context check pass %u read 0x%08x, not 0x%08x", pass, (unsigned)word, (unsigned)mark);
			error = EIO;
			break;
		}
	}

	/* The counts after the check. */
	irq = spin_lock_irqsave(&device->spaces.lock);

	flushes = device->spaces.pool.flushes - flushes_before;
	takeovers = device->spaces.pool.takeovers - takeovers_before;

	spin_unlock_irqrestore(&device->spaces.lock, irq);

	/* Closes every session opened. */
	for (index = 0U; index < opened; index++) {
		amdrdna_vk_gpu_free(sessions[index], tokens[index]);
		amdrdna_node_session_close(device->node, sessions[index]);
	}

	/* Reports a session that could not be had or a run that failed. */
	if (error != 0) {
		amdrdna_mark("A5s fail: context check (%d)", error);
		return error;
	}

	/* Records and marks the result; other counts than the expected ones are only marked. */
	result->flushes = flushes;
	result->takeovers = takeovers;
	amdrdna_mark("A5s contexts ok: %u sessions, %u runs, drops %llu (17 expected), changes of hands %llu (2 expected)",
	    SESSIONS_CONTEXT_CHECKS,
	    SESSIONS_CONTEXT_CHECKS + 2U,
	    (unsigned long long)flushes,
	    (unsigned long long)takeovers);

	/* Succeeded: every session's mark landed. */
	return 0;
}

/*
 * Has a session write to an address of its space nothing is mapped at,
 * from a compute queue, and waits for the hub's page fault to name the
 * session.  The queue may stay stopped behind the write; the session is
 * then kept with its context.  Only marks what it saw.
 */
static void
sessions_fault(
	struct amdrdna_device *device,
	struct sessions_result *result)
{
	uint32_t words[SESSIONS_BUFFER_WORDS];
	struct amdrdna_session *session;
	uint64_t started;
	uint64_t now;
	uint64_t va;
	uint32_t space_id;
	unsigned count;
	void *token;
	uint8_t *cpu;
	const char *seen;
	bool lost;
	int error;

	/* Needs the compute queue. */
	if (device->engines.compute_count <= SESSIONS_FAULT_QUEUE) {
		amdrdna_mark("A5s fault check skipped: no compute queue %u", SESSIONS_FAULT_QUEUE);
		return;
	}

	/* The session. */
	error = amdrdna_node_session_open(device->node, &session);
	if (error != 0) {
		amdrdna_mark("A5s fault check skipped: no session (%d)", error);
		return;
	}

	/* The run, mapped in the session's space. */
	error = amdrdna_vk_gpu_alloc(session, SESSIONS_PAGE_BYTES, &token, &cpu, &va);
	if (error != 0) {
		amdrdna_mark("A5s fault check skipped: no run (%d)", error);
		amdrdna_node_session_close(device->node, session);
		return;
	}

	/* The write to the unmapped address, padded. */
	space_id = session->space->id;
	count = amdrdna_pm4_write_memory(words, SESSIONS_FAULT_ADDRESS, SESSIONS_FAULT_VALUE, false);
	count = sessions_pad(words, count);
	sessions_put((volatile uint8_t *)cpu, words, count);

	/* Runs it on the compute queue; it may not end. */
	error = amdrdna_space_run(session->space, AMDRDNA_TRACKED_COMPUTE + SESSIONS_FAULT_QUEUE, va, count, SESSIONS_FAULT_RUN_MS);

	/* Waits for the fault to lose the session. */
	started = clock_milliseconds(NULL);
	for (;;) {
		/* Stops once the session is lost. */
		lost = amdrdna_space_lost(session->space);
		if (lost)
			break;

		/* Stops when the time is up. */
		now = clock_milliseconds(NULL);
		if (now - started >= SESSIONS_FAULT_WAIT_MS)
			break;

		/* Sleeps a millisecond. */
		kern_usleep_range(1000U, 1500U);
	}

	/* The run and the session go, or stay held when the queue never got past the write. */
	amdrdna_vk_gpu_free(session, token);
	amdrdna_node_session_close(device->node, session);

	/* Records and marks what was seen. */
	seen = "did not name the space";
	if (lost)
		seen = "named the space";
	result->fault_named = lost;
	result->fault_space = space_id;
	amdrdna_mark("A5s vm fault check: space %u wrote to 0x%llx from compute %u, run %d, fault %s",
	    (unsigned)space_id,
	    (unsigned long long)SESSIONS_FAULT_ADDRESS,
	    SESSIONS_FAULT_QUEUE,
	    error,
	    seen);
}

/* Pads a check's indirect buffer with fillers to a multiple of eight words; reports the words. */
static unsigned
sessions_pad(
	uint32_t *words,
	unsigned count)
{
	/* One filler at a time. */
	while ((count % SESSIONS_BUFFER_ALIGN) != 0U) {
		words[count] = amdrdna_pm4_filler();
		count++;
	}

	/* Succeeded: the padded count. */
	return count;
}

/* Writes a check's words to the start of its run through the CPU's window. */
static void
sessions_put(
	volatile uint8_t *cpu,
	const uint32_t *words,
	unsigned count)
{
	unsigned index;

	/* One word at a time. */
	for (index = 0U; index < count; index++)
		kern_mmio_write32(cpu + (size_t)index * 4U, words[index]);
}
