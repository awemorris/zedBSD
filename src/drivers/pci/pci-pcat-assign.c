/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT host's assignment of the memory BARs the firmware left
 * unassigned.
 *
 * The Latitude 5330's firmware leaves the 64-bit BAR0 of its LPSS I2C
 * controllers (00:15.0, 00:15.1) at 0, for the operating system to place
 * in the host bridge's window.  The PCI core holds back the attach of such a function

 * on the root bus (struct drv_pci_bus_ops's defer_unassigned) until the
 * ACPI namespace is loaded, which comes after the PCI scan.  Then the
 * windows of \_SB.PC00's _CRS are read, the ranges something already
 * decodes are listed (every BAR with an address, every bridge's windows,
 * the motherboard resource devices' _CRS, the ECAM space and the firmware's
 * memory map), each unassigned memory BAR of a waiting function is placed
 * (pci-window.c) and written, and the waiting functions are probed.  A
 * machine without unassigned BARs (QEMU) reads nothing and assigns nothing.
 *
 * A driver may also ask for one of its BARs to grow (drv_pci_pcat_grow_bar):
 * the same windows and busy ranges are read, without the function's own
 * BARs and the windows of the bridges above it; those windows grow, or
 * move with the BAR when only that function's BARs are behind them.
 * Nothing of it runs unless a driver asks.
 */

#include <drivers/acpi/acpi.h>
#include <drivers/pci/pci-pcat.h>
#include <drivers/pci/pci-window.h>
#include <drivers/pci/pci.h>
#include <kern/kcrt.h>
#if defined(CONFIG_DRIVER_GRAPHICS) && CONFIG_DRIVER_GRAPHICS
#include "drivers/platform/pcat/graphics/backend.h"
#endif
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

/* The most windows and busy ranges kept; more are dropped and logged. */
#define ASSIGN_WINDOWS_MAX	32U
#define ASSIGN_BUSY_MAX		512U

/* The end of the legacy VGA and option ROM ranges, which are no window for a BAR. */
#define ASSIGN_LEGACY_END	0x100000U

/*
 * A grown BAR: the bridges above its function it passes through, at most;
 * the BARs of a function; the first address above 32 bits; the command
 * register and its I/O, memory and bus-master enables.
 *
 * Values referenced: PCI Local Bus Specification (the command register at
 * 0x04, bits 0 to 2 the I/O space, memory space and bus master enables;
 * six BARs in a type 0 header).
 */
#define GROW_BRIDGES_MAX	8U
#define GROW_BARS		6U
#define GROW_4GIB		UINT64_C(0x100000000)
#define GROW_COMMAND		0x04U
#define GROW_COMMAND_ENABLE	0x0007U

/* The bus numbers ECAM can cover, and the space one bus takes. */
#define ASSIGN_ECAM_BUSES	256U
#define ASSIGN_ECAM_BUS_SHIFT	20U

/* A type 1 header's memory window, prefetchable window and their upper halves. */
#define BRIDGE_MEMORY_BASE	0x20U
#define BRIDGE_MEMORY_LIMIT	0x22U
#define BRIDGE_PREFETCH_BASE	0x24U
#define BRIDGE_PREFETCH_LIMIT	0x26U
#define BRIDGE_PREFETCH_UPPER_BASE	0x28U
#define BRIDGE_PREFETCH_UPPER_LIMIT	0x2cU
#define BRIDGE_WINDOW_MASK	0xfff0U
#define BRIDGE_PREFETCH_64	0x0001U
#define BRIDGE_WINDOW_SHIFT	16U
#define BRIDGE_WINDOW_GRANULE	0xfffffU

/*
 * The state of one assignment pass: the windows the BARs may be placed
 * in, the ranges they must stay clear of, and how many of each could not
 * be kept.  A grow keeps more: the function, its BAR and the sizes, the
 * bridges above it with their old windows, whether something else is
 * behind them, the plan (the BAR's place, the windows' span, the other
 * BARs' places) and the range in the way for the log.  It lives for the
 * pass only.
 */
struct assign_state {
	struct drv_pci_range windows[ASSIGN_WINDOWS_MAX];
	unsigned window_count;
	struct drv_pci_range busy[ASSIGN_BUSY_MAX];
	unsigned busy_count;
	unsigned dropped;
	unsigned waiting;
	struct drv_pci_device *grown;
	unsigned index;
	uint64_t size;
	uint64_t old_base;
	uint64_t old_size;
	struct drv_pci_device *chain[GROW_BRIDGES_MAX];
	uint64_t chain_base[GROW_BRIDGES_MAX];
	uint64_t chain_limit[GROW_BRIDGES_MAX];
	unsigned chain_count;
	bool sharing;
	bool moved;
	uint64_t new_base;
	uint64_t new_window_base;
	uint64_t new_window_limit;
	uint64_t target[GROW_BARS];
	unsigned bar_count;
	uint64_t in_way_base;
	uint64_t in_way_length;
	const char *step;
	uint64_t step_address;
};

/*
 * The firmware facts the HAL of the PC/AT port keeps: the ECAM space the
 * MCFG table gave (bsp-pcat/acpi.c) and the memory map the loader handed
 * over (bsp-pcat/boot.c).
 */
extern int amd64_acpi_ecam_address(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint64_t *result) __attribute__((weak));
extern uint32_t bsp_mem_range_count(void) __attribute__((weak));
extern int bsp_mem_range(uint32_t index, uint64_t *base, uint64_t *size, uint32_t *type) __attribute__((weak));

static int count_visit(struct drv_pci_device *device, void *argument);
static int window_visit(const struct drv_acpi_resource *resource, void *argument);
static int system_visit(const struct drv_acpi_resource *resource, void *argument);
static int device_busy_visit(struct drv_pci_device *device, void *argument);
static void bridge_busy_add(struct assign_state *state, struct drv_pci_device *device);
static void ecam_busy_add(struct assign_state *state);
static void memory_map_busy_add(struct assign_state *state);
static void busy_add(struct assign_state *state, uint64_t base, uint64_t length);
static int assign_visit(struct drv_pci_device *device, void *argument);
static void bar_assign(struct assign_state *state, struct drv_pci_device *device, const struct drv_pci_bar *bar);
static void assign_pass(struct assign_state *state);
static bool grow_plan(struct assign_state *state, struct drv_pci_device *device, uint64_t base, bool moved);
static uint64_t grow_first(const struct assign_state *state, uint64_t from);
static int grow_apply(struct assign_state *state, struct drv_pci_device *device);
static int grow_busy_visit(struct drv_pci_device *device, void *argument);
static int bridge_prefetch_read(struct drv_pci_device *bridge, uint64_t *base, uint64_t *limit, bool *wide);
static int bridge_prefetch_write(struct drv_pci_device *bridge, uint64_t base, uint64_t limit);
static bool grow_fits(struct assign_state *state, uint64_t base, uint64_t end);

/*
 * Assigns the unassigned memory BARs of the functions that wait, and
 * probes them.
 *
 * It is called once the ACPI namespace is loaded.  Whatever cannot be
 * read or placed is logged; the waiting functions are probed in every
 * case.
 */
void
drv_pci_pcat_assign_deferred(void)
{
	struct assign_state *state;

	/* Allocates the pass's state, too large for a kernel stack frame. */
	state = kern_calloc(1U, sizeof(*state));
	if (state == NULL) {
		kern_logf("pci: no memory to assign the unassigned BARs\n");
		(void)drv_pci_probe_deferred();
		return;
	}

	/* Assigns what the windows have room for. */
	assign_pass(state);

	/* The state is no longer needed. */
	kern_free(state);

	/* Attaches the functions that waited, with what addresses they now have. */
	(void)drv_pci_probe_deferred();
}

/* Reads the windows and the busy ranges, and assigns each unassigned BAR of the waiting functions. */
static void
assign_pass(
	struct assign_state *state)
{
	unsigned index;
	uint64_t last;
	int error;

	/* A machine whose firmware assigned every BAR has nothing to do. */
	(void)drv_pci_foreach_device(count_visit, state);
	if (state->waiting == 0)
		return;

	/* Reads the windows of the host bridge of segment 0, bus 0. */
	error = drv_acpi_pci_root_resources_walk(0, 0, window_visit, state);
	if (error != 0) {
		kern_logf("pci: the host bridge's _CRS could not be read (%d); unassigned BARs stay at 0\n", error);
		return;
	}

	/* Names the windows the BARs may take, for the log of the machine. */
	for (index = 0; index < state->window_count; index++) {
		last = state->windows[index].base + (state->windows[index].length - 1U);
		kern_logf("pci: _CRS window 0x%llx-0x%llx\n",
			  (unsigned long long)state->windows[index].base,
			  (unsigned long long)last);
	}

	/* Lists what already decodes addresses: the functions, the chipset, ECAM and the memory map. */
	(void)drv_pci_foreach_device(device_busy_visit, state);
	(void)drv_acpi_system_resources_walk(system_visit, state);
	ecam_busy_add(state);
	memory_map_busy_add(state);
	if (state->dropped != 0)
		kern_logf("pci: %u ranges were not kept for the BAR assignment\n", state->dropped);

	/* Places and writes each unassigned BAR of the waiting functions. */
	(void)drv_pci_foreach_device(assign_visit, state);
}

/* Counts the functions that wait for a BAR. */
static int
count_visit(
	struct drv_pci_device *device,
	void *argument)
{
	struct assign_state *state;
	bool waits;

	/* Counts a function the core held back. */
	state = argument;
	waits = drv_pci_device_probe_deferred(device);
	if (waits)
		state->waiting++;

	/* Goes on with the next function. */
	return 0;
}

/* Keeps one memory window of the host bridge's _CRS. */
static int
window_visit(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	struct assign_state *state;

	/* Only a memory range with a length, above the legacy ranges below 1 MiB, is a window a BAR can use. */
	state = argument;
	if (resource->kind != DRV_ACPI_RESOURCE_MEMORY)
		return 0;
	if (resource->length == 0)
		return 0;
	if (resource->base < ASSIGN_LEGACY_END)
		return 0;

	/* A window past the table's room is counted as dropped. */
	if (state->window_count >= ASSIGN_WINDOWS_MAX) {
		state->dropped++;
		return 0;
	}

	/* Keeps the window. */
	state->windows[state->window_count].base = resource->base;
	state->windows[state->window_count].length = resource->length;
	state->window_count++;

	/* Goes on with the next resource. */
	return 0;
}

/* Keeps one memory range a motherboard resource device reserves. */
static int
system_visit(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	/* Only a memory range can be in a BAR's way. */
	if (resource->kind != DRV_ACPI_RESOURCE_MEMORY)
		return 0;

	/* Keeps the range. */
	busy_add(argument, resource->base, resource->length);

	/* Goes on with the next resource. */
	return 0;
}

/* Keeps the ranges one function decodes: its assigned memory BARs and, for a bridge, its windows. */
static int
device_busy_visit(
	struct drv_pci_device *device,
	void *argument)
{
	struct drv_pci_bar bar;
	unsigned count;
	unsigned index;
	bool bridge;
	int error;

	/* Keeps each memory BAR that has an address and a size. */
	count = drv_pci_device_bar_count(device);
	for (index = 0; index < count; index++) {
		/* An absent BAR, an I/O BAR and an unassigned one decode no memory. */
		error = drv_pci_device_bar(device, index, &bar);
		if (error != 0)
			continue;
		if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
			continue;
		if (bar.bus_address == 0)
			continue;

		/* Keeps the BAR's range. */
		busy_add(argument, bar.bus_address, bar.size);
	}

	/* Keeps a bridge's windows, which hold the BARs behind it and room for more. */
	bridge = drv_pci_device_is_bridge(device);
	if (bridge)
		bridge_busy_add(argument, device);

	/* Goes on with the next function. */
	return 0;
}

/* Keeps the memory window and the prefetchable window of a PCI-to-PCI bridge. */
static void
bridge_busy_add(
	struct assign_state *state,
	struct drv_pci_device *device)
{
	uint16_t base_register;
	uint16_t limit_register;
	uint32_t upper_base;
	uint32_t upper_limit;
	uint64_t base;
	uint64_t limit;
	int error;

	/* Reads the memory window: bits 31 to 20 of its base and its limit. */
	error = drv_pci_device_config_read16(device, BRIDGE_MEMORY_BASE, &base_register);
	if (error != 0)
		return;
	error = drv_pci_device_config_read16(device, BRIDGE_MEMORY_LIMIT, &limit_register);
	if (error != 0)
		return;

	/* Keeps the window when it is open: a base above its limit closes it. */
	base = (uint64_t)(base_register & BRIDGE_WINDOW_MASK) << BRIDGE_WINDOW_SHIFT;
	limit = ((uint64_t)(limit_register & BRIDGE_WINDOW_MASK) << BRIDGE_WINDOW_SHIFT) | BRIDGE_WINDOW_GRANULE;
	if (base < limit)
		busy_add(state, base, limit - base + 1U);

	/* Reads the prefetchable window the same way. */
	error = drv_pci_device_config_read16(device, BRIDGE_PREFETCH_BASE, &base_register);
	if (error != 0)
		return;
	error = drv_pci_device_config_read16(device, BRIDGE_PREFETCH_LIMIT, &limit_register);
	if (error != 0)
		return;
	base = (uint64_t)(base_register & BRIDGE_WINDOW_MASK) << BRIDGE_WINDOW_SHIFT;
	limit = ((uint64_t)(limit_register & BRIDGE_WINDOW_MASK) << BRIDGE_WINDOW_SHIFT) | BRIDGE_WINDOW_GRANULE;

	/* A 64-bit prefetchable window has its upper 32 bits in two more registers. */
	if ((base_register & BRIDGE_PREFETCH_64) != 0) {
		error = drv_pci_device_config_read32(device, BRIDGE_PREFETCH_UPPER_BASE, &upper_base);
		if (error != 0)
			return;
		error = drv_pci_device_config_read32(device, BRIDGE_PREFETCH_UPPER_LIMIT, &upper_limit);
		if (error != 0)
			return;
		base |= (uint64_t)upper_base << 32;
		limit |= (uint64_t)upper_limit << 32;
	}

	/* Keeps the window when it is open. */
	if (base < limit)
		busy_add(state, base, limit - base + 1U);
}

/* Keeps the ECAM space of segment 0, from its first bus to the last one it covers. */
static void
ecam_busy_add(
	struct assign_state *state)
{
	uint64_t first;
	uint64_t address;
	unsigned bus;
	int error;

	/* A machine without ECAM for bus 0 has none to keep. */
	if (amd64_acpi_ecam_address == NULL)
		return;
	error = amd64_acpi_ecam_address(0, 0, 0, 0, &first);
	if (error != 0)
		return;

	/* Finds the first bus past the region that holds bus 0. */
	for (bus = 1; bus < ASSIGN_ECAM_BUSES; bus++) {
		/* A bus outside the region, or in another region, ends it. */
		error = amd64_acpi_ecam_address(0, (uint8_t)bus, 0, 0, &address);
		if (error != 0)
			break;
		if (address != first + ((uint64_t)bus << ASSIGN_ECAM_BUS_SHIFT))
			break;
	}

	/* Keeps the region. */
	busy_add(state, first, (uint64_t)bus << ASSIGN_ECAM_BUS_SHIFT);
}

/* Keeps every range of the firmware's memory map, RAM and reserved alike. */
static void
memory_map_busy_add(
	struct assign_state *state)
{
	uint64_t base;
	uint64_t size;
	uint32_t count;
	uint32_t index;
	uint32_t type;
	int valid;

	/* Keeps each range the loader handed over. */
	if (bsp_mem_range_count == NULL || bsp_mem_range == NULL)
		return;
	count = bsp_mem_range_count();
	for (index = 0; index < count; index++) {
		/* A range that cannot be read is passed over. */
		valid = bsp_mem_range(index, &base, &size, &type);
		if (valid == 0)
			continue;

		/* Keeps the range whatever its type: a BAR may take none of it. */
		busy_add(state, base, size);
	}
}

/* Keeps one busy range, or counts it as dropped when the table is full. */
static void
busy_add(
	struct assign_state *state,
	uint64_t base,
	uint64_t length)
{
	/* An empty range decodes nothing. */
	if (length == 0)
		return;

	/* A range past the table's room is counted as dropped. */
	if (state->busy_count >= ASSIGN_BUSY_MAX) {
		state->dropped++;
		return;
	}

	/* Keeps the range. */
	state->busy[state->busy_count].base = base;
	state->busy[state->busy_count].length = length;
	state->busy_count++;
}

/* Assigns each unassigned memory BAR of a waiting function. */
static int
assign_visit(
	struct drv_pci_device *device,
	void *argument)
{
	struct drv_pci_bar bar;
	unsigned count;
	unsigned index;
	bool waits;
	int error;

	/* Passes over a function that does not wait. */
	waits = drv_pci_device_probe_deferred(device);
	if (!waits)
		return 0;

	/* Assigns each memory BAR with a size and no address. */
	count = drv_pci_device_bar_count(device);
	for (index = 0; index < count; index++) {
		/* An absent BAR, an I/O BAR and an assigned one are left as they are. */
		error = drv_pci_device_bar(device, index, &bar);
		if (error != 0)
			continue;
		if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
			continue;
		if (bar.size == 0 || bar.bus_address != 0)
			continue;

		/* Places and writes the BAR. */
		bar_assign(argument, device, &bar);
	}

	/* Goes on with the next function. */
	return 0;
}

/* Places one BAR in a window, writes it, and keeps its range busy. */
static void
bar_assign(
	struct assign_state *state,
	struct drv_pci_device *device,
	const struct drv_pci_bar *bar)
{
	struct drv_pci_address location;
	uint64_t address;
	uint64_t window_last;
	unsigned window;
	bool wide;
	int error;

	/* Names the function in what the log says. */
	drv_pci_device_address(device, &location);

	/* Finds the place: a 64-bit BAR prefers the windows above 4 GiB. */
	wide = false;
	if (bar->type == DRV_PCI_BAR_MEMORY64)
		wide = true;
	error = drv_pci_window_place(state->windows,
				     state->window_count,
				     state->busy,
				     state->busy_count,
				     bar->size,
				     wide,
				     &address,
				     &window);
	if (error != 0) {
		kern_logf("pci: %04x:%02x:%02x.%u BAR%u size 0x%llx fits in no _CRS window (%d)\n",
			  location.segment,
			  location.bus,
			  location.device,
			  location.function,
			  bar->index,
			  (unsigned long long)bar->size,
			  error);
		return;
	}

	/* Writes the address, low half last with decoding off. */
	error = drv_pci_device_assign_bar(device, bar->index, address);
	if (error != 0) {
		kern_logf("pci: %04x:%02x:%02x.%u BAR%u could not be written at 0x%llx (%d)\n",
			  location.segment,
			  location.bus,
			  location.device,
			  location.function,
			  bar->index,
			  (unsigned long long)address,
			  error);
		return;
	}

	/* No later BAR may take the same range, a page at least. */
	if (bar->size < DRV_PCI_WINDOW_MIN_SPAN) {
		busy_add(state, address, DRV_PCI_WINDOW_MIN_SPAN);
	} else {
		busy_add(state, address, bar->size);
	}

	/* Says where the BAR went and from which window. */
	window_last = state->windows[window].base + (state->windows[window].length - 1U);
	kern_logf("pci: %04x:%02x:%02x.%u BAR%u assigned 0x%llx size 0x%llx from _CRS window 0x%llx-0x%llx\n",
		  location.segment,
		  location.bus,
		  location.device,
		  location.function,
		  bar->index,
		  (unsigned long long)address,
		  (unsigned long long)bar->size,
		  (unsigned long long)state->windows[window].base,
		  (unsigned long long)window_last);
}

/*
 * Grows a function's 64-bit prefetchable memory BAR to size.
 *
 * In place when the bridges' prefetchable windows can grow past it: the
 * function's other BARs the new range covers move past it.  Otherwise the
 * BAR moves to the first place on its new size in a host bridge window
 * above 4 GiB where nothing decodes, when the windows of the bridges above
 * it hold nothing but this function's prefetchable BARs: those BARs follow
 * it, past the grown BAR, and the windows move with them.  The BAR's
 * contents stay; the kernel's mapping of the boot framebuffer follows it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_device.c (amdgpu_device_resize_fb_bar:
 * a root window above 4 GiB is needed, the BARs move and the size changes
 * with decoding off) and the PCI-to-PCI Bridge Architecture Specification
 * (the prefetchable window in 1 MiB units, its upper 32 bits in two more
 * registers).
 */
int
drv_pci_pcat_grow_bar(
	struct drv_pci_device *device,
	unsigned index,
	uint64_t size)
{
	struct assign_state *state;
	struct drv_pci_address location;
	struct drv_pci_bar bar;
	struct drv_pci_bus *bus;
	struct drv_pci_device *bridge;
	uint64_t window_base;
	uint64_t window_limit;
	uint64_t base;
	bool wide;
	bool placed;
	int error;

	/* A 64-bit prefetchable memory BAR with an address, growing to a power of two. */
	error = drv_pci_device_bar(device, index, &bar);
	if (error != 0)
		return error;
	if (bar.type != DRV_PCI_BAR_MEMORY64 || !bar.prefetchable || bar.bus_address == 0U)
		return EINVAL;
	if (size <= bar.size || (size & (size - 1U)) != 0U)
		return EINVAL;

	/* The pass's state, too large for a kernel stack frame. */
	state = kern_calloc(1U, sizeof(*state));
	if (state == NULL)
		return ENOMEM;
	state->grown = device;
	state->index = index;
	state->size = size;
	state->old_base = bar.bus_address;
	state->old_size = bar.size;

	/* The bridges above the function, nearest first, each with a 64-bit prefetchable window. */
	bus = drv_pci_device_bus(device);
	bridge = NULL;
	if (bus != NULL)
		bridge = drv_pci_bus_bridge(bus);
	while (bridge != NULL) {
		/* Too many bridges. */
		if (state->chain_count >= GROW_BRIDGES_MAX) {
			error = ENOSPC;
			break;
		}

		/* The window's base, last byte and width. */
		error = bridge_prefetch_read(bridge, &window_base, &window_limit, &wide);
		if (error == 0 && (!wide || window_base > window_limit))
			error = ENOSPC;
		if (error != 0)
			break;

		/* Kept with its old window; then the bridge's own bus. */
		state->chain[state->chain_count] = bridge;
		state->chain_base[state->chain_count] = window_base;
		state->chain_limit[state->chain_count] = window_limit;
		state->chain_count++;
		bus = drv_pci_device_bus(bridge);
		bridge = NULL;
		if (bus != NULL)
			bridge = drv_pci_bus_bridge(bus);
	}

	/* The host bridge's windows and what else decodes: other functions, other bridges, the chipset, ECAM, the memory map. */
	if (error == 0)
		error = drv_acpi_pci_root_resources_walk(0, 0, window_visit, state);
	if (error == 0) {
		(void)drv_pci_foreach_device(grow_busy_visit, state);
		(void)drv_acpi_system_resources_walk(system_visit, state);
		ecam_busy_add(state);
		memory_map_busy_add(state);
		if (state->dropped != 0U)
			error = ENOSPC;
	}

	/* In place first, then moved when the windows above hold only this function. */
	placed = false;
	if (error == 0) {
		placed = grow_plan(state, device, state->old_base, false);
		if (!placed && !state->sharing && state->old_base >= GROW_4GIB) {
			for (base = grow_first(state, 0U); base != 0U && !placed; base = grow_first(state, base + size))
				placed = grow_plan(state, device, base, true);
		}

		/* Neither in place nor moved: no room. */
		if (!placed)
			error = ENOSPC;
	}

	/* Names the function in what the log says; a size without room changes nothing. */
	drv_pci_device_address(device, &location);
	if (error != 0) {
		kern_logf("pci: %04x:%02x:%02x.%u BAR%u stays 0x%llx bytes: no room for 0x%llx (%d; bridges %u, windows %u, busy %u, "
			  "dropped %u, shared %u, in the way 0x%llx+0x%llx)\n",
			  location.segment,
			  location.bus,
			  location.device,
			  location.function,
			  index,
			  (unsigned long long)bar.size,
			  (unsigned long long)size,
			  error,
			  state->chain_count,
			  state->window_count,
			  state->busy_count,
			  state->dropped,
			  (unsigned)state->sharing,
			  (unsigned long long)state->in_way_base,
			  (unsigned long long)state->in_way_length);
		kern_free(state);
		return error;
	}

	/* Makes the change; a failure undoes what was done. */
	state->step = "start";
	error = grow_apply(state, device);
	if (error != 0) {
		kern_logf("pci: %04x:%02x:%02x.%u BAR%u stays 0x%llx bytes: the change failed at %s 0x%llx (%d; planned 0x%llx, windows "
			  "0x%llx-0x%llx)\n",
			  location.segment,
			  location.bus,
			  location.device,
			  location.function,
			  index,
			  (unsigned long long)bar.size,
			  state->step,
			  (unsigned long long)state->step_address,
			  error,
			  (unsigned long long)state->new_base,
			  (unsigned long long)state->new_window_base,
			  (unsigned long long)state->new_window_limit);
		kern_free(state);
		return error;
	}

	/* Says what changed. */
	kern_logf("pci: %04x:%02x:%02x.%u BAR%u 0x%llx bytes at 0x%llx grown to 0x%llx bytes at 0x%llx, %u bridge windows 0x%llx-0x%llx\n",
		  location.segment,
		  location.bus,
		  location.device,
		  location.function,
		  index,
		  (unsigned long long)state->old_size,
		  (unsigned long long)state->old_base,
		  (unsigned long long)size,
		  (unsigned long long)state->new_base,
		  state->chain_count,
		  (unsigned long long)state->new_window_base,
		  (unsigned long long)state->new_window_limit);

	/* The kernel's mapping of the boot framebuffer follows a BAR that moved. */
#if CONFIG_DRIVER_GRAPHICS_PCAT
	if (state->new_base != state->old_base)
		drv_pcat_graphics_framebuffer_moved(state->old_base, state->new_base, state->old_size);
#endif
	kern_free(state);

	/* Succeeded: the BAR has its size, the windows hold it. */
	return 0;
}

/*
 * Plans the grown BAR at base: where the function's other BARs go (in
 * place, the ones the new range covers; moved, every prefetchable one,
 * which lay in the old windows), and the windows' new span, in 1 MiB
 * units.  Reports whether the span lies in one host bridge window, clear
 * of everything else, and every window above can hold it.
 */
static bool
grow_plan(
	struct assign_state *state,
	struct drv_pci_device *device,
	uint64_t base,
	bool moved)
{
	struct drv_pci_bar other;
	uint64_t end;
	uint64_t cursor;
	unsigned count;
	unsigned at;
	int found;

	/* The grown BAR on its own size. */
	if ((base & (state->size - 1U)) != 0U)
		return false;
	end = base + state->size;

	/* The other BARs: those in the way (or, when the BAR moves, every prefetchable one) go past it, each on its own size. */
	count = drv_pci_device_bar_count(device);
	if (count > GROW_BARS)
		count = GROW_BARS;
	state->bar_count = count;
	cursor = end;
	for (at = 0U; at < count; at++) {
		/* A BAR that stays. */
		state->target[at] = 0U;
		if (at == state->index)
			continue;
		found = drv_pci_device_bar(device, at, &other);
		if (found != 0)
			continue;
		if (other.type != DRV_PCI_BAR_MEMORY32 && other.type != DRV_PCI_BAR_MEMORY64)
			continue;
		if (other.bus_address == 0U)
			continue;
		if (!moved && (other.bus_address >= end || other.bus_address + other.size <= base))
			continue;
		if (moved && !other.prefetchable)
			continue;

		/* One that moves must be able to: 64-bit and prefetchable. */
		if (other.type != DRV_PCI_BAR_MEMORY64 || !other.prefetchable)
			return false;
		state->target[at] = (cursor + other.size - 1U) & ~(other.size - 1U);
		cursor = state->target[at] + other.size;
	}

	/* The span from the BAR to the last BAR, in 1 MiB units (in place, each window only grows to it). */
	state->moved = moved;
	state->new_base = base;
	state->new_window_base = base;
	state->new_window_limit = ((cursor + BRIDGE_WINDOW_GRANULE) & ~(uint64_t)BRIDGE_WINDOW_GRANULE) - 1U;

	/* In place, every window above must open at or below the BAR. */
	for (at = 0U; at < state->chain_count && !moved; at++) {
		if (state->chain_base[at] > base)
			return false;
	}

	/* Succeeded or not: the span's room. */
	return grow_fits(state, base, state->new_window_limit + 1U);
}

/* Reports the first place on the grown size at or above from in a host bridge window above 4 GiB (0 for none). */
static uint64_t
grow_first(
	const struct assign_state *state,
	uint64_t from)
{
	uint64_t best;
	uint64_t candidate;
	uint64_t window_end;
	unsigned index;

	/* The lowest such place over every window. */
	best = 0U;
	for (index = 0U; index < state->window_count; index++) {
		window_end = state->windows[index].base + state->windows[index].length;
		candidate = state->windows[index].base;
		if (candidate < from)
			candidate = from;
		if (candidate < GROW_4GIB)
			candidate = GROW_4GIB;
		candidate = (candidate + state->size - 1U) & ~(state->size - 1U);
		if (candidate >= window_end || window_end - candidate < state->size)
			continue;
		if (best == 0U || candidate < best)
			best = candidate;
	}

	/* The place, or none. */
	return best;
}

/*
 * Makes the planned change with the function's decoding off: the windows
 * above it (the host bridge's side first), the grown BAR's place and size,
 * its other BARs' places; then decoding as before.  A failure gives
 * everything back in the reverse order.
 */
static int
grow_apply(
	struct assign_state *state,
	struct drv_pci_device *device)
{
	struct drv_pci_bar other;
	uint64_t previous[GROW_BARS];
	uint64_t window_base;
	uint64_t window_limit;
	uint16_t command;
	unsigned windows;
	unsigned at;
	bool placed;
	bool resized;
	int error;

	/* The function stops decoding and mastering. */
	error = drv_pci_device_config_read16(device, GROW_COMMAND, &command);
	if (error != 0)
		return error;
	error = drv_pci_device_config_write16(device, GROW_COMMAND, (uint16_t)(command & ~GROW_COMMAND_ENABLE));
	if (error != 0)
		return error;

	/* The windows, the host bridge's side first: moved to the span, or grown to hold it. */
	windows = 0U;
	while (error == 0 && windows < state->chain_count) {
		at = state->chain_count - 1U - windows;
		window_base = state->new_window_base;
		window_limit = state->new_window_limit;
		if (!state->moved && state->chain_base[at] < window_base)
			window_base = state->chain_base[at];
		if (!state->moved && state->chain_limit[at] > window_limit)
			window_limit = state->chain_limit[at];
		error = bridge_prefetch_write(state->chain[at], window_base, window_limit);
		if (error == 0)
			windows++;
		state->step = "window";
		state->step_address = window_base;
	}

	/* The grown BAR: to its place, then its size. */
	placed = false;
	resized = false;
	if (error == 0 && state->new_base != state->old_base) {
		state->step = "place";
		state->step_address = state->new_base;
		error = drv_pci_device_assign_bar(device, state->index, state->new_base);
		if (error == 0)
			placed = true;
	}

	/* Then its size. */
	if (error == 0) {
		state->step = "size";
		state->step_address = state->size;
		error = drv_pci_device_resize_bar(device, state->index, state->size);
		if (error == 0)
			resized = true;
	}

	/* The other BARs, each keeping its old place for a rollback. */
	for (at = 0U; at < state->bar_count; at++)
		previous[at] = 0U;
	for (at = 0U; at < state->bar_count && error == 0; at++) {
		/* A BAR that stays. */
		if (state->target[at] == 0U)
			continue;

		/* Moves it. */
		(void)drv_pci_device_bar(device, at, &other);
		state->step = "other";
		state->step_address = state->target[at];
		error = drv_pci_device_assign_bar(device, at, state->target[at]);
		if (error == 0)
			previous[at] = other.bus_address;
	}

	/* A failure gives back the BARs' places, the size, the place and the windows (the nearest first). */
	if (error != 0) {
		for (at = 0U; at < state->bar_count; at++) {
			if (previous[at] != 0U)
				(void)drv_pci_device_assign_bar(device, at, previous[at]);
		}

		/* The size, the place and the windows. */
		if (resized)
			(void)drv_pci_device_resize_bar(device, state->index, state->old_size);
		if (placed)
			(void)drv_pci_device_assign_bar(device, state->index, state->old_base);
		for (at = state->chain_count - windows; at < state->chain_count; at++)
			(void)bridge_prefetch_write(state->chain[at], state->chain_base[at], state->chain_limit[at]);
	}

	/* Decoding as before, in either case. */
	(void)drv_pci_device_config_write16(device, GROW_COMMAND, command);
	return error;
}

/*
 * Keeps the ranges a function decodes, but not the growing function's own
 * nor the windows of the bridges above it; notes another function's BAR
 * inside one of those windows (the windows then cannot move).
 */
static int
grow_busy_visit(
	struct drv_pci_device *device,
	void *argument)
{
	struct assign_state *state;
	struct drv_pci_bar bar;
	unsigned count;
	unsigned index;
	unsigned at;
	bool bridge;
	bool above;
	int error;

	/* The growing function decodes the span itself. */
	state = argument;
	if (device == state->grown)
		return 0;

	/* Keeps each memory BAR that has an address and a size. */
	count = drv_pci_device_bar_count(device);
	for (index = 0; index < count; index++) {
		/* An absent BAR, an I/O BAR and an unassigned one decode no memory. */
		error = drv_pci_device_bar(device, index, &bar);
		if (error != 0)
			continue;
		if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
			continue;
		if (bar.bus_address == 0)
			continue;

		/* Keeps the BAR's range; inside a window above the growing function, the windows are shared. */
		busy_add(state, bar.bus_address, bar.size);
		for (at = 0U; at < state->chain_count; at++) {
			if (bar.bus_address >= state->chain_base[at] && bar.bus_address <= state->chain_limit[at])
				state->sharing = true;
		}
	}

	/* Keeps a bridge's windows unless the bridge is above the growing function. */
	bridge = drv_pci_device_is_bridge(device);
	above = false;
	for (at = 0U; at < state->chain_count; at++) {
		if (state->chain[at] == device)
			above = true;
	}

	/* The windows of any other bridge. */
	if (bridge && !above)
		bridge_busy_add(state, device);

	/* Goes on with the next function. */
	return 0;
}

/* Reads a bridge's prefetchable window: its base, its last byte, and whether it is 64-bit. */
static int
bridge_prefetch_read(
	struct drv_pci_device *bridge,
	uint64_t *base,
	uint64_t *limit,
	bool *wide)
{
	uint16_t base_register;
	uint16_t limit_register;
	uint32_t upper_base;
	uint32_t upper_limit;
	int error;

	/* The low words: bits 31 to 20 of the base and the limit, and the 64-bit flag. */
	error = drv_pci_device_config_read16(bridge, BRIDGE_PREFETCH_BASE, &base_register);
	if (error == 0)
		error = drv_pci_device_config_read16(bridge, BRIDGE_PREFETCH_LIMIT, &limit_register);
	if (error != 0)
		return error;
	*wide = (base_register & BRIDGE_PREFETCH_64) != 0U;
	*base = (uint64_t)(base_register & BRIDGE_WINDOW_MASK) << BRIDGE_WINDOW_SHIFT;
	*limit = ((uint64_t)(limit_register & BRIDGE_WINDOW_MASK) << BRIDGE_WINDOW_SHIFT) | BRIDGE_WINDOW_GRANULE;

	/* A 32-bit window has no upper halves. */
	if (!*wide)
		return 0;

	/* A 64-bit window's upper halves. */
	error = drv_pci_device_config_read32(bridge, BRIDGE_PREFETCH_UPPER_BASE, &upper_base);
	if (error == 0)
		error = drv_pci_device_config_read32(bridge, BRIDGE_PREFETCH_UPPER_LIMIT, &upper_limit);
	if (error != 0)
		return error;
	*base |= (uint64_t)upper_base << 32;
	*limit |= (uint64_t)upper_limit << 32;

	/* Succeeded: the window is read. */
	return 0;
}

/* Writes a 64-bit prefetchable window's base and last byte (the flag bits kept), and reads them back. */
static int
bridge_prefetch_write(
	struct drv_pci_device *bridge,
	uint64_t base,
	uint64_t limit)
{
	uint16_t base_register;
	uint16_t limit_register;
	uint16_t read_base;
	uint16_t read_limit;
	uint32_t read_upper_base;
	uint32_t read_upper_limit;
	int error;

	/* The low words keep their flag bits. */
	error = drv_pci_device_config_read16(bridge, BRIDGE_PREFETCH_BASE, &base_register);
	if (error == 0)
		error = drv_pci_device_config_read16(bridge, BRIDGE_PREFETCH_LIMIT, &limit_register);
	if (error != 0)
		return error;
	base_register = (uint16_t)((base_register & ~BRIDGE_WINDOW_MASK) | ((uint16_t)(base >> BRIDGE_WINDOW_SHIFT) & BRIDGE_WINDOW_MASK));
	limit_register = (uint16_t)((limit_register & ~BRIDGE_WINDOW_MASK) |
				    ((uint16_t)(limit >> BRIDGE_WINDOW_SHIFT) & BRIDGE_WINDOW_MASK));

	/* The upper halves, then the low words. */
	error = drv_pci_device_config_write32(bridge, BRIDGE_PREFETCH_UPPER_BASE, (uint32_t)(base >> 32));
	if (error == 0)
		error = drv_pci_device_config_write32(bridge, BRIDGE_PREFETCH_UPPER_LIMIT, (uint32_t)(limit >> 32));
	if (error == 0)
		error = drv_pci_device_config_write16(bridge, BRIDGE_PREFETCH_BASE, base_register);
	if (error == 0)
		error = drv_pci_device_config_write16(bridge, BRIDGE_PREFETCH_LIMIT, limit_register);

	/* Read back. */
	if (error == 0)
		error = drv_pci_device_config_read32(bridge, BRIDGE_PREFETCH_UPPER_BASE, &read_upper_base);
	if (error == 0)
		error = drv_pci_device_config_read32(bridge, BRIDGE_PREFETCH_UPPER_LIMIT, &read_upper_limit);
	if (error == 0)
		error = drv_pci_device_config_read16(bridge, BRIDGE_PREFETCH_BASE, &read_base);
	if (error == 0)
		error = drv_pci_device_config_read16(bridge, BRIDGE_PREFETCH_LIMIT, &read_limit);
	if (error == 0 && (read_upper_base != (uint32_t)(base >> 32) || read_upper_limit != (uint32_t)(limit >> 32) ||
			   read_base != base_register || read_limit != limit_register))
		error = EIO;

	/* The window as written, or the error. */
	return error;
}

/*
 * Reports whether [base, end) lies in one host bridge window and touches
 * no busy range; the first busy range in the way is kept for the log.
 */
static bool
grow_fits(
	struct assign_state *state,
	uint64_t base,
	uint64_t end)
{
	unsigned index;
	bool inside;

	/* Inside one window. */
	inside = false;
	for (index = 0U; index < state->window_count; index++) {
		if (base >= state->windows[index].base && end - state->windows[index].base <= state->windows[index].length)
			inside = true;
	}

	/* Outside every window, it does not fit. */
	if (!inside)
		return false;

	/* Clear of every busy range. */
	for (index = 0U; index < state->busy_count; index++) {
		if (state->busy[index].base < end && base < state->busy[index].base + state->busy[index].length) {
			state->in_way_base = state->busy[index].base;
			state->in_way_length = state->busy[index].length;
			return false;
		}
	}

	/* Succeeded: the span is free. */
	return true;
}
