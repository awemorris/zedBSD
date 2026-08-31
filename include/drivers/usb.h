/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generic USB host bus and built-in driver interface
 *
 * This interface references and uses API concepts, terminology, object
 * hierarchy, and host-side driver conventions from the Linux USB
 * subsystem, including its URB model.  The contracts are adapted for
 * zedBSD; no Linux implementation source code is included in this file.
 * Host-controller-specific objects such as UHCI TD/QH, EHCI qTD/QH, and
 * xHCI TRB/rings must remain private to their respective HCDs.
 */

#ifndef ZEDBSD_DRIVERS_USB_H
#define ZEDBSD_DRIVERS_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/dma.h>

#define DRV_USB_ANY_ID	((uint16_t)0xffffU)
#define DRV_USB_ANY_CLASS	((uint8_t)0xffU)
#define DRV_USB_MAX_ADDRESS	127U
#define DRV_USB_MAX_CONFIGURATIONS	8U
#define DRV_USB_MAX_ENDPOINTS	32U
#define DRV_USB_MAX_INTERFACES	32U
#define DRV_USB_MAX_ALTERNATES	32U
#define DRV_USB_MAX_IADS	32U

#define DRV_USB_DIR_OUT	0x00U
#define DRV_USB_DIR_IN	0x80U

#define DRV_USB_REQUEST_STANDARD	0x00U
#define DRV_USB_REQUEST_CLASS	0x20U
#define DRV_USB_REQUEST_VENDOR	0x40U
#define DRV_USB_RECIP_DEVICE	0x00U
#define DRV_USB_RECIP_INTERFACE	0x01U
#define DRV_USB_RECIP_ENDPOINT	0x02U
#define DRV_USB_RECIP_OTHER	0x03U

#define DRV_USB_DESCRIPTOR_DEVICE	1U
#define DRV_USB_DESCRIPTOR_CONFIGURATION	2U
#define DRV_USB_DESCRIPTOR_STRING	3U
#define DRV_USB_DESCRIPTOR_INTERFACE	4U
#define DRV_USB_DESCRIPTOR_ENDPOINT	5U
#define DRV_USB_DESCRIPTOR_INTERFACE_ASSOCIATION	11U
#define DRV_USB_DESCRIPTOR_BOS	15U
#define DRV_USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION	48U

#define DRV_USB_ID_VENDOR	(1U << 0)
#define DRV_USB_ID_PRODUCT	(1U << 1)
#define DRV_USB_ID_RELEASE_RANGE	(1U << 2)
#define DRV_USB_ID_DEVICE_CLASS	(1U << 3)
#define DRV_USB_ID_DEVICE_SUBCLASS	(1U << 4)
#define DRV_USB_ID_DEVICE_PROTOCOL	(1U << 5)
#define DRV_USB_ID_IF_CLASS	(1U << 6)
#define DRV_USB_ID_IF_SUBCLASS	(1U << 7)
#define DRV_USB_ID_IF_PROTOCOL	(1U << 8)
#define DRV_USB_ID_IF_NUMBER	(1U << 9)

#define DRV_USB_URB_SHORT_OK	(1U << 0)
#define DRV_USB_URB_ZERO_PACKET	(1U << 1)
#define DRV_USB_URB_NO_DMA_MAP	(1U << 2)
#define DRV_USB_URB_ISO_ASAP	(1U << 3)
/* Eligibility hint for HCDs which advertise a bounded reclaim reserve.  The
 * complete transfer path may run while VM reclaim has no free page, so a
 * supporting HCD uses preallocated request/DMA storage.  HCDs without that
 * capability retain their existing behavior; the flag alone is not a
 * portable no-allocation guarantee. */
#define DRV_USB_URB_RECLAIM_SAFE	(1U << 4)
#define DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE	8192U

/* The HCD accepts one active URB per endpoint instead of one per controller. */
#define DRV_USB_HCD_CAP_CONCURRENT_URBS	(1U << 0)

