/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 3D draw tests on the started GT.
 *
 * Five scenarios submit the fixed draws of the draw fixture on the render
 * engine through the request path the compute test owns: the single-colour
 * RECTLIST draw, the textured draw, a mix of draws and compute requests over
 * several contexts, a plan of texture updates and binding switches, and the
 * same plan with the bilinear sampler.  Each scenario takes forcewake the way
 * the device start does, logs the fixture, what the GPU wrote and the verdict,
 * and gives back what it created once the GPU is shown to be done with it:
 * the fixture pages go back to scratch, the GT TLB is invalidated, and only
 * then are the objects freed.  After a hang the engines are reset and the
 * objects are kept, because the GPU may still reach them.
 */

#include "scenarios.h"
#include "eu-internal.h"
#include "eu-test.h"
#include "../fixtures/draw-fixture.h"
#include "../../i915.h"
#include "../../engine.h"
#include "../../ggtt.h"
#include "../../mmio.h"
#include "../../ppgtt.h"
#include "../../submit.h"
#include <kern/kcrt.h>

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../../intel/gt-regs.h"

/* How many pages the fixture addresses span, from the state page to the second texture. */
#define I915_TEST_FIXTURE_PAGES		6U

/* How many dwords a fixture batch may hold; a batch that fills the page is refused. */
#define I915_TEST_BATCH_CAPACITY	1024U

/* How many pixels the 32x32 render target holds. */
#define I915_TEST_RT_PIXELS		1024U

/* The render target's pre-fill: neither an expected pixel nor zero. */
#define I915_TEST_RT_PREFILL		0x5a5a5a5aU

/* The value the compute markers hold before the GPU writes them. */
#define I915_TEST_MARKER_UNWRITTEN	0xdead0000U

/* How many pipeline statistics a hang record reads. */
#define I915_TEST_DRAW_STATS		6U

/* How many contexts the mixed test can create, and how many steps it can run. */
#define I915_TEST_R1_CTX_MAX		5U
#define I915_TEST_R1_STEPS_MAX		16U

/* How many steps a texture plan can run. */
#define I915_TEST_T3_STEPS_MAX		12U

/*
 * What one draw scenario runs against: the live GT's pieces.
 *
 * Filled from the device at the start of a scenario; the engine and its
 * execlists are filled once the render engine was found.
 */
struct i915_test_draw_env {
	/* The GT's engines, the kernel address space and the object pool. */
	struct i915_gt_engines *es;
	struct i915_gt_ppgtt *vm;
	struct i915_gt_mem *gm;

	/* The register access and the lock the engine reset takes. */
	struct i915_mmio *m;
	struct spinlock *uncore_lock;

	/* The fused slice information the compute batch's thread limit is computed from. */
	const struct i915_sseu *sseu;

	/* How long one request may take to retire. */
	unsigned timeout_ms;

	/* The render engine and its execlists. */
	struct i915_gt_engine *ge;
	struct i915_execlists *el;
};

/*
 * The GT interrupt counters at the start of a scenario.
 *
 * The verdict line logs how many interrupts of each kind the scenario saw.
 */
struct i915_test_irq_snapshot {
	unsigned user;
	unsigned ctx_switch;
	unsigned error;
};

/*
 * The single-colour draw: state page, batch and render target, submitted the
 * way the compute request is.
 *
 * One instance lives for one scenario run; the request path's record is the
 * embedded compute test.
 */
struct i915_test_draw {
	/* The context, the request, the state page (shared), the batch and the hang record. */
	struct i915_test_eu t;

	/* The render target. */
	struct i915_gt_object *rt;

	/* The MOCS index the fixture was built with, and the hash of the whole state page. */
	uint32_t mocs;
	uint64_t state_hash;

	/* The markers as read back. */
	uint32_t marker_before;
	uint32_t marker_middraw;
	uint32_t marker_after;
	uint32_t ps_marker;

	/* The first, middle and last pixel, and how many pixels hold the expected colour. */
	uint32_t px_first;
	uint32_t px_mid;
	uint32_t px_last;
	unsigned px_match;
	unsigned px_total;

	/* After a hang only: the pipeline statistics read live before the reset. */
	uint32_t stats_live[I915_TEST_DRAW_STATS];
	int stats_valid;
};

/*
 * The textured draw: the single-colour draw's objects plus an 8x8 texture.
 *
 * One instance lives for one scenario run.
 */
struct i915_test_tex {
	/* The context, the request, the state page, the batch and the hang record. */
	struct i915_test_eu t;

	/* The render target and the texture. */
	struct i915_gt_object *rt;
	struct i915_gt_object *tex;

	/* The MOCS index, and the hashes of the state page and the texture page. */
	uint32_t mocs;
	uint64_t state_hash;
	uint64_t tex_hash;

	/* The markers as read back. */
	uint32_t marker_before;
	uint32_t marker_middraw;
	uint32_t marker_after;
	uint32_t ps_marker;

	/* How many pixels match, still hold the pre-fill, and exist. */
	unsigned px_match;
	unsigned px_stale;
	unsigned px_total;

	/* The first wrong pixel, what it should have been and what it was. */
	int first_bad_x;
	int first_bad_y;
	uint32_t first_bad_expected;
	uint32_t first_bad_observed;

	/* Texel bytes that differ from what the CPU wrote, and guard bytes after the image that changed. */
	unsigned tex_changed_bytes;
	unsigned guard_bad_bytes;

	/* After a hang only: the pipeline statistics read live before the reset. */
	uint32_t stats_live[I915_TEST_DRAW_STATS];
	int stats_valid;
};

/*
 * One context of a multi-step test, with its own timeline.
 *
 * Created by the first step that runs on it and released with the test.
 */
struct i915_test_r1_ctx {
	struct i915_gt_context ce;
	struct i915_gt_object *tl_page;
	uint32_t seqno;

	/* Nonzero once the context image was allocated. */
	int created;
};

/*
 * What one step of the mixed test did.
 *
 * A draw step fills the draw fields, a compute step the compute fields.
 */
struct i915_test_r1_step {
	/* The context ('A'..) and the kind: 'D' for a draw, 'C' for the compute request. */
	char ctx;
	char kind;

	/* The wait's result, and whether the request completed, the engine parked and the step passed. */
	int rc;
	int completed;
	int parked;
	int pass;

	/* The context, the seqno the request carried and the one the status page showed. */
	uint32_t lrca;
	uint32_t seqno;
	uint32_t hwsp_observed;

	/* How many polls the wait took. */
	unsigned polls;

	/* The batch and state page as submitted. */
	uint64_t batch_hash;
	uint64_t state_hash;

	/* The draw's markers and pixels; px_stale counts pixels still holding the pre-fill. */
	uint32_t before;
	uint32_t middraw;
	uint32_t after;
	uint32_t ps_marker;
	unsigned px_match;
	unsigned px_stale;
	uint32_t px_first;
	uint32_t px_last;

	/* The compute request's markers and readback checks. */
	uint32_t ready;
	uint32_t eu;
	uint32_t done;
	uint32_t cs;
	int idd_rb_ok;
	int kernel_rb_ok;
};

/*
 * The mixed test: draws repeated on new contexts and compute requests
 * switched with draws on one context and across contexts.
 *
 * One instance lives for one scenario run.  The embedded compute test holds
 * the shared page and the compute batch; the draw batch sits beside it.
 */
struct i915_test_r1 {
	/* The shared page, the compute batch, the poll count and the hang record. */
	struct i915_test_eu t;

	/* The render target and the draw batch. */
	struct i915_gt_object *rt;
	struct i915_gt_object *dbatch;
	unsigned dbatch_dwords;

	/* The MOCS index and the hashes of the two batches as built. */
	uint32_t mocs;
	uint64_t c1_batch_hash;
	uint64_t draw_batch_hash;

	/* The request every step reuses. */
	struct i915_gt_request rq;

	/* The contexts and the steps. */
	struct i915_test_r1_ctx ctx[I915_TEST_R1_CTX_MAX];
	struct i915_test_r1_step step[I915_TEST_R1_STEPS_MAX];
	unsigned n_steps;
	unsigned n_planned;
	unsigned passed;
};

/*
 * What one step of a texture plan did.
 *
 * The expectation follows the image the bound texture holds after the
 * step's upload and the sampler filter of the step.
 */
struct i915_test_t3_step {
	/* The context, the texture binding table entry 1 names, and the texture rewritten first or '-'. */
	char ctx;
	char bind;
	char upload;
	int upload_variant;

	/* The image the bound texture holds, and the filter: 0 nearest, 1 bilinear. */
	int expect_variant;
	int linear;

	/* Expected pixels that differ from the nearest expectation, and the largest channel difference seen. */
	unsigned differs_from_nearest;
	unsigned max_channel_diff;

	/* The wait's result, and whether the request completed, the engine parked and the step passed. */
	int rc;
	int completed;
	int parked;
	int pass;

	/* The context, the seqno the request carried and the one the status page showed. */
	uint32_t lrca;
	uint32_t seqno;
	uint32_t hwsp_observed;

	/* How many polls the wait took. */
	unsigned polls;

	/* The state page, render target and both textures as hashed around the draw. */
	uint64_t state_hash;
	uint64_t rt_hash;
	uint64_t tex_a_hash;
	uint64_t tex_b_hash;

	/* The markers as read back. */
	uint32_t before;
	uint32_t middraw;
	uint32_t after;
	uint32_t ps_marker;

	/* How many pixels match and still hold the pre-fill, and the first wrong one. */
	unsigned px_match;
	unsigned px_stale;
	int first_bad_x;
	int first_bad_y;
	uint32_t first_bad_expected;
	uint32_t first_bad_observed;

	/* Texel and guard bytes that changed, over both textures. */
	unsigned tex_changed_bytes;
	unsigned guard_bad_bytes;
};

/*
 * A texture plan: texture updates, binding switches, redraws and a new
 * context, all with one batch.
 *
 * One instance lives for one scenario run.
 */
struct i915_test_t3 {
	/* The state page, the batch, the poll count and the hang record. */
	struct i915_test_eu t;

	/* The render target and the two textures. */
	struct i915_gt_object *rt;
	struct i915_gt_object *tex_a;
	struct i915_gt_object *tex_b;

	/* The MOCS index. */
	uint32_t mocs;

	/* The request every step reuses. */
	struct i915_gt_request rq;

	/* The two contexts. */
	struct i915_test_r1_ctx ctx[2];

	/* The image each texture currently holds. */
	int content[2];

	/* The steps. */
	struct i915_test_t3_step step[I915_TEST_T3_STEPS_MAX];
	unsigned n_steps;
	unsigned n_planned;
	unsigned passed;
};

/*
 * One row of a texture plan.
 *
 * The context the step runs on, the texture it binds, the texture the CPU
 * rewrites first ('-' for none) with which image, and the sampler filter.
 */
struct i915_test_t3_plan_row {
	char ctx;
	char bind;
	char upload;
	int variant;
	int linear;
};

/*
 * The pipeline statistics a hang record reads: IA vertices, IA primitives,
 * VS invocations, clipper invocations, clipper primitives, PS invocations.
 */
static const uint32_t i915_test_stat_reg[I915_TEST_DRAW_STATS] = {
	0x2310U, 0x2318U, 0x2320U, 0x2338U, 0x2340U, 0x2348U
};

/* Where the single-colour draw maps its state page, batch and render target. */
static const uint64_t i915_test_draw_va[3] = {
	I915_TEST_EU_SHARED_VA, I915_TEST_EU_BATCH_VA, I915_TEST_DRAW_RT_VA
};

/* Where the textured draw maps its objects; the page at 0x100403000 stays at scratch. */
static const uint64_t i915_test_tex_va[4] = {
	I915_TEST_EU_SHARED_VA, I915_TEST_EU_BATCH_VA, I915_TEST_DRAW_RT_VA, I915_TEX_FIXTURE_TEX_VA
};

/* Where the mixed test maps its shared page, compute batch, render target and draw batch. */
static const uint64_t i915_test_r1_va[4] = {
	I915_TEST_EU_SHARED_VA, I915_TEST_EU_BATCH_VA, I915_TEST_DRAW_RT_VA, I915_TEST_R1_DRAW_BATCH_VA
};

/* Where a texture plan maps its objects; the page at 0x100403000 stays at scratch. */
static const uint64_t i915_test_t3_va[5] = {
	I915_TEST_EU_SHARED_VA, I915_TEST_EU_BATCH_VA, I915_TEST_DRAW_RT_VA,
	I915_TEX_FIXTURE_TEX_VA, I915_TEX_FIXTURE_TEX_B_VA
};

/*
 * The mixed test's plan, one character per step.
 *
 * A: new context, draw x4.  B: new context, draw x2.  C: new context, draw,
 * compute, draw on the same context.  Then switching between contexts: A
 * compute, B draw, A compute.  A and B are reused because the GT window has
 * no room for five render contexts next to the migrate ring.
 */
static const char i915_test_r1_plan_ctx[] = "AAAABBCCCABA";
static const char i915_test_r1_plan_kind[] = "DDDDDDDCDCDC";

/*
 * The texture plan.
 *
 * Context A (new):  1 bind A (image 0), the plain textured draw
 *                   2 update A := image 1, bind A: content update of the same object
 *                   3 bind B (image 0): binding switch (A now holds image 1)
 *                   4 bind A: switch back
 *                   5 bind A: plain redraw
 * Context B (new):  6 bind A   7 bind B: the same fixture on a new context
 *                   8 update B := image 2, bind B
 * Context A again:  9 bind B (image 2): an older context after another one ran
 */
