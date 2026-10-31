/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The V3D 7.1 job runner: bin and render command lists, texture formatting
 * (TFU) and compute dispatch (CSD), each retired by its completion
 * interrupt, with bounded progress checks that report a hang.
 *
 * It keeps the name and the contract of the Pi 4's v3d-job.c, which a Pi 5
 * kernel does not build.  The command-list queues are where 4.2 has them;
 * the TFU's registers moved to 0x700 and gained an output control word,
 * every coefficient is written, the CSD's queued configuration moved to
 * 0x930 with an eighth word, and compute completion is bit 6 of the core's
 * interrupts.  The performance counters are not read on 7.1 yet; a result's
 * counters stay zero.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/v3d/v3d_sched.c lines
 * 226-428 (the order of the bin, render, TFU and CSD writes),
 * v3d_regs.h lines 99-147 (TFU), 283 (compute done), 292-346 (command
 * lists) and 407-476 (CSD).
 */

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/v3d-job.h"
#include "drivers/gpu/bcm2711/v3d71-private.h"

/* Real counter time bounds each 500 ms window; polling count is a fallback guard. */
#define JOB_POLL_STEPS 50000U
#define JOB_PROGRESS_WINDOWS 4U

/* The command-list registers: status, current and return addresses, frame counters, queue. */
#define CLE_CT0CS 0x100U
#define CLE_CT1CS 0x104U
#define CLE_CT0EA 0x108U
#define CLE_CT1EA 0x10cU
#define CLE_CT0CA 0x110U
#define CLE_CT1CA 0x114U
#define CLE_CT0RA 0x118U
#define CLE_CT1RA 0x11cU
#define CLE_PCS 0x130U
#define CLE_BFC 0x134U
#define CLE_RFC 0x138U
#define CLE_CT0QTS 0x15cU
#define CLE_CT0QBA 0x160U
#define CLE_CT1QBA 0x164U
#define CLE_CT0QEA 0x168U
#define CLE_CT1QEA 0x16cU
#define CLE_CT0QMA 0x170U
#define CLE_CT0QMS 0x174U
#define PTB_BPOA 0x308U
#define PTB_BPOS 0x30cU

/* The tile state's enable bit in its queue register. */
#define CLE_TILE_STATE_ENABLE 0x2U

/* The core's interrupt clear register. */
#define CORE_INT_CLR 0x058U
#define HUB_INT_CLR 0x058U

/* The texture formatting unit's registers on 7.1, and its busy and completion-interrupt bits. */
#define TFU_CS 0x700U
#define TFU_ICFG 0x708U
#define TFU_IIA 0x70cU
#define TFU_ICA 0x710U
#define TFU_IIS 0x714U
#define TFU_IUA 0x718U
#define TFU_IOC 0x71cU
#define TFU_IOA 0x720U
#define TFU_IOS 0x724U
#define TFU_COEF0 0x728U
#define TFU_BUSY 0x1U
#define TFU_INTERRUPT_ON_COMPLETION 0x1U

/* The compute dispatcher's status and queued configuration on 7.1. */
#define CSD_STATUS 0x900U
#define CSD_QUEUED_CFG0 0x930U
#define CSD_QUEUED_CFG7 0x94cU
#define CSD_CURRENT_CFG4 0x968U

/* A single queue's completion source and optional progress registers. */
struct job_queue {
	uint32_t core_event;
	uint32_t hub_event;
	uint32_t current;
	uint32_t return_address;
};

