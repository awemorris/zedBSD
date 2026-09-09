/* SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <kern/cdev.h>
#include <kern/memory-device.h>
#include <zedbsd/poll.h>

static struct cdev devices[2];
static unsigned registered;
static unsigned released;
static unsigned removed;
static unsigned fail_at;

int cdev_register_managed(const char *name, dev_t rdev,
    const struct cdev_ops *ops, void *data, cdev_finalizer_t finalizer,
    struct cdev **result)
{
	struct cdev *device;
	(void)finalizer;
	if (++registered == fail_at)
		return ENOSPC;
	assert(registered <= 2);
	device = &devices[registered - 1];
	strcpy(device->name, name);
	device->rdev = rdev;
	device->ops = ops;
	device->data = data;
	*result = device;
	return 0;
}

int cdev_unregister(struct cdev *device)
{
	assert(device == &devices[0]);
	removed++;
	return 0;
}

void cdev_release(struct cdev *device)
{
	assert(device == &devices[0] || device == &devices[1]);
	released++;
}

int main(void)
{
	unsigned char data[65538];
	unsigned i;
	short ready;

	assert(drv_memory_device_register() == 0);
	assert(registered == 2 && released == 2 && removed == 0);
	assert(strcmp(devices[0].name, "null") == 0);
	assert(strcmp(devices[1].name, "zero") == 0);
	assert(devices[0].rdev != devices[1].rdev);
	memset(data, 0xa5, sizeof(data));
	assert(devices[0].ops->read(NULL, data, sizeof(data)) == 0);
	for (i = 0; i < sizeof(data); i++)
		assert(data[i] == 0xa5);
	assert(devices[1].ops->read(NULL, data + 1, 65536) == 65536);
	assert(data[0] == 0xa5 && data[65537] == 0xa5);
	for (i = 1; i <= 65536; i++)
		assert(data[i] == 0);
	for (i = 0; i < 2; i++) {
		assert(devices[i].ops->read(NULL, NULL, 0) == 0);
		assert(devices[i].ops->write(NULL, data, sizeof(data)) == sizeof(data));
		assert(devices[i].ops->write(NULL, NULL, 0) == 0);
		assert(devices[i].ops->poll(NULL, POLLIN | POLLOUT | POLLPRI,
		    &ready) == 0);
		assert(ready == (POLLIN | POLLOUT));
		assert(devices[i].ops->poll(NULL, POLLRDNORM | POLLWRNORM,
		    &ready) == 0);
		assert(ready == (POLLRDNORM | POLLWRNORM));
	}
	registered = released = removed = 0;
	fail_at = 1;
	assert(drv_memory_device_register() == ENOSPC);
	assert(released == 0 && removed == 0);
	registered = 0;
	fail_at = 2;
	assert(drv_memory_device_register() == ENOSPC);
	assert(released == 1 && removed == 1);
	puts("PASS memory devices: transfers, boundaries, readiness, registration rollback");
	return 0;
}
