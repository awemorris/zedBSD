/* WS004 p014 production CDC NCM driver fixture. SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#define sched_yield host_sched_yield_declaration
#include <pthread.h>
#undef sched_yield
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../../src/drivers/usb-cdc-ncm-net.c"

static unsigned checks;
#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "check %u failed at %s:%d: %s\n", checks, \
		    __FILE__, __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

struct drv_usb_endpoint {
	struct drv_usb_interface *interface;
	enum drv_usb_transfer_type type;
	uint8_t address;
	uint16_t max_packet;
};

struct drv_usb_host_interface {
	struct drv_usb_interface_descriptor descriptor;
	struct drv_usb_endpoint endpoints[2];
	unsigned endpoint_count;
	const uint8_t *extras[4];
	size_t extra_lengths[4];
	unsigned extra_count;
};

struct drv_usb_interface {
	struct drv_usb_device *device;
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface_descriptor descriptor;
	struct drv_usb_host_interface alternates[2];
	unsigned alternate_count;
	unsigned active_index;
	struct drv_usb_interface *claimed_by;
	void *driver_data;
};

struct drv_usb_configuration {
	struct drv_usb_device *device;
	struct drv_usb_interface interfaces[2];
	struct drv_usb_interface_association_descriptor iad;
};

struct drv_usb_device {
	struct drv_usb_configuration configuration;
	unsigned capabilities;
	char mac[32];
	uint8_t parameters[DRV_USB_CDC_NCM_NTB_PARAMETERS_SIZE];
	uint8_t ncm_extra[6];
	unsigned controls[32];
	unsigned control_count;
	unsigned control_attempts;
	unsigned fail_control_at;
	unsigned alternate_attempts;
	unsigned fail_alternate_at;
	unsigned auto_complete_notification;
	unsigned fail_submit_once;
	int submit_error;
	unsigned submit_error_count;
	unsigned live_urbs;
	unsigned data_pending_urbs;
	unsigned alt1_live_urbs;
	unsigned alt1_registry_count;
	unsigned alt1_has_driver_data;
	unsigned alt1_adapter_ready;
};

struct drv_usb_urb {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	void *buffer;
	size_t length;
	size_t actual;
	enum drv_usb_urb_status status;
	drv_usb_urb_callback_t callback;
	void *callback_argument;
	unsigned hcd_owned;
};

static size_t hal_live;
static size_t hal_attempts;
static size_t hal_fail_at;
static unsigned live_urbs;
static unsigned pending_urbs;
static unsigned drain_fail_once;
static unsigned packet_live;
static unsigned packet_frees;
static unsigned received_packets;
static uint8_t received_frame[DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE];
static size_t received_length;
static struct net_device *published_device;
static struct net_device *device_registry[NET_DEVICE_MAX];
static unsigned device_registry_count;
static struct drv_usb_driver *registered_driver;
static int create_open_result;
static unsigned claim_calls;
static unsigned release_calls;
static unsigned driver_data_set_calls;
static unsigned driver_data_clear_calls;
static unsigned net_device_gone_fail_once;
static _Thread_local unsigned poll_fixture_thread;
static atomic_int block_poll_drain;
static atomic_int poll_drain_entered;
static atomic_int poll_thread_returned;
static atomic_int detach_thread_started;
static atomic_int detach_thread_returned;
static atomic_int detach_thread_result;

static void
fixture_thread_yield(void)
{
	const struct timespec delay = {0, 100000L};

	(void)nanosleep(&delay, NULL);
}

void *
hal_malloc(size_t size)
{
	void *pointer;

	hal_attempts++;
	if (hal_fail_at != 0 && hal_attempts == hal_fail_at)
		return NULL;
	pointer = malloc(size);
	if (pointer != NULL)
		hal_live++;
	return pointer;
}

void
hal_free(void *pointer)
{
	if (pointer == NULL)
		return;
	CHECK(hal_live != 0);
	hal_live--;
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	(void)format;
	return 0;
}

void
sched_yield(void)
{
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

void
spin_lock(struct spinlock *lock)
{
	while (__atomic_exchange_n(&lock->held.value, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		;
}

int
spin_trylock(struct spinlock *lock)
{
	unsigned expected = 0;

	return __atomic_compare_exchange_n(&lock->held.value, &expected, 1U, 0,
	    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void
spin_unlock(struct spinlock *lock)
{
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	spin_lock(lock);
	return 1;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	spin_unlock(lock);
}

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static size_t
make_two_datagram_ntb(uint8_t *ntb, uint16_t sequence,
	const uint8_t frame[64])
{
	memset(ntb, 0, 162);
	put_le32(ntb, DRV_USB_CDC_NCM_NTH16_SIGNATURE);
	put_le16(ntb + 4U, DRV_USB_CDC_NCM_NTH16_SIZE);
	put_le16(ntb + 6U, sequence);
	put_le16(ntb + 8U, 162);
	put_le16(ntb + 10U, 12);
	put_le32(ntb + 12U, DRV_USB_CDC_NCM_NDP16_NOCRC_SIGNATURE);
	put_le16(ntb + 16U, 20);
	put_le16(ntb + 20U, 34);
	put_le16(ntb + 22U, 64);
	put_le16(ntb + 24U, 98);
	put_le16(ntb + 26U, 64);
	memcpy(ntb + 34U, frame, 64);
	memcpy(ntb + 98U, frame, 64);
	return 162;
}

static const uint8_t header_descriptor[] = {5, 0x24, 0, 0x10, 0x01};
static const uint8_t union_descriptor[] = {5, 0x24, 6, 0, 1};
static const uint8_t ethernet_descriptor[] = {
	13, 0x24, 0x0f, 3, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0
};
static const uint8_t ncm_descriptor[] = {
	6, 0x24, 0x1a, 0x00, 0x01,
	NCM_CAP_PACKET_FILTER | NCM_CAP_MAX_DATAGRAM_SIZE | NCM_CAP_CRC_MODE
};
static const uint8_t truncated_cs_descriptor[] = {2, 0x24};

static void
function_init(struct drv_usb_device *device, unsigned capabilities)
{
	struct drv_usb_configuration *configuration;
	struct drv_usb_interface *control, *data;
	struct drv_usb_host_interface *alternate;

	memset(device, 0, sizeof(*device));
	device->capabilities = capabilities;
	strcpy(device->mac, "001122AABBCC");
	put_le16(device->parameters, sizeof(device->parameters));
	put_le16(device->parameters + 2U,
	    DRV_USB_CDC_NCM_NTB16_SUPPORTED |
	    DRV_USB_CDC_NCM_NTB32_SUPPORTED);
	put_le32(device->parameters + 4U, NCM_NTB_BUFFER_SIZE);
	put_le16(device->parameters + 8U, 4);
	put_le16(device->parameters + 10U, 0);
	put_le16(device->parameters + 12U, 4);
	put_le32(device->parameters + 16U, NCM_NTB_BUFFER_SIZE);
	put_le16(device->parameters + 20U, 4);
	put_le16(device->parameters + 22U, 0);
	put_le16(device->parameters + 24U, 4);
	put_le16(device->parameters + 26U, NCM_RX_QUEUE_MAX);
	configuration = &device->configuration;
	configuration->device = device;
	configuration->iad.length = 8;
	configuration->iad.descriptor_type =
	    DRV_USB_DESCRIPTOR_INTERFACE_ASSOCIATION;
	configuration->iad.first_interface = 0;
	configuration->iad.interface_count = 2;
	configuration->iad.function_class = NCM_COMMUNICATION_CLASS;
	configuration->iad.function_subclass = NCM_COMMUNICATION_SUBCLASS;
	configuration->iad.function_protocol = NCM_COMMUNICATION_PROTOCOL;
	control = &configuration->interfaces[0];
	control->device = device;
	control->configuration = configuration;
	control->alternate_count = 1;
	alternate = &control->alternates[0];
	alternate->descriptor.length = 9;
	alternate->descriptor.descriptor_type = DRV_USB_DESCRIPTOR_INTERFACE;
	alternate->descriptor.interface_number = 0;
	alternate->descriptor.endpoint_count = 1;
	alternate->descriptor.interface_class = NCM_COMMUNICATION_CLASS;
	alternate->descriptor.interface_subclass = NCM_COMMUNICATION_SUBCLASS;
	alternate->descriptor.interface_protocol = NCM_COMMUNICATION_PROTOCOL;
	alternate->endpoint_count = 1;
	alternate->endpoints[0].interface = control;
	alternate->endpoints[0].type = DRV_USB_TRANSFER_INTERRUPT;
	alternate->endpoints[0].address = 0x83;
	alternate->endpoints[0].max_packet = NCM_NOTIFICATION_SIZE;
	alternate->extras[0] = header_descriptor;
	alternate->extra_lengths[0] = sizeof(header_descriptor);
	alternate->extras[1] = union_descriptor;
	alternate->extra_lengths[1] = sizeof(union_descriptor);
	alternate->extras[2] = ethernet_descriptor;
	alternate->extra_lengths[2] = sizeof(ethernet_descriptor);
	memcpy(device->ncm_extra, ncm_descriptor, sizeof(device->ncm_extra));
	alternate->extras[3] = device->ncm_extra;
	alternate->extra_lengths[3] = sizeof(ncm_descriptor);
	alternate->extra_count = 4;
	control->descriptor = alternate->descriptor;
	data = &configuration->interfaces[1];
	data->device = device;
	data->configuration = configuration;
	data->alternate_count = 2;
	alternate = &data->alternates[0];
	alternate->descriptor.length = 9;
	alternate->descriptor.descriptor_type = DRV_USB_DESCRIPTOR_INTERFACE;
	alternate->descriptor.interface_number = 1;
	alternate->descriptor.interface_class = NCM_DATA_CLASS;
	alternate->descriptor.interface_subclass = NCM_DATA_SUBCLASS;
	alternate->descriptor.interface_protocol = NCM_DATA_PROTOCOL;
	alternate = &data->alternates[1];
	alternate->descriptor = data->alternates[0].descriptor;
	alternate->descriptor.alternate_setting = 1;
	alternate->descriptor.endpoint_count = 2;
	alternate->endpoint_count = 2;
	alternate->endpoints[0].interface = data;
	alternate->endpoints[0].type = DRV_USB_TRANSFER_BULK;
	alternate->endpoints[0].address = 0x84;
	alternate->endpoints[0].max_packet = 512;
	alternate->endpoints[1].interface = data;
	alternate->endpoints[1].type = DRV_USB_TRANSFER_BULK;
	alternate->endpoints[1].address = 0x05;
	alternate->endpoints[1].max_packet = 512;
	data->descriptor = data->alternates[0].descriptor;
}

unsigned
drv_usb_device_configuration_count(const struct drv_usb_device *device)
{
	return device == NULL ? 0 : 1;
}

struct drv_usb_configuration *
drv_usb_device_configuration(struct drv_usb_device *device, unsigned index)
{
	return device != NULL && index == 0 ? &device->configuration : NULL;
}

struct drv_usb_configuration *
drv_usb_device_active_configuration(struct drv_usb_device *device)
{
	return device == NULL ? NULL : &device->configuration;
}

unsigned
drv_usb_configuration_interface_count(const struct drv_usb_configuration *c)
{
	return c == NULL ? 0 : 2;
}

struct drv_usb_interface *
drv_usb_configuration_interface(struct drv_usb_configuration *c,
	unsigned index)
{
	return c != NULL && index < 2 ? &c->interfaces[index] : NULL;
}

struct drv_usb_interface *
drv_usb_configuration_find_interface(struct drv_usb_configuration *c,
	unsigned number)
{
	unsigned index;

	for (index = 0; c != NULL && index < 2; index++)
		if (c->interfaces[index].descriptor.interface_number == number)
			return &c->interfaces[index];
	return NULL;
}

unsigned
drv_usb_configuration_iad_count(const struct drv_usb_configuration *c)
{
	return c == NULL ? 0 : 1;
}

const struct drv_usb_interface_association_descriptor *
drv_usb_configuration_iad(const struct drv_usb_configuration *c,
	unsigned index)
{
	return c != NULL && index == 0 ? &c->iad : NULL;
}

struct drv_usb_device *
drv_usb_interface_device(const struct drv_usb_interface *interface)
{
	return interface == NULL ? NULL : interface->device;
}

const struct drv_usb_interface_descriptor *
drv_usb_interface_descriptor(const struct drv_usb_interface *interface)
{
	return interface == NULL ? NULL : &interface->descriptor;
}

unsigned
drv_usb_interface_number(const struct drv_usb_interface *interface)
{
	return interface == NULL ? 0 : interface->descriptor.interface_number;
}

unsigned
drv_usb_interface_alternate_count(const struct drv_usb_interface *interface)
{
	return interface == NULL ? 0 : interface->alternate_count;
}

const struct drv_usb_host_interface *
drv_usb_interface_alternate(const struct drv_usb_interface *interface,
	unsigned index)
{
	return interface != NULL && index < interface->alternate_count ?
	    &interface->alternates[index] : NULL;
}

const struct drv_usb_host_interface *
drv_usb_interface_active_alternate(const struct drv_usb_interface *interface)
{
	return interface != NULL ? &interface->alternates[interface->active_index] :
	    NULL;
}

const struct drv_usb_interface_descriptor *
drv_usb_host_interface_descriptor(const struct drv_usb_host_interface *host)
{
	return host == NULL ? NULL : &host->descriptor;
}

unsigned
drv_usb_host_interface_endpoint_count(const struct drv_usb_host_interface *h)
{
	return h == NULL ? 0 : h->endpoint_count;
}

struct drv_usb_endpoint *
drv_usb_host_interface_endpoint(const struct drv_usb_host_interface *h,
	unsigned index)
{
	return h != NULL && index < h->endpoint_count ?
	    (struct drv_usb_endpoint *)&h->endpoints[index] : NULL;
}

unsigned
drv_usb_host_interface_extra_count(const struct drv_usb_host_interface *h)
{
	return h == NULL ? 0 : h->extra_count;
}

int
drv_usb_host_interface_extra(const struct drv_usb_host_interface *h,
	unsigned index, const void **descriptor, size_t *length)
{
	if (h == NULL || index >= h->extra_count || descriptor == NULL ||
	    length == NULL)
		return EINVAL;
	*descriptor = h->extras[index];
	*length = h->extra_lengths[index];
	return 0;
}

enum drv_usb_transfer_type
drv_usb_endpoint_type(const struct drv_usb_endpoint *endpoint)
{
	return endpoint == NULL ? DRV_USB_TRANSFER_CONTROL : endpoint->type;
}

bool
drv_usb_endpoint_is_input(const struct drv_usb_endpoint *endpoint)
{
	return endpoint != NULL && (endpoint->address & DRV_USB_DIR_IN) != 0;
}

uint16_t
drv_usb_endpoint_max_packet_size(const struct drv_usb_endpoint *endpoint)
{
	return endpoint == NULL ? 0 : endpoint->max_packet;
}

unsigned
drv_usb_device_hcd_capabilities(const struct drv_usb_device *device)
{
	return device == NULL ? 0 : device->capabilities;
}

int
drv_usb_interface_claim(struct drv_usb_interface *owner,
	struct drv_usb_interface *target)
{
	if (owner == NULL || target == NULL || target->claimed_by != NULL)
		return EBUSY;
	claim_calls++;
	target->claimed_by = owner;
	return 0;
}

int
drv_usb_interface_release(struct drv_usb_interface *owner,
	struct drv_usb_interface *target)
{
	if (target == NULL || target->claimed_by != owner)
		return EPERM;
	release_calls++;
	target->claimed_by = NULL;
	return 0;
}

int
drv_usb_interface_set_alternate(struct drv_usb_interface *interface,
	unsigned setting)
{
	struct drv_usb_device *device = interface->device;
	unsigned attempt, index;

	attempt = __atomic_add_fetch(&device->alternate_attempts, 1U,
	    __ATOMIC_ACQ_REL);
	device->controls[device->control_count++] = 0x100U + setting;
	if (setting == 1U) {
		struct drv_usb_interface *control =
		    &device->configuration.interfaces[0];
		struct ncm_adapter *adapter = control->driver_data;

		device->alt1_live_urbs = device->live_urbs;
		device->alt1_registry_count = device_registry_count;
		device->alt1_has_driver_data = adapter != NULL;
		device->alt1_adapter_ready = adapter != NULL &&
		    adapter->ready != 0;
	}
	if (device->fail_alternate_at == attempt)
		return EIO;
	/* p015 permits allocated, idle URBs to survive an alternate change.  Only
	 * accepted work on the target logical interface excludes the switch. */
	if (device->data_pending_urbs != 0)
		return EBUSY;
	for (index = 0; index < interface->alternate_count; index++)
		if (interface->alternates[index].descriptor.alternate_setting ==
		    setting) {
			interface->active_index = index;
			interface->descriptor =
			    interface->alternates[index].descriptor;
			return 0;
		}
	return ENOENT;
}

