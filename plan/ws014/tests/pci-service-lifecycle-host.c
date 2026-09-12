/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the production PCI service lifecycle without GPU hardware.
 */

#include <drivers/pci.h>
#include <kern/kmem.h>
#include <kern/klog.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * One simulated hardware instance, retained until its detach succeeds.
 * The test changes failure reports only between serialized PCI operations.
 */
struct service_fixture {
	int attach_error;
	int publish_error;
	int unpublish_error;
	int detach_error;
	unsigned attaches;
	unsigned publishes;
	unsigned unpublishes;
	unsigned detaches;
	unsigned live;
	unsigned published;
};

/* The sole fixture stays alive across failed publication and teardown retries. */
static struct service_fixture fixture;

/* PCI borrows this driver while the test's device is attached or quarantined. */
static struct drv_pci_driver driver;

/* This contract is initialized once, before any PCI operation can borrow it. */
static struct drv_pci_service_interface service;

static int config_read(void *argument, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *result);
static int config_write(void *argument, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t word);
static int fixture_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int fixture_detach(struct drv_pci_device *device, unsigned flags);
static int fixture_publish(struct drv_pci_device *device, void *argument);
static int fixture_unpublish(struct drv_pci_device *device, void *argument);

/*
 * Supplies host storage to the unmodified production PCI allocation path.
 */
void *
kern_malloc(
	size_t size)
{
	void *storage;

	/* Obtains the memory requested by the production core. */
	storage = malloc(size);

	/* Reports host exhaustion without changing the kernel contract. */
	if (storage == NULL)
		return NULL;

	/* Succeeded: the PCI core owns this host allocation. */
	return storage;
}

/*
 * Releases the storage returned by the host allocator.
 */
void
kern_free(
	void *storage)
{
	/* Gives the completed PCI allocation back to the host. */
	free(storage);

	/* Succeeded: no host allocation remains at this address. */
	return;
}

/*
 * Accepts expected failure diagnostics without changing their control flow.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* The fixture checks return codes and ownership instead of log wording. */
	(void)format;

	/* Succeeded: the production diagnostic has no host-side dependency. */
	return;
}

/*
 * Verifies service ordering, failed publication, and retained detach ownership.
 */