static const struct i915_test_t3_plan_row i915_test_t3_plan[] = {
	{ 'A', 'A', '-', 0, 0 }, { 'A', 'A', 'A', 1, 0 }, { 'A', 'B', '-', 0, 0 }, { 'A', 'A', '-', 0, 0 },
	{ 'A', 'A', '-', 0, 0 }, { 'B', 'A', '-', 0, 0 }, { 'B', 'B', '-', 0, 0 }, { 'B', 'B', 'B', 2, 0 },
	{ 'A', 'B', '-', 0, 0 },
};

/*
 * The bilinear plan.
 *
 * Image 3 (multiples of 64) in texture A.  Context A: nearest (control),
 * bilinear, nearest again (the filter change does not stick).  Context B
 * (new): bilinear.
 */
static const struct i915_test_t3_plan_row i915_test_bl_plan[] = {
	{ 'A', 'A', 'A', 3, 0 }, { 'A', 'A', '-', 0, 1 }, { 'A', 'A', '-', 0, 0 },
	{ 'B', 'A', '-', 0, 1 },
};

/*
 * The records of the five scenarios.
 *
 * They are too large for the kernel stack.  Each scenario clears its record
 * when it starts, and the start worker runs one scenario at a time.
 */
static struct i915_test_draw i915_draw_test;
static struct i915_test_tex i915_tex_test;
static struct i915_test_r1 i915_r1_test;
static struct i915_test_t3 i915_t3_test;

/*
 * The images the textured draw and the texture plans upload and compare
 * against.  Filled by the scenario that uses them before its first upload.
 */
static uint8_t i915_tex_test_pattern[I915_TEX_FIXTURE_TEX_BYTES];
static uint8_t i915_t3_test_pattern[I915_TEX_FIXTURE_VARIANTS][I915_TEX_FIXTURE_TEX_BYTES];

static void i915_draw_env_init(struct i915_test_draw_env *env, struct i915_device *device);
static void i915_draw_irq_snapshot(struct i915_device *device, struct i915_test_irq_snapshot *snapshot);
static int i915_draw_find_engine(struct i915_test_eu *t, struct i915_test_draw_env *env);
static int i915_draw_object_create(struct i915_test_eu *t, struct i915_gt_mem *gm, struct i915_gt_object **object);
static int i915_draw_map_objects(struct i915_test_eu *t, struct i915_test_draw_env *env, struct i915_gt_object *const *objects, const uint64_t *va, unsigned count, unsigned range_pages);
static int i915_draw_context_create(struct i915_test_eu *t, struct i915_gt_context *ce, struct i915_gt_object **tl_page, struct i915_test_draw_env *env, const char *create_step, const char *timeline_step, const char *pin_step);
static void i915_draw_walk_fixture(struct i915_test_eu *t, struct i915_gt_ppgtt *vm, const uint64_t *va, unsigned count);
static int i915_draw_submit_wait(struct i915_test_eu *t, struct i915_test_draw_env *env);
static void i915_draw_read_markers(struct i915_test_eu *t, uint32_t *before, uint32_t *middraw, uint32_t *after, uint32_t *ps_marker);
static void i915_draw_hang(struct i915_test_eu *t, struct i915_test_draw_env *env, uint32_t *stats_live, int *stats_valid);
static void i915_draw_tex_upload(struct i915_gt_object *tex, const uint8_t *pattern);
static unsigned i915_draw_tex_diff(struct i915_gt_object *tex, const uint8_t *pattern, unsigned *guard_bad);
static int i915_draw_gpu_idle(const struct i915_test_eu *t, int completed, int parked);
static int i915_draw_release(struct i915_device *device, struct i915_test_eu *t, const char *tag, int gpu_idle, struct i915_test_r1_ctx *contexts, unsigned context_count, struct i915_gt_object **const *objects, unsigned object_count);
static unsigned i915_draw_dss_count(const struct i915_sseu *sseu);

static int i915_test_draw_run(struct i915_test_draw *d, struct i915_test_draw_env *env);
static int i915_test_tex_run(struct i915_test_tex *x, struct i915_test_draw_env *env);
static void i915_test_tex_compare(struct i915_test_tex *x);

static void i915_r1_write_c1_state(void *page_cpu);
static int i915_test_r1_setup(struct i915_test_r1 *r, struct i915_test_draw_env *env);
static int i915_test_r1_run(struct i915_test_r1 *r, struct i915_test_draw_env *env);
static int i915_test_r1_step_prepare(struct i915_test_r1 *r, struct i915_test_draw_env *env, unsigned s);
static int i915_test_r1_step_submit(struct i915_test_r1 *r, struct i915_test_draw_env *env, unsigned s, int *wait_rc);
static void i915_test_r1_step_read(struct i915_test_r1 *r, unsigned s);
static int i915_test_r1_step_finish(struct i915_test_r1 *r, struct i915_test_draw_env *env, unsigned s, int wait_rc);

static int i915_test_t3_setup(struct i915_test_t3 *x, struct i915_test_draw_env *env);
static int i915_test_t3_run_plan(struct i915_test_t3 *x, struct i915_test_draw_env *env, const struct i915_test_t3_plan_row *plan, unsigned n_plan);
static int i915_test_t3_step_prepare(struct i915_test_t3 *x, struct i915_test_draw_env *env, const struct i915_test_t3_plan_row *row, unsigned s);
static int i915_test_t3_step_submit(struct i915_test_t3 *x, struct i915_test_draw_env *env, unsigned s, int *wait_rc);
static void i915_test_t3_step_read(struct i915_test_t3 *x, unsigned s);
static void i915_test_t3_compare_pixels(struct i915_test_t3_step *st, const volatile uint32_t *px, const uint8_t *want_img);
static int i915_test_t3_step_finish(struct i915_test_t3 *x, struct i915_test_draw_env *env, unsigned s, int wait_rc);
static int i915_test_t3_scenario(struct i915_device *device, const char *tag, const char *last_tag, const struct i915_test_t3_plan_row *plan, unsigned n_plan);

static void i915_draw_log_walks(const char *tag, const struct i915_test_eu *t, int with_va);
static void i915_draw_log_batch(const char *tag, const struct i915_test_eu *t);
static void i915_draw_log_state(const char *tag, const struct i915_test_eu *t);
static void i915_draw_log_tex(const char *tag, const struct i915_gt_object *tex);
static void i915_draw_log_rt(const char *tag, const struct i915_gt_object *rt);
static void i915_draw_log_stats(const char *tag, const uint32_t *stats_live, int stats_valid);
static const char *i915_draw_engine_name(struct i915_device *device, const struct i915_test_eu *t);
static void i915_test_draw_log(struct i915_device *device, const struct i915_test_draw *d, int run, const struct i915_test_irq_snapshot *irq);
static void i915_test_tex_log(struct i915_device *device, const struct i915_test_tex *x, int run, const struct i915_test_irq_snapshot *irq);
static void i915_test_r1_log(struct i915_device *device, const struct i915_test_r1 *r, int run, const struct i915_test_irq_snapshot *irq);
static void i915_test_t3_log(struct i915_device *device, const struct i915_test_t3 *x, const char *tag, const char *last_tag, int run, const struct i915_test_irq_snapshot *irq);
static int i915_draw_scenario_result(int passed, int run, int released);

/*
 * Runs the single-colour draw on the render engine and logs the result.
 *
 * The RECTLIST draw fills the 32x32 target with one colour; the scenario
 * passes when every marker and every pixel is what the fixture promises.
 */
int
drv_i915_test_execution_draw(
	struct i915_device *device)
{
	struct i915_test_draw_env env;
	struct i915_test_irq_snapshot irq;
	struct i915_test_draw *d;
	struct i915_gt_object **objects[1];
	unsigned held;
	int error;
	int run;
	int released;
	int passed;
	int idle;

	d = &i915_draw_test;

	/* Takes the five forcewake domains the GT tests run under. */
	held = 0U;
	error = drv_i915_test_forcewake_get_all(device, &held);
	if (error != 0) {
		kern_logf("i915: DRAW-TEST not run: forcewake failed rc=%d\n", error);
		drv_i915_test_forcewake_put_all(device, held);
		return error;
	}

	/* Runs the draw and logs what it did. */
	i915_draw_env_init(&env, device);
	i915_draw_irq_snapshot(device, &irq);
	run = i915_test_draw_run(d, &env);
	i915_test_draw_log(device, d, run, &irq);

	/* The draw passes when the fixture's promise held. */
	passed = 0;
	if (d->t.outcome == I915_TEST_EU_PASS)
		passed = 1;

	/* Gives back what the draw created once the GPU is done with it. */
	idle = i915_draw_gpu_idle(&d->t, d->t.completed, d->t.parked);
	objects[0] = &d->rt;
	released = i915_draw_release(device, &d->t, "DRAW-TEST", idle, NULL, 0U, objects, 1U);

	/* Puts back the forcewake domains. */
	drv_i915_test_forcewake_put_all(device, held);

	/* Reports the verdict, or a release that left the device in doubt. */
	error = i915_draw_scenario_result(passed, run, released);
	if (error != 0)
		return error;

	/* Succeeded: the draw passed and the device is clean. */
	return 0;
}

/*
 * Runs the mixed draw and compute test on the render engine and logs it.
 *
 * Twelve requests alternate between draws and the compute request over
 * three contexts; the scenario passes when every step passed.
 */
int
drv_i915_test_execution_r1(
	struct i915_device *device)
{
	struct i915_test_draw_env env;
	struct i915_test_irq_snapshot irq;
	struct i915_test_r1 *r;
	struct i915_gt_object **objects[2];
	unsigned held;
	int error;
	int run;
	int released;
	int passed;
	int idle;
	int completed;
	int parked;

	r = &i915_r1_test;

	/* Takes the five forcewake domains the GT tests run under. */
	held = 0U;
	error = drv_i915_test_forcewake_get_all(device, &held);
	if (error != 0) {
		kern_logf("i915: R1 not run: forcewake failed rc=%d\n", error);
		drv_i915_test_forcewake_put_all(device, held);
		return error;
	}

	/* Runs the plan and logs every step. */
	i915_draw_env_init(&env, device);
	i915_draw_irq_snapshot(device, &irq);
	run = i915_test_r1_run(r, &env);
	i915_test_r1_log(device, r, run, &irq);

	/* The test passes when every planned step passed. */
	passed = 0;
	if (run == 0 && r->passed == r->n_planned)
		passed = 1;

	/* The last step says whether a request may still hold the engine. */
	completed = 0;
	parked = 0;
	if (r->n_steps != 0U) {
		completed = r->step[r->n_steps - 1U].completed;
		parked = r->step[r->n_steps - 1U].parked;
	}

	/* Gives back what the test created once the GPU is done with it. */
	idle = i915_draw_gpu_idle(&r->t, completed, parked);
	objects[0] = &r->rt;
	objects[1] = &r->dbatch;
	released = i915_draw_release(device, &r->t, "R1", idle, r->ctx, I915_TEST_R1_CTX_MAX, objects, 2U);

	/* Puts back the forcewake domains. */
	drv_i915_test_forcewake_put_all(device, held);

	/* Reports the verdict, or a release that left the device in doubt. */
	error = i915_draw_scenario_result(passed, run, released);
	if (error != 0)
		return error;

	/* Succeeded: every step passed and the device is clean. */
	return 0;
}

/*
 * Runs the textured draw on the render engine and logs the result.
 *
 * The draw samples an 8x8 texture into the 32x32 target; the scenario
 * passes when every pixel is the expected texel and the texture and its
 * guard are unchanged.
 */
int
drv_i915_test_execution_tex(
	struct i915_device *device)
{
	struct i915_test_draw_env env;
	struct i915_test_irq_snapshot irq;
	struct i915_test_tex *x;
	struct i915_gt_object **objects[2];
	unsigned held;
	int error;
	int run;
	int released;
	int passed;
	int idle;

	x = &i915_tex_test;

	/* Takes the five forcewake domains the GT tests run under. */
	held = 0U;
	error = drv_i915_test_forcewake_get_all(device, &held);
	if (error != 0) {
		kern_logf("i915: TEX-TEST not run: forcewake failed rc=%d\n", error);
		drv_i915_test_forcewake_put_all(device, held);
		return error;
	}

	/* Runs the draw and logs what it did. */
	i915_draw_env_init(&env, device);
	i915_draw_irq_snapshot(device, &irq);
	run = i915_test_tex_run(x, &env);
	i915_test_tex_log(device, x, run, &irq);

	/* The draw passes when the fixture's promise held. */
	passed = 0;
	if (x->t.outcome == I915_TEST_EU_PASS)
		passed = 1;

	/* Gives back what the draw created once the GPU is done with it. */
	idle = i915_draw_gpu_idle(&x->t, x->t.completed, x->t.parked);
	objects[0] = &x->rt;
	objects[1] = &x->tex;
	released = i915_draw_release(device, &x->t, "TEX-TEST", idle, NULL, 0U, objects, 2U);

	/* Puts back the forcewake domains. */
	drv_i915_test_forcewake_put_all(device, held);

	/* Reports the verdict, or a release that left the device in doubt. */
	error = i915_draw_scenario_result(passed, run, released);
	if (error != 0)
		return error;

	/* Succeeded: the draw passed and the device is clean. */
	return 0;
}

/*
 * Runs the texture plan on the render engine and logs every step.
 *
 * Nine textured draws update a texture, switch the binding, redraw and move
 * to a new context and back; the scenario passes when every step passed.
 */
int
drv_i915_test_execution_t3(
	struct i915_device *device)
{
	int error;

	/* Runs the nearest-sampling plan. */
	error = i915_test_t3_scenario(
		device,
		"T3",
		"T3-LAST",
		i915_test_t3_plan,
		(unsigned)(sizeof(i915_test_t3_plan) / sizeof(i915_test_t3_plan[0])));
	if (error != 0)
		return error;

