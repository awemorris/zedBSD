/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A check, in a test kernel, of the core's compute path: the driver's
 * own programs run on the core without a user program, and what they
 * wrote is read back.
 *
 * It reports three lines.  The first says whether a compute kernel wrote
 * every word of a 64 KiB window, read through the uncached alias.  The
 * second names the path of the core's data masters, the stores of the
 * shading cluster, and checks whether the processor's cached view sees
 * them: clean lines loaded before the core writes, eight rounds of
 * 64 KiB across 2 MiB, nothing done about the core's system level cache,
 * the verdict from the cached view only.  The third tells whether the
 * task issue counts temporaries in blocks of four or of two, where the
 * hardware shows it.  None of them changes how memory is shared.
 *
 * The coherency rounds follow a fixed procedure.  A control first shows
 * that the processor alone can be shown a stale line: clean lines loaded
 * through the cached view, other values written through the uncached
 * alias, and the old values read cached again.  Without it a matching
 * round would prove nothing.  Each round then loads its window's lines
 * clean before the core writes, so a write that does not reach the
 * processor's caches reads back stale.  The eight windows of 64 KiB lie
 * 128 KiB apart across 2 MiB, so they pass through different sets of the
 * level-two cache, and each is small enough that no line is evicted and
 * written back to fake a pass.  The verdict comes from the cached view
 * only; the alias only tells whether a write reached memory at all.  The
 * whole range is dropped from the caches afterwards, because the control
 * and a failing round leave stale clean lines that the memory's next
 * owner would read.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/render-sr-program.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/compute-program.h"
#include "drivers/gpu/powervr/compiler/shader.h"
#include "drivers/gpu/powervr/compiler/shader-private.h"
#include "drivers/gpu/powervr/compiler/usc-encode.h"

/* The vkcompute fill shader the compiled-fill probe compiles (its SPIR-V words, a header of the test program). */
#include "../userland/tests/vkcompute/shader.h"

/*
 * The run the rounds use, the rounds, how far apart they are, the bytes
 * each covers, and the steps one instance takes over a window.
 */
#define CHECK_BYTES		0x200000U
#define CHECK_ROUNDS		8U
#define CHECK_STRIDE		0x20000U
#define CHECK_WINDOW		0x10000U
#define CHECK_WORDS		(CHECK_WINDOW / 4U)
#define CHECK_STEPS		(CHECK_WINDOW / POWERVR_COMPUTE_STORE_STRIDE)

/* Where in the run the temporaries programs write, past every window. */
#define TEMPS_OFFSET		0x1f0000U

/*
 * The device addresses of the run, of the buffer holding the control
 * streams and the resume area, and of the data sequencer and shading
 * cluster programs, each in its heap with room before it.
 */

/* The stream buffer: the resume area first, then one stream per kick. */
#define STREAM_BYTES		0x10000U
#define STREAM_FIRST		0x1000U
#define STREAM_SPACING		0x100U

/* The data sequencer buffer: one 64-byte slot per program, data then code. */
#define PDS_BYTES		0x1000U
#define PDS_SLOT		0x40U
#define PDS_CODE		0x10U

/* The shading cluster buffer: one 32 KiB room per program. */
#define USC_ROOM		0x8000U
#define USC_PROGRAMS		(CHECK_ROUNDS + 4U)

/*
 * The probes' rooms, after the temporaries program's: each probe program
 * and the two short store programs 4 KiB apart in them (three rooms).
 */
#define USC_PROBE_ROOM		((CHECK_ROUNDS + 1U) * USC_ROOM)
#define USC_PROBE_SPACING	0x1000U
#define USC_STEPS_ONE		(USC_PROBE_ROOM + POWERVR_COMPUTE_PROBES * USC_PROBE_SPACING)
#define USC_STEPS_FOUR		(USC_STEPS_ONE + USC_PROBE_SPACING)
#define USC_BYTES		(USC_ROOM * USC_PROGRAMS)

/*
 * The kicks: one per round, then the temporaries program issued with
 * blocks of four and of two; the fence program takes the slot after.
 */
#define KICK_TEMPS_FOUR		CHECK_ROUNDS
#define KICK_TEMPS_TWO		(CHECK_ROUNDS + 1U)
#define KICK_COUNT		(CHECK_ROUNDS + 2U)

/* The probe kicks' streams, after the fence program's slot. */
#define KICK_END_ONLY		(KICK_COUNT + 1U)
#define KICK_FENCE_ONLY		(KICK_COUNT + 2U)

/* The probe kicks of the integer path, and the two short store kicks, after them. */
#define KICK_PROBE_FIRST	(KICK_COUNT + 3U)
#define KICK_STEPS_ONE		(KICK_PROBE_FIRST + POWERVR_COMPUTE_PROBES)
#define KICK_STEPS_FOUR		(KICK_STEPS_ONE + 1U)

/* The temporaries program issued with the wide field, after the short store kicks. */
#define KICK_TEMPS_WIDE		(KICK_STEPS_FOUR + 1U)

/*
 * The temporaries field the store, probe and wide temporaries kicks
 * give: 16 in the field, at least 16 registers an instance whatever the
 * unit the task issue counts in.  The board's checks wrote fewer words
 * the smaller the field was, and a program of eight temporaries faulted
 * with the fields of four and two, as if neighbouring instances shared
 * registers.
 */
#define CHECK_WIDE_FIELD	16U

/*
 * The grid of the temporaries program's kicks that tells which setting
 * gives an instance its registers: the kernel's unified store size (the
 * registers written into KERNEL0, 32, 16, 8 or 0) against the task
 * issue's temporaries field (16, 8, 4 or 2), widest first, after the
 * wide temporaries kick.
 */
#define GRID_SIZES		4U

/* Whether the grid runs: off, since its narrow cells fault and lose the context (set it to 1 to read the grid). */
#define CHECK_GRID		0
#define GRID_KICKS		(GRID_SIZES * GRID_SIZES)
#define KICK_GRID_FIRST		(KICK_TEMPS_WIDE + 1U)

/*
 * The compiled-fill probe's program, 4 KiB after the short stores' in
 * the probes' rooms (its kick the one-kernel executor's form in the
 * first probe's slot, write_workgroup_kick), and the words it fills.
 */
#define USC_COMPILED		(USC_STEPS_FOUR + USC_PROBE_SPACING)
#define COMPILED_OFFSET		0x1c0000U

/*
 * What the check reports by default: the hand-written kernel, the
 * coherency rounds, the temporaries and the compiled fill over 1, 4 and
 * 128 workgroups.  With POWERVR_DIAGNOSTICS also the probes (the integer
 * path, the short stores, the workgroup id, the compiled fill's register
 * dumps and the words it wrote), and with POWERVR_COMPUTE_LOCKUP_PROBES
 * too the ones that lock the core up or fault (the fence variants, the
 * executor layouts, the no-operation probe, the compiled fill's own
 * workgroup id).
 */
#define CHECK_DIAGNOSTICS	POWERVR_DIAGNOSTICS
#define CHECK_LOCKUP		(POWERVR_DIAGNOSTICS && POWERVR_COMPUTE_LOCKUP_PROBES)
#define COMPILED_GROUPS		4U
#define COMPILED_VK_GROUPS	128U

/*
 * The room for the compiled fill and its dump programs (to the end of
 * the shading cluster buffer), "no register" for the dump's operations,
 * and the special register that holds the instance (the lane).
 */
/*
 * The words the compiled fill's scan covers (64 KiB either side of its
 * words), the pattern each starts with, and how many changes are listed.
 */
#define SCAN_FIRST		(COMPILED_OFFSET - 0x10000U)
#define SCAN_WORDS		(0x20000U / 4U)
#define SCAN_PATTERN(word)	(0x5a000000U | (uint32_t)(word))
#define SCAN_LINES		48U

#define USC_DUMP_ROOM		(USC_BYTES - USC_COMPILED)
#define DUMP_NONE		0xffffU
#define DUMP_SPECIAL_INSTANCE	51U

/*
 * The layout probes: one stream of kernels of the one-step store program
 * (its data sequencer program shared), a fence kernel and the end, as
 * the Vulkan executor lays a dispatch out; its stream at the kick slot
 * whose place is 0x8000 into the stream buffer.  Then the changes to
 * that layout the probes try.
 */
#define KICK_MANY		((0x8000U - STREAM_FIRST) / STREAM_SPACING)
#define MANY_NO_FENCE		0x1U
#define MANY_OWN_SLOTS		0x2U
#define MANY_ALL_FENCED		0x4U
#define MANY_NO_RESUME		0x8U
#define MANY_FENCE_KEEP_SHARED	0x10U
#define MANY_FENCE_STORE_PDS	0x20U
#define MANY_TWO_FENCES		0x40U
#define FENCE_VARIANT_KERNELS	16U

/*
 * Where in the run the probes write: each probe's area 4 KiB apart
 * from 0x1a0000 (inside the run, below the temporaries program's), and
 * the two short store programs' windows.
 */
#define PROBE_OFFSET		0x1a0000U
#define PROBE_SPACING		0x1000U
#define STEPS_ONE_OFFSET	0x180000U
#define STEPS_FOUR_OFFSET	0x190000U

/* How long after a probe kick the core's registers are read, and how many. */
#define PROBE_STATE_MS		100U
#define PROBE_REGISTERS		38U
#define PDS_FENCE		KICK_COUNT

/* The shared register allocation kernel's data sequencer program: the last slot. */
#define PDS_SHARED		(PDS_BYTES / PDS_SLOT - 1U)

/* The data sequencer temporaries a compute kernel's inputs take. */
#define PDS_INPUT_TEMPORARIES	3U

/* The task size of the fence kernel. */
#define FENCE_INSTANCES		8U

/* The bits of check_state.mapped: which buffers the session maps. */
#define MAPPED_RUN		0x1U
#define MAPPED_STREAMS		0x2U
#define MAPPED_PDS		0x4U
#define MAPPED_USC		0x8U

/* How long one kick may take. */
#define CHECK_WAIT_MS		2000U

/* What the control found: whether the processor alone can be shown a stale line. */
enum control_result {
	CONTROL_OK = 0,
	CONTROL_INCONCLUSIVE
};

/* The buffers of the check, where the core sees them, and the session they are mapped in. */
/*
 * Where the run and the streams are mapped: places the session's general
 * heap hands out, so they never meet what the session itself mapped
 * there (the check runs once, on one thread).
 */
static uint64_t run_address;
static uint64_t stream_address;

/* Where the programs are mapped, as offsets in their heaps: places the session's program heaps hand out. */
static uint32_t pds_offset;
static uint32_t usc_offset;

struct check_state {
	struct powervr_render *render;
	struct powervr_session *session;
	struct powervr_buffer *run;
	struct powervr_buffer *streams;
	struct powervr_buffer *pds;
	struct powervr_buffer *usc;
	volatile uint32_t *alias;
	const volatile uint32_t *cached;
	unsigned mapped;
	unsigned placed;
	int probing;
	int no_resume;
	unsigned max_instances;
};

/* What the rounds found, word by word. */
struct check_counts {
	unsigned rounds_matched;
	uint64_t stale;
	uint64_t unwritten;
	int error;
};

static int setup(struct check_state *state, const char **step);
static void teardown(struct check_state *state);
static int take_buffer(struct check_state *state, uint64_t bytes, uint64_t address, struct powervr_buffer **result);
static int take_program(struct check_state *state, uint64_t bytes, enum powervr_program_heap heap, struct powervr_buffer **result, uint32_t *offset);
static int write_programs(struct check_state *state);
static int write_kick(struct check_state *state, unsigned kick, uint32_t usc_place, unsigned inputs, unsigned field);
static int write_probes(struct check_state *state, uint8_t *code);
static void run_probes(struct check_state *state);
static void run_nop_probe(struct check_state *state);
static void run_compiled_fill(struct check_state *state, int dumps, unsigned groups, unsigned field_override, unsigned max_instances);
static int compile_fill(struct powervr_shader_workspace *workspace, struct powervr_shader_program *program, uint8_t *code, uint64_t address, unsigned *field);
static void run_cf0_probe(struct check_state *state);
static int fresh_session(struct check_state *state);
static void run_rule_probes(struct check_state *state);
static int run_prefixes(struct check_state *state, const struct shader_backend *backend);
static int dump_append(struct shader_backend *dump, unsigned kind, unsigned destination, unsigned source_0_bank, unsigned source_0, unsigned source_1, unsigned source_2, uint32_t immediate);
static int dump_program(const struct shader_backend *backend, struct shader_backend *dump, unsigned keep, uint64_t address, uint8_t *code, size_t room, size_t *length);
static int run_many_kernels(struct check_state *state, unsigned kernels, unsigned flags, const char *variant);
static int run_fence_variants(struct check_state *state);
static int put_fence_kernel(struct check_state *state, uint32_t *stream);
static int put_shared_kernel(struct check_state *state, uint32_t *stream);
static void run_layouts(struct check_state *state);
static void run_workgroup_probe(struct check_state *state);
static struct shader_machine_operation *dump_last(struct shader_backend *dump);
static int workgroup_program(struct shader_backend *dump, unsigned bank, unsigned index, uint64_t address, uint8_t *code, size_t room, size_t *length);
static int write_workgroup_kick(struct check_state *state, unsigned field, unsigned groups);
static int write_grid(struct check_state *state);
static void run_grid(struct check_state *state);
static uint32_t own_words(struct check_state *state, unsigned kind);
static uint32_t address_words(struct check_state *state, unsigned kind);
static uint32_t probe_word(struct check_state *state, unsigned kind, uint32_t word);
static int run_store_window(struct check_state *state, unsigned kick_index, uint32_t offset, uint32_t words, unsigned *written);
static int write_probe_streams(struct check_state *state);
static int probe_kick(struct check_state *state, unsigned kick_index, const char *what);
static void probe_report(struct check_state *state, unsigned kick_index);
static void probe_registers(struct powervr_core *core);
static void put_bytes(struct powervr_buffer *buffer, uint32_t offset, const uint8_t *bytes, size_t length);
static void put_word(struct powervr_buffer *buffer, uint32_t offset, uint32_t word);
static int kick(struct check_state *state, unsigned kick_index);
static int run_kernel(struct check_state *state, unsigned *written);
static enum control_result run_control(struct check_state *state);
static void run_round(struct check_state *state, unsigned round, struct check_counts *counts);
static void run_coherency(struct check_state *state);
static int run_temps(struct check_state *state, unsigned kick_index, unsigned *wrong);
static void report_temps(int four_error, unsigned four_wrong, int two_error, unsigned two_wrong);
static uint32_t pattern(unsigned round, uint32_t word);
static uint32_t before(unsigned round, uint32_t word);
static void load_cached(const volatile uint32_t *words, uint32_t count);

/*
 * Runs the driver's own compute work on the core and reports what it saw.
 */
void
powervr_compute_check(
	struct powervr_render *render)
{
	static const unsigned sweep[] = { 1, COMPILED_GROUPS, COMPILED_VK_GROUPS };
	struct check_state state;
	const char *step;
	unsigned written;
	unsigned four_wrong;
	unsigned two_wrong;
	unsigned wide_wrong;
	unsigned index;
	int wide_error;
	int four_error;
	int two_error;
	int kernel_error;
	int error;

	/* Opens a session of the check's own, with its buffers mapped and its programs written. */
	state.render = render;
	step = "open";
	error = setup(&state, &step);
	if (error != 0) {
		kern_logf("powervr: compute kernel: not run, %s (%d)\n", step, error);
		teardown(&state);
		return;
	}