#define DRV_USB_DETACH_FORCE	(1U << 0)
#define DRV_USB_DETACH_QUIET	(1U << 1)
#define DRV_USB_DETACH_ATTACH_FAILED	(1U << 2)

struct drv_usb_bus;
struct drv_usb_device;
struct drv_usb_configuration;
struct drv_usb_interface;
struct drv_usb_host_interface;
struct drv_usb_endpoint;
struct drv_usb_urb;
struct drv_usb_driver;
struct drv_usb_hcd;

enum drv_usb_speed {
	DRV_USB_SPEED_UNKNOWN,
	DRV_USB_SPEED_LOW,
	DRV_USB_SPEED_FULL,
	DRV_USB_SPEED_HIGH,
	DRV_USB_SPEED_SUPER,
	DRV_USB_SPEED_SUPER_PLUS
};

enum drv_usb_device_state {
	DRV_USB_STATE_NOT_ATTACHED,
	DRV_USB_STATE_ATTACHED,
	DRV_USB_STATE_POWERED,
	DRV_USB_STATE_DEFAULT,
	DRV_USB_STATE_ADDRESS,
	DRV_USB_STATE_CONFIGURED,
	DRV_USB_STATE_SUSPENDED
};

enum drv_usb_transfer_type {
	DRV_USB_TRANSFER_CONTROL,
	DRV_USB_TRANSFER_ISOCHRONOUS,
	DRV_USB_TRANSFER_BULK,
	DRV_USB_TRANSFER_INTERRUPT
};

enum drv_usb_urb_status {
	DRV_USB_URB_IDLE,
	DRV_USB_URB_PENDING,
	DRV_USB_URB_COMPLETE,
	DRV_USB_URB_CANCELLED,
	DRV_USB_URB_STALL,
	DRV_USB_URB_TIMEOUT,
	DRV_USB_URB_DISCONNECTED,
	DRV_USB_URB_IO_ERROR
};

struct drv_usb_device_descriptor {
	uint8_t length;
	uint8_t descriptor_type;
	uint16_t usb_release;
	uint8_t device_class;
	uint8_t device_subclass;
	uint8_t device_protocol;
	uint8_t endpoint0_max_packet_size;
	uint16_t vendor;
	uint16_t product;
	uint16_t device_release;
	uint8_t manufacturer_string;
	uint8_t product_string;
	uint8_t serial_string;
	uint8_t configuration_count;
} __attribute__((packed));

struct drv_usb_configuration_descriptor {
	uint8_t length;
	uint8_t descriptor_type;
	uint16_t total_length;
	uint8_t interface_count;
	uint8_t configuration_value;
	uint8_t configuration_string;
	uint8_t attributes;
	uint8_t maximum_power;
} __attribute__((packed));

struct drv_usb_interface_descriptor {
	uint8_t length;
	uint8_t descriptor_type;
	uint8_t interface_number;
	uint8_t alternate_setting;
	uint8_t endpoint_count;
	uint8_t interface_class;
	uint8_t interface_subclass;
	uint8_t interface_protocol;
	uint8_t interface_string;
} __attribute__((packed));

struct drv_usb_interface_association_descriptor {
	uint8_t length;
	uint8_t descriptor_type;
	uint8_t first_interface;
	uint8_t interface_count;
	uint8_t function_class;
	uint8_t function_subclass;
	uint8_t function_protocol;
	uint8_t function_string;
} __attribute__((packed));

struct drv_usb_endpoint_descriptor {
	uint8_t length;
	uint8_t descriptor_type;
	uint8_t address;
	uint8_t attributes;
	uint16_t maximum_packet_size;
	uint8_t interval;
} __attribute__((packed));

struct drv_usb_superspeed_endpoint_companion_descriptor {
	uint8_t length;
	uint8_t descriptor_type;
	uint8_t maximum_burst;
	uint8_t attributes;
	uint16_t bytes_per_interval;
} __attribute__((packed));

