/*
 * Generic USB host bus and built-in driver interface
 * Copyright (C) 2026 Awe Morris
 *
 * This interface references and uses API concepts, terminology, object
 * hierarchy, and host-side driver conventions from the Linux USB
 * subsystem, including its URB model.  The contracts are adapted for
 * zedBSD; no Linux implementation source code is included in this file.
 * Host-controller-specific objects such as UHCI TD/QH, EHCI qTD/QH, and
 * xHCI TRB/rings must remain private to their respective HCDs.
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_USB_H
#define ZEDBSD_DRIVERS_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/dma.h>

#define DRV_USB_ANY_ID             ((uint16_t)0xffffU)
#define DRV_USB_ANY_CLASS          ((uint8_t)0xffU)
#define DRV_USB_MAX_ADDRESS        127U
#define DRV_USB_MAX_ENDPOINTS      32U
#define DRV_USB_MAX_INTERFACES     32U

#define DRV_USB_DIR_OUT            0x00U
#define DRV_USB_DIR_IN             0x80U

#define DRV_USB_REQUEST_STANDARD   0x00U
#define DRV_USB_REQUEST_CLASS      0x20U
#define DRV_USB_REQUEST_VENDOR     0x40U
#define DRV_USB_RECIP_DEVICE       0x00U
#define DRV_USB_RECIP_INTERFACE    0x01U
#define DRV_USB_RECIP_ENDPOINT     0x02U
#define DRV_USB_RECIP_OTHER        0x03U

#define DRV_USB_DESCRIPTOR_DEVICE        1U
#define DRV_USB_DESCRIPTOR_CONFIGURATION 2U
#define DRV_USB_DESCRIPTOR_STRING        3U
#define DRV_USB_DESCRIPTOR_INTERFACE     4U
#define DRV_USB_DESCRIPTOR_ENDPOINT      5U
#define DRV_USB_DESCRIPTOR_BOS           15U

#define DRV_USB_ID_VENDOR          (1U << 0)
#define DRV_USB_ID_PRODUCT         (1U << 1)
#define DRV_USB_ID_RELEASE_RANGE   (1U << 2)
#define DRV_USB_ID_DEVICE_CLASS    (1U << 3)
#define DRV_USB_ID_DEVICE_SUBCLASS (1U << 4)
#define DRV_USB_ID_DEVICE_PROTOCOL (1U << 5)
#define DRV_USB_ID_IF_CLASS        (1U << 6)
#define DRV_USB_ID_IF_SUBCLASS     (1U << 7)
#define DRV_USB_ID_IF_PROTOCOL     (1U << 8)
#define DRV_USB_ID_IF_NUMBER       (1U << 9)

#define DRV_USB_URB_SHORT_OK       (1U << 0)
#define DRV_USB_URB_ZERO_PACKET    (1U << 1)
#define DRV_USB_URB_NO_DMA_MAP     (1U << 2)
#define DRV_USB_URB_ISO_ASAP       (1U << 3)

#define DRV_USB_DETACH_FORCE       (1U << 0)
#define DRV_USB_DETACH_QUIET       (1U << 1)

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

struct drv_usb_endpoint_descriptor {
	uint8_t length;
	uint8_t descriptor_type;
	uint8_t address;
	uint8_t attributes;
	uint16_t maximum_packet_size;
	uint8_t interval;
} __attribute__((packed));

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

typedef void (*drv_usb_urb_callback_t)(struct drv_usb_urb *, void *);
typedef int (*drv_usb_bus_iterator_t)(struct drv_usb_bus *, void *);
typedef int (*drv_usb_device_iterator_t)(struct drv_usb_device *, void *);
typedef int (*drv_usb_interface_iterator_t)(struct drv_usb_interface *, void *);

/*
 * A function driver binds to one interface in the active configuration.
 * A composite device may therefore be served by several different drivers.
 */
struct drv_usb_driver {
	const char *name;
	const struct drv_usb_id *ids;
	size_t id_count;
	int (*match)(struct drv_usb_interface *, const struct drv_usb_id *);
	int (*attach)(struct drv_usb_interface *, const struct drv_usb_id *);
	int (*detach)(struct drv_usb_interface *, unsigned);
	int (*suspend)(struct drv_usb_interface *);
	int (*resume)(struct drv_usb_interface *);
	void (*shutdown)(struct drv_usb_interface *);
	uintptr_t private_data[4];
};

