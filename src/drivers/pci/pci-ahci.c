/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * AHCI (SATA) host controller driver.
 *
 * Each SATA disk behind an AHCI HBA is published to the disk layer as sdN.
 * Commands are READ/WRITE DMA EXT, FLUSH CACHE (EXT) and IDENTIFY DEVICE,
 * issued one at a time per port in slot 0 through a bounce buffer, the way
 * NVMe stages its I/O.  S0 idle stops the ports after a flush and resets
 * their links on resume.  NCQ, TRIM, link power management, hotplug and
 * ATAPI are not handled yet.
 */

#include <drivers/pci/pci-ahci.h>
#include <drivers/pci/pci-ahci-protocol.h>
#include <drivers/pci/pci.h>
#include <kern/kcrt.h>

#include <kern/clock.h>
#include <kern/disk.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/sysctl.h>

#include <limits.h>
#include <stddef.h>
#include <uapi/errno.h>
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/device-io.h"

/* PCI configuration space: the command register and the MSI capability. */
#define AHCI_PCI_COMMAND		0x04U
#define AHCI_PCI_COMMAND_IO		0x0001U
#define AHCI_PCI_COMMAND_MEMORY		0x0002U
#define AHCI_PCI_COMMAND_MASTER		0x0004U
#define AHCI_PCI_MSI_CAPABILITY		0x05U
#define AHCI_PCI_MSI_CONTROL		0x02U
#define AHCI_PCI_MSI_ENABLE		0x0001U

/* Intel functions in RAID (RST) mode keep an AHCI ABAR (ws231 U2). */
#define AHCI_PCI_VENDOR_INTEL		0x8086U
#define AHCI_PCI_CLASS_RAID		0x010400U
#define AHCI_PCI_CLASS_NVME		0x010802U

/* The staging buffer one command moves through, and so the largest command. */
#define AHCI_BOUNCE_SIZE		(64U * 1024U)
#define AHCI_BOUNCE_ALIGN		4096U

/*
 * The slot every command is issued in.  Without NCQ a port runs one command
 * at a time, so only slot 0 has a command table.
 */
#define AHCI_COMMAND_SLOT		0U

/* Bounds of the waits in bring-up and recovery, in milliseconds. */
#define AHCI_ENGINE_STOP_MS		500U
#define AHCI_HBA_RESET_MS		1000U
#define AHCI_COMRESET_HOLD_MS		2U
#define AHCI_LINK_UP_MS			1000U
#define AHCI_LINK_ABSENT_MS		100U
#define AHCI_DEVICE_READY_MS		10000U
#define AHCI_HANDOFF_MS			25U
#define AHCI_HANDOFF_BUSY_MS		2000U

/*
 * How long one command may run.  A spinning disk retrying a hard sector can
 * take tens of seconds before it answers.  IDENTIFY at boot and the flush at
 * shutdown get a shorter bound, so one broken disk cannot hold boot for long.
 */
#define AHCI_COMMAND_TIMEOUT_MS		30000U
#define AHCI_SHORT_TIMEOUT_MS		5000U

/*
 * The longest a waiter sleeps before it looks at the port itself.  A
 * completion found that way means an interrupt was late or lost; those are
 * counted and logged, so a broken interrupt path is visible rather than
 * merely slow.
 */
#define AHCI_POLL_MS			100U

/* How many more times a request runs after a timeout or a link fault. */
#define AHCI_IO_RETRIES			2U

/* The longest model string IDENTIFY carries (words 27 to 46), plus NUL. */
#define AHCI_MODEL_TEXT_SIZE		41U

/*
 * One entry of the Intel RAID-mode table.
 *
 * It names one real operation of the table, binding one Intel device ID in
 * class 01.04.00, so it stays a macro.
 */
#define AHCI_INTEL_RAID(product) \
	{ \
		.vendor = AHCI_PCI_VENDOR_INTEL, \
		.device = (product), \
		.subvendor = DRV_PCI_ANY_ID, \
		.subdevice = DRV_PCI_ANY_ID, \
		.class_code = AHCI_PCI_CLASS_RAID, \
		.class_mask = 0xffffffU, \
	}

/* What the probe's link reset found on one port. */
enum ahci_link {
	AHCI_LINK_PENDING,
	AHCI_LINK_UP,
	AHCI_LINK_ABSENT
};

/*
 * One implemented port of an HBA.
 *
 * Attach creates a record for every implemented port, so the interrupt
 * handler always finds an initialized lock.  The record lives as long as the
 * kernel, because detach is refused.  The command mutex lets one command own
 * the port at a time; the state lock protects what the interrupt handler and
 * the waiter share.
 */
struct ahci_port {
	struct ahci_controller *controller;
	unsigned number;
	volatile uint8_t *registers;

	struct drv_dma_buffer command_list_dma;
	struct drv_dma_buffer received_fis_dma;
	struct drv_dma_buffer table_dma;
	struct drv_dma_buffer bounce_dma;
	unsigned bounce_allocated;

	/* Set once PxCLB and PxFB point at this port's memory; resume rewrites them. */
	unsigned memory_installed;

	struct mutex command_mutex;
	struct spinlock state_lock;
	struct wait_queue waitq;

	/* What the probe's link reset found; used only by the probe. */
	enum ahci_link link;

	/*
	 * Set once the port's engine runs and its interrupts are enabled.
	 * Before that the handler only clears the port's status.
	 */
	unsigned started;

	/*
	 * The state of the one command the port runs (DRV_AHCI_COMMAND_*).
	 * The issuer sets it ACTIVE and the waiter moves it on, both under the
	 * state lock; the interrupt handler only folds status in.
	 */
	unsigned command_state;

	/*
	 * Every PxIS bit seen since the current command was issued.  The
	 * interrupt handler and the waiter both fold the hardware status into
	 * it under the state lock, so neither loses what the other cleared.
	 */
	uint32_t interrupt_status;

	/*
	 * Set when the port is out of service.  From then on every request
	 * fails at once instead of waiting for a dead link.
	 */
	unsigned dead;

	/*
	 * Set when the engine or FIS reception would not stop: the HBA may
	 * still write the port's memory, so that memory is never freed.
	 */
	unsigned dma_unsafe;

	/*
	 * Completions a waiter found only after a poll-interval sleep that no
	 * interrupt ended (drv_ahci_completion_by_polling).  Zero while the
	 * interrupt path works; the log shows it at each power of two.
	 */
	unsigned poll_completions;

	/*
	 * Set by every write command and cleared by a flush that finished, both
	 * under the command mutex.  A close with it set flushes the disk's write
	 * cache, so raw writes reach the medium without waiting for shutdown.
	 */
	unsigned cache_dirty;

	struct drv_ahci_identity identity;
	struct disk *disk;
};

/*
 * One AHCI HBA bound by the PCI core.
 *
 * It is created by attach and never freed, since detach is refused while its
 * disks may be in use.  ports[] is filled by attach before the interrupt is
 * installed and never changes afterwards.
 */
struct ahci_controller {
	struct ahci_controller *next;
	unsigned index;
	struct drv_pci_device *pci;
	struct drv_pci_mapping mapping;
	struct drv_pci_enable_state enable_state;
	volatile uint8_t *registers;
	uint32_t class_code;
	uint32_t capability;
	uint32_t capability2;
	uint32_t ports_implemented;
	uint32_t version;
	unsigned slot_count;

	struct drv_pci_irq irq;
	void *irq_cookie;

	unsigned bar_claimed;
	unsigned enable_state_saved;
	unsigned bar_mapped;
	unsigned irq_allocated;

	/* Set under the registry lock by the probe that takes this HBA. */
	unsigned probed;

	/*
	 * Set by suspend after the ports stopped, cleared by resume.  While it
	 * is set the suspending thread holds every port's command mutex, so
	 * new requests wait instead of failing.
	 */
	unsigned suspended;

	struct ahci_port *ports[DRV_AHCI_PORT_MAX];
};

/*
 * Every attached HBA, newest first.
 *
 * Attach adds to the list and the probe walks it; nothing is ever removed,
 * so a pointer read under the registry lock stays valid without it.
 */
static struct ahci_controller *ahci_controllers;

/*
 * The number the next attached HBA is known by in the log.
 *
 * It only increases, under the registry lock.
 */
static unsigned ahci_next_index;

/* Protects ahci_controllers, ahci_next_index and each HBA's probed flag. */
static struct spinlock ahci_registry_lock;

/*
 * Forward declarations the operation tables need.
 */
static int ahci_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int ahci_detach(struct drv_pci_device *device, unsigned flags);
static void ahci_shutdown(struct drv_pci_device *device);
static int ahci_suspend(struct drv_pci_device *device);
static int ahci_resume(struct drv_pci_device *device);
static void ahci_disk_close(struct disk *disk);
static int ahci_disk_submit(struct disk *disk, struct bio *bio);
static int ahci_disk_ioctl(struct disk *disk, unsigned long request, void *argument);

/* Device operation and registration tables. */
static const struct disk_ops ahci_disk_ops = {
	.close = ahci_disk_close,
	.submit = ahci_disk_submit,
	.ioctl = ahci_disk_ioctl,
};

/*
 * The functions this driver binds.
 *
 * Every function of class 01.06.01 is AHCI.  Intel's RAID (RST) mode
 * reports class 01.04.00 with the same ABAR; those are matched by device
 * ID, the way Linux's ahci driver lists them (from memory of its table:
 * each ID is still to be confirmed on a real machine).
 */
static const struct drv_pci_id ahci_ids[] = {
	{
		.vendor = DRV_PCI_ANY_ID,
		.device = DRV_PCI_ANY_ID,
		.subvendor = DRV_PCI_ANY_ID,
		.subdevice = DRV_PCI_ANY_ID,
		.class_code = DRV_AHCI_PCI_CLASS,
		.class_mask = 0xffffffU,
	},
	/* ICH8R to ICH10R, and the mobile RAID ID later PCHs keep (6th gen on). */
	AHCI_INTEL_RAID(0x2822U),
	AHCI_INTEL_RAID(0x282aU),
	/* 6 and 7 series and C200/C600. */
	AHCI_INTEL_RAID(0x1c04U),
	AHCI_INTEL_RAID(0x1c06U),
	AHCI_INTEL_RAID(0x1d04U),
	AHCI_INTEL_RAID(0x1d06U),
	AHCI_INTEL_RAID(0x1e04U),
	AHCI_INTEL_RAID(0x1e05U),
	AHCI_INTEL_RAID(0x1e06U),
	AHCI_INTEL_RAID(0x1e07U),
	AHCI_INTEL_RAID(0x1e0eU),
	/* 8 and 9 series (Lynx Point, Wildcat Point). */
	AHCI_INTEL_RAID(0x8c04U),
	AHCI_INTEL_RAID(0x8c05U),
	AHCI_INTEL_RAID(0x8c06U),
	AHCI_INTEL_RAID(0x8c07U),
	AHCI_INTEL_RAID(0x8c0eU),
	AHCI_INTEL_RAID(0x8c0fU),
	AHCI_INTEL_RAID(0x9c04U),
	AHCI_INTEL_RAID(0x9c05U),
	AHCI_INTEL_RAID(0x9c06U),
	AHCI_INTEL_RAID(0x9c07U),
	AHCI_INTEL_RAID(0x9c0eU),
	AHCI_INTEL_RAID(0x9c0fU),
	AHCI_INTEL_RAID(0x9c85U),
	AHCI_INTEL_RAID(0x9c87U),
	AHCI_INTEL_RAID(0x9c8fU),
	/* 100 series (Sunrise Point) and later PCHs. */
	AHCI_INTEL_RAID(0xa105U),
	AHCI_INTEL_RAID(0xa106U),
	AHCI_INTEL_RAID(0xa107U),
	AHCI_INTEL_RAID(0xa10fU),
	AHCI_INTEL_RAID(0x9d05U),
	AHCI_INTEL_RAID(0x9d07U),
	AHCI_INTEL_RAID(0x02d7U),
	AHCI_INTEL_RAID(0x06d6U),
};

