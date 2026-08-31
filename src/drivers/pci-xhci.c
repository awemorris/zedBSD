/* Native PCI xHCI host controller. Copyright (C) 2026 Awe Morris;
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
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <limits.h>
#include <string.h>

#define XHCI_USBCMD 0x00U
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
#define XHCI_PCI_COMMAND_ENABLE \
	(XHCI_PCI_COMMAND_IO | XHCI_PCI_COMMAND_MEMORY | \
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
struct xhci_request {
	struct drv_usb_urb *urb;
	struct xhci_device *device;
	struct drv_dma_buffer bounce;
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
	size_t short_actual;
	size_t completion_actual, completion_residual;
	unsigned completion_trb_offset;
	enum drv_usb_urb_status terminal_status;
	struct xhci_request *completion_next;
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
	/* Protected by active_lock.  Each endpoint remains queue-depth one, while
	 * these counts provide bounded controller teardown barriers. */
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

static void xhci_request_release(struct xhci_controller *,
	struct xhci_request *);

static uint8_t
rd8(volatile uint8_t *b, unsigned o)
{
	return b[o];
}
static uint32_t
rd32(volatile uint8_t *b, unsigned o)
{
	return *(volatile uint32_t *)(b + o);
}
static void
wr32(volatile uint8_t *b, unsigned o, uint32_t v)
{
	*(volatile uint32_t *)(b + o) = v;
	hal_io_mb();
}
static void
wr8(volatile uint8_t *b, unsigned o, uint8_t v)
{
	b[o] = v;
	hal_io_mb();
}
static void
wr64(volatile uint8_t *b, unsigned o, uint64_t v)
{
	wr32(b, o, (uint32_t)v);
	wr32(b, o + 4U, (uint32_t)(v >> 32));
}
static struct xhci_controller *
hcd_controller(struct drv_usb_hcd *h)
{
	return (void *)h->private_data[0];
}

static int
wait_bits(volatile uint8_t *b, unsigned o, uint32_t mask, uint32_t wanted)
{
	unsigned n;
	uint32_t value;

	for (n = 0; n < XHCI_TIMEOUT; n++) {
		value = rd32(b, o);
		if (!drv_xhci_mmio32_valid(value))
			return EIO;
		if ((value & mask) == wanted)
			return 0;
	}
	return ETIMEDOUT;
}

static void
xhci_bar_raw(struct drv_pci_device *device, enum drv_pci_bar_type type,
	uint32_t *low, uint32_t *high)
{
	*low = 0xffffffffU;
	*high = 0;
	(void)drv_pci_device_config_read32(device, XHCI_PCI_BAR0, low);
	if (type == DRV_PCI_BAR_MEMORY64) {
		*high = 0xffffffffU;
		(void)drv_pci_device_config_read32(device, XHCI_PCI_BAR0 + 4U,
		    high);
	}
}

static void
xhci_pci_identity(struct drv_pci_device *device)
{
	struct drv_pci_address address, bridge_address;
	struct drv_pci_bus *bus;
	struct drv_pci_device *bridge;

	drv_pci_device_address(device, &address);
	bus = drv_pci_device_bus(device);
	bridge = bus != NULL ? drv_pci_bus_bridge(bus) : NULL;
	if (bridge != NULL) {
		drv_pci_device_address(bridge, &bridge_address);
		hal_printf(
		    "xhci: pci %04x:%02x:%02x.%u id=%04x:%04x sub=%04x:%04x rev=%02x parent=%04x:%02x:%02x.%u\n",
		    address.segment, address.bus, address.device,
		    address.function, drv_pci_device_vendor(device),
		    drv_pci_device_product(device),
		    drv_pci_device_subvendor(device),
		    drv_pci_device_subproduct(device),
		    drv_pci_device_revision(device), bridge_address.segment,
		    bridge_address.bus, bridge_address.device,
		    bridge_address.function);
	} else {
		hal_printf(
		    "xhci: pci %04x:%02x:%02x.%u id=%04x:%04x sub=%04x:%04x rev=%02x parent=root\n",
		    address.segment, address.bus, address.device,
		    address.function, drv_pci_device_vendor(device),
		    drv_pci_device_product(device),
		    drv_pci_device_subvendor(device),
		    drv_pci_device_subproduct(device),
		    drv_pci_device_revision(device));
	}
}

static int
xhci_restore_bar(struct xhci_controller *controller)
{
	struct drv_pci_bar current;
	int error;

	if (!controller->original_bar_valid)
		return 0;
	error = drv_pci_device_bar(controller->pci, 0, &current);
	if (error != 0)
		return error;
	if (current.bus_address == controller->original_bar.bus_address)
		return 0;
	return drv_pci_device_assign_bar(controller->pci, 0,
	    controller->original_bar.bus_address);
}

static int
xhci_bus_master_disable(struct xhci_controller *controller)
{
	uint16_t command;
	int error;

	error = drv_pci_device_set_bus_master(controller->pci, false);
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci,
	    XHCI_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	return (command & XHCI_PCI_COMMAND_MASTER) == 0 ? 0 : EIO;
}

