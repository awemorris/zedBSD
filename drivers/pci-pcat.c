/* PC/AT PCI Configuration Mechanism #1 host. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/pci-pcat.h>
#include <drivers/pci.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PCI_CONFIG_ADDRESS 0x0cf8U
#define PCI_CONFIG_DATA 0x0cfcU

static volatile unsigned config_lock;
static struct drv_dma_device *pcat_dma;
static uint32_t pci_small_mmio_next = 0xf0800000U;

static uint32_t port_read32(uint16_t port)
{
	uint32_t value;
	__asm__ volatile("inl %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void port_write32(uint16_t port, uint32_t value)
{
	__asm__ volatile("outl %0,%w1" : : "a"(value), "Nd"(port));
}

static bool lock_enter(void)
{
	bool enabled = hal_irq_disable();
	while (__atomic_exchange_n(&config_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");
	return enabled;
}

static void lock_leave(bool enabled)
{
	__atomic_store_n(&config_lock, 0U, __ATOMIC_RELEASE);
	if (enabled) hal_irq_enable();
}

static uint32_t address_value(const struct drv_pci_address *address,
	unsigned offset)
{
	return 0x80000000U | ((uint32_t)address->bus << 16) |
	    ((uint32_t)address->device << 11) |
	    ((uint32_t)address->function << 8) | (offset & ~3U);
}

static int pcat_config_read(void *context,
	const struct drv_pci_address *address, unsigned offset, unsigned width,
	uint32_t *result)
{
	uint32_t value;
	bool enabled;
	(void)context;
	if (address == NULL || result == NULL || address->segment != 0 ||
	    address->device >= 32U || address->function >= 8U ||
	    offset + width > 256U || (width != 1 && width != 2 && width != 4) ||
	    (width == 2 && (offset & 1U)) || (width == 4 && (offset & 3U)))
		return EINVAL;
	enabled = lock_enter();
	port_write32(PCI_CONFIG_ADDRESS, address_value(address, offset));
	value = port_read32(PCI_CONFIG_DATA);
	lock_leave(enabled);
	value >>= (offset & 3U) * 8U;
	if (width == 1) value &= 0xffU;
	else if (width == 2) value &= 0xffffU;
	*result = value;
	return 0;
}

static int pcat_config_write(void *context,
	const struct drv_pci_address *address, unsigned offset, unsigned width,
	uint32_t value)
{
	uint32_t current, shift, mask;
	bool enabled;
	(void)context;
	if (address == NULL || address->segment != 0 || address->device >= 32U ||
	    address->function >= 8U || offset + width > 256U ||
	    (width != 1 && width != 2 && width != 4) ||
	    (width == 2 && (offset & 1U)) || (width == 4 && (offset & 3U)))
		return EINVAL;
	enabled = lock_enter();
	port_write32(PCI_CONFIG_ADDRESS, address_value(address, offset));
	if (width == 4) {
		current = value;
	} else {
		current = port_read32(PCI_CONFIG_DATA);
		shift = (offset & 3U) * 8U;
		mask = (width == 1 ? 0xffU : 0xffffU) << shift;
		current = (current & ~mask) | ((value << shift) & mask);
		port_write32(PCI_CONFIG_ADDRESS, address_value(address, offset));
	}
	port_write32(PCI_CONFIG_DATA, current);
	lock_leave(enabled);
	return 0;
}

static int pcat_map_bar(void *context, struct drv_pci_device *device,
	const struct drv_pci_bar *bar, unsigned flags,
	struct drv_pci_mapping *mapping)
{
	struct hal_pmem_request request;
	struct hal_pmem *memory;
	(void)context; (void)device;
	if (bar == NULL || mapping == NULL || bar->type == DRV_PCI_BAR_IO ||
	    bar->bus_address == 0 || bar->size == 0)
		return EINVAL;
	memory = hal_malloc(sizeof(*memory));
	if (memory == NULL) return ENOMEM;
	request.paddr = bar->bus_address; request.size = (size_t)bar->size;
	request.alignment = 4096U; request.type = HAL_PMEM_TYPE_MMIO;
	request.attr = (flags & DRV_PCI_MAP_WRITETHROUGH) ?
	    HAL_PMEM_ATTR_WRITETHRU : HAL_PMEM_ATTR_NOCACHE;
	if (hal_pmem_alloc(&request, memory) != HAL_OK) {
		uint32_t assigned;
		/* The initial PC/AT HAL exposes one 16-MiB PCI MMIO window. */
		if (bar->type != DRV_PCI_BAR_MEMORY32 || bar->size > 0x01000000U) {
			hal_free(memory); return ENOMEM;
		}
		if (bar->size >= 0x00400000U) {
			assigned = 0xf0000000U;
		} else {
			uint32_t alignment = (uint32_t)bar->size;
			assigned = (pci_small_mmio_next + alignment - 1U) &
			    ~(alignment - 1U);
			if (assigned > 0xf1000000U - bar->size) {
				hal_free(memory); return ENOMEM;
			}
			pci_small_mmio_next = assigned + (uint32_t)bar->size;
		}
		if (drv_pci_device_assign_bar(device, bar->index, assigned) != 0) {
			hal_free(memory); return ENOMEM;
		}
		request.paddr = assigned;
		if (hal_pmem_alloc(&request, memory) != HAL_OK) {
			hal_free(memory); return ENOMEM;
		}
		hal_printf("pci: BAR%u assigned to %08x (%u KiB)\n",
		    bar->index, assigned, (unsigned)(bar->size / 1024U));
	}
	mapping->address = memory->vaddr; mapping->size = memory->size;
	mapping->type = bar->type; mapping->private_data[0] = (uintptr_t)memory;
	mapping->private_data[1] = 0; return 0;
}

