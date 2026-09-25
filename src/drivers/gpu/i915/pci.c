/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI configuration access, device enable and MSI setup (see pci.h).
 *
 * The enable, decode, capability walk and MSI logic is written once over the
 * operations table.  The real operations at the end of the file are only the
 * boundary to the kernel: they reach the configuration space through
 * drv_pci_device_config_*() and reserve MSI vectors from the HAL.
 */

#include "i915.h"
#include "pci.h"
#include "trace.h"

#include <drivers/pci/pci.h>
#include <hal/hal.h>

#include <uapi/errno.h>
#include <stddef.h>

/*
 * The most capability entries the walk follows.
 *
 * A capability list that loops back on itself would otherwise keep the walk
 * running forever; 48 entries of at least 4 bytes cover the whole 192-byte
 * capability area.
 */
#define I915_PCI_CAP_WALK_MAX		48U

/* The first configuration offset past the standard header, where capabilities start. */
#define I915_PCI_CAP_AREA_START		0x40U

/* The low bits of a capability pointer that are reserved and must be ignored. */
#define I915_PCI_CAP_POINTER_MASK	0xFCU

/* The x86 architectural MSI address; the destination is fixed. */
#define I915_PCI_MSI_ARCH_ADDRESS	0xFEE00000U

/* The low bit of a BAR that marks an IO BAR. */
#define I915_PCI_BAR_IO			0x1U

/* The flag bits under an IO BAR's and a memory BAR's address. */
#define I915_PCI_BAR_IO_FLAGS		0x3U
#define I915_PCI_BAR_MEM_FLAGS		0xFU

static void i915_pci_note(struct i915_pci *pci, uint16_t op, const char *what, uint64_t argument0, uint64_t argument1);
static uint16_t i915_pci_wanted_decode(struct i915_pci *pci);
static void i915_pci_msi_write_message(struct i915_pci *pci, unsigned msi, uint16_t flags, uint32_t address, uint16_t data);
static void i915_pci_msi_set_enable(struct i915_pci *pci, unsigned msi, int on);
static uint8_t i915_pci_config_read8(void *context, unsigned offset);
static uint16_t i915_pci_config_read16(void *context, unsigned offset);
static uint32_t i915_pci_config_read32(void *context, unsigned offset);
static void i915_pci_config_write8(void *context, unsigned offset, uint8_t value);
static void i915_pci_config_write16(void *context, unsigned offset, uint16_t value);
static void i915_pci_config_write32(void *context, unsigned offset, uint32_t value);
static int i915_pci_alloc_msi_vector(void *context, int *vector);
static void i915_pci_free_msi_vector(void *context, int vector);
static void i915_pci_format_msi_source(char *source, const struct drv_pci_address *address);
static char i915_pci_hex_digit(unsigned value);

/*
 * Prepares the PCI access of one device with nothing enabled.
 */
void
drv_i915_pci_init(
	struct i915_pci *pci,
	const struct i915_pci_ops *ops,
	void *context,
	struct i915_trace *trace)
{
	/* Binds the bus access. */
	pci->ops = ops;
	pci->context = context;
	pci->trace = trace;

	/* Starts with no enable outstanding and no command register saved. */
	pci->enable_count = 0;
	pci->saved_command = 0U;
	pci->saved_valid = 0;
	pci->bus_master = 0;

	/* Starts with no MSI vector held. */
	pci->msi_enabled = 0;
	pci->msi_vector = -1;
}

/*
 * Returns the operations that reach a real PCI device.
 *
 * The context those operations receive is a struct i915_pci_context.  The
 * vector allocation reserves a HAL vector with no handler; the handler is
 * attached later, when the interrupt code installs it, and the vector is
 * freed only after the device has stopped sending it.
 */