	/* Succeeded: every step passed and the device is clean. */
	return 0;
}

/*
 * Runs the bilinear texture plan on the render engine and logs every step.
 *
 * Nearest, bilinear and nearest again on one context, then bilinear on a
 * new one; the scenario passes when every step passed.
 */
int
drv_i915_test_execution_bl(
	struct i915_device *device)
{
	int error;

	/* Runs the bilinear plan. */
	error = i915_test_t3_scenario(
		device,
		"BL",
		"BL-LAST",
		i915_test_bl_plan,
		(unsigned)(sizeof(i915_test_bl_plan) / sizeof(i915_test_bl_plan[0])));
	if (error != 0)
		return error;

	/* Succeeded: every step passed and the device is clean. */
	return 0;
}

/* Fills a scenario's view of the live GT. */
static void
i915_draw_env_init(
	struct i915_test_draw_env *env,
	struct i915_device *device)
{
	/* Names the GT's pieces the tests submit through. */
	kern_memset(env, 0, sizeof(*env));
	env->es = &device->gt.engines;
	env->vm = &device->gt.ppgtt;
	env->gm = &device->gt.mem;
	env->m = &device->gt.mmio;
	env->uncore_lock = &device->gt.uncore_lock;
	env->sseu = &device->gt.info.sseu;
	env->timeout_ms = I915_TEST_TIMEOUT_MS;
}

/* Records the GT interrupt counters at the start of a scenario. */
static void
i915_draw_irq_snapshot(
	struct i915_device *device,
	struct i915_test_irq_snapshot *snapshot)
{
	/* Reads the counters the interrupt handler moves. */
	snapshot->user = device->gt.irq.gt_user_intr;
	snapshot->ctx_switch = device->gt.irq.gt_ctx_switch_intr;
	snapshot->error = device->gt.irq.gt_error_intr;
}

/* Finds the render engine and its execlists. */
static int
i915_draw_find_engine(
	struct i915_test_eu *t,
	struct i915_test_draw_env *env)
{
	unsigned i;

	/* Looks the render engine up in the GT's engine table. */
	for (i = 0U; i < env->es->n; i++) {
		if (env->es->ge[i].info->class == I915_RENDER_CLASS)
			break;
	}

	/* The GT has no render engine. */
	if (i == env->es->n) {
		(void)drv_i915_test_eu_fail(t, ENODEV, "no render engine");
		return ENODEV;
	}

	/* Names the engine and its execlists the requests go to. */
	t->engine_idx = i;
	env->ge = &env->es->ge[i];
	env->el = &env->es->el[i];

	/* Succeeded: the engine is known. */
	return 0;
}

/* Creates one page-sized test object, or records the allocation failure. */
static int
i915_draw_object_create(
	struct i915_test_eu *t,
	struct i915_gt_mem *gm,
	struct i915_gt_object **object)
{
	/* Allocates the object from the GT's pool. */
	*object = drv_i915_gt_object_create(gm, 4096U);
	if (*object == NULL) {
		(void)drv_i915_test_eu_fail(t, ENOMEM, "gem_create");
		return ENOMEM;
	}

	/* Succeeded: the object exists. */
	return 0;
}

/* Maps each object's page at its fixed address in the kernel address space. */
static int
i915_draw_map_objects(
	struct i915_test_eu *t,
	struct i915_test_draw_env *env,
	struct i915_gt_object *const *objects,
	const uint64_t *va,
	unsigned count,
	unsigned range_pages)
{
	uint64_t dma;
	unsigned i;
	int error;

	/* Allocates the page tables of the whole fixture range. */
	error = drv_i915_gt_ppgtt_alloc_range(env->gm, env->vm, I915_TEST_EU_SHARED_VA, (uint64_t)range_pages * 4096U);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, "allocate_va_range");
		return error;
	}

	/* Points each address at its object's page with PAT 0 (I915_CACHE_LLC). */
	for (i = 0U; i < count; i++) {
		error = drv_i915_gt_object_page_dma(objects[i], 0U, &dma);
		if (error != 0)
			break;

		error = drv_i915_gt_ppgtt_insert_page(env->vm, dma, va[i], 0U);
		if (error != 0)
			break;
	}

	/* Reports the page that could not be mapped. */
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, "ppgtt_insert");
		return error;
	}

	/* Succeeded: every object is mapped. */
	return 0;
}

/*
 * Creates a context of the kernel address space with its own timeline.
 *
 * intel_context_create(engine): the image inherits the engine's default
 * state; the timeline page is bound into the GGTT for the breadcrumbs.
 */
static int
i915_draw_context_create(
	struct i915_test_eu *t,
	struct i915_gt_context *ce,
	struct i915_gt_object **tl_page,
	struct i915_test_draw_env *env,
	const char *create_step,
	const char *timeline_step,
	const char *pin_step)
{
	int error;

	/* Allocates the context image and its ring. */
	error = drv_i915_lrc_alloc(ce, env->ge, env->vm, env->gm, 4096U, 0U);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, create_step);
		return error;
	}

	/* Creates the timeline page. */
	*tl_page = drv_i915_gt_object_create(env->gm, 4096U);
	if (*tl_page == NULL) {
		(void)drv_i915_test_eu_fail(t, ENOMEM, timeline_step);
		return ENOMEM;
	}

	/* Binds the timeline page where the GPU writes the breadcrumbs. */
	error = drv_i915_gt_ggtt_bind(env->gm, *tl_page);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, pin_step);
		return error;
	}

	/* Fills the register state and points the ring registers at the empty ring. */
	drv_i915_lrc_init_state(ce);
	(void)drv_i915_lrc_update_regs(ce, ce->ring.tail);

	/* Succeeded: the context can take requests. */
	return 0;
}

/*
 * Records what the GPU will walk for the fixture addresses.
 *
 * Read immediately before submission from the tables PDP0 of the test
 * context names.
 */
static void
i915_draw_walk_fixture(
	struct i915_test_eu *t,
	struct i915_gt_ppgtt *vm,
	const uint64_t *va,
	unsigned count)
{
	uint64_t pdp0;
	unsigned i;

	/* Checks that the context names the address space's top directory. */
	pdp0 = ((uint64_t)t->ce.lrc_reg_state[CTX_PDP0_UDW] << 32) | t->ce.lrc_reg_state[CTX_PDP0_LDW];
	t->pdp0_matches_top = 0;
	if (pdp0 == vm->top_pd_dma)
		t->pdp0_matches_top = 1;

	/* Walks each fixture address. */
	for (i = 0U; i < count; i++)
		(void)drv_i915_test_ppgtt_walk(vm, va[i], &t->walk[i]);

	t->walks = count;
}

/* Submits the test's request and waits for it to retire. */
static int
i915_draw_submit_wait(
	struct i915_test_eu *t,
	struct i915_test_draw_env *env)
{
	int error;
	int wait_rc;

	/* Hands the request to the engine. */
	error = drv_i915_execlists_submit(env->ge, env->el, env->m, &t->rq);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, "execlists_submit");
		return error;
	}

	t->submitted = 1;

	/* Waits for the request like i915_request_wait(). */
	wait_rc = drv_i915_test_eu_wait_retired(t, env->ge, env->el, &t->rq, env->m, env->timeout_ms);
	if (wait_rc == 0) {
		/* The request landed and the engine reported the context complete. */
		t->completed = 1;
	} else if (wait_rc == ETIMEDOUT) {
		/* The request never retired. */
		t->timed_out = 1;
	} else {
		/* The wait itself failed. */
		(void)drv_i915_test_eu_fail(t, wait_rc, "i915_request_wait");
	}

	/* Succeeded: the request was submitted; the record says how it ended. */
	return 0;
}

/* Reads the draw fixture's markers from the state page. */
static void
i915_draw_read_markers(
	struct i915_test_eu *t,
	uint32_t *before,
	uint32_t *middraw,
	uint32_t *after,
	uint32_t *ps_marker)
{
	volatile uint32_t *mk;

	/* Reads the before, after, mid-draw and pixel shader markers. */
	mk = (volatile uint32_t *)t->shared->cpu + I915_DRAW_FIXTURE_MARKER_OFFSET / 4U;
	*before = mk[0];
	*after = mk[1];
	*middraw = mk[2];
	*ps_marker = mk[4];
}

/*
 * Records a request that did not complete and resets the engines.
 *
 * The context is still on the hardware, so the pipeline statistics are
 * live; they are read before the reset like intel_gt_set_wedged().
 */
static void
i915_draw_hang(
	struct i915_test_eu *t,
	struct i915_test_draw_env *env,
	uint32_t *stats_live,
	int *stats_valid)
{
	unsigned i;

	/* A request that timed out is a hang; anything else stays an error. */
	if (t->timed_out)
		t->outcome = I915_TEST_EU_HANG;

	/* Reads the pipeline statistics while the context is loaded. */
	for (i = 0U; i < I915_TEST_DRAW_STATS; i++)
		stats_live[i] = drv_i915_read32(env->m, i915_test_stat_reg[i]);

	*stats_valid = 1;

	/* Logs the request's record, dumps the engine and resets the engines. */
	drv_i915_test_eu_log_record(t, env->ge, env->el, &t->rq, &t->ce, "hang");
	drv_i915_test_eu_hang_dump_reset(t, env->es, env->ge, env->el, env->m, env->uncore_lock);
}

/* Writes an image into a texture page and fills the rest of the page with the guard byte. */
static void
i915_draw_tex_upload(
	struct i915_gt_object *tex,
	const uint8_t *pattern)
{
	volatile uint8_t *tb;
	unsigned i;

	/* Copies the image, then the guard after it. */
	tb = (volatile uint8_t *)tex->cpu;
	for (i = 0U; i < 4096U; i++) {
		if (i < I915_TEX_FIXTURE_TEX_BYTES) {
			tb[i] = pattern[i];
		} else {
			tb[i] = (uint8_t)I915_TEST_TEX_GUARD_BYTE;
		}
	}
}

/* Counts the image bytes and the guard bytes of a texture page that changed. */
static unsigned
i915_draw_tex_diff(
	struct i915_gt_object *tex,
	const uint8_t *pattern,
	unsigned *guard_bad)
{
	volatile uint8_t *tb;
	unsigned changed;
	unsigned i;
	uint8_t want;

	/* Compares every byte of the page with what the CPU wrote. */
	tb = (volatile uint8_t *)tex->cpu;
	changed = 0U;
	for (i = 0U; i < 4096U; i++) {
		if (i < I915_TEX_FIXTURE_TEX_BYTES) {
			want = pattern[i];
		} else {
			want = (uint8_t)I915_TEST_TEX_GUARD_BYTE;
		}

		/* Counts a changed texel byte or a changed guard byte. */
		if (tb[i] != want) {
			if (i < I915_TEX_FIXTURE_TEX_BYTES) {
				changed++;
			} else {
				(*guard_bad)++;
			}
		}
	}

	/* Reports how many image bytes changed. */
	return changed;
}

/* Tells whether the GPU is shown to be done with everything the test submitted. */
static int
i915_draw_gpu_idle(
	const struct i915_test_eu *t,
	int completed,
	int parked)
{
	/* A reset engine may still reach what the hung request referenced. */
	if (t->wedged)
		return 0;

	/* A retired request whose engine did not switch back still has its context loaded. */
	if (completed && !parked)
		return 0;

	/* Reports an idle GPU. */
	return 1;
}

/*
 * Gives back what a test created, in the order the GPU needs.
 *
 * The fixture pages go back to scratch, the GT TLB is invalidated, and only
 * then are the contexts and the objects freed.  When the GPU is not shown
 * to be done, or the TLB could not be invalidated, everything is kept.
 */
static int
i915_draw_release(
	struct i915_device *device,
	struct i915_test_eu *t,
	const char *tag,
	int gpu_idle,
	struct i915_test_r1_ctx *contexts,
	unsigned context_count,
	struct i915_gt_object **const *objects,
	unsigned object_count)
{
	struct i915_gt_mem *gm;
	unsigned scrubbed;
	unsigned i;
	int error;

	gm = &device->gt.mem;

	/* Keeps everything a request that was not shown finished may still use. */
	if (!gpu_idle) {
		kern_logf("i915: %s release: the GPU is not shown idle (wedged=%d): the test's objects are kept\n",
			  tag,
			  t->wedged);
		return EBUSY;
	}

	/* Points the fixture addresses back at scratch before any page they named is freed. */
	drv_i915_test_eu_scrub_ptes(t, &device->gt.ppgtt, I915_TEST_EU_SHARED_VA, I915_TEST_FIXTURE_PAGES);
	scrubbed = t->ptes_scrubbed;

	/* Drops every translation the engines may have cached for them. */
	error = drv_i915_test_eu_invalidate_tlb(device);
	if (error != 0) {
		kern_logf("i915: %s release: TLB invalidation failed rc=%d: the test's objects are kept\n", tag, error);
		return error;
	}

	/* Frees the contexts the steps created, and their timelines. */
	for (i = 0U; i < context_count; i++) {
		/* Frees the context's timeline page. */
		if (contexts[i].tl_page != NULL) {
			drv_i915_gt_object_destroy(gm, contexts[i].tl_page);
			contexts[i].tl_page = NULL;
		}

		/* Frees the context image and its ring. */
		if (contexts[i].created && contexts[i].ce.allocated)
			drv_i915_lrc_release(&contexts[i].ce, gm);
	}

	/* Frees the test's own objects. */
	for (i = 0U; i < object_count; i++) {
		if (*objects[i] != NULL) {
			drv_i915_gt_object_destroy(gm, *objects[i]);
			*objects[i] = NULL;
		}
	}

	/* Frees the request path's context, timeline, batch and shared page. */
	drv_i915_test_eu_release(t, gm);
	kern_logf("i915: %s release: fixture ptes scrubbed=%u, TLB invalidated, objects freed\n", tag, scrubbed);

	/* Succeeded: the device holds nothing of the test. */
	return 0;
}