static int
xhci_pci_quiesce(struct xhci_controller *controller)
{
	uint16_t command;
	int error;

	error = drv_pci_device_config_read16(controller->pci,
	    XHCI_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	error = drv_pci_device_config_write16(controller->pci,
	    XHCI_PCI_COMMAND,
	    (uint16_t)(command & ~XHCI_PCI_COMMAND_ENABLE));
	if (error != 0)
		return error;
	error = drv_pci_device_config_read16(controller->pci,
	    XHCI_PCI_COMMAND, &command);
	if (error != 0)
		return error;
	return (command & XHCI_PCI_COMMAND_ENABLE) == 0 ? 0 : EIO;
}

static void
xhci_legacy_release(struct xhci_controller *controller)
{
	uint32_t control;
	uint8_t owned;

	if (!controller->legacy_claimed || controller->capability == NULL ||
	    !drv_xhci_region_fits(controller->mapping.size,
		controller->legacy_offset, 8U))
		return;
	control = rd32(controller->capability,
	    controller->legacy_offset + 4U);
	control = drv_xhci_legacy_control_restore(control,
	    controller->legacy_control);
	wr32(controller->capability, controller->legacy_offset + 4U, control);
	owned = rd8(controller->capability, controller->legacy_offset + 3U);
	wr8(controller->capability, controller->legacy_offset + 3U,
	    owned & (uint8_t)~1U);
	controller->legacy_claimed = 0;
}

static int
xhci_pci_release(struct xhci_controller *controller)
{
	int error, bar_error = 0, master_error = 0, quiesce_error;

	if (controller->pci_state_saved || controller->bar_mapped)
		master_error = xhci_bus_master_disable(controller);
	if (master_error != 0) {
		hal_printf("xhci: PCI bus-master disable failed (%d)\n",
		    master_error);
		return master_error;
	}
	xhci_legacy_release(controller);
	if (controller->bar_mapped) {
		drv_pci_device_unmap_bar(controller->pci, &controller->mapping);
		controller->bar_mapped = 0;
	}
	bar_error = xhci_restore_bar(controller);
	if (bar_error != 0) {
		hal_printf("xhci: BAR0 restore failed (%d)\n", bar_error);
		quiesce_error = xhci_pci_quiesce(controller);
		if (quiesce_error != 0)
			hal_printf(
			    "xhci: PCI quiesce after BAR failure failed (%d)\n",
			    quiesce_error);
		return bar_error;
	}
	if (controller->pci_state_saved) {
		error = drv_pci_device_restore_enable_state(controller->pci,
		    &controller->pci_enable_state);
		if (error != 0) {
			hal_printf(
			    "xhci: PCI command restore failed (%d)\n",
			    error);
			quiesce_error = xhci_pci_quiesce(controller);
			if (quiesce_error != 0)
				hal_printf(
				    "xhci: PCI command failure quiesce failed (%d)\n",
				    quiesce_error);
			return error;
		}
		controller->pci_state_saved = 0;
	}
	if (controller->bar_claimed) {
		drv_pci_device_release_bar(controller->pci, 0);
		controller->bar_claimed = 0;
	}
	return 0;
}

static void
xhci_quarantine(struct xhci_controller *controller, const char *stage,
	int error)
{
	if (!controller->quarantined) {
		controller->quarantined = 1;
		drv_pci_device_set_driver_data(controller->pci, controller);
		controller->next = controllers;
		controllers = controller;
	}
	hal_printf(
	    "xhci: attach quarantined at %s (%d); controller ownership retained\n",
	    stage, error);
}

static int
ring_alloc(struct xhci_controller *c, struct xhci_ring *r)
{
	int e = drv_dma_alloc_coherent(c->hcd.dma, 4096U, 64U, &r->dma);
	if (e)
		return e;
	memset(r->dma.address, 0, 4096U);
	r->trbs = r->dma.address;
	r->enqueue = 0;
	r->cycle = 1;
	return 0;
}
static void
ring_free(struct xhci_controller *c, struct xhci_ring *r)
{
	if (r->dma.address)
		drv_dma_free_coherent(c->hcd.dma, &r->dma);
	memset(r, 0, sizeof(*r));
}
static uint64_t
ring_push(struct xhci_ring *r, uint64_t parameter, uint32_t status,
	  uint32_t control)
{
	struct xhci_trb *t = &r->trbs[r->enqueue];
	uint64_t trb_address = r->dma.device_address +
	    (uint64_t)r->enqueue * sizeof(*t);
	t->parameter_low = (uint32_t)parameter;
	t->parameter_high = (uint32_t)(parameter >> 32);
	t->status = status;
	hal_io_wmb();
	t->control = control | (r->cycle ? XHCI_TRB_CYCLE : 0);
	hal_io_wmb();
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
	return trb_address;
}

static int
event_take(struct xhci_controller *c, struct xhci_trb *out)
{
	struct xhci_trb *t = &c->events[c->event_dequeue];
	uint32_t control = t->control;
	if ((control & 1U) != c->event_cycle)
		return 0;
	hal_io_rmb();
	*out = *t;
	if (++c->event_dequeue == XHCI_RING_TRBS) {
		c->event_dequeue = 0;
		c->event_cycle ^= 1U;
	}
	wr64(c->runtime, 0x38U,
	     (c->event_memory.device_address + c->event_dequeue * sizeof(*t)) |
		 8U);
	return 1;
}

static void
event_lock(struct xhci_controller *c)
{
	while (__atomic_exchange_n(&c->event_busy, 1U, __ATOMIC_ACQUIRE))
		hal_compiler_barrier();
}

static void
event_unlock(struct xhci_controller *c)
{
	__atomic_store_n(&c->event_busy, 0U, __ATOMIC_RELEASE);
}

static int transfer_claim(struct xhci_controller *c,
	const struct xhci_trb *event);
static void xhci_completion_drain(struct xhci_controller *c);
static unsigned xhci_endpoint_state(struct xhci_controller *c,
	struct xhci_device *d, unsigned dci);

static void
port_change_defer(struct xhci_controller *c)
{
	struct thread *worker;
	if (!c->root_ready)
		return;
	c->port_pending = 1U;
	worker = c->port_worker;
	if (worker)
		kernel_notify_task(worker->task);
}

static uint64_t
xhci_event_pointer(const struct xhci_trb *event)
{
	return (uint64_t)event->parameter_low |
	    ((uint64_t)event->parameter_high << 32);
}

static int
command_ex(struct xhci_controller *c, uint64_t parameter, uint32_t status,
	uint32_t control, unsigned *slot, unsigned *completion)
{
	struct xhci_trb event;
	unsigned n, type;
	uint64_t command_address;
	uint32_t iman;
	bool enabled = hal_irq_disable();
	int result = ETIMEDOUT;
	if (completion)
		*completion = 0;
	while (__atomic_exchange_n(&c->command_busy, 1U, __ATOMIC_ACQUIRE)) {
		if (enabled)
			hal_irq_enable();
		sched_yield();
		enabled = hal_irq_disable();
	}
	if (c->dma_quiesced || c->command_failed) {
		__atomic_store_n(&c->command_busy, 0U, __ATOMIC_RELEASE);
		if (enabled)
			hal_irq_enable();
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
	for (n = 0; n < XHCI_TIMEOUT; n++) {
		int available;
		event_lock(c);
		if (c->command_event_ready) {
			event = c->command_event;
			c->command_event_ready = 0;
			available = 1;
		} else {
			available = event_take(c, &event);
		}
		type = available ? (event.control >> 10) & 0x3fU : 0;
		if (available && type == 32U)
			(void)transfer_claim(c, &event);
		event_unlock(c);
		if (!available)
			continue;
		if (type == 32U)
			continue;
		if (type == 34U) {
			port_change_defer(c);
			continue;
		}
		if (type != 33U)
			continue;
		if (!drv_xhci_command_completion_matches(command_address,
		    xhci_event_pointer(&event))) {
			hal_printf(
			    "xhci: ignored command completion for %x:%x, expected %x:%x\n",
			    event.parameter_high, event.parameter_low,
			    (uint32_t)(command_address >> 32),
			    (uint32_t)command_address);
			continue;
		}
		if (slot)
			*slot = event.control >> 24;
		if (completion)
			*completion = (event.status >> 24) & 0xffU;
		result = ((event.status >> 24) & 0xffU) == 1U ? 0 : EIO;
		if (result)
			hal_printf("xhci: command %u failed, completion=%u\n",
				   (control >> 10) & 0x3fU,
				   (event.status >> 24) & 0xffU);
		break;
	}
	event_lock(c);
	c->command_address = 0;
	c->command_event_ready = 0;
	event_unlock(c);
	wr32(c->runtime, 0x20U, (iman & 2U) | 1U);
	if (result == ETIMEDOUT)
		c->command_failed = 1;
	__atomic_store_n(&c->command_busy, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
	/* Transfer Events consumed while polling are claimed before event_lock is
	 * released, but callbacks are deferred until the command gate is open. */
	xhci_completion_drain(c);
	if (result == ETIMEDOUT)
		hal_printf("xhci: command %u timed out\n",
			   (control >> 10) & 0x3fU);
	return result;
}

static int
command(struct xhci_controller *c, uint64_t parameter, uint32_t status,
	uint32_t control, unsigned *slot)
{
	return command_ex(c, parameter, status, control, slot, NULL);
}

static int
ownership(struct xhci_controller *c)
{
	uint32_t hcc = rd32(c->capability, 0x10U);
	unsigned offset = ((hcc >> 16) & 0xffffU) * 4U;

	if (offset != 0 && (offset < 0x20U ||
	    !drv_xhci_region_fits(c->mapping.size, offset, 4U)))
		return EIO;
	while (offset != 0) {
		uint32_t cap = rd32(c->capability, offset);
		unsigned id = cap & 0xffU;
		unsigned next;
		int step;

		if (id == 0U || id == 0xffU)
			return EIO;
		if (id == 1U) {
			uint32_t control;
			uint8_t owned;
			unsigned count;

			if (c->legacy_claimed)
				return EIO;
			if (!drv_xhci_region_fits(c->mapping.size, offset, 8U))
				return EIO;
			c->legacy_offset = offset;
			c->legacy_control = rd32(c->capability, offset + 4U);
			owned = rd8(c->capability, offset + 3U);
			wr8(c->capability, offset + 3U, owned | 1U);
			c->legacy_claimed = 1;
			for (count = 0; count < XHCI_TIMEOUT; count++)
				if (drv_xhci_legacy_ownership_ready(
				    rd8(c->capability, offset + 2U),
				    rd8(c->capability, offset + 3U)))
					break;
			if (count == XHCI_TIMEOUT) {
				xhci_legacy_release(c);
				return ETIMEDOUT;
			}
			control = rd32(c->capability, offset + 4U);
			wr32(c->capability, offset + 4U,
			    drv_xhci_legacy_control_disable(control));
			if ((rd32(c->capability, offset + 4U) &
			    DRV_XHCI_LEGACY_SMI_ENABLE) != 0) {
				xhci_legacy_release(c);
				return EIO;
			}
		}
		step = drv_xhci_extended_capability_next(c->mapping.size,
		    offset, cap, &next);
		if (step < 0)
			return EIO;
		if (step == 0)
			return 0;
		offset = next;
	}
	return 0;
}

static void
fill_slot(struct xhci_controller *c, struct xhci_device *d, void *context,
	  unsigned entries)
{
	uint32_t *w = context;
	memset(context, 0, c->context_size);
	w[0] = ((d->speed_id & 15U) << 20) | ((entries & 31U) << 27);
	w[1] = drv_usb_device_port(d->usb) << 16;
}
static void
fill_endpoint(struct xhci_controller *c, void *context,
	      struct xhci_endpoint *ep,
	      const struct drv_xhci_endpoint_context_words *encoded)
{
	uint32_t *w = context;
	uint64_t dequeue = ep->ring.dma.device_address +
	    (uint64_t)ep->ring.enqueue * sizeof(struct xhci_trb);

	dequeue |= ep->ring.cycle ? 1U : 0U;
	memset(context, 0, c->context_size);
	w[0] = encoded->word0;
	w[1] = encoded->word1;
	w[2] = (uint32_t)dequeue;
	w[3] = (uint32_t)(dequeue >> 32);
	w[4] = encoded->word4;
}
static struct xhci_device *
xhci_usb_device(struct drv_usb_device *u)
{
	struct xhci_device *device;

	device = (struct xhci_device *)drv_usb_device_hcd_data(u, 0);
	return device != NULL && device->usb == u ? device : NULL;
}

static struct xhci_device *
xhci_slot_device_locked(struct xhci_controller *c, unsigned slot)
{
	struct xhci_device *device;

	for (device = c->devices; device != NULL; device = device->next)
		if (device->slot == slot)
			return device;
	return NULL;
}

static struct xhci_request *
xhci_event_request_locked(struct xhci_controller *c,
	const struct xhci_trb *event, unsigned *trb_offset)
{
	struct xhci_device *device;
	struct xhci_endpoint *endpoint;
	struct xhci_request *request;
	uint64_t pointer = (uint64_t)event->parameter_low |
	    ((uint64_t)event->parameter_high << 32);
	unsigned slot = event->control >> 24;
	unsigned dci = (event->control >> 16) & 31U;

	if (dci == 0 || dci >= 32U)
		return NULL;
	device = xhci_slot_device_locked(c, slot);
	if (device == NULL)
		return NULL;
	endpoint = &device->endpoints[dci];
	request = endpoint->active;
	if (request == NULL || request->device != device ||
	    request->endpoint != endpoint ||
	    !drv_xhci_transfer_event_matches(endpoint->ring.dma.device_address,
		XHCI_RING_TRBS, request->slot, request->dci,
		request->first_trb, request->trb_count, pointer, slot, dci,
		trb_offset))
		return NULL;
	return request;
}

static int
xhci_request_publish_locked(struct xhci_controller *c,
	struct xhci_request *request)
{
	if (request->endpoint->active != NULL)
		return EBUSY;
	if (c->active_count == UINT_MAX)
		__builtin_trap();
	request->endpoint->active = request;
	c->active_count++;
	return 0;
}

static int
xhci_request_unlink_locked(struct xhci_controller *c,
	struct xhci_request *request)
{
	if (request->endpoint->active != request)
		return 0;
	if (c->active_count == 0)
		__builtin_trap();
	request->endpoint->active = NULL;
	c->active_count--;
	return 1;
}

static void
xhci_recovery_leave_locked(struct xhci_controller *c,
	struct xhci_endpoint *endpoint)
{
	if (!endpoint->recovering || endpoint->stall_publishing ||
	    c->endpoint_recoveries_busy == 0)
		__builtin_trap();
	endpoint->recovering = 0;
	c->endpoint_recoveries_busy--;
}

static int
xhci_device_recovery_busy_locked(const struct xhci_device *device)
{
	unsigned dci;

	for (dci = 1; dci < 32U; dci++)
		if (device->endpoints[dci].recovering)
			return 1;
	return 0;
}

static int
xhci_device_request_busy_locked(const struct xhci_device *device)
{
	unsigned dci;

	for (dci = 1; dci < 32U; dci++)
		if (device->endpoints[dci].active != NULL)
			return 1;
	return 0;
}

static void
xhci_default_owner_release(struct xhci_controller *c, struct xhci_device *d)
{
	unsigned expected;

	if (d == NULL || !d->default_owned)
		return;
	expected = d->slot;
	if (hal_atomic_compare_exchange_acq_rel(&c->default_slot, &expected,
	    0U))
		d->default_owned = 0;
}

static void
xhci_device_release(struct drv_usb_hcd *h, struct xhci_device *d)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device **link;
	unsigned i;
	unsigned long irq;

	if (d == NULL || !d->slot_disabled) {
		hal_printf("xhci: refusing device release before Disable Slot\n");
		return;
	}
	xhci_default_owner_release(c, d);
	irq = spin_lock_irqsave(&c->active_lock);
	if (xhci_device_request_busy_locked(d) ||
	    xhci_device_recovery_busy_locked(d) || d->completions_busy != 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		hal_printf(
		    "xhci: slot %u release crossed an active ownership boundary\n",
		    d->slot);
		__builtin_trap();
	}
	if (xhci_usb_device(d->usb) == d)
		(void)drv_usb_device_set_hcd_data(d->usb, 0, 0);
	for (link = &c->devices; *link != NULL; link = &(*link)->next)
		if (*link == d) {
			*link = d->next;
			break;
		}
	spin_unlock_irqrestore(&c->active_lock, irq);
	if (c->dcbaa.address != NULL && d->slot <= c->max_slots) {
		((uint64_t *)c->dcbaa.address)[d->slot] = 0;
		hal_io_wmb();
	}
	for (i = 1; i < 32; i++)
		ring_free(c, &d->endpoints[i].ring);
	if (d->input_context.address != NULL)
		drv_dma_free_coherent(h->dma, &d->input_context);
	if (d->output_context.address != NULL)
		drv_dma_free_coherent(h->dma, &d->output_context);
	hal_free(d);
}

static int
xhci_device_enable(struct drv_usb_hcd *h, struct drv_usb_device *u)
{
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
	if (hal_atomic_load_acquire(&c->default_slot) != 0)
		return EBUSY;
	d = hal_malloc(sizeof(*d));
	if (!d)
		return ENOMEM;
	memset(d, 0, sizeof(*d));
	d->usb = u;
	if (drv_usb_device_port(u) == 0 || drv_usb_device_port(u) > c->ports) {
		hal_free(d);
		return EINVAL;
	}
	portsc = rd32(c->operational,
	    XHCI_PORTSC(drv_usb_device_port(u) - 1U));
	d->speed_id = drv_xhci_port_speed_id(portsc);
	if (portsc == UINT32_MAX || d->speed_id == 0) {
		hal_free(d);
		return EIO;
	}
	if ((e = command(c, 0, 0, XHCI_TRB_TYPE(9), &slot)) != 0 || slot == 0) {
		hal_free(d);
		return e ? e : EIO;
	}
	d->slot = slot;
	/* Publish partial ownership immediately.  Every later failure must pass
	 * through checked Disable Slot before any controller-visible DMA is freed. */
	irq = spin_lock_irqsave(&c->active_lock);
	d->next = c->devices;
	c->devices = d;
	(void)drv_usb_device_set_hcd_data(u, 0, (uintptr_t)d);
	spin_unlock_irqrestore(&c->active_lock, irq);
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U,
					&d->output_context)) != 0)
		goto fail;
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U,
					&d->input_context)) != 0)
		goto fail;
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
	packet = drv_usb_device_speed(u) >= DRV_USB_SPEED_SUPER	 ? 512U
		 : drv_usb_device_speed(u) >= DRV_USB_SPEED_HIGH ? 64U
								 : 8U;
	if (!drv_xhci_endpoint_context_encode(drv_usb_device_speed(u), 4U,
	    (uint16_t)packet, 0, NULL, &endpoint_context)) {
		e = EINVAL;
		goto fail;
	}
	fill_endpoint(c, input + 2U * c->context_size, &d->endpoints[1],
	    &endpoint_context);
	{
		unsigned expected = 0;

		if (!hal_atomic_compare_exchange_acq_rel(&c->default_slot,
		    &expected, slot)) {
			e = EBUSY;
			goto fail;
		}
		d->default_owned = 1U;
	}
	e = command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(11) | (1U << 9) | XHCI_TRB_SLOT(slot), NULL);
	if (e)
		goto fail;
	d->context_entries = 1;
	return 0;
fail:
	{
		unsigned completion = 0;
		int disable_error = command_ex(c, 0, 0,
		    XHCI_TRB_TYPE(10) | XHCI_TRB_SLOT(slot), NULL,
		    &completion);

		if (disable_error == 0) {
			d->slot_disabled = 1;
			xhci_device_release(h, d);
		} else {
			hal_printf(
			    "xhci: slot %u enable rollback failed (%d, completion=%u); contexts retained\n",
			    slot, disable_error, completion);
		}
	}
	return e;
}

