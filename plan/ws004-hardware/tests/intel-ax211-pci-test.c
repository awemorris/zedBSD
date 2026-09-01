/*
 * Intel AX211 exact PCI/CNVio2 detection fixture
 * SPDX-License-Identifier: Zlib
 */

#include <drivers/pci.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum fixture_failure {
	FIXTURE_FAIL_NONE,
	FIXTURE_FAIL_CLAIM,
	FIXTURE_FAIL_BAR,
	FIXTURE_FAIL_SAVE,
	FIXTURE_FAIL_MASTER,
	FIXTURE_FAIL_MAP,
	FIXTURE_FAIL_ENABLE,
	FIXTURE_FAIL_COMMAND_READ,
	FIXTURE_FAIL_BAR_RESTORE,
	FIXTURE_FAIL_RESTORE
};

struct drv_pci_device {
	uint16_t vendor;
	uint16_t product;
	uint16_t subvendor;
	uint16_t subproduct;
	uint32_t class_code;
	uint8_t revision;
	struct drv_pci_address address;
	struct drv_pci_bar bar;
	uint16_t command;
	uint16_t saved_command;
	uint32_t registers[0x4000U / sizeof(uint32_t)];
	enum drv_pci_bar_type mapping_type;
	size_t mapping_size;
	enum fixture_failure failure;
	unsigned claimed;
	unsigned mapped;
	unsigned map_flags;
	unsigned map_reassigns_bar;
	unsigned bar_reads;
	unsigned memory_enabled_without_master;
	unsigned command_read_without_master;
	char events[32];
	size_t event_count;
};

#include "../../../src/drivers/pci-intel-ax211.c"

static struct drv_pci_driver *registered_driver;
static unsigned read_barrier_count;

static void fixture_event(struct drv_pci_device *device, char event);
static void fixture_reset(struct drv_pci_device *device);
static void test_registration_and_exact_match(void);
static void test_detection_and_truthful_nonpublication(void);
static void test_hardware_identity_rejection(void);
static void test_bar_and_mapping_rejection(void);
static void test_failure_unwind(void);
static void test_restore_failure_quiesces_master(void);

static void
fixture_event(
	struct drv_pci_device *device,
	char event)
{
	assert(device->event_count + 1U < sizeof(device->events));
	device->events[device->event_count++] = event;
	device->events[device->event_count] = '\0';
}

static void
fixture_reset(
	struct drv_pci_device *device)
{
	memset(device, 0, sizeof(*device));
	device->vendor = 0x8086U;
	device->product = 0x51f0U;
	device->subvendor = 0x8086U;
	device->subproduct = 0x4090U;
	device->class_code = 0x028000U;
	device->revision = 0x01U;
	device->address.segment = 0U;
	device->address.bus = 0U;
	device->address.device = 20U;
	device->address.function = 3U;
	device->bar.index = 0U;
	device->bar.type = DRV_PCI_BAR_MEMORY64;
	device->bar.bus_address = 0xf0810000U;
	device->bar.size = 0x4000U;
	device->command = 0x0005U;
	device->mapping_type = DRV_PCI_BAR_MEMORY64;
	device->mapping_size = 0x4000U;
	device->registers[0x028U / sizeof(uint32_t)] = 0x00000370U;
	device->registers[0x09cU / sizeof(uint32_t)] = 0x2010d000U;
	read_barrier_count = 0U;
}

uint16_t
drv_pci_device_vendor(
	const struct drv_pci_device *device)
{
	return device->vendor;
}

uint16_t
drv_pci_device_product(
	const struct drv_pci_device *device)
{
	return device->product;
}

uint16_t
drv_pci_device_subvendor(
	const struct drv_pci_device *device)
{
	return device->subvendor;
}

uint16_t
drv_pci_device_subproduct(
	const struct drv_pci_device *device)
{
	return device->subproduct;
}

uint32_t
drv_pci_device_class(
	const struct drv_pci_device *device)
{
	return device->class_code;
}

