/*
 * PCI UHCI host controller driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-uhci.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <string.h>

#define UHCI_USBCMD       0x00U
#define UHCI_USBSTS       0x02U
#define UHCI_USBINTR      0x04U
#define UHCI_FRNUM        0x06U
#define UHCI_FRNUM_MASK   0x07ffU
#define UHCI_FLBASEADD    0x08U
#define UHCI_SOFMOD       0x0cU
#define UHCI_PORTSC1      0x10U
#define UHCI_PORTSC2      0x12U

#define UHCI_CMD_RUN      0x0001U
#define UHCI_CMD_HCRESET  0x0002U
#define UHCI_CMD_GRESET   0x0004U
#define UHCI_CMD_CF       0x0040U
#define UHCI_STS_ALL      0x003fU
#define UHCI_STS_HOST_SYSTEM_ERROR 0x0008U
#define UHCI_STS_PROCESS_ERROR 0x0010U
#define UHCI_STS_HALTED   0x0020U
#define UHCI_PORT_CCS     0x0001U
#define UHCI_PORT_CSC     0x0002U
#define UHCI_PORT_PE      0x0004U
#define UHCI_PORT_PEC     0x0008U
#define UHCI_PORT_RD      0x0040U
#define UHCI_PORT_LSDA    0x0100U
#define UHCI_PORT_RESET   0x0200U
#define UHCI_PORT_SUSPEND 0x1000U
#define UHCI_LINK_TERM    0x00000001U
#define UHCI_LINK_QH      0x00000002U
#define UHCI_LINK_DEPTH   0x00000004U
#define UHCI_LINK_RESERVED 0x00000008U
#define UHCI_LINK_ADDRESS 0xfffffff0U
#define UHCI_TD_SHORT_PACKET 0x20000000U
#define UHCI_TD_ACTIVE    0x00800000U
#define UHCI_TD_IOC       0x01000000U
#define UHCI_TD_LOW_SPEED 0x04000000U
#define UHCI_TD_STALLED   0x00400000U
#define UHCI_TD_ERRORS    0x007e0000U
#define UHCI_PID_OUT      0xe1U
#define UHCI_PID_IN       0x69U
#define UHCI_PID_SETUP    0x2dU
#define UHCI_MAX_TDS      255U
#define UHCI_PCI_COMMAND  0x04U
#define UHCI_PCI_MASTER   0x0004U
#define UHCI_IRQ_DRAIN_TICKS 100U
#define UHCI_RETIRE_TICKS 100U
#define UHCI_QUIESCE_TICKS 100U
#define UHCI_ADVANCE_POLL_TICKS 1U
/* The kernel clock is 100 Hz, so ten ticks are a bounded 100-ms poll. */
#define UHCI_ROOT_POLL_TICKS 10U
#define UHCI_PERIODIC_LEVELS 8U
#define UHCI_MAX_PERIODIC_INTERVAL (1U << (UHCI_PERIODIC_LEVELS - 1U))
#define UHCI_ASYNC_SKELETON UHCI_PERIODIC_LEVELS
#define UHCI_SKELETON_COUNT (UHCI_PERIODIC_LEVELS + 1U)
#define UHCI_FRAME_BIT_TIMES 12000U
#define UHCI_ASYNC_RESERVE_BIT_TIMES 1200U
#define UHCI_PERIODIC_BUDGET_BIT_TIMES \
	(UHCI_FRAME_BIT_TIMES - UHCI_ASYNC_RESERVE_BIT_TIMES)
#define UHCI_TRANSACTION_OVERHEAD_BITS 160U
#define UHCI_PORT_RW_BITS \
	(UHCI_PORT_PE | UHCI_PORT_RD | UHCI_PORT_RESET | UHCI_PORT_SUSPEND)
#define UHCI_PORT_CHANGE_BITS (UHCI_PORT_CSC | UHCI_PORT_PEC)

struct uhci_qh { volatile uint32_t head, element, reserved[2]; };
struct uhci_td { volatile uint32_t link, status, token, buffer; };

enum uhci_request_state {
	UHCI_REQUEST_ACTIVE,
	UHCI_REQUEST_WAIT_FRAME_COMPLETE,
	UHCI_REQUEST_WAIT_FRAME_CANCEL,
	UHCI_REQUEST_COMPLETING,
	UHCI_REQUEST_RETIRED_CANCEL,
	UHCI_REQUEST_FAILED
};

struct uhci_request {
	struct drv_usb_urb *urb;
	struct drv_usb_endpoint *endpoint;
	struct drv_dma_buffer schedule, bounce;
	struct uhci_qh *qh;
	struct uhci_td *tds;
	struct uhci_request *active_next;
	struct uhci_request *schedule_previous, *schedule_next;
	struct uhci_request *retirement_next;
	unsigned td_count, data_first, data_count;
	unsigned terminal_td_count;
	unsigned periodic_level, periodic_cost;
	unsigned advance_candidate;
	uint32_t advance_element, advance_status;
	uint16_t unlink_frame;
	uint16_t advance_frame;
	uint64_t advance_started_tick;
	enum uhci_request_state state;
	enum drv_usb_urb_status completion_status;
	int retirement_error;
	bool input, low_speed, periodic, scheduled, retirement_queued;
	bool periodic_reserved;
};

struct uhci_controller {
	struct drv_pci_device *pci;
	uint16_t io_base;
	unsigned bar_index;
	struct drv_pci_enable_state pci_enable_state;
	struct drv_dma_buffer frame_list;
	struct drv_dma_buffer skeleton_memory;
	struct uhci_qh *skeleton;
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct spinlock active_lock;
	struct uhci_request *active;
	struct uhci_request *periodic[UHCI_PERIODIC_LEVELS];
	struct uhci_request *asynchronous;
	struct uhci_request *retirement_head, *retirement_tail;
	struct thread *retirement_worker;
	struct thread *root_worker;
	unsigned bar_claimed, pci_state_saved, hcd_registered, irq_allocated;
	unsigned dma_quiesced, quiescing, submitting, active_count;
	unsigned completion_inflight;
	unsigned periodic_bit_times;
	unsigned listed, quarantined;
	volatile unsigned retirement_pending, retirement_stopping;
	unsigned retirement_joining;
	uint64_t retirement_wake_generation;
	int retirement_error;
	volatile unsigned root_stopping, root_force_scan;
	unsigned root_ready, root_joining;
	uint64_t root_wake_generation;
	uint16_t root_port_status[2];
	unsigned root_port_status_valid;
#ifdef ZEDBSD_TEST_CHECKPOINTS
	unsigned retirement_evidence;
	unsigned root_evidence;
	unsigned shutdown_evidence;
#endif
	struct uhci_controller *next;
};

static struct uhci_controller *uhci_controllers;

static int
uhci_retirement_worker_stop(struct uhci_controller *controller);
static int
uhci_quiesce_requests(struct uhci_controller *controller);
static int
uhci_root_worker_stop(struct uhci_controller *controller);
static void
uhci_root_worker_request_stop(struct uhci_controller *controller);
static int
uhci_hardware_stop(struct uhci_controller *controller, const char *context);

static uint8_t in8(uint16_t port)
{ uint8_t v; __asm__ volatile("inb %w1,%0":"=a"(v):"Nd"(port)); return v; }
static uint16_t in16(uint16_t port)
{ uint16_t v; __asm__ volatile("inw %w1,%0":"=a"(v):"Nd"(port)); return v; }
static void out8(uint16_t port,uint8_t v)
{ __asm__ volatile("outb %0,%w1"::"a"(v),"Nd"(port)); }
static void out16(uint16_t port,uint16_t v)
{ __asm__ volatile("outw %0,%w1"::"a"(v),"Nd"(port)); }
static void out32(uint16_t port,uint32_t v)
{ __asm__ volatile("outl %0,%w1"::"a"(v),"Nd"(port)); }

static struct uhci_controller *hcd_controller(struct drv_usb_hcd *hcd)
{ return (struct uhci_controller *)hcd->private_data[0]; }

static void io_pause(unsigned count)
{
	while (count-- != 0) (void)in8(0x80U);
}

static uint32_t
uhci_skeleton_link(struct uhci_controller *controller, unsigned index)
{
	return (uint32_t)(controller->skeleton_memory.device_address +
	    index * sizeof(*controller->skeleton)) | UHCI_LINK_QH;
}

static unsigned
uhci_frame_periodic_level(unsigned frame)
{
	unsigned level = 0;

	while (level + 1U < UHCI_PERIODIC_LEVELS && (frame & 1U) == 0) {
		level++;
		frame >>= 1;
	}
	return level;
}

static void
uhci_schedule_release(struct uhci_controller *controller)
{
	if (controller->skeleton_memory.address != NULL) {
		drv_dma_free_coherent(controller->hcd.dma,
		    &controller->skeleton_memory);
		memset(&controller->skeleton_memory, 0,
		    sizeof(controller->skeleton_memory));
		controller->skeleton = NULL;
	}
	if (controller->frame_list.address != NULL) {
		drv_dma_free_coherent(controller->hcd.dma,
		    &controller->frame_list);
		memset(&controller->frame_list, 0,
		    sizeof(controller->frame_list));
	}
}

static int
uhci_schedule_initialize(struct uhci_controller *controller)
{
	uint32_t *frames;
	unsigned index;
	int error;

	error = drv_dma_alloc_coherent(controller->hcd.dma, 4096U, 4096U,
	    &controller->frame_list);
	if (error != 0)
		return error;
	error = drv_dma_alloc_coherent(controller->hcd.dma,
	    UHCI_SKELETON_COUNT * sizeof(*controller->skeleton), 16U,
	    &controller->skeleton_memory);
	if (error != 0) {
		uhci_schedule_release(controller);
		return error;
	}
	controller->skeleton = controller->skeleton_memory.address;
	memset(controller->skeleton, 0,
	    UHCI_SKELETON_COUNT * sizeof(*controller->skeleton));
	for (index = 0; index < UHCI_SKELETON_COUNT; index++)
		controller->skeleton[index].element = UHCI_LINK_TERM;
	controller->skeleton[0].head = uhci_skeleton_link(controller,
	    UHCI_ASYNC_SKELETON);
	for (index = 1; index < UHCI_PERIODIC_LEVELS; index++)
		controller->skeleton[index].head =
		    uhci_skeleton_link(controller, index - 1U);
	controller->skeleton[UHCI_ASYNC_SKELETON].head = UHCI_LINK_TERM;
	frames = controller->frame_list.address;
	for (index = 0; index < 1024U; index++)
		frames[index] = uhci_skeleton_link(controller,
		    uhci_frame_periodic_level(index));
	hal_io_wmb();
	return 0;
}

static int
uhci_request_sets_empty_locked(struct uhci_controller *controller)
{
	unsigned level;

	if (controller->active != NULL || controller->active_count != 0 ||
	    controller->completion_inflight != 0 ||
	    controller->asynchronous != NULL ||
	    controller->retirement_head != NULL ||
	    controller->retirement_tail != NULL ||
	    controller->periodic_bit_times != 0)
		return 0;
	for (level = 0; level < UHCI_PERIODIC_LEVELS; level++)
		if (controller->periodic[level] != NULL)
			return 0;
	return 1;
}

