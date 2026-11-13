/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Intel Volume Management Device (VMD) driver.
 *
 * With the firmware's storage mode set to RAID, the chipset hides the
 * PCI Express root ports of the NVMe disks behind a VMD function and
 * shows their configuration space through the VMD's first BAR instead.
 * This driver binds to that function, numbers the buses behind it and
 * places their BARs in the VMD's memory windows (pci-vmd-plan.c), and
 * presents them to the PCI core as a root bus of their own, on which the
 * NVMe driver binds as on any other.
 *
 * The children's message interrupts cannot reach the processor directly:
 * the VMD catches each one and raises one of its own MSI-X vectors
 * instead.  This driver takes one such vector, gives every child's
 * message the address that selects it, and runs every child's handler
 * when it fires.
 *
 * Across S0 idle the PCI core suspends the functions behind the VMD
 * first, saves the configuration of the root ports (they have no driver),
 * and then puts the VMD itself in D3hot; it brings them back in the
 * reverse order.  The core saves and writes back the VMD's header and
 * MSI-X table; this driver keeps the one register of its own a reset
 * loses, the configuration register (the bus range and the bypass bit),
 * and checks on resume that the buses and the windows are where the
 * functions behind it were placed.
 */

#include <drivers/pci/pci-vmd.h>
#include <drivers/pci/pci.h>
#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <stddef.h>

/*
 * The VMD's own registers: the BARs that hold the configuration space
 * behind it (CFGBAR) and its two memory windows (MEMBAR1, MEMBAR2), the
 * capability and configuration registers, and the configuration bit that
 * lets the children's messages pass without being caught.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c (VMD_CAP, VMD_CONFIG,
 * VMD_BYPASS_MSI, and the BARs vmd_attach maps); Linux 6.12.5
 * drivers/pci/controller/vmd.c (VMD_CFGBAR, VMD_MEMBAR1, VMD_MEMBAR2).
 */
#define VMD_BAR_CONFIG		0U
#define VMD_BAR_MEMORY1		2U
#define VMD_BAR_MEMORY2		4U
#define VMD_REG_CAPABILITY	0x40U
#define VMD_REG_CONFIGURATION	0x44U
#define VMD_CONFIGURATION_BYPASS	0x0002U

/*
 * The head of MEMBAR2 the VMD keeps for its own MSI-X table and pending
 * bits; the children's window starts after it.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c (vmd_attach, the
 * second memory region starts at MEMBAR2 + 0x2000); Linux 6.12.5
 * drivers/pci/controller/vmd.c (membar2_offset).
 */
#define VMD_MEMBAR2_HEAD	UINT64_C(0x2000)

/* The smallest CFGBAR: one bus, 1 MiB.  A bus takes 1 MiB of it. */
#define VMD_CFGBAR_MIN		UINT64_C(0x100000)
#define VMD_CFGBAR_BUS_SHIFT	20U

/*
 * The first of the VMD's MSI-X vectors the children's messages may be
 * caught into: the client VMDs keep vector 0 for themselves.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c (the VECTOR_OFFSET
 * flag of the 467f row, vmd_fist_vector); Linux 6.12.5
 * drivers/pci/controller/vmd.c (VMD_FEAT_OFFSET_FIRST_VECTOR).
 */
#define VMD_CHILD_VECTOR	1U

/*
 * The vendor-specific capability a hypervisor gives an emulated VMD, and
 * the signature ("SHDW") that says its windows lie elsewhere in the host.
 *
 * Values referenced: Linux 6.12.5 drivers/pci/controller/vmd.c
 * (vmd_get_phys_offsets, the "SHDW" signature at capability + 4).
 */
#define VMD_CAP_VENDOR		0x09U
#define VMD_SHADOW_OFFSET	4U
#define VMD_SHADOW_SIGNATURE	0x53484457U

/*
 * The MSI-X capability, its table size field, and one table entry: its
 * size and the mask bit of its vector control word.
 *
 * Values referenced: PCI Local Bus Specification Revision 3.0, section
 * 6.8.2 (the MSI-X capability and table entry).
 */
#define VMD_CAP_MSIX		0x11U
#define VMD_MSIX_ENTRY_WORDS	4U
#define VMD_MSIX_CONTROL_WORD	3U
#define VMD_MSIX_ENTRY_MASK	0x00000001U

/* Marks a parameter an operation of the table does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* The segment of the first VMD's bus; later ones count down from it. */
#define VMD_SEGMENT_FIRST	0xffffU

/* The most VMD functions one machine has. */
#define VMD_INSTANCES_MAX	4U

/* How long a removed message waits for an interrupt still running it: 1000 polls of 1 ms. */
#define VMD_DRAIN_POLLS		1000U
#define VMD_DRAIN_POLL_US	1000U

/*
 * The boot parameters that stop the driver at a stage, to find on a
 * machine without a serial console which stage stops its boot: leave every
 * VMD alone; only read and log what the firmware left behind it; arrange
 * everything but publish no bus.
 */
#define VMD_TOKEN_OFF		"pci.vmd=off"
#define VMD_TOKEN_READ		"pci.vmd=read"
#define VMD_TOKEN_NOSCAN	"pci.vmd=noscan"

/* The slots of a bus, the functions of a slot, and a type 1 header's kind. */
#define VMD_SLOTS		32U
#define VMD_FUNCTIONS		8U
#define VMD_HEADER_MULTI	0x80U
#define VMD_HEADER_KIND		0x7fU
#define VMD_HEADER_BRIDGE	0x01U

/*
 * How far the driver goes with a VMD, as the boot parameters ask.
 */
enum vmd_stage {
	VMD_STAGE_OFF = 0,
	VMD_STAGE_READ,
	VMD_STAGE_NOSCAN,
	VMD_STAGE_FULL
};

/*
 * One VMD this driver serves.
 *
 * It lives from a successful attach on: the root bus it publishes keeps
 * a pointer to it, and the VMD is never detached.  config_lock serializes
 * every access to the configuration space behind it; message_lock guards
 * the children's message table and is the only lock the interrupt takes.
 */
struct vmd_host {
	struct drv_pci_device *device;
	unsigned instance;
	uint16_t segment;
	struct drv_pci_mapping cfgbar;
	uint64_t cfgbar_size;
	struct drv_pci_bar membar1;
	struct drv_pci_bar membar2;
	struct drv_pci_vmd_domain domain;
	struct spinlock config_lock;
	struct spinlock message_lock;
	struct drv_pci_vmd_messages messages;
	struct drv_pci_irq irq;
	void *irq_cookie;
	unsigned long interrupts;
	struct drv_pci_bus *root;
	unsigned claimed[3];
	uint16_t saved_configuration;
};

