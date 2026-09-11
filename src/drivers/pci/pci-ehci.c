/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PCI EHCI host controller driver
 */

#include <drivers/pci-ehci.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <errno.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <limits.h>
#include <string.h>
#include "kern/klog.h"
#include "kern/kmem.h"
#include "kern/device-io.h"

#define EHCI_USBCMD 0x00U
#define EHCI_USBSTS 0x04U
#define EHCI_USBINTR 0x08U
#define EHCI_FRINDEX 0x0cU
#define EHCI_CTRLDSSEGMENT 0x10U
#define EHCI_PERIODICLISTBASE 0x14U
#define EHCI_ASYNCLISTADDR 0x18U
#define EHCI_CONFIGFLAG 0x40U
#define EHCI_PORTSC(n) (0x44U + 4U * (n))
#define EHCI_HCSPARAMS_PPC 0x00000010U
#define EHCI_HCSPARAMS_N_CC_MASK 0x0000f000U
#define EHCI_HCSPARAMS_N_CC_SHIFT 12U

#define EHCI_CMD_RUN 0x00000001U
#define EHCI_CMD_RESET 0x00000002U
#define EHCI_CMD_PERIODIC 0x00000010U
#define EHCI_CMD_ASYNC 0x00000020U
#define EHCI_CMD_IAAD 0x00000040U

#define EHCI_STS_USBINT 0x00000001U
#define EHCI_STS_USBERRINT 0x00000002U
#define EHCI_STS_PCD 0x00000004U
#define EHCI_STS_HSE 0x00000010U
#define EHCI_STS_IAA 0x00000020U
#define EHCI_STS_HALTED 0x00001000U
#define EHCI_STS_PERIODIC 0x00004000U
#define EHCI_STS_ASYNC 0x00008000U
#define EHCI_STS_ALL 0x0000003fU

#define EHCI_LINK_TERM 0x00000001U
#define EHCI_LINK_QH 0x00000002U
#define EHCI_QTD_ACTIVE 0x00000080U
#define EHCI_QTD_HALTED 0x00000040U
#define EHCI_QTD_ERRORS 0x0000007eU
#define EHCI_QTD_IOC 0x00008000U
#define EHCI_PID_OUT 0U
#define EHCI_PID_IN 1U
#define EHCI_PID_SETUP 2U

#define EHCI_PORT_CONNECT 0x00000001U
#define EHCI_PORT_CONNECT_CHANGE 0x00000002U
#define EHCI_PORT_ENABLE 0x00000004U
#define EHCI_PORT_ENABLE_CHANGE 0x00000008U
#define EHCI_PORT_OVER_CURRENT 0x00000010U
#define EHCI_PORT_OVER_CURRENT_CHANGE 0x00000020U
#define EHCI_PORT_FORCE_RESUME 0x00000040U
#define EHCI_PORT_SUSPEND 0x00000080U
#define EHCI_PORT_RESET 0x00000100U
#define EHCI_PORT_LINE_STATUS 0x00000c00U
#define EHCI_PORT_LINE_K_STATE 0x00000400U
#define EHCI_PORT_POWER 0x00001000U
#define EHCI_PORT_OWNER 0x00002000U
#define EHCI_PORT_INDICATOR 0x0000c000U
#define EHCI_PORT_TEST_CONTROL 0x000f0000U
#define EHCI_PORT_WAKE_BITS 0x00700000U
#define EHCI_PORT_CHANGE_BITS                                                  \
	(EHCI_PORT_CONNECT_CHANGE | EHCI_PORT_ENABLE_CHANGE |                  \
	 EHCI_PORT_OVER_CURRENT_CHANGE)
#define EHCI_PORT_RW_BITS                                                      \
	(EHCI_PORT_ENABLE | EHCI_PORT_FORCE_RESUME | EHCI_PORT_SUSPEND |       \
	 EHCI_PORT_RESET | EHCI_PORT_POWER | EHCI_PORT_OWNER |                 \
	 EHCI_PORT_INDICATOR | EHCI_PORT_TEST_CONTROL | EHCI_PORT_WAKE_BITS)

#define EHCI_MAX_QTDS 124U
#define EHCI_PCI_COMMAND 0x04U
#define EHCI_PCI_COMMAND_MASTER 0x0004U
#define EHCI_QUIESCE_TICKS 100U
#define EHCI_HARDWARE_STOP_WAIT_TICKS (EHCI_QUIESCE_TICKS * 3U)
#define EHCI_RETIRE_TICKS 100U
#define EHCI_ROOT_POLL_TICKS 10U
#define EHCI_PORT_POWER_GOOD_TICKS 2U
#define EHCI_PERIODIC_FRAMES 1024U
#define EHCI_PERIODIC_LEVELS 11U
#define EHCI_PERIODIC_NODES ((EHCI_PERIODIC_FRAMES * 2U) - 1U)
#define EHCI_PERIODIC_MICROFRAMES 8U
/*
 * A 125-us high-speed microframe carries 60,000 raw bit times; periodic
 * traffic is limited to 80 percent.  The reservation charges worst-case data
 * bit stuffing plus a deliberately conservative 512 bit times per scheduled
 * transaction for token, handshake, CRC, framing, and inter-packet gaps.
 */
#define EHCI_PERIODIC_BUDGET_BITS 48000U
#define EHCI_PERIODIC_TRANSACTION_BITS 512U
#define EHCI_MAX_ROOT_PORTS 15U
#define EHCI_ENDPOINT_STALL_PUBLISHING_SLOT 1U

enum ehci_request_state {
	EHCI_REQUEST_ACTIVE,
	EHCI_REQUEST_DEACTIVATING,
	EHCI_REQUEST_WAIT_IAA,
	EHCI_REQUEST_WAIT_PERIODIC,
	EHCI_REQUEST_COMPLETING,
	EHCI_REQUEST_RETIRED_CANCEL,
	EHCI_REQUEST_FAILED
};

enum ehci_schedule_class { EHCI_SCHEDULE_ASYNC, EHCI_SCHEDULE_PERIODIC };

enum ehci_retirement_reason {
	EHCI_RETIRE_COMPLETE,
	EHCI_RETIRE_CANCEL,
	EHCI_RETIRE_DISCONNECT
};

struct ehci_qtd {
	volatile uint32_t next;
	volatile uint32_t alternate;
	volatile uint32_t token;
	volatile uint32_t buffer[5];
};

/* Sixty-four-byte stride keeps every QH on the required 32-byte boundary. */
struct ehci_qh {
	volatile uint32_t horizontal;
	volatile uint32_t characteristics;
	volatile uint32_t capabilities;
	volatile uint32_t current;
	volatile uint32_t next;
	volatile uint32_t alternate;
	volatile uint32_t token;
	volatile uint32_t buffer[5];
	uint32_t reserved[4];
};

struct ehci_request {
	struct drv_usb_urb *urb;
	struct drv_usb_endpoint *endpoint;
	struct drv_dma_buffer schedule;
	struct drv_dma_buffer bounce;
	struct ehci_qh *qh;
	struct ehci_qtd *qtds;
	struct ehci_request *active_next;
	struct ehci_request *schedule_previous;
	struct ehci_request *schedule_next;
	struct ehci_request *retirement_next;
	unsigned qtd_count;
	unsigned data_first;
	unsigned data_count;
	unsigned periodic_node;
	unsigned periodic_period;
	unsigned periodic_phase;
	unsigned periodic_microframe_slots;
	unsigned periodic_cost;
	uint8_t periodic_service_mask;
	uint16_t requested[EHCI_MAX_QTDS];
	bool input;
	bool control;
	bool linked;
	bool periodic_reserved;
	bool retirement_queued;
	enum ehci_schedule_class schedule_class;
	enum ehci_request_state state;
	enum ehci_retirement_reason retirement_reason;
	enum drv_usb_urb_status completion_status;
	uint64_t retirement_generation;
	uint64_t retirement_started;
	unsigned iaa_observed;
	const char *failure_stage;
	int failure_error;
	unsigned failure_reported;
	bool reclaim_reserved;
};

struct ehci_controller {
	struct drv_pci_device *pci;
	struct drv_pci_mapping registers;
	struct drv_pci_enable_state pci_enable_state;
	volatile uint8_t *capability;
	volatile uint8_t *operational;
	struct drv_dma_buffer periodic;
	struct drv_dma_buffer periodic_skeleton_memory;
	struct drv_dma_buffer async_head_memory;
	struct ehci_qh *periodic_skeleton;
	struct ehci_qh *async_head;
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct spinlock active_lock;
	struct ehci_request *active;
	struct ehci_request *async_first;
	struct ehci_request *async_last;
	struct ehci_request *periodic_heads[EHCI_PERIODIC_NODES];
	struct ehci_request *retirement_head;
	struct ehci_request *retirement_tail;
	struct ehci_request *iaa_owner;
	struct thread *retirement_worker;
	struct thread *root_worker;
	struct ehci_request reclaim_request;
	struct ehci_controller *next;
	uint64_t retirement_generation;
	unsigned periodic_phase_next[EHCI_PERIODIC_LEVELS];
	unsigned periodic_microframe_phase_next[3];
	uint16_t periodic_budget[EHCI_PERIODIC_FRAMES]
				[EHCI_PERIODIC_MICROFRAMES];
	unsigned bar_claimed;
	unsigned bar_mapped;
	unsigned pci_state_saved;
	unsigned hcd_registered;
	unsigned irq_allocated;
	unsigned dma_quiesced;
	unsigned quiescing;
	unsigned builders;
	unsigned active_count;
	unsigned completion_inflight;
	unsigned reclaim_request_busy;
	unsigned periodic_updating;
	unsigned listed;
	unsigned quarantined;
	unsigned fatal_mmio_invalid;
	unsigned hardware_stop_in_progress;
	unsigned hardware_stopped;
	unsigned hardware_stop_waiters;
	uint64_t hardware_stop_generation;
	uint64_t hardware_stop_result_generation;
	int hardware_stop_error;
	unsigned retirement_success_reported;
#ifdef KERN_TEST_CHECKPOINTS
	unsigned shutdown_evidence_reported;
#endif
	volatile unsigned fatal_pending;
	volatile unsigned fatal_stopping;
	volatile unsigned retirement_pending;
	volatile unsigned retirement_stopping;
	unsigned retirement_joining;
	volatile unsigned root_pending;
	volatile unsigned root_stopping;
	unsigned root_ready;
	unsigned root_dispatching;
	unsigned root_joining;
	uint64_t root_wake_generation;
	uint32_t root_port_status[EHCI_MAX_ROOT_PORTS];
	unsigned root_port_status_valid;
	volatile unsigned root_force_scan;
	unsigned port_power_control;
	unsigned companion_count;
};

static struct ehci_controller *ehci_controllers;