static int
uhci_wait_running(struct uhci_controller *controller)
{
	uint64_t started;

	started = sched_ticks();
	for (;;) {
		uint16_t status = in16(controller->io_base + UHCI_USBSTS);

		hal_io_mb();
		if (status == UINT16_MAX ||
		    (status & (UHCI_STS_HOST_SYSTEM_ERROR |
		    UHCI_STS_PROCESS_ERROR)) != 0)
			return EIO;
		if ((status & UHCI_STS_HALTED) == 0)
			return 0;
		if (sched_ticks() - started >= UHCI_IRQ_DRAIN_TICKS)
			return ETIMEDOUT;
		sched_yield();
	}
}

static int uhci_start(struct drv_usb_hcd *hcd)
{
	struct uhci_controller *controller = hcd_controller(hcd);
	unsigned long irq;
	unsigned timeout;
	int error, run_started = 0, stop_error;

	error = uhci_schedule_initialize(controller);
	if (error != 0)
		return error;
	out16(controller->io_base + UHCI_USBCMD, UHCI_CMD_GRESET);
	io_pause(10000U);
	out16(controller->io_base + UHCI_USBCMD, 0);
	out16(controller->io_base + UHCI_USBCMD, UHCI_CMD_HCRESET);
	for (timeout = 0; timeout < 100000U; timeout++)
		if ((in16(controller->io_base + UHCI_USBCMD) & UHCI_CMD_HCRESET) == 0)
			break;
	if (timeout == 100000U) {
		uhci_schedule_release(controller);
		return ETIMEDOUT;
	}
	out16(controller->io_base + UHCI_USBINTR, 0);
	out16(controller->io_base + UHCI_USBSTS, UHCI_STS_ALL);
	out16(controller->io_base + UHCI_FRNUM, 0);
	out32(controller->io_base + UHCI_FLBASEADD,
	    (uint32_t)controller->frame_list.device_address);
	out8(controller->io_base + UHCI_SOFMOD, 64U);
	/* From this point the frame list may be controller-visible.  Publish the
	 * non-quiesced state before RUN so every failure path retains that graph
	 * until halt, bus-master disable, and IRQ drain have all been proved. */
	irq = spin_lock_irqsave(&controller->active_lock);
	controller->dma_quiesced = 0;
	controller->quiescing = 1;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	run_started = 1;
	out16(controller->io_base + UHCI_USBCMD, UHCI_CMD_CF | UHCI_CMD_RUN);
	error = uhci_wait_running(controller);
	if (error != 0)
		goto fail;
	{
		irq = spin_lock_irqsave(&controller->active_lock);

		controller->quiescing = 0;
		controller->submitting = 0;
		controller->active_count = 0;
		controller->completion_inflight = 0;
		controller->periodic_bit_times = 0;
		controller->active = NULL;
		controller->asynchronous = NULL;
		memset(controller->periodic, 0, sizeof(controller->periodic));
		controller->retirement_head = NULL;
		controller->retirement_tail = NULL;
		controller->retirement_pending = 0;
		controller->retirement_stopping = 0;
		controller->retirement_joining = 0;
		controller->retirement_error = 0;
#ifdef ZEDBSD_TEST_CHECKPOINTS
		controller->retirement_evidence = 0;
		controller->root_evidence = 0;
		controller->shutdown_evidence = 0;
#endif
		spin_unlock_irqrestore(&controller->active_lock, irq);
	}
	return 0;

fail:
	if (!run_started) {
		uhci_schedule_release(controller);
		return error;
	}
	irq = spin_lock_irqsave(&controller->active_lock);
	controller->quiescing = 1;
	controller->quarantined = 1;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	stop_error = uhci_hardware_stop(controller, "start failure");
	if (stop_error == 0)
		uhci_schedule_release(controller);
	else
		hal_printf(
		    "uhci: start failed (%d), checked DMA stop failed (%d); schedule retained\n",
		    error, stop_error);
	return error;
}

static void uhci_stop(struct drv_usb_hcd *hcd)
{
	struct uhci_controller *controller = hcd_controller(hcd);
	unsigned long irq;
	int releasable;

	irq = spin_lock_irqsave(&controller->active_lock);
	releasable = controller->dma_quiesced &&
	    uhci_request_sets_empty_locked(controller) &&
	    controller->submitting == 0 &&
	    controller->retirement_worker == NULL &&
	    !controller->retirement_joining &&
	    controller->root_worker == NULL && !controller->root_joining;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	if (!releasable) {
		hal_printf("uhci: refusing to release DMA before checked quiesce\n");
		return;
	}
	out16(controller->io_base + UHCI_USBCMD, 0);
	out16(controller->io_base + UHCI_USBINTR, 0);
	uhci_schedule_release(controller);
}

static int
uhci_bus_master_disable(struct uhci_controller *controller)
{
	uint16_t command;
	int error;

	error = drv_pci_device_set_bus_master(controller->pci, false);
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci,
	    UHCI_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	return (command & UHCI_PCI_MASTER) == 0 ? 0 : EIO;
}

static int
uhci_irq_disestablish(struct uhci_controller *controller)
{
	uint64_t started;
	int error;

	if (controller->irq_cookie == NULL)
		return 0;
	started = sched_ticks();
	for (;;) {
		error = drv_pci_device_disestablish_irq_checked(controller->pci,
		    controller->irq_cookie);
		if (error != EBUSY)
			break;
		if (sched_ticks() - started >= UHCI_IRQ_DRAIN_TICKS) {
			hal_printf(
			    "uhci: IRQ removal timed out; retaining controller resources\n");
			return EBUSY;
		}
		sched_yield();
	}
	if (error != 0) {
		hal_printf(
		    "uhci: checked IRQ removal failed (%d); retaining controller resources\n",
		    error);
		return error;
	}
	controller->irq_cookie = NULL;
	return 0;
}

static int
uhci_hardware_stop(struct uhci_controller *controller, const char *context)
{
	uint64_t started;
	uint16_t command, status;
	unsigned long irq;
	int bus_master_error, halt_error = 0, irq_error;

	out16(controller->io_base + UHCI_USBINTR, 0);
	command = in16(controller->io_base + UHCI_USBCMD);
	if (command == UINT16_MAX)
		halt_error = EIO;
	else {
		out16(controller->io_base + UHCI_USBCMD,
		    (uint16_t)(command & (uint16_t)~UHCI_CMD_RUN));
		started = sched_ticks();
		for (;;) {
			status = in16(controller->io_base + UHCI_USBSTS);
			hal_io_mb();
			if (status == UINT16_MAX) {
				halt_error = EIO;
				break;
			}
			if ((status & UHCI_STS_HALTED) != 0)
				break;
			if (sched_ticks() - started >= UHCI_IRQ_DRAIN_TICKS) {
				halt_error = ETIMEDOUT;
				break;
			}
			sched_yield();
		}
	}
	/* Even when the I/O register halt proof fails, cut off PCI DMA and drain
	 * the handler.  The ownership graph is still retained because all three
	 * proofs must succeed before dma_quiesced can be published. */
	bus_master_error = uhci_bus_master_disable(controller);
	irq_error = uhci_irq_disestablish(controller);
	if (halt_error != 0 || bus_master_error != 0 || irq_error != 0) {
		hal_printf(
		    "uhci: %s stop incomplete (halt=%d master=%d irq=%d); retaining controller ownership\n",
		    context, halt_error, bus_master_error, irq_error);
		if (halt_error != 0)
			return halt_error;
		if (bus_master_error != 0)
			return bus_master_error;
		return irq_error;
	}
	irq = spin_lock_irqsave(&controller->active_lock);
	controller->dma_quiesced = 1;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	return 0;
}

static int
uhci_report_shutdown_evidence(struct uhci_controller *controller)
{
	unsigned long irq;
	int ready;
#ifdef ZEDBSD_TEST_CHECKPOINTS
	int report = 0;
#endif

	irq = spin_lock_irqsave(&controller->active_lock);
	ready = controller->dma_quiesced &&
	    uhci_request_sets_empty_locked(controller) &&
	    controller->submitting == 0 &&
	    controller->retirement_worker == NULL &&
	    !controller->retirement_joining &&
	    controller->root_worker == NULL && !controller->root_joining;
#ifdef ZEDBSD_TEST_CHECKPOINTS
	if (ready && !controller->shutdown_evidence) {
		controller->shutdown_evidence = 1;
		report = 1;
	}
#endif
	spin_unlock_irqrestore(&controller->active_lock, irq);
	if (!ready)
		return EBUSY;
#ifdef ZEDBSD_TEST_CHECKPOINTS
	if (report)
		hal_printf("uhci: checked shutdown workers joined\n");
#endif
	return 0;
}

static int
uhci_wait_submissions(struct uhci_controller *controller)
{
	uint64_t started = sched_ticks();
	unsigned long irq;

	for (;;) {
		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->submitting == 0) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			return 0;
		}
		spin_unlock_irqrestore(&controller->active_lock, irq);
		if (sched_ticks() - started >= UHCI_QUIESCE_TICKS)
			return EBUSY;
		sched_yield();
	}
}

static int
uhci_quiesce(struct drv_usb_hcd *hcd)
{
	struct uhci_controller *controller = hcd_controller(hcd);
	unsigned long irq;
	int already_quiesced, builders_error, hardware_error;
	int requests_error, root_error, worker_error = 0;
	int requests_empty;

	irq = spin_lock_irqsave(&controller->active_lock);
	/* Close admission before joining builders.  A builder which entered earlier
	 * may finish allocating, but it cannot publish after this point. */
	controller->quiescing = 1;
	already_quiesced = controller->dma_quiesced != 0;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	builders_error = uhci_wait_submissions(controller);
	requests_error = uhci_quiesce_requests(controller);
	root_error = uhci_root_worker_stop(controller);

	irq = spin_lock_irqsave(&controller->active_lock);
	requests_empty = uhci_request_sets_empty_locked(controller) &&
	    controller->submitting == 0;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	/* Root topology work is now joined.  The clean request path has no remaining
	 * terminal owner, so join its worker before entering the controller-global
	 * DMA barrier.  A failed or incomplete local proof instead retains the
	 * request graph and retirement worker for diagnosis. */
	if (requests_error == 0 && requests_empty)
		worker_error = uhci_retirement_worker_stop(controller);

	/* A request-local retirement or worker-join failure must never bypass the
	 * global DMA cutoff.  Preserve that original error, but still mask IRQs,
	 * clear RUN, disable PCI bus mastering, and drain the checked IRQ owner. */
	hardware_error = already_quiesced ? 0 :
	    uhci_hardware_stop(controller, "quiesce");

	if (builders_error != 0)
		return builders_error;
	if (requests_error != 0)
		return requests_error;
	if (root_error != 0)
		return root_error;
	if (worker_error != 0)
		return worker_error;
	if (hardware_error != 0)
		return hardware_error;
	return uhci_report_shutdown_evidence(controller);
}

static uint32_t uhci_token(uint8_t pid,unsigned address,unsigned endpoint,
	unsigned toggle,unsigned length)
{
	uint32_t encoded_length=length==0?0x7ffU:length-1U;
	return pid|((uint32_t)address<<8)|((uint32_t)endpoint<<15)|
	    ((uint32_t)toggle<<19)|(encoded_length<<21);
}

