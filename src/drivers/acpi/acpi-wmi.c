/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI-to-WMI mapping: the blocks the WMI mapping devices (PNP0C14)
 * list in _WDG, and the calls of their control methods for drivers.
 *
 * At attach, a device's _WDG is evaluated only when it is data (a Name):
 * on some machines _WDG is a method that enters the firmware's System
 * Management Mode, and attaching must not.  A driver that knows a device
 * whose _WDG method only returns data names it with
 * drv_acpi_wmi_attach_path().  Nothing here enters SMM by itself; a block's
 * method or query does whatever its AML does, at the driver's request.
 *
 * The blocks are kept in one table for the life of the system.  Attaching
 * appends to it under the interpreter lock (no AML runs while it is held
 * here); a published block never changes, so readers take the published
 * count and read the blocks below it without a lock.
 */

#include <drivers/acpi/acpi.h>
#include <drivers/acpi/wmi.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aml-os.h"

/* The most blocks and mapping devices the layer keeps. */
#define WMI_BLOCKS_MAX		48U
#define WMI_DEVICES_MAX		16U

/* PNP0C14, the WMI mapping device, as _HID encodes it as an EISA identifier. */
#define WMI_EISA_ID		0x140cd041ULL

/* _STA: present. */
#define WMI_STATUS_PRESENT	0x01U

/* The room for a device's path in the log. */
#define WMI_PATH_MAX		64U

/* The arguments of WMxx: the instance index, the method ID and the input buffer. */
#define WMI_METHOD_ARGUMENTS	3U

/*
 * The mapping devices one walk of the namespace found, before their _WDG
 * is read.
 */
struct wmi_scan {
	struct drv_acpi_node *devices[WMI_DEVICES_MAX];
	unsigned count;
};

/*
 * The blocks, in the order the devices and their _WDG list them.
 *
 * A slot below wmi_block_count is published and never changes.  Only an
 * attach, holding the interpreter lock, fills the slots above it and then
 * moves the count up with a release store; readers load the count with
 * an acquire load and read only below it.
 */
static struct drv_acpi_wmi_block wmi_blocks[WMI_BLOCKS_MAX];
static unsigned wmi_block_count;

/*
 * The devices whose _WDG is in the table, so that a second attach skips
 * them.  Read and written only by an attach holding the interpreter lock.
 */
static struct drv_acpi_node *wmi_devices[WMI_DEVICES_MAX];
static unsigned wmi_device_count;

static int wmi_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool wmi_is_mapping_device(struct drv_acpi_node *node);
static bool wmi_present(struct drv_acpi_node *node);
static int wmi_attach_device(struct drv_acpi_node *device, bool method_allowed);
static int wmi_publish(struct drv_acpi_node *device, const uint8_t *bytes, size_t length, unsigned *added);
static bool wmi_device_known(struct drv_acpi_node *device);
static int wmi_method_arguments(uint32_t instance, uint32_t method_id, const void *input, size_t size, struct drv_acpi_object **arguments);
static void wmi_collection(const struct drv_acpi_wmi_block *block, uint64_t enable);
static void wmi_device_path(struct drv_acpi_node *device, char *path);

/*
 * Lists the blocks of every present WMI mapping device whose _WDG is data.
 *
 * A device whose _WDG is a method is passed over (it is logged), and so is
 * one already listed, so the call may be repeated to retry a device that
 * failed.  Returns 0 when some device's blocks are listed, or ENODEV when
 * none could be.
 */
int
drv_acpi_wmi_attach(void)
{
	struct wmi_scan scan;
	unsigned index;
	unsigned listed;
	int error;

	/* Finds the present mapping devices. */
	kern_memset(&scan, 0, sizeof(scan));
	(void)drv_acpi_walk(NULL, wmi_visitor, &scan);

	/* Lists each one's blocks; a failure is logged and leaves the others. */
	listed = 0;
	for (index = 0; index < scan.count; index++) {
		error = wmi_attach_device(scan.devices[index], false);
		if (error == 0)
			listed++;
	}

	/* Reports a machine where no device's blocks could be listed. */
	if (listed == 0U)
		return ENODEV;

	/* Succeeded: the blocks are listed. */
	return 0;
}

