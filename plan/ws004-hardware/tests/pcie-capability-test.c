#include <drivers/pci.h>
#include <hal/hal.h>

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

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
bool hal_irq_disable(void) { return true; }
void hal_irq_enable(void) { }
void
hal_irq_mask(int irq)
{
	(void)irq;
}
void
hal_irq_unmask(int irq)
{
	(void)irq;
}
void
hal_irq_send_eoi(hal_irq_ack_t acknowledge)
{
	(void)acknowledge;
}
void hal_io_mb(void) { }
void hal_io_rmb(void) { }
int
hal_irq_set_handler(int irq, hal_irq_handler_t handler, void *argument)
{
	(void)irq;
	(void)handler;
	(void)argument;
	return HAL_OK;
}
int hal_irq_register_msi(const char *s, hal_irq_handler_t h, void *a, int *i,
	paddr_t *p, uint32_t *e)
{
	(void)s; (void)h; (void)a; (void)i; (void)p; (void)e;
	return HAL_ERR_UNSUPPORTED;
}
int hal_irq_unregister_msi(int irq) { (void)irq; return HAL_ERR_UNSUPPORTED; }

static int
config_read(void *context, const struct drv_pci_address *address,
	    unsigned offset, unsigned width, uint32_t *result)
{
	(void)context;
	(void)width;
	*result = 0;
	if (address->bus == 0 && address->device == 1) {
		if (offset == 0)
			*result = 0x11111234U;
		else if (offset == 8)
			*result = 0x06040001U;
		else if (offset == 0x0c)
			*result = 0x00010000U;
		else if (offset == 0x19)
			*result = 2U;
		else if (offset == 0x100)
			*result = 0x12010001U;
		else if (offset == 0x120)
			*result = 0x00010010U;
		return 0;
	}
	if (address->bus == 2 && address->device == 0) {
		if (offset == 0)
			*result = 0x22221234U;
		else if (offset == 8)
			*result = 0x0c033001U;
		return 0;
	}
	*result = 0xffffffffU;
	return 0;
}

static int
config_write(void *context, const struct drv_pci_address *address,
	     unsigned offset, unsigned width, uint32_t value)
{
	(void)context;
	(void)address;
	(void)offset;
	(void)width;
	(void)value;
	return 0;
}

static int
count_device(struct drv_pci_device *device, void *argument)
{
	(void)device;
	(*(unsigned *)argument)++;
	return 0;
}

int
main(void)
{
	static const struct drv_pci_bus_ops operations = {
	    .config_space_size = 4096,
	    .config_read = config_read,
	    .config_write = config_write,
	};
	struct drv_pci_address address = {0, 0, 1, 0};
	struct drv_pci_bus *bus;
	struct drv_pci_device *bridge;
	unsigned capability = 0, count = 0;

	assert(drv_pci_init() == 0);
	assert(drv_pci_bus_create_root(0, 0, &operations, NULL, NULL, &bus) ==
	       0);
	assert(drv_pci_bus_scan_tree(bus) == 0);
	bridge = drv_pci_find_device(&address);
	assert(bridge != NULL);
	assert(drv_pci_device_find_extended_capability(bridge, 0x10, 0,
						       &capability) == 0);
	assert(capability == 0x120);
	assert(drv_pci_foreach_device(count_device, &count) == 0);
	assert(count == 2);
	return 0;
}