void *
drv_usb_interface_driver_data(const struct drv_usb_interface *interface)
{
	return interface == NULL ? NULL : interface->driver_data;
}

int
drv_usb_interface_set_driver_data(struct drv_usb_interface *interface,
	void *data)
{
	if (interface == NULL)
		return EINVAL;
	if (data == NULL)
		driver_data_clear_calls++;
	else
		driver_data_set_calls++;
	interface->driver_data = data;
	return 0;
}

int
drv_usb_device_get_string(struct drv_usb_device *device,
	unsigned string_index, unsigned language_id, char *buffer, size_t capacity)
{
	(void)language_id;
	if (device == NULL || string_index != 3 || buffer == NULL ||
	    strlen(device->mac) + 1U > capacity)
		return EINVAL;
	strcpy(buffer, device->mac);
	return 0;
}

__attribute__((noinline)) int
drv_usb_control(struct drv_usb_device *device, uint8_t request_type,
	uint8_t request, uint16_t value, uint16_t index, void *buffer,
	size_t length, unsigned timeout, size_t *actual)
{
	(void)request_type;
	(void)value;
	(void)index;
	(void)timeout;
	device->control_attempts++;
	device->controls[device->control_count++] = request;
	if (device->fail_control_at == device->control_attempts)
		return EIO;
	if (request == NCM_GET_NTB_PARAMETERS) {
		if (length < sizeof(device->parameters) || buffer == NULL)
			return EMSGSIZE;
		memcpy(buffer, device->parameters, sizeof(device->parameters));
		*actual = sizeof(device->parameters);
	} else
		*actual = length;
	return 0;
}