static int
xhci_set_address(struct drv_usb_hcd *h, struct drv_usb_device *u,
		 unsigned address)
{
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
	if (!d)
		return ENODEV;
	if (d->quiescing)
		return ENODEV;
	for (attempt = 0; attempt < 1000U; attempt++) {
		if (atomic_raw_load_acquire(&c->completion_busy) == 0)
			break;
		sched_yield();
	}
	if (attempt == 1000U)
		return EBUSY;
	descriptor = drv_usb_device_descriptor(u);
	if (descriptor == NULL || !drv_xhci_ep0_max_packet_size(
	    drv_usb_device_speed(u), descriptor->endpoint0_max_packet_size,
	    &packet))
		return EIO;
	dequeue = d->endpoints[1].ring.dma.device_address +
	    (uint64_t)d->endpoints[1].ring.enqueue * sizeof(struct xhci_trb);
	dequeue |= d->endpoints[1].ring.cycle ? 1U : 0U;
	if (!drv_xhci_ep0_context(packet, dequeue, &ep0))
		return EIO;
	input = d->input_context.address;
	memset(input, 0, 4096U);
	control = (uint32_t *)input;
	control[1] = 3U;
	fill_slot(c, d, input + c->context_size, 1U);
	memset(input + 2U * c->context_size, 0, c->context_size);
	memcpy(input + 2U * c->context_size, ep0.words,
	    sizeof(ep0.words));
	{
		int error = command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(11) | XHCI_TRB_SLOT(d->slot), NULL);

		if (error == 0)
			xhci_default_owner_release(c, d);
		return error;
	}
}

static void
xhci_device_disable(struct drv_usb_hcd *h, struct drv_usb_device *u)
{
	struct xhci_device *d = xhci_usb_device(u);

	if (d == NULL)
		return;
	if (!d->slot_disabled) {
		hal_printf(
		    "xhci: slot %u release requested before checked teardown; retaining contexts\n",
		    d->slot);
		return;
	}
	xhci_device_release(h, d);
}

static int
xhci_endpoint_enable(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d =
	    xhci_usb_device(drv_usb_endpoint_device(usbep));
	struct drv_xhci_endpoint_context_words endpoint_context;
	struct xhci_endpoint *ep;
	const struct drv_usb_superspeed_endpoint_companion_descriptor *companion;
	const struct drv_usb_endpoint_descriptor *desc;
	uint8_t *input;
	uint32_t *control;
	unsigned number, dci, entries, type;
	int ring_allocated = 0;
	int e;
	if (!d)
		return ENODEV;
	desc = drv_usb_endpoint_descriptor(usbep);
	number = desc->address & 15U;
	dci = number * 2U + ((desc->address & DRV_USB_DIR_IN) ? 1U : 0U);
	if (dci < 2U || dci >= 32U)
		return EINVAL;
	ep = &d->endpoints[dci];
	if (ep->enabled)
		return 0;
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
	companion = drv_usb_endpoint_superspeed_companion(usbep);
	if (!drv_xhci_endpoint_context_encode(drv_usb_device_speed(d->usb),
	    type, desc->maximum_packet_size, desc->interval, companion,
	    &endpoint_context))
		return EINVAL;
	if (ep->ring.dma.address == NULL) {
		if ((e = ring_alloc(c, &ep->ring)) != 0)
			return e;
		ring_allocated = 1;
	} else if (ep->dci != dci) {
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
	e = command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(12) | XHCI_TRB_SLOT(d->slot), NULL);
	if (e) {
		if (ring_allocated) {
			ring_free(c, &ep->ring);
			ep->dci = 0;
		}
		return e;
	}
	d->context_entries = entries;
	ep->enabled = 1;
	return 0;
}
static int
xhci_endpoint_disable(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d =
	    xhci_usb_device(drv_usb_endpoint_device(usbep));
	struct xhci_endpoint *ep;
	uint8_t *input;
	uint32_t *control;
	unsigned dci, entries;
	unsigned long irq;
	int error;
	if (!d)
		return ENODEV;
	dci = (drv_usb_endpoint_address(usbep) & 15U) * 2U +
	      (drv_usb_endpoint_is_input(usbep) ? 1U : 0U);
	if (dci < 2U || dci >= 32U)
		return EINVAL;
	if (!d->endpoints[dci].enabled)
		return 0;
	ep = &d->endpoints[dci];
	irq = spin_lock_irqsave(&c->active_lock);
	if (ep->active != NULL || ep->recovering || d->quiescing) {
		error = d->quiescing ? ENODEV : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);
		return error;
	}
	if (c->endpoint_recoveries_busy == UINT_MAX)
		__builtin_trap();
	ep->recovering = 1U;
	c->endpoint_recoveries_busy++;
	spin_unlock_irqrestore(&c->active_lock, irq);
	for (entries = 31U; entries > 1U; entries--)
		if (entries != dci && d->endpoints[entries].enabled)
			break;
	input = d->input_context.address;
	memset(input, 0, 4096U);
	control = (uint32_t *)input;
	control[0] = 1U << dci;
	control[1] = 1U;
	fill_slot(c, d, input + c->context_size, entries);
	error = command(c, d->input_context.device_address, 0,
	    XHCI_TRB_TYPE(12) | XHCI_TRB_SLOT(d->slot), NULL);
	irq = spin_lock_irqsave(&c->active_lock);
	if (error == 0)
		ep->enabled = 0;
	xhci_recovery_leave_locked(c, ep);
	spin_unlock_irqrestore(&c->active_lock, irq);
	if (error != 0)
		return error;
	d->context_entries = entries;
	return 0;
}

/* event_lock is held across this ownership claim.  Therefore cancellation,
 * Disable Slot, and ring reuse cannot pass an event which was dequeued but had
 * not yet acquired its endpoint owner. */
static int
transfer_claim(struct xhci_controller *c, const struct xhci_trb *event)
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
	request = xhci_event_request_locked(c, event, &trb_offset);
	if (request == NULL) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return 0;
	}
	if (request->cancelling) {
		request->transfer_seen = 1U;
		request->completion_code = code;
		spin_unlock_irqrestore(&c->active_lock, irq);
		return 1;
	}
	control_request = drv_usb_urb_control_request(request->urb);
	actual = residual < request->length ? request->length - residual : 0;
	if (control_request != NULL && drv_xhci_control_short_data_event(
	    request->input, trb_offset, request->trb_count, code)) {
		request->short_seen = 1U;
		request->short_actual = actual;
		spin_unlock_irqrestore(&c->active_lock, irq);
		return 1;
	}
	if (control_request == NULL && request->input && code == 13U)
		normal_short_valid = drv_xhci_normal_short_actual(
		    request->bounce.device_address, request->length, trb_offset,
		    residual, &actual);
	if (request->short_seen)
		actual = request->short_actual;
	request->terminal_status = (code == 1U ||
	    (control_request == NULL && code == 13U && request->input &&
		normal_short_valid)) ?
		DRV_USB_URB_COMPLETE :
	    code == 6U ? DRV_USB_URB_STALL : DRV_USB_URB_IO_ERROR;
	request->completion_code = code;
	request->completion_actual = actual;
	request->completion_residual = residual;
	request->completion_trb_offset = trb_offset;
	request->completion_next = NULL;
	if (request->terminal_status == DRV_USB_URB_STALL &&
	    control_request == NULL &&
	    (drv_usb_endpoint_type(drv_usb_urb_endpoint(request->urb)) ==
		DRV_USB_TRANSFER_BULK ||
	    drv_usb_endpoint_type(drv_usb_urb_endpoint(request->urb)) ==
		DRV_USB_TRANSFER_INTERRUPT)) {
		/* Keep HCD admission closed until drv_usb_hcd_complete() has latched
		 * the STALL in the USB core.  Once the active request is unlinked,
		 * this marker is the only HCD-side barrier against a same-endpoint TD
		 * slipping into the completion-publication window. */
		if (request->endpoint->recovering ||
		    request->endpoint->stall_publishing ||
		    c->endpoint_recoveries_busy == UINT_MAX)
			__builtin_trap();
		request->endpoint->recovering = 1U;
		request->endpoint->stall_publishing = 1U;
		c->endpoint_recoveries_busy++;
		request->stall_publication = 1U;
	}
	if (atomic_raw_fetch_add_relaxed(&c->completion_busy, 1U) ==
	    UINT_MAX)
		__builtin_trap();
	device = request->device;
	if (device->completions_busy == UINT_MAX ||
	    !xhci_request_unlink_locked(c, request))
		__builtin_trap();
	device->completions_busy++;
	if (c->completion_tail != NULL)
		c->completion_tail->completion_next = request;
	else
		c->completion_head = request;
	c->completion_tail = request;
	spin_unlock_irqrestore(&c->active_lock, irq);
	return 1;
}

static void
xhci_completion_finish(struct xhci_controller *c,
	struct xhci_request *request)
{
	struct xhci_device *device = request->device;
	struct drv_usb_urb *urb = request->urb;
	const struct drv_usb_control_request *control_request =
	    drv_usb_urb_control_request(urb);
	enum drv_usb_urb_status terminal_status = request->terminal_status;
	struct xhci_endpoint *stall_endpoint = request->stall_publication ?
	    request->endpoint : NULL;
	size_t completion_actual = request->completion_actual;
	unsigned long irq;

	if (request->terminal_status != DRV_USB_URB_COMPLETE ||
	    (request->completion_residual != 0 &&
	    request->completion_code != 13U)) {
		if (control_request != NULL)
			hal_printf(
			    "xhci: control port=%u slot=%u request=%02x type=%02x value=%04x index=%04x stage=%u completion=%u residual=%u length=%u state=%u\n",
			    request->port, request->slot, control_request->request,
			    control_request->request_type, control_request->value,
			    control_request->index,
			    request->completion_trb_offset,
			    request->completion_code,
			    (unsigned)request->completion_residual,
			    (unsigned)request->length,
			    xhci_endpoint_state(c, device, request->dci));
		else
			hal_printf(
			    "xhci: transfer completion=%u residual=%u length=%u slot=%u endpoint=%u port=%u direction=%s\n",
			    request->completion_code,
			    (unsigned)request->completion_residual,
			    (unsigned)request->length, request->slot, request->dci,
			    request->port, request->input ? "in" : "out");
	}
	if (request->terminal_status == DRV_USB_URB_COMPLETE &&
	    request->input && request->completion_actual != 0)
		memcpy(drv_usb_urb_buffer(urb), request->bounce.address,
		    request->completion_actual);
	drv_usb_urb_set_hcd_data(urb, NULL);
	/* Return request/DMA ownership before terminal publication, allowing a
	 * callback to submit a different URB immediately.  The completed URB stays
	 * HCD-owned until its own callback returns. */
	xhci_request_release(c, request);
	drv_usb_hcd_complete(&c->hcd, urb, terminal_status, completion_actual);

	irq = spin_lock_irqsave(&c->active_lock);
	/* transfer_claim incremented this device's completions_busy before unlink;
	 * xhci_device_quiesce cannot release the endpoint graph until the marker is
	 * cleared and that checked owner is dropped below. */
	if (stall_endpoint != NULL) {
		if (!stall_endpoint->stall_publishing)
			__builtin_trap();
		stall_endpoint->stall_publishing = 0U;
		xhci_recovery_leave_locked(c, stall_endpoint);
	}
	if (device->completions_busy == 0)
		__builtin_trap();
	device->completions_busy--;
	spin_unlock_irqrestore(&c->active_lock, irq);
	if (atomic_raw_fetch_add_release(&c->completion_busy,
	    (unsigned)-1) == 0)
		__builtin_trap();
}

