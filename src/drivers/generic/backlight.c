/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backlight devices' class.
 *
 * Each registered provider gets the lowest free number N and the device
 * backlightN, which devfs shows as /dev/backlight/backlightN.  A request
 * runs the provider's operation under the device's mutex, and the
 * withdrawal takes the provider away under the same mutex, so no operation
 * runs after kern_backlight_unregister() returns; a file still open on the
 * device then answers ENXIO, and the record goes with the device's last
 * reference.
 */

#include "kern/backlight.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/uaccess.h"
#include <kern/kcrt.h>

#include <uapi/backlight.h>
#include <uapi/errno.h>
#include <uapi/fcntl.h>

/* The device numbers of the class: one major, the minor is N. */
#define BACKLIGHT_DEVICE_BASE	0x000d0000U

/* The most a brightness may be (a percentage). */
#define BACKLIGHT_PERCENT_MAX	100U

/* One published provider. */
struct kern_backlight {
	/*
	 * Serializes the requests with each other and with the withdrawal,
	 * which clears ops: a request that finds ops NULL answers ENXIO.
	 */
	struct mutex lock;
	const struct kern_backlight_ops *ops;
	void *context;

	/* What GETINFO reports; fixed at registration. */
	char name[BACKLIGHTMAXNAMELENGTH];
	uint32_t type;

	/* The slot the provider holds and the device's reference the registration owns. */
	unsigned number;
	struct cdev *node;
};

static int backlight_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int backlight_get_status(struct kern_backlight *backlight, uintptr_t argument);
static int backlight_update_status(struct file *file, struct kern_backlight *backlight, uintptr_t argument);
static int backlight_get_info(struct kern_backlight *backlight, uintptr_t argument);
static int backlight_slot_claim(struct kern_backlight *backlight);
static void backlight_slot_release(struct kern_backlight *backlight);
static void backlight_finalize(void *data);

/* The operations of every backlight device. */
static const struct cdev_ops backlight_cdev_ops = {
	.ioctl = backlight_ioctl
};

/* The provider holding each number, NULL for a free one; guarded by backlight_registry_lock. */
static struct kern_backlight *backlight_slots[KERN_BACKLIGHT_MAX];

/* Serializes the claiming and the release of the numbers for the kernel's life. */
static struct spinlock backlight_registry_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "backlight registry", 0, 0
};

/*
 * Registers a provider and publishes its device.
 *
 * Returns 0 with the provider's record in *result, EINVAL for a missing
 * name, operation or result or an unknown type, ENOSPC when every number
 * is taken, ENOMEM, or the error of publishing the device.
 */
int
kern_backlight_register(
	const char *name,
	uint32_t type,
	const struct kern_backlight_ops *ops,
	void *context,
	struct kern_backlight **result)
{
	struct kern_backlight *backlight;
	struct cdev *node;
	char node_name[32];
	int error;

	/* A provider names itself, does both operations and says what it lights. */
	if (name == NULL || ops == NULL || result == NULL)
		return EINVAL;
	if (ops->get == NULL || ops->set == NULL)
		return EINVAL;
	if (type != BACKLIGHT_TYPE_PANEL && type != BACKLIGHT_TYPE_KEYBOARD)
		return EINVAL;

	/* The provider's record. */
	backlight = kern_calloc(1U, sizeof(*backlight));
	if (backlight == NULL)
		return ENOMEM;

	/* The lock of its requests and its withdrawal. */
	error = mutex_init(&backlight->lock, LOCK_RANK_DEVICE, "backlight");
	if (error != 0) {
		kern_free(backlight);
		return error;
	}

	/* The provider and its description. */
	backlight->ops = ops;
	backlight->context = context;
	kern_strncpy(backlight->name, name, sizeof(backlight->name) - 1U);
	backlight->name[sizeof(backlight->name) - 1U] = '\0';
	backlight->type = type;

	/* The lowest free number. */
	error = backlight_slot_claim(backlight);
	if (error != 0) {
		kern_free(backlight);
		return error;
	}

	/* Publishes backlightN; the record lives as long as the device does. */
	kern_snprintf(node_name, sizeof(node_name), "backlight%u", backlight->number);
	error = cdev_register_managed(node_name,
	    (dev_t)(BACKLIGHT_DEVICE_BASE + backlight->number),
	    &backlight_cdev_ops,
	    backlight,
	    backlight_finalize,
	    &node);
	if (error != 0) {
		backlight_slot_release(backlight);
		kern_free(backlight);
		return error;
	}

	/* The registration's reference, given back at the withdrawal. */
	backlight->node = node;

	/* Succeeded: the device is published and the record is the provider's handle. */
	*result = backlight;
	return 0;
}

/*
 * Withdraws a provider and its device.
 */
void
kern_backlight_unregister(
	struct kern_backlight *backlight)
{
	struct cdev *node;

	/* Nothing was registered. */
	if (backlight == NULL)
		return;

	/* Takes the provider away after the request running now, if any. */
	mutex_lock(&backlight->lock);

	backlight->ops = NULL;
	backlight->context = NULL;

	mutex_unlock(&backlight->lock);

	/* The number may be given again. */
	backlight_slot_release(backlight);

	/*
	 * Unpublishes the device and gives back the registration's reference;
	 * the record may be freed by then (the device's last reference), so it
	 * is not touched after this.
	 */
	node = backlight->node;
	(void)cdev_unregister(node);
	cdev_release(node);
}

