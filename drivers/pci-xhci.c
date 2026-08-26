/* Native PCI xHCI host controller. Copyright (C) 2026 Awe Morris;
 * SPDX-License-Identifier: Zlib */
#include <drivers/pci-xhci.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
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
#define XHCI_PORT_CCS 0x00000001U
#define XHCI_PORT_PED 0x00000002U
#define XHCI_PORT_PR 0x00000010U
#define XHCI_PORT_PP 0x00000200U
#define XHCI_PORT_CHANGE 0x00fe0000U
#define XHCI_TRB_CYCLE 0x00000001U
#define XHCI_TRB_CHAIN 0x00000010U
#define XHCI_TRB_IOC 0x00000020U
#define XHCI_TRB_IDT 0x00000040U
#define XHCI_TRB_TYPE(n) ((uint32_t)(n) << 10)
#define XHCI_TRB_DIR_IN 0x00010000U
#define XHCI_TRB_SLOT(n) ((uint32_t)(n) << 24)
#define XHCI_RING_TRBS 256U
#define XHCI_TIMEOUT 10000000U

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
	unsigned dci;
	int enabled;
};
struct xhci_device {
	struct drv_usb_device *usb;
	struct drv_dma_buffer output_context, input_context;
	struct xhci_endpoint endpoints[32];
	unsigned slot, context_entries;
	struct xhci_device *next;
};
struct xhci_request {
	struct drv_usb_urb *urb;
	struct drv_dma_buffer bounce;
	struct xhci_endpoint *endpoint;
	size_t length;
	unsigned first_trb;
	unsigned trb_count;
	unsigned slot;
	unsigned dci;
	int input;
};
struct xhci_controller {
	struct drv_pci_device *pci;
	struct drv_pci_mapping mapping;
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
	struct xhci_request *active;
	struct spinlock active_lock;
	struct thread *port_worker;
	volatile unsigned command_busy, event_busy;
	volatile unsigned port_pending, port_stopping, root_ready;
	struct xhci_controller *next;
};
static struct xhci_controller *controllers;

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
	for (n = 0; n < XHCI_TIMEOUT; n++)
		if ((rd32(b, o) & mask) == wanted)
			return 0;
	return ETIMEDOUT;
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

static int transfer_complete(struct xhci_controller *c,
			     const struct xhci_trb *event);

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

static int
command(struct xhci_controller *c, uint64_t parameter, uint32_t status,
	uint32_t control, unsigned *slot)
{
	struct xhci_trb event;
	unsigned n, type;
	uint32_t iman;
	bool enabled = hal_irq_disable();
	int result = ETIMEDOUT;
	while (__atomic_exchange_n(&c->command_busy, 1U, __ATOMIC_ACQUIRE)) {
		if (enabled)
			hal_irq_enable();
		sched_yield();
		enabled = hal_irq_disable();
	}
	/* Interrupter 0 and the polling path share the event-ring consumer. */
	iman = rd32(c->runtime, 0x20U);
	wr32(c->runtime, 0x20U, iman & ~2U);
	ring_push(&c->command, parameter, status, control);
	wr32(c->doorbells, 0, 0);
	for (n = 0; n < XHCI_TIMEOUT; n++) {
		int available;
		event_lock(c);
		available = event_take(c, &event);
		event_unlock(c);
		if (!available)
			continue;
		type = (event.control >> 10) & 0x3fU;
		if (type == 32U) {
			(void)transfer_complete(c, &event);
			continue;
		}
		if (type == 34U) {
			port_change_defer(c);
			continue;
		}
		if (type != 33U)
			continue;
		if (slot)
			*slot = event.control >> 24;
		result = ((event.status >> 24) & 0xffU) == 1U ? 0 : EIO;
		if (result)
			hal_printf("xhci: command %u failed, completion=%u\n",
				   (control >> 10) & 0x3fU,
				   (event.status >> 24) & 0xffU);
		break;
	}
	wr32(c->runtime, 0x20U, 2U);
	__atomic_store_n(&c->command_busy, 0U, __ATOMIC_RELEASE);
	if (enabled)
		hal_irq_enable();
	if (result == ETIMEDOUT)
		hal_printf("xhci: command %u timed out\n",
			   (control >> 10) & 0x3fU);
	return result;
}