static void
xhci_completion_drain(struct xhci_controller *c)
{
	struct xhci_request *request;
	unsigned long irq;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->completion_dispatch_busy) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return;
	}
	c->completion_dispatch_busy = 1U;
	for (;;) {
		request = c->completion_head;
		if (request == NULL) {
			c->completion_tail = NULL;
			c->completion_dispatch_busy = 0;
			spin_unlock_irqrestore(&c->active_lock, irq);
			return;
		}
		c->completion_head = request->completion_next;
		if (c->completion_head == NULL)
			c->completion_tail = NULL;
		request->completion_next = NULL;
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_completion_finish(c, request);
		irq = spin_lock_irqsave(&c->active_lock);
	}
}
static int
xhci_irq(void *argument)
{
	struct xhci_controller *c = argument;
	struct xhci_trb event;
	int available;
	int handled = 0;
	uint32_t status;
	if (atomic_raw_fetch_add_relaxed(&c->irq_busy, 1U) == UINT_MAX)
		__builtin_trap();
	status = rd32(c->operational, XHCI_USBSTS);
	if (!(status & (XHCI_STS_EINT | XHCI_STS_FATAL)))
		goto out;
	wr32(c->operational, XHCI_USBSTS, status);
	wr32(c->runtime, 0x20U, rd32(c->runtime, 0x20U) | 1U);
	for (;;) {
		unsigned type;

		event_lock(c);
		available = event_take(c, &event);
		type = available ? (event.control >> 10) & 0x3fU : 0;
		if (available && type == 32U)
			(void)transfer_claim(c, &event);
		event_unlock(c);
		if (!available)
			break;
		handled = 1;
		if (type == 33U) {
			uint64_t pointer = xhci_event_pointer(&event);

			event_lock(c);
			if (__atomic_load_n(&c->command_busy,
			    __ATOMIC_ACQUIRE) != 0 &&
			    drv_xhci_command_completion_matches(
				c->command_address, pointer) &&
			    !c->command_event_ready) {
				c->command_event = event;
				c->command_event_ready = 1;
			} else {
				hal_printf(
				    "xhci: unmatched command completion %x:%x\n",
				    event.parameter_high, event.parameter_low);
			}
			event_unlock(c);
		}
		else if (type == 34U)
			port_change_defer(c);
	}
	/* A polling command owns command_busy and will drain every claim after it
	 * releases that gate.  Never run a command-recursive callback from the
	 * competing IRQ path while the command is still in flight. */
	if (__atomic_load_n(&c->command_busy, __ATOMIC_ACQUIRE) == 0)
		xhci_completion_drain(c);
out:
	if (atomic_raw_fetch_add_release(&c->irq_busy, (unsigned)-1) == 0)
		__builtin_trap();
	return handled;
}

static void
xhci_port_worker(void *argument)
{
	struct xhci_controller *c = argument;
	for (;;) {
		if (c->port_stopping)
			return;
		if (__atomic_exchange_n(&c->port_pending, 0U,
					__ATOMIC_ACQ_REL)) {
			drv_usb_hcd_root_hub_changed(&c->hcd);
			continue;
		}
		kernel_wait_task();
	}
}

static int
xhci_worker_start(struct xhci_controller *c)
{
	struct thread *worker;
	int error;
	c->port_stopping = 0;
	c->port_pending = 0;
	error = kthread_create(xhci_port_worker, c, SCHED_PRIORITY_DEFAULT,
			       &worker);
	if (error)
		return error;
	c->port_worker = worker;
	thread_start(worker);
	return 0;
}

static void
xhci_worker_stop(struct xhci_controller *c)
{
	struct thread *worker = c->port_worker;
	if (!worker)
		return;
	c->port_worker = NULL;
	c->port_stopping = 1;
	kernel_notify_task(worker->task);
	while (worker->state != THREAD_ZOMBIE)
		sched_yield();
	(void)thread_wait(worker, NULL);
}

static unsigned
normal_trb_count(uint64_t address, size_t length)
{
	unsigned count = 0;
	if (length == 0)
		return 1;
	while (length != 0) {
		size_t chunk = 0x10000U - (size_t)(address & 0xffffU);
		if (chunk > length)
			chunk = length;
		address += chunk;
		length -= chunk;
		count++;
	}
	return count;
}

static uint64_t
enqueue_normal(struct xhci_ring *ring, uint64_t address, size_t length,
	       int input, size_t maximum_packet_size)
{
	unsigned count = normal_trb_count(address, length);
	size_t cumulative = 0;
	size_t total_length = length;
	uint64_t final_trb = 0;
	if (length == 0) {
		return ring_push(ring, address, 0,
		    XHCI_TRB_TYPE(1) | XHCI_TRB_IOC);
	}
	while (length != 0) {
		size_t chunk = 0x10000U - (size_t)(address & 0xffffU);
		uint32_t control = XHCI_TRB_TYPE(1);
		if (chunk > length)
			chunk = length;
		count--;
		cumulative += chunk;
		if (input)
			control |= DRV_XHCI_TRB_ISP;
		if (count != 0)
			control |= XHCI_TRB_CHAIN;
		else
			control |= XHCI_TRB_IOC;
		final_trb = ring_push(ring, address,
		    (uint32_t)chunk |
			(drv_xhci_normal_td_size(total_length, cumulative,
			    maximum_packet_size, count == 0) << 17),
		    control);
		address += chunk;
		length -= chunk;
	}
	return final_trb;
}

static int
xhci_endpoint_restart_empty(struct xhci_controller *c, struct xhci_device *d,
	struct xhci_endpoint *endpoint, unsigned dci)
{
	uint64_t deadline;
	unsigned state;

	if (endpoint->ring.dma.address == NULL)
		return EIO;
	/* Set TR Dequeue points at the software producer, whose cycle bit denotes
	 * an empty ring.  Ring the endpoint and require hardware to publish Running
	 * before either recovery succeeds or cancelled DMA ownership is released. */
	wr32(c->doorbells, d->slot * 4U, dci);
	deadline = sched_ticks() + 100U;
	for (;;) {
		state = xhci_endpoint_state(c, d, dci);
		if (state == DRV_XHCI_ENDPOINT_RUNNING)
			return 0;
		if (state == DRV_XHCI_ENDPOINT_DISABLED ||
		    state == DRV_XHCI_ENDPOINT_HALTED ||
		    state == DRV_XHCI_ENDPOINT_ERROR)
			return EIO;
		if (c->controller_stopping || c->dma_quiesced || d->quiescing)
			return ENODEV;
		if (sched_ticks() >= deadline)
			return ETIMEDOUT;
		sched_yield();
	}
}

static int
xhci_endpoint_recover(struct xhci_controller *c, struct xhci_device *d,
	struct xhci_endpoint *endpoint, unsigned dci)
{
	enum drv_xhci_cancel_action action;
	enum drv_xhci_cancel_action previous_action =
	    DRV_XHCI_CANCEL_QUIESCE_CONTROLLER;
	unsigned state = UINT32_MAX, previous_state = UINT32_MAX;
	unsigned attempt, completion = 0;
	uint64_t dequeue;
	int error = EIO;

	for (attempt = 0; attempt < 8U; attempt++) {
		state = xhci_endpoint_state(c, d, dci);
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
		switch (action) {
		case DRV_XHCI_CANCEL_COMPLETE:
			return 0;
		case DRV_XHCI_CANCEL_RESET_ENDPOINT:
			error = command_ex(c, 0, 0,
			    XHCI_TRB_TYPE(14) | (dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
			break;
		case DRV_XHCI_CANCEL_SET_TR_DEQUEUE:
			if (endpoint->ring.dma.address == NULL)
				return EIO;
			dequeue = endpoint->ring.dma.device_address +
			    (uint64_t)endpoint->ring.enqueue *
				sizeof(struct xhci_trb);
			dequeue |= endpoint->ring.cycle ? 1U : 0U;
			error = command_ex(c, dequeue, 0,
			    XHCI_TRB_TYPE(16) | (dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
			if (error == 0) {
				error = xhci_endpoint_restart_empty(c, d, endpoint,
				    dci);
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
		/* Completion 19 can be a state-transition race.  Re-read the
		 * output context, but never repeat a command against an unchanged
		 * state/action pair. */
		if (error != 0 && completion != 19U)
			break;
	}
	hal_printf(
	    "xhci: slot %u endpoint %u recovery failed (%d, completion=%u, state=%u); new TD rejected\n",
	    d->slot, dci, error, completion, state);
	return error != 0 ? error : EIO;
}

static int
xhci_endpoint_reset(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d;
	struct xhci_endpoint *endpoint;
	enum drv_xhci_endpoint_reset_admission admission;
	uint64_t wait_started;
	unsigned dci;
	unsigned long irq;
	int error;

	if (usbep == NULL)
		return EINVAL;
	d = xhci_usb_device(drv_usb_endpoint_device(usbep));
	if (d == NULL)
		return ENODEV;
	dci = (drv_usb_endpoint_address(usbep) & 15U) * 2U +
	    (drv_usb_endpoint_is_input(usbep) ? 1U : 0U);
	if (dci < 2U || dci >= 32U)
		return EINVAL;
	endpoint = &d->endpoints[dci];
	wait_started = sched_ticks();
	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		if (!endpoint->enabled || endpoint->dci != dci ||
		    endpoint->ring.dma.address == NULL || d->quiescing ||
		    c->controller_stopping || c->dma_quiesced) {
			error = !endpoint->enabled || d->quiescing ||
			    c->controller_stopping || c->dma_quiesced ? ENODEV : EIO;
			spin_unlock_irqrestore(&c->active_lock, irq);
			return error;
		}
		admission = drv_xhci_endpoint_reset_admit(
		    endpoint->active != NULL, endpoint->recovering,
		    endpoint->stall_publishing);
		if (admission == DRV_XHCI_ENDPOINT_RESET_ACQUIRE) {
			if (c->endpoint_recoveries_busy == UINT_MAX)
				__builtin_trap();
			endpoint->recovering = 1U;
			c->endpoint_recoveries_busy++;
			spin_unlock_irqrestore(&c->active_lock, irq);
			break;
		}
		spin_unlock_irqrestore(&c->active_lock, irq);
		if (admission != DRV_XHCI_ENDPOINT_RESET_WAIT_PUBLICATION)
			return EBUSY;
		/* drv_usb_urb_drain() may observe the core HCD reference drop just
		 * before this completion thread releases its STALL publication owner.
		 * Join only that bounded handoff; every other recovery remains EBUSY. */
		if (sched_ticks() - wait_started >= 100U)
			return EBUSY;
		sched_yield();
	}

	error = xhci_endpoint_recover(c, d, endpoint, dci);
	irq = spin_lock_irqsave(&c->active_lock);
	if (error == 0 && (d->quiescing || c->controller_stopping ||
	    c->dma_quiesced))
		error = ENODEV;
	xhci_recovery_leave_locked(c, endpoint);
	spin_unlock_irqrestore(&c->active_lock, irq);
	return error;
}

/*
 * Reclaim may reach a USB-backed swap source after consuming the last free
 * physical page.  A transfer on that path must not allocate the DMA page
 * which is needed to free a page.  USB storage serializes its BOT stages, so
 * one request and one bounded coherent buffer reserved at start are sufficient
 * for its reclaim-safe transfers even while unrelated endpoints remain
 * active.  Ordinary traffic, including persistent networking, always uses the
 * dynamic path.  A caller marks larger storage I/O ordinary before submit.
 */
static struct xhci_request *
xhci_request_alloc(struct xhci_controller *c, struct drv_usb_hcd *h,
	size_t length, unsigned flags, int *error)
{
	struct xhci_request *request;
	enum drv_xhci_reserve_action action;
	unsigned long irq;
	int allocation_error;

	if ((flags & DRV_USB_URB_RECLAIM_SAFE) != 0) {
		irq = spin_lock_irqsave(&c->active_lock);
		action = drv_xhci_reserve_action(1, length,
		    c->transfer_reserve.size,
		    c->transfer_reserve.address != NULL,
		    c->transfer_reserve_busy != 0);
		if (action != DRV_XHCI_RESERVE_USE) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			*error = action == DRV_XHCI_RESERVE_BUSY ? EBUSY :
			    length > DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE ?
			    EMSGSIZE : ENOMEM;
			return NULL;
		}
		c->transfer_reserve_busy = 1U;
		spin_unlock_irqrestore(&c->active_lock, irq);
		request = &c->transfer_request;
		memset(request, 0, sizeof(*request));
		request->reserved = 1U;
		request->bounce = c->transfer_reserve;
		*error = 0;
		return request;
	}

	request = hal_malloc(sizeof(*request));
	if (request == NULL) {
		*error = ENOMEM;
		return NULL;
	}
	memset(request, 0, sizeof(*request));
	allocation_error = drv_dma_alloc_coherent(h->dma,
	    length != 0 ? length : 8U, 64U, &request->bounce);
	if (allocation_error != 0) {
		hal_free(request);
		*error = allocation_error;
		return NULL;
	}
	*error = 0;
	return request;
}

static void
xhci_request_release(struct xhci_controller *c, struct xhci_request *request)
{
	unsigned long irq;

	if (!request->reserved) {
		drv_dma_free_coherent(c->hcd.dma, &request->bounce);
		hal_free(request);
		return;
	}
	irq = spin_lock_irqsave(&c->active_lock);
	if (request->endpoint != NULL && request->endpoint->active == request) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}
	if (!c->transfer_reserve_busy) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}
	memset(request, 0, sizeof(*request));
	c->transfer_reserve_busy = 0;
	spin_unlock_irqrestore(&c->active_lock, irq);
}

static int
xhci_submission_enter(struct xhci_controller *c)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->controller_stopping || c->dma_quiesced) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return ENODEV;
	}
	if (c->submissions_busy == UINT_MAX) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}
	c->submissions_busy++;
	spin_unlock_irqrestore(&c->active_lock, irq);
	return 0;
}

