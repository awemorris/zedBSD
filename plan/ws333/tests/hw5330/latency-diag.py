# ws333 p029, test only: adds latency diagnostics to the i915 tree as it is (the request worker's queue and run times, its
# rests and wakes, the waits that ran out, the RPS and the actual GT frequency every 5 s; streams over 1 s; each
# pipeline's compile time; the GPU layer's jobs that outlive their deadline; the actual GT frequency as cur_mhz in
# hw.gputelemetry).  Applied for a kernel build and taken off again (git diff > PATCH; git apply -R PATCH):
#   python3 -I plan/ws333/tests/hw5330/latency-diag.py TOP
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import sys
top = sys.argv[1]
d = top + '/src/drivers/gpu/i915/'
def edit(path, old, new, need=True):
	s = open(d + path).read()
	if old not in s:
		if need:
			raise SystemExit('make-diag: %s: anchor missing: %s' % (path, old[:60]))
		return False
	s = s.replace(old, new, 1)
	open(d + path, 'w').write(s)
	return True
holds = 'worker_awake' in open(d + 'gt.h').read()
awake = 'worker->device->gt.worker_awake' if holds else '0U'
awake2 = 'device->gt.worker_awake' if holds else '0U'
refs2 = 'device->gt.awake_refs' if holds else '0U'
globs = '''#include "intel/commands.h"
#include "gt-power.h"
#include "intel/gt-power.h"

/* Test-only diagnostics of the worker's latencies, summed per window. */
static struct {
	uint64_t window_start;
	unsigned rests;
	unsigned wakes;
	uint64_t wake_max_ns;
	uint64_t queue_max_ns;
	unsigned queue_kind;
	uint64_t item_max_ns;
	unsigned item_kind;
	unsigned items;
	unsigned slow_logged;
	unsigned wait_irq_seen;
	unsigned wait_tmo_seen;
	unsigned boosts_seen;
	unsigned up_seen;
	unsigned down_seen;
} i915_diag;
'''
funcs = '''static void
i915_diag_note(
	struct i915_worker *worker,
	unsigned kind,
	uint64_t queued_at,
	uint64_t started,
	uint64_t ended)
{
	uint64_t queue_ns;
	uint64_t item_ns;

	(void)worker;
	queue_ns = 0U;
	if (queued_at != 0U && started > queued_at)
		queue_ns = started - queued_at;
	item_ns = 0U;
	if (ended > started)
		item_ns = ended - started;
	i915_diag.items++;
	if (queue_ns > i915_diag.queue_max_ns) {
		i915_diag.queue_max_ns = queue_ns;
		i915_diag.queue_kind = kind;
	}
	if (item_ns > i915_diag.item_max_ns) {
		i915_diag.item_max_ns = item_ns;
		i915_diag.item_kind = kind;
	}
	if ((queue_ns > 200000000ULL || item_ns > 200000000ULL) && i915_diag.slow_logged < 60U) {
		i915_diag.slow_logged++;
		kern_logf("i915: diag: slow item kind=%u queued %llu ms ran %llu ms at %llu ms (awake %u)\\n",
		    kind,
		    (unsigned long long)(queue_ns / 1000000ULL),
		    (unsigned long long)(item_ns / 1000000ULL),
		    (unsigned long long)(ended / 1000000ULL),
		    AWAKE);
	}
}

static void
i915_diag_window(
	struct i915_worker *worker)
{
	struct i915_device *device;
	struct i915_rps *rps;
	uint64_t now;
	uint32_t cagf;

	device = worker->device;
	rps = &device->gt.init.rps;
	now = drv_i915_perf_now();
	if (i915_diag.window_start == 0U) {
		i915_diag.window_start = now;
		return;
	}
	if (now - i915_diag.window_start < 5000000000ULL)
		return;
	cagf = (drv_i915_raw_read32(&device->gt.mmio, I915_GT_GEN12_RPSTAT1) & I915_GT_GEN12_CAGF_MASK) >> I915_GT_GEN12_CAGF_SHIFT;
	kern_logf("i915: diag: %llu ms at %llu ms: items %u rests %u wakes %u (max %llu us) waits irq %u tmo %u | queue max %llu ms (kind %u) item max %llu ms (kind %u) | rps cur %u MHz waiters %u boosts %u up %u down %u | cagf %u MHz awake %u refs %u\\n",
	    (unsigned long long)((now - i915_diag.window_start) / 1000000ULL),
	    (unsigned long long)(now / 1000000ULL),
	    i915_diag.items,
	    i915_diag.rests,
	    i915_diag.wakes,
	    (unsigned long long)(i915_diag.wake_max_ns / 1000ULL),
	    worker->wait_interrupts - i915_diag.wait_irq_seen,
	    worker->wait_timeouts - i915_diag.wait_tmo_seen,
	    (unsigned long long)(i915_diag.queue_max_ns / 1000000ULL),
	    i915_diag.queue_kind,
	    (unsigned long long)(i915_diag.item_max_ns / 1000000ULL),
	    i915_diag.item_kind,
	    rps->cur_freq * 50U / 3U,
	    rps->waiters,
	    rps->boosts - i915_diag.boosts_seen,
	    rps->up_events - i915_diag.up_seen,
	    rps->down_events - i915_diag.down_seen,
	    cagf * 50U / 3U,
	    AWAKE2,
	    REFS2);
	i915_diag.window_start = now;
	i915_diag.items = 0U;
	i915_diag.rests = 0U;
	i915_diag.wakes = 0U;
	i915_diag.wake_max_ns = 0U;
	i915_diag.queue_max_ns = 0U;
	i915_diag.item_max_ns = 0U;
	i915_diag.wait_irq_seen = worker->wait_interrupts;
	i915_diag.wait_tmo_seen = worker->wait_timeouts;
	i915_diag.boosts_seen = rps->boosts;
	i915_diag.up_seen = rps->up_events;
	i915_diag.down_seen = rps->down_events;
}

'''.replace('AWAKE2', awake2).replace('REFS2', refs2).replace('AWAKE', awake)
edit('worker.c', '#include "intel/commands.h"\n', globs)
edit('worker.c', 'static int i915_worker_loop(struct i915_worker *worker, int in_display);', funcs + 'static int i915_worker_loop(struct i915_worker *worker, int in_display);')
edit('worker.c', '''					spin_unlock_irqrestore(&device->irq_lock, irq);
					drv_i915_device_worker_rest(device);''', '''					spin_unlock_irqrestore(&device->irq_lock, irq);
					i915_diag.rests++;
					drv_i915_device_worker_rest(device);''', holds)
