/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * USB host core

 * The driver model and URB terminology follow the Linux USB API.
 * This is an independent implementation and contains no Linux
 * implementation code.
 */

#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/io-stats.h>
#include <kern/sched.h>
#include <string.h>

#define USB_REQ_GET_DESCRIPTOR 6U
#define USB_REQ_CLEAR_FEATURE 1U
#define USB_REQ_SET_ADDRESS 5U
#define USB_REQ_SET_CONFIGURATION 9U
#define USB_REQ_SET_INTERFACE 11U
#define USB_FEATURE_ENDPOINT_HALT 0U
#define USB_ADDRESS_RECOVERY_TICKS 2U
#define USB_RESET_RECOVERY_TICKS 2U
#define USB_CONTROL_TIMEOUT_MS 1000U

#define USB_DEVICE_LIFECYCLE_DISCONNECTING (1U << 31)
#define USB_DEVICE_LIFECYCLE_FINALIZING (1U << 30)
#define USB_DEVICE_LIFECYCLE_URB_MASK ((1U << 30) - 1U)
#define USB_IO_GATE_CLOSED (1U << 31)
#define USB_IO_GATE_COUNT_MASK (USB_IO_GATE_CLOSED - 1U)
#define USB_CONTROL_ADMISSION_NONE 0U
#define USB_CONTROL_ADMISSION_OWNED 1U
#define USB_CONTROL_ADMISSION_EXTERNAL 2U
#define USB_DISCONNECT_BARRIER_RUNNING 1U
#define USB_DISCONNECT_BARRIER_DONE 2U

enum usb_binding_state {
	USB_BINDING_DEAD,
	USB_BINDING_PROBING,
	USB_BINDING_BOUND,
	USB_BINDING_UNBINDING,
	USB_BINDING_DETACH_PENDING
};

struct drv_usb_configuration {
	struct drv_usb_configuration_descriptor descriptor;
	struct drv_usb_device *device;
	struct drv_usb_interface *interfaces;
	struct drv_usb_interface_association_descriptor *iads;
	unsigned interface_count;
	unsigned iad_count;
	uint8_t *raw;
	size_t raw_length;
};

struct drv_usb_endpoint {
	struct drv_usb_interface *interface;
	struct drv_usb_host_interface *alternate;
	struct drv_usb_endpoint_descriptor descriptor;
	struct drv_usb_superspeed_endpoint_companion_descriptor companion;
	enum drv_usb_transfer_type type;
	unsigned companion_valid;
	atomic_uint_t halted;
	uintptr_t hcd_private[4];
};

struct drv_usb_host_interface {
	struct drv_usb_interface *interface;
	struct drv_usb_interface_descriptor descriptor;
	struct drv_usb_endpoint *endpoints;
	unsigned endpoint_count;
	unsigned extra_count;
	size_t raw_offset;
	size_t raw_length;
	struct drv_usb_host_interface *next;
};

struct drv_usb_interface {
	struct drv_usb_device *device;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface_descriptor descriptor;
	struct drv_usb_endpoint *endpoints;
	unsigned endpoint_count;
	unsigned alternate_count;
	struct drv_usb_host_interface *alternates;
	struct drv_usb_host_interface *active_alternate;
	struct drv_usb_interface *claimed_by;
	struct drv_usb_driver *driver;
	void *driver_data;
	atomic_uint_t io_gate;
	atomic_uint_t binding_gate;
	atomic_uint_t binding_submitters;
	atomic_uint_t binding_state;
	struct drv_usb_interface *next;
};

struct drv_usb_device {
	struct drv_usb_bus *bus;
	struct drv_usb_device *parent, *next;
	struct drv_usb_interface *interfaces;
	struct drv_usb_configuration *configurations, *active_configuration;
	struct drv_usb_device_descriptor descriptor;
	unsigned configuration_count, address, port;
	uint64_t generation, port_generation;
	enum drv_usb_speed speed;
	enum drv_usb_device_state state;
	unsigned lifecycle;
	unsigned hcd_urb_count;
	unsigned quarantined;
	unsigned report_disconnect;
	atomic_uint_t selection_gate;
	atomic_uint_t control_gate;
	atomic_uint_t control_inflight;
	atomic_uint_t submit_gate;
	atomic_uint_t binding_transactions;
	atomic_uint_t disconnect_barrier;
	struct drv_usb_urb *recovery_urb;
	void *quarantine_buffer;
	uintptr_t hcd_private[4];
	struct drv_usb_endpoint endpoint0;
};

struct drv_usb_bus {
	struct usb_port_state {
		unsigned observed;
		unsigned connected;
		unsigned enabled;
		uint64_t connection_generation;
	} *ports;
	unsigned number;
	unsigned stopping;
	/*
	 * Protected by usb_topology_gate.  HCD quiesce may join a root worker
	 * which is itself waiting for that gate, so shutdown/unregister claim
	 * the bus here and invoke the blocking HCD barrier with the gate
	 * released.
	 */
	unsigned lifecycle_claimed;
	unsigned shutdown_processed;
	uint64_t shutdown_attempt_generation;
	struct drv_usb_hcd *hcd;
	struct drv_usb_device *root_hub, *devices;
	struct drv_usb_bus *next;
	uint8_t address_used[DRV_USB_MAX_ADDRESS + 1U];
};

struct usb_submit_commit {
	struct drv_usb_device *device;
	struct drv_usb_interface *binding_owner;
	atomic_uint_t finished;
};

struct drv_usb_urb {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	void *buffer;
	void *sync_buffer, *sync_client;
	size_t sync_capacity;
	unsigned sync_shared;
	void *transfer_reservation;
	size_t transfer_capacity;
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
	unsigned control_admitted;
	unsigned control_counted;
	struct drv_usb_interface *io_interface;
	struct drv_usb_interface *binding_owner;
	struct usb_submit_commit *submit_commit;
	unsigned submit_commit_pending;
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
static uint64_t usb_shutdown_generation;
static uint64_t usb_device_generation;
static bool usb_initialized;
static atomic_uint_t usb_topology_gate;

/*
 * Forward declaration
 */
static void usb_topology_lock(void);
static void usb_topology_unlock(void);
static void device_begin_disconnect(struct drv_usb_device *device);
static void io_gate_close(atomic_uint_t *gate);
static int detach_interfaces(struct drv_usb_device *device);
static int shutdown_interfaces(struct drv_usb_device *device, int *retain);
static int interface_binding_quiesce(struct drv_usb_interface *interface, unsigned expected_state);
static int interface_binding_detach(struct drv_usb_interface *interface, unsigned flags, unsigned expected_state);
static void interface_binding_clear(struct drv_usb_interface *interface);
static struct drv_usb_interface * interface_claim_owner(const struct drv_usb_interface *interface);
static void io_gate_close_wait(atomic_uint_t *gate);
static int io_gate_close_empty(atomic_uint_t *gate);
static void interface_publish_claim(struct drv_usb_interface *interface, struct drv_usb_interface *owner);
static void io_gate_open(atomic_uint_t *gate);
static int device_quiesce(struct drv_usb_bus *bus, struct drv_usb_device *device);
static int device_is_quarantined(const struct drv_usb_device *device);
static void device_link(struct drv_usb_bus *bus, struct drv_usb_device *device);
static int device_linked(struct drv_usb_bus *bus, struct drv_usb_device *device);
static struct drv_usb_device *allocate_root_hub(struct drv_usb_bus *bus);
static struct drv_usb_bus *find_hcd_bus(struct drv_usb_hcd *hcd);
static int root_port_status(struct drv_usb_hcd *hcd, unsigned port, uint32_t *status);
static int root_port_acknowledge_changes(struct drv_usb_hcd *hcd, unsigned port, uint32_t status);
static uint64_t usb_generation_next(uint64_t *generation);
static struct drv_usb_device *find_port_device(struct drv_usb_bus *bus, unsigned port);
static int device_is_disconnecting(const struct drv_usb_device *device);
static int destroy_device(struct drv_usb_bus *bus, struct drv_usb_device *device);
static int device_disable_active_endpoints(struct drv_usb_device *device);
static struct drv_usb_configuration * device_active_configuration(const struct drv_usb_device *device);
static int configuration_disable_endpoints(struct drv_usb_configuration *configuration);
static int host_interface_disable(struct drv_usb_host_interface *alternate);
static void device_quarantine_selection(struct drv_usb_device *device, const char *stage, int error);
static struct drv_usb_host_interface * interface_active_alternate(const struct drv_usb_interface *interface);
static int host_interface_enable(struct drv_usb_host_interface *alternate);
static int device_release(struct drv_usb_bus *bus, struct drv_usb_device *device);
static void device_finalize(struct drv_usb_bus *bus, struct drv_usb_device *device);
static void free_configurations(struct drv_usb_device *device);
static void free_configuration(struct drv_usb_configuration *configuration);
static void device_publish_configuration(struct drv_usb_device *device, struct drv_usb_configuration *configuration);
static int legacy_root_port_reset(struct drv_usb_hcd *hcd, unsigned port);
static void usb_delay_ticks(uint64_t count);
static int enumerate_port(struct drv_usb_bus *bus, unsigned port, uint32_t status);
static int ep0_packet_size(enum drv_usb_speed speed, uint8_t encoded, unsigned *packet);
static int allocate_address(struct drv_usb_bus *bus);
static int enumerate_configuration(struct drv_usb_device *device, unsigned index, struct drv_usb_configuration *configuration);
static int parse_configuration(struct drv_usb_configuration *configuration, struct drv_usb_device *device, uint8_t *raw, size_t length);
static int configuration_iads_prepare(struct drv_usb_configuration *configuration, const uint8_t *raw, size_t length);
static struct drv_usb_interface * configuration_find_interface(struct drv_usb_configuration *configuration, unsigned number);
static struct drv_usb_host_interface * interface_find_alternate(struct drv_usb_interface *interface, unsigned setting);
static void interface_publish_alternate(struct drv_usb_interface *interface, struct drv_usb_host_interface *alternate);
static int configuration_endpoint_addresses_validate( const struct drv_usb_configuration *configuration);
static int configuration_iads_validate(struct drv_usb_configuration *configuration);
static struct drv_usb_configuration * device_preferred_configuration(struct drv_usb_device *device, int *selected_score);
static int interface_registered_driver_score(struct drv_usb_interface *interface);
static int interface_probe_internal(struct drv_usb_interface *interface, struct drv_usb_driver **matched_driver);
static int device_binding_enter(struct drv_usb_device *device);
static int io_gate_enter(atomic_uint_t *gate);
static void io_gate_exit(atomic_uint_t *gate);
static void device_binding_exit(struct drv_usb_device *device);
static void interface_report_probe(struct drv_usb_interface *interface, int error, struct drv_usb_driver *matched_driver);
static int urb_publish_terminal(struct drv_usb_urb *urb, enum drv_usb_urb_status status, size_t actual);
static void submit_commit_finish(struct drv_usb_urb *urb, struct usb_submit_commit *commit);
static void binding_submitter_put(struct drv_usb_interface *owner);
static void endpoint_publish_halted(struct drv_usb_device *device, struct drv_usb_endpoint *endpoint, unsigned halted);
static int endpoint_uses_halt(const struct drv_usb_device *device, const struct drv_usb_endpoint *endpoint);
static void urb_hcd_put(struct drv_usb_urb *urb);
static void urb_admission_put(struct drv_usb_urb *urb);
static void device_control_unlock(struct drv_usb_device *device);
static void urb_put(struct drv_usb_urb *urb);
static void device_urb_put(struct drv_usb_device *device);
static int configuration_effective_owner(struct drv_usb_configuration *configuration, struct drv_usb_interface **effective_owner);
static struct drv_usb_interface * interface_binding_owner(struct drv_usb_interface *interface);
static int configuration_close_io(struct drv_usb_configuration *configuration, struct drv_usb_interface **closed, unsigned *closed_count);
static int device_control_try_lock(struct drv_usb_device *device);
static int device_reset_connection_check(struct drv_usb_bus *bus, struct drv_usb_device *device, uint64_t device_generation, uint64_t port_generation);
static int configuration_enable_endpoints(struct drv_usb_configuration *configuration);
static void device_quarantine_recovery(struct drv_usb_device *device, const char *stage, int error);
static int usb_control_locked(struct drv_usb_device *device, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, void *buffer, size_t length, unsigned timeout_ms, size_t *actual);
static int configuration_restore(struct drv_usb_device *device, struct drv_usb_configuration *configuration);
static int configuration_reset_endpoints(struct drv_usb_configuration *configuration);
static void configuration_open_io(struct drv_usb_interface **closed, unsigned closed_count);
static int configuration_has_owners(const struct drv_usb_configuration *configuration);
static void configuration_select_defaults(struct drv_usb_configuration *configuration);
static int usb_string_descriptor(struct drv_usb_device *device, uint8_t index, uint16_t language, uint8_t *descriptor, size_t *descriptor_length);
static int utf8_append(char *buffer, size_t capacity, size_t *used, uint32_t codepoint);
static int device_urb_get(struct drv_usb_device *device);
static int endpoint_retained_by_device(const struct drv_usb_device *device, const struct drv_usb_endpoint *endpoint);
static int endpoint_is_halted(const struct drv_usb_device *device, const struct drv_usb_endpoint *endpoint);
static int urb_hcd_get(struct drv_usb_urb *urb);
static int urb_admission_get(struct drv_usb_urb *urb, struct drv_usb_interface **submitting_owner);
static int binding_admission_enter(struct drv_usb_urb *urb, struct drv_usb_interface *interface, struct drv_usb_interface **submitting_owner);
static int urb_cancel_to(struct drv_usb_urb *u, enum drv_usb_urb_status terminal);
static int device_control_lock(struct drv_usb_device *device, unsigned timeout_ms);
static int sync_data(struct drv_usb_device *device, struct drv_usb_endpoint *endpoint, void *buffer, size_t length, unsigned timeout_ms, size_t *actual);
static int host_interface_reset_endpoints(struct drv_usb_host_interface *alternate);
static int endpoint_binding_pin(struct drv_usb_interface *interface, struct drv_usb_interface **pinned_owner);
static int endpoint_clear_halt_request(struct drv_usb_device *device, struct drv_usb_endpoint *endpoint, unsigned *accepted);
static void endpoint_binding_unpin(struct drv_usb_interface *owner);

/*
 * Brings the USB subsystem into service.
 */
int
drv_usb_init(
	void)
{
	/* Handles the usb initialized condition. */
	if (usb_initialized)
		return EALREADY;
	usb_buses = NULL;
	usb_drivers = NULL;
	next_bus_number = 0;
	usb_shutdown_generation = 0;
	usb_device_generation = 0;
	atomic_store_release(&usb_topology_gate, 0U);
	usb_initialized = true;
	hal_printf("usb: URB completion contract q009-release-acquire-v1\n");

	/* Succeeded. */
	return 0;
}

/*
 * Takes the USB subsystem out of service.
 */
void
drv_usb_shutdown(
	void)
{
	struct drv_usb_bus *bus;
	struct drv_usb_device *device;
	int error, retain;
	uint64_t generation;

	usb_topology_lock();

	/* Handles the generation condition. */
	generation = ++usb_shutdown_generation;
	if (generation == 0)
		generation = ++usb_shutdown_generation;
	usb_topology_unlock();
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		retain = 0;

		usb_topology_lock();
		/* Process each linked entry. */
		for (bus = usb_buses; bus != NULL; bus = bus->next) {
			/* Handles the bus condition. */
			if (!bus->shutdown_processed &&
			    !bus->lifecycle_claimed &&
			    bus->shutdown_attempt_generation != generation)
				break;
		}

		/* Handles the bus availability. */
		if (bus == NULL) {
			usb_topology_unlock();

			/* Returns the computed result. */
			return;
		}

		bus->lifecycle_claimed = 1U;
		bus->shutdown_attempt_generation = generation;

		/*
		 * stopping closes admission, but is not evidence that the HCD
		 * has crossed its checked DMA-stop boundary.  In particular,
		 * unregister leaves it set when a worker join or quiesce
		 * attempt fails.  A terminal shutdown must retry that same
		 * checked sequence.
		 */
		hal_atomic_store_release(&bus->stopping, 1U);

		/*
		 * Stop admission first, then let every class driver disconnect
		 * and cancel/drain its own work while the HCD is still
		 * operational.
		 */
		for (device = bus->devices; device != NULL;
		     device = device->next)
			device_begin_disconnect(device);
		/* Process each linked entry. */
		for (device = bus->devices; device != NULL;
		     device = device->next) {
			/* Checks the operation status. */
			error = shutdown_interfaces(device, &retain);
			if (error != 0) {
				hal_printf("usb%u: device %u driver shutdown "
					   "failed (%d); class resources "
					   "retained\n",
					   bus->number, device->address, error);
				retain = 1;
			}
		}

		/*
		 * Driver teardown is the first ownership boundary.  Device/HCD
		 * quiesce follows only after no interface can submit more work.
		 */
		for (device = bus->devices; device != NULL;
		     device = device->next) {
			/* Checks the operation status. */
			error = device_quiesce(bus, device);
			if (error != 0)
				retain = 1;
		}

		/*
		 * A runtime root worker can be blocked trying to enter the
		 * topology gate.  Release it before the HCD joins that worker.
		 * The bus claim prevents concurrent unregister/free while the
		 * pointer is borrowed.
		 */
		usb_topology_unlock();

		/* Checks the operation status. */
		error = bus->hcd->ops->quiesce == NULL
				? 0
				: bus->hcd->ops->quiesce(bus->hcd);
		if (error != 0) {
			hal_printf("usb%u: host controller stop failed\n",
				   bus->number);
			usb_topology_lock();

			/* Handles the bus condition. */
			if (!bus->lifecycle_claimed)
				__builtin_trap();
			bus->lifecycle_claimed = 0U;
			usb_topology_unlock();

			/*
			 * Leave shutdown_processed clear so a later shutdown
			 * request can retry.  This invocation marks it
			 * attempted and continues through every other
			 * controller without spinning on this one.
			 */
			continue;
		}

		/*
		 * A failed class/device teardown keeps callback-visible HCD
		 * memory, but the checked HCD quiesce above still stops DMA
		 * before reboot.
		 */
		if (retain && bus->hcd->ops->quiesce != NULL) {
			hal_printf("usb%u: host controller quiesced; resources "
				   "retained\n",
				   bus->number);
		}

		/* Handles the stop availability. */
		if (!retain && bus->hcd->ops->stop != NULL)
			bus->hcd->ops->stop(bus->hcd);
		usb_topology_lock();
		bus->shutdown_processed = 1U;

		/* Handles the bus condition. */
		if (!bus->lifecycle_claimed)
			__builtin_trap();
		bus->lifecycle_claimed = 0U;
		usb_topology_unlock();
	}
}

/*
 * Publishes a host controller and its root hub as a bus.
 */
int
drv_usb_hcd_register(
	struct drv_usb_hcd *hcd,
	struct drv_usb_bus **result)
{
	struct drv_usb_bus *bus;
	int error;

	/* Handles the hcd availability. */
	if (!usb_initialized || hcd == NULL || hcd->name == NULL ||
	    hcd->ops == NULL || hcd->ops->start == NULL ||
	    hcd->ops->stop == NULL || hcd->ops->urb_enqueue == NULL ||
	    hcd->ops->urb_dequeue == NULL || hcd->ops->endpoint_reset == NULL ||
	    ((hcd->ops->endpoint_enable == NULL) !=
	     (hcd->ops->endpoint_disable == NULL)) ||
	    result == NULL ||
	    (hcd->capabilities & ~(DRV_USB_HCD_CAP_CONCURRENT_URBS |
				   DRV_USB_HCD_CAP_TRANSFER_RESERVE |
				   DRV_USB_HCD_CAP_SHARED_STAGING)) != 0 ||
	    ((hcd->ops->urb_reserve == NULL) !=
	     (hcd->ops->urb_unreserve == NULL)) ||
	    (((hcd->capabilities & DRV_USB_HCD_CAP_SHARED_STAGING) != 0) !=
	     (hcd->ops->urb_reserve_buffer != NULL)) ||
	    ((hcd->capabilities & DRV_USB_HCD_CAP_SHARED_STAGING) != 0 &&
	     (hcd->capabilities & DRV_USB_HCD_CAP_TRANSFER_RESERVE) == 0) ||
	    (((hcd->capabilities & DRV_USB_HCD_CAP_TRANSFER_RESERVE) != 0) !=
	     (hcd->ops->urb_reserve != NULL))) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the bus availability. */
	bus = hal_malloc(sizeof(*bus));
	if (bus == NULL)
		return ENOMEM;
	memset(bus, 0, sizeof(*bus));
	bus->number = next_bus_number++;
	bus->hcd = hcd;
	bus->address_used[0] = 1;

	/* Handles the hcd condition. */
	if (hcd->root_port_count == UINT_MAX) {
		hal_free(bus);

		/* Failed. */
		return EOVERFLOW;
	}

	bus->ports = hal_malloc(((size_t)hcd->root_port_count + 1U) *
				sizeof(*bus->ports));

	/* Handles the ports availability. */
	if (bus->ports == NULL) {
		hal_free(bus);

		/* Failed. */
		return ENOMEM;
	}

	memset(bus->ports, 0,
	       ((size_t)hcd->root_port_count + 1U) * sizeof(*bus->ports));
	bus->root_hub = allocate_root_hub(bus);

	/* Handles the root hub availability. */
	if (bus->root_hub == NULL) {
		hal_free(bus->ports);
		hal_free(bus);

		/* Failed. */
		return ENOMEM;
	}

	/* Checks the operation status. */
	error = hcd->ops->start(hcd);
	if (error != 0) {
		hal_free(bus->root_hub);
		hal_free(bus->ports);
		hal_free(bus);

		/* Failed. */
		return error;
	}

	usb_topology_lock();
	bus->next = usb_buses;
	usb_buses = bus;
	usb_topology_unlock();
	*result = bus;
	hal_printf("usb%u: %s, %u root ports\n", bus->number, hcd->name,
		   hcd->root_port_count);

	/* Succeeded. */
	return 0;
}

/*
 * Takes a host controller and its bus back out of service.
 */
int
drv_usb_hcd_unregister(
	struct drv_usb_hcd *hcd)
{
	struct drv_usb_bus **link, *bus;
	unsigned state;
	int error;

	/* Handles the hcd availability. */
	if (hcd == NULL)
		return EINVAL;
	usb_topology_lock();
	/* Process each linked entry. */
	for (link = &usb_buses; (bus = *link) != NULL; link = &bus->next) {
		/* Handles the bus condition. */
		if (bus->hcd != hcd)
			continue;

		/* Handles the devices availability. */
		if (bus->devices != NULL || bus->lifecycle_claimed) {
			usb_topology_unlock();

			/* Failed. */
			return EBUSY;
		}

		bus->lifecycle_claimed = 1U;
		hal_atomic_store_release(&bus->stopping, 1U);
		device_begin_disconnect(bus->root_hub);

		/*
		 * Root workers enter the topology path in process context.
		 * Never join one while retaining the gate it may be waiting to
		 * acquire.
		 */
		usb_topology_unlock();

		/* Handles the quiesce availability. */
		if (hcd->ops->quiesce != NULL) {
			/* Checks the operation status. */
			error = hcd->ops->quiesce(hcd);
			if (error != 0) {
				usb_topology_lock();

				/* Handles the bus condition. */
				if (!bus->lifecycle_claimed)
					__builtin_trap();
				bus->lifecycle_claimed = 0U;
				usb_topology_unlock();

				/* Failed. */
				return error;
			}
		}

		usb_topology_lock();
		state = hal_atomic_load_acquire(&bus->root_hub->lifecycle);
		/* Continue until the operation reaches a terminal state. */
		for (;;) {
			/* Handles the state condition. */
			if ((state & USB_DEVICE_LIFECYCLE_URB_MASK) != 0) {
				bus->lifecycle_claimed = 0U;
				usb_topology_unlock();

				/* Failed. */
				return EBUSY;
			}

			/* Handles the state condition. */
			if ((state & USB_DEVICE_LIFECYCLE_FINALIZING) != 0) {
				bus->lifecycle_claimed = 0U;
				usb_topology_unlock();

				/* Failed. */
				return EALREADY;
			}

			/* Checks the hal atomic compare exchange acq rel result. */
			if (hal_atomic_compare_exchange_acq_rel(
				    &bus->root_hub->lifecycle, &state,
				    state | USB_DEVICE_LIFECYCLE_FINALIZING))
				break;
		}

		usb_topology_unlock();
		hcd->ops->stop(hcd);
		usb_topology_lock();
		/* Process each linked entry. */
		for (link = &usb_buses; *link != NULL && *link != bus;
		     link = &(*link)->next)
			;

		/* Handles the link condition. */
		if (*link != bus || !bus->lifecycle_claimed)
			__builtin_trap();
		*link = bus->next;
		bus->lifecycle_claimed = 0U;
		usb_topology_unlock();
		hal_free(bus->root_hub);
		hal_free(bus->ports);
		hal_free(bus);

		/* Succeeded. */
		return 0;
	}

	usb_topology_unlock();

	/* Failed. */
	return ENOENT;
}

/*
 * Reads the companion descriptor a SuperSpeed endpoint carries.
 */
int
drv_usb_decode_superspeed_endpoint_companion(
	const void *raw,
	size_t length,
	struct drv_usb_superspeed_endpoint_companion_descriptor *result)
{
	const uint8_t *bytes = raw;
	struct drv_usb_superspeed_endpoint_companion_descriptor descriptor;

	/* Handles the raw availability. */
	if (raw == NULL || result == NULL || length < sizeof(descriptor))
		return EINVAL;

	/* Handles the bytes condition. */
	if (bytes[0] != sizeof(descriptor) ||
	    bytes[1] != DRV_USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION) {
		/* Failed. */
		return EINVAL;
	}
	descriptor.length = bytes[0];
	descriptor.descriptor_type = bytes[1];
	descriptor.maximum_burst = bytes[2];
	descriptor.attributes = bytes[3];
	descriptor.bytes_per_interval =
		(uint16_t)bytes[4] | ((uint16_t)bytes[5] << 8);
	*result = descriptor;
	/* Succeeded. */
	return 0;
}