static void uhci_request_free(struct uhci_controller*c,struct uhci_request*r)
{
	if(!r)return;
	if (r->periodic_reserved)
		__builtin_trap();
	if(r->schedule.address)drv_dma_free_coherent(c->hcd.dma,&r->schedule);
	if(r->bounce.address)drv_dma_free_coherent(c->hcd.dma,&r->bounce);
	hal_free(r);
}

static void
uhci_builder_leave(struct uhci_controller *controller)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->submitting == 0)
		__builtin_trap();
	controller->submitting--;
	spin_unlock_irqrestore(&controller->active_lock, irq);
}

static void
uhci_builder_discard(struct uhci_controller *controller,
	struct uhci_request *request)
{
	/* Keep the builder counted until its private request no longer borrows the
	 * controller DMA domain.  Quiesce may free the controller after observing
	 * submitting == 0, so no controller access may follow builder_leave(). */
	uhci_request_free(controller, request);
	uhci_builder_leave(controller);
}

static int uhci_add_td(struct uhci_request*r,uint8_t pid,unsigned address,
	unsigned endpoint,unsigned toggle,unsigned length,uint32_t buffer)
{
	struct uhci_td*td;uint32_t physical;
	if(r->td_count>=UHCI_MAX_TDS)return E2BIG;
	td=&r->tds[r->td_count];physical=(uint32_t)r->schedule.device_address+
	    16U+r->td_count*sizeof(*td);
	if(r->td_count!=0)r->tds[r->td_count-1U].link=physical|UHCI_LINK_DEPTH;
	td->link=UHCI_LINK_TERM;td->status=UHCI_TD_ACTIVE|(3U<<27)|
	    (r->low_speed ? UHCI_TD_LOW_SPEED : 0U);
	td->token=uhci_token(pid,address,endpoint,toggle,length);td->buffer=buffer;
	r->td_count++;return 0;
}

static unsigned
uhci_periodic_level(uint8_t interval)
{
	unsigned level = 0;
	unsigned period = 1;
	unsigned normalized = interval;

	if (normalized > UHCI_MAX_PERIODIC_INTERVAL)
		normalized = UHCI_MAX_PERIODIC_INTERVAL;

	while (level + 1U < UHCI_PERIODIC_LEVELS &&
	    period * 2U <= normalized) {
		period *= 2U;
		level++;
	}
	return level;
}

static int
uhci_endpoint_parameters(struct drv_usb_endpoint *endpoint,
	enum drv_usb_speed speed, enum drv_usb_transfer_type type,
	unsigned *packet_result, unsigned *number_result)
{
	const struct drv_usb_endpoint_descriptor *descriptor;
	unsigned address, number, packet;

	descriptor = drv_usb_endpoint_descriptor(endpoint);
	if (descriptor == NULL)
		return EINVAL;
	address = drv_usb_endpoint_address(endpoint);
	number = address & 0x0fU;
	/* Bits 6:4 of bEndpointAddress and bits 15:11 of wMaxPacketSize are
	 * reserved for USB 1.1 endpoints.  In particular, the high-bandwidth
	 * transaction multiplier is not meaningful on UHCI. */
	if ((address & 0x70U) != 0 ||
	    (descriptor->maximum_packet_size & 0xf800U) != 0)
		return EINVAL;
	packet = descriptor->maximum_packet_size & 0x07ffU;
	if (packet == 0)
		return EINVAL;

	switch (type) {
	case DRV_USB_TRANSFER_CONTROL:
		if (number != 0)
			return EINVAL;
		if ((speed == DRV_USB_SPEED_LOW && packet != 8U) ||
		    (speed == DRV_USB_SPEED_FULL && packet != 8U &&
		    packet != 16U && packet != 32U && packet != 64U))
			return EINVAL;
		break;
	case DRV_USB_TRANSFER_BULK:
		if (speed != DRV_USB_SPEED_FULL || number == 0 ||
		    (packet != 8U && packet != 16U && packet != 32U &&
		    packet != 64U))
			return EINVAL;
		break;
	case DRV_USB_TRANSFER_INTERRUPT:
		if (number == 0 ||
		    descriptor->interval == 0 ||
		    (speed == DRV_USB_SPEED_LOW && descriptor->interval < 10U) ||
		    (speed == DRV_USB_SPEED_LOW && packet > 8U) ||
		    (speed == DRV_USB_SPEED_FULL && packet > 64U))
			return EINVAL;
		break;
	default:
		return ENOTSUP;
	}
	*packet_result = packet;
	*number_result = number;
	return 0;
}

static int
uhci_required_td_count(enum drv_usb_transfer_type type, size_t length,
	unsigned packet, unsigned *result)
{
	size_t data_count, total;

	data_count = length / packet + (length % packet != 0 ? 1U : 0U);
	if (type == DRV_USB_TRANSFER_CONTROL) {
		if (data_count > UHCI_MAX_TDS - 2U)
			return E2BIG;
		total = data_count + 2U;
	} else {
		total = data_count != 0 ? data_count : 1U;
		if (total > UHCI_MAX_TDS)
			return E2BIG;
	}
	*result = (unsigned)total;
	return 0;
}

static int
uhci_periodic_cost(size_t length, unsigned packet, int low_speed,
	unsigned *result)
{
	size_t offset = 0;
	unsigned total = 0;

	do {
		unsigned chunk = length - offset > packet ? packet :
		    (unsigned)(length - offset);
		unsigned bits = UHCI_TRANSACTION_OVERHEAD_BITS + chunk * 8U;
		unsigned cost = (bits * 7U + 5U) / 6U;

		if (low_speed)
			cost *= 8U;
		if (total > UINT_MAX - cost)
			return EOVERFLOW;
		total += cost;
		offset += chunk;
	} while (offset < length);
	*result = total;
	return 0;
}

static int uhci_build_request(struct uhci_controller*c,struct drv_usb_urb*urb,
	struct uhci_request**result)
{
	struct uhci_request *r;
	struct drv_usb_endpoint *ep;
	struct drv_usb_device *device;
	const struct drv_usb_endpoint_descriptor *descriptor;
	const struct drv_usb_control_request *control;
	enum drv_usb_transfer_type type;
	enum drv_usb_speed speed;
	size_t length, offset = 0;
	unsigned packet, address, endpoint, toggle = 0, required_tds;
	int error;

	if (urb == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
	ep = drv_usb_urb_endpoint(urb);
	device = drv_usb_urb_device(urb);
	control = drv_usb_urb_control_request(urb);
	if (ep == NULL || device == NULL)
		return EINVAL;
	length = drv_usb_urb_length(urb);
	if (length > SIZE_MAX - 8U ||
	    (length != 0 && drv_usb_urb_buffer(urb) == NULL))
		return EINVAL;
	type = drv_usb_endpoint_type(ep);
	speed = drv_usb_device_speed(device);
	if (speed != DRV_USB_SPEED_LOW && speed != DRV_USB_SPEED_FULL)
		return ENOTSUP;
	if (type == DRV_USB_TRANSFER_ISOCHRONOUS ||
	    (type == DRV_USB_TRANSFER_BULK && speed == DRV_USB_SPEED_LOW))
		return ENOTSUP;
	if ((control != NULL) != (type == DRV_USB_TRANSFER_CONTROL))
		return EINVAL;
	descriptor = drv_usb_endpoint_descriptor(ep);
	if (type == DRV_USB_TRANSFER_INTERRUPT &&
	    (descriptor == NULL || descriptor->interval == 0))
		return EINVAL;
	error = uhci_endpoint_parameters(ep, speed, type, &packet, &endpoint);
	if (error != 0)
		return error;
	if (type == DRV_USB_TRANSFER_INTERRUPT && length > packet)
		return E2BIG;
	error = uhci_required_td_count(type, length, packet, &required_tds);
	if (error != 0)
		return error;

	r = hal_malloc(sizeof(*r));
	if (r == NULL)
		return ENOMEM;
	memset(r, 0, sizeof(*r));
	r->urb = urb;
	r->endpoint = ep;
	r->state = UHCI_REQUEST_ACTIVE;
	r->low_speed = speed == DRV_USB_SPEED_LOW;
	r->periodic = type == DRV_USB_TRANSFER_INTERRUPT;
	if (r->periodic) {
		r->periodic_level = uhci_periodic_level(descriptor->interval);
		error = uhci_periodic_cost(length, packet, r->low_speed,
		    &r->periodic_cost);
		if (error != 0) {
			hal_free(r);
			return error;
		}
	}
	error = drv_dma_alloc_coherent(c->hcd.dma, 4096U, 16U,
	    &r->schedule);
	if (error != 0) {
		hal_free(r);
		return error;
	}
	error = drv_dma_alloc_coherent(c->hcd.dma, length + 8U, 16U,
	    &r->bounce);
	if (error != 0) {
		uhci_request_free(c, r);
		return error;
	}
	if (r->schedule.device_address > UINT32_MAX ||
	    r->schedule.device_address + r->schedule.size - 1U > UINT32_MAX ||
	    r->bounce.device_address > UINT32_MAX ||
	    r->bounce.device_address + r->bounce.size - 1U > UINT32_MAX) {
		error = EOVERFLOW;
		goto fail;
	}
	memset(r->schedule.address, 0, 4096U);
	r->qh = r->schedule.address;
	r->tds = (struct uhci_td *)((uint8_t *)r->schedule.address + 16U);
	address = drv_usb_device_address(device);
	if (address > DRV_USB_MAX_ADDRESS) {
		error = EINVAL;
		goto fail;
	}
	if (control != NULL) {
		memcpy(r->bounce.address, control, sizeof(*control));
		error = uhci_add_td(r, UHCI_PID_SETUP, address, 0, 0, 8U,
		    (uint32_t)r->bounce.device_address);
		if (error != 0)
			goto fail;
		r->input = (control->request_type & DRV_USB_DIR_IN) != 0;
		if (!r->input && length != 0)
			memcpy((uint8_t *)r->bounce.address + 8U,
			    drv_usb_urb_buffer(urb), length);
		r->data_first = r->td_count;
		toggle = 1;
		while (offset < length) {
			unsigned chunk = length - offset > packet ? packet :
			    (unsigned)(length - offset);

			error = uhci_add_td(r, r->input ? UHCI_PID_IN :
			    UHCI_PID_OUT, address, 0, toggle, chunk,
			    (uint32_t)(r->bounce.device_address + 8U + offset));
			if (error != 0)
				goto fail;
			offset += chunk;
			toggle ^= 1U;
			r->data_count++;
		}
		error = uhci_add_td(r, r->input ? UHCI_PID_OUT : UHCI_PID_IN,
		    address, 0, 1, 0, 0);
		if (error != 0)
			goto fail;
	} else {
		r->input = drv_usb_endpoint_is_input(ep);
		r->data_first = 0;
		toggle = (unsigned)drv_usb_endpoint_hcd_data(ep, 0) & 1U;
		if (!r->input && length != 0)
			memcpy((uint8_t *)r->bounce.address + 8U,
			    drv_usb_urb_buffer(urb), length);
		while (offset < length) {
			unsigned chunk = length - offset > packet ? packet :
			    (unsigned)(length - offset);

			error = uhci_add_td(r, r->input ? UHCI_PID_IN :
			    UHCI_PID_OUT, address, endpoint, toggle, chunk,
			    (uint32_t)(r->bounce.device_address + 8U + offset));
			if (error != 0)
				goto fail;
			offset += chunk;
			toggle ^= 1U;
			r->data_count++;
		}
		if (length == 0) {
			error = uhci_add_td(r, r->input ? UHCI_PID_IN :
			    UHCI_PID_OUT, address, endpoint, toggle, 0, 0);
			if (error != 0)
				goto fail;
		}
		r->data_count = r->td_count;
	}
	if (r->td_count != required_tds)
		__builtin_trap();
	r->tds[r->td_count - 1U].status |= UHCI_TD_IOC;
	r->qh->head = UHCI_LINK_TERM;
	r->qh->element = (uint32_t)r->schedule.device_address + 16U;
	*result = r;
	return 0;

fail:
	uhci_request_free(c, r);
	return error;
}

static void
uhci_request_advance_clear(struct uhci_request *request)
{
	request->advance_candidate = 0;
	request->advance_element = 0;
	request->advance_status = 0;
	request->advance_frame = 0;
	request->advance_started_tick = 0;
}

static int
uhci_progress_frame_sample(struct uhci_controller *controller,
	uint16_t *frame_result)
{
	uint16_t command, frame, status;

	frame = in16(controller->io_base + UHCI_FRNUM);
	status = in16(controller->io_base + UHCI_USBSTS);
	command = in16(controller->io_base + UHCI_USBCMD);
	hal_io_mb();
	if (frame == UINT16_MAX || status == UINT16_MAX ||
	    command == UINT16_MAX ||
	    (frame & (uint16_t)~UHCI_FRNUM_MASK) != 0 ||
	    (command & UHCI_CMD_RUN) == 0 ||
	    (status & (UHCI_STS_HOST_SYSTEM_ERROR |
	    UHCI_STS_PROCESS_ERROR | UHCI_STS_HALTED)) != 0)
		return EIO;
	*frame_result = frame;
	return 0;
}

/* Return one only for an inactive successful TD for which UHCI should have
 * copied the TD link into this request's QH element.  Return EIO for a link
 * which cannot belong to the immutable request-local TD chain. */
static int
uhci_request_advance_snapshot(struct uhci_request *request,
	unsigned *index_result, uint32_t *element_result,
	uint32_t *status_result, uint32_t *link_result)
{
	uint32_t base, element, link, next_status, physical, status, token;
	unsigned actual, expected, index, offset;

	hal_io_rmb();
	element = request->qh->element;
	if ((element & UHCI_LINK_TERM) != 0)
		return 0;
	if ((element & (UHCI_LINK_QH | UHCI_LINK_RESERVED)) != 0)
		return EIO;
	base = (uint32_t)request->schedule.device_address + 16U;
	physical = element & UHCI_LINK_ADDRESS;
	if (physical < base)
		return EIO;
	offset = physical - base;
	if (offset % sizeof(*request->tds) != 0)
		return EIO;
	index = offset / sizeof(*request->tds);
	if (index >= request->td_count)
		return EIO;
	status = request->tds[index].status;
	if ((status & (UHCI_TD_ACTIVE | UHCI_TD_ERRORS)) != 0)
		return 0;
	link = request->tds[index].link;
	if ((link & UHCI_LINK_TERM) != 0)
		return 0;
	if ((link & (UHCI_LINK_QH | UHCI_LINK_RESERVED)) != 0 ||
	    index + 1U >= request->td_count ||
	    (link & UHCI_LINK_ADDRESS) !=
	    (uint32_t)(request->schedule.device_address + 16U +
	    (index + 1U) * sizeof(*request->tds)))
		return EIO;
	/* The PIIX erratum leaves the QH exactly at the boundary between the
	 * completed TD and the first unexecuted TD.  If the successor is no longer
	 * active, this is not that erratum: advancing could replay an already
	 * completed transfer, so fail closed instead. */
	next_status = request->tds[index + 1U].status;
	if ((next_status & UHCI_TD_ACTIVE) == 0)
		return EIO;

	/* SPD deliberately leaves the QH element unchanged after a short IN.
	 * The current builder never sets SPD, but keeping that distinction here
	 * prevents the PIIX repair from defeating the later short-IN contract. */
	if ((status & UHCI_TD_SHORT_PACKET) != 0) {
		token = request->tds[index].token;
		if ((token & 0xffU) != UHCI_PID_IN)
			return EIO;
		actual = ((status & 0x7ffU) + 1U) & 0x7ffU;
		expected = (((token >> 21) & 0x7ffU) + 1U) & 0x7ffU;
		if (actual < expected)
			return 0;
		if (actual > expected)
			return EIO;
	}
	*index_result = index;
	*element_result = element;
	*status_result = status;
	*link_result = link;
	return 1;
}

/* active_lock protects request lifetime and excludes software unlink while
 * hardware-owned QH/TD state is observed.  A fresh healthy FRNUM after the
 * first snapshot closes the normal status-before-QH-write window. */
static int
uhci_request_qh_progress_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	uint32_t element, link, status;
	uint16_t frame;
	unsigned index;
	int candidate, error;