/* Counts the enabled dual-subslices. */
static unsigned
i915_draw_dss_count(
	const struct i915_sseu *sseu)
{
	unsigned count;
	unsigned bit;

	/* Counts each set bit of the subslice mask. */
	count = 0U;
	for (bit = 0U; bit < 16U; bit++) {
		if (((sseu->subslice_mask >> bit) & 1U) != 0U)
			count++;
	}

	/* Reports the count. */
	return count;
}

/*
 * Runs the single-colour draw.
 *
 * State page, batch and render target are softpinned into the kernel
 * address space and submitted through one request on a new context.
 */
static int
i915_test_draw_run(
	struct i915_test_draw *d,
	struct i915_test_draw_env *env)
{
	struct i915_test_eu *t;
	struct i915_gt_object *objects[3];
	volatile uint32_t *px;
	unsigned i;
	int error;

	/* Starts from an empty record. */
	kern_memset(d, 0, sizeof(*d));
	t = &d->t;

	/* Finds the engine the draw runs on. */
	error = i915_draw_find_engine(t, env);
	if (error != 0)
		return error;

	/* Creates the state page, the batch and the render target. */
	error = i915_draw_object_create(t, env->gm, &t->shared);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &t->batch);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &d->rt);
	if (error != 0)
		return error;

	/* Maps the three objects at their fixed addresses. */
	objects[0] = t->shared;
	objects[1] = t->batch;
	objects[2] = d->rt;
	error = i915_draw_map_objects(t, env, objects, i915_test_draw_va, 3U, 3U);
	if (error != 0)
		return error;

	/* Writes the fixture: a cleared target, the state page and the batch. */
	d->mocs = drv_i915_draw_fixture_mocs();
	kern_memset(d->rt->cpu, 0, 4096U);
	drv_i915_draw_fixture_write_state(t->shared->cpu, I915_TEST_DRAW_RT_VA, d->mocs);
	t->batch_dwords = drv_i915_draw_fixture_build_batch(
		(uint32_t *)t->batch->cpu,
		I915_TEST_BATCH_CAPACITY,
		I915_TEST_EU_SHARED_VA,
		d->mocs);
	if (t->batch_dwords == 0U || t->batch_dwords >= I915_TEST_BATCH_CAPACITY) {
		(void)drv_i915_test_eu_fail(t, ENOSPC, "build_batch");
		return ENOSPC;
	}

	/* Reads the select words back from the submitted object; a bad select is never submitted. */
	t->pipesel_rc = drv_i915_test_draw_check_pipeline_select((const uint32_t *)t->batch->cpu, t->batch_dwords, &t->pipesel);
	if (t->pipesel_rc != 0) {
		(void)drv_i915_test_eu_fail(t, t->pipesel_rc, "pipeline_select_verify");
		return t->pipesel_rc;
	}

	/* Records the fixture's identity: the batch words and the whole state page. */
	t->batch_hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);
	d->state_hash = drv_i915_test_fnv1a64(t->shared->cpu, 4096U, I915_TEST_FNV_BASIS);

	/* Creates the draw's context. */
	error = i915_draw_context_create(
		t,
		&t->ce,
		&t->tl_page,
		env,
		"intel_context_create",
		"intel_timeline_create",
		"intel_timeline_pin");
	if (error != 0)
		return error;

	/* Builds the request that starts the batch. */
	t->tl_seqno = 2U;
	error = drv_i915_test_eu_build_request(t, &t->rq, &t->ce, t->tl_page, t->tl_seqno, I915_TEST_EU_BATCH_VA);
	if (error != 0)
		return error;

	/* Records the walks, then submits and waits. */
	i915_draw_walk_fixture(t, env->vm, i915_test_draw_va, 3U);
	error = i915_draw_submit_wait(t, env);
	if (error != 0)
		return error;

	/* Reads the markers and the pixels, whatever the request did. */
	i915_draw_read_markers(t, &d->marker_before, &d->marker_middraw, &d->marker_after, &d->ps_marker);
	px = (volatile uint32_t *)d->rt->cpu;
	d->px_total = I915_DRAW_FIXTURE_WIDTH * I915_DRAW_FIXTURE_HEIGHT;
	for (i = 0U; i < d->px_total; i++) {
		if (px[i] == I915_DRAW_FIXTURE_EXPECTED_PIXEL)
			d->px_match++;
	}

	d->px_first = px[0];
	d->px_mid = px[d->px_total / 2U];
	d->px_last = px[d->px_total - 1U];

	/* Records a request that did not complete and resets the engines. */
	if (!t->completed) {
		i915_draw_hang(t, env, d->stats_live, &d->stats_valid);
		if (t->err != 0)
			return t->err;

		return ETIMEDOUT;
	}

	/* Logs the completed request and switches the engine back to its kernel context. */
	drv_i915_test_eu_log_record(t, env->ge, env->el, &t->rq, &t->ce, "completed");
	t->parked = drv_i915_test_eu_park(t, env->es, env->ge, env->el, env->m, env->timeout_ms);

	/* The draw passed when every marker and every pixel is the promised one. */
	t->outcome = I915_TEST_EU_ERROR;
	if (d->marker_before == I915_DRAW_FIXTURE_MARKER_BEFORE &&
	    d->marker_middraw == I915_DRAW_FIXTURE_MARKER_MIDDRAW &&
	    d->marker_after == I915_DRAW_FIXTURE_MARKER_AFTER &&
	    d->px_match == d->px_total)
		t->outcome = I915_TEST_EU_PASS;

	/* Names a wrong marker or pixel as the error. */
	if (t->outcome == I915_TEST_EU_ERROR && t->err == 0)
		(void)drv_i915_test_eu_fail(t, EIO, "markers/pixels");

	/* Reports the first error the run recorded. */
	if (t->err != 0)
		return t->err;

	/* Succeeded: the draw did what the fixture promises. */
	return 0;
}

/*
 * Runs the textured draw.
 *
 * The single-colour draw's objects plus a texture page holding an 8x8
 * image and a guard, submitted through one request on a new context.
 */
static int
i915_test_tex_run(
	struct i915_test_tex *x,
	struct i915_test_draw_env *env)
{
	struct i915_test_eu *t;
	struct i915_gt_object *objects[4];
	volatile uint32_t *px;
	unsigned i;
	int error;

	/* Starts from an empty record. */
	kern_memset(x, 0, sizeof(*x));
	t = &x->t;
	x->first_bad_x = -1;
	x->first_bad_y = -1;

	/* Finds the engine the draw runs on. */
	error = i915_draw_find_engine(t, env);
	if (error != 0)
		return error;

	/* Creates the state page, the batch, the render target and the texture. */
	error = i915_draw_object_create(t, env->gm, &t->shared);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &t->batch);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &x->rt);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &x->tex);
	if (error != 0)
		return error;

	/* Maps the four objects; the range covers 0x100400000..0x100404fff. */
	objects[0] = t->shared;
	objects[1] = t->batch;
	objects[2] = x->rt;
	objects[3] = x->tex;
	error = i915_draw_map_objects(t, env, objects, i915_test_tex_va, 4U, 5U);
	if (error != 0)
		return error;

	/* Uploads the texture image with its guard and pre-fills the render target. */
	x->mocs = drv_i915_draw_fixture_mocs();
	drv_i915_tex_fixture_pattern(i915_tex_test_pattern, 0U);
	i915_draw_tex_upload(x->tex, i915_tex_test_pattern);
	px = (volatile uint32_t *)x->rt->cpu;
	x->px_total = I915_DRAW_FIXTURE_WIDTH * I915_DRAW_FIXTURE_HEIGHT;
	for (i = 0U; i < x->px_total; i++)
		px[i] = I915_TEST_RT_PREFILL;

	/* Writes the state page and the batch. */
	drv_i915_tex_fixture_write_state(t->shared->cpu, I915_TEST_DRAW_RT_VA, I915_TEX_FIXTURE_TEX_VA, x->mocs);
	t->batch_dwords = drv_i915_tex_fixture_build_batch(
		(uint32_t *)t->batch->cpu,
		I915_TEST_BATCH_CAPACITY,
		I915_TEST_EU_SHARED_VA,
		x->mocs);
	if (t->batch_dwords == 0U || t->batch_dwords >= I915_TEST_BATCH_CAPACITY) {
		(void)drv_i915_test_eu_fail(t, ENOSPC, "build_batch");
		return ENOSPC;
	}

	/* Reads the select words back from the submitted object; a bad select is never submitted. */
	t->pipesel_rc = drv_i915_test_draw_check_pipeline_select((const uint32_t *)t->batch->cpu, t->batch_dwords, &t->pipesel);
	if (t->pipesel_rc != 0) {
		(void)drv_i915_test_eu_fail(t, t->pipesel_rc, "pipeline_select_verify");
		return t->pipesel_rc;
	}

	/* Records the fixture's identity: the batch, the state page and the texture page. */
	t->batch_hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);
	x->state_hash = drv_i915_test_fnv1a64(t->shared->cpu, 4096U, I915_TEST_FNV_BASIS);
	x->tex_hash = drv_i915_test_fnv1a64(x->tex->cpu, 4096U, I915_TEST_FNV_BASIS);

	/* Creates the draw's context. */
	error = i915_draw_context_create(
		t,
		&t->ce,
		&t->tl_page,
		env,
		"intel_context_create",
		"intel_timeline_create",
		"intel_timeline_pin");
	if (error != 0)
		return error;

	/* Builds the request that starts the batch. */
	t->tl_seqno = 2U;
	error = drv_i915_test_eu_build_request(t, &t->rq, &t->ce, t->tl_page, t->tl_seqno, I915_TEST_EU_BATCH_VA);
	if (error != 0)
		return error;

	/* Records the walks, then submits and waits. */
	i915_draw_walk_fixture(t, env->vm, i915_test_tex_va, 4U);
	error = i915_draw_submit_wait(t, env);
	if (error != 0)
		return error;

	/* Reads the markers, the pixels and the texture page, whatever the request did. */
	i915_draw_read_markers(t, &x->marker_before, &x->marker_middraw, &x->marker_after, &x->ps_marker);
	i915_test_tex_compare(x);

	/* Records a request that did not complete and resets the engines. */
	if (!t->completed) {
		i915_draw_hang(t, env, x->stats_live, &x->stats_valid);
		if (t->err != 0)
			return t->err;

		return ETIMEDOUT;
	}

	/* Logs the completed request and switches the engine back to its kernel context. */
	drv_i915_test_eu_log_record(t, env->ge, env->el, &t->rq, &t->ce, "completed");
	t->parked = drv_i915_test_eu_park(t, env->es, env->ge, env->el, env->m, env->timeout_ms);

	/* The draw passed when the engine parked and markers, pixels, texture and guard are the promised ones. */
	t->outcome = I915_TEST_EU_ERROR;
	if (t->parked &&
	    x->marker_before == I915_DRAW_FIXTURE_MARKER_BEFORE &&
	    x->marker_middraw == I915_DRAW_FIXTURE_MARKER_MIDDRAW &&
	    x->marker_after == I915_DRAW_FIXTURE_MARKER_AFTER &&
	    x->ps_marker == I915_DRAW_FIXTURE_PS_MARKER &&
	    x->px_match == x->px_total &&
	    x->tex_changed_bytes == 0U &&
	    x->guard_bad_bytes == 0U)
		t->outcome = I915_TEST_EU_PASS;

	/* Names a wrong marker, pixel or texture byte as the error. */
	if (t->outcome == I915_TEST_EU_ERROR && t->err == 0)
		(void)drv_i915_test_eu_fail(t, EIO, "markers/pixels/guard");

	/* Reports the first error the run recorded. */
	if (t->err != 0)
		return t->err;

	/* Succeeded: the draw did what the fixture promises. */
	return 0;
}

/* Compares the textured draw's target with the image and checks the texture page. */
static void
i915_test_tex_compare(
	struct i915_test_tex *x)
{
	volatile uint32_t *px;
	unsigned i;
	unsigned xx;
	unsigned yy;
	uint32_t want;
	uint32_t got;

	/* Compares each pixel with the texel nearest sampling gives it. */
	px = (volatile uint32_t *)x->rt->cpu;
	for (i = 0U; i < x->px_total; i++) {
		xx = i % I915_DRAW_FIXTURE_WIDTH;
		yy = i / I915_DRAW_FIXTURE_WIDTH;
		want = drv_i915_tex_fixture_expected_pixel(i915_tex_test_pattern, xx, yy);
		got = px[i];

		/* Counts a match, or records a stale or first wrong pixel. */
		if (got == want) {
			x->px_match++;
		} else {
			if (got == I915_TEST_RT_PREFILL)
				x->px_stale++;

			/* Keeps the first wrong pixel for the log. */
			if (x->first_bad_x < 0) {
				x->first_bad_x = (int)xx;
				x->first_bad_y = (int)yy;
				x->first_bad_expected = want;
				x->first_bad_observed = got;
			}
		}
	}

	/* Counts the texel and guard bytes the draw changed. */
	x->tex_changed_bytes = i915_draw_tex_diff(x->tex, i915_tex_test_pattern, &x->guard_bad_bytes);
}

