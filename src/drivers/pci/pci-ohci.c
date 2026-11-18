/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI OHCI (USB 1.1) host controller driver.
 *
 * Each transfer gets an endpoint descriptor of its own, with its
 * transfer descriptors (pci-ohci-td.c) and an empty tail, in one
 * coherent buffer, and its data in a page-aligned bounce buffer.  The
 * endpoint descriptor joins the end of the control, bulk or periodic
 * list while the controller runs.  Every interrupt endpoint is served
 * every frame: the 32 entries of the interrupt table all lead to one
 * skipped descriptor that heads the periodic list.  Isochronous
 * transfers are not offered.
 *
 * A worker thread retires transfers: one whose endpoint is halted or
 * whose head has reached the tail is done.  To take an endpoint
 * descriptor out of a list, its list is paused, the worker waits two
 * frames so that the controller holds no pointer into it, unlinks it and
 * resumes the list.  The controller's interrupt (the done-queue
 * writeback and root hub changes) wakes the worker, which also looks
 * every 10 ms, so a board without a routed interrupt still works.  A
 * second thread tells the USB core about root port changes, every 100
 * ms or when woken: the core enumerates a new device on that thread,
 * waiting for transfers the first one retires.
 *
 * Some controllers keep the root hub change interrupt (RHSC) raised for
 * as long as a port's change bit is set rather than raising it once per
 * change, so clearing it in the handler would not quiet the line until
 * the root worker clears the change; the handler therefore turns RHSC off,
 * and the root worker turns it back on once no change is left.  An
 * over-current change of the whole hub, which nothing else clears, is
 * cleared there too.
 *
 * The controller's registers and descriptors are little-endian; the
 * driver converts on a big-endian processor.  The data toggle of a bulk
 * or interrupt endpoint is kept in the endpoint's first HCD word between
 * transfers.
 *
 * Values referenced: OpenHCI Open Host Controller Interface
 * Specification for USB, Release 1.0a (1999): the operational registers
 * (chapter 7: HcControl's CBSR, PLE, IE, CLE, BLE, HCFS and IR bits,
 * HcCommandStatus's HCR, CLF, BLF and OCR, the interrupt bits SO, WDH,
 * SF, RD, UE, FNO, RHSC, OC and MIE, HcFmInterval's FI, FSMPS and FIT,
 * HcPeriodicStart at 90% of FI, HcLSThreshold 0x628, the root hub's
 * descriptor A (NDP, NPS, POTPGT), status (LPSC) and port status bits,
 * which are the USB hub class's wPortStatus and wPortChange bits), the
 * HCCA (section 4.4: 256-byte aligned, the 32-entry interrupt table),
 * section 5.2.7.1 (removing an endpoint descriptor from a list: pause
 * the list and wait for the next frame) and section 5.1.1.3 (taking the
 * controller from system management through OwnershipChangeRequest).
 * The Linux kernel's drivers/usb/host/ohci-hcd.c computes FSMPS as
 * 6 * (FI - 210) / 7, and its ohci_irq() turns RHSC off for the same
 * reason (the vendors' level-triggered RHSC) until the hub is polled.
 */

#include <drivers/pci/pci-ohci.h>
#include "drivers/pci/pci-ohci-td.h"
#include <drivers/pci/pci.h>
#include <drivers/usb/usb.h>
#include <uapi/errno.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/device-io.h"
#include <kern/kcrt.h>

/* The operational registers. */
#define OHCI_CONTROL			0x04U
#define OHCI_COMMAND_STATUS		0x08U
#define OHCI_INTERRUPT_STATUS		0x0cU
#define OHCI_INTERRUPT_ENABLE		0x10U
#define OHCI_INTERRUPT_DISABLE		0x14U
#define OHCI_HCCA			0x18U
#define OHCI_CONTROL_HEAD		0x20U
#define OHCI_CONTROL_CURRENT		0x24U
#define OHCI_BULK_HEAD			0x28U
#define OHCI_BULK_CURRENT		0x2cU
#define OHCI_FM_INTERVAL		0x34U
#define OHCI_FM_NUMBER			0x3cU
#define OHCI_PERIODIC_START		0x40U
#define OHCI_LS_THRESHOLD		0x44U
#define OHCI_RH_DESCRIPTOR_A		0x48U
#define OHCI_RH_STATUS			0x50U
#define OHCI_RH_PORT_STATUS		0x54U

/* HcControl. */
#define OHCI_CONTROL_CBSR_4_1		0x00000003U
#define OHCI_CONTROL_PLE		0x00000004U
#define OHCI_CONTROL_CLE		0x00000010U
#define OHCI_CONTROL_BLE		0x00000020U
#define OHCI_CONTROL_STATE_MASK		0x000000c0U
#define OHCI_CONTROL_STATE_RESET	0x00000000U
#define OHCI_CONTROL_STATE_OPERATIONAL	0x00000080U
#define OHCI_CONTROL_IR			0x00000100U

/* HcCommandStatus. */
#define OHCI_COMMAND_HCR		0x00000001U
#define OHCI_COMMAND_CLF		0x00000002U
#define OHCI_COMMAND_BLF		0x00000004U
#define OHCI_COMMAND_OCR		0x00000008U

/* The interrupt bits. */
#define OHCI_INTERRUPT_WDH		0x00000002U
#define OHCI_INTERRUPT_UE		0x00000010U
#define OHCI_INTERRUPT_RHSC		0x00000040U
#define OHCI_INTERRUPT_MIE		0x80000000U
#define OHCI_INTERRUPT_ALL		0xc000007fU

/* HcFmInterval: the frame interval, the largest data packet and the toggle that marks a change. */
#define OHCI_FM_INTERVAL_DEFAULT	11999U
#define OHCI_FM_INTERVAL_MASK		0x00003fffU
#define OHCI_FM_INTERVAL_FIT		0x80000000U
#define OHCI_LS_THRESHOLD_DEFAULT	0x628U

/* The root hub's descriptor A and status. */
#define OHCI_RH_A_PORTS_MASK		0x000000ffU
#define OHCI_RH_A_NO_POWER_SWITCHING	0x00000200U
#define OHCI_RH_A_POWER_GOOD_SHIFT	24U
#define OHCI_RH_STATUS_SET_POWER	0x00010000U
#define OHCI_RH_STATUS_OVER_CURRENT_CHANGE	0x00020000U

/* A root port's status: the bits a write sets or clears, and the change bits. */
#define OHCI_PORT_CONNECTED		0x00000001U
#define OHCI_PORT_ENABLED		0x00000002U
#define OHCI_PORT_SUSPENDED		0x00000004U
#define OHCI_PORT_CLEAR_SUSPEND		0x00000008U
#define OHCI_PORT_RESET			0x00000010U
#define OHCI_PORT_POWER			0x00000100U
#define OHCI_PORT_CLEAR_POWER		0x00000200U
#define OHCI_PORT_CHANGES		0x001f0000U
#define OHCI_PORT_RESET_CHANGE		0x00100000U

/* The hub class requests and features the root hub serves. */
#define HUB_REQUEST_GET_STATUS		0U
#define HUB_REQUEST_CLEAR_FEATURE	1U
#define HUB_REQUEST_SET_FEATURE		3U
#define HUB_FEATURE_ENABLE		1U
#define HUB_FEATURE_SUSPEND		2U
#define HUB_FEATURE_RESET		4U
#define HUB_FEATURE_POWER		8U
#define HUB_FEATURE_C_CONNECTION	16U
#define HUB_FEATURE_C_RESET		20U

/* The most root ports the driver serves. */
#define OHCI_PORTS_MAX			15U

/* The HCCA's size and alignment, then the periodic list's skipped head after it. */
#define OHCI_HCCA_SIZE			256U
#define OHCI_HCCA_INTERRUPT_ENTRIES	32U
#define OHCI_PERIODIC_HEAD_OFFSET	256U
#define OHCI_SHARED_SIZE		512U

/* Where a transfer's descriptors lie in its buffer: the endpoint descriptor first. */
#define OHCI_REQUEST_TD_OFFSET		16U

/* The worker's look between interrupts, the root worker's, and the frames a list is paused for. */
#define OHCI_POLL_MS			10U
#define OHCI_ROOT_POLL_MS		100U
#define OHCI_PAUSE_FRAMES		2U

/* How long a frame wait, a reset of the controller or of a port may take. */
#define OHCI_FRAME_WAIT_MS		50U
#define OHCI_RESET_POLLS		100000U
#define OHCI_PORT_RESET_FRAMES		100U
#define OHCI_PORT_RECOVERY_FRAMES	10U
#define OHCI_OWNERSHIP_POLLS		1000000U

/* The root ports the state report shows. */
#define OHCI_REPORT_PORTS		4U

/* The endpoint's HCD word that keeps its data toggle. */
#define OHCI_ENDPOINT_TOGGLE_SLOT	0U

/* The lists a transfer's endpoint descriptor joins. */
enum ohci_list {
	OHCI_LIST_CONTROL,
	OHCI_LIST_BULK,
	OHCI_LIST_PERIODIC,
	OHCI_LIST_COUNT
};

/* Where a transfer is in its life. */
enum ohci_request_state {
	OHCI_REQUEST_ACTIVE,
	OHCI_REQUEST_RETIRING,
	OHCI_REQUEST_CANCELLING
};

/*
 * One transfer on the bus: its endpoint descriptor and transfer
 * descriptors, its bounce buffer, the plan they were cut by and the tick
 * it was put on the bus at (for the state report).  It is
 * on the controller's active list from its enqueue until it is
 * completed or cancelled, and is freed then.
 */
struct ohci_request {
	struct ohci_request *next;
	struct drv_usb_urb *urb;
	struct drv_usb_endpoint *endpoint;
	enum ohci_list list;
	enum ohci_request_state state;
	struct drv_dma_buffer descriptors;
	struct drv_dma_buffer data;
	struct ohci_transfer_plan plan;
	unsigned td_count;
	int keeps_toggle;
	enum ohci_transfer_result result;
	size_t actual;
	unsigned condition;
	uint64_t submitted;
};

/*
 * One controller: its registers, the shared HCCA, the transfers on the
 * bus and the worker that retires them.  It lives from attach for the
 * life of the system.
 */
struct ohci_controller {
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	struct drv_pci_device *pci;
	struct drv_pci_mapping registers;
	struct drv_dma_buffer shared;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct ohci_controller *next;

	/*
	 * Guards the request list, the lists' pause state, the worker's wake
	 * generation and the flags below; the controller's lists are changed
	 * only under it.
	 */
	struct spinlock lock;
	struct ohci_request *requests;
	unsigned paused[OHCI_LIST_COUNT];
	uint64_t wake_generation;
	struct thread *worker;
	struct thread *root_worker;
	int root_ready;
	int root_force_scan;
	int failed;

	/*
	 * Set while the handler has turned the root hub change interrupt off;
	 * the root worker turns it on again.  Changed under lock.
	 */
	int rhsc_off;

	/*
	 * What the state report shows: the interrupts that were this
	 * controller's, and the transfers retired.  Counted without the lock;
	 * a report may be one behind.
	 */
	uint32_t interrupts;
	uint32_t retired;

	/*
	 * Set while one thread pauses lists to take descriptors out of them;
	 * another waits for it to clear.  Changed under lock.
	 */
	int unlinking;

	uint32_t frame_interval;
	unsigned power_good_ms;
};

/*
 * Every controller this driver holds, for the root probe.  Changed only
 * by attach, which the PCI core runs one at a time.
 */
static struct ohci_controller *ohci_controllers;

static int ohci_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int ohci_detach(struct drv_pci_device *device, unsigned flags);
static int ohci_start(struct drv_usb_hcd *hcd);
static void ohci_stop(struct drv_usb_hcd *hcd);
static int ohci_urb_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb);
static int ohci_urb_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb);
static int ohci_endpoint_reset(struct drv_usb_hcd *hcd, struct drv_usb_endpoint *endpoint);
static uint32_t ohci_frame_number(struct drv_usb_hcd *hcd);
static int ohci_root_hub_status(struct drv_usb_hcd *hcd, void *buffer, size_t size, size_t *actual);
static int ohci_root_hub_control(struct drv_usb_hcd *hcd, const struct drv_usb_control_request *request, void *buffer, size_t size, size_t *actual);
static int ohci_root_port_reset(struct drv_usb_hcd *hcd, unsigned port);