struct drv_usb_urb *
drv_usb_urb_alloc(struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint, unsigned iso_count)
{
	struct drv_usb_urb *urb;

	(void)iso_count;
	urb = hal_malloc(sizeof(*urb));
	if (urb == NULL)
		return NULL;
	memset(urb, 0, sizeof(*urb));
	urb->device = device;
	urb->endpoint = endpoint;
	live_urbs++;
	device->live_urbs++;
	return urb;
}

void
drv_usb_urb_free(struct drv_usb_urb *urb)
{
	if (urb == NULL)
		return;
	CHECK(urb->status != DRV_USB_URB_PENDING && !urb->hcd_owned);
	CHECK(live_urbs != 0);
	live_urbs--;
	CHECK(urb->device->live_urbs != 0);
	urb->device->live_urbs--;
	hal_free(urb);
}

int
drv_usb_urb_setup(struct drv_usb_urb *urb, void *buffer, size_t length,
	unsigned flags, unsigned timeout, drv_usb_urb_callback_t callback,
	void *argument)
{
	(void)flags;
	(void)timeout;
	if (urb == NULL || urb->status == DRV_USB_URB_PENDING || urb->hcd_owned)
		return EBUSY;
	urb->buffer = buffer;
	urb->length = length;
	urb->actual = 0;
	urb->status = DRV_USB_URB_IDLE;
	urb->callback = callback;
	urb->callback_argument = argument;
	return 0;
}

static void
fake_complete(struct drv_usb_urb *urb, enum drv_usb_urb_status status,
	size_t actual)
{
	CHECK(urb != NULL && urb->status == DRV_USB_URB_PENDING &&
	    urb->hcd_owned);
	urb->actual = actual > urb->length ? urb->length : actual;
	urb->status = status;
	if (urb->endpoint != NULL && urb->endpoint->interface ==
	    &urb->device->configuration.interfaces[1]) {
		CHECK(urb->device->data_pending_urbs != 0);
		urb->device->data_pending_urbs--;
	}
	if (pending_urbs != 0)
		pending_urbs--;
	if (urb->callback != NULL)
		urb->callback(urb, urb->callback_argument);
	urb->hcd_owned = 0;
}

int
drv_usb_urb_submit(struct drv_usb_urb *urb)
{
	if (urb == NULL || urb->status == DRV_USB_URB_PENDING)
		return EINVAL;
	if (urb->device->submit_error_count != 0) {
		urb->device->submit_error_count--;
		return urb->device->submit_error;
	}
	if (urb->device->fail_submit_once) {
		urb->device->fail_submit_once = 0;
		return EIO;
	}
	urb->status = DRV_USB_URB_PENDING;
	urb->hcd_owned = 1;
	if (urb->endpoint != NULL && urb->endpoint->interface ==
	    &urb->device->configuration.interfaces[1])
		urb->device->data_pending_urbs++;
	pending_urbs++;
	if (urb->device->auto_complete_notification &&
	    urb->endpoint->type == DRV_USB_TRANSFER_INTERRUPT) {
		uint8_t *bytes = urb->buffer;

		memset(bytes, 0, 8);
		bytes[0] = 0xa1;
		bytes[1] = NCM_NOTIFICATION_NETWORK_CONNECTION;
		bytes[2] = 1;
		fake_complete(urb, DRV_USB_URB_COMPLETE, 8);
	}
	return 0;
}

int
drv_usb_urb_cancel(struct drv_usb_urb *urb)
{
	if (urb == NULL || urb->status != DRV_USB_URB_PENDING)
		return EINVAL;
	fake_complete(urb, DRV_USB_URB_CANCELLED, 0);
	return 0;
}

int
drv_usb_urb_drain(struct drv_usb_urb *urb, unsigned timeout_ms)
{
	(void)timeout_ms;
	if (poll_fixture_thread && urb != NULL && urb->endpoint != NULL &&
	    urb->endpoint->type == DRV_USB_TRANSFER_BULK &&
	    drv_usb_endpoint_is_input(urb->endpoint) &&
	    atomic_load_explicit(&block_poll_drain, memory_order_acquire)) {
		atomic_store_explicit(&poll_drain_entered, 1,
		    memory_order_release);
		while (atomic_load_explicit(&block_poll_drain,
		    memory_order_acquire))
			fixture_thread_yield();
	}
	if (drain_fail_once) {
		drain_fail_once = 0;
		return ETIMEDOUT;
	}
	return urb != NULL &&
	    (urb->status == DRV_USB_URB_PENDING || urb->hcd_owned) ?
	    ETIMEDOUT : 0;
}

enum drv_usb_urb_status
drv_usb_urb_status(const struct drv_usb_urb *urb)
{
	return urb == NULL ? DRV_USB_URB_IO_ERROR : urb->status;
}

size_t
drv_usb_urb_actual_length(const struct drv_usb_urb *urb)
{
	return urb == NULL ? 0 : urb->actual;
}

int
drv_usb_driver_register(struct drv_usb_driver *driver)
{
	registered_driver = driver;
	return 0;
}

struct packet_buf *
packet_buf_alloc(size_t headroom)
{
	struct packet_buf *packet;
	uint8_t *storage;

	if (headroom > PACKET_BUF_STORAGE_SIZE)
		return NULL;
	packet = calloc(1, sizeof(*packet));
	storage = malloc(PACKET_BUF_STORAGE_SIZE);
	if (packet == NULL || storage == NULL) {
		free(packet);
		free(storage);
		return NULL;
	}
	packet->storage = storage;
	packet->data = storage + headroom;
	packet->capacity = PACKET_BUF_STORAGE_SIZE;
	packet_live++;
	return packet;
}

void
packet_buf_free(struct packet_buf *packet)
{
	if (packet == NULL)
		return;
	CHECK(packet_live != 0);
	packet_live--;
	packet_frees++;
	free(packet->storage);
	free(packet);
}

void *
packet_buf_append(struct packet_buf *packet, size_t length)
{
	void *result;
	size_t offset;

	if (packet == NULL)
		return NULL;
	offset = (size_t)(packet->data - packet->storage);
	if (length > packet->capacity - offset - packet->length)
		return NULL;
	result = packet->data + packet->length;
	packet->length += length;
	return result;
}

struct net_device *
net_device_alloc(void)
{
	if (device_registry_count >= NET_DEVICE_MAX)
		return NULL;
	return calloc(1, sizeof(struct net_device));
}