static int vmd_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int vmd_detach(struct drv_pci_device *device, unsigned flags);
static int vmd_suspend(struct drv_pci_device *device);
static int vmd_resume(struct drv_pci_device *device);
static int host_config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *value);
static int host_config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int host_map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void host_unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int host_allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void host_free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);
static int host_msi_establish(void *context, struct drv_pci_device *device, drv_pci_msi_run_t run, void *argument, unsigned *handle, uint64_t *address, uint32_t *data);
static int host_msi_disestablish(void *context, unsigned handle);

/*
 * The operations the PCI core calls on the root bus behind a VMD.
 *
 * The VMD forwards no legacy pin interrupt, so the pin hooks are absent
 * and only message interrupts are offered.  Every BAR is placed before the
 * bus is scanned, so no probe is held back.
 */
static const struct drv_pci_bus_ops vmd_bus_ops = {
	.config_space_size = 4096,
	.config_read = host_config_read,
	.config_write = host_config_write,
	.map_bar = host_map_bar,
	.unmap_bar = host_unmap_bar,
	.allocate_irqs = host_allocate_irqs,
	.free_irqs = host_free_irqs,
	.msi_establish = host_msi_establish,
	.msi_disestablish = host_msi_disestablish
};

/*
 * The VMD functions this driver binds to: the Alder Lake client VMD.
 *
 * Values referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c (vmd_devs, the
 * 8086:467f row).
 */
static const struct drv_pci_id vmd_ids[] = {
	{
		.vendor = 0x8086U,
		.device = 0x467fU,
		.subvendor = DRV_PCI_ANY_ID,
		.subdevice = DRV_PCI_ANY_ID,
		.class_code = 0,
		.class_mask = 0,
	},
};

/*
 * The driver's entry in the PCI core.
 *
 * It has no shutdown hook: the core shuts the children down first, and
 * the VMD itself needs nothing at power-off.
 */
static struct drv_pci_driver vmd_driver = {
	.name = "vmd",
	.ids = vmd_ids,
	.id_count = sizeof(vmd_ids) / sizeof(vmd_ids[0]),
	.attach = vmd_attach,
	.detach = vmd_detach,
	.suspend = vmd_suspend,
	.resume = vmd_resume,
};

/*
 * How many VMDs have been attached, which picks each one's segment.
 *
 * Attach runs from the boot-time PCI scan, one function at a time, so the
 * counter needs no lock; it only grows.
 */
static unsigned vmd_instances;

static enum vmd_stage vmd_boot_stage(void);
static void vmd_log_firmware(struct vmd_host *host);
static void vmd_log_function(struct vmd_host *host, unsigned bus, unsigned device, unsigned function);
static int vmd_read_windows(struct vmd_host *host);
static int vmd_read_buses(struct vmd_host *host);
static int vmd_refuse_shadow(struct vmd_host *host);
static int vmd_mask_vectors(struct vmd_host *host);
static int vmd_take_vector(struct vmd_host *host);
static int vmd_check_windows(struct vmd_host *host);
static int vmd_plan(struct vmd_host *host);
static void vmd_log_plan(struct vmd_host *host, const struct drv_pci_vmd_plan *plan);
static const char *vmd_port_state_name(enum drv_pci_vmd_port_state state);
static int vmd_interrupt(void *argument);
static int vmd_config_read(void *context, unsigned bus, unsigned device, unsigned function, unsigned offset, unsigned width, uint32_t *value);
static int vmd_config_write(void *context, unsigned bus, unsigned device, unsigned function, unsigned offset, unsigned width, uint32_t value);
static void vmd_release(struct vmd_host *host);

/*
 * Registers the VMD driver with the PCI core.
 */
int
drv_pci_vmd_driver_register(
	void)
{
	int error;

	/* Hands the driver to the core, which matches it on the next scan. */
	error = drv_pci_driver_register(&vmd_driver);
	if (error != 0)
		return error;

	/* Succeeded: the driver binds to a VMD as the scan finds it. */
	return 0;
}

/* Binds to a VMD, arranges the buses behind it and publishes them. */
static int
vmd_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct vmd_host *host;
	enum vmd_stage stage;
	int error;

	UNUSED_PARAMETER(id);

	/* Leaves the VMD alone when the boot line asks, so a machine that stops here can still boot. */
	stage = vmd_boot_stage();
	if (stage == VMD_STAGE_OFF) {
		kern_logf("vmd: disabled by %s\n", VMD_TOKEN_OFF);
		return ENODEV;
	}

	/* Refuses a VMD past the segments set aside for them. */
	if (vmd_instances == VMD_INSTANCES_MAX)
		return ENOSPC;

	/* Allocates the host. */
	host = kern_malloc(sizeof(*host));
	if (host == NULL)
		return ENOMEM;

	/* Initializes the host and its locks; the message table is empty before any vector is taken. */
	kern_memset(host, 0, sizeof(*host));
	host->device = device;
	host->instance = vmd_instances;
	host->segment = (uint16_t)(VMD_SEGMENT_FIRST - vmd_instances);
	spin_init(&host->config_lock, LOCK_RANK_DEVICE, "vmd configuration");
	spin_init(&host->message_lock, LOCK_RANK_DEVICE, "vmd messages");
	drv_pci_vmd_messages_init(&host->messages);

	/* Lets the VMD decode its BARs. */
	kern_logf("vmd: attaching (stage %u)\n", (unsigned)stage);
	error = drv_pci_device_enable_memory(device);
	if (error != 0) {
		kern_logf("vmd: memory decoding cannot be enabled (%d)\n", error);
		kern_free(host);
		return error;
	}

	/* Lets the VMD forward its children's DMA. */
	error = drv_pci_device_set_bus_master(device, true);
	if (error != 0) {
		kern_logf("vmd: bus mastering cannot be enabled (%d)\n", error);
		kern_free(host);
		return error;
	}

	/* Takes the BARs and maps the configuration space. */
	error = vmd_read_windows(host);
	if (error != 0) {
		vmd_release(host);
		return error;
	}

	/* Reads which buses the configuration space decodes. */
	error = vmd_read_buses(host);
	if (error != 0) {
		vmd_release(host);
		return error;
	}

	/* Refuses a VMD a hypervisor emulates. */
	error = vmd_refuse_shadow(host);
	if (error != 0) {
		vmd_release(host);
		return error;
	}

	/* Logs what the firmware left behind the VMD, and stops there when asked. */
	vmd_log_firmware(host);
	if (stage == VMD_STAGE_READ) {
		kern_logf("vmd: stopped after reading (%s)\n", VMD_TOKEN_READ);
		vmd_release(host);
		return ENODEV;
	}

	/* Takes the VMD vector the children's messages are caught into. */
	error = vmd_take_vector(host);
	if (error != 0) {
		vmd_release(host);
		return error;
	}

	/* Arranges the buses behind the VMD. */
	error = vmd_plan(host);
	if (error != 0) {
		kern_logf("vmd: buses cannot be arranged (%d)\n", error);
		vmd_release(host);
		return error;
	}

	/* Publishes no bus when asked; the VMD keeps its vector and its arrangement. */
	if (stage == VMD_STAGE_NOSCAN) {
		kern_logf("vmd: %04x: no bus published (%s)\n", host->segment, VMD_TOKEN_NOSCAN);
		vmd_instances++;
		(void)drv_pci_device_set_driver_data(device, host);
		return 0;
	}

	/* Creates the root bus behind the VMD. */
	error = drv_pci_bus_create_root(host->segment, (uint8_t)host->domain.bus_start, &vmd_bus_ops, host, drv_pci_device_dma(device), &host->root);
	if (error != 0) {
		kern_logf("vmd: root bus cannot be created (%d)\n", error);
		vmd_release(host);
		return error;
	}

	/*
	 * The instance counter moves only once the bus exists: its segment is
	 * now taken for good.
	 */
	vmd_instances++;
	(void)drv_pci_device_set_driver_data(device, host);

	/* Puts the bus behind the VMD, so that across a sleep its functions go down before the VMD and come up after it. */
	error = drv_pci_bus_set_host_function(host->root, device);
	if (error != 0)
		kern_logf("vmd: %04x: the bus cannot be put behind the VMD (%d)\n", host->segment, error);

	/* Enumerates the functions behind the VMD, which binds their drivers. */
	error = drv_pci_bus_scan_tree(host->root);
	if (error != 0)
		kern_logf("vmd: %04x: scan failed (%d)\n", host->segment, error);

	/* Succeeded: the buses behind the VMD belong to the PCI core. */
	return 0;
}