/* Writes the compute test's fixture into the shared page, exactly as the compute test does. */
static void
i915_r1_write_c1_state(
	void *page_cpu)
{
	volatile uint32_t *page;
	volatile uint32_t *idd;

	page = (volatile uint32_t *)page_cpu;
	idd = page + I915_TEST_EU_IDD_OFFSET / 4U;

	/* Clears the page and copies the store kernel to its start pointer. */
	kern_memset(page_cpu, 0, 4096U);
	kern_memcpy((char *)page_cpu + I915_TEST_EU_KSP_OFFSET, drv_i915_test_eu_kernel, sizeof(drv_i915_test_eu_kernel));

	/* Writes the interface descriptor: the kernel start pointer and one thread per group. */
	idd[0] = I915_TEST_EU_KSP_OFFSET;
	idd[1] = 0U;
	idd[2] = 1U << 20;
	idd[3] = 0U;
	idd[4] = 0U;
	idd[5] = 0U;
	idd[6] = 1U;
	idd[7] = 0U;

	/* Sets the markers to the value that says the GPU has not written them. */
	page[I915_TEST_EU_READY_OFF / 4U] = I915_TEST_MARKER_UNWRITTEN;
	page[I915_TEST_EU_EU_OFF / 4U] = I915_TEST_MARKER_UNWRITTEN;
	page[I915_TEST_EU_DONE_OFF / 4U] = I915_TEST_MARKER_UNWRITTEN;
	page[I915_TEST_EU_CS_OFF / 4U] = I915_TEST_MARKER_UNWRITTEN;
}

/* Creates and maps the mixed test's objects and builds both batches once. */
static int
i915_test_r1_setup(
	struct i915_test_r1 *r,
	struct i915_test_draw_env *env)
{
	struct i915_test_eu *t;
	struct i915_gt_object *objects[4];
	struct i915_test_pipeline_select check;
	unsigned c1_dwords;
	int error;

	t = &r->t;

	/* Finds the engine the requests run on. */
	error = i915_draw_find_engine(t, env);
	if (error != 0)
		return error;

	/* VFE MaxThreads = max_cs_threads (112) * enabled DSS - 1, as blorp computes it. */
	t->dss_count = i915_draw_dss_count(env->sseu);
	if (t->dss_count == 0U) {
		(void)drv_i915_test_eu_fail(t, EINVAL, "no subslice");
		return EINVAL;
	}

	t->max_threads = 112U * t->dss_count - 1U;

	/* Creates the shared page, the compute batch, the render target and the draw batch. */
	error = i915_draw_object_create(t, env->gm, &t->shared);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &t->batch);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &r->rt);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &r->dbatch);
	if (error != 0)
		return error;

	/* Maps the four objects at their fixed addresses. */
	objects[0] = t->shared;
	objects[1] = t->batch;
	objects[2] = r->rt;
	objects[3] = r->dbatch;
	error = i915_draw_map_objects(t, env, objects, i915_test_r1_va, 4U, 4U);
	if (error != 0)
		return error;

	/* Builds both batches once; their bytes do not depend on their own address. */
	r->mocs = drv_i915_draw_fixture_mocs();
	c1_dwords = drv_i915_test_eu_build_batch(
		(uint32_t *)t->batch->cpu,
		I915_TEST_BATCH_CAPACITY,
		I915_TEST_EU_SHARED_VA,
		I915_TEST_EU_SHARED_VA,
		t->max_threads);
	r->dbatch_dwords = drv_i915_draw_fixture_build_batch(
		(uint32_t *)r->dbatch->cpu,
		I915_TEST_BATCH_CAPACITY,
		I915_TEST_EU_SHARED_VA,
		r->mocs);
	if (c1_dwords == 0U ||
	    r->dbatch_dwords == 0U ||
	    r->dbatch_dwords >= I915_TEST_BATCH_CAPACITY) {
		(void)drv_i915_test_eu_fail(t, ENOSPC, "build_batch");
		return ENOSPC;
	}

	t->batch_dwords = c1_dwords;

	/* Reads the compute batch's select words back; a bad select is never submitted. */
	error = drv_i915_test_eu_check_pipeline_select((const uint32_t *)t->batch->cpu, c1_dwords, &check);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, EINVAL, "pipeline_select_verify");
		return EINVAL;
	}

	/* Reads the draw batch's select words back the same way. */
	error = drv_i915_test_draw_check_pipeline_select((const uint32_t *)r->dbatch->cpu, r->dbatch_dwords, &check);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, EINVAL, "pipeline_select_verify");
		return EINVAL;
	}

	/* Records both batches' identity. */
	r->c1_batch_hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)c1_dwords * 4U, I915_TEST_FNV_BASIS);
	r->draw_batch_hash = drv_i915_test_fnv1a64(r->dbatch->cpu, (size_t)r->dbatch_dwords * 4U, I915_TEST_FNV_BASIS);

	/* Succeeded: both batches are ready. */
	return 0;
}

/*
 * Runs the mixed test's plan.
 *
 * Each step waits for its request and parks the engine before the CPU
 * touches what the next step references.
 */
static int
i915_test_r1_run(
	struct i915_test_r1 *r,
	struct i915_test_draw_env *env)
{
	unsigned s;
	int error;
	int wait_rc;

	/* Starts from an empty record. */
	kern_memset(r, 0, sizeof(*r));
	r->n_planned = (unsigned)(sizeof(i915_test_r1_plan_ctx) - 1U);

	/* Creates the objects and the batches. */
	error = i915_test_r1_setup(r, env);
	if (error != 0)
		return error;

	/* Runs every step in order; the first failed step ends the test. */
	for (s = 0U; s < r->n_planned; s++) {
		/* Creates the step's context when it is new and rewrites its fixture. */
		error = i915_test_r1_step_prepare(r, env, s);
		if (error != 0)
			return error;

		/* Submits the step and waits for it. */
		error = i915_test_r1_step_submit(r, env, s, &wait_rc);
		if (error != 0)
			return error;

		/* Reads what the step wrote and decides it. */
		i915_test_r1_step_read(r, s);
		error = i915_test_r1_step_finish(r, env, s, wait_rc);
		if (error != 0)
			return error;
	}

	/* Succeeded: every step passed. */
	r->t.outcome = I915_TEST_EU_PASS;
	return 0;
}

/*
 * Prepares one step of the mixed test.
 *
 * Creates the step's context when it is new, then rewrites the whole fixture
 * page for the step's kind and pre-fills the target for a draw.
 */
static int
i915_test_r1_step_prepare(
	struct i915_test_r1 *r,
	struct i915_test_draw_env *env,
	unsigned s)
{
	struct i915_test_eu *t;
	struct i915_test_r1_step *st;
	struct i915_test_r1_ctx *cx;
	volatile uint32_t *px;
	uint64_t hash;
	uint64_t expected;
	unsigned i;
	int error;

	t = &r->t;
	st = &r->step[s];
	cx = &r->ctx[(unsigned)(i915_test_r1_plan_ctx[s] - 'A')];

	/* Starts the step's record. */
	kern_memset(st, 0, sizeof(*st));
	r->n_steps = s + 1U;
	st->ctx = i915_test_r1_plan_ctx[s];
	st->kind = i915_test_r1_plan_kind[s];

	/* Creates a new context and a new timeline for the first step on it. */
	if (!cx->created) {
		error = i915_draw_context_create(
			t,
			&cx->ce,
			&cx->tl_page,
			env,
			"r1: intel_context_create",
			"r1: intel_context_create",
			"r1: intel_context_create");
		if (cx->ce.allocated)
			cx->created = 1;

		/* Records the failed creation in the step. */
		if (error != 0) {
			st->rc = error;
			return error;
		}
	}

	/*
	 * The previous request has completed and parked, so the CPU may now
	 * rewrite what it referenced: the whole fixture page for this kind, and
	 * the render target with a pattern that is not the expected one.
	 */
	if (st->kind == 'D') {
		drv_i915_draw_fixture_write_state(t->shared->cpu, I915_TEST_DRAW_RT_VA, r->mocs);
		px = (volatile uint32_t *)r->rt->cpu;
		for (i = 0U; i < I915_TEST_RT_PIXELS; i++)
			px[i] = I915_TEST_RT_PREFILL;
	} else {
		i915_r1_write_c1_state(t->shared->cpu);
	}

	/* Hashes the state page and the step's batch as submitted. */
	st->state_hash = drv_i915_test_fnv1a64(t->shared->cpu, 4096U, I915_TEST_FNV_BASIS);
	if (st->kind == 'D') {
		hash = drv_i915_test_fnv1a64(r->dbatch->cpu, (size_t)r->dbatch_dwords * 4U, I915_TEST_FNV_BASIS);
		expected = r->draw_batch_hash;
	} else {
		hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);
		expected = r->c1_batch_hash;
	}

	st->batch_hash = hash;

	/* Refuses a batch that changed since it was verified. */
	if (hash != expected) {
		st->rc = EINVAL;
		(void)drv_i915_test_eu_fail(t, EINVAL, "r1: batch changed");
		return EINVAL;
	}

	/* Succeeded: the step can be submitted. */
	return 0;
}

/*
 * Submits one step of the mixed test and waits for it.
 *
 * The wait's result goes to wait_rc and into the step; a request that could
 * not be built or submitted is reported as the error.
 */
static int
i915_test_r1_step_submit(
	struct i915_test_r1 *r,
	struct i915_test_draw_env *env,
	unsigned s,
	int *wait_rc)
{
	struct i915_test_eu *t;
	struct i915_test_r1_step *st;
	struct i915_test_r1_ctx *cx;
	uint64_t batch_va;
	unsigned polls0;
	int error;

	t = &r->t;
	st = &r->step[s];
	cx = &r->ctx[(unsigned)(st->ctx - 'A')];
	polls0 = t->polls;

	/* A draw starts the draw batch, the compute request the compute batch. */
	batch_va = I915_TEST_EU_BATCH_VA;
	if (st->kind == 'D')
		batch_va = I915_TEST_R1_DRAW_BATCH_VA;

	/* Builds the step's request on the next seqno of its context's timeline. */
	cx->seqno += 2U;
	st->seqno = cx->seqno;
	st->lrca = cx->ce.lrca;
	error = drv_i915_test_eu_build_request(t, &r->rq, &cx->ce, cx->tl_page, cx->seqno, batch_va);
	if (error != 0) {
		st->rc = error;
		return error;
	}

	/* Hands the request to the engine. */
	error = drv_i915_execlists_submit(env->ge, env->el, env->m, &r->rq);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, "r1: execlists_submit");
		st->rc = error;
		return error;
	}

	/* Waits for the request and records how it ended. */
	*wait_rc = drv_i915_test_eu_wait_retired(t, env->ge, env->el, &r->rq, env->m, env->timeout_ms);
	st->rc = *wait_rc;
	st->completed = 0;
	if (*wait_rc == 0)
		st->completed = 1;

	st->polls = t->polls - polls0;
	st->hwsp_observed = *r->rq.hwsp_cpu;
	st->lrca = cx->ce.lrca;

	/* Succeeded: the request was submitted; the step says how it ended. */
	return 0;
}

/* Reads what one step of the mixed test wrote. */
static void
i915_test_r1_step_read(
	struct i915_test_r1 *r,
	unsigned s)
{
	struct i915_test_r1_step *st;
	volatile uint32_t *page;
	volatile uint32_t *mk;
	volatile uint32_t *px;
	unsigned i;

	st = &r->step[s];
	page = (volatile uint32_t *)r->t.shared->cpu;
	px = (volatile uint32_t *)r->rt->cpu;

	/* Reads a draw's markers and counts its pixels. */
	if (st->kind == 'D') {
		mk = page + I915_DRAW_FIXTURE_MARKER_OFFSET / 4U;
		st->before = mk[0];
		st->after = mk[1];
		st->middraw = mk[2];
		st->ps_marker = mk[4];
		for (i = 0U; i < I915_TEST_RT_PIXELS; i++) {
			if (px[i] == I915_DRAW_FIXTURE_EXPECTED_PIXEL) {
				st->px_match++;
			} else if (px[i] == I915_TEST_RT_PREFILL) {
				st->px_stale++;
			}
		}

		st->px_first = px[0];
		st->px_last = px[I915_TEST_RT_PIXELS - 1U];
		return;
	}

	/* Reads the compute request's markers. */
	st->ready = page[I915_TEST_EU_READY_OFF / 4U];
	st->eu = page[I915_TEST_EU_EU_OFF / 4U];
	st->done = page[I915_TEST_EU_DONE_OFF / 4U];
	st->cs = page[I915_TEST_EU_CS_OFF / 4U];

	/* Checks that the GPU read back the descriptor the CPU wrote. */
	st->idd_rb_ok = 1;
	for (i = 0U; i < 8U; i++) {
		if (page[I915_TEST_EU_IDD_RB_OFF / 4U + i] != page[I915_TEST_EU_IDD_OFFSET / 4U + i])
			st->idd_rb_ok = 0;
	}

	/* Checks that the GPU read back the kernel the CPU wrote. */
	st->kernel_rb_ok = 1;
	for (i = 0U; i < I915_TEST_EU_KERNEL_DWORDS; i++) {
		if (page[I915_TEST_EU_KERNEL_RB_OFF / 4U + i] != drv_i915_test_eu_kernel[i])
			st->kernel_rb_ok = 0;
	}
}

/*
 * Decides one step of the mixed test.
 *
 * A request that did not complete is recorded and the engines are reset;
 * a completed one parks the engine and is compared with the fixture.
 */