uint8_t
drv_pci_device_revision(
	const struct drv_pci_device *device)
{
	return device->revision;
}

void
drv_pci_device_address(
	const struct drv_pci_device *device,
	struct drv_pci_address *address)
{
	*address = device->address;
}

int
drv_pci_device_claim_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	assert(index == 0U);
	fixture_event(device, 'C');
	if (device->failure == FIXTURE_FAIL_CLAIM)
		return EBUSY;
	assert(device->claimed == 0U);
	device->claimed = 1U;
	return 0;
}

void
drv_pci_device_release_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	assert(index == 0U);
	assert(device->claimed != 0U);
	fixture_event(device, 'L');
	device->claimed = 0U;
}

int
drv_pci_device_bar(
	const struct drv_pci_device *constant_device,
	unsigned index,
	struct drv_pci_bar *bar)
{
	struct drv_pci_device *device;

	device = (struct drv_pci_device *)constant_device;
	assert(index == 0U);
	assert(device->claimed != 0U);
	fixture_event(device, device->bar_reads == 0U ? 'B' : 'b');
	device->bar_reads++;
	if (device->failure == FIXTURE_FAIL_BAR)
		return EIO;
	*bar = device->bar;
	return 0;
}

int
drv_pci_device_save_enable_state(
	struct drv_pci_device *device,
	struct drv_pci_enable_state *state)
{
	fixture_event(device, 'S');
	if (device->failure == FIXTURE_FAIL_SAVE)
		return EIO;
	device->saved_command = device->command;
	state->private_data[0] = device->command;
	state->private_data[1] = 1U;
	return 0;
}

int
drv_pci_device_restore_enable_state(
	struct drv_pci_device *device,
	struct drv_pci_enable_state *state)
{
	fixture_event(device, 'T');
	if (device->failure == FIXTURE_FAIL_RESTORE)
		return EIO;
	assert(state->private_data[1] != 0U);
	device->command = (uint16_t)state->private_data[0];
	state->private_data[0] = 0U;
	state->private_data[1] = 0U;
	return 0;
}

int
drv_pci_device_set_bus_master(
	struct drv_pci_device *device,
	bool enabled)
{
	assert(!enabled);
	fixture_event(device, 'Q');
	device->command &= (uint16_t)~0x0004U;
	if (device->failure == FIXTURE_FAIL_MASTER)
		return EIO;
	return 0;
}

int
drv_pci_device_map_bar(
	struct drv_pci_device *device,
	unsigned index,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	assert(index == 0U);
	assert(device->claimed != 0U);
	assert((device->command & 0x0004U) == 0U);
	fixture_event(device, 'M');
	device->map_flags = flags;
	if (device->map_reassigns_bar)
		device->bar.bus_address = 0xf0820000U;
	if (device->failure == FIXTURE_FAIL_MAP)
		return EIO;
	mapping->address = device->registers;
	mapping->size = device->mapping_size;
	mapping->type = device->mapping_type;
	device->mapped = 1U;
	return 0;
}

int
drv_pci_device_assign_bar(
	struct drv_pci_device *device,
	unsigned index,
	uint64_t address)
{
	assert(index == 0U);
	assert(device->claimed != 0U);
	assert((device->command & 0x0004U) == 0U);
	fixture_event(device, 'A');
	if (device->failure == FIXTURE_FAIL_BAR_RESTORE)
		return EIO;
	device->bar.bus_address = address;
	return 0;
}

void
drv_pci_device_unmap_bar(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping)
{
	assert(device->mapped != 0U);
	assert(mapping->address == device->registers);
	fixture_event(device, 'U');
	device->mapped = 0U;
	mapping->address = NULL;
}

int
drv_pci_device_enable_memory(
	struct drv_pci_device *device)
{
	fixture_event(device, 'E');
	if ((device->command & 0x0004U) == 0U)
		device->memory_enabled_without_master++;
	if (device->failure == FIXTURE_FAIL_ENABLE)
		return EIO;
	device->command |= 0x0002U;
	return 0;
}

