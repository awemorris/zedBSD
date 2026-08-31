/*
 * Integrated USB CDC ECM network driver
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/usb-cdc-ecm.h>
#include <drivers/usb.h>
#include <errno.h>
#include <hal/hal.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/sched.h>
#include <limits.h>
#include <string.h>

#define ECM_COMMUNICATION_CLASS 0x02U
#define ECM_COMMUNICATION_SUBCLASS 0x06U
#define ECM_COMMUNICATION_PROTOCOL 0x00U
#define ECM_DATA_CLASS 0x0aU
#define ECM_DATA_SUBCLASS 0x00U
#define ECM_DATA_PROTOCOL 0x00U

#define ECM_CS_INTERFACE 0x24U
#define ECM_HEADER_DESCRIPTOR 0x00U
#define ECM_UNION_DESCRIPTOR 0x06U
#define ECM_ETHERNET_DESCRIPTOR 0x0fU

#define ECM_SET_ETHERNET_PACKET_FILTER 0x43U
#define ECM_FILTER_ALL_MULTICAST 0x0002U
#define ECM_FILTER_DIRECTED 0x0004U
#define ECM_FILTER_BROADCAST 0x0008U

#define ECM_NOTIFICATION_NETWORK_CONNECTION 0x00U
#define ECM_NOTIFICATION_SPEED_CHANGE 0x2aU
#define ECM_NOTIFICATION_SIZE 16U
#define ECM_ETHERNET_HEADER_SIZE 14U
#define ECM_MTU 1500U
#define ECM_FRAME_SIZE (ECM_ETHERNET_HEADER_SIZE + ECM_MTU)
#define ECM_TRANSFER_TIMEOUT_MS 5000U
#define ECM_CONTROL_TIMEOUT_MS 1000U
#define ECM_REARM_RETRY_MAX 3U
#define ECM_COMPLETION_KINDS 3U

#define ECM_COMPLETION_TX 0U
#define ECM_COMPLETION_NOTIFICATION 1U
#define ECM_COMPLETION_RX 2U

struct ecm_binding {
	struct drv_usb_device *device;
	struct drv_usb_interface *control;
	struct drv_usb_interface *data;
	struct drv_usb_endpoint *notification;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	unsigned data_alternate;
	uint8_t mac_string;
	uint16_t max_segment_size;
};

struct ecm_adapter {
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
	struct spinlock lock;
	uint8_t *notification_buffer;
	uint8_t *rx_buffer;
	uint8_t *tx_buffer;
	unsigned data_alternate;
	uint16_t bulk_out_max_packet_size;
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
	int stop_error;
	uint32_t upstream_bps;
	uint32_t downstream_bps;
};

static uint16_t
ecm_le16(const uint8_t *bytes)
{
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t
ecm_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static struct drv_usb_configuration *
ecm_interface_configuration(struct drv_usb_interface *interface)
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
ecm_iad_covers(
	const struct drv_usb_interface_association_descriptor *iad,
	unsigned interface_number)
{
	return iad->interface_count != 0U &&
	    interface_number >= iad->first_interface &&
	    interface_number - iad->first_interface < iad->interface_count;
}

static int
ecm_iad_consistent(struct drv_usb_configuration *configuration,
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
		if (!ecm_iad_covers(iad, control_number) &&
		    !ecm_iad_covers(iad, data_number) &&
		    iad->first_interface != control_number &&
		    iad->first_interface != data_number)
			continue;
		if (association || iad->first_interface != control_number ||
		    iad->interface_count != 2U ||
		    iad->function_class != ECM_COMMUNICATION_CLASS ||
		    iad->function_subclass != ECM_COMMUNICATION_SUBCLASS ||
		    iad->function_protocol != ECM_COMMUNICATION_PROTOCOL ||
		    data_number != control_number + 1U)
			return 0;
		association = 1;
	}
	return 1;
}

static int
ecm_control_descriptors(const struct drv_usb_host_interface *alternate,
	unsigned control_number, unsigned *data_number, uint8_t *mac_string,
	uint16_t *max_segment_size)
{
	unsigned index, header = 0, union_descriptor = 0, ethernet = 0;

	for (index = 0;
	    index < drv_usb_host_interface_extra_count(alternate); index++) {
		const uint8_t *descriptor;
		size_t length;

		if (drv_usb_host_interface_extra(alternate, index,
		    (const void **)&descriptor, &length) != 0 || descriptor == NULL ||
		    length < 2U)
			return 0;
		if (descriptor[1] != ECM_CS_INTERFACE)
			continue;
		if (length < 3U || descriptor[0] != length)
			return 0;
		switch (descriptor[2]) {
		case ECM_HEADER_DESCRIPTOR:
			if (length != 5U || ++header != 1U ||
			    ecm_le16(descriptor + 3U) == 0)
				return 0;
			break;
		case ECM_UNION_DESCRIPTOR:
			if (header != 1U || length != 5U ||
			    ++union_descriptor != 1U ||
			    descriptor[3] != control_number ||
			    descriptor[4] == control_number)
				return 0;
			*data_number = descriptor[4];
			break;
		case ECM_ETHERNET_DESCRIPTOR:
			if (header != 1U || length != 13U || ++ethernet != 1U ||
			    descriptor[3] == 0 ||
			    ecm_le16(descriptor + 8U) < ECM_FRAME_SIZE)
				return 0;
			*mac_string = descriptor[3];
			*max_segment_size = ecm_le16(descriptor + 8U);
			break;
		default:
			break;
		}
	}
	return header == 1U && union_descriptor == 1U && ethernet == 1U;
}

static int
ecm_find_notification(const struct drv_usb_host_interface *alternate,
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
ecm_find_data_alternate(struct drv_usb_interface *data,
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
		    descriptor->interface_class != ECM_DATA_CLASS ||
		    descriptor->interface_subclass != ECM_DATA_SUBCLASS ||
		    descriptor->interface_protocol != ECM_DATA_PROTOCOL)
			return 0;
		if (descriptor->alternate_setting == 0U) {
			if (descriptor->endpoint_count != 0U || empty_found)
				return 0;
			empty_found = 1;
			continue;
		}
		if (descriptor->endpoint_count != 2U || bulk_found)
			return 0;
		for (endpoint_index = 0; endpoint_index < 2U;
		    endpoint_index++) {
			struct drv_usb_endpoint *endpoint =
			    drv_usb_host_interface_endpoint(alternate,
			    endpoint_index);

			if (drv_usb_endpoint_type(endpoint) != DRV_USB_TRANSFER_BULK)
				return 0;
			if (drv_usb_endpoint_is_input(endpoint)) {
				if (input != NULL)
					return 0;
				input = endpoint;
			} else {
				if (output != NULL)
					return 0;
				output = endpoint;
			}
		}
		if (input == NULL || output == NULL)
			return 0;
		bulk_found = 1;
		*bulk_in = input;
		*bulk_out = output;
		*alternate_result = descriptor->alternate_setting;
	}
	return empty_found && bulk_found;
}

static int
ecm_binding_parse(struct drv_usb_interface *control,
	struct ecm_binding *binding)
{
	struct drv_usb_configuration *configuration;
	const struct drv_usb_interface_descriptor *control_descriptor;
	const struct drv_usb_host_interface *control_alternate;
	unsigned control_number, data_number = UINT_MAX;
	uint8_t mac_string = 0;
	uint16_t max_segment_size = 0;

	memset(binding, 0, sizeof(*binding));
	control_descriptor = drv_usb_interface_descriptor(control);
	if (control_descriptor == NULL ||
	    control_descriptor->interface_class != ECM_COMMUNICATION_CLASS ||
	    control_descriptor->interface_subclass !=
	    ECM_COMMUNICATION_SUBCLASS ||
	    control_descriptor->interface_protocol !=
	    ECM_COMMUNICATION_PROTOCOL ||
	    drv_usb_interface_alternate_count(control) != 1U)
		return 0;
	if ((drv_usb_device_hcd_capabilities(
	    drv_usb_interface_device(control)) &
	    DRV_USB_HCD_CAP_CONCURRENT_URBS) == 0)
		return 0;
	configuration = ecm_interface_configuration(control);
	if (configuration == NULL)
		return 0;
	control_alternate = drv_usb_interface_alternate(control, 0);
	control_number = control_descriptor->interface_number;
	if (control_alternate == NULL ||
	    !ecm_control_descriptors(control_alternate, control_number,
	    &data_number, &mac_string, &max_segment_size) ||
	    !ecm_find_notification(control_alternate, &binding->notification))
		return 0;
	binding->data = drv_usb_configuration_find_interface(configuration,
	    data_number);
	if (binding->data == NULL ||
	    !ecm_iad_consistent(configuration, control_number, data_number) ||
	    !ecm_find_data_alternate(binding->data, &binding->bulk_in,
	    &binding->bulk_out, &binding->data_alternate))
		return 0;
	/* Descriptor decoding has already applied the speed-specific packet-size
	 * rules.  Keep these local checks because ECM later divides by bulk-OUT
	 * MPS and requires a complete eight-byte notification header. */
	if (drv_usb_endpoint_max_packet_size(binding->notification) < 8U ||
	    drv_usb_endpoint_max_packet_size(binding->bulk_in) == 0U ||
	    drv_usb_endpoint_max_packet_size(binding->bulk_out) == 0U)
		return 0;
	binding->device = drv_usb_interface_device(control);
	binding->control = control;
	binding->mac_string = mac_string;
	binding->max_segment_size = max_segment_size;
	return 1;
}

