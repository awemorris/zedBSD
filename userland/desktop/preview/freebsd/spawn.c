/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The preview on FreeBSD (WS168 p004; client.h): until keiland-preview
 * confines itself with Capsicum (plan/ws177/backlog-p2.md), the preview is
 * made in the caller's own process, as the callers decoded pictures before
 * (Q1, 2026-10-07: no loss of the previews on FreeBSD meanwhile).
 */

#include "../spawn.h"

/*
 * Makes the preview here and now.  Returns 0 with no child and the status.
 */
int
preview_spawn(
	int input,
	int output,
	const struct preview_request *request,
	struct preview_child **child,
	int *status)
{
	/* In this process. */
	*child = NULL;
	*status = preview_make(input, output, request);

	/* Succeeded: made. */
	return 0;
}

/*
 * Looks at a child: there are none here (every preview is made at once),
 * so whatever is asked has ended.
 */
int
preview_child_ended(
	struct preview_child *child,
	int *status)
{
	/* No child to look at. */
	(void)child;
	*status = PREVIEW_KILLED;
	return 1;
}

/*
 * Ends a child: there are none here.
 */
int
preview_child_end(
	struct preview_child *child)
{
	/* No child to end. */
	(void)child;
	return PREVIEW_KILLED;
}
