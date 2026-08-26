/*
 * zedBSD USB host core
 * Copyright (C) 2026 Awe Morris
 *
 * The driver model and URB terminology follow the Linux USB API.  This is
 * an independent implementation and contains no Linux implementation code.
 *
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/sched.h>
#include <string.h>

#define USB_REQ_GET_DESCRIPTOR 6U
#define USB_REQ_SET_ADDRESS 5U
#define USB_REQ_SET_CONFIGURATION 9U
#define USB_ADDRESS_RECOVERY_TICKS 2U
#define USB_RESET_RECOVERY_TICKS 2U

#define USB_DEVICE_LIFECYCLE_DISCONNECTING (1U << 31)
#define USB_DEVICE_LIFECYCLE_FINALIZING (1U << 30)
#define USB_DEVICE_LIFECYCLE_URB_MASK ((1U << 30) - 1U)

struct drv_usb_configuration {
	struct drv_usb_configuration_descriptor descriptor;
};

struct drv_usb_endpoint {
	struct drv_usb_interface *interface;
	struct drv_usb_endpoint_descriptor descriptor;
	struct drv_usb_superspeed_endpoint_companion_descriptor companion;
	enum drv_usb_transfer_type type;
	unsigned companion_valid;
	uintptr_t hcd_private[4];
};

struct drv_usb_interface {
	struct drv_usb_device *device;
	struct drv_usb_interface_descriptor descriptor;
	struct drv_usb_endpoint *endpoints;
	unsigned endpoint_count;
	unsigned alternate_count;
	struct drv_usb_driver *driver;
	void *driver_data;
	struct drv_usb_interface *next;
};

struct drv_usb_device {
	struct drv_usb_bus *bus;
	struct drv_usb_device *parent, *next;
	struct drv_usb_interface *interfaces;
	struct drv_usb_configuration *configurations, *active_configuration;
	struct drv_usb_device_descriptor descriptor;
	unsigned configuration_count, address, port;
	enum drv_usb_speed speed;
	enum drv_usb_device_state state;
	unsigned lifecycle;
	unsigned hcd_urb_count;
	unsigned quarantined;
	unsigned report_disconnect;
	void *quarantine_buffer;
	uintptr_t hcd_private[4];
	struct drv_usb_endpoint endpoint0;
};

struct drv_usb_bus {
	struct usb_port_state {
		unsigned observed;
		unsigned connected;
	} *ports;
	unsigned number;
	unsigned stopping;
	struct drv_usb_hcd *hcd;
	struct drv_usb_device *root_hub, *devices;
	struct drv_usb_bus *next;
	uint8_t address_used[DRV_USB_MAX_ADDRESS + 1U];
};

struct drv_usb_urb {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	void *buffer;
	size_t length, actual_length;
	unsigned flags, timeout_ms;
	drv_usb_urb_callback_t callback;
	void *callback_argument;
	struct drv_usb_control_request control;
	struct drv_usb_iso_packet *iso_packets;
	unsigned iso_packet_count;
	enum drv_usb_urb_status status;
	unsigned terminal_claimed;
	unsigned hcd_owned;
	refcount_t references;
	uintptr_t hcd_private[6];
};

struct usb_driver_entry {
	struct drv_usb_driver *driver;
	struct usb_driver_entry *next;
};

static struct drv_usb_bus *usb_buses;
static struct usb_driver_entry *usb_drivers;
static unsigned next_bus_number;
static bool usb_initialized;
static atomic_uint_t usb_topology_gate;

static void device_urb_put(struct drv_usb_device *device);

static void
usb_topology_lock(void)
{
	while (!atomic_try_acquire_zero(&usb_topology_gate))
		sched_yield();
}

static void
usb_topology_unlock(void)
{
	atomic_store_release(&usb_topology_gate, 0U);
}

static int
device_urb_get(struct drv_usb_device *device)
{
	unsigned state;

	state = hal_atomic_load_acquire(&device->lifecycle);
	for (;;) {
		if ((state & (USB_DEVICE_LIFECYCLE_DISCONNECTING |
		    USB_DEVICE_LIFECYCLE_FINALIZING)) != 0 ||
		    (state & USB_DEVICE_LIFECYCLE_URB_MASK) ==
		    USB_DEVICE_LIFECYCLE_URB_MASK)
			return 0;
		if (hal_atomic_compare_exchange_acq_rel(&device->lifecycle,
		    &state, state + 1U))
			return 1;
	}
}

static void
device_begin_disconnect(struct drv_usb_device *device)
{
	(void)hal_atomic_fetch_or_release(&device->lifecycle,
	    USB_DEVICE_LIFECYCLE_DISCONNECTING);
}

static int
device_is_disconnecting(const struct drv_usb_device *device)
{
	return (hal_atomic_load_acquire(&device->lifecycle) &
	    USB_DEVICE_LIFECYCLE_DISCONNECTING) != 0;
}

static struct drv_usb_device *allocate_root_hub(struct drv_usb_bus *bus)
{
	struct drv_usb_device *device = hal_malloc(sizeof(*device));
	if (device == NULL) return NULL;
	memset(device, 0, sizeof(*device));
	device->bus = bus;
	device->speed = DRV_USB_SPEED_FULL;
	device->state = DRV_USB_STATE_CONFIGURED;
	device->descriptor.length = sizeof(device->descriptor);
	device->descriptor.descriptor_type = DRV_USB_DESCRIPTOR_DEVICE;
	device->descriptor.device_class = 9U;
	device->descriptor.endpoint0_max_packet_size = 64U;
	device->endpoint0.type = DRV_USB_TRANSFER_CONTROL;
	device->endpoint0.descriptor.address = 0;
	device->endpoint0.descriptor.maximum_packet_size = 64U;
	return device;
}

int drv_usb_init(void)
{
	if (usb_initialized) return EALREADY;
	usb_buses = NULL; usb_drivers = NULL; next_bus_number = 0;
	atomic_store_release(&usb_topology_gate, 0U);
	usb_initialized = true;
	hal_printf("usb: URB completion contract q009-release-acquire-v1\n");
	return 0;
}

void drv_usb_shutdown(void)
{
	struct drv_usb_bus *bus;
	for (bus = usb_buses; bus != NULL; bus = bus->next) {
		hal_atomic_store_release(&bus->stopping, 1U);
		if (bus->hcd->ops->quiesce != NULL &&
		    bus->hcd->ops->quiesce(bus->hcd) != 0) {
			hal_printf("usb%u: host controller stop failed\n",
			    bus->number);
			continue;
		}
		if (bus->hcd->ops->stop != NULL)
			bus->hcd->ops->stop(bus->hcd);
	}
}

int drv_usb_hcd_register(struct drv_usb_hcd *hcd, struct drv_usb_bus **result)
{
	struct drv_usb_bus *bus;
	int error;
	if (!usb_initialized || hcd == NULL || hcd->name == NULL ||
	    hcd->ops == NULL || hcd->ops->start == NULL ||
	    hcd->ops->stop == NULL || hcd->ops->urb_enqueue == NULL ||
	    hcd->ops->urb_dequeue == NULL || result == NULL)
		return EINVAL;
	bus = hal_malloc(sizeof(*bus)); if (bus == NULL) return ENOMEM;
	memset(bus, 0, sizeof(*bus)); bus->number = next_bus_number++;
	bus->hcd = hcd; bus->address_used[0] = 1;
	if (hcd->root_port_count == UINT_MAX) {
		hal_free(bus);
		return EOVERFLOW;
	}
	bus->ports = hal_malloc(((size_t)hcd->root_port_count + 1U) *
	    sizeof(*bus->ports));
	if (bus->ports == NULL) {
		hal_free(bus);
		return ENOMEM;
	}
	memset(bus->ports, 0, ((size_t)hcd->root_port_count + 1U) *
	    sizeof(*bus->ports));
	bus->root_hub = allocate_root_hub(bus);
	if (bus->root_hub == NULL) {
		hal_free(bus->ports);
		hal_free(bus);
		return ENOMEM;
	}
	error = hcd->ops->start(hcd);
	if (error != 0) {
		hal_free(bus->root_hub);
		hal_free(bus->ports);
		hal_free(bus);
		return error;
	}
	usb_topology_lock();
	bus->next = usb_buses;
	usb_buses = bus;
	usb_topology_unlock();
	*result = bus;
	hal_printf("usb%u: %s, %u root ports\n", bus->number, hcd->name,
	    hcd->root_port_count);
	return 0;
}

int drv_usb_hcd_unregister(struct drv_usb_hcd *hcd)
{
	struct drv_usb_bus **link, *bus;
	unsigned state;
	int error;
	if (hcd == NULL) return EINVAL;
	usb_topology_lock();
	for (link = &usb_buses; (bus = *link) != NULL; link = &bus->next) {
		if (bus->hcd != hcd) continue;
		if (bus->devices != NULL) {
			usb_topology_unlock();
			return EBUSY;
		}
		hal_atomic_store_release(&bus->stopping, 1U);
		device_begin_disconnect(bus->root_hub);
		if (hcd->ops->quiesce != NULL) {
			error = hcd->ops->quiesce(hcd);
			if (error != 0) {
				usb_topology_unlock();
				return error;
			}
		}
		state = hal_atomic_load_acquire(&bus->root_hub->lifecycle);
		for (;;) {
			if ((state & USB_DEVICE_LIFECYCLE_URB_MASK) != 0) {
				usb_topology_unlock();
				return EBUSY;
			}
			if ((state & USB_DEVICE_LIFECYCLE_FINALIZING) != 0) {
				usb_topology_unlock();
				return EALREADY;
			}
			if (hal_atomic_compare_exchange_acq_rel(
			    &bus->root_hub->lifecycle, &state,
			    state | USB_DEVICE_LIFECYCLE_FINALIZING))
				break;
		}
		hcd->ops->stop(hcd);
		*link = bus->next;
		hal_free(bus->root_hub);
		hal_free(bus->ports);
		hal_free(bus);
		usb_topology_unlock();
		return 0;
	}
	usb_topology_unlock();
	return ENOENT;
}

static struct drv_usb_bus *find_hcd_bus(struct drv_usb_hcd *hcd)
{ struct drv_usb_bus*b;for(b=usb_buses;b;b=b->next)if(b->hcd==hcd)return b;return NULL; }

static struct drv_usb_device *find_port_device(struct drv_usb_bus *bus,
	unsigned port)
{ struct drv_usb_device*d;for(d=bus->devices;d;d=d->next)if(d->parent==bus->root_hub&&d->port==port)return d;return NULL; }

static void
free_interfaces(struct drv_usb_device *device, int disable)
{
	struct drv_usb_interface *interface, *next;
	for (interface = device->interfaces; interface != NULL; interface = next) {
		unsigned index;
		next = interface->next;
		if (disable && device->bus->hcd->ops->endpoint_disable != NULL)
			for (index = 0; index < interface->endpoint_count; index++)
				device->bus->hcd->ops->endpoint_disable(
				    device->bus->hcd, &interface->endpoints[index]);
		if (interface->endpoints != NULL)
			hal_free(interface->endpoints);
		hal_free(interface);
	}
	device->interfaces = NULL;
}

static int
detach_interfaces(struct drv_usb_device *device)
{
	struct drv_usb_interface *interface;
	int error;

	for (interface = device->interfaces; interface != NULL;
	    interface = interface->next) {
		if (interface->driver == NULL)
			continue;
		error = drv_usb_interface_detach(interface,
		    DRV_USB_DETACH_FORCE | DRV_USB_DETACH_QUIET);
		if (error != 0)
			return error;
	}
	return 0;
}

static int
device_linked(struct drv_usb_bus *bus, struct drv_usb_device *device)
{
	struct drv_usb_device *current;

	for (current = bus->devices; current != NULL; current = current->next)
		if (current == device)
			return 1;
	return 0;
}

static void
device_link(struct drv_usb_bus *bus, struct drv_usb_device *device)
{
	if (device_linked(bus, device))
		return;
	device->next = bus->devices;
	bus->devices = device;
}

static int
device_quiesce(struct drv_usb_bus *bus, struct drv_usb_device *device)
{
	int error;

	if (bus->hcd->ops->device_quiesce == NULL)
		return 0;
	error = bus->hcd->ops->device_quiesce(bus->hcd, device);
	if (error == 0 && drv_usb_device_hcd_urb_count(device) != 0)
		error = EBUSY;
	if (error == 0) {
		device->quarantined = 0;
		return 0;
	}
	if (!device->quarantined)
		hal_printf(
		    "usb%u: device %u port %u teardown failed (%d); device and DMA retained\n",
		    bus->number, device->address, device->port, error);
	device->quarantined = 1;
	device_link(bus, device);
	return error;
}

static void
device_finalize(struct drv_usb_bus *bus, struct drv_usb_device *device)
{
	struct drv_usb_device **link;
	unsigned address = device->address;
	unsigned port = device->port;
	unsigned report_disconnect = device->report_disconnect;

	for (link = &bus->devices; *link != NULL; link = &(*link)->next)
		if (*link == device) {
			*link = device->next;
			break;
		}
	/* A checked device quiesce and successful driver detach have already
	 * stopped every user and HCD path which can reach these interfaces. */
	free_interfaces(device, bus->hcd->ops->device_quiesce == NULL);
	if (bus->hcd->ops->device_disable != NULL)
		bus->hcd->ops->device_disable(bus->hcd, device);
	if (address > 0 && address <= DRV_USB_MAX_ADDRESS)
		bus->address_used[address] = 0;
	if (device->configurations != NULL)
		hal_free(device->configurations);
	if (device->quarantine_buffer != NULL)
		hal_free(device->quarantine_buffer);
	hal_free(device);
	if (report_disconnect)
		hal_printf("usb%u: device %u port %u disconnected\n",
		    bus->number, address, port);
}

