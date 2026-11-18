/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/input/bridge (ws143-p005, design D3): HID devices a program makes from
 * a report descriptor, the Bluetooth daemon's keyboards and mice among
 * them.
 *
 * Only root may open the node, and at most INPUT_BRIDGE_OPENS_MAX opens at a
 * time.  The first write of an open declares one device (struct
 * input_bridge_setup, include/uapi/input-bridge.h); the HID input glue parses its
 * descriptor and registers the evdev devices it declares, as usb-hid does
 * for a USB device.  Every later write is one input report, which the glue
 * decodes into their events.  Closing the file unpublishes them, and the
 * input layer releases what they held: a daemon that ends, or loses its
 * link, leaves no key pressed.
 */

#include <drivers/generic/input-bridge.h>
#include <drivers/generic/hid-input.h>
#include <drivers/generic/hidraw.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/input-bridge.h>
#include <uapi/input.h>
#include <uapi/poll.h>

#include "kern/cdev.h"
#include "kern/clock.h"
#include "kern/cred.h"
#include "kern/file.h"
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/uaccess.h"

/* The device number of /dev/input/bridge (the next free one after bt-hci's 0x00120000). */
#define INPUT_BRIDGE_DEVICE_NUMBER	0x00130000U

/* The longest text the input layer takes, as the setup's fields: 63 bytes and a NUL. */
#define INPUT_BRIDGE_NAME_MAX	INPUT_BRIDGE_TEXT_MAX

/*
 * One open of the node: the device it declared, NULL until its first write
 * declares one.  It lives from open to close; the lock serializes the
 * writes and the requests of the open, and close comes after the last of
 * them.
 */
struct input_bridge_open {
	struct mutex lock;
	struct hid_input *input;
};

static int input_bridge_open(struct file *file);
static int input_bridge_close(struct file *file);
static ssize_t input_bridge_read(struct file *file, void *buffer, size_t size);
static ssize_t input_bridge_write(struct file *file, const void *buffer, size_t size);
static int input_bridge_ioctl(struct file *file, unsigned long request, uintptr_t argument);
static int input_bridge_poll(struct file *file, short requested, short *returned);
static int input_bridge_declare(struct input_bridge_open *state, const void *buffer, size_t size);
static int input_bridge_publish(struct input_bridge_open *state, const struct input_bridge_setup *setup);
static int input_bridge_report(struct input_bridge_open *state, const void *buffer, size_t size);
static void input_bridge_release_node(void);

/* The file operations of the node. */
static const struct cdev_ops input_bridge_ops = {
	.open = input_bridge_open,
	.close = input_bridge_close,
	.read = input_bridge_read,
	.write = input_bridge_write,
	.ioctl = input_bridge_ioctl,
	.poll = input_bridge_poll,
};

/*
 * The lock of input_bridge_busy.  Initialized when the node is registered, it
 * lives as long as the kernel.
 */
static struct mutex input_bridge_lock;

/*
 * How many opens hold the node: counted up by an open that admits itself
 * (at most INPUT_BRIDGE_OPENS_MAX), down when it closes or fails to finish
 * opening.  input_bridge_lock protects it.
 */
static unsigned input_bridge_busy;

/*
 * Publishes /dev/input/bridge.
 */
int
drv_input_bridge_register(
	void)
{
	int error;

	/* The lock of the opens' count. */
	error = mutex_init(&input_bridge_lock, LOCK_RANK_DEVICE, "input bridge");
	if (error != 0)
		return error;

	/* The character device. */
	error = cdev_register("bridge", (dev_t)INPUT_BRIDGE_DEVICE_NUMBER, &input_bridge_ops, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the node exists. */
	return 0;
}

/* Admits one root open with no device declared yet. */
static int
input_bridge_open(
	struct file *file)
{
	struct input_bridge_open *state;
	const struct ucred *credentials;
	int superuser;
	int busy;
	int error;

	/* Refuses anyone but root, whatever the node's mode says. */
	credentials = cred_current();
	superuser = cred_is_superuser(credentials);
	if (!superuser)
		return EPERM;

	/* Admits up to INPUT_BRIDGE_OPENS_MAX opens, each its own device. */
	mutex_lock(&input_bridge_lock);

	busy = 0;
	if (input_bridge_busy >= INPUT_BRIDGE_OPENS_MAX)
		busy = 1;
	else
		input_bridge_busy++;

	mutex_unlock(&input_bridge_lock);

	/* Refuses an open while all of them are held. */
	if (busy)
		return EBUSY;

	/* The open's state; without it the place is given back. */
	state = kern_malloc(sizeof(*state));
	if (state == NULL) {
		input_bridge_release_node();
		return ENOMEM;
	}

	/* No device yet, and the open's own lock. */
	kern_memset(state, 0, sizeof(*state));
	error = mutex_init(&state->lock, LOCK_RANK_DEVICE, "input bridge open");
	if (error != 0) {
		kern_free(state);
		input_bridge_release_node();
		return error;
	}

	/* Succeeded: the file holds the state until it closes. */
	file->f_data = state;
	return 0;
}

/* Removes the declared devices and gives the open's place back. */
static int
input_bridge_close(
	struct file *file)
{
	struct input_bridge_open *state;

	/* The devices go; the input layer releases what they held. */
	state = file->f_data;
	if (state != NULL) {
		if (state->input != NULL) {
			drv_hid_input_unpublish(state->input);
			drv_hid_input_destroy(state->input);
		}

		/* The open's state. */
		kern_free(state);
	}

	/* The file holds nothing any more. */
	file->f_data = NULL;

	/* The open's place. */
	input_bridge_release_node();

	/* Succeeded: the open is gone. */
	return 0;
}

/* Answers a read: no output report is passed back yet, and a read never waits. */
static ssize_t
input_bridge_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	(void)file;
	(void)buffer;
	(void)size;

	/* Nothing to read, now or later. */
	return -EAGAIN;
}