static int
ownership(struct xhci_controller *c)
{
	uint32_t hcc = rd32(c->capability, 0x10U);
	unsigned offset = ((hcc >> 16) & 0xffffU) * 4U, guard = 0;
	while (offset && offset + 4U < c->mapping.size && guard++ < 64U) {
		uint32_t cap = rd32(c->capability, offset);
		unsigned id = cap & 0xffU;
		if (id == 1U) {
			wr32(c->capability, offset, cap | 0x01000000U);
			if (wait_bits(c->capability, offset, 0x00010000U, 0) !=
			    0)
				return ETIMEDOUT;
			return 0;
		}
		offset = ((cap >> 8) & 0xffU) * 4U;
	}
	return 0;
}

static void
fill_slot(struct xhci_controller *c, struct xhci_device *d, void *context,
	  unsigned entries)
{
	uint32_t *w = context;
	unsigned speed;
	memset(context, 0, c->context_size);
	switch (drv_usb_device_speed(d->usb)) {
	case DRV_USB_SPEED_LOW:
		speed = 2;
		break;
	case DRV_USB_SPEED_HIGH:
		speed = 3;
		break;
	case DRV_USB_SPEED_SUPER:
	case DRV_USB_SPEED_SUPER_PLUS:
		speed = 4;
		break;
	default:
		speed = 1;
		break;
	}
	w[0] = (speed << 20) | ((entries & 31U) << 27);
	w[1] = drv_usb_device_port(d->usb) << 16;
}
static void
fill_endpoint(struct xhci_controller *c, void *context,
	      struct xhci_endpoint *ep, unsigned type, unsigned packet,
	      unsigned interval)
{
	uint32_t *w = context;
	uint64_t dequeue = ep->ring.dma.device_address | 1U;
	memset(context, 0, c->context_size);
	w[0] = (interval & 0xffU) << 16;
	w[1] = (3U << 1) | (type << 3) | ((packet & 0xffffU) << 16);
	w[2] = (uint32_t)dequeue;
	w[3] = (uint32_t)(dequeue >> 32);
	w[4] = packet;
}

static unsigned
endpoint_interval(struct xhci_device *device,
		  const struct drv_usb_endpoint_descriptor *descriptor)
{
	unsigned interval, microframes;
	if ((descriptor->attributes & 3U) != DRV_USB_TRANSFER_INTERRUPT &&
	    (descriptor->attributes & 3U) != DRV_USB_TRANSFER_ISOCHRONOUS)
		return 0;
	if (drv_usb_device_speed(device->usb) >= DRV_USB_SPEED_HIGH) {
		interval = descriptor->interval ? descriptor->interval - 1U : 0;
		return interval > 15U ? 15U : interval;
	}
	microframes = (descriptor->interval ? descriptor->interval : 1U) * 8U;
	for (interval = 0; (1U << interval) < microframes && interval < 15U;
	     interval++)
		;
	return interval;
}
static struct xhci_device *
find_device(struct xhci_controller *c, struct drv_usb_device *u)
{
	struct xhci_device *d;
	for (d = c->devices; d; d = d->next)
		if (d->usb == u)
			return d;
	return NULL;
}

static int
xhci_device_enable(struct drv_usb_hcd *h, struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d;
	uint32_t *control;
	uint8_t *input;
	unsigned packet;
	int e;
	unsigned slot = 0;
	d = hal_malloc(sizeof(*d));
	if (!d)
		return ENOMEM;
	memset(d, 0, sizeof(*d));
	d->usb = u;
	if ((e = command(c, 0, 0, XHCI_TRB_TYPE(9), &slot)) != 0 || slot == 0) {
		hal_free(d);
		return e ? e : EIO;
	}
	d->slot = slot;
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
	fill_endpoint(c, input + 2U * c->context_size, &d->endpoints[1], 4,
		      packet, 0);
	e = command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(11) | (1U << 9) | XHCI_TRB_SLOT(slot), NULL);
	if (e)
		goto fail;
	d->context_entries = 1;
	d->next = c->devices;
	c->devices = d;
	return 0;
