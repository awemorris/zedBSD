/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display core's flag checks (../gpu-display-flags.h) on the host: the
 * backend's declared flags, a present's flags and a commit plane's flags,
 * for a backend that shows a 180-degree turn and for one that does not.
 */

#include "../gpu-display-flags.h"
#include <stdio.h>

/* One check's expectation: the flags it passes and the answer it expects. */
struct flags_case {
	const char *name;
	uint32_t plane;
	uint32_t flags;
	uint32_t ops_flags;
	int expected;
};

/*
 * The registration cases: the flags a backend may declare.
 *
 * plane is unused here.
 */
static const struct flags_case ops_cases[] = {
	{"no flag", 0U, 0U, 0U, 0},
	{"ungated", 0U, DRV_GPU_DISPLAY_UNGATED, 0U, 0},
	{"turn", 0U, DRV_GPU_DISPLAY_ROTATE_180, 0U, 0},
	{"ungated and turn", 0U, DRV_GPU_DISPLAY_UNGATED | DRV_GPU_DISPLAY_ROTATE_180, 0U, 0},
	{"unknown 4", 0U, 4U, 0U, EINVAL},
	{"turn and unknown", 0U, DRV_GPU_DISPLAY_ROTATE_180 | 0x80000000U, 0U, EINVAL},
};

/*
 * The present cases: the request's flags against each backend.
 *
 * plane is unused here.
 */
static const struct flags_case present_cases[] = {
	{"FIFO", 0U, GPU_DISPLAY_PRESENT_FIFO, 0U, 0},
	{"FIFO|BLOB", 0U, GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB, 0U, 0},
	{"FIFO on a turning backend", 0U, GPU_DISPLAY_PRESENT_FIFO, DRV_GPU_DISPLAY_ROTATE_180, 0},
	{"FIFO|ROTATE", 0U, GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_ROTATE_180, DRV_GPU_DISPLAY_ROTATE_180, 0},
	{"FIFO|BLOB|ROTATE", 0U, GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_ROTATE_180,
	 DRV_GPU_DISPLAY_ROTATE_180 | DRV_GPU_DISPLAY_UNGATED, 0},
	{"FIFO|ROTATE without the mark", 0U, GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_ROTATE_180,
	 DRV_GPU_DISPLAY_UNGATED, EOPNOTSUPP},
	{"FIFO|BLOB|ROTATE without the mark", 0U,
	 GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_ROTATE_180, 0U, EOPNOTSUPP},
	{"none", 0U, 0U, DRV_GPU_DISPLAY_ROTATE_180, EINVAL},
	{"BLOB alone", 0U, GPU_DISPLAY_PRESENT_BLOB, DRV_GPU_DISPLAY_ROTATE_180, EINVAL},
	{"ROTATE alone", 0U, GPU_DISPLAY_PRESENT_ROTATE_180, DRV_GPU_DISPLAY_ROTATE_180, EINVAL},
	{"ROTATE alone without the mark", 0U, GPU_DISPLAY_PRESENT_ROTATE_180, 0U, EINVAL},
	{"BLOB|ROTATE", 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_ROTATE_180, DRV_GPU_DISPLAY_ROTATE_180,
	 EINVAL},
	{"FIFO|unknown 8", 0U, GPU_DISPLAY_PRESENT_FIFO | 8U, DRV_GPU_DISPLAY_ROTATE_180, EINVAL},
	{"FIFO|ROTATE|unknown 8", 0U, GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_ROTATE_180 | 8U,
	 DRV_GPU_DISPLAY_ROTATE_180, EINVAL},
};

/*
 * The commit plane cases: a plane's own flags (ops_flags unused), then its
 * turn against each backend.
 */