/*
 * Tells the subsystem that a root port has changed.
 */
void
drv_usb_hcd_root_hub_changed(
	struct drv_usb_hcd *hcd)
{
	struct drv_usb_device *present;
	uint32_t status;
	unsigned connected, enabled, connection_changed, enable_changed;
	unsigned initial, state_changed, port_disabled;
	unsigned replace_generation;
	int error;
	struct drv_usb_bus *bus;
	unsigned port;

	usb_topology_lock();

	/* Checks the hal atomic load acquire result. */
	bus = find_hcd_bus(hcd);
	if (bus == NULL || hal_atomic_load_acquire(&bus->stopping) != 0 ||
	    hcd->ops->root_hub_control == NULL) {
		usb_topology_unlock();

		/* Returns the computed result. */
		return;
	}

	/* Process each remaining element. */
	for (port = 1; port <= hcd->root_port_count; port++) {
		status = 0;

		replace_generation = 0;

		/* Checks the operation status. */
		error = root_port_status(hcd, port, &status);
		if (error != 0)
			continue;

		/* Checks the operation status. */
		error = root_port_acknowledge_changes(hcd, port, status);
		if (error != 0) {
			hal_printf("usb%u: port %u change acknowledge failed "
				   "(%d)\n",
				   bus->number, port, error);
			continue;
		}

		connected = (status & 1U) != 0;
		enabled = (status & 2U) != 0;
		connection_changed = (status & (1U << 16)) != 0;
		enable_changed = (status & (1U << 17)) != 0;
		initial = !bus->ports[port].observed;
		state_changed =
			!initial && bus->ports[port].connected != connected;

		/* Handles the initial condition. */
		if (initial || connection_changed || state_changed) {
			usb_generation_next(
				&bus->ports[port].connection_generation);
		}

		port_disabled = connected && !enabled &&
				(!initial &&
				 (bus->ports[port].enabled || enable_changed));
		bus->ports[port].observed = 1U;
		bus->ports[port].connected = connected;
		bus->ports[port].enabled = enabled;
		present = find_port_device(bus, port);

		/*
		 * A connection edge identifies a new physical generation even
		 * when the port is connected again by the time it is sampled.
		 * Never let the retained device, bindings, or URBs cross that
		 * edge.  A failed destroy leaves DISCONNECTING set; periodic
		 * root scans then retry the same teardown without relying on
		 * another hardware change bit.
		 */
		if (present != NULL && connected &&
		    (connection_changed || state_changed || port_disabled ||
		     device_is_disconnecting(present)))
			replace_generation = 1;

		/* Checks the device is disconnecting result. */
		if (present != NULL &&
		    (!connected || connection_changed || state_changed ||
		     port_disabled || device_is_disconnecting(present))) {
			/* Checks the destroy device result. */
			if (destroy_device(bus, present) == 0)
				present = NULL;
		}

		/* Handles the present availability. */
		if (!connected || present != NULL)
			continue;

		/* Handles the initial condition. */
		if (!initial && !connection_changed && !state_changed &&
		    !replace_generation)
			continue;

		/* Handles the root port reset availability. */
		if (hcd->ops->root_port_reset != NULL)
			error = hcd->ops->root_port_reset(hcd, port);
		else
			error = legacy_root_port_reset(hcd, port);
		if (error == 0)
			error = root_port_status(hcd, port, &status);
		if (error == 0) {
			bus->ports[port].connected = (status & 1U) != 0;
			bus->ports[port].enabled = (status & 2U) != 0;

			/* Checks the operation status. */
			if ((status & 3U) == 3U)
				error = enumerate_port(bus, port, status);
			else
				error = ENODEV;
		}
		if (error != 0) {
			hal_printf("usb%u: port %u enumeration failed (%d)\n",
				   bus->number, port, error);
		}
	}

	usb_topology_unlock();
}

/*
 * Completes one transfer a host controller has finished.
 */
void
drv_usb_hcd_complete(
	struct drv_usb_hcd *hcd,
	struct drv_usb_urb *urb,
	enum drv_usb_urb_status status,
	size_t actual)
{
	(void)hcd;
	(void)urb_publish_terminal(urb, status, actual);
	urb_hcd_put(urb);
}

/*
 * Calls back for every bus the subsystem holds.
 */
