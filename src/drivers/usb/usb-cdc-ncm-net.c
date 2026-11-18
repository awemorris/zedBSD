/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* XXX: Need coding style fitting. */

/*
 * Integrated USB CDC NCM network driver
 */

#include <drivers/usb/usb-cdc-ncm.h>
#include <drivers/usb/usb-cdc-notification.h>
#include <drivers/usb/usb.h>
#include <uapi/errno.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/sched.h>
#include <limits.h>
#include "kern/klog.h"
#include "kern/kmem.h"
#include <kern/kcrt.h>

#define NCM_COMMUNICATION_CLASS		0x02U
#define NCM_COMMUNICATION_SUBCLASS	0x0dU
#define NCM_COMMUNICATION_PROTOCOL	0x00U
#define NCM_DATA_CLASS			0x0aU
#define NCM_DATA_SUBCLASS		0x00U
#define NCM_DATA_PROTOCOL		0x01U

#define NCM_CS_INTERFACE		0x24U
#define NCM_HEADER_DESCRIPTOR		0x00U
#define NCM_UNION_DESCRIPTOR		0x06U
#define NCM_ETHERNET_DESCRIPTOR		0x0fU
#define NCM_FUNCTIONAL_DESCRIPTOR	0x1aU

#define NCM_GET_NTB_PARAMETERS		0x80U
#define NCM_SET_ETHERNET_PACKET_FILTER	0x43U
#define NCM_FILTER_ALL_MULTICAST	0x0002U
#define NCM_FILTER_DIRECTED		0x0004U
#define NCM_FILTER_BROADCAST		0x0008U

#define NCM_NOTIFICATION_SIZE		DRV_USB_CDC_NOTIFICATION_TRANSFER
#define NCM_NTB_BUFFER_SIZE		8192U

/*
 * The largest receive block asked of the device, and how many frames of
 * one block are held for the stack.  A 64-bit machine's larger packet pool
 * (packet-buf.h) takes a 16 KB block, ten full frames, and up to the 32 a
 * block of small frames may carry, which would otherwise be refused whole
 * (BUG-222); the small pool of the 32-bit boards keeps 8 KB and 8 frames.
 */
#if PACKET_BUF_POOL_COUNT >= 256U
#define NCM_NTB_IN_SIZE			16384U
#define NCM_RX_QUEUE_MAX		DRV_USB_CDC_NCM_MAX_RX_DATAGRAMS
#else
#define NCM_NTB_IN_SIZE			NCM_NTB_BUFFER_SIZE
#define NCM_RX_QUEUE_MAX		8U
#endif

/*
 * Where the pool has jumbo packets, blocks large enough for jumbo
 * datagrams: a 32 KB receive block holds three of 9014 bytes (or the
 * standard ones a 16 KB block would).  A transmit block of up to 32 KB
 * carries a jumbo datagram or many standard ones; the device's
 * dwNtbOutMaxSize may be smaller (an RTL8156 takes 17404 bytes).
 * NCM_DATAGRAM_MAX is the longest datagram asked of the device.
 */
#if PACKET_BUF_JUMBO_COUNT != 0U && PACKET_BUF_POOL_COUNT >= 256U
#define NCM_NTB_IN_JUMBO_SIZE		32768U
#define NCM_NTB_OUT_SIZE		32768U
#else
#define NCM_NTB_IN_JUMBO_SIZE		NCM_NTB_IN_SIZE
#define NCM_NTB_OUT_SIZE		NCM_NTB_BUFFER_SIZE
#endif
#define NCM_DATAGRAM_MAX		(DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE + NET_MTU_MAX)

/*
 * Frames held while the one transmit transfer is busy, which the next
 * transfer carries together in one block (as many as fit it and
 * NCM_TX_BATCH_MAX): a 64-bit machine's larger pool holds a few blocks'
 * worth, the small pool of the 32-bit boards a burst of acknowledgements.
 */
#if PACKET_BUF_POOL_COUNT >= 256U
#define NCM_TX_QUEUE_MAX		64U
#else
#define NCM_TX_QUEUE_MAX		8U
#endif
#define NCM_TX_BATCH_MAX		DRV_USB_CDC_NCM_MAX_TX_DATAGRAMS

/*
 * The receive and transmit transfers kept on the bus at once, where the
 * host controller queues several on one bulk endpoint
 * (drv_usb_endpoint_set_queue_depth): with four receive blocks asked for,
 * the device has somewhere to put the next while the poll parses one, and
 * with two transmit blocks the next goes out behind the one on the wire.
 * A controller that queues one keeps one of each.
 */
#define NCM_RX_URBS			4U
#define NCM_TX_URBS			2U
#define NCM_TRANSFER_TIMEOUT_MS		5000U
#define NCM_CONTROL_TIMEOUT_MS		1000U
#define NCM_REARM_RETRY_MAX		3U
#define NCM_COMPLETION_KINDS		3U

#define NCM_COMPLETION_TX		0U
#define NCM_COMPLETION_NOTIFICATION	1U
#define NCM_COMPLETION_RX		2U

#define NCM_CAP_PACKET_FILTER		0x01U
#define NCM_CAP_MAX_DATAGRAM_SIZE	0x08U
#define NCM_CAP_CRC_MODE		0x10U

struct ncm_binding {
	struct drv_usb_device *device;
	struct drv_usb_interface *control;
	struct drv_usb_interface *data;
	struct drv_usb_endpoint *notification;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	unsigned data_alternate;
	uint8_t mac_string;
	uint8_t capabilities;
	uint16_t max_segment_size;
};

struct ncm_adapter {
	struct drv_usb_device *usb_device;
	struct drv_usb_interface *control;
	struct drv_usb_interface *data;
	struct drv_usb_endpoint *notification_endpoint;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct drv_usb_urb *notification_urb;
	struct drv_usb_urb *rx_urbs[NCM_RX_URBS];
	struct drv_usb_urb *tx_urbs[NCM_TX_URBS];
	unsigned rx_urb_count;
	unsigned tx_urb_count;
	struct net_device *net_device;
	struct drv_usb_cdc_ncm_profile profile;
	struct drv_usb_cdc_ncm_rx_state rx_state;
	struct spinlock lock;
	uint8_t *notification_buffer;
	/*
	 * The receive buffers, one more than the receive transfers: each
	 * transfer fills the one in rx_urb_buffers, and a completed block is
	 * parsed from its buffer while the transfer goes back on with the
	 * spare (rx_spare, NULL during the parse), which the parsed buffer
	 * then becomes.  Only the poll that holds the block and rearms changes
	 * them.
	 */
	uint8_t *rx_buffers[NCM_RX_URBS + 1U];
	uint8_t *rx_urb_buffers[NCM_RX_URBS];
	uint8_t *rx_spare;
	/* Each transmit transfer's block. */
	uint8_t *tx_buffers[NCM_TX_URBS];
	/*
	 * Under the adapter lock: the receive transfers on the bus in the
	 * order they were submitted (the controller completes them in that
	 * order), as indexes from rx_order_head; rx_done, a bit for each
	 * transfer whose completion has come and not been taken; rx_idle, a
	 * bit for each transfer off the bus and waiting for a rearm.
	 */
	unsigned rx_order[NCM_RX_URBS];
	unsigned rx_order_head;
	unsigned rx_order_count;
	unsigned rx_done;
	unsigned rx_idle;
	/*
	 * Under the adapter lock: tx_free, a bit for each transmit transfer
	 * off the bus; tx_done, a bit for each whose completion has come and
	 * not been taken.
	 */
	unsigned tx_free;
	unsigned tx_done;
	struct packet_buf *rx_queue[NCM_RX_QUEUE_MAX];
	size_t rx_queue_head;
	size_t rx_queue_count;
	unsigned data_alternate;
	uint8_t capabilities;
	/*
	 * The descriptor's wMaxSegmentSize, and the maximum datagram the
	 * device answered GET_MAX_DATAGRAM_SIZE with (0 when it was not
	 * asked or did not answer).
	 */
	uint16_t max_segment_size;
	uint16_t max_datagram_read_back;
	unsigned ready;
	unsigned starts_active;
	unsigned polls_active;
	unsigned poll_cursor;
	unsigned opened;
	unsigned closing;
	unsigned stopping;
	unsigned quarantined;
	unsigned notification_ready;
	unsigned notification_rearm;
	unsigned rx_rearm;
	unsigned notification_rearm_active;
	unsigned rx_rearm_active;
	unsigned notification_retries;
	unsigned rx_retries;
	/*
	 * A thread builds and submits transmit blocks (ncm_tx_pump), the only
	 * one at a time, so the blocks go out in the order of their frames and
	 * sequence numbers.
	 */
	unsigned tx_busy;
	/*
	 * Frames waiting for a transmit transfer, linked through their next
	 * field and owned by the adapter.  The adapter lock guards the list;
	 * the pump sends from the head, and stop frees what is left.
	 */
	struct packet_buf *tx_queue_head;
	struct packet_buf *tx_queue_tail;
	unsigned tx_queue_count;
	uint16_t tx_sequence;
	int stop_error;
	uint32_t upstream_bps;
	uint32_t downstream_bps;
	/*
	 * The notification transfer's length (one packet of an endpoint of
	 * short packets, BUG-222) and the start of a notification whose rest
	 * is still to come; the poll that takes the notification completion
	 * owns the reader, and open empties it before the transfer starts.
	 */
	size_t notification_length;
	struct drv_usb_cdc_notification_reader notification_reader;
};