fail:
	if (d->endpoints[1].ring.dma.address)
		ring_free(c, &d->endpoints[1].ring);
	if (d->input_context.address)
		drv_dma_free_coherent(h->dma, &d->input_context);
	if (d->output_context.address)
		drv_dma_free_coherent(h->dma, &d->output_context);
	(void)command(c, 0, 0, XHCI_TRB_TYPE(10) | XHCI_TRB_SLOT(slot), NULL);
	hal_free(d);
	return e;
}

static int
xhci_set_address(struct drv_usb_hcd *h, struct drv_usb_device *u,
		 unsigned address)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = find_device(c, u);
	(void)address;
	if (!d)
		return ENODEV;
	return command(c, d->input_context.device_address, 0,
		       XHCI_TRB_TYPE(11) | XHCI_TRB_SLOT(d->slot), NULL);
}

static void
xhci_device_disable(struct drv_usb_hcd *h, struct drv_usb_device *u)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d, **link;
	unsigned i;
	for (link = &c->devices; (d = *link) != NULL; link = &d->next)
		if (d->usb == u) {
			*link = d->next;
			(void)command(
			    c, 0, 0, XHCI_TRB_TYPE(10) | XHCI_TRB_SLOT(d->slot),
			    NULL);
			for (i = 1; i < 32; i++)
				ring_free(c, &d->endpoints[i].ring);
			drv_dma_free_coherent(h->dma, &d->input_context);
			drv_dma_free_coherent(h->dma, &d->output_context);
			((uint64_t *)c->dcbaa.address)[d->slot] = 0;
			hal_free(d);
			return;
		}
}

static int
xhci_endpoint_enable(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = find_device(c, drv_usb_endpoint_device(usbep));
	struct xhci_endpoint *ep;
	const struct drv_usb_endpoint_descriptor *desc;
	uint8_t *input;
	uint32_t *control;
	unsigned number, dci, type, packet, interval;
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
	if ((e = ring_alloc(c, &ep->ring)) != 0)
		return e;
	ep->dci = dci;
	input = d->input_context.address;
	memset(input, 0, 4096U);
	control = (uint32_t *)input;
	control[1] = 1U | (1U << dci);
	if (dci > d->context_entries)
		d->context_entries = dci;
	fill_slot(c, d, input + c->context_size, d->context_entries);
	packet = desc->maximum_packet_size & 0x7ffU;
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
	interval = endpoint_interval(d, desc);
	fill_endpoint(c, input + (dci + 1U) * c->context_size, ep, type, packet,
		      interval);
	e = command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(12) | XHCI_TRB_SLOT(d->slot), NULL);
	if (e) {
		ring_free(c, &ep->ring);
		return e;
	}
	ep->enabled = 1;
	return 0;
}
static void
xhci_endpoint_disable(struct drv_usb_hcd *h, struct drv_usb_endpoint *usbep)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = find_device(c, drv_usb_endpoint_device(usbep));
	struct xhci_endpoint *ep;
	uint8_t *input;
	uint32_t *control;
	unsigned dci, entries;
	if (!d)
		return;
	dci = (drv_usb_endpoint_address(usbep) & 15U) * 2U +
	      (drv_usb_endpoint_is_input(usbep) ? 1U : 0U);
	if (dci < 2U || dci >= 32U || !d->endpoints[dci].enabled)
		return;
	ep = &d->endpoints[dci];
	for (entries = 31U; entries > 1U; entries--)
		if (entries != dci && d->endpoints[entries].enabled)
			break;
	input = d->input_context.address;
	memset(input, 0, 4096U);
	control = (uint32_t *)input;
	control[0] = 1U << dci;
	control[1] = 1U;
	fill_slot(c, d, input + c->context_size, entries);
	if (command(c, d->input_context.device_address, 0,
		    XHCI_TRB_TYPE(12) | XHCI_TRB_SLOT(d->slot), NULL) != 0)
		return;
	d->context_entries = entries;
	ep->enabled = 0;
	ring_free(c, &ep->ring);
}

