#include <drivers/pci.h>
#include <hal/hal.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint8_t config[4096];
static uint32_t msix_table[4];
static int registered_irq = -1;
static int unregistered_irq = -1;
static char registered_source[17];

void *hal_malloc(size_t size) { return malloc(size); }
void hal_free(void *pointer) { free(pointer); }
int hal_printf(const char *format, ...) { (void)format; return 0; }
void hal_irq_mask(int irq) { (void)irq; }
void hal_irq_unmask(int irq) { (void)irq; }
void hal_irq_send_eoi(hal_irq_ack_t acknowledge) { (void)acknowledge; }
void hal_io_mb(void) { }
void hal_io_rmb(void) { }
int hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{ (void)irq; (void)handler; (void)argument; return HAL_OK; }

int
hal_irq_register_msi(const char *source, hal_irq_handler_t handler,
	void *argument, int *irq, paddr_t *address, uint32_t *event)
{
	(void)handler;
	(void)argument;
	memcpy(registered_source, source, 17);
	registered_irq = 16;
	*irq = registered_irq;
	*address = (paddr_t)0xfee00000U;
	*event = 0xd0U;
	return HAL_OK;
}

int
hal_irq_unregister_msi(int irq)
{
	unregistered_irq = irq;
	return HAL_OK;
}

static int
config_read(void *context, const struct drv_pci_address *address,
	unsigned offset, unsigned width, uint32_t *result)
{
	(void)context;
	if (address->device != 2 || offset + width > sizeof(config)) {
		*result = 0xffffffffU;
		return 0;
	}
	*result = 0;
	memcpy(result, config + offset, width);
	return 0;
}

static int
config_write(void *context, const struct drv_pci_address *address,
	unsigned offset, unsigned width, uint32_t value)
{
	(void)context;
	if (address->device != 2 || offset + width > sizeof(config))
		return EINVAL;
	if (offset >= 0x10U && offset < 0x28U && width == 4 &&
	    value == 0xffffffffU) {
		uint32_t mask = offset == 0x10U ? 0xfffff000U : 0;
		memcpy(config + offset, &mask, sizeof(mask));
	} else {
		memcpy(config + offset, &value, width);
	}
	return 0;
}

static int
map_bar(void *context, struct drv_pci_device *device,
	const struct drv_pci_bar *bar, unsigned flags,
	struct drv_pci_mapping *mapping)
{
	(void)context; (void)device; (void)flags;
	assert(bar->size == 16U);
	mapping->address = msix_table;
	mapping->size = sizeof(msix_table);
	return 0;
}

static void
unmap_bar(void *context, struct drv_pci_mapping *mapping)
{
	(void)context;
	mapping->address = NULL;
}

static int
allocate_irqs(void *context, struct drv_pci_device *device,
	enum drv_pci_irq_type type, unsigned minimum, unsigned maximum,
	struct drv_pci_irq *irqs, unsigned *count)
{
	unsigned capability;
	uint8_t id;
	(void)context; (void)minimum; (void)maximum;
	id = type == DRV_PCI_IRQ_MSI ? 0x05U :
	    type == DRV_PCI_IRQ_MSIX ? 0x11U : 0;
	if (id == 0 || drv_pci_device_find_capability(device, id,
	    &capability) != 0)
		return ENOTSUP;
	irqs[0].type = type;
	irqs[0].index = 0;
	irqs[0].vector = 0;
	irqs[0].private_data[0] = capability;
	irqs[0].private_data[1] = 0;
	*count = 1;
	return 0;
}

static int handler(void *argument) { (void)argument; return 1; }

static void
put16(unsigned offset, uint16_t value)
{
	memcpy(config + offset, &value, sizeof(value));
}

static void
put32(unsigned offset, uint32_t value)
{
	memcpy(config + offset, &value, sizeof(value));
}

int
main(void)
{
	static const struct drv_pci_bus_ops operations = {
		.config_space_size = 4096,
		.config_read = config_read,
		.config_write = config_write,
		.map_bar = map_bar,
		.unmap_bar = unmap_bar,
		.allocate_irqs = allocate_irqs
	};
	struct drv_pci_address address = { 0, 0, 2, 0 };
	struct drv_pci_device *device;
	struct drv_pci_bus *bus;
	struct drv_pci_irq irq;
	unsigned count = 0;
	void *cookie;
	uint32_t value;

	memset(config, 0, sizeof(config));
	put32(0x00, 0x56781234U);
	put32(0x08, 0x02000001U);
	put16(0x06, 0x0010U);
	config[0x34] = 0x50U;
	config[0x50] = 0x05U;
	config[0x51] = 0;
	put16(0x52, 0x00b0U);
	assert(drv_pci_init() == 0);
	assert(drv_pci_bus_create_root(0, 0, &operations, NULL, NULL, &bus) == 0);
	assert(drv_pci_bus_scan(bus) == 0);
	device = drv_pci_find_device(&address);
	assert(device != NULL);
	assert(drv_pci_device_allocate_irqs(device, DRV_PCI_IRQ_ALLOW_MSI,
	    1, 1, &irq, &count) == 0 && count == 1);
	assert(drv_pci_device_establish_irq(device, &irq, handler, NULL, "test",
	    &cookie) == 0);
	assert(strcmp(registered_source, "PCI 0000:00:02.0") == 0);
	memcpy(&value, config + 0x54, sizeof(value));
	assert(value == 0xfee00000U);
	memcpy(&value, config + 0x5c, sizeof(uint16_t));
	assert((value & 0xffffU) == 0xd0U);
	assert((config[0x52] & 1U) != 0);
	assert((config[0x52] & 0x70U) == 0);
	drv_pci_device_disestablish_irq(device, cookie);
	assert(unregistered_irq == registered_irq);
	memcpy(&value, config + 0x54, sizeof(value));
	assert(value == 0U);
	memcpy(&value, config + 0x58, sizeof(value));
	assert(value == 0U);
	memcpy(&value, config + 0x5c, sizeof(uint16_t));
	assert((value & 0xffffU) == 0U);
	memcpy(&value, config + 0x52, sizeof(uint16_t));
	assert((value & 0xffffU) == 0x00b0U);

	config[0x50] = 0x11U;
	put16(0x52, 0x4000U);
	put32(0x54, 0);
	put32(0x10, 0x1000U);
	msix_table[0] = 0x11111111U;
	msix_table[1] = 0x22222222U;
	msix_table[2] = 0x33333333U;
	msix_table[3] = 0x44444444U;
	assert(drv_pci_device_allocate_irqs(device, DRV_PCI_IRQ_ALLOW_MSIX,
	    1, 1, &irq, &count) == 0);
	assert(drv_pci_device_establish_irq(device, &irq, handler, NULL, "test",
	    &cookie) == 0);
	assert(msix_table[0] == 0xfee00000U);
	assert(msix_table[1] == 0);
	assert(msix_table[2] == 0xd0U);
	assert((msix_table[3] & 1U) == 0);
	drv_pci_device_disestablish_irq(device, cookie);
	assert(msix_table[0] == 0x11111111U);
	assert(msix_table[1] == 0x22222222U);
	assert(msix_table[2] == 0x33333333U);
	assert(msix_table[3] == 0x44444444U);
	memcpy(&value, config + 0x52, sizeof(uint16_t));
	assert((value & 0xffffU) == 0x4000U);
	return 0;
}