static int ehci_root_ports_changed(struct ehci_controller *controller);
static uint32_t rd32(volatile uint8_t *registers, unsigned offset);
static void ehci_controller_fail_locked(struct ehci_controller *controller, int error, const char *stage);
static void ehci_retirement_worker_wakeup(struct ehci_controller *controller);
static void ehci_root_worker_arm(struct ehci_controller *controller);
static uint8_t rd8(volatile uint8_t *registers, unsigned offset);
static void wr32(volatile uint8_t *registers, unsigned offset, uint32_t value);
static void ehci_port_write(struct ehci_controller *controller, unsigned port, uint32_t snapshot, uint32_t set, uint32_t clear, uint32_t acknowledge);
static int ehci_port_handoff(struct ehci_controller *controller, unsigned port, uint32_t status);
static int ehci_port_finish_reset(struct ehci_controller *controller, unsigned port, uint32_t snapshot);
static struct ehci_controller *hcd_controller(struct drv_usb_hcd *hcd);
static int ehci_ownership(struct ehci_controller *controller);
static uint32_t ehci_skeleton_link(struct ehci_controller *controller, unsigned index);
static uint32_t ehci_request_link(const struct ehci_request *request);
static int ehci_dma_buffer_is_32bit(const struct drv_dma_buffer *buffer);
static unsigned ehci_reverse_bits(unsigned value, unsigned count);
static int ehci_periodic_reserve_locked(struct ehci_controller *controller, struct ehci_request *request, uint8_t service_mask);
static void ehci_periodic_release_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_schedule_release(struct ehci_controller *controller);
static int ehci_schedule_initialize(struct ehci_controller *controller);
static int ehci_wait_schedule_status(struct ehci_controller *controller, uint32_t set, uint32_t clear);
static int ehci_start(struct drv_usb_hcd *hcd);
static int ehci_hardware_stop(struct ehci_controller *controller, const char *owner);
static int ehci_bus_master_disable(struct ehci_controller *controller);
static int ehci_irq_disestablish(struct ehci_controller *controller);
static uint32_t ehci_qtd_token(unsigned pid, unsigned toggle, unsigned length);
static void ehci_qtd_buffer(struct ehci_qtd *qtd, uint32_t address, size_t length);
static int ehci_add_qtd(struct ehci_request *request, unsigned pid, unsigned toggle, unsigned length, uint32_t buffer);
static void ehci_request_free(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_reclaim_request_acquire(struct ehci_controller *controller, size_t length, struct ehci_request **result);
static int ehci_periodic_parameters(uint8_t interval, unsigned *period, uint32_t *service_mask, unsigned *microframe_slots);
static int ehci_build_request(struct ehci_controller *controller, struct drv_usb_urb *urb, struct ehci_request **result);
static struct ehci_request * ehci_endpoint_owner_locked(struct ehci_controller *controller, struct drv_usb_endpoint *endpoint);
static void ehci_active_insert_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_active_remove_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_async_insert_locked(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_async_unlink_locked(struct ehci_controller *controller, struct ehci_request *request);
static uint32_t ehci_periodic_parent_link(struct ehci_controller *controller, unsigned node);
static void ehci_periodic_insert_locked(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_periodic_unlink_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_builder_leave(struct ehci_controller *controller);
static void ehci_unpublished_request_discard(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_periodic_update_acquire(struct ehci_controller *controller, int retirement);
static void ehci_periodic_update_release(struct ehci_controller *controller);
static int ehci_periodic_pause(struct ehci_controller *controller);
static int ehci_periodic_resume(struct ehci_controller *controller);
static int ehci_publish_async_request(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_publish_periodic_request(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_urb_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb);
static int ehci_request_is_active_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_retirement_enqueue_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_retirement_pop_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_retirement_begin_locked(struct ehci_controller *controller, struct ehci_request *request, enum ehci_retirement_reason reason, enum drv_usb_urb_status status);
static int ehci_request_terminal(struct ehci_request *request, enum drv_usb_urb_status *result);
static size_t ehci_request_actual(struct ehci_request *request);
static void ehci_request_commit_toggle(struct ehci_request *request);
static void ehci_retirement_report(struct ehci_controller *controller);
static int ehci_retirement_begin_iaa_locked(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_retirement_observe_iaa_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_retirement_finish_locked(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_complete_retired_request(struct ehci_controller *controller, struct ehci_request *request);
static int ehci_retire_periodic_request(struct ehci_controller *controller, struct ehci_request *request);
static void ehci_retirement_progress(struct ehci_controller *controller);
static void ehci_controller_fatal_stop(struct ehci_controller *controller);
static int ehci_root_worker_stop(struct ehci_controller *controller, int wait_dispatch);
static void ehci_retirement_worker(void *argument);
static int ehci_retirement_worker_start(struct ehci_controller *controller);
static int ehci_retirement_worker_stop(struct ehci_controller *controller);
static int ehci_urb_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb);
static void ehci_root_worker(void *argument);
static int ehci_root_worker_start(struct ehci_controller *controller);
static void ehci_root_worker_request_stop(struct ehci_controller *controller);
static void ehci_root_event_defer(struct ehci_controller *controller);
static int ehci_quiesce_requests(struct ehci_controller *controller);
static int ehci_report_shutdown_evidence(struct ehci_controller *controller);
static int ehci_quiesce(struct drv_usb_hcd *hcd);
static void ehci_stop(struct drv_usb_hcd *hcd);
static int ehci_endpoint_enable(struct drv_usb_hcd *hcd, struct drv_usb_endpoint *endpoint);
static int ehci_endpoint_disable(struct drv_usb_hcd *hcd, struct drv_usb_endpoint *endpoint);
static int ehci_endpoint_reset(struct drv_usb_hcd *hcd, struct drv_usb_endpoint *endpoint);
static uint32_t ehci_frame_number(struct drv_usb_hcd *hcd);
static int ehci_root_status(struct drv_usb_hcd *hcd, void *buffer, size_t size, size_t *actual);
static int ehci_root_control(struct drv_usb_hcd *hcd, const struct drv_usb_control_request *request, void *buffer, size_t size, size_t *actual);
static int ehci_irq(void *argument);
static void ehci_publish(struct ehci_controller *controller);
static void ehci_unpublish(struct ehci_controller *controller);
static int ehci_pci_release(struct ehci_controller *controller);
static int ehci_cleanup(struct ehci_controller *controller);
static int ehci_runtime_operational(struct ehci_controller *controller);
static int ehci_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int ehci_detach(struct drv_pci_device *device, unsigned flags);

/* Device operation and registration tables. */
static const struct drv_usb_hcd_ops ehci_ops = {
	.start = ehci_start,
	.quiesce = ehci_quiesce,
	.stop = ehci_stop,
	.urb_enqueue = ehci_urb_enqueue,
	.urb_dequeue = ehci_urb_dequeue,
	.endpoint_enable = ehci_endpoint_enable,
	.endpoint_disable = ehci_endpoint_disable,
	.endpoint_reset = ehci_endpoint_reset,
	.frame_number = ehci_frame_number,
	.root_hub_status = ehci_root_status,
	.root_hub_control = ehci_root_control
};

static const struct drv_pci_id ehci_ids[] = {
	{
		DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
		DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
		0x0c0320U, 0xffffffU, 0
	}
};

static struct drv_pci_driver ehci_driver = {
	.name = "ehci",
	.ids = ehci_ids,
	.id_count = 1,
	.attach = ehci_attach,
	.detach = ehci_detach
};

/*
 * Registers this driver with the PCI bus.
 */
int
drv_pci_ehci_driver_register(
	void)
{
	int error;

	/* Obtains the drv pci driver register result. */
	error = drv_pci_driver_register(&ehci_driver);

	/* Returns the computed result. */
	return error;
}

/*
 * Probes the root ports of every controller this driver holds.
 */
void
drv_pci_ehci_probe_roots(
	void)
{
	struct ehci_controller *controller;

	/* Process each linked entry. */
	for (controller = ehci_controllers; controller != NULL;
	     controller = controller->next) {
		/* Handles the controller condition. */
		if (controller->quarantined)
			continue;

		/* The boot caller may be the idle thread. Never let it hold the
		 * topology gate while a runnable companion waits for that gate. */
		ehci_root_worker_arm(controller);
	}
}

/* Asks whether any root port has reported a change. */
static int
ehci_root_ports_changed(
	struct ehci_controller *controller)
{
	uint32_t current[EHCI_MAX_ROOT_PORTS];
	unsigned ports = controller->hcd.root_port_count;
	unsigned index;
	unsigned long irq;
	int changed;
	int report_failure = 0;

	/* Handles the ports condition. */
	if (ports > EHCI_MAX_ROOT_PORTS)
		return 0;
	/* Process each remaining element. */
	for (index = 0; index < ports; index++) {
		current[index] =
			rd32(controller->operational, EHCI_PORTSC(index));

		/* Handles the current condition. */
		if (current[index] == UINT32_MAX) {
			irq = spin_lock_irqsave(&controller->active_lock);
			report_failure = !controller->quarantined;
			controller->fatal_mmio_invalid = 1;
			ehci_controller_fail_locked(controller, EIO,
						    "root-port status read");
			controller->root_ready = 0;
			__atomic_store_n(&controller->root_stopping, 1U,
					 __ATOMIC_RELEASE);
			spin_unlock_irqrestore(&controller->active_lock, irq);
			if (report_failure) {
				kern_logf(
					"ehci: root-port register unavailable; "
					"controller quarantined\n");
			}

			ehci_retirement_worker_wakeup(controller);

			/* Succeeded. */
			return 0;
		}
	}

	/* Takes the pending root-port change flags. */
	irq = spin_lock_irqsave(&controller->active_lock);

	changed = __atomic_exchange_n(&controller->root_force_scan, 0U,
				      __ATOMIC_ACQ_REL) != 0 ||
		  __atomic_exchange_n(&controller->root_pending, 0U,
				      __ATOMIC_ACQ_REL) != 0 ||
		  !controller->root_port_status_valid;
	/* Process each remaining element. */
	for (index = 0; index < ports; index++) {
		/* Handles the current condition. */
		if ((current[index] &
		     (EHCI_PORT_CONNECT_CHANGE | EHCI_PORT_ENABLE_CHANGE |
		      EHCI_PORT_OVER_CURRENT_CHANGE)) != 0 ||
		    ((current[index] ^ controller->root_port_status[index]) &
		     (EHCI_PORT_CONNECT | EHCI_PORT_ENABLE |
		      EHCI_PORT_OVER_CURRENT | EHCI_PORT_OWNER)) != 0)
			changed = 1;
		controller->root_port_status[index] = current[index];
	}

	controller->root_port_status_valid = 1;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Returns the computed result. */
	return changed;
}

/* Reads one 32-bit controller register. */
static uint32_t
rd32(
	volatile uint8_t *registers,
	unsigned offset)
{
	/* Returns the computed result. */
	return *(volatile uint32_t *)(registers + offset);
}

/* Marks the controller as failed and stops taking requests. */
static void
ehci_controller_fail_locked(
	struct ehci_controller *controller,
	int error,
	const char *stage)
{
	struct ehci_request *request;

	controller->quiescing = 1;
	controller->quarantined = 1;
	__atomic_store_n(&controller->fatal_pending, 1U, __ATOMIC_RELEASE);
	/* Process each element required by the operation. */
	for (request = controller->active; request != NULL;
	     request = request->active_next) {
		/*
		 * Once checked retirement publishes a terminal owner,
		 * fatalization must not steal the request while that owner
		 * removes it from active. Every earlier state still retains its
		 * request and DMA as FAILED.
		 */
		if (request->state == EHCI_REQUEST_FAILED ||
		    request->state == EHCI_REQUEST_COMPLETING ||
		    request->state == EHCI_REQUEST_RETIRED_CANCEL)
			continue;
		request->state = EHCI_REQUEST_FAILED;
		request->failure_error = error != 0 ? error : EIO;
		request->failure_stage = stage;
	}
}

/* Wakes the thread that retires finished requests. */
static void
ehci_retirement_worker_wakeup(
	struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the atomic load n result. */
	worker = controller->retirement_worker;
	if (worker == NULL || controller->retirement_joining ||
	    __atomic_load_n(&controller->retirement_stopping,
			    __ATOMIC_ACQUIRE) != 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Returns the computed result. */
		return;
	}

	__atomic_store_n(&controller->retirement_pending, 1U, __ATOMIC_RELEASE);
	kernel_notify_task(worker->task);

	spin_unlock_irqrestore(&controller->active_lock, irq);
}

/* Arms the thread that serves root port changes. */
static void
ehci_root_worker_arm(
	struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	/* Checks the atomic load n result. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->root_worker == NULL || controller->root_joining ||
	    __atomic_load_n(&controller->root_stopping, __ATOMIC_ACQUIRE) !=
		    0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Returns the computed result. */
		return;
	}

	__atomic_store_n(&controller->root_force_scan, 1U, __ATOMIC_RELEASE);
	controller->root_ready = 1;
	controller->root_wake_generation++;
	worker = controller->root_worker;
	kernel_notify_task(worker->task);

	spin_unlock_irqrestore(&controller->active_lock, irq);
}

/* Reads one byte of a controller register. */
static uint8_t
rd8(
	volatile uint8_t *registers,
	unsigned offset)
{
	/* Returns the computed result. */
	return registers[offset];
}

/* Writes one 32-bit controller register. */
static void
wr32(
	volatile uint8_t *registers,
	unsigned offset,
	uint32_t value)
{
	*(volatile uint32_t *)(registers + offset) = value;

	kern_io_barrier();
}

/* Writes one root port register, keeping the bits it must not clear. */
static void
ehci_port_write(
	struct ehci_controller *controller,
	unsigned port,
	uint32_t snapshot,
	uint32_t set,
	uint32_t clear,
	uint32_t acknowledge)
{
	uint32_t value = snapshot & EHCI_PORT_RW_BITS;

	value &= ~(clear & EHCI_PORT_RW_BITS);
	value |= set & EHCI_PORT_RW_BITS;

	/*
	 * PORTSC change bits are W1C.  Writing zero for every non-target change
	 * preserves an edge which arrived before or during this unrelated
	 * reset/power/owner operation.  RO and reserved bits are never echoed.
	 */
	value |= acknowledge & EHCI_PORT_CHANGE_BITS;
	wr32(controller->operational, EHCI_PORTSC(port), value);
}

/* Hands a port that is not high speed to the companion controller. */
static int
ehci_port_handoff(
	struct ehci_controller *controller,
	unsigned port,
	uint32_t status)
{
	/* Handles the controller condition. */
	if (controller->companion_count == 0)
		return ENOTSUP;
	ehci_port_write(controller, port, status, EHCI_PORT_OWNER, 0, 0);
	status = rd32(controller->operational, EHCI_PORTSC(port));
	kern_io_barrier();
	if (status == UINT32_MAX)
		return EIO;
	if ((status & EHCI_PORT_CONNECT) == 0)
		return ENODEV;

	/* Returns the computed result. */
	return (status & EHCI_PORT_OWNER) != 0 ? 0 : EIO;
}

/* Ends a port reset and reads what speed the device is. */
static int
ehci_port_finish_reset(
	struct ehci_controller *controller,
	unsigned port,
	uint32_t snapshot)
{
	int error;
	uint64_t started = sched_ticks();
	uint32_t status;

	ehci_port_write(controller, port, snapshot, 0, EHCI_PORT_RESET, 0);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		status = rd32(controller->operational, EHCI_PORTSC(port));
		kern_io_barrier();
		if (status == UINT32_MAX)
			return EIO;
		if ((status & EHCI_PORT_RESET) == 0)
			break;

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS)
			return ETIMEDOUT;
		sched_yield();
	}

	/*
	 * A connected port which did not become enabled after reset is a
	 * full-speed device for the companion controller.  A high-speed device
	 * remains owned by EHCI with PED set.
	 */
	if ((status & (EHCI_PORT_CONNECT | EHCI_PORT_ENABLE |
		       EHCI_PORT_OWNER)) == EHCI_PORT_CONNECT) {
		/* Obtains the ehci port handoff result. */
		error = ehci_port_handoff(controller, port, status);

		/* Failed. */
		return error;
	}

	/* Succeeded. */
	return 0;
}

/* Takes the controller behind a host controller handle. */
static struct ehci_controller *
hcd_controller(
	struct drv_usb_hcd *hcd)
{
	/* Returns the computed result. */
	return (struct ehci_controller *)hcd->private_data[0];
}

/* Takes ownership of the controller from the firmware. */
static int
ehci_ownership(
	struct ehci_controller *controller)
{
	uint32_t capability;
	uint32_t hcc = rd32(controller->capability, 8U);
	unsigned eecp = (hcc >> 8) & 0xffU;
	unsigned guard = 0;

	/* Continue while the operation condition remains true. */
	while (eecp >= 0x40U && guard++ < 32U) {
		/* Checks the drv pci device config read32 result. */
		if (drv_pci_device_config_read32(controller->pci, eecp,
						 &capability) != 0) {
			/* Failed. */
			return EIO;
		}

		/* Handles the capability condition. */
		if ((capability & 0xffU) == 1U) {
			capability |= 0x01000000U;

			/* Checks the drv pci device config write32 result. */
			if (drv_pci_device_config_write32(controller->pci, eecp,
							  capability) != 0) {
				/* Failed. */
				return EIO;
			}
			/* Process each element required by the operation. */
			for (guard = 0; guard < 1000000U; guard++) {
				/* Checks the drv pci device config read32 result. */
				if (drv_pci_device_config_read32(
					    controller->pci, eecp,
					    &capability) != 0) {
					/* Failed. */
					return EIO;
				}

				/* Handles the capability condition. */
				if ((capability & 0x00010000U) == 0)
					return 0;
			}

			/* Failed. */
			return ETIMEDOUT;
		}

		eecp = (capability >> 8) & 0xffU;
	}

	/* Succeeded. */
	return 0;
}

/* Builds the empty periodic schedule every frame starts from. */
static uint32_t
ehci_skeleton_link(
	struct ehci_controller *controller,
	unsigned index)
{
	uint32_t function_result;

	/* Computes the function result. */
	function_result =
		(uint32_t)(controller->periodic_skeleton_memory.device_address +
			   (uint64_t)index *
				   sizeof(*controller->periodic_skeleton)) |
		EHCI_LINK_QH;

	/* Returns the computed result. */
	return function_result;
}

/* Reports the descriptor address a request begins at. */
static uint32_t
ehci_request_link(
	const struct ehci_request *request)
{
	/* Returns the computed result. */
	return (uint32_t)request->schedule.device_address | EHCI_LINK_QH;
}

/* Asks whether a buffer lies inside the first four gigabytes. */
static int
ehci_dma_buffer_is_32bit(
	const struct drv_dma_buffer *buffer)
{
	/* Returns the computed result. */
	return buffer->size != 0 && buffer->device_address <= UINT32_MAX &&
	       (uint64_t)(buffer->size - 1U) <=
		       (uint64_t)UINT32_MAX - (uint64_t)buffer->device_address;
}

/* Reverses the bits of a frame index, as the schedule is ordered. */
static unsigned
ehci_reverse_bits(
	unsigned value,
	unsigned count)
{
	unsigned result = 0;

	/* Process each remaining element. */
	while (count-- != 0) {
		result = (result << 1) | (value & 1U);
		value >>= 1;
	}

	/* Returns the computed result. */
	return result;
}

/* Reserves the bandwidth one periodic endpoint needs. */
static int
ehci_periodic_reserve_locked(
	struct ehci_controller *controller,
	struct ehci_request *request,
	uint8_t service_mask)
{
	unsigned frame, microframe;

	/* Handles the request condition. */
	if (request->periodic_reserved || service_mask == 0 ||
	    request->periodic_cost == 0 ||
	    request->periodic_cost > EHCI_PERIODIC_BUDGET_BITS ||
	    request->periodic_period == 0 ||
	    request->periodic_period > EHCI_PERIODIC_FRAMES ||
	    (request->periodic_period & (request->periodic_period - 1U)) != 0 ||
	    request->periodic_phase >= request->periodic_period) {
		/* Failed. */
		return EINVAL;
	}
	/* Process each element required by the operation. */
	for (frame = request->periodic_phase; frame < EHCI_PERIODIC_FRAMES;
	     frame += request->periodic_period) {
		/* Process each element required by the operation. */
		for (microframe = 0; microframe < EHCI_PERIODIC_MICROFRAMES;
		     microframe++) {
			/* Handles the service mask condition. */
			if ((service_mask & (1U << microframe)) == 0)
				continue;

			/* Handles the controller condition. */
			if (controller->periodic_budget[frame][microframe] >
			    EHCI_PERIODIC_BUDGET_BITS - request->periodic_cost) {
				/* Failed. */
				return ENOSPC;
			}
		}
	}

	/* Process each element required by the operation. */
	for (frame = request->periodic_phase; frame < EHCI_PERIODIC_FRAMES;
	     frame += request->periodic_period) {
		/* Process each element required by the operation. */
		for (microframe = 0; microframe < EHCI_PERIODIC_MICROFRAMES;
		     microframe++) {
			/* Handles the service mask condition. */
			if ((service_mask & (1U << microframe)) != 0) {
				controller
					->periodic_budget[frame][microframe] +=
					(uint16_t)request->periodic_cost;
			}
		}
	}

	request->periodic_service_mask = service_mask;
	request->periodic_reserved = true;

	/* Succeeded. */
	return 0;
}

/* Gives that bandwidth back. */
static void
ehci_periodic_release_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	unsigned frame, microframe;

	/* Handles the request condition. */
	if (!request->periodic_reserved)
		return;
	/* Process each element required by the operation. */
	for (frame = request->periodic_phase; frame < EHCI_PERIODIC_FRAMES;
	     frame += request->periodic_period) {
		/* Process each element required by the operation. */
		for (microframe = 0; microframe < EHCI_PERIODIC_MICROFRAMES;
		     microframe++) {
			/* Handles the request condition. */
			if ((request->periodic_service_mask &
			     (1U << microframe)) == 0)
				continue;

			/* Handles the controller condition. */
			if (controller->periodic_budget[frame][microframe] <
			    request->periodic_cost)
				__builtin_trap();
			controller->periodic_budget[frame][microframe] -=
				(uint16_t)request->periodic_cost;
		}
	}

	request->periodic_reserved = false;
	request->periodic_service_mask = 0;
}

/* Gives the memory both schedules live in back. */
static void
ehci_schedule_release(
	struct ehci_controller *controller)
{
	/* Handles the controller condition. */
	if (controller->reclaim_request_busy)
		__builtin_trap();

	/* Handles the address availability. */
	if (controller->reclaim_request.schedule.address != NULL) {
		drv_dma_free_coherent(controller->hcd.dma,
				      &controller->reclaim_request.schedule);
	}

	/* Handles the address availability. */
	if (controller->reclaim_request.bounce.address != NULL) {
		drv_dma_free_coherent(controller->hcd.dma,
				      &controller->reclaim_request.bounce);
	}

	memset(&controller->reclaim_request, 0,
	       sizeof(controller->reclaim_request));

	/* Handles the address availability. */
	if (controller->async_head_memory.address != NULL) {
		drv_dma_free_coherent(controller->hcd.dma,
				      &controller->async_head_memory);
		memset(&controller->async_head_memory, 0,
		       sizeof(controller->async_head_memory));
		controller->async_head = NULL;
	}

	/* Handles the address availability. */
	if (controller->periodic_skeleton_memory.address != NULL) {
		drv_dma_free_coherent(controller->hcd.dma,
				      &controller->periodic_skeleton_memory);
		memset(&controller->periodic_skeleton_memory, 0,
		       sizeof(controller->periodic_skeleton_memory));
		controller->periodic_skeleton = NULL;
	}

	/* Handles the address availability. */
	if (controller->periodic.address != NULL) {
		drv_dma_free_coherent(controller->hcd.dma,
				      &controller->periodic);
		memset(&controller->periodic, 0, sizeof(controller->periodic));
	}
}

/* Builds the asynchronous and periodic schedules. */
static int
ehci_schedule_initialize(
	struct ehci_controller *controller)
{
	struct ehci_qh *qh;
	uint32_t *frames;
	unsigned index;
	int error;

	/* Checks the operation status. */
	error = drv_dma_alloc_coherent(controller->hcd.dma, 4096U, 4096U,
				       &controller->periodic);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_dma_alloc_coherent(
		controller->hcd.dma,
		EHCI_PERIODIC_NODES * sizeof(*controller->periodic_skeleton),
		64U, &controller->periodic_skeleton_memory);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = drv_dma_alloc_coherent(controller->hcd.dma, 4096U, 64U,
				       &controller->async_head_memory);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = drv_dma_alloc_coherent(controller->hcd.dma, 4096U, 64U,
				       &controller->reclaim_request.schedule);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = drv_dma_alloc_coherent(
		controller->hcd.dma,
		DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE +
			sizeof(struct drv_usb_control_request),
		64U, &controller->reclaim_request.bounce);
	if (error != 0)
		goto fail;

	/* Checks the ehci dma buffer is 32bit result. */
	if (!ehci_dma_buffer_is_32bit(&controller->periodic) ||
	    !ehci_dma_buffer_is_32bit(&controller->periodic_skeleton_memory) ||
	    !ehci_dma_buffer_is_32bit(&controller->async_head_memory) ||
	    !ehci_dma_buffer_is_32bit(&controller->reclaim_request.schedule) ||
	    !ehci_dma_buffer_is_32bit(&controller->reclaim_request.bounce)) {
		error = EOVERFLOW;
		goto fail;
	}

	controller->reclaim_request.reclaim_reserved = true;

	controller->periodic_skeleton =
		controller->periodic_skeleton_memory.address;
	memset(controller->periodic_skeleton, 0,
	       EHCI_PERIODIC_NODES * sizeof(*controller->periodic_skeleton));
	/* Process each remaining element. */
	for (index = 0; index < EHCI_PERIODIC_NODES; index++) {
		qh = &controller->periodic_skeleton[index];

		qh->horizontal =
			index == 0 ? EHCI_LINK_TERM
				   : ehci_skeleton_link(controller,
							(index - 1U) / 2U);
		qh->characteristics = (2U << 12) | (64U << 16);

		/*
		 * Periodic-list QHs require a nonzero S-mask even when they are
		 * controller-owned skeleton nodes rather than transfer owners.
		 */
		qh->capabilities = (1U << 30) | 0x01U;
		qh->next = EHCI_LINK_TERM;
		qh->alternate = EHCI_LINK_TERM;
	}

	frames = controller->periodic.address;
	/* Process each remaining element. */
	for (index = 0; index < EHCI_PERIODIC_FRAMES; index++) {
		frames[index] = ehci_skeleton_link(
			controller,
			EHCI_PERIODIC_FRAMES - 1U +
				ehci_reverse_bits(index,
						  EHCI_PERIODIC_LEVELS - 1U));
	}

	/* Builds the asynchronous list head that points at itself. */
	memset(controller->async_head_memory.address, 0, 4096U);
	controller->async_head = controller->async_head_memory.address;
	controller->async_head->horizontal =
		(uint32_t)controller->async_head_memory.device_address |
		EHCI_LINK_QH;
	controller->async_head->characteristics =
		(1U << 15) | (2U << 12) | (64U << 16);
	controller->async_head->capabilities = 1U << 30;
	controller->async_head->next = EHCI_LINK_TERM;
	controller->async_head->alternate = EHCI_LINK_TERM;
	kern_io_write_barrier();

	/* Succeeded. */
	return 0;

fail:
	ehci_schedule_release(controller);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Waits for the controller to report a schedule stopped or started. */
static int
ehci_wait_schedule_status(
	struct ehci_controller *controller,
	uint32_t set,
	uint32_t clear)
{
	uint32_t status;
	uint64_t started = sched_ticks();

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		status = rd32(controller->operational, EHCI_USBSTS);

		kern_io_barrier();

		/* Checks the operation status. */
		if (status == UINT32_MAX || (status & EHCI_STS_HSE) != 0)
			return EIO;

		/* Checks the operation status. */
		if ((status & set) == set && (status & clear) == 0)
			return 0;

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS)
			return ETIMEDOUT;
		sched_yield();
	}
}

/* Brings the controller into service. */
static int
ehci_start(
	struct drv_usb_hcd *hcd)
{
	uint32_t status;
	struct ehci_controller *controller = hcd_controller(hcd);
	unsigned port, timeout;
	unsigned long irq;
	int error;
	int run_started = 0;
	int stop_error;

	/* Checks the operation status. */
	error = ehci_ownership(controller);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = ehci_schedule_initialize(controller);
	if (error != 0)
		return error;

	/* Clears the hardware-stop state a previous run left behind. */
	irq = spin_lock_irqsave(&controller->active_lock);

	controller->hardware_stop_in_progress = 0;
	controller->hardware_stopped = 0;
	controller->hardware_stop_waiters = 0;
	controller->hardware_stop_generation = 0;
	controller->hardware_stop_result_generation = 0;
	controller->hardware_stop_error = 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	wr32(controller->operational, EHCI_USBCMD,
	     rd32(controller->operational, EHCI_USBCMD) & ~EHCI_CMD_RUN);

	/* Checks the operation status. */
	error = ehci_wait_schedule_status(controller, EHCI_STS_HALTED, 0);
	if (error != 0)
		goto fail;
	wr32(controller->operational, EHCI_USBCMD, EHCI_CMD_RESET);
	/* Process each element required by the operation. */
	for (timeout = 0; timeout < 1000000U; timeout++) {
		/* Checks the rd32 result. */
		if ((rd32(controller->operational, EHCI_USBCMD) &
		     EHCI_CMD_RESET) == 0)
			break;
	}

	/* Handles the timeout condition. */
	if (timeout == 1000000U) {
		error = ETIMEDOUT;
		goto fail;
	}

	wr32(controller->operational, EHCI_CTRLDSSEGMENT, 0);
	wr32(controller->operational, EHCI_PERIODICLISTBASE,
	     (uint32_t)controller->periodic.device_address);
	wr32(controller->operational, EHCI_ASYNCLISTADDR,
	     (uint32_t)controller->async_head_memory.device_address);
	wr32(controller->operational, EHCI_USBSTS, EHCI_STS_ALL);
	wr32(controller->operational, EHCI_USBINTR, 0);
	wr32(controller->operational, EHCI_CONFIGFLAG, 1);

	/* Handles the controller condition. */
	if (controller->port_power_control) {
		/* Process each remaining element. */
		for (port = 0; port < controller->hcd.root_port_count; port++) {
			status = rd32(controller->operational,
				      EHCI_PORTSC(port));

			kern_io_barrier();

			/* Checks the operation status. */
			if (status == UINT32_MAX) {
				error = EIO;
				goto fail;
			}

			ehci_port_write(controller, port, status,
					EHCI_PORT_POWER, 0, 0);
		}

		sched_sleep(sched_ticks() + EHCI_PORT_POWER_GOOD_TICKS);
	}

	irq = spin_lock_irqsave(&controller->active_lock);

	controller->dma_quiesced = 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	wr32(controller->operational, EHCI_USBCMD,
	     EHCI_CMD_RUN | EHCI_CMD_ASYNC | EHCI_CMD_PERIODIC);
	run_started = 1;

	/* Checks the operation status. */
	error = ehci_wait_schedule_status(controller,
					  EHCI_STS_ASYNC | EHCI_STS_PERIODIC,
					  EHCI_STS_HALTED);
	if (error != 0)
		goto fail;

	/* Resets every list and counter the controller drives. */
	irq = spin_lock_irqsave(&controller->active_lock);

	controller->quiescing = 0;
	controller->builders = 0;
	controller->active_count = 0;
	controller->completion_inflight = 0;
	controller->active = NULL;
	controller->async_first = NULL;
	controller->async_last = NULL;
	controller->retirement_head = NULL;
	controller->retirement_tail = NULL;
	controller->iaa_owner = NULL;
	controller->retirement_joining = 0;
	controller->periodic_updating = 0;
	controller->fatal_mmio_invalid = 0;
	__atomic_store_n(&controller->fatal_pending, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&controller->fatal_stopping, 0U, __ATOMIC_RELEASE);

	/* Clears the periodic schedule and its budget tables. */
	memset(controller->periodic_heads, 0,
	       sizeof(controller->periodic_heads));
	memset(controller->periodic_phase_next, 0,
	       sizeof(controller->periodic_phase_next));
	memset(controller->periodic_microframe_phase_next, 0,
	       sizeof(controller->periodic_microframe_phase_next));
	memset(controller->periodic_budget, 0,
	       sizeof(controller->periodic_budget));

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Succeeded. */
	return 0;

fail:

	/* Handles the run started condition. */
	if (!run_started) {
		ehci_schedule_release(controller);

		/* Failed. */
		return error;
	}

	irq = spin_lock_irqsave(&controller->active_lock);

	controller->quiescing = 1;
	controller->quarantined = 1;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	stop_error = ehci_hardware_stop(controller, "start failure");
	if (stop_error == 0) {
		irq = spin_lock_irqsave(&controller->active_lock);
		controller->dma_quiesced = 1;
		spin_unlock_irqrestore(&controller->active_lock, irq);
		ehci_schedule_release(controller);
	} else {
		kern_logf("ehci: start failed (%d), DMA stop failed (%d); "
			   "schedule retained\n",
			   error, stop_error);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Stops the controller itself. */
static int
ehci_hardware_stop(
	struct ehci_controller *controller,
	const char *owner)
{
	uint64_t started = sched_ticks();
	uint64_t generation = 0;
	uint32_t command;
	uint32_t status;
	unsigned long irq;
	int error;
	int halt_error = 0;
	int irq_error;
	int master_error;
	int mmio_invalid;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the controller condition. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->hardware_stopped) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Succeeded. */
			return 0;
		}

		/* Handles the controller condition. */
		if (controller->hardware_stop_in_progress) {
			generation = controller->hardware_stop_generation;

			/* Handles the controller condition. */
			if (controller->hardware_stop_waiters == (unsigned)-1) {
				spin_unlock_irqrestore(&controller->active_lock,
						       irq);

				/* Failed. */
				return EBUSY;
			}

			controller->hardware_stop_waiters++;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			break;
		}

		/*
		 * Do not overwrite a failed generation until every caller that
		 * joined it has consumed the exact published result.  Once they
		 * drain, a later independent caller may own a fresh retry.
		 */
		if (controller->hardware_stop_waiters == 0) {
			controller->hardware_stop_in_progress = 1;
			controller->hardware_stop_generation++;

			/* Handles the controller condition. */
			if (controller->hardware_stop_generation == 0)
				controller->hardware_stop_generation++;
			generation = controller->hardware_stop_generation;
			mmio_invalid = controller->fatal_mmio_invalid;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			goto stop_owner;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_HARDWARE_STOP_WAIT_TICKS)
			return EBUSY;
		sched_yield();
	}

	/*
	 * This caller joined an existing stop generation.  A fresh generation
	 * cannot be claimed until this waiter consumes its published result.
	 */
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&controller->active_lock);

		/* Handles the controller condition. */
		if (!controller->hardware_stop_in_progress &&
		    controller->hardware_stop_result_generation == generation) {
			/* Handles the controller condition. */
			if (controller->hardware_stop_waiters == 0)
				__builtin_trap();
			error = controller->hardware_stop_error;
			controller->hardware_stop_waiters--;
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Failed. */
			return error;
		}

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_HARDWARE_STOP_WAIT_TICKS) {
			/* Handles the controller condition. */
			if (controller->hardware_stop_waiters == 0)
				__builtin_trap();
			controller->hardware_stop_waiters--;
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Failed. */
			return EBUSY;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);
		sched_yield();
	}