const struct i915_pci_ops *
drv_i915_pci_device_ops(void)
{
	static const struct i915_pci_ops ops = {
		i915_pci_config_read8,
		i915_pci_config_read16,
		i915_pci_config_read32,
		i915_pci_config_write8,
		i915_pci_config_write16,
		i915_pci_config_write32,
		i915_pci_alloc_msi_vector,
		i915_pci_free_msi_vector
	};

	/* Succeeded: the operations reach the PCI configuration space and the HAL. */
	return &ops;
}

/*
 * Reads one byte of configuration space.
 */
uint8_t
drv_i915_pci_read8(
	struct i915_pci *pci,
	unsigned offset)
{
	uint8_t value;

	/* Reads the register through the bus access. */
	value = pci->ops->read8(pci->context, offset);

	/* Succeeded: reports the register value. */
	return value;
}

/*
 * Reads one 16-bit word of configuration space.
 */
uint16_t
drv_i915_pci_read16(
	struct i915_pci *pci,
	unsigned offset)
{
	uint16_t value;

	/* Reads the register through the bus access. */
	value = pci->ops->read16(pci->context, offset);

	/* Succeeded: reports the register value. */
	return value;
}

/*
 * Reads one 32-bit word of configuration space.
 */
uint32_t
drv_i915_pci_read32(
	struct i915_pci *pci,
	unsigned offset)
{
	uint32_t value;

	/* Reads the register through the bus access. */
	value = pci->ops->read32(pci->context, offset);

	/* Succeeded: reports the register value. */
	return value;
}

/*
 * Writes one byte of configuration space.
 */
void
drv_i915_pci_write8(
	struct i915_pci *pci,
	unsigned offset,
	uint8_t value)
{
	/* Writes the register through the bus access. */
	pci->ops->write8(pci->context, offset, value);
}

/*
 * Writes one 16-bit word of configuration space.
 */
void
drv_i915_pci_write16(
	struct i915_pci *pci,
	unsigned offset,
	uint16_t value)
{
	/* Writes the register through the bus access. */
	pci->ops->write16(pci->context, offset, value);
}

/*
 * Writes one 32-bit word of configuration space.
 */
void
drv_i915_pci_write32(
	struct i915_pci *pci,
	unsigned offset,
	uint32_t value)
{
	/* Writes the register through the bus access. */
	pci->ops->write32(pci->context, offset, value);
}

/*
 * Finds a capability in the capability list.
 *
 * Returns the capability's configuration offset, or 0 when the device has no
 * capability list or the capability is not in it.
 */
unsigned
drv_i915_pci_find_capability(
	struct i915_pci *pci,
	uint8_t capability_id)
{
	uint16_t status;
	unsigned offset;
	unsigned guard;
	uint8_t id;
	uint8_t next;

	/* A device without the capability-list status bit has no list to walk. */
	status = pci->ops->read16(pci->context, I915_PCI_STATUS);
	if ((status & I915_PCI_STATUS_CAP_LIST) == 0U)
		return 0U;

	/* Walks the list from the capability pointer, a bounded number of entries. */
	offset = pci->ops->read8(pci->context, I915_PCI_CAP_PTR) & I915_PCI_CAP_POINTER_MASK;
	for (guard = 0U; guard < I915_PCI_CAP_WALK_MAX; guard++) {
		/* A pointer into the standard header ends the list. */
		if (offset < I915_PCI_CAP_AREA_START)
			break;

		/* Reads the entry's identifier and its link to the next entry. */
		id = pci->ops->read8(pci->context, offset + 0U);
		next = pci->ops->read8(pci->context, offset + 1U);

		/* Succeeded: this entry is the capability asked for. */
		if (id == capability_id)
			return offset;

		offset = next & I915_PCI_CAP_POINTER_MASK;
	}

	/* The capability is not in the list. */
	return 0U;
}

/*
 * Classifies one BAR as unassigned, IO or memory.
 *
 * Returns an enum i915_pci_resource and stores the BAR's base address in
 * *address, or 0 when the BAR is unassigned; address may be NULL.
 */
