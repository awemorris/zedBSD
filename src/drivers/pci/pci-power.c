/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power of PCI functions across S0 idle.
 *
 * drv_pci_suspend_all() suspends every function a driver is bound to,
 * the functions behind a bridge before the bridge's own bus: the driver's
 * suspend stops the function, the configuration is saved, the function
 * goes to D3hot through its power management capability, and the
 * platform (ACPI: _PS3, power resources) follows.  A driver that wants
 * the function to wake the system asks for it with drv_pci_device_set_wake()
 * from its suspend; PME_En is set and the platform arms the wake.  If any
 * step fails, every function suspended so far is resumed again, in the
 * reverse order, and the function that failed is reported: S0 idle is
 * abandoned rather than entered with a device still running.  A bound
 * driver without a suspend cannot be suspended and fails the same way.
 *
 * drv_pci_resume_all() undoes it in the reverse order: the platform
 * powers the function, D0, the configuration written back, the wake
 * disarmed, and the driver's resume.  A driver whose resume reports
 * ESTALE says its function lost the state it had and can be attached
 * afresh; the function is detached and attached again.
 *
 * A function without a driver is left alone, unless a function above it
 * has a driver and so may lose its power: a root port behind a VMD loses
 * its bus numbers and windows when the VMD goes to D3hot.  Such a function
 * only has its configuration saved, after the functions behind it were
 * suspended and before the one above it is; it is written back after the
 * function above it is resumed and before the ones behind it are.  A port
 * whose link went down meanwhile is given the time the link and the device
 * behind it need before they are spoken to.
 *
 * The two calls come from the one thread that enters and leaves S0 idle;
 * a second suspend before the resume is refused.
 */

#include <drivers/pci/pci.h>
#include <kern/kcrt.h>
#include <kern/clock.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include "kern/klog.h"
#include "kern/kmem.h"

/*
 * The configuration space registers the save and the restore use: the
 * command register, the dword holding the header type, the header type's
 * layout field and the bridge layout, and the dword holding a bridge's
 * bus numbers.
 */
#define PCI_COMMAND		0x04U
#define PCI_HEADER_DWORDS	16U
#define PCI_HEADER_TYPE_DWORD	3U
#define PCI_HEADER_TYPE_SHIFT	16U
#define PCI_HEADER_LAYOUT	0x7fU
#define PCI_HEADER_BRIDGE	0x01U
#define PCI_BRIDGE_BUSES_DWORD	6U

/* The capabilities: power management, MSI, PCI Express and MSI-X. */
#define CAPABILITY_PM		0x01U
#define CAPABILITY_MSI		0x05U
#define CAPABILITY_PCIE		0x10U
#define CAPABILITY_MSIX		0x11U

/* The extended capabilities: Latency Tolerance Reporting and L1 PM Substates. */
#define EXTENDED_LTR		0x0018U
#define EXTENDED_L1SS		0x001eU

/*
 * PMCSR, the power management control and status register: the power
 * state, No_Soft_Reset, PME_En and PME_Status (written as one to clear).
 */
#define PM_CONTROL		0x04U
#define PM_STATE_MASK		0x0003U
#define PM_NO_SOFT_RESET	0x0008U
#define PM_PME_ENABLE		0x0100U
#define PM_PME_STATUS		0x8000U

/* How long a function needs after entering or leaving D3hot, in milliseconds. */
#define PM_D3HOT_DELAY_MS	10U

/* MSI: the control register and its bits that size the capability. */
#define MSI_CONTROL		0x02U
#define MSI_64BIT		0x0080U
#define MSI_PER_VECTOR_MASK	0x0100U

/* MSI-X: the control register, its function mask, and the size of an entry in dwords. */
#define MSIX_CONTROL		0x02U
#define MSIX_FUNCTION_MASK	0x4000U
#define MSIX_ENTRY_DWORDS	4U

/*
 * The PCI Express capability: the capabilities register with the port's
 * type and the slot bit, the link capabilities with the bit that says the
 * port reports an active data link, the link status with that bit, and the
 * control registers saved: device, link, slot, root, device 2 and link 2.
 * The slot control exists on a port with a slot, the root control on a
 * root port and a root complex event collector.
 *
 * Values referenced: PCI Express Base Specification Revision 4.0
 * Version 1.0, section 7.5.3 (the PCI Express Capabilities, Link
 * Capabilities, Link Control, Link Status, Slot Control and Root Control
 * registers) and section 7.8.3.3 (the L1 PM Substates are enabled only
 * while ASPM L1 is disabled).
 */
#define PCIE_CAPABILITIES	0x02U
#define PCIE_TYPE_MASK		0x00f0U
#define PCIE_TYPE_ROOT_PORT	0x0040U
#define PCIE_TYPE_DOWNSTREAM	0x0060U
#define PCIE_TYPE_EVENT_COLLECTOR	0x00a0U
#define PCIE_SLOT_IMPLEMENTED	0x0100U
#define PCIE_DEVICE_CONTROL	0x08U
#define PCIE_LINK_CAPABILITIES	0x0cU
#define PCIE_LINK_ACTIVE_REPORTING	0x00100000U
#define PCIE_LINK_CONTROL	0x10U
#define PCIE_LINK_ASPM		0x0003U
#define PCIE_LINK_STATUS	0x12U
#define PCIE_LINK_ACTIVE	0x2000U
#define PCIE_SLOT_CONTROL	0x18U
#define PCIE_ROOT_CONTROL	0x1cU
#define PCIE_DEVICE_CONTROL2	0x28U
#define PCIE_LINK_CONTROL2	0x30U

/* Where each saved control sits in the saved state's pcie[]. */
#define SAVED_DEVICE_CONTROL	0U
#define SAVED_LINK_CONTROL	1U
#define SAVED_DEVICE_CONTROL2	2U
#define SAVED_LINK_CONTROL2	3U
#define SAVED_SLOT_CONTROL	4U
#define SAVED_ROOT_CONTROL	5U