static int validate_job(struct bcm2711_v3d *engine, const struct bcm2711_v3d_job *job);
static int validate_span(struct bcm2711_v3d *engine, uint32_t address, uint64_t bytes);
static int admit_job(struct bcm2711_v3d *engine);
static void finish_job(struct bcm2711_v3d *engine, bool failed_dma);
static int run_cl(struct bcm2711_v3d *engine, const struct bcm2711_v3d_cl_job *job, struct bcm2711_v3d_job_result *result);
static int run_tfu(struct bcm2711_v3d *engine, const struct bcm2711_v3d_tfu_job *job, struct bcm2711_v3d_job_result *result);
static int run_compute(struct bcm2711_v3d *engine, const struct bcm2711_v3d_compute_job *job, struct bcm2711_v3d_job_result *result);
static int wait_queue(struct bcm2711_v3d *engine, const struct job_queue *queue, const struct bcm2711_v3d_cl_job *bin, struct bcm2711_v3d_job_result *result);
static int feed_overflow(struct bcm2711_v3d *engine, const struct bcm2711_v3d_cl_job *job, struct bcm2711_v3d_job_result *result);
static int launch_admit_locked(struct bcm2711_v3d *engine);
static void report_failure(struct bcm2711_v3d *engine, const struct bcm2711_v3d_job *job, const struct bcm2711_v3d_job_result *result, int error);

/*
 * Runs one trusted job while its caller retains every mapped owner.
 *
 * Failure after any launch sets retired=false and closes admission; the
 * caller must quarantine mappings until an explicit global reset succeeds.
 * This operation never resets on its own.
 */
int
bcm2711_v3d_job_run(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_job *job,
	struct bcm2711_v3d_job_result *result)
{
	uint64_t started;
	bool failed_dma;
	int error;

	/* Refusals before launch do not borrow any caller DMA allocation. */
	kern_memset(result, 0, sizeof(*result));
	result->retired = true;
	error = admit_job(engine);
	if (error != 0)
		return error;

	/* Requires every referenced span to be mapped before the first launch. */
	error = validate_job(engine, job);
	if (error != 0) {
		finish_job(engine, false);
		return error;
	}

	/* The caller's cache clean must become visible before any queue consumes inputs; then the job runs, timed for hw.gputelemetry. */
	kern_io_write_barrier();
	started = bcm2711_v3d_busy_clock();
	if (job->kind == BCM2711_V3D_JOB_CL) {
		error = run_cl(engine, &job->command.cl, result);
	} else if (job->kind == BCM2711_V3D_JOB_TFU) {
		error = run_tfu(engine, &job->command.tfu, result);
	} else {
		error = run_compute(engine, &job->command.compute, result);
	}

	/* The time the job ran, ended or failed, counts in the engine's busy time. */
	bcm2711_v3d_busy_add(engine, started);

	/* Records the failed queues before masking or reset discards their state. */
	if (error != 0)
		report_failure(engine, job, result, error);

	/* Only success or a refusal before the first launch proves DMA retirement. */
	failed_dma = false;
	if (error != 0 && !result->retired)
		failed_dma = true;
	finish_job(engine, failed_dma);
	if (error != 0)
		return error;

	/* Succeeded: the completion IRQ and any required GPU cache clean were observed. */
	return 0;
}