/* Declares the device on the first write, and takes one input report on each later one. */
static ssize_t
input_bridge_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct input_bridge_open *state;
	int error;

	/* The writes of one open are taken one at a time. */
	state = file->f_data;
	mutex_lock(&state->lock);

	/* The first write declares the device; every later one is a report. */
	if (state->input == NULL)
		error = input_bridge_declare(state, buffer, size);
	else
		error = input_bridge_report(state, buffer, size);

	mutex_unlock(&state->lock);

	/* Reports a refused write. */
	if (error != 0)
		return -error;

	/* Succeeded: the whole write was taken. */
	return (ssize_t)size;
}

/*
 * Answers INPUT_BRIDGE_GET_DEVICE: what the open made.  This is the one place
 * the request is answered (ws143-p005 Q2).
 */
static int
input_bridge_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct input_bridge_device device;
	struct input_bridge_open *state;
	int event;
	int touch_event;
	int numbered;
	int error;

	/* No other request. */
	if (request != INPUT_BRIDGE_GET_DEVICE)
		return ENOTTY;

	/* The open's devices, under its lock: none before the setup. */
	state = file->f_data;
	kern_memset(&device, 0, sizeof(device));
	device.event = -1;
	device.touch_event = -1;
	mutex_lock(&state->lock);

	if (state->input != NULL) {
		drv_hid_input_numbers(state->input, &event, &touch_event);
		device.event = event;
		device.touch_event = touch_event;
		device.malformed = drv_hid_input_malformed(state->input);
		device.report_max = (uint32_t)drv_hid_input_report_max(state->input);
		numbered = drv_hid_input_report_ids(state->input);
		if (numbered)
			device.flags |= INPUT_BRIDGE_FLAG_REPORT_IDS;
	}

	mutex_unlock(&state->lock);

	/* The answer, to the caller. */
	error = copyout(&device, argument, sizeof(device));
	if (error != 0)
		return error;

	/* Succeeded: the caller has the answer. */
	return 0;
}

/* Says what an open can do: write, always; never read. */
static int
input_bridge_poll(
	struct file *file,
	short requested,
	short *returned)
{
	(void)file;

	/* A caller that gives no place for the answer. */
	if (returned == NULL)
		return EINVAL;

	/* Writable only. */
	*returned = requested & (POLLOUT | POLLWRNORM);
	return 0;
}

/*
 * Declares the open's device from one setup, exactly its size.  A refusal
 * leaves the open without a device, so it may declare again.
 */
static int
input_bridge_declare(
	struct input_bridge_open *state,
	const void *buffer,
	size_t size)
{
	struct input_bridge_setup *setup;
	int valid;
	int error;

	/* The whole setup and nothing else. */
	if (size != sizeof(*setup))
		return EINVAL;

	/* A copy of its own, aligned and kept off the stack (it is over 4 KiB). */
	setup = kern_malloc(sizeof(*setup));
	if (setup == NULL)
		return ENOMEM;
	kern_memcpy(setup, buffer, sizeof(*setup));

	/* A well formed setup. */
	valid = drv_input_bridge_setup_valid(setup);
	if (!valid) {
		kern_free(setup);
		return EINVAL;
	}

	/* The devices it declares. */
	error = input_bridge_publish(state, setup);
	kern_free(setup);
	if (error != 0)
		return error;

	/* Succeeded: the open has its device. */
	return 0;
}