static uint16_t ncm_le16(const uint8_t *bytes);
static struct drv_usb_configuration * ncm_interface_configuration(struct drv_usb_interface *interface);
static int ncm_iad_covers(const struct drv_usb_interface_association_descriptor *iad, unsigned interface_number);
static int ncm_iad_consistent(struct drv_usb_configuration *configuration, unsigned control_number, unsigned data_number);
static int ncm_control_descriptors(const struct drv_usb_host_interface *alternate, unsigned control_number, unsigned *data_number, uint8_t *mac_string, uint8_t *capabilities, uint16_t *max_segment_size);
static int ncm_find_notification(const struct drv_usb_host_interface *alternate, struct drv_usb_endpoint **result);
static int ncm_find_data_alternate(struct drv_usb_interface *data, struct drv_usb_endpoint **bulk_in, struct drv_usb_endpoint **bulk_out, unsigned *alternate_result);
static int ncm_binding_parse(struct drv_usb_interface *control, struct ncm_binding *binding);
static int ncm_hex(unsigned char character);
static int ncm_get_mac(const struct ncm_binding *binding, uint8_t mac[6]);
static int ncm_control(struct ncm_adapter *adapter, uint8_t request_type, uint8_t request, uint16_t value, void *buffer, size_t length, size_t *actual);
static int ncm_program_profile(struct ncm_adapter *adapter);
static int ncm_program_max_datagram(struct ncm_adapter *adapter);
static int ncm_set_max_datagram(struct ncm_adapter *adapter, uint16_t size, uint16_t *read_back);
static int ncm_program_packet_filter(struct ncm_adapter *adapter);
static int ncm_urb_status_error(enum drv_usb_urb_status status);
static int ncm_tx_status_is_error(enum drv_usb_urb_status status);
static void ncm_completion(struct drv_usb_urb *urb, void *argument);
static int ncm_start_urb(struct ncm_adapter *adapter, struct drv_usb_urb *urb, void *buffer, size_t length);
static int ncm_cancel_and_drain(struct drv_usb_urb *urb);
static void ncm_wait_activity(struct ncm_adapter *adapter);
static void ncm_free_rx_queue(struct ncm_adapter *adapter);
static int ncm_stop(struct ncm_adapter *adapter);
static int ncm_open(struct net_device *device);
static void ncm_close(struct net_device *device);
static int ncm_transmit(struct net_device *device, struct packet_buf *packet);
static int ncm_tx_submit(struct ncm_adapter *adapter, unsigned index, struct packet_buf *packets);
static void ncm_tx_pump(struct ncm_adapter *adapter);
static int ncm_rx_start_idle(struct ncm_adapter *adapter);
static int ncm_rx_head_done_locked(const struct ncm_adapter *adapter);
static unsigned ncm_lowest_bit(unsigned mask);
static void ncm_tx_queue_free(struct ncm_adapter *adapter);
static int ncm_queue_datagram(const void *frame, size_t length, void *argument);
static void ncm_notification_process(struct ncm_adapter *adapter);
static void ncm_notification_take(struct ncm_adapter *adapter, const struct drv_usb_cdc_notification *notification);
static int ncm_rearm(struct ncm_adapter *adapter, int notification);
static unsigned ncm_deliver_queued(struct ncm_adapter *adapter, unsigned budget);
static int ncm_poll_enter(struct ncm_adapter *adapter);
static void ncm_poll_exit(struct ncm_adapter *adapter);
static int ncm_take_pending(struct ncm_adapter *adapter, unsigned *pending);
static void ncm_restore_pending(struct ncm_adapter *adapter, unsigned *pending);
static int ncm_take_notification_rearm(struct ncm_adapter *adapter);
static int ncm_take_rx_rearm(struct ncm_adapter *adapter);
static int ncm_poll_tx_completion(struct ncm_adapter *adapter);
static int ncm_poll_notification_completion(struct ncm_adapter *adapter);
static int ncm_poll_rx_completion(struct net_device *device, struct ncm_adapter *adapter);
static int ncm_has_poll_work(struct ncm_adapter *adapter);
static unsigned ncm_poll_receive(struct net_device *device, unsigned budget);
static void ncm_release(void *driver_data);
static void ncm_set_ready(struct ncm_adapter *adapter, int ready);
static void ncm_urbs_free(struct ncm_adapter *adapter);
static int ncm_urbs_alloc(struct ncm_adapter *adapter);
static void ncm_buffers_free(struct ncm_adapter *adapter);
static int ncm_buffers_alloc(struct ncm_adapter *adapter);
static int ncm_net_device_create(struct ncm_adapter *adapter, const uint8_t mac[6]);
static int ncm_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id);
static int ncm_detach(struct drv_usb_interface *interface, unsigned flags);
static void ncm_shutdown(struct drv_usb_interface *interface);
static int ncm_match(struct drv_usb_interface *interface, const struct drv_usb_id *id);

static const struct net_device_ops ncm_net_ops = {
	.open = ncm_open,
	.close = ncm_close,
	.transmit = ncm_transmit,
	.poll_receive = ncm_poll_receive,
	.release = ncm_release
};

static const struct drv_usb_id ncm_ids[] = {
	{
		.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS | DRV_USB_ID_IF_PROTOCOL,
		.interface_class = NCM_COMMUNICATION_CLASS,
		.interface_subclass = NCM_COMMUNICATION_SUBCLASS,
		.interface_protocol = NCM_COMMUNICATION_PROTOCOL
	}
};

static struct drv_usb_driver ncm_driver = {
	.name = "usb-cdc-ncm",
	.ids = ncm_ids,
	.id_count = sizeof(ncm_ids) / sizeof(ncm_ids[0]),
	.match = ncm_match,
	.attach = ncm_attach,
	.detach = ncm_detach,
	.shutdown = ncm_shutdown
};

/*
 * Registers this driver with the USB subsystem.
 */
int
drv_usb_cdc_ncm_driver_register(
	void)
{
	int error;

	/* Obtains the drv usb driver register result. */
	error = drv_usb_driver_register(&ncm_driver);

	/* Returns the computed result. */
	return error;
}

/* Reads a 16-bit field, least significant byte first. */
static uint16_t
ncm_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/* Reports the configuration an interface belongs to. */
static struct drv_usb_configuration *
ncm_interface_configuration(
	struct drv_usb_interface *interface)
{
	struct drv_usb_configuration *configuration;
	unsigned interface_index;
	struct drv_usb_device *device = drv_usb_interface_device(interface);
	unsigned configuration_index;