/*
 * What a port whose link went down needs before the device behind it is
 * spoken to: up to a second for the link to come up, polled every 10
 * milliseconds, then 100 milliseconds; a port that cannot report its link
 * gets the 100 milliseconds alone.
 *
 * Values referenced: PCI Express Base Specification Revision 4.0
 * Version 1.0, section 6.6.1 (the 100 ms after the link is up, and the
 * 1.0 s within which a device must answer after a reset).
 */
#define LINK_UP_POLLS		100U
#define LINK_UP_POLL_MS		10U
#define LINK_SETTLE_MS		100U

/* LTR: the maximum snoop and no-snoop latencies. */
#define LTR_LATENCIES		0x04U

/* L1 PM Substates: the two control registers. */
#define L1SS_CONTROL1		0x08U
#define L1SS_CONTROL2		0x0cU

/*
 * One function the suspend took on: its saved configuration, how deep it
 * is in the bus tree, and how far its suspend went, which is exactly what
 * its resume undoes.  wake_wanted is set by the driver's suspend through
 * drv_pci_device_set_wake(), keep_d0 through drv_pci_device_keep_d0().
 * save_only marks a function without a driver behind one with a driver:
 * only its configuration is saved and written back.  link_reported and
 * link_active say whether such a port reports its link, and whether the
 * link was up when it was saved.
 */
struct power_entry {
	struct drv_pci_device *device;
	struct drv_pci_saved_state saved;
	unsigned depth;
	uint8_t save_only;
	uint8_t link_reported;
	uint8_t link_active;
	uint8_t driver_suspended;
	uint8_t wake_wanted;
	uint8_t wake_armed;
	uint8_t keep_d0;
	uint8_t in_d3;
	uint8_t platform_d3;
};

/*
 * What the collection of the bound functions fills: the entries, how many
 * are filled, and how many there is room for.
 */
struct power_collection {
	struct power_entry *entries;
	unsigned count;
	unsigned room;
};

/*
 * The functions the current suspend took on, deepest first, and the
 * platform's power operations.  entries is NULL while no suspend holds
 * the functions; drv_pci_suspend_all() fills it and drv_pci_resume_all()
 * frees it, both from the one thread of S0 idle; suspended is set while
 * the functions sleep (also when no function is bound), and current names
 * the function whose driver's suspend runs.  platform is set once by the
 * platform code at start and only read afterwards.
 */
static struct {
	struct power_entry *entries;
	unsigned count;
	unsigned current;
	const struct drv_pci_platform_power *platform;
	uint8_t suspended;
} power;

static int count_visitor(struct drv_pci_device *device, void *argument);
static int collect_visitor(struct drv_pci_device *device, void *argument);
static int taken_on(struct drv_pci_device *device, uint8_t *save_only);
static unsigned device_depth(struct drv_pci_device *device);
static void sort_deepest_first(struct power_entry *entries, unsigned count);
static int suspend_entry(struct power_entry *entry);
static int save_entry(struct power_entry *entry);
static int resume_entry(struct power_entry *entry);
static int restore_entry(struct power_entry *entry);
static void settle_link(struct power_entry *entry);
static int port_with_link(const struct drv_pci_saved_state *state);
static int platform_state(struct drv_pci_device *device, unsigned state);
static int platform_wake(struct drv_pci_device *device, bool wake);
static int pme_set(struct drv_pci_device *device, bool enable);
static void sleep_ms(unsigned milliseconds);
static unsigned msi_dwords(uint16_t control);
static uint32_t read16_or_zero(struct drv_pci_device *device, unsigned offset);
static void log_device(const char *what, struct drv_pci_device *device, int error);

/*
 * Saves the configuration a function loses in D3hot.
 *
 * It reports ENOMEM when the MSI-X table cannot be copied; nothing is kept
 * then.
 */
int
drv_pci_device_save_state(
	struct drv_pci_device *device,
	struct drv_pci_saved_state *state)
{
	struct drv_pci_mapping table;
	volatile uint32_t *entry;
	unsigned capability;
	unsigned index;
	unsigned count;
	unsigned type;
	uint16_t value;
	int error;

	/* Refuses no function or nowhere to save. */
	if (device == NULL || state == NULL)
		return EINVAL;

	/* Starts from nothing saved. */
	kern_memset(state, 0, sizeof(*state));

	/* Saves the first 64 bytes. */
	for (index = 0; index < PCI_HEADER_DWORDS; index++) {
		/* Reads one dword; a failed read refuses the save. */
		error = drv_pci_device_config_read32(device, index * 4U, &state->header[index]);
		if (error != 0)
			return EIO;
	}

	/* Saves the MSI capability's registers, as many as it has. */
	error = drv_pci_device_find_capability(device, CAPABILITY_MSI, &capability);
	if (error == 0) {
		/* Reads the control register, which says how long the capability is. */
		error = drv_pci_device_config_read16(device, capability + MSI_CONTROL, &value);
		if (error != 0)
			return EIO;

		/* Reads the dwords of the capability. */
		count = msi_dwords(value);
		for (index = 0; index < count; index++) {
			/* Reads one dword; a failed read refuses the save. */
			error = drv_pci_device_config_read32(device, capability + index * 4U, &state->msi[index]);
			if (error != 0)
				return EIO;
		}

		/* Remembers where the capability is. */
		state->msi_offset = (uint16_t)capability;
	}