/*
 * Parses a well formed setup's descriptor and registers its devices under
 * the setup's names.  ENXIO for a FIDO descriptor; the glue's error
 * otherwise, with nothing left registered.
 */
static int
input_bridge_publish(
	struct input_bridge_open *state,
	const struct input_bridge_setup *setup)
{
	struct drv_hidraw_layout raw;
	struct hid_input_identity identity;
	struct hid_input *input;
	char name[INPUT_BRIDGE_NAME_MAX];
	char touch_name[INPUT_BRIDGE_NAME_MAX];
	char touch_physical_path[INPUT_BRIDGE_NAME_MAX];
	const char *bus_name;
	unsigned kind;
	int described;
	int event;
	int touch_event;
	int error;

	/* A security key is not an input device: its page is FIDO's (hidraw's, ws161). */
	described = drv_hidraw_describe(setup->descriptor, setup->descriptor_size, &raw);
	if (described == 0 && raw.usage_page == HIDRAW_USAGE_PAGE_FIDO)
		return ENXIO;

	/* The devices the descriptor declares. */
	error = drv_hid_input_prepare(setup->descriptor, setup->descriptor_size, &input);
	if (error != 0)
		return error;

	/* The device's name, or one after what it chiefly is. */
	bus_name = "Bluetooth";
	if (setup->bus == BUS_VIRTUAL)
		bus_name = "Virtual";
	kind = drv_hid_input_kind(input);
	if (setup->name[0] != '\0') {
		(void)kern_snprintf(name, sizeof(name), "%s", setup->name);
	} else if (kind == HID_INPUT_KIND_PEN) {
		(void)kern_snprintf(name, sizeof(name), "%s HID pen", bus_name);
	} else if (kind == HID_INPUT_KIND_TABLET) {
		(void)kern_snprintf(name, sizeof(name), "%s HID tablet", bus_name);
	} else if (kind == HID_INPUT_KIND_MOUSE) {
		(void)kern_snprintf(name, sizeof(name), "%s HID mouse", bus_name);
	} else {
		(void)kern_snprintf(name, sizeof(name), "%s HID keyboard", bus_name);
	}

	/* The touch device's name and place, cut to the input layer's 63 bytes as usb-hid's are. */
	(void)kern_snprintf(touch_name, sizeof(touch_name), "%s Touchscreen", name);
	(void)kern_snprintf(touch_physical_path, sizeof(touch_physical_path), "%s/touch", setup->physical_path);

	/* The identity the devices are registered under. */
	kern_memset(&identity, 0, sizeof(identity));
	identity.name = name;
	identity.physical_path = setup->physical_path;
	identity.unique_id = setup->unique_id;
	identity.touch_name = touch_name;
	identity.touch_physical_path = touch_physical_path;
	identity.id.bustype = setup->bus;
	identity.id.vendor = setup->vendor;
	identity.id.product = setup->product;
	identity.id.version = setup->release;

	/* The devices; a refusal frees the description, and the open may declare again. */
	error = drv_hid_input_publish(input, &identity);
	if (error != 0) {
		drv_hid_input_destroy(input);
		return error;
	}

	/* The open's device from now on. */
	state->input = input;
	drv_hid_input_numbers(input, &event, &touch_event);
	kern_logf("input-bridge: %s: event=%d touch=%d bus=%u name=%s\n",
		  setup->physical_path,
		  event,
		  touch_event,
		  (unsigned)setup->bus,
		  name);

	/* Succeeded: the devices are published. */
	return 0;
}

/*
 * Takes one input report: refused when empty, longer than
 * INPUT_BRIDGE_REPORT_MAX or shorter than its report ID declares; otherwise the
 * glue decodes it (counting and dropping one it cannot) at the time it came.
 */
static int
input_bridge_report(
	struct input_bridge_open *state,
	const void *buffer,
	size_t size)
{
	uint64_t milliseconds;
	int short_report;

	/* A report of a length no device sends in one write. */
	if (size == 0U)
		return EINVAL;
	if (size > INPUT_BRIDGE_REPORT_MAX)
		return EINVAL;

	/* A piece of a report, not a whole one. */
	short_report = drv_hid_input_report_short(state->input, buffer, size);
	if (short_report)
		return EINVAL;

	/* The report, at the time it came. */
	milliseconds = clock_milliseconds(NULL);
	drv_hid_input_report(state->input, buffer, size, milliseconds);

	/* Succeeded: the report was handed on. */
	return 0;
}

/* Gives one open's place back. */
static void
input_bridge_release_node(
	void)
{
	/* One open fewer: the next open may take its place. */
	mutex_lock(&input_bridge_lock);

	if (input_bridge_busy > 0U)
		input_bridge_busy--;

	mutex_unlock(&input_bridge_lock);
}
