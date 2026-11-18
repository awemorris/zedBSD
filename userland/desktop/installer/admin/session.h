/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writing requests of install-admin's session: one change of a
 * partition table (edit) and the installation (install).  Each checks
 * the request again against the disk as it is now, writes, and reports
 * on the output one line at a time:
 *
 *   STEP N TOTAL NAME     the step begun
 *   BYTES DONE TOTAL      the bytes unpacked so far
 *   UNIT NAME             the unit being unpacked
 *   STATE TEXT            what the disk holds now
 *   DONE                  the request is done
 *   FAIL STEP ERRNO TEXT  the request failed at a step
 *
 * A cancellation ("cancel" on the input, or its end) is taken at the end
 * of a step and between the files of a unit.
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_SESSION_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_SESSION_H

#include "index.h"
#include "plan.h"

#include <stdio.h>

/*
 * What a session holds from its start to its end: the index it checks
 * requests against, where it reports, where a cancellation comes from,
 * the disk its first writing request bound it to, and whether the
 * installer has cancelled (or gone).
 */
struct admin_session {
	const struct installer_index *index;
	FILE *output;
	int input;
	uint64_t disk;
	int bound;
	int canceled;
};

int session_edit(struct admin_session *session, const struct plan_request *request);
int session_install(struct admin_session *session, struct plan_request *request);

#endif
