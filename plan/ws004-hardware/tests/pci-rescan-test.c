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
	if (address->device != 1 || address->function != 0) {
		*result = 0xffffffffU;
		return 0;
	}
	switch (offset) {
	case 0:
		*result = 0x11111234U;
		break;
	case 8:
		*result = 0x02000001U;
		break;
	case 0x0c:
		*result = 0;
		break;
	default:
		*result = 0;
		break;
	}
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
	unsigned *count = argument;
	(void)device;
	(*count)++;
	return 0;
}

int
main(void)
{
	static const struct drv_pci_bus_ops operations = {
	    .config_read = config_read,
	    .config_write = config_write,
	};
	struct drv_pci_bus *bus;
	unsigned count = 0;

	assert(drv_pci_init() == 0);
	assert(drv_pci_bus_create_root(0, 0, &operations, NULL, NULL, &bus) ==
	       0);
	assert(drv_pci_bus_scan(bus) == 0);
	assert(drv_pci_bus_rescan(bus) == 0);
	assert(drv_pci_bus_foreach_device(bus, count_device, &count) == 0);
	assert(count == 1);
	return 0;
}