/*
 * Lists the blocks of the WMI mapping device at path, even when its _WDG is
 * a method.
 *
 * Only for a device whose _WDG method the caller knows to return data
 * without entering the firmware.  Returns 0 (also when the device is
 * already listed), ENOENT when nothing is at path, ENODEV when the node
 * is not a present mapping device, or the error of reading _WDG.
 */
int
drv_acpi_wmi_attach_path(
	const char *path)
{
	struct drv_acpi_node *device;
	bool mapping;
	bool present;
	int error;

	/* Finds the node. */
	device = NULL;
	error = drv_acpi_lookup(NULL, path, &device);
	if (error != 0)
		return error;
	if (device == NULL)
		return ENOENT;

	/* Refuses a node that is not a present mapping device. */
	mapping = wmi_is_mapping_device(device);
	if (!mapping)
		return ENODEV;
	present = wmi_present(device);
	if (!present)
		return ENODEV;

	/* Lists its blocks, evaluating a method _WDG. */
	error = wmi_attach_device(device, true);
	if (error != 0)
		return error;

	/* Succeeded: the device's blocks are listed. */
	return 0;
}

/*
 * Reports how many blocks are listed.
 */
unsigned
drv_acpi_wmi_block_count(void)
{
	unsigned count;

	/* Takes the published count; the blocks below it are complete. */
	count = __atomic_load_n(&wmi_block_count, __ATOMIC_ACQUIRE);

	/* Reports the count. */
	return count;
}

/*
 * Lends the listed block at index, or NULL past the last one.
 */
const struct drv_acpi_wmi_block *
drv_acpi_wmi_block_at(
	unsigned index)
{
	unsigned count;

	/* Refuses an index past the published blocks. */
	count = __atomic_load_n(&wmi_block_count, __ATOMIC_ACQUIRE);
	if (index >= count)
		return NULL;

	/* Lends the block, which never changes. */
	return &wmi_blocks[index];
}

/*
 * Finds the nth listed block (from 0) whose GUID is guid_text.
 *
 * Returns 0, EINVAL when guid_text is not a GUID, or ENOENT when fewer
 * blocks carry it.
 */
int
drv_acpi_wmi_find(
	const char *guid_text,
	unsigned nth,
	const struct drv_acpi_wmi_block **block)
{
	uint8_t guid[DRV_ACPI_WMI_GUID_SIZE];
	unsigned count;
	unsigned index;
	unsigned seen;
	int compared;
	int error;

	/* Starts with no block. */
	*block = NULL;

	/* Converts the text into _WDG's bytes. */
	error = drv_acpi_wmi_guid_parse(guid_text, guid);
	if (error != 0)
		return error;

	/* Walks the published blocks for the nth one with the GUID. */
	count = __atomic_load_n(&wmi_block_count, __ATOMIC_ACQUIRE);
	seen = 0;
	for (index = 0; index < count; index++) {
		/* Passes over a block with another GUID. */
		compared = kern_memcmp(wmi_blocks[index].entry.guid, guid, DRV_ACPI_WMI_GUID_SIZE);
		if (compared != 0)
			continue;

		/* Passes over the ones before the nth. */
		if (seen < nth) {
			seen++;
			continue;
		}

		/* Lends the nth. */
		*block = &wmi_blocks[index];
		return 0;
	}

	/* Reports that fewer blocks carry the GUID. */
	return ENOENT;
}

/*
 * Calls a method of a method block: WMxx(instance, method_id, input).
 *
 * input is passed as a buffer of size bytes (a copy: the AML may change
 * its argument).  result is what WMxx returned, NULL when nothing, and the
 * caller releases it.  Returns 0; EINVAL for a block that is not a method
 * block, an instance past the block's count or a missing input; E2BIG for
 * an input larger than DRV_ACPI_WMI_INPUT_MAX; ENOMEM; or the error of the
 * evaluation (ENOENT when the device has no WMxx).
 */