int
main(
	void)
{
	static const struct drv_pci_id ids[] = {
		{0x1234U, 0x5678U, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0, 0, 0}
	};
	struct drv_pci_bus_ops operations;
	struct drv_pci_bus *bus;
	struct drv_pci_device *device;
	struct drv_pci_driver *owner;
	struct drv_pci_service_interface invalid_service;
	void *private_data;
	unsigned detach_count;
	unsigned unpublish_count;
	int error;

	/* Freezes the fixture's publication contract before PCI can borrow it. */
	service.publish = fixture_publish;
	service.unpublish = fixture_unpublish;

	/* Presents one fake PCI function through the normal enumeration API. */
	memset(&operations, 0, sizeof(operations));
	operations.config_read = config_read;
	operations.config_write = config_write;
	error = drv_pci_init();
	assert(error == 0);
	error = drv_pci_bus_create_root(0, 0, &operations, NULL, NULL, &bus);
	assert(error == 0);
	error = drv_pci_bus_scan(bus);
	assert(error == 0);
	device = drv_pci_find_id(0x1234U, 0x5678U, NULL);
	assert(device != NULL);

	/* Rejects service contracts outside the owning attach transaction. */
	error = drv_pci_device_set_service(device, &service, &fixture);
	assert(error == EBUSY);
	invalid_service = service;
	invalid_service.unpublish = NULL;
	error = drv_pci_device_set_service(device, &invalid_service, &fixture);
	assert(error == EINVAL);

	/* Registers the normal driver and observes attach before publication. */
	memset(&driver, 0, sizeof(driver));
	driver.name = "pci-service-fixture";
	driver.ids = ids;
	driver.id_count = 1;
	driver.attach = fixture_attach;
	driver.detach = fixture_detach;
	error = drv_pci_driver_register(&driver);
	assert(error == 0);
	assert(fixture.attaches == 1);
	assert(fixture.publishes == 1);
	assert(fixture.live == 1);
	assert(fixture.published == 1);

	/* Retains hardware while the subsystem still has an open reference. */
	fixture.unpublish_error = EBUSY;
	detach_count = fixture.detaches;
	error = drv_pci_device_detach(device, DRV_PCI_DETACH_FORCE);
	assert(error == EBUSY);
	assert(fixture.detaches == detach_count);
	assert(fixture.live == 1);
	owner = drv_pci_device_driver(device);
	assert(owner == &driver);

	/* Retries the same service before releasing hardware after references drain. */
	fixture.unpublish_error = 0;
	error = drv_pci_device_detach(device, 0);
	assert(error == 0);
	assert(fixture.published == 0);
	assert(fixture.live == 0);
	owner = drv_pci_device_driver(device);
	assert(owner == NULL);
	private_data = drv_pci_device_driver_data(device);
	assert(private_data == NULL);

	/* Rolls hardware back when publication fails before becoming visible. */
	fixture.publish_error = ENOMEM;
	unpublish_count = fixture.unpublishes;
	error = drv_pci_device_probe(device);
	assert(error == ENOMEM);
	assert(fixture.live == 0);
	assert(fixture.unpublishes == unpublish_count);
	owner = drv_pci_device_driver(device);
	assert(owner == NULL);

	/* Retains a failed publication's hardware when rollback also fails. */
	fixture.detach_error = EIO;
	error = drv_pci_device_probe(device);
	assert(error == ENOMEM);
	assert(fixture.live == 1);
	assert(fixture.published == 0);
	owner = drv_pci_device_driver(device);
	assert(owner == &driver);
	private_data = drv_pci_device_driver_data(device);
	assert(private_data == &fixture);
	error = drv_pci_device_probe(device);
	assert(error == EBUSY);

	/* Cleans the retained hardware without unpublishing a nonexistent service. */
	fixture.detach_error = 0;
	unpublish_count = fixture.unpublishes;
	error = drv_pci_device_detach(device, 0);
	assert(error == 0);
	assert(fixture.live == 0);
	assert(fixture.unpublishes == unpublish_count);

	/* Does not publish a descriptor staged by an unsuccessful attach. */
	fixture.publish_error = 0;
	fixture.attach_error = EIO;
	unpublish_count = fixture.publishes;
	error = drv_pci_device_probe(device);
	assert(error == EIO);
	assert(fixture.live == 0);
	assert(fixture.publishes == unpublish_count);
	owner = drv_pci_device_driver(device);
	assert(owner == NULL);

	/* Stages a fresh descriptor after attach failure and makes it visible. */
	fixture.attach_error = 0;
	error = drv_pci_device_probe(device);
	assert(error == 0);
	assert(fixture.published == 1);

	/* Keeps hardware cleanup retryable after successful service removal. */
	fixture.detach_error = EIO;
	error = drv_pci_device_detach(device, 0);
	assert(error == EIO);
	assert(fixture.published == 0);
	assert(fixture.live == 1);
	unpublish_count = fixture.unpublishes;

	/* Avoids calling a released service again during hardware cleanup retry. */
	fixture.detach_error = 0;
	error = drv_pci_device_detach(device, 0);
	assert(error == 0);
	assert(fixture.live == 0);
	assert(fixture.unpublishes == unpublish_count);

	/* Removes the test driver only after all of its instance ownership ends. */
	error = drv_pci_driver_unregister(&driver);
	assert(error == 0);

	/* Succeeded: every tested publication and teardown boundary held. */
	puts("PCI service lifecycle: PASS");
	return 0;
}

/* Returns one configuration-space function for production enumeration. */
static int
config_read(
	void *argument,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *result)
{
	/* Configuration storage is fixed and has no hardware width side effects. */
	(void)argument;
	(void)width;

	/* Leaves every unused device slot absent from the simulated bus. */
	if (address->device != 1 || address->function != 0) {
		*result = 0xffffffffU;

		/* Succeeded: this slot contains no enumerated function. */
		return 0;
	}

	/* Supplies identity and a display class while leaving all BARs empty. */
	*result = 0;
	if (offset == 0) {
		*result = 0x56781234U;
	} else if (offset == 8) {
		*result = 0x03000001U;
	}

	/* Succeeded: the core receives the requested configuration word. */
	return 0;
}

