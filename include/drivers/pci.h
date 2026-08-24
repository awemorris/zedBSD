/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generic PCI bus and built-in driver interface
 *
 * This interface was designed with reference to the PCI driver APIs of
 * Linux, FreeBSD, NetBSD, and OpenBSD.  It is an independent zedBSD
 * interface; no source code from those kernels is included in this file.
 */

#ifndef ZEDBSD_DRIVERS_PCI_H
#define ZEDBSD_DRIVERS_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/dma.h>

#define DRV_PCI_ANY_ID	((uint16_t)0xffffU)

#define DRV_PCI_MAP_READ	(1U << 0)
#define DRV_PCI_MAP_WRITE	(1U << 1)
#define DRV_PCI_MAP_NOCACHE	(1U << 2)
#define DRV_PCI_MAP_WRITETHROUGH	(1U << 3)
#define DRV_PCI_MAP_PREFETCHABLE	(1U << 4)

#define DRV_PCI_IRQ_ALLOW_INTX	(1U << 0)
#define DRV_PCI_IRQ_ALLOW_MSI	(1U << 1)
#define DRV_PCI_IRQ_ALLOW_MSIX	(1U << 2)

#define DRV_PCI_DETACH_FORCE	(1U << 0)
#define DRV_PCI_DETACH_QUIET	(1U << 1)

struct drv_pci_bus;
struct drv_pci_device;
struct drv_pci_driver;

struct drv_pci_address {
	uint16_t segment;
	uint8_t bus;
	uint8_t device;
	uint8_t function;
};

struct drv_pci_id {
	uint16_t vendor;
	uint16_t device;
	uint16_t subvendor;
	uint16_t subdevice;
	uint32_t class_code;
	uint32_t class_mask;
	uintptr_t driver_data;
};

enum drv_pci_match {
	DRV_PCI_MATCH_NONE = 0,
	DRV_PCI_MATCH_CLASS = 10,
	DRV_PCI_MATCH_GENERIC = 20,
	DRV_PCI_MATCH_VENDOR = 30,
	DRV_PCI_MATCH_EXACT = 40
};

enum drv_pci_bar_type {
	DRV_PCI_BAR_NONE,
	DRV_PCI_BAR_IO,
	DRV_PCI_BAR_MEMORY32,
	DRV_PCI_BAR_MEMORY64
};

struct drv_pci_bar {
	unsigned index;
	enum drv_pci_bar_type type;
	uint64_t bus_address;
	uint64_t size;
	bool prefetchable;
};

struct drv_pci_mapping {
	void *address;
	size_t size;
	enum drv_pci_bar_type type;
	uintptr_t private_data[2];
};

enum drv_pci_irq_type {
	DRV_PCI_IRQ_INTX,
	DRV_PCI_IRQ_MSI,
	DRV_PCI_IRQ_MSIX
};

struct drv_pci_irq {
	enum drv_pci_irq_type type;
	unsigned index;
	unsigned vector;
	uintptr_t private_data[2];
};

typedef int (
	*drv_pci_irq_handler_t)(
	void *);
typedef int (
	*drv_pci_bus_iterator_t)(
	struct drv_pci_bus *,
	void *);
typedef int (
	*drv_pci_device_iterator_t)(
	struct drv_pci_device *,
	void *);

struct drv_pci_bus_ops {
	int (
		*config_read)(
		void *,
		const struct drv_pci_address *,
		unsigned,
		unsigned,
		uint32_t *);
	int (
		*config_write)(
		void *,
		const struct drv_pci_address *,
		unsigned,
		unsigned,
		uint32_t);
	int (
		*map_bar)(
		void *,
		struct drv_pci_device *,
		const struct drv_pci_bar *,
		unsigned,
		struct drv_pci_mapping *);
	void (
		*unmap_bar)(
		void *,
		struct drv_pci_mapping *);
	int (
		*allocate_irqs)(
		void *,
		struct drv_pci_device *,
		enum drv_pci_irq_type,
		unsigned,
		unsigned,
		struct drv_pci_irq *,
		unsigned *);
	void (
		*free_irqs)(
		void *,
		struct drv_pci_device *,
		struct drv_pci_irq *,
		unsigned);
};