/*
 * Host controller contract.  The USB core owns enumeration, descriptor
 * parsing, address assignment, driver binding, timeouts, and completion
 * delivery.  The HCD owns hardware scheduling and root-hub operations.
 */
struct drv_usb_hcd_ops {
	int (*start)(struct drv_usb_hcd *);
	void (*stop)(struct drv_usb_hcd *);
	int (*urb_enqueue)(struct drv_usb_hcd *,
		struct drv_usb_urb *);
	int (*urb_dequeue)(struct drv_usb_hcd *,
		struct drv_usb_urb *);
	int (*endpoint_enable)(struct drv_usb_hcd *,
		struct drv_usb_endpoint *);
	void (*endpoint_disable)(struct drv_usb_hcd *,
		struct drv_usb_endpoint *);
	uint32_t (*frame_number)(struct drv_usb_hcd *);
	int (*root_hub_status)(struct drv_usb_hcd *, void *, size_t,
		size_t *);
	int (*root_hub_control)(struct drv_usb_hcd *,
		const struct drv_usb_control_request *, void *, size_t,
		size_t *);
};

struct drv_usb_hcd {
	const char *name;
	const struct drv_usb_hcd_ops *ops;
	struct drv_dma_device *dma;
	unsigned root_port_count;
	uintptr_t private_data[6];
};

/* Core and host-controller lifecycle. */
int drv_usb_init(void);
void drv_usb_shutdown(void);
int drv_usb_hcd_register(struct drv_usb_hcd *, struct drv_usb_bus **);
int drv_usb_hcd_unregister(struct drv_usb_hcd *);
void drv_usb_hcd_root_hub_changed(struct drv_usb_hcd *);
void drv_usb_hcd_complete(struct drv_usb_hcd *, struct drv_usb_urb *,
	enum drv_usb_urb_status, size_t);

/* Bus, device, and interface enumeration. */
int drv_usb_foreach_bus(drv_usb_bus_iterator_t, void *);
int drv_usb_foreach_device(drv_usb_device_iterator_t, void *);
int drv_usb_bus_foreach_device(struct drv_usb_bus *,
	drv_usb_device_iterator_t, void *);
int drv_usb_device_foreach_interface(struct drv_usb_device *,
	drv_usb_interface_iterator_t, void *);
struct drv_usb_device *drv_usb_find_device(unsigned, unsigned);
unsigned drv_usb_bus_number(const struct drv_usb_bus *);
struct drv_usb_hcd *drv_usb_bus_hcd(const struct drv_usb_bus *);
struct drv_usb_device *drv_usb_bus_root_hub(const struct drv_usb_bus *);

/* Device identity, topology, state, and standard requests. */
struct drv_usb_bus *drv_usb_device_bus(const struct drv_usb_device *);
struct drv_usb_device *drv_usb_device_parent(const struct drv_usb_device *);
unsigned drv_usb_device_address(const struct drv_usb_device *);
unsigned drv_usb_device_port(const struct drv_usb_device *);
enum drv_usb_speed drv_usb_device_speed(const struct drv_usb_device *);
enum drv_usb_device_state drv_usb_device_state(const struct drv_usb_device *);
const struct drv_usb_device_descriptor *drv_usb_device_descriptor(
	const struct drv_usb_device *);
struct drv_dma_device *drv_usb_device_dma(struct drv_usb_device *);
int drv_usb_device_reset(struct drv_usb_device *);
int drv_usb_device_set_configuration(struct drv_usb_device *, unsigned);
int drv_usb_device_get_string(struct drv_usb_device *, unsigned, unsigned,
	char *, size_t);
int drv_usb_control(struct drv_usb_device *, uint8_t, uint8_t, uint16_t,
	uint16_t, void *, size_t, unsigned, size_t *);

/* Configuration and interface descriptor access. */
const struct drv_usb_configuration_descriptor *
drv_usb_configuration_descriptor(const struct drv_usb_configuration *);
unsigned drv_usb_device_configuration_count(const struct drv_usb_device *);
struct drv_usb_configuration *drv_usb_device_configuration(
	struct drv_usb_device *, unsigned);
struct drv_usb_configuration *drv_usb_device_active_configuration(
	struct drv_usb_device *);
struct drv_usb_device *drv_usb_interface_device(
	const struct drv_usb_interface *);
