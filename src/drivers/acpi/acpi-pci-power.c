/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ACPI side of a PCI function's power (ws052-p004): the platform
 * power operations of pci-power.c.
 *
 * A PCI function's device in the namespace is the device below a PCI host
 * bridge whose _ADR, _BBN, _SEG and the bridges between them name the
 * function (drv_acpi_pci_location()).  Its D-state goes through
 * drv_acpi_device_power_set() (_PSx and the power resources) and its wake
 * through drv_acpi_device_wake_enable() (_PRW, _DSW).  A function without
 * a device in the namespace is reported with ENOENT, which pci-power.c
 * takes as nothing for the platform to do.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <drivers/acpi/acpi.h>
#include <drivers/pci/pci.h>
#include "aml-internal.h"
#include "aml-os.h"

/* The most functions whose device the cache remembers. */
#define NODE_CACHE_MAX		64U

/*
 * One function whose device was looked for: its address, and its device,
 * or NULL when the namespace has none.
 */
struct node_cache_entry {
	struct drv_pci_address address;
	struct drv_acpi_node *node;
};

/*
 * What the walk looks for, and the device it found.
 */
struct node_search {
	struct drv_pci_address address;
	struct drv_acpi_node *found;
};

/*
 * The functions looked for so far, the first cache_count entries used.
 * The namespace does not change once loaded, so an answer stays right;
 * a full cache only means later functions are looked for each time.
 * Read and written with the interpreter held.
 */
static struct node_cache_entry node_cache[NODE_CACHE_MAX];
static unsigned node_cache_count;

static int platform_set_state(void *argument, struct drv_pci_device *device, unsigned state);
static int platform_wake(void *argument, struct drv_pci_device *device, bool wake);

/* The platform power operations pci-power.c calls. */
static const struct drv_pci_platform_power acpi_platform_power = {
	platform_set_state,
	platform_wake,
	NULL
};

static struct drv_acpi_node *node_of(struct drv_pci_device *device);
static int find_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static bool same_address(const struct drv_pci_address *left, const struct drv_pci_address *right);

/*
 * Makes the namespace the platform side of the PCI functions' power.
 */
int
drv_acpi_pci_power_attach(void)
{
	/* Hands the operations to the PCI power code. */
	drv_pci_platform_power_set(&acpi_platform_power);

	/* Succeeded: suspended functions follow their devices' _PSx and wake. */
	return 0;
}

/* Puts a function's device in the D-state the PCI power code asks for. */
static int
platform_set_state(
	void *argument,
	struct drv_pci_device *device,
	unsigned state)
{
	struct drv_acpi_node *node;
	enum drv_acpi_device_state acpi_state;
	int error;

	UNUSED_PARAMETER(argument);

	/* A function without a device in the namespace has nothing to do. */
	node = node_of(device);
	if (node == NULL)
		return ENOENT;

	/* D3hot keeps the power resources of _PR3; D0 takes those of _PR0. */
	acpi_state = DRV_ACPI_D0;
	if (state == DRV_PCI_D3_HOT)
		acpi_state = DRV_ACPI_D3_HOT;

	/* Changes the device's state. */
	error = drv_acpi_device_power_set(node, acpi_state);
	if (error != 0)
		return error;

	/* Succeeded: the device follows the function. */
	return 0;
}

/* Arms or disarms a function's wake through its device. */
static int
platform_wake(
	void *argument,
	struct drv_pci_device *device,
	bool wake)
{
	struct drv_acpi_node *node;
	int error;

	UNUSED_PARAMETER(argument);

	/* A function without a device in the namespace has nothing to do. */
	node = node_of(device);
	if (node == NULL)
		return ENOENT;

	/* Disarms the wake; a wake that was never armed is nothing to undo. */
	if (!wake) {
		error = drv_acpi_device_wake_disable(node);
		if (error == EINVAL)
			return 0;

		/* Reports a disarm that failed. */
		if (error != 0)
			return error;

		/* Succeeded: the device no longer wakes the system. */
		return 0;
	}

	/* Arms it for the D3hot the function will be in; a device without _PRW reports ENOENT. */
	error = drv_acpi_device_wake_enable(node, DRV_ACPI_D3_HOT);
	if (error != 0)
		return error;

	/* Succeeded: the device wakes the system. */
	return 0;
}