	/* Process each remaining element. */
	for (configuration_index = 0;
	     configuration_index < drv_usb_device_configuration_count(device);
	     configuration_index++) {
		configuration = drv_usb_device_configuration(
			device, configuration_index);

		/* Process each remaining element. */
		for (interface_index = 0;
		     interface_index <
		     drv_usb_configuration_interface_count(configuration);
		     interface_index++) {
			/* Checks the drv usb configuration interface result. */
			if (drv_usb_configuration_interface(configuration,
							    interface_index) ==
			    interface) {
				/* Returns the computed result. */
				return configuration;
			}
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Asks whether an association covers a given interface. */
static int
ncm_iad_covers(
	const struct drv_usb_interface_association_descriptor *iad,
	unsigned interface_number)
{
	/* Returns the computed result. */
	return iad->interface_count != 0U &&
	       interface_number >= iad->first_interface &&
	       interface_number - iad->first_interface < iad->interface_count;
}

/* Refuses an association that does not describe this function. */
static int
ncm_iad_consistent(
	struct drv_usb_configuration *configuration,
	unsigned control_number,
	unsigned data_number)
{
	const struct drv_usb_interface_association_descriptor *iad;
	unsigned index;
	int association = 0;

	/* Process each remaining element. */
	for (index = 0; index < drv_usb_configuration_iad_count(configuration);
	     index++) {
		/* Handles the iad availability. */
		iad = drv_usb_configuration_iad(configuration, index);
		if (iad == NULL)
			return 0;

		/* Checks the ncm iad covers result. */
		if (!ncm_iad_covers(iad, control_number) &&
		    !ncm_iad_covers(iad, data_number) &&
		    iad->first_interface != control_number &&
		    iad->first_interface != data_number)
			continue;

		/* Handles the association condition. */
		if (association || iad->first_interface != control_number ||
		    iad->interface_count != 2U ||
		    iad->function_class != NCM_COMMUNICATION_CLASS ||
		    iad->function_subclass != NCM_COMMUNICATION_SUBCLASS ||
		    iad->function_protocol != NCM_COMMUNICATION_PROTOCOL ||
		    data_number != control_number + 1U) {
			/* Succeeded. */
			return 0;
		}
		association = 1;
	}

	/* Reports operation failure. */
	return 1;
}

/* Finds the class descriptors the control interface carries. */
static int
ncm_control_descriptors(
	const struct drv_usb_host_interface *alternate,
	unsigned control_number,
	unsigned *data_number,
	uint8_t *mac_string,
	uint8_t *capabilities,
	uint16_t *max_segment_size)
{
	const uint8_t *descriptor;
	size_t length;
	unsigned index, header = 0, union_descriptor = 0, ethernet = 0, ncm = 0;

	/* Process each remaining element. */
	for (index = 0; index < drv_usb_host_interface_extra_count(alternate);
	     index++) {
		/* Checks the drv usb host interface extra result. */
		if (drv_usb_host_interface_extra(alternate, index,
						 (const void **)&descriptor,
						 &length) != 0 ||
		    descriptor == NULL || length < 2U) {
			/* Succeeded. */
			return 0;
		}

		/* Checks the file descriptor. */
		if (descriptor[1] != NCM_CS_INTERFACE)
			continue;

		/* Checks the current data length. */
		if (length < 3U || descriptor[0] != length)
			return 0;
		/* Dispatch the selected operation case. */
		switch (descriptor[2]) {
		case NCM_HEADER_DESCRIPTOR:
			/* Checks the ncm le16 result. */
			if (length != 5U || ++header != 1U ||
			    ncm_le16(descriptor + 3U) == 0) {
				/* Succeeded. */
				return 0;
			}
			break;
		case NCM_UNION_DESCRIPTOR:
			/* Handles the header condition. */
			if (header != 1U || length != 5U ||
			    ++union_descriptor != 1U ||
			    descriptor[3] != control_number ||
			    descriptor[4] == control_number) {
				/* Succeeded. */
				return 0;
			}
			*data_number = descriptor[4];
			break;
		case NCM_ETHERNET_DESCRIPTOR:
			/* Checks the ncm le16 result. */
			if (header != 1U || length != 13U || ++ethernet != 1U ||
			    descriptor[3] == 0 ||
			    ncm_le16(descriptor + 8U) <
				    DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE) {
				/* Succeeded. */
				return 0;
			}
			*mac_string = descriptor[3];
			*max_segment_size = ncm_le16(descriptor + 8U);
			break;
		case NCM_FUNCTIONAL_DESCRIPTOR:
			/* Checks the ncm le16 result. */
			if (header != 1U || length != 6U || ++ncm != 1U ||
			    ncm_le16(descriptor + 3U) != 0x0100U ||
			    (descriptor[5] & 0xc0U) != 0) {
				/* Succeeded. */
				return 0;
			}
			*capabilities = descriptor[5];
			break;
		default:
			break;
		}
	}

	/* Returns the computed result. */
	return header == 1U && union_descriptor == 1U && ethernet == 1U &&
	       ncm == 1U;
}

/* Finds the interrupt endpoint notifications arrive on. */
static int
ncm_find_notification(
	const struct drv_usb_host_interface *alternate,
	struct drv_usb_endpoint **result)
{
	struct drv_usb_endpoint *endpoint;
	unsigned index;
	struct drv_usb_endpoint *notification = NULL;

	/* Process each remaining element. */
	for (index = 0;
	     index < drv_usb_host_interface_endpoint_count(alternate);
	     index++) {
		/* Checks the drv usb endpoint type result. */
		endpoint = drv_usb_host_interface_endpoint(alternate, index);
		if (drv_usb_endpoint_type(endpoint) !=
			    DRV_USB_TRANSFER_INTERRUPT ||
		    !drv_usb_endpoint_is_input(endpoint) ||
		    notification != NULL) {
			/* Succeeded. */
			return 0;
		}
		notification = endpoint;
	}

	/* Handles the notification availability. */
	if (notification == NULL)
		return 0;
	*result = notification;
	/* Reports operation failure. */
	return 1;
}

/* Finds the data setting that actually carries packets. */
static int
ncm_find_data_alternate(
	struct drv_usb_interface *data,
	struct drv_usb_endpoint **bulk_in,
	struct drv_usb_endpoint **bulk_out,
	unsigned *alternate_result)
{
	struct drv_usb_endpoint *endpoint;
	const struct drv_usb_host_interface *alternate;
	const struct drv_usb_interface_descriptor *descriptor;
	struct drv_usb_endpoint *input, *output;
	unsigned endpoint_index;
	unsigned index;
	int empty_found = 0, bulk_found = 0;

	/* Process each remaining element. */
	for (index = 0; index < drv_usb_interface_alternate_count(data);
	     index++) {
		alternate = drv_usb_interface_alternate(data, index);
		descriptor = drv_usb_host_interface_descriptor(alternate);
		input = NULL;
		output = NULL;

		/* Handles the descriptor availability. */
		if (descriptor == NULL ||
		    descriptor->interface_class != NCM_DATA_CLASS ||
		    descriptor->interface_subclass != NCM_DATA_SUBCLASS ||
		    descriptor->interface_protocol != NCM_DATA_PROTOCOL) {
			/* Succeeded. */
			return 0;
		}

		/* Checks the file descriptor. */
		if (descriptor->alternate_setting == 0U) {
			/* Checks the file descriptor. */
			if (descriptor->endpoint_count != 0U || empty_found)
				return 0;
			empty_found = 1;
			continue;
		}

		/* Checks the file descriptor. */
		if (descriptor->endpoint_count != 2U)
			continue;
		/* Process each remaining element. */
		for (endpoint_index = 0; endpoint_index < 2U;
		     endpoint_index++) {
			/* Checks the drv usb endpoint type result. */
			endpoint = drv_usb_host_interface_endpoint(
				alternate, endpoint_index);
			if (drv_usb_endpoint_type(endpoint) !=
			    DRV_USB_TRANSFER_BULK)
				break;

			/* Handles the drv usb endpoint is input condition. */
			if (drv_usb_endpoint_is_input(endpoint)) {
				/* Handles the input availability. */
				if (input != NULL)
					break;
				input = endpoint;
			} else {
				/* Handles the output availability. */
				if (output != NULL)
					break;
				output = endpoint;
			}
		}

		/* Handles the input availability. */
		if (endpoint_index != 2U || input == NULL || output == NULL)
			continue;

		/* Handles the bulk found condition. */
		if (bulk_found)
			return 0;
		bulk_found = 1;
		*bulk_in = input;
		*bulk_out = output;
		*alternate_result = descriptor->alternate_setting;
	}

	/* Returns the computed result. */
	return empty_found && bulk_found;
}

/* Reads everything this driver needs out of the interface. */
static int
ncm_binding_parse(
	struct drv_usb_interface *control,
	struct ncm_binding *binding)
{
	struct drv_usb_configuration *configuration;
	const struct drv_usb_interface_descriptor *control_descriptor;
	const struct drv_usb_host_interface *control_alternate;
	unsigned control_number, data_number = UINT_MAX;
	uint8_t mac_string = 0;

	kern_memset(binding, 0, sizeof(*binding));

	/* Checks the drv usb interface alternate count result. */
	control_descriptor = drv_usb_interface_descriptor(control);
	if (control_descriptor == NULL ||
	    control_descriptor->interface_class != NCM_COMMUNICATION_CLASS ||
	    control_descriptor->interface_subclass !=
		    NCM_COMMUNICATION_SUBCLASS ||
	    control_descriptor->interface_protocol !=
		    NCM_COMMUNICATION_PROTOCOL ||
	    drv_usb_interface_alternate_count(control) != 1U) {
		/* Succeeded. */
		return 0;
	}

	/* Checks the drv usb device hcd capabilities result. */
	if ((drv_usb_device_hcd_capabilities(
		     drv_usb_interface_device(control)) &
	     DRV_USB_HCD_CAP_CONCURRENT_URBS) == 0) {
		/* Succeeded. */
		return 0;
	}

	/* Handles the configuration availability. */
	configuration = ncm_interface_configuration(control);
	if (configuration == NULL)
		return 0;
	control_alternate = drv_usb_interface_alternate(control, 0);

	/* Checks the ncm control descriptors result. */
	control_number = control_descriptor->interface_number;
	if (control_alternate == NULL ||
	    !ncm_control_descriptors(control_alternate, control_number,
				     &data_number, &mac_string,
				     &binding->capabilities,
				     &binding->max_segment_size) ||
	    !ncm_find_notification(control_alternate, &binding->notification)) {
		/* Succeeded. */
		return 0;
	}
	binding->data = drv_usb_configuration_find_interface(configuration,
							     data_number);

	/* Checks the ncm iad consistent result. */
	if (binding->data == NULL ||
	    !ncm_iad_consistent(configuration, control_number, data_number) ||
	    !ncm_find_data_alternate(binding->data, &binding->bulk_in,
				     &binding->bulk_out,
				     &binding->data_alternate)) {
		/* Succeeded. */
		return 0;
	}
	binding->device = drv_usb_interface_device(control);
	binding->control = control;
	binding->mac_string = mac_string;

	/* Reports operation failure. */
	return 1;
}

/* Renders one hexadecimal character as its value. */
static int
ncm_hex(
	unsigned char character)
{
	/* Classifies the current input character. */
	if (character >= '0' && character <= '9')
		return character - '0';

	/* Classifies the current input character. */
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;

	/* Classifies the current input character. */
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Reports operation failure. */
	return -1;
}

/* Reads the hardware address out of a string descriptor. */
static int
ncm_get_mac(
	const struct ncm_binding *binding,
	uint8_t mac[6])
{
	int high;
	int low;
	char string[13];
	unsigned index;
	int all_zero = 1;

	/* Checks the drv usb device get string result. */
	if (drv_usb_device_get_string(binding->device, binding->mac_string, 0,
				      string, sizeof(string)) != 0 ||
	    kern_strlen(string) != 12U) {
		/* Failed. */
		return EINVAL;
	}
	/* Process each remaining element. */
	for (index = 0; index < 6U; index++) {
		high = ncm_hex((unsigned char)string[index * 2U]);

		/* Handles the high condition. */
		low = ncm_hex((unsigned char)string[index * 2U + 1U]);
		if (high < 0 || low < 0)
			return EINVAL;
		mac[index] = (uint8_t)((high << 4) | low);

		/* Handles the mac condition. */
		if (mac[index] != 0)
			all_zero = 0;
	}

	/* Handles the all zero condition. */
	if (all_zero || (mac[0] & 1U) != 0)
		return EINVAL;

	/* Succeeded. */
	return 0;
}

/* Runs one class control request against the device. */
static int
ncm_control(
	struct ncm_adapter *adapter,
	uint8_t request_type,
	uint8_t request,
	uint16_t value,
	void *buffer,
	size_t length,
	size_t *actual)
{
	int error;

	/* Obtains the drv usb control result. */
	error = drv_usb_control(
		adapter->usb_device, request_type, request, value,
		(uint16_t)drv_usb_interface_number(adapter->control), buffer,
		length, NCM_CONTROL_TIMEOUT_MS, actual);

	/* Returns the computed result. */
	return error;
}

/* Tells the device the block sizes this driver will use. */
static int
ncm_program_profile(
	struct ncm_adapter *adapter)
{
	struct drv_usb_cdc_ncm_control_request request;
	size_t actual;
	int error;
	static const enum drv_usb_cdc_ncm_control_step steps[] = {
		DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16,
		DRV_USB_CDC_NCM_CONTROL_SET_INPUT_SIZE,
		DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE,
		DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC};
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(steps) / sizeof(steps[0]); index++) {
		actual = 0;

		/* The maximum datagram, for a device that takes it, by its own steps. */
		if (steps[index] ==
		    DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE) {
			if ((adapter->capabilities & NCM_CAP_MAX_DATAGRAM_SIZE) == 0)
				continue;
			error = ncm_program_max_datagram(adapter);
			if (error != 0)
				return error;
			continue;
		}

		/* Handles the steps condition. */
		if (steps[index] == DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC &&
		    (adapter->capabilities & NCM_CAP_CRC_MODE) == 0)
			continue;

		/* Checks the operation status. */
		error = drv_usb_cdc_ncm_make_control_request(
			&adapter->profile, steps[index], &request);
		if (error == EOPNOTSUPP &&
		    steps[index] == DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16)
			continue;
		if (error != 0)
			return error;

		/* Checks the operation status. */
		error = ncm_control(adapter,
				    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
					    DRV_USB_RECIP_INTERFACE,
				    request.request, request.value,
				    request.payload, request.length, &actual);
		if (error != 0 || actual != request.length)
			return error != 0 ? error : EIO;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Sets the device's maximum datagram, from the longest the profile's
 * blocks take down: that one, then the descriptor's wMaxSegmentSize, then
 * the standard 1514.  The device's answer to GET_MAX_DATAGRAM_SIZE after
 * each set is the size it took, and becomes the profile's when smaller.
 * One beyond wMaxSegmentSize is kept only when the device reads it back;
 * any size the device refuses (a STALL) passes to the next.
 *
 * Procedure referenced: USB CDC NCM 1.0, 6.2.9 SetMaxDatagramSize and
 * 6.2.8 GetMaxDatagramSize.
 */
static int
ncm_program_max_datagram(
	struct ncm_adapter *adapter)
{
	uint16_t sizes[3];
	uint16_t read_back;
	uint16_t bounded;
	unsigned count;
	unsigned index;
	int error;

	/* The sizes to try, longest first, each once. */
	bounded = adapter->max_segment_size;
	if (bounded < DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE)
		bounded = DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE;
	count = 0;
	sizes[count++] = adapter->profile.max_datagram_size;
	if (bounded < sizes[count - 1U])
		sizes[count++] = bounded;
	if (DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE < sizes[count - 1U])
		sizes[count++] = DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE;

	/* Takes the first the device accepts. */
	error = EIO;
	for (index = 0; index < count; index++) {
		read_back = 0;
		error = ncm_set_max_datagram(adapter, sizes[index], &read_back);
		if (error != 0)
			continue;

		/* A size the device did not confirm is kept only within its descriptor. */
		if (read_back == 0 && sizes[index] > bounded) {
			error = EIO;
			continue;
		}

		/* The size the device took. */
		adapter->max_datagram_read_back = read_back;
		adapter->profile.max_datagram_size = sizes[index];
		if (read_back != 0 && read_back < sizes[index])
			adapter->profile.max_datagram_size = read_back;
		return 0;
	}

	/* Failed: the device refused even the standard size. */
	return error;
}

/*
 * Sends SET_MAX_DATAGRAM_SIZE with size and reads GET_MAX_DATAGRAM_SIZE
 * back into *read_back: the device's value when it answered with one of
 * at least the standard size, else 0.  Fails only when the set failed.
 */
static int
ncm_set_max_datagram(
	struct ncm_adapter *adapter,
	uint16_t size,
	uint16_t *read_back)
{
	struct drv_usb_cdc_ncm_control_request request;
	struct drv_usb_cdc_ncm_profile profile;
	uint8_t answer[2];
	size_t actual;
	uint16_t value;
	int error;

	/* The request for this size. */
	profile = adapter->profile;
	profile.max_datagram_size = size;
	error = drv_usb_cdc_ncm_make_control_request(
		&profile, DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE, &request);
	if (error != 0)
		return error;

	/* Sets it. */
	actual = 0;
	error = ncm_control(adapter,
			    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
				    DRV_USB_RECIP_INTERFACE,
			    request.request, request.value,
			    request.payload, request.length, &actual);
	if (error != 0)
		return error;
	if (actual != request.length)
		return EIO;

	/* Reads back what the device took; a device that does not answer leaves 0. */
	*read_back = 0;
	actual = 0;
	error = ncm_control(adapter,
			    DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS |
				    DRV_USB_RECIP_INTERFACE,
			    DRV_USB_CDC_NCM_GET_MAX_DATAGRAM_SIZE, 0,
			    answer, sizeof(answer), &actual);
	if (error == 0 && actual == sizeof(answer)) {
		value = ncm_le16(answer);
		if (value >= DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE)
			*read_back = value;
	}

	/* Succeeded: the set was taken. */
	return 0;
}

/* Tells the device which packets to pass up. */
static int
ncm_program_packet_filter(
	struct ncm_adapter *adapter)
{
	size_t actual = 0;
	unsigned long irq;
	int error;

	/* Handles the adapter condition. */
	if ((adapter->capabilities & NCM_CAP_PACKET_FILTER) == 0)
		return 0;

	/* Handles the adapter condition. */
	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Failed. */
		return ENETDOWN;
	}

	adapter->starts_active++;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Checks the operation status. */
	error = ncm_control(adapter,
			    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
				    DRV_USB_RECIP_INTERFACE,
			    NCM_SET_ETHERNET_PACKET_FILTER,
			    NCM_FILTER_DIRECTED | NCM_FILTER_ALL_MULTICAST |
				    NCM_FILTER_BROADCAST,
			    NULL, 0, &actual);
	if (error != 0 || actual != 0)
		error = error != 0 ? error : EIO;
	irq = spin_lock_irqsave(&adapter->lock);

	/* Handles the adapter condition. */
	if (adapter->starts_active == 0)
		__builtin_trap();

	/* Checks the operation status. */
	if (error == 0 && (adapter->closing || !adapter->opened))
		error = ENETDOWN;
	adapter->starts_active--;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Renders a transfer status as the error it stands for. */
static int
ncm_urb_status_error(
	enum drv_usb_urb_status status)
{
	/* Checks the operation status. */
	if (status == DRV_USB_URB_COMPLETE)
		return 0;

	/* Checks the operation status. */
	if (status == DRV_USB_URB_TIMEOUT)
		return ETIMEDOUT;

	/* Checks the operation status. */
	if (status == DRV_USB_URB_STALL)
		return EPIPE;

	/* Checks the operation status. */
	if (status == DRV_USB_URB_DISCONNECTED)
		return ENODEV;

	/* Failed. */
	return EIO;
}

/* Asks whether a transmit status is one to report. */
static int
ncm_tx_status_is_error(
	enum drv_usb_urb_status status)
{
	/* Returns the computed result. */
	return status == DRV_USB_URB_STALL || status == DRV_USB_URB_TIMEOUT ||
	       status == DRV_USB_URB_DISCONNECTED ||
	       status == DRV_USB_URB_IO_ERROR;
}

/* Takes one finished transfer. */
static void
ncm_completion(
	struct drv_usb_urb *urb,
	void *argument)
{
	struct ncm_adapter *adapter = argument;
	enum drv_usb_urb_status status;
	unsigned long irq;
	unsigned index;
	int tx_error = 0;

	/* The transfer's end, read before the lock. */
	status = drv_usb_urb_status(urb);

	/* Marks the transfer that ended for the poll to take. */
	irq = spin_lock_irqsave(&adapter->lock);

	if (urb == adapter->notification_urb)
		adapter->notification_ready = 1;

	/* A receive transfer. */
	for (index = 0; index < adapter->rx_urb_count; index++) {
		if (urb == adapter->rx_urbs[index])
			adapter->rx_done |= 1U << index;
	}

	/* A transmit transfer, and whether it failed. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		if (urb != adapter->tx_urbs[index])
			continue;
		adapter->tx_done |= 1U << index;
		tx_error = ncm_tx_status_is_error(status);
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Checks the operation status. */
	if (tx_error)
		net_device_tx_error(adapter->net_device);
	net_device_schedule_poll(adapter->net_device);
}

/* Puts one transfer back on its endpoint. */
static int
ncm_start_urb(
	struct ncm_adapter *adapter,
	struct drv_usb_urb *urb,
	void *buffer,
	size_t length)
{
	unsigned long irq;
	int error;

	/* Handles the adapter condition. */
	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Failed. */
		return ENETDOWN;
	}

	adapter->starts_active++;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Checks the operation status. */
	error = drv_usb_urb_setup(urb, buffer, length, 0, 0, ncm_completion,
				  adapter);
	if (error == 0)
		error = drv_usb_urb_submit(urb);
	irq = spin_lock_irqsave(&adapter->lock);

	/* Handles the adapter condition. */
	if (adapter->starts_active == 0)
		__builtin_trap();
	adapter->starts_active--;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Cancels a transfer and waits for it to leave. */
static int
ncm_cancel_and_drain(
	struct drv_usb_urb *urb)
{
	int error;
	enum drv_usb_urb_status status;

	/* Handles the urb availability. */
	if (urb == NULL)
		return 0;

	/* Checks the operation status. */
	status = drv_usb_urb_status(urb);
	if (status == DRV_USB_URB_PENDING)
		(void)drv_usb_urb_cancel(urb);

	/* Obtains the drv usb urb drain result. */
	error = drv_usb_urb_drain(urb, NCM_TRANSFER_TIMEOUT_MS);

	/* Returns the computed result. */
	return error;
}

/* Waits for everything this device has in flight to finish. */
static void
ncm_wait_activity(
	struct ncm_adapter *adapter)
{
	unsigned long irq;
	unsigned active;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		irq = spin_lock_irqsave(&adapter->lock);
		active = adapter->starts_active + adapter->polls_active;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Handles the active condition. */
		if (active == 0)
			return;
		sched_yield();
	}
}

/* Gives back the packets that were waiting to be delivered. */
static void
ncm_free_rx_queue(
	struct ncm_adapter *adapter)
{
	struct packet_buf *packets[NCM_RX_QUEUE_MAX];
	unsigned long irq;
	size_t count = 0;

	irq = spin_lock_irqsave(&adapter->lock);

	/* Process each remaining element. */
	while (adapter->rx_queue_count != 0) {
		packets[count++] = adapter->rx_queue[adapter->rx_queue_head];
		adapter->rx_queue[adapter->rx_queue_head] = NULL;
		adapter->rx_queue_head =
			(adapter->rx_queue_head + 1U) % NCM_RX_QUEUE_MAX;
		adapter->rx_queue_count--;
	}

	adapter->rx_queue_head = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Process each remaining element. */
	while (count != 0)
		packet_buf_free(packets[--count]);
}

/* Stops the interface carrying traffic. */
static int
ncm_stop(
	struct ncm_adapter *adapter)
{
	unsigned long irq;
	unsigned index;
	int error = 0, candidate;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles the adapter condition. */
		irq = spin_lock_irqsave(&adapter->lock);
		if (!adapter->stopping)
			break;
		spin_unlock_irqrestore(&adapter->lock, irq);
		sched_yield();
	}

	adapter->stopping = 1;
	adapter->closing = 1;
	adapter->opened = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/*
	 * A poll admitted before closing was published may still parse an RX
	 * buffer or attempt a rearm.  Join that complete worker section before
	 * cancelling URBs or changing the data-interface alternate.
	 */
	ncm_wait_activity(adapter);

	/* Checks the operation status. */
	candidate = ncm_cancel_and_drain(adapter->notification_urb);
	if (error == 0)
		error = candidate;

	/*
	 * The receive transfers, then the transmit ones.  Where several are
	 * queued on an endpoint, the first cancel ends them all and the others
	 * are only waited for.
	 */
	for (index = 0; index < adapter->rx_urb_count; index++) {
		candidate = ncm_cancel_and_drain(adapter->rx_urbs[index]);
		if (error == 0)
			error = candidate;
	}

	/* The transmit ones. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		candidate = ncm_cancel_and_drain(adapter->tx_urbs[index]);
		if (error == 0)
			error = candidate;
	}
	if (error == 0)
		ncm_free_rx_queue(adapter);
	irq = spin_lock_irqsave(&adapter->lock);

	/* Every transfer is off the bus: all receive ones wait for a rearm, all transmit ones are free. */
	if (error == 0) {
		adapter->notification_ready = 0;
		adapter->rx_done = 0;
		adapter->rx_order_head = 0;
		adapter->rx_order_count = 0;
		adapter->rx_idle = (1U << adapter->rx_urb_count) - 1U;
		adapter->tx_done = 0;
		adapter->tx_free = (1U << adapter->tx_urb_count) - 1U;
		adapter->notification_rearm = 0;
		adapter->rx_rearm = 0;
		adapter->notification_rearm_active = 0;
		adapter->rx_rearm_active = 0;
		adapter->notification_retries = 0;
		adapter->rx_retries = 0;
		adapter->tx_busy = 0;
	} else {
		adapter->quarantined = 1;
	}

	adapter->stop_error = error;
	adapter->closing = 0;
	adapter->stopping = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Drops the frames still waiting; no transfer refers to them. */
	ncm_tx_queue_free(adapter);

	/*
	 * The carrier is the device's last word on its link and stays as it
	 * was across an administrative close: the device tells a link only
	 * when it changes (its NetworkConnection notification), so a carrier
	 * cleared here was never set again when the interface came back up
	 * with the cable still in, and Settings said "No cable" over a
	 * working link (BUG-277: a cable that went and came back before
	 * networkd took the interface down and up).  A cable pulled while the
	 * interface is down is told by the notification the device holds for
	 * the next open.  A stop that failed leaves the device quarantined,
	 * without a link.
	 */
	if (error != 0 && adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Brings the interface up and starts its transfers. */
static int
ncm_open(
	struct net_device *device)
{
	struct ncm_adapter *adapter = device->driver_data;
	unsigned long irq;
	unsigned rearm;
	int error;

	irq = spin_lock_irqsave(&adapter->lock);

	/* Handles the adapter condition. */
	if (!adapter->ready || adapter->opened || adapter->closing ||
	    adapter->stopping || adapter->quarantined) {
		error = !adapter->ready ? ENETDOWN
					: (adapter->quarantined ? EIO : EBUSY);
		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Failed. */
		return error;
	}

	adapter->opened = 1;
	adapter->stop_error = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/*
	 * The data alternate is active by the time the net device becomes
	 * ready. Some functions discard their filter across an administrative
	 * close, so program it on every open and before publishing any
	 * persistent URB.
	 */

	/* Checks the operation status. */
	error = ncm_program_packet_filter(adapter);
	if (error == 0) {
		drv_usb_cdc_notification_reset(&adapter->notification_reader);
		error = ncm_start_urb(adapter, adapter->notification_urb,
				      adapter->notification_buffer,
				      adapter->notification_length);
	}
	if (error == 0) {
		/* Holds the right to rearm while every receive transfer goes on. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->rx_rearm_active = 1;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Puts every receive transfer on the bus. */
		error = ncm_rx_start_idle(adapter);

		/* Gives the right up; a poll that asked for a rearm meanwhile is run again. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->rx_rearm_active = 0;
		rearm = adapter->rx_rearm;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Runs the poll for a rearm asked for while open held the right. */
		if (error == 0 && rearm)
			net_device_schedule_poll(device);
	}
	if (error != 0) {
		(void)ncm_stop(adapter);

		/* Failed. */
		return error;
	}

	/* Succeeded. */
	return 0;
}

/* Takes the interface down. */
static void
ncm_close(
	struct net_device *device)
{
	struct ncm_adapter *adapter = device->driver_data;

	(void)ncm_stop(adapter);
}

/*
 * Queues one packet for the device and, when no thread is sending and a
 * transmit transfer is free, sends the queue, wrapped in the block format
 * the device wants.
 */
static int
ncm_transmit(
	struct net_device *device,
	struct packet_buf *packet)
{
	struct ncm_adapter *adapter = device->driver_data;
	unsigned long irq;
	int pump;

	/* Handles the packet availability. */
	if (packet == NULL)
		return EINVAL;

	/* Handles the adapter condition. */
	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);

		/* Failed. */
		return ENETDOWN;
	}

	/* Only a full queue drops the frame; TCP keeps it and hands it over again. */
	if (adapter->tx_queue_count >= NCM_TX_QUEUE_MAX) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);

		/* Failed. */
		return ENOBUFS;
	}