	if (request->state != UHCI_REQUEST_ACTIVE || !request->scheduled) {
		uhci_request_advance_clear(request);
		return 0;
	}
	if (request->advance_candidate != 0) {
		error = uhci_progress_frame_sample(controller, &frame);
		if (error != 0)
			return error;
		candidate = uhci_request_advance_snapshot(request, &index,
		    &element, &status, &link);
		if (candidate > 1)
			return candidate;
		if (candidate == 1 &&
		    request->advance_candidate == index + 1U &&
		    request->advance_element == element &&
		    request->advance_status == status) {
			if (frame != request->advance_frame) {
				request->qh->element = link;
				hal_io_wmb();
				uhci_request_advance_clear(request);
				return 0;
			}
			if (sched_ticks() - request->advance_started_tick >=
			    UHCI_RETIRE_TICKS)
				return ETIMEDOUT;
			return 0;
		}
		uhci_request_advance_clear(request);
		if (candidate == 0)
			return 0;
	}

	candidate = uhci_request_advance_snapshot(request, &index, &element,
	    &status, &link);
	if (candidate > 1)
		return candidate;
	if (candidate == 0)
		return 0;
	/* This frame is intentionally sampled after the coherent-memory snapshot;
	 * otherwise a boundary between the two observations could be mistaken for
	 * evidence that the QH writeback window has closed. */
	error = uhci_progress_frame_sample(controller, &frame);
	if (error != 0)
		return error;
	request->advance_candidate = index + 1U;
	request->advance_element = element;
	request->advance_status = status;
	request->advance_frame = frame;
	request->advance_started_tick = sched_ticks();
	return 0;
}

static int
uhci_request_terminal(struct uhci_request *request,
	enum drv_usb_urb_status *result, unsigned *terminal_td_count)
{
	unsigned index;

	hal_io_rmb();
	for (index = 0; index < request->td_count; index++) {
		uint32_t status = request->tds[index].status;

		/* A retryable error, including NAK, can coexist with ACTIVE.  Only
		 * an inactive TD with terminal error bits stops the remaining chain. */
		if ((status & UHCI_TD_ACTIVE) != 0)
			return 0;
		if ((status & UHCI_TD_ERRORS) != 0) {
			*result = (status & UHCI_TD_STALLED) != 0 ?
			    DRV_USB_URB_STALL : DRV_USB_URB_IO_ERROR;
			*terminal_td_count = index + 1U;
			return 1;
		}
	}
	*result = DRV_USB_URB_COMPLETE;
	*terminal_td_count = request->td_count;
	return 1;
}

static size_t
uhci_request_actual(struct uhci_request *request)
{
	size_t actual = 0;
	size_t requested = drv_usb_urb_length(request->urb);
	unsigned index;

	for (index = 0; index < request->data_count; index++) {
		unsigned td_index = request->data_first + index;
		uint32_t status;
		size_t count;

		if (td_index >= request->terminal_td_count)
			break;
		status = request->tds[td_index].status;
		if ((status & UHCI_TD_ACTIVE) != 0)
			break;
		count = status & 0x7ffU;
		if (count == 0x7ffU)
			continue;
		count++;
		if (count > requested - actual) {
			actual = requested;
			break;
		}
		actual += count;
	}
	return actual;
}

static void
uhci_request_commit_toggle(struct uhci_request *request)
{
	unsigned index;
	unsigned next_toggle = 0;
	int advanced = 0;

	if (drv_usb_urb_control_request(request->urb) != NULL)
		return;
	for (index = 0; index < request->data_count; index++) {
		unsigned td_index = request->data_first + index;
		uint32_t status;
		uint32_t token;

		if (td_index >= request->td_count)
			break;
		status = request->tds[td_index].status;
		if ((status & (UHCI_TD_ACTIVE | UHCI_TD_ERRORS)) != 0)
			break;
		token = request->tds[td_index].token;
		next_toggle = ((token >> 19) & 1U) ^ 1U;
		advanced = 1;
	}
	if (advanced)
		(void)drv_usb_endpoint_set_hcd_data(
		    drv_usb_urb_endpoint(request->urb), 0, next_toggle);
}

static void
uhci_request_prepare_toggle(struct uhci_request *request)
{
	unsigned index;
	unsigned toggle;

	if (drv_usb_urb_control_request(request->urb) != NULL)
		return;
	toggle = (unsigned)drv_usb_endpoint_hcd_data(request->endpoint, 0) & 1U;
	for (index = 0; index < request->data_count; index++) {
		unsigned td_index = request->data_first + index;
		uint32_t token;

		if (td_index >= request->td_count)
			__builtin_trap();
		token = request->tds[td_index].token;
		token &= ~(1U << 19);
		token |= (uint32_t)toggle << 19;
		request->tds[td_index].token = token;
		toggle ^= 1U;
	}
}

static uint32_t
uhci_request_link(const struct uhci_request *request)
{
	return (uint32_t)request->schedule.device_address | UHCI_LINK_QH;
}

static struct uhci_request **
uhci_schedule_head(struct uhci_controller *controller,
	struct uhci_request *request, struct uhci_qh **anchor)
{
	if (request->periodic) {
		*anchor = &controller->skeleton[request->periodic_level];
		return &controller->periodic[request->periodic_level];
	}
	*anchor = &controller->skeleton[UHCI_ASYNC_SKELETON];
	return &controller->asynchronous;
}

static uint32_t
uhci_schedule_tail_link(struct uhci_controller *controller,
	const struct uhci_request *request)
{
	if (!request->periodic)
		return UHCI_LINK_TERM;
	if (request->periodic_level == 0)
		return uhci_skeleton_link(controller, UHCI_ASYNC_SKELETON);
	return uhci_skeleton_link(controller, request->periodic_level - 1U);
}

static int
uhci_endpoint_owned_locked(struct uhci_controller *controller,
	const struct drv_usb_endpoint *endpoint)
{
	struct uhci_request *request;

	for (request = controller->active; request != NULL;
	    request = request->active_next)
		if (request->endpoint == endpoint)
			return 1;
	return 0;
}

static int
uhci_request_active_locked(struct uhci_controller *controller,
	const struct uhci_request *target)
{
	struct uhci_request *request;

	for (request = controller->active; request != NULL;
	    request = request->active_next)
		if (request == target)
			return 1;
	return 0;
}