static int
i915_test_r1_step_finish(
	struct i915_test_r1 *r,
	struct i915_test_draw_env *env,
	unsigned s,
	int wait_rc)
{
	struct i915_test_eu *t;
	struct i915_test_r1_step *st;
	struct i915_test_r1_ctx *cx;

	t = &r->t;
	st = &r->step[s];
	cx = &r->ctx[(unsigned)(st->ctx - 'A')];

	/* Records a request that did not complete and resets the engines. */
	if (!st->completed) {
		if (wait_rc == ETIMEDOUT) {
			t->timed_out = 1;
			t->outcome = I915_TEST_EU_HANG;
		} else {
			(void)drv_i915_test_eu_fail(t, wait_rc, "r1: i915_request_wait");
		}

		drv_i915_test_eu_log_record(t, env->ge, env->el, &r->rq, &cx->ce, "r1-hang");
		drv_i915_test_eu_hang_dump_reset(t, env->es, env->ge, env->el, env->m, env->uncore_lock);
		return wait_rc;
	}

	/* Switches the engine back to its kernel context. */
	st->parked = drv_i915_test_eu_park(t, env->es, env->ge, env->el, env->m, env->timeout_ms);

	/* A draw passed when the markers and every pixel are the promised ones. */
	st->pass = 0;
	if (st->kind == 'D') {
		if (st->parked &&
		    st->hwsp_observed == st->seqno &&
		    st->before == I915_DRAW_FIXTURE_MARKER_BEFORE &&
		    st->middraw == I915_DRAW_FIXTURE_MARKER_MIDDRAW &&
		    st->after == I915_DRAW_FIXTURE_MARKER_AFTER &&
		    st->ps_marker == I915_DRAW_FIXTURE_PS_MARKER &&
		    st->px_match == I915_TEST_RT_PIXELS)
			st->pass = 1;
	} else {
		/* The compute request passed when every marker landed and both readbacks match. */
		if (st->parked &&
		    st->hwsp_observed == st->seqno &&
		    st->ready == I915_TEST_EU_READY_TAG &&
		    st->eu == I915_TEST_EU_STORE_TAG &&
		    st->done == I915_TEST_EU_DONE_TAG &&
		    st->cs == I915_TEST_EU_CS_TAG &&
		    st->idd_rb_ok &&
		    st->kernel_rb_ok)
			st->pass = 1;
	}

	/* Ends the test at the first step that did not pass. */
	if (!st->pass) {
		t->outcome = I915_TEST_EU_ERROR;
		(void)drv_i915_test_eu_fail(t, EIO, "r1: markers/pixels");
		return EIO;
	}

	/* Succeeded: the step passed. */
	r->passed++;
	return 0;
}

/* Creates and maps a texture plan's objects and builds its one batch. */
static int
i915_test_t3_setup(
	struct i915_test_t3 *x,
	struct i915_test_draw_env *env)
{
	struct i915_test_eu *t;
	struct i915_gt_object *objects[5];
	unsigned i;
	int error;

	t = &x->t;

	/* Finds the engine the draws run on. */
	error = i915_draw_find_engine(t, env);
	if (error != 0)
		return error;

	/* Creates the state page, the batch, the render target and both textures. */
	error = i915_draw_object_create(t, env->gm, &t->shared);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &t->batch);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &x->rt);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &x->tex_a);
	if (error != 0)
		return error;

	error = i915_draw_object_create(t, env->gm, &x->tex_b);
	if (error != 0)
		return error;

	/* Maps the five objects; the range covers 0x100400000..0x100405fff. */
	objects[0] = t->shared;
	objects[1] = t->batch;
	objects[2] = x->rt;
	objects[3] = x->tex_a;
	objects[4] = x->tex_b;
	error = i915_draw_map_objects(t, env, objects, i915_test_t3_va, 5U, 6U);
	if (error != 0)
		return error;

	/* Generates the four images and uploads image 0 into both textures. */
	x->mocs = drv_i915_draw_fixture_mocs();
	for (i = 0U; i < I915_TEX_FIXTURE_VARIANTS; i++)
		drv_i915_tex_fixture_pattern(i915_t3_test_pattern[i], i);

	i915_draw_tex_upload(x->tex_a, i915_t3_test_pattern[0]);
	i915_draw_tex_upload(x->tex_b, i915_t3_test_pattern[0]);
	x->content[0] = 0;
	x->content[1] = 0;

	/* Builds one batch for every step: it names no texture, the binding table does. */
	t->batch_dwords = drv_i915_tex_fixture_build_batch(
		(uint32_t *)t->batch->cpu,
		I915_TEST_BATCH_CAPACITY,
		I915_TEST_EU_SHARED_VA,
		x->mocs);
	if (t->batch_dwords == 0U || t->batch_dwords >= I915_TEST_BATCH_CAPACITY) {
		(void)drv_i915_test_eu_fail(t, ENOSPC, "build_batch");
		return ENOSPC;
	}

	/* Reads the select words back from the submitted object; a bad select is never submitted. */
	t->pipesel_rc = drv_i915_test_draw_check_pipeline_select((const uint32_t *)t->batch->cpu, t->batch_dwords, &t->pipesel);
	if (t->pipesel_rc != 0) {
		(void)drv_i915_test_eu_fail(t, t->pipesel_rc, "pipeline_select_verify");
		return t->pipesel_rc;
	}

	/* Records the batch's identity. */
	t->batch_hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);

	/* Succeeded: the plan can run. */
	return 0;
}

/*
 * Runs a texture plan.
 *
 * Each step waits for its request and parks the engine before the CPU
 * touches what the next step references.
 */
static int
i915_test_t3_run_plan(
	struct i915_test_t3 *x,
	struct i915_test_draw_env *env,
	const struct i915_test_t3_plan_row *plan,
	unsigned n_plan)
{
	unsigned s;
	int error;
	int wait_rc;

	/* Starts from an empty record. */
	kern_memset(x, 0, sizeof(*x));
	x->n_planned = n_plan;

	/* Creates the objects and the batch. */
	error = i915_test_t3_setup(x, env);
	if (error != 0)
		return error;

	/* Runs every step in order; the first failed step ends the plan. */
	for (s = 0U; s < x->n_planned; s++) {
		/* Creates the step's context when it is new and rewrites its fixture. */
		error = i915_test_t3_step_prepare(x, env, &plan[s], s);
		if (error != 0)
			return error;

		/* Submits the step and waits for it. */
		error = i915_test_t3_step_submit(x, env, s, &wait_rc);
		if (error != 0)
			return error;

		/* Reads what the step wrote and decides it. */
		i915_test_t3_step_read(x, s);
		error = i915_test_t3_step_finish(x, env, s, wait_rc);
		if (error != 0)
			return error;
	}

	/* Succeeded: every step passed. */
	x->t.outcome = I915_TEST_EU_PASS;
	return 0;
}

/*
 * Prepares one step of a texture plan.
 *
 * The previous request has completed and parked: only now does the CPU
 * touch what it referenced.  The step's upload, then the state page with
 * the step's binding and filter, then the render target's pre-fill.
 */
static int
i915_test_t3_step_prepare(
	struct i915_test_t3 *x,
	struct i915_test_draw_env *env,
	const struct i915_test_t3_plan_row *row,
	unsigned s)
{
	struct i915_test_eu *t;
	struct i915_test_t3_step *st;
	struct i915_test_r1_ctx *cx;
	volatile uint32_t *px;
	uint64_t hash;
	unsigned bind_b;
	unsigned i;
	int error;

	t = &x->t;
	st = &x->step[s];
	cx = &x->ctx[(unsigned)(row->ctx - 'A')];

	/* Starts the step's record. */
	kern_memset(st, 0, sizeof(*st));
	x->n_steps = s + 1U;
	st->ctx = row->ctx;
	st->bind = row->bind;
	st->upload = row->upload;
	st->upload_variant = row->variant;
	st->first_bad_x = -1;
	st->first_bad_y = -1;

	/* Creates a new context and a new timeline for the first step on it. */
	if (!cx->created) {
		error = i915_draw_context_create(
			t,
			&cx->ce,
			&cx->tl_page,
			env,
			"t3: intel_context_create",
			"t3: intel_context_create",
			"t3: intel_context_create");
		if (cx->ce.allocated)
			cx->created = 1;

		/* Records the failed creation in the step. */
		if (error != 0) {
			st->rc = error;
			return error;
		}
	}

	/* Rewrites the texture this step uploads, and remembers what it now holds. */
	if (row->upload == 'A') {
		i915_draw_tex_upload(x->tex_a, i915_t3_test_pattern[row->variant]);
		x->content[0] = row->variant;
	} else if (row->upload == 'B') {
		i915_draw_tex_upload(x->tex_b, i915_t3_test_pattern[row->variant]);
		x->content[1] = row->variant;
	}

	/* Expects the image the bound texture holds, under the step's filter. */
	bind_b = 0U;
	if (row->bind == 'B')
		bind_b = 1U;

	st->expect_variant = x->content[bind_b];
	st->linear = row->linear;

	/* Writes the state page with this step's binding and filter, and pre-fills the target. */
	drv_i915_tex_fixture_write_state_ab_filter(
		t->shared->cpu,
		I915_TEST_DRAW_RT_VA,
		I915_TEX_FIXTURE_TEX_VA,
		I915_TEX_FIXTURE_TEX_B_VA,
		bind_b,
		(unsigned)row->linear,
		x->mocs);
	px = (volatile uint32_t *)x->rt->cpu;
	for (i = 0U; i < I915_TEST_RT_PIXELS; i++)
		px[i] = I915_TEST_RT_PREFILL;

	/* Hashes the state page and both images as submitted. */
	st->state_hash = drv_i915_test_fnv1a64(t->shared->cpu, 4096U, I915_TEST_FNV_BASIS);
	st->tex_a_hash = drv_i915_test_fnv1a64(x->tex_a->cpu, I915_TEX_FIXTURE_TEX_BYTES, I915_TEST_FNV_BASIS);
	st->tex_b_hash = drv_i915_test_fnv1a64(x->tex_b->cpu, I915_TEX_FIXTURE_TEX_BYTES, I915_TEST_FNV_BASIS);

	/* Refuses a batch that changed since it was verified. */
	hash = drv_i915_test_fnv1a64(t->batch->cpu, (size_t)t->batch_dwords * 4U, I915_TEST_FNV_BASIS);
	if (hash != t->batch_hash) {
		st->rc = EINVAL;
		(void)drv_i915_test_eu_fail(t, EINVAL, "t3: batch changed");
		return EINVAL;
	}

	/* Succeeded: the step can be submitted. */
	return 0;
}

/*
 * Submits one step of a texture plan and waits for it.
 *
 * The wait's result goes to wait_rc and into the step; a request that could
 * not be built or submitted is reported as the error.
 */
static int
i915_test_t3_step_submit(
	struct i915_test_t3 *x,
	struct i915_test_draw_env *env,
	unsigned s,
	int *wait_rc)
{
	struct i915_test_eu *t;
	struct i915_test_t3_step *st;
	struct i915_test_r1_ctx *cx;
	unsigned polls0;
	int error;

	t = &x->t;
	st = &x->step[s];
	cx = &x->ctx[(unsigned)(st->ctx - 'A')];
	polls0 = t->polls;

	/* Builds the step's request on the next seqno of its context's timeline. */
	cx->seqno += 2U;
	st->seqno = cx->seqno;
	error = drv_i915_test_eu_build_request(t, &x->rq, &cx->ce, cx->tl_page, cx->seqno, I915_TEST_EU_BATCH_VA);
	if (error != 0) {
		st->rc = error;
		return error;
	}

	/* Hands the request to the engine. */
	error = drv_i915_execlists_submit(env->ge, env->el, env->m, &x->rq);
	if (error != 0) {
		(void)drv_i915_test_eu_fail(t, error, "t3: execlists_submit");
		st->rc = error;
		return error;
	}

	/* Waits for the request and records how it ended. */
	*wait_rc = drv_i915_test_eu_wait_retired(t, env->ge, env->el, &x->rq, env->m, env->timeout_ms);
	st->rc = *wait_rc;
	st->completed = 0;
	if (*wait_rc == 0)
		st->completed = 1;

	st->polls = t->polls - polls0;
	st->hwsp_observed = *x->rq.hwsp_cpu;
	st->lrca = cx->ce.lrca;

	/* Succeeded: the request was submitted; the step says how it ended. */
	return 0;
}

/* Reads what one step of a texture plan wrote. */
static void
i915_test_t3_step_read(
	struct i915_test_t3 *x,
	unsigned s)
{
	struct i915_test_t3_step *st;
	unsigned changed_a;
	unsigned changed_b;

	st = &x->step[s];

	/* Reads the markers and compares the pixels with the bound image. */
	i915_draw_read_markers(&x->t, &st->before, &st->middraw, &st->after, &st->ps_marker);
	i915_test_t3_compare_pixels(st, (const volatile uint32_t *)x->rt->cpu, i915_t3_test_pattern[st->expect_variant]);

	/* Hashes the target and counts the texel and guard bytes the draw changed. */
	st->rt_hash = drv_i915_test_fnv1a64(x->rt->cpu, 4096U, I915_TEST_FNV_BASIS);
	changed_a = i915_draw_tex_diff(x->tex_a, i915_t3_test_pattern[x->content[0]], &st->guard_bad_bytes);
	changed_b = i915_draw_tex_diff(x->tex_b, i915_t3_test_pattern[x->content[1]], &st->guard_bad_bytes);
	st->tex_changed_bytes = changed_a + changed_b;
}