/* The operations the USB core drives the controller with. */
static const struct drv_usb_hcd_ops ohci_ops = {
	.start = ohci_start,
	.stop = ohci_stop,
	.urb_enqueue = ohci_urb_enqueue,
	.urb_dequeue = ohci_urb_dequeue,
	.endpoint_reset = ohci_endpoint_reset,
	.frame_number = ohci_frame_number,
	.root_hub_status = ohci_root_hub_status,
	.root_hub_control = ohci_root_hub_control,
	.root_port_reset = ohci_root_port_reset
};

/* The functions the driver binds: any USB host controller of the OHCI programming interface. */
static const struct drv_pci_id ohci_ids[] = {
	{
		DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
		DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
		0x0c0310U,
		0xffffffU,
		0
	}
};

/* The driver the PCI core matches against its functions. */
static struct drv_pci_driver ohci_driver = {
	.name = "ohci",
	.ids = ohci_ids,
	.id_count = 1,
	.attach = ohci_attach,
	.detach = ohci_detach
};

static struct ohci_controller *hcd_controller(struct drv_usb_hcd *hcd);
static uint32_t to_le32(uint32_t value);
static uint32_t read_register(struct ohci_controller *controller, uint32_t offset);
static void write_register(struct ohci_controller *controller, uint32_t offset, uint32_t value);
static void write_word(volatile uint32_t *word, uint32_t value);
static uint32_t read_word(const volatile uint32_t *word);
static volatile uint32_t *request_ed(struct ohci_request *request);
static volatile uint32_t *periodic_head(struct ohci_controller *controller);
static int frames_pass(struct ohci_controller *controller, unsigned frames);
static int ohci_irq(void *argument);
static void wake_worker(struct ohci_controller *controller);
static int worker_start(struct ohci_controller *controller);
static void worker(void *argument);
static void root_worker(void *argument);
static void retire_finished(struct ohci_controller *controller);
static void unlinking_begin(struct ohci_controller *controller);
static void unlinking_end(struct ohci_controller *controller);
static void lists_pause(struct ohci_controller *controller, const unsigned *lists);
static void lists_resume(struct ohci_controller *controller, const unsigned *lists);
static void list_link(struct ohci_controller *controller, struct ohci_request *request);
static void list_unlink(struct ohci_controller *controller, struct ohci_request *request);
static void request_remove(struct ohci_controller *controller, struct ohci_request *request);
static int request_build(struct ohci_controller *controller, struct drv_usb_urb *urb, struct ohci_request **result);
static void request_read_outcome(struct ohci_request *request);
static void request_finish(struct ohci_controller *controller, struct ohci_request *request);
static void request_free(struct ohci_controller *controller, struct ohci_request *request);
static void root_ports_check(struct ohci_controller *controller);
static void port_write(struct ohci_controller *controller, unsigned port, uint32_t value);
static void root_change_interrupt_restore(struct ohci_controller *controller);
static void report_controller(struct ohci_controller *controller);

/*
 * Registers this driver with the PCI bus.
 */
int
drv_pci_ohci_driver_register(
	void)
{
	int error;

	/* Hands the driver to the PCI core, which binds the functions it finds. */
	error = drv_pci_driver_register(&ohci_driver);
	if (error != 0)
		return error;

	/* Succeeded: the driver is registered. */
	return 0;
}

/*
 * Has every controller this driver holds look at its root ports, once
 * the system can enumerate what is attached there.
 */
void
drv_pci_ohci_probe_roots(
	void)
{
	struct ohci_controller *controller;
	unsigned long irq;

	/* Arms each controller's worker for its first scan. */
	for (controller = ohci_controllers; controller != NULL; controller = controller->next) {
		irq = spin_lock_irqsave(&controller->lock);

		controller->root_ready = 1;
		controller->root_force_scan = 1;

		spin_unlock_irqrestore(&controller->lock, irq);

		/* Wakes the worker to scan the ports. */
		wake_worker(controller);
	}
}

/*
 * Writes each controller's state to the kernel log: its interrupts and
 * retired transfers, its registers, the transfers on the bus and how long
 * the oldest has waited, and its first root ports.  For a boot that
 * stops, to tell a transfer the controller never finishes from one never
 * given to it.
 */
void
drv_pci_ohci_report(
	void)
{
	struct ohci_controller *controller;

	/* Each controller this driver holds. */
	for (controller = ohci_controllers; controller != NULL; controller = controller->next)
		report_controller(controller);
}