edit('worker.c', '''			(void)waitq_sleep(&worker->work, &device->irq_lock, observed, deadline, 0U);
		}
''' if holds else '''			(void)waitq_sleep(&worker->work, &device->irq_lock, observed, sched_ticks() + I915_WORKER_SLEEP_TICKS, 0U);
		}
''', ('''			(void)waitq_sleep(&worker->work, &device->irq_lock, observed, deadline, 0U);
''' if holds else '''			(void)waitq_sleep(&worker->work, &device->irq_lock, observed, sched_ticks() + I915_WORKER_SLEEP_TICKS, 0U);
''') + '''			spin_unlock_irqrestore(&device->irq_lock, irq);
			i915_diag_window(worker);
			irq = spin_lock_irqsave(&device->irq_lock);
		}
''')
edit('worker.c', '''			if (!awake) {
				spin_unlock_irqrestore(&device->irq_lock, irq);
				drv_i915_device_worker_wake(device);
				irq = spin_lock_irqsave(&device->irq_lock);
			}''', '''			if (!awake) {
				uint64_t wake_start;

				spin_unlock_irqrestore(&device->irq_lock, irq);
				wake_start = drv_i915_perf_now();
				drv_i915_device_worker_wake(device);
				i915_diag.wakes++;
				if (drv_i915_perf_now() - wake_start > i915_diag.wake_max_ns)
					i915_diag.wake_max_ns = drv_i915_perf_now() - wake_start;
				irq = spin_lock_irqsave(&device->irq_lock);
			}''', holds)
edit('worker.c', '''			i915_worker_run_sync_item(worker, item, in_display);

			irq = spin_lock_irqsave(&device->irq_lock);
			continue;''', '''			{
				uint64_t diag_start;
				uint64_t diag_queued;

				diag_queued = item->diag_queued_at;
				diag_start = drv_i915_perf_now();
				i915_worker_run_sync_item(worker, item, in_display);
				i915_diag_note(worker, 10U + (unsigned)item->kind, diag_queued, diag_start, drv_i915_perf_now());
				i915_diag_window(worker);
			}

			irq = spin_lock_irqsave(&device->irq_lock);
			continue;''')
edit('worker.c', '''		i915_worker_run_request(worker, request);

		irq = spin_lock_irqsave(&device->irq_lock);
		worker->current = NULL;''', '''		{
			uint64_t diag_start;
			uint64_t diag_queued;
			unsigned diag_kind;

			diag_queued = request->queued_at;
			diag_kind = 1U;
			if (request->batch == NULL)
				diag_kind = 2U;
			diag_start = drv_i915_perf_now();
			i915_worker_run_request(worker, request);
			i915_diag_note(worker, diag_kind, diag_queued, diag_start, drv_i915_perf_now());
			i915_diag_window(worker);
		}

		irq = spin_lock_irqsave(&device->irq_lock);
		worker->current = NULL;''')
