/*
 * Integrated USB CDC NCM network driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/usb-cdc-ncm.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/sched.h>
#include <limits.h>
#include <string.h>

#define NCM_COMMUNICATION_CLASS 0x02U
#define NCM_COMMUNICATION_SUBCLASS 0x0dU
#define NCM_COMMUNICATION_PROTOCOL 0x00U
#define NCM_DATA_CLASS 0x0aU
#define NCM_DATA_SUBCLASS 0x00U
#define NCM_DATA_PROTOCOL 0x01U

#define NCM_CS_INTERFACE 0x24U
#define NCM_HEADER_DESCRIPTOR 0x00U
#define NCM_UNION_DESCRIPTOR 0x06U
#define NCM_ETHERNET_DESCRIPTOR 0x0fU
#define NCM_FUNCTIONAL_DESCRIPTOR 0x1aU

#define NCM_GET_NTB_PARAMETERS 0x80U
#define NCM_SET_ETHERNET_PACKET_FILTER 0x43U
#define NCM_FILTER_ALL_MULTICAST 0x0002U
#define NCM_FILTER_DIRECTED 0x0004U
#define NCM_FILTER_BROADCAST 0x0008U

#define NCM_NOTIFICATION_NETWORK_CONNECTION 0x00U
#define NCM_NOTIFICATION_SPEED_CHANGE 0x2aU
#define NCM_NOTIFICATION_SIZE 16U
#define NCM_NTB_BUFFER_SIZE 8192U
#define NCM_RX_QUEUE_MAX 8U
#define NCM_TRANSFER_TIMEOUT_MS 5000U
#define NCM_CONTROL_TIMEOUT_MS 1000U
#define NCM_REARM_RETRY_MAX 3U
#define NCM_COMPLETION_KINDS 3U

#define NCM_COMPLETION_TX 0U
#define NCM_COMPLETION_NOTIFICATION 1U
#define NCM_COMPLETION_RX 2U

#define NCM_CAP_PACKET_FILTER 0x01U
#define NCM_CAP_MAX_DATAGRAM_SIZE 0x08U
#define NCM_CAP_CRC_MODE 0x10U

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
};

struct ncm_adapter {
	struct drv_usb_device *usb_device;
	struct drv_usb_interface *control;
	struct drv_usb_interface *data;
	struct drv_usb_endpoint *notification_endpoint;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	struct drv_usb_urb *notification_urb;
	struct drv_usb_urb *rx_urb;
	struct drv_usb_urb *tx_urb;
	struct net_device *net_device;
	struct drv_usb_cdc_ncm_profile profile;
	struct drv_usb_cdc_ncm_rx_state rx_state;
	struct spinlock lock;
	uint8_t *notification_buffer;
	uint8_t *rx_buffer;
	uint8_t *tx_buffer;
	struct packet_buf *rx_queue[NCM_RX_QUEUE_MAX];
	size_t rx_queue_head;
	size_t rx_queue_count;
	unsigned data_alternate;
	uint8_t capabilities;
	unsigned ready;
	unsigned starts_active;
	unsigned polls_active;
	unsigned poll_cursor;
	unsigned opened;
	unsigned closing;
	unsigned stopping;
	unsigned quarantined;
	unsigned notification_ready;
	unsigned rx_ready;
	unsigned tx_ready;
	unsigned notification_rearm;
	unsigned rx_rearm;
	unsigned notification_rearm_active;
	unsigned rx_rearm_active;
	unsigned notification_retries;
	unsigned rx_retries;
	unsigned tx_busy;
	uint16_t tx_sequence;
	int stop_error;
	uint32_t upstream_bps;
	uint32_t downstream_bps;
};

static uint16_t
ncm_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t
ncm_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static struct drv_usb_configuration *
ncm_interface_configuration(struct drv_usb_interface *interface)
{
	struct drv_usb_device *device = drv_usb_interface_device(interface);
	unsigned configuration_index;

	for (configuration_index = 0;
	    configuration_index < drv_usb_device_configuration_count(device);
	    configuration_index++) {
		struct drv_usb_configuration *configuration =
		    drv_usb_device_configuration(device, configuration_index);
		unsigned interface_index;

		for (interface_index = 0;
		    interface_index <
		    drv_usb_configuration_interface_count(configuration);
		    interface_index++)
			if (drv_usb_configuration_interface(configuration,
			    interface_index) == interface)
				return configuration;
	}
	return NULL;
}

static int
ncm_iad_covers(
	const struct drv_usb_interface_association_descriptor *iad,
	unsigned interface_number)
{
	return iad->interface_count != 0U &&
	    interface_number >= iad->first_interface &&
	    interface_number - iad->first_interface < iad->interface_count;
}

static int
ncm_iad_consistent(struct drv_usb_configuration *configuration,
	unsigned control_number, unsigned data_number)
{
	unsigned index;
	int association = 0;

	for (index = 0; index < drv_usb_configuration_iad_count(configuration);
	    index++) {
		const struct drv_usb_interface_association_descriptor *iad =
		    drv_usb_configuration_iad(configuration, index);

		if (iad == NULL)
			return 0;
		if (!ncm_iad_covers(iad, control_number) &&
		    !ncm_iad_covers(iad, data_number) &&
		    iad->first_interface != control_number &&
		    iad->first_interface != data_number)
			continue;
		if (association || iad->first_interface != control_number ||
		    iad->interface_count != 2U ||
		    iad->function_class != NCM_COMMUNICATION_CLASS ||
		    iad->function_subclass != NCM_COMMUNICATION_SUBCLASS ||
		    iad->function_protocol != NCM_COMMUNICATION_PROTOCOL ||
		    data_number != control_number + 1U)
			return 0;
		association = 1;
	}
	return 1;
}

static int
ncm_control_descriptors(const struct drv_usb_host_interface *alternate,
	unsigned control_number, unsigned *data_number, uint8_t *mac_string,
	uint8_t *capabilities)
{
	unsigned index, header = 0, union_descriptor = 0, ethernet = 0, ncm = 0;

	for (index = 0;
	    index < drv_usb_host_interface_extra_count(alternate); index++) {
		const uint8_t *descriptor;
		size_t length;

		if (drv_usb_host_interface_extra(alternate, index,
		    (const void **)&descriptor, &length) != 0 || descriptor == NULL ||
		    length < 2U)
			return 0;
		if (descriptor[1] != NCM_CS_INTERFACE)
			continue;
		if (length < 3U || descriptor[0] != length)
			return 0;
		switch (descriptor[2]) {
		case NCM_HEADER_DESCRIPTOR:
			if (length != 5U || ++header != 1U ||
			    ncm_le16(descriptor + 3U) == 0)
				return 0;
			break;
		case NCM_UNION_DESCRIPTOR:
			if (header != 1U || length != 5U ||
			    ++union_descriptor != 1U ||
			    descriptor[3] != control_number ||
			    descriptor[4] == control_number)
				return 0;
			*data_number = descriptor[4];
			break;
		case NCM_ETHERNET_DESCRIPTOR:
			if (header != 1U || length != 13U || ++ethernet != 1U ||
			    descriptor[3] == 0 ||
			    ncm_le16(descriptor + 8U) <
				DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE)
				return 0;
			*mac_string = descriptor[3];
			break;
		case NCM_FUNCTIONAL_DESCRIPTOR:
			if (header != 1U || length != 6U || ++ncm != 1U ||
			    ncm_le16(descriptor + 3U) != 0x0100U ||
			    (descriptor[5] & 0xc0U) != 0)
				return 0;
			*capabilities = descriptor[5];
			break;
		default:
			break;
		}
	}
	return header == 1U && union_descriptor == 1U && ethernet == 1U &&
	    ncm == 1U;
}

static int
ncm_find_notification(const struct drv_usb_host_interface *alternate,
	struct drv_usb_endpoint **result)
{
	unsigned index;
	struct drv_usb_endpoint *notification = NULL;

	for (index = 0;
	    index < drv_usb_host_interface_endpoint_count(alternate); index++) {
		struct drv_usb_endpoint *endpoint =
		    drv_usb_host_interface_endpoint(alternate, index);

		if (drv_usb_endpoint_type(endpoint) != DRV_USB_TRANSFER_INTERRUPT ||
		    !drv_usb_endpoint_is_input(endpoint) || notification != NULL)
			return 0;
		notification = endpoint;
	}
	if (notification == NULL)
		return 0;
	*result = notification;
	return 1;
}

static int
ncm_find_data_alternate(struct drv_usb_interface *data,
	struct drv_usb_endpoint **bulk_in, struct drv_usb_endpoint **bulk_out,
	unsigned *alternate_result)
{
	unsigned index;
	int empty_found = 0, bulk_found = 0;

	for (index = 0; index < drv_usb_interface_alternate_count(data);
	    index++) {
		const struct drv_usb_host_interface *alternate =
		    drv_usb_interface_alternate(data, index);
		const struct drv_usb_interface_descriptor *descriptor =
		    drv_usb_host_interface_descriptor(alternate);
		struct drv_usb_endpoint *input = NULL, *output = NULL;
		unsigned endpoint_index;

		if (descriptor == NULL ||
		    descriptor->interface_class != NCM_DATA_CLASS ||
		    descriptor->interface_subclass != NCM_DATA_SUBCLASS ||
		    descriptor->interface_protocol != NCM_DATA_PROTOCOL)
			return 0;
		if (descriptor->alternate_setting == 0U) {
			if (descriptor->endpoint_count != 0U || empty_found)
				return 0;
			empty_found = 1;
			continue;
		}
		if (descriptor->endpoint_count != 2U)
			continue;
		for (endpoint_index = 0; endpoint_index < 2U;
		    endpoint_index++) {
			struct drv_usb_endpoint *endpoint =
			    drv_usb_host_interface_endpoint(alternate,
			    endpoint_index);

			if (drv_usb_endpoint_type(endpoint) !=
			    DRV_USB_TRANSFER_BULK)
				break;
			if (drv_usb_endpoint_is_input(endpoint)) {
				if (input != NULL)
					break;
				input = endpoint;
			} else {
				if (output != NULL)
					break;
				output = endpoint;
			}
		}
		if (endpoint_index != 2U || input == NULL || output == NULL)
			continue;
		if (bulk_found)
			return 0;
		bulk_found = 1;
		*bulk_in = input;
		*bulk_out = output;
		*alternate_result = descriptor->alternate_setting;
	}
	return empty_found && bulk_found;
}

static int
ncm_binding_parse(struct drv_usb_interface *control,
	struct ncm_binding *binding)
{
	struct drv_usb_configuration *configuration;
	const struct drv_usb_interface_descriptor *control_descriptor;
	const struct drv_usb_host_interface *control_alternate;
	unsigned control_number, data_number = UINT_MAX;
	uint8_t mac_string = 0;

	memset(binding, 0, sizeof(*binding));
	control_descriptor = drv_usb_interface_descriptor(control);
	if (control_descriptor == NULL ||
	    control_descriptor->interface_class != NCM_COMMUNICATION_CLASS ||
	    control_descriptor->interface_subclass != NCM_COMMUNICATION_SUBCLASS ||
	    control_descriptor->interface_protocol != NCM_COMMUNICATION_PROTOCOL ||
	    drv_usb_interface_alternate_count(control) != 1U)
		return 0;
	if ((drv_usb_device_hcd_capabilities(
	    drv_usb_interface_device(control)) &
	    DRV_USB_HCD_CAP_CONCURRENT_URBS) == 0)
		return 0;
	configuration = ncm_interface_configuration(control);
	if (configuration == NULL)
		return 0;
	control_alternate = drv_usb_interface_alternate(control, 0);
	control_number = control_descriptor->interface_number;
	if (control_alternate == NULL ||
	    !ncm_control_descriptors(control_alternate, control_number,
	    &data_number, &mac_string, &binding->capabilities) ||
	    !ncm_find_notification(control_alternate, &binding->notification))
		return 0;
	binding->data = drv_usb_configuration_find_interface(configuration,
	    data_number);
	if (binding->data == NULL ||
	    !ncm_iad_consistent(configuration, control_number, data_number) ||
	    !ncm_find_data_alternate(binding->data, &binding->bulk_in,
	    &binding->bulk_out, &binding->data_alternate))
		return 0;
	binding->device = drv_usb_interface_device(control);
	binding->control = control;
	binding->mac_string = mac_string;
	return 1;
}

static int
ncm_hex(unsigned char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

static int
ncm_get_mac(const struct ncm_binding *binding, uint8_t mac[6])
{
	char string[13];
	unsigned index;
	int all_zero = 1;

	if (drv_usb_device_get_string(binding->device, binding->mac_string, 0,
	    string, sizeof(string)) != 0 || strlen(string) != 12U)
		return EINVAL;
	for (index = 0; index < 6U; index++) {
		int high = ncm_hex((unsigned char)string[index * 2U]);
		int low = ncm_hex((unsigned char)string[index * 2U + 1U]);

		if (high < 0 || low < 0)
			return EINVAL;
		mac[index] = (uint8_t)((high << 4) | low);
		if (mac[index] != 0)
			all_zero = 0;
	}
	if (all_zero || (mac[0] & 1U) != 0)
		return EINVAL;
	return 0;
}

static int
ncm_control(struct ncm_adapter *adapter, uint8_t request_type,
	uint8_t request, uint16_t value, void *buffer, size_t length,
	size_t *actual)
{
	return drv_usb_control(adapter->usb_device, request_type, request, value,
	    (uint16_t)drv_usb_interface_number(adapter->control), buffer, length,
	    NCM_CONTROL_TIMEOUT_MS, actual);
}

static int
ncm_program_profile(struct ncm_adapter *adapter)
{
	static const enum drv_usb_cdc_ncm_control_step steps[] = {
		DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16,
		DRV_USB_CDC_NCM_CONTROL_SET_INPUT_SIZE,
		DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE,
		DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC
	};
	unsigned index;

	for (index = 0; index < sizeof(steps) / sizeof(steps[0]); index++) {
		struct drv_usb_cdc_ncm_control_request request;
		size_t actual = 0;
		int error;

		if (steps[index] ==
		    DRV_USB_CDC_NCM_CONTROL_SET_MAX_DATAGRAM_SIZE &&
		    (adapter->capabilities & NCM_CAP_MAX_DATAGRAM_SIZE) == 0)
			continue;
		if (steps[index] == DRV_USB_CDC_NCM_CONTROL_DISABLE_CRC &&
		    (adapter->capabilities & NCM_CAP_CRC_MODE) == 0)
			continue;
		error = drv_usb_cdc_ncm_make_control_request(&adapter->profile,
		    steps[index], &request);

		if (error == EOPNOTSUPP &&
		    steps[index] == DRV_USB_CDC_NCM_CONTROL_SELECT_NTH16)
			continue;
		if (error != 0)
			return error;
		error = ncm_control(adapter,
		    DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
		    DRV_USB_RECIP_INTERFACE, request.request, request.value,
		    request.payload, request.length, &actual);
		if (error != 0 || actual != request.length)
			return error != 0 ? error : EIO;
	}
	return 0;
}

static int
ncm_program_packet_filter(struct ncm_adapter *adapter)
{
	size_t actual = 0;
	unsigned long irq;
	int error;

	if ((adapter->capabilities & NCM_CAP_PACKET_FILTER) == 0)
		return 0;
	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		return ENETDOWN;
	}
	adapter->starts_active++;
	spin_unlock_irqrestore(&adapter->lock, irq);
	error = ncm_control(adapter, DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
	    DRV_USB_RECIP_INTERFACE, NCM_SET_ETHERNET_PACKET_FILTER,
	    NCM_FILTER_DIRECTED | NCM_FILTER_ALL_MULTICAST |
	    NCM_FILTER_BROADCAST, NULL, 0, &actual);
	if (error != 0 || actual != 0)
		error = error != 0 ? error : EIO;
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->starts_active == 0)
		__builtin_trap();
	if (error == 0 && (adapter->closing || !adapter->opened))
		error = ENETDOWN;
	adapter->starts_active--;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return error;
}

static int
ncm_urb_status_error(enum drv_usb_urb_status status)
{
	if (status == DRV_USB_URB_COMPLETE)
		return 0;
	if (status == DRV_USB_URB_TIMEOUT)
		return ETIMEDOUT;
	if (status == DRV_USB_URB_STALL)
		return EPIPE;
	if (status == DRV_USB_URB_DISCONNECTED)
		return ENODEV;
	return EIO;
}

static int
ncm_tx_status_is_error(enum drv_usb_urb_status status)
{
	return status == DRV_USB_URB_STALL ||
	    status == DRV_USB_URB_TIMEOUT ||
	    status == DRV_USB_URB_DISCONNECTED ||
	    status == DRV_USB_URB_IO_ERROR;
}

static void
ncm_completion(struct drv_usb_urb *urb, void *argument)
{
	struct ncm_adapter *adapter = argument;
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int tx_error = 0;

	if (urb == adapter->notification_urb)
		adapter->notification_ready = 1;
	else if (urb == adapter->rx_urb)
		adapter->rx_ready = 1;
	else if (urb == adapter->tx_urb) {
		adapter->tx_ready = 1;
		tx_error = ncm_tx_status_is_error(drv_usb_urb_status(urb));
	}
	spin_unlock_irqrestore(&adapter->lock, irq);
	if (tx_error)
		net_device_tx_error(adapter->net_device);
	net_device_schedule_poll(adapter->net_device);
}

static int
ncm_start_urb(struct ncm_adapter *adapter, struct drv_usb_urb *urb,
	void *buffer, size_t length)
{
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		return ENETDOWN;
	}
	adapter->starts_active++;
	spin_unlock_irqrestore(&adapter->lock, irq);
	error = drv_usb_urb_setup(urb, buffer, length, 0, 0, ncm_completion,
	    adapter);
	if (error == 0)
		error = drv_usb_urb_submit(urb);
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->starts_active == 0)
		__builtin_trap();
	adapter->starts_active--;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return error;
}

static int
ncm_cancel_and_drain(struct drv_usb_urb *urb)
{
	enum drv_usb_urb_status status;

	if (urb == NULL)
		return 0;
	status = drv_usb_urb_status(urb);
	if (status == DRV_USB_URB_PENDING)
		(void)drv_usb_urb_cancel(urb);
	return drv_usb_urb_drain(urb, NCM_TRANSFER_TIMEOUT_MS);
}

static void
ncm_wait_activity(struct ncm_adapter *adapter)
{
	for (;;) {
		unsigned long irq = spin_lock_irqsave(&adapter->lock);
		unsigned active = adapter->starts_active + adapter->polls_active;

		spin_unlock_irqrestore(&adapter->lock, irq);
		if (active == 0)
			return;
		sched_yield();
	}
}

static void
ncm_free_rx_queue(struct ncm_adapter *adapter)
{
	struct packet_buf *packets[NCM_RX_QUEUE_MAX];
	unsigned long irq;
	size_t count = 0;

	irq = spin_lock_irqsave(&adapter->lock);
	while (adapter->rx_queue_count != 0) {
		packets[count++] = adapter->rx_queue[adapter->rx_queue_head];
		adapter->rx_queue[adapter->rx_queue_head] = NULL;
		adapter->rx_queue_head =
		    (adapter->rx_queue_head + 1U) % NCM_RX_QUEUE_MAX;
		adapter->rx_queue_count--;
	}
	adapter->rx_queue_head = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	while (count != 0)
		packet_buf_free(packets[--count]);
}

static int
ncm_stop(struct ncm_adapter *adapter)
{
	unsigned long irq;
	int error = 0, candidate;

	for (;;) {
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
	/* A poll admitted before closing was published may still parse an RX
	 * buffer or attempt a rearm.  Join that complete worker section before
	 * cancelling URBs or changing the data-interface alternate. */
	ncm_wait_activity(adapter);
	candidate = ncm_cancel_and_drain(adapter->notification_urb);
	if (error == 0)
		error = candidate;
	candidate = ncm_cancel_and_drain(adapter->rx_urb);
	if (error == 0)
		error = candidate;
	candidate = ncm_cancel_and_drain(adapter->tx_urb);
	if (error == 0)
		error = candidate;
	if (error == 0)
		ncm_free_rx_queue(adapter);
	irq = spin_lock_irqsave(&adapter->lock);
	if (error == 0) {
		adapter->notification_ready = 0;
		adapter->rx_ready = 0;
		adapter->tx_ready = 0;
		adapter->notification_rearm = 0;
		adapter->rx_rearm = 0;
		adapter->notification_rearm_active = 0;
		adapter->rx_rearm_active = 0;
		adapter->notification_retries = 0;
		adapter->rx_retries = 0;
		adapter->tx_busy = 0;
	} else
		adapter->quarantined = 1;
	adapter->stop_error = error;
	adapter->closing = 0;
	adapter->stopping = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	if (adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);
	return error;
}