int
drv_i915_pci_bar_kind(
	struct i915_pci *pci,
	unsigned index,
	uint64_t *address)
{
	uint32_t bar;

	/* Starts the address as unassigned. */
	if (address != NULL)
		*address = 0U;

	/* A BAR number past the last BAR names no resource. */
	if (index >= I915_PCI_BAR_COUNT)
		return I915_PCI_RES_NONE;

	/* A BAR that reads zero is unassigned. */
	bar = pci->ops->read32(pci->context, I915_PCI_BAR0 + index * 4U);
	if (bar == 0U)
		return I915_PCI_RES_NONE;

	/* An IO BAR keeps its address above the two flag bits. */
	if ((bar & I915_PCI_BAR_IO) != 0U) {
		if (address != NULL)
			*address = bar & ~I915_PCI_BAR_IO_FLAGS;
		return I915_PCI_RES_IO;
	}

	/* A memory BAR keeps its address above the four flag bits. */
	if (address != NULL)
		*address = bar & ~I915_PCI_BAR_MEM_FLAGS;

	/* Succeeded: the BAR decodes memory. */
	return I915_PCI_RES_MEM;
}

/*
 * Moves the device to a power state through its power-management capability.
 *
 * A device without the capability is left alone and the call succeeds, as
 * pci_set_power_state() does.  Returns 0.
 */
int
drv_i915_pci_set_power_state(
	struct i915_pci *pci,
	enum i915_pci_power state)
{
	unsigned pm;
	uint16_t pmcsr;

	/* A device without power management has no state to change. */
	pm = drv_i915_pci_find_capability(pci, I915_PCI_CAP_ID_PM);
	if (pm == 0U) {
		i915_pci_note(pci, I915_TRACE_NOTE, "pci_set_power_no_pm_cap", (uint64_t)state, 0U);
		return 0;
	}

	/* Replaces only the power-state field of the control and status register. */
	pmcsr = pci->ops->read16(pci->context, pm + I915_PCI_PM_CTRL);
	pmcsr = (uint16_t)((pmcsr & ~I915_PCI_PM_STATE_MASK) | ((uint16_t)state & I915_PCI_PM_STATE_MASK));
	pci->ops->write16(pci->context, pm + I915_PCI_PM_CTRL, pmcsr);
	i915_pci_note(pci, I915_TRACE_NOTE, "pci_set_power_state", (uint64_t)state, pm);

	/* Succeeded: the device was asked for the state. */
	return 0;
}

/*
 * Takes one enable reference, powering the device and turning decode on at the first.
 *
 * The first enable moves the device to D0, saves the command register for
 * the restore, and turns on decode only for the resource kinds the device's
 * BARs have.  A later enable only counts.  Returns 0.
 */
int
drv_i915_pci_enable_device(
	struct i915_pci *pci)
{
	uint16_t command;
	uint16_t wanted;

	/*
	 * A reference already held means the hardware is enabled: the new
	 * reference is only counted and the hardware is not touched again.
	 */
	i915_pci_note(pci, I915_TRACE_ENTRY, "pci_enable_device", (uint64_t)pci->enable_count, 0U);
	pci->enable_count++;
	if (pci->enable_count > 1) {
		i915_pci_note(pci, I915_TRACE_NOTE, "pci_enable_refcount", (uint64_t)pci->enable_count, 0U);
		return 0;
	}

	/* Powers the device up; a device without power management is already in D0. */
	(void)drv_i915_pci_set_power_state(pci, I915_PCI_D0);

	/* Saves the command register as the firmware left it, once, for the restore. */
	command = pci->ops->read16(pci->context, I915_PCI_COMMAND);
	if (pci->saved_valid == 0) {
		pci->saved_command = command;
		pci->saved_valid = 1;
	}

	/* Turns on decode only for the resources present, never a blanket IO and memory. */
	wanted = i915_pci_wanted_decode(pci);
	command = (uint16_t)(command | wanted);
	pci->ops->write16(pci->context, I915_PCI_COMMAND, command);

	i915_pci_note(pci, I915_TRACE_ACQUIRE, "pci_device", wanted, command);
	i915_pci_note(pci, I915_TRACE_EXIT, "pci_enable_device", 0U, 0U);