int
net_device_create(struct net_device *device)
{
	unsigned index, free_index = NET_DEVICE_MAX;
	int open_error;

	if (device == NULL)
		return EINVAL;
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (device_registry[index] == NULL) {
			if (free_index == NET_DEVICE_MAX)
				free_index = index;
			continue;
		}
		if (!strcmp(device_registry[index]->name, device->name))
			return EEXIST;
	}
	if (free_index == NET_DEVICE_MAX)
		return ENOSPC;
	device->state = 2;
	device_registry[free_index] = device;
	device_registry_count++;
	published_device = device;
	/* Publication deliberately precedes the final SET_INTERFACE commit, but
	 * the object must not be openable until the driver publishes ready. */
	open_error = device->ops->open(device);
	create_open_result = open_error;
	if (open_error == 0)
		device->ops->close(device);
	return 0;
}

int
net_device_gone(struct net_device *device)
{
	if (device == NULL)
		return EINVAL;
	if (net_device_gone_fail_once) {
		net_device_gone_fail_once = 0;
		return EWOULDBLOCK;
	}
	if (device->open_count != 0) {
		device->open_count = 0;
		device->closing = 1;
		device->ops->close(device);
		device->closing = 0;
	}
	device->state = 4;
	return 0;
}

void
net_device_destroy(struct net_device *device)
{
	void (*release)(void *);
	void *driver_data;
	unsigned index;

	if (device == NULL)
		return;
	for (index = 0; index < NET_DEVICE_MAX; index++)
		if (device_registry[index] == device) {
			device_registry[index] = NULL;
			CHECK(device_registry_count != 0);
			device_registry_count--;
			break;
		}
	if (published_device == device) {
		published_device = NULL;
		for (index = 0; index < NET_DEVICE_MAX; index++)
			if (device_registry[index] != NULL)
				published_device = device_registry[index];
	}
	release = device->ops == NULL ? NULL : device->ops->release;
	driver_data = device->driver_data;
	free(device);
	if (release != NULL)
		release(driver_data);
}

int
net_device_set_carrier(struct net_device *device, int carrier)
{
	if (device == NULL || device->state != 2)
		return ENODEV;
	device->carrier = carrier != 0;
	if (device->carrier && device->open_count != 0)
		device->flags |= NET_DEVICE_RUNNING;
	else
		device->flags &= ~NET_DEVICE_RUNNING;
	return 0;
}

void
net_device_schedule_poll(struct net_device *device)
{
	if (device != NULL && device->state == 2 && device->open_count != 0 &&
	    !device->opening && !device->closing)
		device->poll_scheduled = 1;
}

int
net_device_open(struct net_device *device)
{
	int error;

	device->opening = 1;
	error = device->ops->open(device);
	device->opening = 0;
	if (error == 0) {
		device->open_count = 1;
		device->flags |= NET_DEVICE_UP;
		net_device_schedule_poll(device);
	}
	return error;
}

void
net_device_close(struct net_device *device)
{
	if (device != NULL && device->open_count != 0) {
		device->open_count = 0;
		device->closing = 1;
		device->ops->close(device);
		device->closing = 0;
	}
}

unsigned
net_device_poll(struct net_device *device, unsigned budget)
{
	if (device == NULL || !device->poll_scheduled)
		return 0;
	device->poll_scheduled = 0;
	return device->ops->poll_receive(device, budget);
}

int
net_device_transmit(struct net_device *device, struct packet_buf *packet)
{
	return device->ops->transmit(device, packet);
}

void
net_device_receive(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	received_packets++;
	received_length = packet->length;
	memcpy(received_frame, packet->data, packet->length);
	packet_buf_free(packet);
}

static void
core_binding_clear(struct drv_usb_interface *control)
{
	struct drv_usb_configuration *configuration = control->configuration;
	unsigned index;

	for (index = 0; index < 2U; index++)
		if (configuration->interfaces[index].claimed_by == control)
			configuration->interfaces[index].claimed_by = NULL;
	control->driver_data = NULL;
}

static int
core_attach_attempt(struct drv_usb_device *device)
{
	struct drv_usb_interface *control = &device->configuration.interfaces[0];
	int error = ncm_driver.attach(control, &ncm_ids[0]);

	if (error != 0 && ncm_driver.detach(control,
	    DRV_USB_DETACH_ATTACH_FAILED) == 0)
		core_binding_clear(control);
	return error;
}

static int
core_detach(struct drv_usb_interface *control, unsigned flags)
{
	int error = ncm_driver.detach(control, flags);

	if (error == 0)
		core_binding_clear(control);
	return error;
}

struct detach_thread_context {
	struct drv_usb_interface *control;
	unsigned flags;
};

static void *
poll_thread_main(void *argument)
{
	struct net_device *device = argument;

	poll_fixture_thread = 1;
	(void)net_device_poll(device, 8);
	poll_fixture_thread = 0;
	atomic_store_explicit(&poll_thread_returned, 1, memory_order_release);
	return NULL;
}

static void *
detach_thread_main(void *argument)
{
	struct detach_thread_context *context = argument;
	int error;

	atomic_store_explicit(&detach_thread_started, 1, memory_order_release);
	error = core_detach(context->control, context->flags);
	atomic_store_explicit(&detach_thread_result, error, memory_order_release);
	atomic_store_explicit(&detach_thread_returned, 1, memory_order_release);
	return NULL;
}

static void
wait_for_atomic(atomic_int *value)
{
	unsigned iteration;

	for (iteration = 0; iteration < 1000000U; iteration++) {
		if (atomic_load_explicit(value, memory_order_acquire))
			return;
		fixture_thread_yield();
	}
	CHECK(0 && "timed out waiting for fixture thread");
}

static struct ncm_adapter *
attach_function(struct drv_usb_device *device)
{
	struct drv_usb_interface *control = &device->configuration.interfaces[0];

	CHECK(ncm_driver.match(control, &ncm_ids[0]) == 100);
	CHECK(core_attach_attempt(device) == 0);
	CHECK(control->driver_data != NULL);
	return control->driver_data;
}

static void
test_binding_and_attach(void)
{
	struct drv_usb_device device;
	struct ncm_adapter *adapter;
	static const unsigned expected[] = {
		NCM_GET_NTB_PARAMETERS, DRV_USB_CDC_NCM_SET_NTB_FORMAT,
		DRV_USB_CDC_NCM_SET_NTB_INPUT_SIZE,
		DRV_USB_CDC_NCM_SET_MAX_DATAGRAM_SIZE,
		DRV_USB_CDC_NCM_SET_CRC_MODE,
		NCM_SET_ETHERNET_PACKET_FILTER, 0x101U
	};
	unsigned index, releases_before;

	function_init(&device, 0);
	CHECK(ncm_driver.match(&device.configuration.interfaces[0],
	    &ncm_ids[0]) == 0);
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(!strcmp(published_device->name, "ue0"));
	CHECK(!memcmp(published_device->hwaddr,
	    (uint8_t[]){0x00, 0x11, 0x22, 0xaa, 0xbb, 0xcc}, 6));
	CHECK(device.control_count == sizeof(expected) / sizeof(expected[0]));
	for (index = 0; index < device.control_count; index++)
		CHECK(device.controls[index] == expected[index]);
	CHECK(device.configuration.interfaces[1].claimed_by ==
	    &device.configuration.interfaces[0]);
	CHECK(adapter->profile.ntb_in_max_size == NCM_NTB_BUFFER_SIZE);
	CHECK(create_open_result == ENETDOWN && adapter->ready != 0);
	CHECK(device.alt1_live_urbs == 3 && device.alt1_registry_count == 1 &&
	    device.alt1_has_driver_data != 0 && device.alt1_adapter_ready == 0);
	CHECK(adapter->notification_urb != NULL && adapter->rx_urb != NULL &&
	    adapter->tx_urb != NULL && live_urbs == 3 && pending_urbs == 0);
	releases_before = release_calls;
	CHECK(core_detach(&device.configuration.interfaces[0], 0) == 0);
	CHECK(release_calls == releases_before);
	CHECK(device.configuration.interfaces[1].claimed_by == NULL);
	CHECK(device.configuration.interfaces[1].active_index == 0);
	CHECK(published_device == NULL && live_urbs == 0 && hal_live == 0);
}