static int
ecm_hex(unsigned char character)
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
ecm_get_mac(const struct ecm_binding *binding, uint8_t mac[6])
{
	char string[13];
	unsigned index;
	int all_zero = 1;

	if (drv_usb_device_get_string(binding->device, binding->mac_string, 0,
	    string, sizeof(string)) != 0 || strlen(string) != 12U)
		return EINVAL;
	for (index = 0; index < 6U; index++) {
		int high = ecm_hex((unsigned char)string[index * 2U]);
		int low = ecm_hex((unsigned char)string[index * 2U + 1U]);

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
ecm_control(struct ecm_adapter *adapter, uint8_t request_type,
	uint8_t request, uint16_t value, void *buffer, size_t length,
	size_t *actual)
{
	return drv_usb_control(adapter->usb_device, request_type, request, value,
	    (uint16_t)drv_usb_interface_number(adapter->control), buffer, length,
	    ECM_CONTROL_TIMEOUT_MS, actual);
}

static int
ecm_program_packet_filter(struct ecm_adapter *adapter)
{
	size_t actual = 0;
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
	error = ecm_control(adapter, DRV_USB_DIR_OUT | DRV_USB_REQUEST_CLASS |
	    DRV_USB_RECIP_INTERFACE, ECM_SET_ETHERNET_PACKET_FILTER,
	    ECM_FILTER_DIRECTED | ECM_FILTER_ALL_MULTICAST |
	    ECM_FILTER_BROADCAST, NULL, 0, &actual);
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
ecm_urb_status_error(enum drv_usb_urb_status status)
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

static void
ecm_completion(struct drv_usb_urb *urb, void *argument)
{
	struct ecm_adapter *adapter = argument;
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	if (urb == adapter->notification_urb)
		adapter->notification_ready = 1;
	else if (urb == adapter->rx_urb)
		adapter->rx_ready = 1;
	else if (urb == adapter->tx_urb)
		adapter->tx_ready = 1;
	spin_unlock_irqrestore(&adapter->lock, irq);
	net_device_schedule_poll(adapter->net_device);
}

static int
ecm_start_urb(struct ecm_adapter *adapter, struct drv_usb_urb *urb,
	void *buffer, size_t length, unsigned flags)
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
	error = drv_usb_urb_setup(urb, buffer, length, flags, 0,
	    ecm_completion, adapter);
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
ecm_cancel_and_drain(struct drv_usb_urb *urb)
{
	enum drv_usb_urb_status status;

	if (urb == NULL)
		return 0;
	status = drv_usb_urb_status(urb);
	if (status == DRV_USB_URB_PENDING)
		(void)drv_usb_urb_cancel(urb);
	return drv_usb_urb_drain(urb, ECM_TRANSFER_TIMEOUT_MS);
}

static void
ecm_wait_activity(struct ecm_adapter *adapter)
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

static int
ecm_stop(struct ecm_adapter *adapter)
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
	ecm_wait_activity(adapter);
	candidate = ecm_cancel_and_drain(adapter->notification_urb);
	if (error == 0)
		error = candidate;
	candidate = ecm_cancel_and_drain(adapter->rx_urb);
	if (error == 0)
		error = candidate;
	candidate = ecm_cancel_and_drain(adapter->tx_urb);
	if (error == 0)
		error = candidate;
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
	} else {
		adapter->quarantined = 1;
	}
	adapter->stop_error = error;
	adapter->closing = 0;
	adapter->stopping = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	if (adapter->net_device != NULL)
		(void)net_device_set_carrier(adapter->net_device, 0);
	return error;
}

static int
ecm_open(struct net_device *device)
{
	struct ecm_adapter *adapter = device->driver_data;
	unsigned long irq;
	int error;

	irq = spin_lock_irqsave(&adapter->lock);
	if (!adapter->ready || adapter->opened || adapter->closing ||
	    adapter->stopping || adapter->quarantined) {
		error = !adapter->ready ? ENETDOWN :
		    (adapter->quarantined ? EIO : EBUSY);
		spin_unlock_irqrestore(&adapter->lock, irq);
		return error;
	}
	adapter->opened = 1;
	adapter->stop_error = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	error = ecm_program_packet_filter(adapter);
	if (error == 0)
		error = ecm_start_urb(adapter, adapter->notification_urb,
		    adapter->notification_buffer, ECM_NOTIFICATION_SIZE, 0);
	if (error == 0)
		error = ecm_start_urb(adapter, adapter->rx_urb,
		    adapter->rx_buffer, ECM_FRAME_SIZE, 0);
	if (error != 0) {
		(void)ecm_stop(adapter);
		return error;
	}
	return 0;
}

static void
ecm_close(struct net_device *device)
{
	struct ecm_adapter *adapter = device->driver_data;

	(void)ecm_stop(adapter);
}

static int
ecm_transmit(struct net_device *device, struct packet_buf *packet)
{
	struct ecm_adapter *adapter = device->driver_data;
	unsigned long irq;
	unsigned flags = 0;
	size_t length;
	int error = 0;

	if (packet == NULL)
		return EINVAL;
	length = packet->length;
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
	if (length < ECM_ETHERNET_HEADER_SIZE || length > ECM_FRAME_SIZE) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		packet_buf_free(packet);
		return EMSGSIZE;
	}
	adapter->tx_busy = 1;
	adapter->starts_active++;
	spin_unlock_irqrestore(&adapter->lock, irq);
	memcpy(adapter->tx_buffer, packet->data, length);
	packet_buf_free(packet);
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->closing || !adapter->opened)
		error = ENETDOWN;
	spin_unlock_irqrestore(&adapter->lock, irq);
	if (error == 0 && length != 0 &&
	    length % adapter->bulk_out_max_packet_size == 0)
		flags |= DRV_USB_URB_ZERO_PACKET;
	if (error == 0)
		error = drv_usb_urb_setup(adapter->tx_urb, adapter->tx_buffer,
		    length, flags, ECM_TRANSFER_TIMEOUT_MS, ecm_completion, adapter);
	if (error == 0)
		error = drv_usb_urb_submit(adapter->tx_urb);
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->starts_active == 0)
		__builtin_trap();
	adapter->starts_active--;
	if (error != 0)
		adapter->tx_busy = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return error;
}

