/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * PC/AT PCI Configuration Mechanism #1 host. Copyright (C) 2026 Awe Morris;
 * SPDX-License-Identifier: Zlib
 */
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

struct pcat_bar_mapping {
	struct drv_pci_device *device;
	unsigned bar_index;
	uint64_t bus_address;
	size_t size;
	void *virtual_address;
	unsigned references;
	struct pcat_bar_mapping *next;
};

static struct pcat_bar_mapping *bar_mappings;

#if defined(__x86_64__)
extern int amd64_acpi_ecam_address(uint16_t, uint8_t, uint8_t, uint8_t,
				   paddr_t *);
extern int amd64_acpi_ecam_pointer(uint16_t, uint8_t, uint8_t, uint8_t,
				   volatile uint8_t **);
#endif

static int ecam_function_address(const struct drv_pci_address *address, paddr_t *result);
static int pcat_config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *result);
static int ecam_map(const struct drv_pci_address *address, volatile uint8_t **result);
static bool lock_enter(void);
static void port_write32(uint16_t port, uint32_t value);
static uint32_t address_value(const struct drv_pci_address *address, unsigned offset);
static uint32_t port_read32(uint16_t port);
static void lock_leave(bool enabled);
static int pcat_config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int pcat_map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void pcat_unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int pcat_allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void pcat_free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);

static const struct drv_pci_bus_ops pcat_bus_ops = {
	.config_space_size = 4096,
	.config_read = pcat_config_read,
	.config_write = pcat_config_write,
	.map_bar = pcat_map_bar,
	.unmap_bar = pcat_unmap_bar,
	.allocate_irqs = pcat_allocate_irqs,
	.free_irqs = pcat_free_irqs};

/*
 * Implements the drv pci pcat init operation.
 */
int
drv_pci_pcat_init(
	void)
{
	int function_result;

#ifdef ZEDBSD_TEST_CHECKPOINTS
	uint32_t value0, value4;
	const struct drv_pci_address address0 = {0, 0, 0, 0};
	const struct drv_pci_address address4 = {0, 0, 4, 0};
#endif

	/* Describes what this bus can address for DMA. */
	const struct drv_dma_constraints constraints = {
		.address_bits = 32,
		.max_segment_size = 16U * 1024U * 1024U,
		.segment_boundary = 0,
		.coherent = 1};
	struct drv_pci_bus *root;
	paddr_t ecam;
	int error;

	/* Checks the ecam function address result. */
	if (ecam_function_address(&(const struct drv_pci_address){0, 0, 0, 0},
				  &ecam) == HAL_OK) {
		hal_printf("pci: ECAM segment 0000 bus 00 at %08x:%08x\n",
			   (uint32_t)((uint64_t)ecam >> 32), (uint32_t)ecam);
	}

#ifdef ZEDBSD_TEST_CHECKPOINTS
	value0 = 0;
	value4 = 0;
	(void)pcat_config_read(NULL, &address0, 0, 4, &value0);
	(void)pcat_config_read(NULL, &address4, 0, 4, &value4);
	hal_printf("WS004 PCI CONFIG %08x %08x\n", value0, value4);
#endif

	/* Checks the operation status. */
	error = drv_dma_device_create(&constraints, &pcat_dma);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_pci_bus_create_root(0, 0, &pcat_bus_ops, NULL, pcat_dma,
					&root);
	if (error != 0) {
		(void)drv_dma_device_destroy(pcat_dma);

		/* Failed. */
		return error;
	}

	/* Obtains the drv pci scan all result. */
	function_result = drv_pci_scan_all();

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ecam function address operation. */
static int
ecam_function_address(
	const struct drv_pci_address *address,
	paddr_t *result)
{
#if defined(__x86_64__)
	int error;

	/* Obtains the amd64 acpi ecam address result. */
	error = amd64_acpi_ecam_address(address->segment,
						  address->bus, address->device,
						  address->function, result);

	/* Failed. */
	return error;

#else
	(void)address;
	(void)result;

	/* Returns the computed result. */
	return HAL_ERR_UNSUPPORTED;
#endif
}

/* Supports the pcat config read operation. */
static int
pcat_config_read(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *result)
{
	volatile uint8_t *base_local;
	volatile uint8_t *base_local1;
	int error;
	uint32_t value;
	bool enabled;

	(void)context;

	/* Handles the address availability. */
	if (address == NULL || result == NULL || address->device >= 32U ||
	    address->function >= 8U || offset + width > 4096U ||
	    (width != 1 && width != 2 && width != 4) ||
	    (width == 2 && (offset & 1U)) || (width == 4 && (offset & 3U))) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the current offset. */
	if (offset >= 256U || address->segment != 0) {
		/* Checks the operation status. */
		error = ecam_map(address, &base_local);
		if (error != 0)
			return error;

		/* Handles the width condition. */
		if (width == 1)
			value = base_local[offset];
		else if (width == 2)
			value = *(volatile uint16_t *)(base_local + offset);
		else
			value = *(volatile uint32_t *)(base_local + offset);
		*result = value;
		/* Succeeded. */
		return 0;
	}

	/* Checks the ecam map result. */
	if (ecam_map(address, &base_local1) == 0) {
		/* Handles the width condition. */
		if (width == 1)
			value = base_local1[offset];
		else if (width == 2)
			value = *(volatile uint16_t *)(base_local1 + offset);
		else
			value = *(volatile uint32_t *)(base_local1 + offset);
		*result = value;
		/* Succeeded. */
		return 0;
	}

	enabled = lock_enter();
	port_write32(PCI_CONFIG_ADDRESS, address_value(address, offset));
	value = port_read32(PCI_CONFIG_DATA);
	lock_leave(enabled);
	value >>= (offset & 3U) * 8U;

	/* Handles the width condition. */
	if (width == 1)
		value &= 0xffU;
	else if (width == 2)
		value &= 0xffffU;
	*result = value;
	/* Succeeded. */
	return 0;
}

/* Supports the ecam map operation. */
static int
ecam_map(
	const struct drv_pci_address *address,
	volatile uint8_t **result)
{
#if defined(__x86_64__)
	int error;

	/* Computes the function result. */
	error =
		amd64_acpi_ecam_pointer(address->segment, address->bus,
					address->device, address->function,
					result) == HAL_OK
			? 0
			: ENOTSUP;

	/* Failed. */
	return error;

#else
	(void)address;
	(void)result;

	/* Failed. */
	return ENOTSUP;
#endif
}

/* Supports the lock enter operation. */
static bool
lock_enter(
	void)
{
	bool enabled = hal_irq_disable();

	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&config_lock, 1U, __ATOMIC_ACQUIRE) != 0)
		__asm__ volatile("pause");

	/* Returns the computed result. */
	return enabled;
}

