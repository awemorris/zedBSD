/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs one render job of a render pass instance on a session's render
 * context: its programs and commands are built, kicked and waited for.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_JOB_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_JOB_H

#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/render-private.h"

/*
 * Runs a render job without draws: each tile is cleared or loaded as the
 * colour attachment's load operation says and stored back.  ENOTSUP for
 * what is not built yet (draws, more than one colour attachment, a depth
 * attachment that is loaded or stored, an attachment wider than one
 * word); ETIMEDOUT when it did not finish in time, the core then may
 * still run it and its memory stays held.
 */
int powervr_render_job_run(struct powervr_render *render, struct powervr_session *session, const struct powervr_render_job *job);

#endif