/* Checks the register fields and all directly described GPU address spans. */
static int
validate_job(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_job *job)
{
	const struct bcm2711_v3d_cl_job *cl;
	const struct bcm2711_v3d_tfu_job *tfu;
	const struct bcm2711_v3d_compute_job *compute;
	uint32_t output_format;
	uint32_t index;
	int error;

	/* Picks the checks of the job's kind. */
	if (job->kind == BCM2711_V3D_JOB_CL) {
		/* Command lists consume ordered byte endpoints. */
		cl = &job->command.cl;
		if (cl->render_end <= cl->render_start)
			return EINVAL;
		error = validate_span(engine, cl->render_start, cl->render_end - cl->render_start);
		if (error != 0)
			return error;

		/* A missing bin queue is explicit; half-defined endpoints are not accepted. */
		if (cl->bin_start != 0 || cl->bin_end != 0) {
			if (cl->bin_end <= cl->bin_start)
				return EINVAL;
			error = validate_span(engine, cl->bin_start, cl->bin_end - cl->bin_start);
			if (error != 0)
				return error;

			/* The tile pool and tile state need their aligned addresses. */
			if ((cl->pool_address & 63U) != 0 || (cl->state_address & 255U) != 0)
				return EINVAL;
			error = validate_span(engine, cl->pool_address, cl->pool_bytes);
			if (error != 0)
				return error;
			error = validate_span(engine, cl->state_address, cl->state_bytes);
			if (error != 0)
				return error;
		}

		/* Overflow storage must already be mapped; IRQ context never allocates it. */
		if (cl->overflow_count > 4)
			return EINVAL;
		for (index = 0; index < cl->overflow_count; index++) {
			/* Each overflow pool is a mapped, page-aligned 256 KiB. */
			if ((cl->overflow[index].address & 4095U) != 0 || cl->overflow[index].bytes != 0x40000U)
				return EINVAL;
			error = validate_span(engine, cl->overflow[index].address, cl->overflow[index].bytes);
			if (error != 0)
				return error;
		}
	} else if (job->kind == BCM2711_V3D_JOB_TFU) {
		/* Every plane and the output start on 64-byte boundaries. */
		tfu = &job->command.tfu;
		if ((tfu->input & 63U) != 0 || (tfu->output & 63U) != 0)
			return EINVAL;

		/* The output control word names a tiled output format. */
		output_format = (tfu->output_format >> 12) & 7U;
		if (output_format < 3U)
			return EINVAL;
		error = validate_span(engine, tfu->input, tfu->input_bytes);
		if (error != 0)
			return error;
		error = validate_span(engine, tfu->output, tfu->output_bytes);
		if (error != 0)
			return error;

		/* A YUV input's other planes are mapped like its first one. */
		if (tfu->chroma != 0) {
			if ((tfu->chroma & 63U) != 0)
				return EINVAL;
			error = validate_span(engine, tfu->chroma, tfu->chroma_bytes);
			if (error != 0)
				return error;
		}

		/* A three-plane input's separate U plane is checked the same way. */
		if (tfu->u_plane != 0) {
			if ((tfu->u_plane & 63U) != 0)
				return EINVAL;
			error = validate_span(engine, tfu->u_plane, tfu->u_plane_bytes);
			if (error != 0)
				return error;
		}
	} else if (job->kind == BCM2711_V3D_JOB_COMPUTE) {
		/* QPU execution requires nonempty workgroups and mapped code and uniforms. */
		compute = &job->command.compute;
		for (index = 0; index < 3; index++) {
			/* Every dimension has at least one workgroup. */
			if ((compute->configuration[index] >> 16) == 0)
				return EINVAL;
		}

		/* The code is whole eight-byte instructions; both spans are mapped. */
		if ((compute->shader_bytes & 7U) != 0)
			return EINVAL;
		error = validate_span(engine, compute->configuration[5] & ~7U, compute->shader_bytes);
		if (error != 0)
			return error;
		error = validate_span(engine, compute->configuration[6], compute->uniform_bytes);
		if (error != 0)
			return error;
	} else {
		return EINVAL;
	}

	/* Succeeded: the job's direct spans exist in the installed table. */
	return 0;
}

/* Validates whole mapped spans, including their final PTE and physical reachability. */
static int
validate_span(
	struct bcm2711_v3d *engine,
	uint32_t address,
	uint64_t bytes)
{
	const uint32_t *table;
	uint64_t limit;
	uint32_t first;
	uint32_t last;
	uint32_t index;
	uint32_t entry;

	/* The zero VA page remains disabled and endpoints must stay within 4 GiB. */
	if (address < 4096U || bytes == 0)
		return EINVAL;
	if (bytes > 0x100000000ULL - address)
		return EINVAL;
	first = address >> 12;
	last = (uint32_t)(((uint64_t)address + bytes - 1) >> 12);
	table = engine->hardware.pages->address;
	limit = 1ULL << (bcm2712_v3d_of(engine)->table_physical_bits - 12U);

	/* No implicit scratch redirect may substitute for missing caller storage. */
	for (index = first; index <= last; index++) {
		/* Each page is an ordinary present, writable 4 KiB page the engine reaches. */
		entry = table[index];
		if ((entry & 0xf0000000U) != 0x30000000U)
			return EFAULT;
		if ((entry & 0x00ffffffU) >= limit)
			return EFAULT;
	}

	/* Succeeded: every page is mapped. */
	return 0;
}