	/* Succeeded: the device is powered and decodes its resources. */
	return 0;
}

/*
 * Drops one enable reference; the last one turns decode off.
 */
void
drv_i915_pci_disable_device(
	struct i915_pci *pci)
{
	uint16_t command;

	/* A disable without an enable has nothing to drop. */
	if (pci->enable_count == 0)
		return;

	/* Other holders remain, so decode must stay on. */
	pci->enable_count--;
	if (pci->enable_count > 0) {
		i915_pci_note(pci, I915_TRACE_NOTE, "pci_disable_refcount", (uint64_t)pci->enable_count, 0U);
		return;
	}

	/* Turns IO and memory decode off. */
	command = pci->ops->read16(pci->context, I915_PCI_COMMAND);
	command = (uint16_t)(command & ~(I915_PCI_CMD_IO | I915_PCI_CMD_MEMORY));
	pci->ops->write16(pci->context, I915_PCI_COMMAND, command);
	i915_pci_note(pci, I915_TRACE_RELEASE, "pci_device", 0U, command);
}

/*
 * Reports nonzero while at least one enable reference is held.
 */
int
drv_i915_pci_is_enabled(
	const struct i915_pci *pci)
{
	/* A held reference means the device is enabled. */
	if (pci->enable_count > 0)
		return 1;

	/* No reference is held. */
	return 0;
}

/*
 * Puts the command register back as it was before the first enable.
 *
 * Every enable reference is dropped with it, and the bus-master state follows
 * the restored register.
 */
void
drv_i915_pci_restore(
	struct i915_pci *pci)
{
	/* A device that was never enabled has nothing saved to restore. */
	if (pci->saved_valid == 0)
		return;

	/* Writes the saved command register back. */
	pci->ops->write16(pci->context, I915_PCI_COMMAND, pci->saved_command);
	i915_pci_note(pci, I915_TRACE_RELEASE, "pci_device_restore", 0U, pci->saved_command);

	/* The restore ends every enable reference. */
	pci->enable_count = 0;

	/* Bus mastering is whatever the restored register says. */
	if ((pci->saved_command & I915_PCI_CMD_MASTER) != 0U) {
		pci->bus_master = 1;
	} else {
		pci->bus_master = 0;
	}
}

/*
 * Turns bus mastering on (nonzero) or off (zero).
 */
void
drv_i915_pci_set_bus_master(
	struct i915_pci *pci,
	int on)
{
	uint16_t command;

	/* Changes only the bus-master bit of the command register. */
	command = pci->ops->read16(pci->context, I915_PCI_COMMAND);
	if (on != 0) {
		command = (uint16_t)(command | I915_PCI_CMD_MASTER);
	} else {
		command = (uint16_t)(command & ~I915_PCI_CMD_MASTER);
	}
	pci->ops->write16(pci->context, I915_PCI_COMMAND, command);

	/* Records the new state as taken or given back. */
	if (on != 0) {
		pci->bus_master = 1;
		i915_pci_note(pci, I915_TRACE_ACQUIRE, "pci_bus_master", (uint64_t)on, command);
	} else {
		pci->bus_master = 0;
		i915_pci_note(pci, I915_TRACE_RELEASE, "pci_bus_master", (uint64_t)on, command);
	}
}

/*
 * Sets up MSI: allocates a vector, programs the message from it, then enables.
 *
 * Returns 0; ENODEV when the device has no MSI capability or the bus offers
 * no vectors, which the caller answers by using INTx; or another errno when
 * the vector could not be allocated or formed, in which case nothing is left
 * allocated or enabled.  Installing the interrupt handler is a separate step.
 */
int
drv_i915_pci_setup_msi(
	struct i915_pci *pci)
{
	unsigned msi;
	uint16_t flags;
	uint16_t data;
	int vector;
	int error;