	/* Saves the PCI Express control registers. */
	error = drv_pci_device_find_capability(device, CAPABILITY_PCIE, &capability);
	if (error == 0) {
		/* Reads the port's type and whether it has a slot, which say which controls exist. */
		state->pcie_flags = (uint16_t)read16_or_zero(device, capability + PCIE_CAPABILITIES);

		/* Reads the device, link, device 2 and link 2 controls; one that fails stays zero. */
		state->pcie[SAVED_DEVICE_CONTROL] = read16_or_zero(device, capability + PCIE_DEVICE_CONTROL);
		state->pcie[SAVED_LINK_CONTROL] = read16_or_zero(device, capability + PCIE_LINK_CONTROL);
		state->pcie[SAVED_DEVICE_CONTROL2] = read16_or_zero(device, capability + PCIE_DEVICE_CONTROL2);
		state->pcie[SAVED_LINK_CONTROL2] = read16_or_zero(device, capability + PCIE_LINK_CONTROL2);

		/* Reads the slot control of a port with a slot. */
		if ((state->pcie_flags & PCIE_SLOT_IMPLEMENTED) != 0)
			state->pcie[SAVED_SLOT_CONTROL] = read16_or_zero(device, capability + PCIE_SLOT_CONTROL);

		/* Reads the root control of a root port and of an event collector. */
		type = state->pcie_flags & PCIE_TYPE_MASK;
		if (type == PCIE_TYPE_ROOT_PORT || type == PCIE_TYPE_EVENT_COLLECTOR)
			state->pcie[SAVED_ROOT_CONTROL] = read16_or_zero(device, capability + PCIE_ROOT_CONTROL);

		/* Remembers where the capability is. */
		state->pcie_offset = (uint16_t)capability;
	}

	/* Saves the LTR latencies, which the platform's low-power states depend on. */
	error = drv_pci_device_find_extended_capability(device, EXTENDED_LTR, 0, &capability);
	if (error == 0) {
		/* Reads the latencies. */
		error = drv_pci_device_config_read32(device, capability + LTR_LATENCIES, &state->ltr);
		if (error != 0)
			return EIO;

		/* Remembers where the capability is. */
		state->ltr_offset = (uint16_t)capability;
	}

	/* Saves the L1 PM Substates controls. */
	error = drv_pci_device_find_extended_capability(device, EXTENDED_L1SS, 0, &capability);
	if (error == 0) {
		/* Reads the first control register. */
		error = drv_pci_device_config_read32(device, capability + L1SS_CONTROL1, &state->l1ss[0]);
		if (error != 0)
			return EIO;

		/* Reads the second one. */
		error = drv_pci_device_config_read32(device, capability + L1SS_CONTROL2, &state->l1ss[1]);
		if (error != 0)
			return EIO;

		/* Remembers where the capability is. */
		state->l1ss_offset = (uint16_t)capability;
	}

	/* Saves the MSI-X control register and the whole table, which lives in the function's memory. */
	error = drv_pci_device_find_capability(device, CAPABILITY_MSIX, &capability);
	if (error == 0) {
		/* Reads the control register. */
		error = drv_pci_device_config_read16(device, capability + MSIX_CONTROL, &state->msix_control);
		if (error != 0)
			return EIO;

		/* Maps the table. */
		error = drv_pci_device_map_msix_table(device, &table, &count);
		if (error != 0)
			return error;

		/* Takes room for a copy. */
		state->msix_table = kern_malloc((size_t)count * MSIX_ENTRY_DWORDS * sizeof(uint32_t));
		if (state->msix_table == NULL) {
			drv_pci_device_unmap_bar(device, &table);
			return ENOMEM;
		}

		/* Copies every entry. */
		entry = table.address;
		for (index = 0; index < count * MSIX_ENTRY_DWORDS; index++)
			state->msix_table[index] = entry[index];

		/* Lets the mapping go and remembers the capability. */
		drv_pci_device_unmap_bar(device, &table);
		state->msix_entries = count;
		state->msix_offset = (uint16_t)capability;
	}

	/* Succeeded: saved says the restore has something to write back. */
	state->saved = 1;
	return 0;
}

/*
 * Writes a saved configuration back after the function returned to D0,
 * and frees the copy of the MSI-X table.
 *
 * The PCI Express controls (device, link without ASPM, slot, root,
 * device 2, link 2), LTR, L1 PM Substates and the link control with ASPM
 * come first, then the
 * header from its last dword down with the command register last, then
 * MSI and MSI-X with their enables last.  A failed write is reported once
 * everything else was written.
 */
int
drv_pci_device_restore_state(
	struct drv_pci_device *device,
	struct drv_pci_saved_state *state)
{
	struct drv_pci_mapping table;
	volatile uint32_t *entry;
	unsigned capability;
	unsigned index;
	unsigned count;
	unsigned entries;
	unsigned type;
	uint16_t msi_control;
	uint16_t slot_control;
	int first_error;
	int error;

	/* Refuses no function, and a state nothing was saved in. */
	if (device == NULL || state == NULL || !state->saved)
		return EINVAL;

	/* Writes the PCI Express controls back. */
	first_error = 0;
	capability = state->pcie_offset;
	if (capability != 0) {
		/* The device control, and the link control without ASPM until the L1 PM Substates are back. */
		(void)drv_pci_device_config_write16(device, capability + PCIE_DEVICE_CONTROL, (uint16_t)state->pcie[SAVED_DEVICE_CONTROL]);
		(void)drv_pci_device_config_write16(device, capability + PCIE_LINK_CONTROL, (uint16_t)(state->pcie[SAVED_LINK_CONTROL] & ~PCIE_LINK_ASPM));

		/*
		 * The slot control of a port with a slot, only when it differs:
		 * a write is a command to the slot's controller.
		 */
		if ((state->pcie_flags & PCIE_SLOT_IMPLEMENTED) != 0) {
			slot_control = (uint16_t)read16_or_zero(device, capability + PCIE_SLOT_CONTROL);
			if (slot_control != (uint16_t)state->pcie[SAVED_SLOT_CONTROL])
				(void)drv_pci_device_config_write16(device, capability + PCIE_SLOT_CONTROL, (uint16_t)state->pcie[SAVED_SLOT_CONTROL]);
		}

		/* The root control of a root port and of an event collector. */
		type = state->pcie_flags & PCIE_TYPE_MASK;
		if (type == PCIE_TYPE_ROOT_PORT || type == PCIE_TYPE_EVENT_COLLECTOR)
			(void)drv_pci_device_config_write16(device, capability + PCIE_ROOT_CONTROL, (uint16_t)state->pcie[SAVED_ROOT_CONTROL]);

		/* The device 2 and link 2 controls. */
		(void)drv_pci_device_config_write16(device, capability + PCIE_DEVICE_CONTROL2, (uint16_t)state->pcie[SAVED_DEVICE_CONTROL2]);
		(void)drv_pci_device_config_write16(device, capability + PCIE_LINK_CONTROL2, (uint16_t)state->pcie[SAVED_LINK_CONTROL2]);
	}