edit('worker.c', '''	/* Queues the item and wakes the worker under the IRQ lock. */
	irq = spin_lock_irqsave(&device->irq_lock);
''', '''	/* Queues the item and wakes the worker under the IRQ lock. */
	item->diag_queued_at = drv_i915_perf_now();
	irq = spin_lock_irqsave(&device->irq_lock);
''')
edit('worker.c', '''	/* When the item was queued (drv_i915_perf_now()), for the context's queue and round times. */
	uint64_t queued_at;
''', '''	/* When the item was queued (drv_i915_perf_now()), for the context's queue and round times. */
	uint64_t queued_at;
	uint64_t diag_queued_at;
''')
# The actual GT frequency in the telemetry.
edit('gt-power.c', '''	/* The busy time at the driver's clock. */
	entry->time_ns = now;''', '''	/* Test-only: the actual GT frequency, 0 while the GT is in RC6. */
	if (rps->mmio != NULL) {
		entry->cur_mhz = ((drv_i915_raw_read32(rps->mmio, I915_GT_GEN12_RPSTAT1) & I915_GT_GEN12_CAGF_MASK) >> I915_GT_GEN12_CAGF_SHIFT) * 50U / 3U;
		entry->valid |= GPU_TELEMETRY_CUR_MHZ;
	}

	/* The busy time at the driver's clock. */
	entry->time_ns = now;''')
edit('command.c', '''	foreign = i915_stream_is_foreign(buffer, bytes);
	if (foreign != 0 && session->vk != NULL) {
		error = i915_command_render(session, buffer, bytes);
		if (error != 0)
			return error;
''', '''	foreign = i915_stream_is_foreign(buffer, bytes);
	if (foreign != 0 && session->vk != NULL) {
		uint64_t diag_start;
		uint64_t diag_ns;
		static unsigned diag_logged;

		diag_start = drv_i915_perf_now();
		error = i915_command_render(session, buffer, bytes);
		diag_ns = drv_i915_perf_now() - diag_start;
		if (diag_ns > 1000000000ULL && diag_logged < 60U) {
			diag_logged++;
			kern_logf("i915: diag: stream of %u bytes took %llu ms (error %d) at %llu ms\\n",
			    bytes,
			    (unsigned long long)(diag_ns / 1000000ULL),
			    error,
			    (unsigned long long)(drv_i915_perf_now() / 1000000ULL));
		}
		if (error != 0)
			return error;
''')
edit('vk/vk-pipeline-prepare.c', '''	unsigned graphics_ver;

	/* The kernels are compiled for the session's GT. */''', '''	unsigned graphics_ver;
	uint64_t diag_start;

	/* The kernels are compiled for the session's GT. */''')
edit('vk/vk-pipeline-prepare.c', '''	/* Compiles the vertex stage. */
	error = i915_pipeline_compile_stage(pipeline->vertex''', '''	diag_start = drv_i915_perf_now();

	/* Compiles the vertex stage. */
	error = i915_pipeline_compile_stage(pipeline->vertex''')
edit('vk/vk-pipeline-prepare.c', '''	kern_logf("i915: vk: pipeline compiled by the executor: vs %u bytes''', '''	kern_logf("i915: diag: pipeline compiled in %llu ms at %llu ms\\n",
	    (unsigned long long)((drv_i915_perf_now() - diag_start) / 1000000ULL),
	    (unsigned long long)(drv_i915_perf_now() / 1000000ULL));
	kern_logf("i915: vk: pipeline compiled by the executor: vs %u bytes''')
edit('vk/vk-pipeline-prepare.c', '#include "../i915.h"\n', '#include "../i915.h"\n#include "../perf.h"\n')
print('make-diag: done (holds=%d)' % holds)
# The GPU layer: a job that outlived its deadline (test-only).
g = top + '/src/drivers/gpu/gpu.c'
s = open(g).read()
old = '''			/* First expiry fixes this session's error and starts its independent stop interval. */
			if (candidate <= now) {
				gpu_session_fail_locked(session, ETIMEDOUT, 1U);'''
new = '''			/* First expiry fixes this session's error and starts its independent stop interval. */
			if (candidate <= now) {
				kern_logf("gpu: diag: session %p fails: job %llu (state %u) past its deadline by %llu ticks at tick %llu\\n",
				    (void *)session,
				    (unsigned long long)completion->sequence,
				    (unsigned)completion->job_state,
				    (unsigned long long)(now - candidate),
				    (unsigned long long)now);
				gpu_session_fail_locked(session, ETIMEDOUT, 1U);'''
assert old in s
s = s.replace(old, new, 1)
old = '''		if (completion->job_deadline != 0U && completion->job_deadline <= sched_ticks())
			error = ETIMEDOUT;
'''
new = '''		if (completion->job_deadline != 0U && completion->job_deadline <= sched_ticks()) {
			error = ETIMEDOUT;
			kern_logf("gpu: diag: job %llu ran past its reservation (tick %llu, deadline %llu)\\n",
			    (unsigned long long)sequence,
			    (unsigned long long)sched_ticks(),
			    (unsigned long long)completion->job_deadline);
		}
'''
assert old in s
s = s.replace(old, new, 1)
open(g, 'w').write(s)
print('make-diag: gpu.c done')
