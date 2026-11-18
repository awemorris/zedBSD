/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The System Monitor's rules: the system's state graded in four levels
 * from the frames (design.md section 3.7).
 *
 * Each rule has an attention threshold and a critical one.  A rule whose
 * attention threshold has held for 10 seconds makes the state Elevated,
 * for 60 seconds Warning; its critical threshold held for its time makes
 * it Critical.  The state rises at once and falls one level every 5
 * seconds.  Only the fields the source really gave count: a simulated
 * field never grades the state (design.md section 1.5), except from the
 * simulation itself, whose excursions are there to be seen.
 */

#include "monitor.h"

#include <stdio.h>
#include <string.h>

/* How long a condition holds before it raises the state, and how often the state falls a level, in milliseconds. */
#define RULE_ELEVATED_MS	10000U
#define RULE_WARNING_MS		60000U
#define RULE_FALL_MS		5000U

/* A rule's name and unit in the summary, the field it needs, and how long its critical threshold must hold. */
struct rule_shape {
	const char *name;
	const char *unit;
	unsigned field;
	uint64_t critical_ms;
};

static const struct rule_shape rule_shapes[SM_RULE_COUNT] = {
	{ "CPU", "%", SM_HAVE_CPU, 120000U },
	{ "Memory available", "%", SM_HAVE_MEMORY, 10000U },
	{ "Swap", "%", SM_HAVE_SWAP, 10000U },
	{ "Disk latency", " ms", SM_HAVE_DISK_LATENCY, 10000U },
	{ "GPU", "%", SM_HAVE_GPU_BUSY, 30000U },
	{ "GPU temperature", " C", SM_HAVE_GPU_TEMPERATURE, 30000U }
};

static void rule_judge(const struct sm_info *info, const struct sm_frame *frame, unsigned rule, int *attention, int *critical, double *value);
static void rule_track(uint64_t *since, int holds, uint64_t now);
static enum sm_level rule_level(const uint64_t *since, uint64_t critical_ms, uint64_t now);

/*
 * Starts the rules with the state Normal.
 */
void
sm_rules_init(
	struct sm_rules *rules)
{
	/* No condition holds yet. */
	memset(rules, 0, sizeof(*rules));
	rules->level = SM_LEVEL_NORMAL;
	rules->cause = -1;
	strcpy(rules->summary, "Stable");
}

/*
 * Grades the state with a new frame and reports it; count_simulated lets
 * the simulation's fields grade it too.
 */
enum sm_level
sm_rules_update(
	struct sm_rules *rules,
	const struct sm_info *info,
	const struct sm_frame *frame,
	int count_simulated)
{
	enum sm_level target;
	enum sm_level level;
	unsigned rule;
	unsigned real;
	int attention;
	int critical;
	int cause;
	double value;
	double cause_value;

	/* The fields the source really gave (all of them from the simulation). */
	real = frame->valid & ~frame->simulated;
	if (count_simulated)
		real = frame->valid;

	/* Each rule's conditions, and the highest level any of them reaches. */
	target = SM_LEVEL_NORMAL;
	cause = -1;
	cause_value = 0.0;
	for (rule = 0; rule < SM_RULE_COUNT; rule++) {
		/* A rule without its real field holds nothing. */
		attention = 0;
		critical = 0;
		value = 0.0;
		if ((real & rule_shapes[rule].field) != 0U)
			rule_judge(info, frame, rule, &attention, &critical, &value);

		/* Its conditions' times, and the level they reach. */
		rule_track(&rules->since_ms[rule][0], attention, frame->time_ms);
		rule_track(&rules->since_ms[rule][1], critical, frame->time_ms);
		level = rule_level(rules->since_ms[rule], rule_shapes[rule].critical_ms, frame->time_ms);
		rules->rule_levels[rule] = level;

		/* The first rule at the highest level is the cause. */
		if (level > target) {
			target = level;
			cause = (int)rule;
			cause_value = value;
		}
	}

	/* The state rises at once, and falls one level at a time. */
	if (target > rules->level) {
		rules->level = target;
		rules->changed_ms = frame->time_ms;
	} else if (target < rules->level && frame->time_ms >= rules->changed_ms + RULE_FALL_MS) {
		rules->level--;
		rules->changed_ms = frame->time_ms;
	}

	/* The summary: what holds the state up, or that all is well. */
	if (cause >= 0) {
		rules->cause = cause;
		(void)snprintf(rules->summary, sizeof(rules->summary), "%s %.0f%s", rule_shapes[cause].name, cause_value,
			       rule_shapes[cause].unit);
	} else if (rules->level == SM_LEVEL_NORMAL) {
		rules->cause = -1;
		strcpy(rules->summary, "Stable");
	}

	/* Succeeded: the state. */
	return rules->level;
}

