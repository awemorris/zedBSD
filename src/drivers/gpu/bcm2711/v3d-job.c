/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* IRQ completion, not register submission or a CPU delay, retires native work. */
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/v3d-job.h"

/* Real counter time bounds each 500 ms window; polling count is a fallback guard. */
#define JOB_POLL_STEPS 50000U
#define JOB_PROGRESS_WINDOWS 4U

/* A single native queue's completion source and optional progress registers. */
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
static void counters_prepare(struct bcm2711_v3d *engine);
static void counters_select(struct bcm2711_v3d *engine, const uint8_t *sources);
static void counters_read(struct bcm2711_v3d *engine, uint32_t *values);
static int run_cl(struct bcm2711_v3d *engine, const struct bcm2711_v3d_cl_job *job, struct bcm2711_v3d_job_result *result);
static int run_bin(struct bcm2711_v3d *engine, const struct bcm2711_v3d_cl_job *job, struct bcm2711_v3d_job_result *result, uint32_t *kept, bool counted);
static int render_launch(struct bcm2711_v3d *engine, const struct bcm2711_v3d_cl_job *job, struct bcm2711_v3d_job_result *result, struct bcm2711_v3d_flight *flight);
static int render_wait(struct bcm2711_v3d *engine, struct bcm2711_v3d_flight *flight);
static void report_failure(struct bcm2711_v3d *engine, enum bcm2711_v3d_job_kind kind, int error, const struct bcm2711_v3d_job_result *result);
static int run_tfu(struct bcm2711_v3d *engine, const struct bcm2711_v3d_tfu_job *job, struct bcm2711_v3d_job_result *result);
static int run_compute(struct bcm2711_v3d *engine, const struct bcm2711_v3d_compute_job *job, struct bcm2711_v3d_job_result *result);
static int wait_queue(struct bcm2711_v3d *engine, const struct job_queue *queue, const struct bcm2711_v3d_cl_job *bin, struct bcm2711_v3d_job_result *result, uint32_t *kept);
static int feed_overflow(struct bcm2711_v3d *engine, const struct bcm2711_v3d_cl_job *job, struct bcm2711_v3d_job_result *result);
static int launch_admit_locked(struct bcm2711_v3d *engine);

/*
 * Runs one trusted native job while its caller retains every mapped owner.
 * Failure after any launch sets retired=false and closes native admission;
 * the caller must quarantine mappings until explicit global recovery succeeds.
 * This operation never resets behind the common GPU core's other owners.
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

	/* Requires every referenced native span to be mapped before the first launch. */
	error = validate_job(engine, job);
	if (error != 0) {
		finish_job(engine, false);
		return error;
	}

	/* The caller's cache clean must become visible before any queue consumes inputs; the job's run is timed for hw.gputelemetry. */
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

	/* Record the failed native queues before masking or reset discards their fault state. */
	if (error != 0)
		report_failure(engine, job->kind, error, result);

	/* Only success or a refusal before the first launch proves DMA retirement. */
	failed_dma = false;
	if (error != 0 && !result->retired)
		failed_dma = true;
	finish_job(engine, failed_dma);
	if (error != 0)
		return error;

	/* Succeeded: actual IRQ completion and the required GPU cache clean were observed. */
	return 0;
}

/*
 * Runs a job, launching only the render queue of a CL job: its flight
 * runs on until bcm2711_v3d_job_land, and holds the worker slot until then.
 * binned says the CL job's bin queue already ran (bcm2711_v3d_job_prebin),
 * its tile lists still in its pool.  Any other job runs whole, with the
 * flight not active.
 */