/* Brings up a controller the PCI bus has just matched. */
static int
ohci_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct ohci_controller *controller;
	struct drv_pci_address location;
	uint32_t descriptor;
	unsigned count;
	unsigned ports;
	int error;

	(void)id;

	/* The controller's record. */
	controller = kern_malloc(sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;
	kern_memset(controller, 0, sizeof(*controller));
	spin_init(&controller->lock, LOCK_RANK_DEVICE, "OHCI requests");
	controller->pci = device;

	/* The registers, in the first BAR. */
	error = drv_pci_device_claim_bar(device, 0);
	if (error != 0)
		goto fail;
	error = drv_pci_device_map_bar(device, 0, DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE, &controller->registers);
	if (error != 0)
		goto fail;

	/* The root hub's ports. */
	descriptor = read_register(controller, OHCI_RH_DESCRIPTOR_A);
	ports = descriptor & OHCI_RH_A_PORTS_MASK;
	if (ports == 0 || ports > OHCI_PORTS_MAX) {
		error = ENODEV;
		goto fail;
	}

	/* The HCCA and the periodic list's head, shared by every transfer. */
	error = drv_dma_alloc_coherent(drv_pci_device_dma(device), OHCI_SHARED_SIZE, OHCI_HCCA_SIZE, &controller->shared);
	if (error != 0)
		goto fail;
	if (controller->shared.device_address > UINT32_MAX - OHCI_SHARED_SIZE) {
		error = EOVERFLOW;
		goto fail;
	}

	/* The host controller the USB core drives. */
	controller->hcd.name = "OHCI";
	controller->hcd.ops = &ohci_ops;
	controller->hcd.dma = drv_pci_device_dma(device);
	controller->hcd.root_port_count = ports;
	controller->hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	controller->hcd.private_data[0] = (uintptr_t)controller;

	/* Memory decoding and bus mastering. */
	error = drv_pci_device_enable_memory(device);
	if (error != 0)
		goto fail;
	error = drv_pci_device_set_bus_master(device, true);
	if (error != 0)
		goto fail;

	/* The worker, before the core can submit anything. */
	error = worker_start(controller);
	if (error != 0)
		goto fail;

	/* Registers the bus; the core starts the controller. */
	error = drv_usb_hcd_register(&controller->hcd, &controller->bus);
	if (error != 0)
		goto fail;

	/* The interrupt; without one the worker's look every 10 ms serves. */
	count = 0;
	error = drv_pci_device_allocate_irqs(device, DRV_PCI_IRQ_ALLOW_INTX, 1, 1, &controller->irq, &count);
	if (error == 0) {
		error = drv_pci_device_establish_irq(device, &controller->irq, ohci_irq, controller, "ohci", &controller->irq_cookie);
		if (error != 0)
			kern_logf("ohci: interrupt unavailable (%d); polling\n", error);
	} else {
		kern_logf("ohci: no interrupt routed (%d); polling\n", error);
	}

	/* Publishes the controller to the root probe. */
	drv_pci_device_set_driver_data(device, controller);
	controller->next = ohci_controllers;
	ohci_controllers = controller;
	drv_pci_device_address(device, &location);
	kern_logf("ohci: PCI controller %u:%02x.%u at %llx, ports=%u, interrupt %s\n",
		  (unsigned)location.bus, (unsigned)location.device, (unsigned)location.function,
		  (unsigned long long)controller->registers.physical_address, ports,
		  controller->irq_cookie != NULL ? "routed" : "none (polled)");

	/* Succeeded: the controller is in service. */
	return 0;

fail:
	/*
	 * A failure before the bus was registered leaves nothing the
	 * controller can reach; the record is kept, so that a worker already
	 * started never sees it freed.
	 */
	kern_logf("ohci: attach failed (%d)\n", error);
	controller->failed = 1;

	/* Reports why the controller could not be brought up. */
	return error;
}

/* Refuses to take a controller out of service: the driver keeps it for the life of the system. */
static int
ohci_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	(void)device;
	(void)flags;

	/* A controller in service stays. */
	return EBUSY;
}

/*
 * Resets the controller and brings it to the operational state with
 * empty lists, its interrupts on and its ports powered.
 */
static int
ohci_start(
	struct drv_usb_hcd *hcd)
{
	struct ohci_controller *controller;
	volatile uint32_t *hcca;
	volatile uint32_t *head;
	uint32_t control;
	uint32_t interval;
	uint32_t largest;
	uint32_t descriptor;
	unsigned index;
	unsigned polls;

	/* Takes the controller from system management firmware when it owns it. */
	controller = hcd_controller(hcd);
	control = read_register(controller, OHCI_CONTROL);
	if ((control & OHCI_CONTROL_IR) != 0) {
		write_register(controller, OHCI_COMMAND_STATUS, OHCI_COMMAND_OCR);
		for (polls = 0; polls < OHCI_OWNERSHIP_POLLS; polls++) {
			control = read_register(controller, OHCI_CONTROL);
			if ((control & OHCI_CONTROL_IR) == 0)
				break;
		}
		if (polls == OHCI_OWNERSHIP_POLLS)
			return ETIMEDOUT;
	}

	/* The frame interval the firmware set, or the standard one. */
	interval = read_register(controller, OHCI_FM_INTERVAL) & OHCI_FM_INTERVAL_MASK;
	if (interval == 0)
		interval = OHCI_FM_INTERVAL_DEFAULT;
	controller->frame_interval = interval;

	/* No interrupt while the controller is reset. */
	write_register(controller, OHCI_INTERRUPT_DISABLE, OHCI_INTERRUPT_ALL);

	/* The software reset, which leaves the controller suspended. */
	write_register(controller, OHCI_COMMAND_STATUS, OHCI_COMMAND_HCR);
	for (polls = 0; polls < OHCI_RESET_POLLS; polls++) {
		control = read_register(controller, OHCI_COMMAND_STATUS);
		if ((control & OHCI_COMMAND_HCR) == 0)
			break;
	}
	if (polls == OHCI_RESET_POLLS)
		return ETIMEDOUT;

	/* Every interrupt table entry leads to the periodic list's skipped head. */
	kern_memset(controller->shared.address, 0, OHCI_SHARED_SIZE);
	hcca = controller->shared.address;
	head = periodic_head(controller);
	write_word(&head[0], OHCI_ED_SKIP);
	for (index = 0; index < OHCI_HCCA_INTERRUPT_ENTRIES; index++)
		write_word(&hcca[index], (uint32_t)controller->shared.device_address + OHCI_PERIODIC_HEAD_OFFSET);
	kern_io_write_barrier();

	/* The HCCA and empty control and bulk lists. */
	write_register(controller, OHCI_HCCA, (uint32_t)controller->shared.device_address);
	write_register(controller, OHCI_CONTROL_HEAD, 0);
	write_register(controller, OHCI_CONTROL_CURRENT, 0);
	write_register(controller, OHCI_BULK_HEAD, 0);
	write_register(controller, OHCI_BULK_CURRENT, 0);
	write_register(controller, OHCI_INTERRUPT_STATUS, OHCI_INTERRUPT_ALL);

	/* The frame: its interval, the largest data packet it fits, and when periodic work starts. */
	largest = (6U * (interval - 210U)) / 7U;
	write_register(controller, OHCI_FM_INTERVAL, OHCI_FM_INTERVAL_FIT | (largest << 16) | interval);
	write_register(controller, OHCI_PERIODIC_START, (interval * 9U) / 10U);
	write_register(controller, OHCI_LS_THRESHOLD, OHCI_LS_THRESHOLD_DEFAULT);

	/* Operational, with the periodic list on; the control and bulk lists start when they are filled. */
	write_register(controller, OHCI_CONTROL, OHCI_CONTROL_CBSR_4_1 | OHCI_CONTROL_PLE | OHCI_CONTROL_STATE_OPERATIONAL);
	write_register(controller, OHCI_INTERRUPT_ENABLE, OHCI_INTERRUPT_MIE | OHCI_INTERRUPT_WDH | OHCI_INTERRUPT_RHSC | OHCI_INTERRUPT_UE);

	/*
	 * Powers the ports, all at once and each one, unless they are always
	 * powered; the worker gives them the power-good time before the
	 * first scan.
	 */
	descriptor = read_register(controller, OHCI_RH_DESCRIPTOR_A);
	controller->power_good_ms = 2U * (descriptor >> OHCI_RH_A_POWER_GOOD_SHIFT);
	if ((descriptor & OHCI_RH_A_NO_POWER_SWITCHING) == 0) {
		write_register(controller, OHCI_RH_STATUS, OHCI_RH_STATUS_SET_POWER);
		for (index = 1; index <= controller->hcd.root_port_count; index++)
			port_write(controller, index, OHCI_PORT_POWER);
	}

	/* Succeeded: the controller runs. */
	return 0;
}

/* Stops the controller: no interrupt, and the reset state. */
static void
ohci_stop(
	struct drv_usb_hcd *hcd)
{
	struct ohci_controller *controller;

	/* Interrupts off, then the reset state, which ends every list. */
	controller = hcd_controller(hcd);
	write_register(controller, OHCI_INTERRUPT_DISABLE, OHCI_INTERRUPT_ALL);
	write_register(controller, OHCI_CONTROL, OHCI_CONTROL_STATE_RESET);
}

/* Puts one transfer on the bus. */
static int
ohci_urb_enqueue(
	struct drv_usb_hcd *hcd,
	struct drv_usb_urb *urb)
{
	struct ohci_controller *controller;
	struct ohci_request *request;
	unsigned long irq;
	int error;

