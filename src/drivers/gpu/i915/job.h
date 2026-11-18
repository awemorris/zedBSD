/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Supervised GPU jobs.
 *
 * The GPU core reserves a request slot and its completion before userspace
 * submits native work, then commits the reservation as a marker request or
 * cancels it.  The reservation token is the slot's address; it is resolved
 * by comparison against the engine records' slots, never dereferenced first.
 */

#ifndef DRIVERS_GPU_I915_JOB_H
#define DRIVERS_GPU_I915_JOB_H

struct drv_gpu_ops;

void drv_i915_job_bind_ops(struct drv_gpu_ops *ops);

#endif