int
drv_acpi_wmi_method(
	const struct drv_acpi_wmi_block *block,
	uint32_t instance,
	uint32_t method_id,
	const void *input,
	size_t size,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *arguments[WMI_METHOD_ARGUMENTS];
	char name[5];
	unsigned index;
	int error;

	/* Starts with no result. */
	*result = NULL;

	/* Refuses a block that is an event or a data block. */
	if ((block->entry.flags & DRV_ACPI_WMI_FLAG_EVENT) != 0U)
		return EINVAL;
	if ((block->entry.flags & DRV_ACPI_WMI_FLAG_METHOD) == 0U)
		return EINVAL;

	/* Refuses an instance the block does not have. */
	if (instance >= block->entry.instance_count)
		return EINVAL;

	/* Refuses an input that is missing or too large. */
	if (input == NULL && size != 0U)
		return EINVAL;
	if (size > DRV_ACPI_WMI_INPUT_MAX)
		return E2BIG;

	/* Makes the method's name from the block's identifier. */
	error = drv_acpi_wmi_control_name(&block->entry, 'W', 'M', name);
	if (error != 0)
		return error;

	/* Makes WMxx's three arguments. */
	error = wmi_method_arguments(instance, method_id, input, size, arguments);
	if (error != 0)
		return error;

	/* Runs WMxx on the mapping device. */
	error = drv_acpi_evaluate(block->device, name, arguments, WMI_METHOD_ARGUMENTS, result);

	/* Gives the arguments back; the AML kept its own references. */
	for (index = 0; index < WMI_METHOD_ARGUMENTS; index++)
		drv_acpi_object_release(arguments[index]);

	/* Reports a failed call. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds what WMxx returned, if anything. */
	return 0;
}

/*
 * Queries an instance of a data block: WQxx(instance).
 *
 * A WQxx that is data rather than a method is read as it is.  A block
 * flagged EXPENSIVE has its collection enabled around the query (WCxx(1),
 * then WCxx(0)) when the device has a WCxx.  result is the block's
 * contents, and the caller releases it.  Returns 0; EINVAL for a method
 * block, an event or an instance past the block's count; ENOENT when the
 * device has no WQxx (a block _WDG lists stays listed without one); or the
 * error of the evaluation.
 */
int
drv_acpi_wmi_query(
	const struct drv_acpi_wmi_block *block,
	uint32_t instance,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *argument;
	struct drv_acpi_node *query;
	enum drv_acpi_type type;
	char name[5];
	bool expensive;
	int error;

	/* Starts with no result. */
	*result = NULL;

	/* Refuses an event and a method block. */
	if ((block->entry.flags & DRV_ACPI_WMI_FLAG_EVENT) != 0U)
		return EINVAL;
	if ((block->entry.flags & DRV_ACPI_WMI_FLAG_METHOD) != 0U)
		return EINVAL;

	/* Refuses an instance the block does not have. */
	if (instance >= block->entry.instance_count)
		return EINVAL;

	/* Finds WQxx on the mapping device. */
	error = drv_acpi_wmi_control_name(&block->entry, 'W', 'Q', name);
	if (error != 0)
		return error;
	query = NULL;
	error = drv_acpi_lookup(block->device, name, &query);
	if (error != 0)
		return error;
	if (query == NULL)
		return ENOENT;

	/* Makes the instance index, the method's one argument. */
	argument = drv_acpi_object_integer_new(instance);
	if (argument == NULL)
		return ENOMEM;

	/*
	 * Procedure referenced: Microsoft, "Windows Instrumentation: WMI and
	 * ACPI" (dn614028), "Data Collection Enable and Disable Control
	 * Method": for a block flagged EXPENSIVE, WCxx is invoked with a
	 * nonzero value before its data is collected and with 0 after.
	 */
	expensive = false;
	if ((block->entry.flags & DRV_ACPI_WMI_FLAG_EXPENSIVE) != 0U)
		expensive = true;
	if (expensive)
		wmi_collection(block, 1U);

	/*
	 * Procedure referenced: the same document, "Data Block Query Control
	 * Method": WQxx takes the instance index and evaluates to the
	 * instance's contents.  Firmware also declares WQxx as a Name.
	 */
	type = drv_acpi_node_type(query);
	if (type == DRV_ACPI_TYPE_METHOD) {
		error = drv_acpi_evaluate(query, NULL, &argument, 1U, result);
	} else {
		error = drv_acpi_evaluate(query, NULL, NULL, 0U, result);
	}

	/* Disables the collection again. */
	if (expensive)
		wmi_collection(block, 0U);

	/* Gives the argument back. */
	drv_acpi_object_release(argument);

	/* Reports a failed query. */
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the instance's contents. */
	return 0;
}