struct drv_pci_driver {
	const char *name;
	const struct drv_pci_id *ids;
	size_t id_count;
	int (
		*match)(
		struct drv_pci_device *,
		const struct drv_pci_id *);
	int (
		*attach)(
		struct drv_pci_device *,
		const struct drv_pci_id *);
	int (
		*detach)(
		struct drv_pci_device *,
		unsigned);
	void (
		*shutdown)(
		struct drv_pci_device *);
	int (
		*suspend)(
		struct drv_pci_device *);
	int (
		*resume)(
		struct drv_pci_device *);
	uintptr_t private_data[4];
};

/*
 * PCI core lifecycle and enumeration.
 */
int
drv_pci_init(void);
void
drv_pci_shutdown(void);
int
drv_pci_scan_all(void);
int
drv_pci_foreach_bus(
	drv_pci_bus_iterator_t fn,
	void *arg);
int
drv_pci_foreach_device(
	drv_pci_device_iterator_t fn,
	void *arg);

/*
 * Host bridge and PCI bus objects.
 */
int
drv_pci_bus_create_root(
	uint16_t segment,
	uint8_t number,
	const struct drv_pci_bus_ops *ops,
	void *host,
	struct drv_dma_device *dma,
	struct drv_pci_bus **result);
int
drv_pci_bus_create_child(
	struct drv_pci_bus *parent,
	struct drv_pci_device *bridge,
	uint8_t number,
	struct drv_pci_bus **result);
int
drv_pci_bus_destroy(
	struct drv_pci_bus *bus);
int
drv_pci_bus_scan(
	struct drv_pci_bus *bus);
int
drv_pci_bus_rescan(
	struct drv_pci_bus *bus);
int
drv_pci_bus_scan_tree(
	struct drv_pci_bus *bus);
uint16_t
drv_pci_bus_segment(
	const struct drv_pci_bus *b);
uint8_t
drv_pci_bus_number(
	const struct drv_pci_bus *b);
struct drv_pci_bus *
drv_pci_bus_parent(
	const struct drv_pci_bus *b);
struct drv_pci_device *
drv_pci_bus_bridge(
	const struct drv_pci_bus *b);
int
drv_pci_bus_foreach_device(
	struct drv_pci_bus *b,
	drv_pci_device_iterator_t fn,
	void *arg);

/*
 * Device identity and topology.
 */
struct drv_pci_device *
drv_pci_find_device(
	const struct drv_pci_address *a);
struct drv_pci_device *
drv_pci_find_id(
	uint16_t v,
	uint16_t p,
	struct drv_pci_device *after);
struct drv_pci_device *
drv_pci_find_class(
	uint32_t c,
	uint32_t m,
	struct drv_pci_device *after);
struct drv_pci_bus *
drv_pci_device_bus(
	const struct drv_pci_device *d);
struct drv_pci_bus *
drv_pci_device_subordinate_bus(
	const struct drv_pci_device *d);
void
drv_pci_device_address(
	const struct drv_pci_device *d,
	struct drv_pci_address *a);
uint16_t
drv_pci_device_vendor(
	const struct drv_pci_device *d);
uint16_t
drv_pci_device_product(
	const struct drv_pci_device *d);
uint16_t
drv_pci_device_subvendor(
	const struct drv_pci_device *d);
uint16_t
drv_pci_device_subproduct(
	const struct drv_pci_device *d);
uint32_t
drv_pci_device_class(
	const struct drv_pci_device *d);
uint8_t
drv_pci_device_revision(
	const struct drv_pci_device *d);
uint8_t
drv_pci_device_header_type(
	const struct drv_pci_device *d);
bool
drv_pci_device_is_bridge(
	const struct drv_pci_device *d);
bool
drv_pci_device_is_multifunction(
	const struct drv_pci_device *d);

/*
 * PCI configuration space.
 */
int
drv_pci_device_config_read8(
	struct drv_pci_device *d,
	unsigned o,
	uint8_t *v);
int
drv_pci_device_config_read16(
	struct drv_pci_device *d,
	unsigned o,
	uint16_t *v);
int
drv_pci_device_config_read32(
	struct drv_pci_device *d,
	unsigned o,
	uint32_t *v);
int
drv_pci_device_config_write8(
	struct drv_pci_device *d,
	unsigned o,
	uint8_t v);