int
drv_usb_foreach_bus(
	drv_usb_bus_iterator_t fn,
	void *argument)
{
	struct drv_usb_bus *b;
	int e;

	/* Handles the fn condition. */
	if (!fn)
		return EINVAL;
	/* Process each linked entry. */
	for (b = usb_buses; b; b = b->next) {
		/* Checks the fn result. */
		if ((e = fn(b, argument)) != 0)
			return e;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Calls back for every device of one bus.
 */
int
drv_usb_bus_foreach_device(
	struct drv_usb_bus *b,
	drv_usb_device_iterator_t fn,
	void *a)
{
	struct drv_usb_device *d;
	int e;

	/* Handles the b condition. */
	if (!b || !fn)
		return EINVAL;

	/* Checks the fn result. */
	if ((e = fn(b->root_hub, a)) != 0)
		return e;
	/* Process each linked entry. */
	for (d = b->devices; d; d = d->next) {
		/* Checks the fn result. */
		if ((e = fn(d, a)) != 0)
			return e;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Calls back for every device of every bus.
 */
int
drv_usb_foreach_device(
	drv_usb_device_iterator_t fn,
	void *a)
{
	struct drv_usb_bus *b;
	int e;

	/* Handles the fn condition. */
	if (!fn)
		return EINVAL;
	/* Process each linked entry. */
	for (b = usb_buses; b; b = b->next) {
		/* Checks the drv usb bus foreach device result. */
		if ((e = drv_usb_bus_foreach_device(b, fn, a)) != 0)
			return e;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Calls back for every interface of one device.
 */
int
drv_usb_device_foreach_interface(
	struct drv_usb_device *d,
	drv_usb_interface_iterator_t fn,
	void *a)
{
	struct drv_usb_interface *i;
	int e;

	/* Checks the current descriptor. */
	if (!d || !fn)
		return EINVAL;
	/* Process each linked entry. */
	for (i = d->interfaces; i; i = i->next) {
		/* Checks the fn result. */
		if ((e = fn(i, a)) != 0)
			return e;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Finds a device by its bus and address.
 */
struct drv_usb_device *
drv_usb_find_device(
	unsigned bus,
	unsigned address)
{
	struct drv_usb_bus *b;
	struct drv_usb_device *d;

	/* Process each linked entry. */
	for (b = usb_buses; b; b = b->next) {
		/* Handles the b condition. */
		if (b->number == bus) {
			/* Handles the address condition. */
			if (address == 0)
				return b->root_hub;
			/* Process each linked entry. */
			for (d = b->devices; d; d = d->next) {
				/* Checks the current descriptor. */
				if (d->address == address)
					return d;
			}
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Reports the number a bus was published under.
 */
unsigned
drv_usb_bus_number(
	const struct drv_usb_bus *b)
{
	/* Returns the computed result. */
	return b ? b->number : 0;
}

/*
 * Reports the host controller behind a bus.
 */
struct drv_usb_hcd *
drv_usb_bus_hcd(
	const struct drv_usb_bus *b)
{
	/* Returns the computed result. */
	return b ? b->hcd : NULL;
}

/*
 * Reports the root hub device of a bus.
 */
struct drv_usb_device *
drv_usb_bus_root_hub(
	const struct drv_usb_bus *b)
{
	/* Returns the computed result. */
	return b ? b->root_hub : NULL;
}

/*
 * Reports the bus a device is on.
 */
struct drv_usb_bus *
drv_usb_device_bus(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->bus : NULL;
}

/*
 * Reports the hub a device hangs from.
 */
struct drv_usb_device *
drv_usb_device_parent(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->parent : NULL;
}

/*
 * Reports the bus address a device was given.
 */
unsigned
drv_usb_device_address(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->address : 0;
}

/*
 * Reports which port of its parent a device is on.
 */
unsigned
drv_usb_device_port(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->port : 0;
}

/*
 * Reports the speed a device was enumerated at.
 */
enum drv_usb_speed
drv_usb_device_speed(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->speed : DRV_USB_SPEED_UNKNOWN;
}

/*
 * Reports where a device stands in its life.
 */
enum drv_usb_device_state
drv_usb_device_state(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->state : DRV_USB_STATE_NOT_ATTACHED;
}

/*
 * Reports the device descriptor that was read from it.
 */
const struct drv_usb_device_descriptor *
drv_usb_device_descriptor(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? &d->descriptor : NULL;
}

/*
 * Reports how many transfers the controller still holds.
 */
unsigned
drv_usb_device_hcd_urb_count(
	const struct drv_usb_device *d)
{
	unsigned function_result;

	/* Computes the function result. */
	function_result = d ? hal_atomic_load_acquire(&d->hcd_urb_count) : 0;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Asks whether a device is already being disconnected.
 */
int
drv_usb_device_is_tearing_down(
	const struct drv_usb_device *device)
{
	int error;

	/* Computes the function result. */
	error =
		device != NULL && (hal_atomic_load_acquire(&device->lifecycle) &
				   (USB_DEVICE_LIFECYCLE_DISCONNECTING |
				    USB_DEVICE_LIFECYCLE_FINALIZING)) != 0;

	/* Returns the computed result. */
	return error;
}

/*
 * Reports what the controller can do for this device.
 */
unsigned
drv_usb_device_hcd_capabilities(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d && d->bus && d->bus->hcd ? d->bus->hcd->capabilities : 0;
}

/*
 * Reports the controller's own state for this device.
 */
uintptr_t
drv_usb_device_hcd_data(
	const struct drv_usb_device *d,
	unsigned n)
{
	uintptr_t function_result;

	/* Computes the function result. */
	function_result = d && n < 4U ? __atomic_load_n(&d->hcd_private[n],
							__ATOMIC_ACQUIRE)
				      : 0;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Gives the controller somewhere to keep that state.
 */
int
drv_usb_device_set_hcd_data(
	struct drv_usb_device *d,
	unsigned n,
	uintptr_t value)
{
	/* Checks the current descriptor. */
	if (!d || n >= 4U)
		return EINVAL;
	__atomic_store_n(&d->hcd_private[n], value, __ATOMIC_RELEASE);

	/* Succeeded. */
	return 0;
}

/*
 * Reports the DMA device transfers to this device use.
 */
struct drv_dma_device *
drv_usb_device_dma(
	struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->bus->hcd->dma : NULL;
}

/*
 * Resets a device through the port it hangs from.
 */
int
drv_usb_device_reset(
	struct drv_usb_device *device)
{
	int identity_error;
	int proof_error;
	size_t actual;
	struct drv_usb_bus *bus;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *closed[DRV_USB_MAX_INTERFACES];
	struct drv_usb_interface *effective_owner;
	uint64_t device_generation, port_generation;
	uint32_t port_status = 0;
	unsigned closed_count = 0, old_address;
	int binding_closed = 0, selection_locked = 0, control_locked = 0;
	int error, rollback_error;

	/* Handles the device availability. */
	if (device == NULL)
		return EINVAL;

	/*
	 * A reset caller may be running immediately after an URB callback.
	 * Never wait behind root-worker teardown while that path is joining the
	 * callback or its submission admission; topology ownership is an
	 * all-or-nothing preflight condition for this synchronous API.
	 */
	if (!atomic_try_acquire_zero(&usb_topology_gate))
		return EBUSY;

	/* Handles the bus availability. */
	bus = device->bus;
	if (bus == NULL || device == bus->root_hub || device->parent == NULL) {
		error = EINVAL;
		goto out;
	}

	/* Handles the device condition. */
	if (device->parent != bus->root_hub) {
		error = ENOTSUP;
		goto out;
	}

	/* Checks the device linked result. */
	if (device->port == 0 || device->port > bus->hcd->root_port_count ||
	    !device_linked(bus, device) || device_is_disconnecting(device) ||
	    device_is_quarantined(device) ||
	    hal_atomic_load_acquire(&bus->stopping) != 0) {
		error = ENODEV;
		goto out;
	}

	/* Checks the operation status. */
	error = root_port_status(bus->hcd, device->port, &port_status);
	if (error != 0)
		goto out;

	/* Handles the port status condition. */
	if ((port_status & 3U) != 3U || (port_status & (1U << 16)) != 0 ||
	    bus->ports[device->port].connection_generation !=
		    device->port_generation) {
		error = ENODEV;
		goto out;
	}

	device_generation = device->generation;
	port_generation = bus->ports[device->port].connection_generation;

	/* Checks the io gate close empty result. */
	if (io_gate_close_empty(&device->binding_transactions) != 0) {
		error = EBUSY;
		goto out;
	}

	binding_closed = 1;

	/* Checks the atomic try acquire zero result. */
	if (!atomic_try_acquire_zero(&device->selection_gate)) {
		error = EBUSY;
		goto out;
	}

	selection_locked = 1;
	configuration = device_active_configuration(device);

	/* Checks the operation status. */
	error = configuration_effective_owner(configuration, &effective_owner);
	if (error != 0)
		goto out;
	(void)effective_owner;

	/* Checks the operation status. */
	error = configuration_close_io(configuration, closed, &closed_count);
	if (error != 0)
		goto out;

	/* Checks the operation status. */
	error = device_control_try_lock(device);
	if (error != 0)
		goto out;
	control_locked = 1;

	/* Checks the device is disconnecting result. */
	if (device_is_disconnecting(device) || device_is_quarantined(device) ||
	    hal_atomic_load_acquire(&bus->stopping) != 0 ||
	    device->generation != device_generation ||
	    bus->ports[device->port].connection_generation != port_generation ||
	    drv_usb_device_hcd_urb_count(device) != 0) {
		error = device_is_disconnecting(device) ||
					hal_atomic_load_acquire(
						&bus->stopping) != 0
				? ENODEV
				: EBUSY;
		goto out;
	}

	/* Checks the operation status. */
	error = device_disable_active_endpoints(device);
	if (error != 0)
		goto out;

	/*
	 * Endpoint scheduling is disabled, but the HCD device and physical port
	 * are still intact.  Recheck the captured physical generation
	 * immediately before the destructive boundary and compensate this
	 * invocation on failure.
	 */

	/* Checks the operation status. */
	error = device_reset_connection_check(bus, device, device_generation,
					      port_generation);
	if (error != 0) {
		identity_error = error;
		proof_error = identity_error == ENODEV
				      ? identity_error
				      : device_reset_connection_check(
						bus, device, device_generation,
						port_generation);

		/*
		 * Never restore the old endpoint schedule onto a detached or
		 * replaced physical device.  A transient status-read failure
		 * may be compensated only after a second positive identity
		 * proof; otherwise the retained core object is quarantined for
		 * terminal topology teardown.
		 */
		if (proof_error == 0) {
			rollback_error =
				configuration == NULL
					? 0
					: configuration_enable_endpoints(
						  configuration);

			/* Checks the operation status. */
			if (rollback_error != 0) {
				device_quarantine_recovery(
					device, "device reset rollback",
					rollback_error);
			}
		} else {
			/* Checks the operation status. */
			if (proof_error == ENODEV)
				error = ENODEV;
			device_quarantine_recovery(
				device, "device reset identity", error);
		}

		goto out;
	}

	/*
	 * Calling the HCD's checked per-device DMA barrier is the destructive
	 * boundary.  Do not use device_quiesce(): that helper is for terminal
	 * teardown and may relink the object or change its quarantine state.
	 */
	if (bus->hcd->ops->device_quiesce != NULL) {
		/* Checks the operation status. */
		error = bus->hcd->ops->device_quiesce(bus->hcd, device);
		if (error != 0)
			goto fail_destructive;

		/* Checks the drv usb device hcd urb count result. */
		if (drv_usb_device_hcd_urb_count(device) != 0) {
			error = EBUSY;
			goto fail_destructive;
		}
	}

	/* Handles the device disable availability. */
	if (bus->hcd->ops->device_disable != NULL)
		bus->hcd->ops->device_disable(bus->hcd, device);

	/*
	 * A controller with a per-device quiesce/release operation has crossed
	 * its boundary already.  Sample again before touching the physical
	 * port.
	 */
	if (bus->hcd->ops->device_quiesce != NULL ||
	    bus->hcd->ops->device_disable != NULL) {
		/* Checks the operation status. */
		error = device_reset_connection_check(
			bus, device, device_generation, port_generation);
		if (error != 0)
			goto fail_destructive;
	}

	/* Handles the root port reset availability. */
	if (bus->hcd->ops->root_port_reset != NULL)
		error = bus->hcd->ops->root_port_reset(bus->hcd, device->port);
	else
		error = legacy_root_port_reset(bus->hcd, device->port);
	if (error != 0)
		goto fail_destructive;

	/* Checks the operation status. */
	error = device_reset_connection_check(bus, device, device_generation,
					      port_generation);
	if (error != 0)
		goto fail_destructive;
	bus->ports[device->port].connected = 1U;
	bus->ports[device->port].enabled = 1U;
	old_address = device->address;
	device->state = DRV_USB_STATE_DEFAULT;
	device->address = 0;

	/* Handles the device enable availability. */
	if (bus->hcd->ops->device_enable != NULL) {
		/* Checks the operation status. */
		error = bus->hcd->ops->device_enable(bus->hcd, device);
		if (error != 0) {
			device->address = old_address;
			goto fail_destructive;
		}
	}

	/* Checks the operation status. */
	error = device_reset_connection_check(bus, device, device_generation,
					      port_generation);
	if (error != 0)
		goto fail_destructive;

	/* Handles the device set address availability. */
	if (bus->hcd->ops->device_set_address != NULL) {
		device->address = old_address;
		error = bus->hcd->ops->device_set_address(bus->hcd, device,
							  old_address);
	} else {
		actual = 0;

		/* Puts the device back at the address it already had. */
		error = usb_control_locked(
			device,
			DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
				DRV_USB_RECIP_DEVICE,
			USB_REQ_SET_ADDRESS, (uint16_t)old_address, 0, NULL, 0,
			USB_CONTROL_TIMEOUT_MS, &actual);
		device->address = old_address;
	}
	if (error != 0)
		goto fail_destructive;
	device->state = DRV_USB_STATE_ADDRESS;
	usb_delay_ticks(USB_ADDRESS_RECOVERY_TICKS);

	/* Checks the operation status. */
	error = device_reset_connection_check(bus, device, device_generation,
					      port_generation);
	if (error != 0)
		goto fail_destructive;

	/* Checks the operation status. */
	error = configuration_restore(device, configuration);
	if (error != 0)
		goto fail_destructive;

	/*
	 * This final non-acknowledging sample is both the post-configuration
	 * seam and the publication barrier for all retained binding objects.
	 */

	/* Checks the operation status. */
	error = device_reset_connection_check(bus, device, device_generation,
					      port_generation);
	if (error != 0)
		goto fail_destructive;
	device_publish_configuration(device, configuration);
	device->interfaces =
		configuration == NULL ? NULL : configuration->interfaces;
	device->state = configuration == NULL ? DRV_USB_STATE_ADDRESS
					      : DRV_USB_STATE_CONFIGURED;
	error = 0;
	goto out;

fail_destructive:

	/* Checks the device is disconnecting result. */
	if (!device_is_disconnecting(device) &&
	    hal_atomic_load_acquire(&bus->stopping) == 0)
		device_quarantine_recovery(device, "device reset", error);
	else
		error = ENODEV;

out:

	/* Handles the control locked condition. */
	if (control_locked)
		device_control_unlock(device);
	configuration_open_io(closed, closed_count);

	/* Handles the selection locked condition. */
	if (selection_locked)
		atomic_store_release(&device->selection_gate, 0U);

	/* Handles the binding closed condition. */
	if (binding_closed)
		io_gate_open(&device->binding_transactions);
	usb_topology_unlock();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Selects which configuration a device presents.
 */
int
drv_usb_device_set_configuration(
	struct drv_usb_device *device,
	unsigned configuration_value)
{
	struct drv_usb_configuration *old, *target = NULL;
	struct drv_usb_interface *old_closed[DRV_USB_MAX_INTERFACES];
	struct drv_usb_interface *target_closed[DRV_USB_MAX_INTERFACES];
	unsigned old_closed_count = 0, target_closed_count = 0;
	unsigned index;
	size_t actual = 0;
	int error, rollback_error;
	int control_locked = 0, selection_locked = 0;

	/* Handles the device availability. */
	if (device == NULL || device->parent == NULL ||
	    (device->state != DRV_USB_STATE_ADDRESS &&
	     device->state != DRV_USB_STATE_CONFIGURED)) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the device is disconnecting condition. */
	if (device_is_disconnecting(device) || device_is_quarantined(device))
		return ENODEV;

	/* Handles the configuration value condition. */
	if (configuration_value != 0) {
		/* Process each remaining element. */
		for (index = 0; index < device->configuration_count; index++) {
			/* Handles the device condition. */
			if (device->configurations[index]
				    .descriptor.configuration_value ==
			    configuration_value) {
				target = &device->configurations[index];
				break;
			}
		}

		/* Handles the target availability. */
		if (target == NULL)
			return ENOENT;
	}

	/* Checks the atomic try acquire zero result. */
	if (!atomic_try_acquire_zero(&device->selection_gate))
		return EBUSY;
	selection_locked = 1;

	/* Checks the device is disconnecting result. */
	if ((device->state != DRV_USB_STATE_ADDRESS &&
	     device->state != DRV_USB_STATE_CONFIGURED) ||
	    device_is_disconnecting(device) || device_is_quarantined(device)) {
		error = ENODEV;
		goto out;
	}

	/* Handles the old condition. */
	old = device_active_configuration(device);
	if (old == target) {
		error = 0;
		goto out;
	}

	/* Checks the configuration has owners result. */
	if ((old != NULL && configuration_has_owners(old)) ||
	    (target != NULL && configuration_has_owners(target)) ||
	    configuration_close_io(old, old_closed, &old_closed_count) != 0 ||
	    configuration_close_io(target, target_closed,
				   &target_closed_count) != 0) {
		error = EBUSY;
		goto out;
	}

	/* Checks the operation status. */
	error = device_control_try_lock(device);
	if (error != 0)
		goto out;
	control_locked = 1;

	/* Handles the old availability. */
	if (old != NULL) {
		/* Checks the operation status. */
		error = configuration_disable_endpoints(old);
		if (error != 0)
			goto out;
	}

	/* Checks the operation status. */
	error = usb_control_locked(device,
				   DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
					   DRV_USB_RECIP_DEVICE,
				   USB_REQ_SET_CONFIGURATION,
				   (uint16_t)configuration_value, 0, NULL, 0,
				   USB_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0) {
		/*
		 * STALL rejects the request without changing the selected
		 * configuration. Other transport failures leave device state
		 * unknowable and must not republish endpoints.
		 */
		if (error != EPIPE) {
			device_quarantine_selection(device, "set-configuration",
						    error);
			goto out;
		}

		rollback_error =
			old == NULL ? 0 : configuration_enable_endpoints(old);

		/* Checks the operation status. */
		if (rollback_error != 0) {
			device_quarantine_selection(device, "set-configuration",
						    rollback_error);
		}

		goto out;
	}

	/* Handles the target availability. */
	if (target != NULL) {
		configuration_select_defaults(target);

		/* Checks the operation status. */
		error = configuration_enable_endpoints(target);
		if (error != 0) {
			/* Checks the device is quarantined result. */
			rollback_error = 0;
			if (!device_is_quarantined(device)) {
				rollback_error =
					configuration_restore(device, old);
			}
			if (rollback_error != 0) {
				device_quarantine_selection(
					device, "configuration-enable",
					rollback_error);
			}

			goto out;
		}

		/* Checks the operation status. */
		error = configuration_reset_endpoints(target);
		if (error != 0) {
			/*
			 * SET_CONFIGURATION has succeeded; schedule
			 * compensation cannot reconstruct the old device-side
			 * endpoint state.
			 */
			device_quarantine_selection(
				device, "configuration-reset", error);
			goto out;
		}
	}

	device_publish_configuration(device, target);
	device->interfaces = target == NULL ? NULL : target->interfaces;
	device->state = target == NULL ? DRV_USB_STATE_ADDRESS
				       : DRV_USB_STATE_CONFIGURED;
	error = 0;

out:

	/* Handles the control locked condition. */
	if (control_locked)
		device_control_unlock(device);
	configuration_open_io(target_closed, target_closed_count);
	configuration_open_io(old_closed, old_closed_count);

	/* Handles the selection locked condition. */
	if (selection_locked)
		atomic_store_release(&device->selection_gate, 0U);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Reads one string descriptor from a device.
 */
int
drv_usb_device_get_string(
	struct drv_usb_device *device,
	unsigned index,
	unsigned language,
	char *buffer,
	size_t capacity)
{
	uint32_t low;
	uint32_t codepoint;
	uint8_t descriptor[255];
	size_t descriptor_length, offset, used = 0;
	uint16_t selected_language;
	int error;

	/* Handles the device availability. */
	if (device == NULL || index == 0 || index > UINT8_MAX ||
	    language > UINT16_MAX || buffer == NULL || capacity == 0) {
		/* Failed. */
		return EINVAL;
	}
	buffer[0] = '\0';

	/* Handles the selected language condition. */
	selected_language = (uint16_t)language;
	if (selected_language == 0) {
		/* Checks the operation status. */
		error = usb_string_descriptor(device, 0, 0, descriptor,
					      &descriptor_length);
		if (error != 0)
			return error;

		/* Handles the descriptor length condition. */
		if (descriptor_length < 4U)
			return EILSEQ;

		/* Handles the selected language condition. */
		selected_language = (uint16_t)(descriptor[2] |
					       ((uint16_t)descriptor[3] << 8));
		if (selected_language == 0)
			return EILSEQ;
	}

	/* Checks the operation status. */
	error = usb_string_descriptor(device, (uint8_t)index, selected_language,
				      descriptor, &descriptor_length);
	if (error != 0)
		return error;
	/* Process each remaining element. */
	for (offset = 2; offset < descriptor_length; offset += 2U) {
		/* Handles the codepoint condition. */
		codepoint = descriptor[offset] |
			    ((uint32_t)descriptor[offset + 1U] << 8);
		if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
			/* Checks the current offset. */
			if (offset + 3U >= descriptor_length) {
				buffer[0] = '\0';

				/* Failed. */
				return EILSEQ;
			}

			/* Handles the low condition. */
			low = descriptor[offset + 2U] |
			      ((uint32_t)descriptor[offset + 3U] << 8);
			if (low < 0xdc00U || low > 0xdfffU) {
				buffer[0] = '\0';

				/* Failed. */
				return EILSEQ;
			}

			codepoint = 0x10000U + ((codepoint - 0xd800U) << 10) +
				    (low - 0xdc00U);
			offset += 2U;
		} else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
			buffer[0] = '\0';

			/* Failed. */
			return EILSEQ;
		}

		/* Checks the operation status. */
		error = utf8_append(buffer, capacity, &used, codepoint);
		if (error != 0) {
			buffer[0] = '\0';

			/* Failed. */
			return error;
		}
	}

	buffer[used] = '\0';

	/* Succeeded. */
	return 0;
}

/*
 * Takes the state one transfer needs.
 */
struct drv_usb_urb *
drv_usb_urb_alloc(
	struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint,
	unsigned iso_count)
{
	struct drv_usb_urb *urb;

	/* Checks the device urb get result. */
	if (device == NULL || !device_urb_get(device))
		return NULL;

	/* Handles the endpoint availability. */
	if (endpoint == NULL)
		endpoint = &device->endpoint0;

	/* Handles the endpoint condition. */
	if (endpoint != &device->endpoint0) {
		/* Checks the atomic try acquire zero result. */
		if (!atomic_try_acquire_zero(&device->selection_gate)) {
			device_urb_put(device);

			/* Reports that no result is available. */
			return NULL;
		}

		/* Checks the endpoint retained by device result. */
		if (!endpoint_retained_by_device(device, endpoint) ||
		    device->state != DRV_USB_STATE_CONFIGURED ||
		    device_is_disconnecting(device) ||
		    device_is_quarantined(device) ||
		    device_active_configuration(device) !=
			    endpoint->interface->configuration) {
			atomic_store_release(&device->selection_gate, 0U);
			device_urb_put(device);

			/* Reports that no result is available. */
			return NULL;
		}

		atomic_store_release(&device->selection_gate, 0U);
	}

	/* Handles the urb availability. */
	urb = hal_malloc(sizeof(*urb));
	if (urb == NULL) {
		device_urb_put(device);

		/* Reports that no result is available. */
		return NULL;
	}

	memset(urb, 0, sizeof(*urb));
	refcount_init(&urb->references, 1U);
	urb->device = device;
	urb->endpoint = endpoint;
	urb->iso_packet_count = iso_count;

	/* Handles the iso count condition. */
	if (iso_count != 0) {
		urb->iso_packets =
			hal_malloc(sizeof(*urb->iso_packets) * iso_count);

		/* Handles the iso packets availability. */
		if (urb->iso_packets == NULL) {
			hal_free(urb);
			device_urb_put(device);

			/* Reports that no result is available. */
			return NULL;
		}

		memset(urb->iso_packets, 0,
		       sizeof(*urb->iso_packets) * iso_count);
	}

	/* Returns the computed result. */
	return urb;
}

/*
 * Gives that state back.
 */
void
drv_usb_urb_free(
	struct drv_usb_urb *u)
{
	/* Handles the u condition. */
	if (u)
		urb_put(u);
}

/*
 * Reserve before entering reclaim or a class-driver I/O lock. The HCD-owned reference retains this storage even if a failed cancellation outlives wait.
 */
int
drv_usb_urb_reserve_sync(
	struct drv_usb_urb *u,
	size_t capacity)
{
	int error;
	void *buffer;

	/* Handles the u availability. */
	if (u == NULL || u->iso_packet_count != 0)
		return EINVAL;

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&u->hcd_owned) != 0 ||
	    hal_atomic_load_acquire(&u->status) == DRV_USB_URB_PENDING) {
		/* Failed. */
		return EBUSY;
	}

	/* Handles the capacity condition. */
	if (capacity <= u->sync_capacity)
		return 0;

	/* Handles the u condition. */
	if (u->sync_shared) {
		/* Obtains the drv usb urb reserve transfer result. */
		error = drv_usb_urb_reserve_transfer(u, capacity);

		/* Failed. */
		return error;
	}

	/* Handles the buffer availability. */
	buffer = hal_malloc(capacity);
	if (buffer == NULL)
		return ENOMEM;
	io_stats_record(IO_USB_BUFFER_ALLOC, capacity);

	/* Handles the sync buffer availability. */
	if (u->sync_buffer != NULL)
		io_stats_record(IO_USB_BUFFER_FREE, u->sync_capacity);
	hal_free(u->sync_buffer);
	u->sync_buffer = buffer;
	u->sync_capacity = capacity;

	/* Succeeded. */
	return 0;
}

/*
 * Reserves core staging and HCD backing before entering the I/O path. The caller exclusively owns an idle URB; active or retained resources cannot grow.
 */
int
drv_usb_urb_reserve_transfer(
	struct drv_usb_urb *urb,
	size_t capacity)
{
	struct drv_usb_hcd *hcd;
	void *buffer;
	void *reservation;
	void *shared_buffer;
	size_t shared_capacity;
	int shared;
	int error;

	/* Validates the reservation owner and controller contract. */
	if (urb == NULL || capacity == 0 || urb->iso_packet_count != 0)
		return EINVAL;

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&urb->hcd_owned) != 0 ||
	    hal_atomic_load_acquire(&urb->status) == DRV_USB_URB_PENDING) {
		/* Failed. */
		return EBUSY;
	}

	/* Handles the urb reserve availability. */
	hcd = urb->device->bus->hcd;
	if (!(hcd->capabilities & DRV_USB_HCD_CAP_TRANSFER_RESERVE) ||
	    hcd->ops->urb_reserve == NULL || hcd->ops->urb_unreserve == NULL) {
		/* Failed. */
		return EOPNOTSUPP;
	}

	/* Handles the capacity condition. */
	if (capacity > DRV_USB_TRANSFER_RESERVE_MAX_SIZE)
		return EMSGSIZE;

	/* Handles the capacity condition. */
	if (capacity <= urb->transfer_capacity)
		return 0;

	/* Handles the urb reserve buffer availability. */
	shared = (hcd->capabilities & DRV_USB_HCD_CAP_SHARED_STAGING) != 0;
	if (shared && hcd->ops->urb_reserve_buffer == NULL)
		return EOPNOTSUPP;

	/*
	 * Keeps the old core staging intact until both replacement allocations
	 * succeed.
	 */

	/* Handles the shared condition. */
	buffer = NULL;
	if (!shared && capacity > urb->sync_capacity) {
		/* Handles the buffer availability. */
		buffer = hal_malloc(capacity);
		if (buffer == NULL)
			return ENOMEM;
	}

	reservation = NULL;

	/* Checks the operation status. */
	error = hcd->ops->urb_reserve(hcd, urb, capacity, &reservation);
	if (error != 0) {
		hal_free(buffer);

		/* Failed. */
		return error;
	}

	/* Handles the reservation availability. */
	if (reservation == NULL)
		__builtin_trap();
	shared_buffer = NULL;

	/* Handles the shared condition. */
	shared_capacity = 0;
	if (shared) {
		/* Handles the shared buffer availability. */
		shared_buffer = hcd->ops->urb_reserve_buffer(hcd, reservation,
							     &shared_capacity);
		if (shared_buffer == NULL || shared_capacity < capacity) {
			hcd->ops->urb_unreserve(hcd, reservation);

			/* Failed. */
			return EIO;
		}
	}

	/*
	 * Replaces idle resources only after the complete new reservation is
	 * available.
	 */
	if (urb->transfer_reservation != NULL) {
		hcd->ops->urb_unreserve(hcd, urb->transfer_reservation);
		io_stats_record(IO_USB_TRANSFER_RESERVE_FREE,
				urb->transfer_capacity);
	}

	urb->transfer_reservation = reservation;
	urb->transfer_capacity = capacity;
	io_stats_record(IO_USB_TRANSFER_RESERVE_ALLOC, capacity);

	/* Handles the buffer availability. */
	if (buffer != NULL || shared) {
		/* Handles the buffer availability. */
		if (buffer != NULL)
			io_stats_record(IO_USB_BUFFER_ALLOC, capacity);

		/* Handles the sync buffer availability. */
		if (urb->sync_buffer != NULL && !urb->sync_shared)
			io_stats_record(IO_USB_BUFFER_FREE, urb->sync_capacity);

		/* Handles the urb condition. */
		if (!urb->sync_shared)
			hal_free(urb->sync_buffer);
		urb->sync_buffer = shared ? shared_buffer : buffer;
		urb->sync_capacity = capacity;
		urb->sync_shared = shared;
	}

	/*
	 * Publishes an allocation-free capacity for subsequent synchronous
	 * submissions.
	 */
	return 0;
}

/*
 * Returns HCD-owned reservation metadata under the caller's existing URB reference.
 */
void *
drv_usb_urb_transfer_reservation(
	const struct drv_usb_urb *urb)
{
	/* Handles the urb availability. */
	if (urb == NULL)
		return NULL;

	/* Returns the computed result. */
	return urb->transfer_reservation;
}

/*
 * Describes a transfer of bulk or interrupt data.
 */
int
drv_usb_urb_setup(
	struct drv_usb_urb *u,
	void *b,
	size_t n,
	unsigned f,
	unsigned t,
	drv_usb_urb_callback_t cb,
	void *a)
{
	/* Handles the u availability. */
	if (u == NULL || (b == NULL && n != 0))
		return EINVAL;

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&u->status) == DRV_USB_URB_PENDING ||
	    hal_atomic_load_acquire(&u->hcd_owned) != 0) {
		/* Failed. */
		return EBUSY;
	}

	/* Handles the u condition. */
	if (u->transfer_capacity != 0 && n > u->transfer_capacity)
		return EINVAL;

	/* Handles the sync buffer availability. */
	if (u->sync_buffer != NULL && (cb != NULL || n > u->sync_capacity))
		return EINVAL;
	u->sync_client = u->sync_buffer != NULL ? b : NULL;
	u->buffer = u->sync_buffer != NULL ? u->sync_buffer : b;

	/* Handles the sync buffer availability. */
	if (n != 0 && u->sync_buffer != NULL && b != u->sync_buffer &&
	    (u->endpoint->type == DRV_USB_TRANSFER_CONTROL ||
	     (u->endpoint->descriptor.address & DRV_USB_DIR_IN) == 0)) {
		memcpy(u->sync_buffer, b, n);
		io_stats_record(IO_USB_STAGING_COPY, n);
	}

	u->length = n;
	u->flags = f;
	u->timeout_ms = t;
	u->callback = cb;
	u->callback_argument = a;
	u->actual_length = 0;
	hal_atomic_store_relaxed(&u->terminal_claimed, 0U);
	hal_atomic_store_release(&u->status, DRV_USB_URB_IDLE);

	/* Succeeded. */
	return 0;
}

/*
 * Describes a control transfer, with flags of its own.
 */
int
drv_usb_urb_setup_control_flags(
	struct drv_usb_urb *u,
	const struct drv_usb_control_request *r,
	void *b,
	size_t n,
	unsigned f,
	unsigned t,
	drv_usb_urb_callback_t cb,
	void *a)
{
	int error;

	/* Handles the u availability. */
	if (u == NULL || r == NULL ||
	    u->endpoint->type != DRV_USB_TRANSFER_CONTROL) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = drv_usb_urb_setup(u, b, n, f, t, cb, a);
	if (error == 0)
		u->control = *r;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Describes a control transfer.
 */
int
drv_usb_urb_setup_control(
	struct drv_usb_urb *u,
	const struct drv_usb_control_request *r,
	void *b,
	size_t n,
	unsigned t,
	drv_usb_urb_callback_t cb,
	void *a)
{
	int error;

	/* Obtains the drv usb urb setup control flags result. */
	error =
		drv_usb_urb_setup_control_flags(u, r, b, n, 0, t, cb, a);

	/* Returns the computed result. */
	return error;
}

/*
 * Describes an isochronous transfer.
 */
int
drv_usb_urb_setup_isochronous(
	struct drv_usb_urb *u,
	struct drv_usb_iso_packet *p,
	unsigned n)
{
	/* Handles the u condition. */
	if (!u || !p || n != u->iso_packet_count ||
	    u->endpoint->type != DRV_USB_TRANSFER_ISOCHRONOUS) {
		/* Failed. */
		return EINVAL;
	}
	memcpy(u->iso_packets, p, n * sizeof(*p));

	/* Succeeded. */
	return 0;
}

/*
 * Hands a transfer to the host controller.
 */
int
drv_usb_urb_submit(
	struct drv_usb_urb *urb)
{
	int function_result;
	struct drv_usb_device *device;
	struct drv_usb_interface *submitting_owner = NULL;
	struct usb_submit_commit commit;
	struct usb_submit_commit *claimed = NULL;
	bool irq_enabled;
	int error;

	/* Handles the urb availability. */
	if (urb == NULL)
		return EINVAL;

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&urb->hcd_owned) != 0)
		return EBUSY;

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&urb->status) == DRV_USB_URB_PENDING)
		return EINVAL;

	/* Checks the hal atomic load acquire result. */
	device = urb->device;
	if (hal_atomic_load_acquire(&device->bus->stopping) != 0)
		return EBUSY;

	/* Handles the device is disconnecting condition. */
	if (device_is_disconnecting(device) || device_is_quarantined(device))
		return ENODEV;

	/* Checks the endpoint is halted result. */
	if (endpoint_is_halted(device, urb->endpoint))
		return EPIPE;

	/* Checks the operation status. */
	error = io_gate_enter(&device->submit_gate);
	if (error != 0) {
		/* Computes the function result. */
		function_result = device_is_disconnecting(device) ||
						  device_is_quarantined(device)
					  ? ENODEV
					  : error;

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the operation status. */
	error = urb_hcd_get(urb);
	if (error != 0)
		goto out_submit;

	/* Checks the operation status. */
	error = urb_admission_get(urb, &submitting_owner);
	if (error != 0)
		goto out_hcd;

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&device->bus->stopping) != 0 ||
	    device_is_disconnecting(device) || device_is_quarantined(device)) {
		error = ENODEV;
		goto out_hcd;
	}

	/*
	 * Recheck after interface and binding admission.  The completion-side
	 * HCD publication barrier cannot be released until the STALL latch is
	 * visible, so no TD can enter between this check and enqueue.
	 */
	if (endpoint_is_halted(device, urb->endpoint)) {
		error = EPIPE;
		goto out_hcd;
	}

	urb->actual_length = 0;
	hal_atomic_store_relaxed(&urb->terminal_claimed, 0U);
	commit.device = device;
	commit.binding_owner = submitting_owner;
	atomic_store_release(&commit.finished, 0U);
	submitting_owner = NULL;
	hal_atomic_store_release(&urb->submit_commit_pending, 1U);
	__atomic_store_n(&urb->submit_commit, &commit, __ATOMIC_RELEASE);

	/*
	 * Publish PENDING only after the terminal path can observe the complete
	 * commit handoff.  A concurrent cancel which sees PENDING can therefore
	 * never enter its callback ahead of the short-gate release.
	 */
	hal_atomic_store_release(&urb->status, DRV_USB_URB_PENDING);
	error = device->bus->hcd->ops->urb_enqueue(device->bus->hcd, urb);

	/*
	 * If synchronous completion already claimed the stack record, it
	 * publishes finished before entering the callback.  Claiming it here
	 * and releasing the short submit gates must be indivisible with respect
	 * to a local HCD interrupt: otherwise that interrupt can observe a NULL
	 * record, wait for submit_commit_pending, and deadlock on the submitter
	 * it preempted.  A remote completion which wins the claim can still
	 * finish while local IRQs are masked.
	 */
	irq_enabled = hal_irq_disable();

	/* Checks the atomic load acquire result. */
	if (atomic_load_acquire(&commit.finished) == 0) {
		/* Handles the claimed availability. */
		claimed = __atomic_exchange_n(&urb->submit_commit, NULL,
					      __ATOMIC_ACQ_REL);
		if (claimed != NULL)
			submit_commit_finish(urb, claimed);
	}

	/* Handles the irq enabled condition. */
	if (irq_enabled)
		hal_irq_enable();

	/* Handles the claimed availability. */
	if (claimed == NULL) {
		/* Continue while the operation condition remains true. */
		while (atomic_load_acquire(&commit.finished) == 0)
			sched_yield();
	}

	/* Checks the operation status. */
	if (error != 0) {
		hal_atomic_store_release(&urb->status, DRV_USB_URB_IDLE);
		urb_hcd_put(urb);
	}

	/* Failed. */
	return error;

out_hcd:
	urb_hcd_put(urb);
	binding_submitter_put(submitting_owner);
out_submit:
	io_gate_exit(&device->submit_gate);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Asks the host controller to give a transfer up.
 */
int
drv_usb_urb_cancel(
	struct drv_usb_urb *u)
{
	int error;

	/* Obtains the urb cancel to result. */
	error = urb_cancel_to(u, DRV_USB_URB_CANCELLED);

	/* Returns the computed result. */
	return error;
}

/*
 * Waits for a transfer to complete.
 */
int
drv_usb_urb_wait(
	struct drv_usb_urb *urb)
{
	int error;
	uint64_t deadline, cancel_deadline = 0;

	/* Handles the urb availability. */
	if (urb == NULL)
		return EINVAL;
	deadline =
		urb->timeout_ms
			? sched_ticks() + ((uint64_t)urb->timeout_ms + 9U) / 10U
			: 0;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/*
		 * Keep the atomic observation inside the switch expression.
		 * Besides making the single-load terminal mapping explicit,
		 * this avoids GCC's analyzer losing the initialized state of an
		 * automatic enum across the cancel/retry back edge.
		 */
		/* Dispatch the current operation state. */
		switch (hal_atomic_load_acquire(&urb->status)) {
		case DRV_USB_URB_PENDING:
			break;
		case DRV_USB_URB_COMPLETE:
			/* Succeeded. */
			return 0;
		case DRV_USB_URB_TIMEOUT:
			/* Failed. */
			return ETIMEDOUT;
		case DRV_USB_URB_STALL:
			/* Failed. */
			return EPIPE;
		case DRV_USB_URB_DISCONNECTED:
			/* Failed. */
			return ENODEV;
		case DRV_USB_URB_IDLE:
		case DRV_USB_URB_IO_ERROR:
		case DRV_USB_URB_CANCELLED:
		default:
			/* Failed. */
			return EIO;
		}

		/* Checks the sched ticks result. */
		if (deadline != 0 && sched_ticks() >= deadline) {
			/* Checks the operation status. */
			error = urb_cancel_to(urb, DRV_USB_URB_TIMEOUT);
			if (error == 0)
				continue;
			if (error != EBUSY && error != EINVAL &&
			    error != EALREADY) {
				/* Failed. */
				return error;
			}

			/* Handles the cancel deadline condition. */
			if (cancel_deadline == 0)
				cancel_deadline = sched_ticks() + 100U;

			/* Checks the sched ticks result. */
			if (sched_ticks() >= cancel_deadline)
				return ETIMEDOUT;
			sched_yield();
			continue;
		}

		hal_compiler_barrier();
	}
}

/*
 * Waits for a transfer to leave the controller entirely.
 */
int
drv_usb_urb_drain(
	struct drv_usb_urb *u,
	unsigned timeout_ms)
{
	uint64_t now;
	uint64_t ticks;
	enum drv_usb_urb_status status;
	unsigned owned;
	uint64_t deadline = 0;

	/* Handles the u availability. */
	if (u == NULL)
		return EINVAL;

	/* Handles the timeout ms condition. */
	if (timeout_ms != 0) {
		now = sched_ticks();
		ticks = ((uint64_t)timeout_ms + 9U) / 10U;

		deadline = UINT64_MAX - now < ticks ? UINT64_MAX : now + ticks;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		status = hal_atomic_load_acquire(&u->status);

		/* Checks the operation status. */
		owned = hal_atomic_load_acquire(&u->hcd_owned);
		if (status != DRV_USB_URB_PENDING && owned == 0)
			return 0;

		/* Checks the sched ticks result. */
		if (deadline != 0 && sched_ticks() >= deadline)
			return ETIMEDOUT;
		sched_yield();
	}
}

/*
 * Waits until a transfer's state may be used again.
 */
int
drv_usb_urb_wait_reusable(
	struct drv_usb_urb *u)
{
	int error, drained;
	unsigned attempt;

	/* Handles the u availability. */
	if (u == NULL || u->callback != NULL)
		return EINVAL;
	error = drv_usb_urb_wait(u);

	/*
	 * A hard cancel failure used to leave this caller waiting forever.
	 * Retry checked retirement, then detach only the caller's view of an
	 * isolated buffer. Never forge completion or release the HCD reference.
	 */
	for (attempt = 0; attempt < 2U && hal_atomic_load_acquire(&u->status) ==
						  DRV_USB_URB_PENDING;
	     attempt++) {
		(void)urb_cancel_to(u, DRV_USB_URB_TIMEOUT);
		sched_yield();
	}

	/* Handles the sync buffer availability. */
	drained = drv_usb_urb_drain(u, 1000U);
	if (drained != 0 && u->sync_buffer != NULL)
		io_stats_record(IO_USB_BUFFER_RETAINED, u->sync_capacity);

	/* Handles the sync buffer availability. */
	if (drained != 0 && u->sync_buffer == NULL && u->length != 0) {
		/*
		 * Legacy unbuffered users still own their buffer until
		 * retirement. All core synchronous helpers and storage reserve
		 * staging.
		 */
		(void)drv_usb_urb_drain(u, 0);
		drained = 0;
	}

	/* Handles the sync client availability. */
	if (drained == 0 && u->sync_client != NULL &&
	    u->sync_client != u->sync_buffer && u->actual_length <= u->length &&
	    ((u->endpoint->type == DRV_USB_TRANSFER_CONTROL &&
	      (u->control.request_type & DRV_USB_DIR_IN) != 0) ||
	     (u->endpoint->type != DRV_USB_TRANSFER_CONTROL &&
	      (u->endpoint->descriptor.address & DRV_USB_DIR_IN) != 0))) {
		memcpy(u->sync_client, u->sync_buffer, u->actual_length);
		io_stats_record(IO_USB_STAGING_COPY, u->actual_length);
	}

	u->sync_client = NULL;

	/* Returns the computed result. */
	return error != 0 ? error : drained;
}

/*
 * Reports how a completed transfer ended.
 */
enum drv_usb_urb_status
drv_usb_urb_status(
	const struct drv_usb_urb *u)
{
	enum drv_usb_urb_status function_result;

	/* Computes the function result. */
	function_result =
		u ? hal_atomic_load_acquire(&u->status) : DRV_USB_URB_IO_ERROR;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Reports how many bytes a transfer moved.
 */
size_t
drv_usb_urb_actual_length(
	const struct drv_usb_urb *u)
{
	enum drv_usb_urb_status status;

	/* Handles the u condition. */
	if (!u)
		return 0;
	status = hal_atomic_load_acquire(&u->status);

	/* Returns the computed result. */
	return status == DRV_USB_URB_PENDING ? 0 : u->actual_length;
}

/*
 * Reports the buffer a transfer uses.
 */
void *
drv_usb_urb_buffer(
	const struct drv_usb_urb *u)
{
	/* Returns the computed result. */
	return u ? u->buffer : NULL;
}

/*
 * Reports how many bytes a transfer asked for.
 */
size_t
drv_usb_urb_length(
	const struct drv_usb_urb *u)
{
	/* Returns the computed result. */
	return u ? u->length : 0;
}

/*
 * Reports the flags a transfer was described with.
 */
unsigned
drv_usb_urb_flags(
	const struct drv_usb_urb *u)
{
	/* Returns the computed result. */
	return u ? u->flags : 0;
}

/*
 * Reports the request a control transfer carries.
 */
const struct drv_usb_control_request *
drv_usb_urb_control_request(
	const struct drv_usb_urb *u)
{
	/* Returns the computed result. */
	return u && u->endpoint->type == DRV_USB_TRANSFER_CONTROL ? &u->control
								  : NULL;
}

/*
 * Reports the controller's own state for this transfer.
 */
void *
drv_usb_urb_hcd_data(
	const struct drv_usb_urb *u)
{
	/* Returns the computed result. */
	return u ? (void *)u->hcd_private[0] : NULL;
}

/*
 * Gives the controller somewhere to keep that state.
 */
int
drv_usb_urb_set_hcd_data(
	struct drv_usb_urb *u,
	void *d)
{
	/* Handles the u condition. */
	if (!u)
		return EINVAL;
	u->hcd_private[0] = (uintptr_t)d;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the device a transfer is addressed to.
 */
struct drv_usb_device *
drv_usb_urb_device(
	const struct drv_usb_urb *u)
{
	/* Returns the computed result. */
	return u ? u->device : NULL;
}

/*
 * Reports the endpoint a transfer is addressed to.
 */
struct drv_usb_endpoint *
drv_usb_urb_endpoint(
	const struct drv_usb_urb *u)
{
	/* Returns the computed result. */
	return u ? u->endpoint : NULL;
}

/*
 * Runs one control transfer and waits for it.
 */
int
drv_usb_control(
	struct drv_usb_device *device,
	uint8_t request_type,
	uint8_t request,
	uint16_t value,
	uint16_t index,
	void *buffer,
	size_t length,
	unsigned timeout_ms,
	size_t *actual)
{
	int error;

	/* Handles the device availability. */
	if (device == NULL || length > UINT16_MAX)
		return EINVAL;

	/* Checks the operation status. */
	error = device_control_lock(device, timeout_ms);
	if (error != 0)
		return error;
	error = usb_control_locked(device, request_type, request, value, index,
				   buffer, length, timeout_ms, actual);
	device_control_unlock(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Runs one bulk transfer and waits for it.
 */
int
drv_usb_bulk(
	struct drv_usb_device *d,
	struct drv_usb_endpoint *e,
	void *b,
	size_t n,
	unsigned t,
	size_t *a)
{
	int error;

	/* Computes the function result. */
	error = e && e->type == DRV_USB_TRANSFER_BULK
				  ? sync_data(d, e, b, n, t, a)
				  : EINVAL;

	/* Returns the computed result. */
	return error;
}

/*
 * Runs one interrupt transfer and waits for it.
 */
int
drv_usb_interrupt(
	struct drv_usb_device *d,
	struct drv_usb_endpoint *e,
	void *b,
	size_t n,
	unsigned t,
	size_t *a)
{
	int error;

	/* Computes the function result. */
	error = e && e->type == DRV_USB_TRANSFER_INTERRUPT
				  ? sync_data(d, e, b, n, t, a)
				  : EINVAL;

	/* Returns the computed result. */
	return error;
}

/*
 * Reports the descriptor of one configuration.
 */
const struct drv_usb_configuration_descriptor *
drv_usb_configuration_descriptor(
	const struct drv_usb_configuration *c)
{
	/* Returns the computed result. */
	return c ? &c->descriptor : NULL;
}

/*
 * Reports how many configurations a device offers.
 */
unsigned
drv_usb_device_configuration_count(
	const struct drv_usb_device *d)
{
	/* Returns the computed result. */
	return d ? d->configuration_count : 0;
}

/*
 * Reports one configuration of a device by its index.
 */
struct drv_usb_configuration *
drv_usb_device_configuration(
	struct drv_usb_device *d,
	unsigned i)
{
	/* Returns the computed result. */
	return d && i < d->configuration_count ? &d->configurations[i] : NULL;
}

/*
 * Reports the configuration a device currently presents.
 */
struct drv_usb_configuration *
drv_usb_device_active_configuration(
	struct drv_usb_device *d)
{
	struct drv_usb_configuration *function_result;

	/* Computes the function result. */
	function_result = d ? device_active_configuration(d) : NULL;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Reports the descriptor bytes a configuration was read as.
 */
const void *
drv_usb_configuration_raw_descriptors(
	const struct drv_usb_configuration *configuration,
	size_t *length)
{
	/* Handles the length availability. */
	if (length != NULL)
		*length = configuration == NULL ? 0 : configuration->raw_length;
	/* Returns the computed result. */
	return configuration == NULL ? NULL : configuration->raw;
}

/*
 * Reports how many interfaces a configuration holds.
 */
unsigned
drv_usb_configuration_interface_count(
	const struct drv_usb_configuration *configuration)
{
	/* Returns the computed result. */
	return configuration == NULL ? 0 : configuration->interface_count;
}

/*
 * Reports one interface of a configuration by its index.
 */
struct drv_usb_interface *
drv_usb_configuration_interface(
	struct drv_usb_configuration *configuration,
	unsigned index)
{
	struct drv_usb_interface *interface;

	/* Handles the configuration availability. */
	if (configuration == NULL)
		return NULL;
	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Checks the current index. */
		if (index-- == 0)
			return interface;
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Finds an interface of a configuration by its number.
 */
struct drv_usb_interface *
drv_usb_configuration_find_interface(
	struct drv_usb_configuration *configuration,
	unsigned interface_number)
{
	struct drv_usb_interface *function_result;

	/* Computes the function result. */
	function_result = configuration == NULL
				  ? NULL
				  : configuration_find_interface(
					    configuration, interface_number);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Reports how many interface associations a configuration holds.
 */
unsigned
drv_usb_configuration_iad_count(
	const struct drv_usb_configuration *configuration)
{
	/* Returns the computed result. */
	return configuration == NULL ? 0 : configuration->iad_count;
}

/*
 * Reports one of those associations by its index.
 */
const struct drv_usb_interface_association_descriptor *
drv_usb_configuration_iad(
	const struct drv_usb_configuration *configuration,
	unsigned index)
{
	/* Returns the computed result. */
	return configuration != NULL && index < configuration->iad_count
		       ? &configuration->iads[index]
		       : NULL;
}

/*
 * Reports the device an interface belongs to.
 */
struct drv_usb_device *
drv_usb_interface_device(
	const struct drv_usb_interface *i)
{
	/* Returns the computed result. */
	return i ? i->device : NULL;
}

/*
 * Reports the descriptor of an interface's active setting.
 */
const struct drv_usb_interface_descriptor *
drv_usb_interface_descriptor(
	const struct drv_usb_interface *interface)
{
	const struct drv_usb_host_interface *alternate =
		interface == NULL ? NULL
				  : interface_active_alternate(interface);

	/* Returns the computed result. */
	return alternate == NULL ? NULL : &alternate->descriptor;
}

/*
 * Reports the number an interface was given.
 */
unsigned
drv_usb_interface_number(
	const struct drv_usb_interface *interface)
{
	/* Returns the computed result. */
	return interface == NULL || interface->alternates == NULL
		       ? 0
		       : interface->alternates->descriptor.interface_number;
}

/*
 * Reports how many alternate settings an interface has.
 */
unsigned
drv_usb_interface_alternate_count(
	const struct drv_usb_interface *i)
{
	/* Returns the computed result. */
	return i ? i->alternate_count : 0;
}

/*
 * Reports the setting an interface currently presents.
 */
const struct drv_usb_host_interface *
drv_usb_interface_active_alternate(
	const struct drv_usb_interface *interface)
{
	const struct drv_usb_host_interface *function_result;

	/* Computes the function result. */
	function_result =
		interface == NULL || device_is_quarantined(interface->device)
			? NULL
			: interface_active_alternate(interface);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Reports one alternate setting by its index.
 */
const struct drv_usb_host_interface *
drv_usb_interface_alternate(
	const struct drv_usb_interface *interface,
	unsigned index)
{
	const struct drv_usb_host_interface *alternate;

	/* Handles the interface availability. */
	if (interface == NULL)
		return NULL;
	/* Process each linked entry. */
	for (alternate = interface->alternates; alternate != NULL;
	     alternate = alternate->next) {
		/* Checks the current index. */
		if (index-- == 0)
			return alternate;
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Finds an alternate setting by its number.
 */
const struct drv_usb_host_interface *
drv_usb_interface_find_alternate(
	const struct drv_usb_interface *interface,
	unsigned alternate_setting)
{
	const struct drv_usb_host_interface *function_result;

	/* Computes the function result. */
	function_result =
		interface == NULL
			? NULL
			: interface_find_alternate(
				  (struct drv_usb_interface *)interface,
				  alternate_setting);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Selects which alternate setting an interface presents.
 */
int
drv_usb_interface_set_alternate(
	struct drv_usb_interface *interface,
	unsigned alternate_setting)
{
	struct drv_usb_device *device;
	struct drv_usb_host_interface *old, *target;
	size_t actual = 0;
	int error, rollback_error;
	int control_locked = 0, interface_locked = 0, selection_locked = 0;

	/* Handles the interface availability. */
	if (interface == NULL)
		return EINVAL;
	device = interface->device;

	/* Handles the target availability. */
	target = interface_find_alternate(interface, alternate_setting);
	if (target == NULL)
		return ENOENT;

	/* Handles the device is quarantined condition. */
	if (device_is_quarantined(device) || device_is_disconnecting(device))
		return EBUSY;

	/* Checks the atomic try acquire zero result. */
	if (!atomic_try_acquire_zero(&device->selection_gate))
		return EBUSY;
	selection_locked = 1;

	/* Handles the device is quarantined condition. */
	if (device_is_quarantined(device) || device_is_disconnecting(device)) {
		error = EBUSY;
		goto out;
	}

	/* Checks the device active configuration result. */
	if (device_active_configuration(device) != interface->configuration ||
	    device->state != DRV_USB_STATE_CONFIGURED) {
		error = ENODEV;
		goto out;
	}

	/* Handles the old condition. */
	old = interface_active_alternate(interface);
	if (old == target) {
		error = 0;
		goto out;
	}

	/* Checks the operation status. */
	error = io_gate_close_empty(&interface->io_gate);
	if (error != 0)
		goto out;
	interface_locked = 1;

	/* Checks the operation status. */
	error = device_control_try_lock(device);
	if (error != 0)
		goto out;
	control_locked = 1;

	/* Checks the operation status. */
	error = host_interface_disable(old);
	if (error != 0)
		goto out;

	/* Checks the operation status. */
	error = usb_control_locked(device,
				   DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
					   DRV_USB_RECIP_INTERFACE,
				   USB_REQ_SET_INTERFACE,
				   target->descriptor.alternate_setting,
				   old->descriptor.interface_number, NULL, 0,
				   USB_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0) {
		/*
		 * A STALL leaves the old alternate selected. A timeout or I/O
		 * failure is ambiguous, so retain no published HCD endpoint
		 * set.
		 */
		if (error != EPIPE) {
			device_quarantine_selection(device, "set-interface",
						    error);
			goto out;
		}

		/* Checks the operation status. */
		rollback_error = host_interface_enable(old);
		if (rollback_error != 0) {
			device_quarantine_selection(device, "set-interface",
						    rollback_error);
		}

		goto out;
	}

	/* Checks the operation status. */
	error = host_interface_enable(target);
	if (error != 0) {
		/* Checks the device is quarantined result. */
		rollback_error = 0;
		if (!device_is_quarantined(device)) {
			rollback_error = usb_control_locked(
				device,
				DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
					DRV_USB_RECIP_INTERFACE,
				USB_REQ_SET_INTERFACE,
				old->descriptor.alternate_setting,
				old->descriptor.interface_number, NULL, 0,
				USB_CONTROL_TIMEOUT_MS, &actual);
		}
		if (rollback_error == 0 && !device_is_quarantined(device)) {
			/* Checks the operation status. */
			rollback_error = host_interface_enable(old);
			if (rollback_error == 0) {
				rollback_error =
					host_interface_reset_endpoints(old);
			}
		}
		if (rollback_error != 0) {
			device_quarantine_selection(device, "alternate-enable",
						    rollback_error);
		}

		goto out;
	}

	/* Checks the operation status. */
	error = host_interface_reset_endpoints(target);
	if (error != 0) {
		/*
		 * SET_INTERFACE has succeeded.  A host reset failure leaves the
		 * selected endpoint state uncertain and cannot be rolled back
		 * by a schedule-only enable.
		 */
		device_quarantine_selection(device, "alternate-reset", error);
		goto out;
	}

	interface_publish_alternate(interface, target);
	error = 0;

out:

	/* Handles the control locked condition. */
	if (control_locked)
		device_control_unlock(device);

	/* Handles the interface locked condition. */
	if (interface_locked)
		io_gate_open(&interface->io_gate);

	/* Handles the selection locked condition. */
	if (selection_locked)
		atomic_store_release(&device->selection_gate, 0U);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Claims an interface for one driver.
 */
int
drv_usb_interface_claim(
	struct drv_usb_interface *owner,
	struct drv_usb_interface *target)
{
	struct drv_usb_device *device;
	unsigned owner_state;
	int error;

	/* Handles the owner availability. */
	if (owner == NULL || target == NULL || owner == target ||
	    owner->device != target->device ||
	    owner->configuration != target->configuration) {
		/* Failed. */
		return EINVAL;
	}
	device = owner->device;

	/* Checks the operation status. */
	error = device_binding_enter(device);
	if (error != 0)
		return error;

	/* Checks the device active configuration result. */
	if (device_active_configuration(device) != owner->configuration) {
		error = EINVAL;
		goto out_device;
	}

	/* Handles the device is quarantined condition. */
	if (device_is_quarantined(device)) {
		error = ENODEV;
		goto out_device;
	}

	/* Handles the owner state condition. */
	owner_state = atomic_load_acquire(&owner->binding_state);
	if (owner_state != USB_BINDING_PROBING &&
	    owner_state != USB_BINDING_BOUND) {
		error = EPERM;
		goto out_device;
	}

	/* Checks the operation status. */
	error = io_gate_enter(&owner->binding_submitters);
	if (error != 0)
		goto out_device;

	/* Checks the atomic load acquire result. */
	if (atomic_load_acquire(&owner->binding_state) != owner_state) {
		binding_submitter_put(owner);
		error = EBUSY;
		goto out_device;
	}

	/* Checks the operation status. */
	error = io_gate_close_empty(&target->io_gate);
	if (error != 0) {
		binding_submitter_put(owner);
		goto out_device;
	}

	/* Checks the interface claim owner result. */
	if (target->driver != NULL || interface_claim_owner(target) != NULL ||
	    atomic_load_acquire(&target->binding_state) != USB_BINDING_DEAD) {
		io_gate_open(&target->io_gate);
		binding_submitter_put(owner);
		error = EBUSY;
		goto out_device;
	}

	interface_publish_claim(target, owner);
	io_gate_open(&target->io_gate);
	binding_submitter_put(owner);
	error = 0;

out_device:
	device_binding_exit(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Gives a claimed interface back.
 */
int
drv_usb_interface_release(
	struct drv_usb_interface *owner,
	struct drv_usb_interface *target)
{
	struct drv_usb_device *device;
	unsigned owner_state;
	int error;

	/* Handles the owner availability. */
	if (owner == NULL || target == NULL || owner->device != target->device)
		return EINVAL;
	device = owner->device;

	/* Checks the operation status. */
	error = device_binding_enter(device);
	if (error != 0)
		return error;

	/* Checks the interface claim owner result. */
	if (interface_claim_owner(target) != owner) {
		error = EPERM;
		goto out_device;
	}

	/* Handles the owner state condition. */
	owner_state = atomic_load_acquire(&owner->binding_state);
	if (owner_state != USB_BINDING_PROBING &&
	    owner_state != USB_BINDING_BOUND) {
		error = EPERM;
		goto out_device;
	}

	/* Checks the operation status. */
	error = io_gate_enter(&owner->binding_submitters);
	if (error != 0)
		goto out_device;

	/* Checks the atomic load acquire result. */
	if (atomic_load_acquire(&owner->binding_state) != owner_state) {
		binding_submitter_put(owner);
		error = EBUSY;
		goto out_device;
	}

	/* Checks the io gate close empty result. */
	if (io_gate_close_empty(&target->io_gate) != 0) {
		binding_submitter_put(owner);
		error = EBUSY;
		goto out_device;
	}

	/* Checks the interface claim owner result. */
	if (interface_claim_owner(target) != owner) {
		io_gate_open(&target->io_gate);
		binding_submitter_put(owner);
		error = EPERM;
		goto out_device;
	}

	interface_publish_claim(target, NULL);
	io_gate_open(&target->io_gate);
	binding_submitter_put(owner);
	error = 0;

out_device:
	device_binding_exit(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Asks whether a given driver holds an interface.
 */
struct drv_usb_interface *
drv_usb_interface_claimed_by(
	const struct drv_usb_interface *interface)
{
	struct drv_usb_interface *function_result;

	/* Computes the function result. */
	function_result =
		interface == NULL ? NULL : interface_claim_owner(interface);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Reports the driver that holds an interface.
 */
struct drv_usb_driver *
drv_usb_interface_driver(
	const struct drv_usb_interface *i)
{
	/* Returns the computed result. */
	return i ? i->driver : NULL;
}

/*
 * Reports that driver's own state for the interface.
 */
void *
drv_usb_interface_driver_data(
	const struct drv_usb_interface *i)
{
	/* Returns the computed result. */
	return i ? i->driver_data : NULL;
}

/*
 * Gives the driver somewhere to keep that state.
 */
int
drv_usb_interface_set_driver_data(
	struct drv_usb_interface *i,
	void *d)
{
	/* Checks the current index. */
	if (!i)
		return EINVAL;
	i->driver_data = d;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the descriptor of one alternate setting.
 */
const struct drv_usb_interface_descriptor *
drv_usb_host_interface_descriptor(
	const struct drv_usb_host_interface *alternate)
{
	/* Returns the computed result. */
	return alternate == NULL ? NULL : &alternate->descriptor;
}

/*
 * Reports how many endpoints one setting has.
 */
unsigned
drv_usb_host_interface_endpoint_count(
	const struct drv_usb_host_interface *alternate)
{
	/* Returns the computed result. */
	return alternate == NULL ? 0 : alternate->endpoint_count;
}

/*
 * Reports one endpoint of a setting by its index.
 */
struct drv_usb_endpoint *
drv_usb_host_interface_endpoint(
	const struct drv_usb_host_interface *alternate,
	unsigned index)
{
	/* Returns the computed result. */
	return alternate != NULL && index < alternate->endpoint_count
		       ? &alternate->endpoints[index]
		       : NULL;
}

/*
 * Reports how many extra descriptors a setting carries.
 */
unsigned
drv_usb_host_interface_extra_count(
	const struct drv_usb_host_interface *alternate)
{
	/* Returns the computed result. */
	return alternate == NULL ? 0 : alternate->extra_count;
}

/*
 * Reports one of those descriptors by its index.
 */
int
drv_usb_host_interface_extra(
	const struct drv_usb_host_interface *alternate,
	unsigned index,
	const void **descriptor,
	size_t *length)
{
	uint8_t descriptor_length;
	uint8_t descriptor_type;
	const struct drv_usb_configuration *configuration;
	const uint8_t *raw;
	size_t offset, end;

	/* Handles the alternate availability. */
	if (alternate == NULL || descriptor == NULL || length == NULL)
		return EINVAL;
	configuration = alternate->interface->configuration;
	raw = configuration->raw;
	offset = alternate->raw_offset + raw[alternate->raw_offset];
	end = alternate->raw_offset + alternate->raw_length;
	/* Continue while the operation condition remains true. */
	while (offset < end) {
		descriptor_length = raw[offset];

		/* Handles the descriptor type condition. */
		descriptor_type = raw[offset + 1U];
		if (descriptor_type != DRV_USB_DESCRIPTOR_ENDPOINT &&
		    descriptor_type !=
			    DRV_USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION &&
		    descriptor_type !=
			    DRV_USB_DESCRIPTOR_INTERFACE_ASSOCIATION) {
			/* Checks the current index. */
			if (index-- == 0) {
				*descriptor = raw + offset;
				*length = descriptor_length;
				/* Succeeded. */
				return 0;
			}
		}

		offset += descriptor_length;
	}

	/* Failed. */
	return ENOENT;
}

/*
 * Reports how many endpoints an interface has active.
 */
unsigned
drv_usb_interface_endpoint_count(
	const struct drv_usb_interface *interface)
{
	const struct drv_usb_host_interface *alternate =
		interface == NULL ? NULL
				  : interface_active_alternate(interface);

	/* Returns the computed result. */
	return alternate == NULL ? 0 : alternate->endpoint_count;
}

/*
 * Reports one active endpoint by its index.
 */
struct drv_usb_endpoint *
drv_usb_interface_endpoint(
	struct drv_usb_interface *interface,
	unsigned index)
{
	struct drv_usb_host_interface *alternate =
		interface == NULL ? NULL
				  : interface_active_alternate(interface);

	/* Returns the computed result. */
	return alternate != NULL && index < alternate->endpoint_count
		       ? &alternate->endpoints[index]
		       : NULL;
}

/*
 * Finds an active endpoint by its address.
 */
struct drv_usb_endpoint *
drv_usb_interface_find_endpoint(
	struct drv_usb_interface *interface,
	enum drv_usb_transfer_type type,
	uint8_t direction,
	struct drv_usb_endpoint *after)
{
	struct drv_usb_host_interface *alternate;
	unsigned index = 0;
	int found = after == NULL;

	/* Handles the interface availability. */
	if (interface == NULL)
		return NULL;

	/* Handles the alternate availability. */
	alternate = interface_active_alternate(interface);
	if (alternate == NULL)
		return NULL;

	/* Handles the after availability. */
	if (after != NULL) {
		/* Process each remaining element. */
		for (; index < alternate->endpoint_count; index++) {
			/* Handles the alternate condition. */
			if (&alternate->endpoints[index] == after) {
				index++;
				found = 1;
				break;
			}
		}
	}

	/* Handles the found condition. */
	if (!found)
		return NULL;
	/* Process each remaining element. */
	for (; index < alternate->endpoint_count; index++) {
		/* Handles the alternate condition. */
		if (alternate->endpoints[index].type == type &&
		    (alternate->endpoints[index].descriptor.address &
		     DRV_USB_DIR_IN) == direction) {
			/* Returns the computed result. */
			return &alternate->endpoints[index];
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Reports the device an endpoint belongs to.
 */
struct drv_usb_device *
drv_usb_endpoint_device(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e && e->interface ? e->interface->device : NULL;
}

/*
 * Reports the descriptor of an endpoint.
 */
const struct drv_usb_endpoint_descriptor *
drv_usb_endpoint_descriptor(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e ? &e->descriptor : NULL;
}

/*
 * Reports which of the four transfer types an endpoint is.
 */
enum drv_usb_transfer_type
drv_usb_endpoint_type(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e ? e->type : DRV_USB_TRANSFER_CONTROL;
}

/*
 * Reports the address an endpoint answers on.
 */
uint8_t
drv_usb_endpoint_address(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e ? e->descriptor.address : 0;
}

/*
 * Reports the largest packet an endpoint takes.
 */
uint16_t
drv_usb_endpoint_max_packet_size(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e ? e->descriptor.maximum_packet_size : 0;
}

/*
 * Reports how many packets an endpoint takes in a burst.
 */
uint8_t
drv_usb_endpoint_maximum_burst(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e && e->companion_valid ? e->companion.maximum_burst : 0;
}

/*
 * Reports the companion descriptor a SuperSpeed endpoint has.
 */
const struct drv_usb_superspeed_endpoint_companion_descriptor *
drv_usb_endpoint_superspeed_companion(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e && e->companion_valid ? &e->companion : NULL;
}

/*
 * Asks whether an endpoint carries data toward the host.
 */
bool
drv_usb_endpoint_is_input(
	const struct drv_usb_endpoint *e)
{
	/* Returns the computed result. */
	return e && (e->descriptor.address & DRV_USB_DIR_IN) != 0;
}

/*
 * Reports the controller's own state for this endpoint.
 */
uintptr_t
drv_usb_endpoint_hcd_data(
	const struct drv_usb_endpoint *e,
	unsigned n)
{
	/* Returns the computed result. */
	return e && n < 4U ? e->hcd_private[n] : 0;
}

/*
 * Gives the controller somewhere to keep that state.
 */
int
drv_usb_endpoint_set_hcd_data(
	struct drv_usb_endpoint *e,
	unsigned n,
	uintptr_t value)
{
	/* Handles the e condition. */
	if (!e || n >= 4U)
		return EINVAL;
	e->hcd_private[n] = value;

	/* Succeeded. */
	return 0;
}

/*
 * Clears the halt an endpoint has fallen into.
 */
int
drv_usb_endpoint_clear_halt(
	struct drv_usb_endpoint *endpoint)
{
	struct drv_usb_device *device;
	struct drv_usb_interface *interface, *owner = NULL;
	unsigned accepted = 0;
	int binding_entered = 0, selection_locked = 0;
	int interface_locked = 0, control_locked = 0;
	int error;

	/* Handles the endpoint availability. */
	if (endpoint == NULL || endpoint->interface == NULL ||
	    endpoint->alternate == NULL ||
	    (endpoint->type != DRV_USB_TRANSFER_BULK &&
	     endpoint->type != DRV_USB_TRANSFER_INTERRUPT) ||
	    (endpoint->descriptor.address & 0x0fU) == 0) {
		/* Failed. */
		return EINVAL;
	}
	interface = endpoint->interface;

	/* Checks the endpoint retained by device result. */
	device = interface->device;
	if (!endpoint_retained_by_device(device, endpoint))
		return ENODEV;

	/* Checks the device is disconnecting result. */
	if (device_is_disconnecting(device) || device_is_quarantined(device) ||
	    hal_atomic_load_acquire(&device->bus->stopping) != 0) {
		/* Failed. */
		return ENODEV;
	}

	/* Checks the operation status. */
	error = device_binding_enter(device);
	if (error != 0)
		return error;
	binding_entered = 1;

	/* Checks the operation status. */
	error = endpoint_binding_pin(interface, &owner);
	if (error != 0)
		goto out;

	/* Checks the atomic try acquire zero result. */
	if (!atomic_try_acquire_zero(&device->selection_gate)) {
		error = EBUSY;
		goto out;
	}

	selection_locked = 1;

	/* Checks the device is disconnecting result. */
	if (device_is_disconnecting(device) || device_is_quarantined(device) ||
	    hal_atomic_load_acquire(&device->bus->stopping) != 0 ||
	    device->state != DRV_USB_STATE_CONFIGURED ||
	    device_active_configuration(device) != interface->configuration ||
	    interface_active_alternate(interface) != endpoint->alternate ||
	    interface_binding_owner(interface) != owner) {
		error = ENODEV;
		goto out;
	}

	/* Checks the operation status. */
	error = io_gate_close_empty(&interface->io_gate);
	if (error != 0)
		goto out;
	interface_locked = 1;

	/* Checks the operation status. */
	error = device_control_try_lock(device);
	if (error != 0)
		goto out;
	control_locked = 1;

	/* Checks the device is disconnecting result. */
	if (device_is_disconnecting(device) || device_is_quarantined(device) ||
	    hal_atomic_load_acquire(&device->bus->stopping) != 0) {
		error = ENODEV;
		goto out;
	}

	/* Checks the operation status. */
	error = endpoint_clear_halt_request(device, endpoint, &accepted);
	if (error != 0) {
		/* Checks the operation status. */
		if (accepted && error != EPIPE &&
		    !device_is_disconnecting(device) &&
		    hal_atomic_load_acquire(&device->bus->stopping) == 0) {
			device_quarantine_recovery(device, "clear-halt wire",
						   error);
		} else if (device_is_disconnecting(device) ||
			   hal_atomic_load_acquire(&device->bus->stopping) != 0)
			error = ENODEV;
		goto out;
	}

	/* Checks the device is disconnecting result. */
	if (device_is_disconnecting(device) ||
	    hal_atomic_load_acquire(&device->bus->stopping) != 0) {
		error = ENODEV;
		goto out;
	}

	/* Checks the operation status. */
	error = device->bus->hcd->ops->endpoint_reset(device->bus->hcd,
						      endpoint);
	if (error != 0) {
		/* Checks the device is disconnecting result. */
		if (device_is_disconnecting(device) ||
		    hal_atomic_load_acquire(&device->bus->stopping) != 0)
			error = ENODEV;
		else
			device_quarantine_recovery(device, "endpoint reset",
						   error);
		goto out;
	}

	/* Checks the device is disconnecting result. */
	if (device_is_disconnecting(device) ||
	    hal_atomic_load_acquire(&device->bus->stopping) != 0) {
		error = ENODEV;
		goto out;
	}

	endpoint_publish_halted(device, endpoint, 0U);
	error = 0;

out:

	/* Handles the control locked condition. */
	if (control_locked)
		device_control_unlock(device);

	/* Handles the interface locked condition. */
	if (interface_locked)
		io_gate_open(&interface->io_gate);

	/* Handles the selection locked condition. */
	if (selection_locked)
		atomic_store_release(&device->selection_gate, 0U);
	endpoint_binding_unpin(owner);

	/* Handles the binding entered condition. */
	if (binding_entered)
		device_binding_exit(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Asks whether one identifier pattern matches an interface.
 */
int
drv_usb_id_match(
	const struct drv_usb_id *id,
	const struct drv_usb_interface *interface)
{
	const struct drv_usb_device_descriptor *device_descriptor;
	const struct drv_usb_host_interface *alternate;
	const struct drv_usb_interface_descriptor *interface_descriptor;

	/* Handles the id availability. */
	if (id == NULL || interface == NULL)
		return 0;

	/* Handles the alternate availability. */
	alternate = interface_active_alternate(interface);
	if (alternate == NULL)
		return 0;
	device_descriptor = &interface->device->descriptor;
	interface_descriptor = &alternate->descriptor;

	/* Returns the computed result. */
	return (!(id->match_flags & DRV_USB_ID_VENDOR) ||
		id->vendor == device_descriptor->vendor) &&
	       (!(id->match_flags & DRV_USB_ID_PRODUCT) ||
		id->product == device_descriptor->product) &&
	       (!(id->match_flags & DRV_USB_ID_RELEASE_RANGE) ||
		(device_descriptor->device_release >= id->release_minimum &&
		 device_descriptor->device_release <= id->release_maximum)) &&
	       (!(id->match_flags & DRV_USB_ID_DEVICE_CLASS) ||
		id->device_class == device_descriptor->device_class) &&
	       (!(id->match_flags & DRV_USB_ID_DEVICE_SUBCLASS) ||
		id->device_subclass == device_descriptor->device_subclass) &&
	       (!(id->match_flags & DRV_USB_ID_DEVICE_PROTOCOL) ||
		id->device_protocol == device_descriptor->device_protocol) &&
	       (!(id->match_flags & DRV_USB_ID_IF_CLASS) ||
		id->interface_class == interface_descriptor->interface_class) &&
	       (!(id->match_flags & DRV_USB_ID_IF_SUBCLASS) ||
		id->interface_subclass ==
			interface_descriptor->interface_subclass) &&
	       (!(id->match_flags & DRV_USB_ID_IF_PROTOCOL) ||
		id->interface_protocol ==
			interface_descriptor->interface_protocol) &&
	       (!(id->match_flags & DRV_USB_ID_IF_NUMBER) ||
		id->interface_number == interface_descriptor->interface_number);
}

/*
 * Finds the pattern of a driver that matches an interface.
 */
const struct drv_usb_id *
drv_usb_driver_find_id(
	const struct drv_usb_driver *d,
	const struct drv_usb_interface *i)
{
	size_t n;

	/* Checks the current descriptor. */
	if (!d || !i)
		return NULL;
	/* Process each remaining element. */
	for (n = 0; n < d->id_count; n++) {
		/* Checks the drv usb id match result. */
		if (drv_usb_id_match(&d->ids[n], i))
			return &d->ids[n];
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Offers an interface to every registered driver in turn.
 */
int
drv_usb_interface_probe(
	struct drv_usb_interface *interface)
{
	int error;

	/* Obtains the interface probe internal result. */
	error = interface_probe_internal(interface, NULL);

	/* Returns the computed result. */
	return error;
}

/*
 * Tells the driver holding an interface to give it up.
 */
int
drv_usb_interface_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct drv_usb_device *device;
	unsigned state;
	int error;

	/* Handles the interface availability. */
	if (interface == NULL)
		return EINVAL;
	device = interface->device;

	/* Checks the operation status. */
	error = device_binding_enter(device);
	if (error != 0)
		return error;

	/* Handles the driver availability. */
	state = atomic_load_acquire(&interface->binding_state);
	if (interface->driver == NULL || state == USB_BINDING_DEAD)
		error = EINVAL;
	else if (state != USB_BINDING_BOUND &&
		 state != USB_BINDING_DETACH_PENDING)
		error = EBUSY;
	else
		error = interface_binding_detach(interface, flags, state);
	device_binding_exit(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Registers a driver with the USB subsystem.
 */
int
drv_usb_driver_register(
	struct drv_usb_driver *d)
{
	struct usb_driver_entry *e;

	/* Handles the usb initialized condition. */
	if (!usb_initialized || !d || !d->name)
		return EINVAL;
	/* Process each linked entry. */
	for (e = usb_drivers; e; e = e->next) {
		/* Handles the e condition. */
		if (e->driver == d)
			return EEXIST;
	}

	/* Handles the e condition. */
	e = hal_malloc(sizeof(*e));
	if (!e)
		return ENOMEM;
	e->driver = d;
	e->next = usb_drivers;
	usb_drivers = e;

	/* Succeeded. */
	return 0;
}

/*
 * Takes a driver back out of the subsystem.
 */
int
drv_usb_driver_unregister(
	struct drv_usb_driver *d)
{
	struct usb_driver_entry **p, *e;

	/* Checks the current descriptor. */
	if (!d)
		return EINVAL;
	/* Process each linked entry. */
	for (p = &usb_drivers; (e = *p) != NULL; p = &e->next) {
		/* Handles the e condition. */
		if (e->driver == d) {
			*p = e->next;
			hal_free(e);

			/* Succeeded. */
			return 0;
		}
	}

	/* Failed. */
	return ENOENT;
}

/*
 * Prints the whole device tree for debugging.
 */
void
drv_usb_dump(
	void)
{
	struct drv_usb_bus *b;

	/* Process each linked entry. */
	for (b = usb_buses; b; b = b->next) {
		hal_printf("usb%u: hcd=%s ports=%u\n", b->number, b->hcd->name,
			   b->hcd->root_port_count);
	}
}

/* Takes the lock that guards the whole device tree. */
static void
usb_topology_lock(
	void)
{
	/* Continue while the operation condition remains true. */
	while (!atomic_try_acquire_zero(&usb_topology_gate))
		sched_yield();
}

/* Gives that lock back. */
static void
usb_topology_unlock(
	void)
{
	atomic_store_release(&usb_topology_gate, 0U);
}

/* Marks a device as disconnecting, once and only once. */
static void
device_begin_disconnect(
	struct drv_usb_device *device)
{
	unsigned expected = 0;

	/* Checks the atomic compare exchange result. */
	if (!atomic_compare_exchange(&device->disconnect_barrier, &expected,
				     USB_DISCONNECT_BARRIER_RUNNING)) {
		/* Continue while the operation condition remains true. */
		while (atomic_load_acquire(&device->disconnect_barrier) !=
		       USB_DISCONNECT_BARRIER_DONE)
			sched_yield();

		/* Returns the computed result. */
		return;
	}

	(void)hal_atomic_fetch_or_release(&device->lifecycle,
					  USB_DEVICE_LIFECYCLE_DISCONNECTING);

	/*
	 * Attach/detach may legitimately select an alternate.  First stop and
	 * join every binding transaction, then take the selection gate
	 * permanently.
	 */
	io_gate_close(&device->binding_transactions);
	/* Process each remaining element. */
	while ((atomic_load_acquire(&device->binding_transactions) &
		USB_IO_GATE_COUNT_MASK) != 0)
		sched_yield();

	/*
	 * A selection operation which entered before the lifecycle publication
	 * is allowed to finish.  Keeping its exclusive gate permanently owned
	 * joins that transaction and prevents any later endpoint/control
	 * selection from racing detach or HCD quiesce.
	 */
	while (!atomic_try_acquire_zero(&device->selection_gate))
		sched_yield();
	io_gate_close(&device->submit_gate);
	/* Process each remaining element. */
	while ((atomic_load_acquire(&device->submit_gate) &
		USB_IO_GATE_COUNT_MASK) != 0)
		sched_yield();
	atomic_store_release(&device->disconnect_barrier,
			     USB_DISCONNECT_BARRIER_DONE);
}

/* Closes the gate that lets transfers into a device. */
static void
io_gate_close(
	atomic_uint_t *gate)
{
	unsigned state;

	state = atomic_load_acquire(gate);
	/* Continue while the operation condition remains true. */
	while ((state & USB_IO_GATE_CLOSED) == 0) {
		/* Handles the atomic compare exchange condition. */
		if (atomic_compare_exchange(gate, &state,
					    state | USB_IO_GATE_CLOSED)) {
			/* Returns the computed result. */
			return;
		}
	}
}

/* Tells every driver holding an interface to give it up. */
static int
detach_interfaces(
	struct drv_usb_device *device)
{
	struct drv_usb_interface *interface;
	unsigned state;
	int error, first_error = 0;

	/* Process each linked entry. */
	for (interface = device->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Handles the driver availability. */
		state = atomic_load_acquire(&interface->binding_state);
		if (interface->driver == NULL || state == USB_BINDING_DEAD)
			continue;

		/* Handles the state condition. */
		if (state == USB_BINDING_BOUND ||
		    state == USB_BINDING_DETACH_PENDING) {
			error = interface_binding_detach(
				interface,
				DRV_USB_DETACH_FORCE | DRV_USB_DETACH_QUIET,
				state);
		} else {
			error = EBUSY;
		}

		/*
		 * A failed function must not prevent the remaining functions on
		 * a composite device from closing and draining their own work.
		 */
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* Returns the computed result. */
	return first_error;
}

/* Stops class activity while retaining media still referenced by the root. */
static int
shutdown_interfaces(
	struct drv_usb_device *device,
	int *retain)
{
	struct drv_usb_interface *interface;
	unsigned state;
	int error;
	int first_error;

	/* Visits every function, even when one function cannot finish stopping. */
	first_error = 0;
	for (interface = device->interfaces; interface != NULL;
	     interface = interface->next) {
		state = atomic_load_acquire(&interface->binding_state);
		if (interface->driver == NULL)
			continue;
		if (state == USB_BINDING_DEAD)
			continue;

		/* Uses checked non-destructive stopping when the class supports it. */
		if (state != USB_BINDING_BOUND &&
		    state != USB_BINDING_DETACH_PENDING) {
			error = EBUSY;
		} else if (interface->driver->quiesce != NULL) {
			*retain = 1;
			error = interface_binding_quiesce(interface, state);
		} else {
			error = interface_binding_detach(interface,
				DRV_USB_DETACH_FORCE | DRV_USB_DETACH_QUIET,
				state);
		}

		/* Preserves the first error without leaving other producers running. */
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	return first_error;
}

/* Closes binding admission, joins the class, and keeps referenced objects. */
static int
interface_binding_quiesce(
	struct drv_usb_interface *interface,
	unsigned expected_state)
{
	int error;

	/* Claims the binding against a concurrent detach or class operation. */
	if (!atomic_compare_exchange(&interface->binding_state, &expected_state,
		USB_BINDING_UNBINDING))
		return EBUSY;
	io_gate_close(&interface->binding_gate);
	io_gate_close(&interface->binding_submitters);
	while ((atomic_load_acquire(&interface->binding_submitters) &
		USB_IO_GATE_COUNT_MASK) != 0)
		sched_yield();

	/* A successful class stop must leave no transfer/callback binding pins. */
	error = interface->driver->quiesce(interface);
	if (error == 0 && (atomic_load_acquire(&interface->binding_gate) &
		USB_IO_GATE_COUNT_MASK) != 0)
		error = EBUSY;

	/* Keeps admission closed and ownership available for a checked retry. */
	atomic_store_release(&interface->binding_state,
		USB_BINDING_DETACH_PENDING);
	return error;
}

/* Detaches the driver bound to one interface. */
static int
interface_binding_detach(
	struct drv_usb_interface *interface,
	unsigned flags,
	unsigned expected_state)
{
	int error = 0;

	/* Checks the atomic compare exchange result. */
	if (!atomic_compare_exchange(&interface->binding_state, &expected_state,
				     USB_BINDING_UNBINDING)) {
		/* Returns the computed result. */
		return expected_state == USB_BINDING_DEAD ? EINVAL : EBUSY;
	}
	io_gate_close(&interface->binding_gate);
	io_gate_close(&interface->binding_submitters);
	/* Process each remaining element. */
	while ((atomic_load_acquire(&interface->binding_submitters) &
		USB_IO_GATE_COUNT_MASK) != 0)
		sched_yield();

	/* Handles the detach availability. */
	if (interface->driver->detach != NULL)
		error = interface->driver->detach(interface, flags);
	if (error == 0 && (atomic_load_acquire(&interface->binding_gate) &
			   USB_IO_GATE_COUNT_MASK) != 0)
		error = EBUSY;
	if (error != 0) {
		atomic_store_release(&interface->binding_state,
				     USB_BINDING_DETACH_PENDING);

		/* Failed. */
		return error;
	}

	interface_binding_clear(interface);

	/* Succeeded. */
	return 0;
}

/* Forgets which driver an interface was bound to. */
static void
interface_binding_clear(
	struct drv_usb_interface *interface)
{
	struct drv_usb_interface *sibling;

	/* Process each linked entry. */
	for (sibling = interface->configuration->interfaces; sibling != NULL;
	     sibling = sibling->next) {
		/* Checks the interface claim owner result. */
		if (interface_claim_owner(sibling) == interface) {
			/*
			 * Never reopen a gate owned by an
			 * alternate/configuration transaction.  Wait until that
			 * owner publishes and reopens, then acquire a distinct
			 * closed interval for claim removal.
			 */
			io_gate_close_wait(&sibling->io_gate);
			interface_publish_claim(sibling, NULL);
			io_gate_open(&sibling->io_gate);
		}
	}

	interface->driver = NULL;
	interface->driver_data = NULL;
	atomic_store_release(&interface->binding_gate, USB_IO_GATE_CLOSED);
	atomic_store_release(&interface->binding_submitters,
			     USB_IO_GATE_CLOSED);
	atomic_store_release(&interface->binding_state, USB_BINDING_DEAD);
}

/* Reports the driver that holds an interface. */
static struct drv_usb_interface *
interface_claim_owner(
	const struct drv_usb_interface *interface)
{
	struct drv_usb_interface *function_result;

	/* Obtains the atomic load n result. */
	function_result =
		__atomic_load_n(&interface->claimed_by, __ATOMIC_ACQUIRE);

	/* Returns the computed result. */
	return function_result;
}

/* Waits for the transfers already inside the gate to leave. */
static void
io_gate_close_wait(
	atomic_uint_t *gate)
{
	/* Continue while the operation condition remains true. */
	while (io_gate_close_empty(gate) != 0)
		sched_yield();
}

/* Closes the gate of a device that has no transfer inside it. */
static int
io_gate_close_empty(
	atomic_uint_t *gate)
{
	int error;
	unsigned expected = 0;

	/* Computes the function result. */
	error =
		atomic_compare_exchange(gate, &expected, USB_IO_GATE_CLOSED)
			? 0
			: EBUSY;

	/* Returns the computed result. */
	return error;
}

/* Publishes a driver's claim on an interface. */
static void
interface_publish_claim(
	struct drv_usb_interface *interface,
	struct drv_usb_interface *owner)
{
	__atomic_store_n(&interface->claimed_by, owner, __ATOMIC_RELEASE);
}

/* Opens the gate that lets transfers into a device. */
static void
io_gate_open(
	atomic_uint_t *gate)
{
	unsigned state = atomic_load_acquire(gate);

	/* Handles the state condition. */
	if ((state & USB_IO_GATE_COUNT_MASK) != 0)
		__builtin_trap();
	atomic_store_release(gate, 0U);
}

/* Stops a device and waits for everything it holds to finish. */
static int
device_quiesce(
	struct drv_usb_bus *bus,
	struct drv_usb_device *device)
{
	int error;

	/* Handles the device quiesce availability. */
	if (bus->hcd->ops->device_quiesce == NULL) {
		/* Checks the device is quarantined result. */
		if (!device_is_quarantined(device))
			return 0;
		device_link(bus, device);

		/* Failed. */
		return EBUSY;
	}

	/* Checks the operation status. */
	error = bus->hcd->ops->device_quiesce(bus->hcd, device);
	if (error == 0 && drv_usb_device_hcd_urb_count(device) != 0)
		error = EBUSY;
	if (error == 0) {
		hal_atomic_store_release(&device->quarantined, 0U);

		/* Succeeded. */
		return 0;
	}

	/* Checks the device is quarantined result. */
	if (!device_is_quarantined(device)) {
		hal_printf("usb%u: device %u port %u teardown failed (%d); "
			   "device and DMA retained\n",
			   bus->number, device->address, device->port, error);
	}

	hal_atomic_store_release(&device->quarantined, 1U);
	device_link(bus, device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Asks whether a device has been put out of use. */
static int
device_is_quarantined(
	const struct drv_usb_device *device)
{
	int error;

	/* Computes the function result. */
	error = hal_atomic_load_acquire(&device->quarantined) != 0;

	/* Returns the computed result. */
	return error;
}

/* Links a device into the tree of its bus. */
static void
device_link(
	struct drv_usb_bus *bus,
	struct drv_usb_device *device)
{
	/* Handles the device linked condition. */
	if (device_linked(bus, device))
		return;
	device->next = bus->devices;
	bus->devices = device;
}

/* Asks whether a device is still in that tree. */
static int
device_linked(
	struct drv_usb_bus *bus,
	struct drv_usb_device *device)
{
	struct drv_usb_device *current;

	/* Process each linked entry. */
	for (current = bus->devices; current != NULL; current = current->next) {
		/* Handles the current condition. */
		if (current == device)
			return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Builds the device that stands for a controller's root hub. */
static struct drv_usb_device *
allocate_root_hub(
	struct drv_usb_bus *bus)
{
	struct drv_usb_device *device = hal_malloc(sizeof(*device));

	/* Handles the device availability. */
	if (device == NULL)
		return NULL;
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

	/* Returns the computed result. */
	return device;
}

/* Finds the bus a host controller was published as. */
static struct drv_usb_bus *
find_hcd_bus(
	struct drv_usb_hcd *hcd)
{
	struct drv_usb_bus *b;

	/* Process each linked entry. */
	for (b = usb_buses; b; b = b->next) {
		/* Handles the b condition. */
		if (b->hcd == hcd)
			return b;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Reads the status of one root port. */
static int
root_port_status(
	struct drv_usb_hcd *hcd,
	unsigned port,
	uint32_t *status)
{
	int function_result;
	struct drv_usb_control_request request = {0xa3U, 0, 0, (uint16_t)port,
						  4};
	size_t actual = 0;
	int error;

	error = hcd->ops->root_hub_control(hcd, &request, status,
					   sizeof(*status), &actual);

	/* Computes the function result. */
	function_result = error != 0		      ? error
			  : actual == sizeof(*status) ? 0
						      : EIO;

	/* Returns the computed result. */
	return function_result;
}

/* Acknowledges the changes a root port has reported. */
static int
root_port_acknowledge_changes(
	struct drv_usb_hcd *hcd,
	unsigned port,
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
	struct drv_usb_control_request request = {0x23U, 1, 0, (uint16_t)port,
						  0};
	size_t actual;
	unsigned index;
	int error;

	/* Process each remaining element. */
	for (index = 0;
	     index < sizeof(change_features) / sizeof(change_features[0]);
	     index++) {
		/* Checks the operation status. */
		if ((status & (1U << change_features[index])) == 0)
			continue;
		request.value = change_features[index];
		actual = 0;

		/* Checks the operation status. */
		error = hcd->ops->root_hub_control(hcd, &request, NULL, 0,
						   &actual);
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return 0;
}

/* Takes the next generation number the tree is stamped with. */
static uint64_t
usb_generation_next(
	uint64_t *generation)
{
	(*generation)++;

	/* Handles the generation condition. */
	if (*generation == 0)
		(*generation)++;

	/* Returns the computed result. */
	return *generation;
}

/* Finds the device that hangs from one port of a hub. */
static struct drv_usb_device *
find_port_device(
	struct drv_usb_bus *bus,
	unsigned port)
{
	struct drv_usb_device *d;

	/* Process each linked entry. */
	for (d = bus->devices; d; d = d->next) {
		/* Checks the current descriptor. */
		if (d->parent == bus->root_hub && d->port == port)
			return d;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Asks whether a device is already being disconnected. */
static int
device_is_disconnecting(
	const struct drv_usb_device *device)
{
	int error;

	/* Obtains the drv usb device is tearing down result. */
	error = drv_usb_device_is_tearing_down(device);

	/* Returns the computed result. */
	return error;
}

/* Gives a device and everything below it back. */
static int
destroy_device(
	struct drv_usb_bus *bus,
	struct drv_usb_device *device)
{
	int error;
	int detach_error, quiesce_error;

	/* Handles the device availability. */
	if (device == NULL || device == bus->root_hub)
		return EINVAL;
	device_begin_disconnect(device);
	detach_error = detach_interfaces(device);

	/*
	 * Admission is closed before detach.  Even a class-driver failure must
	 * reach the checked DMA barrier before the bus owner may release
	 * memory.
	 */

	/* Checks the operation status. */
	quiesce_error = device_quiesce(bus, device);
	if (quiesce_error == 0 && bus->hcd->ops->device_quiesce == NULL)
		quiesce_error = device_disable_active_endpoints(device);
	if (detach_error != 0 || quiesce_error != 0) {
		/* Checks the operation status. */
		if (detach_error != 0 && !device_is_quarantined(device)) {
			hal_printf("usb%u: device %u port %u driver detach "
				   "pending (%d); device retained\n",
				   bus->number, device->address, device->port,
				   detach_error);
		}

		hal_atomic_store_release(&device->quarantined, 1U);
		device_link(bus, device);

		/* Returns the computed result. */
		return detach_error != 0 ? detach_error : quiesce_error;
	}

	device->report_disconnect = 1U;

	/* Obtains the device release result. */
	error = device_release(bus, device);

	/* Returns the computed result. */
	return error;
}

/* Unconfigures every endpoint a device has active. */
static int
device_disable_active_endpoints(
	struct drv_usb_device *device)
{
	int error;
	struct drv_usb_configuration *configuration =
		device_active_configuration(device);

	/* Computes the function result. */
	error =
		configuration == NULL
			? 0
			: configuration_disable_endpoints(configuration);

	/* Returns the computed result. */
	return error;
}

/* Reports the configuration a device currently presents. */
static struct drv_usb_configuration *
device_active_configuration(
	const struct drv_usb_device *device)
{
	struct drv_usb_configuration *function_result;

	/* Obtains the atomic load n result. */
	function_result = __atomic_load_n(&device->active_configuration,
					  __ATOMIC_ACQUIRE);

	/* Returns the computed result. */
	return function_result;
}

/* Unconfigures every endpoint of one configuration. */
static int
configuration_disable_endpoints(
	struct drv_usb_configuration *configuration)
{
	int rollback;
	struct drv_usb_interface *interface;
	struct drv_usb_interface *disabled[DRV_USB_MAX_INTERFACES];
	unsigned disabled_count = 0;
	int error, rollback_error = 0;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Checks the operation status. */
		error = host_interface_disable(
			interface_active_alternate(interface));
		if (error != 0) {
			/* Process each remaining element. */
			while (disabled_count != 0) {
				interface = disabled[--disabled_count];

				/* Checks the operation status. */
				rollback = host_interface_enable(
					interface_active_alternate(interface));
				if (rollback_error == 0 && rollback != 0)
					rollback_error = rollback;
			}
			if (rollback_error != 0) {
				device_quarantine_selection(
					configuration->device,
					"configuration-disable",
					rollback_error);
			}

			/* Failed. */
			return error;
		}

		disabled[disabled_count++] = interface;
	}

	/* Succeeded. */
	return 0;
}

/* Unconfigures every endpoint of one alternate setting. */
static int
host_interface_disable(
	struct drv_usb_host_interface *alternate)
{
	int rollback;
	struct drv_usb_hcd *hcd = alternate->interface->device->bus->hcd;
	unsigned index, rollback_index;
	int error, rollback_error = 0;

	/* Handles the endpoint disable availability. */
	if (hcd->ops->endpoint_disable == NULL)
		return 0;
	/* Process each remaining element. */
	for (index = 0; index < alternate->endpoint_count; index++) {
		/* Checks the operation status. */
		error = hcd->ops->endpoint_disable(
			hcd, &alternate->endpoints[index]);
		if (error == 0)
			continue;

		/*
		 * SET_INTERFACE has not yet been issued.  Re-enable only the
		 * endpoints disabled by this invocation, in reverse order.
		 */
		for (rollback_index = index; rollback_index != 0;) {
			rollback_index--;

			/* Checks the operation status. */
			rollback = hcd->ops->endpoint_enable(
				hcd, &alternate->endpoints[rollback_index]);
			if (rollback_error == 0 && rollback != 0)
				rollback_error = rollback;
		}
		if (rollback_error != 0) {
			device_quarantine_selection(
				alternate->interface->device,
				"endpoint-disable", rollback_error);
		}

		/* Failed. */
		return error;
	}

	/* Succeeded. */
	return 0;
}

/* Puts a device out of use after a failed selection. */
static void
device_quarantine_selection(
	struct drv_usb_device *device,
	const char *stage,
	int error)
{
	/* Checks the device is quarantined result. */
	if (!device_is_quarantined(device)) {
		hal_printf("usb%u: device %u %s rollback failed (%d); "
			   "quarantined\n",
			   device->bus->number, device->address, stage, error);
	}

	hal_atomic_store_release(&device->quarantined, 1U);
	io_gate_close(&device->submit_gate);
}

/* Reports the alternate setting an interface presents. */
static struct drv_usb_host_interface *
interface_active_alternate(
	const struct drv_usb_interface *interface)
{
	struct drv_usb_host_interface *function_result;

	/* Obtains the atomic load n result. */
	function_result =
		__atomic_load_n(&interface->active_alternate, __ATOMIC_ACQUIRE);

	/* Returns the computed result. */
	return function_result;
}

/* Configures every endpoint of one alternate setting. */
static int
host_interface_enable(
	struct drv_usb_host_interface *alternate)
{
	int rollback;
	struct drv_usb_hcd *hcd = alternate->interface->device->bus->hcd;
	unsigned index, rollback_index;
	int error, rollback_error = 0;

	/* Handles the endpoint enable availability. */
	if (hcd->ops->endpoint_enable == NULL)
		return 0;
	/* Process each remaining element. */
	for (index = 0; index < alternate->endpoint_count; index++) {
		/* Checks the operation status. */
		error = hcd->ops->endpoint_enable(hcd,
						  &alternate->endpoints[index]);
		if (error != 0) {
			/*
			 * Only endpoints enabled by this invocation are
			 * eligible for compensation.  Reverse order preserves
			 * HCD dependencies.
			 */
			for (rollback_index = index; rollback_index != 0;) {
				rollback_index--;

				/* Checks the operation status. */
				rollback = hcd->ops->endpoint_disable(
					hcd,
					&alternate->endpoints[rollback_index]);
				if (rollback_error == 0 && rollback != 0)
					rollback_error = rollback;
			}
			if (rollback_error != 0) {
				device_quarantine_selection(
					alternate->interface->device,
					"endpoint-enable", rollback_error);
			}

			/* Failed. */
			return error;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Gives every resource a device held back. */
static int
device_release(
	struct drv_usb_bus *bus,
	struct drv_usb_device *device)
{
	struct drv_usb_urb *recovery_urb;
	unsigned state;

	device_begin_disconnect(device);

	/*
	 * The retained recovery URB owns one device lifecycle reference.
	 * Binding transactions are joined above, so no clear-halt operation can
	 * still use it.  Drop it before testing the final lifecycle-reference
	 * count.
	 */
	recovery_urb = device->recovery_urb;
	device->recovery_urb = NULL;

	/* Handles the recovery urb availability. */
	if (recovery_urb != NULL)
		drv_usb_urb_free(recovery_urb);
	state = hal_atomic_load_acquire(&device->lifecycle);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the state condition. */
		if ((state & USB_DEVICE_LIFECYCLE_URB_MASK) != 0) {
			/* Checks the device is quarantined result. */
			if (!device_is_quarantined(device)) {
				hal_printf(
					"usb%u: device %u port %u release "
					"waiting for %u URB reference(s); "
					"device retained\n",
					bus->number, device->address,
					device->port,
					state & USB_DEVICE_LIFECYCLE_URB_MASK);
			}

			hal_atomic_store_release(&device->quarantined, 1U);
			device_link(bus, device);

			/* Failed. */
			return EBUSY;
		}

		/* Handles the state condition. */
		if ((state & USB_DEVICE_LIFECYCLE_FINALIZING) != 0)
			return EALREADY;

		/* Checks the hal atomic compare exchange acq rel result. */
		if (hal_atomic_compare_exchange_acq_rel(
			    &device->lifecycle, &state,
			    state | USB_DEVICE_LIFECYCLE_FINALIZING))
			break;
	}

	device_finalize(bus, device);

	/* Succeeded. */
	return 0;
}

/* Retires a device once nothing refers to it any more. */
static void
device_finalize(
	struct drv_usb_bus *bus,
	struct drv_usb_device *device)
{
	struct drv_usb_device **link;
	unsigned address = device->address;
	unsigned port = device->port;
	unsigned report_disconnect = device->report_disconnect;

	/* Process each linked entry. */
	for (link = &bus->devices; *link != NULL; link = &(*link)->next) {
		/* Handles the link condition. */
		if (*link == device) {
			*link = device->next;
			break;
		}
	}

	/*
	 * A checked device/endpoint quiesce and successful driver detach have
	 * already stopped every path which can reach these descriptors.
	 */
	free_configurations(device);

	/* Handles the device disable availability. */
	if (bus->hcd->ops->device_disable != NULL)
		bus->hcd->ops->device_disable(bus->hcd, device);

	/* Handles the address condition. */
	if (address > 0 && address <= DRV_USB_MAX_ADDRESS)
		bus->address_used[address] = 0;

	/* Handles the quarantine buffer availability. */
	if (device->quarantine_buffer != NULL)
		hal_free(device->quarantine_buffer);
	hal_free(device);

	/* Handles the report disconnect condition. */
	if (report_disconnect) {
		hal_printf("usb%u: device %u port %u disconnected\n",
			   bus->number, address, port);
	}
}

/* Gives every configuration a device read back. */
static void
free_configurations(
	struct drv_usb_device *device)
{
	unsigned index;

	/* Handles the configurations availability. */
	if (device->configurations != NULL) {
		/* Process each remaining element. */
		for (index = 0; index < device->configuration_count; index++)
			free_configuration(&device->configurations[index]);
		hal_free(device->configurations);
	}

	device->configurations = NULL;
	device->configuration_count = 0;
	device_publish_configuration(device, NULL);
	device->interfaces = NULL;
}

/* Gives one configuration and its interfaces back. */
static void
free_configuration(
	struct drv_usb_configuration *configuration)
{
	struct drv_usb_host_interface *alternate, *next_alternate;
	struct drv_usb_interface *interface, *next_interface;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = next_interface) {
		next_interface = interface->next;
		/* Process each linked entry. */
		for (alternate = interface->alternates; alternate != NULL;
		     alternate = next_alternate) {
			next_alternate = alternate->next;

			/* Handles the endpoints availability. */
			if (alternate->endpoints != NULL)
				hal_free(alternate->endpoints);
			hal_free(alternate);
		}

		hal_free(interface);
	}

	/* Handles the iads availability. */
	if (configuration->iads != NULL)
		hal_free(configuration->iads);

	/* Handles the raw availability. */
	if (configuration->raw != NULL)
		hal_free(configuration->raw);
	memset(configuration, 0, sizeof(*configuration));
}

/* Publishes the configuration a device now presents. */
static void
device_publish_configuration(
	struct drv_usb_device *device,
	struct drv_usb_configuration *configuration)
{
	__atomic_store_n(&device->active_configuration, configuration,
			 __ATOMIC_RELEASE);
}

/* Resets a root port on a controller with no reset call. */
static int
legacy_root_port_reset(
	struct drv_usb_hcd *hcd,
	unsigned port)
{
	struct drv_usb_control_request request = {0x23U, 3, 4, (uint16_t)port,
						  0};
	size_t actual = 0;
	int error;

	/* Checks the operation status. */
	error = hcd->ops->root_hub_control(hcd, &request, NULL, 0, &actual);
	if (error != 0)
		return error;
	usb_delay_ticks(5U);
	request.request = 1;

	/* Checks the operation status. */
	error = hcd->ops->root_hub_control(hcd, &request, NULL, 0, &actual);
	if (error != 0)
		return error;
	request.request = 3;
	request.value = 1;

	/* Checks the operation status. */
	error = hcd->ops->root_hub_control(hcd, &request, NULL, 0, &actual);
	if (error != 0)
		return error;
	usb_delay_ticks(USB_RESET_RECOVERY_TICKS);

	/* Succeeded. */
	return 0;
}

/* Reports how many ticks a delay in milliseconds takes. */
static void
usb_delay_ticks(
	uint64_t count)
{
	uint64_t deadline = sched_ticks() + count;

	/* Continue while the operation condition remains true. */
	while (sched_ticks() < deadline)
		hal_compiler_barrier();
}

/* Enumerates whatever has just been attached to one port. */
static int
enumerate_port(
	struct drv_usb_bus *bus,
	unsigned port,
	uint32_t status)
{
	struct drv_usb_driver *matched_driver;
	struct drv_usb_device *device;
	struct drv_usb_configuration *preferred;
	struct drv_usb_interface *interface;
	uint8_t first[8];
	size_t actual = 0;
	unsigned configuration_index, other, packet;
	int address = 0, cleanup_error, error = 0, preferred_score;

	/* Handles the device availability. */
	device = hal_malloc(sizeof(*device));
	if (device == NULL)
		return ENOMEM;
	memset(device, 0, sizeof(*device));
	device->bus = bus;
	device->parent = bus->root_hub;
	device->port = port;
	device->generation = usb_generation_next(&usb_device_generation);

	/* Handles the bus condition. */
	if (bus->ports[port].connection_generation == 0)
		usb_generation_next(&bus->ports[port].connection_generation);
	device->port_generation = bus->ports[port].connection_generation;
	device->speed = (status & 0x800U)   ? DRV_USB_SPEED_SUPER
			: (status & 0x400U) ? DRV_USB_SPEED_HIGH
			: (status & 0x200U) ? DRV_USB_SPEED_LOW
					    : DRV_USB_SPEED_FULL;
	device->state = DRV_USB_STATE_DEFAULT;
	device->endpoint0.type = DRV_USB_TRANSFER_CONTROL;
	device->endpoint0.descriptor.maximum_packet_size = 8U;

	/* Handles the device enable availability. */
	if (bus->hcd->ops->device_enable != NULL) {
		/* Checks the operation status. */
		error = bus->hcd->ops->device_enable(bus->hcd, device);
		if (error != 0)
			goto fail;
	}

	/* Checks the operation status. */
	error = drv_usb_control(device,
				DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
					DRV_USB_RECIP_DEVICE,
				USB_REQ_GET_DESCRIPTOR,
				(uint16_t)(DRV_USB_DESCRIPTOR_DEVICE << 8), 0,
				first, sizeof(first), USB_CONTROL_TIMEOUT_MS,
				&actual);
	if (error != 0 || actual != sizeof(first)) {
		/* Checks the operation status. */
		if (error == 0)
			error = EIO;
		goto fail;
	}

	/* Checks the ep0 packet size result. */
	if (first[0] != sizeof(struct drv_usb_device_descriptor) ||
	    first[1] != DRV_USB_DESCRIPTOR_DEVICE ||
	    !ep0_packet_size(device->speed, first[7], &packet)) {
		error = EIO;
		goto fail;
	}

	device->descriptor.length = first[0];
	device->descriptor.descriptor_type = first[1];
	device->descriptor.endpoint0_max_packet_size = first[7];
	device->endpoint0.descriptor.maximum_packet_size = (uint16_t)packet;

	/* Handles the address condition. */
	address = allocate_address(bus);
	if (address < 0) {
		address = 0;
		error = ENOSPC;
		goto fail;
	}

	/*
	 * Reserve the address in the device before the HCD transition so failed
	 * checked teardown retains, and successful teardown releases, one
	 * owner.
	 */
	device->address = (unsigned)address;

	/* Handles the device set address availability. */
	if (bus->hcd->ops->device_set_address != NULL) {
		error = bus->hcd->ops->device_set_address(bus->hcd, device,
							  (unsigned)address);
	} else {
		/*
		 * A wire SET_ADDRESS request is addressed to endpoint zero at
		 * the default address.  Keep the allocated value in the device
		 * for failure cleanup, but expose address zero while the
		 * synchronous request is built and completed.
		 * Controller-command HCDs use the callback above.
		 */
		device->address = 0;
		error = drv_usb_control(
			device,
			DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
				DRV_USB_RECIP_DEVICE,
			USB_REQ_SET_ADDRESS, (uint16_t)address, 0, NULL, 0,
			USB_CONTROL_TIMEOUT_MS, &actual);
		device->address = (unsigned)address;
	}
	if (error != 0)
		goto fail;
	device->state = DRV_USB_STATE_ADDRESS;
	usb_delay_ticks(USB_ADDRESS_RECOVERY_TICKS);

	/* Checks the operation status. */
	error = drv_usb_control(device,
				DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
					DRV_USB_RECIP_DEVICE,
				USB_REQ_GET_DESCRIPTOR,
				(uint16_t)(DRV_USB_DESCRIPTOR_DEVICE << 8), 0,
				&device->descriptor, sizeof(device->descriptor),
				USB_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0 || actual != sizeof(device->descriptor)) {
		/* Checks the operation status. */
		if (error == 0)
			error = EIO;
		goto fail;
	}

	/* Handles the device condition. */
	if (device->descriptor.configuration_count == 0 ||
	    device->descriptor.configuration_count >
		    DRV_USB_MAX_CONFIGURATIONS) {
		error = EINVAL;
		goto fail;
	}

	device->configuration_count = device->descriptor.configuration_count;
	device->configurations = hal_malloc(device->configuration_count *
					    sizeof(*device->configurations));

	/* Handles the configurations availability. */
	if (device->configurations == NULL) {
		error = ENOMEM;
		goto fail;
	}

	memset(device->configurations, 0,
	       device->configuration_count * sizeof(*device->configurations));
	/* Process each remaining element. */
	for (configuration_index = 0;
	     configuration_index < device->configuration_count;
	     configuration_index++) {
		/* Checks the operation status. */
		error = enumerate_configuration(
			device, configuration_index,
			&device->configurations[configuration_index]);
		if (error != 0)
			goto fail;
		/* Process each remaining element. */
		for (other = 0; other < configuration_index; other++) {
			/* Handles the device condition. */
			if (device->configurations[other]
				    .descriptor.configuration_value ==
			    device->configurations[configuration_index]
				    .descriptor.configuration_value) {
				error = EINVAL;
				goto fail;
			}
		}
	}

	/*
	 * Endpoint recovery must remain usable while reclaim itself is waiting
	 * on USB-backed storage.  Reserve its endpoint-zero URB before any
	 * class driver can enter an error path.
	 */
	device->recovery_urb = drv_usb_urb_alloc(device, NULL, 0);

	/* Handles the recovery urb availability. */
	if (device->recovery_urb == NULL) {
		error = ENOMEM;
		goto fail;
	}

	preferred = device_preferred_configuration(device, &preferred_score);

	/* Checks the operation status. */
	error = drv_usb_device_set_configuration(
		device, preferred->descriptor.configuration_value);
	if (error != 0)
		goto fail;
	device_link(bus, device);
	hal_printf("usb%u: device %u port %u %04x:%04x class %02x "
		   "configuration=%u configured%s\n",
		   bus->number, device->address, port,
		   device->descriptor.vendor, device->descriptor.product,
		   device->descriptor.device_class,
		   preferred->descriptor.configuration_value,
		   preferred_score == 0
			   ? " (no supported configuration; first selected)"
			   : "");
	/* Process each linked entry. */
	for (interface = device->interfaces; interface != NULL;
	     interface = interface->next) {
		matched_driver = NULL;

		error = interface_probe_internal(interface, &matched_driver);
		interface_report_probe(interface, error, matched_driver);
	}

	/* Succeeded. */
	return 0;

fail:

	/* Checks the operation status. */
	if (error == 0)
		error = EIO;
	device_begin_disconnect(device);

	/* Checks the operation status. */
	cleanup_error = device_quiesce(bus, device);
	if (cleanup_error != 0) {
		return error;
	}

	device_release(bus, device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reads the packet size a device's control endpoint takes. */
static int
ep0_packet_size(
	enum drv_usb_speed speed,
	uint8_t encoded,
	unsigned *packet)
{
	unsigned decoded = 0;

	/* Dispatch the selected operation case. */
	switch (speed) {
	case DRV_USB_SPEED_LOW:
		decoded = encoded == 8U ? 8U : 0;
		break;
	case DRV_USB_SPEED_FULL:
		/* Handles the encoded condition. */
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

	/* Handles the packet availability. */
	if (packet != NULL)
		*packet = decoded;
	/* Returns the computed result. */
	return decoded != 0;
}

/* Takes the next free bus address for a new device. */
static int
allocate_address(
	struct drv_usb_bus *bus)
{
	unsigned n;

	/* Process each element required by the operation. */
	for (n = 1; n <= DRV_USB_MAX_ADDRESS; n++) {
		/* Handles the bus condition. */
		if (!bus->address_used[n]) {
			bus->address_used[n] = 1;

			/* Returns the computed result. */
			return (int)n;
		}
	}

	/* Reports operation failure. */
	return -1;
}

/* Reads and parses one configuration of a device. */
static int
enumerate_configuration(
	struct drv_usb_device *device,
	unsigned index,
	struct drv_usb_configuration *configuration)
{
	int function_result;
	struct drv_usb_configuration_descriptor descriptor;
	uint8_t *raw;
	size_t actual = 0;
	int error;

	/* Reads the header of the configuration descriptor. */

	/* Checks the operation status. */
	error = drv_usb_control(
		device,
		DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_DEVICE,
		USB_REQ_GET_DESCRIPTOR,
		(uint16_t)((DRV_USB_DESCRIPTOR_CONFIGURATION << 8) | index), 0,
		&descriptor, sizeof(descriptor), USB_CONTROL_TIMEOUT_MS,
		&actual);
	if (error != 0)
		return error;

	/* Handles the actual condition. */
	if (actual != sizeof(descriptor) ||
	    descriptor.length < sizeof(descriptor) ||
	    descriptor.descriptor_type != DRV_USB_DESCRIPTOR_CONFIGURATION ||
	    descriptor.total_length < descriptor.length) {
		/* Failed. */
		return EIO;
	}

	/* Handles the raw availability. */
	raw = hal_malloc(descriptor.total_length);
	if (raw == NULL)
		return ENOMEM;

	/* Checks the operation status. */
	error = drv_usb_control(
		device,
		DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_DEVICE,
		USB_REQ_GET_DESCRIPTOR,
		(uint16_t)((DRV_USB_DESCRIPTOR_CONFIGURATION << 8) | index), 0,
		raw, descriptor.total_length, USB_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0 || actual != descriptor.total_length) {
		hal_free(raw);

		/* Returns the computed result. */
		return error != 0 ? error : EIO;
	}

	/* Obtains the parse configuration result. */
	function_result =
		parse_configuration(configuration, device, raw, actual);

	/* Returns the computed result. */
	return function_result;
}

/* Splits a configuration's descriptor bytes into its parts. */
static int
parse_configuration(
	struct drv_usb_configuration *configuration,
	struct drv_usb_device *device,
	uint8_t *raw,
	size_t length)
{
	struct drv_usb_interface_descriptor interface_descriptor;
	struct drv_usb_host_interface **alternate_tail;
	struct drv_usb_endpoint_descriptor endpoint_descriptor;
	unsigned endpoint_number, endpoint_index;
	uint8_t descriptor_length;
	uint8_t descriptor_type;
	struct drv_usb_configuration_descriptor descriptor;
	struct drv_usb_interface **interface_tail;
	struct drv_usb_interface *current_interface = NULL;
	struct drv_usb_host_interface *current_alternate = NULL;
	struct drv_usb_endpoint *current_endpoint = NULL;
	size_t offset;
	unsigned iad_index = 0;
	int error;

	memset(configuration, 0, sizeof(*configuration));
	configuration->raw = raw;
	configuration->raw_length = length;
	configuration->device = device;

	/* Checks the current data length. */
	if (length < sizeof(descriptor)) {
		error = EINVAL;
		goto fail;
	}

	memcpy(&descriptor, raw, sizeof(descriptor));

	/* Checks the file descriptor. */
	if (descriptor.length < sizeof(descriptor) ||
	    descriptor.length > length ||
	    descriptor.descriptor_type != DRV_USB_DESCRIPTOR_CONFIGURATION ||
	    descriptor.total_length != length ||
	    descriptor.configuration_value == 0 ||
	    descriptor.interface_count > DRV_USB_MAX_INTERFACES) {
		error = EINVAL;
		goto fail;
	}

	configuration->descriptor = descriptor;

	/* Checks the operation status. */
	error = configuration_iads_prepare(configuration, raw, length);
	if (error != 0)
		goto fail;
	interface_tail = &configuration->interfaces;
	offset = descriptor.length;
	/* Process each remaining element. */
	while (offset < length) {
		descriptor_length = raw[offset];

		/* Handles the descriptor type condition. */
		descriptor_type = raw[offset + 1U];
		if (descriptor_type == DRV_USB_DESCRIPTOR_INTERFACE) {
			/* Handles the descriptor length condition. */
			if (descriptor_length < sizeof(interface_descriptor)) {
				error = EINVAL;
				goto fail;
			}

			/* Handles the current alternate availability. */
			if (current_alternate != NULL) {
				/* Handles the current alternate condition. */
				if (current_alternate->endpoint_count !=
				    current_alternate->descriptor
					    .endpoint_count) {
					error = EINVAL;
					goto fail;
				}

				current_alternate->raw_length =
					offset - current_alternate->raw_offset;
			}

			memcpy(&interface_descriptor, raw + offset,
			       sizeof(interface_descriptor));

			/* Handles the interface descriptor condition. */
			if (interface_descriptor.endpoint_count >
			    DRV_USB_MAX_ENDPOINTS) {
				error = EINVAL;
				goto fail;
			}

			/* Handles the current interface availability. */
			current_interface = configuration_find_interface(
				configuration,
				interface_descriptor.interface_number);
			if (current_interface == NULL) {
				/* Handles the configuration condition. */
				if (configuration->interface_count ==
				    DRV_USB_MAX_INTERFACES) {
					error = EINVAL;
					goto fail;
				}

				current_interface =
					hal_malloc(sizeof(*current_interface));

				/* Handles the current interface availability. */
				if (current_interface == NULL) {
					error = ENOMEM;
					goto fail;
				}

				memset(current_interface, 0,
				       sizeof(*current_interface));
				current_interface->device = device;
				current_interface->configuration =
					configuration;
				current_interface->descriptor =
					interface_descriptor;
				atomic_store_release(
					&current_interface->binding_gate,
					USB_IO_GATE_CLOSED);
				atomic_store_release(
					&current_interface->binding_submitters,
					USB_IO_GATE_CLOSED);
				*interface_tail = current_interface;
				interface_tail = &current_interface->next;
				configuration->interface_count++;
			}

			/* Checks the interface find alternate result. */
			if (current_interface->alternate_count ==
				    DRV_USB_MAX_ALTERNATES ||
			    interface_find_alternate(
				    current_interface,
				    interface_descriptor.alternate_setting) !=
				    NULL) {
				error = EINVAL;
				goto fail;
			}

			current_alternate =
				hal_malloc(sizeof(*current_alternate));

			/* Handles the current alternate availability. */
			if (current_alternate == NULL) {
				error = ENOMEM;
				goto fail;
			}

			memset(current_alternate, 0,
			       sizeof(*current_alternate));
			current_alternate->interface = current_interface;
			current_alternate->descriptor = interface_descriptor;
			current_alternate->raw_offset = offset;

			/* Handles the interface descriptor condition. */
			if (interface_descriptor.endpoint_count != 0) {
				current_alternate->endpoints = hal_malloc(
					interface_descriptor.endpoint_count *
					sizeof(*current_alternate->endpoints));

				/* Handles the endpoints availability. */
				if (current_alternate->endpoints == NULL) {
					hal_free(current_alternate);
					current_alternate = NULL;
					error = ENOMEM;
					goto fail;
				}

				memset(current_alternate->endpoints, 0,
				       interface_descriptor.endpoint_count *
					       sizeof(*current_alternate
							       ->endpoints));
			}

			alternate_tail = &current_interface->alternates;
			/* Continue while the operation condition remains true. */
			while (*alternate_tail != NULL)
				alternate_tail = &(*alternate_tail)->next;
			*alternate_tail = current_alternate;
			current_interface->alternate_count++;
			current_endpoint = NULL;
		} else if (descriptor_type == DRV_USB_DESCRIPTOR_ENDPOINT) {
			/* Handles the current alternate availability. */
			if (current_alternate == NULL ||
			    descriptor_length < sizeof(endpoint_descriptor) ||
			    current_alternate->endpoint_count >=
				    current_alternate->descriptor
					    .endpoint_count) {
				error = EINVAL;
				goto fail;
			}

			memcpy(&endpoint_descriptor, raw + offset,
			       sizeof(endpoint_descriptor));

			/* Handles the endpoint number condition. */
			endpoint_number = endpoint_descriptor.address & 0x0fU;
			if (endpoint_number == 0 ||
			    (endpoint_descriptor.address & 0x70U) != 0 ||
			    (endpoint_descriptor.attributes & 3U) ==
				    DRV_USB_TRANSFER_CONTROL ||
			    endpoint_descriptor.maximum_packet_size == 0) {
				error = EINVAL;
				goto fail;
			}

			/* Process each remaining element. */
			for (endpoint_index = 0;
			     endpoint_index < current_alternate->endpoint_count;
			     endpoint_index++) {
				/* Handles the current alternate condition. */
				if (current_alternate->endpoints[endpoint_index]
					    .descriptor.address ==
				    endpoint_descriptor.address) {
					error = EINVAL;
					goto fail;
				}
			}

			current_endpoint =
				&current_alternate->endpoints
					 [current_alternate->endpoint_count++];
			current_endpoint->interface = current_interface;
			current_endpoint->alternate = current_alternate;
			current_endpoint->descriptor = endpoint_descriptor;
			current_endpoint->type = (enum drv_usb_transfer_type)(
				endpoint_descriptor.attributes & 3U);
		} else if (descriptor_type ==
			   DRV_USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION) {
			/* Checks the drv usb decode superspeed endpoint companion result. */
			if (current_endpoint == NULL ||
			    current_endpoint->companion_valid ||
			    drv_usb_decode_superspeed_endpoint_companion(
				    raw + offset, descriptor_length,
				    &current_endpoint->companion) != 0) {
				error = EINVAL;
				goto fail;
			}

			current_endpoint->companion_valid = 1U;
			current_endpoint = NULL;
		} else if (descriptor_type ==
			   DRV_USB_DESCRIPTOR_INTERFACE_ASSOCIATION) {
			/* Handles the iad index condition. */
			if (iad_index >= configuration->iad_count) {
				error = EINVAL;
				goto fail;
			}

			memcpy(&configuration->iads[iad_index++], raw + offset,
			       sizeof(configuration->iads[0]));
			current_endpoint = NULL;
		} else {
			/* Handles the descriptor type condition. */
			if (descriptor_type ==
			    DRV_USB_DESCRIPTOR_CONFIGURATION) {
				error = EINVAL;
				goto fail;
			}

			/* Handles the current alternate availability. */
			if (current_alternate != NULL)
				current_alternate->extra_count++;
			current_endpoint = NULL;
		}

		offset += descriptor_length;
	}

	/* Handles the current alternate availability. */
	if (current_alternate != NULL) {
		/* Handles the current alternate condition. */
		if (current_alternate->endpoint_count !=
		    current_alternate->descriptor.endpoint_count) {
			error = EINVAL;
			goto fail;
		}

		current_alternate->raw_length =
			offset - current_alternate->raw_offset;
	}

	/* Handles the configuration condition. */
	if (configuration->interface_count != descriptor.interface_count ||
	    iad_index != configuration->iad_count) {
		error = EINVAL;
		goto fail;
	}

	/* Process each linked entry. */
	for (current_interface = configuration->interfaces;
	     current_interface != NULL;
	     current_interface = current_interface->next) {
		current_alternate =
			interface_find_alternate(current_interface, 0);

		/* Handles the current alternate availability. */
		if (current_alternate == NULL) {
			error = EINVAL;
			goto fail;
		}

		interface_publish_alternate(current_interface,
					    current_alternate);
	}

	/* Checks the operation status. */
	error = configuration_endpoint_addresses_validate(configuration);
	if (error != 0)
		goto fail;

	/* Checks the operation status. */
	error = configuration_iads_validate(configuration);
	if (error != 0)
		goto fail;

	/* Succeeded. */
	return 0;

fail:
	free_configuration(configuration);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Builds the interface associations a configuration declares. */
static int
configuration_iads_prepare(
	struct drv_usb_configuration *configuration,
	const uint8_t *raw,
	size_t length)
{
	uint8_t descriptor_length, descriptor_type;
	size_t offset = 0;
	unsigned count = 0;

	/* Process each remaining element. */
	while (offset < length) {
		/* Checks the current data length. */
		if (length - offset < 2U)
			return EINVAL;
		descriptor_length = raw[offset];
		descriptor_type = raw[offset + 1U];

		/* Handles the descriptor length condition. */
		if (descriptor_length < 2U ||
		    descriptor_length > length - offset) {
			/* Failed. */
			return EINVAL;
		}

		/* Handles the descriptor type condition. */
		if (descriptor_type ==
		    DRV_USB_DESCRIPTOR_INTERFACE_ASSOCIATION) {
			/* Handles the descriptor length condition. */
			if (descriptor_length !=
				    sizeof(struct
					   drv_usb_interface_association_descriptor) ||
			    count == DRV_USB_MAX_IADS) {
				/* Failed. */
				return EINVAL;
			}
			count++;
		}

		offset += descriptor_length;
	}

	/* Checks the remaining item count. */
	if (count != 0) {
		configuration->iads =
			hal_malloc(count * sizeof(*configuration->iads));

		/* Handles the iads availability. */
		if (configuration->iads == NULL)
			return ENOMEM;
		memset(configuration->iads, 0,
		       count * sizeof(*configuration->iads));
	}

	configuration->iad_count = count;

	/* Succeeded. */
	return 0;
}

/* Finds an interface of a configuration by its number. */
static struct drv_usb_interface *
configuration_find_interface(
	struct drv_usb_configuration *configuration,
	unsigned number)
{
	struct drv_usb_interface *interface;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Handles the interface condition. */
		if (interface->alternates->descriptor.interface_number ==
		    number) {
			/* Returns the computed result. */
			return interface;
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Finds an alternate setting of an interface by its number. */
static struct drv_usb_host_interface *
interface_find_alternate(
	struct drv_usb_interface *interface,
	unsigned setting)
{
	struct drv_usb_host_interface *alternate;

	/* Process each linked entry. */
	for (alternate = interface->alternates; alternate != NULL;
	     alternate = alternate->next) {
		/* Handles the alternate condition. */
		if (alternate->descriptor.alternate_setting == setting)
			return alternate;
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Publishes the alternate setting an interface now presents. */
static void
interface_publish_alternate(
	struct drv_usb_interface *interface,
	struct drv_usb_host_interface *alternate)
{
	interface->descriptor = alternate->descriptor;
	interface->endpoints = alternate->endpoints;
	interface->endpoint_count = alternate->endpoint_count;
	__atomic_store_n(&interface->active_alternate, alternate,
			 __ATOMIC_RELEASE);
}

/* Refuses a configuration that gives one address twice. */
static int
configuration_endpoint_addresses_validate(
	const struct drv_usb_configuration *configuration)
{
	uint8_t address;
	unsigned bit;
	unsigned endpoint_index;
	const struct drv_usb_host_interface *alternate;
	uint32_t interface_addresses;
	const struct drv_usb_interface *interface;
	uint32_t configuration_addresses = 0;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		interface_addresses = 0;

		/* Process each linked entry. */
		for (alternate = interface->alternates; alternate != NULL;
		     alternate = alternate->next) {
			/* Process each remaining element. */
			for (endpoint_index = 0;
			     endpoint_index < alternate->endpoint_count;
			     endpoint_index++) {
				address = alternate->endpoints[endpoint_index]
						  .descriptor.address;
				bit = (address & 0x0fU) +
				      ((address & DRV_USB_DIR_IN) != 0 ? 16U
								       : 0U);

				interface_addresses |= (uint32_t)1U << bit;
			}
		}

		/*
		 * Alternate settings of one logical interface may reuse an
		 * endpoint address, but independently selectable interfaces may
		 * not alias an HCD endpoint context.
		 */
		if ((configuration_addresses & interface_addresses) != 0)
			return EINVAL;
		configuration_addresses |= interface_addresses;
	}

	/* Succeeded. */
	return 0;
}

/* Refuses an association that names interfaces it cannot have. */
static int
configuration_iads_validate(
	struct drv_usb_configuration *configuration)
{
	const struct drv_usb_interface_association_descriptor *other;
	unsigned other_end;
	const struct drv_usb_interface_association_descriptor *iad;
	unsigned first;
	unsigned end;
	unsigned left, right;

	/* Process each remaining element. */
	for (left = 0; left < configuration->iad_count; left++) {
		iad = &configuration->iads[left];
		first = iad->first_interface;

		/* Handles the iad condition. */
		end = first + iad->interface_count;
		if (iad->interface_count == 0 || end > (unsigned)UINT8_MAX + 1U)
			return EINVAL;
		/* Process each element required by the operation. */
		for (; first < end; first++) {
			/* Checks the configuration find interface result. */
			if (configuration_find_interface(configuration,
							 first) == NULL) {
				/* Failed. */
				return EINVAL;
			}
		}

		/* Process each element required by the operation. */
		for (right = 0; right < left; right++) {
			other = &configuration->iads[right];
			other_end =
				other->first_interface + other->interface_count;

			/* Handles the iad condition. */
			if (iad->first_interface < other_end &&
			    other->first_interface < end) {
				/* Failed. */
				return EINVAL;
			}
		}
	}

	/* Succeeded. */
	return 0;
}

/* Picks the configuration a device should be set to. */
static struct drv_usb_configuration *
device_preferred_configuration(
	struct drv_usb_device *device,
	int *selected_score)
{
	int interface_score;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *interface;
	int score;
	struct drv_usb_configuration *best = &device->configurations[0];
	int best_score = 0;
	unsigned configuration_index;

	/* Process each remaining element. */
	for (configuration_index = 0;
	     configuration_index < device->configuration_count;
	     configuration_index++) {
		configuration = &device->configurations[configuration_index];

		score = 0;

		/* Process each linked entry. */
		for (interface = configuration->interfaces; interface != NULL;
		     interface = interface->next) {
			interface_score =
				interface_registered_driver_score(interface);

			/* Handles the interface score condition. */
			if (interface_score > score)
				score = interface_score;
		}

		/*
		 * Strictly greater preserves descriptor order for ties and for
		 * the all-unsupported case.  Matching is observational:
		 * inactive configurations are never attached or published.
		 */
		if (configuration_index == 0 || score > best_score) {
			best = configuration;
			best_score = score;
		}
	}

	/* Handles the selected score availability. */
	if (selected_score != NULL)
		*selected_score = best_score;
	/* Returns the computed result. */
	return best;
}

/* Reports how well a registered driver matches an interface. */
static int
interface_registered_driver_score(
	struct drv_usb_interface *interface)
{
	const struct drv_usb_id *id;
	int score;
	struct usb_driver_entry *entry;
	int best = 0;

	/* Process each linked entry. */
	for (entry = usb_drivers; entry != NULL; entry = entry->next) {
		/* Handles the id availability. */
		id = drv_usb_driver_find_id(entry->driver, interface);
		if (id == NULL)
			continue;

		/* Handles the score condition. */
		score = entry->driver->match != NULL
				? entry->driver->match(interface, id)
				: 1;
		if (score > best)
			best = score;
	}

	/* Returns the computed result. */
	return best;
}

/* Offers one interface to the drivers that could take it. */
static int
interface_probe_internal(
	struct drv_usb_interface *interface,
	struct drv_usb_driver **matched_driver)
{
	struct drv_usb_device *device;
	struct usb_driver_entry *entry;
	const struct drv_usb_id *id;
	unsigned expected_state;
	int score, best = 0, error, cleanup_error;
	struct drv_usb_driver *driver = NULL;

	/* Handles the matched driver availability. */
	if (matched_driver != NULL)
		*matched_driver = NULL;
	/* Handles the interface availability. */
	if (interface == NULL)
		return EINVAL;
	device = interface->device;

	/* Checks the operation status. */
	error = device_binding_enter(device);
	if (error != 0)
		return error;

	/* Checks the device active configuration result. */
	if (device->state != DRV_USB_STATE_CONFIGURED ||
	    device_active_configuration(device) != interface->configuration ||
	    device_is_quarantined(device)) {
		error = ENODEV;
		goto out;
	}

	/* Checks the interface claim owner result. */
	if (interface->driver != NULL ||
	    interface_claim_owner(interface) != NULL ||
	    atomic_load_acquire(&interface->binding_state) !=
		    USB_BINDING_DEAD) {
		error = EBUSY;
		goto out;
	}

	/* Checks the atomic compare exchange result. */
	expected_state = USB_BINDING_DEAD;
	if (!atomic_compare_exchange(&interface->binding_state, &expected_state,
				     USB_BINDING_PROBING)) {
		error = EBUSY;
		goto out;
	}

	/* Checks the io gate close empty result. */
	if (io_gate_close_empty(&interface->io_gate) != 0) {
		atomic_store_release(&interface->binding_state,
				     USB_BINDING_DEAD);
		error = EBUSY;
		goto out;
	}

	/* Checks the device active configuration result. */
	if (device->state != DRV_USB_STATE_CONFIGURED ||
	    device_active_configuration(device) != interface->configuration ||
	    device_is_disconnecting(device) || device_is_quarantined(device)) {
		io_gate_open(&interface->io_gate);
		atomic_store_release(&interface->binding_state,
				     USB_BINDING_DEAD);
		error = ENODEV;
		goto out;
	}

	/* Process each linked entry. */
	for (entry = usb_drivers; entry != NULL; entry = entry->next) {
		/* Handles the id availability. */
		id = drv_usb_driver_find_id(entry->driver, interface);
		if (id == NULL)
			continue;

		/* Handles the score condition. */
		score = entry->driver->match != NULL
				? entry->driver->match(interface, id)
				: 1;
		if (score > best) {
			best = score;
			driver = entry->driver;
		}
	}

	/* Handles the driver availability. */
	if (driver == NULL) {
		io_gate_open(&interface->io_gate);
		atomic_store_release(&interface->binding_state,
				     USB_BINDING_DEAD);
		error = ENODEV;
		goto out;
	}

	/* Handles the matched driver availability. */
	if (matched_driver != NULL)
		*matched_driver = driver;
	id = drv_usb_driver_find_id(driver, interface);
	interface->driver = driver;
	io_gate_open(&interface->binding_gate);
	io_gate_open(&interface->binding_submitters);
	io_gate_open(&interface->io_gate);

	/* Checks the operation status. */
	error = driver->attach != NULL ? driver->attach(interface, id) : 0;
	if (error == 0) {
		atomic_store_release(&interface->binding_state,
				     USB_BINDING_BOUND);
		goto out;
	}

	cleanup_error = interface_binding_detach(
		interface, DRV_USB_DETACH_ATTACH_FAILED, USB_BINDING_PROBING);
	(void)cleanup_error;

out:
	device_binding_exit(device);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Joins the gate that keeps binding out of a disconnect. */
static int
device_binding_enter(
	struct drv_usb_device *device)
{
	int function_result;
	int error;

	/* Checks the operation status. */
	error = io_gate_enter(&device->binding_transactions);
	if (error != 0) {
		/* Computes the function result. */
		function_result =
			device_is_disconnecting(device) ? ENODEV : error;

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the device is disconnecting condition. */
	if (device_is_disconnecting(device)) {
		io_gate_exit(&device->binding_transactions);

		/* Failed. */
		return ENODEV;
	}

	/* Succeeded. */
	return 0;
}

/* Joins the gate that lets a transfer into a device. */
static int
io_gate_enter(
	atomic_uint_t *gate)
{
	unsigned state;

	state = atomic_load_acquire(gate);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the state condition. */
		if ((state & USB_IO_GATE_CLOSED) != 0 ||
		    (state & USB_IO_GATE_COUNT_MASK) == USB_IO_GATE_COUNT_MASK) {
			/* Failed. */
			return EBUSY;
		}

		/* Handles the atomic compare exchange condition. */
		if (atomic_compare_exchange(gate, &state, state + 1U))
			return 0;
	}
}

/* Leaves that gate. */
static void
io_gate_exit(
	atomic_uint_t *gate)
{
	unsigned previous;

	/* Handles the previous condition. */
	previous = atomic_raw_fetch_add_release(&gate->value, (unsigned)-1);
	if ((previous & USB_IO_GATE_COUNT_MASK) == 0)
		__builtin_trap();

	/* Handles the previous condition. */
	if ((previous & USB_IO_GATE_COUNT_MASK) == 1U)
		hal_atomic_fence_acquire();
}

/* Leaves the binding gate. */
static void
device_binding_exit(
	struct drv_usb_device *device)
{
	io_gate_exit(&device->binding_transactions);
}

/* Records which driver took an interface, and how. */
static void
interface_report_probe(
	struct drv_usb_interface *interface,
	int error,
	struct drv_usb_driver *matched_driver)
{
	const struct drv_usb_interface_descriptor *owner_descriptor;
	const struct drv_usb_interface_descriptor *descriptor;
	struct drv_usb_interface *owner;
	struct drv_usb_driver *driver;
	unsigned bus, address;

	/* Handles the descriptor availability. */
	descriptor = drv_usb_interface_descriptor(interface);
	if (descriptor == NULL)
		return;
	bus = interface->device->bus->number;
	address = interface->device->address;
	owner = interface_claim_owner(interface);

	/* Checks the operation status. */
	if (error == 0) {
		driver = interface->driver;
		hal_printf("usb%u: device %u interface %u class %02x/%02x/%02x "
			   "driver=%s\n",
			   bus, address, descriptor->interface_number,
			   descriptor->interface_class,
			   descriptor->interface_subclass,
			   descriptor->interface_protocol,
			   driver != NULL ? driver->name : "unknown");
	} else if (error == EBUSY && owner != NULL) {
		owner_descriptor = drv_usb_interface_descriptor(owner);

		/* Reports the outcome in the form the failure calls for. */
		driver = owner->driver;
		hal_printf("usb%u: device %u interface %u class %02x/%02x/%02x "
			   "claimed-by=%u driver=%s\n",
			   bus, address, descriptor->interface_number,
			   descriptor->interface_class,
			   descriptor->interface_subclass,
			   descriptor->interface_protocol,
			   owner_descriptor != NULL
				   ? owner_descriptor->interface_number
				   : 0,
			   driver != NULL ? driver->name : "unknown");
	} else if (error == ENODEV && matched_driver == NULL) {
		hal_printf("usb%u: device %u interface %u class %02x/%02x/%02x "
			   "no-driver\n",
			   bus, address, descriptor->interface_number,
			   descriptor->interface_class,
			   descriptor->interface_subclass,
			   descriptor->interface_protocol);
	} else if (matched_driver != NULL) {
		hal_printf("usb%u: device %u interface %u class %02x/%02x/%02x "
			   "driver=%s attach-failed error=%d\n",
			   bus, address, descriptor->interface_number,
			   descriptor->interface_class,
			   descriptor->interface_subclass,
			   descriptor->interface_protocol, matched_driver->name,
			   error);
	} else {
		hal_printf("usb%u: device %u interface %u class %02x/%02x/%02x "
			   "probe-failed error=%d\n",
			   bus, address, descriptor->interface_number,
			   descriptor->interface_class,
			   descriptor->interface_subclass,
			   descriptor->interface_protocol, error);
	}
}

/* Publishes the result a transfer ended with. */
static int
urb_publish_terminal(
	struct drv_usb_urb *urb,
	enum drv_usb_urb_status status,
	size_t actual)
{
	struct usb_submit_commit *commit;
	unsigned expected = 0;

	/* Handles the urb availability. */
	if (urb == NULL || status == DRV_USB_URB_IDLE ||
	    status == DRV_USB_URB_PENDING) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the hal atomic load acquire result. */
	if (hal_atomic_load_acquire(&urb->status) != DRV_USB_URB_PENDING)
		return 0;

	/*
	 * Completion and successful cancellation can both publish the terminal
	 * state.  Whichever path arrives first must release the short submit
	 * gates before a callback can re-enter detach or disconnect.
	 */

	/* Handles the commit availability. */
	commit = __atomic_exchange_n(&urb->submit_commit, NULL,
				     __ATOMIC_ACQ_REL);
	if (commit != NULL)
		submit_commit_finish(urb, commit);
	else
		/* Continue while the operation condition remains true. */
		while (hal_atomic_load_acquire(&urb->submit_commit_pending) !=
		       0)
			sched_yield();

	/* Checks the hal atomic compare exchange acq rel result. */
	if (!hal_atomic_compare_exchange_acq_rel(&urb->terminal_claimed,
						 &expected, 1U)) {
		/* Succeeded. */
		return 0;
	}

	/*
	 * Publish the protocol halt before the terminal status and callback.
	 * HCDs keep their endpoint publication barrier until this function
	 * returns, so a rearm cannot cross the hardware-STALL/core-latch
	 * handoff.
	 */
	if (status == DRV_USB_URB_STALL)
		endpoint_publish_halted(urb->device, urb->endpoint, 1U);
	urb->actual_length = actual > urb->length ? urb->length : actual;

	/* The terminal state publishes actual_length and all HCD input data. */
	hal_atomic_store_release(&urb->status, status);

	/* Handles the callback availability. */
	if (urb->callback != NULL)
		urb->callback(urb, urb->callback_argument);

	/* Reports operation failure. */
	return 1;
}

/* Finishes a submission that has reached the controller. */
static void
submit_commit_finish(
	struct drv_usb_urb *urb,
	struct usb_submit_commit *commit)
{
	binding_submitter_put(commit->binding_owner);
	io_gate_exit(&commit->device->submit_gate);

	/*
	 * A terminal publisher may have observed that the stack record was
	 * already claimed by the submitting CPU.  Make this the last URB access
	 * before allowing that publisher to enter a callback which can free the
	 * URB.
	 */
	hal_atomic_store_release(&urb->submit_commit_pending, 0U);
	atomic_store_release(&commit->finished, 1U);
}

/* Gives up the submission reference a binding held. */
static void
binding_submitter_put(
	struct drv_usb_interface *owner)
{
	/* Handles the owner availability. */
	if (owner == NULL)
		return;
	io_gate_exit(&owner->binding_submitters);
}

/* Records that an endpoint has fallen into a halt. */
static void
endpoint_publish_halted(
	struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint,
	unsigned halted)
{
	/* Handles the endpoint uses halt condition. */
	if (endpoint_uses_halt(device, endpoint))
		atomic_store_release(&endpoint->halted, halted);
}

/* Asks whether an endpoint's type can halt at all. */
static int
endpoint_uses_halt(
	const struct drv_usb_device *device,
	const struct drv_usb_endpoint *endpoint)
{
	/* Returns the computed result. */
	return endpoint != NULL && endpoint != &device->endpoint0 &&
	       (endpoint->type == DRV_USB_TRANSFER_BULK ||
		endpoint->type == DRV_USB_TRANSFER_INTERRUPT);
}

/* Gives up the reference the controller held on a transfer. */
static void
urb_hcd_put(
	struct drv_usb_urb *urb)
{
	unsigned expected = 1U;

	/* Checks the hal atomic compare exchange acq rel result. */
	if (hal_atomic_compare_exchange_acq_rel(&urb->hcd_owned, &expected,
						2U)) {
		/* Checks the hal atomic fetch add release result. */
		if (hal_atomic_fetch_add_release(&urb->device->hcd_urb_count,
						 (unsigned)-1) == 0)
			__builtin_trap();
		urb_admission_put(urb);
		hal_atomic_store_release(&urb->hcd_owned, 0U);
		urb_put(urb);
	}
}

/* Gives up the reference admission held on a transfer. */
static void
urb_admission_put(
	struct drv_usb_urb *urb)
{
	struct drv_usb_interface *binding_owner = urb->binding_owner;
	struct drv_usb_interface *io_interface = urb->io_interface;
	unsigned control_admitted = urb->control_admitted;

	urb->binding_owner = NULL;
	urb->io_interface = NULL;
	urb->control_admitted = USB_CONTROL_ADMISSION_NONE;

	/* Handles the urb condition. */
	if (urb->control_counted) {
		urb->control_counted = 0;
		atomic_store_release(&urb->device->control_inflight, 0U);
	}

	/* Handles the binding owner availability. */
	if (binding_owner != NULL)
		io_gate_exit(&binding_owner->binding_gate);

	/* Handles the io interface availability. */
	if (io_interface != NULL)
		io_gate_exit(&io_interface->io_gate);

	/* Handles the control admitted condition. */
	if (control_admitted == USB_CONTROL_ADMISSION_OWNED)
		device_control_unlock(urb->device);
}

/* Gives the control endpoint of a device back. */
static void
device_control_unlock(
	struct drv_usb_device *device)
{
	atomic_store_release(&device->control_gate, 0U);
}

/* Gives up one reference to a transfer. */
static void
urb_put(
	struct drv_usb_urb *urb)
{
	struct drv_usb_device *device;

	/* Checks the refcount put result. */
	if (!refcount_put(&urb->references))
		return;

	/* Handles the transfer reservation availability. */
	device = urb->device;
	if (urb->transfer_reservation != NULL) {
		device->bus->hcd->ops->urb_unreserve(device->bus->hcd,
						     urb->transfer_reservation);
		io_stats_record(IO_USB_TRANSFER_RESERVE_FREE,
				urb->transfer_capacity);
	}

	/* Handles the iso packets availability. */
	if (urb->iso_packets != NULL)
		hal_free(urb->iso_packets);

	/* Handles the sync buffer availability. */
	if (urb->sync_buffer != NULL && !urb->sync_shared)
		io_stats_record(IO_USB_BUFFER_FREE, urb->sync_capacity);

	/* Handles the urb condition. */
	if (!urb->sync_shared)
		hal_free(urb->sync_buffer);
	hal_free(urb);
	device_urb_put(device);
}

/* Gives up the reference a device held on a transfer. */
static void
device_urb_put(
	struct drv_usb_device *device)
{
	unsigned previous;

	previous =
		hal_atomic_fetch_add_release(&device->lifecycle, (unsigned)-1);

	/* Handles the previous condition. */
	if ((previous & USB_DEVICE_LIFECYCLE_URB_MASK) == 0)
		__builtin_trap();

	/* Handles the previous condition. */
	if ((previous & USB_DEVICE_LIFECYCLE_URB_MASK) == 1U)
		hal_atomic_fence_acquire();
}

/* Reports which driver a configuration is really owned by. */
static int
configuration_effective_owner(
	struct drv_usb_configuration *configuration,
	struct drv_usb_interface **effective_owner)
{
	struct drv_usb_interface *interface, *owner;
	unsigned state;

	*effective_owner = NULL;
	/* Handles the configuration availability. */
	if (configuration == NULL)
		return 0;
	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Handles the owner availability. */
		owner = interface_binding_owner(interface);
		if (owner == NULL)
			continue;

		/* Handles the state condition. */
		state = atomic_load_acquire(&owner->binding_state);
		if (state != USB_BINDING_PROBING && state != USB_BINDING_BOUND)
			return EBUSY;

		/* Handles the effective owner availability. */
		if (*effective_owner == NULL)
			*effective_owner = owner;
		else if (*effective_owner != owner)

			/* Failed. */
			return ENOTSUP;
	}

	/* Succeeded. */
	return 0;
}

/* Reports the driver an interface is bound to. */
static struct drv_usb_interface *
interface_binding_owner(
	struct drv_usb_interface *interface)
{
	struct drv_usb_interface *function_result;
	struct drv_usb_interface *owner = interface_claim_owner(interface);

	/* Handles the owner availability. */
	if (owner != NULL)
		return owner;

	/* Computes the function result. */
	function_result =
		atomic_load_acquire(&interface->binding_state) != USB_BINDING_DEAD ? interface : NULL;

	/* Returns the computed result. */
	return function_result;
}

/* Closes the transfer gate of every interface of a configuration. */
static int
configuration_close_io(
	struct drv_usb_configuration *configuration,
	struct drv_usb_interface **closed,
	unsigned *closed_count)
{
	struct drv_usb_interface *interface;

	*closed_count = 0;
	/* Handles the configuration availability. */
	if (configuration == NULL)
		return 0;
	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Checks the io gate close empty result. */
		if (io_gate_close_empty(&interface->io_gate) != 0) {
			/* Process each remaining element. */
			while (*closed_count != 0)
				io_gate_open(&closed[--*closed_count]->io_gate);

			/* Failed. */
			return EBUSY;
		}

		closed[(*closed_count)++] = interface;
	}

	/* Succeeded. */
	return 0;
}

/* Takes the control endpoint of a device, if it is free. */
static int
device_control_try_lock(
	struct drv_usb_device *device)
{
	/* Checks the atomic try acquire zero result. */
	if (!atomic_try_acquire_zero(&device->control_gate))
		return EBUSY;

	/*
	 * A timed-out synchronous caller may release its transaction while its
	 * isolated URB is still retained. EP0 remains unavailable until the HCD
	 * drops that request, independently of the caller's bounded wait.
	 */
	if (atomic_load_acquire(&device->control_inflight) != 0) {
		atomic_store_release(&device->control_gate, 0U);

		/* Failed. */
		return EBUSY;
	}

	/* Succeeded. */
	return 0;
}

/* Asks whether a device is still the one that was reset. */
static int
device_reset_connection_check(
	struct drv_usb_bus *bus,
	struct drv_usb_device *device,
	uint64_t device_generation,
	uint64_t port_generation)
{
	uint32_t status = 0;
	int error;

	/* Checks the operation status. */
	error = root_port_status(bus->hcd, device->port, &status);
	if (error != 0)
		return error;

	/*
	 * GET_STATUS does not acknowledge CSC.  With the topology worker
	 * excluded, the hardware edge bit is the only witness for a
	 * detach/reinsert which happens between two restore operations.
	 */
	if ((status & 3U) != 3U || (status & (1U << 16)) != 0 ||
	    device->generation != device_generation ||
	    bus->ports[device->port].connection_generation != port_generation) {
		/* Failed. */
		return ENODEV;
	}

	/* Succeeded. */
	return 0;
}

/* Configures every endpoint of one configuration. */
static int
configuration_enable_endpoints(
	struct drv_usb_configuration *configuration)
{
	int rollback;
	struct drv_usb_interface *interface;
	struct drv_usb_interface *enabled[DRV_USB_MAX_INTERFACES];
	unsigned enabled_count = 0;
	int error, rollback_error = 0;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Checks the operation status. */
		error = host_interface_enable(
			interface_active_alternate(interface));
		if (error != 0) {
			/* Process each remaining element. */
			while (enabled_count != 0) {
				interface = enabled[--enabled_count];

				/* Checks the operation status. */
				rollback = host_interface_disable(
					interface_active_alternate(interface));
				if (rollback_error == 0 && rollback != 0)
					rollback_error = rollback;
			}
			if (rollback_error != 0) {
				device_quarantine_selection(
					configuration->device,
					"configuration-enable", rollback_error);
			}

			/* Failed. */
			return error;
		}

		enabled[enabled_count++] = interface;
	}

	/* Succeeded. */
	return 0;
}

/* Puts a device out of use after a failed recovery. */
static void
device_quarantine_recovery(
	struct drv_usb_device *device,
	const char *stage,
	int error)
{
	/* Checks the device is quarantined result. */
	if (!device_is_quarantined(device)) {
		hal_printf("usb%u: device %u %s failed (%d); quarantined\n",
			   device->bus->number, device->address, stage, error);
	}

	hal_atomic_store_release(&device->quarantined, 1U);
	io_gate_close(&device->submit_gate);
}

/* Runs one control transfer with the device's control lock held. */
static int
usb_control_locked(
	struct drv_usb_device *device,
	uint8_t request_type,
	uint8_t request,
	uint16_t value,
	uint16_t index,
	void *buffer,
	size_t length,
	unsigned timeout_ms,
	size_t *actual)
{
	int function_result;
	struct drv_usb_control_request control = {request_type, request, value,
						  index, (uint16_t)length};
	struct drv_usb_urb *urb;
	int error;

	/* Handles the device availability. */
	if (device == NULL || length > UINT16_MAX)
		return EINVAL;

	/* Handles the device is disconnecting condition. */
	if (device_is_disconnecting(device) || device_is_quarantined(device))
		return ENODEV;

	/* Handles the urb availability. */
	urb = drv_usb_urb_alloc(device, NULL, 0);
	if (urb == NULL) {
		/* Computes the function result. */
		function_result = device_is_disconnecting(device) ||
						  device_is_quarantined(device)
					  ? ENODEV
					  : ENOMEM;

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the operation status. */
	error = drv_usb_urb_reserve_sync(urb, length);
	if (error == 0) {
		error = drv_usb_urb_setup_control(urb, &control, buffer, length,
						  timeout_ms, NULL, NULL);
	}
	if (error == 0) {
		urb->control_admitted = USB_CONTROL_ADMISSION_EXTERNAL;
		error = drv_usb_urb_submit(urb);
	}
	if (error == 0)
		error = drv_usb_urb_wait_reusable(urb);

	/* Handles the actual availability. */
	if (actual != NULL)
		*actual = drv_usb_urb_actual_length(urb);
	drv_usb_urb_free(urb);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Puts a configuration back the way it was before a change. */
static int
configuration_restore(
	struct drv_usb_device *device,
	struct drv_usb_configuration *configuration)
{
	int function_result;
	struct drv_usb_host_interface *alternate;
	struct drv_usb_interface *interface;
	size_t actual = 0;
	int error;

	/* Handles the configuration availability. */
	if (configuration == NULL) {
		/* Obtains the usb control locked result. */
		function_result = usb_control_locked(
			device,
			DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
				DRV_USB_RECIP_DEVICE,
			USB_REQ_SET_CONFIGURATION, 0, 0, NULL, 0,
			USB_CONTROL_TIMEOUT_MS, &actual);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the operation status. */
	error = usb_control_locked(
		device,
		DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_DEVICE,
		USB_REQ_SET_CONFIGURATION,
		configuration->descriptor.configuration_value, 0, NULL, 0,
		USB_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;
	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Handles the alternate condition. */
		alternate = interface_active_alternate(interface);
		if (alternate->descriptor.alternate_setting == 0)
			continue;

		/* Checks the operation status. */
		error = usb_control_locked(
			device,
			DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
				DRV_USB_RECIP_INTERFACE,
			USB_REQ_SET_INTERFACE,
			alternate->descriptor.alternate_setting,
			alternate->descriptor.interface_number, NULL, 0,
			USB_CONTROL_TIMEOUT_MS, &actual);
		if (error != 0)
			return error;
	}

	error = configuration_enable_endpoints(configuration);

	/* Computes the function result. */
	function_result =
		error != 0 ? error
			   : configuration_reset_endpoints(configuration);

	/* Returns the computed result. */
	return function_result;
}

/* Clears the halt of every endpoint of a configuration. */
static int
configuration_reset_endpoints(
	struct drv_usb_configuration *configuration)
{
	struct drv_usb_host_interface *alternate_local;
	struct drv_usb_host_interface *alternate_local1;
	struct drv_usb_interface *interface;
	struct drv_usb_device *device = configuration->device;
	struct drv_usb_hcd *hcd = device->bus->hcd;
	unsigned index;
	int error;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		alternate_local = interface_active_alternate(interface);

		/* Process each remaining element. */
		for (index = 0; index < alternate_local->endpoint_count;
		     index++) {
			/* Checks the operation status. */
			error = hcd->ops->endpoint_reset(
				hcd, &alternate_local->endpoints[index]);
			if (error != 0)
				return error;
		}
	}

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		alternate_local1 = interface_active_alternate(interface);

		/* Process each remaining element. */
		for (index = 0; index < alternate_local1->endpoint_count;
		     index++) {
			endpoint_publish_halted(
				device, &alternate_local1->endpoints[index],
				0U);
		}
	}

	/* Succeeded. */
	return 0;
}

/* Opens the transfer gate of every interface of a configuration. */
static void
configuration_open_io(
	struct drv_usb_interface **closed,
	unsigned closed_count)
{
	/* Process each remaining element. */
	while (closed_count != 0)
		io_gate_open(&closed[--closed_count]->io_gate);
}

/* Asks whether any driver still holds part of a configuration. */
static int
configuration_has_owners(
	const struct drv_usb_configuration *configuration)
{
	const struct drv_usb_interface *interface;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		/* Checks the atomic load acquire result. */
		if (atomic_load_acquire(&interface->binding_state) !=
			    USB_BINDING_DEAD ||
		    interface_claim_owner(interface) != NULL) {
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Selects the default alternate setting of every interface. */
static void
configuration_select_defaults(
	struct drv_usb_configuration *configuration)
{
	struct drv_usb_interface *interface;

	/* Process each linked entry. */
	for (interface = configuration->interfaces; interface != NULL;
	     interface = interface->next) {
		interface_publish_alternate(
			interface, interface_find_alternate(interface, 0));
	}
}

/* Reads one string descriptor and renders it as UTF-8. */
static int
usb_string_descriptor(
	struct drv_usb_device *device,
	uint8_t index,
	uint16_t language,
	uint8_t *descriptor,
	size_t *descriptor_length)
{
	uint8_t header[2];
	size_t actual = 0;
	int error;

	/* Reads the header to learn how long the string is. */

	/* Checks the operation status. */
	error = drv_usb_control(
		device,
		DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_DEVICE,
		USB_REQ_GET_DESCRIPTOR,
		(uint16_t)((DRV_USB_DESCRIPTOR_STRING << 8) | index), language,
		header, sizeof(header), USB_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;

	/* Handles the actual condition. */
	if (actual != sizeof(header) || header[0] < 2U ||
	    (header[0] & 1U) != 0 || header[1] != DRV_USB_DESCRIPTOR_STRING) {
		/* Failed. */
		return EILSEQ;
	}

	/* Checks the operation status. */
	error = drv_usb_control(
		device,
		DRV_USB_DIR_IN | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_DEVICE,
		USB_REQ_GET_DESCRIPTOR,
		(uint16_t)((DRV_USB_DESCRIPTOR_STRING << 8) | index), language,
		descriptor, header[0], USB_CONTROL_TIMEOUT_MS, &actual);
	if (error != 0)
		return error;

	/* Handles the actual condition. */
	if (actual != header[0] || descriptor[0] != header[0] ||
	    descriptor[1] != DRV_USB_DESCRIPTOR_STRING) {
		/* Failed. */
		return EILSEQ;
	}
	*descriptor_length = actual;
	/* Succeeded. */
	return 0;
}

/* Appends one code point to a UTF-8 string being built. */
static int
utf8_append(
	char *buffer,
	size_t capacity,
	size_t *used,
	uint32_t codepoint)
{
	uint8_t encoded[4];
	unsigned count, index;

	/* Handles the codepoint condition. */
	if (codepoint == 0 || codepoint > 0x10ffffU ||
	    (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
		/* Failed. */
		return EILSEQ;
	}

	/* Handles the codepoint condition. */
	if (codepoint < 0x80U) {
		encoded[0] = (uint8_t)codepoint;
		count = 1;
	} else if (codepoint < 0x800U) {
		encoded[0] = (uint8_t)(0xc0U | (codepoint >> 6));
		encoded[1] = (uint8_t)(0x80U | (codepoint & 0x3fU));
		count = 2;
	} else if (codepoint < 0x10000U) {
		encoded[0] = (uint8_t)(0xe0U | (codepoint >> 12));
		encoded[1] = (uint8_t)(0x80U | ((codepoint >> 6) & 0x3fU));
		encoded[2] = (uint8_t)(0x80U | (codepoint & 0x3fU));
		count = 3;
	} else {
		encoded[0] = (uint8_t)(0xf0U | (codepoint >> 18));
		encoded[1] = (uint8_t)(0x80U | ((codepoint >> 12) & 0x3fU));
		encoded[2] = (uint8_t)(0x80U | ((codepoint >> 6) & 0x3fU));
		encoded[3] = (uint8_t)(0x80U | (codepoint & 0x3fU));
		count = 4;
	}

	/* Checks the current capacity usage. */
	if (*used > capacity || count >= capacity - *used)
		return ENOSPC;
	/* Process each remaining element. */
	for (index = 0; index < count; index++)
		buffer[(*used)++] = (char)encoded[index];

	/* Succeeded. */
	return 0;
}

/* Takes a reference on a transfer for its device. */
static int
device_urb_get(
	struct drv_usb_device *device)
{
	unsigned state;

	state = hal_atomic_load_acquire(&device->lifecycle);
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the state condition. */
		if ((state & (USB_DEVICE_LIFECYCLE_DISCONNECTING |
			      USB_DEVICE_LIFECYCLE_FINALIZING)) != 0 ||
		    (state & USB_DEVICE_LIFECYCLE_URB_MASK) ==
			    USB_DEVICE_LIFECYCLE_URB_MASK) {
			/* Succeeded. */
			return 0;
		}

		/* Checks the hal atomic compare exchange acq rel result. */
		if (hal_atomic_compare_exchange_acq_rel(&device->lifecycle,
							&state, state + 1U)) {
			/* Reports operation failure. */
			return 1;
		}
	}
}

/* Asks whether a device still holds an endpoint's state. */
static int
endpoint_retained_by_device(
	const struct drv_usb_device *device,
	const struct drv_usb_endpoint *endpoint)
{
	const struct drv_usb_interface *interface;
	const struct drv_usb_host_interface *alternate;
	unsigned index;

	/* Handles the device availability. */
	if (device == NULL || endpoint == NULL || endpoint->interface == NULL ||
	    endpoint->alternate == NULL) {
		/* Succeeded. */
		return 0;
	}

	/* Handles the configuration availability. */
	interface = endpoint->interface;
	if (interface->device != device || interface->configuration == NULL ||
	    interface->configuration->device != device ||
	    endpoint->alternate->interface != interface) {
		/* Succeeded. */
		return 0;
	}
	/* Process each linked entry. */
	for (alternate = interface->alternates; alternate != NULL;
	     alternate = alternate->next) {
		/* Handles the alternate condition. */
		if (alternate == endpoint->alternate)
			break;
	}

	/* Handles the alternate availability. */
	if (alternate == NULL)
		return 0;
	/* Process each remaining element. */
	for (index = 0; index < alternate->endpoint_count; index++) {
		/* Handles the alternate condition. */
		if (&alternate->endpoints[index] == endpoint)
			return 1;
	}

	/* Succeeded. */
	return 0;
}

/* Asks whether an endpoint has fallen into a halt. */
static int
endpoint_is_halted(
	const struct drv_usb_device *device,
	const struct drv_usb_endpoint *endpoint)
{
	int error;

	/* Computes the function result. */
	error = endpoint_uses_halt(device, endpoint) &&
			  atomic_load_acquire(&endpoint->halted) != 0;

	/* Returns the computed result. */
	return error;
}

/* Takes a reference on a transfer for the controller. */
static int
urb_hcd_get(
	struct drv_usb_urb *urb)
{
	unsigned expected = 0;

	/* Checks the hal atomic compare exchange acq rel result. */
	if (!hal_atomic_compare_exchange_acq_rel(&urb->hcd_owned, &expected,
						 1U)) {
		/* Failed. */
		return EBUSY;
	}

	/*
	 * The submitting caller owns a reference until this function returns,
	 * so publishing HCD ownership before taking its reference is safe.  No
	 * HCD can complete the URB until the later enqueue callback.
	 */
	refcount_get(&urb->references);

	/* Checks the hal atomic fetch add relaxed result. */
	if (hal_atomic_fetch_add_relaxed(&urb->device->hcd_urb_count, 1U) ==
	    UINT_MAX)
		__builtin_trap();

	/* Succeeded. */
	return 0;
}

/* Takes a reference on a transfer for admission. */
static int
urb_admission_get(
	struct drv_usb_urb *urb,
	struct drv_usb_interface **submitting_owner)
{
	struct drv_usb_device *device = urb->device;
	struct drv_usb_endpoint *endpoint = urb->endpoint;
	struct drv_usb_interface *interface;
	int error;

	*submitting_owner = NULL;
	/* Handles the endpoint condition. */
	if (endpoint == &device->endpoint0) {
		/* Handles the urb condition. */
		if (urb->control_admitted != USB_CONTROL_ADMISSION_EXTERNAL) {
			/* Checks the operation status. */
			error = device_control_try_lock(device);
			if (error != 0)
				return error;
			urb->control_admitted = USB_CONTROL_ADMISSION_OWNED;
		}

		/* Checks the atomic try acquire zero result. */
		if (!atomic_try_acquire_zero(&device->control_inflight))
			return EBUSY;
		urb->control_counted = 1U;

		/* Succeeded. */
		return 0;
	}

	/* Checks the endpoint retained by device result. */
	if (!endpoint_retained_by_device(device, endpoint))
		return EINVAL;
	interface = endpoint->interface;

	/* Checks the operation status. */
	error = io_gate_enter(&interface->io_gate);
	if (error != 0)
		return error;
	urb->io_interface = interface;

	/* Checks the operation status. */
	error = binding_admission_enter(urb, interface, submitting_owner);
	if (error != 0) {
		urb_admission_put(urb);

		/* Failed. */
		return error;
	}

	/* Checks the device active configuration result. */
	if (device->state != DRV_USB_STATE_CONFIGURED ||
	    device_active_configuration(device) != interface->configuration ||
	    interface_active_alternate(interface) != endpoint->alternate) {
		urb_admission_put(urb);

		/* Failed. */
		return ENODEV;
	}

	/* Succeeded. */
	return 0;
}

/* Joins the gate that admits a binding's transfers. */
static int
binding_admission_enter(
	struct drv_usb_urb *urb,
	struct drv_usb_interface *interface,
	struct drv_usb_interface **submitting_owner)
{
	struct drv_usb_interface *owner;
	unsigned state;
	int error;

	*submitting_owner = NULL;

	/* Handles the owner availability. */
	owner = interface_binding_owner(interface);
	if (owner == NULL)
		return 0;

	/* Handles the state condition. */
	state = atomic_load_acquire(&owner->binding_state);
	if (state != USB_BINDING_PROBING && state != USB_BINDING_BOUND)
		return ENODEV;

	/* Checks the operation status. */
	error = io_gate_enter(&owner->binding_submitters);
	if (error != 0) {
		state = atomic_load_acquire(&owner->binding_state);

		/* Returns the computed result. */
		return state == USB_BINDING_PROBING ||
				       state == USB_BINDING_BOUND
			       ? error
			       : ENODEV;
	}

	*submitting_owner = owner;

	/* Handles the state condition. */
	state = atomic_load_acquire(&owner->binding_state);
	if (state != USB_BINDING_PROBING && state != USB_BINDING_BOUND) {
		binding_submitter_put(owner);
		*submitting_owner = NULL;
		/* Failed. */
		return ENODEV;
	}

	/* Checks the operation status. */
	error = io_gate_enter(&owner->binding_gate);
	if (error != 0) {
		binding_submitter_put(owner);
		*submitting_owner = NULL;
		state = atomic_load_acquire(&owner->binding_state);

		/* Returns the computed result. */
		return state == USB_BINDING_PROBING ||
				       state == USB_BINDING_BOUND
			       ? error
			       : ENODEV;
	}

	/* Checks the interface binding owner result. */
	state = atomic_load_acquire(&owner->binding_state);
	if ((state != USB_BINDING_PROBING && state != USB_BINDING_BOUND) ||
	    interface_binding_owner(interface) != owner) {
		io_gate_exit(&owner->binding_gate);
		binding_submitter_put(owner);
		*submitting_owner = NULL;
		/* Failed. */
		return ENODEV;
	}

	urb->binding_owner = owner;

	/* Succeeded. */
	return 0;
}

/* Cancels a transfer and waits for it to reach a given state. */
static int
urb_cancel_to(
	struct drv_usb_urb *u,
	enum drv_usb_urb_status terminal)
{
	int e, published;

	/* Checks the hal atomic load acquire result. */
	if (!u || hal_atomic_load_acquire(&u->status) != DRV_USB_URB_PENDING)
		return EINVAL;

	/* Handles the e condition. */
	e = u->device->bus->hcd->ops->urb_dequeue(u->device->bus->hcd, u);
	if (e)
		return e;
	published = urb_publish_terminal(u, terminal, 0);
	urb_hcd_put(u);

	/* Returns the computed result. */
	return published ? 0 : EALREADY;
}

/* Takes the control endpoint of a device and waits for it. */
static int
device_control_lock(
	struct drv_usb_device *device,
	unsigned timeout_ms)
{
	uint64_t now;
	uint64_t ticks;
	uint64_t deadline = 0;

	/* Handles the timeout ms condition. */
	if (timeout_ms != 0) {
		now = sched_ticks();
		ticks = ((uint64_t)timeout_ms + 9U) / 10U;

		deadline = UINT64_MAX - now < ticks ? UINT64_MAX : now + ticks;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the device control try lock result. */
		if (device_control_try_lock(device) == 0)
			return 0;

		/* Handles the device is disconnecting condition. */
		if (device_is_disconnecting(device) ||
		    device_is_quarantined(device)) {
			/* Failed. */
			return ENODEV;
		}

		/* Checks the hal atomic load acquire result. */
		if (hal_atomic_load_acquire(&device->bus->stopping) != 0)
			return EBUSY;

		/* Checks the sched ticks result. */
		if (deadline != 0 && sched_ticks() >= deadline)
			return ETIMEDOUT;
		sched_yield();
	}
}

/* Waits for the data a transfer moved to become visible. */
static int
sync_data(
	struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint,
	void *buffer,
	size_t length,
	unsigned timeout_ms,
	size_t *actual)
{
	struct drv_usb_urb *urb;
	int error;

	/* Handles the device availability. */
	if (device == NULL || endpoint == NULL)
		return EINVAL;

	/* Handles the urb availability. */
	urb = drv_usb_urb_alloc(device, endpoint, 0);
	if (urb == NULL)
		return ENOMEM;

	/* Checks the operation status. */
	error = drv_usb_urb_reserve_sync(urb, length);
	if (error == 0) {
		error = drv_usb_urb_setup(urb, buffer, length, 0, timeout_ms,
					  NULL, NULL);
	}
	if (error == 0)
		error = drv_usb_urb_submit(urb);
	if (error == 0)
		error = drv_usb_urb_wait_reusable(urb);

	/* Handles the actual availability. */
	if (actual != NULL)
		*actual = drv_usb_urb_actual_length(urb);
	drv_usb_urb_free(urb);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Clears the halt of every endpoint of one alternate setting. */
static int
host_interface_reset_endpoints(
	struct drv_usb_host_interface *alternate)
{
	struct drv_usb_device *device = alternate->interface->device;
	struct drv_usb_hcd *hcd = device->bus->hcd;
	unsigned index;
	int error;

	/*
	 * Keep every core latch closed until all host-side endpoint state
	 * agrees with the confirmed device-side reset.  A partial HCD reset is
	 * visible only inside a subsequently quarantined device.
	 */
	for (index = 0; index < alternate->endpoint_count; index++) {
		/* Checks the operation status. */
		error = hcd->ops->endpoint_reset(hcd,
						 &alternate->endpoints[index]);
		if (error != 0)
			return error;
	}

	/* Process each remaining element. */
	for (index = 0; index < alternate->endpoint_count; index++) {
		endpoint_publish_halted(device, &alternate->endpoints[index],
					0U);
	}

	/* Succeeded. */
	return 0;
}

/* Pins an endpoint's state while a binding uses it. */
static int
endpoint_binding_pin(
	struct drv_usb_interface *interface,
	struct drv_usb_interface **pinned_owner)
{
	int function_result;
	struct drv_usb_interface *owner;
	unsigned state;
	int error;

	*pinned_owner = NULL;

	/* Handles the owner availability. */
	owner = interface_binding_owner(interface);
	if (owner == NULL)
		return ENODEV;

	/* Handles the state condition. */
	state = atomic_load_acquire(&owner->binding_state);
	if (state != USB_BINDING_PROBING && state != USB_BINDING_BOUND)
		return ENODEV;

	/* Checks the operation status. */
	error = io_gate_enter(&owner->binding_submitters);
	if (error != 0) {
		/* Computes the function result. */
		function_result =
			atomic_load_acquire(&owner->binding_state) == state
				? error
				: ENODEV;

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the atomic load acquire result. */
	if (atomic_load_acquire(&owner->binding_state) != state) {
		binding_submitter_put(owner);

		/* Failed. */
		return ENODEV;
	}

	/* Checks the operation status. */
	error = io_gate_enter(&owner->binding_gate);
	if (error != 0) {
		binding_submitter_put(owner);

		/* Computes the function result. */
		function_result =
			atomic_load_acquire(&owner->binding_state) == state
				? error
				: ENODEV;

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the atomic load acquire result. */
	if (atomic_load_acquire(&owner->binding_state) != state ||
	    interface_binding_owner(interface) != owner) {
		io_gate_exit(&owner->binding_gate);
		binding_submitter_put(owner);

		/* Failed. */
		return ENODEV;
	}

	*pinned_owner = owner;
	/* Succeeded. */
	return 0;
}

/* Sends the device the request that clears an endpoint halt. */
static int
endpoint_clear_halt_request(
	struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint,
	unsigned *accepted)
{
	int function_result;
	struct drv_usb_control_request request = {
		DRV_USB_DIR_OUT | DRV_USB_REQUEST_STANDARD |
			DRV_USB_RECIP_ENDPOINT,
		USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
		endpoint->descriptor.address, 0};
	struct drv_usb_urb *urb = device->recovery_urb;
	int error;

	*accepted = 0;
	/* Handles the urb availability. */
	if (urb == NULL)
		return ENODEV;

	/* Checks the operation status. */
	error = drv_usb_urb_setup_control_flags(
		urb, &request, NULL, 0, DRV_USB_URB_RECLAIM_SAFE,
		USB_CONTROL_TIMEOUT_MS, NULL, NULL);
	if (error != 0)
		return error;
	urb->control_admitted = USB_CONTROL_ADMISSION_EXTERNAL;

	/* Checks the operation status. */
	error = drv_usb_urb_submit(urb);
	if (error != 0)
		return error;
	*accepted = 1U;
	/* Obtains the drv usb urb wait reusable result. */
	function_result = drv_usb_urb_wait_reusable(urb);

	/* Returns the computed result. */
	return function_result;
}

/* Gives that pin back. */
static void
endpoint_binding_unpin(
	struct drv_usb_interface *owner)
{
	/* Handles the owner availability. */
	if (owner == NULL)
		return;
	io_gate_exit(&owner->binding_gate);
	binding_submitter_put(owner);
}