static struct drv_pci_driver ahci_driver = {
	.name = "ahci",
	.ids = ahci_ids,
	.id_count = sizeof(ahci_ids) / sizeof(ahci_ids[0]),
	.attach = ahci_attach,
	.detach = ahci_detach,
	.shutdown = ahci_shutdown,
	.suspend = ahci_suspend,
	.resume = ahci_resume,
};

/*
 * Forward declarations.
 */
static uint32_t ahci_read(const struct ahci_controller *controller, size_t offset);
static void ahci_write(struct ahci_controller *controller, size_t offset, uint32_t value);
static uint32_t ahci_port_read(const struct ahci_port *port, size_t offset);
static void ahci_port_write(struct ahci_port *port, size_t offset, uint32_t value);
static void ahci_delay_ms(unsigned milliseconds);
static int ahci_port_wait_clear(struct ahci_port *port, size_t offset, uint32_t mask, unsigned milliseconds);
static int ahci_pci_quiesce(struct ahci_controller *controller);
static int ahci_attach_ports(struct ahci_controller *controller);
static void ahci_attach_quiet_ports(struct ahci_controller *controller);
static void ahci_attach_undo(struct ahci_controller *controller);
static void ahci_intel_remap_check(struct ahci_controller *controller);
static int ahci_irq(void *argument);
static void ahci_port_service_locked(struct ahci_port *port);
static void ahci_port_clear_status(struct ahci_port *port);
static void ahci_probe_controller(struct ahci_controller *controller);
static void ahci_bios_handoff(struct ahci_controller *controller);
static int ahci_stop_all_ports(struct ahci_controller *controller);
static int ahci_hba_reset(struct ahci_controller *controller);
static int ahci_port_memory_install(struct ahci_port *port);
static void ahci_port_prepare_link(struct ahci_port *port);
static void ahci_links_reset(struct ahci_controller *controller);
static void ahci_links_wait(struct ahci_controller *controller);
static void ahci_devices_wait(struct ahci_controller *controller);
static void ahci_port_admit(struct ahci_port *port);
static void ahci_port_retire(struct ahci_port *port);
static int ahci_port_engine_stop(struct ahci_port *port);
static int ahci_port_receive_stop(struct ahci_port *port);
static void ahci_port_engine_start(struct ahci_port *port);
static int ahci_port_comreset(struct ahci_port *port);
static int ahci_port_identify(struct ahci_port *port);
static int ahci_port_publish(struct ahci_port *port);
static int ahci_port_execute(struct ahci_port *port, uint8_t command, uint64_t lba, uint32_t count, int write, size_t length, unsigned timeout_ms, int *retryable);
static unsigned ahci_port_wait_command(struct ahci_port *port, uint32_t slot_mask, unsigned timeout_ms);
static int ahci_port_recover(struct ahci_port *port);
static void ahci_port_quarantine(struct ahci_port *port);
static int ahci_port_transfer(struct ahci_port *port, struct bio *bio, size_t *transferred);
static int ahci_port_flush(struct ahci_port *port, unsigned timeout_ms, unsigned retries);
static void ahci_ports_lock(struct ahci_controller *controller);
static void ahci_ports_unlock(struct ahci_controller *controller);
static int ahci_suspend_stop(struct ahci_controller *controller);
static void ahci_resume_start(struct ahci_controller *controller);

/*
 * Registers the AHCI class driver with the PCI bus.
 */
int
drv_pci_ahci_driver_register(
	void)
{
	int error;

	/* Prepares the registry the probe later walks. */
	spin_init(&ahci_registry_lock, LOCK_RANK_DEVICE, "AHCI HBA registry");

	/* Asks the PCI core to bind every AHCI function to this driver. */
	error = drv_pci_driver_register(&ahci_driver);
	if (error != 0)
		return error;

	/* Succeeded: matching HBAs attach as the bus is enumerated. */
	return 0;
}

/*
 * Brings up the ports of every attached HBA and publishes their disks.
 */
void
drv_pci_ahci_probe_ports(
	void)
{
	struct ahci_controller *controller;
	unsigned long irq;

	/* Takes each HBA no probe has taken yet, one at a time. */
	for (;;) {
		irq = spin_lock_irqsave(&ahci_registry_lock);

		/* Finds the first HBA no probe has taken and takes it. */
		controller = ahci_controllers;
		while (controller != NULL && controller->probed)
			controller = controller->next;
		if (controller != NULL)
			controller->probed = 1;

		spin_unlock_irqrestore(&ahci_registry_lock, irq);

		/* Stops once every HBA has been probed. */
		if (controller == NULL)
			break;

		/* Brings up this HBA's ports outside the registry lock. */
		ahci_probe_controller(controller);
	}
}

/*
 * Binds one AHCI function.
 *
 * The order follows NVMe's attach: the function's decoding, bus mastering
 * and message interrupt are turned off before its registers are mapped, so
 * whatever the firmware left running cannot reach memory; only memory
 * decoding comes back.  Bus mastering is turned on by the probe once every
 * port points at this driver's memory.
 */
static int
ahci_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct ahci_controller *controller;
	struct drv_pci_address address;
	struct drv_pci_bar bar;
	const char *stage;
	const char *irq_name;
	size_t first_port_end;
	unsigned irq_count;
	unsigned long irq;
	uint16_t command;
	uint32_t control;
	int error;

	(void)id;

	/* Allocates the controller record. */
	controller = kern_malloc(sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;

	/* Initializes the record and learns where the function sits on the bus. */
	kern_memset(controller, 0, sizeof(*controller));
	controller->pci = device;
	controller->class_code = drv_pci_device_class(device);
	drv_pci_device_address(device, &address);

	/* Takes ownership of the ABAR. */
	stage = "ABAR claim";
	error = drv_pci_device_claim_bar(device, DRV_AHCI_ABAR_INDEX);
	if (error != 0)
		goto fail;
	controller->bar_claimed = 1;

	/* The ABAR must be a memory BAR. */
	stage = "ABAR inspection";
	error = drv_pci_device_bar(device, DRV_AHCI_ABAR_INDEX, &bar);
	if (error != 0)
		goto fail;
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64) {
		error = ENODEV;
		goto fail;
	}

	/* Remembers the PCI enable bits so shutdown can put them back. */
	stage = "PCI command save";
	error = drv_pci_device_save_enable_state(device, &controller->enable_state);
	if (error != 0)
		goto fail;
	controller->enable_state_saved = 1;

	/* Silences the function: no message interrupt, no decoding, no DMA. */
	stage = "PCI quiesce";
	error = ahci_pci_quiesce(controller);
	if (error != 0)
		goto fail;

	/* Maps the ABAR uncached for register access. */
	stage = "ABAR map";
	error = drv_pci_device_map_bar(
		device,
		DRV_AHCI_ABAR_INDEX,
		DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
		&controller->mapping);
	if (error != 0)
		goto fail;
	controller->bar_mapped = 1;

	/* The mapping must reach at least the first port's registers. */
	first_port_end = drv_ahci_port_offset(1U);
	if (controller->mapping.address == NULL ||
	    controller->mapping.size < first_port_end) {
		error = ENODEV;
		goto fail;
	}

	/* Lets the function decode its memory BAR again, and nothing more. */
	stage = "PCI memory enable";
	error = drv_pci_device_enable_memory(device);
	if (error != 0)
		goto fail;

	/* Bus mastering must still be off. */
	error = drv_pci_device_config_read16(device, AHCI_PCI_COMMAND, &command);
	if (error != 0)
		goto fail;
	if ((command & AHCI_PCI_COMMAND_MEMORY) == 0U ||
	    (command & AHCI_PCI_COMMAND_MASTER) != 0U) {
		error = EIO;
		goto fail;
	}

	/* Puts the HBA in AHCI mode with its interrupt off. */
	controller->registers = controller->mapping.address;
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control |= DRV_AHCI_GHC_AE;
	control &= ~DRV_AHCI_GHC_IE;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);

	/* Reads what the HBA says it can do. */
	controller->capability = ahci_read(controller, DRV_AHCI_REG_CAP);
	controller->capability2 = ahci_read(controller, DRV_AHCI_REG_CAP2);
	controller->ports_implemented = ahci_read(controller, DRV_AHCI_REG_PI);
	controller->version = ahci_read(controller, DRV_AHCI_REG_VS);
	controller->slot_count = drv_ahci_slot_count(controller->capability);

	/* A function that does not answer like an AHCI HBA is left alone. */
	stage = "AHCI registers";
	if (controller->version == UINT32_MAX ||
	    controller->version < 0x00010000U ||
	    controller->ports_implemented == 0U) {
		error = ENODEV;
		goto fail;
	}

	/* Logs NVMe drives an Intel RST HBA hides behind its ABAR. */
	if (controller->class_code == AHCI_PCI_CLASS_RAID)
		ahci_intel_remap_check(controller);

	/* Creates a record for each implemented port. */
	stage = "port records";
	error = ahci_attach_ports(controller);
	if (error != 0)
		goto fail;

	/* Silences every port's interrupts and clears what the firmware left. */
	ahci_attach_quiet_ports(controller);

	/* Takes one MSI vector, or the shared line when MSI is absent. */
	stage = "IRQ allocation";
	irq_count = 0;
	error = drv_pci_device_allocate_irqs(
		device,
		DRV_PCI_IRQ_ALLOW_MSI | DRV_PCI_IRQ_ALLOW_INTX,
		1,
		1,
		&controller->irq,
		&irq_count);
	if (error != 0)
		goto fail;
	if (irq_count != 1U) {
		error = EIO;
		goto fail;
	}

	/* Remembers that the vector must be given back if a later step fails. */
	controller->irq_allocated = 1;

	/* Installs the handler that completes commands. */
	stage = "IRQ establishment";
	error = drv_pci_device_establish_irq(
		device,
		&controller->irq,
		ahci_irq,
		controller,
		"ahci",
		&controller->irq_cookie);
	if (error != 0)
		goto fail;

	/* Lets shutdown find this controller from its PCI function. */
	stage = "driver data";
	error = drv_pci_device_set_driver_data(device, controller);
	if (error != 0)
		goto fail;

	/* Publishes the HBA for the probe. */
	irq = spin_lock_irqsave(&ahci_registry_lock);

	controller->index = ahci_next_index;
	ahci_next_index++;
	controller->next = ahci_controllers;
	ahci_controllers = controller;

	spin_unlock_irqrestore(&ahci_registry_lock, irq);

	/* Names the interrupt kind for the log. */
	irq_name = "INTx";
	if (controller->irq.type == DRV_PCI_IRQ_MSI)
		irq_name = "MSI";

	/* Logs what the HBA reports about itself. */
	kern_logf("ahci%u: PCI %04x:%02x:%02x.%u class=%06x version=%08x "
		  "cap=%08x ports=%08x slots=%u %s\n",
		  controller->index,
		  address.segment,
		  address.bus,
		  address.device,
		  address.function,
		  controller->class_code,
		  controller->version,
		  controller->capability,
		  controller->ports_implemented,
		  controller->slot_count,
		  irq_name);

	/* Succeeded: the HBA is bound and waits for the probe. */
	return 0;

fail:
	kern_logf("ahci: attach failed at %s (%d)\n", stage, error);
	ahci_attach_undo(controller);
	kern_free(controller);

	/* Reports why the HBA could not be bound. */
	return error;
}

/* Refuses to unbind an HBA whose disks may be in use. */
static int
ahci_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	(void)device;
	(void)flags;

	/* Hot removal is not handled yet, so the binding stays. */
	return EBUSY;
}

