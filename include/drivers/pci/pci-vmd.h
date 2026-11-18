/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Intel Volume Management Device (VMD) driver and its hardware-free
 * planning half.
 *
 * A VMD hides PCI Express root ports, and the disks behind them, from the
 * machine's own configuration space and shows them through its first BAR
 * instead.  The driver numbers the buses behind it, places the functions'
 * BARs inside its memory windows, and presents the result to the PCI core
 * as a root bus of its own.  The planning half below does the numbering,
 * the placement and the bookkeeping of the forwarded message interrupts
 * without touching hardware, so a host test compiles it as it is.
 */

#ifndef DRIVERS_PCI_PCI_VMD_H
#define DRIVERS_PCI_PCI_VMD_H

#include <stdbool.h>
#include <stdint.h>

#include <drivers/pci/pci-window.h>

/* The most root ports, and the most BARs behind one port, a plan keeps. */
#define DRV_PCI_VMD_PORTS_MAX		8U
#define DRV_PCI_VMD_PORT_BARS_MAX	16U

/* The message slots the children of one VMD can hold at a time. */
#define DRV_PCI_VMD_MESSAGES_MAX	16U

/* The windows a VMD forwards to the buses behind it: MEMBAR1 and MEMBAR2. */
#define DRV_PCI_VMD_WINDOWS_MAX		2U

/*
 * Reads or writes one register of a function behind the VMD.
 *
 * bus is the absolute bus number, as the root ports are programmed with.
 * width is 1, 2 or 4.  A function that is not there reads as all ones and
 * reports 0, exactly as configuration space does; a nonzero report means
 * the access could not be made at all.
 */
typedef int (
	*drv_pci_vmd_config_read_t)(
	void *,
	unsigned,
	unsigned,
	unsigned,
	unsigned,
	unsigned,
	uint32_t *);
typedef int (
	*drv_pci_vmd_config_write_t)(
	void *,
	unsigned,
	unsigned,
	unsigned,
	unsigned,
	unsigned,
	uint32_t);

/*
 * The way a plan reaches the configuration space behind one VMD.
 *
 * The kernel passes its CFGBAR accessors; a host test passes a model of
 * the space in memory.  context goes back to both callbacks.
 */
struct drv_pci_vmd_config_access {
	drv_pci_vmd_config_read_t read;
	drv_pci_vmd_config_write_t write;
	void *context;
};

/*
 * What one VMD offers the buses behind it.
 *
 * bus_start and bus_end bound the bus numbers its CFGBAR decodes; windows
 * are the memory ranges it forwards, MEMBAR2 already without the part the
 * VMD keeps for itself.
 */
struct drv_pci_vmd_domain {
	unsigned bus_start;
	unsigned bus_end;
	struct drv_pci_range windows[DRV_PCI_VMD_WINDOWS_MAX];
	unsigned window_count;
};

/*
 * What became of one root port, and why.
 *
 * Only a port in DRV_PCI_VMD_PORT_USED keeps its bus numbers and decodes
 * a window; every other one has its bus numbers zeroed, its windows
 * closed and its command register cleared, so the PCI core never looks
 * behind it.
 */
enum drv_pci_vmd_port_state {
	DRV_PCI_VMD_PORT_USED = 0,
	DRV_PCI_VMD_PORT_NO_BUS,
	DRV_PCI_VMD_PORT_SWITCH,
	DRV_PCI_VMD_PORT_OWN_BAR,
	DRV_PCI_VMD_PORT_NOT_D0,
	DRV_PCI_VMD_PORT_TOO_MANY_BARS,
	DRV_PCI_VMD_PORT_NO_WINDOW
};

/*
 * One memory BAR of a function behind a root port, and where it went.
 *
 * address is zero until the BAR has been placed.
 */
struct drv_pci_vmd_bar {
	unsigned device;
	unsigned function;
	unsigned index;
	uint64_t size;
	uint64_t address;
	bool wide;
	bool prefetchable;
};

/*
 * The plan of one root port on the VMD's first bus.
 *
 * The firmware_ fields keep what the port held before the plan rewrote it,
 * for the log: the bus number register at 0x18 and the memory window
 * register at 0x20.
 */