int
bcm2711_v3d_job_launch(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_job *job,
	struct bcm2711_v3d_job_result *result,
	struct bcm2711_v3d_flight *flight,
	bool binned)
{
	uint64_t started;
	bool failed_dma;
	int error;

	/* Nothing runs on yet; a job without a render queue runs whole. */
	kern_memset(flight, 0, sizeof(*flight));
	if (job->kind != BCM2711_V3D_JOB_CL) {
		error = bcm2711_v3d_job_run(engine, job, result);
		if (error != 0)
			return error;

		/* Succeeded: the job ran whole, and nothing runs on. */
		return 0;
	}

	/* Refusals before launch do not borrow any caller DMA allocation. */
	kern_memset(result, 0, sizeof(*result));
	result->retired = true;
	error = admit_job(engine);
	if (error != 0)
		return error;

	/* Requires every referenced native span to be mapped before the first launch. */
	error = validate_job(engine, job);
	if (error != 0) {
		finish_job(engine, false);
		return error;
	}

	/* The caller's cache clean is visible before the bin queue consumes inputs; the render queue is launched after it. */
	kern_io_write_barrier();
	started = bcm2711_v3d_busy_clock();
	error = 0;
	if (!binned)
		error = run_bin(engine, &job->command.cl, result, NULL, true);
	if (error == 0)
		error = render_launch(engine, &job->command.cl, result, flight);

	/* A failure ends the job here, as bcm2711_v3d_job_run would. */
	if (error != 0) {
		bcm2711_v3d_busy_add(engine, started);
		report_failure(engine, job->kind, error, result);
		failed_dma = false;
		if (!result->retired)
			failed_dma = true;
		finish_job(engine, failed_dma);
		return error;
	}

	/* Succeeded: the render queue runs on and the slot stays held. */
	flight->started = started;
	flight->active = true;
	return 0;
}

/*
 * Runs the next CL job's bin queue while the flight's render queue runs.
 *
 * The bin reads only what its own lists, pool and state and the caller's
 * prepared inputs name, none of which the running render writes.  A
 * refusal before launch (result->retired true) leaves the flight as it was.
 */
int
bcm2711_v3d_job_prebin(
	struct bcm2711_v3d *engine,
	struct bcm2711_v3d_flight *flight,
	const struct bcm2711_v3d_cl_job *next,
	struct bcm2711_v3d_job_result *result)
{
	struct bcm2711_v3d_job job;
	int error;

	/* Nothing launched yet for the next job. */
	kern_memset(result, 0, sizeof(*result));
	result->retired = true;
	if (!flight->active || flight->error != 0 || next->bin_start == 0)
		return ENOTSUP;

	/* The next job's spans are mapped before its bin launches. */
	job.kind = BCM2711_V3D_JOB_CL;
	job.command.cl = *next;
	error = validate_job(engine, &job);
	if (error != 0)
		return error;

	/* The bin queue alone, without its counters (the render's are counting); a failure leaves the render uncertain too. */
	kern_io_write_barrier();
	error = run_bin(engine, next, result, flight->kept, false);
	if (error != 0) {
		if (!result->retired)
			flight->error = error;
		return error;
	}

	/* Succeeded: the bin queue finished; its tile lists wait for the job's render. */
	result->retired = true;
	return 0;
}

/*
 * Waits for the flight's render queue, cleans the output when asked, and
 * ends the job, releasing the worker slot.
 */
int
bcm2711_v3d_job_land(
	struct bcm2711_v3d *engine,
	struct bcm2711_v3d_flight *flight)
{
	struct bcm2711_v3d_job_result *result;
	bool failed_dma;
	int error;

	/* A flight that does not run on was ended by its launch. */
	if (!flight->active)
		return 0;
	flight->active = false;

	/* The render queue's completion, unless a bin launched beside it failed. */
	result = flight->result;
	error = flight->error;
	if (error == 0) {
		error = render_wait(engine, flight);
	} else {
		result->retired = false;
	}

	/* The job's time counts in the engine's busy time; a failure is recorded before masking. */
	bcm2711_v3d_busy_add(engine, flight->started);
	if (error != 0)
		report_failure(engine, BCM2711_V3D_JOB_CL, error, result);