const struct drv_usb_interface_descriptor *drv_usb_interface_descriptor(
	const struct drv_usb_interface *);
unsigned drv_usb_interface_number(const struct drv_usb_interface *);
unsigned drv_usb_interface_alternate_count(const struct drv_usb_interface *);
int drv_usb_interface_set_alternate(struct drv_usb_interface *, unsigned);
struct drv_usb_driver *drv_usb_interface_driver(
	const struct drv_usb_interface *);
void *drv_usb_interface_driver_data(const struct drv_usb_interface *);
int drv_usb_interface_set_driver_data(struct drv_usb_interface *, void *);

/* Endpoint discovery and properties. */
unsigned drv_usb_interface_endpoint_count(const struct drv_usb_interface *);
struct drv_usb_endpoint *drv_usb_interface_endpoint(
	struct drv_usb_interface *, unsigned);
struct drv_usb_endpoint *drv_usb_interface_find_endpoint(
	struct drv_usb_interface *, enum drv_usb_transfer_type, uint8_t,
	struct drv_usb_endpoint *);
const struct drv_usb_endpoint_descriptor *drv_usb_endpoint_descriptor(
	const struct drv_usb_endpoint *);
enum drv_usb_transfer_type drv_usb_endpoint_type(
	const struct drv_usb_endpoint *);
uint8_t drv_usb_endpoint_address(const struct drv_usb_endpoint *);
uint16_t drv_usb_endpoint_max_packet_size(const struct drv_usb_endpoint *);
bool drv_usb_endpoint_is_input(const struct drv_usb_endpoint *);

/* Asynchronous USB Request Block (URB) allocation and submission. */
struct drv_usb_urb *drv_usb_urb_alloc(struct drv_usb_device *,
	struct drv_usb_endpoint *, unsigned);
void drv_usb_urb_free(struct drv_usb_urb *);
int drv_usb_urb_setup(struct drv_usb_urb *, void *, size_t,
	unsigned, unsigned, drv_usb_urb_callback_t, void *);
int drv_usb_urb_setup_control(struct drv_usb_urb *,
	const struct drv_usb_control_request *, void *, size_t, unsigned,
	drv_usb_urb_callback_t, void *);
int drv_usb_urb_setup_isochronous(struct drv_usb_urb *,
	struct drv_usb_iso_packet *, unsigned);
int drv_usb_urb_submit(struct drv_usb_urb *);
int drv_usb_urb_cancel(struct drv_usb_urb *);
int drv_usb_urb_wait(struct drv_usb_urb *);
enum drv_usb_urb_status drv_usb_urb_status(
	const struct drv_usb_urb *);
size_t drv_usb_urb_actual_length(const struct drv_usb_urb *);
void *drv_usb_urb_buffer(const struct drv_usb_urb *);
size_t drv_usb_urb_length(const struct drv_usb_urb *);
unsigned drv_usb_urb_flags(const struct drv_usb_urb *);
const struct drv_usb_control_request *drv_usb_urb_control_request(
	const struct drv_usb_urb *);
void *drv_usb_urb_hcd_data(const struct drv_usb_urb *);
int drv_usb_urb_set_hcd_data(struct drv_usb_urb *, void *);
struct drv_usb_device *drv_usb_urb_device(
	const struct drv_usb_urb *);
struct drv_usb_endpoint *drv_usb_urb_endpoint(
	const struct drv_usb_urb *);

/* Convenient synchronous transfers implemented on top of URBs. */
int drv_usb_bulk(struct drv_usb_device *, struct drv_usb_endpoint *, void *,
	size_t, unsigned, size_t *);
int drv_usb_interrupt(struct drv_usb_device *, struct drv_usb_endpoint *,
	void *, size_t, unsigned, size_t *);

/* Built-in interface-driver registry and matching. */
int drv_usb_driver_register(struct drv_usb_driver *);
int drv_usb_driver_unregister(struct drv_usb_driver *);
int drv_usb_interface_probe(struct drv_usb_interface *);
int drv_usb_interface_detach(struct drv_usb_interface *, unsigned);
int drv_usb_id_match(const struct drv_usb_id *,
	const struct drv_usb_interface *);
const struct drv_usb_id *drv_usb_driver_find_id(
	const struct drv_usb_driver *, const struct drv_usb_interface *);

void drv_usb_dump(void);

#endif