	/* Writes the LTR latencies back. */
	if (state->ltr_offset != 0)
		(void)drv_pci_device_config_write32(device, state->ltr_offset + LTR_LATENCIES, state->ltr);

	/* Writes the L1 PM Substates controls back, the second first, since the first enables the substates. */
	if (state->l1ss_offset != 0) {
		(void)drv_pci_device_config_write32(device, state->l1ss_offset + L1SS_CONTROL2, state->l1ss[1]);
		(void)drv_pci_device_config_write32(device, state->l1ss_offset + L1SS_CONTROL1, state->l1ss[0]);
	}

	/* Enables ASPM again once the substates are programmed: the saved link control whole. */
	if (capability != 0 && (state->pcie[SAVED_LINK_CONTROL] & PCIE_LINK_ASPM) != 0)
		(void)drv_pci_device_config_write16(device, capability + PCIE_LINK_CONTROL, (uint16_t)state->pcie[SAVED_LINK_CONTROL]);

	/* Writes the header back from its last dword, leaving the identifiers and the class, which are read-only. */
	for (index = PCI_HEADER_DWORDS - 1U; index > 2U; index--) {
		/* Writes one dword; the first failure is kept. */
		error = drv_pci_device_config_write32(device, index * 4U, state->header[index]);
		if (error != 0 && first_error == 0)
			first_error = EIO;
	}

	/* Writes the command register last, alone, so that the status register's error bits are not cleared. */
	error = drv_pci_device_config_write16(device, PCI_COMMAND, (uint16_t)state->header[1]);
	if (error != 0 && first_error == 0)
		first_error = EIO;

	/* Writes MSI back: the address, the data and the masks, then the control with its enable. */
	capability = state->msi_offset;
	if (capability != 0) {
		/* Counts the dwords after the control, leaving the pending bits, which are read-only. */
		msi_control = (uint16_t)(state->msi[0] >> 16);
		count = msi_dwords(msi_control);
		if ((msi_control & MSI_PER_VECTOR_MASK) != 0)
			count--;

		/* Writes them. */
		for (index = 1; index < count; index++)
			(void)drv_pci_device_config_write32(device, capability + index * 4U, state->msi[index]);

		/* Writes the control. */
		(void)drv_pci_device_config_write16(device, capability + MSI_CONTROL, msi_control);
	}

	/* Writes MSI-X back: masked as a whole while the table is written, then the saved control. */
	capability = state->msix_offset;
	if (capability != 0 && state->msix_table != NULL) {
		/* Masks the function while its table is incomplete. */
		(void)drv_pci_device_config_write16(device, capability + MSIX_CONTROL, (uint16_t)(state->msix_control | MSIX_FUNCTION_MASK));

		/* Writes the table through a fresh mapping. */
		error = drv_pci_device_map_msix_table(device, &table, &entries);
		if (error == 0) {
			/* Writes as many entries as were saved and fit. */
			entry = table.address;
			if (entries > state->msix_entries)
				entries = state->msix_entries;

			/* Writes the entries' dwords. */
			for (index = 0; index < entries * MSIX_ENTRY_DWORDS; index++)
				entry[index] = state->msix_table[index];

			/* Lets the mapping go. */
			drv_pci_device_unmap_bar(device, &table);
		} else if (first_error == 0) {
			/* A table that cannot be mapped leaves the interrupts lost. */
			first_error = error;
		}

		/* Writes the saved control. */
		(void)drv_pci_device_config_write16(device, capability + MSIX_CONTROL, state->msix_control);
	}

	/* The copy of the table is no longer needed. */
	drv_pci_device_discard_state(state);

	/* Reports the first write that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: the function has its configuration again. */
	return 0;
}

/*
 * Frees what a saved state holds without writing it back.
 */
void
drv_pci_device_discard_state(
	struct drv_pci_saved_state *state)
{
	/* Nothing to free without a state. */
	if (state == NULL)
		return;

	/* Frees the copy of the MSI-X table; saved going to zero says nothing is held. */
	if (state->msix_table != NULL)
		kern_free(state->msix_table);

	/* Nothing is held any more. */
	state->msix_table = NULL;
	state->saved = 0;
}

/*
 * Puts a function in D0 or D3hot through its power management capability.
 *
 * The function is given its 10 milliseconds after entering or leaving
 * D3hot.  It reports ENOENT for a function without the capability, and
 * EIO when the function did not reach the state.
 */
int
drv_pci_device_set_power_state(
	struct drv_pci_device *device,
	unsigned state)
{
	unsigned capability;
	unsigned current;
	uint16_t control;
	int error;

	/* Refuses no function. */
	if (device == NULL)
		return EINVAL;

	/* Refuses a state other than D0 and D3hot. */
	if (state != DRV_PCI_D0 && state != DRV_PCI_D3_HOT)
		return EINVAL;

	/* Finds the power management capability. */
	error = drv_pci_device_find_capability(device, CAPABILITY_PM, &capability);
	if (error != 0)
		return error;

	/* Reads the current state. */
	error = drv_pci_device_config_read16(device, capability + PM_CONTROL, &control);
	if (error != 0)
		return EIO;

	/* A function already in the state has nothing to do. */
	current = control & PM_STATE_MASK;
	if (current == state)
		return 0;

	/* Writes the new state, leaving PME_Status alone (it clears when written as one). */
	control = (uint16_t)((control & ~(PM_STATE_MASK | PM_PME_STATUS)) | state);
	error = drv_pci_device_config_write16(device, capability + PM_CONTROL, control);
	if (error != 0)
		return EIO;

	/* Gives the function its time after D3hot. */
	sleep_ms(PM_D3HOT_DELAY_MS);

	/* Reads the state back. */
	error = drv_pci_device_config_read16(device, capability + PM_CONTROL, &control);
	if (error != 0)
		return EIO;

	/* Refuses a function that did not reach the state. */
	if ((control & PM_STATE_MASK) != state)
		return EIO;

	/* Succeeded: the function is in the state. */
	return 0;
}

