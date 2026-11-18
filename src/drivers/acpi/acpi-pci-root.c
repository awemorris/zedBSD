/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The address ranges a PCI BAR may be placed in, and those it must stay
 * clear of, as the namespace gives them (BUG-210).
 *
 * The host bridge of a PCI segment and bus is the PNP0A03 or PNP0A08
 * device whose _SEG and _BBN (zero when absent) name them; its _CRS lists
 * the windows it forwards to the bus.  The motherboard resource devices
 * (PNP0C01, PNP0C02) list the ranges the chipset decodes itself, which no
 * BAR may take: Intel's hide devices of the PCH there, and keep their _STA
 * at "functioning, not present" (0x08), so a device is passed over only
 * when it is neither present nor functioning.  Both are read with the
 * resource template walker (acpi-resource.c).
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>

#include "aml-internal.h"

/* The EISA identifiers of the host bridges and the motherboard resource devices. */
#define ROOT_PCI_EISA_ID	0x030ad041ULL
#define ROOT_PCIE_EISA_ID	0x080ad041ULL
#define SYSTEM_PNP0C01_EISA_ID	0x010cd041ULL
#define SYSTEM_PNP0C02_EISA_ID	0x020cd041ULL

/* The _STA bits of a present device and of a functioning one. */
#define STATUS_PRESENT		0x01U
#define STATUS_FUNCTIONING	0x08U

/* The most motherboard resource devices whose ranges are read. */
#define SYSTEM_DEVICES_MAX	32U

/*
 * The kinds of device the namespace walks look for.
 */
enum device_kind {
	DEVICE_PCI_ROOT,
	DEVICE_SYSTEM
};

/*
 * What the walk for a host bridge looks for, and the bridge it found.
 */
struct root_search {
	uint16_t segment;
	uint8_t bus;
	struct drv_acpi_node *found;
};

/*
 * The motherboard resource devices the walk found, the first count used.
 * Their _CRS is read after the walk, so that a _CRS method that makes
 * names of its own does not change the namespace while it is walked.
 */
struct system_search {
	struct drv_acpi_node *nodes[SYSTEM_DEVICES_MAX];
	unsigned count;
};

static int root_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static int system_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool device_is(struct drv_acpi_node *node, enum device_kind kind);
static bool identifier_is(const struct drv_acpi_object *object, enum device_kind kind);
static bool eisa_is(uint64_t value, enum device_kind kind);
static bool string_is(const char *text, enum device_kind kind);
static bool device_in_use(struct drv_acpi_node *node);
static uint64_t own_integer(struct drv_acpi_node *node, const char *name);

/*
 * Walks the resources of the host bridge of a PCI segment and bus.
 *
 * Reports ENOENT when the namespace has no such bridge, and otherwise what
 * drv_acpi_resources_walk() reports for its _CRS.
 */
int
drv_acpi_pci_root_resources_walk(
	uint16_t segment,
	uint8_t bus,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct root_search search;
	int error;

	/* Refuses a missing visitor. */
	if (visitor == NULL)
		return EINVAL;

	/* Looks for the bridge with the interpreter held, so the namespace stays as it is. */
	kern_memset(&search, 0, sizeof(search));
	search.segment = segment;
	search.bus = bus;
	thread = drv_acpi_enter(&storage);
	(void)drv_acpi_walk(NULL, root_visitor, &search);
	drv_acpi_leave(thread);

	/* A namespace without the bridge has no windows to give. */
	if (search.found == NULL)
		return ENOENT;

	/* Walks the bridge's _CRS. */
	error = drv_acpi_resources_walk(search.found, NULL, visitor, argument);
	if (error != 0)
		return error;

	/* Succeeded: the visitor saw every resource of the bridge. */
	return 0;
}

/*
 * Walks the resources of every motherboard resource device in use.
 *
 * A device whose _CRS cannot be read is passed over; the walk reports
 * the visitor's value when the visitor stops it.
 */