	/* The fence variants, which lock the compute data master up on the VF2, only when asked, first, leaving a fresh session. */
	if (CHECK_LOCKUP) {
		error = run_fence_variants(&state);
		if (error != 0) {
			teardown(&state);
			return;
		}
	}

	/* The kernel alone: every word of the first window, read through the alias. */
	written = 0;
	error = run_kernel(&state, &written);
	if (error != 0) {
		kern_logf("powervr: compute kernel: FAILED (%d), %u/%u words written\n", error, written, (unsigned)CHECK_WORDS);
		powervr_firmware_dump(state.render->core, POWERVR_KERNEL_CCB_SLOTS);
	} else if (written != CHECK_WORDS) {
		kern_logf("powervr: compute kernel: FAILED, %u/%u words written\n", written, (unsigned)CHECK_WORDS);
		powervr_firmware_dump(state.render->core, POWERVR_KERNEL_CCB_SLOTS);
	} else {
		kern_logf("powervr: compute kernel: OK, %u/%u words written\n", written, (unsigned)CHECK_WORDS);
	}
	kernel_error = error;
	if (kernel_error == 0 && written != CHECK_WORDS)
		kernel_error = EIO;

	/* A kick the core never finished may still write; everything stays with the core. */
	if (error == ETIMEDOUT) {
		kern_logf("powervr: compute check: stopped, its memory left to the core\n");
		return;
	}

	/* The coherency rounds, only over a kernel that wrote what it should. */
	if (error == 0 && written == CHECK_WORDS)
		run_coherency(&state);

	/* The probes of the integer path and the short stores, before any kick that may fault, when diagnosing. */
	if (CHECK_DIAGNOSTICS)
		run_probes(&state);

	/* The temporaries program with the wide field first: it must always work. */
	wide_wrong = 0;
	wide_error = run_temps(&state, KICK_TEMPS_WIDE, &wide_wrong);
	kern_logf("powervr: compute temporaries: wide field %u/%u wrong (%d)\n", wide_wrong, (unsigned)POWERVR_COMPUTE_INSTANCES, wide_error);
	if (wide_error == ETIMEDOUT) {
		kern_logf("powervr: compute check: stopped, its memory left to the core\n");
		return;
	}

	/* The temporaries field: the driver's (one register more) first, the exact one last, as it may fault. */
	four_wrong = 0;
	two_wrong = 0;
	two_error = run_temps(&state, KICK_TEMPS_TWO, &two_wrong);
	four_error = 0;
	if (two_error != ETIMEDOUT)
		four_error = run_temps(&state, KICK_TEMPS_FOUR, &four_wrong);
	report_temps(four_error, four_wrong, two_error, two_wrong);
	if (four_error == ETIMEDOUT || two_error == ETIMEDOUT) {
		kern_logf("powervr: compute check: stopped, its memory left to the core\n");
		return;
	}

	/* The probe with no-operation groups, which may stop the core, only when asked. */
	if (CHECK_LOCKUP)
		run_nop_probe(&state);

	/* The compiler's program (no spacing, as Mesa's) as one workgroup, its prefixes dumped. */
	if (CHECK_DIAGNOSTICS)
		run_compiled_fill(&state, 1, 1, 0, 0);

	/* The register-window probe only when asked; the hand-written workgroup id probe (sessions of its own). */
	if (CHECK_DIAGNOSTICS)
		run_workgroup_probe(&state);

	/* The shading cluster rules the compiler kept, each on the three kernels, when diagnosing, on a fresh session. */
	if (CHECK_DIAGNOSTICS) {
		error = fresh_session(&state);
		if (error == 0)
			run_rule_probes(&state);
	}

	/* The compiled fill's own move from the coefficients over four workgroups, only when asked, on a fresh session. */
	if (CHECK_LOCKUP) {
		error = fresh_session(&state);
		if (error == 0)
			run_cf0_probe(&state);
	}

	/* The compiled fill over 1, 4 and 128 workgroups (as vkcompute), the defaults, each on a fresh session. */
	for (index = 0; index < sizeof(sweep) / sizeof(sweep[0]); index++) {
		error = fresh_session(&state);
		if (error == 0)
			run_compiled_fill(&state, 0, sweep[index], 0, 0);
	}

	/* The executor's layouts, only when asked. */
	if (CHECK_LOCKUP)
		run_layouts(&state);

	/* The grid of unified sizes and fields, last and only when asked, as its narrow cells fault. */
	if (CHECK_GRID)
		run_grid(&state);

	/* Unmaps and returns everything. */
	teardown(&state);

	/*
	 * The compute stage is the hand-written kernel's: a probe's reset
	 * (the compiled fill is reported on its own lines) does not keep the
	 * Vulkan device closed.
	 */
	if (kernel_error == 0)
		powervr_stage_ok(POWERVR_STAGE_COMPUTE);
	else
		powervr_stage_fail(POWERVR_STAGE_COMPUTE, powervr_stage_reason_of(kernel_error));
}

/* Opens the session, takes and maps the buffers, and writes the programs. */
static int
setup(
	struct check_state *state,
	const char **step)
{
	void *opaque;
	int error;

	/* Nothing is held yet. */
	state->session = NULL;
	state->run = NULL;
	state->streams = NULL;
	state->pds = NULL;
	state->usc = NULL;
	state->mapped = 0;
	state->placed = 0;
	state->probing = 0;
	state->no_resume = 0;
	state->max_instances = 0;

	/* A session of the check's own. */
	*step = "open";
	error = state->render->ops.open(state->render, &opaque);
	if (error != 0)
		return error;
	state->session = opaque;

	/* Places for the run and the streams in the session's general heap. */
	*step = "memory places";
	mutex_lock(&state->render->core->lock);

	error = powervr_gpu_space_take(&state->session->space, CHECK_BYTES, &run_address);
	if (error == 0) {
		state->placed |= MAPPED_RUN;
		error = powervr_gpu_space_take(&state->session->space, STREAM_BYTES, &stream_address);
		if (error == 0)
			state->placed |= MAPPED_STREAMS;
	}

	mutex_unlock(&state->render->core->lock);
	if (error != 0)
		return error;

	/* The run the kernels write, with its alias and its cached view. */
	*step = "memory: run";
	error = take_buffer(state, CHECK_BYTES, run_address, &state->run);
	if (error != 0)
		return error;
	state->alias = (volatile uint32_t *)state->run->view;
	state->cached = kern_pmem_to_kernel(state->run->run.paddr);

	/* The control streams and the resume area, the data sequencer and the shading cluster programs. */
	*step = "memory: streams";
	error = take_buffer(state, STREAM_BYTES, stream_address, &state->streams);
	if (error != 0)
		return error;

	*step = "memory: data sequencer programs";
	error = take_program(state, PDS_BYTES, POWERVR_PROGRAM_PDS, &state->pds, &pds_offset);
	if (error != 0)
		return error;
	state->mapped |= MAPPED_PDS;

	*step = "memory: shading cluster programs";
	error = take_program(state, USC_BYTES, POWERVR_PROGRAM_USC, &state->usc, &usc_offset);
	if (error != 0)
		return error;
	state->mapped |= MAPPED_USC;

	/* Has the core drop stale table entries before it walks the new ones. */
	*step = "mmu";
	mutex_lock(&state->render->core->lock);

	error = powervr_command_mmu_flush(state->render->core);

	mutex_unlock(&state->render->core->lock);
	if (error != 0)
		return error;

	/* The programs and the streams. */
	*step = "programs";
	error = write_programs(state);
	if (error != 0)
		return error;

	/* Succeeded: the check can kick. */
	return 0;
}

/* Unmaps the buffers, closes the session and returns the buffers. */
static void
teardown(
	struct check_state *state)
{
	struct powervr_render *render;

	/* Removes the programs' mappings; each unmap takes the lock and flushes. */
	render = state->render;
	if ((state->mapped & MAPPED_PDS) != 0)
		powervr_session_program_unmap(state->session, POWERVR_PROGRAM_PDS, state->pds, POWERVR_PDS_HEAP_BASE + pds_offset);
	if ((state->mapped & MAPPED_USC) != 0)
		powervr_session_program_unmap(state->session, POWERVR_PROGRAM_USC, state->usc, POWERVR_USC_HEAP_BASE + usc_offset);

	/* Removes the mappings the check made. */
	mutex_lock(&render->core->lock);

	if ((state->mapped & MAPPED_RUN) != 0)
		powervr_gpu_mmu_unmap(&state->session->mmu, run_address, state->run->run.size);
	if ((state->mapped & MAPPED_STREAMS) != 0)
		powervr_gpu_mmu_unmap(&state->session->mmu, stream_address, state->streams->run.size);
	if (state->mapped != 0)
		(void)powervr_command_mmu_flush(render->core);
	if ((state->placed & MAPPED_RUN) != 0)
		(void)powervr_gpu_space_give(&state->session->space, run_address);
	if ((state->placed & MAPPED_STREAMS) != 0)
		(void)powervr_gpu_space_give(&state->session->space, stream_address);

	mutex_unlock(&render->core->lock);

	/* Drops every line the cached reads loaded, so nothing stale outlives the check. */
	if (state->run != NULL)
		kern_dcache_invalidate_range((const void *)state->cached, CHECK_BYTES);

	/* Closes the session. */
	if (state->session != NULL)
		render->ops.close(render, state->session);

	/* Returns the buffers. */
	if (state->run != NULL)
		powervr_buffer_put(render, state->run);
	if (state->streams != NULL)
		powervr_buffer_put(render, state->streams);
	if (state->pds != NULL)
		powervr_buffer_put(render, state->pds);
	if (state->usc != NULL)
		powervr_buffer_put(render, state->usc);
}

/*
 * Takes a buffer and maps it at a device address of the session, going
 * around the core's system level cache as every buffer the processor
 * reads or writes does.
 */
static int
take_buffer(
	struct check_state *state,
	uint64_t bytes,
	uint64_t address,
	struct powervr_buffer **result)
{
	struct powervr_buffer *buffer;
	unsigned flags;
	int error;

	/* A zeroed, physically contiguous buffer with its uncached view. */
	error = powervr_buffer_create(state->render, bytes, &buffer);
	if (error != 0)
		return error;

	/* The attributes of a buffer the processor sees. */
	flags = powervr_gpu_buffer_flags(1, 0);

	/* Maps it at its place. */
	mutex_lock(&state->render->core->lock);

	error = powervr_gpu_mmu_map(&state->session->mmu, address, buffer->run.paddr, buffer->run.size, flags);

	mutex_unlock(&state->render->core->lock);

	/* Not mapped: the buffer goes back. */
	if (error != 0) {
		powervr_buffer_put(state->render, buffer);
		return error;
	}

	/* Notes the mapping by the buffer's place, so teardown removes it. */
	if (address == run_address) {
		state->mapped |= MAPPED_RUN;
	} else {
		state->mapped |= MAPPED_STREAMS;
	}

	/* Succeeded: the buffer is mapped. */
	*result = buffer;
	return 0;
}

/*
 * Writes the shading cluster programs, the data sequencer programs and
 * one control stream per kick.
 */
static int
write_programs(
	struct check_state *state)
{
	uint8_t *code;
	size_t length;
	uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS];
	uint32_t words[POWERVR_COMPUTE_PDS_CODE_MAX];
	unsigned count;
	unsigned field_four;
	unsigned field_two;
	unsigned store_field;
	unsigned round;
	unsigned i;
	int error;

	/* A staging area for one program. */
	code = kern_malloc(USC_ROOM);
	if (code == NULL)
		return ENOMEM;

	/* One store program per round, each over its own window. */
	error = 0;
	for (round = 0; round < CHECK_ROUNDS; round++) {
		error = powervr_compute_store_program(run_address + (uint64_t)round * CHECK_STRIDE, CHECK_STEPS, code, USC_ROOM, &length);
		if (error != 0)
			break;
		put_bytes(state->usc, round * USC_ROOM, code, length);
	}

	/* The temporaries program, after the store programs. */
	if (error == 0) {
		error = powervr_compute_temps_program(run_address + TEMPS_OFFSET, code, USC_ROOM, &length);
		if (error == 0)
			put_bytes(state->usc, CHECK_ROUNDS * USC_ROOM, code, length);
	}

	/* The probe programs and their kicks, after the temporaries program. */
	if (error == 0)
		error = write_probes(state, code);

	kern_free(code);
	if (error != 0)
		return error;

	/* The fence program, in the slot after the kicks'. */
	error = powervr_compute_fence_program(data, words, &count);
	if (error != 0)
		return error;

	put_word(state->pds, PDS_FENCE * PDS_SLOT, data[0]);
	put_word(state->pds, PDS_FENCE * PDS_SLOT + 4U, data[1]);
	for (i = 0; i < count; i++)
		put_word(state->pds, PDS_FENCE * PDS_SLOT + PDS_CODE + i * 4U, words[i]);

	/* The shared register allocation kernel's program, in the last slot. */
	error = powervr_compute_shared_program(data, words, &count);
	if (error != 0)
		return error;
	put_word(state->pds, PDS_SHARED * PDS_SLOT, data[0]);
	put_word(state->pds, PDS_SHARED * PDS_SLOT + 4U, data[1]);
	for (i = 0; i < count; i++)
		put_word(state->pds, PDS_SHARED * PDS_SLOT + PDS_CODE + i * 4U, words[i]);

	/* The probe kicks' streams: the end alone; the fence kernel and the end. */
	error = write_probe_streams(state);
	if (error != 0)
		return error;

	/*
	 * One kick per round, issuing the round's store program.  Its field
	 * counts the temporaries in blocks of four, as Mesa's task issue does
	 * (pvr_pds_setup_doutu) for one register more than the program uses
	 * (powervr_compute_task_field), with no unified store inputs.
	 */
	error = powervr_compute_task_field(POWERVR_COMPUTE_STORE_TEMPORARIES, &store_field);
	if (error != 0)
		return error;

	for (round = 0; round < CHECK_ROUNDS; round++) {
		error = write_kick(state, round, round * USC_ROOM, 0, store_field);
		if (error != 0)
			return error;
	}

	/*
	 * The temporaries program issued with exactly its eight registers in
	 * blocks of four (field 2), and with one register more (field 3, the
	 * driver's field).
	 */
	error = powervr_compute_temporaries_field(POWERVR_COMPUTE_TEMPS_TEMPORARIES, 4, &field_four);
	if (error != 0)
		return error;

	error = powervr_compute_task_field(POWERVR_COMPUTE_TEMPS_TEMPORARIES, &field_two);
	if (error != 0)
		return error;

	error = write_kick(state, KICK_TEMPS_FOUR, CHECK_ROUNDS * USC_ROOM, 0, field_four);
	if (error != 0)
		return error;

	error = write_kick(state, KICK_TEMPS_TWO, CHECK_ROUNDS * USC_ROOM, 0, field_two);
	if (error != 0)
		return error;

	/* And with the wide field: it must always work. */
	error = write_kick(state, KICK_TEMPS_WIDE, CHECK_ROUNDS * USC_ROOM, 0, CHECK_WIDE_FIELD);
	if (error != 0)
		return error;

	/* The grid of unified store sizes and temporaries fields. */
	error = write_grid(state);
	if (error != 0)
		return error;

	/* Succeeded: everything the kicks read is in memory. */
	return 0;
}

/*
 * Writes one kick's data sequencer program and its control stream: the
 * kernel, the fence kernel, the end.
 */