	/* Builds the descriptors and the bounce buffer. */
	controller = hcd_controller(hcd);
	if (controller->failed)
		return ENODEV;
	error = request_build(controller, urb, &request);
	if (error != 0)
		return error;

	/* Ties the request to the transfer. */
	error = drv_usb_urb_set_hcd_data(urb, request);
	if (error != 0) {
		request_free(controller, request);
		return error;
	}

	/* Puts the endpoint descriptor at the end of its list. */
	irq = spin_lock_irqsave(&controller->lock);

	request->submitted = sched_ticks();
	request->next = controller->requests;
	controller->requests = request;
	list_link(controller, request);

	spin_unlock_irqrestore(&controller->lock, irq);

	/* Succeeded: the controller owns the transfer. */
	return 0;
}

/*
 * Cancels a transfer still on the bus: its endpoint is skipped, taken
 * out of its list and freed.  A transfer the worker has already taken
 * is left to complete (EBUSY).
 */
static int
ohci_urb_dequeue(
	struct drv_usb_hcd *hcd,
	struct drv_usb_urb *urb)
{
	struct ohci_controller *controller;
	struct ohci_request *request;
	unsigned lists[OHCI_LIST_COUNT];
	volatile uint32_t *ed;
	unsigned long irq;

	/* Takes the transfer from the bus unless the worker has it. */
	controller = hcd_controller(hcd);
	irq = spin_lock_irqsave(&controller->lock);

	request = drv_usb_urb_hcd_data(urb);
	if (request == NULL || request->urb != urb || request->state != OHCI_REQUEST_ACTIVE) {
		spin_unlock_irqrestore(&controller->lock, irq);
		return EBUSY;
	}
	request->state = OHCI_REQUEST_CANCELLING;
	ed = request_ed(request);
	write_word(&ed[0], read_word(&ed[0]) | OHCI_ED_SKIP);

	spin_unlock_irqrestore(&controller->lock, irq);

	/* Takes its endpoint descriptor out of its paused list. */
	kern_memset(lists, 0, sizeof(lists));
	lists[request->list] = 1;
	unlinking_begin(controller);
	lists_pause(controller, lists);
	(void)frames_pass(controller, OHCI_PAUSE_FRAMES);
	irq = spin_lock_irqsave(&controller->lock);

	list_unlink(controller, request);
	request_remove(controller, request);

	spin_unlock_irqrestore(&controller->lock, irq);

	lists_resume(controller, lists);
	unlinking_end(controller);

	/* Keeps the toggle the endpoint reached and frees the request. */
	request_read_outcome(request);
	if (request->keeps_toggle) {
		ed = request_ed(request);
		(void)drv_usb_endpoint_set_hcd_data(request->endpoint, OHCI_ENDPOINT_TOGGLE_SLOT, (read_word(&ed[2]) & OHCI_ED_HEAD_TOGGLE) >> 1);
	}
	(void)drv_usb_urb_set_hcd_data(urb, NULL);
	request_free(controller, request);

	/* Succeeded: the core publishes the cancellation. */
	return 0;
}

/* Clears an endpoint's data toggle after the device's endpoint was reset. */
static int
ohci_endpoint_reset(
	struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	int error;

	(void)hcd;

	/* The next transfer starts at DATA0. */
	if (endpoint == NULL)
		return EINVAL;
	error = drv_usb_endpoint_set_hcd_data(endpoint, OHCI_ENDPOINT_TOGGLE_SLOT, 0);
	if (error != 0)
		return error;

	/* Succeeded: the toggle is DATA0. */
	return 0;
}

/* Reports the frame number the bus stands at. */
static uint32_t
ohci_frame_number(
	struct drv_usb_hcd *hcd)
{
	uint32_t frame;

	/* The low 11 bits are the frame number on the bus. */
	frame = read_register(hcd_controller(hcd), OHCI_FM_NUMBER) & 0x7ffU;

	/* Succeeded: the frame. */
	return frame;
}

/* Reports which root ports have changed, one bit a port from bit 1. */
static int
ohci_root_hub_status(
	struct drv_usb_hcd *hcd,
	void *buffer,
	size_t size,
	size_t *actual)
{
	struct ohci_controller *controller;
	uint8_t *bits;
	uint32_t status;
	unsigned port;
	size_t length;

	/* Room for a bit a port and the hub's own. */
	controller = hcd_controller(hcd);
	length = (controller->hcd.root_port_count + 8U) / 8U;
	if (buffer == NULL || size < length)
		return EINVAL;
	bits = buffer;
	kern_memset(bits, 0, length);

	/* A port with a change bit set. */
	for (port = 1; port <= controller->hcd.root_port_count; port++) {
		status = read_register(controller, OHCI_RH_PORT_STATUS + (port - 1U) * 4U);
		if ((status & OHCI_PORT_CHANGES) != 0)
			bits[port / 8U] |= (uint8_t)(1U << (port % 8U));
	}

	/* Succeeded: the bitmap. */
	if (actual != NULL)
		*actual = length;
	return 0;
}

/*
 * Serves one hub class request to a root port.  The port's status
 * register has the hub's wPortStatus in its low half and wPortChange in
 * its high half; each feature is one bit written to it.
 */
static int
ohci_root_hub_control(
	struct drv_usb_hcd *hcd,
	const struct drv_usb_control_request *request,
	void *buffer,
	size_t size,
	size_t *actual)
{
	struct ohci_controller *controller;
	uint32_t status;
	uint32_t bit;

	/* Requires a port of the root hub. */
	controller = hcd_controller(hcd);
	if (request == NULL || request->index < 1U || request->index > controller->hcd.root_port_count)
		return EINVAL;
	if (actual != NULL)
		*actual = 0;

	/* GET_STATUS: the register, already in the hub's form. */
	if (request->request == HUB_REQUEST_GET_STATUS) {
		if (buffer == NULL || size < sizeof(status))
			return EINVAL;
		status = read_register(controller, OHCI_RH_PORT_STATUS + (request->index - 1U) * 4U);
		kern_memcpy(buffer, &status, sizeof(status));
		if (actual != NULL)
			*actual = sizeof(status);
		return 0;
	}

	/* SET_FEATURE: enable, suspend, reset and power are the status bits themselves. */
	if (request->request == HUB_REQUEST_SET_FEATURE) {
		switch (request->value) {
		case HUB_FEATURE_ENABLE:
			bit = OHCI_PORT_ENABLED;
			break;
		case HUB_FEATURE_SUSPEND:
			bit = OHCI_PORT_SUSPENDED;
			break;
		case HUB_FEATURE_RESET:
			bit = OHCI_PORT_RESET;
			break;
		case HUB_FEATURE_POWER:
			bit = OHCI_PORT_POWER;
			break;
		default:
			return ENOTSUP;
		}
		port_write(controller, request->index, bit);
		return 0;
	}

	/* Anything but CLEAR_FEATURE is not served. */
	if (request->request != HUB_REQUEST_CLEAR_FEATURE)
		return ENOTSUP;

	/* CLEAR_FEATURE: each clear has a bit of its own; a change is cleared by writing it. */
	switch (request->value) {
	case HUB_FEATURE_ENABLE:
		bit = OHCI_PORT_CONNECTED;
		break;
	case HUB_FEATURE_SUSPEND:
		bit = OHCI_PORT_CLEAR_SUSPEND;
		break;
	case HUB_FEATURE_RESET:
		/* The reset ends by itself. */
		return 0;
	case HUB_FEATURE_POWER:
		bit = OHCI_PORT_CLEAR_POWER;
		break;
	default:
		if (request->value < HUB_FEATURE_C_CONNECTION || request->value > HUB_FEATURE_C_RESET)
			return ENOTSUP;
		bit = 1U << request->value;
		break;
	}
	port_write(controller, request->index, bit);

	/* Succeeded: the feature is cleared. */
	return 0;
}

/*
 * Resets a root port and returns once it is enabled: the reset is
 * signalled for the controller's 10 ms, then the port recovers.
 */
