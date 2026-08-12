/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/boot-device.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/filedesc.h"
#include "kern/kmem.h"
#include "kern/pc98/linux-boot.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include "kern/uaccess.h"

#include <zedbsd/boot.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

static struct pc98_linux_image *pending_image;
static int accepting_requests = 1;

static int
boot_ioctl(struct file *device, unsigned long request, uintptr_t argument)
{
	struct zedbsd_boot_linux input;
	struct pc98_linux_image *image = NULL;
	struct process *process;
	struct file *kernel;
	char *arguments;
	bool enabled;
	int error;
	unsigned i;
	(void)device;

	if (request != ZEDBSD_BOOT_LINUX)
		return EOPNOTSUPP;
	error = copyin(argument, &input, sizeof(input));
	if (error != 0)
		return error;
	if (input.flags != 0 || input.command_line_length > 4095U ||
	    (input.command_line_length != 0 && input.command_line == 0))
		return EINVAL;
	for (i = 0; i < 3; i++)
		if (input.reserved[i] != 0)
			return EINVAL;
	process = curthread != NULL ? curthread->proc : NULL;
	kernel = process != NULL ? filedesc_get(process->fd, input.kernel_fd) : NULL;
	if (kernel == NULL)
		return EBADF;
	arguments = kern_malloc((size_t)input.command_line_length + 1U);
	if (arguments == NULL)
		return ENOMEM;
	if (input.command_line_length != 0) {
		error = copyin(input.command_line, arguments,
		    input.command_line_length);
		if (error != 0) {
			kern_free(arguments);
			return error;
		}
	}
	arguments[input.command_line_length] = '\0';
	file_ref(kernel);
	error = pc98_linux_prepare(kernel, arguments,
	    input.boot_device_index, &image);
	(void)file_close(kernel);
	kern_free(arguments);
	if (error != 0)
		return error;
	enabled = hal_irq_disable();
	if (!accepting_requests || pending_image != NULL) {
		if (enabled) hal_irq_enable();
		pc98_linux_discard(image);
		return EBUSY;
	}
	pending_image = image;
	accepting_requests = 0;
	if (enabled) hal_irq_enable();
	sched_sleep(0);
	HAL_FATAL("published Linux request resumed");
	return EIO;
}

static const struct cdev_ops boot_ops = { .ioctl = boot_ioctl };

int
boot_device_register(void)
{
	return cdev_register("boot", 0x00010003U, &boot_ops, NULL);
}

int
kern_boot_pending(void)
{
	return pending_image != NULL;
}

void
kern_boot_execute_pending(void)
{
	struct pc98_linux_image *image;
	(void)hal_irq_disable();
	if (curthread != &thread0 || pending_image == NULL)
		HAL_FATAL("invalid Linux handoff context");
	image = pending_image;
	pending_image = NULL;
	process_force_quiesce_users();
	pc98_linux_commit(image);
}