static void
ecm_notification_process(struct ecm_adapter *adapter)
{
	const uint8_t *notification = adapter->notification_buffer;
	size_t length = drv_usb_urb_actual_length(adapter->notification_urb);
	uint16_t interface_number;

	if (drv_usb_urb_status(adapter->notification_urb) !=
	    DRV_USB_URB_COMPLETE || length < 8U || notification[0] !=
	    (DRV_USB_DIR_IN | DRV_USB_REQUEST_CLASS | DRV_USB_RECIP_INTERFACE))
		return;
	interface_number = ecm_le16(notification + 4U);
	if (interface_number != drv_usb_interface_number(adapter->control) &&
	    interface_number != drv_usb_interface_number(adapter->data))
		return;
	if (notification[1] == ECM_NOTIFICATION_NETWORK_CONNECTION &&
	    ecm_le16(notification + 6U) == 0U && length == 8U)
		(void)net_device_set_carrier(adapter->net_device,
		    ecm_le16(notification + 2U) != 0U);
	else if (notification[1] == ECM_NOTIFICATION_SPEED_CHANGE &&
	    ecm_le16(notification + 2U) == 0U &&
	    ecm_le16(notification + 6U) == 8U && length == 16U) {
		unsigned long irq = spin_lock_irqsave(&adapter->lock);

		adapter->downstream_bps = ecm_le32(notification + 8U);
		adapter->upstream_bps = ecm_le32(notification + 12U);
		spin_unlock_irqrestore(&adapter->lock, irq);
	}
}

