/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Begin consolidated pci-xhci.c. */
/*
 * Native PCI xHCI host controller. Copyright (C) 2026 Awe Morris;
 * SPDX-License-Identifier: Zlib */
#include <drivers/pci-xhci.h>
#include <drivers/pci.h>
#include <drivers/pci-xhci-capability.h>
#include <drivers/pci-xhci-control.h>
#include <drivers/pci-xhci-lifecycle.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/io-stats.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <limits.h>
#include <string.h>

#define XHCI_USBCMD 0x00U
#ifndef ZEDBSD_XHCI_IMOD
#define ZEDBSD_XHCI_IMOD 4000U
#endif
_Static_assert(ZEDBSD_XHCI_IMOD <= 65535U, "xHCI IMOD interval range");
#define XHCI_USBSTS 0x04U
#define XHCI_PAGESIZE 0x08U
#define XHCI_CRCR 0x18U
#define XHCI_DCBAAP 0x30U
#define XHCI_CONFIG 0x38U
#define XHCI_PORTSC(n) (0x400U + 0x10U * (n))
#define XHCI_CMD_RUN 0x01U
#define XHCI_CMD_RESET 0x02U
#define XHCI_CMD_INTE 0x04U
#define XHCI_STS_HALTED 0x01U
#define XHCI_STS_EINT 0x08U
#define XHCI_STS_FATAL 0x04U
#define XHCI_STS_CNR 0x00000800U
#define XHCI_PORT_CCS 0x00000001U
#define XHCI_PORT_PED 0x00000002U
#define XHCI_PORT_PR 0x00000010U
#define XHCI_PORT_PP 0x00000200U
#define XHCI_PORT_CSC 0x00020000U
#define XHCI_PORT_CHANGE 0x00fe0000U
#define XHCI_TRB_CYCLE 0x00000001U
#define XHCI_TRB_CHAIN 0x00000010U
#define XHCI_TRB_IOC 0x00000020U
#define XHCI_TRB_IDT 0x00000040U
#define XHCI_TRB_TYPE(n) ((uint32_t)(n) << 10)
#define XHCI_TRB_DIR_IN 0x00010000U
#define XHCI_TRB_SLOT(n) ((uint32_t)(n) << 24)
#define XHCI_RING_TRBS 256U
#define XHCI_TRANSFER_RESERVE_SIZE DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE
#define XHCI_TIMEOUT 10000000U
#define XHCI_PCI_COMMAND 0x04U
#define XHCI_PCI_BAR0 0x10U
#define XHCI_PCI_COMMAND_IO 0x0001U
#define XHCI_PCI_COMMAND_MEMORY 0x0002U
#define XHCI_PCI_COMMAND_MASTER 0x0004U
#define XHCI_PCI_COMMAND_ENABLE                                                \
	(XHCI_PCI_COMMAND_IO | XHCI_PCI_COMMAND_MEMORY |                       \
	 XHCI_PCI_COMMAND_MASTER)

struct xhci_trb {
	volatile uint32_t parameter_low, parameter_high, status, control;
};
struct xhci_erst {
	uint64_t address;
	uint32_t size, reserved;
};
struct xhci_ring {
	struct drv_dma_buffer dma;
	struct xhci_trb *trbs;
	unsigned enqueue, cycle;
};
struct xhci_endpoint {
	struct xhci_ring ring;
	struct xhci_request *active;
	unsigned dci;
	unsigned enabled, recovering, stall_publishing;
};
struct xhci_device {
	struct drv_usb_device *usb;
	struct drv_dma_buffer output_context, input_context;
	struct xhci_endpoint endpoints[32];
	unsigned slot, context_entries;
	unsigned speed_id;
	unsigned quiescing, slot_disabled, default_owned;
	unsigned completions_busy;
	struct xhci_device *next;
};
struct xhci_urb_reservation;

struct xhci_request {
	struct drv_usb_urb *urb;
	struct xhci_device *device;
	struct drv_dma_buffer bounce;
	struct drv_dma_vector *vector;
	void *staging;
	unsigned normal_count;
	struct drv_dma_segment normal[DRV_DMA_VECTOR_MAX_SEGMENTS];
	struct xhci_endpoint *endpoint;
	size_t length;
	unsigned first_trb;
	unsigned trb_count;
	unsigned slot;
	unsigned dci, port;
	int input;
	unsigned cancelling, transfer_seen, completion_code;
	unsigned short_seen;
	unsigned stall_publication;
	unsigned reserved;
	struct xhci_urb_reservation *reservation;
	uint64_t reservation_generation;
	size_t short_actual;
	size_t completion_actual, completion_residual;
	unsigned completion_trb_offset;
	enum drv_usb_urb_status terminal_status;
	struct xhci_request *completion_next;
};
struct xhci_urb_reservation {
	struct xhci_request request;
	struct drv_dma_buffer backing;
	struct drv_dma_vector *vector;
	size_t capacity;
	uint64_t generation;
	unsigned busy;
};
struct xhci_controller {
	struct drv_pci_device *pci;
	struct drv_pci_mapping mapping;
	struct drv_pci_bar original_bar;
	struct drv_pci_enable_state pci_enable_state;
	volatile uint8_t *capability, *operational, *runtime, *doorbells;
	struct drv_dma_buffer dcbaa, command_memory, event_memory, erst_memory;
	struct drv_dma_buffer scratchpad_array, *scratchpads;
	struct xhci_ring command;
	struct xhci_trb *events;
	unsigned event_dequeue, event_cycle, context_size, max_slots, ports;
	unsigned scratchpad_count;
	struct drv_usb_hcd hcd;
	struct drv_usb_bus *bus;
	struct drv_pci_irq irq;
	void *irq_cookie;
	struct xhci_device *devices;
	struct xhci_request *completion_head, *completion_tail;
	struct xhci_request transfer_request;
	struct drv_dma_buffer transfer_reserve;
	struct spinlock active_lock;
	struct xhci_trb command_event;
	uint64_t command_address;
	struct thread *port_worker;
	volatile unsigned command_busy, command_failed, completion_busy;
	volatile unsigned irq_busy;
	volatile unsigned event_busy;
	volatile unsigned command_event_ready;
	/*
 * Protected by active_lock.  Each endpoint remains queue-depth one,
	 * while these counts provide bounded controller teardown barriers. */
	unsigned active_count;
	unsigned endpoint_recoveries_busy;
	unsigned transfer_reserve_busy;
	unsigned operations_busy;
	unsigned submissions_busy;
	unsigned completion_dispatch_busy;
	unsigned controller_stopping;
	volatile unsigned port_pending, port_stopping, root_ready;
	unsigned bar_claimed, bar_mapped, original_bar_valid, pci_state_saved;
	volatile unsigned default_slot;
	unsigned dma_quiesced, hcd_registered, irq_allocated, quarantined;
	unsigned legacy_offset, legacy_claimed;
	uint32_t legacy_control;
	struct xhci_controller *next;
};

static struct xhci_controller *controllers;

static int xhci_urb_reserve(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb, size_t capacity, void **result);
static void xhci_urb_unreserve(struct drv_usb_hcd *hcd, void *opaque);
static void *xhci_urb_reserve_buffer(struct drv_usb_hcd *hcd, void *opaque, size_t *capacity);
static int xhci_sg_plan(struct xhci_request *request, size_t length);
static int xhci_sg_short(const struct xhci_request *request, unsigned offset, size_t residual, size_t *actual);
static void xhci_sg_enqueue(struct xhci_ring *ring, const struct xhci_request *request, int input, size_t packet_size, int zero_packet);
static void xhci_request_release(struct xhci_controller *, struct xhci_request *);

/* Supports the rd8 operation. */
static uint8_t rd8(volatile uint8_t *b, unsigned o);

/*
 * Forward declaration.
 */
static uint32_t rd32(volatile uint8_t *b, unsigned o);
static void wr32(volatile uint8_t *b, unsigned o, uint32_t v);
static void wr8(volatile uint8_t *b, unsigned o, uint8_t v);
static void wr64(volatile uint8_t *b, unsigned o, uint64_t v);
static struct xhci_controller *hcd_controller(struct drv_usb_hcd *h);
static int wait_bits(volatile uint8_t *b, unsigned o, uint32_t mask, uint32_t wanted);
static void xhci_bar_raw(struct drv_pci_device *device, enum drv_pci_bar_type type, uint32_t *low, uint32_t *high);
static void xhci_pci_identity(struct drv_pci_device *device);
static int xhci_restore_bar(struct xhci_controller *controller);
static int xhci_bus_master_disable(struct xhci_controller *controller);
static int xhci_pci_quiesce(struct xhci_controller *controller);
static void xhci_legacy_release(struct xhci_controller *controller);
static int xhci_pci_release(struct xhci_controller *controller);
static void xhci_mark_quarantined(struct xhci_controller *controller);
static void xhci_quarantine(struct xhci_controller *controller, const char *stage, int error);
static int ring_alloc(struct xhci_controller *c, struct xhci_ring *r);
static void ring_free(struct xhci_controller *c, struct xhci_ring *r);
static uint64_t ring_push(struct xhci_ring *r, uint64_t parameter, uint32_t status, uint32_t control);
static int event_take(struct xhci_controller *c, struct xhci_trb *out);
static void event_lock(struct xhci_controller *c);
static void event_unlock(struct xhci_controller *c);
static int transfer_claim(struct xhci_controller *c, const struct xhci_trb *event);
static void xhci_completion_drain(struct xhci_controller *c);
static unsigned xhci_endpoint_state(struct xhci_controller *c, struct xhci_device *d, unsigned dci);
static void port_change_defer(struct xhci_controller *c);
static uint64_t xhci_event_pointer(const struct xhci_trb *event);
static int command_ex(struct xhci_controller *c, uint64_t parameter, uint32_t status, uint32_t control, unsigned *slot, unsigned *completion);
static int command(struct xhci_controller *c, uint64_t parameter, uint32_t status, uint32_t control, unsigned *slot);
static int ownership(struct xhci_controller *c);
static void fill_slot(struct xhci_controller *c, struct xhci_device *d, void *context, unsigned entries);
static void fill_endpoint(struct xhci_controller *c, void *context, struct xhci_endpoint *ep, const struct drv_xhci_endpoint_context_words *encoded);
static struct xhci_device *xhci_usb_device(struct drv_usb_device *u);
static struct xhci_device *xhci_slot_device_locked(struct xhci_controller *c, unsigned slot);
static struct xhci_request * xhci_event_request_locked(struct xhci_controller *c, const struct xhci_trb *event, unsigned *trb_offset);
static int xhci_request_publish_locked(struct xhci_controller *c, struct xhci_request *request);
static int xhci_request_unlink_locked(struct xhci_controller *c, struct xhci_request *request);
static void xhci_recovery_leave_locked(struct xhci_controller *c, struct xhci_endpoint *endpoint);
static int xhci_device_recovery_busy_locked(const struct xhci_device *device);
static int xhci_device_request_busy_locked(const struct xhci_device *device);
static void xhci_default_owner_release(struct xhci_controller *c, struct xhci_device *d);
static void xhci_device_release(struct drv_usb_hcd *h, struct xhci_device *d);
static int xhci_device_enable(struct drv_usb_hcd *h, struct drv_usb_device *u);
static int xhci_set_address(struct drv_usb_hcd *h, struct drv_usb_device *u, unsigned address);
static void xhci_device_disable(struct drv_usb_hcd *h, struct drv_usb_device *u);
static int xhci_endpoint_enable(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep);
static int xhci_endpoint_disable(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep);
static void xhci_completion_finish(struct xhci_controller *c, struct xhci_request *request);
static int xhci_irq(void *argument);
static void xhci_port_worker(void *argument);
static int xhci_worker_start(struct xhci_controller *c);
static void xhci_worker_stop(struct xhci_controller *c);
static unsigned normal_trb_count(uint64_t address, size_t length);
static uint64_t enqueue_normal(struct xhci_ring *ring, uint64_t address, size_t length, int input, size_t maximum_packet_size, int zero_packet);
static int xhci_endpoint_restart_empty(struct xhci_controller *c, struct xhci_device *d, struct xhci_endpoint *endpoint, unsigned dci);
static int xhci_endpoint_recover(struct xhci_controller *c, struct xhci_device *d, struct xhci_endpoint *endpoint, unsigned dci);
static int xhci_endpoint_reset(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep);
static struct xhci_request *xhci_request_alloc(struct xhci_controller *c, struct drv_usb_hcd *h, size_t length, unsigned flags, struct drv_usb_urb *urb, int *error);
static int xhci_submission_enter(struct xhci_controller *c);
static int xhci_operation_enter(struct xhci_controller *c);
static void xhci_operation_leave(struct xhci_controller *c);
static void xhci_submission_leave(struct xhci_controller *c);
static int xhci_urb_enqueue(struct drv_usb_hcd *h, struct drv_usb_urb *u);
static int xhci_cancel_request(struct xhci_controller *c, struct xhci_device *d, struct xhci_request *r);
static int xhci_urb_dequeue(struct drv_usb_hcd *h, struct drv_usb_urb *u);
static int xhci_endpoint_quiesce(struct xhci_controller *c, struct xhci_device *d, unsigned dci);
static int xhci_device_quiesce(struct drv_usb_hcd *h, struct drv_usb_device *u);
static uint32_t xhci_frame(struct drv_usb_hcd *h);
static int xhci_root_status(struct drv_usb_hcd *h, void *b, size_t n, size_t *a);
static int xhci_root_control(struct drv_usb_hcd *h, const struct drv_usb_control_request *r, void *b, size_t n, size_t *a);
static int xhci_root_port_reset(struct drv_usb_hcd *h, unsigned port);
static void xhci_scratchpads_free(struct xhci_controller *c);
static int xhci_scratchpads_alloc(struct xhci_controller *c);
static void xhci_controller_drain_requests(struct xhci_controller *c);
static int xhci_submission_quiesce(struct xhci_controller *c);
static int xhci_irq_quiesce(struct xhci_controller *c);
static int xhci_irq_disestablish(struct xhci_controller *c);
static int xhci_quiesce(struct drv_usb_hcd *h);
static int xhci_release_resources(struct drv_usb_hcd *h);
static void xhci_stop(struct drv_usb_hcd *h);
static int xhci_stop_checked(struct drv_usb_hcd *h);
static int xhci_start(struct drv_usb_hcd *h);
static int xhci_guarded_device_enable(struct drv_usb_hcd *h, struct drv_usb_device *u);
static int xhci_guarded_set_address(struct drv_usb_hcd *h, struct drv_usb_device *u, unsigned address);
static int xhci_guarded_device_quiesce(struct drv_usb_hcd *h, struct drv_usb_device *u);
static void xhci_guarded_device_disable(struct drv_usb_hcd *h, struct drv_usb_device *u);
static int xhci_guarded_urb_dequeue(struct drv_usb_hcd *h, struct drv_usb_urb *u);
static int xhci_guarded_endpoint_enable(struct drv_usb_hcd *h, struct drv_usb_endpoint *endpoint);
static int xhci_guarded_endpoint_reset(struct drv_usb_hcd *h, struct drv_usb_endpoint *endpoint);
static int xhci_guarded_endpoint_disable(struct drv_usb_hcd *h, struct drv_usb_endpoint *endpoint);
static uint32_t xhci_guarded_frame(struct drv_usb_hcd *h);
static int xhci_guarded_root_status(struct drv_usb_hcd *h, void *buffer, size_t length, size_t *actual);
static int xhci_guarded_root_control(struct drv_usb_hcd *h, const struct drv_usb_control_request *request, void *buffer, size_t length, size_t *actual);
static int xhci_guarded_root_port_reset(struct drv_usb_hcd *h, unsigned port);
static int xhci_attach(struct drv_pci_device *d, const struct drv_pci_id *id);
static int xhci_detach(struct drv_pci_device *d, unsigned flags);

/* Supports the rd8 operation. */
static uint8_t
rd8(
	volatile uint8_t *b,
	unsigned o)
{
	/* Returns the computed result. */
	return b[o];
}
/* Supports the rd32 operation. */

/* Supports the rd32 operation. */
static uint32_t
rd32(
	volatile uint8_t *b,
	unsigned o)
{
	/* Returns the computed result. */
	return *(volatile uint32_t *)(b + o);
}
/* Supports the wr32 operation. */

/* Supports the wr32 operation. */
static void
wr32(
	volatile uint8_t *b,
	unsigned o,
	uint32_t v)
{
	*(volatile uint32_t *)(b + o) = v;

	hal_io_mb();
}
/* Supports the wr8 operation. */

/* Supports the wr8 operation. */
static void
wr8(
	volatile uint8_t *b,
	unsigned o,
	uint8_t v)
{
	b[o] = v;
	hal_io_mb();
}
/* Supports the wr64 operation. */

/* Supports the wr64 operation. */
static void
wr64(
	volatile uint8_t *b,
	unsigned o,
	uint64_t v)
{
	wr32(b, o, (uint32_t)v);
	wr32(b, o + 4U, (uint32_t)(v >> 32));
}
/* Supports the hcd controller operation. */

/* Supports the hcd controller operation. */
static struct xhci_controller *
hcd_controller(
	struct drv_usb_hcd *h)
{
	/* Returns the computed result. */
	return (void *)h->private_data[0];
}

/* Supports the wait bits operation. */

/* Supports the wait bits operation. */
static int
wait_bits(
	volatile uint8_t *b,
	unsigned o,
	uint32_t mask,
	uint32_t wanted)
{
	unsigned n;
	uint32_t value;

	/* Process each element required by the operation. */
	for (n = 0; n < XHCI_TIMEOUT; n++) {
		/* Checks the drv xhci mmio32 valid result. */
		value = rd32(b, o);
		if (!drv_xhci_mmio32_valid(value))
			return EIO;

		/* Validates the current value. */
		if ((value & mask) == wanted)
			return 0;
	}

	/* Returns the computed result. */
	return ETIMEDOUT;
}

/* Supports the xhci bar raw operation. */

/* Supports the xhci bar raw operation. */
static void
xhci_bar_raw(
	struct drv_pci_device *device,
	enum drv_pci_bar_type type,
	uint32_t *low,
	uint32_t *high)
{
	*low = 0xffffffffU;
	*high = 0;
	(void)drv_pci_device_config_read32(device, XHCI_PCI_BAR0, low);

	/* Handles the type condition. */
	if (type == DRV_PCI_BAR_MEMORY64) {
		*high = 0xffffffffU;
		(void)drv_pci_device_config_read32(device, XHCI_PCI_BAR0 + 4U,
						   high);
	}
}

/* Supports the xhci pci identity operation. */

/* Supports the xhci pci identity operation. */
static void
xhci_pci_identity(
	struct drv_pci_device *device)
{
	struct drv_pci_address address, bridge_address;
	struct drv_pci_bus *bus;
	struct drv_pci_device *bridge;

	drv_pci_device_address(device, &address);
	bus = drv_pci_device_bus(device);

	/* Handles the bridge availability. */
	bridge = bus != NULL ? drv_pci_bus_bridge(bus) : NULL;
	if (bridge != NULL) {
		drv_pci_device_address(bridge, &bridge_address);
		hal_printf("xhci: pci %04x:%02x:%02x.%u id=%04x:%04x "
			   "sub=%04x:%04x rev=%02x parent=%04x:%02x:%02x.%u\n",
			   address.segment, address.bus, address.device,
			   address.function, drv_pci_device_vendor(device),
			   drv_pci_device_product(device),
			   drv_pci_device_subvendor(device),
			   drv_pci_device_subproduct(device),
			   drv_pci_device_revision(device),
			   bridge_address.segment, bridge_address.bus,
			   bridge_address.device, bridge_address.function);
	} else {
		hal_printf("xhci: pci %04x:%02x:%02x.%u id=%04x:%04x "
			   "sub=%04x:%04x rev=%02x parent=root\n",
			   address.segment, address.bus, address.device,
			   address.function, drv_pci_device_vendor(device),
			   drv_pci_device_product(device),
			   drv_pci_device_subvendor(device),
			   drv_pci_device_subproduct(device),
			   drv_pci_device_revision(device));
	}
}

/* Supports the xhci restore bar operation. */

/* Supports the xhci restore bar operation. */
static int
xhci_restore_bar(
	struct xhci_controller *controller)
{
	int function_result;
	struct drv_pci_bar current;
	int error;

	/* Handles the controller condition. */
	if (!controller->original_bar_valid)
		return 0;