static int
write_kick(
	struct check_state *state,
	unsigned kick_index,
	uint32_t usc_place,
	unsigned inputs,
	unsigned field)
{
	struct powervr_compute_kernel kernel;
	uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS];
	uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX];
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	uint32_t slot;
	uint32_t stream;
	unsigned count;
	unsigned i;
	int error;

	/* The program issuing the shading cluster program, in the kick's slot. */
	error = powervr_compute_kick_program(usc_offset + (uint64_t)usc_place, field, data, code, &count);
	if (error != 0)
		return error;

	slot = kick_index * PDS_SLOT;
	put_word(state->pds, slot, data[0]);
	put_word(state->pds, slot + 4U, data[1]);
	for (i = 0; i < count; i++)
		put_word(state->pds, slot + PDS_CODE + i * 4U, code[i]);

	/* The kernel: one workgroup of a whole task, on any cluster, no shared registers. */
	kern_memset(&kernel, 0, sizeof(kernel));
	kernel.data_offset = pds_offset + slot;
	kernel.code_offset = pds_offset + slot + PDS_CODE;
	kernel.data_words = POWERVR_COMPUTE_PDS_DATA_WORDS;
	kernel.pds_temporaries = PDS_INPUT_TEMPORARIES;
	kernel.usc_inputs = inputs;
	kernel.instances = POWERVR_COMPUTE_INSTANCES;
	kernel.max_instances = 0;
	kernel.shared_from_usc = POWERVR_COMPUTE_FENCE_KERNEL ? 1U : 0U;
	kernel.any_cluster = 1;
	error = powervr_compute_kernel_words(&kernel, words);
	if (error != 0)
		return error;

	/* The shared register allocation kernel when the fence is used, then the kernel. */
	stream = STREAM_FIRST + kick_index * STREAM_SPACING;
	error = put_shared_kernel(state, &stream);
	if (error != 0)
		return error;
	for (i = 0; i < POWERVR_COMPUTE_KERNEL_WORDS; i++)
		put_word(state->streams, stream + i * 4U, words[i]);

	/* The fence kernel when it is used (POWERVR_COMPUTE_FENCE_KERNEL), then the end of the stream. */
	stream += POWERVR_COMPUTE_KERNEL_WORDS * 4U;
	error = put_fence_kernel(state, &stream);
	if (error != 0)
		return error;
	put_word(state->streams, stream, powervr_compute_terminate_word());

	/* Succeeded: the kick's program and stream are written. */
	return 0;
}

/*
 * Writes the probe programs and the two short store programs into the
 * probes' room, with a kick each; the field is counted in twos, as the
 * store kicks' is.
 */
static int
write_probes(
	struct check_state *state,
	uint8_t *code)
{
	size_t length;
	unsigned field;
	unsigned kind;
	int error;

	/* The probes' field: the wide one. */
	error = powervr_compute_task_field(POWERVR_COMPUTE_PROBE_TEMPORARIES, &field);
	if (error != 0)
		return error;

	/* Each probe: its program in its place, and its kick. */
	for (kind = 0; kind < POWERVR_COMPUTE_PROBES; kind++) {
		error = powervr_compute_probe_program(kind, run_address + PROBE_OFFSET + kind * PROBE_SPACING, code, USC_PROBE_SPACING, &length);
		if (error != 0)
			return error;
		put_bytes(state->usc, USC_PROBE_ROOM + kind * USC_PROBE_SPACING, code, length);
		error = write_kick(state, KICK_PROBE_FIRST + kind, USC_PROBE_ROOM + kind * USC_PROBE_SPACING, 0, field);
		if (error != 0)
			return error;
	}

	/* The store program of one step and of four, each over its own window. */

	error = powervr_compute_store_program(run_address + STEPS_ONE_OFFSET, 1, code, USC_PROBE_SPACING, &length);
	if (error != 0)
		return error;

	put_bytes(state->usc, USC_STEPS_ONE, code, length);
	error = powervr_compute_task_field(POWERVR_COMPUTE_STORE_TEMPORARIES, &field);
	if (error != 0)
		return error;

	error = write_kick(state, KICK_STEPS_ONE, USC_STEPS_ONE, 0, field);
	if (error != 0)
		return error;

	error = powervr_compute_store_program(run_address + STEPS_FOUR_OFFSET, 4, code, USC_PROBE_SPACING, &length);
	if (error != 0)
		return error;

	put_bytes(state->usc, USC_STEPS_FOUR, code, length);
	error = write_kick(state, KICK_STEPS_FOUR, USC_STEPS_FOUR, 0, field);
	if (error != 0)
		return error;

	/* Succeeded: the probes are in memory. */
	return 0;
}

/*
 * Runs the probes and reports them on two lines: what the multiply-add
 * and the instance number gave (the last writer's word), which of the 32
 * words the instances' own addresses reached and the first values there,
 * and how many words the store programs of one and of four steps wrote.
 */
static void
run_probes(
	struct check_state *state)
{
	volatile uint32_t *words;
	uint32_t values[POWERVR_COMPUTE_PROBES];
	uint32_t mask;
	unsigned one;
	unsigned four;
	unsigned kind;
	unsigned word;
	int errors[POWERVR_COMPUTE_PROBES + 2U];
	int error;

	/*
	 * Each probe over words no instance writes.  The probes that make
	 * their address in place (CONSTANT's multiply-add, LIKE_STORE_IN_PLACE's
	 * 64-bit add) are not run: their wrong addresses write elsewhere in
	 * the session's memory, and a later kick's program was overwritten
	 * once (the board's lockup of 2026-10-20); they report -1.
	 */
	for (kind = 0; kind < POWERVR_COMPUTE_PROBES; kind++) {
		words = state->alias + (PROBE_OFFSET + kind * PROBE_SPACING) / 4U;
		for (word = 0; word < PROBE_SPACING / 4U; word++)
			words[word] = 0xffffffffU;
		if (kind == POWERVR_COMPUTE_PROBE_CONSTANT || kind == POWERVR_COMPUTE_PROBE_LIKE_STORE_IN_PLACE || kind == POWERVR_COMPUTE_PROBE_NOP_BETWEEN) {
			errors[kind] = -1;
			values[kind] = 0xffffffffU;
			continue;
		}
		errors[kind] = kick(state, KICK_PROBE_FIRST + kind);
		values[kind] = words[0];
		if (errors[kind] == ETIMEDOUT) {
			kern_logf("powervr: compute probe %u: timed out, its memory left to the core\n", kind);
			return;
		}
	}

	/* The address probe's words: those holding their own number. */
	mask = own_words(state, POWERVR_COMPUTE_PROBE_ADDRESS);
	words = state->alias + (PROBE_OFFSET + POWERVR_COMPUTE_PROBE_ADDRESS * PROBE_SPACING) / 4U;
	kern_logf("powervr: compute probes: madd %u (want 22, %d), instance last %u (%d), instance*4 last %u (%d), own words %08x [1]=%08x [2]=%08x [31]=%08x (%d)\n",
		  values[POWERVR_COMPUTE_PROBE_MADD],
		  errors[POWERVR_COMPUTE_PROBE_MADD],
		  values[POWERVR_COMPUTE_PROBE_INSTANCE],
		  errors[POWERVR_COMPUTE_PROBE_INSTANCE],
		  values[POWERVR_COMPUTE_PROBE_INSTANCE_MADD],
		  errors[POWERVR_COMPUTE_PROBE_INSTANCE_MADD],
		  mask,
		  words[1],
		  words[2],
		  words[31],
		  errors[POWERVR_COMPUTE_PROBE_ADDRESS]);

	/* The strided and waiting probes: which instances' words hold their number. */
	kern_logf("powervr: compute probes: own words at stride 16 %08x (%d), 64 %08x (%d), 128 %08x (%d), with a wait %08x (%d)\n",
		  own_words(state, POWERVR_COMPUTE_PROBE_STRIDE_16),
		  errors[POWERVR_COMPUTE_PROBE_STRIDE_16],
		  own_words(state, POWERVR_COMPUTE_PROBE_STRIDE_64),
		  errors[POWERVR_COMPUTE_PROBE_STRIDE_64],
		  own_words(state, POWERVR_COMPUTE_PROBE_STRIDE_128),
		  errors[POWERVR_COMPUTE_PROBE_STRIDE_128],
		  own_words(state, POWERVR_COMPUTE_PROBE_ADDRESS_WAIT),
		  errors[POWERVR_COMPUTE_PROBE_ADDRESS_WAIT]);

	/* The address's own value, a constant factor, a constant index, and a moved address. */
	kern_logf("powervr: compute probes: computed low %08x high %08x (want %08x, %d), zero madd [0]=%08x (%d), constant [0]=%08x [1]=%08x (%d), moved %08x (%d)\n",
		  probe_word(state, POWERVR_COMPUTE_PROBE_COMPUTED, 0),
		  probe_word(state, POWERVR_COMPUTE_PROBE_COMPUTED, 1),
		  (uint32_t)(run_address + PROBE_OFFSET + POWERVR_COMPUTE_PROBE_COMPUTED * PROBE_SPACING) + (POWERVR_COMPUTE_INSTANCES - 1U) * 4U,
		  errors[POWERVR_COMPUTE_PROBE_COMPUTED],
		  probe_word(state, POWERVR_COMPUTE_PROBE_ZERO_MADD, 0),
		  errors[POWERVR_COMPUTE_PROBE_ZERO_MADD],
		  probe_word(state, POWERVR_COMPUTE_PROBE_CONSTANT, 0),
		  probe_word(state, POWERVR_COMPUTE_PROBE_CONSTANT, 1),
		  errors[POWERVR_COMPUTE_PROBE_CONSTANT],
		  own_words(state, POWERVR_COMPUTE_PROBE_MOVED),
		  errors[POWERVR_COMPUTE_PROBE_MOVED]);

	/* The one-step store's way, its variants. */
	kern_logf("powervr: compute probes: like store (address) %08x (%d), number %08x (%d), in place %08x (%d), early %08x (%d)\n",
		  address_words(state, POWERVR_COMPUTE_PROBE_LIKE_STORE),
		  errors[POWERVR_COMPUTE_PROBE_LIKE_STORE],
		  own_words(state, POWERVR_COMPUTE_PROBE_LIKE_STORE_NUMBER),
		  errors[POWERVR_COMPUTE_PROBE_LIKE_STORE_NUMBER],
		  own_words(state, POWERVR_COMPUTE_PROBE_LIKE_STORE_IN_PLACE),
		  errors[POWERVR_COMPUTE_PROBE_LIKE_STORE_IN_PLACE],
		  own_words(state, POWERVR_COMPUTE_PROBE_LIKE_STORE_EARLY),
		  errors[POWERVR_COMPUTE_PROBE_LIKE_STORE_EARLY]);

	/* An addend overwritten right after the multiply-add read it, and two groups later. */
	kern_logf("powervr: compute probes: addend rewritten by the next group %08x (%d), two groups later %08x (%d)\n",
		  own_words(state, POWERVR_COMPUTE_PROBE_REWRITE),
		  errors[POWERVR_COMPUTE_PROBE_REWRITE],
		  own_words(state, POWERVR_COMPUTE_PROBE_REWRITE_LATER),
		  errors[POWERVR_COMPUTE_PROBE_REWRITE_LATER]);

	/* Two waits after each group before the store (the no-operation version runs last, run_nop_probe). */
	kern_logf("powervr: compute probes: wait between groups %08x (%d)\n",
		  own_words(state, POWERVR_COMPUTE_PROBE_WAIT_BETWEEN),
		  errors[POWERVR_COMPUTE_PROBE_WAIT_BETWEEN]);

	/* The short stores: one step (32 words) and four (128). */
	one = 0;
	four = 0;
	error = run_store_window(state, KICK_STEPS_ONE, STEPS_ONE_OFFSET, POWERVR_COMPUTE_INSTANCES, &one);
	errors[POWERVR_COMPUTE_PROBES] = error;
	if (error != ETIMEDOUT) {
		error = run_store_window(state, KICK_STEPS_FOUR, STEPS_FOUR_OFFSET, POWERVR_COMPUTE_INSTANCES * 4U, &four);
		errors[POWERVR_COMPUTE_PROBES + 1U] = error;
	} else {
		errors[POWERVR_COMPUTE_PROBES + 1U] = 0;
	}
	kern_logf("powervr: compute probes: store of 1 step %u/32 (%d), of 4 steps %u/128 (%d)\n",
		  one,
		  errors[POWERVR_COMPUTE_PROBES],
		  four,
		  errors[POWERVR_COMPUTE_PROBES + 1U]);
}

/* The unified store sizes, in registers, and the temporaries fields of the grid, each widest first. */
static const unsigned grid_unified[GRID_SIZES] = { 32, 16, 8, 0 };
static const unsigned grid_field[GRID_SIZES] = { 16, 8, 4, 2 };

/* Writes the grid's kicks of the temporaries program. */
static int
write_grid(
	struct check_state *state)
{
	unsigned unified;
	unsigned field;
	int error;

