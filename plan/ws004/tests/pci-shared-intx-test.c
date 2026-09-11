/* Generic PCI shared-INTx dispatch and checked teardown fixture. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <drivers/pci.h>
#include <hal/hal.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_IRQ 11
#define TEST_DEVICE_FIRST 2U
#define TEST_DEVICE_COUNT 3U

static uint8_t config[TEST_DEVICE_COUNT][256];
static hal_irq_handler_t registered_handler;
static void *registered_argument;
static int injected_remove_error;
static bool interrupts_enabled = true;
static unsigned install_calls;
static unsigned remove_calls;
static unsigned mask_calls;
static unsigned unmask_calls;
static unsigned eoi_calls;
static hal_irq_ack_t last_acknowledge;

struct handler_fixture {
	struct drv_pci_device *device;
	void *cookie;
	unsigned calls;
	unsigned remove_self;
	int remove_result;
};

void *
hal_malloc(size_t size)
{
	return malloc(size);
}

void
hal_free(void *pointer)
{
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

bool
hal_irq_disable(void)
{
	bool was_enabled = interrupts_enabled;

	interrupts_enabled = false;
	return was_enabled;
}

void
hal_irq_enable(void)
{
	interrupts_enabled = true;
}

void
hal_irq_mask(int irq)
{
	assert(irq == TEST_IRQ);
	mask_calls++;
}

void
hal_irq_unmask(int irq)
{
	assert(irq == TEST_IRQ);
	unmask_calls++;
}

void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	eoi_calls++;
	last_acknowledge = acknowledge;
}

void
hal_io_mb(void)
{
}

void
hal_io_rmb(void)
{
}

int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	assert(irq == TEST_IRQ);
	if (handler != NULL) {
		install_calls++;
		if (registered_handler != NULL)
			return HAL_ERR_BUSY;
		registered_handler = handler;
		registered_argument = argument;
		return HAL_OK;
	}
	assert(argument == NULL);
	remove_calls++;
	if (injected_remove_error != HAL_OK) {
		int error = injected_remove_error;

		injected_remove_error = HAL_OK;
		return error;
	}
	assert(registered_handler != NULL);
	registered_handler = NULL;
	registered_argument = NULL;
	return HAL_OK;
}

int
hal_irq_register_msi(const char *source, hal_irq_handler_t handler,
	void *argument, int *irq, paddr_t *address, uint32_t *event)
{
	(void)source;
	(void)handler;
	(void)argument;
	(void)irq;
	(void)address;
	(void)event;
	return HAL_ERR_UNSUPPORTED;
}

int
hal_irq_unregister_msi(int irq)
{
	(void)irq;
	return HAL_ERR_UNSUPPORTED;
}

static int
config_read(void *context, const struct drv_pci_address *address,
	unsigned offset, unsigned width, uint32_t *result)
{
	unsigned index;

	(void)context;
	if (address->segment != 0 || address->bus != 0 ||
	    address->function != 0 || address->device < TEST_DEVICE_FIRST ||
	    address->device >= TEST_DEVICE_FIRST + TEST_DEVICE_COUNT) {
		*result = UINT32_MAX;
		return 0;
	}
	index = address->device - TEST_DEVICE_FIRST;
	assert(offset <= sizeof(config[index]));
	assert(width <= sizeof(config[index]) - offset);
	*result = 0;
	memcpy(result, config[index] + offset, width);
	return 0;
}

static int
config_write(void *context, const struct drv_pci_address *address,
	unsigned offset, unsigned width, uint32_t value)
{
	unsigned index;

	(void)context;
	if (address->segment != 0 || address->bus != 0 ||
	    address->function != 0 || address->device < TEST_DEVICE_FIRST ||
	    address->device >= TEST_DEVICE_FIRST + TEST_DEVICE_COUNT)
		return EINVAL;
	index = address->device - TEST_DEVICE_FIRST;
	assert(offset <= sizeof(config[index]));
	assert(width <= sizeof(config[index]) - offset);
	/* The fixture exposes no BARs. */
	if (offset >= 0x10U && offset < 0x28U && width == 4 &&
	    value == UINT32_MAX)
		value = 0;
	memcpy(config[index] + offset, &value, width);
	return 0;
}

static int
allocate_irqs(void *context, struct drv_pci_device *device,
	enum drv_pci_irq_type type, unsigned minimum, unsigned maximum,
	struct drv_pci_irq *irqs, unsigned *count)
{
	(void)context;
	(void)device;
	if (type != DRV_PCI_IRQ_INTX || minimum != 1U || maximum < 1U)
		return ENOTSUP;
	irqs[0].type = DRV_PCI_IRQ_INTX;
	irqs[0].index = 0;
	irqs[0].vector = TEST_IRQ;
	irqs[0].private_data[0] = 0;
	irqs[0].private_data[1] = 0;
	*count = 1;
	return 0;
}