static int
ecm_rearm(struct ecm_adapter *adapter, int notification)
{
	struct drv_usb_urb *urb = notification ? adapter->notification_urb :
	    adapter->rx_urb;
	void *buffer = notification ? (void *)adapter->notification_buffer :
	    (void *)adapter->rx_buffer;
	size_t length = notification ? ECM_NOTIFICATION_SIZE : ECM_FRAME_SIZE;
	int error = ecm_start_urb(adapter, urb, buffer, length, 0);
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
		*retry = 0;
	} else if (stopping) {
		*retry = 0;
		error = 0;
	} else if (retryable && *retry < ECM_REARM_RETRY_MAX) {
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
	if (quarantine && error == EAGAIN)
		return EIO;
	return error;
}

static int
ecm_poll_enter(struct ecm_adapter *adapter)
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
ecm_poll_exit(struct ecm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	if (adapter->polls_active == 0)
		__builtin_trap();
	adapter->polls_active--;
	spin_unlock_irqrestore(&adapter->lock, irq);
}

static int
ecm_take_pending(struct ecm_adapter *adapter, unsigned *pending)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int taken = *pending != 0;

	if (taken)
		*pending = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return taken;
}

static void
ecm_restore_pending(struct ecm_adapter *adapter, unsigned *pending)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	*pending = 1;
	spin_unlock_irqrestore(&adapter->lock, irq);
}