static int
uhci_periodic_admit_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	if (!request->periodic)
		return 0;
	if (request->periodic_reserved)
		__builtin_trap();
	/* Do not phase-balance endpoints: charge every periodic request against
	 * the same worst-case frame.  The remaining ten percent of a USB 1.1
	 * frame is therefore always available to the asynchronous skeleton. */
	if (request->periodic_cost > UHCI_PERIODIC_BUDGET_BIT_TIMES ||
	    controller->periodic_bit_times >
	    UHCI_PERIODIC_BUDGET_BIT_TIMES - request->periodic_cost)
		return ENOSPC;
	controller->periodic_bit_times += request->periodic_cost;
	request->periodic_reserved = true;
	return 0;
}

static void
uhci_periodic_release_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	if (!request->periodic_reserved)
		return;
	if (!request->periodic || controller->periodic_bit_times <
	    request->periodic_cost)
		__builtin_trap();
	controller->periodic_bit_times -= request->periodic_cost;
	request->periodic_reserved = false;
}

static void
uhci_active_insert_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	if (controller->active_count == UINT_MAX)
		__builtin_trap();
	request->active_next = controller->active;
	controller->active = request;
	controller->active_count++;
}

static void
uhci_active_remove_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	struct uhci_request **link;

	for (link = &controller->active; *link != NULL;
	    link = &(*link)->active_next)
		if (*link == request) {
			*link = request->active_next;
			request->active_next = NULL;
			if (controller->active_count == 0)
				__builtin_trap();
			controller->active_count--;
			return;
		}
	__builtin_trap();
}

static void
uhci_schedule_insert_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	struct uhci_request **head;
	struct uhci_qh *anchor;

	if (request->scheduled)
		__builtin_trap();
	head = uhci_schedule_head(controller, request, &anchor);
	request->schedule_previous = NULL;
	request->schedule_next = *head;
	request->qh->head = *head != NULL ? uhci_request_link(*head) :
	    uhci_schedule_tail_link(controller, request);
	if (*head != NULL)
		(*head)->schedule_previous = request;
	hal_io_wmb();
	anchor->head = uhci_request_link(request);
	*head = request;
	request->scheduled = true;
	hal_io_wmb();
}

static void
uhci_schedule_unlink_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	struct uhci_request **head;
	struct uhci_qh *anchor;
	uint32_t successor;

	if (!request->scheduled)
		__builtin_trap();
	head = uhci_schedule_head(controller, request, &anchor);
	successor = request->qh->head;
	if (request->schedule_previous != NULL)
		request->schedule_previous->qh->head = successor;
	else {
		if (*head != request)
			__builtin_trap();
		anchor->head = successor;
		*head = request->schedule_next;
	}
	if (request->schedule_next != NULL)
		request->schedule_next->schedule_previous =
		    request->schedule_previous;
	request->schedule_previous = NULL;
	request->schedule_next = NULL;
	request->scheduled = false;
	/* Keep the removed QH's horizontal link intact until the checked frame
	 * boundary.  Hardware may already be traversing that QH and must still be
	 * able to reach every unrelated successor. */
	hal_io_wmb();
}

static void
uhci_retirement_enqueue_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	if (request->retirement_queued)
		__builtin_trap();
	request->retirement_next = NULL;
	if (controller->retirement_tail != NULL)
		controller->retirement_tail->retirement_next = request;
	else
		controller->retirement_head = request;
	controller->retirement_tail = request;
	request->retirement_queued = true;
}

static void
uhci_retirement_remove_locked(struct uhci_controller *controller,
	struct uhci_request *request)
{
	struct uhci_request **link;
	struct uhci_request *previous = NULL;

	if (!request->retirement_queued)
		__builtin_trap();
	for (link = &controller->retirement_head; *link != NULL;
	    link = &(*link)->retirement_next) {
		if (*link == request) {
			*link = request->retirement_next;
			if (controller->retirement_tail == request)
				controller->retirement_tail = previous;
			request->retirement_next = NULL;
			request->retirement_queued = false;
			return;
		}
		previous = *link;
	}
	__builtin_trap();
}

static void
uhci_retirement_begin_locked(struct uhci_controller *controller,
	struct uhci_request *request, enum uhci_request_state state)
{
	if (request->state != UHCI_REQUEST_ACTIVE)
		__builtin_trap();
	uhci_request_advance_clear(request);
	request->state = state;
	uhci_schedule_unlink_locked(controller, request);
	uhci_retirement_enqueue_locked(controller, request);
	/* The FRNUM snapshot follows publication of this request's schedule unlink.
	 * A later, different FRNUM is the only successful DMA-retirement proof. */
	hal_io_wmb();
	/* Preserve the raw register value.  Masking an absent-device 0xffff read
	 * here could turn it into a plausible frame number and later false proof. */
	request->unlink_frame = in16(controller->io_base + UHCI_FRNUM);
}

static int
uhci_wait_frame_advance(struct uhci_controller *controller,
	uint16_t unlink_frame)
{
	uint64_t started = sched_ticks();

	for (;;) {
		uint16_t frame = in16(controller->io_base + UHCI_FRNUM);
		uint16_t status;
		uint16_t command;

		status = in16(controller->io_base + UHCI_USBSTS);
		command = in16(controller->io_base + UHCI_USBCMD);
		hal_io_mb();
		if (controller->retirement_stopping ||
		    controller->quarantined ||
		    unlink_frame == UINT16_MAX || frame == UINT16_MAX ||
		    status == UINT16_MAX || command == UINT16_MAX ||
		    (unlink_frame & (uint16_t)~UHCI_FRNUM_MASK) != 0 ||
		    (frame & (uint16_t)~UHCI_FRNUM_MASK) != 0 ||
		    (command & UHCI_CMD_RUN) == 0 ||
		    (status & (UHCI_STS_HOST_SYSTEM_ERROR |
		    UHCI_STS_PROCESS_ERROR | UHCI_STS_HALTED)) != 0)
			return EIO;
		/* Health must be established before a changed frame can prove that
		 * hardware crossed the frame-list unlink boundary. */
		if (frame != unlink_frame)
			return 0;
		if (sched_ticks() - started >= UHCI_RETIRE_TICKS)
			return ETIMEDOUT;
		sched_yield();
	}
}

static void
uhci_retirement_fail(struct uhci_controller *controller,
	struct uhci_request *request, enum uhci_request_state expected, int error)
{
	unsigned long irq;
	int report = 0;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (uhci_request_active_locked(controller, request) &&
	    request->state == expected) {
		if (request->retirement_queued)
			uhci_retirement_remove_locked(controller, request);
		request->retirement_error = error;
		if (controller->retirement_error == 0)
			controller->retirement_error = error;
		request->state = UHCI_REQUEST_FAILED;
		controller->quiescing = 1;
		controller->quarantined = 1;
		report = 1;
	}
	spin_unlock_irqrestore(&controller->active_lock, irq);
	if (report) {
		uhci_root_worker_request_stop(controller);
		hal_printf(
		    "uhci: frame retirement failed (%d); retaining QH/TD/bounce DMA\n",
		    error);
	}
}

static void
uhci_finish_completion(struct uhci_controller *controller,
	struct uhci_request *request)
{
	struct drv_usb_urb *urb = request->urb;
	enum drv_usb_urb_status completion_status = request->completion_status;
	size_t actual;
	unsigned long irq;

	/* FRNUM has advanced since the unlink snapshot, so neither descriptors
	 * nor the bounce buffer can still be reached by this controller. */
	hal_io_rmb();
	actual = uhci_request_actual(request);
	uhci_request_commit_toggle(request);
	if (request->input && actual != 0)
		memcpy(drv_usb_urb_buffer(urb),
		    (uint8_t *)request->bounce.address + 8U, actual);

	irq = spin_lock_irqsave(&controller->active_lock);
	if (!uhci_request_active_locked(controller, request) ||
	    request->state != UHCI_REQUEST_COMPLETING ||
	    request->retirement_queued || request->scheduled ||
	    drv_usb_urb_hcd_data(urb) != request) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}
	uhci_periodic_release_locked(controller, request);
	if (controller->completion_inflight == UINT_MAX) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}
	controller->completion_inflight++;
	uhci_active_remove_locked(controller, request);
	(void)drv_usb_urb_set_hcd_data(urb, NULL);
	spin_unlock_irqrestore(&controller->active_lock, irq);
	uhci_request_free(controller, request);
	drv_usb_hcd_complete(&controller->hcd, urb, completion_status, actual);
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->completion_inflight == 0) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}
	controller->completion_inflight--;
	spin_unlock_irqrestore(&controller->active_lock, irq);
}

static void
uhci_retirement_process_request(struct uhci_controller *controller,
	struct uhci_request *request)
{
	enum uhci_request_state expected;
	uint16_t unlink_frame;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (!uhci_request_active_locked(controller, request) ||
	    !request->retirement_queued ||
	    (request->state != UHCI_REQUEST_WAIT_FRAME_COMPLETE &&
	    request->state != UHCI_REQUEST_WAIT_FRAME_CANCEL)) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return;
	}
	expected = request->state;
	unlink_frame = request->unlink_frame;
	spin_unlock_irqrestore(&controller->active_lock, irq);

	error = uhci_wait_frame_advance(controller, unlink_frame);
	if (error != 0) {
		uhci_retirement_fail(controller, request, expected, error);
		return;
	}
#ifdef ZEDBSD_TEST_CHECKPOINTS
	if (!__atomic_exchange_n(&controller->retirement_evidence, 1U,
	    __ATOMIC_RELAXED))
		hal_printf("uhci: checked frame retirement active\n");
#endif

	irq = spin_lock_irqsave(&controller->active_lock);
	if (!uhci_request_active_locked(controller, request) ||
	    !request->retirement_queued || request->state != expected) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return;
	}
	uhci_retirement_remove_locked(controller, request);
	if (expected == UHCI_REQUEST_WAIT_FRAME_CANCEL) {
		request->state = UHCI_REQUEST_RETIRED_CANCEL;
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return;
	}
	request->state = UHCI_REQUEST_COMPLETING;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	uhci_finish_completion(controller, request);
}

static int
uhci_qh_progress_watchdog(struct uhci_controller *controller)
{
	struct uhci_request *request;
	unsigned long irq;
	uint16_t command;
	int active = 0, error = 0, report = 0;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->quarantined || controller->retirement_stopping) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return 0;
	}
	for (request = controller->active; request != NULL;
	    request = request->active_next) {
		if (request->state != UHCI_REQUEST_ACTIVE || !request->scheduled)
			continue;
		active = 1;
		error = uhci_request_qh_progress_locked(controller, request);
		if (error != 0)
			break;
	}
	if (error != 0) {
		report = !controller->quarantined;
		controller->quiescing = 1;
		controller->quarantined = 1;
		active = 0;
	}
	spin_unlock_irqrestore(&controller->active_lock, irq);
	if (error != 0) {
		uhci_root_worker_request_stop(controller);
		out16(controller->io_base + UHCI_USBINTR, 0);
		command = in16(controller->io_base + UHCI_USBCMD);
		if (command != UINT16_MAX)
			out16(controller->io_base + UHCI_USBCMD,
			    command & (uint16_t)~UHCI_CMD_RUN);
		if (report)
			hal_printf(
			    "uhci: QH advance observation failed (%d); controller quarantined with DMA retained\n",
			    error);
	}
	return active;
}