static int
ohci_root_port_reset(
	struct drv_usb_hcd *hcd,
	unsigned port)
{
	struct ohci_controller *controller;
	uint32_t offset;
	uint32_t status;
	unsigned frames;
	int passed;

	/* Requires a port with something on it. */
	controller = hcd_controller(hcd);
	if (port < 1U || port > controller->hcd.root_port_count)
		return EINVAL;
	offset = OHCI_RH_PORT_STATUS + (port - 1U) * 4U;
	status = read_register(controller, offset);
	if ((status & OHCI_PORT_CONNECTED) == 0)
		return ENODEV;

	/* Starts the reset and waits, a frame at a time, for it to end. */
	port_write(controller, port, OHCI_PORT_RESET);
	for (frames = 0; frames < OHCI_PORT_RESET_FRAMES; frames++) {
		status = read_register(controller, offset);
		if ((status & OHCI_PORT_RESET_CHANGE) != 0)
			break;
		passed = frames_pass(controller, 1);
		if (!passed)
			return ETIMEDOUT;
	}
	if (frames == OHCI_PORT_RESET_FRAMES)
		return ETIMEDOUT;

	/* Clears the reset's change and gives the device its recovery time. */
	port_write(controller, port, OHCI_PORT_RESET_CHANGE);
	(void)frames_pass(controller, OHCI_PORT_RECOVERY_FRAMES);

	/* The port must be enabled now. */
	status = read_register(controller, offset);
	if ((status & OHCI_PORT_ENABLED) == 0)
		return EIO;

	/* Succeeded: the port is enabled. */
	return 0;
}

/* Takes the controller behind a host controller handle. */
static struct ohci_controller *
hcd_controller(
	struct drv_usb_hcd *hcd)
{
	/* The handle's first private word. */
	return (struct ohci_controller *)hcd->private_data[0];
}

/* Converts a word between the processor's order and little-endian, which are the same or reversed. */
static uint32_t
to_le32(
	uint32_t value)
{
	/* A big-endian processor reverses the bytes. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	return __builtin_bswap32(value);
#else
	return value;
#endif
}

/* Reads an operational register. */
static uint32_t
read_register(
	struct ohci_controller *controller,
	uint32_t offset)
{
	uint32_t value;

	/* The register, little-endian. */
	value = kern_mmio_read32((volatile uint8_t *)controller->registers.address + offset);

	/* Succeeded: the register's value. */
	return to_le32(value);
}

/* Writes an operational register. */
static void
write_register(
	struct ohci_controller *controller,
	uint32_t offset,
	uint32_t value)
{
	/* The register, little-endian. */
	kern_mmio_write32((volatile uint8_t *)controller->registers.address + offset, to_le32(value));
}

/* Writes a descriptor word the controller reads. */
static void
write_word(
	volatile uint32_t *word,
	uint32_t value)
{
	/* The word, little-endian. */
	*word = to_le32(value);
}

/* Reads a descriptor word the controller wrote. */
static uint32_t
read_word(
	const volatile uint32_t *word)
{
	uint32_t value;

	/* The word, little-endian. */
	value = *word;

	/* Succeeded: the word in the processor's order. */
	return to_le32(value);
}

/* Reports a request's endpoint descriptor: control, tail, head, next. */
static volatile uint32_t *
request_ed(
	struct ohci_request *request)
{
	/* The first descriptor of the request's buffer. */
	return (volatile uint32_t *)request->descriptors.address;
}

/* Reports the periodic list's skipped head, after the HCCA. */
static volatile uint32_t *
periodic_head(
	struct ohci_controller *controller)
{
	/* The shared buffer's second half. */
	return (volatile uint32_t *)((uint8_t *)controller->shared.address + OHCI_PERIODIC_HEAD_OFFSET);
}

/*
 * Waits for frames to pass on the bus.  Reports 1 when they did, 0 when
 * the frame counter did not move for OHCI_FRAME_WAIT_MS.
 */
static int
frames_pass(
	struct ohci_controller *controller,
	unsigned frames)
{
	uint64_t deadline;
	uint32_t first;
	uint32_t now;

	/* Counts frames from the one the bus is in. */
	first = read_register(controller, OHCI_FM_NUMBER) & 0xffffU;
	deadline = sched_ticks() + kern_ms_to_ticks(OHCI_FRAME_WAIT_MS * frames);
	for (;;) {
		now = read_register(controller, OHCI_FM_NUMBER) & 0xffffU;
		if (((now - first) & 0xffffU) >= frames)
			return 1;
		if (sched_ticks() > deadline)
			return 0;
		sched_yield();
	}
}

/* Serves the controller's interrupt: clears what it reports and wakes the worker. */
static int
ohci_irq(
	void *argument)
{
	struct ohci_controller *controller;
	unsigned long irq;
	uint32_t status;

	/* What the controller raised, among the interrupts it may raise. */
	controller = argument;
	status = read_register(controller, OHCI_INTERRUPT_STATUS);
	status &= OHCI_INTERRUPT_ALL & ~OHCI_INTERRUPT_MIE;
	if (status == 0)
		return 0;
	write_register(controller, OHCI_INTERRUPT_STATUS, status);
	controller->interrupts++;

	/* A root hub change turns its interrupt off until the root worker has served it. */
	if ((status & OHCI_INTERRUPT_RHSC) != 0) {
		irq = spin_lock_irqsave(&controller->lock);

		write_register(controller, OHCI_INTERRUPT_DISABLE, OHCI_INTERRUPT_RHSC);
		controller->rhsc_off = 1;

		spin_unlock_irqrestore(&controller->lock, irq);
	}

	/* An unrecoverable error stops the controller for good. */
	if ((status & OHCI_INTERRUPT_UE) != 0 && !controller->failed) {
		controller->failed = 1;
		kern_logf("ohci: unrecoverable error; controller stopped\n");
	}

	/* The worker retires what is done and looks at the ports. */
	wake_worker(controller);

	/* Succeeded: the interrupt was the controller's. */
	return 1;
}

/* Wakes the worker; a wake before it sleeps is kept by the generation. */
static void
wake_worker(
	struct ohci_controller *controller)
{
	unsigned long irq;

	/* Counts the wake, then wakes the thread. */
	irq = spin_lock_irqsave(&controller->lock);

	controller->wake_generation++;
	if (controller->worker != NULL)
		sched_notify_task(controller->worker->task);
	if (controller->root_worker != NULL)
		sched_notify_task(controller->root_worker->task);

	spin_unlock_irqrestore(&controller->lock, irq);
}

/* Starts the worker and the root worker. */
static int
worker_start(
	struct ohci_controller *controller)
{
	struct thread *thread;
	struct thread *root_thread;
	unsigned long irq;
	int error;

	/* Makes the threads, publishes them, then runs them. */
	error = kthread_create(worker, controller, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0)
		return error;
	error = kthread_create(root_worker, controller, SCHED_PRIORITY_DEFAULT, &root_thread);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->lock);

	controller->worker = thread;
	controller->root_worker = root_thread;

	spin_unlock_irqrestore(&controller->lock, irq);

	thread_start(thread);
	thread_start(root_thread);

	/* Succeeded: the workers run. */
	return 0;
}

/*
 * Retires finished transfers, then sleeps until woken or OHCI_POLL_MS
 * have passed.
 */
static void
worker(
	void *argument)
{
	struct ohci_controller *controller;
	uint64_t generation;
	unsigned long irq;
	uint32_t status;

	/* Serves the controller for the life of the system. */
	controller = argument;
	for (;;) {
		/* The wake this pass answers. */
		irq = spin_lock_irqsave(&controller->lock);

		generation = controller->wake_generation;

		spin_unlock_irqrestore(&controller->lock, irq);

		/* Clears what an undelivered interrupt left, then retires what is done. */
		if (!controller->failed) {
			status = read_register(controller, OHCI_INTERRUPT_STATUS);
			status &= OHCI_INTERRUPT_ALL & ~OHCI_INTERRUPT_MIE;
			if (status != 0)
				write_register(controller, OHCI_INTERRUPT_STATUS, status);
			retire_finished(controller);
		}

		/* Sleeps unless a wake came during the pass. */
		irq = spin_lock_irqsave(&controller->lock);

		if (controller->wake_generation == generation)
			sched_sleep_locked(sched_ticks() + kern_ms_to_ticks(OHCI_POLL_MS), &controller->lock);

		spin_unlock_irqrestore(&controller->lock, irq);
	}
}

/*
 * Reports root port changes to the USB core, which enumerates what is
 * attached on this thread; then sleeps until woken or OHCI_ROOT_POLL_MS
 * have passed.
 */
static void
root_worker(
	void *argument)
{
	struct ohci_controller *controller;
	uint64_t generation;
	unsigned long irq;

	/* Serves the root ports for the life of the system. */
	controller = argument;
	for (;;) {
		/* The wake this pass answers. */
		irq = spin_lock_irqsave(&controller->lock);

		generation = controller->wake_generation;

		spin_unlock_irqrestore(&controller->lock, irq);

		/* Looks at the ports, then lets them interrupt again once no change is left. */
		if (!controller->failed) {
			root_ports_check(controller);
			root_change_interrupt_restore(controller);
		}

		/* Sleeps unless a wake came during the pass. */
		irq = spin_lock_irqsave(&controller->lock);

		if (controller->wake_generation == generation)
			sched_sleep_locked(sched_ticks() + kern_ms_to_ticks(OHCI_ROOT_POLL_MS), &controller->lock);

		spin_unlock_irqrestore(&controller->lock, irq);
	}
}