static int
ecm_take_rearm(struct ecm_adapter *adapter, int notification)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	unsigned *pending = notification ? &adapter->notification_rearm :
	    &adapter->rx_rearm;
	unsigned *active = notification ? &adapter->notification_rearm_active :
	    &adapter->rx_rearm_active;
	int taken = *pending != 0 && *active == 0;

	if (taken) {
		*pending = 0;
		*active = 1;
	}
	spin_unlock_irqrestore(&adapter->lock, irq);
	return taken;
}

static int
ecm_poll_tx_completion(struct ecm_adapter *adapter)
{
	unsigned long irq;

	if (!ecm_take_pending(adapter, &adapter->tx_ready))
		return 0;
	irq = spin_lock_irqsave(&adapter->lock);
	if (adapter->starts_active != 0) {
		spin_unlock_irqrestore(&adapter->lock, irq);
		ecm_restore_pending(adapter, &adapter->tx_ready);
		return 0;
	}
	spin_unlock_irqrestore(&adapter->lock, irq);
	if (drv_usb_urb_drain(adapter->tx_urb,
	    ECM_TRANSFER_TIMEOUT_MS) != 0) {
		ecm_restore_pending(adapter, &adapter->tx_ready);
		return 0;
	}
	irq = spin_lock_irqsave(&adapter->lock);
	adapter->tx_busy = 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
	return 1;
}