int
drv_usb_decode_superspeed_endpoint_companion(
	const void *raw,
	size_t length,
	struct drv_usb_superspeed_endpoint_companion_descriptor *result);

struct drv_usb_control_request {
	uint8_t request_type;
	uint8_t request;
	uint16_t value;
	uint16_t index;
	uint16_t length;
} __attribute__((packed));

struct drv_usb_iso_packet {
	size_t offset;
	size_t length;
	size_t actual_length;
	enum drv_usb_urb_status status;
};

struct drv_usb_id {
	uint32_t match_flags;
	uint16_t vendor;
	uint16_t product;
	uint16_t release_minimum;
	uint16_t release_maximum;
	uint8_t device_class;
	uint8_t device_subclass;
	uint8_t device_protocol;
	uint8_t interface_class;
	uint8_t interface_subclass;
	uint8_t interface_protocol;
	uint8_t interface_number;
	uintptr_t driver_data;
};

typedef void (
	*drv_usb_urb_callback_t)(
	struct drv_usb_urb *,
	void *);
typedef int (
	*drv_usb_bus_iterator_t)(
	struct drv_usb_bus *,
	void *);
typedef int (
	*drv_usb_device_iterator_t)(
	struct drv_usb_device *,
	void *);
typedef int (
	*drv_usb_interface_iterator_t)(
	struct drv_usb_interface *,
	void *);

/*
 * A function driver binds to one interface in the active configuration.
 * A composite device may therefore be served by several different drivers.
 */
struct drv_usb_driver {
	const char *name;
	const struct drv_usb_id *ids;
	size_t id_count;
	/* Matching is observational and may run against a retained inactive
	 * configuration while the core chooses which configuration to select. */
	int (
		*match)(
		struct drv_usb_interface *,
		const struct drv_usb_id *);
	int (
		*attach)(
		struct drv_usb_interface *,
		const struct drv_usb_id *);
	int (
		*detach)(
		struct drv_usb_interface *,
		unsigned);
	int (
		*suspend)(
		struct drv_usb_interface *);
	int (
		*resume)(
		struct drv_usb_interface *);
	void (
		*shutdown)(
		struct drv_usb_interface *);
	uintptr_t private_data[4];
};

/*
 * Host controller contract.  The USB core owns enumeration, descriptor
 * parsing, address assignment, driver binding, timeouts, and completion
 * delivery.  The HCD owns hardware scheduling and root-hub operations.
 */