static int
transfer_complete(struct xhci_controller *c, const struct xhci_trb *event)
{
	struct xhci_request *r;
	enum drv_usb_urb_status status;
	unsigned long irq;
	uint64_t pointer = (uint64_t)event->parameter_low |
	    ((uint64_t)event->parameter_high << 32);
	unsigned event_slot = event->control >> 24;
	unsigned event_dci = (event->control >> 16) & 31U;
	unsigned code = (event->status >> 24) & 0xffU;
	size_t residual = event->status & 0x00ffffffU, actual;
	unsigned index, current, n;
	irq = spin_lock_irqsave(&c->active_lock);
	r = c->active;
	if (r == NULL) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return 0;
	}
	if (pointer < r->endpoint->ring.dma.device_address ||
	    pointer >= r->endpoint->ring.dma.device_address + 4096U ||
	    (pointer & (sizeof(struct xhci_trb) - 1U)) != 0) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return 0;
	}
	index = (unsigned)((pointer - r->endpoint->ring.dma.device_address) /
	    sizeof(struct xhci_trb));
	current = r->first_trb;
	for (n = 0; n < r->trb_count && current != index; n++) {
		current++;
		if (current == XHCI_RING_TRBS - 1U)
			current = 0;
	}
	if (n == r->trb_count || event_slot != r->slot ||
	    event_dci != r->dci) {
		spin_unlock_irqrestore(&c->active_lock, irq);
		return 0;
	}
	c->active = NULL;
	spin_unlock_irqrestore(&c->active_lock, irq);
	actual = residual < r->length ? r->length - residual : 0;
	status = (code == 1U || (code == 13U && r->input)) ?
		     DRV_USB_URB_COMPLETE
		 : code == 6U		     ? DRV_USB_URB_STALL
					     : DRV_USB_URB_IO_ERROR;
	if (status != DRV_USB_URB_COMPLETE || residual != 0)
		hal_printf("xhci: transfer completion=%u residual=%u length=%u "
		    "slot=%u endpoint=%u direction=%s\n", code,
		    (unsigned)residual, (unsigned)r->length, r->slot, r->dci,
		    r->input ? "in" : "out");
	if (r->input && actual)
		memcpy(drv_usb_urb_buffer(r->urb), r->bounce.address, actual);
	drv_usb_urb_set_hcd_data(r->urb, NULL);
	drv_dma_free_coherent(c->hcd.dma, &r->bounce);
	{
		struct drv_usb_urb *u = r->urb;
		hal_free(r);
		drv_usb_hcd_complete(&c->hcd, u, status, actual);
	}
	return 1;
}
static int
xhci_irq(void *argument)
{
	struct xhci_controller *c = argument;
	struct xhci_trb event;
	int handled = 0;
	uint32_t status = rd32(c->operational, XHCI_USBSTS);
	if (!(status & (XHCI_STS_EINT | XHCI_STS_FATAL)))
		return 0;
	wr32(c->operational, XHCI_USBSTS, status);
	wr32(c->runtime, 0x20U, rd32(c->runtime, 0x20U) | 1U);
	event_lock(c);
	while (event_take(c, &event)) {
		unsigned type = (event.control >> 10) & 0x3fU;
		handled = 1;
		if (type == 32U)
			(void)transfer_complete(c, &event);
		else if (type == 34U)
			port_change_defer(c);
	}
	event_unlock(c);
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
	       int input)
{
	unsigned count = normal_trb_count(address, length);
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
		if (count != 0)
			control |= XHCI_TRB_CHAIN;
		else
			control |= XHCI_TRB_IOC;
		final_trb = ring_push(ring, address,
		    (uint32_t)chunk | ((count > 31U ? 31U : count) << 17),
		    control);
		address += chunk;
		length -= chunk;
	}
	(void)input;
	return final_trb;
}