static int
ecm_poll_notification_completion(struct ecm_adapter *adapter)
{
	if (!ecm_take_pending(adapter, &adapter->notification_ready))
		return 0;
	if (drv_usb_urb_drain(adapter->notification_urb,
	    ECM_TRANSFER_TIMEOUT_MS) != 0) {
		ecm_restore_pending(adapter, &adapter->notification_ready);
		return 0;
	}
	ecm_notification_process(adapter);
	ecm_restore_pending(adapter, &adapter->notification_rearm);
	return 1;
}

static int
ecm_poll_rx_completion(struct net_device *device,
	struct ecm_adapter *adapter)
{
	struct packet_buf *packet = NULL;
	void *destination;
	size_t length;
	int error;

	if (!ecm_take_pending(adapter, &adapter->rx_ready))
		return 0;
	if (drv_usb_urb_drain(adapter->rx_urb,
	    ECM_TRANSFER_TIMEOUT_MS) != 0) {
		ecm_restore_pending(adapter, &adapter->rx_ready);
		return 0;
	}
	error = ecm_urb_status_error(drv_usb_urb_status(adapter->rx_urb));
	length = drv_usb_urb_actual_length(adapter->rx_urb);
	if (error == 0 && (length < ECM_ETHERNET_HEADER_SIZE ||
	    length > ECM_FRAME_SIZE))
		error = EMSGSIZE;
	if (error == 0) {
		packet = packet_buf_alloc(0);
		if (packet == NULL)
			error = ENOBUFS;
	}
	if (error == 0) {
		destination = packet_buf_append(packet, length);
		if (destination == NULL)
			error = EMSGSIZE;
		else
			memcpy(destination, adapter->rx_buffer, length);
	}
	if (error == 0)
		net_device_receive(device, packet);
	else {
		if (packet != NULL)
			packet_buf_free(packet);
		device->rx_errors++;
	}
	ecm_restore_pending(adapter, &adapter->rx_rearm);
	return 1;
}

static int
ecm_has_poll_work(struct ecm_adapter *adapter)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);
	int pending = adapter->ready && adapter->opened && !adapter->closing &&
	    !adapter->stopping && !adapter->quarantined &&
	    (adapter->notification_ready || adapter->rx_ready ||
	    adapter->tx_ready || (adapter->notification_rearm &&
	    !adapter->notification_rearm_active) || (adapter->rx_rearm &&
	    !adapter->rx_rearm_active));

	spin_unlock_irqrestore(&adapter->lock, irq);
	return pending;
}

