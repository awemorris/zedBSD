/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI configuration access, device enable and MSI setup.
 *
 * This is the driver's side of pci_enable_device(), pci_set_master(),
 * pci_set_power_state() and pci_enable_msi():
 *
 *   - the device enable turns on decode per resource, the IO bit only for an
 *     IO BAR and the memory bit only for a memory BAR, as
 *     pci_enable_resources() does; it never sets both blindly.  It is
 *     reference counted: N enables need N disables, and the last disable is
 *     the one that clears decode.
 *   - the MSI setup is a resource, not a bit: it allocates an interrupt
 *     vector, builds the message from that vector, and only then sets the
 *     enable bit.  A failure part way frees what was taken and leaves MSI
 *     reported as disabled.
 *   - a capability is operated only when the capability walk found it; an
 *     absent power-management or MSI capability is a no-op or ENODEV, never a
 *     write to a guessed offset.
 *
 * The configuration access and the vector allocation are reached through an
 * operations table, so the same logic runs against a mock configuration space
 * in the host tests.
 */

#ifndef DRIVERS_GPU_I915_PCI_H
#define DRIVERS_GPU_I915_PCI_H

#include <stdint.h>

struct drv_pci_device;
struct i915_trace;

/* The configuration header registers the driver reads and writes. */
#define I915_PCI_COMMAND		0x04U
#define I915_PCI_STATUS			0x06U
#define I915_PCI_CAP_PTR		0x34U

/* BARs 0 to 5 sit at 0x10, 0x14, ..., 0x24. */
#define I915_PCI_BAR0			0x10U
#define I915_PCI_BAR_COUNT		6U

/* The command register bits. */
#define I915_PCI_CMD_IO			0x0001U
#define I915_PCI_CMD_MEMORY		0x0002U
#define I915_PCI_CMD_MASTER		0x0004U

/* The status register bit that says a capability list exists. */
#define I915_PCI_STATUS_CAP_LIST	0x0010U

/* The capability identifiers the driver looks for. */
#define I915_PCI_CAP_ID_PM		0x01U
#define I915_PCI_CAP_ID_MSI		0x05U
#define I915_PCI_CAP_ID_PCIE		0x10U
#define I915_PCI_CAP_ID_MSIX		0x11U

/* The power-management capability: the control and status register and its state field. */
#define I915_PCI_PM_CTRL		0x04U
#define I915_PCI_PM_STATE_MASK		0x0003U

/* The MSI capability: the message control register and its bits. */
#define I915_PCI_MSI_FLAGS		0x02U
#define I915_PCI_MSI_ENABLE		0x0001U
#define I915_PCI_MSI_64BIT		0x0080U

/* The MSI capability: the message address and the data offset for each address width. */
#define I915_PCI_MSI_ADDR_LO		0x04U
#define I915_PCI_MSI_DATA_32		0x08U
#define I915_PCI_MSI_DATA_64		0x0CU

/* The device power states the driver asks for. */
enum i915_pci_power {
	I915_PCI_D0 = 0,
	I915_PCI_D3HOT = 3
};

/* What kind of resource a BAR decodes. */
enum i915_pci_resource {
	I915_PCI_RES_NONE = 0,
	I915_PCI_RES_IO,
	I915_PCI_RES_MEM
};

/*
 * The bus end of configuration access and MSI vector allocation.
 *
 * The real device reaches the PCI configuration space and the HAL interrupt
 * allocator; a host test substitutes a configuration file of its own.
 */
struct i915_pci_ops {
	/* Read one configuration register of each width. */
	uint8_t (*read8)(void *context, unsigned offset);
	uint16_t (*read16)(void *context, unsigned offset);
	uint32_t (*read32)(void *context, unsigned offset);

	/* Write one configuration register of each width. */
	void (*write8)(void *context, unsigned offset, uint8_t value);
	void (*write16)(void *context, unsigned offset, uint16_t value);
	void (*write32)(void *context, unsigned offset, uint32_t value);

	/*
	 * Reserves an interrupt vector for MSI without attaching a handler.
	 * Returns 0 with the vector stored, or a positive errno.  May be NULL
	 * when the bus offers no MSI.
	 */
	int (*alloc_msi_vector)(void *context, int *vector);

	/* Releases a vector once the device has stopped sending it; may be NULL. */
	void (*free_msi_vector)(void *context, int vector);
};

/*
 * The PCI function of one device and the state its enable and MSI setup hold.
 *
 * It lives inside the device from the device start to the device stop.  It
 * is used only by the thread that runs the device start and stop.
 */
struct i915_pci {
	/* The bus access and the context it is given. */
	const struct i915_pci_ops *ops;
	void *context;

	/* Where the enable and MSI events are recorded; may be NULL. */
	struct i915_trace *trace;

	/*
	 * How many enables are outstanding.  The first enable turns decode on
	 * and the disable that returns the count to zero turns it off.
	 */
	int enable_count;

	/* The command register as it was before the first enable, for the restore. */
	uint16_t saved_command;

	/* Nonzero once saved_command holds the pre-enable value. */
	int saved_valid;

	/* Nonzero while the driver has bus mastering on. */
	int bus_master;

	/* Nonzero while MSI is set up and enabled. */
	int msi_enabled;

	/* The vector the MSI setup allocated, or -1 when none is held. */
	int msi_vector;
};

/*
 * The context the real PCI operations are given.
 *
 * The device start fills it before the PCI access is initialized and keeps
 * it until the MSI vector has been freed.
 */
struct i915_pci_context {
	/* The PCI device whose configuration space is reached. */
	struct drv_pci_device *pci;

	/* The logical interrupt the HAL allocated for MSI, or -1 when none. */
	int msi_irq;

	/* The canonical "PCI ssss:bb:dd.f" name the HAL knows the MSI source by. */
	char msi_source[17];
};

void drv_i915_pci_init(struct i915_pci *pci, const struct i915_pci_ops *ops, void *context, struct i915_trace *trace);
const struct i915_pci_ops *drv_i915_pci_device_ops(void);

uint8_t drv_i915_pci_read8(struct i915_pci *pci, unsigned offset);
uint16_t drv_i915_pci_read16(struct i915_pci *pci, unsigned offset);
uint32_t drv_i915_pci_read32(struct i915_pci *pci, unsigned offset);
void drv_i915_pci_write8(struct i915_pci *pci, unsigned offset, uint8_t value);
void drv_i915_pci_write16(struct i915_pci *pci, unsigned offset, uint16_t value);
void drv_i915_pci_write32(struct i915_pci *pci, unsigned offset, uint32_t value);

unsigned drv_i915_pci_find_capability(struct i915_pci *pci, uint8_t capability_id);
int drv_i915_pci_bar_kind(struct i915_pci *pci, unsigned index, uint64_t *address);
int drv_i915_pci_set_power_state(struct i915_pci *pci, enum i915_pci_power state);

int drv_i915_pci_enable_device(struct i915_pci *pci);
void drv_i915_pci_disable_device(struct i915_pci *pci);
int drv_i915_pci_is_enabled(const struct i915_pci *pci);
void drv_i915_pci_restore(struct i915_pci *pci);
void drv_i915_pci_set_bus_master(struct i915_pci *pci, int on);

int drv_i915_pci_setup_msi(struct i915_pci *pci);
void drv_i915_pci_teardown_msi(struct i915_pci *pci);
int drv_i915_pci_msi_enabled(const struct i915_pci *pci);

#endif