/*
 * Takes every finished transfer out of its list and completes it: the
 * lists the finished ones are in are paused once for all of them.
 */
static void
retire_finished(
	struct ohci_controller *controller)
{
	struct ohci_request *request;
	struct ohci_request *finished;
	struct ohci_request *next;
	unsigned lists[OHCI_LIST_COUNT];
	volatile uint32_t *ed;
	unsigned long irq;
	uint32_t head;
	uint32_t tail;
	int any;

	/* Marks each active transfer whose endpoint is halted or at its tail. */
	kern_memset(lists, 0, sizeof(lists));
	any = 0;
	unlinking_begin(controller);
	irq = spin_lock_irqsave(&controller->lock);

	for (request = controller->requests; request != NULL; request = request->next) {
		if (request->state != OHCI_REQUEST_ACTIVE)
			continue;
		ed = request_ed(request);
		head = read_word(&ed[2]);
		tail = read_word(&ed[1]);
		if ((head & OHCI_ED_HEAD_HALTED) == 0 &&
		    (head & OHCI_ED_POINTER_MASK) != (tail & OHCI_ED_POINTER_MASK))
			continue;
		request->state = OHCI_REQUEST_RETIRING;
		lists[request->list] = 1;
		any = 1;
	}

	spin_unlock_irqrestore(&controller->lock, irq);

	/* Nothing finished: nothing to take out. */
	if (!any) {
		unlinking_end(controller);
		return;
	}

	/* Takes the finished ones out of their paused lists and off the active list. */
	lists_pause(controller, lists);
	(void)frames_pass(controller, OHCI_PAUSE_FRAMES);
	finished = NULL;
	irq = spin_lock_irqsave(&controller->lock);

	for (request = controller->requests; request != NULL; request = next) {
		next = request->next;
		if (request->state != OHCI_REQUEST_RETIRING)
			continue;
		list_unlink(controller, request);
		request_remove(controller, request);
		request->next = finished;
		finished = request;
	}

	spin_unlock_irqrestore(&controller->lock, irq);

	lists_resume(controller, lists);
	unlinking_end(controller);

	/* Completes each, outside every lock: a completion may submit or cancel. */
	for (request = finished; request != NULL; request = next) {
		next = request->next;
		controller->retired++;
		request_finish(controller, request);
	}
}

/* Waits until no other thread is taking descriptors out of a list, then is the one. */
static void
unlinking_begin(
	struct ohci_controller *controller)
{
	unsigned long irq;
	int taken;

	/* Takes the flag, yielding while another holds it. */
	for (;;) {
		irq = spin_lock_irqsave(&controller->lock);

		taken = 0;
		if (!controller->unlinking) {
			controller->unlinking = 1;
			taken = 1;
		}

		spin_unlock_irqrestore(&controller->lock, irq);

		/* The flag is this thread's. */
		if (taken)
			return;
		sched_yield();
	}
}

/* Lets the next thread take descriptors out of a list. */
static void
unlinking_end(
	struct ohci_controller *controller)
{
	unsigned long irq;

	/* Clears the flag. */
	irq = spin_lock_irqsave(&controller->lock);

	controller->unlinking = 0;

	spin_unlock_irqrestore(&controller->lock, irq);
}

/*
 * Stops the controller from walking the lists marked in lists; a list
 * linked meanwhile stays stopped until lists_resume().
 */
static void
lists_pause(
	struct ohci_controller *controller,
	const unsigned *lists)
{
	unsigned long irq;
	uint32_t control;

	/* Clears each marked list's enable bit. */
	irq = spin_lock_irqsave(&controller->lock);

	control = read_register(controller, OHCI_CONTROL);
	if (lists[OHCI_LIST_CONTROL]) {
		controller->paused[OHCI_LIST_CONTROL] = 1;
		control &= ~OHCI_CONTROL_CLE;
	}
	if (lists[OHCI_LIST_BULK]) {
		controller->paused[OHCI_LIST_BULK] = 1;
		control &= ~OHCI_CONTROL_BLE;
	}
	if (lists[OHCI_LIST_PERIODIC]) {
		controller->paused[OHCI_LIST_PERIODIC] = 1;
		control &= ~OHCI_CONTROL_PLE;
	}
	write_register(controller, OHCI_CONTROL, control);

	spin_unlock_irqrestore(&controller->lock, irq);
}

/*
 * Lets the controller walk the paused lists again: from their heads,
 * each control or bulk list only when it holds something, and marked as
 * filled.
 */
static void
lists_resume(
	struct ohci_controller *controller,
	const unsigned *lists)
{
	unsigned long irq;
	uint32_t control;
	uint32_t command;
	uint32_t head;

	/* Sets each resumed list's enable bit back. */
	irq = spin_lock_irqsave(&controller->lock);

	control = read_register(controller, OHCI_CONTROL);
	command = 0;
	if (lists[OHCI_LIST_CONTROL]) {
		controller->paused[OHCI_LIST_CONTROL] = 0;
		write_register(controller, OHCI_CONTROL_CURRENT, 0);
		head = read_register(controller, OHCI_CONTROL_HEAD);
		if (head != 0) {
			control |= OHCI_CONTROL_CLE;
			command |= OHCI_COMMAND_CLF;
		}
	}
	if (lists[OHCI_LIST_BULK]) {
		controller->paused[OHCI_LIST_BULK] = 0;
		write_register(controller, OHCI_BULK_CURRENT, 0);
		head = read_register(controller, OHCI_BULK_HEAD);
		if (head != 0) {
			control |= OHCI_CONTROL_BLE;
			command |= OHCI_COMMAND_BLF;
		}
	}
	if (lists[OHCI_LIST_PERIODIC]) {
		controller->paused[OHCI_LIST_PERIODIC] = 0;
		control |= OHCI_CONTROL_PLE;
	}
	write_register(controller, OHCI_CONTROL, control);
	if (command != 0)
		write_register(controller, OHCI_COMMAND_STATUS, command);

	spin_unlock_irqrestore(&controller->lock, irq);
}

/*
 * Puts a request's endpoint descriptor at the end of its list and has
 * the controller walk the list.  Called with the lock held.
 */
static void
list_link(
	struct ohci_controller *controller,
	struct ohci_request *request)
{
	struct ohci_request *walk;
	struct ohci_request *tail;
	volatile uint32_t *head;
	uint32_t address;
	uint32_t control;
	uint32_t enable;
	uint32_t filled;
	uint32_t head_register;
	uint32_t current_register;

	/* The list's last descriptor before this one: the newest older request of the same list. */
	tail = NULL;
	for (walk = request->next; walk != NULL; walk = walk->next) {
		if (walk->list == request->list) {
			tail = walk;
			break;
		}
	}
	address = (uint32_t)request->descriptors.device_address;
	kern_io_write_barrier();

	/* The periodic list hangs from its skipped head, after the newest periodic request. */
	if (request->list == OHCI_LIST_PERIODIC) {
		head = periodic_head(controller);
		if (tail != NULL)
			head = request_ed(tail);
		write_word(&head[3], address);
		return;
	}

	/* A control or bulk request after the list's last, or as its head. */
	head_register = OHCI_CONTROL_HEAD;
	current_register = OHCI_CONTROL_CURRENT;
	enable = OHCI_CONTROL_CLE;
	filled = OHCI_COMMAND_CLF;
	if (request->list == OHCI_LIST_BULK) {
		head_register = OHCI_BULK_HEAD;
		current_register = OHCI_BULK_CURRENT;
		enable = OHCI_CONTROL_BLE;
		filled = OHCI_COMMAND_BLF;
	}
	if (tail != NULL) {
		write_word(&request_ed(tail)[3], address);
	} else {
		/* An empty list is stopped: it takes its head and starts there. */
		write_register(controller, head_register, address);
		if (!controller->paused[request->list]) {
			write_register(controller, current_register, 0);
			control = read_register(controller, OHCI_CONTROL);
			write_register(controller, OHCI_CONTROL, control | enable);
		}
	}

	/* Tells the controller the list has work. */
	write_register(controller, OHCI_COMMAND_STATUS, filled);
}

/*
 * Takes a request's endpoint descriptor out of its paused list: the one
 * before it, or the list's head, skips to the one after.  Called with
 * the lock held and the request still on the active list.
 */
static void
list_unlink(
	struct ohci_controller *controller,
	struct ohci_request *request)
{
	struct ohci_request *walk;
	struct ohci_request *before;
	volatile uint32_t *head;
	uint32_t after;
	uint32_t head_register;

	/*
	 * The descriptor before it in the controller's list: the newest
	 * older request of the same list.  The active list holds the newest
	 * first, so that is the first of its list after it.
	 */
	before = NULL;
	for (walk = request->next; walk != NULL; walk = walk->next) {
		if (walk->list == request->list) {
			before = walk;
			break;
		}
	}
	after = read_word(&request_ed(request)[3]);