/* HCD callbacks other than start/stop/quiesce may use controller MMIO,
 * command/event rings, or device DMA without submitting an URB.  Track them
 * independently so a port worker which entered enumeration just before USB
 * shutdown cannot race the final controller-DMA release. */
static int
xhci_operation_enter(struct xhci_controller *c)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->controller_stopping || c->dma_quiesced) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return ENODEV;
	}
	if (c->operations_busy == UINT_MAX) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}
	c->operations_busy++;
	spin_unlock_irqrestore(&c->active_lock, irq);
	return 0;
}

static void
xhci_operation_leave(struct xhci_controller *c)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->operations_busy == 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}
	c->operations_busy--;
	spin_unlock_irqrestore(&c->active_lock, irq);
}

static void
xhci_submission_leave(struct xhci_controller *c)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&c->active_lock);
	if (c->submissions_busy == 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		__builtin_trap();
	}
	c->submissions_busy--;
	spin_unlock_irqrestore(&c->active_lock, irq);
}

static int
xhci_urb_enqueue(struct drv_usb_hcd *h, struct drv_usb_urb *u)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = xhci_usb_device(drv_usb_urb_device(u));
	struct xhci_endpoint *ep;
	struct xhci_request *r;
	struct drv_xhci_trb_words setup_words = { 0 };
	struct drv_xhci_trb_words data_words = { 0 };
	struct drv_xhci_trb_words status_words = { 0 };
	enum drv_xhci_control_data control_data = DRV_XHCI_CONTROL_NO_DATA;
	const struct drv_usb_control_request *q =
	    drv_usb_urb_control_request(u);
	size_t length = drv_usb_urb_length(u);
	uint64_t dma;
	unsigned dci, maximum_packet_size = 0;
	unsigned long irq;
	int e, input;

	e = xhci_submission_enter(c);
	if (e != 0)
		return e;
	if (!d) {
		xhci_submission_leave(c);
		return ENODEV;
	}
	dci = q ? 1U
		: ((drv_usb_endpoint_address(drv_usb_urb_endpoint(u)) & 15U) *
		       2U +
		   (drv_usb_endpoint_is_input(drv_usb_urb_endpoint(u)) ? 1U
								       : 0U));
	if (dci == 0 || dci >= 32U) {
		xhci_submission_leave(c);
		return EINVAL;
	}
	ep = &d->endpoints[dci];
	if (q == NULL) {
		maximum_packet_size = drv_usb_endpoint_max_packet_size(
		    drv_usb_urb_endpoint(u)) & 0x7ffU;
		if (maximum_packet_size == 0) {
			xhci_submission_leave(c);
			return EINVAL;
		}
	}
	irq = spin_lock_irqsave(&c->active_lock);
	if (!ep->enabled || d->quiescing || c->controller_stopping ||
	    ep->active != NULL || ep->recovering) {
		e = !ep->enabled || d->quiescing || c->controller_stopping ?
		    ENODEV : EBUSY;
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_submission_leave(c);
		return e;
	}
	if (c->endpoint_recoveries_busy == UINT_MAX)
		__builtin_trap();
	ep->recovering = 1U;
	c->endpoint_recoveries_busy++;
	spin_unlock_irqrestore(&c->active_lock, irq);

	r = xhci_request_alloc(c, h, length, drv_usb_urb_flags(u), &e);
	if (r == NULL) {
		irq = spin_lock_irqsave(&c->active_lock);
		xhci_recovery_leave_locked(c, ep);
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_submission_leave(c);
		return e;
	}
	r->urb = u;
	r->device = d;
	r->endpoint = ep;
	r->length = length;
	r->input = q ? (q->request_type & DRV_USB_DIR_IN) != 0
		     : drv_usb_endpoint_is_input(drv_usb_urb_endpoint(u));
	if (!r->input && length)
		memcpy(r->bounce.address, drv_usb_urb_buffer(u), length);
	dma = r->bounce.device_address;
	if ((!q && normal_trb_count(dma, length) >= XHCI_RING_TRBS - 1U) ||
	    (q && length != 0 &&
	     (length > 0x10000U || (dma & 0xffffU) + length > 0x10000U))) {
		xhci_request_release(c, r);
		irq = spin_lock_irqsave(&c->active_lock);
		xhci_recovery_leave_locked(c, ep);
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_submission_leave(c);
		return EOVERFLOW;
	}
	input = r->input;
	if (q != NULL) {
		uint64_t setup = 0;

		memcpy(&setup, q, sizeof(*q));
		control_data = length == 0 ? DRV_XHCI_CONTROL_NO_DATA :
		    input ? DRV_XHCI_CONTROL_DATA_IN :
			DRV_XHCI_CONTROL_DATA_OUT;
		if (!drv_xhci_control_setup_words(setup, control_data,
		    &setup_words) ||
		    (length != 0 && !drv_xhci_control_data_words(dma,
			(uint32_t)length, control_data, &data_words)) ||
		    !drv_xhci_control_status_words(control_data, &status_words)) {
			xhci_request_release(c, r);
			irq = spin_lock_irqsave(&c->active_lock);
			xhci_recovery_leave_locked(c, ep);
			spin_unlock_irqrestore(&c->active_lock, irq);
			xhci_submission_leave(c);
			return EINVAL;
		}
	}
	r->slot = d->slot;
	r->dci = dci;
	r->port = drv_usb_device_port(drv_usb_urb_device(u));
	/* EP0 retains implicit checked recovery because a control STALL is not
	 * core-latched.  Every nonzero endpoint must already be Running; only the
	 * explicit endpoint_reset callback may issue its recovery commands. */
	e = q != NULL ? xhci_endpoint_recover(c, d, ep, dci) :
	    xhci_endpoint_state(c, d, dci) == DRV_XHCI_ENDPOINT_RUNNING ?
		0 : EIO;
	irq = spin_lock_irqsave(&c->active_lock);
	if (e != 0 || ep->active != NULL || d->quiescing ||
	    c->controller_stopping) {
		if (e == 0)
			e = d->quiescing || c->controller_stopping ?
			    ENODEV : EBUSY;
		xhci_recovery_leave_locked(c, ep);
		spin_unlock_irqrestore(&c->active_lock, irq);
		xhci_request_release(c, r);
		xhci_submission_leave(c);
		return e;
	}
	r->first_trb = ep->ring.enqueue;
	r->trb_count = q != NULL ? (length ? 3U : 2U) :
	    normal_trb_count(dma, length);
	if (xhci_request_publish_locked(c, r) != 0)
		__builtin_trap();
	/* Publish the request, its URB association, the complete TD, and its
	 * doorbell under one barrier.  Teardown takes this same lock and must
	 * never observe a half-built request which it could cancel and free. */
	drv_usb_urb_set_hcd_data(u, r);
	if (q) {
		ring_push(&ep->ring,
		    (uint64_t)setup_words.parameter_low |
			((uint64_t)setup_words.parameter_high << 32),
		    setup_words.status, setup_words.control);
		if (length)
			ring_push(&ep->ring,
			    (uint64_t)data_words.parameter_low |
				((uint64_t)data_words.parameter_high << 32),
			    data_words.status, data_words.control);
		(void)ring_push(&ep->ring,
		    (uint64_t)status_words.parameter_low |
			((uint64_t)status_words.parameter_high << 32),
		    status_words.status, status_words.control);
	} else {
		(void)enqueue_normal(&ep->ring, dma, length, input,
		    maximum_packet_size);
	}
	wr32(c->doorbells, d->slot * 4U, dci);
	xhci_recovery_leave_locked(c, ep);
	spin_unlock_irqrestore(&c->active_lock, irq);
	xhci_submission_leave(c);
	return 0;
}

static unsigned
xhci_endpoint_state(struct xhci_controller *c, struct xhci_device *d,
	unsigned dci)
{
	volatile uint32_t *context;

	if (d == NULL || dci == 0 || dci >= 32U ||
	    d->output_context.address == NULL)
		return 7U;
	context = (volatile uint32_t *)((uint8_t *)d->output_context.address +
	    (size_t)dci * c->context_size);
	hal_io_rmb();
	return context[0] & 7U;
}