/*
 * Flushes each disk's write cache and stops the HBA's DMA before the machine
 * resets or powers off.
 */
static void
ahci_shutdown(
	struct drv_pci_device *device)
{
	struct ahci_controller *controller;
	struct ahci_port *port;
	uint32_t control;
	unsigned number;
	int acquired;
	int error;

	/* Finds the controller attach recorded on the function. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL)
		return;

	/*
	 * Flushes each disk's write cache once more, so power-off cannot lose
	 * what raw writes or an unmounted disk left in it (ws231 U3: FLUSH
	 * only, no STANDBY IMMEDIATE).  A port another thread still holds is
	 * skipped rather than waited for.
	 */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->disk == NULL)
			continue;

		/* Takes the port only when no command owns it. */
		acquired = mutex_trylock(&port->command_mutex);
		if (!acquired) {
			kern_logf("ahci%u: port %u: busy at shutdown; cache not flushed\n",
				  controller->index,
				  number);
			continue;
		}

		/* Flushes the disk's write cache with the short boot-time bound. */
		error = ahci_port_flush(port, AHCI_SHORT_TIMEOUT_MS, 0U);

		mutex_unlock(&port->command_mutex);

		/* Logs a cache the disk could not flush; shutdown goes on. */
		if (error != 0) {
			kern_logf("ahci%u: port %u: shutdown flush failed (%d)\n",
				  controller->index,
				  number,
				  error);
		}
	}

	/* Turns the HBA's interrupt off. */
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control &= ~DRV_AHCI_GHC_IE;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);

	/* Stops each running port: the engine first, then FIS reception. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (!port->started)
			continue;
		if (port->dma_unsafe)
			continue;

		/* Silences the port, then stops its engine and FIS reception. */
		ahci_port_write(port, DRV_AHCI_PX_IE, 0U);
		error = ahci_port_engine_stop(port);
		if (error == 0)
			error = ahci_port_receive_stop(port);
		if (error != 0) {
			kern_logf("ahci%u: port %u: does not stop at shutdown\n",
				  controller->index,
				  number);
		}
	}

	/* Gives the PCI enable bits back as attach found them. */
	(void)drv_pci_device_restore_enable_state(device, &controller->enable_state);
}

/*
 * Stops the HBA for S0 idle.
 *
 * Every port's command mutex is taken, which lets the running commands
 * finish and holds new requests back until resume.  Each disk's cache is
 * flushed, the ports are stopped and bus mastering goes off.  A suspend that
 * cannot finish starts the ports again and reports why.
 */
static int
ahci_suspend(
	struct drv_pci_device *device)
{
	struct ahci_controller *controller;
	int error;

	/* Finds the controller attach recorded on the function. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL)
		return ENODEV;

	/* A second suspend before the resume is refused. */
	if (controller->suspended)
		return EBUSY;

	/* Waits for the running commands and holds new requests back. */
	ahci_ports_lock(controller);

	/* Flushes, stops the ports and takes the HBA off the bus. */
	error = ahci_suspend_stop(controller);
	if (error != 0) {
		ahci_resume_start(controller);
		ahci_ports_unlock(controller);
		kern_logf("ahci%u: suspend failed (%d); the HBA runs on\n",
			  controller->index,
			  error);
		return error;
	}

	/* suspended tells resume that it owns the held command mutexes. */
	controller->suspended = 1;
	kern_logf("ahci%u: suspended\n", controller->index);

	/* Succeeded: the HBA is stopped until ahci_resume(). */
	return 0;
}

/*
 * Starts the HBA again after S0 idle.
 *
 * The PCI core has given the configuration back; the ports are pointed at
 * their memory again, their links reset and their engines started, and the
 * held requests are let in.  A disk that does not come back fails its
 * requests.
 */
static int
ahci_resume(
	struct drv_pci_device *device)
{
	struct ahci_controller *controller;

	/* Finds the controller attach recorded on the function. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL)
		return ENODEV;

	/* A resume without a suspend has nothing to start. */
	if (!controller->suspended)
		return 0;

	/* Starts the ports, then lets the held requests in. */
	ahci_resume_start(controller);
	controller->suspended = 0;
	ahci_ports_unlock(controller);
	kern_logf("ahci%u: resumed\n", controller->index);

	/* Succeeded: the HBA takes requests again. */
	return 0;
}

/*
 * Flushes the disk's write cache when a write since the last flush may
 * still sit in it.
 *
 * Raw writes to the whole disk (dd to /dev/sdN) are not covered by a
 * filesystem's sync, so the close that ends them flushes, the way FreeBSD's
 * ada(4) does.  A failed flush is logged; the close itself cannot fail.
 */
static void
ahci_disk_close(
	struct disk *disk)
{
	struct ahci_port *port;
	int error;

	/* Ignores a close without a disk this driver published. */
	if (disk == NULL)
		return;
	port = disk->d_data;
	if (port == NULL)
		return;

	/* Flushes with the port to itself, only when a write is unflushed. */
	error = 0;
	mutex_lock(&port->command_mutex);

	if (port->cache_dirty && !port->dead)
		error = ahci_port_flush(port, AHCI_COMMAND_TIMEOUT_MS, AHCI_IO_RETRIES);

	mutex_unlock(&port->command_mutex);

	/* Logs a cache the disk could not flush; the data may not be durable. */
	if (error != 0) {
		kern_logf("ahci%u: port %u: flush at close failed (%d)\n",
			  port->controller->index,
			  port->number,
			  error);
	}
}

/* Serves one block request against an AHCI disk. */
static int
ahci_disk_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct ahci_port *port;
	size_t transferred;
	uint64_t last;
	int error;

	/* Refuses a request without a disk this driver published. */
	if (disk == NULL || bio == NULL)
		return EINVAL;
	port = disk->d_data;
	if (port == NULL)
		return EINVAL;

	/* Refuses an operation the driver does not implement. */
	if (bio->b_op != BIO_READ &&
	    bio->b_op != BIO_WRITE &&
	    bio->b_op != BIO_FLUSH)
		return EOPNOTSUPP;

	/*
	 * A read or a write must name blocks that are all on the disk.  The
	 * disk layer has already turned a partition's block into the whole
	 * disk's in b_mapped_block.
	 */
	if (bio->b_op != BIO_FLUSH) {
		if (bio->b_block_count == 0U)
			return EINVAL;
		if (bio->b_mapped_block >= port->identity.sector_count)
			return EOVERFLOW;
		last = bio->b_mapped_block + bio->b_block_count;
		if (last > port->identity.sector_count)
			return EOVERFLOW;
		if (last < bio->b_mapped_block)
			return EOVERFLOW;
	}

	/* Runs the request with the port to itself. */
	transferred = 0;
	mutex_lock(&port->command_mutex);

	if (bio->b_op == BIO_FLUSH) {
		error = ahci_port_flush(port, AHCI_COMMAND_TIMEOUT_MS, AHCI_IO_RETRIES);
	} else {
		error = ahci_port_transfer(port, bio, &transferred);
	}

	mutex_unlock(&port->command_mutex);

	/* Completes the request with what was moved before any failure. */
	bio_complete(bio, error, transferred);

	/* Succeeded: the request is completed. */
	return 0;
}

/* Reports a disk's default geometry for partition tools. */
static int
ahci_disk_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	struct disk_geometry *geometry;
	struct ahci_port *port;
	uint64_t cylinders;

	/* Only the geometry request is answered. */
	if (request != DISK_IOCTL_GET_GEOMETRY)
		return EOPNOTSUPP;

	/* Refuses a missing disk or a missing answer buffer. */
	if (disk == NULL)
		return EINVAL;
	port = disk->d_data;
	geometry = argument;
	if (port == NULL || geometry == NULL)
		return EINVAL;

	/* Uses the disk's own geometry when IDENTIFY still reports one. */
	if (port->identity.heads != 0U && port->identity.sectors_per_track != 0U) {
		geometry->cylinders = port->identity.cylinders;
		geometry->heads = port->identity.heads;
		geometry->sectors_per_track = port->identity.sectors_per_track;

		/* Succeeded: the disk's reported geometry. */
		return 0;
	}

	/* Otherwise uses the conventional 255 heads of 63 sectors. */
	cylinders = port->identity.sector_count / (255U * 63U);
	if (cylinders > UINT32_MAX)
		cylinders = UINT32_MAX;
	geometry->cylinders = (uint32_t)cylinders;
	geometry->heads = 255U;
	geometry->sectors_per_track = 63U;

	/* Succeeded: the synthesized geometry. */
	return 0;
}

/* Reads one HBA register. */
static uint32_t
ahci_read(
	const struct ahci_controller *controller,
	size_t offset)
{
	uint32_t value;

	/* Reads the register through the uncached mapping. */
	value = kern_mmio_read32(controller->registers + offset);

	/* Reports the register's value. */
	return value;
}

/* Writes one HBA register. */
static void
ahci_write(
	struct ahci_controller *controller,
	size_t offset,
	uint32_t value)
{
	/* Writes the register through the uncached mapping. */
	kern_mmio_write32(controller->registers + offset, value);
}

/* Reads one port register. */
static uint32_t
ahci_port_read(
	const struct ahci_port *port,
	size_t offset)
{
	uint32_t value;

	/* Reads the register inside the port's block. */
	value = kern_mmio_read32(port->registers + offset);

	/* Reports the register's value. */
	return value;
}

/* Writes one port register. */
static void
ahci_port_write(
	struct ahci_port *port,
	size_t offset,
	uint32_t value)
{
	/* Writes the register inside the port's block. */
	kern_mmio_write32(port->registers + offset, value);
}

/* Lets at least the given time pass, yielding the CPU meanwhile. */
static void
ahci_delay_ms(
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;

	/* One extra tick covers the part of the current tick already gone. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;

	/* Yields until the deadline passes. */
	for (;;) {
		now = sched_ticks();
		if (now >= deadline)
			break;
		sched_yield();
	}
}

/* Waits for the given bits of a port register to read as zero. */
static int
ahci_port_wait_clear(
	struct ahci_port *port,
	size_t offset,
	uint32_t mask,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t value;

	/* Polls the register, yielding between reads, until the deadline. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (;;) {
		/* Stops as soon as the bits read as zero. */
		value = ahci_port_read(port, offset);
		if ((value & mask) == 0U)
			return 0;

		/* Gives up once the wait has run its course. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* Gives the CPU away before the next look. */
		sched_yield();
	}

	/* The bits stayed set for the whole wait. */
	return ETIMEDOUT;
}

/* Turns off the function's message interrupt, decoding and bus mastering. */
static int
ahci_pci_quiesce(
	struct ahci_controller *controller)
{
	unsigned capability;
	uint16_t control;
	uint16_t command;
	int error;

	/*
	 * Masks an MSI the firmware may have enabled; the interrupt allocation
	 * enables it again with this kernel's vector.
	 */
	error = drv_pci_device_find_capability(
		controller->pci,
		AHCI_PCI_MSI_CAPABILITY,
		&capability);
	if (error == 0) {
		error = drv_pci_device_config_read16(
			controller->pci,
			capability + AHCI_PCI_MSI_CONTROL,
			&control);
		if (error != 0)
			return error;

		/* Clears the MSI enable bit and writes the control word back. */
		control &= (uint16_t)~AHCI_PCI_MSI_ENABLE;
		error = drv_pci_device_config_write16(
			controller->pci,
			capability + AHCI_PCI_MSI_CONTROL,
			control);
		if (error != 0)
			return error;
	} else if (error != ENOENT) {
		/* A capability list that cannot be read is reported. */
		return error;
	}

	/* Clears I/O, memory and bus-master enable together. */
	error = drv_pci_device_config_read16(controller->pci, AHCI_PCI_COMMAND, &command);
	if (error != 0)
		return error;