/* Reserves the sole worker slot and discards stale completions under the IRQ guard. */
static int
admit_job(
	struct bcm2711_v3d *engine)
{
	struct bcm2711_v3d_hardware *hardware;
	unsigned long enabled;

	/* Failed starts and resets refuse access before a guard or table is assumed. */
	hardware = &engine->hardware;
	if (!hardware->initialized || !hardware->ready)
		return ENODEV;
	enabled = spin_lock_irqsave(&hardware->guard);

	if (hardware->job_busy) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return EBUSY;
	}

	/* Sticky faults cannot be hidden by discarding an earlier queue's latches. */
	if (hardware->faulted || !hardware->irq_live) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return EIO;
	}

	/* Only old completions are discarded: the core's are all completions, the hub's is TFU's. */
	kern_mmio_write32(engine->core.mapped + CORE_INT_CLR, BCM2712_V3D_CORE_EVENTS);
	kern_mmio_write32(engine->hub.mapped + HUB_INT_CLR, BCM2712_V3D_HUB_TFU_DONE);
	kern_memset(&hardware->events, 0, sizeof(hardware->events));
	hardware->job_busy = true;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Succeeded: no other caller can start a queue until this job finishes. */
	return 0;
}

/* Releases the worker slot while keeping a failed launched job's DMA quarantined. */
static void
finish_job(
	struct bcm2711_v3d *engine,
	bool failed_dma)
{
	unsigned long enabled;

	/* A failed launched queue stays stopped until an explicit reset proves it idle. */
	if (failed_dma)
		bcm2711_v3d_hardware_mask(engine);
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	engine->hardware.job_busy = false;
	if (failed_dma)
		engine->hardware.faulted = true;

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);
}

/* Submits bin then render using the IRQ dependency, with no hardware semaphore packets. */
static int
run_cl(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_cl_job *job,
	struct bcm2711_v3d_job_result *result)
{
	const struct job_queue bin = {BCM2712_V3D_CORE_BIN_DONE, 0, CLE_CT0CA, CLE_CT0RA};
	const struct job_queue render = {BCM2712_V3D_CORE_RENDER_DONE, 0, CLE_CT1CA, CLE_CT1RA};
	unsigned long enabled;
	uint64_t step;
	uint64_t bin_ticks;
	uint64_t render_ticks;
	uint64_t clean_ticks;
	int error;

	/* Measures queue completion separately from the output cache drain. */
	bin_ticks = 0;
	clean_ticks = 0;

	/* The bin queue writes tile lists into its validated pool and state allocations. */
	if (job->bin_start != 0) {
		error = bcm2711_v3d_hardware_invalidate(engine);
		if (error != 0)
			return error;
		enabled = spin_lock_irqsave(&engine->hardware.guard);

		/* Fault delivery and the launch share one admission boundary. */
		error = launch_admit_locked(engine);
		if (error != 0) {
			spin_unlock_irqrestore(&engine->hardware.guard, enabled);
			return error;
		}

		/* The end write starts execution; all earlier writes define this job only. */
		kern_mmio_write32(engine->core.mapped + PTB_BPOA, 0);
		kern_mmio_write32(engine->core.mapped + PTB_BPOS, 0);
		kern_mmio_write32(engine->core.mapped + CLE_CT0QMA, job->pool_address);
		kern_mmio_write32(engine->core.mapped + CLE_CT0QMS, job->pool_bytes);
		kern_mmio_write32(engine->core.mapped + CLE_CT0QTS, job->state_address | CLE_TILE_STATE_ENABLE);
		kern_mmio_write32(engine->core.mapped + CLE_CT0QBA, job->bin_start);
		result->retired = false;
		kern_mmio_write32(engine->core.mapped + CLE_CT0QEA, job->bin_end);

		spin_unlock_irqrestore(&engine->hardware.guard, enabled);

		/* Render stays unsubmitted while bin completion or overflow is pending. */
		step = clock_ticks();
		error = wait_queue(engine, &bin, job, result);
		bin_ticks = clock_ticks() - step;
		if (error != 0)
			return error;
	}