stop_owner:

	/*
	 * This owner serializes every USBCMD stop transition with periodic
	 * pause/resume and fresh-IAA publication.  Polling is deliberately
	 * outside active_lock; fatal state prevents any later command restore.
	 */
	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the mmio invalid condition. */
	if (!mmio_invalid) {
		command = rd32(controller->operational, EHCI_USBCMD);
		kern_io_barrier();

		/* Handles the command condition. */
		if (command == UINT32_MAX) {
			mmio_invalid = 1;
		} else {
			wr32(controller->operational, EHCI_USBINTR, 0);
			wr32(controller->operational, EHCI_USBCMD,
			     command & ~(EHCI_CMD_RUN | EHCI_CMD_PERIODIC |
					 EHCI_CMD_ASYNC));
		}
	}

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the mmio invalid condition. */
	if (!mmio_invalid) {
		started = sched_ticks();
		/* Continue until the operation reaches a terminal state. */
		for (;;) {
			status = rd32(controller->operational, EHCI_USBSTS);
			kern_io_barrier();
			if (status == UINT32_MAX) {
				mmio_invalid = 1;
				break;
			}

			/* Checks the operation status. */
			if ((status & EHCI_STS_HALTED) != 0)
				break;

			/* Checks the sched ticks result. */
			if (sched_ticks() - started >= EHCI_QUIESCE_TICKS) {
				halt_error = ETIMEDOUT;
				break;
			}

			sched_yield();
		}
	}

	master_error = ehci_bus_master_disable(controller);
	irq_error = ehci_irq_disestablish(controller);

	/*
	 * An all-ones MMIO read leaves RUN state unknowable.  BME-off prevents
	 * further DMA, but treating that as a releasable stop would let
	 * PCI-state restoration re-enable BME after schedule memory was freed.
	 * Retain the graph and PCI lease permanently fail-closed instead.
	 */
	error = mmio_invalid	    ? EIO
		: halt_error != 0   ? halt_error
		: master_error != 0 ? master_error
				    : irq_error;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the mmio invalid condition. */
	if (mmio_invalid)
		controller->fatal_mmio_invalid = 1;
	controller->hardware_stop_error = error;
	controller->hardware_stopped = error == 0;
	controller->hardware_stop_result_generation = generation;
	controller->hardware_stop_in_progress = 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	if (error != 0) {
		kern_logf("ehci: %s hardware stop incomplete (halt=%d "
			   "master=%d irq=%d); ownership retained\n",
			   owner, halt_error, master_error, irq_error);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Stops the controller from mastering the bus. */
static int
ehci_bus_master_disable(
	struct ehci_controller *controller)
{
	uint16_t command;
	int error;

	/* Checks the operation status. */
	error = drv_pci_device_set_bus_master(controller->pci, false);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_pci_device_config_read16(controller->pci, EHCI_PCI_COMMAND,
					     &command);
	if (error != 0)
		return error;

	/* Returns the computed result. */
	return (command & EHCI_PCI_COMMAND_MASTER) == 0 ? 0 : EIO;
}

/* Takes the interrupt handler out of service. */
static int
ehci_irq_disestablish(
	struct ehci_controller *controller)
{
	uint64_t started;
	int error;

	/* Handles the irq cookie availability. */
	if (controller->irq_cookie == NULL)
		return 0;
	started = sched_ticks();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the operation status. */
		error = drv_pci_device_disestablish_irq_checked(
			controller->pci, controller->irq_cookie);
		if (error != EBUSY)
			break;

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS) {
			kern_logf("ehci: IRQ removal timed out; retaining "
				   "controller ownership\n");

			/* Failed. */
			return EBUSY;
		}

		sched_yield();
	}

	/* Checks the operation status. */
	if (error != 0) {
		kern_logf("ehci: checked IRQ removal failed (%d); retaining "
			   "controller ownership\n",
			   error);

		/* Failed. */
		return error;
	}

	controller->irq_cookie = NULL;

	/* Succeeded. */
	return 0;
}

/* Builds the token word one transfer descriptor carries. */
static uint32_t
ehci_qtd_token(
	unsigned pid,
	unsigned toggle,
	unsigned length)
{
	/* Returns the computed result. */
	return EHCI_QTD_ACTIVE | (3U << 10) | (pid << 8) |
	       ((uint32_t)length << 16) | ((uint32_t)toggle << 31);
}

/* Fills in the buffer pointers one transfer descriptor carries. */
static void
ehci_qtd_buffer(
	struct ehci_qtd *qtd,
	uint32_t address,
	size_t length)
{
	unsigned index;
	unsigned pages;

	qtd->buffer[0] = address;
	pages = (unsigned)(((address & 0xfffU) + length + 4095U) / 4096U);
	/* Process each remaining element. */
	for (index = 1; index < 5U && index < pages; index++) {
		qtd->buffer[index] =
			(address + (uint32_t)index * 4096U) & ~0xfffU;
	}
}

/* Appends one transfer descriptor to a request. */
static int
ehci_add_qtd(
	struct ehci_request *request,
	unsigned pid,
	unsigned toggle,
	unsigned length,
	uint32_t buffer)
{
	struct ehci_qtd *qtd;
	uint32_t physical;

	/* Handles the request condition. */
	if (request->qtd_count >= EHCI_MAX_QTDS)
		return E2BIG;
	qtd = &request->qtds[request->qtd_count];

	/* Handles the request condition. */
	physical = (uint32_t)request->schedule.device_address + 128U +
		   request->qtd_count * sizeof(*qtd);
	if (request->qtd_count != 0)
		request->qtds[request->qtd_count - 1U].next = physical;
	qtd->next = EHCI_LINK_TERM;
	qtd->alternate = EHCI_LINK_TERM;
	qtd->token = ehci_qtd_token(pid, toggle, length);
	ehci_qtd_buffer(qtd, buffer, length);
	request->requested[request->qtd_count] = (uint16_t)length;
	request->qtd_count++;

	/* Succeeded. */
	return 0;
}

/* Gives a request's descriptors and state back. */
static void
ehci_request_free(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct drv_dma_buffer schedule, bounce;
	unsigned long irq;

	/* Handles the request availability. */
	if (request == NULL)
		return;

	/* Handles the request condition. */
	if (request->periodic_reserved)
		__builtin_trap();

	/* Handles the request condition. */
	if (request->reclaim_reserved) {
		/* Handles the request condition. */
		if (request != &controller->reclaim_request)
			__builtin_trap();
		schedule = request->schedule;
		bounce = request->bounce;
		memset(request, 0, sizeof(*request));
		request->schedule = schedule;
		request->bounce = bounce;
		request->reclaim_reserved = true;

		/* Handles the controller condition. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (!controller->reclaim_request_busy) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			__builtin_trap();
		}

		controller->reclaim_request_busy = 0;
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Returns the computed result. */
		return;
	}

	/* Handles the address availability. */
	if (request->schedule.address != NULL)
		drv_dma_free_coherent(controller->hcd.dma, &request->schedule);

	/* Handles the address availability. */
	if (request->bounce.address != NULL)
		drv_dma_free_coherent(controller->hcd.dma, &request->bounce);
	kern_free(request);
}

/* Takes a request off the reclaim list to be reused. */
static int
ehci_reclaim_request_acquire(
	struct ehci_controller *controller,
	size_t length,
	struct ehci_request **result)
{
	struct drv_dma_buffer schedule, bounce;
	struct ehci_request *request = &controller->reclaim_request;
	unsigned long irq;
	int error = 0;

	*result = NULL;
	/* Checks the current data length. */
	if (length > DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE)
		return EMSGSIZE;
	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the address availability. */
	if (request->schedule.address == NULL ||
	    request->schedule.size < 4096U || request->bounce.address == NULL ||
	    request->bounce.size <
		    DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE +
			    sizeof(struct drv_usb_control_request))
		error = ENOMEM;
	else if (controller->reclaim_request_busy)
		error = EBUSY;
	else
		controller->reclaim_request_busy = 1U;
	schedule = request->schedule;
	bounce = request->bounce;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	memset(request, 0, sizeof(*request));
	request->schedule = schedule;
	request->bounce = bounce;
	request->reclaim_reserved = true;
	*result = request;
	/* Succeeded. */
	return 0;
}

/* Works out the interval and offset a periodic endpoint gets. */
static int
ehci_periodic_parameters(
	uint8_t interval,
	unsigned *period,
	uint32_t *service_mask,
	unsigned *microframe_slots)
{
	unsigned microframes;

	/* Handles the interval condition. */
	if (interval == 0)
		return EINVAL;

	/* The 1,024-frame list can represent at most 8,192 microframes. */
	if (interval > 14U)
		return ENOTSUP;

	/* Handles the microframes condition. */
	microframes = 1U << (interval - 1U);
	if (microframes == 1U) {
		*period = 1U;
		*service_mask = 0xffU;
		*microframe_slots = 1U;
	} else if (microframes == 2U) {
		*period = 1U;
		*service_mask = 0x55U;
		*microframe_slots = 2U;
	} else if (microframes == 4U) {
		*period = 1U;
		*service_mask = 0x11U;
		*microframe_slots = 4U;
	} else {
		*period = microframes / 8U;
		*service_mask = 0x01U;
		*microframe_slots = 8U;
	}

	/* Succeeded. */
	return 0;
}

/* Builds the descriptors one transfer needs. */
static int
ehci_build_request(
	struct ehci_controller *controller,
	struct drv_usb_urb *urb,
	struct ehci_request **result)
{
	unsigned chunk_local;
	unsigned chunk_local1;
	unsigned payload;
	struct ehci_request *request;
	struct drv_usb_endpoint *endpoint = drv_usb_urb_endpoint(urb);
	struct drv_usb_device *device = drv_usb_urb_device(urb);
	const struct drv_usb_endpoint_descriptor *descriptor;
	const struct drv_usb_control_request *control;
	enum drv_usb_transfer_type type;
	size_t length = drv_usb_urb_length(urb);
	size_t offset = 0;
	uint32_t service_mask = 0;
	unsigned packet_raw;
	unsigned packet;
	unsigned mult;
	unsigned required_qtds;
	unsigned microframe_slots = 0;
	size_t data_qtds;
	unsigned address;
	unsigned endpoint_number;
	unsigned toggle = 0;
	unsigned initial_toggle = 0;
	unsigned zero_packet = 0;
	int error;

	/* Handles the endpoint availability. */
	if (endpoint == NULL || device == NULL || result == NULL)
		return EINVAL;

	/* Checks the drv usb device speed result. */
	if (drv_usb_device_speed(device) != DRV_USB_SPEED_HIGH)
		return ENOTSUP;