	command &= (uint16_t)~(AHCI_PCI_COMMAND_IO | AHCI_PCI_COMMAND_MEMORY |
			       AHCI_PCI_COMMAND_MASTER);

	/* Writes the command register back with all three bits clear. */
	error = drv_pci_device_config_write16(controller->pci, AHCI_PCI_COMMAND, command);
	if (error != 0)
		return error;

	/* Succeeded: the function can neither be reached nor reach memory. */
	return 0;
}

/* Creates the record of every implemented port inside the mapping. */
static int
ahci_attach_ports(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	size_t needed;
	unsigned number;

	/* Allocates and initializes one record per implemented port. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		if ((controller->ports_implemented & (UINT32_C(1) << number)) == 0U)
			continue;

		/* A port whose registers lie past the mapping is not used. */
		needed = drv_ahci_port_offset(number) + DRV_AHCI_PORT_STRIDE;
		if (needed > controller->mapping.size)
			continue;

		/* Allocates the port record. */
		port = kern_malloc(sizeof(*port));
		if (port == NULL)
			return ENOMEM;

		/* Initializes the record and its locks. */
		kern_memset(port, 0, sizeof(*port));
		port->controller = controller;
		port->number = number;
		port->registers = controller->registers + drv_ahci_port_offset(number);
		port->link = AHCI_LINK_PENDING;
		port->command_state = DRV_AHCI_COMMAND_IDLE;
		(void)mutex_init(&port->command_mutex, LOCK_RANK_DISK, "AHCI port command");
		spin_init(&port->state_lock, LOCK_RANK_DISK, "AHCI port state");
		waitq_init(&port->waitq, "AHCI port command");
		controller->ports[number] = port;
	}

	/* Succeeded: every reachable port has a record. */
	return 0;
}

/*
 * Turns off every port's interrupts and clears the status the firmware left,
 * in the order AHCI requires: PxSERR, then PxIS, then IS.
 */
static void
ahci_attach_quiet_ports(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint32_t pending;
	uint32_t control;
	unsigned number;

	/* Silences and clears each port. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;

		/* Masks the port's interrupts, then clears its recorded status. */
		ahci_port_write(port, DRV_AHCI_PX_IE, 0U);
		ahci_port_clear_status(port);
	}

	/* Turns command completion coalescing off where the HBA has it. */
	if ((controller->capability & DRV_AHCI_CAP_CCCS) != 0U) {
		control = ahci_read(controller, DRV_AHCI_REG_CCC_CTL);
		control &= ~DRV_AHCI_CCC_CTL_EN;
		ahci_write(controller, DRV_AHCI_REG_CCC_CTL, control);
	}

	/* Clears the HBA's summary of the ports' interrupts. */
	pending = ahci_read(controller, DRV_AHCI_REG_IS);
	ahci_write(controller, DRV_AHCI_REG_IS, pending);
}

/* Gives back what a failed attach had taken, newest first. */
static void
ahci_attach_undo(
	struct ahci_controller *controller)
{
	unsigned number;

	/* Removes the handler and gives back the vector. */
	if (controller->irq_cookie != NULL)
		drv_pci_device_disestablish_irq(controller->pci, controller->irq_cookie);
	if (controller->irq_allocated)
		drv_pci_device_free_irqs(controller->pci, &controller->irq, 1);

	/* Frees the port records; no port was started. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		if (controller->ports[number] != NULL)
			kern_free(controller->ports[number]);
		controller->ports[number] = NULL;
	}

	/* Unmaps the registers. */
	if (controller->bar_mapped)
		drv_pci_device_unmap_bar(controller->pci, &controller->mapping);

	/* Puts the PCI enable bits back as attach found them. */
	if (controller->enable_state_saved) {
		(void)drv_pci_device_restore_enable_state(
			controller->pci,
			&controller->enable_state);
	}

	/* Releases the ABAR. */
	if (controller->bar_claimed)
		drv_pci_device_release_bar(controller->pci, DRV_AHCI_ABAR_INDEX);
}

/*
 * Logs NVMe drives an Intel RST HBA has remapped behind its ABAR.
 *
 * Such drives are not visible on PCI and are not reachable by this driver
 * (ws231 U2); the log line says why a known NVMe drive is missing.
 */
static void
ahci_intel_remap_check(
	struct ahci_controller *controller)
{
	uint32_t vendor_capability;
	uint32_t remap_capability;
	uint32_t device_class;
	unsigned remapped;
	unsigned index;

	/* The remap registers exist only on Intel parts with a large enough ABAR. */
	if (controller->mapping.size < DRV_AHCI_INTEL_REMAP_END)
		return;

	/* Intel's vendor-specific capability says whether the remap exists. */
	vendor_capability = ahci_read(controller, DRV_AHCI_REG_INTEL_VSCAP);
	if ((vendor_capability & 1U) == 0U)
		return;

	/* Counts the remapped devices that are NVMe controllers. */
	remap_capability = ahci_read(controller, DRV_AHCI_REG_INTEL_REMAP_CAP);
	remapped = 0;
	for (index = 0; index < DRV_AHCI_INTEL_REMAP_MAX; index++) {
		if ((remap_capability & (UINT32_C(1) << index)) == 0U)
			continue;

		/* Reads the class code of the device remapped in this slot. */
		device_class = ahci_read(
			controller,
			DRV_AHCI_REG_INTEL_REMAP_DCC + (size_t)index * DRV_AHCI_PORT_STRIDE);
		if (device_class == AHCI_PCI_CLASS_NVME)
			remapped++;
	}

	/* Says so when an NVMe drive is hidden. */
	if (remapped != 0U) {
		kern_logf("ahci: Intel RST hides %u NVMe drive(s); switch the "
			  "firmware's SATA mode to AHCI to reach them\n",
			  remapped);
	}
}

/* Takes an HBA interrupt: collects each port's status and wakes its waiter. */
static int
ahci_irq(
	void *argument)
{
	struct ahci_controller *controller;
	struct ahci_port *port;
	unsigned long irq;
	uint32_t pending;
	unsigned number;

	/* Recovers the HBA this interrupt was established for. */
	controller = argument;

	/* Reads which ports raised the interrupt. */
	pending = ahci_read(controller, DRV_AHCI_REG_IS);
	if (pending == 0U || pending == UINT32_MAX)
		return 0;

	/* Clears and records each raising port's status. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		if ((pending & (UINT32_C(1) << number)) == 0U)
			continue;
		port = controller->ports[number];
		if (port == NULL)
			continue;

		/* Collects this port's status under its state lock. */
		irq = spin_lock_irqsave(&port->state_lock);

		ahci_port_service_locked(port);

		spin_unlock_irqrestore(&port->state_lock, irq);
	}

	/* Acknowledges the ports only after their own status was cleared. */
	ahci_write(controller, DRV_AHCI_REG_IS, pending);

	/* Succeeded: the interrupt was this HBA's. */
	return 1;
}

/*
 * Folds a port's interrupt status into its record and wakes the waiter.
 *
 * A port that has not started only has its status cleared.
 */
static void
ahci_port_service_locked(
	struct ahci_port *port)
{
	uint32_t status;
	uint32_t errors;

	/* Reads what the port raised since the last look. */
	status = ahci_port_read(port, DRV_AHCI_PX_IS);
	if (status == 0U || status == UINT32_MAX)
		return;

	/*
	 * A PhyRdy or connect change is held by PxSERR, not PxIS; clearing the
	 * SError bits is what lets those two drop.
	 */
	if ((status & (DRV_AHCI_PX_IS_PRCS | DRV_AHCI_PX_IS_PCS)) != 0U) {
		errors = ahci_port_read(port, DRV_AHCI_PX_SERR);
		ahci_port_write(port, DRV_AHCI_PX_SERR, errors);
	}

	/*
	 * Clears exactly the bits read, so a later event is not lost, and then
	 * the port's summary bit, which a waiter that got here before the
	 * interrupt handler would otherwise leave set.
	 */
	ahci_port_write(port, DRV_AHCI_PX_IS, status);
	ahci_write(port->controller, DRV_AHCI_REG_IS, UINT32_C(1) << port->number);

	/* A port that has not started has no waiter to tell. */
	if (!port->started)
		return;

	/* Keeps the bits for the command's waiter and wakes it. */
	port->interrupt_status |= status;
	waitq_wake_all(&port->waitq);
}

/* Clears a port's SError and interrupt status. */
static void
ahci_port_clear_status(
	struct ahci_port *port)
{
	uint32_t errors;
	uint32_t status;

	/* PxSERR first, since it holds two of the PxIS bits. */
	errors = ahci_port_read(port, DRV_AHCI_PX_SERR);
	ahci_port_write(port, DRV_AHCI_PX_SERR, errors);
	status = ahci_port_read(port, DRV_AHCI_PX_IS);
	ahci_port_write(port, DRV_AHCI_PX_IS, status);
}

/*
 * Takes an HBA over from the firmware and brings up its ports.
 *
 * Every port is stopped and pointed at this driver's memory before bus
 * mastering is turned on.  The link resets run on all ports at once, so
 * empty ports cost one short wait together rather than one each.  The HBA
 * interrupt is turned on once, after every port is set up.
 */
static void
ahci_probe_controller(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint32_t control;
	uint32_t pending;
	unsigned number;
	int error;

	/* Asks the firmware to let go of the HBA where it supports handoff. */
	if ((controller->capability2 & DRV_AHCI_CAP2_BOH) != 0U)
		ahci_bios_handoff(controller);

	/* Stops every port; an HBA whose ports will not stop gets a reset. */
	error = ahci_stop_all_ports(controller);
	if (error != 0)
		error = ahci_hba_reset(controller);
	if (error != 0) {
		kern_logf("ahci%u: ports do not stop; HBA left without bus mastering\n",
			  controller->index);
		return;
	}

	/* Points each port at this driver's command list and FIS area. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;

		/* Allocates the port's DMA memory and points the port at it. */
		error = ahci_port_memory_install(port);
		if (error != 0) {
			kern_logf("ahci%u: port %u: DMA memory unavailable (%d)\n",
				  controller->index,
				  number,
				  error);
			port->dead = 1;
		}
	}

	/* Lets the HBA fetch commands and post FISes, now into this memory only. */
	error = drv_pci_device_set_bus_master(controller->pci, true);
	if (error != 0) {
		kern_logf("ahci%u: bus mastering refused (%d)\n",
			  controller->index,
			  error);
		return;
	}

	/* Configures each port's power and FIS reception for the link reset. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->dead)
			continue;

		/* Powers the port up and starts FIS reception. */
		ahci_port_prepare_link(port);
	}

	/* Resets every link, then waits for links and devices together. */
	ahci_links_reset(controller);
	ahci_links_wait(controller);
	ahci_devices_wait(controller);

	/* Starts each port with an ATA disk behind it; stops the others. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->dead)
			continue;

		/* Starts the port if an ATA disk answers, otherwise stops it. */
		ahci_port_admit(port);
	}

	/* Turns the HBA's interrupt on, once, after every port is set up. */
	pending = ahci_read(controller, DRV_AHCI_REG_IS);
	ahci_write(controller, DRV_AHCI_REG_IS, pending);
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control |= DRV_AHCI_GHC_AE | DRV_AHCI_GHC_IE;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);

	/* Identifies and publishes each started disk. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (!port->started)
			continue;

		/* Reads the disk's identity, then publishes it to the disk layer. */
		error = ahci_port_identify(port);
		if (error == 0)
			error = ahci_port_publish(port);
		if (error != 0)
			ahci_port_retire(port);
	}
}