static int
ncm_open(struct net_device *device)
{
	struct ncm_adapter *adapter = device->driver_data;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->opened || adapter->closing ||
	    adapter->stopping ||
	    adapter->quarantined) {
		error = !adapter->ready ? ENETDOWN :
		    (adapter->quarantined ? EIO : EBUSY);
		spin_unlock_irqrestore(&adapter->lock, irq);
		return error;
	}
	adapter->opened = 1;
	adapter->stop_error = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	/* The data alternate is active by the time the net device becomes ready.
	 * Some functions discard their filter across an administrative close, so
	 * program it on every open and before publishing any persistent URB. */
	error = ncm_program_packet_filter(adapter);
	if (error == 0)
		error = ncm_start_urb(adapter, adapter->notification_urb,
	    adapter->notification_buffer, NCM_NOTIFICATION_SIZE);
	if (error == 0)
		error = ncm_start_urb(adapter, adapter->rx_urb,
		    adapter->rx_buffer, adapter->profile.ntb_in_max_size);
	if (error != 0) {
		(void)ncm_stop(adapter);
		return error;
	}
	return 0;
}

static void
ncm_close(struct net_device *device)
{
	struct ncm_adapter *adapter = device->driver_data;

	(void)ncm_stop(adapter);
}

static int
ncm_transmit(struct net_device *device, struct packet_buf *packet)
{
	struct ncm_adapter *adapter = device->driver_data;
	unsigned long irq;
	size_t ntb_length = 0;
	uint16_t sequence;
	int error;

	if (packet == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->quarantined) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);
		return ENETDOWN;
	}
	if (adapter->tx_busy) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);
		return ENOBUFS;
	}
	adapter->tx_busy = 1;
	adapter->starts_active++;
	sequence = adapter->tx_sequence;
	spin_unlock_irqrestore(&adapter->lock, irq);
	error = drv_usb_cdc_ncm_build_ntb16(&adapter->profile,
	    sequence, packet->data, packet->length,
	    adapter->tx_buffer, adapter->profile.ntb_out_max_size, &ntb_length);
	packet_buf_free(packet);
	irq = spin_lock_irqsave(&adapter->lock);
	if (error == 0 && (adapter->closing || !adapter->opened))
		error = ENETDOWN;
	spin_unlock_irqrestore(&adapter->lock, irq);
	if (error == 0)
		error = drv_usb_urb_setup(adapter->tx_urb, adapter->tx_buffer,
		    ntb_length, 0, NCM_TRANSFER_TIMEOUT_MS, ncm_completion,
		    adapter);
	if (error == 0)
		error = drv_usb_urb_submit(adapter->tx_urb);
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->starts_active == 0)
		__builtin_trap();
	if (error == 0)
		adapter->tx_sequence++;
	adapter->starts_active--;
	if (error != 0)
		adapter->tx_busy = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return error;
}