struct drv_usb_hcd_ops {
	int (
		*start)(
		struct drv_usb_hcd *);
	/* Optional checked stop barrier.  A failure keeps the registered bus and
	 * all HCD-owned resources so the driver can retry or remain quarantined. */
	int (
		*quiesce)(
		struct drv_usb_hcd *);
	/* Release resources after quiesce, or perform the legacy unchecked stop
	 * for controllers which do not provide a quiesce callback. */
	void (
		*stop)(
		struct drv_usb_hcd *);
	int (
		*device_enable)(
		struct drv_usb_hcd *,
		struct drv_usb_device *);
	int (
		*device_set_address)(
		struct drv_usb_hcd *,
		struct drv_usb_device *,
		unsigned);
	/* Optional checked device-DMA barrier.  On failure the USB core keeps
	 * the device and every HCD-owned resource quarantined for a later retry. */
	int (
		*device_quiesce)(
		struct drv_usb_hcd *,
		struct drv_usb_device *);
	/* Release resources only after device_quiesce succeeds, or perform the
	 * legacy unchecked teardown when device_quiesce is not implemented. */
	void (
		*device_disable)(
		struct drv_usb_hcd *,
		struct drv_usb_device *);
	/* Zero accepts HCD ownership and requires exactly one call to
	 * drv_usb_hcd_complete(); completion may be synchronous but an accepted
	 * enqueue must still return zero.  A nonzero return leaves ownership with
	 * the core and forbids completion for that enqueue attempt. */
	int (
		*urb_enqueue)(
		struct drv_usb_hcd *,
		struct drv_usb_urb *);
	/* Zero retires the HCD request and transfers terminal publication and
	 * ownership release to the core, so the HCD must not complete it afterward.
	 * A nonzero return leaves the accepted request under the normal completion
	 * contract. */
	int (
		*urb_dequeue)(
		struct drv_usb_hcd *,
		struct drv_usb_urb *);
	/* Endpoint callbacks are a pair: both must be supplied, or both omitted
	 * by HCDs whose schedules need no endpoint-level programming.  A failed
	 * enable must leave the endpoint disabled and without HCD-owned resources;
	 * the USB core compensates only endpoints whose enable returned success. */
	int (
		*endpoint_enable)(
		struct drv_usb_hcd *,
		struct drv_usb_endpoint *);
	/* A checked disable is required for safe alternate/configuration
	 * transitions.  Failure means the endpoint and all of its HCD-owned
	 * resources remain enabled and reachable by the old setting. */
	int (
		*endpoint_disable)(
		struct drv_usb_hcd *,
		struct drv_usb_endpoint *);
	uint32_t (
		*frame_number)(
		struct drv_usb_hcd *);
	int (
		*root_hub_status)(
		struct drv_usb_hcd *,
		void *,
		size_t,
		size_t *);
	int (
		*root_hub_control)(
		struct drv_usb_hcd *,
		const struct drv_usb_control_request *,
		void *,
		size_t,
		size_t *);
	/* Optional controller-specific synchronous root-port reset.  The callback
	 * returns only after the port is enabled at its usable link state. */
	int (
		*root_port_reset)(
		struct drv_usb_hcd *,
		unsigned);
};

struct drv_usb_hcd {
	const char *name;
	const struct drv_usb_hcd_ops *ops;
	struct drv_dma_device *dma;
	unsigned root_port_count;
	unsigned capabilities;
	uintptr_t private_data[6];
};

/*
 * Core and host-controller lifecycle.
 */
int
drv_usb_init(void);
void
drv_usb_shutdown(void);
int
drv_usb_hcd_register(
	struct drv_usb_hcd *hcd,
	struct drv_usb_bus **result);
int
drv_usb_hcd_unregister(
	struct drv_usb_hcd *hcd);
void
drv_usb_hcd_root_hub_changed(
	struct drv_usb_hcd *hcd);
void
drv_usb_hcd_complete(
	struct drv_usb_hcd *hcd,
	struct drv_usb_urb *urb,
	enum drv_usb_urb_status status,
	size_t actual);

/*
 * Bus, device, and interface enumeration.
 */
int
drv_usb_foreach_bus(
	drv_usb_bus_iterator_t fn,
	void *argument);
int
drv_usb_foreach_device(
	drv_usb_device_iterator_t fn,
	void *a);
int
drv_usb_bus_foreach_device(
	struct drv_usb_bus *b,
	drv_usb_device_iterator_t fn,
	void *a);
int
drv_usb_device_foreach_interface(
	struct drv_usb_device *d,
	drv_usb_interface_iterator_t fn,
	void *a);
struct drv_usb_device *
drv_usb_find_device(
	unsigned bus,
	unsigned address);
unsigned
drv_usb_bus_number(
	const struct drv_usb_bus *b);
struct drv_usb_hcd *
drv_usb_bus_hcd(
	const struct drv_usb_bus *b);
struct drv_usb_device *
drv_usb_bus_root_hub(
	const struct drv_usb_bus *b);

/*
 * Device identity, topology, state, and standard requests.
 */
struct drv_usb_bus *
drv_usb_device_bus(
	const struct drv_usb_device *d);
struct drv_usb_device *
drv_usb_device_parent(
	const struct drv_usb_device *d);
unsigned
drv_usb_device_address(
	const struct drv_usb_device *d);
