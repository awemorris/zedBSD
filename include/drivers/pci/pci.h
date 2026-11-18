/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generic PCI bus and built-in driver interface
 */

#ifndef KERN_DRIVERS_PCI_H
#define KERN_DRIVERS_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/dma.h>

#define DRV_PCI_ANY_ID		((uint16_t)0xffffU)

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

/*
 * The bus, device and driver objects of the interface.
 *
 * Procedure referenced: the PCI driver APIs of Linux, FreeBSD, NetBSD and
 * OpenBSD (the shape of the driver interface).
 */
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
	/* CPU physical base after host-bridge translation or BAR relocation. */
	uint64_t physical_address;
	size_t size;
	enum drv_pci_bar_type type;
	uintptr_t private_data[2];
};

/* Opaque token for restoring PCI I/O, memory, and bus-master enable bits. */
struct drv_pci_enable_state {
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

/*
 * Runs every handler on one shared pin line and returns.
 *
 * It sends no end of interrupt: the host that called it ends its own
 * interrupt once, after it has served every pin it found raised.
 */
typedef void (
	*drv_pci_intx_run_t)(
	void *);

/*
 * Runs the handler of one message interrupt and returns what the handler
 * returned: nonzero when it found work.
 *
 * Like drv_pci_intx_run_t it sends no end of interrupt: the host that
 * received the message ends its own interrupt once.
 */
typedef int (
	*drv_pci_msi_run_t)(
	void *);

struct drv_pci_bus_ops {
	/* Zero selects conventional 256-byte PCI configuration space. */
	unsigned config_space_size;
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

	/*
	 * Nonzero asks the core to hold back the probe of a function on a
	 * root bus whose memory BAR the firmware left unassigned (address 0,
	 * size not 0), so the host can give the BAR an address once it knows
	 * its windows; drv_pci_probe_deferred() ends the wait (BUG-210).
	 */
	unsigned defer_unassigned;

	/*
	 * A host that demultiplexes its own pins sets all four, or none; with
	 * none the core gives each pin line to kern_irq_register() by its
	 * vector, as before.  vector is then the host's own pin number, and a
	 * line is told apart by (host, vector), so two hosts may use the same
	 * numbers.
	 *
	 * intx_establish, intx_mask and intx_unmask are called with the core's
	 * line lock held and interrupts off: they neither sleep nor allocate,
	 * and the only lock they take is the host's own, with interrupts off.
	 * intx_establish puts (run, line) in the host's table and leaves the
	 * pin masked: the core unmasks it once the line is published.  It
	 * returns 0, EBUSY when the pin is taken, or another error.
	 *
	 * intx_disestablish is called in thread context with no core lock: it
	 * masks the pin, empties its entry, and waits until no hart is still
	 * inside run for it.  It returns 0 once drained, or EBUSY with the
	 * entry and the pin as they were before the call.
	 *
	 * The host calls run without its own lock held, and sends the end of
	 * interrupt itself, once, after its status is acknowledged.
	 */
	int (
		*intx_establish)(
		void *,
		unsigned,
		drv_pci_intx_run_t,
		void *);
	int (
		*intx_disestablish)(
		void *,
		unsigned,
		drv_pci_intx_run_t,
		void *);
	void (
		*intx_mask)(
		void *,
		unsigned);
	void (
		*intx_unmask)(
		void *,
		unsigned);