	/* The render caches see bin-produced tile lists only after its IRQ. */
	error = bcm2711_v3d_hardware_invalidate(engine);
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* Starts the render queue; its end write launches it. */
	kern_mmio_write32(engine->core.mapped + CLE_CT1QBA, job->render_start);
	result->retired = false;
	kern_mmio_write32(engine->core.mapped + CLE_CT1QEA, job->render_end);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* The caller's pool and overflow storage stay alive through render completion. */
	step = clock_ticks();
	error = wait_queue(engine, &render, NULL, result);
	render_ticks = clock_ticks() - step;
	if (error != 0)
		return error;

	/* Cleans the output out of the GPU caches when the caller asked for it. */
	if (job->clean_output) {
		step = clock_ticks();
		error = bcm2711_v3d_hardware_clean(engine);
		clean_ticks = clock_ticks() - step;
		if (error != 0)
			return error;
	}

	/* The frame counters supplement the IRQ completion evidence. */
	result->bin_counter = kern_mmio_read32(engine->core.mapped + CLE_BFC);
	result->render_counter = kern_mmio_read32(engine->core.mapped + CLE_RFC);
	result->retired = true;
	result->bin_ticks = bin_ticks;
	result->render_ticks = render_ticks;
	result->clean_ticks = clean_ticks;

	/* Succeeded: both queues and any requested output clean finished. */
	return 0;
}

/* Submits a texture formatting job with its launch write strictly last. */
static int
run_tfu(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_tfu_job *job,
	struct bcm2711_v3d_job_result *result)
{
	const struct job_queue queue = {0, BCM2712_V3D_HUB_TFU_DONE, 0, 0};
	unsigned long enabled;
	uint32_t status;
	uint32_t index;
	int error;

	/* TFU does not use the bin/render cache-invalidate sequence. */
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* A still-busy unit is not treated as free merely because software is idle. */
	status = kern_mmio_read32(engine->hub.mapped + TFU_CS);
	if ((status & TFU_BUSY) != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return EBUSY;
	}

	/* The input planes, the output, its control word, the size and every coefficient. */
	kern_mmio_write32(engine->hub.mapped + TFU_IIA, job->input);
	kern_mmio_write32(engine->hub.mapped + TFU_IIS, job->stride);
	kern_mmio_write32(engine->hub.mapped + TFU_ICA, job->chroma);
	kern_mmio_write32(engine->hub.mapped + TFU_IUA, job->u_plane);
	kern_mmio_write32(engine->hub.mapped + TFU_IOA, job->output);
	kern_mmio_write32(engine->hub.mapped + TFU_IOC, job->output_format);
	kern_mmio_write32(engine->hub.mapped + TFU_IOS, job->size);
	for (index = 0; index < 4; index++)
		kern_mmio_write32(engine->hub.mapped + TFU_COEF0 + index * 4U, job->coefficients[index]);

	/* Asks for the completion interrupt; this final write launches the unit. */
	result->retired = false;
	kern_mmio_write32(engine->hub.mapped + TFU_ICFG, job->configuration | TFU_INTERRUPT_ON_COMPLETION);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* The output stays borrowed until the unit's completion. */
	error = wait_queue(engine, &queue, NULL, result);
	if (error != 0)
		return error;
	result->retired = true;

	/* Succeeded: the hub's completion interrupt retired the conversion. */
	return 0;
}

/* Submits a compute dispatch and always cleans TMU output before completion. */
static int
run_compute(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_compute_job *job,
	struct bcm2711_v3d_job_result *result)
{
	const struct job_queue queue = {BCM2712_V3D_CORE_COMPUTE_DONE, 0, CSD_CURRENT_CFG4, 0};
	unsigned long enabled;
	uint32_t index;
	int error;