	/* Checks the operation status. */
	error = drv_pci_device_bar(controller->pci, 0, &current);
	if (error != 0)
		return error;

	/* Handles the current condition. */
	if (current.bus_address == controller->original_bar.bus_address)
		return 0;

	/* Obtains the drv pci device assign bar result. */
	function_result = drv_pci_device_assign_bar(
		controller->pci, 0, controller->original_bar.bus_address);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the xhci bus master disable operation. */

/* Supports the xhci bus master disable operation. */
static int
xhci_bus_master_disable(
	struct xhci_controller *controller)
{
	uint16_t command;
	int error;

	/* Checks the operation status. */
	error = drv_pci_device_set_bus_master(controller->pci, false);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_pci_device_config_read16(controller->pci, XHCI_PCI_COMMAND,
					     &command);
	if (error != 0)
		return error;

	/* Returns the computed result. */
	return (command & XHCI_PCI_COMMAND_MASTER) == 0 ? 0 : EIO;
}

/* Supports the xhci pci quiesce operation. */

/* Supports the xhci pci quiesce operation. */
static int
xhci_pci_quiesce(
	struct xhci_controller *controller)
{
	uint16_t command;
	int error;

	/* Checks the operation status. */
	error = drv_pci_device_config_read16(controller->pci, XHCI_PCI_COMMAND,
					     &command);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_pci_device_config_write16(
		controller->pci, XHCI_PCI_COMMAND,
		(uint16_t)(command & ~XHCI_PCI_COMMAND_ENABLE));
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = drv_pci_device_config_read16(controller->pci, XHCI_PCI_COMMAND,
					     &command);
	if (error != 0)
		return error;

	/* Returns the computed result. */
	return (command & XHCI_PCI_COMMAND_ENABLE) == 0 ? 0 : EIO;
}

/* Supports the xhci legacy release operation. */

/* Supports the xhci legacy release operation. */
static void
xhci_legacy_release(
	struct xhci_controller *controller)
{
	uint32_t control;
	uint8_t owned;

	/* Checks the drv xhci region fits result. */
	if (!controller->legacy_claimed || controller->capability == NULL ||
	    !drv_xhci_region_fits(controller->mapping.size,
				  controller->legacy_offset, 8U)) {
		/* Returns the computed result. */
		return;
	}
	control = rd32(controller->capability, controller->legacy_offset + 4U);
	control = drv_xhci_legacy_control_restore(control,
						  controller->legacy_control);
	wr32(controller->capability, controller->legacy_offset + 4U, control);
	owned = rd8(controller->capability, controller->legacy_offset + 3U);
	wr8(controller->capability, controller->legacy_offset + 3U,
	    owned & (uint8_t)~1U);
	controller->legacy_claimed = 0;
}

/* Supports the xhci pci release operation. */

/* Supports the xhci pci release operation. */
static int
xhci_pci_release(
	struct xhci_controller *controller)
{
	int error, bar_error = 0, master_error = 0, quiesce_error;

	/* Handles the controller condition. */
	if (controller->pci_state_saved || controller->bar_mapped)
		master_error = xhci_bus_master_disable(controller);

	/* Checks the operation status. */
	if (master_error != 0) {
		hal_printf("xhci: PCI bus-master disable failed (%d)\n",
			   master_error);

		/* Returns the computed result. */
		return master_error;
	}

	xhci_legacy_release(controller);

	/* Handles the controller condition. */
	if (controller->bar_mapped) {
		drv_pci_device_unmap_bar(controller->pci, &controller->mapping);
		controller->bar_mapped = 0;
	}

	/* Checks the operation status. */
	bar_error = xhci_restore_bar(controller);
	if (bar_error != 0) {
		hal_printf("xhci: BAR0 restore failed (%d)\n", bar_error);

		/* Checks the operation status. */
		quiesce_error = xhci_pci_quiesce(controller);
		if (quiesce_error != 0) {
			hal_printf("xhci: PCI quiesce after BAR failure failed "
				   "(%d)\n",
				   quiesce_error);
		}

		/* Returns the computed result. */
		return bar_error;
	}

	/* Handles the controller condition. */
	if (controller->pci_state_saved) {
		/* Checks the operation status. */
		error = drv_pci_device_restore_enable_state(
			controller->pci, &controller->pci_enable_state);
		if (error != 0) {
			hal_printf("xhci: PCI command restore failed (%d)\n",
				   error);

			/* Checks the operation status. */
			quiesce_error = xhci_pci_quiesce(controller);
			if (quiesce_error != 0) {
				hal_printf("xhci: PCI command failure quiesce "
					   "failed (%d)\n",
					   quiesce_error);
			}

			/* Returns the computed result. */
			return error;
		}

		controller->pci_state_saved = 0;
	}

	/* Handles the controller condition. */
	if (controller->bar_claimed) {
		drv_pci_device_release_bar(controller->pci, 0);
		controller->bar_claimed = 0;
	}

	/* Reports successful completion. */
	return 0;
}

/* Counts first entry into retained controller ownership, not retained bytes. */
/* Supports the xhci mark quarantined operation. */
static void
xhci_mark_quarantined(
	struct xhci_controller *controller)
{
	/* Handles the controller condition. */
	if (!controller->quarantined)
		io_stats_record(IO_XHCI_QUARANTINE, 0);
	controller->quarantined = 1;
}

/* Supports the xhci quarantine operation. */

/* Supports the xhci quarantine operation. */
static void
xhci_quarantine(
	struct xhci_controller *controller,
	const char *stage,
	int error)
{
	/* Handles the controller condition. */
	if (!controller->quarantined) {
		xhci_mark_quarantined(controller);
		drv_pci_device_set_driver_data(controller->pci, controller);
		controller->next = controllers;
		controllers = controller;
	}

	hal_printf("xhci: attach quarantined at %s (%d); controller ownership "
		   "retained\n",
		   stage, error);
}

/* Supports the ring alloc operation. */

/* Supports the ring alloc operation. */
static int
ring_alloc(
	struct xhci_controller *c,
	struct xhci_ring *r)
{
	int e = drv_dma_alloc_coherent(c->hcd.dma, 4096U, 64U, &r->dma);

	/* Handles the e condition. */
	if (e)
		return e;
	memset(r->dma.address, 0, 4096U);
	r->trbs = r->dma.address;
	r->enqueue = 0;
	r->cycle = 1;

	/* Reports successful completion. */
	return 0;
}
/* Supports the ring free operation. */

/* Supports the ring free operation. */
static void
ring_free(
	struct xhci_controller *c,
	struct xhci_ring *r)
{
	/* Handles the r condition. */
	if (r->dma.address)
		drv_dma_free_coherent(c->hcd.dma, &r->dma);
	memset(r, 0, sizeof(*r));
}
/* Supports the ring push operation. */

/* Supports the ring push operation. */
static uint64_t
ring_push(
	struct xhci_ring *r,
	uint64_t parameter,
	uint32_t status,
	uint32_t control)
{
	struct xhci_trb *t = &r->trbs[r->enqueue];
	uint64_t trb_address =
		r->dma.device_address + (uint64_t)r->enqueue * sizeof(*t);

	/* Fills the block before the cycle bit hands it over. */
	t->parameter_low = (uint32_t)parameter;
	t->parameter_high = (uint32_t)(parameter >> 32);
	t->status = status;
	hal_io_wmb();
	t->control = control | (r->cycle ? XHCI_TRB_CYCLE : 0);
	hal_io_wmb();

	/* Handles the r condition. */
	if (++r->enqueue == XHCI_RING_TRBS - 1U) {
		t = &r->trbs[r->enqueue];
		t->parameter_low = (uint32_t)r->dma.device_address;
		t->parameter_high = (uint32_t)(r->dma.device_address >> 32);
		t->status = 0;
		hal_io_wmb();
		t->control = XHCI_TRB_TYPE(6) | 0x2U |
			     (control & XHCI_TRB_CHAIN) | (r->cycle ? 1U : 0U);
		hal_io_wmb();
		r->enqueue = 0;
		r->cycle ^= 1U;
	}

	/* Returns the computed result. */
	return trb_address;
}

/* Supports the event take operation. */

/* Supports the event take operation. */
static int
event_take(
	struct xhci_controller *c,
	struct xhci_trb *out)
{
	struct xhci_trb *t = &c->events[c->event_dequeue];
	uint32_t control = t->control;

	/* Handles the control condition. */
	if ((control & 1U) != c->event_cycle)
		return 0;
	hal_io_rmb();
	*out = *t;
	/* Classifies the current input character. */
	if (++c->event_dequeue == XHCI_RING_TRBS) {
		c->event_dequeue = 0;
		c->event_cycle ^= 1U;
	}

	wr64(c->runtime, 0x38U,
	     (c->event_memory.device_address + c->event_dequeue * sizeof(*t)) |
		     8U);

	/* Reports operation failure. */
	return 1;
}

/* Supports the event lock operation. */

/* Supports the event lock operation. */
static void
event_lock(
	struct xhci_controller *c)
{
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&c->event_busy, 1U, __ATOMIC_ACQUIRE))
		hal_compiler_barrier();
}

/* Supports the event unlock operation. */

/* Supports the event unlock operation. */
static void
event_unlock(
	struct xhci_controller *c)
{
	__atomic_store_n(&c->event_busy, 0U, __ATOMIC_RELEASE);
}

/* Supports the port change defer operation. */

/* Supports the port change defer operation. */
static void
port_change_defer(
	struct xhci_controller *c)
{
	struct thread *worker;

	/* Classifies the current input character. */
	if (!c->root_ready)
		return;
	c->port_pending = 1U;

	/* Handles the worker condition. */
	worker = c->port_worker;
	if (worker)
		kernel_notify_task(worker->task);
}

/* Supports the xhci event pointer operation. */

/* Supports the xhci event pointer operation. */
static uint64_t
xhci_event_pointer(
	const struct xhci_trb *event)
{
	/* Returns the computed result. */
	return (uint64_t)event->parameter_low |
	       ((uint64_t)event->parameter_high << 32);
}

/* Supports the command ex operation. */

/* Supports the command ex operation. */
static int
command_ex(
	struct xhci_controller *c,
	uint64_t parameter,
	uint32_t status,
	uint32_t control,
	unsigned *slot,
	unsigned *completion)
{
	int available;
	struct xhci_trb event;
	unsigned n, type;
	uint64_t command_address;
	uint32_t iman;
	bool enabled = hal_irq_disable();
	int result = ETIMEDOUT;

	/* Handles the completion condition. */
	if (completion)
		*completion = 0;
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&c->command_busy, 1U, __ATOMIC_ACQUIRE)) {
		/* Handles the enabled condition. */
		if (enabled)
			hal_irq_enable();
		sched_yield();
		enabled = hal_irq_disable();
	}

	/* Checks the operation status. */
	if (c->dma_quiesced || c->command_failed) {
		__atomic_store_n(&c->command_busy, 0U, __ATOMIC_RELEASE);

		/* Handles the enabled condition. */
		if (enabled)
			hal_irq_enable();

		/* Returns the computed result. */
		return EIO;
	}

	/* Interrupter 0 and the polling path share the event-ring consumer. */
	iman = rd32(c->runtime, 0x20U);
	wr32(c->runtime, 0x20U, iman & ~2U);
	command_address = ring_push(&c->command, parameter, status, control);
	event_lock(c);
	c->command_address = command_address;
	c->command_event_ready = 0;
	event_unlock(c);
	wr32(c->doorbells, 0, 0);
	/* Process each element required by the operation. */
	for (n = 0; n < XHCI_TIMEOUT; n++) {
		event_lock(c);

		/* Classifies the current input character. */
		if (c->command_event_ready) {
			event = c->command_event;
			c->command_event_ready = 0;
			available = 1;
		} else {
			available = event_take(c, &event);
		}

		/* Handles the available condition. */
		type = available ? (event.control >> 10) & 0x3fU : 0;
		if (available && type == 32U)
			(void)transfer_claim(c, &event);
		event_unlock(c);

		/* Handles the available condition. */
		if (!available)
			continue;

		/* Handles the type condition. */
		if (type == 32U)
			continue;

		/* Handles the type condition. */
		if (type == 34U) {
			port_change_defer(c);
			continue;
		}

		/* Handles the type condition. */
		if (type != 33U)
			continue;

		/* Checks the drv xhci command completion matches result. */
		if (!drv_xhci_command_completion_matches(
			    command_address, xhci_event_pointer(&event))) {
			hal_printf("xhci: ignored command completion for "
				   "%x:%x, expected %x:%x\n",
				   event.parameter_high, event.parameter_low,
				   (uint32_t)(command_address >> 32),
				   (uint32_t)command_address);
			continue;
		}

		/* Handles the slot condition. */
		if (slot)
			*slot = event.control >> 24;
		/* Handles the completion condition. */
		if (completion)
			*completion = (event.status >> 24) & 0xffU;

		/* Checks the operation result. */
		result = ((event.status >> 24) & 0xffU) == 1U ? 0 : EIO;
		if (result) {
			hal_printf("xhci: command %u failed, completion=%u\n",
				   (control >> 10) & 0x3fU,
				   (event.status >> 24) & 0xffU);
		}

		break;
	}

	event_lock(c);
	c->command_address = 0;
	c->command_event_ready = 0;
	event_unlock(c);
	wr32(c->runtime, 0x20U, (iman & 2U) | 1U);

	/* Checks the operation result. */
	if (result == ETIMEDOUT)
		c->command_failed = 1;
	__atomic_store_n(&c->command_busy, 0U, __ATOMIC_RELEASE);

	/* Handles the enabled condition. */
	if (enabled)
		hal_irq_enable();

	/*
 * Transfer Events consumed while polling are claimed before event_lock
	 * is released, but callbacks are deferred until the command gate is
	 * open. */
	xhci_completion_drain(c);

	/* Checks the operation result. */
	if (result == ETIMEDOUT) {
		hal_printf("xhci: command %u timed out\n",
			   (control >> 10) & 0x3fU);
	}

	/* Returns the computed result. */
	return result;
}

/* Supports the command operation. */

/* Supports the command operation. */
static int
command(
	struct xhci_controller *c,
	uint64_t parameter,
	uint32_t status,
	uint32_t control,
	unsigned *slot)
{
	int error;

	/* Obtains the command ex result. */
	error = command_ex(c, parameter, status, control, slot, NULL);

	/* Returns the computed result. */
	return error;
}

/* Supports the ownership operation. */

/* Supports the ownership operation. */
static int
ownership(
	struct xhci_controller *c)
{
	uint32_t control;
	uint8_t owned;
	unsigned count;
	uint32_t cap;
	unsigned id;
	unsigned next;
	int step;
	uint32_t hcc = rd32(c->capability, 0x10U);
	unsigned offset = ((hcc >> 16) & 0xffffU) * 4U;

