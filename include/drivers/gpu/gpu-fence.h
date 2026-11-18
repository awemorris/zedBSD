/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reference-bearing completion state independent of an open GPU session.
 */

#ifndef KERN_DRIVERS_GPU_FENCE_H
#define KERN_DRIVERS_GPU_FENCE_H

#include <kern/handle.h>
#include <stdint.h>

#define DRV_GPU_FENCE_PENDING	0U
#define DRV_GPU_FENCE_SIGNALED	1U
#define DRV_GPU_FENCE_ERROR	2U

/* One locked observation, generation changes invalidate prior pending work. */
struct drv_gpu_fence_state {
	uint64_t generation;
	uint32_t state;
	int error;
};

/* Creation returns one typed handle reference, device identity is immutable. */
int drv_gpu_fence_create(uint64_t device, unsigned signaled, struct kernel_handle **result);
int drv_gpu_fence_device(struct kernel_handle *handle, uint64_t device);
int drv_gpu_fence_query(struct kernel_handle *handle, uint64_t generation, struct drv_gpu_fence_state *state);
int drv_gpu_fence_wait(struct kernel_handle *handle, uint64_t generation, uint64_t deadline, unsigned immediate, struct drv_gpu_fence_state *state);

/* Kernel dependencies refuse pending generations without an admitted driver job. */
int drv_gpu_fence_wait_work(struct kernel_handle *handle, uint64_t generation, struct drv_gpu_fence_state *state);
int drv_gpu_fence_admit(struct kernel_handle *handle, uint64_t generation, void *owner);
int drv_gpu_fence_reset(struct kernel_handle *handle, uint64_t generation, struct drv_gpu_fence_state *state);

/* A retained owner binds before submission and must signal or release before retirement. */
int drv_gpu_fence_bind(struct kernel_handle *handle, uint64_t generation, void *owner);
int drv_gpu_fence_unbind(struct kernel_handle *handle, uint64_t generation, void *owner);
int drv_gpu_fence_signal(struct kernel_handle *handle, uint64_t generation, void *owner, int error);

/* IRQ producers holding a registry lock defer poll_notify until after that lock is released. */
int drv_gpu_fence_signal_deferred(struct kernel_handle *handle, uint64_t generation, void *owner, int error);

#endif