	/* Only success proves DMA retirement. */
	failed_dma = false;
	if (error != 0 && !result->retired)
		failed_dma = true;
	finish_job(engine, failed_dma);
	if (error != 0)
		return error;

	/* Succeeded: the render completed and its output was cleaned as asked. */
	return 0;
}

/* Records the failed native queues before masking or reset discards their fault state. */
static void
report_failure(
	struct bcm2711_v3d *engine,
	enum bcm2711_v3d_job_kind kind,
	int error,
	const struct bcm2711_v3d_job_result *result)
{
	/* The failure, then the queues' registers and status. */
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"job failed kind=%u error=%d retired=%u irq=%x/%x fault=%x:%x",
		(unsigned)kind,
		error,
		(unsigned)result->retired,
		result->core_events,
		result->hub_events,
		result->fault_client,
		result->fault_address);
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"job queues bin=%x/%x render=%x/%x counters=%u/%u",
		kern_mmio_read32(engine->core.mapped + 0x110),
		kern_mmio_read32(engine->core.mapped + 0x108),
		kern_mmio_read32(engine->core.mapped + 0x114),
		kern_mmio_read32(engine->core.mapped + 0x10c),
		kern_mmio_read32(engine->core.mapped + 0x134),
		kern_mmio_read32(engine->core.mapped + 0x138));
	bcm2711_stage_mark(
		BCM2711_FAMILY_V3D,
		"job status bin=%x render=%x pipeline=%x",
		kern_mmio_read32(engine->core.mapped + 0x100),
		kern_mmio_read32(engine->core.mapped + 0x104),
		kern_mmio_read32(engine->core.mapped + 0x130));

	/* Succeeded: the failure is in the log. */
	return;
}

/* Checks private register fields and all directly described GPU address spans. */
static int
validate_job(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_job *job)
{
	const struct bcm2711_v3d_cl_job *cl;
	const struct bcm2711_v3d_tfu_job *tfu;
	const struct bcm2711_v3d_compute_job *compute;
	uint32_t index;
	int error;

	/* CL control consumes ordered byte endpoints, not a count inferred from submission. */
	if (job->kind == BCM2711_V3D_JOB_CL) {
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

			/* PTB pool and tile state flags require disjoint address alignment bits. */
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
		if (tfu->output_format < 3U || tfu->output_format > 7U)
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
		/* QPU execution requires nonempty direct workgroups and mapped code/uniforms. */
		compute = &job->command.compute;
		for (index = 0; index < 3; index++) {
			if ((compute->configuration[index] >> 16) == 0)
				return EINVAL;
		}

		/* The compiler supplies eight-byte QPU instructions and uniform storage. */
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

	/* Succeeded: the trusted lowering's direct spans exist in the installed table. */
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
	limit = 1ULL << (engine->hardware.physical_bits - 12U);

	/* No implicit scratch redirect may substitute for missing caller storage. */
	for (index = first; index <= last; index++) {
		entry = table[index];
		if ((entry & 0xf0000000U) != 0x30000000U)
			return EFAULT;
		if ((entry & 0x00ffffffU) >= limit)
			return EFAULT;
	}

	/* Succeeded: every page is an ordinary mapped writable 4 KiB physical page. */
	return 0;
}

/* Reserves the sole worker slot and discards stale queue completions under the IRQ guard. */
static int
admit_job(
	struct bcm2711_v3d *engine)
{
	static bool admit_reported;
	struct bcm2711_v3d_hardware *hardware;
	struct bcm2711_v3d_events events;
	unsigned long enabled;

	/* Failed boot and reset refuse native access before a guard or table is assumed. */
	hardware = &engine->hardware;
	if (!hardware->initialized || !hardware->ready)
		return ENODEV;
	enabled = spin_lock_irqsave(&hardware->guard);

	if (hardware->job_busy) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
		return EBUSY;
	}

	/* Sticky faults cannot be hidden by discarding an earlier queue's event latches; the first refusal says which fault it was. */
	if (hardware->faulted || !hardware->irq_live) {
		events = hardware->events;
		spin_unlock_irqrestore(&hardware->guard, enabled);
		if (!admit_reported) {
			admit_reported = true;
			bcm2711_stage_mark(
				BCM2711_FAMILY_V3D,
				"job refused faulted=%u irq_live=%u events=%x/%x fault=%x:%x error=%d",
				(unsigned)hardware->faulted,
				(unsigned)hardware->irq_live,
				events.core,
				events.hub,
				events.fault_client,
				events.fault_address,
				events.error);
		}
		return EIO;
	}

	/* Fault sources are preserved; only old non-fault completions may be discarded. */
	kern_mmio_write32(engine->core.mapped + 0x58, 0x87);
	kern_mmio_write32(engine->hub.mapped + 0x58, 2);
	kern_memset(&hardware->events, 0, sizeof(hardware->events));
	hardware->job_busy = true;

	spin_unlock_irqrestore(&hardware->guard, enabled);

	/* Succeeded: no other caller can publish any native queue until worker release. */
	return 0;
}

