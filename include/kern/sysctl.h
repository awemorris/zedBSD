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
 * Reports which USB transfers the kernel log describes (hw.usb.trace, the
 * HW_USB_TRACE_ bits).
 */
uint64_t
kern_usb_trace(void);

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
 * Lists a GPU in hw.gputelemetry: its driver's name, the
 * function that reads it and its context, which must live as long as the
 * kernel.  The same context is listed once.  Returns 0, or ENOSPC when the
 * list is full.
 */
int
kern_gpu_telemetry_register(
	const char *driver,
	kern_gpu_telemetry_read_t read,
	void *context);

/* The most bytes of hw.gpu.powervr.stages, its NUL included. */
#define KERN_GPU_STAGES_MAX	256U

/*
 * Writes the text of hw.gpu.powervr.stages into text (capacity bytes) and
 * returns its length without a NUL.
 */
typedef size_t (*kern_gpu_stages_read_t)(char *text, size_t capacity);

/*
 * Installs the function behind hw.gpu.powervr.stages; called once, on the
 * boot thread, before user space runs.
 */
void
kern_gpu_stages_set(
	kern_gpu_stages_read_t read);

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
