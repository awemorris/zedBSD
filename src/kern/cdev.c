/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/poll.h"

#include <errno.h>
#include <string.h>

static struct cdev devices[CDEV_MAX] __attribute__((section(".vfs_bss")));
static unsigned device_count __attribute__((section(".vfs_bss")));

void cdev_reset(void)
{
	memset(devices, 0, sizeof(devices));
	device_count = 0;
}

int
cdev_register(const char *name, dev_t rdev, const struct cdev_ops *ops,
	      void *data)
{
	size_t length;
	unsigned i;

	if (name == NULL || ops == NULL)
		return EINVAL;
	length = strlen(name);
	if (length == 0 || length >= sizeof(devices[0].name) ||
	    strchr(name, '/') != NULL)
		return EINVAL;
	for (i = 0; i < device_count; i++)
		if (!strcmp(devices[i].name, name))
			return EEXIST;
	if (device_count >= CDEV_MAX)
		return ENOSPC;
	strcpy(devices[device_count].name, name);
	devices[device_count].rdev = rdev;
	devices[device_count].ops = ops;
	devices[device_count].data = data;
	device_count++;
	return 0;
}

const struct cdev *cdev_find(const char *name)
{
	unsigned i;
	if (name == NULL)
		return NULL;
	for (i = 0; i < device_count; i++)
		if (!strcmp(devices[i].name, name))
			return &devices[i];
	return NULL;
}

const struct cdev *cdev_at(unsigned index)
{
	return index < device_count ? &devices[index] : NULL;
}

unsigned cdev_count(void) { return device_count; }

static const struct cdev *file_cdev(struct file *file)
{
	return file != NULL && file->f_inode != NULL ? file->f_inode->i_data : NULL;
}

static int cdev_open_file(struct file *file)
{
	const struct cdev *device = file_cdev(file);
	file->f_data = device != NULL ? device->data : NULL;
	return device == NULL ? ENODEV :
		device->ops->open != NULL ? device->ops->open(file) : 0;
}

static int cdev_close_file(struct file *file)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->close != NULL ?
		device->ops->close(file) : 0;
}

static ssize_t cdev_read_file(struct file *file, void *buffer, size_t size)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->read != NULL ?
		device->ops->read(file, buffer, size) : -EOPNOTSUPP;
}

static ssize_t cdev_write_file(struct file *file, const void *buffer,
			       size_t size)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->write != NULL ?
		device->ops->write(file, buffer, size) : -EOPNOTSUPP;
}

static int cdev_ioctl_file(struct file *file, unsigned long request,
			   uintptr_t argument)
{
	const struct cdev *device = file_cdev(file);
	return device != NULL && device->ops->ioctl != NULL ?
		device->ops->ioctl(file, request, argument) : EOPNOTSUPP;
}

static int cdev_poll_file(struct file *file, short events, short *revents)
{
	const struct cdev *device = file_cdev(file);
	if (revents == NULL)
		return EINVAL;
	if (device == NULL) {
		*revents = POLLERR | POLLHUP;
		return 0;
	}
	if (device->ops->poll == NULL) {
		*revents = 0;
		return 0;
	}
	return device->ops->poll(file, events, revents);
}

const struct file_ops cdev_file_ops = {
	.open = cdev_open_file,
	.close = cdev_close_file,
	.read = cdev_read_file,
	.write = cdev_write_file,
	.ioctl = cdev_ioctl_file,
	.poll = cdev_poll_file,
};