/* Takes a present WMI mapping device into the scan. */
static int
wmi_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct wmi_scan *scan;
	enum drv_acpi_type type;
	bool mapping;
	bool present;

	/* The depth does not matter. */
	(void)depth;
	scan = argument;

	/* Only a device can be a mapping device. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* A device whose _HID is PNP0C14, and present. */
	mapping = wmi_is_mapping_device(node);
	if (!mapping)
		return 0;
	present = wmi_present(node);
	if (!present)
		return 0;

	/* A full scan takes no more; the walk stops. */
	if (scan->count >= WMI_DEVICES_MAX) {
		drv_acpi_os_log("acpi: WMI: more than %u mapping devices, the rest are not listed\n", WMI_DEVICES_MAX);
		return -1;
	}

	/* The scan holds it. */
	scan->devices[scan->count] = node;
	scan->count++;

	/* Goes on with the next node. */
	return 0;
}

/* Tells whether a node's _HID is PNP0C14, as an EISA identifier or a string. */
static bool
wmi_is_mapping_device(
	struct drv_acpi_node *node)
{
	struct drv_acpi_object *hid;
	enum drv_acpi_type type;
	const char *text;
	uint64_t value;
	bool matched;
	int compared;
	int error;

	/* Evaluates the _HID; a node without one is not a mapping device. */
	hid = NULL;
	error = drv_acpi_evaluate(node, "_HID", NULL, 0U, &hid);
	if (error != 0)
		return false;
	if (hid == NULL)
		return false;

	/* Compares it in its form. */
	matched = false;
	type = drv_acpi_object_type(hid);
	if (type == DRV_ACPI_TYPE_INTEGER) {
		value = drv_acpi_object_integer(hid);
		if (value == WMI_EISA_ID)
			matched = true;
	} else if (type == DRV_ACPI_TYPE_STRING) {
		text = drv_acpi_object_string(hid, NULL);
		compared = kern_strcmp(text, "PNP0C14");
		if (compared == 0)
			matched = true;
	}

	/* The _HID is no longer needed. */
	drv_acpi_object_release(hid);

	/* Reports whether the _HID named PNP0C14. */
	return matched;
}

/* Tells whether a device is present: its _STA's present bit, or no _STA at all. */
static bool
wmi_present(
	struct drv_acpi_node *node)
{
	uint64_t status;
	int error;

	/* Without a _STA a device is present. */
	error = drv_acpi_evaluate_integer(node, "_STA", &status);
	if (error != 0)
		return true;

	/* Succeeded: its present bit. */
	if ((status & WMI_STATUS_PRESENT) == 0U)
		return false;
	return true;
}

/*
 * Reads a mapping device's _WDG and lists its blocks.
 *
 * A _WDG that is a method is evaluated only when method_allowed; one that
 * is neither a method nor a buffer is refused.  The outcome is logged.
 */