	/* One kick for each size and field. */
	for (unified = 0; unified < GRID_SIZES; unified++) {
		for (field = 0; field < GRID_SIZES; field++) {
			error = write_kick(state, KICK_GRID_FIRST + unified * GRID_SIZES + field, CHECK_ROUNDS * USC_ROOM, grid_unified[unified], grid_field[field]);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the grid is in memory. */
	return 0;
}

/*
 * Runs the grid and reports the instances whose word is wrong for each
 * unified size (a row) and field (a column), "F" from a kick that
 * failed; a failure stops the grid, as the context is lost.
 */
static void
run_grid(
	struct check_state *state)
{
	char line[160];
	unsigned wrong;
	unsigned unified;
	unsigned field;
	size_t used;
	int error;

	/* Each kick in order, its count after the row's label. */
	used = 0;
	error = 0;
	for (unified = 0; unified < GRID_SIZES && error == 0; unified++) {
		used += (size_t)kern_snprintf(line + used, sizeof(line) - used, " u%u:", grid_unified[unified]);
		for (field = 0; field < GRID_SIZES; field++) {
			wrong = 0;
			error = run_temps(state, KICK_GRID_FIRST + unified * GRID_SIZES + field, &wrong);
			if (error != 0) {
				used += (size_t)kern_snprintf(line + used, sizeof(line) - used, " f%u=F(%d)", grid_field[field], error);
				break;
			}
			used += (size_t)kern_snprintf(line + used, sizeof(line) - used, " f%u=%u", grid_field[field], wrong);
		}
	}

	/* The grid on one line: wrong instances of 32 for each unified size and field. */
	kern_logf("powervr: compute grid (wrong of 32, unified regs u, field f):%s\n", line);
}

/*
 * Compiles vkcompute's fill shader the executor's way (the buffer's
 * address inlined, the workgroup id from the coefficients) into code
 * (USC_PROBE_SPACING bytes), and its task field.
 */
static int
compile_fill(
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program,
	uint8_t *code,
	uint64_t address,
	unsigned *field)
{
	struct powervr_ir_shader *shader;
	const struct powervr_shader_request *request;
	uint32_t values[POWERVR_SHADER_REQUESTS * 4U];
	unsigned index;
	unsigned word;
	int error;

	/* What the shader asks for, then its words. */
	error = powervr_shader_read(vkcompute_fill_shader, sizeof(vkcompute_fill_shader) / 4U, POWERVR_SHADER_COMPUTE, NULL, workspace, program, &shader);
	for (index = 0; error == 0 && index < program->request_count; index++) {
		request = &program->requests[index];
		if (request->shared + request->words > POWERVR_SHADER_REQUESTS * 4U) {
			error = ENOTSUP;
			break;
		}
		for (word = 0; word < request->words; word++)
			values[request->shared + word] = request->kind == POWERVR_SHADER_REQUEST_NUM_WORKGROUPS ? 1U : 0U;
		if (request->kind == POWERVR_SHADER_REQUEST_DESCRIPTOR) {
			values[request->shared] = (uint32_t)address;
			values[request->shared + 1U] = (uint32_t)(address >> 32);
		}
	}

	/* The program compiled with them, and its field. */
	if (error == 0)
		error = powervr_shader_compile_inline(vkcompute_fill_shader, sizeof(vkcompute_fill_shader) / 4U, POWERVR_SHADER_COMPUTE, NULL, values, program->shareds, workspace, program, code, USC_PROBE_SPACING);
	if (error == 0 && program->workgroup[0] != POWERVR_COMPUTE_INSTANCES)
		error = ENOTSUP;
	if (error == 0)
		error = powervr_compute_task_field(program->temps, field);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Compiles the vkcompute fill shader (data[x] = 3x + 7) with the
 * workgroup id 0 and the buffer's address as immediates, as the Vulkan
 * executor does, and runs it as the check runs its own programs: one
 * kernel of its workgroup's instances, the check's data sequencer
 * program, the task field for its temporaries.  Reports the words that
 * hold their value, on one line.
 */
static void
run_compiled_fill(
	struct check_state *state,
	int dumps,
	unsigned groups,
	unsigned field_override,
	unsigned max_instances)
{
	struct powervr_shader_workspace *workspace;
	struct powervr_shader_program *program;
	volatile uint32_t *words;
	uint8_t *code;
	unsigned field;
	unsigned word;
	unsigned good;
	unsigned changed;
	int error;

	/* The working memory. */
	workspace = kern_malloc(powervr_shader_workspace_size());
	program = kern_malloc(sizeof(*program));
	code = kern_malloc(USC_PROBE_SPACING);
	error = 0;
	if (workspace == NULL || program == NULL || code == NULL)
		error = ENOMEM;

	/* The program compiled for the buffer at the probe's area, and its field. */
	field = 0;
	if (error == 0)
		error = compile_fill(workspace, program, code, run_address + COMPILED_OFFSET, &field);
	if (field_override != 0)
		field = field_override;

	/* Its prefixes first, each with a dump of r0 to r15 after it; a prefix the core does not finish stops the check here. */
	if (error == 0 && dumps) {
		error = run_prefixes(state, &workspace->backend);
		if (error != 0) {
			kern_free(code);
			kern_free(program);
			kern_free(workspace);
			return;
		}
	}

	if (error == 0) {
		put_bytes(state->usc, USC_COMPILED, code, program->length);
		state->max_instances = max_instances;
		error = write_workgroup_kick(state, field, groups);
		state->max_instances = 0;
	}

	/* The words and 64 KiB either side of them, each its own pattern, then the kick. */
	words = state->alias + COMPILED_OFFSET / 4U;
	for (word = 0; word < SCAN_WORDS; word++)
		state->alias[SCAN_FIRST / 4U + word] = SCAN_PATTERN(word);
	if (error == 0)
		error = kick(state, KICK_PROBE_FIRST);

	/* Every word that changed, where and to what (the first SCAN_LINES of them), and how many, when diagnosing. */
	changed = 0;
	for (word = 0; CHECK_DIAGNOSTICS && word < SCAN_WORDS; word++) {
		if (state->alias[SCAN_FIRST / 4U + word] == SCAN_PATTERN(word))
			continue;
		if (changed < SCAN_LINES) {
			kern_logf("powervr: compute probes: compiled fill wrote +%x: %08x\n",
				  SCAN_FIRST + word * 4U, state->alias[SCAN_FIRST / 4U + word]);
		}
		changed++;
	}

	/* How many changed, when diagnosing. */
	if (CHECK_DIAGNOSTICS) {
		kern_logf("powervr: compute probes: compiled fill changed %u words in +%x..+%x\n",
			  changed, SCAN_FIRST, SCAN_FIRST + SCAN_WORDS * 4U);
	}

	/* The words that hold 3x + 7. */
	good = 0;
	for (word = 0; word < groups * POWERVR_COMPUTE_INSTANCES; word++) {
		if (words[word] == word * 3U + 7U)
			good++;
	}
	kern_logf("powervr: compute probes: compiled fill %u workgroups, max instances %u: %u/%u (%d), %u bytes, %u temps, field %u, words 0 %08x 32 %08x\n",
		  groups,
		  max_instances,
		  good,
		  groups * (unsigned)POWERVR_COMPUTE_INSTANCES,
		  error,
		  program != NULL ? (unsigned)program->length : 0U,
		  program != NULL ? (unsigned)program->temps : 0U,
		  field,
		  words[0],
		  words[POWERVR_COMPUTE_INSTANCES]);

	/* The working memory goes. */
	kern_free(code);
	kern_free(program);
	kern_free(workspace);
}

/*
 * The prefix step, the registers each prefix's dump holds (r0 to r15,
 * every register the compiled fill uses), the temporaries the dump adds
 * (r16 to r26), and where the dump goes: per lane 64 bytes, 2 KiB in all.
 */
#define PREFIX_STEP		4U
#define DUMP_REGISTERS		16U
#define DUMP_TEMPORARIES	27U
#define DUMP_OFFSET		0x1c8000U
#define DUMP_LANE_BYTES		(DUMP_REGISTERS * 4U)
#define DUMP_FILLER		0xdeaddeadU

/*
 * Runs the compiled fill's first N groups followed by a dump of r0 to r15
 * of every lane (lane L's register K at DUMP_OFFSET + L * 64 + K * 4),
 * for N in steps of PREFIX_STEP up to all the groups but the end, each as
 * one kernel the check's way, and reports lanes 0, 1 and 31 on one line
 * each, to compare with the host simulator's registers after the same
 * groups.  Stops at the first that fails (the context is lost); returns
 * that error.
 */
static int
run_prefixes(
	struct check_state *state,
	const struct shader_backend *backend)
{
	static const unsigned lanes[] = { 0, 1, 31 };
	struct shader_backend *dump;
	volatile uint32_t *words;
	const volatile uint32_t *row;
	uint8_t *code;
	size_t length;
	unsigned field;
	unsigned keep;
	unsigned last;
	unsigned word;
	unsigned i;
	int error;

	/* The dump's words, the program room and copy, the field for the dump's registers too. */
	words = state->alias + DUMP_OFFSET / 4U;
	code = kern_malloc(USC_DUMP_ROOM);
	dump = kern_malloc(sizeof(*dump));
	error = 0;
	if (code == NULL || dump == NULL)
		error = ENOMEM;
	if (error == 0)
		error = powervr_compute_task_field(DUMP_TEMPORARIES, &field);
	kern_logf("powervr: compute probes: compiled fill dump: run at %08x_%08x, dump at +%x\n",
		  (uint32_t)(run_address >> 32), (uint32_t)run_address, DUMP_OFFSET);

	/* Each prefix, the last one all but the end. */
	last = backend->operation_count - 1U;
	for (keep = PREFIX_STEP; error == 0; keep += PREFIX_STEP) {
		if (keep > last)
			keep = last;
		length = 0;
		error = dump_program(backend, dump, keep, run_address + DUMP_OFFSET, code, USC_DUMP_ROOM, &length);
		if (error == 0) {
			put_bytes(state->usc, USC_COMPILED, code, length);
			error = write_workgroup_kick(state, field, 1);
		}
		for (word = 0; word < 32U * DUMP_REGISTERS; word++)
			words[word] = DUMP_FILLER;
		if (error == 0)
			error = kick(state, KICK_PROBE_FIRST);
		for (i = 0; i < sizeof(lanes) / sizeof(lanes[0]); i++) {
			row = words + lanes[i] * DUMP_REGISTERS;
			kern_logf("powervr: compute probes: dump %u/%u lane %u (%d, %u bytes):"
				  " %08x %08x %08x %08x %08x %08x %08x %08x"
				  " %08x %08x %08x %08x %08x %08x %08x %08x\n",
				  keep, last, lanes[i], error, (unsigned)length,
				  row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7],
				  row[8], row[9], row[10], row[11], row[12], row[13], row[14], row[15]);
		}
		if (keep == last)
			break;
	}

	/* The room goes. */
	kern_free(dump);
	kern_free(code);
	if (error != 0)
		return error;

	/* Succeeded: every prefix ran. */
	return 0;
}

/* Appends one operation to the dump program, or returns ENOSPC. */
static int
dump_append(
	struct shader_backend *dump,
	unsigned kind,
	unsigned destination,
	unsigned source_0_bank,
	unsigned source_0,
	unsigned source_1,
	unsigned source_2,
	uint32_t immediate)
{
	struct shader_machine_operation *operation;
	unsigned index;

	/* Room for it. */
	if (dump->operation_count >= SHADER_MACHINE_OPERATIONS)
		return ENOSPC;
	operation = &dump->operations[dump->operation_count++];
	kern_memset(operation, 0, sizeof(*operation));
	operation->kind = (uint8_t)kind;
	operation->condition = POWERVR_USC_RUN_ENABLED;
	operation->immediate = immediate;
	operation->destination[0].bank = destination != DUMP_NONE ? POWERVR_USC_BANK_TEMPORARY : 0xff;
	operation->destination[0].index = (uint16_t)(destination != DUMP_NONE ? destination : 0);
	operation->destination[1].bank = 0xff;
	for (index = 0; index < 4; index++)
		operation->source[index].bank = 0xff;
	if (source_0 != DUMP_NONE) {
		operation->source[0].bank = (uint8_t)source_0_bank;
		operation->source[0].index = (uint16_t)source_0;
	}
	if (source_1 != DUMP_NONE) {
		operation->source[1].bank = POWERVR_USC_BANK_TEMPORARY;
		operation->source[1].index = (uint16_t)source_1;
	}
	if (source_2 != DUMP_NONE) {
		operation->source[2].bank = POWERVR_USC_BANK_TEMPORARY;
		operation->source[2].index = (uint16_t)source_2;
	}
	if (kind == MACHINE_ADD64) {
		operation->destination[1].bank = POWERVR_USC_BANK_TEMPORARY;
		operation->destination[1].index = (uint16_t)(destination + 1U);
	}

	/* Succeeded: appended. */
	return 0;
}

/*
 * Builds the compiled fill's first keep groups and the dump after them
 * into a copy of its back end, and encodes it (the encoder spaces it as
 * any compiled program).  r19 the lane, r20 the lane times 64, r22 and
 * r23 the register's word for lane 0, r24 and r25 the lane's, r26 the
 * register; no group writes a register it reads.  No branch may be
 * among the kept groups.
 */
static int
dump_program(
	const struct shader_backend *backend,
	struct shader_backend *dump,
	unsigned keep,
	uint64_t address,
	uint8_t *code,
	size_t room,
	size_t *length)
{
	unsigned index;
	int error;

	/* The kept groups. */
	kern_memcpy(dump, backend, sizeof(*dump));
	for (index = 0; index < keep; index++) {
		if (backend->operations[index].kind == MACHINE_BRANCH)
			return ENOTSUP;
	}
	dump->operation_count = keep;

	/* The lane's place: r19 the lane, r21 six, r20 the lane shifted by it; r23 the high word. */
	error = dump_append(dump, MACHINE_MOVE_SPECIAL, 19, POWERVR_USC_BANK_SPECIAL, DUMP_SPECIAL_INSTANCE, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 21, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 6);
	if (error == 0)
		error = dump_append(dump, MACHINE_SHIFT, 20, POWERVR_USC_BANK_TEMPORARY, 19, 21, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 23, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, (uint32_t)(address >> 32));

	/* Each register: its word's address for the lane, the register right after it, the store, its wait. */
	for (index = 0; error == 0 && index < DUMP_REGISTERS; index++) {
		error = dump_append(dump, MACHINE_IMMEDIATE, 22, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, (uint32_t)address + index * 4U);
		if (error == 0)
			error = dump_append(dump, MACHINE_ADD64, 24, POWERVR_USC_BANK_TEMPORARY, 22, 23, 20, 0);
		if (error == 0)
			error = dump_append(dump, MACHINE_MOVE, 26, POWERVR_USC_BANK_TEMPORARY, index, DUMP_NONE, DUMP_NONE, 0);
		if (error == 0) {
			error = dump_append(dump, MACHINE_STORE, DUMP_NONE, POWERVR_USC_BANK_TEMPORARY, 26, DUMP_NONE, DUMP_NONE, 0);
			dump->operations[dump->operation_count - 1U].source[1].bank = POWERVR_USC_BANK_TEMPORARY;
			dump->operations[dump->operation_count - 1U].source[1].index = 24;
		}
		if (error == 0)
			error = dump_append(dump, MACHINE_WAIT, DUMP_NONE, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 0);
	}
	if (error == 0)
		error = dump_append(dump, MACHINE_END, DUMP_NONE, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 0);

	/* Encodes it. */
	if (error == 0)
		error = shader_backend_encode(dump, code, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * The workgroup id probe: one kernel of WG_GROUPS workgroups of 32
 * instances, its data sequencer program the one-kernel executor's
 * (powervr_compute_workgroup_program, which writes the id into the
 * common store), and a shading cluster program that reads one register,
 * keeps its low two bits as the workgroup, and stores the register to
 * WG_OFFSET + workgroup * 128 + instance * 4.  Its kick slot, stream and
 * data sequencer slot are the first probe's (each variant runs on a
 * session of its own).
 */
#define WG_GROUPS		4U
#define WG_OFFSET		0x1d8000U
#define WG_TEMPORARIES		15U
#define WG_COEFFICIENTS		3U
#define WG_PDS_INPUTS		4U
#define WG_FILLER		0xdeaddeadU

/* The last operation dump_append added. */
static struct shader_machine_operation *
dump_last(
	struct shader_backend *dump)
{
	return &dump->operations[dump->operation_count - 1U];
}

/* Builds and encodes the workgroup probe's shading cluster program, reading bank:index. */
static int
workgroup_program(
	struct shader_backend *dump,
	unsigned bank,
	unsigned index,
	uint64_t address,
	uint8_t *code,
	size_t room,
	size_t *length)
{
	int error;

	/* r0 the register, r2 its low two bits (r1 three), r4 those times 128 (r3 seven). */
	kern_memset(dump, 0, sizeof(*dump));
	error = dump_append(dump, MACHINE_MOVE, 0, bank, index, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 1, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 3);
	if (error == 0) {
		error = dump_append(dump, MACHINE_LOGIC, 2, POWERVR_USC_BANK_TEMPORARY, 0, 1, DUMP_NONE, 0);
		if (error == 0)
			dump_last(dump)->variant = 1;
	}
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 3, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 7);
	if (error == 0)
		error = dump_append(dump, MACHINE_SHIFT, 4, POWERVR_USC_BANK_TEMPORARY, 2, 3, DUMP_NONE, 0);

	/* r7 the instance times four (r5 the instance, r6 two). */
	if (error == 0)
		error = dump_append(dump, MACHINE_MOVE_SPECIAL, 5, POWERVR_USC_BANK_SPECIAL, DUMP_SPECIAL_INSTANCE, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 6, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 2);
	if (error == 0)
		error = dump_append(dump, MACHINE_SHIFT, 7, POWERVR_USC_BANK_TEMPORARY, 5, 6, DUMP_NONE, 0);

	/* r12 and r13 the word's address, r14 the register right after it, the store, its wait, the end. */
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 8, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, (uint32_t)address);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 9, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, (uint32_t)(address >> 32));
	if (error == 0)
		error = dump_append(dump, MACHINE_ADD64, 10, POWERVR_USC_BANK_TEMPORARY, 8, 9, 4, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_ADD64, 12, POWERVR_USC_BANK_TEMPORARY, 10, 11, 7, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_MOVE, 14, POWERVR_USC_BANK_TEMPORARY, 0, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0) {
		error = dump_append(dump, MACHINE_STORE, DUMP_NONE, POWERVR_USC_BANK_TEMPORARY, 14, DUMP_NONE, DUMP_NONE, 0);
		if (error == 0) {
			dump->operations[dump->operation_count - 1U].source[1].bank = POWERVR_USC_BANK_TEMPORARY;
			dump->operations[dump->operation_count - 1U].source[1].index = 12;
		}
	}
	if (error == 0)
		error = dump_append(dump, MACHINE_WAIT, DUMP_NONE, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_END, DUMP_NONE, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 0);