/* Refuses to let the VMD go: the buses behind it stay published until power-off. */
static int
vmd_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(flags);

	/* Reports the VMD as in use. */
	return EBUSY;
}

/*
 * Prepares the VMD for D3hot: keeps its configuration register.
 *
 * The PCI core has suspended the functions behind the VMD and saved the
 * root ports by now; it saves the VMD's header and MSI-X table itself
 * after this returns.
 */
static int
vmd_suspend(
	struct drv_pci_device *device)
{
	struct vmd_host *host;
	uint16_t configuration;
	int error;

	/* Refuses a function this driver holds nothing for. */
	host = drv_pci_device_driver_data(device);
	if (host == NULL)
		return EINVAL;

	/* Reads the configuration register: the bus range and whether messages are caught. */
	error = drv_pci_device_config_read16(device, VMD_REG_CONFIGURATION, &configuration);
	if (error != 0) {
		kern_logf("vmd: %04x: the configuration register cannot be read (%d)\n", host->segment, error);
		return error;
	}

	/* Keeps it for the resume. */
	host->saved_configuration = configuration;

	/* Succeeded: the VMD can go to D3hot. */
	return 0;
}

/*
 * Brings the VMD back after D0: its configuration register as it was,
 * then checks that the buses and the windows are where the functions
 * behind it were placed.
 *
 * The PCI core has written back the VMD's header and MSI-X table by now,
 * and writes back the root ports and resumes the functions behind them
 * after this returns.  EIO reports a VMD whose bus range or windows did
 * not come back, behind which nothing can be reached.
 */
static int
vmd_resume(
	struct drv_pci_device *device)
{
	struct vmd_host *host;
	uint16_t capability;
	uint16_t configuration;
	unsigned start;
	bool reset;
	int error;

	/* Refuses a function this driver holds nothing for. */
	host = drv_pci_device_driver_data(device);
	if (host == NULL)
		return EINVAL;

	/* Reads the configuration register as the reset left it. */
	error = drv_pci_device_config_read16(device, VMD_REG_CONFIGURATION, &configuration);
	if (error != 0) {
		kern_logf("vmd: %04x: the configuration register cannot be read (%d)\n", host->segment, error);
		return EIO;
	}

	/* Writes the saved register back when the reset changed it, and reads what took. */
	reset = false;
	if (configuration != host->saved_configuration) {
		reset = true;
		error = drv_pci_device_config_write16(device, VMD_REG_CONFIGURATION, host->saved_configuration);
		if (error != 0) {
			kern_logf("vmd: %04x: the configuration register cannot be written (%d)\n", host->segment, error);
			return EIO;
		}

		/* Reads it back. */
		error = drv_pci_device_config_read16(device, VMD_REG_CONFIGURATION, &configuration);
		if (error != 0)
			return EIO;
	}

	/* Reads the capability register, which says whether the bus range applies. */
	error = drv_pci_device_config_read16(device, VMD_REG_CAPABILITY, &capability);
	if (error != 0)
		return EIO;

	/* Refuses a bus range other than the one the buses were numbered in. */
	error = drv_pci_vmd_bus_start(capability, configuration, &start);
	if (error != 0 || start != host->domain.bus_start) {
		kern_logf("vmd: %04x: the buses did not come back (configuration %04x, saved %04x)\n",
			  host->segment,
			  configuration,
			  host->saved_configuration);
		return EIO;
	}

	/* Refuses windows that moved. */
	error = vmd_check_windows(host);
	if (error != 0)
		return EIO;

	/* Logs the VMD back, and whether the sleep had reset its register. */
	kern_logf("vmd: %04x: resumed (configuration %04x, %s)\n", host->segment, configuration, reset ? "written back after a reset" : "kept");

	/* Succeeded: the root ports behind the VMD can be written back. */
	return 0;
}

/* Reads a register behind the VMD for the PCI core. */
static int
host_config_read(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	int error;

	/* Reads through the CFGBAR. */
	error = vmd_config_read(context, address->bus, address->device, address->function, offset, width, value);
	if (error != 0)
		return error;

	/* Succeeded: the register's value. */
	return 0;
}

/* Writes a register behind the VMD for the PCI core. */
static int
host_config_write(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	int error;

	/* Writes through the CFGBAR. */
	error = vmd_config_write(context, address->bus, address->device, address->function, offset, width, value);
	if (error != 0)
		return error;

	/* Succeeded: the register is written. */
	return 0;
}

/*
 * Maps a child's BAR.
 *
 * The VMD forwards its windows' addresses unchanged, so a BAR's bus
 * address is the processor's physical address; a BAR outside both
 * windows is refused.
 */