/*
 * Asks, from a driver's suspend, that the function wake the system.
 *
 * The suspend sets PME_En and asks the platform to arm the wake once the
 * driver's suspend returned; the resume disarms it.  It reports EINVAL
 * outside a suspend.
 */
int
drv_pci_device_set_wake(
	struct drv_pci_device *device,
	bool wake)
{
	struct power_entry *entry;

	/* Refuses a call outside the suspend of a function. */
	if (power.entries == NULL || power.current >= power.count)
		return EINVAL;

	/* Refuses another function than the one being suspended. */
	entry = &power.entries[power.current];
	if (entry->device != device)
		return EINVAL;

	/* wake_wanted makes the suspend arm the wake after the driver's suspend. */
	entry->wake_wanted = 0;
	if (wake)
		entry->wake_wanted = 1;

	/* Succeeded: the suspend will arm the wake as asked. */
	return 0;
}

/*
 * Asks, from a driver's suspend, that the function stay in D0 while
 * the system is in S0 idle.  The configuration is still saved and
 * written back; neither the function nor the platform is powered
 * down.  It reports EINVAL outside a suspend.
 */
int
drv_pci_device_keep_d0(
	struct drv_pci_device *device,
	bool keep)
{
	struct power_entry *entry;

	/* Refuses a call outside the suspend of a function. */
	if (power.entries == NULL || power.current >= power.count)
		return EINVAL;

	/* Refuses another function than the one being suspended. */
	entry = &power.entries[power.current];
	if (entry->device != device)
		return EINVAL;

	/* keep_d0 makes the suspend leave the function and the platform powered. */
	entry->keep_d0 = 0;
	if (keep)
		entry->keep_d0 = 1;

	/* Succeeded: the suspend will keep the function in D0 as asked. */
	return 0;
}

/*
 * Writes a function's name for messages: "pci SSSS:BB:DD.F DRIVER".
 */
void
drv_pci_device_name(
	struct drv_pci_device *device,
	char *text,
	size_t size)
{
	struct drv_pci_address address;
	struct drv_pci_driver *driver;
	const char *driver_name;

	/* Nowhere to write is nothing to do. */
	if (text == NULL || size == 0)
		return;

	/* Takes the address and the driver's name, "-" without a driver. */
	drv_pci_device_address(device, &address);
	driver = drv_pci_device_driver(device);
	driver_name = "-";
	if (driver != NULL && driver->name != NULL)
		driver_name = driver->name;

	/* Writes the name. */
	(void)kern_snprintf(text, size, "pci %04x:%02x:%02x.%x %s", (unsigned)address.segment, (unsigned)address.bus, (unsigned)address.device, (unsigned)address.function, driver_name);
}

/*
 * Sets the platform's power operations for the functions.
 *
 * The platform code calls it once at start; NULL means the platform does
 * nothing beyond PCI power management.
 */
void
drv_pci_platform_power_set(
	const struct drv_pci_platform_power *platform)
{
	/* Keeps the operations for every later suspend and resume. */
	power.platform = platform;
}

/*
 * Suspends every function a driver is bound to, deepest in the bus tree
 * first, and saves the configuration of the functions without a driver
 * behind them.
 *
 * On a failure the functions already suspended are resumed again, failed
 * receives the function that failed (it may be NULL), and the failure is
 * reported: ENOTSUP for a driver without a suspend, or the error of the
 * step that failed.  It reports EBUSY while a suspend holds the functions.
 */
int
drv_pci_suspend_all(
	struct drv_pci_device **failed)
{
	struct power_collection collection;
	unsigned index;
	int error;

	/* No function has failed yet. */
	if (failed != NULL)
		*failed = NULL;

	/* Refuses a suspend inside a suspend. */
	if (power.suspended)
		return EBUSY;

	/* Counts the functions the suspend takes on. */
	kern_memset(&collection, 0, sizeof(collection));
	(void)drv_pci_foreach_device(count_visitor, &collection);

	/* Takes room for their entries; with none, the suspend has nothing to do. */
	collection.room = collection.count;
	collection.count = 0;
	if (collection.room != 0) {
		collection.entries = kern_malloc(collection.room * sizeof(struct power_entry));
		if (collection.entries == NULL)
			return ENOMEM;

		/* Starts every entry with nothing done. */
		kern_memset(collection.entries, 0, collection.room * sizeof(struct power_entry));
	}

	/* Collects them and orders them deepest first. */
	(void)drv_pci_foreach_device(collect_visitor, &collection);
	sort_deepest_first(collection.entries, collection.count);

	/* Publishes the entries, which drv_pci_device_set_wake() looks at. */
	power.entries = collection.entries;
	power.count = collection.count;

	/* Suspends each function in turn. */
	error = 0;
	for (index = 0; index < power.count; index++) {
		/* current names the function whose driver may ask for its wake. */
		power.current = index;

		/* Suspends it; a failure undoes this function's steps and stops here. */
		error = suspend_entry(&power.entries[index]);
		if (error != 0)
			break;
	}

	/* The drivers' suspends are over. */
	power.current = power.count;

	/* Undoes a suspend that failed: reports the function, and resumes the ones suspended before it, the last first. */
	if (index != power.count) {
		log_device("suspend", power.entries[index].device, error);
		if (failed != NULL)
			*failed = power.entries[index].device;

		/* Resumes each; a failure is logged and the rest still resumed. */
		while (index > 0) {
			index--;
			(void)resume_entry(&power.entries[index]);
		}

		/* Frees the entries; no suspend holds the functions any more. */
		kern_free(power.entries);
		power.entries = NULL;
		power.count = 0;
		return error;
	}

	/* Succeeded: every bound function sleeps until drv_pci_resume_all(). */
	power.suspended = 1;
	return 0;
}

/*
 * Resumes the functions drv_pci_suspend_all() suspended, the shallowest
 * first.
 *
 * Every function is resumed even after one failed; the first failure is
 * reported.  It reports EINVAL when no suspend holds the functions.
 */