/* Releases the worker slot while preserving every failed launched job's DMA quarantine. */
static void
finish_job(
	struct bcm2711_v3d *engine,
	bool failed_dma)
{
	unsigned long enabled;

	/* A failed launched queue remains stopped until an explicit global reset proves idle. */
	if (failed_dma)
		bcm2711_v3d_hardware_mask(engine);
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	engine->hardware.job_busy = false;
	if (failed_dma)
		engine->hardware.faulted = true;

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Succeeded: the native slot no longer belongs to this completed runner call. */
	return;
}

/*
 * The counter sources of a bin run (cycles, QPU idle, coordinate shading,
 * valid instructions, waits for coordinate shading, instruction and uniform
 * cache hits and misses, VDW and VCD stalls, bin-thread cycles, L2T hits and
 * misses, primitives binned, reversed primitives discarded) and of a render
 * run (counters_prepare's).  Values referenced: Mesa
 * src/broadcom/common/v3d_performance_counters.h (the V3D 4.2 counter
 * numbers).
 */
static const uint8_t job_bin_sources[BCM2711_V3D_COUNTERS] = {
	32, 13, 14, 16, 33, 20, 21, 22, 23, 26, 27, 28, 30, 31, 35, 12
};
static const uint8_t job_render_sources[BCM2711_V3D_COUNTERS] = {
	32, 13, 15, 16, 17, 18, 19, 34, 24, 25, 54, 29, 3, 9, 84, 82
};

/* The bin and render queues' completion events and progress registers. */
static const struct job_queue job_bin_queue = {2, 0, 0x110, 0x118};
static const struct job_queue job_render_queue = {1, 0, 0x114, 0x11c};

/* Submits bin then render using IRQ dependency, with no hardware semaphore packets. */
static int
run_cl(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_cl_job *job,
	struct bcm2711_v3d_job_result *result)
{
	struct bcm2711_v3d_flight flight;
	int error;

	/* The bin queue, then the render queue launched and waited for. */
	kern_memset(&flight, 0, sizeof(flight));
	error = run_bin(engine, job, result, NULL, true);
	if (error != 0)
		return error;
	error = render_launch(engine, job, result, &flight);
	if (error != 0)
		return error;
	error = render_wait(engine, &flight);
	if (error != 0)
		return error;

	/* Succeeded: both queues and any requested output cache clean finished. */
	return 0;
}

/*
 * Runs a CL job's bin queue (none when it has no bin list) and waits for it.
 *
 * kept carries the completion events of another queue that runs meanwhile
 * (NULL when none does); counted selects and reads the bin counters, which
 * only a bin that runs alone may do.
 */