static int
host_map_bar(
	void *context,
	struct drv_pci_device *device,
	const struct drv_pci_bar *bar,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	struct vmd_host *host;
	const struct drv_pci_range *window;
	uint64_t last;
	uint64_t window_last;
	unsigned index;
	void *address;
	bool inside;
	int error;

	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(flags);

	/* Refuses an I/O BAR, an empty one and missing storage. */
	host = context;
	if (bar == NULL || mapping == NULL)
		return EINVAL;
	if (bar->type == DRV_PCI_BAR_IO || bar->type == DRV_PCI_BAR_NONE)
		return EINVAL;
	if (bar->size == 0 || bar->bus_address == 0)
		return EINVAL;
	if (bar->bus_address > UINT64_MAX - (bar->size - 1U))
		return EINVAL;

	/* Looks for the window that holds the whole BAR. */
	last = bar->bus_address + (bar->size - 1U);
	inside = false;
	for (index = 0; index < host->domain.window_count; index++) {
		window = &host->domain.windows[index];
		window_last = window->base + (window->length - 1U);
		if (bar->bus_address >= window->base && last <= window_last) {
			inside = true;
			break;
		}
	}

	/* Refuses a BAR the VMD does not forward. */
	if (!inside)
		return EINVAL;

	/* Maps the BAR as uncached device memory. */
	error = kern_device_map(bar->bus_address, (size_t)bar->size, KERN_DEVICE_UNCACHED, &address);
	if (error != 0)
		return error;

	/* Describes the mapping; the first private word marks it as made here. */
	mapping->address = address;
	mapping->physical_address = bar->bus_address;
	mapping->size = (size_t)bar->size;
	mapping->type = bar->type;
	mapping->private_data[0] = 1U;
	mapping->private_data[1] = 0;

	/* Succeeded: the driver reaches the BAR at address. */
	return 0;
}

/* Unmaps a child's BAR. */
static void
host_unmap_bar(
	void *context,
	struct drv_pci_mapping *mapping)
{
	UNUSED_PARAMETER(context);

	/* Ignores storage that holds no mapping made here. */
	if (mapping == NULL || mapping->private_data[0] != 1U)
		return;

	/* Releases the mapping. */
	(void)kern_device_unmap(mapping->address, mapping->size);
	kern_memset(mapping, 0, sizeof(*mapping));
}

/*
 * Offers a child one message interrupt.
 *
 * The VMD forwards no pin interrupt, so only MSI-X and MSI are offered,
 * one message each; the core then asks host_msi_establish() for its
 * address and data.
 */
static int
host_allocate_irqs(
	void *context,
	struct drv_pci_device *device,
	enum drv_pci_irq_type type,
	unsigned minimum,
	unsigned maximum,
	struct drv_pci_irq *irqs,
	unsigned *count)
{
	unsigned capability;
	uint8_t id;
	int error;

	UNUSED_PARAMETER(context);

	/* Refuses missing arguments and a request for more than one message. */
	if (device == NULL || irqs == NULL || count == NULL)
		return EINVAL;
	if (minimum != 1U || maximum < minimum)
		return ENOTSUP;

	/* Offers nothing for a pin. */
	if (type != DRV_PCI_IRQ_MSI && type != DRV_PCI_IRQ_MSIX)
		return ENOTSUP;

	/* Refuses a function without the capability asked for. */
	id = 0x05U;
	if (type == DRV_PCI_IRQ_MSIX)
		id = VMD_CAP_MSIX;
	error = drv_pci_device_find_capability(device, id, &capability);
	if (error != 0)
		return ENOTSUP;

	/* Describes the one message; the core establishes it through the host. */
	irqs[0].type = type;
	irqs[0].index = 0;
	irqs[0].vector = 0;
	irqs[0].private_data[0] = capability;
	irqs[0].private_data[1] = 0;
	*count = 1U;

	/* Succeeded: one message. */
	return 0;
}

/* Gives back a child's messages: nothing was taken by host_allocate_irqs(). */
static void
host_free_irqs(
	void *context,
	struct drv_pci_device *device,
	struct drv_pci_irq *irqs,
	unsigned count)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(irqs);
	UNUSED_PARAMETER(count);
}

/*
 * Takes a slot for one child message and reports what the child writes.
 *
 * The address selects the VMD vector the driver took, so the VMD catches
 * the message and raises that vector; vmd_interrupt() then runs every
 * child's handler.
 */
static int
host_msi_establish(
	void *context,
	struct drv_pci_device *device,
	drv_pci_msi_run_t run,
	void *argument,
	unsigned *handle,
	uint64_t *address,
	uint32_t *data)
{
	struct drv_pci_address location;
	struct vmd_host *host;
	unsigned long enabled;
	unsigned long served;
	unsigned slot;
	int error;

	/* Refuses missing arguments. */
	host = context;
	if (run == NULL || handle == NULL || address == NULL || data == NULL)
		return EINVAL;

	/* Fills a free slot of the message table, and reads how many interrupts came so far. */
	enabled = spin_lock_irqsave(&host->message_lock);

	error = drv_pci_vmd_message_take(&host->messages, run, argument, &slot);
	served = host->interrupts;

	spin_unlock_irqrestore(&host->message_lock, enabled);

	/* Reports a full table. */
	if (error != 0)
		return error;

	/* Logs the message, and the interrupts the VMD vector has served before it. */
	drv_pci_device_address(device, &location);
	kern_logf("vmd: %04x:%02x:%02x.%u message %u (%lu interrupts served so far)\n",
		  location.segment,
		  location.bus,
		  location.device,
		  location.function,
		  slot,
		  served);

	/* Succeeded: the slot is the handle, and the message selects the VMD vector. */
	*handle = slot;
	drv_pci_vmd_message_address(VMD_CHILD_VECTOR, address, data);
	return 0;
}

/*
 * Frees one child message, waiting for an interrupt still running it.
 *
 * EBUSY reports an interrupt that did not return within a second; the
 * slot is then put back as it was.
 */