/* Requests HBA ownership from the firmware (AHCI 1.3 section 10.6.3). */
static void
ahci_bios_handoff(
	struct ahci_controller *controller)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t handoff;

	/* Sets OS ownership; the firmware answers by clearing its own bit. */
	handoff = ahci_read(controller, DRV_AHCI_REG_BOHC);
	handoff |= DRV_AHCI_BOHC_OOS;
	ahci_write(controller, DRV_AHCI_REG_BOHC, handoff);

	/* Waits 25 ms for the firmware to release the HBA. */
	ahci_delay_ms(AHCI_HANDOFF_MS);
	handoff = ahci_read(controller, DRV_AHCI_REG_BOHC);

	/* A firmware that says it is busy gets up to two seconds more. */
	if ((handoff & DRV_AHCI_BOHC_BB) != 0U) {
		deadline = sched_ticks() + kern_ms_to_ticks(AHCI_HANDOFF_BUSY_MS);
		while ((handoff & DRV_AHCI_BOHC_BOS) != 0U) {
			/* Stops waiting at the deadline. */
			now = sched_ticks();
			if (now >= deadline)
				break;

			/* Gives the CPU away, then looks at the handoff register again. */
			sched_yield();
			handoff = ahci_read(controller, DRV_AHCI_REG_BOHC);
		}
	}

	/* Goes on regardless; a firmware that never lets go is only logged. */
	if ((handoff & DRV_AHCI_BOHC_BOS) != 0U) {
		kern_logf("ahci%u: firmware kept HBA ownership (%08x)\n",
			  controller->index,
			  handoff);
	}
}

/* Stops every port's engine and then its FIS reception. */
static int
ahci_stop_all_ports(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint32_t command;
	unsigned number;
	int failed;
	int error;

	/* Stops each port in the order AHCI requires: ST, then FRE. */
	failed = 0;
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;

		/* Stops the engine, then FIS reception. */
		error = ahci_port_engine_stop(port);
		if (error == 0)
			error = ahci_port_receive_stop(port);
		if (error != 0) {
			command = ahci_port_read(port, DRV_AHCI_PX_CMD);
			kern_logf("ahci%u: port %u: does not stop (cmd=%08x)\n",
				  controller->index,
				  number,
				  command);
			failed = 1;
		}
	}

	/* Reports a port that kept running. */
	if (failed)
		return ETIMEDOUT;

	/* Succeeded: no port fetches commands or posts FISes. */
	return 0;
}

/*
 * Resets the whole HBA when a port would not stop, and checks that every
 * port is stopped afterwards.
 */
static int
ahci_hba_reset(
	struct ahci_controller *controller)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t control;
	int error;

	/* Sets GHC.HR; the HBA clears it when the reset is done. */
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control |= DRV_AHCI_GHC_HR;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);

	/* Waits up to a second for the reset to finish. */
	deadline = sched_ticks() + kern_ms_to_ticks(AHCI_HBA_RESET_MS) + 1U;
	for (;;) {
		control = ahci_read(controller, DRV_AHCI_REG_GHC);
		if ((control & DRV_AHCI_GHC_HR) == 0U)
			break;

		/* An HBA still resetting at the deadline is given up. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Gives the CPU away before the next look. */
		sched_yield();
	}

	/* The reset can drop AHCI mode; it is set again, interrupt off. */
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control |= DRV_AHCI_GHC_AE;
	control &= ~DRV_AHCI_GHC_IE;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);

	/* The reset also cleared the ports' interrupt masks and status. */
	ahci_attach_quiet_ports(controller);

	/* Checks that the reset left every port stopped. */
	error = ahci_stop_all_ports(controller);
	if (error != 0)
		return error;

	/* Succeeded: the HBA is reset and every port is stopped. */
	return 0;
}

/*
 * Allocates a port's command list, FIS area and command table, and points
 * the port at them.  The port must be stopped.  What was allocated stays
 * with the port for good, since the port's registers may point at it.
 */
static int
ahci_port_memory_install(
	struct ahci_port *port)
{
	struct ahci_controller *controller;
	struct drv_dma_device *dma;
	uint64_t highest;
	uint64_t end;
	int error;

	/* Finds the HBA and the DMA allocator its function uses. */
	controller = port->controller;
	dma = drv_pci_device_dma(controller->pci);

	/* The command list: 32 headers of 32 bytes, 1 KiB aligned. */
	error = drv_dma_alloc_coherent(
		dma,
		DRV_AHCI_COMMAND_LIST_SIZE,
		DRV_AHCI_COMMAND_LIST_ALIGN,
		&port->command_list_dma);
	if (error != 0)
		return error;

	/* The received FIS area, 256 bytes aligned. */
	error = drv_dma_alloc_coherent(
		dma,
		DRV_AHCI_RECEIVED_FIS_SIZE,
		DRV_AHCI_RECEIVED_FIS_ALIGN,
		&port->received_fis_dma);
	if (error != 0)
		return error;

	/* The command table of slot 0, 128 bytes aligned. */
	error = drv_dma_alloc_coherent(
		dma,
		DRV_AHCI_TABLE_SIZE,
		DRV_AHCI_TABLE_ALIGN,
		&port->table_dma);
	if (error != 0)
		return error;

	/* Clears what the HBA will read before it can read it. */
	kern_memset(port->command_list_dma.address, 0, DRV_AHCI_COMMAND_LIST_SIZE);
	kern_memset(port->received_fis_dma.address, 0, DRV_AHCI_RECEIVED_FIS_SIZE);
	kern_memset(port->table_dma.address, 0, DRV_AHCI_TABLE_SIZE);

	/* Finds the end of the highest of the three areas. */
	highest = port->command_list_dma.device_address + DRV_AHCI_COMMAND_LIST_SIZE;
	end = port->received_fis_dma.device_address + DRV_AHCI_RECEIVED_FIS_SIZE;
	if (end > highest)
		highest = end;
	end = port->table_dma.device_address + DRV_AHCI_TABLE_SIZE;
	if (end > highest)
		highest = end;

	/* An HBA without 64-bit addressing reaches only the low 4 GiB. */
	if ((controller->capability & DRV_AHCI_CAP_S64A) == 0U &&
	    highest > UINT64_C(0x100000000))
		return ERANGE;

	/* Points the port at its command list and FIS area. */
	port->memory_installed = 1;
	ahci_port_write(
		port,
		DRV_AHCI_PX_CLB,
		(uint32_t)(port->command_list_dma.device_address & 0xffffffffU));
	ahci_port_write(
		port,
		DRV_AHCI_PX_CLBU,
		(uint32_t)(port->command_list_dma.device_address >> 32));
	ahci_port_write(
		port,
		DRV_AHCI_PX_FB,
		(uint32_t)(port->received_fis_dma.device_address & 0xffffffffU));
	ahci_port_write(
		port,
		DRV_AHCI_PX_FBU,
		(uint32_t)(port->received_fis_dma.device_address >> 32));

	/* Succeeded: the port's DMA lands in this driver's memory. */
	return 0;
}

/*
 * Powers a stopped port up, turns link power management and FIS-based
 * switching off, and starts FIS reception, ready for the link reset.
 */
static void
ahci_port_prepare_link(
	struct ahci_port *port)
{
	struct ahci_controller *controller;
	uint32_t command;
	uint32_t value;

	/* Finds the HBA whose capabilities say what the port has. */
	controller = port->controller;

	/* FIS-based switching is for port multipliers, which are not used. */
	if ((controller->capability & DRV_AHCI_CAP_FBSS) != 0U) {
		value = ahci_port_read(port, DRV_AHCI_PX_FBS);
		value &= ~DRV_AHCI_PX_FBS_EN;
		ahci_port_write(port, DRV_AHCI_PX_FBS, value);
	}

	/* Automatic DevSleep is a power state the driver does not manage. */
	if ((controller->capability2 & DRV_AHCI_CAP2_SDS) != 0U) {
		value = ahci_port_read(port, DRV_AHCI_PX_DEVSLP);
		value &= ~DRV_AHCI_PX_DEVSLP_ADSE;
		ahci_port_write(port, DRV_AHCI_PX_DEVSLP, value);
	}

	/*
	 * Spins the device up (staggered spin-up), powers it on where the
	 * port controls power, keeps the link active without aggressive power
	 * management, and turns FIS reception on.
	 */
	command = ahci_port_read(port, DRV_AHCI_PX_CMD);
	command |= DRV_AHCI_PX_CMD_SUD;
	if ((controller->capability & DRV_AHCI_CAP_CPD) != 0U)
		command |= DRV_AHCI_PX_CMD_POD;
	command &= ~(DRV_AHCI_PX_CMD_ALPE | DRV_AHCI_PX_CMD_ASP);
	command &= ~DRV_AHCI_PX_CMD_ICC_MASK;
	command |= DRV_AHCI_PX_CMD_ICC_ACTIVE;
	command |= DRV_AHCI_PX_CMD_FRE;
	ahci_port_write(port, DRV_AHCI_PX_CMD, command);
}

/*
 * Sends COMRESET on every prepared port at once and releases it after the
 * hold time.  The speed limit is kept; Partial and Slumber are refused.
 */
static void
ahci_links_reset(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint32_t control;
	unsigned number;

	/* Asserts COMRESET on every port. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->dead)
			continue;

		/* Starts COMRESET on the port, keeping its speed limit. */
		control = ahci_port_read(port, DRV_AHCI_PX_SCTL);
		control &= DRV_AHCI_PX_SCTL_SPD_MASK;
		control |= DRV_AHCI_PX_SCTL_IPM_NONE | DRV_AHCI_PX_SCTL_DET_INIT;
		ahci_port_write(port, DRV_AHCI_PX_SCTL, control);
		port->link = AHCI_LINK_PENDING;
	}

	/* Holds it for at least a millisecond. */
	ahci_delay_ms(AHCI_COMRESET_HOLD_MS);

	/* Releases it on every port. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->dead)
			continue;

		/* Releases COMRESET so the PHY starts negotiating. */
		control = ahci_port_read(port, DRV_AHCI_PX_SCTL);
		control &= ~DRV_AHCI_PX_SCTL_DET_MASK;
		ahci_port_write(port, DRV_AHCI_PX_SCTL, control);
	}
}

/*
 * Waits for the links of all ports together.
 *
 * A port with an established link is up.  A port that still shows no device
 * at all after 100 ms is empty.  A port that sees a device but no link yet
 * gets up to one second.
 */
static void
ahci_links_wait(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint64_t absent_after;
	uint64_t deadline;
	uint64_t now;
	uint32_t detection;
	unsigned number;
	unsigned pending;

	/* Sets the two deadlines the wait is judged against. */
	now = sched_ticks();
	absent_after = now + kern_ms_to_ticks(AHCI_LINK_ABSENT_MS) + 1U;
	deadline = now + kern_ms_to_ticks(AHCI_LINK_UP_MS) + 1U;

	/* Looks at every pending port until none is left or time runs out. */
	for (;;) {
		/* Looks at each port whose link is still undecided. */
		now = sched_ticks();
		pending = 0;
		for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
			port = controller->ports[number];
			if (port == NULL)
				continue;
			if (port->dead)
				continue;
			if (port->link != AHCI_LINK_PENDING)
				continue;

			/* Sorts the port by what its PHY reports. */
			detection = ahci_port_read(port, DRV_AHCI_PX_SSTS) &
				    DRV_AHCI_PX_SSTS_DET_MASK;
			if (detection == DRV_AHCI_PX_SSTS_DET_PHY) {
				port->link = AHCI_LINK_UP;
			} else if (detection == DRV_AHCI_PX_SSTS_DET_NONE && now >= absent_after) {
				port->link = AHCI_LINK_ABSENT;
			} else if (now >= deadline) {
				port->link = AHCI_LINK_ABSENT;
			} else {
				pending++;
			}
		}

		/* Stops once every port is sorted. */
		if (pending == 0U)
			break;

		/* Gives the CPU away before the next look. */
		sched_yield();
	}
}

