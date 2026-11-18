/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A bounded retry of a step that may report EBUSY.
 */

#include "drivers/gpu/powervr/retry.h"
#include <uapi/errno.h>

/*
 * Runs a step until it stops reporting EBUSY or the attempts are spent.
 */
int
powervr_retry_busy(
	const struct powervr_retry *retry,
	unsigned attempts,
	unsigned *tries)
{
	unsigned attempt;
	int answer;

	/* Nothing has been tried yet; no attempts at all is a refusal. */
	*tries = 0;
	answer = EBUSY;

	/* Tries the step, pausing only between attempts. */
	for (attempt = 0; attempt < attempts; attempt++) {
		if (attempt != 0U)
			retry->pause(retry->context);

		/* Runs the step once more. */
		answer = retry->attempt(retry->context);
		*tries = attempt + 1U;
		if (answer != EBUSY)
			break;
	}

	/* Reports a step that never stopped colliding. */
	if (answer != 0)
		return answer;

	/* Succeeded: the step went through. */
	return 0;
}