static int
host_msi_disestablish(
	void *context,
	unsigned handle)
{
	struct vmd_host *host;
	drv_pci_vmd_run_t run;
	unsigned long enabled;
	unsigned in_flight;
	unsigned poll;
	void *argument;
	int error;

	/* The context is the host whose table holds the message. */
	host = context;

	/* Empties the slot so that no new interrupt runs it. */
	enabled = spin_lock_irqsave(&host->message_lock);

	error = drv_pci_vmd_message_clear(&host->messages, handle, &run, &argument);

	spin_unlock_irqrestore(&host->message_lock, enabled);

	/* Refuses a slot that holds nothing. */
	if (error != 0)
		return error;

	/* Waits, a millisecond at a time, for the interrupts still inside the handler. */
	in_flight = 0;
	for (poll = 0; poll < VMD_DRAIN_POLLS; poll++) {
		/* Reads how many interrupts still run the slot. */
		enabled = spin_lock_irqsave(&host->message_lock);

		in_flight = drv_pci_vmd_message_in_flight(&host->messages, handle);

		spin_unlock_irqrestore(&host->message_lock, enabled);

		/* Drained: no hart runs the slot and none will. */
		if (in_flight == 0)
			break;

		/* Lets the other hart finish. */
		kern_usleep_range(VMD_DRAIN_POLL_US, VMD_DRAIN_POLL_US);
	}

	/* Puts the slot back when an interrupt did not come back within the bound. */
	if (in_flight != 0) {
		enabled = spin_lock_irqsave(&host->message_lock);

		drv_pci_vmd_message_restore(&host->messages, handle, run, argument);

		spin_unlock_irqrestore(&host->message_lock, enabled);

		kern_logf("vmd: %04x: message %u still running after a second\n", host->segment, handle);
		return EBUSY;
	}

	/* Succeeded: the slot is free and no interrupt runs it. */
	return 0;
}

/* Reads from the boot line how far the driver is to go. */
static enum vmd_stage
vmd_boot_stage(
	void)
{
	const char *line;
	int present;

	/* Reads the line the loader handed over; without one the driver goes all the way. */
	line = kern_boot_handoff("boot.command-line");
	if (line == NULL)
		return VMD_STAGE_FULL;

	/* Looks for each whole token, the one that does least first. */
	present = kern_boot_parameters_token_present(line, VMD_TOKEN_OFF);
	if (present)
		return VMD_STAGE_OFF;
	present = kern_boot_parameters_token_present(line, VMD_TOKEN_READ);
	if (present)
		return VMD_STAGE_READ;
	present = kern_boot_parameters_token_present(line, VMD_TOKEN_NOSCAN);
	if (present)
		return VMD_STAGE_NOSCAN;

	/* Reports the whole driver as wanted. */
	return VMD_STAGE_FULL;
}

/*
 * Logs every function on the VMD's first bus, and on the buses its root
 * ports had been given, as the firmware left them.  It only reads.
 */
static void
vmd_log_firmware(
	struct vmd_host *host)
{
	uint32_t identity;
	uint32_t header;
	uint32_t buses;
	unsigned device;
	unsigned function;
	unsigned functions;
	unsigned secondary;
	unsigned below;

	/* Looks at each slot of the first bus. */
	for (device = 0; device < VMD_SLOTS; device++) {
		/* A slot whose first function does not answer is empty. */
		(void)vmd_config_read(host, host->domain.bus_start, device, 0, 0x00U, 4U, &identity);
		if ((identity & 0xffffU) == 0xffffU)
			continue;

		/* A multi-function device may answer on all eight. */
		(void)vmd_config_read(host, host->domain.bus_start, device, 0, 0x0eU, 1U, &header);
		functions = 1U;
		if ((header & VMD_HEADER_MULTI) != 0)
			functions = VMD_FUNCTIONS;

		/* Logs each function of the slot, and what is behind a bridge. */
		for (function = 0; function < functions; function++) {
			/* Passes over a function that does not answer. */
			(void)vmd_config_read(host, host->domain.bus_start, device, function, 0x00U, 4U, &identity);
			if ((identity & 0xffffU) == 0xffffU)
				continue;

			vmd_log_function(host, host->domain.bus_start, device, function);

			/* Looks behind a bridge whose secondary bus the CFGBAR decodes. */
			(void)vmd_config_read(host, host->domain.bus_start, device, function, 0x0eU, 1U, &header);
			if ((header & VMD_HEADER_KIND) != VMD_HEADER_BRIDGE)
				continue;
			(void)vmd_config_read(host, host->domain.bus_start, device, function, 0x18U, 4U, &buses);
			secondary = (buses >> 8) & 0xffU;
			if (secondary <= host->domain.bus_start || secondary > host->domain.bus_end)
				continue;

			/* Logs each slot of the bus behind the bridge. */
			for (below = 0; below < VMD_SLOTS; below++) {
				/* Logs the first function of each slot that answers. */
				(void)vmd_config_read(host, secondary, below, 0, 0x00U, 4U, &identity);
				if ((identity & 0xffffU) == 0xffffU)
					continue;

				vmd_log_function(host, secondary, below, 0);
			}
		}
	}
}

/* Logs one function's identity, command, header, bus numbers or first BARs, and window. */
static void
vmd_log_function(
	struct vmd_host *host,
	unsigned bus,
	unsigned device,
	unsigned function)
{
	uint32_t identity;
	uint32_t command;
	uint32_t header;
	uint32_t first;
	uint32_t second;
	uint32_t window;

	/* Reads the registers the log shows. */
	(void)vmd_config_read(host, bus, device, function, 0x00U, 4U, &identity);
	(void)vmd_config_read(host, bus, device, function, 0x04U, 4U, &command);
	(void)vmd_config_read(host, bus, device, function, 0x0cU, 4U, &header);
	(void)vmd_config_read(host, bus, device, function, 0x10U, 4U, &first);
	(void)vmd_config_read(host, bus, device, function, 0x18U, 4U, &second);
	(void)vmd_config_read(host, bus, device, function, 0x20U, 4U, &window);

	/* One line per function: 0x18 holds a bridge's bus numbers, an endpoint's BAR2. */
	kern_logf("vmd: firmware %02x:%02x.%u id %08x command %08x header %08x bar0 %08x reg18 %08x reg20 %08x\n",
		  bus,
		  device,
		  function,
		  identity,
		  command,
		  header,
		  first,
		  second,
		  window);
}

/*
 * Claims the VMD's three BARs, maps its configuration space and records
 * its windows.
 */
static int
vmd_read_windows(
	struct vmd_host *host)
{
	struct drv_pci_bar config;
	static const unsigned bars[3] = { VMD_BAR_CONFIG, VMD_BAR_MEMORY1, VMD_BAR_MEMORY2 };
	unsigned index;
	int error;

	/* Claims each BAR, so that nothing else maps the VMD's windows. */
	for (index = 0; index < 3U; index++) {
		error = drv_pci_device_claim_bar(host->device, bars[index]);
		if (error != 0) {
			kern_logf("vmd: BAR%u cannot be claimed (%d)\n", bars[index], error);
			return error;
		}
		host->claimed[index] = 1;
	}

	/* Reads the CFGBAR. */
	error = drv_pci_device_bar(host->device, VMD_BAR_CONFIG, &config);
	if (error != 0) {
		kern_logf("vmd: BAR%u cannot be read (%d)\n", VMD_BAR_CONFIG, error);
		return error;
	}