static void
test_strict_binding(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface *control, *data;

	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	control = &device.configuration.interfaces[0];
	control->alternates[0].extra_count = 3;
	CHECK(ncm_driver.match(control, &ncm_ids[0]) == 0);
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	device.configuration.iad.first_interface = 7;
	CHECK(ncm_driver.match(&device.configuration.interfaces[0],
	    &ncm_ids[0]) == 0);
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	control = &device.configuration.interfaces[0];
	control->alternates[0].extras[0] = truncated_cs_descriptor;
	control->alternates[0].extra_lengths[0] =
	    sizeof(truncated_cs_descriptor);
	CHECK(ncm_driver.match(control, &ncm_ids[0]) == 0);
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	device.ncm_extra[4] = 2;
	CHECK(ncm_driver.match(&device.configuration.interfaces[0],
	    &ncm_ids[0]) == 0);
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	device.ncm_extra[5] |= 0x40U;
	CHECK(ncm_driver.match(&device.configuration.interfaces[0],
	    &ncm_ids[0]) == 0);
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	strcpy(device.mac, "001122AABBCZ");
	CHECK(core_attach_attempt(&device) == EINVAL);
	CHECK(device.configuration.interfaces[1].claimed_by == NULL);
	CHECK(hal_live == 0);
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	control = &device.configuration.interfaces[0];
	data = &device.configuration.interfaces[1];
	data->claimed_by = data;
	CHECK(core_attach_attempt(&device) == EBUSY);
	CHECK(data->claimed_by == data && control->driver_data == NULL);
	data->claimed_by = NULL;
	CHECK(published_device == NULL && live_urbs == 0 && hal_live == 0);
}

static void
test_optional_capabilities(void)
{
	struct drv_usb_device device;
	static const unsigned expected[] = {
		NCM_GET_NTB_PARAMETERS, DRV_USB_CDC_NCM_SET_NTB_FORMAT,
		DRV_USB_CDC_NCM_SET_NTB_INPUT_SIZE, 0x101U
	};
	unsigned index;

	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	device.ncm_extra[5] = 0;
	(void)attach_function(&device);
	CHECK(device.control_count == sizeof(expected) / sizeof(expected[0]));
	for (index = 0; index < device.control_count; index++)
		CHECK(device.controls[index] == expected[index]);
	CHECK(core_detach(&device.configuration.interfaces[0], 0) == 0);
	CHECK(hal_live == 0);
}

static void
test_io_and_lifetime(void)
{
	struct drv_usb_device device;
	struct ncm_adapter *adapter;
	struct packet_buf *packet;
	struct drv_usb_endpoint storage_endpoint;
	struct drv_usb_urb *storage_urb;
	uint8_t frame[64], storage_buffer[64], ntb[NCM_NTB_BUFFER_SIZE];
	size_t ntb_length;
	uint64_t rx_errors;
	unsigned before_frees, iteration;

	memset(frame, 0x5a, sizeof(frame));
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	CHECK(pending_urbs == 2);
	memset(adapter->notification_buffer, 0, 8);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_NETWORK_CONNECTION;
	adapter->notification_buffer[2] = 1;
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 8);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(published_device->carrier != 0);
	CHECK(adapter->notification_urb->status == DRV_USB_URB_PENDING);
	CHECK(drv_usb_cdc_ncm_build_ntb16(&adapter->profile, 0, frame,
	    sizeof(frame), ntb, sizeof(ntb), &ntb_length) == 0);
	memcpy(adapter->rx_buffer, ntb, ntb_length);
	fake_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE, ntb_length);
	CHECK(net_device_poll(published_device, 1) == 1);
	CHECK(received_packets == 1 && received_length == sizeof(frame));
	CHECK(!memcmp(received_frame, frame, sizeof(frame)));
	/* Alternate 1 stays selected across close/open, so the receive sequence
	 * must continue rather than resetting with the network administrative
	 * state. */
	net_device_close(published_device);
	CHECK(net_device_open(published_device) == 0);
	CHECK(drv_usb_cdc_ncm_build_ntb16(&adapter->profile, 1, frame,
	    sizeof(frame), ntb, sizeof(ntb), &ntb_length) == 0);
	memcpy(adapter->rx_buffer, ntb, ntb_length);
	fake_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE, ntb_length);
	CHECK(net_device_poll(published_device, 1) == 1);
	CHECK(received_packets == 2);
	ntb_length = make_two_datagram_ntb(ntb, 2, frame);
	memcpy(adapter->rx_buffer, ntb, ntb_length);
	fake_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE, ntb_length);
	CHECK(net_device_poll(published_device, 1) == 1);
	CHECK(received_packets == 3 && published_device->poll_scheduled != 0 &&
	    adapter->rx_queue_count == 1);
	CHECK(net_device_poll(published_device, 1) == 1);
	CHECK(received_packets == 4 && adapter->rx_queue_count == 0);
	rx_errors = published_device->rx_errors;
	CHECK(drv_usb_cdc_ncm_build_ntb16(&adapter->profile, 3, frame,
	    sizeof(frame), ntb, sizeof(ntb), &ntb_length) == 0);
	ntb[0] ^= 0xffU;
	memcpy(adapter->rx_buffer, ntb, ntb_length);
	fake_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE, ntb_length);
	CHECK(net_device_poll(published_device, 1) == 0);
	CHECK(published_device->rx_errors == rx_errors + 1U &&
	    received_packets == 4);
	memset(adapter->notification_buffer, 0, 8);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_NETWORK_CONNECTION;
	adapter->notification_buffer[2] = 1;
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 8);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(published_device->carrier != 0);
	memset(adapter->notification_buffer, 0, 16);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_SPEED_CHANGE;
	adapter->notification_buffer[6] = 8;
	put_le32(adapter->notification_buffer + 8U, 100000000U);
	put_le32(adapter->notification_buffer + 12U, 20000000U);
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 16);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(adapter->downstream_bps == 100000000U &&
	    adapter->upstream_bps == 20000000U);
	memset(adapter->notification_buffer, 0, 8);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_NETWORK_CONNECTION;
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 8);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(published_device->carrier == 0);
	memset(adapter->notification_buffer, 0, 16);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_SPEED_CHANGE;
	adapter->notification_buffer[2] = 1;
	adapter->notification_buffer[6] = 8;
	put_le32(adapter->notification_buffer + 8U, 1U);
	put_le32(adapter->notification_buffer + 12U, 2U);
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 16);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(adapter->downstream_bps == 100000000U &&
	    adapter->upstream_bps == 20000000U);
	packet = packet_buf_alloc(0);
	CHECK(packet != NULL && packet_buf_append(packet, sizeof(frame)) != NULL);
	memcpy(packet->data, frame, sizeof(frame));
	before_frees = packet_frees;
	CHECK(net_device_transmit(published_device, packet) == 0);
	CHECK(packet_frees == before_frees + 1U && adapter->tx_busy);
	packet = packet_buf_alloc(0);
	CHECK(packet != NULL && packet_buf_append(packet, sizeof(frame)) != NULL);
	CHECK(net_device_transmit(published_device, packet) == ENOBUFS);
	CHECK(packet_frees == before_frees + 2U);
	memset(&storage_endpoint, 0, sizeof(storage_endpoint));
	storage_endpoint.interface = &device.configuration.interfaces[0];
	storage_endpoint.type = DRV_USB_TRANSFER_BULK;
	storage_endpoint.address = 0x87;
	storage_endpoint.max_packet = 512;
	storage_urb = drv_usb_urb_alloc(&device, &storage_endpoint, 0);
	CHECK(storage_urb != NULL);
	CHECK(drv_usb_urb_setup(storage_urb, storage_buffer,
	    sizeof(storage_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(storage_urb) == 0);
	CHECK(pending_urbs == 4);
	fake_complete(storage_urb, DRV_USB_URB_COMPLETE, sizeof(storage_buffer));
	drv_usb_urb_free(storage_urb);
	fake_complete(adapter->tx_urb, DRV_USB_URB_COMPLETE,
	    adapter->tx_urb->length);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(!adapter->tx_busy);
	CHECK(adapter->tx_sequence == 1);
	packet = packet_buf_alloc(0);
	CHECK(packet != NULL && packet_buf_append(packet,
	    DRV_USB_CDC_NCM_MAX_DATAGRAM_SIZE + 1U) != NULL);
	CHECK(net_device_transmit(published_device, packet) == EINVAL);
	CHECK(adapter->tx_sequence == 1);
	packet = packet_buf_alloc(0);
	CHECK(packet != NULL && packet_buf_append(packet, sizeof(frame)) != NULL);
	device.fail_submit_once = 1;
	CHECK(net_device_transmit(published_device, packet) == EIO);
	CHECK(adapter->tx_sequence == 1);
	packet = packet_buf_alloc(0);
	CHECK(packet != NULL && packet_buf_append(packet, sizeof(frame)) != NULL);
	CHECK(net_device_transmit(published_device, packet) == 0);
	CHECK(ncm_le16(adapter->tx_buffer + 6U) == 1U);
	fake_complete(adapter->tx_urb, DRV_USB_URB_COMPLETE,
	    adapter->tx_urb->length);
	CHECK(net_device_poll(published_device, 8) == 0);
	net_device_close(published_device);
	CHECK(pending_urbs == 0 && packet_live == 0);
	for (iteration = 0; iteration < 12U; iteration++) {
		CHECK(net_device_open(published_device) == 0);
		CHECK(pending_urbs == 2);
		net_device_close(published_device);
		CHECK(pending_urbs == 0);
	}
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(device.alternate_attempts == 1);
	CHECK(published_device == NULL && live_urbs == 0 && hal_live == 0);
}

static void
test_immediate_completion(void)
{
	struct drv_usb_device device;

	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	device.auto_complete_notification = 1;
	(void)attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	CHECK(published_device->poll_scheduled != 0);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(published_device->carrier != 0);
	net_device_close(published_device);
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(hal_live == 0);
}

static void
test_rearm_retry_and_quarantine(void)
{
	struct drv_usb_device device;
	struct ncm_adapter *adapter;

	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	memset(adapter->notification_buffer, 0, 8);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_NETWORK_CONNECTION;
	adapter->notification_buffer[2] = 1;
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 8);
	device.submit_error = ENOMEM;
	device.submit_error_count = 1;
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(adapter->notification_retries == 1 &&
	    adapter->notification_rearm != 0 &&
	    adapter->quarantined == 0 && published_device->carrier != 0);
	CHECK(published_device->poll_scheduled != 0 && pending_urbs == 1);
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(adapter->notification_retries == 0 &&
	    adapter->notification_rearm == 0 &&
	    adapter->quarantined == 0 && published_device->carrier != 0);
	CHECK(adapter->notification_urb->status == DRV_USB_URB_PENDING &&
	    pending_urbs == 2);
	net_device_close(published_device);
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(hal_live == 0 && live_urbs == 0 && pending_urbs == 0);

	/* The retry budget is finite.  Persistent scheduler/HCD backpressure is
	 * quarantined on the first attempt beyond the configured budget. */
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	memset(adapter->notification_buffer, 0, 8);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_NETWORK_CONNECTION;
	adapter->notification_buffer[2] = 1;
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 8);
	device.submit_error = EAGAIN;
	device.submit_error_count = NCM_REARM_RETRY_MAX + 1U;
	for (unsigned attempt = 0; attempt < NCM_REARM_RETRY_MAX; attempt++) {
		CHECK(net_device_poll(published_device, 8) == 0);
		CHECK(adapter->quarantined == 0 &&
		    published_device->poll_scheduled != 0);
	}
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(adapter->quarantined != 0 && adapter->opened == 0 &&
	    published_device->carrier == 0 &&
	    published_device->poll_scheduled == 0);
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(hal_live == 0 && live_urbs == 0 && pending_urbs == 0);

	/* A non-transient start failure cannot be retried safely forever.  The
	 * interface is quarantined and carrier is withdrawn, while detach still
	 * owns and can retire the complete resource graph. */
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	memset(adapter->notification_buffer, 0, 8);
	adapter->notification_buffer[0] = 0xa1;
	adapter->notification_buffer[1] = NCM_NOTIFICATION_NETWORK_CONNECTION;
	adapter->notification_buffer[2] = 1;
	fake_complete(adapter->notification_urb, DRV_USB_URB_COMPLETE, 8);
	device.submit_error = EIO;
	device.submit_error_count = 1;
	CHECK(net_device_poll(published_device, 8) == 0);
	CHECK(adapter->quarantined != 0 && adapter->opened == 0 &&
	    published_device->carrier == 0);
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(hal_live == 0 && live_urbs == 0 && pending_urbs == 0);
}