static int
child_handler(void *argument)
{
	struct handler_fixture *fixture = argument;

	fixture->calls++;
	if (fixture->remove_self)
		fixture->remove_result =
		    drv_pci_device_disestablish_irq_checked(fixture->device,
		    fixture->cookie);
	return 1;
}

static void
deliver(hal_irq_ack_t acknowledge)
{
	bool was_enabled = interrupts_enabled;

	assert(registered_handler != NULL);
	interrupts_enabled = false;
	registered_handler(TEST_IRQ, acknowledge, registered_argument);
	interrupts_enabled = was_enabled;
}

static void
put32(unsigned device, unsigned offset, uint32_t value)
{
	assert(device >= TEST_DEVICE_FIRST);
	assert(device < TEST_DEVICE_FIRST + TEST_DEVICE_COUNT);
	memcpy(config[device - TEST_DEVICE_FIRST] + offset, &value,
	    sizeof(value));
}

int
main(void)
{
	static const struct drv_pci_bus_ops operations = {
		.config_read = config_read,
		.config_write = config_write,
		.allocate_irqs = allocate_irqs,
	};
	struct handler_fixture fixtures[TEST_DEVICE_COUNT];
	struct drv_pci_bus *bus;
	struct drv_pci_irq irq;
	unsigned count;
	unsigned index;

	memset(config, 0, sizeof(config));
	memset(fixtures, 0, sizeof(fixtures));
	for (index = 0; index < TEST_DEVICE_COUNT; index++) {
		put32(TEST_DEVICE_FIRST + index, 0x00U,
		    ((0x2000U + index) << 16) | 0x1234U);
		put32(TEST_DEVICE_FIRST + index, 0x08U, 0x0c030001U);
		config[index][0x3cU] = TEST_IRQ;
	}
	assert(drv_pci_init() == 0);
	assert(drv_pci_bus_create_root(0, 0, &operations, NULL, NULL, &bus) ==
	    0);
	assert(drv_pci_bus_scan(bus) == 0);
	for (index = 0; index < TEST_DEVICE_COUNT; index++) {
		struct drv_pci_address address = {
			0, 0, TEST_DEVICE_FIRST + index, 0
		};

		fixtures[index].device = drv_pci_find_device(&address);
		assert(fixtures[index].device != NULL);
		count = 0;
		assert(drv_pci_device_allocate_irqs(fixtures[index].device,
		    DRV_PCI_IRQ_ALLOW_INTX, 1, 1, &irq, &count) == 0);
		assert(count == 1U && irq.type == DRV_PCI_IRQ_INTX &&
		    irq.vector == TEST_IRQ);
		assert(drv_pci_device_establish_irq(fixtures[index].device,
		    &irq, child_handler, &fixtures[index], "shared-intx",
		    &fixtures[index].cookie) == 0);
	}
	assert(install_calls == 1U);
	assert(unmask_calls == 1U);

	deliver(0x101U);
	assert(fixtures[0].calls == 1U);
	assert(fixtures[1].calls == 1U);
	assert(fixtures[2].calls == 1U);
	assert(eoi_calls == 1U && last_acknowledge == 0x101U);

	/* A non-final owner leaves the physical line and both peers active. */
	assert(drv_pci_device_disestablish_irq_checked(fixtures[1].device,
	    fixtures[1].cookie) == 0);
	fixtures[1].cookie = NULL;
	assert(mask_calls == 0U && remove_calls == 0U);
	deliver(0x102U);
	assert(fixtures[0].calls == 2U);
	assert(fixtures[1].calls == 1U);
	assert(fixtures[2].calls == 2U);

	/* Removal from inside the shared dispatcher is checked and retains the
	 * complete cookie/list state for a later retry. */
	fixtures[0].remove_self = 1;
	deliver(0x103U);
	assert(fixtures[0].remove_result == EBUSY);
	assert(fixtures[0].calls == 3U);
	assert(fixtures[2].calls == 3U);
	fixtures[0].remove_self = 0;
	assert(drv_pci_device_disestablish_irq_checked(fixtures[0].device,
	    fixtures[0].cookie) == 0);
	fixtures[0].cookie = NULL;
	assert(mask_calls == 0U && remove_calls == 0U);
	deliver(0x104U);
	assert(fixtures[0].calls == 3U);
	assert(fixtures[2].calls == 4U);

	/* A failing final HAL drain restores the registered, unmasked line. */
	injected_remove_error = HAL_ERR_BUSY;
	assert(drv_pci_device_disestablish_irq_checked(fixtures[2].device,
	    fixtures[2].cookie) == EBUSY);
	assert(mask_calls == 1U && remove_calls == 1U);
	assert(unmask_calls == 2U && registered_handler != NULL);
	deliver(0x105U);
	assert(fixtures[2].calls == 5U);
	assert(drv_pci_device_disestablish_irq_checked(fixtures[2].device,
	    fixtures[2].cookie) == 0);
	fixtures[2].cookie = NULL;
	assert(mask_calls == 2U && remove_calls == 2U);
	assert(registered_handler == NULL);
	assert(eoi_calls == 5U && last_acknowledge == 0x105U);
	return 0;
}