static int
run_bin(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_cl_job *job,
	struct bcm2711_v3d_job_result *result,
	uint32_t *kept,
	bool counted)
{
	uint32_t before[BCM2711_V3D_COUNTERS];
	uint32_t after[BCM2711_V3D_COUNTERS];
	uint32_t index;
	unsigned long enabled;
	uint64_t step;
	int error;

	/* A job without a bin list renders alone. */
	if (job->bin_start == 0)
		return 0;

	/* The bin queue writes tile lists into its prevalidated pool and state allocations. */
	error = bcm2711_v3d_hardware_invalidate(engine);
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	/* Fault delivery and command launch share a single admission boundary. */
	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* The end write starts execution; all earlier writes define this job only. */
	kern_mmio_write32(engine->core.mapped + 0x308, 0);
	kern_mmio_write32(engine->core.mapped + 0x30c, 0);
	kern_mmio_write32(engine->core.mapped + 0x170, job->pool_address);
	kern_mmio_write32(engine->core.mapped + 0x174, job->pool_bytes);
	kern_mmio_write32(engine->core.mapped + 0x15c, job->state_address | 2U);
	kern_mmio_write32(engine->core.mapped + 0x160, job->bin_start);
	result->retired = false;
	if (counted) {
		counters_prepare(engine);
		counters_select(engine, job_bin_sources);
		counters_read(engine, before);
	}
	kern_mmio_write32(engine->core.mapped + 0x168, job->bin_end);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Render remains unsubmitted while bin completion or overflow is still pending. */
	step = clock_ticks();
	error = wait_queue(engine, &job_bin_queue, job, result, kept);
	result->bin_ticks = clock_ticks() - step;
	if (error != 0)
		return error;

	/* The bin run's counters, when it ran alone. */
	if (counted) {
		counters_read(engine, after);
		for (index = 0; index < BCM2711_V3D_COUNTERS; index++)
			result->bin_counters[index] = after[index] - before[index];
	}

	/* Succeeded: the tile lists are written. */
	return 0;
}

/* Launches a CL job's render queue after its bin completed; the flight then waits for it. */
static int
render_launch(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_cl_job *job,
	struct bcm2711_v3d_job_result *result,
	struct bcm2711_v3d_flight *flight)
{
	unsigned long enabled;
	int error;

	/* The render cache sees bin-produced tile lists only after its IRQ fence. */
	error = bcm2711_v3d_hardware_invalidate(engine);
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* The counters before the run, as the render queue alone runs from here. */
	counters_prepare(engine);
	if (job->bin_start != 0)
		counters_select(engine, job_render_sources);
	counters_read(engine, flight->before);

	/* No bin-dependent renderer is placed in a hardware FIFO before software completion. */
	kern_mmio_write32(engine->core.mapped + 0x164, job->render_start);
	result->retired = false;
	kern_mmio_write32(engine->core.mapped + 0x16c, job->render_end);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Succeeded: the render queue runs; the flight knows its job. */
	flight->job = job;
	flight->result = result;
	flight->render_started = clock_ticks();
	return 0;
}

/* Waits for a launched render queue, then cleans the output when the job asks. */
static int
render_wait(
	struct bcm2711_v3d *engine,
	struct bcm2711_v3d_flight *flight)
{
	const struct bcm2711_v3d_cl_job *job;
	struct bcm2711_v3d_job_result *result;
	uint32_t after[BCM2711_V3D_COUNTERS];
	uint32_t index;
	uint64_t render_ticks;
	uint64_t clean_ticks;
	uint64_t step;
	int error;

	/* The same caller-owned pool and overflow storage stay alive through render completion. */
	job = flight->job;
	result = flight->result;
	error = wait_queue(engine, &job_render_queue, NULL, result, flight->kept);
	render_ticks = clock_ticks() - flight->render_started;
	if (error != 0)
		return error;
	counters_read(engine, after);
	for (index = 0; index < BCM2711_V3D_COUNTERS; index++)
		result->counters[index] = after[index] - flight->before[index];
	clean_ticks = 0;
	if (job->clean_output) {
		step = clock_ticks();
		error = bcm2711_v3d_hardware_clean(engine);
		clean_ticks = clock_ticks() - step;
		if (error != 0)
			return error;
	}