	/* Handles the type condition. */
	type = drv_usb_endpoint_type(endpoint);
	if (type == DRV_USB_TRANSFER_ISOCHRONOUS)
		return ENOTSUP;

	/* Handles the type condition. */
	if (type != DRV_USB_TRANSFER_CONTROL && type != DRV_USB_TRANSFER_BULK &&
	    type != DRV_USB_TRANSFER_INTERRUPT) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the control availability. */
	control = drv_usb_urb_control_request(urb);
	if ((control != NULL) != (type == DRV_USB_TRANSFER_CONTROL))
		return EINVAL;

	/* Handles the descriptor availability. */
	descriptor = drv_usb_endpoint_descriptor(endpoint);
	if (descriptor == NULL || length > SIZE_MAX - 8U)
		return EINVAL;
	endpoint_number = drv_usb_endpoint_address(endpoint) & 15U;

	/* Handles the type condition. */
	if (type == DRV_USB_TRANSFER_INTERRUPT) {
		/* Checks the operation status. */
		error = ehci_periodic_parameters(descriptor->interval, &toggle,
						 &service_mask,
						 &microframe_slots);
		if (error != 0)
			return error;
	}

	packet_raw = drv_usb_endpoint_max_packet_size(endpoint);
	packet = packet_raw & 0x7ffU;
	mult = ((packet_raw >> 11) & 3U) + 1U;

	/*
	 * USB 2.0 high-speed endpoint rules.  Validate the complete encoded
	 * wMaxPacketSize before allocating DMA: control and bulk cannot request
	 * high-bandwidth transactions, while interrupt may encode at most three
	 * transactions and a 1,024-byte payload.
	 */
	if ((packet_raw & 0xe000U) != 0 || packet == 0 || mult > 3U)
		return EINVAL;

	/* Handles the type condition. */
	if ((type == DRV_USB_TRANSFER_CONTROL &&
	     (endpoint_number != 0 ||
	      (packet_raw != 8U && packet_raw != 64U))) ||
	    (type == DRV_USB_TRANSFER_BULK && packet_raw != 512U) ||
	    (type == DRV_USB_TRANSFER_INTERRUPT && packet > 1024U)) {
		/* Failed. */
		return EINVAL;
	}

	/*
	 * The common USB core starts endpoint zero at eight bytes until the
	 * first descriptor is read.  A high-speed control QH nevertheless
	 * always uses the architected 64-byte endpoint-zero packet size.
	 */
	if (type == DRV_USB_TRANSFER_CONTROL) {
		packet = 64U;
		mult = 1U;
	}

	zero_packet = control == NULL && type == DRV_USB_TRANSFER_BULK &&
		      !drv_usb_endpoint_is_input(endpoint) && length != 0 &&
		      (drv_usb_urb_flags(urb) & DRV_USB_URB_ZERO_PACKET) != 0 &&
		      length % packet == 0;

	/* Handles the type condition. */
	data_qtds = length / 0x4000U + (length % 0x4000U != 0);
	if (type == DRV_USB_TRANSFER_CONTROL) {
		/* Handles the data qtds condition. */
		if (data_qtds > EHCI_MAX_QTDS - 2U)
			return E2BIG;
		required_qtds = (unsigned)data_qtds + 2U;
	} else {
		/* Handles the data qtds condition. */
		if (data_qtds > EHCI_MAX_QTDS - zero_packet)
			return E2BIG;
		required_qtds =
			data_qtds == 0 ? 1U : (unsigned)data_qtds + zero_packet;
	}

	/* Handles the required qtds condition. */
	if (required_qtds > EHCI_MAX_QTDS)
		return E2BIG;

	/* Checks the drv usb urb flags result. */
	if ((drv_usb_urb_flags(urb) & DRV_USB_URB_RECLAIM_SAFE) != 0) {
		/* Checks the operation status. */
		error = ehci_reclaim_request_acquire(controller, length,
						     &request);
		if (error != 0)
			return error;
	} else {
		/* Handles the request availability. */
		request = kern_malloc(sizeof(*request));
		if (request == NULL)
			return ENOMEM;
		memset(request, 0, sizeof(*request));
	}

	request->urb = urb;
	request->endpoint = endpoint;
	request->state = EHCI_REQUEST_ACTIVE;
	request->completion_status = DRV_USB_URB_COMPLETE;
	request->schedule_class = type == DRV_USB_TRANSFER_INTERRUPT
					  ? EHCI_SCHEDULE_PERIODIC
					  : EHCI_SCHEDULE_ASYNC;

	/* Handles the request condition. */
	if (request->schedule_class == EHCI_SCHEDULE_PERIODIC)
		request->periodic_period = toggle;
	request->periodic_microframe_slots = microframe_slots;

	/* Handles the request condition. */
	if (request->schedule_class == EHCI_SCHEDULE_PERIODIC) {
		payload = packet * mult;

		request->periodic_cost = mult * EHCI_PERIODIC_TRANSACTION_BITS +
					 (payload * 56U + 5U) / 6U;
	}

	/* Handles the request condition. */
	if (!request->reclaim_reserved) {
		/* Checks the operation status. */
		error = drv_dma_alloc_coherent(controller->hcd.dma, 4096U, 64U,
					       &request->schedule);
		if (error != 0) {
			kern_free(request);

			/* Failed. */
			return error;
		}

		/* Checks the operation status. */
		error = drv_dma_alloc_coherent(
			controller->hcd.dma,
			length + sizeof(struct drv_usb_control_request), 64U,
			&request->bounce);
		if (error != 0)
			goto fail;
	}

	/* Checks the ehci dma buffer is 32bit result. */
	if (!ehci_dma_buffer_is_32bit(&request->schedule) ||
	    !ehci_dma_buffer_is_32bit(&request->bounce)) {
		error = EOVERFLOW;
		goto fail;
	}

	memset(request->schedule.address, 0, 4096U);
	request->qh = request->schedule.address;
	request->qtds =
		(struct ehci_qtd *)((uint8_t *)request->schedule.address +
				    128U);
	address = drv_usb_device_address(device);
	request->control = control != NULL;

	/* Handles the control availability. */
	if (control != NULL) {
		memcpy(request->bounce.address, control, sizeof(*control));

		/* Checks the operation status. */
		error = ehci_add_qtd(request, EHCI_PID_SETUP, 0, 8U,
				     (uint32_t)request->bounce.device_address);
		if (error != 0)
			goto fail;
		request->input = (control->request_type & DRV_USB_DIR_IN) != 0;

		/* Handles the request condition. */
		if (!request->input && length != 0) {
			memcpy((uint8_t *)request->bounce.address + 8U,
			       drv_usb_urb_buffer(urb), length);
		}

		request->data_first = request->qtd_count;
		toggle = 1U;
		/* Process each remaining element. */
		while (offset < length) {
			chunk_local = length - offset > 0x4000U
					      ? 0x4000U
					      : (unsigned)(length - offset);

			error = ehci_add_qtd(
				request,
				request->input ? EHCI_PID_IN : EHCI_PID_OUT,

				/* Queues one chunk of the data stage. */
				toggle, chunk_local,
				(uint32_t)(request->bounce.device_address + 8U +
					   offset));

			/* Checks the operation status. */
			if (error != 0)
				goto fail;
			offset += chunk_local;

			/*
			 * DTC is set for control QHs.  The following qTD starts
			 * with the toggle after every packet in this qTD, not
			 * after one descriptor.
			 */
			toggle ^= ((chunk_local + packet - 1U) / packet) & 1U;
			request->data_count++;
		}

		/* Checks the operation status. */
		error = ehci_add_qtd(
			request, request->input ? EHCI_PID_OUT : EHCI_PID_IN,
			1U, 0, 0);
		if (error != 0)
			goto fail;
	} else {
		request->input = drv_usb_endpoint_is_input(endpoint);
		initial_toggle =
			(unsigned)drv_usb_endpoint_hcd_data(endpoint, 0) & 1U;
		toggle = initial_toggle;

		/* Handles the request condition. */
		if (!request->input && length != 0) {
			memcpy((uint8_t *)request->bounce.address + 8U,
			       drv_usb_urb_buffer(urb), length);
		}

		/* Process each remaining element. */
		while (offset < length) {
			chunk_local1 = length - offset > 0x4000U
					       ? 0x4000U
					       : (unsigned)(length - offset);

			error = ehci_add_qtd(
				request,
				request->input ? EHCI_PID_IN : EHCI_PID_OUT,

				/* Queues one chunk of the data stage. */
				toggle, chunk_local1,
				(uint32_t)(request->bounce.device_address + 8U +
					   offset));

			/* Checks the operation status. */
			if (error != 0)
				goto fail;
			offset += chunk_local1;

			/*
			 * The next qTD starts with the toggle after every
			 * packet in this qTD.  This matters when a terminating
			 * zero-length packet follows an exact packet multiple.
			 */
			toggle ^= ((chunk_local1 + packet - 1U) / packet) & 1U;
			request->data_count++;
		}

		/* Checks the current data length. */
		if (length == 0 || zero_packet) {
			/* Checks the operation status. */
			error = ehci_add_qtd(request,
					     request->input ? EHCI_PID_IN
							    : EHCI_PID_OUT,
					     toggle, 0, 0);
			if (error != 0)
				goto fail;
		}

		request->data_count = request->qtd_count;
	}

	/* Handles the request condition. */
	if (request->qtd_count != required_qtds)
		__builtin_trap();
	request->qtds[request->qtd_count - 1U].token |= EHCI_QTD_IOC;
	request->qh->horizontal = EHCI_LINK_TERM;
	request->qh->characteristics =
		(address & 0x7fU) | ((endpoint_number & 15U) << 8) |
		(2U << 12) | (request->control ? (1U << 14) : 0U) |
		((packet & 0x7ffU) << 16) |
		(request->schedule_class == EHCI_SCHEDULE_ASYNC ? (4U << 28)
								: 0U);
	request->qh->capabilities =
		request->schedule_class == EHCI_SCHEDULE_PERIODIC
			? service_mask | (mult << 30)
			: (1U << 30);
	request->qh->next = (uint32_t)request->schedule.device_address + 128U;
	request->qh->alternate = EHCI_LINK_TERM;
	request->qh->token =
		request->control ? 0U : ((uint32_t)initial_toggle << 31);
	*result = request;
	/* Succeeded. */
	return 0;

fail:
	ehci_request_free(controller, request);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Finds the endpoint a request belongs to. */
static struct ehci_request *
ehci_endpoint_owner_locked(
	struct ehci_controller *controller,
	struct drv_usb_endpoint *endpoint)
{
	struct ehci_request *request;

	/* Process each element required by the operation. */
	for (request = controller->active; request != NULL;
	     request = request->active_next) {
		/* Handles the request condition. */
		if (request->endpoint == endpoint)
			return request;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Puts a request on the list of those the controller is running. */
static void
ehci_active_insert_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	request->active_next = controller->active;
	controller->active = request;
	controller->active_count++;
}

/* Takes it back off that list. */
static void
ehci_active_remove_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct ehci_request **link;

	/* Process each element required by the operation. */
	for (link = &controller->active; *link != NULL;
	     link = &(*link)->active_next) {
		/* Handles the link condition. */
		if (*link != request)
			continue;
		*link = request->active_next;
		request->active_next = NULL;

		/* Handles the controller condition. */
		if (controller->active_count == 0)
			__builtin_trap();
		controller->active_count--;

		/* Returns the computed result. */
		return;
	}

	__builtin_trap();
}

/* Links a request into the asynchronous schedule. */
static void
ehci_async_insert_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	request->schedule_previous = controller->async_last;
	request->schedule_next = NULL;
	request->qh->horizontal =
		(uint32_t)controller->async_head_memory.device_address |
		EHCI_LINK_QH;

	/* Handles the async last availability. */
	if (controller->async_last != NULL) {
		controller->async_last->schedule_next = request;

		/*
		 * The controller-visible predecessor link is the publication
		 * store. Make the complete private QH/qTD graph and its tail
		 * link visible before hardware can follow that store.
		 */
		kern_io_write_barrier();
		controller->async_last->qh->horizontal =
			ehci_request_link(request);
	} else {
		controller->async_first = request;
		kern_io_write_barrier();
		controller->async_head->horizontal = ehci_request_link(request);
	}

	controller->async_last = request;
	request->linked = true;
}

/* Unlinks it from that schedule. */
static int
ehci_async_unlink_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct ehci_request *previous = request->schedule_previous;
	struct ehci_request *next = request->schedule_next;
	uint32_t next_link = next != NULL
				     ? ehci_request_link(next)
				     : (uint32_t)controller->async_head_memory
						       .device_address |
					       EHCI_LINK_QH;

	/* Handles the request condition. */
	if (!request->linked || request->schedule_class != EHCI_SCHEDULE_ASYNC)
		return EINVAL;

	/* Handles the previous availability. */
	if (previous != NULL) {
		/* Handles the previous condition. */
		if (previous->schedule_next != request)
			return EIO;
		previous->schedule_next = next;
		previous->qh->horizontal = next_link;
	} else {
		/* Handles the controller condition. */
		if (controller->async_first != request)
			return EIO;
		controller->async_first = next;
		controller->async_head->horizontal = next_link;
	}

	/* Handles the next availability. */
	if (next != NULL) {
		next->schedule_previous = previous;
	} else {
		/* Handles the controller condition. */
		if (controller->async_last != request)
			return EIO;
		controller->async_last = previous;
	}

	request->schedule_previous = NULL;
	request->schedule_next = NULL;
	request->linked = false;

	/* Succeeded. */
	return 0;
}

/* Finds where in the periodic schedule a request links in. */
static uint32_t
ehci_periodic_parent_link(
	struct ehci_controller *controller,
	unsigned node)
{
	uint32_t function_result;

	/* Computes the function result. */
	function_result =
		node == 0 ? EHCI_LINK_TERM
			  : ehci_skeleton_link(controller, (node - 1U) / 2U);

	/* Returns the computed result. */
	return function_result;
}

/* Links a request into the periodic schedule. */
static void
ehci_periodic_insert_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct ehci_request *first =
		controller->periodic_heads[request->periodic_node];

	/* Links the request in front of the list it joins. */
	request->schedule_previous = NULL;
	request->schedule_next = first;
	request->qh->horizontal =
		first != NULL ? ehci_request_link(first)
			      : ehci_periodic_parent_link(
					controller, request->periodic_node);

	/* Handles the first availability. */
	if (first != NULL)
		first->schedule_previous = request;
	controller->periodic_heads[request->periodic_node] = request;

	/*
	 * Although PSS is stopped for this update, retain the same publication
	 * contract: initialize the private graph, order it, then expose its
	 * link from the controller-owned skeleton.
	 */
	kern_io_write_barrier();
	controller->periodic_skeleton[request->periodic_node].horizontal =
		ehci_request_link(request);

	/*
	 * Order the publication store before the later PSE resume MMIO write.
	 */
	kern_io_write_barrier();
	request->linked = true;
}

/* Unlinks it from that schedule. */
static int
ehci_periodic_unlink_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct ehci_request *previous = request->schedule_previous;
	struct ehci_request *next = request->schedule_next;
	uint32_t next_link =
		next != NULL ? ehci_request_link(next)
			     : ehci_periodic_parent_link(
				       controller, request->periodic_node);

	/* Handles the request condition. */
	if (!request->linked ||
	    request->schedule_class != EHCI_SCHEDULE_PERIODIC ||
	    request->periodic_node >= EHCI_PERIODIC_NODES) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the previous availability. */
	if (previous != NULL) {
		/* Handles the previous condition. */
		if (previous->schedule_next != request)
			return EIO;
		previous->schedule_next = next;
		previous->qh->horizontal = next_link;
	} else {
		/* Handles the controller condition. */
		if (controller->periodic_heads[request->periodic_node] !=
		    request) {
			/* Failed. */
			return EIO;
		}
		controller->periodic_heads[request->periodic_node] = next;
		controller->periodic_skeleton[request->periodic_node]
			.horizontal = next_link;
	}

	/* Handles the next availability. */
	if (next != NULL)
		next->schedule_previous = previous;
	request->schedule_previous = NULL;
	request->schedule_next = NULL;
	ehci_periodic_release_locked(controller, request);
	request->linked = false;

	/* Succeeded. */
	return 0;
}

/* Leaves the gate that serializes request building. */
static void
ehci_builder_leave(
	struct ehci_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the controller condition. */
	if (controller->builders == 0)
		__builtin_trap();
	controller->builders--;

	spin_unlock_irqrestore(&controller->active_lock, irq);
}

/* Discards a request that was never linked into a schedule. */
static void
ehci_unpublished_request_discard(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	/*
	 * builders is the lifetime pin for both the controller-owned reclaim
	 * reserve and an ordinary request's DMA allocator.  Drop it only after
	 * the private request has been returned; quiesce may release the whole
	 * schedule as soon as the counter reaches zero.
	 */
	ehci_request_free(controller, request);
	ehci_builder_leave(controller);
}

/* Takes the right to change the periodic schedule. */
static int
ehci_periodic_update_acquire(
	struct ehci_controller *controller,
	int retirement)
{
	unsigned long irq;
	uint64_t started = sched_ticks();

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the retirement condition. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (!retirement &&
		    (controller->quiescing || controller->dma_quiesced ||
		     controller->quarantined)) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Failed. */
			return ENODEV;
		}

		/*
		 * USBCMD.IAAD must not be replayed by a periodic pause/resume
		 * read-modify-write.  The fresh-IAA owner therefore excludes
		 * the whole PSS transaction, and the PSS owner excludes a new
		 * doorbell above.
		 */
		if (!controller->periodic_updating &&
		    controller->iaa_owner == NULL) {
			controller->periodic_updating = 1;
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Succeeded. */
			return 0;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS)
			return EBUSY;
		sched_yield();
	}
}

/* Gives that right back. */
static void
ehci_periodic_update_release(
	struct ehci_controller *controller)
{
	unsigned long irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the controller condition. */
	if (!controller->periodic_updating)
		__builtin_trap();
	controller->periodic_updating = 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);
}

/* Stops the controller walking the periodic schedule. */
static int
ehci_periodic_pause(
	struct ehci_controller *controller)
{
	int error;
	uint32_t command;
	uint32_t status;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the atomic load n result. */
	if (controller->quarantined || controller->fatal_mmio_invalid ||
	    __atomic_load_n(&controller->fatal_pending, __ATOMIC_ACQUIRE) !=
		    0 ||
	    __atomic_load_n(&controller->fatal_stopping, __ATOMIC_ACQUIRE) !=
		    0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return ENODEV;
	}

	command = rd32(controller->operational, EHCI_USBCMD);
	status = rd32(controller->operational, EHCI_USBSTS);
	kern_io_barrier();

	/* Handles the command condition. */
	if (command == UINT32_MAX || status == UINT32_MAX ||
	    (status & (EHCI_STS_HSE | EHCI_STS_HALTED)) != 0 ||
	    (command & (EHCI_CMD_RUN | EHCI_CMD_PERIODIC)) !=
		    (EHCI_CMD_RUN | EHCI_CMD_PERIODIC) ||
	    (status & EHCI_STS_PERIODIC) == 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EIO;
	}