static const struct flags_case plane_cases[] = {
	{"primary shown", 0U, GPU_DISPLAY_PLANE_SHOWN, 0U, 0},
	{"primary shown with an image", 0U, GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_IMAGE, 0U, 0},
	{"primary hidden", 0U, 0U, 0U, 0},
	{"primary shown and turned", 0U, GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_ROTATE_180, 0U, 0},
	{"primary turned with an image", 0U,
	 GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_IMAGE | GPU_DISPLAY_PLANE_ROTATE_180, 0U, 0},
	{"primary hidden and turned", 0U, GPU_DISPLAY_PLANE_ROTATE_180, 0U, EINVAL},
	{"overlay shown", 1U, GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_IMAGE, 0U, 0},
	{"overlay turned", 1U, GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_ROTATE_180, 0U, EINVAL},
	{"last overlay turned", GPU_DISPLAY_OVERLAY_MAX, GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_ROTATE_180, 0U,
	 EINVAL},
	{"unknown 8", 0U, GPU_DISPLAY_PLANE_SHOWN | 8U, 0U, EINVAL},
};

/* The commit plane turn cases: a checked plane's flags against each backend. */
static const struct flags_case rotation_cases[] = {
	{"unturned, no mark", 0U, GPU_DISPLAY_PLANE_SHOWN, 0U, 0},
	{"unturned, mark", 0U, GPU_DISPLAY_PLANE_SHOWN, DRV_GPU_DISPLAY_ROTATE_180, 0},
	{"turned, mark", 0U, GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_ROTATE_180,
	 DRV_GPU_DISPLAY_ROTATE_180 | DRV_GPU_DISPLAY_UNGATED, 0},
	{"turned, no mark", 0U, GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_ROTATE_180, DRV_GPU_DISPLAY_UNGATED,
	 EOPNOTSUPP},
};

/*
 * The answers that did not match.
 *
 * Counted by check() for the whole run; zero means every case passed.
 */
static unsigned failures;

/* The cases checked, for the summary. */
static unsigned checks;

static void check(const char *group, const struct flags_case *item, int answer);

/*
 * Runs every case and reports the count.
 *
 * The exit status is zero only when every answer matched.
 */
int
main(void)
{
	size_t index;
	int answer;

	/* The flags a backend declares at registration. */
	for (index = 0; index < sizeof(ops_cases) / sizeof(ops_cases[0]); index++) {
		answer = gpu_display_ops_flags_check(ops_cases[index].flags);
		check("ops", &ops_cases[index], answer);
	}

	/* A present's flags against the backend. */
	for (index = 0; index < sizeof(present_cases) / sizeof(present_cases[0]); index++) {
		answer = gpu_display_present_flags_check(present_cases[index].flags, present_cases[index].ops_flags);
		check("present", &present_cases[index], answer);
	}

	/* A commit plane's own flags. */
	for (index = 0; index < sizeof(plane_cases) / sizeof(plane_cases[0]); index++) {
		answer = gpu_display_plane_flags_check(plane_cases[index].plane, plane_cases[index].flags);
		check("plane", &plane_cases[index], answer);
	}

	/* A commit plane's turn against the backend. */
	for (index = 0; index < sizeof(rotation_cases) / sizeof(rotation_cases[0]); index++) {
		answer = gpu_display_plane_rotation_check(rotation_cases[index].flags, rotation_cases[index].ops_flags);
		check("rotation", &rotation_cases[index], answer);
	}

	/* Reports the tally. */
	printf("host-display-flags-test: %u checks, %u failures\n", checks, failures);

	/* Some answer did not match. */
	if (failures != 0U)
		return 1;

	/* Succeeded: every answer matched. */
	return 0;
}

/* Compares one answer with its case's expectation and counts it. */
static void
check(
	const char *group,
	const struct flags_case *item,
	int answer)
{
	/* Every case counts toward the summary. */
	checks++;

	/* A mismatch is printed and counted. */
	if (answer != item->expected) {
		printf("FAIL %s: %s: %d, expected %d\n", group, item->name, answer, item->expected);
		failures++;
	}

	/* Succeeded: the answer is counted. */
	return;
}