static int
ncm_queue_datagram(const void *frame, size_t length, void *argument)
{
	struct ncm_adapter *adapter = argument;
	struct packet_buf *packet;
	void *destination;
	unsigned long irq;

	if (length < DRV_USB_CDC_NCM_ETHERNET_HEADER_SIZE ||
	    length > DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE)
		return EINVAL;
	packet = packet_buf_alloc(0);
	if (packet == NULL)
		return ENOBUFS;
	destination = packet_buf_append(packet, length);
	if (destination == NULL) {
		packet_buf_free(packet);
		return EMSGSIZE;
	}
	memcpy(destination, frame, length);
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->rx_queue_count >= NCM_RX_QUEUE_MAX) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);
		return ENOBUFS;
	}
	adapter->rx_queue[(adapter->rx_queue_head +
	    adapter->rx_queue_count) % NCM_RX_QUEUE_MAX] = packet;
	adapter->rx_queue_count++;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return 0;
}

static void
ncm_notification_process(struct ncm_adapter *adapter)
{
	const uint8_t *notification = adapter->notification_buffer;
	size_t length = drv_usb_urb_actual_length(adapter->notification_urb);
	uint16_t interface_number;

	if (drv_usb_urb_status(adapter->notification_urb) !=
	    DRV_USB_URB_COMPLETE || length < 8U || notification[0] !=
	    (DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE))
		return;
	/* Some NCM functions, including RTL8156 configuration 2, name the
	 * associated data interface rather than the communication interface.
	 * The binding has already validated this exact control/data pair. */
	interface_number = ncm_le16(notification + 4U);
	if (interface_number != drv_usb_interface_number(adapter->control) &&
	    interface_number != drv_usb_interface_number(adapter->data))
		return;
	if (notification[1] == NCM_NOTIFICATION_NETWORK_CONNECTION &&
	    ncm_le16(notification + 6U) == 0U && length == 8U)
		(void)net_device_set_carrier(adapter->net_device,
		    ncm_le16(notification + 2U) != 0U);
	else if (notification[1] == NCM_NOTIFICATION_SPEED_CHANGE &&
	    ncm_le16(notification + 2U) == 0U &&
	    ncm_le16(notification + 6U) == 8U && length == 16U) {
		unsigned long irq = spin_lock_irqsave(&adapter->lock);

		adapter->downstream_bps = ncm_le32(notification + 8U);
		adapter->upstream_bps = ncm_le32(notification + 12U);
		spin_unlock_irqrestore(&adapter->lock, irq);
	}
}