	/* Checks the drv xhci region fits result. */
	if (offset != 0 && (offset < 0x20U ||
			    !drv_xhci_region_fits(c->mapping.size, offset, 4U))) {
		/* Returns the computed result. */
		return EIO;
	}
	/* Continue while the operation condition remains true. */
	while (offset != 0) {
		cap = rd32(c->capability, offset);

		/* Handles the id condition. */
		id = cap & 0xffU;
		if (id == 0U || id == 0xffU)
			return EIO;

		/* Handles the id condition. */
		if (id == 1U) {
			/* Classifies the current input character. */
			if (c->legacy_claimed)
				return EIO;

			/* Checks the drv xhci region fits result. */
			if (!drv_xhci_region_fits(c->mapping.size, offset, 8U))
				return EIO;
			c->legacy_offset = offset;
			c->legacy_control = rd32(c->capability, offset + 4U);
			owned = rd8(c->capability, offset + 3U);
			wr8(c->capability, offset + 3U, owned | 1U);
			c->legacy_claimed = 1;
			/* Process each remaining element. */
			for (count = 0; count < XHCI_TIMEOUT; count++) {
				/* Checks the drv xhci legacy ownership ready result. */
				if (drv_xhci_legacy_ownership_ready(
					    rd8(c->capability, offset + 2U),
					    rd8(c->capability, offset + 3U)))
					break;
			}

			/* Checks the remaining item count. */
			if (count == XHCI_TIMEOUT) {
				xhci_legacy_release(c);

				/* Returns the computed result. */
				return ETIMEDOUT;
			}

			control = rd32(c->capability, offset + 4U);
			wr32(c->capability, offset + 4U,
			     drv_xhci_legacy_control_disable(control));

			/* Checks the rd32 result. */
			if ((rd32(c->capability, offset + 4U) &
			     DRV_XHCI_LEGACY_SMI_ENABLE) != 0) {
				xhci_legacy_release(c);

				/* Returns the computed result. */
				return EIO;
			}
		}

		/* Handles the step condition. */
		step = drv_xhci_extended_capability_next(c->mapping.size,
							 offset, cap, &next);
		if (step < 0)
			return EIO;

		/* Handles the step condition. */
		if (step == 0)
			return 0;
		offset = next;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the fill slot operation. */

/* Supports the fill slot operation. */
static void
fill_slot(
	struct xhci_controller *c,
	struct xhci_device *d,
	void *context,
	unsigned entries)
{
	uint32_t *w = context;

	memset(context, 0, c->context_size);
	w[0] = ((d->speed_id & 15U) << 20) | ((entries & 31U) << 27);
	w[1] = drv_usb_device_port(d->usb) << 16;
}
/* Supports the fill endpoint operation. */

/* Supports the fill endpoint operation. */
static void
fill_endpoint(
	struct xhci_controller *c,
	void *context,
	struct xhci_endpoint *ep,
	const struct drv_xhci_endpoint_context_words *encoded)
{
	uint32_t *w = context;
	uint64_t dequeue = ep->ring.dma.device_address +
			   (uint64_t)ep->ring.enqueue * sizeof(struct xhci_trb);

	/* Points the endpoint context at the ring it drives. */
	dequeue |= ep->ring.cycle ? 1U : 0U;
	memset(context, 0, c->context_size);
	w[0] = encoded->word0;
	w[1] = encoded->word1;
	w[2] = (uint32_t)dequeue;
	w[3] = (uint32_t)(dequeue >> 32);
	w[4] = encoded->word4;
}
/* Supports the xhci usb device operation. */

/* Supports the xhci usb device operation. */
static struct xhci_device *
xhci_usb_device(
	struct drv_usb_device *u)
{
	struct xhci_device *device;

	device = (struct xhci_device *)drv_usb_device_hcd_data(u, 0);

	/* Returns the computed result. */
	return device != NULL && device->usb == u ? device : NULL;
}

/* Supports the xhci slot device locked operation. */

/* Supports the xhci slot device locked operation. */
static struct xhci_device *
xhci_slot_device_locked(
	struct xhci_controller *c,
	unsigned slot)
{
	struct xhci_device *device;

	/* Process each linked entry. */
	for (device = c->devices; device != NULL; device = device->next) {
		/* Handles the device condition. */
		if (device->slot == slot)
			return device;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the xhci event request locked operation. */

/* Supports the xhci event request locked operation. */
static struct xhci_request *
xhci_event_request_locked(
	struct xhci_controller *c,
	const struct xhci_trb *event,
	unsigned *trb_offset)
{
	struct xhci_device *device;
	struct xhci_endpoint *endpoint;
	struct xhci_request *request;
	uint64_t pointer = (uint64_t)event->parameter_low |
			   ((uint64_t)event->parameter_high << 32);
	unsigned slot = event->control >> 24;
	unsigned dci = (event->control >> 16) & 31U;

	/* Handles the dci condition. */
	if (dci == 0 || dci >= 32U)
		return NULL;

	/* Handles the device availability. */
	device = xhci_slot_device_locked(c, slot);
	if (device == NULL)
		return NULL;
	endpoint = &device->endpoints[dci];

	/* Checks the drv xhci transfer event matches result. */
	request = endpoint->active;
	if (request == NULL || request->device != device ||
	    request->endpoint != endpoint ||
	    !drv_xhci_transfer_event_matches(
		    endpoint->ring.dma.device_address, XHCI_RING_TRBS,
		    request->slot, request->dci, request->first_trb,
		    request->trb_count, pointer, slot, dci, trb_offset)) {
		/* Reports that no result is available. */
		return NULL;
	}

	/* Returns the computed result. */
	return request;
}

/* Supports the xhci request publish locked operation. */

/* Supports the xhci request publish locked operation. */
static int
xhci_request_publish_locked(
	struct xhci_controller *c,
	struct xhci_request *request)
{
	/* Handles the active availability. */
	if (request->endpoint->active != NULL)
		return EBUSY;

	/* Classifies the current input character. */
	if (c->active_count == UINT_MAX)
		__builtin_trap();
	request->endpoint->active = request;
	c->active_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci request unlink locked operation. */

/* Supports the xhci request unlink locked operation. */
static int
xhci_request_unlink_locked(
	struct xhci_controller *c,
	struct xhci_request *request)
{
	/* Handles the request condition. */
	if (request->endpoint->active != request)
		return 0;

	/* Classifies the current input character. */
	if (c->active_count == 0)
		__builtin_trap();
	request->endpoint->active = NULL;
	c->active_count--;

	/* Reports operation failure. */
	return 1;
}

/* Supports the xhci recovery leave locked operation. */

/* Supports the xhci recovery leave locked operation. */
static void
xhci_recovery_leave_locked(
	struct xhci_controller *c,
	struct xhci_endpoint *endpoint)
{
	/* Handles the endpoint condition. */
	if (!endpoint->recovering || endpoint->stall_publishing ||
	    c->endpoint_recoveries_busy == 0)
		__builtin_trap();
	endpoint->recovering = 0;
	c->endpoint_recoveries_busy--;
}

/* Supports the xhci device recovery busy locked operation. */

/* Supports the xhci device recovery busy locked operation. */
static int
xhci_device_recovery_busy_locked(
	const struct xhci_device *device)
{
	unsigned dci;

	/* Process each element required by the operation. */
	for (dci = 1; dci < 32U; dci++) {
		/* Handles the device condition. */
		if (device->endpoints[dci].recovering)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci device request busy locked operation. */

/* Supports the xhci device request busy locked operation. */
static int
xhci_device_request_busy_locked(
	const struct xhci_device *device)
{
	unsigned dci;

	/* Process each element required by the operation. */
	for (dci = 1; dci < 32U; dci++) {
		/* Handles the active availability. */
		if (device->endpoints[dci].active != NULL)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci default owner release operation. */

/* Supports the xhci default owner release operation. */
static void
xhci_default_owner_release(
	struct xhci_controller *c,
	struct xhci_device *d)
{
	unsigned expected;

	/* Handles the d availability. */
	if (d == NULL || !d->default_owned)
		return;

	/* Checks the hal atomic compare exchange acq rel result. */
	expected = d->slot;
	if (hal_atomic_compare_exchange_acq_rel(&c->default_slot, &expected,
						0U))
		d->default_owned = 0;
}

/* Supports the xhci device release operation. */

/* Supports the xhci device release operation. */
static void
xhci_device_release(
	struct drv_usb_hcd *h,
	struct xhci_device *d)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device **link;
	unsigned i;
	unsigned long irq;

	/* Handles the d availability. */
	if (d == NULL || !d->slot_disabled) {
		hal_printf(
			"xhci: refusing device release before Disable Slot\n");

		/* Returns the computed result. */
		return;
	}

	xhci_default_owner_release(c, d);

	/* Checks the xhci device request busy locked result. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (xhci_device_request_busy_locked(d) ||
	    xhci_device_recovery_busy_locked(d) || d->completions_busy != 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		hal_printf("xhci: slot %u release crossed an active ownership "
			   "boundary\n",
			   d->slot);
		__builtin_trap();
	}

	/* Checks the xhci usb device result. */
	if (xhci_usb_device(d->usb) == d)
		(void)drv_usb_device_set_hcd_data(d->usb, 0, 0);
	/* Process each linked entry. */
	for (link = &c->devices; *link != NULL; link = &(*link)->next) {
		/* Handles the link condition. */
		if (*link == d) {
			*link = d->next;
			break;
		}
	}

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Handles the address availability. */
	if (c->dcbaa.address != NULL && d->slot <= c->max_slots) {
		((uint64_t *)c->dcbaa.address)[d->slot] = 0;
		hal_io_wmb();
	}

	/* Process each element required by the operation. */
	for (i = 1; i < 32; i++)
		ring_free(c, &d->endpoints[i].ring);

	/* Handles the address availability. */
	if (d->input_context.address != NULL)
		drv_dma_free_coherent(h->dma, &d->input_context);

	/* Handles the address availability. */
	if (d->output_context.address != NULL)
		drv_dma_free_coherent(h->dma, &d->output_context);
	hal_free(d);
}

/* Supports the xhci device enable operation. */

/* Supports the xhci device enable operation. */
static int
xhci_device_enable(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	unsigned expected;
	unsigned completion;
	int disable_error;
	struct xhci_controller *c = hcd_controller(h);
	struct drv_xhci_endpoint_context_words endpoint_context;
	struct xhci_device *d;
	uint32_t *control;
	uint8_t *input;
	unsigned packet;
	unsigned long irq;
	uint32_t portsc;
	int e;
	unsigned slot = 0;

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&c->default_slot) != 0)
		return EBUSY;

	/* Checks the current descriptor. */
	d = hal_malloc(sizeof(*d));
	if (!d)
		return ENOMEM;
	memset(d, 0, sizeof(*d));
	d->usb = u;

	/* Checks the drv usb device port result. */
	if (drv_usb_device_port(u) == 0 || drv_usb_device_port(u) > c->ports) {
		hal_free(d);

		/* Returns the computed result. */
		return EINVAL;
	}

	portsc = rd32(c->operational, XHCI_PORTSC(drv_usb_device_port(u) - 1U));
	d->speed_id = drv_xhci_port_speed_id(portsc);

	/* Handles the portsc condition. */
	if (portsc == UINT32_MAX || d->speed_id == 0) {
		hal_free(d);

		/* Returns the computed result. */
		return EIO;
	}

	/* Checks the command result. */
	if ((e = command(c, 0, 0, XHCI_TRB_TYPE(9), &slot)) != 0 || slot == 0) {
		hal_free(d);

		/* Returns the computed result. */
		return e ? e : EIO;
	}

	d->slot = slot;

	/*
 * Publish partial ownership immediately.  Every later failure must pass
	 * through checked Disable Slot before any controller-visible DMA is
	 * freed. */
	irq = spin_lock_irqsave(&c->active_lock);

	d->next = c->devices;
	c->devices = d;
	(void)drv_usb_device_set_hcd_data(u, 0, (uintptr_t)d);

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Checks the drv dma alloc coherent result. */
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U,
					&d->output_context)) != 0)
		goto fail;

	/* Checks the drv dma alloc coherent result. */
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U,
					&d->input_context)) != 0)
		goto fail;

	/* Checks the ring alloc result. */
	if ((e = ring_alloc(c, &d->endpoints[1].ring)) != 0)
		goto fail;
	d->endpoints[1].dci = 1;
	d->endpoints[1].enabled = 1;
	memset(d->output_context.address, 0, 4096U);
	memset(d->input_context.address, 0, 4096U);
	((uint64_t *)c->dcbaa.address)[slot] = d->output_context.device_address;
	input = d->input_context.address;
	control = (uint32_t *)input;
	control[1] = 3U;
	fill_slot(c, d, input + c->context_size, 1);

	/* Checks the drv xhci endpoint context encode result. */
	packet = drv_usb_device_speed(u) >= DRV_USB_SPEED_SUPER	 ? 512U
		 : drv_usb_device_speed(u) >= DRV_USB_SPEED_HIGH ? 64U
								 : 8U;
	if (!drv_xhci_endpoint_context_encode(drv_usb_device_speed(u), 4U,
					      (uint16_t)packet, 0, NULL,
					      &endpoint_context)) {
		e = EINVAL;
		goto fail;
	}

	fill_endpoint(c, input + 2U * c->context_size, &d->endpoints[1],
		      &endpoint_context);

	/* Checks the hal atomic compare exchange acq rel result. */
	expected = 0;
	if (!hal_atomic_compare_exchange_acq_rel(&c->default_slot, &expected,
						 slot)) {
		e = EBUSY;
		goto fail;
	}

	d->default_owned = 1U;

	/* Handles the e condition. */
	e = command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(11) | (1U << 9) | XHCI_TRB_SLOT(slot), NULL);
	if (e)
		goto fail;
	d->context_entries = 1;

	/* Reports successful completion. */
	return 0;
fail:
	completion = 0;
	disable_error =
		command_ex(c, 0, 0, XHCI_TRB_TYPE(10) | XHCI_TRB_SLOT(slot),
			   NULL, &completion);

	/* Checks the operation status. */
	if (disable_error == 0) {
		d->slot_disabled = 1;
		xhci_device_release(h, d);
	} else {
		hal_printf("xhci: slot %u enable rollback failed (%d, "
			   "completion=%u); contexts retained\n",
			   slot, disable_error, completion);
	}

	/* Returns the computed result. */
	return e;
}

/* Supports the xhci set address operation. */

/* Supports the xhci set address operation. */
static int
xhci_set_address(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u,
	unsigned address)
{
	int error;
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = xhci_usb_device(u);
	const struct drv_usb_device_descriptor *descriptor;
	struct drv_xhci_ep0_context_words ep0;
	uint8_t *input;
	uint32_t *control;
	uint64_t dequeue;
	uint16_t packet;
	unsigned attempt;

	(void)address;

	/* Checks the current descriptor. */
	if (!d)
		return ENODEV;

	/* Checks the current descriptor. */
	if (d->quiescing)
		return ENODEV;
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 1000U; attempt++) {
		/* Checks the atomic raw load acquire result. */
		if (atomic_raw_load_acquire(&c->completion_busy) == 0)
			break;
		sched_yield();
	}

	/* Handles the attempt condition. */
	if (attempt == 1000U)
		return EBUSY;

	/* Checks the drv xhci ep0 max packet size result. */
	descriptor = drv_usb_device_descriptor(u);
	if (descriptor == NULL ||
	    !drv_xhci_ep0_max_packet_size(drv_usb_device_speed(u),
					  descriptor->endpoint0_max_packet_size,
					  &packet)) {
		/* Returns the computed result. */
		return EIO;
	}
	dequeue = d->endpoints[1].ring.dma.device_address +
		  (uint64_t)d->endpoints[1].ring.enqueue *
			  sizeof(struct xhci_trb);
	dequeue |= d->endpoints[1].ring.cycle ? 1U : 0U;

	/* Checks the drv xhci ep0 context result. */
	if (!drv_xhci_ep0_context(packet, dequeue, &ep0))
		return EIO;
	input = d->input_context.address;
	memset(input, 0, 4096U);
	control = (uint32_t *)input;
	control[1] = 3U;
	fill_slot(c, d, input + c->context_size, 1U);
	memset(input + 2U * c->context_size, 0, c->context_size);
	memcpy(input + 2U * c->context_size, ep0.words, sizeof(ep0.words));

	/* Checks the operation status. */
	error = command(c, d->input_context.device_address, 0,
			XHCI_TRB_TYPE(11) | XHCI_TRB_SLOT(d->slot), NULL);
	if (error == 0)
		xhci_default_owner_release(c, d);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci device disable operation. */

/* Supports the xhci device disable operation. */
static void
xhci_device_disable(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	struct xhci_device *d = xhci_usb_device(u);

	/* Handles the d availability. */
	if (d == NULL)
		return;

	/* Checks the current descriptor. */
	if (!d->slot_disabled) {
		hal_printf("xhci: slot %u release requested before checked "
			   "teardown; retaining contexts\n",
			   d->slot);

		/* Returns the computed result. */
		return;
	}

	xhci_device_release(h, d);
}

/* Supports the xhci endpoint enable operation. */

/* Supports the xhci endpoint enable operation. */
static int
xhci_endpoint_enable(
	struct drv_usb_hcd *h,
	struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = xhci_usb_device(drv_usb_endpoint_device(usbep));
	struct drv_xhci_endpoint_context_words endpoint_context;
	struct xhci_endpoint *ep;
	const struct drv_usb_superspeed_endpoint_companion_descriptor
		*companion;
	const struct drv_usb_endpoint_descriptor *desc;
	uint8_t *input;
	uint32_t *control;
	unsigned number, dci, entries, type;
	int ring_allocated = 0;
	int e;

	/* Checks the current descriptor. */
	if (!d)
		return ENODEV;
	desc = drv_usb_endpoint_descriptor(usbep);
	number = desc->address & 15U;

	/* Handles the dci condition. */
	dci = number * 2U + ((desc->address & DRV_USB_DIR_IN) ? 1U : 0U);
	if (dci < 2U || dci >= 32U)
		return EINVAL;

	/* Handles the ep condition. */
	ep = &d->endpoints[dci];
	if (ep->enabled)
		return 0;
	/* Dispatch the selected syntax or record type. */
	switch (drv_usb_endpoint_type(usbep)) {
	case DRV_USB_TRANSFER_ISOCHRONOUS:
		type = (desc->address & 0x80U) ? 5U : 1U;
		break;
	case DRV_USB_TRANSFER_BULK:
		type = (desc->address & 0x80U) ? 6U : 2U;
		break;
	case DRV_USB_TRANSFER_INTERRUPT:
		type = (desc->address & 0x80U) ? 7U : 3U;
		break;
	default:
		type = 4U;
		break;
	}

	/* Checks the drv xhci endpoint context encode result. */
	companion = drv_usb_endpoint_superspeed_companion(usbep);
	if (!drv_xhci_endpoint_context_encode(drv_usb_device_speed(d->usb),
					      type, desc->maximum_packet_size,
					      desc->interval, companion,
					      &endpoint_context)) {
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Handles the address availability. */
	if (ep->ring.dma.address == NULL) {
		/* Checks the ring alloc result. */
		if ((e = ring_alloc(c, &ep->ring)) != 0)
			return e;
		ring_allocated = 1;
	} else if (ep->dci != dci) {
		/* Returns the computed result. */
		return EIO;
	}

	ep->dci = dci;
	input = d->input_context.address;
	memset(input, 0, 4096U);
	control = (uint32_t *)input;
	control[1] = 1U | (1U << dci);
	entries = dci > d->context_entries ? dci : d->context_entries;
	fill_slot(c, d, input + c->context_size, entries);
	fill_endpoint(c, input + (dci + 1U) * c->context_size, ep,
		      &endpoint_context);

	/* Handles the e condition. */
	e = command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(12) | XHCI_TRB_SLOT(d->slot), NULL);
	if (e) {
		/* Handles the ring allocated condition. */
		if (ring_allocated) {
			ring_free(c, &ep->ring);
			ep->dci = 0;
		}

		/* Returns the computed result. */
		return e;
	}

	d->context_entries = entries;
	ep->enabled = 1;

	/* Reports successful completion. */
	return 0;
}
/* Supports the xhci endpoint disable operation. */

/* Supports the xhci endpoint disable operation. */
static int
xhci_endpoint_disable(
	struct drv_usb_hcd *h,
	struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = xhci_usb_device(drv_usb_endpoint_device(usbep));
	struct xhci_endpoint *ep;
	uint8_t *input;
	uint32_t *control;
	unsigned dci, entries;
	unsigned long irq;
	int error;

	/* Checks the current descriptor. */
	if (!d)
		return ENODEV;

	/* Handles the dci condition. */
	dci = (drv_usb_endpoint_address(usbep) & 15U) * 2U +
	      (drv_usb_endpoint_is_input(usbep) ? 1U : 0U);
	if (dci < 2U || dci >= 32U)
		return EINVAL;

	/* Checks the current descriptor. */
	if (!d->endpoints[dci].enabled)
		return 0;
	ep = &d->endpoints[dci];

	/* Handles the active availability. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (ep->active != NULL || ep->recovering || d->quiescing) {
		error = d->quiescing ? ENODEV : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return error;
	}

	/* Classifies the current input character. */
	if (c->endpoint_recoveries_busy == UINT_MAX)
		__builtin_trap();
	ep->recovering = 1U;
	c->endpoint_recoveries_busy++;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Process each element required by the operation. */
	for (entries = 31U; entries > 1U; entries--) {
		/* Handles the entries condition. */
		if (entries != dci && d->endpoints[entries].enabled)
			break;
	}

	input = d->input_context.address;
	memset(input, 0, 4096U);
	control = (uint32_t *)input;
	control[0] = 1U << dci;
	control[1] = 1U;
	fill_slot(c, d, input + c->context_size, entries);
	error = command(c, d->input_context.device_address, 0,
			XHCI_TRB_TYPE(12) | XHCI_TRB_SLOT(d->slot), NULL);
	irq = spin_lock_irqsave(&c->active_lock);

	/* Checks the operation status. */
	if (error == 0)
		ep->enabled = 0;
	xhci_recovery_leave_locked(c, ep);

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	d->context_entries = entries;

	/* Reports successful completion. */
	return 0;
}