	/* Encodes it. */
	if (error == 0)
		error = shader_backend_encode(dump, code, room, length);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/*
 * Writes the one-kernel executor's form of a kick of the program at
 * USC_COMPILED in the first probe's slot: the workgroup data sequencer
 * program, one kernel of the given workgroups, the fence kernel and the
 * end.
 */
static int
write_workgroup_kick(
	struct check_state *state,
	unsigned field,
	unsigned groups)
{
	struct powervr_compute_kernel kernel;
	uint32_t data[POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS];
	uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX];
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	uint32_t slot;
	uint32_t stream;
	unsigned count;
	unsigned i;
	int error;

	/* The program, in the first probe's slot: its data words, then its code. */
	slot = KICK_PROBE_FIRST * PDS_SLOT;
	error = powervr_compute_workgroup_program(usc_offset + USC_COMPILED, field, data, code, &count);
	if (error != 0)
		return error;
	for (i = 0; i < POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS; i++)
		put_word(state->pds, slot + i * 4U, data[i]);
	for (i = 0; i < count; i++)
		put_word(state->pds, slot + POWERVR_COMPUTE_PDS_GROUP_CODE + i * 4U, code[i]);

	/* The kernel: the workgroups, a whole task each, their ids in the common store, the local id in the unified store, the task size the check names (0, all 32). */
	kern_memset(&kernel, 0, sizeof(kernel));
	kernel.data_offset = pds_offset + slot;
	kernel.code_offset = pds_offset + slot + POWERVR_COMPUTE_PDS_GROUP_CODE;
	kernel.data_words = POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS;
	kernel.pds_temporaries = WG_PDS_INPUTS;
	kernel.workgroups[0] = groups;
	kernel.usc_common = WG_COEFFICIENTS;
	kernel.usc_temporaries = WG_TEMPORARIES;
	kernel.instances = POWERVR_COMPUTE_INSTANCES;
	kernel.max_instances = state->max_instances;
	kernel.usc_inputs = POWERVR_COMPUTE_GROUP_USC_INPUTS;
	kernel.shared_from_usc = POWERVR_COMPUTE_FENCE_KERNEL ? 1U : 0U;
	kernel.any_cluster = 1;
	error = powervr_compute_kernel_words(&kernel, words);
	if (error != 0)
		return error;
	stream = STREAM_FIRST + KICK_PROBE_FIRST * STREAM_SPACING;
	error = put_shared_kernel(state, &stream);
	if (error != 0)
		return error;
	for (i = 0; i < POWERVR_COMPUTE_KERNEL_WORDS; i++)
		put_word(state->streams, stream + i * 4U, words[i]);
	stream += POWERVR_COMPUTE_KERNEL_WORDS * 4U;

	/* The fence kernel when it is used, and the end, as every kick's. */
	error = put_fence_kernel(state, &stream);
	if (error != 0)
		return error;
	put_word(state->streams, stream, powervr_compute_terminate_word());

	/* Succeeded: the kick is written. */
	return 0;
}

/*
 * Runs the workgroup id probe once for each place the id may be read
 * from (coefficient 0, coefficient 2, shared register 0), each on a
 * session of its own, and reports on one line each which workgroups'
 * 32 words all hold their own id (the mask, 0xf when all four do), the
 * first word of each workgroup's window, and whether a one-kernel kick
 * after it still runs.
 */
static void
run_workgroup_probe(
	struct check_state *state)
{
	static const struct {
		unsigned bank;
		unsigned index;
		const char *name;
	} places[] = {
		{ POWERVR_USC_BANK_COEFFICIENT, 0, "cf0" },
		{ POWERVR_USC_BANK_COEFFICIENT, 2, "cf2" },
		{ POWERVR_USC_BANK_SHARED, 0, "sh0" },
	};
	struct shader_backend *dump;
	volatile uint32_t *words;
	uint8_t *code;
	const char *step;
	size_t length;
	unsigned field;
	unsigned after;
	unsigned mask;
	unsigned group;
	unsigned word;
	unsigned good;
	unsigned i;
	int after_error;
	int error;

	/* The program room and the back end to build in. */
	code = kern_malloc(USC_DUMP_ROOM);
	dump = kern_malloc(sizeof(*dump));
	if (code == NULL || dump == NULL) {
		kern_free(dump);
		kern_free(code);
		kern_logf("powervr: compute probes: workgroup id: not run (%d)\n", ENOMEM);
		return;
	}

	/* Each place on a session opened for it. */
	for (i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
		teardown(state);
		step = "open";
		error = setup(state, &step);
		if (error != 0) {
			kern_logf("powervr: compute probes: workgroup id via %s: not run, %s (%d)\n", places[i].name, step, error);
			break;
		}

		/* The program, the kick, the windows filled with other values. */
		length = 0;
		error = powervr_compute_task_field(WG_TEMPORARIES, &field);
		if (error == 0)
			error = workgroup_program(dump, places[i].bank, places[i].index, run_address + WG_OFFSET, code, USC_DUMP_ROOM, &length);
		if (error == 0) {
			put_bytes(state->usc, USC_COMPILED, code, length);
			error = write_workgroup_kick(state, field, WG_GROUPS);
		}
		words = state->alias + WG_OFFSET / 4U;
		for (word = 0; word < WG_GROUPS * POWERVR_COMPUTE_INSTANCES; word++)
			words[word] = WG_FILLER;
		if (error == 0)
			error = kick(state, KICK_PROBE_FIRST);

		/* Which workgroups' words all hold their id. */
		mask = 0;
		for (group = 0; group < WG_GROUPS; group++) {
			good = 0;
			for (word = 0; word < POWERVR_COMPUTE_INSTANCES; word++)
				good += words[group * POWERVR_COMPUTE_INSTANCES + word] == group;
			if (good == POWERVR_COMPUTE_INSTANCES)
				mask |= 1U << group;
		}

		/* The one-kernel store after it: refused when the kick locked the core up. */
		after = 0;
		after_error = error;
		if (error == 0)
			after_error = run_store_window(state, KICK_STEPS_ONE, STEPS_ONE_OFFSET, POWERVR_COMPUTE_INSTANCES, &after);
		kern_logf("powervr: compute probes: workgroup id via coefficients %s %x (%d), %u bytes, first words %08x %08x %08x %08x, one kernel after %u/32 (%d)\n",
			  places[i].name, mask, error, (unsigned)length,
			  words[0], words[POWERVR_COMPUTE_INSTANCES], words[2U * POWERVR_COMPUTE_INSTANCES], words[3U * POWERVR_COMPUTE_INSTANCES],
			  after, after_error);
	}

	/* The room goes. */
	kern_free(dump);
	kern_free(code);
}

/*
 * The rule probes: whether the shading cluster rules the board seemed to
 * show while the workgroup program was not Mesa's still
 * hold with the three kernels and Mesa's program.  Each is one program
 * of groups in the order listed (no spacing added), one workgroup; its
 * words at RULE_OFFSET, lane L at +L*4.
 */
#define RULE_OFFSET		0x1e0000U
#define RULE_GROUPS		128U
#define RULE_TEMPORARIES	32U
#define RULE_SETTLE		8U
#define RULE_FILLER		0xdeaddeadU

/* The rules. */
enum rule_kind {
	RULE_ADD64_IN_PLACE = 0,
	RULE_STORE_DATA_APART,
	RULE_MADD_SOURCE_NEXT,
	RULE_MADD_SPECIAL,
	RULE_MADD_RESULT_NEXT,
	RULE_KINDS
};

/* A rule probe's program being built. */
struct rule_program {
	struct shader_machine_operation operations[RULE_GROUPS];
	unsigned count;
	int error;
};

/* Appends one operation: a temporary destination (a pair for the 64-bit add) and up to three sources. */
static struct shader_machine_operation *
rule_add(
	struct rule_program *program,
	unsigned kind,
	unsigned destination,
	unsigned source_0_bank,
	unsigned source_0,
	unsigned source_1_bank,
	unsigned source_1,
	unsigned source_2_bank,
	unsigned source_2,
	uint32_t immediate)
{
	struct shader_machine_operation *operation;
	unsigned index;

	/* Room for it. */
	if (program->count >= RULE_GROUPS) {
		program->error = ENOSPC;
		return NULL;
	}
	operation = &program->operations[program->count++];
	kern_memset(operation, 0, sizeof(*operation));
	operation->kind = (uint8_t)kind;
	operation->condition = POWERVR_USC_RUN_ENABLED;
	operation->immediate = immediate;
	operation->destination[0].bank = destination != DUMP_NONE ? POWERVR_USC_BANK_TEMPORARY : 0xff;
	operation->destination[0].index = (uint16_t)(destination != DUMP_NONE ? destination : 0);
	operation->destination[1].bank = 0xff;
	if (kind == MACHINE_ADD64) {
		operation->destination[1].bank = POWERVR_USC_BANK_TEMPORARY;
		operation->destination[1].index = (uint16_t)(destination + 1U);
	}
	for (index = 0; index < 4; index++)
		operation->source[index].bank = 0xff;
	if (source_0 != DUMP_NONE) {
		operation->source[0].bank = (uint8_t)source_0_bank;
		operation->source[0].index = (uint16_t)source_0;
	}
	if (source_1 != DUMP_NONE) {
		operation->source[1].bank = (uint8_t)source_1_bank;
		operation->source[1].index = (uint16_t)source_1;
	}
	if (source_2 != DUMP_NONE) {
		operation->source[2].bank = (uint8_t)source_2_bank;
		operation->source[2].index = (uint16_t)source_2;
	}

	/* Succeeded: the operation, for its other fields. */
	return operation;
}

/* Appends RULE_SETTLE groups that move s0 into r30: time between the set-up and the rule. */
static void
rule_settle(
	struct rule_program *program)
{
	unsigned index;

	for (index = 0; index < RULE_SETTLE; index++)
		(void)rule_add(program, MACHINE_MOVE_SPECIAL, 30, POWERVR_USC_BANK_SPECIAL, 0, 0, DUMP_NONE, 0, DUMP_NONE, 0);
}

/*
 * Builds one rule probe: r0 the lane, r12:r13 the lane's word (r16, r17
 * the base, r10 the lane times four, r11 two); the rule's groups; then,
 * settled, the result moved into r14 (right after the address pair) and
 * stored, its wait and the end.  The store-data rule stores r20 with
 * r14 holding something else.
 */
static int
rule_build(
	struct rule_program *program,
	unsigned kind,
	uint64_t address,
	uint8_t *code,
	size_t room,
	size_t *length)
{
	const unsigned T = POWERVR_USC_BANK_TEMPORARY;
	const unsigned S = POWERVR_USC_BANK_SPECIAL;
	struct powervr_usc_group *groups;
	unsigned result;
	unsigned index;
	int error;

	/* The lane, its word's address, settled. */
	kern_memset(program, 0, sizeof(*program));
	(void)rule_add(program, MACHINE_MOVE_SPECIAL, 0, S, DUMP_SPECIAL_INSTANCE, 0, DUMP_NONE, 0, DUMP_NONE, 0);
	(void)rule_add(program, MACHINE_IMMEDIATE, 11, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 2);
	(void)rule_add(program, MACHINE_IMMEDIATE, 16, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, (uint32_t)address);
	(void)rule_add(program, MACHINE_IMMEDIATE, 17, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, (uint32_t)(address >> 32));
	(void)rule_add(program, MACHINE_IMMEDIATE, 1, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 3);
	(void)rule_add(program, MACHINE_IMMEDIATE, 2, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 5);
	(void)rule_add(program, MACHINE_IMMEDIATE, 5, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0);
	(void)rule_add(program, MACHINE_IMMEDIATE, 6, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0x100);
	rule_settle(program);
	(void)rule_add(program, MACHINE_SHIFT, 10, T, 0, T, 11, 0, DUMP_NONE, 0);
	rule_settle(program);
	(void)rule_add(program, MACHINE_ADD64, 12, T, 16, T, 17, T, 10, 0);
	rule_settle(program);

	/* The rule. */
	result = 4;
	switch (kind) {
	case RULE_ADD64_IN_PLACE:
		/* r4 the lane times four, then r4:r5 = (r4, r5) + 0x100 in place: 4L + 0x100. */
		(void)rule_add(program, MACHINE_MOVE, 4, T, 10, 0, DUMP_NONE, 0, DUMP_NONE, 0);
		rule_settle(program);
		(void)rule_add(program, MACHINE_ADD64, 4, T, 4, T, 5, T, 6, 0);
		break;
	case RULE_STORE_DATA_APART:
		/* r20 0x1234 for every lane, r14 (right after the address pair) another value; the store names r20. */
		(void)rule_add(program, MACHINE_IMMEDIATE, 20, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0x1234);
		(void)rule_add(program, MACHINE_IMMEDIATE, 14, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0xbad0);
		result = 20;
		break;
	case RULE_MADD_SOURCE_NEXT:
		/* r4 = 3L + 5, its multiplier rewritten by the very next group. */
		(void)rule_add(program, MACHINE_INTEGER, 4, T, 0, T, 1, T, 2, 0);
		(void)rule_add(program, MACHINE_IMMEDIATE, 1, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 7);
		break;
	case RULE_MADD_SPECIAL:
		/* r4 = L * s1 + s5 (the constants 1 and 5): L + 5. */
		(void)rule_add(program, MACHINE_INTEGER, 4, T, 0, S, 1, S, 5, 0);
		break;
	case RULE_MADD_RESULT_NEXT:
		/* r7 = 3L + 5, read by the very next group into r4. */
		(void)rule_add(program, MACHINE_INTEGER, 7, T, 0, T, 1, T, 2, 0);
		(void)rule_add(program, MACHINE_MOVE, 4, T, 7, 0, DUMP_NONE, 0, DUMP_NONE, 0);
		break;
	default:
		return EINVAL;
	}
	rule_settle(program);

	/* The result into r14 (but for the store-data rule), the store, its wait, the end. */
	if (kind != RULE_STORE_DATA_APART) {
		(void)rule_add(program, MACHINE_MOVE, 14, T, result, 0, DUMP_NONE, 0, DUMP_NONE, 0);
		rule_settle(program);
	}
	(void)rule_add(program, MACHINE_STORE, DUMP_NONE, T, result == 4 ? 14U : result, T, 12, 0, DUMP_NONE, 0);
	(void)rule_add(program, MACHINE_WAIT, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0);
	(void)rule_add(program, MACHINE_END, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0, DUMP_NONE, 0);
	if (program->error != 0)
		return program->error;

	/* The groups as listed, then the bytes. */
	groups = kern_malloc(program->count * sizeof(*groups));
	if (groups == NULL)
		return ENOMEM;
	error = 0;
	for (index = 0; error == 0 && index < program->count; index++)
		error = shader_backend_group(&program->operations[index], 0, &groups[index]);
	if (error == 0)
		error = powervr_usc_program_encode(groups, program->count, code, room, length);
	kern_free(groups);
	if (error != 0)
		return error;

	/* Succeeded: the program. */
	return 0;
}

