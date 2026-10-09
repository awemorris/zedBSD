/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One private ordered execution queue retains callbacks independently of native DMA. */
#ifndef KERN_DRIVERS_GPU_BCM2711_RENDER_WORKER_H
#define KERN_DRIVERS_GPU_BCM2711_RENDER_WORKER_H

#include <stdbool.h>
#include <stdint.h>
#include <kern/waitq.h>

#define BCM2711_RENDER_REQUESTS 16U
#define BCM2711_RENDER_RESERVATIONS 8U

struct thread;
struct drv_gpu_completion;
struct drv_gpu_ops;
struct bcm2711_render_device;
struct bcm2711_render_session;

/* Execution and disposal run under the controller mutex; callbacks run without it. */
typedef int (*bcm2711_render_execute)(struct bcm2711_render_device *, struct bcm2711_render_session *, void *, bool *);
typedef int (*bcm2711_render_dispose)(struct bcm2711_render_device *, void *, bool);

/* The native IRQ guard protects every slot field while its prepared payload remains owned. */
struct bcm2711_render_request {
	struct bcm2711_render_request *next;
	struct bcm2711_render_session *session;
	struct drv_gpu_completion *completion;
	bcm2711_render_execute execute;
	bcm2711_render_dispose dispose;
	void *payload;
	/* A supervised callback retains its old queue domain through final callback retirement. */
	uint32_t timeline;
	uint32_t state;
	int canceled;
	bool supervised;
};

/* The native IRQ guard protects the FIFO, slots, pending counts and uncertain flag. */
struct bcm2711_render_worker {
	struct bcm2711_render_request requests[BCM2711_RENDER_REQUESTS];
	struct bcm2711_render_request *head;
	struct bcm2711_render_request *tail;
	struct thread *thread;
	struct wait_queue available;
	struct wait_queue retired;
	bool uncertain;
};

void bcm2711_render_jobs_bind(struct drv_gpu_ops *operations);
void bcm2711_render_worker_init(struct bcm2711_render_device *controller);
int bcm2711_render_worker_start(struct bcm2711_render_device *controller);
int bcm2711_render_worker_submit(struct bcm2711_render_session *session, bcm2711_render_execute execute, bcm2711_render_dispose dispose, void *payload, struct drv_gpu_completion *completion);
int bcm2711_render_worker_step(struct bcm2711_render_device *controller);
void bcm2711_render_worker_stop(struct bcm2711_render_session *session, int error);
int bcm2711_render_worker_stopped(struct bcm2711_render_session *session);
void bcm2711_render_worker_drain(struct bcm2711_render_session *session);
void bcm2711_render_worker_fault(struct bcm2711_render_device *controller, int error);
int bcm2711_render_worker_recovered(struct bcm2711_render_device *controller);

#endif