static int
xhci_urb_enqueue(struct drv_usb_hcd *h, struct drv_usb_urb *u)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = find_device(c, drv_usb_urb_device(u));
	struct xhci_endpoint *ep;
	struct xhci_request *r;
	const struct drv_usb_control_request *q =
	    drv_usb_urb_control_request(u);
	size_t length = drv_usb_urb_length(u);
	uint64_t dma;
	unsigned dci;
	int e, input;
	if (!d)
		return ENODEV;
	dci = q ? 1U
		: ((drv_usb_endpoint_address(drv_usb_urb_endpoint(u)) & 15U) *
		       2U +
		   (drv_usb_endpoint_is_input(drv_usb_urb_endpoint(u)) ? 1U
								       : 0U));
	ep = &d->endpoints[dci];
	if (!ep->enabled)
		return ENODEV;
	r = hal_malloc(sizeof(*r));
	if (!r)
		return ENOMEM;
	memset(r, 0, sizeof(*r));
	r->urb = u;
	r->endpoint = ep;
	r->length = length;
	r->input = q ? (q->request_type & DRV_USB_DIR_IN) != 0
		     : drv_usb_endpoint_is_input(drv_usb_urb_endpoint(u));
	if ((e = drv_dma_alloc_coherent(h->dma, length ? length : 8U, 64U,
					&r->bounce)) != 0) {
		hal_free(r);
		return e;
	}
	if (!r->input && length)
		memcpy(r->bounce.address, drv_usb_urb_buffer(u), length);
	dma = r->bounce.device_address;
	if ((!q && normal_trb_count(dma, length) >= XHCI_RING_TRBS - 1U) ||
	    (q && length != 0 &&
	     (length > 0x10000U || (dma & 0xffffU) + length > 0x10000U))) {
		drv_dma_free_coherent(h->dma, &r->bounce);
		hal_free(r);
		return EOVERFLOW;
	}
	input = r->input;
	r->first_trb = ep->ring.enqueue;
	r->slot = d->slot;
	r->dci = dci;
	{
		unsigned long irq = spin_lock_irqsave(&c->active_lock);
		if (c->active != NULL) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			drv_dma_free_coherent(h->dma, &r->bounce);
			hal_free(r);
			return EBUSY;
		}
		c->active = r;
		spin_unlock_irqrestore(&c->active_lock, irq);
	}
	drv_usb_urb_set_hcd_data(u, r);
	if (q) {
		uint64_t setup = 0;
		r->trb_count = length ? 3U : 2U;
		memcpy(&setup, q, sizeof(*q));
		ring_push(&ep->ring, setup, 8U | ((length ? 2U : 1U) << 17),
			  XHCI_TRB_TYPE(2) | XHCI_TRB_IDT | XHCI_TRB_CHAIN |
			      ((length ? (input ? 3U : 2U) : 0U) << 16));
		if (length)
			ring_push(&ep->ring, dma, (uint32_t)length | (1U << 17),
				  XHCI_TRB_TYPE(3) | XHCI_TRB_CHAIN |
				      (input ? XHCI_TRB_DIR_IN : 0));
		(void)ring_push(&ep->ring, 0, 0,
			  XHCI_TRB_TYPE(4) | XHCI_TRB_IOC |
			      (input ? 0 : XHCI_TRB_DIR_IN));
	} else {
		r->trb_count = normal_trb_count(dma, length);
		(void)enqueue_normal(&ep->ring, dma, length, input);
	}
	wr32(c->doorbells, d->slot * 4U, dci);
	return 0;
}
static int
xhci_urb_dequeue(struct drv_usb_hcd *h, struct drv_usb_urb *u)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_device *d = find_device(c, drv_usb_urb_device(u));
	struct xhci_request *r;
	struct xhci_endpoint *ep;
	uint64_t dequeue;
	unsigned dci;
	int error;
	if (!d)
		return EINVAL;
	/* Hide the request before Stop Endpoint so a late transfer event cannot
	 * complete an URB which the caller is cancelling.  The bounce buffer
	 * stays owned until the controller confirms that it has stopped
	 * fetching. */
	{
		unsigned long irq = spin_lock_irqsave(&c->active_lock);
		r = c->active;
		if (r == NULL || r->urb != u) {
			spin_unlock_irqrestore(&c->active_lock, irq);
			return EBUSY;
		}
		c->active = NULL;
		spin_unlock_irqrestore(&c->active_lock, irq);
	}
	dci = r->endpoint->dci;
	ep = &d->endpoints[dci];
	drv_usb_urb_set_hcd_data(u, NULL);
	error = command(
	    c, 0, 0, XHCI_TRB_TYPE(15) | (dci << 16) | XHCI_TRB_SLOT(d->slot),
	    NULL);
	if (error != 0) {
		hal_printf("xhci: endpoint %u stop failed during cancel (%d); "
			   "retaining DMA buffer\n",
			   dci, error);
		return error;
	}
	dequeue = ep->ring.dma.device_address +
		  (uint64_t)ep->ring.enqueue * sizeof(struct xhci_trb);
	dequeue |= ep->ring.cycle ? 1U : 0U;
	error = command(
	    c, dequeue, 0,
	    XHCI_TRB_TYPE(16) | (dci << 16) | XHCI_TRB_SLOT(d->slot), NULL);
	drv_dma_free_coherent(h->dma, &r->bounce);
	hal_free(r);
	return error;
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
		if (s & (1U << 21))
			v |= 0x100000U;
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
		else if (r->value == 20)
			change = 1U << 21;
		else if (r->value != 4)
			return ENOTSUP;
		wr32(c->operational, XHCI_PORTSC(p),
		     (s & XHCI_PORT_PP) | change);
		if (a)
			*a = 0;
		return 0;
	}
	if (r->request == 3 && r->value == 1) {
		wr32(c->operational, XHCI_PORTSC(p), XHCI_PORT_PP);
		if (a)
			*a = 0;
		return 0;
	}
	return ENOTSUP;
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