static unsigned
ecm_poll_receive(struct net_device *device, unsigned budget)
{
	struct ecm_adapter *adapter = device->driver_data;
	unsigned cursor, index, work = 0;
	unsigned long irq;
	int notification_completed = 0, rx_completed = 0;

	if (!ecm_poll_enter(adapter))
		return 0;
	irq = spin_lock_irqsave(&adapter->lock);
	cursor = adapter->poll_cursor % ECM_COMPLETION_KINDS;
	spin_unlock_irqrestore(&adapter->lock, irq);
	for (index = 0; index < ECM_COMPLETION_KINDS && work < budget;
	    index++) {
		unsigned kind = (cursor + index) % ECM_COMPLETION_KINDS;
		int completed;

		if (kind == ECM_COMPLETION_TX)
			completed = ecm_poll_tx_completion(adapter);
		else if (kind == ECM_COMPLETION_NOTIFICATION)
			completed = ecm_poll_notification_completion(adapter);
		else
			completed = ecm_poll_rx_completion(device, adapter);
		if (!completed)
			continue;
		if (kind == ECM_COMPLETION_NOTIFICATION)
			notification_completed = 1;
		else if (kind == ECM_COMPLETION_RX)
			rx_completed = 1;
		work++;
		irq = spin_lock_irqsave(&adapter->lock);
		adapter->poll_cursor = (kind + 1U) % ECM_COMPLETION_KINDS;
		spin_unlock_irqrestore(&adapter->lock, irq);
	}
	if (notification_completed) {
		if (ecm_take_rearm(adapter, 1))
			(void)ecm_rearm(adapter, 1);
	} else if (work < budget && ecm_take_rearm(adapter, 1)) {
		(void)ecm_rearm(adapter, 1);
		work++;
	}
	if (rx_completed) {
		if (ecm_take_rearm(adapter, 0))
			(void)ecm_rearm(adapter, 0);
	} else if (work < budget && ecm_take_rearm(adapter, 0)) {
		(void)ecm_rearm(adapter, 0);
		work++;
	}
	if (ecm_has_poll_work(adapter))
		net_device_schedule_poll(device);
	ecm_poll_exit(adapter);
	return work;
}

static void
ecm_release(void *driver_data)
{
	hal_free(driver_data);
}

static void
ecm_set_ready(struct ecm_adapter *adapter, int ready)
{
	unsigned long irq = spin_lock_irqsave(&adapter->lock);

	adapter->ready = ready != 0;
	spin_unlock_irqrestore(&adapter->lock, irq);
}

static const struct net_device_ops ecm_net_ops = {
	.open = ecm_open,
	.close = ecm_close,
	.transmit = ecm_transmit,
	.poll_receive = ecm_poll_receive,
	.release = ecm_release
};

static void
ecm_urbs_free(struct ecm_adapter *adapter)
{
	drv_usb_urb_free(adapter->tx_urb);
	drv_usb_urb_free(adapter->rx_urb);
	drv_usb_urb_free(adapter->notification_urb);
	adapter->tx_urb = NULL;
	adapter->rx_urb = NULL;
	adapter->notification_urb = NULL;
}

static int
ecm_urbs_alloc(struct ecm_adapter *adapter)
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
	ecm_urbs_free(adapter);
	return ENOMEM;
}

static void
ecm_buffers_free(struct ecm_adapter *adapter)
{
	hal_free(adapter->tx_buffer);
	hal_free(adapter->rx_buffer);
	hal_free(adapter->notification_buffer);
	adapter->tx_buffer = NULL;
	adapter->rx_buffer = NULL;
	adapter->notification_buffer = NULL;
}

static int
ecm_buffers_alloc(struct ecm_adapter *adapter)
{
	adapter->notification_buffer = hal_malloc(ECM_NOTIFICATION_SIZE);
	adapter->rx_buffer = hal_malloc(ECM_FRAME_SIZE);
	adapter->tx_buffer = hal_malloc(ECM_FRAME_SIZE);
	if (adapter->notification_buffer != NULL && adapter->rx_buffer != NULL &&
	    adapter->tx_buffer != NULL)
		return 0;
	ecm_buffers_free(adapter);
	return ENOMEM;
}

static int
ecm_net_device_create(struct ecm_adapter *adapter, const uint8_t mac[6])
{
	struct net_device *device = net_device_alloc();
	unsigned index;
	int error = ENOSPC;

	if (device == NULL)
		return ENOSPC;
	device->flags = NET_DEVICE_BROADCAST | NET_DEVICE_MULTICAST;
	device->mtu = ECM_MTU;
	memcpy(device->hwaddr, mac, 6U);
	device->hwaddr_len = 6U;
	device->ops = &ecm_net_ops;
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
		device->driver_data = NULL;
		net_device_destroy(device);
		return error;
	}
	adapter->net_device = device;
	return 0;
}