static void
test_shutdown(void)
{
	struct drv_usb_device device;
	struct ncm_adapter *adapter;
	struct packet_buf *packet;
	uint8_t frame[64];

	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	CHECK(pending_urbs == 2 && live_urbs == 3);
	(void)net_device_set_carrier(published_device, 1);
	memset(frame, 0x3c, sizeof(frame));
	packet = packet_buf_alloc(0);
	CHECK(packet != NULL && packet_buf_append(packet, sizeof(frame)) != NULL);
	memcpy(packet->data, frame, sizeof(frame));
	CHECK(net_device_transmit(published_device, packet) == 0);
	CHECK(pending_urbs == 3 && adapter->tx_busy != 0);
	ncm_driver.shutdown(&device.configuration.interfaces[0]);
	CHECK(pending_urbs == 0 && live_urbs == 3);
	CHECK(adapter->opened == 0 && adapter->closing == 0 &&
	    adapter->stopping == 0 && adapter->quarantined == 0 &&
	    adapter->stop_error == 0 && adapter->tx_busy == 0);
	CHECK(adapter->notification_ready == 0 && adapter->rx_ready == 0 &&
	    adapter->tx_ready == 0 && adapter->notification_rearm == 0 &&
	    adapter->rx_rearm == 0 && adapter->notification_retries == 0 &&
	    adapter->rx_retries == 0);
	CHECK(adapter->notification_urb->status == DRV_USB_URB_CANCELLED &&
	    !adapter->notification_urb->hcd_owned &&
	    adapter->rx_urb->status == DRV_USB_URB_CANCELLED &&
	    !adapter->rx_urb->hcd_owned &&
	    adapter->tx_urb->status == DRV_USB_URB_CANCELLED &&
	    !adapter->tx_urb->hcd_owned);
	CHECK(published_device->carrier == 0 &&
	    (published_device->flags & NET_DEVICE_RUNNING) == 0 &&
	    device.configuration.interfaces[1].claimed_by ==
	    &device.configuration.interfaces[0] &&
	    device.configuration.interfaces[0].driver_data == adapter);
	CHECK(device.configuration.interfaces[1].active_index == 1 &&
	    published_device != NULL);
	ncm_driver.shutdown(&device.configuration.interfaces[0]);
	CHECK(pending_urbs == 0 && live_urbs == 3 &&
	    adapter->stop_error == 0 && adapter->quarantined == 0);
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(hal_live == 0 && live_urbs == 0 && pending_urbs == 0);
}

static void
test_failure_unwind(void)
{
	unsigned failure;

	for (failure = 1; failure <= 6U; failure++) {
		struct drv_usb_device device;
		int error;

		function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
		device.fail_control_at = failure;
		error = core_attach_attempt(&device);
		CHECK(error != 0);
		CHECK(device.configuration.interfaces[1].claimed_by == NULL);
		CHECK(published_device == NULL && live_urbs == 0 && hal_live == 0);
	}
	for (failure = 1; failure <= 7U; failure++) {
		struct drv_usb_device device;
		int error;

		function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
		hal_attempts = 0;
		hal_fail_at = failure;
		error = core_attach_attempt(&device);
		hal_fail_at = 0;
		CHECK(error == ENOMEM);
		CHECK(device.configuration.interfaces[1].claimed_by == NULL);
		CHECK(published_device == NULL && live_urbs == 0 && hal_live == 0);
	}
}

