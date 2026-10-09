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
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/buffer.h"
#include "drivers/gpu/bcm2711/v3d-job.h"

/* Each progress sample gets 500 ms; a finite job occupies at most four windows. */
#define JOB_POLL_US 10U
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
static int run_cl(struct bcm2711_v3d *engine, const struct bcm2711_v3d_cl_job *job, struct bcm2711_v3d_job_result *result);
static int run_tfu(struct bcm2711_v3d *engine, const struct bcm2711_v3d_tfu_job *job, struct bcm2711_v3d_job_result *result);
static int run_compute(struct bcm2711_v3d *engine, const struct bcm2711_v3d_compute_job *job, struct bcm2711_v3d_job_result *result);
static int wait_queue(struct bcm2711_v3d *engine, const struct job_queue *queue, const struct bcm2711_v3d_cl_job *bin, struct bcm2711_v3d_job_result *result);
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

	/* The caller's cache clean must become visible before any queue consumes inputs. */
	kern_io_write_barrier();
	if (job->kind == BCM2711_V3D_JOB_CL) {
		error = run_cl(engine, &job->command.cl, result);
	} else if (job->kind == BCM2711_V3D_JOB_TFU) {
		error = run_tfu(engine, &job->command.tfu, result);
	} else {
		error = run_compute(engine, &job->command.compute, result);
	}

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
		/* The first implementation lowers packed RGB images, not chroma planes. */
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
	struct bcm2711_v3d_hardware *hardware;
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

	/* Sticky faults cannot be hidden by discarding an earlier queue's event latches. */
	if (hardware->faulted || !hardware->irq_live) {
		spin_unlock_irqrestore(&hardware->guard, enabled);
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
}

/* Submits bin then render using IRQ dependency, with no hardware semaphore packets. */
static int
run_cl(
	struct bcm2711_v3d *engine,
	const struct bcm2711_v3d_cl_job *job,
	struct bcm2711_v3d_job_result *result)
{
	const struct job_queue bin = {2, 0, 0x110, 0x118};
	const struct job_queue render = {1, 0, 0x114, 0x11c};
	unsigned long enabled;
	int error;

	/* The bin queue writes tile lists into its prevalidated pool and state allocations. */
	if (job->bin_start != 0) {
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
		kern_mmio_write32(engine->core.mapped + 0x168, job->bin_end);

		spin_unlock_irqrestore(&engine->hardware.guard, enabled);

		/* Render remains unsubmitted while bin completion or overflow is still pending. */
		error = wait_queue(engine, &bin, job, result);
		if (error != 0)
			return error;
	}

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

	/* No bin-dependent renderer is placed in a hardware FIFO before software completion. */
	kern_mmio_write32(engine->core.mapped + 0x164, job->render_start);
	result->retired = false;
	kern_mmio_write32(engine->core.mapped + 0x16c, job->render_end);

	spin_unlock_irqrestore(&engine->hardware.guard, enabled);

	/* The same caller-owned pool and overflow storage stay alive through render completion. */
	error = wait_queue(engine, &render, NULL, result);
	if (error != 0)
		return error;
	if (job->clean_output) {
		error = bcm2711_v3d_hardware_clean(engine);
		if (error != 0)
			return error;
	}

	/* Hardware frame counters supplement the actual IRQ completion evidence. */
	result->bin_counter = kern_mmio_read32(engine->core.mapped + 0x134);
	result->render_counter = kern_mmio_read32(engine->core.mapped + 0x138);
	result->retired = true;

	/* Succeeded: both queues and any requested output cache clean finished. */
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

	/* This private packed-color lowering has no chroma or separate U-plane address. */
	kern_mmio_write32(engine->hub.mapped + 0x40c, job->input);
	kern_mmio_write32(engine->hub.mapped + 0x414, job->stride);
	kern_mmio_write32(engine->hub.mapped + 0x410, 0);
	kern_mmio_write32(engine->hub.mapped + 0x418, 0);
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
	error = wait_queue(engine, &queue, NULL, result);
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
	error = wait_queue(engine, &queue, NULL, result);
	if (error != 0)
		return error;
	error = bcm2711_v3d_hardware_clean(engine);
	if (error != 0)
		return error;
	result->retired = true;

	/* Succeeded: actual compute completion and both required GPU drain phases finished. */
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
		/* Consumes real events without acquiring an IRQ guard around the wait. */
		for (attempt = 0; attempt <= JOB_POLL_STEPS; attempt++) {
			bcm2711_v3d_hardware_events(engine, &events);
			result->core_events |= events.core;
			result->hub_events |= events.hub;
			if (events.error != 0) {
				result->fault_client = events.fault_client;
				result->fault_address = events.fault_address;
				return events.error;
			}

			/* Completion outranks a simultaneous early OOM warning from the retiring binner. */
			if ((events.core & queue->core_event) != 0 || (events.hub & queue->hub_event) != 0)
				return 0;
			if (bin != NULL && (events.core & 4U) != 0) {
				error = feed_overflow(engine, bin, result);
				if (error != 0)
					return error;
			}

			/* The final sample is at the bound, with no additional unobserved delay. */
			if (attempt == JOB_POLL_STEPS)
				break;
			kern_usleep_range(JOB_POLL_US, JOB_POLL_US);
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