/* Compares a texture plan step's target with the bound image under the step's filter. */
static void
i915_test_t3_compare_pixels(
	struct i915_test_t3_step *st,
	const volatile uint32_t *px,
	const uint8_t *want_img)
{
	unsigned i;
	unsigned chn;
	uint32_t near;
	uint32_t want;
	uint32_t got;
	int dd;

	/* Compares every pixel of the target. */
	for (i = 0U; i < I915_TEST_RT_PIXELS; i++) {
		near = drv_i915_tex_fixture_expected_pixel(want_img, i % 32U, i / 32U);
		if (st->linear) {
			want = drv_i915_tex_fixture_expected_pixel_linear(want_img, i % 32U, i / 32U, NULL);
		} else {
			want = near;
		}

		got = px[i];

		/* Counts the pixels where the filter changes the expectation. */
		if (want != near)
			st->differs_from_nearest++;

		/* Records the largest channel difference of a written pixel, for diagnosis only. */
		for (chn = 0U; chn < 4U; chn++) {
			dd = (int)((got >> (8U * chn)) & 255U) - (int)((want >> (8U * chn)) & 255U);
			if (dd < 0)
				dd = -dd;

			/* Keeps the largest difference. */
			if ((unsigned)dd > st->max_channel_diff && got != I915_TEST_RT_PREFILL)
				st->max_channel_diff = (unsigned)dd;
		}

		/* Counts a match, or records a stale or first wrong pixel. */
		if (got == want) {
			st->px_match++;
		} else {
			if (got == I915_TEST_RT_PREFILL)
				st->px_stale++;

			/* Keeps the first wrong pixel for the log. */
			if (st->first_bad_x < 0) {
				st->first_bad_x = (int)(i % 32U);
				st->first_bad_y = (int)(i / 32U);
				st->first_bad_expected = want;
				st->first_bad_observed = got;
			}
		}
	}
}

/*
 * Decides one step of a texture plan.
 *
 * A request that did not complete is recorded and the engines are reset;
 * a completed one parks the engine and is compared with the fixture.
 */
static int
i915_test_t3_step_finish(
	struct i915_test_t3 *x,
	struct i915_test_draw_env *env,
	unsigned s,
	int wait_rc)
{
	struct i915_test_eu *t;
	struct i915_test_t3_step *st;
	struct i915_test_r1_ctx *cx;

	t = &x->t;
	st = &x->step[s];
	cx = &x->ctx[(unsigned)(st->ctx - 'A')];

	/* Records a request that did not complete and resets the engines. */
	if (!st->completed) {
		if (wait_rc == ETIMEDOUT) {
			t->timed_out = 1;
			t->outcome = I915_TEST_EU_HANG;
		} else {
			(void)drv_i915_test_eu_fail(t, wait_rc, "t3: i915_request_wait");
		}

		drv_i915_test_eu_log_record(t, env->ge, env->el, &x->rq, &cx->ce, "t3-hang");
		drv_i915_test_eu_hang_dump_reset(t, env->es, env->ge, env->el, env->m, env->uncore_lock);
		return wait_rc;
	}

	/* Switches the engine back to its kernel context. */
	st->parked = drv_i915_test_eu_park(t, env->es, env->ge, env->el, env->m, env->timeout_ms);

	/* The step passed when markers, pixels, textures and guards are the promised ones. */
	st->pass = 0;
	if (st->parked &&
	    st->hwsp_observed == st->seqno &&
	    st->before == I915_DRAW_FIXTURE_MARKER_BEFORE &&
	    st->middraw == I915_DRAW_FIXTURE_MARKER_MIDDRAW &&
	    st->after == I915_DRAW_FIXTURE_MARKER_AFTER &&
	    st->ps_marker == I915_DRAW_FIXTURE_PS_MARKER &&
	    st->px_match == I915_TEST_RT_PIXELS &&
	    st->tex_changed_bytes == 0U &&
	    st->guard_bad_bytes == 0U)
		st->pass = 1;

	/* Ends the plan at the first step that did not pass. */
	if (!st->pass) {
		t->outcome = I915_TEST_EU_ERROR;
		(void)drv_i915_test_eu_fail(t, EIO, "t3: markers/pixels/guard");
		return EIO;
	}

	/* Succeeded: the step passed. */
	x->passed++;
	return 0;
}

/*
 * Runs one texture plan as a scenario.
 *
 * Takes forcewake, runs the plan, logs every step and the last step's
 * objects under last_tag, and gives back what the plan created.
 */
static int
i915_test_t3_scenario(
	struct i915_device *device,
	const char *tag,
	const char *last_tag,
	const struct i915_test_t3_plan_row *plan,
	unsigned n_plan)
{
	struct i915_test_draw_env env;
	struct i915_test_irq_snapshot irq;
	struct i915_test_t3 *x;
	struct i915_gt_object **objects[3];
	unsigned held;
	int error;
	int run;
	int released;
	int passed;
	int idle;
	int completed;
	int parked;

	x = &i915_t3_test;

	/* Takes the five forcewake domains the GT tests run under. */
	held = 0U;
	error = drv_i915_test_forcewake_get_all(device, &held);
	if (error != 0) {
		kern_logf("i915: %s not run: forcewake failed rc=%d\n", tag, error);
		drv_i915_test_forcewake_put_all(device, held);
		return error;
	}

	/* Runs the plan and logs every step. */
	i915_draw_env_init(&env, device);
	i915_draw_irq_snapshot(device, &irq);
	run = i915_test_t3_run_plan(x, &env, plan, n_plan);
	i915_test_t3_log(device, x, tag, last_tag, run, &irq);

	/* The plan passes when every planned step passed. */
	passed = 0;
	if (run == 0 && x->passed == x->n_planned)
		passed = 1;

	/* The last step says whether a request may still hold the engine. */
	completed = 0;
	parked = 0;
	if (x->n_steps != 0U) {
		completed = x->step[x->n_steps - 1U].completed;
		parked = x->step[x->n_steps - 1U].parked;
	}

	/* Gives back what the plan created once the GPU is done with it. */
	idle = i915_draw_gpu_idle(&x->t, completed, parked);
	objects[0] = &x->rt;
	objects[1] = &x->tex_a;
	objects[2] = &x->tex_b;
	released = i915_draw_release(device, &x->t, tag, idle, x->ctx, 2U, objects, 3U);

	/* Puts back the forcewake domains. */
	drv_i915_test_forcewake_put_all(device, held);

	/* Reports the verdict, or a release that left the device in doubt. */
	error = i915_draw_scenario_result(passed, run, released);
	if (error != 0)
		return error;

	/* Succeeded: every step passed and the device is clean. */
	return 0;
}

/* Logs what the GPU walks for each fixture address. */
static void
i915_draw_log_walks(
	const char *tag,
	const struct i915_test_eu *t,
	int with_va)
{
	const struct i915_test_ppgtt_walk *w;
	unsigned i;

	/* Logs one line per walked address. */
	for (i = 0U; i < t->walks; i++) {
		w = &t->walk[i];

		/* The textured draw's form names the address; the single-colour draw's does not. */
		if (with_va) {
			kern_logf("i915: %s walk[%u] va=0x%llx levels=%d leaf=0x%llx present=%d rw=%d pat=%u scr=%d\n",
				  tag,
				  i,
				  (unsigned long long)w->va,
				  w->levels,
				  (unsigned long long)w->leaf_dma,
				  w->leaf_present,
				  w->leaf_rw,
				  w->leaf_pat,
				  w->scratch[3]);
		} else {
			kern_logf("i915: %s walk[%u] levels=%d leaf=0x%llx present=%d rw=%d pat=%u scr=%d\n",
				  tag,
				  i,
				  w->levels,
				  (unsigned long long)w->leaf_dma,
				  w->leaf_present,
				  w->leaf_rw,
				  w->leaf_pat,
				  w->scratch[3]);
		}
	}
}

/* Logs the submitted batch, eight dwords per line. */
static void
i915_draw_log_batch(
	const char *tag,
	const struct i915_test_eu *t)
{
	const uint32_t *bd;
	unsigned i;

	/* Logs nothing when no batch was built. */
	if (t->batch == NULL || t->batch_dwords == 0U)
		return;

	/* Logs the batch words; the page is larger than any batch, so the last line stays inside it. */
	bd = (const uint32_t *)t->batch->cpu;
	for (i = 0U; i < t->batch_dwords; i += 8U) {
		kern_logf("i915: %s batch[%03u]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  tag,
			  i,
			  bd[i],
			  bd[i + 1U],
			  bd[i + 2U],
			  bd[i + 3U],
			  bd[i + 4U],
			  bd[i + 5U],
			  bd[i + 6U],
			  bd[i + 7U]);
	}
}

/* Logs the state page as the GPU saw it, skipping rows that are all zero. */
static void
i915_draw_log_state(
	const char *tag,
	const struct i915_test_eu *t)
{
	const uint32_t *sp;
	uint32_t row_bits;
	unsigned i;

	/* Logs nothing when no state page exists. */
	if (t->shared == NULL)
		return;

	/* Logs each row of eight dwords that holds something. */
	sp = (const uint32_t *)t->shared->cpu;
	for (i = 0U; i < 1024U; i += 8U) {
		row_bits = sp[i] | sp[i + 1U] | sp[i + 2U] | sp[i + 3U] | sp[i + 4U] | sp[i + 5U] | sp[i + 6U] | sp[i + 7U];
		if (row_bits == 0U)
			continue;

		kern_logf("i915: %s state[%04u]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  tag,
			  i,
			  sp[i],
			  sp[i + 1U],
			  sp[i + 2U],
			  sp[i + 3U],
			  sp[i + 4U],
			  sp[i + 5U],
			  sp[i + 6U],
			  sp[i + 7U]);
	}
}

/* Logs a texture's 256-byte image. */
static void
i915_draw_log_tex(
	const char *tag,
	const struct i915_gt_object *tex)
{
	const uint32_t *tp;
	unsigned i;

	/* Logs nothing when no texture exists. */
	if (tex == NULL)
		return;

	/* Logs the image, eight dwords per line. */
	tp = (const uint32_t *)tex->cpu;
	for (i = 0U; i < 64U; i += 8U) {
		kern_logf("i915: %s tex[%02u]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  tag,
			  i,
			  tp[i],
			  tp[i + 1U],
			  tp[i + 2U],
			  tp[i + 3U],
			  tp[i + 4U],
			  tp[i + 5U],
			  tp[i + 6U],
			  tp[i + 7U]);
	}
}

/* Logs the 32x32 render target. */
static void
i915_draw_log_rt(
	const char *tag,
	const struct i915_gt_object *rt)
{
	const uint32_t *rp;
	unsigned i;

	/* Logs nothing when no target exists. */
	if (rt == NULL)
		return;

	/* Logs the pixels, eight per line. */
	rp = (const uint32_t *)rt->cpu;
	for (i = 0U; i < I915_TEST_RT_PIXELS; i += 8U) {
		kern_logf("i915: %s rt[%04u]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			  tag,
			  i,
			  rp[i],
			  rp[i + 1U],
			  rp[i + 2U],
			  rp[i + 3U],
			  rp[i + 4U],
			  rp[i + 5U],
			  rp[i + 6U],
			  rp[i + 7U]);
	}
}

/* Logs the pipeline statistics a hang record read. */
static void
i915_draw_log_stats(
	const char *tag,
	const uint32_t *stats_live,
	int stats_valid)
{
	/* Logs nothing when the request did not hang. */
	if (!stats_valid)
		return;

	/* Logs the six counters. */
	kern_logf("i915: %s stats(live, before reset): ia_vertices=%u ia_primitives=%u "
		  "vs_invocations=%u cl_invocations=%u cl_primitives=%u ps_invocations=%u\n",
		  tag,
		  stats_live[0],
		  stats_live[1],
		  stats_live[2],
		  stats_live[3],
		  stats_live[4],
		  stats_live[5]);
}

/* Names the engine a test ran on. */
static const char *
i915_draw_engine_name(
	struct i915_device *device,
	const struct i915_test_eu *t)
{
	struct i915_gt_engine *ge;

	/* An engine table without the index has no name to give. */
	if (t->engine_idx >= device->gt.engines.n)
		return "-";

	/* Reports the engine's name. */
	ge = &device->gt.engines.ge[t->engine_idx];
	return ge->info->name;
}

/* Logs the single-colour draw: fixture, walks, batch, state page, statistics and verdict. */
static void
i915_test_draw_log(
	struct i915_device *device,
	const struct i915_test_draw *d,
	int run,
	const struct i915_test_irq_snapshot *irq)
{
	const struct i915_test_eu *dt;
	const char *where;

	dt = &d->t;

	/* Logs the fixture's identity and the select words read from the submitted object. */
	kern_logf("i915: DRAW-TEST fixture: batch_hash=%016llx "
		  "state_hash=%016llx batch_dwords=%u mocs=%u pdp0_matches_top=%d | pipeline_select "
		  "(read from the submitted object): rc=%d 3d=%u@%u gpgpu=%u bad=%u@%u bad_word=%08x\n",
		  (unsigned long long)dt->batch_hash,
		  (unsigned long long)d->state_hash,
		  dt->batch_dwords,
		  d->mocs,
		  dt->pdp0_matches_top,
		  dt->pipesel_rc,
		  dt->pipesel.n_3d,
		  dt->pipesel.idx_3d,
		  dt->pipesel.n_gpgpu,
		  dt->pipesel.n_bad,
		  dt->pipesel.idx_bad,
		  dt->pipesel.bad_word);

	/* Logs the walks, the batch, the state page and a hang's statistics. */
	i915_draw_log_walks("DRAW-TEST", dt, 0);
	i915_draw_log_batch("DRAW-TEST", dt);
	i915_draw_log_state("DRAW-TEST", dt);
	i915_draw_log_stats("DRAW-TEST", d->stats_live, d->stats_valid);

	/* Logs the verdict. */
	where = "-";
	if (dt->err_where != NULL)
		where = dt->err_where;

	kern_logf("i915: DRAW-TEST %s: rc=%d where=%s engine=%s batch_dwords=%u submitted=%d "
		  "completed=%d parked=%d timed_out=%d wedged=%d polls=%u | before=%08x middraw=%08x after=%08x "
		  "ps_marker=%08x | pixels match=%u/%u first=%08x mid=%08x last=%08x expected=%08x | "
		  "rq seqno=%u krq seqno=%u | gt irq: user=%u ctx_switch=%u error=%u\n",
		  drv_i915_test_eu_outcome_name(dt->outcome),
		  run,
		  where,
		  i915_draw_engine_name(device, dt),
		  dt->batch_dwords,
		  dt->submitted,
		  dt->completed,
		  dt->parked,
		  dt->timed_out,
		  dt->wedged,
		  dt->polls,
		  d->marker_before,
		  d->marker_middraw,
		  d->marker_after,
		  d->ps_marker,
		  d->px_match,
		  d->px_total,
		  d->px_first,
		  d->px_mid,
		  d->px_last,
		  I915_DRAW_FIXTURE_EXPECTED_PIXEL,
		  dt->rq.seqno,
		  dt->krq.seqno,
		  device->gt.irq.gt_user_intr - irq->user,
		  device->gt.irq.gt_ctx_switch_intr - irq->ctx_switch,
		  device->gt.irq.gt_error_intr - irq->error);
}