static void
xhci_stop(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	wr32(c->operational, XHCI_USBCMD, 0);
	wr32(c->runtime, 0x20U, 0);
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
}

static int
xhci_start(struct drv_usb_hcd *h)
{
	struct xhci_controller *c = hcd_controller(h);
	struct xhci_erst *erst;
	int e;
	wr32(c->operational, XHCI_USBCMD,
	     rd32(c->operational, XHCI_USBCMD) & ~XHCI_CMD_RUN);
	if ((e = wait_bits(c->operational, XHCI_USBSTS, XHCI_STS_HALTED,
			   XHCI_STS_HALTED)) != 0)
		return e;
	wr32(c->operational, XHCI_USBCMD, XHCI_CMD_RESET);
	if ((e = wait_bits(c->operational, XHCI_USBCMD, XHCI_CMD_RESET, 0)) !=
	    0)
		return e;
	if ((rd32(c->operational, XHCI_PAGESIZE) & 1U) == 0)
		return ENOTSUP;
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
	memset(c->event_memory.address, 0, 4096U);
	memset(c->erst_memory.address, 0, 4096U);
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
	xhci_stop(h);
	return e;
}
static const struct drv_usb_hcd_ops xhci_ops = {
    .start = xhci_start,
    .stop = xhci_stop,
    .device_enable = xhci_device_enable,
    .device_set_address = xhci_set_address,
    .device_disable = xhci_device_disable,
    .urb_enqueue = xhci_urb_enqueue,
    .urb_dequeue = xhci_urb_dequeue,
    .endpoint_enable = xhci_endpoint_enable,
    .endpoint_disable = xhci_endpoint_disable,
    .frame_number = xhci_frame,
    .root_hub_status = xhci_root_status,
    .root_hub_control = xhci_root_control};