int
drv_pci_device_config_read16(
	struct drv_pci_device *device,
	unsigned offset,
	uint16_t *value)
{
	assert(offset == 0x04U);
	fixture_event(device, 'R');
	if ((device->command & 0x0004U) == 0U)
		device->command_read_without_master++;
	if (device->failure == FIXTURE_FAIL_COMMAND_READ)
		return EIO;
	*value = device->command;
	return 0;
}

int
drv_pci_driver_register(
	struct drv_pci_driver *driver)
{
	registered_driver = driver;
	return 0;
}

void
hal_io_rmb(void)
{
	read_barrier_count++;
}

int
hal_printf(
	const char *format,
	...)
{
	(void)format;
	return 0;
}

static void
test_registration_and_exact_match(void)
{
	struct drv_pci_device device;

	registered_driver = NULL;
	assert(drv_pci_intel_ax211_driver_register() == 0);
	assert(registered_driver == &ax211_pci_driver);
	assert(registered_driver->id_count == 1U);
	assert(registered_driver->ids[0].vendor == 0x8086U);
	assert(registered_driver->ids[0].device == 0x51f0U);
	assert(registered_driver->ids[0].subvendor == 0x8086U);
	assert(registered_driver->ids[0].subdevice == 0x4090U);
	assert(registered_driver->ids[0].class_code == 0x028000U);
	assert(registered_driver->ids[0].class_mask == 0xffffffU);

	fixture_reset(&device);
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_EXACT);
	device.vendor++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.product++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.subvendor++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.subproduct++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.revision++;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
	fixture_reset(&device);
	device.class_code = 0x028001U;
	assert(ax211_pci_match(&device, &ax211_pci_ids[0]) ==
	    DRV_PCI_MATCH_NONE);
}

static void
test_detection_and_truthful_nonpublication(void)
{
	struct drv_pci_device device;
	int error;

	fixture_reset(&device);
	error = ax211_pci_attach(&device, &ax211_pci_ids[0]);
	assert(error == ENOTSUP);
	assert(strcmp(device.events, "CBSQMERUbTL") == 0);
	assert(device.command == device.saved_command);
	assert(device.command == 0x0005U);
	assert(device.claimed == 0U);
	assert(device.mapped == 0U);
	assert(device.map_flags ==
	    (DRV_PCI_MAP_READ | DRV_PCI_MAP_NOCACHE));
	assert((device.map_flags & DRV_PCI_MAP_WRITE) == 0U);
	assert(device.memory_enabled_without_master == 1U);
	assert(device.command_read_without_master == 1U);
	assert(read_barrier_count == 2U);

	fixture_reset(&device);
	device.map_reassigns_bar = 1U;
	error = ax211_pci_attach(&device, &ax211_pci_ids[0]);
	assert(error == ENOTSUP);
	assert(strcmp(device.events, "CBSQMERUbATL") == 0);
	assert(device.bar.bus_address == 0xf0810000U);
	assert(device.command == 0x0005U);
}

static void
test_hardware_identity_rejection(void)
{
	struct drv_pci_device device;

	fixture_reset(&device);
	device.registers[0x028U / sizeof(uint32_t)] = 0x00000440U;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENODEV);
	assert(strcmp(device.events, "CBSQMERUbTL") == 0);
	assert(device.command == 0x0005U);

	fixture_reset(&device);
	device.registers[0x028U / sizeof(uint32_t)] = 0x00000430U;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENOTSUP);
	assert(strcmp(device.events, "CBSQMERUbTL") == 0);
	assert(device.command == 0x0005U);

	fixture_reset(&device);
	device.registers[0x09cU / sizeof(uint32_t)] = 0x20110000U;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENODEV);
	assert(strcmp(device.events, "CBSQMERUbTL") == 0);
	assert(device.command == 0x0005U);

	fixture_reset(&device);
	device.registers[0x09cU / sizeof(uint32_t)] = 0x3010d000U;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENODEV);
	assert(strcmp(device.events, "CBSQMERUbTL") == 0);
	assert(device.command == 0x0005U);
}