static int
xhci_cancel_request(struct xhci_controller *c, struct xhci_device *d,
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

	for (attempt = 0; attempt < 8U; attempt++) {
		state = (enum drv_xhci_endpoint_state)
		    xhci_endpoint_state(c, d, r->dci);
		action = drv_xhci_cancel_action(state, c->dma_quiesced != 0);
		if (attempt != 0 && state == previous_state &&
		    action == previous_action) {
			error = EIO;
			goto retain;
		}
		previous_state = state;
		previous_action = action;
		completion = 0;
		switch (action) {
		case DRV_XHCI_CANCEL_COMPLETE:
			releasable = drv_xhci_request_resources_releasable(0, 1);
			goto release;
		case DRV_XHCI_CANCEL_STOP_ENDPOINT:
			error = command_ex(c, 0, 0,
			    XHCI_TRB_TYPE(15) | (r->dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
			break;
		case DRV_XHCI_CANCEL_RESET_ENDPOINT:
			error = command_ex(c, 0, 0,
			    XHCI_TRB_TYPE(14) | (r->dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
			break;
		case DRV_XHCI_CANCEL_SET_TR_DEQUEUE:
			/* Queue depth is one per endpoint.  The producer is the first
			 * safe dequeue position after this endpoint's cancelled TD.  Keep
			 * the owner published until this command completes so the same ring
			 * slots cannot be reused after a software-only unlink. */
			dequeue = ep->ring.dma.device_address +
			    (uint64_t)ep->ring.enqueue * sizeof(struct xhci_trb);
			dequeue |= ep->ring.cycle ? 1U : 0U;
			error = command_ex(c, dequeue, 0,
			    XHCI_TRB_TYPE(16) | (r->dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
			if (error == 0) {
				/* Set TR Dequeue is the DMA-ownership boundary.  A normal
				 * cancellation must restart the emptied endpoint before it can
				 * accept another TD.  Disconnect has permanently closed core
				 * admission, so demanding Running from an absent endpoint would
				 * retain an already retired request forever. */
				if (drv_xhci_cancel_post_dequeue_action(
				    drv_usb_device_is_tearing_down(
					drv_usb_urb_device(r->urb))) ==
				    DRV_XHCI_POST_DEQUEUE_RELEASE_REQUEST) {
					releasable =
					    drv_xhci_request_resources_releasable(1, 0);
					goto release;
				}
				error = xhci_endpoint_restart_empty(c, d, ep,
				    r->dci);
				if (error == 0) {
					releasable =
					    drv_xhci_request_resources_releasable(1, 0);
					goto release;
				}
			}
			break;
		case DRV_XHCI_CANCEL_QUIESCE_CONTROLLER:
		default:
			error = EIO;
			goto retain;
		}
		/* Context State Error means software raced a hardware state
		 * transition.  Re-read the output context; never blindly accept it. */
		if (error != 0 && completion != 19U)
			goto retain;
	}
	error = EIO;

retain:
	irq = spin_lock_irqsave(&c->active_lock);
	if (r->endpoint->active == r)
		r->cancelling = 2U;
	spin_unlock_irqrestore(&c->active_lock, irq);
	hal_printf(
	    "xhci: slot %u endpoint %u port %u cancel failed (%d, completion=%u, state=%u); request and DMA retained\n",
	    r->slot, r->dci, r->port, error, completion, (unsigned)state);
	return error;

release:
	if (!releasable)
		goto retain;
	irq = spin_lock_irqsave(&c->active_lock);
	if (!xhci_request_unlink_locked(c, r)) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EBUSY;
	}
	spin_unlock_irqrestore(&c->active_lock, irq);
	drv_usb_urb_set_hcd_data(r->urb, NULL);
	xhci_request_release(c, r);
	return 0;
}

static int
xhci_urb_dequeue(struct drv_usb_hcd *h, struct drv_usb_urb *u)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d;
	struct xhci_endpoint *endpoint;
	struct xhci_request *r;
	const struct drv_usb_control_request *control;
	unsigned dci;
	unsigned long irq;

	d = xhci_usb_device(drv_usb_urb_device(u));
	if (d == NULL)
		return ENODEV;
	control = drv_usb_urb_control_request(u);
	dci = control != NULL ? 1U :
	    (drv_usb_endpoint_address(drv_usb_urb_endpoint(u)) & 15U) * 2U +
	    (drv_usb_endpoint_is_input(drv_usb_urb_endpoint(u)) ? 1U : 0U);
	if (dci == 0 || dci >= 32U)
		return EINVAL;
	endpoint = &d->endpoints[dci];
	irq = spin_lock_irqsave(&c->active_lock);
	r = endpoint->active;
	if (r == NULL || r->urb != u || r->device != d || r->dci != dci) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EBUSY;
	}
	if (r->cancelling == 1U) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EALREADY;
	}
	r->cancelling = 1U;
	spin_unlock_irqrestore(&c->active_lock, irq);
	return xhci_cancel_request(c, d, r);
}

static int
xhci_endpoint_quiesce(struct xhci_controller *c, struct xhci_device *d,
	unsigned dci)
{
	struct xhci_endpoint *endpoint = &d->endpoints[dci];
	uint64_t dequeue;
	unsigned attempt, completion = 0;
	unsigned state = DRV_XHCI_ENDPOINT_DISABLED;
	unsigned previous_state = UINT32_MAX;
	int error;

	if (!endpoint->enabled || c->dma_quiesced)
		return 0;
	for (attempt = 0; attempt < 8U; attempt++) {
		state = xhci_endpoint_state(c, d, dci);
		if (attempt != 0 && state == previous_state) {
			error = EIO;
			break;
		}
		previous_state = state;
		completion = 0;
		if (state == DRV_XHCI_ENDPOINT_DISABLED)
			return 0;
		if (state == DRV_XHCI_ENDPOINT_RUNNING)
			error = command_ex(c, 0, 0,
			    XHCI_TRB_TYPE(15) | (dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
		else if (state == DRV_XHCI_ENDPOINT_HALTED)
			error = command_ex(c, 0, 0,
			    XHCI_TRB_TYPE(14) | (dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
		else if (state == DRV_XHCI_ENDPOINT_STOPPED ||
		    state == DRV_XHCI_ENDPOINT_ERROR) {
			if (endpoint->ring.dma.address == NULL)
				return EIO;
			dequeue = endpoint->ring.dma.device_address +
			    (uint64_t)endpoint->ring.enqueue *
				sizeof(struct xhci_trb);
			dequeue |= endpoint->ring.cycle ? 1U : 0U;
			error = command_ex(c, dequeue, 0,
			    XHCI_TRB_TYPE(16) | (dci << 16) |
				XHCI_TRB_SLOT(d->slot), NULL, &completion);
			if (error == 0)
				return 0;
		} else {
			error = EIO;
		}
		if (error != 0 && completion != 19U)
			break;
	}
	hal_printf(
	    "xhci: slot %u endpoint %u teardown quiesce failed (%d, completion=%u, state=%u); slot retained\n",
	    d->slot, dci, error, completion, state);
	return error != 0 ? error : EIO;
}

static int
xhci_device_quiesce(struct drv_usb_hcd *h, struct drv_usb_device *u)
{
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

	if (d == NULL)
		return 0;
	if (d->slot_disabled)
		return 0;
	/* Close admission before inspecting endpoint ownership.  A submit which
	 * entered first either publishes a complete TD or leaves its endpoint's
	 * recovery barrier before this loop proceeds. */
	wait_started = sched_ticks();
	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		d->quiescing = 1U;
		if (!xhci_device_recovery_busy_locked(d) &&
		    d->completions_busy == 0) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			break;
		}
		spin_unlock_irqrestore(&c->active_lock, irq);
		if (sched_ticks() - wait_started >= 100U) {
			hal_printf(
			    "xhci: slot %u teardown completion barrier timed out; ownership retained\n",
			    d->slot);
			return EBUSY;
		}
		sched_yield();
	}

	/* Drain every endpoint owned by this device.  A failed endpoint remains
	 * published and therefore quarantined, but cannot prevent a different
	 * endpoint from reaching its own checked cancellation boundary. */
	wait_started = sched_ticks();
	for (;;) {
		r = NULL;
		urb = NULL;
		wait_for_cancel = 0;
		irq = spin_lock_irqsave(&c->active_lock);
		for (dci = 1; dci < 32U; dci++) {
			struct xhci_request *candidate = d->endpoints[dci].active;

			if (candidate == NULL || (attempted & (1U << dci)) != 0)
				continue;
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
		if (r != NULL) {
			error = xhci_cancel_request(c, d, r);
			if (error != 0) {
				if (first_error == 0)
					first_error = error;
			} else {
				/* The owner and DMA were detached by Stop Endpoint plus Set
				 * TR Dequeue (or by a fully quiesced controller). */
				drv_usb_hcd_complete(h, urb,
				    DRV_USB_URB_DISCONNECTED, 0);
			}
			continue;
		}
		if (!wait_for_cancel)
			break;
		if (sched_ticks() - wait_started >= 100U) {
			if (first_error == 0)
				first_error = EBUSY;
			break;
		}
		sched_yield();
	}
	if (first_error != 0)
		return first_error;

	/* A user cancellation can detach the hardware owner just before its USB
	 * terminal publication.  Do not Disable Slot inside that publication
	 * window. */
	wait_started = sched_ticks();
	for (;;) {
		owned = drv_usb_device_hcd_urb_count(u);
		irq = spin_lock_irqsave(&c->active_lock);
		completion = d->completions_busy;
		wait_for_cancel = xhci_device_request_busy_locked(d) ||
		    xhci_device_recovery_busy_locked(d);
		spin_unlock_irqrestore(&c->active_lock, irq);
		if (owned == 0 && completion == 0 && !wait_for_cancel)
			break;
		if (sched_ticks() - wait_started >= 100U) {
			hal_printf(
			    "xhci: slot %u teardown timed out (URBs=%u completion=%u endpoint=%u); ownership retained\n",
			    d->slot, owned, completion, wait_for_cancel);
			return EBUSY;
		}
		sched_yield();
	}
	for (dci = 1; dci < 32U; dci++) {
		error = xhci_endpoint_quiesce(c, d, dci);
		if (error != 0)
			return error;
	}
	if (c->dma_quiesced) {
		d->slot_disabled =
		    drv_xhci_device_resources_releasable(0, 1) ? 1U : 0U;
		if (d->slot_disabled)
			xhci_default_owner_release(c, d);
		return d->slot_disabled ? 0 : EIO;
	}
	error = command_ex(c, 0, 0,
	    XHCI_TRB_TYPE(10) | XHCI_TRB_SLOT(d->slot), NULL, &completion);
	if (error != 0) {
		hal_printf(
		    "xhci: slot %u Disable Slot failed (%d, completion=%u); rings and contexts retained\n",
		    d->slot, error, completion);
		return error;
	}
	if (!drv_xhci_device_resources_releasable(1, 0))
		return EIO;
	d->slot_disabled = 1U;
	xhci_default_owner_release(c, d);
	return 0;
}

static uint32_t
xhci_frame(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	return rd32(c->runtime, 0) & 0x3fffU;
}
static int
xhci_root_status(struct drv_usb_hcd *h, void *b, size_t n, size_t *a)
{
	struct xhci_controller *c = hcd_controller(h);
	uint8_t *bits = b;
	unsigned p, bytes = (c->ports + 1U + 7U) / 8U;
	if (!b || n < bytes)
		return EINVAL;
	memset(bits, 0, bytes);
	for (p = 0; p < c->ports; p++)
		if (rd32(c->operational, XHCI_PORTSC(p)) & XHCI_PORT_CHANGE)
			bits[(p + 1U) / 8U] |= (uint8_t)(1U << ((p + 1U) & 7U));
	if (a)
		*a = bytes;
	return 0;
}
static int
xhci_root_control(struct drv_usb_hcd *h,
		  const struct drv_usb_control_request *r, void *b, size_t n,
		  size_t *a)
{
	struct xhci_controller *c = hcd_controller(h);
	unsigned p;
	uint32_t s, v = 0;
	if (!r || r->index < 1 || r->index > c->ports)
		return EINVAL;
	p = r->index - 1U;
	s = rd32(c->operational, XHCI_PORTSC(p));
	if (r->request == 0 && b && n >= 4) {
		unsigned speed = (s >> 10) & 15U;
		if (s & XHCI_PORT_CCS)
			v |= 1U;
		if (s & XHCI_PORT_PED)
			v |= 2U;
		if (s & XHCI_PORT_PR)
			v |= 0x10U;
		if (s & XHCI_PORT_PP)
			v |= 0x100U;
		if (speed == 2U)
			v |= 0x200U;
		else if (speed == 3U)
			v |= 0x400U;
		else if (speed >= 4U)
			v |= 0x800U;
		if (s & (1U << 17))
			v |= 0x10000U;
		if (s & (1U << 18))
			v |= 0x20000U;
		if (s & (1U << 19))
			v |= 0x200000U;
		if (s & (1U << 20))
			v |= 0x80000U;
		if (s & (1U << 21))
			v |= 0x100000U;
		if (s & (1U << 22))
			v |= 0x400000U;
		if (s & (1U << 23))
			v |= 0x800000U;
		memcpy(b, &v, 4);
		if (a)
			*a = 4;
		return 0;
	}
	if (r->request == 3 && r->value == 4) {
		wr32(c->operational, XHCI_PORTSC(p),
		     (s & XHCI_PORT_PP) | XHCI_PORT_PR | XHCI_PORT_PP);
		if (a)
			*a = 0;
		return 0;
	}
	if (r->request == 1) {
		uint32_t change = 0;
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
			return ENOTSUP;
		wr32(c->operational, XHCI_PORTSC(p),
		     (s & XHCI_PORT_PP) | change);
		if (a)
			*a = 0;
		return 0;
	}
	if (r->request == 3 && r->value == 8) {
		wr32(c->operational, XHCI_PORTSC(p), XHCI_PORT_PP);
		if (a)
			*a = 0;
		return 0;
	}
	return ENOTSUP;
}

static int
xhci_root_port_reset(struct drv_usb_hcd *h, unsigned port)
{
	struct xhci_controller *c = hcd_controller(h);
	enum drv_xhci_port_reset_decision decision;
	uint64_t deadline;
	uint32_t portsc;
	unsigned index;

	if (port == 0 || port > c->ports)
		return EINVAL;
	index = port - 1U;
	portsc = rd32(c->operational, XHCI_PORTSC(index));
	if (portsc == UINT32_MAX)
		return EIO;
	if ((portsc & XHCI_PORT_CSC) != 0)
		return ENODEV;
	if ((portsc & XHCI_PORT_CCS) == 0)
		return ENODEV;
	if ((portsc & XHCI_PORT_CHANGE) != 0)
		wr32(c->operational, XHCI_PORTSC(index),
		    (portsc & XHCI_PORT_PP) |
			(portsc & (XHCI_PORT_CHANGE & ~XHCI_PORT_CSC)));
	portsc = rd32(c->operational, XHCI_PORTSC(index));
	if (portsc == UINT32_MAX)
		return EIO;
	if ((portsc & XHCI_PORT_CSC) != 0 ||
	    (portsc & XHCI_PORT_CCS) == 0)
		return ENODEV;
	wr32(c->operational, XHCI_PORTSC(index),
	    (portsc & XHCI_PORT_PP) | XHCI_PORT_PP | XHCI_PORT_PR);
	deadline = sched_ticks() + 100U;
	for (;;) {
		portsc = rd32(c->operational, XHCI_PORTSC(index));
		decision = drv_xhci_port_reset_status(portsc);
		if (decision == DRV_XHCI_PORT_RESET_SUCCESS) {
			wr32(c->operational, XHCI_PORTSC(index),
			    (portsc & XHCI_PORT_PP) |
				(portsc &
				    (XHCI_PORT_CHANGE & ~XHCI_PORT_CSC)));
			portsc = rd32(c->operational, XHCI_PORTSC(index));
			if (portsc == UINT32_MAX)
				return EIO;
			if ((portsc & XHCI_PORT_CSC) != 0 ||
			    (portsc & XHCI_PORT_CCS) == 0)
				return ENODEV;
			{
				/* Two 10-ms ticks guarantee at least one full recovery
				 * interval even when reset completes on a tick boundary. */
				uint64_t recovery = sched_ticks() + 2U;

				while (sched_ticks() < recovery)
					sched_yield();
			}
			/* The mandatory recovery delay is part of reset.  Preserve a
			 * detach/reinsert edge which arrives during that interval too. */
			portsc = rd32(c->operational, XHCI_PORTSC(index));
			if (portsc == UINT32_MAX)
				return EIO;
			if ((portsc & XHCI_PORT_CSC) != 0 ||
			    (portsc & XHCI_PORT_CCS) == 0)
				return ENODEV;
			hal_printf("xhci: port %u reset complete portsc=%08x\n",
			    port, portsc);
			return 0;
		}
		if (decision == DRV_XHCI_PORT_RESET_DISCONNECTED)
			return ENODEV;
		if (decision == DRV_XHCI_PORT_RESET_INVALID)
			return EIO;
		if (sched_ticks() >= deadline)
			break;
		sched_yield();
	}
	hal_printf("xhci: port %u reset timed out portsc=%08x\n", port,
	    portsc);
	return ETIMEDOUT;
}

static void
xhci_scratchpads_free(struct xhci_controller *c)
{
	unsigned i;
	if (c->scratchpads) {
		for (i = 0; i < c->scratchpad_count; i++)
			if (c->scratchpads[i].address)
				drv_dma_free_coherent(c->hcd.dma,
						      &c->scratchpads[i]);
		hal_free(c->scratchpads);
		c->scratchpads = NULL;
	}
	if (c->scratchpad_array.address)
		drv_dma_free_coherent(c->hcd.dma, &c->scratchpad_array);
}

static int
xhci_scratchpads_alloc(struct xhci_controller *c)
{
	uint64_t *array;
	unsigned i;
	int e;
	if (!c->scratchpad_count)
		return 0;
	c->scratchpads =
	    hal_malloc(sizeof(*c->scratchpads) * c->scratchpad_count);
	if (!c->scratchpads)
		return ENOMEM;
	memset(c->scratchpads, 0,
	       sizeof(*c->scratchpads) * c->scratchpad_count);
	e = drv_dma_alloc_coherent(
	    c->hcd.dma, (size_t)c->scratchpad_count * sizeof(uint64_t), 64U,
	    &c->scratchpad_array);
	if (e)
		goto fail;
	memset(c->scratchpad_array.address, 0, c->scratchpad_array.size);
	array = c->scratchpad_array.address;
	for (i = 0; i < c->scratchpad_count; i++) {
		e = drv_dma_alloc_coherent(c->hcd.dma, 4096U, 4096U,
					   &c->scratchpads[i]);
		if (e)
			goto fail;
		array[i] = c->scratchpads[i].device_address;
	}
	((uint64_t *)c->dcbaa.address)[0] = c->scratchpad_array.device_address;
	return 0;
fail:
	xhci_scratchpads_free(c);
	return e;
}

/* HCHalted, PCI bus-master disable, and IRQ drain are all prerequisites for
 * this software-only ownership drop.  Until then an endpoint owner remains
 * published even when a cancellation command failed, so a late Transfer
 * Event can never alias a reused ring slot. */
static void
xhci_controller_drain_requests(struct xhci_controller *c)
{
	struct xhci_device *device;
	struct xhci_request *request;
	struct drv_usb_urb *urb;
	unsigned dci;
	unsigned long irq;

	for (;;) {
		device = NULL;
		request = NULL;
		urb = NULL;
		irq = spin_lock_irqsave(&c->active_lock);
		for (device = c->devices; device != NULL; device = device->next) {
			for (dci = 1; dci < 32U; dci++) {
				request = device->endpoints[dci].active;
				if (request != NULL)
					break;
			}
			if (request != NULL)
				break;
		}
		if (request == NULL) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			return;
		}
		if (!c->dma_quiesced || request->device != device ||
		    request->endpoint != &device->endpoints[dci] ||
		    device->endpoints[dci].recovering ||
		    !xhci_request_unlink_locked(c, request))
			__builtin_trap();
		if (device->completions_busy == UINT_MAX ||
		    atomic_raw_fetch_add_relaxed(&c->completion_busy, 1U) ==
			UINT_MAX)
			__builtin_trap();
		device->completions_busy++;
		urb = request->urb;
		spin_unlock_irqrestore(&c->active_lock, irq);

		drv_usb_urb_set_hcd_data(urb, NULL);
		xhci_request_release(c, request);
		/* Terminal publication and callbacks must not run under active_lock. */
		drv_usb_hcd_complete(&c->hcd, urb,
		    DRV_USB_URB_DISCONNECTED, 0);

		irq = spin_lock_irqsave(&c->active_lock);
		if (device->completions_busy == 0)
			__builtin_trap();
		device->completions_busy--;
		spin_unlock_irqrestore(&c->active_lock, irq);
		if (atomic_raw_fetch_add_release(&c->completion_busy,
		    (unsigned)-1) == 0)
			__builtin_trap();
	}
}

static int
xhci_submission_quiesce(struct xhci_controller *c)
{
	uint64_t started = sched_ticks();
	unsigned active, completion, command, operations, recovery;
	unsigned submissions;
	unsigned long irq;

	irq = spin_lock_irqsave(&c->active_lock);
	c->controller_stopping = 1U;
	spin_unlock_irqrestore(&c->active_lock, irq);
	for (;;) {
		irq = spin_lock_irqsave(&c->active_lock);
		recovery = c->endpoint_recoveries_busy;
		operations = c->operations_busy;
		submissions = c->submissions_busy;
		active = c->active_count;
		spin_unlock_irqrestore(&c->active_lock, irq);
		completion = atomic_raw_load_acquire(&c->completion_busy);
		command = atomic_raw_load_acquire(&c->command_busy);
		if (!recovery && !operations && !submissions && !completion &&
		    !command)
			return 0;
		if (sched_ticks() - started >= 100U) {
			hal_printf(
			    "xhci: controller operation barrier timed out (operations=%u submit=%u recovery=%u completion=%u command=%u active=%u); retaining all DMA\n",
			    operations, submissions, recovery, completion, command,
			    active);
			return EBUSY;
		}
		sched_yield();
	}
}

static int
xhci_irq_quiesce(struct xhci_controller *c)
{
	uint64_t started = sched_ticks();

	while (atomic_raw_load_acquire(&c->irq_busy) != 0) {
		if (sched_ticks() - started >= 100U) {
			hal_printf(
			    "xhci: IRQ completion barrier timed out; retaining all DMA\n");
			return EIO;
		}
		sched_yield();
	}
	return 0;
}

static int
xhci_irq_disestablish(struct xhci_controller *c)
{
	uint64_t started = sched_ticks();
	int error;

	if (c->irq_cookie == NULL)
		return xhci_irq_quiesce(c);
	/* The checked PCI path masks the source before reporting EBUSY.  That
	 * closes the i386 INTx arrival window while the already-entered handler
	 * drains; only a successful retry owns and frees the cookie. */
	for (;;) {
		error = drv_pci_device_disestablish_irq_checked(c->pci,
		    c->irq_cookie);
		if (error != EBUSY)
			break;
		if (sched_ticks() - started >= 100U) {
			hal_printf(
			    "xhci: IRQ removal barrier timed out; retaining all DMA\n");
			return EBUSY;
		}
		sched_yield();
	}
	if (error != 0) {
		hal_printf(
		    "xhci: checked IRQ disestablish failed (%d); retaining all DMA\n",
		    error);
		return error;
	}
	c->irq_cookie = NULL;
	return xhci_irq_quiesce(c);
}

static int
xhci_quiesce(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	int barrier_error, halt_error, master_error;
	uint32_t command, iman;

	if (c->dma_quiesced)
		return 0;
	barrier_error = xhci_submission_quiesce(c);
	if (barrier_error != 0)
		return barrier_error;
	iman = rd32(c->runtime, 0x20U);
	wr32(c->runtime, 0x20U, (iman & ~2U) | 1U);
	command = rd32(c->operational, XHCI_USBCMD);
	wr32(c->operational, XHCI_USBCMD,
	    command & ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
	halt_error = wait_bits(c->operational, XHCI_USBSTS,
	    XHCI_STS_HALTED, XHCI_STS_HALTED);
	master_error = xhci_bus_master_disable(c);
	if (halt_error != 0) {
		hal_printf(
		    "xhci: stop did not reach HCHalted (halt=%d master=%d); retaining DMA/IRQ state\n",
		    halt_error, master_error);
		return halt_error;
	}
	if (master_error != 0) {
		hal_printf(
		    "xhci: bus-master disable failed; retaining DMA/IRQ state\n");
		return master_error;
	}
	barrier_error = xhci_irq_disestablish(c);
	if (barrier_error != 0)
		return barrier_error;
	/* Only this point proves that no controller actor can reach a request.
	 * Retained endpoint owners may now be detached and terminally published. */
	c->dma_quiesced = 1U;
	xhci_controller_drain_requests(c);
	return 0;
}

static int
xhci_release_resources(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	struct drv_dma_buffer transfer_reserve;
	unsigned resources_safe;
	unsigned long irq;

	if (!c->dma_quiesced) {
		hal_printf("xhci: refusing to release DMA before HCHalted\n");
		return EBUSY;
	}
	memset(&transfer_reserve, 0, sizeof(transfer_reserve));
	irq = spin_lock_irqsave(&c->active_lock);
	resources_safe = c->controller_stopping && c->active_count == 0 &&
	    !c->transfer_reserve_busy && !c->operations_busy &&
	    !c->submissions_busy &&
	    !c->endpoint_recoveries_busy && !c->completion_dispatch_busy &&
	    c->completion_head == NULL && c->completion_tail == NULL &&
	    atomic_raw_load_acquire(&c->completion_busy) == 0 &&
	    atomic_raw_load_acquire(&c->command_busy) == 0 &&
	    atomic_raw_load_acquire(&c->irq_busy) == 0;
	if (resources_safe) {
		transfer_reserve = c->transfer_reserve;
		memset(&c->transfer_reserve, 0, sizeof(c->transfer_reserve));
	}
	spin_unlock_irqrestore(&c->active_lock, irq);
	if (!resources_safe) {
		hal_printf(
		    "xhci: controller resources are still owned; retaining all DMA\n");
		return EBUSY;
	}
	if (transfer_reserve.address != NULL)
		drv_dma_free_coherent(h->dma, &transfer_reserve);
	xhci_scratchpads_free(c);
	if (c->erst_memory.address)
		drv_dma_free_coherent(h->dma, &c->erst_memory);
	if (c->event_memory.address)
		drv_dma_free_coherent(h->dma, &c->event_memory);
	if (c->command.dma.address)
		ring_free(c, &c->command);
	if (c->dcbaa.address)
			drv_dma_free_coherent(h->dma, &c->dcbaa);
	memset(&c->command_memory, 0, sizeof(c->command_memory));
	c->events = NULL;
	return 0;
}

static void
xhci_stop(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	int error;

	error = xhci_release_resources(h);
	if (error != 0)
		c->quarantined = 1U;
}

static int
xhci_stop_checked(struct drv_usb_hcd *h)
{
	int error;

	error = xhci_quiesce(h);
	if (error != 0)
		return error;
	return xhci_release_resources(h);
}

static int
xhci_start(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_erst *erst;
	unsigned long irq;
	int e, stop_error;
	if ((e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_CNR, 0)) != 0)
		return e;
	wr32(c->operational, XHCI_USBCMD,
	     rd32(c->operational, XHCI_USBCMD) & ~XHCI_CMD_RUN);
	if ((e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_HALTED,
			   XHCI_STS_HALTED)) != 0)
		return e;
	wr32(c->operational, XHCI_USBCMD, XHCI_CMD_RESET);
	if ((e = wait_bits(c->operational, XHCI_USBCMD, XHCI_CMD_RESET, 0)) !=
	    0)
		return e;
	if ((e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_CNR, 0)) != 0)
		return e;
	if ((rd32(c->operational, XHCI_PAGESIZE) & 1U) == 0)
		return ENOTSUP;
	irq = spin_lock_irqsave(&c->active_lock);
	if (c->active_count != 0 || c->transfer_reserve_busy ||
	    c->operations_busy || c->submissions_busy ||
	    c->endpoint_recoveries_busy || c->completion_dispatch_busy ||
	    c->completion_head != NULL || c->completion_tail != NULL ||
	    atomic_raw_load_acquire(&c->completion_busy) != 0 ||
	    atomic_raw_load_acquire(&c->command_busy) != 0 ||
	    atomic_raw_load_acquire(&c->irq_busy) != 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return EBUSY;
	}
	c->controller_stopping = 0;
	c->dma_quiesced = 0;
	spin_unlock_irqrestore(&c->active_lock, irq);
	c->command_failed = 0;
	c->default_slot = 0;
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U, &c->dcbaa)) != 0)
		goto fail;
	memset(c->dcbaa.address, 0, 4096U);
	if ((e = xhci_scratchpads_alloc(c)) != 0)
		goto fail;
	if ((e = ring_alloc(c, &c->command)) != 0)
		goto fail;
	c->command_memory = c->command.dma;
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U,
					&c->event_memory)) != 0)
		goto fail;
	if ((e = drv_dma_alloc_coherent(h->dma, 4096U, 64U, &c->erst_memory)) !=
	    0)
		goto fail;
	if ((e = drv_dma_alloc_coherent(h->dma, XHCI_TRANSFER_RESERVE_SIZE,
		XHCI_TRANSFER_RESERVE_SIZE, &c->transfer_reserve)) != 0)
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
	wr32(c->runtime, 0x24U, 4000U);
	wr32(c->runtime, 0x20U, 2U);
	wr32(c->operational, XHCI_CONFIG, c->max_slots);
	wr32(c->operational, XHCI_USBSTS, 0xffffffffU);
	wr32(c->operational, XHCI_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE);
	e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_HALTED, 0);
	if (!e)
		return 0;