	/* Keep RUN and ASE intact so bulk/control traffic continues. */
	wr32(controller->operational, EHCI_USBCMD,
	     command & ~EHCI_CMD_PERIODIC);

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Obtains the ehci wait schedule status result. */
	error = ehci_wait_schedule_status(
		controller, 0, EHCI_STS_PERIODIC | EHCI_STS_HALTED);

	/* Returns the computed result. */
	return error;
}

/* Lets it walk that schedule again. */
static int
ehci_periodic_resume(
	struct ehci_controller *controller)
{
	int error;
	uint32_t command;
	uint32_t status;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the atomic load n result. */
	if (controller->quarantined || controller->fatal_mmio_invalid ||
	    __atomic_load_n(&controller->fatal_pending, __ATOMIC_ACQUIRE) !=
		    0 ||
	    __atomic_load_n(&controller->fatal_stopping, __ATOMIC_ACQUIRE) !=
		    0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return ENODEV;
	}

	command = rd32(controller->operational, EHCI_USBCMD);
	status = rd32(controller->operational, EHCI_USBSTS);
	kern_io_barrier();

	/* Handles the command condition. */
	if (command == UINT32_MAX || status == UINT32_MAX ||
	    (status & (EHCI_STS_HSE | EHCI_STS_HALTED)) != 0 ||
	    (command & (EHCI_CMD_RUN | EHCI_CMD_ASYNC)) !=
		    (EHCI_CMD_RUN | EHCI_CMD_ASYNC)) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EIO;
	}

	wr32(controller->operational, EHCI_USBCMD, command | EHCI_CMD_PERIODIC);

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Obtains the ehci wait schedule status result. */
	error = ehci_wait_schedule_status(
		controller, EHCI_STS_PERIODIC, EHCI_STS_HALTED);

	/* Returns the computed result. */
	return error;
}

/* Publishes a request into the asynchronous schedule. */
static int
ehci_publish_async_request(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	unsigned long irq;
	int error;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&controller->active_lock);

		/* Checks the atomic load n result. */
		if (controller->quiescing || controller->dma_quiesced ||
		    controller->quarantined ||
		    controller->retirement_worker == NULL ||
		    controller->retirement_joining ||
		    __atomic_load_n(&controller->retirement_stopping,
				    __ATOMIC_ACQUIRE) != 0) {
			error = ENODEV;
		} else if (ehci_endpoint_owner_locked(
				   controller, request->endpoint) != NULL ||
			   drv_usb_endpoint_hcd_data(
				   request->endpoint,
				   EHCI_ENDPOINT_STALL_PUBLISHING_SLOT) != 0) {
			error = EBUSY;
		} else if (controller->iaa_owner != NULL) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			sched_yield();
			continue;
		} else {
			ehci_active_insert_locked(controller, request);
			(void)drv_usb_urb_set_hcd_data(request->urb, request);
			ehci_async_insert_locked(controller, request);

			/* Handles the controller condition. */
			if (controller->builders == 0)
				__builtin_trap();
			controller->builders--;
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Succeeded. */
			return 0;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);
		ehci_unpublished_request_discard(controller, request);

		/* Failed. */
		return error;
	}
}

/* Publishes a request into the periodic schedule. */
static int
ehci_publish_periodic_request(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	unsigned microframe_phase;
	uint8_t service_mask;
	unsigned frame_offset, frame_start;
	unsigned microframe_index, microframe_offset, microframe_start;
	unsigned slots;
	uint32_t base_service_mask;
	unsigned level = 0;
	unsigned long irq;
	unsigned accepted = 0;
	int error;
	int resume_error;

	/* Checks the operation status. */
	error = ehci_periodic_update_acquire(controller, 0);
	if (error != 0) {
		ehci_unpublished_request_discard(controller, request);

		/* Failed. */
		return error;
	}

	/* Checks the operation status. */
	error = ehci_periodic_pause(controller);
	if (error != 0) {
		irq = spin_lock_irqsave(&controller->active_lock);
		ehci_controller_fail_locked(controller, error,
					    "periodic publication pause");
		spin_unlock_irqrestore(&controller->active_lock, irq);
		ehci_periodic_update_release(controller);
		ehci_retirement_worker_wakeup(controller);
		ehci_unpublished_request_discard(controller, request);

		/* Failed. */
		return error;
	}

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the atomic load n result. */
	if (controller->quiescing || controller->dma_quiesced ||
	    controller->quarantined || controller->retirement_worker == NULL ||
	    controller->retirement_joining ||
	    __atomic_load_n(&controller->retirement_stopping,
			    __ATOMIC_ACQUIRE) != 0)
		error = ENODEV;
	else if (ehci_endpoint_owner_locked(controller, request->endpoint) !=
			 NULL ||
		 drv_usb_endpoint_hcd_data(
			 request->endpoint,
			 EHCI_ENDPOINT_STALL_PUBLISHING_SLOT) != 0) {
		error = EBUSY;
	} else {
		microframe_index = 0;
		microframe_start = 0;
		slots = request->periodic_microframe_slots;
		base_service_mask = request->qh->capabilities & 0xffU;

		/* Continue while the operation condition remains true. */
		while ((1U << level) < request->periodic_period)
			level++;

		/* Handles the slots condition. */
		if (slots > 1U) {
			/* Handles the slots condition. */
			if (slots == 2U)
				microframe_index = 0U;
			else if (slots == 4U)
				microframe_index = 1U;
			else
				microframe_index = 2U;
			microframe_start =
				controller->periodic_microframe_phase_next
					[microframe_index] &
				(slots - 1U);
		}

		frame_start = controller->periodic_phase_next[level] &
			      (request->periodic_period - 1U);
		error = ENOSPC;
		/* Process each element required by the operation. */
		for (frame_offset = 0;
		     frame_offset < request->periodic_period && !accepted;
		     frame_offset++) {
			request->periodic_phase =
				(frame_start + frame_offset) &
				(request->periodic_period - 1U);
			request->periodic_node =
				(1U << level) - 1U +
				ehci_reverse_bits(request->periodic_phase,
						  level);
			/* Process each element required by the operation. */
			for (microframe_offset = 0; microframe_offset < slots;
			     microframe_offset++) {
				microframe_phase =
					(microframe_start + microframe_offset) &
					(slots - 1U);
				service_mask = (uint8_t)(base_service_mask
							 << microframe_phase);

				/* Checks the operation status. */
				error = ehci_periodic_reserve_locked(
					controller, request, service_mask);
				if (error == 0) {
					request->qh->capabilities =
						(request->qh->capabilities &
						 ~0xffU) |
						service_mask;
					controller->periodic_phase_next[level] =
						(request->periodic_phase + 1U) &
						(request->periodic_period - 1U);

					/* Handles the slots condition. */
					if (slots > 1U) {
						controller->periodic_microframe_phase_next
							[microframe_index] =
							(microframe_phase +
							 1U) &
							(slots - 1U);
					}

					accepted = 1;
					break;
				}

				/* Checks the operation status. */
				if (error != ENOSPC)
					break;
			}

			/* Checks the operation status. */
			if (error != 0 && error != ENOSPC)
				break;
		}

		/* Handles the accepted condition. */
		if (accepted) {
			ehci_active_insert_locked(controller, request);
			(void)drv_usb_urb_set_hcd_data(request->urb, request);
			ehci_periodic_insert_locked(controller, request);
			error = 0;
		}
	}

	spin_unlock_irqrestore(&controller->active_lock, irq);

	resume_error = ehci_periodic_resume(controller);
	ehci_periodic_update_release(controller);

	/* Handles the accepted condition. */
	if (accepted)
		ehci_builder_leave(controller);

	/* Checks the operation status. */
	if (resume_error != 0) {
		irq = spin_lock_irqsave(&controller->active_lock);
		ehci_controller_fail_locked(controller, resume_error,
					    "periodic publication resume");
		spin_unlock_irqrestore(&controller->active_lock, irq);
		ehci_retirement_worker_wakeup(controller);

		/* Handles the accepted condition. */
		if (accepted)
			return 0;
		ehci_unpublished_request_discard(controller, request);

		/* Returns the computed result. */
		return resume_error;
	}

	/* Handles the accepted condition. */
	if (!accepted) {
		ehci_unpublished_request_discard(controller, request);

		/* Failed. */
		return error;
	}

	/* Succeeded. */
	return 0;
}

/* Puts one transfer into the schedule of its endpoint. */
static int
ehci_urb_enqueue(
	struct drv_usb_hcd *hcd,
	struct drv_usb_urb *urb)
{
	int function_result;
	struct ehci_controller *controller = hcd_controller(hcd);
	struct ehci_request *request;
	struct drv_usb_endpoint *endpoint = drv_usb_urb_endpoint(urb);
	unsigned long irq;
	int error;

	/* Handles the endpoint availability. */
	if (endpoint == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the atomic load n result. */
	if (controller->quiescing || controller->dma_quiesced ||
	    controller->quarantined || controller->retirement_worker == NULL ||
	    controller->retirement_joining ||
	    __atomic_load_n(&controller->retirement_stopping,
			    __ATOMIC_ACQUIRE) != 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return ENODEV;
	}

	/* Checks the ehci endpoint owner locked result. */
	if (ehci_endpoint_owner_locked(controller, endpoint) != NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	/* Checks the drv usb endpoint hcd data result. */
	if (drv_usb_endpoint_hcd_data(
		    endpoint, EHCI_ENDPOINT_STALL_PUBLISHING_SLOT) != 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	controller->builders++;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	error = ehci_build_request(controller, urb, &request);
	if (error != 0) {
		ehci_builder_leave(controller);

		/* Failed. */
		return error;
	}

	/* Handles the request condition. */
	if (request->schedule_class == EHCI_SCHEDULE_PERIODIC) {
		/* Obtains the ehci publish periodic request result. */
		function_result =
			ehci_publish_periodic_request(controller, request);

		/* Returns the computed result. */
		return function_result;
	}

	/* Obtains the ehci publish async request result. */
	function_result = ehci_publish_async_request(controller, request);

	/* Returns the computed result. */
	return function_result;
}

/* Asks whether the controller is still running a request. */
static int
ehci_request_is_active_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct ehci_request *cursor;

	/* Process each element required by the operation. */
	for (cursor = controller->active; cursor != NULL;
	     cursor = cursor->active_next) {
		/* Checks the current cursor position. */
		if (cursor == request)
			return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Puts a request on the list of those waiting to be retired. */
static void
ehci_retirement_enqueue_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	/* Handles the request condition. */
	if (request->retirement_queued)
		__builtin_trap();
	request->retirement_next = NULL;

	/* Handles the retirement tail availability. */
	if (controller->retirement_tail != NULL)
		controller->retirement_tail->retirement_next = request;
	else
		controller->retirement_head = request;
	controller->retirement_tail = request;
	request->retirement_queued = true;
}

/* Takes the next request off that list. */
static void
ehci_retirement_pop_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	/* Handles the controller condition. */
	if (controller->retirement_head != request ||
	    !request->retirement_queued)
		__builtin_trap();
	controller->retirement_head = request->retirement_next;

	/* Handles the retirement head availability. */
	if (controller->retirement_head == NULL)
		controller->retirement_tail = NULL;
	request->retirement_next = NULL;
	request->retirement_queued = false;
}

/* Starts retiring one request. */
static void
ehci_retirement_begin_locked(
	struct ehci_controller *controller,
	struct ehci_request *request,
	enum ehci_retirement_reason reason,
	enum drv_usb_urb_status status)
{
	/* Handles the request condition. */
	if (request->state != EHCI_REQUEST_ACTIVE || request->retirement_queued)
		__builtin_trap();
	request->state = EHCI_REQUEST_DEACTIVATING;
	request->retirement_reason = reason;
	request->completion_status = status;

	/*
	 * Queue residence is not part of this request's hardware-barrier
	 * timeout.  The head request starts a fresh timer when it first owns
	 * the unlink barrier, and an async request starts another generation
	 * when its own IAAD is issued.
	 */
	request->retirement_started = 0;

	/*
	 * The schedule still owns its qTDs here.  Do not manufacture completion
	 * by clearing ACTIVE in controller-visible descriptors.  Async requests
	 * become unreachable before the fresh IAA boundary; periodic requests
	 * become unreachable while PSS is checked clear.
	 */
	ehci_retirement_enqueue_locked(controller, request);
}

/* Reports the status the last descriptor of a request holds. */
static int
ehci_request_terminal(
	struct ehci_request *request,
	enum drv_usb_urb_status *result)
{
	uint32_t token;
	uint32_t remaining;
	unsigned index;
	int all_inactive = 1;
	int short_packet = 0;

	kern_io_read_barrier();
	/* Process each remaining element. */
	for (index = 0; index < request->qtd_count; index++) {
		token = request->qtds[index].token;
		remaining = (token >> 16) & 0x7fffU;

		/* Checks the operation status. */
		if ((token & EHCI_QTD_ERRORS) != 0) {
			*result = (token & EHCI_QTD_ERRORS) == EHCI_QTD_HALTED
					  ? DRV_USB_URB_STALL
					  : DRV_USB_URB_IO_ERROR;

			/* Reports operation failure. */
			return 1;
		}

		/* Handles the token condition. */
		if ((token & EHCI_QTD_ACTIVE) != 0)
			all_inactive = 0;
		else if (request->input && index >= request->data_first &&
			 index < request->data_first + request->data_count &&
			 remaining != 0)
			short_packet = 1;
	}

	/* Checks the operation status. */
	if ((request->qh->token & EHCI_QTD_ERRORS) != 0) {
		*result = (request->qh->token & EHCI_QTD_ERRORS) ==
					  EHCI_QTD_HALTED
				  ? DRV_USB_URB_STALL
				  : DRV_USB_URB_IO_ERROR;

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the all inactive condition. */
	if (all_inactive ||
	    (short_packet && (request->qh->token & EHCI_QTD_ACTIVE) == 0)) {
		*result = DRV_USB_URB_COMPLETE;
		/* Reports operation failure. */
		return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Reports how many bytes a request actually moved. */
static size_t
ehci_request_actual(
	struct ehci_request *request)
{
	unsigned qtd_index;
	uint32_t token;
	uint32_t remaining;
	size_t transferred;
	size_t actual = 0;
	size_t length = drv_usb_urb_length(request->urb);
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < request->data_count; index++) {
		/* Handles the qtd index condition. */
		qtd_index = request->data_first + index;
		if (qtd_index >= request->qtd_count)
			break;

		/* Handles the token condition. */
		token = request->qtds[qtd_index].token;
		if ((token & EHCI_QTD_ACTIVE) != 0)
			break;

		/* Handles the remaining condition. */
		remaining = (token >> 16) & 0x7fffU;
		if (remaining > request->requested[qtd_index])
			break;

		/* Handles the actual condition. */
		transferred = request->requested[qtd_index] - remaining;
		if (actual > length || transferred > length - actual)
			break;
		actual += transferred;

		/* Checks the operation status. */
		if (remaining != 0 || (token & EHCI_QTD_ERRORS) != 0)
			break;
	}

	/* Returns the computed result. */
	return actual;
}

/* Saves the data toggle a request leaves its endpoint at. */
static void
ehci_request_commit_toggle(
	struct ehci_request *request)
{
	/* Handles the request condition. */
	if (!request->control) {
		(void)drv_usb_endpoint_set_hcd_data(
			request->endpoint, 0, (request->qh->token >> 31) & 1U);
	}
}

/* Hands a retired request back to its caller. */
static void
ehci_retirement_report(
	struct ehci_controller *controller)
{
	struct ehci_request *request;
	const char *stage;
	unsigned long irq;
	int error;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		stage = NULL;

		error = 0;

		irq = spin_lock_irqsave(&controller->active_lock);
		/* Process each element required by the operation. */
		for (request = controller->active; request != NULL;
		     request = request->active_next) {
			/* Checks the operation status. */
			if (request->state != EHCI_REQUEST_FAILED ||
			    request->failure_reported)
				continue;
			request->failure_reported = 1;
			stage = request->failure_stage;
			error = request->failure_error;
			break;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Handles the stage availability. */
		if (stage == NULL)
			return;
		kern_logf("ehci: request retirement failed at %s (%d); "
			   "request and DMA retained\n",
			   stage, error);
	}
}

/* Asks the controller to acknowledge an unlink. */
static int
ehci_retirement_begin_iaa_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	uint32_t command;
	uint32_t status;
	int error;

	/* Handles the iaa owner availability. */
	if (request->state != EHCI_REQUEST_DEACTIVATING ||
	    request->schedule_class != EHCI_SCHEDULE_ASYNC ||
	    controller->retirement_head != request ||
	    controller->iaa_owner != NULL) {
		/* Failed. */
		return EINVAL;
	}

	/*
	 * periodic_updating owns USBCMD while it crosses the PSS barrier. Since
	 * both gates are published under active_lock, observing it clear here
	 * serializes this IAAD write against pause/resume without stopping ASE.
	 */
	if (controller->periodic_updating)
		return EAGAIN;
	status = rd32(controller->operational, EHCI_USBSTS);
	command = rd32(controller->operational, EHCI_USBCMD);
	kern_io_barrier();
	if (status == UINT32_MAX || command == UINT32_MAX ||
	    (status & (EHCI_STS_HSE | EHCI_STS_HALTED)) != 0) {
		/* Failed. */
		return EIO;
	}

	/* Handles the command condition. */
	if ((command & (EHCI_CMD_RUN | EHCI_CMD_ASYNC)) !=
		    (EHCI_CMD_RUN | EHCI_CMD_ASYNC) ||
	    (status & EHCI_STS_ASYNC) == 0) {
		/* Failed. */
		return EIO;
	}

	/* Handles the command condition. */
	if ((command & EHCI_CMD_IAAD) != 0)
		return EAGAIN;

	/* A latched acknowledgement predating this unlink cannot prove it. */
	if ((status & EHCI_STS_IAA) != 0) {
		wr32(controller->operational, EHCI_USBSTS, EHCI_STS_IAA);
		status = rd32(controller->operational, EHCI_USBSTS);
		kern_io_barrier();
		if (status == UINT32_MAX || (status & EHCI_STS_IAA) != 0)
			return EIO;
	}

	/* Checks the operation status. */
	error = ehci_async_unlink_locked(controller, request);
	if (error != 0)
		return error;
	kern_io_write_barrier();
	controller->retirement_generation++;

	/* Handles the controller condition. */
	if (controller->retirement_generation == 0)
		controller->retirement_generation++;
	request->retirement_generation = controller->retirement_generation;
	request->state = EHCI_REQUEST_WAIT_IAA;
	request->iaa_observed = 0;
	controller->iaa_owner = request;
	request->retirement_started = sched_ticks();
	wr32(controller->operational, EHCI_USBCMD, command | EHCI_CMD_IAAD);

	/* Succeeded. */
	return 0;
}

/* Notes that the controller has acknowledged it. */
static int
ehci_retirement_observe_iaa_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	uint32_t command;
	uint32_t status;

	/* Handles the request condition. */
	if (request->state != EHCI_REQUEST_WAIT_IAA ||
	    controller->iaa_owner != request ||
	    request->retirement_generation == 0 ||
	    request->retirement_generation != controller->retirement_generation) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the request condition. */
	if (!request->iaa_observed)
		return EAGAIN;
	status = rd32(controller->operational, EHCI_USBSTS);
	command = rd32(controller->operational, EHCI_USBCMD);
	kern_io_barrier();
	if (status == UINT32_MAX || command == UINT32_MAX ||
	    (status & (EHCI_STS_HSE | EHCI_STS_HALTED)) != 0 ||
	    (status & EHCI_STS_ASYNC) == 0 ||
	    (command & (EHCI_CMD_RUN | EHCI_CMD_ASYNC)) !=
		    (EHCI_CMD_RUN | EHCI_CMD_ASYNC)) {
		/* Failed. */
		return EIO;
	}

	/*
	 * INTx acknowledgement belongs to the IRQ handler.  Its software latch
	 * identifies this exact owner/generation; IAAD clear and no re-latched
	 * IAA complete the checked observation before the request can retire.
	 */
	if ((status & EHCI_STS_IAA) != 0 || (command & EHCI_CMD_IAAD) != 0)
		return EAGAIN;
	request->iaa_observed = 0;
	controller->iaa_owner = NULL;

	/* Succeeded. */
	return 0;
}

/* Finishes retiring one request. */
static void
ehci_retirement_finish_locked(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	ehci_retirement_pop_locked(controller, request);

	/* Handles the request condition. */
	if (request->retirement_reason == EHCI_RETIRE_CANCEL)
		request->state = EHCI_REQUEST_RETIRED_CANCEL;
	else
		request->state = EHCI_REQUEST_COMPLETING;
}

/* Completes a request whose descriptors are no longer reachable. */
static void
ehci_complete_retired_request(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	struct drv_usb_urb *urb = request->urb;
	struct drv_usb_endpoint *endpoint = request->endpoint;
	enum drv_usb_urb_status result = request->completion_status;
	size_t actual = 0;
	unsigned long irq;
	int stall_publication;

	/*
	 * The QH is unreachable and the controller-specific barrier has
	 * completed.
	 */
	kern_io_read_barrier();

	/* Handles the request condition. */
	if (request->retirement_reason != EHCI_RETIRE_DISCONNECT) {
		actual = ehci_request_actual(request);
		ehci_request_commit_toggle(request);

		/* Handles the request condition. */
		if (request->input && actual != 0) {
			memcpy(drv_usb_urb_buffer(urb),
			       (uint8_t *)request->bounce.address + 8U, actual);
		}
	} else {
		result = DRV_USB_URB_DISCONNECTED;
	}

	stall_publication =
		result == DRV_USB_URB_STALL &&
		(drv_usb_endpoint_type(endpoint) == DRV_USB_TRANSFER_BULK ||
		 drv_usb_endpoint_type(endpoint) == DRV_USB_TRANSFER_INTERRUPT);

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the ehci request is active locked result. */
	if (!ehci_request_is_active_locked(controller, request) ||
	    request->state != EHCI_REQUEST_COMPLETING ||
	    drv_usb_urb_hcd_data(urb) != request || request->linked)
		__builtin_trap();

	/* Handles the controller condition. */
	if (controller->completion_inflight == UINT_MAX)
		__builtin_trap();

	/* Checks the drv usb endpoint hcd data result. */
	if (stall_publication &&
	    (drv_usb_endpoint_hcd_data(
		     endpoint, EHCI_ENDPOINT_STALL_PUBLISHING_SLOT) != 0 ||
	     drv_usb_endpoint_set_hcd_data(
		     endpoint, EHCI_ENDPOINT_STALL_PUBLISHING_SLOT, 1U) != 0))
		__builtin_trap();

	/*
	 * Keep controller quiesce closed across callback publication.  The
	 * endpoint marker is deliberately not touched after the core completion
	 * call: runtime device teardown may release the endpoint as soon as
	 * core HCD ownership is dropped.  A successful endpoint_reset clears
	 * the retained marker.
	 */
	controller->completion_inflight++;
	ehci_active_remove_locked(controller, request);
	(void)drv_usb_urb_set_hcd_data(urb, NULL);

	spin_unlock_irqrestore(&controller->active_lock, irq);

	ehci_request_free(controller, request);
	drv_usb_hcd_complete(&controller->hcd, urb, result, actual);

	/* Handles the controller condition. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->completion_inflight == 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}

	controller->completion_inflight--;

	spin_unlock_irqrestore(&controller->active_lock, irq);
}

/* Retires a request that was in the periodic schedule. */
static int
ehci_retire_periodic_request(
	struct ehci_controller *controller,
	struct ehci_request *request)
{
	unsigned long irq;
	int error;
	int resume_error;
	int complete = 0;

	/* Handles the controller condition. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_head != request ||
	    request->state != EHCI_REQUEST_DEACTIVATING) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	/*
	 * This request is now the retirement head and owns the upcoming PSS
	 * barrier.  Time queued behind earlier requests is deliberately
	 * excluded.
	 */
	request->retirement_started = sched_ticks();

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	error = ehci_periodic_update_acquire(controller, 1);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = ehci_periodic_pause(controller);
	if (error != 0) {
		ehci_periodic_update_release(controller);
		goto fail;
	}

	/* Handles the controller condition. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_head != request ||
	    request->state != EHCI_REQUEST_DEACTIVATING) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		(void)ehci_periodic_resume(controller);
		ehci_periodic_update_release(controller);

		/* Failed. */
		return EBUSY;
	}

	/* Checks the operation status. */
	error = ehci_periodic_unlink_locked(controller, request);
	if (error == 0) {
		request->state = EHCI_REQUEST_WAIT_PERIODIC;
		kern_io_write_barrier();
	}

	spin_unlock_irqrestore(&controller->active_lock, irq);

	resume_error = ehci_periodic_resume(controller);
	ehci_periodic_update_release(controller);
	if (error == 0 && resume_error != 0)
		error = resume_error;
	if (error != 0)
		goto fail;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the controller condition. */
	if (controller->retirement_head == request &&
	    request->state == EHCI_REQUEST_WAIT_PERIODIC) {
		ehci_retirement_finish_locked(controller, request);
		complete = request->state == EHCI_REQUEST_COMPLETING;
	}

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the complete condition. */
	if (complete)
		ehci_complete_retired_request(controller, request);

	/* Succeeded. */
	return 0;

fail:
	irq = spin_lock_irqsave(&controller->active_lock);

	ehci_controller_fail_locked(controller, error,
				    "periodic schedule retirement");

	spin_unlock_irqrestore(&controller->active_lock, irq);

	ehci_retirement_report(controller);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Moves every request waiting to retire one step further. */
static void
ehci_retirement_progress(
	struct ehci_controller *controller)
{
	struct ehci_request *request;
	unsigned long irq;
	int complete;
	int error;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		complete = 0;
		error = 0;

		irq = spin_lock_irqsave(&controller->active_lock);

		/* Checks the atomic load n result. */
		request = controller->retirement_head;
		if (request == NULL ||
		    __atomic_load_n(&controller->retirement_stopping,
				    __ATOMIC_ACQUIRE) != 0) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Returns the computed result. */
			return;
		}

		/* Checks the operation status. */
		if (request->state == EHCI_REQUEST_FAILED) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			ehci_retirement_report(controller);

			/* Returns the computed result. */
			return;
		}

		/* Handles the request condition. */
		if (request->schedule_class == EHCI_SCHEDULE_PERIODIC &&
		    request->state == EHCI_REQUEST_DEACTIVATING) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			(void)ehci_retire_periodic_request(controller, request);
			continue;
		}