static int
xhci_attach(struct drv_pci_device *d, const struct drv_pci_id *id)
{
	struct xhci_controller *c;
	const char *stage = "allocation";
	unsigned count = 0;
	uint32_t hcs, hcs2, hcc, doorbell_offset, runtime_offset;
	unsigned capability_length, version;
	const char *irq_type;
	int e;
	(void)id;
	c = hal_malloc(sizeof(*c));
	if (!c)
		return ENOMEM;
	memset(c, 0, sizeof(*c));
	spin_init(&c->active_lock, LOCK_RANK_DEVICE, "xHCI active request");
	c->pci = d;
	stage = "BAR claim";
	e = drv_pci_device_claim_bar(d, 0);
	if (e != 0)
		goto fail_unmapped;
	stage = "BAR map";
	e = drv_pci_device_map_bar(
	    d, 0, DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &c->mapping);
	if (e != 0)
		goto fail_claimed;
	c->capability = c->mapping.address;
	capability_length = rd8(c->capability, 0);
	version = rd32(c->capability, 0) >> 16;
	hcs = rd32(c->capability, 4);
	hcs2 = rd32(c->capability, 8);
	hcc = rd32(c->capability, 0x10U);
	doorbell_offset = rd32(c->capability, 0x14U) & ~3U;
	runtime_offset = rd32(c->capability, 0x18U) & ~31U;
	c->max_slots = hcs & 0xffU;
	c->ports = (hcs >> 24) & 0xffU;
	c->context_size = (hcc & (1U << 2)) ? 64U : 32U;
	c->scratchpad_count =
	    (((hcs2 >> 27) & 31U) << 5) | ((hcs2 >> 21) & 31U);
	if (!c->max_slots || !c->ports || capability_length < 0x20U ||
	    (version != 0x100U && version != 0x110U) ||
	    capability_length + XHCI_PORTSC(c->ports - 1U) + 4U >
		c->mapping.size ||
	    runtime_offset > c->mapping.size ||
	    0x40U > c->mapping.size - runtime_offset ||
	    doorbell_offset > c->mapping.size ||
	    (size_t)(c->max_slots + 1U) * 4U >
		c->mapping.size - doorbell_offset) {
		e = ENODEV;
		stage = "capabilities";
		goto fail;
	}
	c->operational = c->capability + capability_length;
	c->runtime = c->capability + runtime_offset;
	c->doorbells = c->capability + doorbell_offset;
	c->hcd.name = "xHCI";
	c->hcd.ops = &xhci_ops;
	c->hcd.dma = drv_pci_device_dma(d);
	c->hcd.root_port_count = c->ports;
	c->hcd.private_data[0] = (uintptr_t)c;
	stage = "ownership";
	if ((e = ownership(c)) != 0)
		goto fail;
	stage = "PCI enable";
	if ((e = drv_pci_device_enable_memory(d)) != 0 ||
	    (e = drv_pci_device_set_bus_master(d, true)) != 0)
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
	stage = "IRQ establishment";
	if ((e = drv_pci_device_establish_irq(d, &c->irq, xhci_irq, c, "xhci",
					      &c->irq_cookie)) != 0)
		goto fail;
	stage = "controller start";
	if ((e = drv_usb_hcd_register(&c->hcd, &c->bus)) != 0)
		goto fail;
	stage = "port worker";
	if ((e = xhci_worker_start(c)) != 0) {
		(void)drv_usb_hcd_unregister(&c->hcd);
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
	    rd32(c->capability, 0) >> 16, c->ports, c->max_slots, c->irq.vector,
	    irq_type);
	return 0;
fail:
	if (c->irq_cookie)
		drv_pci_device_disestablish_irq(d, c->irq_cookie);
	if (count)
		drv_pci_device_free_irqs(d, &c->irq, 1);
	(void)drv_pci_device_set_bus_master(d, false);
	drv_pci_device_unmap_bar(d, &c->mapping);
fail_claimed:
	drv_pci_device_release_bar(d, 0);
fail_unmapped:
	hal_printf("xhci: attach failed at %s (%d)\n", stage, e);
	hal_free(c);
	return e;
}
static int
xhci_detach(struct drv_pci_device *d, unsigned flags)
{
	struct xhci_controller *c = drv_pci_device_driver_data(d);
	struct xhci_controller **link;
	int error;
	(void)flags;
	if (!c)
		return 0;
	xhci_worker_stop(c);
	error = drv_usb_hcd_unregister(&c->hcd);
	if (error) {
		(void)xhci_worker_start(c);
		return error;
	}
	if (c->irq_cookie)
		drv_pci_device_disestablish_irq(d, c->irq_cookie);
	drv_pci_device_free_irqs(d, &c->irq, 1);
	(void)drv_pci_device_set_bus_master(d, false);
	drv_pci_device_unmap_bar(d, &c->mapping);
	drv_pci_device_release_bar(d, 0);
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
		drv_usb_hcd_root_hub_changed(&c->hcd);
		c->port_pending = 0;
		c->root_ready = 1;
	}
}