static int
device_release(struct drv_usb_bus *bus, struct drv_usb_device *device)
{
	unsigned state;

	device_begin_disconnect(device);
	state = hal_atomic_load_acquire(&device->lifecycle);
	for (;;) {
		if ((state & USB_DEVICE_LIFECYCLE_URB_MASK) != 0) {
			if (!device->quarantined)
				hal_printf(
				    "usb%u: device %u port %u release waiting for %u URB reference(s); device retained\n",
				    bus->number, device->address, device->port,
				    state & USB_DEVICE_LIFECYCLE_URB_MASK);
			device->quarantined = 1;
			device_link(bus, device);
			return EBUSY;
		}
		if ((state & USB_DEVICE_LIFECYCLE_FINALIZING) != 0)
			return EALREADY;
		if (hal_atomic_compare_exchange_acq_rel(&device->lifecycle,
		    &state, state | USB_DEVICE_LIFECYCLE_FINALIZING))
			break;
	}
	device_finalize(bus, device);
	return 0;
}

static void
device_urb_put(struct drv_usb_device *device)
{
	unsigned previous;

	previous = hal_atomic_fetch_add_release(&device->lifecycle,
	    (unsigned)-1);
	if ((previous & USB_DEVICE_LIFECYCLE_URB_MASK) == 0)
		__builtin_trap();
	if ((previous & USB_DEVICE_LIFECYCLE_URB_MASK) == 1U)
		hal_atomic_fence_acquire();
}