/* The value a rule probe's lane word holds when the rule does not bind. */
static uint32_t
rule_expected(
	unsigned kind,
	unsigned lane)
{
	switch (kind) {
	case RULE_ADD64_IN_PLACE:
		return lane * 4U + 0x100U;
	case RULE_STORE_DATA_APART:
		return 0x1234U;
	case RULE_MADD_SPECIAL:
		return lane + 5U;
	default:
		return lane * 3U + 5U;
	}
}

/*
 * Runs each rule probe as one workgroup of the three kernels and Mesa's
 * program, and reports on one line each how many lanes hold the value
 * they would if the rule did not bind, and lanes 0 and 31's words.
 */
static void
run_rule_probes(
	struct check_state *state)
{
	static const char *const names[] = {
		"64-bit add in place", "store data apart from its address", "madd source rewritten by the next group",
		"madd from special constants", "madd result read by the next group",
	};
	struct rule_program *program;
	volatile uint32_t *words;
	uint8_t *code;
	size_t length;
	unsigned field;
	unsigned kind;
	unsigned lane;
	unsigned good;
	int error;

	/* The room, the program, the field. */
	code = kern_malloc(USC_DUMP_ROOM);
	program = kern_malloc(sizeof(*program));
	error = 0;
	if (code == NULL || program == NULL)
		error = ENOMEM;
	if (error == 0)
		error = powervr_compute_task_field(RULE_TEMPORARIES, &field);
	words = state->alias + RULE_OFFSET / 4U;

	/* Each rule. */
	for (kind = 0; error == 0 && kind < RULE_KINDS; kind++) {
		length = 0;
		error = rule_build(program, kind, run_address + RULE_OFFSET, code, USC_DUMP_ROOM, &length);
		if (error == 0) {
			put_bytes(state->usc, USC_COMPILED, code, length);
			error = write_workgroup_kick(state, field, 1);
		}
		for (lane = 0; lane < POWERVR_COMPUTE_INSTANCES; lane++)
			words[lane] = RULE_FILLER;
		if (error == 0)
			error = kick(state, KICK_PROBE_FIRST);
		good = 0;
		for (lane = 0; lane < POWERVR_COMPUTE_INSTANCES; lane++)
			good += words[lane] == rule_expected(kind, lane);
		kern_logf("powervr: compute probes: rule %s: %u/32 as if it does not bind (%d), lane 0 %08x lane 31 %08x, %u bytes\n",
			  names[kind], good, error, words[0], words[POWERVR_COMPUTE_INSTANCES - 1U], (unsigned)length);
	}

	/* The room goes. */
	kern_free(program);
	kern_free(code);
}

/*
 * The compiled fill's workgroup id probe: the fill's groups up to its
 * move from the coefficients (as the compiler emits them), then the
 * hand-written workgroup probe's tail (the value's low two bits pick
 * the workgroup's window, the value is stored at the lane's word), as
 * one kernel of WG_GROUPS workgroups; it reports which windows hold
 * their workgroup's id, as the workgroup id probe does.
 */
#define CF0_OFFSET		0x1d9000U
#define CF0_TEMPORARIES		34U

static void
run_cf0_probe(
	struct check_state *state)
{
	struct powervr_shader_workspace *workspace;
	struct powervr_shader_program *program;
	struct shader_backend *dump;
	volatile uint32_t *words;
	uint8_t *code;
	uint64_t address;
	size_t length;
	unsigned field;
	unsigned keep;
	unsigned value;
	unsigned group;
	unsigned word;
	unsigned good;
	unsigned mask;
	unsigned i;
	int error;

	/* The working memory and the compiled fill. */
	workspace = kern_malloc(powervr_shader_workspace_size());
	program = kern_malloc(sizeof(*program));
	code = kern_malloc(USC_DUMP_ROOM);
	dump = kern_malloc(sizeof(*dump));
	error = 0;
	if (workspace == NULL || program == NULL || code == NULL || dump == NULL)
		error = ENOMEM;
	if (error == 0)
		error = compile_fill(workspace, program, code, run_address + COMPILED_OFFSET, &field);

	/* Its groups up to the first move from the coefficients. */
	keep = 0;
	value = 0;
	for (i = 0; error == 0 && i < workspace->backend.operation_count; i++) {
		if (workspace->backend.operations[i].source[0].bank == POWERVR_USC_BANK_COEFFICIENT) {
			keep = i + 1U;
			value = workspace->backend.operations[i].destination[0].index;
			break;
		}
	}
	if (error == 0 && keep == 0)
		error = ENOENT;

	/* The tail: r20 its low two bits (r21 three), r22 those times 128 (r23 seven), r26 the lane times four (r24 the lane, r25 two), r29:r30 and r31:r32 the word, r33 the value. */
	address = run_address + CF0_OFFSET;
	if (error == 0) {
		kern_memcpy(dump, &workspace->backend, sizeof(*dump));
		dump->operation_count = keep;
		error = dump_append(dump, MACHINE_IMMEDIATE, 21, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 3);
	}
	if (error == 0) {
		error = dump_append(dump, MACHINE_LOGIC, 20, POWERVR_USC_BANK_TEMPORARY, value, 21, DUMP_NONE, 0);
		if (error == 0)
			dump_last(dump)->variant = 1;
	}
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 23, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 7);
	if (error == 0)
		error = dump_append(dump, MACHINE_SHIFT, 22, POWERVR_USC_BANK_TEMPORARY, 20, 23, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_MOVE_SPECIAL, 24, POWERVR_USC_BANK_SPECIAL, DUMP_SPECIAL_INSTANCE, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 25, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 2);
	if (error == 0)
		error = dump_append(dump, MACHINE_SHIFT, 26, POWERVR_USC_BANK_TEMPORARY, 24, 25, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 27, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, (uint32_t)address);
	if (error == 0)
		error = dump_append(dump, MACHINE_IMMEDIATE, 28, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, (uint32_t)(address >> 32));
	if (error == 0)
		error = dump_append(dump, MACHINE_ADD64, 29, POWERVR_USC_BANK_TEMPORARY, 27, 28, 22, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_ADD64, 31, POWERVR_USC_BANK_TEMPORARY, 29, 30, 26, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_MOVE, 33, POWERVR_USC_BANK_TEMPORARY, value, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0) {
		error = dump_append(dump, MACHINE_STORE, DUMP_NONE, POWERVR_USC_BANK_TEMPORARY, 33, DUMP_NONE, DUMP_NONE, 0);
		if (error == 0) {
			dump->operations[dump->operation_count - 1U].source[1].bank = POWERVR_USC_BANK_TEMPORARY;
			dump->operations[dump->operation_count - 1U].source[1].index = 31;
		}
	}
	if (error == 0)
		error = dump_append(dump, MACHINE_WAIT, DUMP_NONE, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 0);
	if (error == 0)
		error = dump_append(dump, MACHINE_END, DUMP_NONE, 0, DUMP_NONE, DUMP_NONE, DUMP_NONE, 0);
	length = 0;
	if (error == 0)
		error = shader_backend_encode(dump, code, USC_DUMP_ROOM, &length);

	/* The kick of WG_GROUPS workgroups over windows filled with other values. */
	if (error == 0)
		error = powervr_compute_task_field(CF0_TEMPORARIES, &field);
	if (error == 0) {
		put_bytes(state->usc, USC_COMPILED, code, length);
		error = write_workgroup_kick(state, field, WG_GROUPS);
	}
	words = state->alias + CF0_OFFSET / 4U;
	for (word = 0; word < WG_GROUPS * POWERVR_COMPUTE_INSTANCES; word++)
		words[word] = WG_FILLER;
	if (error == 0)
		error = kick(state, KICK_PROBE_FIRST);

	/* Which workgroups' words all hold their id. */
	mask = 0;
	for (group = 0; group < WG_GROUPS; group++) {
		good = 0;
		for (word = 0; word < POWERVR_COMPUTE_INSTANCES; word++)
			good += words[group * POWERVR_COMPUTE_INSTANCES + word] == group;
		if (good == POWERVR_COMPUTE_INSTANCES)
			mask |= 1U << group;
	}
	kern_logf("powervr: compute probes: compiled fill's workgroup id (its first %u groups, r%u) %x (%d), %u bytes, first words %08x %08x %08x %08x\n",
		  keep, value, mask, error, (unsigned)length,
		  words[0], words[POWERVR_COMPUTE_INSTANCES], words[2U * POWERVR_COMPUTE_INSTANCES], words[3U * POWERVR_COMPUTE_INSTANCES]);

	/* The working memory goes. */
	kern_free(dump);
	kern_free(code);
	kern_free(program);
	kern_free(workspace);
}

/* Closes the check's session and opens another; returns the error that kept it from opening. */
static int
fresh_session(
	struct check_state *state)
{
	const char *step;
	int error;

	teardown(state);
	step = "open";
	error = setup(state, &step);
	if (error != 0) {
		kern_logf("powervr: compute probes: session not opened, %s (%d)\n", step, error);
		return error;
	}

	/* Succeeded: a fresh session. */
	return 0;
}

/*
 * Runs one stream of the given number of kernels of the one-step store
 * program, a fence kernel and the end, as the Vulkan executor lays a
 * dispatch out, changed by the MANY_ flags, and reports, on one line,
 * whether the window holds its words and whether a one-kernel kick after
 * it in the same session still runs (a lockup the recovery ends returns
 * the kick that caused it as finished and refuses the next).  Returns
 * the kick's error.
 */
static int
run_many_kernels(
	struct check_state *state,
	unsigned kernels,
	unsigned flags,
	const char *variant)
{
	struct powervr_compute_kernel kernel;
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	uint32_t stream;
	uint32_t slot;
	unsigned written;
	unsigned after;
	unsigned count;
	unsigned field;
	unsigned i;
	int after_error;
	int error;
	unsigned fences;

	/* Kernels of their own slots: the probes' slots issue the one-step store too. */
	error = 0;
	if ((flags & MANY_OWN_SLOTS) != 0) {
		if (kernels > POWERVR_COMPUTE_PROBES)
			return EINVAL;
		error = powervr_compute_task_field(POWERVR_COMPUTE_STORE_TEMPORARIES, &field);
		for (count = 0; error == 0 && count < kernels; count++)
			error = write_kick(state, KICK_PROBE_FIRST + count, USC_STEPS_ONE, 0, field);
	}

	/* Every kernel issues the one-step store's data sequencer program, its own or the shared one. */
	stream = STREAM_FIRST + KICK_MANY * STREAM_SPACING;
	for (count = 0; error == 0 && count < kernels; count++) {
		slot = KICK_STEPS_ONE * PDS_SLOT;
		if ((flags & MANY_OWN_SLOTS) != 0)
			slot = (KICK_PROBE_FIRST + count) * PDS_SLOT;
		kern_memset(&kernel, 0, sizeof(kernel));
		kernel.data_offset = pds_offset + slot;
		kernel.code_offset = pds_offset + slot + PDS_CODE;
		kernel.data_words = POWERVR_COMPUTE_PDS_DATA_WORDS;
		kernel.pds_temporaries = PDS_INPUT_TEMPORARIES;
		kernel.instances = POWERVR_COMPUTE_INSTANCES;
		kernel.any_cluster = 1;
		kernel.fence = (flags & MANY_ALL_FENCED) != 0 ? 1U : 0U;
		error = powervr_compute_kernel_words(&kernel, words);
		for (i = 0; error == 0 && i < POWERVR_COMPUTE_KERNEL_WORDS; i++)
			put_word(state->streams, stream + i * 4U, words[i]);
		stream += POWERVR_COMPUTE_KERNEL_WORDS * 4U;
	}

	/*
	 * The fence kernel (two with MANY_TWO_FENCES), unless left out: its
	 * own program and words, without freeing the shared registers with
	 * MANY_FENCE_KEEP_SHARED, or the one-step store's kernel with only
	 * the fence bit set with MANY_FENCE_STORE_PDS.  Then the end.
	 */
	fences = 1;
	if ((flags & MANY_TWO_FENCES) != 0)
		fences = 2;
	if ((flags & MANY_NO_FENCE) != 0)
		fences = 0;
	for (count = 0; error == 0 && count < fences; count++) {
		kern_memset(&kernel, 0, sizeof(kernel));
		kernel.data_offset = pds_offset + PDS_FENCE * PDS_SLOT;
		kernel.code_offset = pds_offset + PDS_FENCE * PDS_SLOT + PDS_CODE;
		kernel.data_words = POWERVR_COMPUTE_PDS_DATA_WORDS;
		kernel.instances = 1;
		kernel.max_instances = FENCE_INSTANCES;
		kernel.fence = 1;
		kernel.shared_from_pds = 1;
		kernel.common_shared = (flags & MANY_FENCE_KEEP_SHARED) != 0 ? 0U : 1U;
		kernel.any_cluster = 1;
		if ((flags & MANY_FENCE_STORE_PDS) != 0) {
			kernel.data_offset = pds_offset + KICK_STEPS_ONE * PDS_SLOT;
			kernel.code_offset = pds_offset + KICK_STEPS_ONE * PDS_SLOT + PDS_CODE;
			kernel.pds_temporaries = PDS_INPUT_TEMPORARIES;
			kernel.instances = POWERVR_COMPUTE_INSTANCES;
			kernel.max_instances = 0;
			kernel.shared_from_pds = 0;
			kernel.common_shared = 0;
		}
		error = powervr_compute_kernel_words(&kernel, words);
		for (i = 0; error == 0 && i < POWERVR_COMPUTE_KERNEL_WORDS; i++)
			put_word(state->streams, stream + i * 4U, words[i]);
		stream += POWERVR_COMPUTE_KERNEL_WORDS * 4U;
	}
	if (error == 0)
		put_word(state->streams, stream, powervr_compute_terminate_word());

	/* The window and the kick, with the command's resume word or without it. */
	written = 0;
	state->no_resume = (flags & MANY_NO_RESUME) != 0;
	if (error == 0)
		error = run_store_window(state, KICK_MANY, STEPS_ONE_OFFSET, POWERVR_COMPUTE_INSTANCES, &written);
	state->no_resume = 0;

	/* The one-kernel store after it: refused when the stream locked the core up. */
	after = 0;
	after_error = error;
	if (error == 0)
		after_error = run_store_window(state, KICK_STEPS_ONE, STEPS_ONE_OFFSET, POWERVR_COMPUTE_INSTANCES, &after);
	if (variant != NULL) {
		kern_logf("powervr: compute probes: fence variant %s %u/32 (%d), one kernel after %u/32 (%d)\n",
			  variant, written, error, after, after_error);
	} else {
		kern_logf("powervr: compute probes: executor layout %u kernels%s%s%s%s %u/32 (%d), %u words (%u bytes), one kernel after %u/32 (%d)\n",
			  kernels,
			  (flags & MANY_NO_FENCE) != 0 ? " no fence" : "",
			  (flags & MANY_OWN_SLOTS) != 0 ? " own slots" : "",
			  (flags & MANY_ALL_FENCED) != 0 ? " all fenced" : "",
			  (flags & MANY_NO_RESUME) != 0 ? " no resume word" : "",
			  written, error,
			  (unsigned)((stream + 4U - (STREAM_FIRST + KICK_MANY * STREAM_SPACING)) / 4U),
			  (unsigned)(stream + 4U - (STREAM_FIRST + KICK_MANY * STREAM_SPACING)),
			  after, after_error);
	}

	/* The kick's error first, then the one after it. */
	if (error != 0)
		return error;
	if (after_error != 0)
		return after_error;

	/* Succeeded: the kick and the one after it finished. */
	return 0;
}