static int
wmi_attach_device(
	struct drv_acpi_node *device,
	bool method_allowed)
{
	struct drv_acpi_object *object;
	struct drv_acpi_node *wdg;
	enum drv_acpi_type type;
	const uint8_t *bytes;
	char path[WMI_PATH_MAX];
	size_t length;
	unsigned added;
	int error;

	/* Names the device for the log. */
	wmi_device_path(device, path);

	/* Finds _WDG, which every mapping device has. */
	wdg = NULL;
	error = drv_acpi_lookup(device, "_WDG", &wdg);
	if (error != 0 || wdg == NULL) {
		drv_acpi_os_log("acpi: WMI %s has no _WDG\n", path);
		return ENOENT;
	}

	/* Passes over a method _WDG unless the caller vouched for it, and refuses anything but a buffer. */
	type = drv_acpi_node_type(wdg);
	if (type == DRV_ACPI_TYPE_METHOD) {
		if (!method_allowed) {
			drv_acpi_os_log("acpi: WMI %s: _WDG is a method, not evaluated\n", path);
			return EOPNOTSUPP;
		}
	} else if (type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_os_log("acpi: WMI %s: _WDG is not a buffer (type %u)\n", path, (unsigned)type);
		return EINVAL;
	}

	/* Reads _WDG. */
	object = NULL;
	error = drv_acpi_evaluate(wdg, NULL, NULL, 0U, &object);
	if (error != 0) {
		drv_acpi_os_log("acpi: WMI %s: _WDG failed (error %d)\n", path, error);
		return error;
	}

	/* Refuses a method _WDG that returned nothing. */
	if (object == NULL) {
		drv_acpi_os_log("acpi: WMI %s: _WDG returned nothing\n", path);
		return EINVAL;
	}

	/* Refuses a _WDG that did not give a buffer. */
	type = drv_acpi_object_type(object);
	if (type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_os_log("acpi: WMI %s: _WDG is not a buffer (type %u)\n", path, (unsigned)type);
		drv_acpi_object_release(object);
		return EINVAL;
	}

	/* Lists the entries, then gives _WDG back. */
	bytes = drv_acpi_object_buffer(object, &length);
	added = 0;
	error = wmi_publish(device, bytes, length, &added);
	drv_acpi_object_release(object);

	/* Reports the outcome in the log. */
	if (error == EEXIST) {
		return 0;
	} else if (error == E2BIG) {
		drv_acpi_os_log("acpi: WMI %s: %u blocks listed, the table is full\n", path, added);
		return 0;
	} else if (error != 0) {
		drv_acpi_os_log("acpi: WMI %s: _WDG of %u bytes is refused (error %d)\n", path, (unsigned)length, error);
		return error;
	}

	/* Says how many blocks the device listed. */
	drv_acpi_os_log("acpi: WMI %s: %u blocks\n", path, added);

	/* Succeeded: the device's blocks are listed. */
	return 0;
}

/*
 * Appends a device's _WDG entries to the table and publishes them.
 *
 * Runs under the interpreter lock, which serializes attaches; no AML runs
 * here.  added says how many blocks were published.  Returns 0, EEXIST
 * when the device is already listed, EINVAL for a _WDG cut inside an
 * entry, ENOSPC when no device or block fits, or E2BIG when only some of
 * the entries fit (those are published).
 */
static int
wmi_publish(
	struct drv_acpi_node *device,
	const uint8_t *bytes,
	size_t length,
	unsigned *added)
{
	struct drv_acpi_wmi_entry entries[WMI_BLOCKS_MAX];
	unsigned count;
	unsigned room;
	unsigned index;
	bool known;
	int parsed;

	/* Starts with nothing published. */
	*added = 0;

	/* Decodes the entries before taking the lock. */
	count = 0;
	parsed = drv_acpi_wmi_entries_parse(bytes, length, entries, WMI_BLOCKS_MAX, &count);
	if (parsed == EINVAL)
		return EINVAL;

	/* Appends them as the only attach running. */
	drv_acpi_os_lock();

	/* Refuses a device already listed. */
	known = wmi_device_known(device);
	if (known) {
		drv_acpi_os_unlock();
		return EEXIST;
	}

	/* Refuses a device past the room for devices. */
	if (wmi_device_count >= WMI_DEVICES_MAX) {
		drv_acpi_os_unlock();
		return ENOSPC;
	}

	/* Cuts the entries to the room for blocks. */
	room = WMI_BLOCKS_MAX - wmi_block_count;
	if (count > room) {
		count = room;
		parsed = E2BIG;
	}

	/* Fills the slots above the published count. */
	for (index = 0; index < count; index++) {
		wmi_blocks[wmi_block_count + index].device = device;
		wmi_blocks[wmi_block_count + index].entry = entries[index];
	}

	/*
	 * The release store publishes the filled slots: a reader that loads
	 * the new count with an acquire load sees them complete.
	 */
	__atomic_store_n(&wmi_block_count, wmi_block_count + count, __ATOMIC_RELEASE);
	wmi_devices[wmi_device_count] = device;
	wmi_device_count++;

	drv_acpi_os_unlock();

	/* Reports the entries that did not fit. */
	*added = count;
	if (parsed == E2BIG)
		return E2BIG;

	/* Succeeded: every entry is published. */
	return 0;
}