	/* A request in the middle: the one before it skips it. */
	if (before != NULL) {
		write_word(&request_ed(before)[3], after);
		return;
	}

	/* The periodic list's first request: its skipped head skips it. */
	if (request->list == OHCI_LIST_PERIODIC) {
		head = periodic_head(controller);
		write_word(&head[3], after);
		return;
	}

	/* A control or bulk list's head: the list starts after it. */
	head_register = OHCI_CONTROL_HEAD;
	if (request->list == OHCI_LIST_BULK)
		head_register = OHCI_BULK_HEAD;
	write_register(controller, head_register, after);
}

/* Takes a request off the active list.  Called with the lock held. */
static void
request_remove(
	struct ohci_controller *controller,
	struct ohci_request *request)
{
	struct ohci_request **link;

	/* Finds the link that leads to it. */
	for (link = &controller->requests; *link != NULL; link = &(*link)->next) {
		if (*link == request) {
			*link = request->next;
			break;
		}
	}
	request->next = NULL;
}

/*
 * Builds a transfer's request: its endpoint descriptor, transfer
 * descriptors and empty tail, its setup packet and its bounce buffer.
 */
static int
request_build(
	struct ohci_controller *controller,
	struct drv_usb_urb *urb,
	struct ohci_request **result)
{
	const struct drv_usb_control_request *setup;
	struct ohci_td_words tds[OHCI_TD_MAX];
	struct ohci_request *request;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_device *device;
	enum drv_usb_transfer_type type;
	enum drv_usb_speed speed;
	volatile uint32_t *words;
	uint8_t *setup_bytes;
	size_t descriptor_size;
	uint32_t toggle;
	unsigned packet;
	unsigned count;
	unsigned index;
	int error;

	/* The transfer's endpoint and device, and what the controller can serve. */
	endpoint = drv_usb_urb_endpoint(urb);
	device = drv_usb_urb_device(urb);
	setup = drv_usb_urb_control_request(urb);
	if (endpoint == NULL || device == NULL)
		return EINVAL;
	type = drv_usb_endpoint_type(endpoint);
	if (type == DRV_USB_TRANSFER_ISOCHRONOUS)
		return ENOTSUP;
	speed = drv_usb_device_speed(device);
	if (speed != DRV_USB_SPEED_LOW && speed != DRV_USB_SPEED_FULL)
		return ENOTSUP;
	if ((setup != NULL) != (type == DRV_USB_TRANSFER_CONTROL))
		return EINVAL;
	packet = drv_usb_endpoint_max_packet_size(endpoint) & 0x7ffU;
	if (packet == 0)
		return EINVAL;

	/* The request. */
	request = kern_malloc(sizeof(*request));
	if (request == NULL)
		return ENOMEM;
	kern_memset(request, 0, sizeof(*request));
	request->urb = urb;
	request->endpoint = endpoint;
	request->state = OHCI_REQUEST_ACTIVE;

	/* The plan: control or not, the direction, the length and a closing empty packet. */
	request->plan.control = setup != NULL;
	request->plan.length = drv_usb_urb_length(urb);
	if (request->plan.length != 0 && drv_usb_urb_buffer(urb) == NULL) {
		kern_free(request);
		return EINVAL;
	}
	if (setup != NULL) {
		request->plan.input = (setup->request_type & DRV_USB_DIR_IN) != 0;
		request->list = OHCI_LIST_CONTROL;
	} else {
		request->plan.input = drv_usb_endpoint_is_input(endpoint);
		request->list = OHCI_LIST_BULK;
		if (type == DRV_USB_TRANSFER_INTERRUPT)
			request->list = OHCI_LIST_PERIODIC;
		request->keeps_toggle = 1;
		if (!request->plan.input &&
		    request->plan.length != 0 &&
		    request->plan.length % packet == 0 &&
		    (drv_usb_urb_flags(urb) & DRV_USB_URB_ZERO_PACKET) != 0)
			request->plan.zero_packet = 1;
	}

	/* Refuses a transfer longer than the descriptors can carry. */
	request->td_count = ohci_td_count(&request->plan);
	if (request->td_count > OHCI_TD_MAX) {
		kern_free(request);
		return E2BIG;
	}

	/* The descriptors' buffer: the endpoint, the transfers, the tail and the setup packet. */
	descriptor_size = OHCI_REQUEST_TD_OFFSET + (request->td_count + 1U) * OHCI_DESCRIPTOR_SIZE + OHCI_DESCRIPTOR_SIZE;
	error = drv_dma_alloc_coherent(controller->hcd.dma, descriptor_size, OHCI_DESCRIPTOR_SIZE, &request->descriptors);
	if (error != 0) {
		kern_free(request);
		return error;
	}
	kern_memset(request->descriptors.address, 0, descriptor_size);

	/* The bounce buffer, page-aligned so that each transfer descriptor stays in one page. */
	if (request->plan.length != 0) {
		error = drv_dma_alloc_coherent(controller->hcd.dma, request->plan.length, OHCI_TD_DATA_MAX, &request->data);
		if (error != 0) {
			request_free(controller, request);
			return error;
		}
		if (!request->plan.input)
			kern_memcpy(request->data.address, drv_usb_urb_buffer(urb), request->plan.length);
		request->plan.data_address = (uint32_t)request->data.device_address;
	}

	/* Refuses buffers the controller's 32-bit pointers cannot reach. */
	if (request->descriptors.device_address > UINT32_MAX - descriptor_size ||
	    request->data.device_address > UINT32_MAX - request->plan.length) {
		request_free(controller, request);
		return EOVERFLOW;
	}

	/* The setup packet, little-endian on the wire, after the tail. */
	request->plan.td_address = (uint32_t)request->descriptors.device_address + OHCI_REQUEST_TD_OFFSET;
	request->plan.setup_address = request->plan.td_address + (request->td_count + 1U) * OHCI_DESCRIPTOR_SIZE;
	if (setup != NULL) {
		setup_bytes = (uint8_t *)request->descriptors.address + (request->plan.setup_address - (uint32_t)request->descriptors.device_address);
		setup_bytes[0] = setup->request_type;
		setup_bytes[1] = setup->request;
		setup_bytes[2] = (uint8_t)setup->value;
		setup_bytes[3] = (uint8_t)(setup->value >> 8);
		setup_bytes[4] = (uint8_t)setup->index;
		setup_bytes[5] = (uint8_t)(setup->index >> 8);
		setup_bytes[6] = (uint8_t)setup->length;
		setup_bytes[7] = (uint8_t)(setup->length >> 8);
	}

	/* The transfer descriptors, in the controller's byte order. */
	error = ohci_td_build(&request->plan, tds, OHCI_TD_MAX, &count);
	if (error != 0 || count != request->td_count) {
		request_free(controller, request);
		return E2BIG;
	}
	words = (volatile uint32_t *)((uint8_t *)request->descriptors.address + OHCI_REQUEST_TD_OFFSET);
	for (index = 0; index < count; index++) {
		write_word(&words[index * 4U + 0U], tds[index].control);
		write_word(&words[index * 4U + 1U], tds[index].buffer);
		write_word(&words[index * 4U + 2U], tds[index].next);
		write_word(&words[index * 4U + 3U], tds[index].end);
	}

	/* The endpoint descriptor: its function and endpoint, the tail, and the head with the toggle. */
	toggle = 0;
	if (request->keeps_toggle && drv_usb_endpoint_hcd_data(endpoint, OHCI_ENDPOINT_TOGGLE_SLOT) != 0)
		toggle = OHCI_ED_HEAD_TOGGLE;
	words = request_ed(request);
	write_word(&words[0], ohci_ed_control(drv_usb_device_address(device),
					      drv_usb_endpoint_address(endpoint) & 0xfU,
					      setup != NULL,
					      request->plan.input,
					      speed == DRV_USB_SPEED_LOW,
					      packet));
	write_word(&words[1], request->plan.td_address + count * OHCI_DESCRIPTOR_SIZE);
	write_word(&words[2], request->plan.td_address | toggle);
	write_word(&words[3], 0);

	/* Succeeded: the request is ready to link. */
	*result = request;
	return 0;
}

/* Reads how a request's transfer ended from its descriptors and endpoint. */
static void
request_read_outcome(
	struct ohci_request *request)
{
	struct ohci_td_words tds[OHCI_TD_MAX];
	volatile uint32_t *words;
	volatile uint32_t *ed;
	unsigned index;