	/* A device without MSI keeps using INTx; that is not a failure of the device. */
	msi = drv_i915_pci_find_capability(pci, I915_PCI_CAP_ID_MSI);
	if (msi == 0U) {
		i915_pci_note(pci, I915_TRACE_NOTE, "pci_msi_absent", 0U, 0U);
		return ENODEV;
	}

	/* A bus that cannot allocate vectors cannot deliver MSI. */
	if (pci->ops->alloc_msi_vector == NULL)
		return ENODEV;

	/* Reserves the interrupt vector; on failure nothing is held. */
	vector = -1;
	error = pci->ops->alloc_msi_vector(pci->context, &vector);
	if (error != 0) {
		i915_pci_note(pci, I915_TRACE_FAIL, "pci_msi_alloc", (uint64_t)error, 0U);
		return error;
	}

	/*
	 * Builds the message data from the vector just allocated, never from a
	 * fixed copy.  A zero vector cannot form a message, so it is released
	 * and the setup fails.
	 */
	flags = pci->ops->read16(pci->context, msi + I915_PCI_MSI_FLAGS);
	data = (uint16_t)(vector & 0xFF);
	if (data == 0U) {
		if (pci->ops->free_msi_vector != NULL)
			pci->ops->free_msi_vector(pci->context, vector);
		i915_pci_note(pci, I915_TRACE_FAIL, "pci_msi_message", 0U, 0U);
		return EIO;
	}

	/* Programs the message address and data into the capability. */
	i915_pci_msi_write_message(pci, msi, flags, I915_PCI_MSI_ARCH_ADDRESS, data);

	/* Enables MSI only now that the vector and the message are in place. */
	i915_pci_msi_set_enable(pci, msi, 1);
	pci->msi_enabled = 1;
	pci->msi_vector = vector;
	i915_pci_note(pci, I915_TRACE_ACQUIRE, "pci_msi", (uint64_t)vector, msi);

	/* Succeeded: the device signals interrupts through the allocated vector. */
	return 0;
}

/*
 * Stops MSI and frees its vector.
 *
 * The device is told to stop sending first, so the vector is never freed
 * while it can still arrive.
 */
void
drv_i915_pci_teardown_msi(
	struct i915_pci *pci)
{
	unsigned msi;

	/* Nothing was set up. */
	if (pci->msi_enabled == 0)
		return;

	/* Stops the device from sending MSI. */
	msi = drv_i915_pci_find_capability(pci, I915_PCI_CAP_ID_MSI);
	if (msi != 0U)
		i915_pci_msi_set_enable(pci, msi, 0);

	/* Frees the vector now that its source has stopped. */
	if (pci->ops->free_msi_vector != NULL && pci->msi_vector >= 0)
		pci->ops->free_msi_vector(pci->context, pci->msi_vector);
	i915_pci_note(pci, I915_TRACE_RELEASE, "pci_msi", (uint64_t)pci->msi_vector, 0U);

	/* No MSI vector is held any more. */
	pci->msi_enabled = 0;
	pci->msi_vector = -1;
}

/*
 * Reports nonzero while MSI is set up and enabled.
 */
int
drv_i915_pci_msi_enabled(
	const struct i915_pci *pci)
{
	/* Reports the MSI state. */
	return pci->msi_enabled;
}

/* Records a PCI event when the device has a trace. */
static void
i915_pci_note(
	struct i915_pci *pci,
	uint16_t op,
	const char *what,
	uint64_t argument0,
	uint64_t argument1)
{
	/* A PCI access without a trace records nothing. */
	if (pci->trace == NULL)
		return;

	drv_i915_trace_record(pci->trace, 0U, op, what, argument0, argument1);
}

/* Collects the decode bits for the resource kinds the device's BARs actually have. */
static uint16_t
i915_pci_wanted_decode(
	struct i915_pci *pci)
{
	uint16_t wanted;
	uint64_t address;
	unsigned index;
	int kind;