/* event_lock is held across this ownership claim.  Therefore cancellation, Disable Slot, and ring reuse cannot pass an event which was dequeued but had not yet acquired its endpoint owner. */
/* Supports the transfer claim operation. */
static int
transfer_claim(
	struct xhci_controller *c,
	const struct xhci_trb *event)
{
	struct xhci_request *request;
	struct xhci_device *device;
	const struct drv_usb_control_request *control_request;
	unsigned long irq;
	unsigned code = (event->status >> 24) & 0xffU;
	size_t residual = event->status & 0x00ffffffU, actual;
	unsigned trb_offset;
	int normal_short_valid = 1;

	irq = spin_lock_irqsave(&c->active_lock);

	/* Handles the request availability. */
	request = xhci_event_request_locked(c, event, &trb_offset);
	if (request == NULL) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the request condition. */
	if (request->cancelling) {
		request->transfer_seen = 1U;
		request->completion_code = code;
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Reports operation failure. */
		return 1;
	}

	control_request = drv_usb_urb_control_request(request->urb);
	actual = residual < request->length ? request->length - residual : 0;

	/* Checks the drv xhci control short data event result. */
	if (control_request != NULL &&
	    drv_xhci_control_short_data_event(request->input, trb_offset,
					      request->trb_count, code)) {
		request->short_seen = 1U;
		request->short_actual = actual;
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the control request availability. */
	if (control_request == NULL && request->input && code == 13U) {
		/* Handles the vector availability. */
		if (request->vector != NULL) {
			normal_short_valid = xhci_sg_short(request, trb_offset,
							   residual, &actual);
		} else {
			normal_short_valid = drv_xhci_normal_short_actual(
				request->bounce.device_address, request->length,
				trb_offset, residual, &actual);
		}
	}

	/* Handles the request condition. */
	if (request->short_seen)
		actual = request->short_actual;
	request->terminal_status =
		(code == 1U || (control_request == NULL && code == 13U &&
				request->input && normal_short_valid))
			? DRV_USB_URB_COMPLETE
		: code == 6U ? DRV_USB_URB_STALL
			     : DRV_USB_URB_IO_ERROR;
	request->completion_code = code;
	request->completion_actual = actual;
	request->completion_residual = residual;
	request->completion_trb_offset = trb_offset;
	request->completion_next = NULL;

	/* Checks the drv usb endpoint type result. */
	if (request->terminal_status == DRV_USB_URB_STALL &&
	    control_request == NULL &&
	    (drv_usb_endpoint_type(drv_usb_urb_endpoint(request->urb)) ==
		     DRV_USB_TRANSFER_BULK ||
	     drv_usb_endpoint_type(drv_usb_urb_endpoint(request->urb)) ==
		     DRV_USB_TRANSFER_INTERRUPT)) {
		/*
 * Keep HCD admission closed until drv_usb_hcd_complete() has
		 * latched the STALL in the USB core.  Once the active request
		 * is unlinked, this marker is the only HCD-side barrier against
		 * a same-endpoint TD slipping into the completion-publication
		 * window. */
		if (request->endpoint->recovering ||
		    request->endpoint->stall_publishing ||
		    c->endpoint_recoveries_busy == UINT_MAX)
			__builtin_trap();
		request->endpoint->recovering = 1U;
		request->endpoint->stall_publishing = 1U;
		c->endpoint_recoveries_busy++;
		request->stall_publication = 1U;
	}

	/* Checks the atomic raw fetch add relaxed result. */
	if (atomic_raw_fetch_add_relaxed(&c->completion_busy, 1U) == UINT_MAX)
		__builtin_trap();

	/* Checks the xhci request unlink locked result. */
	device = request->device;
	if (device->completions_busy == UINT_MAX ||
	    !xhci_request_unlink_locked(c, request))
		__builtin_trap();
	device->completions_busy++;

	/* Handles the completion tail availability. */
	if (c->completion_tail != NULL)
		c->completion_tail->completion_next = request;
	else
		c->completion_head = request;
	c->completion_tail = request;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Reports operation failure. */
	return 1;
}

/* Supports the xhci completion finish operation. */

/* Supports the xhci completion finish operation. */
static void
xhci_completion_finish(
	struct xhci_controller *c,
	struct xhci_request *request)
{
	struct xhci_device *device = request->device;
	struct drv_usb_urb *urb = request->urb;
	const struct drv_usb_control_request *control_request =
		drv_usb_urb_control_request(urb);
	enum drv_usb_urb_status terminal_status = request->terminal_status;
	struct xhci_endpoint *stall_endpoint =
		request->stall_publication ? request->endpoint : NULL;
	size_t completion_actual = request->completion_actual;
	unsigned long irq;

	/* Handles the request condition. */
	if (request->terminal_status != DRV_USB_URB_COMPLETE ||
	    (request->completion_residual != 0 &&
	     request->completion_code != 13U)) {
		/* Handles the control request availability. */
		if (control_request != NULL) {
			hal_printf(
				"xhci: control port=%u slot=%u request=%02x "
				"type=%02x value=%04x index=%04x stage=%u "
				"completion=%u residual=%u length=%u "
				"state=%u\n",
				request->port, request->slot,
				control_request->request,
				control_request->request_type,
				control_request->value, control_request->index,
				request->completion_trb_offset,
				request->completion_code,
				(unsigned)request->completion_residual,
				(unsigned)request->length,
				xhci_endpoint_state(c, device, request->dci));
		} else {
			hal_printf("xhci: transfer completion=%u residual=%u "
				   "length=%u slot=%u endpoint=%u port=%u "
				   "direction=%s\n",
				   request->completion_code,
				   (unsigned)request->completion_residual,
				   (unsigned)request->length, request->slot,
				   request->dci, request->port,
				   request->input ? "in" : "out");
		}
	}

	/* Checks the drv usb urb buffer result. */
	if (request->terminal_status == DRV_USB_URB_COMPLETE &&
	    request->input && request->completion_actual != 0 &&
	    drv_usb_urb_buffer(urb) != request->staging) {
		memcpy(drv_usb_urb_buffer(urb), request->staging,
		       request->completion_actual);
		io_stats_record(IO_XHCI_BOUNCE_COPY,
				request->completion_actual);
	}

	drv_usb_urb_set_hcd_data(urb, NULL);

	/*
 * Return request/DMA ownership before terminal publication, allowing a
	 * callback to submit a different URB immediately.  The completed URB
	 * stays HCD-owned until its own callback returns. */
	xhci_request_release(c, request);
	drv_usb_hcd_complete(&c->hcd, urb, terminal_status, completion_actual);

	irq = spin_lock_irqsave(&c->active_lock);

	/*
 * transfer_claim incremented this device's completions_busy before
	 * unlink; xhci_device_quiesce cannot release the endpoint graph until
	 * the marker is cleared and that checked owner is dropped below. */
	if (stall_endpoint != NULL) {
		/* Handles the stall endpoint condition. */
		if (!stall_endpoint->stall_publishing)
			__builtin_trap();
		stall_endpoint->stall_publishing = 0U;
		xhci_recovery_leave_locked(c, stall_endpoint);
	}

	/* Handles the device condition. */
	if (device->completions_busy == 0)
		__builtin_trap();
	device->completions_busy--;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Checks the atomic raw fetch add release result. */
	if (atomic_raw_fetch_add_release(&c->completion_busy, (unsigned)-1) ==
	    0)
		__builtin_trap();
}

/* Supports the xhci completion drain operation. */
/* Supports the xhci completion drain operation. */
static void
xhci_completion_drain(
	struct xhci_controller *c)
{
	struct xhci_request *request;
	unsigned long irq;

	/* Classifies the current input character. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (c->completion_dispatch_busy) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return;
	}

	c->completion_dispatch_busy = 1U;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the request availability. */
		request = c->completion_head;
		if (request == NULL) {
			c->completion_tail = NULL;
			c->completion_dispatch_busy = 0;
			spin_unlock_irqrestore(&c->active_lock, irq);

			/* Returns the computed result. */
			return;
		}

		c->completion_head = request->completion_next;

		/* Handles the completion head availability. */
		if (c->completion_head == NULL)
			c->completion_tail = NULL;
		request->completion_next = NULL;
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_completion_finish(c, request);
		irq = spin_lock_irqsave(&c->active_lock);
	}
}
/* Supports the xhci irq operation. */

/* Supports the xhci irq operation. */
static int
xhci_irq(
	void *argument)
{
	uint64_t pointer;
	unsigned type;
	struct xhci_controller *c = argument;
	struct xhci_trb event;
	int available;
	int handled = 0;
	uint32_t status;

	/* Checks the atomic raw fetch add relaxed result. */
	if (atomic_raw_fetch_add_relaxed(&c->irq_busy, 1U) == UINT_MAX)
		__builtin_trap();

	/* Checks the operation status. */
	status = rd32(c->operational, XHCI_USBSTS);
	if (!(status & (XHCI_STS_EINT | XHCI_STS_FATAL)))
		goto out;
	wr32(c->operational, XHCI_USBSTS, status);
	wr32(c->runtime, 0x20U, rd32(c->runtime, 0x20U) | 1U);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		event_lock(c);
		available = event_take(c, &event);

		/* Handles the available condition. */
		type = available ? (event.control >> 10) & 0x3fU : 0;
		if (available && type == 32U)
			(void)transfer_claim(c, &event);
		event_unlock(c);

		/* Handles the available condition. */
		if (!available)
			break;
		handled = 1;

		/* Handles the type condition. */
		if (type == 33U) {
			pointer = xhci_event_pointer(&event);

			event_lock(c);

			/* Checks the atomic load n result. */
			if (__atomic_load_n(&c->command_busy,
					    __ATOMIC_ACQUIRE) != 0 &&
			    drv_xhci_command_completion_matches(
				    c->command_address, pointer) &&
			    !c->command_event_ready) {
				c->command_event = event;
				c->command_event_ready = 1;
			} else {
				hal_printf("xhci: unmatched command completion "
					   "%x:%x\n",
					   event.parameter_high,
					   event.parameter_low);
			}

			event_unlock(c);
		} else if (type == 34U)
			port_change_defer(c);
	}

	/*
 * A polling command owns command_busy and will drain every claim after
	 * it releases that gate.  Never run a command-recursive callback from
	 * the competing IRQ path while the command is still in flight. */
	if (__atomic_load_n(&c->command_busy, __ATOMIC_ACQUIRE) == 0)
		xhci_completion_drain(c);
out:

	/* Checks the atomic raw fetch add release result. */
	if (atomic_raw_fetch_add_release(&c->irq_busy, (unsigned)-1) == 0)
		__builtin_trap();

	/* Returns the computed result. */
	return handled;
}

/* Supports the xhci port worker operation. */

/* Supports the xhci port worker operation. */
static void
xhci_port_worker(
	void *argument)
{
	struct xhci_controller *c = argument;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Classifies the current input character. */
		if (c->port_stopping)
			return;

		/* Checks the atomic exchange n result. */
		if (__atomic_exchange_n(&c->port_pending, 0U,
					__ATOMIC_ACQ_REL)) {
			drv_usb_hcd_root_hub_changed(&c->hcd);
			continue;
		}

		kernel_wait_task();
	}
}

/* Supports the xhci worker start operation. */

/* Supports the xhci worker start operation. */
static int
xhci_worker_start(
	struct xhci_controller *c)
{
	struct thread *worker;
	int error;

	c->port_stopping = 0;
	c->port_pending = 0;

	/* Checks the operation status. */
	error = kthread_create(xhci_port_worker, c, SCHED_PRIORITY_DEFAULT,
			       &worker);
	if (error)
		return error;
	c->port_worker = worker;
	thread_start(worker);

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci worker stop operation. */

/* Supports the xhci worker stop operation. */
static void
xhci_worker_stop(
	struct xhci_controller *c)
{
	struct thread *worker = c->port_worker;

	/* Handles the worker condition. */
	if (!worker)
		return;
	c->port_worker = NULL;
	c->port_stopping = 1;
	kernel_notify_task(worker->task);
	/* Continue while the operation condition remains true. */
	while (worker->state != THREAD_ZOMBIE)
		sched_yield();
	(void)thread_wait(worker, NULL);
}

/* Supports the normal trb count operation. */

/* Supports the normal trb count operation. */
static unsigned
normal_trb_count(
	uint64_t address,
	size_t length)
{
	size_t chunk;
	unsigned count = 0;

	/* Checks the current data length. */
	if (length == 0)
		return 1;
	/* Process each remaining element. */
	while (length != 0) {
		/* Handles the chunk condition. */
		chunk = 0x10000U - (size_t)(address & 0xffffU);
		if (chunk > length)
			chunk = length;
		address += chunk;
		length -= chunk;
		count++;
	}

	/* Returns the computed result. */
	return count;
}

/* Supports the enqueue normal operation. */

/* Supports the enqueue normal operation. */
static uint64_t
enqueue_normal(
	struct xhci_ring *ring,
	uint64_t address,
	size_t length,
	int input,
	size_t maximum_packet_size,
	int zero_packet)
{
	uint64_t function_result;
	size_t chunk;
	uint32_t control;
	unsigned td_size;
	int final;
	unsigned count = normal_trb_count(address, length);
	size_t cumulative = 0;
	size_t total_length = length;
	uint64_t final_trb = 0;

	/* Checks the current data length. */
	if (length == 0) {
		/* Obtains the ring push result. */
		function_result = ring_push(ring, address, 0,
					    XHCI_TRB_TYPE(1) | XHCI_TRB_IOC);

		/* Returns the computed result. */
		return function_result;
	}
	while (length != 0) {
		chunk = 0x10000U - (size_t)(address & 0xffffU);
		control = XHCI_TRB_TYPE(1);

		/* Handles the chunk condition. */
		if (chunk > length)
			chunk = length;
		count--;
		cumulative += chunk;

		/* Validates the current input. */
		if (input)
			control |= DRV_XHCI_TRB_ISP;

		/* Handles the final condition. */
		final = count == 0 && !zero_packet;
		if (!final)
			control |= XHCI_TRB_CHAIN;
		else
			control |= XHCI_TRB_IOC;
		td_size = drv_xhci_normal_td_size(total_length, cumulative,
						  maximum_packet_size, final);

		/*
 * A requested terminating zero packet is one more packet in
		 * this TD. The final payload TRB therefore reports one packet
		 * remaining instead of looking terminal to the controller. */
		if (zero_packet && td_size < 31U)
			td_size++;
		final_trb =
			ring_push(ring, address,
				  (uint32_t)chunk | (td_size << 17), control);
		address += chunk;
		length -= chunk;
	}

	/* Handles the zero packet condition. */
	if (zero_packet) {
		final_trb = ring_push(ring, address, 0,
				      XHCI_TRB_TYPE(1) | XHCI_TRB_IOC);
	}

	/* Returns the computed result. */
	return final_trb;
}

/* Supports the xhci endpoint restart empty operation. */

/* Supports the xhci endpoint restart empty operation. */
static int
xhci_endpoint_restart_empty(
	struct xhci_controller *c,
	struct xhci_device *d,
	struct xhci_endpoint *endpoint,
	unsigned dci)
{
	uint64_t deadline;
	unsigned state;

	/* Handles the address availability. */
	if (endpoint->ring.dma.address == NULL)
		return EIO;

	/*
 * Set TR Dequeue points at the software producer, whose cycle bit
	 * denotes an empty ring.  Ring the endpoint and require hardware to
	 * publish Running before either recovery succeeds or cancelled DMA
	 * ownership is released. */
	wr32(c->doorbells, d->slot * 4U, dci);
	deadline = sched_ticks() + 100U;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the state condition. */
		state = xhci_endpoint_state(c, d, dci);
		if (state == DRV_XHCI_ENDPOINT_RUNNING)
			return 0;

		/* Checks the operation status. */
		if (state == DRV_XHCI_ENDPOINT_DISABLED ||
		    state == DRV_XHCI_ENDPOINT_HALTED ||
		    state == DRV_XHCI_ENDPOINT_ERROR) {
			/* Returns the computed result. */
			return EIO;
		}

		/* Classifies the current input character. */
		if (c->controller_stopping || c->dma_quiesced || d->quiescing)
			return ENODEV;

		/* Checks the sched ticks result. */
		if (sched_ticks() >= deadline)
			return ETIMEDOUT;
		sched_yield();
	}
}

/* Supports the xhci endpoint recover operation. */

/* Supports the xhci endpoint recover operation. */
static int
xhci_endpoint_recover(
	struct xhci_controller *c,
	struct xhci_device *d,
	struct xhci_endpoint *endpoint,
	unsigned dci)
{
	enum drv_xhci_cancel_action action;
	enum drv_xhci_cancel_action previous_action =
		DRV_XHCI_CANCEL_QUIESCE_CONTROLLER;
	unsigned state = UINT32_MAX, previous_state = UINT32_MAX;
	unsigned attempt, completion = 0;
	uint64_t dequeue;
	int error = EIO;

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 8U; attempt++) {
		state = xhci_endpoint_state(c, d, dci);

		/* Handles the attempt condition. */
		action = drv_xhci_recovery_action(
			(enum drv_xhci_endpoint_state)state);
		if (attempt != 0 && state == previous_state &&
		    action == previous_action) {
			error = EIO;
			break;
		}

		previous_state = state;
		previous_action = action;
		completion = 0;
		/* Dispatch the selected operation case. */
		switch (action) {
		case DRV_XHCI_CANCEL_COMPLETE:
			/* Reports successful completion. */
			return 0;
		case DRV_XHCI_CANCEL_RESET_ENDPOINT:
			error = command_ex(c, 0, 0,
					   XHCI_TRB_TYPE(14) | (dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
			break;
		case DRV_XHCI_CANCEL_SET_TR_DEQUEUE:
			/* Handles the address availability. */
			if (endpoint->ring.dma.address == NULL)
				return EIO;
			dequeue = endpoint->ring.dma.device_address +
				  (uint64_t)endpoint->ring.enqueue *
					  sizeof(struct xhci_trb);
			dequeue |= endpoint->ring.cycle ? 1U : 0U;

			/* Checks the operation status. */
			error = command_ex(c, dequeue, 0,
					   XHCI_TRB_TYPE(16) | (dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
			if (error == 0) {
				/* Checks the operation status. */
				error = xhci_endpoint_restart_empty(
					c, d, endpoint, dci);
				if (error == 0)
					return 0;
			}

			break;
		case DRV_XHCI_CANCEL_STOP_ENDPOINT:
		case DRV_XHCI_CANCEL_QUIESCE_CONTROLLER:
		default:
			error = EIO;
			break;
		}

		/*
 * Completion 19 can be a state-transition race.  Re-read the
		 * output context, but never repeat a command against an
		 * unchanged state/action pair. */
		if (error != 0 && completion != 19U)
			break;
	}

	hal_printf("xhci: slot %u endpoint %u recovery failed (%d, "
		   "completion=%u, state=%u); new TD rejected\n",
		   d->slot, dci, error, completion, state);

	/* Returns the computed result. */
	return error != 0 ? error : EIO;
}

/* Supports the xhci endpoint reset operation. */

/* Supports the xhci endpoint reset operation. */
static int
xhci_endpoint_reset(
	struct drv_usb_hcd *h,
	struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d;
	struct xhci_endpoint *endpoint;
	enum drv_xhci_endpoint_reset_admission admission;
	uint64_t wait_started;
	unsigned dci;
	unsigned long irq;
	int error;

	/* Handles the usbep availability. */
	if (usbep == NULL)
		return EINVAL;

	/* Handles the d availability. */
	d = xhci_usb_device(drv_usb_endpoint_device(usbep));
	if (d == NULL)
		return ENODEV;

	/* Handles the dci condition. */
	dci = (drv_usb_endpoint_address(usbep) & 15U) * 2U +
	      (drv_usb_endpoint_is_input(usbep) ? 1U : 0U);
	if (dci < 2U || dci >= 32U)
		return EINVAL;
	endpoint = &d->endpoints[dci];
	wait_started = sched_ticks();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);

		/* Handles the address availability. */
		if (!endpoint->enabled || endpoint->dci != dci ||
		    endpoint->ring.dma.address == NULL || d->quiescing ||
		    c->controller_stopping || c->dma_quiesced) {
			error = !endpoint->enabled || d->quiescing ||
						c->controller_stopping ||
						c->dma_quiesced
					? ENODEV
					: EIO;
			spin_unlock_irqrestore(&c->active_lock, irq);

			/* Returns the computed result. */
			return error;
		}

		/* Handles the admission condition. */
		admission = drv_xhci_endpoint_reset_admit(
			endpoint->active != NULL, endpoint->recovering,
			endpoint->stall_publishing);
		if (admission == DRV_XHCI_ENDPOINT_RESET_ACQUIRE) {
			/* Classifies the current input character. */
			if (c->endpoint_recoveries_busy == UINT_MAX)
				__builtin_trap();
			endpoint->recovering = 1U;
			c->endpoint_recoveries_busy++;
			spin_unlock_irqrestore(&c->active_lock, irq);
			break;
		}

		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Handles the admission condition. */
		if (admission != DRV_XHCI_ENDPOINT_RESET_WAIT_PUBLICATION)
			return EBUSY;

		/*
 * drv_usb_urb_drain() may observe the core HCD reference drop
		 * just before this completion thread releases its STALL
		 * publication owner. Join only that bounded handoff; every
		 * other recovery remains EBUSY. */
		if (sched_ticks() - wait_started >= 100U)
			return EBUSY;
		sched_yield();
	}

	error = xhci_endpoint_recover(c, d, endpoint, dci);
	irq = spin_lock_irqsave(&c->active_lock);

	/* Checks the operation status. */
	if (error == 0 &&
	    (d->quiescing || c->controller_stopping || c->dma_quiesced))
		error = ENODEV;
	xhci_recovery_leave_locked(c, endpoint);

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reclaim may reach a USB-backed swap source after consuming the last free physical page.  A transfer on that path must not allocate the DMA page which is needed to free a page.  USB storage serializes its BOT stages, so one request and one bounded coherent buffer reserved at start are sufficient for its reclaim-safe transfers even while unrelated endpoints remain active.  URBs with normal reservations use their own request/DMA first. Other ordinary traffic, including persistent networking, uses the dynamic path. */

/* Supports the xhci request alloc operation. */
static struct xhci_request *
xhci_request_alloc(
	struct xhci_controller *c,
	struct drv_usb_hcd *h,
	size_t length,
	unsigned flags,
	struct drv_usb_urb *urb,
	int *error)
{
	struct xhci_request *request;
	enum drv_xhci_reserve_action action;
	unsigned long irq;
	int allocation_error;

	struct xhci_urb_reservation *reservation;

	/*
 * Borrows this URB's normal reserve without consuming the reclaim
	 * reserve. */

	/* Handles the reservation availability. */
	reservation = drv_usb_urb_transfer_reservation(urb);
	if (reservation != NULL) {
		/* Handles the reservation condition. */
		irq = spin_lock_irqsave(&c->active_lock);
		if (reservation->busy || length > reservation->capacity) {
			*error = reservation->busy ? EBUSY : EMSGSIZE;
			spin_unlock_irqrestore(&c->active_lock, irq);

			/* Reports that no result is available. */
			return NULL;
		}

		/* Handles the reservation condition. */
		if (reservation->generation == UINT64_MAX) {
			*error = EOVERFLOW;
			spin_unlock_irqrestore(&c->active_lock, irq);

			/* Reports that no result is available. */
			return NULL;
		}

		reservation->generation++;
		reservation->busy = 1U;
		spin_unlock_irqrestore(&c->active_lock, irq);
		request = &reservation->request;
		memset(request, 0, sizeof(*request));
		request->reservation = reservation;
		request->reservation_generation = reservation->generation;
		request->bounce = reservation->backing;
		request->vector = reservation->vector;
		request->staging =
			reservation->vector != NULL
				? drv_dma_vector_address(reservation->vector)
				: reservation->backing.address;
		*error = 0;
		/* Returns the computed result. */
		return request;
	}

	/* Checks the active flags. */
	if ((flags & DRV_USB_URB_RECLAIM_SAFE) != 0) {
		irq = spin_lock_irqsave(&c->active_lock);

		/* Handles the action condition. */
		action = drv_xhci_reserve_action(
			1, length, c->transfer_reserve.size,
			c->transfer_reserve.address != NULL,
			c->transfer_reserve_busy != 0);
		if (action != DRV_XHCI_RESERVE_USE) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			*error = action == DRV_XHCI_RESERVE_BUSY ? EBUSY
				 : length > DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE
					 ? EMSGSIZE
					 : ENOMEM;

			/* Reports that no result is available. */
			return NULL;
		}

		c->transfer_reserve_busy = 1U;
		spin_unlock_irqrestore(&c->active_lock, irq);
		request = &c->transfer_request;
		memset(request, 0, sizeof(*request));
		request->reserved = 1U;
		request->bounce = c->transfer_reserve;
		request->staging = request->bounce.address;
		*error = 0;
		/* Returns the computed result. */
		return request;
	}

	/* Handles the request availability. */
	request = hal_malloc(sizeof(*request));
	if (request == NULL) {
		*error = ENOMEM;
		/* Reports that no result is available. */
		return NULL;
	}

	io_stats_record(IO_XHCI_REQUEST_ALLOC, sizeof(*request));
	memset(request, 0, sizeof(*request));

	/* Checks the operation status. */
	allocation_error = drv_dma_alloc_coherent(
		h->dma, length != 0 ? length : 8U, 64U, &request->bounce);
	if (allocation_error != 0) {
		hal_free(request);
		*error = allocation_error;
		/* Reports that no result is available. */
		return NULL;
	}

	*error = 0;
	request->staging = request->bounce.address;

	/* Returns the computed result. */
	return request;
}

/* Supports the xhci request release operation. */
/* Supports the xhci request release operation. */
static void
xhci_request_release(
	struct xhci_controller *c,
	struct xhci_request *request)
{
	unsigned long irq;

	struct xhci_urb_reservation *reservation;

	/*
 * Retires a normal reserved request while preserving its permanent
	 * backing. */

	/* Handles the reservation availability. */
	reservation = request->reservation;
	if (reservation != NULL) {
		irq = spin_lock_irqsave(&c->active_lock);

		/* Handles the endpoint availability. */
		if (!reservation->busy ||
		    request->reservation_generation !=
			    reservation->generation ||
		    (request->endpoint != NULL &&
		     request->endpoint->active == request)) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			__builtin_trap();
		}

		memset(request, 0, sizeof(*request));
		reservation->busy = 0;
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (!request->reserved) {
		drv_dma_free_coherent(c->hcd.dma, &request->bounce);
		hal_free(request);

		/* Returns the computed result. */
		return;
	}

	/* Handles the endpoint availability. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (request->endpoint != NULL && request->endpoint->active == request) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}

	/* Classifies the current input character. */
	if (!c->transfer_reserve_busy) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}

	memset(request, 0, sizeof(*request));
	c->transfer_reserve_busy = 0;

	spin_unlock_irqrestore(&c->active_lock, irq);
}