unsigned
drv_usb_device_port(
	const struct drv_usb_device *d);
enum drv_usb_speed
drv_usb_device_speed(
	const struct drv_usb_device *d);
enum drv_usb_device_state
drv_usb_device_state(
	const struct drv_usb_device *d);
const struct drv_usb_device_descriptor *
drv_usb_device_descriptor(
	const struct drv_usb_device *d);
/* HCD teardown barrier: nonzero while an accepted URB is still owned by the
 * controller or is between HCD dequeue and USB-core terminal publication. */
unsigned
drv_usb_device_hcd_urb_count(
	const struct drv_usb_device *d);
/* Public HCD behavior only; callers must not inspect an opaque controller's
 * name, ops table, or private data to infer concurrency support. */
unsigned
drv_usb_device_hcd_capabilities(
	const struct drv_usb_device *d);
/* Host-controller-private association.  USB lifecycle ownership, not this
 * raw value, keeps the associated object alive. */
uintptr_t
drv_usb_device_hcd_data(
	const struct drv_usb_device *d,
	unsigned n);
int
drv_usb_device_set_hcd_data(
	struct drv_usb_device *d,
	unsigned n,
	uintptr_t value);
struct drv_dma_device *
drv_usb_device_dma(
	struct drv_usb_device *d);
int
drv_usb_device_reset(
	struct drv_usb_device *d);
/* Select by bConfigurationValue; zero returns the device to Address state. */
int
drv_usb_device_set_configuration(
	struct drv_usb_device *d,
	unsigned configuration_value);
/* language_id zero discovers the first advertised LANGID.  The result is
 * bounded, NUL-terminated UTF-8. */
int
drv_usb_device_get_string(
	struct drv_usb_device *d,
	unsigned string_index,
	unsigned language_id,
	char *buffer,
	size_t capacity);
int
drv_usb_control(
	struct drv_usb_device *d,
	uint8_t rt,
	uint8_t r,
	uint16_t v,
	uint16_t i,
	void *b,
	size_t n,
	unsigned t,
	size_t *a);

/*
 * Configuration and interface descriptor access.
 */
const struct drv_usb_configuration_descriptor *
drv_usb_configuration_descriptor(
	const struct drv_usb_configuration *c);
unsigned
drv_usb_device_configuration_count(
	const struct drv_usb_device *d);
struct drv_usb_configuration *
drv_usb_device_configuration(
	struct drv_usb_device *d,
	unsigned i);
struct drv_usb_configuration *
drv_usb_device_active_configuration(
	struct drv_usb_device *d);
const void *
drv_usb_configuration_raw_descriptors(
	const struct drv_usb_configuration *c,
	size_t *length);
unsigned
drv_usb_configuration_interface_count(
	const struct drv_usb_configuration *c);
struct drv_usb_interface *
drv_usb_configuration_interface(
	struct drv_usb_configuration *c,
	unsigned index);
struct drv_usb_interface *
drv_usb_configuration_find_interface(
	struct drv_usb_configuration *c,
	unsigned interface_number);
unsigned
drv_usb_configuration_iad_count(
	const struct drv_usb_configuration *c);
const struct drv_usb_interface_association_descriptor *
drv_usb_configuration_iad(
	const struct drv_usb_configuration *c,
	unsigned index);
struct drv_usb_device *
drv_usb_interface_device(
	const struct drv_usb_interface *i);
const struct drv_usb_interface_descriptor *
drv_usb_interface_descriptor(
	const struct drv_usb_interface *i);
unsigned
drv_usb_interface_number(
	const struct drv_usb_interface *i);
unsigned
drv_usb_interface_alternate_count(
	const struct drv_usb_interface *i);
const struct drv_usb_host_interface *
drv_usb_interface_active_alternate(
	const struct drv_usb_interface *i);
const struct drv_usb_host_interface *
drv_usb_interface_alternate(
	const struct drv_usb_interface *i,
	unsigned index);