int
drv_pci_resume_all(void)
{
	unsigned index;
	int first_error;
	int error;

	/* Refuses a resume without a suspend. */
	if (!power.suspended)
		return EINVAL;

	/* Resumes each function, parents before the functions behind them. */
	first_error = 0;
	index = power.count;
	while (index > 0) {
		/* Resumes one; the first failure is kept. */
		index--;
		error = resume_entry(&power.entries[index]);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* Frees the entries; no suspend holds the functions any more. */
	if (power.entries != NULL)
		kern_free(power.entries);

	/* No suspend holds the functions any more. */
	power.entries = NULL;
	power.count = 0;
	power.suspended = 0;

	/* Reports the first function that did not resume. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: every function runs again. */
	return 0;
}

/* Counts one function the suspend takes on. */
static int
count_visitor(
	struct drv_pci_device *device,
	void *argument)
{
	struct power_collection *collection;
	uint8_t save_only;
	int taken;

	/* Counts the function when the suspend takes it on. */
	collection = argument;
	taken = taken_on(device, &save_only);
	if (taken)
		collection->count++;

	/* Goes on with the next function. */
	return 0;
}

/* Collects one function the suspend takes on, with its depth. */
static int
collect_visitor(
	struct drv_pci_device *device,
	void *argument)
{
	struct power_collection *collection;
	struct power_entry *entry;
	uint8_t save_only;
	int taken;

	/* Skips a function the suspend leaves alone, and one beyond the room counted. */
	collection = argument;
	taken = taken_on(device, &save_only);
	if (!taken || collection->count == collection->room)
		return 0;

	/* Fills the entry. */
	entry = &collection->entries[collection->count];
	entry->device = device;
	entry->depth = device_depth(device);
	entry->save_only = save_only;
	collection->count++;

	/* Goes on with the next function. */
	return 0;
}

/*
 * Reports whether the suspend takes a function on: one with a driver is
 * suspended; one without is only saved when a function above it has a
 * driver, which may take the power away from below it.  save_only receives
 * which of the two it is.
 */
static int
taken_on(
	struct drv_pci_device *device,
	uint8_t *save_only)
{
	struct drv_pci_device *above;
	struct drv_pci_driver *driver;

	/* A function with a driver is suspended. */
	*save_only = 0;
	driver = drv_pci_device_driver(device);
	if (driver != NULL)
		return 1;

	/* Climbs the functions above it, looking for one with a driver. */
	above = drv_pci_device_upstream(device);
	while (above != NULL) {
		/* One with a driver may power down what is below it: the function is saved. */
		driver = drv_pci_device_driver(above);
		if (driver != NULL) {
			*save_only = 1;
			return 1;
		}

		/* One function further up. */
		above = drv_pci_device_upstream(above);
	}

	/* Nothing above it powers down: the function is left alone. */
	return 0;
}

/*
 * Reports how deep a function is: one on a root bus of the platform's host
 * bridge is at depth 1, and each function above it (a bridge, or the
 * function that presents its root bus) adds one.
 */
static unsigned
device_depth(
	struct drv_pci_device *device)
{
	struct drv_pci_device *above;
	unsigned depth;

	/* Climbs the functions above it. */
	depth = 1;
	above = drv_pci_device_upstream(device);
	while (above != NULL) {
		/* One function further up. */
		above = drv_pci_device_upstream(above);
		depth++;
	}

	/* Reports the depth. */
	return depth;
}

/* Orders the entries deepest first, keeping the bus order within one depth. */
static void
sort_deepest_first(
	struct power_entry *entries,
	unsigned count)
{
	struct power_entry moved;
	unsigned index;
	unsigned place;

	/* Inserts each entry after the deeper and equally deep ones before it. */
	for (index = 1; index < count; index++) {
		/* Moves the entry back past the shallower ones. */
		moved = entries[index];
		place = index;
		while (place > 0 && entries[place - 1U].depth < moved.depth) {
			entries[place] = entries[place - 1U];
			place--;
		}

		/* Puts it in its place. */
		entries[place] = moved;
	}
}

/*
 * Suspends one function: its driver, the save, the wake, D3hot and the
 * platform.  On a failure the steps already done are undone and the
 * failure is reported.
 */
static int
suspend_entry(
	struct power_entry *entry)
{
	struct drv_pci_device *device;
	struct drv_pci_driver *driver;
	int error;

	/* Only saves a function without a driver. */
	if (entry->save_only) {
		error = save_entry(entry);
		if (error != 0)
			return error;

		/* Succeeded: the function's configuration is kept. */
		return 0;
	}

	/* Refuses a driver that cannot suspend its function. */
	device = entry->device;
	driver = drv_pci_device_driver(device);
	if (driver == NULL || driver->suspend == NULL)
		return ENOTSUP;

	/* Lets the driver stop the function. */
	error = driver->suspend(device);
	if (error != 0)
		return error;

	/* driver_suspended makes the undo call the driver's resume. */
	entry->driver_suspended = 1;

	/* Saves the configuration the function loses in D3hot. */
	error = drv_pci_device_save_state(device, &entry->saved);
	if (error != 0) {
		(void)resume_entry(entry);
		return error;
	}

	/* Arms the wake the driver asked for: PME_En, then the platform. */
	if (entry->wake_wanted) {
		/* wake_armed makes the undo disarm it, also after a partial arming. */
		entry->wake_armed = 1;
		error = pme_set(device, true);
		if (error != 0) {
			(void)resume_entry(entry);
			return error;
		}

		/* Lets the platform arm its side of the wake. */
		error = platform_wake(device, true);
		if (error != 0) {
			(void)resume_entry(entry);
			return error;
		}
	}

	/* A function its driver keeps in D0 sleeps powered (drv_pci_device_keep_d0). */
	if (entry->keep_d0)
		return 0;

	/* Puts the function in D3hot; one without power management stays in D0. */
	error = drv_pci_device_set_power_state(device, DRV_PCI_D3_HOT);
	if (error == 0) {
		/* in_d3 makes the undo bring it back to D0. */
		entry->in_d3 = 1;
	} else if (error != ENOENT) {
		/* A function that did not reach D3hot is undone. */
		(void)resume_entry(entry);
		return error;
	}

	/* Lets the platform follow. */
	error = platform_state(device, DRV_PCI_D3_HOT);
	if (error != 0) {
		(void)resume_entry(entry);
		return error;
	}

	/* platform_d3 makes the undo give the platform's power back. */
	entry->platform_d3 = 1;

	/* Succeeded: the function sleeps. */
	return 0;
}

/*
 * Saves the configuration of a function without a driver, and for a port
 * whether its link was up.
 */
static int
save_entry(
	struct power_entry *entry)
{
	struct drv_pci_device *device;
	unsigned capability;
	uint32_t link_capabilities;
	uint16_t link_status;
	int is_port;
	int error;

	/* Saves the configuration. */
	device = entry->device;
	error = drv_pci_device_save_state(device, &entry->saved);
	if (error != 0)
		return error;

	/* Nothing more for a function that is not a port with a link below it. */
	is_port = port_with_link(&entry->saved);
	if (!is_port)
		return 0;

	/* Reads whether the port reports its link; one that cannot gives no more. */
	capability = entry->saved.pcie_offset;
	error = drv_pci_device_config_read32(device, capability + PCIE_LINK_CAPABILITIES, &link_capabilities);
	if (error != 0)
		return 0;
	if ((link_capabilities & PCIE_LINK_ACTIVE_REPORTING) == 0)
		return 0;

	/* Records whether the link is up. */
	entry->link_reported = 1;
	error = drv_pci_device_config_read16(device, capability + PCIE_LINK_STATUS, &link_status);
	if (error == 0 && (link_status & PCIE_LINK_ACTIVE) != 0)
		entry->link_active = 1;

	/* Succeeded: the configuration is saved. */
	return 0;
}

/*
 * Resumes one function as far as its suspend went: the platform, D0, the
 * configuration, the wake and the driver.  Every step is tried; the first
 * failure is reported.
 */
static int
resume_entry(
	struct power_entry *entry)
{
	struct drv_pci_device *device;
	struct drv_pci_driver *driver;
	int first_error;
	int error;

	/* Only writes back the configuration of a function without a driver. */
	if (entry->save_only) {
		error = restore_entry(entry);
		if (error != 0) {
			log_device("resume", entry->device, error);
			return error;
		}

		/* Succeeded: the function has its configuration again. */
		return 0;
	}

	/* Gives the platform's power back first, so that the function has power for D0. */
	device = entry->device;
	first_error = 0;
	if (entry->platform_d3) {
		error = platform_state(device, DRV_PCI_D0);
		if (error != 0)
			first_error = error;

		/* The platform's power is back. */
		entry->platform_d3 = 0;
	}

	/* Brings the function back to D0. */
	if (entry->in_d3) {
		error = drv_pci_device_set_power_state(device, DRV_PCI_D0);
		if (error != 0 && first_error == 0)
			first_error = error;

		/* The function is in D0. */
		entry->in_d3 = 0;
	}

	/* Writes the configuration back. */
	if (entry->saved.saved) {
		error = drv_pci_device_restore_state(device, &entry->saved);
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* Disarms the wake: the platform, then PME_En, which also clears PME_Status. */
	if (entry->wake_armed) {
		(void)platform_wake(device, false);
		(void)pme_set(device, false);
		entry->wake_armed = 0;
	}

	/* Lets the driver run the function again. */
	driver = drv_pci_device_driver(device);
	error = 0;
	if (entry->driver_suspended &&
	    driver != NULL &&
	    driver->resume != NULL)
		error = driver->resume(device);

	/* The function is no longer suspended. */
	entry->driver_suspended = 0;

	/* A driver whose function lost its state has it detached and attached again. */
	if (error == ESTALE) {
		kern_logf("pci: a function lost its state in the sleep; it is attached again\n");
		error = drv_pci_device_reprobe(device);
	}

	/* Keeps a resume or a new attach that failed. */
	if (error != 0 && first_error == 0)
		first_error = error;

	/* Reports the first step that failed. */
	if (first_error != 0) {
		log_device("resume", device, first_error);
		return first_error;
	}

	/* Succeeded: the function runs again. */
	return 0;
}

/*
 * Writes back the configuration of a function without a driver.  A port
 * that lost its bus numbers was reset with what is behind it, and its
 * link is given the time it needs.
 */
static int
restore_entry(
	struct power_entry *entry)
{
	struct drv_pci_device *device;
	uint32_t buses;
	int is_port;
	int lost;
	int error;

	/* Nothing to write back when the save did not happen. */
	device = entry->device;
	if (!entry->saved.saved)
		return 0;

	/* Reads whether a port still has its bus numbers. */
	lost = 0;
	is_port = port_with_link(&entry->saved);
	if (is_port) {
		error = drv_pci_device_config_read32(device, PCI_BRIDGE_BUSES_DWORD * 4U, &buses);
		if (error != 0 || buses != entry->saved.header[PCI_BRIDGE_BUSES_DWORD])
			lost = 1;
	}

	/* Writes the configuration back. */
	error = drv_pci_device_restore_state(device, &entry->saved);
	if (error != 0)
		return error;

	/* Waits for the link of a port that was reset. */
	if (lost)
		settle_link(entry);

	/* Succeeded: the function has its configuration again. */
	return 0;
}

/*
 * Waits after a port's configuration came back for its link: up to a
 * second for a link that was up to come up again, then the time the
 * device behind it needs; a port that cannot report its link gets that
 * time alone.  A link that stays down is logged; the functions behind it
 * then fail their own resume.
 */
static void
settle_link(
	struct power_entry *entry)
{
	struct drv_pci_address address;
	struct drv_pci_device *device;
	unsigned capability;
	unsigned poll;
	uint16_t link_status;
	int error;

	/* A port that cannot report its link, or whose link was down, is given the time alone. */
	device = entry->device;
	if (!entry->link_reported || !entry->link_active) {
		sleep_ms(LINK_SETTLE_MS);
		return;
	}

	/* Polls until the data link layer is up again. */
	capability = entry->saved.pcie_offset;
	for (poll = 0; poll < LINK_UP_POLLS; poll++) {
		/* Reads the link status. */
		link_status = 0;
		error = drv_pci_device_config_read16(device, capability + PCIE_LINK_STATUS, &link_status);
		if (error == 0 && (link_status & PCIE_LINK_ACTIVE) != 0)
			break;

		/* Lets the link train. */
		sleep_ms(LINK_UP_POLL_MS);
	}

	/* Logs a link that did not come back, or how long it took. */
	if (poll == LINK_UP_POLLS) {
		log_device("link of", device, ETIMEDOUT);
	} else {
		drv_pci_device_address(device, &address);
		kern_logf("pci: %04x:%02x:%02x.%x was reset; its link was up again after %u ms\n", (unsigned)address.segment, (unsigned)address.bus, (unsigned)address.device, (unsigned)address.function, poll * LINK_UP_POLL_MS);
	}

	/* Gives the device behind the port its time after the link is up. */
	sleep_ms(LINK_SETTLE_MS);
}

/*
 * Reports whether a saved function is a bridge whose secondary side is a
 * PCI Express link: a root port or a switch's downstream port.
 */
static int
port_with_link(
	const struct drv_pci_saved_state *state)
{
	unsigned layout;
	unsigned type;

	/* A function with another layout than a bridge's has no secondary side. */
	layout = (state->header[PCI_HEADER_TYPE_DWORD] >> PCI_HEADER_TYPE_SHIFT) & PCI_HEADER_LAYOUT;
	if (layout != PCI_HEADER_BRIDGE)
		return 0;

	/* A bridge without the PCI Express capability has no link to wait for. */
	if (state->pcie_offset == 0)
		return 0;

	/* Root ports and downstream ports have a link below them. */
	type = state->pcie_flags & PCIE_TYPE_MASK;
	if (type == PCIE_TYPE_ROOT_PORT)
		return 1;
	if (type == PCIE_TYPE_DOWNSTREAM)
		return 1;

	/* Any other bridge has none. */
	return 0;
}

/* Asks the platform to follow a function's power state; a function it does not know is no failure. */
static int
platform_state(
	struct drv_pci_device *device,
	unsigned state)
{
	const struct drv_pci_platform_power *platform;
	int error;

	/* A platform without the operation does nothing. */
	platform = power.platform;
	if (platform == NULL || platform->set_state == NULL)
		return 0;

	/* Asks it. */
	error = platform->set_state(platform->argument, device, state);
	if (error == ENOENT)
		return 0;

	/* Reports a platform that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the platform followed. */
	return 0;
}

/* Asks the platform to arm or disarm a function's wake; a function it does not know is no failure. */
static int
platform_wake(
	struct drv_pci_device *device,
	bool wake)
{
	const struct drv_pci_platform_power *platform;
	int error;

	/* A platform without the operation does nothing. */
	platform = power.platform;
	if (platform == NULL || platform->wake == NULL)
		return 0;

	/* Asks it. */
	error = platform->wake(platform->argument, device, wake);
	if (error == ENOENT)
		return 0;

	/* Reports a platform that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the platform armed or disarmed the wake. */
	return 0;
}

/*
 * Sets or clears PME_En, clearing PME_Status as well.  A function without
 * power management cannot signal PME, which is no failure: its wake goes
 * through the platform alone.
 */
static int
pme_set(
	struct drv_pci_device *device,
	bool enable)
{
	unsigned capability;
	uint16_t control;
	int error;

	/* A function without the capability has no PME. */
	error = drv_pci_device_find_capability(device, CAPABILITY_PM, &capability);
	if (error == ENOENT)
		return 0;

	/* Reports a capability list that cannot be read. */
	if (error != 0)
		return error;

	/* Reads the register. */
	error = drv_pci_device_config_read16(device, capability + PM_CONTROL, &control);
	if (error != 0)
		return EIO;

	/* Clears a PME already signalled, and sets or clears the enable. */
	control |= PM_PME_STATUS;
	control &= (uint16_t)~PM_PME_ENABLE;
	if (enable)
		control |= PM_PME_ENABLE;

	/* Writes it. */
	error = drv_pci_device_config_write16(device, capability + PM_CONTROL, control);
	if (error != 0)
		return EIO;

	/* Succeeded: the function signals PME as asked. */
	return 0;
}

/* Sleeps for some milliseconds. */
static void
sleep_ms(
	unsigned milliseconds)
{
	/* Sleeps until the tick the time ends at. */
	sched_sleep(sched_ticks() + kern_ms_to_ticks(milliseconds));
}

/* Reports how many dwords an MSI capability with this control register has. */
static unsigned
msi_dwords(
	uint16_t control)
{
	unsigned count;

	/* The control, the address and the data. */
	count = 3U;

	/* A 64-bit address takes a dword more. */
	if ((control & MSI_64BIT) != 0)
		count++;

	/* Per-vector masking adds the mask and the pending bits. */
	if ((control & MSI_PER_VECTOR_MASK) != 0)
		count += 2U;

	/* Reports the length. */
	return count;
}

/* Reads a 16-bit register, or zero when the read fails. */
static uint32_t
read16_or_zero(
	struct drv_pci_device *device,
	unsigned offset)
{
	uint16_t value;
	int error;

	/* Reads it. */
	value = 0;
	error = drv_pci_device_config_read16(device, offset, &value);
	if (error != 0)
		return 0;

	/* Succeeded: reports the register. */
	return value;
}

/* Logs a step of a function that failed, with the function's address. */
static void
log_device(
	const char *what,
	struct drv_pci_device *device,
	int error)
{
	struct drv_pci_address address;

	/* Reads the address and writes the line. */
	drv_pci_device_address(device, &address);
	kern_logf("pci: %s of %04x:%02x:%02x.%x failed (error %d)\n", what, (unsigned)address.segment, (unsigned)address.bus, (unsigned)address.device, (unsigned)address.function, error);
}