/*
 * Names a level for the screen and the log.
 */
const char *
sm_level_name(
	enum sm_level level)
{
	/* One word each. */
	switch (level) {
	case SM_LEVEL_ELEVATED:
		return "Elevated";
	case SM_LEVEL_WARNING:
		return "Warning";
	case SM_LEVEL_CRITICAL:
		return "Critical";
	default:
		return "Normal";
	}
}

/* Tells whether a rule's attention and critical thresholds are passed, and the value judged. */
static void
rule_judge(
	const struct sm_info *info,
	const struct sm_frame *frame,
	unsigned rule,
	int *attention,
	int *critical,
	double *value)
{
	double share;

	/* Each rule its own measure. */
	switch (rule) {
	case SM_RULE_CPU:
		*value = frame->cpu * 100.0;
		*attention = frame->cpu > 0.80;
		*critical = frame->cpu > 0.95;
		break;
	case SM_RULE_MEMORY:
		/* The share still available. */
		share = 1.0;
		if (info->memory_total != 0U)
			share = (double)frame->memory_available / (double)info->memory_total;
		*value = share * 100.0;
		*attention = share < 0.15;
		*critical = share < 0.05;
		break;
	case SM_RULE_SWAP:
		/* The share of the swap in use. */
		share = 0.0;
		if (info->swap_total != 0U)
			share = (double)frame->swap_used / (double)info->swap_total;
		*value = share * 100.0;
		*attention = share > 0.50;
		*critical = share > 0.90;
		break;
	case SM_RULE_LATENCY:
		*value = frame->disk_latency_ms;
		*attention = frame->disk_latency_ms > 5.0;
		*critical = frame->disk_latency_ms > 50.0;
		break;
	case SM_RULE_GPU:
		*value = frame->gpu_busy[0] * 100.0;
		*attention = frame->gpu_busy[0] > 0.90;
		*critical = 0;
		break;
	default:
		*value = frame->gpu_celsius[0];
		*attention = frame->gpu_celsius[0] > 80.0;
		*critical = frame->gpu_celsius[0] > 95.0;
		break;
	}
}

/* Keeps when a condition started holding, plus one (0 while it does not hold). */
static void
rule_track(
	uint64_t *since,
	int holds,
	uint64_t now)
{
	/* A condition that stops holding forgets its start. */
	if (!holds) {
		*since = 0;
		return;
	}

	/* One that starts holding keeps the time, one more so that a start at 0 is not "none". */
	if (*since == 0U)
		*since = now + 1U;
}

/* The level a rule's conditions reach by how long they have held. */
static enum sm_level
rule_level(
	const uint64_t *since,
	uint64_t critical_ms,
	uint64_t now)
{
	/* The critical threshold held long enough (the starts are kept one more). */
	if (since[1] != 0U && now + 1U >= since[1] + critical_ms)
		return SM_LEVEL_CRITICAL;

	/* The attention threshold held for a minute, then for ten seconds. */
	if (since[0] != 0U && now + 1U >= since[0] + RULE_WARNING_MS)
		return SM_LEVEL_WARNING;
	if (since[0] != 0U && now + 1U >= since[0] + RULE_ELEVATED_MS)
		return SM_LEVEL_ELEVATED;

	/* Nothing held long enough. */
	return SM_LEVEL_NORMAL;
}