/* Supports the port write32 operation. */
static void
port_write32(
	uint16_t port,
	uint32_t value)
{
	__asm__ volatile("outl %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the address value operation. */
static uint32_t
address_value(
	const struct drv_pci_address *address,
	unsigned offset)
{
	/* Returns the computed result. */
	return 0x80000000U | ((uint32_t)address->bus << 16) |
	       ((uint32_t)address->device << 11) |
	       ((uint32_t)address->function << 8) | (offset & ~3U);
}

/* Supports the port read32 operation. */
static uint32_t
port_read32(
	uint16_t port)
{
	uint32_t value;

	__asm__ volatile("inl %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the lock leave operation. */
static void
lock_leave(
	bool enabled)
{
	__atomic_store_n(&config_lock, 0U, __ATOMIC_RELEASE);

	/* Handles the enabled condition. */
	if (enabled)
		hal_irq_enable();
}

/* Supports the pcat config write operation. */
static int
pcat_config_write(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	volatile uint8_t *base;
	uint32_t current, shift, mask;
	bool enabled;

	(void)context;

	/* Handles the address availability. */
	if (address == NULL || address->device >= 32U ||
	    address->function >= 8U || offset + width > 4096U ||
	    (width != 1 && width != 2 && width != 4) ||
	    (width == 2 && (offset & 1U)) || (width == 4 && (offset & 3U))) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the ecam map result. */
	if (ecam_map(address, &base) == 0) {
		/* Handles the width condition. */
		if (width == 1)
			base[offset] = (uint8_t)value;
		else if (width == 2) {
			*(volatile uint16_t *)(base + offset) = (uint16_t)value;
		} else {
			*(volatile uint32_t *)(base + offset) = value;
		}

		hal_io_mb();

		/* Succeeded. */
		return 0;
	}

	/* Handles the address condition. */
	if (address->segment != 0 || offset >= 256U)
		return ENOTSUP;
	enabled = lock_enter();
	port_write32(PCI_CONFIG_ADDRESS, address_value(address, offset));

	/* Handles the width condition. */
	if (width == 4) {
		current = value;
	} else {
		current = port_read32(PCI_CONFIG_DATA);
		shift = (offset & 3U) * 8U;
		mask = (width == 1 ? 0xffU : 0xffffU) << shift;
		current = (current & ~mask) | ((value << shift) & mask);
		port_write32(PCI_CONFIG_ADDRESS,
			     address_value(address, offset));
	}

	port_write32(PCI_CONFIG_DATA, current);
	lock_leave(enabled);

	/* Succeeded. */
	return 0;
}

/* Supports the pcat map bar operation. */
static int
pcat_map_bar(
	void *context,
	struct drv_pci_device *device,
	const struct drv_pci_bar *bar,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	uint32_t alignment;
	uint32_t assigned;
	struct hal_pmem_request request;
	struct hal_pmem *memory;
	struct pcat_bar_mapping *record;

	(void)context;

	/* Handles the bar availability. */
	if (bar == NULL || mapping == NULL || bar->type == DRV_PCI_BAR_IO ||
	    bar->bus_address == 0 || bar->size == 0) {
		/* Failed. */
		return EINVAL;
	}

	/*
	 * MSI-X tables commonly occupy a small region of a BAR which the device
	 * driver has already mapped.  Reuse that mapping rather than asking the
	 * HAL to claim overlapping physical memory a second time.
	 */
	for (record = bar_mappings; record != NULL; record = record->next) {
		/* Handles the record condition. */
		if (record->device == device &&
		    record->bar_index == bar->index &&
		    bar->bus_address >= record->bus_address &&
		    bar->bus_address - record->bus_address <= record->size &&
		    bar->size <= record->size - (size_t)(bar->bus_address -
							 record->bus_address)) {
			mapping->address = (uint8_t *)record->virtual_address +
					   (size_t)(bar->bus_address -
						    record->bus_address);
			mapping->size = bar->size;
			mapping->type = bar->type;
			mapping->private_data[0] = 0;
			mapping->private_data[1] = (uintptr_t)record;
			record->references++;

			/* Succeeded. */
			return 0;
		}
	}

	/* Handles the memory availability. */
	memory = hal_malloc(sizeof(*memory));
	if (memory == NULL)
		return ENOMEM;
	request.paddr = bar->bus_address;
	request.size = (size_t)bar->size;
	request.alignment = 4096U;
	request.type = HAL_PMEM_TYPE_MMIO;
	request.attr = (flags & DRV_PCI_MAP_WRITETHROUGH)
			       ? HAL_PMEM_ATTR_WRITETHRU
			       : HAL_PMEM_ATTR_NOCACHE;

	/* Checks the hal pmem alloc result. */
	if (hal_pmem_alloc(&request, memory) != HAL_OK) {
		/* The initial PC/AT HAL exposes one 16-MiB PCI MMIO window. */
		/* A 64-bit BAR may still be reassigned below 4 GiB. */
		if ((bar->type != DRV_PCI_BAR_MEMORY32 &&
		     bar->type != DRV_PCI_BAR_MEMORY64) ||
		    bar->size > 0x01000000U) {
			hal_free(memory);

			/* Failed. */
			return ENOMEM;
		}

		/* Handles the bar condition. */
		if (bar->size >= 0x00400000U) {
			assigned = 0xf0000000U;
		} else {
			alignment = (uint32_t)bar->size;

			/* Handles the assigned condition. */
			assigned = (pci_small_mmio_next + alignment - 1U) &
				   ~(alignment - 1U);
			if (assigned > 0xf1000000U - bar->size) {
				hal_free(memory);

				/* Failed. */
				return ENOMEM;
			}

			pci_small_mmio_next = assigned + (uint32_t)bar->size;
		}

		/* Checks the drv pci device assign bar result. */
		if (drv_pci_device_assign_bar(device, bar->index, assigned) !=
		    0) {
			hal_free(memory);

			/* Failed. */
			return ENOMEM;
		}

		request.paddr = assigned;

		/* Checks the hal pmem alloc result. */
		if (hal_pmem_alloc(&request, memory) != HAL_OK) {
			hal_free(memory);

			/* Failed. */
			return ENOMEM;
		}

		hal_printf("pci: BAR%u assigned to %08x (%u KiB)\n", bar->index,
			   assigned, (unsigned)(bar->size / 1024U));
	}

	mapping->address = memory->vaddr;
	mapping->size = memory->size;
	mapping->type = bar->type;
	mapping->private_data[0] = (uintptr_t)memory;

	/* Handles the record availability. */
	record = hal_malloc(sizeof(*record));
	if (record == NULL) {
		(void)hal_pmem_free(memory);
		hal_free(memory);
		memset(mapping, 0, sizeof(*mapping));

		/* Failed. */
		return ENOMEM;
	}

	record->device = device;
	record->bar_index = bar->index;
	record->bus_address = request.paddr;
	record->size = request.size;
	record->virtual_address = memory->vaddr;
	record->references = 1;
	record->next = bar_mappings;
	bar_mappings = record;
	mapping->private_data[1] = (uintptr_t)record;

	/* Succeeded. */
	return 0;
}

/* Supports the pcat unmap bar operation. */
static void
pcat_unmap_bar(
	void *context,
	struct drv_pci_mapping *mapping)
{
	struct hal_pmem *memory;
	struct pcat_bar_mapping *record, **link;

	(void)context;

	/* Handles the mapping availability. */
	if (mapping == NULL)
		return;

	/* Handles the mapping condition. */
	record = (struct pcat_bar_mapping *)mapping->private_data[1];
	if (mapping->private_data[0] == 0) {
		/* Handles the record availability. */
		if (record != NULL && record->references != 0)
			record->references--;
		memset(mapping, 0, sizeof(*mapping));

		/* Returns the computed result. */
		return;
	}

	memory = (struct hal_pmem *)mapping->private_data[0];

	/* Handles the record availability. */
	if (record != NULL) {
		/* Handles the record condition. */
		if (record->references != 1U) {
			hal_printf("pci: BAR%u unmap retained with %u "
				   "references\n",
				   record->bar_index, record->references);

			/* Returns the computed result. */
			return;
		}

		/* Process each linked entry. */
		for (link = &bar_mappings; *link != NULL;
		     link = &(*link)->next) {
			/* Handles the link condition. */
			if (*link == record) {
				*link = record->next;
				break;
			}
		}

		hal_free(record);
	}

	(void)hal_pmem_free(memory);
	hal_free(memory);
	memset(mapping, 0, sizeof(*mapping));
}

/* Supports the pcat allocate irqs operation. */
static int
pcat_allocate_irqs(
	void *context,
	struct drv_pci_device *device,
	enum drv_pci_irq_type type,
	unsigned minimum,
	unsigned maximum,
	struct drv_pci_irq *irqs,
	unsigned *count)
{
	uint8_t id;
	uint8_t line;
	unsigned capability;

	(void)context;

	/* Handles the device availability. */
	if (device == NULL || irqs == NULL || count == NULL)
		return EINVAL;

	/* Handles the minimum condition. */
	if (minimum != 1U || maximum < minimum)
		return ENOTSUP;

	/* Handles the type condition. */
	if (type == DRV_PCI_IRQ_MSI || type == DRV_PCI_IRQ_MSIX) {
		/* Checks the drv pci device find capability result. */
		id = type == DRV_PCI_IRQ_MSI ? 0x05U : 0x11U;
		if (drv_pci_device_find_capability(device, id, &capability) !=
		    0) {
			/* Failed. */
			return ENOTSUP;
		}
		irqs[0].type = type;
		irqs[0].index = 0;
		irqs[0].vector = 0;
		irqs[0].private_data[0] = capability;
		irqs[0].private_data[1] = 0;
		*count = 1;
		/* Succeeded. */
		return 0;
	}

	/* Handles the type condition. */
	if (type != DRV_PCI_IRQ_INTX)
		return ENOTSUP;

	/* Checks the drv pci device config read8 result. */
	if (drv_pci_device_config_read8(device, 0x3cU, &line) != 0 ||
	    line == 0xffU || line >= 16U) {
		/* Failed. */
		return ENODEV;
	}
	irqs[0].type = type;
	irqs[0].index = 0;
	irqs[0].vector = line;
	irqs[0].private_data[0] = irqs[0].private_data[1] = 0;
	*count = 1;
	/* Succeeded. */
	return 0;
}

/* Supports the pcat free irqs operation. */
static void
pcat_free_irqs(
	void *context,
	struct drv_pci_device *device,
	struct drv_pci_irq *irqs,
	unsigned count)
{
	(void)context;
	(void)device;
	(void)irqs;
	(void)count;
}