	/* Compute fetches newly written code and uniforms through the invalidated caches. */
	error = bcm2711_v3d_hardware_invalidate(engine);
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* Writes configuration words 1 to 6, keeps word 7 zero, then word 0 launches the dispatch. */
	for (index = 1; index < 7; index++)
		kern_mmio_write32(engine->core.mapped + CSD_QUEUED_CFG0 + index * 4U, job->configuration[index]);
	kern_mmio_write32(engine->core.mapped + CSD_QUEUED_CFG7, 0);
	result->retired = false;
	kern_mmio_write32(engine->core.mapped + CSD_QUEUED_CFG0, job->configuration[0]);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* The completion interrupt alone cannot expose TMU output still in a write combiner. */
	error = wait_queue(engine, &queue, NULL, result);
	if (error != 0)
		return error;
	error = bcm2711_v3d_hardware_clean(engine);
	if (error != 0)
		return error;
	result->retired = true;

	/* Succeeded: compute completion and the GPU cache drain both finished. */
	return 0;
}

/* Waits for IRQ events and feeds preallocated overflow while sampling bounded progress. */
static int
wait_queue(
	struct bcm2711_v3d *engine,
	const struct job_queue *queue,
	const struct bcm2711_v3d_cl_job *bin,
	struct bcm2711_v3d_job_result *result)
{
	struct bcm2711_v3d_events events;
	uint32_t attempt;
	uint32_t window;
	uint32_t current;
	uint32_t returned;
	uint32_t previous_current;
	uint32_t previous_return;
	uint64_t started;
	uint64_t frequency;
	uint64_t now;
	uint64_t current_frequency;
	uint64_t window_ticks;
	uint64_t observed;
	bool timed;
	int error;

	/* No motion from launch through the first 500 ms is a stalled queue. */
	previous_current = 0;
	previous_return = 0;
	if (queue->current != 0)
		previous_current = kern_mmio_read32(engine->core.mapped + queue->current);
	if (queue->return_address != 0)
		previous_return = kern_mmio_read32(engine->core.mapped + queue->return_address);

	/* A finite bound also stops a cyclic list that keeps changing its current pointer. */
	for (window = 0; window < JOB_PROGRESS_WINDOWS; window++) {
		/* A missing timebase cannot grant an unbounded hardware wait. */
		timed = kern_rtc_read_counter(&started, &frequency);
		if (!timed)
			return EIO;
		if (frequency == 0)
			return EIO;

		/* Rounds half a second upward. */
		window_ticks = frequency / 2U + frequency % 2U;

		/* Consumes real events without holding the IRQ guard around the wait. */
		for (attempt = 0; attempt <= JOB_POLL_STEPS; attempt++) {
			/* Takes the events latched since the last look. */
			observed = bcm2711_v3d_hardware_event_sequence(engine);
			bcm2711_v3d_hardware_events(engine, &events);
			result->core_events |= events.core;
			result->hub_events |= events.hub;
			if (events.error != 0) {
				result->fault_client = events.fault_client;
				result->fault_address = events.fault_address;
				return events.error;
			}

			/* Completion outranks a simultaneous out-of-memory warning from the retiring binner. */
			if ((events.core & queue->core_event) != 0 || (events.hub & queue->hub_event) != 0)
				return 0;

			/* Feeds a preallocated overflow pool only while binning remains pending. */
			if (bin != NULL && (events.core & BCM2712_V3D_CORE_OUT_OF_MEMORY) != 0) {
				error = feed_overflow(engine, bin, result);
				if (error != 0)
					return error;
			}

			/* Measures elapsed time after checking events, including a final completion. */
			timed = kern_rtc_read_counter(&now, &current_frequency);
			if (!timed)
				return EIO;
			if (current_frequency != frequency)
				return EIO;

			/* Finishes this window even when one yield took an entire scheduler tick. */
			if (now - started >= window_ticks)
				break;
			if (attempt == JOB_POLL_STEPS)
				break;

			/* Sleeps until the next IRQ or tick. */
			bcm2711_v3d_hardware_event_wait(engine, observed);
		}

		/* TFU has no progress register and uses a single timeout window. */
		if (queue->current == 0)
			return ETIMEDOUT;

		/* A queue whose position did not move in the window has hung. */
		current = kern_mmio_read32(engine->core.mapped + queue->current);
		returned = 0;
		if (queue->return_address != 0)
			returned = kern_mmio_read32(engine->core.mapped + queue->return_address);
		if (current == previous_current && returned == previous_return)
			return ETIMEDOUT;
		previous_current = current;
		previous_return = returned;
	}