static int
ncm_rearm(struct ncm_adapter *adapter, int notification)
{
	struct drv_usb_urb *urb = notification ? adapter->notification_urb :
	    adapter->rx_urb;
	void *buffer = notification ? (void *)adapter->notification_buffer :
	    (void *)adapter->rx_buffer;
	size_t length = notification ? NCM_NOTIFICATION_SIZE :
	    adapter->profile.ntb_in_max_size;
	int error = ncm_start_urb(adapter, urb, buffer, length);
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	unsigned *retry = notification ? &adapter->notification_retries :
	    &adapter->rx_retries;
	unsigned *pending = notification ? &adapter->notification_rearm :
	    &adapter->rx_rearm;
	unsigned *active = notification ? &adapter->notification_rearm_active :
	    &adapter->rx_rearm_active;
	int retryable = error == EBUSY || error == ENOMEM || error == EAGAIN ||
	    error == ENOBUFS;
	int retry_scheduled = 0;
	int quarantine = 0;
	int stopping = !adapter->ready || !adapter->opened || adapter->closing ||
	    adapter->stopping;

	if (*active == 0)
		__builtin_trap();
	if (error == 0) {
		/* The caller atomically claimed the old rearm request.  Preserve a new
		 * one that an immediately completed submission may already have
		 * published through another poll. */
		*retry = 0;
	} else if (stopping) {
		/* A poll admitted immediately before close may reach rearm after
		 * close has withdrawn admission.  This is orderly retirement, not a
		 * transfer failure and must not quarantine the adapter. */
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
	if (quarantine && adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);
	if (retry_scheduled)
		return EAGAIN;
	/* EAGAIN is the internal retry signal.  Once its bounded budget has been
	 * exhausted, return a terminal error so the caller cannot reschedule the
	 * quarantined adapter forever. */
	if (quarantine && error == EAGAIN)
		return EIO;
	return error;
}

static unsigned
ncm_deliver_queued(struct ncm_adapter *adapter, unsigned budget)
{
	unsigned delivered = 0;

	while (delivered < budget) {
		struct packet_buf *packet;
		unsigned long irq = spin_lock_irqsave(&adapter->lock);

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
	return delivered;
}

static int
ncm_poll_enter(struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int admitted = adapter->ready && adapter->opened && !adapter->closing &&
	    !adapter->stopping && !adapter->quarantined;

	if (admitted)
		adapter->polls_active++;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return admitted;
}

static void
ncm_poll_exit(struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	if (adapter->polls_active == 0)
		__builtin_trap();
	adapter->polls_active--;
	spin_unlock_irqrestore(&adapter->lock, irq);
}

static int
ncm_take_pending(struct ncm_adapter *adapter, unsigned *pending)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int taken = *pending != 0;

	if (taken)
		*pending = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return taken;
}

static void
ncm_restore_pending(struct ncm_adapter *adapter, unsigned *pending)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	*pending = 1;
	spin_unlock_irqrestore(&adapter->lock, irq);
}