int
drv_acpi_system_resources_walk(
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct system_search search;
	unsigned index;
	int error;

	/* Refuses a missing visitor. */
	if (visitor == NULL)
		return EINVAL;

	/* Lists the devices with the interpreter held, so the namespace stays as it is. */
	kern_memset(&search, 0, sizeof(search));
	thread = drv_acpi_enter(&storage);
	(void)drv_acpi_walk(NULL, system_visitor, &search);
	drv_acpi_leave(thread);

	/* Walks each device's _CRS. */
	error = 0;
	for (index = 0; index < search.count; index++) {
		/* A template that cannot be read is passed over; a visitor's stop ends the walk. */
		error = drv_acpi_resources_walk(search.nodes[index], NULL, visitor, argument);
		if (error < 0)
			break;
		error = 0;
	}

	/* Reports the visitor's stop. */
	if (error != 0)
		return error;

	/* Succeeded: the visitor saw the ranges of every device in use. */
	return 0;
}

/* Stops the walk at the host bridge of the segment and bus looked for. */
static int
root_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct root_search *search;
	enum drv_acpi_type type;
	uint64_t segment;
	uint64_t bus;
	bool root;

	UNUSED_PARAMETER(depth);

	/* Only a device can be a host bridge. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* Goes on below a device that is no host bridge. */
	root = device_is(node, DEVICE_PCI_ROOT);
	if (!root)
		return 0;

	/* Reads the bridge's segment and bus, zero when it gives none. */
	segment = own_integer(node, "_SEG");
	bus = own_integer(node, "_BBN");

	/* Keeps the bridge looked for and ends the walk. */
	search = argument;
	if (segment == search->segment && bus == search->bus) {
		search->found = node;
		return -1;
	}

	/* Skips below another bridge: host bridges are not nested. */
	return 1;
}

/* Keeps each motherboard resource device in use. */
static int
system_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct system_search *search;
	enum drv_acpi_type type;
	bool system;
	bool in_use;

	UNUSED_PARAMETER(depth);

	/* Only a device can be one. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* Passes over a device of another kind, or one neither present nor functioning. */
	system = device_is(node, DEVICE_SYSTEM);
	if (!system)
		return 0;
	in_use = device_in_use(node);
	if (!in_use)
		return 0;

	/* Keeps the device; a full list ends the walk. */
	search = argument;
	search->nodes[search->count] = node;
	search->count++;
	if (search->count >= SYSTEM_DEVICES_MAX)
		return -1;

	/* Goes on with the next device. */
	return 0;
}

/* Tells whether a device's own _HID or _CID names a device of the kind. */
static bool
device_is(
	struct drv_acpi_node *node,
	enum device_kind kind)
{
	struct drv_acpi_node *found;
	struct drv_acpi_object *object;
	struct drv_acpi_object *element;
	enum drv_acpi_type type;
	unsigned count;
	unsigned index;
	bool matched;
	int error;

	/* The hardware identifier first, when the device has its own. */
	error = drv_acpi_lookup(node, "_HID", &found);
	if (error == 0 && found != NULL) {
		/* A _HID of the kind is enough. */
		object = NULL;
		error = drv_acpi_evaluate(found, NULL, NULL, 0, &object);
		if (error == 0) {
			matched = identifier_is(object, kind);
			drv_acpi_object_release(object);
			if (matched)
				return true;
		}
	}

	/* Then the compatible identifiers, when the device has its own. */
	error = drv_acpi_lookup(node, "_CID", &found);
	if (error != 0 || found == NULL)
		return false;
	object = NULL;
	error = drv_acpi_evaluate(found, NULL, NULL, 0, &object);
	if (error != 0 || object == NULL)
		return false;

	/* A package names several; any of them may be of the kind. */
	matched = false;
	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_PACKAGE) {
		count = drv_acpi_object_package_count(object);
		for (index = 0; index < count; index++) {
			/* The first identifier of the kind is enough. */
			element = drv_acpi_object_package_element(object, index);
			matched = identifier_is(element, kind);
			if (matched)
				break;
		}
	} else {
		matched = identifier_is(object, kind);
	}

	/* The object is no longer needed. */
	drv_acpi_object_release(object);

	/* Succeeded: whether the device is of the kind. */
	return matched;
}