fail:
	stop_error = xhci_stop_checked(h);
	if (stop_error != 0)
		return stop_error;
	return e;
}

static int
xhci_guarded_device_enable(struct drv_usb_hcd *h, struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_device_enable(h, u);
	xhci_operation_leave(c);
	return error;
}

static int
xhci_guarded_set_address(struct drv_usb_hcd *h, struct drv_usb_device *u,
	unsigned address)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_set_address(h, u, address);
	xhci_operation_leave(c);
	return error;
}

static int
xhci_guarded_device_quiesce(struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_device_quiesce(h, u);
	xhci_operation_leave(c);
	return error;
}

static void
xhci_guarded_device_disable(struct drv_usb_hcd *h,
	struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);

	if (xhci_operation_enter(c) != 0)
		return;
	xhci_device_disable(h, u);
	xhci_operation_leave(c);
}

static int
xhci_guarded_urb_dequeue(struct drv_usb_hcd *h, struct drv_usb_urb *u)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_urb_dequeue(h, u);
	xhci_operation_leave(c);
	return error;
}

static int
xhci_guarded_endpoint_enable(struct drv_usb_hcd *h,
	struct drv_usb_endpoint *endpoint)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_endpoint_enable(h, endpoint);
	xhci_operation_leave(c);
	return error;
}