static void pcat_unmap_bar(void *context, struct drv_pci_mapping *mapping)
{
	struct hal_pmem *memory;
	(void)context;
	if (mapping == NULL || mapping->private_data[0] == 0) return;
	memory = (struct hal_pmem *)mapping->private_data[0];
	(void)hal_pmem_free(memory); hal_free(memory);
	memset(mapping, 0, sizeof(*mapping));
}

static int pcat_allocate_irqs(void *context, struct drv_pci_device *device,
	enum drv_pci_irq_type type, unsigned minimum, unsigned maximum,
	struct drv_pci_irq *irqs, unsigned *count)
{
	uint8_t line;
	(void)context; (void)minimum; (void)maximum;
	if (device == NULL || irqs == NULL || count == NULL) return EINVAL;
	if (type != DRV_PCI_IRQ_INTX) return ENOTSUP;
	if (drv_pci_device_config_read8(device, 0x3cU, &line) != 0 ||
	    line == 0xffU || line >= 16U) return ENODEV;
	irqs[0].type = type; irqs[0].index = 0; irqs[0].vector = line;
	irqs[0].private_data[0] = irqs[0].private_data[1] = 0;
	*count = 1; return 0;
}

static void pcat_free_irqs(void *context, struct drv_pci_device *device,
	struct drv_pci_irq *irqs, unsigned count)
{ (void)context; (void)device; (void)irqs; (void)count; }

static const struct drv_pci_bus_ops pcat_bus_ops = {
	.config_read = pcat_config_read, .config_write = pcat_config_write,
	.map_bar = pcat_map_bar, .unmap_bar = pcat_unmap_bar,
	.allocate_irqs = pcat_allocate_irqs, .free_irqs = pcat_free_irqs
};

int drv_pci_pcat_init(void)
{
	const struct drv_dma_constraints constraints = {
		.address_bits = 32, .max_segment_size = 16U * 1024U * 1024U,
		.segment_boundary = 0, .coherent = 1
	};
	struct drv_pci_bus *root;
	int error;
	error = drv_dma_device_create(&constraints, &pcat_dma);
	if (error != 0) return error;
	error = drv_pci_bus_create_root(0, 0, &pcat_bus_ops, NULL, pcat_dma,
	    &root);
	if (error != 0) { (void)drv_dma_device_destroy(pcat_dma); return error; }
	return drv_pci_scan_all();
}