	/* Hardware frame counters supplement the actual IRQ completion evidence. */
	result->bin_counter = kern_mmio_read32(engine->core.mapped + 0x134);
	result->render_counter = kern_mmio_read32(engine->core.mapped + 0x138);
	result->retired = true;

	/* The submission cost report totals queue waits, which include scheduler delay as well as GPU busy time. */
	result->render_ticks = render_ticks;
	result->clean_ticks = clean_ticks;

	/* Succeeded: the render queue and any requested output cache clean finished. */
	return 0;
}

/* Submits packed-image TFU configuration with its launch write strictly last. */
static int
run_tfu(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_tfu_job *job,
	struct bcm2711_v3d_job_result *result)
{
	const struct job_queue queue = {0, 2, 0, 0};
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

	/* A still-busy converter is not treated as a free queue merely because software is idle. */
	status = kern_mmio_read32(engine->hub.mapped + 0x400);
	if ((status & 1U) != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return EBUSY;
	}

	/* The input planes: the first, the chroma and the separate U plane, zero when absent. */
	kern_mmio_write32(engine->hub.mapped + 0x40c, job->input);
	kern_mmio_write32(engine->hub.mapped + 0x414, job->stride);
	kern_mmio_write32(engine->hub.mapped + 0x410, job->chroma);
	kern_mmio_write32(engine->hub.mapped + 0x418, job->u_plane);
	kern_mmio_write32(engine->hub.mapped + 0x41c, job->output | (job->output_format << 3));
	kern_mmio_write32(engine->hub.mapped + 0x420, job->size);
	kern_mmio_write32(engine->hub.mapped + 0x424, job->coefficients[0]);
	if ((job->coefficients[0] & 0x80000000U) != 0) {
		/* Additional coefficients belong only to configurations that explicitly use them. */
		for (index = 1; index < 4; index++)
			kern_mmio_write32(engine->hub.mapped + 0x424 + index * 4U, job->coefficients[index]);
	}

	/* IOC requests the actual completion interrupt; this final write launches TFU. */
	result->retired = false;
	kern_mmio_write32(engine->hub.mapped + 0x408, job->configuration | 1U);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Output storage remains borrowed until actual TFU completion. */
	error = wait_queue(engine, &queue, NULL, result, NULL);
	if (error != 0)
		return error;
	result->retired = true;

	/* Succeeded: the converter's hub IRQ retired the packed-image transfer. */
	return 0;
}

/* Submits direct compute configuration and always cleans TMU output before completion. */
static int
run_compute(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_compute_job *job,
	struct bcm2711_v3d_job_result *result)
{
	const struct job_queue queue = {0x80, 0, 0x930, 0};
	unsigned long enabled;
	uint32_t index;
	int error;

	/* Compute fetches newly compiled code and uniforms through the invalidated caches. */
	error = bcm2711_v3d_hardware_invalidate(engine);
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&engine->hardware.guard);

	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* first configuration word launches execution only after all remaining configuration words are installed. */
	for (index = 1; index < 7; index++)
		kern_mmio_write32(engine->core.mapped + 0x904 + index * 4U, job->configuration[index]);
	result->retired = false;
	kern_mmio_write32(engine->core.mapped + 0x904, job->configuration[0]);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* CSD IRQ alone cannot expose TMU output while a write combiner still holds it. */
	error = wait_queue(engine, &queue, NULL, result, NULL);
	if (error != 0)
		return error;
	error = bcm2711_v3d_hardware_clean(engine);
	if (error != 0)
		return error;
	result->retired = true;

	/* Succeeded: actual compute completion and both required GPU drain phases finished. */
	return 0;
}