static void
uhci_retirement_process(struct uhci_controller *controller)
{
	for (;;) {
		struct uhci_request *request;
		unsigned long irq;

		irq = spin_lock_irqsave(&controller->active_lock);
		request = controller->retirement_head;
		spin_unlock_irqrestore(&controller->active_lock, irq);
		if (request == NULL)
			return;
		uhci_retirement_process_request(controller, request);
	}
}

static void
uhci_retirement_worker(void *argument)
{
	struct uhci_controller *controller = argument;

	for (;;) {
		uint64_t observed_generation;
		unsigned long irq;
		int active;

		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->retirement_stopping) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			return;
		}
		observed_generation = controller->retirement_wake_generation;
		spin_unlock_irqrestore(&controller->active_lock, irq);
		if (__atomic_exchange_n(&controller->retirement_pending, 0U,
		    __ATOMIC_ACQ_REL))
			uhci_retirement_process(controller);
		active = uhci_qh_progress_watchdog(controller);
		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->retirement_stopping) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			return;
		}
		if (controller->retirement_wake_generation !=
		    observed_generation) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			continue;
		}
		sched_sleep_locked(active ?
		    sched_ticks() + UHCI_ADVANCE_POLL_TICKS : 0,
		    &controller->active_lock);
		spin_unlock_irqrestore(&controller->active_lock, irq);
	}
}

static int
uhci_retirement_worker_start(struct uhci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_worker != NULL ||
	    controller->retirement_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EALREADY;
	}
	__atomic_store_n(&controller->retirement_stopping, 0U,
	    __ATOMIC_RELEASE);
	__atomic_store_n(&controller->retirement_pending, 0U,
	    __ATOMIC_RELEASE);
	controller->retirement_wake_generation = 1;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	error = kthread_create(uhci_retirement_worker, controller,
	    SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->retirement_worker != NULL ||
	    controller->retirement_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}
	controller->retirement_worker = worker;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	thread_start(worker);
	return 0;
}

static int
uhci_retirement_worker_stop(struct uhci_controller *controller)
{
	struct thread *worker;
	uint64_t started;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);
	worker = controller->retirement_worker;
	if (worker == NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return 0;
	}
	/* A completion callback runs on this worker and may re-enter checked
	 * teardown.  It cannot synchronously join itself; retain the stopped HCD
	 * and let a later external teardown retry perform the join. */
	if (controller->retirement_joining || worker == curthread ||
	    controller->retirement_head != NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EBUSY;
	}
	controller->retirement_joining = 1;
	/* Joining blocks every notifier, while this reference keeps the published
	 * worker pointer valid across a successful thread_wait() reap. */
	thread_ref(worker);
	__atomic_store_n(&controller->retirement_stopping, 1U,
	    __ATOMIC_RELEASE);
	controller->retirement_wake_generation++;
	kernel_notify_task(worker->task);
	spin_unlock_irqrestore(&controller->active_lock, irq);
	started = sched_ticks();
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	    THREAD_ZOMBIE) {
		if (sched_ticks() - started >= UHCI_QUIESCE_TICKS) {
			irq = spin_lock_irqsave(&controller->active_lock);
			if (controller->retirement_worker != worker ||
			    !controller->retirement_joining) {
				spin_unlock_irqrestore(&controller->active_lock, irq);
				__builtin_trap();
			}
			controller->retirement_joining = 0;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			thread_release(worker);
			return EBUSY;
		}
		sched_yield();
	}
	error = thread_wait(worker, NULL);
	irq = spin_lock_irqsave(&controller->active_lock);
	if (!controller->retirement_joining ||
	    controller->retirement_worker != worker) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}
	if (error == 0)
		controller->retirement_worker = NULL;
	controller->retirement_joining = 0;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	thread_release(worker);
	return error;
}

static void
uhci_retirement_defer(struct uhci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);
	__atomic_store_n(&controller->retirement_pending, 1U,
	    __ATOMIC_RELEASE);
	controller->retirement_wake_generation++;
	worker = controller->retirement_worker;
	if (worker != NULL && !controller->retirement_joining)
		kernel_notify_task(worker->task);
	spin_unlock_irqrestore(&controller->active_lock, irq);
}

static int
uhci_quiesce_requests(struct uhci_controller *controller)
{
	struct uhci_request *request;
	uint64_t started = sched_ticks();
	unsigned long irq;
	int failure_error = 0;
	int inline_retirement;
	int pending;
	int wake = 0;

	irq = spin_lock_irqsave(&controller->active_lock);
	failure_error = controller->retirement_error;
	for (request = controller->active; request != NULL;
	    request = request->active_next) {
		if (request->state == UHCI_REQUEST_FAILED) {
			if (failure_error == 0)
				failure_error = request->retirement_error != 0 ?
				    request->retirement_error : EIO;
			continue;
		}
		if (request->state != UHCI_REQUEST_ACTIVE)
			continue;
		request->completion_status = DRV_USB_URB_DISCONNECTED;
		request->terminal_td_count = request->td_count;
		uhci_retirement_begin_locked(controller, request,
		    UHCI_REQUEST_WAIT_FRAME_COMPLETE);
		wake = 1;
	}
	inline_retirement = curthread == controller->retirement_worker;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	if (wake) {
		if (inline_retirement)
			uhci_retirement_process(controller);
		else
			uhci_retirement_defer(controller);
	}

	for (;;) {
		irq = spin_lock_irqsave(&controller->active_lock);
		if (uhci_request_sets_empty_locked(controller)) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			return 0;
		}
		pending = controller->retirement_head != NULL;
		if (failure_error == 0)
			failure_error = controller->retirement_error;
		for (request = controller->active; request != NULL;
		    request = request->active_next) {
			if (request->state != UHCI_REQUEST_FAILED)
				continue;
			if (failure_error == 0)
				failure_error = request->retirement_error != 0 ?
				    request->retirement_error : EIO;
		}
		spin_unlock_irqrestore(&controller->active_lock, irq);
		/* Once every queued request has recorded its failed local proof, retain
		 * the full graph and let quiesce perform the independent global stop. */
		if (failure_error != 0 && !pending)
			return failure_error;
		if (sched_ticks() - started >= UHCI_RETIRE_TICKS)
			return failure_error != 0 ? failure_error : EBUSY;
		if (inline_retirement)
			uhci_retirement_process(controller);
		else {
			uhci_retirement_defer(controller);
			sched_yield();
		}
	}
}

static void
uhci_retirement_watchdog_arm(struct uhci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);
	controller->retirement_wake_generation++;
	worker = controller->retirement_worker;
	if (worker != NULL && !controller->retirement_joining)
		kernel_notify_task(worker->task);
	spin_unlock_irqrestore(&controller->active_lock, irq);
}

static int uhci_urb_enqueue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct uhci_controller *c = hcd_controller(hcd);
	struct uhci_request *r = NULL;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->quiescing || c->dma_quiesced ||
	    c->retirement_worker == NULL) {
		error = c->quiescing || c->dma_quiesced ||
		    c->retirement_worker == NULL ? ENODEV : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);
		return error;
	}
	if (c->submitting == UINT_MAX)
		__builtin_trap();
	c->submitting++;
	spin_unlock_irqrestore(&c->active_lock, irq);
	error = uhci_build_request(c, urb, &r);
	irq = spin_lock_irqsave(&c->active_lock);
	if (error != 0) {
		if (c->submitting == 0)
			__builtin_trap();
		c->submitting--;
		spin_unlock_irqrestore(&c->active_lock, irq);
		return error;
	}
	if (c->quiescing || c->dma_quiesced ||
	    c->retirement_worker == NULL ||
	    uhci_endpoint_owned_locked(c, r->endpoint)) {
		error = c->quiescing || c->dma_quiesced ||
		    c->retirement_worker == NULL ? ENODEV : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);
		uhci_builder_discard(c, r);
		return error;
	}
	error = uhci_periodic_admit_locked(c, r);
	if (error != 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		uhci_builder_discard(c, r);
		return error;
	}
	error = drv_usb_urb_set_hcd_data(urb, r);
	if (error != 0) {
		uhci_periodic_release_locked(c, r);
		spin_unlock_irqrestore(&c->active_lock, irq);
		uhci_builder_discard(c, r);
		return error;
	}
	/* Builders run concurrently outside the controller lock.  Rebase the
	 * request's data toggles at the publication point so a same-endpoint
	 * predecessor which retired during construction cannot leave stale tokens. */
	uhci_request_prepare_toggle(r);
	uhci_active_insert_locked(c, r);
	uhci_schedule_insert_locked(c, r);
	spin_unlock_irqrestore(&c->active_lock, irq);
	uhci_retirement_watchdog_arm(c);
	uhci_builder_leave(c);
	return 0;
}
static int uhci_urb_dequeue(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{
	struct uhci_controller *c = hcd_controller(hcd);
	struct uhci_request *r;
	unsigned long irq;
	int error, inline_retirement;

	irq = spin_lock_irqsave(&c->active_lock);
	r = drv_usb_urb_hcd_data(urb);
	if (r == NULL || !uhci_request_active_locked(c, r) || r->urb != urb ||
	    r->state != UHCI_REQUEST_ACTIVE) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EBUSY;
	}
	uhci_retirement_begin_locked(c, r, UHCI_REQUEST_WAIT_FRAME_CANCEL);
	inline_retirement = curthread == c->retirement_worker;
	spin_unlock_irqrestore(&c->active_lock, irq);
	/* Completion callbacks run on this worker.  A callback may enqueue and
	 * synchronously cancel another URB; waking and waiting on ourselves would
	 * deadlock, so execute the same bounded retirement path inline. */
	if (inline_retirement)
		uhci_retirement_process_request(c, r);
	else
		uhci_retirement_defer(c);

	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		if (!uhci_request_active_locked(c, r) || r->urb != urb ||
		    drv_usb_urb_hcd_data(urb) != r) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			return EBUSY;
		}
		if (r->state == UHCI_REQUEST_RETIRED_CANCEL) {
			hal_io_rmb();
			uhci_request_commit_toggle(r);
			if (r->retirement_queued || r->scheduled) {
				spin_unlock_irqrestore(&c->active_lock, irq);
				__builtin_trap();
			}
			uhci_periodic_release_locked(c, r);
			uhci_active_remove_locked(c, r);
			(void)drv_usb_urb_set_hcd_data(urb, NULL);
			spin_unlock_irqrestore(&c->active_lock, irq);
			uhci_request_free(c, r);
			return 0;
		}
		if (r->state == UHCI_REQUEST_FAILED) {
			error = r->retirement_error != 0 ?
			    r->retirement_error : EIO;
			spin_unlock_irqrestore(&c->active_lock, irq);
			return error;
		}
		if (r->state != UHCI_REQUEST_WAIT_FRAME_CANCEL) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			return EBUSY;
		}
		spin_unlock_irqrestore(&c->active_lock, irq);
		sched_yield();
	}
}