static void
test_drain_quarantine(void)
{
	struct drv_usb_device device;
	struct ncm_adapter *adapter;

	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	drain_fail_once = 1;
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == ETIMEDOUT);
	CHECK(adapter->quarantined != 0);
	CHECK(device.configuration.interfaces[1].claimed_by != NULL);
	/* The first cancel already retired every fake request.  A bus-level
	 * teardown retry may now finish the retained ownership graph. */
	adapter->quarantined = 0;
	CHECK(core_detach(&device.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(published_device == NULL && live_urbs == 0 && hal_live == 0);
}

static void
test_registry_ninth_bind(void)
{
	struct drv_usb_device devices[NET_DEVICE_MAX + 1U];
	struct ncm_adapter *adapters[NET_DEVICE_MAX];
	size_t live_before_ninth;
	unsigned index;

	for (index = 0; index < NET_DEVICE_MAX; index++) {
		function_init(&devices[index],
		    DRV_USB_HCD_CAP_CONCURRENT_URBS);
		adapters[index] = attach_function(&devices[index]);
		CHECK(adapters[index]->net_device->name[0] == 'u' &&
		    adapters[index]->net_device->name[1] == 'e' &&
		    adapters[index]->net_device->name[2] == (char)('0' + index));
	}
	CHECK(device_registry_count == NET_DEVICE_MAX);
	live_before_ninth = hal_live;
	function_init(&devices[NET_DEVICE_MAX],
	    DRV_USB_HCD_CAP_CONCURRENT_URBS);
	CHECK(core_attach_attempt(&devices[NET_DEVICE_MAX]) == ENOSPC);
	CHECK(hal_live == live_before_ninth &&
	    devices[NET_DEVICE_MAX].configuration.interfaces[1].claimed_by ==
	    NULL);
	for (index = 0; index < NET_DEVICE_MAX; index++)
		CHECK(core_detach(
		    &devices[index].configuration.interfaces[0],
		    DRV_USB_DETACH_FORCE) == 0);
	CHECK(device_registry_count == 0 && published_device == NULL &&
	    hal_live == 0 && live_urbs == 0);
}

static void
test_twelve_reconnects(void)
{
	unsigned iteration;

	for (iteration = 0; iteration < 12U; iteration++) {
		struct drv_usb_device device;
		struct ncm_adapter *adapter;

		function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
		adapter = attach_function(&device);
		CHECK(!strcmp(adapter->net_device->name, "ue0"));
		CHECK(net_device_open(adapter->net_device) == 0);
		net_device_close(adapter->net_device);
		CHECK(core_detach(&device.configuration.interfaces[0],
		    DRV_USB_DETACH_FORCE) == 0);
		CHECK(device.configuration.interfaces[1].claimed_by == NULL &&
		    device_registry_count == 0 && hal_live == 0 &&
		    live_urbs == 0 && pending_urbs == 0);
	}
}

static void
test_independent_storage_during_detach(void)
{
	struct drv_usb_device device;
	struct drv_usb_interface storage_interface;
	struct drv_usb_endpoint storage_endpoint;
	struct drv_usb_urb *storage_urb;
	uint8_t storage_buffer[512];

	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	(void)attach_function(&device);
	CHECK(net_device_open(published_device) == 0 && pending_urbs == 2);
	memset(&storage_interface, 0, sizeof(storage_interface));
	storage_interface.device = &device;
	memset(&storage_endpoint, 0, sizeof(storage_endpoint));
	storage_endpoint.interface = &storage_interface;
	storage_endpoint.type = DRV_USB_TRANSFER_BULK;
	storage_endpoint.address = 0x87;
	storage_endpoint.max_packet = 512;
	storage_urb = drv_usb_urb_alloc(&device, &storage_endpoint, 0);
	CHECK(storage_urb != NULL);
	CHECK(drv_usb_urb_setup(storage_urb, storage_buffer,
	    sizeof(storage_buffer), 0, 0, NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(storage_urb) == 0 && pending_urbs == 3);

	/* Normal class unbind retires only the NCM function.  An accepted request
	 * belonging to an independent function on the same USB device remains HCD
	 * owned and can complete normally afterwards. */
	CHECK(core_detach(&device.configuration.interfaces[0], 0) == 0);
	CHECK(storage_urb->status == DRV_USB_URB_PENDING &&
	    storage_urb->hcd_owned != 0 && pending_urbs == 1 && live_urbs == 1);
	CHECK(device.configuration.interfaces[1].active_index == 0 &&
	    device.configuration.interfaces[1].claimed_by == NULL &&
	    published_device == NULL && device_registry_count == 0);
	fake_complete(storage_urb, DRV_USB_URB_COMPLETE,
	    sizeof(storage_buffer));
	CHECK(pending_urbs == 0);
	drv_usb_urb_free(storage_urb);
	CHECK(live_urbs == 0 && hal_live == 0);
}

static void
test_poll_detach_join(void)
{
	struct drv_usb_device device;
	struct ncm_adapter *adapter;
	struct detach_thread_context detach_context;
	pthread_t poll_thread, detach_thread;
	uint8_t frame[64], ntb[NCM_NTB_BUFFER_SIZE];
	size_t ntb_length;

	memset(frame, 0x69, sizeof(frame));
	function_init(&device, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&device);
	CHECK(net_device_open(published_device) == 0);
	CHECK(drv_usb_cdc_ncm_build_ntb16(&adapter->profile, 0, frame,
	    sizeof(frame), ntb, sizeof(ntb), &ntb_length) == 0);
	memcpy(adapter->rx_buffer, ntb, ntb_length);
	fake_complete(adapter->rx_urb, DRV_USB_URB_COMPLETE, ntb_length);
	CHECK(published_device->poll_scheduled != 0);

	atomic_store_explicit(&block_poll_drain, 1, memory_order_release);
	atomic_store_explicit(&poll_drain_entered, 0, memory_order_release);
	atomic_store_explicit(&poll_thread_returned, 0, memory_order_release);
	atomic_store_explicit(&detach_thread_started, 0, memory_order_release);
	atomic_store_explicit(&detach_thread_returned, 0, memory_order_release);
	atomic_store_explicit(&detach_thread_result, 0x7fff,
	    memory_order_release);
	CHECK(pthread_create(&poll_thread, NULL, poll_thread_main,
	    published_device) == 0);
	wait_for_atomic(&poll_drain_entered);
	CHECK(adapter->polls_active == 1);

	/* Keep the graph after the joined alt0 commit so the test can inspect that
	 * an ENETDOWN rearm during close did not quarantine the adapter. */
	net_device_gone_fail_once = 1;
	detach_context.control = &device.configuration.interfaces[0];
	detach_context.flags = 0;
	CHECK(pthread_create(&detach_thread, NULL, detach_thread_main,
	    &detach_context) == 0);
	wait_for_atomic(&detach_thread_started);
	for (unsigned iteration = 0; iteration < 100U; iteration++)
		fixture_thread_yield();
	CHECK(atomic_load_explicit(&detach_thread_returned,
	    memory_order_acquire) == 0);
	CHECK(__atomic_load_n(&device.alternate_attempts, __ATOMIC_ACQUIRE) == 1);
	CHECK(device.configuration.interfaces[1].active_index == 1);

	atomic_store_explicit(&block_poll_drain, 0, memory_order_release);
	CHECK(pthread_join(poll_thread, NULL) == 0);
	CHECK(pthread_join(detach_thread, NULL) == 0);
	CHECK(atomic_load_explicit(&poll_thread_returned,
	    memory_order_acquire) != 0);
	CHECK(atomic_load_explicit(&detach_thread_result,
	    memory_order_acquire) == EWOULDBLOCK);
	CHECK(adapter->polls_active == 0 && adapter->quarantined == 0 &&
	    adapter->ready == 0 && pending_urbs == 0);
	CHECK(device.configuration.interfaces[1].active_index == 0 &&
	    device.configuration.interfaces[1].claimed_by ==
	    &device.configuration.interfaces[0]);
	CHECK(adapter->notification_urb != NULL && adapter->rx_urb != NULL &&
	    adapter->tx_urb != NULL && published_device == adapter->net_device);
	CHECK(core_detach(&device.configuration.interfaces[0], 0) == 0);
	CHECK(hal_live == 0 && live_urbs == 0 && pending_urbs == 0 &&
	    published_device == NULL && device_registry_count == 0);
}

static void
test_attach_commit_and_detach_retry(void)
{
	struct drv_usb_device attach_failure, attach_pending, detach_failure;
	struct drv_usb_device gone_failure;
	struct ncm_adapter *adapter;
	struct drv_usb_urb *notification, *rx, *tx;
	uint8_t *notification_buffer, *rx_buffer, *tx_buffer;
	struct net_device *netdev;
	size_t live_before;
	unsigned releases_before, clears_before;

	/* All software ownership exists, but remains not-ready, before alt1 is
	 * attempted as the final fallible attach commit.  A failed commit needs no
	 * compensating SET_INTERFACE and core attach-abort removes the graph. */
	function_init(&attach_failure, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	attach_failure.fail_alternate_at = 1;
	create_open_result = 0x7fff;
	releases_before = release_calls;
	clears_before = driver_data_clear_calls;
	CHECK(core_attach_attempt(&attach_failure) == EIO);
	CHECK(create_open_result == ENETDOWN);
	CHECK(attach_failure.alt1_live_urbs == 3 &&
	    attach_failure.alt1_registry_count == 1 &&
	    attach_failure.alt1_has_driver_data != 0 &&
	    attach_failure.alt1_adapter_ready == 0);
	CHECK(attach_failure.configuration.interfaces[1].active_index == 0 &&
	    attach_failure.configuration.interfaces[1].claimed_by == NULL &&
	    attach_failure.configuration.interfaces[0].driver_data == NULL);
	CHECK(attach_failure.alternate_attempts == 1 &&
	    attach_failure.controls[attach_failure.control_count - 2U] ==
	    NCM_SET_ETHERNET_PACKET_FILTER &&
	    attach_failure.controls[attach_failure.control_count - 1U] == 0x101U);
	CHECK(release_calls == releases_before &&
	    driver_data_clear_calls == clears_before);
	CHECK(published_device == NULL && device_registry_count == 0 &&
	    live_urbs == 0 && hal_live == 0);

	/* If attach-abort cleanup itself cannot withdraw the unpublished-ready
	 * network identity, USB core retains one closed provisional binding.  A
	 * later forced detach must release that exact graph once, without replaying
	 * alt1 or allocating replacement objects. */
	function_init(&attach_pending, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	attach_pending.fail_alternate_at = 1;
	net_device_gone_fail_once = 1;
	releases_before = release_calls;
	clears_before = driver_data_clear_calls;
	CHECK(core_attach_attempt(&attach_pending) == EIO);
	adapter = attach_pending.configuration.interfaces[0].driver_data;
	CHECK(adapter != NULL && adapter->ready == 0);
	notification = adapter->notification_urb;
	rx = adapter->rx_urb;
	tx = adapter->tx_urb;
	notification_buffer = adapter->notification_buffer;
	rx_buffer = adapter->rx_buffer;
	tx_buffer = adapter->tx_buffer;
	netdev = adapter->net_device;
	live_before = hal_live;
	CHECK(attach_pending.configuration.interfaces[1].active_index == 0 &&
	    attach_pending.configuration.interfaces[1].claimed_by ==
	    &attach_pending.configuration.interfaces[0]);
	CHECK(adapter->notification_urb == notification &&
	    adapter->rx_urb == rx && adapter->tx_urb == tx &&
	    adapter->notification_buffer == notification_buffer &&
	    adapter->rx_buffer == rx_buffer && adapter->tx_buffer == tx_buffer &&
	    adapter->net_device == netdev);
	CHECK(published_device == netdev && device_registry_count == 1 &&
	    live_urbs == 3 && hal_live == live_before && pending_urbs == 0 &&
	    attach_pending.alternate_attempts == 1);
	CHECK(release_calls == releases_before &&
	    driver_data_clear_calls == clears_before);
	CHECK(core_detach(&attach_pending.configuration.interfaces[0],
	    DRV_USB_DETACH_FORCE) == 0);
	CHECK(attach_pending.configuration.interfaces[1].claimed_by == NULL &&
	    attach_pending.configuration.interfaces[0].driver_data == NULL &&
	    attach_pending.alternate_attempts == 1);
	CHECK(release_calls == releases_before &&
	    driver_data_clear_calls == clears_before);
	CHECK(published_device == NULL && device_registry_count == 0 &&
	    live_urbs == 0 && hal_live == 0 && pending_urbs == 0);

	/* A failed normal alt0 commit retains the exact graph and its identity for
	 * retry.  It is withdrawn from new opens by ready=0, but is neither gone
	 * nor rebuilt. */
	function_init(&detach_failure, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&detach_failure);
	notification = adapter->notification_urb;
	rx = adapter->rx_urb;
	tx = adapter->tx_urb;
	notification_buffer = adapter->notification_buffer;
	rx_buffer = adapter->rx_buffer;
	tx_buffer = adapter->tx_buffer;
	netdev = adapter->net_device;
	live_before = hal_live;
	releases_before = release_calls;
	clears_before = driver_data_clear_calls;
	detach_failure.fail_alternate_at = 2;
	CHECK(core_detach(&detach_failure.configuration.interfaces[0], 0) == EIO);
	CHECK(adapter->ready == 0 && adapter->notification_urb == notification &&
	    adapter->rx_urb == rx && adapter->tx_urb == tx);
	CHECK(adapter->notification_buffer == notification_buffer &&
	    adapter->rx_buffer == rx_buffer && adapter->tx_buffer == tx_buffer &&
	    adapter->net_device == netdev);
	CHECK(detach_failure.configuration.interfaces[1].active_index == 1 &&
	    detach_failure.configuration.interfaces[1].claimed_by ==
	    &detach_failure.configuration.interfaces[0] &&
	    detach_failure.configuration.interfaces[0].driver_data == adapter);
	CHECK(published_device == netdev && device_registry_count == 1 &&
	    live_urbs == 3 && hal_live == live_before && pending_urbs == 0);
	CHECK(net_device_open(netdev) == ENETDOWN);
	CHECK(release_calls == releases_before &&
	    driver_data_clear_calls == clears_before);
	detach_failure.fail_alternate_at = 0;
	CHECK(core_detach(&detach_failure.configuration.interfaces[0], 0) == 0);
	CHECK(detach_failure.configuration.interfaces[1].active_index == 0 &&
	    detach_failure.configuration.interfaces[1].claimed_by == NULL &&
	    detach_failure.configuration.interfaces[0].driver_data == NULL);
	CHECK(release_calls == releases_before &&
	    driver_data_clear_calls == clears_before);
	CHECK(hal_live == 0 && live_urbs == 0 && device_registry_count == 0 &&
	    published_device == NULL);

	/* The registry can still refuse withdrawal after alt0 committed.  That
	 * error also retains the complete graph; retrying must not reconstruct or
	 * double-release any object. */
	function_init(&gone_failure, DRV_USB_HCD_CAP_CONCURRENT_URBS);
	adapter = attach_function(&gone_failure);
	notification = adapter->notification_urb;
	rx = adapter->rx_urb;
	tx = adapter->tx_urb;
	notification_buffer = adapter->notification_buffer;
	rx_buffer = adapter->rx_buffer;
	tx_buffer = adapter->tx_buffer;
	netdev = adapter->net_device;
	live_before = hal_live;
	net_device_gone_fail_once = 1;
	CHECK(core_detach(&gone_failure.configuration.interfaces[0], 0) ==
	    EWOULDBLOCK);
	CHECK(gone_failure.configuration.interfaces[1].active_index == 0 &&
	    gone_failure.configuration.interfaces[1].claimed_by ==
	    &gone_failure.configuration.interfaces[0] &&
	    gone_failure.configuration.interfaces[0].driver_data == adapter);
	CHECK(adapter->notification_urb == notification &&
	    adapter->rx_urb == rx && adapter->tx_urb == tx &&
	    adapter->notification_buffer == notification_buffer &&
	    adapter->rx_buffer == rx_buffer && adapter->tx_buffer == tx_buffer &&
	    adapter->net_device == netdev && adapter->ready == 0);
	CHECK(published_device == netdev && device_registry_count == 1 &&
	    live_urbs == 3 && hal_live == live_before);
	CHECK(core_detach(&gone_failure.configuration.interfaces[0], 0) == 0);
	CHECK(gone_failure.configuration.interfaces[1].claimed_by == NULL &&
	    gone_failure.configuration.interfaces[0].driver_data == NULL &&
	    published_device == NULL && device_registry_count == 0 &&
	    live_urbs == 0 && hal_live == 0);
}

int
main(void)
{
	CHECK(drv_usb_cdc_ncm_driver_register() == 0);
	CHECK(registered_driver == &ncm_driver);
	test_binding_and_attach();
	test_strict_binding();
	test_optional_capabilities();
	test_io_and_lifetime();
	test_immediate_completion();
	test_rearm_retry_and_quarantine();
	test_shutdown();
	test_failure_unwind();
	test_drain_quarantine();
	test_registry_ninth_bind();
	test_twelve_reconnects();
	test_independent_storage_during_detach();
	test_poll_detach_join();
	test_attach_commit_and_detach_retry();
	CHECK(packet_live == 0 && pending_urbs == 0 && live_urbs == 0 &&
	    hal_live == 0 && published_device == NULL &&
	    device_registry_count == 0);
	printf("usb-cdc-ncm driver fixture: PASS (%u checks)\n", checks);
	return 0;
}