static int
ncm_take_notification_rearm(struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int taken = adapter->notification_rearm != 0 &&
	    adapter->notification_rearm_active == 0;

	if (taken) {
		adapter->notification_rearm = 0;
		adapter->notification_rearm_active = 1;
	}
	spin_unlock_irqrestore(&adapter->lock, irq);
	return taken;
}

static int
ncm_take_rx_rearm(struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int taken = adapter->rx_rearm != 0 &&
	    adapter->rx_rearm_active == 0 && adapter->rx_queue_count == 0;

	if (taken) {
		adapter->rx_rearm = 0;
		adapter->rx_rearm_active = 1;
	}
	spin_unlock_irqrestore(&adapter->lock, irq);
	return taken;
}

static int
ncm_poll_tx_completion(struct ncm_adapter *adapter)
{
	unsigned long irq;

	if (!ncm_take_pending(adapter, &adapter->tx_ready))
		return 0;
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->starts_active != 0) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		ncm_restore_pending(adapter, &adapter->tx_ready);
		return 0;
	}
	spin_unlock_irqrestore(&adapter->lock, irq);
	if (drv_usb_urb_drain(adapter->tx_urb,
	    NCM_TRANSFER_TIMEOUT_MS) != 0) {
		ncm_restore_pending(adapter, &adapter->tx_ready);
		return 0;
	}
	irq = spin_lock_irqsave(&adapter->lock);
	adapter->tx_busy = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return 1;
}