/* Finds a function's device in the namespace, or NULL; the answer is cached. */
static struct drv_acpi_node *
node_of(
	struct drv_pci_device *device)
{
	struct drv_acpi_thread storage;
	struct drv_acpi_thread *thread;
	struct node_search search;
	unsigned index;
	bool same;

	/* Looks with the interpreter held, so that the cache and the namespace stay as they are. */
	thread = drv_acpi_enter(&storage);

	/* Takes a cached answer. */
	kern_memset(&search, 0, sizeof(search));
	drv_pci_device_address(device, &search.address);
	for (index = 0; index < node_cache_count; index++) {
		/* The function's own entry. */
		same = same_address(&node_cache[index].address, &search.address);
		if (same) {
			search.found = node_cache[index].node;
			drv_acpi_leave(thread);
			return search.found;
		}
	}

	/* Walks the namespace for the device. */
	(void)drv_acpi_walk(NULL, find_visitor, &search);

	/* Remembers the answer, the absence of a device too, while there is room. */
	if (node_cache_count < NODE_CACHE_MAX) {
		node_cache[node_cache_count].address = search.address;
		node_cache[node_cache_count].node = search.found;
		node_cache_count++;
	}

	/* Lets the interpreter go. */
	drv_acpi_leave(thread);

	/* Succeeded: reports the device, or NULL. */
	return search.found;
}

/* Stops the walk at the device that stands for the function looked for. */
static int
find_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct node_search *search;
	struct drv_acpi_node *address_node;
	struct drv_pci_device *function;
	struct drv_pci_address address;
	enum drv_acpi_type type;
	bool below;
	bool same;
	bool bridge;
	int error;

	UNUSED_PARAMETER(depth);

	/* Only a device stands for a function. */
	type = drv_acpi_node_type(node);
	if (type != DRV_ACPI_TYPE_DEVICE)
		return 0;

	/* Only a device with its own _ADR does. */
	error = drv_acpi_lookup(node, "_ADR", &address_node);
	if (error != 0)
		return 0;

	/* Only one below a PCI host bridge does. */
	below = drv_acpi_pci_below_root(node);
	if (!below)
		return 0;

	/* Finds the function the device stands for. */
	kern_memset(&address, 0, sizeof(address));
	error = drv_acpi_pci_location(node, &address.segment, &address.bus, &address.device, &address.function);
	if (error != 0)
		return 0;

	/* Keeps the device that stands for the function and ends the walk. */
	search = argument;
	same = same_address(&address, &search->address);
	if (same) {
		search->found = node;
		return -1;
	}

	/*
	 * Goes below another function only when it is a PCI-to-PCI bridge:
	 * the devices below any other function (a USB controller's hubs and
	 * ports, a GPU's outputs) are not PCI functions, though their _ADR
	 * would name one.
	 */
	function = drv_pci_find_device(&address);
	if (function == NULL)
		return 1;

	/* Skips below a function that is no bridge. */
	bridge = drv_pci_device_is_bridge(function);
	if (!bridge)
		return 1;

	/* Goes on below the bridge. */
	return 0;
}

/* Tells whether two PCI addresses name the same function. */
static bool
same_address(
	const struct drv_pci_address *left,
	const struct drv_pci_address *right)
{
	/* The segment and the bus. */
	if (left->segment != right->segment || left->bus != right->bus)
		return false;

	/* The device and the function. */
	if (left->device != right->device || left->function != right->function)
		return false;

	/* The same function. */
	return true;
}