/* Supports the xhci submission enter operation. */

/* Supports the xhci submission enter operation. */
static int
xhci_submission_enter(
	struct xhci_controller *c)
{
	unsigned long irq;

	/* Classifies the current input character. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (c->controller_stopping || c->dma_quiesced) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return ENODEV;
	}

	/* Classifies the current input character. */
	if (c->submissions_busy == UINT_MAX) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}

	c->submissions_busy++;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Reports successful completion. */
	return 0;
}

/* HCD callbacks other than start/stop/quiesce may use controller MMIO, command/event rings, or device DMA without submitting an URB.  Track them independently so a port worker which entered enumeration just before USB shutdown cannot race the final controller-DMA release. */

/* Supports the xhci operation enter operation. */
static int
xhci_operation_enter(
	struct xhci_controller *c)
{
	unsigned long irq;

	/* Classifies the current input character. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (c->controller_stopping || c->dma_quiesced) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return ENODEV;
	}

	/* Classifies the current input character. */
	if (c->operations_busy == UINT_MAX) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}

	c->operations_busy++;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci operation leave operation. */

/* Supports the xhci operation leave operation. */
static void
xhci_operation_leave(
	struct xhci_controller *c)
{
	unsigned long irq;

	/* Classifies the current input character. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (c->operations_busy == 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}

	c->operations_busy--;

	spin_unlock_irqrestore(&c->active_lock, irq);
}

/* Reserves an idle URB's request and DMA, aligned to avoid a 64 KiB control boundary. */
/* Supports the xhci urb reserve operation. */
static int
xhci_urb_reserve(
	struct drv_usb_hcd *hcd,
	struct drv_usb_urb *urb,
	size_t capacity,
	void **result)
{
	struct xhci_controller *controller;
	struct xhci_urb_reservation *reservation;
	size_t alignment;
	int error;

	/* Holds the controller operation barrier while allocating backing. */
	if (result == NULL || capacity == 0 ||
	    capacity > DRV_USB_TRANSFER_RESERVE_MAX_SIZE) {
		/* Returns the computed result. */
		return EINVAL;
	}
	*result = NULL;
	controller = hcd_controller(hcd);

	/* Checks the operation status. */
	error = xhci_operation_enter(controller);
	if (error != 0)
		return error;

	/* Handles the reservation availability. */
	reservation = hal_malloc(sizeof(*reservation));
	if (reservation == NULL) {
		xhci_operation_leave(controller);

		/* Returns the computed result. */
		return ENOMEM;
	}

	memset(reservation, 0, sizeof(*reservation));
	reservation->capacity = capacity;

	/* Checks the drv usb urb endpoint result. */
	if (drv_usb_urb_endpoint(urb) != NULL &&
	    drv_usb_endpoint_type(drv_usb_urb_endpoint(urb)) ==
		    DRV_USB_TRANSFER_BULK) {
		/* Checks the operation status. */
		error = drv_dma_vector_create(hcd->dma, capacity,
					      &reservation->vector);
		if (error == 0) {
			*result = reservation;
			xhci_operation_leave(controller);

			/* Reports successful completion. */
			return 0;
		}

		/* Checks the operation status. */
		if (error != EOPNOTSUPP) {
			hal_free(reservation);
			xhci_operation_leave(controller);

			/* Returns the computed result. */
			return error;
		}
	}

	/*
 * Aligns each power-of-two bounded payload inside one transfer
	 * boundary. */
	alignment = 64U;
	/* Continue while the operation condition remains true. */
	while (alignment < capacity)
		alignment *= 2U;

	/* Checks the operation status. */
	error = drv_dma_alloc_coherent(hcd->dma, capacity, alignment,
				       &reservation->backing);
	if (error != 0) {
		hal_free(reservation);
		xhci_operation_leave(controller);

		/* Returns the computed result. */
		return error;
	}

	*result = reservation;
	xhci_operation_leave(controller);

	/* Returns the idle reservation to its URB owner. */
	return 0;
}

/* Shares the reservation's lifetime-stable CPU staging with USB core. */
/* Supports the xhci urb reserve buffer operation. */
static void *
xhci_urb_reserve_buffer(
	struct drv_usb_hcd *hcd,
	void *opaque,
	size_t *capacity)
{
	void *function_result;
	struct xhci_urb_reservation *reservation;

	(void)hcd;

	/* Handles the reservation availability. */
	reservation = opaque;
	if (reservation == NULL || capacity == NULL)
		return NULL;
	*capacity = reservation->capacity;
	/* Computes the function result. */
	function_result = reservation->vector != NULL
				  ? drv_dma_vector_address(reservation->vector)
				  : reservation->backing.address;

	/* Returns the computed result. */
	return function_result;
}

/* Frees an URB reservation only after all HCD references and DMA have retired. */
/* Supports the xhci urb unreserve operation. */
static void
xhci_urb_unreserve(
	struct drv_usb_hcd *hcd,
	void *opaque)
{
	struct xhci_urb_reservation *reservation;

	/* Handles the reservation availability. */
	reservation = opaque;
	if (reservation == NULL)
		return;

	/* Handles the reservation condition. */
	if (reservation->busy)
		__builtin_trap();

	/* Handles the vector availability. */
	if (reservation->vector != NULL) {
		/* Checks the drv dma vector free result. */
		if (drv_dma_vector_free(reservation->vector) != 0)
			HAL_FATAL("xHCI reservation DMA retirement failed");
	} else {
		drv_dma_free_coherent(hcd->dma, &reservation->backing);
	}

	hal_free(reservation);
}

/* Supports the xhci submission leave operation. */

/* Supports the xhci submission leave operation. */
static void
xhci_submission_leave(
	struct xhci_controller *c)
{
	unsigned long irq;

	/* Classifies the current input character. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (c->submissions_busy == 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}

	c->submissions_busy--;

	spin_unlock_irqrestore(&c->active_lock, irq);
}

/* Supports the xhci urb enqueue operation. */

/* Supports the xhci urb enqueue operation. */
static int
xhci_urb_enqueue(
	struct drv_usb_hcd *h,
	struct drv_usb_urb *u)
{
	uint64_t setup;
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = xhci_usb_device(drv_usb_urb_device(u));
	struct xhci_endpoint *ep;
	struct xhci_request *r;
	struct drv_xhci_trb_words setup_words = {0};
	struct drv_xhci_trb_words data_words = {0};
	struct drv_xhci_trb_words status_words = {0};
	enum drv_xhci_control_data control_data = DRV_XHCI_CONTROL_NO_DATA;
	const struct drv_usb_control_request *q =
		drv_usb_urb_control_request(u);
	size_t length = drv_usb_urb_length(u);
	uint64_t dma;
	unsigned dci, maximum_packet_size = 0, normal_count = 0;
	unsigned long irq;
	int e, input, zero_packet = 0;

	/* Handles the e condition. */
	e = xhci_submission_enter(c);
	if (e != 0)
		return e;

	/* Checks the current descriptor. */
	if (!d) {
		xhci_submission_leave(c);

		/* Returns the computed result. */
		return ENODEV;
	}

	/* Handles the dci condition. */
	dci = q ? 1U
		: ((drv_usb_endpoint_address(drv_usb_urb_endpoint(u)) & 15U) *
			   2U +
		   (drv_usb_endpoint_is_input(drv_usb_urb_endpoint(u)) ? 1U
								       : 0U));
	if (dci == 0 || dci >= 32U) {
		xhci_submission_leave(c);

		/* Returns the computed result. */
		return EINVAL;
	}

	ep = &d->endpoints[dci];

	/* Handles the q availability. */
	if (q == NULL) {
		/* Handles the maximum packet size condition. */
		maximum_packet_size = drv_usb_endpoint_max_packet_size(
					      drv_usb_urb_endpoint(u)) &
				      0x7ffU;
		if (maximum_packet_size == 0) {
			xhci_submission_leave(c);

			/* Returns the computed result. */
			return EINVAL;
		}
	}

	irq = spin_lock_irqsave(&c->active_lock);

	/* Handles the active availability. */
	if (!ep->enabled || d->quiescing || c->controller_stopping ||
	    ep->active != NULL || ep->recovering) {
		e = !ep->enabled || d->quiescing || c->controller_stopping
			    ? ENODEV
			    : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_submission_leave(c);

		/* Returns the computed result. */
		return e;
	}

	/* Classifies the current input character. */
	if (c->endpoint_recoveries_busy == UINT_MAX)
		__builtin_trap();
	ep->recovering = 1U;
	c->endpoint_recoveries_busy++;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Handles the r availability. */
	r = xhci_request_alloc(c, h, length, drv_usb_urb_flags(u), u, &e);
	if (r == NULL) {
		irq = spin_lock_irqsave(&c->active_lock);
		xhci_recovery_leave_locked(c, ep);
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_submission_leave(c);

		/* Returns the computed result. */
		return e;
	}

	r->urb = u;
	r->device = d;
	r->endpoint = ep;
	r->length = length;
	r->input = q ? (q->request_type & DRV_USB_DIR_IN) != 0
		     : drv_usb_endpoint_is_input(drv_usb_urb_endpoint(u));

	/* Checks the drv usb urb buffer result. */
	if (!r->input && length && r->staging != drv_usb_urb_buffer(u)) {
		memcpy(r->staging, drv_usb_urb_buffer(u), length);
		io_stats_record(IO_XHCI_BOUNCE_COPY, length);
	}

	/* Checks the drv usb urb buffer result. */
	if (length != 0 && r->staging == drv_usb_urb_buffer(u))
		io_stats_record(IO_XHCI_SHARED_STAGING, length);
	dma = r->bounce.device_address;
	input = r->input;

	/* Handles the q availability. */
	if (q == NULL) {
		/* Handles the vector availability. */
		if (r->vector != NULL) {
			/* Handles the e condition. */
			e = xhci_sg_plan(r, length);
			if (e != 0) {
				xhci_request_release(c, r);
				irq = spin_lock_irqsave(&c->active_lock);
				xhci_recovery_leave_locked(c, ep);
				spin_unlock_irqrestore(&c->active_lock, irq);
				xhci_submission_leave(c);

				/* Returns the computed result. */
				return e;
			}

			normal_count = r->normal_count;
		} else {
			normal_count = normal_trb_count(dma, length);
		}

		zero_packet =
			drv_usb_endpoint_type(drv_usb_urb_endpoint(u)) ==
				DRV_USB_TRANSFER_BULK &&
			!input && length != 0 &&
			(drv_usb_urb_flags(u) & DRV_USB_URB_ZERO_PACKET) != 0 &&
			length % maximum_packet_size == 0;
	}

	/* Handles the q condition. */
	if ((!q && (normal_count >= XHCI_RING_TRBS - 1U ||
		    (zero_packet && normal_count >= XHCI_RING_TRBS - 2U))) ||
	    (q && length != 0 &&
	     (length > 0x10000U || (dma & 0xffffU) + length > 0x10000U))) {
		xhci_request_release(c, r);
		irq = spin_lock_irqsave(&c->active_lock);
		xhci_recovery_leave_locked(c, ep);
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_submission_leave(c);

		/* Returns the computed result. */
		return EOVERFLOW;
	}

	/* Handles the q availability. */
	if (q != NULL) {
		setup = 0;

		memcpy(&setup, q, sizeof(*q));

		/* Checks the drv xhci control setup words result. */
		control_data = length == 0 ? DRV_XHCI_CONTROL_NO_DATA
			       : input	   ? DRV_XHCI_CONTROL_DATA_IN
					   : DRV_XHCI_CONTROL_DATA_OUT;
		if (!drv_xhci_control_setup_words(setup, control_data,
						  &setup_words) ||
		    (length != 0 &&
		     !drv_xhci_control_data_words(dma, (uint32_t)length,
						  control_data, &data_words)) ||
		    !drv_xhci_control_status_words(control_data,
						   &status_words)) {
			xhci_request_release(c, r);
			irq = spin_lock_irqsave(&c->active_lock);
			xhci_recovery_leave_locked(c, ep);
			spin_unlock_irqrestore(&c->active_lock, irq);
			xhci_submission_leave(c);

			/* Returns the computed result. */
			return EINVAL;
		}
	}

	r->slot = d->slot;
	r->dci = dci;
	r->port = drv_usb_device_port(drv_usb_urb_device(u));

	/*
 * EP0 retains implicit checked recovery because a control STALL is not
	 * core-latched.  Every nonzero endpoint must already be Running; only
	 * the explicit endpoint_reset callback may issue its recovery commands.
	 */
	e = q != NULL ? xhci_endpoint_recover(c, d, ep, dci)
	    : xhci_endpoint_state(c, d, dci) == DRV_XHCI_ENDPOINT_RUNNING ? 0
									  : EIO;
	irq = spin_lock_irqsave(&c->active_lock);

	/* Handles the active availability. */
	if (e != 0 || ep->active != NULL || d->quiescing ||
	    c->controller_stopping) {
		/* Handles the e condition. */
		if (e == 0) {
			e = d->quiescing || c->controller_stopping ? ENODEV
								   : EBUSY;
		}

		xhci_recovery_leave_locked(c, ep);
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_request_release(c, r);
		xhci_submission_leave(c);

		/* Returns the computed result. */
		return e;
	}

	r->first_trb = ep->ring.enqueue;
	r->trb_count = q != NULL ? (length ? 3U : 2U)
				 : normal_count + (zero_packet ? 1U : 0U);

	/* Checks the xhci request publish locked result. */
	if (xhci_request_publish_locked(c, r) != 0)
		__builtin_trap();

	/*
 * Publish the request, its URB association, the complete TD, and its
	 * doorbell under one barrier.  Teardown takes this same lock and must
	 * never observe a half-built request which it could cancel and free. */
	drv_usb_urb_set_hcd_data(u, r);

	/* Handles the q condition. */
	if (q) {
		ring_push(&ep->ring,
			  (uint64_t)setup_words.parameter_low |
				  ((uint64_t)setup_words.parameter_high << 32),
			  setup_words.status, setup_words.control);

		/* Checks the current data length. */
		if (length) {
			ring_push(&ep->ring,
				  (uint64_t)data_words.parameter_low |
					  ((uint64_t)data_words.parameter_high
					   << 32),
				  data_words.status, data_words.control);
		}

		(void)ring_push(
			&ep->ring,
			(uint64_t)status_words.parameter_low |
				((uint64_t)status_words.parameter_high << 32),
			status_words.status, status_words.control);
	} else {
		/* Handles the vector availability. */
		if (r->vector != NULL) {
			xhci_sg_enqueue(&ep->ring, r, input,
					maximum_packet_size, zero_packet);

			/* Handles the r condition. */
			if (r->normal_count > 1U)
				io_stats_record(IO_XHCI_SG_TD, length);
		} else {
			(void)enqueue_normal(&ep->ring, dma, length, input,
					     maximum_packet_size, zero_packet);
		}
	}

	wr32(c->doorbells, d->slot * 4U, dci);
	xhci_recovery_leave_locked(c, ep);

	spin_unlock_irqrestore(&c->active_lock, irq);