static int
ncm_poll_notification_completion(struct ncm_adapter *adapter)
{
	if (!ncm_take_pending(adapter, &adapter->notification_ready))
		return 0;
	if (drv_usb_urb_drain(adapter->notification_urb,
	    NCM_TRANSFER_TIMEOUT_MS) != 0) {
		ncm_restore_pending(adapter, &adapter->notification_ready);
		return 0;
	}
	ncm_notification_process(adapter);
	ncm_restore_pending(adapter, &adapter->notification_rearm);
	return 1;
}

static int
ncm_poll_rx_completion(struct net_device *device,
	struct ncm_adapter *adapter)
{
	size_t count = 0;
	int error;

	if (!ncm_take_pending(adapter, &adapter->rx_ready))
		return 0;
	if (drv_usb_urb_drain(adapter->rx_urb,
	    NCM_TRANSFER_TIMEOUT_MS) != 0) {
		ncm_restore_pending(adapter, &adapter->rx_ready);
		return 0;
	}
	error = ncm_urb_status_error(drv_usb_urb_status(adapter->rx_urb));
	if (error == 0)
		error = drv_usb_cdc_ncm_parse_ntb16(&adapter->profile,
		    &adapter->rx_state, adapter->rx_buffer,
		    drv_usb_urb_actual_length(adapter->rx_urb),
		    ncm_queue_datagram, adapter, &count);
	if (error != 0)
		device->rx_errors++;
	ncm_restore_pending(adapter, &adapter->rx_rearm);
	return 1;
}

static int
ncm_has_poll_work(struct ncm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int pending = adapter->ready && adapter->opened && !adapter->closing &&
	    !adapter->stopping && !adapter->quarantined &&
	    (adapter->notification_ready || adapter->rx_ready ||
	    adapter->tx_ready || (adapter->notification_rearm &&
	    !adapter->notification_rearm_active) || (adapter->rx_rearm &&
	    !adapter->rx_rearm_active) || adapter->rx_queue_count != 0);

	spin_unlock_irqrestore(&adapter->lock, irq);
	return pending;
}