	/* Queues the frame behind those waiting, so the frames go out in order. */
	packet->next = NULL;
	if (adapter->tx_queue_tail != NULL)
		adapter->tx_queue_tail->next = packet;
	else
		adapter->tx_queue_head = packet;
	adapter->tx_queue_tail = packet;
	adapter->tx_queue_count++;

	/* Becomes the sender when none sends and a transfer is free; starts_active holds off teardown. */
	pump = 0;
	if (!adapter->tx_busy && adapter->tx_free != 0) {
		adapter->tx_busy = 1;
		adapter->starts_active++;
		pump = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Sends the queue. */
	if (pump)
		ncm_tx_pump(adapter);

	/* Succeeded: the frame is queued or sent. */
	return 0;
}

/*
 * Sends the queued frames, a block a free transmit transfer at a time,
 * until the queue is empty, no transfer is free, or the adapter goes down.
 *
 * The caller has set tx_busy and counted itself in starts_active under the
 * adapter lock; this gives both up in the same section that finds nothing
 * more to send, so a transmit completion that frees a transfer afterwards
 * finds tx_busy clear and sends itself.  The frames of a block whose
 * submit fails are dropped.
 */
static void
ncm_tx_pump(
	struct ncm_adapter *adapter)
{
	struct packet_buf *packets;
	struct packet_buf *last;
	unsigned long irq;
	unsigned index;
	unsigned taken;
	int error;