int
drv_pci_device_config_write16(
	struct drv_pci_device *d,
	unsigned o,
	uint16_t v);
int
drv_pci_device_config_write32(
	struct drv_pci_device *d,
	unsigned o,
	uint32_t v);
int
drv_pci_device_find_capability(
	struct drv_pci_device *d,
	uint8_t id,
	unsigned *result);
int
drv_pci_device_find_extended_capability(
	struct drv_pci_device *d,
	uint16_t id,
	unsigned start,
	unsigned *result);

/*
 * Device command register and BAR access.
 */
int
drv_pci_device_enable(
	struct drv_pci_device *d);
void
drv_pci_device_disable(
	struct drv_pci_device *d);
int
drv_pci_device_enable_io(
	struct drv_pci_device *d);
int
drv_pci_device_enable_memory(
	struct drv_pci_device *d);
int
drv_pci_device_set_bus_master(
	struct drv_pci_device *d,
	bool on);
unsigned
drv_pci_device_bar_count(
	const struct drv_pci_device *d);
int
drv_pci_device_bar(
	const struct drv_pci_device *d,
	unsigned i,
	struct drv_pci_bar *b);
int
drv_pci_device_assign_bar(
	struct drv_pci_device *d,
	unsigned i,
	uint64_t a);
int
drv_pci_device_claim_bar(
	struct drv_pci_device *d,
	unsigned i);
void
drv_pci_device_release_bar(
	struct drv_pci_device *d,
	unsigned i);
int
drv_pci_device_map_bar(
	struct drv_pci_device *d,
	unsigned i,
	unsigned f,
	struct drv_pci_mapping *m);
int
drv_pci_device_map_bar_region(
	struct drv_pci_device *d,
	unsigned i,
	uint64_t o,
	size_t s,
	unsigned f,
	struct drv_pci_mapping *m);
void
drv_pci_device_unmap_bar(
	struct drv_pci_device *d,
	struct drv_pci_mapping *m);

/*
 * Interrupt allocation and handler registration.
 */
int
drv_pci_device_allocate_irqs(
	struct drv_pci_device *d,
	unsigned flags,
	unsigned min,
	unsigned max,
	struct drv_pci_irq *i,
	unsigned *n);
void
drv_pci_device_free_irqs(
	struct drv_pci_device *d,
	struct drv_pci_irq *i,
	unsigned n);
int
drv_pci_device_establish_irq(
	struct drv_pci_device *d,
	const struct drv_pci_irq *i,
	drv_pci_irq_handler_t h,
	void *a,
	const char *n,
	void **result);
void
drv_pci_device_disestablish_irq(
	struct drv_pci_device *d,
	void *cookie);

/*
 * Driver binding and per-device driver state.
 */
struct drv_dma_device *
drv_pci_device_dma(
	struct drv_pci_device *d);
struct drv_pci_driver *
drv_pci_device_driver(
	const struct drv_pci_device *d);
void *
drv_pci_device_driver_data(
	const struct drv_pci_device *d);
int
drv_pci_device_set_driver_data(
	struct drv_pci_device *d,
	void *p);
int
drv_pci_device_probe(
	struct drv_pci_device *d);
int
drv_pci_device_detach(
	struct drv_pci_device *d,
	unsigned f);
int
drv_pci_device_reprobe(
	struct drv_pci_device *d);

/*
 * Built-in driver registry and matching.
 */
int
drv_pci_driver_register(
	struct drv_pci_driver *r);
int
drv_pci_driver_unregister(
	struct drv_pci_driver *r);
const char *
drv_pci_driver_name(
	const struct drv_pci_driver *r);
size_t
drv_pci_driver_device_count(
	const struct drv_pci_driver *r);
int
drv_pci_driver_foreach_device(
	struct drv_pci_driver *r,
	drv_pci_device_iterator_t fn,
	void *a);
int
drv_pci_id_match(
	const struct drv_pci_id *i,
	const struct drv_pci_device *d);
const struct drv_pci_id *
drv_pci_driver_find_id(
	const struct drv_pci_driver *r,
	const struct drv_pci_device *d);
int
drv_pci_driver_match(
	struct drv_pci_driver *r,
	struct drv_pci_device *d,
	const struct drv_pci_id **out);

void
drv_pci_dump(void);

#endif
