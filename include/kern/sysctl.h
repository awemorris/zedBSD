/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_SYSCTL_H
#define KERN_KERN_SYSCTL_H

#include <stddef.h>
#include <stdint.h>

/*
 * What a GPU driver lets root do with the devices it holds back at boot
 * (hw.gpu.start).
 *
 * A driver that can hold its devices until root asks for the start (the
 * i915 with the boot parameter i915.start=manual) hands these to the kernel
 * once, while it registers.  The operations live as long as the kernel.
 */
struct kern_gpu_start_ops {
	/* Reports how many devices are held for the start. */
	uint64_t (*held)(void);

	/* Starts every held device; ENODEV when none is held. */
	int (*start)(void);
};

void
sysctl_init(void);

/*
 * Counts a GPU device whose driver has attached it and will publish its node
 * later (hw.gpu.attaching).
 */
void
kern_gpu_attach_begin(void);

/*
 * Uncounts a GPU device whose node is now published or never will be.
 */
void
kern_gpu_attach_end(void);

/*
 * Installs the operations behind hw.gpu.start; called once, on the boot
 * thread, before user space runs.
 */
void
kern_gpu_start_ops_set(
	const struct kern_gpu_start_ops *ops);

struct gpu_telemetry_entry;

/*
 * Reads one GPU's telemetry into an entry whose driver name is filled and
 * whose other fields are zero: the driver sets the fields it keeps and
 * their bits in valid.  Returns 0, or an errno to leave the GPU out.
 */
typedef int (*kern_gpu_telemetry_read_t)(void *context, struct gpu_telemetry_entry *entry);

/*
 * Lists a GPU in hw.gputelemetry (ws134-p007): its driver's name, the
 * function that reads it and its context, which must live as long as the
 * kernel.  The same context is listed once.  Returns 0, or ENOSPC when the
 * list is full.
 */
int
kern_gpu_telemetry_register(
	const char *driver,
	kern_gpu_telemetry_read_t read,
	void *context);

struct thermal_entry;

/*
 * Reads a source's sensors into entries (at most capacity of them, zeroed
 * by the caller) and gives how many it filled.  Returns 0, or an errno to
 * leave the source out.
 */
typedef int (*kern_thermal_read_t)(void *context, struct thermal_entry *entries, unsigned capacity, unsigned *count);

/*
 * Lists a source of temperatures in hw.thermal (ws134-p009): the function
 * that reads its sensors and its context, which must live as long as the
 * kernel.  The same context is listed once.  Returns 0, ENOSPC when the
 * list is full, or EINVAL without a function.
 */
int
kern_thermal_register(
	kern_thermal_read_t read,
	void *context);

int
kern_sysctl(
	const int *name,
	unsigned namelen,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen,
	int superuser);

#endif