/* Tells whether a device's blocks are already listed (under the interpreter lock). */
static bool
wmi_device_known(
	struct drv_acpi_node *device)
{
	unsigned index;

	/* Looks for the device among the listed ones. */
	for (index = 0; index < wmi_device_count; index++) {
		if (wmi_devices[index] == device)
			return true;
	}

	/* Not listed. */
	return false;
}

/*
 * Makes the arguments of WMxx: the instance index, the method ID and a
 * buffer with a copy of the input.  Returns 0, or ENOMEM after giving
 * back the ones made.
 */
static int
wmi_method_arguments(
	uint32_t instance,
	uint32_t method_id,
	const void *input,
	size_t size,
	struct drv_acpi_object **arguments)
{
	/*
	 * Procedure referenced: Microsoft, "Windows Instrumentation: WMI and
	 * ACPI" (dn614028), "Method Execution Control Method": WMxx takes
	 * the instance index, the method ID, and a buffer with the method's
	 * input, and returns a buffer with its result.
	 */
	arguments[0] = drv_acpi_object_integer_new(instance);
	if (arguments[0] == NULL)
		return ENOMEM;

	/* The method ID. */
	arguments[1] = drv_acpi_object_integer_new(method_id);
	if (arguments[1] == NULL) {
		drv_acpi_object_release(arguments[0]);
		return ENOMEM;
	}

	/* The input, copied: the AML may change its argument. */
	arguments[2] = drv_acpi_object_buffer_new(input, size);
	if (arguments[2] == NULL) {
		drv_acpi_object_release(arguments[1]);
		drv_acpi_object_release(arguments[0]);
		return ENOMEM;
	}

	/* Succeeded: the caller holds the three arguments. */
	return 0;
}

/* Enables (1) or disables (0) the collection of a data block by its WCxx, when the device has one. */
static void
wmi_collection(
	const struct drv_acpi_wmi_block *block,
	uint64_t enable)
{
	struct drv_acpi_object *argument;
	struct drv_acpi_object *ignored;
	char name[5];
	int error;

	/* Makes WCxx's name. */
	error = drv_acpi_wmi_control_name(&block->entry, 'W', 'C', name);
	if (error != 0)
		return;

	/* Makes the argument. */
	argument = drv_acpi_object_integer_new(enable);
	if (argument == NULL)
		return;

	/* Runs WCxx; a device without one needs none. */
	ignored = NULL;
	error = drv_acpi_evaluate(block->device, name, &argument, 1U, &ignored);
	if (error == 0 && ignored != NULL)
		drv_acpi_object_release(ignored);

	/* Gives the argument back. */
	drv_acpi_object_release(argument);
}

/* Writes a device's path for the log, cut to WMI_PATH_MAX. */
static void
wmi_device_path(
	struct drv_acpi_node *device,
	char *path)
{
	int error;

	/* The path, or a mark when it cannot be written. */
	error = drv_acpi_node_path(device, path, WMI_PATH_MAX);
	if (error != 0) {
		path[0] = '?';
		path[1] = '\0';
	}

	/* A path cut at the room still ends. */
	path[WMI_PATH_MAX - 1U] = '\0';
}