	/* Reads MEMBAR1. */
	error = drv_pci_device_bar(host->device, VMD_BAR_MEMORY1, &host->membar1);
	if (error != 0) {
		kern_logf("vmd: BAR%u cannot be read (%d)\n", VMD_BAR_MEMORY1, error);
		return error;
	}

	/* Reads MEMBAR2. */
	error = drv_pci_device_bar(host->device, VMD_BAR_MEMORY2, &host->membar2);
	if (error != 0) {
		kern_logf("vmd: BAR%u cannot be read (%d)\n", VMD_BAR_MEMORY2, error);
		return error;
	}

	/* Refuses a CFGBAR smaller than one bus. */
	if (config.size < VMD_CFGBAR_MIN) {
		kern_logf("vmd: CFGBAR size 0x%llx is less than one bus\n", (unsigned long long)config.size);
		return ENODEV;
	}

	/* Refuses a MEMBAR2 with no room past the VMD's own head. */
	if (host->membar2.size <= VMD_MEMBAR2_HEAD) {
		kern_logf("vmd: MEMBAR2 size 0x%llx leaves no window\n", (unsigned long long)host->membar2.size);
		return ENODEV;
	}

	/* Maps the configuration space behind the VMD. */
	error = drv_pci_device_map_bar(host->device, VMD_BAR_CONFIG, DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE, &host->cfgbar);
	if (error != 0) {
		kern_logf("vmd: CFGBAR 0x%llx/0x%llx cannot be mapped (%d)\n",
			  (unsigned long long)config.bus_address,
			  (unsigned long long)config.size,
			  error);
		return error;
	}
	host->cfgbar_size = config.size;

	/* Records the windows: MEMBAR1 whole, MEMBAR2 past the VMD's own head. */
	host->domain.windows[0].base = host->membar1.bus_address;
	host->domain.windows[0].length = host->membar1.size;
	host->domain.windows[1].base = host->membar2.bus_address + VMD_MEMBAR2_HEAD;
	host->domain.windows[1].length = host->membar2.size - VMD_MEMBAR2_HEAD;
	host->domain.window_count = 2;

	/* Succeeded: the VMD's space and windows are known. */
	return 0;
}

/* Reads which bus numbers the VMD's configuration space decodes. */
static int
vmd_read_buses(
	struct vmd_host *host)
{
	struct drv_pci_address location;
	uint16_t capability;
	uint16_t configuration;
	uint64_t buses;
	unsigned start;
	int error;

	/* Reads the capability register. */
	error = drv_pci_device_config_read16(host->device, VMD_REG_CAPABILITY, &capability);
	if (error != 0) {
		kern_logf("vmd: the capability register cannot be read (%d)\n", error);
		return error;
	}

	/* Reads the configuration register. */
	error = drv_pci_device_config_read16(host->device, VMD_REG_CONFIGURATION, &configuration);
	if (error != 0) {
		kern_logf("vmd: the configuration register cannot be read (%d)\n", error);
		return error;
	}

	/* Finds the first bus. */
	error = drv_pci_vmd_bus_start(capability, configuration, &start);
	if (error != 0) {
		kern_logf("vmd: unknown bus start in configuration %04x\n", configuration);
		return ENODEV;
	}

	/* The last bus is as far as the CFGBAR reaches, and never past 255. */
	buses = host->cfgbar_size >> VMD_CFGBAR_BUS_SHIFT;
	host->domain.bus_start = start;
	host->domain.bus_end = 255U;
	if (start + buses - 1U < 255U)
		host->domain.bus_end = (unsigned)(start + buses - 1U);

	/* Reports the VMD, its windows and its registers. */
	drv_pci_device_address(host->device, &location);
	kern_logf("vmd: %04x:%02x:%02x.%u 8086:467f buses %02x-%02x cfgbar 0x%llx/0x%llx membar1 0x%llx/0x%llx membar2 0x%llx/0x%llx capability %04x configuration %04x\n",
		  location.segment,
		  location.bus,
		  location.device,
		  location.function,
		  host->domain.bus_start,
		  host->domain.bus_end,
		  (unsigned long long)host->cfgbar.physical_address,
		  (unsigned long long)host->cfgbar_size,
		  (unsigned long long)host->membar1.bus_address,
		  (unsigned long long)host->membar1.size,
		  (unsigned long long)host->membar2.bus_address,
		  (unsigned long long)host->membar2.size,
		  capability,
		  configuration);

	/* Succeeded: the bus range is known. */
	return 0;
}

/*
 * Refuses a VMD a hypervisor emulates.
 *
 * Its "SHDW" capability says the windows lie at other host addresses,
 * which this driver does not translate.
 */
static int
vmd_refuse_shadow(
	struct vmd_host *host)
{
	unsigned capability;
	uint32_t signature;
	int error;

	/* A VMD without the vendor capability is bare metal. */
	error = drv_pci_device_find_capability(host->device, VMD_CAP_VENDOR, &capability);
	if (error != 0)
		return 0;

	/* Reads the capability's signature. */
	error = drv_pci_device_config_read32(host->device, capability + VMD_SHADOW_OFFSET, &signature);
	if (error != 0)
		return 0;

	/* Refuses the emulated VMD. */
	if (signature == VMD_SHADOW_SIGNATURE) {
		kern_logf("vmd: an emulated VMD (SHDW) is not supported\n");
		return ENODEV;
	}

	/* Succeeded: the VMD is bare metal. */
	return 0;
}

/*
 * Masks every entry of the VMD's MSI-X table and checks it has the vector
 * the children's messages are caught into.
 *
 * The firmware may have left entries unmasked; only the one this driver
 * establishes is unmasked again, by the PCI core.
 */
static int
vmd_mask_vectors(
	struct vmd_host *host)
{
	struct drv_pci_mapping table;
	volatile uint32_t *words;
	unsigned entries;
	unsigned entry;
	uint32_t control;
	int error;

	/* Maps the whole table. */
	error = drv_pci_device_map_msix_table(host->device, &table, &entries);
	if (error != 0) {
		kern_logf("vmd: no MSI-X table (%d)\n", error);
		return ENODEV;
	}

	/* Refuses a VMD without a vector past the one it keeps. */
	if (entries <= VMD_CHILD_VECTOR) {
		drv_pci_device_unmap_bar(host->device, &table);
		kern_logf("vmd: %u MSI-X vectors, too few\n", entries);
		return ENODEV;
	}

	/* Sets the mask bit of each entry's vector control word. */
	words = table.address;
	for (entry = 0; entry < entries; entry++) {
		control = kern_mmio_read32(&words[entry * VMD_MSIX_ENTRY_WORDS + VMD_MSIX_CONTROL_WORD]);
		kern_mmio_write32(&words[entry * VMD_MSIX_ENTRY_WORDS + VMD_MSIX_CONTROL_WORD], control | VMD_MSIX_ENTRY_MASK);
	}
	kern_io_barrier();