static void
test_bar_and_mapping_rejection(void)
{
	struct drv_pci_device device;

	fixture_reset(&device);
	device.bar.type = DRV_PCI_BAR_IO;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENODEV);
	assert(strcmp(device.events, "CBL") == 0);

	fixture_reset(&device);
	device.bar.size = 0x3fffU;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == ENODEV);
	assert(strcmp(device.events, "CBL") == 0);

	fixture_reset(&device);
	device.mapping_type = DRV_PCI_BAR_IO;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == EIO);
	assert(strcmp(device.events, "CBSQMUbTL") == 0);
	assert(read_barrier_count == 0U);

	fixture_reset(&device);
	device.mapping_size = 0x3fffU;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == EIO);
	assert(strcmp(device.events, "CBSQMUbTL") == 0);
	assert(read_barrier_count == 0U);
}

static void
test_failure_unwind(void)
{
	static const struct {
		enum fixture_failure failure;
		const char *events;
	} cases[] = {
		{ FIXTURE_FAIL_CLAIM, "C" },
		{ FIXTURE_FAIL_BAR, "CBL" },
		{ FIXTURE_FAIL_SAVE, "CBSL" },
		{ FIXTURE_FAIL_MASTER, "CBSQTL" },
		{ FIXTURE_FAIL_MAP, "CBSQMbTL" },
		{ FIXTURE_FAIL_ENABLE, "CBSQMEUbTL" },
		{ FIXTURE_FAIL_COMMAND_READ, "CBSQMERUbTL" },
	};
	struct drv_pci_device device;
	size_t index;

	for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
		fixture_reset(&device);
		device.failure = cases[index].failure;
		assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) != 0);
		assert(strcmp(device.events, cases[index].events) == 0);
		assert(device.claimed == 0U);
		assert(device.mapped == 0U);
		if (cases[index].failure >= FIXTURE_FAIL_MASTER)
			assert(device.command == device.saved_command);
		if (cases[index].failure <= FIXTURE_FAIL_COMMAND_READ)
			assert(read_barrier_count == 0U);
	}

	fixture_reset(&device);
	device.map_reassigns_bar = 1U;
	device.failure = FIXTURE_FAIL_MAP;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == EIO);
	assert(strcmp(device.events, "CBSQMbATL") == 0);
	assert(device.bar.bus_address == 0xf0810000U);
	assert(device.command == 0x0005U);
}

static void
test_restore_failure_quiesces_master(void)
{
	struct drv_pci_device device;

	fixture_reset(&device);
	device.failure = FIXTURE_FAIL_RESTORE;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == EIO);
	assert(strcmp(device.events, "CBSQMERUbTQL") == 0);
	assert((device.command & 0x0004U) == 0U);
	assert(device.claimed == 0U);
	assert(device.mapped == 0U);

	fixture_reset(&device);
	device.map_reassigns_bar = 1U;
	device.failure = FIXTURE_FAIL_BAR_RESTORE;
	assert(ax211_pci_attach(&device, &ax211_pci_ids[0]) == EIO);
	assert(strcmp(device.events, "CBSQMERUbAQL") == 0);
	assert((device.command & 0x0004U) == 0U);
	assert(device.bar.bus_address == 0xf0820000U);
	assert(device.claimed == 0U);
	assert(device.mapped == 0U);
}

int
main(void)
{
	test_registration_and_exact_match();
	test_detection_and_truthful_nonpublication();
	test_hardware_identity_rejection();
	test_bar_and_mapping_rejection();
	test_failure_unwind();
	test_restore_failure_quiesces_master();
	puts("intel ax211 pci detection fixture: PASS");
	return 0;
}