	/* An assigned IO BAR wants IO decode and an assigned memory BAR wants memory decode. */
	wanted = 0U;
	for (index = 0U; index < I915_PCI_BAR_COUNT; index++) {
		kind = drv_i915_pci_bar_kind(pci, index, &address);
		if (kind == I915_PCI_RES_IO && address != 0U) {
			wanted |= I915_PCI_CMD_IO;
		} else if (kind == I915_PCI_RES_MEM && address != 0U) {
			wanted |= I915_PCI_CMD_MEMORY;
		}
	}

	/* Succeeded: reports the decode bits the resources need. */
	return wanted;
}

/* Programs the MSI message address and data for the capability's address width. */
static void
i915_pci_msi_write_message(
	struct i915_pci *pci,
	unsigned msi,
	uint16_t flags,
	uint32_t address,
	uint16_t data)
{
	unsigned data_offset;

	/* A 64-bit capable function keeps its data one word further on. */
	data_offset = I915_PCI_MSI_DATA_32;
	if ((flags & I915_PCI_MSI_64BIT) != 0U)
		data_offset = I915_PCI_MSI_DATA_64;

	/* Writes the address, with a zero upper half on a 64-bit capable function. */
	pci->ops->write32(pci->context, msi + I915_PCI_MSI_ADDR_LO, address);
	if ((flags & I915_PCI_MSI_64BIT) != 0U)
		pci->ops->write32(pci->context, msi + I915_PCI_MSI_ADDR_LO + 4U, 0U);

	/* Writes the data the interrupt arrives with. */
	pci->ops->write16(pci->context, msi + data_offset, data);
}

/* Sets or clears the MSI enable bit in the message control register. */
static void
i915_pci_msi_set_enable(
	struct i915_pci *pci,
	unsigned msi,
	int on)
{
	uint16_t flags;

	/* Changes only the enable bit. */
	flags = pci->ops->read16(pci->context, msi + I915_PCI_MSI_FLAGS);
	if (on != 0) {
		flags = (uint16_t)(flags | I915_PCI_MSI_ENABLE);
	} else {
		flags = (uint16_t)(flags & ~I915_PCI_MSI_ENABLE);
	}
	pci->ops->write16(pci->context, msi + I915_PCI_MSI_FLAGS, flags);
}

/* Reads one configuration byte; a failed read reports all-ones. */
static uint8_t
i915_pci_config_read8(
	void *context,
	unsigned offset)
{
	struct i915_pci_context *device;
	uint8_t value;

	/* A failed read leaves the all-ones value a missing device returns. */
	device = context;
	value = 0xFFU;
	(void)drv_pci_device_config_read8(device->pci, offset, &value);

	/* Succeeded: reports the register value. */
	return value;
}

/* Reads one configuration word; a failed read reports all-ones. */
static uint16_t
i915_pci_config_read16(
	void *context,
	unsigned offset)
{
	struct i915_pci_context *device;
	uint16_t value;

	/* A failed read leaves the all-ones value a missing device returns. */
	device = context;
	value = 0xFFFFU;
	(void)drv_pci_device_config_read16(device->pci, offset, &value);

	/* Succeeded: reports the register value. */
	return value;
}

/* Reads one configuration double word; a failed read reports all-ones. */
static uint32_t
i915_pci_config_read32(
	void *context,
	unsigned offset)
{
	struct i915_pci_context *device;
	uint32_t value;

	/* A failed read leaves the all-ones value a missing device returns. */
	device = context;
	value = 0xFFFFFFFFU;
	(void)drv_pci_device_config_read32(device->pci, offset, &value);

	/* Succeeded: reports the register value. */
	return value;
}

/* Writes one configuration byte; a failed write is dropped. */
static void
i915_pci_config_write8(
	void *context,
	unsigned offset,
	uint8_t value)
{
	struct i915_pci_context *device;

	/* Writes the register; the bus reports nothing the caller could act on. */
	device = context;
	(void)drv_pci_device_config_write8(device->pci, offset, value);
}