	/*
	 * A host that receives its functions' message interrupts itself sets
	 * both, or neither; with neither the core takes each message from
	 * kern_irq_register_msi(), as before.  Both are called in thread
	 * context with no core lock held.
	 *
	 * msi_establish takes one free message of the host for (run,
	 * argument): it reports in its last three arguments a handle for
	 * msi_disestablish, and the address and data the function writes.
	 * The host calls run(argument) from its own interrupt each time the
	 * message arrives, without its own lock held, and ends its interrupt
	 * itself, once.  A host whose messages can pass the function's earlier
	 * writes may call run again while it reports no work.  It returns 0, ENOSPC when no message is free, or
	 * another error.
	 *
	 * msi_disestablish frees the message and waits until no hart is still
	 * inside run for it.  It returns 0 once drained, or EBUSY with the
	 * message kept as it was.
	 */
	int (
		*msi_establish)(
		void *,
		struct drv_pci_device *,
		drv_pci_msi_run_t,
		void *,
		unsigned *,
		uint64_t *,
		uint32_t *);
	int (
		*msi_disestablish)(
		void *,
		unsigned);
};

/*
 * One subsystem publication contract staged by a PCI driver's attach.
 *
 * The interface and its opaque argument remain alive until detach succeeds.
 * PCI invokes publish after attach and unpublish before hardware detach.
 * A failed publish must undo its own visibility and acquired references.
 * A failed unpublish retains all state and may be retried; it must prevent
 * new operations from making teardown unbounded. Neither callback frees
 * the driver's hardware instance. PCI serializes these lifecycle callbacks.
 */
struct drv_pci_service_interface {
	int (
		*publish)(
		struct drv_pci_device *,
		void *);
	int (
		*unpublish)(
		struct drv_pci_device *,
		void *);
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
drv_pci_bus_set_host_function(
	struct drv_pci_bus *bus,
	struct drv_pci_device *function);
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
struct drv_pci_device *
drv_pci_device_upstream(
	const struct drv_pci_device *device);
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
/*
 * Save the current I/O, memory, and bus-master enable bits in a zeroed token.
 * Restore performs a read/modify/write so unrelated command bits survive.
 * This is an exclusive driver attach/detach lease; overlapping tokens or use
 * alongside drv_pci_device_enable()/disable() on the same device are invalid.
 */
int
drv_pci_device_save_enable_state(
	struct drv_pci_device *d,
	struct drv_pci_enable_state *state);
int
drv_pci_device_restore_enable_state(
	struct drv_pci_device *d,
	struct drv_pci_enable_state *state);
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
/*
 * Reprogram a BAR transactionally.  A failed transaction restores the old
 * BAR when possible and deliberately leaves I/O, memory, and bus mastering
 * disabled so a partial address can never be decoded or used for DMA.
 */
int
drv_pci_device_assign_bar(
	struct drv_pci_device *d,
	unsigned i,
	uint64_t a);
/*
 * Gives a resizable memory BAR a new size, keeping its address: the size
 * must be one its Resizable BAR capability offers, and the address a
 * multiple of it.  The BAR must not be claimed.  The device's command bits
 * are as before on return; a failure leaves the old size.
 */
int
drv_pci_device_resize_bar(
	struct drv_pci_device *d,
	unsigned i,
	uint64_t size);
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
 *
 * allocate_irqs gives from min up to max vectors of the first type the
 * flags allow that has min of them: MSI-X as many as the function's table
 * holds (vector i uses table entry i) where the bus offers several, MSI and
 * the pin one.  The caller establishes each vector it was given; a
 * function's MSI-X stays on until its last vector is disestablished.
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
/* Remove this source from interrupt dispatch.  A final INTx owner also masks
 * and removes the physical line handler.  EBUSY leaves the cookie, handler,
 * and all mappings owned by the caller so in-flight dispatch can drain before
 * a retry. */
int
drv_pci_device_disestablish_irq_checked(
	struct drv_pci_device *d,
	void *cookie);
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

/*
 * Stage one service during this driver's attach callback. Both callbacks
 * and a driver detach callback are required. The opaque argument may be NULL.
 * Staging does not publish anything; attach failure must clean its own state.
 * Outside attach, or when a service is already staged, this returns EBUSY.
 */
int
drv_pci_device_set_service(
	struct drv_pci_device *device,
	const struct drv_pci_service_interface *interface,
	void *argument);
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
 * Probes held back for an unassigned memory BAR (struct drv_pci_bus_ops's
 * defer_unassigned).  drv_pci_device_probe_deferred() tells whether a
 * function waits; drv_pci_probe_deferred() ends the wait on every root bus
 * and probes the functions that waited, assigned or not.
 */
bool
drv_pci_device_probe_deferred(
	const struct drv_pci_device *device);
int
drv_pci_probe_deferred(void);

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

/*
 * Describes the function at one position of the enumeration for
 * /dev/system.  Reports ENOENT past the last function.
 */
struct system_pci_device_info;
int
drv_pci_system_describe(
	uint32_t index,
	struct system_pci_device_info *info);
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

/*
 * The power of PCI functions across S0 idle (ws052-p004, pci-power.c).
 */

/* The PCI power states a function is put in: D0, and D3hot. */
#define DRV_PCI_D0		0U
#define DRV_PCI_D3_HOT		3U

/*
 * The PCI Express control registers a saved state keeps: device, link,
 * device 2, link 2, slot and root.
 */
#define DRV_PCI_SAVED_PCIE	6U

/*
 * The configuration of one function, saved before it goes to D3hot and
 * written back after it returns to D0, because a function without
 * No_Soft_Reset loses it there.  It keeps the first 64 bytes of the
 * configuration space, the MSI capability, the PCI Express control
 * registers (pcie_flags is the capability's own register, which says
 * whether the slot and root controls exist), the LTR latencies and the L1
 * PM Substates controls, and the whole MSI-X table, which
 * drv_pci_device_save_state() allocates and drv_pci_device_restore_state()
 * or drv_pci_device_discard_state() frees.  A zero offset means the
 * function lacks that capability.
 */
struct drv_pci_saved_state {
	uint32_t header[16];
	uint32_t msi[6];
	uint32_t pcie[DRV_PCI_SAVED_PCIE];
	uint32_t ltr;
	uint32_t l1ss[2];
	uint32_t *msix_table;
	unsigned msix_entries;
	uint16_t msix_control;
	uint16_t msi_offset;
	uint16_t msix_offset;
	uint16_t pcie_offset;
	uint16_t pcie_flags;
	uint16_t ltr_offset;
	uint16_t l1ss_offset;
	uint8_t saved;
};

/*
 * What the platform does for a function's power beyond its own PCI power
 * management: on ACPI, the function's _PSx, power resources and wake
 * (_PRW, _DSW).  set_state takes DRV_PCI_D0 or DRV_PCI_D3_HOT; wake arms or
 * disarms the function's platform wake.  Either may be NULL.  A function
 * the platform knows nothing about reports ENOENT, which is no failure.
 */
struct drv_pci_platform_power {
	int (
		*set_state)(
		void *,
		struct drv_pci_device *,
		unsigned);
	int (
		*wake)(
		void *,
		struct drv_pci_device *,
		bool);
	void *argument;
};

int
drv_pci_device_save_state(
	struct drv_pci_device *device,
	struct drv_pci_saved_state *state);

int
drv_pci_device_restore_state(
	struct drv_pci_device *device,
	struct drv_pci_saved_state *state);

void
drv_pci_device_discard_state(
	struct drv_pci_saved_state *state);

int
drv_pci_device_set_power_state(
	struct drv_pci_device *device,
	unsigned state);

int
drv_pci_device_map_msix_table(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping,
	unsigned *entries);

int
drv_pci_device_set_wake(
	struct drv_pci_device *device,
	bool wake);

int
drv_pci_device_keep_d0(
	struct drv_pci_device *device,
	bool keep);

void
drv_pci_device_name(
	struct drv_pci_device *device,
	char *text,
	size_t size);

void
drv_pci_platform_power_set(
	const struct drv_pci_platform_power *power);

int
drv_pci_suspend_all(
	struct drv_pci_device **failed);

/* A driver's resume that reports ESTALE has its function detached and attached again. */
int
drv_pci_resume_all(void);

#endif
