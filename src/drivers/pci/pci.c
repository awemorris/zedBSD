/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * Generic PCI bus core. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier:
 * Zlib */
#include <drivers/pci.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PCI_COMMAND 0x04U
#define PCI_STATUS 0x06U
#define PCI_CLASS_REVISION 0x08U
#define PCI_HEADER_TYPE 0x0eU
#define PCI_BAR0 0x10U
#define PCI_CAPABILITIES 0x34U
#define PCI_INTERRUPT_LINE 0x3cU
#define PCI_COMMAND_IO 0x0001U
#define PCI_COMMAND_MEMORY 0x0002U
#define PCI_COMMAND_MASTER 0x0004U
#define PCI_COMMAND_INTX_DISABLE 0x0400U
#define PCI_COMMAND_ENABLE_MASK                                                \
	(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)
#define PCI_MSI_CONTROL 0x02U
#define PCI_MSI_ADDRESS 0x04U
#define PCI_MSI_ENABLE 0x0001U
#define PCI_MSI_MME_MASK 0x0070U
#define PCI_MSI_64BIT 0x0080U
#define PCI_MSIX_CONTROL 0x02U
#define PCI_MSIX_TABLE 0x04U
#define PCI_MSIX_ENABLE 0x8000U
#define PCI_MSIX_FUNCTION_MASK 0x4000U
#define PCI_MSIX_ENTRY_SIZE 16U
#define PCI_MSIX_ENTRY_MASK 0x00000001U

struct drv_pci_bus {
	uint16_t segment;
	uint8_t number;
	const struct drv_pci_bus_ops *ops;
	void *host;
	struct drv_dma_device *dma;
	struct drv_pci_bus *parent, *next;
	struct drv_pci_device *bridge, *devices;
};

struct drv_pci_device {
	struct drv_pci_address address;
	struct drv_pci_bus *bus;
	struct drv_pci_device *next;
	struct drv_pci_bus *subordinate;
	struct drv_pci_driver *driver;
	void *driver_data;
	uint16_t vendor, product, subvendor, subproduct;
	uint32_t class_code;
	uint8_t revision, header_type;
	unsigned enable_count, bar_count;
	struct drv_pci_bar bars[6];
	uint8_t bar_claimed[6];
	uint32_t irq_claimed;
};

struct pci_driver_entry {
	struct drv_pci_driver *driver;
	struct pci_driver_entry *next;
};

struct pci_intx_line;

struct pci_irq_cookie {
	int irq;
	drv_pci_irq_handler_t handler;
	void *argument;
	struct drv_pci_device *device;
	struct pci_intx_line *intx_line;
	struct pci_irq_cookie *intx_next;
	enum drv_pci_irq_type type;
	unsigned capability, index;
	struct drv_pci_mapping table;
	unsigned table_mapped;
	unsigned message_registered;
	unsigned msi_state_saved;
	uint16_t msi_control_saved;
	uint16_t msi_data_saved;
	uint32_t msi_address_low_saved;
	uint32_t msi_address_high_saved;
	unsigned msix_state_saved;
	uint16_t msix_control_saved;
	uint32_t msix_entry_saved[4];
};

struct pci_intx_line {
	int irq;
	unsigned dispatching;
	unsigned removing;
	struct pci_irq_cookie *handlers;
	struct pci_intx_line *next;
};

static struct drv_pci_bus *root_buses;
static struct pci_driver_entry *drivers;
static struct pci_intx_line *intx_lines;
static volatile unsigned intx_lock;
static int initialized;

int drv_pci_device_probe(struct drv_pci_device *device);

static int cfg_read(struct drv_pci_bus *bus, const struct drv_pci_address *a, unsigned offset, unsigned width, uint32_t *value);
static struct drv_pci_device * find_device_on_bus(struct drv_pci_bus *bus, const struct drv_pci_address *address);
static int read_device(struct drv_pci_device *device);
static int cfg_write(struct drv_pci_bus *bus, const struct drv_pci_address *a, unsigned offset, unsigned width, uint32_t value);
static int foreach_device_tree(struct drv_pci_bus *b, drv_pci_device_iterator_t fn, void *arg);
static int command_set(struct drv_pci_device *d, uint16_t set, uint16_t clear);
static int pci_bar_read_raw(struct drv_pci_device *device, unsigned index, enum drv_pci_bar_type type, uint32_t *low, uint32_t *high);
static int pci_command_quiesce(struct drv_pci_device *device, uint16_t command);
static int pci_bar_write_raw(struct drv_pci_device *device, unsigned index, enum drv_pci_bar_type type, uint32_t low, uint32_t high);
static void pci_bar_cache_readback(struct drv_pci_device *device, unsigned index, struct drv_pci_bar *bar);
static uint64_t pci_bar_address(enum drv_pci_bar_type type, uint32_t low, uint32_t high);
static int establish_intx(struct pci_irq_cookie *cookie, const struct drv_pci_irq *irq);
static bool intx_lock_enter(void);
static struct pci_intx_line *find_intx_line(int irq);
static void intx_lock_leave(bool enabled);
static int establish_msi(struct pci_irq_cookie *cookie, const struct drv_pci_irq *irq);
static void pci_source(const struct drv_pci_address *address, char result[17]);
static int establish_msix(struct pci_irq_cookie *cookie);
static int map_msix_entry(struct pci_irq_cookie *cookie);
static int disestablish_intx(struct pci_irq_cookie *cookie);
static void pci_irq_dispatch(int irq, hal_irq_ack_t acknowledge, void *argument);
static void pci_intx_dispatch(int irq, hal_irq_ack_t acknowledge, void *argument);

/*
 * Implements the drv pci init operation.
 */