static int uhci_irq(void *argument)
{
	struct uhci_controller *c = argument;
	struct uhci_request *r, *next;
	uint16_t status;
	unsigned terminal_td_count;
	unsigned long irq;
	enum drv_usb_urb_status result;
	int wake_worker = 0, report_failure = 0;

	status = in16(c->io_base + UHCI_USBSTS);
	if ((status & UHCI_STS_ALL) == 0)
		return 0;
	out16(c->io_base + UHCI_USBSTS, status & UHCI_STS_ALL);
	irq = spin_lock_irqsave(&c->active_lock);
	if (status == UINT16_MAX ||
	    (status & (UHCI_STS_HOST_SYSTEM_ERROR |
	    UHCI_STS_PROCESS_ERROR | UHCI_STS_HALTED)) != 0) {
		report_failure = !c->quarantined;
		c->quiescing = 1;
		c->quarantined = 1;
		spin_unlock_irqrestore(&c->active_lock, irq);
		uhci_root_worker_request_stop(c);
		out16(c->io_base + UHCI_USBINTR, 0);
		out16(c->io_base + UHCI_USBCMD,
		    in16(c->io_base + UHCI_USBCMD) &
		    (uint16_t)~UHCI_CMD_RUN);
		if (report_failure)
			hal_printf(
			    "uhci: fatal IRQ status %04x; controller quarantined\n",
			    status);
		return 1;
	}
	for (r = c->active; r != NULL; r = next) {
		next = r->active_next;
		if (r->state != UHCI_REQUEST_ACTIVE ||
		    !uhci_request_terminal(r, &result, &terminal_td_count))
			continue;
		r->completion_status = result;
		r->terminal_td_count = terminal_td_count;
		uhci_retirement_begin_locked(c, r,
		    UHCI_REQUEST_WAIT_FRAME_COMPLETE);
		wake_worker = 1;
	}
	spin_unlock_irqrestore(&c->active_lock, irq);
	if (wake_worker)
		uhci_retirement_defer(c);
	return 1;
}
static int uhci_endpoint_enable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{ (void)hcd; (void)endpoint; return 0; }
static int uhci_endpoint_disable(struct drv_usb_hcd *hcd,
	struct drv_usb_endpoint *endpoint)
{ (void)hcd; (void)endpoint; return 0; }
static uint32_t uhci_frame_number(struct drv_usb_hcd *hcd)
{ struct uhci_controller*c=hcd_controller(hcd);return in16(c->io_base+UHCI_FRNUM)&0x7ffU; }

static int uhci_root_hub_status(struct drv_usb_hcd *hcd, void *buffer,
	size_t size, size_t *actual)
{
	struct uhci_controller *c=hcd_controller(hcd);uint8_t bits=0;
	uint16_t p1=in16(c->io_base+UHCI_PORTSC1),p2=in16(c->io_base+UHCI_PORTSC2);
	if(buffer==NULL||size<1)return EINVAL;
	if(p1&(UHCI_PORT_CSC|UHCI_PORT_PEC))bits|=2U;
	if(p2&(UHCI_PORT_CSC|UHCI_PORT_PEC))bits|=4U;
	*(uint8_t*)buffer=bits;if(actual)*actual=1;return 0;
}

static int
uhci_root_port_update(struct uhci_controller *controller, unsigned index,
	uint16_t set, uint16_t clear, uint16_t acknowledge)
{
	uint16_t port, current, value;

	if (index < 1U || index > 2U ||
	    ((set | clear) & (uint16_t)~UHCI_PORT_RW_BITS) != 0 ||
	    (acknowledge & (uint16_t)~UHCI_PORT_CHANGE_BITS) != 0 ||
	    (set & clear) != 0)
		return EINVAL;
	port = controller->io_base +
	    (index == 1U ? UHCI_PORTSC1 : UHCI_PORTSC2);
	current = in16(port);
	if (current == UINT16_MAX)
		return EIO;
	/* PORTSC contains RO, reserved, and write-one-to-clear fields.  Rebuild
	 * writes from only the four R/W state bits and the specifically requested
	 * change acknowledgement; never echo a sampled CSC/PEC or RO bit. */
	value = current & UHCI_PORT_RW_BITS;
	value &= (uint16_t)~clear;
	value |= set;
	value |= acknowledge;
	out16(port, value);
	return 0;
}

static int
uhci_root_hub_control(struct drv_usb_hcd *hcd,
	const struct drv_usb_control_request *request, void *buffer, size_t size,
	size_t *actual)
{
	struct uhci_controller *controller = hcd_controller(hcd);
	uint16_t port, value;
	uint16_t set = 0, clear = 0, acknowledge = 0;
	uint32_t status = 0;
	int error;

	if (request == NULL || request->index < 1U || request->index > 2U)
		return EINVAL;
	port = controller->io_base +
	    (request->index == 1U ? UHCI_PORTSC1 : UHCI_PORTSC2);
	if (request->request == 0 && buffer != NULL && size >= sizeof(status)) {
		value = in16(port);
		if (value == UINT16_MAX)
			return EIO;
		if ((value & UHCI_PORT_CCS) != 0)
			status |= 1U;
		if ((value & UHCI_PORT_PE) != 0)
			status |= 2U;
		if ((value & UHCI_PORT_RESET) != 0)
			status |= 0x10U;
		if ((value & UHCI_PORT_LSDA) != 0)
			status |= 0x200U;
		if ((value & UHCI_PORT_CSC) != 0)
			status |= 0x10000U;
		if ((value & UHCI_PORT_PEC) != 0)
			status |= 0x20000U;
		memcpy(buffer, &status, sizeof(status));
		if (actual != NULL)
			*actual = sizeof(status);
		return 0;
	}
	if (request->request == 3) {
		if (request->value == 4)
			set = UHCI_PORT_RESET;
		else if (request->value == 1)
			set = UHCI_PORT_PE;
		else
			return ENOTSUP;
	} else if (request->request == 1) {
		if (request->value == 16)
			acknowledge = UHCI_PORT_CSC;
		else if (request->value == 17)
			acknowledge = UHCI_PORT_PEC;
		else if (request->value == 4)
			clear = UHCI_PORT_RESET;
		else if (request->value == 1)
			clear = UHCI_PORT_PE;
		else
			return ENOTSUP;
	} else {
		return ENOTSUP;
	}
	error = uhci_root_port_update(controller, request->index, set, clear,
	    acknowledge);
	if (error != 0)
		return error;
	if (actual != NULL)
		*actual = 0;
	return 0;
}

static int
uhci_root_ports_changed(struct uhci_controller *controller)
{
	uint16_t current[2];
	unsigned long irq;
	int changed, report_failure = 0;

	current[0] = in16(controller->io_base + UHCI_PORTSC1);
	current[1] = in16(controller->io_base + UHCI_PORTSC2);
	if (current[0] == UINT16_MAX || current[1] == UINT16_MAX) {
		irq = spin_lock_irqsave(&controller->active_lock);
		report_failure = !controller->quarantined;
		controller->quiescing = 1;
		controller->quarantined = 1;
		controller->root_ready = 0;
		controller->root_stopping = 1;
		spin_unlock_irqrestore(&controller->active_lock, irq);
		if (report_failure)
			hal_printf(
			    "uhci: root-port register unavailable; controller quarantined\n");
		return 0;
	}
	irq = spin_lock_irqsave(&controller->active_lock);
	changed = __atomic_exchange_n(&controller->root_force_scan, 0U,
	    __ATOMIC_ACQ_REL) != 0 || !controller->root_port_status_valid ||
	    (current[0] & (UHCI_PORT_CSC | UHCI_PORT_PEC)) != 0 ||
	    (current[1] & (UHCI_PORT_CSC | UHCI_PORT_PEC)) != 0 ||
	    ((current[0] ^ controller->root_port_status[0]) & UHCI_PORT_CCS) != 0 ||
	    ((current[1] ^ controller->root_port_status[1]) & UHCI_PORT_CCS) != 0;
	controller->root_port_status[0] = current[0];
	controller->root_port_status[1] = current[1];
	controller->root_port_status_valid = 1;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	return changed;
}

static void
uhci_root_worker(void *argument)
{
	struct uhci_controller *controller = argument;

	for (;;) {
		uint64_t observed_generation;
		unsigned long irq;
		int ready;

		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->root_stopping) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			return;
		}
		observed_generation = controller->root_wake_generation;
		ready = controller->root_ready != 0;
		spin_unlock_irqrestore(&controller->active_lock, irq);
		if (ready) {
			/* A changed sample is useful diagnostic state, but it is not a
			 * dispatch gate.  Retained disconnect teardown may need another
			 * common-core pass after CSC/PEC has already been acknowledged. */
			(void)uhci_root_ports_changed(controller);
			irq = spin_lock_irqsave(&controller->active_lock);
			ready = controller->root_ready &&
			    !controller->root_stopping && !controller->quarantined;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			if (ready)
				drv_usb_hcd_root_hub_changed(&controller->hcd);
		}
		/* A generation change closes the check-to-sleep window.  The locked
		 * scheduler handoff publishes THREAD_SLEEPING before a notifier can
		 * acquire active_lock, so arm and stop cannot be delayed by a lost wake. */
		irq = spin_lock_irqsave(&controller->active_lock);
		if (controller->root_stopping) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			return;
		}
		if (controller->root_wake_generation != observed_generation) {
			spin_unlock_irqrestore(&controller->active_lock, irq);
			continue;
		}
		sched_sleep_locked(sched_ticks() + UHCI_ROOT_POLL_TICKS,
		    &controller->active_lock);
		spin_unlock_irqrestore(&controller->active_lock, irq);
	}
}

static int
uhci_root_worker_start(struct uhci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->root_worker != NULL || controller->root_joining) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EALREADY;
	}
	controller->root_stopping = 0;
	controller->root_ready = 0;
	controller->root_wake_generation = 1;
	controller->root_force_scan = 1;
	controller->root_port_status_valid = 0;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	error = kthread_create(uhci_root_worker, controller,
	    SCHED_PRIORITY_DEFAULT, &worker);
	if (error != 0)
		return error;
	irq = spin_lock_irqsave(&controller->active_lock);
	controller->root_worker = worker;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	thread_start(worker);
	return 0;
}

static void
uhci_root_worker_arm(struct uhci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->root_worker == NULL || controller->root_stopping) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return;
	}
	controller->root_force_scan = 1;
	controller->root_ready = 1;
	controller->root_wake_generation++;
	worker = controller->root_worker;
	kernel_notify_task(worker->task);
	spin_unlock_irqrestore(&controller->active_lock, irq);
#ifdef ZEDBSD_TEST_CHECKPOINTS
	if (!__atomic_exchange_n(&controller->root_evidence, 1U,
	    __ATOMIC_ACQ_REL))
		hal_printf("uhci: root hotplug worker active\n");
#endif
}

static void
uhci_root_worker_request_stop(struct uhci_controller *controller)
{
	struct thread *worker;
	unsigned long irq;

	irq = spin_lock_irqsave(&controller->active_lock);
	controller->root_ready = 0;
	controller->root_stopping = 1;
	controller->root_wake_generation++;
	worker = controller->root_worker;
	if (worker != NULL && !controller->root_joining)
		kernel_notify_task(worker->task);
	spin_unlock_irqrestore(&controller->active_lock, irq);
}