	xhci_submission_leave(c);

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci endpoint state operation. */
/* Supports the xhci endpoint state operation. */
static unsigned
xhci_endpoint_state(
	struct xhci_controller *c,
	struct xhci_device *d,
	unsigned dci)
{
	volatile uint32_t *context;

	/* Handles the d availability. */
	if (d == NULL || dci == 0 || dci >= 32U ||
	    d->output_context.address == NULL) {
		/* Returns the computed result. */
		return 7U;
	}
	context = (volatile uint32_t *)((uint8_t *)d->output_context.address +
					(size_t)dci * c->context_size);
	hal_io_rmb();

	/* Returns the computed result. */
	return context[0] & 7U;
}

/* Supports the xhci cancel request operation. */

/* Supports the xhci cancel request operation. */
static int
xhci_cancel_request(
	struct xhci_controller *c,
	struct xhci_device *d,
	struct xhci_request *r)
{
	struct xhci_endpoint *ep = r->endpoint;
	enum drv_xhci_cancel_action action;
	enum drv_xhci_cancel_action previous_action =
		DRV_XHCI_CANCEL_QUIESCE_CONTROLLER;
	enum drv_xhci_endpoint_state state;
	enum drv_xhci_endpoint_state previous_state =
		(enum drv_xhci_endpoint_state)UINT32_MAX;
	uint64_t dequeue;
	unsigned attempt, completion = 0;
	unsigned long irq;
	int error = EIO, releasable = 0;

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 8U; attempt++) {
		state = (enum drv_xhci_endpoint_state)xhci_endpoint_state(
			c, d, r->dci);

		/* Handles the attempt condition. */
		action = drv_xhci_cancel_action(state, c->dma_quiesced != 0);
		if (attempt != 0 && state == previous_state &&
		    action == previous_action) {
			error = EIO;
			goto retain;
		}

		previous_state = state;
		previous_action = action;
		completion = 0;
		/* Dispatch the selected operation case. */
		switch (action) {
		case DRV_XHCI_CANCEL_COMPLETE:
			releasable =
				drv_xhci_request_resources_releasable(0, 1);
			goto release;
		case DRV_XHCI_CANCEL_STOP_ENDPOINT:
			error = command_ex(c, 0, 0,
					   XHCI_TRB_TYPE(15) | (r->dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
			break;
		case DRV_XHCI_CANCEL_RESET_ENDPOINT:
			error = command_ex(c, 0, 0,
					   XHCI_TRB_TYPE(14) | (r->dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
			break;
		case DRV_XHCI_CANCEL_SET_TR_DEQUEUE:
			/*
 * Queue depth is one per endpoint.  The producer is the
			 * first safe dequeue position after this endpoint's
			 * cancelled TD.  Keep the owner published until this
			 * command completes so the same ring slots cannot be
			 * reused after a software-only unlink. */
			dequeue = ep->ring.dma.device_address +
				  (uint64_t)ep->ring.enqueue *
					  sizeof(struct xhci_trb);
			dequeue |= ep->ring.cycle ? 1U : 0U;

			/* Checks the operation status. */
			error = command_ex(c, dequeue, 0,
					   XHCI_TRB_TYPE(16) | (r->dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
			if (error == 0) {
				/*
 * Set TR Dequeue is the DMA-ownership boundary.
				 * A normal cancellation must restart the
				 * emptied endpoint before it can accept another
				 * TD.  Disconnect has permanently closed core
				 * admission, so demanding Running from an
				 * absent endpoint would retain an already
				 * retired request forever. */
				if (drv_xhci_cancel_post_dequeue_action(
					    drv_usb_device_is_tearing_down(
						    drv_usb_urb_device(
							    r->urb))) ==
				    DRV_XHCI_POST_DEQUEUE_RELEASE_REQUEST) {
					releasable =
						drv_xhci_request_resources_releasable(
							1, 0);
					goto release;
				}

				/* Checks the operation status. */
				error = xhci_endpoint_restart_empty(c, d, ep,
								    r->dci);
				if (error == 0) {
					releasable =
						drv_xhci_request_resources_releasable(
							1, 0);
					goto release;
				}
			}

			break;
		case DRV_XHCI_CANCEL_QUIESCE_CONTROLLER:
		default:
			error = EIO;
			goto retain;
		}

		/*
 * Context State Error means software raced a hardware state
		 * transition.  Re-read the output context; never blindly accept
		 * it. */
		if (error != 0 && completion != 19U)
			goto retain;
	}

	error = EIO;

retain:

	/* Handles the r condition. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (r->endpoint->active == r)
		r->cancelling = 2U;

	spin_unlock_irqrestore(&c->active_lock, irq);

	hal_printf("xhci: slot %u endpoint %u port %u cancel failed (%d, "
		   "completion=%u, state=%u); request and DMA retained\n",
		   r->slot, r->dci, r->port, error, completion,
		   (unsigned)state);

	/* Returns the computed result. */
	return error;

release:

	/* Handles the releasable condition. */
	if (!releasable)
		goto retain;

	/* Checks the xhci request unlink locked result. */
	irq = spin_lock_irqsave(&c->active_lock);
	if (!xhci_request_unlink_locked(c, r)) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return EBUSY;
	}

	spin_unlock_irqrestore(&c->active_lock, irq);

	drv_usb_urb_set_hcd_data(r->urb, NULL);
	xhci_request_release(c, r);

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci urb dequeue operation. */

/* Supports the xhci urb dequeue operation. */
static int
xhci_urb_dequeue(
	struct drv_usb_hcd *h,
	struct drv_usb_urb *u)
{
	int error;
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d;
	struct xhci_endpoint *endpoint;
	struct xhci_request *r;
	const struct drv_usb_control_request *control;
	unsigned dci;
	unsigned long irq;

	/* Handles the d availability. */
	d = xhci_usb_device(drv_usb_urb_device(u));
	if (d == NULL)
		return ENODEV;
	control = drv_usb_urb_control_request(u);

	/* Handles the dci condition. */
	dci = control != NULL
		      ? 1U
		      : (drv_usb_endpoint_address(drv_usb_urb_endpoint(u)) &
			 15U) * 2U +
				(drv_usb_endpoint_is_input(
					 drv_usb_urb_endpoint(u))
					 ? 1U
					 : 0U);
	if (dci == 0 || dci >= 32U)
		return EINVAL;
	endpoint = &d->endpoints[dci];
	irq = spin_lock_irqsave(&c->active_lock);

	/* Handles the r availability. */
	r = endpoint->active;
	if (r == NULL || r->urb != u || r->device != d || r->dci != dci) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return EBUSY;
	}

	/* Handles the r condition. */
	if (r->cancelling == 1U) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return EALREADY;
	}

	r->cancelling = 1U;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Obtains the xhci cancel request result. */
	error = xhci_cancel_request(c, d, r);

	/* Returns the computed result. */
	return error;
}

/* Supports the xhci endpoint quiesce operation. */

/* Supports the xhci endpoint quiesce operation. */
static int
xhci_endpoint_quiesce(
	struct xhci_controller *c,
	struct xhci_device *d,
	unsigned dci)
{
	struct xhci_endpoint *endpoint = &d->endpoints[dci];
	uint64_t dequeue;
	unsigned attempt, completion = 0;
	unsigned state = DRV_XHCI_ENDPOINT_DISABLED;
	unsigned previous_state = UINT32_MAX;
	int error;

	/* Handles the endpoint condition. */
	if (!endpoint->enabled || c->dma_quiesced)
		return 0;
	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 8U; attempt++) {
		/* Handles the attempt condition. */
		state = xhci_endpoint_state(c, d, dci);
		if (attempt != 0 && state == previous_state) {
			error = EIO;
			break;
		}

		previous_state = state;
		completion = 0;

		/* Handles the state condition. */
		if (state == DRV_XHCI_ENDPOINT_DISABLED)
			return 0;

		/* Handles the state condition. */
		if (state == DRV_XHCI_ENDPOINT_RUNNING) {
			error = command_ex(c, 0, 0,
					   XHCI_TRB_TYPE(15) | (dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
		} else if (state == DRV_XHCI_ENDPOINT_HALTED) {
			error = command_ex(c, 0, 0,
					   XHCI_TRB_TYPE(14) | (dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
		} else if (state == DRV_XHCI_ENDPOINT_STOPPED ||
			   state == DRV_XHCI_ENDPOINT_ERROR) {
			/* Handles the address availability. */
			if (endpoint->ring.dma.address == NULL)
				return EIO;
			dequeue = endpoint->ring.dma.device_address +
				  (uint64_t)endpoint->ring.enqueue *
					  sizeof(struct xhci_trb);
			dequeue |= endpoint->ring.cycle ? 1U : 0U;

			/* Checks the operation status. */
			error = command_ex(c, dequeue, 0,
					   XHCI_TRB_TYPE(16) | (dci << 16) |
						   XHCI_TRB_SLOT(d->slot),
					   NULL, &completion);
			if (error == 0)
				return 0;
		} else {
			error = EIO;
		}

		/* Checks the operation status. */
		if (error != 0 && completion != 19U)
			break;
	}

	hal_printf("xhci: slot %u endpoint %u teardown quiesce failed (%d, "
		   "completion=%u, state=%u); slot retained\n",
		   d->slot, dci, error, completion, state);

	/* Returns the computed result. */
	return error != 0 ? error : EIO;
}

/* Supports the xhci device quiesce operation. */

/* Supports the xhci device quiesce operation. */
static int
xhci_device_quiesce(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	struct xhci_request *candidate;
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = xhci_usb_device(u);
	struct xhci_request *r;
	struct drv_usb_urb *urb;
	uint32_t attempted = 0;
	unsigned completion = 0;
	unsigned dci, owned, wait_for_cancel;
	unsigned long irq;
	uint64_t wait_started;
	int error, first_error = 0;

	/* Handles the d availability. */
	if (d == NULL)
		return 0;

	/* Checks the current descriptor. */
	if (d->slot_disabled)
		return 0;

	/*
 * Close admission before inspecting endpoint ownership.  A submit which
	 * entered first either publishes a complete TD or leaves its endpoint's
	 * recovery barrier before this loop proceeds. */
	wait_started = sched_ticks();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		d->quiescing = 1U;

		/* Checks the xhci device recovery busy locked result. */
		if (!xhci_device_recovery_busy_locked(d) &&
		    d->completions_busy == 0) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			break;
		}

		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Checks the sched ticks result. */
		if (sched_ticks() - wait_started >= 100U) {
			hal_printf("xhci: slot %u teardown completion barrier "
				   "timed out; ownership retained\n",
				   d->slot);

			/* Returns the computed result. */
			return EBUSY;
		}

		sched_yield();
	}

	/*
 * Drain every endpoint owned by this device.  A failed endpoint remains
	 * published and therefore quarantined, but cannot prevent a different
	 * endpoint from reaching its own checked cancellation boundary. */
	wait_started = sched_ticks();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		r = NULL;
		urb = NULL;
		wait_for_cancel = 0;
		irq = spin_lock_irqsave(&c->active_lock);
		/* Process each element required by the operation. */
		for (dci = 1; dci < 32U; dci++) {
			/* Handles the candidate availability. */
			candidate = d->endpoints[dci].active;
			if (candidate == NULL || (attempted & (1U << dci)) != 0)
				continue;

			/* Handles the candidate condition. */
			if (candidate->cancelling == 1U) {
				wait_for_cancel = 1U;
				continue;
			}

			candidate->cancelling = 1U;
			attempted |= 1U << dci;
			r = candidate;
			urb = candidate->urb;
			break;
		}

		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Handles the r availability. */
		if (r != NULL) {
			/* Checks the operation status. */
			error = xhci_cancel_request(c, d, r);
			if (error != 0) {
				/* Checks the operation status. */
				if (first_error == 0)
					first_error = error;
			} else {
				/*
 * The owner and DMA were detached by Stop
				 * Endpoint plus Set TR Dequeue (or by a fully
				 * quiesced controller). */
				drv_usb_hcd_complete(
					h, urb, DRV_USB_URB_DISCONNECTED, 0);
			}

			continue;
		}

		/* Handles the wait for cancel condition. */
		if (!wait_for_cancel)
			break;

		/* Checks the sched ticks result. */
		if (sched_ticks() - wait_started >= 100U) {
			/* Checks the operation status. */
			if (first_error == 0)
				first_error = EBUSY;
			break;
		}

		sched_yield();
	}

	/* Checks the operation status. */
	if (first_error != 0)
		return first_error;

	/*
 * A user cancellation can detach the hardware owner just before its USB
	 * terminal publication.  Do not Disable Slot inside that publication
	 * window. */
	wait_started = sched_ticks();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		owned = drv_usb_device_hcd_urb_count(u);
		irq = spin_lock_irqsave(&c->active_lock);
		completion = d->completions_busy;
		wait_for_cancel = xhci_device_request_busy_locked(d) ||
				  xhci_device_recovery_busy_locked(d);
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Handles the owned condition. */
		if (owned == 0 && completion == 0 && !wait_for_cancel)
			break;

		/* Checks the sched ticks result. */
		if (sched_ticks() - wait_started >= 100U) {
			hal_printf("xhci: slot %u teardown timed out (URBs=%u "
				   "completion=%u endpoint=%u); ownership "
				   "retained\n",
				   d->slot, owned, completion, wait_for_cancel);

			/* Returns the computed result. */
			return EBUSY;
		}

		sched_yield();
	}

	/* Process each element required by the operation. */
	for (dci = 1; dci < 32U; dci++) {
		/* Checks the operation status. */
		error = xhci_endpoint_quiesce(c, d, dci);
		if (error != 0)
			return error;
	}

	/* Classifies the current input character. */
	if (c->dma_quiesced) {
		d->slot_disabled =
			drv_xhci_device_resources_releasable(0, 1) ? 1U : 0U;

		/* Checks the current descriptor. */
		if (d->slot_disabled)
			xhci_default_owner_release(c, d);

		/* Returns the computed result. */
		return d->slot_disabled ? 0 : EIO;
	}

	/* Checks the operation status. */
	error = command_ex(c, 0, 0, XHCI_TRB_TYPE(10) | XHCI_TRB_SLOT(d->slot),
			   NULL, &completion);
	if (error != 0) {
		hal_printf("xhci: slot %u Disable Slot failed (%d, "
			   "completion=%u); rings and contexts retained\n",
			   d->slot, error, completion);

		/* Returns the computed result. */
		return error;
	}

	/* Checks the drv xhci device resources releasable result. */
	if (!drv_xhci_device_resources_releasable(1, 0))
		return EIO;
	d->slot_disabled = 1U;
	xhci_default_owner_release(c, d);

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci frame operation. */

/* Supports the xhci frame operation. */
static uint32_t
xhci_frame(
	struct drv_usb_hcd *h)
{
	uint32_t function_result;
	struct xhci_controller *c = hcd_controller(h);

	/* Computes the function result. */
	function_result = rd32(c->runtime, 0) & 0x3fffU;

	/* Returns the computed result. */
	return function_result;
}
/* Supports the xhci root status operation. */

/* Supports the xhci root status operation. */
static int
xhci_root_status(
	struct drv_usb_hcd *h,
	void *b,
	size_t n,
	size_t *a)
{
	struct xhci_controller *c = hcd_controller(h);
	uint8_t *bits = b;
	unsigned p, bytes = (c->ports + 1U + 7U) / 8U;

	/* Handles the b condition. */
	if (!b || n < bytes)
		return EINVAL;
	memset(bits, 0, bytes);
	/* Process each element required by the operation. */
	for (p = 0; p < c->ports; p++) {
		/* Checks the rd32 result. */
		if (rd32(c->operational, XHCI_PORTSC(p)) & XHCI_PORT_CHANGE)
			bits[(p + 1U) / 8U] |= (uint8_t)(1U << ((p + 1U) & 7U));
	}

	/* Handles the a condition. */
	if (a)
		*a = bytes;
	/* Reports successful completion. */
	return 0;
}
/* Supports the xhci root control operation. */

/* Supports the xhci root control operation. */
static int
xhci_root_control(
	struct drv_usb_hcd *h,
	const struct drv_usb_control_request *r,
	void *b,
	size_t n,
	size_t *a)
{
	unsigned speed;
	uint32_t change;
	struct xhci_controller *c = hcd_controller(h);
	unsigned p;
	uint32_t s, v = 0;

	/* Handles the r condition. */
	if (!r || r->index < 1 || r->index > c->ports)
		return EINVAL;
	p = r->index - 1U;

	/* Handles the r condition. */
	s = rd32(c->operational, XHCI_PORTSC(p));
	if (r->request == 0 && b && n >= 4) {
		speed = (s >> 10) & 15U;

		/* Checks the current string state. */
		if (s & XHCI_PORT_CCS)
			v |= 1U;

		/* Checks the current string state. */
		if (s & XHCI_PORT_PED)
			v |= 2U;

		/* Checks the current string state. */
		if (s & XHCI_PORT_PR)
			v |= 0x10U;

		/* Checks the current string state. */
		if (s & XHCI_PORT_PP)
			v |= 0x100U;

		/* Handles the speed condition. */
		if (speed == 2U)
			v |= 0x200U;
		else if (speed == 3U)
			v |= 0x400U;
		else if (speed >= 4U)
			v |= 0x800U;

		/* Checks the current string state. */
		if (s & (1U << 17))
			v |= 0x10000U;

		/* Checks the current string state. */
		if (s & (1U << 18))
			v |= 0x20000U;

		/* Checks the current string state. */
		if (s & (1U << 19))
			v |= 0x200000U;

		/* Checks the current string state. */
		if (s & (1U << 20))
			v |= 0x80000U;

		/* Checks the current string state. */
		if (s & (1U << 21))
			v |= 0x100000U;

		/* Checks the current string state. */
		if (s & (1U << 22))
			v |= 0x400000U;

		/* Checks the current string state. */
		if (s & (1U << 23))
			v |= 0x800000U;
		memcpy(b, &v, 4);

		/* Handles the a condition. */
		if (a)
			*a = 4;
		/* Reports successful completion. */
		return 0;
	}

	/* Handles the r condition. */
	if (r->request == 3 && r->value == 4) {
		wr32(c->operational, XHCI_PORTSC(p),
		     (s & XHCI_PORT_PP) | XHCI_PORT_PR | XHCI_PORT_PP);

		/* Handles the a condition. */
		if (a)
			*a = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Handles the r condition. */
	if (r->request == 1) {
		/* Handles the r condition. */
		change = 0;
		if (r->value == 16)
			change = 1U << 17;
		else if (r->value == 17)
			change = 1U << 18;
		else if (r->value == 19)
			change = 1U << 20;
		else if (r->value == 20)
			change = 1U << 21;
		else if (r->value == 21)
			change = 1U << 19;
		else if (r->value == 22)
			change = 1U << 22;
		else if (r->value == 23)
			change = 1U << 23;
		else if (r->value != 4)

			/* Returns the computed result. */
			return ENOTSUP;
		wr32(c->operational, XHCI_PORTSC(p),
		     (s & XHCI_PORT_PP) | change);

		/* Handles the a condition. */
		if (a)
			*a = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Handles the r condition. */
	if (r->request == 3 && r->value == 8) {
		wr32(c->operational, XHCI_PORTSC(p), XHCI_PORT_PP);

		/* Handles the a condition. */
		if (a)
			*a = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Returns the computed result. */
	return ENOTSUP;
}

/* Supports the xhci root port reset operation. */

/* Supports the xhci root port reset operation. */
static int
xhci_root_port_reset(
	struct drv_usb_hcd *h,
	unsigned port)
{
	uint64_t recovery;
	struct xhci_controller *c = hcd_controller(h);
	enum drv_xhci_port_reset_decision decision;
	uint64_t deadline;
	uint32_t portsc;
	unsigned index;

	/* Handles the port condition. */
	if (port == 0 || port > c->ports)
		return EINVAL;
	index = port - 1U;

	/* Handles the portsc condition. */
	portsc = rd32(c->operational, XHCI_PORTSC(index));
	if (portsc == UINT32_MAX)
		return EIO;

	/* Handles the portsc condition. */
	if ((portsc & XHCI_PORT_CSC) != 0)
		return ENODEV;

	/* Handles the portsc condition. */
	if ((portsc & XHCI_PORT_CCS) == 0)
		return ENODEV;

	/* Handles the portsc condition. */
	if ((portsc & XHCI_PORT_CHANGE) != 0) {
		wr32(c->operational, XHCI_PORTSC(index),
		     (portsc & XHCI_PORT_PP) |
			     (portsc & (XHCI_PORT_CHANGE & ~XHCI_PORT_CSC)));
	}

	/* Handles the portsc condition. */
	portsc = rd32(c->operational, XHCI_PORTSC(index));
	if (portsc == UINT32_MAX)
		return EIO;

	/* Handles the portsc condition. */
	if ((portsc & XHCI_PORT_CSC) != 0 || (portsc & XHCI_PORT_CCS) == 0)
		return ENODEV;
	wr32(c->operational, XHCI_PORTSC(index),
	     (portsc & XHCI_PORT_PP) | XHCI_PORT_PP | XHCI_PORT_PR);
	deadline = sched_ticks() + 100U;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		portsc = rd32(c->operational, XHCI_PORTSC(index));

		/* Handles the decision condition. */
		decision = drv_xhci_port_reset_status(portsc);
		if (decision == DRV_XHCI_PORT_RESET_SUCCESS) {
			wr32(c->operational, XHCI_PORTSC(index),
			     (portsc & XHCI_PORT_PP) |
				     (portsc &
				      (XHCI_PORT_CHANGE & ~XHCI_PORT_CSC)));

			/* Handles the portsc condition. */
			portsc = rd32(c->operational, XHCI_PORTSC(index));
			if (portsc == UINT32_MAX)
				return EIO;

			/* Handles the portsc condition. */
			if ((portsc & XHCI_PORT_CSC) != 0 ||
			    (portsc & XHCI_PORT_CCS) == 0) {
				/* Returns the computed result. */
				return ENODEV;
			}

			/*
 * Two 10-ms ticks guarantee at least one full
			 * recovery interval even when reset completes
			 * on a tick boundary. */
			recovery = sched_ticks() + 2U;

			/* Continue while the operation condition remains true. */
			while (sched_ticks() < recovery)
				sched_yield();

			/*
 * The mandatory recovery delay is part of reset.
			 * Preserve a detach/reinsert edge which arrives during
			 * that interval too. */

			/* Handles the portsc condition. */
			portsc = rd32(c->operational, XHCI_PORTSC(index));
			if (portsc == UINT32_MAX)
				return EIO;

			/* Handles the portsc condition. */
			if ((portsc & XHCI_PORT_CSC) != 0 ||
			    (portsc & XHCI_PORT_CCS) == 0) {
				/* Returns the computed result. */
				return ENODEV;
			}
			hal_printf("xhci: port %u reset complete portsc=%08x\n",
				   port, portsc);

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the decision condition. */
		if (decision == DRV_XHCI_PORT_RESET_DISCONNECTED)
			return ENODEV;

		/* Handles the decision condition. */
		if (decision == DRV_XHCI_PORT_RESET_INVALID)
			return EIO;

		/* Checks the sched ticks result. */
		if (sched_ticks() >= deadline)
			break;
		sched_yield();
	}

	hal_printf("xhci: port %u reset timed out portsc=%08x\n", port, portsc);

	/* Returns the computed result. */
	return ETIMEDOUT;
}

/* Supports the xhci scratchpads free operation. */

/* Supports the xhci scratchpads free operation. */
static void
xhci_scratchpads_free(
	struct xhci_controller *c)
{
	unsigned i;

	/* Classifies the current input character. */
	if (c->scratchpads) {
		/* Process each remaining element. */
		for (i = 0; i < c->scratchpad_count; i++) {
			/* Classifies the current input character. */
			if (c->scratchpads[i].address) {
				drv_dma_free_coherent(c->hcd.dma,
						      &c->scratchpads[i]);
			}
		}

		hal_free(c->scratchpads);
		c->scratchpads = NULL;
	}

	/* Classifies the current input character. */
	if (c->scratchpad_array.address)
		drv_dma_free_coherent(c->hcd.dma, &c->scratchpad_array);
}

/* Supports the xhci scratchpads alloc operation. */

/* Supports the xhci scratchpads alloc operation. */
static int
xhci_scratchpads_alloc(
	struct xhci_controller *c)
{
	uint64_t *array;
	unsigned i;
	int e;

	/* Classifies the current input character. */
	if (!c->scratchpad_count)
		return 0;
	c->scratchpads =
		hal_malloc(sizeof(*c->scratchpads) * c->scratchpad_count);

	/* Classifies the current input character. */
	if (!c->scratchpads)
		return ENOMEM;
	memset(c->scratchpads, 0,
	       sizeof(*c->scratchpads) * c->scratchpad_count);

	/* Handles the e condition. */
	e = drv_dma_alloc_coherent(
		c->hcd.dma, (size_t)c->scratchpad_count * sizeof(uint64_t), 64U,
		&c->scratchpad_array);
	if (e)
		goto fail;
	memset(c->scratchpad_array.address, 0, c->scratchpad_array.size);
	array = c->scratchpad_array.address;
	/* Process each remaining element. */
	for (i = 0; i < c->scratchpad_count; i++) {
		/* Handles the e condition. */
		e = drv_dma_alloc_coherent(c->hcd.dma, 4096U, 4096U,
					   &c->scratchpads[i]);
		if (e)
			goto fail;
		array[i] = c->scratchpads[i].device_address;
	}

	((uint64_t *)c->dcbaa.address)[0] = c->scratchpad_array.device_address;

	/* Reports successful completion. */
	return 0;
fail:
	xhci_scratchpads_free(c);

	/* Returns the computed result. */
	return e;
}

/* HCHalted, PCI bus-master disable, and IRQ drain are all prerequisites for this software-only ownership drop.  Until then an endpoint owner remains published even when a cancellation command failed, so a late Transfer Event can never alias a reused ring slot. */

/* Supports the xhci controller drain requests operation. */
static void
xhci_controller_drain_requests(
	struct xhci_controller *c)
{
	struct xhci_device *device;
	struct xhci_request *request;
	struct drv_usb_urb *urb;
	unsigned dci;
	unsigned long irq;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		device = NULL;
		request = NULL;
		urb = NULL;
		irq = spin_lock_irqsave(&c->active_lock);
		/* Process each linked entry. */
		for (device = c->devices; device != NULL;
		     device = device->next) {
			/* Process each element required by the operation. */
			for (dci = 1; dci < 32U; dci++) {
				/* Handles the request availability. */
				request = device->endpoints[dci].active;
				if (request != NULL)
					break;
			}

			/* Handles the request availability. */
			if (request != NULL)
				break;
		}

		/* Handles the request availability. */
		if (request == NULL) {
			spin_unlock_irqrestore(&c->active_lock, irq);

			/* Returns the computed result. */
			return;
		}

		/* Checks the xhci request unlink locked result. */
		if (!c->dma_quiesced || request->device != device ||
		    request->endpoint != &device->endpoints[dci] ||
		    device->endpoints[dci].recovering ||
		    !xhci_request_unlink_locked(c, request))
			__builtin_trap();

		/* Checks the atomic raw fetch add relaxed result. */
		if (device->completions_busy == UINT_MAX ||
		    atomic_raw_fetch_add_relaxed(&c->completion_busy, 1U) ==
			    UINT_MAX)
			__builtin_trap();
		device->completions_busy++;
		urb = request->urb;
		spin_unlock_irqrestore(&c->active_lock, irq);

		drv_usb_urb_set_hcd_data(urb, NULL);
		xhci_request_release(c, request);

		/*
 * Terminal publication and callbacks must not run under
		 * active_lock. */
		drv_usb_hcd_complete(&c->hcd, urb, DRV_USB_URB_DISCONNECTED, 0);

		/* Handles the device condition. */
		irq = spin_lock_irqsave(&c->active_lock);
		if (device->completions_busy == 0)
			__builtin_trap();
		device->completions_busy--;
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Checks the atomic raw fetch add release result. */
		if (atomic_raw_fetch_add_release(&c->completion_busy,
						 (unsigned)-1) == 0)
			__builtin_trap();
	}
}

/* Supports the xhci submission quiesce operation. */

/* Supports the xhci submission quiesce operation. */
static int
xhci_submission_quiesce(
	struct xhci_controller *c)
{
	uint64_t started = sched_ticks();
	unsigned active, completion, command, operations, recovery;
	unsigned submissions;
	unsigned long irq;

	irq = spin_lock_irqsave(&c->active_lock);

	c->controller_stopping = 1U;

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		recovery = c->endpoint_recoveries_busy;
		operations = c->operations_busy;
		submissions = c->submissions_busy;
		active = c->active_count;
		spin_unlock_irqrestore(&c->active_lock, irq);
		completion = atomic_raw_load_acquire(&c->completion_busy);

		/* Handles the recovery condition. */
		command = atomic_raw_load_acquire(&c->command_busy);
		if (!recovery && !operations && !submissions && !completion &&
		    !command) {
			/* Reports successful completion. */
			return 0;
		}

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= 100U) {
			hal_printf("xhci: controller operation barrier timed "
				   "out (operations=%u submit=%u recovery=%u "
				   "completion=%u command=%u active=%u); "
				   "retaining all DMA\n",
				   operations, submissions, recovery,
				   completion, command, active);

			/* Returns the computed result. */
			return EBUSY;
		}

		sched_yield();
	}
}

/* Supports the xhci irq quiesce operation. */

/* Supports the xhci irq quiesce operation. */
static int
xhci_irq_quiesce(
	struct xhci_controller *c)
{
	uint64_t started = sched_ticks();

	/* Continue while the operation condition remains true. */
	while (atomic_raw_load_acquire(&c->irq_busy) != 0) {
		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= 100U) {
			hal_printf("xhci: IRQ completion barrier timed out; "
				   "retaining all DMA\n");

			/* Returns the computed result. */
			return EIO;
		}

		sched_yield();
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci irq disestablish operation. */

/* Supports the xhci irq disestablish operation. */
static int
xhci_irq_disestablish(
	struct xhci_controller *c)
{
	int function_result;
	uint64_t started = sched_ticks();
	int error;

	/* Handles the irq cookie availability. */
	if (c->irq_cookie == NULL) {
		/* Obtains the xhci irq quiesce result. */
		function_result = xhci_irq_quiesce(c);

		/* Returns the computed result. */
		return function_result;
	}

	/*
 * The checked PCI path masks the source before reporting EBUSY.  That
	 * closes the i386 INTx arrival window while the already-entered handler
	 * drains; only a successful retry owns and frees the cookie. */
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the operation status. */
		error = drv_pci_device_disestablish_irq_checked(c->pci,
								c->irq_cookie);
		if (error != EBUSY)
			break;

		/* Checks the sched ticks result. */
		if (sched_ticks() - started >= 100U) {
			hal_printf("xhci: IRQ removal barrier timed out; "
				   "retaining all DMA\n");

			/* Returns the computed result. */
			return EBUSY;
		}

		sched_yield();
	}

	/* Checks the operation status. */
	if (error != 0) {
		hal_printf("xhci: checked IRQ disestablish failed (%d); "
			   "retaining all DMA\n",
			   error);

		/* Returns the computed result. */
		return error;
	}

	c->irq_cookie = NULL;

	/* Obtains the xhci irq quiesce result. */
	function_result = xhci_irq_quiesce(c);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the xhci quiesce operation. */

/* Supports the xhci quiesce operation. */
static int
xhci_quiesce(
	struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	int barrier_error, halt_error, master_error;
	uint32_t command, iman;

	/* Classifies the current input character. */
	if (c->dma_quiesced)
		return 0;

	/* Checks the operation status. */
	barrier_error = xhci_submission_quiesce(c);
	if (barrier_error != 0)
		return barrier_error;
	iman = rd32(c->runtime, 0x20U);
	wr32(c->runtime, 0x20U, (iman & ~2U) | 1U);
	command = rd32(c->operational, XHCI_USBCMD);
	wr32(c->operational, XHCI_USBCMD,
	     command & ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
	halt_error = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_HALTED,
			       XHCI_STS_HALTED);

	/* Checks the operation status. */
	master_error = xhci_bus_master_disable(c);
	if (halt_error != 0) {
		hal_printf("xhci: stop did not reach HCHalted (halt=%d "
			   "master=%d); retaining DMA/IRQ state\n",
			   halt_error, master_error);

		/* Returns the computed result. */
		return halt_error;
	}

	/* Checks the operation status. */
	if (master_error != 0) {
		hal_printf("xhci: bus-master disable failed; retaining DMA/IRQ "
			   "state\n");

		/* Returns the computed result. */
		return master_error;
	}

	/* Checks the operation status. */
	barrier_error = xhci_irq_disestablish(c);
	if (barrier_error != 0)
		return barrier_error;

	/*
 * Only this point proves that no controller actor can reach a request.
	 * Retained endpoint owners may now be detached and terminally
	 * published. */
	c->dma_quiesced = 1U;
	xhci_controller_drain_requests(c);

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci release resources operation. */

/* Supports the xhci release resources operation. */
static int
xhci_release_resources(
	struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	struct drv_dma_buffer transfer_reserve;
	unsigned resources_safe;
	unsigned long irq;

	/* Classifies the current input character. */
	if (!c->dma_quiesced) {
		hal_printf("xhci: refusing to release DMA before HCHalted\n");

		/* Returns the computed result. */
		return EBUSY;
	}

	memset(&transfer_reserve, 0, sizeof(transfer_reserve));
	irq = spin_lock_irqsave(&c->active_lock);

	/* Handles the resources safe condition. */
	resources_safe = c->controller_stopping && c->active_count == 0 &&
			 !c->transfer_reserve_busy && !c->operations_busy &&
			 !c->submissions_busy && !c->endpoint_recoveries_busy &&
			 !c->completion_dispatch_busy &&
			 c->completion_head == NULL &&
			 c->completion_tail == NULL &&
			 atomic_raw_load_acquire(&c->completion_busy) == 0 &&
			 atomic_raw_load_acquire(&c->command_busy) == 0 &&
			 atomic_raw_load_acquire(&c->irq_busy) == 0;
	if (resources_safe) {
		transfer_reserve = c->transfer_reserve;
		memset(&c->transfer_reserve, 0, sizeof(c->transfer_reserve));
	}

	spin_unlock_irqrestore(&c->active_lock, irq);

	/* Handles the resources safe condition. */
	if (!resources_safe) {
		hal_printf("xhci: controller resources are still owned; "
			   "retaining all DMA\n");

		/* Returns the computed result. */
		return EBUSY;
	}

	/* Handles the address availability. */
	if (transfer_reserve.address != NULL)
		drv_dma_free_coherent(h->dma, &transfer_reserve);
	xhci_scratchpads_free(c);

	/* Classifies the current input character. */
	if (c->erst_memory.address)
		drv_dma_free_coherent(h->dma, &c->erst_memory);

	/* Classifies the current input character. */
	if (c->event_memory.address)
		drv_dma_free_coherent(h->dma, &c->event_memory);

	/* Classifies the current input character. */
	if (c->command.dma.address)
		ring_free(c, &c->command);

	/* Classifies the current input character. */
	if (c->dcbaa.address)
		drv_dma_free_coherent(h->dma, &c->dcbaa);
	memset(&c->command_memory, 0, sizeof(c->command_memory));
	c->events = NULL;

	/* Reports successful completion. */
	return 0;
}

/* Supports the xhci stop operation. */

/* Supports the xhci stop operation. */
static void
xhci_stop(
	struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	int error;

	/* Checks the operation status. */
	error = xhci_release_resources(h);
	if (error != 0)
		xhci_mark_quarantined(c);
}

/* Supports the xhci stop checked operation. */

/* Supports the xhci stop checked operation. */
static int
xhci_stop_checked(
	struct drv_usb_hcd *h)
{
	int function_result;
	int error;

	/* Checks the operation status. */
	error = xhci_quiesce(h);
	if (error != 0)
		return error;

	/* Obtains the xhci release resources result. */
	function_result = xhci_release_resources(h);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the xhci start operation. */

/* Supports the xhci start operation. */
static int
xhci_start(
	struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_erst *erst;
	unsigned long irq;
	int e, stop_error;

	/* Checks the wait bits result. */
	if ((e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_CNR, 0)) != 0)
		return e;
	wr32(c->operational, XHCI_USBCMD,
	     rd32(c->operational, XHCI_USBCMD) & ~XHCI_CMD_RUN);

	/* Checks the wait bits result. */
	if ((e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_HALTED,
			   XHCI_STS_HALTED)) != 0) {
		/* Returns the computed result. */
		return e;
	}
	wr32(c->operational, XHCI_USBCMD, XHCI_CMD_RESET);

	/* Checks the wait bits result. */
	if ((e = wait_bits(c->operational, XHCI_USBCMD, XHCI_CMD_RESET, 0)) !=
	    0) {
		/* Returns the computed result. */
		return e;
	}

	/* Checks the wait bits result. */
	if ((e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_CNR, 0)) != 0)
		return e;

	/* Checks the rd32 result. */
	if ((rd32(c->operational, XHCI_PAGESIZE) & 1U) == 0)
		return ENOTSUP;
	irq = spin_lock_irqsave(&c->active_lock);

	/* Checks the atomic raw load acquire result. */
	if (c->active_count != 0 || c->transfer_reserve_busy ||
	    c->operations_busy || c->submissions_busy ||
	    c->endpoint_recoveries_busy || c->completion_dispatch_busy ||
	    c->completion_head != NULL || c->completion_tail != NULL ||
	    atomic_raw_load_acquire(&c->completion_busy) != 0 ||
	    atomic_raw_load_acquire(&c->command_busy) != 0 ||
	    atomic_raw_load_acquire(&c->irq_busy) != 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);

		/* Returns the computed result. */
		return EBUSY;
	}

	c->controller_stopping = 0;
	c->dma_quiesced = 0;

	spin_unlock_irqrestore(&c->active_lock, irq);

	c->command_failed = 0;
	c->default_slot = 0;

	/* Checks the drv dma alloc coherent result. */
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U, &c->dcbaa)) != 0)
		goto fail;
	memset(c->dcbaa.address, 0, 4096U);