/*
 * Runs the fence kernel's variants on 16 kernels of the one-step store
 * (the count that locks up), each on a session of its own, first in the
 * check: (a) the fence kernel as it is, which matches Mesa's
 * (pvr_compute_generate_fence and the fence data sequencer program) bit
 * for bit; (b) one that keeps the shared registers (Mesa's
 * deallocate_shareds false); (c) none (the store program already waits
 * for its data before its end); (d) the one-step store's kernel with
 * only the fence bit set; (e) two fence kernels.  Leaves a fresh
 * session, or returns the error that kept one from opening.
 */
static int
run_fence_variants(
	struct check_state *state)
{
	static const struct {
		const char *name;
		unsigned flags;
	} variants[] = {
		{ "a (as is, Mesa's bytes)", 0 },
		{ "b (keeps the shared registers)", MANY_FENCE_KEEP_SHARED },
		{ "c (none, the store waits)", MANY_NO_FENCE },
		{ "d (store kernel with the fence bit)", MANY_FENCE_STORE_PDS },
		{ "e (two fence kernels)", MANY_TWO_FENCES },
	};
	const char *step;
	unsigned i;
	int error;

	/* Each variant on a session opened for it, then one for the rest of the check. */
	for (i = 0; i <= sizeof(variants) / sizeof(variants[0]); i++) {
		teardown(state);
		step = "open";
		error = setup(state, &step);
		if (error != 0) {
			kern_logf("powervr: compute probes: fence variants: session not opened, %s (%d)\n", step, error);
			return error;
		}
		if (i < sizeof(variants) / sizeof(variants[0]))
			(void)run_many_kernels(state, FENCE_VARIANT_KERNELS, variants[i].flags, variants[i].name);
	}

	/* Succeeded: a fresh session is open. */
	return 0;
}

/*
 * Runs the executor's layouts, each in a session of its own (a lockup
 * loses the session it happens in): 1, 8, 9, 10, 12, 14 and 16 kernels,
 * then 16 with no fence kernel, with a data sequencer slot each, with
 * every kernel fenced, and with no resume word in the command.
 */
static void
run_layouts(
	struct check_state *state)
{
	static const struct {
		unsigned kernels;
		unsigned flags;
	} cases[] = {
		{ 1, 0 }, { 8, 0 }, { 9, 0 }, { 10, 0 }, { 12, 0 }, { 14, 0 }, { 16, 0 },
		{ 16, MANY_NO_FENCE }, { 16, MANY_OWN_SLOTS }, { 16, MANY_ALL_FENCED }, { 16, MANY_NO_RESUME },
	};
	const char *step;
	unsigned i;
	int error;

	/* Each case on a session opened for it. */
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		teardown(state);
		step = "open";
		error = setup(state, &step);
		if (error != 0) {
			kern_logf("powervr: compute probes: executor layout %u kernels: not run, %s (%d)\n", cases[i].kernels, step, error);
			return;
		}
		(void)run_many_kernels(state, cases[i].kernels, cases[i].flags, NULL);
	}
}

/*
 * Runs the probe with two control no-operation groups after each group
 * before its store (the compiler's spacing group), on one line; it runs
 * last, as a group the core does not take may stop it.
 */
static void
run_nop_probe(
	struct check_state *state)
{
	volatile uint32_t *words;
	uint32_t word;
	int error;

	/* Its words, then the kick. */
	words = state->alias + (PROBE_OFFSET + POWERVR_COMPUTE_PROBE_NOP_BETWEEN * PROBE_SPACING) / 4U;
	for (word = 0; word < PROBE_SPACING / 4U; word++)
		words[word] = 0xffffffffU;
	error = kick(state, KICK_PROBE_FIRST + POWERVR_COMPUTE_PROBE_NOP_BETWEEN);

	/* The instances whose word holds their number. */
	kern_logf("powervr: compute probes: nop between groups %08x (%d)\n", own_words(state, POWERVR_COMPUTE_PROBE_NOP_BETWEEN), error);
}

/* Reports, as a mask, which instances' words of a probe hold their own address's low word. */
static uint32_t
address_words(
	struct check_state *state,
	unsigned kind)
{
	volatile uint32_t *words;
	uint32_t address;
	uint32_t mask;
	uint32_t instance;

	/* Each instance's word. */
	words = state->alias + (PROBE_OFFSET + kind * PROBE_SPACING) / 4U;
	mask = 0;
	for (instance = 0; instance < POWERVR_COMPUTE_INSTANCES; instance++) {
		address = (uint32_t)(run_address + PROBE_OFFSET + kind * PROBE_SPACING + instance * 4U);
		if (words[instance] == address)
			mask |= 1U << instance;
	}

	/* Succeeded: the mask. */
	return mask;
}

/* Reads one word of a probe's area. */
static uint32_t
probe_word(
	struct check_state *state,
	unsigned kind,
	uint32_t word)
{
	uint32_t value;

	/* The word through the alias. */
	value = state->alias[(PROBE_OFFSET + kind * PROBE_SPACING) / 4U + word];

	/* Succeeded: the value. */
	return value;
}

/* Reports, as a mask, which instances' words of a probe hold their own number. */
static uint32_t
own_words(
	struct check_state *state,
	unsigned kind)
{
	volatile uint32_t *words;
	uint32_t stride;
	uint32_t mask;
	uint32_t instance;

	/* Each instance's word at its stride. */
	words = state->alias + (PROBE_OFFSET + kind * PROBE_SPACING) / 4U;
	stride = powervr_compute_probe_stride(kind) / 4U;
	mask = 0;
	for (instance = 0; instance < POWERVR_COMPUTE_INSTANCES; instance++) {
		if (words[instance * stride] == instance)
			mask |= 1U << instance;
	}

	/* Succeeded: the mask. */
	return mask;
}

/*
 * Runs a store kick over a window of words filled with other values and
 * counts, through the alias, the words that hold their own address.
 */
static int
run_store_window(
	struct check_state *state,
	unsigned kick_index,
	uint32_t offset,
	uint32_t words,
	unsigned *written)
{
	volatile uint32_t *window;
	uint32_t address;
	uint32_t word;
	int error;

	/* Every word flipped from its address. */
	window = state->alias + offset / 4U;
	for (word = 0; word < words; word++) {
		address = (uint32_t)(run_address + offset + word * 4U);
		window[word] = ~address;
	}

	/* The kick. */
	*written = 0;
	error = kick(state, kick_index);
	if (error != 0)
		return error;

	/* The words that hold their own address. */
	for (word = 0; word < words; word++) {
		address = (uint32_t)(run_address + offset + word * 4U);
		if (window[word] == address)
			(*written)++;
	}

	/* Succeeded: the count is the caller's to judge. */
	return 0;
}

/* Copies bytes into a buffer through its uncached view. */
static void
put_bytes(
	struct powervr_buffer *buffer,
	uint32_t offset,
	const uint8_t *bytes,
	size_t length)
{
	size_t i;

	/* One byte at a time; the view is uncached, so each reaches memory. */
	for (i = 0; i < length; i++)
		buffer->view[offset + i] = bytes[i];
}

/* Writes one little-endian word into a buffer through its uncached view. */
static void
put_word(
	struct powervr_buffer *buffer,
	uint32_t offset,
	uint32_t word)
{
	/* One aligned store through the view. */
	*(volatile uint32_t *)(buffer->view + offset) = word;
}

/* Sends one kick's compute command and waits, bounded, for it to finish. */
static int
kick(
	struct check_state *state,
	unsigned kick_index)
{
	uint32_t command[POWERVR_COMPUTE_COMMAND_WORDS];
	uint32_t value;
	int error;

	/* The command names the kick's stream and the shared resume area. */
	powervr_compute_command(stream_address + STREAM_FIRST + (uint64_t)kick_index * STREAM_SPACING, stream_address, command);
	if (POWERVR_COMPUTE_SWITCH_STATE && !state->no_resume)
		command[POWERVR_COMPUTE_RESUME_PDS1_WORD] = powervr_compute_resume_pds1(0);

	/* Sends it with the next timeline value. */
	mutex_lock(&state->render->core->lock);

	state->session->submitted++;
	value = state->session->submitted;
	error = powervr_session_kick_compute(state->render, state->session, value, command, POWERVR_COMPUTE_COMMAND_WORDS);

	mutex_unlock(&state->render->core->lock);
	if (error != 0)
		return error;

	/* A probe kick reads the core's state a little after the kick, before any recovery. */
	if (state->probing) {
		sched_sleep(sched_ticks() + kern_ms_to_ticks(PROBE_STATE_MS));
		probe_registers(state->render->core);
	}

	/* Waits for the timeline to reach it. */
	error = powervr_session_wait(state->session, value, CHECK_WAIT_MS);
	if (error != 0)
		return error;

	/* Succeeded: the kick finished. */
	return 0;
}

/*
 * Runs the first round's kernel over a window filled with other values
 * and counts, through the alias, the words it wrote.
 */
static int
run_kernel(
	struct check_state *state,
	unsigned *written)
{
	uint32_t word;
	int error;
	uint32_t expected;

	/*
	 * Two smaller kicks first when diagnosing, to tell which part locks
	 * the compute data master up: a stream of its end alone, then one of
	 * the fence kernel and the end.  A kick that fails stops the check
	 * there.
	 */
	if (CHECK_DIAGNOSTICS) {
		error = probe_kick(state, KICK_END_ONLY, "end only");
		if (error != 0)
			return error;
	}
	if (CHECK_LOCKUP) {
		error = probe_kick(state, KICK_FENCE_ONLY, "fence and end");
		if (error != 0)
			return error;
	}

	/* Fills the window with values the kernel never writes. */
	for (word = 0; word < CHECK_WORDS; word++)
		state->alias[word] = before(0, word);

	/* Runs the kernel. */
	error = kick(state, 0);
	if (error != 0)
		return error;

	/* Counts the words that hold their own address. */
	*written = 0;
	for (word = 0; word < CHECK_WORDS; word++) {
		expected = pattern(0, word);
		if (state->alias[word] == expected)
			(*written)++;
	}

	/* Succeeded: the count is the caller's to judge. */
	return 0;
}

/*
 * The control: loads clean lines through the cached view, writes other
 * values through the alias, and reads the cached view again.  Seeing the
 * old values shows the processor alone can be shown a stale line, so a
 * matching round later means something.
 */
static enum control_result
run_control(
	struct check_state *state)
{
	uint32_t word;
	uint32_t stale;
	uint32_t expected;

	/* Fills the first window through the alias and drops any line of it. */
	for (word = 0; word < CHECK_WORDS; word++)
		state->alias[word] = before(0, word);
	kern_dcache_invalidate_range((const void *)state->cached, CHECK_WINDOW);

	/* Loads the window into the cache, clean. */
	load_cached(state->cached, CHECK_WORDS);

	/* Writes other values through the alias, then counts the old ones the cached view still shows. */
	for (word = 0; word < CHECK_WORDS; word++)
		state->alias[word] = pattern(0, word);
	stale = 0;
	for (word = 0; word < CHECK_WORDS; word++) {
		expected = before(0, word);
		if (state->cached[word] == expected)
			stale++;
	}

	/* Drops the lines again, so the rounds start clean. */
	kern_dcache_invalidate_range((const void *)state->cached, CHECK_WINDOW);

	/* Every word stale: the control holds. */
	if (stale == CHECK_WORDS)
		return CONTROL_OK;

	/* The control did not hold. */
	return CONTROL_INCONCLUSIVE;
}

/*
 * One round: loads a window's clean lines, has a kernel write every word
 * of it, and counts what the cached view and the alias show.
 */
static void
run_round(
	struct check_state *state,
	unsigned round,
	struct check_counts *counts)
{
	const volatile uint32_t *cached;
	volatile uint32_t *alias;
	uint32_t word;
	uint32_t matched;
	uint32_t value;
	int error;
	uint32_t written_value;
	uint32_t old_value;

	/* The window of this round. */
	alias = state->alias + round * (CHECK_STRIDE / 4U);
	cached = state->cached + round * (CHECK_STRIDE / 4U);

	/* Fills it through the alias with the values from before, and drops any line of it. */
	for (word = 0; word < CHECK_WORDS; word++)
		alias[word] = before(round, word);
	kern_dcache_invalidate_range((const void *)cached, CHECK_WINDOW);

	/* Loads its lines into the cache, clean. */
	load_cached(cached, CHECK_WORDS);

	/* Has the kernel write every word; no cache of the core is flushed. */
	error = kick(state, round);
	if (error != 0) {
		counts->error = error;
		return;
	}

	/* Counts the words the cached view shows written, and those it shows stale. */
	matched = 0;
	for (word = 0; word < CHECK_WORDS; word++) {
		value = cached[word];
		written_value = pattern(round, word);
		old_value = before(round, word);
		if (value == written_value) {
			matched++;
		} else if (value == old_value) {
			counts->stale++;
		}
	}

	/* Counts the words that never reached memory, through the alias. */
	for (word = 0; word < CHECK_WORDS; word++) {
		written_value = pattern(round, word);
		if (alias[word] != written_value)
			counts->unwritten++;
	}

	/* The round matched when the cached view showed every word written. */
	if (matched == CHECK_WORDS)
		counts->rounds_matched++;
}

/*
 * Runs the control and the rounds and reports what the cached view saw
 * of the shading cluster's stores, on one line.
 */
static void
run_coherency(
	struct check_state *state)
{
	struct check_counts counts;
	enum control_result control;
	const char *verdict;
	const char *control_word;
	unsigned round;

	/* The control: the processor alone can be shown a stale line. */
	control = run_control(state);

	/* Runs the rounds, each 128 KiB further into the run. */
	counts.rounds_matched = 0;
	counts.stale = 0;
	counts.unwritten = 0;
	counts.error = 0;
	for (round = 0; round < CHECK_ROUNDS; round++) {
		run_round(state, round, &counts);
		if (counts.error != 0)
			break;
	}

	/* A round the core did not finish says nothing. */
	if (counts.error != 0) {
		kern_logf("powervr: coherency (data master writes): not finished (%d)\n", counts.error);
		return;
	}

	/* Names the outcome: writes that never reached memory say nothing about the caches. */
	if (counts.unwritten != 0U) {
		verdict = "INCONCLUSIVE, writes missing";
	} else if (counts.rounds_matched == CHECK_ROUNDS && control == CONTROL_OK) {
		verdict = "COHERENT";
	} else if (counts.rounds_matched == CHECK_ROUNDS) {
		verdict = "MATCHED WITHOUT CONTROL";
	} else {
		verdict = "NOT COHERENT";
	}

	/* The control's word. */
	if (control == CONTROL_OK) {
		control_word = "OK";
	} else {
		control_word = "INCONCLUSIVE";
	}

	/* Reports the outcome on one line; the sharing stays as it is whatever it says. */
	kern_logf("powervr: coherency (data master writes, slc bypass): %s, control %s, %u/%u rounds, %llu stale, %llu missing\n",
		  verdict,
		  control_word,
		  counts.rounds_matched,
		  (unsigned)CHECK_ROUNDS,
		  (unsigned long long)counts.stale,
		  (unsigned long long)counts.unwritten);
}

/*
 * Runs the temporaries program through one of its kicks and counts the
 * instances whose word is not their number.
 */
static int
run_temps(
	struct check_state *state,
	unsigned kick_index,
	unsigned *wrong)
{
	volatile uint32_t *words;
	uint32_t instance;
	int error;

	/* Fills the instances' words with a value no instance writes. */
	words = state->alias + TEMPS_OFFSET / 4U;
	for (instance = 0; instance < POWERVR_COMPUTE_INSTANCES; instance++)
		words[instance] = 0xffffffffU;

	/* Runs the program. */
	error = kick(state, kick_index);
	if (error != 0)
		return error;

	/* Counts the words that are not their instance's number. */
	*wrong = 0;
	for (instance = 0; instance < POWERVR_COMPUTE_INSTANCES; instance++) {
		if (words[instance] != instance)
			(*wrong)++;
	}

	/* Succeeded: the count is the caller's to judge. */
	return 0;
}