static int
xhci_guarded_endpoint_reset(struct drv_usb_hcd *h,
	struct drv_usb_endpoint *endpoint)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_endpoint_reset(h, endpoint);
	xhci_operation_leave(c);
	return error;
}

static int
xhci_guarded_endpoint_disable(struct drv_usb_hcd *h,
	struct drv_usb_endpoint *endpoint)
{
	struct xhci_controller *c = hcd_controller(h);
	int error;

	error = xhci_operation_enter(c);
	if (error != 0)
		return error;
	error = xhci_endpoint_disable(h, endpoint);
	xhci_operation_leave(c);
	return error;
}

static uint32_t
xhci_guarded_frame(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	uint32_t frame;

	if (xhci_operation_enter(c) != 0)
		return 0;
	frame = xhci_frame(h);
	xhci_operation_leave(c);
	return frame;
}

static int
xhci_guarded_root_status(struct drv_usb_hcd *h, void *buffer, size_t length,
	size_t *actual)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_root_status(h, buffer, length, actual);
	xhci_operation_leave(c);
	return error;
}

static int
xhci_guarded_root_control(struct drv_usb_hcd *h,
	const struct drv_usb_control_request *request, void *buffer,
	size_t length, size_t *actual)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_root_control(h, request, buffer, length, actual);
	xhci_operation_leave(c);
	return error;
}

static int
xhci_guarded_root_port_reset(struct drv_usb_hcd *h, unsigned port)
{
	struct xhci_controller *c = hcd_controller(h);
	int error = xhci_operation_enter(c);

	if (error != 0)
		return error;
	error = xhci_root_port_reset(h, port);
	xhci_operation_leave(c);
	return error;
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
    .urb_dequeue = xhci_guarded_urb_dequeue,
    .endpoint_enable = xhci_guarded_endpoint_enable,
    .endpoint_disable = xhci_guarded_endpoint_disable,
    .endpoint_reset = xhci_guarded_endpoint_reset,
    .frame_number = xhci_guarded_frame,
    .root_hub_status = xhci_guarded_root_status,
    .root_hub_control = xhci_guarded_root_control,
    .root_port_reset = xhci_guarded_root_port_reset};

static int
xhci_attach(struct drv_pci_device *d, const struct drv_pci_id *id)
{
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
	e = drv_pci_device_claim_bar(d, 0);
	if (e != 0)
		goto fail;
	c->bar_claimed = 1;
	stage = "BAR inspection";
	e = drv_pci_device_bar(d, 0, &c->original_bar);
	if (e != 0)
		goto fail;
	c->original_bar_valid = 1;
	xhci_bar_raw(d, c->original_bar.type, &original_low, &original_high);
	stage = "PCI command save";
	e = drv_pci_device_config_read16(d, XHCI_PCI_COMMAND,
	    &command_before);
	if (e != 0)
		goto fail;
	e = drv_pci_device_save_enable_state(d, &c->pci_enable_state);
	if (e != 0)
		goto fail;
	c->pci_state_saved = 1;
	stage = "BAR map";
	e = drv_pci_device_map_bar(
	    d, 0, DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &c->mapping);
	if (e != 0)
		goto fail;
	c->bar_mapped = 1;
	stage = "BAR readback";
	if (drv_pci_device_bar(d, 0, &mapped_bar) != 0 ||
	    (mapped_bar.type != DRV_PCI_BAR_MEMORY32 &&
	    mapped_bar.type != DRV_PCI_BAR_MEMORY64) ||
	    c->mapping.address == NULL || c->mapping.size == 0 ||
	    c->mapping.size > mapped_bar.size) {
		e = EIO;
		goto fail;
	}
	xhci_bar_raw(d, mapped_bar.type, &mapped_low, &mapped_high);
	hal_printf(
	    "xhci: pci %04x:%02x:%02x.%u BAR0 type=%u size=%08x:%08x original=%08x:%08x/%08x:%08x final=%08x:%08x/%08x:%08x mapped=%u\n",
	    address.segment, address.bus, address.device, address.function,
	    mapped_bar.type == DRV_PCI_BAR_MEMORY64 ? 64U : 32U,
	    (uint32_t)(mapped_bar.size >> 32), (uint32_t)mapped_bar.size,
	    (uint32_t)(c->original_bar.bus_address >> 32),
	    (uint32_t)c->original_bar.bus_address, original_high, original_low,
	    (uint32_t)(mapped_bar.bus_address >> 32),
	    (uint32_t)mapped_bar.bus_address, mapped_high, mapped_low,
	    (unsigned)c->mapping.size);
	if (!drv_xhci_bar_readback_matches(mapped_bar.bus_address,
	    mapped_bar.type == DRV_PCI_BAR_MEMORY64, mapped_low, mapped_high)) {
		e = EIO;
		goto fail;
	}
	if (drv_pci_device_config_read16(d, XHCI_PCI_COMMAND,
	    &command_mapped) != 0 ||
	    (command_mapped & 7U) != (command_before & 7U)) {
		e = EIO;
		stage = "BAR command restore";
		goto fail;
	}

	/* Mapping a BAR does not require device decode.  Capability MMIO does. */
	stage = "PCI memory enable";
	if ((e = drv_pci_device_set_bus_master(d, false)) != 0 ||
	    (e = drv_pci_device_enable_memory(d)) != 0 ||
	    (e = drv_pci_device_config_read16(d, XHCI_PCI_COMMAND,
		&command_after)) != 0)
		goto fail;
	hal_printf(
	    "xhci: pci %04x:%02x:%02x.%u command=%04x->%04x (MEM on, MASTER off)\n",
	    address.segment, address.bus, address.device, address.function,
	    command_before, command_after);
	if ((command_after & XHCI_PCI_COMMAND_MEMORY) == 0 ||
	    (command_after & XHCI_PCI_COMMAND_MASTER) != 0) {
		e = EIO;
		goto fail;
	}
	hal_io_mb();

	c->capability = c->mapping.address;
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.mapping_size = c->mapping.size;
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
	hal_printf(
	    "xhci: pci %04x:%02x:%02x.%u caps len=%02x version=%04x hcs1=%08x hcs2=%08x hcc1=%08x dboff=%08x rtsoff=%08x reject=%08x:%s\n",
	    address.segment, address.bus, address.device, address.function,
	    snapshot.capability_length, snapshot.version,
	    snapshot.structural_parameters1, snapshot.structural_parameters2,
	    snapshot.capability_parameters1, snapshot.doorbell_offset_raw,
	    snapshot.runtime_offset_raw, reasons,
	    drv_xhci_capability_reason_name(reasons));
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
	c->hcd.capabilities = DRV_USB_HCD_CAP_CONCURRENT_URBS;
	c->hcd.private_data[0] = (uintptr_t)c;
	stage = "ownership";
	if ((e = ownership(c)) != 0)
		goto fail;
	stage = "PCI bus master";
	if ((e = drv_pci_device_set_bus_master(d, true)) != 0)
		goto fail;
	stage = "IRQ allocation";
	/* One vector is sufficient; use MSI-X when present, then MSI or INTx.
	 */
	if ((e = drv_pci_device_allocate_irqs(d,
					      DRV_PCI_IRQ_ALLOW_MSIX |
						  DRV_PCI_IRQ_ALLOW_MSI |
						  DRV_PCI_IRQ_ALLOW_INTX,
					      1, 1, &c->irq, &count)) != 0)
		goto fail;
	c->irq_allocated = 1;
	stage = "IRQ establishment";
	if ((e = drv_pci_device_establish_irq(d, &c->irq, xhci_irq, c, "xhci",
					      &c->irq_cookie)) != 0)
		goto fail;
	stage = "controller start";
	if ((e = drv_usb_hcd_register(&c->hcd, &c->bus)) != 0)
		goto fail;
	c->hcd_registered = 1;
	stage = "port worker";
	if ((e = xhci_worker_start(c)) != 0) {
		int unregister_error = drv_usb_hcd_unregister(&c->hcd);

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
	hal_printf(
	    "xhci: PCI controller, version=%x ports=%u slots=%u irq=%u %s\n",
	    snapshot.version, c->ports, c->max_slots, c->irq.vector,
	    irq_type);
	return 0;
fail:
	if (c->hcd_registered || !c->dma_quiesced) {
		xhci_quarantine(c, stage, e);
		return 0;
	}
	if (c->irq_cookie != NULL) {
		cleanup_error = xhci_irq_disestablish(c);
		if (cleanup_error != 0) {
			xhci_quarantine(c, "IRQ disestablish", cleanup_error);
			return 0;
		}
	}
	if (c->irq_allocated) {
		drv_pci_device_free_irqs(d, &c->irq, 1);
		c->irq_allocated = 0;
	}
	cleanup_error = xhci_pci_release(c);
	if (cleanup_error != 0) {
		hal_printf("xhci: attach failed at %s (%d)\n", stage, e);
		xhci_quarantine(c, "PCI release", cleanup_error);
		return 0;
	}
	hal_printf("xhci: attach failed at %s (%d)\n", stage, e);
	hal_free(c);
	return e;
}
static int
xhci_detach(struct drv_pci_device *d, unsigned flags)
{
	struct xhci_controller *c = drv_pci_device_driver_data(d);
	struct xhci_controller **link;
	int error, had_worker;
	(void)flags;
	if (!c)
		return 0;
	had_worker = c->port_worker != NULL;
	if (had_worker)
		xhci_worker_stop(c);
	if (c->hcd_registered) {
		error = drv_usb_hcd_unregister(&c->hcd);
		if (error) {
			if (had_worker && error == EBUSY)
				(void)xhci_worker_start(c);
			else
				c->quarantined = 1;
			return error;
		}
		c->hcd_registered = 0;
		if (c->quarantined)
			return EBUSY;
	}
	if (!c->dma_quiesced) {
		error = xhci_stop_checked(&c->hcd);
		if (error != 0) {
			c->quarantined = 1;
			return error;
		}
	}
	if (c->irq_cookie != NULL) {
		error = xhci_irq_disestablish(c);
		if (error != 0) {
			c->quarantined = 1;
			return error;
		}
	}
	if (c->irq_allocated) {
		drv_pci_device_free_irqs(d, &c->irq, 1);
		c->irq_allocated = 0;
	}
	error = xhci_pci_release(c);
	if (error != 0) {
		c->quarantined = 1;
		return error;
	}
	drv_pci_device_set_driver_data(d, NULL);
	for (link = &controllers; *link != NULL; link = &(*link)->next)
		if (*link == c) {
			*link = c->next;
			break;
		}
	hal_free(c);
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
int
drv_pci_xhci_driver_register(void)
{
	return drv_pci_driver_register(&driver);
}
void
drv_pci_xhci_probe_roots(void)
{
	struct xhci_controller *c;
	for (c = controllers; c; c = c->next) {
		if (c->quarantined)
			continue;
		drv_usb_hcd_root_hub_changed(&c->hcd);
		c->port_pending = 0;
		c->root_ready = 1;
	}
}