/* Answers a request on a backlight device. */
static int
backlight_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct kern_backlight *backlight;
	int error;

	/* The provider the device was opened on. */
	backlight = file->f_data;

	/* Does what the request asks. */
	switch (request) {
	case BACKLIGHTGETSTATUS:
		error = backlight_get_status(backlight, argument);
		break;
	case BACKLIGHTUPDATESTATUS:
		error = backlight_update_status(file, backlight, argument);
		break;
	case BACKLIGHTGETINFO:
		error = backlight_get_info(backlight, argument);
		break;
	default:
		error = ENOTTY;
		break;
	}

	/* Reports why the request failed. */
	if (error != 0)
		return error;

	/* Succeeded: the request was answered. */
	return 0;
}

/* Reports the brightness: BACKLIGHTGETSTATUS. */
static int
backlight_get_status(
	struct kern_backlight *backlight,
	uintptr_t argument)
{
	struct backlight_props props;
	uint32_t percent;
	int error;

	/* Asks the provider, unless it is gone. */
	percent = 0U;
	error = ENXIO;
	mutex_lock(&backlight->lock);

	if (backlight->ops != NULL)
		error = backlight->ops->get(backlight->context, &percent);

	mutex_unlock(&backlight->lock);

	/* Reports why the brightness could not be read. */
	if (error != 0)
		return error;

	/* A provider's answer above the range is the highest light. */
	if (percent > BACKLIGHT_PERCENT_MAX)
		percent = BACKLIGHT_PERCENT_MAX;

	/* The status: the brightness, and no fixed levels (any percentage is taken). */
	kern_memset(&props, 0, sizeof(props));
	props.brightness = percent;
	props.nlevels = 0U;

	/* Hands it to the caller. */
	error = copyout(&props, argument, sizeof(props));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the brightness. */
	return 0;
}

/* Sets the brightness: BACKLIGHTUPDATESTATUS, for a file open for writing. */
static int
backlight_update_status(
	struct file *file,
	struct kern_backlight *backlight,
	uintptr_t argument)
{
	struct backlight_props props;
	int mode;
	int error;

	/* Only a writer changes the light. */
	mode = file_status_flags_get(file) & O_ACCMODE;
	if (mode == O_RDONLY)
		return EBADF;

	/* The caller's status; only its brightness is read. */
	error = copyin(argument, &props, sizeof(props));
	if (error != 0)
		return error;

	/* A brightness is a percentage. */
	if (props.brightness > BACKLIGHT_PERCENT_MAX)
		return EINVAL;

	/* Asks the provider, unless it is gone. */
	error = ENXIO;
	mutex_lock(&backlight->lock);

	if (backlight->ops != NULL)
		error = backlight->ops->set(backlight->context, props.brightness);

	mutex_unlock(&backlight->lock);

	/* Reports why the brightness could not be set. */
	if (error != 0)
		return error;

	/* Succeeded: the light has the brightness. */
	return 0;
}

/* Reports the provider's name and what it lights: BACKLIGHTGETINFO. */
static int
backlight_get_info(
	struct kern_backlight *backlight,
	uintptr_t argument)
{
	struct backlight_info info;
	int gone;
	int error;

	/* Samples whether the provider is still there. */
	gone = 1;
	mutex_lock(&backlight->lock);

	if (backlight->ops != NULL)
		gone = 0;

	mutex_unlock(&backlight->lock);

	/* A withdrawn provider describes nothing. */
	if (gone)
		return ENXIO;

	/* The description fixed at registration. */
	kern_memset(&info, 0, sizeof(info));
	kern_memcpy(info.name, backlight->name, sizeof(info.name));
	info.type = (enum backlight_info_type)backlight->type;

	/* Hands it to the caller. */
	error = copyout(&info, argument, sizeof(info));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the description. */
	return 0;
}

/* Gives a provider the lowest free number; ENOSPC when none is free. */
static int
backlight_slot_claim(
	struct kern_backlight *backlight)
{
	unsigned long irq;
	unsigned index;
	int error;

	/* Takes the first free slot. */
	error = ENOSPC;
	irq = spin_lock_irqsave(&backlight_registry_lock);

	for (index = 0U; index < KERN_BACKLIGHT_MAX; index++) {
		if (backlight_slots[index] == NULL) {
			backlight_slots[index] = backlight;
			backlight->number = index;
			error = 0;
			break;
		}
	}

	spin_unlock_irqrestore(&backlight_registry_lock, irq);

	/* Reports that every number is taken. */
	if (error != 0)
		return error;

	/* Succeeded: the provider holds its number. */
	return 0;
}

/* Frees a provider's number. */
static void
backlight_slot_release(
	struct kern_backlight *backlight)
{
	unsigned long irq;

	/* The slot is free again, if the provider still holds it. */
	irq = spin_lock_irqsave(&backlight_registry_lock);

	if (backlight_slots[backlight->number] == backlight)
		backlight_slots[backlight->number] = NULL;

	spin_unlock_irqrestore(&backlight_registry_lock, irq);
}

/* Frees a provider's record with its device's last reference. */
static void
backlight_finalize(
	void *data)
{
	/* No request and no provider can reach the record any more. */
	kern_free(data);
}