/* Accepts configuration writes without inventing a hardware resource. */
static int
config_write(
	void *argument,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t word)
{
	/* The simulated device has no writable command or BAR state. */
	(void)argument;
	(void)address;
	(void)offset;
	(void)width;
	(void)word;

	/* Succeeded: enumeration may continue after its sizing transaction. */
	return 0;
}

/* Stages one service after establishing the fake hardware instance. */
static int
fixture_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	int error;

	/* The fixed device identity needs no per-match hardware variation. */
	(void)id;

	/* Records the new hardware owner before staging its service. */
	fixture.attaches++;
	fixture.live = 1;
	error = drv_pci_device_set_driver_data(device, &fixture);
	assert(error == 0);
	error = drv_pci_device_set_service(device, &service, &fixture);
	assert(error == 0);

	/* Rejects descriptor replacement while the first borrow is still live. */
	error = drv_pci_device_set_service(device, &service, &fixture);
	assert(error == EBUSY);

	/* An unsuccessful attach undoes its hardware work before returning. */
	if (fixture.attach_error != 0) {
		fixture.live = 0;
		error = drv_pci_device_set_driver_data(device, NULL);
		assert(error == 0);

		/* Reports the intended hardware failure without publishing a service. */
		return fixture.attach_error;
	}

	/* Succeeded: PCI may now publish the initialized hardware instance. */
	return 0;
}

/* Gives the simulated hardware back only after service references end. */
static int
fixture_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	int error;

	/* The fixture has no optional force policy for releasing live references. */
	(void)flags;

	/* Verifies that PCI removed visibility before hardware destruction. */
	assert(fixture.published == 0);
	assert(fixture.live == 1);
	fixture.detaches++;

	/* Rejects reentrant teardown while the outer destructor owns its state. */
	error = drv_pci_device_detach(device, 0);
	assert(error == EBUSY);

	/* Keeps hardware state intact when its checked teardown fails. */
	if (fixture.detach_error != 0)
		return fixture.detach_error;

	/* Releases the hardware instance after every prior boundary succeeded. */
	fixture.live = 0;

	/* Succeeded: PCI may discard its borrowed private data. */
	return 0;
}

/* Publishes an attached service or leaves no publication on failure. */
static int
fixture_publish(
	struct drv_pci_device *device,
	void *argument)
{
	struct drv_pci_driver *owner;
	int error;

	/* Verifies the instance and driver remain owned during publication. */
	assert(argument == &fixture);
	assert(fixture.live == 1);
	assert(fixture.published == 0);
	fixture.publishes++;
	owner = drv_pci_device_driver(device);
	assert(owner == &driver);

	/* Rejects reentrant probe and teardown during the publication boundary. */
	error = drv_pci_device_probe(device);
	assert(error == EBUSY);
	error = drv_pci_device_detach(device, 0);
	assert(error == EBUSY);

	/* Failure leaves no visibility that would require a later unpublish. */
	if (fixture.publish_error != 0)
		return fixture.publish_error;

	/* Makes the fake service visible after all initialization checks pass. */
	fixture.published = 1;

	/* Succeeded: unpublish now owns the reference released before hardware. */
	return 0;
}

/* Retains the service until its simulated references permit removal. */
static int
fixture_unpublish(
	struct drv_pci_device *device,
	void *argument)
{
	int error;

	/* Verifies hardware outlives every service teardown attempt. */
	assert(argument == &fixture);
	assert(fixture.live == 1);
	assert(fixture.published == 1);
	fixture.unpublishes++;

	/* Rejects reentrant destruction from inside the service callback. */
	error = drv_pci_device_detach(device, 0);
	assert(error == EBUSY);

	/* Keeps the service reference intact while removal remains busy. */
	if (fixture.unpublish_error != 0)
		return fixture.unpublish_error;

	/* Removes publication before PCI is allowed to destroy the instance. */
	fixture.published = 0;

	/* Succeeded: no callback may continue using the hardware afterward. */
	return 0;
}