/*
 * Waits for IRQ events and feeds preallocated overflow while sampling bounded progress.
 *
 * kept (core and hub, NULL when one queue runs) holds the completion events
 * this wait consumes for another queue that runs meanwhile, and gives this
 * wait the ones another wait consumed for it.
 */
static int
wait_queue(
	struct bcm2711_v3d *engine,
	const struct job_queue *queue,
	const struct bcm2711_v3d_cl_job *bin,
	struct bcm2711_v3d_job_result *result,
	uint32_t *kept)
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
	uint64_t completion_delay;
	uint64_t observed;
	bool timed;
	int error;

	/* No motion from launch through the first 500 ms constitutes a stalled queue. */
	previous_current = 0;
	previous_return = 0;
	if (queue->current != 0)
		previous_current = kern_mmio_read32(engine->core.mapped + queue->current);
	if (queue->return_address != 0)
		previous_return = kern_mmio_read32(engine->core.mapped + queue->return_address);

	/* A finite upper bound also stops a cyclic CL that keeps changing its current pointer. */
	for (window = 0; window < JOB_PROGRESS_WINDOWS; window++) {
		/* Sleep rounds to scheduler ticks, so a count of 10us sleeps is not elapsed time. */
		timed = kern_rtc_read_counter(&started, &frequency);
		if (!timed)
			return EIO;

		/* A missing timebase cannot grant an unbounded hardware wait. */
		if (frequency == 0)
			return EIO;

		/* Rounds half a second upward without overflowing the counter frequency. */
		window_ticks = frequency / 2U + frequency % 2U;

		/* Consumes real events without acquiring an IRQ guard around the wait. */
		for (attempt = 0; attempt <= JOB_POLL_STEPS; attempt++) {
			observed = bcm2711_v3d_hardware_event_sequence(engine);
			bcm2711_v3d_hardware_events(engine, &events);
			if (kept != NULL) {
				events.core |= kept[0];
				events.hub |= kept[1];
				kept[0] = 0;
				kept[1] = 0;
			}
			result->core_events |= events.core;
			result->hub_events |= events.hub;
			if (events.error != 0) {
				result->fault_client = events.fault_client;
				result->fault_address = events.fault_address;
				return events.error;
			}

			/* The other queue's completions wait for its own wait (an out-of-memory warning is this bin's). */
			if (kept != NULL) {
				kept[0] = events.core & ~(queue->core_event | 4U);
				kept[1] = events.hub & ~queue->hub_event;
			}

			/* Completion outranks a simultaneous early OOM warning from the retiring binner. */
			if ((events.core & queue->core_event) != 0 || (events.hub & queue->hub_event) != 0) {
				/* Measure IRQ-to-worker delay without changing the event retirement rule. */
				completion_delay = kern_ticks_to_ms(clock_ticks() - events.arrival_tick);
				if (completion_delay >= 20U) {
					kern_logf(
						"v3d: completion delay ms=%llu event=%x/%x\n",
						(unsigned long long)completion_delay,
						queue->core_event,
						queue->hub_event);
				}

				/* Succeeded: the required native event was observed. */
				return 0;
			}

			/* Feed a preallocated overflow interval only while binning remains pending. */
			if (bin != NULL && (events.core & 4U) != 0) {
				error = feed_overflow(engine, bin, result);
				if (error != 0)
					return error;
			}

			/* Measures actual elapsed time after checking events, including a final completion. */
			timed = kern_rtc_read_counter(&now, &current_frequency);
			if (!timed)
				return EIO;

			/* A changed timebase supplies no comparable deadline. */
			if (current_frequency != frequency)
				return EIO;

			/* Finishes this window even when one yield took an entire scheduler tick. */
			if (now - started >= window_ticks)
				break;

			/* The finite poll guard also stops a counter that fails to advance. */
			if (attempt == JOB_POLL_STEPS)
				break;
			bcm2711_v3d_hardware_event_wait(engine, observed);
		}

		/* TFU has no native progress register and uses a single timeout window. */
		if (queue->current == 0)
			return ETIMEDOUT;
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

	/* A bin completion racing worker wakeup needs no further PTB allocation. */
	error = launch_admit_locked(engine);
	if (error != 0) {
		spin_unlock_irqrestore(&engine->hardware.guard, enabled);
		return error;
	}

	/* A completion observed since wakeup makes the next overflow pool unnecessary. */
	if ((engine->hardware.events.core & 2U) == 0) {
		kern_mmio_write32(engine->core.mapped + 0x308, pool->address);
		kern_mmio_write32(engine->core.mapped + 0x30c, pool->bytes);
		result->overflow_used++;
	}

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* Succeeded: storage remains borrowed through the dependent render's completion. */
	return 0;
}