		/* Handles the request condition. */
		if (request->state == EHCI_REQUEST_DEACTIVATING) {
			/*
			 * The request has reached the queue head.  Bound only
			 * its own wait for a clean doorbell opportunity, never
			 * time spent behind another request.  begin_iaa resets
			 * this for the fresh generation.
			 */
			if (request->retirement_started == 0)
				request->retirement_started = sched_ticks();
			error = ehci_retirement_begin_iaa_locked(controller,
								 request);
		} else if (request->state == EHCI_REQUEST_WAIT_IAA) {
			error = ehci_retirement_observe_iaa_locked(controller,
								   request);
		} else {
			error = EIO;
		}

		/* Checks the operation status. */
		if (error == 0 && request->state == EHCI_REQUEST_WAIT_IAA &&
		    controller->iaa_owner == request) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			sched_yield();
			continue;
		}

		/* Checks the operation status. */
		if (error == 0) {
			ehci_retirement_finish_locked(controller, request);
			complete = request->state == EHCI_REQUEST_COMPLETING;
		} else if (error != EAGAIN ||
			   sched_ticks() - request->retirement_started >=
				   EHCI_RETIRE_TICKS) {
			ehci_controller_fail_locked(
				controller, error == EAGAIN ? ETIMEDOUT : error,
				request->state == EHCI_REQUEST_WAIT_IAA
					? "async-advance acknowledgement"
					: "async-ring unlink");
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Checks the operation status. */
		if (error != 0 && error != EAGAIN) {
			ehci_retirement_report(controller);

			/* Returns the computed result. */
			return;
		}

		/* Checks the operation status. */
		if (error == EAGAIN) {
			/* Checks the sched ticks result. */
			if (sched_ticks() - request->retirement_started >=
			    EHCI_RETIRE_TICKS) {
				ehci_retirement_report(controller);

				/* Returns the computed result. */
				return;
			}

			sched_yield();
			continue;
		}

		/* Checks the atomic exchange n result. */
		if (__atomic_exchange_n(
			    &controller->retirement_success_reported, 1U,
			    __ATOMIC_ACQ_REL) == 0) {
			kern_logf("ehci: checked request-local retirement "
				   "active\n");
		}

		/* Handles the complete condition. */
		if (complete)
			ehci_complete_retired_request(controller, request);
	}
}

/* Stops a controller that has failed beyond recovery. */
static void
ehci_controller_fatal_stop(
	struct ehci_controller *controller)
{
	int root_error;
	int stop_error;

	__atomic_store_n(&controller->fatal_stopping, 1U, __ATOMIC_RELEASE);

	/* Checks the atomic exchange n result. */
	if (__atomic_exchange_n(&controller->fatal_pending, 0U,
				__ATOMIC_ACQ_REL) == 0) {
		__atomic_store_n(&controller->fatal_stopping, 0U,
				 __ATOMIC_RELEASE);

		/* Returns the computed result. */
		return;
	}

	/*
	 * Prevent any new topology dispatch first.  If a dispatch is already in
	 * the USB core, do not join it from the retirement worker: it may be
	 * waiting for request retirement.  The checked stop request still makes
	 * that dispatch the last one, and cleanup can join it later.
	 */
	root_error = ehci_root_worker_stop(controller, 0);
	stop_error = ehci_hardware_stop(controller, "fatal");
	__atomic_store_n(&controller->fatal_stopping, 0U, __ATOMIC_RELEASE);
	if (stop_error != 0 || (root_error != 0 && root_error != EBUSY)) {
		kern_logf("ehci: fatal stop incomplete (hardware=%d root=%d); "
			   "ownership retained\n",
			   stop_error, root_error);
	}
}

/* Stops the thread that serves root port changes. */
static int
ehci_root_worker_stop(
	struct ehci_controller *controller,
	int wait_dispatch)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the worker availability. */
	worker = controller->root_worker;
	if (worker == NULL) {
		/* Handles the controller condition. */
		if (controller->root_joining) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Failed. */
			return EBUSY;
		}

		__atomic_store_n(&controller->root_stopping, 1U,
				 __ATOMIC_RELEASE);
		controller->root_ready = 0;
		controller->root_wake_generation++;
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Succeeded. */
		return 0;
	}

	/* Handles the curthread condition. */
	if (curthread == worker || controller->root_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	__atomic_store_n(&controller->root_stopping, 1U, __ATOMIC_RELEASE);
	controller->root_ready = 0;
	controller->root_wake_generation++;
	kernel_notify_task(worker->task);

	/*
	 * Fatal shutdown runs on the retirement worker.  A root dispatch may be
	 * waiting for request retirement, so that path passes
	 * wait_dispatch=false to stop admission without joining a mutually
	 * dependent dispatch.  Normal quiesce and cleanup run from the USB
	 * core's lock-free stop phase and pass true so the worker is fully
	 * joined before release.
	 */
	if (controller->root_dispatching && !wait_dispatch) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	controller->root_joining = 1;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Continue while the operation condition remains true. */
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	       THREAD_ZOMBIE)
		sched_yield();
	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the controller condition. */
	if (controller->root_worker != worker || controller->root_dispatching ||
	    !controller->root_joining)
		__builtin_trap();
	controller->root_worker = NULL;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	error = thread_wait(worker, NULL);

	/* Handles the root worker availability. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (!controller->root_joining || controller->root_worker != NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}

	controller->root_joining = 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Retires finished requests outside interrupt context. */
static void
ehci_retirement_worker(
	void *argument)
{
	struct ehci_controller *controller = argument;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the atomic load n result. */
		if (__atomic_load_n(&controller->retirement_stopping,
				    __ATOMIC_ACQUIRE) != 0) {
			/* Returns the computed result. */
			return;
		}

		/* Checks the atomic load n result. */
		if (__atomic_load_n(&controller->fatal_pending,
				    __ATOMIC_ACQUIRE) != 0) {
			ehci_controller_fatal_stop(controller);
			continue;
		}

		/* Checks the atomic exchange n result. */
		if (__atomic_exchange_n(&controller->retirement_pending, 0U,
					__ATOMIC_ACQ_REL) != 0) {
			ehci_retirement_progress(controller);
			continue;
		}

		kern_thread_block();
	}
}

/* Starts the thread that does so. */
static int
ehci_retirement_worker_start(
	struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	/* Handles the retirement worker availability. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_worker != NULL ||
	    controller->retirement_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EALREADY;
	}

	__atomic_store_n(&controller->retirement_stopping, 0U,
			 __ATOMIC_RELEASE);
	__atomic_store_n(&controller->retirement_pending, 0U, __ATOMIC_RELEASE);

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	error = kthread_create(ehci_retirement_worker, controller,
			       SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;

	/* Handles the retirement worker availability. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_worker != NULL ||
	    controller->retirement_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}

	controller->retirement_worker = worker;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	thread_start(worker);

	/* Succeeded. */
	return 0;
}

/* Stops that thread and waits for it. */
static int
ehci_retirement_worker_stop(
	struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the worker availability. */
	worker = controller->retirement_worker;
	if (worker == NULL) {
		/* Handles the controller condition. */
		if (controller->retirement_joining) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Failed. */
			return EBUSY;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Succeeded. */
		return 0;
	}

	/*
	 * Completion callbacks execute on this worker and may re-enter PCI
	 * teardown.  It cannot synchronously join itself, and a queued
	 * retirement must remain owned by the live worker until the caller
	 * drains it.
	 */
	if (curthread == worker || controller->retirement_head != NULL ||
	    controller->retirement_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	controller->retirement_joining = 1;
	__atomic_store_n(&controller->retirement_stopping, 1U,
			 __ATOMIC_RELEASE);
	kernel_notify_task(worker->task);

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Continue while the operation condition remains true. */
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	       THREAD_ZOMBIE)
		sched_yield();

	/* Handles the controller condition. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_worker != worker ||
	    !controller->retirement_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}

	controller->retirement_worker = NULL;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	error = thread_wait(worker, NULL);

	/* Handles the retirement worker availability. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (!controller->retirement_joining ||
	    controller->retirement_worker != NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}

	controller->retirement_joining = 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Cancels one transfer that has not completed. */
static int
ehci_urb_dequeue(
	struct drv_usb_hcd *hcd,
	struct drv_usb_urb *urb)
{
	struct ehci_controller *controller = hcd_controller(hcd);
	struct ehci_request *request;
	unsigned long irq;
	int error;
	int inline_retirement;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the ehci request is active locked result. */
	request = drv_usb_urb_hcd_data(urb);
	if (request == NULL || request->urb != urb ||
	    !ehci_request_is_active_locked(controller, request)) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	/* Checks the atomic load n result. */
	if (request->state != EHCI_REQUEST_ACTIVE ||
	    controller->retirement_worker == NULL ||
	    controller->retirement_joining ||
	    __atomic_load_n(&controller->retirement_stopping,
			    __ATOMIC_ACQUIRE) != 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	ehci_retirement_begin_locked(controller, request, EHCI_RETIRE_CANCEL,
				     DRV_USB_URB_CANCELLED);
	inline_retirement = curthread == controller->retirement_worker;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the inline retirement condition. */
	if (inline_retirement)
		ehci_retirement_progress(controller);
	else
		ehci_retirement_worker_wakeup(controller);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the ehci request is active locked result. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (!ehci_request_is_active_locked(controller, request) ||
		    request->urb != urb ||
		    drv_usb_urb_hcd_data(urb) != request) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Failed. */
			return EBUSY;
		}

		/* Handles the request condition. */
		if (request->state == EHCI_REQUEST_RETIRED_CANCEL) {
			kern_io_read_barrier();
			ehci_request_commit_toggle(request);
			ehci_active_remove_locked(controller, request);
			(void)drv_usb_urb_set_hcd_data(urb, NULL);
			spin_unlock_irqrestore(&controller->active_lock, irq);
			ehci_request_free(controller, request);

			/* Succeeded. */
			return 0;
		}

		/* Checks the operation status. */
		if (request->state == EHCI_REQUEST_FAILED) {
			error = request->failure_error != 0
					? request->failure_error
					: EIO;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			ehci_retirement_report(controller);

			/* Failed. */
			return error;
		}

		/* Handles the request condition. */
		if ((request->state != EHCI_REQUEST_DEACTIVATING &&
		     request->state != EHCI_REQUEST_WAIT_IAA &&
		     request->state != EHCI_REQUEST_WAIT_PERIODIC) ||
		    request->retirement_reason != EHCI_RETIRE_CANCEL) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Failed. */
			return EBUSY;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Handles the inline retirement condition. */
		if (inline_retirement)
			ehci_retirement_progress(controller);
		else
			sched_yield();
	}
}