static int
destroy_device(struct drv_usb_bus *bus, struct drv_usb_device *device)
{
	int error;

	if (device == NULL || device == bus->root_hub)
		return EINVAL;
	device_begin_disconnect(device);
	error = device_quiesce(bus, device);
	if (error != 0)
		return error;
	error = detach_interfaces(device);
	if (error != 0) {
		if (!device->quarantined)
			hal_printf(
			    "usb%u: device %u port %u driver detach pending (%d); device retained\n",
			    bus->number, device->address, device->port, error);
		device->quarantined = 1;
		device_link(bus, device);
		return error;
	}
	device->report_disconnect = 1U;
	return device_release(bus, device);
}

static int allocate_address(struct drv_usb_bus *bus)
{ unsigned n;for(n=1;n<=DRV_USB_MAX_ADDRESS;n++)if(!bus->address_used[n]){bus->address_used[n]=1;return (int)n;}return -1; }

static int
ep0_packet_size(enum drv_usb_speed speed, uint8_t encoded, unsigned *packet)
{
	unsigned decoded = 0;

	switch (speed) {
	case DRV_USB_SPEED_LOW:
		decoded = encoded == 8U ? 8U : 0;
		break;
	case DRV_USB_SPEED_FULL:
		if (encoded == 8U || encoded == 16U || encoded == 32U ||
		    encoded == 64U)
			decoded = encoded;
		break;
	case DRV_USB_SPEED_HIGH:
		decoded = encoded == 64U ? 64U : 0;
		break;
	case DRV_USB_SPEED_SUPER:
	case DRV_USB_SPEED_SUPER_PLUS:
		decoded = encoded == 9U ? 512U : 0;
		break;
	default:
		break;
	}
	if (packet != NULL)
		*packet = decoded;
	return decoded != 0;
}

static void usb_delay_ticks(uint64_t count)
{
	uint64_t deadline=sched_ticks()+count;
	while(sched_ticks()<deadline)hal_compiler_barrier();
}

int
drv_usb_decode_superspeed_endpoint_companion(const void *raw, size_t length,
	struct drv_usb_superspeed_endpoint_companion_descriptor *result)
{
	struct drv_usb_superspeed_endpoint_companion_descriptor descriptor;

	if (raw == NULL || result == NULL || length < sizeof(descriptor))
		return EINVAL;
	memcpy(&descriptor, raw, sizeof(descriptor));
	if (descriptor.length != sizeof(descriptor) ||
	    descriptor.descriptor_type !=
		DRV_USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION)
		return EINVAL;
	*result = descriptor;
	return 0;
}

static int parse_configuration(struct drv_usb_device *device,
	const uint8_t *raw,size_t length)
{
	struct drv_usb_interface *current=NULL,**tail=&device->interfaces;
	struct drv_usb_endpoint *current_endpoint=NULL;
	size_t offset=0;
	while(offset+2U<=length){uint8_t n=raw[offset],type=raw[offset+1U];
		if(n<2U||offset+n>length)return EINVAL;
		if(type==DRV_USB_DESCRIPTOR_INTERFACE&&n>=sizeof(struct drv_usb_interface_descriptor)){
			struct drv_usb_interface_descriptor descriptor;memcpy(&descriptor,raw+offset,sizeof(descriptor));
			current_endpoint=NULL;
			if(descriptor.alternate_setting==0){current=hal_malloc(sizeof(*current));if(!current)return ENOMEM;memset(current,0,sizeof(*current));current->device=device;current->descriptor=descriptor;current->alternate_count=1;if(descriptor.endpoint_count){current->endpoints=hal_malloc(sizeof(*current->endpoints)*descriptor.endpoint_count);if(!current->endpoints){hal_free(current);return ENOMEM;}memset(current->endpoints,0,sizeof(*current->endpoints)*descriptor.endpoint_count);}*tail=current;tail=&current->next;}
			else if(current&&descriptor.interface_number==current->descriptor.interface_number)current->alternate_count++;
		}else if(type==DRV_USB_DESCRIPTOR_ENDPOINT&&n>=sizeof(struct drv_usb_endpoint_descriptor)&&current&&current->endpoint_count<current->descriptor.endpoint_count){struct drv_usb_endpoint*endpoint=&current->endpoints[current->endpoint_count++];memcpy(&endpoint->descriptor,raw+offset,sizeof(endpoint->descriptor));endpoint->interface=current;endpoint->type=(enum drv_usb_transfer_type)(endpoint->descriptor.attributes&3U);current_endpoint=endpoint;}
		else if(type==DRV_USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION&&current_endpoint!=NULL){if(drv_usb_decode_superspeed_endpoint_companion(raw+offset,n,&current_endpoint->companion)!=0)return EINVAL;current_endpoint->companion_valid=1U;current_endpoint=NULL;}
		offset+=n;
	}
	return 0;
}