/* Tells whether an identifier object, as an EISA identifier or a string, is of the kind. */
static bool
identifier_is(
	const struct drv_acpi_object *object,
	enum device_kind kind)
{
	enum drv_acpi_type type;
	const char *text;
	uint64_t value;
	size_t length;
	bool matched;

	/* No object names nothing. */
	if (object == NULL)
		return false;

	/* The EISA form. */
	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_INTEGER) {
		value = drv_acpi_object_integer(object);
		matched = eisa_is(value, kind);
		return matched;
	}

	/* The string form. */
	if (type != DRV_ACPI_TYPE_STRING)
		return false;
	text = drv_acpi_object_string(object, &length);
	if (text == NULL)
		return false;
	matched = string_is(text, kind);

	/* Succeeded: whether the string is of the kind. */
	return matched;
}

/* Tells whether an EISA identifier is of the kind. */
static bool
eisa_is(
	uint64_t value,
	enum device_kind kind)
{
	/* A host bridge is PNP0A03 or PNP0A08. */
	if (kind == DEVICE_PCI_ROOT) {
		if (value == ROOT_PCI_EISA_ID)
			return true;
		if (value == ROOT_PCIE_EISA_ID)
			return true;
		return false;
	}

	/* A motherboard resource device is PNP0C01 or PNP0C02. */
	if (value == SYSTEM_PNP0C01_EISA_ID)
		return true;
	if (value == SYSTEM_PNP0C02_EISA_ID)
		return true;

	/* Succeeded: the identifier is of another kind. */
	return false;
}

/* Tells whether a string identifier is of the kind. */
static bool
string_is(
	const char *text,
	enum device_kind kind)
{
	int compared;

	/* A host bridge is PNP0A03 or PNP0A08. */
	if (kind == DEVICE_PCI_ROOT) {
		compared = kern_strcmp(text, "PNP0A03");
		if (compared == 0)
			return true;
		compared = kern_strcmp(text, "PNP0A08");
		if (compared == 0)
			return true;
		return false;
	}

	/* A motherboard resource device is PNP0C01 or PNP0C02. */
	compared = kern_strcmp(text, "PNP0C01");
	if (compared == 0)
		return true;
	compared = kern_strcmp(text, "PNP0C02");
	if (compared == 0)
		return true;

	/* Succeeded: the identifier is of another kind. */
	return false;
}

/* Tells whether a device is present or functioning: its _STA says so, or it has none. */
static bool
device_in_use(
	struct drv_acpi_node *node)
{
	struct drv_acpi_node *found;
	uint64_t status;
	int error;

	/* A device without its own _STA is in use. */
	error = drv_acpi_lookup(node, "_STA", &found);
	if (error != 0 || found == NULL)
		return true;

	/* A _STA that cannot be read leaves the device in use, so its ranges stay reserved. */
	error = drv_acpi_evaluate_integer(found, NULL, &status);
	if (error != 0)
		return true;

	/* A device neither present nor functioning decodes nothing. */
	if ((status & (STATUS_PRESENT | STATUS_FUNCTIONING)) == 0U)
		return false;

	/* Succeeded: the device is in use. */
	return true;
}

/* Reads an integer object of the device's own, zero when it has none or it cannot be read. */
static uint64_t
own_integer(
	struct drv_acpi_node *node,
	const char *name)
{
	struct drv_acpi_node *found;
	uint64_t value;
	int error;

	/* A device without the object gives zero. */
	error = drv_acpi_lookup(node, name, &found);
	if (error != 0 || found == NULL)
		return 0;

	/* An object that cannot be read gives zero too. */
	value = 0;
	error = drv_acpi_evaluate_integer(found, NULL, &value);
	if (error != 0)
		return 0;

	/* Succeeded: the object's value. */
	return value;
}