static int
uhci_root_worker_stop(struct uhci_controller *controller)
{
	struct thread *worker;
	uint64_t started;
	unsigned long irq;
	int error;

	uhci_root_worker_request_stop(controller);
	irq = spin_lock_irqsave(&controller->active_lock);
	worker = controller->root_worker;
	if (worker == NULL) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return 0;
	}
	if (controller->root_joining || worker == curthread) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EBUSY;
	}
	controller->root_joining = 1;
	/* Keep the published pointer alive until it is cleared under active_lock;
	 * joining suppresses all task notifications during the reap window. */
	thread_ref(worker);
	kernel_notify_task(worker->task);
	spin_unlock_irqrestore(&controller->active_lock, irq);
	started = sched_ticks();
	while (atomic_raw_load_acquire((volatile unsigned *)&worker->state) !=
	    THREAD_ZOMBIE) {
		if (sched_ticks() - started >= UHCI_QUIESCE_TICKS) {
			irq = spin_lock_irqsave(&controller->active_lock);
			if (controller->root_worker != worker ||
			    !controller->root_joining) {
				spin_unlock_irqrestore(&controller->active_lock, irq);
				__builtin_trap();
			}
			controller->root_joining = 0;
			spin_unlock_irqrestore(&controller->active_lock, irq);
			thread_release(worker);
			return EBUSY;
		}
		sched_yield();
	}
	error = thread_wait(worker, NULL);
	irq = spin_lock_irqsave(&controller->active_lock);
	if (!controller->root_joining || controller->root_worker != worker) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		__builtin_trap();
	}
	if (error == 0)
		controller->root_worker = NULL;
	controller->root_joining = 0;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	thread_release(worker);
	return error;
}

static const struct drv_usb_hcd_ops uhci_ops = {
	.start=uhci_start,.quiesce=uhci_quiesce,.stop=uhci_stop,
	.urb_enqueue=uhci_urb_enqueue,
	.urb_dequeue=uhci_urb_dequeue,.endpoint_enable=uhci_endpoint_enable,
	.endpoint_disable=uhci_endpoint_disable,.frame_number=uhci_frame_number,
	.root_hub_status=uhci_root_hub_status,.root_hub_control=uhci_root_hub_control
};

static void
uhci_publish(struct uhci_controller *controller)
{
	if (controller->listed)
		return;
	drv_pci_device_set_driver_data(controller->pci, controller);
	controller->next = uhci_controllers;
	uhci_controllers = controller;
	controller->listed = 1;
}

static void
uhci_unpublish(struct uhci_controller *controller)
{
	struct uhci_controller **link;

	if (!controller->listed)
		return;
	for (link = &uhci_controllers; *link != NULL; link = &(*link)->next) {
		if (*link == controller) {
			*link = controller->next;
			break;
		}
	}
	controller->next = NULL;
	controller->listed = 0;
}

static int
uhci_pci_release(struct uhci_controller *controller)
{
	int error;

	/* A RUN write makes both schedule allocations part of the controller's
	 * ownership graph.  Never restore a saved BME bit or release the I/O lease
	 * while that graph lacks the checked halt/master/IRQ barrier. */
	if (controller->frame_list.address != NULL ||
	    controller->skeleton_memory.address != NULL ||
	    !controller->dma_quiesced)
		return EBUSY;
	if (controller->pci_state_saved) {
		error = uhci_bus_master_disable(controller);
		if (error != 0)
			return error;
		error = drv_pci_device_restore_enable_state(controller->pci,
		    &controller->pci_enable_state);
		if (error != 0)
			return error;
		controller->pci_state_saved = 0;
	}
	if (controller->bar_claimed) {
		drv_pci_device_release_bar(controller->pci,
		    controller->bar_index);
		controller->bar_claimed = 0;
	}
	return 0;
}

static int
uhci_cleanup(struct uhci_controller *controller)
{
	unsigned long irq;
	unsigned had_root, root_was_ready;
	int dma_quiesced;
	int error, restart_error;

	/* Root scans and completion callbacks can re-enter PCI teardown.  Joining
	 * the current worker is impossible, so reject before stopping either
	 * worker or closing HCD admission. */
	irq = spin_lock_irqsave(&controller->active_lock);
	if (controller->root_joining || controller->retirement_joining ||
	    controller->root_worker == curthread ||
	    controller->retirement_worker == curthread) {
		spin_unlock_irqrestore(&controller->active_lock, irq);
		return EBUSY;
	}
	had_root = controller->root_worker != NULL;
	root_was_ready = controller->root_ready;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	if (had_root) {
		error = uhci_root_worker_stop(controller);
		if (error != 0)
			return error;
	}

	if (controller->hcd_registered) {
		error = drv_usb_hcd_unregister(&controller->hcd);
		if (error != 0) {
			/* EBUSY before HCD quiesce leaves the controller operational.  Restore
			 * runtime root observation rather than silently losing hotplug. */
			if (error == EBUSY && had_root && !controller->quiescing) {
				restart_error = uhci_root_worker_start(controller);
				if (restart_error == 0 && root_was_ready)
					uhci_root_worker_arm(controller);
				else if (restart_error != 0)
					return restart_error;
			}
			return error;
		}
		controller->hcd_registered = 0;
	}
	/* drv_usb_hcd_register() does not invoke stop() after a failing start().
	 * A RUN-visible frame graph therefore remains owned here until the same
	 * checked halt/BME/IRQ barrier succeeds on this or a later detach retry. */
	if (controller->frame_list.address != NULL ||
	    controller->skeleton_memory.address != NULL) {
		irq = spin_lock_irqsave(&controller->active_lock);
		dma_quiesced = controller->dma_quiesced != 0;
		spin_unlock_irqrestore(&controller->active_lock, irq);
		if (!dma_quiesced) {
			error = uhci_hardware_stop(controller, "attach cleanup");
			if (error != 0)
				return error;
		}
		uhci_schedule_release(controller);
	}
	if (controller->irq_allocated) {
		/* A registered HCD removes the checked IRQ from its quiesce
		 * callback. An attach failure before registration has no cookie. */
		if (controller->irq_cookie != NULL)
			return EBUSY;
		drv_pci_device_free_irqs(controller->pci, &controller->irq, 1);
		controller->irq_allocated = 0;
	}
	return uhci_pci_release(controller);
}

static int
uhci_runtime_operational(struct uhci_controller *controller)
{
	unsigned long irq;
	int operational;

	irq = spin_lock_irqsave(&controller->active_lock);
	operational = controller->hcd_registered &&
	    !controller->quarantined && !controller->quiescing &&
	    !controller->dma_quiesced &&
	    controller->retirement_worker != NULL &&
	    !controller->retirement_joining &&
	    controller->root_worker != NULL && !controller->root_joining;
	spin_unlock_irqrestore(&controller->active_lock, irq);
	return operational;
}

static int
uhci_attach(struct drv_pci_device *device, const struct drv_pci_id *id)
{
	struct uhci_controller *controller;
	struct drv_pci_bar bar;
	unsigned count = 0, index;
	int cleanup_error, error;
	const char *stage = "allocation";

	(void)id;
	controller = hal_malloc(sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;
	memset(controller, 0, sizeof(*controller));
	spin_init(&controller->active_lock, LOCK_RANK_DEVICE,
	    "UHCI active request");
	controller->pci = device;
	controller->dma_quiesced = 1;
	controller->quiescing = 1;
	for (index = 0; index < drv_pci_device_bar_count(device); index++)
		if (drv_pci_device_bar(device, index, &bar) == 0 &&
		    bar.type == DRV_PCI_BAR_IO)
			break;
	if (index == drv_pci_device_bar_count(device) ||
	    bar.bus_address > 0xffffU) {
		error = ENODEV;
		goto fail;
	}
	controller->bar_index = index;
	controller->io_base = (uint16_t)bar.bus_address;
	stage = "I/O BAR claim";
	error = drv_pci_device_claim_bar(device, index);
	if (error != 0)
		goto fail;
	controller->bar_claimed = 1;
	stage = "PCI command save";
	error = drv_pci_device_save_enable_state(device,
	    &controller->pci_enable_state);
	if (error != 0)
		goto fail;
	controller->pci_state_saved = 1;
	controller->hcd.name = "UHCI";
	controller->hcd.ops = &uhci_ops;
	controller->hcd.dma = drv_pci_device_dma(device);
	controller->hcd.root_port_count = 2;
	controller->hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	controller->hcd.private_data[0] = (uintptr_t)controller;
	stage = "PCI enable";
	if ((error = drv_pci_device_enable_io(device)) != 0 ||
	    (error = drv_pci_device_set_bus_master(device, true)) != 0)
		goto fail;
	stage = "HCD registration";
	error = drv_usb_hcd_register(&controller->hcd, &controller->bus);
	if (error != 0)
		goto fail;
	controller->hcd_registered = 1;
	stage = "IRQ allocation";
	error = drv_pci_device_allocate_irqs(device, DRV_PCI_IRQ_ALLOW_INTX,
	    1, 1, &controller->irq, &count);
	if (error != 0)
		goto fail;
	controller->irq_allocated = 1;
	stage = "IRQ establishment";
	error = drv_pci_device_establish_irq(device, &controller->irq,
	    uhci_irq, controller, "uhci", &controller->irq_cookie);
	if (error != 0)
		goto fail;
	stage = "retirement worker";
	error = uhci_retirement_worker_start(controller);
	if (error != 0)
		goto fail;
	stage = "root hotplug worker";
	error = uhci_root_worker_start(controller);
	if (error != 0)
		goto fail;
	out16(controller->io_base + UHCI_USBINTR, 0x000dU);
	uhci_publish(controller);
#ifdef ZEDBSD_TEST_CHECKPOINTS
	hal_printf("uhci: concurrent per-endpoint scheduling active\n");
#endif
	hal_printf("uhci: PCI controller at I/O %04x, ports=%u\n",
	    controller->io_base, controller->hcd.root_port_count);
	return 0;

fail:
	cleanup_error = uhci_cleanup(controller);
	if (cleanup_error != 0) {
		controller->quarantined = 1;
		uhci_publish(controller);
		hal_printf(
		    "uhci: attach failed at %s (%d), cleanup failed (%d); controller quarantined\n",
		    stage, error, cleanup_error);
		return 0;
	}
	hal_free(controller);
	return error;
}

static int
uhci_detach(struct drv_pci_device *device, unsigned flags)
{
	struct uhci_controller *controller =
	    drv_pci_device_driver_data(device);
	int error;

	(void)flags;
	if (controller == NULL)
		return 0;
	error = uhci_cleanup(controller);
	if (error != 0) {
		/* EBUSY is a normal retry boundary when teardown was re-entered by
		 * one of our workers or the USB core still owns devices.  If cleanup
		 * left the runtime fully operational, do not poison that controller. */
		if (error != EBUSY || !uhci_runtime_operational(controller))
			controller->quarantined = 1;
		return error;
	}
	uhci_unpublish(controller);
	drv_pci_device_set_driver_data(device, NULL);
	hal_free(controller);
	return 0;
}
static const struct drv_pci_id uhci_ids[]={
	{DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,DRV_PCI_ANY_ID,0x0c0300U,0xffffffU,0}
};
static struct drv_pci_driver uhci_driver={.name="uhci",.ids=uhci_ids,.id_count=1,.attach=uhci_attach,.detach=uhci_detach};
int drv_pci_uhci_driver_register(void){return drv_pci_driver_register(&uhci_driver);}
void
drv_pci_uhci_probe_roots(void)
{
	struct uhci_controller *controller;

	for (controller = uhci_controllers; controller != NULL;
	    controller = controller->next) {
		if (controller->quarantined)
			continue;
		drv_usb_hcd_root_hub_changed(&controller->hcd);
		uhci_root_worker_arm(controller);
	}
}