	/* The descriptors as the controller left them. */
	kern_io_read_barrier();
	words = (volatile uint32_t *)((uint8_t *)request->descriptors.address + OHCI_REQUEST_TD_OFFSET);
	for (index = 0; index < request->td_count; index++) {
		tds[index].control = read_word(&words[index * 4U + 0U]);
		tds[index].buffer = read_word(&words[index * 4U + 1U]);
		tds[index].next = read_word(&words[index * 4U + 2U]);
		tds[index].end = read_word(&words[index * 4U + 3U]);
	}

	/* The outcome, under the endpoint's head. */
	ed = request_ed(request);
	request->result = ohci_td_outcome(&request->plan, tds, request->td_count, read_word(&ed[2]), &request->actual, &request->condition);
}

/* Completes a retired request: its data, the endpoint's toggle and the core's completion. */
static void
request_finish(
	struct ohci_controller *controller,
	struct ohci_request *request)
{
	enum drv_usb_urb_status status;
	struct drv_usb_urb *urb;
	volatile uint32_t *ed;
	size_t actual;

	/* How it ended. */
	request_read_outcome(request);
	status = DRV_USB_URB_COMPLETE;
	if (request->result == OHCI_TRANSFER_STALL) {
		status = DRV_USB_URB_STALL;
	} else if (request->result != OHCI_TRANSFER_COMPLETE) {
		status = DRV_USB_URB_IO_ERROR;
		kern_logf("ohci: transfer ended with condition %u\n", request->condition);
	}

	/* The data received, into the transfer's buffer. */
	urb = request->urb;
	if (request->plan.input && request->actual != 0)
		kern_memcpy(drv_usb_urb_buffer(urb), request->data.address, request->actual);

	/* The toggle the endpoint reached, for its next transfer. */
	if (request->keeps_toggle) {
		ed = request_ed(request);
		(void)drv_usb_endpoint_set_hcd_data(request->endpoint, OHCI_ENDPOINT_TOGGLE_SLOT, (read_word(&ed[2]) & OHCI_ED_HEAD_TOGGLE) >> 1);
	}

	/* Frees the request, then hands the transfer back with the bytes moved. */
	actual = request->actual;
	(void)drv_usb_urb_set_hcd_data(urb, NULL);
	request_free(controller, request);
	drv_usb_hcd_complete(&controller->hcd, urb, status, actual);
}

/* Frees a request's buffers and record. */
static void
request_free(
	struct ohci_controller *controller,
	struct ohci_request *request)
{
	/* The bounce buffer and the descriptors, then the record. */
	if (request->data.address != NULL)
		drv_dma_free_coherent(controller->hcd.dma, &request->data);
	if (request->descriptors.address != NULL)
		drv_dma_free_coherent(controller->hcd.dma, &request->descriptors);
	kern_free(request);
}

/* Tells the USB core about root port changes, once the system can enumerate. */
static void
root_ports_check(
	struct ohci_controller *controller)
{
	unsigned long irq;
	uint32_t status;
	unsigned port;
	int changed;
	int ready;
	int first;

	/* Nothing before the first probe. */
	irq = spin_lock_irqsave(&controller->lock);

	ready = controller->root_ready;
	first = controller->root_force_scan;
	controller->root_force_scan = 0;

	spin_unlock_irqrestore(&controller->lock, irq);

	/* Waits until the system asks for the first scan. */
	if (!ready)
		return;

	/* The first scan waits for the ports' power to be good. */
	if (first)
		(void)frames_pass(controller, controller->power_good_ms + 20U);

	/* A port with a change bit, or the first scan. */
	changed = first;
	for (port = 1; port <= controller->hcd.root_port_count; port++) {
		status = read_register(controller, OHCI_RH_PORT_STATUS + (port - 1U) * 4U);
		if ((status & OHCI_PORT_CHANGES) != 0)
			changed = 1;
	}

	/* The core reads the ports and enumerates what changed. */
	if (changed)
		(void)drv_usb_hcd_root_hub_changed(&controller->hcd);
}

/* Writes one bit of action to a root port's status register. */
static void
port_write(
	struct ohci_controller *controller,
	unsigned port,
	uint32_t value)
{
	/* Each port's register follows the last. */
	write_register(controller, OHCI_RH_PORT_STATUS + (port - 1U) * 4U, value);
}

/*
 * Turns the root hub change interrupt back on once no root port has a
 * change left, after clearing a hub over-current change nothing else
 * clears.
 */
static void
root_change_interrupt_restore(
	struct ohci_controller *controller)
{
	unsigned long irq;
	uint32_t status;
	unsigned port;
	int off;

	/* Nothing to do while the interrupt is on. */
	irq = spin_lock_irqsave(&controller->lock);

	off = controller->rhsc_off;

	spin_unlock_irqrestore(&controller->lock, irq);
	if (!off)
		return;

	/* The hub's over-current change is cleared by writing it. */
	status = read_register(controller, OHCI_RH_STATUS);
	if ((status & OHCI_RH_STATUS_OVER_CURRENT_CHANGE) != 0) {
		write_register(controller, OHCI_RH_STATUS, OHCI_RH_STATUS_OVER_CURRENT_CHANGE);
		kern_logf("ohci: root hub over-current change cleared (status %08x)\n", status);
	}

	/* A port's change still set waits for the next look. */
	for (port = 1; port <= controller->hcd.root_port_count; port++) {
		status = read_register(controller, OHCI_RH_PORT_STATUS + (port - 1U) * 4U);
		if ((status & OHCI_PORT_CHANGES) != 0)
			return;
	}

	/* The changes are served: the interrupt again, its stale status cleared first. */
	irq = spin_lock_irqsave(&controller->lock);

	controller->rhsc_off = 0;
	write_register(controller, OHCI_INTERRUPT_STATUS, OHCI_INTERRUPT_RHSC);
	write_register(controller, OHCI_INTERRUPT_ENABLE, OHCI_INTERRUPT_RHSC);

	spin_unlock_irqrestore(&controller->lock, irq);
}

/* Writes one controller's state to the kernel log. */
static void
report_controller(
	struct ohci_controller *controller)
{
	struct drv_pci_address location;
	struct ohci_request *request;
	struct ohci_request *oldest;
	volatile uint32_t *ed;
	unsigned long irq;
	uint32_t ports[OHCI_REPORT_PORTS];
	uint32_t words[3];
	uint64_t now;
	unsigned requests;
	unsigned list;
	unsigned port;

	/* The transfers on the bus and the oldest of them, with its descriptor's words. */
	now = sched_ticks();
	requests = 0;
	oldest = NULL;
	list = OHCI_LIST_COUNT;
	kern_memset(words, 0, sizeof(words));
	irq = spin_lock_irqsave(&controller->lock);

	for (request = controller->requests; request != NULL; request = request->next) {
		requests++;
		if (oldest == NULL || request->submitted < oldest->submitted)
			oldest = request;
	}
	if (oldest != NULL) {
		ed = request_ed(oldest);
		words[0] = read_word(&ed[0]);
		words[1] = read_word(&ed[1]);
		words[2] = read_word(&ed[2]);
		list = oldest->list;
		now -= oldest->submitted;
	}

	spin_unlock_irqrestore(&controller->lock, irq);

	/* The first root ports' status. */
	kern_memset(ports, 0, sizeof(ports));
	for (port = 1; port <= controller->hcd.root_port_count && port <= OHCI_REPORT_PORTS; port++)
		ports[port - 1U] = read_register(controller, OHCI_RH_PORT_STATUS + (port - 1U) * 4U);

	/* The controller, its counts and registers. */
	drv_pci_device_address(controller->pci, &location);
	kern_logf("ohci %02x usb%u: irq=%u done=%u fm=%u ctl=%08x int=%08x/%08x%s%s\n",
		  (unsigned)location.device,
		  controller->bus != NULL ? drv_usb_bus_number(controller->bus) : 99U,
		  controller->interrupts, controller->retired,
		  read_register(controller, OHCI_FM_NUMBER) & 0xffffU,
		  read_register(controller, OHCI_CONTROL),
		  read_register(controller, OHCI_INTERRUPT_STATUS),
		  read_register(controller, OHCI_INTERRUPT_ENABLE),
		  controller->failed ? " FAILED" : "",
		  controller->rhsc_off ? " rhsc-off" : "");

	/* The first root ports, and how many transfers are on the bus. */
	kern_logf("ohci %02x: ports %08x %08x %08x %08x; %u on the bus\n",
		  (unsigned)location.device, ports[0], ports[1], ports[2], ports[3], requests);

	/* The oldest transfer: its list, age and endpoint descriptor's words. */
	if (oldest == NULL)
		return;
	kern_logf("ohci %02x: oldest %s %llums ed %08x %08x %08x\n",
		  (unsigned)location.device,
		  list == OHCI_LIST_CONTROL ? "control" : (list == OHCI_LIST_BULK ? "bulk" : "periodic"),
		  (unsigned long long)kern_ticks_to_ms(now),
		  words[0], words[1], words[2]);
}