const struct drv_usb_host_interface *
drv_usb_interface_find_alternate(
	const struct drv_usb_interface *i,
	unsigned alternate_setting);
int
drv_usb_interface_set_alternate(
	struct drv_usb_interface *i,
	unsigned alternate_setting);
int
drv_usb_interface_claim(
	struct drv_usb_interface *owner,
	struct drv_usb_interface *target);
int
drv_usb_interface_release(
	struct drv_usb_interface *owner,
	struct drv_usb_interface *target);
struct drv_usb_interface *
drv_usb_interface_claimed_by(
	const struct drv_usb_interface *i);
struct drv_usb_driver *
drv_usb_interface_driver(
	const struct drv_usb_interface *i);
void *
drv_usb_interface_driver_data(
	const struct drv_usb_interface *i);
int
drv_usb_interface_set_driver_data(
	struct drv_usb_interface *i,
	void *d);

/*
 * Alternate-setting descriptor access.  Extra descriptors are retained in
 * their original byte representation and order, excluding the interface,
 * endpoint, SuperSpeed companion, and IAD descriptors represented by typed
 * accessors elsewhere in this interface.
 */
const struct drv_usb_interface_descriptor *
drv_usb_host_interface_descriptor(
	const struct drv_usb_host_interface *h);
unsigned
drv_usb_host_interface_endpoint_count(
	const struct drv_usb_host_interface *h);
struct drv_usb_endpoint *
drv_usb_host_interface_endpoint(
	const struct drv_usb_host_interface *h,
	unsigned index);
unsigned
drv_usb_host_interface_extra_count(
	const struct drv_usb_host_interface *h);
int
drv_usb_host_interface_extra(
	const struct drv_usb_host_interface *h,
	unsigned index,
	const void **descriptor,
	size_t *length);

/*
 * Endpoint discovery and properties.
 */
unsigned
drv_usb_interface_endpoint_count(
	const struct drv_usb_interface *i);
struct drv_usb_endpoint *
drv_usb_interface_endpoint(
	struct drv_usb_interface *i,
	unsigned n);
struct drv_usb_endpoint *
drv_usb_interface_find_endpoint(
	struct drv_usb_interface *i,
	enum drv_usb_transfer_type t,
	uint8_t dir,
	struct drv_usb_endpoint *after);
struct drv_usb_device *
drv_usb_endpoint_device(
	const struct drv_usb_endpoint *endpoint);
const struct drv_usb_endpoint_descriptor *
drv_usb_endpoint_descriptor(
	const struct drv_usb_endpoint *e);
enum drv_usb_transfer_type
drv_usb_endpoint_type(
	const struct drv_usb_endpoint *e);
uint8_t
drv_usb_endpoint_address(
	const struct drv_usb_endpoint *e);
uint16_t
drv_usb_endpoint_max_packet_size(
	const struct drv_usb_endpoint *e);
uint8_t
drv_usb_endpoint_maximum_burst(
	const struct drv_usb_endpoint *e);
const struct drv_usb_superspeed_endpoint_companion_descriptor *
drv_usb_endpoint_superspeed_companion(
	const struct drv_usb_endpoint *e);
bool
drv_usb_endpoint_is_input(
	const struct drv_usb_endpoint *e);
uintptr_t
drv_usb_endpoint_hcd_data(
	const struct drv_usb_endpoint *e,
	unsigned n);
int
drv_usb_endpoint_set_hcd_data(
	struct drv_usb_endpoint *e,
	unsigned n,
	uintptr_t value);

/*
 * Asynchronous USB Request Block (URB) allocation and submission.
 */
struct drv_usb_urb *
drv_usb_urb_alloc(
	struct drv_usb_device *d,
	struct drv_usb_endpoint *e,
	unsigned iso_count);
void
drv_usb_urb_free(
	struct drv_usb_urb *u);