/* Joins IRQ fault publication with the launch write under the engine's persistent guard. */
static int
launch_admit_locked(
	struct bcm2711_v3d *engine)
{
	/* A late fault wins before any queue can borrow another DMA buffer. */
	if (engine->hardware.faulted || !engine->hardware.irq_live)
		return EIO;
	if (!engine->hardware.ready || !engine->power.ready)
		return ENODEV;

	/* Succeeded: the caller owns the job reservation and IRQ publication boundary. */
	return 0;
}

/*
 * Gives the performance counters their sources and starts them, once after each power-up or reset.
 *
 * The sources are V3D 4.2 event numbers, four 7-bit fields a register.
 */
static void
counters_prepare(
	struct bcm2711_v3d *engine)
{
	static const uint8_t sources[BCM2711_V3D_COUNTERS] = {
		32, 13, 15, 16, 17, 18, 19, 34, 24, 25, 54, 29, 3, 9, 84, 82
	};
	uint32_t index;
	uint32_t word;

	/* Already counting. */
	if (engine->hardware.counters_ready)
		return;

	/* The sources of counters 0 to 15, then enabled, cleared and without overflow. */
	for (index = 0; index < BCM2711_V3D_COUNTERS / 4U; index++) {
		word = (uint32_t)sources[index * 4U] |
		    (uint32_t)sources[index * 4U + 1U] << 8 |
		    (uint32_t)sources[index * 4U + 2U] << 16 |
		    (uint32_t)sources[index * 4U + 3U] << 24;
		kern_mmio_write32(engine->core.mapped + 0x660 + index * 4U, word);
	}
	kern_mmio_write32(engine->core.mapped + 0x650, 0xffffU);
	kern_mmio_write32(engine->core.mapped + 0x654, 0xffffU);
	kern_mmio_write32(engine->core.mapped + 0x658, 0xffffU);
	engine->hardware.counters_ready = true;
}

/* Reads the performance counters 0 to 15. */
static void
counters_read(
	struct bcm2711_v3d *engine,
	uint32_t *values)
{
	uint32_t index;

	/* Free-running 32-bit counts; their differences give a run's share. */
	for (index = 0; index < BCM2711_V3D_COUNTERS; index++)
		values[index] = kern_mmio_read32(engine->core.mapped + 0x680 + index * 4U);
}

/* Points the sixteen counters at the given sources; their free-running counts go on from where they were. */
static void
counters_select(
	struct bcm2711_v3d *engine,
	const uint8_t *sources)
{
	uint32_t index;
	uint32_t word;

	/* Four sources a register. */
	for (index = 0; index < BCM2711_V3D_COUNTERS / 4U; index++) {
		word = (uint32_t)sources[index * 4U] |
		    (uint32_t)sources[index * 4U + 1U] << 8 |
		    (uint32_t)sources[index * 4U + 2U] << 16 |
		    (uint32_t)sources[index * 4U + 3U] << 24;
		kern_mmio_write32(engine->core.mapped + 0x660 + index * 4U, word);
	}
}