static unsigned
ncm_poll_receive(struct net_device *device, unsigned budget)
{
	struct ncm_adapter *adapter = device->driver_data;
	unsigned cursor, delivered, index, work = 0;
	unsigned long irq;
	int notification_completed = 0, rx_progress = 0;

	if (!ncm_poll_enter(adapter))
		return 0;
	delivered = ncm_deliver_queued(adapter, budget);
	work += delivered;
	rx_progress = delivered != 0;

	irq = spin_lock_irqsave(&adapter->lock);
	cursor = adapter->poll_cursor % NCM_COMPLETION_KINDS;
	spin_unlock_irqrestore(&adapter->lock, irq);
	for (index = 0; index < NCM_COMPLETION_KINDS && work < budget;
	    index++) {
		unsigned kind = (cursor + index) % NCM_COMPLETION_KINDS;
		int completed;

		if (kind == NCM_COMPLETION_TX)
			completed = ncm_poll_tx_completion(adapter);
		else if (kind == NCM_COMPLETION_NOTIFICATION)
			completed = ncm_poll_notification_completion(adapter);
		else
			completed = ncm_poll_rx_completion(device, adapter);
		if (!completed)
			continue;
		if (kind == NCM_COMPLETION_NOTIFICATION)
			notification_completed = 1;
		else if (kind == NCM_COMPLETION_RX)
			rx_progress = 1;
		work++;
		irq = spin_lock_irqsave(&adapter->lock);
		adapter->poll_cursor = (kind + 1U) % NCM_COMPLETION_KINDS;
		spin_unlock_irqrestore(&adapter->lock, irq);
	}
	if (work < budget) {
		delivered = ncm_deliver_queued(adapter, budget - work);
		work += delivered;
		if (delivered != 0)
			rx_progress = 1;
	}

	/* A rearm caused by a completion (or by draining the last queued frame)
	 * is part of that already-budgeted work item.  A standalone retry consumes
	 * one unit so persistent HCD backpressure cannot escape the poll budget. */
	if (notification_completed) {
		if (ncm_take_notification_rearm(adapter))
			(void)ncm_rearm(adapter, 1);
	} else if (work < budget &&
	    ncm_take_notification_rearm(adapter)) {
		(void)ncm_rearm(adapter, 1);
		work++;
	}
	if (rx_progress) {
		if (ncm_take_rx_rearm(adapter))
			(void)ncm_rearm(adapter, 0);
	} else if (work < budget && ncm_take_rx_rearm(adapter)) {
		(void)ncm_rearm(adapter, 0);
		work++;
	}
	if (ncm_has_poll_work(adapter))
		net_device_schedule_poll(device);
	ncm_poll_exit(adapter);
	return work;
}

static void
ncm_release(void *driver_data)
{
	hal_free(driver_data);
}

static void
ncm_set_ready(struct ncm_adapter *adapter, int ready)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	adapter->ready = ready != 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
}

static const struct net_device_ops ncm_net_ops = {
	.open = ncm_open,
	.close = ncm_close,
	.transmit = ncm_transmit,
	.poll_receive = ncm_poll_receive,
	.release = ncm_release
};

static void
ncm_urbs_free(struct ncm_adapter *adapter)
{
	drv_usb_urb_free(adapter->tx_urb);
	drv_usb_urb_free(adapter->rx_urb);
	drv_usb_urb_free(adapter->notification_urb);
	adapter->tx_urb = NULL;
	adapter->rx_urb = NULL;
	adapter->notification_urb = NULL;
}

static int
ncm_urbs_alloc(struct ncm_adapter *adapter)
{
	adapter->notification_urb = drv_usb_urb_alloc(adapter->usb_device,
	    adapter->notification_endpoint, 0);
	adapter->rx_urb = drv_usb_urb_alloc(adapter->usb_device,
	    adapter->bulk_in, 0);
	adapter->tx_urb = drv_usb_urb_alloc(adapter->usb_device,
	    adapter->bulk_out, 0);
	if (adapter->notification_urb != NULL && adapter->rx_urb != NULL &&
	    adapter->tx_urb != NULL)
		return 0;
	ncm_urbs_free(adapter);
	return ENOMEM;
}

static void
ncm_buffers_free(struct ncm_adapter *adapter)
{
	hal_free(adapter->tx_buffer);
	hal_free(adapter->rx_buffer);
	hal_free(adapter->notification_buffer);
	adapter->tx_buffer = NULL;
	adapter->rx_buffer = NULL;
	adapter->notification_buffer = NULL;
}

static int
ncm_buffers_alloc(struct ncm_adapter *adapter)
{
	adapter->notification_buffer = hal_malloc(NCM_NOTIFICATION_SIZE);
	adapter->rx_buffer = hal_malloc(adapter->profile.ntb_in_max_size);
	adapter->tx_buffer = hal_malloc(adapter->profile.ntb_out_max_size);
	if (adapter->notification_buffer != NULL && adapter->rx_buffer != NULL &&
	    adapter->tx_buffer != NULL)
		return 0;
	ncm_buffers_free(adapter);
	return ENOMEM;
}

static int
ncm_net_device_create(struct ncm_adapter *adapter, const uint8_t mac[6])
{
	struct net_device *device = net_device_alloc();
	unsigned index;
	int error = ENOSPC;

	if (device == NULL)
		return ENOSPC;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->mtu = DRV_USB_CDC_NCM_MTU;
	memcpy(device->hwaddr, mac, 6U);
	device->hwaddr_len = 6U;
	device->ops = &ncm_net_ops;
	device->driver_data = adapter;
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		device->name[0] = 'u';
		device->name[1] = 'e';
		device->name[2] = (char)('0' + index);
		device->name[3] = '\0';
		error = net_device_create(device);
		if (error != EEXIST)
			break;
	}
	if (error != 0) {
		/* The allocation owner, not the unpublished device, still owns the
		 * adapter on an attach error. */
		device->driver_data = NULL;
		net_device_destroy(device);
		return error;
	}
	adapter->net_device = device;
	return 0;
}