/*
 * Clears the link errors of every linked port and waits, for all of them
 * together, until their devices clear BSY and DRQ.  A device still busy at
 * the end of the wait is treated as absent.
 */
static void
ahci_devices_wait(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint64_t deadline;
	uint64_t now;
	uint32_t task_file;
	uint32_t command;
	unsigned number;
	unsigned pending;
	unsigned expired;

	/*
	 * Clears the errors the link bring-up recorded, and makes sure FIS
	 * reception is on: the device's first D2H FIS is what clears BSY in
	 * PxTFD, and an HBA that dropped FRE across the reset would leave the
	 * task file at its reset value.
	 */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->link != AHCI_LINK_UP)
			continue;

		/* Clears every recorded link error, then makes sure FRE is on. */
		ahci_port_write(port, DRV_AHCI_PX_SERR, UINT32_MAX);
		command = ahci_port_read(port, DRV_AHCI_PX_CMD);
		if ((command & DRV_AHCI_PX_CMD_FRE) == 0U)
			ahci_port_write(port, DRV_AHCI_PX_CMD, command | DRV_AHCI_PX_CMD_FRE);
	}

	/* Waits until no linked device is busy, or the time runs out. */
	deadline = sched_ticks() + kern_ms_to_ticks(AHCI_DEVICE_READY_MS) + 1U;
	for (;;) {
		/* Notes whether the wait has run out before this look. */
		now = sched_ticks();
		expired = 0;
		if (now >= deadline)
			expired = 1;

		/* Looks at each linked port's task file. */
		pending = 0;
		for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
			port = controller->ports[number];
			if (port == NULL)
				continue;
			if (port->link != AHCI_LINK_UP)
				continue;

			/* An idle device is ready. */
			task_file = ahci_port_read(port, DRV_AHCI_PX_TFD);
			if ((task_file & (DRV_AHCI_PX_TFD_BSY | DRV_AHCI_PX_TFD_DRQ)) == 0U)
				continue;

			/* A device still busy at the deadline is given up. */
			if (expired) {
				kern_logf("ahci%u: port %u: device stays busy (tfd=%08x)\n",
					  controller->index,
					  number,
					  task_file);
				port->link = AHCI_LINK_ABSENT;
				continue;
			}

			/* Counts the device as still busy; the loop looks again. */
			pending++;
		}

		/* Stops once no device is busy any more. */
		if (pending == 0U)
			break;

		/* Gives the CPU away before the next look. */
		sched_yield();
	}
}

/*
 * Starts a port whose link came up with an ATA disk behind it, and stops
 * FIS reception on every other port.
 */
static void
ahci_port_admit(
	struct ahci_port *port)
{
	struct ahci_controller *controller;
	struct drv_dma_device *dma;
	uint32_t signature;
	unsigned device_class;
	int error;

	/* Finds the HBA whose capabilities the port depends on. */
	controller = port->controller;

	/* An empty port keeps its memory installed but receives nothing. */
	if (port->link != AHCI_LINK_UP) {
		ahci_port_retire(port);
		return;
	}

	/* Only an ATA disk is used; ATAPI, port multipliers and SEMB come later. */
	signature = ahci_port_read(port, DRV_AHCI_PX_SIG);
	device_class = drv_ahci_signature_class(signature);
	if (device_class != DRV_AHCI_SIG_CLASS_ATA) {
		kern_logf("ahci%u: port %u: signature %08x not supported\n",
			  controller->index,
			  port->number,
			  signature);
		ahci_port_retire(port);
		return;
	}

	/* The bounce buffer every command's data moves through. */
	dma = drv_pci_device_dma(controller->pci);
	error = drv_dma_alloc_coherent(
		dma,
		AHCI_BOUNCE_SIZE,
		AHCI_BOUNCE_ALIGN,
		&port->bounce_dma);
	if (error != 0) {
		kern_logf("ahci%u: port %u: bounce buffer unavailable (%d)\n",
			  controller->index,
			  port->number,
			  error);
		ahci_port_retire(port);
		return;
	}

	/* Remembers that the bounce buffer exists. */
	port->bounce_allocated = 1;

	/* An HBA without 64-bit addressing cannot reach a high bounce buffer. */
	if ((controller->capability & DRV_AHCI_CAP_S64A) == 0U &&
	    port->bounce_dma.device_address + AHCI_BOUNCE_SIZE > UINT64_C(0x100000000)) {
		kern_logf("ahci%u: port %u: bounce buffer above 4 GiB\n",
			  controller->index,
			  port->number);
		ahci_port_retire(port);
		return;
	}

	/*
	 * Clears stale status, marks the port started for the handler, lets it
	 * interrupt and starts its engine.  The HBA interrupt itself is turned
	 * on later, once for all ports.
	 */
	ahci_port_clear_status(port);
	port->started = 1;
	kern_io_write_barrier();
	ahci_port_write(port, DRV_AHCI_PX_IE, DRV_AHCI_PX_IE_DEFAULT);
	ahci_port_engine_start(port);
}

/*
 * Takes a port out of service: interrupts off, engine and FIS reception
 * stopped.  A port that will not stop is quarantined.  The port's memory is
 * kept either way, small as it is, since its registers still point at it.
 */
static void
ahci_port_retire(
	struct ahci_port *port)
{
	int error;

	/* Stops the port's interrupts and engines, then takes it out of service. */
	ahci_port_write(port, DRV_AHCI_PX_IE, 0U);
	error = ahci_port_engine_stop(port);
	if (error == 0)
		error = ahci_port_receive_stop(port);
	port->dead = 1;

	/* A port still running may write its memory. */
	if (error != 0)
		ahci_port_quarantine(port);
}

/* Stops a port's command engine and waits until it has stopped. */
static int
ahci_port_engine_stop(
	struct ahci_port *port)
{
	uint32_t command;
	int error;

	/* Clears ST; the HBA answers by clearing CR, PxCI and PxSACT. */
	command = ahci_port_read(port, DRV_AHCI_PX_CMD);
	command &= ~DRV_AHCI_PX_CMD_ST;
	ahci_port_write(port, DRV_AHCI_PX_CMD, command);

	/* Waits up to 500 ms for the command list to go idle. */
	error = ahci_port_wait_clear(
		port,
		DRV_AHCI_PX_CMD,
		DRV_AHCI_PX_CMD_CR,
		AHCI_ENGINE_STOP_MS);
	if (error != 0)
		return error;

	/* Succeeded: the engine is stopped. */
	return 0;
}

/* Stops a port's FIS reception and waits until it has stopped. */
static int
ahci_port_receive_stop(
	struct ahci_port *port)
{
	uint32_t command;
	int error;

	/* Clears FRE, which AHCI allows only once the engine is stopped. */
	command = ahci_port_read(port, DRV_AHCI_PX_CMD);
	command &= ~DRV_AHCI_PX_CMD_FRE;
	ahci_port_write(port, DRV_AHCI_PX_CMD, command);

	/* Waits up to 500 ms for FIS reception to go idle. */
	error = ahci_port_wait_clear(
		port,
		DRV_AHCI_PX_CMD,
		DRV_AHCI_PX_CMD_FR,
		AHCI_ENGINE_STOP_MS);
	if (error != 0)
		return error;

	/* Succeeded: no more FISes are written to memory. */
	return 0;
}

/* Starts a port's command engine. */
static void
ahci_port_engine_start(
	struct ahci_port *port)
{
	uint32_t command;

	/* Sets ST; FRE is on and the device is idle. */
	command = ahci_port_read(port, DRV_AHCI_PX_CMD);
	command |= DRV_AHCI_PX_CMD_ST;
	ahci_port_write(port, DRV_AHCI_PX_CMD, command);
}

/*
 * Resets one stopped port's link and waits for its device.
 *
 * Used by recovery; the probe resets all ports at once instead.
 */
static int
ahci_port_comreset(
	struct ahci_port *port)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t control;
	uint32_t detection;
	int error;

	/* Holds COMRESET for at least a millisecond, then releases it. */
	control = ahci_port_read(port, DRV_AHCI_PX_SCTL);
	control &= DRV_AHCI_PX_SCTL_SPD_MASK;
	control |= DRV_AHCI_PX_SCTL_IPM_NONE | DRV_AHCI_PX_SCTL_DET_INIT;
	ahci_port_write(port, DRV_AHCI_PX_SCTL, control);
	ahci_delay_ms(AHCI_COMRESET_HOLD_MS);
	control &= ~DRV_AHCI_PX_SCTL_DET_MASK;
	ahci_port_write(port, DRV_AHCI_PX_SCTL, control);

	/* Waits up to a second for the link. */
	deadline = sched_ticks() + kern_ms_to_ticks(AHCI_LINK_UP_MS) + 1U;
	for (;;) {
		detection = ahci_port_read(port, DRV_AHCI_PX_SSTS) &
			    DRV_AHCI_PX_SSTS_DET_MASK;
		if (detection == DRV_AHCI_PX_SSTS_DET_PHY)
			break;

		/* A link that does not come back ends the recovery. */
		now = sched_ticks();
		if (now >= deadline)
			return ENODEV;

		/* Gives the CPU away before the next look. */
		sched_yield();
	}

	/* Clears the link errors and waits for the device to be ready. */
	ahci_port_write(port, DRV_AHCI_PX_SERR, UINT32_MAX);
	error = ahci_port_wait_clear(
		port,
		DRV_AHCI_PX_TFD,
		DRV_AHCI_PX_TFD_BSY | DRV_AHCI_PX_TFD_DRQ,
		AHCI_DEVICE_READY_MS);
	if (error != 0)
		return error;

	/* Succeeded: the link is up and the device is idle. */
	return 0;
}

/* Reads and decodes the disk's IDENTIFY DEVICE answer. */
static int
ahci_port_identify(
	struct ahci_port *port)
{
	uint16_t words[DRV_AHCI_IDENTIFY_SIZE / 2U];
	char model[AHCI_MODEL_TEXT_SIZE];
	const char *cache_text;
	uint32_t reasons;
	int retryable;
	int error;

	/* Issues IDENTIFY DEVICE once, with the short boot timeout. */
	mutex_lock(&port->command_mutex);

	error = ahci_port_execute(
		port,
		DRV_AHCI_ATA_IDENTIFY,
		0U,
		0U,
		0,
		DRV_AHCI_IDENTIFY_SIZE,
		AHCI_SHORT_TIMEOUT_MS,
		&retryable);
	if (error == 0)
		kern_memcpy(words, port->bounce_dma.address, sizeof(words));

	mutex_unlock(&port->command_mutex);

	/* Reports a disk that did not answer. */
	if (error != 0) {
		kern_logf("ahci%u: port %u: IDENTIFY failed (%d)\n",
			  port->controller->index,
			  port->number,
			  error);
		return error;
	}

	/* Decodes the addressing, the sector size and the cache. */
	drv_ahci_identify_text(words, 27U, 20U, model, sizeof(model));
	reasons = drv_ahci_identify_parse(words, &port->identity);

	/* Refuses a disk this driver cannot address. */
	if (reasons != 0U) {
		kern_logf("ahci%u: port %u: %s not supported (%08x)\n",
			  port->controller->index,
			  port->number,
			  model,
			  reasons);
		return EOPNOTSUPP;
	}

	/* Names the write cache in the log only when the disk has one. */
	cache_text = "";
	if (port->identity.write_cache)
		cache_text = " write-cache";

	/* Logs what the disk reported. */
	kern_logf("ahci%u: port %u: %s sectors=%08x:%08x sector-size=%u%s\n",
		  port->controller->index,
		  port->number,
		  model,
		  (uint32_t)(port->identity.sector_count >> 32),
		  (uint32_t)port->identity.sector_count,
		  port->identity.sector_size,
		  cache_text);

	/* Succeeded: the disk is identified. */
	return 0;
}