	/* Sends block after block. */
	for (;;) {
		irq = spin_lock_irqsave(&adapter->lock);

		/* Stops when going down, with nothing queued, or with no transfer free. */
		if (adapter->closing || !adapter->opened ||
		    adapter->tx_queue_head == NULL || adapter->tx_free == 0) {
			if (adapter->starts_active == 0)
				__builtin_trap();
			adapter->tx_busy = 0;
			adapter->starts_active--;
			spin_unlock_irqrestore(&adapter->lock, irq);
			break;
		}

		/* Takes a free transfer. */
		index = ncm_lowest_bit(adapter->tx_free);
		adapter->tx_free &= ~(1U << index);

		/* Takes the waiting frames one block carries off the queue as one list. */
		packets = adapter->tx_queue_head;
		last = packets;
		for (taken = 1; taken < NCM_TX_BATCH_MAX && last->next != NULL; taken++)
			last = last->next;
		adapter->tx_queue_head = last->next;
		if (adapter->tx_queue_head == NULL)
			adapter->tx_queue_tail = NULL;
		adapter->tx_queue_count -= taken;
		last->next = NULL;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Builds and submits the block; the frames it does not carry go back to the head of the queue. */
		error = ncm_tx_submit(adapter, index, packets);
		if (error == 0)
			continue;

		/* A transfer that did not go keeps nothing: it is free again and its frames are dropped. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->tx_free |= 1U << index;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Counts the frames lost with it. */
		if (adapter->net_device != NULL)
			adapter->net_device->tx_dropped++;
	}
}

/*
 * Wraps frames in one transfer block and submits it on transmit transfer
 * index.
 *
 * packets is a list of frames in order, linked through their next field.
 * The block carries as many of them as fit it; those it does not carry go
 * back to the head of the queue, in order, for the next transfer.  The
 * caller is the pump and has taken the transfer off tx_free.  The frames
 * carried are consumed, on a failure too.
 */
static int
ncm_tx_submit(
	struct ncm_adapter *adapter,
	unsigned index,
	struct packet_buf *packets)
{
	struct packet_buf *list[NCM_TX_BATCH_MAX];
	const void *frames[NCM_TX_BATCH_MAX];
	size_t lengths[NCM_TX_BATCH_MAX];
	struct packet_buf *rest;
	struct packet_buf *tail;
	unsigned long irq;
	size_t ntb_length = 0;
	size_t count;
	size_t taken;
	size_t frame;
	unsigned left;
	uint16_t sequence;
	int error;

	/* Collects the frames in order, up to a block's datagrams; the list past them stays linked. */
	count = 0;
	rest = packets;
	while (rest != NULL && count < NCM_TX_BATCH_MAX) {
		list[count] = rest;
		frames[count] = rest->data;
		lengths[count] = rest->length;
		count++;
		rest = rest->next;
	}

	/* Samples the sequence number of the block; the transfer never refers to the packets. */
	irq = spin_lock_irqsave(&adapter->lock);

	sequence = adapter->tx_sequence;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Builds a block of as many frames as it takes; a failure drops them all. */
	taken = 0;
	error = drv_usb_cdc_ncm_build_ntb16_frames(
		&adapter->profile, sequence, frames, lengths, count,
		adapter->tx_buffers[index], adapter->profile.ntb_out_max_size,
		&ntb_length, &taken);
	if (error != 0)
		taken = count;

	/* The frames the block does not carry, with the rest of the list, go back to the head of the queue. */
	if (taken < count) {
		list[count - 1U]->next = rest;
		rest = list[taken];
	}

	/* Counts the frames that go back and finds the last of them. */
	left = 0;
	tail = NULL;
	for (packets = rest; packets != NULL; packets = packets->next) {
		tail = packets;
		left++;
	}

	/* Refuses a block for an adapter going down, and puts the frames not carried back. */
	irq = spin_lock_irqsave(&adapter->lock);

	/* A block built for an adapter that is closing is not sent. */
	if (error == 0 && (adapter->closing || !adapter->opened))
		error = ENETDOWN;

	/* The frames not carried are the next transfer's first. */
	if (rest != NULL) {
		tail->next = adapter->tx_queue_head;
		if (adapter->tx_queue_head == NULL)
			adapter->tx_queue_tail = tail;
		adapter->tx_queue_head = rest;
		adapter->tx_queue_count += left;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Frees the frames carried (or, on a failure, those that would have been). */
	for (frame = 0; frame < taken; frame++) {
		list[frame]->next = NULL;
		packet_buf_free(list[frame]);
	}

	/* Submits the block. */
	if (error == 0) {
		error = drv_usb_urb_setup(
			adapter->tx_urbs[index], adapter->tx_buffers[index],
			ntb_length, 0, NCM_TRANSFER_TIMEOUT_MS, ncm_completion,
			adapter);
	}
	if (error == 0)
		error = drv_usb_urb_submit(adapter->tx_urbs[index]);

	/* The next block takes the next sequence number. */
	if (error == 0) {
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->tx_sequence++;

		spin_unlock_irqrestore(&adapter->lock, irq);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Frees every frame still waiting for the transmit transfer. */
static void
ncm_tx_queue_free(
	struct ncm_adapter *adapter)
{
	struct packet_buf *packets;
	struct packet_buf *packet;
	unsigned long irq;

	/* Takes the whole list under the lock. */
	irq = spin_lock_irqsave(&adapter->lock);

	packets = adapter->tx_queue_head;
	adapter->tx_queue_head = NULL;
	adapter->tx_queue_tail = NULL;
	adapter->tx_queue_count = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Frees it outside the lock. */
	for (;;) {
		packet = packets;
		if (packet == NULL)
			break;
		packets = packet->next;
		packet_buf_free(packet);
	}
}

/* Puts one received packet on the delivery queue. */
static int
ncm_queue_datagram(
	const void *frame,
	size_t length,
	void *argument)
{
	struct ncm_adapter *adapter = argument;
	struct packet_buf *packet;
	void *destination;
	unsigned long irq;
	int error;

	/* Checks the current data length. */
	if (length < DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE ||
	    length > adapter->profile.max_datagram_size) {
		/* Failed. */
		return EINVAL;
	}

	/*
	 * A packet of the class the datagram needs (a driver's receive may
	 * take the reserved jumbo ones).  Without one this datagram is
	 * dropped and the block's others still delivered.
	 */
	packet = packet_buf_alloc_for(0, length, PACKET_BUF_ALLOC_RECEIVE, &error);
	if (packet == NULL) {
		adapter->net_device->rx_dropped++;
		return 0;
	}

	/* Handles the destination availability. */
	destination = packet_buf_append(packet, length);
	if (destination == NULL) {
		packet_buf_free(packet);

		/* Failed. */
		return EMSGSIZE;
	}

	kern_memcpy(destination, frame, length);

	/* Handles the adapter condition. */
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->rx_queue_count >= NCM_RX_QUEUE_MAX) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);

		/* The queue is full: this datagram is dropped, the block's others tried. */
		adapter->net_device->rx_dropped++;
		return 0;
	}

	adapter->rx_queue[(adapter->rx_queue_head + adapter->rx_queue_count) %
			  NCM_RX_QUEUE_MAX] = packet;
	adapter->rx_queue_count++;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Succeeded. */
	return 0;
}

/*
 * Takes the notifications the device has sent.
 *
 * The transfer's bytes join what the reader kept of an earlier one, and
 * each whole notification is taken in turn (BUG-222: a speed's
 * notification may come split over two transfers, or behind another).
 */
static void
ncm_notification_process(
	struct ncm_adapter *adapter)
{
	struct drv_usb_cdc_notification notification;
	enum drv_usb_urb_status status;
	size_t length;
	int whole;

	/* A transfer that failed leaves a gap: what was kept cannot be joined to what comes next. */
	status = drv_usb_urb_status(adapter->notification_urb);
	if (status != DRV_USB_URB_COMPLETE) {
		drv_usb_cdc_notification_reset(&adapter->notification_reader);
		return;
	}

	/* Adds the transfer's bytes to the reader. */
	length = drv_usb_urb_actual_length(adapter->notification_urb);
	drv_usb_cdc_notification_add(&adapter->notification_reader, adapter->notification_buffer, length);