/* Logs the textured draw: fixture, walks, batch, state page, texture, target, statistics and verdict. */
static void
i915_test_tex_log(
	struct i915_device *device,
	const struct i915_test_tex *x,
	int run,
	const struct i915_test_irq_snapshot *irq)
{
	const struct i915_test_eu *tt;
	const char *where;

	tt = &x->t;

	/* Logs the fixture's identity and the select words read from the submitted object. */
	kern_logf("i915: TEX-TEST fixture: batch_hash=%016llx state_hash=%016llx "
		  "tex_hash=%016llx batch_dwords=%u mocs=%u pdp0_matches_top=%d | pipeline_select (read from the "
		  "submitted object): rc=%d 3d=%u@%u gpgpu=%u bad=%u\n",
		  (unsigned long long)tt->batch_hash,
		  (unsigned long long)x->state_hash,
		  (unsigned long long)x->tex_hash,
		  tt->batch_dwords,
		  x->mocs,
		  tt->pdp0_matches_top,
		  tt->pipesel_rc,
		  tt->pipesel.n_3d,
		  tt->pipesel.idx_3d,
		  tt->pipesel.n_gpgpu,
		  tt->pipesel.n_bad);

	/* Logs the walks, the batch, the state page, the texture, the target and a hang's statistics. */
	i915_draw_log_walks("TEX-TEST", tt, 1);
	i915_draw_log_batch("TEX-TEST", tt);
	i915_draw_log_state("TEX-TEST", tt);
	i915_draw_log_tex("TEX-TEST", x->tex);
	i915_draw_log_rt("TEX-TEST", x->rt);
	i915_draw_log_stats("TEX-TEST", x->stats_live, x->stats_valid);

	/* Logs the verdict. */
	where = "-";
	if (tt->err_where != NULL)
		where = tt->err_where;

	kern_logf("i915: TEX-TEST %s: rc=%d where=%s engine=%s batch_dwords=%u submitted=%d completed=%d "
		  "parked=%d timed_out=%d wedged=%d polls=%u | before=%08x middraw=%08x after=%08x ps_marker=%08x | "
		  "pixels match=%u/%u stale=%u first_bad=(%d,%d) expected=%08x observed=%08x | texture changed_bytes=%u "
		  "guard_bad_bytes=%u | rq seqno=%u krq seqno=%u | gt irq: user=%u ctx_switch=%u error=%u\n",
		  drv_i915_test_eu_outcome_name(tt->outcome),
		  run,
		  where,
		  i915_draw_engine_name(device, tt),
		  tt->batch_dwords,
		  tt->submitted,
		  tt->completed,
		  tt->parked,
		  tt->timed_out,
		  tt->wedged,
		  tt->polls,
		  x->marker_before,
		  x->marker_middraw,
		  x->marker_after,
		  x->ps_marker,
		  x->px_match,
		  x->px_total,
		  x->px_stale,
		  x->first_bad_x,
		  x->first_bad_y,
		  x->first_bad_expected,
		  x->first_bad_observed,
		  x->tex_changed_bytes,
		  x->guard_bad_bytes,
		  tt->rq.seqno,
		  tt->krq.seqno,
		  device->gt.irq.gt_user_intr - irq->user,
		  device->gt.irq.gt_ctx_switch_intr - irq->ctx_switch,
		  device->gt.irq.gt_error_intr - irq->error);
}

/* Logs the mixed test: fixture, every step and the verdict. */
static void
i915_test_r1_log(
	struct i915_device *device,
	const struct i915_test_r1 *r,
	int run,
	const struct i915_test_irq_snapshot *irq)
{
	const struct i915_test_r1_step *st;
	const char *verdict;
	const char *where;
	unsigned i;

	/* Logs both batches' identity and the fixture addresses. */
	kern_logf("i915: R1 fixture: c1_batch_hash=%016llx (dwords=%u @0x%llx) "
		  "draw_batch_hash=%016llx (dwords=%u @0x%llx) state@0x%llx rt@0x%llx mocs=%u\n",
		  (unsigned long long)r->c1_batch_hash,
		  r->t.batch_dwords,
		  (unsigned long long)I915_TEST_EU_BATCH_VA,
		  (unsigned long long)r->draw_batch_hash,
		  r->dbatch_dwords,
		  (unsigned long long)I915_TEST_R1_DRAW_BATCH_VA,
		  (unsigned long long)I915_TEST_EU_SHARED_VA,
		  (unsigned long long)I915_TEST_DRAW_RT_VA,
		  r->mocs);

	/* Logs every step that ran, in the form of its kind. */
	for (i = 0U; i < r->n_steps; i++) {
		st = &r->step[i];
		if (st->kind == 'D') {
			kern_logf("i915: R1 step=%u ctx=%c kind=draw lrca=%08x seqno=%u hwsp_observed=%u rc=%d "
				  "completed=%d parked=%d pass=%d polls=%u state_hash=%016llx | before=%08x middraw=%08x "
				  "after=%08x ps_marker=%08x | pixels match=%u/1024 stale=%u first=%08x last=%08x\n",
				  i + 1U,
				  st->ctx,
				  st->lrca,
				  st->seqno,
				  st->hwsp_observed,
				  st->rc,
				  st->completed,
				  st->parked,
				  st->pass,
				  st->polls,
				  (unsigned long long)st->state_hash,
				  st->before,
				  st->middraw,
				  st->after,
				  st->ps_marker,
				  st->px_match,
				  st->px_stale,
				  st->px_first,
				  st->px_last);
		} else {
			kern_logf("i915: R1 step=%u ctx=%c kind=c1 lrca=%08x seqno=%u hwsp_observed=%u rc=%d "
				  "completed=%d parked=%d pass=%d polls=%u state_hash=%016llx | ready=%08x eu=%08x "
				  "done=%08x cs=%08x idd_rb_ok=%d kernel_rb_ok=%d\n",
				  i + 1U,
				  st->ctx,
				  st->lrca,
				  st->seqno,
				  st->hwsp_observed,
				  st->rc,
				  st->completed,
				  st->parked,
				  st->pass,
				  st->polls,
				  (unsigned long long)st->state_hash,
				  st->ready,
				  st->eu,
				  st->done,
				  st->cs,
				  st->idd_rb_ok,
				  st->kernel_rb_ok);
		}
	}

	/* Names the verdict: every step passed, the engines were reset, or something else failed. */
	verdict = "ERROR";
	if (run == 0 && r->passed == r->n_planned) {
		verdict = "PASS";
	} else if (r->t.wedged) {
		verdict = "HANG";
	}

	/* Logs the verdict. */
	where = "-";
	if (r->t.err_where != NULL)
		where = r->t.err_where;

	kern_logf("i915: R1 %s: rc=%d where=%s steps=%u/%u passed=%u wedged=%d polls=%u | "
		  "gt irq: user=%u ctx_switch=%u error=%u\n",
		  verdict,
		  run,
		  where,
		  r->n_steps,
		  r->n_planned,
		  r->passed,
		  r->t.wedged,
		  r->t.polls,
		  device->gt.irq.gt_user_intr - irq->user,
		  device->gt.irq.gt_ctx_switch_intr - irq->ctx_switch,
		  device->gt.irq.gt_error_intr - irq->error);
}

/* Logs a texture plan: fixture, every step, the last step's objects and the verdict. */
static void
i915_test_t3_log(
	struct i915_device *device,
	const struct i915_test_t3 *x,
	const char *tag,
	const char *last_tag,
	int run,
	const struct i915_test_irq_snapshot *irq)
{
	const struct i915_test_t3_step *st;
	const char *verdict;
	const char *where;
	const char *filter;
	unsigned i;

	/* Logs the batch's identity, the fixture addresses and the select words. */
	kern_logf("i915: %s fixture: batch_hash=%016llx batch_dwords=%u mocs=%u "
		  "state@0x%llx batch@0x%llx rt@0x%llx texA@0x%llx texB@0x%llx | pipeline_select rc=%d 3d=%u gpgpu=%u bad=%u\n",
		  tag,
		  (unsigned long long)x->t.batch_hash,
		  x->t.batch_dwords,
		  x->mocs,
		  (unsigned long long)I915_TEST_EU_SHARED_VA,
		  (unsigned long long)I915_TEST_EU_BATCH_VA,
		  (unsigned long long)I915_TEST_DRAW_RT_VA,
		  (unsigned long long)I915_TEX_FIXTURE_TEX_VA,
		  (unsigned long long)I915_TEX_FIXTURE_TEX_B_VA,
		  x->t.pipesel_rc,
		  x->t.pipesel.n_3d,
		  x->t.pipesel.n_gpgpu,
		  x->t.pipesel.n_bad);

	/* Logs every step that ran. */
	for (i = 0U; i < x->n_steps; i++) {
		st = &x->step[i];
		filter = "nearest";
		if (st->linear)
			filter = "linear";

		kern_logf("i915: %s step=%u ctx=%c bind=%c upload=%c:%d expect_variant=%d filter=%s "
			  "differs_from_nearest=%u max_channel_diff=%u lrca=%08x seqno=%u "
			  "hwsp_observed=%u rc=%d completed=%d parked=%d pass=%d polls=%u | markers=%08x %08x %08x ps=%08x | "
			  "pixels match=%u/1024 stale=%u first_bad=(%d,%d) expected=%08x observed=%08x | tex changed=%u "
			  "guard_bad=%u | state_hash=%016llx rt_hash=%016llx texA_hash=%016llx texB_hash=%016llx\n",
			  tag,
			  i + 1U,
			  st->ctx,
			  st->bind,
			  st->upload,
			  st->upload_variant,
			  st->expect_variant,
			  filter,
			  st->differs_from_nearest,
			  st->max_channel_diff,
			  st->lrca,
			  st->seqno,
			  st->hwsp_observed,
			  st->rc,
			  st->completed,
			  st->parked,
			  st->pass,
			  st->polls,
			  st->before,
			  st->middraw,
			  st->after,
			  st->ps_marker,
			  st->px_match,
			  st->px_stale,
			  st->first_bad_x,
			  st->first_bad_y,
			  st->first_bad_expected,
			  st->first_bad_observed,
			  st->tex_changed_bytes,
			  st->guard_bad_bytes,
			  (unsigned long long)st->state_hash,
			  (unsigned long long)st->rt_hash,
			  (unsigned long long)st->tex_a_hash,
			  (unsigned long long)st->tex_b_hash);
	}

	/* Logs the last step's objects in the textured draw's dump form. */
	i915_draw_log_batch(last_tag, &x->t);
	i915_draw_log_state(last_tag, &x->t);
	i915_draw_log_tex(last_tag, x->tex_b);
	i915_draw_log_rt(last_tag, x->rt);
	kern_logf("i915: %s fixture: batch_dwords=%u\n", last_tag, x->t.batch_dwords);

	/* Names the verdict: every step passed, the engines were reset, or something else failed. */
	verdict = "ERROR";
	if (run == 0 && x->passed == x->n_planned) {
		verdict = "PASS";
	} else if (x->t.wedged) {
		verdict = "HANG";
	}

	/* Logs the verdict. */
	where = "-";
	if (x->t.err_where != NULL)
		where = x->t.err_where;

	kern_logf("i915: %s %s: rc=%d where=%s steps=%u/%u passed=%u wedged=%d polls=%u | "
		  "gt irq: user=%u ctx_switch=%u error=%u\n",
		  tag,
		  verdict,
		  run,
		  where,
		  x->n_steps,
		  x->n_planned,
		  x->passed,
		  x->t.wedged,
		  x->t.polls,
		  device->gt.irq.gt_user_intr - irq->user,
		  device->gt.irq.gt_ctx_switch_intr - irq->ctx_switch,
		  device->gt.irq.gt_error_intr - irq->error);
}

/*
 * Turns a scenario's verdict and release into its return value.
 *
 * A test that did not pass reports its run's error, or EIO when the run
 * recorded none; a passed test whose release failed reports that failure.
 */
static int
i915_draw_scenario_result(
	int passed,
	int run,
	int released)
{
	/* Reports a test that did not pass. */
	if (!passed) {
		if (run != 0)
			return run;

		return EIO;
	}

	/* Reports a release that left the device in doubt. */
	if (released != 0)
		return released;

	/* Succeeded: the test passed and its release completed. */
	return 0;
}