struct drv_pci_vmd_port {
	unsigned device;
	unsigned function;
	enum drv_pci_vmd_port_state state;
	unsigned secondary;
	uint64_t window_base;
	uint64_t window_length;
	uint32_t firmware_buses;
	uint32_t firmware_window;
	struct drv_pci_vmd_bar bars[DRV_PCI_VMD_PORT_BARS_MAX];
	unsigned bar_count;
};

/*
 * The plan of one VMD's buses.
 *
 * ports lists the root ports in the order they were found; endpoints
 * counts the functions found on the first bus that are not root ports,
 * which the plan leaves switched off; ports_dropped counts root ports
 * beyond DRV_PCI_VMD_PORTS_MAX, which are switched off too.
 */
struct drv_pci_vmd_plan {
	struct drv_pci_vmd_port ports[DRV_PCI_VMD_PORTS_MAX];
	unsigned port_count;
	unsigned endpoints;
	unsigned ports_dropped;
};

/*
 * The handler of one forwarded message interrupt.
 *
 * It has the type of the PCI core's drv_pci_msi_run_t: it runs the
 * child's handler and reports nonzero when that handler found work.
 */
typedef int (
	*drv_pci_vmd_run_t)(
	void *);

/*
 * One forwarded message interrupt of a child.
 *
 * run is NULL while the slot is free.  in_flight counts the interrupts that
 * have taken run out of the slot and not yet returned; a slot is reused
 * only when both are clear.
 */
struct drv_pci_vmd_message {
	drv_pci_vmd_run_t run;
	void *argument;
	unsigned in_flight;
};

/*
 * The forwarded message interrupts of one VMD's children.
 *
 * Every function here is to be called with the caller's lock held; the
 * table itself takes none.
 */
struct drv_pci_vmd_messages {
	struct drv_pci_vmd_message slots[DRV_PCI_VMD_MESSAGES_MAX];
};

/*
 * One message taken out of the table by an interrupt, to run unlocked.
 */
struct drv_pci_vmd_pending {
	unsigned slot;
	drv_pci_vmd_run_t run;
	void *argument;
};

int
drv_pci_vmd_driver_register(void);

int
drv_pci_vmd_bus_start(
	uint16_t capability,
	uint16_t configuration,
	unsigned *bus_start);

int
drv_pci_vmd_config_offset(
	unsigned bus_start,
	unsigned bus_end,
	uint64_t cfgbar_size,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned offset,
	unsigned width,
	uint64_t *result);

uint32_t
drv_pci_vmd_absent_value(
	unsigned width);

int
drv_pci_vmd_plan_domain(
	const struct drv_pci_vmd_config_access *access,
	const struct drv_pci_vmd_domain *domain,
	struct drv_pci_vmd_plan *plan);

void
drv_pci_vmd_message_address(
	unsigned vector,
	uint64_t *address,
	uint32_t *data);

void
drv_pci_vmd_messages_init(
	struct drv_pci_vmd_messages *messages);

int
drv_pci_vmd_message_take(
	struct drv_pci_vmd_messages *messages,
	drv_pci_vmd_run_t run,
	void *argument,
	unsigned *slot);

int
drv_pci_vmd_message_clear(
	struct drv_pci_vmd_messages *messages,
	unsigned slot,
	drv_pci_vmd_run_t *run,
	void **argument);

void
drv_pci_vmd_message_restore(
	struct drv_pci_vmd_messages *messages,
	unsigned slot,
	drv_pci_vmd_run_t run,
	void *argument);

unsigned
drv_pci_vmd_message_in_flight(
	const struct drv_pci_vmd_messages *messages,
	unsigned slot);

unsigned
drv_pci_vmd_messages_collect(
	struct drv_pci_vmd_messages *messages,
	struct drv_pci_vmd_pending *pending);

void
drv_pci_vmd_messages_release(
	struct drv_pci_vmd_messages *messages,
	const struct drv_pci_vmd_pending *pending,
	unsigned count);

#endif