	/* Takes every whole notification the reader holds. */
	for (;;) {
		whole = drv_usb_cdc_notification_next(&adapter->notification_reader, &notification);
		if (!whole)
			break;
		ncm_notification_take(adapter, &notification);
	}
}

/* Acts on one notification: the link's coming and going, and its speed. */
static void
ncm_notification_take(
	struct ncm_adapter *adapter,
	const struct drv_usb_cdc_notification *notification)
{
	unsigned long irq;
	uint32_t fastest;
	uint32_t mbps;
	unsigned control_number;
	unsigned data_number;
	int connected;

	/*
	 * Passes over a notification of another interface.  Some NCM
	 * functions, RTL8156's configuration 2 among them, name the data
	 * interface rather than the communication one; the binding has already
	 * checked this control and data pair.
	 */
	control_number = drv_usb_interface_number(adapter->control);
	data_number = drv_usb_interface_number(adapter->data);
	if (notification->interface_number != control_number && notification->interface_number != data_number)
		return;

	/* The link came or went: the carrier follows the cable. */
	if (notification->code == DRV_USB_CDC_NETWORK_CONNECTION && notification->data_length == 0U) {
		/* Whether the device says the cable is in. */
		connected = 0;
		if (notification->value != 0U)
			connected = 1;
		(void)net_device_set_carrier(adapter->net_device, connected);

		/* A link that went has no speed until the device tells the next one. */
		if (!connected && adapter->net_device != NULL)
			adapter->net_device->link_mbps = 0U;
		return;
	}

	/* Passes over anything but the speed's notification. */
	if (notification->code != DRV_USB_CDC_CONNECTION_SPEED_CHANGE || notification->data_length != 8U)
		return;

	/* Keeps both directions' speeds. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->downstream_bps = notification->downstream_bps;
	adapter->upstream_bps = notification->upstream_bps;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* The interface reports the faster direction as its link's speed, in Mb/s, which ifconfig and Settings show. */
	fastest = notification->downstream_bps;
	if (notification->upstream_bps > fastest)
		fastest = notification->upstream_bps;
	mbps = fastest / 1000000U;

	/* An adapter without its interface yet has nowhere to show it. */
	if (adapter->net_device == NULL)
		return;

	/* Logs a speed that changed, for a look at the link without the desktop. */
	if (adapter->net_device->link_mbps != mbps) {
		kern_logf("usb-cdc-ncm: %s link %u Mb/s\n",
			  adapter->net_device->name,
			  (unsigned)mbps);
	}

	/* The speed ifconfig and Settings read (SIOCGIFSTATS). */
	adapter->net_device->link_mbps = mbps;
}

/* Puts the receive and notification transfers back on. */
static int
ncm_rearm(
	struct ncm_adapter *adapter,
	int notification)
{
	int error;
	unsigned long irq;
	unsigned *retry;
	unsigned *pending;
	unsigned *active;
	int retryable;
	int retry_scheduled = 0;
	int quarantine = 0;
	int stopping;

	/* Puts the notification transfer, or every receive transfer off the bus, back on. */
	if (notification) {
		error = ncm_start_urb(adapter, adapter->notification_urb,
				      adapter->notification_buffer,
				      adapter->notification_length);
	} else {
		error = ncm_rx_start_idle(adapter);
	}

	/* Settles the rearm under the lock: its retry count, pending request and right. */
	irq = spin_lock_irqsave(&adapter->lock);
	retry = notification ? &adapter->notification_retries
				       : &adapter->rx_retries;
	pending = notification ? &adapter->notification_rearm
			       : &adapter->rx_rearm;
	active = notification ? &adapter->notification_rearm_active
			      : &adapter->rx_rearm_active;
	retryable = error == EBUSY || error == ENOMEM || error == EAGAIN ||
		    error == ENOBUFS;
	stopping = !adapter->ready || !adapter->opened ||
		   adapter->closing || adapter->stopping;

	/* Handles the active condition. */
	if (*active == 0)
		__builtin_trap();

	/* Checks the operation status. */
	if (error == 0) {
		/*
		 * The caller atomically claimed the old rearm request. Preserve
		 * a new one that an immediately completed submission may
		 * already have published through another poll.
		 */
		*retry = 0;
	} else if (stopping) {
		/*
		 * A poll admitted immediately before close may reach rearm
		 * after close has withdrawn admission.  This is orderly
		 * retirement, not a transfer failure and must not quarantine
		 * the adapter.
		 */
		*retry = 0;
		error = 0;
	} else if (retryable && *retry < NCM_REARM_RETRY_MAX) {
		(*retry)++;
		*pending = 1;
		retry_scheduled = 1;
	} else {
		*pending = 0;
		adapter->quarantined = 1;
		adapter->opened = 0;
		quarantine = 1;
	}

	*active = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Handles the net device availability. */
	if (quarantine && adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);

	/* Handles the retry scheduled condition. */
	if (retry_scheduled)
		return EAGAIN;

	/*
	 * EAGAIN is the internal retry signal.  Once its bounded budget has
	 * been exhausted, return a terminal error so the caller cannot
	 * reschedule the quarantined adapter forever.
	 */
	if (quarantine && error == EAGAIN)
		return EIO;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Puts every receive transfer that is off the bus back on, each with the
 * buffer it is to fill, and notes it in the submission order first: the
 * controller completes them in that order, and a completion may come
 * before the submit returns.  The caller holds the right to rearm.  A
 * transfer whose submit fails stays off the bus for the next rearm.
 */
static int
ncm_rx_start_idle(
	struct ncm_adapter *adapter)
{
	unsigned long irq;
	unsigned index;
	unsigned tail;
	int error;

	/* One transfer after another until none is off the bus. */
	error = 0;
	for (;;) {
		irq = spin_lock_irqsave(&adapter->lock);

		/* Nothing is off the bus. */
		if (adapter->rx_idle == 0) {
			spin_unlock_irqrestore(&adapter->lock, irq);
			break;
		}

		/* Takes one and places it last in the order. */
		index = ncm_lowest_bit(adapter->rx_idle);
		adapter->rx_idle &= ~(1U << index);
		tail = (adapter->rx_order_head + adapter->rx_order_count) % NCM_RX_URBS;
		adapter->rx_order[tail] = index;
		adapter->rx_order_count++;

		spin_unlock_irqrestore(&adapter->lock, irq);

		/* Submits it. */
		error = ncm_start_urb(adapter, adapter->rx_urbs[index],
				      adapter->rx_urb_buffers[index],
				      adapter->profile.ntb_in_max_size);
		if (error == 0)
			continue;

		/* It did not go: out of the order (it is the last) and off the bus again. */
		irq = spin_lock_irqsave(&adapter->lock);

		adapter->rx_order_count--;
		adapter->rx_idle |= 1U << index;

		spin_unlock_irqrestore(&adapter->lock, irq);
		break;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Tells whether the oldest receive transfer on the bus has completed, under the adapter lock. */
static int
ncm_rx_head_done_locked(
	const struct ncm_adapter *adapter)
{
	unsigned index;

	/* None is on the bus. */
	if (adapter->rx_order_count == 0)
		return 0;

	/* The oldest one's completion has come. */
	index = adapter->rx_order[adapter->rx_order_head];
	if ((adapter->rx_done & (1U << index)) != 0)
		return 1;

	/* It is still on the bus. */
	return 0;
}

/* Returns the index of the lowest bit set in a nonzero mask. */
static unsigned
ncm_lowest_bit(
	unsigned mask)
{
	unsigned index;

	/* Counts the clear bits below it. */
	for (index = 0; index < 31U; index++) {
		if ((mask & (1U << index)) != 0)
			break;
	}

	/* Succeeded. */
	return index;
}

/* Hands the queued packets up to the network stack. */
static unsigned
ncm_deliver_queued(
	struct ncm_adapter *adapter,
	unsigned budget)
{
	struct packet_buf *packet;
	unsigned long irq;
	unsigned delivered = 0;

	/* Continue while the operation condition remains true. */
	while (delivered < budget) {
		/* Handles the adapter condition. */
		irq = spin_lock_irqsave(&adapter->lock);
		if (adapter->rx_queue_count == 0) {
			spin_unlock_irqrestore(&adapter->lock, irq);
			break;
		}

		packet = adapter->rx_queue[adapter->rx_queue_head];
		adapter->rx_queue[adapter->rx_queue_head] = NULL;
		adapter->rx_queue_head =
			(adapter->rx_queue_head + 1U) % NCM_RX_QUEUE_MAX;
		adapter->rx_queue_count--;
		spin_unlock_irqrestore(&adapter->lock, irq);
		net_device_receive(adapter->net_device, packet);
		delivered++;
	}

	/* Returns the computed result. */
	return delivered;
}

/* Joins the gate that serializes polling. */
static int
ncm_poll_enter(
	struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int admitted = adapter->ready && adapter->opened && !adapter->closing &&
		       !adapter->stopping && !adapter->quarantined;

	/* Handles the admitted condition. */
	if (admitted)
		adapter->polls_active++;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Returns the computed result. */
	return admitted;
}

/* Leaves that gate. */
static void
ncm_poll_exit(
	struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	/* Handles the adapter condition. */
	if (adapter->polls_active == 0)
		__builtin_trap();
	adapter->polls_active--;

	spin_unlock_irqrestore(&adapter->lock, irq);
}

/* Takes whatever the poll has to deal with. */
static int
ncm_take_pending(
	struct ncm_adapter *adapter,
	unsigned *pending)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int taken = *pending != 0;

	/* Handles the taken condition. */
	if (taken)
		*pending = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Returns the computed result. */
	return taken;
}

/* Puts back what the poll could not deal with yet. */
static void
ncm_restore_pending(
	struct ncm_adapter *adapter,
	unsigned *pending)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	*pending = 1;

	spin_unlock_irqrestore(&adapter->lock, irq);
}

/* Claims the right to re-arm the notification transfer. */
static int
ncm_take_notification_rearm(
	struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int taken = adapter->notification_rearm != 0 &&
		    adapter->notification_rearm_active == 0;

	/* Handles the taken condition. */
	if (taken) {
		adapter->notification_rearm = 0;
		adapter->notification_rearm_active = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Returns the computed result. */
	return taken;
}

/* Claims the right to re-arm the receive transfer. */
static int
ncm_take_rx_rearm(
	struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int taken = adapter->rx_rearm != 0 && adapter->rx_rearm_active == 0 &&
		    adapter->rx_queue_count == 0;

	/* Handles the taken condition. */
	if (taken) {
		adapter->rx_rearm = 0;
		adapter->rx_rearm_active = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Returns the computed result. */
	return taken;
}

/*
 * Takes the transmit completions the poll found: each transfer that ended
 * is free again, and, when no thread sends and frames wait, this poll
 * sends them.
 */
static int
ncm_poll_tx_completion(
	struct ncm_adapter *adapter)
{
	unsigned long irq;
	unsigned done;
	unsigned index;
	int error;
	int pump;

	/* Takes the completions that came. */
	irq = spin_lock_irqsave(&adapter->lock);

	done = adapter->tx_done;
	adapter->tx_done = 0;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* None came. */
	if (done == 0)
		return 0;

	/* Frees each transfer once its completion has wholly returned; one still returning is taken by a later poll. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		if ((done & (1U << index)) == 0)
			continue;
		error = drv_usb_urb_drain(adapter->tx_urbs[index], NCM_TRANSFER_TIMEOUT_MS);
		irq = spin_lock_irqsave(&adapter->lock);
		if (error == 0)
			adapter->tx_free |= 1U << index;
		else
			adapter->tx_done |= 1U << index;
		spin_unlock_irqrestore(&adapter->lock, irq);
	}

	/* Becomes the sender when none sends, a transfer is free and frames wait. */
	irq = spin_lock_irqsave(&adapter->lock);

	pump = 0;
	if (!adapter->tx_busy && adapter->tx_free != 0 &&
	    adapter->tx_queue_head != NULL && adapter->opened &&
	    !adapter->closing) {
		adapter->tx_busy = 1;
		adapter->starts_active++;
		pump = 1;
	}

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Sends the waiting frames. */
	if (pump)
		ncm_tx_pump(adapter);

	/* Succeeded: a completion was taken. */
	return 1;
}

/* Takes the notification completion the poll found. */
static int
ncm_poll_notification_completion(
	struct ncm_adapter *adapter)
{
	/* Checks the ncm take pending result. */
	if (!ncm_take_pending(adapter, &adapter->notification_ready))
		return 0;

	/* Checks the drv usb urb drain result. */
	if (drv_usb_urb_drain(adapter->notification_urb,
			      NCM_TRANSFER_TIMEOUT_MS) != 0) {
		ncm_restore_pending(adapter, &adapter->notification_ready);

		/* Succeeded. */
		return 0;
	}

	ncm_notification_process(adapter);
	ncm_restore_pending(adapter, &adapter->notification_rearm);

	/* Reports operation failure. */
	return 1;
}

/*
 * Takes the oldest receive transfer, when it has completed: it goes back
 * on at once with the spare buffer, so the device fills the next block
 * while this one is parsed and queued for the stack (the delivery queue
 * is empty here, the poll hands each block up whole before it takes the
 * next).
 */
static int
ncm_poll_rx_completion(
	struct net_device *device,
	struct ncm_adapter *adapter)
{
	struct drv_usb_urb *urb;
	size_t count = 0;
	size_t length;
	uint8_t *block;
	unsigned long irq;
	unsigned index;
	int rearm;
	int done;
	int error;

	/* The oldest transfer on the bus, if its completion has come. */
	irq = spin_lock_irqsave(&adapter->lock);

	done = ncm_rx_head_done_locked(adapter);
	index = adapter->rx_order[adapter->rx_order_head];

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Still on the bus. */
	if (!done)
		return 0;

	/* Waits for its completion to return wholly; a later poll takes one still returning. */
	urb = adapter->rx_urbs[index];
	error = drv_usb_urb_drain(urb, NCM_TRANSFER_TIMEOUT_MS);
	if (error != 0)
		return 0;

	/* Takes it off the bus and out of the order. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->rx_done &= ~(1U << index);
	adapter->rx_order_head = (adapter->rx_order_head + 1U) % NCM_RX_URBS;
	adapter->rx_order_count--;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* The block received, and the spare buffer for the transfer's next. */
	error = ncm_urb_status_error(drv_usb_urb_status(urb));
	length = drv_usb_urb_actual_length(urb);
	block = adapter->rx_urb_buffers[index];
	adapter->rx_urb_buffers[index] = adapter->rx_spare;
	adapter->rx_spare = NULL;

	/* Asks for the transfer to be armed again. */
	irq = spin_lock_irqsave(&adapter->lock);

	adapter->rx_idle |= 1U << index;
	adapter->rx_rearm = 1;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Arms it at once, when nothing else holds the right to. */
	rearm = ncm_take_rx_rearm(adapter);
	if (rearm)
		(void)ncm_rearm(adapter, 0);

	/* Parses the block and queues its datagrams. */
	if (error == 0) {
		error = drv_usb_cdc_ncm_parse_ntb16(
			&adapter->profile, &adapter->rx_state, block, length,
			ncm_queue_datagram, adapter, &count);
	}

	/* The parsed block's buffer is the next spare. */
	adapter->rx_spare = block;

	/* Counts a block that could not be taken as a receive error. */
	if (error != 0)
		device->rx_errors++;

	/* Succeeded: a block was taken. */
	return 1;
}

/* Asks whether the poll has anything left to do. */
static int
ncm_has_poll_work(
	struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int pending = adapter->ready && adapter->opened && !adapter->closing &&
		      !adapter->stopping && !adapter->quarantined &&
		      (adapter->notification_ready ||
		       ncm_rx_head_done_locked(adapter) ||
		       adapter->tx_done != 0 ||
		       (adapter->notification_rearm &&
			!adapter->notification_rearm_active) ||
		       (adapter->rx_rearm && !adapter->rx_rearm_active) ||
		       adapter->rx_queue_count != 0);

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Returns the computed result. */
	return pending;
}

/* Serves one round of polling for this device. */
static unsigned
ncm_poll_receive(
	struct net_device *device,
	unsigned budget)
{
	unsigned kind;
	int completed;
	struct ncm_adapter *adapter = device->driver_data;
	unsigned cursor, delivered, index, work = 0;
	unsigned long irq;
	int notification_completed = 0, rx_progress = 0;

	/* Checks the ncm poll enter result. */
	if (!ncm_poll_enter(adapter))
		return 0;
	delivered = ncm_deliver_queued(adapter, budget);
	work += delivered;
	rx_progress = delivered != 0;

	irq = spin_lock_irqsave(&adapter->lock);

	cursor = adapter->poll_cursor % NCM_COMPLETION_KINDS;

	spin_unlock_irqrestore(&adapter->lock, irq);

	/* Process each remaining element. */
	for (index = 0; index < NCM_COMPLETION_KINDS && work < budget;
	     index++) {
		/* Handles the kind condition. */
		kind = (cursor + index) % NCM_COMPLETION_KINDS;
		if (kind == NCM_COMPLETION_TX)
			completed = ncm_poll_tx_completion(adapter);
		else if (kind == NCM_COMPLETION_NOTIFICATION)
			completed = ncm_poll_notification_completion(adapter);
		else
			completed = ncm_poll_rx_completion(device, adapter);

		/* Handles the completed condition. */
		if (!completed)
			continue;

		/*
		 * A block received is handed up whole at once (the stack's
		 * input queue takes it), so that the receive transfer goes back
		 * on in this same poll: waiting for a later poll to deliver the
		 * rest kept the device without a transfer while the stack worked
		 * on the block before, and a fast link overflows the device's
		 * buffer meanwhile (BUG-222).
		 */
		if (kind == NCM_COMPLETION_NOTIFICATION) {
			notification_completed = 1;
		} else if (kind == NCM_COMPLETION_RX) {
			rx_progress = 1;
			(void)ncm_deliver_queued(adapter, NCM_RX_QUEUE_MAX);
		}
		work++;
		irq = spin_lock_irqsave(&adapter->lock);
		adapter->poll_cursor = (kind + 1U) % NCM_COMPLETION_KINDS;
		spin_unlock_irqrestore(&adapter->lock, irq);
	}

	/* The receive blocks that completed behind the first, each handed up whole before the next. */
	while (work < budget) {
		completed = ncm_poll_rx_completion(device, adapter);
		if (!completed)
			break;
		rx_progress = 1;
		(void)ncm_deliver_queued(adapter, NCM_RX_QUEUE_MAX);
		work++;
	}

	/* Handles the work condition. */
	if (work < budget) {
		delivered = ncm_deliver_queued(adapter, budget - work);
		work += delivered;

		/* Handles the delivered condition. */
		if (delivered != 0)
			rx_progress = 1;
	}

	/*
	 * A rearm caused by a completion (or by draining the last queued frame)
	 * is part of that already-budgeted work item.  A standalone retry
	 * consumes one unit so persistent HCD backpressure cannot escape the
	 * poll budget.
	 */
	if (notification_completed) {
		/* Handles the ncm take notification rearm condition. */
		if (ncm_take_notification_rearm(adapter))
			(void)ncm_rearm(adapter, 1);
	} else if (work < budget && ncm_take_notification_rearm(adapter)) {
		(void)ncm_rearm(adapter, 1);
		work++;
	}

	/* Handles the rx progress condition. */
	if (rx_progress) {
		/* Handles the ncm take rx rearm condition. */
		if (ncm_take_rx_rearm(adapter))
			(void)ncm_rearm(adapter, 0);
	} else if (work < budget && ncm_take_rx_rearm(adapter)) {
		(void)ncm_rearm(adapter, 0);
		work++;
	}

	/* Handles the ncm has poll work condition. */
	if (ncm_has_poll_work(adapter))
		net_device_schedule_poll(device);
	ncm_poll_exit(adapter);

	/* Returns the computed result. */
	return work;
}

/* Gives every resource this device held back. */
static void
ncm_release(
	void *driver_data)
{
	kern_free(driver_data);
}

/* Marks the device as ready to carry traffic. */
static void
ncm_set_ready(
	struct ncm_adapter *adapter,
	int ready)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	adapter->ready = ready != 0;

	spin_unlock_irqrestore(&adapter->lock, irq);
}

/* Gives the transfers this device used back. */
static void
ncm_urbs_free(
	struct ncm_adapter *adapter)
{
	unsigned index;

	/* Each transmit and receive transfer, then the notification one. */
	for (index = 0; index < NCM_TX_URBS; index++) {
		drv_usb_urb_free(adapter->tx_urbs[index]);
		adapter->tx_urbs[index] = NULL;
	}

	/* The receive ones. */
	for (index = 0; index < NCM_RX_URBS; index++) {
		drv_usb_urb_free(adapter->rx_urbs[index]);
		adapter->rx_urbs[index] = NULL;
	}

	/* The notification one. */
	drv_usb_urb_free(adapter->notification_urb);
	adapter->notification_urb = NULL;
}

/* Takes the transfers this device needs: the notification one and rx_urb_count and tx_urb_count of the bulk ones. */
static int
ncm_urbs_alloc(
	struct ncm_adapter *adapter)
{
	unsigned index;
	int missing;

	/* The notification transfer. */
	missing = 0;
	adapter->notification_urb = drv_usb_urb_alloc(
		adapter->usb_device, adapter->notification_endpoint, 0);
	if (adapter->notification_urb == NULL)
		missing = 1;

	/* The receive transfers. */
	for (index = 0; index < adapter->rx_urb_count; index++) {
		adapter->rx_urbs[index] =
			drv_usb_urb_alloc(adapter->usb_device, adapter->bulk_in, 0);
		if (adapter->rx_urbs[index] == NULL)
			missing = 1;
	}

	/* The transmit transfers. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		adapter->tx_urbs[index] =
			drv_usb_urb_alloc(adapter->usb_device, adapter->bulk_out, 0);
		if (adapter->tx_urbs[index] == NULL)
			missing = 1;
	}

	/* All of them, or none. */
	if (missing) {
		ncm_urbs_free(adapter);

		/* Failed. */
		return ENOMEM;
	}

	/* None is on the bus: all receive ones wait for a rearm, all transmit ones are free. */
	adapter->rx_idle = (1U << adapter->rx_urb_count) - 1U;
	adapter->tx_free = (1U << adapter->tx_urb_count) - 1U;

	/* Succeeded. */
	return 0;
}

/* Gives the buffers this device used back. */
static void
ncm_buffers_free(
	struct ncm_adapter *adapter)
{
	unsigned index;

	/* Each buffer once, from the lists that own them. */
	for (index = 0; index < NCM_TX_URBS; index++) {
		kern_free(adapter->tx_buffers[index]);
		adapter->tx_buffers[index] = NULL;
	}

	/* The receive buffers. */
	for (index = 0; index < NCM_RX_URBS + 1U; index++) {
		kern_free(adapter->rx_buffers[index]);
		adapter->rx_buffers[index] = NULL;
	}

	/* No transfer refers to them any more. */
	for (index = 0; index < NCM_RX_URBS; index++)
		adapter->rx_urb_buffers[index] = NULL;
	adapter->rx_spare = NULL;

	/* The notification buffer. */
	kern_free(adapter->notification_buffer);
	adapter->notification_buffer = NULL;
}

/*
 * Takes the buffers this device needs: the notification one, one more
 * receive buffer than receive transfers (the spare), and one block for
 * each transmit transfer.
 */
static int
ncm_buffers_alloc(
	struct ncm_adapter *adapter)
{
	unsigned index;
	int missing;

	/* The notification buffer. */
	missing = 0;
	adapter->notification_buffer = kern_malloc(NCM_NOTIFICATION_SIZE);
	if (adapter->notification_buffer == NULL)
		missing = 1;

	/* The receive buffers: each transfer's, and the spare last. */
	for (index = 0; index < adapter->rx_urb_count + 1U; index++) {
		adapter->rx_buffers[index] = kern_malloc(adapter->profile.ntb_in_max_size);
		if (adapter->rx_buffers[index] == NULL)
			missing = 1;
	}

	/* The transmit blocks. */
	for (index = 0; index < adapter->tx_urb_count; index++) {
		adapter->tx_buffers[index] = kern_malloc(adapter->profile.ntb_out_max_size);
		if (adapter->tx_buffers[index] == NULL)
			missing = 1;
	}

	/* All of them, or none. */
	if (missing) {
		ncm_buffers_free(adapter);

		/* Failed. */
		return ENOMEM;
	}

	/* Hands each receive transfer its buffer, and keeps the spare. */
	for (index = 0; index < adapter->rx_urb_count; index++)
		adapter->rx_urb_buffers[index] = adapter->rx_buffers[index];
	adapter->rx_spare = adapter->rx_buffers[adapter->rx_urb_count];

	/* Succeeded. */
	return 0;
}

/* Publishes this device as a network interface. */
static int
ncm_net_device_create(
	struct ncm_adapter *adapter,
	const uint8_t mac[6])
{
	struct net_device *device = net_device_alloc();
	unsigned index;
	int error = ENOSPC;

	/* Handles the device availability. */
	if (device == NULL)
		return ENOSPC;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->mtu = DRV_USB_CDC_NCM_MTU;
	device->mtu_max = adapter->profile.max_datagram_size - DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE;
	if (device->mtu_max > NET_MTU_MAX)
		device->mtu_max = NET_MTU_MAX;
	kern_memcpy(device->hwaddr, mac, 6U);
	device->hwaddr_len = 6U;
	device->ops = &ncm_net_ops;
	device->driver_data = adapter;
	/* Process each remaining element. */
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		device->name[0] = 'u';
		device->name[1] = 'e';
		device->name[2] = (char)('0' + index);
		device->name[3] = '\0';

		/* Checks the operation status. */
		error = net_device_create(device);
		if (error != EEXIST)
			break;
	}
	if (error != 0) {
		/*
		 * The allocation owner, not the unpublished device, still owns
		 * the adapter on an attach error.
		 */
		device->driver_data = NULL;
		net_device_destroy(device);

		/* Failed. */
		return error;
	}

	adapter->net_device = device;

	/* Succeeded. */
	return 0;
}

/* Binds this driver to an interface the bus has matched. */
static int
ncm_attach(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	struct ncm_binding binding;
	struct ncm_adapter *adapter;
	struct drv_usb_cdc_ncm_limits limits;
	uint8_t parameters[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE];
	uint8_t mac[6];
	size_t actual = 0;
	int error;

	(void)id;

	/* Checks the ncm binding parse result. */
	if (!ncm_binding_parse(interface, &binding))
		return ENODEV;

	/* Handles the adapter availability. */
	adapter = kern_malloc(sizeof(*adapter));
	if (adapter == NULL)
		return ENOMEM;
	kern_memset(adapter, 0, sizeof(*adapter));
	adapter->usb_device = binding.device;
	adapter->control = binding.control;
	adapter->data = binding.data;
	adapter->notification_endpoint = binding.notification;
	adapter->bulk_in = binding.bulk_in;
	adapter->bulk_out = binding.bulk_out;
	adapter->data_alternate = binding.data_alternate;
	adapter->capabilities = binding.capabilities;
	adapter->max_segment_size = binding.max_segment_size;
	adapter->notification_length = drv_usb_cdc_notification_transfer_length(
		drv_usb_endpoint_max_packet_size(binding.notification));
	spin_init(&adapter->lock, LOCK_RANK_DEVICE, "usb-cdc-ncm");

	/*
	 * The USB core owns failed-attach cleanup through the provisional
	 * binding. Publish partial driver state before acquiring any sibling or
	 * resource.
	 */

	/* Checks the operation status. */
	error = drv_usb_interface_set_driver_data(interface, adapter);
	if (error != 0) {
		kern_free(adapter);

		/* Failed. */
		return error;
	}

	/* Checks the operation status. */
	error = drv_usb_interface_claim(interface, binding.data);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = ncm_get_mac(&binding, mac);
	if (error != 0)
		return error;
	kern_memset(&limits, 0, sizeof(limits));
	limits.ntb_in_max_size = NCM_NTB_IN_SIZE;
	limits.ntb_out_max_size = NCM_NTB_BUFFER_SIZE;

	/*
	 * Jumbo datagrams where the pool has jumbo packets: up to the
	 * descriptor's wMaxSegmentSize, or, for a device that takes
	 * SET_MAX_DATAGRAM_SIZE, up to the longest the pool holds, which the
	 * device's read back then confirms or lowers.
	 */
	limits.max_datagram_size = binding.max_segment_size;
	if ((binding.capabilities & NCM_CAP_MAX_DATAGRAM_SIZE) != 0)
		limits.max_datagram_size = NCM_DATAGRAM_MAX;
	if (limits.max_datagram_size > NCM_DATAGRAM_MAX)
		limits.max_datagram_size = NCM_DATAGRAM_MAX;
	if (limits.max_datagram_size > DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE) {
		limits.ntb_in_max_size = NCM_NTB_IN_JUMBO_SIZE;
		limits.ntb_out_max_size = NCM_NTB_OUT_SIZE;
	}
	limits.rx_max_datagrams = NCM_RX_QUEUE_MAX;
	limits.tx_max_datagrams = NCM_TX_BATCH_MAX;
	limits.ndp_chain_max = DRV_USB_CDC_NCM_MAX_NDP_CHAIN;
	limits.bulk_out_max_packet_size =
		drv_usb_endpoint_max_packet_size(binding.bulk_out);

	/* Checks the operation status. */
	error = ncm_control(adapter,
			    DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS |
				    DRV_USB_RECIP_INTERFACE,
			    NCM_GET_NTB_PARAMETERS, 0, parameters,
			    sizeof(parameters), &actual);
	if (error != 0 || actual != sizeof(parameters)) {
		error = error != 0 ? error : EIO;

		/* Failed. */
		return error;
	}

	/* Checks the operation status. */
	error = drv_usb_cdc_ncm_negotiate_nth16(parameters, actual, &limits,
						&adapter->profile);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = ncm_program_profile(adapter);
	if (error != 0)
		return error;
	drv_usb_cdc_ncm_rx_reset(&adapter->rx_state);

	/*
	 * Asks the host controller to keep several bulk transfers on the bus
	 * at once; one that keeps one, or refuses, gives one of each.
	 */
	error = drv_usb_endpoint_set_queue_depth(binding.bulk_in, NCM_RX_URBS, &adapter->rx_urb_count);
	if (error != 0)
		adapter->rx_urb_count = 1;
	error = drv_usb_endpoint_set_queue_depth(binding.bulk_out, NCM_TX_URBS, &adapter->tx_urb_count);
	if (error != 0)
		adapter->tx_urb_count = 1;

	/* Checks the operation status. */
	error = ncm_buffers_alloc(adapter);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = ncm_urbs_alloc(adapter);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = ncm_net_device_create(adapter, mac);
	if (error != 0)
		return error;

	/*
	 * Idle URBs deliberately retain the inactive data endpoints.  The p015
	 * interface transaction makes this the final fallible attach operation.
	 */

	/* Checks the operation status. */
	error = drv_usb_interface_set_alternate(binding.data,
						binding.data_alternate);
	if (error != 0)
		return error;
	ncm_set_ready(adapter, 1);
	kern_logf("usb-cdc-ncm: %s mac=%02x:%02x:%02x:%02x:%02x:%02x "
		   "segment=%u ntb_in=%u ntb_out=%u max_datagram=%u read_back=%u "
		   "caps=0x%02x mtu_max=%u urbs=%u/%u\n",
		   adapter->net_device->name, mac[0], mac[1], mac[2], mac[3],
		   mac[4], mac[5], adapter->max_segment_size,
		   (unsigned)adapter->profile.ntb_in_max_size,
		   (unsigned)adapter->profile.ntb_out_max_size,
		   adapter->profile.max_datagram_size,
		   adapter->max_datagram_read_back, adapter->capabilities,
		   adapter->net_device->mtu_max, adapter->rx_urb_count,
		   adapter->tx_urb_count);

	/* Succeeded. */
	return 0;
}

/* Gives that interface up and everything held for it. */
static int
ncm_detach(
	struct drv_usb_interface *interface,
	unsigned flags)
{
	struct ncm_adapter *adapter = drv_usb_interface_driver_data(interface);
	int error;

	/* Handles the adapter availability. */
	if (adapter == NULL)
		return 0;
	ncm_set_ready(adapter, 0);

	/* Checks the operation status. */
	error = ncm_stop(adapter);
	if (error != 0)
		return error;

	/*
	 * p015 permits allocated, completely drained URBs to retain inactive
	 * endpoint objects across SET_INTERFACE.  Keep the complete graph until
	 * this final normal-detach hardware transaction has succeeded.
	 */
	if ((flags & (DRV_USB_DETACH_FORCE | DRV_USB_DETACH_ATTACH_FAILED)) ==
	    0) {
		/* Checks the operation status. */
		error = drv_usb_interface_set_alternate(adapter->data, 0);
		if (error != 0)
			return error;
	}

	/* Checks the operation status. */
	error = adapter->net_device != NULL
			? net_device_gone(adapter->net_device)
			: 0;
	if (error != 0)
		return error;
	ncm_urbs_free(adapter);
	ncm_buffers_free(adapter);

	/* Handles the net device availability. */
	if (adapter->net_device != NULL)
		net_device_destroy(adapter->net_device);
	else
		kern_free(adapter);

	/* Succeeded. */
	return 0;
}

/* Takes the device out of service at system shutdown. */
static void
ncm_shutdown(
	struct drv_usb_interface *interface)
{
	struct ncm_adapter *adapter = drv_usb_interface_driver_data(interface);

	/* Handles the adapter availability. */
	if (adapter != NULL) {
		ncm_set_ready(adapter, 0);
		(void)ncm_stop(adapter);
	}
}

/* Reports whether this driver can drive an interface. */
static int
ncm_match(
	struct drv_usb_interface *interface,
	const struct drv_usb_id *id)
{
	int error;
	struct ncm_binding binding;

	(void)id;

	/* Computes the function result. */
	error = ncm_binding_parse(interface, &binding) ? 100 : 0;

	/* Returns the computed result. */
	return error;
}