static int
ecm_attach(struct drv_usb_interface *interface, const struct drv_usb_id *id)
{
	struct ecm_binding binding;
	struct ecm_adapter *adapter;
	uint8_t mac[6];
	int error;
	(void)id;

	if (!ecm_binding_parse(interface, &binding))
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
	adapter->bulk_out_max_packet_size =
	    drv_usb_endpoint_max_packet_size(binding.bulk_out);
	spin_init(&adapter->lock, LOCK_RANK_DEVICE, "usb-cdc-ecm");
	error = drv_usb_interface_set_driver_data(interface, adapter);
	if (error != 0) {
		hal_free(adapter);
		return error;
	}
	error = drv_usb_interface_claim(interface, binding.data);
	if (error != 0)
		return error;
	error = ecm_get_mac(&binding, mac);
	if (error != 0)
		return error;
	error = ecm_buffers_alloc(adapter);
	if (error != 0)
		return error;
	error = ecm_urbs_alloc(adapter);
	if (error != 0)
		return error;
	error = ecm_net_device_create(adapter, mac);
	if (error != 0)
		return error;
	error = drv_usb_interface_set_alternate(binding.data,
	    binding.data_alternate);
	if (error != 0)
		return error;
	ecm_set_ready(adapter, 1);
	hal_printf("usb-cdc-ecm: %s mac=%02x:%02x:%02x:%02x:%02x:%02x "
	    "segment=%u\n", adapter->net_device->name, mac[0], mac[1], mac[2],
	    mac[3], mac[4], mac[5], binding.max_segment_size);
	return 0;
}

static int
ecm_detach(struct drv_usb_interface *interface, unsigned flags)
{
	struct ecm_adapter *adapter = drv_usb_interface_driver_data(interface);
	int error;

	if (adapter == NULL)
		return 0;
	ecm_set_ready(adapter, 0);
	error = ecm_stop(adapter);
	if (error != 0)
		return error;
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
	ecm_urbs_free(adapter);
	ecm_buffers_free(adapter);
	if (adapter->net_device != NULL)
		net_device_destroy(adapter->net_device);
	else
		hal_free(adapter);
	return 0;
}

static void
ecm_shutdown(struct drv_usb_interface *interface)
{
	struct ecm_adapter *adapter = drv_usb_interface_driver_data(interface);

	if (adapter != NULL) {
		ecm_set_ready(adapter, 0);
		(void)ecm_stop(adapter);
	}
}

static int
ecm_match(struct drv_usb_interface *interface, const struct drv_usb_id *id)
{
	struct ecm_binding binding;
	(void)id;

	/* NCM returns 100.  A function offering both standards configurations
	 * retains the richer NCM choice while an ECM-only configuration remains
	 * eligible. */
	return ecm_binding_parse(interface, &binding) ? 80 : 0;
}

static const struct drv_usb_id ecm_ids[] = {{
	.match_flags = DRV_USB_ID_IF_CLASS | DRV_USB_ID_IF_SUBCLASS |
	    DRV_USB_ID_IF_PROTOCOL,
	.interface_class = ECM_COMMUNICATION_CLASS,
	.interface_subclass = ECM_COMMUNICATION_SUBCLASS,
	.interface_protocol = ECM_COMMUNICATION_PROTOCOL
}};

static struct drv_usb_driver ecm_driver = {
	.name = "usb-cdc-ecm",
	.ids = ecm_ids,
	.id_count = sizeof(ecm_ids) / sizeof(ecm_ids[0]),
	.match = ecm_match,
	.attach = ecm_attach,
	.detach = ecm_detach,
	.shutdown = ecm_shutdown
};

int
drv_usb_cdc_ecm_driver_register(void)
{
	return drv_usb_driver_register(&ecm_driver);
}