	/* Lets the table go. */
	drv_pci_device_unmap_bar(host->device, &table);

	/* Succeeded: every vector is masked. */
	return 0;
}

/*
 * Takes the VMD vector the children's messages are caught into, and has
 * the VMD catch them.
 *
 * The PCI core offers one MSI-X record for entry 0; its index is moved to
 * VMD_CHILD_VECTOR before it is established, because the client VMDs keep
 * entry 0 for themselves.  The core maps and programs the entry the index
 * names (establish_msix and map_msix_entry in pci.c read irq->index), so
 * no other entry is touched.
 */
static int
vmd_take_vector(
	struct vmd_host *host)
{
	uint16_t configuration;
	unsigned count;
	int error;

	/* Masks the table the firmware left. */
	error = vmd_mask_vectors(host);
	if (error != 0)
		return error;

	/* Asks the core for an MSI-X record. */
	error = drv_pci_device_allocate_irqs(host->device, DRV_PCI_IRQ_ALLOW_MSIX, 1U, 1U, &host->irq, &count);
	if (error != 0) {
		kern_logf("vmd: no MSI-X vector (%d)\n", error);
		return error;
	}

	/* Points the record at the vector the children's messages are caught into. */
	host->irq.index = VMD_CHILD_VECTOR;

	/* Runs every child's handler when the vector fires. */
	error = drv_pci_device_establish_irq(host->device, &host->irq, vmd_interrupt, host, "vmd", &host->irq_cookie);
	if (error != 0) {
		drv_pci_device_free_irqs(host->device, &host->irq, 1U);
		kern_logf("vmd: MSI-X vector %u could not be established (%d)\n", VMD_CHILD_VECTOR, error);
		return error;
	}

	/* Refuses a VMD whose windows moved while its vector was mapped. */
	error = vmd_check_windows(host);
	if (error != 0)
		return error;

	/*
	 * Has the VMD catch the children's messages: the bypass bit cleared.
	 *
	 * Procedure referenced: FreeBSD 15.0 sys/dev/vmd/vmd.c
	 * (vmd_set_msi_bypass(dev, false) after the vectors are set up).
	 */
	error = drv_pci_device_config_read16(host->device, VMD_REG_CONFIGURATION, &configuration);
	if (error != 0)
		return error;
	if ((configuration & VMD_CONFIGURATION_BYPASS) != 0) {
		error = drv_pci_device_config_write16(host->device, VMD_REG_CONFIGURATION, configuration & (uint16_t)~VMD_CONFIGURATION_BYPASS);
		if (error != 0)
			return error;
	}

	/* Succeeded: the children's messages reach vmd_interrupt(). */
	return 0;
}

/*
 * Checks that the VMD's windows are where they were read.
 *
 * The PC/AT host may move a small BAR when it cannot map it where the
 * firmware put it (pcat_map_bar in pci-pcat.c); a moved MEMBAR would leave
 * the children's addresses outside the window.
 */
static int
vmd_check_windows(
	struct vmd_host *host)
{
	struct drv_pci_bar membar1;
	struct drv_pci_bar membar2;
	int error;

	/* Reads the windows again. */
	error = drv_pci_device_bar(host->device, VMD_BAR_MEMORY1, &membar1);
	if (error != 0)
		return error;
	error = drv_pci_device_bar(host->device, VMD_BAR_MEMORY2, &membar2);
	if (error != 0)
		return error;

	/* Refuses a window that moved. */
	if (membar1.bus_address != host->membar1.bus_address || membar2.bus_address != host->membar2.bus_address) {
		kern_logf("vmd: a window moved (0x%llx 0x%llx)\n",
			  (unsigned long long)membar1.bus_address,
			  (unsigned long long)membar2.bus_address);
		return ENODEV;
	}

	/* Succeeded: the windows stand. */
	return 0;
}

/* Numbers and places everything behind the VMD, and logs the result. */
static int
vmd_plan(
	struct vmd_host *host)
{
	struct drv_pci_vmd_config_access access;
	struct drv_pci_vmd_plan *plan;
	int error;

	/* Allocates the plan, which is too large for the stack. */
	plan = kern_malloc(sizeof(*plan));
	if (plan == NULL)
		return ENOMEM;

	/* Arranges the buses through the CFGBAR. */
	access.read = vmd_config_read;
	access.write = vmd_config_write;
	access.context = host;
	error = drv_pci_vmd_plan_domain(&access, &host->domain, plan);
	if (error != 0) {
		kern_free(plan);
		return error;
	}

	/* Reports what became of each port. */
	vmd_log_plan(host, plan);
	kern_free(plan);

	/* Succeeded: the buses behind the VMD are numbered and placed. */
	return 0;
}

/* Logs the firmware's values and the plan of each root port. */
static void
vmd_log_plan(
	struct vmd_host *host,
	const struct drv_pci_vmd_plan *plan)
{
	const struct drv_pci_vmd_port *port;
	const struct drv_pci_vmd_bar *bar;
	unsigned index;
	unsigned bar_index;

	/* Reports the functions left off on the first bus. */
	if (plan->endpoints != 0 || plan->ports_dropped != 0) {
		kern_logf("vmd: %04x: %u functions left off on bus %02x, %u root ports beyond %u\n",
			  host->segment,
			  plan->endpoints,
			  host->domain.bus_start,
			  plan->ports_dropped,
			  DRV_PCI_VMD_PORTS_MAX);
	}

	/* Reports each root port and the BARs behind it. */
	for (index = 0; index < plan->port_count; index++) {
		port = &plan->ports[index];
		kern_logf("vmd: root port %04x:%02x:%02x.%u %s bus %02x window 0x%llx/0x%llx (firmware buses %08x window %08x)\n",
			  host->segment,
			  host->domain.bus_start,
			  port->device,
			  port->function,
			  vmd_port_state_name(port->state),
			  port->secondary,
			  (unsigned long long)port->window_base,
			  (unsigned long long)port->window_length,
			  port->firmware_buses,
			  port->firmware_window);

		/* Reports the BARs only where they were written. */
		if (port->state != DRV_PCI_VMD_PORT_USED)
			continue;

		for (bar_index = 0; bar_index < port->bar_count; bar_index++) {
			bar = &port->bars[bar_index];
			kern_logf("vmd: %04x:%02x:%02x.%u BAR%u 0x%llx size 0x%llx%s%s\n",
				  host->segment,
				  port->secondary,
				  bar->device,
				  bar->function,
				  bar->index,
				  (unsigned long long)bar->address,
				  (unsigned long long)bar->size,
				  bar->wide ? " 64-bit" : "",
				  bar->prefetchable ? " prefetchable" : "");
		}
	}
}