	/* Checks the xhci scratchpads alloc result. */
	if ((e = xhci_scratchpads_alloc(c)) != 0)
		goto fail;

	/* Checks the ring alloc result. */
	if ((e = ring_alloc(c, &c->command)) != 0)
		goto fail;
	c->command_memory = c->command.dma;

	/* Checks the drv dma alloc coherent result. */
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U,
					&c->event_memory)) != 0)
		goto fail;

	/* Checks the drv dma alloc coherent result. */
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U, &c->erst_memory)) !=
	    0)
		goto fail;

	/* Checks the drv dma alloc coherent result. */
	if ((e = drv_dma_alloc_coherent(h->dma, XHCI_TRANSFER_RESERVE_SIZE,
					XHCI_TRANSFER_RESERVE_SIZE,
					&c->transfer_reserve)) != 0)
		goto fail;
	memset(c->event_memory.address, 0, 4096U);
	memset(c->erst_memory.address, 0, 4096U);
	memset(c->transfer_reserve.address, 0, c->transfer_reserve.size);
	c->events = c->event_memory.address;
	c->event_dequeue = 0;
	c->event_cycle = 1;
	erst = c->erst_memory.address;
	erst->address = c->event_memory.device_address;
	erst->size = XHCI_RING_TRBS;
	wr64(c->operational, XHCI_DCBAAP, c->dcbaa.device_address);
	wr64(c->operational, XHCI_CRCR, c->command.dma.device_address | 1U);
	wr32(c->runtime, 0x28U, 1);
	wr64(c->runtime, 0x30U, c->erst_memory.device_address);
	wr64(c->runtime, 0x38U, c->event_memory.device_address);
	wr32(c->runtime, 0x24U, ZEDBSD_XHCI_IMOD);
	wr32(c->runtime, 0x20U, 2U);
	wr32(c->operational, XHCI_CONFIG, c->max_slots);
	wr32(c->operational, XHCI_USBSTS, 0xffffffffU);
	wr32(c->operational, XHCI_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE);

	/* Handles the e condition. */
	e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_HALTED, 0);
	if (!e)
		return 0;
fail:

	/* Checks the operation status. */
	stop_error = xhci_stop_checked(h);
	if (stop_error != 0)
		return stop_error;

	/* Returns the computed result. */
	return e;
}

/* Supports the xhci guarded device enable operation. */