int
drv_pci_init(
	void)
{
	/* Handles the initialized condition. */
	if (initialized)
		return EALREADY;
	root_buses = NULL;
	drivers = NULL;
	intx_lines = NULL;
	intx_lock = 0;
	initialized = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci shutdown operation.
 */
void
drv_pci_shutdown(
	void)
{
	struct drv_pci_device *device;
	struct drv_pci_bus *bus;

	/* Process each linked entry. */
	for (bus = root_buses; bus != NULL; bus = bus->next) {
		/* Process each linked entry. */
		for (device = bus->devices; device != NULL;
		     device = device->next) {
			/* Handles the driver availability. */
			if (device->driver != NULL &&
			    device->driver->shutdown != NULL)
				device->driver->shutdown(device);
		}
	}
}

/*
 * Implements the drv pci bus create root operation.
 */
int
drv_pci_bus_create_root(
	uint16_t segment,
	uint8_t number,
	const struct drv_pci_bus_ops *ops,
	void *host,
	struct drv_dma_device *dma,
	struct drv_pci_bus **result)
{
	struct drv_pci_bus *bus;

	/* Handles the ops availability. */
	if (!initialized || ops == NULL || ops->config_read == NULL ||
	    ops->config_write == NULL || result == NULL)

		/* Returns the computed result. */
		return EINVAL;
	bus = hal_malloc(sizeof(*bus));

	/* Handles the bus availability. */
	if (bus == NULL)
		return ENOMEM;
	memset(bus, 0, sizeof(*bus));
	bus->segment = segment;
	bus->number = number;
	bus->ops = ops;
	bus->host = host;
	bus->dma = dma;
	bus->next = root_buses;
	root_buses = bus;
	*result = bus;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci bus create child operation.
 */
int
drv_pci_bus_create_child(
	struct drv_pci_bus *parent,
	struct drv_pci_device *bridge,
	uint8_t number,
	struct drv_pci_bus **result)
{
	struct drv_pci_bus *bus;

	/* Handles the parent availability. */
	if (parent == NULL || bridge == NULL || result == NULL)
		return EINVAL;
	bus = hal_malloc(sizeof(*bus));

	/* Handles the bus availability. */
	if (bus == NULL)
		return ENOMEM;
	memset(bus, 0, sizeof(*bus));
	bus->segment = parent->segment;
	bus->number = number;
	bus->ops = parent->ops;
	bus->host = parent->host;
	bus->dma = parent->dma;
	bus->parent = parent;
	bus->bridge = bridge;
	bridge->subordinate = bus;
	*result = bus;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci bus destroy operation.
 */
int
drv_pci_bus_destroy(
	struct drv_pci_bus *bus)
{
	struct drv_pci_device *device, *next;
	struct drv_pci_bus **link;

	/* Handles the bus availability. */
	if (bus == NULL || bus->devices != NULL)
		return EBUSY;
	/* Process each linked entry. */
	for (device = bus->devices; device != NULL; device = next) {
		next = device->next;
		hal_free(device);
	}

	/* Handles the parent availability. */
	if (bus->parent == NULL) {
		/* Process each linked entry. */
		for (link = &root_buses; *link != NULL; link = &(*link)->next) {
			/* Handles the link condition. */
			if (*link == bus) {
				*link = bus->next;
				break;
			}
		}
	}

	/* Handles the bridge availability. */
	if (bus->bridge != NULL)
		bus->bridge->subordinate = NULL;
	hal_free(bus);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci bus scan operation.
 */
int
drv_pci_bus_scan(
	struct drv_pci_bus *bus)
{
	uint8_t secondary;
	struct drv_pci_device *device;
	uint32_t value;
	unsigned functions;
	unsigned slot, function;

	/* Handles the bus availability. */
	if (bus == NULL)
		return EINVAL;
	/* Process each element required by the operation. */
	for (slot = 0; slot < 32U; slot++) {
		functions = 1;
		/* Process each element required by the operation. */
		for (function = 0; function < functions; function++) {
			struct drv_pci_address address = {
				bus->segment, bus->number, (uint8_t)slot,
				(uint8_t)function};

			/* Checks the cfg read result. */
			if (cfg_read(bus, &address, 0, 4, &value) != 0 ||
			    (value & 0xffffU) == 0xffffU)
				continue;
			device = find_device_on_bus(bus, &address);

			/* Handles the device availability. */
			if (device != NULL) {
				/* Handles the function condition. */
				if (function == 0 &&
				    (device->header_type & 0x80U) != 0)
					functions = 8;
				continue;
			}
			device = hal_malloc(sizeof(*device));

			/* Handles the device availability. */
			if (device == NULL)
				return ENOMEM;
			memset(device, 0, sizeof(*device));
			device->address = address;
			device->bus = bus;

			/* Checks the read device result. */
			if (read_device(device) != 0) {
				hal_free(device);
				continue;
			}
			device->next = bus->devices;
			bus->devices = device;

			/* Handles the function condition. */
			if (function == 0 && (device->header_type & 0x80U) != 0)
				functions = 8;

			/* Handles the device condition. */
			if ((device->header_type & 0x7fU) == 1U) {
				secondary = 0;

				/* Checks the drv pci device config read8 result. */
				if (drv_pci_device_config_read8(
					    device, 0x19U, &secondary) == 0 &&
				    secondary != 0 &&
				    secondary != bus->number) {
					(void)drv_pci_bus_create_child(
						bus, device, secondary,
						&device->subordinate);
				}
			}
			(void)drv_pci_device_probe(device);
		}
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci bus rescan operation.
 */
int
drv_pci_bus_rescan(
	struct drv_pci_bus *bus)
{
	int function_result;

	/* Obtains the drv pci bus scan result. */
	function_result = drv_pci_bus_scan(bus);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci bus scan tree operation.
 */
int
drv_pci_bus_scan_tree(
	struct drv_pci_bus *bus)
{
	struct drv_pci_device *d;
	int e;

	/* Checks the drv pci bus scan result. */
	if ((e = drv_pci_bus_scan(bus)) != 0)
		return e;
	/* Process each linked entry. */
	for (d = bus->devices; d; d = d->next) {
		/* Checks the drv pci bus scan tree result. */
		if (d->subordinate &&
		    (e = drv_pci_bus_scan_tree(d->subordinate)) != 0)

			/* Returns the computed result. */
			return e;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci scan all operation.
 */
int
drv_pci_scan_all(
	void)
{
	struct drv_pci_bus *b;
	int e;

	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Checks the drv pci bus scan tree result. */
		if ((e = drv_pci_bus_scan_tree(b)) != 0)
			return e;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci foreach bus operation.
 */
int
drv_pci_foreach_bus(
	drv_pci_bus_iterator_t fn,
	void *arg)
{
	struct drv_pci_bus *b;
	int e;

	/* Handles the fn condition. */
	if (!fn)
		return EINVAL;
	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Checks the fn result. */
		if ((e = fn(b, arg)) != 0)
			return e;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci foreach device operation.
 */
int
drv_pci_foreach_device(
	drv_pci_device_iterator_t fn,
	void *arg)
{
	struct drv_pci_bus *b;
	int e;

	/* Handles the fn condition. */
	if (!fn)
		return EINVAL;
	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Checks the foreach device tree result. */
		if ((e = foreach_device_tree(b, fn, arg)) != 0)
			return e;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci bus segment operation.
 */
uint16_t
drv_pci_bus_segment(
	const struct drv_pci_bus *b)
{
	/* Returns the computed result. */
	return b ? b->segment : 0;
}

/*
 * Implements the drv pci bus number operation.
 */
uint8_t
drv_pci_bus_number(
	const struct drv_pci_bus *b)
{
	/* Returns the computed result. */
	return b ? b->number : 0;
}

/*
 * Implements the drv pci bus parent operation.
 */
struct drv_pci_bus *
drv_pci_bus_parent(
	const struct drv_pci_bus *b)
{
	/* Returns the computed result. */
	return b ? b->parent : NULL;
}

/*
 * Implements the drv pci bus bridge operation.
 */
struct drv_pci_device *
drv_pci_bus_bridge(
	const struct drv_pci_bus *b)
{
	/* Returns the computed result. */
	return b ? b->bridge : NULL;
}

/*
 * Implements the drv pci bus foreach device operation.
 */
int
drv_pci_bus_foreach_device(
	struct drv_pci_bus *b,
	drv_pci_device_iterator_t fn,
	void *arg)
{
	struct drv_pci_device *d;
	int e;

	/* Handles the b condition. */
	if (!b || !fn)
		return EINVAL;
	/* Process each linked entry. */
	for (d = b->devices; d; d = d->next) {
		/* Checks the fn result. */
		if ((e = fn(d, arg)) != 0)
			return e;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci find device operation.
 */
struct drv_pci_device *
drv_pci_find_device(
	const struct drv_pci_address *a)
{
	struct drv_pci_bus *b;
	struct drv_pci_device *d;

	/* Handles the a condition. */
	if (!a)
		return NULL;
	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Process each linked entry. */
		for (d = b->devices; d; d = d->next) {
			/* Handles the memcmp condition. */
			if (memcmp(&d->address, a, sizeof(*a)) == 0)
				return d;
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the drv pci find id operation.
 */
struct drv_pci_device *
drv_pci_find_id(
	uint16_t v,
	uint16_t p,
	struct drv_pci_device *after)
{
	struct drv_pci_bus *b;
	struct drv_pci_device *d;
	int found = after == NULL;

	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Process each linked entry. */
		for (d = b->devices; d; d = d->next) {
			/* Handles the found condition. */
			if (!found) {
				/* Checks the current descriptor. */
				if (d == after)
					found = 1;
				continue;
			}

			/* Checks the current descriptor. */
			if (d->vendor == v && d->product == p)
				return d;
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the drv pci find class operation.
 */
struct drv_pci_device *
drv_pci_find_class(
	uint32_t c,
	uint32_t m,
	struct drv_pci_device *after)
{
	struct drv_pci_bus *b;
	struct drv_pci_device *d;
	int found = after == NULL;

	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Process each linked entry. */
		for (d = b->devices; d; d = d->next) {
			/* Handles the found condition. */
			if (!found) {
				/* Checks the current descriptor. */
				if (d == after)
					found = 1;
				continue;
			}

			/* Checks the current descriptor. */
			if ((d->class_code & m) == (c & m))
				return d;
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the drv pci device bus operation.
 */
struct drv_pci_bus *
drv_pci_device_bus(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->bus : NULL;
}

/*
 * Implements the drv pci device subordinate bus operation.
 */
struct drv_pci_bus *
drv_pci_device_subordinate_bus(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->subordinate : NULL;
}

/*
 * Implements the drv pci device address operation.
 */
void
drv_pci_device_address(
	const struct drv_pci_device *d,
	struct drv_pci_address *a)
{
	/* Checks the current descriptor. */
	if (d && a)
		*a = d->address;
}

/*
 * Implements the drv pci device vendor operation.
 */
uint16_t
drv_pci_device_vendor(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->vendor : DRV_PCI_ANY_ID;
}

/*
 * Implements the drv pci device product operation.
 */
uint16_t
drv_pci_device_product(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->product : DRV_PCI_ANY_ID;
}

/*
 * Implements the drv pci device subvendor operation.
 */
uint16_t
drv_pci_device_subvendor(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->subvendor : DRV_PCI_ANY_ID;
}

/*
 * Implements the drv pci device subproduct operation.
 */
uint16_t
drv_pci_device_subproduct(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->subproduct : DRV_PCI_ANY_ID;
}

/*
 * Implements the drv pci device class operation.
 */
uint32_t
drv_pci_device_class(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->class_code : 0;
}

/*
 * Implements the drv pci device revision operation.
 */
uint8_t
drv_pci_device_revision(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->revision : 0;
}

/*
 * Implements the drv pci device header type operation.
 */
uint8_t
drv_pci_device_header_type(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->header_type : 0xff;
}

/*
 * Implements the drv pci device is bridge operation.
 */
bool
drv_pci_device_is_bridge(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d && ((d->class_code >> 8) == 0x0604U);
}

/*
 * Implements the drv pci device is multifunction operation.
 */
bool
drv_pci_device_is_multifunction(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d && (d->header_type & 0x80U);
}

/*
 * Implements the drv pci device config read8 operation.
 */
int
drv_pci_device_config_read8(
	struct drv_pci_device *d,
	unsigned o,
	uint8_t *v)
{
	uint32_t x;
	int e;

	/* Checks the current descriptor. */
	if (!d || !v)
		return EINVAL;
	e = cfg_read(d->bus, &d->address, o, 1, &x);
	*v = (uint8_t)x;
	/* Returns the computed result. */
	return e;
}

/*
 * Implements the drv pci device config read16 operation.
 */
int
drv_pci_device_config_read16(
	struct drv_pci_device *d,
	unsigned o,
	uint16_t *v)
{
	uint32_t x;
	int e;

	/* Checks the current descriptor. */
	if (!d || !v)
		return EINVAL;
	e = cfg_read(d->bus, &d->address, o, 2, &x);
	*v = (uint16_t)x;
	/* Returns the computed result. */
	return e;
}

/*
 * Implements the drv pci device config read32 operation.
 */
int
drv_pci_device_config_read32(
	struct drv_pci_device *d,
	unsigned o,
	uint32_t *v)
{
	int function_result;

	/* Computes the function result. */
	function_result = d ? cfg_read(d->bus, &d->address, o, 4, v) : EINVAL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device config write8 operation.
 */
int
drv_pci_device_config_write8(
	struct drv_pci_device *d,
	unsigned o,
	uint8_t v)
{
	int function_result;

	/* Computes the function result. */
	function_result = d ? cfg_write(d->bus, &d->address, o, 1, v) : EINVAL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device config write16 operation.
 */
int
drv_pci_device_config_write16(
	struct drv_pci_device *d,
	unsigned o,
	uint16_t v)
{
	int function_result;

	/* Computes the function result. */
	function_result = d ? cfg_write(d->bus, &d->address, o, 2, v) : EINVAL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device config write32 operation.
 */
int
drv_pci_device_config_write32(
	struct drv_pci_device *d,
	unsigned o,
	uint32_t v)
{
	int function_result;

	/* Computes the function result. */
	function_result = d ? cfg_write(d->bus, &d->address, o, 4, v) : EINVAL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device find capability operation.
 */
int
drv_pci_device_find_capability(
	struct drv_pci_device *d,
	uint8_t id,
	unsigned *result)
{
	uint16_t status;
	uint8_t p, n;
	unsigned guard = 0;

	/* Checks the current descriptor. */
	if (!d || !result)
		return EINVAL;

	/* Checks the drv pci device config read16 result. */
	if (drv_pci_device_config_read16(d, PCI_STATUS, &status) ||
	    !(status & 0x10U))

		/* Returns the computed result. */
		return ENOENT;

	/* Handles the drv pci device config read8 condition. */
	if (drv_pci_device_config_read8(d, PCI_CAPABILITIES, &p))
		return EIO;
	p &= ~3U;
	/* Continue while the operation condition remains true. */
	while (p >= 0x40U && guard++ < 48U) {
		/* Handles the drv pci device config read8 condition. */
		if (drv_pci_device_config_read8(d, p, &n))
			return EIO;

		/* Checks the current item count. */
		if (n == id) {
			*result = p;
			/* Reports successful completion. */
			return 0;
		}

		/* Handles the drv pci device config read8 condition. */
		if (drv_pci_device_config_read8(d, p + 1U, &p))
			return EIO;
		p &= ~3U;
	}

	/* Returns the computed result. */
	return ENOENT;
}

/*
 * Implements the drv pci device find extended capability operation.
 */
int
drv_pci_device_find_extended_capability(
	struct drv_pci_device *d,
	uint16_t id,
	unsigned start,
	unsigned *result)
{
	unsigned p = start ? start : 0x100U, guard = 0, limit;
	uint32_t h;

	/* Checks the current descriptor. */
	if (!d || !result || p < 0x100U || (p & 3U))
		return EINVAL;
	limit = d->bus->ops->config_space_size ? d->bus->ops->config_space_size
					       : 256U;

	/* Handles the limit condition. */
	if (limit < 4096U)
		return ENOTSUP;
	/* Continue while the operation condition remains true. */
	while (p >= 0x100U && p + 4U <= limit && guard++ < 960U) {
		/* Checks the drv pci device config read32 result. */
		if (drv_pci_device_config_read32(d, p, &h) != 0)
			return EIO;

		/* Handles the h condition. */
		if (h == 0 || h == 0xffffffffU)
			return ENOENT;

		/* Handles the h condition. */
		if ((h & 0xffffU) == id) {
			*result = p;
			/* Reports successful completion. */
			return 0;
		}

		/* Handles the h condition. */
		if (((h >> 20) & 0xfffU) == 0)
			return ENOENT;

		/* Handles the h condition. */
		if (((h >> 20) & 0xfffU) <= p || (((h >> 20) & 0xfffU) & 3U))
			return EIO;
		p = (h >> 20) & 0xfffU;
	}

	/* Returns the computed result. */
	return EIO;
}

/*
 * Implements the drv pci device enable operation.
 */
int
drv_pci_device_enable(
	struct drv_pci_device *d)
{
	int e;

	/* Checks the current descriptor. */
	if (!d)
		return EINVAL;

	/* Checks the current descriptor. */
	if (d->enable_count++ != 0)
		return 0;
	e = command_set(d, PCI_COMMAND_IO | PCI_COMMAND_MEMORY, 0);

	/* Handles the e condition. */
	if (e)
		d->enable_count--;

	/* Returns the computed result. */
	return e;
}

/*
 * Implements the drv pci device disable operation.
 */
void
drv_pci_device_disable(
	struct drv_pci_device *d)
{
	/* Checks the current descriptor. */
	if (d && d->enable_count && --d->enable_count == 0) {
		(void)command_set(d, 0,
				  PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
					  PCI_COMMAND_MASTER);
	}
}

/*
 * Implements the drv pci device enable io operation.
 */
int
drv_pci_device_enable_io(
	struct drv_pci_device *d)
{
	int function_result;

	/* Obtains the command set result. */
	function_result = command_set(d, PCI_COMMAND_IO, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device enable memory operation.
 */
int
drv_pci_device_enable_memory(
	struct drv_pci_device *d)
{
	int function_result;

	/* Obtains the command set result. */
	function_result = command_set(d, PCI_COMMAND_MEMORY, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device save enable state operation.
 */
int
drv_pci_device_save_enable_state(
	struct drv_pci_device *d,
	struct drv_pci_enable_state *state)
{
	uint16_t command;
	int error;

	/* Handles the d availability. */
	if (d == NULL || state == NULL)
		return EINVAL;

	/* Handles the state condition. */
	if (state->private_data[1] != 0)
		return EBUSY;
	error = drv_pci_device_config_read16(d, PCI_COMMAND, &command);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	state->private_data[0] = command & PCI_COMMAND_ENABLE_MASK;
	state->private_data[1] = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci device restore enable state operation.
 */
int
drv_pci_device_restore_enable_state(
	struct drv_pci_device *d,
	struct drv_pci_enable_state *state)
{
	uint16_t command, readback, restored;
	int error;

	/* Handles the d availability. */
	if (d == NULL || state == NULL)
		return EINVAL;

	/* Handles the state condition. */
	if (state->private_data[1] == 0)
		return 0;
	error = drv_pci_device_config_read16(d, PCI_COMMAND, &command);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	restored = (uint16_t)((command & ~PCI_COMMAND_ENABLE_MASK) |
			      (uint16_t)state->private_data[0]);

	/* Handles the restored condition. */
	if (restored != command) {
		error = drv_pci_device_config_write16(d, PCI_COMMAND, restored);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}
	error = drv_pci_device_config_read16(d, PCI_COMMAND, &readback);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the readback condition. */
	if ((readback & PCI_COMMAND_ENABLE_MASK) !=
	    (restored & PCI_COMMAND_ENABLE_MASK))

		/* Returns the computed result. */
		return EIO;
	state->private_data[0] = 0;
	state->private_data[1] = 0;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci device set bus master operation.
 */
int
drv_pci_device_set_bus_master(
	struct drv_pci_device *d,
	bool on)
{
	int function_result;

	/* Obtains the command set result. */
	function_result = command_set(d, on ? PCI_COMMAND_MASTER : 0,
				      on ? 0 : PCI_COMMAND_MASTER);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device bar count operation.
 */
unsigned
drv_pci_device_bar_count(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->bar_count : 0;
}

/*
 * Implements the drv pci device bar operation.
 */
int
drv_pci_device_bar(
	const struct drv_pci_device *d,
	unsigned i,
	struct drv_pci_bar *b)
{
	/* Checks the current descriptor. */
	if (!d || !b || i >= d->bar_count)
		return EINVAL;
	*b = d->bars[i];
	/* Returns the computed result. */
	return b->type == DRV_PCI_BAR_NONE ? ENOENT : 0;
}

/*
 * Implements the drv pci device assign bar operation.
 */
int
drv_pci_device_assign_bar(
	struct drv_pci_device *device,
	unsigned index,
	uint64_t address)
{
	struct drv_pci_bar *bar;
	uint32_t original_low, original_high, low, high, read_low, read_high;
	uint16_t original_command, read_command;
	int error, quiesce_error;

	/* Handles the device availability. */
	if (device == NULL || index >= device->bar_count)
		return EINVAL;
	bar = &device->bars[index];

	/* Handles the bar condition. */
	if (bar->type == DRV_PCI_BAR_NONE || bar->size == 0 ||
	    (bar->size & (bar->size - 1U)) != 0 ||
	    (address & (bar->size - 1U)) != 0 ||
	    address > UINT64_MAX - (bar->size - 1U) ||
	    (bar->type != DRV_PCI_BAR_MEMORY64 &&
	     (address > UINT32_MAX || bar->size - 1U > UINT32_MAX - address)) ||
	    (bar->type == DRV_PCI_BAR_MEMORY64 &&
	     index + 1U >= device->bar_count))

		/* Returns the computed result. */
		return EINVAL;
	error = drv_pci_device_config_read16(device, PCI_COMMAND,
					     &original_command);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = pci_bar_read_raw(device, index, bar->type, &original_low,
				 &original_high);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = pci_command_quiesce(device, original_command);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Encodes the address with the flags the region kind needs. */
	low = (uint32_t)address |
	      (bar->type == DRV_PCI_BAR_IO ? 1U
					   : (bar->prefetchable ? 8U : 0U)) |
	      (bar->type == DRV_PCI_BAR_MEMORY64 ? 4U : 0U);
	high = (uint32_t)(address >> 32);
	error = pci_bar_write_raw(device, index, bar->type, low, high);

	/* Checks the operation status. */
	if (error == 0) {
		error = pci_bar_read_raw(device, index, bar->type, &read_low,
					 &read_high);
	}

	/* Checks the operation status. */
	if (error == 0 &&
	    (read_low != low ||
	     (bar->type == DRV_PCI_BAR_MEMORY64 && read_high != high)))
		error = EIO;

	/* Checks the operation status. */
	if (error != 0)
		goto rollback;

	error = drv_pci_device_config_write16(device, PCI_COMMAND,
					      original_command);

	/* Checks the operation status. */
	if (error == 0) {
		error = drv_pci_device_config_read16(device, PCI_COMMAND,
						     &read_command);
	}

	/* Checks the operation status. */
	if (error == 0 && read_command != original_command)
		error = EIO;

	/* Checks the operation status. */
	if (error != 0) {
		/*
 * Never roll a BAR back while the failed restore may have
		 * enabled decode or DMA. */
		quiesce_error = pci_command_quiesce(device, original_command);

		/* Checks the operation status. */
		if (quiesce_error != 0) {
			bar->bus_address = address;

			/* Returns the computed result. */
			return error;
		}
		goto rollback;
	}
	bar->bus_address = address;

	/* Reports successful completion. */
	return 0;

rollback:

	/*
 * A failed transaction deliberately leaves decode and bus mastering
	 * off. The caller may retry or detach without exposing a partial BAR.
	 */
	(void)pci_bar_write_raw(device, index, bar->type, original_low,
				original_high);
	pci_bar_cache_readback(device, index, bar);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv pci device claim bar operation.
 */
int
drv_pci_device_claim_bar(
	struct drv_pci_device *d,
	unsigned i)
{
	/* Checks the current descriptor. */
	if (!d || i >= d->bar_count)
		return EINVAL;

	/* Checks the current descriptor. */
	if (d->bar_claimed[i])
		return EBUSY;
	d->bar_claimed[i] = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci device release bar operation.
 */
void
drv_pci_device_release_bar(
	struct drv_pci_device *d,
	unsigned i)
{
	/* Checks the current descriptor. */
	if (d && i < d->bar_count)
		d->bar_claimed[i] = 0;
}

/*
 * Implements the drv pci device map bar region operation.
 */
int
drv_pci_device_map_bar_region(
	struct drv_pci_device *d,
	unsigned i,
	uint64_t o,
	size_t s,
	unsigned f,
	struct drv_pci_mapping *m)
{
	int function_result;
	struct drv_pci_bar b;

	/* Checks the current descriptor. */
	if (!d || !m || i >= d->bar_count || !d->bar_claimed[i] ||
	    !d->bus->ops->map_bar || s == 0)

		/* Returns the computed result. */
		return EINVAL;
	b = d->bars[i];

	/* Handles the o condition. */
	if (o > b.size || s > b.size - o)
		return EINVAL;
	b.bus_address += o;
	b.size = s;

	/* Computes the function result. */
	function_result = d->bus->ops->map_bar(d->bus->host, d, &b, f, m);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device map bar operation.
 */
int
drv_pci_device_map_bar(
	struct drv_pci_device *d,
	unsigned i,
	unsigned f,
	struct drv_pci_mapping *m)
{
	int function_result;

	/* Checks the current descriptor. */
	if (!d || i >= d->bar_count)
		return EINVAL;

	/* Obtains the drv pci device map bar region result. */
	function_result = drv_pci_device_map_bar_region(
		d, i, 0, (size_t)d->bars[i].size, f, m);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci device unmap bar operation.
 */
void
drv_pci_device_unmap_bar(
	struct drv_pci_device *d,
	struct drv_pci_mapping *m)
{
	/* Checks the current descriptor. */
	if (d && m && d->bus->ops->unmap_bar)
		d->bus->ops->unmap_bar(d->bus->host, m);
}

/*
 * Implements the drv pci device allocate irqs operation.
 */
int
drv_pci_device_allocate_irqs(
	struct drv_pci_device *device,
	unsigned flags,
	unsigned minimum,
	unsigned maximum,
	struct drv_pci_irq *irqs,
	unsigned *count)
{
	static const struct {
		unsigned flag;
		enum drv_pci_irq_type type;
	} choices[] = {{DRV_PCI_IRQ_ALLOW_MSIX, DRV_PCI_IRQ_MSIX},
		       {DRV_PCI_IRQ_ALLOW_MSI, DRV_PCI_IRQ_MSI},
		       {DRV_PCI_IRQ_ALLOW_INTX, DRV_PCI_IRQ_INTX}};
	unsigned choice;
	int error = ENOTSUP;

	/* Handles the device availability. */
	if (device == NULL || irqs == NULL || count == NULL || minimum == 0 ||
	    maximum < minimum || device->bus->ops->allocate_irqs == NULL)

		/* Returns the computed result. */
		return EINVAL;
	/* Process each remaining element. */
	for (choice = 0; choice < sizeof(choices) / sizeof(choices[0]);
	     choice++) {
		/* Checks the active flags. */
		if ((flags & choices[choice].flag) == 0)
			continue;
		error = device->bus->ops->allocate_irqs(
			device->bus->host, device, choices[choice].type,
			minimum, maximum, irqs, count);

		/* Checks the operation status. */
		if (error == 0)
			return 0;

		/* Checks the operation status. */
		if (error != ENOTSUP && error != ENODEV)
			return error;
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv pci device free irqs operation.
 */
void
drv_pci_device_free_irqs(
	struct drv_pci_device *device,
	struct drv_pci_irq *irqs,
	unsigned count)
{
	/* Handles the device availability. */
	if (device != NULL && device->bus->ops->free_irqs != NULL) {
		device->bus->ops->free_irqs(device->bus->host, device, irqs,
					    count);
	}
}

/*
 * Implements the drv pci device establish irq operation.
 */
int
drv_pci_device_establish_irq(
	struct drv_pci_device *device,
	const struct drv_pci_irq *irq,
	drv_pci_irq_handler_t handler,
	void *argument,
	const char *name,
	void **result)
{
	struct pci_irq_cookie *cookie;
	int error;

	(void)name;

	/* Handles the device availability. */
	if (device == NULL || irq == NULL || handler == NULL || result == NULL)
		return EINVAL;
	cookie = hal_malloc(sizeof(*cookie));

	/* Handles the cookie availability. */
	if (cookie == NULL)
		return ENOMEM;
	memset(cookie, 0, sizeof(*cookie));
	cookie->device = device;
	cookie->type = irq->type;
	cookie->capability = (unsigned)irq->private_data[0];
	cookie->index = irq->index;
	cookie->handler = handler;
	cookie->argument = argument;

	/* Handles the irq condition. */
	if (irq->type == DRV_PCI_IRQ_INTX) {
		error = establish_intx(cookie, irq);
	} else if (irq->type == DRV_PCI_IRQ_MSI) {
		error = establish_msi(cookie, irq);
	} else if (irq->type == DRV_PCI_IRQ_MSIX) {
		error = establish_msix(cookie);
	} else {
		error = EINVAL;
	}

	/* Checks the operation status. */
	if (error != 0) {
		hal_free(cookie);

		/* Returns the computed result. */
		return error;
	}
	*result = cookie;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci device disestablish irq checked operation.
 */
int
drv_pci_device_disestablish_irq_checked(
	struct drv_pci_device *device,
	void *value)
{
	int function_result;
	volatile uint32_t *entry_local;
	volatile uint32_t *entry_local1;
	struct pci_irq_cookie *cookie = value;
	uint16_t control;
	int hal_error;

	(void)device;

	/* Handles the cookie availability. */
	if (cookie == NULL)
		return EINVAL;

	/* Handles the cookie condition. */
	if (cookie->type == DRV_PCI_IRQ_INTX) {
		/* Obtains the disestablish intx result. */
		function_result = disestablish_intx(cookie);

		/* Returns the computed result. */
		return function_result;
	} else if (cookie->type == DRV_PCI_IRQ_MSI) {
		/* Handles the cookie condition. */
		if (cookie->message_registered) {
			/* Checks the drv pci device config read16 result. */
			if (drv_pci_device_config_read16(
				    cookie->device,
				    cookie->capability + PCI_MSI_CONTROL,
				    &control) != 0 ||
			    drv_pci_device_config_write16(
				    cookie->device,
				    cookie->capability + PCI_MSI_CONTROL,
				    control & (uint16_t)~PCI_MSI_ENABLE) != 0)

				/* Returns the computed result. */
				return EIO;
			hal_error = hal_irq_unregister_msi(cookie->irq);

			/* Checks the operation status. */
			if (hal_error != HAL_OK)
				return hal_error == HAL_ERR_BUSY ? EBUSY : EIO;
			cookie->message_registered = 0;
		}

		/* Checks the drv pci device config write32 result. */
		if (!cookie->msi_state_saved ||
		    drv_pci_device_config_write32(
			    cookie->device,
			    cookie->capability + PCI_MSI_ADDRESS,
			    cookie->msi_address_low_saved) != 0 ||
		    ((cookie->msi_control_saved & PCI_MSI_64BIT) != 0 &&
		     drv_pci_device_config_write32(
			     cookie->device,
			     cookie->capability + PCI_MSI_ADDRESS + 4U,
			     cookie->msi_address_high_saved) != 0) ||
		    drv_pci_device_config_write16(
			    cookie->device,
			    (cookie->msi_control_saved & PCI_MSI_64BIT) != 0
				    ? cookie->capability + 12U
				    : cookie->capability + 8U,
			    cookie->msi_data_saved) != 0 ||
		    drv_pci_device_config_write16(
			    cookie->device,
			    cookie->capability + PCI_MSI_CONTROL,
			    cookie->msi_control_saved) != 0)

			/* Returns the computed result. */
			return EIO;
		cookie->msi_state_saved = 0;
	} else if (cookie->type == DRV_PCI_IRQ_MSIX) {
		/* Handles the cookie condition. */
		if (cookie->message_registered && cookie->table_mapped) {
			entry_local = cookie->table.address;
			entry_local[3] |= PCI_MSIX_ENTRY_MASK;
			hal_io_mb();
		}

		/* Handles the cookie condition. */
		if (cookie->message_registered) {
			hal_error = hal_irq_unregister_msi(cookie->irq);

			/* Checks the operation status. */
			if (hal_error != HAL_OK)
				return hal_error == HAL_ERR_BUSY ? EBUSY : EIO;
			cookie->message_registered = 0;
		}

		/* Handles the cookie condition. */
		if (!cookie->msix_state_saved || !cookie->table_mapped)
			return EIO;
		entry_local1 = cookie->table.address;

		/* Restores the saved table entry with the vector masked. */
		entry_local1[0] = cookie->msix_entry_saved[0];
		entry_local1[1] = cookie->msix_entry_saved[1];
		entry_local1[2] = cookie->msix_entry_saved[2];
		entry_local1[3] =
			cookie->msix_entry_saved[3] | PCI_MSIX_ENTRY_MASK;
		hal_io_mb();

		/* Checks the drv pci device config write16 result. */
		if (drv_pci_device_config_write16(
			    cookie->device,
			    cookie->capability + PCI_MSIX_CONTROL,
			    cookie->msix_control_saved) != 0)

			/* Returns the computed result. */
			return EIO;
		entry_local1[3] = cookie->msix_entry_saved[3];
		hal_io_mb();
		cookie->msix_state_saved = 0;

		/* Handles the cookie condition. */
		if (cookie->table_mapped) {
			cookie->device->bus->ops->unmap_bar(
				cookie->device->bus->host, &cookie->table);
		}
	} else {
		/* Returns the computed result. */
		return EINVAL;
	}
	hal_free(cookie);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci device disestablish irq operation.
 */
void
drv_pci_device_disestablish_irq(
	struct drv_pci_device *device,
	void *value)
{
	/*
 * Legacy callers cannot report a busy interrupt teardown.  Drivers
	 * which must free their handler argument immediately use the checked
	 * API and retain their complete device state until a successful retry.
	 */
	(void)drv_pci_device_disestablish_irq_checked(device, value);
}

/*
 * Implements the drv pci device dma operation.
 */
struct drv_dma_device *
drv_pci_device_dma(
	struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->bus->dma : NULL;
}

/*
 * Implements the drv pci device driver operation.
 */
struct drv_pci_driver *
drv_pci_device_driver(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->driver : NULL;
}

/*
 * Implements the drv pci device driver data operation.
 */
void *
drv_pci_device_driver_data(
	const struct drv_pci_device *d)
{
	/* Returns the computed result. */
	return d ? d->driver_data : NULL;
}

/*
 * Implements the drv pci device set driver data operation.
 */
int
drv_pci_device_set_driver_data(
	struct drv_pci_device *d,
	void *p)
{
	/* Checks the current descriptor. */
	if (!d)
		return EINVAL;
	d->driver_data = p;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci id match operation.
 */
int
drv_pci_id_match(
	const struct drv_pci_id *i,
	const struct drv_pci_device *d)
{
	/* Checks the current index. */
	if (!i || !d)
		return 0;

	/* Returns the computed result. */
	return (i->vendor == DRV_PCI_ANY_ID || i->vendor == d->vendor) &&
	       (i->device == DRV_PCI_ANY_ID || i->device == d->product) &&
	       (i->subvendor == DRV_PCI_ANY_ID ||
		i->subvendor == d->subvendor) &&
	       (i->subdevice == DRV_PCI_ANY_ID ||
		i->subdevice == d->subproduct) &&
	       ((d->class_code & i->class_mask) ==
		(i->class_code & i->class_mask));
}

/*
 * Implements the drv pci driver find id operation.
 */
const struct drv_pci_id *
drv_pci_driver_find_id(
	const struct drv_pci_driver *r,
	const struct drv_pci_device *d)
{
	size_t n;

	/* Handles the r condition. */
	if (!r)
		return NULL;
	/* Process each remaining element. */
	for (n = 0; n < r->id_count; n++) {
		/* Checks the drv pci id match result. */
		if (drv_pci_id_match(&r->ids[n], d))
			return &r->ids[n];
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Implements the drv pci driver match operation.
 */
int
drv_pci_driver_match(
	struct drv_pci_driver *r,
	struct drv_pci_device *d,
	const struct drv_pci_id **out)
{
	const struct drv_pci_id *i = drv_pci_driver_find_id(r, d);
	int score;

	/* Checks the current index. */
	if (!i)
		return DRV_PCI_MATCH_NONE;
	score = r->match ? r->match(d, i) : DRV_PCI_MATCH_GENERIC;

	/* Handles the score condition. */
	if (score > 0 && out)
		*out = i;
	/* Returns the computed result. */
	return score;
}

/*
 * Implements the drv pci device probe operation.
 */
int
drv_pci_device_probe(
	struct drv_pci_device *d)
{
	struct pci_driver_entry *e, *best = NULL;
	const struct drv_pci_id *i, *best_id = NULL;
	int score, best_score = 0, error;

	/* Checks the current descriptor. */
	if (!d || d->driver)
		return d ? EBUSY : EINVAL;
	/* Process each linked entry. */
	for (e = drivers; e; e = e->next) {
		/* Checks the drv pci driver match result. */
		if ((score = drv_pci_driver_match(e->driver, d, &i)) >
		    best_score) {
			best = e;
			best_id = i;
			best_score = score;
		}
	}

	/* Handles the best condition. */
	if (!best)
		return ENODEV;
	error = best->driver->attach ? best->driver->attach(d, best_id) : 0;

	/* Checks the operation status. */
	if (error)
		return error;
	d->driver = best->driver;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci device detach operation.
 */
int
drv_pci_device_detach(
	struct drv_pci_device *d,
	unsigned f)
{
	int e = 0;

	/* Checks the current descriptor. */
	if (!d || !d->driver)
		return EINVAL;

	/* Checks the current descriptor. */
	if (d->driver->detach)
		e = d->driver->detach(d, f);

	/* Handles the e condition. */
	if (!e) {
		d->driver = NULL;
		d->driver_data = NULL;
	}

	/* Returns the computed result. */
	return e;
}

/*
 * Implements the drv pci device reprobe operation.
 */
int
drv_pci_device_reprobe(
	struct drv_pci_device *d)
{
	int function_result;
	int e;

	/* Checks the current descriptor. */
	if (!d)
		return EINVAL;

	/* Checks the current descriptor. */
	if (d->driver) {
		e = drv_pci_device_detach(d, 0);

		/* Handles the e condition. */
		if (e)
			return e;
	}

	/* Obtains the drv pci device probe result. */
	function_result = drv_pci_device_probe(d);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pci driver register operation.
 */
int
drv_pci_driver_register(
	struct drv_pci_driver *driver)
{
	struct pci_driver_entry *entry, **tail;
	struct drv_pci_bus *bus;
	struct drv_pci_device *device;

	/* Handles the driver availability. */
	if (!initialized || driver == NULL || driver->name == NULL)
		return EINVAL;
	/* Process each linked entry. */
	for (entry = drivers; entry != NULL; entry = entry->next) {
		/* Handles the entry condition. */
		if (entry->driver == driver)
			return EEXIST;
	}
	entry = hal_malloc(sizeof(*entry));

	/* Handles the entry availability. */
	if (entry == NULL)
		return ENOMEM;
	entry->driver = driver;
	entry->next = NULL;
	/* Process each linked entry. */
	for (tail = &drivers; *tail != NULL; tail = &(*tail)->next)
		continue;
	*tail = entry;
	/* Process each linked entry. */
	for (bus = root_buses; bus != NULL; bus = bus->next) {
		/* Process each linked entry. */
		for (device = bus->devices; device != NULL;
		     device = device->next) {
			/* Handles the driver availability. */
			if (device->driver == NULL)
				(void)drv_pci_device_probe(device);
		}
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci driver unregister operation.
 */
int
drv_pci_driver_unregister(
	struct drv_pci_driver *r)
{
	struct pci_driver_entry **p, *e;
	struct drv_pci_bus *b;
	struct drv_pci_device *d;

	/* Handles the r condition. */
	if (!r)
		return EINVAL;
	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Process each linked entry. */
		for (d = b->devices; d; d = d->next) {
			/* Checks the drv pci device detach result. */
			if (d->driver == r && drv_pci_device_detach(d, 0))
				return EBUSY;
		}
	}
	/* Process each linked entry. */
	for (p = &drivers; (e = *p) != NULL; p = &e->next) {
		/* Handles the e condition. */
		if (e->driver == r) {
			*p = e->next;
			hal_free(e);

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Returns the computed result. */
	return ENOENT;
}

/*
 * Implements the drv pci driver name operation.
 */
const char *
drv_pci_driver_name(
	const struct drv_pci_driver *r)
{
	/* Returns the computed result. */
	return r ? r->name : NULL;
}

/*
 * Implements the drv pci driver device count operation.
 */
size_t
drv_pci_driver_device_count(
	const struct drv_pci_driver *r)
{
	size_t n = 0;
	struct drv_pci_bus *b;
	struct drv_pci_device *d;

	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Process each linked entry. */
		for (d = b->devices; d; d = d->next) {
			/* Checks the current descriptor. */
			if (d->driver == r)
				n++;
		}
	}

	/* Returns the computed result. */
	return n;
}

/*
 * Implements the drv pci driver foreach device operation.
 */
int
drv_pci_driver_foreach_device(
	struct drv_pci_driver *r,
	drv_pci_device_iterator_t fn,
	void *a)
{
	struct drv_pci_bus *b;
	struct drv_pci_device *d;
	int e;

	/* Handles the r condition. */
	if (!r || !fn)
		return EINVAL;
	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Process each linked entry. */
		for (d = b->devices; d; d = d->next) {
			/* Checks the fn result. */
			if (d->driver == r && (e = fn(d, a)) != 0)
				return e;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pci dump operation.
 */
void
drv_pci_dump(
	void)
{
	struct drv_pci_bus *b;
	struct drv_pci_device *d;

	/* Process each linked entry. */
	for (b = root_buses; b; b = b->next) {
		/* Process each linked entry. */
		for (d = b->devices; d; d = d->next) {
			hal_printf("pci: %04x:%02x:%02x.%u %04x:%04x class "
				   "%06x%s%s\n",
				   d->address.segment, d->address.bus,
				   d->address.device, d->address.function,
				   d->vendor, d->product, d->class_code,
				   d->driver ? " driver=" : "",
				   d->driver ? d->driver->name : "");
		}
	}
}

/* Supports the cfg read operation. */
static int
cfg_read(
	struct drv_pci_bus *bus,
	const struct drv_pci_address *a,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	int function_result;
	unsigned limit = bus != NULL && bus->ops != NULL &&
					 bus->ops->config_space_size != 0
				 ? bus->ops->config_space_size
				 : 256U;

	/* Handles the bus availability. */
	if (bus == NULL || bus->ops == NULL || bus->ops->config_read == NULL ||
	    value == NULL || offset > limit || width > limit - offset ||
	    (width != 1 && width != 2 && width != 4))

		/* Returns the computed result. */
		return EINVAL;

	/* Computes the function result. */
	function_result =
		bus->ops->config_read(bus->host, a, offset, width, value);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the find device on bus operation. */
static struct drv_pci_device *
find_device_on_bus(
	struct drv_pci_bus *bus,
	const struct drv_pci_address *address)
{
	struct drv_pci_device *device;

	/* Process each linked entry. */
	for (device = bus->devices; device != NULL; device = device->next) {
		/* Handles the device condition. */
		if (device->address.segment == address->segment &&
		    device->address.bus == address->bus &&
		    device->address.device == address->device &&
		    device->address.function == address->function)

			/* Returns the computed result. */
			return device;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the read device operation. */
static int
read_device(
	struct drv_pci_device *device)
{
	uint32_t high, high_mask;
	uint32_t original, mask;
	struct drv_pci_bar *bar;
	unsigned offset;
	uint32_t value;
	uint32_t command = 0;
	unsigned index, limit;
	int error;

	error = cfg_read(device->bus, &device->address, 0, 4, &value);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	device->vendor = (uint16_t)value;
	device->product = (uint16_t)(value >> 16);
	(void)cfg_read(device->bus, &device->address, PCI_CLASS_REVISION, 4,
		       &value);
	device->revision = (uint8_t)value;
	device->class_code = value >> 8;
	(void)cfg_read(device->bus, &device->address, 0x0cU, 4, &value);
	device->header_type = (uint8_t)(value >> 16);
	limit = (device->header_type & 0x7fU) == 1U ? 2U : 6U;
	device->bar_count = limit;

	/* BAR sizing must never decode the temporary all-ones address. */
	(void)cfg_read(device->bus, &device->address, PCI_COMMAND, 2, &command);
	(void)cfg_write(device->bus, &device->address, PCI_COMMAND, 2,
			command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
	/* Process each remaining element. */
	for (index = 0; index < limit; index++) {
		bar = &device->bars[index];
		offset = PCI_BAR0 + index * 4U;
		memset(bar, 0, sizeof(*bar));
		bar->index = index;

		/* Checks the cfg read result. */
		if (cfg_read(device->bus, &device->address, offset, 4,
			     &original) != 0 ||
		    original == 0xffffffffU)
			continue;
		(void)cfg_write(device->bus, &device->address, offset, 4,
				0xffffffffU);
		(void)cfg_read(device->bus, &device->address, offset, 4, &mask);
		(void)cfg_write(device->bus, &device->address, offset, 4,
				original);

		/* Handles the original condition. */
		if (original & 1U) {
			bar->type = DRV_PCI_BAR_IO;
			bar->bus_address = original & ~3U;
			bar->size =
				mask == 0 ? 0 : (uint32_t)(~(mask & ~3U) + 1U);
		} else {
			bar->prefetchable = (original & 8U) != 0;
			bar->type = ((original >> 1) & 3U) == 2U
					    ? DRV_PCI_BAR_MEMORY64
					    : DRV_PCI_BAR_MEMORY32;
			bar->bus_address = original & ~15U;
			bar->size =
				mask == 0 ? 0 : (uint32_t)(~(mask & ~15U) + 1U);

			/* Handles the bar condition. */
			if (bar->type == DRV_PCI_BAR_MEMORY64 &&
			    index + 1U < limit) {
				(void)cfg_read(device->bus, &device->address,
					       offset + 4U, 4, &high);
				(void)cfg_write(device->bus, &device->address,
						offset + 4U, 4, 0xffffffffU);
				(void)cfg_read(device->bus, &device->address,
					       offset + 4U, 4, &high_mask);
				(void)cfg_write(device->bus, &device->address,
						offset + 4U, 4, high);
				bar->bus_address |= (uint64_t)high << 32;
				bar->size = ~(((uint64_t)high_mask << 32) |
					      (mask & ~15U)) +
					    1U;
				index++;
			}
		}
	}
	(void)cfg_write(device->bus, &device->address, PCI_COMMAND, 2, command);

	/* Handles the device condition. */
	if ((device->header_type & 0x7fU) == 0U) {
		(void)cfg_read(device->bus, &device->address, 0x2cU, 4, &value);
		device->subvendor = (uint16_t)value;
		device->subproduct = (uint16_t)(value >> 16);
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the cfg write operation. */
static int
cfg_write(
	struct drv_pci_bus *bus,
	const struct drv_pci_address *a,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	int function_result;
	unsigned limit = bus != NULL && bus->ops != NULL &&
					 bus->ops->config_space_size != 0
				 ? bus->ops->config_space_size
				 : 256U;

	/* Handles the bus availability. */
	if (bus == NULL || bus->ops == NULL || bus->ops->config_write == NULL ||
	    offset > limit || width > limit - offset ||
	    (width != 1 && width != 2 && width != 4))

		/* Returns the computed result. */
		return EINVAL;

	/* Computes the function result. */
	function_result =
		bus->ops->config_write(bus->host, a, offset, width, value);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the foreach device tree operation. */
static int
foreach_device_tree(
	struct drv_pci_bus *b,
	drv_pci_device_iterator_t fn,
	void *arg)
{
	struct drv_pci_device *d;
	int e;

	/* Process each linked entry. */
	for (d = b->devices; d; d = d->next) {
		/* Checks the fn result. */
		if ((e = fn(d, arg)) != 0)
			return e;

		/* Checks the foreach device tree result. */
		if (d->subordinate &&
		    (e = foreach_device_tree(d->subordinate, fn, arg)) != 0)

			/* Returns the computed result. */
			return e;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the command set operation. */
static int
command_set(
	struct drv_pci_device *d,
	uint16_t set,
	uint16_t clear)
{
	int function_result;
	uint16_t v;
	int e;

	/* Checks the current descriptor. */
	if (!d)
		return EINVAL;
	e = drv_pci_device_config_read16(d, PCI_COMMAND, &v);

	/* Handles the e condition. */
	if (e)
		return e;

	/* Obtains the drv pci device config write16 result. */
	function_result = drv_pci_device_config_write16(
		d, PCI_COMMAND, (uint16_t)((v | set) & ~clear));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the pci bar read raw operation. */
static int
pci_bar_read_raw(
	struct drv_pci_device *device,
	unsigned index,
	enum drv_pci_bar_type type,
	uint32_t *low,
	uint32_t *high)
{
	int function_result;
	int error;

	error = drv_pci_device_config_read32(device, PCI_BAR0 + index * 4U,
					     low);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	*high = 0;
	/* Handles the type condition. */
	if (type == DRV_PCI_BAR_MEMORY64) {
		/* Obtains the drv pci device config read32 result. */
		function_result = drv_pci_device_config_read32(
			device, PCI_BAR0 + (index + 1U) * 4U, high);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the pci command quiesce operation. */
static int
pci_command_quiesce(
	struct drv_pci_device *device,
	uint16_t command)
{
	uint16_t readback;
	int error;

	command &= (uint16_t)~PCI_COMMAND_ENABLE_MASK;
	error = drv_pci_device_config_write16(device, PCI_COMMAND, command);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(device, PCI_COMMAND, &readback);

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Returns the computed result. */
	return (readback & PCI_COMMAND_ENABLE_MASK) == 0 ? 0 : EIO;
}

/* Supports the pci bar write raw operation. */
static int
pci_bar_write_raw(
	struct drv_pci_device *device,
	unsigned index,
	enum drv_pci_bar_type type,
	uint32_t low,
	uint32_t high)
{
	int function_result;
	int error;

	/* Decode is disabled by the caller, so commit the low half last. */
	if (type == DRV_PCI_BAR_MEMORY64) {
		error = drv_pci_device_config_write32(
			device, PCI_BAR0 + (index + 1U) * 4U, high);

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}

	/* Obtains the drv pci device config write32 result. */
	function_result = drv_pci_device_config_write32(
		device, PCI_BAR0 + index * 4U, low);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the pci bar cache readback operation. */
static void
pci_bar_cache_readback(
	struct drv_pci_device *device,
	unsigned index,
	struct drv_pci_bar *bar)
{
	uint32_t low, high;

	/* Checks the pci bar read raw result. */
	if (pci_bar_read_raw(device, index, bar->type, &low, &high) == 0)
		bar->bus_address = pci_bar_address(bar->type, low, high);
	else
		bar->bus_address = 0;
}

/* Supports the pci bar address operation. */
static uint64_t
pci_bar_address(
	enum drv_pci_bar_type type,
	uint32_t low,
	uint32_t high)
{
	/* Handles the type condition. */
	if (type == DRV_PCI_BAR_IO)
		return low & ~3U;

	/* Returns the computed result. */
	return ((uint64_t)(type == DRV_PCI_BAR_MEMORY64 ? high : 0) << 32) |
	       (low & ~15U);
}

/* Supports the establish intx operation. */
static int
establish_intx(
	struct pci_irq_cookie *cookie,
	const struct drv_pci_irq *irq)
{
	struct pci_intx_line *candidate, *line;
	struct pci_irq_cookie **link;
	bool enabled;
	int hal_error;

	candidate = hal_malloc(sizeof(*candidate));

	/* Handles the candidate availability. */
	if (candidate == NULL)
		return ENOMEM;
	memset(candidate, 0, sizeof(*candidate));
	candidate->irq = (int)irq->vector;

	enabled = intx_lock_enter();
	line = find_intx_line(candidate->irq);

	/* Handles the line availability. */
	if (line != NULL) {
		/* Handles the line condition. */
		if (line->removing) {
			intx_lock_leave(enabled);
			hal_free(candidate);

			/* Returns the computed result. */
			return EBUSY;
		}
		/* Process each element required by the operation. */
		for (link = &line->handlers; *link != NULL;
		     link = &(*link)->intx_next)
			;
		*link = cookie;
		cookie->irq = line->irq;
		cookie->intx_line = line;
		intx_lock_leave(enabled);
		hal_free(candidate);

		/* Reports successful completion. */
		return 0;
	}

	/*
 * Publish a line only after the sole HAL handler has been installed.
	 * The line remains masked until both the handler and cookie list exist.
	 */
	hal_error = hal_irq_set_handler(candidate->irq, pci_intx_dispatch,
					candidate);

	/* Checks the operation status. */
	if (hal_error == HAL_OK) {
		candidate->handlers = cookie;
		candidate->next = intx_lines;
		intx_lines = candidate;
		cookie->irq = candidate->irq;
		cookie->intx_line = candidate;
		hal_irq_unmask(candidate->irq);
	}
	intx_lock_leave(enabled);

	/* Checks the operation status. */
	if (hal_error != HAL_OK) {
		hal_free(candidate);

		/* Returns the computed result. */
		return hal_error == HAL_ERR_BUSY ? EBUSY : EIO;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the intx lock enter operation. */
static bool
intx_lock_enter(
	void)
{
	bool enabled = hal_irq_disable();

	/* Continue while the operation condition remains true. */
	while (!hal_atomic_uint_try_acquire(&intx_lock))
		hal_atomic_relax();

	/* Returns the computed result. */
	return enabled;
}

/* Supports the find intx line operation. */
static struct pci_intx_line *
find_intx_line(
	int irq)
{
	struct pci_intx_line *line;

	/* Process each linked entry. */
	for (line = intx_lines; line != NULL; line = line->next) {
		/* Handles the line condition. */
		if (line->irq == irq)
			return line;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the intx lock leave operation. */
static void
intx_lock_leave(
	bool enabled)
{
	hal_atomic_store_release(&intx_lock, 0U);

	/* Handles the enabled condition. */
	if (enabled)
		hal_irq_enable();
}

/* Supports the establish msi operation. */
static int
establish_msi(
	struct pci_irq_cookie *cookie,
	const struct drv_pci_irq *irq)
{
	struct drv_pci_device *device = cookie->device;
	char source[17];
	paddr_t address;
	uint32_t event;
	uint16_t control;
	unsigned data_offset;
	int error;

	/* Checks the drv pci device config read16 result. */
	if (drv_pci_device_config_read16(device,
					 cookie->capability + PCI_MSI_CONTROL,
					 &control) != 0 ||
	    drv_pci_device_config_read32(device,
					 cookie->capability + PCI_MSI_ADDRESS,
					 &cookie->msi_address_low_saved) != 0)

		/* Returns the computed result. */
		return EIO;
	cookie->msi_control_saved = control;

	/* Handles the control condition. */
	if ((control & PCI_MSI_64BIT) != 0) {
		/* Checks the drv pci device config read32 result. */
		if (drv_pci_device_config_read32(
			    device, cookie->capability + PCI_MSI_ADDRESS + 4U,
			    &cookie->msi_address_high_saved) != 0)

			/* Returns the computed result. */
			return EIO;
		data_offset = cookie->capability + 12U;
	} else {
		cookie->msi_address_high_saved = 0;
		data_offset = cookie->capability + 8U;
	}

	/* Checks the drv pci device config read16 result. */
	if (drv_pci_device_config_read16(device, data_offset,
					 &cookie->msi_data_saved) != 0)

		/* Returns the computed result. */
		return EIO;
	cookie->msi_state_saved = 1;

	pci_source(&device->address, source);
	error = hal_irq_register_msi(source, pci_irq_dispatch, cookie,
				     &cookie->irq, &address, &event);

	/* Checks the operation status. */
	if (error != HAL_OK)
		return error == HAL_ERR_NOMEM ? ENOMEM : EIO;
	cookie->message_registered = 1;

	/* Handles the event condition. */
	if (event > UINT16_MAX) {
		error = EIO;
		goto fail;
	}
	control &= (uint16_t)~(PCI_MSI_ENABLE | PCI_MSI_MME_MASK);

	/* Checks the drv pci device config write16 result. */
	if (drv_pci_device_config_write16(device,
					  cookie->capability + PCI_MSI_CONTROL,
					  control) != 0 ||
	    drv_pci_device_config_write32(device,
					  cookie->capability + PCI_MSI_ADDRESS,
					  (uint32_t)address) != 0) {
		error = EIO;
		goto fail;
	}

	/* Handles the control condition. */
	if ((control & PCI_MSI_64BIT) != 0) {
		/* Checks the drv pci device config write32 result. */
		if (drv_pci_device_config_write32(
			    device, cookie->capability + PCI_MSI_ADDRESS + 4U,
			    (uint32_t)((uint64_t)address >> 32)) != 0) {
			error = EIO;
			goto fail;
		}
		data_offset = cookie->capability + 12U;
	} else {
		/* Handles the address condition. */
		if (address > UINT32_MAX) {
			error = ERANGE;
			goto fail;
		}
		data_offset = cookie->capability + 8U;
	}

	/* Checks the drv pci device config write16 result. */
	if (drv_pci_device_config_write16(device, data_offset,
					  (uint16_t)event) != 0 ||
	    drv_pci_device_config_write16(device,
					  cookie->capability + PCI_MSI_CONTROL,
					  control | PCI_MSI_ENABLE) != 0) {
		error = EIO;
		goto fail;
	}
	(void)irq;

	/* Reports successful completion. */
	return 0;
fail:

	/* Checks the hal irq unregister msi result. */
	if (cookie->message_registered &&
	    hal_irq_unregister_msi(cookie->irq) == HAL_OK)
		cookie->message_registered = 0;
	(void)drv_pci_device_config_write32(
		device, cookie->capability + PCI_MSI_ADDRESS,
		cookie->msi_address_low_saved);

	/* Handles the cookie condition. */
	if ((cookie->msi_control_saved & PCI_MSI_64BIT) != 0) {
		(void)drv_pci_device_config_write32(
			device, cookie->capability + PCI_MSI_ADDRESS + 4U,
			cookie->msi_address_high_saved);
	}
	(void)drv_pci_device_config_write16(
		device,
		(cookie->msi_control_saved & PCI_MSI_64BIT) != 0
			? cookie->capability + 12U
			: cookie->capability + 8U,
		cookie->msi_data_saved);
	(void)drv_pci_device_config_write16(
		device, cookie->capability + PCI_MSI_CONTROL,
		cookie->msi_control_saved);

	/* Returns the computed result. */
	return error;
}

/* Supports the pci source operation. */
static void
pci_source(
	const struct drv_pci_address *address,
	char result[17])
{
	static const char hex[] = "0123456789abcdef";

	/* Renders the address as the fixed source-name layout. */
	result[0] = 'P';
	result[1] = 'C';
	result[2] = 'I';
	result[3] = ' ';
	result[4] = hex[address->segment >> 12];
	result[5] = hex[(address->segment >> 8) & 15U];
	result[6] = hex[(address->segment >> 4) & 15U];
	result[7] = hex[address->segment & 15U];
	result[8] = ':';
	result[9] = hex[address->bus >> 4];
	result[10] = hex[address->bus & 15U];
	result[11] = ':';
	result[12] = hex[address->device >> 4];
	result[13] = hex[address->device & 15U];
	result[14] = '.';
	result[15] = hex[address->function];
	result[16] = '\0';
}

/* Supports the establish msix operation. */
static int
establish_msix(
	struct pci_irq_cookie *cookie)
{
	unsigned index_for;
	volatile uint32_t *entry;
	char source[17];
	paddr_t address;
	uint32_t event;
	uint16_t control;
	int error;

	error = map_msix_entry(cookie);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	cookie->table_mapped = 1;
	entry = cookie->table.address;

	/* Checks the drv pci device config read16 result. */
	if (drv_pci_device_config_read16(cookie->device,
					 cookie->capability + PCI_MSIX_CONTROL,
					 &cookie->msix_control_saved) != 0) {
		error = EIO;
		goto fail;
	}
	hal_io_rmb();
	/* Process each remaining element. */
	for (index_for = 0; index_for < 4U; index_for++)
		cookie->msix_entry_saved[index_for] = entry[index_for];
	cookie->msix_state_saved = 1;
	entry[3] |= PCI_MSIX_ENTRY_MASK;
	pci_source(&cookie->device->address, source);
	error = hal_irq_register_msi(source, pci_irq_dispatch, cookie,
				     &cookie->irq, &address, &event);

	/* Checks the operation status. */
	if (error != HAL_OK) {
		error = error == HAL_ERR_NOMEM ? ENOMEM : EIO;
		goto fail;
	}
	cookie->message_registered = 1;
	entry[0] = (uint32_t)address;
	entry[1] = (uint32_t)((uint64_t)address >> 32);
	entry[2] = event;
	hal_io_mb();

	/* Checks the drv pci device config read16 result. */
	if (drv_pci_device_config_read16(cookie->device,
					 cookie->capability + PCI_MSIX_CONTROL,
					 &control) != 0 ||
	    drv_pci_device_config_write16(
		    cookie->device, cookie->capability + PCI_MSIX_CONTROL,
		    (control | PCI_MSIX_ENABLE) &
			    (uint16_t)~PCI_MSIX_FUNCTION_MASK) != 0) {
		error = EIO;

		/* Checks the hal irq unregister msi result. */
		if (hal_irq_unregister_msi(cookie->irq) == HAL_OK)
			cookie->message_registered = 0;
		goto fail;
	}
	entry[3] &= ~PCI_MSIX_ENTRY_MASK;
	hal_io_mb();

	/* Reports successful completion. */
	return 0;
fail:

	/* Checks the hal irq unregister msi result. */
	if (cookie->message_registered &&
	    hal_irq_unregister_msi(cookie->irq) == HAL_OK)
		cookie->message_registered = 0;

	/* Handles the cookie condition. */
	if (cookie->msix_state_saved && cookie->table_mapped) {
		entry = cookie->table.address;
		entry[0] = cookie->msix_entry_saved[0];
		entry[1] = cookie->msix_entry_saved[1];
		entry[2] = cookie->msix_entry_saved[2];
		entry[3] = cookie->msix_entry_saved[3] | PCI_MSIX_ENTRY_MASK;
		hal_io_mb();
		(void)drv_pci_device_config_write16(
			cookie->device, cookie->capability + PCI_MSIX_CONTROL,
			cookie->msix_control_saved);
		entry[3] = cookie->msix_entry_saved[3];
		hal_io_mb();
	}

	/* Handles the cookie condition. */
	if (cookie->table_mapped) {
		cookie->device->bus->ops->unmap_bar(cookie->device->bus->host,
						    &cookie->table);
		cookie->table_mapped = 0;
	}

	/* Returns the computed result. */
	return error;
}

/* Supports the map msix entry operation. */
static int
map_msix_entry(
	struct pci_irq_cookie *cookie)
{
	int function_result;
	struct drv_pci_device *device = cookie->device;
	struct drv_pci_bar bar;
	uint32_t table;
	uint64_t offset;
	unsigned bir;

	/* Checks the drv pci device config read32 result. */
	if (drv_pci_device_config_read32(
		    device, cookie->capability + PCI_MSIX_TABLE, &table) != 0)

		/* Returns the computed result. */
		return EIO;
	bir = table & 7U;
	offset = (uint64_t)(table & ~7U) +
		 (uint64_t)cookie->index * PCI_MSIX_ENTRY_SIZE;

	/* Checks the drv pci device bar result. */
	if (bir >= device->bar_count ||
	    drv_pci_device_bar(device, bir, &bar) != 0 || offset > bar.size ||
	    PCI_MSIX_ENTRY_SIZE > bar.size - offset)

		/* Returns the computed result. */
		return EINVAL;
	bar.bus_address += offset;
	bar.size = PCI_MSIX_ENTRY_SIZE;

	/* Handles the map bar availability. */
	if (device->bus->ops->map_bar == NULL)
		return ENOTSUP;

	/* Computes the function result. */
	function_result = device->bus->ops->map_bar(
		device->bus->host, device, &bar,
		DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
		&cookie->table);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the disestablish intx operation. */
static int
disestablish_intx(
	struct pci_irq_cookie *cookie)
{
	struct pci_intx_line *line;
	struct pci_intx_line **line_link;
	struct pci_irq_cookie **cookie_link;
	bool enabled;
	int hal_error;

	enabled = intx_lock_enter();
	line = cookie->intx_line;

	/* Handles the line availability. */
	if (line == NULL || line->removing) {
		intx_lock_leave(enabled);

		/* Returns the computed result. */
		return line == NULL ? EINVAL : EBUSY;
	}
	/* Process each element required by the operation. */
	for (cookie_link = &line->handlers; *cookie_link != NULL;
	     cookie_link = &(*cookie_link)->intx_next) {
		/* Handles the cookie link condition. */
		if (*cookie_link == cookie)
			break;
	}

	/* Handles the cookie link availability. */
	if (*cookie_link == NULL) {
		intx_lock_leave(enabled);

		/* Returns the computed result. */
		return EINVAL;
	}

	/*
 * Preserve the exact registered state on EBUSY. In particular, a driver
	 * which retains its argument after an in-flight callback must remain
	 * able to retry without repairing a half-unlinked shared handler. */
	if (line->dispatching != 0) {
		intx_lock_leave(enabled);

		/* Returns the computed result. */
		return EBUSY;
	}

	/* Handles the intx next availability. */
	if (line->handlers != cookie || cookie->intx_next != NULL) {
		*cookie_link = cookie->intx_next;
		cookie->intx_line = NULL;
		cookie->intx_next = NULL;
		intx_lock_leave(enabled);
		hal_free(cookie);

		/* Reports successful completion. */
		return 0;
	}

	/* Only the final owner may mask and remove the physical IRQ handler. */
	line->removing = 1;
	hal_irq_mask(line->irq);
	intx_lock_leave(enabled);
	hal_error = hal_irq_set_handler(line->irq, NULL, NULL);

	/* Checks the operation status. */
	if (hal_error != HAL_OK) {
		enabled = intx_lock_enter();
		line->removing = 0;
		hal_irq_unmask(line->irq);
		intx_lock_leave(enabled);

		/* Returns the computed result. */
		return hal_error == HAL_ERR_BUSY ? EBUSY : EIO;
	}

	/*
 * HAL removal is a checked drain barrier, so no dispatcher can still
	 * hold the line or its final cookie once it succeeds. */
	enabled = intx_lock_enter();
	/* Process each linked entry. */
	for (line_link = &intx_lines; *line_link != NULL;
	     line_link = &(*line_link)->next) {
		/* Handles the line link condition. */
		if (*line_link == line) {
			*line_link = line->next;
			break;
		}
	}
	line->handlers = NULL;
	cookie->intx_line = NULL;
	intx_lock_leave(enabled);
	hal_free(line);
	hal_free(cookie);

	/* Reports successful completion. */
	return 0;
}

/* Supports the pci irq dispatch operation. */
static void
pci_irq_dispatch(
	int irq,
	hal_irq_ack_t acknowledge,
	void *argument)
{
	struct pci_irq_cookie *cookie = argument;

	(void)irq;
	(void)cookie->handler(cookie->argument);
	hal_irq_send_eoi(acknowledge);
}

/* Supports the pci intx dispatch operation. */
static void
pci_intx_dispatch(
	int irq,
	hal_irq_ack_t acknowledge,
	void *argument)
{
	drv_pci_irq_handler_t handler;
	void *handler_argument;
	struct pci_irq_cookie *next;
	struct pci_intx_line *line = argument;
	struct pci_irq_cookie *cookie;
	bool enabled;

	(void)irq;
	enabled = intx_lock_enter();
	line->dispatching++;
	cookie = line->handlers;
	intx_lock_leave(enabled);
	/* Continue while the operation condition remains true. */
	while (cookie != NULL) {
		enabled = intx_lock_enter();

		/*
 * Checked removal cannot unlink a cookie while any dispatcher
		 * is walking this line. Establishment may append a new cookie,
		 * which may be observed on this interrupt or the next one. */
		handler = cookie->handler;
		handler_argument = cookie->argument;
		next = cookie->intx_next;
		intx_lock_leave(enabled);
		(void)handler(handler_argument);
		cookie = next;
	}
	enabled = intx_lock_enter();
	line->dispatching--;
	intx_lock_leave(enabled);
	hal_irq_send_eoi(acknowledge);
}