/* Names a port's state for the log. */
static const char *
vmd_port_state_name(
	enum drv_pci_vmd_port_state state)
{
	/* Picks the word for the state. */
	switch (state) {
	case DRV_PCI_VMD_PORT_USED:
		return "used";
	case DRV_PCI_VMD_PORT_NO_BUS:
		return "unused (no bus number left)";
	case DRV_PCI_VMD_PORT_SWITCH:
		return "unused (switch behind it)";
	case DRV_PCI_VMD_PORT_OWN_BAR:
		return "unused (BAR of its own)";
	case DRV_PCI_VMD_PORT_NOT_D0:
		return "unused (function not in D0)";
	case DRV_PCI_VMD_PORT_TOO_MANY_BARS:
		return "unused (too many BARs)";
	case DRV_PCI_VMD_PORT_NO_WINDOW:
		return "unused (no window below 4 GiB)";
	}

	/* A state this file does not know. */
	return "unknown";
}

/*
 * Serves the VMD vector: runs the handler of every child message.
 *
 * The VMD raises one vector for any child; which child wrote is not
 * known, so every one is asked.  The handlers run without the table's
 * lock, each slot marked as running so that a removal waits for it.
 */
static int
vmd_interrupt(
	void *argument)
{
	struct drv_pci_vmd_pending pending[DRV_PCI_VMD_MESSAGES_MAX];
	struct vmd_host *host;
	unsigned long enabled;
	unsigned count;
	unsigned index;
	int work;
	int found;

	/* The argument is the host the vector was established for. */
	host = argument;

	/* Takes the held messages out of the table. */
	enabled = spin_lock_irqsave(&host->message_lock);

	count = drv_pci_vmd_messages_collect(&host->messages, pending);

	/*
	 * The count is logged from thread context (host_msi_establish), never
	 * from here: the console may not be entered from an interrupt.
	 */
	host->interrupts++;

	spin_unlock_irqrestore(&host->message_lock, enabled);

	/* Runs each child's handler. */
	found = 0;
	for (index = 0; index < count; index++) {
		work = pending[index].run(pending[index].argument);
		if (work != 0)
			found = 1;
	}

	/* Marks the messages as no longer running. */
	enabled = spin_lock_irqsave(&host->message_lock);

	drv_pci_vmd_messages_release(&host->messages, pending, count);

	spin_unlock_irqrestore(&host->message_lock, enabled);

	/* Reports whether a child found work. */
	return found;
}

/*
 * Reads a register behind the VMD through the CFGBAR.
 *
 * Accesses are serialized: the VMD may stall the processor on concurrent
 * configuration accesses.  An access the CFGBAR does not decode reads as
 * all ones.
 *
 * Procedure referenced: Linux 6.12.5 drivers/pci/controller/vmd.c
 * (vmd_pci_read, the accesses taken under one lock).
 */
static int
vmd_config_read(
	void *context,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	struct vmd_host *host;
	volatile uint8_t *address;
	unsigned long enabled;
	uint64_t position;
	int error;

	/* The context is the host whose CFGBAR is read. */
	host = context;

	/* An access the CFGBAR does not decode reads as nothing there. */
	error = drv_pci_vmd_config_offset(host->domain.bus_start, host->domain.bus_end, host->cfgbar_size, bus, device, function, offset, width, &position);
	if (error != 0) {
		*value = drv_pci_vmd_absent_value(width);
		return 0;
	}
	address = (volatile uint8_t *)host->cfgbar.address + position;

	/* Reads the register under the lock. */
	enabled = spin_lock_irqsave(&host->config_lock);

	if (width == 1U) {
		*value = kern_mmio_read8(address);
	} else if (width == 2U) {
		*value = kern_mmio_read16(address);
	} else {
		*value = kern_mmio_read32(address);
	}

	spin_unlock_irqrestore(&host->config_lock, enabled);

	/* Succeeded: the register's value. */
	return 0;
}

/*
 * Writes a register behind the VMD through the CFGBAR, and reads it back.
 *
 * The VMD turns a configuration write into a posted memory write; the
 * read-back waits for it to land before the caller goes on.
 *
 * Procedure referenced: Linux 6.12.5 drivers/pci/controller/vmd.c
 * (vmd_pci_write, the read-back after the write, under one lock).
 */
static int
vmd_config_write(
	void *context,
	unsigned bus,
	unsigned device,
	unsigned function,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	struct vmd_host *host;
	volatile uint8_t *address;
	unsigned long enabled;
	uint64_t position;
	int error;

	/* The context is the host whose CFGBAR is written. */
	host = context;

	/* Refuses an access the CFGBAR does not decode. */
	error = drv_pci_vmd_config_offset(host->domain.bus_start, host->domain.bus_end, host->cfgbar_size, bus, device, function, offset, width, &position);
	if (error != 0)
		return error;
	address = (volatile uint8_t *)host->cfgbar.address + position;

	/* Writes the register and reads it back under the lock. */
	enabled = spin_lock_irqsave(&host->config_lock);

	if (width == 1U) {
		kern_mmio_write8(address, (uint8_t)value);
		(void)kern_mmio_read8(address);
	} else if (width == 2U) {
		kern_mmio_write16(address, (uint16_t)value);
		(void)kern_mmio_read16(address);
	} else {
		kern_mmio_write32(address, value);
		(void)kern_mmio_read32(address);
	}

	spin_unlock_irqrestore(&host->config_lock, enabled);

	/* Succeeded: the register is written. */
	return 0;
}

/* Undoes a failed attach: the vector, the mapping, the BARs and the host. */
static void
vmd_release(
	struct vmd_host *host)
{
	static const unsigned bars[3] = { VMD_BAR_CONFIG, VMD_BAR_MEMORY1, VMD_BAR_MEMORY2 };
	unsigned index;

	/* Takes the handler off the vector; the record goes back with it. */
	if (host->irq_cookie != NULL) {
		drv_pci_device_disestablish_irq(host->device, host->irq_cookie);
		drv_pci_device_free_irqs(host->device, &host->irq, 1U);
		host->irq_cookie = NULL;
	}

	/* Unmaps the configuration space. */
	if (host->cfgbar.address != NULL)
		drv_pci_device_unmap_bar(host->device, &host->cfgbar);

	/* Gives back the BARs. */
	for (index = 0; index < 3U; index++) {
		if (host->claimed[index])
			drv_pci_device_release_bar(host->device, bars[index]);
	}

	/* Frees the host. */
	kern_free(host);
}