/* Supports the xhci guarded device enable operation. */
static int
xhci_guarded_device_enable(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_device_enable(h, u);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded set address operation. */

/* Supports the xhci guarded set address operation. */
static int
xhci_guarded_set_address(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u,
	unsigned address)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_set_address(h, u, address);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded device quiesce operation. */

/* Supports the xhci guarded device quiesce operation. */
static int
xhci_guarded_device_quiesce(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_device_quiesce(h, u);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded device disable operation. */

/* Supports the xhci guarded device disable operation. */
static void
xhci_guarded_device_disable(
	struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);

	/* Checks the xhci operation enter result. */
	if (xhci_operation_enter(c) != 0)
		return;
	xhci_device_disable(h, u);
	xhci_operation_leave(c);
}

/* Supports the xhci guarded urb dequeue operation. */

/* Supports the xhci guarded urb dequeue operation. */
static int
xhci_guarded_urb_dequeue(
	struct drv_usb_hcd *h,
	struct drv_usb_urb *u)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_urb_dequeue(h, u);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded endpoint enable operation. */

/* Supports the xhci guarded endpoint enable operation. */
static int
xhci_guarded_endpoint_enable(
	struct drv_usb_hcd *h,
	struct drv_usb_endpoint *endpoint)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_endpoint_enable(h, endpoint);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded endpoint reset operation. */

/* Supports the xhci guarded endpoint reset operation. */
static int
xhci_guarded_endpoint_reset(
	struct drv_usb_hcd *h,
	struct drv_usb_endpoint *endpoint)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_endpoint_reset(h, endpoint);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded endpoint disable operation. */

/* Supports the xhci guarded endpoint disable operation. */
static int
xhci_guarded_endpoint_disable(
	struct drv_usb_hcd *h,
	struct drv_usb_endpoint *endpoint)
{
	struct xhci_controller *c = hcd_controller(h);
	int error;

	/* Checks the operation status. */
	error = xhci_operation_enter(c);
	if (error != 0)
		return error;
	error = xhci_endpoint_disable(h, endpoint);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded frame operation. */

/* Supports the xhci guarded frame operation. */
static uint32_t
xhci_guarded_frame(
	struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	uint32_t frame;

	/* Checks the xhci operation enter result. */
	if (xhci_operation_enter(c) != 0)
		return 0;
	frame = xhci_frame(h);
	xhci_operation_leave(c);

	/* Returns the computed result. */
	return frame;
}

/* Supports the xhci guarded root status operation. */

/* Supports the xhci guarded root status operation. */
static int
xhci_guarded_root_status(
	struct drv_usb_hcd *h,
	void *buffer,
	size_t length,
	size_t *actual)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_root_status(h, buffer, length, actual);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded root control operation. */

/* Supports the xhci guarded root control operation. */
static int
xhci_guarded_root_control(
	struct drv_usb_hcd *h,
	const struct drv_usb_control_request *request,
	void *buffer,
	size_t length,
	size_t *actual)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_root_control(h, request, buffer, length, actual);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the xhci guarded root port reset operation. */

/* Supports the xhci guarded root port reset operation. */
static int
xhci_guarded_root_port_reset(
	struct drv_usb_hcd *h,
	unsigned port)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	/* Checks the operation status. */
	if (error != 0)
		return error;
	error = xhci_root_port_reset(h, port);
	xhci_operation_leave(c);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static const struct drv_usb_hcd_ops xhci_ops = {
	.start = xhci_start,
	.quiesce = xhci_quiesce,
	.stop = xhci_stop,
	.device_enable = xhci_guarded_device_enable,
	.device_set_address = xhci_guarded_set_address,
	.device_quiesce = xhci_guarded_device_quiesce,
	.device_disable = xhci_guarded_device_disable,
	.urb_enqueue = xhci_urb_enqueue,
	.urb_reserve = xhci_urb_reserve,
	.urb_unreserve = xhci_urb_unreserve,
	.urb_reserve_buffer = xhci_urb_reserve_buffer,
	.urb_dequeue = xhci_guarded_urb_dequeue,
	.endpoint_enable = xhci_guarded_endpoint_enable,
	.endpoint_disable = xhci_guarded_endpoint_disable,
	.endpoint_reset = xhci_guarded_endpoint_reset,
	.frame_number = xhci_guarded_frame,
	.root_hub_status = xhci_guarded_root_status,
	.root_hub_control = xhci_guarded_root_control,
	.root_port_reset = xhci_guarded_root_port_reset};

/* Supports the xhci attach operation. */

/* Supports the xhci attach operation. */
static int
xhci_attach(
	struct drv_pci_device *d,
	const struct drv_pci_id *id)
{
	int unregister_error;
	struct xhci_controller *c;
	struct drv_xhci_capability_snapshot snapshot;
	struct drv_pci_address address;
	struct drv_pci_bar mapped_bar;
	const char *stage = "allocation";
	unsigned count = 0;
	uint32_t first, reasons, doorbell_offset, runtime_offset;
	uint32_t original_low, original_high, mapped_low, mapped_high;
	uint16_t command_before = 0xffffU, command_mapped, command_after;
	const char *irq_type;
	int cleanup_error, e;

	(void)id;

	/* Classifies the current input character. */
	c = hal_malloc(sizeof(*c));
	if (!c)
		return ENOMEM;
	memset(c, 0, sizeof(*c));
	c->dma_quiesced = 1;
	spin_init(&c->active_lock, LOCK_RANK_DEVICE, "xHCI active request");
	c->pci = d;
	xhci_pci_identity(d);
	drv_pci_device_address(d, &address);
	stage = "BAR claim";

	/* Handles the e condition. */
	e = drv_pci_device_claim_bar(d, 0);
	if (e != 0)
		goto fail;
	c->bar_claimed = 1;
	stage = "BAR inspection";

	/* Handles the e condition. */
	e = drv_pci_device_bar(d, 0, &c->original_bar);
	if (e != 0)
		goto fail;
	c->original_bar_valid = 1;
	xhci_bar_raw(d, c->original_bar.type, &original_low, &original_high);
	stage = "PCI command save";

	/* Handles the e condition. */
	e = drv_pci_device_config_read16(d, XHCI_PCI_COMMAND, &command_before);
	if (e != 0)
		goto fail;

	/* Handles the e condition. */
	e = drv_pci_device_save_enable_state(d, &c->pci_enable_state);
	if (e != 0)
		goto fail;
	c->pci_state_saved = 1;
	stage = "BAR map";

	/* Handles the e condition. */
	e = drv_pci_device_map_bar(d, 0,
				   DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE |
					   DRV_PCI_MAP_NOCACHE,
				   &c->mapping);
	if (e != 0)
		goto fail;
	c->bar_mapped = 1;
	stage = "BAR readback";

	/* Checks the drv pci device bar result. */
	if (drv_pci_device_bar(d, 0, &mapped_bar) != 0 ||
	    (mapped_bar.type != DRV_PCI_BAR_MEMORY32 &&
	     mapped_bar.type != DRV_PCI_BAR_MEMORY64) ||
	    c->mapping.address == NULL || c->mapping.size == 0 ||
	    c->mapping.size > mapped_bar.size) {
		e = EIO;
		goto fail;
	}

	xhci_bar_raw(d, mapped_bar.type, &mapped_low, &mapped_high);
	hal_printf("xhci: pci %04x:%02x:%02x.%u BAR0 type=%u size=%08x:%08x "
		   "original=%08x:%08x/%08x:%08x final=%08x:%08x/%08x:%08x "
		   "mapped=%u\n",
		   address.segment, address.bus, address.device,
		   address.function,
		   mapped_bar.type == DRV_PCI_BAR_MEMORY64 ? 64U : 32U,
		   (uint32_t)(mapped_bar.size >> 32), (uint32_t)mapped_bar.size,
		   (uint32_t)(c->original_bar.bus_address >> 32),
		   (uint32_t)c->original_bar.bus_address, original_high,
		   original_low, (uint32_t)(mapped_bar.bus_address >> 32),
		   (uint32_t)mapped_bar.bus_address, mapped_high, mapped_low,
		   (unsigned)c->mapping.size);

	/* Checks the drv xhci bar readback matches result. */
	if (!drv_xhci_bar_readback_matches(mapped_bar.bus_address,
					   mapped_bar.type ==
						   DRV_PCI_BAR_MEMORY64,
					   mapped_low, mapped_high)) {
		e = EIO;
		goto fail;
	}

	/* Checks the drv pci device config read16 result. */
	if (drv_pci_device_config_read16(d, XHCI_PCI_COMMAND,
					 &command_mapped) != 0 ||
	    (command_mapped & 7U) != (command_before & 7U)) {
		e = EIO;
		stage = "BAR command restore";
		goto fail;
	}

	/*
 * Mapping a BAR does not require device decode.  Capability MMIO does.
	 */
	stage = "PCI memory enable";

	/* Checks the drv pci device set bus master result. */
	if ((e = drv_pci_device_set_bus_master(d, false)) != 0 ||
	    (e = drv_pci_device_enable_memory(d)) != 0 ||
	    (e = drv_pci_device_config_read16(d, XHCI_PCI_COMMAND,
					      &command_after)) != 0)
		goto fail;
	hal_printf("xhci: pci %04x:%02x:%02x.%u command=%04x->%04x (MEM on, "
		   "MASTER off)\n",
		   address.segment, address.bus, address.device,
		   address.function, command_before, command_after);

	/* Handles the command after condition. */
	if ((command_after & XHCI_PCI_COMMAND_MEMORY) == 0 ||
	    (command_after & XHCI_PCI_COMMAND_MASTER) != 0) {
		e = EIO;
		goto fail;
	}

	hal_io_mb();

	c->capability = c->mapping.address;
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.mapping_size = c->mapping.size;

	/* Classifies the current input character. */
	if (c->mapping.size >= 0x1cU) {
		first = rd32(c->capability, 0);
		snapshot.capability_length = first & 0xffU;
		snapshot.version = first >> 16;
		snapshot.structural_parameters1 = rd32(c->capability, 4);
		snapshot.structural_parameters2 = rd32(c->capability, 8);
		snapshot.capability_parameters1 = rd32(c->capability, 0x10U);
		snapshot.doorbell_offset_raw = rd32(c->capability, 0x14U);
		snapshot.runtime_offset_raw = rd32(c->capability, 0x18U);
	}

	reasons = drv_xhci_capability_validate(&snapshot);
	hal_printf("xhci: pci %04x:%02x:%02x.%u caps len=%02x version=%04x "
		   "hcs1=%08x hcs2=%08x hcc1=%08x dboff=%08x rtsoff=%08x "
		   "reject=%08x:%s\n",
		   address.segment, address.bus, address.device,
		   address.function, snapshot.capability_length,
		   snapshot.version, snapshot.structural_parameters1,
		   snapshot.structural_parameters2,
		   snapshot.capability_parameters1,
		   snapshot.doorbell_offset_raw, snapshot.runtime_offset_raw,
		   reasons, drv_xhci_capability_reason_name(reasons));

	/* Handles the reasons condition. */
	if (reasons != 0) {
		e = ENODEV;
		stage = "capabilities";
		goto fail;
	}

	doorbell_offset = snapshot.doorbell_offset_raw & ~3U;
	runtime_offset = snapshot.runtime_offset_raw & ~31U;
	c->max_slots = snapshot.structural_parameters1 & 0xffU;
	c->ports = (snapshot.structural_parameters1 >> 24) & 0xffU;
	c->context_size =
		(snapshot.capability_parameters1 & (1U << 2)) ? 64U : 32U;
	c->scratchpad_count =
		drv_xhci_scratchpad_count(snapshot.structural_parameters2);
	c->operational = c->capability + snapshot.capability_length;
	c->runtime = c->capability + runtime_offset;
	c->doorbells = c->capability + doorbell_offset;
	c->hcd.name = "xHCI";
	c->hcd.ops = &xhci_ops;
	c->hcd.dma = drv_pci_device_dma(d);
	c->hcd.root_port_count = c->ports;
	c->hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS |
			      DRV_USB_HCD_CAP_TRANSFER_RESERVE |
			      DRV_USB_HCD_CAP_SHARED_STAGING;
	c->hcd.private_data[0] = (uintptr_t)c;

	/* Checks the ownership result. */
	stage = "ownership";
	if ((e = ownership(c)) != 0)
		goto fail;

	/* Checks the drv pci device set bus master result. */
	stage = "PCI bus master";
	if ((e = drv_pci_device_set_bus_master(d, true)) != 0)
		goto fail;
	stage = "IRQ allocation";

	/*
 * One vector is sufficient; use MSI-X when present, then MSI or INTx.
	 */
	if ((e = drv_pci_device_allocate_irqs(d,
					      DRV_PCI_IRQ_ALLOW_MSIX |
						      DRV_PCI_IRQ_ALLOW_MSI |
						      DRV_PCI_IRQ_ALLOW_INTX,
					      1, 1, &c->irq, &count)) != 0)
		goto fail;
	c->irq_allocated = 1;

	/* Checks the drv pci device establish irq result. */
	stage = "IRQ establishment";
	if ((e = drv_pci_device_establish_irq(d, &c->irq, xhci_irq, c, "xhci",
					      &c->irq_cookie)) != 0)
		goto fail;

	/* Checks the drv usb hcd register result. */
	stage = "controller start";
	if ((e = drv_usb_hcd_register(&c->hcd, &c->bus)) != 0)
		goto fail;
	c->hcd_registered = 1;
	stage = "port worker";

	/* Checks the xhci worker start result. */
	if ((e = xhci_worker_start(c)) != 0) {
		/* Checks the operation status. */
		unregister_error = drv_usb_hcd_unregister(&c->hcd);
		if (unregister_error != 0) {
			e = unregister_error;
			goto fail;
		}

		c->hcd_registered = 0;
		goto fail;
	}

	drv_pci_device_set_driver_data(d, c);
	c->next = controllers;
	controllers = c;
	irq_type = c->irq.type == DRV_PCI_IRQ_MSI    ? "MSI"
		   : c->irq.type == DRV_PCI_IRQ_MSIX ? "MSI-X"
						     : "INTx";
	hal_printf("xhci: PCI controller, version=%x ports=%u slots=%u irq=%u "
		   "%s\n",
		   snapshot.version, c->ports, c->max_slots, c->irq.vector,
		   irq_type);

	/* Reports successful completion. */
	return 0;
fail:

	/* Classifies the current input character. */
	if (c->hcd_registered || !c->dma_quiesced) {
		xhci_quarantine(c, stage, e);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the irq cookie availability. */
	if (c->irq_cookie != NULL) {
		/* Checks the operation status. */
		cleanup_error = xhci_irq_disestablish(c);
		if (cleanup_error != 0) {
			xhci_quarantine(c, "IRQ disestablish", cleanup_error);

			/* Reports successful completion. */
			return 0;
		}
	}

	/* Classifies the current input character. */
	if (c->irq_allocated) {
		drv_pci_device_free_irqs(d, &c->irq, 1);
		c->irq_allocated = 0;
	}

	/* Checks the operation status. */
	cleanup_error = xhci_pci_release(c);
	if (cleanup_error != 0) {
		hal_printf("xhci: attach failed at %s (%d)\n", stage, e);
		xhci_quarantine(c, "PCI release", cleanup_error);

		/* Reports successful completion. */
		return 0;
	}

	hal_printf("xhci: attach failed at %s (%d)\n", stage, e);
	hal_free(c);

	/* Returns the computed result. */
	return e;
}
/* Supports the xhci detach operation. */

/* Supports the xhci detach operation. */
static int
xhci_detach(
	struct drv_pci_device *d,
	unsigned flags)
{
	struct xhci_controller *c = drv_pci_device_driver_data(d);
	struct xhci_controller **link;
	int error, had_worker;

	(void)flags;

	/* Classifies the current input character. */
	if (!c)
		return 0;

	/* Handles the had worker condition. */
	had_worker = c->port_worker != NULL;
	if (had_worker)
		xhci_worker_stop(c);

	/* Classifies the current input character. */
	if (c->hcd_registered) {
		/* Checks the operation status. */
		error = drv_usb_hcd_unregister(&c->hcd);
		if (error) {
			/* Checks the operation status. */
			if (had_worker && error == EBUSY)
				(void)xhci_worker_start(c);
			else
				xhci_mark_quarantined(c);

			/* Returns the computed result. */
			return error;
		}

		c->hcd_registered = 0;

		/* Classifies the current input character. */
		if (c->quarantined)
			return EBUSY;
	}

	/* Classifies the current input character. */
	if (!c->dma_quiesced) {
		/* Checks the operation status. */
		error = xhci_stop_checked(&c->hcd);
		if (error != 0) {
			xhci_mark_quarantined(c);

			/* Returns the computed result. */
			return error;
		}
	}

	/* Handles the irq cookie availability. */
	if (c->irq_cookie != NULL) {
		/* Checks the operation status. */
		error = xhci_irq_disestablish(c);
		if (error != 0) {
			xhci_mark_quarantined(c);

			/* Returns the computed result. */
			return error;
		}
	}

	/* Classifies the current input character. */
	if (c->irq_allocated) {
		drv_pci_device_free_irqs(d, &c->irq, 1);
		c->irq_allocated = 0;
	}

	/* Checks the operation status. */
	error = xhci_pci_release(c);
	if (error != 0) {
		xhci_mark_quarantined(c);

		/* Returns the computed result. */
		return error;
	}

	drv_pci_device_set_driver_data(d, NULL);
	/* Process each linked entry. */
	for (link = &controllers; *link != NULL; link = &(*link)->next) {
		/* Handles the link condition. */
		if (*link == c) {
			*link = c->next;
			break;
		}
	}

	hal_free(c);

	/* Reports successful completion. */
	return 0;
}
static const struct drv_pci_id ids[] = {{DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
					 DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
					 0x0c0330U, 0xffffffU, 0}};
static struct drv_pci_driver driver = {.name = "xhci",
				       .ids = ids,
				       .id_count = 1,
				       .attach = xhci_attach,
				       .detach = xhci_detach};
/*
 * Implements the drv pci xhci driver register operation.
 */
/*
 * Implements the drv pci xhci driver register operation.
 */
int
drv_pci_xhci_driver_register(
	void)
{
	int error;

	/* Obtains the drv pci driver register result. */
	error = drv_pci_driver_register(&driver);

	/* Returns the computed result. */
	return error;
}
/*
 * Implements the drv pci xhci probe roots operation.
 */
/*
 * Implements the drv pci xhci probe roots operation.
 */
void
drv_pci_xhci_probe_roots(
	void)
{
	struct xhci_controller *c;

	/* Process each linked entry. */
	for (c = controllers; c; c = c->next) {
		/* Classifies the current input character. */
		if (c->quarantined)
			continue;
		drv_usb_hcd_root_hub_changed(&c->hcd);
		c->port_pending = 0;
		c->root_ready = 1;
	}
}

/* Begin consolidated pci-xhci-sg.inc. */
/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* The same immutable plan drives publication and short-event accounting. */
static int
xhci_sg_plan(
	struct xhci_request *request,
	size_t length)
{
	struct drv_dma_segment segment;
	size_t remaining, chunk;
	unsigned index, count;

	request->normal_count = 0;

	/* Checks the remaining item count. */
	count = drv_dma_vector_count(request->vector);
	if (count == 0 || count > DRV_DMA_VECTOR_MAX_SEGMENTS ||
	    length > DRV_DMA_VECTOR_MAX_SIZE) {
		/* Returns the computed result. */
		return EINVAL;
	}

	/* Checks the current data length. */
	if (length == 0) {
		request->normal_count = 1;
		request->normal[0].address = 0;
		request->normal[0].length = 0;

		/* Reports successful completion. */
		return 0;
	}

	remaining = length;
	/* Process each remaining element. */
	for (index = 0; index < count && remaining != 0; index++) {
		/* Checks the drv dma vector segment result. */
		if (drv_dma_vector_segment(request->vector, index, &segment) !=
			    0 ||
		    segment.length == 0 ||
		    segment.length - 1U > UINT64_MAX - segment.address) {
			/* Returns the computed result. */
			return EINVAL;
		}

		/* Handles the segment condition. */
		if (segment.length > remaining)
			segment.length = remaining;
		/* Process each remaining element. */
		while (segment.length != 0) {
			/* Handles the chunk condition. */
			chunk = 0x10000U - (size_t)(segment.address & 0xffffU);
			if (chunk > segment.length)
				chunk = segment.length;

			/* Handles the request condition. */
			if (request->normal_count ==
			    DRV_DMA_VECTOR_MAX_SEGMENTS) {
				/* Returns the computed result. */
				return E2BIG;
			}
			request->normal[request->normal_count].address =
				segment.address;
			request->normal[request->normal_count++].length = chunk;
			segment.address += chunk;
			segment.length -= chunk;
			remaining -= chunk;
		}
	}

	/* Returns the computed result. */
	return remaining == 0 ? 0 : EINVAL;
}

/* Supports the xhci sg short operation. */
static int
xhci_sg_short(
	const struct xhci_request *request,
	unsigned offset,
	size_t residual,
	size_t *actual)
{
	size_t prefix;
	unsigned index;

	/* Handles the actual availability. */
	if (actual == NULL || offset >= request->normal_count ||
	    request->normal[offset].length == 0 ||
	    residual > request->normal[offset].length) {
		/* Reports successful completion. */
		return 0;
	}
	prefix = 0;
	/* Process each remaining element. */
	for (index = 0; index < offset; index++)
		prefix += request->normal[index].length;
	*actual = prefix + request->normal[offset].length - residual;
	/* Reports operation failure. */
	return 1;
}

/* Supports the xhci sg enqueue operation. */
static void
xhci_sg_enqueue(
	struct xhci_ring *ring,
	const struct xhci_request *request,
	int input,
	size_t packet_size,
	int zero_packet)
{
	size_t cumulative;
	uint32_t control;
	unsigned index, td_size;
	int final;

	cumulative = 0;
	/* Process each remaining element. */
	for (index = 0; index < request->normal_count; index++) {
		cumulative += request->normal[index].length;
		final = index + 1U == request->normal_count && !zero_packet;

		/* Validates the current input. */
		control = XHCI_TRB_TYPE(1) |
			  (final ? XHCI_TRB_IOC : XHCI_TRB_CHAIN);
		if (input)
			control |= DRV_XHCI_TRB_ISP;

		/* Handles the zero packet condition. */
		td_size = drv_xhci_normal_td_size(request->length, cumulative,
						  packet_size, final);
		if (zero_packet && td_size < 31U)
			td_size++;
		(void)ring_push(ring, request->normal[index].address,
				(uint32_t)request->normal[index].length |
					(td_size << 17),
				control);
		io_stats_record(IO_XHCI_DATA_TRB,
				request->normal[index].length);
	}

	/* Handles the zero packet condition. */
	if (zero_packet) {
		(void)ring_push(ring, 0, 0, XHCI_TRB_TYPE(1) | XHCI_TRB_IOC);
		io_stats_record(IO_XHCI_DATA_TRB, 0);
	}
}
/* End consolidated pci-xhci-sg.inc. */
/* End consolidated pci-xhci.c. */