/* Publishes an identified disk to the disk layer. */
static int
ahci_port_publish(
	struct ahci_port *port)
{
	struct disk *disk;
	int error;

	/* Allocates the disk. */
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;

	/* Names it like every other ATA and SCSI disk. */
	error = disk_alloc_sd_name(disk);
	if (error != 0) {
		(void)disk_destroy(disk);
		return error;
	}

	/* Describes the disk; one request moves at most one bounce buffer. */
	disk->d_flags = 0;
	disk->d_block_size = port->identity.sector_size;
	disk->d_block_count = port->identity.sector_count;
	disk->d_max_transfer_blocks = AHCI_BOUNCE_SIZE / port->identity.sector_size;
	disk->d_ops = &ahci_disk_ops;
	disk->d_data = port;

	/* hw.diskstats lists it as an ATA disk ("IDE", ws231 U4). */
	disk_set_stats_kind(disk, DISK_STATS_KIND_IDE);

	/* Makes the disk visible; the port is fully running by now. */
	port->disk = disk;
	error = disk_create(disk);
	if (error != 0) {
		port->disk = NULL;
		(void)disk_destroy(disk);
		return error;
	}

	/* Logs the name the disk is reachable by. */
	kern_logf("ahci%u: port %u: /dev/%s\n",
		  port->controller->index,
		  port->number,
		  disk->d_name);

	/* Succeeded: the disk is published. */
	return 0;
}

/*
 * Runs one ATA command on a port and waits for it.
 *
 * The caller holds the command mutex and has put write data in the bounce
 * buffer; read data is left there.  On failure the port is recovered, and
 * *retryable says whether running the same command again may succeed.
 */
static int
ahci_port_execute(
	struct ahci_port *port,
	uint8_t command,
	uint64_t lba,
	uint32_t count,
	int write,
	size_t length,
	unsigned timeout_ms,
	int *retryable)
{
	struct drv_ahci_command_header *header;
	struct drv_ahci_fis_h2d fis;
	struct drv_ahci_prd *entries;
	const char *outcome;
	uint8_t *table;
	uint32_t slot_mask;
	uint32_t moved;
	uint32_t task_file;
	uint32_t serr;
	unsigned long irq;
	unsigned prd_count;
	unsigned state;
	int built;
	int recovered;

	/* Starts with a command that is not worth repeating. */
	*retryable = 0;

	/* A port out of service takes no more commands. */
	if (port->dead)
		return ENXIO;

	/* Builds the command FIS; its C bit marks a command register update. */
	built = drv_ahci_fis_h2d_build(&fis, command, lba, count);
	if (!built)
		return EINVAL;

	/* Copies the FIS into slot 0's table over a cleared FIS area. */
	table = port->table_dma.address;
	kern_memset(table, 0, DRV_AHCI_TABLE_PRDT_OFFSET);
	kern_memcpy(table, &fis, sizeof(fis));

	/* Describes the bounce buffer as the command's data. */
	prd_count = 0;
	if (length != 0U) {
		if (length > AHCI_BOUNCE_SIZE)
			return EINVAL;
		entries = (struct drv_ahci_prd *)(table + DRV_AHCI_TABLE_PRDT_OFFSET);
		built = drv_ahci_prdt_build(
			entries,
			DRV_AHCI_TABLE_PRD_MAX,
			port->bounce_dma.device_address,
			length,
			&prd_count);
		if (!built)
			return EINVAL;
	}

	/*
	 * Points slot 0's command header at the table.  The header's own C bit
	 * (clear busy on R_OK) stays clear.
	 */
	header = (struct drv_ahci_command_header *)port->command_list_dma.address +
		 AHCI_COMMAND_SLOT;
	built = drv_ahci_command_header_build(
		header,
		DRV_AHCI_FIS_H2D_DWORDS,
		write,
		prd_count,
		port->table_dma.device_address);
	if (!built)
		return EINVAL;

	/* A write may leave data in the disk's cache from here on, whatever its outcome. */
	port->cache_dirty = drv_ahci_cache_dirty_after(port->cache_dirty, command, 0);

	/*
	 * Issues the slot.  Status left from before is absorbed first, so it
	 * cannot be read as this command's.  The table and header must reach
	 * memory before the HBA sees the PxCI bit.
	 */
	slot_mask = UINT32_C(1) << AHCI_COMMAND_SLOT;
	irq = spin_lock_irqsave(&port->state_lock);

	ahci_port_service_locked(port);
	port->interrupt_status = 0;
	port->command_state = DRV_AHCI_COMMAND_ACTIVE;
	kern_io_write_barrier();
	ahci_port_write(port, DRV_AHCI_PX_CI, slot_mask);

	spin_unlock_irqrestore(&port->state_lock, irq);

	/* Waits for the command to reach an outcome or to be aborted. */
	state = ahci_port_wait_command(port, slot_mask, timeout_ms);

	/* A finished data command must have moved every byte. */
	if (state == DRV_AHCI_COMMAND_DONE) {
		kern_io_read_barrier();
		moved = header->transferred;

		/* A finished flush has emptied the cache of every earlier write. */
		if (length == 0U) {
			port->cache_dirty = drv_ahci_cache_dirty_after(port->cache_dirty, command, 1);
			return 0;
		}

		/* Succeeded: the data command moved every byte. */
		if ((size_t)moved == length)
			return 0;

		/* A short transfer is logged and treated as a link fault. */
		kern_logf("ahci%u: port %u: command %02x moved %u of %u bytes\n",
			  port->controller->index,
			  port->number,
			  command,
			  moved,
			  (unsigned)length);
		state = DRV_AHCI_COMMAND_LINK_ERROR;
	}

	/* Names how the command ended for the log. */
	if (state == DRV_AHCI_COMMAND_DEVICE_ERROR) {
		outcome = "device error";
	} else if (state == DRV_AHCI_COMMAND_LINK_ERROR) {
		outcome = "link error";
	} else {
		outcome = "timeout";
	}

	/* Reads what the port shows before recovery clears it. */
	task_file = ahci_port_read(port, DRV_AHCI_PX_TFD);
	serr = ahci_port_read(port, DRV_AHCI_PX_SERR);

	/* Logs that state together with the command that failed. */
	kern_logf("ahci%u: port %u: command %02x lba=%08x:%08x count=%u %s "
		  "is=%08x tfd=%08x serr=%08x\n",
		  port->controller->index,
		  port->number,
		  command,
		  (uint32_t)(lba >> 32),
		  (uint32_t)lba,
		  count,
		  outcome,
		  port->interrupt_status,
		  task_file,
		  serr);

	/* Brings the port back for the next command. */
	recovered = ahci_port_recover(port);
	if (recovered != 0) {
		kern_logf("ahci%u: port %u: recovery failed; port out of service\n",
			  port->controller->index,
			  port->number);
		return EIO;
	}

	/* A device error is the disk's answer; repeating it changes nothing. */
	if (state == DRV_AHCI_COMMAND_DEVICE_ERROR)
		return EIO;

	/* A link fault or a timeout may pass once the link is reset. */
	*retryable = 1;
	if (state == DRV_AHCI_COMMAND_LINK_ERROR)
		return EIO;

	/* The command did not finish in time. */
	return ETIMEDOUT;
}

/*
 * Waits until the port's command leaves ACTIVE, and aborts it when the
 * timeout passes.
 *
 * The decision to abort is taken under the state lock after one last look
 * at the hardware, so a command that finished just in time keeps its
 * outcome, and the state is ABORTED before recovery stops the engine.
 * Returns the command's final state.
 */
static unsigned
ahci_port_wait_command(
	struct ahci_port *port,
	uint32_t slot_mask,
	unsigned timeout_ms)
{
	struct thread *thread;
	uint64_t deadline;
	uint64_t now;
	uint64_t wake;
	uint64_t sequence;
	uint64_t after;
	unsigned long irq;
	uint32_t command_issue;
	uint32_t task_file;
	unsigned state;
	int polled;
	int slept;
	int woken;
	int idle;

	/* Fixes the moment the command is given up on. */
	deadline = sched_ticks() + kern_ms_to_ticks(timeout_ms) + 1U;

	/*
	 * Early VFS discovery runs in CPU0's idle thread, which cannot sleep on
	 * a wait queue (see nvme_io_wait_locked).  That context yields instead.
	 */
	idle = 1;
	thread = thread_current();
	if (thread != NULL && (thread->flags & THREAD_FLAG_IDLE) == 0U)
		idle = 0;

	/*
	 * Looks at the port, then sleeps or yields, until the command resolves.
	 * slept and woken describe the last sleep: whether there was one, and
	 * whether a wakeup (only the interrupt handler wakes this queue while
	 * the waiter sleeps) ended it rather than the poll interval.
	 */
	slept = 0;
	woken = 0;
	irq = spin_lock_irqsave(&port->state_lock);
	for (;;) {
		/* Folds in status the interrupt has not delivered yet. */
		ahci_port_service_locked(port);
		command_issue = ahci_port_read(port, DRV_AHCI_PX_CI);
		task_file = ahci_port_read(port, DRV_AHCI_PX_TFD);
		state = drv_ahci_command_advance(
			port->command_state,
			port->interrupt_status,
			task_file,
			command_issue,
			slot_mask);
		port->command_state = state;
		if (state != DRV_AHCI_COMMAND_ACTIVE)
			break;

		/* Aborts the command once its time has run out. */
		now = sched_ticks();
		if (now >= deadline) {
			state = drv_ahci_command_expire(state);
			port->command_state = state;
			break;
		}

		/* Yields in the idle thread; sleeps up to the poll interval elsewhere. */
		if (idle) {
			spin_unlock_irqrestore(&port->state_lock, irq);
			sched_yield();
			irq = spin_lock_irqsave(&port->state_lock);
		} else {
			wake = now + kern_ms_to_ticks(AHCI_POLL_MS) + 1U;
			if (wake > deadline)
				wake = deadline;
			sequence = waitq_sequence(&port->waitq);
			(void)waitq_sleep(&port->waitq, &port->state_lock, sequence, wake, 0);

			/* An advanced sequence means the interrupt handler woke the waiter. */
			slept = 1;
			woken = 0;
			after = waitq_sequence(&port->waitq);
			if (after != sequence)
				woken = 1;
		}
	}

	spin_unlock_irqrestore(&port->state_lock, irq);

	/*
	 * Counts a completion found only because the poll interval ended a
	 * sleep that no interrupt ended; a working interrupt leaves the count
	 * at zero.  The log shows the first and then every power of two.
	 */
	polled = drv_ahci_completion_by_polling(state, idle, slept, woken);
	if (polled) {
		port->poll_completions++;
		if ((port->poll_completions & (port->poll_completions - 1U)) == 0U) {
			kern_logf("ahci%u: port %u: completion found by polling (%u)\n",
				  port->controller->index,
				  port->number,
				  port->poll_completions);
		}
	}

	/* Reports the command's final state. */
	return state;
}

/*
 * Brings a port back after a failed or aborted command.
 *
 * Stopping the engine drops the command.  A device still busy, or an engine
 * that will not stop, gets a COMRESET.  A port that will not stop even then
 * is quarantined.  The caller holds the command mutex.
 */
static int
ahci_port_recover(
	struct ahci_port *port)
{
	unsigned long irq;
	uint32_t task_file;
	int stopped;
	int error;

	/* Stops the engine, which drops the failed command from PxCI. */
	stopped = ahci_port_engine_stop(port);