/*
 * Reports how the task issue counts temporaries, on one line.
 *
 * Counted in fours, the field asks for exactly the eight temporaries
 * the program uses; counted in twos it asks for sixteen.  If the core
 * counts in twos, the first gives each instance four, and the program
 * sees a neighbour's fill when the instances' temporaries lie next to
 * each other.  The second must always work; if it does not, the check
 * itself is broken.
 */
static void
report_temps(
	int four_error,
	unsigned four_wrong,
	int two_error,
	unsigned two_wrong)
{
	const char *reading;

	/* Reads the two outcomes: the driver's field must work; the exact one says whether the extra register is needed. */
	if (two_error != 0 || two_wrong != 0) {
		reading = "BROKEN, the driver's field failed";
	} else if (four_error != 0 || four_wrong != 0) {
		reading = "OK, one register more than used is needed";
	} else {
		reading = "OK, the exact field is enough too";
	}

	/* Reports both counts and the reading. */
	kern_logf("powervr: compute temporaries: exact field %u/%u wrong (%d), one more %u/%u wrong (%d): %s\n",
		  four_wrong,
		  (unsigned)POWERVR_COMPUTE_INSTANCES,
		  four_error,
		  two_wrong,
		  (unsigned)POWERVR_COMPUTE_INSTANCES,
		  two_error,
		  reading);
}

/* The value a kernel writes to a word of a round's window: the word's own device address. */
static uint32_t
pattern(
	unsigned round,
	uint32_t word)
{
	uint64_t address;

	/* The low half of the word's device address. */
	address = run_address + (uint64_t)round * CHECK_STRIDE + (uint64_t)word * 4U;

	/* Succeeded: the value. */
	return (uint32_t)address;
}

/* The value a word holds before the kernel writes it. */
static uint32_t
before(
	unsigned round,
	uint32_t word)
{
	/* Every bit of the written value flipped. */
	return ~pattern(round, word);
}

/* Reads every word through the cached view, so its lines are loaded clean. */
static void
load_cached(
	const volatile uint32_t *words,
	uint32_t count)
{
	uint32_t word;
	uint32_t sum;

	/* Touches each word; the sum only keeps the reads. */
	sum = 0;
	for (word = 0; word < count; word++)
		sum += words[word];
	(void)sum;
}

/* Takes a zeroed buffer for programs and maps it where the session's program heap places it; reports its offset in the heap. */
static int
take_program(
	struct check_state *state,
	uint64_t bytes,
	enum powervr_program_heap heap,
	struct powervr_buffer **result,
	uint32_t *offset)
{
	struct powervr_buffer *buffer;
	uint64_t address;
	uint64_t base;
	int error;

	/* The buffer. */
	error = powervr_buffer_create(state->render, bytes, &buffer);
	if (error != 0)
		return error;

	/* Its place in the heap. */
	error = powervr_session_program_map(state->session, heap, buffer, &address);
	if (error != 0) {
		powervr_buffer_put(state->render, buffer);
		return error;
	}

	/* Succeeded: the buffer and its offset from the heap's base. */
	base = heap == POWERVR_PROGRAM_PDS ? POWERVR_PDS_HEAP_BASE : POWERVR_USC_HEAP_BASE;
	*offset = (uint32_t)(address - base);
	*result = buffer;
	return 0;
}

/*
 * Writes the shared register allocation kernel at *stream and moves
 * *stream past it, when the streams use the fence kernel (Mesa's three
 * kernels, POWERVR_COMPUTE_FENCE_KERNEL); otherwise writes nothing.
 */
static int
put_shared_kernel(
	struct check_state *state,
	uint32_t *stream)
{
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	unsigned i;
	int error;

	/* Nothing when the streams leave the fence out. */
	if (!POWERVR_COMPUTE_FENCE_KERNEL)
		return 0;

	/* The kernel over its program in the last slot. */
	error = powervr_compute_shared_kernel_words(pds_offset + PDS_SHARED * PDS_SLOT, pds_offset + PDS_SHARED * PDS_SLOT + PDS_CODE, words);
	if (error != 0)
		return error;
	for (i = 0; i < POWERVR_COMPUTE_KERNEL_WORDS; i++)
		put_word(state->streams, *stream + i * 4U, words[i]);
	*stream += POWERVR_COMPUTE_KERNEL_WORDS * 4U;

	/* Succeeded: written, or left out. */
	return 0;
}

/*
 * Writes the fence kernel at *stream and moves *stream past it, when the
 * streams use one (POWERVR_COMPUTE_FENCE_KERNEL); otherwise writes
 * nothing, each program waiting for its stores before its end instead.
 */
static int
put_fence_kernel(
	struct check_state *state,
	uint32_t *stream)
{
	struct powervr_compute_kernel kernel;
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	unsigned i;
	int error;

	/* Nothing when the streams leave it out. */
	if (!POWERVR_COMPUTE_FENCE_KERNEL)
		return 0;

	/* The fence kernel: it waits for the work and frees the shared registers. */
	kern_memset(&kernel, 0, sizeof(kernel));
	kernel.data_offset = pds_offset + PDS_FENCE * PDS_SLOT;
	kernel.code_offset = pds_offset + PDS_FENCE * PDS_SLOT + PDS_CODE;
	kernel.data_words = POWERVR_COMPUTE_PDS_DATA_WORDS;
	kernel.instances = 1;
	kernel.max_instances = FENCE_INSTANCES;
	kernel.fence = 1;
	kernel.shared_from_pds = 1;
	kernel.common_shared = 1;
	kernel.any_cluster = 1;
	error = powervr_compute_kernel_words(&kernel, words);
	if (error != 0)
		return error;
	for (i = 0; i < POWERVR_COMPUTE_KERNEL_WORDS; i++)
		put_word(state->streams, *stream + i * 4U, words[i]);
	*stream += POWERVR_COMPUTE_KERNEL_WORDS * 4U;

	/* Succeeded: written, or left out. */
	return 0;
}

/* Writes the probe kicks' streams: the end alone, and the fence kernel followed by the end. */
static int
write_probe_streams(
	struct check_state *state)
{
	struct powervr_compute_kernel kernel;
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	uint32_t stream;
	unsigned i;
	int error;

	/* The end alone. */
	stream = STREAM_FIRST + KICK_END_ONLY * STREAM_SPACING;
	put_word(state->streams, stream, powervr_compute_terminate_word());

	/* The fence kernel as the kicks have it, then the end. */
	kern_memset(&kernel, 0, sizeof(kernel));
	kernel.data_offset = pds_offset + PDS_FENCE * PDS_SLOT;
	kernel.code_offset = pds_offset + PDS_FENCE * PDS_SLOT + PDS_CODE;
	kernel.data_words = POWERVR_COMPUTE_PDS_DATA_WORDS;
	kernel.instances = 1;
	kernel.max_instances = FENCE_INSTANCES;
	kernel.fence = 1;
	kernel.shared_from_pds = 1;
	kernel.common_shared = 1;
	kernel.any_cluster = 1;
	error = powervr_compute_kernel_words(&kernel, words);
	if (error != 0)
		return error;

	stream = STREAM_FIRST + KICK_FENCE_ONLY * STREAM_SPACING;
	for (i = 0; i < POWERVR_COMPUTE_KERNEL_WORDS; i++)
		put_word(state->streams, stream + i * 4U, words[i]);
	put_word(state->streams, stream + POWERVR_COMPUTE_KERNEL_WORDS * 4U, powervr_compute_terminate_word());

	/* Succeeded: both streams are written. */
	return 0;
}

/* Runs one probe kick and reports whether it finished and whether the firmware reset the context. */
static int
probe_kick(
	struct check_state *state,
	unsigned kick_index,
	const char *what)
{
	int error;

	/* The kick, between its two lines. */
	powervr_firmware_trace_mark(state->render->core);
	kern_logf("powervr: kick: compute probe %s sent (context %s)\n", what, state->session->lost ? "reset" : "kept");
	state->probing = 1;
	error = kick(state, kick_index);
	state->probing = 0;
	kern_logf("powervr: compute probe: %s: %s (%d), context %s\n", what, error == 0 ? "finished" : "FAILED", error,
		  state->session->lost ? "reset" : "kept");
	if (error == 0 && state->session->lost)
		error = EIO;
	if (error != 0) {
		powervr_firmware_dump(state->render->core, POWERVR_KERNEL_CCB_SLOTS);
		probe_report(state, kick_index);
		return error;
	}

	/* Succeeded: the probe's kick finished with the context kept. */
	return 0;
}

/*
 * Prints what the compute data master reads for a kick: the stream's and
 * the resume area's page entries next to their buffers' pages, the
 * programs' entries, the context record's fields and its switch state.
 */
static void
probe_report(
	struct check_state *state,
	unsigned kick_index)
{
	struct powervr_session *session;
	uint64_t stream;
	unsigned index;

	/* The stream, the resume area and the programs, as the session's page table maps them. */
	session = state->session;
	stream = stream_address + STREAM_FIRST + (uint64_t)kick_index * STREAM_SPACING;
	kern_logf("powervr: probe: stream 0x%llx entry 0x%llx, resume 0x%llx entry 0x%llx, buffer page 0x%llx; pds 0x%llx entry 0x%llx (page 0x%llx); usc 0x%llx entry 0x%llx (page 0x%llx); catalogue 0x%llx\n",
		  (unsigned long long)stream, (unsigned long long)powervr_gpu_mmu_lookup(&session->mmu, stream),
		  (unsigned long long)stream_address, (unsigned long long)powervr_gpu_mmu_lookup(&session->mmu, stream_address),
		  (unsigned long long)state->streams->run.paddr,
		  (unsigned long long)(POWERVR_PDS_HEAP_BASE + pds_offset), (unsigned long long)powervr_gpu_mmu_lookup(&session->mmu, POWERVR_PDS_HEAP_BASE + pds_offset),
		  (unsigned long long)state->pds->run.paddr,
		  (unsigned long long)(POWERVR_USC_HEAP_BASE + usc_offset), (unsigned long long)powervr_gpu_mmu_lookup(&session->mmu, POWERVR_USC_HEAP_BASE + usc_offset),
		  (unsigned long long)state->usc->run.paddr,
		  (unsigned long long)powervr_gpu_mmu_root(&session->mmu));

	/* The context record's common fields. */
	kern_logf("powervr: probe: context 0x%x: ccb control 0x%x ccb 0x%x state 0x%x flags 0x%x priority %u dm %u deadline %u memory 0x%x id 0x%x\n",
		  (unsigned)powervr_object_address(&session->context, 0),
		  (unsigned)powervr_object_get32(&session->context, 0U),
		  (unsigned)powervr_object_get32(&session->context, 4U),
		  (unsigned)powervr_object_get32(&session->context, 24U),
		  (unsigned)powervr_object_get32(&session->context, 28U),
		  (unsigned)powervr_object_get32(&session->context, 32U),
		  (unsigned)powervr_object_get32(&session->context, 60U),
		  (unsigned)powervr_object_get32(&session->context, 104U),
		  (unsigned)powervr_object_get32(&session->context, 136U),
		  (unsigned)powervr_object_get32(&session->context, 140U));

	/* The switch state, seven 64-bit words after the common part. */
	for (index = 0; index < 7U; index++) {
		kern_logf("powervr: probe: switch state %u: 0x%08x%08x\n", index,
			  (unsigned)powervr_object_get32(&session->context, 152U + index * 8U + 4U),
			  (unsigned)powervr_object_get32(&session->context, 152U + index * 8U));
	}
}

/*
 * Prints the core's registers that say whether the compute data master
 * and the bus's MMU are stuck, read while a probe kick runs.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/rgx_cr_defs_km.h
 * (RGX_CR_CORE_ID 0x0018, RGX_CR_CORE_REVISION 0x0020 (the packed BVNC
 * on a core with PBVNC_COREID_REG), RGX_CR_TIMER 0x0160, read first and the
 * timer again last, to show the reads are the core's own;
 * RGX_CR_EVENT_STATUS 0x0130, RGX_CR_SIDEKICK_IDLE 0x03C8,
 * RGX_CR_CDM_CONTEXT_STORE_STATUS 0x04A0, RGX_CR_CDM_CONTEXT_PDS0 0x04A8,
 * RGX_CR_CDM_CONTEXT_PDS1 0x04B0, RGX_CR_CDM_TERMINATE_PDS 0x04B8,
 * RGX_CR_CDM_CONTEXT_LOAD_PDS0 0x04D8, RGX_CR_BIF_CAT_BASE0 0x1200,
 * RGX_CR_BIF_CAT_BASE1 to 7 0x1208 to 0x1238, RGX_CR_BIF_CAT_BASE_INDEX
 * 0x1240,
 * RGX_CR_BIF_MMU_ENTRY_STATUS 0x1288, RGX_CR_BIF_MMU_ENTRY 0x1290,
 * RGX_CR_BIF_FAULT_BANK0_MMU_STATUS 0x12B0, ..._REQ_STATUS 0x12B8,
 * RGX_CR_BIF_FAULT_BANK1_MMU_STATUS 0x12C0, ..._REQ_STATUS 0x12C8,
 * RGX_CR_BIF_MMU_STATUS 0x12D0, RGX_CR_BIF_READS_EXT_STATUS 0x1320,
 * RGX_CR_BIF_READS_INT_STATUS 0x1328, RGX_CR_BIF_STATUS_MMU 0x1358,
 * RGX_CR_FWCORE_MEM_CAT_BASE0 0x30F0, RGX_CR_FWCORE_MEM_CAT_BASE1 0x30F8,
 * RGX_CR_SLC_STATUS1 0x3870, RGX_CR_SLC_IDLE 0x3898, RGX_CR_JONES_IDLE
 * 0x8328, RGX_CR_MMU_CBASE_MAPPING_CONTEXT 0xE140, RGX_CR_MMU_CBASE_MAPPING
 * 0xE148, RGX_CR_MMU_FAULT_STATUS 0xE150).
 */
static void
probe_registers(
	struct powervr_core *core)
{
	static const uint32_t offsets[PROBE_REGISTERS] = {
		0x0018U, 0x0020U, 0x0160U, 0x0130U, 0x03c8U, 0x04a0U, 0x04a8U, 0x04b0U,
		0x04b8U, 0x04d8U, 0x1200U, 0x1208U, 0x1210U, 0x1218U, 0x1220U, 0x1228U,
		0x1230U, 0x1238U, 0x1240U, 0x1288U, 0x1290U, 0x12b0U, 0x12b8U, 0x12c0U,
		0x12c8U, 0x12d0U, 0x1320U, 0x1328U, 0x1358U, 0x30f0U, 0x30f8U, 0x3870U,
		0x3898U, 0x8328U, 0xe140U, 0xe148U, 0xe150U, 0x0160U,
	};
	unsigned index;
	uint64_t values[PROBE_REGISTERS];

	/* Every register first, back to back, so the timer read last shows how long the reads took; then the lines. */
	for (index = 0; index < PROBE_REGISTERS; index++)
		values[index] = powervr_read64(core, offsets[index]);
	for (index = 0; index < PROBE_REGISTERS; index++)
		kern_logf("powervr: probe: register 0x%04x = 0x%016llx\n", (unsigned)offsets[index], (unsigned long long)values[index]);
}