static int enumerate_port(struct drv_usb_bus *bus,unsigned port,uint32_t status)
{
	struct drv_usb_configuration_descriptor config;
	struct drv_usb_device *device;
	struct drv_usb_interface *interface;
	uint8_t first[8], *raw = NULL;
	size_t actual = 0;
	unsigned packet;
	int address = 0, cleanup_error, error = 0;

	device = hal_malloc(sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	memset(device, 0, sizeof(*device));
	device->bus = bus;
	device->parent = bus->root_hub;
	device->port = port;
	device->speed = (status & 0x800U) ? DRV_USB_SPEED_SUPER :
	    (status & 0x400U) ? DRV_USB_SPEED_HIGH :
	    (status & 0x200U) ? DRV_USB_SPEED_LOW : DRV_USB_SPEED_FULL;
	device->state = DRV_USB_STATE_DEFAULT;
	device->endpoint0.type = DRV_USB_TRANSFER_CONTROL;
	device->endpoint0.descriptor.maximum_packet_size = 8U;
	if (bus->hcd->ops->device_enable != NULL) {
		error = bus->hcd->ops->device_enable(bus->hcd, device);
		if (error != 0)
			goto fail;
	}
	error = drv_usb_control(device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE,
	    USB_REQ_GET_DESCRIPTOR,
	    (uint16_t)(DRV_USB_DESCRIPTOR_DEVICE << 8), 0, first,
	    sizeof(first), 1000U, &actual);
	if (error != 0 || actual != sizeof(first)) {
		if (error == 0)
			error = EIO;
		goto fail;
	}
	if (first[0] != sizeof(struct drv_usb_device_descriptor) ||
	    first[1] != DRV_USB_DESCRIPTOR_DEVICE ||
	    !ep0_packet_size(device->speed, first[7], &packet)) {
		error = EIO;
		goto fail;
	}
	device->descriptor.length = first[0];
	device->descriptor.descriptor_type = first[1];
	device->descriptor.endpoint0_max_packet_size = first[7];
	device->endpoint0.descriptor.maximum_packet_size =
	    (uint16_t)packet;
	address = allocate_address(bus);
	if (address < 0) {
		address = 0;
		error = ENOSPC;
		goto fail;
	}
	/* Reserve the address in the device before the HCD transition so failed
	 * checked teardown retains, and successful teardown releases, one owner. */
	device->address = (unsigned)address;
	if (bus->hcd->ops->device_set_address != NULL)
		error = bus->hcd->ops->device_set_address(bus->hcd, device,
		    (unsigned)address);
	else
		error = drv_usb_control(device,
		    DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_DEVICE,
		    USB_REQ_SET_ADDRESS, (uint16_t)address, 0, NULL, 0,
		    1000U, &actual);
	if (error != 0)
		goto fail;
	device->state = DRV_USB_STATE_ADDRESS;
	usb_delay_ticks(USB_ADDRESS_RECOVERY_TICKS);
	error = drv_usb_control(device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE,
	    USB_REQ_GET_DESCRIPTOR,
	    (uint16_t)(DRV_USB_DESCRIPTOR_DEVICE << 8), 0,
	    &device->descriptor, sizeof(device->descriptor), 1000U, &actual);
	if (error != 0 || actual != sizeof(device->descriptor)) {
		if (error == 0)
			error = EIO;
		goto fail;
	}
	error = drv_usb_control(device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE,
	    USB_REQ_GET_DESCRIPTOR,
	    (uint16_t)(DRV_USB_DESCRIPTOR_CONFIGURATION << 8), 0, &config,
	    sizeof(config), 1000U, &actual);
	if (error != 0 || actual != sizeof(config) ||
	    config.total_length < sizeof(config)) {
		if (error == 0)
			error = EIO;
		goto fail;
	}
	raw = hal_malloc(config.total_length);
	if (raw == NULL) {
		error = ENOMEM;
		goto fail;
	}
	error = drv_usb_control(device,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE,
	    USB_REQ_GET_DESCRIPTOR,
	    (uint16_t)(DRV_USB_DESCRIPTOR_CONFIGURATION << 8), 0, raw,
	    config.total_length, 1000U, &actual);
	if (error != 0 || actual < sizeof(config)) {
		if (error == 0)
			error = EIO;
		goto fail;
	}
	device->configurations = hal_malloc(sizeof(*device->configurations));
	if (device->configurations == NULL) {
		error = ENOMEM;
		goto fail;
	}
	memset(device->configurations, 0, sizeof(*device->configurations));
	device->configurations[0].descriptor = config;
	device->configuration_count = 1;
	error = parse_configuration(device, raw, actual);
	if (error != 0)
		goto fail;
	error = drv_usb_control(device,
	    DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD | DRV_USB_RECIP_DEVICE,
	    USB_REQ_SET_CONFIGURATION, config.configuration_value, 0, NULL, 0,
	    1000U, &actual);
	if (error != 0)
		goto fail;
	for (interface = device->interfaces; interface != NULL;
	    interface = interface->next) {
		unsigned endpoint;

		for (endpoint = 0; endpoint < interface->endpoint_count;
		    endpoint++)
			if (bus->hcd->ops->endpoint_enable != NULL &&
			    (error = bus->hcd->ops->endpoint_enable(bus->hcd,
				&interface->endpoints[endpoint])) != 0)
				goto fail;
	}
	device->active_configuration = device->configurations;
	device->state = DRV_USB_STATE_CONFIGURED;
	device_link(bus, device);
	hal_free(raw);
	hal_printf("usb%u: device %u port %u %04x:%04x class %02x configured\n",
	    bus->number, device->address, port, device->descriptor.vendor,
	    device->descriptor.product, device->descriptor.device_class);
	for (interface = device->interfaces; interface != NULL;
	    interface = interface->next)
		(void)drv_usb_interface_probe(interface);
	return 0;

fail:
	if (error == 0)
		error = EIO;
	device_begin_disconnect(device);
	cleanup_error = device_quiesce(bus, device);
	if (cleanup_error != 0) {
		device->quarantine_buffer = raw;
		return error;
	}
	if (raw != NULL)
		hal_free(raw);
	device_release(bus, device);
	return error;
}

static int
root_port_status(struct drv_usb_hcd *hcd, unsigned port, uint32_t *status)
{
	struct drv_usb_control_request request = {
		0xa3U, 0, 0, (uint16_t)port, 4
	};
	size_t actual = 0;
	int error;

	error = hcd->ops->root_hub_control(hcd, &request, status,
	    sizeof(*status), &actual);
	return error != 0 ? error : actual == sizeof(*status) ? 0 : EIO;
}

static int
root_port_acknowledge_changes(struct drv_usb_hcd *hcd, unsigned port,
	uint32_t status)
{
	static const uint16_t change_features[] = {
		16U, /* C_PORT_CONNECTION */
		17U, /* C_PORT_ENABLE */
		18U, /* C_PORT_SUSPEND */
		19U, /* C_PORT_OVER_CURRENT */
		20U, /* C_PORT_RESET */
		21U, /* C_BH_PORT_RESET */
		22U, /* C_PORT_LINK_STATE */
		23U  /* C_PORT_CONFIG_ERROR */
	};
	struct drv_usb_control_request request = {
		0x23U, 1, 0, (uint16_t)port, 0
	};
	size_t actual;
	unsigned index;
	int error;

	for (index = 0; index < sizeof(change_features) /
	    sizeof(change_features[0]); index++) {
		if ((status & (1U << change_features[index])) == 0)
			continue;
		request.value = change_features[index];
		actual = 0;
		error = hcd->ops->root_hub_control(hcd, &request, NULL, 0,
		    &actual);
		if (error != 0)
			return error;
	}
	return 0;
}

static int
legacy_root_port_reset(struct drv_usb_hcd *hcd, unsigned port)
{
	struct drv_usb_control_request request = {
		0x23U, 3, 4, (uint16_t)port, 0
	};
	size_t actual = 0;
	int error;

	error = hcd->ops->root_hub_control(hcd, &request, NULL, 0, &actual);
	if (error != 0)
		return error;
	usb_delay_ticks(5U);
	request.request = 1;
	error = hcd->ops->root_hub_control(hcd, &request, NULL, 0, &actual);
	if (error != 0)
		return error;
	request.request = 3;
	request.value = 1;
	error = hcd->ops->root_hub_control(hcd, &request, NULL, 0, &actual);
	if (error != 0)
		return error;
	usb_delay_ticks(USB_RESET_RECOVERY_TICKS);
	return 0;
}

void drv_usb_hcd_root_hub_changed(struct drv_usb_hcd *hcd)
{
	struct drv_usb_bus *bus;
	unsigned port;

	usb_topology_lock();
	bus = find_hcd_bus(hcd);
	if (bus == NULL || hal_atomic_load_acquire(&bus->stopping) != 0 ||
	    hcd->ops->root_hub_control == NULL) {
		usb_topology_unlock();
		return;
	}
	for (port = 1; port <= hcd->root_port_count; port++) {
		struct drv_usb_device *present;
		uint32_t status = 0;
		unsigned connected, connection_changed, initial, state_changed;
		int error;

		error = root_port_status(hcd, port, &status);
		if (error != 0)
			continue;
		error = root_port_acknowledge_changes(hcd, port, status);
		if (error != 0) {
			hal_printf("usb%u: port %u change acknowledge failed (%d)\n",
			    bus->number, port, error);
			continue;
		}
		connected = (status & 1U) != 0;
		connection_changed = (status & (1U << 16)) != 0;
		initial = !bus->ports[port].observed;
		state_changed = !initial &&
		    bus->ports[port].connected != connected;
		bus->ports[port].observed = 1U;
		bus->ports[port].connected = connected;
		present = find_port_device(bus, port);
		if (present != NULL && (!connected ||
		    device_is_disconnecting(present))) {
			if (destroy_device(bus, present) == 0)
				present = NULL;
		}
		if (!connected || present != NULL)
			continue;
		if (!initial && !connection_changed && !state_changed)
			continue;
		if (hcd->ops->root_port_reset != NULL)
			error = hcd->ops->root_port_reset(hcd, port);
		else
			error = legacy_root_port_reset(hcd, port);
		if (error == 0)
			error = root_port_status(hcd, port, &status);
		if (error == 0 && (status & 1U) != 0)
			error = enumerate_port(bus, port, status);
		else if (error == 0)
			error = ENODEV;
		if (error != 0)
			hal_printf("usb%u: port %u enumeration failed (%d)\n",
			    bus->number, port, error);
	}
	usb_topology_unlock();
}

static void
urb_put(struct drv_usb_urb *urb)
{
	struct drv_usb_device *device;

	if (!refcount_put(&urb->references))
		return;
	device = urb->device;
	if (urb->iso_packets != NULL)
		hal_free(urb->iso_packets);
	hal_free(urb);
	device_urb_put(device);
}

static int
urb_hcd_get(struct drv_usb_urb *urb)
{
	unsigned expected = 0;

	refcount_get(&urb->references);
	if (!hal_atomic_compare_exchange_acq_rel(&urb->hcd_owned, &expected,
	    1U)) {
		urb_put(urb);
		return EBUSY;
	}
	if (hal_atomic_fetch_add_relaxed(&urb->device->hcd_urb_count, 1U) ==
	    UINT_MAX)
		__builtin_trap();
	return 0;
}

static void
urb_hcd_put(struct drv_usb_urb *urb)
{
	unsigned expected = 1U;

	if (hal_atomic_compare_exchange_acq_rel(&urb->hcd_owned, &expected,
	    0U)) {
		if (hal_atomic_fetch_add_release(&urb->device->hcd_urb_count,
		    (unsigned)-1) == 0)
			__builtin_trap();
		urb_put(urb);
	}
}

static int
urb_publish_terminal(struct drv_usb_urb *urb,
	enum drv_usb_urb_status status, size_t actual)
{
	unsigned expected = 0;

	if (urb == NULL || status == DRV_USB_URB_IDLE ||
	    status == DRV_USB_URB_PENDING)
		return 0;
	if (hal_atomic_load_acquire(&urb->status) != DRV_USB_URB_PENDING)
		return 0;
	if (!hal_atomic_compare_exchange_acq_rel(&urb->terminal_claimed,
	    &expected, 1U))
		return 0;
	urb->actual_length = actual > urb->length ? urb->length : actual;
	/* The terminal state publishes actual_length and all HCD input data. */
	hal_atomic_store_release(&urb->status, status);
	if (urb->callback != NULL)
		urb->callback(urb, urb->callback_argument);
	return 1;
}

void drv_usb_hcd_complete(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb,
	enum drv_usb_urb_status status, size_t actual)
{
	(void)hcd;
	(void)urb_publish_terminal(urb, status, actual);
	urb_hcd_put(urb);
}

int drv_usb_foreach_bus(drv_usb_bus_iterator_t fn, void *argument)
{ struct drv_usb_bus*b;int e;if(!fn)return EINVAL;for(b=usb_buses;b;b=b->next)if((e=fn(b,argument))!=0)return e;return 0; }
int drv_usb_bus_foreach_device(struct drv_usb_bus*b,drv_usb_device_iterator_t fn,void*a)
{struct drv_usb_device*d;int e;if(!b||!fn)return EINVAL;if((e=fn(b->root_hub,a))!=0)return e;for(d=b->devices;d;d=d->next)if((e=fn(d,a))!=0)return e;return 0;}
int drv_usb_foreach_device(drv_usb_device_iterator_t fn,void*a)
{struct drv_usb_bus*b;int e;if(!fn)return EINVAL;for(b=usb_buses;b;b=b->next)if((e=drv_usb_bus_foreach_device(b,fn,a))!=0)return e;return 0;}
int drv_usb_device_foreach_interface(struct drv_usb_device*d,drv_usb_interface_iterator_t fn,void*a)
{struct drv_usb_interface*i;int e;if(!d||!fn)return EINVAL;for(i=d->interfaces;i;i=i->next)if((e=fn(i,a))!=0)return e;return 0;}
struct drv_usb_device*drv_usb_find_device(unsigned bus,unsigned address)
{struct drv_usb_bus*b;struct drv_usb_device*d;for(b=usb_buses;b;b=b->next)if(b->number==bus){if(address==0)return b->root_hub;for(d=b->devices;d;d=d->next)if(d->address==address)return d;}return NULL;}
unsigned drv_usb_bus_number(const struct drv_usb_bus*b){return b?b->number:0;}
struct drv_usb_hcd*drv_usb_bus_hcd(const struct drv_usb_bus*b){return b?b->hcd:NULL;}
struct drv_usb_device*drv_usb_bus_root_hub(const struct drv_usb_bus*b){return b?b->root_hub:NULL;}
struct drv_usb_bus*drv_usb_device_bus(const struct drv_usb_device*d){return d?d->bus:NULL;}
struct drv_usb_device*drv_usb_device_parent(const struct drv_usb_device*d){return d?d->parent:NULL;}
unsigned drv_usb_device_address(const struct drv_usb_device*d){return d?d->address:0;}
unsigned drv_usb_device_port(const struct drv_usb_device*d){return d?d->port:0;}
enum drv_usb_speed drv_usb_device_speed(const struct drv_usb_device*d){return d?d->speed:DRV_USB_SPEED_UNKNOWN;}
enum drv_usb_device_state drv_usb_device_state(const struct drv_usb_device*d){return d?d->state:DRV_USB_STATE_NOT_ATTACHED;}
const struct drv_usb_device_descriptor*drv_usb_device_descriptor(const struct drv_usb_device*d){return d?&d->descriptor:NULL;}
unsigned drv_usb_device_hcd_urb_count(const struct drv_usb_device*d){return d?hal_atomic_load_acquire(&d->hcd_urb_count):0;}
uintptr_t drv_usb_device_hcd_data(const struct drv_usb_device*d,unsigned n){return d&&n<4U?__atomic_load_n(&d->hcd_private[n],__ATOMIC_ACQUIRE):0;}
int drv_usb_device_set_hcd_data(struct drv_usb_device*d,unsigned n,uintptr_t value){if(!d||n>=4U)return EINVAL;__atomic_store_n(&d->hcd_private[n],value,__ATOMIC_RELEASE);return 0;}
struct drv_dma_device*drv_usb_device_dma(struct drv_usb_device*d){return d?d->bus->hcd->dma:NULL;}
int drv_usb_device_reset(struct drv_usb_device*d){(void)d;return ENOTSUP;}
int drv_usb_device_set_configuration(struct drv_usb_device*d,unsigned n){(void)d;(void)n;return ENOTSUP;}
int drv_usb_device_get_string(struct drv_usb_device*d,unsigned i,unsigned l,char*b,size_t n){(void)d;(void)i;(void)l;(void)b;(void)n;return ENOTSUP;}

struct drv_usb_urb *drv_usb_urb_alloc(struct drv_usb_device*d,
	struct drv_usb_endpoint*e,unsigned iso_count)
{
	struct drv_usb_urb*u;if(!d||!device_urb_get(d))return NULL;
	if(e==NULL)e=&d->endpoint0;
	u=hal_malloc(sizeof(*u));if(!u){device_urb_put(d);return NULL;}memset(u,0,sizeof(*u));
	refcount_init(&u->references,1U);u->device=d;u->endpoint=e;u->iso_packet_count=iso_count;
	if(iso_count){u->iso_packets=hal_malloc(sizeof(*u->iso_packets)*iso_count);if(!u->iso_packets){hal_free(u);device_urb_put(d);return NULL;}memset(u->iso_packets,0,sizeof(*u->iso_packets)*iso_count);}
	return u;
}
void drv_usb_urb_free(struct drv_usb_urb*u){if(u)urb_put(u);}
int drv_usb_urb_setup(struct drv_usb_urb*u,void*b,size_t n,unsigned f,unsigned t,drv_usb_urb_callback_t cb,void*a){if(!u||hal_atomic_load_acquire(&u->status)==DRV_USB_URB_PENDING||(!b&&n))return EINVAL;if(hal_atomic_load_acquire(&u->hcd_owned))return EBUSY;u->buffer=b;u->length=n;u->flags=f;u->timeout_ms=t;u->callback=cb;u->callback_argument=a;u->actual_length=0;hal_atomic_store_relaxed(&u->terminal_claimed,0U);hal_atomic_store_release(&u->status,DRV_USB_URB_IDLE);return 0;}
int drv_usb_urb_setup_control(struct drv_usb_urb*u,const struct drv_usb_control_request*r,void*b,size_t n,unsigned t,drv_usb_urb_callback_t cb,void*a){int e;if(!u||!r||u->endpoint->type!=DRV_USB_TRANSFER_CONTROL)return EINVAL;e=drv_usb_urb_setup(u,b,n,0,t,cb,a);if(!e)u->control=*r;return e;}
int drv_usb_urb_setup_isochronous(struct drv_usb_urb*u,struct drv_usb_iso_packet*p,unsigned n){if(!u||!p||n!=u->iso_packet_count||u->endpoint->type!=DRV_USB_TRANSFER_ISOCHRONOUS)return EINVAL;memcpy(u->iso_packets,p,n*sizeof(*p));return 0;}
int drv_usb_urb_submit(struct drv_usb_urb*u){int e;if(!u||hal_atomic_load_acquire(&u->status)==DRV_USB_URB_PENDING)return EINVAL;if(hal_atomic_load_acquire(&u->device->bus->stopping)!=0)return EBUSY;if(device_is_disconnecting(u->device)||u->device->quarantined)return ENODEV;if((e=urb_hcd_get(u))!=0)return e;if(hal_atomic_load_acquire(&u->device->bus->stopping)!=0||device_is_disconnecting(u->device)||u->device->quarantined){urb_hcd_put(u);return ENODEV;}u->actual_length=0;hal_atomic_store_relaxed(&u->terminal_claimed,0U);hal_atomic_store_release(&u->status,DRV_USB_URB_PENDING);e=u->device->bus->hcd->ops->urb_enqueue(u->device->bus->hcd,u);if(e){hal_atomic_store_release(&u->status,DRV_USB_URB_IDLE);urb_hcd_put(u);}return e;}
static int urb_cancel_to(struct drv_usb_urb*u,enum drv_usb_urb_status terminal){int e,published;if(!u||hal_atomic_load_acquire(&u->status)!=DRV_USB_URB_PENDING)return EINVAL;e=u->device->bus->hcd->ops->urb_dequeue(u->device->bus->hcd,u);if(e)return e;published=urb_publish_terminal(u,terminal,0);urb_hcd_put(u);return published?0:EALREADY;}
int drv_usb_urb_cancel(struct drv_usb_urb*u){return urb_cancel_to(u,DRV_USB_URB_CANCELLED);}
static int urb_status_error(enum drv_usb_urb_status status){return status==DRV_USB_URB_COMPLETE?0:status==DRV_USB_URB_TIMEOUT?ETIMEDOUT:status==DRV_USB_URB_STALL?EPIPE:status==DRV_USB_URB_DISCONNECTED?ENODEV:EIO;}
int drv_usb_urb_wait(struct drv_usb_urb*u){uint64_t deadline,cancel_deadline=0;enum drv_usb_urb_status status;if(!u)return EINVAL;deadline=u->timeout_ms?sched_ticks()+(u->timeout_ms+9U)/10U:0;for(;;){status=hal_atomic_load_acquire(&u->status);if(status!=DRV_USB_URB_PENDING)return urb_status_error(status);if(deadline&&sched_ticks()>=deadline){int e=urb_cancel_to(u,DRV_USB_URB_TIMEOUT);if(e==0)continue;if(e!=EBUSY&&e!=EINVAL&&e!=EALREADY)return e;if(cancel_deadline==0)cancel_deadline=sched_ticks()+100U;if(sched_ticks()>=cancel_deadline)return ETIMEDOUT;sched_yield();continue;}hal_compiler_barrier();}}
enum drv_usb_urb_status drv_usb_urb_status(const struct drv_usb_urb*u){return u?hal_atomic_load_acquire(&u->status):DRV_USB_URB_IO_ERROR;}
size_t drv_usb_urb_actual_length(const struct drv_usb_urb*u){enum drv_usb_urb_status status;if(!u)return 0;status=hal_atomic_load_acquire(&u->status);return status==DRV_USB_URB_PENDING?0:u->actual_length;}
void*drv_usb_urb_buffer(const struct drv_usb_urb*u){return u?u->buffer:NULL;}
size_t drv_usb_urb_length(const struct drv_usb_urb*u){return u?u->length:0;}
unsigned drv_usb_urb_flags(const struct drv_usb_urb*u){return u?u->flags:0;}
const struct drv_usb_control_request*drv_usb_urb_control_request(const struct drv_usb_urb*u){return u&&u->endpoint->type==DRV_USB_TRANSFER_CONTROL?&u->control:NULL;}
void*drv_usb_urb_hcd_data(const struct drv_usb_urb*u){return u?(void*)u->hcd_private[0]:NULL;}
int drv_usb_urb_set_hcd_data(struct drv_usb_urb*u,void*d){if(!u)return EINVAL;u->hcd_private[0]=(uintptr_t)d;return 0;}
struct drv_usb_device*drv_usb_urb_device(const struct drv_usb_urb*u){return u?u->device:NULL;}
struct drv_usb_endpoint*drv_usb_urb_endpoint(const struct drv_usb_urb*u){return u?u->endpoint:NULL;}

int drv_usb_control(struct drv_usb_device*d,uint8_t rt,uint8_t r,uint16_t v,uint16_t i,void*b,size_t n,unsigned t,size_t*a){struct drv_usb_control_request q={rt,r,v,i,(uint16_t)n};struct drv_usb_urb*u;int e;if(!d||n>0xffffU)return EINVAL;u=drv_usb_urb_alloc(d,NULL,0);if(!u)return ENOMEM;e=drv_usb_urb_setup_control(u,&q,b,n,t,NULL,NULL);if(!e)e=drv_usb_urb_submit(u);if(!e)e=drv_usb_urb_wait(u);if(a)*a=drv_usb_urb_actual_length(u);drv_usb_urb_free(u);return e;}
static int sync_data(struct drv_usb_device*d,struct drv_usb_endpoint*ep,void*b,size_t n,unsigned t,size_t*a){struct drv_usb_urb*u;int e;if(!d||!ep)return EINVAL;u=drv_usb_urb_alloc(d,ep,0);if(!u)return ENOMEM;e=drv_usb_urb_setup(u,b,n,0,t,NULL,NULL);if(!e)e=drv_usb_urb_submit(u);if(!e)e=drv_usb_urb_wait(u);if(a)*a=drv_usb_urb_actual_length(u);drv_usb_urb_free(u);return e;}
int drv_usb_bulk(struct drv_usb_device*d,struct drv_usb_endpoint*e,void*b,size_t n,unsigned t,size_t*a){return e&&e->type==DRV_USB_TRANSFER_BULK?sync_data(d,e,b,n,t,a):EINVAL;}
int drv_usb_interrupt(struct drv_usb_device*d,struct drv_usb_endpoint*e,void*b,size_t n,unsigned t,size_t*a){return e&&e->type==DRV_USB_TRANSFER_INTERRUPT?sync_data(d,e,b,n,t,a):EINVAL;}

const struct drv_usb_configuration_descriptor*drv_usb_configuration_descriptor(const struct drv_usb_configuration*c){return c?&c->descriptor:NULL;}
unsigned drv_usb_device_configuration_count(const struct drv_usb_device*d){return d?d->configuration_count:0;}
struct drv_usb_configuration*drv_usb_device_configuration(struct drv_usb_device*d,unsigned i){return d&&i<d->configuration_count?&d->configurations[i]:NULL;}
struct drv_usb_configuration*drv_usb_device_active_configuration(struct drv_usb_device*d){return d?d->active_configuration:NULL;}
struct drv_usb_device*drv_usb_interface_device(const struct drv_usb_interface*i){return i?i->device:NULL;}
const struct drv_usb_interface_descriptor*drv_usb_interface_descriptor(const struct drv_usb_interface*i){return i?&i->descriptor:NULL;}
unsigned drv_usb_interface_number(const struct drv_usb_interface*i){return i?i->descriptor.interface_number:0;}
unsigned drv_usb_interface_alternate_count(const struct drv_usb_interface*i){return i?i->alternate_count:0;}
int drv_usb_interface_set_alternate(struct drv_usb_interface*i,unsigned a){(void)i;(void)a;return ENOTSUP;}
struct drv_usb_driver*drv_usb_interface_driver(const struct drv_usb_interface*i){return i?i->driver:NULL;}
void*drv_usb_interface_driver_data(const struct drv_usb_interface*i){return i?i->driver_data:NULL;}
int drv_usb_interface_set_driver_data(struct drv_usb_interface*i,void*d){if(!i)return EINVAL;i->driver_data=d;return 0;}
unsigned drv_usb_interface_endpoint_count(const struct drv_usb_interface*i){return i?i->endpoint_count:0;}
struct drv_usb_endpoint*drv_usb_interface_endpoint(struct drv_usb_interface*i,unsigned n){return i&&n<i->endpoint_count?&i->endpoints[n]:NULL;}
struct drv_usb_endpoint*drv_usb_interface_find_endpoint(struct drv_usb_interface*i,enum drv_usb_transfer_type t,uint8_t dir,struct drv_usb_endpoint*after){unsigned n=0;if(!i)return NULL;if(after)n=(unsigned)(after-i->endpoints)+1U;for(;n<i->endpoint_count;n++)if(i->endpoints[n].type==t&&(i->endpoints[n].descriptor.address&DRV_USB_DIR_IN)==dir)return&i->endpoints[n];return NULL;}
struct drv_usb_device*drv_usb_endpoint_device(const struct drv_usb_endpoint*e){return e&&e->interface?e->interface->device:NULL;}
const struct drv_usb_endpoint_descriptor*drv_usb_endpoint_descriptor(const struct drv_usb_endpoint*e){return e?&e->descriptor:NULL;}
enum drv_usb_transfer_type drv_usb_endpoint_type(const struct drv_usb_endpoint*e){return e?e->type:DRV_USB_TRANSFER_CONTROL;}
uint8_t drv_usb_endpoint_address(const struct drv_usb_endpoint*e){return e?e->descriptor.address:0;}
uint16_t drv_usb_endpoint_max_packet_size(const struct drv_usb_endpoint*e){return e?e->descriptor.maximum_packet_size:0;}
uint8_t drv_usb_endpoint_maximum_burst(const struct drv_usb_endpoint*e){return e&&e->companion_valid?e->companion.maximum_burst:0;}
bool drv_usb_endpoint_is_input(const struct drv_usb_endpoint*e){return e&&(e->descriptor.address&DRV_USB_DIR_IN)!=0;}
uintptr_t drv_usb_endpoint_hcd_data(const struct drv_usb_endpoint*e,unsigned n){return e&&n<4U?e->hcd_private[n]:0;}
int drv_usb_endpoint_set_hcd_data(struct drv_usb_endpoint*e,unsigned n,uintptr_t value){if(!e||n>=4U)return EINVAL;e->hcd_private[n]=value;return 0;}

int drv_usb_id_match(const struct drv_usb_id*id,const struct drv_usb_interface*i){const struct drv_usb_device_descriptor*d;if(!id||!i)return 0;d=&i->device->descriptor;return(!(id->match_flags&DRV_USB_ID_VENDOR)||id->vendor==d->vendor)&&(!(id->match_flags&DRV_USB_ID_PRODUCT)||id->product==d->product)&&(!(id->match_flags&DRV_USB_ID_RELEASE_RANGE)||(d->device_release>=id->release_minimum&&d->device_release<=id->release_maximum))&&(!(id->match_flags&DRV_USB_ID_DEVICE_CLASS)||id->device_class==d->device_class)&&(!(id->match_flags&DRV_USB_ID_DEVICE_SUBCLASS)||id->device_subclass==d->device_subclass)&&(!(id->match_flags&DRV_USB_ID_DEVICE_PROTOCOL)||id->device_protocol==d->device_protocol)&&(!(id->match_flags&DRV_USB_ID_IF_CLASS)||id->interface_class==i->descriptor.interface_class)&&(!(id->match_flags&DRV_USB_ID_IF_SUBCLASS)||id->interface_subclass==i->descriptor.interface_subclass)&&(!(id->match_flags&DRV_USB_ID_IF_PROTOCOL)||id->interface_protocol==i->descriptor.interface_protocol)&&(!(id->match_flags&DRV_USB_ID_IF_NUMBER)||id->interface_number==i->descriptor.interface_number);}
const struct drv_usb_id*drv_usb_driver_find_id(const struct drv_usb_driver*d,const struct drv_usb_interface*i){size_t n;if(!d||!i)return NULL;for(n=0;n<d->id_count;n++)if(drv_usb_id_match(&d->ids[n],i))return&d->ids[n];return NULL;}
int drv_usb_interface_probe(struct drv_usb_interface*i){struct usb_driver_entry*e;const struct drv_usb_id*id;int score,best=0,error;struct drv_usb_driver*driver=NULL;if(!i||i->driver)return i?EBUSY:EINVAL;for(e=usb_drivers;e;e=e->next){id=drv_usb_driver_find_id(e->driver,i);if(!id)continue;score=e->driver->match?e->driver->match(i,id):1;if(score>best){best=score;driver=e->driver;}}if(!driver)return ENODEV;id=drv_usb_driver_find_id(driver,i);error=driver->attach?driver->attach(i,id):0;if(!error)i->driver=driver;return error;}
int drv_usb_interface_detach(struct drv_usb_interface*i,unsigned f){int e=0;if(!i||!i->driver)return EINVAL;if(i->driver->detach)e=i->driver->detach(i,f);if(!e){i->driver=NULL;i->driver_data=NULL;}return e;}
int drv_usb_driver_register(struct drv_usb_driver*d){struct usb_driver_entry*e;if(!usb_initialized||!d||!d->name)return EINVAL;for(e=usb_drivers;e;e=e->next)if(e->driver==d)return EEXIST;e=hal_malloc(sizeof(*e));if(!e)return ENOMEM;e->driver=d;e->next=usb_drivers;usb_drivers=e;return 0;}
int drv_usb_driver_unregister(struct drv_usb_driver*d){struct usb_driver_entry**p,*e;if(!d)return EINVAL;for(p=&usb_drivers;(e=*p)!=NULL;p=&e->next)if(e->driver==d){*p=e->next;hal_free(e);return 0;}return ENOENT;}
void drv_usb_dump(void){struct drv_usb_bus*b;for(b=usb_buses;b;b=b->next)hal_printf("usb%u: hcd=%s ports=%u\n",b->number,b->hcd->name,b->hcd->root_port_count);}