	/* Exhausted progress windows supply no DMA retirement proof. */
	return ETIMEDOUT;
}

/* Gives an active binner one already translated overflow pool from worker context. */
static int
feed_overflow(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_cl_job *job,
	struct bcm2711_v3d_job_result *result)
{
	const struct bcm2711_v3d_overflow *pool;
	unsigned long enabled;
	int error;

	/* An exhausted reserve cannot be turned into ordinary bin completion. */
	if (result->overflow_used >= job->overflow_count)
		return ENOMEM;
	pool = &job->overflow[result->overflow_used];
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	/* A bin completion racing the worker's wakeup needs no further pool. */
	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* Hands the next pool to the binner unless it already finished. */
	if ((engine->hardware.events.core & BCM2712_V3D_CORE_BIN_DONE) == 0) {
		kern_mmio_write32(engine->core.mapped + PTB_BPOA, pool->address);
		kern_mmio_write32(engine->core.mapped + PTB_BPOS, pool->bytes);
		result->overflow_used++;
	}

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Succeeded: the pool stays borrowed through the dependent render's completion. */
	return 0;
}

/* Joins IRQ fault publication with the launch write under the engine's guard. */
static int
launch_admit_locked(
	struct bcm2711_v3d *engine)
{
	/* A late fault wins before any queue can borrow another DMA buffer. */
	if (engine->hardware.faulted || !engine->hardware.irq_live)
		return EIO;
	if (!engine->hardware.ready || !engine->power.ready)
		return ENODEV;

	/* Succeeded: the caller owns the job reservation and the IRQ boundary. */
	return 0;
}

/* Logs a failed job's events and the queues' registers before a reset discards them. */
static void
report_failure(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_job *job,
	const struct bcm2711_v3d_job_result *result,
	int error)
{
	/* Names the job and what its IRQs said. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"job failed kind=%u error=%d retired=%u irq=%x/%x fault=%x:%x",
		(unsigned)job->kind,
		error,
		(unsigned)result->retired,
		result->core_events,
		result->hub_events,
		result->fault_client,
		result->fault_address);

	/* Shows where each queue stood, unless the engine lost power. */
	if (!engine->power.ready)
		return;
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"job queues bin=%x/%x render=%x/%x counters=%u/%u",
		kern_mmio_read32(engine->core.mapped + CLE_CT0CA),
		kern_mmio_read32(engine->core.mapped + CLE_CT0EA),
		kern_mmio_read32(engine->core.mapped + CLE_CT1CA),
		kern_mmio_read32(engine->core.mapped + CLE_CT1EA),
		kern_mmio_read32(engine->core.mapped + CLE_BFC),
		kern_mmio_read32(engine->core.mapped + CLE_RFC));
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"job status bin=%x render=%x pipeline=%x tfu=%x csd=%x",
		kern_mmio_read32(engine->core.mapped + CLE_CT0CS),
		kern_mmio_read32(engine->core.mapped + CLE_CT1CS),
		kern_mmio_read32(engine->core.mapped + CLE_PCS),
		kern_mmio_read32(engine->hub.mapped + TFU_CS),
		kern_mmio_read32(engine->core.mapped + CSD_STATUS));
}