static int
ncm_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id)
{
	struct ncm_binding binding;
	struct ncm_adapter *adapter;
	struct drv_usb_cdc_ncm_limits limits;
	uint8_t parameters[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE];
	uint8_t mac[6];
	size_t actual = 0;
	int error;
	(void)id;

	if (!ncm_binding_parse(interface, &binding))
		return ENODEV;
	adapter = hal_malloc(sizeof(*adapter));
	if (adapter == NULL)
		return ENOMEM;
	memset(adapter, 0, sizeof(*adapter));
	adapter->usb_device = binding.device;
	adapter->control = binding.control;
	adapter->data = binding.data;
	adapter->notification_endpoint = binding.notification;
	adapter->bulk_in = binding.bulk_in;
	adapter->bulk_out = binding.bulk_out;
	adapter->data_alternate = binding.data_alternate;
	adapter->capabilities = binding.capabilities;
	spin_init(&adapter->lock, LOCK_RANK_DEVICE, "usb-cdc-ncm");
	/* The USB core owns failed-attach cleanup through the provisional binding.
	 * Publish partial driver state before acquiring any sibling or resource. */
	error = drv_usb_interface_set_driver_data(interface, adapter);
	if (error != 0) {
		hal_free(adapter);
		return error;
	}
	error = drv_usb_interface_claim(interface, binding.data);
	if (error != 0)
		return error;
	error = ncm_get_mac(&binding, mac);
	if (error != 0)
		return error;
	memset(&limits, 0, sizeof(limits));
	limits.ntb_in_max_size = NCM_NTB_BUFFER_SIZE;
	limits.ntb_out_max_size = NCM_NTB_BUFFER_SIZE;
	limits.rx_max_datagrams = NCM_RX_QUEUE_MAX;
	limits.tx_max_datagrams = 1U;
	limits.ndp_chain_max = DRV_USB_CDC_NCM_MAX_NDP_CHAIN;
	limits.bulk_out_max_packet_size =
	    drv_usb_endpoint_max_packet_size(binding.bulk_out);
	error = ncm_control(adapter,
	    DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE,
	    NCM_GET_NTB_PARAMETERS, 0, parameters, sizeof(parameters), &actual);
	if (error != 0 || actual != sizeof(parameters)) {
		error = error != 0 ? error : EIO;
		return error;
	}
	error = drv_usb_cdc_ncm_negotiate_nth16(parameters, actual, &limits,
	    &adapter->profile);
	if (error != 0)
		return error;
	error = ncm_program_profile(adapter);
	if (error != 0)
		return error;
	drv_usb_cdc_ncm_rx_reset(&adapter->rx_state);
	error = ncm_buffers_alloc(adapter);
	if (error != 0)
		return error;
	error = ncm_urbs_alloc(adapter);
	if (error != 0)
		return error;
	error = ncm_net_device_create(adapter, mac);
	if (error != 0)
		return error;
	/* Idle URBs deliberately retain the inactive data endpoints.  The p015
	 * interface transaction makes this the final fallible attach operation. */
	error = drv_usb_interface_set_alternate(binding.data,
	    binding.data_alternate);
	if (error != 0)
		return error;
	ncm_set_ready(adapter, 1);
	hal_printf("usb-cdc-ncm: %s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
	    adapter->net_device->name, mac[0], mac[1], mac[2], mac[3], mac[4],
	    mac[5]);
	return 0;
}

static int
ncm_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct ncm_adapter *adapter = drv_usb_interface_driver_data(interface);
	int error;

	if (adapter == NULL)
		return 0;
	ncm_set_ready(adapter, 0);
	error = ncm_stop(adapter);
	if (error != 0)
		return error;
	/* p015 permits allocated, completely drained URBs to retain inactive
	 * endpoint objects across SET_INTERFACE.  Keep the complete graph until
	 * this final normal-detach hardware transaction has succeeded. */
	if ((flags & (DRV_USB_DETACH_FORCE |
	    DRV_USB_DETACH_ATTACH_FAILED)) == 0) {
		error = drv_usb_interface_set_alternate(adapter->data, 0);
		if (error != 0)
			return error;
	}
	error = adapter->net_device != NULL ?
	    net_device_gone(adapter->net_device) : 0;
	if (error != 0)
		return error;
	ncm_urbs_free(adapter);
	ncm_buffers_free(adapter);
	if (adapter->net_device != NULL)
		net_device_destroy(adapter->net_device);
	else
		hal_free(adapter);
	return 0;
}

static void
ncm_shutdown(struct drv_usb_interface *interface)
{
	struct ncm_adapter *adapter = drv_usb_interface_driver_data(interface);

	if (adapter != NULL) {
		ncm_set_ready(adapter, 0);
		(void)ncm_stop(adapter);
	}
}

static int
ncm_match(struct drv_usb_interface *interface, const struct drv_usb_id *id)
{
	struct ncm_binding binding;
	(void)id;

	return ncm_binding_parse(interface, &binding) ? 100 : 0;
}

static const struct drv_usb_id ncm_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS |
	    DRV_USB_ID_IF_PROTOCOL,
	.interface_class = NCM_COMMUNICATION_CLASS,
	.interface_subclass = NCM_COMMUNICATION_SUBCLASS,
	.interface_protocol = NCM_COMMUNICATION_PROTOCOL
}};

static struct drv_usb_driver ncm_driver = {
	.name = "usb-cdc-ncm",
	.ids = ncm_ids,
	.id_count = sizeof(ncm_ids) / sizeof(ncm_ids[0]),
	.match = ncm_match,
	.attach = ncm_attach,
	.detach = ncm_detach,
	.shutdown = ncm_shutdown
};

int
drv_usb_cdc_ncm_driver_register(void)
{
	return drv_usb_driver_register(&ncm_driver);
}