/* Serves root port changes outside interrupt context. */
static void
ehci_root_worker(
	void *argument)
{
	uint64_t observed_generation;
	unsigned long irq;
	int dispatch;
	int ready;
	struct ehci_controller *controller = argument;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		dispatch = 0;

		/* Checks the atomic load n result. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (__atomic_load_n(&controller->root_stopping,
				    __ATOMIC_ACQUIRE) != 0) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Returns the computed result. */
			return;
		}

		observed_generation = controller->root_wake_generation;
		ready = controller->root_ready != 0;
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Handles the ready condition. */
		if (ready) {
			(void)ehci_root_ports_changed(controller);
			irq = spin_lock_irqsave(&controller->active_lock);

			/* Checks the atomic load n result. */
			if (__atomic_load_n(&controller->root_stopping,
					    __ATOMIC_ACQUIRE) == 0 &&
			    controller->root_ready &&
			    !controller->quarantined) {
				controller->root_dispatching = 1;
				dispatch = 1;
			}

			spin_unlock_irqrestore(&controller->active_lock, irq);
		}

		/* Handles the dispatch condition. */
		if (dispatch) {
			/*
			 * This operation takes the USB topology lock and
			 * performs synchronous control transfers.  It must
			 * remain independent of both IRQ context and the
			 * request-retirement worker.
			 */
			drv_usb_hcd_root_hub_changed(&controller->hcd);

			/* Handles the controller condition. */
			irq = spin_lock_irqsave(&controller->active_lock);
			if (!controller->root_dispatching)
				__builtin_trap();
			controller->root_dispatching = 0;
			spin_unlock_irqrestore(&controller->active_lock, irq);
		}

		/*
		 * PCD is the fast path.  This unconditional low-frequency scan
		 * also detects a missed edge and lets the USB core retry a
		 * partially completed device teardown even when no new change
		 * bit is raised. The generation check and locked scheduler
		 * handoff close the check-to-sleep window for PCD, arm, and
		 * stop notifications.
		 */

		/* Checks the atomic load n result. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (__atomic_load_n(&controller->root_stopping,
				    __ATOMIC_ACQUIRE) != 0) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Returns the computed result. */
			return;
		}

		/* Handles the controller condition. */
		if (controller->root_wake_generation != observed_generation) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			continue;
		}

		sched_sleep_locked(sched_ticks() + EHCI_ROOT_POLL_TICKS,
				   &controller->active_lock);
		spin_unlock_irqrestore(&controller->active_lock, irq);
	}
}

/* Starts the thread that does so. */
static int
ehci_root_worker_start(
	struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	/* Handles the root worker availability. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->root_worker != NULL || controller->root_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EALREADY;
	}

	controller->root_ready = 0;
	controller->root_dispatching = 0;
	controller->root_wake_generation = 1;
	controller->root_port_status_valid = 0;
	__atomic_store_n(&controller->root_stopping, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&controller->root_pending, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&controller->root_force_scan, 1U, __ATOMIC_RELEASE);
	memset(controller->root_port_status, 0,
	       sizeof(controller->root_port_status));

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	error = kthread_create(ehci_root_worker, controller,
			       SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;

	/* Handles the root worker availability. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->root_worker != NULL || controller->root_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}

	controller->root_worker = worker;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	thread_start(worker);

	/* Succeeded. */
	return 0;
}

/* Asks that thread to stop. */
static void
ehci_root_worker_request_stop(
	struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);

	__atomic_store_n(&controller->root_stopping, 1U, __ATOMIC_RELEASE);
	controller->root_ready = 0;
	controller->root_wake_generation++;

	/* Handles the worker availability. */
	worker = controller->root_worker;
	if (worker != NULL && !controller->root_joining)
		kernel_notify_task(worker->task);

	spin_unlock_irqrestore(&controller->active_lock, irq);
}

/* Hands a root port change to that thread. */
static void
ehci_root_event_defer(
	struct ehci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the atomic load n result. */
	worker = controller->root_worker;
	if (worker == NULL || controller->root_joining ||
	    __atomic_load_n(&controller->root_stopping, __ATOMIC_ACQUIRE) !=
		    0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Returns the computed result. */
		return;
	}

	__atomic_store_n(&controller->root_pending, 1U, __ATOMIC_RELEASE);
	controller->root_wake_generation++;
	kernel_notify_task(worker->task);

	spin_unlock_irqrestore(&controller->active_lock, irq);
}

/* Waits for every outstanding request to leave. */
static int
ehci_quiesce_requests(
	struct ehci_controller *controller)
{
	int failed;
	int failure_error;
	struct ehci_request *request;
	uint64_t started = sched_ticks();
	unsigned long irq;
	int inline_retirement;
	int wake = 0;

	irq = spin_lock_irqsave(&controller->active_lock);

	/* Process each element required by the operation. */
	for (request = controller->active; request != NULL;
	     request = request->active_next) {
		/* Checks the operation status. */
		if (request->state == EHCI_REQUEST_FAILED) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			ehci_retirement_report(controller);

			/* Returns the computed result. */
			return request->failure_error != 0
				       ? request->failure_error
				       : EIO;
		}

		/* Handles the request condition. */
		if (request->state != EHCI_REQUEST_ACTIVE)
			continue;
		ehci_retirement_begin_locked(controller, request,
					     EHCI_RETIRE_DISCONNECT,
					     DRV_USB_URB_DISCONNECTED);
		wake = 1;
	}

	inline_retirement = curthread == controller->retirement_worker;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the wake condition. */
	if (wake) {
		/* Handles the inline retirement condition. */
		if (inline_retirement)
			ehci_retirement_progress(controller);
		else
			ehci_retirement_worker_wakeup(controller);
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		failed = 0;
		failure_error = EIO;

		/* Handles the controller condition. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->active_count == 0 &&
		    controller->completion_inflight == 0 &&
		    controller->reclaim_request_busy == 0) {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Succeeded. */
			return 0;
		}

		/* Process each element required by the operation. */
		for (request = controller->active; request != NULL;
		     request = request->active_next) {
			/* Checks the operation status. */
			if (request->state != EHCI_REQUEST_FAILED)
				continue;
			failed = 1;
			failure_error = request->failure_error != 0
						? request->failure_error
						: EIO;
			break;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Checks the operation status. */
		if (failed) {
			ehci_retirement_report(controller);

			/* Returns the computed result. */
			return failure_error;
		}

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS)
			return EBUSY;

		/* Handles the inline retirement condition. */
		if (inline_retirement) {
			ehci_retirement_progress(controller);
		} else {
			ehci_retirement_worker_wakeup(controller);
			sched_yield();
		}
	}
}

/* Records what state the controller was left in. */
static int
ehci_report_shutdown_evidence(
	struct ehci_controller *controller)
{
	unsigned long irq;
	int ready;
#ifdef KERN_TEST_CHECKPOINTS
	int report = 0;

#endif

	/* Tests that nothing is left in flight or running. */
	irq = spin_lock_irqsave(&controller->active_lock);

	ready = controller->dma_quiesced && controller->hardware_stopped &&
		!controller->hardware_stop_in_progress &&
		controller->hardware_stop_waiters == 0 &&
		controller->active == NULL && controller->active_count == 0 &&
		controller->completion_inflight == 0 &&
		controller->builders == 0 &&
		controller->reclaim_request_busy == 0 &&
		controller->retirement_head == NULL &&
		controller->iaa_owner == NULL &&
		!controller->periodic_updating &&
		controller->retirement_worker == NULL &&
		!controller->retirement_joining &&
		controller->root_worker == NULL && !controller->root_joining &&
		!controller->root_dispatching;
#ifdef KERN_TEST_CHECKPOINTS

	/* Handles the ready condition. */
	if (ready && !controller->shutdown_evidence_reported) {
		controller->shutdown_evidence_reported = 1;
		report = 1;
	}

#endif

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the ready condition. */
	if (!ready)
		return EBUSY;
#ifdef KERN_TEST_CHECKPOINTS

	/* Handles the report condition. */
	if (report)
		kern_logf("ehci: checked shutdown workers joined\n");
#endif

	/* Succeeded. */
	return 0;
}

/* Brings the controller to a stop that nothing is running under. */
static int
ehci_quiesce(
	struct drv_usb_hcd *hcd)
{
	int function_result;
	unsigned fatal_pending;
	unsigned fatal_stopping;
	struct ehci_controller *controller = hcd_controller(hcd);
	uint64_t started;
	unsigned long irq;
	int error;
	int request_error;
	int root_error;

	irq = spin_lock_irqsave(&controller->active_lock);

	controller->quiescing = 1;

	/* Handles the controller condition. */
	if (controller->dma_quiesced) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Checks the operation status. */
		root_error = ehci_root_worker_stop(controller, 1);
		if (root_error != 0)
			return root_error;

		/* Checks the operation status. */
		error = ehci_retirement_worker_stop(controller);
		if (error != 0)
			return error;

		/* Obtains the ehci report shutdown evidence result. */
		function_result = ehci_report_shutdown_evidence(controller);

		/* Returns the computed result. */
		return function_result;
	}

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/*
	 * A fatal IRQ delegates the blocking halt/master/IRQ sequence to the
	 * retirement worker.  The USB core's lock-free quiesce phase waits for
	 * that single checked hardware-stop owner instead of racing it.
	 */
	started = sched_ticks();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		fatal_pending = __atomic_load_n(&controller->fatal_pending,
						__ATOMIC_ACQUIRE);

		/* Handles the fatal pending condition. */
		fatal_stopping = __atomic_load_n(&controller->fatal_stopping,
						 __ATOMIC_ACQUIRE);
		if (!fatal_pending && !fatal_stopping)
			break;

		/* Handles the fatal pending condition. */
		if (fatal_pending)
			ehci_retirement_worker_wakeup(controller);

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS)
			return EBUSY;
		sched_yield();
	}

	/* Checks the operation status. */
	root_error = ehci_root_worker_stop(controller, 1);
	if (root_error != 0)
		return root_error;
	started = sched_ticks();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the controller condition. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->builders == 0) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			break;
		}

		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= EHCI_QUIESCE_TICKS)
			return EBUSY;
		sched_yield();
	}

	request_error = ehci_quiesce_requests(controller);

	/*
	 * A retirement failure publishes fatal_pending, but the worker is only
	 * an asynchronous stop owner.  The shutdown caller must join the
	 * serialized hardware barrier before returning even when request DMA
	 * remains retained.
	 */

	/* Checks the operation status. */
	error = ehci_hardware_stop(controller, "quiesce");
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->active_lock);

	/* Handles the controller condition. */
	if (!controller->hardware_stopped)
		__builtin_trap();
	controller->dma_quiesced = 1;

	/* Checks the operation status. */
	if (request_error != 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Returns the computed result. */
		return request_error;
	}

	/* Handles the active availability. */
	if (controller->active != NULL || controller->active_count != 0 ||
	    controller->completion_inflight != 0 || controller->builders != 0 ||
	    controller->reclaim_request_busy != 0 ||
	    controller->retirement_head != NULL ||
	    controller->iaa_owner != NULL || controller->periodic_updating)
		__builtin_trap();

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Checks the operation status. */
	error = ehci_retirement_worker_stop(controller);
	if (error != 0)
		return error;

	/* Obtains the ehci report shutdown evidence result. */
	function_result = ehci_report_shutdown_evidence(controller);

	/* Returns the computed result. */
	return function_result;
}

/* Stops the controller. */
static void
ehci_stop(
	struct drv_usb_hcd *hcd)
{
	struct ehci_controller *controller = hcd_controller(hcd);
	unsigned long irq;
	int releasable;

	/* Tests that nothing is left in flight or running. */
	irq = spin_lock_irqsave(&controller->active_lock);

	releasable = controller->dma_quiesced && controller->active == NULL &&
		     controller->active_count == 0 &&
		     controller->completion_inflight == 0 &&
		     controller->builders == 0 &&
		     controller->reclaim_request_busy == 0 &&
		     controller->retirement_head == NULL &&
		     controller->iaa_owner == NULL &&
		     !controller->periodic_updating &&
		     controller->hardware_stopped &&
		     !controller->hardware_stop_in_progress &&
		     controller->hardware_stop_waiters == 0 &&
		     controller->retirement_worker == NULL &&
		     !controller->retirement_joining &&
		     controller->root_worker == NULL &&
		     !controller->root_joining && !controller->root_dispatching;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the releasable condition. */
	if (!releasable) {
		kern_logf("ehci: refusing to release DMA before checked "
			   "quiesce\n");
		controller->quarantined = 1;

		/* Returns the computed result. */
		return;
	}

	ehci_schedule_release(controller);
}

/* Prepares the driver state one endpoint needs. */
static int
ehci_endpoint_enable(
	struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	(void)hcd;
	(void)endpoint;

	/* Succeeded. */
	return 0;
}