int
drv_usb_urb_setup(
	struct drv_usb_urb *u,
	void *b,
	size_t n,
	unsigned f,
	unsigned t,
	drv_usb_urb_callback_t cb,
	void *a);
int
drv_usb_urb_setup_control(
	struct drv_usb_urb *u,
	const struct drv_usb_control_request *r,
	void *b,
	size_t n,
	unsigned t,
	drv_usb_urb_callback_t cb,
	void *a);
int
drv_usb_urb_setup_control_flags(
	struct drv_usb_urb *u,
	const struct drv_usb_control_request *r,
	void *b,
	size_t n,
	unsigned f,
	unsigned t,
	drv_usb_urb_callback_t cb,
	void *a);
int
drv_usb_urb_setup_isochronous(
	struct drv_usb_urb *u,
	struct drv_usb_iso_packet *p,
	unsigned n);
int
drv_usb_urb_submit(
	struct drv_usb_urb *u);
int
drv_usb_urb_cancel(
	struct drv_usb_urb *u);
int
drv_usb_urb_wait(
	struct drv_usb_urb *u);
/* Join an asynchronous URB without initiating cancellation.  Success means
 * it is terminal and HCD ownership was dropped after any callback returned.
 * timeout_ms zero waits indefinitely.  A timed-out caller retains the whole
 * URB/callback graph and may retry.  A callback must not drain its own URB. */
int
drv_usb_urb_drain(
	struct drv_usb_urb *u,
	unsigned timeout_ms);
/* Synchronous reusable URBs have no callback.  In addition to a terminal
 * status, wait until the HCD has dropped its private ownership so the caller
 * can immediately call drv_usb_urb_setup*() on the same object.  If a timeout
 * cancellation cannot retire the HCD request immediately, this ownership
 * barrier may extend past the requested transfer timeout. */
int
drv_usb_urb_wait_reusable(
	struct drv_usb_urb *u);
enum drv_usb_urb_status
drv_usb_urb_status(
	const struct drv_usb_urb *u);
size_t
drv_usb_urb_actual_length(
	const struct drv_usb_urb *u);
void *
drv_usb_urb_buffer(
	const struct drv_usb_urb *u);
size_t
drv_usb_urb_length(
	const struct drv_usb_urb *u);
unsigned
drv_usb_urb_flags(
	const struct drv_usb_urb *u);
const struct drv_usb_control_request *
drv_usb_urb_control_request(
	const struct drv_usb_urb *u);
void *
drv_usb_urb_hcd_data(
	const struct drv_usb_urb *u);
int
drv_usb_urb_set_hcd_data(
	struct drv_usb_urb *u,
	void *d);
struct drv_usb_device *
drv_usb_urb_device(
	const struct drv_usb_urb *u);
struct drv_usb_endpoint *
drv_usb_urb_endpoint(
	const struct drv_usb_urb *u);

/*
 * Convenient synchronous transfers implemented on top of URBs.
 */
int
drv_usb_bulk(
	struct drv_usb_device *d,
	struct drv_usb_endpoint *e,
	void *b,
	size_t n,
	unsigned t,
	size_t *a);
int
drv_usb_interrupt(
	struct drv_usb_device *d,
	struct drv_usb_endpoint *e,
	void *b,
	size_t n,
	unsigned t,
	size_t *a);

/*
 * Built-in interface-driver registry and matching.
 */
int
drv_usb_driver_register(
	struct drv_usb_driver *d);
int
drv_usb_driver_unregister(
	struct drv_usb_driver *d);
int
drv_usb_interface_probe(
	struct drv_usb_interface *i);
int
drv_usb_interface_detach(
	struct drv_usb_interface *i,
	unsigned f);
int
drv_usb_id_match(
	const struct drv_usb_id *id,
	const struct drv_usb_interface *i);
const struct drv_usb_id *
drv_usb_driver_find_id(
	const struct drv_usb_driver *d,
	const struct drv_usb_interface *i);

void
drv_usb_dump(void);

#endif