	/* Resets the link when the engine or the device is stuck. */
	task_file = ahci_port_read(port, DRV_AHCI_PX_TFD);
	if (stopped != 0 ||
	    (task_file & (DRV_AHCI_PX_TFD_BSY | DRV_AHCI_PX_TFD_DRQ)) != 0U) {
		/* The reset retires the disk's durability proof (USB does the same). */
		if (port->disk != NULL)
			disk_persistence_forget(port->disk);

		/* Resets the link and waits for the engine to go idle. */
		error = ahci_port_comreset(port);
		if (error == 0) {
			error = ahci_port_wait_clear(
				port,
				DRV_AHCI_PX_CMD,
				DRV_AHCI_PX_CMD_CR,
				AHCI_ENGINE_STOP_MS);
		}

		/* A port that stays stuck is taken out of service. */
		if (error != 0) {
			ahci_port_quarantine(port);
			return error;
		}
	}

	/* Clears the recorded errors and the dropped command's state. */
	irq = spin_lock_irqsave(&port->state_lock);

	ahci_port_clear_status(port);
	port->interrupt_status = 0;
	port->command_state = DRV_AHCI_COMMAND_IDLE;

	spin_unlock_irqrestore(&port->state_lock, irq);

	/* Starts the engine again. */
	ahci_port_engine_start(port);

	/* Succeeded: the port takes commands again. */
	return 0;
}

/*
 * Takes a port that will not stop out of service for good.
 *
 * Its interrupts go off and its disk stops admitting requests.  Its memory
 * is never freed, because the HBA may still write it.
 */
static void
ahci_port_quarantine(
	struct ahci_port *port)
{
	/* Silences the port and marks it unusable with unsafe DMA. */
	ahci_port_write(port, DRV_AHCI_PX_IE, 0U);
	port->dead = 1;
	port->dma_unsafe = 1;

	/* Closes the disk to new requests. */
	if (port->disk != NULL)
		disk_media_revoke(port->disk);

	/* Logs that the port is gone for good. */
	kern_logf("ahci%u: port %u: quarantined; its DMA memory is kept\n",
		  port->controller->index,
		  port->number);
}

/* Moves a read or write request through the bounce buffer, chunk by chunk. */
static int
ahci_port_transfer(
	struct ahci_port *port,
	struct bio *bio,
	size_t *transferred)
{
	uint8_t *bytes;
	uint64_t block;
	uint32_t remaining;
	uint32_t count;
	size_t length;
	unsigned attempt;
	uint8_t command;
	int write;
	int retryable;
	int error;

	/* Chooses the command for the direction. */
	write = 0;
	command = DRV_AHCI_ATA_READ_DMA_EXT;
	if (bio->b_op == BIO_WRITE) {
		write = 1;
		command = DRV_AHCI_ATA_WRITE_DMA_EXT;
	}

	/* Runs one command per bounce-buffer-sized chunk. */
	*transferred = 0;
	bytes = bio->b_data;
	block = bio->b_mapped_block;
	remaining = bio->b_block_count;
	while (remaining != 0U) {
		count = drv_ahci_chunk_blocks(
			remaining,
			port->identity.sector_size,
			AHCI_BOUNCE_SIZE);
		if (count == 0U)
			return EINVAL;
		length = (size_t)count * port->identity.sector_size;

		/*
		 * Runs the chunk, and again after a timeout or a link fault:
		 * reading, or writing the same data, twice is harmless.
		 */
		attempt = 0;
		for (;;) {
			if (write)
				kern_memcpy(port->bounce_dma.address, bytes, length);
			error = ahci_port_execute(
				port,
				command,
				block,
				count,
				write,
				length,
				AHCI_COMMAND_TIMEOUT_MS,
				&retryable);
			if (error == 0)
				break;
			if (!retryable)
				break;
			if (attempt >= AHCI_IO_RETRIES)
				break;
			attempt++;
		}

		/* Reports the failed chunk; the earlier chunks stay counted. */
		if (error != 0)
			return error;

		/* Hands read data back to the caller. */
		if (!write)
			kern_memcpy(bytes, port->bounce_dma.address, length);

		/* Advances past the chunk that moved. */
		*transferred += length;
		bytes += length;
		block += count;
		remaining -= count;
	}

	/* Succeeded: every block of the request moved. */
	return 0;
}

/*
 * Makes the disk's volatile write cache durable.
 *
 * FLUSH CACHE EXT is used where the disk has it, FLUSH CACHE otherwise.
 * The caller holds the command mutex.
 */
static int
ahci_port_flush(
	struct ahci_port *port,
	unsigned timeout_ms,
	unsigned retries)
{
	unsigned attempt;
	uint8_t command;
	int retryable;
	int error;

	/* Chooses the flush command the disk supports. */
	command = DRV_AHCI_ATA_FLUSH_CACHE;
	if (port->identity.flush_ext)
		command = DRV_AHCI_ATA_FLUSH_CACHE_EXT;

	/* Issues it, and again after a timeout or a link fault. */
	attempt = 0;
	for (;;) {
		error = ahci_port_execute(
			port,
			command,
			0U,
			0U,
			0,
			0U,
			timeout_ms,
			&retryable);
		if (error == 0)
			break;
		if (!retryable)
			break;
		if (attempt >= retries)
			break;
		attempt++;
	}

	/* Reports a cache the disk could not flush. */
	if (error != 0)
		return error;

	/* Succeeded: the disk's cache is on the medium. */
	return 0;
}

/* Takes every port's command mutex, in port order. */
static void
ahci_ports_lock(
	struct ahci_controller *controller)
{
	unsigned number;

	/* Waits for each port's running command, if any, to finish. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		if (controller->ports[number] == NULL)
			continue;

		/* Takes this port's mutex, waiting for its running command. */
		mutex_lock(&controller->ports[number]->command_mutex);
	}
}

/* Gives every port's command mutex back. */
static void
ahci_ports_unlock(
	struct ahci_controller *controller)
{
	unsigned number;

	/* Lets each port's waiting requests in. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		if (controller->ports[number] == NULL)
			continue;

		mutex_unlock(&controller->ports[number]->command_mutex);
	}
}

/*
 * Flushes every disk, stops every running port and turns bus mastering off.
 * The caller holds every port's command mutex.
 */
static int
ahci_suspend_stop(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint32_t control;
	unsigned number;
	int error;

	/* Flushes each disk's write cache; a disk that cannot be flushed stops the suspend. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->disk == NULL)
			continue;
		if (port->dead)
			continue;

		/* Flushes this disk's write cache with the short bound. */
		error = ahci_port_flush(port, AHCI_SHORT_TIMEOUT_MS, 0U);
		if (error != 0)
			return error;
	}

	/* Turns the HBA's interrupt off. */
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control &= ~DRV_AHCI_GHC_IE;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);

	/* Stops each running port: the engine first, then FIS reception. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (!port->started)
			continue;
		if (port->dead)
			continue;

		/* Silences the port, then stops its engine and FIS reception. */
		ahci_port_write(port, DRV_AHCI_PX_IE, 0U);
		error = ahci_port_engine_stop(port);
		if (error == 0)
			error = ahci_port_receive_stop(port);
		if (error != 0)
			return error;
	}

	/* Takes the HBA off the bus; the PCI core saves the command register next. */
	error = drv_pci_device_set_bus_master(controller->pci, false);
	if (error != 0)
		return error;

	/* Succeeded: the HBA neither runs commands nor touches memory. */
	return 0;
}

/*
 * Brings the ports that ran before suspend back: AHCI mode, the saved
 * registers, each port's memory, bus mastering, a link reset and the
 * engines.  A port whose disk does not come back is taken out of service.
 * The caller holds every port's command mutex.
 */
static void
ahci_resume_start(
	struct ahci_controller *controller)
{
	struct ahci_port *port;
	uint32_t control;
	uint32_t pending;
	uint32_t signature;
	unsigned device_class;
	unsigned number;
	int error;

	/* Puts the HBA back in AHCI mode with its interrupt off. */
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control |= DRV_AHCI_GHC_AE;
	control &= ~DRV_AHCI_GHC_IE;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);

	/*
	 * Writes back the write-once capability and port registers, which some
	 * HBAs lose with power (Linux restores them the same way).
	 */
	ahci_write(controller, DRV_AHCI_REG_CAP, controller->capability);
	ahci_write(controller, DRV_AHCI_REG_CAP2, controller->capability2);
	ahci_write(controller, DRV_AHCI_REG_PI, controller->ports_implemented);

	/*
	 * Points every port that had memory back at it, and clears its status.
	 * A port still running (a suspend undone half-way) is stopped first,
	 * since its registers may only be rewritten while it is stopped.
	 */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->dma_unsafe)
			continue;

		/* Silences the port, then stops its engine and FIS reception. */
		ahci_port_write(port, DRV_AHCI_PX_IE, 0U);
		error = ahci_port_engine_stop(port);
		if (error == 0)
			error = ahci_port_receive_stop(port);
		if (error != 0) {
			ahci_port_quarantine(port);
			continue;
		}

		/* Rewrites the registers that locate the port's command list and FIS area. */
		if (port->memory_installed) {
			ahci_port_write(
				port,
				DRV_AHCI_PX_CLB,
				(uint32_t)(port->command_list_dma.device_address & 0xffffffffU));
			ahci_port_write(
				port,
				DRV_AHCI_PX_CLBU,
				(uint32_t)(port->command_list_dma.device_address >> 32));
			ahci_port_write(
				port,
				DRV_AHCI_PX_FB,
				(uint32_t)(port->received_fis_dma.device_address & 0xffffffffU));
			ahci_port_write(
				port,
				DRV_AHCI_PX_FBU,
				(uint32_t)(port->received_fis_dma.device_address >> 32));
		}

		/* Drops the status the stopped port still shows. */
		ahci_port_clear_status(port);
	}

	/* Lets the HBA reach memory again. */
	error = drv_pci_device_set_bus_master(controller->pci, true);
	if (error != 0) {
		kern_logf("ahci%u: bus mastering refused on resume (%d)\n",
			  controller->index,
			  error);
		return;
	}

	/* Prepares and resets the links of the ports that were running. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->dead)
			continue;

		/* Powers the port up and starts FIS reception. */
		ahci_port_prepare_link(port);
	}

	/* Resets every link, then waits for links and devices together. */
	ahci_links_reset(controller);
	ahci_links_wait(controller);
	ahci_devices_wait(controller);

	/* Starts each port whose ATA disk answers again; retires the others. */
	for (number = 0; number < DRV_AHCI_PORT_MAX; number++) {
		port = controller->ports[number];
		if (port == NULL)
			continue;
		if (port->dead)
			continue;

		/* A link reset retires the disk's durability proof. */
		if (port->disk != NULL)
			disk_persistence_forget(port->disk);

		/* A port whose disk did not come back is taken out of service. */
		signature = ahci_port_read(port, DRV_AHCI_PX_SIG);
		device_class = drv_ahci_signature_class(signature);
		if (port->link != AHCI_LINK_UP ||
		    device_class != DRV_AHCI_SIG_CLASS_ATA) {
			kern_logf("ahci%u: port %u: disk did not come back after resume\n",
				  controller->index,
				  number);
			ahci_port_retire(port);
			if (port->disk != NULL)
				disk_media_revoke(port->disk);
			continue;
		}

		/* Clears stale status and the command state, then starts the port. */
		ahci_port_clear_status(port);
		port->interrupt_status = 0;
		port->command_state = DRV_AHCI_COMMAND_IDLE;
		ahci_port_write(port, DRV_AHCI_PX_IE, DRV_AHCI_PX_IE_DEFAULT);
		ahci_port_engine_start(port);
	}

	/* Turns the HBA's interrupt on again. */
	pending = ahci_read(controller, DRV_AHCI_REG_IS);
	ahci_write(controller, DRV_AHCI_REG_IS, pending);
	control = ahci_read(controller, DRV_AHCI_REG_GHC);
	control |= DRV_AHCI_GHC_AE | DRV_AHCI_GHC_IE;
	ahci_write(controller, DRV_AHCI_REG_GHC, control);
}