/* Writes one configuration word; a failed write is dropped. */
static void
i915_pci_config_write16(
	void *context,
	unsigned offset,
	uint16_t value)
{
	struct i915_pci_context *device;

	/* Writes the register; the bus reports nothing the caller could act on. */
	device = context;
	(void)drv_pci_device_config_write16(device->pci, offset, value);
}

/* Writes one configuration double word; a failed write is dropped. */
static void
i915_pci_config_write32(
	void *context,
	unsigned offset,
	uint32_t value)
{
	struct i915_pci_context *device;

	/* Writes the register; the bus reports nothing the caller could act on. */
	device = context;
	(void)drv_pci_device_config_write32(device->pci, offset, value);
}

/* Reserves a HAL MSI vector with no handler and reports its message data. */
static int
i915_pci_alloc_msi_vector(
	void *context,
	int *vector)
{
	struct i915_pci_context *device;
	struct drv_pci_address address;
	paddr_t message_address;
	uint32_t message_event;
	int irq;
	int status;

	/* Names the MSI source by the device's PCI address, as the HAL expects. */
	device = context;
	drv_pci_device_address(device->pci, &address);
	i915_pci_format_msi_source(device->msi_source, &address);

	/*
	 * Reserves the vector, its routing and its message; the handler is
	 * attached later, when the interrupt code installs it.
	 *
	 * XXX: the HAL's reason for a failure is not passed on; the caller
	 * sees EIO.
	 */
	irq = -1;
	message_address = 0;
	message_event = 0U;
	status = hal_irq_alloc_msi(device->msi_source, &irq, &message_address, &message_event);
	if (status != HAL_OK)
		return EIO;

	/* Keeps the logical interrupt for the free and the handler attach. */
	device->msi_irq = irq;

	/* The MSI message data is built from the vector this reports. */
	*vector = (int)(message_event & 0xFFU);

	/* Succeeded: the vector is reserved with no handler. */
	return 0;
}

/* Frees the HAL MSI vector once the device has stopped sending it. */
static void
i915_pci_free_msi_vector(
	void *context,
	int vector)
{
	struct i915_pci_context *device;

	UNUSED_PARAMETER(vector);

	/*
	 * The caller has already cleared the MSI enable bit, so the source is
	 * stopped and the logical interrupt can be given back.
	 */
	device = context;
	if (device->msi_irq >= 0) {
		(void)hal_irq_free_msi(device->msi_irq);
		device->msi_irq = -1;
	}
}

/* Formats the canonical "PCI ssss:bb:dd.f" source name of a PCI function. */
static void
i915_pci_format_msi_source(
	char *source,
	const struct drv_pci_address *address)
{
	/* The bus the name belongs to. */
	source[0] = 'P';
	source[1] = 'C';
	source[2] = 'I';
	source[3] = ' ';

	/* The segment, four hexadecimal digits. */
	source[4] = i915_pci_hex_digit((unsigned)address->segment >> 12);
	source[5] = i915_pci_hex_digit((unsigned)address->segment >> 8);
	source[6] = i915_pci_hex_digit((unsigned)address->segment >> 4);
	source[7] = i915_pci_hex_digit((unsigned)address->segment);
	source[8] = ':';

	/* The bus, two hexadecimal digits. */
	source[9] = i915_pci_hex_digit((unsigned)address->bus >> 4);
	source[10] = i915_pci_hex_digit((unsigned)address->bus);
	source[11] = ':';

	/* The device, two hexadecimal digits. */
	source[12] = i915_pci_hex_digit((unsigned)address->device >> 4);
	source[13] = i915_pci_hex_digit((unsigned)address->device);
	source[14] = '.';

	/* The function, one hexadecimal digit, and the end of the name. */
	source[15] = i915_pci_hex_digit((unsigned)address->function);
	source[16] = '\0';
}

/* Names the low four bits of a value as one lower-case hexadecimal digit. */
static char
i915_pci_hex_digit(
	unsigned value)
{
	static const char digits[] = "0123456789abcdef";

	/* Succeeded: only the low four bits are named. */
	return digits[value & 0xFU];
}
