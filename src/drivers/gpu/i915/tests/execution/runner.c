/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test runner of the i915 test build.
 *
 * Linked only into the test build, where it provides the checkpoint the
 * device start calls once the device has started and before its node is
 * served.  The build selects one scenario with the compile-time constant
 * I915_TEST_SCENARIO (a bare name, e.g. -DI915_TEST_SCENARIO=eu); the runner
 * finds it in its table, runs it on the start worker, logs one verdict line
 * and returns, so the node is published and served as in production.  A
 * scenario that needs the GPU to itself therefore runs before anything else
 * can submit.
 */

#include "ktest.h"
#include "scenarios.h"
#include "../display/scenarios.h"
#include "../render/scenarios.h"
#include <kern/kcrt.h>

#include "../../i915.h"

#include <kern/klog.h>

#include <stddef.h>

/* Turns the scenario's bare name into a string, after expanding the macro that carries it. */
#define I915_TEST_STRING(name)		#name
#define I915_TEST_NAME(name)		I915_TEST_STRING(name)

/* The scenario the build selected; an empty name when it selected none. */
#ifdef I915_TEST_SCENARIO
#define I915_TEST_SELECTED		I915_TEST_NAME(I915_TEST_SCENARIO)
#else
#define I915_TEST_SELECTED		""
#endif

/*
 * One scenario the runner can select.
 *
 * An execution scenario returns its verdict; a display scenario, or the
 * render scenario that runs once the node is served, logs its own and
 * returns nothing.  Exactly one of the two functions is set.
 */
struct i915_test_scenario {
	/* The name I915_TEST_SCENARIO selects it by. */
	const char *name;

	/* The execution scenario, which reports 0 for a pass. */
	int (*execution)(struct i915_device *device);

	/* The display or render scenario, which logs its own verdict. */
	void (*display)(struct i915_device *device);
};

void drv_i915_test_after_start(struct i915_device *device);

/*
 * The display scenarios.
 *
 * The display tests define them in their own files.  They are weak so that
 * the execution tests link while a display scenario is not yet written; a
 * missing one is a null entry in the table and is reported as not linked.
 */
extern void drv_i915_test_display_lcdb(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_lcdc(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_lcdd(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_lcdg(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_lcdo(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_lcdr(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_hdmib(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_dual(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_dual_share(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_n1(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_aux(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_hdmi_edid(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_hdmi_hpd(struct i915_device *device) __attribute__((weak));
extern void drv_i915_test_display_ktest(struct i915_device *device) __attribute__((weak));

/*
 * Every scenario the runner can select, by name.
 *
 * The execution scenarios first, then the render scenarios "vkx" (the
 * Vulkan executor, driven once the node is served), "vkc" (the shader
 * compiler on the GPU, likewise), "vke1" (blending, uniform buffers and
 * several sampled images, likewise) and "vke2" (matrices, integers, loops,
 * sixteen varyings and attributes, likewise), then the display
 * scenarios; the display test suite is "display_ktest", apart from the unit
 * test suite "ktest".  The table never changes.
 */
static const struct i915_test_scenario i915_test_scenarios[] = {
	{ "ktest", drv_i915_test_execution_ktest, NULL },
	{ "eu", drv_i915_test_execution_eu, NULL },
	{ "draw", drv_i915_test_execution_draw, NULL },
	{ "r1", drv_i915_test_execution_r1, NULL },
	{ "tex", drv_i915_test_execution_tex, NULL },
	{ "t3", drv_i915_test_execution_t3, NULL },
	{ "bl", drv_i915_test_execution_bl, NULL },
	{ "vkx", NULL, drv_i915_test_render_executor },
	{ "vkc", NULL, drv_i915_test_render_compiler },
	{ "vke1", NULL, drv_i915_test_render_features },
	{ "vke2", NULL, drv_i915_test_render_generality },
	{ "lcdb", NULL, drv_i915_test_display_lcdb },
	{ "lcdc", NULL, drv_i915_test_display_lcdc },
	{ "lcdd", NULL, drv_i915_test_display_lcdd },
	{ "lcdg", NULL, drv_i915_test_display_lcdg },
	{ "lcdo", NULL, drv_i915_test_display_lcdo },
	{ "lcdr", NULL, drv_i915_test_display_lcdr },
	{ "hdmib", NULL, drv_i915_test_display_hdmib },
	{ "dual", NULL, drv_i915_test_display_dual },
	{ "dual_share", NULL, drv_i915_test_display_dual_share },
	{ "n1", NULL, drv_i915_test_display_n1 },
	{ "aux", NULL, drv_i915_test_display_aux },
	{ "hdmi_edid", NULL, drv_i915_test_display_hdmi_edid },
	{ "hdmi_hpd", NULL, drv_i915_test_display_hdmi_hpd },
	{ "display_ktest", NULL, drv_i915_test_display_ktest }
};

static const struct i915_test_scenario *i915_test_find(const char *name);

/*
 * Runs the scenario the test build selected on the started device.
 *
 * Called by the device start once, on the start worker, right before the node
 * is published and served.  Logs one line that names the scenario and its
 * verdict, and returns whatever the verdict, so the device is served either
 * way.
 */
void
drv_i915_test_after_start(
	struct i915_device *device)
{
	const struct i915_test_scenario *scenario;
	const char *selected;
	int error;

	/* A test build that selected no scenario only says so. */
	selected = I915_TEST_SELECTED;
	if (selected[0] == 0) {
		kern_logf("i915: test: no scenario selected (build with -DI915_TEST_SCENARIO=<name>)\n");
		return;
	}

	/* Refuses a name the table does not know. */
	scenario = i915_test_find(selected);
	if (scenario == NULL) {
		kern_logf("i915: test %s: FAIL (no such scenario)\n", selected);
		return;
	}

	kern_logf("i915: test %s: begin\n", scenario->name);

	/* Runs an execution scenario and reports its verdict. */
	if (scenario->execution != NULL) {
		error = scenario->execution(device);
		if (error != 0) {
			kern_logf("i915: test %s: FAIL rc=%d\n", scenario->name, error);
		} else {
			kern_logf("i915: test %s: PASS\n", scenario->name);
		}

		return;
	}

	/* Refuses a display scenario the display tests do not define. */
	if (scenario->display == NULL) {
		kern_logf("i915: test %s: FAIL (the display scenario is not linked)\n", scenario->name);
		return;
	}

	/* Runs a display scenario; its own lines carry the verdict. */
	scenario->display(device);
	kern_logf("i915: test %s: end (the verdict is in the scenario's own log lines)\n", scenario->name);
}

/*
 * Runs the in-kernel unit test suite.
 *
 * Returns 0 when every check that ran passed.
 */
int
drv_i915_test_execution_ktest(
	struct i915_device *device)
{
	static struct i915_ktest ktest;
	int error;

	/* Runs every part and reports the tally. */
	error = drv_i915_ktest_run(device, &ktest);
	if (error != 0)
		return error;

	/* Succeeded: no check failed. */
	return 0;
}

/* Finds a scenario by name. */
static const struct i915_test_scenario *
i915_test_find(
	const char *name)
{
	unsigned count;
	unsigned i;
	int differs;

	/* Compares the name with every entry of the table. */
	count = sizeof(i915_test_scenarios) / sizeof(i915_test_scenarios[0]);
	for (i = 0U; i < count; i++) {
		differs = kern_strcmp(i915_test_scenarios[i].name, name);
		if (differs == 0)
			return &i915_test_scenarios[i];
	}

	/* No scenario has that name. */
	return NULL;
}