/* Gives that state back. */
static int
ehci_endpoint_disable(
	struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct ehci_controller *controller = hcd_controller(hcd);
	unsigned long irq = spin_lock_irqsave(&controller->active_lock);
	int error = ehci_endpoint_owner_locked(controller, endpoint) == NULL
			    ? 0
			    : EBUSY;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Clears an endpoint's halt and its data toggle. */
static int
ehci_endpoint_reset(
	struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{
	struct ehci_controller *controller = hcd_controller(hcd);
	unsigned long irq;
	int error;

	/* Handles the endpoint availability. */
	if (endpoint == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&controller->active_lock);

	/* Checks the atomic load n result. */
	if (controller->quiescing || controller->dma_quiesced ||
	    controller->quarantined || controller->hardware_stopped ||
	    controller->retirement_worker == NULL ||
	    controller->retirement_joining ||
	    __atomic_load_n(&controller->retirement_stopping,
			    __ATOMIC_ACQUIRE) != 0) {
		error = ENODEV;
	} else if (ehci_endpoint_owner_locked(controller, endpoint) != NULL) {
		error = EBUSY;
	} else if (drv_usb_endpoint_hcd_data(
			   endpoint, EHCI_ENDPOINT_STALL_PUBLISHING_SLOT) >
		   1U) {
		error = EIO;
	} else {
		/* Checks the operation status. */
		error = drv_usb_endpoint_set_hcd_data(endpoint, 0, 0);
		if (error == 0) {
			error = drv_usb_endpoint_set_hcd_data(
				endpoint, EHCI_ENDPOINT_STALL_PUBLISHING_SLOT,
				0);
		}
	}

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the frame number the bus stands at. */
static uint32_t
ehci_frame_number(
	struct drv_usb_hcd *hcd)
{
	uint32_t function_result;
	struct ehci_controller *controller = hcd_controller(hcd);

	/* Computes the function result. */
	function_result = rd32(controller->operational, EHCI_FRINDEX) >> 3;

	/* Returns the computed result. */
	return function_result;
}

/* Reports which root ports have changed. */
static int
ehci_root_status(
	struct drv_usb_hcd *hcd,
	void *buffer,
	size_t size,
	size_t *actual)
{
	uint32_t status;
	struct ehci_controller *controller = hcd_controller(hcd);
	uint8_t *bits = buffer;
	size_t bytes = ((size_t)hcd->root_port_count + 1U + 7U) / 8U;
	unsigned port;

	/* Handles the buffer availability. */
	if (buffer == NULL || size < bytes)
		return EINVAL;
	memset(bits, 0, bytes);
	/* Process each remaining element. */
	for (port = 0; port < hcd->root_port_count; port++) {
		/* Checks the operation status. */
		status = rd32(controller->operational, EHCI_PORTSC(port));
		if (status == UINT32_MAX)
			return EIO;
		if ((status &
		     (EHCI_PORT_CONNECT_CHANGE | EHCI_PORT_ENABLE_CHANGE |
		      EHCI_PORT_OVER_CURRENT_CHANGE)) != 0) {
			bits[(port + 1U) / 8U] |=
				(uint8_t)(1U << ((port + 1U) % 8U));
		}
	}

	/* Handles the actual availability. */
	if (actual != NULL)
		*actual = bytes;
	/* Succeeded. */
	return 0;
}

/* Serves one hub request against the root ports. */
static int
ehci_root_control(
	struct drv_usb_hcd *hcd,
	const struct drv_usb_control_request *request,
	void *buffer,
	size_t size,
	size_t *actual)
{
	int error_local;
	int error_local1;
	uint32_t value;
	struct ehci_controller *controller = hcd_controller(hcd);
	unsigned port;
	uint32_t status;

	/* Handles the request availability. */
	if (request == NULL || request->index < 1 ||
	    request->index > hcd->root_port_count) {
		/* Failed. */
		return EINVAL;
	}
	port = request->index - 1U;

	/* Checks the operation status. */
	status = rd32(controller->operational, EHCI_PORTSC(port));
	if (status == UINT32_MAX)
		return EIO;

	/* Handles the buffer availability. */
	if (request->request == 0 && buffer != NULL && size >= 4U) {
		/* Checks the operation status. */
		value = 0;
		if ((status & EHCI_PORT_OWNER) == 0) {
			/* Checks the operation status. */
			if ((status & EHCI_PORT_CONNECT) != 0)
				value |= 1U;

			/* Checks the operation status. */
			if ((status & EHCI_PORT_ENABLE) != 0)
				value |= 2U;

			/* Checks the operation status. */
			if ((status & EHCI_PORT_OVER_CURRENT) != 0)
				value |= 8U;

			/* Checks the operation status. */
			if ((status & EHCI_PORT_RESET) != 0)
				value |= 0x10U;
			value |= 0x400U;
		}

		/* Checks the operation status. */
		if ((status & EHCI_PORT_CONNECT_CHANGE) != 0)
			value |= 0x10000U;

		/* Checks the operation status. */
		if ((status & EHCI_PORT_ENABLE_CHANGE) != 0)
			value |= 0x20000U;

		/* Checks the operation status. */
		if ((status & EHCI_PORT_OVER_CURRENT_CHANGE) != 0)
			value |= 0x80000U;
		memcpy(buffer, &value, sizeof(value));

		/* Handles the actual availability. */
		if (actual != NULL)
			*actual = sizeof(value);
		/* Succeeded. */
		return 0;
	}

	/* Handles the request condition. */
	if (request->request == 3 && request->value == 4) {
		/*
		 * A directly attached low/full-speed device belongs to a
		 * companion UHCI controller.  Only low-speed K-state can be
		 * handed off before reset.  A full-speed J-state must first
		 * undergo reset and is handed off only when it remains
		 * connected without PED afterwards.
		 */
		if ((status & EHCI_PORT_OWNER) != 0) {
			/* Handles the actual availability. */
			if (actual != NULL)
				*actual = 0;
			/* Succeeded. */
			return 0;
		}

		/* Checks the operation status. */
		if ((status & EHCI_PORT_CONNECT) == 0)
			return ENODEV;

		/* Checks the operation status. */
		if ((status & EHCI_PORT_LINE_STATUS) ==
		    EHCI_PORT_LINE_K_STATE) {
			error_local =
				ehci_port_handoff(controller, port, status);

			/* Checks the operation status. */
			if (error_local != 0)
				return error_local;

			/* Handles the actual availability. */
			if (actual != NULL)
				*actual = 0;
			/* Succeeded. */
			return 0;
		}

		ehci_port_write(controller, port, status,
				EHCI_PORT_RESET | EHCI_PORT_POWER,
				EHCI_PORT_ENABLE, 0);

		/* Handles the actual availability. */
		if (actual != NULL)
			*actual = 0;
		/* Succeeded. */
		return 0;
	}

	/* Handles the request condition. */
	if (request->request == 1) {
		/* Handles the request condition. */
		if (request->value == 4) {
			/* Checks the operation status. */
			error_local1 = ehci_port_finish_reset(controller, port,
							      status);
			if (error_local1 != 0)
				return error_local1;
		} else if (request->value == 16) {
			ehci_port_write(controller, port, status, 0, 0,
					EHCI_PORT_CONNECT_CHANGE);
		} else if (request->value == 17) {
			ehci_port_write(controller, port, status, 0, 0,
					EHCI_PORT_ENABLE_CHANGE);
		} else if (request->value == 19) {
			ehci_port_write(controller, port, status, 0, 0,
					EHCI_PORT_OVER_CURRENT_CHANGE);
		} else {
			/* Failed. */
			return ENOTSUP;
		}

		/* Handles the actual availability. */
		if (actual != NULL)
			*actual = 0;
		/* Succeeded. */
		return 0;
	}

	/* Handles the request condition. */
	if (request->request == 3 && request->value == 1) {
		/* Handles the actual availability. */
		if (actual != NULL)
			*actual = 0;
		/* Succeeded. */
		return 0;
	}

	/* Failed. */
	return ENOTSUP;
}

/* Serves one interrupt from the controller. */
static int
ehci_irq(
	void *argument)
{
	enum drv_usb_urb_status result;
	struct ehci_controller *controller = argument;
	struct ehci_request *request;
	struct ehci_request *iaa_request = NULL;
	const char *fatal_stage = NULL;
	uint32_t acknowledge;
	uint32_t readback;
	uint32_t status;
	unsigned long irq;
	int fatal_event = 0;
	int mmio_invalid = 0;
	int report_controller = 0;
	int wake_retirement = 0;
	int wake_root = 0;

	irq = spin_lock_irqsave(&controller->active_lock);

	status = rd32(controller->operational, EHCI_USBSTS);
	kern_io_barrier();
	if (status == UINT32_MAX) {
		report_controller = !controller->quarantined;
		controller->fatal_mmio_invalid = 1;
		ehci_controller_fail_locked(controller, EIO, "IRQ status read");
		spin_unlock_irqrestore(&controller->active_lock, irq);
		ehci_root_worker_request_stop(controller);

		/* Handles the report controller condition. */
		if (report_controller) {
			kern_logf("ehci: invalid IRQ status; controller "
				   "quarantined\n");
		}

		ehci_retirement_report(controller);
		ehci_retirement_worker_wakeup(controller);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the acknowledge condition. */
	acknowledge = status & EHCI_STS_ALL;
	if (acknowledge == 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Succeeded. */
		return 0;
	}

	/* Checks the operation status. */
	if ((status & EHCI_STS_HSE) != 0) {
		report_controller = !controller->quarantined;
		ehci_controller_fail_locked(controller, EIO,
					    "host-system error");
		fatal_stage = "host-system error";
		fatal_event = 1;
		wake_retirement = 1;
	} else if ((status & EHCI_STS_IAA) != 0 &&
		   controller->iaa_owner != NULL) {
		/* Handles the iaa request condition. */
		iaa_request = controller->iaa_owner;
		if (iaa_request->state != EHCI_REQUEST_WAIT_IAA ||
		    iaa_request->retirement_generation == 0 ||
		    iaa_request->retirement_generation !=
			    controller->retirement_generation) {
			report_controller = !controller->quarantined;
			ehci_controller_fail_locked(controller, EIO,
						    "async-advance owner");
			fatal_stage = "invalid async-advance owner";
			fatal_event = 1;
			iaa_request = NULL;
			wake_retirement = 1;
		}
	}

	/*
	 * EHCI is currently attached through INTx.  Always deassert a latched
	 * IAA in IRQ context, read it back, and publish software evidence only
	 * to the exact request/generation which owned the doorbell.  Leaving
	 * IAA for the worker would keep the level interrupt asserted and can
	 * livelock it.
	 */
	wr32(controller->operational, EHCI_USBSTS, acknowledge);

	/* Handles the acknowledge condition. */
	if ((acknowledge & EHCI_STS_IAA) != 0) {
		readback = rd32(controller->operational, EHCI_USBSTS);
		kern_io_barrier();

		/* Handles the readback condition. */
		if (readback == UINT32_MAX || (readback & EHCI_STS_IAA) != 0) {
			/* Handles the readback condition. */
			if (readback == UINT32_MAX) {
				controller->fatal_mmio_invalid = 1;
				mmio_invalid = 1;
			}

			/* Handles the fatal event condition. */
			if (!fatal_event) {
				report_controller = !controller->quarantined;
				ehci_controller_fail_locked(
					controller, EIO,
					"async-advance acknowledgement");
				fatal_stage =
					"async-advance acknowledgement failure";
				fatal_event = 1;
				wake_retirement = 1;
			}

			iaa_request = NULL;
		} else if (!fatal_event && iaa_request != NULL &&
			   !iaa_request->iaa_observed) {
			/*
			 * Only the owner captured under active_lock receives
			 * the untagged hardware indication.  A duplicate is
			 * still W1C-acked above but neither changes generations
			 * nor queues another wake.
			 */
			iaa_request->iaa_observed = 1;
			wake_retirement = 1;
		}
	}

	/* Handles the fatal event condition. */
	if (!fatal_event && (status & EHCI_STS_PCD) != 0)
		wake_root = 1;

	/* Handles the fatal event condition. */
	if (!fatal_event &&
	    (status & (EHCI_STS_USBINT | EHCI_STS_USBERRINT)) != 0) {
		/* Process each element required by the operation. */
		for (request = controller->active; request != NULL;
		     request = request->active_next) {
			/* Checks the ehci request terminal result. */
			if (request->state != EHCI_REQUEST_ACTIVE ||
			    !ehci_request_terminal(request, &result))
				continue;
			ehci_retirement_begin_locked(controller, request,
						     EHCI_RETIRE_COMPLETE,
						     result);
			wake_retirement = 1;
		}
	}

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the fatal event condition. */
	if (fatal_event) {
		/*
		 * Mask immediately to prevent an interrupt storm; bounded
		 * RUN/DMA shutdown and checked IRQ removal execute on the
		 * worker.
		 */
		if (!mmio_invalid)
			wr32(controller->operational, EHCI_USBINTR, 0);
		ehci_root_worker_request_stop(controller);
	}

	/* Handles the report controller condition. */
	if (report_controller)
		kern_logf("ehci: %s; controller quarantined\n", fatal_stage);

	/* Handles the fatal event condition. */
	if (fatal_event)
		ehci_retirement_report(controller);

	/* Handles the wake retirement condition. */
	if (wake_retirement)
		ehci_retirement_worker_wakeup(controller);

	/* Handles the wake root condition. */
	if (wake_root)
		ehci_root_event_defer(controller);

	/* Reports operation failure. */
	return 1;
}

/* Publishes the controller so the rest of the kernel sees it. */
static void
ehci_publish(
	struct ehci_controller *controller)
{
	/* Handles the controller condition. */
	if (controller->listed)
		return;
	drv_pci_device_set_driver_data(controller->pci, controller);
	controller->next = ehci_controllers;
	ehci_controllers = controller;
	controller->listed = 1;
}

/* Takes it back out of view. */
static void
ehci_unpublish(
	struct ehci_controller *controller)
{
	struct ehci_controller **link;

	/* Handles the controller condition. */
	if (!controller->listed)
		return;
	/* Process each linked entry. */
	for (link = &ehci_controllers; *link != NULL; link = &(*link)->next) {
		/* Handles the link condition. */
		if (*link != controller)
			continue;
		*link = controller->next;
		break;
	}

	controller->next = NULL;
	controller->listed = 0;
}

/* Gives every PCI resource this driver claimed back. */
static int
ehci_pci_release(
	struct ehci_controller *controller)
{
	unsigned long irq;
	int error;
	int releasable;

	/* Tests that no worker or stop is still running. */
	irq = spin_lock_irqsave(&controller->active_lock);

	releasable = controller->retirement_worker == NULL &&
		     !controller->retirement_joining &&
		     controller->root_worker == NULL &&
		     !controller->root_joining &&
		     !controller->root_dispatching &&
		     !controller->hardware_stop_in_progress &&
		     controller->hardware_stop_waiters == 0 &&
		     controller->reclaim_request_busy == 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the address availability. */
	if (!releasable || controller->periodic.address != NULL ||
	    controller->periodic_skeleton_memory.address != NULL ||
	    controller->async_head_memory.address != NULL ||
	    controller->reclaim_request.schedule.address != NULL ||
	    controller->reclaim_request.bounce.address != NULL ||
	    !controller->dma_quiesced) {
		/* Failed. */
		return EBUSY;
	}

	/* Handles the controller condition. */
	if (controller->pci_state_saved) {
		/* Checks the operation status. */
		error = ehci_bus_master_disable(controller);
		if (error != 0)
			return error;
	}

	/* Handles the controller condition. */
	if (controller->bar_mapped) {
		drv_pci_device_unmap_bar(controller->pci,
					 &controller->registers);
		controller->bar_mapped = 0;
		controller->capability = NULL;
		controller->operational = NULL;
	}

	/* Handles the controller condition. */
	if (controller->pci_state_saved) {
		/* Checks the operation status. */
		error = drv_pci_device_restore_enable_state(
			controller->pci, &controller->pci_enable_state);
		if (error != 0)
			return error;
		controller->pci_state_saved = 0;
	}

	/* Handles the controller condition. */
	if (controller->bar_claimed) {
		drv_pci_device_release_bar(controller->pci, 0);
		controller->bar_claimed = 0;
	}

	/* Succeeded. */
	return 0;
}

/* Undoes everything a failed or finished attach had done. */
static int
ehci_cleanup(
	struct ehci_controller *controller)
{
	int function_result;
	unsigned had_root;
	unsigned root_was_ready;
	unsigned long irq;
	int error;
	int restart_error;

	/*
	 * Root topology dispatch and completion callbacks may re-enter PCI
	 * teardown.  Reject before closing either worker or HCD admission when
	 * the caller cannot own both joins, or another caller already owns one.
	 */

	/* Handles the controller condition. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->root_worker == curthread || controller->root_joining ||
	    controller->retirement_worker == curthread ||
	    controller->retirement_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	had_root = controller->root_worker != NULL;
	root_was_ready = controller->root_ready;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Handles the controller condition. */
	if (controller->hcd_registered) {
		/*
		 * Detach/attach-unwind runs outside the USB topology lock, so
		 * close and join the root producer before unregister enters
		 * that lock.
		 */

		/* Checks the operation status. */
		error = ehci_root_worker_stop(controller, 1);
		if (error != 0)
			return error;

		/* Checks the operation status. */
		error = drv_usb_hcd_unregister(&controller->hcd);
		if (error != 0) {
			/*
			 * EBUSY before quiesce leaves the HCD live.  Restore
			 * root observation so a later disconnect or detach
			 * retry can progress.
			 */
			if (error == EBUSY && had_root &&
			    !controller->quiescing) {
				restart_error =
					ehci_root_worker_start(controller);

				/* Checks the operation status. */
				if (restart_error != 0)
					return restart_error;

				/* Handles the root was ready condition. */
				if (root_was_ready)
					ehci_root_worker_arm(controller);
			}

			/* Failed. */
			return error;
		}

		controller->hcd_registered = 0;
	} else {
		/*
		 * Defensive attach-unwind path: a worker must never outlive its
		 * controller even if registration did not become externally
		 * visible.
		 */

		/* Checks the operation status. */
		error = ehci_root_worker_stop(controller, 1);
		if (error != 0)
			return error;

		/* Checks the operation status. */
		error = ehci_retirement_worker_stop(controller);
		if (error != 0)
			return error;
	}

	/*
	 * A start failure may have made the schedule controller-visible before
	 * registration completed.  Retry the unique checked stop on every later
	 * cleanup attempt; never unmap PCI state or free that graph until it
	 * wins.
	 */
	if (controller->periodic.address != NULL ||
	    controller->periodic_skeleton_memory.address != NULL ||
	    controller->async_head_memory.address != NULL ||
	    controller->reclaim_request.schedule.address != NULL ||
	    controller->reclaim_request.bounce.address != NULL) {
		/* Handles the controller condition. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->dma_quiesced) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			ehci_schedule_release(controller);
		} else {
			spin_unlock_irqrestore(&controller->active_lock, irq);

			/* Checks the operation status. */
			error = ehci_hardware_stop(controller,
						   "attach cleanup");
			if (error != 0)
				return error;
			irq = spin_lock_irqsave(&controller->active_lock);
			controller->dma_quiesced = 1;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			ehci_schedule_release(controller);
		}
	}

	/* Handles the controller condition. */
	if (controller->irq_allocated) {
		/* A registered HCD removes the checked IRQ from quiesce. */
		if (controller->irq_cookie != NULL)
			return EBUSY;
		drv_pci_device_free_irqs(controller->pci, &controller->irq, 1);
		controller->irq_allocated = 0;
	}

	/* Obtains the ehci pci release result. */
	function_result = ehci_pci_release(controller);

	/* Returns the computed result. */
	return function_result;
}

/* Asks whether the controller is running and free of faults. */
static int
ehci_runtime_operational(
	struct ehci_controller *controller)
{
	unsigned long irq;
	int operational;

	/* Tests that the controller is registered and running. */
	irq = spin_lock_irqsave(&controller->active_lock);

	operational =
		controller->hcd_registered && !controller->quarantined &&
		!controller->quiescing && !controller->dma_quiesced &&
		!controller->hardware_stopped &&
		!controller->hardware_stop_in_progress &&
		controller->retirement_worker != NULL &&
		!controller->retirement_joining &&
		controller->root_worker != NULL && !controller->root_joining &&
		__atomic_load_n(&controller->fatal_pending, __ATOMIC_ACQUIRE) ==
			0 &&
		__atomic_load_n(&controller->fatal_stopping,
				__ATOMIC_ACQUIRE) == 0;

	spin_unlock_irqrestore(&controller->active_lock, irq);

	/* Returns the computed result. */
	return operational;
}

/* Brings up a controller the PCI bus has just matched. */
static int
ehci_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct ehci_controller *controller;
	unsigned count = 0;
	unsigned ports;
	const char *stage = "allocation";
	int cleanup_error;
	int error;

	(void)id;

	/* Handles the controller availability. */
	controller = kern_malloc(sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;
	memset(controller, 0, sizeof(*controller));
	spin_init(&controller->active_lock, LOCK_RANK_DEVICE,
		  "EHCI request/schedule");
	controller->pci = device;
	controller->dma_quiesced = 1;
	controller->quiescing = 1;
	stage = "BAR claim";

	/* Checks the operation status. */
	error = drv_pci_device_claim_bar(device, 0);
	if (error != 0)
		goto fail;
	controller->bar_claimed = 1;
	stage = "PCI command save";

	/* Checks the operation status. */
	error = drv_pci_device_save_enable_state(device,
						 &controller->pci_enable_state);
	if (error != 0)
		goto fail;
	controller->pci_state_saved = 1;
	stage = "BAR map";

	/* Checks the operation status. */
	error = drv_pci_device_map_bar(device, 0,
				       DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE |
					       DRV_PCI_MAP_NOCACHE,
				       &controller->registers);
	if (error != 0)
		goto fail;
	controller->bar_mapped = 1;
	controller->capability = controller->registers.address;
	controller->operational =
		controller->capability + rd8(controller->capability, 0);
	ports = rd32(controller->capability, 4);
	controller->port_power_control = (ports & EHCI_HCSPARAMS_PPC) != 0;
	controller->companion_count =
		(ports & EHCI_HCSPARAMS_N_CC_MASK) >> EHCI_HCSPARAMS_N_CC_SHIFT;
	ports &= 15U;

	/* Handles the ports condition. */
	if (ports == 0) {
		error = ENODEV;
		stage = "capabilities";
		goto fail;
	}

	controller->hcd.name = "EHCI";
	controller->hcd.ops = &ehci_ops;
	controller->hcd.dma = drv_pci_device_dma(device);
	controller->hcd.root_port_count = ports;
	controller->hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	controller->hcd.private_data[0] = (uintptr_t)controller;

	/* Checks the operation status. */
	stage = "PCI enable";
	if ((error = drv_pci_device_enable_memory(device)) != 0 ||
	    (error = drv_pci_device_set_bus_master(device, true)) != 0)
		goto fail;
	stage = "HCD registration";

	/* Checks the operation status. */
	error = drv_usb_hcd_register(&controller->hcd, &controller->bus);
	if (error != 0)
		goto fail;
	controller->hcd_registered = 1;
	stage = "IRQ allocation";

	/* Checks the operation status. */
	error = drv_pci_device_allocate_irqs(device, DRV_PCI_IRQ_ALLOW_INTX, 1,
					     1, &controller->irq, &count);
	if (error != 0)
		goto fail;
	controller->irq_allocated = 1;
	stage = "IRQ establishment";

	/* Checks the operation status. */
	error = drv_pci_device_establish_irq(device, &controller->irq, ehci_irq,
					     controller, "ehci",
					     &controller->irq_cookie);
	if (error != 0)
		goto fail;
	stage = "retirement worker";

	/* Checks the operation status. */
	error = ehci_retirement_worker_start(controller);
	if (error != 0)
		goto fail;
	stage = "root hotplug worker";

	/* Checks the operation status. */
	error = ehci_root_worker_start(controller);
	if (error != 0)
		goto fail;
	wr32(controller->operational, EHCI_USBINTR,
	     EHCI_STS_USBINT | EHCI_STS_USBERRINT | EHCI_STS_PCD |
		     EHCI_STS_HSE | EHCI_STS_IAA);
	ehci_publish(controller);
	kern_logf("ehci: PCI controller, ports=%u version=%x\n", ports,
		   *(volatile uint16_t *)(controller->capability + 2));
	kern_logf("ehci: concurrent async/periodic scheduling active\n");
	kern_logf("ehci: root hotplug worker active\n");

	/* Succeeded. */
	return 0;

fail:

	/* Checks the operation status. */
	cleanup_error = ehci_cleanup(controller);
	if (cleanup_error != 0) {
		controller->quarantined = 1;
		ehci_publish(controller);
		kern_logf("ehci: attach failed at %s (%d), cleanup failed "
			   "(%d); controller quarantined\n",
			   stage, error, cleanup_error);

		/* Succeeded. */
		return 0;
	}

	kern_free(controller);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes a controller out of service and gives it back. */
static int
ehci_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	struct ehci_controller *controller = drv_pci_device_driver_data(device);
	int error;

	(void)flags;

	/* Handles the controller availability. */
	if (controller == NULL)
		return 0;

	/* Checks the operation status. */
	error = ehci_cleanup(controller);
	if (error != 0) {
		/*
		 * An early detach can legitimately race a worker or live USB
		 * device. Cleanup restores root observation in that EBUSY path;
		 * do not poison a controller whose full runtime ownership graph
		 * is still operational.
		 */
		if (error != EBUSY || !ehci_runtime_operational(controller))
			controller->quarantined = 1;

		/* Failed. */
		return error;
	}

	ehci_unpublish(controller);
	drv_pci_device_set_driver_data(device, NULL);
	kern_free(controller);

	/* Succeeded. */
	return 0;
}

/*
 * EHCI
 */
